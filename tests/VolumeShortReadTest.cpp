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
#include "../src/Library/Geometry/TriangleMeshLoader3DS.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "TestTempDir.h"
#include "../src/Library/Interfaces/ILogPriv.h"
#include <iterator>
#include <stdexcept>
#include <functional>
#ifndef NO_TIFF_SUPPORT
#include <tiffio.h>
#include "../src/Library/RasterImages/TIFFCommon.h"
#endif
#ifndef NO_EXR_SUPPORT
#include "../src/Library/RasterImages/EXRReader.h"
#include "../src/Library/RasterImages/EXRWriter.h"
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

	// Each of these would ftell(NULL)/fread(NULL) crash pre-guard.
	Check( db->getUInt() == 0,   "F: getUInt on null-hFile returns 0 (no crash)" );
	Check( db->getDouble() == 0.0, "F: getDouble on null-hFile returns 0 (no crash)" );
	unsigned char dest[8];
	std::memset( dest, 0x33, sizeof( dest ) );
	const bool failed = !db->getBytes(dest, sizeof(dest));
	bool zeroed = true;
	for( unsigned char byte : dest ) zeroed &= byte == 0;
	Check( failed && zeroed, "F: missing-file reads fail and zero every unread destination byte" );

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
	Check( v->Width() == 0 && v->Height() == 0 && v->Depth() == 0,
		"G: overflow volume reports zero dimensions (rejected before alloc)" );
	Check( ByteAt( v, 0, 0, 0 ) == 0.0, "G: overflow volume GetValue returns 0 (no crash / no OOB)" );
	v->release();

	// A dimension >= 2^31: the int members would sign-extend to a huge (ull)
	// and wrap the product SMALL if the guard read the members instead of the
	// raw unsigned params.  Must still be rejected (empty volume).
	Volume<unsigned char>* v2 = new Volume<unsigned char>( pattern.c_str(), 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0 );
	Check( v2->Width() == 0 && v2->Height() == 0 && v2->Depth() == 0,
		"G: >=2^31 dimension rejected (no sign-extension bypass)" );
	Check( ByteAt( v2, 0, 0, 0 ) == 0.0, "G: >=2^31 volume GetValue returns 0 (no OOB)" );
	v2->release();

	// Reversed Z range (zend < zstart) would underflow the unsigned depth to a
	// huge value; must be rejected too.
	Volume<unsigned char>* v3 = new Volume<unsigned char>( pattern.c_str(), 4, 4, 5, 2 );
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

static void TestTruncatedOpenFile()
{
	const auto path = RISE::TestSupport::ScratchPath( "rise_open_truncate_" +
		std::to_string(RISE::TestSupport::CurrentProcessId()) );
	const std::vector<std::function<bool(DiskFileReadBuffer&)>> readers = {
		[](DiskFileReadBuffer& b) { return b.getChar() == 'a' && b.getChar() == 0; },
		[](DiskFileReadBuffer& b) { return b.getUChar() == 'a' && b.getUChar() == 0; },
		[](DiskFileReadBuffer& b) { return b.getWord() == 0; },
		[](DiskFileReadBuffer& b) { return b.getUWord() == 0; },
		[](DiskFileReadBuffer& b) { return b.getInt() == 0; },
		[](DiskFileReadBuffer& b) { return b.getUInt() == 0; },
		[](DiskFileReadBuffer& b) { return b.getFloat() == 0; },
		[](DiskFileReadBuffer& b) { return b.getDouble() == 0; },
		[](DiskFileReadBuffer& b) {
			char bytes[6] = {};
			return !b.getBytes(bytes, sizeof(bytes)) && bytes[0] == 'a' && bytes[1] == 0 && bytes[5] == 0;
		},
		[](DiskFileReadBuffer& b) {
			char line[8] = {};
			return b.getLine(line, sizeof(line)) == 1 && line[0] == 'a' && b.getLine(line, sizeof(line)) == 0;
		}
	};
	for( const auto& read : readers ) {
		std::ofstream(path, std::ios::binary) << "abcdef";
		auto* buffer = new DiskFileReadBuffer(path.c_str());
		std::filesystem::resize_file(path, 1);
		Check( read(*buffer) && buffer->EndOfBuffer() && buffer->Size64() == 1 && buffer->getCurPos64() == 1,
			"post-open truncation exposes the actual EOF and terminates readers" );
		buffer->release();
	}
	std::filesystem::remove(path);
}

static void TestMediaSearchRoots()
{
	namespace fs = std::filesystem;
	const auto root = fs::path( RISE::TestSupport::ScratchPath( "media_lookup" ) );
	const auto first = root / "first";
	const auto second = root / "second";
	const std::string name = u8"rise_media_é.bin";
	const std::string firstRoot = first.string();
	const std::string secondRoot = second.string();
	fs::create_directories( first );
	fs::create_directories( second );
	std::ofstream( firstRoot + "/" + name ) << '1';
	std::ofstream( secondRoot + "/" + name ) << '2';
	MediaPathLocator locator;
	Check( !locator.AddPath( "" ) && locator.AddPath( firstRoot.c_str() ) &&
		!locator.AddPath( (firstRoot + "/").c_str() ) && locator.AddPath( secondRoot.c_str() ),
		"media roots reject empty input and equivalent duplicate spellings" );
	const auto found = locator.Find(name.c_str());
	std::ifstream readable(found.c_str(), std::ios::binary);
	char value = 0;
	Check( found == (firstRoot + "/" + name).c_str() && readable.get(value) && value == '1',
		"media search order and native non-ASCII paths match narrow stream opens" );
	readable.close();
	const std::string direct = secondRoot + "/" + name;
	Check( locator.Find( direct.c_str() ) == direct.c_str() &&
		locator.RemovePath( (firstRoot + "/").c_str() ) &&
		locator.Find( name.c_str() ) == (secondRoot + "/" + name).c_str(),
		"media lookup prefers a direct path and removal accepts the normalized spelling" );
	locator.ClearAllPaths();
	Check( locator.Find( name.c_str() ) == name.c_str(), "a media miss preserves original spelling" );
	fs::remove_all( root );
}

namespace
{
	using Bytes3DS = std::vector<unsigned char>;

	void Append3DSWord( Bytes3DS& bytes, unsigned int value )
	{
		bytes.push_back( static_cast<unsigned char>(value) );
		bytes.push_back( static_cast<unsigned char>(value >> 8) );
	}

	void Append3DSUInt( Bytes3DS& bytes, unsigned int value )
	{
		for( unsigned int shift = 0; shift < 32; shift += 8 )
			bytes.push_back( static_cast<unsigned char>(value >> shift) );
	}

	void Append3DSBytes( Bytes3DS& bytes, const Bytes3DS& extra )
	{
		bytes.insert( bytes.end(), extra.begin(), extra.end() );
	}

	Bytes3DS Make3DSChunk( unsigned int id, const Bytes3DS& payload )
	{
		Bytes3DS bytes;
		Append3DSWord( bytes, id );
		Append3DSUInt( bytes, static_cast<unsigned int>(payload.size() + 6) );
		Append3DSBytes( bytes, payload );
		return bytes;
	}

	Bytes3DS Make3DSObject( bool emptyVertices, bool emptyFaces, bool invalidIndex = false, bool omitFaces = false )
	{
		Bytes3DS vertices;
		Append3DSWord( vertices, emptyVertices ? 0 : 3 );
		if( !emptyVertices ) {
			for( float component : { 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f } ) {
				std::uint32_t bits;
				std::memcpy( &bits, &component, sizeof(bits) );
				Append3DSUInt( vertices, bits );
			}
		}
		Bytes3DS faces;
		Append3DSWord( faces, emptyFaces ? 0 : 1 );
		if( !emptyFaces ) {
			for( unsigned int value : { 0U, 1U, invalidIndex ? 3U : 2U, 0U } ) Append3DSWord( faces, value );
		}
		Bytes3DS mesh = Make3DSChunk( 0x4110, vertices );
		if( !omitFaces ) Append3DSBytes( mesh, Make3DSChunk( 0x4120, faces ) );
		Bytes3DS object = { 'm', 0 };
		Append3DSBytes( object, Make3DSChunk( 0x4100, mesh ) );
		return Make3DSChunk( 0x4000, object );
	}

	void Check3DSLoad( const Bytes3DS& bytes, int selected, bool expected, unsigned int points )
	{
		auto* buffer = new MemoryBuffer( static_cast<unsigned int>(bytes.size()) );
		buffer->setBytes( bytes.data(), static_cast<unsigned int>(bytes.size()) );
		auto* loader = new TriangleMeshLoader3DS( buffer );
		auto* geometry = new TriangleMeshGeometryIndexed( false, false );
		Check( loader->LoadTriangleMesh( geometry, selected ) == expected && geometry->numPoints() == points,
			"3DS compatibility fixture preserves usable vertices and load outcome" );
		geometry->release();
		loader->release();
		buffer->release();
	}
}

static void Test3DSCompatibility()
{
	for( int version : { 3, 2, -1 } ) {
		for( unsigned int empty : { 0U, 1U, 2U } ) {
			for( bool badTail : { false, true } ) {
				Bytes3DS main;
				if( version >= 0 ) {
					Bytes3DS value;
					Append3DSUInt( value, static_cast<unsigned int>(version) );
					Append3DSBytes( main, Make3DSChunk( 0x0002, value ) );
				}
				Bytes3DS edit;
				if( empty ) Append3DSBytes( edit, Make3DSObject( empty == 1, true ) );
				Append3DSBytes( edit, Make3DSObject( false, false ) );
				Append3DSBytes( main, Make3DSChunk( 0x3d3d, edit ) );
				if( badTail ) {
					Append3DSWord( main, 0xb000 );
					Append3DSUInt( main, 1 );
				}
				auto bytes = Make3DSChunk( 0x4d4d, main );
				Check3DSLoad( bytes, -1, true, 3 );
				if( empty ) Check3DSLoad( bytes, 0, true, 0 );
			}
		}
	}
	Bytes3DS emptyWithoutFaces = Make3DSObject(true, true, false, true);
	Append3DSBytes( emptyWithoutFaces, Make3DSObject(false, false) );
	Check3DSLoad( Make3DSChunk(0x4d4d, Make3DSChunk(0x3d3d, emptyWithoutFaces)), -1, true, 3 );
	const auto edit = Make3DSChunk( 0x3d3d, Make3DSObject( false, false ) );
	auto truncatedTail = Make3DSChunk( 0x4d4d, edit );
	truncatedTail[2] += 100;
	Check3DSLoad( truncatedTail, -1, true, 3 );
	auto truncatedMesh = Make3DSChunk( 0x4d4d, edit );
	truncatedMesh.pop_back();
	Check3DSLoad( truncatedMesh, -1, false, 0 );
	Check3DSLoad( Make3DSChunk( 0x4d4d, Make3DSChunk( 0x3d3d,
		Make3DSObject(false, false, true) ) ), -1, false, 0 );
}

static void TestLargeDiskWrites()
{
#ifndef _WIN32
	// POSIX filesystems leave unwritten gaps sparse, keeping this fixture small.
	const auto path = RISE::TestSupport::ScratchPath( "rise_large_buffer_" +
		std::to_string(RISE::TestSupport::CurrentProcessId()) );
	auto* buffer = new DiskFileWriteBuffer( path.c_str() );
	const auto boundary = static_cast<std::uint64_t>(std::numeric_limits<unsigned int>::max());
	Check( buffer->seek(IBuffer::START, std::numeric_limits<int>::max()) &&
		buffer->seek(IBuffer::CUR, std::numeric_limits<int>::max()) && buffer->setUInt(0x12345678),
		"disk scalar write crosses the 4 GiB boundary" );
	Check( buffer->getCurPos64() == boundary + 3 && buffer->Size64() == boundary + 3 &&
		buffer->getCurPos() == std::numeric_limits<unsigned int>::max() &&
		buffer->Size() == std::numeric_limits<unsigned int>::max(),
		"large disk position and size retain 64 bits while legacy queries saturate" );
	Check( buffer->seek(IBuffer::CUR, -2) && buffer->getCurPos64() == boundary + 1 &&
		buffer->HowFarToEnd() == 2 && !buffer->EndOfBuffer() && buffer->setUWord(0xabcd) && buffer->Finish(),
		"large backward seek and overwrite preserve the 64-bit cursor" );
	buffer->release();
	Check( std::filesystem::file_size(path) == boundary + 3, "large disk output has the complete sparse-file length" );
	std::ifstream file( path, std::ios::binary );
	file.seekg( static_cast<std::streamoff>(boundary - 1) );
	unsigned char bytes[4] = {};
	file.read( reinterpret_cast<char*>(bytes), sizeof(bytes) );
	const unsigned char expected[] = { 0x78, 0x56, 0xcd, 0xab };
	Check( file && std::memcmp(bytes, expected, sizeof(bytes)) == 0,
		"bytes on both sides of 4 GiB match independent little-endian expectations" );
	file.close();
	auto* reader = new DiskFileReadBuffer( path.c_str() );
	Check( reader->Size64() == boundary + 3 && reader->seek64(IBuffer::START, boundary - 1) &&
		reader->getUInt() == 0xabcd5678 && reader->EndOfBuffer(),
		"disk scalar read crosses 4 GiB and reaches the actual end" );
	reader->release();
	std::filesystem::remove( path );
#endif
}

static void TestLargeCodecOffsets()
{
#if !defined(_WIN32) && (!defined(NO_EXR_SUPPORT) || !defined(NO_TIFF_SUPPORT))
	const auto path = RISE::TestSupport::ScratchPath( "rise_codec_offsets_" +
		std::to_string(RISE::TestSupport::CurrentProcessId()) );
	const std::uint64_t offset = static_cast<std::uint64_t>(std::numeric_limits<unsigned int>::max()) + 16;
	auto* writer = new DiskFileWriteBuffer(path.c_str());
#ifndef NO_EXR_SUPPORT
	OStreamWrapper output(*writer);
	output.seekp(offset);
	output.write("EXR!", 4);
	Check( output.tellp() == offset + 4, "EXR output wrapper preserves a large offset" );
#endif
#ifndef NO_TIFF_SUPPORT
	auto* writeInterface = static_cast<IWriteBuffer*>(writer);
	char markerWrite = 'T';
	Check( TIFFSeekWriter(writeInterface, offset + 32, SEEK_SET) == offset + 32 &&
		TIFFWrite(writeInterface, &markerWrite, 1) == 1 &&
		TIFFSizeWriter(writeInterface) == offset + 33, "TIFF output callbacks preserve large offsets and sizes" );
#endif
	Check( writer->Finish(), "large codec callback file closes successfully" );
	writer->release();
	auto* reader = new DiskFileReadBuffer(path.c_str());
#ifndef NO_EXR_SUPPORT
	IStreamWrapper input(*reader);
	input.seekg(offset);
	char marker[4]{};
	Check( input.read(marker, 4) && std::memcmp(marker, "EXR!", 4) == 0 && input.tellg() == offset + 4,
		"EXR input wrapper reads at the complete large offset" );
#endif
#ifndef NO_TIFF_SUPPORT
	auto* readInterface = static_cast<IReadBuffer*>(reader);
	char markerTIFF = 0;
	Check( TIFFSeekReader(readInterface, offset + 32, SEEK_SET) == offset + 32 &&
		TIFFRead(readInterface, &markerTIFF, 1) == 1 && markerTIFF == 'T' &&
		TIFFSizeReader(readInterface) == offset + 33, "TIFF input callbacks preserve large offsets and sizes" );
	Check( TIFFSeekReader(readInterface, static_cast<toff_t>(-1), SEEK_END) == offset + 32 &&
		TIFFRead(readInterface, &markerTIFF, 1) == 1 && markerTIFF == 'T' &&
		TIFFSeekReader(readInterface, static_cast<toff_t>(-1), SEEK_CUR) == offset + 32,
		"TIFF callbacks handle negative relative offsets without losing the large base" );
#endif
	reader->release();
	std::filesystem::remove(path);
#endif
#ifndef NO_EXR_SUPPORT
	char byte = 'x';
	auto* borrowed = new MemoryBuffer(&byte, 1, false);
	OStreamWrapper outputSmall(*borrowed);
	bool refused = false;
	try { outputSmall.write("xx", 2); }
	catch( const std::exception& ) { refused = true; }
	Check( refused && byte == 'x', "EXR output wrapper reports a failed write without overrunning borrowed storage" );
	refused = false;
	try { outputSmall.seekp(static_cast<std::uint64_t>(std::numeric_limits<int>::max()) + 1); }
	catch( const std::exception& ) { refused = true; }
	Check( refused && borrowed->getCurPos() == 0, "EXR output wrapper reports an unrepresentable legacy-buffer seek" );
	borrowed->release();
#endif
#ifndef NO_TIFF_SUPPORT
	class RejectingSeekBuffer final : public MemoryBuffer {
	public:
		unsigned int writes = 0;
		RejectingSeekBuffer() : MemoryBuffer(1) {}
		bool setBytes(const void*, unsigned int) override { ++writes; return false; }
	};
	auto* limited = new RejectingSeekBuffer();
	Check( TIFFSeekWriter(static_cast<IWriteBuffer*>(limited),
		static_cast<toff_t>(std::numeric_limits<int>::max()) + 1, SEEK_SET) == static_cast<toff_t>(-1) &&
		limited->writes == 0 && limited->getCurPos() == 0,
		"TIFF refuses an unsupported legacy-buffer seek before filling any gap" );
	limited->release();
#endif
}

static void TestStagedOutputFallback()
{
	namespace fs = std::filesystem;
	const fs::path root = RISE::TestSupport::ScratchTestRoot( "rise_staged_output_" );
	fs::create_directory( root );
	const auto path = root / "image.bin";
	const auto read = []( const fs::path& input ) {
		std::ifstream file( input, std::ios::binary );
		return std::string( std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() );
	};
	std::ofstream( path ) << "original";
	Check( !DiskFileWriteBuffer::WriteAtomically(path.string(), []( DiskFileWriteBuffer& buffer ) {
		buffer.setBytes("partial", 7);
		return buffer.setBytes(nullptr, 1);
	}) && read(path) == "original", "failed staged write preserves the previous file" );
	Check( !DiskFileWriteBuffer::WriteAtomically(path.string(), []( DiskFileWriteBuffer& buffer ) -> bool {
		buffer.setBytes("partial", 7);
		throw std::runtime_error("encoder fixture failure");
	}) && read(path) == "original", "encoder exceptions preserve the previous file and report failure" );
	Check( !DiskFileWriteBuffer::WriteAtomically(path.string(), []( DiskFileWriteBuffer& ) { return true; }) &&
		read(path) == "original", "empty staged output preserves the previous file" );
	Check( std::distance(fs::directory_iterator(root), fs::directory_iterator()) == 1,
		"failed staged write removes its temporary file and directory" );
	Check( DiskFileWriteBuffer::WriteAtomically(path.string(), []( DiskFileWriteBuffer& buffer ) {
		return buffer.setBytes("complete", 8);
	}) && read(path) == "complete", "successful staged write replaces the file" );
#ifndef _WIN32
	fs::permissions( root, fs::perms::owner_read | fs::perms::owner_exec );
	std::error_code permissionError;
	const bool restricted = !fs::create_directory( root / "permission-probe", permissionError );
	if( restricted ) {
		bool invoked = false;
		const bool refused = !DiskFileWriteBuffer::WriteAtomically(path.string(), [&]( DiskFileWriteBuffer& ) {
			invoked = true;
			return true;
		});
		const bool fallback = DiskFileWriteBuffer::WriteAtomically(path.string(), []( DiskFileWriteBuffer& buffer ) {
			return buffer.setBytes("direct", 6);
		}, DiskFileWriteBuffer::StagingPolicy::AllowInPlace);
		fs::permissions( root, fs::perms::owner_all );
		Check( refused && !invoked && fallback && read(path) == "direct",
			"restricted parent preserves required staging and permits explicit in-place output" );
	} else {
		fs::permissions( root, fs::perms::owner_all );
		fs::remove( root / "permission-probe" );
		std::cout << "SKIP: process can create files in the restricted staging parent" << std::endl;
	}
	const auto target = root / "created.bin";
	const auto link = root / "broken-link.bin";
	fs::create_symlink( target.filename(), link );
	Check( DiskFileWriteBuffer::WriteAtomically(link.string(), []( DiskFileWriteBuffer& buffer ) {
		return buffer.setBytes("linked", 6);
	}, DiskFileWriteBuffer::StagingPolicy::AllowInPlace) && fs::is_symlink(link) && read(target) == "linked",
		"in-place fallback writes through a dangling symlink and preserves the link" );
#endif
	fs::remove_all( root );
}

static const std::string& LateLogPath()
{
	static const auto path = RISE::TestSupport::ScratchPath( "rise_late_log_" +
		std::to_string(RISE::TestSupport::CurrentProcessId()) + std::string(80, 'l') + ".txt" );
	return path;
}

static void CheckLateLogRecreation()
{
	GlobalLogCleanupAndShutdown();
	GlobalLog()->Print( eLog_Event, "late static-destruction log fixture" );
	GlobalLogCleanupAndShutdown();
	std::ifstream file( LateLogPath() );
	const std::string contents( std::istreambuf_iterator<char>(file), {} );
	if( contents.find("late static-destruction log fixture") == std::string::npos ) std::abort();
	file.close();
	std::remove( LateLogPath().c_str() );
}

int main( int /*argc*/, char* /*argv*/[] )
{
	// Register before constructing the logger's filename storage. Its former
	// static-string destructor ran before this callback, which ASan detects.
	LateLogPath();
	std::atexit( CheckLateLogRecreation );
	SetGlobalLogFileName( LateLogPath().c_str() );
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
	TestTruncatedOpenFile();
	Test3DSCompatibility();
	TestLargeDiskWrites();
	TestLargeCodecOffsets();
	TestStagedOutputFallback();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
