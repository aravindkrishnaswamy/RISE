//////////////////////////////////////////////////////////////////////
//
//  FrameEncoders.cpp - Implementation of the 7 concrete IFrameEncoders
//  + the FrameEncoderRegistry singleton.
//
//  See FrameEncoders.h for the design context.  Per-format byte
//  output is delegated to the existing IRasterImageWriter classes
//  (PNGWriter, EXRWriter, etc.) via the RISE_API_Create*Writer
//  factories — same writers FileRasterizerOutput uses, so byte-
//  identity holds modulo the DisplayTransformWriter wrap decision
//  (which we mirror exactly).
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FrameEncoders.h"
#include "DisplayTransformWriter.h"
#include "FrameStore.h"

#include "../Interfaces/ILog.h"
#include "../Interfaces/IWriteBuffer.h"
#include "../RISE_API.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <vector>

// L5c — libpng for HDR10 PNG encoder.
#ifndef NO_PNG_SUPPORT
#include <png.h>
#include <zlib.h>  // Z_BEST_COMPRESSION
#endif
#include <cctype>

using namespace RISE;
using namespace RISE::Implementation;
using namespace RISE::FrameStoreOutput;

namespace RISE
{
	namespace Implementation
	{

		// ─────────────────────────────────────────────────────────────
		// FrameEncoderBase::Encode — shared "construct, wrap, dump"
		// ─────────────────────────────────────────────────────────────
		void FrameEncoderBase::Encode(
			const FrameStore& store,
			IWriteBuffer&     dst,
			const EncodeOpts& opts )
		{
			IRasterImageWriter* pWriter = CreateWriter( dst, opts );
			if ( !pWriter ) {
				GlobalLog()->PrintEx( eLog_Error,
					"FrameEncoder[%s]: writer factory returned null",
					FormatName().c_str() );
				return;
			}

			// Decide whether to wrap in DisplayTransformWriter.
			// Mirrors the gate in FileRasterizerOutput.cpp:232 —
			// LDR-only AND non-default exposure or tone curve.
			//
			// Total EV is the sum of the caller-supplied static
			// exposure (from opts.viewTransform.exposureEV — UI
			// slider, scene-declared exposure_compensation, etc.)
			// and the per-frame camera-side EV (from
			// store.Meta().cameraExposureEV — set by the rasterizer
			// at frame start via SetCameraExposureCompensationEV).
			// This matches FileRasterizerOutput.cpp:231.  See L2
			// adversarial review HIGH-2.
			IRasterImageWriter* pEffective = pWriter;
			DisplayTransformWriter* pDtw = nullptr;
			if ( !IsHDRFormat() ) {
				const Scalar staticEV =
					static_cast<Scalar>( opts.viewTransform.exposureEV );
				const Scalar cameraEV =
					static_cast<Scalar>( store.Meta().cameraExposureEV );
				const Scalar totalEV = staticEV + cameraEV;
				const bool useDt =
					   ( opts.viewTransform.toneCurve != eDisplayTransform_None )
					|| ( totalEV != Scalar( 0 ) );
				if ( useDt ) {
					pDtw = new DisplayTransformWriter(
						*pWriter, totalEV, opts.viewTransform.toneCurve );
					GlobalLog()->PrintNew(
						pDtw, __FILE__, __LINE__, "DisplayTransformWriter" );
					pEffective = pDtw;
				}
			}

			// Drive the writer through the FrameStore's
			// IRasterImage shim.  BeautyRasterImageView::DumpImage
			// walks pixels in the same row-major order as
			// RasterImage_Template::DumpImage, so the bytes the
			// writer emits are byte-identical to the legacy path
			// for the same input.
			store.AsBeautyRasterImage().DumpImage( pEffective );

			// Release wrapper before inner writer (match
			// FileRasterizerOutput.cpp:268-277 ordering).
			if ( pDtw ) {
				GlobalLog()->PrintDelete( pDtw, __FILE__, __LINE__ );
				safe_release( pDtw );
			}
			GlobalLog()->PrintDelete( pWriter, __FILE__, __LINE__ );
			safe_release( pWriter );
		}

		// ─────────────────────────────────────────────────────────────
		// Per-format CreateWriter overrides
		// ─────────────────────────────────────────────────────────────
		IRasterImageWriter* PNGFrameEncoder::CreateWriter(
			IWriteBuffer& dst, const EncodeOpts& opts ) const
		{
			IRasterImageWriter* w = nullptr;
			RISE_API_CreatePNGWriter( &w, dst, opts.bpp, opts.colorSpace );
			return w;
		}

		IRasterImageWriter* EXRFrameEncoder::CreateWriter(
			IWriteBuffer& dst, const EncodeOpts& opts ) const
		{
			IRasterImageWriter* w = nullptr;
			// bpp >= 32 selects 32-bit FLOAT EXR channels; lower (the
			// default 8 / the common 16) keep the historical half/FP16
			// output byte-for-byte.  Half caps at 65504, which clamps
			// legitimate bright HDR pixels (caustic / specular fireflies)
			// to +Inf on write — float preserves the full linear range.
			const bool write_float = ( opts.bpp >= 32 );
			RISE_API_CreateEXRWriter( &w, dst,
				opts.colorSpace, opts.exrCompression, opts.exrWithAlpha, write_float );
			return w;
		}

		IRasterImageWriter* TIFFFrameEncoder::CreateWriter(
			IWriteBuffer& dst, const EncodeOpts& opts ) const
		{
			IRasterImageWriter* w = nullptr;
			RISE_API_CreateTIFFWriter( &w, dst, opts.colorSpace );
			return w;
		}

		IRasterImageWriter* HDRFrameEncoder::CreateWriter(
			IWriteBuffer& dst, const EncodeOpts& opts ) const
		{
			IRasterImageWriter* w = nullptr;
			RISE_API_CreateHDRWriter( &w, dst, opts.colorSpace );
			return w;
		}

		IRasterImageWriter* RGBEAFrameEncoder::CreateWriter(
			IWriteBuffer& dst, const EncodeOpts& /*opts*/ ) const
		{
			// RGBEA factory does NOT take a color_space — see
			// RISE_API.h:1992.  RGBEA always writes Radiance-RGBE
			// linear radiance.
			IRasterImageWriter* w = nullptr;
			RISE_API_CreateRGBEAWriter( &w, dst );
			return w;
		}

		IRasterImageWriter* TGAFrameEncoder::CreateWriter(
			IWriteBuffer& dst, const EncodeOpts& opts ) const
		{
			IRasterImageWriter* w = nullptr;
			RISE_API_CreateTGAWriter( &w, dst, opts.colorSpace );
			return w;
		}

		IRasterImageWriter* PPMFrameEncoder::CreateWriter(
			IWriteBuffer& dst, const EncodeOpts& opts ) const
		{
			IRasterImageWriter* w = nullptr;
			RISE_API_CreatePPMWriter( &w, dst, opts.colorSpace );
			return w;
		}

		// ─────────────────────────────────────────────────────────────
		// L5c — HDR10 PNG encoder (BT.2020 + PQ + cICP)
		// ─────────────────────────────────────────────────────────────
#ifndef NO_PNG_SUPPORT
		namespace
		{
			// libpng I/O glue — write callback that pumps `length`
			// bytes into an `IWriteBuffer*`.  Same pattern used by
			// the legacy `PNGWriter::EndWrite` (see
			// RasterImages/PNGWriter.cpp:202 `png_write_data`).
			void HDR10PNG_WriteCallback( png_structp png_ptr, png_bytep data, png_size_t length )
			{
				IWriteBuffer* buf = static_cast<IWriteBuffer*>( png_get_io_ptr( png_ptr ) );
				if ( !buf ) return;
				buf->setBytes( static_cast<const void*>( data ), static_cast<unsigned int>( length ) );
			}
			void HDR10PNG_FlushCallback( png_structp /*png_ptr*/ )
			{
				// IWriteBuffer flushes implicitly on close — no-op here.
			}
		}
#endif  // NO_PNG_SUPPORT

		void HDR10PNGFrameEncoder::Encode(
			const FrameStore&    store,
			IWriteBuffer&        dst,
			const EncodeOpts&    opts )
		{
#ifdef NO_PNG_SUPPORT
			(void)store; (void)dst; (void)opts;
			GlobalLog()->PrintSourceError(
				"HDR10PNGFrameEncoder::Encode: NO_PNG_SUPPORT — "
				"libpng not compiled in; cannot emit HDR10 PNG",
				__FILE__, __LINE__ );
			return;
#else
			// L5c review P1 — host-endian guard.  `png_set_swap`
			// below assumes little-endian host (the only platforms
			// RISE targets today: x86_64, arm64).  On big-endian
			// hosts (theoretical PowerPC) the swap would
			// double-swap to little-endian and produce malformed
			// PNG.  Hard-fail at compile time rather than emit
			// silently broken output.
			static_assert(
#ifdef __BYTE_ORDER__
				__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
#else
				// Compilers without __BYTE_ORDER__: assume little-endian
				// (the only practical targets) but the pre-processor
				// runs the static_assert as `true` in that branch.
				true,
#endif
				"HDR10 PNG encoder assumes little-endian host (uses png_set_swap "
				"to convert host-endian uint16 samples to PNG's big-endian)" );

			const unsigned int W = static_cast<unsigned int>( store.Width() );
			const unsigned int H = static_cast<unsigned int>( store.Height() );
			if ( W == 0 || H == 0 ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"HDR10PNGFrameEncoder::Encode: empty FrameStore (%ux%u); skipping",
					W, H );
				return;
			}

			// Step 1 — allocate a uint16 RGB buffer and render the
			// FrameStore through the L5c RGB16_BT2020_PQ pipeline.
			// 3-channel (no alpha) keeps the file size minimal and
			// matches the typical HDR10 PNG profile (PNG color type 2).
			const size_t rowStride = static_cast<size_t>( W ) * 3u * sizeof( uint16_t );
			std::vector<uint8_t> bytes( rowStride * H );

			Rect roi( 0, 0, H, W );  // top, left, bottom, right (inclusive)
			store.Render(
				bytes.data(),
				rowStride,
				roi,
				FrameStoreOutput::TargetFormat::RGB16_BT2020_PQ,
				opts.viewTransform );

			// Step 2 — initialise libpng for write.
			png_structp png_ptr = png_create_write_struct( PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr );
			if ( !png_ptr ) {
				GlobalLog()->PrintSourceError(
					"HDR10PNGFrameEncoder::Encode: png_create_write_struct failed",
					__FILE__, __LINE__ );
				return;
			}
			png_infop info_ptr = png_create_info_struct( png_ptr );
			if ( !info_ptr ) {
				png_destroy_write_struct( &png_ptr, nullptr );
				GlobalLog()->PrintSourceError(
					"HDR10PNGFrameEncoder::Encode: png_create_info_struct failed",
					__FILE__, __LINE__ );
				return;
			}

			// libpng uses setjmp/longjmp for error reporting.  If any
			// libpng call fails inside the protected region below,
			// control returns here and we clean up.
			if ( setjmp( png_jmpbuf( png_ptr ) ) ) {
				png_destroy_write_struct( &png_ptr, &info_ptr );
				GlobalLog()->PrintSourceError(
					"HDR10PNGFrameEncoder::Encode: libpng error during write",
					__FILE__, __LINE__ );
				return;
			}

			png_set_write_fn( png_ptr, &dst, HDR10PNG_WriteCallback, HDR10PNG_FlushCallback );

			// Filtering + compression — match the legacy PNGWriter
			// settings for byte-shape consistency with the SDR PNG
			// path (PNG_NO_FILTERS + Z_BEST_COMPRESSION).
			png_set_filter( png_ptr, 0, PNG_NO_FILTERS );
			png_set_compression_level( png_ptr, Z_BEST_COMPRESSION );

			// PNG IHDR — 16-bit RGB (no alpha, matches RGB16_BT2020_PQ).
			png_set_IHDR(
				png_ptr, info_ptr,
				W, H,
				/*bit_depth=*/   16,
				/*color_type=*/  PNG_COLOR_TYPE_RGB,
				PNG_INTERLACE_NONE,
				PNG_COMPRESSION_TYPE_DEFAULT,
				PNG_FILTER_TYPE_DEFAULT );

			// Step 3 — write info header THEN write the cICP chunk.
			// PNG spec puts cICP after IHDR and before any IDAT;
			// libpng's `png_write_info` outputs IHDR + any registered
			// ancillary chunks.  We use `png_write_chunk` AFTER
			// `png_write_info` to emit cICP — that places it after
			// the IHDR and any auto-emitted ancillary chunks but
			// BEFORE the IDAT pixel data, which is spec-compliant.
			png_write_info( png_ptr, info_ptr );

			// cICP (Coding-Independent Code Points) — 4 bytes:
			//   colorPrimaries          = 9   (BT.2020)
			//   transferCharacteristics = 16  (SMPTE ST.2084 / PQ)
			//   matrixCoefficients      = 0   (Identity / RGB)
			//   videoFullRangeFlag      = 1   (full range, 0..65535)
			//
			// PNG cICP chunk is documented in PNG 3rd Edition (W3C
			// 2024); cICP values are CICP H.273 / ITU-T T.273
			// registered code points.  Modern HDR-aware viewers
			// (Chrome 100+, Edge 100+, macOS Preview 14+) honour
			// this and tone-map the content to the display's HDR
			// capabilities.
			// Android NDK bundles an older libpng whose
			// `png_write_chunk` declares `chunk_name` / `data` as
			// non-const `png_bytep` (modern libpng uses
			// `png_const_bytep`).  Declaring the locals as plain
			// non-const arrays satisfies both APIs.
			png_byte cICPType[5]  = { 'c', 'I', 'C', 'P', 0 };
			png_byte cICP_data[4] = { 9, 16, 0, 1 };
			png_write_chunk( png_ptr, cICPType, cICP_data, sizeof( cICP_data ) );

			// Endian fix-up — libpng expects 16-bit samples in
			// network (big-endian) byte order.  Our `Render` produced
			// the data in HOST endianness.  On little-endian hosts
			// (the only platforms RISE targets today), swap.
			png_set_swap( png_ptr );

			// Step 4 — write rows.
			for ( unsigned int y = 0; y < H; ++y ) {
				png_bytep rowp = bytes.data() + static_cast<size_t>( y ) * rowStride;
				png_write_row( png_ptr, rowp );
			}

			png_write_end( png_ptr, info_ptr );
			png_destroy_write_struct( &png_ptr, &info_ptr );
#endif
		}

		// ─────────────────────────────────────────────────────────────
		// FrameEncoderRegistry
		// ─────────────────────────────────────────────────────────────

		// Case-insensitive ASCII compare (helper for lookup methods).
		static bool IEqualsASCII( const std::string& a, const std::string& b )
		{
			if ( a.size() != b.size() ) return false;
			for ( size_t i = 0; i < a.size(); ++i ) {
				const unsigned char ca = static_cast<unsigned char>( a[i] );
				const unsigned char cb = static_cast<unsigned char>( b[i] );
				if ( std::tolower( ca ) != std::tolower( cb ) ) return false;
			}
			return true;
		}

		// Strip a leading "." from an extension if present.
		static std::string StripDot( const std::string& s )
		{
			return ( !s.empty() && s[0] == '.' ) ? s.substr( 1 ) : s;
		}

		FrameEncoderRegistry& FrameEncoderRegistry::Get()
		{
			// Meyers singleton — thread-safe initialization
			// guaranteed by C++11 §6.7p4 (function-local statics).
			static FrameEncoderRegistry instance;
			return instance;
		}

		FrameEncoderRegistry::FrameEncoderRegistry()
		{
			RegisterBuiltins();
		}

		FrameEncoderRegistry::~FrameEncoderRegistry()
		{
			std::lock_guard<std::mutex> lock( mutex_ );
			for ( IFrameEncoder* enc : encoders_ ) {
				safe_release( enc );
			}
			encoders_.clear();
		}

		void FrameEncoderRegistry::RegisterBuiltins()
		{
			// Register order is iteration order returned by All().
			Register( new PNGFrameEncoder() );
			Register( new EXRFrameEncoder() );
			Register( new TIFFFrameEncoder() );
			Register( new HDRFrameEncoder() );
			Register( new RGBEAFrameEncoder() );
			Register( new TGAFrameEncoder() );
			Register( new PPMFrameEncoder() );
			// L5c — HDR10 PNG encoder.  Same .png extension as PNG;
			// users select via FormatName "HDR10_PNG" rather than
			// extension lookup (ByExtension("png") still returns the
			// SDR PNG encoder, which is the safer default).
			Register( new HDR10PNGFrameEncoder() );
		}

		void FrameEncoderRegistry::Register( IFrameEncoder* encoder )
		{
			if ( !encoder ) return;

			std::lock_guard<std::mutex> lock( mutex_ );

			// Replace existing entry with the same FormatName.
			for ( auto it = encoders_.begin(); it != encoders_.end(); ++it ) {
				if ( IEqualsASCII( ( *it )->FormatName(), encoder->FormatName() ) ) {
					safe_release( *it );
					*it = encoder;
					return;
				}
			}
			encoders_.push_back( encoder );
		}

		IFrameEncoder* FrameEncoderRegistry::ByFormatName( const std::string& name ) const
		{
			std::lock_guard<std::mutex> lock( mutex_ );
			for ( IFrameEncoder* enc : encoders_ ) {
				if ( IEqualsASCII( enc->FormatName(), name ) ) {
					return enc;
				}
			}
			return nullptr;
		}

		IFrameEncoder* FrameEncoderRegistry::ByExtension( const std::string& ext ) const
		{
			const std::string needle = StripDot( ext );
			std::lock_guard<std::mutex> lock( mutex_ );
			for ( IFrameEncoder* enc : encoders_ ) {
				for ( const std::string& e : enc->Extensions() ) {
					if ( IEqualsASCII( e, needle ) ) {
						return enc;
					}
				}
			}
			return nullptr;
		}

		std::vector<IFrameEncoder*> FrameEncoderRegistry::All() const
		{
			std::lock_guard<std::mutex> lock( mutex_ );
			return encoders_;
		}

	} // namespace Implementation
} // namespace RISE
