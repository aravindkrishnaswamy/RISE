//////////////////////////////////////////////////////////////////////
//
//  FireProductionComputeUnsupported.cpp - honest non-Metal capability
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "FireProductionCompute.h"

namespace RISE
{
	bool QueryFireProductionComputeCapability(
		FireProductionComputeCapability& capability )
	{
		capability=FireProductionComputeCapability();
		capability.backend="unavailable";
		capability.structuredError=
			"production fire compute capability unavailable: Metal is not built on this platform";
		return true;
	}
}
