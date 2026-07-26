//////////////////////////////////////////////////////////////////////
//
//  PathTracingPelRasterizer.h - Pure path tracing rasterizer (RGB).
//
//    Bypasses the shader op pipeline entirely.  Uses
//    PathTracingIntegrator for direct iterative path tracing,
//    inheriting the standard pixel-based sample loop from
//    PixelBasedPelRasterizer.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 10, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PATHTRACING_PEL_RASTERIZER_
#define PATHTRACING_PEL_RASTERIZER_

#include "PixelBasedPelRasterizer.h"
#include "AOVBuffers.h"
#include "../Utilities/AdaptiveSamplingConfig.h"
#include "../Utilities/StabilityConfig.h"
#include "../Utilities/ManifoldSolver.h"
#include "../Utilities/SMSPhotonMap.h"
#include <atomic>
#include <stdint.h>

namespace RISE
{
	namespace Implementation
	{
		class PathTracingIntegrator;

		class PathTracingPelRasterizer :
			public PixelBasedPelRasterizer
		{
		protected:
			virtual ~PathTracingPelRasterizer();

			PathTracingIntegrator*	pIntegrator;
			bool					mHasVariantTransportConfig;
			unsigned int			mVariantMaxPathDepth;
			bool					mVariantIndirectOnly;
			bool					mVariantClayOverride;
			std::atomic<bool>		mInteractiveDenoiseSuppressed;

			/// SMS photon-aided seeding store.  See BDPTRasterizerBase for
			/// the full rationale.  Null when smsConfig.photonCount was 0.
			mutable SMSPhotonMap*	pSMSPhotonMap;
			unsigned int			mSMSPhotonCount;

			/// Progressive rendering should run to adaptive_max_samples
			/// when adaptive sampling is enabled.
			unsigned int GetProgressiveTotalSPP() const override;

#ifdef RISE_ENABLE_OIDN
			bool ShouldDenoise() const override;
#endif

			void IntegratePixel(
				const RuntimeContext& rc,
				const unsigned int x,
				const unsigned int y,
				const unsigned int height,
				const IScene& pScene,
				RISEColor& cret,
				const bool temporal_samples,
				const Scalar temporal_start,
				const Scalar temporal_exposure
				) const override;

			/// Single-sample RGB integration using the iterative integrator.
			/// Optionally populates first-hit AOV data for the denoiser.
			RISEPel IntegratePixelRGB(
				const RuntimeContext& rc,
				const RasterizerState& rast,
				const Point2& ptOnScreen,
				const IScene& pScene,
				ISampler& sampler,
				const IRadianceMap* pRadianceMap,
				PixelAOV* pAOV
				) const;

		public:
			// Deferred photon-map gate (IRasterizer): own light transport, never
			// reads the scene photon maps -> false (overrides the inherited
			// PixelBasedPelRasterizer::true).
			bool ConsumesScenePhotonMaps() const override { return false; }

			PathTracingPelRasterizer(
				IRayCaster* pCaster_,
				const ManifoldSolverConfig& smsConfig,
				const PathGuidingConfig& guidingConfig,
				const AdaptiveSamplingConfig& adaptiveConfig,
				const StabilityConfig& stabilityConfig,
				bool useZSobol_,
				RISE::Implementation::FrameStore* frameStore = nullptr
				);

			/// GUI render modes P2a fix (docs/gui/RENDER_MODES.md §6):
			/// post-construction setter forwarding to
			/// PathTracingIntegrator::SetMaxPathDepth -- see that method's doc
			/// for the exact depth-accounting mapping ("direct" == 1 means
			/// camera-hit + NEE only, no continuation bounce traced).  n==0
			/// maps to the historical default (128).  No render-in-flight
			/// synchronization here -- deliberately NOT part of the C-ABI
			/// factory signature (see RISE_API.h's abi-preserving-api-evolution
			/// discipline); CreateBeautyVariantPipeline is the only caller
			/// today, and it calls this immediately after construction, before
			/// the pipeline is ever handed to the render loop.
			void SetMaxPathDepth( unsigned int n );

			/// GUI render modes P2b (docs/gui/RENDER_MODES.md §3 Lighting):
			/// post-construction setters forwarding to PathTracingIntegrator::
			/// SetIndirectOnly / SetClayOverride -- see those methods' doc for
			/// the exact semantics.  Same discipline as SetMaxPathDepth above:
			/// no render-in-flight synchronization, not part of the C-ABI
			/// factory signature, called immediately after construction by
			/// CreateBeautyVariantPipeline before the pipeline is ever handed
			/// to the render loop.
			void SetIndirectOnly( bool b );
			void SetClayOverride( bool b );

			//! Atomically suppresses end-of-pass OIDN for a BeautyVariant pass
			//! that became obsolete when a new interaction began.  This is safe
			//! to set while RasterizeScene is winding down after cancellation.
			void SetInteractiveDenoiseSuppressed( bool suppressed )
			{
				mInteractiveDenoiseSuppressed.store(
					suppressed, std::memory_order_release );
			}
			bool ForTest_IsInteractiveDenoiseSuppressed() const
			{
				return mInteractiveDenoiseSuppressed.load(
					std::memory_order_acquire );
			}

			void PrepareRuntimeContext( RuntimeContext& rc ) const override;

			/// Runs the one-time SMS photon-aided seeding pass when
			/// smsConfig.photonCount > 0.  Inherited from
			/// PixelBasedRasterizerHelper; invoked by the base
			/// RasterizeScene before the per-pixel dispatch.
			void PreRenderSetup(
				const IScene& pScene,
				const Rect* pRect
				) const override;
		};
	}
}

#endif
