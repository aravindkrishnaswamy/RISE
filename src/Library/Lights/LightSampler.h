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
			/// Non-null when this sample comes from
			/// `SampleEnvLightEmission` (environment-map emission).
			/// The spectral integrators (`GenerateLightSubpathNM`,
			/// NM s=1 NEE / connect) use this to recover wavelength-
			/// resolved emission via `pEnvLight->GetRadianceNM(...)`,
			/// since `Le` only carries RGB.  NULL for explicit lights
			/// and mesh luminaries — they use their own pLight /
			/// pLuminary emitter for NM.
			const IRadianceMap*	pEnvLight;
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
			LightSampler();

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
				const IObject* pMediumObject						///< [in] Object enclosing the medium (NULL = unbounded/global medium)
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
				const IObject* pMediumObject						///< [in] Object enclosing the medium (NULL = unbounded/global medium)
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
