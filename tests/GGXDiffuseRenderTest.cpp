//////////////////////////////////////////////////////////////////////
//
//  GGXDiffuseRenderTest.cpp - DL-37 render-fixture smoke test.
//
//  The committed scene is the before/after image fixture for
//  GGX diffuse interface transmission.  This test deliberately verifies
//  only that its native-v7 CST path loads and its seeded render completes.
//  The default multithreaded renderer is used; noise is not bit-reproducible;
//  transport correctness is covered by the focused GGX tests.
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>

#include "../src/Library/Job.h"

using namespace RISE;

int main()
{
	const char* const scenePath = "scenes/Tests/Materials/ggx_diffuse_transmission.RISEscene";
	Job* const pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( scenePath ) ) {
		std::printf( "FAIL: could not load %s via CST\n", scenePath );
		pJob->release();
		return 1;
	}

	std::srand( 37037u );
	const bool rendered = pJob->Rasterize();
	pJob->release();

	if( !rendered ) {
		std::printf( "FAIL: seeded render did not complete\n" );
		return 1;
	}

	std::printf( "GGX diffuse render fixture passed\n" );
	return 0;
}
