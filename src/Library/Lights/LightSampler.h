//////////////////////////////////////////////////////////////////////
//
//  LightSampler.h - Unified light source sampling with explicit
//  PDFs for path tracing and bidirectional path tracing.
//
//  Provides two services:
//
//  1. EMISSION SAMPLING (SampleLight) — used by BDPT to start
//     light subpaths.  Selects a light proportional to exitance,
//     samples position and direction, returns explicit PDFs.
//
//  2. DIRECT LIGHTING (EvaluateDirectLighting) — used by both PT
//     and BDPT for next-event estimation (NEE).  Selects one light
//     (non-mesh or mesh) proportional to exitance, evaluates shadow
//     visibility and BRDF, and applies MIS weights against BSDF
//     sampling.  Lights with zero exitance (ambient, directional)
//     are evaluated deterministically since they cannot participate
//     in proportional selection.
//
//  LIGHT SELECTION:
//  An alias table (Vose's algorithm) is built during Prepare()
//  over all lights weighted by radiant exitance.  This gives O(1)
//  selection and O(1) PDF lookup regardless of light count.
//
//  SPATIAL RESAMPLING (RIS):
//  When SetRISCandidates(M) is called with M>0, direct lighting
//  evaluation uses Resampled Importance Sampling: M candidates are
//  drawn from the global alias table and reweighted by
//  exitance/distance^2 at the shading point.  One candidate is
//  then selected proportional to these spatially-aware weights.
//  This concentrates samples on lights that contribute most from
//  the current shading position, dramatically reducing variance
//  in many-light scenes where distant lights dominate the global
//  distribution but contribute negligibly to local illumination.
//
//  EMISSION SAMPLING:
//  - Non-mesh lights (point/spot): delta position (pdfPosition=1),
//    uniform solid angle sampling (point=sphere, spot=cone),
//    pdfDirection queried from the light via pdfDirection().
//  - Mesh luminaries: uniform position on surface (pdfPos=1/area),
//    cosine-weighted hemisphere direction (pdfDir=cos/pi).
//
//  MIS FOR DIRECT LIGHTING:
//  - Delta lights (point/spot): no MIS needed (only one sampling
//    strategy can reach a delta position).
//  - Area lights (mesh luminaries), RIS OFF: power heuristic MIS
//    weight using the alias-table selection PDF converted to solid
//    angle vs the BSDF sampling PDF.
//  - Area lights (mesh luminaries), RIS ON: MIS is disabled
//    (w_nee = 1).  The exact finite-M RIS technique density is
//    intractable (it requires marginalizing over all possible
//    M-candidate sets), so no closed-form MIS weight is available.
//    The BSDF-hit emitter contribution is suppressed on the
//    PathTracingShaderOp side to avoid double-counting.
//
//  SELF-EXCLUSION:
//  When the shading object is itself an emitter in the light table,
//  it is excluded from selection (self-illumination is physically
//  meaningless for convex/flat surfaces).  For RIS, the self
//  entry's resampling weight is zeroed.  For the alias table, a
//  rejection draw is used with a (1-p_self) correction factor.
//  This prevents wasting samples on an always-zero contribution.
//
//  Call Prepare() once after the scene is attached to cache the
//  light list, luminaries list, and build the alias table.  All
//  query methods then use the cached state.
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

#include "../Interfaces/IScene.h"
#include "../Interfaces/ILight.h"
#include "../Interfaces/IObject.h"
#include "../Interfaces/IBSDF.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/Reference.h"
#include "../Utilities/ISampler.h"
#include "../Utilities/AliasTable.h"
#include "../Rendering/LuminaryManager.h"
#include "../Rendering/EnvironmentSampler.h"

namespace RISE
{
	class IRayCaster;
	class IMaterial;
	class ILightPriv;

	namespace Implementation
	{
		/// Describes a sampled emission event from a light or mesh luminary
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
		};

		/// Unified light sampling utility shared by PT and BDPT.
		///
		/// Wraps existing light/luminaire sampling with explicit PDF
		/// computation and provides a single-call NEE evaluator that
		/// handles both non-mesh lights and mesh luminaries with
		/// correct MIS weights.
		///
		/// Uses an alias table for O(1) light selection and PDF queries.
		class LightSampler : public virtual Reference
		{
		protected:
			virtual ~LightSampler();

			/// Cached state set by Prepare()
			const IScene*									pPreparedScene;
			const LuminaryManager::LuminariesList*			pPreparedLuminaries;
			Scalar											cachedTotalExitance;

			/// A single entry in the combined light table
			struct LightEntry
			{
				const ILightPriv*	pLight;		///< Non-null for non-mesh lights
				unsigned int		lumIndex;	///< Index into luminaries list (valid when pLight==0)
				Scalar				exitance;	///< MaxValue of radiant exitance
				Point3				position;	///< Representative position for distance estimates
			};

			std::vector<LightEntry>		lightEntries;	///< All selectable lights
			AliasTable					aliasTable;		///< O(1) selection table
			unsigned int				risCandidates;	///< Number of RIS candidates (0=disabled)
			Scalar						lightSampleRRThreshold;	///< Light-sample RR threshold (0=disabled)

			/// Environment map importance sampler (null when no env map)
			const EnvironmentSampler*	pEnvSampler;
			const IRadianceMap*			pEnvironmentMap;

		public:
			LightSampler();

			/// Cache the scene and luminaries list for subsequent queries.
			/// Builds the alias table for O(1) light selection.
			/// Must be called once after the scene is fully attached and
			/// the luminary list is built (e.g. in RayCaster::AttachScene).
			void Prepare(
				const IScene& scene,								///< [in] The scene containing lights
				const LuminaryManager::LuminariesList& luminaries	///< [in] List of mesh luminaries
				);

			/// Selects a light proportional to exitance, samples an emission
			/// position and direction, and fills the LightSample struct.
			/// \return True if a valid sample was generated, false if no lights exist
			bool SampleLight(
				const IScene& scene,								///< [in] The scene containing lights
				const LuminaryManager::LuminariesList& luminaries,	///< [in] List of mesh luminaries
				ISampler& sampler,									///< [in] Low-discrepancy sampler
				LightSample& sample									///< [out] The generated light sample
				) const;

			/// Returns the probability of selecting a given non-mesh light
			/// \return Selection probability proportional to exitance
			Scalar PdfSelectLight(
				const IScene& scene,								///< [in] The scene containing lights
				const LuminaryManager::LuminariesList& luminaries,	///< [in] List of mesh luminaries
				const ILight& light									///< [in] The light to query
				) const;

			/// Returns the probability of selecting a given mesh luminary
			/// \return Selection probability proportional to exitance
			Scalar PdfSelectLuminary(
				const IScene& scene,								///< [in] The scene containing lights
				const LuminaryManager::LuminariesList& luminaries,	///< [in] List of mesh luminaries
				const IObject& luminary								///< [in] The luminary to query
				) const;

			//
			// Unified direct lighting evaluation (NEE)
			//

			/// Evaluates direct lighting at a shading point by selecting
			/// one light source proportional to exitance and computing
			/// the shadowed, BRDF-weighted, MIS-weighted contribution.
			///
			/// Lights with zero exitance (ambient, directional) are
			/// evaluated deterministically outside the stochastic
			/// selection to preserve backward compatibility.
			///
			/// \return Direct lighting contribution (RGB)
			RISEPel EvaluateDirectLighting(
				const RayIntersectionGeometric& ri,					///< [in] Geometric intersection at shading point
				const IBSDF& brdf,									///< [in] BRDF at the shading point
				const IMaterial* pMaterial,							///< [in] Material at shading point (for BSDF PDF query)
				const IRayCaster& caster,							///< [in] Ray caster for shadow tests
				ISampler& sampler,									///< [in] Low-discrepancy sampler
				const IObject* pShadingObject						///< [in] Object being shaded (to skip self-illumination)
				) const;

			/// Spectral variant of EvaluateDirectLighting.
			/// \return Direct lighting contribution for a single wavelength
			Scalar EvaluateDirectLightingNM(
				const RayIntersectionGeometric& ri,					///< [in] Geometric intersection at shading point
				const IBSDF& brdf,									///< [in] BRDF at the shading point
				const IMaterial* pMaterial,							///< [in] Material at shading point (for BSDF PDF query)
				const Scalar nm,									///< [in] Wavelength in nanometers
				const IRayCaster& caster,							///< [in] Ray caster for shadow tests
				ISampler& sampler,									///< [in] Low-discrepancy sampler
				const IObject* pShadingObject						///< [in] Object being shaded (to skip self-illumination)
				) const;

			/// Returns the alias-table selection probability for a given
			/// mesh luminary.  Used for MIS weight computation when a
			/// BSDF-sampled ray hits an emitter (the "other strategy"
			/// PDF).  When RIS is active the caller should NOT use this
			/// for MIS — the BSDF-hit emitter contribution is suppressed
			/// instead (see PathTracingShaderOp).
			/// \return Selection probability, or 0 if luminary not found
			Scalar CachedPdfSelectLuminary(
				const IObject& luminary								///< [in] The luminary to query
				) const;

			/// Returns whether RIS spatial resampling is active.
			bool IsRISActive() const { return risCandidates > 0; }

			/// Sets the number of RIS candidates for spatially-aware
			/// light selection.  When M>0, EvaluateDirectLighting draws
			/// M candidates from the global alias table and resamples
			/// one proportional to exitance/distance^2.  When M=0
			/// (default), plain alias-table selection is used.
			void SetRISCandidates(
				const unsigned int M								///< [in] Number of RIS candidates (0=disabled)
				);

			/// Sets the environment map and its importance sampler for
			/// environment NEE.  The EnvironmentSampler must already be
			/// built (Build() called).  Ownership is NOT transferred.
			void SetEnvironmentSampler(
				const IRadianceMap* pEnvMap,							///< [in] The radiance map (for radiance queries)
				const EnvironmentSampler* pSampler					///< [in] The importance sampler (for direction sampling/PDF)
				);

			/// \return The environment importance sampler, or NULL
			const EnvironmentSampler* GetEnvironmentSampler() const { return pEnvSampler; }

			/// Sets the threshold for light-sample Russian roulette.
			/// When > 0, mesh luminary shadow samples whose estimated
			/// geometric contribution (exitance * cos_surface * area *
			/// cos_light / dist^2) falls below this threshold are
			/// probabilistically terminated.  Survivors are divided
			/// by the survival probability to maintain unbiasedness.
			/// When 0 (default), all shadow samples are evaluated.
			void SetLightSampleRRThreshold(
				const Scalar threshold								///< [in] RR threshold (0=disabled)
				);

		protected:
			/// Selects one light using RIS: draws M candidates from the
			/// alias table, reweights by exitance/dist^2, and returns
			/// the selected index.
			///
			/// When selfIdx >= 0, that entry's resampling weight is
			/// forced to zero so self-illumination is excluded from the
			/// candidate pool without wasting the sample.
			///
			/// Returns two values:
			///   pdfAlias   = alias-table PDF q(j) of the selected light
			///   risWeight  = RIS correction: (1/M) * sum(W_i) / W_j
			///
			/// The caller's estimator should be:
			///   result = integrand * risWeight / pdfAlias
			///
			/// When RIS is active, MIS with BSDF sampling is disabled
			/// (w_nee = 1) because the exact finite-M technique density
			/// is intractable.
			///
			/// \return Selected lightEntries index
			unsigned int SelectLightRIS(
				const Point3& shadingPoint,							///< [in] World-space shading position
				ISampler& sampler,									///< [in] Sampler for candidate draws
				Scalar& pdfAlias,									///< [out] Alias-table PDF (for estimator weight)
				Scalar& risWeight,									///< [out] RIS correction factor
				const int selfIdx									///< [in] Index to exclude (-1 = none)
				) const;

			/// Finds the lightEntries index for a given luminary object.
			/// \return Index into lightEntries, or -1 if not found
			int FindLuminaryIndex(
				const IObject* pLuminary							///< [in] The luminary to search for
				) const;
		};
	}
}

#endif
