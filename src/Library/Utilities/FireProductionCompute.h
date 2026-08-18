//////////////////////////////////////////////////////////////////////
//
//  FireProductionCompute.h - production-fire compute capability seam
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FIREPRODUCTIONCOMPUTE_
#define FIREPRODUCTIONCOMPUTE_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace RISE
{
	struct FireProductionComputeCapability
	{
		bool available;
		bool identityKernelPassed;
		bool unifiedMemory;
		std::string backend;
		std::string deviceName;
		std::string deviceFamily;
		std::uint64_t registryId;
		std::size_t maximumThreadsPerThreadgroup;
		std::string structuredError;

		FireProductionComputeCapability() : available(false),
			identityKernelPassed(false), unifiedMemory(false), registryId(0),
			maximumThreadsPerThreadgroup(0) {}
	};

	//! Queries the owned production compute seam.  `available` becomes true
	//! only after an embedded fp32 kernel has compiled, dispatched, completed,
	//! and returned the exact expected bytes.
	bool QueryFireProductionComputeCapability(
		FireProductionComputeCapability& capability );

	//! Executes a nonidentity integer transform through the same owned Metal
	//! command seam.  It exists so the capability gate can challenge the device
	//! with caller-selected bytes instead of trusting self-reported booleans.
	bool RunFireProductionComputeChallenge(
		const std::uint32_t* input,
		std::size_t count,
		std::vector<std::uint32_t>& output,
		std::string* error=0 );
}

#endif
