//////////////////////////////////////////////////////////////////////
//
//  RGBToSpectrumTable_LUTData.h - Declarations for the baked-in
//    Jakob-Hanika sigmoid LUT data.  Definitions live in the
//    generated RGBToSpectrumTable_LUTData.cpp; only RGBToSpectrumTable
//    and the generator script reference these symbols.
//
//  The data layout is target-colour-space agnostic; which target the
//  baked floats describe is determined at LUT-bake time by the
//  `--target` flag passed to tools/JakobHanikaLUTGen.cpp +
//  tools/GenerateSpectrumLUTHeader.py.  The runtime
//  RGBToSpectrumTable converts its input `RISEPel` into the LUT's
//  target colour space at the call boundary — collapses to identity
//  when RISEPel matches the LUT target.
//
//  See RGBToSpectrumTable_LUTData.cpp's header comment for the
//  rationale (avoid runtime fopen of extlib/jakob-hanika-luts/
//  *.coeff so spectral painters work even when launched from a
//  shortcut without RISE_MEDIA_PATH set).
//
//////////////////////////////////////////////////////////////////////

#ifndef RGB_TO_SPECTRUM_TABLE_LUT_DATA_
#define RGB_TO_SPECTRUM_TABLE_LUT_DATA_

namespace RISE
{
	// Resolution of the LUT cube along each axis (one slice per max-
	// channel ∈ {R, G, B}, so total cells = 3 * res^3).  Must match
	// what the JH generator produced; the runtime `Load()` uses this
	// to size its `data` vector.
	extern const unsigned int kSpectrumLUTResolution;

	// Total number of floats in `kSpectrumLUTFloats` (= 3 * res^3 * 3,
	// where the trailing 3 is the per-cell `(c0, c1, c2)` sigmoid
	// coefficient triple).
	extern const unsigned int kSpectrumLUTNumFloats;

	// The baked LUT body: 3 * res^3 cells of `(c0, c1, c2)` floats,
	// laid out so that index = ((maxC * res + iz) * res + ix) * res + iy
	// addresses cell (maxC, iz, ix, iy).  Matches the on-disk
	// `*.coeff` body byte-for-byte (we drop the on-disk magic +
	// header at bake time since the runtime no longer needs them).
	extern const float kSpectrumLUTFloats[];
}

#endif
