//////////////////////////////////////////////////////////////////////
//
//  FireProductionForce.h - strict-binary32 production force primitives
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FIRE_PRODUCTION_FORCE_H
#define FIRE_PRODUCTION_FORCE_H

#include "FireProductionProjection.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace RISE
{
	struct FireProductionVremanInput
	{
		std::array<float,9> velocityGradientPerS;
		std::array<float,3> directionalWidthsM;
		float coefficient;

		FireProductionVremanInput() : coefficient(0.07f)
		{
			velocityGradientPerS.fill(0.0f);
			directionalWidthsM.fill(1.0f);
		}
	};

	//! Strict-binary32 transcription of the record Vreman expression. The
	//! caller supplies the coefficient obtained from the certified record.
	bool EvaluateFireProductionVremanEddyViscosity(
		const FireProductionVremanInput& input,
		float& eddyViscosityM2PerS,
		std::string* error=0 );

	struct FireProductionViscousSchedule
	{
		std::uint32_t substepCount;
		float substepTimeS;
		double outwardWork;
		double representedProductUpper;

		FireProductionViscousSchedule() : substepCount(0u),substepTimeS(0.0f),
			outwardWork(0.0),representedProductUpper(0.0) {}
	};

	//! Host half of the r90/r91 outward stability certificate. Both inputs are
	//! stored fp32 bytes; selection certifies the represented fp32 dt/N and
	//! rejects a required ninth substep before any physical update.
	bool SelectFireProductionViscousSchedule(
		float timeStepS,
		float outwardLambdaPerS,
		FireProductionViscousSchedule& schedule,
		std::string* error=0 );

	struct FireProductionFrozenForceRequest
	{
		FireProductionProjectionShape shape;
		float timeStepS;
		float ambientDensityKGPerM3;
		float vremanCoefficient;
		std::array<float,3> gravityMPerS2;
		std::array<FireProductionProjectionBoundary,6> boundary;
		std::vector<float> cellGasDensityKGPerM3;
		std::vector<float> molecularKinematicViscosityM2PerS;
		std::array<std::vector<float>,3> faceDensityKGPerM3;
		std::array<std::vector<float>,3> beginningMomentumKGPerM2S;

		FireProductionFrozenForceRequest() : timeStepS(0.0f),ambientDensityKGPerM3(1.0f),
			vremanCoefficient(0.07f)
		{
			gravityMPerS2.fill(0.0f);
			boundary.fill(FireProductionProjectionPeriodic);
		}
	};

	struct FireProductionFrozenForceResult
	{
		std::vector<float> eddyKinematicViscosityM2PerS;
		std::vector<float> effectiveDynamicViscosityPaS;
		std::array<std::vector<float>,3> beginningViscousMomentumRateKGPerM2S2;
		std::array<std::vector<float>,3> gravityMomentumIncrementKGPerM2S;
	};

	//! Logical peak of the CPU frozen-force comparator, including caller input,
	//! local work, and the atomically published result payload.
	bool FireProductionFrozenForceWorkingSetBytes(
		const FireProductionProjectionShape& shape,
		std::uint64_t& bytes );

	bool FireProductionFrozenForceAdvanceWorkingSetBytes(
		const FireProductionProjectionShape& shape,
		std::uint64_t& bytes );

	//! Beginning-state, strict-binary32 force field used by the production P3
	//! comparator. Vreman and mu are frozen; viscous rates update only interior
	//! normal faces, relative gravity also owns pressure-open endpoint faces,
	//! and wall-normal endpoints remain prescribed zero.
	bool BuildFireProductionFrozenForceFieldsCPU(
		const FireProductionFrozenForceRequest& request,
		FireProductionFrozenForceResult& result,
		std::string* error=0 );

	struct FireProductionFrozenForceAdvanceResult
	{
		FireProductionFrozenForceResult frozenFields;
		FireProductionViscousSchedule schedule;
		std::array<std::vector<float>,3> momentumKGPerM2S;
		std::array<std::uint64_t,8> intermediateMomentumByteDigests;
		std::uint32_t intermediateMomentumDigestCount;
		std::uint32_t executedViscousSubstepCount;

		FireProductionFrozenForceAdvanceResult() :
			intermediateMomentumDigestCount(0u),executedViscousSubstepCount(0u)
		{
			intermediateMomentumByteDigests.fill(0u);
		}
	};

	//! Strict-binary32 force comparator after the resident preflight has supplied
	//! its outward Lambda scalar. mu_eff and face density stay frozen through all
	//! viscous substeps; relative gravity is added exactly once afterward.
	bool AdvanceFireProductionFrozenForceCPU(
		const FireProductionFrozenForceRequest& request,
		float outwardLambdaPerS,
		FireProductionFrozenForceAdvanceResult& result,
		std::string* error=0 );
}

#endif
