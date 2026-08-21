//////////////////////////////////////////////////////////////////////
//
//  FireProductionAdvection.h - fp32 conservative production remap
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FIREPRODUCTIONADVECTION_
#define FIREPRODUCTIONADVECTION_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace RISE
{
	enum FireProductionRemapBoundary
	{
		FireProductionRemapPeriodic,
		FireProductionRemapPressureOpen,
		FireProductionRemapWall
	};

	//! One batch of logically independent uniform-grid lines. Values are SoA:
	//! [component][line][cell], velocities are [line][face], ambient is one
	//! authored value per component.
	struct FireProductionRemapRequest
	{
		std::size_t lineLength;
		std::size_t lineCount;
		std::size_t componentCount;
		float cellWidthM;
		float timeStepS;
		FireProductionRemapBoundary boundary;
		bool asymmetricBoundaries;
		FireProductionRemapBoundary lowerBoundary;
		FireProductionRemapBoundary upperBoundary;
		std::vector<float> values;
		std::vector<float> faceVelocityMPerS;
		std::vector<float> ambientValues;
		bool lineSpecificAmbientValues;
		std::vector<float> lowerAmbientValues;
		std::vector<float> upperAmbientValues;

		FireProductionRemapRequest() : lineLength(0), lineCount(0),
			componentCount(0), cellWidthM(0.0f), timeStepS(0.0f),
			boundary(FireProductionRemapPeriodic), asymmetricBoundaries(false),
			lowerBoundary(FireProductionRemapPeriodic),
			upperBoundary(FireProductionRemapPeriodic),lineSpecificAmbientValues(false) {}
	};

	struct FireProductionRemapResult
	{
		std::vector<float> updatedValues;
		std::vector<float> faceFluxes;
		std::vector<float> sharedLimiterAlpha;
		double deviceElapsedMS;

		FireProductionRemapResult() : deviceElapsedMS(0.0) {}
	};

	//! Outward-rounded Metal allocation bytes for the request's buffer topology.
	bool FireProductionRemapWorkingSetBytes(
		const FireProductionRemapRequest& request,
		std::uint64_t& bytes );

	bool ValidateFireProductionRemapRequest(
		const FireProductionRemapRequest& request,
		std::string* error=0 );

	//! r124 continuous common-alpha cap. Exposed for independent contract gates.
	float FireProductionContinuousSharedLimiterAlpha(
		float alpha, float headroom, float signedConsumption,
		float center, float envelope ) noexcept;

	//! Binary32 CPU oracle with the same stored intermediates as the Metal path.
	bool RemapFireProductionCPU(
		const FireProductionRemapRequest& request,
		FireProductionRemapResult& result,
		std::string* error=0 );

	//! Metal implementation. Non-Metal builds fail explicitly without CPU fallback.
	bool RemapFireProductionMetal(
		const FireProductionRemapRequest& request,
		FireProductionRemapResult& result,
		std::string* error=0 );
}

#endif
