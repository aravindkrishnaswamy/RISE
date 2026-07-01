//////////////////////////////////////////////////////////////////////
//
//  HalResetRegressionTest.cpp - Regression guard for the legacy
//    parser's hal() QMC-sequence per-top-level-parse reset.
//
//  The legacy streaming loader (AsciiSceneParser::ParseAndLoadScene)
//  evaluates the scene-expression function hal(d) via a file-scope
//  MultiHalton `mh`, advancing that dimension's sample index once per
//  hal(d) occurrence.  Each TOP-LEVEL LoadAsciiScene must start that
//  sequence FRESH (standalone-equivalent + order-independent); the
//  sample state must NOT leak across successive top-level loads in one
//  process (this reaches 3DSMax's 2-loads-per-render, DRISE, GUI reopen).
//
//  Slice 6b split the parser into a streaming loader + a shared
//  registry TU, and the mh.Reset() moved (with ClearParseState) into
//  the registry TU -- resetting the registry's DEAD copy of `mh`, not
//  the one the streaming loader actually consumes.  That left the
//  consumer's sequence reset NOWHERE, so hal() leaked across loads:
//    load#1 hal(1) == 0.0 ; load#2 hal(1) == 0.333... (LEAKED).
//
//  This test legacy-loads a scene whose camera location.x is `hal(1)`
//  TWICE in one process and asserts the second load's resolved value
//  EQUALS the first's (both fresh).  Pre-fix it FAILS (0.333 != 0.0);
//  post-fix both are 0.0.
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/Interfaces/ICameraManager.h"
#include "../src/Library/Interfaces/IScenePriv.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* testName )
{
	if( condition ) { passCount++; }
	else { failCount++; std::cout << "  FAIL: " << testName << std::endl; }
}

// Finite poison: a value hal() (which lands in [0,1)) can never legitimately
// be, so an unchanged `out` after LoadAndReadHal signals a load/camera
// failure -- surfaced via an explicit existence Check, not a NaN/Inf sentinel
// (those fold under -ffast-math; see docs/skills/red-proof-and-test-integrity.md,
// enforced by SourceHygieneTest).
static const Scalar kPoison = Scalar( -999.0 );

// Minimal scene shell.  The camera's location.x is bound to hal(1) via
// the $(...) scene-expression syntax, so the resolved x co-ordinate is
// the value hal() produced for dimension 1 during THIS parse.  Chunk
// braces on their own lines (parser requirement).
static const char* kSceneShell =
	"RISE ASCII SCENE 6\n"
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultDirectLighting\n"
	"}\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation $(hal(1)) 0 20\n"
	"\tlookat 0 0 0\n"
	"\tup 0 1 0\n"
	"\tfov 30\n"
	"}\n"
	"\n";

static bool WriteScene( const std::string& path )
{
	std::ofstream out( path.c_str() );
	if( !out.is_open() ) return false;
	out << kSceneShell;
	return out.good();
}

// Legacy-load the scene; on success set `outX` to the active camera's
// location.x (the resolved hal(1) value) and return true.  On any load /
// camera failure leaves `outX` at its poison and returns false.
static bool LoadAndReadHal( const std::string& path, Scalar& outX )
{
	outX = kPoison;
	Job* pJob = new Job();
	if( !pJob->LoadAsciiScene( path.c_str() ) ) {
		pJob->release();
		return false;
	}
	const ICamera* cam = pJob->GetScene()->GetCamera();
	const bool ok = ( cam != nullptr );
	if( ok ) {
		outX = cam->GetLocation().x;
	}
	pJob->release();
	return ok;
}

int main( int /*argc*/, char* /*argv*/[] )
{
	std::cout << "=== HalResetRegressionTest ===" << std::endl;

	const std::string path = "/tmp/rise_hal_reset_test.RISEscene";
	Check( WriteScene( path ), "wrote hal() scene" );

	// Two TOP-LEVEL legacy loads in one process.
	Scalar first  = kPoison;
	Scalar second = kPoison;
	const bool ok1 = LoadAndReadHal( path, first  );
	const bool ok2 = LoadAndReadHal( path, second );

	std::cout << "  load#1 hal(1) resolved location.x = " << first  << std::endl;
	std::cout << "  load#2 hal(1) resolved location.x = " << second << std::endl;

	// Explicit existence Checks (finite poison, no NaN sentinel).
	Check( ok1, "load#1 succeeded + camera present" );
	Check( ok2, "load#2 succeeded + camera present" );

	// Fresh hal(1) sequence => halton(1, 0) == 0.0 on both loads.
	Check( std::fabs( first ) < 1e-9,
	       "load#1 hal(1) == 0.0 (fresh sequence)" );

	// THE REGRESSION ASSERTION: the second load must match the first
	// (fresh sequence each), i.e. the QMC state does NOT leak.  Pre-fix
	// this fails because load#2 sees halton(1,1) == 0.333...
	Check( std::fabs( second - first ) < 1e-9,
	       "load#2 hal(1) EQUALS load#1 (no cross-load QMC leak)" );

	std::remove( path.c_str() );

	std::cout << "Passed: " << passCount << "  Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
