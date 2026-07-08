//////////////////////////////////////////////////////////////////////
//
//  AgentLoopbackHttpTest.cpp - Secure-MCP slice 3+4: the loopback HTTP/1.1
//    transport regression guard for AgentLoopbackHttpServer.
//
//  Drives the REAL server class (a background thread running Serve())
//  against a REAL raw client socket -- not a subprocess, since the thing
//  under test IS the socket/HTTP layer, and AgentMcpAdapter's own
//  behaviour is already covered end-to-end by AgentMcpAdapterTest /
//  AgentMcpStdioSmokeTest.  Covers:
//
//    (a) bind is loopback: connect to 127.0.0.1:port succeeds; the bound
//        sockaddr is asserted to be INADDR_LOOPBACK (a environment-
//        independent proxy for "not reachable on a non-loopback
//        address" -- see the file for why a live LAN-IP connect attempt
//        is not used).
//    (b) full round-trip: initialize, tools/call render, tools/call
//        read_image (PNG magic).
//    (c) HTTP correctness: GET / -> 405; POST to the wrong path -> 404;
//        missing Content-Length -> 400; Content-Length over the cap ->
//        413 (no crash/OOM); a truncated body -> clean 400, no hang.
//    (d) serialization: N concurrent client connections each POST a
//        distinct request; every response is correct and un-interleaved,
//        and the reentrancy guard never trips (it would abort the WHOLE
//        TEST PROCESS if it did -- so "the process is still here at the
//        end" IS the proof it never tripped).
//    (e) malformed-input fuzz: truncated request line, unterminated
//        headers (bounded by the server's own recv timeout), a
//        non-numeric Content-Length, a negative-looking Content-Length,
//        embedded NULs in the body -- each closes cleanly, never hangs
//        (the test's own connect/read calls are themselves given a
//        bounded timeout so a regression here fails the TEST instead of
//        wedging the runner), never crashes the server (proven by a
//        healthy request succeeding immediately afterward).
//    (f) clean shutdown: Stop() + thread join completes within a bounded
//        time.
//    (g) request-smuggling preconditions (RFC 7230 SS3.3.3, review P2):
//        a request with TWO Content-Length headers -> 400; a request
//        bearing a Transfer-Encoding header at all -> 400 (this server
//        never does chunked). RED-PROVE: reverting the
//        contentLengthCount/hasTransferEncoding checks in
//        AgentLoopbackHttpServer.cpp flips both of these back to 200.
//    (h) oversized header block (review P3-2): a 128 KB header block
//        (with no terminating blank line) -> 431 Request Header Fields
//        Too Large. RED-PROVE: before the fix, ReadUntilDoubleCrlf reused
//        kMaxBodyBytes (8 MiB) as the header cap, so this same 128 KB
//        input was still WITHIN the cap and the loop kept reading
//        (eventually timing out rather than rejecting promptly); with the
//        dedicated kMaxHeaderBytes (64 KiB) cap this is now rejected
//        immediately.
//    (i) shutdown-race stress guard (review P1-1): repeated Bind/
//        Serve-thread/concurrent-connect/Stop cycles with no crash/hang --
//        a behavioural stand-in for a TSan run (not available in this
//        build; see the slice-3 final report) against the
//        std::atomic<SOCKET> fix for the Stop()/Serve() data race on
//        mListenSock.
//    (j) [SLICE 4] bearer-token auth: no Authorization header -> 401 +
//        WWW-Authenticate: Bearer (RED-PROVE: with the auth check removed,
//        this same request dispatches and returns 200); wrong token -> 401;
//        correct token -> 200 with a valid MCP response; every OTHER test
//        case in this file (a)-(i) now sends the correct token via
//        DoRequest()'s auto-attached Authorization header, which is itself
//        proof the auth gate doesn't false-positive-reject a legitimate
//        client.
//    (k) [SLICE 4] Origin validation: `Origin: http://evil.com` present,
//        even WITH a correct bearer token, -> 403 (RED-PROVE: with the
//        Origin check removed, this same request dispatches and returns
//        200 -- proves the Origin check is NOT subsumed by the auth check,
//        i.e. a stolen/leaked token alone is not enough to get past a
//        browser-borne DNS-rebinding request); an explicit loopback Origin
//        (http://127.0.0.1:<port>) is accepted; no Origin header at all
//        (the non-browser-client case) is accepted.
//    (l) [SLICE 4] Host validation: `Host: evil.com` (the DNS-rebinding
//        shape: an attacker domain that a browser resolves to 127.0.0.1)
//        -> 403 (RED-PROVE: with the Host check removed, this same request
//        dispatches and returns 200); `Host: 127.0.0.1:<port>` and
//        `Host: localhost` are both accepted.
//    (m) [SLICE 4] per-launch token entropy: TWO separate server
//        construction cycles in this one test binary produce two DIFFERENT
//        64-hex-char tokens -- proves the token comes from a per-launch
//        CSPRNG draw, not a fixed/compiled-in constant.
//    (n) [SLICE 4] ConstantTimeEquals unit coverage (the helper is
//        anonymous-namespace-private to AgentLoopbackHttpServer.cpp, so
//        this is exercised INDIRECTLY via the wrong-token-differs-by-one-
//        byte-at-various-positions cases in (j) -- equal compares true;
//        differing at the first byte, the last byte, or by length all
//        compare false; see the doc comment on ConstantTimeEquals itself
//        in the .cpp for why it does not short-circuit) PLUS a DIRECT unit
//        check via the ForTest_ConstantTimeEquals() seam
//        [SLICE 4 HARDENING] added specifically for the 65536-length-fold
//        regression below (o).
//    (o) [SLICE 4 HARDENING] IsLoopbackHost case-insensitivity + IPv6-
//        bracket parsing: `Host: Localhost` / `Host: LOCALHOST` -> 200
//        (RED-PROVE: pre-fix, the case-sensitive `== "localhost"` compare
//        403'd these); `Host: 127.0.0.1.evil.com` still -> 403 (regression
//        guard -- proves the fix didn't loosen this into a suffix/prefix
//        match); `Origin: http://LOCALHOST:<port>` -> 200 (the Origin path
//        delegates host-parsing to the same fixed function).
//    (p) [SLICE 4 HARDENING] ConstantTimeEquals length-fold: a direct unit
//        check (via ForTest_ConstantTimeEquals(), a test-only forwarder --
//        see the .h) that two strings whose lengths differ by an EXACT
//        multiple of 65536 (64 vs 65600) compare UNEQUAL. RED-PROVE:
//        pre-fix, the length fold packed only the low 16 bits of
//        `lenA ^ lenB`, which are all zero for a 65536 stride, so this
//        exact pair folded to "equal" -- unreachable via a live HTTP
//        request today (65600 > kMaxHeaderBytes, so a real request that
//        long 431s before reaching the token compare) but a landmine
//        against a future header-cap increase; this unit test exercises
//        the helper directly, independent of that cap.
//
//  POSIX-only (BSD sockets, pthread via std::thread). On Windows the
//  whole body compiles to a trivial pass (matches every other
//  fork/socket-based agentic-surface test in this suite) -- the Windows
//  leg of AgentLoopbackHttpServer itself is UNCOMPILED/unverified here;
//  see the slice-3 final report for the owed follow-up (this now includes
//  the WSAStartup wiring added in the review-fix round).
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/AgentLoopbackHttpServer.h"
#include "../src/Library/Agent/AgentMcpAdapter.h"
#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/Json.h"
#include "../src/Library/Agent/Base64.h"
#include "../src/Library/Job.h"                        // Secure-MCP slice 5c end-to-end test
#include "../src/Library/SceneEditor/SceneEditController.h"  // Secure-MCP slice 5c end-to-end test
#include "../src/Library/Interfaces/IMaterial.h"        // Secure-MCP slice 5c end-to-end test
#include "../src/Library/Interfaces/IMaterialManager.h" // Secure-MCP slice 5c end-to-end test
#include "../src/Library/Interfaces/IEmitter.h"         // Secure-MCP slice 5c end-to-end test

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace RISE;
using namespace RISE::Agent;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const std::string& w )
{
	if( c ) ++g_pass;
	else { ++g_fail; std::printf( "  FAIL: %s\n", w.c_str() ); }
}

#ifndef _WIN32

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <csignal>

// Same tiny inline native-v7 scene the other agentic-surface tests use.
static const char* const kScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 16\n\theight 16\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n\n"
	"standard_object\n{\n\tname obj_sph\n\tgeometry sph\n\tmaterial mat_diffuse\n}\n";

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

//! Connects a bare TCP client socket to 127.0.0.1:port, with a bounded
//! connect + recv/send timeout so a regression in the server (a hang,
//! a wedge) fails THIS test promptly instead of wedging the whole suite
//! runner. Returns -1 on failure.
static int ConnectLoopback( unsigned short port, int timeoutMs = 3000 )
{
	int s = socket( AF_INET, SOCK_STREAM, 0 );
	if( s < 0 ) return -1;

	struct timeval tv;
	tv.tv_sec = timeoutMs / 1000;
	tv.tv_usec = ( timeoutMs % 1000 ) * 1000;
	setsockopt( s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof( tv ) );
	setsockopt( s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof( tv ) );

	struct sockaddr_in addr;
	memset( &addr, 0, sizeof( addr ) );
	addr.sin_family = AF_INET;
	addr.sin_port = htons( port );
	inet_pton( AF_INET, "127.0.0.1", &addr.sin_addr );

	if( connect( s, reinterpret_cast<struct sockaddr*>( &addr ), sizeof( addr ) ) < 0 ) {
		close( s );
		return -1;
	}
	return s;
}

//! Sends `data` in full (loop over short writes); returns false on any
//! send failure.
static bool SendAll( int s, const std::string& data )
{
	std::size_t sent = 0;
	while( sent < data.size() ) {
		const ssize_t n = send( s, data.data() + sent, data.size() - sent, 0 );
		if( n <= 0 ) return false;
		sent += static_cast<std::size_t>( n );
	}
	return true;
}

//! Reads until the peer closes (or the socket's own recv timeout fires),
//! returning everything read. Used for a v1 no-keep-alive server where
//! every response ends with the server closing the connection.
static std::string ReadAllUntilClose( int s )
{
	std::string out;
	char buf[4096];
	for( ;; ) {
		const ssize_t n = recv( s, buf, sizeof( buf ), 0 );
		if( n <= 0 ) break;
		out.append( buf, static_cast<std::size_t>( n ) );
	}
	return out;
}

struct HttpResponse
{
	bool        ok = false;   // parsed a status line + got a body per Content-Length
	int         status = 0;
	std::string body;
};

static HttpResponse ParseHttpResponse( const std::string& raw )
{
	HttpResponse r;
	const std::size_t headEnd = raw.find( "\r\n\r\n" );
	if( headEnd == std::string::npos ) return r;
	const std::string head = raw.substr( 0, headEnd );
	const std::string body = raw.substr( headEnd + 4 );

	const std::size_t firstCrlf = head.find( "\r\n" );
	const std::string statusLine = ( firstCrlf == std::string::npos ) ? head : head.substr( 0, firstCrlf );
	// "HTTP/1.1 200 OK"
	const std::size_t sp1 = statusLine.find( ' ' );
	if( sp1 == std::string::npos ) return r;
	const std::size_t sp2 = statusLine.find( ' ', sp1 + 1 );
	const std::string codeStr = ( sp2 == std::string::npos ) ? statusLine.substr( sp1 + 1 )
	                                                          : statusLine.substr( sp1 + 1, sp2 - sp1 - 1 );
	r.status = std::atoi( codeStr.c_str() );
	r.body = body;
	r.ok = r.status != 0;
	return r;
}

//! Extra headers a test wants layered onto an otherwise well-formed
//! request, for the auth/Origin/Host coverage below. Any field left empty
//! is omitted entirely (rather than sent as an empty header value) EXCEPT
//! `host`, which always defaults to "127.0.0.1" (a bare TCP client like
//! this test harness must send SOME Host for the server's parser to see a
//! well-formed request; the Host-rebind test overrides it explicitly).
struct ExtraHeaders
{
	std::string host = "127.0.0.1";
	std::string authorization;   // e.g. "Bearer <token>" -- omitted if empty
	bool        hasAuthorization = false;
	std::string origin;          // omitted unless hasOrigin
	bool        hasOrigin = false;
};

//! Round-trips ONE HTTP request over a fresh connection and returns the
//! parsed response. `extra` carries the Secure-MCP slice 4 headers
//! (Authorization / Origin / Host override) -- defaulted so every
//! pre-slice-4 call site below is unchanged in spirit, but see the
//! `g_testToken`-sending overload immediately below, which is what most
//! call sites actually use post-slice-4 (a request with NO Authorization
//! header now gets 401 before ever reaching the cases these earlier tests
//! were written to exercise).
static HttpResponse DoRequestEx( unsigned short port, const std::string& method,
                                   const std::string& path, const std::string& body,
                                   const ExtraHeaders& extra )
{
	const int s = ConnectLoopback( port );
	if( s < 0 ) return HttpResponse();

	std::string req = method + " " + path + " HTTP/1.1\r\n";
	req += "Host: " + extra.host + "\r\n";
	if( extra.hasAuthorization ) req += "Authorization: " + extra.authorization + "\r\n";
	if( extra.hasOrigin ) req += "Origin: " + extra.origin + "\r\n";
	req += "Content-Length: " + std::to_string( body.size() ) + "\r\n";
	req += "\r\n";
	req += body;

	HttpResponse r;
	if( SendAll( s, req ) ) {
		const std::string raw = ReadAllUntilClose( s );
		r = ParseHttpResponse( raw );
	}
	close( s );
	return r;
}

//! The bearer token for whichever server the CURRENT test block is
//! driving -- set once near the top of main() (and again for the
//! per-launch-token-differs case, which spins up SEPARATE servers).
//! A plain global is fine here: this test, like the server it drives, is
//! single-threaded except for the deliberately-concurrent sections, which
//! only ever read this (never write it).
static std::string g_testToken;

//! Convenience wrapper: same as DoRequestEx, but with a valid
//! `Authorization: Bearer <g_testToken>` already attached -- this is what
//! every PRE-EXISTING slice-3 test case below now needs to keep passing,
//! since a request with no Authorization header is unconditionally 401
//! post-slice-4.
static HttpResponse DoRequest( unsigned short port, const std::string& method,
                                const std::string& path, const std::string& body )
{
	ExtraHeaders extra;
	extra.hasAuthorization = true;
	extra.authorization = "Bearer " + g_testToken;
	return DoRequestEx( port, method, path, body, extra );
}

//! Builds an MCP/JSON-RPC request line.
static std::string Req( double id, const std::string& method, const JsonValue& params )
{
	JsonValue r = JsonValue::MakeObject();
	r.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
	r.set( "id", JsonValue::MakeNumber( id ) );
	r.set( "method", JsonValue::MakeString( method ) );
	r.set( "params", params );
	return JsonSerialize( r );
}

static std::string ReqInitialize( double id )
{
	JsonValue params = JsonValue::MakeObject();
	params.set( "protocolVersion", JsonValue::MakeString( "2025-03-26" ) );
	params.set( "capabilities", JsonValue::MakeObject() );
	return Req( id, "initialize", params );
}

static std::string ReqToolCall( double id, const std::string& name, const JsonValue& args )
{
	JsonValue params = JsonValue::MakeObject();
	params.set( "name", JsonValue::MakeString( name ) );
	params.set( "arguments", args );
	return Req( id, "tools/call", params );
}

//////////////////////////////////////////////////////////////////////
// Secure-MCP slice 5c: a SceneEditController subclass whose
// DoOneRenderPass is a fast no-op-ish sleep loop (mirrors
// AgentLiveCommitTest.cpp's own TestController) -- gives StageProposal /
// ListProposals / ResolveProposal a REAL render thread + REAL mMutex to
// serialize against, without needing an actual rasterizer wired up.
//////////////////////////////////////////////////////////////////////
class Slice5cTestController : public SceneEditController
{
public:
	explicit Slice5cTestController( IJobPriv& job, unsigned int simulatedRenderMs = 10 )
	: SceneEditController( job, /*interactiveRasterizer*/0 )
	, mSimulatedRenderMs( simulatedRenderMs )
	{}

protected:
	void DoOneRenderPass() override
	{
		const unsigned int sliceMs = 2;
		const unsigned int slices  = ( mSimulatedRenderMs + sliceMs - 1 ) / sliceMs;
		for( unsigned int i = 0; i < slices; ++i ) {
			if( IsCancelRequested() ) return;
			std::this_thread::sleep_for( std::chrono::milliseconds( sliceMs ) );
		}
	}

private:
	unsigned int mSimulatedRenderMs;
};

//////////////////////////////////////////////////////////////////////
// Secure-MCP slice 5c: THE end-to-end topology 5b couldn't test
// headlessly (no controller) -- now with a real live TestController:
//
//   1. An External-authority AgentSession WrapJob's the SAME Job a
//      TestController is attached to, is AttachController'd to it, gets
//      a session label, and is wrapped in an AgentMcpAdapter (Propose
//      autonomy) fronted by a REAL AgentLoopbackHttpServer on its own
//      thread -- exactly the shape RISEViewportBridge's
//      -startAgentHostedServerWithLabel: builds in the GUI.
//   2. A REAL HTTP client (raw socket, like every other case in this
//      file) POSTs tools/call propose_patch through that server -- this
//      is "an external HTTP client stages a proposal that lands in the
//      LIVE controller's queue".
//   3. A SECOND, Owner-authority AgentSession (mirroring the GUI's
//      in-process `_agentDispatcher`) wraps the SAME Job, is
//      AttachController'd to the SAME controller, and lists + resolves
//      that proposal DIRECTLY (in-process, no HTTP) -- exactly like the
//      GUI's agentHandleLine path.
//   4. The live scene actually changes: LumR() reads the material's
//      derived emissive value before and after approval.
//
// Also covers: sessionLabel populated end-to-end (the staged proposal,
// read back via the OWNER's ListProposals, carries the label the
// External session was constructed with) -- and the two-dispatcher-one-
// controller concurrency claim (external-server-thread stages ‖ a
// concurrent GUI-style edit on the owner side), watchdogged.
//////////////////////////////////////////////////////////////////////
static void TestGuiHostedServerEndToEnd()
{
	std::printf( "=== Secure-MCP slice 5c: GUI-hosted server end-to-end (external stages into a live scene, owner approves) ===\n" );

	// `exitance white` (color 1 1 1) so averageRadiantExitance().r == scale
	// exactly -- matches AgentLiveCommitTest.cpp's kBaseScene convention.
	const char* const kMatScene =
		"RISE ASCII SCENE 7\n"
		"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
		"lambertian_luminaire_material\n{\nname lum\nexitance white\nscale 5.0\nmaterial none\n}\n"
		"sphere_geometry\n{\nname s\nradius 1\n}\n"
		"standard_object\n{\nname obj\ngeometry s\nmaterial lum\n}\n";

	const std::string scenePath = WriteTemp( "rise_agent_http_5c_test.RISEscene", kMatScene );
	Check( !scenePath.empty(), "5c: wrote the end-to-end scene to a temp file" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "5c: base scene loads via the CST path" );

	auto lumScale = [&]() -> double {
		IMaterial* m = pJob->GetMaterials() ? pJob->GetMaterials()->GetItem( "lum" ) : 0;
		if( !m || !m->GetEmitter() ) return -1.0;
		return (double)m->GetEmitter()->averageRadiantExitance().r;
	};

	{
		Slice5cTestController controller( *pJob, /*simulatedRenderMs*/10 );
		controller.Start();

		// ---- (1) the GUI-hosted EXTERNAL server, over the SAME live
		// controller, labelled -- mirrors -startAgentHostedServerWithLabel:.
		std::unique_ptr<AgentSession> extSession =
			AgentSession::WrapJob( pJob, AgentAuthority::External );
		Check( extSession != nullptr, "5c: external AgentSession constructed (WrapJob)" );
		extSession->AttachController( &controller );
		extSession->SetSessionLabel( "external-http" );

		AgentMcpAdapter extAdapter( std::move( extSession ), RISE::Agent::AgentAutonomy::Propose );
		AgentLoopbackHttpServer extServer( &extAdapter, "/mcp" );
		Check( extServer.Bind( 0 ), "5c: external server Bind(0) succeeds" );
		const unsigned short extPort = extServer.BoundPort();
		const std::string extToken = extServer.ForTest_Token();
		std::thread extServerThread( [&]() { extServer.Serve(); } );

		// ---- (2) a REAL external HTTP client stages a propose_patch.
		{
			ExtraHeaders extra;
			extra.hasAuthorization = true;
			extra.authorization = "Bearer " + extToken;
			JsonValue args = JsonValue::MakeObject();
			args.set( "target", JsonValue::MakeString( "lum" ) );
			args.set( "kind",   JsonValue::MakeString( "lambertian_luminaire_material" ) );
			args.set( "param",  JsonValue::MakeString( "scale" ) );
			args.set( "value",  JsonValue::MakeString( "13.0" ) );
			HttpResponse r = DoRequestEx( extPort, "POST", "/mcp",
				ReqToolCall( 1, "propose_patch", args ), extra );
			Check( r.ok && r.status == 200, "5c: external tools/call(propose_patch) -> HTTP 200" );
			JsonValue env; std::string perr;
			Check( JsonParse( r.body, env, perr ), "5c: external propose_patch response parses as JSON" );
			const JsonValue& result = env.get( "result" );
			Check( !result.get( "isError" ).asBool( true ),
			       "5c: external propose_patch is NOT a tool-execution error (isError == false)" );
		}
		Check( lumScale() > 4.9 && lumScale() < 5.1,
		       "5c: RED-PROVE the stage is INERT -- the live scene's scale is STILL ~5.0 "
		       "(would already be ~13.0 if External committed instead of staging)" );

		// ---- (3) the OWNER session (mirrors the GUI's in-process
		// _agentDispatcher) lists + resolves it IN-PROCESS, no HTTP.
		std::unique_ptr<AgentSession> ownerSession =
			AgentSession::WrapJob( pJob, AgentAuthority::Owner );
		Check( ownerSession != nullptr, "5c: owner AgentSession constructed (WrapJob)" );
		ownerSession->AttachController( &controller );

		const std::vector<AgentSession::AgentProposalEntry> listed = ownerSession->ListProposals();
		Check( listed.size() == 1, "5c: owner's ListProposals sees exactly the one staged proposal" );
		std::uint64_t proposalId = 0;
		if( listed.size() == 1 ) {
			proposalId = listed[0].id;
			Check( listed[0].status == "pending", "5c: the staged proposal's status is \"pending\"" );
			Check( listed[0].kind == "param_edit", "5c: the staged proposal's kind is \"param_edit\"" );
			Check( listed[0].sessionLabel == "external-http",
			       "5c: sessionLabel WIRED END-TO-END -- the proposal staged by the labelled "
			       "External session carries \"external-http\" when read back via the owner's "
			       "ListProposals (RED-PROVE: this was always \"\" before slice 5c)" );
		}

		Check( proposalId != 0, "5c: a nonzero proposal id was staged" );
		if( proposalId != 0 ) {
			const AgentSession::AgentResolveResult rr =
				ownerSession->ResolveProposal( proposalId, /*approve=*/true );
			Check( rr.ok, "5c: owner ResolveProposal(approve) runs" );
			Check( rr.status == "applied", "5c: resolve status is \"applied\"" );
		}

		Check( lumScale() > 12.9 && lumScale() < 13.1,
		       "5c: THE LIVE SCENE ACTUALLY CHANGED -- scale is now ~13.0 after owner approval "
		       "(external staged it; owner approved it; the render-thread-guarded live controller "
		       "queue carried it end-to-end)" );

		// ---- (4) two-dispatcher-one-controller concurrency: fire a
		// second external stage CONCURRENTLY with an owner-side resolve
		// of a THIRD proposal, watchdogged (no deadlock/corruption).
		std::unique_ptr<AgentSession> extSession2 =
			AgentSession::WrapJob( pJob, AgentAuthority::External );
		extSession2->AttachController( &controller );
		extSession2->SetSessionLabel( "external-http" );
		Agent::AgentSetPatch p3;
		p3.target = "lum"; p3.kind = "lambertian_luminaire_material";
		p3.param  = "scale"; p3.value = "21.0";
		const AgentPatchResult thirdStage = extSession2->ProposePatch( p3 );
		Check( thirdStage.status == "staged", "5c: a third proposal stages cleanly before the concurrency stress" );

		std::atomic<bool> raceDone( false );
		std::thread raceExternal( [&]() {
			// A burst of external stages via the HTTP server, concurrent
			// with the owner resolving below.
			for( int i = 0; i < 20; ++i ) {
				ExtraHeaders extra;
				extra.hasAuthorization = true;
				extra.authorization = "Bearer " + extToken;
				JsonValue args = JsonValue::MakeObject();
				args.set( "target", JsonValue::MakeString( "lum" ) );
				args.set( "kind",   JsonValue::MakeString( "lambertian_luminaire_material" ) );
				args.set( "param",  JsonValue::MakeString( "scale" ) );
				args.set( "value",  JsonValue::MakeString( "7.0" ) );
				DoRequestEx( extPort, "POST", "/mcp", ReqToolCall( 100 + i, "propose_patch", args ), extra );
			}
			raceDone.store( true, std::memory_order_release );
		} );
		// Owner-side: resolve the third proposal + repeatedly list, racing
		// the external stages above -- exercises the controller's mMutex
		// from both the hosted-server thread and this (owner) thread at
		// the same time, matching the two-dispatcher-one-controller model.
		std::uint64_t thirdId = 0;
		for( const auto& e : ownerSession->ListProposals() ) if( e.status == "pending" && e.value == "21.0" ) thirdId = e.id;
		Check( thirdId != 0, "5c: found the third proposal's id before the concurrency stress" );
		bool sawNoCrash = true;
		for( int i = 0; i < 20; ++i ) {
			(void)ownerSession->ListProposals();
			std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
		}
		if( thirdId != 0 ) {
			const AgentSession::AgentResolveResult rrThird = ownerSession->ResolveProposal( thirdId, /*approve=*/true );
			sawNoCrash = sawNoCrash && rrThird.ok;
		}

		// Bounded join -- fail loudly rather than hang the suite if the
		// race produced a deadlock.
		{
			std::atomic<bool> joined( false );
			std::thread joiner( [&]() { raceExternal.join(); joined.store( true, std::memory_order_release ); } );
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 10 );
			while( !joined.load( std::memory_order_acquire ) && std::chrono::steady_clock::now() < deadline ) {
				std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
			}
			Check( joined.load( std::memory_order_acquire ),
			       "5c: the concurrent external-stage burst thread joined within 10s (no deadlock)" );
			if( joiner.joinable() ) joiner.join();
		}
		Check( raceDone.load( std::memory_order_acquire ), "5c: the external-stage burst completed" );
		Check( sawNoCrash, "5c: owner-side resolve during the concurrent external-stage burst succeeded, no corruption" );

		// ---- teardown, in the documented order: stop the external
		// server (join) BEFORE detaching sessions / stopping the
		// controller -- mirrors -shutdown's ordering in RISEViewportBridge.mm.
		extServer.Stop();
		if( extServerThread.joinable() ) extServerThread.join();
		ownerSession->AttachController( nullptr );
		extSession2->AttachController( nullptr );
		controller.Stop();
	}

	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== Secure-MCP slice 5c: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// Secure-MCP slice 6 test seam: a deterministic millisecond clock the
// mutating-verb rate limiter can be pointed at via
// AgentLoopbackHttpServer::ForTest_SetRateLimitClockFn -- lets the
// window-rollover case below advance the fixed window WITHOUT a real
// 60-second sleep. A plain global (not thread-local) is fine here: the
// test thread only ever WRITES it strictly BEFORE issuing the next
// request (whose full synchronous round-trip over the loopback socket is
// what the server's single serving thread reads it AFTER) -- the
// request/response round-trip is itself the happens-before edge, so
// there is no data race despite the value crossing threads.
//////////////////////////////////////////////////////////////////////
static std::atomic<std::int64_t> g_fakeRateLimitClockMs( 0 );
static std::int64_t FakeRateLimitClockMs() { return g_fakeRateLimitClockMs.load( std::memory_order_relaxed ); }

//////////////////////////////////////////////////////////////////////
// Secure-MCP slice 6: the mutating-verb fixed-window rate limit + the
// per-connection total-request read deadline. Self-contained (its own
// scene + server constructions) so it doesn't perturb the request counts
// the sections in main() above already rely on.
//
//   (a) the first kMutatingRateLimitMaxCalls mutating calls in a window
//       are NOT rate-limited; the (max+1)th IS, with a distinct top-level
//       JSON-RPC error (kMutatingRateLimitExceeded, -32013) that echoes
//       the original request's id. RED-PROVE target: removing
//       AgentLoopbackHttpServer::CheckAndConsumeMutatingRateLimit's cap
//       check would let this same (max+1)th call dispatch normally
//       instead.
//   (b) a READ-SAFE verb (render) is UNAFFECTED even in the SAME
//       exhausted window -- the limiter counts ONLY mutating tools/call
//       names.
//   (c) the fixed window ROLLS OVER: using the injected fake clock, a
//       call made exactly kMutatingRateLimitWindowMs after the window
//       started succeeds again, proving this is a genuine fixed-window
//       counter (reset on rollover), not a permanent one-shot cap.
//   (d) [total-request deadline] a per-byte "slow-loris" drip whose
//       INDIVIDUAL gaps are well inside the 5s per-recv socket timeout,
//       but whose TOTAL time exceeds a (test-shrunk)
//       mTotalRequestDeadlineMs, is cut off with 408 Request Timeout --
//       RED-PROVE target: the per-recv timeout ALONE (pre-slice-6
//       behaviour) would let this same drip keep the connection alive
//       indefinitely (bounded only by however long the test is willing to
//       keep dripping). A normal, non-dripped request against the SAME
//       short deadline still succeeds -- the deadline doesn't
//       false-positive-reject ordinary traffic.
//////////////////////////////////////////////////////////////////////
static void TestMutatingRateLimitAndTotalDeadline()
{
	std::printf( "=== Secure-MCP slice 6: mutating-verb rate limit + total-request deadline ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_http_slice6_test.RISEscene", kScene );
	Check( !scenePath.empty(), "slice6: wrote the smoke scene to a temp file" );

	//------------------------------------------------------------------
	// (a)+(b): window cap + read-safe-verb exemption.
	//------------------------------------------------------------------
	{
		std::unique_ptr<AgentSession> session = AgentSession::LoadFromFile( scenePath );
		Check( session != nullptr, "slice6(a): AgentSession::LoadFromFile loaded the temp scene" );
		// Read autonomy: propose_patch is refused downstream (kAutonomyRefused)
		// regardless of the rate limiter -- irrelevant here, since the
		// limiter is a TRANSPORT-layer check that fires BEFORE dispatch,
		// purely off the request's {method, params.name} shape. Using Read
		// (no live controller) keeps this section self-contained and
		// proves the limiter counts a mutating CALL ATTEMPT, not a
		// mutating call that actually succeeded downstream.
		AgentMcpAdapter adapter( std::move( session ), RISE::Agent::AgentAutonomy::Read );
		AgentLoopbackHttpServer server( &adapter, "/mcp" );
		Check( server.Bind( 0 ), "slice6(a): server Bind(0) succeeds" );
		const unsigned short port = server.BoundPort();
		const std::string token = server.ForTest_Token();
		std::thread serverThread( [&]() { server.Serve(); } );

		ExtraHeaders extra;
		extra.hasAuthorization = true;
		extra.authorization = "Bearer " + token;

		JsonValue args = JsonValue::MakeObject();
		args.set( "target", JsonValue::MakeString( "sph" ) );
		args.set( "param",  JsonValue::MakeString( "radius" ) );
		args.set( "value",  JsonValue::MakeString( "0.9" ) );

		bool allowedOk = true;
		for( int i = 0; i < AgentLoopbackHttpServer::kMutatingRateLimitMaxCalls; ++i ) {
			HttpResponse r = DoRequestEx( port, "POST", "/mcp",
				ReqToolCall( i + 1, "propose_patch", args ), extra );
			if( !r.ok || r.status != 200 ) { allowedOk = false; break; }
			JsonValue env; std::string perr;
			if( !JsonParse( r.body, env, perr ) ) { allowedOk = false; break; }
			// Each of these first kMutatingRateLimitMaxCalls calls may
			// still be a NORMAL kAutonomyRefused tool-execution error (a
			// SUCCESS envelope whose CallToolResult carries isError:true,
			// per Read autonomy) -- that is NOT this test's concern. Only
			// a TOP-LEVEL JSON-RPC error carrying the rate-limit code
			// would mean this call was wrongly throttled.
			if( env.has( "error" ) ) {
				const JsonValue& err = env.get( "error" );
				const JsonValue* code = err.find( "code" );
				if( code && code->isNumber() && code->asNumber() == -32013.0 ) { allowedOk = false; break; }
			}
		}
		Check( allowedOk, "slice6(a): the first kMutatingRateLimitMaxCalls mutating calls in the window are NOT rate-limited" );

		// The (max+1)th mutating call in the SAME window IS rate-limited.
		HttpResponse overflow = DoRequestEx( port, "POST", "/mcp",
			ReqToolCall( 9000, "propose_patch", args ), extra );
		Check( overflow.ok && overflow.status == 200,
		       "slice6(a): the rate-limited response is still HTTP 200 (the JSON-RPC error rides inside 200, "
		       "matching every other dispatcher-level error on this transport)" );
		JsonValue overflowEnv; std::string operr;
		Check( JsonParse( overflow.body, overflowEnv, operr ), "slice6(a): rate-limited response body parses as JSON" );
		Check( overflowEnv.has( "error" ),
		       "RED-PROVE: the (max+1)th mutating call in the window is a top-level JSON-RPC error" );
		if( overflowEnv.has( "error" ) ) {
			const JsonValue& err = overflowEnv.get( "error" );
			const JsonValue* code = err.find( "code" );
			Check( code && code->isNumber() && code->asNumber() == -32013.0,
			       "RED-PROVE: the rate-limit error carries the distinct kMutatingRateLimitExceeded code (-32013)" );
		}
		const JsonValue* idv = overflowEnv.find( "id" );
		Check( idv && idv->isNumber() && idv->asNumber() == 9000.0,
		       "the rate-limit error echoes the ORIGINAL request's id (9000)" );

		// (b) a READ-SAFE verb is UNAFFECTED -- unlimited, even in the SAME
		// exhausted window.
		HttpResponse readOk = DoRequestEx( port, "POST", "/mcp",
			ReqToolCall( 9001, "render", JsonValue::MakeObject() ), extra );
		Check( readOk.ok && readOk.status == 200, "slice6(b): a read-safe verb (render) still gets HTTP 200 in the SAME exhausted window" );
		JsonValue readEnv; std::string rerr;
		Check( JsonParse( readOk.body, readEnv, rerr ), "slice6(b): render response body parses as JSON" );
		Check( !readEnv.has( "error" ),
		       "slice6(b): render is NOT rate-limited (no top-level error at all) even though the mutating "
		       "window is exhausted -- the limiter only ever counts mutating verb names" );

		server.Stop();
		if( serverThread.joinable() ) serverThread.join();
	}

	//------------------------------------------------------------------
	// (c) window rollover, via the injected fake clock.
	//------------------------------------------------------------------
	{
		std::unique_ptr<AgentSession> session = AgentSession::LoadFromFile( scenePath );
		Check( session != nullptr, "slice6(c): AgentSession::LoadFromFile loaded the temp scene" );
		AgentMcpAdapter adapter( std::move( session ), RISE::Agent::AgentAutonomy::Read );
		AgentLoopbackHttpServer server( &adapter, "/mcp" );
		Check( server.Bind( 0 ), "slice6(c): server Bind(0) succeeds" );
		const unsigned short port = server.BoundPort();
		const std::string token = server.ForTest_Token();

		g_fakeRateLimitClockMs.store( 0, std::memory_order_relaxed );
		server.ForTest_SetRateLimitClockFn( &FakeRateLimitClockMs );
		server.ForTest_ResetRateLimit();

		std::thread serverThread( [&]() { server.Serve(); } );

		ExtraHeaders extra;
		extra.hasAuthorization = true;
		extra.authorization = "Bearer " + token;
		JsonValue args = JsonValue::MakeObject();
		args.set( "target", JsonValue::MakeString( "sph" ) );
		args.set( "param",  JsonValue::MakeString( "radius" ) );
		args.set( "value",  JsonValue::MakeString( "0.9" ) );

		// Fill the window at fake-time 0.
		bool fillOk = true;
		for( int i = 0; i < AgentLoopbackHttpServer::kMutatingRateLimitMaxCalls; ++i ) {
			HttpResponse r = DoRequestEx( port, "POST", "/mcp",
				ReqToolCall( i + 1, "propose_patch", args ), extra );
			if( !r.ok || r.status != 200 ) { fillOk = false; break; }
		}
		Check( fillOk, "slice6(c): filled the window at fake-time 0" );

		// Still within the SAME window (fake time unchanged) -> refused.
		{
			HttpResponse r = DoRequestEx( port, "POST", "/mcp",
				ReqToolCall( 500, "propose_patch", args ), extra );
			JsonValue env; std::string perr;
			Check( r.ok && r.status == 200 && JsonParse( r.body, env, perr ) && env.has( "error" ),
			       "slice6(c): still within the SAME (unrolled) window, the next mutating call is refused" );
		}

		// Advance the fake clock past kMutatingRateLimitWindowMs -> the
		// window rolls over -> a mutating call succeeds again.
		g_fakeRateLimitClockMs.store(
			static_cast<std::int64_t>( AgentLoopbackHttpServer::kMutatingRateLimitWindowMs ),
			std::memory_order_relaxed );
		{
			HttpResponse r = DoRequestEx( port, "POST", "/mcp",
				ReqToolCall( 501, "propose_patch", args ), extra );
			JsonValue env; std::string perr;
			Check( r.ok && r.status == 200 && JsonParse( r.body, env, perr ) && !env.has( "error" ),
			       "slice6(c): AFTER the fake clock advances past kMutatingRateLimitWindowMs, a fresh window "
			       "starts and the SAME verb succeeds again (a top-level error here would mean the window "
			       "never rolled over)" );
		}

		server.Stop();
		if( serverThread.joinable() ) serverThread.join();
		// Restore the real clock for any server constructed after this
		// point in the process (defensive -- every OTHER section in this
		// file constructs its own fresh server instance, each defaulting
		// to the real clock, but this leaves no doubt).
		server.ForTest_SetRateLimitClockFn( nullptr );
	}

	//------------------------------------------------------------------
	// (d) total-request read deadline: slow-loris drip vs. normal traffic.
	//------------------------------------------------------------------
	{
		std::unique_ptr<AgentSession> session = AgentSession::LoadFromFile( scenePath );
		Check( session != nullptr, "slice6(d): AgentSession::LoadFromFile loaded the temp scene" );
		AgentMcpAdapter adapter( std::move( session ), RISE::Agent::AgentAutonomy::Read );
		AgentLoopbackHttpServer server( &adapter, "/mcp" );
		Check( server.Bind( 0 ), "slice6(d): server Bind(0) succeeds" );
		const unsigned short port = server.BoundPort();
		const std::string token = server.ForTest_Token();

		// Shrink the deadline for a fast test -- the PRODUCTION default
		// (kDefaultTotalRequestDeadlineMs, 15s) is exercised structurally
		// by this same code path; only the NUMBER is test-shrunk.
		const int kTestDeadlineMs = 300;
		server.ForTest_SetTotalRequestDeadlineMs( kTestDeadlineMs );

		std::thread serverThread( [&]() { server.Serve(); } );

		// (d.1) RED-PROVE: a drip whose individual gaps (20ms) are each
		// WELL inside the 5s per-recv socket timeout, but whose TOTAL time
		// (well past kTestDeadlineMs) is not -- cut off with 408.
		{
			const int s = ConnectLoopback( port, 5000 );
			Check( s >= 0, "slice6(d): connected for the slow-loris drip case" );
			if( s >= 0 ) {
				const std::string body = ReqToolCall( 1, "render", JsonValue::MakeObject() );
				const std::string req = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n"
				                         "Authorization: Bearer " + token + "\r\n"
				                         "Content-Length: " + std::to_string( body.size() ) + "\r\n\r\n" + body;

				// Split into ~24 pieces so 24 * 20ms = ~480ms total,
				// comfortably past the 300ms deadline while staying a fast
				// test; each individual gap (20ms) is nowhere near the 5s
				// per-recv timeout.
				// NOTE: once the deadline trips server-side, Serve() closes
				// this connection immediately after ServeOneConnection
				// returns -- this drip loop's LATER SendAll calls are
				// therefore EXPECTED to start failing (the peer is
				// already gone) partway through sending; that's the point
				// of this test, not a failure. Only the final response
				// status matters below.
				const std::size_t kPieces = 24;
				const std::size_t pieceLen = ( req.size() + kPieces - 1 ) / kPieces;
				for( std::size_t off = 0; off < req.size(); off += pieceLen ) {
					const std::size_t len = std::min( pieceLen, req.size() - off );
					if( !SendAll( s, req.substr( off, len ) ) ) break;   // server already closed -- expected once the deadline trips
					std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
				}
				HttpResponse r = ParseHttpResponse( ReadAllUntilClose( s ) );
				Check( r.ok && r.status == 408,
				       "RED-PROVE: a per-piece drip whose TOTAL time exceeds the (test-shrunk) total request "
				       "deadline is cut off with 408 Request Timeout, even though every individual gap (20ms) "
				       "is nowhere near the 5s per-recv socket timeout" );
				close( s );
			}
		}

		// (d.2) a NORMAL (non-dripped) request against the SAME short
		// deadline still succeeds -- the deadline doesn't
		// false-positive-reject ordinary traffic.
		{
			ExtraHeaders extra;
			extra.hasAuthorization = true;
			extra.authorization = "Bearer " + token;
			HttpResponse r = DoRequestEx( port, "POST", "/mcp", ReqInitialize( 2 ), extra );
			Check( r.ok && r.status == 200,
			       "slice6(d): a normal, non-dripped request still succeeds under the same short total deadline" );
		}

		server.Stop();
		if( serverThread.joinable() ) serverThread.join();
	}

	std::remove( scenePath.c_str() );
	std::printf( "=== Secure-MCP slice 6: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

int main()
{
	// Secure-MCP slice 6's total-request-deadline test (d) deliberately
	// drives a scenario where the SERVER closes a connection (408, on
	// deadline expiry) WHILE this same process's client-side drip loop is
	// still mid-send -- a subsequent send() on that now-peer-closed socket
	// raises SIGPIPE, whose default disposition is to terminate the whole
	// process. Every other test in this file only ever has the CLIENT
	// side close early (never write-after-peer-close), so this is new as
	// of that test; ignoring SIGPIPE process-wide (the standard idiom for
	// networking code -- send() then reports EPIPE via its return value,
	// which this file's SendAll() already checks) is the correct fix,
	// not a workaround local to that one test.
	std::signal( SIGPIPE, SIG_IGN );

	std::printf( "=== AgentLoopbackHttpTest (Secure-MCP slice 3: loopback HTTP transport) ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_http_test.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the smoke scene to a temp file" );

	std::unique_ptr<AgentSession> session = AgentSession::LoadFromFile( scenePath );
	Check( session != nullptr, "AgentSession::LoadFromFile loaded the temp scene" );

	AgentMcpAdapter adapter( std::move( session ), RISE::Agent::AgentAutonomy::Read );
	AgentLoopbackHttpServer server( &adapter, "/mcp" );

	Check( server.Bind( 0 ), "Bind(0) succeeded (OS-assigned ephemeral loopback port)" );
	const unsigned short port = server.BoundPort();
	Check( port != 0, "BoundPort() reports a nonzero ephemeral port after Bind(0)" );
	Check( server.IsBound(), "IsBound() is true after a successful Bind()" );

	// Secure-MCP slice 4: read this server's per-launch bearer token via the
	// ForTest_ accessor so every DoRequest/DoRequestEx call in the rest of
	// this test can present valid auth -- a request with no Authorization
	// header (or the wrong one) is now unconditionally 401 before it ever
	// reaches any of the HTTP-correctness/smuggling/fuzz cases below.
	g_testToken = server.ForTest_Token();
	Check( g_testToken.size() == 64, "the per-launch bearer token is 64 hex chars (32 bytes = 256 bits)" );

	// Serve() is the serial accept-handle LOOP -- it must run on its own
	// thread for this test to act as a client against it. This is the
	// realistic production shape too (RunAgentHttp calls Serve() after
	// Bind() on whatever thread invoked it; a caller wanting the process
	// to keep doing other things would put it on its own thread the same
	// way).
	std::thread serverThread( [&]() { server.Serve(); } );

	//------------------------------------------------------------------
	// (a) bind is loopback.
	//------------------------------------------------------------------
	std::printf( "[bind] loopback-only\n" );
	{
		// (1) Functional: connecting to 127.0.0.1:port succeeds.
		const int s = ConnectLoopback( port );
		Check( s >= 0, "connect() to 127.0.0.1:port succeeds" );
		if( s >= 0 ) close( s );
	}
	{
		// (2) STRUCTURAL, environment-independent proof of the bind
		// target: AgentLoopbackHttpServer::BoundAddress() reads back the
		// LISTEN SOCKET's own bound address via getsockname() inside
		// Bind() itself (AgentLoopbackHttpServer.cpp) -- not re-derived
		// from a client connect, which would trivially read back
		// "127.0.0.1" regardless of what the server actually bound,
		// since the test dialed that literal itself. A live attempt to
		// connect to the machine's LAN IP was deliberately NOT used here
		// (CI sandboxes and machines with no configured non-loopback
		// interface would give a false pass/fail for the wrong reason);
		// this check is the direct, sandboxable equivalent.
		//
		// RED-PROVE: temporarily change Bind()'s sockaddr_in construction
		// in AgentLoopbackHttpServer.cpp from
		// `inet_pton(AF_INET,"127.0.0.1",&addr.sin_addr)` to
		// `addr.sin_addr.s_addr = htonl(INADDR_ANY)` and rebuild -- this
		// assertion flips from "127.0.0.1" to "0.0.0.0" and fails, which
		// is exactly the class of regression this guards (binding all
		// interfaces instead of loopback-only on a no-auth-yet control
		// endpoint).
		Check( server.BoundAddress() == "127.0.0.1",
		       "the listen socket's OWN bound address (via getsockname in Bind()) is exactly \"127.0.0.1\", "
		       "never \"0.0.0.0\"/INADDR_ANY" );
	}

	//------------------------------------------------------------------
	// (b) full round-trip.
	//------------------------------------------------------------------
	std::printf( "[round-trip] initialize / tools/call render / tools/call read_image\n" );
	{
		HttpResponse r = DoRequest( port, "POST", "/mcp", ReqInitialize( 1 ) );
		Check( r.ok, "initialize: got a parseable HTTP response" );
		Check( r.status == 200, "initialize: HTTP 200" );
		JsonValue env; std::string perr;
		Check( JsonParse( r.body, env, perr ), "initialize: body parses as JSON" );
		Check( env.get( "id" ).asNumber( -999 ) == 1.0, "initialize: id echoes 1" );
		Check( !env.has( "error" ), "initialize: not a JSON-RPC error" );
		Check( env.get( "result" ).get( "serverInfo" ).get( "name" ).asString() == "rise",
		       "initialize: serverInfo.name == \"rise\"" );
	}
	{
		HttpResponse r = DoRequest( port, "POST", "/mcp",
			ReqToolCall( 2, "render", JsonValue::MakeObject() ) );
		Check( r.ok && r.status == 200, "tools/call(render): HTTP 200" );
		JsonValue env; std::string perr;
		Check( JsonParse( r.body, env, perr ), "tools/call(render): body parses as JSON" );
		Check( !env.has( "error" ), "tools/call(render): not a JSON-RPC error" );
		const JsonValue& result = env.get( "result" );
		Check( !result.get( "isError" ).asBool( true ), "tools/call(render): isError == false" );
	}
	{
		HttpResponse r = DoRequest( port, "POST", "/mcp",
			ReqToolCall( 3, "read_image", JsonValue::MakeObject() ) );
		Check( r.ok && r.status == 200, "tools/call(read_image): HTTP 200" );
		JsonValue env; std::string perr;
		Check( JsonParse( r.body, env, perr ), "tools/call(read_image): body parses as JSON" );
		const JsonValue& result = env.get( "result" );
		Check( !result.get( "isError" ).asBool( true ), "tools/call(read_image): isError == false" );
		const JsonValue& content = result.get( "content" );
		bool foundImageBlock = false;
		for( std::size_t i = 0; i < content.size(); ++i ) {
			const JsonValue& block = content.at( i );
			if( block.get( "type" ).asString() == "image" ) {
				foundImageBlock = true;
				const std::string b64 = block.get( "data" ).asString();
				std::vector<unsigned char> png;
				Check( Base64Decode( b64, png ), "read_image: base64 decodes cleanly" );
				Check( png.size() >= 8 &&
				       png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G' &&
				       png[4] == 0x0D && png[5] == 0x0A && png[6] == 0x1A && png[7] == 0x0A,
				       "read_image: base64 decodes to a valid PNG signature" );
			}
		}
		Check( foundImageBlock, "read_image: response includes an {type:\"image\"} content block" );
	}

	//------------------------------------------------------------------
	// (c) HTTP correctness.
	//------------------------------------------------------------------
	std::printf( "[http] method/path/Content-Length correctness\n" );
	{
		HttpResponse r = DoRequest( port, "GET", "/mcp", "" );
		Check( r.ok && r.status == 405, "GET /mcp -> 405 Method Not Allowed" );
	}
	{
		HttpResponse r = DoRequest( port, "POST", "/wrongpath", "{}" );
		Check( r.ok && r.status == 404, "POST /wrongpath -> 404 Not Found" );
	}
	{
		// Missing Content-Length: send a raw request with no such header.
		const int s = ConnectLoopback( port );
		Check( s >= 0, "connect for missing-Content-Length case" );
		if( s >= 0 ) {
			const std::string raw = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n"
			                         "Authorization: Bearer " + g_testToken + "\r\n\r\n";
			Check( SendAll( s, raw ), "sent a request with no Content-Length header" );
			HttpResponse r = ParseHttpResponse( ReadAllUntilClose( s ) );
			Check( r.ok && r.status == 400, "missing Content-Length -> clean 400 (not a hang/crash)" );
			close( s );
		}
	}
	{
		// Content-Length far over the server's cap -- RED-PROVE target:
		// with AgentLoopbackHttpServer::kMaxBodyBytes's check removed (or
		// the comparison inverted), this request would make the server
		// attempt to read/allocate toward the declared length instead of
		// rejecting immediately -- this test's own bounded socket timeout
		// turns that failure mode into a prompt, diagnosable test FAILURE
		// rather than a wedged suite.
		const int s = ConnectLoopback( port );
		Check( s >= 0, "connect for oversized-Content-Length case" );
		if( s >= 0 ) {
			const std::string raw = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n"
			                         "Authorization: Bearer " + g_testToken + "\r\n"
			                         "Content-Length: 999999999999\r\n\r\n";
			Check( SendAll( s, raw ), "sent a request declaring an oversized Content-Length" );
			HttpResponse r = ParseHttpResponse( ReadAllUntilClose( s ) );
			Check( r.ok && r.status == 413, "oversized Content-Length -> 413 Payload Too Large, BEFORE any body read" );
			close( s );
		}
	}
	{
		// Truncated body: declare more bytes than we actually send, then
		// close our write side (half-close) so the server observes EOF
		// mid-body rather than hanging on a read that will never
		// complete. Bounded by the server's own per-connection recv
		// timeout either way.
		const int s = ConnectLoopback( port );
		Check( s >= 0, "connect for truncated-body case" );
		if( s >= 0 ) {
			const std::string raw = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n"
			                         "Authorization: Bearer " + g_testToken + "\r\n"
			                         "Content-Length: 100\r\n\r\n{\"short\":true}";
			Check( SendAll( s, raw ), "sent a request whose body is shorter than its declared Content-Length" );
			shutdown( s, SHUT_WR );   // half-close: no more bytes are coming
			HttpResponse r = ParseHttpResponse( ReadAllUntilClose( s ) );
			Check( r.ok && r.status == 400, "truncated body -> clean 400, no hang" );
			close( s );
		}
	}

	//------------------------------------------------------------------
	// (g) request-smuggling preconditions (review P2, RFC 7230 SS3.3.3).
	//------------------------------------------------------------------
	std::printf( "[smuggling] duplicate Content-Length / Transfer-Encoding are rejected\n" );
	{
		// Two Content-Length headers, even numerically identical ones --
		// MUST be rejected outright, never "last one wins".
		// RED-PROVE: reverting the contentLengthCount > 1 check in
		// ServeOneConnection makes this request succeed with 200 (the
		// second Content-Length silently overwrites the first).
		const int s = ConnectLoopback( port );
		Check( s >= 0, "connect for duplicate-Content-Length case" );
		if( s >= 0 ) {
			const std::string raw = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n"
			                         "Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}";
			Check( SendAll( s, raw ), "sent a request with two Content-Length headers" );
			HttpResponse r = ParseHttpResponse( ReadAllUntilClose( s ) );
			Check( r.ok && r.status == 400, "duplicate Content-Length -> clean 400" );
			close( s );
		}
	}
	{
		// A Transfer-Encoding header at all -- this server never speaks
		// chunked, so its mere presence is rejected rather than silently
		// ignored (an ignored Transfer-Encoding is the exact ambiguity a
		// request-smuggling attack against a future front proxy would
		// exploit). RED-PROVE: reverting the hasTransferEncoding check
		// makes this request succeed with 200 (the header is parsed for
		// nothing, and the body is still read strictly by Content-Length).
		const int s = ConnectLoopback( port );
		Check( s >= 0, "connect for Transfer-Encoding case" );
		if( s >= 0 ) {
			const std::string raw = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n"
			                         "Content-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n{}";
			Check( SendAll( s, raw ), "sent a request bearing a Transfer-Encoding header" );
			HttpResponse r = ParseHttpResponse( ReadAllUntilClose( s ) );
			Check( r.ok && r.status == 400, "Transfer-Encoding present -> clean 400" );
			close( s );
		}
	}

	//------------------------------------------------------------------
	// (h) oversized header block (review P3-2): the header cap is now a
	// dedicated kMaxHeaderBytes (64 KiB), independent of kMaxBodyBytes
	// (8 MiB).
	//------------------------------------------------------------------
	std::printf( "[header-cap] an oversized header block is rejected promptly\n" );
	{
		// 128 KB of header-shaped bytes (well over kMaxHeaderBytes, well
		// under kMaxBodyBytes) with NO terminating blank line.
		//
		// RED-PROVE: before the fix, ReadUntilDoubleCrlf's cap here was
		// kMaxBodyBytes (8 MiB), so 128 KB was still UNDER that cap and the
		// read loop kept accumulating -- it only ever stopped via the
		// server's 5s-per-recv socket timeout, which happens to ALSO map to
		// a 431 response (ReadUntilDoubleCrlf returns false for "cap
		// exceeded" and "timed out" alike), so a bare status-code check
		// does not distinguish "rejected by the cap" from "rejected by an
		// unrelated timeout" -- confirmed empirically: with the old 8 MiB
		// cap this same request still returns 431, just ~5s later. The
		// WALL-CLOCK bound below is what actually red-proves the fix: a
		// cap-triggered rejection returns in well under a second (no
		// recv() call even blocks -- the loop's size check fires between
		// reads), while a timeout-triggered "431" takes ~5s.
		const auto reqStart = std::chrono::steady_clock::now();
		const int s = ConnectLoopback( port, 8000 );
		Check( s >= 0, "connect for oversized-header-block case" );
		if( s >= 0 ) {
			std::string raw = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n";
			// Pad with a single oversized (but well-formed-looking) header
			// line comfortably past kMaxHeaderBytes (64 KiB).
			raw += "X-Padding: " + std::string( 128 * 1024, 'a' ) + "\r\n";
			Check( SendAll( s, raw ), "sent a request with a 128KB header block" );
			HttpResponse r = ParseHttpResponse( ReadAllUntilClose( s ) );
			const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - reqStart ).count();
			Check( r.ok && r.status == 431, "128KB header block -> 431 Request Header Fields Too Large" );
			Check( elapsedMs < 2000,
			       "128KB header block rejected in well under the 5s recv timeout (i.e. by the SIZE cap, "
			       "not by timing out -- the real red-prove for this fix)" );
			close( s );
		}
	}
	{
		// A healthy request immediately after both new rejection classes
		// proves neither wedged or crashed the server.
		HttpResponse r = DoRequest( port, "POST", "/mcp", ReqInitialize( 98 ) );
		Check( r.ok && r.status == 200, "server still answers a well-formed request after the smuggling/header-cap cases" );
	}

	//------------------------------------------------------------------
	// (e) malformed-input fuzz (folded in here, before the concurrency
	// case, so a crash from any of these is attributed precisely).
	//------------------------------------------------------------------
	std::printf( "[fuzz] malformed input never hangs or crashes the server\n" );
	{
		// Truncated request line (no HTTP version token at all).
		const int s = ConnectLoopback( port );
		if( s >= 0 ) {
			SendAll( s, "POST /mcp\r\n\r\n" );
			HttpResponse r = ParseHttpResponse( ReadAllUntilClose( s ) );
			Check( r.ok && r.status == 400, "truncated request line -> clean 400" );
			close( s );
		}
	}
	{
		// Headers with no CRLFCRLF terminator at all -- bounded by the
		// server's recv timeout (kSocketTimeoutMs, 5s) and this test's
		// own client timeout; the connection is expected to close/drop
		// rather than the read blocking forever.
		const int s = ConnectLoopback( port, 8000 );
		if( s >= 0 ) {
			SendAll( s, "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n" );   // no terminator
			shutdown( s, SHUT_WR );
			const std::string raw = ReadAllUntilClose( s );
			// Either a clean error response, or the server simply closed
			// with nothing to say (both are "did not hang, did not
			// crash" -- the server process being alive afterward, proven
			// by the next healthy request below, is the real assertion).
			Check( true, "unterminated headers did not hang the test (bounded read returned)" );
			(void)raw;
			close( s );
		}
	}
	{
		// Non-numeric Content-Length.
		const int s = ConnectLoopback( port );
		if( s >= 0 ) {
			SendAll( s, "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + g_testToken + "\r\nContent-Length: notanumber\r\n\r\n" );
			HttpResponse r = ParseHttpResponse( ReadAllUntilClose( s ) );
			Check( r.ok && r.status == 400, "non-numeric Content-Length -> clean 400" );
			close( s );
		}
	}
	{
		// Negative-looking Content-Length (a leading '-' is not a digit
		// under the server's strict all-digits check, so this is
		// rejected the same way as any other non-numeric value).
		const int s = ConnectLoopback( port );
		if( s >= 0 ) {
			SendAll( s, "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + g_testToken + "\r\nContent-Length: -5\r\n\r\n" );
			HttpResponse r = ParseHttpResponse( ReadAllUntilClose( s ) );
			Check( r.ok && r.status == 400, "negative-looking Content-Length -> clean 400" );
			close( s );
		}
	}
	{
		// Embedded NULs in the body -- must not truncate/crash the JSON
		// layer; expect a clean JSON-RPC parse-error envelope (still HTTP
		// 200 -- the HTTP transport succeeded, the JSON-RPC payload
		// inside it was malformed, exactly like the stdio transport's own
		// -32700 mapping).
		const int s = ConnectLoopback( port );
		if( s >= 0 ) {
			std::string body = std::string( "{\"a\":\"" ) + '\0' + "\"}";
			std::string raw = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + g_testToken +
			                   "\r\nContent-Length: " +
			                   std::to_string( body.size() ) + "\r\n\r\n" + body;
			// SendAll on a std::string with an embedded NUL is fine --
			// std::string tracks length explicitly, unlike a C string.
			bool sent = SendAll( s, raw );
			Check( sent, "sent a body containing an embedded NUL byte" );
			HttpResponse r = ParseHttpResponse( ReadAllUntilClose( s ) );
			Check( r.ok && r.status == 200, "embedded-NUL body -> still a well-formed HTTP response (200)" );
			close( s );
		}
	}
	{
		// A healthy request AFTER the fuzz batch proves none of the
		// malformed inputs above wedged or crashed the server.
		HttpResponse r = DoRequest( port, "POST", "/mcp", ReqInitialize( 99 ) );
		Check( r.ok && r.status == 200, "server still answers a well-formed request after the fuzz batch" );
	}

	//------------------------------------------------------------------
	// (j) [SLICE 4] bearer-token auth.
	//------------------------------------------------------------------
	std::printf( "[auth] bearer token: absent -> 401, wrong -> 401, correct -> 200\n" );
	{
		// No Authorization header at all.
		// RED-PROVE: with the auth-check block removed from
		// ServeOneConnection (AgentLoopbackHttpServer.cpp), this exact
		// request reaches the dispatcher and returns 200 instead.
		ExtraHeaders extra;   // hasAuthorization defaults to false
		HttpResponse r = DoRequestEx( port, "POST", "/mcp", ReqInitialize( 200 ), extra );
		Check( r.ok && r.status == 401, "no Authorization header -> 401 Unauthorized" );
	}
	{
		// Wrong token -- same length as the real one (so this exercises
		// ConstantTimeEquals's byte-loop, not just its length-mismatch
		// fold), differing at the FIRST byte.
		std::string wrong = g_testToken;
		wrong[0] = ( wrong[0] == '0' ) ? '1' : '0';
		ExtraHeaders extra;
		extra.hasAuthorization = true;
		extra.authorization = "Bearer " + wrong;
		HttpResponse r = DoRequestEx( port, "POST", "/mcp", ReqInitialize( 201 ), extra );
		Check( r.ok && r.status == 401, "wrong token (differs at first byte) -> 401 Unauthorized" );
	}
	{
		// Wrong token differing only at the LAST byte -- a byte-wise
		// early-return compare would still catch this correctly (it's the
		// LAST byte that differs, so a naive compare walks the whole
		// string anyway), but this is the complementary case to "differs
		// at the first byte" above for exercising the full comparison
		// range.
		std::string wrong = g_testToken;
		wrong.back() = ( wrong.back() == '0' ) ? '1' : '0';
		ExtraHeaders extra;
		extra.hasAuthorization = true;
		extra.authorization = "Bearer " + wrong;
		HttpResponse r = DoRequestEx( port, "POST", "/mcp", ReqInitialize( 202 ), extra );
		Check( r.ok && r.status == 401, "wrong token (differs at last byte) -> 401 Unauthorized" );
	}
	{
		// Wrong-length token (shorter than the real one).
		ExtraHeaders extra;
		extra.hasAuthorization = true;
		extra.authorization = "Bearer " + g_testToken.substr( 0, g_testToken.size() - 1 );
		HttpResponse r = DoRequestEx( port, "POST", "/mcp", ReqInitialize( 203 ), extra );
		Check( r.ok && r.status == 401, "wrong-length (truncated) token -> 401 Unauthorized" );
	}
	{
		// Correct token, wrong scheme case ("bearer" lowercase) -- RFC 7235
		// SS2.1 says the scheme name is matched case-insensitively.
		ExtraHeaders extra;
		extra.hasAuthorization = true;
		extra.authorization = "bearer " + g_testToken;
		HttpResponse r = DoRequestEx( port, "POST", "/mcp", ReqInitialize( 204 ), extra );
		Check( r.ok && r.status == 200, "lowercase \"bearer\" scheme with the correct token -> 200 (scheme is case-insensitive)" );
	}
	{
		// Correct token -> 200 with a valid MCP response, AND the
		// WWW-Authenticate header is present on the 401 responses above --
		// checked separately via a raw connect since HttpResponse doesn't
		// parse arbitrary headers.
		HttpResponse r = DoRequest( port, "POST", "/mcp", ReqInitialize( 205 ) );
		Check( r.ok && r.status == 200, "correct token -> 200" );
		JsonValue env; std::string perr;
		Check( JsonParse( r.body, env, perr ), "correct token: body parses as JSON" );
		Check( env.get( "id" ).asNumber( -999 ) == 205.0, "correct token: id echoes 205" );
	}
	{
		// WWW-Authenticate: Bearer must accompany a 401, per RFC 7235 SS4.1.
		const int s = ConnectLoopback( port );
		Check( s >= 0, "connect for WWW-Authenticate case" );
		if( s >= 0 ) {
			const std::string raw = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 2\r\n\r\n{}";
			Check( SendAll( s, raw ), "sent a request with no Authorization header" );
			const std::string respRaw = ReadAllUntilClose( s );
			HttpResponse r = ParseHttpResponse( respRaw );
			Check( r.ok && r.status == 401, "no-auth request -> 401 (WWW-Authenticate check)" );
			Check( respRaw.find( "WWW-Authenticate: Bearer" ) != std::string::npos,
			       "401 response carries a WWW-Authenticate: Bearer header" );
			Check( respRaw.find( g_testToken ) == std::string::npos,
			       "401 response NEVER echoes the expected token anywhere in the raw HTTP response" );
			close( s );
		}
	}

	//------------------------------------------------------------------
	// (k) [SLICE 4] Origin validation -- independent of auth.
	//------------------------------------------------------------------
	std::printf( "[origin] Origin present and not loopback -> 403, even with a correct token\n" );
	{
		// RED-PROVE: with the Origin-check block removed from
		// ServeOneConnection, this exact request (correct token AND all)
		// reaches the dispatcher and returns 200 instead.
		ExtraHeaders extra;
		extra.hasAuthorization = true;
		extra.authorization = "Bearer " + g_testToken;
		extra.hasOrigin = true;
		extra.origin = "http://evil.com";
		HttpResponse r = DoRequestEx( port, "POST", "/mcp", ReqInitialize( 210 ), extra );
		Check( r.ok && r.status == 403,
		       "Origin: http://evil.com present WITH a correct bearer token -> 403 Forbidden "
		       "(proves the Origin check is not subsumed by auth)" );
	}
	{
		// An explicit loopback Origin is accepted.
		ExtraHeaders extra;
		extra.hasAuthorization = true;
		extra.authorization = "Bearer " + g_testToken;
		extra.hasOrigin = true;
		extra.origin = "http://127.0.0.1:" + std::to_string( port );
		HttpResponse r = DoRequestEx( port, "POST", "/mcp", ReqInitialize( 211 ), extra );
		Check( r.ok && r.status == 200, "an explicit loopback Origin (http://127.0.0.1:<port>) is accepted" );
	}
	{
		// No Origin header at all (the non-browser MCP client case) is
		// accepted -- this is exactly what DoRequest() already sends for
		// every other case in this file, so this is really re-asserting
		// the baseline the rest of the suite depends on.
		HttpResponse r = DoRequest( port, "POST", "/mcp", ReqInitialize( 212 ) );
		Check( r.ok && r.status == 200, "no Origin header at all is accepted (non-browser MCP client)" );
	}

	//------------------------------------------------------------------
	// (l) [SLICE 4] Host validation (anti-DNS-rebinding).
	//------------------------------------------------------------------
	std::printf( "[host] Host header naming a non-loopback domain -> 403 (DNS-rebinding defense)\n" );
	{
		// RED-PROVE: with the Host-check block removed from
		// ServeOneConnection, this exact request (correct token, no Origin
		// header at all -- exactly the shape a same-origin XHR after a DNS
		// rebind would have, since Origin reflects the ATTACKER page's
		// origin which the attacker controls but Host reflects the
		// resolved-via-rebind hostname) reaches the dispatcher and returns
		// 200 instead.
		ExtraHeaders extra;
		extra.host = "evil.com";
		extra.hasAuthorization = true;
		extra.authorization = "Bearer " + g_testToken;
		HttpResponse r = DoRequestEx( port, "POST", "/mcp", ReqInitialize( 220 ), extra );
		Check( r.ok && r.status == 403, "Host: evil.com (DNS-rebind shape) -> 403 Forbidden" );
	}
	{
		// Host with an explicit port is accepted.
		ExtraHeaders extra;
		extra.host = "127.0.0.1:" + std::to_string( port );
		extra.hasAuthorization = true;
		extra.authorization = "Bearer " + g_testToken;
		HttpResponse r = DoRequestEx( port, "POST", "/mcp", ReqInitialize( 221 ), extra );
		Check( r.ok && r.status == 200, "Host: 127.0.0.1:<port> is accepted" );
	}
	{
		// Host: localhost (no port) is accepted.
		ExtraHeaders extra;
		extra.host = "localhost";
		extra.hasAuthorization = true;
		extra.authorization = "Bearer " + g_testToken;
		HttpResponse r = DoRequestEx( port, "POST", "/mcp", ReqInitialize( 222 ), extra );
		Check( r.ok && r.status == 200, "Host: localhost is accepted" );
	}

	//------------------------------------------------------------------
	// (o) [SLICE 4 HARDENING] IsLoopbackHost case-insensitivity + the
	// suffix-reject regression guard (P2/P3-2).
	//------------------------------------------------------------------
	std::printf( "[host-case] Host header naming loopback in mixed/upper case is accepted\n" );
	{
		// RED-PROVE: before the EqualsIgnoreCase fix, `hostOnly == "localhost"`
		// was a case-SENSITIVE compare, so this 403'd.
		ExtraHeaders extra;
		extra.host = "Localhost";
		extra.hasAuthorization = true;
		extra.authorization = "Bearer " + g_testToken;
		HttpResponse r = DoRequestEx( port, "POST", "/mcp", ReqInitialize( 230 ), extra );
		Check( r.ok && r.status == 200, "Host: Localhost (mixed case) is accepted" );
	}
	{
		ExtraHeaders extra;
		extra.host = "LOCALHOST";
		extra.hasAuthorization = true;
		extra.authorization = "Bearer " + g_testToken;
		HttpResponse r = DoRequestEx( port, "POST", "/mcp", ReqInitialize( 231 ), extra );
		Check( r.ok && r.status == 200, "Host: LOCALHOST (all caps) is accepted" );
	}
	{
		// Regression guard: the case-insensitivity fix must NOT loosen the
		// match into a suffix/prefix check -- a hostile domain that merely
		// CONTAINS "127.0.0.1" as a substring/prefix is still rejected.
		ExtraHeaders extra;
		extra.host = "127.0.0.1.evil.com";
		extra.hasAuthorization = true;
		extra.authorization = "Bearer " + g_testToken;
		HttpResponse r = DoRequestEx( port, "POST", "/mcp", ReqInitialize( 232 ), extra );
		Check( r.ok && r.status == 403,
		       "Host: 127.0.0.1.evil.com is still rejected (no suffix-match regression)" );
	}
	std::printf( "[origin-case] Origin header naming loopback in upper case is accepted\n" );
	{
		// The Origin path delegates host-parsing to the same IsLoopbackHost,
		// so the case-insensitivity fix must flow through here too.
		ExtraHeaders extra;
		extra.hasAuthorization = true;
		extra.authorization = "Bearer " + g_testToken;
		extra.hasOrigin = true;
		extra.origin = "http://LOCALHOST:" + std::to_string( port );
		HttpResponse r = DoRequestEx( port, "POST", "/mcp", ReqInitialize( 233 ), extra );
		Check( r.ok && r.status == 200, "Origin: http://LOCALHOST:<port> (upper case) is accepted" );
	}

	//------------------------------------------------------------------
	// (q) [SECURE-MCP SLICE 4 FOLLOW-UP, P3] IsLoopbackHost rejects
	// trailing garbage after the host[:port] label -- both the bracketed
	// IPv6 form and the bare form. Pre-fix, IsLoopbackHost truncated at
	// the closing `]` or the first colon and never checked what (if
	// anything) followed, so e.g. "[::1]evil" and "127.0.0.1:notaport"
	// both matched the loopback label and returned 200. This is a
	// robustness/correctness fix on an already fail-closed path (the
	// Host LABEL itself still has to exact-match; a real DNS-rebind
	// attacker sends `Host: evil.com`, never `Host: 127.0.0.1evil.com`),
	// not a security bypass.
	//------------------------------------------------------------------
	std::printf( "[host-malformed] IsLoopbackHost rejects trailing garbage after host[:port]\n" );
	{
		// RED-PROVE (documented, not re-executed here): against the
		// pre-fix IsLoopbackHost, every "malformed rejected" case below
		// returned 200 instead of 403 -- confirmed by hand-reverting the
		// bracket/port tightening locally and re-running this exact block
		// (all 9 flip to 200), then restoring the fix. See the commit
		// message / session notes for the reverted-code transcript.
		struct MalformedCase { const char* host; const char* label; };
		const MalformedCase kRejected[] = {
			{ "[::1]evil",                    "[::1]evil (trailing garbage after bracket)" },
			{ "[::1]extra",                   "[::1]extra (trailing garbage after bracket)" },
			{ "[::1]:notaport",               "[::1]:notaport (non-numeric port)" },
			{ "[::1]:8765extra",              "[::1]:8765extra (garbage after numeric port)" },
			{ "[::1]]",                       "[::1]] (extra closing bracket)" },
			{ "127.0.0.1:notaport",           "127.0.0.1:notaport (non-numeric port)" },
			{ "127.0.0.1:",                   "127.0.0.1: (empty port)" },
			{ "127.0.0.1:8765evil.com",       "127.0.0.1:8765evil.com (garbage after numeric port)" },
			{ "127.0.0.1:PORT.evil.com",      "127.0.0.1:PORT.evil.com (non-numeric, dotted trailer)" },
			{ "[::1]:",                       "[::1]: (empty port)" },
		};
		int reqId = 240;
		for( const auto& c : kRejected ) {
			ExtraHeaders extra;
			extra.host = c.host;
			extra.hasAuthorization = true;
			extra.authorization = "Bearer " + g_testToken;
			HttpResponse r = DoRequestEx( port, "POST", "/mcp", ReqInitialize( reqId++ ), extra );
			Check( r.ok && r.status == 403, std::string( "Host: " ) + c.label + " -> 403 Forbidden" );
		}
	}
	std::printf( "[host-malformed-crash] previously-fuzzed unclosed/degenerate bracket forms stay fail-closed, no crash\n" );
	{
		// These already 403'd pre-fix (the closing-bracket-not-found
		// early return) -- re-asserted here so the tightened parse
		// doesn't regress the crash-fuzz property the 5c review checked.
		const char* kCrashFuzz[] = { "[", "[]", "[::1", "]evil" };
		int reqId = 250;
		for( const char* h : kCrashFuzz ) {
			ExtraHeaders extra;
			extra.host = h;
			extra.hasAuthorization = true;
			extra.authorization = "Bearer " + g_testToken;
			HttpResponse r = DoRequestEx( port, "POST", "/mcp", ReqInitialize( reqId++ ), extra );
			Check( r.ok && r.status == 403,
			       std::string( "Host: \"" ) + h + "\" (crash-fuzz form) -> 403 Forbidden, no crash" );
		}
	}
	std::printf( "[host-valid-regression] well-formed loopback Host/Origin values still accepted\n" );
	{
		// Regression guard for the tightened parse: every previously-valid
		// shape (bare, ported, case-varied, and now also the bracketed
		// IPv6 forms actually reaching the dispatcher) must still return
		// 200.
		struct ValidCase { std::string host; const char* label; };
		const std::vector<ValidCase> kValid = {
			{ "127.0.0.1",                          "127.0.0.1" },
			{ "127.0.0.1:" + std::to_string( port ), "127.0.0.1:<port>" },
			{ "localhost",                           "localhost" },
			{ "localhost:" + std::to_string( port ), "localhost:<port>" },
			{ "Localhost",                           "Localhost (mixed case)" },
			{ "[::1]",                               "[::1]" },
			{ "[::1]:" + std::to_string( port ),     "[::1]:<port>" },
		};
		int reqId = 260;
		for( const auto& c : kValid ) {
			ExtraHeaders extra;
			extra.host = c.host;
			extra.hasAuthorization = true;
			extra.authorization = "Bearer " + g_testToken;
			HttpResponse r = DoRequestEx( port, "POST", "/mcp", ReqInitialize( reqId++ ), extra );
			Check( r.ok && r.status == 200, std::string( "Host: " ) + c.label + " is accepted" );
		}
	}

	//------------------------------------------------------------------
	// (p) [SLICE 4 HARDENING] ConstantTimeEquals length-fold (P3-1) --
	// direct unit check via the ForTest_ConstantTimeEquals() seam.
	//------------------------------------------------------------------
	std::printf( "[constant-time] length fold does not alias at a 65536-multiple stride\n" );
	{
		// RED-PROVE: the pre-fix fold packed only the low 16 bits of
		// (lenA ^ lenB) into `diff`; 65600 - 64 == 65536, whose low 16 bits
		// are all zero, so the pre-fix version returned true (wrongly
		// "equal") for this exact pair. The fixed `lenA != lenB` fold
		// returns false for ANY differing length, this stride included.
		const std::string shortStr( 64, 'a' );
		const std::string longStr( 65600, 'a' );
		Check( !AgentLoopbackHttpServer::ForTest_ConstantTimeEquals( shortStr, longStr ),
		       "ConstantTimeEquals(64 bytes, 65600 bytes) -> false (65536-multiple length-fold fix)" );
		Check( !AgentLoopbackHttpServer::ForTest_ConstantTimeEquals( longStr, shortStr ),
		       "ConstantTimeEquals(65600 bytes, 64 bytes) -> false (argument order doesn't matter)" );
		// Sanity: equal-length equal-content still compares true, and
		// equal-length differing-content still compares false -- the fold
		// fix must not have broken the ordinary cases.
		Check( AgentLoopbackHttpServer::ForTest_ConstantTimeEquals( shortStr, shortStr ),
		       "ConstantTimeEquals(same string, same string) -> true" );
		std::string shortStrDiff = shortStr;
		shortStrDiff[0] = 'b';
		Check( !AgentLoopbackHttpServer::ForTest_ConstantTimeEquals( shortStr, shortStrDiff ),
		       "ConstantTimeEquals(same length, differs at byte 0) -> false" );
	}

	//------------------------------------------------------------------
	// (d) serialization: N concurrent client connections.
	//------------------------------------------------------------------
	std::printf( "[serialization] N concurrent connections, no interleaving, no reentrancy abort\n" );
	{
		const int kN = 8;
		std::vector<std::thread> clients;
		std::vector<HttpResponse> results( kN );
		for( int i = 0; i < kN; ++i ) {
			clients.emplace_back( [&, i]() {
				results[i] = DoRequest( port, "POST", "/mcp",
					ReqToolCall( 1000 + i, "read_document", JsonValue::MakeObject() ) );
			} );
		}
		for( auto& t : clients ) t.join();

		bool allOk = true;
		for( int i = 0; i < kN; ++i ) {
			if( !results[i].ok || results[i].status != 200 ) { allOk = false; continue; }
			JsonValue env; std::string perr;
			if( !JsonParse( results[i].body, env, perr ) ) { allOk = false; continue; }
			if( env.get( "id" ).asNumber( -999 ) != static_cast<double>( 1000 + i ) ) { allOk = false; }
			if( env.has( "error" ) ) { allOk = false; }
		}
		Check( allOk, "all 8 concurrent connections got correct, non-interleaved, matching-id responses" );
		// If the reentrancy guard had tripped, AgentLoopbackHttpServer
		// would have called std::abort() on the server's own thread,
		// taking this whole test PROCESS down mid-run -- so simply
		// reaching this Check() at all is part of the proof the guard
		// stayed armed-but-untripped throughout the concurrent batch.
		Check( true, "process is still alive after N concurrent connections (reentrancy guard never tripped)" );
	}

	//------------------------------------------------------------------
	// (f) clean shutdown.
	//------------------------------------------------------------------
	std::printf( "[shutdown] Stop() + Serve()-thread join completes promptly\n" );
	{
		const auto stopStart = std::chrono::steady_clock::now();
		server.Stop();
		Check( !server.IsBound(), "IsBound() is false immediately after Stop()" );

		// join() with a bounded wait: spin up a watchdog that would let
		// the test fail loudly (rather than hang the whole suite runner)
		// if Serve() never returns after Stop() closes the listen socket.
		std::thread watchdog;
		std::atomic<bool> joined( false );
		std::thread joiner( [&]() { serverThread.join(); joined.store( true, std::memory_order_release ); } );
		const auto deadline = stopStart + std::chrono::seconds( 5 );
		while( !joined.load( std::memory_order_acquire ) && std::chrono::steady_clock::now() < deadline ) {
			std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
		}
		Check( joined.load( std::memory_order_acquire ), "Serve() returned (and joined) within 5s of Stop()" );
		if( joiner.joinable() ) joiner.join();
	}

	//------------------------------------------------------------------
	// (i) shutdown-race stress guard (review P1-1: the mListenSock data
	// race between Stop() -- called from this test's main thread -- and
	// Serve()'s loop-guard + accept() -- running on a server thread).
	//
	// A code-level assertion isn't feasible here (no TSan target is wired
	// into this repo's default make build -- confirmed no -fsanitize=
	// flag/target exists under build/make/rise; noting that as owed rather
	// than fabricating a check). This is a BEHAVIOURAL stand-in instead:
	// repeatedly bind, start Serve() on its own thread, fire a burst of
	// concurrent connections WHILE Stop() is called from this thread (so
	// the close()-vs-accept()/loop-guard race in the .h/.cpp's doc is
	// actually exercised every iteration, not just a clean start/stop with
	// no traffic in flight), and join with a bounded watchdog. 50
	// iterations, no crash/abort/hang, is the guard: with mListenSock back
	// to a plain (non-atomic) SOCKET this same loop is a TSan-reportable
	// data race (unchanged observable behaviour on most POSIX runs since
	// closing a parked fd happens to unblock accept() today -- which is
	// exactly why this needs a stress rerun rather than a single pass to
	// have any chance of surfacing a scheduling-dependent regression).
	//------------------------------------------------------------------
	std::printf( "[stress] repeated bind/serve/concurrent-connect/stop cycles (P1-1 shutdown-race guard)\n" );
	{
		std::unique_ptr<AgentSession> stressSession = AgentSession::LoadFromFile( scenePath );
		Check( stressSession != nullptr, "stress: reloaded the temp scene for the stress guard" );
		AgentMcpAdapter stressAdapter( std::move( stressSession ), RISE::Agent::AgentAutonomy::Read );

		bool allCyclesOk = true;
		const int kCycles = 50;
		for( int cycle = 0; cycle < kCycles; ++cycle ) {
			AgentLoopbackHttpServer stressServer( &stressAdapter, "/mcp" );
			if( !stressServer.Bind( 0 ) ) { allCyclesOk = false; break; }
			const unsigned short stressPort = stressServer.BoundPort();

			std::thread stressThread( [&]() { stressServer.Serve(); } );

			// Fire a handful of connections concurrently with the Stop()
			// call below, so at least some of them race accept()/the loop
			// guard against the close()-then-store in Stop() -- exactly
			// the window the atomic fix closes.
			std::vector<std::thread> conns;
			for( int c = 0; c < 4; ++c ) {
				conns.emplace_back( [stressPort]() {
					const int cs = ConnectLoopback( stressPort, 1000 );
					if( cs >= 0 ) close( cs );   // connect-and-drop is enough to exercise accept()
				} );
			}

			stressServer.Stop();

			for( auto& t : conns ) t.join();

			std::atomic<bool> stressJoined( false );
			std::thread stressJoiner( [&]() { stressThread.join(); stressJoined.store( true, std::memory_order_release ); } );
			const auto stressDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 3 );
			while( !stressJoined.load( std::memory_order_acquire ) && std::chrono::steady_clock::now() < stressDeadline ) {
				std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
			}
			if( !stressJoined.load( std::memory_order_acquire ) ) { allCyclesOk = false; }
			if( stressJoiner.joinable() ) stressJoiner.join();
			if( !allCyclesOk ) break;
		}
		Check( allCyclesOk, "50 bind/serve/concurrent-connect/stop cycles: no crash, no hang, every Serve() thread joined promptly" );
	}

	//------------------------------------------------------------------
	// (m) [SLICE 4] per-launch token entropy: two separate server
	// construction cycles produce two DIFFERENT tokens -- proves the
	// token is a fresh CSPRNG draw per construction, not a fixed/
	// compiled-in constant. (Extremely rare false failure: two 256-bit
	// CSPRNG draws colliding has probability ~2^-256 -- not a real flake
	// risk.)
	//------------------------------------------------------------------
	std::printf( "[token-entropy] two server-construction cycles produce two different bearer tokens\n" );
	{
		std::unique_ptr<AgentSession> sessionA = AgentSession::LoadFromFile( scenePath );
		std::unique_ptr<AgentSession> sessionB = AgentSession::LoadFromFile( scenePath );
		Check( sessionA != nullptr && sessionB != nullptr, "token-entropy: reloaded the temp scene twice" );

		AgentMcpAdapter adapterA( std::move( sessionA ), RISE::Agent::AgentAutonomy::Read );
		AgentMcpAdapter adapterB( std::move( sessionB ), RISE::Agent::AgentAutonomy::Read );
		AgentLoopbackHttpServer serverA( &adapterA, "/mcp" );
		AgentLoopbackHttpServer serverB( &adapterB, "/mcp" );

		const std::string tokenA = serverA.ForTest_Token();
		const std::string tokenB = serverB.ForTest_Token();
		Check( tokenA.size() == 64 && tokenB.size() == 64,
		       "token-entropy: both tokens are 64 hex chars" );
		Check( tokenA != tokenB,
		       "token-entropy: two separate AgentLoopbackHttpServer construction cycles produce "
		       "DIFFERENT bearer tokens (per-launch CSPRNG, not a fixed constant)" );
	}

	//------------------------------------------------------------------
	// (n) [SLICE 4] ConstantTimeEquals-shaped coverage note: the helper
	// itself is anonymous-namespace-private to AgentLoopbackHttpServer.cpp
	// and not exposed for direct unit-testing from this file. Its
	// behavioural contract (equal -> true; differ at first byte / last
	// byte / length -> false; no early return) is exercised end-to-end via
	// the wrong-token cases in section (j) above (first-byte-diff,
	// last-byte-diff, and truncated-length all correctly 401). See the
	// doc comment on ConstantTimeEquals in AgentLoopbackHttpServer.cpp for
	// the code-read argument that it does not short-circuit (the
	// accumulate-then-check-once `diff` pattern, `volatile` to block a
	// smart compiler from re-introducing a data-dependent branch).
	//------------------------------------------------------------------

	std::remove( scenePath.c_str() );

	// Secure-MCP slice 5c: the GUI-hosted-server end-to-end topology --
	// self-contained (its own scene/controller/servers), run after the
	// slice-3/4 transport-correctness cases above.
	TestGuiHostedServerEndToEnd();

	// Secure-MCP slice 6: mutating-verb rate limit + total-request
	// deadline -- self-contained (its own scene/server constructions).
	TestMutatingRateLimitAndTotalDeadline();

	std::printf( "=== AgentLoopbackHttpTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}

#else  // _WIN32

int main()
{
	std::printf( "=== AgentLoopbackHttpTest: POSIX-only (BSD sockets); trivially passing on Windows ===\n" );
	(void)g_pass; (void)g_fail;
	return 0;
}

#endif
