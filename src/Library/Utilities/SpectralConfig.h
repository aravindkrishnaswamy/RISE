//////////////////////////////////////////////////////////////////////
//
//  SpectralConfig.h - Parser-facing configuration for spectral
//    rasterization (shared across all spectral rasterizers).
//
//  Defaults match legacy `pixelintegratingspectral_rasterizer`:
//  [380, 780] nm, 10 discrete bins (ignored by continuous MLT spectral;
//  binned PT/BDPT/VCM scenes typically use 80), 1 spectral sample per
//  pixel (override to 4-16 for multi-wavelength paths), and HWSS off
//  (opt-in via `hwss true` where BSDFs are HWSS-validated).
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
		Scalar			nmBegin;			///< Integration range lower bound in nm (default 380)
		Scalar			nmEnd;				///< Integration range upper bound in nm (default 780)
		unsigned int	numWavelengths;		///< Discrete bins across [nmBegin, nmEnd] (default 10; ignored by MLT spectral)
		unsigned int	spectralSamples;	///< Spectral samples per pixel per pass (default 1)
		bool			useHWSS;			///< Hero Wavelength Spectral Sampling (Wilkie et al. 2014; default false, opt-in)

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
