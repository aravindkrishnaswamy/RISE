//////////////////////////////////////////////////////////////////////
//
//  math_utils.h - Utility functions vaguely math like stuff
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 9, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MATH_UTILS_
#define MATH_UTILS_

namespace RISE
{
	namespace rmath {

		// Does a smooth s-curve interpolation
		// Just like Renderman shaders
		template<typename T>
		inline T smoothstep( const T a, const T b, const T x )
		{
			if( x <= a ) {
				return 0;
			} else if( x >= b ) {
				return 1;
			}
			const T d = (x-a) / (b-a);
			return d*d*(3.0-2.0*d);
		}
	}
}

#define r_min( a, b ) (a < b ? a : b)
#define r_max( a, b ) (a > b ? a : b)

#endif
