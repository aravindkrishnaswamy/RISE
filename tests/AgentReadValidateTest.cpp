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
		// Doc 91: `a` binds `material rm` (declared further down -- fine,
		// the reference-graph scan is order-independent per the P2-1 note
		// just below) so condition G (DESIGN_UNBOUND_MATERIAL) does not
		// ALSO fire: this fixture's whole point is that the note goes
		// FULLY empty, and an unreferenced ggx_material would otherwise
		// make that legitimately false.  `b` binds a SECOND material whose
		// `reflectance` is a procedural expression field, so condition H
		// (DESIGN_FLAT_ALBEDO) does not fire EITHER -- `rm` spells out only
		// `alphax` (its OTHER colour-pipe slots, `rd`/`rs`/`emissive`, are
		// left at their descriptor defaults, which do not count as bound),
		// so without this the document would have zero SPELLED colour
		// bindings and H would fire VACUOUSLY, the
		// same "every one that exists is a constant" wording condition A's
		// own comment just below explains for the scalar pipe.
		"standard_object\n{\n\tname a\n\tmaterial rm\n}\n\n"
		"standard_object\n{\n\tname b\n\tmaterial doc91_procedural_mat\n}\n\n"
		"standard_object\n{\n\tname c\n}\n\n"
		// Deliberately only 4 distinct numeric literals in the body (below
		// kParamErosionLiteralGate == 6) so this fixture does not ALSO trip
		// DESIGN_PARAM_METADATA_EROSION -- that condition is orthogonal to
		// what this fixture is testing.
		"expression_painter\n{\n\tname doc91_field\n\texpr fbm(P*4.0,4,0.5,2.0)\n}\n\n"
		"lambertian_material\n{\n\tname doc91_procedural_mat\n\treflectance doc91_field\n}\n\n"
		// Adoption polish item 3: condition A is now BINDING-aware, so a
		// scalar_painter chunk with nothing referencing it no longer
		// silences the note (that WAS the bug -- a decoy chunk used to be
		// enough).  This fixture now genuinely varies a physical-scalar
		// material slot: ggx_material's `alphax` bound to a scalar_painter
		// carrying the "expression" (spatially-varying) form.  DECLARE
		// scalar_painter `r` BEFORE ggx_material `rm` -- DeriveToJob is
		// strict declare-before-use, and a forward reference here left the
		// material's `alphax` unresolved at derive time (review-round P2-1:
		// the CST-level design scan is order-independent so the assertions
		// still passed, but every run spammed an eLog_Error).
		"scalar_painter\n{\n\tname r\n\texpression\tu\n}\n\n"
		"ggx_material\n{\n\tname rm\n\talphax r\n}\n";
	{
		const std::string note = AgentSession::ComputeDesignNote( docA3WithScalar );
		Check( note.empty(),
		       "GREEN-PROVE A: the SAME 3 objects + a bound scalar_painter silences condition A "
		       "(and B stays silent, still 3 < 4) -- empty, not just A's clause missing" );
	}

	const std::string docB4AllBoxWithScalar =
		"RISE ASCII SCENE 7\n"
		// Same fix as docA3WithScalar above -- a genuinely-varying binding,
		// not a decoy chunk (adoption polish item 3), declared BEFORE the
		// ggx_material that references it (declare-before-use, P2-1).
		"scalar_painter\n{\n\tname r\n\texpression\tu\n}\n\n"
		"ggx_material\n{\n\tname rm\n\talphax r\n}\n\n"
		"box_geometry\n{\n\tname geo\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n"
		// Doc 91: `a` binds `material rm` so the derived docB4WithSdf fixture
		// below (whose whole point is a FULLY empty note) does not also trip
		// condition G on the otherwise-unreferenced ggx_material; `b` binds a
		// SECOND material with a procedural `reflectance` so condition H does
		// not fire either (`rm` spells only `alphax` -- its other colour-pipe
		// slots are unspelled defaults, which do not count as bound -- so
		// without this the document would have zero SPELLED colour bindings and H
		// would fire VACUOUSLY -- see docA3WithScalar's identical note above).
		"expression_painter\n{\n\tname doc91_field\n\texpr fbm(P*4.0,4,0.5,2.0)\n}\n\n"
		"lambertian_material\n{\n\tname doc91_procedural_mat\n\treflectance doc91_field\n}\n\n"
		// RELIEF_MODIFIER_DESIGN sec 9 (2026-09-06), condition Q: `b`'s
		// varying `reflectance` + real geometry + no `modifier` would
		// otherwise ALSO be a DESIGN_FLAT_RELIEF candidate, and the whole
		// point of the docB4WithSdf fixture below is a FULLY empty note --
		// bind a `relief_modifier` on `b` (built on the SAME `doc91_field`,
		// so this is not a decoy chunk either) purely to silence Q, exactly
		// as `rm`/`doc91_procedural_mat` above silence G/H.
		"scalar_painter\n{\n\tname doc91_field_h\n\tpainter doc91_field\n\tchannel R\n}\n\n"
		"relief_modifier\n{\n\tname doc91_relief\n\theight doc91_field_h\n\tscale 0.02\n}\n\n"
		"standard_object\n{\n\tname a\n\tgeometry geo\n\tmaterial rm\n}\n\n"
		"standard_object\n{\n\tname b\n\tgeometry geo\n\tmaterial doc91_procedural_mat\n"
		"\tmodifier doc91_relief\n}\n\n"
		"standard_object\n{\n\tname c\n\tgeometry geo\n}\n\n"
		"standard_object\n{\n\tname d\n\tgeometry geo\n}\n";
	{
		const std::string note = AgentSession::ComputeDesignNote( docB4AllBoxWithScalar );
		Check( note.find( "DESIGN NOTE" ) != std::string::npos &&
		       note.find( "geometry census" ) != std::string::npos &&
		       note.find( "box_geometry" ) != std::string::npos,
		       "RED-PROVE B: 4 standard_object all bound to box_geometry, no advanced-geometry "
		       "chunk anywhere, fires condition B" );
		Check( note.find( "no physical-scalar material parameter" ) == std::string::npos,
		       "...and condition A stays silent (a scalar_painter IS bound AND actually varies)" );
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
		// Adoption polish item 3: the literal phrase "the scalar pipe is
		// unused" was retired with the stale wording it named -- condition
		// A's clause now asserts only what the binding-aware check tests
		// (FormatScalarPipeUnusedClause_).
		Check( note.find( "DESIGN NOTE" ) != std::string::npos &&
		       note.find( "no physical-scalar material parameter" ) != std::string::npos &&
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
// Creative-richness P2.b carrier test (73-creative-richness-design.md sec
// 9's closing recommendation): `validate` (both the HEAD form and the
// stateless `text` form) NO LONGER attaches a `note` field -- EVER, clean
// or triggering, this is not a "same convention as read_skill" omission
// anymore -- and instead carries the SAME two design-note conditions as
// Info-severity `diagnostics` entries (DESIGN_SCALAR_PIPE_UNUSED /
// DESIGN_NO_ADVANCED_GEOMETRY), via the ONE shared
// AppendDesignDiagnostics_ helper.  (Formerly RunValidateDesignNoteCarrierTest
// -- renamed because validate's carrier mechanism changed from `note` to
// `diagnostics`; the render-result carrier's `note` is untouched and has
// its own coverage in RunDesignNoteScanTest above.)
//----------------------------------------------------------------------
static void RunValidateDesignDiagnosticsCarrierTest()
{
	std::printf( "[validate] design-diagnostics carrier (head form + text form)\n" );

	auto findDiag = []( const JsonValue& diags, const std::string& code ) -> const JsonValue* {
		for( std::size_t i = 0; i < diags.size(); ++i )
			if( diags.at( i ).get( "code" ).asString() == code ) return &diags.at( i );
		return nullptr;
	};

	// -- HEAD form, triggering document -------------------------------
	{
		const std::string scenePath = WriteTemp( "rise_validate_diag_trigger.RISEscene", kValidateNoteTriggerScene );
		std::unique_ptr<AgentSession> session = AgentSession::LoadFromFile( scenePath );
		Check( session != nullptr, "design-diagnostics trigger scene loads" );
		if( session ) {
			AgentRpcDispatcher rpc( std::move( session ) );
			JsonValue env; std::string err;
			Check( JsonParse( rpc.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"validate\",\"params\":{}}" ), env, err ),
				"head-form validate response parses" );
			const JsonValue& r = env.get( "result" );
			Check( !r.has( "note" ),
			       "MONEY (P2.b): validate's HEAD form carries NO `note` field, even on a "
			       "triggering document -- render keeps `note`, validate carries `diagnostics`" );
			const JsonValue& diags = r.get( "diagnostics" );
			const JsonValue* d = findDiag( diags, "DESIGN_SCALAR_PIPE_UNUSED" );
			Check( d != nullptr,
			       "MONEY: validate's HEAD form carries a DESIGN_SCALAR_PIPE_UNUSED diagnostic on a "
			       "triggering document (3 spheres, no scalar_painter)" );
			if( d ) {
				Check( d->get( "severity" ).asString() == "info",
				       "DESIGN_SCALAR_PIPE_UNUSED rides at severity \"info\"" );
				Check( d->get( "message" ).asString().find( "scalar_painter" ) != std::string::npos,
				       "the message carries the same scalar_painter claim the render-result note does" );
				Check( d->get( "message" ).asString().find( "If flat/simple styling is intentional" ) != std::string::npos,
				       "the message carries the self-disarm suffix (sec 9's caveat: no escape-clause "
				       "slot in the diagnostics shape, so it lives in the message)" );
			}
			Check( findDiag( diags, "DESIGN_NO_ADVANCED_GEOMETRY" ) == nullptr,
			       "...and DESIGN_NO_ADVANCED_GEOMETRY stays silent (3 < 4)" );
		}
		std::remove( scenePath.c_str() );
	}

	// -- HEAD form, clean document (kGoodScene: 0 standard_object -- below
	//    either gate) -- no `note`, and no DESIGN_* diagnostics either.
	{
		const std::string scenePath = WriteTemp( "rise_validate_diag_clean.RISEscene", kGoodScene );
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
			       "MONEY: validate's HEAD form OMITS the `note` key entirely (clean document)" );
			const JsonValue& diags = r.get( "diagnostics" );
			Check( findDiag( diags, "DESIGN_SCALAR_PIPE_UNUSED" ) == nullptr &&
			       findDiag( diags, "DESIGN_NO_ADVANCED_GEOMETRY" ) == nullptr,
			       "MONEY: validate's HEAD form carries NO design diagnostics on a clean document "
			       "(0 standard_object -- below either gate)" );
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
		Check( !r.has( "note" ),
		       "MONEY (P2.b): validate's TEXT form carries NO `note` field either" );
		Check( findDiag( r.get( "diagnostics" ), "DESIGN_SCALAR_PIPE_UNUSED" ) != nullptr,
		       "MONEY: validate's TEXT form ALSO carries the design diagnostic on a triggering "
		       "candidate -- the SAME shared conditions scan, no separate implementation to drift" );
	}

	// -- TEXT form, clean candidate -- no `note`, no DESIGN_* diagnostics.
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
		Check( findDiag( r.get( "diagnostics" ), "DESIGN_SCALAR_PIPE_UNUSED" ) == nullptr &&
		       findDiag( r.get( "diagnostics" ), "DESIGN_NO_ADVANCED_GEOMETRY" ) == nullptr,
		       "MONEY: validate's TEXT form carries NO design diagnostics on a clean candidate" );
	}
}

//----------------------------------------------------------------------
// Creative-richness P2.b threshold scan, at the AgentSession::ValidateText
// layer (mirrors RunDesignNoteScanTest's fixtures/structure above, but
// asserts on the AgentDiagnostic vector rather than the note string) --
// proves the SAME six orthogonal fixtures fire the RIGHT diagnostic
// code(s), at Info severity, with the self-disarm suffix, and that the
// shared ComputeDesignNoteConditionsFromDoc_ scan really is shared (the
// note and the diagnostics never disagree on which condition(s) fired).
//----------------------------------------------------------------------
static void RunValidateDesignDiagnosticsScanTest()
{
	std::printf( "[design-diagnostics] AgentSession::ValidateText DESIGN_* red-proofs\n" );

	auto hasCode = []( const std::vector<AgentDiagnostic>& diags, const std::string& code ) {
		for( const AgentDiagnostic& d : diags ) if( d.code == code ) return true;
		return false;
	};
	auto findCode = []( const std::vector<AgentDiagnostic>& diags, const std::string& code ) -> const AgentDiagnostic* {
		for( const AgentDiagnostic& d : diags ) if( d.code == code ) return &d;
		return nullptr;
	};

	const std::string docA3NoScalar =
		"RISE ASCII SCENE 7\n"
		"standard_object\n{\n\tname a\n}\n\n"
		"standard_object\n{\n\tname b\n}\n\n"
		"standard_object\n{\n\tname c\n}\n";
	{
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docA3NoScalar );
		const AgentDiagnostic* d = findCode( diags, "DESIGN_SCALAR_PIPE_UNUSED" );
		Check( d != nullptr, "RED-PROVE A: 3 standard_object + no scalar_painter fires DESIGN_SCALAR_PIPE_UNUSED" );
		if( d ) {
			Check( d->severity == AgentDiagnostic::Severity::Info, "...at Info severity" );
			Check( d->message.find( "scalar_painter" ) != std::string::npos, "...naming scalar_painter" );
			Check( d->message.find( "If flat/simple styling is intentional, this is fine -- ignore." ) != std::string::npos,
			       "...carrying the exact self-disarm suffix" );
			Check( d->offset == 0 && d->length == 0, "...not localizable (offset/length 0/0)" );
		}
		Check( !hasCode( diags, "DESIGN_NO_ADVANCED_GEOMETRY" ), "...and condition B stays silent (3 < 4)" );
	}

	const std::string docA3WithScalar =
		"RISE ASCII SCENE 7\n"
		"standard_object\n{\n\tname a\n}\n\n"
		"standard_object\n{\n\tname b\n}\n\n"
		"standard_object\n{\n\tname c\n}\n\n"
		// Adoption polish item 3: condition A is now BINDING-aware, so a
		// scalar_painter chunk with nothing referencing it no longer
		// silences the note (that WAS the bug -- a decoy chunk used to be
		// enough).  This fixture now genuinely varies a physical-scalar
		// material slot: ggx_material's `alphax` bound to a scalar_painter
		// carrying the "expression" (spatially-varying) form.  DECLARE
		// scalar_painter `r` BEFORE ggx_material `rm` -- DeriveToJob is
		// strict declare-before-use, and a forward reference here left the
		// material's `alphax` unresolved at derive time (review-round P2-1:
		// the CST-level design scan is order-independent so the assertions
		// still passed, but every run spammed an eLog_Error).
		"scalar_painter\n{\n\tname r\n\texpression\tu\n}\n\n"
		"ggx_material\n{\n\tname rm\n\talphax r\n}\n";
	{
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docA3WithScalar );
		Check( !hasCode( diags, "DESIGN_SCALAR_PIPE_UNUSED" ) && !hasCode( diags, "DESIGN_NO_ADVANCED_GEOMETRY" ),
		       "GREEN-PROVE A: the SAME 3 objects + a bound scalar_painter silences BOTH design codes" );
	}

	const std::string docB4AllBoxWithScalar =
		"RISE ASCII SCENE 7\n"
		// Same fix as docA3WithScalar above -- a genuinely-varying binding,
		// not a decoy chunk (adoption polish item 3), declared BEFORE the
		// ggx_material that references it (declare-before-use, P2-1).
		"scalar_painter\n{\n\tname r\n\texpression\tu\n}\n\n"
		"ggx_material\n{\n\tname rm\n\talphax r\n}\n\n"
		"box_geometry\n{\n\tname geo\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n"
		"standard_object\n{\n\tname a\n\tgeometry geo\n}\n\n"
		"standard_object\n{\n\tname b\n\tgeometry geo\n}\n\n"
		"standard_object\n{\n\tname c\n\tgeometry geo\n}\n\n"
		"standard_object\n{\n\tname d\n\tgeometry geo\n}\n";
	{
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docB4AllBoxWithScalar );
		const AgentDiagnostic* d = findCode( diags, "DESIGN_NO_ADVANCED_GEOMETRY" );
		Check( d != nullptr, "RED-PROVE B: 4 standard_object all box_geometry, no advanced geometry, "
		       "fires DESIGN_NO_ADVANCED_GEOMETRY" );
		if( d ) {
			Check( d->severity == AgentDiagnostic::Severity::Info, "...at Info severity" );
			Check( d->message.find( "geometry census" ) != std::string::npos &&
			       d->message.find( "box_geometry" ) != std::string::npos,
			       "...carrying the geometry census clause" );
			Check( d->message.find( "If flat/simple styling is intentional, this is fine -- ignore." ) != std::string::npos,
			       "...carrying the exact self-disarm suffix" );
		}
		Check( !hasCode( diags, "DESIGN_SCALAR_PIPE_UNUSED" ), "...and condition A stays silent (a scalar_painter IS bound)" );
	}

	const std::string docB4WithSdf = docB4AllBoxWithScalar +
		"sdf_geometry\n{\n\tname sdf_geo\n\tpart\t\tsphere union 0 0 0 0 0 0 0 0 0 1 0\n}\n";
	{
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docB4WithSdf );
		Check( !hasCode( diags, "DESIGN_SCALAR_PIPE_UNUSED" ) && !hasCode( diags, "DESIGN_NO_ADVANCED_GEOMETRY" ),
		       "GREEN-PROVE B: the SAME 4-box scene + an sdf_geometry chunk silences BOTH design codes" );
	}

	const std::string docBelowThresholds =
		"RISE ASCII SCENE 7\n"
		"standard_object\n{\n\tname a\n}\n\n"
		"standard_object\n{\n\tname b\n}\n";
	{
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docBelowThresholds );
		Check( !hasCode( diags, "DESIGN_SCALAR_PIPE_UNUSED" ) && !hasCode( diags, "DESIGN_NO_ADVANCED_GEOMETRY" ),
		       "GREEN-PROVE (thresholds): 2 standard_object -- below BOTH gates -- fires neither code" );
	}

	const std::string docCombined =
		"RISE ASCII SCENE 7\n"
		"box_geometry\n{\n\tname geo\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n"
		"standard_object\n{\n\tname a\n\tgeometry geo\n}\n\n"
		"standard_object\n{\n\tname b\n\tgeometry geo\n}\n\n"
		"standard_object\n{\n\tname c\n\tgeometry geo\n}\n\n"
		"standard_object\n{\n\tname d\n\tgeometry geo\n}\n";
	{
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docCombined );
		Check( hasCode( diags, "DESIGN_SCALAR_PIPE_UNUSED" ) && hasCode( diags, "DESIGN_NO_ADVANCED_GEOMETRY" ),
		       "COMBINED: 4 standard_object, no scalar_painter, no advanced geometry -- fires BOTH "
		       "codes as TWO SEPARATE diagnostics" );
		std::size_t designCount = 0;
		for( const AgentDiagnostic& d : diags )
			if( d.code == "DESIGN_SCALAR_PIPE_UNUSED" || d.code == "DESIGN_NO_ADVANCED_GEOMETRY" ) ++designCount;
		Check( designCount == 2, "...exactly two design diagnostics, not a merged/combined one" );
	}

	// Empty document text -> ValidateText reports EMPTY_DOCUMENT and
	// returns early (see AgentSession::ValidateText step (a2)) -- the
	// design scan never even runs, so no DESIGN_* code can appear.
	{
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( std::string() );
		Check( !hasCode( diags, "DESIGN_SCALAR_PIPE_UNUSED" ) && !hasCode( diags, "DESIGN_NO_ADVANCED_GEOMETRY" ),
		       "empty document text -> no design diagnostics (EMPTY_DOCUMENT short-circuits first)" );
	}
}

//----------------------------------------------------------------------
// 88 (2026-08-19) condition C -- DESIGN_HAND_REPEATED_COPIES.
//
// Motivated by a MEASURED miss: a live gemini-3.7-flash run on an
// apothecary-workbench prompt ("each shelf lined with rows of the same
// glass bottle") reused the GEOMETRY -- six standard_objects on one
// `shelf_wares_tall_bottle_geo` -- but hand-authored six chunks instead
// of one `source` + `count_u` chunk.  The gate shipped at FIVE, tuned to
// agree with the skill prose's old "more than about four times" line and
// to catch that six-copy miss with one to spare.
//
// 2026-08-23 RETUNING.  Four live firings of this exact note, across two
// providers, at 5-7 copies -- the apothecary regime this scan was BUILT
// to catch -- were all declined, and the user has ruled every one of
// those declines CORRECT: at a handful of copies, hand-authored VARIETY
// is good authoring (twelve varied apothecary bottles beat twelve
// stamped ones), so a gate that fires there was nagging about the wrong
// problem.  Instancing's genuine regime is LARGE-SCALE REGULAR
// replication -- fence posts, colonnades, rivet rows -- where uniformity
// IS the point.  The gate moved 5 -> 10 (kRepeatedCopyGate, above this
// file's DesignNoteConditions_ scan); `collapse_to_instances`' own floor
// (kCollapseMinGroup == 3, the verb a caller can still invoke by name on
// a small run they HAVE decided to collapse) is untouched -- the two
// constants are independent by construction.
//
// Every fixture below is built to DISCRIMINATE rather than merely pass:
// each non-firing case differs from the firing one in exactly ONE
// respect, so a condition that over-fires (or that silently stops
// firing) fails a named assertion rather than sliding through.  The
// fixtures now carry TWO money assertions instead of one: the classic
// "fires at the gate" shape, AND the new "stays silent below it" shape
// at the exact 5-7 count that used to fire and was ruled a correct
// decline.
//----------------------------------------------------------------------
static void RunDesignRepeatedCopiesScanTest()
{
	std::printf( "[design-note] condition C: DESIGN_HAND_REPEATED_COPIES red-proofs\n" );

	auto hasCode = []( const std::vector<AgentDiagnostic>& diags, const std::string& code ) {
		for( const AgentDiagnostic& d : diags ) if( d.code == code ) return true;
		return false;
	};
	auto findCode = []( const std::vector<AgentDiagnostic>& diags, const std::string& code ) -> const AgentDiagnostic* {
		for( const AgentDiagnostic& d : diags ) if( d.code == code ) return &d;
		return nullptr;
	};

	// The document PREAMBLE every fixture shares: a geometry, a material,
	// and a scalar_painter + sdf_geometry so conditions A and B are both
	// silenced.  That isolation matters -- it means a fixture's note being
	// non-empty can ONLY be condition C.
	const std::string preamble =
		"RISE ASCII SCENE 7\n"
		// Adoption polish item 3: condition A is now BINDING-aware, so an
		// unreferenced decoy scalar_painter no longer silences it -- this
		// preamble now genuinely varies a physical-scalar slot (a
		// ggx_material, unused by any object, whose `alphax` binds to the
		// scalar_painter's spatially-varying "expression" form), declared
		// BEFORE the ggx_material that references it (declare-before-use,
		// review-round P2-1).
		"scalar_painter\n{\n\tname r\n\texpression\tu\n}\n\n"
		"ggx_material\n{\n\tname rm\n\talphax r\n}\n\n"
		"sdf_geometry\n{\n\tname sdf_geo\n\tpart\t\tsphere union 0 0 0 0 0 0 0 0 0 1 0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.6 0.6 0.6\n}\n\n"
		// Materials-realism item 1: condition H is now COVERAGE-aware, so
		// `mat` -- the HERO material bound to every repeated bottle in
		// these fixtures (6-7 objects) -- must itself vary; a flat hero
		// plus one incidental varying anchor elsewhere (the old silencing
		// trick) is now EXACTLY the "coverage still low" case this
		// condition is built to keep firing on.  Kept below
		// kParamErosionLiteralGate (4 distinct literals) so it doesn't ALSO
		// trip condition E, same precaution as `anchors`' doc91_field.
		"expression_painter\n{\n\tname mat_field\n\texpr fbm(P*3.0,3,0.5,2.0)\n}\n\n"
		"lambertian_material\n{\n\tname mat\n\treflectance mat_field\n}\n\n"
		"lambertian_material\n{\n\tname mat2\n\treflectance pnt\n}\n\n"
		"sphere_geometry\n{\n\tname bottle_geo\n\tradius 0.2\n}\n\n"
		// RELIEF_MODIFIER_DESIGN sec 9 (2026-09-06), condition Q: every
		// `bottle` below (and `procedural_anchor` further down) binds `mat`
		// (a genuinely varying `reflectance`) to real geometry with no
		// `modifier` -- otherwise a DESIGN_FLAT_RELIEF candidate, which would
		// break every "note goes fully empty" assertion this whole fixture
		// family relies on.  ONE relief_modifier, built on the SAME
		// `mat_field` (not a decoy), bound identically on every bottle
		// object -- an IDENTICAL extra param on every one leaves condition
		// C's binding-signature grouping unchanged.
		"scalar_painter\n{\n\tname mat_field_h\n\tpainter mat_field\n\tchannel R\n}\n\n"
		"relief_modifier\n{\n\tname mat_relief\n\theight mat_field_h\n\tscale 0.02\n}\n\n";

	// One `standard_object` bound to bottle_geo/mat at x = `x`.
	auto bottle = []( const std::string& name, const std::string& x ) {
		return "standard_object\n{\n\tname " + name + "\n\tgeometry bottle_geo\n\tmaterial mat\n"
		       "\tmodifier mat_relief\n"
		       "\tposition " + x + " 0 0\n}\n\n";
	};

	// The anchor chunks that keep conditions G (DESIGN_UNBOUND_MATERIAL)
	// and H (DESIGN_FLAT_ALBEDO) silent -- doc 91's isolation trio, now
	// factored out since more fixtures below need full note-emptiness
	// (not just "condition C absent") than the original 5-gate version
	// did.  Two bind `rm` and `mat2` (both otherwise unreferenced),
	// silencing G; the third binds a PROCEDURAL colour painter, silencing
	// H -- without it every colour slot in these fixtures (`mat`/`mat2`'s
	// `reflectance`, both bound to the constant `pnt`) would be a flat
	// constant.  Each anchor's OWN binding signature is unique (different
	// `material`), so none joins the `bottle_geo`/`mat` group condition C
	// measures.
	const std::string anchors =
		"standard_object\n{\n\tname rm_anchor\n\tgeometry bottle_geo\n\tmaterial rm\n}\n\n"
		"standard_object\n{\n\tname mat2_anchor\n\tgeometry bottle_geo\n\tmaterial mat2\n}\n\n"
		// Deliberately only 4 distinct numeric literals in the body (below
		// kParamErosionLiteralGate == 6) so this does not ALSO trip
		// DESIGN_PARAM_METADATA_EROSION -- that condition is orthogonal to
		// what these fixtures are testing.
		"expression_painter\n{\n\tname doc91_field\n\texpr fbm(P*4.0,4,0.5,2.0)\n}\n\n"
		"lambertian_material\n{\n\tname doc91_procedural_mat\n\treflectance doc91_field\n}\n\n"
		// Condition Q (see the `preamble`'s own comment above): this anchor's
		// varying `reflectance` on real geometry needs the SAME silencing --
		// reuses `mat_relief` (bound on an unrelated field is fine; Q only
		// asks whether the object's `modifier` slot resolves to anything).
		"standard_object\n{\n\tname procedural_anchor\n\tgeometry bottle_geo\n"
		"\tmaterial doc91_procedural_mat\n\tmodifier mat_relief\n}\n\n";

	// -- (1) THE NEW MONEY ASSERTION: SILENT at 6 and 7 copies -- the
	//    apothecary regime.  This is exactly the shape that used to fire
	//    (the measured miss this scan was built from was six copies) and
	//    was declined four times, correctly, per the user's ruling above.
	//    Manually red-proved during this retuning by reverting
	//    kRepeatedCopyGate to 5 and re-running: the 7-copy case below
	//    FAILS (fires) at gate 5 and PASSES (silent) at gate 10 -- see the
	//    session report for the exact counts both ways.
	{
		std::string docSix = preamble + anchors;
		for( int i = 0; i < 6; ++i ) docSix += bottle( "b" + std::to_string( i ), std::to_string( i ) );
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docSix );
		Check( !hasCode( diags, "DESIGN_HAND_REPEATED_COPIES" ),
		       "SILENT-PROVE C (apothecary, 6): six standard_objects on ONE geometry, transform-only "
		       "differences, no source/count_u -- stays SILENT now (gate 10) where it used to fire at "
		       "gate 5; this is the declined-firing shape the user ruled a correct decline" );
		Check( AgentSession::ComputeDesignNote( docSix ).empty(),
		       "...and the render-result note goes FULLY empty (A/B/G/H are silenced by the preamble "
		       "and the anchors, so this proves C alone, not an accidental empty note)" );
	}
	{
		std::string docSeven = preamble + anchors;
		for( int i = 0; i < 7; ++i ) docSeven += bottle( "b" + std::to_string( i ), std::to_string( i ) );
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docSeven );
		Check( !hasCode( diags, "DESIGN_HAND_REPEATED_COPIES" ),
		       "SILENT-PROVE C (apothecary, 7): SEVEN copies -- one more than the old gate of 5 -- also "
		       "stay silent under gate 10, the same declined-and-ruled-correct regime" );
		Check( AgentSession::ComputeDesignNote( docSeven ).empty(),
		       "...note fully empty here too" );
	}

	// -- (2) BOUNDARY, exact, from both sides on one fixture: NINE stays
	//    silent, the TENTH copy fires.  This replaces the old 4/5
	//    boundary pair (Recipe 2's four-legged table sat one below the
	//    old gate of 5; a nine-copy run now sits one below the new gate
	//    of 10 for the identical reason).
	std::string docNine = preamble + anchors;
	for( int i = 0; i < 9; ++i ) docNine += bottle( "b" + std::to_string( i ), std::to_string( i ) );
	{
		Check( !hasCode( AgentSession::ValidateText( docNine ), "DESIGN_HAND_REPEATED_COPIES" ),
		       "BOUNDARY C: NINE copies stay silent (gate is 10) -- one below the line, same as Recipe "
		       "2's four legs sat one below the old gate of 5" );
	}
	const std::string docTen = docNine + bottle( "b9", "9" );
	{
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docTen );
		const AgentDiagnostic* d = findCode( diags, "DESIGN_HAND_REPEATED_COPIES" );
		Check( d != nullptr,
		       "BOUNDARY C: the SAME document plus a TENTH copy fires -- the gate is 10, proven from "
		       "both sides on one fixture" );
		if( d ) {
			Check( d->severity == AgentDiagnostic::Severity::Info, "...at Info severity (advisory, like A and B)" );
			Check( d->message.find( "10 standard_objects" ) != std::string::npos,
			       "...reporting the actual group SIZE (10), not a generic count" );
			Check( d->message.find( "`bottle_geo`" ) != std::string::npos,
			       "...NAMING the shared geometry, so the author knows which run is meant" );
			Check( d->message.find( "`source <that one>`" ) != std::string::npos &&
			       d->message.find( "count_u 9" ) != std::string::npos,
			       "...pricing the alternative BY NAME (`source` + `count_u 9` -- one fewer than 10, "
			       "because the source still renders)" );
			Check( d->message.find( "COPIES rather than moves or hides" ) != std::string::npos &&
			       d->message.find( "are 10, not 9" ) != std::string::npos,
			       "...teaching the off-by-one trap the section teaches (`source` copies, so count_u 9 "
			       "off a visible source yields TEN)" );
			Check( d->message.find( "This is what `source` + `count_u` is FOR" ) != std::string::npos &&
			       d->message.find( "a handful of deliberately varied siblings is good authoring, not a smell" )
			           != std::string::npos,
			       "...MONEY (the 2026-08-23 scoping sentence): the clause itself now says a large "
			       "regular run is what the verb is for, and a handful of varied siblings is not a smell" );
			Check( d->message.find( "this is fine -- ignore and do not churn" ) != std::string::npos,
			       "...self-disarming, the same advisory discipline conditions A and B follow" );
		}
		// A and B are silenced by the preamble, so a non-empty note here is
		// condition C and nothing else.
		const std::string note = AgentSession::ComputeDesignNote( docTen );
		Check( note.find( "no physical-scalar material parameter" ) == std::string::npos &&
		       note.find( "geometry census" ) == std::string::npos,
		       "...with conditions A and B provably silent (the fixture binds both a scalar_painter "
		       "and an sdf_geometry), so the note below is condition C in isolation" );
		Check( note.find( "DESIGN NOTE" ) != std::string::npos &&
		       note.find( "10 standard_objects" ) != std::string::npos,
		       "...and the RENDER-RESULT note carrier fires on the same document" );

		// THE VERBATIM-COPY INVARIANT (the note clause and the diagnostic
		// message must never be able to disagree).  Condition C keeps it
		// STRUCTURALLY -- one shared FormatRepeatedCopiesClause_, two
		// callers -- and this asserts the observable consequence: the
		// diagnostic's whole message appears, byte for byte, inside the note.
		if( d ) {
			Check( note.find( d->message ) != std::string::npos,
			       "MONEY (verbatim invariant): the DESIGN_HAND_REPEATED_COPIES message appears "
			       "BYTE-IDENTICALLY inside the render-result note -- one shared clause formatter, "
			       "so note and diagnostic cannot drift" );
		}
	}

	// -- (3) GREEN-PROVE (already-instanced disarm, UNCHANGED SHAPE): the
	//    SAME ten objects, but the repetition is already expressed with
	//    source/count_u.  Firing here would be actively wrong -- the
	//    author did the right thing.  Built on docTen (the fires-fixture)
	//    now that docSix/docSeven no longer fire on their own.
	{
		const std::string docInstanced = docTen +
			"standard_object\n{\n\tname shelf_row\n\tsource b0\n\tcount_u 3\n\tposition expr(10+i) 1 0\n}\n";
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docInstanced );
		Check( !hasCode( diags, "DESIGN_HAND_REPEATED_COPIES" ),
		       "GREEN-PROVE C (already instanced): the SAME 10-copy run plus ONE source/count_u chunk "
		       "silences condition C -- a note there would price an idiom the author already used" );
		Check( AgentSession::ComputeDesignNote( docInstanced ).empty(),
		       "...and the note goes fully empty (A and B were already silent)" );
	}

	// -- (3b) ...and `source` ALONE (no counts) disarms it too: a single
	//    instance is still proof the author has the idiom in hand.
	{
		const std::string docSourceOnly = docTen +
			"standard_object\n{\n\tname shelf_copy\n\tsource b0\n\tposition 0 1 0\n}\n";
		Check( !hasCode( AgentSession::ValidateText( docSourceOnly ), "DESIGN_HAND_REPEATED_COPIES" ),
		       "GREEN-PROVE C: a bare `source` (no count_u) disarms condition C as well" );
	}

	// -- (4) GREEN-PROVE (non-reference-param silence, UNCHANGED SHAPE):
	//    eighteen objects on one geometry that differ in more than
	//    placement (two materials, nine each) are NOT one array -- an
	//    instancing chunk copies bindings wholesale, so it could not have
	//    written them.  Nine-and-nine is deliberate: EACH side sits one
	//    below the new gate of 10 (the same boundary fact as (2) above),
	//    while the MERGED total (18) would clear it -- so this proves the
	//    split is by binding, not merely both sides being small.
	{
		std::string docMixedBindings = preamble;
		for( int i = 0; i < 18; ++i ) {
			docMixedBindings += "standard_object\n{\n\tname m" + std::to_string( i ) +
				"\n\tgeometry bottle_geo\n\tmaterial " + ( ( i % 2 ) ? "mat2" : "mat" ) +
				"\n\tposition " + std::to_string( i ) + " 0 0\n}\n\n";
		}
		Check( !hasCode( AgentSession::ValidateText( docMixedBindings ), "DESIGN_HAND_REPEATED_COPIES" ),
		       "GREEN-PROVE C (bindings): 18 objects on one geometry split 9/9 across TWO materials "
		       "stay silent -- neither group reaches 10 even though the merged total (18) would, and "
		       "one `source` chunk could not have produced both" );
	}

	// -- (5) GREEN-PROVE (containers, UNCHANGED SHAPE): geometry-less
	//    CONTAINERS never enter a group.  This is the harness's own
	//    machine-minted `<prefix>element_root` shape (AgentSession::
	//    ElementRootName mints exactly `standard_object { name
	//    <prefix>element_root }`), and the trajectory that motivated this
	//    slice carried 93 such links -- so a condition that counted them
	//    would fire on every staged build.  CONFIRMED here, not assumed.
	//
	//    The containers below each carry a DISTINCT `position`, which is
	//    not decoration: `place_element` writes exactly one transform onto
	//    each element root, so in a real staged build the roots are a run
	//    of same-binding, different-transform nodes -- the shape condition
	//    C looks for in every respect EXCEPT the missing `geometry`.  A
	//    fixture without those positions passes for the wrong reason (the
	//    co-located rule catches it instead) and would keep passing with
	//    the container rule deleted; this one does not.
	{
		std::string docRoots = preamble;
		const char* const roots[] = { "shelf", "bench", "wall", "props", "glass", "lamp" };
		for( int i = 0; i < 6; ++i )
			docRoots += std::string( "standard_object\n{\n\tname " ) + roots[i] +
				"_element_root\n\tposition " + std::to_string( i ) + " 0 0\n}\n\n";
		Check( !hasCode( AgentSession::ValidateText( docRoots ), "DESIGN_HAND_REPEATED_COPIES" ),
		       "GREEN-PROVE C (element roots): SIX geometry-less `<prefix>element_root` containers, "
		       "each posed by its own `place_element` transform, stay silent -- a container carries "
		       "no `geometry`, so it never enters a group" );
		// ...and the SAME document with TEN real, geometry-bearing copies
		// added DOES fire (ten, not six, now that the gate is 10), proving
		// the silence above is the container rule and not the fixture
		// failing to reach the scan at all.
		std::string docRootsPlusCopies = docRoots;
		for( int i = 0; i < 10; ++i ) docRootsPlusCopies += bottle( "b" + std::to_string( i ), std::to_string( i ) );
		Check( hasCode( AgentSession::ValidateText( docRootsPlusCopies ), "DESIGN_HAND_REPEATED_COPIES" ),
		       "...while the SAME six containers plus ten geometry-bearing copies DO fire -- the "
		       "silence above is the container rule, not a dead scan" );
	}

	// -- (6) GREEN-PROVE (co-located, UNCHANGED SHAPE but bumped past the
	//    gate): TEN co-located, byte-identical copies are a duplication
	//    BUG, not an array.  The clause claims they "differ only in their
	//    transform"; with no distinct transform at all that claim would be
	//    false, so the condition must not make it.  Bumped from 6 to 10 (at
	//    or past the new gate) so this proves the co-located rule itself
	//    stays silent even where the count alone would otherwise fire --
	//    not merely that ten happens to be under some other threshold.
	{
		std::string docStacked = preamble;
		for( int i = 0; i < 10; ++i )
			docStacked += "standard_object\n{\n\tname s" + std::to_string( i ) +
				"\n\tgeometry bottle_geo\n\tmaterial mat\n}\n\n";
		Check( !hasCode( AgentSession::ValidateText( docStacked ), "DESIGN_HAND_REPEATED_COPIES" ),
		       "GREEN-PROVE C (co-located): 10 copies with NO transform between them stay silent -- "
		       "that is a duplication bug, and `count_u` is advice about a different problem" );
	}

	// -- (7) Whitespace/indentation must not split a group: the signature
	//    compares BINDINGS, not bytes.  Ten copies (bumped from 6 to clear
	//    the new gate), wildly different formatting, still one group.
	{
		const std::string docFormatting = preamble +
			"standard_object{\nname w0\ngeometry bottle_geo\nmaterial mat\nposition 0 0 0\n}\n\n"
			"standard_object\n{\n\t\tname w1\n\t\tgeometry   bottle_geo\n\t\tmaterial\tmat\n\t\tposition 1 0 0\n}\n\n"
			"standard_object\n{\n name w2\n geometry bottle_geo\n material mat\n position 2 0 0\n}\n\n"
			"standard_object\n{\n\tname w3\n\tgeometry bottle_geo\n\tmaterial mat\n\tposition 3 0 0\n}\n\n"
			"standard_object\n{\n\tname w4\n\tgeometry bottle_geo\n\tmaterial mat\n\tposition 4 0 0\n}\n\n"
			"standard_object\n{\n\tname w5\n\tgeometry bottle_geo\n\tmaterial mat\n\tposition 5 0 0\n}\n\n"
			"standard_object{\nname w6\ngeometry bottle_geo\nmaterial mat\nposition 6 0 0\n}\n\n"
			"standard_object\n{\n\t\tname w7\n\t\tgeometry   bottle_geo\n\t\tmaterial\tmat\n\t\tposition 7 0 0\n}\n\n"
			"standard_object\n{\n name w8\n geometry bottle_geo\n material mat\n position 8 0 0\n}\n\n"
			"standard_object\n{\n\tname w9\n\tgeometry bottle_geo\n\tmaterial mat\n\tposition 9 0 0\n}\n";
		Check( hasCode( AgentSession::ValidateText( docFormatting ), "DESIGN_HAND_REPEATED_COPIES" ),
		       "C (signature): ten copies formatted many different ways are still ONE group -- the "
		       "signature reads pvalue tokens, never raw bytes" );
	}

	// -- (8) The three conditions are INDEPENDENT diagnostics, not a merged
	//    one: a document that trips all three carries three entries.  Ten
	//    boxes (bumped from 6 to clear the new gate).
	{
		std::string docAll = "RISE ASCII SCENE 7\n"
			"box_geometry\n{\n\tname geo\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n";
		for( int i = 0; i < 10; ++i )
			docAll += "standard_object\n{\n\tname a" + std::to_string( i ) +
				"\n\tgeometry geo\n\tposition " + std::to_string( i ) + " 0 0\n}\n\n";
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docAll );
		Check( hasCode( diags, "DESIGN_SCALAR_PIPE_UNUSED" ) && hasCode( diags, "DESIGN_NO_ADVANCED_GEOMETRY" ) &&
		       hasCode( diags, "DESIGN_HAND_REPEATED_COPIES" ),
		       "COMBINED: a 10-box fan-out with no scalar_painter and no advanced geometry fires ALL "
		       "THREE design codes as three separate diagnostics" );
	}
}

//----------------------------------------------------------------------
// "Adoption polish" (2026-08-21), motivated by a Gemini trajectory
// analysis: three diagnostic-surface changes on the SAME shared
// ComputeDesignNoteConditionsFromDoc_ scan.
//
//   Item 1 -- DESIGN_PARAM_METADATA_EROSION: an expression_painter /
//     scalar_painter{expression} chunk whose body carries many distinct
//     numeric literals but declares (almost) no `param` lines.
//   Item 2 -- DESIGN_ORPHANED_PAINTERS: a Painter/Function chunk nothing
//     in the document references, computed from SceneReferenceGraph's
//     FULL edge list (so an environment/`radiance_map` referrer counts).
//   Item 3 -- DESIGN_SCALAR_PIPE_UNUSED fixed to be BINDING-aware: it
//     used to fire on every scene whose roughness varies via
//     pbr_metallic_roughness_material's COLOUR-pipe `roughness` slot
//     (no `scalar_painter` keyword in sight, so the old grep always
//     missed it) -- 11/11 live-census false fires.
//----------------------------------------------------------------------
static void RunAdoptionPolishScanTest()
{
	std::printf( "[design-note] adoption polish: DESIGN_PARAM_METADATA_EROSION / "
	             "DESIGN_ORPHANED_PAINTERS / binding-aware DESIGN_SCALAR_PIPE_UNUSED\n" );

	auto hasCode = []( const std::vector<AgentDiagnostic>& diags, const std::string& code ) {
		for( const AgentDiagnostic& d : diags ) if( d.code == code ) return true;
		return false;
	};
	auto findCode = []( const std::vector<AgentDiagnostic>& diags, const std::string& code ) -> const AgentDiagnostic* {
		for( const AgentDiagnostic& d : diags ) if( d.code == code ) return &d;
		return nullptr;
	};

	//--------------------------------------------------------------
	// Item 3: condition A must NOT fire on a scene whose only spatial
	// roughness variation is pbr's COLOUR-pipe `roughness` slot bound to
	// an expression_painter -- no `scalar_painter` chunk anywhere.  THE
	// bug this fixes: the OLD keyword-grep fired here on 11/11 live
	// census renders.
	//--------------------------------------------------------------
	{
		const std::string docPbrColourPipeVaries =
			"RISE ASCII SCENE 7\n"
			"expression_painter\n{\n\tname rough_field\n\tparam base 0.4 min 0 max 1 step 0.01 label \"Base roughness\"\n"
			"\tdef n fbm(P*4.0,4,0.5,2.0)\n\texpr clamp(base+n*0.2,0.05,0.95)\n}\n\n"
			"pbr_metallic_roughness_material\n{\n\tname m\n\tbase_color rough_field\n\troughness rough_field\n}\n\n"
			"sphere_geometry\n{\n\tname geo\n\tradius 0.7\n}\n\n"
			"standard_object\n{\n\tname a\n\tgeometry geo\n\tmaterial m\n}\n\n"
			"standard_object\n{\n\tname b\n\tgeometry geo\n\tmaterial m\n}\n\n"
			"standard_object\n{\n\tname c\n\tgeometry geo\n\tmaterial m\n}\n";
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docPbrColourPipeVaries );
		Check( !hasCode( diags, "DESIGN_SCALAR_PIPE_UNUSED" ),
		       "ITEM 3 GREEN-PROVE: pbr roughness varying via expression_painter (Color-pipe-by-"
		       "construction, no scalar_painter chunk anywhere) silences DESIGN_SCALAR_PIPE_UNUSED -- "
		       "this is the exact false-fire the old keyword-grep hit 11/11 times" );
	}
	{
		const std::string docPbrAllConstant =
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\n\tname c\n\tcolor 0.7 0.6 0.5\n}\n\n"
			"pbr_metallic_roughness_material\n{\n\tname m\n\tbase_color c\n\troughness 0.4\n}\n\n"
			"sphere_geometry\n{\n\tname geo\n\tradius 0.7\n}\n\n"
			"standard_object\n{\n\tname a\n\tgeometry geo\n\tmaterial m\n}\n\n"
			"standard_object\n{\n\tname b\n\tgeometry geo\n\tmaterial m\n}\n\n"
			"standard_object\n{\n\tname c2\n\tgeometry geo\n\tmaterial m\n}\n";
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docPbrAllConstant );
		const AgentDiagnostic* d = findCode( diags, "DESIGN_SCALAR_PIPE_UNUSED" );
		Check( d != nullptr,
		       "ITEM 3 RED-PROVE: the SAME pbr material with `roughness 0.4` (a bare constant, no "
		       "expression_painter anywhere) DOES fire DESIGN_SCALAR_PIPE_UNUSED -- the fix silences "
		       "the false POSITIVE, it does not silence the check itself" );
		if( d ) Check( d->message.find( "no physical-scalar material parameter" ) != std::string::npos,
		               "...carrying the corrected (non-stale) clause text" );
	}

	//--------------------------------------------------------------
	// Item 1: DESIGN_PARAM_METADATA_EROSION.
	//--------------------------------------------------------------
	{
		// Shaped like the live-census floor run's pnt_shelf_wear_mask_v3:
		// 0 `param` lines, 3 `def`s, one `expr`, ~14 distinct numeric
		// literals scattered across smoothstep/fbm calls.
		const std::string docEroded =
			"RISE ASCII SCENE 7\n"
			"expression_painter\n{\n\tname pnt_shelf_wear_mask_v3\n"
			"\tdef edge_dist smoothstep(0.02, 0.12, P.z)\n"
			"\tdef chipping fbm(P * vec3(120.0, 50.0, 120.0) + vec3(11.0, 0, 7.0), 4, 0.55, 2.0)\n"
			"\tdef wear edge_dist * 0.82 + chipping * 0.38\n"
			"\texpr smoothstep(0.40, 0.58, wear)\n}\n";
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docEroded );
		const AgentDiagnostic* d = findCode( diags, "DESIGN_PARAM_METADATA_EROSION" );
		Check( d != nullptr,
		       "ITEM 1 RED-PROVE: 0-param expression_painter with ~14 distinct literals across its "
		       "defs+expr fires DESIGN_PARAM_METADATA_EROSION" );
		if( d ) {
			Check( d->severity == AgentDiagnostic::Severity::Info, "...at Info severity" );
			Check( d->message.find( "`pnt_shelf_wear_mask_v3`" ) != std::string::npos,
			       "...NAMING the eroded chunk" );
			Check( d->message.find( "param <name> <value>" ) != std::string::npos,
			       "...teaching the promotion mechanism" );
		}
	}
	{
		// The calibration exemplar: a fully-annotated body (matching the
		// scenes/Tests/GUI/panel_stress_params.RISEscene / FeatureBased/
		// Textures authoring convention) must NOT fire even with a
		// comparable literal count.
		const std::string docNotEroded =
			"RISE ASCII SCENE 7\n"
			"expression_painter\n{\n\tname pnt_shelf_wear_mask_v3\n"
			"\tparam edge_lo 0.02 min 0 max 1 step 0.01 label \"Edge low\"\n"
			"\tparam edge_hi 0.12 min 0 max 1 step 0.01 label \"Edge high\"\n"
			"\tdef edge_dist smoothstep(edge_lo, edge_hi, P.z)\n"
			"\tdef chipping fbm(P * vec3(120.0, 50.0, 120.0) + vec3(11.0, 0, 7.0), 4, 0.55, 2.0)\n"
			"\tdef wear edge_dist * 0.82 + chipping * 0.38\n"
			"\texpr smoothstep(0.40, 0.58, wear)\n}\n";
		Check( !hasCode( AgentSession::ValidateText( docNotEroded ), "DESIGN_PARAM_METADATA_EROSION" ),
		       "ITEM 1 GREEN-PROVE: the SAME body-shaped chunk with 2 real `param` lines (already "
		       "above kParamErosionMaxParams) stays silent -- real params disarm the note" );
	}
	{
		// Review-round P2-2's boundary pin: kParamErosionMaxParams == 1
		// means EXACTLY one `param` line still QUALIFIES as a candidate
		// (the gate that disarms is `occurrences > kParamErosionMaxParams`,
		// i.e. TWO or more) -- one named param is a start, not proof the
		// author is done.  Same body-shape as docNotEroded above but with
		// only ONE `param` line, so it must still fire.
		const std::string docOneParamStillFires =
			"RISE ASCII SCENE 7\n"
			"expression_painter\n{\n\tname pnt_shelf_wear_mask_v3\n"
			"\tparam edge_lo 0.02 min 0 max 1 step 0.01 label \"Edge low\"\n"
			"\tdef edge_dist smoothstep(edge_lo, 0.12, P.z)\n"
			"\tdef chipping fbm(P * vec3(120.0, 50.0, 120.0) + vec3(11.0, 0, 7.0), 4, 0.55, 2.0)\n"
			"\tdef wear edge_dist * 0.82 + chipping * 0.38\n"
			"\texpr smoothstep(0.40, 0.58, wear)\n}\n";
		Check( hasCode( AgentSession::ValidateText( docOneParamStillFires ), "DESIGN_PARAM_METADATA_EROSION" ),
		       "ITEM 1 BOUNDARY: exactly ONE `param` line (== kParamErosionMaxParams) still fires -- "
		       "only TWO OR MORE param lines disarm the note (review-round P2-2)" );
	}
	{
		// Bounded-list formatting: more than 3 eroded chunks lists 3 +
		// "and N more".
		std::string docManyEroded = "RISE ASCII SCENE 7\n";
		for( int i = 0; i < 5; ++i )
			docManyEroded += "expression_painter\n{\n\tname erode" + std::to_string( i ) +
				"\n\tdef n fbm(P*4.0,4,0.5,2.0)\n"
				"\texpr clamp(0.1+n*0.83, 0.02, 0.97)+1.5+2.5+3.5+4.5\n}\n\n";
		// NOTE: `diags` MUST be a named local, not a temporary bound
		// directly into findCode's argument -- ValidateText returns
		// std::vector<AgentDiagnostic> BY VALUE, and `d` below outlives
		// the single full-expression a temporary would have been alive
		// for.
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docManyEroded );
		const AgentDiagnostic* d = findCode( diags, "DESIGN_PARAM_METADATA_EROSION" );
		Check( d != nullptr, "bounded-list fixture: 5 eroded chunks fires the code at all" );
		if( d ) {
			Check( d->message.find( "5 expression chunks" ) != std::string::npos,
			       "...reports the TRUE total count (5), not the truncated display count" );
			Check( d->message.find( "and 2 more" ) != std::string::npos,
			       "...bounded list shows 3 named chunks + \"and 2 more\"" );
		}
	}

	//--------------------------------------------------------------
	// Item 2: DESIGN_ORPHANED_PAINTERS.
	//--------------------------------------------------------------
	{
		const std::string docOrphan =
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\n\tname orphaned_v2\n\tcolor 0.5 0.5 0.5\n}\n\n"
			"uniformcolor_painter\n{\n\tname used\n\tcolor 0.4 0.4 0.4\n}\n\n"
			"lambertian_material\n{\n\tname m\n\treflectance used\n}\n";
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docOrphan );
		const AgentDiagnostic* d = findCode( diags, "DESIGN_ORPHANED_PAINTERS" );
		Check( d != nullptr,
		       "ITEM 2 RED-PROVE: a painter chunk nothing references fires DESIGN_ORPHANED_PAINTERS" );
		if( d ) {
			Check( d->severity == AgentDiagnostic::Severity::Info, "...at Info severity" );
			Check( d->message.find( "`orphaned_v2`" ) != std::string::npos, "...NAMING the orphan" );
			Check( d->message.find( "used" ) == std::string::npos ||
			       d->message.find( "`used`" ) == std::string::npos,
			       "...NOT naming the referenced painter (`used`)" );
			Check( d->message.find( "remove_chunk" ) != std::string::npos,
			       "...naming the cleanup call" );
		}
	}
	{
		const std::string docNoOrphan =
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\n\tname used\n\tcolor 0.4 0.4 0.4\n}\n\n"
			"lambertian_material\n{\n\tname m\n\treflectance used\n}\n";
		Check( !hasCode( AgentSession::ValidateText( docNoOrphan ), "DESIGN_ORPHANED_PAINTERS" ),
		       "ITEM 2 GREEN-PROVE: a painter referenced by a material stays silent" );
	}
	{
		// THE environment case the brief calls out by name: a painter
		// referenced ONLY by a rasterizer's `radiance_map` (never by any
		// Painter/Material chunk) must NOT be flagged -- proves condition
		// F reads SceneReferenceGraph's FULL, unfiltered edge list (every
		// referrer category), not the node-graph canvas's own narrower
		// PainterMaterialGraph (whose edge-seeding only attributes a
		// Painter/Material/promoted-Function referrer, and would miss
		// this exact binding).
		const std::string docEnvReferenced =
			"RISE ASCII SCENE 7\n"
			"hdr_painter\n{\n\tname env_dome\n\tfile none\n}\n\n"
			"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n"
			"\tradiance_map env_dome\n}\n";
		Check( !hasCode( AgentSession::ValidateText( docEnvReferenced ), "DESIGN_ORPHANED_PAINTERS" ),
		       "ITEM 2 GREEN-PROVE (environment): a painter bound only as the rasterizer's "
		       "`radiance_map` is NOT flagged as orphaned" );
	}
}

//----------------------------------------------------------------------
// Doc 91 (2026-08-23), the same shared ComputeDesignNoteConditionsFromDoc_
// scan, two more conditions:
//
//   Condition G -- DESIGN_UNBOUND_MATERIAL: a Material-category chunk NO
//     standard_object (or other geometry-bearing chunk) references.
//     Suppressed while a build-protocol element is actively mid-build
//     (`inPiecesPhase`) -- a material authored before the object that
//     will bind it is normal there.
//   Condition H -- DESIGN_FLAT_ALBEDO: every colour-carrying material
//     slot (base_color / reflectance / ..., enumerated from the
//     descriptors, not a hand list) is a flat uniformcolor_painter, at
//     the SAME >=3-object threshold condition A uses.
//
// Fixtures are deliberately NON-CREATURE (a still-life: a lamp base and
// shade) -- the point of this slice is that neither condition is keyed
// to any subject, so a furniture/still-life scene is what proves that
// rather than undermines it.
//----------------------------------------------------------------------
static void RunUnboundMaterialAndFlatAlbedoScanTest()
{
	std::printf( "[design-note] doc 91: DESIGN_UNBOUND_MATERIAL / DESIGN_FLAT_ALBEDO\n" );

	auto hasCode = []( const std::vector<AgentDiagnostic>& diags, const std::string& code ) {
		for( const AgentDiagnostic& d : diags ) if( d.code == code ) return true;
		return false;
	};
	auto findCode = []( const std::vector<AgentDiagnostic>& diags, const std::string& code ) -> const AgentDiagnostic* {
		for( const AgentDiagnostic& d : diags ) if( d.code == code ) return &d;
		return nullptr;
	};

	//--------------------------------------------------------------
	// Condition G: DESIGN_UNBOUND_MATERIAL.
	//--------------------------------------------------------------
	// A still-life lamp base: one bound material, one authored-then-
	// never-attached material (`unused_shade_mat`) -- the general shape
	// of the dragon-run gap (a fourth palette material never bound to
	// anything), on a fixture with nothing creature-specific about it.
	const std::string docLampUnboundMat =
		"RISE ASCII SCENE 7\n"
		"uniformcolor_painter\n{\n\tname base_pnt\n\tcolor 0.55 0.5 0.45\n}\n\n"
		"lambertian_material\n{\n\tname base_mat\n\treflectance base_pnt\n}\n\n"
		"lambertian_material\n{\n\tname unused_shade_mat\n\treflectance base_pnt\n}\n\n"
		"cylinder_geometry\n{\n\tname base_geo\n\tradius 0.3\n\theight 0.15\n}\n\n"
		"standard_object\n{\n\tname base_obj\n\tgeometry base_geo\n\tmaterial base_mat\n}\n";
	{
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docLampUnboundMat );
		const AgentDiagnostic* d = findCode( diags, "DESIGN_UNBOUND_MATERIAL" );
		Check( d != nullptr,
		       "CONDITION G RED-PROVE: a material chunk no standard_object references fires "
		       "DESIGN_UNBOUND_MATERIAL" );
		if( d ) {
			Check( d->severity == AgentDiagnostic::Severity::Info, "...at Info severity" );
			Check( d->message.find( "`unused_shade_mat`" ) != std::string::npos,
			       "...NAMING the unbound material" );
			Check( d->message.find( "`base_mat`" ) == std::string::npos,
			       "...NOT naming the bound material" );
			Check( d->message.find( "material <name>" ) != std::string::npos,
			       "...stating the FIRST honest fix (bind it)" );
			Check( d->message.find( "remove_chunk" ) != std::string::npos,
			       "...stating the SECOND honest fix (remove it)" );

			// THE VERBATIM-COPY INVARIANT: condition G carries no
			// kSelfDisarm suffix of its own (its shared clause states its
			// own escape, the condition C/D/E/F precedent), so its whole
			// diagnostic message must appear BYTE-IDENTICALLY inside the
			// render-result note -- one shared FormatUnboundMaterialClause_,
			// two callers, so they cannot drift.
			const std::string note = AgentSession::ComputeDesignNote( docLampUnboundMat );
			Check( note.find( d->message ) != std::string::npos,
			       "MONEY (verbatim invariant): the DESIGN_UNBOUND_MATERIAL message appears "
			       "BYTE-IDENTICALLY inside the render-result note" );
		}
	}
	// GREEN-PROVE: the SAME material, now bound to a second object -- must
	// go silent.
	{
		const std::string docBound = docLampUnboundMat +
			"standard_object\n{\n\tname shade_obj\n\tgeometry base_geo\n\tmaterial unused_shade_mat\n}\n";
		Check( !hasCode( AgentSession::ValidateText( docBound ), "DESIGN_UNBOUND_MATERIAL" ),
		       "CONDITION G GREEN-PROVE: binding the material to an object silences the note" );
	}
	// PHASE GATE: the identical unbound-material fixture, but with the
	// caller declaring it is mid-build (`inPiecesPhase = true`) -- a
	// material authored before the object that will bind it is normal
	// there, so the note must stay silent even though the underlying fact
	// (a zero-referrer material) is unchanged.
	{
		Check( !hasCode( AgentSession::ValidateText( docLampUnboundMat, /*inPiecesPhase=*/true ),
		                 "DESIGN_UNBOUND_MATERIAL" ),
		       "CONDITION G PHASE GATE: the SAME unbound-material fixture stays silent when the "
		       "caller reports an element actively mid-build (Pieces phase) -- authored-before-bound "
		       "is normal there" );
		// ...and restoring inPiecesPhase=false (ValidateText's default) is
		// what makes it fire again -- proves the gate is the phase
		// argument, not some other difference between the two calls.
		Check( hasCode( AgentSession::ValidateText( docLampUnboundMat ), "DESIGN_UNBOUND_MATERIAL" ),
		       "CONDITION G PHASE GATE: ...and omitting inPiecesPhase (outside any build phase) "
		       "fires again on the IDENTICAL document" );
	}

	//--------------------------------------------------------------
	// Condition H: DESIGN_FLAT_ALBEDO.  Materials-realism item 1
	// (2026-08-24): H is now COVERAGE-aware and hero-weighted, like D --
	// the gate is on DISTINCT eligible materials (not standard_object
	// count), and the disarm needs the HERO material (>= kHeroObjectCount
	// objects bound) to vary, not just any incidental one elsewhere.  This
	// fixture family was rewritten from doc 91's original (3 objects
	// sharing ONE flat material, silenced by ANY unrelated procedural
	// painter existing anywhere) to demonstrate the new mechanism -- the
	// OLD shape is now precisely the "3 chrome spheres" case the brief
	// says must not fire (1 distinct material never reaches the gate of
	// 3), so it could not stay as the RED-PROVE.
	//--------------------------------------------------------------
	// base_mat is the HERO (bound to a/b/c, 3 objects); extra_mat1/
	// extra_mat2 are incidental (1 object each) -- all three flat, so
	// eligibleColorMaterialCount == 3 (>= gate) and coverage is 0/3.
	const std::string docFlatAlbedo =
		"RISE ASCII SCENE 7\n"
		"uniformcolor_painter\n{\n\tname base_pnt\n\tcolor 0.55 0.5 0.45\n}\n\n"
		"lambertian_material\n{\n\tname base_mat\n\treflectance base_pnt\n}\n\n"
		"lambertian_material\n{\n\tname extra_mat1\n\treflectance base_pnt\n}\n\n"
		"lambertian_material\n{\n\tname extra_mat2\n\treflectance base_pnt\n}\n\n"
		"cylinder_geometry\n{\n\tname base_geo\n\tradius 0.3\n\theight 0.15\n}\n\n"
		"standard_object\n{\n\tname a\n\tgeometry base_geo\n\tmaterial base_mat\n}\n"
		"standard_object\n{\n\tname b\n\tgeometry base_geo\n\tmaterial base_mat\n}\n"
		"standard_object\n{\n\tname c\n\tgeometry base_geo\n\tmaterial base_mat\n}\n"
		"standard_object\n{\n\tname d\n\tgeometry base_geo\n\tmaterial extra_mat1\n}\n"
		"standard_object\n{\n\tname e\n\tgeometry base_geo\n\tmaterial extra_mat2\n}\n";
	{
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docFlatAlbedo );
		const AgentDiagnostic* d = findCode( diags, "DESIGN_FLAT_ALBEDO" );
		Check( d != nullptr,
		       "CONDITION H RED-PROVE: 3 distinct flat-colour materials (the HERO bound to 3 objects "
		       "plus 2 incidental one-object materials), none varying -- fires DESIGN_FLAT_ALBEDO" );
		if( d ) {
			Check( d->severity == AgentDiagnostic::Severity::Info, "...at Info severity" );
			Check( d->message.find( "0 of 3" ) != std::string::npos,
			       "...reporting the true coverage count (0 of 3 vary)" );
			Check( d->message.find( "base_color" ) != std::string::npos &&
			       d->message.find( "reflectance" ) != std::string::npos,
			       "...naming the procedural colour PATH generically (base_color/reflectance), not a "
			       "single material's slot" );
			Check( d->message.find( "read_skill" ) != std::string::npos &&
			       d->message.find( "procedural-textures" ) != std::string::npos,
			       "...pointing at read_skill procedural-textures" );
			Check( d->message.find( "this is fine -- ignore" ) != std::string::npos,
			       "...self-disarming, condition A's own escape clause" );

			// THE VERBATIM-COPY INVARIANT, clause-level: condition H's
			// diagnostic carries condition A's kSelfDisarm suffix (the note
			// carrier does not), so the message as a WHOLE cannot appear
			// byte-identically inside the note the way condition G's does
			// -- but the shared clause text itself (FormatFlatAlbedoClause_,
			// one function, two callers) must, proving note and diagnostic
			// read the identical claim.
			const std::string note = AgentSession::ComputeDesignNote( docFlatAlbedo );
			const std::string kClaim = "are still a flat uniformcolor_painter";
			Check( d->message.find( kClaim ) != std::string::npos && note.find( kClaim ) != std::string::npos,
			       "MONEY (verbatim invariant): the DESIGN_FLAT_ALBEDO claim text appears "
			       "BYTE-IDENTICALLY in both the diagnostic and the render-result note -- one shared "
			       "FormatFlatAlbedoClause_, two callers" );
		}
	}
	// GREEN-PROVE (hero-weighted): the IDENTICAL document, except the HERO
	// material (base_mat, bound to 3 objects) now reflects a procedural
	// field, AND one incidental one-object material (extra_mat1) ALSO
	// varies, with only ONE incidental (extra_mat2) still flat.
	//
	// Review-round P2-1: the hero rule is NECESSARY but not SUFFICIENT
	// (CoverageSatisfied_'s own doc) -- "every hero varies" alone used to
	// silence this with BOTH incidentals still flat (1 of 3, 33%), which
	// re-opened the exact "one fix silences a mostly-flat document" bug
	// item 1 was built to close.  This fixture now ALSO clears the 50%
	// fraction (2 of 3 = 67%), so it demonstrates the real, corrected
	// mechanism: the hero must vary (necessary) AND the document must be
	// majority-not-flat (sufficient) -- one incidental staying flat still
	// does not veto it once both hold.
	{
		const std::string docHeroVaries =
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\n\tname base_pnt\n\tcolor 0.55 0.5 0.45\n}\n\n"
			// 4 distinct literals -- below kParamErosionLiteralGate (6) so
			// this doesn't ALSO trip DESIGN_PARAM_METADATA_EROSION.
			"expression_painter\n{\n\tname base_field\n\texpr fbm(P*3.0,3,0.5,2.0)\n}\n\n"
			"expression_painter\n{\n\tname extra_field\n\texpr fbm(P*4.0,3,0.5,2.0)\n}\n\n"
			"lambertian_material\n{\n\tname base_mat\n\treflectance base_field\n}\n\n"
			"lambertian_material\n{\n\tname extra_mat1\n\treflectance extra_field\n}\n\n"
			"lambertian_material\n{\n\tname extra_mat2\n\treflectance base_pnt\n}\n\n"
			"cylinder_geometry\n{\n\tname base_geo\n\tradius 0.3\n\theight 0.15\n}\n\n"
			"standard_object\n{\n\tname a\n\tgeometry base_geo\n\tmaterial base_mat\n}\n"
			"standard_object\n{\n\tname b\n\tgeometry base_geo\n\tmaterial base_mat\n}\n"
			"standard_object\n{\n\tname c\n\tgeometry base_geo\n\tmaterial base_mat\n}\n"
			"standard_object\n{\n\tname d\n\tgeometry base_geo\n\tmaterial extra_mat1\n}\n"
			"standard_object\n{\n\tname e\n\tgeometry base_geo\n\tmaterial extra_mat2\n}\n";
		Check( !hasCode( AgentSession::ValidateText( docHeroVaries ), "DESIGN_FLAT_ALBEDO" ),
		       "CONDITION H GREEN-PROVE (hero-weighted, P2-1-corrected): the HERO material (3 "
		       "objects) varies AND coverage clears 50% (2 of 3) -- silent even though ONE "
		       "incidental one-object material stays flat" );
	}
	// FIRING PIN (review-round P2-1): the HERO varies, but coverage is
	// STILL below 50% because many incidental one-off props stay flat --
	// this is the exact bug the hero-alone-sufficient rule re-opened
	// (one varying hero + many flat props used to go silent).  Must fire.
	{
		std::string docOneHeroManyFlatProps =
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\n\tname base_pnt\n\tcolor 0.55 0.5 0.45\n}\n\n"
			"expression_painter\n{\n\tname base_field\n\texpr fbm(P*3.0,3,0.5,2.0)\n}\n\n"
			"lambertian_material\n{\n\tname base_mat\n\treflectance base_field\n}\n\n"
			"cylinder_geometry\n{\n\tname base_geo\n\tradius 0.3\n\theight 0.15\n}\n\n"
			"standard_object\n{\n\tname a\n\tgeometry base_geo\n\tmaterial base_mat\n}\n"
			"standard_object\n{\n\tname b\n\tgeometry base_geo\n\tmaterial base_mat\n}\n"
			"standard_object\n{\n\tname c\n\tgeometry base_geo\n\tmaterial base_mat\n}\n";
		for( int i = 0; i < 15; ++i ) {
			const std::string nm = "prop" + std::to_string( i );
			docOneHeroManyFlatProps += "lambertian_material\n{\n\tname " + nm + "\n\treflectance base_pnt\n}\n\n";
			docOneHeroManyFlatProps += "standard_object\n{\n\tname obj_" + nm +
				"\n\tgeometry base_geo\n\tmaterial " + nm + "\n}\n";
		}
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docOneHeroManyFlatProps );
		const AgentDiagnostic* d = findCode( diags, "DESIGN_FLAT_ALBEDO" );
		Check( d != nullptr,
		       "CONDITION H FIRING PIN (P2-1): one varying HERO (3 objects) + 15 flat one-off "
		       "props (1 of 16 = 6.25%, below the 50% fraction) -- fires, hero-alone is not "
		       "enough" );
		if( d ) Check( d->message.find( "1 of 16" ) != std::string::npos,
		               "...reporting the true coverage count (1 of 16 vary)" );
	}
	// BELOW THRESHOLD: only 2 DISTINCT flat materials (the new gate is on
	// ELIGIBLE MATERIAL COUNT, not standard_object count) -- must stay
	// silent even with >=3 standard_objects total, which is exactly the
	// "3 chrome spheres" shape the brief calls out.
	{
		const std::string docBelowThreshold =
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\n\tname base_pnt\n\tcolor 0.55 0.5 0.45\n}\n\n"
			"lambertian_material\n{\n\tname base_mat\n\treflectance base_pnt\n}\n\n"
			"lambertian_material\n{\n\tname extra_mat1\n\treflectance base_pnt\n}\n\n"
			"cylinder_geometry\n{\n\tname base_geo\n\tradius 0.3\n\theight 0.15\n}\n\n"
			"standard_object\n{\n\tname a\n\tgeometry base_geo\n\tmaterial base_mat\n}\n"
			"standard_object\n{\n\tname b\n\tgeometry base_geo\n\tmaterial base_mat\n}\n"
			"standard_object\n{\n\tname c\n\tgeometry base_geo\n\tmaterial extra_mat1\n}\n";
		Check( !hasCode( AgentSession::ValidateText( docBelowThreshold ), "DESIGN_FLAT_ALBEDO" ),
		       "CONDITION H BELOW THRESHOLD (a.k.a. the 3-chrome-spheres case): 3 standard_objects but "
		       "only 2 DISTINCT flat materials -- below the eligible-material gate of 3 -- stays "
		       "silent" );
	}
}

//----------------------------------------------------------------------
// RELIEF_MODIFIER_DESIGN.md sec 9 (2026-09-06), condition Q:
// DESIGN_FLAT_RELIEF -- the decal-on-plastic detector.  Same shared
// ComputeDesignNoteConditionsFromDoc_ scan as every sibling above; unlike
// conditions D/H/L/P there is no hero-material pick (Phase 4 ships no
// verb), so this fires on >= 1 qualifying OBJECT -- an object whose
// material binds a spatially-varying colour-pipe slot while the object
// itself binds no `modifier`.
//
// Fixture is a still-life vessel (a candlestick / a ceramic pot), matching
// this family's own non-creature convention -- nothing here is keyed to a
// subject.
//----------------------------------------------------------------------
static void RunFlatReliefScanTest()
{
	std::printf( "[design-note] RELIEF_MODIFIER_DESIGN sec 9: DESIGN_FLAT_RELIEF\n" );

	auto hasCode = []( const std::vector<AgentDiagnostic>& diags, const std::string& code ) {
		for( const AgentDiagnostic& d : diags ) if( d.code == code ) return true;
		return false;
	};
	auto findCode = []( const std::vector<AgentDiagnostic>& diags, const std::string& code ) -> const AgentDiagnostic* {
		for( const AgentDiagnostic& d : diags ) if( d.code == code ) return &d;
		return nullptr;
	};
	auto countCode = []( const std::vector<AgentDiagnostic>& diags, const std::string& code ) {
		int n = 0;
		for( const AgentDiagnostic& d : diags ) if( d.code == code ) ++n;
		return n;
	};

	const std::string kPreamble =
		"RISE ASCII SCENE 7\n"
		"expression_painter\n{\n\tname relief_field\n\texpr fbm(P*3.0,3,0.5,2.0)\n}\n\n"
		"uniformcolor_painter\n{\n\tname flat_pnt\n\tcolor 0.55 0.5 0.45\n}\n\n"
		"cylinder_geometry\n{\n\tname vessel_geo\n\tradius 0.3\n\theight 0.6\n}\n\n";

	//--------------------------------------------------------------
	// (a) RED-PROVE: a varying `reflectance`, no `modifier` -- fires,
	// naming the object, the material, the slot and the painter.
	//--------------------------------------------------------------
	const std::string docVaryingNoModifier = kPreamble +
		"lambertian_material\n{\n\tname vessel_mat\n\treflectance relief_field\n}\n\n"
		"standard_object\n{\n\tname vessel_obj\n\tgeometry vessel_geo\n\tmaterial vessel_mat\n}\n";
	{
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docVaryingNoModifier );
		const AgentDiagnostic* d = findCode( diags, "DESIGN_FLAT_RELIEF" );
		Check( d != nullptr,
		       "(a) RED-PROVE: a varying `reflectance` and no `modifier` fires DESIGN_FLAT_RELIEF" );
		if( d ) {
			Check( d->severity == AgentDiagnostic::Severity::Info, "...at Info severity" );
			Check( d->message.find( "`vessel_obj`" ) != std::string::npos, "...NAMING the object" );
			Check( d->message.find( "`vessel_mat`" ) != std::string::npos, "...NAMING the material" );
			Check( d->message.find( "`reflectance`" ) != std::string::npos, "...NAMING the varying slot" );
			Check( d->message.find( "`relief_field`" ) != std::string::npos,
			       "...NAMING the painter bound there" );
			Check( d->message.find( "scalar_painter { painter relief_field channel R }" ) != std::string::npos,
			       "...stating the scalar_painter bridge, naming the SAME field" );
			Check( d->message.find( "relief_modifier" ) != std::string::npos,
			       "...stating the relief_modifier half of the two-chunk fix" );
			Check( d->message.find( "`modifier`" ) != std::string::npos,
			       "...naming the attach mechanism (the object's `modifier` parameter)" );
			Check( d->message.find( "read_skill" ) != std::string::npos &&
			       d->message.find( "procedural-textures" ) != std::string::npos,
			       "...pointing at read_skill procedural-textures" );
			Check( d->message.find( "this is fine -- ignore" ) != std::string::npos,
			       "...self-disarming, condition A/H's own escape clause" );

			// THE VERBATIM-COPY INVARIANT (condition H's precedent): the
			// SHARED clause text (not the kSelfDisarm suffix, which only the
			// diagnostic carries) must appear byte-identically in the
			// render-result note.
			const std::string note = AgentSession::ComputeDesignNote( docVaryingNoModifier );
			const std::string kClaim = "the colour changes across the surface and the shading normal never does";
			Check( d->message.find( kClaim ) != std::string::npos && note.find( kClaim ) != std::string::npos,
			       "MONEY (verbatim invariant): the DESIGN_FLAT_RELIEF claim text appears BYTE-IDENTICALLY "
			       "in both the diagnostic and the render-result note -- one shared FormatFlatReliefClause_, "
			       "two callers" );
		}
	}

	//--------------------------------------------------------------
	// (b) GREEN-PROVE: the SAME material/object, now with `modifier`
	// naming a `relief_modifier` directly -- silent.
	//--------------------------------------------------------------
	{
		const std::string docWithReliefModifier = kPreamble +
			"lambertian_material\n{\n\tname vessel_mat\n\treflectance relief_field\n}\n\n"
			"scalar_painter\n{\n\tname relief_h\n\tpainter relief_field\n\tchannel R\n}\n\n"
			"relief_modifier\n{\n\tname vessel_relief\n\theight relief_h\n\tscale 0.02\n}\n\n"
			"standard_object\n{\n\tname vessel_obj\n\tgeometry vessel_geo\n\tmaterial vessel_mat\n"
			"\tmodifier vessel_relief\n}\n";
		Check( !hasCode( AgentSession::ValidateText( docWithReliefModifier ), "DESIGN_FLAT_RELIEF" ),
		       "(b) GREEN-PROVE: the IDENTICAL varying material, now with `modifier vessel_relief` bound "
		       "directly -- silent" );
	}

	//--------------------------------------------------------------
	// (c) GREEN-PROVE: `modifier` names a `modifier_stack` wrapping the
	// SAME relief_modifier -- a stack counts as bound, silent.
	//--------------------------------------------------------------
	{
		const std::string docWithModifierStack = kPreamble +
			"lambertian_material\n{\n\tname vessel_mat\n\treflectance relief_field\n}\n\n"
			"scalar_painter\n{\n\tname relief_h\n\tpainter relief_field\n\tchannel R\n}\n\n"
			"relief_modifier\n{\n\tname vessel_relief\n\theight relief_h\n\tscale 0.02\n}\n\n"
			"modifier_stack\n{\n\tname vessel_stack\n\tmodifier vessel_relief\n}\n\n"
			"standard_object\n{\n\tname vessel_obj\n\tgeometry vessel_geo\n\tmaterial vessel_mat\n"
			"\tmodifier vessel_stack\n}\n";
		Check( !hasCode( AgentSession::ValidateText( docWithModifierStack ), "DESIGN_FLAT_RELIEF" ),
		       "(c) GREEN-PROVE: `modifier vessel_stack` (a modifier_stack wrapping the SAME relief_modifier) "
		       "-- a stack counts as bound -- silent" );
	}

	//--------------------------------------------------------------
	// (d) GREEN-PROVE: a flat (uniformcolor_painter) reflectance, no
	// `modifier` -- nothing varies, silent.
	//--------------------------------------------------------------
	{
		const std::string docFlatAlbedoNoModifier = kPreamble +
			"lambertian_material\n{\n\tname vessel_mat\n\treflectance flat_pnt\n}\n\n"
			"standard_object\n{\n\tname vessel_obj\n\tgeometry vessel_geo\n\tmaterial vessel_mat\n}\n";
		Check( !hasCode( AgentSession::ValidateText( docFlatAlbedoNoModifier ), "DESIGN_FLAT_RELIEF" ),
		       "(d) GREEN-PROVE: a flat uniformcolor_painter reflectance and no modifier -- nothing varies, "
		       "silent" );
	}

	//--------------------------------------------------------------
	// (e) GREEN-PROVE: hair_material, hair_geometry, and a luminaire --
	// three objects, each excluded by clause (iii)/(iv) for a DIFFERENT
	// reason, none firing.
	//   * hairy_obj: bound to `hair_material` (its `color` slot varies) --
	//     the MATERIAL KIND exclusion, regardless of its (ordinary)
	//     geometry.
	//   * furry_obj: geometry is `hair_geometry`, material is the
	//     ordinary varying `vessel_mat` -- the GEOMETRY KIND exclusion,
	//     regardless of its material.
	//   * glow_obj: bound to `lambertian_luminaire_material` (its
	//     `exitance` slot varies) -- the LUMINAIRE exclusion.
	//--------------------------------------------------------------
	{
		const std::string docExclusions = kPreamble +
			"lambertian_material\n{\n\tname vessel_mat\n\treflectance relief_field\n}\n\n"
			"hair_material\n{\n\tname hairy_mat\n\tcolor relief_field\n}\n\n"
			"standard_object\n{\n\tname hairy_obj\n\tgeometry vessel_geo\n\tmaterial hairy_mat\n}\n\n"
			"hair_geometry\n{\n\tname fur_geo\n\tbase_geometry vessel_geo\n\tcount 10\n\tlength 0.01\n}\n\n"
			"standard_object\n{\n\tname furry_obj\n\tgeometry fur_geo\n\tmaterial vessel_mat\n}\n\n"
			"lambertian_luminaire_material\n{\n\tname glow_mat\n\texitance relief_field\n}\n\n"
			"standard_object\n{\n\tname glow_obj\n\tgeometry vessel_geo\n\tmaterial glow_mat\n}\n";
		Check( !hasCode( AgentSession::ValidateText( docExclusions ), "DESIGN_FLAT_RELIEF" ),
		       "(e) GREEN-PROVE: hair_material (material-kind exclusion), hair_geometry (geometry-kind "
		       "exclusion) and a luminaire (emissive exclusion) -- three DIFFERENT reasons, all silent" );
	}

	//--------------------------------------------------------------
	// (f) DEDUPE: two qualifying objects sharing one material -- ONE
	// diagnostic entry (not two), naming the first in full and counting
	// the rest; calling ValidateText a second time on the IDENTICAL
	// document reproduces the SAME single entry (stateless -- no
	// accumulation across passes).
	//--------------------------------------------------------------
	{
		const std::string docTwoObjects = kPreamble +
			"lambertian_material\n{\n\tname vessel_mat\n\treflectance relief_field\n}\n\n"
			"standard_object\n{\n\tname vessel_obj\n\tgeometry vessel_geo\n\tmaterial vessel_mat\n}\n"
			"standard_object\n{\n\tname second_obj\n\tgeometry vessel_geo\n\tmaterial vessel_mat\n}\n";

		const std::vector<AgentDiagnostic> pass1 = AgentSession::ValidateText( docTwoObjects );
		Check( countCode( pass1, "DESIGN_FLAT_RELIEF" ) == 1,
		       "(f) DEDUPE: two qualifying objects still yield exactly ONE DESIGN_FLAT_RELIEF entry per "
		       "pass (one finding per object, one DIAGNOSTIC per condition -- not one per object)" );
		{
			const AgentDiagnostic* d = findCode( pass1, "DESIGN_FLAT_RELIEF" );
			Check( d != nullptr && d->message.find( "`vessel_obj`" ) != std::string::npos,
			       "...naming the FIRST object (document order) in full" );
			Check( d != nullptr && d->message.find( "1 more object" ) != std::string::npos,
			       "...and counting the second" );
		}

		const std::vector<AgentDiagnostic> pass2 = AgentSession::ValidateText( docTwoObjects );
		Check( countCode( pass2, "DESIGN_FLAT_RELIEF" ) == 1,
		       "(f) DEDUPE: a SECOND ValidateText pass over the IDENTICAL document reproduces the SAME "
		       "single entry -- stateless, no cross-pass accumulation" );
	}

	//==============================================================
	// FIX ROUND 1 (2026-09-06) -- the EFFECTIVE modifier, and four
	// varying-slot classification fixes.  See
	// RELIEF_MODIFIER_DESIGN.md sec 12's "Phase 4 -- fix round 1".
	//==============================================================

	// The relief plumbing every case below reuses.
	const std::string kRelief =
		"scalar_painter\n{\n\tname relief_h\n\tpainter relief_field\n\tchannel R\n}\n\n"
		"relief_modifier\n{\n\tname vessel_relief\n\theight relief_h\n\tscale 0.02\n}\n\n";

	//--------------------------------------------------------------
	// (g) P1-1 GREEN-PROVE: `source` INSTANCING INHERITS `modifier`.
	// Cst.cpp's MergeChunkParams folds the whole source chain into the
	// derived instance and `modifier` is NOT in IsInstanceOwnParam, so
	// `copy` renders WITH `orig`'s relief -- even though `copy`'s own
	// chunk never spells `modifier`.  The pre-fix literal-param test
	// fired on `copy`.
	//--------------------------------------------------------------
	{
		const std::string docInheritedModifier = kPreamble + kRelief +
			"lambertian_material\n{\n\tname flat_mat\n\treflectance flat_pnt\n}\n\n"
			"lambertian_material\n{\n\tname vessel_mat\n\treflectance relief_field\n}\n\n"
			"standard_object\n{\n\tname orig\n\tgeometry vessel_geo\n\tmaterial flat_mat\n"
			"\tmodifier vessel_relief\n}\n\n"
			"standard_object\n{\n\tname copy\n\tsource orig\n\tmaterial vessel_mat\n}\n";
		Check( !hasCode( AgentSession::ValidateText( docInheritedModifier ), "DESIGN_FLAT_RELIEF" ),
		       "(g) P1-1 GREEN-PROVE: a `source` copy overriding only `material` INHERITS the source's "
		       "`modifier` (MergeChunkParams; `modifier` is not IsInstanceOwnParam) -- silent" );
	}

	//--------------------------------------------------------------
	// (h) P1-1 RED-PROVE: the same instancing shape, but the copy
	// spells `modifier none` -- which genuinely CLEARS the inherited
	// binding (the copy's own params are merged LAST, and the parser
	// passes 0 for a "none" modifier).  So the copy IS flat and MUST
	// fire: the helper has to treat "none" as a chain-STOPPING answer,
	// not as an absent param.
	//--------------------------------------------------------------
	{
		const std::string docClearedModifier = kPreamble + kRelief +
			"lambertian_material\n{\n\tname flat_mat\n\treflectance flat_pnt\n}\n\n"
			"lambertian_material\n{\n\tname vessel_mat\n\treflectance relief_field\n}\n\n"
			"standard_object\n{\n\tname orig\n\tgeometry vessel_geo\n\tmaterial flat_mat\n"
			"\tmodifier vessel_relief\n}\n\n"
			"standard_object\n{\n\tname copy\n\tsource orig\n\tmaterial vessel_mat\n\tmodifier none\n}\n";
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docClearedModifier );
		const AgentDiagnostic* d = findCode( diags, "DESIGN_FLAT_RELIEF" );
		Check( d != nullptr && d->message.find( "`copy`" ) != std::string::npos,
		       "(h) P1-1 RED-PROVE: the SAME copy spelling `modifier none` CLEARS the inherited relief "
		       "-- fires, naming `copy` (so \"none\" stops the source walk, it is not \"absent\")" );
	}

	//--------------------------------------------------------------
	// (i) P1-2 GREEN-PROVE (a): a composite that binds NO modifier but
	// whose EVERY operand does.  CSGObject::IntersectRay reports the
	// OPERAND's modifier on each hit, so the composite's whole surface
	// is relief-bearing even though its own chunk binds nothing.
	//--------------------------------------------------------------
	{
		const std::string docCsgOperandsBound = kPreamble + kRelief +
			"lambertian_material\n{\n\tname flat_mat\n\treflectance flat_pnt\n}\n\n"
			"lambertian_material\n{\n\tname vessel_mat\n\treflectance relief_field\n}\n\n"
			"standard_object\n{\n\tname op_a\n\tgeometry vessel_geo\n\tmaterial flat_mat\n"
			"\tmodifier vessel_relief\n}\n\n"
			"standard_object\n{\n\tname op_b\n\tgeometry vessel_geo\n\tmaterial flat_mat\n"
			"\tmodifier vessel_relief\n}\n\n"
			"csg_object\n{\n\tname comp\n\tobja op_a\n\tobjb op_b\n\toperation union\n"
			"\tmaterial vessel_mat\n}\n";
		Check( !hasCode( AgentSession::ValidateText( docCsgOperandsBound ), "DESIGN_FLAT_RELIEF" ),
		       "(i) P1-2 GREEN-PROVE (a): a csg_object overriding only `material` (varying), with EVERY "
		       "operand carrying relief -- the composite's surface IS relief-bearing -- silent" );
	}

	//--------------------------------------------------------------
	// (j) P1-2 GREEN-PROVE (b): the composite carries the relief and
	// the OPERANDS carry the varying material.  A composite's own
	// binding takes final precedence over the operand's on every hit
	// (the conditional overrides at the bottom of
	// CSGObject::IntersectRay), so neither operand is flat.
	//--------------------------------------------------------------
	{
		const std::string docCsgCompositeBound = kPreamble + kRelief +
			"lambertian_material\n{\n\tname vessel_mat\n\treflectance relief_field\n}\n\n"
			"standard_object\n{\n\tname op_a\n\tgeometry vessel_geo\n\tmaterial vessel_mat\n}\n\n"
			"standard_object\n{\n\tname op_b\n\tgeometry vessel_geo\n\tmaterial vessel_mat\n}\n\n"
			"csg_object\n{\n\tname comp\n\tobja op_a\n\tobjb op_b\n\toperation union\n"
			"\tmodifier vessel_relief\n}\n";
		Check( !hasCode( AgentSession::ValidateText( docCsgCompositeBound ), "DESIGN_FLAT_RELIEF" ),
		       "(j) P1-2 GREEN-PROVE (b): the composite binds the relief and its operands carry the "
		       "varying material -- the composite's binding overrides theirs -- silent on the operands" );
	}

	//--------------------------------------------------------------
	// (k) P1-2 RED-PROVE (c): a composite binding NO modifier where
	// only ONE of its two operands carries relief.  "Every operand"
	// fails, so the composite's own varying material IS on a partly
	// flat surface -- fires, naming the COMPOSITE.  (The operands
	// themselves carry a flat material, so neither is a finding of its
	// own; the single entry is unambiguously about `comp`.)
	//--------------------------------------------------------------
	{
		const std::string docCsgPartial = kPreamble + kRelief +
			"lambertian_material\n{\n\tname flat_mat\n\treflectance flat_pnt\n}\n\n"
			"lambertian_material\n{\n\tname vessel_mat\n\treflectance relief_field\n}\n\n"
			"standard_object\n{\n\tname op_a\n\tgeometry vessel_geo\n\tmaterial flat_mat\n"
			"\tmodifier vessel_relief\n}\n\n"
			"standard_object\n{\n\tname op_b\n\tgeometry vessel_geo\n\tmaterial flat_mat\n}\n\n"
			"csg_object\n{\n\tname comp\n\tobja op_a\n\tobjb op_b\n\toperation union\n"
			"\tmaterial vessel_mat\n}\n";
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docCsgPartial );
		const AgentDiagnostic* d = findCode( diags, "DESIGN_FLAT_RELIEF" );
		Check( d != nullptr && d->message.find( "`comp`" ) != std::string::npos,
		       "(k) P1-2 RED-PROVE (c): a composite with NO modifier and only ONE of two operands "
		       "bearing relief fires, naming the composite" );
		Check( d != nullptr && d->message.find( "more object" ) == std::string::npos,
		       "...and it is the ONLY finding (the flat-material operands are not findings of their own)" );
	}

	//--------------------------------------------------------------
	// (l) P2-1: a STRUCTURALLY CONSTANT `blend_painter`.  The colour
	// classifier used to be name-list based, so a blend whose colora,
	// colorb and mask are ALL uniform classified "Varying" purely
	// because "blend_painter" is not one of the three
	// constant-by-construction kinds -- and Q advised relief on a
	// single flat colour.  It now recurses, exactly as the scalar twin
	// already walked its base/multiply chains.
	//--------------------------------------------------------------
	{
		const std::string docConstantBlend = kPreamble +
			"uniformcolor_painter\n{\n\tname flat_pnt2\n\tcolor 0.2 0.3 0.4\n}\n\n"
			"blend_painter\n{\n\tname flat_blend\n\tcolora flat_pnt\n\tcolorb flat_pnt2\n"
			"\tmask flat_pnt2\n}\n\n"
			"lambertian_material\n{\n\tname vessel_mat\n\treflectance flat_blend\n}\n\n"
			"standard_object\n{\n\tname vessel_obj\n\tgeometry vessel_geo\n\tmaterial vessel_mat\n}\n";
		Check( !hasCode( AgentSession::ValidateText( docConstantBlend ), "DESIGN_FLAT_RELIEF" ),
		       "(l) P2-1 GREEN-PROVE: a blend_painter whose colora/colorb/mask are ALL uniform is "
		       "structurally CONSTANT -- silent" );

		// RED-PROVE the recursion: swap ONE input for the fbm field.
		const std::string docVaryingBlend = kPreamble +
			"uniformcolor_painter\n{\n\tname flat_pnt2\n\tcolor 0.2 0.3 0.4\n}\n\n"
			"blend_painter\n{\n\tname varying_blend\n\tcolora flat_pnt\n\tcolorb flat_pnt2\n"
			"\tmask relief_field\n}\n\n"
			"lambertian_material\n{\n\tname vessel_mat\n\treflectance varying_blend\n}\n\n"
			"standard_object\n{\n\tname vessel_obj\n\tgeometry vessel_geo\n\tmaterial vessel_mat\n}\n";
		const std::vector<AgentDiagnostic> varyingDiags = AgentSession::ValidateText( docVaryingBlend );
		const AgentDiagnostic* d = findCode( varyingDiags, "DESIGN_FLAT_RELIEF" );
		Check( d != nullptr && d->message.find( "`varying_blend`" ) != std::string::npos,
		       "(l) P2-1 RED-PROVE: the SAME blend with ONE varying input (mask) is Varying -- fires, "
		       "naming the blend" );
	}

	//--------------------------------------------------------------
	// (m) P2-2(a): a WRAPPER material.  `coated_material` has no
	// colour slot of its own that varies, so the pre-fix scan saw a
	// completely flat material and stayed silent -- while the surface
	// under the coat is fully textured.  The search now follows every
	// ParameterPipe::Material slot into the wrapped base.
	//--------------------------------------------------------------
	{
		const std::string docCoatedVaryingBase = kPreamble +
			"lambertian_material\n{\n\tname vessel_mat\n\treflectance relief_field\n}\n\n"
			"coated_material\n{\n\tname vessel_coated\n\tbase vessel_mat\n}\n\n"
			"standard_object\n{\n\tname vessel_obj\n\tgeometry vessel_geo\n\tmaterial vessel_coated\n}\n";
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docCoatedVaryingBase );
		const AgentDiagnostic* d = findCode( diags, "DESIGN_FLAT_RELIEF" );
		Check( d != nullptr && d->message.find( "`vessel_obj`" ) != std::string::npos &&
		       d->message.find( "`vessel_mat`" ) != std::string::npos,
		       "(m) P2-2(a) RED-PROVE: a coated_material wrapping a varying base fires, naming the "
		       "object and the WRAPPED material whose slot actually varies" );
	}

	//--------------------------------------------------------------
	// (n) P2-2(b): `add_wetness`'s GGX branch rebinds the colour slot
	// to a WETNESS-PRELUDE expression_painter.  Advising relief there
	// is physically backwards (that verb's own hook-point note: a wet
	// film conforms to relief, it does not add it), so a varying
	// binding carrying condition H's `dryness`/`film_amount` marker
	// does not count.
	//--------------------------------------------------------------
	{
		const std::string docWetRebind = kPreamble +
			"expression_painter\n{\n\tname vessel_mat_wet\n"
			"\tdef dryness 0.4\n\tdef film_amount 0.6\n"
			"\texpr vec3(0.4,0.4,0.4) * (1.0 - film_amount * (1.0 - dryness))\n}\n\n"
			"ggx_material\n{\n\tname vessel_mat\n\trd vessel_mat_wet\n\trs flat_pnt\n"
			"\talphax 0.2\n\talphay 0.2\n}\n\n"
			"standard_object\n{\n\tname vessel_obj\n\tgeometry vessel_geo\n\tmaterial vessel_mat\n}\n";
		Check( !hasCode( AgentSession::ValidateText( docWetRebind ), "DESIGN_FLAT_RELIEF" ),
		       "(n) P2-2(b) GREEN-PROVE: a colour slot rebound by add_wetness to a wetness-prelude "
		       "expression_painter (dryness + film_amount) does not count as authored texture -- silent" );
	}

	//--------------------------------------------------------------
	// (o) P2-3: a varying `emissive` on a NON-luminaire kind.
	// `ggx_material` carries an optional `emissive` painter and
	// ColorMaterialSlotsByKind_ (registry-derived off
	// ParameterPipe::Color) rightly lists it -- but relief cannot sell
	// a GLOW, so an emission slot is not a candidate.  rd/rs here are
	// flat, so the material has nothing else to offer.
	//--------------------------------------------------------------
	{
		const std::string docVaryingEmissive = kPreamble +
			"ggx_material\n{\n\tname rune_mat\n\trd flat_pnt\n\trs flat_pnt\n"
			"\talphax 0.2\n\talphay 0.2\n\temissive relief_field\n}\n\n"
			"standard_object\n{\n\tname vessel_obj\n\tgeometry vessel_geo\n\tmaterial rune_mat\n}\n";
		Check( !hasCode( AgentSession::ValidateText( docVaryingEmissive ), "DESIGN_FLAT_RELIEF" ),
		       "(o) P2-3 GREEN-PROVE: a varying `emissive` over flat rd/rs on a non-luminaire kind is "
		       "a painted GLOW, not a surface relief cue -- silent" );

		// RED-PROVE the exclusion is SLOT-scoped, not material-scoped: the
		// SAME kind with a varying `rd` still fires.
		const std::string docVaryingDiffuse = kPreamble +
			"ggx_material\n{\n\tname rune_mat\n\trd relief_field\n\trs flat_pnt\n"
			"\talphax 0.2\n\talphay 0.2\n\temissive relief_field\n}\n\n"
			"standard_object\n{\n\tname vessel_obj\n\tgeometry vessel_geo\n\tmaterial rune_mat\n}\n";
		const std::vector<AgentDiagnostic> diffuseDiags = AgentSession::ValidateText( docVaryingDiffuse );
		const AgentDiagnostic* d = findCode( diffuseDiags, "DESIGN_FLAT_RELIEF" );
		Check( d != nullptr && d->message.find( "`rd`" ) != std::string::npos,
		       "(o) P2-3 RED-PROVE: the SAME material with a varying `rd` still fires, naming `rd` -- "
		       "the exclusion is SLOT-scoped, not material-scoped" );
	}

	//==============================================================
	// FIX ROUND 2 (2026-09-06) -- clause (i) taught the SAME two
	// engine rules fix round 1 taught clause (ii), plus the `source`
	// chain bound.  See RELIEF_MODIFIER_DESIGN.md sec 12's "Phase 4
	// -- fix round 2".
	//==============================================================

	//--------------------------------------------------------------
	// (p) P1 GREEN-PROVE: the composite's MATERIAL override.  A
	// csg_object's own `material` takes final precedence over the
	// operand's on every hit it reports -- the matched pair
	// `if( pMaterial ) ri.pMaterial = pMaterial;  if( pModifier )
	// ri.pModifier = pModifier;` at the bottom of
	// CSGObject::IntersectRay.  So `op_a`'s varying material is never
	// shaded, and advising relief on it advises a texture that does
	// not exist.  (Fix round 1 taught clause (ii) both halves of that
	// pair; clause (i) still resolved the operand's own literal.)
	//--------------------------------------------------------------
	{
		const std::string docCsgMaterialOverride = kPreamble +
			"lambertian_material\n{\n\tname flat_mat\n\treflectance flat_pnt\n}\n\n"
			"lambertian_material\n{\n\tname vessel_mat\n\treflectance relief_field\n}\n\n"
			"standard_object\n{\n\tname op_a\n\tgeometry vessel_geo\n\tmaterial vessel_mat\n}\n\n"
			"standard_object\n{\n\tname op_b\n\tgeometry vessel_geo\n\tmaterial flat_mat\n}\n\n"
			"csg_object\n{\n\tname comp\n\tobja op_a\n\tobjb op_b\n\toperation union\n"
			"\tmaterial flat_mat\n}\n";
		Check( !hasCode( AgentSession::ValidateText( docCsgMaterialOverride ), "DESIGN_FLAT_RELIEF" ),
		       "(p) P1 GREEN-PROVE: an operand whose enclosing composite spells its OWN `material` is "
		       "not a candidate on the operand's material -- that texture is never shaded -- silent" );
	}

	//--------------------------------------------------------------
	// (p') P1 RED-PROVE / control: the IDENTICAL document with the
	// composite's `material` line dropped.  Nothing overrides the
	// operand now, so its varying material IS what gets shaded and
	// the object fires -- which is what makes (p) a rule about the
	// override rather than a blanket "operands never fire".
	//--------------------------------------------------------------
	{
		const std::string docCsgNoOverride = kPreamble +
			"lambertian_material\n{\n\tname flat_mat\n\treflectance flat_pnt\n}\n\n"
			"lambertian_material\n{\n\tname vessel_mat\n\treflectance relief_field\n}\n\n"
			"standard_object\n{\n\tname op_a\n\tgeometry vessel_geo\n\tmaterial vessel_mat\n}\n\n"
			"standard_object\n{\n\tname op_b\n\tgeometry vessel_geo\n\tmaterial flat_mat\n}\n\n"
			"csg_object\n{\n\tname comp\n\tobja op_a\n\tobjb op_b\n\toperation union\n}\n";
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docCsgNoOverride );
		const AgentDiagnostic* d = findCode( diags, "DESIGN_FLAT_RELIEF" );
		Check( d != nullptr && d->message.find( "`op_a`" ) != std::string::npos,
		       "(p') P1 RED-PROVE: drop the composite's `material` and the SAME operand fires, naming "
		       "`op_a` -- the operand's own material is what gets shaded again" );
	}

	//--------------------------------------------------------------
	// (p'') P1 NESTED: an inner composite overrides the material, the
	// outer one does not.  The outermost SPELLED material is what
	// survives the inside-out adoption, so the operands are silent
	// (the middle composite overrode them) while the MIDDLE composite
	// itself fires on the varying material it spells -- nothing above
	// it overrides that, and it binds no modifier.
	//--------------------------------------------------------------
	{
		const std::string docNestedCsg = kPreamble +
			"lambertian_material\n{\n\tname flat_mat\n\treflectance flat_pnt\n}\n\n"
			"lambertian_material\n{\n\tname vessel_mat\n\treflectance relief_field\n}\n\n"
			"lambertian_material\n{\n\tname mid_mat\n\treflectance relief_field\n}\n\n"
			"standard_object\n{\n\tname op_a\n\tgeometry vessel_geo\n\tmaterial vessel_mat\n}\n\n"
			"standard_object\n{\n\tname op_b\n\tgeometry vessel_geo\n\tmaterial vessel_mat\n}\n\n"
			"csg_object\n{\n\tname comp_mid\n\tobja op_a\n\tobjb op_b\n\toperation union\n"
			"\tmaterial mid_mat\n}\n\n"
			"standard_object\n{\n\tname spacer\n\tgeometry vessel_geo\n\tmaterial flat_mat\n}\n\n"
			"csg_object\n{\n\tname comp_outer\n\tobja comp_mid\n\tobjb spacer\n\toperation union\n}\n";
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docNestedCsg );
		const AgentDiagnostic* d = findCode( diags, "DESIGN_FLAT_RELIEF" );
		Check( d != nullptr && d->message.find( "`comp_mid`" ) != std::string::npos,
		       "(p'') P1 NESTED: with the MIDDLE composite overriding the material and the outer one "
		       "not, the middle composite is the finding" );
		Check( d != nullptr && d->message.find( "more object" ) == std::string::npos,
		       "...and it is the ONLY one -- both operands are silenced by the middle composite's "
		       "override, exactly as a single-level one silences them" );
	}

	//--------------------------------------------------------------
	// (q) P2-2 fix round 2: clause (i) is `source`-AWARE.  A copy
	// inherits `material` exactly as it inherits `modifier`
	// (MergeChunkParams; neither is an IsInstanceOwnParam), so a
	// bare `copy { source orig }` renders the SAME varying material
	// with the same absent relief.  The literal-param lookup this
	// replaced dropped every such copy -- an under-report that also
	// mis-counted the clause's "and N more objects" tally.
	//--------------------------------------------------------------
	{
		const std::string docInheritedMaterial = kPreamble +
			"lambertian_material\n{\n\tname vessel_mat\n\treflectance relief_field\n}\n\n"
			"standard_object\n{\n\tname orig\n\tgeometry vessel_geo\n\tmaterial vessel_mat\n}\n\n"
			"standard_object\n{\n\tname copy\n\tsource orig\n\tposition 1 0 0\n}\n";
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docInheritedMaterial );
		const AgentDiagnostic* d = findCode( diags, "DESIGN_FLAT_RELIEF" );
		Check( d != nullptr && d->message.find( "`orig`" ) != std::string::npos,
		       "(q) P2-2 RED-PROVE: a copy that spells NO material of its own inherits the source's -- "
		       "fires, the first-authored object named in full" );
		Check( d != nullptr && d->message.find( "1 more object" ) != std::string::npos,
		       "...and the copy is COUNTED as the second finding (the tally was short by one before "
		       "the material lookup followed `source`)" );
	}

	//--------------------------------------------------------------
	// (r) P2-1 fix round 2: the `source` chain bound.  The walk used
	// to stop after 7 hops on a bound justified by a `source` cycle
	// that Cst.cpp makes impossible (forward references are refused,
	// and SourceChainOf caps at 256 purely as a belt).  A LEGAL
	// 9-deep chain with the relief at its root therefore resolved to
	// "no modifier" at its far end and fired falsely.  The bound is
	// now the engine's own 256, so the whole chain is silent.
	//--------------------------------------------------------------
	{
		std::string docDeepChain = kPreamble + kRelief +
			"lambertian_material\n{\n\tname vessel_mat\n\treflectance relief_field\n}\n\n"
			"standard_object\n{\n\tname o0\n\tgeometry vessel_geo\n\tmaterial vessel_mat\n"
			"\tmodifier vessel_relief\n}\n\n";
		for( int k = 1; k <= 9; ++k ) {
			docDeepChain += "standard_object\n{\n\tname o" + std::to_string( k ) +
				"\n\tsource o" + std::to_string( k - 1 ) +
				"\n\tposition " + std::to_string( k ) + " 0 0\n}\n\n";
		}
		Check( !hasCode( AgentSession::ValidateText( docDeepChain ), "DESIGN_FLAT_RELIEF" ),
		       "(r) P2-1 GREEN-PROVE: a LEGAL 9-deep `source` chain with the relief at its root is "
		       "silent at every link -- the walk reaches the root, as the engine's own expansion does" );
	}
}

//----------------------------------------------------------------------
// Materials-realism (2026-08-24), items 1 + 4.
//   Item 1: coverage-fraction disarm at exactly the 50% boundary (no
//     hero material -- see AgentVaryMaterialTest.cpp's G3/G4b for the
//     hero-weighted cases).
//   Item 4: DESIGN_MATERIAL_KIND_MISMATCH, the briefed-vs-bound note.
//----------------------------------------------------------------------
static void RunMaterialsRealismScanTest()
{
	std::printf( "[design-note] materials-realism items 1 + 4\n" );

	auto hasCode = []( const std::vector<AgentDiagnostic>& diags, const std::string& code ) {
		for( const AgentDiagnostic& d : diags ) if( d.code == code ) return true;
		return false;
	};
	auto findCode = []( const std::vector<AgentDiagnostic>& diags, const std::string& code ) -> const AgentDiagnostic* {
		for( const AgentDiagnostic& d : diags ) if( d.code == code ) return &d;
		return nullptr;
	};

	//--------------------------------------------------------------
	// Item 1: fraction-boundary (no hero -- every material bound to
	// exactly ONE object, so CoverageSatisfied_ falls back to the plain
	// fraction).  kCoverageFraction is 0.5: exactly 2 of 4 (50%) must
	// SATISFY (>=), and 1 of 4 (25%) must NOT (already pinned by
	// AgentVaryMaterialTest.cpp's G3).
	//--------------------------------------------------------------
	{
		const std::string docHalfVaries =
			"RISE ASCII SCENE 7\n"
			"scalar_painter\n{\n\tname sp_var\n\texpression\tu\n}\n\n"
			"ggx_material\n{\n\tname m1\n\talphax 0.3\n}\n\n"
			"ggx_material\n{\n\tname m2\n\talphax 0.4\n}\n\n"
			"ggx_material\n{\n\tname m3\n\talphax sp_var\n}\n\n"
			"ggx_material\n{\n\tname m4\n\talphax sp_var\n}\n\n"
			"sphere_geometry\n{\n\tname geo\n\tradius 0.5\n}\n\n"
			"standard_object\n{\n\tname o1\n\tgeometry geo\n\tmaterial m1\n}\n"
			"standard_object\n{\n\tname o2\n\tgeometry geo\n\tmaterial m2\n}\n"
			"standard_object\n{\n\tname o3\n\tgeometry geo\n\tmaterial m3\n}\n"
			"standard_object\n{\n\tname o4\n\tgeometry geo\n\tmaterial m4\n}\n";
		Check( !hasCode( AgentSession::ValidateText( docHalfVaries ), "DESIGN_CONSTANT_MICROSURFACE" ),
		       "ITEM 1 FRACTION BOUNDARY: 2 of 4 materials vary (exactly 50%, no hero -- every "
		       "material bound to one object) -- coverage IS satisfied (the gate is >=, not >)" );
	}
	{
		// RED-PROVE (item 1): the SAME shape, but only 1 of 4 varies
		// (25%, below the fraction) -- must still fire, and report the
		// true coverage.
		const std::string docBelowFraction =
			"RISE ASCII SCENE 7\n"
			"scalar_painter\n{\n\tname sp_var\n\texpression\tu\n}\n\n"
			"ggx_material\n{\n\tname m1\n\talphax 0.3\n}\n\n"
			"ggx_material\n{\n\tname m2\n\talphax 0.4\n}\n\n"
			"ggx_material\n{\n\tname m3\n\talphax 0.5\n}\n\n"
			"ggx_material\n{\n\tname m4\n\talphax sp_var\n}\n\n"
			"sphere_geometry\n{\n\tname geo\n\tradius 0.5\n}\n\n"
			"standard_object\n{\n\tname o1\n\tgeometry geo\n\tmaterial m1\n}\n"
			"standard_object\n{\n\tname o2\n\tgeometry geo\n\tmaterial m2\n}\n"
			"standard_object\n{\n\tname o3\n\tgeometry geo\n\tmaterial m3\n}\n"
			"standard_object\n{\n\tname o4\n\tgeometry geo\n\tmaterial m4\n}\n";
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docBelowFraction );
		const AgentDiagnostic* d = findCode( diags, "DESIGN_CONSTANT_MICROSURFACE" );
		Check( d != nullptr,
		       "ITEM 1 RED-PROVE: 1 of 4 materials vary (25%, below the 50% fraction, no hero) -- "
		       "fires DESIGN_CONSTANT_MICROSURFACE" );
		if( d ) Check( d->message.find( "1 of 4" ) != std::string::npos,
		               "...reporting the true coverage count (1 of 4 vary)" );
	}

	//--------------------------------------------------------------
	// Item 4: DESIGN_MATERIAL_KIND_MISMATCH.
	//--------------------------------------------------------------
	{
		// RED-PROVE: an object/material NAMED like a jellyfish (the
		// "emissive-jellyfish" shape from vcm_sdf_luminaire_jellyfish.RISEscene
		// -- pbr_metallic_roughness_material, NOT itself a luminaire kind)
		// bound to an OPAQUE material fires the mismatch note.
		const std::string docJellyfish =
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.6 0.7 0.8\n}\n\n"
			"pbr_metallic_roughness_material\n{\n\tname jellyfish_bell_mat\n\tbase_color pnt\n"
			"\tmetallic 0.0\n\troughness 0.3\n}\n\n"
			"sphere_geometry\n{\n\tname geo\n\tradius 0.5\n}\n\n"
			"standard_object\n{\n\tname jellyfish_bell\n\tgeometry geo\n\tmaterial jellyfish_bell_mat\n}\n";
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docJellyfish );
		const AgentDiagnostic* d = findCode( diags, "DESIGN_MATERIAL_KIND_MISMATCH" );
		Check( d != nullptr,
		       "ITEM 4 RED-PROVE: an object/material named `jellyfish_bell`/`jellyfish_bell_mat` "
		       "bound to an OPAQUE pbr_metallic_roughness_material fires DESIGN_MATERIAL_KIND_MISMATCH" );
		if( d ) {
			Check( d->severity == AgentDiagnostic::Severity::Info, "...at Info severity" );
			Check( d->message.find( "`jellyfish_bell`" ) != std::string::npos,
			       "...naming the mismatched object" );
			Check( d->message.find( "dielectric_material" ) != std::string::npos &&
			       d->message.find( "subsurfacescattering_material" ) != std::string::npos,
			       "...naming the transmissive alternatives" );
			Check( d->message.find( "named like" ) != std::string::npos,
			       "...hedging as a heuristic on the NAME" );
			Check( d->message.find( "this is fine, ignore it" ) != std::string::npos,
			       "...self-disarming" );
		}
	}
	{
		// GREEN-PROVE (the brief's own named case): a painter named
		// `glass_green` bound through a GENUINELY transmissive
		// dielectric_material -- must NOT fire.  The material KIND, not
		// the painter's name, is what this condition classifies.
		const std::string docGenuineGlass =
			"RISE ASCII SCENE 7\n"
			// `tau` is a SCALAR-pipe slot (ISCALARPAINTER_REFACTOR.md), so
			// the painter naming "glass" here is a scalar_painter, not a
			// colour one -- still exactly the brief's scenario: a chunk
			// named like the trigger words, genuinely bound through a
			// transmissive material kind.
			"scalar_painter\n{\n\tname glass_green\n\tvalue 0.9\n}\n\n"
			"dielectric_material\n{\n\tname mat_pane\n\ttau glass_green\n\tior 1.5\n}\n\n"
			"box_geometry\n{\n\tname geo\n\twidth 1\n\theight 1\n\tdepth 0.05\n}\n\n"
			"standard_object\n{\n\tname pane\n\tgeometry geo\n\tmaterial mat_pane\n}\n";
		Check( !hasCode( AgentSession::ValidateText( docGenuineGlass ), "DESIGN_MATERIAL_KIND_MISMATCH" ),
		       "ITEM 4 GREEN-PROVE: a painter named `glass_green` bound through a genuinely "
		       "transmissive dielectric_material stays silent -- the material KIND decides, not the "
		       "painter's name" );
	}
	{
		// GREEN-PROVE (calibration): a TRUE luminaire named `glass1` (the
		// photon_cloister.RISEscene / bdpt_cloister.RISEscene shape, 7 of
		// the corpus's 25 raw name hits) stays silent -- a glowing shell
		// sold by emission, not transmission, is deliberately excluded.
		const std::string docGlassLuminaire =
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\n\tname pnt\n\tcolor 5 5 5\n}\n\n"
			"lambertian_luminaire_material\n{\n\tname glass1\n\texitance pnt\n\tscale 20.0\n\tmaterial none\n}\n\n"
			"sphere_geometry\n{\n\tname geo\n\tradius 0.2\n}\n\n"
			"standard_object\n{\n\tname glass1_obj\n\tgeometry geo\n\tmaterial glass1\n}\n";
		Check( !hasCode( AgentSession::ValidateText( docGlassLuminaire ), "DESIGN_MATERIAL_KIND_MISMATCH" ),
		       "ITEM 4 GREEN-PROVE (calibration): a `glass1`-named lambertian_luminaire_material "
		       "(the photon_cloister/bdpt_cloister shape) stays silent -- emitters are excluded from "
		       "the opaque set" );
	}
	{
		// GREEN-PROVE (review-round P2-3): a `membrane`-named object bound
		// to biospec_skin_material -- "membrane" is a plausible name on a
		// tissue material.  HONEST NOTE (found while red-proving P2-3):
		// this stays silent TODAY via a DIFFERENT, upstream mechanism, not
		// the `subdermal_layer` marker this fix added -- biospec_skin_
		// material's Reference params were never individually pipe-
		// audited (all sit at ParameterSemantics.pipe == Unspecified), so
		// the kind never enters ScalarMaterialSlotsByKind_/
		// ColorMaterialSlotsByKind_/pendingMaterials at all, and condition
		// I never classifies it either way.  `subdermal_layer` is kept as
		// the correct marker for the day that pipe audit happens (see
		// OpaqueReflectionOnlyMaterialKinds_'s own doc) -- this pin still
		// documents the user-visible behaviour (silent), just not (today)
		// via the mechanism its own name suggests.
		const std::string docSkinMembrane =
			"RISE ASCII SCENE 7\n"
			"biospec_skin_material\n{\n\tname membrane_mat\n}\n\n"
			"sphere_geometry\n{\n\tname geo\n\tradius 0.2\n}\n\n"
			"standard_object\n{\n\tname membrane_layer\n\tgeometry geo\n\tmaterial membrane_mat\n}\n";
		Check( !hasCode( AgentSession::ValidateText( docSkinMembrane ), "DESIGN_MATERIAL_KIND_MISMATCH" ),
		       "ITEM 4 GREEN-PROVE (P2-3): a `membrane`-named object bound to biospec_skin_material "
		       "stays silent (today: never classified at all, its Reference params being pipe-"
		       "Unspecified -- see the doc comment above)" );
	}
	{
		// Same pin, donner_jensen_skin_bssrdf_material -- SAME honest
		// caveat as biospec_skin_material above: its params are ALSO
		// pipe-Unspecified today, so it is silent via the same upstream
		// non-classification, not via `epidermis_thickness` (kept for the
		// same future-pipe-audit reason).
		const std::string docDonnerJensenMembrane =
			"RISE ASCII SCENE 7\n"
			"donner_jensen_skin_bssrdf_material\n{\n\tname membrane_mat2\n}\n\n"
			"sphere_geometry\n{\n\tname geo\n\tradius 0.2\n}\n\n"
			"standard_object\n{\n\tname membrane_layer2\n\tgeometry geo\n\tmaterial membrane_mat2\n}\n";
		Check( !hasCode( AgentSession::ValidateText( docDonnerJensenMembrane ), "DESIGN_MATERIAL_KIND_MISMATCH" ),
		       "ITEM 4 GREEN-PROVE (P2-3): a `membrane`-named object bound to "
		       "donner_jensen_skin_bssrdf_material stays silent (today: never classified, pipe-"
		       "Unspecified)" );
	}
	{
		// Same pin, generic_human_tissue_material -- the ONE of the three
		// where `sca` is genuinely LOAD-BEARING: `sca` carries an audited
		// `semantics.pipe == ParameterPipe::Scalar`, so this kind DOES
		// enter pendingMaterials/condition I's classification, and `sca`
		// is what keeps it out of the opaque set.  Red-proved for real
		// (removing `sca` from the marker set flips this test to FAIL).
		const std::string docTissueMembrane =
			"RISE ASCII SCENE 7\n"
			"generic_human_tissue_material\n{\n\tname membrane_mat3\n}\n\n"
			"sphere_geometry\n{\n\tname geo\n\tradius 0.2\n}\n\n"
			"standard_object\n{\n\tname membrane_layer3\n\tgeometry geo\n\tmaterial membrane_mat3\n}\n";
		Check( !hasCode( AgentSession::ValidateText( docTissueMembrane ), "DESIGN_MATERIAL_KIND_MISMATCH" ),
		       "ITEM 4 GREEN-PROVE (P2-3 MONEY): a `membrane`-named object bound to "
		       "generic_human_tissue_material stays silent -- `sca` genuinely marks it transmissive" );
	}
}

//----------------------------------------------------------------------
// Materials-realism FIX 2 (2026-08-24): DESIGN_SDF_BLEND_SCALE, the
// blend-scale law mechanized (db9a88a9's `part` guidance: a smin k
// comparable to the smallest joined part's own dimension dissolves it --
// want k at about a third of that dimension or less).  Fixture numbers
// below reproduce the apothecary-cat ear that motivated the fix: a
// roundcone tip radius of 0.004 joined at k=0.012 (3x the tip radius).
//----------------------------------------------------------------------
static void RunSDFBlendScaleScanTest()
{
	std::printf( "[design-note] materials-realism FIX 2: DESIGN_SDF_BLEND_SCALE\n" );

	auto hasCode = []( const std::vector<AgentDiagnostic>& diags, const std::string& code ) {
		for( const AgentDiagnostic& d : diags ) if( d.code == code ) return true;
		return false;
	};
	auto findCode = []( const std::vector<AgentDiagnostic>& diags, const std::string& code ) -> const AgentDiagnostic* {
		for( const AgentDiagnostic& d : diags ) if( d.code == code ) return &d;
		return nullptr;
	};

	{
		// RED-PROVE: the apothecary-cat ear itself -- a roundcone tip
		// radius of 0.004 (a=0.02 base, b=0.004 tip, c=0.05 height; the
		// characteristic dimension is min(a,b) = the tip) joined via smin
		// at k=0.012, exactly 3x the tip radius -- must fire, naming the
		// chunk, the part index, the primitive kind, k, and the max bound.
		//
		// Review-round C, P3-a: this fixture is ALSO the in-tree source
		// for kSizeMismatchGate's own doc comment's cited "12.5" ratio
		// (previously cited from an out-of-band session, not a committed
		// fixture) -- worked here so a reader can check it against
		// COMMITTED numbers, not take it on faith: dimCur = min(a,b) =
		// 0.004 (the ear).  The sphere head is the only prior part, dim
		// 0.05, within reach (distance 0.05, well inside its own +
		// the ear's envelope radii plus 2k).  priorMinDim = 0.05, so
		// mismatch = max(dimCur, priorMinDim) / min(...) = 0.05 / 0.004
		// = 12.5 -- comfortably past kSizeMismatchGate (3.0), which is
		// why this fires on genuine size mismatch alone (independent of
		// the k >= dimEffective total-dissolve bypass, kTotalDissolveGate
		// 5.0 -- this join's own k/dimEffective is 0.012/0.004 = 3.0,
		// under that bypass's own gate).
		const std::string docCatEar =
			"RISE ASCII SCENE 7\n"
			"sdf_geometry\n{\n\tname apothecary_cat_head\n"
			"\tpart\tsphere union 0  0 0 0  0 0 0  1 1 1  0.05 0 0  0\n"
			"\tpart\troundcone smin 0.012  0 0.05 0  0 0 0  1 1 1  0.02 0.004 0.05  0\n"
			"}\n";
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docCatEar );
		const AgentDiagnostic* d = findCode( diags, "DESIGN_SDF_BLEND_SCALE" );
		Check( d != nullptr,
		       "RED-PROVE: apothecary_cat_head's ear (roundcone tip radius 0.004, k=0.012, 3x ratio) "
		       "fires DESIGN_SDF_BLEND_SCALE" );
		if( d ) {
			Check( d->severity == AgentDiagnostic::Severity::Info, "...at Info severity" );
			Check( d->message.find( "apothecary_cat_head" ) != std::string::npos,
			       "...naming the sdf_geometry chunk" );
			Check( d->message.find( "part 2" ) != std::string::npos, "...naming the offending part index" );
			Check( d->message.find( "roundcone" ) != std::string::npos, "...naming the primitive kind" );
			Check( d->message.find( "k=0.012" ) != std::string::npos, "...reporting the authored k" );
			Check( d->message.find( "smin joint" ) != std::string::npos &&
			       d->message.find( "wider than about a third" ) != std::string::npos,
			       "...stating the blend-scale law" );
			// Creature scaffold slice: the clause now names the CALLABLE
			// verb for a creature body, not just the chunk kind -- the
			// twice-proven "mechanized verbs convert, prose doesn't" law.
			Check( d->message.find( "insert_geometry_scaffold family:quadruped" ) != std::string::npos,
			       "...MONEY: naming insert_geometry_scaffold family:quadruped alongside skeleton_geometry" );
		}
	}
	{
		// GREEN-PROVE: the SAME ear geometry, but k dropped to 0.001 --
		// 1/4 of the 0.004 tip radius, comfortably under the ~1/3 bound --
		// must stay silent.
		const std::string docCompliantEar =
			"RISE ASCII SCENE 7\n"
			"sdf_geometry\n{\n\tname compliant_cat_head\n"
			"\tpart\tsphere union 0  0 0 0  0 0 0  1 1 1  0.05 0 0  0\n"
			"\tpart\troundcone smin 0.001  0 0.05 0  0 0 0  1 1 1  0.02 0.004 0.05  0\n"
			"}\n";
		Check( !hasCode( AgentSession::ValidateText( docCompliantEar ), "DESIGN_SDF_BLEND_SCALE" ),
		       "GREEN-PROVE: the same ear at k=0.001 (~1/4 of the 0.004 tip radius, under the ~1/3 "
		       "bound) stays silent" );
	}
	{
		// GREEN-PROVE (exclusion): skeleton_geometry is a DIFFERENT CST
		// role at this scan's level (it only expands into an sdf_geometry
		// at derive time) -- a joint with an equivalently tiny radius must
		// not fire, because this scan never looks inside a skeleton_geometry
		// chunk at all.
		const std::string docSkeleton =
			"RISE ASCII SCENE 7\n"
			"skeleton_geometry\n{\n\tname apothecary_cat_skel\n"
			"\tjoint\thead none 0 0 0 0.05\n"
			"\tjoint\tear head 0 0.05 0 0.004\n"
			"}\n";
		Check( !hasCode( AgentSession::ValidateText( docSkeleton ), "DESIGN_SDF_BLEND_SCALE" ),
		       "GREEN-PROVE (exclusion): a skeleton_geometry chunk with an equivalently tiny joint "
		       "radius stays silent -- skeleton_geometry self-scales its own blends and is excluded" );
	}
	{
		// Bounded list: 5 offending smin joints on one sdf_geometry chunk
		// -- message shows the first 3 (FormatBoundedNameList_'s cap) plus
		// "and 2 more".  RADIATING star layout (not a line): each of the 5
		// small satellites sits far from the OTHER satellites but close to
		// the shared big base, so review-round B's nearest-by-gap pick
		// (below) lands each one on the base, not on a same-sized sibling
		// -- a line would let satellite N pick satellite N-1 as its
		// "nearest", which the discriminator correctly treats as a
		// comparable-size meld and silences.
		const std::string docManyBlobs =
			"RISE ASCII SCENE 7\n"
			"sdf_geometry\n{\n\tname manyblobs\n"
			"\tpart\tsphere union 0  0 0 0  0 0 0  1 1 1  1 0 0  0\n"
			"\tpart\tsphere smin 0.5  1.1 0 0  0 0 0  1 1 1  0.2 0 0  0\n"
			"\tpart\tsphere smin 0.5  0 1.1 0  0 0 0  1 1 1  0.2 0 0  0\n"
			"\tpart\tsphere smin 0.5  -1.1 0 0  0 0 0  1 1 1  0.2 0 0  0\n"
			"\tpart\tsphere smin 0.5  0 -1.1 0  0 0 0  1 1 1  0.2 0 0  0\n"
			"\tpart\tsphere smin 0.5  0 0 1.1  0 0 0  1 1 1  0.2 0 0  0\n"
			"}\n";
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docManyBlobs );
		const AgentDiagnostic* d = findCode( diags, "DESIGN_SDF_BLEND_SCALE" );
		Check( d != nullptr, "BOUNDED LIST: 5 offending smin joints on one chunk fires" );
		if( d ) {
			Check( d->message.find( "5 smin joints" ) != std::string::npos,
			       "...reporting the true offender count (5)" );
			Check( d->message.find( "and 2 more" ) != std::string::npos,
			       "...bounding the named list to 3, stating the truncation (2 more)" );
		}
	}

	//------------------------------------------------------------------
	// Review-round B (2026-08-24): P1 (compare against the smallest
	// PART IN THE JOIN, not just the current one), P2 (a first-part smin
	// is geometrically inert), and the size-mismatch discriminator (fire
	// only on a genuine small-feature-into-big-mass join, not a gradual/
	// comparable-size meld).
	//------------------------------------------------------------------
	{
		// P1 RED-PROVE: thin-end-first -- a TINY part authored FIRST
		// (r=0.05), a BIG roundcone smin-joined onto it SECOND at
		// k=0.2.  Under a current-part-only comparison this is SILENT
		// (dim=min(1,1)=1, max~0.333, k=0.2 <= 0.333) -- the false
		// negative the brief calls out.  Comparing against the smallest
		// part WITHIN REACH (here, the tiny predecessor) catches it:
		// dim_effective=0.05, max~0.0167, k=0.2 fires.
		const std::string docThinEndFirst =
			"RISE ASCII SCENE 7\n"
			"sdf_geometry\n{\n\tname thin_first_chain\n"
			"\tpart\tsphere union 0  0 0 0  0 0 0  1 1 1  0.05 0 0  0\n"
			"\tpart\troundcone smin 0.2  0 0.05 0  0 0 0  1 1 1  1 1 0.5  0\n"
			"}\n";
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docThinEndFirst );
		const AgentDiagnostic* d = findCode( diags, "DESIGN_SDF_BLEND_SCALE" );
		Check( d != nullptr,
		       "P1 RED-PROVE: thin-end-first (tiny part FIRST, big roundcone smin-joined onto it "
		       "SECOND at k=0.2) fires -- a current-part-only comparison would stay silent" );
		if( d ) Check( d->message.find( "part 2" ) != std::string::npos,
		               "...naming the SECOND part (the join), not the tiny first part" );
	}
	{
		// P2 GREEN-PROVE: a chunk whose FIRST part is authored `smin` --
		// the running field starts empty, so it composes against nothing
		// and dissolves nothing (SDFGeometry.cpp's own ParsePartLines
		// comment).  Even with an extreme k, part 1 must not fire.
		const std::string docFirstPartSmin =
			"RISE ASCII SCENE 7\n"
			"sdf_geometry\n{\n\tname smin_first\n"
			"\tpart\troundcone smin 5.0  0 0 0  0 0 0  1 1 1  0.02 0.004 0.05  0\n"
			"\tpart\tsphere union 0.0  0 0.05 0  0 0 0  1 1 1  0.05 0 0  0\n"
			"}\n";
		Check( !hasCode( AgentSession::ValidateText( docFirstPartSmin ), "DESIGN_SDF_BLEND_SCALE" ),
		       "P2 GREEN-PROVE: a first-part `smin` (geometrically inert -- the running field starts "
		       "empty) never fires regardless of k" );
	}
	{
		// P3 GREEN-PROVE: a heavily-ROUNDED roundbox -- a=b=c=0.05 but
		// round=0.5 -- reaches max(half-extent, round) per axis
		// (ComputeBounds, SDFGeometry.cpp ~500-516), so its TRUE
		// characteristic dimension is 0.5, not 0.05.  The predecessor
		// part is placed far out of reach so this isolates
		// SDFPartCharacteristicDim_'s roundbox case alone (no P1/
		// discriminator interaction).  Pre-P3, min(a,b,c)=0.05 would
		// have put max~0.0167 and fired on k=0.1; post-P3 the true
		// dim=0.5 puts max~0.1667 and stays silent.
		const std::string docRoundedBox =
			"RISE ASCII SCENE 7\n"
			"sdf_geometry\n{\n\tname rounded_box_chunk\n"
			"\tpart\tsphere union 0  100 100 100  0 0 0  1 1 1  0.01 0 0  0\n"
			"\tpart\troundbox smin 0.1  0 0 0  0 0 0  1 1 1  0.05 0.05 0.05  0.5\n"
			"}\n";
		Check( !hasCode( AgentSession::ValidateText( docRoundedBox ), "DESIGN_SDF_BLEND_SCALE" ),
		       "P3 GREEN-PROVE: a heavily-rounded roundbox (a=b=c=0.05, round=0.5) is judged by "
		       "max(half-extent, round)=0.5, not the bare 0.05 half-extents -- stays silent at k=0.1" );
	}
	{
		// Discriminator GREEN-PROVE: a real BuildBlendedVessel draw
		// (insert_geometry_scaffold "blended_vessel" "vjit0" 0.5 0.0 0.7,
		// verbatim from AgentSession::ReadDocument()'s own output) -- a
		// turned-profile taper where each roundcone's base radius
		// EXACTLY matches the previous segment's tip radius.  Before the
		// discriminator this fired on 44/45 jittered draws across the
		// family's whole size/detail/aspect range; a comparably-sized,
		// continuous join is legitimate authoring, not the law's target.
		const std::string docVessel =
			"RISE ASCII SCENE 7\n"
			"sdf_geometry\n{\n\tname tmpl_vjit0_vessel\n"
			"\tpart\troundcone union 0.0000  0.0000 0.0000 0.0000  0.0000 0.0000 0.0000  1.0000 1.0000 1.0000  0.0569 0.0729 0.0456  0.0000\n"
			"\tpart\troundcone smin 0.1113  0.0000 0.0456 0.0000  0.0000 0.0000 0.0000  1.0000 1.0000 1.0000  0.0729 0.2165 0.1794  0.0000\n"
			"\tpart\troundcone smin 0.0830  0.0000 0.2250 0.0000  0.0000 0.0000 0.0000  1.0000 1.0000 1.0000  0.2165 0.1225 0.1047  0.0000\n"
			"\tpart\tbox subtract 0.0000  0.0000 -0.0853 0.0000  0.0000 0.0000 0.0000  1.0000 1.0000 1.0000  0.1138 0.0853 0.1138  0.0000\n"
			"}\n";
		Check( !hasCode( AgentSession::ValidateText( docVessel ), "DESIGN_SDF_BLEND_SCALE" ),
		       "DISCRIMINATOR GREEN-PROVE: a real blended_vessel scaffold draw (turned-profile taper, "
		       "matched base/tip radii at every joint) stays silent" );
	}
	{
		// Discriminator GREEN-PROVE: the mermaid-tail SPINE, verbatim
		// from scenes/Benchmarks/dreamscape_coral_queens_hour.RISEscene
		// -- a gradual, equal-ish-radius metaball chain (0.05 -> 0.175
		// across 8 joints).  Before the discriminator this fired on
		// every one of its 8 joints.
		const std::string docMermaid =
			"RISE ASCII SCENE 7\n"
			"sdf_geometry\n{\n\tname mermaid_tail\n"
			"\tpart\tsphere union 0  -1.72 0.34 0  0 0 0  1 1 1  0.05 0 0  0.0\n"
			"\tpart\tsphere smin 0.16  -1.56 0.22 0  0 0 0  1 1 1  0.075 0 0  0.0\n"
			"\tpart\tsphere smin 0.13  -1.38 0.10 0  0 0 0  1 1 1  0.095 0 0  0.0\n"
			"\tpart\tsphere smin 0.12  -1.16 0.01 0  0 0 0  1 1 1  0.11 0 0  0.0\n"
			"\tpart\tsphere smin 0.12  -0.92 -0.04 0  0 0 0  1 1 1  0.13 0 0  0.0\n"
			"\tpart\tsphere smin 0.12  -0.66 -0.03 0  0 0 0  1 1 1  0.15 0 0  0.0\n"
			"\tpart\tsphere smin 0.12  -0.40 0.05 0  0 0 0  1 1 1  0.165 0 0  0.0\n"
			"\tpart\tsphere smin 0.12  -0.16 0.16 0  0 0 0  1 1 1  0.175 0 0  0.0\n"
			"\tpart\tsphere smin 0.12  0.05 0.28 0  0 0 0  1 1 1  0.165 0 0  0.0\n"
			"}\n";
		Check( !hasCode( AgentSession::ValidateText( docMermaid ), "DESIGN_SDF_BLEND_SCALE" ),
		       "DISCRIMINATOR GREEN-PROVE: the mermaid-tail spine (gradual equal-radius metaball "
		       "chain) stays silent" );
	}
	{
		// Discriminator RED-PROVE (still fires): the apothecary-cat ear
		// AND se_creature's limb (scenes/Tests/Geometry/superellipsoid_
		// stress.RISEscene) both survive the discriminator -- a genuine
		// size mismatch (torso dim 0.8 vs limb dim 0.18, ratio 4.4x)
		// clears kSizeMismatchGate comfortably.
		const std::string docCreature =
			"RISE ASCII SCENE 7\n"
			"sdf_geometry\n{\n\tname se_creature\n"
			"\tpart\tsuperellipsoid union 0  0 0 0  0 0 0  1.0 1.25 0.8  1.0 0.5 0.7  0\n"
			"\tpart\tsphere smin 0.45  0 1.55 0  0 0 0  1 1 1  0.52 0 0  0\n"
			"\tpart\troundcone smin 0.4  -0.75 -0.3 0  0 0 35  1 1 1  0.34 0.18 1.25  0\n"
			"\tpart\troundcone smin 0.4  0.75 -0.3 0  0 0 -35  1 1 1  0.34 0.18 1.25  0\n"
			"}\n";
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( docCreature );
		const AgentDiagnostic* d = findCode( diags, "DESIGN_SDF_BLEND_SCALE" );
		Check( d != nullptr, "DISCRIMINATOR RED-PROVE: se_creature's limb joints still fire "
		       "(genuine 4.4x size mismatch against the torso)" );
		if( d ) Check( d->message.find( "part 3" ) != std::string::npos &&
		               d->message.find( "part 4" ) != std::string::npos,
		               "...naming BOTH limb joints" );
	}
}

//----------------------------------------------------------------------
// GPT slice item 4 (2026-08-24): DESIGN_ENV_REFLECTION, the env-
// reflection advisory -- a low-roughness metallic pbr_metallic_
// roughness_material bound to an object, coexisting with a strongly
// saturated (HSV saturation >= 0.5) env dome bound as the rasterizer's
// radiance_map.  Deliberately narrow (see the diagnostic code's own doc
// comment): only a flat uniformcolor_painter dome, only pbr_metallic_
// roughness_material with LITERAL metallic/roughness.
//----------------------------------------------------------------------
static void RunEnvReflectionScanTest()
{
	std::printf( "[design-note] GPT slice item 4: DESIGN_ENV_REFLECTION\n" );

	auto hasCode = []( const std::vector<AgentDiagnostic>& diags, const std::string& code ) {
		for( const AgentDiagnostic& d : diags ) if( d.code == code ) return true;
		return false;
	};
	auto findCode = []( const std::vector<AgentDiagnostic>& diags, const std::string& code ) -> const AgentDiagnostic* {
		for( const AgentDiagnostic& d : diags ) if( d.code == code ) return &d;
		return nullptr;
	};

	// Shared scaffolding: a rasterizer binding `dome_pnt` as radiance_map,
	// a sphere geometry, and a `metal_obj` bound to a
	// pbr_metallic_roughness_material -- everything held fixed except the
	// dome colour / metallic / roughness / binding under test.
	auto Doc = []( const std::string& domeColor, const std::string& metallic,
	              const std::string& roughness, bool bindMaterial ) {
		std::string s =
			"RISE ASCII SCENE 7\n"
			"pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise false\n"
			"\tradiance_map dome_pnt\n}\n\n"
			"uniformcolor_painter\n{\n\tname dome_pnt\n\tcolor " + domeColor + "\n}\n\n"
			"uniformcolor_painter\n{\n\tname base_pnt\n\tcolor 0.02 0.02 0.02\n}\n\n"
			"pbr_metallic_roughness_material\n{\n\tname metal_mat\n\tbase_color base_pnt\n"
			"\tmetallic " + metallic + "\n\troughness " + roughness + "\n}\n\n"
			"sphere_geometry\n{\n\tname sph\n\tradius 0.5\n}\n\n";
		if( bindMaterial )
			s += "standard_object\n{\n\tname metal_obj\n\tgeometry sph\n\tmaterial metal_mat\n}\n";
		return s;
	};

	{
		// RED-PROVE: a strongly saturated dusk-blue dome (0.05 0.02 0.5,
		// saturation 0.96) + a near-mirror metal (metallic 1.0,
		// roughness 0.05) bound to an object.
		const std::string doc = Doc( "0.05 0.02 0.5", "1.0", "0.05", /*bindMaterial=*/true );
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( doc );
		const AgentDiagnostic* d = findCode( diags, "DESIGN_ENV_REFLECTION" );
		Check( d != nullptr,
		       "RED-PROVE: a saturated dusk-blue dome + a bound near-mirror metal fires "
		       "DESIGN_ENV_REFLECTION" );
		if( d ) {
			Check( d->severity == AgentDiagnostic::Severity::Info, "...at Info severity" );
			Check( d->message.find( "metal_mat" ) != std::string::npos, "...naming the material" );
			Check( d->message.find( "dome_pnt" ) != std::string::npos, "...naming the dome painter" );
			Check( d->message.find( "this is fine, ignore" ) != std::string::npos ||
			       d->message.find( "fine -- ignore" ) != std::string::npos,
			       "...self-disarming (a saturated dome can be the deliberate look)" );
		}
	}
	{
		// GREEN-PROVE: a pale, barely-saturated sky-blue dome (0.55 0.65
		// 0.7, saturation ~0.214) -- same metal -- stays silent.
		const std::string doc = Doc( "0.55 0.65 0.7", "1.0", "0.05", /*bindMaterial=*/true );
		Check( !hasCode( AgentSession::ValidateText( doc ), "DESIGN_ENV_REFLECTION" ),
		       "GREEN-PROVE: a pale, barely-saturated sky dome (saturation ~0.21) stays silent" );
	}
	{
		// GREEN-PROVE: the SAME saturated dome, but the material is NOT
		// metallic (metallic 0.0, ordinary dielectric) -- stays silent.
		const std::string doc = Doc( "0.05 0.02 0.5", "0.0", "0.05", /*bindMaterial=*/true );
		Check( !hasCode( AgentSession::ValidateText( doc ), "DESIGN_ENV_REFLECTION" ),
		       "GREEN-PROVE: a saturated dome with a NON-metallic material (metallic 0.0) stays "
		       "silent" );
	}
	{
		// GREEN-PROVE: the SAME saturated dome + metal, but roughness is
		// too high to read as a near-mirror (0.6) -- stays silent.
		const std::string doc = Doc( "0.05 0.02 0.5", "1.0", "0.6", /*bindMaterial=*/true );
		Check( !hasCode( AgentSession::ValidateText( doc ), "DESIGN_ENV_REFLECTION" ),
		       "GREEN-PROVE: a saturated dome with a HIGH-roughness metal (0.6) stays silent" );
	}
	{
		// GREEN-PROVE: the SAME saturated dome + qualifying metal, but it
		// is never bound to any standard_object -- stays silent
		// (materialObjectCounts gate).
		const std::string doc = Doc( "0.05 0.02 0.5", "1.0", "0.05", /*bindMaterial=*/false );
		Check( !hasCode( AgentSession::ValidateText( doc ), "DESIGN_ENV_REFLECTION" ),
		       "GREEN-PROVE: a qualifying material that is never BOUND to an object stays silent" );
	}
}

int main()
{
	// G2 (2026-08-10): the build-plan gate is ON by default in production (a
	// construction site nobody remembered to touch gets it -- the fail-safe
	// polarity).  This binary does not test the gate, and its fixtures insert
	// geometry directly, so opt OUT once here rather than at every session.
	// The gate's own coverage lives in AgentChunkCrudTest's G2 block, which
	// re-enables it explicitly per session.
	RISE::Agent::AgentSession::SetBuildPlanGateDefaultEnabled( false );
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
			// ... and, as of the GPT-tolerance fix (2026-08-24), so does an
			// EXPLICIT EMPTY STRING.  This REVERSES an earlier decision (see
			// git history) that deliberately routed `{"text":""}` to the text
			// form and reported EMPTY_DOCUMENT -- sound for a caller who
			// TYPED an empty string on purpose, but the GPT wire transport's
			// SDK convention fills every unset optional string with "" rather
			// than omitting it, so a GPT-backed session could not express
			// "no text" at all: it retried this exact call three times
			// verbatim against a live, non-empty head, each time told its
			// (unintended) empty candidate was empty.  Whitespace-only and
			// comments-only strings are UNCHANGED below -- only the literal
			// `""` is reinterpreted, which is the one shape that convention
			// produces.
			const JsonValue emptyText = call( rpc, 109, "{\"text\":\"\"}" );
			Check( !emptyText.has( "error" ) &&
			       emptyText.get( "result" ).get( "validated" ).asString() == "head",
			       "validate {text:\"\"} now takes the HEAD form -- an explicit empty string reads "
			       "as absent, same as omission or null" );
			Check( emptyText.get( "result" ).get( "headVersion" ).isObject(),
			       "validate {text:\"\"} stamps a headVersion (it validated the live head, not a "
			       "candidate)" );
			Check( emptyText.get( "result" ).get( "diagnostics" ).size() ==
			       noArg.get( "result" ).get( "diagnostics" ).size(),
			       "validate {text:\"\"} reports the SAME diagnostics as validate {} -- byte-identical "
			       "head form, not a distinct EMPTY_DOCUMENT verdict" );
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
	// instead of ReadDocument(), because kWedgeScene's own document (2
	// standard_object -- below BOTH creative-richness gates, see
	// RunValidateDesignDiagnosticsScanTest above) carries no diagnostics of
	// ANY severity pre-edit, so there is no "clean but noisy" head built
	// from THIS fixture to accidentally pass on.  (A scene with >=3
	// standard_object and no scalar_painter WOULD be clean-but-noisy since
	// P2.b -- errors==0 but an Info DESIGN_SCALAR_PIPE_UNUSED entry present
	// -- kWedgeScene is deliberately shaped to stay below that gate so this
	// section's ERROR-severity assertions below are not confounded by it.)
	// (Mutation probe: swapping ReadDocument() for a constant left every
	// other assertion in this file green.)
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
				// "Clean" here means no Error/Warning diagnostic, not a
				// literal empty array: adoption-polish item 2's
				// DESIGN_ORPHANED_PAINTERS correctly (Info-severity) flags
				// kGoodScene's `p` -- a `uniformcolor_painter` nothing in
				// that minimal fixture ever references -- which is a real,
				// harmless finding, not the wedge's kind of error.
				bool anyErrorOrWarning = false;
				{
					const JsonValue& cd = cleanEnv.get( "result" ).get( "diagnostics" );
					for( std::size_t i = 0; i < cd.size(); ++i ) {
						const std::string sev = cd.at( i ).get( "severity" ).asString();
						if( sev == "error" || sev == "warning" ) anyErrorOrWarning = true;
					}
				}
				Check( !cleanEnv.has( "error" ) &&
				       cleanEnv.get( "result" ).get( "diagnostics" ).isArray() &&
				       !anyErrorOrWarning &&
				       cleanEnv.get( "result" ).get( "validated" ).asString() == "text",
				       "the text form on a CLEAN candidate stays clean (no Error/Warning) even "
				       "while the head is wedged (the two forms are genuinely independent)" );
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
	RunDesignRepeatedCopiesScanTest();
	RunValidateDesignDiagnosticsScanTest();
	RunValidateDesignDiagnosticsCarrierTest();
	RunAdoptionPolishScanTest();
	RunMaterialsRealismScanTest();
	RunSDFBlendScaleScanTest();
	RunEnvReflectionScanTest();
	RunUnboundMaterialAndFlatAlbedoScanTest();
	RunFlatReliefScanTest();

	std::printf( "=== AgentReadValidateTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
