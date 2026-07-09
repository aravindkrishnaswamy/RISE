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

#include <zlib.h>						// crc32 for the crafted PNG chunks

#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/RasterImages/TGAReader.h"
#include "../src/Library/RasterImages/HDRReader.h"
#include "../src/Library/RasterImages/PNGReader.h"
#include "../src/Library/RasterImages/TIFFReader.h"

#ifndef NO_TIFF_SUPPORT
	#include <tiffio.h>
#endif

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const char* name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
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
	std::string dir = tmp ? tmp : "/tmp/";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	return dir;
}

static std::vector<char> ReadFileBytes( const std::string& path )
{
	std::ifstream f( path.c_str(), std::ios::binary );
	return std::vector<char>( (std::istreambuf_iterator<char>( f )), std::istreambuf_iterator<char>() );
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
	mb->addref();
	TGAReader* reader = new TGAReader( *mb, eColorSpace_Rec709RGB_Linear );
	reader->addref();
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
	mb->addref();
	HDRReader* reader = new HDRReader( *mb );
	reader->addref();
	const bool ok = reader->BeginRead( w, h );
	if( ok && px0 && w >= 1 )        reader->ReadColor( *px0,   0,   0 );
	if( ok && pxLast && w >= 1 )     reader->ReadColor( *pxLast, w-1, 0 );
	reader->release();
	mb->release();
	return ok;
}

#ifndef NO_PNG_SUPPORT
static bool RunPNG( std::vector<char>& bytes, unsigned int& w, unsigned int& h )
{
	w = h = 0;
	MemoryBuffer* mb = new MemoryBuffer( bytes.data(), static_cast<unsigned int>( bytes.size() ), false );
	mb->addref();
	PNGReader* reader = new PNGReader( *mb, eColorSpace_Rec709RGB_Linear );
	reader->addref();
	const bool ok = reader->BeginRead( w, h );
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
	MemoryBuffer* mb = new MemoryBuffer( bytes.data(), static_cast<unsigned int>( bytes.size() ), false );
	mb->addref();
	TIFFReader* reader = new TIFFReader( *mb, eColorSpace_Rec709RGB_Linear );
	reader->addref();
	const bool ok = reader->BeginRead( w, h );
	if( ok && px0 && w >= 1 && h >= 1 )           reader->ReadColor( *px0, 0, 0 );
	reader->release();
	mb->release();
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
	AppendByte( v, (unsigned char)( w & 0xFF ) );
	AppendByte( v, (unsigned char)( (w >> 8) & 0xFF ) );
	AppendByte( v, (unsigned char)( h & 0xFF ) );
	AppendByte( v, (unsigned char)( (h >> 8) & 0xFF ) );
	AppendByte( v, bpp );
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

	// Valid 2x2: TGA stores each pixel as [A,B,G,R] (ReadColor's mapping) and
	// reads scanlines backwards-y, so both rows are populated.  All four
	// pixels are set to A=255,B=10,G=100,R=200 -> decoded R>G>B, which both
	// verifies the channel de-interleave and exercises the (size_t)y*width*4
	// row offset (pixel (1,1) lives in the other scanline).
	std::vector<char> good = MakeTGAHeader( 2, 2, 32 );
	for( int p = 0; p < 4; ++p ) { AppendByte( good, 255 ); AppendByte( good, 10 ); AppendByte( good, 100 ); AppendByte( good, 200 ); }
	RISEColor tga0, tgaCorner;
	const bool tgaOk = RunTGA( good, w, h, &tga0, &tgaCorner );
	Check( tgaOk && w == 2 && h == 2, "A: valid 2x2 TGA decodes" );
	Check( tgaOk && tga0.base.r > tga0.base.g && tga0.base.g > tga0.base.b && tga0.base.b > 0.0,
		"A: TGA pixel (0,0) decodes R>G>B (channel order)" );
	Check( tgaOk && tgaCorner.base.r > tgaCorner.base.g && tgaCorner.base.g > tgaCorner.base.b,
		"A: TGA pixel (1,1) decodes R>G>B (backwards-y row offset)" );
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

	// Over-long RLE run (num=200 -> length 72) into a 4-wide scanline.
	std::vector<char> run = MakeHDRHeader( "-Y 2 +X 4\n" );
	AppendByte( run, 2 ); AppendByte( run, 2 ); AppendByte( run, 0 ); AppendByte( run, 4 );
	AppendByte( run, 200 ); AppendByte( run, 0xAB );
	Check( !RunHDR( run, w, h ), "B: over-long-RLE HDR returns false" );

	// Truncated scanline (col header, no run codes) -> zero-length run -> reject.
	std::vector<char> trunc = MakeHDRHeader( "-Y 2 +X 4\n" );
	AppendByte( trunc, 2 ); AppendByte( trunc, 2 ); AppendByte( trunc, 0 ); AppendByte( trunc, 4 );
	Check( !RunHDR( trunc, w, h ), "B: truncated-scanline HDR returns false" );

	// Valid 4x1 new-RLE: four raw runs of 4 values; R<G<B so we can assert
	// the decode order wasn't transposed.
	std::vector<char> good = MakeHDRHeader( "-Y 1 +X 4\n" );
	AppendByte( good, 2 ); AppendByte( good, 2 ); AppendByte( good, 0 ); AppendByte( good, 4 );
	const unsigned char comp[4][4] = {
		{ 10, 20, 30, 40 }, { 11, 21, 31, 41 }, { 12, 22, 32, 42 }, { 130, 130, 130, 130 }
	};
	for( int c = 0; c < 4; ++c ) {
		AppendByte( good, 4 );
		for( int p = 0; p < 4; ++p ) AppendByte( good, comp[c][p] );
	}
	RISEColor px0;
	const bool ok = RunHDR( good, w, h, &px0 );
	Check( ok && w == 4 && h == 1, "B: valid 4x1 new-RLE HDR decodes" );
	Check( ok && px0.a == 1.0, "B: decoded pixel alpha == 1.0" );
	Check( ok && px0.base.r > 0.0 && px0.base.r < px0.base.g && px0.base.g < px0.base.b,
		"B: pixel channels decode in R<G<B order (no transposition)" );
}

static void TestHDROldRLE()
{
	std::cout << "C: HDR old-RLE valid raw / valid run / crafted run-at-pixel-0" << std::endl;
	unsigned int w, h;

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

	// Crafted old-RLE run at pixel 0: (1,1,1,5) at scanline start references
	// pos-step == -1 -> the back-reference bounds guard must reject.
	std::vector<char> badrun = MakeHDRHeader( "-Y 1 +X 4\n" );
	AppendByte( badrun, 1 ); AppendByte( badrun, 1 ); AppendByte( badrun, 1 ); AppendByte( badrun, 5 );
	Check( !RunHDR( badrun, w, h ), "C: old-RLE run at pixel 0 (back-ref underflow) returns false" );
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

	std::vector<char> patched = bytes;
	PatchTIFFDims( patched );
	Check( !RunTIFF( patched, w, h ), "E: 65535^2-dimension TIFF safely rejected (no OOB)" );
}
#endif

int main( int /*argc*/, char* /*argv*/[] )
{
	std::cout << "ImageReaderOverflowTest — header-driven write-side overflows are rejected"
		<< std::endl;

	TestTGA();
	TestHDRNewRLE();
	TestHDROldRLE();
#ifndef NO_PNG_SUPPORT
	TestPNG();
#endif
#ifndef NO_TIFF_SUPPORT
	TestTIFF();
#endif

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
