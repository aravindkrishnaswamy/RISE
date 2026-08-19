//////////////////////////////////////////////////////////////////////
//
//  FireProductionTransport.h - fp32 production coupled transport
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FIREPRODUCTIONTRANSPORT_
#define FIREPRODUCTIONTRANSPORT_

#include "FireProductionProjection.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace RISE
{
	//! Advection-only P3 oracle surface.  Carrier velocities and boundaries are
	//! frozen from the beginning snapshot; only conservativeValues advances.
	struct FireProductionCellPalindromeRequest
	{
		FireProductionProjectionShape shape;
		std::size_t componentCount;
		float timeStepS;
		std::array<FireProductionProjectionBoundary,6> boundary;
		std::vector<float> conservativeValues;
		std::array<std::vector<float>,3> frozenVelocityMPerS;
		std::vector<float> ambientValues;

		FireProductionCellPalindromeRequest() : componentCount(0u),timeStepS(0.0f)
		{
			boundary.fill(FireProductionProjectionWall);
		}
	};

	struct FireProductionCellPalindromeResult
	{
		std::vector<float> conservativeValues;
		std::uint32_t executedSubmapCount;
		std::uint32_t privateResidentBufferCount;
		std::uint32_t sharedBufferCount;
		std::uint32_t commandCommitCount;
		std::uint32_t interstageFullGridReadbackCount;
		std::uint64_t certifiedWorkingSetBytes;
		std::uint64_t actualTrackedWorkingSetBytes;
		double deviceElapsedMS;

		FireProductionCellPalindromeResult() : executedSubmapCount(0u),
			privateResidentBufferCount(0u),sharedBufferCount(0u),commandCommitCount(0u),
			interstageFullGridReadbackCount(0u),certifiedWorkingSetBytes(0u),
			actualTrackedWorkingSetBytes(0u),deviceElapsedMS(0.0) {}
	};

	//! Dual-grid momentum comparison surface. Face arrays include their positive
	//! publication plane; it is a duplicate only on periodic component-normal axes.
	struct FireProductionDualMomentumRequest
	{
		FireProductionProjectionShape shape;
		float timeStepS;
		float ambientDensityKGPerM3;
		std::array<FireProductionProjectionBoundary,6> boundary;
		std::array<std::vector<float>,3> beginningFaceDensity;
		std::array<std::vector<float>,3> beginningMomentum;
		std::array<std::vector<float>,3> frozenVelocityMPerS;

		FireProductionDualMomentumRequest() : timeStepS(0.0f),ambientDensityKGPerM3(1.0f)
		{
			boundary.fill(FireProductionProjectionPeriodic);
		}
	};

	struct FireProductionDualMomentumResult
	{
		std::array<std::vector<float>,3> auxiliaryFaceDensity;
		std::array<std::vector<float>,3> momentum;
		std::uint32_t executedSubmapCount;
		std::uint32_t canonicalSeamCopyCount;
		std::uint32_t commandCommitCount;
		std::uint32_t interstageFullGridTransferCount;
		std::uint64_t actualMetalAllocationBytes;
		double deviceElapsedMS;

		FireProductionDualMomentumResult() : executedSubmapCount(0u),
			canonicalSeamCopyCount(0u),commandCommitCount(0u),
			interstageFullGridTransferCount(0u),actualMetalAllocationBytes(0u),
			deviceElapsedMS(0.0) {}
	};

	using FireProductionPeriodicDualMomentumRequest=FireProductionDualMomentumRequest;
	using FireProductionPeriodicDualMomentumResult=FireProductionDualMomentumResult;

	bool ValidateFireProductionCellPalindromeRequest(
		const FireProductionCellPalindromeRequest& request,
		std::string* error=0 );

	//! Complete standalone-comparator peak, including caller request/result,
	//! Shared staging, private resident scratch, all five parameter pairs, and
	//! conservative 16-KiB outward allocation rounding for every Metal buffer.
	bool FireProductionCellPalindromeWorkingSetBytes(
		const FireProductionProjectionShape& shape,
		std::size_t componentCount,
		std::uint64_t& bytes );

	//! Strict-binary32 five-pass oracle: x/2,y/2,z,y/2,x/2.
	bool RemapFireProductionCellPalindromeCPU(
		const FireProductionCellPalindromeRequest& request,
		FireProductionCellPalindromeResult& result,
		std::string* error=0 );

	//! One-command Metal comparator for the same five-pass composition.  Full
	//! grids remain private between passes; Shared staging exists only at the
	//! standalone request and publication boundaries.
	bool RemapFireProductionCellPalindromeMetal(
		const FireProductionCellPalindromeRequest& request,
		FireProductionCellPalindromeResult& result,
		std::string* error=0 );

	//! Test/publication wrapper around the Private-buffer resident seam. It
	//! performs boundary upload and terminal staging outside the resident call,
	//! and reports only the resident command/transfer diagnostics.
	bool RemapFireProductionCellPalindromeMetalResidentComparator(
		const FireProductionCellPalindromeRequest& request,
		FireProductionCellPalindromeResult& result,
		std::string* error=0 );

#if defined(__OBJC__) && defined(__APPLE__)
	struct FireProductionMetalCellPalindromeResidentInput
	{
		id<MTLBuffer> conservativeValues;
		std::array<id<MTLBuffer>,3> frozenVelocityMPerS;
		id<MTLBuffer> ambientValues;

		FireProductionMetalCellPalindromeResidentInput() : conservativeValues(nil),
			ambientValues(nil)
		{
			frozenVelocityMPerS.fill(nil);
		}
	};

	struct FireProductionMetalCellPalindromeResidentResult
	{
		id<MTLBuffer> conservativeValues;
		std::uint32_t executedSubmapCount;
		std::uint32_t commandCommitCount;
		std::uint32_t interstageFullGridTransferCount;
		std::uint64_t actualMetalAllocationBytes;
		double deviceElapsedMS;

		FireProductionMetalCellPalindromeResidentResult() : conservativeValues(nil),
			executedSubmapCount(0u),commandCommitCount(0u),
			interstageFullGridTransferCount(0u),actualMetalAllocationBytes(0u),
			deviceElapsedMS(0.0) {}
	};

	//! Private-buffer production seam. The host request supplies validation and
	//! schedule bytes; the authoritative full-grid operands are the resident
	//! buffers. No full-grid staging or host access is permitted in this call.
	bool RemapFireProductionCellPalindromeMetalResident(
		const FireProductionCellPalindromeRequest& request,
		const FireProductionMetalCellPalindromeResidentInput& input,
		FireProductionMetalCellPalindromeResidentResult& result,
		std::string* error=0 );
#endif

	//! Strict-binary32 periodic MAC oracle.  Each noncollocated momentum
	//! component owns a separate dual tuple and five-pass palindrome.
	bool RemapFireProductionPeriodicDualMomentumCPU(
		const FireProductionPeriodicDualMomentumRequest& request,
		FireProductionPeriodicDualMomentumResult& result,
		std::string* error=0 );

	//! Step-boundary packing oracle for one periodic MAC component. This builds
	//! the common auxiliary-density/momentum cell tuple and its three frozen
	//! dual carriers; resident execution consumes the corresponding Private
	//! buffers without rebuilding or staging them interstage.
	bool BuildFireProductionPeriodicDualCellRequest(
		const FireProductionPeriodicDualMomentumRequest& request,
		unsigned int transportedComponent,
		FireProductionCellPalindromeRequest& dualRequest,
		std::string* error=0 );

	//! Conservative resident periodic-dual peak. It adds all nine borrowed and
	//! six atomically published face arrays to the largest two-channel resident
	//! palindrome certificate.
	bool FireProductionPeriodicDualMomentumResidentWorkingSetBytes(
		const FireProductionProjectionShape& shape,
		std::uint64_t& bytes );

	//! Standalone oracle wrapper around the periodic Private-buffer resident
	//! dual-grid seam. Boundary uploads and the final tap are outside the
	//! measured resident interval.
	bool RemapFireProductionPeriodicDualMomentumMetal(
		const FireProductionPeriodicDualMomentumRequest& request,
		FireProductionPeriodicDualMomentumResult& result,
		std::string* error=0 );

	//! Strict-binary32 six-side MAC oracle. Component-normal wall planes are
	//! prescribed, open endpoints skip only their normal sweep, and transverse
	//! sweeps consume line-resolved ambient tuples through the shared P1 kernel.
	bool RemapFireProductionDualMomentumCPU(
		const FireProductionDualMomentumRequest& request,
		FireProductionDualMomentumResult& result,
		std::string* error=0 );

#if defined(__OBJC__) && defined(__APPLE__)
	struct FireProductionMetalPeriodicDualMomentumResidentInput
	{
		std::array<id<MTLBuffer>,3> beginningFaceDensity;
		std::array<id<MTLBuffer>,3> beginningMomentum;
		std::array<id<MTLBuffer>,3> frozenVelocityMPerS;

		FireProductionMetalPeriodicDualMomentumResidentInput()
		{
			beginningFaceDensity.fill(nil);beginningMomentum.fill(nil);
			frozenVelocityMPerS.fill(nil);
		}
	};

	struct FireProductionMetalPeriodicDualMomentumResidentResult
	{
		std::array<id<MTLBuffer>,3> auxiliaryFaceDensity;
		std::array<id<MTLBuffer>,3> momentum;
		std::uint32_t executedSubmapCount;
		std::uint32_t commandCommitCount;
		std::uint32_t interstageFullGridTransferCount;
		std::uint64_t actualMetalAllocationBytes;
		double deviceElapsedMS;

		FireProductionMetalPeriodicDualMomentumResidentResult() :
			executedSubmapCount(0u),commandCommitCount(0u),
			interstageFullGridTransferCount(0u),actualMetalAllocationBytes(0u),
			deviceElapsedMS(0.0)
		{
			auxiliaryFaceDensity.fill(nil);momentum.fill(nil);
		}
	};

	bool RemapFireProductionPeriodicDualMomentumMetalResident(
		const FireProductionPeriodicDualMomentumRequest& request,
		const FireProductionMetalPeriodicDualMomentumResidentInput& input,
		FireProductionMetalPeriodicDualMomentumResidentResult& result,
		std::string* error=0 );
#endif
}

#endif
