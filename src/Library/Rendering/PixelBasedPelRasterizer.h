//////////////////////////////////////////////////////////////////////
//
//  PixelBasedPelRasterizer.h - A class that we can use for
//    pixel based non real time rasterizers.  Uses the core ray
//    casting engine for every pixel sample
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 23, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PIXEL_BASED_PEL_RASTERIZER_
#define PIXEL_BASED_PEL_RASTERIZER_

#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/IRasterImage.h"
#include "../Interfaces/IPixelFilter.h"
#include "../Utilities/PathGuidingField.h"
#include "../Utilities/AdaptiveSamplingConfig.h"
#include "../Utilities/StabilityConfig.h"
#include "PixelBasedRasterizerHelper.h"

#ifdef RISE_ENABLE_OPENPGL
namespace RISE { namespace Implementation { class PathGuidingField; } }
#endif
namespace RISE { namespace Implementation { class OptimalMISAccumulator; } }

namespace RISE
{
	namespace Implementation
	{
		class PixelBasedPelRasterizer : public virtual PixelBasedRasterizerHelper
		{
		protected:
			virtual ~PixelBasedPelRasterizer( );

			inline bool TakeSingleSample(
				const RuntimeContext& rc,
				const RasterizerState& rast,
				const Ray& ray,
				RISEPel& c
				) const
			{
				return pCaster->CastRay( rc, rast, ray, c, IRayCaster::RAY_STATE(), 0, 0 );
			}

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
				) const;

			unsigned int GetProgressiveTotalSPP() const;

			/// Adaptive-sample-map intent for ProgressiveFilm::Resolve.
			/// See PixelBasedRasterizerHelper.h docs and the
			/// "show_adaptive_map ineffective in progressive mode" fix
			/// landed 2026-05-24.
			bool GetAdaptiveShowMap() const { return adaptiveConfig.showMap && adaptiveConfig.maxSamples > 0; }
			unsigned int GetAdaptiveTargetSamples() const { return adaptiveConfig.maxSamples; }

#ifdef RISE_ENABLE_OPENPGL
			mutable PathGuidingField*	pGuidingField;
			mutable Scalar				guidingAlphaScale;
#endif
			mutable OptimalMISAccumulator*	pOptimalMISAccumulator;
			PathGuidingConfig			guidingConfig;
			AdaptiveSamplingConfig		adaptiveConfig;
			StabilityConfig				stabilityConfig;

		public:
			PixelBasedPelRasterizer(
				IRayCaster* pCaster_,
				const PathGuidingConfig& guidingCfg,
				const AdaptiveSamplingConfig& adaptiveCfg,
				const StabilityConfig& stabilityCfg,
				bool useZSobol_,
				RISE::Implementation::FrameStore* frameStore = nullptr
				);

			void PrepareRuntimeContext( RuntimeContext& rc ) const;
			void PreRenderSetup( const IScene& pScene, const Rect* pRect ) const;

			// Deferred photon-map gate (IRasterizer): this is the shaderop-graph
			// runner, whose shader may contain photon-map shaderops that consume the
			// scene's photon maps, so the deferred shoot must fire.  Integrator
			// subclasses (BDPTPel/VCMPel/PathTracingPel) override back to false.
			bool ConsumesScenePhotonMaps() const { return true; }
			void PostRenderCleanup() const;
		};
	}
}

#endif
