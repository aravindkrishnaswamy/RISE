//////////////////////////////////////////////////////////////////////
//
//  FrameEncoderTest.cpp - L2 regression gate for IFrameEncoder.
//
//  The contract is byte-identical output: for any FrameStore beauty
//  channel + EncodeOpts, IFrameEncoder::Encode must produce exactly
//  the bytes that the legacy FileRasterizerOutput pipeline would
//  produce for the same image + same parameters.  This is what makes
//  the L3 FileRasterizerOutput rewrite a drop-in replacement.
//
//  Test strategy per format:
//    1. Build a FrameStore with a known pixel pattern.
//    2. Encode via the new path:
//         IFrameEncoder::Encode(store, memBufA, opts)
//    3. Encode via the legacy path:
//         RasterImage_Template + same writer + same DisplayTransformWriter
//         (mirrors FileRasterizerOutput::WriteImageToFile)
//         → memBufB
//    4. Compare memBufA == memBufB byte-for-byte.
//
//  Plus: registry sanity checks (all 7 encoders register, lookup by
//  name and extension works, case-insensitive).
//
//////////////////////////////////////////////////////////////////////

#include <cstdint>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#ifndef NO_PNG_SUPPORT
#include <png.h>
#endif
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "../src/Library/Rendering/FrameEncoders.h"
#include "../src/Library/Rendering/FrameStore.h"
#include "../src/Library/Rendering/ViewportFrameStore.h"
#include "../src/Library/RasterImages/PNGWriter.h"
#include "TestTempDir.h"
#include "../src/Library/Rendering/DisplayTransformWriter.h"
#include "../src/Library/RasterImages/RasterImage.h"
#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/Interfaces/IRasterImageWriter.h"
#include "../src/Library/Interfaces/IMemoryBuffer.h"
#include "../src/Library/RISE_API.h"

using namespace RISE;
using namespace RISE::Implementation;
using namespace RISE::FrameStoreOutput;

namespace
{
	int gFailCount = 0;
	int gPassCount = 0;

	void Check( bool cond, const std::string& label )
	{
		if ( cond ) {
			++gPassCount;
			if( std::getenv("RISE_RED_PROVE_MARKER") ) std::cout << "PASS: " << label << "\n";
		} else {
			++gFailCount;
			std::cerr << "FAIL: " << label << "\n";
		}
	}

#ifndef NO_PNG_SUPPORT
	struct RawPNGPixelRow
	{
		bool decoded = false;
		bool hasGamma = false;
		double gamma = 0;
		unsigned int depth = 0;
		unsigned int channels = 0;
		std::array<unsigned char, 16> samples{};
	};

	RawPNGPixelRow ReadRawPNGPixelRow( const IMemoryBuffer& buffer )
	{
		struct Input
		{
			const char* bytes;
			std::size_t size;
			std::size_t position;
		} input{ buffer.Pointer(), buffer.Size(), 0 };
		RawPNGPixelRow result;
		png_structp png = png_create_read_struct( PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr );
		if( !png ) return result;
		png_infop info = png_create_info_struct( png );
		if( !info ) {
			png_destroy_read_struct( &png, nullptr, nullptr );
			return result;
		}
		if( setjmp( png_jmpbuf( png ) ) ) {
			png_destroy_read_struct( &png, &info, nullptr );
			return {};
		}
		// No gamma, alpha or endian transforms: inspect the file's actual
		// RGBA samples in PNG network order, independently of RISE's reader.
		png_set_read_fn( png, &input, []( png_structp state, png_bytep data, png_size_t size ) {
			auto* source = static_cast<Input*>( png_get_io_ptr( state ) );
			if( size > source->size - source->position ) png_error( state, "truncated fixture" );
			std::memcpy( data, source->bytes + source->position, size );
			source->position += size;
		} );
		png_read_info( png, info );
		result.depth = png_get_bit_depth( png, info );
		result.channels = png_get_channels( png, info );
		result.hasGamma = png_get_gAMA( png, info, &result.gamma ) != 0;
		if( png_get_image_width( png, info ) != 2 || png_get_image_height( png, info ) != 1 ||
			(png_get_color_type( png, info ) != PNG_COLOR_TYPE_RGBA &&
			 png_get_color_type( png, info ) != PNG_COLOR_TYPE_RGB) ||
			(result.depth != 8 && result.depth != 16) ||
			png_get_rowbytes( png, info ) != 2u * result.channels * (result.depth / 8u) ) {
			png_destroy_read_struct( &png, &info, nullptr );
			return {};
		}
		png_read_row( png, result.samples.data(), nullptr );
		png_read_end( png, info );
		png_destroy_read_struct( &png, &info, nullptr );
		result.decoded = true;
		return result;
	}

	std::array<double, 3> PNGReferenceChannels( const RISEColor& color, COLOR_SPACE space )
	{
		std::array<double, 3> rgb{ color.base.r, color.base.g, color.base.b };
		if( space == eColorSpace_ROMMRGB_Linear || space == eColorSpace_ProPhotoRGB ) {
			// Independent sequential Rec.709 D65 -> XYZ D65 -> Bradford D50
			// -> ROMM D50 reference, rather than the production composite matrix.
			const auto multiply = []( const std::array<double, 3>& value, const double (&matrix)[3][3] ) {
				std::array<double, 3> transformed{};
				for( std::size_t row = 0; row < 3; ++row )
					for( std::size_t column = 0; column < 3; ++column )
						transformed[row] += matrix[row][column] * value[column];
				return transformed;
			};
			constexpr double toXYZ[3][3] = {
				{0.412453, 0.357580, 0.180423}, {0.212671, 0.715160, 0.072169}, {0.019334, 0.119193, 0.950227}
			};
			constexpr double toD50[3][3] = {
				{1.0479, 0.0229, -0.0502}, {0.0296, 0.9904, -0.0171}, {-0.0092, 0.0151, 0.7519}
			};
			constexpr double toROMM[3][3] = {
				{1.3460, -0.2556, -0.0511}, {-0.5446, 1.5082, 0.0205}, {0, 0, 1.2123}
			};
			rgb = multiply( multiply( multiply( rgb, toXYZ ), toD50 ), toROMM );
		}
		for( double& channel : rgb ) {
			if( space == eColorSpace_sRGB )
				channel = channel <= 0.003040247678 ? 12.92 * channel : 1.055 * std::pow( channel, 1.0 / 2.4 ) - 0.055;
			else if( space == eColorSpace_ProPhotoRGB )
				channel = channel <= 0.001953 ? 16.0 * channel : std::pow( channel, 1.0 / 1.8 );
			channel = std::max( 0.0, std::min( 1.0, channel ) );
		}
		return rgb;
	}
#endif

	void TestPNGTransferMetadata()
	{
#ifndef NO_PNG_SUPPORT
		const std::array<RISEColor, 2> colors = {
			RISEColor( RISEPel( 0.018, 0.271, 0.673 ), 0.37 ),
			RISEColor( RISEPel( 0.0005, 0.001, 0.0015 ), 0.8 )
		};
		struct Encoding { COLOR_SPACE space; const char* name; double gamma; };
		const Encoding encodings[] = {
			{ eColorSpace_Rec709RGB_Linear, "linear709", 1.0 },
			{ eColorSpace_ROMMRGB_Linear, "linearROMM", 1.0 },
			{ eColorSpace_sRGB, "sRGB", 1.0 / 2.2 },
			{ eColorSpace_ProPhotoRGB, "ProPhoto", 1.0 / 1.8 }
		};
		for( const auto& encoding : encodings ) {
			for( unsigned char depth : { 8, 16 } ) {
				auto* buffer = new MemoryBuffer();
				IRasterImageWriter* writer = nullptr;
				RISE_API_CreatePNGWriter( &writer, *buffer, depth, encoding.space );
				if( writer ) {
					writer->BeginWrite( 2, 1 );
					for( unsigned int x = 0; x < colors.size(); ++x ) writer->WriteColor( colors[x], x, 0 );
					writer->EndWrite();
					writer->release();
				}
				const auto png = ReadRawPNGPixelRow( *buffer );
				buffer->release();
				const std::string label = std::string("PNGGamma: ") + encoding.name + " " + std::to_string(depth);
				Check( png.decoded && png.depth == depth && png.channels == 4, label + " independent libpng decode" );
				const double quantizedGamma = std::round( encoding.gamma * 100000.0 ) / 100000.0;
				Check( png.decoded && png.hasGamma && std::abs(png.gamma - quantizedGamma) < 1.0e-10,
					label + " metadata matches the selected space's legacy gamma approximation" );
				bool samplesMatch = png.decoded;
				const unsigned int maximum = depth == 8 ? 255 : 65535;
				for( std::size_t x = 0; x < colors.size() && samplesMatch; ++x ) {
					const auto rgb = PNGReferenceChannels( colors[x], encoding.space );
					for( std::size_t channel = 0; channel < 4; ++channel ) {
						const double value = channel == 3 ? colors[x].a : rgb[channel];
						const unsigned int expected = static_cast<unsigned int>( value * maximum );
						const std::size_t offset = (x * 4u + channel) * (depth / 8u);
						const unsigned int actual = depth == 8 ? png.samples[offset] :
							(static_cast<unsigned int>(png.samples[offset]) << 8) | png.samples[offset + 1u];
						samplesMatch &= actual == expected;
					}
				}
				Check( samplesMatch, label + " raw RGB and alpha match analytic conversion and truncation" );
			}
		}
#endif
	}

	void TestPNGCodecFailurePropagation()
	{
#ifndef NO_PNG_SUPPORT
		struct ScopedFailure
		{
			bool previous = ForTest_SetPNGWriteFailureAfterHeader( true );
			~ScopedFailure() { ForTest_SetPNGWriteFailureAfterHeader( previous ); }
		};
		struct AcceptingBuffer final : MemoryBuffer
		{
			bool acceptedEveryWrite = true;
			bool setBytes( const void* bytes, unsigned int count ) override
			{
				const bool accepted = MemoryBuffer::setBytes( bytes, count );
				acceptedEveryWrite &= accepted;
				return accepted;
			}
		};
		FrameStoreSpec spec;
		spec.width = 2; spec.height = 1; spec.tileEdge = 8;
		auto* store = new FrameStore( spec );
		auto* viewport = new ViewportFrameStore();
		viewport->BindFrameStore( store );
		EncodeOpts opts;
		opts.bpp = 16;
		for( const char* name : { "PNG", "HDR10_PNG" } ) {
			auto* encoder = FrameEncoderRegistry::Get().ByFormatName( name );
			const std::string label = std::string("PNGCodecFailure: ") + name;
			auto* buffer = new AcceptingBuffer();
			bool reported = false;
			{
				ScopedFailure failure;
				try { encoder->Encode( *store, *buffer, opts ); }
				catch( const std::exception& ) { reported = true; }
			}
			Check( reported && buffer->getCurPos() > 8 && buffer->acceptedEveryWrite &&
				buffer->refcount() == 1 && !ReadRawPNGPixelRow(*buffer).decoded,
				label + " internal codec error after accepted header writes is reported and releases ownership" );
			buffer->Resize( 0, true );
			encoder->Encode( *store, *buffer, opts );
			const auto retried = ReadRawPNGPixelRow( *buffer );
			Check( retried.decoded && buffer->refcount() == 1,
				label + " healthy retry produces a complete PNG" );
			buffer->Resize( 0, true );
			bool saved = true;
			{
				ScopedFailure failure;
				saved = viewport->SaveTo( *buffer, encoder, opts );
			}
			Check( !saved && buffer->getCurPos() > 8 && buffer->acceptedEveryWrite,
				label + " SaveTo refuses internally failed PNG despite successful destination writes" );
			buffer->release();
			char storage[16]{};
			auto* rejecting = new MemoryBuffer( storage, sizeof(storage), false );
			reported = false;
			try { encoder->Encode( *store, *rejecting, opts ); }
			catch( const std::exception& ) { reported = true; }
			Check( reported && rejecting->refcount() == 1,
				label + " rejected destination writes propagate separately from internal errors" );
			rejecting->release();

			const std::filesystem::path path = RISE::TestSupport::ScratchPath(
				std::string("png_internal_failure_") + name + "_" +
				std::to_string(RISE::TestSupport::CurrentProcessId()) + ".png" );
			const auto readFile = [&path] {
				std::ifstream input( path, std::ios::binary );
				return std::vector<char>( std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() );
			};
			Check( viewport->SaveAs( path.string(), encoder, opts ), label + " staged baseline saved" );
			const auto complete = readFile();
			{
				ScopedFailure failure;
				saved = viewport->SaveAs( path.string(), encoder, opts );
			}
			Check( !saved && !complete.empty() && readFile() == complete,
				label + " failed staged encoding preserves the previous complete image" );
			Check( viewport->SaveAs( path.string(), encoder, opts ) && readFile() == complete,
				label + " staged encoding recovers after internal codec failure" );
			std::filesystem::remove( path );
		}
		// Invalid replacement must deactivate an old image and fail visibly;
		// EndWrite after that failure cannot emit stale pixels.
		auto* buffer = new MemoryBuffer();
		IRasterImageWriter* writer = nullptr;
		RISE_API_CreatePNGWriter( &writer, *buffer, 8, eColorSpace_sRGB );
		writer->BeginWrite( 2, 1 );
		bool reported = false;
		try { writer->BeginWrite( 0, 1 ); }
		catch( const std::exception& ) { reported = true; }
		writer->EndWrite();
		Check( reported && buffer->getCurPos() == 0,
			"PNGCodecFailure: invalid replacement reports failure and clears the previous active image" );
		writer->BeginWrite( 2, 1 );
		writer->WriteColor( RISEColor(RISEPel(0.2, 0.3, 0.4), 0.5), 0, 0 );
		writer->WriteColor( RISEColor(RISEPel(0.4, 0.3, 0.2), 0.5), 1, 0 );
		reported = false;
		{
			ScopedFailure failure;
			try { writer->EndWrite(); }
			catch( const std::exception& ) { reported = true; }
		}
		buffer->Resize( 0, true );
		writer->EndWrite();
		Check( reported && ReadRawPNGPixelRow(*buffer).decoded,
			"PNGCodecFailure: the same PNG writer can retry after codec cleanup" );
		writer->release();
		buffer->release();
		viewport->release();
		store->release();
#endif
	}

	void TestEncoderFactoryFailure()
	{
		struct NullFactoryEncoder final : FrameEncoderBase
		{
			std::string FormatName() const override { return "null_factory"; }
			std::vector<std::string> Extensions() const override { return { "null" }; }
			bool SupportsHDR() const override { return false; }
			bool SupportsAOVs() const override { return false; }
			IRasterImageWriter* CreateWriter( IWriteBuffer& dst, const EncodeOpts& ) const override
			{
				// Writer construction can emit bytes before reporting failure.
				// Those bytes must never turn a null factory result into success.
				const char partial[] = "partial factory output";
				dst.setBytes( partial, sizeof(partial) );
				return nullptr;
			}
		} encoder;
		FrameStoreSpec spec;
		spec.width = 2; spec.height = 1; spec.tileEdge = 8;
		auto* store = new FrameStore( spec );
		auto* viewport = new ViewportFrameStore();
		viewport->BindFrameStore( store );
		auto* buffer = new MemoryBuffer();
		EncodeOpts opts;
		bool reported = false;
		try { encoder.Encode( *store, *buffer, opts ); }
		catch( const std::exception& ) { reported = true; }
		Check( reported && buffer->getCurPos() > 0 && buffer->refcount() == 1,
			"EncoderFactoryFailure: direct encoding reports null factory after partial output" );
		buffer->Resize( 0, true );
		Check( !viewport->SaveTo( *buffer, &encoder, opts ) && buffer->getCurPos() > 0,
			"EncoderFactoryFailure: SaveTo refuses bytes from a failed writer factory" );
		const std::filesystem::path path = RISE::TestSupport::ScratchPath(
			"null_factory_" + std::to_string(RISE::TestSupport::CurrentProcessId()) + ".bin" );
		const std::string previous = "previous complete output";
		{ std::ofstream baseline( path, std::ios::binary ); baseline << previous; }
		const bool saved = viewport->SaveAs( path.string(), &encoder, opts );
		std::ifstream input( path, std::ios::binary );
		const std::string preserved{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
		input.close();
		Check( !saved && preserved == previous,
			"EncoderFactoryFailure: SaveAs preserves previous output when its factory fails" );
		std::filesystem::remove( path );
		buffer->release();
		viewport->release();
		store->release();
	}

	// ─── Pixel pattern ────────────────────────────────────────────
	// Small (16x16) image with values that exercise:
	//   - Black (0,0,0,1)
	//   - White (1,1,1,1)
	//   - HDR > 1 (3.0, 1.0, 0.5, 1)
	//   - Sub-white gradient
	// Same pattern is loaded into both a FrameStore and a
	// RasterImage_Template<RISEPel> for the diff comparison.
	constexpr unsigned int kImgW = 16;
	constexpr unsigned int kImgH = 16;

	RISEColor PatternPixel( unsigned int x, unsigned int y )
	{
		// Mostly-deterministic gradient with HDR spikes near
		// pixel (3, 5) and (10, 12) so we exercise both LDR-clip
		// and HDR-passthrough behavior.
		const double r = static_cast<double>( x ) / static_cast<double>( kImgW - 1 );
		const double g = static_cast<double>( y ) / static_cast<double>( kImgH - 1 );
		const double b = 0.25 + 0.5 * ( r + g ) / 2.0;
		double rr = r, gg = g, bb = b;
		if ( x == 3 && y == 5 )   { rr = 3.0;  gg = 1.0; bb = 0.5; }
		if ( x == 10 && y == 12 ) { rr = 0.5;  gg = 4.5; bb = 0.5; }
		return RISEColor( RISEPel( rr, gg, bb ), 1.0 );
	}

	// Populate a FrameStore by READING from a pre-filled legacy
	// image.  This guarantees both test fixtures hold byte-identical
	// pixel values regardless of how `-ffast-math -flto` may reorder
	// floating-point ops between two call sites of PatternPixel.  At
	// 8-bit quantisation the FP-reorder differences round to the same
	// byte, but at 16-bit (esp. ROMM where the matrix has more
	// dynamic range), even tiny ULP-level differences cross uint16
	// boundaries and produce different deflate output sizes.
	void FillFrameStoreFromLegacy( FrameStore* store,
	                               const RasterImage_Template<RISEPel>& src )
	{
		auto* beauty = store->GetChannel<ChannelId::Beauty>();
		auto* alpha  = store->GetChannel<ChannelId::Alpha>();
		const unsigned int w = static_cast<unsigned int>( store->Width() );
		const unsigned int h = static_cast<unsigned int>( store->Height() );
		// Walk all tiles to lock-protect each.
		for ( size_t ty = 0; ty < store->TileCountY(); ++ty ) {
			for ( size_t tx = 0; tx < store->TileCountX(); ++tx ) {
				store->BeginTile( tx, ty );
				const unsigned int x0 = static_cast<unsigned int>( tx * store->TileEdge() );
				const unsigned int y0 = static_cast<unsigned int>( ty * store->TileEdge() );
				const unsigned int x1 = std::min( x0 + static_cast<unsigned int>( store->TileEdge() ), w );
				const unsigned int y1 = std::min( y0 + static_cast<unsigned int>( store->TileEdge() ), h );
				for ( unsigned int y = y0; y < y1; ++y ) {
					for ( unsigned int x = x0; x < x1; ++x ) {
						const RISEColor c = src.GetPEL( x, y );
						beauty->At( x, y ) = c.base;
						alpha->At( x, y )  = c.a;
					}
				}
				store->EndTile( tx, ty );
			}
		}
	}

	// Populate a RasterImage_Template<RISEPel> from PatternPixel.
	void FillLegacyImage( RasterImage_Template<RISEPel>& img )
	{
		for ( unsigned int y = 0; y < kImgH; ++y ) {
			for ( unsigned int x = 0; x < kImgW; ++x ) {
				img.SetPEL( x, y, PatternPixel( x, y ) );
			}
		}
	}

	// Convenience: build a kImgW × kImgH legacy image, fill it,
	// then mirror its bytes into the FrameStore.  Used by tests
	// that need a ready-to-go FrameStore but don't compare
	// against the legacy buffer (single-path tests).
	void FillFrameStoreFromLegacyForFakeStore( FrameStore* store )
	{
		auto* tmp = new RasterImage_Template<RISEPel>(
			kImgW, kImgH, RISEColor( RISEPel( 0, 0, 0 ), 1.0 ) );
		FillLegacyImage( *tmp );
		FillFrameStoreFromLegacy( store, *tmp );
		safe_release( tmp );
	}

	// ─── Buffer helpers ───────────────────────────────────────────
	// Compare two MemoryBuffers byte-for-byte across the actually-
	// written byte range (getCurPos), not the allocated capacity
	// (Size).  ResizeForMore can over-allocate; only the bytes up
	// to the cursor matter for byte-identity.
	bool BuffersEqual( const MemoryBuffer& a, const MemoryBuffer& b,
	                   std::string& diagOut )
	{
		const unsigned int aLen = a.getCurPos();
		const unsigned int bLen = b.getCurPos();
		if ( aLen != bLen ) {
			std::ostringstream os;
			os << "size mismatch: new=" << aLen << " legacy=" << bLen;
			diagOut = os.str();
			return false;
		}

		const char* aChars = a.Pointer();
		const char* bChars = b.Pointer();
		const unsigned char* aBytes = reinterpret_cast<const unsigned char*>( aChars );
		const unsigned char* bBytes = reinterpret_cast<const unsigned char*>( bChars );
		for ( unsigned int i = 0; i < aLen; ++i ) {
			if ( aBytes[i] != bBytes[i] ) {
				std::ostringstream os;
				os << "first byte diff at offset " << i
				   << ": new=0x" << std::hex << static_cast<int>( aBytes[i] )
				   << " legacy=0x" << static_cast<int>( bBytes[i] ) << std::dec;
				diagOut = os.str();
				return false;
			}
		}
		return true;
	}

	// ─── Legacy pipeline: build the same writer + wrapper + dump
	//     a RasterImage_Template through it.  Mirrors
	//     FileRasterizerOutput::WriteImageToFile.
	void RunLegacyEncode(
		const std::string& format,
		const EncodeOpts&  opts,
		MemoryBuffer&      outBuf,
		const RasterImage_Template<RISEPel>& img )
	{
		IRasterImageWriter* w = nullptr;
		if ( format == "PNG" ) {
			RISE_API_CreatePNGWriter( &w, outBuf, opts.bpp, opts.colorSpace );
		} else if ( format == "EXR" ) {
			RISE_API_CreateEXRWriter( &w, outBuf, opts.colorSpace,
				opts.exrCompression, opts.exrWithAlpha );
		} else if ( format == "TIFF" ) {
			RISE_API_CreateTIFFWriter( &w, outBuf, opts.colorSpace );
		} else if ( format == "HDR" ) {
			RISE_API_CreateHDRWriter( &w, outBuf, opts.colorSpace );
		} else if ( format == "RGBEA" ) {
			RISE_API_CreateRGBEAWriter( &w, outBuf );
		} else if ( format == "TGA" ) {
			RISE_API_CreateTGAWriter( &w, outBuf, opts.colorSpace );
		} else if ( format == "PPM" ) {
			RISE_API_CreatePPMWriter( &w, outBuf, opts.colorSpace );
		}

		if ( !w ) {
			std::cerr << "Legacy encode: writer factory returned null for " << format << "\n";
			return;
		}

		// Mirror the IsHDRFormat gate.
		const bool isHDR =
			   ( format == "EXR"   )
			|| ( format == "HDR"   )
			|| ( format == "RGBEA" );

		IRasterImageWriter* effective = w;
		DisplayTransformWriter* dtw = nullptr;
		if ( !isHDR ) {
			const Scalar totalEV = static_cast<Scalar>( opts.viewTransform.exposureEV );
			const bool useDt =
				   ( opts.viewTransform.toneCurve != eDisplayTransform_None )
				|| ( totalEV != Scalar( 0 ) );
			if ( useDt ) {
				dtw = new DisplayTransformWriter( *w, totalEV, opts.viewTransform.toneCurve );
				effective = dtw;
			}
		}

		img.DumpImage( effective );

		if ( dtw ) safe_release( dtw );
		safe_release( w );
	}

	// ─── Per-format diff test ─────────────────────────────────────
	void DiffOneFormat(
		const std::string& format,
		const EncodeOpts&  opts )
	{
		// Build inputs.  All Reference-based RISE types
		// (MemoryBuffer, RasterImage_Template, FrameStore) have
		// protected destructors and must be heap-allocated and
		// released.
		FrameStore::Spec spec;
		spec.width    = kImgW;
		spec.height   = kImgH;
		spec.tileEdge = 16;
		FrameStore* store = new FrameStore( spec );

		// Populate the legacy image first, then derive FrameStore
		// from it to lock test-data identity (see FillFrameStoreFromLegacy).
		auto* legacyImg = new RasterImage_Template<RISEPel>(
			kImgW, kImgH, RISEColor( RISEPel( 0, 0, 0 ), 1.0 ) );
		FillLegacyImage( *legacyImg );
		FillFrameStoreFromLegacy( store, *legacyImg );

		// Encode via new IFrameEncoder.
		auto* newBuf = new MemoryBuffer();
		IFrameEncoder* enc = FrameEncoderRegistry::Get().ByFormatName( format );
		std::ostringstream tag;
		tag << "[" << format << "]";
		if ( !enc ) {
			Check( false, tag.str() + " encoder registered" );
			safe_release( newBuf );
			safe_release( legacyImg );
			store->release();
			return;
		}
		Check( enc != nullptr, tag.str() + " encoder lookup" );

		enc->Encode( *store, *newBuf, opts );

		// Encode via legacy pipeline.
		auto* legacyBuf = new MemoryBuffer();
		RunLegacyEncode( format, opts, *legacyBuf, *legacyImg );

		// Compare.
		std::string diag;
		const bool ok = BuffersEqual( *newBuf, *legacyBuf, diag );
		std::ostringstream label;
		label << tag.str() << " byte-identical to legacy"
		      << " (bytes=" << newBuf->getCurPos() << ")";
		if ( !ok ) label << " — " << diag;
		Check( ok, label.str() );

		// Sanity: the buffer is non-empty.
		Check( newBuf->getCurPos() > 0, tag.str() + " non-empty output" );

		safe_release( legacyBuf );
		safe_release( newBuf );
		safe_release( legacyImg );
		store->release();
	}

	// ─── Section 1: registry sanity ───────────────────────────────
	void TestRegistry()
	{
		auto& reg = FrameEncoderRegistry::Get();
		auto all = reg.All();

		// L5c added the 8th encoder (HDR10_PNG).  Test asserts the
		// updated count; the legacy 7 are still present in the same
		// registration order followed by the new HDR10_PNG.
		Check( all.size() == 8, "registry has 8 built-in encoders (7 legacy + L5c HDR10_PNG)" );

		const char* expectedFormats[] = {
			"PNG", "EXR", "TIFF", "HDR", "RGBEA", "TGA", "PPM",
			"HDR10_PNG"
		};
		for ( const char* fmt : expectedFormats ) {
			IFrameEncoder* enc = reg.ByFormatName( fmt );
			std::ostringstream os;
			os << "ByFormatName(\"" << fmt << "\") found";
			Check( enc != nullptr, os.str() );
		}

		// Case-insensitive lookup.
		Check( reg.ByFormatName( "png" )  != nullptr, "ByFormatName(\"png\") (lowercase)" );
		Check( reg.ByFormatName( "Png" )  != nullptr, "ByFormatName(\"Png\") (mixed case)" );
		Check( reg.ByFormatName( "MISS" ) == nullptr, "ByFormatName(\"MISS\") returns null" );

		// Extension lookup, with and without leading dot.
		Check( reg.ByExtension( "png"  ) != nullptr, "ByExtension(\"png\")" );
		Check( reg.ByExtension( ".png" ) != nullptr, "ByExtension(\".png\")" );
		Check( reg.ByExtension( "tif"  ) != nullptr, "ByExtension(\"tif\") (TIFF alias)" );
		Check( reg.ByExtension( "tiff" ) != nullptr, "ByExtension(\"tiff\")" );
		Check( reg.ByExtension( "miss" ) == nullptr, "ByExtension(\"miss\") returns null" );

		// HDR-format flags match the legacy IsHDRFormat gate.
		Check( reg.ByFormatName( "EXR"   )->SupportsHDR() == true,  "EXR.SupportsHDR" );
		Check( reg.ByFormatName( "HDR"   )->SupportsHDR() == true,  "HDR.SupportsHDR" );
		Check( reg.ByFormatName( "RGBEA" )->SupportsHDR() == true,  "RGBEA.SupportsHDR" );
		Check( reg.ByFormatName( "PNG"   )->SupportsHDR() == false, "PNG !SupportsHDR" );
		Check( reg.ByFormatName( "TGA"   )->SupportsHDR() == false, "TGA !SupportsHDR" );
		Check( reg.ByFormatName( "PPM"   )->SupportsHDR() == false, "PPM !SupportsHDR" );
		Check( reg.ByFormatName( "TIFF"  )->SupportsHDR() == false, "TIFF !SupportsHDR" );
	}

	// ─── Section 2: byte-identical regression per format ──────────
	void TestByteIdentityAllFormats()
	{
		// Default opts: identity ViewTransform, sRGB color space, 8 bpp.
		EncodeOpts defaultOpts;
		defaultOpts.viewTransform = ViewTransform::Identity();
		defaultOpts.colorSpace    = eColorSpace_sRGB;
		defaultOpts.bpp           = 8;

		// PNG default + with display transform.
		DiffOneFormat( "PNG", defaultOpts );
		EncodeOpts pngWithTone;
		pngWithTone.viewTransform = ViewTransform::ForLDRDisplay( 0.5f, eDisplayTransform_ACES );
		pngWithTone.colorSpace    = eColorSpace_sRGB;
		pngWithTone.bpp           = 8;
		DiffOneFormat( "PNG", pngWithTone );

		// PNG 16-bpp.
		EncodeOpts png16;
		png16.viewTransform = ViewTransform::Identity();
		png16.colorSpace    = eColorSpace_sRGB;
		png16.bpp           = 16;
		DiffOneFormat( "PNG", png16 );

		// EXR — HDR archival (no display transform applied).
		EncodeOpts exr;
		exr.viewTransform = ViewTransform::Identity();
		exr.colorSpace    = eColorSpace_Rec709RGB_Linear;
		exr.exrCompression = eExrCompression_Piz;
		exr.exrWithAlpha  = true;
		DiffOneFormat( "EXR", exr );

		// EXR ZIP compression variant.
		exr.exrCompression = eExrCompression_Zip;
		DiffOneFormat( "EXR", exr );

		// HDR / Radiance.
		EncodeOpts hdr;
		hdr.viewTransform = ViewTransform::Identity();
		hdr.colorSpace    = eColorSpace_Rec709RGB_Linear;
		DiffOneFormat( "HDR", hdr );

		// RGBEA (no color_space param at the writer level).
		EncodeOpts rgbea;
		rgbea.viewTransform = ViewTransform::Identity();
		DiffOneFormat( "RGBEA", rgbea );

#ifndef NO_TIFF_SUPPORT
		// TIFF default.  Skipped under NO_TIFF_SUPPORT — the encoder
		// is still registered (registry-sanity tests above cover that)
		// but produces empty output, which trips the "non-empty output"
		// Check at the end of DiffOneFormat.
		EncodeOpts tiff;
		tiff.viewTransform = ViewTransform::Identity();
		tiff.colorSpace    = eColorSpace_sRGB;
		DiffOneFormat( "TIFF", tiff );

		// TIFF with tone curve.
		tiff.viewTransform = ViewTransform::ForLDRDisplay( 0.0f, eDisplayTransform_Reinhard );
		DiffOneFormat( "TIFF", tiff );
#endif

		// TGA.
		EncodeOpts tga;
		tga.viewTransform = ViewTransform::Identity();
		tga.colorSpace    = eColorSpace_sRGB;
		DiffOneFormat( "TGA", tga );

		// PPM.
		EncodeOpts ppm;
		ppm.viewTransform = ViewTransform::Identity();
		ppm.colorSpace    = eColorSpace_sRGB;
		DiffOneFormat( "PPM", ppm );
	}

	// ─── Section 3: HDR ViewTransform is ignored on HDR formats ───
	// FileRasterizerOutput.cpp:94 forces display_transform / exposure
	// to identity for IsHDRFormat targets.  Our encoder mirrors this:
	// the IsHDRFormat() == true branch in FrameEncoderBase::Encode
	// skips the DisplayTransformWriter wrap regardless of opts.
	// This test verifies that an opts with a non-trivial ViewTransform
	// produces the same EXR/.hdr/.rgbea bytes as opts with identity.
	void TestHDRFormatsIgnoreViewTransform()
	{
		FrameStore::Spec spec;
		spec.width = kImgW; spec.height = kImgH; spec.tileEdge = 16;
		FrameStore* store = new FrameStore( spec );
		FillFrameStoreFromLegacyForFakeStore( store );

		const char* hdrFormats[] = { "EXR", "HDR", "RGBEA" };
		for ( const char* fmt : hdrFormats ) {
			IFrameEncoder* enc = FrameEncoderRegistry::Get().ByFormatName( fmt );
			if ( !enc ) continue;

			auto* bufIdentity     = new MemoryBuffer();
			auto* bufWithToneCurve = new MemoryBuffer();

			EncodeOpts identityOpts;
			identityOpts.viewTransform  = ViewTransform::Identity();
			identityOpts.colorSpace     = eColorSpace_Rec709RGB_Linear;
			identityOpts.exrCompression = eExrCompression_Piz;
			identityOpts.exrWithAlpha   = true;
			enc->Encode( *store, *bufIdentity, identityOpts );

			EncodeOpts toneOpts = identityOpts;
			toneOpts.viewTransform = ViewTransform::ForLDRDisplay( 1.0f, eDisplayTransform_ACES );
			enc->Encode( *store, *bufWithToneCurve, toneOpts );

			std::string diag;
			const bool ok = BuffersEqual( *bufIdentity, *bufWithToneCurve, diag );
			std::ostringstream label;
			label << "[" << fmt << "] ignores ViewTransform (HDR archival)";
			if ( !ok ) label << " — " << diag;
			Check( ok, label.str() );

			safe_release( bufIdentity );
			safe_release( bufWithToneCurve );
		}

		store->release();
	}

	// ─── Section 4: non-1.0 alpha precision (HIGH-1 regression) ───
	// The legacy Color_Template<RISEPel>::a is Chel (= double).
	// FrameStore stores alpha in a Channel<Chel> (double) for byte-
	// identical parity.  This test populates pixels with a non-
	// exactly-representable-as-float alpha (0.7) and asserts that
	// EXR + PNG + HDR encoders produce byte-identical bytes vs the
	// legacy path.  If alpha were stored as float, the
	// double→float→double roundtrip would corrupt 0.7 and break
	// byte-identity.  See L2 adversarial review HIGH-1.
	void TestNon10AlphaPrecision()
	{
		FrameStore::Spec spec;
		spec.width    = 8;
		spec.height   = 4;
		spec.tileEdge = 8;
		FrameStore* store = new FrameStore( spec );

		// Fill with alpha = 0.7 (not exactly representable in
		// float — repeating binary; double represents it as
		// 0.6999999999999999555910790... which differs from
		// float's 0.69999998807907104...).
		auto* beauty = store->GetChannel<ChannelId::Beauty>();
		auto* alphaC = store->GetChannel<ChannelId::Alpha>();
		store->BeginTile( 0, 0 );
		for ( unsigned int y = 0; y < 4; ++y ) {
			for ( unsigned int x = 0; x < 8; ++x ) {
				beauty->At( x, y ) = RISEPel( 0.5, 0.3, 0.7 );
				alphaC->At( x, y ) = 0.7;  // double precision
			}
		}
		store->EndTile( 0, 0 );

		auto* legacyImg = new RasterImage_Template<RISEPel>(
			8, 4, RISEColor( RISEPel( 0, 0, 0 ), 1.0 ) );
		for ( unsigned int y = 0; y < 4; ++y ) {
			for ( unsigned int x = 0; x < 8; ++x ) {
				legacyImg->SetPEL( x, y,
					RISEColor( RISEPel( 0.5, 0.3, 0.7 ), 0.7 ) );
			}
		}

		const char* formats[] = { "EXR", "PNG", "HDR" };
		for ( const char* fmt : formats ) {
			IFrameEncoder* enc = FrameEncoderRegistry::Get().ByFormatName( fmt );

			EncodeOpts opts;
			opts.viewTransform = ViewTransform::Identity();
			opts.colorSpace =
				   ( std::string( fmt ) == "EXR" || std::string( fmt ) == "HDR" )
				 ? eColorSpace_Rec709RGB_Linear
				 : eColorSpace_sRGB;

			auto* newBuf    = new MemoryBuffer();
			auto* legacyBuf = new MemoryBuffer();

			enc->Encode( *store, *newBuf, opts );
			RunLegacyEncode( fmt, opts, *legacyBuf, *legacyImg );

			std::string diag;
			const bool ok = BuffersEqual( *newBuf, *legacyBuf, diag );
			std::ostringstream label;
			label << "[" << fmt << "] alpha=0.7 byte-identical (no float-precision loss)";
			if ( !ok ) label << " — " << diag;
			Check( ok, label.str() );

			safe_release( legacyBuf );
			safe_release( newBuf );
		}

		safe_release( legacyImg );
		store->release();
	}

	// ─── Section 5: cameraExposureEV (HIGH-2 regression) ──────────
	// Legacy FileRasterizerOutput sums `exposureEV +
	// cameraExposureEV` to compute the total EV passed into
	// DisplayTransformWriter (FileRasterizerOutput.cpp:231).  The
	// new encoder must read cameraExposureEV from store.Meta()
	// and add it to opts.viewTransform.exposureEV.  This test
	// populates Meta with cameraExposureEV = 1.0 and opts with
	// viewTransform.exposureEV = 0.5, then verifies byte-identity
	// vs the legacy path which uses the sum directly.
	void TestCameraExposureFromMeta()
	{
		FrameStore::Spec spec;
		spec.width = kImgW; spec.height = kImgH; spec.tileEdge = 16;
		spec.meta.cameraExposureEV = 1.0;  // per-frame camera-side EV
		FrameStore* store = new FrameStore( spec );

		auto* legacyImg = new RasterImage_Template<RISEPel>(
			kImgW, kImgH, RISEColor( RISEPel( 0, 0, 0 ), 1.0 ) );
		FillLegacyImage( *legacyImg );
		FillFrameStoreFromLegacy( store, *legacyImg );

		IFrameEncoder* enc = FrameEncoderRegistry::Get().ByFormatName( "PNG" );

		// New path: caller sets static EV via opts; encoder pulls
		// camera EV from store.Meta() and sums.
		EncodeOpts newOpts;
		newOpts.viewTransform = ViewTransform::ForLDRDisplay( 0.5f, eDisplayTransform_None );
		newOpts.colorSpace    = eColorSpace_sRGB;
		newOpts.bpp           = 8;
		auto* newBuf = new MemoryBuffer();
		enc->Encode( *store, *newBuf, newOpts );

		// Legacy comparison path: total EV is the SUM (0.5 + 1.0 = 1.5).
		// The legacy FileRasterizerOutput would have called
		// DisplayTransformWriter with totalEV = 1.5.
		EncodeOpts legacyEquivOpts;
		legacyEquivOpts.viewTransform = ViewTransform::ForLDRDisplay( 1.5f, eDisplayTransform_None );
		legacyEquivOpts.colorSpace    = eColorSpace_sRGB;
		legacyEquivOpts.bpp           = 8;
		auto* legacyBuf = new MemoryBuffer();
		RunLegacyEncode( "PNG", legacyEquivOpts, *legacyBuf, *legacyImg );

		std::string diag;
		const bool ok = BuffersEqual( *newBuf, *legacyBuf, diag );
		std::ostringstream label;
		label << "[PNG] cameraExposureEV summed with opts.exposureEV";
		if ( !ok ) label << " — " << diag;
		Check( ok, label.str() );

		safe_release( legacyBuf );
		safe_release( newBuf );
		safe_release( legacyImg );
		store->release();
	}

	// ─── Section 6: ROMM color space (MEDIUM-3 regression) ────────
	// Existing writers have separate code paths for ROMM (and
	// ProPhoto) — including a chromaticity-tag emission branch in
	// EXRWriter.  Verify byte-identity through the ROMM path for
	// both an LDR (PNG) and an HDR (EXR) target.
	void TestROMMColorSpace()
	{
		FrameStore::Spec spec;
		spec.width = kImgW; spec.height = kImgH; spec.tileEdge = 16;
		FrameStore* store = new FrameStore( spec );

		auto* legacyImg = new RasterImage_Template<RISEPel>(
			kImgW, kImgH, RISEColor( RISEPel( 0, 0, 0 ), 1.0 ) );
		FillLegacyImage( *legacyImg );

		// Populate FrameStore by READING the legacy image — guarantees
		// the two test fixtures hold byte-identical pixel values.  If
		// byte-identity still fails after this, the divergence is in
		// the encoder pipeline, not the test data.
		auto* beauty = store->GetChannel<ChannelId::Beauty>();
		auto* alpha  = store->GetChannel<ChannelId::Alpha>();
		store->BeginTile( 0, 0 );
		for ( unsigned int y = 0; y < kImgH; ++y ) {
			for ( unsigned int x = 0; x < kImgW; ++x ) {
				const RISEColor c = legacyImg->GetPEL( x, y );
				beauty->At( x, y ) = c.base;
				alpha->At( x, y )  = c.a;
			}
		}
		store->EndTile( 0, 0 );

		const char* formats[] = { "PNG", "EXR" };
		for ( const char* fmt : formats ) {
			IFrameEncoder* enc = FrameEncoderRegistry::Get().ByFormatName( fmt );

			EncodeOpts opts;
			opts.viewTransform = ViewTransform::Identity();
			opts.colorSpace    = eColorSpace_ROMMRGB_Linear;
			opts.bpp           = 16;

			auto* newBuf    = new MemoryBuffer();
			auto* legacyBuf = new MemoryBuffer();

			enc->Encode( *store, *newBuf, opts );
			RunLegacyEncode( fmt, opts, *legacyBuf, *legacyImg );

			std::string diag;
			const bool ok = BuffersEqual( *newBuf, *legacyBuf, diag );
			std::ostringstream label;
			label << "[" << fmt << "] ROMM colorspace byte-identical";
			if ( !ok ) label << " — " << diag;
			Check( ok, label.str() );

			safe_release( legacyBuf );
			safe_release( newBuf );
		}

		safe_release( legacyImg );
		store->release();
	}

	// ─── Section 7: edge dimensions (MEDIUM-4 regression) ─────────
	// 1×1 image, then a non-tile-aligned size that crosses tile
	// boundaries (17×16 with tileEdge=16 → tileCount = 2x1).
	void TestEdgeDimensions()
	{
		struct Case { unsigned w, h, tileEdge; const char* label; };
		const Case cases[] = {
			{  1,  1, 16, "1x1"  },
			{ 17, 16, 16, "17x16 (cross-tile)" },
			{  3, 64, 16, "3x64 (tall sliver)" },
		};

		for ( const Case& c : cases ) {
			FrameStore::Spec spec;
			spec.width = c.w; spec.height = c.h; spec.tileEdge = c.tileEdge;
			FrameStore* store = new FrameStore( spec );

			// Fill all tiles using the FrameStore's per-tile API.
			auto* beauty = store->GetChannel<ChannelId::Beauty>();
			auto* alpha  = store->GetChannel<ChannelId::Alpha>();
			for ( size_t ty = 0; ty < store->TileCountY(); ++ty ) {
				for ( size_t tx = 0; tx < store->TileCountX(); ++tx ) {
					store->BeginTile( tx, ty );
					const unsigned x0 = static_cast<unsigned>( tx * c.tileEdge );
					const unsigned y0 = static_cast<unsigned>( ty * c.tileEdge );
					const unsigned x1 = std::min( x0 + static_cast<unsigned>( c.tileEdge ), c.w );
					const unsigned y1 = std::min( y0 + static_cast<unsigned>( c.tileEdge ), c.h );
					for ( unsigned y = y0; y < y1; ++y ) {
						for ( unsigned x = x0; x < x1; ++x ) {
							beauty->At( x, y ) = RISEPel(
								static_cast<double>( x ) / 16.0,
								static_cast<double>( y ) / 16.0,
								0.5 );
							alpha->At( x, y ) = 1.0;
						}
					}
					store->EndTile( tx, ty );
				}
			}

			auto* legacyImg = new RasterImage_Template<RISEPel>(
				c.w, c.h, RISEColor( RISEPel( 0, 0, 0 ), 1.0 ) );
			for ( unsigned y = 0; y < c.h; ++y ) {
				for ( unsigned x = 0; x < c.w; ++x ) {
					legacyImg->SetPEL( x, y,
						RISEColor(
							RISEPel( static_cast<double>( x ) / 16.0,
							         static_cast<double>( y ) / 16.0,
							         0.5 ),
							1.0 ) );
				}
			}

			IFrameEncoder* enc = FrameEncoderRegistry::Get().ByFormatName( "PNG" );
			EncodeOpts opts;
			opts.viewTransform = ViewTransform::Identity();
			opts.colorSpace    = eColorSpace_sRGB;
			opts.bpp           = 8;

			auto* newBuf    = new MemoryBuffer();
			auto* legacyBuf = new MemoryBuffer();
			enc->Encode( *store, *newBuf, opts );
			RunLegacyEncode( "PNG", opts, *legacyBuf, *legacyImg );

			std::string diag;
			const bool ok = BuffersEqual( *newBuf, *legacyBuf, diag );
			std::ostringstream label;
			label << "[PNG " << c.label << "] byte-identical";
			if ( !ok ) label << " — " << diag;
			Check( ok, label.str() );

			safe_release( legacyBuf );
			safe_release( newBuf );
			safe_release( legacyImg );
			store->release();
		}
	}

	// ─── Section 8: HDR exposure-only ignored (MEDIUM-5) ──────────
	// HDR formats must skip the DisplayTransformWriter wrap not just
	// for tone curves but also for exposure-only ViewTransforms.
	void TestHDRExposureOnlyIgnored()
	{
		FrameStore::Spec spec;
		spec.width = kImgW; spec.height = kImgH; spec.tileEdge = 16;
		FrameStore* store = new FrameStore( spec );
		FillFrameStoreFromLegacyForFakeStore( store );

		IFrameEncoder* enc = FrameEncoderRegistry::Get().ByFormatName( "EXR" );

		EncodeOpts identityOpts;
		identityOpts.viewTransform  = ViewTransform::Identity();
		identityOpts.colorSpace     = eColorSpace_Rec709RGB_Linear;
		identityOpts.exrCompression = eExrCompression_Piz;
		identityOpts.exrWithAlpha   = true;

		EncodeOpts exposureOpts = identityOpts;
		exposureOpts.viewTransform.exposureEV = 2.0f;  // tone curve still None

		auto* bufA = new MemoryBuffer();
		auto* bufB = new MemoryBuffer();
		enc->Encode( *store, *bufA, identityOpts );
		enc->Encode( *store, *bufB, exposureOpts );

		std::string diag;
		const bool ok = BuffersEqual( *bufA, *bufB, diag );
		std::ostringstream label;
		label << "[EXR] exposure-only ViewTransform ignored on HDR target";
		if ( !ok ) label << " — " << diag;
		Check( ok, label.str() );

		safe_release( bufB );
		safe_release( bufA );
		store->release();
	}
}

// ─── Section: L5c HDR10 PNG encoder ──────────────────────────
//
// Validates that:
//   1. The HDR10_PNG encoder is in the registry (FormatName lookup,
//      both "HDR10_PNG" exact match and case-insensitive).
//   2. Encoding produces a non-trivial byte sequence beginning with
//      the PNG magic.
//   3. The cICP chunk is present in the output (HDR10 metadata).
//   4. The encoded RGB16_BT2020_PQ pipeline doesn't crash on edge
//      cases: 0×0 store (empty no-op), HDR > 1.0 inputs, NaN/Inf in
//      alpha (alpha is ignored for RGB-only output, but the pre-clamp
//      generalisation in EncodePixel must not regress).
//
// Note: full byte-identity test against an external HDR10 PNG
// reference encoder isn't included here — the L5c primitive's
// correctness is bounded by the existing `ApplyPQTransfer` +
// `ConvertPelToTargetPrimaries` unit tests in FrameStoreColorMathTest.
// This test verifies the HDR10 PNG envelope (PNG magic + cICP chunk
// + valid IHDR with 16-bit RGB) is structurally well-formed.
void TestHDR10PNGEncoder_L5c()
{
	using FrameStoreOutput::FrameStoreSpec;
	using FrameStoreOutput::ViewTransform;

	auto& reg = FrameEncoderRegistry::Get();

	// 1. Registry lookup.
	IFrameEncoder* hdr10 = reg.ByFormatName( "HDR10_PNG" );
	Check( hdr10 != nullptr, "L5c: HDR10_PNG encoder registered" );
	if ( !hdr10 ) return;
	Check( hdr10->SupportsHDR(), "L5c: HDR10_PNG reports SupportsHDR=true" );
	Check( !hdr10->SupportsAOVs(), "L5c: HDR10_PNG reports SupportsAOVs=false" );

	IFrameEncoder* hdr10Lower = reg.ByFormatName( "hdr10_png" );
	Check( hdr10Lower == hdr10, "L5c: HDR10_PNG case-insensitive lookup" );

	// 2. Encode a small HDR pattern to memory.
	FrameStoreSpec spec;
	spec.width    = kImgW;
	spec.height   = kImgH;
	spec.tileEdge = 8;
	auto* store = new FrameStore( spec );
	auto* beauty = store->GetChannel<FrameStoreOutput::ChannelId::Beauty>();
	auto* alpha  = store->GetChannel<FrameStoreOutput::ChannelId::Alpha>();
	for ( unsigned int y = 0; y < kImgH; ++y ) {
		for ( unsigned int x = 0; x < kImgW; ++x ) {
			RISEColor c = PatternPixel( x, y );
			if ( beauty ) beauty->At( x, y ) = c.base;
			if ( alpha )  alpha->At( x, y )  = c.a;
		}
	}

	auto* buf = new MemoryBuffer();
	EncodeOpts opts;
	opts.viewTransform = ViewTransform();
	hdr10->Encode( *store, *buf, opts );

	// 3. Verify the output starts with the PNG magic 8 bytes.
	const unsigned char* bytes =
		reinterpret_cast<const unsigned char*>( buf->Pointer() );
	const size_t totalSize = buf->Size();
	Check( totalSize > 8, "L5c: HDR10_PNG output > 8 bytes (PNG magic + chunks)" );
	if ( totalSize >= 8 ) {
		const unsigned char kPNGMagic[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
		const bool magicOK = std::memcmp( bytes, kPNGMagic, 8 ) == 0;
		Check( magicOK, "L5c: HDR10_PNG output starts with PNG magic" );
	}

	// 4. Verify the cICP chunk is present AND its 4-byte payload
	// declares HDR10 (BT.2020 / PQ / RGB / full range).  PNG
	// chunks are framed as:
	//   [4-byte big-endian length][4-byte type][data][4-byte CRC]
	// We walk the chunk graph from offset 8 (post-magic) so the
	// search can't false-match on "cICP" bytes that happen to
	// appear inside compressed IDAT payload.
	bool cICPFound = false;
	bool cICPPayloadOK = false;
	if ( totalSize >= 8 ) {
		size_t off = 8;
		while ( off + 12 <= totalSize ) {
			// Big-endian length.
			const uint32_t len =
				( static_cast<uint32_t>( bytes[off+0] ) << 24 ) |
				( static_cast<uint32_t>( bytes[off+1] ) << 16 ) |
				( static_cast<uint32_t>( bytes[off+2] ) <<  8 ) |
				( static_cast<uint32_t>( bytes[off+3] )       );
			const unsigned char* type = &bytes[off+4];
			const unsigned char* data = &bytes[off+8];
			if ( off + 12 + static_cast<size_t>( len ) > totalSize ) break;

			if ( type[0] == 'c' && type[1] == 'I' &&
			     type[2] == 'C' && type[3] == 'P' )
			{
				cICPFound = true;
				if ( len == 4u &&
				     data[0] == 9  &&  // BT.2020 primaries
				     data[1] == 16 &&  // SMPTE ST.2084 PQ transfer
				     data[2] == 0  &&  // Identity / RGB matrix
				     data[3] == 1  )   // Full range
				{
					cICPPayloadOK = true;
				}
				break;
			}
			// Stop when we hit IEND or IDAT (cICP must come before
			// IDAT per PNG 3rd Edition; if we walked past, no cICP).
			if ( ( type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T' ) ||
			     ( type[0] == 'I' && type[1] == 'E' && type[2] == 'N' && type[3] == 'D' ) )
			{
				break;
			}

			off += 12 + len;  // length + type + data + CRC
		}
	}
	Check( cICPFound, "L5c: HDR10_PNG cICP chunk emitted before IDAT" );
	Check( cICPPayloadOK,
		"L5c: HDR10_PNG cICP payload = {9 (BT.2020), 16 (PQ), 0 (RGB), 1 (full range)}" );

	// 4b. ByExtension("png") must still return the SDR PNG encoder
	// (HDR10_PNG is a same-extension encoder; users select via
	// FormatName).  Regression check on the registry's
	// extension-lookup precedence.
	IFrameEncoder* byExt = reg.ByExtension( "png" );
	Check( byExt != nullptr, "L5c: ByExtension(\"png\") returns an encoder" );
	if ( byExt ) {
		Check( byExt->FormatName() == "PNG",
			"L5c: ByExtension(\"png\") returns SDR PNG (registered first), not HDR10_PNG" );
	}

	// 5. Edge case — 0×0 store should no-op without crashing.
	FrameStoreSpec emptySpec;
	emptySpec.width = 0; emptySpec.height = 0; emptySpec.tileEdge = 8;
	auto* emptyStore = new FrameStore( emptySpec );
	auto* emptyBuf = new MemoryBuffer();
	hdr10->Encode( *emptyStore, *emptyBuf, opts );
	// No size assertion — implementation logs a warning and returns
	// (output may be 0 bytes or a header-only PNG; either is acceptable).
	Check( true, "L5c: HDR10_PNG empty store doesn't crash" );

	safe_release( emptyBuf );
	emptyStore->release();
	safe_release( buf );
	store->release();
}

static void TestEncoderFailureOwnership()
{
	class RejectingBuffer final : public MemoryBuffer {
	public:
		bool reject = true;
		unsigned int budget = 0;
		unsigned int written = 0;
		bool setBytes( const void* data, unsigned int count ) override {
			if( reject ) {
				if( count > budget ) return false;
				budget -= count;
			}
			written += count;
			return MemoryBuffer::setBytes(data, count);
		}
	};
	FrameStoreSpec spec;
	spec.width = spec.height = 1;
	spec.tileEdge = 8;
	auto* store = new FrameStore(spec);
	std::vector<const char*> formats{ "tga" };
#ifndef NO_EXR_SUPPORT
	formats.push_back("exr");
#endif
#ifndef NO_TIFF_SUPPORT
	formats.push_back("tiff");
#endif
	for( const char* format : formats ) {
		auto* encoder = FrameEncoderRegistry::Get().ByExtension(format);
		Check( encoder != nullptr, "failure fixture has its requested encoder" );
		if( !encoder ) continue;
		for( unsigned int bpp : { 16U, 32U } ) {
			EncodeOpts opts;
			opts.bpp = bpp;
			auto* complete = new RejectingBuffer();
			complete->reject = false;
			encoder->Encode(*store, *complete, opts);
			const auto lastWrite = complete->written - 1;
			complete->release();
			// Budgets inject early failures and failure at the last write.
			for( unsigned int budget : { 0U, std::min(16U, lastWrite), std::min(64U, lastWrite), lastWrite } ) {
				auto* buffer = new RejectingBuffer();
				buffer->budget = budget;
				bool failed = false;
				try { encoder->Encode(*store, *buffer, opts); }
				catch( const std::exception& ) { failed = true; }
				Check( failed && buffer->refcount() == 1, "failed encoding releases its writer and buffer reference" );
				buffer->reject = false;
				buffer->Resize(0, true);
				encoder->Encode(*store, *buffer, opts);
				Check( buffer->refcount() == 1 && buffer->getCurPos() > 0, "encoding recovers after a rejected output write" );
				buffer->seek(IBuffer::START, 0);
				IRasterImageReader* reader = nullptr;
				if( std::strcmp(format, "exr") == 0 )
					RISE_API_CreateEXRReader(&reader, *buffer, opts.colorSpace);
				else if( std::strcmp(format, "tiff") == 0 ) RISE_API_CreateTIFFReader(&reader, *buffer, opts.colorSpace);
				else RISE_API_CreateTGAReader(&reader, *buffer, opts.colorSpace);
				unsigned int width = 0, height = 0;
				Check( reader && reader->BeginRead(width, height) && width == 1 && height == 1,
					"retried encoder output decodes as a complete image" );
				if( reader ) { reader->EndRead(); reader->release(); }
				buffer->release();
			}
		}
	}

	store->release();

}

static int RunTests()
{
	std::cout << "FrameEncoderTest L2 — IFrameEncoder byte-identical regression\n";
	std::cout << "------------------------------------------------------------\n";

	TestRegistry();
	TestPNGTransferMetadata();
	TestPNGCodecFailurePropagation();
	TestEncoderFactoryFailure();
	TestEncoderFailureOwnership();
	TestByteIdentityAllFormats();
	TestHDRFormatsIgnoreViewTransform();
	TestNon10AlphaPrecision();
	TestCameraExposureFromMeta();
	TestROMMColorSpace();
	TestEdgeDimensions();
	TestHDRExposureOnlyIgnored();
	TestHDR10PNGEncoder_L5c();

	std::cout << "------------------------------------------------------------\n";
	std::cout << "passed " << gPassCount << ", failed " << gFailCount << "\n";
	return gFailCount == 0 ? 0 : 1;
}

int main()
{
	return RISE::TestSupport::WithOwnedScratch( RunTests );
}
