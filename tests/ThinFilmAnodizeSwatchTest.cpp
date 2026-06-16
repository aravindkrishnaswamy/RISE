//////////////////////////////////////////////////////////////////////
//
//  ThinFilmAnodizeSwatchTest.cpp - Phase-1 anodize-colour VALIDATION
//    GATE for the thin-film interference feature
//    (docs/THIN_FILM_INTERFERENCE.md §8/§9, piece P1-C).
//
//    Builds the air/oxide/metal optical stack for each of the four
//    canonical (metal, oxide) pairs (Ti/TiO2, Steel/Fe3O4, Ta/Ta2O5,
//    Nb/Nb2O5), sweeps oxide thickness 5..250 nm, computes the PREVIEW
//    colorimetry of §8 (reflectance under D65, white-normalised so a
//    perfect reflector is neutral), and:
//
//      1. asserts every swatch RGB is finite and in [0,1];
//      2. asserts the canonical Ti anodization hue ladder advances
//         through the interference order (straw/bronze -> gold -> violet
//         -> blue -> cyan/teal -> yellow-green), with GENEROUS sector
//         tolerances (exact thickness depends on the rutile index, not
//         over-constrained);
//      3. asserts second-order swatches desaturate vs first order at a
//         matched hue (the robust optical signature of interference);
//      4. asserts the d->0 limit is the near-neutral bare-metal colour,
//         clearly distinct from a coloured film;
//      5. prints the full per-metal ladder (thickness, dominant lambda,
//         hue, chroma, linear + sRGB RGB) for a human to eyeball against
//         a published chart, and writes a swatch GRID png.
//
//    The colour pipeline reuses the renderer's CMFs
//    (ColorUtils::XYZFromNM) and XYZ->Rec.709 matrix so the swatch
//    matches what the renderer would produce; the png is written via
//    RISE_API_CreatePNGWriter(..., 8, eColorSpace_sRGB), which applies
//    the sRGB OETF on write — we therefore feed it LINEAR Rec.709 (no
//    double gamma).
//
//    Model: tests/HosekWilkieReferenceTest.cpp (file-static pass/fail
//    counters; Check(bool, const char*); section prints; final
//    "Results: P passed, F failed."; return s_fail == 0 ? 0 : 1).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#define _USE_MATH_DEFINES
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>	// create_directories / absolute (cross-platform; POSIX mkdir/realpath are not portable to MSVC)
#include <iostream>
#include <string>
#include <vector>

#include "thinfilm/OpticalConstants.h"
#include "thinfilm/AnodizeSwatch.h"
#include "thinfilm/AiryReference.h"
#include "thinfilm/TmmReference.h"

#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IRasterImageWriter.h"
#include "../src/Library/Interfaces/IWriteBuffer.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/Color/ColorUtils.h"
#include "../src/Library/Utilities/Optics.h"		// CalculateConductorReflectance (bare-metal anchor)

using namespace RISE;
using namespace RISE::ThinFilmReference;

namespace
{
	int s_pass = 0;
	int s_fail = 0;

	void Check( bool ok, const char* what )
	{
		if( ok ) {
			++s_pass;
		} else {
			++s_fail;
			std::cout << "  FAIL: " << what << "\n";
		}
	}

	// Repo root: tests are normally invoked with CWD = repo root (per
	// tests/README.md and run_all_tests.sh).  But run_all_tests.sh does
	// not itself cd, so harden against an unexpected CWD by probing a few
	// candidate roots for the optical-constant data and using the first
	// that resolves.  Returns "." if none match (the loader then reports
	// the missing-data failure loudly).
	std::string ResolveRepoRoot()
	{
		static const char* kCandidates[] = { ".", "..", "../..", "../../..", "../../../.." };
		for( const char* cand : kCandidates ) {
			std::string probe = std::string( cand ) + "/colors/thinfilm/substrates/Ti.n";
			FILE* f = std::fopen( probe.c_str(), "r" );
			if( f ) { std::fclose( f ); return cand; }
		}
		return ".";
	}

	//! A human-readable hue family from chroma + dominant wavelength, used
	//! only for the printed ladder (the asserts use the numeric metrics
	//! directly).  Classification is purely by chroma (achromatic cutoff)
	//! and dominant wavelength (spectral buckets / purple flag) — the
	//! CIELAB hue ANGLE is not needed here, so it is not a parameter.
	const char* HueFamily( double chroma, double dominantNm )
	{
		if( chroma < 6.0 ) {
			return "neutral/grey-metal";
		}
		if( dominantNm < 0.0 ) {
			return "purple/magenta (non-spectral)";
		}
		// Coarse spectral buckets by dominant wavelength.
		if( dominantNm < 480.0 ) return "blue";
		if( dominantNm < 490.0 ) return "blue/cyan";
		if( dominantNm < 510.0 ) return "cyan/teal";
		if( dominantNm < 560.0 ) return "green";
		if( dominantNm < 575.0 ) return "yellow-green";
		if( dominantNm < 585.0 ) return "yellow/gold";
		if( dominantNm < 600.0 ) return "gold/orange";
		if( dominantNm < 620.0 ) return "orange/bronze";
		return "red/bronze";
	}

	//! Linear Rec.709 -> 8-bit sRGB for printing the SAME bytes the PNG
	//! writer stores.  Uses the renderer's own sRGB OETF, and mirrors
	//! PNGWriter's Integerize<sRGBPel>: clamp to [0,1], encode, then
	//! TRUNCATE (cast) to 8-bit (not round) so the printed triples equal
	//! the actual pixel values.
	void LinearToSRGB8( const RISEPel& lin, int& r, int& g, int& b )
	{
		Rec709RGBPel c( lin );
		c.r = c.r < 0 ? 0 : ( c.r > 1 ? 1 : c.r );
		c.g = c.g < 0 ? 0 : ( c.g > 1 ? 1 : c.g );
		c.b = c.b < 0 ? 0 : ( c.b > 1 ? 1 : c.b );
		const sRGBPel s = ColorUtils::sRGBNonLinearization( c );
		r = int( s.r * 255.0 );
		g = int( s.g * 255.0 );
		b = int( s.b * 255.0 );
	}

	//! Smallest absolute angular separation between two hue angles (deg).
	double HueSep( double a, double b )
	{
		double d = std::fabs( a - b );
		if( d > 180.0 ) d = 360.0 - d;
		return d;
	}
}

int main()
{
	std::cout << "ThinFilmAnodizeSwatchTest -- Phase-1 anodize-colour validation gate\n";

	// ---- Load the four canonical (metal, oxide) pairs -----------------
	const std::string repoRoot = ResolveRepoRoot();
	std::vector<MetalOxidePair> pairs = LoadCanonicalPairs( repoRoot );
	std::cout << "\n[setup] Loaded " << pairs.size() << " of 4 canonical metal/oxide pairs"
	          << " from " << repoRoot << "/colors/thinfilm/\n";
	Check( pairs.size() == 4, "all 4 canonical metal/oxide pairs loaded" );
	if( pairs.empty() ) {
		std::cout << "\n  FATAL: no optical-constant data loaded; cannot run the gate.\n";
		std::cout << "\nResults: " << s_pass << " passed, " << s_fail << " failed.\n";
		return 1;
	}

	// ---- Thickness sweep + grid geometry ------------------------------
	// 5 nm steps, 5..250 nm — fine enough to read the ladder.
	const double kThickMin = 5.0;
	const double kThickMax = 250.0;
	const double kThickStep = 5.0;
	std::vector<double> thicknesses;
	for( double d = kThickMin; d <= kThickMax + 1e-9; d += kThickStep ) {
		thicknesses.push_back( d );
	}
	const size_t nCols = thicknesses.size();
	const size_t nRows = pairs.size();

	// ---- D65 white point on the integration grid ----------------------
	// (1 nm, 380..780; matches ComputeSwatch's defaults so the
	// normalisation and chroma reference share the white exactly.)
	const XYZPel white = D65WhitePointForGrid( 380, 780, 1.0 );
	std::cout << "[setup] D65 white point (swatch normalisation): "
	          << "X=" << white.X << " Y=" << white.Y << " Z=" << white.Z
	          << "  (Y should be ~1.0)\n";
	{
		const double wsum = white.X + white.Y + white.Z;
		const double wx = wsum > 0 ? white.X / wsum : 0.0;
		const double wy = wsum > 0 ? white.Y / wsum : 0.0;
		std::printf( "[setup] D65 white chromaticity: x=%.4f y=%.4f "
		             "(CIE D65 reference x=0.3127 y=0.3290)\n", wx, wy );
		// White-normalisation correctness: a perfect white reflector
		// (R == 1) maps to the D65 white point; Y == 1 by construction,
		// and the chromaticity must match published D65.
		Check( std::fabs( white.Y - 1.0 ) < 1e-6,
		       "white-reflector luminance Y == 1 (D65 normalisation)" );
		Check( std::fabs( wx - 0.3127 ) < 0.003 && std::fabs( wy - 0.3290 ) < 0.003,
		       "white-reflector chromaticity == D65 white point (neutral, not tinted)" );
	}

	// ---- Compute every swatch -----------------------------------------
	std::vector< std::vector<SwatchColor> > grid( nRows );
	for( size_t r = 0; r < nRows; ++r ) {
		grid[r].reserve( nCols );
		for( size_t c = 0; c < nCols; ++c ) {
			grid[r].push_back( ComputeSwatch( pairs[r], thicknesses[c], white ) );
		}
	}

	// ===================================================================
	//  GATE 1 — every swatch RGB finite and in [0,1]
	// ===================================================================
	std::cout << "\n[1] All swatch RGB finite and in [0,1]\n";
	{
		bool allFiniteInRange = true;
		for( size_t r = 0; r < nRows; ++r ) {
			for( size_t c = 0; c < nCols; ++c ) {
				const RISEPel& p = grid[r][c].linearRGB;
				const double ch[3] = { double( p.r ), double( p.g ), double( p.b ) };
				for( int k = 0; k < 3; ++k ) {
					// Out-of-range detection by COMPARISON, not std::isfinite:
					// the RISE build mandates -ffast-math, under which
					// std::isfinite is folded to true and never fires (see
					// src/Library/Utilities/RasterSanityScan.h for the same
					// rationale).  The negated in-range test rejects NaN and
					// +/-Inf as well as out-of-[0,1] values under -ffast-math.
					if( !( ch[k] >= -1e-9 && ch[k] <= 1.0 + 1e-9 ) ) {
						allFiniteInRange = false;
					}
				}
				// Reflectance-derived luminance must also be physical:
				// 0 <= Y <= 1 (no energy gain — the swatch is a
				// reflectance under a normalised illuminant).  Same
				// comparison-based (NOT std::isfinite) rejection as above,
				// so it stays effective under -ffast-math.
				if( !( grid[r][c].Y >= -1e-9 && grid[r][c].Y <= 1.0 + 1e-6 ) ) {
					allFiniteInRange = false;
				}
			}
		}
		Check( allFiniteInRange,
		       "every swatch (4 metals x all thicknesses) has finite RGB in [0,1] and Y in [0,1]" );
	}

	// ===================================================================
	//  Print the full per-metal ladder for human eyeballing
	// ===================================================================
	std::cout << "\n[ladder] Per-metal anodize ladders "
	          << "(thickness nm | dominant lambda | hue deg | C* chroma | linear RGB | sRGB 8-bit | family)\n";
	for( size_t r = 0; r < nRows; ++r ) {
		std::cout << "\n  === " << pairs[r].metalName << " / " << pairs[r].oxideName
		          << " ===\n";
		for( size_t c = 0; c < nCols; ++c ) {
			const SwatchColor& s = grid[r][c];
			int ri, gi, bi;
			LinearToSRGB8( s.linearRGB, ri, gi, bi );
			char domBuf[32];
			if( s.dominantNm > 0.0 ) {
				std::snprintf( domBuf, sizeof( domBuf ), "%6.1f nm", s.dominantNm );
			} else if( s.dominantNm < 0.0 ) {
				std::snprintf( domBuf, sizeof( domBuf ), "purp(%.0f)", -s.dominantNm );
			} else {
				std::snprintf( domBuf, sizeof( domBuf ), "  achrom" );
			}
			std::printf(
				"   d=%6.1f | %s | hue=%6.1f | C*=%6.2f | lin(%.3f,%.3f,%.3f) | sRGB(%3d,%3d,%3d) | %s\n",
				s.thickness_nm, domBuf, s.hueAngleDeg, s.chroma,
				double( s.linearRGB.r ), double( s.linearRGB.g ), double( s.linearRGB.b ),
				ri, gi, bi,
				HueFamily( s.chroma, s.dominantNm ) );
		}
	}

	// Find the Ti row (the canonical chart-comparison ladder).
	int tiRow = -1;
	for( size_t r = 0; r < nRows; ++r ) {
		if( pairs[r].metalName == "Ti" ) { tiRow = int( r ); break; }
	}

	// ===================================================================
	//  GATE 4 — d->0 limit is near-neutral bare metal
	// ===================================================================
	std::cout << "\n[4] d->0 limit is the near-neutral bare-metal colour\n";
	if( tiRow >= 0 ) {
		// Compute the true bare-metal (no film) swatch and the thinnest
		// film swatch; the thinnest film must be close to bare metal and
		// low-chroma, and both must be far less saturated than the
		// vivid mid-ladder interference colours.
		const SwatchColor bare = ComputeSwatch( pairs[tiRow], 0.0, white );
		int br, bg, bb;
		LinearToSRGB8( bare.linearRGB, br, bg, bb );
		std::printf( "   Ti bare-metal (d=0):  hue=%6.1f  C*=%6.2f  sRGB(%3d,%3d,%3d)\n",
		             bare.hueAngleDeg, bare.chroma, br, bg, bb );

		const SwatchColor& thin = grid[tiRow][0];	// d = 5 nm
		std::printf( "   Ti thinnest (d=%.0f):   hue=%6.1f  C*=%6.2f\n",
		             thin.thickness_nm, thin.hueAngleDeg, thin.chroma );

		// Peak chroma anywhere in the Ti ladder (a vivid interference
		// colour) for the "clearly distinct" comparison.
		double peakChroma = 0.0;
		for( size_t c = 0; c < nCols; ++c ) {
			peakChroma = std::max( peakChroma, double( grid[tiRow][c].chroma ) );
		}
		std::printf( "   Ti peak ladder chroma: C*=%6.2f\n", peakChroma );

		Check( bare.chroma < 18.0,
		       "Ti bare metal (d=0) is near-neutral (low chroma)" );
		Check( thin.chroma < peakChroma * 0.6,
		       "Ti thinnest film is clearly less saturated than the vivid mid-ladder" );
		// The thinnest film should still be close to the bare metal
		// (only a few nm of oxide).
		Check( HueSep( thin.hueAngleDeg, bare.hueAngleDeg ) < 90.0 || thin.chroma < 12.0,
		       "Ti thinnest film stays near the bare-metal colour" );
	} else {
		Check( false, "Ti row present for d->0 test" );
	}

	// ===================================================================
	//  GATE 2 — Ti hue ladder advances through the interference order
	// ===================================================================
	std::cout << "\n[2] Ti/TiO2 hue ladder reproduces the titanium anodization sequence\n";
	if( tiRow >= 0 ) {
		const std::vector<SwatchColor>& L = grid[tiRow];

		// The known titanium sequence, by dominant-wavelength FAMILY, as
		// thickness grows through the FIRST interference order.  We do NOT
		// pin exact thicknesses (the rutile index sets those).  Instead we
		// assert the ORDER in which hue families first appear:
		//   gold/yellow (warm) -> purple/violet -> blue -> cyan/teal
		//   -> (second order) yellow-green/green.
		//
		// Encode this as the thickness of first appearance of each family
		// and assert the thicknesses are monotonically increasing.

		auto firstThicknessWithDominantInRange =
			[&]( double loNm, double hiNm, double minChroma ) -> double {
				for( size_t c = 0; c < L.size(); ++c ) {
					if( L[c].chroma < minChroma ) continue;
					const double dnm = L[c].dominantNm;
					if( dnm > 0.0 && dnm >= loNm && dnm <= hiNm ) {
						return L[c].thickness_nm;
					}
				}
				return -1.0;
			};
		auto firstThicknessPurple =
			[&]( double minChroma ) -> double {
				for( size_t c = 0; c < L.size(); ++c ) {
					if( L[c].chroma < minChroma ) continue;
					if( L[c].dominantNm < 0.0 ) {	// non-spectral (purple line)
						return L[c].thickness_nm;
					}
				}
				return -1.0;
			};

		// Warm (gold/yellow/orange/bronze): dominant 565..620 nm.
		const double dGold   = firstThicknessWithDominantInRange( 565.0, 625.0, 8.0 );
		// Purple/violet (non-spectral, between blue and red).
		const double dPurple = firstThicknessPurple( 10.0 );
		// Blue: dominant 450..485 nm.
		const double dBlue   = firstThicknessWithDominantInRange( 450.0, 485.0, 10.0 );
		// Cyan/teal: dominant 485..510 nm.
		const double dCyan   = firstThicknessWithDominantInRange( 485.0, 515.0, 8.0 );

		std::printf( "   first gold/yellow (565-625nm) at d = %6.1f nm\n", dGold );
		std::printf( "   first purple/violet  (non-spectral) at d = %6.1f nm\n", dPurple );
		std::printf( "   first blue        (450-485nm) at d = %6.1f nm\n", dBlue );
		std::printf( "   first cyan/teal   (485-515nm) at d = %6.1f nm\n", dCyan );

		// All four families must appear somewhere in the ladder.
		Check( dGold   > 0.0, "Ti ladder shows a warm gold/yellow band" );
		Check( dPurple > 0.0, "Ti ladder shows a purple/violet band" );
		Check( dBlue   > 0.0, "Ti ladder shows a blue band" );
		Check( dCyan   > 0.0, "Ti ladder shows a cyan/teal band" );

		// Canonical first-order ORDER: gold -> purple -> blue -> cyan.
		// Generous: each subsequent family appears at greater thickness.
		if( dGold > 0.0 && dPurple > 0.0 ) {
			Check( dPurple > dGold, "purple appears after gold (first-order progression)" );
		}
		if( dPurple > 0.0 && dBlue > 0.0 ) {
			Check( dBlue > dPurple, "blue appears after purple (first-order progression)" );
		}
		if( dBlue > 0.0 && dCyan > 0.0 ) {
			Check( dCyan > dBlue, "cyan/teal appears after blue (first-order progression)" );
		}
	} else {
		Check( false, "Ti row present for hue-sequence test" );
	}

	// ===================================================================
	//  GATE 3 — higher-order desaturation at matched hue
	// ===================================================================
	std::cout << "\n[3] Higher interference orders desaturate vs lower orders at matched hue\n";
	if( tiRow >= 0 ) {
		const std::vector<SwatchColor>& L = grid[tiRow];

		// The robust optical signature of thin-film interference: as the
		// film thickens, the colour cycles repeatedly through the SAME
		// hues (one cycle ~ every lambda/(2n) ~ 100 nm for rutile), but
		// the chroma ENVELOPE decays with order — at high order the
		// reflectance oscillation sweeps several fringes within the broad
		// dominant-wavelength response window, so the net colour washes
		// out toward grey.
		//
		// NOTE (a real subtlety we must NOT paper over): the chroma is
		// NOT monotone the moment you cross d=0.  The very first order is
		// on the rising edge out of the bare-metal limit, so its chroma
		// can be LOWER than the second order for some hues (e.g. the
		// d~20 nm gold sits right next to bare metal).  A naive
		// "mean chroma of d<=X vs d>X" split is therefore the WRONG
		// metric (it inverts depending on where the split lands relative
		// to the hue cycles).  The physically-correct, robust signature
		// is matched-hue across orders and the peak-chroma envelope —
		// which is what we assert here.

		// Bin spectral BLUE swatches (the cleanest recurring spectral
		// hue) by interference order.  Order windows in thickness are
		// set by the ~100 nm rutile cycle; we take the PEAK blue chroma
		// in each window (the order's most saturated blue).
		struct OrderWin { const char* name; double lo, hi; };
		const OrderWin blueWins[] = {
			{ "order-1 blue", 25.0,  75.0  },
			{ "order-2 blue", 135.0, 175.0 },
			{ "order-3 blue", 235.0, 250.0 },
		};
		double bluePeak[3] = { -1.0, -1.0, -1.0 };
		double blueAtD[3]  = { 0.0, 0.0, 0.0 };
		for( int w = 0; w < 3; ++w ) {
			for( size_t c = 0; c < L.size(); ++c ) {
				if( L[c].thickness_nm < blueWins[w].lo ||
				    L[c].thickness_nm > blueWins[w].hi ) continue;
				if( L[c].dominantNm <= 0.0 ) continue;		// spectral only
				// Blue hue family: dominant 440..490 nm.
				if( L[c].dominantNm < 440.0 || L[c].dominantNm > 492.0 ) continue;
				if( L[c].chroma > bluePeak[w] ) {
					bluePeak[w] = L[c].chroma;
					blueAtD[w] = L[c].thickness_nm;
				}
			}
			std::printf( "   %s peak C*=%6.2f at d=%.0f nm\n",
			             blueWins[w].name, bluePeak[w], blueAtD[w] );
		}

		// All three blue orders must exist for a meaningful matched-hue
		// comparison across the 5..250 nm sweep.
		Check( bluePeak[0] > 0.0 && bluePeak[1] > 0.0 && bluePeak[2] > 0.0,
		       "blue recurs in (at least) three interference orders" );
		if( bluePeak[0] > 0.0 && bluePeak[1] > 0.0 && bluePeak[2] > 0.0 ) {
			// The envelope is non-increasing order-1 -> order-2.  No slack:
			// the previous "+5.0" tolerance was a tautology (it would have
			// passed even if order-2 sat ABOVE order-1, which is not a
			// desaturation signature).  Order-1 and order-2 ARE expected to
			// be nearly equal here — and that near-equality is itself a
			// sampling artifact, not physics: the true order-1 blue peak
			// (~70.8 at d~32.5 nm) falls between the 5 nm grid points, so
			// the grid-sampled order-1 (~67 at d=35) undershoots its own
			// envelope, while order-2's peak (d~140) lands on a grid point.
			// We therefore do NOT lean on the order-1/order-2 gap as the
			// signature; we only assert order-2 is not ABOVE order-1...
			Check( bluePeak[1] <= bluePeak[0],
			       "matched-hue blue: order-2 chroma not above order-1 (envelope non-increasing)" );
			// ...and put the real weight on the UNAMBIGUOUS signature: by
			// order-3 the fringe density inside the blue CMF response window
			// has washed the colour out to well under HALF the saturation of
			// BOTH lower orders.  This is the robust, sampling-insensitive
			// fingerprint of interference desaturation.
			Check( bluePeak[2] < bluePeak[1] * 0.5,
			       "matched-hue blue: order-3 chroma < half of order-2 (interference desaturation)" );
			Check( bluePeak[2] < bluePeak[0] * 0.5,
			       "matched-hue blue: order-3 chroma < half of order-1 (robust desaturation signature)" );
		}

		// Peak-chroma ENVELOPE over the whole ladder: the deep-film
		// region must be less saturated than the mid-ladder contrast
		// peak (the order-broadening washout), independent of hue.
		double midPeak = 0.0, deepPeak = 0.0;
		for( size_t c = 0; c < L.size(); ++c ) {
			if( L[c].thickness_nm >= 25.0 && L[c].thickness_nm <= 145.0 ) {
				midPeak = std::max( midPeak, double( L[c].chroma ) );
			}
			if( L[c].thickness_nm >= 235.0 ) {
				deepPeak = std::max( deepPeak, double( L[c].chroma ) );
			}
		}
		std::printf( "   peak chroma: mid-ladder (25-145nm) C*=%6.2f  vs deep (>=235nm) C*=%6.2f\n",
		             midPeak, deepPeak );
		Check( deepPeak < midPeak,
		       "deep-film peak chroma < mid-ladder peak chroma (high-order washout)" );
	} else {
		Check( false, "Ti row present for higher-order desaturation test" );
	}

	// ===================================================================
	//  Cross-check: Airy == TMM at normal incidence (sanity on the
	//  reflectance the swatch integral consumes).
	// ===================================================================
	std::cout << "\n[xcheck] Airy reflectance == TMM reflectance at normal incidence\n";
	{
		double maxDiff = 0.0;
		for( size_t r = 0; r < nRows; ++r ) {
			for( double nm = 400.0; nm <= 700.0; nm += 50.0 ) {
				for( double d = 20.0; d <= 240.0; d += 40.0 ) {
					const Stack st = pairs[r].BuildStack( d, nm );
					const double airy = AiryReflectanceUnpolarized( st, nm, 0.0 );
					const double tmm  = TmmReflectanceUnpolarized( st, nm, 0.0 );
					maxDiff = std::max( maxDiff, std::fabs( airy - tmm ) );
				}
			}
		}
		std::printf( "   max |Airy - TMM| over the swatch grid = %.3e\n", maxDiff );
		Check( maxDiff < 1e-9, "Airy and TMM agree to ~machine epsilon (single-film cross-check)" );
	}

	// ===================================================================
	//  Anchor: the d->0 (bare-metal) reflectance the oracle consumes must
	//  equal RISE's OWN conductor Fresnel (Optics::CalculateConductorReflectance).
	//  This ties the swatch's optics to the renderer's Fresnel and would
	//  catch a shared Airy+TMM sign-convention error that the Airy==TMM
	//  cross-check alone cannot (§9 bare-substrate-limit anchor).
	// ===================================================================
	std::cout << "\n[xcheck] Bare-metal (d->0) R == RISE Optics::CalculateConductorReflectance\n";
	{
		const Vector3 nrm( 0, 0, 1 );
		const Vector3 inc( 0, 0, 1 );	// normal incidence
		double maxDiff = 0.0;
		for( size_t r = 0; r < nRows; ++r ) {
			for( double nm = 400.0; nm <= 700.0; nm += 50.0 ) {
				const Complex Nm = pairs[r].metal.IndexAtNM( nm );
				// RISE conductor Fresnel: air (Ni=1) -> metal (n + i*k).
				const Scalar Rrise = Optics::CalculateConductorReflectance<Scalar>(
					inc, nrm, Scalar( 1.0 ), Scalar( Nm.real() ), Scalar( Nm.imag() ) );
				// Oracle bare-metal limit (d=0 single film == no film).
				const Stack bare = pairs[r].BuildBareStack( nm );
				const double Rtmm = TmmReflectanceUnpolarized( bare, nm, 0.0 );
				maxDiff = std::max( maxDiff, std::fabs( double( Rrise ) - Rtmm ) );
			}
		}
		std::printf( "   max |oracle d->0  -  RISE conductor Fresnel| = %.3e\n", maxDiff );
		Check( maxDiff < 1e-9,
		       "oracle bare-metal limit == RISE's own conductor Fresnel (ties optics to renderer)" );
	}

	// ===================================================================
	//  Anchor: the Ti hue-cycle PERIOD in thickness must match the
	//  half-wave optical prediction lambda_mid/(2 n) ~ 100 nm for rutile.
	//  This is the strongest guard against an index / units / integration
	//  shift: a mis-scaled ladder would have the wrong cycle period even
	//  if its hue ORDER were preserved.
	// ===================================================================
	std::cout << "\n[xcheck] Ti hue-cycle period matches the half-wave optical prediction\n";
	if( tiRow >= 0 ) {
		const std::vector<SwatchColor>& L = grid[tiRow];
		// The PURPLE/violet band (non-spectral, between blue and red) is
		// the most distinctive recurring hue and — unlike the warm band —
		// it sits clear of the bare-metal rising edge at d->0, so its
		// per-order peaks give an honest cycle period.  Collect the
		// thickness of each LOCAL chroma maximum whose hue is purple
		// (dominantNm < 0, i.e. on the purple line).
		std::vector<double> purplePeaks;
		for( size_t c = 1; c + 1 < L.size(); ++c ) {
			const SwatchColor& s = L[c];
			if( s.dominantNm >= 0.0 ) continue;		// purple line only
			if( s.chroma < 30.0 ) continue;
			if( s.chroma >= L[c - 1].chroma && s.chroma >= L[c + 1].chroma ) {
				// Don't double-count adjacent samples of one peak.
				if( purplePeaks.empty() || s.thickness_nm - purplePeaks.back() > 40.0 ) {
					purplePeaks.push_back( s.thickness_nm );
				}
			}
		}
		std::printf( "   purple-band chroma peaks at d =" );
		for( double d : purplePeaks ) std::printf( " %.0f", d );
		std::printf( " nm\n" );

		// Mid-visible rutile index (the swatch's own oxide table at 550nm).
		const double nMid = pairs[tiRow].oxide.IndexAtNM( 550.0 ).real();
		const double predictedPeriod = 550.0 / ( 2.0 * nMid );	// half-wave color clock
		std::printf( "   predicted period lambda/(2n) = 550/(2*%.3f) = %.1f nm\n",
		             nMid, predictedPeriod );

		Check( purplePeaks.size() >= 2,
		       "Ti ladder shows at least two purple-band interference orders" );
		if( purplePeaks.size() >= 2 ) {
			double sumGap = 0.0;
			for( size_t i = 1; i < purplePeaks.size(); ++i ) {
				sumGap += purplePeaks[i] - purplePeaks[i - 1];
			}
			const double measuredPeriod = sumGap / ( purplePeaks.size() - 1 );
			std::printf( "   measured mean cycle period = %.1f nm\n", measuredPeriod );
			// 30% tolerance: the perceived-color period tracks but is not
			// exactly lambda_mid/(2n) (the dominant wavelength drifts
			// across the cycle and the mid-visible index is a single
			// representative).  A units/index error (factor-of-2, or a
			// 10x oxide-index slip) blows well past 30%.
			Check( std::fabs( measuredPeriod - predictedPeriod ) < 0.30 * predictedPeriod,
			       "Ti cycle period matches half-wave prediction (no index/units shift)" );
		}
	} else {
		Check( false, "Ti row present for cycle-period anchor" );
	}

	// ===================================================================
	//  Render the swatch GRID png to ./rendered/
	// ===================================================================
	std::cout << "\n[png] Writing the swatch grid\n";
	std::string pngPath;
	{
		// Write to <repoRoot>/rendered/ (gitignored), creating it if
		// needed.  Anchoring to the resolved repo root keeps the output
		// in a predictable place regardless of the invoking CWD.
		const std::string outDir = repoRoot + "/rendered";
		std::error_code mkdirEc;
		std::filesystem::create_directories( outDir, mkdirEc );	// no-op if it exists; ignore EEXIST-equivalent

		const unsigned int kCell = 26;		// px per swatch cell
		const unsigned int kGap  = 2;		// px gap between cells
		const unsigned int imgW  = static_cast<unsigned int>( nCols * ( kCell + kGap ) + kGap );
		const unsigned int imgH  = static_cast<unsigned int>( nRows * ( kCell + kGap ) + kGap );

		pngPath = outDir + "/thinfilm_anodize_swatches.png";

		IWriteBuffer* buf = nullptr;
		if( !RISE_API_CreateDiskFileWriteBuffer( &buf, pngPath.c_str() ) || !buf ) {
			Check( false, "create disk file write buffer for the swatch png" );
		} else {
			IRasterImageWriter* writer = nullptr;
			// 8 bpp, sRGB: the writer applies the sRGB OETF on write, so
			// we feed LINEAR Rec.709 colours (RISEColor stores RISEPel ==
			// linear Rec.709).
			if( !RISE_API_CreatePNGWriter( &writer, *buf, 8, eColorSpace_sRGB ) || !writer ) {
				Check( false, "create png writer for the swatch grid" );
			} else {
				writer->BeginWrite( imgW, imgH );

				// Background: mid grey (linear 0.05) so swatch edges read.
				const RISEColor bgCol( RISEPel( 0.05, 0.05, 0.05 ), 1.0 );

				for( unsigned int y = 0; y < imgH; ++y ) {
					for( unsigned int x = 0; x < imgW; ++x ) {
						RISEColor col = bgCol;
						// Which cell?
						const int cx = ( int( x ) - int( kGap ) ) / int( kCell + kGap );
						const int cy = ( int( y ) - int( kGap ) ) / int( kCell + kGap );
						const int ox = ( int( x ) - int( kGap ) ) % int( kCell + kGap );
						const int oy = ( int( y ) - int( kGap ) ) % int( kCell + kGap );
						if( cx >= 0 && cx < int( nCols ) && cy >= 0 && cy < int( nRows ) &&
						    ox >= 0 && ox < int( kCell ) && oy >= 0 && oy < int( kCell ) ) {
							const RISEPel& p = grid[cy][cx].linearRGB;
							col = RISEColor( p, 1.0 );
						}
						writer->WriteColor( col, x, y );
					}
				}

				writer->EndWrite();
				safe_release( writer );
				Check( true, "swatch grid png written" );
			}
			safe_release( buf );
		}
	}
	if( !pngPath.empty() ) {
		// Print the absolute path so the swatch is findable regardless of
		// the invoking CWD.  std::filesystem::weakly_canonical is the
		// cross-platform analogue of POSIX realpath (which has no MSVC
		// equivalent): it makes the path absolute AND collapses any
		// "."/".." segments.  Fall back to the as-written path on error.
		std::error_code absEc;
		const std::filesystem::path absPath =
			std::filesystem::weakly_canonical( pngPath, absEc );
		std::cout << "   swatch grid PNG: "
		          << ( absEc ? pngPath : absPath.string() ) << "\n";
		std::cout << "   (rows top->bottom:";
		for( size_t r = 0; r < nRows; ++r ) {
			std::cout << " " << pairs[r].metalName << "/" << pairs[r].oxideName;
		}
		std::cout << "; columns left->right: d = " << kThickMin << ".." << kThickMax
		          << " nm step " << kThickStep << ")\n";
	}

	std::cout << "\nResults: " << s_pass << " passed, " << s_fail << " failed.\n";
	return ( s_fail == 0 ) ? 0 : 1;
}
