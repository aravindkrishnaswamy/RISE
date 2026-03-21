//////////////////////////////////////////////////////////////////////
//
//  BDPTRasterizer.h - Pixel-based rasterizer for Bidirectional
//    Path Tracing.  Extends PixelBasedPelRasterizer to add light
//    subpath generation, all (s,t) connection strategies, MIS
//    weighting, and thread-safe film splatting for s<=1 strategies.
//
//    Supports both RGB and spectral (per-wavelength) rendering modes.
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
