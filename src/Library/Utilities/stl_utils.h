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

		template<typename A, typename B>
		inline bool shuffle( std::vector< A > const& left, std::vector< B > const& right, std::vector< std::pair< A, B > >& result ) {
			typedef std::vector< A > const left_t;
			typedef std::vector< B > const right_t;

			// sanity check
			if (left.size() != right.size()) {
				return false;
			}

			// result.clear(); // optional really, but hey.
			result.reserve( result.size() + left.size() );

			typename left_t::const_iterator it_l;
			typename right_t::const_iterator it_r;
			{for(
				(it_l = left.begin()), (it_r = right.begin());
				(it_l != left.end()) && (it_r != right.end());
				++it_l, ++it_r
				)
			{
				result.push_back( std::pair< A, B >( *it_l, *it_r ) );
			}}

			return true;
		}
	}
}

#endif
