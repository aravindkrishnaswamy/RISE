//////////////////////////////////////////////////////////////////////
//
//  RGBToSpectrumTable_ROMMData.h - Declarations for the baked-in
//    Jakob-Hanika sigmoid LUT data.  Definitions live in the
//    generated RGBToSpectrumTable_ROMMData.cpp; only RGBToSpectrumTable
//    and the generator script reference these symbols.
//
//  See RGBToSpectrumTable_ROMMData.cpp's header comment for the
//  rationale (avoid runtime fopen of extlib/jakob-hanika-luts/
//  romm.coeff so spectral painters work even when launched from a
//  shortcut without RISE_MEDIA_PATH set).
//
//////////////////////////////////////////////////////////////////////

#ifndef RGB_TO_SPECTRUM_TABLE_ROMM_DATA_
#define RGB_TO_SPECTRUM_TABLE_ROMM_DATA_

namespace RISE
{
	// Resolution of the LUT cube along each axis (one slice per max-
	// channel ∈ {R, G, B}, so total cells = 3 * res^3).  Must match
	// what the JH generator produced; the runtime `Load()` uses this
	// to size its `data` vector.
	extern const unsigned int kROMMDataResolution;

	// Total number of floats in `kROMMDataFloats` (= 3 * res^3 * 3,
	// where the trailing 3 is the per-cell `(c0, c1, c2)` sigmoid
	// coefficient triple).
	extern const unsigned int kROMMDataNumFloats;

	// The baked LUT body: 3 * res^3 cells of `(c0, c1, c2)` floats,
	// laid out so that index = ((maxC * res + iz) * res + ix) * res + iy
	// addresses cell (maxC, iz, ix, iy).  Matches the on-disk
	// `romm.coeff` body byte-for-byte (we drop the on-disk magic +
	// header at bake time since the runtime no longer needs them).
	extern const float kROMMDataFloats[];
}

#endif
