//////////////////////////////////////////////////////////////////////
//
//  CstCameraEraseTest.cpp - Model-B P5 (Phase-4 RemoveCamera): the
//    trivia-preserving GENERAL camera-chunk erase (Cst::DocEraseChunkTidy
//    + Job::ApplyCstDeleteCameraChunk).  This is the SAFE removal for
//    FILE-AUTHORED cameras -- the counterpart to the CLONE-UNDO-ONLY
//    Job::ApplyCstRemoveCameraChunk, which drops the item at idx-1
//    UNCONDITIONALLY and GLUES the previous chunk onto the next
//    (`}pinhole_camera` on one line) when used on a hand-written camera.
//
//    All cameras below are authored via a SCENE STRING with normal author
//    spacing (blank line between chunks) -- NOT clone-inserted -- so the
//    separator between chunks is the file's own "\n\n" trivia leaf, and
//    the clone-undo idx-1 unconditional drop would be a LANDMINE.
//
//    Cases:
//      CDEL-A -- delete the FIRST chunk (camera is the first top-level chunk).
//      CDEL-B -- delete a MIDDLE chunk (camera between two other chunks).
//      CDEL-C -- delete the LAST chunk (camera is the last chunk).
//      CDEL-D -- NO-TRAILING-NEWLINE doc: delete a camera, assert no glue.
//      CDEL-E -- TWO named cameras: delete one, the other survives (+ reloads).
//
//    Each asserts: (1) NO `}<keyword>` glue in the serialized Document;
//    (2) the removed camera is GONE (DocFindByNameAnyRole == 0);
//    (3) the OTHER chunks are intact and the Document RELOADS via
//    LoadAsciiSceneViaCst to a valid scene; (4) minimality -- no NET gain of
//    blank-line separators vs the same scene authored without that camera.
//
//    RED-PROVE: temporarily make DocEraseChunkTidy also drop the item at
//    index-1 unconditionally (the clone-undo behavior) -> the MIDDLE / FIRST
//    glue assertion fails with a `}<keyword>` glue in the serialized text.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <string>
#include <fstream>
#include <cstdio>

#include "../src/Library/Job.h"
#include "../src/Library/Cst/Cst.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0, failCount = 0;
static void Check( bool c, const char* n ) { if( c ) ++passCount; else { ++failCount; std::cout << "  FAIL: " << n << std::endl; } }

// The serialized retained Document as a string ("" if none).
static std::string DocText( Job& j )
{
	const RISE::Cst::Document* d = j.GetCstDocument();
	return d ? RISE::Cst::SerializeCst( *d ) : std::string();
}

// Well-formedness scan: TRUE iff NOWHERE a `}` is immediately followed by a
// non-whitespace byte -- i.e. no `}<keyword>` glue (`}pinhole_camera`, `}film`,
// `}sphere_geometry`).  A well-formed serialized scene always has `}` end a line.
static bool NoRBraceGlue( const std::string& s )
{
	for( std::string::size_type p = s.find( '}' ); p != std::string::npos; p = s.find( '}', p + 1 ) ) {
		const std::string::size_type q = p + 1;
		if( q < s.size() ) {
			const char c = s[q];
			if( c != '\n' && c != '\r' && c != ' ' && c != '\t' && c != '}' ) return false;
		}
	}
	return true;
}

// Does the retained Document still carry a camera of this name?  (0 = absent.)
static bool HasCamera( Job& j, const char* name )
{
	const RISE::Cst::Document* d = j.GetCstDocument();
	if( !d ) return false;
	return RISE::Cst::DocFindByNameAnyRole( *d, name, nullptr, "camera", false ) != 0;
}

// Count NON-overlapping occurrences of `needle` in `hay`.
static int CountOcc( const std::string& hay, const std::string& needle )
{
	if( needle.empty() ) return 0;
	int n = 0;
	for( std::string::size_type p = hay.find( needle ); p != std::string::npos; p = hay.find( needle, p + needle.size() ) ) ++n;
	return n;
}

// Load a scene STRING via the CST path (writes it to `path`, loads into a fresh Job).
static Job* LoadString( const char* path, const std::string& scene )
{
	{ std::ofstream o( path ); o << scene; }
	Job* j = new Job();
	if( !j->LoadAsciiSceneViaCst( path ) ) { j->release(); return 0; }
	return j;
}

int main()
{
	std::cout << "CstCameraEraseTest" << std::endl;
	const char* tf = "cst_camerase.RISEscene";

	// ---- CDEL-A: delete the FIRST chunk (camera first, before film + object) ----
	{
		const std::string scene =
			"RISE ASCII SCENE 6\n"
			"\n"
			"pinhole_camera\n{\n\tname camA\n\tlocation 0 0 3\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30\n}\n"
			"\n"
			"film\n{\n\twidth 64\n\theight 64\n}\n"
			"\n"
			"sphere_geometry\n{\n\tname s\n\tradius 1\n}\n";
		Job* j = LoadString( tf, scene );
		Check( j != 0, "CDEL-A: loads FIRST-camera scene via CST" );
		if( j ) {
			Check( HasCamera( *j, "camA" ), "CDEL-A: camA present before delete" );
			Check( j->ApplyCstDeleteCameraChunk( "camA" ) == 1, "CDEL-A: ApplyCstDeleteCameraChunk(camA) returns 1" );
			const std::string s = DocText( *j );
			Check( NoRBraceGlue( s ), "CDEL-A: NO `}<keyword>` glue after deleting the first chunk" );
			Check( !HasCamera( *j, "camA" ), "CDEL-A: camA is GONE from the Document" );
			Check( CountOcc( s, "pinhole_camera" ) == 0, "CDEL-A: no pinhole_camera chunk remains" );
			Check( CountOcc( s, "film {" ) == 1 || CountOcc( s, "film\n{" ) == 1, "CDEL-A: film chunk intact" );
			Check( CountOcc( s, "sphere_geometry" ) == 1, "CDEL-A: sphere_geometry chunk intact" );
			// Reload: the pruned Document is a valid loadable scene.
			{ std::ofstream o( tf ); o << s; }
			Job* jr = new Job();
			Check( jr->LoadAsciiSceneViaCst( tf ), "CDEL-A: the pruned Document RELOADS via CST" );
			jr->release();
			j->release();
		}
	}

	// ---- CDEL-B: delete a MIDDLE chunk (camera between film and object) ----
	{
		const std::string scene =
			"RISE ASCII SCENE 6\n"
			"\n"
			"film\n{\n\twidth 64\n\theight 64\n}\n"
			"\n"
			"pinhole_camera\n{\n\tname camB\n\tlocation 0 0 3\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30\n}\n"
			"\n"
			"sphere_geometry\n{\n\tname s\n\tradius 1\n}\n";
		Job* j = LoadString( tf, scene );
		Check( j != 0, "CDEL-B: loads MIDDLE-camera scene via CST" );
		if( j ) {
			// Minimality reference: the SAME scene authored WITHOUT camB (one separator, no blank-line accumulation).
			const std::string sceneNoCam =
				"RISE ASCII SCENE 6\n"
				"\n"
				"film\n{\n\twidth 64\n\theight 64\n}\n"
				"\n"
				"sphere_geometry\n{\n\tname s\n\tradius 1\n}\n";

			Check( HasCamera( *j, "camB" ), "CDEL-B: camB present before delete" );
			Check( j->ApplyCstDeleteCameraChunk( "camB" ) == 1, "CDEL-B: ApplyCstDeleteCameraChunk(camB) returns 1" );
			const std::string s = DocText( *j );
			Check( NoRBraceGlue( s ), "CDEL-B: NO `}<keyword>` glue after deleting the MIDDLE chunk" );
			Check( !HasCamera( *j, "camB" ), "CDEL-B: camB is GONE from the Document" );
			Check( CountOcc( s, "pinhole_camera" ) == 0, "CDEL-B: no pinhole_camera chunk remains" );
			Check( CountOcc( s, "film" ) == 1 && CountOcc( s, "sphere_geometry" ) == 1, "CDEL-B: film + sphere chunks intact" );
			// Minimality: the pruned text is BYTE-IDENTICAL to the same scene authored without camB
			// (exactly one "\n\n" separator between film and sphere -- NO extra blank line accumulated).
			Check( s == sceneNoCam, "CDEL-B: pruned text == scene-authored-without-camB (minimal, exactly one separator)" );
			{ std::ofstream o( tf ); o << s; }
			Job* jr = new Job();
			Check( jr->LoadAsciiSceneViaCst( tf ), "CDEL-B: the pruned Document RELOADS via CST" );
			jr->release();
			j->release();
		}
	}

	// ---- CDEL-C: delete the LAST chunk (camera is the last chunk) ----
	{
		const std::string scene =
			"RISE ASCII SCENE 6\n"
			"\n"
			"film\n{\n\twidth 64\n\theight 64\n}\n"
			"\n"
			"sphere_geometry\n{\n\tname s\n\tradius 1\n}\n"
			"\n"
			"pinhole_camera\n{\n\tname camC\n\tlocation 0 0 3\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30\n}\n";
		Job* j = LoadString( tf, scene );
		Check( j != 0, "CDEL-C: loads LAST-camera scene via CST" );
		if( j ) {
			Check( HasCamera( *j, "camC" ), "CDEL-C: camC present before delete" );
			Check( j->ApplyCstDeleteCameraChunk( "camC" ) == 1, "CDEL-C: ApplyCstDeleteCameraChunk(camC) returns 1" );
			const std::string s = DocText( *j );
			Check( NoRBraceGlue( s ), "CDEL-C: NO `}<keyword>` glue after deleting the LAST chunk" );
			Check( !HasCamera( *j, "camC" ), "CDEL-C: camC is GONE from the Document" );
			Check( CountOcc( s, "pinhole_camera" ) == 0, "CDEL-C: no pinhole_camera chunk remains" );
			Check( CountOcc( s, "film" ) == 1 && CountOcc( s, "sphere_geometry" ) == 1, "CDEL-C: film + sphere chunks intact" );
			// The document keeps its final newline (the LAST-chunk collapse tidies nothing).
			Check( !s.empty() && s.back() == '\n', "CDEL-C: the pruned Document keeps its final newline" );
			{ std::ofstream o( tf ); o << s; }
			Job* jr = new Job();
			Check( jr->LoadAsciiSceneViaCst( tf ), "CDEL-C: the pruned Document RELOADS via CST" );
			jr->release();
			j->release();
		}
	}

	// ---- CDEL-D: NO-TRAILING-NEWLINE doc: delete a camera, assert no glue ----
	{
		const std::string scene =
			"RISE ASCII SCENE 6\n"
			"\n"
			"film\n{\n\twidth 64\n\theight 64\n}\n"
			"\n"
			"pinhole_camera\n{\n\tname camD\n\tfov 30\n}\n"
			"\n"
			"sphere_geometry\n{\n\tname s\n\tradius 1\n}";   // NO final newline
		Job* j = LoadString( tf, scene );
		Check( j != 0, "CDEL-D: loads no-trailing-newline scene via CST" );
		if( j ) {
			Check( j->ApplyCstDeleteCameraChunk( "camD" ) == 1, "CDEL-D: ApplyCstDeleteCameraChunk(camD) returns 1" );
			const std::string s = DocText( *j );
			Check( NoRBraceGlue( s ), "CDEL-D: NO `}<keyword>` glue on a no-trailing-newline doc" );
			Check( !HasCamera( *j, "camD" ), "CDEL-D: camD is GONE from the Document" );
			Check( CountOcc( s, "film" ) == 1 && CountOcc( s, "sphere_geometry" ) == 1, "CDEL-D: film + sphere chunks intact" );
			{ std::ofstream o( tf ); o << s; }
			Job* jr = new Job();
			Check( jr->LoadAsciiSceneViaCst( tf ), "CDEL-D: the pruned Document RELOADS via CST" );
			jr->release();
			j->release();
		}
	}

	// ---- CDEL-E: TWO named cameras: delete one, the other survives ----
	{
		const std::string scene =
			"RISE ASCII SCENE 6\n"
			"\n"
			"film\n{\n\twidth 64\n\theight 64\n}\n"
			"\n"
			"pinhole_camera\n{\n\tname camX\n\tfov 30\n}\n"
			"\n"
			"pinhole_camera\n{\n\tname camY\n\tfov 45\n}\n"
			"\n"
			"sphere_geometry\n{\n\tname s\n\tradius 1\n}\n";
		Job* j = LoadString( tf, scene );
		Check( j != 0, "CDEL-E: loads two-camera scene via CST" );
		if( j ) {
			Check( HasCamera( *j, "camX" ) && HasCamera( *j, "camY" ), "CDEL-E: both camX and camY present before delete" );
			Check( j->ApplyCstDeleteCameraChunk( "camX" ) == 1, "CDEL-E: ApplyCstDeleteCameraChunk(camX) returns 1" );
			const std::string s = DocText( *j );
			Check( NoRBraceGlue( s ), "CDEL-E: NO `}<keyword>` glue after deleting camX" );
			Check( !HasCamera( *j, "camX" ), "CDEL-E: camX is GONE" );
			Check( HasCamera( *j, "camY" ), "CDEL-E: camY SURVIVES" );
			Check( CountOcc( s, "pinhole_camera" ) == 1, "CDEL-E: exactly one camera chunk remains" );
			Check( s.find( "camY" ) != std::string::npos, "CDEL-E: camY's chunk text is intact" );
			{ std::ofstream o( tf ); o << s; }
			Job* jr = new Job();
			Check( jr->LoadAsciiSceneViaCst( tf ), "CDEL-E: the pruned Document RELOADS via CST" );
			Check( RISE::Cst::DocFindByNameAnyRole( *jr->GetCstDocument(), "camY", nullptr, "camera", false ) != 0,
			       "CDEL-E: camY resolves in the reloaded Document" );
			jr->release();
			j->release();
		}
	}

	std::remove( tf );
	std::cout << passCount << " passed, " << failCount << " failed." << std::endl;
	return failCount == 0 ? 0 : 1;
}
