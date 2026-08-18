//////////////////////////////////////////////////////////////////////
//
//  FireProductionAdvectionUnsupported.cpp - honest non-Metal remap seam
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "FireProductionAdvection.h"

namespace RISE
{
	bool RemapFireProductionMetal( const FireProductionRemapRequest&,
		FireProductionRemapResult& result, std::string* error )
	{
		result=FireProductionRemapResult();
		if( error ) *error=
			"production fire Metal remap unavailable: Metal is not built on this platform";
		return false;
	}
}
