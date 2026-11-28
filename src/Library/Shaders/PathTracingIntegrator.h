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

#include <atomic>
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

			unsigned int		mMaxPathDepth;	//!< Vertex-loop cap (default 128; see SetMaxPathDepth)
			bool				mIndirectOnly;	//!< Suppresses camera-vertex direct lighting (see SetIndirectOnly)
			bool				mClayOverride;	//!< Mid-grey Lambertian reflectance override (see SetClayOverride)

			//! One-shot diagnostic gate for fire transport in Pel mode.
			mutable std::atomic<bool>	mFirePelDiagnosticEmitted;

			//! Phase-B diagnostic witnesses (relaxed atomics; never read by transport).
			mutable std::atomic<bool>	mUnsupportedContinuationDiagnosticEmitted;
			mutable std::atomic<bool>	mUnsupportedFallbackSegmentObserved;
			mutable std::atomic<bool>	mUnsupportedFallbackSegmentCompeted;
			mutable std::atomic<bool>	mUnsupportedFallbackEndpointAttempted;
			mutable std::atomic<unsigned long long>	mCompetingMediumVertexCount;
			mutable std::atomic<unsigned long long>	mCompetingMediumEndpointAttemptCount;
			mutable std::atomic<unsigned long long>	mNonCompetingMediumFallbackVertexCount;
			mutable std::atomic<bool>				mCompetingMediumGuideAlphaNonzero;
			mutable std::atomic<unsigned long long>	mCompetingMediumGuideSampleCount;
			mutable std::atomic<bool>				mCompetingMediumReachPdfMismatch;
			mutable std::atomic<bool>				mCompetingMediumZeroSurvivalObserved;
			mutable std::atomic<bool>				mCompetingMediumIntermediateSurvivalObserved;
			mutable std::atomic<bool>				mCompetingMediumUnitSurvivalObserved;
			mutable std::atomic<unsigned long long>	mNonCompetingSurfaceGuideInitializationCount;
			mutable std::atomic<unsigned long long>	mNonCompetingSurfaceRISCount;
			mutable std::atomic<unsigned long long>	mSurfaceVolumeEndpointAttemptCount;
			mutable std::atomic<bool>				mSSSOrdinaryDirectContributionObserved;
			mutable std::atomic<unsigned long long>	mNonCompetingSurfaceGuidedCandidateInstalledCount;
			mutable std::atomic<unsigned long long>	mNonCompetingSurfaceRISCandidateInstalledCount;

			//! Shared, stateless, read-only mid-grey Lambertian for `clay_lights`
			//! (from CreateClayOverrideMaterial).  pClayBRDF/pClaySPF are borrowed
			//! from pClayMaterial so NEE evaluation, continuation sampling, and
			//! MIS Pdf share one parameter set; GetEmitter() is always null.
			const IBSDF*		pClayBRDF;
			const ISPF*			pClaySPF;
			const IMaterial*	pClayMaterial;

		private:
			static std::atomic<long long> sConstructionCount;
			static std::atomic<long long> sDestructionCount;

		public:
			PathTracingIntegrator(
				const ManifoldSolverConfig& smsConfig,
				const StabilityConfig& stabilityCfg
				);

			//! Process-wide refcount/lifetime test hooks (relaxed atomics only).
			static long long ConstructionCount() { return sConstructionCount.load( std::memory_order_relaxed ); }
			static long long DestructionCount() { return sDestructionCount.load( std::memory_order_relaxed ); }

			//! Builds the synthetic mid-grey Lambertian used by `clay_lights` (caller owns ref).
			static const IMaterial* CreateClayOverrideMaterial();

			//! Diagnostic-only witnesses for Phase-B volume/guide regression tests.
			bool UnsupportedContinuationDiagnosticEmitted() const {
				return mUnsupportedContinuationDiagnosticEmitted.load(
					std::memory_order_relaxed );
			}
			bool UnsupportedFallbackSegmentObserved() const {
				return mUnsupportedFallbackSegmentObserved.load(
					std::memory_order_relaxed );
			}
			bool UnsupportedFallbackSegmentCompeted() const {
				return mUnsupportedFallbackSegmentCompeted.load(
					std::memory_order_relaxed );
			}
			bool UnsupportedFallbackEndpointAttempted() const {
				return mUnsupportedFallbackEndpointAttempted.load(
					std::memory_order_relaxed );
			}
			unsigned long long CompetingMediumVertexCount() const {
				return mCompetingMediumVertexCount.load(std::memory_order_relaxed);
			}
			bool CompetingMediumGuideAlphaNonzero() const {
				return mCompetingMediumGuideAlphaNonzero.load(
					std::memory_order_relaxed);
			}
			unsigned long long CompetingMediumGuideSampleCount() const {
				return mCompetingMediumGuideSampleCount.load(
					std::memory_order_relaxed);
			}
			bool CompetingMediumReachPdfMismatch() const {
				return mCompetingMediumReachPdfMismatch.load(
					std::memory_order_relaxed);
			}
			bool CompetingMediumZeroSurvivalObserved() const {
				return mCompetingMediumZeroSurvivalObserved.load(
					std::memory_order_relaxed);
			}
			bool CompetingMediumIntermediateSurvivalObserved() const {
				return mCompetingMediumIntermediateSurvivalObserved.load(
					std::memory_order_relaxed);
			}
			bool CompetingMediumUnitSurvivalObserved() const {
				return mCompetingMediumUnitSurvivalObserved.load(
					std::memory_order_relaxed);
			}
			unsigned long long CompetingMediumEndpointAttemptCount() const {
				return mCompetingMediumEndpointAttemptCount.load(
					std::memory_order_relaxed);
			}
			unsigned long long NonCompetingMediumFallbackVertexCount() const {
				return mNonCompetingMediumFallbackVertexCount.load(
					std::memory_order_relaxed);
			}
			unsigned long long NonCompetingSurfaceGuideInitializationCount() const {
				return mNonCompetingSurfaceGuideInitializationCount.load(
					std::memory_order_relaxed);
			}
			unsigned long long NonCompetingSurfaceRISCount() const {
				return mNonCompetingSurfaceRISCount.load(
					std::memory_order_relaxed);
			}
			unsigned long long SurfaceVolumeEndpointAttemptCount() const {
				return mSurfaceVolumeEndpointAttemptCount.load(
					std::memory_order_relaxed);
			}
			bool SSSOrdinaryDirectContributionObserved() const {
				return mSSSOrdinaryDirectContributionObserved.load(
					std::memory_order_relaxed);
			}
			unsigned long long NonCompetingSurfaceGuidedCandidateInstalledCount() const {
				return mNonCompetingSurfaceGuidedCandidateInstalledCount.load(
					std::memory_order_relaxed);
			}
			unsigned long long NonCompetingSurfaceRISCandidateInstalledCount() const {
				return mNonCompetingSurfaceRISCandidateInstalledCount.load(
					std::memory_order_relaxed);
			}

			//! Configure vertex-loop cap (`depth < mMaxPathDepth`; 0 maps to
			//! kDefaultPathTracingMaxDepth = 128).  `depth` counts processed
			//! vertices: n == 1 is direct lighting only (camera hit, 0 bounces;
			//! spectral fire may still march the outgoing segment once for
			//! source/thermal collision without processing its endpoint); n == 2
			//! is camera hit + 1 indirect bounce.
			void SetMaxPathDepth( unsigned int n ) { mMaxPathDepth = ( n == 0 ) ? kDefaultPathTracingMaxDepth : n; }

			//! `indirect` render mode (docs/gui/RENDER_MODES.md §3): suppresses
			//! camera-visible direct light as an MIS pair across Pel/NM and HWSS
			//! while keeping continuation sampling in lockstep:
			//!   - primary miss (env): suppressed in IntegrateRay*
			//!   - NEE: suppressed unconditionally at raw loop-local `depth == 0`
			//!   - emission: suppressed at `depth == 0`, or (`depth == 1` &&
			//!     depth-0 scatter was non-delta, tracked via bPassedThroughSpecular)
			//!   - loop env-miss: suppressed at `depth == 1` && depth-0 non-delta
			//! Uses raw `depth` (never `depth - startDepth`) because mid-path
			//! HWSS->NM, BSSRDF, and medium delegations pass current depth as startDepth.
			void SetIndirectOnly( bool b ) { mIndirectOnly = b; }

			//! `clay_lights` render mode (docs/gui/RENDER_MODES.md §3): substitutes
			//! every surface's BRDF, SPF, and NEE MIS Pdf with the shared neutral
			//! Lambertian (`pClayMaterial`) and bypasses SSS/BSSRDF, leaving
			//! authored emitters and real lights/GI untouched.
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
				PixelAOV* pAOV,
				bool* pPrimaryHit = 0
				) const;

			/// Traces the indirect-only estimator once while also returning the
			/// camera-vertex direct estimator suppressed by that mode.  This is
			/// the RGB-only multiview sharing seam: one set of camera/intersection/
			/// shadow/continuation rays feeds both Direct and Indirect panes.
			/// The caller must configure this integrator as indirect-only.
			RISEPel IntegrateRayDirectIndirect(
				const RuntimeContext& rc,
				const RasterizerState& rast,
				const Ray& cameraRay,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				const IRadianceMap* pRadianceMap,
				RISEPel& direct,
				PixelAOV* pAOV,
				bool* pPrimaryHit = 0
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
				PixelAOV* pAOV = 0,
				//! Incoming vertex's MIS-partner pdf (< 0 means same as bsdfPdf_;
				//! differs under volume guiding via RAY_STATE::MisPartnerPdf()).
				Scalar bsdfMisPdf_ = -1,
				//! Cast-level RR compensation from RayCaster::CastRay re-entry;
				//! scales first vertex's NEE training integrand only.
				Scalar castRRCompensation_ = 1,
				//! Self-excluded emitter object at previous vertex (full weight on return hit).
				const IObject* pNEESelfObject_ = nullptr
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
				PixelAOV* pAOV = 0,
				Scalar bsdfMisPdf_ = -1,		///< See IntegrateFromHit
				Scalar castRRCompensation_ = 1,	///< See IntegrateFromHit
				bool smsChainUncovered_ = false,	///< HWSS hand-off: chain crossed an SMS-uncovered delta vertex
				const SMSChainRecord* pSMSChain_ = nullptr,	///< HWSS hand-off: recorded SMS anchor + delta chain
				bool smsIgnoreExtended_ = false,	///< Internal HWSS hand-off provenance mode
				const IObject* pNEESelfObject_ = nullptr	///< See IntegrateFromHit
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
				PixelAOV* pAOV = 0,
				Scalar bsdfMisPdf_ = -1,		///< See IntegrateFromHit
				Scalar castRRCompensation_ = 1,	///< Forwarded to firstHit hero-bundle NEE training only
				const IORStack* preseededOriginLanes_ = 0,
				const IObject* pNEESelfObject_ = nullptr	///< See IntegrateFromHit
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
				PixelAOV* pAOV = 0,
				bool* pPrimaryHit = 0
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
				PixelAOV* pAOV = 0,
				bool* pPrimaryHit = 0
				) const;

			/// Accessors for NM/HWSS code in PathTracingShaderOp
			bool GetSMSEnabled() const { return bSMSEnabled; }
			ManifoldSolver* GetSolver() const { return pSolver; }

		private:
			// Shared Pel/NM templates on SpectralDispatch tags (PelTag / NMTag),
			// instantiated in PathTracingIntegrator.cpp.  HWSS stays standalone
			// (see SpectralValueTraits.h).

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
				typename SpectralDispatch::SpectralValueTraits<Tag>::value_type* pDirectResult,
				const Tag& tag,
				Scalar bsdfMisPdf_ = -1,
				Scalar castRRCompensation_ = 1,
				bool smsChainUncovered_ = false,
				const SMSChainRecord* pSMSChain_ = nullptr,
				bool smsIgnoreExtended_ = false,
				const IObject* pNEESelfObject_ = nullptr
				) const;

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
				typename SpectralDispatch::SpectralValueTraits<Tag>::value_type* pDirectResult,
				const Tag& tag,
				bool* pPrimaryHit = 0
				) const;

			/// Tag-dispatched delegation from IntegrateRayTemplated (camera-ray entry: SMS flags false).
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
				const Tag& tag,
				Scalar bsdfMisPdf = -1
				) const;
		};
	}
}

#endif
