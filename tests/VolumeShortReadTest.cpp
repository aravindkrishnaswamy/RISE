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
#include <string>
#include <vector>
#include <cstdio>
#include <cmath>

#include "../src/Library/Utilities/Math3D/Math3D.h"   // Scalar (double)
#include "../src/Library/Volume/Volume.h"
#include "../src/Library/Utilities/MemoryBuffer.h"

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
	std::string dir = tmp ? tmp : "/tmp/";
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

// GetValue returns byte/255 for T=unsigned char; recover the raw byte.
static double ByteAt( const Volume<unsigned char>* v, int x, int y, int z )
{
	return v->GetValue( x, y, z ) * 255.0;
}

//----------------------------------------------------------------------
// Volume: present / truncated / missing slices.
//   w=h=4, slices z=0,1,2 (centered coords: z=-1 -> slice0, 0 -> slice1,
//   +1 -> slice2; x,y in [-2,1]).
//   slice0: full 16 bytes of 200 (present)
//   slice1: only 8 of 16 bytes of 100 (TRUNCATED -> tail must be 0)
//   slice2: file absent (MISSING -> whole slice must be 0)
//----------------------------------------------------------------------
static void TestVolumeSlices()
{
	std::cout << "A: Volume present / truncated / missing slices read back zero (not garbage)" << std::endl;

	const std::string base = TmpDir() + "rise_voltest_slice_";
	const std::string s0 = base + "0.raw";
	const std::string s1 = base + "1.raw";
	const std::string s2 = base + "2.raw";
	std::remove( s2.c_str() );          // ensure slice 2 is absent
	WriteBytes( s0, 200, 16 );          // full slice
	WriteBytes( s1, 100, 8 );           // truncated: 8 of 16 bytes

	const std::string pattern = base + "%d.raw";
	Volume<unsigned char>* v = new Volume<unsigned char>( pattern.c_str(), 4, 4, 0, 2 );
	v->addref();

	// slice0 (z=-1): fully present -> 200 everywhere.
	Check( std::fabs( ByteAt( v, -2, -2, -1 ) - 200.0 ) < 1.5, "A: slice0 corner == 200 (present)" );
	Check( std::fabs( ByteAt( v,  1,  1, -1 ) - 200.0 ) < 1.5, "A: slice0 far corner == 200 (present)" );

	// slice1 (z=0): first 8 bytes (rows y=-2,-1) == 100; tail (rows y=0,1) zeroed.
	Check( std::fabs( ByteAt( v, -2, -2, 0 ) - 100.0 ) < 1.5, "A: slice1 read region == 100" );
	Check( std::fabs( ByteAt( v, -2,  0, 0 ) - 0.0   ) < 1.5, "A: slice1 truncated tail == 0 (not garbage)" );
	Check( std::fabs( ByteAt( v,  1,  1, 0 ) - 0.0   ) < 1.5, "A: slice1 far truncated tail == 0" );

	// slice2 (z=+1): file missing -> entirely zero.
	Check( std::fabs( ByteAt( v, -2, -2, 1 ) - 0.0 ) < 1.5, "A: missing slice2 corner == 0 (not garbage)" );
	Check( std::fabs( ByteAt( v,  1,  1, 1 ) - 0.0 ) < 1.5, "A: missing slice2 far corner == 0" );

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

int main( int /*argc*/, char* /*argv*/[] )
{
	std::cout << "VolumeShortReadTest — missing/truncated binary reads zero, never uninitialised"
		<< std::endl;

	TestVolumeSlices();
	TestMemoryBufferMissingFile();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
