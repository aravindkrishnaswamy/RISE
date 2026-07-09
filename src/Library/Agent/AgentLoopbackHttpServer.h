//////////////////////////////////////////////////////////////////////
//
//  AgentLoopbackHttpServer.h - Secure-MCP slice 3+4: a LOOPBACK-ONLY
//    HTTP/1.1 transport for AgentMcpAdapter (the same 14 agent verbs
//    the stdio path serves, over a local TCP socket instead of a pipe).
//
//    Slice 3 shipped the infra:
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
//    Slice 4 (this revision) adds the actual security boundary on top
//    of that infra:
//      * a per-launch, cryptographically-random bearer token (see
//        GenerateBearerToken() in the .cpp) that EVERY request must
//        present via `Authorization: Bearer <token>`, checked in
//        CONSTANT TIME (ConstantTimeEquals()) so a timing side-channel
//        can't be used to guess the token byte-by-byte,
//      * Origin validation: a request that carries an `Origin` header
//        at all is rejected with 403 unless that Origin is an explicit
//        localhost/127.0.0.1 origin -- a non-browser MCP client never
//        sends Origin (passes), a browser tab (including one doing a
//        DNS-rebinding attack against 127.0.0.1) always does (rejected),
//      * Host validation: the `Host` header must name 127.0.0.1/localhost
//        (with or without the bound port) -- the companion half of the
//        anti-DNS-rebinding pair (a rebind attack points a hostile
//        domain's DNS at 127.0.0.1 and relies on the browser sending
//        THAT hostname as Host).
//    Binding loopback-only was already a real (if coarse) boundary --
//    only a process on this machine could connect at all -- but slice 3
//    stopped there: any OTHER local user/process, or a browser tab via
//    DNS rebinding, could still drive the adapter with no credential.
//    With the token + Origin/Host checks in place, this is now a real
//    (loopback-scoped) authentication boundary, not merely network
//    isolation -- see the startup banner in commandconsole.cpp for the
//    honest, still-limited claim (single-user local dev tool, not an
//    internet-facing auth system).
//
//    Autonomy default: unlike the stdio transport's CLI-layer default
//    (also Read), a caller embedding this server should treat an HTTP
//    endpoint as reachable by an arbitrary local MCP client, so Read is
//    the only sane default here too -- commandconsole.cpp constructs the
//    wrapped AgentMcpAdapter with the SAME `--agent-autonomy` resolution
//    the stdio paths use (default Read; `--agent-autonomy=commit`
//    overrides).  This class itself takes no autonomy opinion -- it just
//    forwards a fully-constructed AgentMcpAdapter's HandleLine.  Auth is
//    ORTHOGONAL to autonomy: a valid bearer token gets a request past the
//    transport's front door; autonomy still gates which verbs the
//    adapter will actually execute once inside.
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

			//! The per-launch bearer token every request must present via
			//! `Authorization: Bearer <token>` (see GenerateBearerToken() in
			//! the .cpp for the CSPRNG source). Generated once at
			//! construction and held for the server's lifetime. Production
			//! code has exactly one legitimate reader: the startup banner in
			//! commandconsole.cpp, which prints it ONCE to stderr. Named
			//! `ForTest_` per this codebase's test-accessor convention even
			//! though the banner also uses it -- there is no separate
			//! production getter, so the name documents "read sparingly and
			//! never log/echo this", not "test-only capability". NEVER log
			//! this, NEVER include it in a response body, NEVER write it to
			//! the file log.
			const std::string& ForTest_Token() const { return mBearerToken; }

			//! Test-only forwarder to the anonymous-namespace-private
			//! ConstantTimeEquals() in the .cpp (see its doc comment there
			//! for the full constant-time-compare contract). Exposed as a
			//! static class member -- rather than making the free function
			//! itself non-anonymous-namespace -- so production code still
			//! has exactly zero callers outside this translation unit; this
			//! is purely a seam for AgentLoopbackHttpTest.cpp to unit-test
			//! the length-fold behaviour directly (e.g. two lengths an
			//! exact multiple of 65536 apart), which end-to-end HTTP
			//! requests through this server cannot exercise (a request
			//! carrying a header/body anywhere near that size is already
			//! rejected by the 64 KiB / 8 MiB caps before reaching the
			//! token comparison).
			static bool ForTest_ConstantTimeEquals( const std::string& a, const std::string& b );

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

			//! Maximum accepted request HEADER block size in bytes (the
			//! cap ReadUntilDoubleCrlf enforces while accumulating up to
			//! the blank line before the body). Deliberately much smaller
			//! than kMaxBodyBytes -- no real HTTP header block from any
			//! client this server expects (a same-machine MCP client)
			//! approaches even a small fraction of this; 64 KiB is a
			//! generous ceiling against a hostile/broken peer that never
			//! sends a terminating blank line, so the header-read loop
			//! cannot be walked up anywhere near the 8 MiB body cap before
			//! being rejected. See the .cpp for the red-prove this guards.
			static const std::size_t kMaxHeaderBytes = 64u * 1024u;

			//! Secure-MCP slice 6: the per-connection TOTAL wall-clock
			//! deadline (milliseconds) covering the READ phase ONLY --
			//! from ServeOneConnection's entry (right after accept()) until
			//! the full request (header block + declared body) has been
			//! received. NOT a per-recv timeout (that is kSocketTimeoutMs
			//! in the .cpp, unrelated and unchanged): a per-recv timeout
			//! resets on every byte a peer sends, so a "slow-loris" drip --
			//! one byte every few seconds, forever -- never trips it. This
			//! deadline is checked against a FIXED wall-clock point set once
			//! at connection start, so no amount of dripping can extend it.
			//! Deliberately does NOT cover dispatch (AgentMcpAdapter::
			//! HandleLine, which includes `render` -- a legitimate
			//! multi-second-or-longer path-traced render is normal, SERVER-
			//! side latency, not an attacker-controlled receive-pacing
			//! attack) or the response write (that is the SEPARATE
			//! kDefaultTotalResponseWriteDeadlineMs below). 15s is
			//! generously above any realistic time a same-machine MCP
			//! client needs to finish SENDING a request (the body cap
			//! above is 8 MiB; even a pathologically slow loopback link
			//! finishes that transfer in a small fraction of 15s), so a
			//! well-behaved client should never observe this. See
			//! ForTest_SetTotalRequestDeadlineMs to shrink it for a test.
			static const int kDefaultTotalRequestDeadlineMs = 15000;

			//! Secure-MCP slice 6 fix round: the symmetric counterpart to
			//! kDefaultTotalRequestDeadlineMs above, covering the RESPONSE
			//! WRITE phase instead of the request read phase. Read-side
			//! slow-loris (a peer dripping request bytes so slowly that no
			//! single recv() ever times out, but the total read never
			//! finishes) was closed by the deadline above; a peer can just
			//! as easily mount the SAME attack on the write side -- accept
			//! a response (e.g. read_image's base64 PNG, potentially large)
			//! so slowly that no single send() ever hits SO_SNDTIMEO
			//! (kSocketTimeoutMs in the .cpp), but the total write never
			//! finishes, holding this single-threaded SERIAL server's one
			//! connection slot open indefinitely. SendAll takes a deadline
			//! of this same shape (a FIXED wall-clock point, checked before
			//! every send() call, set once per response) so a slow-read
			//! peer is cut off the same way a slow-send peer is on the read
			//! side. A fresh deadline is computed right before EACH
			//! SendAll call (not reused from the read-phase deadline above,
			//! and not shared across the whole connection) so a legitimate
			//! multi-second dispatch (a real render) never eats into the
			//! write budget, and vice versa. See
			//! ForTest_SetTotalResponseWriteDeadlineMs to shrink it for a
			//! test.
			static const int kDefaultTotalResponseWriteDeadlineMs = 15000;

			//! Secure-MCP slice 6: the mutating-verb rate limit -- a simple
			//! FIXED WINDOW (not sliding/token-bucket) counter: every
			//! kMutatingRateLimitWindowMs milliseconds the count resets to
			//! zero; a mutating tools/call (propose_patch/insert_chunk/
			//! remove_chunk/resolve_proposal -- see IsMutatingMcpToolCall in
			//! the .cpp) increments it, and the (kMutatingRateLimitMaxCalls+1)th
			//! call within the SAME window is refused with a structured
			//! JSON-RPC error (kMutatingRateLimitExceeded, defined in the
			//! .cpp) instead of reaching the adapter at all. Read-safe verbs
			//! (render, read_document, ...) are NEVER counted or limited.
			//! Enforced HERE, in the HTTP TRANSPORT, rather than in
			//! AgentMcpAdapter/AgentRpcDispatcher (which also serve `rise
			//! --agent-stdio`): this class is constructed ONLY for the two
			//! HTTP entry points (RunAgentHttp's CLI transport and the
			//! GUI-hosted loopback server in RISEViewportBridge.mm) and
			//! NEVER for stdio, so placing the limiter's state here bounds
			//! it to exactly the transport this slice is scoped to change,
			//! by construction -- no autonomy/posture flag needed to keep
			//! the stdio (GUI owner) path unaffected. Loopback + per-launch
			//! bearer token means every request through one server instance
			//! is the SAME authenticated client, so one GLOBAL (not
			//! per-connection) window is the right shape -- a hostile or
			//! malfunctioning client cannot reset the window by opening a
			//! fresh TCP connection (this server has no keep-alive; every
			//! request is already a fresh connection).  60/min is generously
			//! above any legitimate interactive or agentic edit cadence
			//! (an LLM-driven propose loop issuing a mutating call roughly
			//! once per turn) while still bounding a runaway/hostile client's
			//! ability to hammer the mutating verbs.
			static const int kMutatingRateLimitWindowMs = 60000;
			static const int kMutatingRateLimitMaxCalls = 60;

			//! Test seam: shrinks (or restores, passing
			//! kDefaultTotalRequestDeadlineMs) the per-connection total
			//! request deadline above -- lets a test exercise the slow-loris
			//! cutoff with a short, real wait instead of a real 15-second
			//! one. Affects connections accepted AFTER this call; safe to
			//! call before Serve() starts or between test sections (this
			//! class is single-threaded per its own documented contract, so
			//! there is no concurrent Serve() to race).
			void ForTest_SetTotalRequestDeadlineMs( int ms );

			//! Test seam: shrinks (or restores, passing
			//! kDefaultTotalResponseWriteDeadlineMs) the per-response total
			//! write deadline above. Same call-timing/threading contract as
			//! ForTest_SetTotalRequestDeadlineMs.
			void ForTest_SetTotalResponseWriteDeadlineMs( int ms );

			//! Test seam: overrides the millisecond wall-clock source the
			//! mutating-verb rate limiter reads (see kMutatingRateLimitWindowMs).
			//! `fn` is called with no arguments and must return a
			//! monotonically-nondecreasing millisecond count; pass nullptr
			//! (the default, restored by ForTest_ResetRateLimitClock) to use
			//! the real steady_clock-based source. A deterministic function
			//! lets a test roll the fixed window across its boundary without
			//! a real 60-second sleep.
			void ForTest_SetRateLimitClockFn( std::int64_t (*fn)() );

			//! Test seam: resets the mutating-verb rate limiter to an empty
			//! window (as if this server instance had just been
			//! constructed) -- lets a test start a rate-limit scenario from
			//! a clean slate regardless of how many mutating calls earlier
			//! test sections in the SAME process/instance already made.
			void ForTest_ResetRateLimit();

		private:
			AgentMcpAdapter*    mAdapter;      //!< borrowed
			std::string         mPath;         //!< the one servable POST path
			std::string         mBearerToken;  //!< per-launch CSPRNG token; see ForTest_Token()

			//! The listen socket. std::atomic<SOCKET> (NOT a plain SOCKET)
			//! because Stop() (called from a thread OTHER than the one
			//! running Serve(), per this class's documented threading
			//! contract) writes it while Serve()'s loop guard and accept()
			//! call read it on the server thread -- a plain SOCKET here is
			//! a TSan-confirmed data race (undefined behaviour that only
			//! "works" today because closing a parked fd happens to unblock
			//! a concurrent accept() on POSIX). Stop() closes the fd FIRST,
			//! THEN stores kBadSocket, so a racing load on the server
			//! thread only ever observes either the still-valid fd (accept()
			//! unblocks because the underlying descriptor was just closed)
			//! or kBadSocket (the loop's guard exits before calling
			//! accept() at all) -- never a torn/partial value. Serve() loads
			//! this once per iteration into a local SOCKET and uses that
			//! local for both the guard check and the accept() call, so a
			//! second concurrent Stop() can't change the value out from
			//! under a single iteration's use of it.
			std::atomic<SOCKET> mListenSock;
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

			//! Secure-MCP slice 6: the (test-overridable) total per-connection
			//! request-read deadline -- see kDefaultTotalRequestDeadlineMs's
			//! doc. Plain int (not atomic): only ever read/written on
			//! whichever single thread calls Serve()/ServeOneConnection, per
			//! this class's documented single-caller serving contract; the
			//! ForTest_ setter is documented to be called only before Serve()
			//! starts or between test sections, never concurrently with it.
			int mTotalRequestDeadlineMs;

			//! Secure-MCP slice 6 fix round: the (test-overridable) total
			//! per-response write deadline -- see
			//! kDefaultTotalResponseWriteDeadlineMs's doc. Same plain-int,
			//! single-threaded-access rationale as mTotalRequestDeadlineMs
			//! above.
			int mTotalResponseWriteDeadlineMs;

			//! Secure-MCP slice 6: the mutating-verb fixed-window rate
			//! limiter's state -- see kMutatingRateLimitWindowMs's doc.
			//! mRateWindowStartMs is the millisecond timestamp (per
			//! mRateLimitClockFn) the CURRENT window began;
			//! mRateWindowCount is how many mutating calls have landed in
			//! it so far; mRateWindowValid is false until the FIRST
			//! mutating call (or a ForTest_ResetRateLimit) starts a real
			//! window -- distinguishes "no window yet, start one now" from
			//! "window started at timestamp 0", which would otherwise be
			//! ambiguous for an injected test clock that legitimately
			//! begins counting at 0. Same single-threaded-access rationale
			//! as mTotalRequestDeadlineMs above -- no atomics needed.
			std::int64_t mRateWindowStartMs;
			int          mRateWindowCount;
			bool         mRateWindowValid;

			//! Test seam target for ForTest_SetRateLimitClockFn: nullptr
			//! (the production default) reads the real steady_clock; a test
			//! override replaces it with a deterministic source.
			std::int64_t (*mRateLimitClockFn)();

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

			//! Secure-MCP slice 6 fix round: a FRESH total-write deadline
			//! (now + mTotalResponseWriteDeadlineMs) -- see
			//! kDefaultTotalResponseWriteDeadlineMs's doc for why this is
			//! computed anew immediately before EACH SendAll call rather
			//! than threaded through from the request-read deadline or
			//! shared across the whole connection.
			std::chrono::steady_clock::time_point MakeResponseWriteDeadline() const;

			//! Secure-MCP slice 6: consults + ADVANCES the mutating-verb
			//! fixed-window rate limiter (mRateWindowStartMs/mRateWindowCount,
			//! per mRateLimitClockFn) and returns whether this call is
			//! allowed. Rolls the window forward (resetting the count to 0)
			//! if the current window has expired, THEN increments the count
			//! and compares against kMutatingRateLimitMaxCalls -- so this is
			//! a combined check-and-consume, called at most once per
			//! request that ServeOneConnection has already classified as a
			//! mutating tools/call (see IsMutatingMcpToolCall in the .cpp).
			bool CheckAndConsumeMutatingRateLimit();
		};
	}
}

#endif
