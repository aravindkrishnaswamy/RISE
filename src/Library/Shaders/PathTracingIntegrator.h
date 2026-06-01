//////////////////////////////////////////////////////////////////////
//
//  PathTracingIntegrator.h - Standalone iterative unidirectional
//    path tracer.  Modeled on BDPTIntegrator: uses direct
//    intersection (no shader dispatch), inline material evaluation,
//    and iterative main loop with stochastic single-lobe scatter
//    selection (path-tree branching was excised in 2026-05).
//
//    Features:
//    - Next event estimation (NEE) via LightSampler with MIS
//    - BSDF sampling with MIS-weighted emission
//    - BSSRDF disk-projection and random-walk SSS
//    - Specular Manifold Sampling (SMS) for caustics
//    - Participating media (delta-tracking scatter + transmittance)
//    - Per-type bounce limits and Russian roulette
//    - Path guiding (OpenPGL) integration
//    - Optimal MIS weight accumulation
//    - Environment map evaluation with MIS
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 10, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PATHTRACING_INTEGRATOR_
#define PATHTRACING_INTEGRATOR_

#include "../Interfaces/IReference.h"
#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/IScene.h"
#include "../Interfaces/IRadianceMap.h"
#include "../Utilities/Reference.h"
#include "../Utilities/ManifoldSolver.h"
#include "../Utilities/StabilityConfig.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Color/Color_Template.h"
#include "../Utilities/Color/SampledWavelengths.h"
#include "../Utilities/Color/SpectralValueTraits.h"
#include "../Utilities/IORStack.h"
#include "../Intersection/RayIntersection.h"

namespace RISE
{
	struct PixelAOV;
	class ISampler;

	namespace Implementation
	{
		class LightSampler;
		class PathGuidingField;

		class PathTracingIntegrator :
			public virtual IReference,
			public virtual Reference
		{
		protected:
			virtual ~PathTracingIntegrator();

			ManifoldSolver*		pSolver;
			const bool			bSMSEnabled;
			StabilityConfig		stabilityConfig;

		public:
			PathTracingIntegrator(
				const ManifoldSolverConfig& smsConfig,
				const StabilityConfig& stabilityCfg
				);

			/// Traces one complete path from a camera ray and returns
			/// the estimated radiance (RGB).  Optionally populates
			/// first-hit AOV data for the denoiser.
			RISEPel IntegrateRay(
				const RuntimeContext& rc,
				const RasterizerState& rast,
				const Ray& cameraRay,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				const IRadianceMap* pRadianceMap,
				PixelAOV* pAOV
				) const;

			/// Traces a path starting from a pre-computed surface hit.
			/// Both IntegrateRay and the ShaderOp wrapper delegate here.
			/// pAOV (when non-null and rc.aovPrefilterMode is Accurate)
			/// receives the first-non-delta-vertex aux capture: the
			/// loop walks past delta-only scatters (glass / mirror)
			/// and records albedo + normal at the first vertex where
			/// the shader sampled a non-delta direction.  When
			/// rc.aovPrefilterMode is Fast, pAOV is filled at the
			/// camera ray's first hit by IntegrateRay before this
			/// function is called and is not touched here.
			RISEPel IntegrateFromHit(
				const RuntimeContext& rc,
				const RasterizerState& rast,
				const RayIntersection& firstHit,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				const IRadianceMap* pRadianceMap,
				unsigned int startDepth,
				const IORStack& initialIorStack,
				Scalar bsdfPdf_,
				const RISEPel& bsdfTimesCos_,
				bool considerEmission_,
				Scalar importance_,
				IRayCaster::RAY_STATE::RayType rayType_,
				unsigned int diffuseBounces_,
				unsigned int glossyBounces_,
				unsigned int transmissionBounces_,
				unsigned int translucentBounces_,
				unsigned int volumeBounces_,
				Scalar glossyFilterWidth_,
				bool smsPassedThroughSpecular_,
				bool smsHadNonSpecularShading_,
				PixelAOV* pAOV = 0
				) const;

			/// Traces a path starting from a pre-computed surface hit (NM).
			/// Both IntegrateRayNM and the ShaderOp wrapper delegate here.
			Scalar IntegrateFromHitNM(
				const RuntimeContext& rc,
				const RasterizerState& rast,
				const RayIntersection& firstHit,
				const Scalar nm,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				const IRadianceMap* pRadianceMap,
				unsigned int startDepth,
				const IORStack& initialIorStack,
				Scalar bsdfPdf_,
				Scalar bsdfTimesCosNM_,
				bool considerEmission_,
				Scalar importance_,
				IRayCaster::RAY_STATE::RayType rayType_,
				unsigned int diffuseBounces_,
				unsigned int glossyBounces_,
				unsigned int transmissionBounces_,
				unsigned int translucentBounces_,
				unsigned int volumeBounces_,
				Scalar glossyFilterWidth_,
				bool smsPassedThroughSpecular_ = false,
				bool smsHadNonSpecularShading_ = false,
				PixelAOV* pAOV = 0
				) const;

			/// Traces a path starting from a pre-computed surface hit (HWSS).
			/// Both IntegrateRayHWSS and the ShaderOp wrapper delegate here.
			void IntegrateFromHitHWSS(
				const RuntimeContext& rc,
				const RasterizerState& rast,
				const RayIntersection& firstHit,
				SampledWavelengths& swl,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				const IRadianceMap* pRadianceMap,
				unsigned int startDepth,
				const IORStack& initialIorStack,
				Scalar bsdfPdf_,
				bool considerEmission_,
				Scalar importance_,
				IRayCaster::RAY_STATE::RayType rayType_,
				unsigned int diffuseBounces_,
				unsigned int glossyBounces_,
				unsigned int transmissionBounces_,
				unsigned int translucentBounces_,
				unsigned int volumeBounces_,
				Scalar glossyFilterWidth_,
				Scalar result[SampledWavelengths::N]
				) const;

			/// Traces one complete path for a single wavelength.
			Scalar IntegrateRayNM(
				const RuntimeContext& rc,
				const RasterizerState& rast,
				const Ray& cameraRay,
				const Scalar nm,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				const IRadianceMap* pRadianceMap,
				PixelAOV* pAOV = 0
				) const;

			/// Traces one complete path for a bundle of HWSS wavelengths.
			void IntegrateRayHWSS(
				const RuntimeContext& rc,
				const RasterizerState& rast,
				const Ray& cameraRay,
				SampledWavelengths& swl,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				const IRadianceMap* pRadianceMap,
				Scalar result[SampledWavelengths::N]
				) const;

			/// Accessors for NM/HWSS code in PathTracingShaderOp
			bool GetSMSEnabled() const { return bSMSEnabled; }
			ManifoldSolver* GetSolver() const { return pSolver; }

		private:
			// ----------------------------------------------------------------
			// Phase 2b templatization (Pre-Phase-1 Piece 3).  The Pel (RGB)
			// and NM (single-wavelength) variants of the camera-ray entry
			// and the iterative core are collapsed into function templates on
			// a SpectralDispatch tag (PelTag / NMTag).  HWSS is deliberately
			// NOT a tag — IntegrateRayHWSS / IntegrateFromHitHWSS remain
			// standalone hero-bundle methods (see SpectralValueTraits.h
			// header comment).  These are only instantiated inside
			// PathTracingIntegrator.cpp by the thin public forwarders, so
			// adding them changes neither the vtable nor the class layout.

			/// Shared body of IntegrateFromHit / IntegrateFromHitNM.  The
			/// iterative path-tracing core; HWSS stays standalone.  The
			/// SMS emission-suppression flags and pAOV are carried through;
			/// the AOV first-non-delta hook compiles in only for the
			/// AOV-capable tag (supports_aov).  Only instantiated inside
			/// PathTracingIntegrator.cpp by the thin public forwarders, so
			/// adding it changes neither the vtable nor the class layout.
			template<class Tag>
			typename SpectralDispatch::SpectralValueTraits<Tag>::value_type
			IntegrateFromHitTemplated(
				const RuntimeContext& rc,
				const RasterizerState& rast,
				const RayIntersection& firstHit,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				const IRadianceMap* pRadianceMap,
				unsigned int startDepth,
				const IORStack& initialIorStack,
				Scalar bsdfPdf_,
				const typename SpectralDispatch::SpectralValueTraits<Tag>::value_type& bsdfTimesCos_,
				bool considerEmission_,
				Scalar importance_,
				IRayCaster::RAY_STATE::RayType rayType_,
				unsigned int diffuseBounces_,
				unsigned int glossyBounces_,
				unsigned int transmissionBounces_,
				unsigned int translucentBounces_,
				unsigned int volumeBounces_,
				Scalar glossyFilterWidth_,
				bool smsPassedThroughSpecular_,
				bool smsHadNonSpecularShading_,
				PixelAOV* pAOV,
				const Tag& tag
				) const;

			/// Shared body of IntegrateRay / IntegrateRayNM.
			template<class Tag>
			typename SpectralDispatch::SpectralValueTraits<Tag>::value_type
			IntegrateRayTemplated(
				const RuntimeContext& rc,
				const RasterizerState& rast,
				const Ray& cameraRay,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				const IRadianceMap* pRadianceMap,
				PixelAOV* pAOV,
				const Tag& tag
				) const;

			/// Tag-dispatched delegation to IntegrateFromHit / IntegrateFromHitNM
			/// for the medium-scatter continuation and surface hand-off inside
			/// IntegrateRayTemplated.  The SMS emission-suppression flags are
			/// always false here (camera-ray entry), matching every original
			/// IntegrateRay* call site.
			template<class Tag>
			typename SpectralDispatch::SpectralValueTraits<Tag>::value_type
			IntegrateFromHitForTag(
				const RuntimeContext& rc,
				const RasterizerState& rast,
				const RayIntersection& firstHit,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				const IRadianceMap* pRadianceMap,
				unsigned int startDepth,
				const IORStack& initialIorStack,
				Scalar bsdfPdf,
				const typename SpectralDispatch::SpectralValueTraits<Tag>::value_type& bsdfTimesCos,
				bool considerEmission,
				Scalar importance,
				IRayCaster::RAY_STATE::RayType rayType,
				unsigned int diffuseBounces,
				unsigned int glossyBounces,
				unsigned int transmissionBounces,
				unsigned int translucentBounces,
				unsigned int volumeBounces,
				Scalar glossyFilterWidth,
				PixelAOV* pAOV,
				const Tag& tag
				) const;
		};
	}
}

#endif
