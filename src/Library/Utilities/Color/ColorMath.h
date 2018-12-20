//////////////////////////////////////////////////////////////////////
//
//  ColorMath.h - Math done with color PELs
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 4, 2005
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef COLOR_MATH_H
#define COLOR_MATH_H

#include <math.h>

namespace RISE
{
	struct ColorMath
	{
		// Invert
		template< class T >
		static inline T invert( const T& a )
		{
			return T( 1.0/a[0], 1.0/a[1], 1.0/a[2] );
		}

		// exp
		template< class T >
		static inline T exponential( const T& a )
		{
			return T( exp(a[0]), exp(a[1]), exp(a[2]) );
		}

		// square root
		template< class T >
		static inline T root( const T& a )
		{
			return T( sqrt(a[0]), sqrt(a[1]), sqrt(a[2]) );
		}

		// Takes the values to the given power
		template< class T >
		static inline T pow( const T& b, const T& e )
		{
			return T( ::pow(b[0],e[0]), ::pow(b[1],e[1]), ::pow(b[2],e[2]) );
		}

		// Takes the values to the given power
		template< class T >
		static inline T pow( const T& b, const Scalar e )
		{
			return T( ::pow(b[0],e), ::pow(b[1],e), ::pow(b[2],e) );
		}

		// Clamps
		template< class T >
		static inline bool Clamp( T& ret, const Scalar& l, const Scalar& h )
		{
			bool retval = false;
			if( ret[0] > h ) { ret[0] = h; retval = true; };
			if( ret[1] > h ) { ret[1] = h; retval = true; };
			if( ret[2] > h ) { ret[2] = h; retval = true; };
			if( ret[0] < l ) { ret[0] = l; retval = true; };
			if( ret[1] < l ) { ret[1] = l; retval = true; };
			if( ret[2] < l ) { ret[2] = l; retval = true; };
			return retval;
		}

		// Scale
		template< class T >
		static inline void Scale( T& c )
		{
			if(c[0] < 0) c[0] = 0;
			if(c[1] < 0) c[1] = 0;
			if(c[2] < 0) c[2] = 0;

			Scalar max = c[0];
			if( c[1] > max ) {
				max = c[1];
			}
			if( c[2] > max ) {
				max = c[2];
			}

			if( max > 1 ) {
				Scalar OVMax = 1.0/max;
				c[0] *= OVMax;
				c[1] *= OVMax;
				c[2] *= OVMax;
			}
		}

		// Ensures all values are positive
		template< class T >
		static inline void EnsurePositve( T& c )
		{
			if( c[0] < 0 ) c[0] = 0;
			if( c[1] < 0 ) c[1] = 0;
			if( c[2] < 0 ) c[2] = 0;
		}

		// Returns the minimum value of the components
		template< class T >
		static inline Scalar MinValue( const T& c )
		{
			return (c[0] < c[1] ? (c[0] < c[2] ? c[0] : c[2]) : (c[1] < c[2] ? c[1] : c[2]));
		}

		// Returns the maximum value of the components
		template< class T >
		static inline Scalar MaxValue( const T& c )
		{
			return (c[0] > c[1] ? (c[0] > c[2] ? c[0] : c[2]) : (c[1] > c[2] ? c[1] : c[2]));
		}

	};

	// This is to help out someone
	template< class T >
	static inline T pow( const Scalar b, const T& e )
	{
		return ColorMath::pow( T(b,b,b), e );
	}

	template< class T >
	static inline T exp( const T& c )
	{
		return ColorMath::exponential( c );
	}

	template< class T >
	static inline T sqrt( const T& c )
	{
		return ColorMath::root( c );
	}

	// Specialize on scalars so that we don't confuse the hell out of the stupid compiler
	template<>
	inline Scalar pow( const Scalar b, const Scalar& e )
	{
		return ::pow( b, e );
	}

	template<>
	inline Scalar exp( const Scalar& c )
	{
		return ::exp( c );
	}

	template<>
	inline Scalar sqrt( const Scalar& c )
	{
		return ::sqrt( c );
	}
}


#endif


