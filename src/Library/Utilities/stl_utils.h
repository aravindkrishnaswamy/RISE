//////////////////////////////////////////////////////////////////////
//
//  stl_utils.h - Utility function for dealing with STL things
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 14, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef STL_UTILS_
#define STL_UTILS_

#include <vector>

namespace RISE
{
	namespace stl_utils
	{
		template< typename T >
		inline void container_erase_all( T& v )
		{
			T empty;
			v.swap( empty );
		}

		template< typename T >
		inline void container_optimize( T& v )
		{
			T opto( v.size() );
			std::copy( v.begin(), v.end(), opto.begin() );
			v.swap( opto );
		}

	}
}

#endif
