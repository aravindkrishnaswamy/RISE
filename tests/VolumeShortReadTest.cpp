//////////////////////////////////////////////////////////////////////
//
//  VolumeShortReadTest.cpp - Regression test for the unchecked-`fread`
//    binary loaders (Volume slice loader + MemoryBuffer file ctor).
//
//  Both historically called `fread(...)` and ignored the return count,
//  so a MISSING or TRUNCATED binary file left the destination buffer
//  partly (or wholly) UNINITIALISED — garbage from `new[]` — rather than
//  failing or zero-filling.  For the raw-volume loader a missing slice
//  file (fopen fails) had no else branch at all, leaving that slice's
//  voxels uninitialised.
//
//  The loaders now value-initialise their buffers to zero and check the
//  `fread` count: a missing / short slice leaves ZEROS (and logs an
//  error) instead of exposing uninitialised memory.
//
//  This test drives Volume<unsigned char> with a present slice, a
//  TRUNCATED slice, and a MISSING slice, and asserts the unread regions
//  read back as exactly zero.  It also drives MemoryBuffer with a
//  nonexistent file and asserts the stat-failure guard leaves an empty
//  buffer rather than proceeding with a bogus size.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <limits>

#include "../src/Library/Utilities/Math3D/Math3D.h"   // Scalar (double)
#include "../src/Library/Volume/Volume.h"
#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/Utilities/DiskFileReadBuffer.h"
#include "../src/Library/Utilities/DiskFileWriteBuffer.h"
#include "../src/Library/Utilities/MediaPathLocator.h"
#include "../src/Library/RISE_API.h"
#include "TestTempDir.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const char* name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

static std::string TmpDir()
{
	const char* tmp = getenv( "TMPDIR" );
	std::string dir = tmp ? std::string( tmp ) : ( RISE::TestSupport::ScratchBaseDir() + "/" );
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	return dir;
}

static void WriteBytes( const std::string& path, unsigned char value, int count )
{
	std::ofstream f( path.c_str(), std::ios::binary | std::ios::trunc );
	std::vector<char> bytes( count, static_cast<char>( value ) );
	f.write( bytes.data(), count );
	f.close();
}

// Dirty the heap with 0xFF so that a subsequent `new[]` which the
// allocator recycles from this region reads back non-zero if it were left
// uninitialised.  This makes the "unread region == 0" assertions a
// RELIABLE discriminator against the pre-fix code (fresh zero-pages could
// otherwise let a buggy uninitialised read spuriously pass).  Best-effort:
// allocations of the same size class are the most likely to be recycled.
static void DirtyHeap( size_t nbytes )
{
	std::vector<void*> blocks;
	for( int i = 0; i < 64; ++i ) {
		void* p = std::malloc( nbytes );
		if( p ) { std::memset( p, 0xFF, nbytes ); blocks.push_back( p ); }
	}
	for( void* p : blocks ) std::free( p );
}

// GetValue returns byte/255 for T=unsigned char; recover the raw byte.
static double ByteAt( const Volume<unsigned char>* v, int x, int y, int z )
{
	return v->GetValue( x, y, z ) * 255.0;
}

//----------------------------------------------------------------------
// Volume: present / truncated / missing slices.
//   w=h=4, slices z=0,1,2 (centered coords: z=-1 -> slice0, 0 -> slice1,
//   +1 -> slice2; x,y in [-32,31]).
//   slice0: full 4096 bytes of 200 (present)
//   slice1: only 2048 of 4096 bytes of 100 (TRUNCATED -> tail rows must be 0)
//   slice2: file absent (MISSING -> whole slice must be 0)
//
// Dimensions are 64x64 DELIBERATELY: a small (<=~256 B) buffer lands in
// macOS's `nano` malloc zone, which ALWAYS hands back zeroed memory, so a
// tiny volume's "unread == 0" assertions would pass even against the
// pre-fix (uninitialised-read) code — a green-on-both non-test.  At 64x64
// (12 KB) the buffer escapes the nano zone into the recycling scalable
// zone, so the DirtyHeap poisoning below makes an uninitialised read come
// back as 0xFF garbage — i.e. these assertions genuinely FAIL against the
// buggy code and PASS only with the value-init fix.
//----------------------------------------------------------------------
static void TestVolumeSlices()
{
	std::cout << "A: Volume present / truncated / missing slices read back zero (not garbage)" << std::endl;

	static const int W = 64, H = 64;
	static const int SLICE = W * H;           // 4096 bytes/slice
	static const int TOTAL = SLICE * 3;       // 12288 bytes (escapes nano zone)

	const std::string base = TmpDir() + "rise_voltest_slice_";
	const std::string s0 = base + "0.raw";
	const std::string s1 = base + "1.raw";
	const std::string s2 = base + "2.raw";
	std::remove( s2.c_str() );                // ensure slice 2 is absent
	WriteBytes( s0, 200, SLICE );             // full slice
	WriteBytes( s1, 100, SLICE / 2 );         // truncated: first half of the slice

	// Poison the heap (same size class) so an uninitialised buffer would
	// read back 0xFF, making the "unread == 0" assertions a reliable
	// regression discriminator rather than a green-on-both no-test.
	DirtyHeap( TOTAL );

	const std::string pattern = base + "%d.raw";
	Volume<unsigned char>* v = new Volume<unsigned char>( pattern.c_str(), W, H, 0, 2 );
	v->addref();

	// slice0 (z=-1): fully present -> 200 everywhere.
	Check( std::fabs( ByteAt( v, -32, -32, -1 ) - 200.0 ) < 1.5, "A: slice0 corner == 200 (present)" );
	Check( std::fabs( ByteAt( v,  31,  31, -1 ) - 200.0 ) < 1.5, "A: slice0 far corner == 200 (present)" );

	// slice1 (z=0): first SLICE/2 bytes = rows y in [-32,-1] (== 100); the
	// truncated tail rows y in [0,31] must be zeroed (index >= SLICE/2).
	Check( std::fabs( ByteAt( v, -32, -32, 0 ) - 100.0 ) < 1.5, "A: slice1 read region == 100" );
	Check( std::fabs( ByteAt( v,   0,  -1, 0 ) - 100.0 ) < 1.5, "A: slice1 last read row == 100" );
	Check( std::fabs( ByteAt( v,   0,   0, 0 ) - 0.0   ) < 1.5, "A: slice1 truncated tail == 0 (not garbage)" );
	Check( std::fabs( ByteAt( v,  31,  31, 0 ) - 0.0   ) < 1.5, "A: slice1 far truncated tail == 0" );

	// slice2 (z=+1): file missing -> entirely zero.
	Check( std::fabs( ByteAt( v, -32, -32, 1 ) - 0.0 ) < 1.5, "A: missing slice2 corner == 0 (not garbage)" );
	Check( std::fabs( ByteAt( v,  31,  31, 1 ) - 0.0 ) < 1.5, "A: missing slice2 far corner == 0" );

	v->release();
	std::remove( s0.c_str() );
	std::remove( s1.c_str() );
}

//----------------------------------------------------------------------
// MemoryBuffer: nonexistent file -> stat-failure guard leaves it empty.
//----------------------------------------------------------------------
static void TestMemoryBufferMissingFile()
{
	std::cout << "B: MemoryBuffer on a nonexistent file leaves an empty buffer (stat guard)" << std::endl;

	const std::string missing = TmpDir() + "rise_membuf_definitely_absent_xyz.bin";
	std::remove( missing.c_str() );

	// MemoryBuffer is Reference-counted (heap + release).
	MemoryBuffer* mb = new MemoryBuffer( missing.c_str() );
	mb->addref();
	Check( mb->Size() == 0, "B: missing-file MemoryBuffer has Size()==0 (no bogus alloc/read)" );
	mb->release();
}

//----------------------------------------------------------------------
// C: MemoryBuffer::getBytes over-read — a caller asking for more bytes
// than remain (a malformed/truncated file whose header lies about a chunk
// size) must NOT read out-of-bounds past the heap buffer.  The guard
// (formerly _DEBUG-only) returns the available bytes, zero-fills the rest,
// and signals failure.
//----------------------------------------------------------------------
static void TestMemoryBufferGetBytesOverread()
{
	std::cout << "C: MemoryBuffer::getBytes over-read is bounded (no OOB heap read)" << std::endl;

	const std::string path = TmpDir() + "rise_membuf_small.bin";
	WriteBytes( path, 0xAB, 8 );        // an 8-byte file

	MemoryBuffer* mb = new MemoryBuffer( path.c_str() );
	mb->addref();
	Check( mb->Size() == 8, "C: 8-byte file loaded" );

	// Ask for 32 bytes from an 8-byte buffer.
	unsigned char dest[32];
	std::memset( dest, 0x11, sizeof( dest ) );   // pre-dirty the destination
	const bool ok = mb->getBytes( dest, 32 );

	Check( !ok, "C: over-read returns false" );
	bool firstEight = true;
	for( int i = 0; i < 8; ++i ) if( dest[i] != 0xAB ) firstEight = false;
	Check( firstEight, "C: available 8 bytes copied (0xAB)" );
	bool tailZeroed = true;
	for( int i = 8; i < 32; ++i ) if( dest[i] != 0 ) tailZeroed = false;
	Check( tailZeroed, "C: over-read tail zero-filled (not OOB garbage / not left 0x11)" );

	mb->release();
	std::remove( path.c_str() );
}

//----------------------------------------------------------------------
// D: MemoryBuffer scalar getters (getUInt/getDouble) past EOF return 0
// instead of OOB-reading the heap.  A malformed/truncated file whose
// header drives a count-then-scalar-loop (e.g. .risemesh numpts ->
// getDouble) would otherwise walk off the buffer.  The guard (formerly
// _DEBUG-only) returns 0 and consumes to end so a loop-until-cursor
// caller terminates.
//----------------------------------------------------------------------
static void TestMemoryBufferScalarOverread()
{
	std::cout << "D: MemoryBuffer scalar getters past EOF return 0 (no OOB heap read)" << std::endl;

	// An 8-byte file = exactly two uint32.
	const std::string path = TmpDir() + "rise_membuf_scalar.bin";
	{
		std::ofstream f( path.c_str(), std::ios::binary | std::ios::trunc );
		unsigned int a = 0x11223344u, b = 0x55667788u;
		f.write( reinterpret_cast<const char*>( &a ), 4 );
		f.write( reinterpret_cast<const char*>( &b ), 4 );
		f.close();
	}

	MemoryBuffer* mb = new MemoryBuffer( path.c_str() );
	mb->addref();
	Check( mb->Size() == 8, "D: 8-byte scalar file loaded" );

	const unsigned int v0 = mb->getUInt();   // bytes 0..3
	const unsigned int v1 = mb->getUInt();   // bytes 4..7 (now at EOF)
	Check( v0 == 0x11223344u && v1 == 0x55667788u, "D: two in-range getUInt read correctly" );

	// Now at EOF: further scalar reads must return 0, not OOB.
	const unsigned int over = mb->getUInt();
	Check( over == 0, "D: getUInt past EOF returns 0 (guarded, not OOB)" );

	mb->release();

	// A 4-byte buffer + getDouble (needs 8) -> over-read -> 0.
	const std::string path2 = TmpDir() + "rise_membuf_scalar2.bin";
	WriteBytes( path2, 0xCD, 4 );
	MemoryBuffer* mb2 = new MemoryBuffer( path2.c_str() );
	mb2->addref();
	const double d = mb2->getDouble();       // only 4 of 8 bytes available
	Check( d == 0.0, "D: getDouble with < 8 bytes returns 0 (guarded, not partial/OOB)" );
	mb2->release();

	std::remove( path.c_str() );
	std::remove( path2.c_str() );
}

//----------------------------------------------------------------------
// E: DiskFileReadBuffer::getBytes over-read (the FILE-backed reader).
// Mirrors case C; also asserts the cursor lands at EOF afterward, locking
// the ftell(hFile)-backed cursor-consistency invariant.
//----------------------------------------------------------------------
static void TestDiskFileReadBufferGetBytesOverread()
{
	std::cout << "E: DiskFileReadBuffer::getBytes over-read is bounded + cursor at EOF" << std::endl;

	const std::string path = TmpDir() + "rise_diskbuf_small.bin";
	WriteBytes( path, 0xBE, 8 );

	DiskFileReadBuffer* db = new DiskFileReadBuffer( path.c_str() );
	db->addref();

	unsigned char dest[32];
	std::memset( dest, 0x22, sizeof( dest ) );
	const bool ok = db->getBytes( dest, 32 );

	Check( !ok, "E: over-read returns false" );
	bool firstEight = true;
	for( int i = 0; i < 8; ++i ) if( dest[i] != 0xBE ) firstEight = false;
	Check( firstEight, "E: available 8 bytes read (0xBE)" );
	bool tailZeroed = true;
	for( int i = 8; i < 32; ++i ) if( dest[i] != 0 ) tailZeroed = false;
	Check( tailZeroed, "E: over-read tail zero-filled (not truncated garbage / not 0x22)" );
	Check( db->getCurPos() == 8, "E: cursor left at EOF (8) after over-read" );

	db->release();
	std::remove( path.c_str() );
}

//----------------------------------------------------------------------
// F: DiskFileReadBuffer on a NONEXISTENT file (hFile == NULL) must not
// crash — the scalar getters previously went straight to ftell(NULL) /
// fread(NULL) (UB / SIGSEGV) reachable from the .3ds loader on a missing
// asset.  The null-hFile entry guard degrades to 0 / false.
//----------------------------------------------------------------------
static void TestDiskFileReadBufferMissingFile()
{
	std::cout << "F: DiskFileReadBuffer on a missing file degrades gracefully (no ftell(NULL) crash)" << std::endl;

	const std::string missing = TmpDir() + "rise_diskbuf_definitely_absent_xyz.bin";
	std::remove( missing.c_str() );

	DiskFileReadBuffer* db = new DiskFileReadBuffer( missing.c_str() );
	db->addref();

	// Each of these would ftell(NULL)/fread(NULL) crash pre-guard.
	Check( db->getUInt() == 0,   "F: getUInt on null-hFile returns 0 (no crash)" );
	Check( db->getDouble() == 0.0, "F: getDouble on null-hFile returns 0 (no crash)" );
	unsigned char dest[8];
	std::memset( dest, 0x33, sizeof( dest ) );
	Check( !db->getBytes( dest, 8 ), "F: getBytes on null-hFile returns false (no crash)" );

	// Base DiskBuffer methods (seek-first readers like TIFF call these
	// before any getter): must also be null-safe (was fseek(NULL)/ftell(NULL)).
	Check( db->getCurPos() == 0, "F: getCurPos on null-hFile returns 0 (no crash)" );
	Check( !db->seek( IBuffer::START, 0 ), "F: seek on null-hFile returns false (no crash)" );

	db->release();
}

//----------------------------------------------------------------------
// G: Volume with header/scene-driven overflow dimensions.  The voxel buffer
// is sized from `depth*height*width` (a 32-bit product) — 100000*100000*1
// wraps 32-bit and, unwrapped, exceeds the 2 GiB ceiling.  The ctor must
// degrade to an EMPTY volume (m_pData null, dims 0, all GetValue -> 0)
// rather than allocating a wrapped-small buffer and writing slice data past
// it.  This is the WRITE-side (destination-sizing) twin of the read-side
// short-read hardening above.
//----------------------------------------------------------------------
static void TestVolumeOverflowDims()
{
	std::cout << "G: Volume with overflow dimensions degrades to an empty volume (no OOB alloc)" << std::endl;

	const std::string pattern = TmpDir() + "rise_vol_overflow_%d.raw";
	Volume<unsigned char>* v = new Volume<unsigned char>( pattern.c_str(), 100000, 100000, 0, 0 );
	v->addref();
	Check( v->Width() == 0 && v->Height() == 0 && v->Depth() == 0,
		"G: overflow volume reports zero dimensions (rejected before alloc)" );
	Check( ByteAt( v, 0, 0, 0 ) == 0.0, "G: overflow volume GetValue returns 0 (no crash / no OOB)" );
	v->release();

	// A dimension >= 2^31: the int members would sign-extend to a huge (ull)
	// and wrap the product SMALL if the guard read the members instead of the
	// raw unsigned params.  Must still be rejected (empty volume).
	Volume<unsigned char>* v2 = new Volume<unsigned char>( pattern.c_str(), 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0 );
	v2->addref();
	Check( v2->Width() == 0 && v2->Height() == 0 && v2->Depth() == 0,
		"G: >=2^31 dimension rejected (no sign-extension bypass)" );
	Check( ByteAt( v2, 0, 0, 0 ) == 0.0, "G: >=2^31 volume GetValue returns 0 (no OOB)" );
	v2->release();

	// Reversed Z range (zend < zstart) would underflow the unsigned depth to a
	// huge value; must be rejected too.
	Volume<unsigned char>* v3 = new Volume<unsigned char>( pattern.c_str(), 4, 4, 5, 2 );
	v3->addref();
	Check( v3->Width() == 0 && v3->Depth() == 0, "G: reversed z-range (zend<zstart) rejected" );
	v3->release();
}

static void TestBufferWriteFailures()
{
	IMemoryBuffer* invalidMemory = nullptr;
	Check( !RISE_API_CreateCompatibleMemoryBuffer( &invalidMemory, nullptr, 2, false ) && !invalidMemory,
		"H: wrapping a null pointer with positive capacity is refused" );
	auto* empty = new MemoryBuffer( nullptr, 2, false );
	Check( empty->Size() == 0 && empty->EndOfBuffer() && !empty->PointerAtCursor() && !empty->setChar( 'X' ),
		"H: direct null borrowed storage behaves as an empty buffer" );
	empty->release();
	char storage[] = { 'a', 'b', 'c', 'd' };
	auto* borrowed = new MemoryBuffer( storage, 2, false );
	borrowed->seek( IBuffer::START, 1 );
	Check( !borrowed->seek( IBuffer::START, -1 ) && !borrowed->seek( IBuffer::END, -1 ) &&
		borrowed->getCurPos() == 1 && borrowed->seek( IBuffer::CUR, -1 ) &&
		borrowed->getCurPos() == 0 && borrowed->seek( IBuffer::CUR, 1 ),
		"H: invalid signed seeks preserve the cursor and backward relative seeks work" );
	Check( !borrowed->setBytes( "XY", 2 ) && borrowed->getCurPos() == 1 &&
		std::memcmp( storage, "abcd", sizeof(storage) ) == 0,
		"H: rejected borrowed write preserves cursor and both canary bytes" );
	Check( !borrowed->setDouble( 1 ) && borrowed->getCurPos() == 1 &&
		std::memcmp( storage, "abcd", sizeof(storage) ) == 0,
		"H: rejected borrowed scalar write preserves cursor and storage" );
	borrowed->release();

	auto* owned = new MemoryBuffer( 2u );
	Check( owned->setBytes( "XY", 2 ) && owned->setBytes( owned->Pointer(), 2 ) &&
		owned->getCurPos() == 4 && std::memcmp( owned->Pointer(), "XYXY", 4 ) == 0,
		"H: growing self-copy reads from the original storage" );
	Check( !owned->ResizeForMore( std::numeric_limits<unsigned int>::max() ) &&
		owned->Size() == 4 && owned->getCurPos() == 4,
		"H: capacity request rejects cursor-plus-length overflow" );
	owned->release();

	auto* growing = new MemoryBuffer();
	bool validGrowth = true;
	unsigned int relocations = 0;
	for( unsigned int i = 0; i < 64; ++i ) {
		const auto before = reinterpret_cast<std::uintptr_t>(growing->Pointer());
		validGrowth &= growing->setUInt( i ) && growing->Size() == (i+1)*4 &&
			growing->getCurPos() == growing->Size();
		if( before != reinterpret_cast<std::uintptr_t>(growing->Pointer()) ) ++relocations;
	}
	Check( relocations < 32, "H: sequential appends reuse backing storage" );
	auto* clone = new MemoryBuffer( *growing );
	validGrowth &= clone->Pointer() != growing->Pointer() && clone->seek( IBuffer::START, 0 );
	for( unsigned int i = 0; i < 64; ++i ) validGrowth &= clone->getUInt() == i;
	Check( validGrowth && growing->Resize( 8, true ) && growing->Size() == 8 &&
		growing->getCurPos() == 0 && growing->getUInt() == 0 && growing->getUInt() == 1,
		"H: growing buffers preserve logical size, deep copies and forced shrink" );
	clone->release();
	growing->release();

	auto* adopted = new MemoryBuffer( new char[2]{ 'A', 'B' }, 2, true );
	auto* adoptedCopy = new MemoryBuffer( static_cast<IMemoryBuffer&>(*adopted) );
	Check( adopted->seek( IBuffer::START, 2 ) && adopted->setBytes( adopted->Pointer(), 2 ) &&
		adoptedCopy->DoOwnMemory() && adoptedCopy->Size() == 2 &&
		std::memcmp( adopted->Pointer(), "ABAB", 4 ) == 0 &&
		std::memcmp( adoptedCopy->Pointer(), "AB", 2 ) == 0,
		"H: adopted storage grows safely and interface copy owns independent bytes" );
	adoptedCopy->release();
	adopted->release();

	auto* invalid = new DiskFileWriteBuffer( nullptr );
	Check( !invalid->ReadyToWrite() && !invalid->setChar( 'x' ) && !invalid->setDouble( 1 ) &&
		!invalid->setBytes( "XY", 2 ) && !invalid->Finish(),
		"H: disk writes and close report an unavailable file" );
	invalid->release();

	const auto path = RISE::TestSupport::ScratchPath(
		"rise_buffer_finish_" + std::to_string( RISE::TestSupport::CurrentProcessId() ) );
	auto* file = new DiskFileWriteBuffer( path.c_str() );
	Check( file->setBytes( "XY", 2 ) && file->Finish() && !file->ReadyToWrite(),
		"H: finishing a successful write closes the handle" );
	file->release();
	std::ifstream readback( path, std::ios::binary );
	char actual[2];
	Check( readback.read( actual, sizeof(actual) ) && std::memcmp( actual, "XY", sizeof(actual) ) == 0,
		"H: finished file contains both bytes" );
	readback.close();
	std::remove( path.c_str() );
}

static void TestLittleEndianScalars()
{
	const unsigned char expected[] = {
		0xfe, 0xff, 0xef, 0xbe,
		0xfe, 0xff, 0xff, 0xff, 0xef, 0xcd, 0xab, 0xf9,
		0x00, 0x00, 0xa0, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0xc0
	};
	const auto write = []( IWriteBuffer& buffer ) {
		return buffer.setWord( -2 ) && buffer.setUWord( 0xbeef ) &&
			buffer.setInt( -2 ) && buffer.setUInt( 0xf9abcdef ) &&
			buffer.setFloat( 1.25f ) && buffer.setDouble( -2.5 );
	};
	const auto read = []( IReadBuffer& buffer ) {
		return buffer.getWord() == -2 && buffer.getUWord() == 0xbeef &&
			buffer.getInt() == -2 && buffer.getUInt() == 0xf9abcdef &&
			buffer.getFloat() == 1.25f && buffer.getDouble() == -2.5;
	};
	auto* memory = new MemoryBuffer();
	Check( write( *memory ) && memory->getCurPos() == sizeof(expected) &&
		std::memcmp( memory->Pointer(), expected, sizeof(expected) ) == 0,
		"memory scalar writes match independent little-endian bytes" );
	memory->release();
	char fixture[sizeof(expected)];
	std::memcpy( fixture, expected, sizeof(fixture) );
	auto* borrowed = new MemoryBuffer( fixture, sizeof(fixture), false );
	Check( read( *borrowed ), "memory scalar reads decode independent little-endian bytes" );
	borrowed->release();

	const auto path = RISE::TestSupport::ScratchPath( "scalar_little_endian.bin" );
	auto* diskWrite = new DiskFileWriteBuffer( path.c_str() );
	const bool written = write( *diskWrite );
	Check( diskWrite->Finish( true ) && written, "disk scalar writes complete" );
	diskWrite->release();
	std::ifstream file( path, std::ios::binary );
	unsigned char actual[sizeof(expected)];
	Check( file.read( reinterpret_cast<char*>(actual), sizeof(actual) ) &&
		std::memcmp( actual, expected, sizeof(expected) ) == 0,
		"disk scalar writes match independent little-endian bytes" );
	file.close();
	std::ofstream fixtureFile( path, std::ios::binary );
	fixtureFile.write( fixture, sizeof(fixture) );
	fixtureFile.close();
	auto* diskRead = new DiskFileReadBuffer( path.c_str() );
	Check( read( *diskRead ), "disk scalar reads decode independent little-endian bytes" );
	diskRead->release();
	std::remove( path.c_str() );
}

static void TestMediaSearchRoots()
{
	namespace fs = std::filesystem;
	const auto root = fs::path( RISE::TestSupport::ScratchPath( "media_lookup" ) );
	const auto first = root / "first";
	const auto second = root / "second";
	const std::string name = u8"rise_media_é.bin";
	fs::create_directories( first );
	fs::create_directories( second );
	std::ofstream( first / fs::u8path(name) ) << '1';
	std::ofstream( second / fs::u8path(name) ) << '2';
	MediaPathLocator locator;
	Check( !locator.AddPath( "" ) && locator.AddPath( first.u8string().c_str() ) &&
		!locator.AddPath( (first.u8string() + "/").c_str() ) && locator.AddPath( second.u8string().c_str() ),
		"media roots reject empty input and equivalent duplicate spellings" );
	Check( locator.Find( name.c_str() ) == (first.u8string() + "/" + name).c_str(),
		"media roots search in insertion order, including UTF-8 names" );
	const std::string direct = (second / fs::u8path(name)).u8string();
	Check( locator.Find( direct.c_str() ) == direct.c_str() &&
		locator.RemovePath( (first.u8string() + "/").c_str() ) &&
		locator.Find( name.c_str() ) == (second.u8string() + "/" + name).c_str(),
		"media lookup prefers a direct path and removal accepts the normalized spelling" );
	locator.ClearAllPaths();
	Check( locator.Find( name.c_str() ) == name.c_str(), "a media miss preserves original spelling" );
	fs::remove_all( root );
}

int main( int /*argc*/, char* /*argv*/[] )
{
	std::cout << "VolumeShortReadTest — missing/truncated binary reads zero, never uninitialised"
		<< std::endl;

	TestVolumeSlices();
	TestMemoryBufferMissingFile();
	TestMemoryBufferGetBytesOverread();
	TestMemoryBufferScalarOverread();
	TestDiskFileReadBufferGetBytesOverread();
	TestDiskFileReadBufferMissingFile();
	TestVolumeOverflowDims();
	TestBufferWriteFailures();
	TestLittleEndianScalars();
	TestMediaSearchRoots();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
