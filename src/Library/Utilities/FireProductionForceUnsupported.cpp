//////////////////////////////////////////////////////////////////////
//
//  FireProductionForceUnsupported.cpp - honest non-Metal force seam
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "FireProductionForce.h"

#include <new>

namespace RISE
{
	bool BuildFireProductionFrozenForceFieldsMetal(
		const FireProductionFrozenForceRequest&,
		FireProductionFrozenForceResult& result,
		double& deviceElapsedMS,
		std::string* error )
	{
		result=FireProductionFrozenForceResult();deviceElapsedMS=0.0;
		if( error ) try {
			*error="production frozen-force Metal unavailable: Metal is not built on this platform";
		} catch( const std::bad_alloc& ) { error->clear(); }
		return false;
	}
}
