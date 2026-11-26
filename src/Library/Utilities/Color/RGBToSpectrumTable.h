//////////////////////////////////////////////////////////////////////
//
//  RGBToSpectrumTable.h - Provides tetrahedral-interpolated lookup
//    of Jakob-Hanika sigmoid coefficients (c0, c1, c2) for an input
//    RGB triple (in the LUT's target colour space — Rec.709 Linear
//    after the 2026-05 colour-space migration).
//
//    The LUT data is BAKED INTO THE BINARY via
//    RGBToSpectrumTable_LUTData.cpp (auto-generated from
//    extlib/jakob-hanika-luts/<target>.coeff by
//    tools/GenerateSpectrumLUTHeader.py).  The runtime no longer
//    reads any LUT file from disk — earlier revisions used fopen()
//    through the media-path locator, which silently failed when
//    the GUI was launched without RISE_MEDIA_PATH set, leaving
//    every spectral painter on the constant-0.5 fallback.
//
//    Lookup procedure:
//      1. Convert the input RISEPel into the LUT's target colour
//         space (collapses to identity when RISEPel matches the
//         target; otherwise one 3×3 matrix multiply).
//      2. Find the max channel of the converted rgb.
//      3. Normalize: z = max, x = mid_axis_value/z, y = min_axis_value/z.
//      4. Tetrahedrally interpolate the (c0, c1, c2) coefficients in
//         the corresponding sub-table at (z, x, y).
//      5. Return as RGBSigmoidPolynomial.
//
//    On-disk LUT format (still produced by tools/JakobHanikaLUTGen.cpp
//    so the generator script has something to consume):
//      char     magic[4]      = "RJHL"
//      uint32_t version       = 0x00010000
//      uint32_t resolution    (cells per axis, typically 64)
//      uint32_t numChannels   = 3 (R, G, B max-channel sub-tables)
//      uint32_t numCoeffs     = 3 (c0, c1, c2)
//      float    coeffs[ 3 · res³ · 3 ]
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RGB_TO_SPECTRUM_TABLE_
#define RGB_TO_SPECTRUM_TABLE_

#include "RGBSigmoidPolynomial.h"
#include "Color.h"
#include <string>
#include <vector>

namespace RISE
{
	// The colour-space type the LUT was trained against.  Operations
	// on the table take this type; callers convert from their RISEPel
	// or other source type into it.  When RISEPel == LUTTargetPel (the
	// current Stage B configuration) the conversion is identity.
	//
	// A future migration to ACES AP1 / ACEScg is a one-line change to
	// `AP1RGBPel` HERE + retrain the LUT with `--target acescg` +
	// loosen the bake-script guard.  Callers don't need to know.
	typedef Rec709RGBPel LUTTargetPel;

	class RGBToSpectrumTable
	{
	public:
		// Singleton accessor — initialised once from the baked-in
		// LUT data on first call.  Use this in production code paths.
		static const RGBToSpectrumTable& Get();

		// Direct constructor for tests + offline tools.  Use
		// `LoadFromMemory` to populate from any in-memory float
		// array following the same `(c0, c1, c2)` cell layout the
		// JH generator produces (`3 · res³ · 3` floats).  A failed
		// load leaves the table empty (queries return constant 0.5
		// spectrum).
		RGBToSpectrumTable();
		bool LoadFromMemory(
			unsigned int  resolution,
			const float*  bodyFloats,
			unsigned int  numFloats );

		bool IsLoaded() const { return loaded; }
		int  Resolution() const { return resolution; }

		// Look up sigmoid coefficients for an input in the LUT's
		// target colour space.  When RISEPel == LUTTargetPel (current
		// Stage B configuration) callers can pass `RISEPel` directly —
		// the function binds because the types are aliases.  When the
		// types differ (e.g. a future ACES migration where one of them
		// changes), callers must explicit-construct a `LUTTargetPel`
		// from their source type before calling.
		//
		// Out-of-range channels (negative or > 1) are clamped to [0, 1].
		// At runtime that's the right behaviour: a TexturePainter that
		// returns rgb > 1 (HDR EXR) should be wrapped in
		// RGBUnboundedSpectrum which scales BEFORE this lookup.
		//
		// IMPORTANT for callers that compute their own scale /
		// normalize (RGBUnboundedSpectrum, RGBIlluminantSpectrum):
		// scale + normalize MUST run in the LUT-target colour space.
		// Computing scale on a RISEPel that differs from LUTTargetPel
		// and then converting at the lookup boundary lets the post-
		// conversion clamp to [0, 1] silently desaturate any wide-
		// gamut colour and break the round-trip.  Convert to
		// LUTTargetPel FIRST, normalize there, THEN call this lookup.
		RGBSigmoidPolynomial operator()( const LUTTargetPel& rgb_target ) const;

	private:
		// Sub-table layout — see header comment for the file format.
		// Indexing: data[((maxChannel * res + iz) * res + ix) * res + iy]
		//   gives a struct of (c0, c1, c2) at that grid cell.
		struct CoeffSet { float c0, c1, c2; };

		bool                    loaded;
		int                     resolution;	// cells per axis
		std::vector<CoeffSet>   data;		// 3 · res³ entries

		// z-axis grid uses sin² warp matching the generator (finer
		// resolution near z=1 where natural-image colours cluster).
		// Inverted at lookup time to convert z ∈ [0, 1] to a fractional
		// index ∈ [0, res-1).
		static Scalar GridZToIndex( Scalar z, int res );

		// Tetrahedral interpolation in a unit cube.  Standard 6-tetra
		// decomposition; same as PBRT-v4 / Mitsuba 3 for portability of
		// LUT data.
		static CoeffSet InterpolateTetra(
			const CoeffSet& c000, const CoeffSet& c100,
			const CoeffSet& c010, const CoeffSet& c110,
			const CoeffSet& c001, const CoeffSet& c101,
			const CoeffSet& c011, const CoeffSet& c111,
			Scalar fx, Scalar fy, Scalar fz );

		// Fetch a cell with bounds-clamped indices.
		const CoeffSet& Cell( int maxC, int iz, int ix, int iy ) const;
	};
}

#endif
