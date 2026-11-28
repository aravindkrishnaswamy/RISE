//////////////////////////////////////////////////////////////////////
//
//  LightSampler.h - Unified light source sampling with explicit PDFs
//  for PT, BDPT, VCM, and MLT.  Call Prepare() once after attaching
//  the scene to build the Vose alias table (O(1) draw/PDF) and
//  optional LightBVH.
//
//  Services:
//  - SampleLight(): exitance-proportional emission sampling (delta
//    point/spot: pdfPos=1; mesh luminaries: uniform area + cosine
//    hemisphere; env map: bounding-sphere disk emission).
//  - EvaluateDirectLighting{,NM}(): NEE with shadow visibility, medium
//    transmittance, and MIS.  Zero-exitance lights (ambient/directional)
//    are evaluated deterministically.  Area lights use power-heuristic
//    (or optimal) MIS against BSDF sampling when RIS is off; with RIS
//    (SetRISCandidates(M>0)) MIS is disabled (w_nee=1) and BSDF-hit
//    emission is suppressed for selectable luminaries.
//  - Self-exclusion: RIS zeroes self weight; alias/BVH discards a self
//    draw without retry so other lights' PMFs stay unchanged; return
//    hits onto non-convex emitters keep full weight (pNEESelfObject).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef LIGHT_SAMPLER_
#define LIGHT_SAMPLER_

#include <unordered_map>
#include "../Interfaces/IScene.h"
#include "../Interfaces/ILight.h"
#include "../Interfaces/IObject.h"
#include "../Interfaces/IBSDF.h"
#include "../Intersection/RayIntersectionGeometric.h"	// SurfaceDerivativesInfo / SurfaceSignalInfo / TextureFootprint, by value on EmitterSurfacePayload
#include "../Interfaces/IMedium.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/Reference.h"
#include "../Utilities/ISampler.h"
#include "../Utilities/AliasTable.h"
#include "../Utilities/MISWeights.h"
#include "../Rendering/LuminaryManager.h"
#include "../Rendering/EnvironmentSampler.h"
#include "LightBVH.h"
#include "PhysicalLightSpectrum.h"

namespace RISE
{
	class IRayCaster;
	class IMaterial;
	class ILightPriv;
	class IORStack;
	class IContinuationClosureNM;
	class IPhaseFunction;
	struct ContinuationAvailability;
	struct MediumContinuationAvailability;

	namespace Implementation { class OptimalMISAccumulator; }

	class IORStack;

	/// Optional per-call hook replacing the BSDF-sampling MIS-partner density
	/// in EvaluateDirectLighting{,NM} (used by guided PT so NEE sees the same
	/// mixture density as the guided continuation).  Null = use raw material pdf.
	class IGuidedNEEPdfBlend
	{
	public:
		virtual ~IGuidedNEEPdfBlend() {}

		/// \param rawPdf  Material aggregate pdf for `wo`; MAY BE ZERO (do NOT
		///                skip calling Blend when rawPdf == 0, as a guide term
		///                can still cover `wo`; an unguided impl returns rawPdf).
		virtual Scalar Blend(
			const Vector3& wo,
			Scalar rawPdf
			) const = 0;
	};

	namespace Implementation
	{
		/// Boundary-delimited shadow-segment medium transmittance (shared by
		/// ordinary NEE and volume-emission NEE).  Returns false with outTr = 0
		/// and logs an error if boundary topology is ambiguous.
		bool EvaluateShadowMediumTransmittance(
			const Ray& ray,
			const Scalar maxDist,
			const IMedium* pOriginMedium,
			const IObject* pOriginMediumObject,
			const IScene* pScene,
			const bool bSceneHasObjectMedia,
			RISEPel& outTr,
			const IORStack* pOriginStack = 0
			);

		bool EvaluateShadowMediumTransmittanceNM(
			const Ray& ray,
			const Scalar maxDist,
			const IMedium* pOriginMedium,
			const IObject* pOriginMediumObject,
			const IScene* pScene,
			const bool bSceneHasObjectMedia,
			const Scalar nm,
			Scalar& outTr,
			const IORStack* pOriginStack = 0
			);

		//! Scale-relative emitter-probe constants (fractions of luminary world
		//! AABB diagonal D; see ProbeEmitterSurface):
		//!   standoff = max(kEmitterProbeStandoffFraction * D,
		//!                  kEmitterProbeStandoffMargin * floorWorld
		//!                    + kEmitterProbeStandoffCushionFraction * D)
		static const Scalar kEmitterProbeAcceptFraction = Scalar( 0.01 );
		static const Scalar kEmitterProbeStandoffFraction = Scalar( 0.001 );
		static const Scalar kEmitterProbeStandoffCushionFraction = Scalar( 0.001 );
		static const Scalar kEmitterProbeStandoffMargin = Scalar( 1.01 );

		//! Gated signal payload recovered by ProbeEmitterSurface for a sampled
		//! luminary point (docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §5).
		//! Only carries fields hand-built records leave default (416 B: construct
		//! only inside EmitterProbeWanted()).  Ungated fields (ptIntersection,
		//! ptObjIntersec, ptCoord, pObjectInfo, transforms, pCamera) are filled
		//! ray-free via FillEmitterRecord / EmitterObjectPoint.
		struct EmitterSurfacePayload
		{
			SurfaceDerivativesInfo	derivatives;	///< drives `curv` / `curvR`
			SurfaceSignalInfo		channel;		///< geometry-signal channel
			TextureFootprint		txFootprint;
			bool					valid;			///< false = gated off or refused; do not apply

			EmitterSurfacePayload() : valid( false ) {}
		};

		/// Sampled emission event from a light, mesh luminary, or environment map.
		struct LightSample
		{
			Point3			position;		///< Emission point
			Vector3			normal;			///< Surface normal at emission point
			Vector3			direction;		///< Emission direction
			RISEPel			Le;				///< Emitted radiance
			Scalar			pdfPosition;	///< Area density on light surface
			Scalar			pdfDirection;	///< Solid angle density of emission direction
			Scalar			pdfSelect;		///< Probability of selecting this light
			bool			isDelta;		///< True for point/spot lights (delta position)
			const ILight*	pLight;			///< Non-null for non-mesh lights
			const IObject*	pLuminary;		///< Non-null for mesh lights
			const IRadianceMap*	pEnvLight;	///< Non-null for SampleEnvLightEmission (for spectral GetRadianceNM)
			EmitterSurfacePayload	surface;	///< Probed signal payload (valid=false on delta/env or when gated off)
			Point3					ptObjIntersec;	///< Ungated object-space emission point (`Po`); (0,0,0) on delta/env/CSG
			Point2					ptCoord;		///< Ungated sampled surface UV; (0,0) on delta/env
		};

		/// Single entry in the combined light table (public for LightBVH and tests).
		struct LightEntry
		{
			const ILightPriv*	pLight;		///< Non-null for non-mesh lights
            const void* physicalLight = nullptr;
            using PhysicalProductNM = Scalar (*)(const void*,const Vector3&,Scalar,
                std::initializer_list<PhysicalLightSpectrum::ProductTerm>,
                std::initializer_list<PhysicalLightSpectrum::ProductTerm>);
            PhysicalProductNM physicalProductNM = nullptr;
			unsigned int		lumIndex;	///< Index into luminaries list (valid when pLight==0)
			Scalar				exitance;	///< MaxValue of radiant exitance
			Point3				position;	///< Representative position for distance estimates
			bool				twoSided;	///< Double-sided mesh luminary (emits both faces); false for delta lights

			bool				hasMotionBounds = false;	///< True when BVH bounds cover the full shutter sweep
			BoundingBox			motionBounds;
			Vector3				motionConeAxis;
			Scalar				motionConeHalfAngle = 0;
		};

		/// One wavelength-independent endpoint draw from a labeled emissive
		/// medium.  `pdf` is q_m^V p_m(y), per scene-volume unit.
		struct VolumeEmissionSample
		{
			const IMedium*	pMedium;
			Point3			point;
			Scalar			mediumSelectionPdf;
			Scalar			pointPdf;
			Scalar			pdf;

			VolumeEmissionSample() :
			  pMedium( 0 ), point( 0, 0, 0 ), mediumSelectionPdf( 0 ),
			  pointPdf( 0 ), pdf( 0 )
			{}
		};

		/// Shared auxiliary pivot vector U for one path vertex.  Each emissive
		/// medium contributes one unconditional wavelength-independent draw;
		/// the later NEE endpoint is sampled separately.
		struct VolumeEmissionPivotState
		{
			std::vector<Point3> mediumPivots;
		};

		/// One vertex's shared thermal-volume auxiliary state.  LightSampler
		/// constructs it in the pinned order -- the complete pivot vector U
		/// first, then the independent labeled endpoint Y -- and consumers get
		/// read-only access so NEE and the outgoing march cannot silently mutate
		/// or regenerate either draw.
		class VolumeEmissionVertexSample
		{
		public:
			VolumeEmissionVertexSample() : pivotsReady(false), endpointAttempted(false),
			  endpointReady(false) {}

			bool HasPivots() const { return pivotsReady; }
			bool WasEndpointAttempted() const { return endpointAttempted; }
			bool HasEndpoint() const { return endpointReady; }
			const VolumeEmissionPivotState& Pivots() const { return pivots; }
			const VolumeEmissionSample& Endpoint() const { return endpoint; }

		private:
			friend class LightSampler;
			VolumeEmissionPivotState pivots;
			VolumeEmissionSample endpoint;
			bool pivotsReady;
			bool endpointAttempted;
			bool endpointReady;
		};

		class LightSampler : public virtual Reference
		{
		public:
			/// DL-463: per-light state accumulated over a motion-blur sweep,
			/// indexed by the light manager's order (lights) and the
			/// luminary list's order (luminaries).
			struct MotionSweepRecord
			{
				Scalar			exitanceSum = 0;	///< Sum over samples of the selection weight
				unsigned int	samples = 0;
				bool			anyPositive = false;
				BoundingBox		bounds;
				bool			boundsValid = false;
				Vector3			coneAxis;
				Scalar			coneHalfAngle = 0;
				bool			coneValid = false;
				// Padding for motion between samples, the ObjectManager
				// rule: half the largest change between consecutive samples
				// (box corners per axis; the cone axis's angle).
				BoundingBox		lastBox;
				Vector3			lastAxis;
				Scalar			boxPad[3] = { 0, 0, 0 };
				Scalar			anglePad = 0;
			};
		protected:
			virtual ~LightSampler();

			/// Cached state set by Prepare()
			std::shared_ptr<const SpectralSourceDomain> spectralDomain;
            SpectralSourceDomain::Mass spectralFinitePower;
            // Spectral-domain preparation is per sampler/pass, never a source cache.
            SpectralSourceDomain::Mass EstimateSpectralPower(const ILightPriv& light) const;
            SpectralSourceDomain::Mass EstimateSpectralPower(const IObject& object) const;
			const IScene*									pPreparedScene;
			const LuminaryManager::LuminariesList*			pPreparedLuminaries;
			Scalar											cachedTotalExitance;

			std::vector<LightEntry>		lightEntries;	///< All selectable lights
			//! Mesh luminary -> its index in `lightEntries`, filled with the
			//! table in Prepare().  FindLuminaryIndex and the two selection-pmf
			//! queries are asked on every MIS-weighted emitter hit; this makes
			//! them one hash lookup instead of a scan of the table.
			std::unordered_map<const IObject*, unsigned int> luminaryEntryIndex;
			std::vector<unsigned int>	positionalLightIndices;	///< Indices into lightEntries for positional (point/spot) lights
			Scalar						positionalLightTotalExitance;	///< Sum of exitance for positional lights
			AliasTable					aliasTable;		///< O(1) selection table
			unsigned int				risCandidates;	///< Number of RIS candidates (0=disabled)
			Scalar						lightSampleRRThreshold;	///< Light-sample RR threshold (0=disabled)
			bool bSceneHasAlphaCoverage = false;
            bool						bSceneHasObjectMedia;	///< True if any object has an interior medium (cached during Prepare)
			bool						bSceneHasObjectFireMedia;	///< True if any object has a fire interior medium (cached during Prepare)
			bool						bSceneHasNullBoundaries;	///< True if any object uses the exact null-boundary material
			std::vector<const IMedium*>	volumeEmissionMedia;	///< Deduplicated labeled thermal emitters
			AliasTable					volumeEmissionAlias;	///< q_m^V proportional to W_m
			bool						volumeEmissionDistributionValid;
			std::vector<Scalar>		positionalLightBandPowers;
			std::vector<Scalar>		volumeEmissionPowerProxies;
			AliasTable					equiangularPivotAlias;	///< a_r over positional lights plus emissive media
			bool						equiangularPivotDistributionValid;

			BoundingBox						sweptSceneBox;		///< Union of world-visible objects' boxes over the sweep (env emission disc)
			bool							sweptSceneBoxValid = false;
			std::vector<MotionSweepRecord>	sweptLights;
			std::vector<MotionSweepRecord>	sweptLuminaries;
			bool							bMotionSweep = false;

			/// Light BVH for importance-weighted selection (null when disabled)
			LightBVH*					pLightBVH;
			bool						bUseLightBVH;		///< True to build and use the light BVH

			/// Environment map importance sampler (null when no env map)
			const EnvironmentSampler*	pEnvSampler;
			const IRadianceMap*			pEnvironmentMap;

			/// Probability SampleLight() selects env vs alias table (5% uniform class
			/// support in legacy mixed scenes; 0 when env absent/infeasible; 1 in env-only).
			Scalar						cachedEnvSelectProb;
			void RecomputeEnvSelectProbability();

			/// Scene bounding sphere cached in Prepare() for env-light disk emission
			/// (radius 0 = no visible geometry / env emission infeasible).
			Point3						cachedSceneCenter;
			Scalar						cachedSceneRadius;

			/// Non-owning optimal MIS accumulator (replaces PowerHeuristic when solved).
			mutable const OptimalMISAccumulator*	pOptimalMIS;

		public:
			/// Light-solo target kind (docs/gui/RENDER_MODES.md §3).
			enum class SoloKind { None, Light, Luminary, Environment };

		protected:
			SoloKind				soloKind;
			const ILightPriv*		soloLight;
			const IObject*			soloLuminaryObject;

		public:
			LightSampler();

			//! Fast atomic gate (`SurfaceCurvatureDemand::Any() || SurfaceSignalDemand::Any()`).
			//! Callers on hot loops may check this to avoid constructing the 416 B payload.
			static bool EmitterProbeWanted();

			//! Probes `pLum` along `-normal` from `ptOnLum + standoff * normal` to
			//! recover geometry signals/derivatives (docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §5).
			//! Gated on EmitterProbeWanted(); accepted iff the hit lands within
			//! `kEmitterProbeAcceptFraction * D` of `ptOnLum`.  Leaves outPayload untouched on false.
			static bool ProbeEmitterSurface(
				const IObject*			pLum,
				const IObjectManager*	pObjects,
				const Point3&			ptOnLum,
				const Vector3&			normal,
				EmitterSurfacePayload&	outPayload
				);

			//! Applies an accepted payload to `rig` (no-op if !payload.valid).
			//! Kept out-of-line so SourceHygieneTest tracks `signals` writers in one TU.
			static void ApplyEmitterSurface(
				RayIntersectionGeometric&		rig,
				const EmitterSurfacePayload&	payload
				);

			//! Single ungated, ray-free filler for sampled-emitter records (position,
			//! normals, ONB, UV, ptObjIntersec, ObjectInfo, transforms, pCamera).
			static void FillEmitterRecord(
				RayIntersectionGeometric&	rig,
				const Point3&				position,
				const Vector3&				normal,
				const Point2&				coord,
				const Point3&				ptObjIntersec,
				const IObject*				pLum = 0,
				const ICamera*				pCamera = 0
				);

            //! Alpha-coverage test for sampled emitter endpoints (ungated; leaves radiance record intact).
            static bool AcceptEmitterAlpha(
                const IObject* luminary, const IObjectManager* objects,
                const Point3& position, const RayIntersectionGeometric& context,
                ISampler& sampler, const RasterizerState* raster = nullptr );

			//! Ungated, ray-free object-space emission point (`Po`) via
			//! GetFinalInverseTransformMatrix().  Returns `fallback` for null or
			//! CSG composites (`GetGeometry() == 0`, which have no single object frame).
			static Point3 EmitterObjectPoint(
				const IObject*	pLum,
				const Point3&	ptWorld,
				const Point3&	fallback
				);

			//! True when luminary geometry is double-sided (emits from both faces); false for null/CSG.
			static bool LuminaryIsTwoSided( const IObject* pLum );

			//! Surface-mean radiant exitance via 10x10 stratified UniformRandomPoint
			//! samples through FillEmitterRecord (returns cached average verbatim if uniform).
			struct LuminaryExitance
			{
				RISEPel	average;		///< surface-mean exitance (before area / face count)
				bool	uniform;		///< true: constant over surface, `average` is emitter's cached value
			};
			static LuminaryExitance AverageLuminaryExitance( const IObject* pLum );

			// Light solo (single-threaded, set before render): restricts direct lighting
			// and emission to one target while CachedPdfSelectLuminary returns 1.0 for
			// the soloed luminary (keeping NEE+BSDF MIS unbiased).
			void SetSoloLight( const ILightPriv* pLight )
			{
				soloKind = SoloKind::Light;
				soloLight = pLight;
				soloLuminaryObject = 0;
			}

			void SetSoloLuminary( const IObject* pLuminaryObject )
			{
				soloKind = SoloKind::Luminary;
				soloLight = 0;
				soloLuminaryObject = pLuminaryObject;
			}

			void SetSoloEnvironment()
			{
				soloKind = SoloKind::Environment;
				soloLight = 0;
				soloLuminaryObject = 0;
			}

			void ClearSolo()
			{
				soloKind = SoloKind::None;
				soloLight = 0;
				soloLuminaryObject = 0;
			}

			bool IsSoloActive() const { return soloKind != SoloKind::None; }
			SoloKind GetSoloKind() const { return soloKind; }

			bool IsSoloTargetLight( const ILightPriv* pLight ) const
			{
				return soloKind == SoloKind::Light && soloLight == pLight;
			}

			bool IsSoloTargetLuminary( const IObject* pLuminaryObject ) const
			{
				return soloKind == SoloKind::Luminary && soloLuminaryObject == pLuminaryObject;
			}

			bool IsSoloTargetEnvironment() const { return soloKind == SoloKind::Environment; }

			void SetOptimalMIS( const OptimalMISAccumulator* pAccum ) const
			{
				pOptimalMIS = pAccum;
			}

            // Configure before Prepare; immutable during sampling.
            void SetSpectralSourceDomain(std::shared_ptr<const SpectralSourceDomain> domain);
            const SpectralSourceDomain* GetSpectralSourceDomain() const { return spectralDomain.get(); }
            Scalar LightSampleRRThreshold() const { return lightSampleRRThreshold; }
            bool IsPrepared() const { return pPreparedScene && pPreparedLuminaries; }
            void CopyMotionSweepFrom(const LightSampler& source, const IScene& scene);

			/// Cache scene + luminaries and build alias table / optional LightBVH.
			void Prepare(
				const IScene& scene,
				const LuminaryManager::LuminariesList& luminaries
				);

			/// Shutter-sweep accumulation for motion blur (single-threaded; consumed by next Prepare()).
			/// Ensures lights/BVH cones active at any shutter sample retain positive PMF.
			void ClearMotionSweep();
			void AccumulateMotionSample(
				const IScene& scene,
				const LuminaryManager::LuminariesList& luminaries
				);

			static Scalar LuminarySelectionWeight( const IObject* pLum );
			static Scalar SweptSelectionWeight( const MotionSweepRecord& rec, const Scalar nominal );
			static void ApplySweptBounds( LightEntry& entry, const MotionSweepRecord& rec );

			bool SampleLight(
				const IScene& scene,
				const LuminaryManager::LuminariesList& luminaries,
				ISampler& sampler,
				LightSample& sample
				) const;

			/// SampleLight() selection PMF = (1 - envSelectProb) * alias pmf (never BVH; BDPT/VCM/MLT MIS partner).
			Scalar PdfSelectLight(
				const IScene& scene,
				const LuminaryManager::LuminariesList& luminaries,
				const ILight& light,
				const Point3& shadingPoint,
				const Vector3& shadingNormal
				) const;

			/// SampleLight() selection PMF for a mesh luminary = (1 - envSelectProb) * alias pmf (never BVH).
			/// PT's NEE partner is CachedPdfSelectLuminary().
			Scalar PdfSelectLuminary(
				const IScene& scene,
				const LuminaryManager::LuminariesList& luminaries,
				const IObject& luminary,
				const Point3& shadingPoint,
				const Vector3& shadingNormal
				) const;

			/// True iff luminary is in the light table (selectable by SampleLight/NEE/SMS/BDPT/VCM/MLT).
			bool IsLuminaryInLightTable(
				const IObject* pLuminary
				) const
			{
				return FindLuminaryIndex( pLuminary ) >= 0;
			}

			/// Unified direct lighting (NEE).  Zero-exitance lights are evaluated
			/// deterministically.  `neeTrainingScale` scales OptimalMIS training only
			/// (never returned radiance); `bBsdfSamplingPartnerExists` disables MIS
			/// when no BSDF-sampling sibling exists; `pGradedIndexStack` (separate
			/// from `pMisIorStack`) prices `(top/n(light))^2` only for graded-index
			/// advancing walks; `bSMSCoversDeltaLights` keeps binary shadows on delta
			/// lights when SMS estimates their specular transmission.
			RISEPel EvaluateDirectLighting(
				const RayIntersectionGeometric& ri,
				const IBSDF& brdf,
				const IMaterial* pMaterial,
				const IRayCaster& caster,
				ISampler& sampler,
				const IObject* pShadingObject,
				const IMedium* pMedium,
				const bool isVolumeScatter,
				const IObject* pMediumObject,
				const IGuidedNEEPdfBlend* pGuidedBlend = 0,
				const IORStack* pMisIorStack = 0,
				const Scalar neeTrainingScale = 1,
				const bool bBsdfSamplingPartnerExists = true,
				const IORStack* pGradedIndexStack = 0,
				const bool bSMSCoversDeltaLights = false,
				const IORStack* pMediumStack = 0
				) const;

			/// Spectral variant of EvaluateDirectLighting.
			Scalar EvaluateDirectLightingNM(
				const RayIntersectionGeometric& ri,
				const IBSDF& brdf,
				const IMaterial* pMaterial,
				const Scalar nm,
				const IRayCaster& caster,
				ISampler& sampler,
				const IObject* pShadingObject,
				const IMedium* pMedium,
				const bool isVolumeScatter,
				const IObject* pMediumObject,
				const IGuidedNEEPdfBlend* pGuidedBlend = 0,
				const IORStack* pMisIorStack = 0,
				const Scalar neeTrainingScale = 1,
				const bool bBsdfSamplingPartnerExists = true,
				const IORStack* pGradedIndexStack = 0,
				const bool bSMSCoversDeltaLights = false,
				const IORStack* pMediumStack = 0
				) const;

			/// Draw a labeled thermal-emission endpoint using q_m^V p_m(y).
			bool SampleVolumeEmission(
				ISampler& sampler,
				VolumeEmissionSample& sample
				) const;

			/// Evaluate the same labeled endpoint density without sampling.
			Scalar VolumeEmissionPdf(
				const IMedium& medium,
				const Point3& point
				) const;

			/// Draw the complete auxiliary pivot vector U: one independent
			/// CDF sample per emissive medium, before endpoint selection.
			bool SampleVolumeEmissionPivots(
				ISampler& sampler,
				VolumeEmissionPivotState& pivots
				) const;

			/// Use the immutable pivot vector carried by an originating vertex,
			/// or draw a local vector when this is a camera/legacy segment.  A
			/// supplied vector is never regenerated: invalid shared state fails
			/// closed so NEE and march cannot condition on different U values.
			bool ResolveVolumeEmissionPivots(
				ISampler& sampler,
				const VolumeEmissionPivotState* sharedPivots,
				VolumeEmissionPivotState& pivots
				) const;

			/// Draw the shared per-vertex auxiliary state in the required order:
			/// every medium pivot U_m unconditionally precedes the independent
			/// labeled volume-NEE endpoint Y.
			bool SampleVolumeEmissionVertex(
				ISampler& sampler,
				VolumeEmissionVertexSample& sample
				) const;

			/// Select one equiangular pivot from the watt-dimensioned mixture.
			/// Positional entries use their fixed light position; medium entries
			/// read the corresponding already-drawn element of U.
			bool SampleEquiangularPivot(
				const VolumeEmissionPivotState& pivots,
				const Scalar xi,
				Point3& pivot,
				Scalar& selectionPdf
				) const;

			/// Evaluate the complete positional-light/medium equiangular distance
			/// mixture conditioned on the already-drawn medium pivot vector U.
			/// This is the one density used by both distance-sampling branches and,
			/// later, by both sides of the volume-emission family MIS partition.
			Scalar EquiangularDistancePdf(
				const VolumeEmissionPivotState& pivots,
				const Ray& ray,
				const Scalar tMin,
				const Scalar tMax,
				const bool segmentBounded,
				const Scalar t
				) const;

			/// Evaluate the spectral distance proposal used by both PT entry
			/// routes on one boundary-delimited segment.  The result is kept in
			/// log form so optically thick NEE/march weights do not underflow.
			MISWeights::LogDensity EvaluateVolumeEmissionDistanceLogDensityNM(
				const IMedium& medium,
				const Ray& ray,
				const Scalar proposalMaxDist,
				const bool surfaceBounded,
				const VolumeEmissionPivotState* pivots,
				const Scalar nm,
				const Scalar eventDistance,
				const bool scattered
				) const;

			/// Walk the exact-null connection from a vertex to a labeled volume
			/// endpoint once, returning physical transmittance, endpoint-medium
			/// identity, and the competing march density conditioned on the same U.
			bool EvaluateVolumeEmissionConnectionNM(
				const Ray& ray,
				const Scalar endpointDistance,
				const IMedium* pOriginMedium,
				const IObject* pOriginMediumObject,
				const IORStack* pOriginStack,
				const Scalar nm,
				const VolumeEmissionPivotState& pivots,
				const Scalar directionPdf,
				Scalar& outTransmittance,
				const IMedium** pEndpointMedium,
				MISWeights::LogDensity& outMarchDensity
				) const;

			unsigned int GetEquiangularPivotEntryCount() const
			{
				return equiangularPivotAlias.Size();
			}

			Scalar GetEquiangularPivotSelectionPdf(
				const unsigned int index
				) const
			{
				return static_cast<Scalar>( equiangularPivotAlias.Pdf(index) );
			}

			Scalar GetEquiangularPivotPower(
				const unsigned int index
				) const;

			bool IsEquiangularPivotDistributionValid() const
			{
				return equiangularPivotDistributionValid;
			}

			/// Low-level Phase-B volume-NEE estimator retained for isolated tests
			/// and non-closure callers. Production PT surface and medium vertices use
			/// the closure-bound siblings below so the competing march density is the
			/// exact proposal later sampled at that same vertex.
			Scalar EvaluateVolumeDirectLightingNM(
				const RayIntersectionGeometric& ri,
				const IBSDF& receiver,
				const Scalar nm,
				ISampler& sampler,
				const IMedium* pMedium,
				const bool isVolumeScatter,
				const IObject* pMediumObject,
				const IORStack* pMediumStack = 0
				) const;

			/// Surface-closure Phase-B estimator.  The shared vertex sample was
			/// drawn before this call; A_march controls the weighted response and
			/// its complement remains an NEE-only weight-one term.
			Scalar EvaluateVolumeDirectLightingFromClosureNM(
				const RayIntersectionGeometric& ri,
				const IContinuationClosureNM& closure,
				const ContinuationAvailability& availability,
				Scalar nm,
				const VolumeEmissionVertexSample& vertexSample,
				const IMedium* pMedium,
				const IObject* pMediumObject,
				const IORStack* pMediumStack = 0
				) const;

			/// Medium-vertex sibling.  A retained wavelength-bound phase closure
			/// supplies both response and direction density; the caller supplies
			/// the exact ordinary-RR survival of that counterfactual continuation.
			Scalar EvaluateVolumeDirectLightingFromPhaseClosureNM(
				const Point3& scatterPoint,
				const Vector3& incomingDirection,
				const IPhaseFunction& phaseClosure,
				const MediumContinuationAvailability& availability,
				Scalar rouletteSurvivalProbability,
				Scalar nm,
				const VolumeEmissionVertexSample& vertexSample,
				const IMedium* pMedium,
				const IObject* pMediumObject,
				const IORStack* pMediumStack = 0
				) const;

			unsigned int GetVolumeEmissionMediumCount() const
			{
				return static_cast<unsigned int>( volumeEmissionMedia.size() );
			}

			bool IsVolumeEmissionDistributionValid() const
			{
				return volumeEmissionDistributionValid;
			}

			/// PT NEE selection PMF for a mesh luminary (LightBVH when built, else alias;
			/// under RIS >0 flags selectability for hit suppression; under solo returns 1
			/// for soloed target in table, 0 otherwise; 0 => caller gives hit full weight).
			Scalar CachedPdfSelectLuminary(
				const IObject& luminary,
				const Point3& shadingPoint,
				const Vector3& shadingNormal
				) const;

			bool IsRISActive() const { return risCandidates > 0 && !IsLightBVHActive(); }
			bool IsLightBVHActive() const { return pLightBVH && pLightBVH->IsBuilt(); }
			void SetUseLightBVH(
				const bool enable
				);

			bool SceneHasAlphaCoverage() const { return bSceneHasAlphaCoverage; }
            bool SceneHasMedia() const { return bSceneHasObjectMedia || (pPreparedScene && pPreparedScene->GetGlobalMedium()); }
            bool SceneHasObjectMedia() const { return bSceneHasObjectMedia; }
			bool SceneHasNullBoundaries() const { return bSceneHasNullBoundaries; }
			bool SceneHasFireMedia() const
			{
				const IMedium* global = pPreparedScene ? pPreparedScene->GetGlobalMedium() : 0;
				return bSceneHasObjectFireMedia || ( global && global->IsFireMedium() );
			}

			void SetRISCandidates(
				const unsigned int M
				);
			/// Sets env map + built EnvironmentSampler (non-owning).
			void SetEnvironmentSampler(
				const IRadianceMap* pEnvMap,
				const EnvironmentSampler* pSampler
				);

			const EnvironmentSampler* GetEnvironmentSampler() const { return pEnvSampler; }
			const Point3& GetCachedSceneCenter() const { return cachedSceneCenter; }
			Scalar GetCachedSceneRadius() const { return cachedSceneRadius; }

			/// Probability SampleLight() draws an env sample (docs/IMPROVEMENTS.md #12,
			/// docs/VCM_ENV_MIS_PARTITION_INVESTIGATION.md).
			Scalar EnvSelectProbability() const
			{
				return cachedEnvSelectProb;
			}

			unsigned int GetPositionalLightCount() const { return (unsigned int)positionalLightIndices.size(); }
			const Point3& GetPositionalLightPosition(
				const unsigned int idx
				) const { return lightEntries[positionalLightIndices[idx]].position; }
			Scalar GetPositionalLightExitance(
				const unsigned int idx
				) const { return lightEntries[positionalLightIndices[idx]].exitance; }
			Scalar GetPositionalLightTotalExitance() const { return positionalLightTotalExitance; }

			/// Light-sample Russian roulette threshold (0 = disabled).
			void SetLightSampleRRThreshold(
				const Scalar threshold
				);

		protected:
			/// RIS light selection (M candidates reweighted by exitance/dist^2; selfIdx
			/// weight zeroed; estimator = integrand * risWeight / pdfAlias, w_nee = 1).
			unsigned int SelectLightRIS(
				const Point3& shadingPoint,
				ISampler& sampler,
				Scalar& pdfAlias,
				Scalar& risWeight,
				const int selfIdx
				) const;

			int FindLuminaryIndex(
				const IObject* pLuminary
				) const;

			/// Infinite-area-light disk emission from the env map (pdfPos = 1/(pi*r^2),
			/// pdfDir = pEnvSampler->Pdf(wi), pdfSelect = 1.0 — mixed-scene callers
			/// overwrite pdfSelect).  External `u1` reuses the env-vs-alias roll to
			/// avoid shifting QMC dimensions.
			bool SampleEnvLightEmission(
				const Scalar u1,
				ISampler& sampler,
				LightSample& sample
				) const;
		};
	}
}

#endif
