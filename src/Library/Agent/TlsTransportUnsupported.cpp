//////////////////////////////////////////////////////////////////////
//
//  TlsTransportUnsupported.cpp - the HONEST "no platform TLS" leg of the
//    live eval runner (Eval-harness slice E4).  Compiled on any platform
//    that has no in-tree platform-HTTPS implementation: Linux (make with
//    the non-macOS gate) and Android (NDK / cmake).  See
//    ChatHttpTransport.h.
//
//    v1 deliberately ships NO Linux live-run transport: there is no
//    platform TLS on Linux without adding a library dependency (libcurl /
//    OpenSSL), which the harness's dependency-free premise rules out.  The
//    E2 replay backend runs everywhere and covers Linux CI; only the LIVE
//    multi-provider comparison run is macOS/Windows-only in v1.
//
//    The factory still returns a NON-NULL transport here, so callers can
//    construct-then-probe uniformly across platforms.  Its Post() returns
//    a clear, header-free "unsupported" error (status 0) for every
//    request -- the runner surfaces that as an honest per-run failure
//    rather than crashing or silently doing nothing.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "ChatHttpTransport.h"

namespace RISE
{
	namespace Agent
	{
		namespace
		{
			class UnsupportedTransport : public IChatHttpTransport
			{
			public:
				ChatHttpResponse Post( const ChatHttpRequest& ) override
				{
					ChatHttpResponse out;
					out.status = 0;
					out.body.clear();
					// The sentinel substring lives in ChatHttpTransport.h so the
					// CLI probe + unit test match ONE shared literal (not a copy).
					out.error =
						std::string( "live HTTP transport is " ) + kUnsupportedTransportSentinel +
						" (v1 live runs are macOS/Windows only) -- use the replay "
						"backend (rise eval replay) for headless evaluation here";
					out.elapsedMs = 0;
					return out;
				}
			};
		}

		std::unique_ptr<IChatHttpTransport> CreateSystemChatHttpTransport()
		{
			return std::unique_ptr<IChatHttpTransport>( new UnsupportedTransport() );
		}
	}
}
