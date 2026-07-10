//////////////////////////////////////////////////////////////////////
//
//  TlsTransportMac.mm - the macOS platform-HTTPS leg of the live eval
//    runner (Eval-harness slice E4).  NSURLSession over an ephemeral
//    session, driven SYNCHRONOUSLY: a dispatch_semaphore blocks the
//    calling thread on the completion handler.  See ChatHttpTransport.h
//    for the interface + hygiene contract.
//
//    Why NSURLSession (ObjC++), not pure-C CFNetwork: the CFHTTPStream
//    synchronous-request C API (CFReadStreamCreateForHTTPRequest et al.)
//    is deprecated since macOS 10.11 and warns under -Wall
//    (-Wdeprecated-declarations); RISE treats warnings as bugs and
//    forbids -Wno-*/pragma suppression, so NSURLSession -- Apple's stated
//    replacement -- is the warning-clean path (plan §5, decision 1).
//
//    This TU compiles ONLY in the macOS builds (make with the
//    IS_MACOS_BUILD gate; the Xcode project).  The make build compiles it
//    via a `%.o : %.mm` suffix rule with -x objective-c++ -fobjc-arc, so
//    ARC owns the ObjC object lifetimes here -- there is no manual
//    retain/release.  Foundation is linked via `-framework Foundation`
//    (Config.OSX LDLIBS).
//
//    KEY HYGIENE: on an NSError, the error string is a HEADER-FREE
//    category built from the error's domain + numeric code + localized
//    reason.  NSURLError's localizedDescription is a human sentence ("The
//    request timed out.", "A server with the specified hostname could not
//    be found.") -- it NEVER echoes the request or its headers, so the api
//    key cannot leak through it.  The request headers (which carry the
//    key) are never stringified into the error.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#import <Foundation/Foundation.h>

#include "ChatHttpTransport.h"

#include <chrono>
#include <string>

namespace RISE
{
	namespace Agent
	{
		namespace
		{
			//! Matches the GUI drivers' 300s request timeout (the plan's
			//! documented driver behaviour).  A single POST to an LLM
			//! endpoint can legitimately take a couple of minutes on a long
			//! generation, so this is deliberately generous.
			const NSTimeInterval kRequestTimeoutSeconds = 300.0;

			class MacUrlSessionTransport : public IChatHttpTransport
			{
			public:
				ChatHttpResponse Post( const ChatHttpRequest& req ) override
				{
					ChatHttpResponse out;
					const auto t0 = std::chrono::steady_clock::now();

					@autoreleasepool {
						NSString* urlStr = [NSString stringWithUTF8String:req.url.c_str()];
						NSURL* url = urlStr ? [NSURL URLWithString:urlStr] : nil;
						if( !url ) {
							out.error = "invalid request URL";
							out.status = 0;
							out.elapsedMs = ElapsedMs( t0 );
							return out;
						}

						NSMutableURLRequest* r = [NSMutableURLRequest requestWithURL:url];
						[r setHTTPMethod:@"POST"];
						for( std::size_t i = 0; i < req.headers.size(); ++i ) {
							NSString* name = [NSString stringWithUTF8String:req.headers[i].first.c_str()];
							NSString* val  = [NSString stringWithUTF8String:req.headers[i].second.c_str()];
							if( name && val ) [r setValue:val forHTTPHeaderField:name];
						}
						[r setHTTPBody:[NSData dataWithBytes:req.body.data()
						                              length:static_cast<NSUInteger>( req.body.size() )]];
						[r setTimeoutInterval:kRequestTimeoutSeconds];

						// Ephemeral: no on-disk cache / cookie jar / credential
						// store -- nothing about this request (least of all the
						// auth header) is persisted anywhere.
						NSURLSessionConfiguration* cfg =
							[NSURLSessionConfiguration ephemeralSessionConfiguration];
						cfg.timeoutIntervalForRequest = kRequestTimeoutSeconds;
						NSURLSession* session = [NSURLSession sessionWithConfiguration:cfg];

						dispatch_semaphore_t sem = dispatch_semaphore_create( 0 );
						__block long        blkStatus = 0;
						__block std::string blkBody;
						__block std::string blkError;

						NSURLSessionDataTask* task = [session dataTaskWithRequest:r
							completionHandler:^( NSData* data, NSURLResponse* resp, NSError* error ) {
								if( [resp isKindOfClass:[NSHTTPURLResponse class]] )
									blkStatus = static_cast<long>( [(NSHTTPURLResponse*)resp statusCode] );
								if( data && [data length] > 0 )
									blkBody.assign( reinterpret_cast<const char*>( [data bytes] ),
									                static_cast<std::size_t>( [data length] ) );
								if( error )
									blkError = HeaderFreeErrorString( error );
								dispatch_semaphore_signal( sem );
							}];
						[task resume];
						dispatch_semaphore_wait( sem, DISPATCH_TIME_FOREVER );

						out.status = blkStatus;
						out.body   = blkBody;
						out.error  = blkError;

						// A completed HTTP round-trip has status > 0 (even a 4xx/5xx
						// is a real response the loop/codec should interpret).  No
						// status AND no explicit NSError is an honest "nothing came
						// back" failure -- surface it rather than silently returning
						// a 0/empty success.
						if( out.status == 0 && out.error.empty() )
							out.error = "no HTTP response received";

						// Invariant: a transport FAILURE (status 0) never carries a
						// body, and a SUCCESS never carries a transport error.
						if( out.status == 0 ) out.body.clear();
						else                   out.error.clear();
					}

					out.elapsedMs = ElapsedMs( t0 );
					return out;
				}

			private:
				static long ElapsedMs( std::chrono::steady_clock::time_point t0 )
				{
					return static_cast<long>(
						std::chrono::duration_cast<std::chrono::milliseconds>(
							std::chrono::steady_clock::now() - t0 ).count() );
				}

				//! Build a header-free category string from an NSError.  Uses
				//! ONLY the error's domain, numeric code, and localized reason
				//! -- none of which contain the request headers/api key.
				static std::string HeaderFreeErrorString( NSError* error )
				{
					std::string e = "transport error";
					NSString* dom = [error domain];
					if( dom ) {
						e += " [";
						e += [dom UTF8String];
						e += " ";
						e += std::to_string( static_cast<long>( [error code] ) );
						e += "]";
					}
					NSString* desc = [error localizedDescription];
					if( desc && [desc length] > 0 ) {
						e += ": ";
						e += [desc UTF8String];
					}
					return e;
				}
			};
		}

		std::unique_ptr<IChatHttpTransport> CreateSystemChatHttpTransport()
		{
			return std::unique_ptr<IChatHttpTransport>( new MacUrlSessionTransport() );
		}
	}
}
