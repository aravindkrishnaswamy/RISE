//////////////////////////////////////////////////////////////////////
//
//  SpectralConfig.h - Parser-facing configuration for spectral
//    rasterization.
//
//    Every spectral rasterizer (the legacy PixelBasedSpectralIntegrating
//    path plus the modern PT / BDPT / VCM / MLT spectral variants)
//    needs to agree on the wavelength range it is integrating over,
//    how many wavelength bins / samples to use, and whether Hero
//    Wavelength Spectral Sampling is enabled.  This struct bundles
//    those knobs so the parser and IJob signatures do not have to
//    thread them through every overload.
//
//    Default-constructed state is deliberately conservative, matching
//    the legacy `pixelintegratingspectral_rasterizer` behaviour so
//    existing scenes that rely on defaults continue to render at the
//    same cost and the same quality as before:
//      - `nmBegin = 380` / `nmEnd = 780` — visible spectrum plus a
//        few nanometers of UV/IR tail.  Wider than the 400-700 range
//        used by some older tests; captures phenomena that only show
//        up at the ends (deep reds in skin subsurface scattering,
//        violet ends of prism caustics).
//      - `numWavelengths = 10` — 40 nm per bin across the 400 nm
//        range.  Coarse but fast; adequate for stochastic wavelength
//        selection (one nm drawn per pixel sample).  Scenes doing
//        binned spectral integration (BDPT/VCM/PT spectral) typically
//        override to 80 (5 nm per bin) for finer spectral resolution.
//        Ignored by MLT spectral, which uses continuous
//        hero-wavelength sampling rather than binned integration.
//      - `spectralSamples = 1` — one wavelength draw per pixel sample.
//        This is the canonical PixelBasedSpectralIntegrating default:
//        a single stochastic wavelength per ray, letting spatial
//        sample count drive spectral convergence.  BDPT/VCM/PT
//        spectral scenes typically override to 4-16 to carry
//        multiple wavelengths per path; setting this at the default
//        layer would 16x the render cost for every scene that relies
//        on defaults (including the entire cornellbox_spectral test
//        suite), so opt-in is the safer policy.
//      - `useHWSS = false` — Hero Wavelength Spectral Sampling
//        (Wilkie et al. 2014) is off by default because it requires
//        every BSDF in the scene to handle multiple concurrent
//        wavelengths correctly.  Scenes where HWSS has been validated
//        opt in per-chunk with `hwss true`; turning it on at the
//        default layer risks silent artifacts on materials that were
//        written for single-wavelength evaluation.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 22, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SPECTRAL_CONFIG_
#define SPECTRAL_CONFIG_

#include "../Interfaces/IReference.h"

namespace RISE
{
	struct SpectralConfig
	{
		Scalar			nmBegin;			///< Lower bound of the spectral integration range (nm).  Default 380.
		Scalar			nmEnd;				///< Upper bound of the spectral integration range (nm).  Default 780.
		unsigned int	numWavelengths;		///< Number of discrete wavelength bins across [nmBegin, nmEnd].  Default 10 (40 nm per bin) — conservative / fast.  Binned spectral integrators (BDPT/VCM/PT spectral) typically override to 80 for 5 nm resolution.  Ignored by MLT spectral (continuous-sampling).
		unsigned int	spectralSamples;	///< Spectral samples per pixel per pass.  Default 1 — single stochastic wavelength draw per pixel sample, matching PixelBasedSpectralIntegrating tradition.  Multi-wavelength-per-path integrators typically override to 4-16.
		bool			useHWSS;			///< Hero Wavelength Spectral Sampling (Wilkie et al. 2014).  Default false — opt-in per scene, requires HWSS-aware BSDFs.

		SpectralConfig() :
		  nmBegin( 380.0 ),
		  nmEnd( 780.0 ),
		  numWavelengths( 10 ),
		  spectralSamples( 1 ),
		  useHWSS( false )
		{
		}
	};
}

#endif
