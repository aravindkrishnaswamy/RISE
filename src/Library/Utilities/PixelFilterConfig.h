//////////////////////////////////////////////////////////////////////
//
//  PixelFilterConfig.h - Per-rasterizer pixel sampling and
//    reconstruction filter configuration.
//
//  Production defaults: Owen-scrambled Sobol + Z-order blue-noise
//  shuffle + Gaussian (size 2.0, sigma 0.35 — strictly non-negative to
//  avoid dark halos around MC fireflies).  Set filter = "none" for
//  unfiltered point splats.
//
//  paramA / paramB by filter:
//    gaussian: size (px), sigma | mitchell-netravali: B, C (1/3, 1/3)
//    max: centre, edge weights  | kaiser: beta (paramB unused)
//    others: unused (width/height set support)
//  pixelSamplerParam: jitter half-range for nrooks/stratified (default
//    1.0), min separation for poisson; unused by other samplers.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 22, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PIXEL_FILTER_CONFIG_
#define PIXEL_FILTER_CONFIG_

#include "../Interfaces/IReference.h"
#include "RString.h"

namespace RISE
{
	struct PixelFilterConfig
	{
		//
		// Sample placement
		//

		String			pixelSampler;		///< Sampler: "sobol" (default), "halton", "multijittered", "nrooks", "stratified", "poisson", "random", "uniform"
		Scalar			pixelSamplerParam;	///< Sampler param (jitter for nrooks/stratified, min-sep for poisson; unused otherwise)
		bool			blueNoiseSampler;	///< Z-order blue-noise screen-space shuffle (default true; matches rasterizer useZSobol)

		//
		// Reconstruction filter
		//

		String			filter;			///< Filter: "gaussian" (default), "mitchell-netravali", "lanczos", "box", "tent", "sinc", "windowed_sinc_*", "catmull-rom", "cubic_bspline", "quadratic_bspline", "cook", "max", "kaiser", or "none"
		Scalar			width;			///< Filter width in pixels (default 1.0; unused by gaussian and mitchell-netravali)
		Scalar			height;			///< Filter height in pixels (default 1.0; unused by gaussian and mitchell-netravali)
		Scalar			paramA;			///< Filter param A (Gaussian size, Mitchell B, Kaiser beta, Max centre; default 2.0)
		Scalar			paramB;			///< Filter param B (Gaussian sigma, Mitchell C, Max edge; default 0.35)

		PixelFilterConfig() :
		  pixelSampler( "sobol" ),
		  pixelSamplerParam( 1.0 ),
		  blueNoiseSampler( true ),
		  filter( "gaussian" ),
		  width( 1.0 ),
		  height( 1.0 ),
		  paramA( 2.0 ),	// gaussian size = 2 pixels
		  paramB( 0.35 )	// gaussian sigma
		{
		}
	};
}

#endif
