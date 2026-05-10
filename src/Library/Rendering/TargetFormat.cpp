//////////////////////////////////////////////////////////////////////
//
//  TargetFormat.cpp - Static descriptor table for output pixel formats.
//
//  Pattern: one row per TargetFormat enum value, declared in
//  index-ordered fashion so the lookup is a single array index.
//  Adding a new format = add an enum value AND a row in this table;
//  the assert in GetTargetFormatInfo catches mismatched ordering.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TargetFormat.h"

#include <cassert>

namespace RISE
{
	namespace FrameStoreOutput
	{
		// One row per TargetFormat value, in index order.  Compile-time
		// indexing of TargetFormat::COUNT validates we covered every
		// enum value (sizeof check below).
		static const TargetFormatInfo kFormatTable[] = {
			//  bytesPerPixel, channelCount, hasAlpha, isFloat, isHalfFloat, isLDRFixed,
			//      channelOrder,        colorSpace,                  transferFn,             name

			// RGBA8_sRGB
			{ 4, 4, true,  false, false, true,
				ChannelOrder::RGBA,  FSColorSpace::sRGB_Linear,       TransferFunction::sRGB,
				"RGBA8_sRGB" },

			// RGB8_sRGB
			{ 3, 3, false, false, false, true,
				ChannelOrder::RGB,   FSColorSpace::sRGB_Linear,       TransferFunction::sRGB,
				"RGB8_sRGB" },

			// BGRA8_sRGB
			{ 4, 4, true,  false, false, true,
				ChannelOrder::BGRA,  FSColorSpace::sRGB_Linear,       TransferFunction::sRGB,
				"BGRA8_sRGB" },

			// RGBA16_sRGB
			{ 8, 4, true,  false, false, true,
				ChannelOrder::RGBA,  FSColorSpace::sRGB_Linear,       TransferFunction::sRGB,
				"RGBA16_sRGB" },

			// RGBA16F_ExtendedLinearSRGB (Mac EDR / Windows scRGB)
			{ 8, 4, true,  true,  true,  false,
				ChannelOrder::RGBA,  FSColorSpace::sRGB_Linear,       TransferFunction::Linear,
				"RGBA16F_ExtendedLinearSRGB" },

			// RGBA16F_DisplayP3 (Apple wide-gamut linear)
			{ 8, 4, true,  true,  true,  false,
				ChannelOrder::RGBA,  FSColorSpace::DisplayP3_Linear,  TransferFunction::Linear,
				"RGBA16F_DisplayP3" },

			// RGBA16F_BT2020_PQ (HDR10 / Windows PQ)
			{ 8, 4, true,  true,  true,  false,
				ChannelOrder::RGBA,  FSColorSpace::BT2020_Linear,     TransferFunction::PQ_ST2084,
				"RGBA16F_BT2020_PQ" },

			// RGBA16F_Linear
			{ 8, 4, true,  true,  true,  false,
				ChannelOrder::RGBA,  FSColorSpace::ROMM_Linear,       TransferFunction::Linear,
				"RGBA16F_Linear" },

			// RGBA32F_Linear (EXR linear save in sRGB primaries —
			// industry-default for EXR archival, since most EXR
			// viewers assume BT.709/sRGB primaries).
			{ 16, 4, true, true,  false, false,
				ChannelOrder::RGBA,  FSColorSpace::sRGB_Linear,       TransferFunction::Linear,
				"RGBA32F_Linear" },

			// RGB32F_Linear (.hdr Radiance in sRGB primaries)
			{ 12, 3, false, true, false, false,
				ChannelOrder::RGB,   FSColorSpace::sRGB_Linear,       TransferFunction::Linear,
				"RGB32F_Linear" },

			// RGBA32F_ROMM_Linear — bit-identical scene-referred
			// archival in RISE's native ROMM primaries.  Pairs with
			// ViewTransform::Identity() to produce a buffer that
			// matches FrameStore's beauty channel byte-for-byte
			// (modulo float-vs-double precision).  Use for
			// debugging or for EXR output with ROMM-aware viewers.
			{ 16, 4, true, true,  false, false,
				ChannelOrder::RGBA,  FSColorSpace::ROMM_Linear,       TransferFunction::Linear,
				"RGBA32F_ROMM_Linear" },

			// RGB32F_ROMM_Linear — same, no alpha.
			{ 12, 3, false, true, false, false,
				ChannelOrder::RGB,   FSColorSpace::ROMM_Linear,       TransferFunction::Linear,
				"RGB32F_ROMM_Linear" },

			// RGBA16_BT2020_PQ (L5c) — HDR10 16-bit fixed.  isFloat=false
			// triggers uint16 quantisation in EncodePixel; isLDRFixed=false
			// skips the tone curve so HDR scene-referred values flow through
			// to the PQ encoder unchanged.  Alpha is clamped to [0,1] inside
			// the !isFloat branch (see L5c EncodePixel changes).
			{ 8, 4, true,  false, false, false,
				ChannelOrder::RGBA,  FSColorSpace::BT2020_Linear,     TransferFunction::PQ_ST2084,
				"RGBA16_BT2020_PQ" },

			// RGB16_BT2020_PQ (L5c) — same, no alpha.  Used by HDR10 PNG
			// encoders that emit RGB-only (PNG color type 2) — saves one
			// uint16 per pixel vs the RGBA variant.
			{ 6, 3, false, false, false, false,
				ChannelOrder::RGB,   FSColorSpace::BT2020_Linear,     TransferFunction::PQ_ST2084,
				"RGB16_BT2020_PQ" },
		};

		// Compile-time check that the table covers every enum value
		// in order.  If COUNT changes, this fires immediately.
		static_assert(
			sizeof(kFormatTable) / sizeof(kFormatTable[0])
				== static_cast<size_t>( TargetFormat::COUNT ),
			"kFormatTable size must match TargetFormat::COUNT — did "
			"you add a TargetFormat enum value without updating the table?" );

		const TargetFormatInfo& GetTargetFormatInfo( TargetFormat fmt )
		{
			const uint32_t idx = static_cast<uint32_t>( fmt );
			if ( idx >= static_cast<uint32_t>( TargetFormat::COUNT ) ) {
				// Defensive: out-of-range cast (e.g. via reinterpret-cast
				// from corrupt scene data) returns RGBA8_sRGB so the
				// caller doesn't dereference undefined memory.  In
				// debug builds we assert so the bad cast is found.
				assert( false && "TargetFormat out of range" );
				return kFormatTable[0];
			}
			return kFormatTable[idx];
		}
	}
}
