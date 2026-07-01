//////////////////////////////////////////////////////////////////////
//
//  AgentReadValidateTest.cpp - Facet 5 (agentic surface) slice 0a.
//
//  Proves the FIRST code of the agentic surface end-to-end: an
//  AgentSession over a CST-loaded Job exposing the three read/validate
//  verbs (docs/agentic-redesign/50-agentic-surface.md §2.2.1 / §2.2.4):
//
//    * ReadDocument()  -> the canonical .RISEscene text of the head
//                         (round-trips: ParseToCst(ReadDocument()) is the
//                         same document).
//    * ReadSchema(kw)  -> descriptor-generated JSON (contains the chunk's
//                         params); an unknown keyword is handled gracefully.
//    * Validate(text)  -> zero Error diagnostics on a good scene; an
//                         UNKNOWN_PARAMETER diagnostic (localized to the
//                         offending token) on a bad one -- and NO mutation
//                         of the session's Job (ReadDocument unchanged).
//
//  The localization is RED-PROVEN: the good scene yields no such diag, the
//  bad scene does, and its byte offset lands on the `bogus` token.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentDiagnostic.h"
#include "../src/Library/Agent/SchemaGen.h"
#include "../src/Library/Cst/Cst.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace RISE;
using namespace RISE::Agent;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const std::string& w )
{
	if( c ) ++g_pass;
	else { ++g_fail; std::printf( "  FAIL: %s\n", w.c_str() ); }
}

// A small, self-contained native-v7 scene that derives cleanly.
static const char* const kGoodScene =
	"RISE ASCII SCENE 7\n"
	"sphere_geometry\n"
	"{\n"
	"\tname\t\t\ts\n"
	"\tradius\t\t\t0.6\n"
	"}\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname\t\t\tp\n"
	"\tcolor\t\t\t1 1 1\n"
	"}\n";

// The bad scene: the sphere carries an UNDECLARED parameter `bogus`.
static const std::string kBadScene =
	std::string( "RISE ASCII SCENE 7\n" ) +
	"sphere_geometry\n"
	"{\n"
	"\tname s\n"
	"\tradius 1\n"
	"\tbogus 5\n"
	"}\n";

// Write `text` to a temp file and return its path (or "" on failure).
static std::string WriteTemp( const char* name, const std::string& text )
{
	const char* base = std::getenv( "TMPDIR" );
	std::string dir = base ? base : "/tmp";
	if( !dir.empty() && dir.back() != '/' ) dir += '/';
	std::string path = dir + name;
	std::ofstream f( path.c_str(), std::ios::binary );
	if( !f ) return std::string();
	f.write( text.data(), (std::streamsize)text.size() );
	f.close();
	return path;
}

int main()
{
	std::printf( "=== AgentReadValidateTest (Facet 5 slice 0a: read + schema + validate) ===\n" );

	const std::string goodPath = WriteTemp( "rise_agent_slice0a_good.RISEscene", kGoodScene );
	Check( !goodPath.empty(), "wrote the good scene to a temp file" );

	std::unique_ptr<AgentSession> session = AgentSession::LoadFromFile( goodPath );
	Check( session != nullptr, "AgentSession::LoadFromFile loads the native-v7 scene" );
	if( !session ) { std::printf( "cannot continue without a session\n" ); return 1; }

	//----------------------------------------------------------------------
	// ReadDocument -- the canonical text + round-trip.
	//----------------------------------------------------------------------
	std::printf( "[read_document] canonical text + round-trip\n" );
	Check( session->HasDocument(), "session retains a CST Document" );
	const std::string doc = session->ReadDocument();
	Check( doc.find( "sphere_geometry" ) != std::string::npos,
	       "ReadDocument contains the expected chunk keyword (sphere_geometry)" );
	// Round-trips: re-parsing the head text serializes back byte-identically.
	{
		Cst::Document reparsed = Cst::ParseToCst( doc );
		Check( Cst::SerializeCst( reparsed ) == doc,
		       "ParseToCst(ReadDocument()) round-trips byte-identically" );
	}

	//----------------------------------------------------------------------
	// ReadSchema -- descriptor JSON.
	//----------------------------------------------------------------------
	std::printf( "[read_schema] descriptor-generated JSON\n" );
	const std::string schema = session->ReadSchema( "sphere_geometry" );
	Check( schema.find( "\"radius\"" ) != std::string::npos,
	       "ReadSchema(sphere_geometry) contains the radius parameter" );
	Check( schema.find( "\"type\"" ) != std::string::npos,
	       "ReadSchema(sphere_geometry) carries a param type" );
	// Balanced braces (a minimal validity check on the emitted JSON).
	{
		int depth = 0; bool balanced = true;
		for( char c : schema ) {
			if( c == '{' ) ++depth;
			else if( c == '}' ) { if( --depth < 0 ) { balanced = false; break; } }
		}
		Check( balanced && depth == 0, "ReadSchema(sphere_geometry) has balanced braces" );
	}
	// Unknown keyword handled gracefully (an error object, never a crash).
	{
		const std::string bad = session->ReadSchema( "not_a_chunk" );
		Check( bad.find( "\"error\"" ) != std::string::npos,
		       "ReadSchema(not_a_chunk) returns an error object, gracefully" );
	}
	// The whole-grammar schema is non-empty + balanced.
	{
		const std::string all = session->ReadSchema();
		Check( all.find( "sphere_geometry" ) != std::string::npos,
		       "ReadSchema() (whole grammar) enumerates chunk keywords" );
	}

	//----------------------------------------------------------------------
	// Validate -- the keystone.
	//----------------------------------------------------------------------
	std::printf( "[validate] good scene has zero Error diagnostics\n" );
	{
		std::vector<AgentDiagnostic> diags = session->Validate( kGoodScene );
		int errors = 0;
		for( const AgentDiagnostic& d : diags )
			if( d.severity == AgentDiagnostic::Severity::Error ) ++errors;
		Check( errors == 0, "Validate(goodText) yields zero Error diagnostics" );
	}

	std::printf( "[validate] bad scene -> localized UNKNOWN_PARAMETER\n" );
	bool sawUnknownParam = false;
	{
		std::vector<AgentDiagnostic> diags = session->Validate( kBadScene );
		// The `bogus` token's byte span in the bad scene text.
		const std::size_t bogusPos = kBadScene.find( "bogus" );
		Check( bogusPos != std::string::npos, "test fixture actually contains `bogus`" );

		for( const AgentDiagnostic& d : diags ) {
			if( d.code == AgentDiagnosticCode::UNKNOWN_PARAMETER ) {
				sawUnknownParam = true;
				// The offset should land ON the bogus token (best-effort
				// localization: assert it points exactly at `bogus`).
				Check( d.offset == bogusPos,
				       "UNKNOWN_PARAMETER offset lands exactly on the `bogus` token" );
				Check( d.length == 5, "UNKNOWN_PARAMETER length spans `bogus` (5 bytes)" );
				// A firmer invariant: the span must be inside the text and
				// its bytes must be `bogus`.
				Check( d.offset < kBadScene.size() &&
				       kBadScene.compare( d.offset, 5, "bogus" ) == 0,
				       "the localized span's bytes ARE `bogus`" );
			}
		}
		Check( sawUnknownParam, "Validate(badText) reports an UNKNOWN_PARAMETER diagnostic" );
	}

	//----------------------------------------------------------------------
	// RED-PROVE: the localization is REAL -- the good scene has NO such diag.
	//----------------------------------------------------------------------
	std::printf( "[validate] red-prove: good scene has NO UNKNOWN_PARAMETER\n" );
	{
		std::vector<AgentDiagnostic> diags = session->Validate( kGoodScene );
		bool anyUnknown = false;
		for( const AgentDiagnostic& d : diags )
			if( d.code == AgentDiagnosticCode::UNKNOWN_PARAMETER ) anyUnknown = true;
		Check( !anyUnknown, "the good scene yields NO UNKNOWN_PARAMETER (localization is real)" );
	}

	//----------------------------------------------------------------------
	// Validate has NO side effects on the session's Job.
	//----------------------------------------------------------------------
	std::printf( "[validate] no mutation of the session head\n" );
	{
		const std::string before = session->ReadDocument();
		session->Validate( kBadScene );      // derive into a THROWAWAY Job
		session->Validate( kGoodScene );
		const std::string after = session->ReadDocument();
		Check( before == after, "ReadDocument() is unchanged after validating candidate text" );
	}

	std::printf( "=== AgentReadValidateTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
