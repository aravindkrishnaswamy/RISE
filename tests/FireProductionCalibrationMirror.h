#ifndef FIRE_PRODUCTION_CALIBRATION_MIRROR_H
#define FIRE_PRODUCTION_CALIBRATION_MIRROR_H

#include "Utilities/FireProductionForce.h"
#include "fire_production_fp64/FireProductionForce.h"

#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace FireProductionCalibration
{
	namespace FP64=RISEFireProductionFP64;

	inline FP64::FireProductionProjectionBoundary Boundary64(
		const RISE::FireProductionProjectionBoundary boundary)
	{
		return static_cast<FP64::FireProductionProjectionBoundary>(boundary);
	}

	inline FP64::FireProductionProjectionShape Shape64(
		const RISE::FireProductionProjectionShape& shape)
	{
		FP64::FireProductionProjectionShape result;
		result.nx=shape.nx;result.ny=shape.ny;result.nz=shape.nz;
		result.cellWidthM=static_cast<double>(shape.cellWidthM);return result;
	}

	template<class T> inline std::vector<double> Promote(const std::vector<T>& values)
	{
		return std::vector<double>(values.begin(),values.end());
	}

	inline FP64::FireProductionFrozenForceRequest Force64(
		const RISE::FireProductionFrozenForceRequest& request)
	{
		FP64::FireProductionFrozenForceRequest result;
		result.shape=Shape64(request.shape);result.timeStepS=request.timeStepS;
		result.ambientDensityKGPerM3=request.ambientDensityKGPerM3;
		result.vremanCoefficient=request.vremanCoefficient;
		for(unsigned int axis=0u;axis<3u;++axis){
			result.gravityMPerS2[axis]=request.gravityMPerS2[axis];
			result.faceDensityKGPerM3[axis]=Promote(request.faceDensityKGPerM3[axis]);
			result.beginningMomentumKGPerM2S[axis]=Promote(request.beginningMomentumKGPerM2S[axis]);
		}
		for(unsigned int side=0u;side<6u;++side)result.boundary[side]=Boundary64(request.boundary[side]);
		result.cellGasDensityKGPerM3=Promote(request.cellGasDensityKGPerM3);
		result.molecularKinematicViscosityM2PerS=Promote(
			request.molecularKinematicViscosityM2PerS);
		return result;
	}

	inline FP64::FireProductionCellPalindromeRequest Cell64(
		const RISE::FireProductionCellPalindromeRequest& request)
	{
		FP64::FireProductionCellPalindromeRequest result;
		result.shape=Shape64(request.shape);result.componentCount=request.componentCount;
		result.timeStepS=request.timeStepS;
		for(unsigned int side=0u;side<6u;++side)result.boundary[side]=Boundary64(request.boundary[side]);
		result.conservativeValues=Promote(request.conservativeValues);
		result.ambientValues=Promote(request.ambientValues);
		for(unsigned int axis=0u;axis<3u;++axis)
			result.frozenVelocityMPerS[axis]=Promote(request.frozenVelocityMPerS[axis]);
		return result;
	}

	inline FP64::FireProductionDualMomentumRequest Dual64(
		const RISE::FireProductionDualMomentumRequest& request)
	{
		FP64::FireProductionDualMomentumRequest result;
		result.shape=Shape64(request.shape);result.timeStepS=request.timeStepS;
		result.ambientDensityKGPerM3=request.ambientDensityKGPerM3;
		for(unsigned int side=0u;side<6u;++side)result.boundary[side]=Boundary64(request.boundary[side]);
		for(unsigned int axis=0u;axis<3u;++axis){
			result.beginningFaceDensity[axis]=Promote(request.beginningFaceDensity[axis]);
			result.beginningMomentum[axis]=Promote(request.beginningMomentum[axis]);
			result.frozenVelocityMPerS[axis]=Promote(request.frozenVelocityMPerS[axis]);
		}
		return result;
	}

	struct ResidentStep64Result
	{
		std::vector<double> conservativeValues;
		FP64::FireProductionFrozenForceAdvanceResult force;
		FP64::FireProductionCellPalindromeResult cell;
		FP64::FireProductionDualMomentumResult dual;
		FP64::FireProductionProjectionResult projection;
	};

	inline bool AdvanceResidentStep64(const RISE::FireProductionResidentStepRequest& request,
		const double outwardLambdaPerS,ResidentStep64Result& result,std::string* error)
	{
		result=ResidentStep64Result();
		FP64::FireProductionFrozenForceRequest force=Force64(request.force);
		FP64::FireProductionCellPalindromeRequest cell=Cell64(request.cellTransport);
		FP64::FireProductionDualMomentumRequest dual=Dual64(request.dualTransport);
		ResidentStep64Result computed;
		if(!FP64::AdvanceFireProductionFrozenForceCPU(force,outwardLambdaPerS,
			computed.force,error)||!FP64::RemapFireProductionCellPalindromeCPU(
			cell,computed.cell,error))return false;
		dual.beginningMomentum=computed.force.momentumKGPerM2S;
		if(!FP64::RemapFireProductionDualMomentumCPU(dual,computed.dual,error))return false;
		computed.conservativeValues=std::move(computed.cell.conservativeValues);
		if(computed.conservativeValues.size()!=request.cellSourceIncrement.size())return false;
		for(std::size_t value=0u;value<computed.conservativeValues.size();++value){
			computed.conservativeValues[value]+=request.cellSourceIncrement[value];
			if(!std::isfinite(computed.conservativeValues[value]))return false;
		}
		for(unsigned int axis=0u;axis<3u;++axis){
			if(computed.dual.momentum[axis].size()!=request.momentumSourceIncrement[axis].size())
				return false;
			for(std::size_t face=0u;face<computed.dual.momentum[axis].size();++face){
				computed.dual.momentum[axis][face]+=request.momentumSourceIncrement[axis][face];
				if(!std::isfinite(computed.dual.momentum[axis][face]))return false;
			}
		}
		const std::size_t cells=force.shape.CellCount();
		if(cell.componentCount!=9u||computed.conservativeValues.size()!=9u*cells)return false;
		FP64::FireProductionProjectionRequest projection;
		projection.shape=force.shape;projection.timeStepS=force.timeStepS;
		projection.ambientDensityKGPerM3=force.ambientDensityKGPerM3;
		for(unsigned int side=0u;side<6u;++side)projection.boundary[side]=force.boundary[side];
		projection.gasDensityKGPerM3.resize(cells);
		for(std::size_t cellIndex=0u;cellIndex<cells;++cellIndex){
			double gas=computed.conservativeValues[cells+cellIndex];
			for(std::size_t component=2u;component<=6u;++component)
				gas+=computed.conservativeValues[component*cells+cellIndex];
			if(!(gas>0.0)||!std::isfinite(gas))return false;
			projection.gasDensityKGPerM3[cellIndex]=gas;
		}
		projection.provisionalMomentumKGPerM2S=computed.dual.momentum;
		projection.divergenceTargetPerS=Promote(request.divergenceTargetPerS);
		if(!FP64::ProjectFireProductionCPU(projection,computed.projection,error))return false;
		result=std::move(computed);return true;
	}
}

#endif
