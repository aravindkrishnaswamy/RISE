#ifndef FIRE_PRODUCTION_ROUNDOFF_TRACE_ADAPTER_H
#define FIRE_PRODUCTION_ROUNDOFF_TRACE_ADAPTER_H

#include "Utilities/FireProductionForce.h"
#include "fire_production_trace/FireProductionForce.h"

#include <string>
#include <vector>

namespace FireProductionRoundoffAdapter
{
	namespace Trace=RISEFireProductionTrace;

	inline Trace::FireProductionProjectionShape Shape(
		const RISE::FireProductionProjectionShape& source)
	{
		Trace::FireProductionProjectionShape result;
		result.nx=source.nx;result.ny=source.ny;result.nz=source.nz;
		result.cellWidthM=source.cellWidthM;return result;
	}

	inline Trace::FireProductionProjectionBoundary Boundary(
		const RISE::FireProductionProjectionBoundary source)
	{
		return static_cast<Trace::FireProductionProjectionBoundary>(source);
	}

	template<class Value> inline std::vector<FireProductionRoundoffTrace::TraceFloat>
		Promote(const std::vector<Value>& source)
	{
		return std::vector<FireProductionRoundoffTrace::TraceFloat>(source.begin(),source.end());
	}

	inline Trace::FireProductionFrozenForceRequest Force(
		const RISE::FireProductionFrozenForceRequest& source)
	{
		Trace::FireProductionFrozenForceRequest result;
		result.shape=Shape(source.shape);result.timeStepS=source.timeStepS;
		result.ambientDensityKGPerM3=source.ambientDensityKGPerM3;
		result.vremanCoefficient=source.vremanCoefficient;
		result.cellGasDensityKGPerM3=Promote(source.cellGasDensityKGPerM3);
		result.molecularKinematicViscosityM2PerS=Promote(
			source.molecularKinematicViscosityM2PerS);
		for(unsigned int axis=0u;axis<3u;++axis){
			result.gravityMPerS2[axis]=source.gravityMPerS2[axis];
			result.faceDensityKGPerM3[axis]=Promote(source.faceDensityKGPerM3[axis]);
			result.beginningMomentumKGPerM2S[axis]=Promote(
				source.beginningMomentumKGPerM2S[axis]);
		}
		for(unsigned int side=0u;side<6u;++side)result.boundary[side]=Boundary(
			source.boundary[side]);
		return result;
	}

	inline Trace::FireProductionCellPalindromeRequest Cell(
		const RISE::FireProductionCellPalindromeRequest& source)
	{
		Trace::FireProductionCellPalindromeRequest result;
		result.shape=Shape(source.shape);result.componentCount=source.componentCount;
		result.timeStepS=source.timeStepS;result.conservativeValues=Promote(
			source.conservativeValues);result.ambientValues=Promote(source.ambientValues);
		for(unsigned int axis=0u;axis<3u;++axis)
			result.frozenVelocityMPerS[axis]=Promote(source.frozenVelocityMPerS[axis]);
		for(unsigned int side=0u;side<6u;++side)result.boundary[side]=Boundary(
			source.boundary[side]);
		return result;
	}

	inline Trace::FireProductionDualMomentumRequest Dual(
		const RISE::FireProductionDualMomentumRequest& source)
	{
		Trace::FireProductionDualMomentumRequest result;
		result.shape=Shape(source.shape);result.timeStepS=source.timeStepS;
		result.ambientDensityKGPerM3=source.ambientDensityKGPerM3;
		for(unsigned int axis=0u;axis<3u;++axis){
			result.beginningFaceDensity[axis]=Promote(source.beginningFaceDensity[axis]);
			result.beginningMomentum[axis]=Promote(source.beginningMomentum[axis]);
			result.frozenVelocityMPerS[axis]=Promote(source.frozenVelocityMPerS[axis]);
		}
		for(unsigned int side=0u;side<6u;++side)result.boundary[side]=Boundary(
			source.boundary[side]);
		return result;
	}

	struct ResidentStepTraceResult
	{
		Trace::FireProductionFrozenForceAdvanceResult force;
		Trace::FireProductionCellPalindromeResult cell;
		Trace::FireProductionDualMomentumResult dual;
		std::vector<FireProductionRoundoffTrace::TraceFloat> conservativeValues;
		Trace::FireProductionProjectionResult physicalProjection;
		Trace::FireProductionProjectionResult projection;
		std::vector<FireProductionRoundoffTrace::Observation> stages;
	};

	inline void AppendStages(ResidentStepTraceResult& result,
		const FireProductionRoundoffTrace::Counters& counters)
	{
		result.stages.insert(result.stages.end(),counters.sealedStages.begin(),
			counters.sealedStages.end());
	}

	inline void SealProjectionOutputs(Trace::FireProductionProjectionResult& projection)
	{
		for(unsigned int axis=0u;axis<3u;++axis){
			FireProductionRoundoffTrace::ObserveAndReset(projection.faceDensityKGPerM3[axis]);
			FireProductionRoundoffTrace::ObserveMetricRangeAndReset(
				projection.velocityMPerS[axis],0u,projection.velocityMPerS[axis].size(),9u+axis);
			FireProductionRoundoffTrace::ObserveAndReset(projection.momentumKGPerM2S[axis]);
		}
		FireProductionRoundoffTrace::ObserveAndReset(projection.pressurePa);
		FireProductionRoundoffTrace::SealCurrentStage();
	}

	inline bool AdvanceResidentStepTrace(const RISE::FireProductionResidentStepRequest& request,
		const float outwardLambdaPerS,ResidentStepTraceResult& result,std::string* error)
	{
		result=ResidentStepTraceResult();ResidentStepTraceResult computed;
		Trace::FireProductionFrozenForceRequest force=Force(request.force);
		Trace::FireProductionCellPalindromeRequest cell=Cell(request.cellTransport);
		Trace::FireProductionDualMomentumRequest dual=Dual(request.dualTransport);
		FireProductionRoundoffTrace::Counters counters;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			if(!Trace::AdvanceFireProductionFrozenForceCPU(force,outwardLambdaPerS,
				computed.force,error))return false;
			FireProductionRoundoffTrace::SealStageAndReset(computed.force.momentumKGPerM2S);
		}
		AppendStages(computed,counters);
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			if(!Trace::RemapFireProductionCellPalindromeCPU(cell,computed.cell,error))return false;
		}
		AppendStages(computed,counters);
		for(unsigned int axis=0u;axis<3u;++axis)
			dual.beginningMomentum[axis]=computed.force.momentumKGPerM2S[axis];
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			if(!Trace::RemapFireProductionDualMomentumCPU(dual,computed.dual,error))return false;
		}
		AppendStages(computed,counters);
		computed.conservativeValues=computed.cell.conservativeValues;
		std::vector<FireProductionRoundoffTrace::TraceFloat> gas;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			if(computed.conservativeValues.size()!=request.cellSourceIncrement.size())return false;
			for(std::size_t value=0u;value<computed.conservativeValues.size();++value)
				computed.conservativeValues[value]+=
					FireProductionRoundoffTrace::TraceFloat(request.cellSourceIncrement[value]);
			for(unsigned int axis=0u;axis<3u;++axis){
				if(computed.dual.momentum[axis].size()!=request.momentumSourceIncrement[axis].size())
					return false;
				for(std::size_t face=0u;face<computed.dual.momentum[axis].size();++face)
					computed.dual.momentum[axis][face]+=
						FireProductionRoundoffTrace::TraceFloat(
							request.momentumSourceIncrement[axis][face]);
			}
			const std::size_t cells=force.shape.CellCount();
			if(cell.componentCount!=9u||computed.conservativeValues.size()!=9u*cells)return false;
			gas.resize(cells);
			for(std::size_t index=0u;index<cells;++index){
				gas[index]=computed.conservativeValues[cells+index];
				for(std::size_t component=2u;component<=6u;++component)
					gas[index]+=computed.conservativeValues[component*cells+index];
			}
			for(std::size_t component=0u;component<9u;++component)
				FireProductionRoundoffTrace::ObserveMetricRangeAndReset(
					computed.conservativeValues,component*cells,cells,
					static_cast<unsigned int>(component));
			FireProductionRoundoffTrace::ObserveAndReset(computed.dual.momentum[0]);
			FireProductionRoundoffTrace::ObserveAndReset(computed.dual.momentum[1]);
			FireProductionRoundoffTrace::ObserveAndReset(computed.dual.momentum[2]);
			FireProductionRoundoffTrace::ObserveAndReset(gas);
			FireProductionRoundoffTrace::SealCurrentStage();
		}
		AppendStages(computed,counters);
		Trace::FireProductionProjectionRequest projection;
		projection.shape=force.shape;projection.timeStepS=force.timeStepS;
		projection.ambientDensityKGPerM3=force.ambientDensityKGPerM3;
		projection.gasDensityKGPerM3=gas;
		projection.provisionalMomentumKGPerM2S=computed.dual.momentum;
		projection.divergenceTargetPerS=Promote(request.divergenceTargetPerS);
		for(unsigned int side=0u;side<6u;++side)projection.boundary[side]=force.boundary[side];
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			if(!Trace::ProjectFireProductionResidentPhysicalCPU(projection,
				computed.physicalProjection,error))return false;
			SealProjectionOutputs(computed.physicalProjection);
		}
		AppendStages(computed,counters);
		projection.provisionalMomentumKGPerM2S=computed.physicalProjection.momentumKGPerM2S;
		projection.divergenceTargetPerS=Promote(request.restorationDivergenceTargetPerS);
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			if(!Trace::ProjectFireProductionRestorationCPU(projection,computed.projection,error))
				return false;
			SealProjectionOutputs(computed.projection);
		}
		AppendStages(computed,counters);
		result=std::move(computed);return true;
	}
}

#endif
