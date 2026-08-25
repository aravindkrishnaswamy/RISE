//////////////////////////////////////////////////////////////////////
//
//  AgentFixBlendScaleTest.cpp - cat plan item 1 (2026-08-25):
//    fix_blend_scale, the CALLABLE VERB half of design-note condition J
//    (the blend-scale law) -- the vary_material of geometry.
//
//  MOTIVATION (measured, cat plan brief): condition J's own ADVICE was
//  read and then ignored four straight times by gemini-3.7-flash on the
//  melted-ear cat, while vary_material (a callable VERB) was acted on in
//  every run across providers.  This verb mechanizes condition J's own
//  suggested fix: clamp every flagged smin joint's k down to its safe
//  bound, in one call.
//
//  Cases:
//    A  BASIC APPLY: the cat-ear fixture (isotropic scale) -- the verb
//       clamps k, the document text actually changes, and a RE-SCAN
//       (AgentSession::ValidateText) goes SILENT afterward.
//    B  TARGET SCOPE: two offending sdf_geometry chunks; `target` fixes
//       only the named one, the other's offender survives untouched.
//    C  NO-OFFENDER REFUSAL: a compliant document -- clean, honest
//       refusal, document byte-identical.
//    D  WIRE SURFACE: chat-codec table, RPC dispatch, MCP (advertised
//       AND routable) -- the P2-4 lesson (in-process-only coverage let a
//       wire bug ship once already).
//    E  PROPORTION CAVEAT HONESTY (engine A/B mid-flight finding): the
//       verb's own per-joint summary carries the SAME caveat condition
//       J's clause does when a flagged part is <=1/5 the size of what it
//       joins (the cat ear); a merely over-blended, proportionate join
//       carries no caveat.
//    F  THE RECONCILIATION INVARIANT (item 3-b): on an ANISOTROPICALLY
//       scaled part, the REPORTED suggested max (what the message shows)
//       can exceed the DETECTION max (what decides fire/silent) -- the
//       verb's actual clamp target is the min of the two, so the re-scan
//       is GUARANTEED silent afterward even when the message's own
//       number would not have been strict enough on its own.
//    G  finish_element's INLINE FINDING (item 3-a): the shown object's
//       worst offending joint is named in the finish message, with the
//       verb, and the SAME proportion caveat when it applies; silent
//       when the shown object has no offender; and the "no score, ever"
//       sweep (Phase 2b's law) still holds over the new sentence.
//
//  Self-contained: no RISE_MEDIA_PATH, inline native-v7 scenes.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "../src/Library/Job.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentRpc.h"
#include "../src/Library/Agent/AgentMcpAdapter.h"
#include "../src/Library/Agent/AgentChatCodecs.h"
#include "../src/Library/Agent/AgentDiagnostic.h"
#include "../src/Library/Agent/Json.h"

using namespace RISE;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const std::string& w )
{
	if( c ) ++g_pass;
	else { ++g_fail; std::printf( "  FAIL: %s\n", w.c_str() ); }
}

static std::string TempPath( const char* name )
{
	const char* base = std::getenv( "TMPDIR" );
	std::string dir = base ? base : "/tmp";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += '/';
	return dir + name;
}

static Job* LoadScene( const std::string& text, const std::string& path )
{
	{ std::ofstream o( path.c_str(), std::ios::binary ); o << text; }
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
		pJob->release();
		std::remove( path.c_str() );
		return nullptr;
	}
	return pJob;
}

//----------------------------------------------------------------------
// Fixtures.
//----------------------------------------------------------------------
static std::string Preamble()
{
	return
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
		"film\n{\n\twidth 24\n\theight 24\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 2 9\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 45.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
		"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
		"directional_light\n{\n\tname key\n\tpower 3.0\n\tcolor 1 1 1\n\tdirection 0.4 0.5 0.9\n}\n\n";
}

//! The apothecary-cat ear: a sphere head (radius 0.05) with a roundcone
//! ear (base 0.02, TIP 0.004, height 0.05) smin-joined at k=0.012 -- 3x
//! the tip radius, and 12.5x mismatched against the head (the SAME
//! fixture AgentReadValidateTest's RunSDFBlendScaleScanTest pins for
//! condition J itself, so this verb is proven against the identical
//! offender the note fires on).  Isotropic scale (1 1 1): reportedMaxK
//! == detectionMaxK here, so this fixture cannot by itself distinguish
//! the two -- case F below uses a squashed twin for that.
static std::string CatEarSdf( const std::string& name )
{
	return "sdf_geometry\n{\n\tname " + name + "\n"
		"\tpart\tsphere union 0  0 0 0  0 0 0  1 1 1  0.05 0 0  0\n"
		"\tpart\troundcone smin 0.012  0 0.05 0  0 0 0  1 1 1  0.02 0.004 0.05  0\n"
		"}\n\n";
}

static std::string StdObj( const std::string& name, const std::string& geom )
{
	return "standard_object\n{\n\tname " + name + "\n\tgeometry " + geom +
		"\n\tmaterial mat_diffuse\n}\n\n";
}

static std::string CatEarScene()
{
	std::string s = Preamble();
	s += CatEarSdf( "cat_head" );
	s += StdObj( "cat_obj", "cat_head" );
	return s;
}

//! The SAME ear geometry, compliant: k dropped to 0.001 (well under the
//! ~1/3 bound) -- no offender anywhere in this document.
static std::string CompliantScene()
{
	std::string s = Preamble();
	s += "sdf_geometry\n{\n\tname compliant_head\n"
		"\tpart\tsphere union 0  0 0 0  0 0 0  1 1 1  0.05 0 0  0\n"
		"\tpart\troundcone smin 0.001  0 0.05 0  0 0 0  1 1 1  0.02 0.004 0.05  0\n"
		"}\n\n";
	s += StdObj( "compliant_obj", "compliant_head" );
	return s;
}

//! TWO offending sdf_geometry chunks, for the target-scoping case.
static std::string TwoOffenderScene()
{
	std::string s = Preamble();
	s += CatEarSdf( "chunk_a" );
	s += CatEarSdf( "chunk_b" );
	s += StdObj( "obj_a", "chunk_a" );
	s += StdObj( "obj_b", "chunk_b" );
	return s;
}

//! The ANISOTROPIC squash: local dims are isotropic-looking (a==b==0.06
//! for the roundcone), but scale (0.05, 1, 1) squashes one axis hard.
//! detection dim (min-scaled) collapses to 0.003 (unreadable, absurd
//! "max ~0.001"); reported dim (median-scaled) stays at 0.06 (readable,
//! "max ~0.0167").  k=0.012 sits BETWEEN the two maxes: it exceeds
//! detectionMaxK (fires) but is UNDER reportedMaxK on its own -- so a
//! clamp that naively used reportedMaxK's rounder number would leave
//! the scan still firing.  This is the exact case the item 3-b
//! reconciliation (clampTargetK = min(reportedMaxK, detectionMaxK))
//! exists for.
static std::string AnisotropicScene()
{
	std::string s = Preamble();
	s += "sdf_geometry\n{\n\tname squashed_head\n"
		"\tpart\tsphere union 0  0 0 0  0 0 0  1 1 1  0.05 0 0  0\n"
		"\tpart\troundcone smin 0.012  0 0.05 0  0 0 0  0.05 1 1  0.06 0.06 0.05  0\n"
		"}\n\n";
	s += StdObj( "squashed_obj", "squashed_head" );
	return s;
}

//! Two SAME-size (radius 0.05) spheres joined at k=0.3 -- well past the
//! total-dissolve gate (dimEffective*5 = 0.25), so it fires on "blend
//! too wide" alone, with NO proportion mismatch (ratio 1:1).  The
//! no-caveat twin of CatEarScene.
static std::string ProportionateOverBlendScene()
{
	std::string s = Preamble();
	s += "sdf_geometry\n{\n\tname proportionate_join\n"
		"\tpart\tsphere union 0  0 0 0  0 0 0  1 1 1  0.05 0 0  0\n"
		"\tpart\tsphere smin 0.3  0.06 0 0  0 0 0  1 1 1  0.05 0 0  0\n"
		"}\n\n";
	s += StdObj( "prop_obj", "proportionate_join" );
	return s;
}

//! The `k` token (index 2 of the 16-token `part` grammar) for `part`
//! occurrence `occ` of chunk `chunkName`, read straight out of the
//! serialized document text -- independent of AgentSession, so this can
//! verify the ACTUAL BYTES changed rather than trusting the struct the
//! verb itself returned.
//! Every fixture in this file authors `part` lines as "\tpart\t<prim> ...",
//! so the literal marker "\tpart\t" (indent + keyword + the pname-to-
//! first-value separator) locates one occurrence -- and, load-bearingly,
//! STILL locates it after DocSetParamValue's own edit: WithParamValue
//! (Cst.cpp) keeps the pname token and its OWN leading trivia untouched,
//! re-tokenizing (to single spaces) only the tokens from the first
//! VALUE token onward. So the "\tpart\t" marker survives the edit
//! byte-for-byte; only the tokens after it change.
static std::string PartKToken( const std::string& doc, const std::string& chunkName, int occ )
{
	const std::size_t chunkPos = doc.find( "name " + chunkName );
	if( chunkPos == std::string::npos ) return std::string();
	static const std::string kMarker = "\tpart\t";
	std::size_t pos = chunkPos;
	int seen = -1;
	while( seen < occ ) {
		pos = doc.find( kMarker, pos );
		if( pos == std::string::npos ) return std::string();
		++seen;
		if( seen == occ ) break;
		pos += kMarker.size();
	}
	// Tokenize the line starting at `pos` (the leading tab before "part").
	const std::size_t eol = doc.find( '\n', pos );
	const std::string line = doc.substr( pos, eol == std::string::npos ? std::string::npos : eol - pos );
	std::vector<std::string> toks;
	std::size_t i = 0;
	while( i < line.size() ) {
		while( i < line.size() && std::isspace( static_cast<unsigned char>( line[i] ) ) ) ++i;
		if( i >= line.size() ) break;
		const std::size_t s = i;
		while( i < line.size() && !std::isspace( static_cast<unsigned char>( line[i] ) ) ) ++i;
		toks.push_back( line.substr( s, i - s ) );
	}
	// toks[0] == "part", toks[1] == prim, toks[2] == op, toks[3] == k
	// (the KEYWORD itself is a token here, unlike the CST param VALUE
	// which excludes it) -- so k is index 3 in THIS line-text tokenization.
	if( toks.size() < 4 ) return std::string();
	return toks[3];
}

//----------------------------------------------------------------------
// A.  BASIC APPLY: clamp, verify the bytes changed, re-scan silent.
//----------------------------------------------------------------------
static void TestBasicApply()
{
	std::printf( "A: basic apply -- clamp, bytes change, re-scan goes silent\n" );
	const std::string tmp = TempPath( "fixblend_a.RISEscene" );
	Job* pJob = LoadScene( CatEarScene(), tmp );
	Check( pJob != nullptr, "A: fixture derives" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

	// PRECONDITION: the scan fires before the call.
	{
		const std::vector<Agent::AgentDiagnostic> diags =
			Agent::AgentSession::ValidateText( sess->ReadDocument() );
		bool fired = false;
		for( const Agent::AgentDiagnostic& d : diags )
			if( d.code == Agent::AgentDiagnosticCode::DESIGN_SDF_BLEND_SCALE ) fired = true;
		Check( fired, "A PRECONDITION: the cat-ear fixture fires DESIGN_SDF_BLEND_SCALE before the call" );
	}

	const std::string before = sess->ReadDocument();
	const std::string kBefore = PartKToken( before, "cat_head", 1 );
	Check( kBefore == "0.012", "A: the authored k reads back as 0.012 before the call (got `" + kBefore + "`)" );

	const Agent::AgentSession::AgentFixBlendScaleResult r = sess->FixBlendScale();
	Check( r.ok && r.applied, "A MONEY: fix_blend_scale applies" );
	Check( r.offendersFound == 1, "A: exactly one offender found" );
	Check( r.fixedCount == 1, "A: exactly one joint clamped" );
	Check( r.remainingCount == 0, "A: nothing left beyond the cap" );
	Check( r.perJointSummary.size() == 1, "A: one per-joint summary line" );
	Check( r.perJointSummary[0].find( "cat_head" ) != std::string::npos &&
	       r.perJointSummary[0].find( "part 2" ) != std::string::npos &&
	       r.perJointSummary[0].find( "0.012" ) != std::string::npos,
	       "A: the summary names the chunk, the part, and the OLD k (got `" + r.perJointSummary[0] + "`)" );

	const std::string after = sess->ReadDocument();
	Check( after != before, "A MONEY: the document actually changed" );
	const std::string kAfter = PartKToken( after, "cat_head", 1 );
	Check( !kAfter.empty() && kAfter != "0.012",
	       "A MONEY: the k TOKEN ITSELF changed in the serialized document (got `" + kAfter + "`)" );
	Check( std::strtod( kAfter.c_str(), nullptr ) < 0.012,
	       "A: ...and the new k is smaller (got " + kAfter + ")" );

	// THE RE-SCAN: the whole point.  Read the NEW document and re-run the
	// SAME scan the fixture was checked against above.
	{
		const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( after );
		bool fired = false;
		for( const Agent::AgentDiagnostic& d : diags )
			if( d.code == Agent::AgentDiagnosticCode::DESIGN_SDF_BLEND_SCALE ) fired = true;
		Check( !fired, "A MONEY: re-scanning the FIXED document is SILENT -- the verb's own clamp target "
		               "actually satisfies the scan's own fire/silent test" );
	}

	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// B.  TARGET SCOPE.
//----------------------------------------------------------------------
static void TestTargetScope()
{
	std::printf( "B: target scope -- fixes only the named chunk\n" );
	const std::string tmp = TempPath( "fixblend_b.RISEscene" );
	Job* pJob = LoadScene( TwoOffenderScene(), tmp );
	Check( pJob != nullptr, "B: fixture derives" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

	const Agent::AgentSession::AgentFixBlendScaleResult r = sess->FixBlendScale( "chunk_a" );
	Check( r.ok && r.applied, "B: the scoped call applies" );
	Check( r.offendersFound == 1, "B MONEY: offendersFound counts ONLY chunk_a's offender, not chunk_b's too" );
	Check( r.fixedCount == 1, "B: one joint clamped" );
	Check( !r.perJointSummary.empty() && r.perJointSummary[0].find( "chunk_a" ) != std::string::npos,
	       "B: the summary names chunk_a" );

	const std::string after = sess->ReadDocument();
	const std::string kA = PartKToken( after, "chunk_a", 1 );
	const std::string kB = PartKToken( after, "chunk_b", 1 );
	Check( kA != "0.012", "B MONEY: chunk_a's k changed" );
	Check( kB == "0.012", "B MONEY: chunk_b's k is UNTOUCHED -- the scope held" );

	// chunk_b's offender must still be findable by a fresh scan.
	{
		const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( after );
		bool fired = false;
		for( const Agent::AgentDiagnostic& d : diags )
			if( d.code == Agent::AgentDiagnosticCode::DESIGN_SDF_BLEND_SCALE &&
			    d.message.find( "chunk_b" ) != std::string::npos ) fired = true;
		Check( fired, "B: chunk_b still fires the scan after the scoped call" );
	}

	// An unresolvable target: a different message than "not found".
	{
		Job* pJob2 = LoadScene( TwoOffenderScene(), TempPath( "fixblend_b2.RISEscene" ) );
		if( pJob2 ) {
			std::unique_ptr<Agent::AgentSession> sess2 = Agent::AgentSession::WrapJob( pJob2 );
			const Agent::AgentSession::AgentFixBlendScaleResult bad = sess2->FixBlendScale( "no_such_chunk" );
			Check( !bad.ok && !bad.applied, "B: an unknown target refuses" );
			Check( bad.message.find( "no chunk named" ) != std::string::npos,
			       "B: ...with the 'does not exist' wording" );
			pJob2->release();
			std::remove( TempPath( "fixblend_b2.RISEscene" ).c_str() );
		}
	}

	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// C.  NO-OFFENDER REFUSAL, document byte-identical.
//----------------------------------------------------------------------
static void TestNoOffenderRefusal()
{
	std::printf( "C: no-offender call -- clean refusal, document unchanged\n" );
	const std::string tmp = TempPath( "fixblend_c.RISEscene" );
	Job* pJob = LoadScene( CompliantScene(), tmp );
	Check( pJob != nullptr, "C: fixture derives" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

	const std::string before = sess->ReadDocument();
	const Agent::AgentSession::AgentFixBlendScaleResult r = sess->FixBlendScale();
	Check( !r.ok && !r.applied, "C MONEY: a clean no-offender call is a refusal (ok=false)" );
	Check( r.offendersFound == 0, "C: offendersFound == 0" );
	Check( r.fixedCount == 0, "C: fixedCount == 0" );
	Check( r.message.find( "no offending smin joints found" ) != std::string::npos,
	       "C: the message is HONEST about there being nothing to fix (got `" + r.message + "`)" );
	Check( sess->ReadDocument() == before, "C MONEY: the document is byte-identical" );

	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// D.  WIRE SURFACE.
//----------------------------------------------------------------------
static void TestWireSurface()
{
	std::printf( "D: wire surface -- chat-codec table, RPC dispatch, MCP advertised AND routable\n" );

	{
		const std::string defs = Agent::ChatToolDefsFingerprint();
		Check( defs.find( "fix_blend_scale" ) != std::string::npos,
		       "D: the verb is declared in the shared kToolDefs table" );
	}

	// MCP: advertised AND routable (the 1ed4e7c3 two-list-drift lesson,
	// pinned by name the way vary_material's own TestWireSurface does).
	{
		const std::string tmp = TempPath( "fixblend_d_mcp.RISEscene" );
		Job* pJob = LoadScene( CatEarScene(), tmp );
		Check( pJob != nullptr, "D MCP: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> mcpSess = Agent::AgentSession::WrapJob( pJob );
			Agent::AgentMcpAdapter mcp( std::move( mcpSess ), Agent::AgentAutonomy::Commit );

			Agent::JsonValue listEnv; std::string lerr;
			Check( Agent::JsonParse( mcp.HandleLine(
			           "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/list\",\"params\":{}}" ),
			       listEnv, lerr ), "D MCP: tools/list parses" );
			bool advertised = false;
			const Agent::JsonValue& tools = listEnv.get( "result" ).get( "tools" );
			for( std::size_t i = 0; i < tools.size(); ++i )
				if( tools.at( i ).get( "name" ).asString() == "fix_blend_scale" ) advertised = true;
			Check( advertised, "D MCP MONEY: tools/list ADVERTISES fix_blend_scale" );

			Agent::JsonValue callEnv; std::string cerr;
			Check( Agent::JsonParse( mcp.HandleLine(
			           "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\","
			           "\"params\":{\"name\":\"fix_blend_scale\",\"arguments\":{}}}" ),
			       callEnv, cerr ), "D MCP: tools/call parses" );
			const bool disowned = callEnv.has( "error" ) &&
			                      callEnv.get( "error" ).get( "code" ).asNumber( 0 ) == -32601.0;
			Check( !disowned, "D MCP MONEY: tools/call ROUTES fix_blend_scale (not -32601)" );
			// And it actually worked, through the MCP transport end to end.
			if( !disowned ) {
				const Agent::JsonValue& result = callEnv.get( "result" );
				Check( !result.get( "isError" ).asBool(), "D MCP: the call is not an error result" );
			}
		}
	}

	// JSON-RPC dispatch, on a fresh job.
	{
		const std::string tmp2 = TempPath( "fixblend_d_rpc.RISEscene" );
		Job* pJob2 = LoadScene( CatEarScene(), tmp2 );
		Check( pJob2 != nullptr, "D RPC: fixture derives" );
		if( pJob2 ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob2 );
			Agent::AgentRpcDispatcher disp( std::move( sess ) );
			const std::string resp = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"fix_blend_scale\",\"params\":{}}" );
			Agent::JsonValue env; std::string perr;
			Check( Agent::JsonParse( resp, env, perr ) && env.isObject(), "D RPC: the response parses" );
			const Agent::JsonValue& result = env.get( "result" );
			Check( result.get( "applied" ).asBool(), "D RPC MONEY: the RPC form applied the fix" );
			Check( static_cast<long long>( result.get( "offendersFound" ).asNumber() ) == 1,
			       "D RPC: ...and echoes offendersFound" );
			Check( static_cast<long long>( result.get( "fixed" ).asNumber() ) == 1,
			       "D RPC: ...and fixed" );
			Check( result.get( "perJoint" ).isArray() && result.get( "perJoint" ).size() == 1,
			       "D RPC: ...and the per-joint summary array" );
			pJob2->release();
			std::remove( tmp2.c_str() );
		}
	}

	// Autonomy: Read refuses; Propose refuses with its OWN message shape
	// (the composite-swap family's convention).
	{
		const std::string tmp3 = TempPath( "fixblend_d_auth.RISEscene" );
		Job* pJob3 = LoadScene( CatEarScene(), tmp3 );
		Check( pJob3 != nullptr, "D auth: fixture derives" );
		if( pJob3 ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob3 );
			Agent::AgentRpcDispatcher disp( std::move( sess ), Agent::AgentAutonomy::Read );
			const std::string resp = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"fix_blend_scale\",\"params\":{}}" );
			Agent::JsonValue env; std::string perr;
			Check( Agent::JsonParse( resp, env, perr ) && env.isObject(), "D auth: response parses" );
			Check( env.has( "error" ), "D auth MONEY: refused under Read autonomy" );
			pJob3->release();
			std::remove( tmp3.c_str() );
		}
	}
	{
		const std::string tmp4 = TempPath( "fixblend_d_prop.RISEscene" );
		Job* pJob4 = LoadScene( CatEarScene(), tmp4 );
		Check( pJob4 != nullptr, "D auth: propose fixture derives" );
		if( pJob4 ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob4 );
			Agent::AgentRpcDispatcher disp( std::move( sess ), Agent::AgentAutonomy::Propose );
			const std::string resp = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"fix_blend_scale\",\"params\":{}}" );
			Agent::JsonValue env; std::string perr;
			Check( Agent::JsonParse( resp, env, perr ) && env.isObject(), "D auth: propose response parses" );
			Check( env.has( "error" ), "D auth: refused under Propose too" );
			const std::string msg = env.get( "error" ).get( "message" ).asString();
			Check( msg.find( "fix_blend_scale" ) != std::string::npos,
			       "D auth: the refusal NAMES the verb" );
			Check( msg.find( "--agent-autonomy=propose" ) != std::string::npos &&
			       msg.find( "--agent-autonomy=commit" ) != std::string::npos,
			       "D auth MONEY: the Propose refusal is the verb's OWN message" );
			pJob4->release();
			std::remove( tmp4.c_str() );
		}
	}

	// External authority: no staged form, document byte-identical.
	{
		const std::string tmp5 = TempPath( "fixblend_d_ext.RISEscene" );
		Job* pJob5 = LoadScene( CatEarScene(), tmp5 );
		Check( pJob5 != nullptr, "D auth: external fixture derives" );
		if( pJob5 ) {
			std::unique_ptr<Agent::AgentSession> sess =
				Agent::AgentSession::WrapJob( pJob5, Agent::AgentAuthority::External );
			const std::string before = sess->ReadDocument();
			const Agent::AgentSession::AgentFixBlendScaleResult r = sess->FixBlendScale();
			Check( !r.ok && !r.applied, "D auth: External authority cannot commit this verb" );
			Check( r.message.find( "no staged-proposal form" ) != std::string::npos,
			       "D auth MONEY: the SAME reason vary_material gives" );
			Check( sess->ReadDocument() == before, "D auth: the document is byte-identical" );
			pJob5->release();
			std::remove( tmp5.c_str() );
		}
	}
}

//----------------------------------------------------------------------
// E.  PROPORTION CAVEAT HONESTY.
//----------------------------------------------------------------------
static void TestProportionCaveat()
{
	std::printf( "E: proportion caveat -- honest on the cat ear, absent on a proportionate over-blend\n" );

	{
		const std::string tmp = TempPath( "fixblend_e1.RISEscene" );
		Job* pJob = LoadScene( CatEarScene(), tmp );
		Check( pJob != nullptr, "E1: cat-ear fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentFixBlendScaleResult r = sess->FixBlendScale();
			Check( r.applied && !r.perJointSummary.empty(), "E1: the cat-ear call applies" );
			if( !r.perJointSummary.empty() )
				Check( r.perJointSummary[0].find( "proportion problem" ) != std::string::npos,
				       "E1 MONEY: the verb's OWN per-joint summary carries the proportion caveat "
				       "(ratio ~12.5, well past the 5x gate) -- got `" + r.perJointSummary[0] + "`" );
			pJob->release();
			std::remove( tmp.c_str() );
		}
	}
	{
		const std::string tmp = TempPath( "fixblend_e2.RISEscene" );
		Job* pJob = LoadScene( ProportionateOverBlendScene(), tmp );
		Check( pJob != nullptr, "E2: proportionate-over-blend fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentFixBlendScaleResult r = sess->FixBlendScale();
			Check( r.applied && !r.perJointSummary.empty(), "E2: the proportionate call applies" );
			if( !r.perJointSummary.empty() )
				Check( r.perJointSummary[0].find( "proportion problem" ) == std::string::npos,
				       "E2 MONEY: NO proportion caveat on a merely over-blended, SAME-size join "
				       "(got `" + r.perJointSummary[0] + "`)" );
			pJob->release();
			std::remove( tmp.c_str() );
		}
	}
}

//----------------------------------------------------------------------
// F.  THE RECONCILIATION INVARIANT (item 3-b): anisotropic scale.
//----------------------------------------------------------------------
static void TestAnisotropicInvariant()
{
	std::printf( "F: anisotropic scale -- clampTargetK = min(reported,detection), re-scan STILL silent\n" );
	const std::string tmp = TempPath( "fixblend_f.RISEscene" );
	Job* pJob = LoadScene( AnisotropicScene(), tmp );
	Check( pJob != nullptr, "F: fixture derives" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

	// PRECONDITION: the message before the fix shows the READABLE
	// (reported) number, not the min-scaled absurd one -- item 3-b's
	// credibility fix, pinned here against the exact fixture the verb is
	// about to act on.
	{
		const std::vector<Agent::AgentDiagnostic> diags =
			Agent::AgentSession::ValidateText( sess->ReadDocument() );
		std::string msg;
		for( const Agent::AgentDiagnostic& d : diags )
			if( d.code == Agent::AgentDiagnosticCode::DESIGN_SDF_BLEND_SCALE ) msg = d.message;
		Check( !msg.empty(), "F PRECONDITION: the squashed fixture fires" );
		Check( msg.find( "max ~0.0166667" ) != std::string::npos,
		       "F PRECONDITION: the message shows the READABLE reported max (~0.0166667), not the "
		       "min-scaled absurd one (~0.001) -- got `" + msg + "`" );
	}

	const Agent::AgentSession::AgentFixBlendScaleResult r = sess->FixBlendScale();
	Check( r.ok && r.applied, "F: the call applies" );

	const std::string after = sess->ReadDocument();
	const std::string kAfter = PartKToken( after, "squashed_head", 1 );
	Check( !kAfter.empty(), "F: the k token reads back" );
	const double kAfterVal = std::strtod( kAfter.c_str(), nullptr );
	// detectionMaxK for this fixture: local dim min(a,b)=0.06 * minScale
	// (0.05) = 0.003; dimEffective = min(0.003, sphere's 0.05) = 0.003;
	// detectionMaxK = 0.003/3 = 0.001.  The clamp MUST be <= this (the
	// GUARANTEED-safe bound), even though the message's own reportedMaxK
	// (~0.0167) is far larger -- if the verb clamped to 0.0167 instead,
	// the re-scan below would still fire.
	Check( kAfterVal <= 0.001 + 1e-9,
	       "F MONEY: the clamp used the STRICTER detection bound (<=~0.001), not the larger reported "
	       "one the message showed (got k=" + kAfter + ")" );

	// THE INVARIANT: re-scan is STILL silent, even on the anisotropic
	// fixture where reported and detection bounds diverge.
	{
		const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( after );
		bool fired = false;
		for( const Agent::AgentDiagnostic& d : diags )
			if( d.code == Agent::AgentDiagnosticCode::DESIGN_SDF_BLEND_SCALE ) fired = true;
		Check( !fired, "F MONEY: the re-scan invariant STILL holds on an anisotropically-scaled part -- "
		               "clampTargetK = min(reportedMaxK, detectionMaxK) reconciles the two" );
	}

	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// G.  finish_element's INLINE FINDING (item 3-a).
//----------------------------------------------------------------------
static std::vector<Agent::AgentSession::AgentBuildPlanEntry> OneElementPlan()
{
	std::vector<Agent::AgentSession::AgentBuildPlanEntry> p;
	Agent::AgentSession::AgentBuildPlanEntry a;
	a.element = "cat";
	a.pieces.push_back( "head" );
	a.pieces.push_back( "ear" );
	a.construction.push_back( "csg" );
	a.outline = "0 0; 2 0; 2 2; 0 2";
	p.push_back( a );
	return p;
}

static void RunNoScoreSweep( const std::string& message, const std::string& label )
{
	std::string lower = message;
	for( std::size_t i = 0; i < lower.size(); ++i )
		lower[i] = static_cast<char>( std::tolower( static_cast<unsigned char>( lower[i] ) ) );
	static const char* const kBanned[] = {
		"similarity", "score", "rmse", "psnr", "ssim", "percent match", "confidence" };
	for( const char* b : kBanned ) {
		Check( lower.find( b ) == std::string::npos,
		       label + ": the blend-scale finding carries no `" + b + "` (Phase 2b's no-score law)" );
	}
}

static void TestFinishElementInlineFinding()
{
	std::printf( "G: finish_element's inline finding -- names the worst offender + the verb, and stays "
	             "silent on a clean shown object\n" );

	// G1: the shown object HAS an offender -- the message names it.
	{
		const std::string tmp = TempPath( "fixblend_g1.RISEscene" );
		Job* pJob = LoadScene( Preamble(), tmp );
		Check( pJob != nullptr, "G1: base fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			Check( sess->FileBuildPlan( OneElementPlan() ).ok, "G1: the plan files" );
			Check( sess->InsertChunk( CatEarSdf( "cat_head" ) ).applied,
			       "G1: the cat-ear sdf_geometry inserts (attributed to the active element)" );
			Check( sess->InsertChunk( StdObj( "cat_obj", "cat_head" ) ).applied,
			       "G1: the cat object inserts" );

			const Agent::AgentSession::AgentFinishElementResult f = sess->FinishElement();
			Check( f.ok && f.rendered, "G1: finish succeeds and renders the shown object" );
			Check( f.isolateObject == "cat_obj", "G1: the cat object is the one shown" );
			Check( f.message.find( "Part 2's blend k dissolves it" ) != std::string::npos,
			       "G1 MONEY: the finish message names the WORST offending part -- got tail: `" +
			       f.message.substr( f.message.size() > 300 ? f.message.size() - 300 : 0 ) + "`" );
			Check( f.message.find( "fix_blend_scale repairs this" ) != std::string::npos,
			       "G1 MONEY: ...and names the VERB that fixes it" );
			Check( f.message.find( "enlarging it" ) != std::string::npos &&
			       f.message.find( "skeleton_geometry" ) != std::string::npos,
			       "G1: ...and the proportion caveat too (this ear IS the 12.5x-mismatched cat ear)" );

			RunNoScoreSweep( f.message, "G1" );
		}
		if( pJob ) { pJob->release(); std::remove( tmp.c_str() ); }
	}

	// G2: the shown object is CLEAN (compliant k) -- silent.
	{
		const std::string tmp = TempPath( "fixblend_g2.RISEscene" );
		Job* pJob = LoadScene( Preamble(), tmp );
		Check( pJob != nullptr, "G2: base fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			Check( sess->FileBuildPlan( OneElementPlan() ).ok, "G2: the plan files" );
			Check( sess->InsertChunk(
				"sdf_geometry\n{\n\tname compliant_head\n"
				"\tpart\tsphere union 0  0 0 0  0 0 0  1 1 1  0.05 0 0  0\n"
				"\tpart\troundcone smin 0.001  0 0.05 0  0 0 0  1 1 1  0.02 0.004 0.05  0\n"
				"}\n" ).applied, "G2: the compliant sdf_geometry inserts" );
			Check( sess->InsertChunk( StdObj( "compliant_obj", "compliant_head" ) ).applied,
			       "G2: the compliant object inserts" );

			const Agent::AgentSession::AgentFinishElementResult f = sess->FinishElement();
			Check( f.ok && f.rendered, "G2: finish succeeds and renders" );
			Check( f.message.find( "fix_blend_scale" ) == std::string::npos,
			       "G2 MONEY: SILENT on a clean shown object -- no offender, nothing to say (got tail: `" +
			       f.message.substr( f.message.size() > 200 ? f.message.size() - 200 : 0 ) + "`" );

			RunNoScoreSweep( f.message, "G2" );
		}
		if( pJob ) { pJob->release(); std::remove( tmp.c_str() ); }
	}
}

int main()
{
	std::printf( "=== AgentFixBlendScaleTest (cat plan item 1: fix_blend_scale) ===\n" );
	TestBasicApply();
	TestTargetScope();
	TestNoOffenderRefusal();
	TestWireSurface();
	TestProportionCaveat();
	TestAnisotropicInvariant();
	TestFinishElementInlineFinding();
	std::printf( "\n=== AgentFixBlendScaleTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
