//////////////////////////////////////////////////////////////////////
//
//  AgentFirstSliceTest.cpp - Facet 5 (agentic surface) slice 0c: the
//    CAPSTONE end-to-end test.
//
//  Drives the WHOLE agentic loop headless, with NO LLM and NO subprocess,
//  through the JSON-RPC AgentRpcDispatcher -- the SINGLE place the loop is
//  realized (docs/agentic-redesign/50-agentic-surface.md §2.1 transport).
//  Driving the dispatcher IS the loop the `rise --agent-stdio` CLI runs;
//  the CLI just wraps HandleLine in a stdin/stdout read-print loop (the
//  GATE exercises that wiring separately with a real subprocess).
//
//  Everything is verified by PARSING the JSON-RPC responses with the same
//  hand-rolled JsonParse the transport serializes with -- proving the codec
//  round-trips the real message set (a base64 PNG string, a multi-line
//  document with embedded quotes/newlines/tabs, nested diagnostics).
//
//  Sequence (each an RPC through the dispatcher):
//    * read_document   -> the head text carries an expected chunk keyword
//                         and round-trips through the CST parser.
//    * read_schema     -> {keyword:"sphere_geometry"} mentions "radius".
//    * validate (bad)  -> a bogus param yields an UNKNOWN_PARAMETER
//                         diagnostic with offset>0; RED-PROVE: a GOOD scene
//                         yields zero error diagnostics.
//    * propose_patch   -> set a real param; then read_document shows the new
//                         value (the loop mutated the head).
//    * render          -> ok==true, dims match the film; capture meanR/G/B.
//    * read_image      -> png_base64 non-empty + decodes to a PNG signature.
//    * loop coherence  -> propose a VISIBLE change, render again, assert the
//                         channel means shifted FAR beyond the noise floor.
//    * error paths     -> unknown method -32601, malformed line -32700,
//                         missing required param -32602.
//
//  Self-contained: an inline native-v7 scene (reused from slice 0b), OIDN
//  off, no RISE_MEDIA_PATH.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentRpc.h"
#include "../src/Library/Agent/Json.h"
#include "../src/Library/Agent/Base64.h"
#include "../src/Library/Cst/Cst.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
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

// The same inline native-v7 scene the slice-0b test uses: a lit diffuse
// sphere + an area emitter, PT at low spp with OIDN off, renders non-black.
static const char* const kScene =
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
	std::string path = dir + name;
	std::ofstream f( path.c_str(), std::ios::binary );
	if( !f ) return std::string();
	f.write( text.data(), (std::streamsize)text.size() );
	f.close();
	return path;
}

// Parse a JSON-RPC response line; assert it is a well-formed 2.0 envelope
// echoing `expectId`.  Returns the parsed envelope (Null on parse failure).
static JsonValue ParseResponse( const std::string& line, double expectId )
{
	JsonValue env; std::string err;
	const bool ok = JsonParse( line, env, err );
	Check( ok, "response line parses as JSON (" + ( ok ? std::string("ok") : err ) + ")" );
	if( !ok ) return JsonValue::MakeNull();
	Check( env.isObject(), "response is a JSON object" );
	Check( env.get( "jsonrpc" ).asString() == "2.0", "response carries jsonrpc==2.0" );
	Check( env.get( "id" ).asNumber( -999 ) == expectId, "response echoes the request id" );
	return env;
}

// Build a JSON-RPC request line for `method` with `params` under `id`.
static std::string Req( double id, const std::string& method, const JsonValue& params )
{
	JsonValue r = JsonValue::MakeObject();
	r.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
	r.set( "id", JsonValue::MakeNumber( id ) );
	r.set( "method", JsonValue::MakeString( method ) );
	r.set( "params", params );
	return JsonSerialize( r );
}

int main()
{
	std::printf( "=== AgentFirstSliceTest (Facet 5 slice 0c: JSON-RPC end-to-end loop) ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_slice0c.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the scene to a temp file" );

	// Construct the dispatcher on the inline scene -- the SAME object the
	// `rise --agent-stdio` CLI drives per stdin line.
	std::unique_ptr<AgentSession> session = AgentSession::LoadFromFile( scenePath );
	Check( session != nullptr, "AgentSession::LoadFromFile loads the native-v7 scene" );
	if( !session ) { std::printf( "cannot continue without a session\n" ); return 1; }
	AgentRpcDispatcher rpc( std::move( session ) );

	//----------------------------------------------------------------------
	// read_document — the head text, round-tripped through the codec + CST.
	//----------------------------------------------------------------------
	std::printf( "[read_document]\n" );
	std::string headText;
	{
		const std::string resp = rpc.HandleLine( Req( 1, "read_document", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 1 );
		const JsonValue& result = env.get( "result" );
		Check( result.isObject(), "read_document returns a result object" );
		headText = result.get( "document" ).asString();
		Check( headText.find( "sphere_geometry" ) != std::string::npos,
		       "document carries the expected 'sphere_geometry' chunk keyword" );
		Check( result.get( "hasDocument" ).asBool(), "hasDocument is true" );
		// The document string (multi-line, embedded tabs/newlines/braces)
		// survived a JSON serialize+parse round-trip byte-identically: it
		// still parses back through the CST parser.
		Cst::Document rt = Cst::ParseToCst( headText );
		Check( Cst::SerializeCst( rt ) == headText,
		       "document round-trips through JSON codec AND the CST parser (embedded newlines/tabs/quotes intact)" );
	}

	//----------------------------------------------------------------------
	// read_schema {keyword:"sphere_geometry"} — mentions "radius".
	//----------------------------------------------------------------------
	std::printf( "[read_schema]\n" );
	{
		JsonValue params = JsonValue::MakeObject();
		params.set( "keyword", JsonValue::MakeString( "sphere_geometry" ) );
		const std::string resp = rpc.HandleLine( Req( 2, "read_schema", params ) );
		JsonValue env = ParseResponse( resp, 2 );
		const JsonValue& schema = env.get( "result" ).get( "schema" );
		Check( schema.isObject(), "read_schema returns the schema as a nested object" );
		// The schema is a nested object; re-serialize it and check it mentions
		// the sphere's 'radius' param (robust to key ordering).
		const std::string schemaStr = JsonSerialize( schema );
		Check( schemaStr.find( "radius" ) != std::string::npos,
		       "sphere_geometry schema mentions 'radius'" );
	}

	//----------------------------------------------------------------------
	// validate — a bogus param yields UNKNOWN_PARAMETER with offset>0;
	// RED-PROVE a GOOD scene yields zero error diagnostics.
	//----------------------------------------------------------------------
	std::printf( "[validate] bad scene -> UNKNOWN_PARAMETER; good scene -> clean\n" );
	{
		// A minimal scene with a bogus param on the sphere.
		const std::string badScene =
			"RISE ASCII SCENE 7\n"
			"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n\tnot_a_real_param 3\n}\n";
		JsonValue params = JsonValue::MakeObject();
		params.set( "text", JsonValue::MakeString( badScene ) );
		const std::string resp = rpc.HandleLine( Req( 3, "validate", params ) );
		JsonValue env = ParseResponse( resp, 3 );
		const JsonValue& diags = env.get( "result" ).get( "diagnostics" );
		Check( diags.isArray(), "validate returns a diagnostics array" );
		bool foundUnknownParam = false;
		for( std::size_t i = 0; i < diags.size(); ++i ) {
			const JsonValue& d = diags.at( i );
			if( d.get( "code" ).asString() == "UNKNOWN_PARAMETER" ) {
				foundUnknownParam = true;
				Check( d.get( "offset" ).asNumber() > 0.0,
				       "UNKNOWN_PARAMETER diagnostic has offset>0 (localized)" );
			}
		}
		Check( foundUnknownParam, "validate(bad scene) reports an UNKNOWN_PARAMETER diagnostic" );
	}
	{
		// RED-PROVE: the ORIGINAL good head validates with zero error diagnostics.
		JsonValue params = JsonValue::MakeObject();
		params.set( "text", JsonValue::MakeString( headText ) );
		const std::string resp = rpc.HandleLine( Req( 4, "validate", params ) );
		JsonValue env = ParseResponse( resp, 4 );
		const JsonValue& diags = env.get( "result" ).get( "diagnostics" );
		int errorCount = 0;
		for( std::size_t i = 0; i < diags.size(); ++i )
			if( diags.at( i ).get( "severity" ).asString() == "error" ) ++errorCount;
		Check( errorCount == 0, "validate(good head) reports ZERO error diagnostics (red-prove)" );
	}

	//----------------------------------------------------------------------
	// propose_patch — set a real param; read_document shows the new value.
	//----------------------------------------------------------------------
	std::printf( "[propose_patch] set a real param; head mutates\n" );
	{
		JsonValue params = JsonValue::MakeObject();
		params.set( "target", JsonValue::MakeString( "pnt_albedo" ) );
		params.set( "param",  JsonValue::MakeString( "color" ) );
		params.set( "value",  JsonValue::MakeString( "0.9 0.1 0.1" ) );
		const std::string resp = rpc.HandleLine( Req( 5, "propose_patch", params ) );
		JsonValue env = ParseResponse( resp, 5 );
		Check( env.get( "result" ).get( "applied" ).asBool(),
		       "propose_patch(pnt_albedo.color) applied==true" );

		// read_document again -> the new value is present (the loop mutated head).
		const std::string resp2 = rpc.HandleLine( Req( 6, "read_document", JsonValue::MakeObject() ) );
		JsonValue env2 = ParseResponse( resp2, 6 );
		const std::string doc2 = env2.get( "result" ).get( "document" ).asString();
		Check( doc2.find( "0.9 0.1 0.1" ) != std::string::npos,
		       "read_document after propose_patch carries the new value (head mutated through the loop)" );
	}

	//----------------------------------------------------------------------
	// render — ok, dims match the film; capture the channel-mean signature.
	//----------------------------------------------------------------------
	std::printf( "[render]\n" );
	double firstMeanR = 0.0, firstMeanG = 0.0, firstMeanB = 0.0;
	{
		const std::string resp = rpc.HandleLine( Req( 7, "render", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 7 );
		const JsonValue& r = env.get( "result" );
		Check( r.get( "ok" ).asBool(), "render ok==true" );
		Check( r.get( "width" ).asNumber() == 24.0 && r.get( "height" ).asNumber() == 24.0,
		       "render dims match the film (24x24)" );
		firstMeanR = r.get( "meanR" ).asNumber();
		firstMeanG = r.get( "meanG" ).asNumber();
		firstMeanB = r.get( "meanB" ).asNumber();
		// render stays lean: no image bytes in the render result.
		Check( !r.has( "png_base64" ), "render result does NOT carry the image bytes (read_image does)" );
	}

	//----------------------------------------------------------------------
	// read_image — png_base64 non-empty + decodes to a PNG signature.
	//----------------------------------------------------------------------
	std::printf( "[read_image] base64 PNG decodes to \\x89PNG\n" );
	{
		const std::string resp = rpc.HandleLine( Req( 8, "read_image", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 8 );
		const std::string b64 = env.get( "result" ).get( "png_base64" ).asString();
		Check( !b64.empty(), "read_image returns a non-empty png_base64 field" );
		// Well-formedness: length is a multiple of 4 (standard padded base64)
		// and comfortably above a minimum-PNG floor.
		Check( b64.size() % 4 == 0, "png_base64 length is a multiple of 4 (well-formed padding)" );
		Check( b64.size() > 60, "png_base64 length exceeds a minimum-PNG floor" );
		// Decode with our base64 and confirm the PNG magic — proves the codec
		// round-trips a real binary payload through a JSON string field.
		std::vector<unsigned char> png;
		Check( Base64Decode( b64, png ), "png_base64 decodes cleanly" );
		Check( png.size() >= 8 &&
		       png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G' &&
		       png[4] == 0x0D && png[5] == 0x0A && png[6] == 0x1A && png[7] == 0x0A,
		       "decoded bytes carry the \\x89PNG signature (base64 round-trips a real PNG)" );
	}

	//----------------------------------------------------------------------
	// loop coherence — a VISIBLE edit then a re-render shifts the channel
	// means FAR beyond the render noise floor (edit flowed Doc->derive->render
	// through the loop).  First measure the same-head noise floor.
	//----------------------------------------------------------------------
	std::printf( "[coherence] visible edit shifts the render beyond the noise floor\n" );
	double noiseFloor = 0.0;
	{
		const std::string resp = rpc.HandleLine( Req( 9, "render", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 9 );
		const JsonValue& r = env.get( "result" );
		noiseFloor =
			std::fabs( r.get( "meanR" ).asNumber() - firstMeanR ) +
			std::fabs( r.get( "meanG" ).asNumber() - firstMeanG ) +
			std::fabs( r.get( "meanB" ).asNumber() - firstMeanB );
		std::printf( "  same-head mean drift (noise floor) = %.5f\n", noiseFloor );
	}
	{
		// A large, obvious recolour of the sphere albedo.
		JsonValue params = JsonValue::MakeObject();
		params.set( "target", JsonValue::MakeString( "pnt_albedo" ) );
		params.set( "param",  JsonValue::MakeString( "color" ) );
		params.set( "value",  JsonValue::MakeString( "0.9 0.05 0.05" ) );
		const std::string presp = rpc.HandleLine( Req( 10, "propose_patch", params ) );
		JsonValue penv = ParseResponse( presp, 10 );
		Check( penv.get( "result" ).get( "applied" ).asBool(), "coherence: recolour edit applied" );

		const std::string rresp = rpc.HandleLine( Req( 11, "render", JsonValue::MakeObject() ) );
		JsonValue renv = ParseResponse( rresp, 11 );
		const JsonValue& r = renv.get( "result" );
		Check( r.get( "ok" ).asBool(), "coherence: post-edit render ok" );
		const double editShift =
			std::fabs( r.get( "meanR" ).asNumber() - firstMeanR ) +
			std::fabs( r.get( "meanG" ).asNumber() - firstMeanG ) +
			std::fabs( r.get( "meanB" ).asNumber() - firstMeanB );
		std::printf( "  post-edit mean shift = %.5f  (noise floor = %.5f)\n", editShift, noiseFloor );
		Check( editShift > 10.0 * noiseFloor + 0.002,
		       "post-edit channel means shift FAR beyond the noise floor (loop coherence: edit->derive->render)" );
	}

	//----------------------------------------------------------------------
	// JSON-RPC error paths — unknown method, malformed line, missing param.
	//----------------------------------------------------------------------
	std::printf( "[errors] -32601 / -32700 / -32602\n" );
	{
		// Unknown method -> -32601.
		const std::string resp = rpc.HandleLine( Req( 12, "no_such_method", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 12 );
		Check( env.get( "error" ).get( "code" ).asNumber() == -32601.0,
		       "unknown method -> error.code == -32601" );
	}
	{
		// Malformed JSON line -> -32700 (id echoes null on an un-attributable error).
		const std::string resp = rpc.HandleLine( "{ this is not valid json " );
		JsonValue env; std::string err;
		Check( JsonParse( resp, env, err ), "the -32700 response is itself valid JSON" );
		Check( env.get( "error" ).get( "code" ).asNumber() == -32700.0,
		       "malformed line -> error.code == -32700" );
		Check( env.get( "id" ).isNull(), "malformed line -> id is null (un-attributable)" );
	}
	{
		// Missing required param (validate needs 'text') -> -32602.
		const std::string resp = rpc.HandleLine( Req( 13, "validate", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 13 );
		Check( env.get( "error" ).get( "code" ).asNumber() == -32602.0,
		       "missing required param -> error.code == -32602" );
	}
	{
		// Envelope missing "method" but with a determinable id -> -32600
		// Invalid Request, and the id is ECHOED (the spec: on an Invalid
		// Request whose id is recoverable, echo it rather than nulling it).
		const std::string resp = rpc.HandleLine( "{\"jsonrpc\":\"2.0\",\"id\":5}" );
		JsonValue env; std::string err;
		Check( JsonParse( resp, env, err ), "the -32600 (missing method) response is itself valid JSON" );
		Check( env.get( "error" ).get( "code" ).asNumber() == -32600.0,
		       "missing 'method' -> error.code == -32600 (Invalid Request)" );
		Check( env.get( "id" ).asNumber( -999 ) == 5.0,
		       "missing 'method' -> id is echoed (id:5, determinable)" );
	}
	{
		// A non-object envelope (a JSON array) -> -32600 Invalid Request with
		// id:null (no id is determinable from a non-object).
		const std::string resp = rpc.HandleLine( "[1,2,3]" );
		JsonValue env; std::string err;
		Check( JsonParse( resp, env, err ), "the -32600 (non-object) response is itself valid JSON" );
		Check( env.get( "error" ).get( "code" ).asNumber() == -32600.0,
		       "non-object envelope -> error.code == -32600 (Invalid Request)" );
		Check( env.get( "id" ).isNull(), "non-object envelope -> id is null (un-attributable)" );
	}

	//----------------------------------------------------------------------
	// render samples guard — a hostile {"samples":1e999} parses to +inf,
	// {"samples":1e999... via a NaN-producing huge literal} likewise; both
	// MUST be rejected as -32602 BEFORE the narrowing static_cast<int>(inf)
	// (which is UB).  The guard is an explicit int32-range comparison rather
	// than std::isfinite precisely so it survives the production -ffast-math
	// (-ffinite-math-only) build, under which the isfinite intrinsic folds to
	// `true` and the guard would be stripped as dead code.  We drive the raw
	// JSON line (not Req(), whose MakeNumber takes a finite double) so the
	// dispatcher's OWN parser produces the infinity.  A normal {"samples":2}
	// must still render ok (the guard is not over-broad).
	std::printf( "[render samples guard] 1e999 -> -32602; normal samples still ok\n" );
	{
		// +inf via an out-of-double-range literal.
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"render\",\"params\":{\"samples\":1e999}}" );
		JsonValue env = ParseResponse( resp, 14 );
		Check( env.has( "error" ), "render(samples:1e999) returns an error (not a success result)" );
		Check( !env.has( "result" ), "render(samples:1e999) has NO result field" );
		Check( env.get( "error" ).get( "code" ).asNumber() == -32602.0,
		       "render(samples:1e999) -> error.code == -32602 (rejected before UB cast; survives -ffast-math)" );
	}
	{
		// -inf sibling: a huge NEGATIVE literal also overflows double to -inf.
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"render\",\"params\":{\"samples\":-1e999}}" );
		JsonValue env = ParseResponse( resp, 15 );
		Check( env.get( "error" ).get( "code" ).asNumber() == -32602.0,
		       "render(samples:-1e999) -> error.code == -32602 (negative-infinity sibling)" );
	}
	{
		// A finite-but-out-of-int32-range literal is ALSO rejected (the cast
		// would truncate/overflow) -- proves the range bound, not just finiteness.
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"render\",\"params\":{\"samples\":1e18}}" );
		JsonValue env = ParseResponse( resp, 16 );
		Check( env.get( "error" ).get( "code" ).asNumber() == -32602.0,
		       "render(samples:1e18) -> error.code == -32602 (out-of-int32-range rejected)" );
	}
	{
		// Not over-broad: a normal in-range {"samples":2} still renders ok.
		JsonValue params = JsonValue::MakeObject();
		params.set( "samples", JsonValue::MakeNumber( 2.0 ) );
		const std::string resp = rpc.HandleLine( Req( 17, "render", params ) );
		JsonValue env = ParseResponse( resp, 17 );
		Check( env.has( "result" ), "render(samples:2) returns a success result (guard not over-broad)" );
		Check( env.get( "result" ).get( "ok" ).asBool(), "render(samples:2) ok==true" );
	}

	std::printf( "=== AgentFirstSliceTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
