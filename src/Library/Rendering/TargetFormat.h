//////////////////////////////////////////////////////////////////////
//
//  TargetFormat.h - Output pixel formats supported by FrameStore::Render.
//
//  Each TargetFormat value bundles three things:
//    - pixel layout    (channel order, bytes per pixel, alpha presence)
//    - color space     (primaries; FSColorSpace from FrameStoreColorSpace.h)
//    - transfer fn     (gamma encode; TransferFunction from same)
//
//  Plus a flag indicating whether the format is "LDR fixed" — i.e.
//  whether the ViewTransform's tone curve should be applied before
//  the transfer function.  HDR float targets skip the tone curve so
//  highlight values past 1.0 propagate to the display / file
//  unmolested; LDR fixed targets need it to compress highlights into
//  the [0, 1] quantisation range.
//
//  Adding a new format = one entry in the enum + one row in the info
//  table in TargetFormat.cpp.  The info table is the single source of
//  truth used by the encoding pipeline.
//
//  Author: design landing L0
//  License: see LICENSE.TXT
//
//////////////////////////////////////////////////////////////////////

#ifndef FRAMESTORE_TARGET_FORMAT_
#define FRAMESTORE_TARGET_FORMAT_

#include <cstdint>
#include <cstddef>
#include "FrameStoreColorSpace.h"

namespace RISE
{
	namespace FrameStoreOutput
	{
		//! Output pixel format.  Drives the entire encode pipeline:
		//! primaries matrix, transfer function, quantisation step,
		//! channel order, and bytes-per-pixel layout.  See
		//! GetTargetFormatInfo() for the per-format details.
		enum class TargetFormat : uint32_t
		{
			// 8-bit fixed (LDR display, common file formats)
			RGBA8_sRGB                  = 0, ///< PNG / Qt RGBA8888 / TGA standard
			RGB8_sRGB                   = 1, ///< 3-byte sRGB
			BGRA8_sRGB                  = 2, ///< Win32 DIB native byte order

			// 16-bit fixed (legacy GUI path; PNG-16; TIFF-16)
			RGBA16_sRGB                 = 3, ///< Today's IJobRasterizerOutput RGBA16 format

			// 16-bit float (HDR display)
			RGBA16F_ExtendedLinearSRGB  = 4, ///< Mac CAMetalLayer EDR / Windows scRGB
			RGBA16F_DisplayP3           = 5, ///< Mac wide-gamut linear (sRGB transfer)
			RGBA16F_BT2020_PQ           = 6, ///< HDR10 / Windows PQ swap chain
			RGBA16F_Linear              = 7, ///< Raw linear half-float (debug, intermediate)

			// 32-bit float (archival, debug).  Note the distinction
			// between *_Linear (sRGB / BT.709 D65 primaries with
			// linear transfer — the industry default for EXR /
			// .hdr archival, since most viewers expect Rec709) and
			// *_ROMM_Linear (RISE's native ROMM RGB primaries with
			// linear transfer — bit-identical to internal storage,
			// for callers that want to round-trip without losing
			// the chromatic-adaptation step).
			RGBA32F_Linear              = 8, ///< EXR linear (sRGB primaries)
			RGB32F_Linear               = 9, ///< .hdr file (Radiance), 3-channel (sRGB primaries)
			RGBA32F_ROMM_Linear         = 10,///< Bit-identical archival in ROMM primaries
			RGB32F_ROMM_Linear          = 11,///< Same, no alpha

			// Sentinel (must stay last; iteration helper)
			COUNT
		};

		//! Channel order for byte-packed targets.  Float-target
		//! channel order is always RGBA / RGB in memory.
		enum class ChannelOrder : uint32_t
		{
			RGBA = 0,  ///< [R, G, B, A]
			RGB  = 1,  ///< [R, G, B]   (no alpha)
			BGRA = 2   ///< [B, G, R, A] (Win32 DIB)
		};

		//! Per-format static descriptor.  Every TargetFormat has
		//! exactly one TargetFormatInfo; the table is built at
		//! static init in TargetFormat.cpp.
		struct TargetFormatInfo
		{
			uint32_t          bytesPerPixel;  ///< stride per pixel
			uint32_t          channelCount;   ///< 3 or 4
			bool              hasAlpha;       ///< true iff channelCount == 4
			bool              isFloat;        ///< float32 / half-float (true) vs fixed-point (false)
			bool              isHalfFloat;    ///< true iff binary16 (FloatToHalf required)
			bool              isLDRFixed;     ///< apply tone curve iff true
			ChannelOrder      channelOrder;
			FSColorSpace      colorSpace;     ///< primaries
			TransferFunction  transferFn;     ///< gamma encode
			const char*       name;           ///< for logging / UI ("RGBA8_sRGB")
		};

		//! Returns the static descriptor for a TargetFormat.  Pointer
		//! is to a static array entry; do not free.  An out-of-range
		//! cast returns the entry for RGBA8_sRGB defensively (the
		//! safest fallback — most-common consumer-facing format).
		const TargetFormatInfo& GetTargetFormatInfo( TargetFormat fmt );

		//! Convenience: total byte count for a contiguous tightly
		//! packed image of the given format.  Returns 0 if width or
		//! height is zero.  The product is computed in size_t to
		//! avoid 32-bit overflow on large images (e.g. 16K × 16K
		//! RGBA32F = 4 GB).
		inline size_t TargetFormatRowStride( TargetFormat fmt, size_t width )
		{
			return GetTargetFormatInfo( fmt ).bytesPerPixel * width;
		}

		inline size_t TargetFormatTotalBytes( TargetFormat fmt, size_t width, size_t height )
		{
			if ( width == 0 || height == 0 ) return 0;
			return TargetFormatRowStride( fmt, width ) * height;
		}
	}
}

#endif
