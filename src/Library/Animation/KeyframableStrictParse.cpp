//////////////////////////////////////////////////////////////////////
//
//  KeyframableStrictParse.cpp - implementation of ParseStrictScalar
//    and strict vector parsers declared in KeyframableHelper.h.
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
#include "../Utilities/FiniteMath.h"

#include <algorithm>
#include <array>
#include <string>

namespace RISE
{
	namespace Implementation
	{
		// Whitespace test that handles all six ASCII whitespace
		// characters strtod's leading-whitespace skip recognises.
		// `static` keeps the helper file-local so it doesn't leak as
		// an external symbol.
		static inline bool IsWhitespace( char ch )
		{
			return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f';
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

		// See header for full rationale.  Two layers of defence
		// against `-ffast-math`:
		//   1. Textual reject of "nan" / "inf" before strtod.
		//   2. The shared volatile-materialised predicate catches an
		//      overflow to +/-Inf.
		// Also requires the entire input to parse — trailing non-
		// whitespace (e.g. "1abc", "1 2") is rejected since a scalar
		// field should be one number, not a prefix of a number.
		static bool ParseScalarText( const char* c, Scalar& out )
		{

			// Layer 1: textual reject.  Skip leading whitespace and
			// optional sign, then match `nan`/`inf` case-insensitive.
			// `strtod` recognises these spellings as valid input, so
			// rejecting them up-front at the STRING layer is immune to any FP
			// flag and to per-site inlining, so it remains the right design.
			// It was historically the ONLY reliable path, under bare
			// `-ffast-math`, which let the compiler assume strtod's result was
			// finite and DCE any subsequent bit check; macOS pairs
			// `-fno-finite-math-only` since 2026-07-29, so a value-level check
			// would work too -- the string layer is still preferred.
			const char* p = c;
			while( IsWhitespace( *p ) ) ++p;
			if( *p == '+' || *p == '-' ) ++p;
			if( MatchCI3( p, "nan" ) || MatchCI3( p, "inf" ) ) {
				return false;
			}

			// Layer 2: parse, then reject an overflow to +/-Inf.
			char* end = nullptr;
			const double parsed = std::strtod( c, &end );
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
			if( !RISE::IsFiniteDouble( parsed ) ) {
				return false;
			}
			out = static_cast<Scalar>( parsed );
			return true;
		}

		bool ParseStrictScalar( const String& s, Scalar& out )
		{
			return ParseScalarText( s.c_str(), out );
		}

		template<size_t N>
		static bool ParseStrictVector( const String& s, double* out )
		{
			std::array<double, N> components;
			const char* c = s.c_str();
			for( double& component : components ) {
				while( IsWhitespace( *c ) ) ++c;
				if( *c == '\0' ) return false;
				const char* start = c;
				while( *c != '\0' && !IsWhitespace( *c ) ) ++c;
				const std::string token( start, static_cast<size_t>( c - start ) );
				if( !ParseScalarText( token.c_str(), component ) ) return false;
			}
			while( IsWhitespace( *c ) ) ++c;
			if( *c != '\0' ) return false;
			std::copy( components.begin(), components.end(), out );
			return true;
		}

		bool ParseStrictVec2( const String& s, double out[2] )
		{
			return ParseStrictVector<2>( s, out );
		}

		bool ParseStrictVec3( const String& s, double out[3] )
		{
			return ParseStrictVector<3>( s, out );
		}
	}
}
