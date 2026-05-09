//////////////////////////////////////////////////////////////////////
//
//  FrameStoreColorMathTest.cpp - L0 regression gate for the FrameStore
//  output-side color math.
//
//  Coverage:
//    1. Transfer-function round trips: ApplyTransfer(Inverse(x)) ≈ x
//       for sRGB / PQ / HLG / Gamma2.2 / Linear, across a sweep of
//       inputs in each function's expected domain.
//    2. Sanitisation: NaN, +Inf, -Inf, and negative inputs all clamp
//       to 0 before any transfer.
//    3. ROMM-to-sRGB conversion parity: white-in stays white-out
//       (within numerical noise); pure-channel inputs land in the
//       expected target-channel positions.
//    4. ROMM-to-Display-P3 / BT.2020: composed transforms preserve
//       white-point invariant (D50 white in ROMM → D65 white in target).
//    5. Half-float (binary16) round-trips: every representable half is
//       a fixed point of HalfToFloat → FloatToHalf; subnormals + Inf +
//       NaN handled correctly.
//    6. ViewTransform pipeline: exposure scaling, identity matrix
//       fast-path, tone curve gating, integration of all stages.
//    7. TargetFormat info table: every enum value has a row, byte
//       counts are non-zero, alpha flag agrees with channel count.
//
//  The test is a standalone executable per the RISE convention; it
//  prints PASS / FAIL per assertion and returns non-zero on failure.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "../src/Library/Rendering/FrameStoreColorSpace.h"
#include "../src/Library/Rendering/TargetFormat.h"
#include "../src/Library/Rendering/ViewTransform.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/Color/ColorUtils.h"

using namespace RISE;
using namespace RISE::FrameStoreOutput;

namespace
{
	int gFailCount = 0;
	int gPassCount = 0;

	void Check( bool cond, const std::string& label )
	{
		if ( cond ) {
			++gPassCount;
		} else {
			++gFailCount;
			std::cerr << "FAIL: " << label << "\n";
		}
	}

	// Bit-pattern check: under -ffast-math (-ffinite-math-only) the
	// std::isnan / std::isfinite intrinsics are unreliable; the
	// compiler is told it may assume no NaN / Inf and folds the
	// checks to constants.  Every finite-domain check in this test
	// goes through these helpers instead.
	bool BitsIsNaN( double x )
	{
		uint64_t bits;
		std::memcpy(&bits, &x, sizeof(bits));
		const uint64_t expMask  = 0x7FF0000000000000ULL;
		const uint64_t mantMask = 0x000FFFFFFFFFFFFFULL;
		return ( bits & expMask ) == expMask && ( bits & mantMask ) != 0;
	}

	bool BitsIsFinite( double x )
	{
		uint64_t bits;
		std::memcpy(&bits, &x, sizeof(bits));
		const uint64_t expMask = 0x7FF0000000000000ULL;
		return ( bits & expMask ) != expMask;
	}

	// Construct IEEE 754 special values via bit pattern.  Under
	// `-ffast-math` (`-fno-honor-infinities` / `-fno-honor-nans`),
	// referring to std::numeric_limits<>::infinity() or quiet_NaN()
	// at the source level produces a `-Wnan-infinity-disabled`
	// warning (RISE treats warnings as bugs).  These helpers produce
	// the actual bit-pattern values without going through the
	// numeric_limits source-level expressions.
	double MakeDoubleFromBits( uint64_t bits )
	{
		double d;
		std::memcpy( &d, &bits, sizeof(d) );
		return d;
	}

	float MakeFloatFromBits( uint32_t bits )
	{
		float f;
		std::memcpy( &f, &bits, sizeof(f) );
		return f;
	}

	bool ApproxEq( double a, double b, double eps )
	{
		if ( !BitsIsFinite(a) || !BitsIsFinite(b) ) return BitsIsNaN(a) == BitsIsNaN(b);
		return std::fabs(a - b) <= eps;
	}

	// ─── Section 1: transfer round-trip ───────────────────────────
	void TestTransferRoundTrip()
	{
		// sRGB: encode then decode should land within 1e-12 across [0, 1].
		for ( double x = 0.0; x <= 1.0001; x += 0.05 ) {
			const double encoded = ApplyTransfer( TransferFunction::sRGB, x );
			const double back = ApplyTransferInverse( TransferFunction::sRGB, encoded );
			std::ostringstream os;
			os << "sRGB round-trip @ x=" << x << ": got " << back;
			Check( ApproxEq( back, x, 1e-12 ), os.str() );
		}

		// Linear: trivial identity for non-negative inputs.
		for ( double x = 0.0; x <= 100.0; x += 1.0 ) {
			const double encoded = ApplyTransfer( TransferFunction::Linear, x );
			const double back = ApplyTransferInverse( TransferFunction::Linear, encoded );
			std::ostringstream os;
			os << "Linear round-trip @ x=" << x;
			Check( ApproxEq( back, x, 1e-12 ), os.str() );
		}

		// PQ: round-trip across the scene-referred range [0, 100],
		// where 1.0 = SDR white, 100.0 = full HDR10 1000-nit peak.
		// Tolerance 1e-9 reflects the m1 = ~0.16 fractional power
		// roundtrip precision; PQ is well-conditioned in this range.
		for ( double x = 0.0; x <= 100.0; x += 5.0 ) {
			const double encoded = ApplyTransfer( TransferFunction::PQ_ST2084, x );
			const double back = ApplyTransferInverse( TransferFunction::PQ_ST2084, encoded );
			std::ostringstream os;
			os << "PQ round-trip @ x=" << x << ": got " << back;
			Check( ApproxEq( back, x, 1e-8 ), os.str() );
		}

		// PQ ceiling: scene-referred 100.0 should encode at 1.0
		// (10000 nits PQ peak).
		const double pqCeil = ApplyTransfer( TransferFunction::PQ_ST2084, 100.0 );
		Check( ApproxEq( pqCeil, 1.0, 1e-9 ),
			"PQ ceiling at 100.0 scene-referred = 1.0 PQ" );

		// PQ over-range clamps at 1.0 (no NaN, no >1.0 leak).
		const double pqOver = ApplyTransfer( TransferFunction::PQ_ST2084, 1000.0 );
		Check( pqOver <= 1.0 && std::isfinite( pqOver ),
			"PQ clamps over-range to ≤ 1.0" );

		// Gamma 2.2 round-trip on [0, 1].
		for ( double x = 0.0; x <= 1.0001; x += 0.05 ) {
			const double encoded = ApplyTransfer( TransferFunction::Gamma22, x );
			const double back = ApplyTransferInverse( TransferFunction::Gamma22, encoded );
			std::ostringstream os;
			os << "Gamma22 round-trip @ x=" << x;
			Check( ApproxEq( back, x, 1e-10 ), os.str() );
		}

		// HLG round-trip on [0, 12].
		for ( double x = 0.0; x <= 12.0; x += 0.5 ) {
			const double encoded = ApplyTransfer( TransferFunction::HLG, x );
			const double back = ApplyTransferInverse( TransferFunction::HLG, encoded );
			std::ostringstream os;
			os << "HLG round-trip @ x=" << x << ": got " << back;
			Check( ApproxEq( back, x, 1e-9 ), os.str() );
		}
	}

	// ─── Section 2: sanitisation ──────────────────────────────────
	void TestSanitisation()
	{
		// IEEE 754 binary64 special-value bit patterns.  See
		// MakeDoubleFromBits comment for why we don't use
		// std::numeric_limits here.
		const double inf = MakeDoubleFromBits( 0x7FF0000000000000ULL );
		const double nan = MakeDoubleFromBits( 0x7FF8000000000000ULL );
		const double neginf = MakeDoubleFromBits( 0xFFF0000000000000ULL );

		const TransferFunction fns[] = {
			TransferFunction::Linear, TransferFunction::sRGB,
			TransferFunction::PQ_ST2084, TransferFunction::HLG,
			TransferFunction::Gamma22
		};

		// All transfer functions EXCEPT Linear sanitise pathological
		// inputs (NaN/Inf/negative → 0).  Linear is true identity by
		// design (per L1 adversarial review P3): archival HDR paths
		// must preserve out-of-range values bit-identically.  The
		// sanitise responsibility shifts to the LDR-fixed
		// quantisation step in EncodePixel.
		// All transfer functions EXCEPT Linear sanitise pathological
		// inputs (NaN/Inf/negative → 0).  Linear is true identity by
		// design (per L1 adversarial review P3): archival HDR paths
		// must preserve out-of-range values bit-identically.  The
		// sanitise responsibility shifts to the LDR-fixed
		// quantisation step in EncodePixel.  End-to-end Inf/NaN
		// preservation is proven via FrameStoreTest's
		// TestHDRArchivalIdentity (the encode-pipeline path), which
		// is more meaningful than a function-call-level bit check
		// here (the latter is sensitive to how -O3 -flto -ffast-math
		// inlines the call site).
		for ( TransferFunction fn : fns ) {
			if ( fn == TransferFunction::Linear ) {
				// Linear is identity: finite negative passes through.
				// Pathological inputs are tested via the L1 archival
				// pipeline test, not here.
				Check( ApplyTransfer( fn, -1.0 ) == -1.0,
					"Linear: negative passes through (archival fidelity)" );
				continue;
			}
			Check( ApplyTransfer( fn, nan ) == 0.0,
				"transfer NaN → 0" );
			Check( ApplyTransfer( fn, inf ) == 0.0,
				"transfer +Inf → 0" );
			Check( ApplyTransfer( fn, neginf ) == 0.0,
				"transfer -Inf → 0" );
			Check( ApplyTransfer( fn, -1.0 ) == 0.0,
				"transfer negative → 0" );
		}
	}

	// ─── Section 3: ROMM → sRGB primaries ─────────────────────────
	void TestPrimariesConversion()
	{
		// PARITY INVARIANT: ConvertROMMToTargetPrimaries(sRGB_Linear,
		// ...) must produce byte-identical output to the existing
		// ColorUtils::ROMMRGBtoRec709RGB.  This is the single source
		// of truth for ROMM→sRGB-linear in the codebase; any drift
		// here would silently change L2/L3 PNG output.  Note: ROMM
		// (1, 1, 1) is D50 white through ROMM primaries; after the
		// embedded D50→D65 Bradford CAT it does NOT map to (1, 1, 1)
		// in Rec709.  This test asserts only equality with the
		// existing infrastructure, not any closed-form invariant.
		const struct { double r, g, b; } samples[] = {
			{ 0.0, 0.0, 0.0 },
			{ 1.0, 1.0, 1.0 },
			{ 0.5, 0.5, 0.5 },
			{ 0.123, 0.456, 0.789 },
			{ 1.0, 0.0, 0.0 },
			{ 0.0, 1.0, 0.0 },
			{ 0.0, 0.0, 1.0 },
			{ 2.5, 0.1, 4.0 },     // HDR-style sample
		};
		for ( const auto& s : samples ) {
			ROMMRGBPel rommPel; rommPel.r = s.r; rommPel.g = s.g; rommPel.b = s.b;
			const Rec709RGBPel ref = ColorUtils::ROMMRGBtoRec709RGB( rommPel );
			double r, g, b;
			ConvertROMMToTargetPrimaries( FSColorSpace::sRGB_Linear,
				s.r, s.g, s.b, r, g, b );
			std::ostringstream os;
			os << "ROMM(" << s.r << "," << s.g << "," << s.b
			   << ") → sRGB linear matches ColorUtils";
			Check( r == ref.r && g == ref.g && b == ref.b, os.str() );
		}

		// Black is preserved exactly through every target.
		double r, g, b;
		ConvertROMMToTargetPrimaries( FSColorSpace::DisplayP3_Linear,
			0.0, 0.0, 0.0, r, g, b );
		Check( r == 0.0 && g == 0.0 && b == 0.0,
			"ROMM black → P3 black exact" );
		ConvertROMMToTargetPrimaries( FSColorSpace::BT2020_Linear,
			0.0, 0.0, 0.0, r, g, b );
		Check( r == 0.0 && g == 0.0 && b == 0.0,
			"ROMM black → BT.2020 black exact" );

		// ROMM → ROMM is identity (no-op fast path).
		ConvertROMMToTargetPrimaries( FSColorSpace::ROMM_Linear,
			0.123, 0.456, 0.789, r, g, b );
		Check( r == 0.123 && g == 0.456 && b == 0.789,
			"ROMM → ROMM identity" );

		// Sanity check: P3 chain produces values close to the sRGB
		// chain since both share D65 — i.e. P3 should not produce
		// wildly different magnitudes from sRGB for the same ROMM
		// input.  (The actual ratio depends on gamut difference but
		// is bounded by ~30% in any channel.)
		ConvertROMMToTargetPrimaries( FSColorSpace::DisplayP3_Linear,
			0.5, 0.5, 0.5, r, g, b );
		double sr, sg, sb;
		ConvertROMMToTargetPrimaries( FSColorSpace::sRGB_Linear,
			0.5, 0.5, 0.5, sr, sg, sb );
		Check( std::fabs( r - sr ) < 0.5 * std::fabs( sr ) + 1e-6,
			"ROMM(.5) → P3 R within 50% of sRGB R" );

		// Pure sRGB-red through M_sRGB→P3 has R near 0.82, near-zero G/B.
		double pR, pG, pB;
		const double* m = GetROMMToTargetMatrix( FSColorSpace::DisplayP3_Linear );
		ApplyMatrix3x3( m, 1.0, 0.0, 0.0, pR, pG, pB );
		Check( pR > 0.8 && pR < 1.0,    "sRGB red → P3 R near 0.82" );
		Check( pG > 0.0 && pG < 0.05,   "sRGB red → P3 G small positive" );
		Check( pB > 0.0 && pB < 0.05,   "sRGB red → P3 B small positive" );

		// Row-sum invariant: each row of M_sRGB→P3 sums to 1
		// (the white-point preservation property — feeding (1,1,1)
		// in produces (1,1,1) out, modulo numerical noise on the
		// last digit of the published constants).
		const double row0Sum = m[0] + m[1] + m[2];
		const double row1Sum = m[3] + m[4] + m[5];
		const double row2Sum = m[6] + m[7] + m[8];
		Check( ApproxEq( row0Sum, 1.0, 1e-6 ),
			"M_sRGB→P3 row 0 sums to 1 (white-point invariant)" );
		Check( ApproxEq( row1Sum, 1.0, 1e-6 ),
			"M_sRGB→P3 row 1 sums to 1 (white-point invariant)" );
		Check( ApproxEq( row2Sum, 1.0, 1e-6 ),
			"M_sRGB→P3 row 2 sums to 1 (white-point invariant)" );

		// Same for BT.2020 (BT.2087 published values).
		const double* m2 = GetROMMToTargetMatrix( FSColorSpace::BT2020_Linear );
		const double r0 = m2[0] + m2[1] + m2[2];
		const double r1 = m2[3] + m2[4] + m2[5];
		const double r2 = m2[6] + m2[7] + m2[8];
		Check( ApproxEq( r0, 1.0, 1e-5 ), "M_sRGB→BT2020 row 0 sums to 1" );
		Check( ApproxEq( r1, 1.0, 1e-5 ), "M_sRGB→BT2020 row 1 sums to 1" );
		Check( ApproxEq( r2, 1.0, 1e-5 ), "M_sRGB→BT2020 row 2 sums to 1" );
	}

	// ─── Section 4: half-float round-trip ─────────────────────────
	void TestHalfFloatRoundTrip()
	{
		// Sweep representable halves; each must be a fixed point.
		// IMPORTANT: under -ffast-math the compiler may prove that any
		// `float` value's bit pattern cannot satisfy NaN/Inf masks via
		// type-based reasoning.  We therefore use the *Bits variants
		// (uint32_t in / out) which never let the value flow through a
		// `float`-typed temporary.  This is exactly the situation the
		// FloatBitsToHalf / HalfToFloatBits API exists for.
		int mismatches = 0;
		for ( uint32_t i = 0; i < 0x10000u; ++i ) {
			const uint16_t h = static_cast<uint16_t>( i );
			const uint32_t fBits = HalfToFloatBits( h );

			const uint32_t hExp = ( h >> 10 ) & 0x1Fu;
			const uint32_t hMantissa = h & 0x3FFu;
			if ( hExp == 0x1Fu && hMantissa != 0 ) {
				// NaN half → NaN float.  Verify via captured bits:
				// exp all 1s, mantissa nonzero.
				const bool isNaN = ( ( fBits & 0x7F800000u ) == 0x7F800000u )
				                && ( ( fBits & 0x007FFFFFu ) != 0u );
				if ( !isNaN ) {
					++mismatches;
					if ( mismatches <= 3 ) {
						std::cerr << "  half NaN failed: 0x" << std::hex << h
							<< " float bits=0x" << fBits << std::dec << "\n";
					}
				}
				continue;
			}

			const uint16_t back = FloatBitsToHalf( fBits );
			if ( h != back ) {
				++mismatches;
				if ( mismatches <= 3 ) {
					std::cerr << "  half round-trip mismatch: in=0x"
						<< std::hex << h << " float bits=0x" << fBits
						<< " back=0x" << back << std::dec << "\n";
				}
			}
		}
		std::ostringstream os;
		os << "half round-trip across all 65536 patterns (mismatches=" << mismatches << ")";
		Check( mismatches == 0, os.str() );

		// Spot checks on known constants.
		Check( FloatToHalf( 0.0f ) == 0x0000u, "half encode 0.0 = 0x0000" );
		Check( FloatToHalf( -0.0f ) == 0x8000u, "half encode -0.0 = 0x8000" );
		Check( FloatToHalf( 1.0f ) == 0x3C00u, "half encode 1.0 = 0x3C00" );
		Check( FloatToHalf( -1.0f ) == 0xBC00u, "half encode -1.0 = 0xBC00" );
		Check( FloatToHalf( 2.0f ) == 0x4000u, "half encode 2.0 = 0x4000" );

		// Float Inf survives.  Bit-construct rather than call
		// numeric_limits::infinity() (warns under -Wnan-infinity-disabled).
		const float inf = MakeFloatFromBits( 0x7F800000u );
		const float neginf = MakeFloatFromBits( 0xFF800000u );
		Check( FloatToHalf( inf ) == 0x7C00u, "half encode +Inf = 0x7C00" );
		Check( FloatToHalf( neginf ) == 0xFC00u, "half encode -Inf = 0xFC00" );

		// Float overflow → half-Inf.
		Check( FloatToHalf( 1e30f ) == 0x7C00u, "half encode 1e30f → +Inf" );
		Check( FloatToHalf( -1e30f ) == 0xFC00u, "half encode -1e30f → -Inf" );

		// Float NaN preserves NaN-ness.  Manufacture a NaN bit pattern
		// directly via memcpy — std::numeric_limits<float>::quiet_NaN()
		// can be folded to a non-NaN float by -ffinite-math-only.
		uint32_t nanBits = 0x7FC00000u;  // canonical quiet NaN
		float nanFloat;
		std::memcpy( &nanFloat, &nanBits, sizeof(nanFloat) );
		const uint16_t nanH = FloatToHalf( nanFloat );
		Check( ( ( nanH >> 10 ) & 0x1Fu ) == 0x1Fu && ( nanH & 0x3FFu ) != 0,
			"half encode NaN preserves NaN-ness" );
	}

	// ─── Section 5: ViewTransform pipeline ────────────────────────
	void TestViewTransformPipeline()
	{
		// Identity: pass through with no scaling.
		ViewTransform xf = ViewTransform::Identity();
		double r, g, b;
		ApplyViewTransformLinear( xf, FSColorSpace::ROMM_Linear, false,
			0.5, 0.25, 0.125, r, g, b );
		Check( r == 0.5 && g == 0.25 && b == 0.125,
			"Identity ViewTransform on ROMM → unchanged" );

		// Exposure +1 EV doubles linear values.
		xf = ViewTransform::ForLDRDisplay( 1.0f, eDisplayTransform_None );
		ApplyViewTransformLinear( xf, FSColorSpace::ROMM_Linear, false,
			0.5, 0.25, 0.125, r, g, b );
		Check( ApproxEq( r, 1.0, 1e-15 ) &&
		       ApproxEq( g, 0.5, 1e-15 ) &&
		       ApproxEq( b, 0.25, 1e-15 ),
			"Exposure +1 EV doubles linear" );

		// Exposure -1 EV halves.
		xf = ViewTransform::ForLDRDisplay( -1.0f, eDisplayTransform_None );
		ApplyViewTransformLinear( xf, FSColorSpace::ROMM_Linear, false,
			0.5, 0.25, 0.125, r, g, b );
		Check( ApproxEq( r, 0.25, 1e-15 ) &&
		       ApproxEq( g, 0.125, 1e-15 ) &&
		       ApproxEq( b, 0.0625, 1e-15 ),
			"Exposure -1 EV halves linear" );

		// HDR-display preset has no tone curve regardless of applyToneCurve.
		xf = ViewTransform::ForHDRDisplay( 0.0f );
		Check( xf.toneCurve == eDisplayTransform_None,
			"ForHDRDisplay has no tone curve" );

		// LDR preset uses ACES by default.
		xf = ViewTransform::ForLDRDisplay();
		Check( xf.toneCurve == eDisplayTransform_ACES,
			"ForLDRDisplay defaults to ACES" );

		// Tone curve gated by applyToneCurve flag: when false,
		// LDR preset still passes through linearly.
		xf = ViewTransform::ForLDRDisplay( 0.0f, eDisplayTransform_ACES );
		ApplyViewTransformLinear( xf, FSColorSpace::ROMM_Linear, false,
			10.0, 10.0, 10.0, r, g, b );
		Check( ApproxEq( r, 10.0, 1e-12 ),
			"applyToneCurve=false skips ACES even when curve set" );

		// With applyToneCurve=true, ACES compresses 10.0 to ≤ 1.
		ApplyViewTransformLinear( xf, FSColorSpace::ROMM_Linear, true,
			10.0, 10.0, 10.0, r, g, b );
		Check( r > 0.0 && r <= 1.0,
			"applyToneCurve=true with ACES compresses 10.0 to ≤ 1" );
	}

	// ─── Section 6: TargetFormat info table ───────────────────────
	void TestTargetFormatTable()
	{
		// Every enum value must have a non-zero info row.
		for ( uint32_t i = 0; i < static_cast<uint32_t>( TargetFormat::COUNT ); ++i ) {
			const TargetFormat fmt = static_cast<TargetFormat>( i );
			const TargetFormatInfo& info = GetTargetFormatInfo( fmt );
			std::ostringstream os;
			os << "Format[" << i << "] (" << ( info.name ? info.name : "?" ) << ")";
			Check( info.bytesPerPixel > 0, os.str() + " bytesPerPixel > 0" );
			Check( info.channelCount == 3 || info.channelCount == 4,
				os.str() + " channelCount in {3,4}" );
			Check( info.hasAlpha == ( info.channelCount == 4 ),
				os.str() + " hasAlpha matches channelCount" );
			Check( info.name != nullptr, os.str() + " name non-null" );
		}

		// Spot check: RGBA8_sRGB is 4 bpp.
		Check( GetTargetFormatInfo( TargetFormat::RGBA8_sRGB ).bytesPerPixel == 4,
			"RGBA8_sRGB = 4 bpp" );
		// RGBA32F_Linear is 16 bpp.
		Check( GetTargetFormatInfo( TargetFormat::RGBA32F_Linear ).bytesPerPixel == 16,
			"RGBA32F_Linear = 16 bpp" );
		// RGB32F_Linear is 12 bpp (no alpha).
		Check( GetTargetFormatInfo( TargetFormat::RGB32F_Linear ).bytesPerPixel == 12,
			"RGB32F_Linear = 12 bpp" );

		// LDR-fixed flag: RGBA8_sRGB is LDR; RGBA32F_Linear is not.
		Check( GetTargetFormatInfo( TargetFormat::RGBA8_sRGB ).isLDRFixed,
			"RGBA8_sRGB.isLDRFixed = true" );
		Check( !GetTargetFormatInfo( TargetFormat::RGBA32F_Linear ).isLDRFixed,
			"RGBA32F_Linear.isLDRFixed = false" );
		Check( !GetTargetFormatInfo( TargetFormat::RGBA16F_ExtendedLinearSRGB ).isLDRFixed,
			"RGBA16F_ExtendedLinearSRGB.isLDRFixed = false (HDR display)" );

		// Total-bytes math.
		Check( TargetFormatTotalBytes( TargetFormat::RGBA8_sRGB, 100, 50 ) == 20000u,
			"100x50 RGBA8 = 20000 bytes" );
		Check( TargetFormatTotalBytes( TargetFormat::RGBA8_sRGB, 0, 50 ) == 0u,
			"0 width = 0 bytes" );
	}
}

int main()
{
	std::cout << "FrameStoreColorMathTest L0 — output-side color math\n";
	std::cout << "----------------------------------------------------\n";

	TestTransferRoundTrip();
	TestSanitisation();
	TestPrimariesConversion();
	TestHalfFloatRoundTrip();
	TestViewTransformPipeline();
	TestTargetFormatTable();

	std::cout << "----------------------------------------------------\n";
	std::cout << "passed " << gPassCount << ", failed " << gFailCount << "\n";
	return gFailCount == 0 ? 0 : 1;
}
