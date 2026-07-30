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

//----------------------------------------------------------------------
// read_schema BATCH form -- {keywords:[...]}.
//
// MOTIVATION (measured, not guessed).  The recorded 2026-07-29 GUI scene
// build (48 LLM turns, 47 tool calls) spent 21 of those tool calls on
// read_schema -- 19 of them before the first edit -- in the index-then-fetch
// shape: 5 {category:...} listings and 16 single-{keyword:...} fetches.
// Every one of those fetches is a full LLM round trip that returns a static
// descriptor dump.  The batch form collapses them into one call.
//
// WHAT MUST HOLD, and is proved below:
//   (a) BATCH == N SINGLES.  Element i of the array is byte-identical to
//       what {keyword:kw[i]} returns on its own -- one generator, not two.
//   (b) The single and category forms are UNCHANGED (object result, not an
//       array), so nothing that already worked moves.
//   (c) The BOUND is enforced ACROSS BOTH PARAMETERS: over the cap is a
//       clean -32602, never a silent truncation that would leave the caller
//       believing it had every schema it asked for, and never a quiet 25th
//       entry when every surface advertises 24.
//   (d) An UNKNOWN keyword inside a batch keeps ITS OWN slot as
//       {keyword, error}, so the good entries still arrive AT THEIR OWN
//       INDEX and the bad one is attributable without counting.
//   (e) POSITIONAL ALIGNMENT IS TOTAL: schema[i] is keywords[i] for every i.
//       Not deduped (a repeated keyword comes back twice) and not reordered;
//       `keyword`, if also sent, is APPENDED, never prepended -- prepending
//       would shift every index by one, which is the trap this contract
//       exists to avoid.
//   (f) `keywords:[]` honestly returns an EMPTY array; it must NOT fall
//       through to the ~286 KB whole-grammar dump.
//----------------------------------------------------------------------
static void RunReadSchemaBatchTest()
{
	std::printf( "[read_schema] batch form {keywords:[...]}\n" );

	// STATELESS: read_schema is a pure descriptor-registry walk, so a
	// session-less dispatcher is the honest fixture (and proves the batch
	// works in the no-head bootstrap an authoring agent starts from).
	std::unique_ptr<AgentSession> none;
	AgentRpcDispatcher rpc( std::move( none ) );
	auto call = [&rpc]( int id, const std::string& paramsJson ) {
		JsonValue env; std::string perr;
		const std::string line = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string( id ) +
			",\"method\":\"read_schema\",\"params\":" + paramsJson + "}";
		Check( JsonParse( rpc.HandleLine( line ), env, perr ),
		       "read_schema response parses as JSON (id " + std::to_string( id ) + ")" );
		return env;
	};

	// The sixteen keywords the recorded trajectory fetched one at a time.
	static const char* const kWanted[] = {
		"sdf_geometry", "pbr_metallic_roughness_material", "uniformcolor_painter",
		"directional_light", "omni_light", "standard_object", "dielectric_material",
		"perfectrefractor_material", "csg_object", "cylinder_geometry", "box_geometry",
		"infiniteplane_geometry", "torus_geometry", "polished_material",
		"lambertian_luminaire_material", "circulardisk_geometry" };
	const std::size_t nWanted = sizeof( kWanted ) / sizeof( kWanted[0] );

	std::string arrJson = "[";
	for( std::size_t i = 0; i < nWanted; ++i ) {
		if( i ) arrJson += ',';
		arrJson += '"'; arrJson += kWanted[i]; arrJson += '"';
	}
	arrJson += ']';

	// (a) MONEY: the batch equals the N singles, element for element.
	{
		const JsonValue env = call( 1, "{\"keywords\":" + arrJson + "}" );
		Check( !env.has( "error" ), "the 16-keyword batch is accepted" );
		const JsonValue& schema = env.get( "result" ).get( "schema" );
		Check( schema.isArray(), "the batch result's `schema` is an ARRAY (not the single form's object)" );
		Check( schema.size() == nWanted,
		       "the array has exactly one entry per requested keyword (" +
		       std::to_string( nWanted ) + ")" );
		// Decoupled, not short-circuited: an `allMatch`-gated order check would
		// pass vacuously the moment the byte comparison failed.
		bool allMatch = ( schema.size() == nWanted );
		bool orderKept = ( schema.size() == nWanted );
		for( std::size_t i = 0; i < nWanted && i < schema.size(); ++i ) {
			const JsonValue single = call( 100 + (int)i, std::string( "{\"keyword\":\"" ) + kWanted[i] + "\"}" );
			if( JsonSerialize( schema.at( i ) ) != JsonSerialize( single.get( "result" ).get( "schema" ) ) )
				allMatch = false;
			if( schema.at( i ).get( "keyword" ).asString() != kWanted[i] )
				orderKept = false;
		}
		Check( allMatch,
		       "MONEY (a): every batch element is BYTE-IDENTICAL to the same keyword's single-form "
		       "result -- one generator, not two" );
		Check( orderKept, "MONEY (a): the array is in REQUEST order, so a caller can index into it" );

		// The saving this exists for, stated as a fact about this result:
		// one call carrying what sixteen used to.
		std::printf( "  [read_schema] 16-keyword batch payload: %zu bytes in ONE call\n",
		             JsonSerialize( schema ).size() );
	}

	// (b) The SINGLE and CATEGORY forms are untouched.
	{
		const JsonValue one = call( 2, "{\"keyword\":\"sphere_geometry\"}" );
		Check( one.get( "result" ).get( "schema" ).isObject() &&
		       one.get( "result" ).get( "schema" ).get( "keyword" ).asString() == "sphere_geometry",
		       "MONEY (b): the single `keyword` form still returns a schema OBJECT" );

		const JsonValue cat = call( 3, "{\"category\":\"material\"}" );
		const JsonValue& cs = cat.get( "result" ).get( "schema" );
		Check( cs.isObject() && cs.get( "category" ).asString() == "material" &&
		       cs.get( "chunks" ).isArray() && cs.get( "chunks" ).size() > 0,
		       "MONEY (b): the `category` form still returns the cheap {category,chunks[]} listing" );
		Check( JsonSerialize( cs ).find( "\"properties\"" ) == std::string::npos,
		       "MONEY (b): the category listing is still NAMES ONLY -- it did not quietly grow "
		       "the per-parameter dump the batch form exists to deliver on demand" );

		// keyword still wins over category (the pre-existing precedence).
		const JsonValue both = call( 4, "{\"keyword\":\"sphere_geometry\",\"category\":\"material\"}" );
		Check( both.get( "result" ).get( "schema" ).get( "keyword" ).asString() == "sphere_geometry",
		       "keyword still takes precedence over category" );
	}

	// (c) The BOUND: 24 accepted, 25 refused, refused CLEANLY -- and counted
	// across BOTH parameters, since both produce entries.
	{
		std::string ok24 = "[", over25 = "[";
		for( int i = 0; i < 25; ++i ) {
			const std::string q = std::string( i ? "," : "" ) + "\"sphere_geometry\"";
			if( i < 24 ) ok24 += q;
			over25 += q;
		}
		ok24 += ']'; over25 += ']';

		const JsonValue at = call( 5, "{\"keywords\":" + ok24 + "}" );
		Check( !at.has( "error" ) && at.get( "result" ).get( "schema" ).size() == 24,
		       "MONEY (c): a batch AT the 24-entry cap is accepted and returns 24 entries" );

		const JsonValue over = call( 6, "{\"keywords\":" + over25 + "}" );
		Check( over.has( "error" ) && over.get( "error" ).get( "code" ).asNumber( 0 ) == -32602,
		       "MONEY (c): a batch OVER the cap is a clean -32602, not a silent truncation" );
		const std::string msg = over.get( "error" ).get( "message" ).asString();
		Check( msg.find( "24" ) != std::string::npos && msg.find( "25" ) != std::string::npos,
		       "the refusal states BOTH the cap and what was supplied" );

		// The off-by-one the cap must not have: 24 in the array PLUS a
		// `keyword` is 25 keywords, and every model-facing surface says 24.
		const JsonValue plusOne = call( 7,
			"{\"keyword\":\"box_geometry\",\"keywords\":" + ok24 + "}" );
		Check( plusOne.has( "error" ) &&
		       plusOne.get( "error" ).get( "message" ).asString().find( "25" ) != std::string::npos,
		       "MONEY (c): the cap counts `keyword` AND `keywords` together -- 24 + 1 is 25 and "
		       "is refused, so the advertised limit is the enforced limit" );
		// ...and 23 + 1 still fits, so the cap was not simply tightened.
		std::string arr23 = "[";
		for( int i = 0; i < 23; ++i ) { if( i ) arr23 += ','; arr23 += "\"sphere_geometry\""; }
		arr23 += ']';
		const JsonValue justFits = call( 8,
			"{\"keyword\":\"box_geometry\",\"keywords\":" + arr23 + "}" );
		Check( !justFits.has( "error" ) &&
		       justFits.get( "result" ).get( "schema" ).size() == 24,
		       "23 + `keyword` is exactly 24 and is accepted" );
	}

	// (d) An UNKNOWN keyword inside a batch: a SELF-IDENTIFYING error object in
	// its own slot, with the good neighbours at their own indices.
	{
		const JsonValue env = call( 9,
			"{\"keywords\":[\"sphere_geometry\",\"not_a_chunk\",\"box_geometry\"]}" );
		Check( !env.has( "error" ),
		       "MONEY (d): one unknown keyword does NOT fail the whole batch" );
		const JsonValue& schema = env.get( "result" ).get( "schema" );
		Check( schema.size() == 3, "the array still has one slot per request" );
		Check( schema.at( 0 ).get( "keyword" ).asString() == "sphere_geometry" &&
		       schema.at( 2 ).get( "keyword" ).asString() == "box_geometry",
		       "MONEY (d): the good entries keep their REQUEST INDEX around the bad one" );
		Check( schema.at( 1 ).has( "error" ) &&
		       schema.at( 1 ).get( "error" ).asString().find( "not_a_chunk" ) != std::string::npos,
		       "MONEY (d): the unknown entry carries the same {error:...} the single form gives" );
		Check( schema.at( 1 ).get( "keyword" ).asString() == "not_a_chunk",
		       "MONEY (d): ...and NAMES ITSELF, so the typo is attributable without counting "
		       "array positions" );
	}

	// (e) POSITIONAL ALIGNMENT, total: schema[i] == keywords[i] for every i,
	// duplicates included; `keyword` is APPENDED, never prepended.
	{
		const JsonValue env = call( 10,
			"{\"keyword\":\"torus_geometry\",\"keywords\":"
			"[\"box_geometry\",\"sphere_geometry\",\"box_geometry\",\"not_a_chunk\"]}" );
		Check( !env.has( "error" ), "keyword + keywords together is accepted, not a -32602" );
		const JsonValue& schema = env.get( "result" ).get( "schema" );
		static const char* const kExpect[] = {
			"box_geometry", "sphere_geometry", "box_geometry", "not_a_chunk", "torus_geometry" };
		Check( schema.size() == 5,
		       "MONEY (e): one entry per requested keyword -- the DUPLICATE is NOT collapsed "
		       "(collapsing would silently shift every later index)" );
		bool aligned = ( schema.size() == 5 );
		for( std::size_t i = 0; aligned && i < 5; ++i )
			if( schema.at( i ).get( "keyword" ).asString() != kExpect[i] ) aligned = false;
		Check( aligned,
		       "MONEY (e): schema[i] IS keywords[i] for every i, and `keyword` lands LAST at "
		       "schema[keywords.length] -- prepending it would shift every index by one" );
	}

	// (f) An EMPTY array is an empty array -- never the whole-grammar dump.
	{
		const JsonValue env = call( 11, "{\"keywords\":[]}" );
		Check( !env.has( "error" ), "keywords:[] is accepted" );
		const JsonValue& schema = env.get( "result" ).get( "schema" );
		Check( schema.isArray() && schema.size() == 0,
		       "MONEY (f): keywords:[] returns an EMPTY array, NOT the ~286 KB whole-grammar dump" );
		// The trap this guards, made explicit: the bare form IS the huge dump.
		const JsonValue bare = call( 12, "{}" );
		Check( JsonSerialize( bare.get( "result" ).get( "schema" ) ).size() > 100000,
		       "PRECONDITION: the bare form really is the huge dump keywords:[] must not become" );
	}

	// Malformed shapes are clean -32602s, and they name the offending index.
	{
		const JsonValue notArray = call( 13, "{\"keywords\":\"sphere_geometry\"}" );
		Check( notArray.has( "error" ) &&
		       notArray.get( "error" ).get( "message" ).asString().find( "array" ) != std::string::npos,
		       "keywords as a STRING is a clean -32602 naming the expected array" );
		const JsonValue badElem = call( 14, "{\"keywords\":[\"sphere_geometry\",7]}" );
		Check( badElem.has( "error" ) &&
		       badElem.get( "error" ).get( "message" ).asString().find( "keywords[1]" ) != std::string::npos,
		       "a non-string ELEMENT is a clean -32602 that names its index" );
		// A malformed `category` must be diagnosed on BOTH forms.  The batch
		// branch returns before the single form's parse block, so hoisting the
		// type check above it is the only thing keeping these two identical --
		// otherwise the same bad value is a clean -32602 on one form and
		// silently ignored on the other.
		const JsonValue badCatSingle = call( 16, "{\"keyword\":\"sphere_geometry\",\"category\":5}" );
		const JsonValue badCatBatch  = call( 17, "{\"keywords\":[\"sphere_geometry\"],\"category\":5}" );
		Check( badCatSingle.has( "error" ) && badCatBatch.has( "error" ) &&
		       badCatSingle.get( "error" ).get( "message" ).asString() ==
		           badCatBatch.get( "error" ).get( "message" ).asString(),
		       "a non-string `category` is the SAME clean -32602 on the batch form as on the "
		       "single form -- one malformed value, one diagnosis" );
		// A well-formed `category` alongside a batch is simply ignored (the
		// batch is more specific), NOT an error.
		const JsonValue catWithBatch = call( 18,
			"{\"keywords\":[\"sphere_geometry\"],\"category\":\"material\"}" );
		Check( !catWithBatch.has( "error" ) &&
		       catWithBatch.get( "result" ).get( "schema" ).isArray() &&
		       catWithBatch.get( "result" ).get( "schema" ).size() == 1,
		       "a VALID `category` alongside `keywords` is ignored, not an error" );
		// null reads as absent, matching every other optional param here.
		const JsonValue nullKws = call( 15, "{\"keywords\":null,\"keyword\":\"sphere_geometry\"}" );
		Check( !nullKws.has( "error" ) &&
		       nullKws.get( "result" ).get( "schema" ).isObject(),
		       "keywords:null reads as ABSENT (the single form still applies)" );
	}
}

//----------------------------------------------------------------------
// Creative-richness P2 (73-creative-richness-design.md sec 2 P2, RE-TARGETED
// by sec 7 to the two MEASURED bare-prompt deficits): unit-level red-proofs
// of AgentSession::ComputeDesignNote, the ONE shared scan both carriers
// (render-result, validate) call.  STATELESS -- calls the static function
// directly on crafted CST text, no session/Job needed.  Every crafted
// document below is minimal (just enough top-level chunks for the scan to
// see, no materials/params that would actually DERIVE) since the scan only
// walks chunk keywords -- it never parses into a Job.
//
// Orthogonal fixtures, one variable changed at a time:
//   docA3NoScalar        -- 3 standard_object, no scalar_painter -> A fires alone (3<4, B silent)
//   docA3WithScalar      -- same 3 objects + a scalar_painter    -> A silenced (B still silent, 3<4)
//   docB4AllBoxWithScalar-- 4 standard_object/box_geometry + a scalar_painter (A silenced) -> B fires alone
//   docB4WithSdf         -- same as above + an sdf_geometry      -> B silenced too -> EMPTY
//   docBelowThresholds   -- 2 standard_object, nothing else      -> both silent (below either gate)
//   docCombined          -- 4 standard_object, no scalar_painter, no advanced geometry -> BOTH fire
//----------------------------------------------------------------------
static void RunDesignNoteScanTest()
{
	std::printf( "[design-note] AgentSession::ComputeDesignNote red-proofs\n" );

	const std::string docA3NoScalar =
		"RISE ASCII SCENE 7\n"
		"standard_object\n{\n\tname a\n}\n\n"
		"standard_object\n{\n\tname b\n}\n\n"
		"standard_object\n{\n\tname c\n}\n";
	{
		const std::string note = AgentSession::ComputeDesignNote( docA3NoScalar );
		Check( note.find( "DESIGN NOTE" ) != std::string::npos &&
		       note.find( "scalar_painter" ) != std::string::npos,
		       "RED-PROVE A: 3 standard_object + no scalar_painter fires condition A" );
		Check( note.find( "geometry census" ) == std::string::npos,
		       "...and condition B stays silent (3 < 4)" );
	}

	const std::string docA3WithScalar =
		"RISE ASCII SCENE 7\n"
		"standard_object\n{\n\tname a\n}\n\n"
		"standard_object\n{\n\tname b\n}\n\n"
		"standard_object\n{\n\tname c\n}\n\n"
		"scalar_painter\n{\n\tname r\n\tfile none\n}\n";
	{
		const std::string note = AgentSession::ComputeDesignNote( docA3WithScalar );
		Check( note.empty(),
		       "GREEN-PROVE A: the SAME 3 objects + a bound scalar_painter silences condition A "
		       "(and B stays silent, still 3 < 4) -- empty, not just A's clause missing" );
	}

	const std::string docB4AllBoxWithScalar =
		"RISE ASCII SCENE 7\n"
		"scalar_painter\n{\n\tname r\n\tfile none\n}\n\n"
		"box_geometry\n{\n\tname geo\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n"
		"standard_object\n{\n\tname a\n\tgeometry geo\n}\n\n"
		"standard_object\n{\n\tname b\n\tgeometry geo\n}\n\n"
		"standard_object\n{\n\tname c\n\tgeometry geo\n}\n\n"
		"standard_object\n{\n\tname d\n\tgeometry geo\n}\n";
	{
		const std::string note = AgentSession::ComputeDesignNote( docB4AllBoxWithScalar );
		Check( note.find( "DESIGN NOTE" ) != std::string::npos &&
		       note.find( "geometry census" ) != std::string::npos &&
		       note.find( "box_geometry" ) != std::string::npos,
		       "RED-PROVE B: 4 standard_object all bound to box_geometry, no advanced-geometry "
		       "chunk anywhere, fires condition B" );
		Check( note.find( "the scalar pipe is unused" ) == std::string::npos,
		       "...and condition A stays silent (a scalar_painter IS bound)" );
	}

	const std::string docB4WithSdf = docB4AllBoxWithScalar +
		"sdf_geometry\n{\n\tname sdf_geo\n\tpart\t\tsphere union 0 0 0 0 0 0 0 0 0 1 0\n}\n";
	{
		const std::string note = AgentSession::ComputeDesignNote( docB4WithSdf );
		Check( note.empty(),
		       "GREEN-PROVE B: the SAME 4-box scene + an sdf_geometry chunk silences condition B "
		       "too (A was already silenced by the scalar_painter) -- fully empty" );
	}

	const std::string docBelowThresholds =
		"RISE ASCII SCENE 7\n"
		"standard_object\n{\n\tname a\n}\n\n"
		"standard_object\n{\n\tname b\n}\n";
	{
		const std::string note = AgentSession::ComputeDesignNote( docBelowThresholds );
		Check( note.empty(),
		       "GREEN-PROVE (thresholds): 2 standard_object -- below BOTH gates (3 for A, 4 for "
		       "B) -- stays silent even with no scalar_painter and no advanced geometry" );
	}

	const std::string docCombined =
		"RISE ASCII SCENE 7\n"
		"box_geometry\n{\n\tname geo\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n"
		"standard_object\n{\n\tname a\n\tgeometry geo\n}\n\n"
		"standard_object\n{\n\tname b\n\tgeometry geo\n}\n\n"
		"standard_object\n{\n\tname c\n\tgeometry geo\n}\n\n"
		"standard_object\n{\n\tname d\n\tgeometry geo\n}\n";
	{
		const std::string note = AgentSession::ComputeDesignNote( docCombined );
		Check( note.find( "DESIGN NOTE" ) != std::string::npos &&
		       note.find( "the scalar pipe is unused" ) != std::string::npos &&
		       note.find( "geometry census" ) != std::string::npos,
		       "COMBINED: 4 standard_object, no scalar_painter, no advanced geometry -- fires "
		       "BOTH conditions into one note" );
		// The anti-churn escape clause is load-bearing from day one (sec 2 P2)
		// -- present regardless of which condition(s) fired.
		Check( note.find( "ignore this note and do not churn" ) != std::string::npos,
		       "the anti-churn escape clause is present" );
	}

	// Empty document text -> empty note (defensive; no carrier ever calls
	// this with an empty string in practice, but the function must not
	// mis-scan a degenerate input as a false trigger).
	Check( AgentSession::ComputeDesignNote( std::string() ).empty(),
	       "empty document text -> empty note" );
}

// A minimal, fully-derivable 3-sphere scene (mirrors AgentObjectMapTest.cpp's
// kScene3 shape) with NO scalar_painter anywhere -- trips design-note
// condition A. Needs a real rasterizer/camera/film so LoadAsciiSceneViaCst
// actually derives (unlike RunDesignNoteScanTest's bare-CST fixtures above,
// `validate`'s HEAD form needs a genuinely loaded session).
static const char* const kValidateNoteTriggerScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 16\n\theight 16\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 6\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"lambertian_material\n{\n\tname mat\n\treflectance pnt\n}\n\n"
	"sphere_geometry\n{\n\tname geo\n\tradius 0.7\n}\n\n"
	"standard_object\n{\n\tname sph_a\n\tgeometry geo\n\tmaterial mat\n\tposition -1.7 0 0\n}\n\n"
	"standard_object\n{\n\tname sph_b\n\tgeometry geo\n\tmaterial mat\n\tposition 0 0 0\n}\n\n"
	"standard_object\n{\n\tname sph_c\n\tgeometry geo\n\tmaterial mat\n\tposition 1.7 0 0\n}\n";

//----------------------------------------------------------------------
// Creative-richness P2 carrier test: `validate` (both the HEAD form and the
// stateless `text` form) attaches the SAME design note the render-result
// carrier does, via the ONE shared AgentSession::ComputeDesignNote helper --
// present (key populated) on a triggering document, OMITTED ENTIRELY (not
// present-but-empty) on a clean one.
//----------------------------------------------------------------------
static void RunValidateDesignNoteCarrierTest()
{
	std::printf( "[validate] design-note carrier (head form + text form)\n" );

	// -- HEAD form, triggering document -------------------------------
	{
		const std::string scenePath = WriteTemp( "rise_validate_note_trigger.RISEscene", kValidateNoteTriggerScene );
		std::unique_ptr<AgentSession> session = AgentSession::LoadFromFile( scenePath );
		Check( session != nullptr, "design-note trigger scene loads" );
		if( session ) {
			AgentRpcDispatcher rpc( std::move( session ) );
			JsonValue env; std::string err;
			Check( JsonParse( rpc.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"validate\",\"params\":{}}" ), env, err ),
				"head-form validate response parses" );
			const JsonValue& r = env.get( "result" );
			Check( r.has( "note" ) &&
			       r.get( "note" ).asString().find( "DESIGN NOTE" ) != std::string::npos &&
			       r.get( "note" ).asString().find( "scalar_painter" ) != std::string::npos,
			       "MONEY: validate's HEAD form carries the design note on a triggering document "
			       "(3 spheres, no scalar_painter)" );
		}
		std::remove( scenePath.c_str() );
	}

	// -- HEAD form, clean document (kGoodScene: 0 standard_object -- below
	//    either gate) -- key must be OMITTED ENTIRELY, not present-empty.
	{
		const std::string scenePath = WriteTemp( "rise_validate_note_clean.RISEscene", kGoodScene );
		std::unique_ptr<AgentSession> session = AgentSession::LoadFromFile( scenePath );
		Check( session != nullptr, "clean scene loads" );
		if( session ) {
			AgentRpcDispatcher rpc( std::move( session ) );
			JsonValue env; std::string err;
			Check( JsonParse( rpc.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"validate\",\"params\":{}}" ), env, err ),
				"head-form validate response parses (clean scene)" );
			const JsonValue& r = env.get( "result" );
			Check( !r.has( "note" ),
			       "MONEY: validate's HEAD form OMITS the `note` key entirely on a clean document "
			       "(key absent, not an empty string)" );
		}
		std::remove( scenePath.c_str() );
	}

	// -- TEXT form (stateless, no head needed), triggering candidate --
	{
		std::unique_ptr<AgentSession> noSession;
		AgentRpcDispatcher headless( std::move( noSession ) );
		JsonValue p = JsonValue::MakeObject();
		p.set( "text", JsonValue::MakeString( kValidateNoteTriggerScene ) );
		JsonValue env; std::string err;
		Check( JsonParse( headless.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"validate\",\"params\":" +
			JsonSerialize( p ) + "}" ), env, err ),
			"text-form validate response parses" );
		const JsonValue& r = env.get( "result" );
		Check( r.get( "validated" ).asString() == "text", "validated == \"text\"" );
		Check( r.has( "note" ) &&
		       r.get( "note" ).asString().find( "DESIGN NOTE" ) != std::string::npos,
		       "MONEY: validate's TEXT form ALSO carries the design note on a triggering candidate "
		       "-- the SAME shared scan, no separate implementation to drift" );
	}

	// -- TEXT form, clean candidate -- key omitted entirely.
	{
		std::unique_ptr<AgentSession> noSession;
		AgentRpcDispatcher headless( std::move( noSession ) );
		JsonValue p = JsonValue::MakeObject();
		p.set( "text", JsonValue::MakeString( kGoodScene ) );
		JsonValue env; std::string err;
		Check( JsonParse( headless.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"validate\",\"params\":" +
			JsonSerialize( p ) + "}" ), env, err ),
			"text-form validate response parses (clean candidate)" );
		const JsonValue& r = env.get( "result" );
		Check( !r.has( "note" ),
		       "MONEY: validate's TEXT form OMITS `note` entirely on a clean candidate" );
	}
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
			// ... but an EMPTY STRING does NOT.  PRESENCE of a string selects
			// the text form; only OMISSION (or null) selects the head.  The
			// empty candidate is answered HONESTLY -- EMPTY_DOCUMENT, not a
			// clean verdict, and not a silent redirect to the head.
			const JsonValue emptyText = call( rpc, 109, "{\"text\":\"\"}" );
			Check( !emptyText.has( "error" ) &&
			       emptyText.get( "result" ).get( "validated" ).asString() == "text",
			       "validate {text:\"\"} takes the TEXT form -- presence of a string selects "
			       "it, so an empty candidate is never silently rerouted to the head" );
			Check( !emptyText.get( "result" ).has( "headVersion" ),
			       "validate {text:\"\"} stamps NO headVersion (it validated a candidate)" );
			Check( errorCount( emptyText ) == 1 &&
			       emptyText.get( "result" ).get( "diagnostics" ).at( 0 ).get( "code" ).asString()
			           == "EMPTY_DOCUMENT",
			       "validate {text:\"\"} reports EMPTY_DOCUMENT -- never a 'clean' verdict on "
			       "a non-document" );
			// The SAME answer for every degenerate shape: whitespace-only and
			// comments-only both round-trip and both derive with zero
			// diagnostics, so emptiness is judged on chunk COUNT, not bytes.
			const JsonValue wsText = call( rpc, 110, "{\"text\":\"   \\n\\t\\n\"}" );
			Check( errorCount( wsText ) == 1 &&
			       wsText.get( "result" ).get( "diagnostics" ).at( 0 ).get( "code" ).asString()
			           == "EMPTY_DOCUMENT",
			       "validate on a WHITESPACE-ONLY candidate reports EMPTY_DOCUMENT too" );
			const JsonValue commentText = call( rpc, 111, "{\"text\":\"# just a comment\\n\"}" );
			Check( errorCount( commentText ) == 1 &&
			       commentText.get( "result" ).get( "diagnostics" ).at( 0 ).get( "code" ).asString()
			           == "EMPTY_DOCUMENT",
			       "validate on a COMMENTS-ONLY candidate reports EMPTY_DOCUMENT too" );

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

		// ...INCLUDING an EMPTY candidate.  Presence of the string selects the
		// text form, so this is a diagnosed candidate, NOT the head form's
		// "no scene is loaded" error.  Reading "" as ABSENT made this call
		// FAIL outright on a headless session -- a valid input turned into an
		// error by a routing special case.
		const JsonValue emptyNoHead = handle( headless,
			"{\"jsonrpc\":\"2.0\",\"id\":302,\"method\":\"validate\",\"params\":{\"text\":\"\"}}" );
		Check( !emptyNoHead.has( "error" ) &&
		       emptyNoHead.get( "result" ).get( "validated" ).asString() == "text",
		       "validate {text:\"\"} with NO scene loaded is the TEXT form, not an error" );
		Check( emptyNoHead.get( "result" ).get( "diagnostics" ).size() == 1 &&
		       emptyNoHead.get( "result" ).get( "diagnostics" ).at( 0 ).get( "code" ).asString()
		           == "EMPTY_DOCUMENT",
		       "...and it reports EMPTY_DOCUMENT" );
	}

	RunReadSchemaBatchTest();
	RunDesignNoteScanTest();
	RunValidateDesignNoteCarrierTest();

	std::printf( "=== AgentReadValidateTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
