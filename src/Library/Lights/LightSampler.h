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
#include "../Intersection/RayIntersectionGeometric.h"	// SurfaceDerivativesInfo / SurfaceSignalInfo / TextureFootprint, by value on EmitterSurfacePayload
#include "../Interfaces/IMedium.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/Reference.h"
#include "../Utilities/ISampler.h"
#include "../Utilities/AliasTable.h"
#include "../Rendering/LuminaryManager.h"
#include "../Rendering/EnvironmentSampler.h"
#include "LightBVH.h"

namespace RISE
{
	class IRayCaster;
	class IMaterial;
	class ILightPriv;

	namespace Implementation { class OptimalMISAccumulator; }

	/// Optional per-call hook (DL-74, docs/DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md
	/// "DL-73 RULED NOT A DEBT" residual) letting a caller replace the raw
	/// material pdf `EvaluateDirectLighting{,NM}`'s env-NEE arm would otherwise
	/// use as the BSDF-sampling technique's MIS-partner density for a given
	/// NEE direction.  Exists so PathTracingIntegrator can plumb the SAME
	/// OpenPGL-guided combined pdf its BSDF-sampling continuation uses for the
	/// escape-direction MIS weight (`PathTransportUtilities::GuidingCombinedPdf`)
	/// into the NEE side too, for the SAME shading point -- without this, the
	/// two sides feed `PowerHeuristic`/`OptimalMIS2Weight` different pdfs for
	/// the same physical direction whenever path guiding is trained and
	/// active, breaking `w_bsdf + w_nee == 1`.  Deliberately NOT specific to
	/// OpenPGL types (no dependency on PathGuidingField here) so this header
	/// stays buildable without RISE_ENABLE_OPENPGL; the concrete
	/// implementation lives behind that guard in PathTracingIntegrator.cpp.
	/// Passing null (the default at every existing call site) reproduces the
	/// pre-DL-74 raw-pdf behavior exactly.
	class IGuidedNEEPdfBlend
	{
	public:
		virtual ~IGuidedNEEPdfBlend() {}

		/// \param wo      The NEE-sampled direction being weighted.
		/// \param rawPdf  The un-guided material pdf the caller already
		///                computed for `wo` (0 if the material has no
		///                support there).
		/// \return The pdf to use in place of `rawPdf` as the BSDF-sampling
		///         technique's MIS-partner density for `wo`.
		virtual Scalar Blend(
			const Vector3& wo,
			Scalar rawPdf
			) const = 0;
	};

	namespace Implementation
	{
		//! THE EMITTER PROBE'S SCALE-RELATIVE CONSTANTS, expressed as a
		//! fraction of the luminary's WORLD bounding-box diagonal `D` --
		//! the same "no absolute length may appear in a geometric gate"
		//! convention `IGeometry::SelfHitRootFloor` and `scaleHint` follow,
		//! and for the same reason: a constant that is right for a 1-unit
		//! lamp is wrong for a 1000-unit sky panel.
		//!
		//! ACCEPT: 1 % of D.  UPPER bound: a hit on a genuinely DIFFERENT
		//! part of a non-convex luminary is O(D) away, so 1 % rejects it
		//! with two orders of margin.  LOWER bound: what the tolerance has
		//! to absorb, now that there is ONE probe and it is NORMAL-ALIGNED
		//! (round-2 transport review, H2 P1-1), is only
		//!
		//!   (a) the PROJECTION RESIDUAL -- `UniformRandomPoint` returns a
		//!       point that is on the true surface only to its own
		//!       accuracy.  `SDFGeometry`'s Newton-projects onto the
		//!       ray-marched zero set; the residual measured during S3 is
		//!       ~3e-5 of the diagonal (LightSampler.cpp quotes the same
		//!       number at the acceptance test, and that is the only place
		//!       it is stated).  An earlier draft of this comment gave 5e-5
		//!       here and 3e-5 there, which is the default `m_epsFrac`
		//!       confused with a measurement.
		//!   (b) the intersector's own perpendicular surface band AT NORMAL
		//!       INCIDENCE -- for an SDF, its hit epsilon over the field's
		//!       Lipschitz shrink; for every analytic primitive, ulp-scale.
		//!
		//! THERE IS NO `delta / cos(theta)` TERM ANY MORE, and that is the
		//! whole point of the unification: the probe stands off along the
		//! sampled point's OWN normal and fires back along it, so the probe
		//! line passes through the sampled point exactly and meets the
		//! surface at incidence 1 by construction.  On a mesh luminary
		//! `UniformRandomPoint`'s normal is the INTERPOLATED vertex normal,
		//! which is not the face normal -- but the probe line still passes
		//! through the sampled point, and a triangle sample has no
		//! projection residual at all, so the two effects multiply to zero
		//! there rather than adding a lateral offset.
		//!
		//! (The pre-unification NEE probe fired along `vToLight`, whose only
		//! guarantee is `cosLight > 0`, and DID carry a `delta / cos(theta)`
		//! term.  Measured on tests/SignalEmitterRecordTest's SDF sphere it
		//! never came close to this tolerance -- family E is green at
		//! `epsilon 0.002`, and an exploratory sweep to `epsilon 0.05` left
		//! it green as well, so that term was far smaller in practice than
		//! the review's arithmetic suggested.  What the along-`vToLight`
		//! probe DID fail at is the wrong-surface window, which is what
		//! family F now witnesses.)
		static const Scalar kEmitterProbeAcceptFraction = Scalar( 0.01 );

		//! STANDOFF FLOOR: 0.1 % of D.  The formula the probe actually uses
		//! is
		//!
		//!   standoff = max( kEmitterProbeStandoffFraction * D,
		//!                   kEmitterProbeStandoffMargin * floorWorld
		//!                     + kEmitterProbeStandoffCushionFraction * D )
		//!
		//! and THIS constant is the first argument: the standoff when the
		//! luminary's own `SelfHitRootFloor` is unavailable (a degenerate
		//! transform, the one branch that cannot ask for it), and the
		//! guarantee that a geometry which UNDER-states its floor -- the
		//! contract permits an ulp-scale answer -- never gets a
		//! zero-length probe ray.
		//!
		//! It has to be a floor rather than the whole standoff because
		//! `IGeometry::SelfHitRootFloor` is virtual with eleven in-tree
		//! overrides and several are far above any fixed fraction of D.
		//! `SDFGeometry`'s, the worst, is
		//! `min( 2 * m_eps / shrink / cosI, 0.5 * diagonal )` with
		//! `m_eps = max( D * m_epsFrac, 1e-6 )`: 1e-4 * D at the 5e-5 scene
		//! default with a uniform field, but 4e-3 * D at `epsilon 0.002`,
		//! 1e-3 * D -- exactly this constant's own fraction, not "far
		//! above" it -- for a part authored `scale 0.1 1 1` (shrink 0.1) at
		//! the default epsilon, and up to 0.5 * D at its documented
		//! grazing/shrink worst case.  Below those the geometry marches the
		//! probe straight past the face it was aimed at and the probe
		//! REFUSES.
		static const Scalar kEmitterProbeStandoffFraction = Scalar( 0.001 );

		//! THE CUSHION ADDED ON TOP OF THE GEOMETRY'S OWN FLOOR, and the
		//! reason it exists: the floor is a bound measured from the TRUE
		//! surface, while what the caller holds is a SAMPLED point that may
		//! sit slightly to either side of it.  `SDFGeometry::March` steps
		//! off when `|Map(o)| <= surfBand`, and `Map` is positive OUTSIDE:
		//! with `o = ptOnLum + s * n` and the sample's true displacement a
		//! signed `delta` (positive = outside), `Map(o) = s + delta`, so a
		//! sample whose projection landed INSIDE (`delta < 0`) LOWERS the
		//! clearance to `s - |delta|` and is the half `kEmitterProbeStandoffMargin
		//! * floor` alone under-shoots on -- a sample that landed OUTSIDE
		//! raises it instead and is the safe half -- and the probe refuses
		//! on those (measured: SignalEmitterRecordTest's family E sat 37 % /
		//! 35 % off its baked control on the BDPT and VCM rows at
		//! `1.01 * floor` with no cushion at all).
		//!
		//! 0.1 % of D -- the smaller of the two values TRIED (0 fails; see
		//! the cushion-0 measurement above).  The first
		//! implementation used `kEmitterProbeAcceptFraction * D` here -- 1 %
		//! -- which is ten times larger than it needs to be and, being
		//! unconditionally above `kEmitterProbeStandoffFraction * D`, made
		//! that constant dead code outside the degenerate-transform branch
		//! (round-2 transport review, H2 P2-3).
		//!
		//! WHAT THE CUSHION COSTS, stated rather than hidden: the probe
		//! takes the CLOSEST hit from `ptOnLum + standoff * n` back along
		//! `-n`, so a SECOND surface of the same luminary lying in the
		//! `(0, standoff]` band directly above the sampled point is
		//! intercepted instead -- and since `standoff` is normally well
		//! under `kEmitterProbeAcceptFraction * D`, that interception is
		//! ACCEPTED rather than refused.  A louvred or stacked
		//! single-object fixture whose blades are within
		//! `max( 0.001 * D, 1.01 * floor + 0.001 * D )` of each other along
		//! the blade normal therefore reads its neighbour's signals.  The
		//! band is a tenth of what it was before this constant was split
		//! out, and no smaller band is available: below the geometry's own
		//! floor the probe cannot be fired at all.
		static const Scalar kEmitterProbeStandoffCushionFraction = Scalar( 0.001 );

		//! How far PAST `SelfHitRootFloor` the standoff stands.  The
		//! published contract (IGeometry.h, "Contract for overriders") is
		//! "the return must be an UPPER bound on the geometry's own gate
		//! along `localDir` -- a caller standing off by more than this and
		//! firing back must get the hit", so ANY strictly-greater
		//! multiplier satisfies it and 1 % is simply a small one.  (An
		//! earlier draft called it "the smallest that also clears the
		//! rounding of the two frame transforms" the floor is carried
		//! through.  Nothing measures that rounding, and nothing here
		//! bounds it; the cushion above, not this multiplier, is what was
		//! actually measured.)
		static const Scalar kEmitterProbeStandoffMargin = Scalar( 1.01 );

		//! THE SHADING PAYLOAD A SAMPLED EMISSION POINT CANNOT CARRY BY
		//! ITSELF (docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §5, slice S3).
		//!
		//! `IObject::UniformRandomPoint` returns a position, a normal and a UV
		//! -- and nothing else.  Every emitter record built from one of those
		//! samples (the two NEE sites in LightSampler.cpp, `SampleLight`'s
		//! own emission record, and the four records BDPT / VCM rebuild
		//! beside it -- the NM hero `Le`, its HWSS companion, the BDPT
		//! `type == LIGHT` root vertex, and VCM's light-vertex NEE record;
		//! seven record sites in all) therefore left `derivatives`,
		//! `signals` and `txFootprint`
		//! DEFAULT, so an emissive material whose radiance keys on `curv`,
		//! `occlusion(r)`, `proximity(r)` -- any of the six geometry signals --
		//! read the documented neutral there while the SAME material read the
		//! live value when a camera ray hit the emitter directly.  That is a
		//! within-scene inconsistency, and it reaches plain PT (NEE), not just
		//! the bidirectional families.
		//!
		//! These three fields are recovered from a REAL intersection --
		//! `LightSampler::ProbeEmitterSurface` fires one object-level
		//! closest-hit at the luminary and keeps the record only if it lands
		//! on the sampled point -- never fabricated.  `valid` false means the
		//! probe was gated off or refused and every consumer must leave its
		//! hand-built record exactly as it was.
		//!
		//! WHY ONLY THESE THREE.  They are precisely the fields the hand-built
		//! records leave DEFAULT *and* that only a signal consumer reads.
		//! Everything the sampled point already determines --
		//! `ptIntersection`, `vNormal`, `vGeomNormal`, `onb`, `ptCoord` -- is
		//! deliberately NOT carried, so the emission geometry and every pdf
		//! derived from it (`cosLight`, `pdfPosition`, `pdfDirection`) stay
		//! bit-for-bit what they are today and the only thing the PROBE can
		//! move is a signal read.
		//!
		//! SIZE: 416 bytes (168 + 136 + 104 + a bool, measured at this
		//! tree's HEAD; an earlier comment said 440).  That is why the call
		//! sites construct one only INSIDE the probe gate rather than once
		//! per light sample unconditionally.
		//!
		//! `ptObjIntersec` DELIBERATELY DOES NOT RIDE HERE, and that is a
		//! correction to this slice's first draft (transport review, P1-1).
		//! `Po` is not signal state: `expression_painter`'s `Po`,
		//! `voronoi3d_painter` and `mapping_painter` all read it, none of
		//! them registers a signal demand, and the probe's gate is
		//! PROCESS-WIDE -- so an emissive material keyed on `Po` would have
		//! rendered 2.94x differently depending on whether some UNRELATED
		//! painter elsewhere in the process happened to keep a signal demand
		//! alive.  It is instead computed UNGATED and WITHOUT a ray at every
		//! emitter-record site by `LightSampler::EmitterObjectPoint`, and
		//! carried on `LightSample::ptObjIntersec` for the five consumers
		//! that rebuild their own record.
		//!
		//! `pmxWorldToObject` is likewise not carried: its only consumer is an
		//! `IRayIntersectionModifier`, and no modifier runs on an emitter
		//! record (`Modify` would perturb `vNormal`, which the paragraph above
		//! forbids).
		struct EmitterSurfacePayload
		{
			SurfaceDerivativesInfo	derivatives;	///< dpdu/dpdv/dndu/dndv + scaleHint + the direct SDF curvature -- drives `curv` / `curvR`
			SurfaceSignalInfo		channel;		///< the geometry-signal channel: own-surface half from the geometry, cross-object triple stamped exactly as ObjectManager::IntersectRay does
			TextureFootprint		txFootprint;	///< pixel footprint; all-zero for a probe ray (no differentials) but carried so a future landing cannot silently reopen the gap
			bool					valid;			///< false = probe gated off or refused; consumers must not apply this

			EmitterSurfacePayload() : valid( false ) {}
		};

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
			/// Non-null when this sample comes from
			/// `SampleEnvLightEmission` (environment-map emission).
			/// The spectral integrators (`GenerateLightSubpathNM`,
			/// NM s=1 NEE / connect) use this to recover wavelength-
			/// resolved emission via `pEnvLight->GetRadianceNM(...)`,
			/// since `Le` only carries RGB.  NULL for explicit lights
			/// and mesh luminaries — they use their own pLight /
			/// pLuminary emitter for NM.
			const IRadianceMap*	pEnvLight;
			/// The shading payload `SampleLight` recovered for `position` by
			/// probing the luminary (see EmitterSurfacePayload above).  Rides
			/// on the sample because FIVE consumers rebuild their own record
			/// from this one point and must all see the same channel:
			/// `SampleLight`'s own `Le` record, `GenerateLightSubpathImpl`'s
			/// NM hero `Le` rebuild AND its HWSS companion-wavelength twin,
			/// the BDPT `type == LIGHT` root vertex (a direct field copy,
			/// not through `ApplyEmitterSurface`), and `VCMIntegrator`'s
			/// light-vertex NEE record.  Apply it with
			/// `LightSampler::ApplyEmitterSurface`.  `valid` false on every
			/// delta light and every env sample (no surface exists) and
			/// whenever the probe was gated off or refused.
			EmitterSurfacePayload	surface;
			/// `position` mapped into the luminary's own object frame -- the
			/// expression VM's `Po`.  UNGATED and ray-free (one matrix
			/// multiply through `IObject::GetFinalInverseTransformMatrix`),
			/// unlike everything on `surface`: `Po` is read by painters that
			/// register no signal demand, so it must not depend on the
			/// process-wide probe gate.  See `EmitterSurfacePayload`'s note.
			/// `(0,0,0)` -- the value every hand-built record used to carry
			/// unconditionally -- for a delta light, an env sample, and a
			/// luminary with no single object frame (a CSG composite).
			Point3					ptObjIntersec;
		};

		/// Unified light sampling utility shared by PT and BDPT.
		///
		/// Wraps existing light/luminaire sampling with explicit PDF
		/// computation and provides a single-call NEE evaluator that
		/// handles both non-mesh lights and mesh luminaries with
		/// correct MIS weights.
		///
		/// Uses an alias table for O(1) light selection and PDF queries.
		/// A single entry in the combined light table.
		/// Public so LightBVH and tests can reference it.
		struct LightEntry
		{
			const ILightPriv*	pLight;		///< Non-null for non-mesh lights
			unsigned int		lumIndex;	///< Index into luminaries list (valid when pLight==0)
			Scalar				exitance;	///< MaxValue of radiant exitance
			Point3				position;	///< Representative position for distance estimates
		};

		class LightSampler : public virtual Reference
		{
		protected:
			virtual ~LightSampler();

			/// Cached state set by Prepare()
			const IScene*									pPreparedScene;
			const LuminaryManager::LuminariesList*			pPreparedLuminaries;
			Scalar											cachedTotalExitance;

			std::vector<LightEntry>		lightEntries;	///< All selectable lights
			std::vector<unsigned int>	positionalLightIndices;	///< Indices into lightEntries for positional (point/spot) lights
			Scalar						positionalLightTotalExitance;	///< Sum of exitance for positional lights
			AliasTable					aliasTable;		///< O(1) selection table
			unsigned int				risCandidates;	///< Number of RIS candidates (0=disabled)
			Scalar						lightSampleRRThreshold;	///< Light-sample RR threshold (0=disabled)
			bool						bSceneHasObjectMedia;	///< True if any object has an interior medium (cached during Prepare)

			/// Light BVH for importance-weighted selection (null when disabled)
			LightBVH*					pLightBVH;
			bool						bUseLightBVH;		///< True to build and use the light BVH

			/// Environment map importance sampler (null when no env map)
			const EnvironmentSampler*	pEnvSampler;
			const IRadianceMap*			pEnvironmentMap;

			/// Pre-computed probability that `SampleLight()` selects env
			/// (vs an alias-table light) on the next call.  Set during
			/// `Prepare()` and `SetEnvironmentSampler()` from env
			/// totalLuminance × disc area vs the alias table's total
			/// weight.  Zero when env doesn't exist or env total
			/// radiance is zero; 1 when env exists and the alias table
			/// is empty (env-only scenes); fractional in mixed
			/// env+other-lights scenes (continuous PMF, matching PBRT-
			/// v4's `LightSampler::PMF(env)` semantics).
			Scalar						cachedEnvSelectProb;

			/// Recomputes `cachedEnvSelectProb` from current env +
			/// alias-table state.  Called from both `Prepare` and
			/// `SetEnvironmentSampler` so the cache is correct
			/// regardless of which is invoked first.
			void RecomputeEnvSelectProbability();

			/// Scene bounding sphere — cached during `Prepare()` by
			/// enumerating every visible object's AABB.  Used by env-
			/// light emission sampling (PBRT-style infinite-area-light
			/// disk emission) so BDPT / VCM / MLT light subpaths can
			/// originate from the environment map on IBL-only scenes
			/// (no explicit luminaries).  Radius 0 means "no geometry
			/// or Prepare() not yet called"; env emission falls back
			/// to false-return from `SampleLight`.
			Point3						cachedSceneCenter;
			Scalar						cachedSceneRadius;

			/// Optimal MIS accumulator — set by the rasterizer before
			/// rendering.  When non-null and solved, EvaluateDirectLighting
			/// uses OptimalMIS2Weight instead of PowerHeuristic.
			/// Lifetime managed by the rasterizer, not owned by LightSampler.
			mutable const OptimalMISAccumulator*	pOptimalMIS;

		public:
			/// LIGHT-SOLO (docs/gui/RENDER_MODES.md §3 "light solo"):
			/// identifies which kind of solo target is active.  `None`
			/// (default) is byte-identical to pre-light-solo behaviour —
			/// every light/luminary/env participates normally.
			enum class SoloKind { None, Light, Luminary, Environment };

		protected:
			SoloKind				soloKind;				///< Active solo target kind (None = disabled)
			const ILightPriv*		soloLight;				///< Valid when soloKind==Light
			const IObject*			soloLuminaryObject;		///< Valid when soloKind==Luminary — compares by identity against LuminaryManager::LUM_ELEM::pLum / PathTracingIntegrator's ri.pObject

		public:
			LightSampler();

			//
			// THE EMITTER-RECORD PROBE (slice S3 of
			// docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §5).
			//
			// Recovers the shading payload (`derivatives` / the signal
			// channel / `txFootprint` / `ptObjIntersec`) for a point that was
			// SAMPLED on a luminary rather than HIT, by firing one
			// object-level closest-hit at that luminary and keeping the
			// resulting record only if it landed on the sampled point.
			//
			// STATIC, and public, for two reasons: the same payload has to
			// reach five hand-built records that live in three different
			// translation units (LightSampler.cpp's two NEE sites,
			// BDPTIntegrator.cpp's NM hero + HWSS companion rebuilds,
			// VCMIntegrator.cpp's light-vertex NEE record), and RGB and NM
			// must share ONE implementation so they cannot drift -- the
			// spectral form of the defect this slice closes.
			//

			//! IS THE PROBE WANTED AT ALL?  `SurfaceCurvatureDemand::Any()
			//! || SurfaceSignalDemand::Any()` -- two relaxed atomic loads.
			//!
			//! `ProbeEmitterSurface` asks this ITSELF and returns false
			//! immediately when it is false, so a call site can never
			//! forget the gate.  It is public only so a site on a hot loop
			//! can ALSO skip the 416-byte `EmitterSurfacePayload` it would
			//! otherwise default-construct per light sample, and skip
			//! fetching the scene's object manager, with the gate closed
			//! (round-2 transport review, H2 P2-6).  Skipping the payload
			//! is the only thing this may be used for; it is not a licence
			//! to build a record from anything but an accepted probe.
			static bool EmitterProbeWanted();

			//! PROBE `pLum` FOR THE SHADING PAYLOAD AT `ptOnLum` -- the ONE
			//! probe, shared by all seven emitter-record sites so that PT,
			//! BDPT and VCM refuse in exactly the same places.
			//!
			//! Stands off from `ptOnLum` along `+normal` and fires back
			//! along `-normal`.  Until the round-2 transport review (H2
			//! P1-1) the two NEE sites instead fired along `vToLight` --
			//! from the shading point toward the sampled point -- through a
			//! SECOND public entry point with its own acceptance behaviour,
			//! so PT and the bidirectional families could and did disagree
			//! about whether a given emitter read its signals live.  That
			//! entry point is gone; `tests/SignalEmitterRecordTest`'s
			//! family F is the witness (PT 23 % off its baked control while
			//! BDPT and VCM were within 0.1 % of theirs, on one scene in one
			//! frame).
			//!
			//! GATED: returns false immediately unless
			//! `EmitterProbeWanted()`.  With the gate closed the rendered
			//! output is unchanged, no ray is cast and no bounding box is
			//! read -- which is the claim that matters and is checkable; an
			//! earlier draft said "byte-for-byte", which overclaims (the
			//! sites still fill `ptObjIntersec`, which is ungated by
			//! design).  The gate lives HERE rather than at the call sites
			//! so a future site cannot forget it.
			//!
			//! THE STANDOFF IS DERIVED FROM THE GEOMETRY, not from a fixed
			//! fraction of the luminary's diagonal `D`:
			//!
			//!   standoff = max( kEmitterProbeStandoffFraction * D,
			//!                   kEmitterProbeStandoffMargin * floorWorld
			//!                     + kEmitterProbeStandoffCushionFraction * D )
			//!
			//! where `floorWorld` is `IObject::SelfHitRootFloor` asked in
			//! the luminary's own local frame and mapped back to world units
			//! by dividing by the direction stretch `|M_inv * dir|` --
			//! exactly the conversion `CSGObject::SelfHitRootFloor` performs
			//! on each operand.  The published contract (IGeometry.h,
			//! "Contract for overriders") is that a caller standing off by
			//! MORE than the returned floor and firing back must get the
			//! hit, so this standoff is the smallest one the engine
			//! guarantees works.  See the three constants above for what
			//! each term is for and what the cushion costs.
			//!
			//! ACCEPTANCE RULE, in full: accepted iff (a) the gate is open,
			//! (b) `pLum` is non-null and its world bounding box has a
			//! finite, strictly-positive diagonal `D`, (c) `normal` is
			//! usable, (d) the derived standoff is finite and no wider than
			//! `D`, (e) the probe ray hits `pLum`, and (f) the hit is within
			//! `kEmitterProbeAcceptFraction * D` of `ptOnLum`.
			//!
			//! THE REFUSAL MODES THAT SURVIVE, and they are now the SAME
			//! THREE for PT, BDPT, VCM and MLT:
			//!
			//!   (a) a non-finite `SelfHitRootFloor`, or one so wide that
			//!       the standoff would exceed `D` -- a geometry whose
			//!       self-hit gate is wider than the whole luminary cannot
			//!       be probed from outside it, and "change nothing" is the
			//!       honest answer.  See the note at that check for which
			//!       geometry can actually reach it.
			//!   (b) a SECOND SURFACE OF THE SAME LUMINARY within the
			//!       standoff band along `+normal` and further than the
			//!       acceptance tolerance from `ptOnLum` -- a louvred or
			//!       stacked single-object fixture.  (Inside the tolerance
			//!       it is accepted instead, which the cushion constant's
			//!       note states.)
			//!   (c) a BSSRDF entry vertex, which is not an emitter record
			//!       at all but is listed with these because it is the
			//!       other place `SurfaceSignalInfo` reads neutral under
			//!       every integrator.
			//!
			//! WHY A DISTANCE TEST AND NOT "any hit on the right object":
			//! for a CONVEX luminary the near-side hit along the normal IS
			//! the sampled point, so the test passes by construction; for a
			//! concave one (a torus, a mesh shell, a two-lobe SDF) some
			//! other part of the same surface can intercept, and its
			//! curvature / occlusion / proximity are a different material
			//! state entirely.  Reading THAT would be a fabrication with
			//! extra steps.
			//!
			//! \return TRUE and fills @a outPayload (with `valid` true);
			//!         FALSE leaves @a outPayload untouched.
			static bool ProbeEmitterSurface(
				const IObject*			pLum,			///< [in] the luminary the point was sampled on
				const IObjectManager*	pObjects,		///< [in] the scene's object manager -- becomes `signals.pScene`, exactly as ObjectManager::IntersectRay stamps it
				const Point3&			ptOnLum,		///< [in] the sampled point, in world space
				const Vector3&			normal,			///< [in] unit outward normal at @a ptOnLum, as `UniformRandomPoint` returned it
				EmitterSurfacePayload&	outPayload		///< [out] written only on acceptance
				);

			//! Copy an accepted payload onto a hand-built emitter record.  A
			//! no-op when `payload.valid` is false, which is what makes every
			//! call site's fallback "do nothing" rather than "build a second
			//! record".
			//!
			//! Deliberately OUT OF LINE (not an inline in this header): the
			//! `signals` assignment it performs is the one SourceHygieneTest
			//! pins at file granularity, and keeping it in LightSampler.cpp
			//! keeps the sanctioned writer set at ONE new file for this slice
			//! instead of four.
			static void ApplyEmitterSurface(
				RayIntersectionGeometric&		rig,
				const EmitterSurfacePayload&	payload
				);

			//! THE OBJECT-SPACE EMISSION POINT -- `Po` to the expression VM.
			//!
			//! UNGATED, RAY-FREE, and deliberately NOT part of the probe
			//! payload (transport review P1-1).  `Po` is read by painters
			//! that register no signal demand at all -- `expression_painter`
			//! exposes it directly, `voronoi3d_painter` and
			//! `mapping_painter` key on it -- so carrying it on the
			//! PROCESS-WIDE-gated `EmitterSurfacePayload` made an emissive
			//! material keyed on `Po` render differently depending on
			//! whether some unrelated painter elsewhere in the process
			//! happened to keep a signal demand alive (measured at 2.94x on
			//! the S3 review's repro).
			//!
			//! It also closes a PRE-EXISTING inconsistency that predates
			//! this slice entirely and reaches plain PT: a camera ray that
			//! HIT the emitter got a live `Po` from `Object::IntersectRay`,
			//! while every NEE / light-root record built from the same
			//! sampled point carried `(0,0,0)`.  CLOSED FOR AN `Object`,
			//! NOT FOR A CSG COMPOSITE: a composite returns @a fallback
			//! here (see below), so its NEE `Po` is still `(0,0,0)` while a
			//! direct hit on it gets the WINNING OPERAND's object point
			//! through `CSGObject::AdoptCsgSurfacePayload`.  That gap is
			//! unchanged by this slice and is stated wherever the closure
			//! is claimed (round-2 transport review, H2 P2-7).
			//!
			//! HOW: one multiply through the luminary's own
			//! `GetFinalInverseTransformMatrix()` -- the very matrix
			//! `Object::IntersectRay` transforms its ray with.  No
			//! intersection is performed and nothing is estimated.
			//!
			//! IT IS NOT THE SAME NUMBER A DIRECT HIT PRODUCES, quite:
			//! `Object::IntersectRay` writes
			//! `localRay.PointAtLength( range - SURFACE_INTERSEC_ERROR )`,
			//! i.e. it backs the object-space point off along the LOCAL ray
			//! by the object's own self-hit epsilon, while this is the
			//! EXACT inverse image of the world point.  The two therefore
			//! agree to `SURFACE_INTERSEC_ERROR` (1e-12 by default,
			//! author-settable per object), not bit-for-bit.
			//!
			//! \return the object-space point, or @a fallback when the
			//!         luminary has no single object frame.  A CSG COMPOSITE
			//!         is that case: `CSGObject::IntersectRay` leaves
			//!         `ptObjIntersec` in the WINNING CHILD OPERAND's frame
			//!         (see `AdoptCsgSurfacePayload`) and clears
			//!         `pmxWorldToObject` for exactly this reason -- the map
			//!         into that frame is the child's inverse composed with
			//!         every enclosing composite's, and which child wins is
			//!         a property of the hit, not of the point.  Detected as
			//!         `GetGeometry() == 0`, which is true of `CSGObject`
			//!         and of a container node and false of every concrete
			//!         `Object`.  Passing the record's existing value as
			//!         @a fallback makes the CSG case "change nothing".
			static Point3 EmitterObjectPoint(
				const IObject*	pLum,			///< [in] the luminary the point was sampled on; null yields @a fallback
				const Point3&	ptWorld,		///< [in] the sampled point, in world space
				const Point3&	fallback		///< [in] what to return when there is no single object frame
				);

			//
			// LIGHT SOLO — render with exactly one light enabled.
			//
			// Designates exactly one light as the sole active light: every
			// OTHER light contributes exactly zero direct lighting
			// (EvaluateDirectLighting{,NM}'s three stages — the zero-
			// exitance deterministic loop, the alias/BVH/RIS-table
			// selection, and env-NEE — each admit ONLY the soloed
			// identity), and CachedPdfSelectLuminary returns 1.0 for the
			// soloed luminary so the integrator's BSDF-hit emission MIS
			// partner (PathTracingIntegrator.cpp PART 1) agrees with NEE's
			// own bypassed selection pdf of 1.0 — this is what keeps the
			// combined NEE+BSDF-hit MIS estimator UNBIASED under solo
			// (both strategies see the same, degenerate single-light
			// selection distribution) rather than merely "mostly dark".
			// Non-target emitters' BSDF-hit emission is suppressed
			// entirely by the integrator (see PART 1's soloSuppressEmission
			// gate), not left to a zero MIS weight — an unreachable-by-NEE
			// emitter (CanBeAreaLight()==false) would otherwise leak
			// unweighted energy through under solo exactly as it does
			// today outside solo.
			//
			// Setting one target clears any other.  Single-threaded: set
			// BEFORE a render, read-only during it — same discipline as
			// PathTracingIntegrator::SetMaxPathDepth.  Default: no solo
			// (SoloKind::None), byte-identical to today.
			//

			/// Designates a non-mesh (point/spot/ambient/directional)
			/// light as the sole active light.
			void SetSoloLight( const ILightPriv* pLight )
			{
				soloKind = SoloKind::Light;
				soloLight = pLight;
				soloLuminaryObject = 0;
			}

			/// Designates a mesh luminary as the sole active light.
			/// `pLuminaryObject` is the IObject* owning the emissive
			/// material — the SAME identity LuminaryManager::LUM_ELEM::
			/// pLum and a BSDF-sampled hit's `ri.pObject` compare against.
			void SetSoloLuminary( const IObject* pLuminaryObject )
			{
				soloKind = SoloKind::Luminary;
				soloLight = 0;
				soloLuminaryObject = pLuminaryObject;
			}

			/// Designates the environment map (global radiance map) as
			/// the sole active light.  Structural completeness for the
			/// solo mechanism's three EvaluateDirectLighting stages — not
			/// yet reachable by name from the agent `render{light:}`
			/// surface (P2b resolves ILightManager + mesh-luminary names
			/// only; env-by-name is a possible future extension, see
			/// docs/gui/RENDER_MODES.md).
			void SetSoloEnvironment()
			{
				soloKind = SoloKind::Environment;
				soloLight = 0;
				soloLuminaryObject = 0;
			}

			/// Clears any solo target — every light contributes normally
			/// again.  Default state.
			void ClearSolo()
			{
				soloKind = SoloKind::None;
				soloLight = 0;
				soloLuminaryObject = 0;
			}

			/// \return True when a solo target is active.
			bool IsSoloActive() const { return soloKind != SoloKind::None; }

			/// \return The active solo kind (None when inactive).
			SoloKind GetSoloKind() const { return soloKind; }

			/// \return True when `pLight` IS the solo target.  Used by
			/// EvaluateDirectLighting{,NM}'s Step-1 zero-exitance loop to
			/// admit only the soloed light.
			bool IsSoloTargetLight( const ILightPriv* pLight ) const
			{
				return soloKind == SoloKind::Light && soloLight == pLight;
			}

			/// \return True when `pLuminaryObject` IS the solo target.
			/// Used by EvaluateDirectLighting{,NM}'s Step-2 bypass,
			/// CachedPdfSelectLuminary's pdfSelect==1.0 branch, and
			/// PathTracingIntegrator's PART-1 emission gate.
			bool IsSoloTargetLuminary( const IObject* pLuminaryObject ) const
			{
				return soloKind == SoloKind::Luminary && soloLuminaryObject == pLuminaryObject;
			}

			/// \return True when the environment map IS the solo target.
			bool IsSoloTargetEnvironment() const { return soloKind == SoloKind::Environment; }

			/// Sets the optimal MIS accumulator for direct lighting.
			/// The accumulator must outlive the LightSampler's use of it.
			/// Pass NULL to disable optimal MIS and revert to PowerHeuristic.
			void SetOptimalMIS( const OptimalMISAccumulator* pAccum ) const
			{
				pOptimalMIS = pAccum;
			}

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
				const ILight& light,								///< [in] The light to query
				const Point3& shadingPoint,							///< [in] Shading point (used for BVH PDF; ignored when BVH inactive)
				const Vector3& shadingNormal						///< [in] Shading normal (used for BVH PDF; ignored when BVH inactive)
				) const;

			/// Returns the probability of selecting a given mesh luminary
			/// \return Selection probability proportional to exitance
			Scalar PdfSelectLuminary(
				const IScene& scene,								///< [in] The scene containing lights
				const LuminaryManager::LuminariesList& luminaries,	///< [in] List of mesh luminaries
				const IObject& luminary,							///< [in] The luminary to query
				const Point3& shadingPoint,							///< [in] Shading point (used for BVH PDF; ignored when BVH inactive)
				const Vector3& shadingNormal						///< [in] Shading normal (used for BVH PDF; ignored when BVH inactive)
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
				const IObject* pShadingObject,						///< [in] Object being shaded (to skip self-illumination)
				const IMedium* pMedium,								///< [in] Current participating medium for transmittance (NULL = vacuum)
				const bool isVolumeScatter,							///< [in] True for volume scatter points — skips cosine weighting and hemisphere rejection
				const IObject* pMediumObject,						///< [in] Object enclosing the medium (NULL = unbounded/global medium)
				const IGuidedNEEPdfBlend* pGuidedBlend = 0			///< [in] DL-74: optional env-NEE MIS-partner pdf override (see IGuidedNEEPdfBlend)
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
				const IObject* pShadingObject,						///< [in] Object being shaded (to skip self-illumination)
				const IMedium* pMedium,								///< [in] Current participating medium for transmittance (NULL = vacuum)
				const bool isVolumeScatter,							///< [in] True for volume scatter points — skips cosine weighting and hemisphere rejection
				const IObject* pMediumObject,						///< [in] Object enclosing the medium (NULL = unbounded/global medium)
				const IGuidedNEEPdfBlend* pGuidedBlend = 0			///< [in] DL-74: optional env-NEE MIS-partner pdf override (see IGuidedNEEPdfBlend)
				) const;

			/// Returns the alias-table selection probability for a given
			/// mesh luminary.  Used for MIS weight computation when a
			/// BSDF-sampled ray hits an emitter (the "other strategy"
			/// PDF).  When RIS is active the caller should NOT use this
			/// for MIS — the BSDF-hit emitter contribution is suppressed
			/// instead (see PathTracingShaderOp).
			/// \return Selection probability, or 0 if luminary not found
			Scalar CachedPdfSelectLuminary(
				const IObject& luminary,							///< [in] The luminary to query
				const Point3& shadingPoint,							///< [in] Shading point (used for BVH PDF; ignored when BVH inactive)
				const Vector3& shadingNormal						///< [in] Shading normal (used for BVH PDF; ignored when BVH inactive)
				) const;

			/// Returns whether RIS spatial resampling is active.
			bool IsRISActive() const { return risCandidates > 0 && !IsLightBVHActive(); }

			/// Returns whether the light BVH is built and active.
			bool IsLightBVHActive() const { return pLightBVH && pLightBVH->IsBuilt(); }

			/// Enables or disables the light BVH.  When enabled, the BVH
			/// is built during Prepare() and used for spatially-aware light
			/// selection with full MIS support.  When disabled (default),
			/// the alias table + optional RIS is used.
			void SetUseLightBVH(
				const bool enable									///< [in] True to enable light BVH
				);

			/// Returns whether the scene has any participating media
			/// (per-object or global).  Used to gate shadow transmittance
			/// evaluation — when false, all shadow transmittance calls
			/// are skipped.
			bool SceneHasMedia() const { return bSceneHasObjectMedia || (pPreparedScene && pPreparedScene->GetGlobalMedium()); }

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

			/// \return The cached scene bounding-sphere centre (set by
			/// Prepare()).  Used by BDPT Path B (eye-subpath escape) to
			/// place the synthetic env-light vertex on the disc that
			/// SampleEnvLightEmission would have produced — keeps the
			/// vertex's distance and pdfFwd numerically consistent with
			/// the area-measure conventions the BDPT MIS walk expects.
			const Point3& GetCachedSceneCenter() const { return cachedSceneCenter; }

			/// \return The cached scene bounding-sphere radius.  Zero
			/// before Prepare() completes or on degenerate scenes (no
			/// visible geometry).  Callers should treat zero as "env
			/// vertex placement infeasible" and skip the synthetic
			/// vertex push.
			Scalar GetCachedSceneRadius() const { return cachedSceneRadius; }

			/// \return Probability that `SampleLight()` produces an
			/// env-light sample (one with `pEnvLight != NULL`) on the
			/// next call.  Continuous in (0, 1] when env exists, by
			/// the env-vs-alias selection roll in `SampleLight()` —
			/// computed during `Prepare()` from env totalLuminance ×
			/// disc area vs the alias table's total weight.  Matches
			/// PBRT-v4's `LightSampler::PMF(env)` semantics so MIS at
			/// the Path B s=0 site has a non-zero pdfRev in mixed
			/// scenes (env + other lights), which closes the MIS
			/// partition-of-unity that the previous binary 0-or-1
			/// formulation broke — see `docs/IMPROVEMENTS.md` #12 and
			/// `docs/PRE_PHASE1_STATUS.md` Session 9 for the rationale
			/// (continuous-PMF refactor 2026-05-29).
			Scalar EnvSelectProbability() const
			{
				return cachedEnvSelectProb;
			}

			/// \return Number of positional (point/spot) lights suitable for equiangular sampling
			unsigned int GetPositionalLightCount() const { return (unsigned int)positionalLightIndices.size(); }

			/// Get position of a positional light by index [0, GetPositionalLightCount())
			const Point3& GetPositionalLightPosition(
				const unsigned int idx							///< [in] Index into positional light list
				) const { return lightEntries[positionalLightIndices[idx]].position; }

			/// Get exitance of a positional light by index [0, GetPositionalLightCount())
			Scalar GetPositionalLightExitance(
				const unsigned int idx							///< [in] Index into positional light list
				) const { return lightEntries[positionalLightIndices[idx]].exitance; }

			/// Get total exitance of all positional lights
			Scalar GetPositionalLightTotalExitance() const { return positionalLightTotalExitance; }

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

			/// PBRT-style infinite-area-light emission sampling for
			/// scenes whose only light source is the environment map.
			///
			/// Geometry: importance-sample a sky direction `wi` from
			/// the env map; place the emission vertex on a disk of
			/// radius `cachedSceneRadius` perpendicular to `wi`,
			/// pushed `cachedSceneRadius` along `wi` away from the
			/// scene centre so the disk lies entirely outside the
			/// scene bounding sphere.  The disk normal points back
			/// INTO the scene (= -wi), so `cosAtLight = 1.0`
			/// uniformly and the disk emits parallel rays in the
			/// direction `-wi` (= sky → scene).
			///
			/// PDFs:
			///   pdfPosition  = 1 / (π · r²)           — uniform on disk
			///   pdfDirection = pEnvSampler->Pdf(wi)   — solid angle
			///   pdfSelect    = 1.0                   — env is the only light
			///
			/// Caveats: the resulting `LightSample.pLight` and
			/// `.pLuminary` are both NULL.  BDPT connection strategies
			/// (s=1 NEE from eye to light vertex 0) currently fall
			/// through their `if (pLight) ... else if (pLuminary) ...`
			/// chains and contribute 0 for env-light vertex 0 — direct
			/// env-NEE on eye vertices still flows through the standard
			/// env-sampler path so there is no double-loss.  s>=2
			/// strategies (light-subpath bounces from the env-light into
			/// the scene and connects to the eye via geometry vertices)
			/// work fully and are what unblocks the IBL-only render
			/// from showing as fully-black.
			///
			/// \return True on success (env sampler must be valid +
			/// scene radius > 0).  False when env or scene info is
			/// missing — caller's `SampleLight` returns false too.
			///
			/// `u1` is the first uniform random for env-direction
			/// importance sampling (the second one is drawn from
			/// `sampler` internally).  Passing `u1` externally lets
			/// `SampleLight` re-use its env-vs-alias selection roll
			/// as `u1` after re-mapping into `[0, 1)` — keeps the
			/// total `Get1D()` consumption per `SampleLight` call
			/// identical to the prior binary-PMF flow (no Sobol /
			/// QMC dimension shift; see Prepare()'s continuous-PMF
			/// block for the 2026-05-26 dimension-shift regression
			/// that motivated this signature).  `pdfSelect` is left
			/// at its computed env-only value (1.0); callers in
			/// mixed scenes should overwrite `sample.pdfSelect`
			/// with the actual env-vs-alias selection probability.
			bool SampleEnvLightEmission(
				const Scalar u1,									///< [in] First uniform random for direction sampling
				ISampler& sampler,									///< [in] Low-discrepancy sampler (for remaining randoms)
				LightSample& sample									///< [out] Populated env-light emission sample
				) const;
		};
	}
}

#endif
