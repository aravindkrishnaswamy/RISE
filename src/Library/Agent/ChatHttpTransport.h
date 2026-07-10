//////////////////////////////////////////////////////////////////////
//
//  ChatHttpTransport.h - the platform-HTTPS seam for the LIVE headless
//    eval runner (Eval-harness slice E4).
//
//    A NARROW synchronous request->response interface: the ONLY thing the
//    live runner needs from the network is "send this POST, hand me back
//    {status, body, error, elapsedMs}".  No streaming, no delegates, no
//    retries -- the AgentChatLoop owns retry/turn logic above this seam,
//    and the codecs own request-building below it.  Deliberately tiny so
//    a MOCK IChatHttpTransport can drive the EXACT SAME runner code path
//    the real socket does, keyless, with zero network (see
//    tests/AgentEvalLiveTransportTest.cpp).
//
//    PLATFORM LEGS (one TU compiles per platform; the OTHERS are gated
//    out of that platform's build entirely -- see the 5-project
//    registration):
//      * macOS  -> TlsTransportMac.mm   : NSURLSession (Foundation),
//                  synchronous via a dispatch_semaphore.  The plan §5
//                  blessed API (docs/agentic-redesign/70-agent-eval-
//                  harness.md).  A .mm (not .cpp): the pure-C CFNetwork
//                  CFHTTPStream sync API is deprecated since macOS 10.11
//                  and cannot compile warning-free, so NSURLSession is
//                  the warning-clean path.
//      * Windows-> TlsTransportWin.cpp  : WinHTTP (winhttp.lib), synchronous.
//      * Linux  -> TlsTransportUnsupported.cpp : an HONEST stub whose
//                  Post() returns a clear "unsupported on this platform"
//                  error.  v1 has no platform-TLS-without-a-lib-dep on
//                  Linux; the E2 replay backend covers Linux CI.  Also the
//                  Android (NDK/cmake) leg.
//
//    KEY HYGIENE (the security spine of E4): the api key rides in the
//    request's auth header ONLY (built by the codecs).  The transport
//    forwards those headers to the wire and NOWHERE else -- in particular
//    a TLS/HTTP FAILURE maps to a HEADER-FREE error CATEGORY
//    (domain/code/reason), NEVER a dump of the request or its headers.
//    ChatHttpResponse.error must never carry the key.  The trajectory
//    recorder (E1) strips the auth header by name AND regex-scrubs every
//    line; this interface's contract is the belt to that suspenders.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_AGENT_CHATHTTPTRANSPORT_
#define RISE_AGENT_CHATHTTPTRANSPORT_

#include <memory>
#include <string>

#include "AgentChatCodecs.h"   // ChatHttpRequest (url/headers/body)

namespace RISE
{
	namespace Agent
	{
		//! The outcome of one synchronous POST.  On a COMPLETED HTTP
		//! round-trip `status` is the HTTP status code (any of 2xx..5xx),
		//! `body` the response bytes verbatim, `error` empty.  On a
		//! TRANSPORT FAILURE (DNS/connect/TLS/timeout -- no HTTP response
		//! ever arrived) `status` is 0, `body` empty, and `error` a short
		//! HEADER-FREE category string (never the request/headers/key).
		//! `elapsedMs` is the measured wall-clock round-trip time in either
		//! case.
		struct ChatHttpResponse
		{
			long        status    = 0;   //!< HTTP status; 0 == no HTTP response reached (see error)
			std::string body;            //!< response body verbatim ("" on a transport failure)
			std::string error;           //!< "" on a completed round-trip; else a header-free failure category
			long        elapsedMs = 0;   //!< measured wall-clock ms of the round-trip
		};

		//! The synchronous HTTPS transport seam.  ONE method: perform the
		//! POST described by `req` and block until a response (or a
		//! transport failure).  Never throws.  Implementations MUST keep
		//! the api key (present in req.headers) out of the returned
		//! ChatHttpResponse.error.
		class IChatHttpTransport
		{
		public:
			virtual ~IChatHttpTransport() {}
			virtual ChatHttpResponse Post( const ChatHttpRequest& req ) = 0;
		};

		//! Construct the platform's system transport (macOS NSURLSession /
		//! Windows WinHTTP / Linux honest-unsupported stub).  NEVER returns
		//! null -- on Linux it returns the stub whose Post() honestly errors,
		//! so a caller can construct-then-probe rather than compile-branch.
		//! Defined by exactly ONE platform TU per build.
		std::unique_ptr<IChatHttpTransport> CreateSystemChatHttpTransport();
	}
}

#endif
