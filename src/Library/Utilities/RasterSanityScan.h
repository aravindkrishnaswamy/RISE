//////////////////////////////////////////////////////////////////////
//
//  RasterSanityScan.h - Pre-file-write pathological-pixel detector.
//
//    Walks an IRasterImage counting pixels whose channel values are
//    negative.  Used by FileRasterizerOutput to emit a warning before
//    writing, so downstream format conversions (PNG clamps to [0,255]
//    → negatives become black pixels) don't silently mask upstream
//    bugs or filter-lobe artifacts.
//
//    Semantics: per-pixel counting.  A pixel with two negative
//    channels contributes 1 to negativeCount (not 2).  maxNegative-
//    Magnitude tracks the worst -v across any channel of any pixel.
//    Alpha is ignored.
//
//    NaN / Inf are intentionally NOT detected here — the RISE build
//    enables -ffast-math on OSX / Linux, which makes `std::isnan` /
//    `std::isinf` unreliable (the compiler assumes they never occur).
//    If those are observed at runtime they typically propagate as
//    gross-scale corruption a user notices immediately; the sanity
//    scan focuses on the subtle case (small negatives) that quietly
//    clamps to black at PNG write time.
//
//    Cost: one GetPEL call per pixel, O(W*H).  Negligible compared
//    to render time — runs once per image output, never inside the
//    integrator hot path.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RASTER_SANITY_SCAN_
#define RASTER_SANITY_SCAN_

#include "../Interfaces/IRasterImage.h"

namespace RISE
{
	struct RasterSanityReport
	{
		unsigned int negativeCount        = 0;
		Scalar       maxNegativeMagnitude = 0;
	};

	/// Scan the image for pixels whose color channels (alpha ignored)
	/// contain a negative value.  A pixel with any negative channel
	/// contributes 1 to negativeCount; maxNegativeMagnitude is the
	/// largest -v observed across all channels of all pixels.
	inline RasterSanityReport ScanRasterImageForPathologicalPixels(
		const IRasterImage& image )
	{
		RasterSanityReport r;
		const unsigned int w = image.GetWidth();
		const unsigned int h = image.GetHeight();
		for( unsigned int y = 0; y < h; y++ ) {
			for( unsigned int x = 0; x < w; x++ ) {
				const RISEColor c = image.GetPEL( x, y );
				bool pixelHasNeg = false;
				for( int ch = 0; ch < 3; ch++ ) {
					const Scalar v = c.base[ch];
					if( v < 0 ) {
						pixelHasNeg = true;
						const Scalar mag = -v;
						if( mag > r.maxNegativeMagnitude ) {
							r.maxNegativeMagnitude = mag;
						}
					}
				}
				if( pixelHasNeg ) r.negativeCount++;
			}
		}
		return r;
	}
}

#endif
