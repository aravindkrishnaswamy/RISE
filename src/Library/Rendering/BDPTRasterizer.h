//////////////////////////////////////////////////////////////////////
//
//  BDPTRasterizer.h - Pixel-based rasterizer for Bidirectional
//    Path Tracing.
//
//    ARCHITECTURE:
//    Extends PixelBasedPelRasterizer so it inherits the existing
//    multi-threaded block-based rendering pipeline.  The only
//    override is IntegratePixel(), which generates both light and
//    eye subpaths via BDPTIntegrator, evaluates all (s,t) strategies,
//    and accumulates contributions.
//
//    SPLAT FILM:
//    Strategies where t<=1 (light path connects to camera) produce
//    contributions at arbitrary pixel positions, not the current
//    pixel being rendered.  These are accumulated into a SplatFilm
//    with row-level mutex locking for thread safety.  After the
//    main render pass completes, the splat film is resolved into
//    the primary image by dividing by the total sample count.
//
//    SPECTRAL MODE:
//    When enabled, each pixel sample generates multiple wavelength
//    samples.  Each wavelength runs a complete BDPT evaluation
//    (GenerateLightSubpathNM / GenerateEyeSubpathNM) and the
//    scalar results are converted to XYZ via color matching
//    functions, then accumulated.  Splats in spectral mode are
//    also XYZ-converted before being added to the SplatFilm.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BDPT_RASTERIZER_
#define BDPT_RASTERIZER_

#include "PixelBasedPelRasterizer.h"
#include "SplatFilm.h"
#include "../Shaders/BDPTIntegrator.h"

namespace RISE
{
	namespace Implementation
	{
		class BDPTRasterizer : public PixelBasedPelRasterizer
		{
		protected:
			BDPTIntegrator*			pIntegrator;
			mutable SplatFilm*		pSplatFilm;

			// Spectral mode parameters
			bool					bSpectralMode;
			Scalar					lambda_begin;
			Scalar					lambda_end;
			unsigned int			num_wavelengths;
			unsigned int			nSpectralSamples;

			virtual ~BDPTRasterizer();

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

			/// Spectral integration for a single pixel sample at a given wavelength
			Scalar IntegratePixelNM(
				const RuntimeContext& rc,
				const Point2& ptOnScreen,
				const IScene& pScene,
				const ICamera& camera,
				const Scalar nm
				) const;

		public:
			BDPTRasterizer(
				IRayCaster* pCaster_,
				unsigned int maxEyeDepth,
				unsigned int maxLightDepth
				);

			/// Enable spectral rendering mode
			void SetSpectralMode(
				const Scalar lambdaBegin,
				const Scalar lambdaEnd,
				const unsigned int numWavelengths,
				const unsigned int spectralSamples
				);

			void RasterizeScene(
				const IScene& pScene,
				const Rect* pRect,
				IRasterizeSequence* pRasterSequence
				) const;
		};
	}
}

#endif
