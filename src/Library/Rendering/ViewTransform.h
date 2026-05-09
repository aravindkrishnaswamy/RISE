//////////////////////////////////////////////////////////////////////
//
//  ViewTransform.h - Per-frame "what to do to the values en route"
//  configuration consumed by FrameStore::Render.
//
//  Pipeline (see docs/FRAMESTORE_DESIGN.md §3.3):
//
//    ROMM-linear pixel
//      → exposure        (multiply by 2^exposureEV)        [Stage 1]
//      → white balance   (3x3 matrix in ROMM)               [Stage 2 — reserved]
//      → primaries       (ROMM → target via TargetFormat)   [Stage 3]
//      → tone curve      (iff TargetFormat.isLDRFixed)      [Stage 4]
//      → output transfer (sRGB / PQ / Linear via TargetFormat) [Stage 5]
//      → quantise        (target-format pixel layout)       [Stage 6]
//
//  ViewTransform owns Stages 1, 2, 4.  TargetFormat owns Stages 3, 5, 6.
//  The split is deliberate: ViewTransform is a value type the user
//  edits live (exposure slider, tone-curve picker), TargetFormat is a
//  near-static contract per output sink (Mac EDR layer, PNG file, etc.).
//
//  Stage 2 (white balance) is a placeholder — it's threaded through
//  the API as Matrix3 but treated as identity by Apply.  Wire-through
//  comes when a chromatic adaptation feature is added.
//
//  Author: design landing L0
//  License: see LICENSE.TXT
//
//////////////////////////////////////////////////////////////////////

#ifndef FRAMESTORE_VIEW_TRANSFORM_
#define FRAMESTORE_VIEW_TRANSFORM_

#include <cstdint>
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/Color/Color.h"
#include "DisplayTransform.h"        // re-uses DISPLAY_TRANSFORM curves
#include "FrameStoreColorSpace.h"    // FSColorSpace, TransferFunction

namespace RISE
{
	namespace FrameStoreOutput
	{
		// Re-export the existing DISPLAY_TRANSFORM enum under a
		// FrameStore-local alias.  Single source of truth: the
		// curve definitions in DisplayTransform.h are battle-tested
		// (used by FileRasterizerOutput); duplicating them here
		// would create drift risk.  ToneCurve names parallel the
		// DISPLAY_TRANSFORM values for clarity in API call sites.
		using ToneCurve = DISPLAY_TRANSFORM;

		//! Per-frame display configuration.  Cheap to copy (~80 bytes).
		//! Pass by const reference into FrameStore::Render.
		//!
		//! Default-constructed ViewTransform is the identity:
		//!   - exposure = 0 EV (no scaling)
		//!   - white balance = identity matrix (no chromatic adaptation)
		//!   - tone curve = None (no perceptual mapping)
		//!   - tone curve strength = 1.0 (full effect when not None)
		struct ViewTransform
		{
			//! Stage 1.  Multiplier on linear radiance is 2^exposureEV.
			//! 0 = no scaling; +1 = double brightness; -1 = half.
			float exposureEV = 0.0f;

			//! Stage 2 (reserved).  3x3 matrix applied in ROMM space
			//! BEFORE primaries conversion.  Identity by default;
			//! placeholder for future chromatic-adaptation support.
			//! Currently passed through Apply() but treated as identity
			//! when bit-equal to the identity matrix (cheap fast-path).
			Matrix3 whiteBalance;

			//! Stage 4.  Perceptual tone curve.  Skipped on HDR float
			//! targets regardless of this value (TargetFormat governs
			//! whether it actually runs — see ApplyViewTransformPipeline).
			ToneCurve toneCurve = eDisplayTransform_None;

			//! Strength multiplier for the tone curve [0, 1].  At 1.0
			//! the curve is fully applied; at 0.0 it's bypassed; intermediate
			//! values lerp between the curved and identity outputs.  Useful
			//! for "tone-mapping intensity" UI slider.
			float toneCurveStrength = 1.0f;

			ViewTransform()
				: exposureEV(0.0f)
				, whiteBalance()  // Matrix3 default is identity per Matrices.h:53
				, toneCurve(eDisplayTransform_None)
				, toneCurveStrength(1.0f)
			{}

			// ── presets ──

			//! Identity: no exposure, no tone curve, no white balance.
			//! Use for HDR archival outputs (EXR, .hdr) where the buffer
			//! must be scene-referred linear.
			static ViewTransform Identity();

			//! Standard LDR display: exposure on, ACES Filmic tone curve.
			//! Pair with an sRGB-transfer TargetFormat (RGBA8_sRGB etc.).
			static ViewTransform ForLDRDisplay(
				float exposureEV = 0.0f,
				ToneCurve tc = eDisplayTransform_ACES );

			//! HDR display (Mac EDR / Windows scRGB): exposure on,
			//! tone curve OFF.  Pair with RGBA16F_ExtendedLinearSRGB.
			//! The display compositor handles tone mapping to its
			//! native dynamic range; baking an SDR tone curve here
			//! would clip the highlights the user is trying to see.
			static ViewTransform ForHDRDisplay( float exposureEV = 0.0f );
		};

		// ─────────────────────────────────────────────────────────────
		// Pipeline application
		// ─────────────────────────────────────────────────────────────

		//! Apply Stages 1–4 of the pipeline (everything ViewTransform
		//! "owns") to one ROMM-linear pixel, returning the result in
		//! the target color space's primaries (still LINEAR — Stage 5
		//! transfer is applied separately).  This split lets bulk
		//! encoders cache the matrix and Stage-2 fast-path bit
		//! across many pixels.
		//!
		//! `applyToneCurve` should be set per the TargetFormat — i.e.
		//! TargetFormatInfo.isLDRFixed.  The function does NOT consult
		//! TargetFormat itself to keep this header free of TargetFormat.h.
		void ApplyViewTransformLinear(
			const ViewTransform&     xf,
			FSColorSpace             targetSpace,
			bool                     applyToneCurve,
			double                   linearROMM_R,
			double                   linearROMM_G,
			double                   linearROMM_B,
			double&                  outR,
			double&                  outG,
			double&                  outB );
	}
}

#endif
