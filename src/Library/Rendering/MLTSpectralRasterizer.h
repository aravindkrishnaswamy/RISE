//////////////////////////////////////////////////////////////////////
//
//  MLTSpectralRasterizer.h - Spectral Metropolis Light Transport
//    rasterizer using PSSMLT (Kelemen et al. 2002) with wavelength
//    integration.
//
//    CONTEXT:
//    This is the spectral extension of MLTRasterizer.  Where MLT-
//    Rasterizer uses BDPTIntegrator's RGB methods, this class uses
//    the NM (spectral) variants.  Each PSSMLT sample evaluates BDPT
//    at one or more wavelengths and converts to CIE XYZ for splatting.
//
//    When HWSS (Hero Wavelength Spectral Sampling) is enabled, each
//    sample evaluates 4 equidistantly-stratified wavelengths via
//    SampledWavelengths, giving ~4x spectral efficiency.  Each
//    wavelength runs a full independent BDPT NM evaluation sharing
//    the same sampler state (equidistant stratification without path
//    sharing — the simpler, robust variant).
//
//    SPECTRAL MLT DESIGN:
//    The wavelength is part of the primary sample space — mutations
//    to the wavelength coordinate are handled naturally by the
//    PSSMLTSampler.  For HWSS, the hero wavelength is mutated and
//    companions are deterministically derived.
//
//    Luminance for the Metropolis acceptance ratio is computed as
//    the Y component of the accumulated CIE XYZ value across all
//    strategies and all active wavelengths.
//
//    Splatting: per-strategy contributions are converted to XYZ
//    (stored as RISEPel(X,Y,Z)) in the SplatFilm.  The rasterizer
//    output chain handles the final XYZ → working-space conversion.
//
//    ARCHITECTURE:
//    Inherits the round-based progressive rendering structure from
//    MLTRasterizer.  The only changes are in EvaluateSample (now
//    spectral) and the per-splat color representation (XYZ instead
//    of RGB).
//
//    REFERENCES:
//    - Wilkie et al. "Hero Wavelength Spectral Sampling."
//      Eurographics Symposium on Rendering 2014.
//    - Kelemen et al. "A Simple and Robust Mutation Strategy for
//      the Metropolis Light Transport Algorithm." CGF 2002.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 9, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MLT_SPECTRAL_RASTERIZER_
#define MLT_SPECTRAL_RASTERIZER_

#include "MLTRasterizer.h"
#include "../Utilities/Color/SampledWavelengths.h"

namespace RISE
{
	namespace Implementation
	{
		class MLTSpectralRasterizer : public MLTRasterizer
		{
		protected:

			Scalar				lambda_begin;		///< Start of spectral range (nm)
			Scalar				lambda_end;			///< End of spectral range (nm)
			unsigned int		nSpectralSamples;	///< Spectral samples per MLT evaluation
			bool				bUseHWSS;			///< Use Hero Wavelength Spectral Sampling

			virtual ~MLTSpectralRasterizer();

			/// Spectral version of EvaluateSample.  Samples one or more
			/// wavelengths (HWSS or individual), evaluates BDPT NM at each,
			/// converts to XYZ, and returns aggregated per-strategy splats
			/// with XYZ colors stored as RISEPel(X,Y,Z).
			MLTSample EvaluateSampleSpectral(
				const IScene& scene,
				const ICamera& camera,
				ISampler& sampler,
				const unsigned int width,
				const unsigned int height
				) const;

			/// Run a fixed number of spectral mutations on an existing chain.
			/// Mirrors RunChainSegment but calls EvaluateSampleSpectral
			/// instead of EvaluateSample.
			void RunChainSegmentSpectral(
				ChainState& state,
				const IScene& scene,
				const ICamera& camera,
				SplatFilm& splatFilm,
				const unsigned int numMutations,
				const Scalar normalization,
				const unsigned int width,
				const unsigned int height
				) const;

			/// Thread dispatch data for parallel spectral chain execution.
			struct SpectralRoundThreadData
			{
				const MLTSpectralRasterizer*	pRasterizer;
				const IScene*				pScene;
				SplatFilm*					pSplatFilm;
				ChainState*					pChainStates;
				unsigned int				chainStart;
				unsigned int				chainEnd;
				unsigned int				mutationsPerChain;
				Scalar						normalization;
				unsigned int				width;
				unsigned int				height;
			};

			/// Thread procedure for parallel spectral chain execution
			static void* SpectralRoundThread_ThreadProc( void* lpParameter );

			/// Evaluate BDPT at a single wavelength.  Returns per-strategy
			/// scalar contributions with pixel positions.
			void EvaluateSingleWavelength(
				const IScene& scene,
				const ICamera& camera,
				ISampler& sampler,
				const unsigned int width,
				const unsigned int height,
				const Ray& cameraRay,
				const Point2& screenPos,
				const Point2& cameraRasterPos,
				const Scalar nm,
				const RuntimeContext& rc,
				std::vector<BDPTIntegrator::ConnectionResultNM>& results,
				std::vector<BDPTVertex>& lightVerts,
				std::vector<BDPTVertex>& eyeVerts
				) const;

		public:
			MLTSpectralRasterizer(
				IRayCaster* pCaster_,
				const unsigned int maxEyeDepth,
				const unsigned int maxLightDepth,
				const unsigned int nBootstrap_,
				const unsigned int nChains_,
				const unsigned int nMutationsPerPixel_,
				const Scalar largeStepProb_,
				const Scalar lambda_begin_,
				const Scalar lambda_end_,
				const unsigned int nSpectralSamples_,
				const bool useHWSS_
				);

			//////////////////////////////////////////////////////////////
			// IRasterizer interface — overrides RasterizeScene to use
			// the spectral evaluation path
			//////////////////////////////////////////////////////////////

			void RasterizeScene(
				const IScene& pScene,
				const Rect* pRect,
				IRasterizeSequence* pRasterSequence
				) const;
		};
	}
}

#endif
