//////////////////////////////////////////////////////////////////////
//
//  ImageReaderOverflowTest.cpp - Regression test for the header-controlled
//    WRITE-side heap overflows in RISE's hand-rolled / delegated image
//    decoders (TGAReader, HDRReader, PNGReader, TIFFReader).
//
//  Distinct from the read-side binary-buffer bounds family (VolumeShortRead
//  Test): those bounded the SOURCE reads.  These decoders size their
//  DESTINATION heap buffer from file-header dimensions with a 32-bit
//  `width*height*channels` multiply that can integer-overflow to a tiny
//  allocation the decode loop then writes past.  HDR additionally had an
//  unbounded Radiance-RLE decode cursor.  PNG/TIFF hand their (untrusted)
//  header dimensions to libpng/libtiff which do NOT validate the caller's
//  destination-buffer size (libpng's default limit is 1e6x1e6; libtiff's
//  TIFFReadRGBAImage writes width*height pixels blind).
//
//  The fix rejects degenerate / oversized dimensions before allocating
//  (64-bit multiply + sane ceiling) and, for HDR, bounds every RLE write to
//  the scanline / buffer, returning false on overrun rather than writing OOB.
//
//  This test crafts in-memory / temp-file "files":
//    TGA : overflow-dim (32768^2 wraps to 0), zero-dim, valid 2x2
//    HDR : new-RLE overflow-run, truncated scanline, valid 4x1 (+pixel),
//          old-RLE valid raw, old-RLE valid run (back-reference), old-RLE
//          crafted run at pixel 0 (back-reference underflow)
//    PNG : IHDR with 100000x100000 dims (valid CRC) -> rejected pre-alloc
//          (valid-PNG decode is covered by the broader scene-test suite,
//          which loads real .png textures)
//    TIFF: valid 2x2 decodes; a 65535^2-patched TIFF is safely rejected
//    EXR : FLOAT RGB/Y/YA range, offset windows, SDK YC/YCA reconstruction
//    All six readers: repeat decode, failed reload, recovery, end, and bounds
//  Under AddressSanitizer the crafted cases trap on the pre-fix code
//  (heap-buffer-overflow); on the fixed code they return false cleanly.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <iterator>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <stdexcept>

#include <zlib.h>						// crc32 for the crafted PNG chunks

#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/RasterImages/TGAReader.h"
#include "../src/Library/RasterImages/HDRReader.h"
#include "../src/Library/RasterImages/PNGReader.h"
#include "../src/Library/RasterImages/TIFFReader.h"
#include "../src/Library/RasterImages/JPEGReader.h"
#include "../src/Library/RasterImages/EXRReader.h"
#include "../src/Library/RasterImages/EXRWriter.h"
#ifndef NO_EXR_SUPPORT
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfChannelList.h>
#endif
#include "../src/Library/RasterImages/TGAWriter.h"
#include "../src/Library/RasterImages/RasterImage.h"
#include "TestTempDir.h"

#ifndef NO_TIFF_SUPPORT
	#include <tiffio.h>
#endif

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const char* name )
{
	if( cond ) {
		++passCount;
		const char* marker = std::getenv("RISE_RED_PROVE_MARKER");
		if( marker && std::strstr(name, marker) ) std::cout << "  PASS: " << name << std::endl;
	}
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

static void TestRasterStorage()
{
	struct Reader final : IRasterImageReader, Reference
	{
		bool ended = false;
		unsigned int side = 1;
		bool BeginRead( unsigned int& w, unsigned int& h ) override { w = h = side; return true; }
		void ReadColor( RISEColor& color, unsigned int, unsigned int ) override
		{
			color = RISEColor( 0.125, 0.25, 0.5, 1.0 );
		}
		void EndRead() override { ended = true; }
	};
	const RISEColor blank( 0.0, 0.0, 0.0, 1.0 );
	const RISEColor marker( 0.5, 0.25, 0.125, 1.0 );
	auto* parent = new RISERasterImage( 6, 4, blank );
	parent->SetPEL( 4, 3, marker );
	auto* view = new RISERasterImage( parent, Rect( 1, 2, 4, 5 ) );
	auto* copy = new RISERasterImage( *view );
	Check( copy->GetStride() == 6 && copy->GetPEL( 2, 2 ).base.r == 0.5,
		"strided image copies retain the last row and its stride" );
	view->SetPEL( 2, 2, blank );
	Check( copy->GetPEL( 2, 2 ).base.r == 0.5 && parent->GetPEL( 4, 3 ).base.r == 0,
		"image copies are independent and views write through" );
	view->SetPEL( 2, 2, marker );
	Reader reader;
	parent->LoadImage( &reader );
	Check( reader.ended && parent->GetWidth() == 1 && parent->GetPEL( 0, 0 ).base.r == 0.125,
		"raster reload replaces dimensions and closes the reader" );
	parent->release();
	Check( view->GetPEL( 2, 2 ).base.r == 0.5, "view retains the old pixel allocation after source reload" );
	view->release();
	copy->release();

	auto* adopted = new RISERasterImage( new RISEColor[1]{marker}, 1, 1, true );
	auto* retained = new RISERasterImage( adopted );
	adopted->release();
	Check( retained->GetPEL( 0, 0 ).base.r == 0.5, "view retains adopted pixels" );
	retained->release();

	RISEColor external[] = {marker};
	auto* borrowed = new RISERasterImage( external, 1, 1, false );
	borrowed->SetPEL( 0, 0, blank );
	borrowed->release();
	Check( external[0].base.r == 0, "borrowed pixels remain caller-owned and writable" );

	struct OwningSource final : RISERasterImage
	{
		std::unique_ptr<RISEColor[]> owner;
		bool& destroyed;
		OwningSource( RISEColor* pixels, bool& ended ) :
			RISERasterImage( pixels, 1, 1, false ), owner( pixels ), destroyed( ended ) {}
		~OwningSource() override { destroyed = true; }
	};
	bool destroyed = false;
	auto* source = new OwningSource( new RISEColor[1]{marker}, destroyed );
	auto* borrowedView = new RISERasterImage( source );
	auto* nested = new RISERasterImage( borrowedView );
	source->release();
	Check( !destroyed && borrowedView->GetPEL( 0, 0 ).base.r == 0.5,
		"borrowed views retain a source that owns its external storage" );
	reader.side = 2;
	borrowedView->LoadImage( &reader );
	borrowedView->release();
	Check( !destroyed && nested->GetPEL( 0, 0 ).base.r == 0.5,
		"nested borrowed view retains its allocation after an intermediate view reloads" );
	nested->release();
	Check( destroyed, "the last borrowed view releases its source" );
}

static void TestHDRWriterBounds()
{
	auto* buffer = new MemoryBuffer();
	IRasterImageWriter* writer = nullptr;
	const bool made = RISE_API_CreateHDRWriter( &writer, *buffer, eColorSpace_Rec709RGB_Linear );
	if( !made || !writer ) {
		Check( false, "create HDR writer" );
		buffer->release();
		return;
	}
	writer->BeginWrite( 65536, 65536 );
	writer->WriteColor( RISEColor(1, 0, 0, 1), 0, 0 );
	writer->EndWrite();
	Check( buffer->getCurPos() == 0, "invalid HDR dimensions emit no partial header or pixels" );
	writer->BeginWrite( 1, 1 );
	writer->WriteColor( RISEColor(1, 0, 0, 1), 0, 0 );
	writer->EndWrite();
	const std::string bytes( buffer->Pointer(), buffer->getCurPos() );
	const std::string dimensions = "-Y 1 +X 1\n";
	const auto offset = bytes.find( dimensions );
	// The existing encoder scales the frexp mantissa by 255.9999, then truncates.
	const unsigned char expected[] = { 127, 0, 0, 129 };
	Check( offset != std::string::npos && bytes.size() == offset + dimensions.size() + sizeof(expected) &&
		std::memcmp( bytes.data() + offset + dimensions.size(), expected, sizeof(expected) ) == 0,
		"HDR writer recovers with one independently specified RGBE pixel" );
	writer->release();
	buffer->release();
}

static void TestTGAWriterBounds()
{
	auto rejectDimensions = []( unsigned int width, unsigned int height ) {
		auto* buffer = new MemoryBuffer();
		auto* writer = new TGAWriter( *buffer, eColorSpace_Rec709RGB_Linear );
		bool rejected = false;
		try { writer->BeginWrite(width, height); }
		catch( const std::invalid_argument& ) { rejected = true; }
		catch( const std::length_error& ) { rejected = true; }
		Check( rejected && buffer->getCurPos() == 0,
			"TGA writer: invalid dimensions throw before emitting any header" );
		writer->release();
		buffer->release();
		return rejected;
	};
	const bool widthRejected = rejectDimensions(65536, 1);
	const bool heightRejected = rejectDimensions(1, 65536);
	rejectDimensions(0, 1);
	rejectDimensions(1, 0);
	// The old writer allocates gigabytes for this overflow case. First prove
	// that dimension validation exists, keeping the baseline RED run bounded.
	const bool validated = widthRejected && heightRejected;
	if( validated ) rejectDimensions(65535, 65535);

	for( bool wide : { false, true } ) {
		const unsigned int width = wide ? 65535 : 1;
		const unsigned int height = wide ? 1 : 65535;
		auto* buffer = new MemoryBuffer();
		auto* writer = new TGAWriter( *buffer, eColorSpace_Rec709RGB_Linear );
		writer->BeginWrite(width, height);
		const RISEColor black(0, 0, 0, 1);
		for( unsigned int y = 0; y < height; ++y ) {
			for( unsigned int x = 0; x < width; ++x ) writer->WriteColor(black, x, y);
		}
		writer->EndWrite();
		const unsigned char expectedHeader[] = {
			0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			static_cast<unsigned char>(wide ? 255 : 1), static_cast<unsigned char>(wide ? 255 : 0),
			static_cast<unsigned char>(wide ? 1 : 255), static_cast<unsigned char>(wide ? 0 : 255), 32, 8
		};
		bool exact = buffer->getCurPos() == 18 + 4 * 65535 &&
			std::memcmp(buffer->Pointer(), expectedHeader, sizeof(expectedHeader)) == 0;
		const auto* bytes = reinterpret_cast<const unsigned char*>(buffer->Pointer());
		for( unsigned int offset = 18; exact && offset < buffer->getCurPos(); offset += 4 ) {
			exact = bytes[offset] == 0 && bytes[offset+1] == 0 && bytes[offset+2] == 0 && bytes[offset+3] == 255;
		}
		Check( exact, "TGA writer: 65535-axis image preserves exact header, byte count, and BGRA pixels" );
		writer->release();
		buffer->release();
	}

	if( !validated ) return; // Avoid exercising known unsafe baseline paths.
	auto* buffer = new MemoryBuffer();
	auto* writer = new TGAWriter( *buffer, eColorSpace_Rec709RGB_Linear );
	writer->EndWrite();
	Check( buffer->getCurPos() == 0, "TGA writer: EndWrite before BeginWrite emits no bytes" );
	writer->BeginWrite(2, 1);
	writer->WriteColor(RISEColor(1, 0, 0, 1), 0, 0);
	buffer->seek(IBuffer::START, 0);
	writer->BeginWrite(1, 1);
	writer->WriteColor(RISEColor(0, 0, 0, 1), 0, 0);
	writer->WriteColor(RISEColor(1, 1, 1, 1), 1, 0);
	writer->WriteColor(RISEColor(1, 1, 1, 1), 0, 1);
	writer->EndWrite();
	const unsigned char expected[] = { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 32, 8, 0, 0, 0, 255 };
	Check( buffer->getCurPos() == sizeof(expected) && std::memcmp(buffer->Pointer(), expected, sizeof(expected)) == 0,
		"TGA writer: repeated BeginWrite resets pixels and invalid coordinates cannot alter output" );
	writer->BeginWrite(1, 1);
	const auto before = buffer->getCurPos();
	bool rejected = false;
	try { writer->BeginWrite(0, 1); }
	catch( const std::invalid_argument& ) { rejected = true; }
	writer->WriteColor(RISEColor(1, 1, 1, 1), 0, 0);
	writer->EndWrite();
	Check( rejected && buffer->getCurPos() == before,
		"TGA writer: invalid replacement clears previous active image" );
	writer->release();
	buffer->release();

	for( unsigned int capacity : { 17U, 20U } ) {
		char storage[20] = {};
		auto* borrowed = new MemoryBuffer(storage, capacity, false);
		auto* failed = new TGAWriter( *borrowed, eColorSpace_Rec709RGB_Linear );
		bool reported = false;
		try {
			failed->BeginWrite(1, 1);
			failed->WriteColor(RISEColor(0, 0, 0, 1), 0, 0);
			failed->EndWrite();
		} catch( const std::runtime_error& ) { reported = true; }
		if( capacity == 17 ) failed->EndWrite();
		Check( reported && borrowed->getCurPos() == (capacity == 17 ? 0U : 18U),
			"TGA writer: header and pixel write failures surface as exceptions without an active failed header" );
		failed->release();
		borrowed->release();
	}
}

//----------------------------------------------------------------------
// Byte-buffer helpers.
//----------------------------------------------------------------------
static void AppendStr( std::vector<char>& v, const char* s )
{
	for( const char* p = s; *p; ++p ) v.push_back( *p );
}

static void AppendByte( std::vector<char>& v, unsigned char b )
{
	v.push_back( static_cast<char>( b ) );
}

static std::string TmpDir()
{
	const char* tmp = getenv( "TMPDIR" );
	std::string dir = tmp ? std::string( tmp ) : ( RISE::TestSupport::ScratchBaseDir() + "/" );
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	return dir;
}

static std::vector<char> ReadFileBytes( const std::string& path )
{
	std::ifstream f( path.c_str(), std::ios::binary );
	return std::vector<char>( (std::istreambuf_iterator<char>( f )), std::istreambuf_iterator<char>() );
}

static void TestJPEGReload()
{
	namespace fs = std::filesystem;
	const auto sameColor = [](const RISEColor& a, const RISEColor& b) {
		return a.base.r == b.base.r && a.base.g == b.base.g && a.base.b == b.base.b && a.a == b.a;
	};
	const fs::path relative = "scenes/Tests/Painters/textures/test_pattern.jpg";
	fs::path fixture;
	if( const char* media = std::getenv("RISE_MEDIA_PATH") ) {
		if( fs::is_regular_file(fs::path(media) / relative) ) fixture = fs::path(media) / relative;
	}
	for( auto root = fs::current_path(); fixture.empty(); root = root.parent_path() ) {
		if( fs::is_regular_file(root / relative) ) fixture = root / relative;
		if( root == root.parent_path() ) break;
	}
	Check( !fixture.empty(), "JPEG reload: locate shipped JPEG fixture" );
	if( fixture.empty() ) return;

	auto* buffer = new MemoryBuffer( fixture.string().c_str() );
	auto* reader = new JPEGReader( *buffer, eColorSpace_Rec709RGB_Linear );
	unsigned int width = 0, height = 0;
	const bool decoded = reader->BeginRead( width, height );
	Check( decoded && width == 16 && height == 16, "JPEG reload: decode the shipped 16x16 fixture" );
	if( decoded && width == 16 && height == 16 ) {
		std::vector<RISEColor> expected( width * height );
		for( unsigned int y = 0; y < height; ++y ) {
			for( unsigned int x = 0; x < width; ++x ) reader->ReadColor( expected[y*width+x], x, y );
		}
		for( unsigned int reload = 0; reload < 4; ++reload ) {
			const bool reloaded = buffer->seek(IBuffer::START, 0) && reader->BeginRead(width, height);
			bool identical = reloaded && width == 16 && height == 16;
			if( identical ) {
				for( unsigned int y = 0; y < height; ++y ) {
					for( unsigned int x = 0; x < width; ++x ) {
						RISEColor pixel;
						reader->ReadColor( pixel, x, y );
						identical &= sameColor(pixel, expected[y*width+x]);
					}
				}
			}
			Check( identical, "JPEG reload: repeated rewind and decode preserves every pixel and dimensions" );
		}

		Check( buffer->Resize(0, true), "JPEG reload: replace the source with an empty input" );
		width = 123;
		height = 456;
		Check( !reader->BeginRead(width, height) && width == 0 && height == 0,
			"JPEG reload: failed decode clears output dimensions" );
		const RISEColor poison( -1000, -2000, -3000, -4000 );
		RISEColor pixel = poison;
		reader->ReadColor( pixel, 0, 0 );
		Check( sameColor(pixel, poison), "JPEG reload: failed decode cannot read the previous image" );
	}
	reader->release();
	buffer->release();
}

//----------------------------------------------------------------------
// Reader drivers.  Each builds a MemoryBuffer over the crafted bytes (no
// ownership; the caller's vector outlives the reader), runs BeginRead, and
// tears everything down so a leak / double-free would surface here.
//----------------------------------------------------------------------
static bool RunTGA( std::vector<char>& bytes, unsigned int& w, unsigned int& h,
					RISEColor* px0 = 0, RISEColor* pxCorner = 0 )
{
	w = h = 0;
	MemoryBuffer* mb = new MemoryBuffer( bytes.data(), static_cast<unsigned int>( bytes.size() ), false );
	TGAReader* reader = new TGAReader( *mb, eColorSpace_Rec709RGB_Linear );
	const bool ok = reader->BeginRead( w, h );
	if( ok && px0 && w >= 1 && h >= 1 )           reader->ReadColor( *px0, 0, 0 );
	if( ok && pxCorner && w >= 1 && h >= 1 )       reader->ReadColor( *pxCorner, w-1, h-1 );
	reader->release();
	mb->release();
	return ok;
}

static bool RunHDR( std::vector<char>& bytes, unsigned int& w, unsigned int& h,
					RISEColor* px0 = 0, RISEColor* pxLast = 0 )
{
	w = h = 0;
	MemoryBuffer* mb = new MemoryBuffer( bytes.data(), static_cast<unsigned int>( bytes.size() ), false );
	HDRReader* reader = new HDRReader( *mb );
	const bool ok = reader->BeginRead( w, h );
	if( ok && px0 && w >= 1 )        reader->ReadColor( *px0,   0,   0 );
	if( ok && pxLast && w >= 1 )     reader->ReadColor( *pxLast, w-1, 0 );
	reader->release();
	mb->release();
	return ok;
}

#ifndef NO_PNG_SUPPORT
static bool RunPNG( std::vector<char>& bytes, unsigned int& w, unsigned int& h, RISEColor* pixel = nullptr )
{
	w = h = 0;
	MemoryBuffer* mb = new MemoryBuffer( bytes.data(), static_cast<unsigned int>( bytes.size() ), false );
	PNGReader* reader = new PNGReader( *mb, eColorSpace_Rec709RGB_Linear );
	const bool ok = reader->BeginRead( w, h );
	if( ok && pixel ) reader->ReadColor( *pixel, 0, 0 );
	reader->release();
	mb->release();
	return ok;
}
#endif

#ifndef NO_TIFF_SUPPORT
static bool RunTIFF( std::vector<char>& bytes, unsigned int& w, unsigned int& h,
					 RISEColor* px0 = 0 )
{
	w = h = 0;
	struct DriverBuffer final : MemoryBuffer {
		bool& destroyed;
		DriverBuffer(std::vector<char>& data, bool& flag) :
			MemoryBuffer(data.data(), static_cast<unsigned int>(data.size()), false), destroyed(flag) {}
		~DriverBuffer() override { destroyed = true; }
	};
	struct DriverReader final : TIFFReader {
		bool& destroyed;
		DriverReader(IReadBuffer& input, bool& flag) :
			TIFFReader(input, eColorSpace_Rec709RGB_Linear), destroyed(flag) {}
		~DriverReader() override { destroyed = true; }
	};
	bool bufferDestroyed = false, readerDestroyed = false;
	auto* mb = new DriverBuffer(bytes, bufferDestroyed);
	auto* reader = new DriverReader(*mb, readerDestroyed);
	const bool ok = reader->BeginRead( w, h );
	if( ok && px0 && w >= 1 && h >= 1 )           reader->ReadColor( *px0, 0, 0 );
	reader->release();
	Check(readerDestroyed && mb->refcount() == 1,
		"TIFF driver teardown: reader destruction releases its buffer reference");
	mb->release();
	Check(bufferDestroyed, "TIFF driver teardown: borrowed buffer wrapper is destroyed");
	return ok;
}
#endif

//======================================================================
// TGA
//======================================================================
static std::vector<char> MakeTGAHeader( unsigned short w, unsigned short h, unsigned char bpp )
{
	std::vector<char> v;
	for( int i = 0; i < 12; ++i ) AppendByte( v, 0 );		// 12-byte preheader
	v[2] = 2; // Uncompressed truecolor image.
	AppendByte( v, (unsigned char)( w & 0xFF ) );
	AppendByte( v, (unsigned char)( (w >> 8) & 0xFF ) );
	AppendByte( v, (unsigned char)( h & 0xFF ) );
	AppendByte( v, (unsigned char)( (h >> 8) & 0xFF ) );
	AppendByte( v, bpp );
	AppendByte( v, 8 ); // Eight alpha bits; bottom-left origin.
	return v;
}

static void TestTGA()
{
	std::cout << "A: TGA overflow / zero / valid dimensions" << std::endl;
	unsigned int w, h;

	// 32768*32768*4 == 2^32 -> wraps the 32-bit product to 0 -> every write OOB.
	std::vector<char> over = MakeTGAHeader( 0x8000, 0x8000, 32 );
	Check( !RunTGA( over, w, h ), "A: overflow-dimension TGA returns false" );

	std::vector<char> zw = MakeTGAHeader( 0, 4, 32 );
	Check( !RunTGA( zw, w, h ), "A: zero-width TGA returns false" );
	std::vector<char> zh = MakeTGAHeader( 4, 0, 32 );
	Check( !RunTGA( zh, w, h ), "A: zero-height TGA returns false" );

	// Independently specified BGRA pixels follow the complete 18-byte header.
	// Both rows have R>G>B and full alpha.
	std::vector<char> good = MakeTGAHeader( 2, 2, 32 );
	for( int p = 0; p < 4; ++p ) { AppendByte( good, 10 ); AppendByte( good, 100 ); AppendByte( good, 200 ); AppendByte( good, 255 ); }
	RISEColor tga0, tgaCorner;
	const bool tgaOk = RunTGA( good, w, h, &tga0, &tgaCorner );
	Check( tgaOk && w == 2 && h == 2, "A: valid 2x2 TGA decodes" );
	Check( tgaOk && tga0.base.r > tga0.base.g && tga0.base.g > tga0.base.b && tga0.base.b > 0.0,
		"A: TGA pixel (0,0) decodes R>G>B (channel order)" );
	Check( tgaOk && tgaCorner.base.r > tgaCorner.base.g && tgaCorner.base.g > tgaCorner.base.b,
		"A: TGA pixel (1,1) decodes R>G>B (backwards-y row offset)" );
}

static void TestTGAFormat()
{
	const RGBA8 samples[] = { {200, 100, 10, 64}, {20, 210, 50, 128},
		{60, 70, 220, 192}, {230, 90, 80, 255} };
	// Explicit file-order tables for bottom-left, bottom-right, top-left,
	// and top-right origins. Samples themselves are always in top-left order.
	const unsigned int orders[4][4] = { {2, 3, 0, 1}, {3, 2, 1, 0}, {0, 1, 2, 3}, {1, 0, 3, 2} };
	auto sameColor = []( const RISEColor& actual, const RISEColor& expected ) {
		return std::fabs(actual.base.r - expected.base.r) < 1e-12 &&
			std::fabs(actual.base.g - expected.base.g) < 1e-12 &&
			std::fabs(actual.base.b - expected.base.b) < 1e-12 && std::fabs(actual.a - expected.a) < 1e-12;
	};
	std::vector<char> valid;
	bool checkedLayout = false;
	for( unsigned int origin = 0; origin < 4; ++origin ) {
		for( unsigned char alphaBits : { 0, 8 } ) {
			auto bytes = MakeTGAHeader(2, 2, 32);
			bytes[0] = 3;
			bytes[17] = static_cast<char>(origin * 0x10 + alphaBits);
			for( unsigned char idByte : { 'I', 'D', '!' } ) AppendByte(bytes, idByte);
			for( const auto index : orders[origin] ) {
				const auto& pixel = samples[index];
				for( unsigned char channel : { pixel.b, pixel.g, pixel.r, pixel.a } ) AppendByte(bytes, channel);
			}
			auto* buffer = new MemoryBuffer(bytes.data(), static_cast<unsigned int>(bytes.size()), false);
			auto* reader = new TGAReader(*buffer, eColorSpace_Rec709RGB_Linear);
			unsigned int width = 0, height = 0;
			bool exact = reader->BeginRead(width, height) && width == 2 && height == 2;
			if( exact ) {
				for( unsigned int i = 0; i < 4; ++i ) {
					RISEColor actual, expected;
					reader->ReadColor(actual, i % 2, i / 2);
					auto sample = samples[i];
					if( alphaBits == 0 ) sample.a = 255;
					expected.SetFromIntegerized<Rec709RGBPel, unsigned char>(sample, 255.0);
					exact &= sameColor(actual, expected);
				}
			}
			Check( exact, "TGA reader: 18-byte BGRA header, ID, both origin axes, and descriptor alpha decode exactly" );
			if( exact ) {
				const RISEColor poison(-1000, -2000, -3000, -4000);
				RISEColor outside = poison;
				reader->ReadColor(outside, 2, 0);
				reader->ReadColor(outside, 0, 2);
				Check( sameColor(outside, poison), "TGA reader: out-of-range coordinates preserve caller color" );
			}
			checkedLayout |= exact;
			reader->release();
			buffer->release();
			valid = bytes;
		}
	}
	unsigned int width = 123, height = 456;
	for( size_t size : { size_t(17), size_t(20), valid.size() - 1 } ) {
		auto truncated = valid;
		truncated.resize(size);
		Check( !RunTGA(truncated, width, height) && width == 0 && height == 0,
			"TGA reader: truncated header, ID, or final pixel fails with empty dimensions" );
	}
	for( unsigned int mode = 0; mode < 5; ++mode ) {
		auto unsupported = valid;
		if( mode == 0 ) unsupported[2] = 10; // RLE.
		if( mode == 1 ) unsupported[1] = 1; // Color map.
		if( mode == 2 ) unsupported[16] = 24;
		if( mode == 3 ) unsupported[17] = 4; // Unsupported attribute depth.
		if( mode == 4 ) unsupported[17] = 0x48; // Interleaved rows.
		Check( !RunTGA(unsupported, width, height), "TGA reader: unsupported encodings are rejected explicitly" );
	}
	if( !checkedLayout ) return; // The baseline reader cannot safely run bounds/reset probes.
	auto* buffer = new MemoryBuffer(static_cast<unsigned int>(valid.size()));
	buffer->setBytes(valid.data(), static_cast<unsigned int>(valid.size()));
	auto* reader = new TGAReader(*buffer, eColorSpace_Rec709RGB_Linear);
	Check( reader->BeginRead(width, height), "TGA reader: initial decode for failed-reload regression" );
	Check( buffer->Resize(0, true), "TGA reader: shrink input for failed-reload regression" );
	const bool failedReload = !reader->BeginRead(width, height);
	const RISEColor poison(-1000, -2000, -3000, -4000);
	RISEColor stale = poison;
	reader->ReadColor(stale, 0, 0);
	Check( failedReload && width == 0 && height == 0 && sameColor(stale, poison),
		"TGA reader: failed reload clears dimensions and previous pixels" );
	reader->EndRead();
	reader->EndRead();
	reader->release();
	buffer->release();

	auto* encoded = new MemoryBuffer();
	auto* writer = new TGAWriter(*encoded, eColorSpace_Rec709RGB_Linear);
	writer->BeginWrite(2, 2);
	for( unsigned int i = 0; i < 4; ++i ) {
		RISEColor color;
		color.SetFromIntegerized<Rec709RGBPel, unsigned char>(samples[i], 255.0);
		writer->WriteColor(color, i % 2, i / 2);
	}
	writer->EndWrite();
	writer->release();
	auto* decoded = new TGAReader(*encoded, eColorSpace_Rec709RGB_Linear);
	bool roundTrip = decoded->BeginRead(width, height) && width == 2 && height == 2;
	if( roundTrip ) {
		for( unsigned int i = 0; i < 4; ++i ) {
			RISEColor color;
			decoded->ReadColor(color, i % 2, i / 2);
			const auto actual = color.Integerize<Rec709RGBPel, unsigned char>(255.0);
			roundTrip &= std::abs(int(actual.r) - samples[i].r) <= 1 &&
				std::abs(int(actual.g) - samples[i].g) <= 1 &&
				std::abs(int(actual.b) - samples[i].b) <= 1 && actual.a == samples[i].a;
		}
	}
	Check( roundTrip, "TGA reader: writer roundtrip preserves all RGBA pixels and image orientation" );
	decoded->release();
	encoded->release();
}

//======================================================================
// HDR (Radiance RGBE)
//======================================================================
static std::vector<char> MakeHDRHeader( const char* resolutionLine )
{
	std::vector<char> v;
	AppendStr( v, "#?RADIANCE\n" );
	AppendStr( v, "FORMAT=32-bit_rle_rgbe\n" );
	AppendStr( v, resolutionLine );							// caller supplies trailing '\n'
	return v;
}

static void TestHDRNewRLE()
{
	std::cout << "B: HDR new-RLE overflow-dims / over-long run / truncated / valid" << std::endl;
	unsigned int w, h;

	// Overflow dimensions rejected before allocation.
	std::vector<char> over = MakeHDRHeader( "-Y 100000 +X 100000\n" );
	Check( !RunHDR( over, w, h ), "B: overflow-dimension HDR returns false" );

	std::vector<char> wrapped = MakeHDRHeader( "-Y 2147483648 +X 2147483648\n" );
	Check( !RunHDR( wrapped, w, h ), "B: HDR rejects dimensions whose byte count wraps 64 bits" );

	std::vector<char> missingAxis = MakeHDRHeader( "-Y 1\n" );
	Check( !RunHDR( missingAxis, w, h ), "B: HDR rejects a resolution line without an X axis" );

	std::vector<char> wrongAxis = MakeHDRHeader( "-Y 1 AX 1\n" );
	AppendByte( wrongAxis, 10 ); AppendByte( wrongAxis, 20 );
	AppendByte( wrongAxis, 30 ); AppendByte( wrongAxis, 130 );
	Check( !RunHDR( wrongAxis, w, h ), "B: HDR rejects an invalid X axis even with a complete pixel" );

	// Over-long RLE run (num=200 -> length 72) into a 8-wide scanline.
	std::vector<char> run = MakeHDRHeader( "-Y 2 +X 8\n" );
	AppendByte( run, 2 ); AppendByte( run, 2 ); AppendByte( run, 0 ); AppendByte( run, 8 );
	AppendByte( run, 200 ); AppendByte( run, 0xAB );
	Check( !RunHDR( run, w, h ), "B: over-long-RLE HDR returns false" );

	// Truncated scanline (col header, no run codes) -> zero-length run -> reject.
	std::vector<char> trunc = MakeHDRHeader( "-Y 2 +X 8\n" );
	AppendByte( trunc, 2 ); AppendByte( trunc, 2 ); AppendByte( trunc, 0 ); AppendByte( trunc, 8 );
	Check( !RunHDR( trunc, w, h ), "B: truncated-scanline HDR returns false" );

	// Valid 8x1 new-RLE: four raw runs of 8 values; R<G<B so we can assert
	// the decode order wasn't transposed.
	std::vector<char> good = MakeHDRHeader( "-Y 1 +X 8\n" );
	AppendByte( good, 2 ); AppendByte( good, 2 ); AppendByte( good, 0 ); AppendByte( good, 8 );
	const unsigned char comp[4][8] = {
		{ 10, 20, 30, 40, 50, 60, 70, 80 }, { 11, 21, 31, 41, 51, 61, 71, 81 },
		{ 12, 22, 32, 42, 52, 62, 72, 82 }, { 130, 130, 130, 130, 130, 130, 130, 130 }
	};
	for( int c = 0; c < 4; ++c ) {
		AppendByte( good, 8 );
		for( int p = 0; p < 8; ++p ) AppendByte( good, comp[c][p] );
	}
	RISEColor px0;
	const bool ok = RunHDR( good, w, h, &px0 );
	Check( ok && w == 8 && h == 1, "B: valid 8x1 new-RLE HDR decodes" );
	Check( ok && px0.a == 1.0, "B: decoded pixel alpha == 1.0" );
	Check( ok && px0.base.r > 0.0 && px0.base.r < px0.base.g && px0.base.g < px0.base.b,
		"B: pixel channels decode in R<G<B order (no transposition)" );
	auto wrongWidth = good;
	wrongWidth[MakeHDRHeader( "-Y 1 +X 8\n" ).size()+3] = 5;
	Check( !RunHDR( wrongWidth, w, h ), "B: HDR rejects an RLE scanline with the wrong encoded width" );
	good.pop_back();
	Check( !RunHDR( good, w, h ), "B: HDR rejects a missing final RLE literal" );
}

static void TestHDROldRLE()
{
	std::cout << "C: HDR old-RLE valid raw / valid run / crafted run-at-pixel-0" << std::endl;
	unsigned int w, h;

	std::vector<char> headerOnly = MakeHDRHeader( "-Y 1 +X 1\n" );
	Check( !RunHDR( headerOnly, w, h ), "C: HDR rejects a header without the first pixel" );

	std::vector<char> longComment;
	AppendStr( longComment, "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n#" );
	longComment.insert( longComment.end(), 1023, ' ' );
	AppendStr( longComment, "-Y 1 +X 2\n-Y 1 +X 1\n" );
	AppendByte( longComment, 2 ); AppendByte( longComment, 2 );
	AppendByte( longComment, 1 ); AppendByte( longComment, 130 );
	Check( RunHDR( longComment, w, h ) && w == 1 && h == 1,
		"C: HDR skips long comment continuations and accepts a flat pixel with an RLE-like prefix" );

	std::vector<char> highBitBlue = MakeHDRHeader( "-Y 1 +X 8\n" );
	for( int i=0; i<8; ++i ) {
		AppendByte( highBitBlue, 2 ); AppendByte( highBitBlue, 2 );
		AppendByte( highBitBlue, 128 ); AppendByte( highBitBlue, 130 );
	}
	Check( RunHDR( highBitBlue, w, h ) && w == 8 && h == 1,
		"C: HDR accepts a flat scanline with a high-bit blue sample" );

	std::vector<char> xyze;
	AppendStr( xyze, "#?RADIANCE\nFORMAT=32-bit_rle_xyze\n-Y 1 +X 1\n" );
	AppendByte( xyze, 10 ); AppendByte( xyze, 20 ); AppendByte( xyze, 30 ); AppendByte( xyze, 130 );
	Check( !RunHDR( xyze, w, h ), "C: HDR rejects unsupported XYZE encoding" );

	// Valid old-RLE raw scanline (first byte != 2 selects old scheme; != 1,1,1
	// selects the raw branch).  Exercises the `col`-not-`buf` store fix.
	std::vector<char> raw = MakeHDRHeader( "-Y 1 +X 2\n" );
	AppendByte( raw, 10 ); AppendByte( raw, 20 ); AppendByte( raw, 30 ); AppendByte( raw, 130 );	// px0
	AppendByte( raw, 11 ); AppendByte( raw, 21 ); AppendByte( raw, 31 ); AppendByte( raw, 131 );	// px1
	RISEColor px0, px1;
	bool ok = RunHDR( raw, w, h, &px0, &px1 );
	Check( ok && w == 2 && h == 1, "C: valid old-RLE raw HDR decodes" );
	Check( ok && px0.base.r > 0.0 && px0.base.r < px0.base.g && px0.base.g < px0.base.b,
		"C: old-RLE raw pixel channels decode in order (col, not header buf)" );

	// Valid old-RLE with a run: px0 raw, then a (1,1,1,2) header repeats px0
	// twice.  Exercises the bounds-checked run writes + the back-reference read.
	std::vector<char> withrun = MakeHDRHeader( "-Y 1 +X 3\n" );
	AppendByte( withrun, 10 ); AppendByte( withrun, 20 ); AppendByte( withrun, 30 ); AppendByte( withrun, 130 ); // px0
	AppendByte( withrun, 1 ); AppendByte( withrun, 1 ); AppendByte( withrun, 1 ); AppendByte( withrun, 2 );      // run x2
	RISEColor last;
	ok = RunHDR( withrun, w, h, 0, &last );
	Check( ok && w == 3 && h == 1, "C: valid old-RLE run HDR decodes" );
	Check( ok && last.base.r > 0.0 && last.base.r < last.base.g && last.base.g < last.base.b,
		"C: old-RLE run copies the previous pixel (back-reference)" );

	// Reference Radiance old-RLE counts are little-endian byte chains.
	const auto chained = [&](unsigned int width, const std::vector<unsigned char>& packets,
		const RISEColor& expectedLast, const char* label) {
		auto bytes = MakeHDRHeader(("-Y 1 +X " + std::to_string(width) + "\n").c_str());
		for(auto byte : packets) AppendByte(bytes,byte);
		RISEColor first, final;
		const bool decoded=RunHDR(bytes,w,h,&first,&final);
		Check(decoded && w==width && h==1 && first.base.r==px0.base.r &&
			final.base.r==expectedLast.base.r && final.base.g==expectedLast.base.g &&
			final.base.b==expectedLast.base.b,label);
	};
	chained(257,{10,20,30,130, 1,1,1,0, 1,1,1,1},px0,
		"C: old-RLE chained count accepts zero low byte and emits 256 repeats");
	chained(258,{10,20,30,130, 1,1,1,1, 1,1,1,1},px0,
		"C: old-RLE chained count emits 257 repeats");
	chained(259,{10,20,30,130, 1,1,1,1, 11,21,31,131, 1,1,1,0, 1,1,1,1},px1,
		"C: old-RLE chained count shift resets after a literal");
	chained(2,{10,20,30,130, 1,1,1,0, 11,21,31,131},px1,
		"C: old-RLE zero count can precede a literal");
	for(const auto& packets : std::vector<std::vector<unsigned char>>{
		{10,20,30,130, 1,1,1,0},
		{10,20,30,130, 1,1,1,0, 1,1,1,0, 1,1,1,1},
		{10,20,30,130, 1,1,1,0, 1,1,1,0, 1,1,1,0, 1,1,1,0, 1,1,1,0}}) {
		auto bytes=MakeHDRHeader("-Y 1 +X 258\n");
		for(auto byte : packets) AppendByte(bytes,byte);
		Check(!RunHDR(bytes,w,h) && w==0 && h==0,
			"C: old-RLE chained count rejects truncated oversized and over-shifted packets");
	}

	// Crafted old-RLE run at pixel 0: (1,1,1,5) at scanline start references
	// pos-step == -1 -> the back-reference bounds guard must reject.
	std::vector<char> badrun = MakeHDRHeader( "-Y 1 +X 4\n" );
	AppendByte( badrun, 1 ); AppendByte( badrun, 1 ); AppendByte( badrun, 1 ); AppendByte( badrun, 5 );
	Check( !RunHDR( badrun, w, h ), "C: old-RLE run at pixel 0 (back-ref underflow) returns false" );

	std::vector<char> secondRowRun = MakeHDRHeader( "-Y 2 +X 1\n" );
	AppendByte( secondRowRun, 10 ); AppendByte( secondRowRun, 20 );
	AppendByte( secondRowRun, 30 ); AppendByte( secondRowRun, 130 );
	AppendByte( secondRowRun, 1 ); AppendByte( secondRowRun, 1 );
	AppendByte( secondRowRun, 1 ); AppendByte( secondRowRun, 1 );
	Check( !RunHDR( secondRowRun, w, h ), "C: old-RLE run cannot reference the previous scanline" );
}

//======================================================================
// PNG — crafted IHDR with overflow dimensions (valid chunk CRCs).
//======================================================================
#ifndef NO_PNG_SUPPORT
static void AppendU32BE( std::vector<char>& v, unsigned int x )
{
	AppendByte( v, (x >> 24) & 0xFF ); AppendByte( v, (x >> 16) & 0xFF );
	AppendByte( v, (x >> 8) & 0xFF );  AppendByte( v, x & 0xFF );
}

static void AppendPNGChunk( std::vector<char>& v, const char type[4], const std::vector<unsigned char>& data )
{
	AppendU32BE( v, (unsigned int)data.size() );
	std::vector<unsigned char> tc;
	tc.insert( tc.end(), type, type + 4 );
	tc.insert( tc.end(), data.begin(), data.end() );
	for( int i = 0; i < 4; ++i ) v.push_back( type[i] );
	for( size_t i = 0; i < data.size(); ++i ) v.push_back( (char)data[i] );
	unsigned long crc = crc32( 0L, Z_NULL, 0 );
	crc = crc32( crc, (const Bytef*)tc.data(), (uInt)tc.size() );
	AppendU32BE( v, (unsigned int)crc );
}

static void TestPNG()
{
	std::cout << "D: PNG with 100000x100000 IHDR is rejected before allocation" << std::endl;

	std::vector<char> png;
	const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	for( int i = 0; i < 8; ++i ) AppendByte( png, sig[i] );

	// IHDR: 100000x100000, 8-bit, colour type 2 (RGB).  Both dims are < the
	// libpng default limit (1e6) so png_read_info accepts them; the 32-bit
	// height*stride (1e5 * (1e5*3)) = 3e10 wraps.  The fix's 64-bit ceiling
	// check rejects it.
	std::vector<unsigned char> ihdr;
	unsigned int dim = 100000;
	for( int s = 24; s >= 0; s -= 8 ) ihdr.push_back( (dim >> s) & 0xFF ); // width  BE
	for( int s = 24; s >= 0; s -= 8 ) ihdr.push_back( (dim >> s) & 0xFF ); // height BE
	ihdr.push_back( 8 );	// bit depth
	ihdr.push_back( 2 );	// colour type RGB
	ihdr.push_back( 0 );	// compression
	ihdr.push_back( 0 );	// filter
	ihdr.push_back( 0 );	// interlace
	AppendPNGChunk( png, "IHDR", ihdr );

	// A structurally-valid IDAT (content never decompressed — we reject before
	// png_read_row) so png_read_info has a chunk to stop at, then IEND.
	std::vector<unsigned char> idat; idat.push_back( 0x78 );
	AppendPNGChunk( png, "IDAT", idat );
	AppendPNGChunk( png, "IEND", std::vector<unsigned char>() );

	unsigned int w, h;
	Check( !RunPNG( png, w, h ), "D: overflow-dimension PNG returns false (no OOB alloc)" );

	std::vector<char> rgba16;
	for( unsigned char byte : sig ) AppendByte( rgba16, byte );
	AppendPNGChunk( rgba16, "IHDR", { 0,0,0,1, 0,0,0,1, 16,6,0,0,0 } );
	const unsigned char row[] = { 0, 0x12,0x34, 0x56,0x78, 0x9a,0xbc, 0xde,0xf0 };
	std::vector<unsigned char> compressed( compressBound( sizeof(row) ) );
	uLongf length = static_cast<uLongf>( compressed.size() );
	const int compressedOk = compress( compressed.data(), &length, row, sizeof(row) );
	Check( compressedOk == Z_OK, "D: create an independent 16-bit PNG scanline fixture" );
	if( compressedOk == Z_OK ) {
		compressed.resize( length );
		AppendPNGChunk( rgba16, "IDAT", compressed );
		AppendPNGChunk( rgba16, "IEND", {} );
		RISEColor pixel;
		const bool ok = RunPNG( rgba16, w, h, &pixel );
		const RISEColor expected( RISEPel( Rec709RGBPel( 0x1234 / 65535.0,
			0x5678 / 65535.0, 0x9abc / 65535.0 ) ), 0xdef0 / 65535.0 );
		Check( ok && w == 1 && h == 1 && std::fabs( pixel.a - expected.a ) < 1e-12 &&
			std::fabs( pixel.base.r - expected.base.r ) < 1e-12 &&
			std::fabs( pixel.base.g - expected.base.g ) < 1e-12 &&
			std::fabs( pixel.base.b - expected.base.b ) < 1e-12,
			"D: 16-bit PNG decodes independently specified network-order RGBA samples" );
		rgba16.pop_back();
		Check( !RunPNG( rgba16, w, h ), "D: short PNG read returns failure" );
	}

	for( unsigned char colorType : { 0, 4 } ) {
		std::vector<char> grayscale;
		for( unsigned char byte : sig ) AppendByte( grayscale, byte );
		AppendPNGChunk( grayscale, "IHDR", { 0,0,0,1, 0,0,0,1, 16,colorType,0,0,0 } );
		const unsigned char grayRow[] = { 0, 0x12,0x34, 0x56,0x78 };
		compressed.resize( compressBound( sizeof(grayRow) ) );
		length = static_cast<uLongf>( compressed.size() );
		if( compress( compressed.data(), &length, grayRow, colorType == 4 ? 5 : 3 ) != Z_OK ) {
			Check( false, "D: compress grayscale fixture" );
			continue;
		}
		compressed.resize( length );
		AppendPNGChunk( grayscale, "IDAT", compressed );
		AppendPNGChunk( grayscale, "IEND", {} );
		RISEColor pixel;
		const bool ok = RunPNG( grayscale, w, h, &pixel );
		const double gray = 0x1234 / 65535.0;
		const RISEPel expected( Rec709RGBPel( gray, gray, gray ) );
		const double alpha = colorType == 4 ? 0x5678 / 65535.0 : 1.0;
		Check( ok && w == 1 && h == 1 && std::fabs( pixel.a-alpha ) < 1e-12 &&
			std::fabs( pixel.base.r-expected.r ) < 1e-12 &&
			std::fabs( pixel.base.g-expected.g ) < 1e-12 &&
			std::fabs( pixel.base.b-expected.b ) < 1e-12,
			colorType == 4 ? "D: expanded grayscale preserves alpha" : "D: expanded grayscale has opaque alpha" );
	}
}
#endif

//======================================================================
// TIFF — real 2x2 via libtiff (happy path) + a dimension-patched copy
// (safely rejected).  TIFF is not exercised by any scene test, so this is
// the only coverage the reader has.
//======================================================================
#ifndef NO_TIFF_SUPPORT
// Patch the little-endian IFD: set the value field of ImageWidth (256),
// ImageLength (257) and RowsPerStrip (278) to 65535 so the resulting
// 65535*65535*4 == 1.7e10 exceeds the 2 GiB ceiling.  RowsPerStrip is patched
// too so the strip count stays 1 (matching the single strip actually present),
// keeping TIFFClientOpen happy so our size guard — not a strip-count error — is
// what rejects the file.
static void PatchTIFFDims( std::vector<char>& t )
{
	if( t.size() < 8 || t[0] != 'I' || t[1] != 'I' ) return;	// little-endian only
	unsigned char* b = (unsigned char*)t.data();
	auto rd32 = [&]( size_t o ) { return (unsigned int)b[o] | (b[o+1]<<8) | (b[o+2]<<16) | ((unsigned int)b[o+3]<<24); };
	auto rd16 = [&]( size_t o ) { return (unsigned int)b[o] | (b[o+1]<<8); };
	size_t ifd = rd32( 4 );
	if( ifd + 2 > t.size() ) return;
	unsigned int n = rd16( ifd );
	for( unsigned int i = 0; i < n; ++i ) {
		size_t e = ifd + 2 + (size_t)i * 12;
		if( e + 12 > t.size() ) break;
		unsigned int tag = rd16( e );
		if( tag == 256 || tag == 257 || tag == 278 ) {
			// Value field at e+8: write 65535 (low 2 bytes; valid for SHORT and LONG).
			b[e+8] = 0xFF; b[e+9] = 0xFF; b[e+10] = 0x00; b[e+11] = 0x00;
		}
	}
}

static void TestDamagedTIFFStrip()
{
	const std::string path = TmpDir() + "rise_bad_strip_" +
		std::to_string(RISE::TestSupport::CurrentProcessId()) + ".tif";
	TIFF* output = TIFFOpen( path.c_str(), "w" );
	if( !output ) { Check(false, "create damaged TIFF strip fixture"); return; }
	TIFFSetField( output, TIFFTAG_IMAGEWIDTH, 4 );
	TIFFSetField( output, TIFFTAG_IMAGELENGTH, 4 );
	TIFFSetField( output, TIFFTAG_SAMPLESPERPIXEL, 3 );
	TIFFSetField( output, TIFFTAG_BITSPERSAMPLE, 8 );
	TIFFSetField( output, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT );
	TIFFSetField( output, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG );
	TIFFSetField( output, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB );
	TIFFSetField( output, TIFFTAG_ROWSPERSTRIP, 1 );
	TIFFSetField( output, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE );
	bool written = true;
	for( unsigned int y = 0; y < 4; ++y ) {
		unsigned char row[12];
		for( unsigned int x = 0; x < 4; ++x ) {
			row[x*3] = static_cast<unsigned char>(20 + y*40);
			row[x*3+1] = 40;
			row[x*3+2] = 80;
		}
		written &= TIFFWriteScanline( output, row, y, 0 ) >= 0;
	}
	TIFFClose( output );
	TIFF* input = TIFFOpen( path.c_str(), "r" );
	std::uint64_t* offsets = nullptr;
	std::uint64_t* counts = nullptr;
	const bool indexed = input && TIFFGetField(input, TIFFTAG_STRIPOFFSETS, &offsets) &&
		TIFFGetField(input, TIFFTAG_STRIPBYTECOUNTS, &counts) && TIFFNumberOfStrips(input) == 4;
	if( !written || !indexed ) {
		Check(false, "write and locate TIFF strips");
		if( input ) TIFFClose(input);
		std::remove(path.c_str());
		return;
	}
	const auto offset = offsets[1];
	const auto count = counts[1];
	TIFFClose( input );
	std::fstream corrupt( path, std::ios::binary | std::ios::in | std::ios::out );
	corrupt.seekp( static_cast<std::streamoff>(offset) );
	for( std::uint64_t i = 0; i < count; ++i ) corrupt.put( static_cast<char>(0xff) );
	corrupt.close();
	input = TIFFOpen( path.c_str(), "r" );
	std::uint32_t pixels[16] = {};
	const bool partial = input && TIFFReadRGBAImageOriented(input, 4, 4, pixels, ORIENTATION_TOPLEFT, 0);
	if( input ) TIFFClose(input);
	Check( partial && TIFFGetR(pixels[0]) == 20 && TIFFGetR(pixels[4]) == 0 &&
		TIFFGetA(pixels[4]) == 255 && TIFFGetR(pixels[8]) == 100,
		"permissive TIFF decoding reports success with an opaque-black damaged strip" );
	unsigned int width = 0, height = 0;
	auto damaged = ReadFileBytes(path);
	Check( !RunTIFF(damaged, width, height), "RISE rejects a TIFF with a damaged compressed strip" );
	std::remove( path.c_str() );
}

static void TestTIFF()
{
	std::cout << "E: TIFF valid 2x2 decodes; 65535^2-patched TIFF safely rejected" << std::endl;

	const std::string path = TmpDir() + "rise_imgoverflow_test.tif";
	std::remove( path.c_str() );

	TIFF* tif = TIFFOpen( path.c_str(), "w" );
	if( !tif ) {
		Check( false, "E: could not create temp TIFF (libtiff)" );
		return;
	}
	TIFFSetField( tif, TIFFTAG_IMAGEWIDTH, 2 );
	TIFFSetField( tif, TIFFTAG_IMAGELENGTH, 2 );
	TIFFSetField( tif, TIFFTAG_SAMPLESPERPIXEL, 4 );
	TIFFSetField( tif, TIFFTAG_BITSPERSAMPLE, 8 );
	TIFFSetField( tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT );
	TIFFSetField( tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG );
	TIFFSetField( tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB );
	TIFFSetField( tif, TIFFTAG_ROWSPERSTRIP, 2 );			// single strip
	unsigned char row[2*4];
	for( int y = 0; y < 2; ++y ) {
		for( int x = 0; x < 2; ++x ) {
			row[x*4+0] = 200; row[x*4+1] = 100; row[x*4+2] = 50; row[x*4+3] = 255;
		}
		TIFFWriteScanline( tif, row, y, 0 );
	}
	TIFFClose( tif );

	std::vector<char> bytes = ReadFileBytes( path );
	std::remove( path.c_str() );
	Check( !bytes.empty(), "E: temp TIFF written and read back" );

	unsigned int w, h;
	RISEColor tpx0;
	const bool tOk = RunTIFF( bytes, w, h, &tpx0 );
	Check( tOk && w == 2 && h == 2, "E: valid 2x2 TIFF decodes" );
	// Assert the decoded pixel is non-empty (buffer actually populated by
	// TIFFReadRGBAImage, not left memset-zero).  We don't assert an exact
	// channel order here to stay robust to libtiff's RGBA byte packing.
	Check( tOk && (tpx0.base.r + tpx0.base.g + tpx0.base.b) > 0.0,
		"E: valid TIFF pixel (0,0) is non-empty (decode populated the buffer)" );

	auto* output = new MemoryBuffer();
	IRasterImageWriter* writer = nullptr;
	if( RISE_API_CreateTIFFWriter( &writer, *output, eColorSpace_Rec709RGB_Linear ) && writer ) {
		writer->BeginWrite( 1, 1 );
		RISEColor red;
		red.SetFromIntegerized<Rec709RGBPel, unsigned char>( {200, 100, 50, 128}, 255.0 );
		writer->WriteColor( red, 0, 0 );
		writer->EndWrite();
		writer->release();
		Check( output->DumpToFile( path.c_str() ), "E: RISE writes a TIFF via a memory buffer" );
	} else {
		Check( false, "E: RISE creates a TIFF writer" );
	}
	output->release();
	TIFF* decoded = TIFFOpen( path.c_str(), "r" );
	std::uint32_t pixel = 0;
	const bool read = decoded && TIFFReadRGBAImage( decoded, 1, 1, &pixel, 1 );
	Check( read && TIFFGetR(pixel) == 100 && TIFFGetG(pixel) == 50 &&
		TIFFGetB(pixel) == 25 && TIFFGetA(pixel) == 128,
		"E: libtiff independently decodes the RISE-written pixel" );
	if( decoded ) TIFFClose( decoded );
	RISEColor roundTrip;
	auto encoded = ReadFileBytes( path );
	const bool readBack = RunTIFF( encoded, w, h, &roundTrip );
	const auto rgba = roundTrip.Integerize<Rec709RGBPel, unsigned char>( 255.0 );
	Check( readBack && std::abs( int(rgba.r) - 200 ) <= 2 && std::abs( int(rgba.g) - 100 ) <= 2 &&
		std::abs( int(rgba.b) - 50 ) <= 2 && rgba.a == 128,
		"E: RISE reads TIFF alpha as straight color" );
	std::remove( path.c_str() );

	std::vector<char> patched = bytes;
	PatchTIFFDims( patched );
	Check( !RunTIFF( patched, w, h ), "E: 65535^2-dimension TIFF safely rejected (no OOB)" );
}
#endif

static void TestReaderFamilyReload()
{
	using Factory = std::function<IRasterImageReader*(IReadBuffer&)>;
	struct Case { const char* name; std::vector<char> bytes; Factory create; };
	const auto encoded = [](const char* format) {
		auto* output = new MemoryBuffer();
		IRasterImageWriter* writer = nullptr;
		if( std::strcmp(format, "tga") == 0 ) RISE_API_CreateTGAWriter(&writer, *output, eColorSpace_Rec709RGB_Linear);
		if( std::strcmp(format, "png") == 0 ) RISE_API_CreatePNGWriter(&writer, *output, 8, eColorSpace_Rec709RGB_Linear);
		if( std::strcmp(format, "tiff") == 0 ) RISE_API_CreateTIFFWriter(&writer, *output, eColorSpace_Rec709RGB_Linear);
		if( std::strcmp(format, "exr") == 0 ) RISE_API_CreateEXRWriter(&writer, *output, eColorSpace_Rec709RGB_Linear, eExrCompression_Piz, true, true);
		std::vector<char> result;
		if( writer ) {
			writer->BeginWrite(2, 2);
			for( unsigned int y = 0; y < 2; ++y ) for( unsigned int x = 0; x < 2; ++x )
				writer->WriteColor(RISEColor(0.25 + 0.25*x, 0.5, 0.75 - 0.25*y, 1), x, y);
			writer->EndWrite();
			result.assign(reinterpret_cast<const char*>(output->Pointer()),
				reinterpret_cast<const char*>(output->Pointer()) + output->Size());
			writer->release();
		}
		output->release();
		return result;
	};
	std::vector<Case> cases;
	cases.push_back({"TGA", encoded("tga"), [](IReadBuffer& b) { IRasterImageReader* r=nullptr; RISE_API_CreateTGAReader(&r,b,eColorSpace_Rec709RGB_Linear); return r; }});
	auto hdr = MakeHDRHeader("-Y 2 +X 2\n");
	for( int i=0; i<4; ++i ) for( unsigned char v : {128,64,32,129} ) AppendByte(hdr,v);
	cases.push_back({"HDR", hdr, [](IReadBuffer& b) { IRasterImageReader* r=nullptr; RISE_API_CreateHDRReader(&r,b); return r; }});
	std::filesystem::path jpeg;
	const std::filesystem::path relative="scenes/Tests/Painters/textures/test_pattern.jpg";
	if( const char* media=std::getenv("RISE_MEDIA_PATH") ) {
		if( std::filesystem::is_regular_file(std::filesystem::path(media)/relative) ) jpeg=std::filesystem::path(media)/relative;
	}
	for( auto root=std::filesystem::current_path(); jpeg.empty(); root=root.parent_path() ) {
		if( std::filesystem::is_regular_file(root/relative) ) jpeg=root/relative;
		if( root==root.parent_path() ) break;
	}
	cases.push_back({"JPEG", jpeg.empty()?std::vector<char>{}:ReadFileBytes(jpeg.string()), [](IReadBuffer& b) { IRasterImageReader* r=nullptr; RISE_API_CreateJPEGReader(&r,b,eColorSpace_Rec709RGB_Linear); return r; }});
#ifndef NO_PNG_SUPPORT
	cases.push_back({"PNG", encoded("png"), [](IReadBuffer& b) { IRasterImageReader* r=nullptr; RISE_API_CreatePNGReader(&r,b,eColorSpace_Rec709RGB_Linear); return r; }});
#endif
#ifndef NO_TIFF_SUPPORT
	cases.push_back({"TIFF", encoded("tiff"), [](IReadBuffer& b) { IRasterImageReader* r=nullptr; RISE_API_CreateTIFFReader(&r,b,eColorSpace_Rec709RGB_Linear); return r; }});
#endif
#ifndef NO_EXR_SUPPORT
	cases.push_back({"EXR", encoded("exr"), [](IReadBuffer& b) { IRasterImageReader* r=nullptr; RISE_API_CreateEXRReader(&r,b,eColorSpace_Rec709RGB_Linear); return r; }});
#endif
	const auto same = [](const RISEColor& a,const RISEColor& b) {
		return a.base.r==b.base.r && a.base.g==b.base.g && a.base.b==b.base.b && a.a==b.a;
	};
	const RISEColor poison(-1000,-2000,-3000,-4000);
	bool allReset = true;
	for( auto& item : cases ) {
		const auto check = [&](bool ok,const char* suffix) { Check(ok,(std::string("reader family ")+item.name+": "+suffix).c_str()); };
		check(!item.bytes.empty(), "valid fixture exists");
		if( item.bytes.empty() ) { allReset=false; continue; }
		auto* input = new MemoryBuffer(static_cast<unsigned int>(item.bytes.size()));
		input->setBytes(item.bytes.data(),static_cast<unsigned int>(item.bytes.size())); input->seek(IBuffer::START,0);
		auto* reader = item.create(*input);
		unsigned int width=0,height=0;
		bool opened = reader && reader->BeginRead(width,height);
		check(opened && width>=2 && height>=2, "initial decode has complete dimensions");
		if( !opened ) { allReset=false; if(reader)reader->release();input->release();continue; }
		const auto expectedWidth=width,expectedHeight=height;
		RISEColor expected;reader->ReadColor(expected,0,0);
		for( int i=0; i<3; ++i ) {
			input->seek(IBuffer::START,0);
			const bool reloaded=reader->BeginRead(width,height);
			RISEColor pixel=poison;if(reloaded)reader->ReadColor(pixel,0,0);
			check(reloaded && width==expectedWidth && height==expectedHeight && same(pixel,expected), "repeated decode preserves pixels and dimensions");
		}
		check(input->Resize(0,true), "empty replacement input installed");
		width=123;height=456;bool threw=false,failed=false;
		try { failed=!reader->BeginRead(width,height); } catch(const std::exception&) { threw=true; }
		RISEColor stale=poison;reader->ReadColor(stale,0,0);
		const bool reset=!threw && failed && width==0 && height==0 && same(stale,poison);
		check(reset, "failed reload clears dimensions and previous pixels without throwing");
		allReset &= reset;
		input->Resize(static_cast<unsigned int>(item.bytes.size()),true);input->seek(IBuffer::START,0);
		input->setBytes(item.bytes.data(),static_cast<unsigned int>(item.bytes.size()));input->seek(IBuffer::START,0);
		const bool recovered=reader->BeginRead(width,height);
		check(recovered && width==expectedWidth && height==expectedHeight, "healthy input recovers after failed reload");
		reader->EndRead();reader->EndRead();stale=poison;reader->ReadColor(stale,0,0);
		check(same(stale,poison), "repeated EndRead leaves no active image");
		reader->release();check(input->refcount()==1, "reader teardown releases its input");input->release();
	}
	// The unfixed readers fail the reset contract above; avoid invoking their
	// old unchecked pixel methods out of range during a normal-assertion RED run.
	if( !allReset ) return;
	for( auto& item : cases ) {
		auto* input=new MemoryBuffer(item.bytes.data(),static_cast<unsigned int>(item.bytes.size()),false);
		auto* reader=item.create(*input);unsigned int width=0,height=0;
		const bool opened=reader->BeginRead(width,height);
		RISEColor pixel=poison;
		if(opened){reader->ReadColor(pixel,width,0);reader->ReadColor(pixel,0,height);}
		Check(opened && same(pixel,poison),(std::string("reader family ")+item.name+": invalid coordinates preserve caller color").c_str());
		reader->release();input->release();
	}
}

#ifndef NO_EXR_SUPPORT
static void TestEXRLuminance()
{
	const auto same = [](const RISEColor& actual,const RISEPel& expected,float alpha) {
		const auto close=[](double a,double b){return std::fabs(a-b)<=1e-12*std::max(1.0,std::fabs(b));};
		return close(actual.base.r,expected.r) && close(actual.base.g,expected.g) &&
			close(actual.base.b,expected.b) && actual.a==alpha;
	};
	for(bool alpha : {false,true}) {
		auto* buffer=new MemoryBuffer();
		const Imath::Box2i window(Imath::V2i(-3,7),Imath::V2i(-2,8));
		const float values[]={0.5f,100000,200000,400000};
		const float opacity[]={0.25f,0.5f,0.75f,1};
		{
			OStreamWrapper stream(*buffer);Imf::Header header(2,2);header.dataWindow()=window;
			header.channels().insert("Y",Imf::Channel(Imf::FLOAT));
			if(alpha) header.channels().insert("A",Imf::Channel(Imf::FLOAT));
			Imf::OutputFile file(stream,header);Imf::FrameBuffer frame;
			frame.insert("Y",Imf::Slice::Make(Imf::FLOAT,values,window,sizeof(float),2*sizeof(float)));
			if(alpha) frame.insert("A",Imf::Slice::Make(Imf::FLOAT,opacity,window,sizeof(float),2*sizeof(float)));
			file.setFrameBuffer(frame);file.writePixels(2);
		}
		buffer->seek(IBuffer::START,0);auto* reader=new EXRReader(*buffer,eColorSpace_Rec709RGB_Linear);
		unsigned int width=0,height=0;bool exact=reader->BeginRead(width,height) && width==2 && height==2;
		if(exact) for(unsigned int i=0;i<4;++i) {
			RISEColor actual;reader->ReadColor(actual,i%2,i/2);
			exact &= same(actual,RISEPel(Rec709RGBPel(values[i],values[i],values[i])),alpha?opacity[i]:1);
		}
		Check(exact,alpha?"EXR luminance: FLOAT YA preserves range alpha and offset window":
			"EXR luminance: FLOAT Y preserves range and fills opaque alpha");
		reader->release();buffer->release();
	}
	for(auto channels : {Imf::WRITE_YC,Imf::WRITE_YCA}) {
		auto* buffer=new MemoryBuffer();
		const Imath::Box2i window(Imath::V2i(-2,4),Imath::V2i(1,7));
		std::vector<Imf::Rgba> source(16),reference(16);
		for(unsigned int i=0;i<16;++i) source[i]=Imf::Rgba(0.25f+0.125f*(i%4),0.2f+0.1f*(i/4),0.75f-0.125f*(i%4),0.5f);
		{
			OStreamWrapper stream(*buffer);Imf::Header header(4,4);header.dataWindow()=window;
			Imf::RgbaOutputFile file(stream,header,channels);
			file.setFrameBuffer(Imf::ComputeBasePointer(source.data(),window),1,4);file.writePixels(4);
		}
		buffer->seek(IBuffer::START,0);
		{
			IStreamWrapper stream(*buffer);Imf::RgbaInputFile file(stream);
			file.setFrameBuffer(Imf::ComputeBasePointer(reference.data(),window),1,4);file.readPixels(4,7);
		}
		buffer->seek(IBuffer::START,0);auto* reader=new EXRReader(*buffer,eColorSpace_Rec709RGB_Linear);
		unsigned int width=0,height=0;bool exact=reader->BeginRead(width,height) && width==4 && height==4;
		if(exact) for(unsigned int i=0;i<16;++i) {
			RISEColor actual;reader->ReadColor(actual,i%4,i/4);const auto& p=reference[i];
			exact &= same(actual,RISEPel(Rec709RGBPel(float(p.r),float(p.g),float(p.b))),float(p.a));
		}
		Check(exact,channels==Imf::WRITE_YC?"EXR luminance: subsampled YC matches independent SDK conversion":
			"EXR luminance: subsampled YCA matches independent SDK conversion and alpha");
		reader->release();buffer->release();
	}
}

static void TestEXROffsetWindow()
{
	auto* buffer=new MemoryBuffer();
	const Imath::Box2i window(Imath::V2i(-3,7), Imath::V2i(-2,8));
	const float red[] = {100000,200000,300000,400000};
	{
		OStreamWrapper output(*buffer);
		Imf::Header header(2,2);header.dataWindow()=window;
		header.channels().insert("R",Imf::Channel(Imf::FLOAT));
		Imf::OutputFile file(output,header);
		Imf::FrameBuffer frame;
		frame.insert("R",Imf::Slice::Make(Imf::FLOAT,red,window,sizeof(float),2*sizeof(float)));
		file.setFrameBuffer(frame);file.writePixels(2);
	}
	buffer->seek(IBuffer::START,0);
	auto* reader=new EXRReader(*buffer,eColorSpace_Rec709RGB_Linear);
	unsigned int width=0,height=0;
	bool exact=reader->BeginRead(width,height) && width==2 && height==2;
	if(exact) for(unsigned int i=0;i<4;++i) {
		RISEColor actual;reader->ReadColor(actual,i%2,i/2);
		const RISEPel expected(Rec709RGBPel(red[i],0,0));
		const auto close=[](double a,double b){return std::fabs(a-b)<=1e-12*std::max(1.0,std::fabs(b));};
		exact &= close(actual.base.r,expected.r) && close(actual.base.g,expected.g) &&
			close(actual.base.b,expected.b) && actual.a==1;
	}
	Check(exact,"reader family EXR: offset data window preserves FLOAT range and fills missing channels");
	reader->release();buffer->release();
}
#endif

static int RunTests()
{
	TestReaderFamilyReload();
#ifndef NO_EXR_SUPPORT
	TestEXROffsetWindow();
	TestEXRLuminance();
#endif
	TestRasterStorage();
	TestHDRWriterBounds();
	TestTGAWriterBounds();
	TestJPEGReload();
	std::cout << "ImageReaderOverflowTest — header-driven write-side overflows are rejected"
		<< std::endl;

	TestTGA();
	TestTGAFormat();
	TestHDRNewRLE();
	TestHDROldRLE();
#ifndef NO_PNG_SUPPORT
	TestPNG();
#endif
#ifndef NO_TIFF_SUPPORT
	TestTIFF();
	TestDamagedTIFFStrip();
#endif

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}

int main()
{
	return RISE::TestSupport::WithOwnedScratch(RunTests);
}
