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
	enum FireProductionDeviceDiscovery
	{
		FireProductionDeviceUnqueried,
		FireProductionDeviceAvailable,
		FireProductionDeviceBlockedByExecutionContext,
		FireProductionDeviceAbsent,
		FireProductionDeviceBackendNotBuilt
	};

	inline const char* FireProductionDeviceDiscoveryName(
		FireProductionDeviceDiscovery discovery )
	{
		switch( discovery ) {
		case FireProductionDeviceAvailable:return "available";
		case FireProductionDeviceBlockedByExecutionContext:
			return "blocked-by-execution-context";
		case FireProductionDeviceAbsent:return "no-device";
		case FireProductionDeviceBackendNotBuilt:return "backend-not-built";
		default:return "unqueried";
		}
	}

	//! Classifies discovery independently of kernel validation.  Apple-silicon
	//! machines always contain an integrated Metal device, so an empty result
	//! from both discovery APIs there is an execution-context denial, not
	//! evidence that the hardware is absent.
	inline FireProductionDeviceDiscovery ClassifyFireProductionDeviceDiscovery(
		bool backendBuilt, bool appleSilicon, bool defaultDevicePresent,
		std::size_t enumeratedDeviceCount )
	{
		if( !backendBuilt ) return FireProductionDeviceBackendNotBuilt;
		if( defaultDevicePresent ) return FireProductionDeviceAvailable;
		if( enumeratedDeviceCount>0u || appleSilicon )
			return FireProductionDeviceBlockedByExecutionContext;
		return FireProductionDeviceAbsent;
	}

	struct FireProductionComputeCapability
	{
		bool available;
		bool identityKernelPassed;
		bool unifiedMemory;
		bool defaultDevicePresent;
		std::string backend;
		std::string deviceName;
		std::string deviceFamily;
		std::uint64_t registryId;
		std::size_t maximumThreadsPerThreadgroup;
		std::size_t enumeratedDeviceCount;
		FireProductionDeviceDiscovery deviceDiscovery;
		std::string structuredError;

		FireProductionComputeCapability() : available(false),
			identityKernelPassed(false), unifiedMemory(false),
			defaultDevicePresent(false),
			registryId(0), maximumThreadsPerThreadgroup(0),
			enumeratedDeviceCount(0),deviceDiscovery(FireProductionDeviceUnqueried) {}
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
