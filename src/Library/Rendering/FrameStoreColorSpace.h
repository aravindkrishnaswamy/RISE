//////////////////////////////////////////////////////////////////////
//
//  FrameStoreColorSpace.h - Output-side color space and transfer
//  function definitions used by the FrameStore Render() readback
//  pipeline.
//
//  This is distinct from the existing Utilities/Color/Color.h
//  COLOR_SPACE enum (which describes the four spaces RISE has
//  historically supported for writers): the FrameStore output system
//  needs Display P3, BT.2020, and HDR transfer functions (PQ, HLG)
//  that the legacy enum doesn't cover.  Rather than extending the
//  legacy enum and risk silent semantic drift in old call sites, this
//  header defines its own enums with explicit FS prefixing.
//
//  Pipeline this header serves (see docs/FRAMESTORE_DESIGN.md §3.3):
//
//      ROMM linear in
//        → exposure (multiply by 2^EV)         [ViewTransform stage 1]
//        → 3x3 matrix (ROMM → target primaries) [this header: matrices]
//        → tone curve (iff target is LDR fixed) [ViewTransform stage 3]
//        → output transfer (sRGB / PQ / etc.)   [this header: ApplyTransfer]
//        → quantise into target pixel layout    [TargetFormat]
//
//  Author: design landing L0
//  License: see LICENSE.TXT
//
//////////////////////////////////////////////////////////////////////

#ifndef FRAMESTORE_COLOR_SPACE_
#define FRAMESTORE_COLOR_SPACE_

#include <cstdint>
#include "../Utilities/Color/Color.h"

namespace RISE
{
	namespace FrameStoreOutput
	{
		//! Output-side color spaces supported by the FrameStore Render
		//! pipeline.  Each value implies (primaries, white point) but
		//! NOT a transfer function — that is a separate axis (TransferFunction).
		enum class FSColorSpace : uint32_t
		{
			ROMM_Linear        = 0,  ///< ProPhoto / ROMM RGB primaries, D50, linear (RISEPel native)
			sRGB_Linear        = 1,  ///< BT.709 primaries, D65, linear
			DisplayP3_Linear   = 2,  ///< DCI-P3 primaries, D65, linear (Apple Display P3)
			BT2020_Linear      = 3   ///< BT.2020 primaries, D65, linear
		};

		//! Output-side transfer functions (linear → encoded).  Applied
		//! AFTER the primaries matrix.  Tone-curve compression (ACES
		//! etc.) is a separate ViewTransform stage that happens BEFORE
		//! the transfer; the transfer here is the gamma-encoding step,
		//! not a perceptual tone-mapper.
		enum class TransferFunction : uint32_t
		{
			Linear   = 0,   ///< Identity.  HDR archival paths and EDR display.
			sRGB     = 1,   ///< IEC 61966-2-1 piecewise sRGB transfer.
			PQ_ST2084 = 2,  ///< SMPTE ST.2084 Perceptual Quantizer (HDR10).
			HLG      = 3,   ///< ARIB STD-B67 / BT.2100 Hybrid Log-Gamma.
			Gamma22  = 4    ///< Pure 1/2.2 gamma (legacy NTSC / PAL displays).
		};

		// ─────────────────────────────────────────────────────────────
		// Transfer functions (scalar, applied per channel)
		// ─────────────────────────────────────────────────────────────

		//! Apply a transfer function to a single linear value, returning
		//! the encoded value.  Negative inputs and non-finite values
		//! collapse to 0 (matches DisplayTransform.h::Sanitise).
		//! For LDR transfers (sRGB, Gamma22) the input is expected in
		//! [0, 1].  For HDR transfers (PQ, HLG) the input is interpreted
		//! per the convention in this header (see ApplyPQTransfer).
		double ApplyTransfer( TransferFunction fn, double linearValue );

		//! Inverse of ApplyTransfer (encoded → linear).  Provided for
		//! round-trip tests and future "decode then re-encode" flows.
		double ApplyTransferInverse( TransferFunction fn, double encoded );

		//! sRGB transfer (IEC 61966-2-1).  Uses the existing
		//! ColorUtils::SRGBTransferFunction internally for byte-identical
		//! parity with the rest of the codebase.
		double ApplySRGBTransfer( double linear );

		//! Pure 1/gamma encode.  Sanitises negatives / non-finite to 0.
		double ApplyGammaTransfer( double linear, double gamma );

		//! SMPTE ST.2084 (PQ) encode.  Convention used here:
		//!   input `linearScene` is scene-referred linear, where
		//!   1.0 = SDR diffuse white reference (100 nits).
		//! The function rescales by 0.01 so that 1.0 scene-referred
		//! lands at 100/10000 = 0.01 in the PQ "1.0 = 10000 nits"
		//! domain, then applies the ST.2084 OETF.  Out-of-range values
		//! are clamped at 1.0 (10000 nits peak); this is correct for
		//! direct-to-display encoding where the OS won't tone-map past
		//! the PQ ceiling.  The rescale factor is documented in
		//! docs/FRAMESTORE_DESIGN.md §9 ("Windows HDR specifics, Path B").
		double ApplyPQTransfer( double linearScene );

		//! ARIB STD-B67 / BT.2100 HLG OETF.  Input is scene-referred
		//! linear (any positive value); output is the HLG-encoded
		//! signal value.  Per BT.2100 Table 5 the OETF is defined for
		//! E in [0, 1] producing E' in [0, 1], with the curve naturally
		//! continuing past 1.0 mathematically.  This implementation
		//! does NOT clamp at the top; LDR-fixed HLG targets must add
		//! their own quantisation clamp (none currently do — no
		//! TargetFormat currently uses HLG).  Inverse via
		//! ApplyTransferInverse is mathematically exact for any
		//! positive input.
		double ApplyHLGTransfer( double linearScene );

		// ─────────────────────────────────────────────────────────────
		// Primaries conversion matrices (3x3, row-major)
		// ─────────────────────────────────────────────────────────────

		//! Returns a row-major 3x3 matrix M for the SECOND stage of the
		//! ROMM → target conversion (sRGB linear → target primaries).
		//! For sRGB and ROMM targets this is the identity; for P3 and
		//! BT.2020 it is the published industry M_sRGB→target matrix.
		//! The full ROMM → target chain runs ROMM → sRGB linear via
		//! ColorUtils::ROMMRGBtoRec709RGB first; ConvertROMMToTargetPrimaries()
		//! does the chain in one call.  Pointer is to a static array;
		//! do not free.
		const double* GetROMMToTargetMatrix( FSColorSpace target );

		//! Apply a 3x3 matrix (row-major) to an RGB triple.  Out is
		//! aliased-safe with in.
		void ApplyMatrix3x3(
			const double* mat,        ///< row-major 9-element matrix
			double inR, double inG, double inB,
			double& outR, double& outG, double& outB );

		//! Convenience: apply ROMM-linear → target color space's linear
		//! primaries.  No transfer function applied.
		void ConvertROMMToTargetPrimaries(
			FSColorSpace target,
			double rommR, double rommG, double rommB,
			double& outR, double& outG, double& outB );

		// ─────────────────────────────────────────────────────────────
		// Half-float (IEEE 754 binary16) helpers
		// ─────────────────────────────────────────────────────────────

		//! Encode a single-precision float into IEEE 754 binary16
		//! (half-precision), returning the bit pattern as uint16_t.
		//! Used by RGBA16F target formats.  Round-to-nearest-even
		//! per IEEE 754; subnormals encoded directly; infinities and
		//! NaNs preserved.  This is a portable software encoder; we
		//! don't depend on _Float16 / __fp16 / Imath::half so the
		//! codebase compiles on platforms whose toolchains lack them.
		uint16_t FloatToHalf( float f );

		//! Decode binary16 → float.  Inverse of FloatToHalf.
		float HalfToFloat( uint16_t h );

		//! Encode the FLOAT-bits (uint32_t representation) of a
		//! single-precision value into binary16, with no float-typed
		//! parameter passing involved.  Identical math to FloatToHalf
		//! but skips the `float` type round-trip — important under
		//! `-ffast-math` (`-ffinite-math-only`), which lets the
		//! compiler assume any `float` value is finite and may then
		//! constant-fold NaN/Inf bit-pattern checks to false even for
		//! genuine NaN inputs.  Use this from contexts that must
		//! correctly round-trip NaN/Inf bit patterns end-to-end.
		uint16_t FloatBitsToHalf( uint32_t floatBits );

		//! Decode binary16 → float-bits (uint32_t).  Counterpart to
		//! FloatBitsToHalf; same fast-math motivation.
		uint32_t HalfToFloatBits( uint16_t h );
	}
}

#endif
