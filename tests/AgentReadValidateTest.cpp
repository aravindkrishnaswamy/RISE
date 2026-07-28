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
#include "../src/Library/Agent/AgentRpc.h"
#include "../src/Library/Agent/SchemaGen.h"
#include "../src/Library/Agent/Json.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Job.h"   // Job::ApplyCstParamEdit -- the wedged-head fixture

#include <algorithm>   // std::sort -- order-independent diagnostics fingerprint
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>
// Round-8 review P2: per-process temp filenames -- see WriteTemp below.
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>			// getpid()
#endif

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

// The WEDGE fixture: derivable as written, but `mat_diffuse` can be
// retargeted to `pnt_emit` -- a painter declared LATER in the document.
// Job::ApplyCstParamEdit (the UNCHECKED GUI fast path) commits that against
// the LIVE managers, leaving a head whose bytes no longer derive in
// document ORDER.  That is the only way to author a head that validates
// with diagnostics, and it is what makes the no-arg validate assertion
// discriminating rather than vacuous.  Shape borrowed from
// AgentChunkCrudTest's kScene, whose G1 case red-proves the wedge.
static const char* const kWedgeScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n\n"
	"standard_object\n{\n\tname obj_sph\n\tgeometry sph\n\tmaterial mat_diffuse\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 30.0\n\tmaterial none\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_emit\n\tpta -0.6 0.6 3.5\n\tptb 0.6 0.6 3.5\n\tptc 0.6 -0.6 3.5\n\tptd -0.6 -0.6 3.5\n}\n\n"
	"standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}\n";

// Write `text` to a temp file and return its path (or "" on failure).
static std::string WriteTemp( const char* name, const std::string& text )
{
	const char* base = std::getenv( "TMPDIR" );
	std::string dir = base ? base : "/tmp";
	if( !dir.empty() && dir.back() != '/' ) dir += '/';
	// Round-8 review P2, reason CORRECTED in round 10: per-process filename.
	// The round-8 comment justified this by asserting that run_all_tests.sh
	// runs the suite in PARALLEL.  IT DOES NOT -- Phase 3 is a plain
	// sequential `for` loop that waits on each binary before starting the
	// next (only the BUILD phases pass -j), and run_all_tests.ps1 is
	// likewise sequential for execution.  The real justification is that
	// nothing stops two copies of THIS binary from running at once: a
	// developer runs it by hand while the suite runs, a stray earlier run
	// has not exited yet, or a repeat-run loop (`for i in $(seq 8); do
	// ./bin/tests/<name> & done` -- the usual way to chase a suspected
	// flake) launches several at once.  With a FIXED temp name those
	// processes clobber each other's scene file mid-load, which surfaces as
	// a bogus "the test is flaky / there is a race" failure -- that already
	// cost a reviewer hours once.  The pid prefix makes the path unique per
	// process.
	std::string path = dir + std::to_string( (long)getpid() ) + "_" + name;
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

	//----------------------------------------------------------------------
	// validate's NO-ARGUMENT current-head form (FIX 4), at the JSON-RPC
	// surface where the argument contract actually lives.
	//
	// MEASURED MOTIVATION: `validate` used to hard-require `text`, while the
	// system prompt told the model to validate after a structural edit -- so
	// the model re-emitted the ENTIRE scene to check its own three-parameter
	// patch.  Trajectory 20260727T063526Z-a7ee472c: calls 17-19 were three
	// propose_patch calls of 183/184/184 request bytes; call 22 was a
	// `validate` whose `text` argument was 19,828 bytes (the whole head,
	// already in memory), and the turn that produced it cost 6,369 output
	// tokens and 27.8 s.
	//
	// Proves: no-arg validates the head (and reports which headVersion); the
	// text form is UNCHANGED (including its no-head-needed property); a
	// DIAGNOSED document reports the SAME diagnostics either way -- i.e. the
	// no-arg form is an argument shortcut, not a second validator; and the
	// no-head case is honest (an error naming the form that does work, never
	// a false "clean" verdict).
	//----------------------------------------------------------------------
	std::printf( "[validate] the no-argument current-head form\n" );
	{
		// A SECOND session over the same file, so nothing above is disturbed.
		std::unique_ptr<AgentSession> rpcSession = AgentSession::LoadFromFile( goodPath );
		Check( rpcSession != nullptr, "loaded a second session for the RPC checks" );
		if( rpcSession ) {
			const std::string headText = rpcSession->ReadDocument();
			AgentRpcDispatcher rpc( std::move( rpcSession ) );

			auto call = []( AgentRpcDispatcher& d, int id, const std::string& params ) {
				const std::string line =
					std::string( "{\"jsonrpc\":\"2.0\",\"id\":" ) + std::to_string( id ) +
					",\"method\":\"validate\",\"params\":" + params + "}";
				JsonValue env;
				std::string perr;
				JsonParse( d.HandleLine( line ), env, perr );
				return env;
			};
			auto errorCount = []( const JsonValue& env ) {
				const JsonValue& arr = env.get( "result" ).get( "diagnostics" );
				std::size_t n = 0;
				for( std::size_t i = 0; i < arr.size(); ++i )
					if( arr.at( i ).get( "severity" ).asString() == "error" ) ++n;
				return n;
			};
			// A stable, order-independent fingerprint of a diagnostics array,
			// in ONE canonical form both the RPC envelope and the C++
			// AgentDiagnostic vector are rendered into -- so the two can be
			// compared field for field rather than merely counted.
			auto fingerprint = []( const JsonValue& env ) {
				const JsonValue& arr = env.get( "result" ).get( "diagnostics" );
				std::vector<std::string> items;
				for( std::size_t i = 0; i < arr.size(); ++i ) {
					const JsonValue& d = arr.at( i );
					items.push_back( d.get( "severity" ).asString() + "|" +
					                 d.get( "code" ).asString() + "|" +
					                 d.get( "message" ).asString() + "|" +
					                 std::to_string( (long long)d.get( "offset" ).asNumber() ) + "|" +
					                 std::to_string( (long long)d.get( "length" ).asNumber() ) );
				}
				std::sort( items.begin(), items.end() );
				std::string out;
				for( std::size_t i = 0; i < items.size(); ++i ) out += items[i] + "\n";
				return out;
			};
			// The same canonical form, from the C++ side.  The severity
			// spelling mirrors AgentRpc.cpp's SeverityName; a divergence there
			// would fail the equality Checks below rather than pass silently.
			auto diagFingerprint = []( const std::vector<AgentDiagnostic>& diags ) {
				std::vector<std::string> items;
				for( std::size_t i = 0; i < diags.size(); ++i ) {
					const AgentDiagnostic& d = diags[i];
					const char* sev = d.severity == AgentDiagnostic::Severity::Error   ? "error"
					                : d.severity == AgentDiagnostic::Severity::Warning ? "warning"
					                                                                   : "info";
					items.push_back( std::string( sev ) + "|" + d.code + "|" + d.message + "|" +
					                 std::to_string( (long long)d.offset ) + "|" +
					                 std::to_string( (long long)d.length ) );
				}
				std::sort( items.begin(), items.end() );
				std::string out;
				for( std::size_t i = 0; i < items.size(); ++i ) out += items[i] + "\n";
				return out;
			};

			// (1) NO ARGUMENTS -> validates the head, cleanly, and says which.
			const JsonValue noArg = call( rpc, 100, "{}" );
			Check( !noArg.has( "error" ), "validate {} succeeds when a scene is loaded" );
			Check( noArg.get( "result" ).get( "diagnostics" ).isArray(),
			       "validate {} returns a diagnostics array" );
			Check( errorCount( noArg ) == 0,
			       "validate {} on the good head reports ZERO error diagnostics" );
			Check( noArg.get( "result" ).get( "headVersion" ).isObject(),
			       "validate {} reports the headVersion it validated" );
			// The self-describing discriminator: a call whose arguments were
			// MALFORMED degrades to empty params and lands here, so the result
			// must say plainly that it checked the HEAD -- otherwise an empty
			// diagnostics array reads as "my candidate is clean".
			Check( noArg.get( "result" ).get( "validated" ).asString() == "head",
			       "validate {} stamps validated:\"head\"" );

			// An OMITTED params object is the same thing (a model that sends
			// no params at all must not fall into the argument-error path).
			JsonValue noParams;
			std::string perr;
			JsonParse( rpc.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":101,\"method\":\"validate\"}" ), noParams, perr );
			Check( !noParams.has( "error" ) && errorCount( noParams ) == 0,
			       "validate with NO params object at all also validates the head" );

			// A present-but-NULL text reads as absent (read_schema's convention).
			const JsonValue nullText = call( rpc, 102, "{\"text\":null}" );
			Check( !nullText.has( "error" ) && nullText.get( "result" ).get( "headVersion" ).isObject(),
			       "validate {text:null} reads as the no-argument head form" );
			// ... and so does an EMPTY string, for the same reason read_schema
			// treats keyword:"" as "no keyword".  Otherwise ValidateText("")
			// would answer with an EMPTY diagnostics array -- a "clean" verdict
			// on a non-document.
			const JsonValue emptyText = call( rpc, 109, "{\"text\":\"\"}" );
			Check( !emptyText.has( "error" ) &&
			       emptyText.get( "result" ).get( "validated" ).asString() == "head",
			       "validate {text:\"\"} reads as the no-argument head form (never a "
			       "'clean' verdict on an empty non-document)" );

			// ... but a non-string, non-null text is still refused.
			const JsonValue badText = call( rpc, 103, "{\"text\":42}" );
			Check( badText.has( "error" ) &&
			       badText.get( "error" ).get( "message" ).asString()
			           .find( "must be a string" ) != std::string::npos,
			       "validate {text:42} is refused with an actionable message" );

			// (2) THE TEXT FORM IS UNCHANGED: same head text passed explicitly
			// gives the identical diagnostics the no-arg form produced.
			JsonValue textParams = JsonValue::MakeObject();
			textParams.set( "text", JsonValue::MakeString( headText ) );
			const JsonValue sameText = call( rpc, 104, JsonSerialize( textParams ) );
			Check( !sameText.has( "error" ) && errorCount( sameText ) == 0,
			       "the text form still validates a clean candidate" );
			Check( fingerprint( sameText ) == fingerprint( noArg ),
			       "no-arg and text-of-the-head agree diagnostic for diagnostic (clean case)" );
			Check( !sameText.get( "result" ).has( "headVersion" ),
			       "the text form does NOT stamp a headVersion (a candidate is not the head)" );
			Check( sameText.get( "result" ).get( "validated" ).asString() == "text",
			       "the text form stamps validated:\"text\" -- the two forms are never "
			       "confusable, even on identical bytes" );

			// (3) THE NO-ARG FORM VALIDATES THE HEAD, NOT ITS ARGUMENT.  On
			// ONE session, the TEXT form fed the DIAGNOSED scene reports the
			// UNKNOWN_PARAMETER error while the NO-ARG form reports none --
			// so the two are demonstrably reading different inputs and the
			// no-arg verdict is not an echo of whatever was passed in.
			JsonValue badParams = JsonValue::MakeObject();
			badParams.set( "text", JsonValue::MakeString( kBadScene ) );
			const JsonValue badTextForm = call( rpc, 105, JsonSerialize( badParams ) );
			Check( errorCount( badTextForm ) > 0,
			       "the text form on the DIAGNOSED candidate reports errors (red-prove: "
			       "the fixture really is diagnosable)" );
			Check( errorCount( call( rpc, 106, "{}" ) ) == 0,
			       "the no-arg form on the SAME session stays clean -- it validates the "
			       "HEAD, not the argument the text form was just given" );

			// (4) ONE VALIDATOR, not two.  For a DIAGNOSED input the RPC text
			// form is diagnostic-for-diagnostic what AgentSession::ValidateText
			// produces -- code, message, severity, and the byte span alike.
			// This is the non-vacuous half of the "same diagnostics either
			// way" claim: it is asserted where diagnostics actually EXIST.
			Check( diagFingerprint( AgentSession::ValidateText( kBadScene ) ) ==
			           fingerprint( badTextForm ),
			       "the RPC text form IS ValidateText(text), field for field, on a "
			       "DIAGNOSED input" );

			// (5) The no-arg form reads the LIVE head on every call.  Mutate
			// the scene and the reported headVersion must be EXACTLY the one
			// the edit produced -- not a snapshot taken at dispatcher
			// construction, and not a fabricated {0,0}.
			const std::string patch =
				"{\"jsonrpc\":\"2.0\",\"id\":107,\"method\":\"propose_patch\",\"params\":"
				"{\"target\":\"s\",\"param\":\"radius\",\"value\":\"0.91\"}}";
			JsonValue patchEnv;
			std::string pperr;
			JsonParse( rpc.HandleLine( patch ), patchEnv, pperr );
			Check( patchEnv.get( "result" ).get( "applied" ).asBool(),
			       "propose_patch applied (fixture for the live-head check)" );
			const JsonValue afterEdit = call( rpc, 108, "{}" );
			Check( JsonSerialize( afterEdit.get( "result" ).get( "headVersion" ) ) !=
			           JsonSerialize( noArg.get( "result" ).get( "headVersion" ) ),
			       "validate {} reports a DIFFERENT headVersion after an edit (not a "
			       "snapshot from dispatcher construction)" );
			Check( JsonSerialize( afterEdit.get( "result" ).get( "headVersion" ) ) ==
			           JsonSerialize( patchEnv.get( "result" ).get( "headVersion" ) ),
			       "validate {} reports EXACTLY the headVersion the edit produced" );

			// (6) is in its own block below -- it needs a WEDGED head, which
			// needs a Job built directly rather than through this session.
		}
	}

	//----------------------------------------------------------------------
	// (6) THE NO-ARG FORM VALIDATES THE HEAD'S ACTUAL BYTES -- proven on a
	// WEDGED head, i.e. one whose committed text no longer derives.
	//
	// This is the assertion that discriminates.  Every OTHER check here
	// passes vacuously if the branch validated some arbitrary clean string
	// instead of ReadDocument(), because a clean head and (say) an empty
	// document both validate to an EMPTY diagnostics array -- this validator
	// only ever emits Error severity, so there is no "clean but noisy" head
	// to compare.  (Mutation probe: swapping ReadDocument() for a constant
	// left every other assertion in this file green.)
	//
	// A wedged head IS authorable, through exactly one route:
	// Job::ApplyCstParamEdit -- the UNCHECKED GUI property-panel fast path
	// (requireFullDerivability = false).  Retargeting a consumer to an
	// entity declared LATER in the document commits incrementally (the LIVE
	// managers have it) yet leaves bytes that fail a document-ORDER derive.
	// AgentChunkCrudTest's G1 red-proves that shape permanently; here it is
	// the fixture.  The AGENT's own edit verbs use ApplyCstParamEditChecked
	// and refuse this, which is why it must be built through the Job.
	//
	// It is also the case the no-arg form is MOST useful for: a co-editing
	// user can wedge the head from the GUI panel, and this is how the agent
	// finds out.
	//----------------------------------------------------------------------
	std::printf( "[validate] the no-arg form on a WEDGED head (the discriminating case)\n" );
	{
		const std::string wedgePath = WriteTemp( "rise_agent_validate_wedge.RISEscene", kWedgeScene );
		Check( !wedgePath.empty(), "wrote the wedge fixture" );
		Job* pJob = new Job();
		const bool loaded = pJob->LoadAsciiSceneViaCst( wedgePath.c_str() );
		Check( loaded, "the wedge fixture loads cleanly BEFORE the edit" );
		if( loaded ) {
			const int code = pJob->ApplyCstParamEdit( "mat_diffuse", "material",
			                                          "reflectance", 0, "pnt_emit" );
			Check( code == 1,
			       "the UNCHECKED path commits the forward-reference retarget (rawCode 1)" );

			std::unique_ptr<AgentSession> wedged = AgentSession::WrapJob( pJob );
			Check( wedged != nullptr, "wrapped the wedged Job in a session" );
			if( wedged ) {
				const std::string wedgedText = wedged->ReadDocument();
				// The fixture must really be wedged, or everything below is
				// vacuous again.
				std::size_t directErrors = 0;
				for( const AgentDiagnostic& d : AgentSession::ValidateText( wedgedText ) )
					if( d.severity == AgentDiagnostic::Severity::Error ) ++directErrors;
				Check( directErrors > 0,
				       "RED-PROVE: the wedged head's own bytes DO validate with errors" );

				AgentRpcDispatcher wrpc( std::move( wedged ) );
				JsonValue env;
				std::string perr;
				JsonParse( wrpc.HandleLine(
					"{\"jsonrpc\":\"2.0\",\"id\":400,\"method\":\"validate\",\"params\":{}}" ),
					env, perr );
				std::size_t rpcErrors = 0;
				const JsonValue& arr = env.get( "result" ).get( "diagnostics" );
				for( std::size_t i = 0; i < arr.size(); ++i )
					if( arr.at( i ).get( "severity" ).asString() == "error" ) ++rpcErrors;
				Check( rpcErrors == directErrors && rpcErrors > 0,
				       "validate {} on a WEDGED head reports the head's OWN error diagnostics "
				       "-- it validates ReadDocument()'s bytes, not some other clean text" );
				Check( env.get( "result" ).get( "validated" ).asString() == "head",
				       "the wedged-head verdict is stamped validated:\"head\"" );

				// ... and the TEXT form on the SAME dispatcher, given a CLEAN
				// candidate, still reports clean -- so the no-arg form is not
				// simply always reporting the head.
				JsonValue cleanParams = JsonValue::MakeObject();
				cleanParams.set( "text", JsonValue::MakeString( kGoodScene ) );
				JsonValue cleanEnv;
				JsonParse( wrpc.HandleLine(
					"{\"jsonrpc\":\"2.0\",\"id\":401,\"method\":\"validate\",\"params\":" +
					JsonSerialize( cleanParams ) + "}" ), cleanEnv, perr );
				// `.size()` alone would pass vacuously on an ERROR envelope
				// (JsonValue::size() is 0 for the static Null get() yields),
				// so assert the call SUCCEEDED and returned a real array too.
				Check( !cleanEnv.has( "error" ) &&
				       cleanEnv.get( "result" ).get( "diagnostics" ).isArray() &&
				       cleanEnv.get( "result" ).get( "diagnostics" ).size() == 0 &&
				       cleanEnv.get( "result" ).get( "validated" ).asString() == "text",
				       "the text form on a CLEAN candidate stays clean even while the head "
				       "is wedged (the two forms are genuinely independent)" );
			}
		}
		pJob->release();
		std::remove( wedgePath.c_str() );
	}

	//----------------------------------------------------------------------
	// validate with NO scene loaded: the text form still works (documented),
	// the no-argument form is HONEST about having nothing to validate.
	//----------------------------------------------------------------------
	std::printf( "[validate] the no-head case is honest\n" );
	{
		std::unique_ptr<AgentSession> noSession;
		AgentRpcDispatcher headless( std::move( noSession ) );
		auto handle = []( AgentRpcDispatcher& d, const std::string& line ) {
			JsonValue env;
			std::string perr;
			JsonParse( d.HandleLine( line ), env, perr );
			return env;
		};
		const JsonValue noArg = handle( headless,
			"{\"jsonrpc\":\"2.0\",\"id\":300,\"method\":\"validate\",\"params\":{}}" );
		Check( noArg.has( "error" ),
		       "validate {} with NO scene loaded is an ERROR, not a false 'clean' verdict" );
		const std::string msg = noArg.get( "error" ).get( "message" ).asString();
		Check( msg.find( "no scene is loaded" ) != std::string::npos &&
		       msg.find( "'text'" ) != std::string::npos,
		       "the no-head refusal says WHY and names the form that does work" );

		// The documented STATELESS text form is untouched by any of this.
		JsonValue p = JsonValue::MakeObject();
		p.set( "text", JsonValue::MakeString( kGoodScene ) );
		const JsonValue withText = handle( headless,
			"{\"jsonrpc\":\"2.0\",\"id\":301,\"method\":\"validate\",\"params\":" +
			JsonSerialize( p ) + "}" );
		Check( !withText.has( "error" ) &&
		       withText.get( "result" ).get( "diagnostics" ).isArray(),
		       "validate {text} still works with NO scene loaded (the stateless contract)" );
	}

	std::printf( "=== AgentReadValidateTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
