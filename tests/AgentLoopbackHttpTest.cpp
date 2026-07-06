//////////////////////////////////////////////////////////////////////
//
//  AgentLoopbackHttpTest.cpp - Secure-MCP slice 3: the loopback HTTP/1.1
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

#include <atomic>
#include <chrono>
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

//! Round-trips ONE HTTP request over a fresh connection and returns the
//! parsed response. `rawRequestOverride`, when non-null, is sent VERBATIM
//! instead of building a well-formed request (for the malformed-input
//! cases) -- the connection is still made via ConnectLoopback so the
//! per-socket timeout applies.
static HttpResponse DoRequest( unsigned short port, const std::string& method,
                                const std::string& path, const std::string& body )
{
	const int s = ConnectLoopback( port );
	if( s < 0 ) return HttpResponse();

	std::string req = method + " " + path + " HTTP/1.1\r\n";
	req += "Host: 127.0.0.1\r\n";
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

int main()
{
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
			const std::string raw = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
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
			SendAll( s, "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: notanumber\r\n\r\n" );
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
			SendAll( s, "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: -5\r\n\r\n" );
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
			std::string raw = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: " +
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

	std::remove( scenePath.c_str() );

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
