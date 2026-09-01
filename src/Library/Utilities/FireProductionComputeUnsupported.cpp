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
		capability.deviceDiscovery=FireProductionDeviceBackendNotBuilt;
		capability.structuredError=
			"production fire compute capability unavailable: Metal is not built on this platform";
		return true;
	}

	bool RunFireProductionComputeChallenge(
		const std::uint32_t*, std::size_t, std::vector<std::uint32_t>& output,
		std::string* error )
	{
		output.clear();
		if( error ) *error=
			"production fire compute challenge unavailable: Metal is not built on this platform";
		return false;
	}
}
