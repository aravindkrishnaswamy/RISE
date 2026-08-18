//////////////////////////////////////////////////////////////////////
//
//  FireProductionProjectionUnsupported.cpp - honest non-Metal P2 seam
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "FireProductionProjection.h"

#include <new>

namespace RISE
{
	bool ProjectFireProductionMetal( const FireProductionProjectionRequest&,
		FireProductionProjectionResult& result, std::string* error )
	{
		result=FireProductionProjectionResult();
		if( error ) {
			try { *error=
				"production fire Metal projection unavailable: Metal is not built on this platform"; }
			catch( const std::bad_alloc& ) { error->clear(); }
		}
		return false;
	}
}
