//////////////////////////////////////////////////////////////////////
//
//  AgentLoopbackHttpServer.cpp - see AgentLoopbackHttpServer.h.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
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
	// Windows CSPRNG (BCryptGenRandom) -- see GenerateBearerToken() below.
	// This #include and the code path it guards are written BY SYMMETRY
	// with the Darwin/Linux legs; this codebase does not compile the
	// Windows leg of this file today (see the Bind()'s WSAStartup comment
	// for the same posture on this file's Windows support), so this is an
	// OWED, not a VERIFIED, code path -- see this slice's final report.
	#include <bcrypt.h>
#elif defined( __APPLE__ ) || defined( __FreeBSD__ ) || defined( __OpenBSD__ ) || defined( __NetBSD__ )
	#include <unistd.h>
	#include <sys/types.h>
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <netinet/tcp.h>
	#include <arpa/inet.h>
	#include <errno.h>
	#include <stdlib.h>   // arc4random_buf -- see GenerateBearerToken() below
	typedef socklen_t socklen_t_compat;
	static const SOCKET kBadSocket = -1;
	#define RISE_AGENT_HTTP_HAVE_ARC4RANDOM 1
#else
	#include <unistd.h>
	#include <sys/types.h>
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <netinet/tcp.h>
	#include <arpa/inet.h>
	#include <errno.h>
	#include <cstdio>   // /dev/urandom read -- see GenerateBearerToken() below
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

	//! Largest single send() call SendAll will issue. send()'s length
	//! parameter is an `int` on both platforms' socket APIs, so handing it
	//! `data.size() - sent` directly (as this used to) truncates or goes
	//! negative once a body exceeds INT_MAX bytes -- unreachable today (the
	//! largest body this server ever builds/reads is bounded well under
	//! 1 MiB: kMaxBodyBytes caps the request side at 8 MiB, and no response
	//! this adapter emits -- including a base64 PNG -- gets remotely close
	//! to 2^31 bytes) but latent and cheap to close. Clamping every call to
	//! this chunk size keeps the cast to `int` always in-range regardless
	//! of how large `data` ever grows.
	const std::size_t kSendChunkBytes = 1u << 20;   // 1 MiB

	//! Sends the entirety of `data` on `sock`, looping over short writes
	//! (and over the chunk cap above for very large bodies). Returns false
	//! on any send() failure/timeout (the caller has nothing further to do
	//! but close -- there is no response-to-a-response in HTTP/1.1).
	bool SendAll( SOCKET sock, const std::string& data )
	{
		std::size_t sent = 0;
		while( sent < data.size() ) {
			const std::size_t remaining = data.size() - sent;
			const std::size_t toSend = remaining < kSendChunkBytes ? remaining : kSendChunkBytes;
			const int n = static_cast<int>( send( sock, data.data() + sent,
			                                       static_cast<int>( toSend ), 0 ) );
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
	//! carry scene text, a base64 PNG, or a bearer token/Authorization
	//! header value): this line has never included, and must never be
	//! changed to include, any header VALUE -- only method/path/status/
	//! sizes, so a REDACTED credential can never accidentally end up in
	//! RISE_Log.txt.
	void LogRequest( const std::string& method, const std::string& path,
	                  int status, std::size_t reqBytes, std::size_t respBytes )
	{
		GlobalLog()->PrintEx( eLog_Event,
			"agent-http: %s %s -> %d (req %zu bytes, resp %zu bytes)",
			method.c_str(), path.c_str(), status, reqBytes, respBytes );
	}

	//////////////////////////////////////////////////////////////////
	// Secure-MCP slice 4: per-launch bearer token + Origin/Host checks.
	//////////////////////////////////////////////////////////////////

	//! Number of raw random bytes behind the token (32 bytes = 256 bits
	//! of entropy, comfortably over the 128-bit floor this slice targets)
	//! -- hex-encoded below to 64 printable characters so it drops
	//! straight into an `Authorization: Bearer <token>` header with no
	//! escaping concerns.
	const std::size_t kTokenRawBytes = 32;

	//! Hex-encodes `raw` (lowercase, 2 chars per byte -- e.g. 32 bytes in,
	//! 64 hex chars out). Used only for the bearer token, never for
	//! anything logged.
	std::string HexEncode( const unsigned char* raw, std::size_t n )
	{
		static const char kHexDigits[] = "0123456789abcdef";
		std::string out;
		out.resize( n * 2 );
		for( std::size_t i = 0; i < n; ++i ) {
			out[i * 2]     = kHexDigits[( raw[i] >> 4 ) & 0x0F];
			out[i * 2 + 1] = kHexDigits[raw[i] & 0x0F];
		}
		return out;
	}

	//! Fills `raw[0..n)` with cryptographically-random bytes from the
	//! platform CSPRNG. FATAL (aborts the process) on any failure to
	//! obtain randomness -- there is no principled fallback: a "weak but
	//! available" RNG here would silently downgrade the entire slice-4
	//! security boundary to guessable, which is worse than refusing to
	//! start. This is the ONLY place in this file real randomness is
	//! sourced; nothing else in this codebase's agentic surface (the CST
	//! head-version counter, MEMORY.md's note on the epoch/uuid minting)
	//! uses a CSPRNG, so there was no existing in-tree helper to reuse.
	void FillCryptoRandom( unsigned char* raw, std::size_t n )
	{
#if defined( WIN32 )
		// BCryptGenRandom, the modern (Vista+) CNG CSPRNG entry point.
		// BCRYPT_USE_SYSTEM_PREFERRED_RNG selects the OS default algorithm
		// provider rather than opening/holding one explicitly -- the
		// standard idiom for a one-shot fill. Written by symmetry; this
		// codebase does not compile the Windows leg of this file today
		// (see the #include<bcrypt.h> comment above) -- OWED, not verified.
		const NTSTATUS status = BCryptGenRandom(
			nullptr, raw, static_cast<ULONG>( n ),
			BCRYPT_USE_SYSTEM_PREFERRED_RNG );
		if( status != 0 /* STATUS_SUCCESS */ ) {
			std::fprintf( stderr,
				"FATAL: rise --agent-http: BCryptGenRandom failed (status 0x%lx) -- "
				"cannot start the loopback HTTP transport without a real bearer "
				"token. Refusing to fall back to a weak RNG.\n",
				static_cast<unsigned long>( status ) );
			std::abort();
		}
#elif defined( RISE_AGENT_HTTP_HAVE_ARC4RANDOM )
		// Darwin/BSD: arc4random_buf is a CSPRNG, always available (no
		// seeding step, no open-file-descriptor failure mode, unlike
		// /dev/urandom) -- it cannot fail, hence no error path here.
		arc4random_buf( raw, n );
#else
		// Linux and other POSIX platforms without arc4random_buf: read
		// directly from /dev/urandom. Deliberately NOT /dev/random (which
		// can block for "true" entropy accounting that modern kernels
		// don't actually need for CSPRNG output) -- /dev/urandom has been
		// cryptographically sound for this purpose since the kernel's
		// CSPRNG is seeded (and Linux blocks /dev/urandom reads itself,
		// pre-5.x, only until first seeding at boot, long before a user
		// process like this one runs). A short read is retried; open
		// failure or a read that returns <=0 is FATAL -- there is no safe
		// fallback (see FillCryptoRandom's doc above).
		std::FILE* f = std::fopen( "/dev/urandom", "rb" );
		if( !f ) {
			std::fprintf( stderr,
				"FATAL: rise --agent-http: could not open /dev/urandom (%s) -- "
				"cannot start the loopback HTTP transport without a real bearer "
				"token. Refusing to fall back to a weak RNG.\n",
				std::strerror( errno ) );
			std::abort();
		}
		std::size_t got = 0;
		while( got < n ) {
			const std::size_t r = std::fread( raw + got, 1, n - got, f );
			if( r == 0 ) {
				std::fclose( f );
				std::fprintf( stderr,
					"FATAL: rise --agent-http: short/failed read from /dev/urandom -- "
					"cannot start the loopback HTTP transport without a real bearer "
					"token. Refusing to fall back to a weak RNG.\n" );
				std::abort();
			}
			got += r;
		}
		std::fclose( f );
#endif
	}

	//! Generates one fresh per-launch bearer token: kTokenRawBytes of
	//! CSPRNG output, hex-encoded. Called exactly once, from this server's
	//! constructor.
	std::string GenerateBearerToken()
	{
		unsigned char raw[kTokenRawBytes];
		FillCryptoRandom( raw, kTokenRawBytes );
		std::string token = HexEncode( raw, kTokenRawBytes );
		// Best-effort scrub of the raw bytes from the stack -- the hex
		// string is the only copy that needs to survive. Not a strong
		// guarantee (the compiler may still have left copies in registers/
		// spill slots), but cheap and removes the most obvious residue.
		std::memset( raw, 0, sizeof( raw ) );
		return token;
	}

	//! Constant-time byte-for-byte comparison: does NOT short-circuit on
	//! the first differing byte (unlike std::string::operator== or
	//! memcmp, both of which are permitted -- and in practice observed --
	//! to return as soon as a mismatch is found, leaking the length of
	//! the matching PREFIX through a timing side channel). Every byte of
	//! BOTH inputs is always touched; the result is accumulated via OR
	//! into a single `diff` value that is only inspected once, at the
	//! end, after the full loop has run.
	//!
	//! Length handling: if the lengths differ, this still walks the
	//! SHORTER length's worth of bytes (comparing `a` against itself is
	//! not an option -- there is nothing to compare a length mismatch
	//! against without reading past one buffer's end) and then folds
	//! `(lenA != lenB)` into `diff` too, so a length mismatch is treated
	//! as "differs" without an early return. That fold is a branch on
	//! LENGTH, never on byte CONTENT -- length is not the secret here:
	//! the token is a fixed 64 hex chars and an attacker already knows
	//! that from this very file/the startup banner's format, so which
	//! length bucket a guess falls into leaks nothing about the token's
	//! BYTES. (An earlier version of this fold packed `lenA ^ lenB`'s low
	//! 16 bits into `diff` instead of a plain `!=` -- correct for the
	//! lengths this server ever actually sees today since colliding on a
	//! multiple of 65536 requires a length past kMaxHeaderBytes, which
	//! 431s before this function is even reached, but WRONG in general:
	//! two lengths exactly 65536 apart XOR to a value whose low 16 bits
	//! are zero, folding away to "equal" for the length check. `!=`
	//! yields a plain 0/1 with no such blind spot, for any lengths,
	//! forever.) This is the standard constant-time-compare idiom
	//! (matches libsodium's sodium_memcmp / OpenSSL's CRYPTO_memcmp
	//! shape); documented here rather than silently trusted per this
	//! slice's requirement to spell out the exact guarantee.
	bool ConstantTimeEquals( const std::string& a, const std::string& b )
	{
		const std::size_t lenA = a.size();
		const std::size_t lenB = b.size();
		const std::size_t minLen = ( lenA < lenB ) ? lenA : lenB;

		volatile unsigned char diff = 0;
		for( std::size_t i = 0; i < minLen; ++i ) {
			diff |= static_cast<unsigned char>( a[i] ) ^ static_cast<unsigned char>( b[i] );
		}
		// Fold the length difference in too (still no early return, no
		// branch on a per-byte comparison result -- see the doc comment
		// above for why a length-only `!=` is sound here for ANY pair of
		// lengths, not just ones within kMaxHeaderBytes of each other).
		diff |= static_cast<unsigned char>( lenA != lenB );

		return diff == 0;
	}

	//! Case-insensitive ASCII match, used for the header NAME lookups
	//! below (HTTP header names are case-insensitive per RFC 7230 §3.2)
	//! and for the "Bearer" scheme token (RFC 7235 §2.1 says the scheme
	//! name is compared case-insensitively; the credential itself is
	//! compared exactly, via ConstantTimeEquals above).
	bool EqualsIgnoreCase( const std::string& a, const std::string& b )
	{
		if( a.size() != b.size() ) return false;
		for( std::size_t i = 0; i < a.size(); ++i ) {
			if( std::tolower( static_cast<unsigned char>( a[i] ) ) !=
			    std::tolower( static_cast<unsigned char>( b[i] ) ) ) return false;
		}
		return true;
	}

	//! Looks up header `name` (case-insensitive) in the raw header block
	//! `head` (the same "METHOD path HTTP/1.1\r\nHeader: value\r\n...\r\n\r\n"
	//! text ServeOneConnection already parses Content-Length out of).
	//! Returns true and fills `outValue` (trimmed of leading/trailing
	//! whitespace, matching the existing Content-Length trim logic) on
	//! the FIRST matching header line; false if absent. Multiple
	//! occurrences of Authorization/Origin/Host are not a smuggling
	//! vector the way duplicate Content-Length is (this server doesn't
	//! sit behind a proxy that could disagree on which one to honour), so
	//! -- unlike Content-Length -- this does not reject on a duplicate;
	//! it simply uses the first.
	bool FindHeader( const std::string& head, const std::string& name, std::string& outValue )
	{
		const std::size_t firstCrlf = head.find( "\r\n" );
		std::size_t lineStart = firstCrlf;
		while( lineStart != std::string::npos && lineStart + 2 <= head.size() ) {
			lineStart += 2;
			const std::size_t lineEnd = head.find( "\r\n", lineStart );
			if( lineEnd == std::string::npos ) break;
			const std::string hline = head.substr( lineStart, lineEnd - lineStart );
			if( hline.empty() ) break;   // the blank line before the body

			const std::size_t colon = hline.find( ':' );
			if( colon != std::string::npos ) {
				const std::string key = hline.substr( 0, colon );
				if( EqualsIgnoreCase( key, name ) ) {
					std::string val = hline.substr( colon + 1 );
					const std::size_t a = val.find_first_not_of( " \t" );
					const std::size_t b = val.find_last_not_of( " \t\r" );
					outValue = ( a == std::string::npos ) ? std::string() : val.substr( a, b - a + 1 );
					return true;
				}
			}
			lineStart = lineEnd;
		}
		return false;
	}

	//! True iff every character in `s` is an ASCII digit and `s` is
	//! non-empty. Used below to validate a Host/Origin ":<port>" suffix
	//! -- the port's numeric value is never actually parsed here (the
	//! caller already knows the port it bound; this function is purely
	//! an allow/reject gate), just checked for syntax, so trailing
	//! garbage like ":notaport" or ":8765extra" is rejected rather than
	//! silently truncated at the first colon.
	bool IsAllDigits( const std::string& s )
	{
		if( s.empty() ) return false;
		for( std::size_t i = 0; i < s.size(); ++i ) {
			if( !std::isdigit( static_cast<unsigned char>( s[i] ) ) ) return false;
		}
		return true;
	}

	//! True iff `host` (the value of a Host header, e.g. "127.0.0.1:9000"
	//! or "localhost") names loopback -- with or without a trailing
	//! ":<port>". This is the anti-DNS-rebinding Host check: a rebind
	//! attack resolves an attacker-controlled DOMAIN name to 127.0.0.1,
	//! but the browser still sends THAT domain as the Host header value
	//! (Host reflects the URL the page navigated to, not the resolved
	//! IP) -- so accepting only the literal loopback names here rejects
	//! the rebind even though the TCP connection itself did land on
	//! 127.0.0.1.
	//!
	//! Host-label parsing: a bracketed IPv6 literal `[::1]:port` puts its
	//! own colons INSIDE the brackets, so "strip everything from the
	//! first colon" truncates it to a single `[` -- wrong. Handle the
	//! bracketed form explicitly (find the closing `]`, the port -- if
	//! any -- starts after a colon immediately following it); otherwise
	//! this is the bare `host[:port]` form, where a SINGLE colon (if any)
	//! separates host from port (a bare, unbracketed `::1` has embedded
	//! colons too -- see the dedicated bare-`::1` check below, which
	//! matches the label before deciding "everything after the first
	//! colon must be a port").
	//!
	//! Bind() only ever opens an AF_INET (IPv4) listener -- there is no
	//! IPv6 listen path in this server today -- so a Host of `[::1]` or
	//! `::1` can never actually be this same connection's own address;
	//! this branch is defensive/future-proofing for if an IPv6 listener
	//! is ever added, not a live path.
	//!
	//! "localhost" is matched case-INSENSITIVELY (RFC 7230 host names
	//! are case-insensitive; `Host: Localhost` / `LOCALHOST` are
	//! legitimate loopback references, not a spoof) via EqualsIgnoreCase,
	//! the same helper used for header names and the Bearer scheme.
	//! "127.0.0.1" and the IPv6 literal forms are digits/colons only, so
	//! case does not apply to them.
	//!
	//! Any trailing garbage after the host[:port] is rejected outright --
	//! a bracketed literal must be followed by nothing or exactly
	//! ":<all-digit port>", and a bare host's optional ":<port>" suffix
	//! must likewise be all-digit with nothing trailing. This closes
	//! malformed forms like "[::1]evil", "[::1]:notaport", and
	//! "127.0.0.1:PORT.evil.com" that a naive "find the first colon /
	//! closing bracket" parse would otherwise silently accept.
	bool IsLoopbackHost( const std::string& host )
	{
		std::string hostOnly = host;

		if( !hostOnly.empty() && hostOnly[0] == '[' ) {
			// Bracketed literal: "[<addr>]" optionally followed by ":<port>".
			const std::size_t closeBracket = hostOnly.find( ']' );
			if( closeBracket == std::string::npos ) return false;   // malformed -- no closing bracket
			const std::string addr = hostOnly.substr( 1, closeBracket - 1 );   // the address between the brackets
			if( addr != "::1" ) return false;

			// Everything after the closing bracket must be either
			// nothing at all, or ":<all-digit port>" with nothing
			// trailing -- reject "[::1]evil", "[::1]]", "[::1]:",
			// "[::1]:notaport", "[::1]:8765extra".
			const std::string trailer = hostOnly.substr( closeBracket + 1 );
			if( trailer.empty() ) return true;
			if( trailer[0] != ':' ) return false;
			return IsAllDigits( trailer.substr( 1 ) );
		}

		// Bare form. A bare IPv6 "::1" contains colons that are NOT a
		// port separator, so check for it (exactly, no port possible in
		// unbracketed form -- RFC 3986 requires brackets around an IPv6
		// host precisely to make a trailing ":port" unambiguous) before
		// assuming a colon means "host:port".
		if( hostOnly == "::1" ) return true;

		const std::size_t colon = hostOnly.find( ':' );
		if( colon != std::string::npos ) {
			// Whatever follows the (single, host-terminating) colon must
			// be an all-digit, non-empty port with nothing trailing --
			// reject "127.0.0.1:", "127.0.0.1:notaport",
			// "127.0.0.1:8765extra", "127.0.0.1:PORT.evil.com".
			if( !IsAllDigits( hostOnly.substr( colon + 1 ) ) ) return false;
			hostOnly = hostOnly.substr( 0, colon );
		}

		return hostOnly == "127.0.0.1" || EqualsIgnoreCase( hostOnly, "localhost" );
	}

	//! True iff `origin` (the value of an Origin header, e.g.
	//! "http://127.0.0.1:9000" or "null") is an allowed loopback origin.
	//! Per the MCP spec's anti-DNS-rebinding guidance: a non-browser MCP
	//! client typically sends NO Origin header at all (handled by the
	//! caller -- absence is accepted independently of this function); a
	//! BROWSER tab's fetch/XHR to 127.0.0.1 always sends an Origin equal
	//! to the PAGE's own origin (the attacking page, e.g.
	//! "http://evil.com", in a DNS-rebinding attempt) -- so the only
	//! Origins this allows are schemes/hosts that are themselves
	//! loopback: http(s)://127.0.0.1[:port], http(s)://localhost[:port],
	//! http(s)://[::1][:port]. Anything else (including "null", which a
	//! sandboxed iframe or a local `file://` page can send) is rejected.
	bool IsAllowedLoopbackOrigin( const std::string& origin )
	{
		// Strip the "scheme://" prefix if present so the host-matching
		// logic can be shared with IsLoopbackHost. An Origin header is
		// always "scheme://host[:port]" (never a bare host) per the Fetch
		// spec, but this is defensive rather than assumed.
		const std::size_t schemeEnd = origin.find( "://" );
		if( schemeEnd == std::string::npos ) return false;   // e.g. "null" -- rejected
		const std::string scheme = origin.substr( 0, schemeEnd );
		if( !EqualsIgnoreCase( scheme, "http" ) && !EqualsIgnoreCase( scheme, "https" ) ) return false;

		const std::string hostAndPort = origin.substr( schemeEnd + 3 );
		return IsLoopbackHost( hostAndPort );
	}
}

bool AgentLoopbackHttpServer::ForTest_ConstantTimeEquals( const std::string& a, const std::string& b )
{
	return ConstantTimeEquals( a, b );
}

AgentLoopbackHttpServer::AgentLoopbackHttpServer( AgentMcpAdapter* adapter, const std::string& path )
	: mAdapter( adapter )
	, mPath( path )
	, mBearerToken( GenerateBearerToken() )   // per-launch CSPRNG token -- see GenerateBearerToken()'s doc
	, mListenSock( kBadSocket )   // std::atomic<SOCKET> -- see the .h's doc on the Stop()/Serve() shutdown race this guards
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
	// Winsock requires WSAStartup before the FIRST socket()/bind() call in
	// the process, or every Winsock call fails outright -- every other
	// in-tree socket consumer (prise_worker.cpp, drise_server.cpp, ...)
	// calls SocketComm::InitializeSocketCommunications() (which wraps this
	// same WSAStartup call, see SocketCommunications.cpp) before its first
	// socket use, for exactly this reason. This server had no such call
	// anywhere on its path (Bind() nor RunAgentHttp/main in
	// commandconsole.cpp), so the Windows leg was dead on arrival -- the
	// very first socket() call below would fail with an uninitialized-
	// Winsock error. This class already writes its own self-contained
	// socket code rather than reusing SocketComm's helpers (see the bind()
	// call's comment below on why EstablishConnection isn't reused here),
	// and SocketCommunications.cpp is not part of librise's own object
	// list (it lives under SRCDRISE in this tree's Filelist -- the DRISE
	// distributed-render source list, built all-platforms for
	// drise_server/drise_client/drise_submitter, NOT Windows-only -- but
	// never linked into librise.a, which is what this agentic-surface code
	// ships in), so this calls WSAStartup directly rather than adding a
	// cross-target Filelist dependency for one function. WSAStartup is
	// refcounted by the OS (a matching WSACleanup is only owed if this
	// process pairs it with a shutdown call, which -- matching
	// prise_worker/drise_server's own posture of never calling
	// CloseSocketCommunications() either -- it does not), so calling this
	// once per Bind() is safe even if some other part of the process
	// already initialized Winsock. This branch is written by symmetry with
	// SocketCommunications.cpp's Windows leg; this codebase does not
	// compile the Windows leg here today, so the call is owed a Windows
	// compile check (see this slice's final report).
	WSADATA wsaData;
	const WORD wsaVersion = MAKEWORD( 2, 0 );
	if( WSAStartup( wsaVersion, &wsaData ) != 0 ) return false;
#endif

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
#ifdef WIN32
		mBoundAddress = "127.0.0.1";
#else
		char addrBuf[INET_ADDRSTRLEN] = {0};
		inet_ntop( AF_INET, &bound.sin_addr, addrBuf, sizeof( addrBuf ) );
		mBoundAddress = addrBuf;
#endif
	} else {
		mBoundPort = port;
		mBoundAddress.clear();
	}

	mListenSock.store( s, std::memory_order_release );
	mStopRequested.store( false, std::memory_order_release );
	return true;
}

bool AgentLoopbackHttpServer::IsBound() const
{
	return mListenSock.load( std::memory_order_acquire ) != kBadSocket;
}

void AgentLoopbackHttpServer::Stop()
{
	mStopRequested.store( true, std::memory_order_release );

	// Shutdown-race note (this is the fix for the TSan-confirmed data race
	// between this method and Serve()'s loop-guard + accept(), both of
	// which read mListenSock from the OTHER thread): close the fd FIRST,
	// THEN store kBadSocket. Serve() loads mListenSock ONCE per iteration
	// into a local, so whichever value it happens to observe is always a
	// COMPLETE, valid one -- either the still-live fd (in which case the
	// close() that just happened on it is exactly what unblocks a thread
	// parked in accept() on that same fd on POSIX) or kBadSocket (in which
	// case Serve()'s guard exits the loop before calling accept() at all).
	// There is no ordering of the two stores/loads that lets Serve() see a
	// torn value or call accept() on an already-closed-and-reused
	// descriptor, because the close() always happens-before the
	// kBadSocket store, and mListenSock is never written anywhere else.
	const SOCKET toClose = mListenSock.load( std::memory_order_acquire );
	if( toClose != kBadSocket ) {
		// Closing the listen socket unblocks a thread parked in accept()
		// on POSIX. (On Windows an in-progress WSAAccept can behave
		// differently across versions -- see the Windows owed-check note
		// in this slice's final report; this codebase does not compile
		// the Windows leg today.)
		CloseSock( toClose );
		mListenSock.store( kBadSocket, std::memory_order_release );
	}
}

void AgentLoopbackHttpServer::Serve()
{
	while( !mStopRequested.load( std::memory_order_acquire ) ) {
		// Load ONCE per iteration into a local: the guard check and the
		// accept() call below both use this same local SOCKET, never a
		// second read of the atomic -- so a concurrent Stop() landing
		// between the guard and the accept() call cannot hand this
		// iteration two different values (see the atomic's doc in the .h
		// and Stop()'s close-then-store ordering above).
		const SOCKET listenSock = mListenSock.load( std::memory_order_acquire );
		if( listenSock == kBadSocket ) break;

#ifdef WIN32
		SOCKET client = accept( listenSock, nullptr, nullptr );
		if( client == INVALID_SOCKET ) {
#else
		SOCKET client = accept( listenSock, nullptr, nullptr );
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
	// kMaxHeaderBytes (64 KiB), NOT kMaxBodyBytes (8 MiB) -- a dedicated,
	// much smaller cap for the header block. Red-prove: before this fix
	// the header-read loop reused kMaxBodyBytes, so a client sending 128 KB
	// of header-shaped bytes with no terminating blank line was accepted
	// (kept reading) for up to 8 MiB before rejection; a request whose
	// header block alone exceeds 64 KiB is malformed by any realistic
	// client this server expects (a same-machine MCP client) and is now
	// rejected promptly with 431.
	if( !ReadUntilDoubleCrlf( client, kMaxHeaderBytes, head, spill ) ) {
		// Malformed / oversized headers, or the peer vanished before
		// sending a complete header block. Nothing sane to respond with
		// (we don't even reliably know it's HTTP) -- close cleanly. 431 is
		// the correct HTTP status for "header block too large" (RFC 6585
		// §5); a short-read/timeout/peer-close ahead of the cap is
		// indistinguishable from an oversized header at this call site, so
		// both are reported the same way (both are "never got a usable
		// header block").
		const std::string resp = PlainError( "431 Request Header Fields Too Large", "malformed, incomplete, or oversized request headers" );
		SendAll( client, resp );
		LogRequest( "?", "?", 431, head.size(), resp.size() );
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

	// ---- Parse headers for Content-Length -- PLUS two request-smuggling preconditions per RFC 7230
	// §3.3.3: a request MUST NOT carry more than one Content-Length, and
	// MUST NOT carry a Transfer-Encoding header at all (this server never
	// advertises or accepts chunked encoding, so a client sending one is
	// either confused or attempting request smuggling against a proxy
	// this server might one day sit behind). Neither is exploitable
	// TODAY -- there is no proxy in front of this loopback-only server --
	// but closing both preconditions now is cheap and removes the
	// precondition for good. ----
	long long contentLength = -1;
	int contentLengthCount = 0;
	bool hasTransferEncoding = false;
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
				if( key == "transfer-encoding" ) {
					hasTransferEncoding = true;
				}
				if( key == "content-length" ) {
					++contentLengthCount;
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

	if( hasTransferEncoding ) {
		// This server never sends or accepts chunked transfer coding --
		// ANY Transfer-Encoding header is out of scope, so reject rather
		// than silently ignoring it (ignoring it is exactly the ambiguity
		// a smuggling attack against a front proxy would exploit: the
		// proxy honouring Transfer-Encoding while this backend honours
		// Content-Length instead).
		const std::string resp = PlainError( "400 Bad Request", "Transfer-Encoding is not supported" );
		SendAll( client, resp );
		LogRequest( method, path, 400, head.size(), resp.size() );
		return;
	}
	if( contentLengthCount > 1 ) {
		// RFC 7230 §3.3.3: a request with multiple Content-Length header
		// fields (even if all agree numerically) is invalid and MUST be
		// rejected -- the classic request-smuggling precondition is a
		// front-end and back-end disagreeing on WHICH of several
		// Content-Length values to honour. This server used to silently
		// take "last one wins"; that is now a hard 400 instead.
		const std::string resp = PlainError( "400 Bad Request", "duplicate Content-Length header" );
		SendAll( client, resp );
		LogRequest( method, path, 400, head.size(), resp.size() );
		return;
	}

	// ---- Secure-MCP slice 4: Host -> Origin -> Auth, cheapest/fail-closed
	// first, all BEFORE method/path dispatch and BEFORE any body byte is
	// read. Each rejection is a distinct, clean HTTP status; none echoes
	// the expected token or the reason the check chose (beyond the fixed,
	// static message below) -- there is no oracle here for an attacker
	// without the token, since none of these three checks can be passed
	// without EITHER already knowing the token (irrelevant to Host/Origin)
	// or already being a legitimate loopback, non-rebound, same-origin
	// caller.
	//
	// (1) Host check: a DNS-rebinding attack points a hostile domain's DNS
	// record at 127.0.0.1 and relies on the browser sending that domain
	// name as Host -- reject any Host that isn't 127.0.0.1/localhost/::1
	// (with or without a port). A request with NO Host header at all
	// (technically invalid HTTP/1.1, but this is a hand-rolled parser, not
	// a strict-mode one) is treated the same as an absent check would be
	// for a non-browser client -- there's nothing to validate, so it is
	// NOT rejected here (that would break a minimal raw-socket MCP client
	// that omits Host, which many hand-rolled test/debug clients do); the
	// security-relevant case is a rebind attack, which always DOES send a
	// (wrong) Host.
	{
		std::string hostValue;
		if( FindHeader( head, "Host", hostValue ) && !IsLoopbackHost( hostValue ) ) {
			const std::string resp = PlainError( "403 Forbidden", "Host header does not name loopback" );
			SendAll( client, resp );
			LogRequest( method, path, 403, head.size(), resp.size() );
			return;
		}
	}

	// (2) Origin check: per the MCP spec's anti-DNS-rebinding guidance, if
	// an Origin header is present at all it must be an explicit loopback
	// origin. A non-browser MCP client (curl, a raw socket, a CLI tool)
	// never sends Origin -- absent passes unconditionally. A BROWSER tab's
	// fetch/XHR to 127.0.0.1 ALWAYS sends Origin equal to the calling
	// page's own origin -- for a DNS-rebinding attack that is the
	// attacker's page origin (e.g. "http://evil.com"), which is not a
	// loopback origin and is rejected here regardless of whether the
	// request also carries a valid bearer token (this check is
	// independent of auth by design -- see section (k) of
	// AgentLoopbackHttpTest.cpp).
	{
		std::string originValue;
		if( FindHeader( head, "Origin", originValue ) && !IsAllowedLoopbackOrigin( originValue ) ) {
			const std::string resp = PlainError( "403 Forbidden", "Origin is not an allowed loopback origin" );
			SendAll( client, resp );
			LogRequest( method, path, 403, head.size(), resp.size() );
			return;
		}
	}

	// (3) Bearer-token auth: every request MUST carry
	// `Authorization: Bearer <token>` matching this server's per-launch
	// token (see GenerateBearerToken()). The scheme name ("Bearer") is
	// matched case-insensitively per RFC 7235 SS2.1; the token itself is
	// compared with ConstantTimeEquals, never a short-circuiting
	// operator==/memcmp. Absent header, wrong scheme, or wrong token are
	// all folded into the same 401 -- distinguishing them would tell an
	// attacker WHICH part of their guess was wrong, which is exactly the
	// oracle a constant-time compare is trying to deny elsewhere in this
	// same check.
	{
		std::string authValue;
		bool authOk = false;
		if( FindHeader( head, "Authorization", authValue ) ) {
			const std::string kSchemePrefix = "Bearer ";
			if( authValue.size() > kSchemePrefix.size() &&
			    EqualsIgnoreCase( authValue.substr( 0, kSchemePrefix.size() ), kSchemePrefix ) ) {
				const std::string presentedToken = authValue.substr( kSchemePrefix.size() );
				authOk = ConstantTimeEquals( presentedToken, mBearerToken );
			}
		}
		if( !authOk ) {
			std::string resp = BuildHttpResponse( "401 Unauthorized", "text/plain", "missing or invalid bearer token" );
			// WWW-Authenticate is the standard companion header to a 401
			// (RFC 7235 SS4.1) -- inserted between the status line and the
			// fixed Content-Type header BuildHttpResponse already wrote, by
			// rebuilding rather than by adding a WWW-Authenticate parameter
			// to BuildHttpResponse itself (this is the only response in this
			// file that needs it, so a bespoke helper isn't warranted).
			const std::string marker = "\r\nContent-Type:";
			const std::size_t pos = resp.find( marker );
			if( pos != std::string::npos ) {
				resp.insert( pos, "\r\nWWW-Authenticate: Bearer" );
			}
			SendAll( client, resp );
			LogRequest( method, path, 401, head.size(), resp.size() );
			return;
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
