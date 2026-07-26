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
//    * render          -> ok==true, dims match the film; capture meanR/G/B;
//                         default agent perception reports exact memory.
//    * read_image      -> png_base64 non-empty + decodes to a PNG signature.
//    * perception      -> same-render beauty/albedo/normal/depth atlas,
//                         metadata, max-edge bound, and zero-allocation opt-out.
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
#include "../src/Library/Interfaces/IRasterImageReader.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/Utilities/Reference.h"

#include <array>
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

typedef std::array<unsigned char, 4> DecodedPixel;
struct DecodedPng
{
	unsigned int width = 0, height = 0;
	std::vector<DecodedPixel> pixels;
	const DecodedPixel& At( unsigned int x, unsigned int y ) const
	{
		return pixels[static_cast<std::size_t>( y ) * width + x];
	}
};

// Decode through RISE's production PNG reader. Linear mode recovers the
// stored byte values, which lets this test verify the streamed writer's
// channel order and actual quadrant content rather than only its IHDR.
static bool DecodePng( const std::vector<unsigned char>& png, DecodedPng& out )
{
	if( png.empty() ) return false;
	Implementation::MemoryBuffer* buffer = new Implementation::MemoryBuffer(
		const_cast<char*>( reinterpret_cast<const char*>( png.data() ) ),
		static_cast<unsigned int>( png.size() ), false );
	IRasterImageReader* reader = 0;
	if( !RISE_API_CreatePNGReader( &reader, *buffer, eColorSpace_Rec709RGB_Linear ) || !reader ) {
		safe_release( buffer );
		return false;
	}
	if( !reader->BeginRead( out.width, out.height ) ) {
		safe_release( reader );
		safe_release( buffer );
		return false;
	}
	out.pixels.resize( static_cast<std::size_t>( out.width ) * out.height );
	auto byte = []( double v ) -> unsigned char {
		int i = static_cast<int>( v * 255.0 + 0.5 );
		if( i < 0 ) i = 0;
		if( i > 255 ) i = 255;
		return static_cast<unsigned char>( i );
	};
	for( unsigned int y = 0; y < out.height; ++y ) {
		for( unsigned int x = 0; x < out.width; ++x ) {
			RISEColor c;
			reader->ReadColor( c, x, y );
			out.pixels[static_cast<std::size_t>( y ) * out.width + x] = {
				byte( c.base.r ), byte( c.base.g ), byte( c.base.b ), byte( c.a ) };
		}
	}
	reader->EndRead();
	safe_release( reader );
	safe_release( buffer );
	return true;
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
		Check( r.get( "perceptionAvailable" ).asBool(),
		       "agent transport enables same-render perception by default" );
		Check( r.get( "perceptionPersistentBytes" ).asNumber() == 24.0 * 24.0 * 7.0,
		       "render reports exact compact sidecar cost (7 bytes/pixel)" );
		Check( r.get( "perceptionAuxiliaryPeakBytes" ).asNumber() == 24.0 * 24.0 * 87.0,
		       "render reports exact managed auxiliary peak (87 bytes/pixel)" );
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

	//----------------------------------------------------------------------
	// Preview-render wire tests (F5 the cheap multi-angle observe loop):
	// width/height clamping [16,512], camera param shape validation, and
	// read_image maxEdge clamping [16,1024].  All driven through the SAME
	// live dispatcher `rpc` used above, so the head carries whatever the
	// coherence block left it at (irrelevant here -- we only check dims /
	// param-shape behaviour, not pixel content).
	//----------------------------------------------------------------------
	std::printf( "[preview-render wire] width/height clamp to [16,512]\n" );
	{
		// Below the floor: 4x4 requested -> clamped to 16x16 (never rejected).
		JsonValue params = JsonValue::MakeObject();
		params.set( "width",  JsonValue::MakeNumber( 4.0 ) );
		params.set( "height", JsonValue::MakeNumber( 4.0 ) );
		const std::string resp = rpc.HandleLine( Req( 30, "render", params ) );
		JsonValue env = ParseResponse( resp, 30 );
		Check( env.has( "result" ), "render(width=4,height=4) returns a success result (clamped, not rejected)" );
		Check( env.get( "result" ).get( "ok" ).asBool(), "render(width=4,height=4) ok==true" );
		Check( env.get( "result" ).get( "width" ).asNumber() == 16.0,
		       "render(width=4) is CLAMPED UP to the floor (16), never rejected" );
		Check( env.get( "result" ).get( "previewWidth" ).asNumber() == 16.0,
		       "previewWidth echoes the clamped 16" );
		Check( env.get( "result" ).get( "previewHeight" ).asNumber() == 16.0,
		       "previewHeight echoes the clamped 16" );
	}
	{
		// Above the ceiling: 9999x9999 requested -> clamped to 512x512.
		JsonValue params = JsonValue::MakeObject();
		params.set( "width",  JsonValue::MakeNumber( 9999.0 ) );
		params.set( "height", JsonValue::MakeNumber( 9999.0 ) );
		const std::string resp = rpc.HandleLine( Req( 31, "render", params ) );
		JsonValue env = ParseResponse( resp, 31 );
		Check( env.has( "result" ), "render(width=9999) returns a success result (clamped, not rejected)" );
		Check( env.get( "result" ).get( "width" ).asNumber() == 512.0,
		       "render(width=9999) is CLAMPED DOWN to the ceiling (512)" );
		Check( env.get( "result" ).get( "height" ).asNumber() == 512.0,
		       "render(height=9999) is CLAMPED DOWN to the ceiling (512)" );
	}
	{
		// width WITHOUT height (ambiguous pairing) -> no override applied;
		// render still succeeds at the Document's authored dims.
		JsonValue params = JsonValue::MakeObject();
		params.set( "width", JsonValue::MakeNumber( 64.0 ) );
		const std::string resp = rpc.HandleLine( Req( 32, "render", params ) );
		JsonValue env = ParseResponse( resp, 32 );
		Check( env.has( "result" ), "render(width only, no height) still succeeds" );
		Check( env.get( "result" ).get( "width" ).asNumber() == 24.0,
		       "width without height is NOT applied (must be paired) -- stays at the authored 24" );
	}

	std::printf( "[preview-render wire] camera override param-shape validation\n" );
	{
		// A well-formed camera override succeeds and reports cameraOverridden.
		JsonValue cam = JsonValue::MakeObject();
		cam.set( "location", JsonValue::MakeString( "0 0 5" ) );
		cam.set( "lookat",   JsonValue::MakeString( "0 0 0" ) );
		JsonValue params = JsonValue::MakeObject();
		params.set( "camera", cam );
		const std::string resp = rpc.HandleLine( Req( 33, "render", params ) );
		JsonValue env = ParseResponse( resp, 33 );
		Check( env.has( "result" ), "render(camera={location,lookat}) succeeds" );
		Check( env.get( "result" ).get( "ok" ).asBool(), "render(camera override) ok==true" );
		Check( env.get( "result" ).get( "cameraOverridden" ).asBool(),
		       "render(camera override) reports cameraOverridden==true" );
	}
	{
		// camera present but missing 'lookat' -> -32602 (both required together).
		JsonValue cam = JsonValue::MakeObject();
		cam.set( "location", JsonValue::MakeString( "0 0 5" ) );
		JsonValue params = JsonValue::MakeObject();
		params.set( "camera", cam );
		const std::string resp = rpc.HandleLine( Req( 34, "render", params ) );
		JsonValue env = ParseResponse( resp, 34 );
		Check( env.has( "error" ), "render(camera missing lookat) is an error" );
		Check( env.get( "error" ).get( "code" ).asNumber() == -32602.0,
		       "render(camera missing lookat) -> -32602 Invalid params" );
	}
	{
		// camera is not an object -> -32602.
		JsonValue params = JsonValue::MakeObject();
		params.set( "camera", JsonValue::MakeString( "not an object" ) );
		const std::string resp = rpc.HandleLine( Req( 35, "render", params ) );
		JsonValue env = ParseResponse( resp, 35 );
		Check( env.has( "error" ), "render(camera as a string) is an error" );
		Check( env.get( "error" ).get( "code" ).asNumber() == -32602.0,
		       "render(camera not an object) -> -32602 Invalid params" );
	}

	//----------------------------------------------------------------------
	// P1-B (RPC-layer shape validation): a camera vector field must parse as
	// EXACTLY 3 finite numbers.  Before this fix, ParseCameraOverrideParam
	// only checked that location/lookat were STRINGS -- any string sailed
	// through unvalidated to AgentSession, where CameraIntrospection::
	// SetProperty's sscanf-based ParseVec3 silently no-ops on a malformed
	// shape like "5 5" (returns false, discarded by the pre-fix caller) or
	// tolerates trailing garbage like "1 2 3 4" (sscanf reads the first 3
	// and ignores the rest).  Both classes are now rejected HERE, at the
	// wire boundary, with a clean -32602 naming the field -- the session
	// underneath is never even asked to apply a shape this bad.
	//----------------------------------------------------------------------
	std::printf( "[preview-render wire] camera vector SHAPE validation (P1-B)\n" );
	{
		// location with only 2 tokens -> -32602 (the exact reported false-positive).
		JsonValue cam = JsonValue::MakeObject();
		cam.set( "location", JsonValue::MakeString( "5 5" ) );
		cam.set( "lookat",   JsonValue::MakeString( "0 0 0" ) );
		JsonValue params = JsonValue::MakeObject();
		params.set( "camera", cam );
		const std::string resp = rpc.HandleLine( Req( 40, "render", params ) );
		JsonValue env = ParseResponse( resp, 40 );
		Check( env.has( "error" ), "render(camera.location=\"5 5\") is an error" );
		Check( env.get( "error" ).get( "code" ).asNumber() == -32602.0,
		       "render(camera.location=\"5 5\", 2 tokens) -> -32602 Invalid params" );
	}
	{
		// location with non-numeric components -> -32602.
		JsonValue cam = JsonValue::MakeObject();
		cam.set( "location", JsonValue::MakeString( "abc def ghi" ) );
		cam.set( "lookat",   JsonValue::MakeString( "0 0 0" ) );
		JsonValue params = JsonValue::MakeObject();
		params.set( "camera", cam );
		const std::string resp = rpc.HandleLine( Req( 41, "render", params ) );
		JsonValue env = ParseResponse( resp, 41 );
		Check( env.has( "error" ), "render(camera.location=\"abc def ghi\") is an error" );
		Check( env.get( "error" ).get( "code" ).asNumber() == -32602.0,
		       "render(camera.location non-numeric) -> -32602 Invalid params" );
	}
	{
		// location with 4 tokens (trailing garbage that sscanf's "%lf %lf
		// %lf" would silently tolerate) -> -32602.  This is the case the
		// AgentSession belt-and-braces layer CANNOT catch on its own
		// (SetProperty's ParseVec3 happily parses the first 3 and ignores
		// the 4th) -- the RPC-layer exact-3-token check is the only gate
		// for this specific shape.
		JsonValue cam = JsonValue::MakeObject();
		cam.set( "location", JsonValue::MakeString( "1 2 3 4" ) );
		cam.set( "lookat",   JsonValue::MakeString( "0 0 0" ) );
		JsonValue params = JsonValue::MakeObject();
		params.set( "camera", cam );
		const std::string resp = rpc.HandleLine( Req( 42, "render", params ) );
		JsonValue env = ParseResponse( resp, 42 );
		Check( env.has( "error" ), "render(camera.location=\"1 2 3 4\") is an error" );
		Check( env.get( "error" ).get( "code" ).asNumber() == -32602.0,
		       "render(camera.location, 4 tokens) -> -32602 Invalid params" );
	}
	{
		// location empty string -> -32602.
		JsonValue cam = JsonValue::MakeObject();
		cam.set( "location", JsonValue::MakeString( "" ) );
		cam.set( "lookat",   JsonValue::MakeString( "0 0 0" ) );
		JsonValue params = JsonValue::MakeObject();
		params.set( "camera", cam );
		const std::string resp = rpc.HandleLine( Req( 43, "render", params ) );
		JsonValue env = ParseResponse( resp, 43 );
		Check( env.has( "error" ), "render(camera.location=\"\") is an error" );
		Check( env.get( "error" ).get( "code" ).asNumber() == -32602.0,
		       "render(camera.location empty) -> -32602 Invalid params" );
	}
	{
		// up malformed while location/lookat are valid -> -32602 (must not
		// silently keep the original up while accepting location/lookat).
		JsonValue cam = JsonValue::MakeObject();
		cam.set( "location", JsonValue::MakeString( "3.5 0 0" ) );
		cam.set( "lookat",   JsonValue::MakeString( "0 0 0" ) );
		cam.set( "up",       JsonValue::MakeString( "9 9" ) );
		JsonValue params = JsonValue::MakeObject();
		params.set( "camera", cam );
		const std::string resp = rpc.HandleLine( Req( 44, "render", params ) );
		JsonValue env = ParseResponse( resp, 44 );
		Check( env.has( "error" ), "render(camera.up=\"9 9\", 2 tokens) is an error" );
		Check( env.get( "error" ).get( "code" ).asNumber() == -32602.0,
		       "render(camera.up malformed while location/lookat valid) -> -32602 Invalid params" );
	}
	{
		// fov out-of-range: 0, 180 (both boundary-exclusive), and negative,
		// all rejected.
		auto CheckBadFov = [&]( double id, double fovValue, const char* label ) {
			JsonValue cam = JsonValue::MakeObject();
			cam.set( "location", JsonValue::MakeString( "0 0 5" ) );
			cam.set( "lookat",   JsonValue::MakeString( "0 0 0" ) );
			cam.set( "fov",      JsonValue::MakeNumber( fovValue ) );
			JsonValue params = JsonValue::MakeObject();
			params.set( "camera", cam );
			const std::string resp = rpc.HandleLine( Req( id, "render", params ) );
			JsonValue env = ParseResponse( resp, id );
			Check( env.has( "error" ), ( std::string( "render(camera.fov=" ) + label + ") is an error" ) );
			Check( env.get( "error" ).get( "code" ).asNumber() == -32602.0,
			       ( std::string( "render(camera.fov=" ) + label + ") -> -32602 Invalid params" ) );
		};
		CheckBadFov( 45, 0.0,   "0 (lower boundary, exclusive)" );
		CheckBadFov( 46, 180.0, "180 (upper boundary, exclusive)" );
		CheckBadFov( 47, -5.0,  "-5 (negative)" );
	}
	{
		// fov 1e999 (non-finite via TEXTUAL double parse -- matches the
		// established 'samples' guard idiom above: the hostile literal is
		// written into the raw JSON-RPC line as TEXT and parsed by
		// JsonParse, so the +inf value is produced by the JSON parser's
		// strtod, not by a C++ double literal -- a C++-side `1e999` literal
		// trips '-Wliteral-range' at compile time (magnitude too large for
		// double), which is exactly the kind of warning CLAUDE.md forbids
		// introducing).
		const std::string resp = rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":48,\"method\":\"render\",\"params\":"
			"{\"camera\":{\"location\":\"0 0 5\",\"lookat\":\"0 0 0\",\"fov\":1e999}}}" );
		JsonValue env = ParseResponse( resp, 48 );
		Check( env.has( "error" ), "render(camera.fov=1e999) is an error" );
		Check( env.get( "error" ).get( "code" ).asNumber() == -32602.0,
		       "render(camera.fov=1e999, parses to +inf) -> -32602 Invalid params" );
	}
	{
		// fov 179.9 (just inside the open interval) -> accepted.
		JsonValue cam = JsonValue::MakeObject();
		cam.set( "location", JsonValue::MakeString( "0 0 5" ) );
		cam.set( "lookat",   JsonValue::MakeString( "0 0 0" ) );
		cam.set( "fov",      JsonValue::MakeNumber( 179.9 ) );
		JsonValue params = JsonValue::MakeObject();
		params.set( "camera", cam );
		const std::string resp = rpc.HandleLine( Req( 49, "render", params ) );
		JsonValue env = ParseResponse( resp, 49 );
		Check( env.has( "result" ), "render(camera.fov=179.9, just inside (0,180)) succeeds" );
		Check( env.get( "result" ).get( "ok" ).asBool(), "render(camera.fov=179.9) ok==true" );
	}
	{
		// The session is still usable after all those rejections -- a
		// well-formed render succeeds cleanly right after.
		const std::string resp = rpc.HandleLine( Req( 50, "render", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 50 );
		Check( env.has( "result" ), "render() with no camera override still succeeds after the malformed-shape rejections" );
		Check( env.get( "result" ).get( "ok" ).asBool(), "render() ok==true (session not left in a broken state)" );
	}

	std::printf( "[preview-render wire] read_image maxEdge clamps to [16,1024]\n" );
	{
		// Ensure a render has happened so read_image has a cached image.
		const std::string rresp = rpc.HandleLine( Req( 36, "render", JsonValue::MakeObject() ) );
		JsonValue renv = ParseResponse( rresp, 36 );
		Check( renv.get( "result" ).get( "ok" ).asBool(), "render before the read_image maxEdge checks succeeds" );

		JsonValue params = JsonValue::MakeObject();
		params.set( "maxEdge", JsonValue::MakeNumber( 8.0 ) );   // below the floor
		const std::string resp = rpc.HandleLine( Req( 37, "read_image", params ) );
		JsonValue env = ParseResponse( resp, 37 );
		Check( env.has( "result" ), "read_image(maxEdge=8) succeeds (clamped, not rejected)" );
		Check( env.get( "result" ).get( "width" ).asNumber() == 16.0,
		       "read_image(maxEdge=8) is CLAMPED UP to the floor (16)" );
		Check( env.get( "result" ).get( "height" ).asNumber() == 16.0,
		       "read_image(maxEdge=8) height also clamped to 16 (square 24x24 source)" );
		const std::string b64 = env.get( "result" ).get( "png_base64" ).asString();
		Check( !b64.empty(), "read_image(maxEdge=8) returns non-empty png_base64" );
		std::vector<unsigned char> decoded;
		Check( Base64Decode( b64, decoded ), "read_image(maxEdge=8) png_base64 decodes cleanly" );
		Check( decoded.size() >= 8 && decoded[0] == 0x89 && decoded[1] == 'P' && decoded[2] == 'N' && decoded[3] == 'G',
		       "read_image(maxEdge=8) decodes to a valid \\x89PNG signature" );
	}
	{
		JsonValue params = JsonValue::MakeObject();
		params.set( "maxEdge", JsonValue::MakeNumber( 99999.0 ) );   // above the ceiling
		const std::string resp = rpc.HandleLine( Req( 38, "read_image", params ) );
		JsonValue env = ParseResponse( resp, 38 );
		Check( env.has( "result" ), "read_image(maxEdge=99999) succeeds (clamped, not rejected)" );
		// The native image is 24x24, well under the 1024 ceiling, so the
		// clamp-then-never-upscale rule leaves it at the native 24x24.
		Check( env.get( "result" ).get( "width" ).asNumber() == 24.0,
		       "read_image(maxEdge=99999) never upscales past the native 24x24" );
	}
	{
		// No maxEdge at all -> legacy shape, dims still reported (additive).
		const std::string resp = rpc.HandleLine( Req( 39, "read_image", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 39 );
		Check( env.get( "result" ).get( "width" ).asNumber() == 24.0,
		       "read_image() with no maxEdge reports the native 24x24 (back-compat + additive width/height)" );
	}

	//----------------------------------------------------------------------
	// Perception representation: a single conventional 2x2 image assembled
	// from the SAME render.  `maxEdge` bounds the complete atlas, not each
	// panel, so model image-token and transfer cost stay predictable.
	//----------------------------------------------------------------------
	std::printf( "[perception] same-render atlas + structured depth/memory metadata\n" );
	{
		JsonValue params = JsonValue::MakeObject();
		params.set( "representation", JsonValue::MakeString( "perception" ) );
		params.set( "maxEdge", JsonValue::MakeNumber( 32.0 ) );
		const std::string resp = rpc.HandleLine( Req( 70, "read_image", params ) );
		JsonValue env = ParseResponse( resp, 70 );
		const JsonValue& r = env.get( "result" );
		Check( r.get( "available" ).asBool(), "perception atlas is available after production beauty" );
		Check( r.get( "representation" ).asString() == "perception", "read_image echoes perception representation" );
		Check( r.get( "width" ).asNumber() == 32.0 && r.get( "height" ).asNumber() == 32.0,
		       "maxEdge bounds the whole 2x2 atlas" );
		Check( r.get( "sourceWidth" ).asNumber() == 24.0 && r.get( "sourceHeight" ).asNumber() == 24.0,
		       "perception metadata preserves source dimensions" );
		const JsonValue& panels = r.get( "panels" );
		Check( panels.isArray() && panels.size() == 4,
		       "perception atlas declares four typed panels" );
		Check( panels.at( 0 ).asString() == "beauty" &&
		       panels.at( 1 ).asString() == "albedo" &&
		       panels.at( 2 ).asString() == "world_normal" &&
		       panels.at( 3 ).asString() == "log_depth",
		       "perception panel order is stable" );
		Check( r.get( "validDepthPixels" ).asNumber() > 0.0,
		       "same-render depth contains visible surface hits" );
		Check( r.get( "depthMin" ).asNumber() > 0.0 &&
		       r.get( "depthMax" ).asNumber() >= r.get( "depthMin" ).asNumber(),
		       "depth range is finite, positive, and ordered" );
		Check( r.get( "persistentBytes" ).asNumber() == 24.0 * 24.0 * 7.0 &&
		       r.get( "auxiliaryPeakBytes" ).asNumber() == 24.0 * 24.0 * 87.0,
		       "read_image exposes exact managed perception memory" );
		Check( r.get( "encoderRowBytes" ).asNumber() == 32.0 * 4.0,
		       "perception encoder uses one RGBA scanline rather than a full atlas staging image" );
		std::vector<unsigned char> png;
		Check( Base64Decode( r.get( "png_base64" ).asString(), png ),
		       "perception png_base64 decodes cleanly" );
		Check( png.size() >= 8 && png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G',
		       "perception payload is a PNG" );
		if( png.size() >= 24 ) {
			const unsigned int ihdrW = ( unsigned( png[16] ) << 24 ) | ( unsigned( png[17] ) << 16 ) |
				( unsigned( png[18] ) << 8 ) | unsigned( png[19] );
			const unsigned int ihdrH = ( unsigned( png[20] ) << 24 ) | ( unsigned( png[21] ) << 16 ) |
				( unsigned( png[22] ) << 8 ) | unsigned( png[23] );
			Check( ihdrW == 32 && ihdrH == 32,
			       "perception PNG IHDR matches the declared whole-atlas dimensions" );
		}
		DecodedPng decoded;
		Check( DecodePng( png, decoded ) && decoded.width == 32 && decoded.height == 32,
		       "streamed perception PNG fully decodes at the declared dimensions" );
		if( decoded.width == 32 && decoded.height == 32 ) {
			const DecodedPixel& albedo = decoded.At( 24, 8 );
			const DecodedPixel& normal = decoded.At( 8, 24 );
			const DecodedPixel& depth = decoded.At( 24, 24 );
			const bool redAlbedo = albedo[0] > albedo[1] + 80 && albedo[0] > albedo[2] + 80;
			if( !redAlbedo ) {
				std::printf( "  decoded atlas probes: albedo=(%u,%u,%u,%u) normal=(%u,%u,%u,%u) depth=(%u,%u,%u,%u)\n",
					unsigned( albedo[0] ), unsigned( albedo[1] ), unsigned( albedo[2] ), unsigned( albedo[3] ),
					unsigned( normal[0] ), unsigned( normal[1] ), unsigned( normal[2] ), unsigned( normal[3] ),
					unsigned( depth[0] ), unsigned( depth[1] ), unsigned( depth[2] ), unsigned( depth[3] ) );
			}
			Check( redAlbedo,
			       "top-right panel decodes as the sphere's edited red diffuse albedo" );
			Check( normal[2] > normal[0] + 30 && normal[2] > normal[1] + 30,
			       "bottom-left panel preserves RGB channel order for the camera-facing normal" );
			Check( depth[0] > 0 && depth[0] == depth[1] && depth[1] == depth[2],
			       "bottom-right panel decodes as nonzero grayscale log depth" );
			Check( albedo[3] == 255 && normal[3] == 255 && depth[3] == 255,
			       "all streamed perception panels are opaque RGBA" );
		}
	}
	{
		JsonValue params = JsonValue::MakeObject();
		params.set( "representation", JsonValue::MakeString( "perception" ) );
		params.set( "maxEdge", JsonValue::MakeNumber( 17.0 ) );
		JsonValue env = ParseResponse( rpc.HandleLine( Req( 74, "read_image", params ) ), 74 );
		const JsonValue& r = env.get( "result" );
		Check( r.get( "width" ).asNumber() == 16.0 && r.get( "height" ).asNumber() == 16.0,
		       "odd maxEdge remains a strict whole-atlas bound" );
		Check( r.get( "encoderRowBytes" ).asNumber() == 16.0 * 4.0,
		       "downscaled atlas still stages exactly one RGBA row" );
	}
	{
		JsonValue params = JsonValue::MakeObject();
		params.set( "representation", JsonValue::MakeString( "tensor" ) );
		JsonValue env = ParseResponse( rpc.HandleLine( Req( 71, "read_image", params ) ), 71 );
		Check( env.get( "error" ).get( "code" ).asNumber() == -32602.0,
		       "unknown read_image representation is rejected" );
	}
	{
		JsonValue params = JsonValue::MakeObject();
		params.set( "perception", JsonValue::MakeBool( false ) );
		JsonValue env = ParseResponse( rpc.HandleLine( Req( 72, "render", params ) ), 72 );
		const JsonValue& r = env.get( "result" );
		Check( r.get( "ok" ).asBool(), "perception:false render succeeds" );
		Check( !r.get( "perceptionAvailable" ).asBool() &&
		       r.get( "perceptionPersistentBytes" ).asNumber() == 0.0 &&
		       r.get( "perceptionAuxiliaryPeakBytes" ).asNumber() == 0.0,
		       "perception:false removes all auxiliary perception allocation" );

		JsonValue read = JsonValue::MakeObject();
		read.set( "representation", JsonValue::MakeString( "perception" ) );
		JsonValue readEnv = ParseResponse( rpc.HandleLine( Req( 73, "read_image", read ) ), 73 );
		const JsonValue& readResult = readEnv.get( "result" );
		Check( !readResult.get( "available" ).asBool() &&
		       readResult.get( "png_base64" ).asString().empty() &&
		       readResult.get( "width" ).asNumber() == 0.0 &&
		       readResult.get( "height" ).asNumber() == 0.0,
		       "perception:false leaves no stale atlas from the prior render" );
	}

	//----------------------------------------------------------------------
	// P1 #1 — NO-HEAD dispatcher: the stateless bootstrap methods work with
	// a NULL session (mirroring `rise --agent-stdio` with no scene / a failed
	// load).  read_schema + validate + read_document need NO head; an agent
	// CONSTRUCTS/REPAIRS a scene from scratch through them.  propose_patch +
	// render STILL require a head.  RED-PROVE: against the old !s-guarded
	// dispatcher these read_schema/validate calls returned a -32603
	// "no session loaded" error, so a SUCCESS result here is the fix.
	//----------------------------------------------------------------------
	std::printf( "[no-head bootstrap] stateless read_schema/validate/read_document with a NULL session\n" );
	{
		// Construct the dispatcher with an EMPTY session -- the exact no-head
		// CLI path (RunAgentStdio with no scene arg).
		std::unique_ptr<AgentSession> nullSession;   // empty -> no head
		AgentRpcDispatcher nohead( std::move( nullSession ) );

		// read_schema {keyword:"sphere_geometry"} -> SUCCESS, mentions "radius".
		{
			JsonValue params = JsonValue::MakeObject();
			params.set( "keyword", JsonValue::MakeString( "sphere_geometry" ) );
			const std::string resp = nohead.HandleLine( Req( 20, "read_schema", params ) );
			JsonValue env = ParseResponse( resp, 20 );
			Check( !env.has( "error" ), "no-head read_schema returns a SUCCESS (NOT a -32603 no-session error)" );
			Check( env.has( "result" ), "no-head read_schema has a result field" );
			const std::string schemaStr = JsonSerialize( env.get( "result" ).get( "schema" ) );
			Check( schemaStr.find( "radius" ) != std::string::npos,
			       "no-head read_schema(sphere_geometry) mentions 'radius'" );
		}

		// validate {good inline scene} -> SUCCESS, zero Error diagnostics.
		{
			const std::string goodScene =
				"RISE ASCII SCENE 7\n"
				"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n";
			JsonValue params = JsonValue::MakeObject();
			params.set( "text", JsonValue::MakeString( goodScene ) );
			const std::string resp = nohead.HandleLine( Req( 21, "validate", params ) );
			JsonValue env = ParseResponse( resp, 21 );
			Check( !env.has( "error" ), "no-head validate(good) returns a SUCCESS (NOT a -32603 no-session error)" );
			const JsonValue& diags = env.get( "result" ).get( "diagnostics" );
			Check( diags.isArray(), "no-head validate(good) returns a diagnostics array" );
			int errorCount = 0;
			for( std::size_t i = 0; i < diags.size(); ++i )
				if( diags.at( i ).get( "severity" ).asString() == "error" ) ++errorCount;
			Check( errorCount == 0, "no-head validate(good scene) reports ZERO error diagnostics" );
		}

		// validate {bogus param} -> SUCCESS with an UNKNOWN_PARAMETER diagnostic.
		{
			const std::string badScene =
				"RISE ASCII SCENE 7\n"
				"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n\tnot_a_real_param 3\n}\n";
			JsonValue params = JsonValue::MakeObject();
			params.set( "text", JsonValue::MakeString( badScene ) );
			const std::string resp = nohead.HandleLine( Req( 22, "validate", params ) );
			JsonValue env = ParseResponse( resp, 22 );
			Check( !env.has( "error" ), "no-head validate(bad) returns a SUCCESS (NOT a -32603 no-session error)" );
			const JsonValue& diags = env.get( "result" ).get( "diagnostics" );
			bool foundUnknownParam = false;
			for( std::size_t i = 0; i < diags.size(); ++i )
				if( diags.at( i ).get( "code" ).asString() == "UNKNOWN_PARAMETER" ) foundUnknownParam = true;
			Check( foundUnknownParam, "no-head validate(bad scene) reports an UNKNOWN_PARAMETER diagnostic" );
		}

		// read_document -> SUCCESS with {document:"", hasDocument:false}.
		{
			const std::string resp = nohead.HandleLine( Req( 23, "read_document", JsonValue::MakeObject() ) );
			JsonValue env = ParseResponse( resp, 23 );
			Check( !env.has( "error" ), "no-head read_document returns a SUCCESS (graceful bootstrap, not an error)" );
			const JsonValue& r = env.get( "result" );
			Check( r.get( "document" ).asString().empty(), "no-head read_document -> document==\"\"" );
			Check( !r.get( "hasDocument" ).asBool(), "no-head read_document -> hasDocument==false" );
		}

		// propose_patch + render STILL require a head -> -32603.
		{
			JsonValue params = JsonValue::MakeObject();
			params.set( "target", JsonValue::MakeString( "sph" ) );
			params.set( "param",  JsonValue::MakeString( "radius" ) );
			params.set( "value",  JsonValue::MakeString( "1.0" ) );
			const std::string resp = nohead.HandleLine( Req( 24, "propose_patch", params ) );
			JsonValue env = ParseResponse( resp, 24 );
			Check( env.get( "error" ).get( "code" ).asNumber() == -32603.0,
			       "no-head propose_patch STILL errors (-32603 -- head required)" );
		}
		{
			const std::string resp = nohead.HandleLine( Req( 25, "render", JsonValue::MakeObject() ) );
			JsonValue env = ParseResponse( resp, 25 );
			Check( env.get( "error" ).get( "code" ).asNumber() == -32603.0,
			       "no-head render STILL errors (-32603 -- head required)" );
		}
	}

	std::printf( "=== AgentFirstSliceTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
