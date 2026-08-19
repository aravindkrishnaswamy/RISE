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

		FireProductionCellPalindromeResult() : executedSubmapCount(0u) {}
	};

	bool ValidateFireProductionCellPalindromeRequest(
		const FireProductionCellPalindromeRequest& request,
		std::string* error=0 );

	//! Strict-binary32 five-pass oracle: x/2,y/2,z,y/2,x/2.
	bool RemapFireProductionCellPalindromeCPU(
		const FireProductionCellPalindromeRequest& request,
		FireProductionCellPalindromeResult& result,
		std::string* error=0 );
}

#endif
