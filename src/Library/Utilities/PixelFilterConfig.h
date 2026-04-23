//////////////////////////////////////////////////////////////////////
//
//  PixelFilterConfig.h - Configuration for the per-rasterizer pixel
//    sampling strategy and reconstruction filter.
//
//    Every pixel-based rasterizer (PT, BDPT, VCM, MLT, MMLT, legacy
//    PixelBased/SpectralIntegrating) needs the same knobs to build
//    its sample generator and reconstruction kernel.  This struct
//    bundles them so the parser and IJob signatures do not have to
//    thread them through every overload.
//
//    The default-constructed configuration is intended to be a
//    solid production preset:
//      - `pixelSampler = "sobol"` — Owen-scrambled Sobol (0,2)-net,
//        low-discrepancy and progressive.  Works well for every
//        sample-count budget without the lattice artifacts of
//        stratified / N-rooks at small N.
//      - `blueNoiseSampler = true` — Z-order screen-space shuffle
//        applied on top of the chosen sampler, which pushes aliasing
//        into blue-noise frequency bands the eye tolerates better.
//      - `filter = "gaussian"` with size 2.0 and sigma 0.35 — strictly
//        non-negative reconstruction.  Monte-Carlo renders routinely
//        produce outlier (firefly) samples; a Mitchell-Netravali /
//        Lanczos default with negative side lobes would spread those
//        outliers as dark halos into neighbouring pixels (clamped to
//        black in LDR output), which is uglier than the small amount
//        of softness Gaussian introduces.  Matches PBRT v4 / Mitsuba
//        / Cycles / Arnold defaults.  Scenes that want sharper
//        reconstruction can opt into `mitchell-netravali`, `lanczos`,
//        or `catmull-rom` explicitly, remembering to also set
//        paramA/paramB appropriate to that filter.
//
//    `paramA` / `paramB` are interpreted per filter:
//      - `gaussian`          : paramA = kernel support size (pixels),
//                              paramB = sigma (stddev).
//      - `mitchell-netravali`: paramA = B, paramB = C (1/3, 1/3 is
//                              the Mitchell–Netravali paper's
//                              recommendation).
//      - `max`               : paramA = centre weight,
//                              paramB = edge weight.
//      - `kaiser`            : paramA = beta, paramB unused.
//      - everything else     : both unused; width/height drive the
//                              kernel extent.
//
//    `pixelSamplerParam` is interpreted per sampler:
//      - `nrooks`     : jitter half-range within each stratum
//                       (1.0 = full-stratum jitter, 0 = centre only).
//      - `stratified` : same as nrooks.
//      - `poisson`    : minimum separation between any two samples.
//      - `sobol`, `halton`, `multijittered`, `uniform`, `random`:
//                       unused.  Default 1.0 is a no-op for these
//                       and a sensible value if the sampler is
//                       later switched to nrooks/stratified.
//
//    Scenes that want unfiltered point splats (the old pre-filter
//    behaviour) can set `filter = "none"`.
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

		String			pixelSampler;		///< Sampler name: "sobol" (default), "halton", "multijittered", "nrooks", "stratified", "poisson", "random", "uniform"
		Scalar			pixelSamplerParam;	///< Sampler-specific parameter (jitter range for nrooks/stratified, min-separation for poisson, unused for sobol/halton/multijittered/uniform/random)
		bool			blueNoiseSampler;	///< Apply Z-order blue-noise screen-space shuffle on top of pixelSampler (true = enabled, matches the `useZSobol` flag inside the rasterizer)

		//
		// Reconstruction filter
		//

		String			filter;			///< Filter name: "gaussian" (default), "mitchell-netravali", "lanczos", "box", "tent", "sinc", "windowed_sinc_*", "catmull-rom", "cubic_bspline", "quadratic_bspline", "cook", "max", "kaiser", or "none" for unfiltered point splats
		Scalar			width;			///< Filter width in pixels (default 1.0).  Unused by "gaussian" (paramA is size) and by "mitchell-netravali" (fixed 2-pixel support).
		Scalar			height;			///< Filter height in pixels (default 1.0).  Unused by "gaussian" / "mitchell-netravali" — see width.
		Scalar			paramA;			///< First filter-specific parameter (Gaussian size, Mitchell B, Kaiser beta, Max centre weight, ...).  Default 2.0 = Gaussian 2-pixel support.
		Scalar			paramB;			///< Second filter-specific parameter (Gaussian sigma, Mitchell C, Max edge weight, ...).  Default 0.35 = Gaussian stddev.

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
