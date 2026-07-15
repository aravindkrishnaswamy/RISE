//////////////////////////////////////////////////////////////////////
//
//  TlsTransportWin.cpp - the Windows platform-HTTPS leg of the live eval
//    runner (Eval-harness slice E4).  WinHTTP, synchronous.  See
//    ChatHttpTransport.h for the interface + hygiene contract.
//
//    This TU compiles ONLY in the Windows build (Library.vcxproj); the
//    make/Xcode/cmake builds gate it out entirely.  It links winhttp.lib
//    (added to RISE-CLI.vcxproj + RISE-GUI.vcxproj AdditionalDependencies,
//    Debug + Release -- the executables that link the RISE static lib).
//
//    COMPILE-BLIND: authored on macOS with no MSVC available -- owed to
//    the next Windows pass (precedent 96e74f59, the same as prior
//    Windows-only agent TUs).  The WinHTTP call sequence is the standard
//    synchronous pattern (Open -> Connect -> OpenRequest -> SendRequest ->
//    ReceiveResponse -> QueryHeaders(status) -> QueryDataAvailable/ReadData
//    loop).
//
//    KEY HYGIENE: on any WinHTTP failure the error string is a header-free
//    category built from the failing function name + the numeric
//    GetLastError() only -- never a dump of the request or its headers, so
//    the api key (carried in the request headers) cannot leak through it.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ChatHttpTransport.h"

#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <string>
#include <vector>

namespace RISE
{
	namespace Agent
	{
		namespace
		{
			//! Resolve/connect phases stay on a short, fixed budget --
			//! DNS + the TCP/TLS handshake either succeeds quickly or
			//! the endpoint is unreachable; neither phase should inherit
			//! the long per-request budget meant for generation time.
			const int kResolveConnectTimeoutMs = 60000;

			std::wstring WidenUtf8( const std::string& s )
			{
				if( s.empty() ) return std::wstring();
				const int n = MultiByteToWideChar( CP_UTF8, 0, s.data(),
					static_cast<int>( s.size() ), nullptr, 0 );
				if( n <= 0 ) return std::wstring();
				std::wstring w( static_cast<std::size_t>( n ), L'\0' );
				MultiByteToWideChar( CP_UTF8, 0, s.data(), static_cast<int>( s.size() ), &w[0], n );
				return w;
			}

			//! Header-free failure category: the failing WinHTTP function name
			//! + the numeric last-error code.  NEVER the request headers/key.
			std::string WinErr( const char* fn )
			{
				return std::string( "transport error [" ) + fn + " " +
					std::to_string( static_cast<long>( GetLastError() ) ) + "]";
			}

			class WinHttpTransport : public IChatHttpTransport
			{
			public:
				ChatHttpResponse Post( const ChatHttpRequest& req ) override
				{
					ChatHttpResponse out;
					const auto t0 = std::chrono::steady_clock::now();

					const std::wstring wurl = WidenUtf8( req.url );

					URL_COMPONENTS uc;
					ZeroMemory( &uc, sizeof( uc ) );
					uc.dwStructSize = sizeof( uc );
					wchar_t host[256] = { 0 };
					wchar_t path[4096] = { 0 };
					uc.lpszHostName    = host;   uc.dwHostNameLength = 255;
					uc.lpszUrlPath     = path;   uc.dwUrlPathLength  = 4095;
					if( !WinHttpCrackUrl( wurl.c_str(), static_cast<DWORD>( wurl.size() ), 0, &uc ) ) {
						out.error = "invalid request URL";
						return Finish( out, t0 );
					}
					const bool https = ( uc.nScheme == INTERNET_SCHEME_HTTPS );
					const INTERNET_PORT port = uc.nPort;

					HINTERNET hSession = WinHttpOpen( L"RISE-AgentEval/1.0",
						WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
						WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 );
					if( !hSession ) { out.error = WinErr( "WinHttpOpen" ); return Finish( out, t0 ); }
					// req.timeoutSeconds is the per-request budget (see
					// ChatHttpRequest::timeoutSeconds): 300s for hosted
					// providers, 900s for local inference (cold model swap +
					// long generation legitimately exceeds 300s).  Resolve
					// and connect stay on the short fixed budget above; send
					// and receive get the full per-request budget.
					const int sendReceiveTimeoutMs =
						static_cast<int>( req.timeoutSeconds * 1000 );
					WinHttpSetTimeouts( hSession, kResolveConnectTimeoutMs, kResolveConnectTimeoutMs,
						sendReceiveTimeoutMs, sendReceiveTimeoutMs );

					HINTERNET hConnect = WinHttpConnect( hSession, host, port, 0 );
					if( !hConnect ) {
						out.error = WinErr( "WinHttpConnect" );
						WinHttpCloseHandle( hSession );
						return Finish( out, t0 );
					}

					const DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
					HINTERNET hRequest = WinHttpOpenRequest( hConnect, L"POST", path, nullptr,
						WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags );
					if( !hRequest ) {
						out.error = WinErr( "WinHttpOpenRequest" );
						WinHttpCloseHandle( hConnect );
						WinHttpCloseHandle( hSession );
						return Finish( out, t0 );
					}

					// KEY HYGIENE: refuse ALL redirects.  A followed 3xx would
					// replay the auth header (carrying the api key) to the redirect
					// target -- possibly a different host.  Our providers answer
					// 200/4xx directly, so a 3xx is returned to the codec as-is.
					// Fail closed: if the policy can't be set we abort rather than
					// send a request that might auto-follow and leak the key.
					DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
					if( !WinHttpSetOption( hRequest, WINHTTP_OPTION_REDIRECT_POLICY,
						&redirectPolicy, sizeof( redirectPolicy ) ) ) {
						out.error = WinErr( "WinHttpSetOption" );
						CloseAll( hRequest, hConnect, hSession );
						return Finish( out, t0 );
					}

					// Build the request headers as one CRLF-joined block.
					std::wstring headers;
					for( std::size_t i = 0; i < req.headers.size(); ++i ) {
						headers += WidenUtf8( req.headers[i].first );
						headers += L": ";
						headers += WidenUtf8( req.headers[i].second );
						headers += L"\r\n";
					}

					BOOL ok = WinHttpSendRequest( hRequest,
						headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
						headers.empty() ? 0 : static_cast<DWORD>( -1L ),   // -1 => compute length
						req.body.empty() ? WINHTTP_NO_REQUEST_DATA
						                  : const_cast<char*>( req.body.data() ),
						static_cast<DWORD>( req.body.size() ),
						static_cast<DWORD>( req.body.size() ), 0 );
					if( !ok ) {
						out.error = WinErr( "WinHttpSendRequest" );
						CloseAll( hRequest, hConnect, hSession );
						return Finish( out, t0 );
					}

					if( !WinHttpReceiveResponse( hRequest, nullptr ) ) {
						out.error = WinErr( "WinHttpReceiveResponse" );
						CloseAll( hRequest, hConnect, hSession );
						return Finish( out, t0 );
					}

					DWORD statusCode = 0;
					DWORD scSize = sizeof( statusCode );
					if( WinHttpQueryHeaders( hRequest,
						WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
						WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &scSize, WINHTTP_NO_HEADER_INDEX ) ) {
						out.status = static_cast<long>( statusCode );
					} else {
						out.error = WinErr( "WinHttpQueryHeaders" );
					}

					// Drain the body.
					std::string body;
					for( ;; ) {
						DWORD avail = 0;
						if( !WinHttpQueryDataAvailable( hRequest, &avail ) ) {
							if( out.error.empty() ) out.error = WinErr( "WinHttpQueryDataAvailable" );
							break;
						}
						if( avail == 0 ) break;
						std::vector<char> buf( avail );
						DWORD read = 0;
						if( !WinHttpReadData( hRequest, buf.data(), avail, &read ) ) {
							if( out.error.empty() ) out.error = WinErr( "WinHttpReadData" );
							break;
						}
						if( read == 0 ) break;
						body.append( buf.data(), read );
					}
					out.body = body;

					CloseAll( hRequest, hConnect, hSession );
					return Finish( out, t0 );
				}

			private:
				static void CloseAll( HINTERNET a, HINTERNET b, HINTERNET c )
				{
					if( a ) WinHttpCloseHandle( a );
					if( b ) WinHttpCloseHandle( b );
					if( c ) WinHttpCloseHandle( c );
				}

				static ChatHttpResponse Finish( ChatHttpResponse out,
				                                std::chrono::steady_clock::time_point t0 )
				{
					out.elapsedMs = static_cast<long>(
						std::chrono::duration_cast<std::chrono::milliseconds>(
							std::chrono::steady_clock::now() - t0 ).count() );
					// A genuine transport error (a failed status query or a
					// read-drain error) means the round-trip FAILED even if a
					// partial status/body arrived -- force status 0 so the invariant
					// below demotes it to a header-free failure with an empty body,
					// rather than letting a truncated response masquerade as clean.
					if( !out.error.empty() ) out.status = 0;
					if( out.status == 0 && out.error.empty() )
						out.error = "no HTTP response received";
					// A transport failure never carries a body; a success never
					// carries a transport error.
					if( out.status == 0 ) out.body.clear();
					else                   out.error.clear();
					return out;
				}
			};
		}

		std::unique_ptr<IChatHttpTransport> CreateSystemChatHttpTransport()
		{
			return std::unique_ptr<IChatHttpTransport>( new WinHttpTransport() );
		}
	}
}
