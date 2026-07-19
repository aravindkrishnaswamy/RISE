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
	class IPainter;
	class IBSDF;
	class ISPF;
	class IMaterial;

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

			//! GUI render modes P2a fix (docs/gui/RENDER_MODES.md §6): the hard
			//! cap on path-vertex loop iterations, replacing what used to be a
			//! hardcoded `const unsigned int maxDepth = 128;` literal in both
			//! the Pel/NM templated loop (IntegrateFromHitTemplated) and the
			//! standalone HWSS loop (IntegrateFromHitHWSS).  See SetMaxPathDepth
			//! below for the exact depth-accounting mapping.  Defaults to 128 so
			//! every existing caller (nothing called the setter before this fix)
			//! gets byte-identical behavior.
			unsigned int		mMaxPathDepth;

			//! GUI render modes P2b `indirect` mode -- see SetIndirectOnly's
			//! doc for the exact semantics.  Default false.
			bool				mIndirectOnly;

			//! GUI render modes P2b `clay_lights` mode -- see
			//! SetClayOverride's doc for the exact semantics.  Default false.
			bool				mClayOverride;

			//! Shared clay reflectance state for `clay_lights`: a mid-grey
			//! (~0.5 albedo) UniformColorPainter wrapped by a LambertianBRDF
			//! and a LambertianSPF, all three built ONCE in the constructor
			//! and released in the destructor.  Stateless and read-only for
			//! the lifetime of the integrator (no scene-derived state, no
			//! mutation after construction), so concurrent reads from every
			//! render-worker thread are safe with no extra synchronization --
			//! the same "read-only for the duration of a render" contract
			//! every other material's IBSDF/ISPF already relies on.  Built
			//! unconditionally (not lazily) so a mid-render SetClayOverride
			//! flip -- which the controller/agent factories never actually
			//! do (the flag is stamped once at pipeline construction, before
			//! the pipeline is ever handed to the render loop) -- would still
			//! be safe if it ever happened.
			const IPainter*		pClayPainter;
			const IBSDF*		pClayBRDF;
			const ISPF*			pClaySPF;

			//! P1-c fix (review-p2b): a lightweight IMaterial adapter that
			//! wraps pClayBRDF/pClaySPF -- passed to LightSampler::
			//! EvaluateDirectLighting{,NM} in place of the authored
			//! ri.pMaterial wherever the NEE eval itself already uses the
			//! clay BRDF.  GetBSDF()/GetSPF() return pClayBRDF/pClaySPF
			//! directly (no duplicate Lambertian pair); GetEmitter() is
			//! always null (clay never lights up -- the surface's own
			//! emission is handled separately and is never clayed).  Pdf/
			//! PdfNM are NOT overridden -- they inherit IMaterial's base
			//! implementation, which delegates to GetSPF()->Pdf(...),
			//! i.e. pClaySPF's OWN pdf formula.  That is deliberate: it
			//! guarantees the MIS BSDF-sampling pdf used by NEE is the
			//! EXACT SAME function as the pdf the continuation ray is
			//! actually sampled from (pClaySPF::Scatter), by construction,
			//! with no hand-duplicated cos/PI formula to drift out of
			//! sync.  Built once alongside the other three clay members;
			//! same stateless/read-only-after-construction contract.
			const IMaterial*	pClayMaterial;

		public:
			PathTracingIntegrator(
				const ManifoldSolverConfig& smsConfig,
				const StabilityConfig& stabilityCfg
				);

			//! Configure the path-vertex loop cap (see mMaxPathDepth's doc).
			//! DEPTH ACCOUNTING: the main loop is
			//! `for (depth = startDepth; depth < mMaxPathDepth; depth++)`.
			//! `depth` is 0-based and counts LOOP ITERATIONS = VERTICES
			//! PROCESSED, not "bounces past the camera hit": iteration
			//! `depth == startDepth` (startDepth is 0 for every camera-ray
			//! entry point -- IntegrateRay/IntegrateRayNM/IntegrateRayHWSS all
			//! pass 0) is the camera-hit vertex ITSELF -- its emission + NEE
			//! contribution is "direct lighting", not an indirect bounce. Only
			//! at the TOP of the NEXT iteration does the loop re-intersect the
			//! scene along the continuation ray sampled during the previous
			//! iteration's BSDF-sample step -- THAT re-intersection is bounce 1.
			//! So:
			//!   n == 1  -> only the camera-hit vertex is processed (emission +
			//!              NEE at that hit); a continuation ray IS sampled at
			//!              the end of that iteration (harmless wasted RNG/BSDF
			//!              work) but the loop exits before it is ever traced --
			//!              genuinely "direct lighting only, zero bounces".
			//!              This is the mapping the "direct" BeautyVariant mode
			//!              (variantMaxBounces=1) relies on.
			//!   n == 2  -> camera hit + exactly one indirect bounce.
			//!   n == 128 (default) -> byte-identical to the pre-fix hardcoded
			//!              behavior.
			//! `n == 0` maps to 128 (the historical default) rather than a cap
			//! that would process zero vertices and return black.
			void SetMaxPathDepth( unsigned int n ) { mMaxPathDepth = ( n == 0 ) ? 128 : n; }

			//! GUI render modes P2b `indirect` mode (docs/gui/RENDER_MODES.md
			//! §3 Lighting): when true, suppresses the DIRECT contribution
			//! evaluated at the camera-visible vertex in BOTH the templated
			//! Pel/NM loop (IntegrateFromHitTemplated) and the standalone
			//! HWSS loop (IntegrateFromHitHWSS).  A directly-visible
			//! env/radiance-map background is "direct" light too (for the
			//! same reason a directly-visible emitter is), but a camera ray
			//! that misses the scene entirely NEVER enters the path loop --
			//! it returns at the top-level primary-miss branch in
			//! IntegrateRayTemplated / IntegrateRayHWSS (before
			//! IntegrateFromHit) -- so THAT is where the directly-visible-
			//! background suppression lives (an `if( mIndirectOnly ) return
			//! zero;` there); the loop's own env-miss adds are reachable
			//! only for CONTINUATION rays (depth >= 1 = indirect environment
			//! lighting) and are kept.
			//!
			//! review-p3 P2-a: the direct term at the camera-visible vertex
			//! is TWO estimator halves, both of which must be suppressed --
			//! NEE@depth0 (the light-sampling strategy's shadow-ray
			//! contribution, MIS-weighted) PLUS emission@depth1-from-that-
			//! vertex (the BSDF/phase-sampled ray fired FROM the camera-
			//! visible vertex that happens to land directly on an emitter --
			//! the MIS *partner* of the depth0 NEE, arriving one loop
			//! iteration later).  Suppressing only the NEE half left
			//! `indirect` rendering direct x w_bsdf instead of zero
			//! whenever the BSDF-sampling strategy has non-negligible pdf
			//! toward the light (a large area light or a bright env -- a
			//! small/distant lamp has w_bsdf approx 0 and hid the leak).
			//! So the two gates are NOT symmetric:
			//!   - NEE gate:      LOOP-LOCAL `depth == 0` only.
			//!   - emission gate: LOOP-LOCAL `depth <= 1`.
			//! depth>=2 emission is the MIS partner of NEE at depth>=1
			//! (genuinely indirect transport) and stays untouched; NEE at
			//! depth>=1 is genuinely indirect and stays untouched too (no
			//! gate applies there at all).
			//!
			//! Both gates use the raw LOOP-LOCAL `depth`, deliberately NOT
			//! `depth - startDepth`: the HWSS loop mid-path-delegates to
			//! IntegrateFromHitNM (the "no BSDF" / SSS-fallback branches,
			//! and the volume phase-function continuation) by passing its
			//! OWN current `depth` as the delegated call's `startDepth`, so
			//! an offset check would spuriously re-trigger at that
			//! delegated call's first iteration even when the ORIGINAL
			//! global depth was already > 1 (genuine indirect transport).
			//! A raw `depth <= 1` compare has no such ambiguity: every
			//! mid-path re-entry (HWSS delegation, the BSSRDF continuation's
			//! recursive caster call, and the medium phase-sampled
			//! continuation at startDepth=1) passes the CURRENT nonzero
			//! depth onward, so the loop-local `depth` can never return to
			//! 0 or 1 partway through a path that has already gone deeper --
			//! the ONLY way to observe depth==1 inside the loop is either
			//! the true camera path's first bounce, or a delegated call
			//! whose OWN startDepth is 1 (the medium in-scattering
			//! continuation, which IS the camera-visible vertex's MIS
			//! partner by construction -- see IntegrateRayTemplated's
			//! medium-scatter branch, `IntegrateFromHitForTag<Tag>(...,
			//! /*startDepth*/1, ...)`).  Both cases are correctly direct.
			//! The continuation ray is STILL sampled and traced at every
			//! vertex -- only the depth<=1 emission / depth==0 NEE
			//! contribution is zeroed, so every indirect bounce is
			//! untouched and full-fidelity.  Net effect: a directly-lit
			//! diffuse surface (or a directly-visible emitter/env
			//! background) reads BLACK at the camera-visible vertex, INCLUDING
			//! its BSDF-sampled MIS partner; energy that arrives via a
			//! genuine >=2-bounce path reads normally.  Default false;
			//! every existing caller is byte-identical.
			void SetIndirectOnly( bool b ) { mIndirectOnly = b; }

			//! GUI render modes P2b `clay_lights` mode (docs/gui/RENDER_MODES.md
			//! §3 Lighting): when true, every surface's REFLECTANCE (both the
			//! BRDF acquired at the top of the surface-hit block and the SPF
			//! used for the continuation-ray direction) is substituted with a
			//! SHARED, STATELESS, mid-grey (~0.5 albedo) Lambertian pair --
			//! see pClayBRDF/pClaySPF below.  Real lights and real global
			//! illumination stay untouched: the emitter accessor
			//! (`ri.pMaterial->GetEmitter()`) is NEVER substituted, so
			//! emissive surfaces still emit exactly as authored, and the
			//! substituted clay BRDF/SPF still receive real NEE/BSDF-sampled
			//! illumination from the scene's real lights and real multi-
			//! bounce GI.  Because the substitution makes EVERY hit report a
			//! non-null BRDF, the "no-BSDF / pure-SPF" branch (originally
			//! reached by delta-only materials such as mirrors/glass) is
			//! never taken under this mode either -- under `clay_lights`
			//! every surface, including originally-specular ones, scatters
			//! as clay so the render answers "is the lighting right,
			//! independent of ANY surface's material," not just diffuse
			//! ones.  NEE's MIS BSDF-sampling pdf is ALSO substituted (see
			//! ClayNEEMaterial / pClayMaterial in the .cpp) so the eval
			//! numerator (clay BRDF) and the MIS denominator's BSDF-pdf
			//! term always agree -- otherwise clay_lights would be biased
			//! and its output would still depend on the hidden authored
			//! material's Pdf() (P1-c fix).  The authored material's
			//! subsurface transport (both the disk-projection diffusion-
			//! profile path and random-walk SSS) is also BYPASSED under
			//! this mode (P2-d fix): a surface with SSS parameters shades
			//! as plain clay Lambertian, not a translucent clay-tinted
			//! diffuser, so the mode's "independent of ANY surface's
			//! material" contract extends to volumetric-looking materials
			//! too, not just BSDF/SPF ones.  Default false; every existing
			//! caller is byte-identical.
			void SetClayOverride( bool b ) { mClayOverride = b; }

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
				Scalar result[SampledWavelengths::N],
				PixelAOV* pAOV = 0
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
				Scalar result[SampledWavelengths::N],
				PixelAOV* pAOV = 0
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
