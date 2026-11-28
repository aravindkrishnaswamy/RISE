//////////////////////////////////////////////////////////////////////
//
//  AgentLoopbackHttpServer.h - loopback-only (127.0.0.1) HTTP/1.1
//    transport fronting an AgentMcpAdapter.
//
//    Transport & threading:
//      * Binds explicitly to INADDR_LOOPBACK (never INADDR_ANY/0.0.0.0).
//      * Minimal HTTP/1.1 (`Connection: close` on every response; no keep-alive,
//        chunked encoding, or pipelining).
//      * Single-threaded serial accept-handle loop (AgentMcpAdapter /
//        AgentRpcDispatcher are not thread-safe) with an atomic reentrancy
//        guard that aborts on concurrent HandleLine entry.
//
//    Security boundary (orthogonal to adapter autonomy):
//      * Per-launch CSPRNG bearer token checked in constant time via
//        `Authorization: Bearer <token>`.
//      * `Origin` (when present) and `Host` headers must name localhost /
//        127.0.0.1 to block browser DNS-rebinding attacks.
//      * Fixed wall-clock read/write deadlines (slow-loris protection) and a
//        global fixed-window rate limit on mutating MCP tool calls.
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
#include <chrono>
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

		//! Loopback-only HTTP/1.1 server fronting one borrowed AgentMcpAdapter.
		//! Serve() runs a serial accept-handle loop on one thread; Stop() is the
		//! only supported cross-thread call.
		class AgentLoopbackHttpServer
		{
		public:
			//! `adapter` is borrowed (must outlive server/Serve()). Only POST on
			//! `path` (default "/mcp") dispatches; other paths/methods return 404/405.
			explicit AgentLoopbackHttpServer( AgentMcpAdapter* adapter,
			                                   const std::string& path = "/mcp" );
			~AgentLoopbackHttpServer();

			AgentLoopbackHttpServer( const AgentLoopbackHttpServer& ) = delete;
			AgentLoopbackHttpServer& operator=( const AgentLoopbackHttpServer& ) = delete;

			//! Per-launch CSPRNG bearer token required via `Authorization: Bearer`.
			//! Production reads it once for the startup banner; never log or echo.
			const std::string& ForTest_Token() const { return mBearerToken; }

			//! Test seam exposing the TU-private constant-time string comparison.
			static bool ForTest_ConstantTimeEquals( const std::string& a, const std::string& b );

			//! Bind + listen on 127.0.0.1:`port` (`0` = OS ephemeral port). Never throws.
			bool Bind( unsigned short port );

			//! Actual bound port after Bind() (0 before/on failure).
			unsigned short BoundPort() const { return mBoundPort; }

			//! Actual dotted-quad bind address read via getsockname() (e.g. "127.0.0.1";
			//! "" before Bind()) for structural loopback-bind verification.
			std::string BoundAddress() const { return mBoundAddress; }

			bool IsBound() const;

			//! Serial accept-handle loop; returns cleanly after Stop() or socket close.
			void Serve();

			//! Thread-safe, idempotent shutdown signal that unblocks accept().
			void Stop();

			//! Max accepted request body (413 before allocation) and header block sizes.
			static const std::size_t kMaxBodyBytes = 8u * 1024u * 1024u;
			static const std::size_t kMaxHeaderBytes = 64u * 1024u;

			//! Total wall-clock deadlines (ms) for request read and per-response write
			//! (slow-loris cutoff; dispatch latency such as renders is excluded).
			static const int kDefaultTotalRequestDeadlineMs = 15000;
			static const int kDefaultTotalResponseWriteDeadlineMs = 15000;

			//! Global fixed-window rate limit (60/min) on mutating MCP tool calls
			//! classified by `IsMutatingMcpToolCall` in `.cpp` (HTTP-only; excess
			//! returns JSON-RPC error -32013 `kMutatingRateLimitExceeded`). Read-safe
			//! verbs (see `AgentRpc.cpp`'s `IsReadSafeVerb`) are never counted.
			static const int kMutatingRateLimitWindowMs = 60000;
			static const int kMutatingRateLimitMaxCalls = 60;

			//! Test seams (call before Serve() or between single-threaded test sections).
			void ForTest_SetTotalRequestDeadlineMs( int ms );
			void ForTest_SetTotalResponseWriteDeadlineMs( int ms );
			void ForTest_SetRateLimitClockFn( std::int64_t (*fn)() );
			void ForTest_ResetRateLimit();

		private:
			AgentMcpAdapter*    mAdapter;      //!< borrowed
			std::string         mPath;         //!< sole servable POST path
			std::string         mBearerToken;  //!< per-launch CSPRNG token

			//! Atomic because Stop() closes/stores from another thread while Serve() reads.
			std::atomic<SOCKET> mListenSock;
			unsigned short      mBoundPort;
			std::string         mBoundAddress;
			std::atomic<bool>   mStopRequested;
			std::atomic<bool>   mInsideHandleLine; //!< aborts on concurrent HandleLine entry

			// Single-threaded serving state (deadline overrides and fixed-window rate limiter).
			int mTotalRequestDeadlineMs;
			int mTotalResponseWriteDeadlineMs;
			std::int64_t mRateWindowStartMs;
			int          mRateWindowCount;
			bool         mRateWindowValid;         //!< distinguishes unset window from t == 0
			std::int64_t (*mRateLimitClockFn)();   //!< nullptr -> steady_clock

			void ServeOneConnection( SOCKET client );
			//! Runs mAdapter->HandleLine under reentrancy guard (empty notification -> HTTP 200 `{}`).
			std::string DispatchGuarded( const std::string& body );
			std::chrono::steady_clock::time_point MakeResponseWriteDeadline() const;
			bool CheckAndConsumeMutatingRateLimit();
		};
	}
}

#endif
