//////////////////////////////////////////////////////////////////////
//
//  AgentLoopbackHttpServer.h - Secure-MCP slice 3: a LOOPBACK-ONLY
//    HTTP/1.1 transport for AgentMcpAdapter (the same 12 agent verbs
//    the stdio path serves, over a local TCP socket instead of a pipe).
//
//    INFRA ONLY -- NOT auth-secure yet.  This slice ships:
//      * a TCP listen socket bound EXPLICITLY to 127.0.0.1 (never
//        INADDR_ANY / 0.0.0.0 -- see Bind()'s doc),
//      * a minimal hand-rolled HTTP/1.1 request/response codec (no
//        keep-alive, no chunked transfer, no pipelining -- Connection:
//        close on every response),
//      * a single-threaded SERIAL accept-handle loop so AgentMcpAdapter
//        (and the AgentRpcDispatcher it wraps -- "NOT thread-safe", see
//        AgentRpc.h) is NEVER entered concurrently by more than one
//        connection's request,
//      * a debug-build reentrancy guard that ABORTS if HandleLine is
//        somehow entered twice at once (belt-and-suspenders against a
//        future change accidentally threading this).
//
//    THIS SLICE HAS NO BEARER TOKEN AND NO ORIGIN CHECK.  Binding
//    loopback-only means only a process ON THIS MACHINE can connect --
//    that is a real (if coarse) boundary, but it is NOT a substitute for
//    authentication: any other local user/process/browser tab that can
//    reach 127.0.0.1 can drive the adapter.  Do not expose this beyond a
//    single-user local dev workflow until Secure-MCP slice 4 (bearer
//    token + Origin check) lands.  The `--agent-http` flag's help text
//    and this server's startup banner both say so explicitly.
//
//    Autonomy default: unlike the stdio transport's CLI-layer default
//    (also Read), a caller embedding this server should treat an HTTP
//    endpoint as reachable by an arbitrary local MCP client, so Read is
//    the only sane default here too -- commandconsole.cpp constructs the
//    wrapped AgentMcpAdapter with the SAME `--agent-autonomy` resolution
//    the stdio paths use (default Read; `--agent-autonomy=commit`
//    overrides).  This class itself takes no autonomy opinion -- it just
//    forwards a fully-constructed AgentMcpAdapter's HandleLine.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_AGENT_AGENTLOOPBACKHTTPSERVER_
#define RISE_AGENT_AGENTLOOPBACKHTTPSERVER_

#include <atomic>
#include <cstdint>
#include <string>

#ifdef WIN32
	#include <winsock.h>
#else
	typedef int SOCKET;
#endif

namespace RISE
{
	namespace Agent
	{
		class AgentMcpAdapter;

		//! A loopback-only HTTP/1.1 server fronting ONE AgentMcpAdapter.
		//! Owns the listen socket; does NOT own the adapter (borrowed --
		//! the caller keeps it alive for the server's lifetime).
		//!
		//! Threading contract: Serve() runs a SERIAL accept-handle loop on
		//! whichever thread calls it (typically a thread the caller spawns
		//! for this purpose) -- it accepts one connection, fully reads +
		//! dispatches + writes + closes it, THEN accepts the next.  A
		//! second client's connection simply queues in the OS listen
		//! backlog while the first is being served; there is no parallel
		//! handling and therefore no possibility of two concurrent
		//! HandleLine calls.  Stop() is safe to call from a DIFFERENT
		//! thread (the only cross-thread use this class supports) and
		//! causes Serve() to return promptly.
		class AgentLoopbackHttpServer
		{
		public:
			//! `adapter` is BORROWED -- must outlive this server (and any
			//! call to Serve()).  `path` is the ONLY HTTP path this server
			//! answers POST on (default "/mcp"); everything else (wrong
			//! method, wrong path) gets a clean 404/405, never dispatched.
			explicit AgentLoopbackHttpServer( AgentMcpAdapter* adapter,
			                                   const std::string& path = "/mcp" );
			~AgentLoopbackHttpServer();

			AgentLoopbackHttpServer( const AgentLoopbackHttpServer& ) = delete;
			AgentLoopbackHttpServer& operator=( const AgentLoopbackHttpServer& ) = delete;

			//! Bind + listen on 127.0.0.1:`port` (INADDR_LOOPBACK explicitly
			//! -- NEVER INADDR_ANY/0.0.0.0; see AgentLoopbackHttpServer.cpp
			//! for the loopback-bind proof this guards).  `port` == 0 asks
			//! the OS for an ephemeral port; read it back via BoundPort()
			//! after a successful Bind().  Returns false on any socket
			//! failure (address in use, permission, etc.) -- never throws.
			bool Bind( unsigned short port );

			//! The actual bound port (meaningful only after a successful
			//! Bind(); 0 before that / on failure).  Useful when Bind(0)
			//! asked the OS to choose.
			unsigned short BoundPort() const { return mBoundPort; }

			//! Test hook / structural loopback-bind proof: the dotted-quad
			//! IPv4 address the listen socket is ACTUALLY bound to, read
			//! back via getsockname() right after a successful Bind() --
			//! NOT re-derived from a client connect (which would trivially
			//! read "127.0.0.1" regardless of what the server bound,
			//! since the test dialed that literal itself). This is what
			//! lets a test RED-PROVE the bind target: temporarily change
			//! Bind()'s sockaddr_in construction to INADDR_ANY and this
			//! accessor reports "0.0.0.0" instead of "127.0.0.1",
			//! failing the assertion. "" before a successful Bind().
			std::string BoundAddress() const { return mBoundAddress; }

			//! True iff Bind() succeeded and the socket is still open.
			bool IsBound() const;

			//! Runs the serial accept-handle loop until Stop() is called
			//! (from another thread) or the listen socket is closed out
			//! from under it. Returns when the loop exits cleanly. Must be
			//! called after a successful Bind().
			void Serve();

			//! Signals Serve() to stop and closes the listen socket so a
			//! blocked accept() unblocks promptly. Idempotent; safe to
			//! call from a thread other than the one running Serve() (the
			//! one cross-thread use this class supports), and safe to call
			//! even if Serve() was never started or already returned.
			void Stop();

			//! Maximum accepted request body size in bytes (Content-Length
			//! cap). A request declaring a larger Content-Length is
			//! rejected with 413 BEFORE any body buffer is allocated --
			//! see the .cpp for the red-prove this guards. 8 MiB is
			//! generously above the largest real payload this adapter
			//! emits/consumes today (a base64 PNG read_image response body
			//! is server->client, unbounded by this cap; the heaviest
			//! CLIENT->server body is a propose_patch/insert_chunk chunk
			//! of scene text, nowhere near this size).
			static const std::size_t kMaxBodyBytes = 8u * 1024u * 1024u;

		private:
			AgentMcpAdapter*    mAdapter;      //!< borrowed
			std::string         mPath;         //!< the one servable POST path
			SOCKET              mListenSock;
			unsigned short      mBoundPort;
			std::string         mBoundAddress;   //!< dotted-quad, e.g. "127.0.0.1" -- see BoundAddress()'s doc
			std::atomic<bool>   mStopRequested;

			//! Debug-build reentrancy guard: true for the duration of any
			//! HandleLine call this server makes. A second entry while
			//! true is a hard violation of the single-caller dispatcher
			//! contract (AgentRpc.h) -- it aborts loudly rather than
			//! silently corrupting shared session state. Always defined
			//! (not just under NDEBUG-off) so the test build exercises it;
			//! the cost is one atomic per request, negligible next to a
			//! socket round trip.
			std::atomic<bool>   mInsideHandleLine;

			//! Accept and fully service ONE connection (read request,
			//! dispatch, write response, close). Never throws -- any
			//! internal failure degrades to closing the connection
			//! without a response (the client sees a reset/EOF, which is
			//! a valid "something went wrong" signal for a v1 transport).
			void ServeOneConnection( SOCKET client );

			//! Dispatch one already-fully-read JSON-RPC body through
			//! mAdapter->HandleLine() under the reentrancy guard. Returns
			//! the response line (may be empty for a true MCP
			//! notification -- see AgentMcpAdapter::HandleLine's doc; the
			//! HTTP layer still owes the client SOME response, so an empty
			//! adapter result is surfaced as a 200 with an empty JSON
			//! object body -- HTTP request/response is not fire-and-forget
			//! the way the stdio transport's notification line can be).
			std::string DispatchGuarded( const std::string& body );
		};
	}
}

#endif
