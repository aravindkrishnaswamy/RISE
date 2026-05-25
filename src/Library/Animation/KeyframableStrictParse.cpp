//////////////////////////////////////////////////////////////////////
//
//  KeyframableStrictParse.cpp - implementation of ParseStrictScalar
//    and ParseStrictVec3 declared in KeyframableHelper.h.
//
//  Split out of KeyframableHelper.cpp on 2026-05-24 to fix
//  Windows LNK2005 on the Library project.  KeyframableHelper.cpp
//  is re-included into every TU on Windows via
//  `INLINE_TEMPLATE_SPECIALIZATIONS` (a 2003-era MSVC pattern that
//  keeps the explicit `Parameter<Vector3>` / `Parameter<Point3>`
//  specialisations COMDAT-foldable through inline visibility of
//  the primary template's in-class member bodies).  Plain free
//  functions in that .cpp therefore got emitted in every TU and
//  collided at link time:
//
//      RISE.lib(*.obj) : error LNK2005:
//          ParseStrictScalar already defined in RISE.lib(RISE_API.obj)
//
//  The Mac/Linux constraint is the opposite: commit 11575d8 dropped
//  `inline` from these functions because LTO elided the symbol from
//  librise.a, breaking out-of-tree consumers (Blender bridge dylib).
//
//  Hosting the bodies in their own TU satisfies both: one external
//  symbol exists in the archive (Mac/Linux LTO happy), and no other
//  TU emits a duplicate (Windows linker happy).  See header comment
//  on `ParseStrictScalar` for the -ffast-math defence-in-depth and
//  why these functions must NOT become `inline`.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "KeyframableHelper.h"

#include <cctype>

namespace RISE
{
	namespace Implementation
	{
		// Whitespace test that handles all four ASCII whitespace
		// characters strtod's leading-whitespace skip recognises.
		// `static` keeps the helper file-local so it doesn't leak as
		// an external symbol.
		static inline bool IsWhitespace( char ch )
		{
			return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
		}

		// Case-insensitive 3-byte match.  Guards against the input
		// being shorter than 3 bytes by null-terminator-checking each
		// position — the input is always null-terminated since it
		// comes from `String::c_str()`.  Without the guard, calls on
		// "", "+", or "i" would read past the terminator.
		static inline bool MatchCI3( const char* a, const char* lit )
		{
			for( int i = 0; i < 3; i++ ) {
				const char ch = a[i];
				if( ch == '\0' ) return false;
				int chL = static_cast<unsigned char>( ch );
				if( chL >= 'A' && chL <= 'Z' ) chL += 32;
				if( chL != lit[i] ) return false;
			}
			return true;
		}

		// See header for full rationale.  Three layers of defence
		// against `-ffast-math`:
		//   1. Textual reject of "nan" / "inf" before strtod.
		//   2. `volatile` on the parsed result to block re-derivation.
		//   3. Bit-pattern check via memcpy for overflow → ±Inf.
		// Also requires the entire input to parse — trailing non-
		// whitespace (e.g. "1abc", "1 2") is rejected since a scalar
		// field should be one number, not a prefix of a number.
		bool ParseStrictScalar( const String& s, Scalar& out )
		{
			const char* c = s.c_str();

			// Layer 1: textual reject.  Skip leading whitespace and
			// optional sign, then match `nan`/`inf` case-insensitive.
			// `strtod` recognises these spellings as valid input, so
			// rejecting them up-front is the only reliable path under
			// `-ffast-math` (which lets the compiler assume strtod's
			// result is finite and DCE any subsequent bit check).
			const char* p = c;
			while( IsWhitespace( *p ) ) ++p;
			if( *p == '+' || *p == '-' ) ++p;
			if( MatchCI3( p, "nan" ) || MatchCI3( p, "inf" ) ) {
				return false;
			}

			// Layer 2 + 3: parse, then check bits for overflow → ±Inf.
			char* end = nullptr;
			volatile double parsedV = std::strtod( c, &end );
			const double parsed = parsedV;
			if( end == c ) {
				return false;
			}
			// Trailing-junk reject: after the parse, only whitespace
			// may remain.  Without this, "1abc" parses to 1, "1 2"
			// parses to 1 — both surprising for a scalar field.
			while( IsWhitespace( *end ) ) ++end;
			if( *end != '\0' ) {
				return false;
			}
			uint64_t bits;
			std::memcpy( &bits, const_cast<const double*>( &parsed ), sizeof( bits ) );
			const uint64_t kExpMask = 0x7FF0000000000000ULL;
			if( ( bits & kExpMask ) == kExpMask ) {
				return false;
			}
			out = static_cast<Scalar>( parsed );
			return true;
		}

		// Strict 3-vector parse: tokenises on whitespace, requires
		// exactly three components, each of which goes through
		// ParseStrictScalar.  Closes the parallel hole on vec3 fields
		// (color, direction, target, position, orientation, scale)
		// that previously accepted "nan nan nan" via `sscanf("%lf
		// %lf %lf")`.
		bool ParseStrictVec3( const String& s, double out[3] )
		{
			const char* c = s.c_str();
			for( int i = 0; i < 3; i++ ) {
				while( IsWhitespace( *c ) ) ++c;
				if( *c == '\0' ) return false;

				const char* tokStart = c;
				while( *c != '\0' && !IsWhitespace( *c ) ) ++c;

				// RISE's String type lacks a (ptr, len) constructor;
				// route through std::string which has one, then pass
				// the null-terminated c_str into ParseStrictScalar.
				const std::string token( tokStart, static_cast<size_t>( c - tokStart ) );
				Scalar v;
				if( !ParseStrictScalar( String( token.c_str() ), v ) ) {
					return false;
				}
				out[i] = static_cast<double>( v );
			}
			// Anything after the third token must be whitespace only.
			while( IsWhitespace( *c ) ) ++c;
			if( *c != '\0' ) return false;
			return true;
		}
	}
}
