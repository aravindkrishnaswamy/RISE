//////////////////////////////////////////////////////////////////////
//
//  KeyframableHelper.cpp - Implementation of template specialized classes
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 31, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "KeyframableHelper.h"

#include <cctype>

namespace RISE
{
	namespace Implementation
	{
		// See header for full rationale.  Three layers of defence
		// against `-ffast-math`:
		//   1. Textual reject of "nan" / "inf" before strtod.
		//   2. `volatile` on the parsed result to block re-derivation.
		//   3. Bit-pattern check via memcpy for overflow → ±Inf.
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
			while( *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ) ++p;
			if( *p == '+' || *p == '-' ) ++p;
			auto matchCI3 = []( const char* a, const char* lit ) -> bool {
				for( int i = 0; i < 3; i++ ) {
					int ch = static_cast<unsigned char>( a[i] );
					if( ch >= 'A' && ch <= 'Z' ) ch += 32;
					if( ch != lit[i] ) return false;
				}
				return true;
			};
			if( matchCI3( p, "nan" ) || matchCI3( p, "inf" ) ) {
				return false;
			}

			// Layer 2 + 3: parse, then check bits for overflow → ±Inf.
			char* end = nullptr;
			volatile double parsedV = std::strtod( c, &end );
			const double parsed = parsedV;
			if( end == c ) {
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

		/////////////////////////////
		// Vector3 specializations
		/////////////////////////////

		template<>
		void Parameter<Vector3>::Add( IKeyframeParameter& result, const IKeyframeParameter& a_, const IKeyframeParameter& b_ )
		{
			const Vector3 a = *(Vector3*)a_.getValue();
			const Vector3 b = *(Vector3*)b_.getValue();
			Vector3 res(a.x+b.x,a.y+b.y,a.z+b.z);
			result.setValue( &res );
		}

		template<>
		void Parameter<Vector3>::Subtract( IKeyframeParameter& result, const IKeyframeParameter& a_, const IKeyframeParameter& b_ )
		{
			const Vector3 a = *(Vector3*)a_.getValue();
			const Vector3 b = *(Vector3*)b_.getValue();
			Vector3 res(a.x-b.x,a.y-b.y,a.z-b.z);
			result.setValue( &res );
		}

		/////////////////////////////
		// Point3 specializations
		/////////////////////////////

		template<>
		void Parameter<Point3>::Add( IKeyframeParameter& result, const IKeyframeParameter& a_, const IKeyframeParameter& b_ )
		{
			const Point3 a = *(Point3*)a_.getValue();
			const Point3 b = *(Point3*)b_.getValue();
			Point3 res(a.x+b.x,a.y+b.y,a.z+b.z);
			result.setValue( &res );
		}

		template<>
		void Parameter<Point3>::Subtract( IKeyframeParameter& result, const IKeyframeParameter& a_, const IKeyframeParameter& b_ )
		{
			const Point3 a = *(Point3*)a_.getValue();
			const Point3 b = *(Point3*)b_.getValue();
			Point3 res(a.x-b.x,a.y-b.y,a.z-b.z);
			result.setValue( &res );
		}

		template<>
		void Parameter<Point3>::ScalarMult( IKeyframeParameter& result, const IKeyframeParameter& a_, const Scalar& t )
		{
			const Point3 a = *(Point3*)a_.getValue();
			Point3 res(a.x*t,a.y*t,a.z*t);
			result.setValue( &res );
		}
	}
}

