//////////////////////////////////////////////////////////////////////
//
//  BidirectionalRasterizerBase.h - Shared infrastructure for
//    bidirectional-style pixel rasterizers (BDPT and VCM).
//
//    Both BDPTRasterizerBase and VCMRasterizerBase previously
//    duplicated the same splat-film plumbing: a SplatFilm for
//    t==1 light-to-camera contributions, a scratch image used to
//    composite splats for intermediate previews and final output,
//    a total-sample counter, and the adaptive-sample tracker.
//
//    This class owns those fields and the related helpers so the
//    two algorithm-specific bases can focus on their own concerns
//    (BDPT: path guiding, SMS, RasterizeScene override; VCM: light
//    pass, merge radius, LightVertexStore).
//
//    Inheritance layout preserves the diamond:
//
//        PixelBasedRasterizerHelper   (virtual base)
//              |
//        BidirectionalRasterizerBase  (virtual inheritance)
//              |
//        BDPTRasterizerBase / VCMRasterizerBase
//              |
//        Pel / Spectral subclasses (join via PixelBasedPelRasterizer
//        or PixelBasedSpectralIntegratingRasterizer, which also
//        virtually inherit PixelBasedRasterizerHelper).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BIDIRECTIONAL_RASTERIZER_BASE_
#define BIDIRECTIONAL_RASTERIZER_BASE_

#include "PixelBasedRasterizerHelper.h"
#include "SplatFilm.h"
#include "../Utilities/StabilityConfig.h"
#include <atomic>
#include <cstdint>

namespace RISE
{
	namespace Implementation
	{
		class BidirectionalRasterizerBase : public virtual PixelBasedRasterizerHelper
		{
		protected:
			mutable SplatFilm*				pSplatFilm;
			mutable IRasterImage*			pScratchImage;		///< Scratch buffer for splat composition
			mutable Scalar					mSplatTotalSamples;	///< Cached for progressive resolve
			mutable std::atomic<uint64_t>	mTotalAdaptiveSamples;	///< Total camera samples across all pixels
			StabilityConfig					stabilityConfig;	///< Production stability controls

			BidirectionalRasterizerBase(
				IRayCaster* pCaster_,
				const StabilityConfig& stabilityCfg
				);

			virtual ~BidirectionalRasterizerBase();

			/// Scaling factor for splat film resolution.  Pel returns 1;
			/// Spectral returns nSpectralSamples [ * SampledWavelengths::N
			/// when HWSS is enabled ].
			virtual Scalar GetSplatSampleScale() const { return 1.0; }

			/// Progress title shown in the CLI during render.
			virtual const char* GetProgressTitle() const = 0;

			/// Splat a t==1 contribution at a FRACTIONAL raster position,
			/// spread across the pixel filter's footprint so caustic /
			/// light-to-camera splats reconstruct with the same
			/// Mitchell-Netravali / Lanczos / box kernel the rest of
			/// the integrator uses.  Falls back to a round-to-nearest
			/// point splat when no filter is configured.
			///
			/// `fx`, `fy` are in image-buffer coordinates (y=0 at top)
			/// — callers convert from `cr.rasterPos` by flipping y with
			/// `camera.GetHeight() - cr.rasterPos.y`.
			void SplatContributionToFilm(
				const Scalar fx,
				const Scalar fy,
				const RISEPel& contribution,
				const unsigned int imageWidth,
				const unsigned int imageHeight
				) const;

			/// Returns a scratch image with resolved splats composited
			/// on top of the current primary, for progressive display.
			/// Scratch buffer is lazily allocated on first call.
			IRasterImage& GetIntermediateOutputImage( IRasterImage& primary ) const;

			/// Copy `src` into the scratch buffer and resolve the splat
			/// film on top.  Shared body for the Flush* overrides that
			/// both algorithms use.  Caller must verify `pSplatFilm` is
			/// non-null before calling.
			IRasterImage& ResolveSplatIntoScratch( const IRasterImage& src ) const;

		public:
			/// Thread-safe: accumulates camera-sample count for
			/// adaptive splat resolution.
			void AddAdaptiveSamples( uint64_t count ) const;

			/// Effective SPP the splat film should divide by at resolve
			/// time, honouring adaptive sampling if any samples were
			/// added via AddAdaptiveSamples.
			Scalar GetEffectiveSplatSPP( unsigned int width, unsigned int height ) const;
		};
	}
}

#endif
