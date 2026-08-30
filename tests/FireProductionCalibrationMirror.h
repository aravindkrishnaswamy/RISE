#ifndef FIRE_PRODUCTION_CALIBRATION_MIRROR_H
#define FIRE_PRODUCTION_CALIBRATION_MIRROR_H

#include "Utilities/FireProductionForce.h"
#include "fire_production_fp64/FireProductionForce.h"
#include "../tools/fire_simulator_core.h"

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
		FP64::FireProductionProjectionResult physicalProjection;
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
		if(!FP64::RemapFireProductionCompatibleDualMomentumCPU(dual,
			computed.cell.acceptedGasMassDoseKGPerM2,computed.dual,error))return false;
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
		projection.residentPhysicalOpenVCycleCount=
			request.physicalOpenProjectionVCycleCount;
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
		RISE::FireProductionManifoldTailTarget tailTarget;
		std::string tailError;
		const bool tailMetadataAvailable=!request.beginningManifoldDeviationPerCell.empty();
		if(tailMetadataAvailable&&!RISE::DeriveFireProductionManifoldTailTarget(
			request.beginningManifoldDeviationPerCell,
			static_cast<double>(request.force.timeStepS),
			static_cast<double>(request.force.shape.cellWidthM),tailTarget,&tailError)){
			if(error)*error=tailError;return false;
		}
		const bool targetedRestorationActive=tailMetadataAvailable&&
			tailTarget.outlierCellCount>0u;
		if(!request.enforceManifoldPlateau&&!targetedRestorationActive){
			if(!FP64::ProjectFireProductionCPU(projection,computed.projection,error))return false;
			result=std::move(computed);return true;
		}
		if(!FP64::ProjectFireProductionResidentPhysicalCPU(projection,
			computed.physicalProjection,error))return false;
		projection.provisionalMomentumKGPerM2S=computed.physicalProjection.momentumKGPerM2S;
		projection.divergenceTargetPerS=Promote(targetedRestorationActive?
			tailTarget.divergenceTargetPerS:request.restorationDivergenceTargetPerS);
		if(!FP64::ProjectFireProductionRestorationCPU(projection,computed.projection,error))return false;
		result=std::move(computed);return true;
	}

	struct ClosureConvergence64Result
	{
		ResidentStep64Result resident;
		double maximumGeneration;
		double maximumDeviation;
		std::uint32_t executedPassCount;

		ClosureConvergence64Result() : maximumGeneration(0.0),maximumDeviation(0.0),
			executedPassCount(0u) {}
	};

	//! Diagnostic same-scheme fixed-pass closure mirror.  The oracle supplies
	//! only the sealed divergence target; every remap and both projections are
	//! the generated binary64 production operators.
	inline bool AdvanceResidentStep64Closure(
		const RISE::FireProductionResidentStepRequest& request,
		const double outwardLambdaPerS,
		const RISE::FireSimulationMethaneRecord& thermochemistry,
		const std::uint32_t passLimit,
		ClosureConvergence64Result& result,std::string* error)
	{
		result=ClosureConvergence64Result();
		if(passLimit<1u||passLimit>8u||
			request.beginningManifoldDeviationPerCell.size()!=request.force.shape.CellCount())
			return false;
		ClosureConvergence64Result computed;
		if(!AdvanceResidentStep64(request,outwardLambdaPerS,computed.resident,error))return false;
		const std::size_t cells=request.force.shape.CellCount();
		const std::vector<double> baseConservative=computed.resident.conservativeValues;
		std::vector<double> gasDensity(cells);
		for(std::size_t cell=0u;cell<cells;++cell){
			double gas=baseConservative[cells+cell];
			for(std::size_t component=2u;component<=6u;++component)
				gas+=baseConservative[component*cells+cell];
			if(!(gas>0.0)||!std::isfinite(gas))return false;
			gasDensity[cell]=gas;
		}
		auto measure=[&](const std::vector<double>& conservative,
			double& maximumGeneration,double& maximumDeviation,
			std::vector<double>* deviations){
			if(conservative.size()!=9u*cells)return false;
			maximumGeneration=0.0;maximumDeviation=0.0;
			if(deviations)deviations->assign(cells,0.0);
			std::array<double,9> state;
			for(std::size_t cell=0u;cell<cells;++cell){
				for(std::size_t component=0u;component<9u;++component)
					state[component]=conservative[component*cells+cell];
				double ratio=0.0;
				if(!thermochemistry.AcceptedConservativeVolumeRatioByComponentOrder(
					// The mirror starts from the exact promoted Binary32 golden payload.
					// Preserve that inherited admissibility envelope while all closure
					// arithmetic and the reported volume ratio remain binary64.
					state.data(),state.size(),RISE::FireStateProducerPrecision::Binary32,
					ratio,error))return false;
				const double deviation=ratio-1.0;
				maximumGeneration=std::max(maximumGeneration,std::fabs(
					deviation-request.beginningManifoldDeviationPerCell[cell]));
				maximumDeviation=std::max(maximumDeviation,std::fabs(deviation));
				if(deviations)(*deviations)[cell]=deviation;
			}
			return true;
		};
		computed.executedPassCount=1u;
		for(std::uint32_t pass=1u;pass<passLimit;++pass){
			std::vector<double> deviations;
			if(!measure(computed.resident.conservativeValues,computed.maximumGeneration,
				computed.maximumDeviation,&deviations))return false;
			if(computed.maximumGeneration==0.0)break;
			FP64::FireProductionProjectionRequest projection;
			projection.shape=Shape64(request.force.shape);
			projection.timeStepS=static_cast<double>(request.force.timeStepS);
			projection.ambientDensityKGPerM3=request.force.ambientDensityKGPerM3;
			projection.residentPhysicalOpenVCycleCount=request.physicalOpenProjectionVCycleCount;
			for(unsigned int side=0u;side<6u;++side)
				projection.boundary[side]=Boundary64(request.force.boundary[side]);
			projection.gasDensityKGPerM3=gasDensity;
			projection.provisionalMomentumKGPerM2S=
				computed.resident.physicalProjection.momentumKGPerM2S;
			projection.divergenceTargetPerS=Promote(request.restorationDivergenceTargetPerS);
			for(std::size_t cell=0u;cell<cells;++cell)
				projection.divergenceTargetPerS[cell]+=(deviations[cell]-
					request.beginningManifoldDeviationPerCell[cell])/
					static_cast<double>(request.force.timeStepS);
			FP64::FireProductionProjectionResult restoration;
			if(!FP64::ProjectFireProductionRestorationCPU(projection,restoration,error))return false;
			FP64::FireProductionCellPalindromeRequest corrector=Cell64(request.cellTransport);
			corrector.frozenVelocityMPerS=restoration.velocityMPerS;
			FP64::FireProductionCellPalindromeResult corrected;
			if(!FP64::RemapFireProductionCellPalindromeCPU(corrector,corrected,error)||
				corrected.conservativeValues.size()!=request.cellSourceIncrement.size())return false;
			for(std::size_t value=0u;value<corrected.conservativeValues.size();++value){
				corrected.conservativeValues[value]+=request.cellSourceIncrement[value];
				if(!std::isfinite(corrected.conservativeValues[value]))return false;
			}
			computed.resident.cell=std::move(corrected);
			computed.resident.conservativeValues=computed.resident.cell.conservativeValues;
			computed.resident.projection=std::move(restoration);
			++computed.executedPassCount;
		}
		if(!measure(computed.resident.conservativeValues,computed.maximumGeneration,
			computed.maximumDeviation,nullptr))return false;
		result=std::move(computed);return true;
	}
}

#endif
