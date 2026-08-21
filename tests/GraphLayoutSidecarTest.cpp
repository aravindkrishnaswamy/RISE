//////////////////////////////////////////////////////////////////////
//
//  GraphLayoutSidecarTest.cpp - doc-88 Phase 3 S13:
//    docs/gui/NODE_GRAPH_CANVAS.md sect. 6's "S13 -- layout sidecar
//    format + read/write" (src/Library/SceneEditor/
//    GraphLayoutSidecar.{h,cpp}).
//
//  Every case here is headless filesystem I/O in a temp directory (the
//  same `fs::temp_directory_path() / "rise_<prefix>_<timestamp>"`
//  pattern AgentTrajectoryTest.cpp already uses) -- no controller, no
//  Job, no live Document; GraphLayoutTest.cpp already covers the
//  layout ALGORITHM this format merely persists.
//
//  Parts:
//    PART 1 -- ParsePositions/SerializePositions round-trip (pure, no
//      filesystem).
//    PART 2 -- ReadSidecar/WriteSidecar through a real temp file:
//      missing-sidecar-falls-back-to-empty, write-then-read round-trip,
//      "only when changed" (proven by making the directory read-only
//      and showing an unchanged rewrite is skipped while a changed one
//      is attempted -- POSIX only, see the case's own comment),
//      atomic-write (no leftover .tmp.* file after a successful write).
//    PART 3 -- malformed / unknown-key / non-finite / absurd-magnitude
//      input handling (design brief: "never fatal").
//    PART 4 -- orphan-prune on write.
//    PART 5 -- MigrateName: success, "nothing to migrate", and the
//      refuse-to-clobber case.
//    PART 6 -- unsaved-scene (empty scenePath) never writes.
//    PART 7 -- composition: MigrateName then WriteSidecar (rename-then-
//      save) -- the migrated entry survives under its new name and no
//      orphan remains under the old one, through a real temp file.
//    PART 8 -- MigrateSidecarOnSaveAs (doc-88 S14 review round P2(2)):
//      a real Save-As copy through a temp file, no-op cases (empty path,
//      equal paths, no old sidecar), the never-overwrite-the-destination
//      rule, and that the OLD sidecar is left in place either way.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <system_error>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "../src/Library/SceneEditor/GraphLayoutSidecar.h"

using namespace RISE;
namespace fs = std::filesystem;

static int passCount = 0, failCount = 0;
static void Check( bool c, const char* n )
{
	if( c ) { ++passCount; }
	else    { ++failCount; std::cout << "  FAIL: " << n << std::endl; }
}
static void CheckDoubleEq( double got, double want, const char* n )
{
	if( got == want ) { ++passCount; return; }
	++failCount;
	std::cout << "  FAIL: " << n << "\n    got  : " << got << "\n    want : " << want << std::endl;
}

static fs::path TestDir()
{
	static long long counter = 0;
	const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch() ).count();
	fs::path dir = fs::temp_directory_path() /
		( std::string( "rise_graphlayoutsidecar_test_" ) + std::to_string( ms ) + "_" + std::to_string( ++counter ) );
	std::error_code ec;
	fs::create_directories( dir, ec );
	return dir;
}

static bool ReadWholeFile( const fs::path& p, std::string& out )
{
	std::ifstream ifs( p, std::ios::in | std::ios::binary );
	if( !ifs.is_open() ) return false;
	std::ostringstream ss;
	ss << ifs.rdbuf();
	out = ss.str();
	return true;
}

static bool AnyTmpFileIn( const fs::path& dir )
{
	std::error_code ec;
	for( const fs::directory_entry& entry : fs::directory_iterator( dir, ec ) ) {
		const std::string name = entry.path().filename().string();
		if( name.find( ".tmp." ) != std::string::npos ) return true;
	}
	return false;
}

// =======================================================================
// PART 1 -- ParsePositions / SerializePositions round-trip
// =======================================================================

static void TestSerializeParseRoundTrip()
{
	std::cout << "PART 1: SerializePositions -> ParsePositions round-trip" << std::endl;
	GraphLayout::Positions original;
	original["Alpha"] = GraphLayoutPoint{ 10.5, -3.25 };
	original["Beta"]  = GraphLayoutPoint{ 0.0, 220.0 };
	original["Gamma"] = GraphLayoutPoint{ -140.0, 0.0 };

	const std::string text = GraphLayoutSidecar::SerializePositions( original );
	Check( text.find( "\"version\"" ) != std::string::npos, "1: serialized text carries a version key" );
	Check( text.find( "\"nodes\"" ) != std::string::npos, "1: serialized text carries a nodes key" );

	GraphLayout::Positions parsed;
	unsigned int sanitized = 999;
	const bool ok = GraphLayoutSidecar::ParsePositions( text, parsed, &sanitized );
	Check( ok, "1: round-tripped text parses successfully" );
	Check( sanitized == 0, "1: nothing needed sanitizing" );
	Check( parsed.size() == original.size(), "1: same entry count after round-trip" );
	for( const std::pair<const std::string, GraphLayoutPoint>& kv : original ) {
		const GraphLayout::Positions::const_iterator it = parsed.find( kv.first );
		Check( it != parsed.end(), "1: every original name survives the round-trip" );
		if( it != parsed.end() ) {
			CheckDoubleEq( it->second.x, kv.second.x, "1: x survives the round-trip exactly" );
			CheckDoubleEq( it->second.y, kv.second.y, "1: y survives the round-trip exactly" );
		}
	}
}

static void TestSerializeIsDeterministic()
{
	std::cout << "PART 1b: SerializePositions is deterministic (map's own key order)" << std::endl;
	GraphLayout::Positions positions;
	positions["Z"] = GraphLayoutPoint{ 1.0, 1.0 };
	positions["A"] = GraphLayoutPoint{ 2.0, 2.0 };
	const std::string t1 = GraphLayoutSidecar::SerializePositions( positions );
	const std::string t2 = GraphLayoutSidecar::SerializePositions( positions );
	Check( t1 == t2, "1b: two serializations of the same map are byte-identical" );
	// std::map iterates lexically -- "A" is serialized before "Z".
	Check( t1.find( "\"A\"" ) < t1.find( "\"Z\"" ), "1b: keys appear in lexical order" );
}

// =======================================================================
// PART 2 -- ReadSidecar/WriteSidecar through a real temp file
// =======================================================================

static void TestMissingSidecarIsEmpty()
{
	std::cout << "PART 2a: no sidecar file on disk -> ReadSidecar returns empty, no error" << std::endl;
	const fs::path dir = TestDir();
	const std::string scenePath = ( dir / "no_sidecar_here.RISEscene" ).string();
	const GraphLayout::Positions pos = GraphLayoutSidecar::ReadSidecar( scenePath );
	Check( pos.empty(), "2a: missing sidecar reads as empty positions" );
}

static void TestWriteThenReadRoundTrip()
{
	std::cout << "PART 2b: WriteSidecar then ReadSidecar round-trips" << std::endl;
	const fs::path dir = TestDir();
	const std::string scenePath = ( dir / "scene.RISEscene" ).string();
	const fs::path expectedSidecar = dir / "scene.RISEscene.risegraph.json";
	Check( GraphLayoutSidecar::SidecarPathForScene( scenePath ) == expectedSidecar.string(),
	       "2b: sidecar path is <scenePath>.risegraph.json" );

	GraphLayout::Positions positions;
	positions["P1"] = GraphLayoutPoint{ 100.0, 0.0 };
	positions["M1"] = GraphLayoutPoint{ 200.0, 50.0 };
	std::set<std::string> live;
	live.insert( "P1" );
	live.insert( "M1" );

	std::string err;
	const bool wrote = GraphLayoutSidecar::WriteSidecar( scenePath, positions, live, err );
	Check( wrote, ( std::string( "2b: WriteSidecar succeeds (" ) + err + ")" ).c_str() );
	Check( fs::exists( expectedSidecar ), "2b: sidecar file exists on disk after write" );
	Check( !AnyTmpFileIn( dir ), "2b: no leftover .tmp.* file after a successful write (atomic write cleanup)" );

	std::string onDisk;
	Check( ReadWholeFile( expectedSidecar, onDisk ), "2b: the sidecar file is readable" );
	Check( onDisk == GraphLayoutSidecar::SerializePositions( positions ),
	       "2b: on-disk bytes match SerializePositions' own canonical form exactly (nothing lost, nothing pruned -- live covers everything)" );

	const GraphLayout::Positions readBack = GraphLayoutSidecar::ReadSidecar( scenePath );
	Check( readBack.size() == 2, "2b: read-back has both entries" );
	const GraphLayout::Positions::const_iterator p1 = readBack.find( "P1" );
	const GraphLayout::Positions::const_iterator m1 = readBack.find( "M1" );
	Check( p1 != readBack.end() && m1 != readBack.end(), "2b: both names present after read-back" );
	if( p1 != readBack.end() ) { CheckDoubleEq( p1->second.x, 100.0, "2b: P1.x" ); CheckDoubleEq( p1->second.y, 0.0, "2b: P1.y" ); }
	if( m1 != readBack.end() ) { CheckDoubleEq( m1->second.x, 200.0, "2b: M1.x" ); CheckDoubleEq( m1->second.y, 50.0, "2b: M1.y" ); }
}

static void TestWriteOnlyWhenChanged()
{
#if defined(_WIN32)
	std::cout << "PART 2c: SKIPPED on Windows (POSIX chmod-based skip proof; standing debt, MSVC-gated per repo convention)" << std::endl;
#else
	std::cout << "PART 2c: an unchanged rewrite is skipped; a changed one is actually attempted" << std::endl;
	const fs::path dir = TestDir();
	const std::string scenePath = ( dir / "scene.RISEscene" ).string();
	std::set<std::string> live;
	live.insert( "P1" );

	GraphLayout::Positions positions;
	positions["P1"] = GraphLayoutPoint{ 1.0, 2.0 };
	std::string err;
	Check( GraphLayoutSidecar::WriteSidecar( scenePath, positions, live, err ), "2c: initial write succeeds" );

	// Lock the DIRECTORY (not the file -- POSIX rename only cares about
	// directory write permission) so any ATTEMPTED write -- opening a new
	// tmp file in this directory -- fails. If WriteSidecar correctly
	// skips an unchanged rewrite without touching the filesystem, this
	// call must still return true; if it always rewrites unconditionally,
	// this call fails, proving the "only when changed" rule is actually
	// load-bearing rather than a comment nobody enforces.
	// NOTE (root-unsafe): this proof relies on the OS actually enforcing
	// 0555 against the process, which does not hold for a root-executed
	// suite -- root bypasses directory permission bits, so the "changed
	// write is attempted and fails against the locked dir" assertion
	// below would itself FAIL (visibly, as a normal Check() failure, not
	// silently) rather than the intended write logic being unverified.
	const fs::path dirPath = dir;
	::chmod( dirPath.c_str(), 0555 );

	const bool unchangedWrite = GraphLayoutSidecar::WriteSidecar( scenePath, positions, live, err );
	Check( unchangedWrite, "2c: rewriting IDENTICAL content is skipped (succeeds even though the dir is locked)" );

	GraphLayout::Positions changed;
	changed["P1"] = GraphLayoutPoint{ 9.0, 9.0 };   // genuinely different content
	const bool changedWrite = GraphLayoutSidecar::WriteSidecar( scenePath, changed, live, err );
	Check( !changedWrite, "2c: a GENUINE content change is actually attempted, and fails against the locked dir" );

	::chmod( dirPath.c_str(), 0755 );   // restore before this test function returns
#endif
}

// =======================================================================
// PART 3 -- malformed / unknown-key / non-finite / absurd handling
// =======================================================================

static void TestMalformedSidecarIsEmptyNeverFatal()
{
	std::cout << "PART 3a: a malformed sidecar file reads as empty, never fatal" << std::endl;
	const fs::path dir = TestDir();
	const std::string scenePath = ( dir / "scene.RISEscene" ).string();
	const fs::path sidecarPath = dir / "scene.RISEscene.risegraph.json";
	{
		std::ofstream ofs( sidecarPath, std::ios::binary );
		ofs << "{ this is not valid json at all ][";
	}
	const GraphLayout::Positions pos = GraphLayoutSidecar::ReadSidecar( scenePath );
	Check( pos.empty(), "3a: malformed sidecar reads as empty (never fatal -- reaching this line proves it)" );

	// A top-level JSON value that IS valid JSON but not an object (the
	// format requires a top-level object) is equally "malformed" for
	// this format's purposes.
	GraphLayout::Positions parsed;
	Check( !GraphLayoutSidecar::ParsePositions( "[1,2,3]", parsed, nullptr ),
	       "3a: a top-level JSON array is rejected (format requires an object)" );
	Check( parsed.empty(), "3a: rejected parse leaves the output empty" );
}

static void TestUnknownKeysIgnored()
{
	std::cout << "PART 3b: unknown top-level and per-node keys are ignored, not fatal" << std::endl;
	const std::string text =
		"{\"version\":1,\"futureField\":\"ignored\","
		"\"nodes\":{\"N\":{\"x\":5.0,\"y\":6.0,\"frame\":\"ignored-too\"}}}";
	GraphLayout::Positions pos;
	const bool ok = GraphLayoutSidecar::ParsePositions( text, pos, nullptr );
	Check( ok, "3b: unknown keys do not fail the parse" );
	Check( pos.size() == 1, "3b: exactly the one real node is recovered" );
	const GraphLayout::Positions::const_iterator it = pos.find( "N" );
	Check( it != pos.end(), "3b: node N present" );
	if( it != pos.end() ) { CheckDoubleEq( it->second.x, 5.0, "3b: N.x" ); CheckDoubleEq( it->second.y, 6.0, "3b: N.y" ); }
}

static void TestAbsentNodesKeyIsEmptyNotMalformed()
{
	std::cout << "PART 3c: {\"version\":1} with no nodes key is well-formed-but-empty, not malformed" << std::endl;
	GraphLayout::Positions pos;
	const bool ok = GraphLayoutSidecar::ParsePositions( "{\"version\":1}", pos, nullptr );
	Check( ok, "3c: an absent nodes key is NOT a parse failure" );
	Check( pos.empty(), "3c: ...just an empty positions map" );
}

static void TestNonFiniteAndAbsurdCoordinatesSanitized()
{
	std::cout << "PART 3d: non-finite (via double overflow) and absurd-magnitude coordinates are sanitized" << std::endl;
	// "1e400"/"−1e400" overflow strtod's double range -- Json.cpp's own
	// number parser (std::strtod) returns +-HUGE_VAL for that, i.e. a
	// REAL non-finite double reaches ParsePositions, even though JSON's
	// own grammar cannot spell "Infinity" directly. This is what lets
	// this test exercise the non-finite branch through the public
	// text-based API rather than only the finite-but-absurd branch.
	const std::string text =
		"{\"version\":1,\"nodes\":{"
		"\"Inf\":{\"x\":1e400,\"y\":-1e400},"
		"\"Big\":{\"x\":2.0e7,\"y\":-2.0e7}"
		"}}";
	GraphLayout::Positions pos;
	unsigned int sanitized = 0;
	const bool ok = GraphLayoutSidecar::ParsePositions( text, pos, &sanitized );
	Check( ok, "3d: a document with sanitizable values still parses successfully" );
	Check( sanitized == 2, "3d: both entries needed sanitizing" );

	const GraphLayout::Positions::const_iterator inf = pos.find( "Inf" );
	Check( inf != pos.end(), "3d: Inf entry recovered (sanitized, not dropped)" );
	if( inf != pos.end() ) {
		CheckDoubleEq( inf->second.x, 0.0, "3d: non-finite x sanitized to 0.0 (no sign to clamp toward)" );
		CheckDoubleEq( inf->second.y, 0.0, "3d: non-finite y sanitized to 0.0" );
	}
	const GraphLayout::Positions::const_iterator big = pos.find( "Big" );
	Check( big != pos.end(), "3d: Big entry recovered (sanitized, not dropped)" );
	if( big != pos.end() ) {
		CheckDoubleEq( big->second.x,  GraphLayoutSidecar::kAbsurdCoordinateBound, "3d: finite-but-absurd x clamped, sign preserved (+)" );
		CheckDoubleEq( big->second.y, -GraphLayoutSidecar::kAbsurdCoordinateBound, "3d: finite-but-absurd y clamped, sign preserved (-)" );
	}
}

static void TestWrongTypedCoordinateSanitized()
{
	std::cout << "PART 3d2: a present-but-non-numeric coordinate is sanitized, counted, and logged like non-finite" << std::endl;
	const std::string text =
		"{\"version\":1,\"nodes\":{"
		"\"Garbage\":{\"x\":\"garbage\",\"y\":5.0}"
		"}}";
	GraphLayout::Positions pos;
	unsigned int sanitized = 0;
	const bool ok = GraphLayoutSidecar::ParsePositions( text, pos, &sanitized );
	Check( ok, "3d2: a document with a wrong-typed coordinate still parses successfully" );
	Check( sanitized == 1, "3d2: the wrong-typed entry counts toward *outSanitizedCount, not silently defaulted" );

	const GraphLayout::Positions::const_iterator it = pos.find( "Garbage" );
	Check( it != pos.end(), "3d2: Garbage entry recovered (sanitized, not dropped)" );
	if( it != pos.end() ) {
		CheckDoubleEq( it->second.x, 0.0, "3d2: non-numeric x sanitized to 0.0" );
		CheckDoubleEq( it->second.y, 5.0, "3d2: the sibling well-typed y is untouched" );
	}
}

static void TestOneMalformedNodeDoesNotCostOthers()
{
	std::cout << "PART 3e: one malformed node entry costs only itself, not the whole file" << std::endl;
	const std::string text =
		"{\"version\":1,\"nodes\":{"
		"\"Good\":{\"x\":1.0,\"y\":2.0},"
		"\"NotAnObject\":5,"
		"\"NeitherCoord\":{\"frame\":\"whatever\"}"
		"}}";
	GraphLayout::Positions pos;
	const bool ok = GraphLayoutSidecar::ParsePositions( text, pos, nullptr );
	Check( ok, "3e: the file as a whole still parses" );
	Check( pos.size() == 1, "3e: only the well-formed node survives" );
	Check( pos.find( "Good" ) != pos.end(), "3e: the well-formed node is exactly the one that survives" );
}

// =======================================================================
// PART 4 -- orphan-prune on write
// =======================================================================

static void TestOrphanPruneOnWrite()
{
	std::cout << "PART 4: an entry whose node is no longer live is pruned on write" << std::endl;
	const fs::path dir = TestDir();
	const std::string scenePath = ( dir / "scene.RISEscene" ).string();

	GraphLayout::Positions positions;
	positions["Alive"]  = GraphLayoutPoint{ 1.0, 1.0 };
	positions["Orphan"] = GraphLayoutPoint{ 2.0, 2.0 };   // NOT in liveNodeNames below
	std::set<std::string> live;
	live.insert( "Alive" );

	std::string err;
	Check( GraphLayoutSidecar::WriteSidecar( scenePath, positions, live, err ), "4: write succeeds" );

	const GraphLayout::Positions readBack = GraphLayoutSidecar::ReadSidecar( scenePath );
	Check( readBack.size() == 1, "4: exactly one entry survives the prune" );
	Check( readBack.find( "Alive" ) != readBack.end(), "4: the live node's entry survives" );
	Check( readBack.find( "Orphan" ) == readBack.end(), "4: the orphaned node's entry is dropped, silently" );
}

// =======================================================================
// PART 5 -- MigrateName
// =======================================================================

static void TestMigrateName()
{
	std::cout << "PART 5: MigrateName -- success, nothing-to-migrate, refuse-to-clobber" << std::endl;

	// Success case.
	{
		GraphLayout::Positions positions;
		positions["OldName"] = GraphLayoutPoint{ 3.0, 4.0 };
		const bool migrated = GraphLayoutSidecar::MigrateName( positions, "OldName", "NewName" );
		Check( migrated, "5a: MigrateName reports success" );
		Check( positions.find( "OldName" ) == positions.end(), "5a: old key is gone" );
		const GraphLayout::Positions::const_iterator it = positions.find( "NewName" );
		Check( it != positions.end(), "5a: new key is present" );
		if( it != positions.end() ) { CheckDoubleEq( it->second.x, 3.0, "5a: position carries over (x)" ); CheckDoubleEq( it->second.y, 4.0, "5a: position carries over (y)" ); }
	}

	// Nothing to migrate: oldName absent.
	{
		GraphLayout::Positions positions;
		positions["Unrelated"] = GraphLayoutPoint{ 9.0, 9.0 };
		const bool migrated = GraphLayoutSidecar::MigrateName( positions, "NeverSaved", "AlsoNeverSaved" );
		Check( !migrated, "5b: MigrateName reports false when oldName has no entry" );
		Check( positions.size() == 1 && positions.find( "Unrelated" ) != positions.end(),
		       "5b: positions map is untouched" );
	}

	// Refuse to clobber: newName already occupied.
	{
		GraphLayout::Positions positions;
		positions["Old"] = GraphLayoutPoint{ 1.0, 1.0 };
		positions["New"] = GraphLayoutPoint{ 2.0, 2.0 };   // some unrelated stale entry already at "New"
		const bool migrated = GraphLayoutSidecar::MigrateName( positions, "Old", "New" );
		Check( !migrated, "5c: MigrateName refuses when newName already has an entry" );
		const GraphLayout::Positions::const_iterator oldIt = positions.find( "Old" );
		const GraphLayout::Positions::const_iterator newIt = positions.find( "New" );
		Check( oldIt != positions.end() && newIt != positions.end(), "5c: BOTH entries remain, neither clobbered" );
		if( oldIt != positions.end() ) CheckDoubleEq( oldIt->second.x, 1.0, "5c: Old's own value untouched" );
		if( newIt != positions.end() ) CheckDoubleEq( newIt->second.x, 2.0, "5c: New's own (unrelated) value untouched" );
	}
}

// =======================================================================
// PART 6 -- unsaved scene never writes
// =======================================================================

static void TestUnsavedSceneNeverWrites()
{
	std::cout << "PART 6: an empty scenePath (unsaved/in-memory-only scene) never writes" << std::endl;
	Check( GraphLayoutSidecar::SidecarPathForScene( "" ).empty(), "6: sidecar path for an empty scenePath is empty" );

	GraphLayout::Positions positions;
	positions["X"] = GraphLayoutPoint{ 1.0, 1.0 };
	std::set<std::string> live;
	live.insert( "X" );
	std::string err = "untouched-sentinel";
	const bool wrote = GraphLayoutSidecar::WriteSidecar( "", positions, live, err );
	Check( wrote, "6: WriteSidecar with an empty scenePath returns true (nothing to do, not an error)" );
	Check( err.empty(), "6: outError is cleared, not left at a stale value" );
}

// =======================================================================
// PART 7 -- composition: MigrateName then WriteSidecar (rename-then-save)
// =======================================================================

static void TestMigrateThenWriteRoundTrips()
{
	std::cout << "PART 7: MigrateName then WriteSidecar -- rename-then-save composition" << std::endl;
	const fs::path dir = TestDir();
	const std::string scenePath = ( dir / "scene.RISEscene" ).string();

	// Seed the sidecar under the OLD name.
	GraphLayout::Positions positions;
	positions["OldName"] = GraphLayoutPoint{ 42.0, -7.0 };
	std::set<std::string> liveOld;
	liveOld.insert( "OldName" );
	std::string err;
	Check( GraphLayoutSidecar::WriteSidecar( scenePath, positions, liveOld, err ), "7: initial write under OldName succeeds" );

	// Rename in memory -- mirrors the caller contract documented on
	// MigrateName itself: called once, immediately after a successful
	// CST rename, with the chunk's pre-rename name as oldName.
	const bool migrated = GraphLayoutSidecar::MigrateName( positions, "OldName", "NewName" );
	Check( migrated, "7: MigrateName succeeds" );

	// Save again with a live-name set reflecting the RENAMED graph:
	// NewName is live, OldName is gone -- exactly what the canvas bridge
	// would pass after a real rename lands.
	std::set<std::string> liveNew;
	liveNew.insert( "NewName" );
	Check( GraphLayoutSidecar::WriteSidecar( scenePath, positions, liveNew, err ), "7: post-rename write succeeds" );

	const GraphLayout::Positions readBack = GraphLayoutSidecar::ReadSidecar( scenePath );
	Check( readBack.size() == 1, "7: exactly one entry survives the rename-then-save" );
	Check( readBack.find( "NewName" ) != readBack.end(), "7: the migrated entry survives under its new name" );
	Check( readBack.find( "OldName" ) == readBack.end(), "7: no orphan remains under the old name" );
	const GraphLayout::Positions::const_iterator it = readBack.find( "NewName" );
	if( it != readBack.end() ) {
		CheckDoubleEq( it->second.x, 42.0, "7: x survives the rename-then-save round-trip" );
		CheckDoubleEq( it->second.y, -7.0, "7: y survives the rename-then-save round-trip" );
	}
}

// =======================================================================
// PART 8 -- MigrateSidecarOnSaveAs (doc-88 S14 review round P2(2))
// =======================================================================

static void TestMigrateSidecarOnSaveAsCopies()
{
	std::cout << "PART 8a: MigrateSidecarOnSaveAs copies the old sidecar to the new path" << std::endl;
	const fs::path dir = TestDir();
	const std::string oldScenePath = ( dir / "old.RISEscene" ).string();
	const std::string newScenePath = ( dir / "new.RISEscene" ).string();

	GraphLayout::Positions positions;
	positions["A"] = GraphLayoutPoint{ 1.0, 2.0 };
	positions["B"] = GraphLayoutPoint{ 3.0, 4.0 };
	std::set<std::string> live; live.insert( "A" ); live.insert( "B" );
	std::string err;
	Check( GraphLayoutSidecar::WriteSidecar( oldScenePath, positions, live, err ), "8a: old sidecar seeded" );

	std::string migErr;
	Check( GraphLayoutSidecar::MigrateSidecarOnSaveAs( oldScenePath, newScenePath, migErr ), "8a: migration succeeds" );
	Check( migErr.empty(), "8a: no error message on success" );

	const GraphLayout::Positions newSide = GraphLayoutSidecar::ReadSidecar( newScenePath );
	Check( newSide.size() == 2, "8a: the new sidecar has both entries" );
	const GraphLayout::Positions::const_iterator itA = newSide.find( "A" );
	Check( itA != newSide.end(), "8a: A migrated" );
	if( itA != newSide.end() ) {
		CheckDoubleEq( itA->second.x, 1.0, "8a: A.x migrated verbatim" );
		CheckDoubleEq( itA->second.y, 2.0, "8a: A.y migrated verbatim" );
	}

	const GraphLayout::Positions oldSide = GraphLayoutSidecar::ReadSidecar( oldScenePath );
	Check( oldSide.size() == 2, "8a: the OLD sidecar is left in place, untouched" );
}

static void TestMigrateSidecarOnSaveAsNoOpCases()
{
	std::cout << "PART 8b: MigrateSidecarOnSaveAs no-ops on empty/equal paths and a missing old sidecar" << std::endl;
	const fs::path dir = TestDir();
	const std::string oldScenePath = ( dir / "old.RISEscene" ).string();
	const std::string newScenePath = ( dir / "new.RISEscene" ).string();

	std::string err;
	Check( GraphLayoutSidecar::MigrateSidecarOnSaveAs( "", newScenePath, err ), "8b: empty oldScenePath no-ops (true)" );
	Check( GraphLayoutSidecar::MigrateSidecarOnSaveAs( oldScenePath, "", err ), "8b: empty newScenePath no-ops (true)" );
	Check( GraphLayoutSidecar::MigrateSidecarOnSaveAs( oldScenePath, oldScenePath, err ), "8b: equal paths (ordinary save, not Save-As) no-ops (true)" );

	// No old sidecar on disk at all -- ordinary "this scene never had one".
	Check( GraphLayoutSidecar::MigrateSidecarOnSaveAs( oldScenePath, newScenePath, err ), "8b: missing old sidecar no-ops (true)" );
	Check( !fs::exists( GraphLayoutSidecar::SidecarPathForScene( newScenePath ) ),
	       "8b: no new sidecar was created when there was nothing to migrate" );
}

static void TestMigrateSidecarOnSaveAsNeverOverwrites()
{
	std::cout << "PART 8c: MigrateSidecarOnSaveAs never overwrites an existing new-path sidecar" << std::endl;
	const fs::path dir = TestDir();
	const std::string oldScenePath = ( dir / "old.RISEscene" ).string();
	const std::string newScenePath = ( dir / "new.RISEscene" ).string();

	GraphLayout::Positions oldPositions;
	oldPositions["FromOld"] = GraphLayoutPoint{ 10.0, 20.0 };
	std::set<std::string> liveOld; liveOld.insert( "FromOld" );
	std::string err;
	Check( GraphLayoutSidecar::WriteSidecar( oldScenePath, oldPositions, liveOld, err ), "8c: old sidecar seeded" );

	GraphLayout::Positions decoy;
	decoy["FromNew"] = GraphLayoutPoint{ -1.0, -1.0 };
	std::set<std::string> liveNew; liveNew.insert( "FromNew" );
	Check( GraphLayoutSidecar::WriteSidecar( newScenePath, decoy, liveNew, err ), "8c: decoy sidecar already sits at the destination" );

	std::string migErr;
	Check( GraphLayoutSidecar::MigrateSidecarOnSaveAs( oldScenePath, newScenePath, migErr ),
	       "8c: migration returns true (never an error) when it refuses to overwrite" );

	const GraphLayout::Positions afterNew = GraphLayoutSidecar::ReadSidecar( newScenePath );
	Check( afterNew.size() == 1 && afterNew.find( "FromNew" ) != afterNew.end() && afterNew.find( "FromOld" ) == afterNew.end(),
	       "8c: the destination's own (decoy) sidecar is untouched, not merged or replaced" );
}

int main()
{
	std::cout << "=== GraphLayoutSidecarTest ===" << std::endl;

	TestSerializeParseRoundTrip();
	TestSerializeIsDeterministic();

	TestMissingSidecarIsEmpty();
	TestWriteThenReadRoundTrip();
	TestWriteOnlyWhenChanged();

	TestMalformedSidecarIsEmptyNeverFatal();
	TestUnknownKeysIgnored();
	TestAbsentNodesKeyIsEmptyNotMalformed();
	TestNonFiniteAndAbsurdCoordinatesSanitized();
	TestWrongTypedCoordinateSanitized();
	TestOneMalformedNodeDoesNotCostOthers();

	TestOrphanPruneOnWrite();

	TestMigrateName();

	TestUnsavedSceneNeverWrites();

	TestMigrateThenWriteRoundTrips();

	TestMigrateSidecarOnSaveAsCopies();
	TestMigrateSidecarOnSaveAsNoOpCases();
	TestMigrateSidecarOnSaveAsNeverOverwrites();

	std::cout << "\n=== " << passCount << " passed, " << failCount << " failed ===" << std::endl;
	return failCount == 0 ? 0 : 1;
}
