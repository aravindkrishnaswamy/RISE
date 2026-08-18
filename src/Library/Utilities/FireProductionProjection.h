//////////////////////////////////////////////////////////////////////
//
//  FireProductionProjection.h - one-pass fp32 production MAC projection
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FIREPRODUCTIONPROJECTION_
#define FIREPRODUCTIONPROJECTION_

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace RISE
{
	enum FireProductionProjectionBoundary
	{
		FireProductionProjectionPeriodic,
		FireProductionProjectionPressureOpen,
		FireProductionProjectionWall
	};

	struct FireProductionProjectionShape
	{
		std::size_t nx,ny,nz;
		float cellWidthM;

		FireProductionProjectionShape() : nx(0),ny(0),nz(0),cellWidthM(0.0f) {}
		std::size_t CellCount() const {return nx*ny*nz;}
	};

	struct FireProductionProjectionRequest
	{
		FireProductionProjectionShape shape;
		float timeStepS;
		float ambientDensityKGPerM3;
		std::array<FireProductionProjectionBoundary,6> boundary;
		std::vector<float> gasDensityKGPerM3;
		std::array<std::vector<float>,3> provisionalMomentumKGPerM2S;
		std::vector<float> divergenceTargetPerS;

		FireProductionProjectionRequest() : timeStepS(0.0f),ambientDensityKGPerM3(0.0f)
		{
			boundary.fill(FireProductionProjectionWall);
		}
	};

	struct FireProductionProjectionResult
	{
		std::array<std::vector<float>,3> faceDensityKGPerM3;
		std::array<std::vector<float>,3> velocityMPerS;
		std::array<std::vector<float>,3> momentumKGPerM2S;
		std::vector<float> pressurePa;
		std::array<std::vector<unsigned char>,6> pressureOpenInflow;
		float maximumPreProjectionResidualPerS;
		float maximumPostProjectionResidualPerS;
		float maximumOpenComplementarityDiscrepancyMPerS;
		bool validationPassed;
		double deviceElapsedMS;

		FireProductionProjectionResult() : maximumPreProjectionResidualPerS(0.0f),
			maximumPostProjectionResidualPerS(0.0f),
			maximumOpenComplementarityDiscrepancyMPerS(0.0f),validationPassed(false),
			deviceElapsedMS(0.0) {}
	};

	std::size_t FireProductionProjectionFaceCount(
		const FireProductionProjectionShape& shape, unsigned int axis );

	bool ValidateFireProductionProjectionRequest(
		const FireProductionProjectionRequest& request,
		std::string* error=0 );

	//! Strict-binary32 comparator implementing the stored P2 schedule.
	bool ProjectFireProductionCPU(
		const FireProductionProjectionRequest& request,
		FireProductionProjectionResult& result,
		std::string* error=0 );

	//! Metal implementation. Non-Metal builds fail explicitly without CPU fallback.
	bool ProjectFireProductionMetal(
		const FireProductionProjectionRequest& request,
		FireProductionProjectionResult& result,
		std::string* error=0 );
}

#endif
