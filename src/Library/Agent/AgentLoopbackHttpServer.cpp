//////////////////////////////////////////////////////////////////////
//
//  AgentLoopbackHttpServer.cpp - see AgentLoopbackHttpServer.h.
//
//////////////////////////////////////////////////////////////////////

#include "AgentLoopbackHttpServer.h"

#include "AgentMcpAdapter.h"
#include "../Interfaces/ILog.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef WIN32
	#include <winsock.h>
	typedef int socklen_t_compat;
#else
	#include <unistd.h>
	#include <sys/types.h>
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <netinet/tcp.h>
	#include <arpa/inet.h>
	#include <errno.h>
	typedef socklen_t socklen_t_compat;
	static const SOCKET kBadSocket = -1;
#endif

#ifdef WIN32
	static const SOCKET kBadSocket = INVALID_SOCKET;
#endif

using namespace RISE;
using namespace RISE::Agent;

namespace
{
	//! Per-connection read/write timeout. Bounds every recv()/send() this
	//! server issues so a client that opens a connection and never sends
	//! (or never drains) cannot wedge the serial accept-handle loop
	//! forever -- a single slow/hostile peer degrades to "this one
	//! connection times out and gets dropped", never "the server hangs".
	const int kSocketTimeoutMs = 5000;

	//! Applies a recv/send timeout to `sock`. Best-effort: a failure here
	//! is not fatal (the connection just loses the bound), so it is not
	//! reported to the caller.
	void SetSocketTimeout( SOCKET sock, int timeoutMs )
	{
#ifdef WIN32
		DWORD tv = static_cast<DWORD>( timeoutMs );
		setsockopt( sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>( &tv ), sizeof( tv ) );
		setsockopt( sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>( &tv ), sizeof( tv ) );
#else
		struct timeval tv;
		tv.tv_sec  = timeoutMs / 1000;
		tv.tv_usec = ( timeoutMs % 1000 ) * 1000;
		setsockopt( sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof( tv ) );
		setsockopt( sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof( tv ) );
#endif
	}

	void CloseSock( SOCKET s )
	{
		if( s == kBadSocket ) return;
#ifdef WIN32
		closesocket( s );
#else
		close( s );
#endif
	}

	//! Reads from `sock` until `needle` ("\r\n\r\n") is found (returning
	//! everything up to and including it in `outHead`, with any bytes read
	//! PAST the needle left in `outSpill` for the caller to treat as the
	//! start of the body) or `capBytes` is exceeded (returns false -- the
	//! caller maps that to a 431-equivalent clean rejection) or the peer
	//! closes / times out (returns false). Never throws, never grows
	//! unbounded.
	bool ReadUntilDoubleCrlf( SOCKET sock, std::size_t capBytes,
	                          std::string& outHead, std::string& outSpill )
	{
		outHead.clear();
		outSpill.clear();
		std::string buf;
		char chunk[4096];
		for( ;; ) {
			const std::size_t pos = buf.find( "\r\n\r\n" );
			if( pos != std::string::npos ) {
				outHead = buf.substr( 0, pos + 4 );
				outSpill = buf.substr( pos + 4 );
				return true;
			}
			if( buf.size() > capBytes ) return false;
			const int n = static_cast<int>( recv( sock, chunk, sizeof( chunk ), 0 ) );
			if( n <= 0 ) return false;   // peer closed, error, or timeout
			buf.append( chunk, static_cast<std::size_t>( n ) );
		}
	}

	//! Reads exactly `need` more bytes into `inOut` (which may already
	//! hold `already` spilled-over bytes from the header read), appending
	//! to whatever `inOut` already holds. Returns false on short
	//! read/timeout/peer-close before `need` total bytes are collected --
	//! the caller treats that as a clean malformed-request drop, never a
	//! hang (bounded by the per-recv socket timeout) and never a crash.
	bool ReadExactly( SOCKET sock, std::string& inOut, std::size_t need )
	{
		char chunk[4096];
		while( inOut.size() < need ) {
			const std::size_t want = need - inOut.size();
			const int toRead = static_cast<int>( want < sizeof( chunk ) ? want : sizeof( chunk ) );
			const int n = static_cast<int>( recv( sock, chunk, toRead, 0 ) );
			if( n <= 0 ) return false;
			inOut.append( chunk, static_cast<std::size_t>( n ) );
		}
		return true;
	}

	//! Sends the entirety of `data` on `sock`, looping over short writes.
	//! Returns false on any send() failure/timeout (the caller has
	//! nothing further to do but close -- there is no response-to-a-
	//! response in HTTP/1.1).
	bool SendAll( SOCKET sock, const std::string& data )
	{
		std::size_t sent = 0;
		while( sent < data.size() ) {
			const int n = static_cast<int>( send( sock, data.data() + sent,
			                                       static_cast<int>( data.size() - sent ), 0 ) );
			if( n <= 0 ) return false;
			sent += static_cast<std::size_t>( n );
		}
		return true;
	}

	//! Builds a complete HTTP/1.1 response: status line + the fixed
	//! headers this v1 transport always sends (Content-Type,
	//! Content-Length, Connection: close) + body. `statusLine` is just
	//! the reason phrase half, e.g. "200 OK".
	std::string BuildHttpResponse( const std::string& statusLine,
	                                const std::string& contentType,
	                                const std::string& body )
	{
		std::string out;
		out.reserve( body.size() + 128 );
		out += "HTTP/1.1 ";
		out += statusLine;
		out += "\r\n";
		out += "Content-Type: ";
		out += contentType;
		out += "\r\n";
		out += "Content-Length: ";
		out += std::to_string( body.size() );
		out += "\r\n";
		out += "Connection: close\r\n";
		out += "\r\n";
		out += body;
		return out;
	}

	std::string PlainError( const std::string& statusLine, const std::string& message )
	{
		return BuildHttpResponse( statusLine, "text/plain", message );
	}

	//! Logs ONE line of request metadata -- method, path, status, and
	//! byte sizes ONLY. NEVER the request or response BODY (that could
	//! carry scene text, a base64 PNG, or -- once slice 4 lands -- a
	//! bearer token): the logging hygiene for this transport starts now,
	//! not when auth is added.
	void LogRequest( const std::string& method, const std::string& path,
	                  int status, std::size_t reqBytes, std::size_t respBytes )
	{
		GlobalLog()->PrintEx( eLog_Event,
			"agent-http: %s %s -> %d (req %zu bytes, resp %zu bytes)",
			method.c_str(), path.c_str(), status, reqBytes, respBytes );
	}
}

AgentLoopbackHttpServer::AgentLoopbackHttpServer( AgentMcpAdapter* adapter, const std::string& path )
	: mAdapter( adapter )
	, mPath( path )
	, mListenSock( kBadSocket )
	, mBoundPort( 0 )
	, mBoundAddress()
	, mStopRequested( false )
	, mInsideHandleLine( false )
{
}

AgentLoopbackHttpServer::~AgentLoopbackHttpServer()
{
	Stop();
}

bool AgentLoopbackHttpServer::Bind( unsigned short port )
{
#ifdef WIN32
	SOCKET s = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
	if( s == INVALID_SOCKET ) return false;
#else
	SOCKET s = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
	if( s < 0 ) return false;
#endif

	// Allow a fast rebind after this process exits (TIME_WAIT) -- purely a
	// convenience for local dev iteration; does NOT relax the loopback
	// bind below in any way.
	{
		int reuse = 1;
#ifdef WIN32
		setsockopt( s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>( &reuse ), sizeof( reuse ) );
#else
		setsockopt( s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
#endif
	}

	// THE load-bearing line: bind to 127.0.0.1 EXPLICITLY via
	// inet_addr/inet_pton on the loopback literal -- never
	// htonl(INADDR_ANY). SocketComm::EstablishConnection (the in-tree
	// server/accept helper used by DRISE) hard-codes INADDR_ANY, which is
	// exactly the wrong default for an agent-control endpoint with no
	// auth yet, so this server binds its own sockaddr_in instead of
	// reusing that helper.
	struct sockaddr_in addr;
	memset( &addr, 0, sizeof( addr ) );
	addr.sin_family = AF_INET;
	addr.sin_port   = htons( port );
#ifdef WIN32
	addr.sin_addr.s_addr = inet_addr( "127.0.0.1" );
#else
	inet_pton( AF_INET, "127.0.0.1", &addr.sin_addr );
#endif

	if( bind( s, reinterpret_cast<struct sockaddr*>( &addr ), sizeof( addr ) ) < 0 ) {
		CloseSock( s );
		return false;
	}

	if( listen( s, 16 ) < 0 ) {
		CloseSock( s );
		return false;
	}

	// Recover the actually-bound port (needed when the caller asked for
	// port 0 -- an OS-assigned ephemeral port).
	struct sockaddr_in bound;
	memset( &bound, 0, sizeof( bound ) );
	socklen_t_compat boundLen = sizeof( bound );
	if( getsockname( s, reinterpret_cast<struct sockaddr*>( &bound ), &boundLen ) == 0 ) {
		mBoundPort = ntohs( bound.sin_port );
		char addrBuf[INET_ADDRSTRLEN] = {0};
#ifdef WIN32
		// inet_ntop is available on Vista+ winsock2; this codebase's
		// Windows leg is uncompiled today (see the owed-check note), so
		// this branch is written by symmetry, not verified.
		inet_ntop( AF_INET, &bound.sin_addr, addrBuf, sizeof( addrBuf ) );
#else
		inet_ntop( AF_INET, &bound.sin_addr, addrBuf, sizeof( addrBuf ) );
#endif
		mBoundAddress = addrBuf;
	} else {
		mBoundPort = port;
		mBoundAddress.clear();
	}

	mListenSock = s;
	mStopRequested.store( false, std::memory_order_release );
	return true;
}

bool AgentLoopbackHttpServer::IsBound() const
{
	return mListenSock != kBadSocket;
}

void AgentLoopbackHttpServer::Stop()
{
	mStopRequested.store( true, std::memory_order_release );
	if( mListenSock != kBadSocket ) {
		// Closing the listen socket unblocks a thread parked in accept()
		// on POSIX. (On Windows an in-progress WSAAccept can behave
		// differently across versions -- see the Windows owed-check note
		// in this slice's final report; this codebase does not compile
		// the Windows leg today.)
		CloseSock( mListenSock );
		mListenSock = kBadSocket;
	}
}

void AgentLoopbackHttpServer::Serve()
{
	while( !mStopRequested.load( std::memory_order_acquire ) ) {
		if( mListenSock == kBadSocket ) break;

#ifdef WIN32
		SOCKET client = accept( mListenSock, nullptr, nullptr );
		if( client == INVALID_SOCKET ) {
#else
		SOCKET client = accept( mListenSock, nullptr, nullptr );
		if( client < 0 ) {
#endif
			if( mStopRequested.load( std::memory_order_acquire ) ) break;
			continue;   // a transient accept error -- keep serving
		}

		SetSocketTimeout( client, kSocketTimeoutMs );

		// SERIALIZATION: handle this connection to completion (read,
		// dispatch, respond, close) BEFORE accepting the next one. This
		// is what guarantees AgentMcpAdapter::HandleLine (and the
		// AgentRpcDispatcher it wraps -- documented NOT thread-safe) is
		// never entered from two connections at once: there is only ever
		// ONE connection alive in this loop's body at a time. A second
		// client's SYN queues in the OS listen backlog (16 deep, set in
		// Bind()) and is accepted on the NEXT loop iteration.
		ServeOneConnection( client );
		CloseSock( client );
	}
}

std::string AgentLoopbackHttpServer::DispatchGuarded( const std::string& body )
{
	// Reentrancy guard: exchange-to-true and check what was there before.
	// If it was ALREADY true, some other call path entered HandleLine
	// concurrently with this one -- a hard violation of the dispatcher's
	// documented single-caller contract (AgentRpc.h). The serial
	// accept-handle loop in Serve() is the only production call site and
	// is single-threaded by construction, so this should be
	// unreachable in production; it exists as cheap, always-armed
	// insurance against a FUTURE change (e.g. someone "optimizing" this
	// into a thread-per-connection server without adding the necessary
	// mutex) silently reintroducing a real corruption bug instead of
	// failing loudly at the first concurrent request.
	const bool alreadyInside = mInsideHandleLine.exchange( true, std::memory_order_acq_rel );
	if( alreadyInside ) {
		std::fprintf( stderr,
			"FATAL: AgentLoopbackHttpServer detected a CONCURRENT entry into "
			"AgentMcpAdapter::HandleLine -- the dispatcher's single-caller "
			"contract (AgentRpc.h) has been violated. Aborting.\n" );
		std::abort();
	}

	std::string response;
	try {
		response = mAdapter->HandleLine( body );
	} catch( ... ) {
		// HandleLine itself is documented to never throw, but this guard
		// exists specifically to keep the reentrancy flag correct even if
		// that contract is ever violated -- the flag MUST be cleared
		// before we rethrow/return, or every subsequent request would
		// falsely trip the abort above.
		mInsideHandleLine.store( false, std::memory_order_release );
		throw;
	}

	mInsideHandleLine.store( false, std::memory_order_release );
	return response;
}

void AgentLoopbackHttpServer::ServeOneConnection( SOCKET client )
{
	std::string head, spill;
	if( !ReadUntilDoubleCrlf( client, kMaxBodyBytes, head, spill ) ) {
		// Malformed / oversized headers, or the peer vanished before
		// sending a complete header block. Nothing sane to respond with
		// (we don't even reliably know it's HTTP) -- close cleanly.
		const std::string resp = PlainError( "400 Bad Request", "malformed or incomplete request headers" );
		SendAll( client, resp );
		LogRequest( "?", "?", 400, head.size(), resp.size() );
		return;
	}

	// ---- Parse the request line: "METHOD SP path SP HTTP/1.1\r\n" ----
	const std::size_t firstCrlf = head.find( "\r\n" );
	const std::string requestLine = ( firstCrlf == std::string::npos ) ? head : head.substr( 0, firstCrlf );

	std::string method, path, httpVersion;
	{
		const std::size_t sp1 = requestLine.find( ' ' );
		if( sp1 != std::string::npos ) {
			method = requestLine.substr( 0, sp1 );
			const std::size_t afterSp1 = sp1 + 1;
			const std::size_t sp2 = requestLine.find( ' ', afterSp1 );
			if( sp2 != std::string::npos ) {
				path = requestLine.substr( afterSp1, sp2 - afterSp1 );
				httpVersion = requestLine.substr( sp2 + 1 );
			}
		}
	}

	if( method.empty() || path.empty() || httpVersion.empty() ) {
		const std::string resp = PlainError( "400 Bad Request", "malformed request line" );
		SendAll( client, resp );
		LogRequest( method.empty() ? "?" : method, path.empty() ? "?" : path, 400, head.size(), resp.size() );
		return;
	}

	// ---- Parse headers for Content-Length (the only header this v1
	// transport reads; everything else -- including any future
	// Authorization/Origin header slice 4 will add -- is out of scope
	// here). ----
	long long contentLength = -1;
	{
		std::size_t lineStart = firstCrlf;
		while( lineStart != std::string::npos && lineStart + 2 <= head.size() ) {
			lineStart += 2;   // skip the CRLF we just consumed
			const std::size_t lineEnd = head.find( "\r\n", lineStart );
			if( lineEnd == std::string::npos ) break;
			const std::string hline = head.substr( lineStart, lineEnd - lineStart );
			if( hline.empty() ) break;   // the blank line before the body

			const std::size_t colon = hline.find( ':' );
			if( colon != std::string::npos ) {
				std::string key = hline.substr( 0, colon );
				for( char& c : key ) c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
				if( key == "content-length" ) {
					std::string val = hline.substr( colon + 1 );
					// trim leading/trailing whitespace
					std::size_t a = val.find_first_not_of( " \t" );
					std::size_t b = val.find_last_not_of( " \t\r" );
					val = ( a == std::string::npos ) ? std::string() : val.substr( a, b - a + 1 );

					// Strict numeric parse: reject empty, non-digit, and
					// negative-looking values loudly rather than letting
					// strtoll silently return 0 for garbage input.
					bool allDigits = !val.empty();
					for( char c : val ) { if( c < '0' || c > '9' ) { allDigits = false; break; } }
					if( !allDigits ) {
						contentLength = -2;   // sentinel: malformed, distinct from "absent"
					} else {
						char* endp = nullptr;
						errno = 0;
						const long long v = std::strtoll( val.c_str(), &endp, 10 );
						if( errno == ERANGE || endp == val.c_str() || *endp != '\0' ) {
							contentLength = -2;
						} else {
							contentLength = v;
						}
					}
				}
			}
			lineStart = lineEnd;
		}
	}

	const bool isPost = ( method == "POST" );

	if( !isPost ) {
		const std::string resp = PlainError( "405 Method Not Allowed", "only POST is served" );
		SendAll( client, resp );
		LogRequest( method, path, 405, head.size(), resp.size() );
		return;
	}

	if( path != mPath ) {
		const std::string resp = PlainError( "404 Not Found", "unknown path" );
		SendAll( client, resp );
		LogRequest( method, path, 404, head.size(), resp.size() );
		return;
	}

	if( contentLength == -1 ) {
		const std::string resp = PlainError( "400 Bad Request", "missing Content-Length" );
		SendAll( client, resp );
		LogRequest( method, path, 400, head.size(), resp.size() );
		return;
	}
	if( contentLength == -2 ) {
		const std::string resp = PlainError( "400 Bad Request", "malformed Content-Length" );
		SendAll( client, resp );
		LogRequest( method, path, 400, head.size(), resp.size() );
		return;
	}
	if( contentLength < 0 ) {
		// Reachable only if strtoll somehow returned a negative value
		// past the all-digits check above (it cannot, given the input is
		// pre-validated to be digits-only) -- kept as defense in depth
		// rather than trusting the parse implicitly.
		const std::string resp = PlainError( "400 Bad Request", "invalid Content-Length" );
		SendAll( client, resp );
		LogRequest( method, path, 400, head.size(), resp.size() );
		return;
	}
	if( static_cast<unsigned long long>( contentLength ) > kMaxBodyBytes ) {
		// THE cap: reject BEFORE allocating/reading a body anywhere near
		// this size. Red-prove: with this check removed (or the cap
		// raised past available memory), a client can declare an
		// enormous Content-Length and either wedge this server allocating
		// for it or exhaust memory -- see AgentLoopbackHttpTest.cpp's
		// oversized-body case.
		const std::string resp = PlainError( "413 Payload Too Large", "Content-Length exceeds the server cap" );
		SendAll( client, resp );
		LogRequest( method, path, 413, head.size(), resp.size() );
		return;
	}

	const std::size_t need = static_cast<std::size_t>( contentLength );
	std::string body = spill;   // bytes already read past the header terminator
	if( body.size() > need ) {
		// Shouldn't happen for a single well-formed request (nothing
		// pipelined follows in this v1, no-keep-alive transport), but if
		// a client sent extra trailing bytes, only the declared length is
		// the body -- truncate defensively rather than passing garbage to
		// the JSON parser.
		body.resize( need );
	} else if( body.size() < need ) {
		if( !ReadExactly( client, body, need ) ) {
			// Truncated body: Content-Length promised more than the peer
			// actually sent (or it timed out / closed mid-body). Clean
			// rejection, no hang (bounded by the per-recv socket
			// timeout set in Serve()), no crash.
			const std::string resp = PlainError( "400 Bad Request", "truncated request body" );
			SendAll( client, resp );
			LogRequest( method, path, 400, head.size() + need, resp.size() );
			return;
		}
	}

	// ---- Dispatch through the (guarded) adapter. ----
	const std::string adapterResponse = DispatchGuarded( body );

	// AgentMcpAdapter::HandleLine returns an EMPTY string for a true MCP
	// notification (no `id` field) -- the stdio transport's contract is
	// "skip writing a line" for that case, but HTTP is a strict
	// request/response protocol: this connection is owed SOME response.
	// A notification therefore gets a 200 with an empty JSON object body
	// -- a harmless, spec-legal "acknowledged, nothing to report" answer
	// rather than leaving the client's HTTP request hanging forever.
	const std::string responseBody = adapterResponse.empty() ? std::string( "{}" ) : adapterResponse;
	const std::string httpResp = BuildHttpResponse( "200 OK", "application/json", responseBody );
	SendAll( client, httpResp );
	LogRequest( method, path, 200, head.size() + need, httpResp.size() );
}
