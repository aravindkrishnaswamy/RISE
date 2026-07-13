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
#include "../src/Library/Agent/Json.h"
#include "../src/Library/Cst/Cst.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
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

// The value-less scene: `radius` sits ALONE on its own line (no same-line
// value), which ParseChunk flattens into a bare pname Token (a direct child
// of the Chunk) and DeriveToJob reports as
// "sphere_geometry: value-less parameter 'radius'".  The localizer must land
// the diagnostic's byte span ON that bare `radius` token.
static const std::string kValuelessScene =
	std::string( "RISE ASCII SCENE 7\n" ) +
	"sphere_geometry\n"
	"{\n"
	"\tname s\n"
	"\tradius\n"
	"}\n";

// The value-HAVING twin of kValuelessScene (identical but for `radius 0.6`):
// derives cleanly and yields NO value-less diagnostic -- the red-prove that
// the value-less localization is REAL, not an artifact of the scene shape.
static const std::string kValuedScene =
	std::string( "RISE ASCII SCENE 7\n" ) +
	"sphere_geometry\n"
	"{\n"
	"\tname s\n"
	"\tradius 0.6\n"
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
	// RED-PROVEN bug (user-reported live-Gemini failure): SchemaGenAll()
	// used to iterate SceneGrammar::AllChunks() with NO dedupe.  A legacy
	// alias parser entry (e.g. `mis_pathtracing_shaderop`, registered in
	// ChunkParserRegistry.cpp's CreateAllChunkParsers with the SAME parser
	// class as `pathtracing_shaderop`) reports its Describe().keyword as
	// the CANONICAL keyword, so the whole-grammar dump emitted
	// "pathtracing_shaderop" TWICE.  Gemini's functionResponse.response
	// rides as a protobuf Struct, which hard-rejects duplicate map keys
	// -- so a bare `read_schema` call killed the whole live-Gemini chat
	// with an HTTP 400.  This must never regress: every top-level chunk
	// keyword appears in ReadSchema()'s output EXACTLY ONCE.
	{
		const std::string all = session->ReadSchema();

		// (a) Targeted raw-count check on the specific keyword the alias
		// affects.  A raw substring count is the right assertion here --
		// unlike JsonValue::find() (last-wins on lookup), the underlying
		// bug is that SERIALIZATION re-emits every stored pair, so a
		// naive "parse then look up" check would not have caught this.
		auto CountOccurrences = []( const std::string& hay, const std::string& needle ) {
			int n = 0;
			for( std::size_t pos = hay.find( needle ); pos != std::string::npos; pos = hay.find( needle, pos + 1 ) )
				++n;
			return n;
		};
		Check( CountOccurrences( all, "\"pathtracing_shaderop\":" ) == 1,
		       "ReadSchema() emits \"pathtracing_shaderop\" exactly once "
		       "(pre-fix this was 2, from the mis_pathtracing_shaderop alias)" );

		// (b) General invariant, not hardcoded to any one keyword: parse
		// the whole-grammar dump and confirm the top-level object's raw
		// member count (which PRESERVES duplicates -- see Json.h) equals
		// the number of DISTINCT keys.  This catches any future alias
		// that reintroduces the same class of bug under a different
		// keyword.
		JsonValue root;
		std::string perr;
		Check( JsonParse( all, root, perr ) && root.isObject(),
		       "ReadSchema() (whole grammar) parses as a JSON object" );
		if( root.isObject() ) {
			const std::vector<std::pair<std::string, JsonValue>>& mem = root.members();
			std::set<std::string> distinctKeys;
			for( const auto& kv : mem ) distinctKeys.insert( kv.first );
			Check( mem.size() == distinctKeys.size(),
			       "ReadSchema() (whole grammar) has no duplicate top-level keyword keys" );
		}
	}

	// Discovery-cost fix: SchemaGenCategory("<name>") is the CHEAP listing
	// mode -- just {keyword, description} per chunk in the category, NOT the
	// full per-parameter dump.  Proven on "material": it lists the material
	// kinds, is a valid {category, chunks[]} object, and is DRAMATICALLY
	// smaller than the whole-grammar dump (no "properties" parameter trees).
	{
		std::printf( "[read_schema] SchemaGenCategory cheap listing mode\n" );
		const std::string mats = RISE::Agent::SchemaGenCategory( "material" );

		JsonValue root;
		std::string perr;
		Check( JsonParse( mats, root, perr ) && root.isObject(),
		       "SchemaGenCategory(material) parses as a JSON object" );
		Check( root.isObject() && root.get( "category" ).asString() == "material",
		       "SchemaGenCategory(material) echoes the category" );
		Check( root.isObject() && root.get( "chunks" ).isArray() && root.get( "chunks" ).size() > 0,
		       "SchemaGenCategory(material) lists a non-empty chunks array" );
		Check( mats.find( "lambertian_material" ) != std::string::npos &&
		       mats.find( "pbr_metallic_roughness_material" ) != std::string::npos,
		       "SchemaGenCategory(material) enumerates the material keywords" );
		// The listing must NOT include the per-parameter schema (that is the
		// expensive dump it exists to AVOID) -- the whole-grammar/per-chunk
		// dumps emit a "properties" parameter tree; the cheap listing never
		// does (a word like "reflectance" may still appear inside a chunk's
		// one-line DESCRIPTION, so "properties" is the right marker).
		Check( mats.find( "\"properties\"" ) == std::string::npos,
		       "SchemaGenCategory(material) omits per-parameter schemas (cheap listing)" );
		// A one-line description rides each entry.
		Check( mats.find( "\"description\"" ) != std::string::npos,
		       "SchemaGenCategory(material) carries one-line descriptions" );
		// It is far smaller than the whole grammar.
		Check( mats.size() * 4 < session->ReadSchema().size(),
		       "SchemaGenCategory(material) is much smaller than the whole-grammar dump" );

		// A geometry category also resolves (not hardcoded to material).
		const std::string geo = RISE::Agent::SchemaGenCategory( "geometry" );
		Check( geo.find( "sphere_geometry" ) != std::string::npos,
		       "SchemaGenCategory(geometry) lists sphere_geometry" );

		// An unknown/empty category fails LOUDLY with an "error" key + empty list.
		const std::string bad = RISE::Agent::SchemaGenCategory( "not_a_category" );
		Check( bad.find( "\"error\"" ) != std::string::npos,
		       "SchemaGenCategory(unknown) carries an error key" );
		Check( bad.find( "\"chunks\":[]" ) != std::string::npos,
		       "SchemaGenCategory(unknown) has an empty chunks array" );
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
	// Validate -- value-less parameter is localized to the BARE pname token.
	// (Fix 1: a value-less line flattens into a bare pname Token that is a
	// direct child of the Chunk, which OffsetOfParamName cannot see; the
	// value-less path now scans the chunk's direct kids for it.)
	//----------------------------------------------------------------------
	std::printf( "[validate] value-less param -> localized INVALID_VALUE\n" );
	bool sawValueless = false;
	{
		std::vector<AgentDiagnostic> diags = session->Validate( kValuelessScene );
		// The lone `radius` token's byte span in the value-less scene text.
		// Anchor on the tab-prefixed line so we find the bare occurrence, not
		// the substring inside some other token.
		const std::size_t radiusLine = kValuelessScene.find( "\tradius\n" );
		Check( radiusLine != std::string::npos, "test fixture actually has a value-less `radius` line" );
		const std::size_t radiusPos = radiusLine + 1;   // skip the leading tab

		for( const AgentDiagnostic& d : diags ) {
			if( d.code == AgentDiagnosticCode::INVALID_VALUE &&
			    d.message.find( "value-less parameter" ) != std::string::npos ) {
				sawValueless = true;
				// The offset must land ON the bare `radius` token -- NOT 0/0.
				Check( d.offset != 0 || d.length != 0,
				       "value-less INVALID_VALUE is localized (not 0/0)" );
				Check( d.offset == radiusPos,
				       "value-less offset lands exactly on the bare `radius` token" );
				Check( d.length == 6, "value-less length spans `radius` (6 bytes)" );
				// A firmer invariant: the span's bytes ARE `radius`.
				Check( d.offset < kValuelessScene.size() &&
				       kValuelessScene.compare( d.offset, 6, "radius" ) == 0,
				       "the localized span's bytes ARE `radius`" );
			}
		}
		Check( sawValueless, "Validate(valuelessText) reports a value-less INVALID_VALUE diagnostic" );
	}

	// RED-PROVE: the value-HAVING twin yields NO value-less diagnostic.
	std::printf( "[validate] red-prove: value-having twin has NO value-less diag\n" );
	{
		std::vector<AgentDiagnostic> diags = session->Validate( kValuedScene );
		bool anyValueless = false;
		for( const AgentDiagnostic& d : diags )
			if( d.message.find( "value-less parameter" ) != std::string::npos ) anyValueless = true;
		Check( !anyValueless, "the value-having twin yields NO value-less diagnostic (localization is real)" );
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
