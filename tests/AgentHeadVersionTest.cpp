//////////////////////////////////////////////////////////////////////
//
//  AgentHeadVersionTest.cpp - Facet 5 (live co-edit) slice 1a: the
//    optimistic-concurrency (uuid,revision) CST head-version + the
//    baseHeadVersion CONFLICT precondition.
//
//  All headless, single-threaded, self-contained (an inline native-v7
//  scene reused from the slice-0c capstone; OIDN off; no RISE_MEDIA_PATH).
//  Two levels are exercised:
//    * AgentSession direct (HeadVersion / ProposePatch with a base) -- the
//      concurrency HEART: bump/no-bump, conflict + recover, uuid fresh
//      across loads (the reload-collision guard), back-compat.
//    * AgentRpcDispatcher::HandleLine (the JSON-RPC surface) -- read_document
//      carries headVersion; a stale baseHeadVersion -> status="conflict"; a
//      matching base applies; a malformed base -> -32602.
//
//  Design context: docs/agentic-redesign/50-agentic-surface.md §2.2.2 / §2.4
//  (baseHeadVersion precondition, CONFLICT) + docs/gui/TRANSACTION_MODEL.md
//  §3.4 ((UUID,revision) document identity).
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentRpc.h"
#include "../src/Library/Agent/Json.h"
#include "../src/Library/Cst/Cst.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
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

// The same inline native-v7 scene the slice-0c capstone uses.
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

// A structured set-patch for pnt_albedo.color (a real, always-present param).
static AgentSetPatch RecolourPatch( const std::string& value )
{
	AgentSetPatch p;
	p.target = "pnt_albedo";
	p.param  = "color";
	p.value  = value;
	return p;
}

// A JSON-RPC request line for `method` with `params` under `id`.
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
	std::printf( "=== AgentHeadVersionTest (Facet 5 slice 1a: (uuid,revision) head-version + conflict) ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_slice1a.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the scene to a temp file" );

	//----------------------------------------------------------------------
	// bump / no-bump (AgentSession direct).
	//----------------------------------------------------------------------
	std::printf( "[bump/no-bump] load -> {U,1}; successful patch bumps; rejected patch does NOT\n" );
	std::unique_ptr<AgentSession> session = AgentSession::LoadFromFile( scenePath );
	Check( session != nullptr, "AgentSession::LoadFromFile loads the native-v7 scene" );
	if( !session ) { std::printf( "cannot continue without a session\n" ); return 1; }

	const RISE::Cst::CstHeadVersion v0 = session->HeadVersion();
	const std::uint64_t U = v0.uuid;
	Check( U != 0, "a freshly-loaded head has a non-zero uuid" );
	Check( v0.revision == 1, "a freshly-loaded head is at revision 1" );

	{
		// A successful edit (no base) bumps the revision, keeps the uuid.
		const AgentPatchResult r = session->ProposePatch( RecolourPatch( "0.9 0.1 0.1" ) );
		Check( r.applied, "propose_patch(pnt_albedo.color, no base) applied" );
		Check( session->HeadVersion().uuid == U, "uuid unchanged by a successful edit" );
		Check( session->HeadVersion().revision == 2, "revision bumped to 2 after one successful edit" );
		Check( r.headVersion.uuid == U && r.headVersion.revision == 2,
		       "the result carries the POST-COMMIT head-version {U,2}" );
	}
	{
		// A rejected edit (bogus target -> code 0) leaves the head UNCHANGED.
		AgentSetPatch bogus;
		bogus.target = "no_such_entity_xyz";
		bogus.param  = "color";
		bogus.value  = "0.1 0.2 0.3";
		const AgentPatchResult r = session->ProposePatch( bogus );
		Check( !r.applied && r.status == "rejected", "propose_patch(bogus target) rejected" );
		Check( session->HeadVersion().uuid == U && session->HeadVersion().revision == 2,
		       "a REJECTED edit leaves the head at {U,2} (no bump on the reject path)" );
		Check( r.headVersion.uuid == U && r.headVersion.revision == 2,
		       "the reject result carries the unchanged head {U,2}" );
	}

	//----------------------------------------------------------------------
	// conflict + recover (AgentSession direct).
	//----------------------------------------------------------------------
	std::printf( "[conflict+recover] stale base -> conflict (no mutation); fresh base -> applies\n" );
	{
		// Capture base vN (currently {U,2}).
		const RISE::Cst::CstHeadVersion baseN = session->HeadVersion();
		Check( baseN.revision == 2, "base captured at revision 2" );

		// A second successful commit moves the head to {U,3}.
		const AgentPatchResult r2 = session->ProposePatch( RecolourPatch( "0.2 0.8 0.2" ) );
		Check( r2.applied && session->HeadVersion().revision == 3, "second commit moved head to revision 3" );

		// The document text right now (for the stability check below).
		const std::string docBeforeConflict = session->ReadDocument();

		// Propose WITH the STALE base vN ({U,2}) -> CONFLICT, no mutation.
		AgentSetPatch stale = RecolourPatch( "0.1 0.1 0.9" );
		stale.hasBaseVersion = true;
		stale.baseVersion    = baseN;   // {U,2}, but head is now {U,3}
		const AgentPatchResult rc = session->ProposePatch( stale );
		Check( !rc.applied, "stale-base propose is NOT applied" );
		Check( rc.status == "conflict", "stale-base propose -> status==\"conflict\"" );
		Check( rc.headVersion.uuid == U && rc.headVersion.revision == 3,
		       "conflict result reports the CURRENT head {U,3} (where to re-sync)" );
		Check( session->HeadVersion().revision == 3,
		       "the head is UNCHANGED by the conflicted attempt (still revision 3)" );
		Check( session->ReadDocument() == docBeforeConflict,
		       "the Document is byte-IDENTICAL across the conflicted attempt (no mutation)" );

		// RED-PROVE: the SAME propose with the FRESH base ({U,3}) APPLIES where
		// the stale base was rejected.
		AgentSetPatch fresh = RecolourPatch( "0.1 0.1 0.9" );
		fresh.hasBaseVersion = true;
		fresh.baseVersion    = session->HeadVersion();   // {U,3}, matches
		const AgentPatchResult rf = session->ProposePatch( fresh );
		Check( rf.applied, "fresh-base propose (same edit) APPLIES (red-prove: base is the only difference)" );
		Check( session->HeadVersion().revision == 4, "the fresh-base apply bumped the revision to 4" );
		Check( rf.headVersion.revision == 4, "the applied result carries the post-commit revision 4" );
		Check( session->ReadDocument().find( "0.1 0.1 0.9" ) != std::string::npos,
		       "the fresh-base edit actually landed in the Document" );
	}

	//----------------------------------------------------------------------
	// uuid fresh across loads -- the reload-collision guard.
	//----------------------------------------------------------------------
	std::printf( "[uuid fresh across loads] a second session gets a DIFFERENT uuid; U1 can NEVER match S2\n" );
	{
		std::unique_ptr<AgentSession> s2 = AgentSession::LoadFromFile( scenePath );
		Check( s2 != nullptr, "a SECOND session loads the same scene into a fresh Job" );
		if( s2 ) {
			const RISE::Cst::CstHeadVersion v2 = s2->HeadVersion();
			Check( v2.uuid != U, "S2's head uuid (U2) differs from S1's (U1) -- fresh mint per load" );
			Check( v2.revision == 1, "S2 starts at revision 1" );

			// A base carrying S1's uuid U1 with S2's revision (1) can NEVER match
			// S2's head, even though the revision numbers coincide -> conflict.
			// This is the reload-collision guard: the uuid disambiguates loads.
			AgentSetPatch crossLoad = RecolourPatch( "0.3 0.3 0.3" );
			crossLoad.hasBaseVersion = true;
			crossLoad.baseVersion.uuid     = U;             // S1's uuid
			crossLoad.baseVersion.revision = v2.revision;   // == S2's revision (1) -- revision alone would MATCH
			const AgentPatchResult rx = s2->ProposePatch( crossLoad );
			Check( !rx.applied && rx.status == "conflict",
			       "a base carrying U1 can NEVER match S2's head even with a coinciding revision (-> conflict)" );
			Check( s2->HeadVersion().revision == 1, "S2's head untouched by the cross-load conflict" );
		}
	}

	//----------------------------------------------------------------------
	// back-compat: propose_patch with NO base still applies unconditionally.
	//----------------------------------------------------------------------
	std::printf( "[back-compat] no-base propose still applies (existing loop unaffected)\n" );
	{
		std::unique_ptr<AgentSession> s3 = AgentSession::LoadFromFile( scenePath );
		Check( s3 != nullptr, "back-compat session loads" );
		if( s3 ) {
			const std::uint64_t rev0 = s3->HeadVersion().revision;
			AgentSetPatch p = RecolourPatch( "0.7 0.2 0.2" );   // hasBaseVersion defaults false
			const AgentPatchResult r = s3->ProposePatch( p );
			Check( r.applied, "no-base propose_patch applies (unconditional, back-compat)" );
			Check( s3->HeadVersion().revision == rev0 + 1, "no-base apply still bumps the revision" );
		}
	}

	//----------------------------------------------------------------------
	// JSON-RPC level (drive AgentRpcDispatcher::HandleLine).
	//----------------------------------------------------------------------
	std::printf( "[json-rpc] read_document carries headVersion; stale base -> conflict; match -> applied; malformed -> -32602\n" );
	{
		std::unique_ptr<AgentSession> rs = AgentSession::LoadFromFile( scenePath );
		Check( rs != nullptr, "json-rpc session loads" );
		if( rs ) {
			AgentRpcDispatcher rpc( std::move( rs ) );

			// read_document -> headVersion {uuid,revision}.
			std::uint64_t rpcUuid = 0, rpcRev = 0;
			{
				const std::string resp = rpc.HandleLine( Req( 1, "read_document", JsonValue::MakeObject() ) );
				JsonValue env; std::string err;
				Check( JsonParse( resp, env, err ), "read_document response parses" );
				const JsonValue& hv = env.get( "result" ).get( "headVersion" );
				Check( hv.isObject(), "read_document result carries a headVersion OBJECT" );
				rpcUuid = static_cast<std::uint64_t>( hv.get( "uuid" ).asNumber() );
				rpcRev  = static_cast<std::uint64_t>( hv.get( "revision" ).asNumber() );
				Check( rpcUuid != 0, "read_document headVersion.uuid is non-zero" );
				Check( rpcRev == 1, "read_document headVersion.revision is 1 (fresh load)" );
			}

			// propose_patch with a MATCHING base -> applied, revision bumps.
			{
				JsonValue base = JsonValue::MakeObject();
				base.set( "uuid",     JsonValue::MakeNumber( static_cast<double>( rpcUuid ) ) );
				base.set( "revision", JsonValue::MakeNumber( static_cast<double>( rpcRev ) ) );
				JsonValue params = JsonValue::MakeObject();
				params.set( "target", JsonValue::MakeString( "pnt_albedo" ) );
				params.set( "param",  JsonValue::MakeString( "color" ) );
				params.set( "value",  JsonValue::MakeString( "0.4 0.4 0.9" ) );
				params.set( "baseHeadVersion", base );
				const std::string resp = rpc.HandleLine( Req( 2, "propose_patch", params ) );
				JsonValue env; std::string err;
				Check( JsonParse( resp, env, err ), "matching-base propose_patch response parses" );
				const JsonValue& r = env.get( "result" );
				Check( r.get( "applied" ).asBool(), "matching-base propose_patch applied==true" );
				Check( r.get( "status" ).asString() == "applied", "matching-base status==\"applied\"" );
				const JsonValue& hv = r.get( "headVersion" );
				Check( static_cast<std::uint64_t>( hv.get( "revision" ).asNumber() ) == rpcRev + 1,
				       "matching-base propose_patch bumped headVersion.revision" );
			}

			// propose_patch with a STALE base ({uuid,1}, but head is now 2) -> conflict.
			{
				JsonValue base = JsonValue::MakeObject();
				base.set( "uuid",     JsonValue::MakeNumber( static_cast<double>( rpcUuid ) ) );
				base.set( "revision", JsonValue::MakeNumber( static_cast<double>( rpcRev ) ) );   // stale (1, head is 2)
				JsonValue params = JsonValue::MakeObject();
				params.set( "target", JsonValue::MakeString( "pnt_albedo" ) );
				params.set( "param",  JsonValue::MakeString( "color" ) );
				params.set( "value",  JsonValue::MakeString( "0.9 0.9 0.1" ) );
				params.set( "baseHeadVersion", base );
				const std::string resp = rpc.HandleLine( Req( 3, "propose_patch", params ) );
				JsonValue env; std::string err;
				Check( JsonParse( resp, env, err ), "stale-base propose_patch response parses" );
				const JsonValue& r = env.get( "result" );
				Check( !r.get( "applied" ).asBool(), "stale-base propose_patch applied==false" );
				Check( r.get( "status" ).asString() == "conflict", "stale-base status==\"conflict\"" );
				Check( r.get( "headVersion" ).isObject(), "conflict result carries a headVersion object" );
				Check( static_cast<std::uint64_t>( r.get( "headVersion" ).get( "revision" ).asNumber() ) == rpcRev + 1,
				       "conflict headVersion reports the current (moved-on) revision" );
			}

			// A MALFORMED baseHeadVersion (non-numeric uuid) -> -32602.
			{
				JsonValue base = JsonValue::MakeObject();
				base.set( "uuid",     JsonValue::MakeString( "not-a-number" ) );
				base.set( "revision", JsonValue::MakeNumber( 1.0 ) );
				JsonValue params = JsonValue::MakeObject();
				params.set( "target", JsonValue::MakeString( "pnt_albedo" ) );
				params.set( "param",  JsonValue::MakeString( "color" ) );
				params.set( "value",  JsonValue::MakeString( "0.5 0.5 0.5" ) );
				params.set( "baseHeadVersion", base );
				const std::string resp = rpc.HandleLine( Req( 4, "propose_patch", params ) );
				JsonValue env; std::string err;
				Check( JsonParse( resp, env, err ), "malformed-base propose_patch response parses" );
				Check( env.has( "error" ), "malformed baseHeadVersion -> an error (not a result)" );
				Check( env.get( "error" ).get( "code" ).asNumber() == -32602.0,
				       "malformed baseHeadVersion (non-numeric uuid) -> error.code == -32602" );
			}

			// Numeric-but-out-of-domain baseHeadVersion values must be
			// rejected with -32602 BEFORE the uint64 cast -- these are the
			// hostile inputs (inf / negative / fractional) that would trip
			// static_cast<uint64_t>(inf) UB or spuriously match after a
			// fractional truncation.  One case each.
			{
				// Feed RAW JSON-RPC wire strings, NOT JsonValue-built requests:
				// the serializer neutralizes non-finite doubles, so `1e999` has
				// to arrive as a literal wire token to actually parse to +inf and
				// exercise the cast guard -- which is exactly what a hostile
				// client sends.  (-1 and 2.5 would serialize fine, but raw
				// strings keep all three consistent.)
				struct BadCase { const char* label; const char* uuid; const char* revision; };
				const BadCase bad[] = {
					{ "uuid 1e999 (parses to +inf)", "1e999", "1"   },
					{ "negative uuid",               "-1",    "1"   },
					{ "fractional revision 2.5",     "1",     "2.5" },
				};
				int rid = 5;
				for( const BadCase& bc : bad ) {
					const std::string line =
						std::string( "{\"jsonrpc\":\"2.0\",\"id\":" ) + std::to_string( rid++ ) +
						",\"method\":\"propose_patch\",\"params\":{"
						"\"target\":\"pnt_albedo\",\"param\":\"color\",\"value\":\"0.5 0.5 0.5\","
						"\"baseHeadVersion\":{\"uuid\":" + bc.uuid + ",\"revision\":" + bc.revision + "}}}";
					const std::string resp = rpc.HandleLine( line );
					JsonValue env; std::string err;
					Check( JsonParse( resp, env, err ),
					       std::string( "out-of-domain base (" ) + bc.label + ") response parses" );
					Check( env.has( "error" ),
					       std::string( "out-of-domain base (" ) + bc.label + ") -> an error, not a result" );
					Check( env.get( "error" ).get( "code" ).asNumber() == -32602.0,
					       std::string( "out-of-domain base (" ) + bc.label + ") -> error.code == -32602" );
				}
			}
		}
	}

	std::printf( "=== AgentHeadVersionTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
