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
//  Lights are selected proportional to their radiant exitance
//  (MaxValue of RGB), matching PhotonTracer's strategy.  This
//  means brighter lights get proportionally more subpath starts.
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
//  - Area lights (mesh luminaries): power heuristic MIS weight
//    using the combined PDF (pdfSelect * solid-angle PDF) vs the
//    BSDF sampling PDF.
//
//  Call Prepare() once after the scene is attached to cache the
//  light list, luminaries list, and total exitance.  All query
//  methods then use the cached state.
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
#include "../Rendering/LuminaryManager.h"

namespace RISE
{
	class IRayCaster;
	class IMaterial;

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
		class LightSampler : public virtual Reference
		{
		protected:
			virtual ~LightSampler();

			// Cached state set by Prepare()
			const IScene*									pPreparedScene;
			const LuminaryManager::LuminariesList*			pPreparedLuminaries;
			Scalar											cachedTotalExitance;

		public:
			LightSampler();

			/// Cache the scene and luminaries list for subsequent queries.
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

			/// Returns the exitance-proportional selection probability
			/// for a given mesh luminary using cached total exitance.
			/// Intended for MIS weight computation when a BSDF-sampled
			/// ray hits an emitter (the "other strategy" PDF).
			/// \return Selection probability, or 0 if luminary has no emitter
			Scalar CachedPdfSelectLuminary(
				const IObject& luminary								///< [in] The luminary to query
				) const;

		protected:
			/// Computes the total exitance across all non-mesh lights and mesh luminaries
			Scalar ComputeTotalExitance(
				const IScene& scene,								///< [in] The scene
				const LuminaryManager::LuminariesList& luminaries	///< [in] List of mesh luminaries
				) const;
		};
	}
}

#endif
