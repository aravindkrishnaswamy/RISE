//////////////////////////////////////////////////////////////////////
//
//  FireRunCheckpoint.cpp - Fire run checkpoint codec and durable publication
//
//  Moved verbatim from tests/FireSequenceTest.cpp (simulation M2, track B).
//  Test-only mutations read FireRunCheckpointTestHooks, which production
//  never installs.  Compiled without fast-math or FP contraction (see the
//  per-file rules in build/make/rise/Makefile).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FireRunCheckpoint.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <system_error>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace RISE;


namespace RISE
{
	namespace FireRun
	{
	namespace
	{
		const FireRunCheckpointTestHooks* installedCheckpointTestHooks=nullptr;
		const FireRunCheckpointTestHooks& CheckpointTestHooks()
		{
			static const FireRunCheckpointTestHooks none;
			return installedCheckpointTestHooks?*installedCheckpointTestHooks:none;
		}
	}
	void SetFireRunCheckpointTestHooks(const FireRunCheckpointTestHooks* hooks)
	{
		installedCheckpointTestHooks=hooks;
	}
	namespace
	{
		FireRunCheckHandler installedCheckHandler=nullptr;
	}
	void SetFireRunCheckHandler(const FireRunCheckHandler handler)
	{
		installedCheckHandler=handler;
	}
	void ReportFireRunCheck(const bool condition,const char* message)
	{
		if(installedCheckHandler)installedCheckHandler(condition,message);
		else if(!condition)std::fprintf(stderr,"FAIL: %s\n",message);
	}

	const char* CurrentActiveSetAlgorithmVersion()
	{
		return "open_active_set_two_class_r81_v2";
	}
	const char* LegacyActiveSetAlgorithmVersion()
	{
		return "legacy_pre_r80_active_set";
	}

	// ------------------------------------------------------------------ frame values and state admission

	// Serializes SolverFrameValues scalars, 3D field vectors, and probe/step histories.
	bool WriteSolverFrameValues(CheckpointWriter& w,const SolverFrameValues& v)
	{
		if(!w.Pod(v.succeeded)||!w.String(v.structuredError)||!w.String(v.reductionMode))return false;
		for(const std::size_t dimension:v.dimensions){const std::uint64_t encoded=dimension;
			if(!w.Pod(encoded))return false;}
		if(!w.Pod(v.cellWidthM)||!w.String(v.caseRecordId)||!w.Pod(v.temperatureK)||
			!w.Pod(v.reactionWPerM3)||!WriteArithmeticVector(w,v.temperature)||
			!WriteArithmeticVector(w,v.reaction)||!WriteArithmeticVector(w,v.carbon))return false;
		const std::uint64_t velocityCount=v.velocity.size();if(!w.Pod(velocityCount))return false;
		for(const auto& velocity:v.velocity)for(const float component:velocity)
			if(!w.Pod(component))return false;
#define WRITE_CHECKPOINT_FIELD(field) if(!w.Pod(v.field))return false
		WRITE_CHECKPOINT_FIELD(realizedHeatReleaseW);WRITE_CHECKPOINT_FIELD(fuelConsumptionKGPerS);
		WRITE_CHECKPOINT_FIELD(realizedRadiativeFraction);WRITE_CHECKPOINT_FIELD(acceptedEscapeFactor);
		WRITE_CHECKPOINT_FIELD(selectedTimeStepS);WRITE_CHECKPOINT_FIELD(acceptedTimeStepS);
		WRITE_CHECKPOINT_FIELD(simulatedTimeS);WRITE_CHECKPOINT_FIELD(flowThroughTimeS);
		WRITE_CHECKPOINT_FIELD(pilotEnergyJ);WRITE_CHECKPOINT_FIELD(expectedPilotEnergyJ);
		WRITE_CHECKPOINT_FIELD(maximumTemperatureK);
		WRITE_CHECKPOINT_FIELD(maximumPilotApproachEOSResidual);
		WRITE_CHECKPOINT_FIELD(maximumAcceptedEOSResidual);
		WRITE_CHECKPOINT_FIELD(minimumActiveHoldTemperatureK);
		WRITE_CHECKPOINT_FIELD(maximumActiveHoldTemperatureK);
		WRITE_CHECKPOINT_FIELD(maximumLimiterClassDiscrepancy);
		WRITE_CHECKPOINT_FIELD(discontinuousLimiterClassSteps);
		WRITE_CHECKPOINT_FIELD(discontinuousClassThreadIdentity);
		WRITE_CHECKPOINT_FIELD(discontinuousClassThreadIdentityChecked);
		WRITE_CHECKPOINT_FIELD(statisticsStartS);WRITE_CHECKPOINT_FIELD(puffingFrequencyHz);
		WRITE_CHECKPOINT_FIELD(puffingRelativeError);WRITE_CHECKPOINT_FIELD(firstStatisticsStepStartS);
		WRITE_CHECKPOINT_FIELD(integratedHeatReleaseJ);WRITE_CHECKPOINT_FIELD(integratedRadiativeLossJ);
		WRITE_CHECKPOINT_FIELD(integratedFuelConsumptionKG);WRITE_CHECKPOINT_FIELD(integratedRadiativeFraction);
		WRITE_CHECKPOINT_FIELD(effectiveRadiativeFraction);WRITE_CHECKPOINT_FIELD(centerlineTemperatureExponent);
		WRITE_CHECKPOINT_FIELD(centerlineFitRMSE);WRITE_CHECKPOINT_FIELD(characteristicDiameterM);
		WRITE_CHECKPOINT_FIELD(mccaffreyFlameTipHeightM);
		WRITE_CHECKPOINT_FIELD(mccaffreyMaximumTemperatureRelativeError);
		WRITE_CHECKPOINT_FIELD(mccaffreyMaximumVelocityRelativeError);
		{const std::uint64_t count=v.mccaffreyPlumeStationCount;if(!w.Pod(count))return false;}
		WRITE_CHECKPOINT_FIELD(ignitedDuringPilot);WRITE_CHECKPOINT_FIELD(sustainedAfterPilot);
		WRITE_CHECKPOINT_FIELD(pilotHoldBandObserved);WRITE_CHECKPOINT_FIELD(pilotHoldBandSatisfied);
		WRITE_CHECKPOINT_FIELD(pilotApproachComplete);
		WRITE_CHECKPOINT_FIELD(statisticsBoundaryObserved);WRITE_CHECKPOINT_FIELD(checkpointCadenceWallS);
		WRITE_CHECKPOINT_FIELD(resumedFromCheckpoint);WRITE_CHECKPOINT_FIELD(resumedFromStep);
		WRITE_CHECKPOINT_FIELD(streamedFrameCount);
#undef WRITE_CHECKPOINT_FIELD
		return WriteArithmeticVector(w,v.probeTimeS)&&WriteArithmeticVector(w,v.probeCenterlineHeatReleaseW)&&
			WriteArithmeticVector(w,v.acceptedMaximumEOSResidualHistory)&&
			WriteArithmeticVector(w,v.acceptedTimeStepHistoryS)&&
			WriteArithmeticVector(w,v.stationProbeTimeS)&&WriteArithmeticVector(w,v.stationProbeHeightM)&&
			WriteArithmeticVector(w,v.stationProbeTemperatureK)&&WriteArithmeticVector(w,v.stationProbeReactionWPerM3)&&
			WriteArithmeticVector(w,v.stationProbeVerticalVelocityMPerS)&&
			WriteArithmeticVector(w,v.centerlineHeightM)&&WriteArithmeticVector(w,v.centerlineTemperatureK)&&
			WriteArithmeticVector(w,v.centerlineVelocityMPerS)&&
			WriteArithmeticVector(w,v.checkpointStepIndices)&&
			WriteArithmeticVector(w,v.workerCountHistory);
	}

	bool ReadSolverFrameValues(CheckpointReader& r,SolverFrameValues& v)
	{
		if(!r.Pod(v.succeeded)||!r.String(v.structuredError)||!r.String(v.reductionMode))return false;
		for(std::size_t& dimension:v.dimensions){std::uint64_t encoded=0u;
			if(!r.Pod(encoded)||encoded>std::numeric_limits<std::size_t>::max())return false;
			dimension=static_cast<std::size_t>(encoded);}
		if(!r.Pod(v.cellWidthM)||!r.String(v.caseRecordId)||!r.Pod(v.temperatureK)||
			!r.Pod(v.reactionWPerM3)||!ReadArithmeticVector(r,v.temperature)||
			!ReadArithmeticVector(r,v.reaction)||!ReadArithmeticVector(r,v.carbon))return false;
		std::uint64_t velocityCount=0u;if(!r.Pod(velocityCount)||velocityCount>200000000u)return false;
		v.velocity.resize(static_cast<std::size_t>(velocityCount));
		for(auto& velocity:v.velocity)for(float& component:velocity)if(!r.Pod(component))return false;
#define READ_CHECKPOINT_FIELD(field) if(!r.Pod(v.field))return false
		READ_CHECKPOINT_FIELD(realizedHeatReleaseW);READ_CHECKPOINT_FIELD(fuelConsumptionKGPerS);
		READ_CHECKPOINT_FIELD(realizedRadiativeFraction);READ_CHECKPOINT_FIELD(acceptedEscapeFactor);
		READ_CHECKPOINT_FIELD(selectedTimeStepS);READ_CHECKPOINT_FIELD(acceptedTimeStepS);
		READ_CHECKPOINT_FIELD(simulatedTimeS);READ_CHECKPOINT_FIELD(flowThroughTimeS);
		READ_CHECKPOINT_FIELD(pilotEnergyJ);READ_CHECKPOINT_FIELD(expectedPilotEnergyJ);
		READ_CHECKPOINT_FIELD(maximumTemperatureK);
		READ_CHECKPOINT_FIELD(maximumPilotApproachEOSResidual);
		READ_CHECKPOINT_FIELD(maximumAcceptedEOSResidual);
		READ_CHECKPOINT_FIELD(minimumActiveHoldTemperatureK);
		READ_CHECKPOINT_FIELD(maximumActiveHoldTemperatureK);
		READ_CHECKPOINT_FIELD(maximumLimiterClassDiscrepancy);
		READ_CHECKPOINT_FIELD(discontinuousLimiterClassSteps);
		READ_CHECKPOINT_FIELD(discontinuousClassThreadIdentity);
		READ_CHECKPOINT_FIELD(discontinuousClassThreadIdentityChecked);
		READ_CHECKPOINT_FIELD(statisticsStartS);READ_CHECKPOINT_FIELD(puffingFrequencyHz);
		READ_CHECKPOINT_FIELD(puffingRelativeError);READ_CHECKPOINT_FIELD(firstStatisticsStepStartS);
		READ_CHECKPOINT_FIELD(integratedHeatReleaseJ);READ_CHECKPOINT_FIELD(integratedRadiativeLossJ);
		READ_CHECKPOINT_FIELD(integratedFuelConsumptionKG);READ_CHECKPOINT_FIELD(integratedRadiativeFraction);
		READ_CHECKPOINT_FIELD(effectiveRadiativeFraction);READ_CHECKPOINT_FIELD(centerlineTemperatureExponent);
		READ_CHECKPOINT_FIELD(centerlineFitRMSE);READ_CHECKPOINT_FIELD(characteristicDiameterM);
		READ_CHECKPOINT_FIELD(mccaffreyFlameTipHeightM);
		READ_CHECKPOINT_FIELD(mccaffreyMaximumTemperatureRelativeError);
		READ_CHECKPOINT_FIELD(mccaffreyMaximumVelocityRelativeError);
		{std::uint64_t count=0u;if(!r.Pod(count)||count>std::numeric_limits<std::size_t>::max())return false;
			v.mccaffreyPlumeStationCount=static_cast<std::size_t>(count);}
		READ_CHECKPOINT_FIELD(ignitedDuringPilot);READ_CHECKPOINT_FIELD(sustainedAfterPilot);
		READ_CHECKPOINT_FIELD(pilotHoldBandObserved);READ_CHECKPOINT_FIELD(pilotHoldBandSatisfied);
		READ_CHECKPOINT_FIELD(pilotApproachComplete);
		READ_CHECKPOINT_FIELD(statisticsBoundaryObserved);READ_CHECKPOINT_FIELD(checkpointCadenceWallS);
		READ_CHECKPOINT_FIELD(resumedFromCheckpoint);READ_CHECKPOINT_FIELD(resumedFromStep);
		READ_CHECKPOINT_FIELD(streamedFrameCount);
#undef READ_CHECKPOINT_FIELD
		return ReadArithmeticVector(r,v.probeTimeS)&&ReadArithmeticVector(r,v.probeCenterlineHeatReleaseW)&&
			ReadArithmeticVector(r,v.acceptedMaximumEOSResidualHistory,1000000u)&&
			ReadArithmeticVector(r,v.acceptedTimeStepHistoryS,1000000u)&&
			ReadArithmeticVector(r,v.stationProbeTimeS)&&ReadArithmeticVector(r,v.stationProbeHeightM)&&
			ReadArithmeticVector(r,v.stationProbeTemperatureK)&&ReadArithmeticVector(r,v.stationProbeReactionWPerM3)&&
			ReadArithmeticVector(r,v.stationProbeVerticalVelocityMPerS)&&
			ReadArithmeticVector(r,v.centerlineHeightM)&&ReadArithmeticVector(r,v.centerlineTemperatureK)&&
			ReadArithmeticVector(r,v.centerlineVelocityMPerS)&&
			ReadArithmeticVector(r,v.checkpointStepIndices,1000000u)&&
			ReadArithmeticVector(r,v.workerCountHistory,1000000u);
	}

	// Verifies positive step-history consistency and exact sum equality with simulationTimeS.
	bool AcceptedCheckpointTimelineValid(const MethaneRunCheckpoint& checkpoint)
	{
		if(checkpoint.acceptedSteps==0u||
			checkpoint.values.acceptedTimeStepHistoryS.size()!=checkpoint.acceptedSteps||
			!std::isfinite(checkpoint.simulationTimeS)||checkpoint.simulationTimeS<=0.0||
			!std::isfinite(checkpoint.previousStepS)||checkpoint.previousStepS<=0.0||
			checkpoint.lastAcceptedStepS!=checkpoint.previousStepS||
			checkpoint.values.acceptedTimeStepHistoryS.empty()||
			checkpoint.values.acceptedTimeStepHistoryS.back()!=checkpoint.previousStepS)return false;
		double acceptedDurationS=0.0;
		for(const double timeStepS:checkpoint.values.acceptedTimeStepHistoryS){
			if(!std::isfinite(timeStepS)||timeStepS<=0.0)return false;
			acceptedDurationS+=timeStepS;
			if(!std::isfinite(acceptedDurationS))return false;
		}
		return acceptedDurationS==checkpoint.simulationTimeS;
	}

	// Checks that all cells are binary64-admissible and reproduce stored temperatureK via EOS inversion.
	bool Binary64CheckpointStateAdmissible(const MethaneRunCheckpoint& checkpoint)
	{
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		for(const MethaneCellState& state:checkpoint.states){
			double reconstructedTemperatureK=0.0;
			if(state.producerPrecision!=FireStateProducerPrecision::Binary64||
				!AcceptedMethaneCellStateAdmissible(state,fuel,
					FireStateProducerPrecision::Binary64)||
				!InvertMethaneTemperatureWithinAcceptedEnvelope(state,300.0,2300.0,fuel,
					FireStateProducerPrecision::Binary64,reconstructedTemperatureK)||
				reconstructedTemperatureK!=state.temperatureK)return false;
		}
		return !checkpoint.states.empty();
	}

	bool CanonicalizeLegacyBinary64CheckpointTemperature(MethaneRunCheckpoint& checkpoint)
	{
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		for(MethaneCellState& state:checkpoint.states){
			double reconstructedTemperatureK=0.0;
			if(state.producerPrecision!=FireStateProducerPrecision::Binary64||
				!AcceptedMethaneCellStateAdmissible(state,fuel,
					FireStateProducerPrecision::Binary64)||
				!InvertMethaneTemperatureWithinAcceptedEnvelope(state,300.0,2300.0,fuel,
					FireStateProducerPrecision::Binary64,reconstructedTemperatureK))return false;
			state.temperatureK=reconstructedTemperatureK;
		}
		return !checkpoint.states.empty();
	}

	std::uint64_t Binary64CheckpointPayloadDigest(const MethaneRunCheckpoint& checkpoint);

	bool IssueBinary64CheckpointAuthority(MethaneRunCheckpoint& checkpoint)
	{
		if(checkpoint.states.empty())return false;
		for(const MethaneCellState& state:checkpoint.states)
			if(state.producerPrecision!=FireStateProducerPrecision::Binary64)return false;
		if(
			!AcceptedCheckpointTimelineValid(checkpoint)||
			!Binary64CheckpointStateAdmissible(checkpoint))return false;
		checkpoint.binary64CheckpointAuthority.digest_=Binary64CheckpointPayloadDigest(checkpoint);
		return true;
	}

	bool RestoreBinary64CheckpointAuthority(MethaneRunCheckpoint& checkpoint,
		const std::uint64_t storedDigest)
	{
		if(storedDigest==0u||storedDigest!=Binary64CheckpointPayloadDigest(checkpoint))return false;
		checkpoint.binary64CheckpointAuthority.digest_=storedDigest;return true;
	}

	// Packs checkpoint state into exact binary32 conservative/MAC arrays, verifies
	// EOS temperature reconstruction, and computes the accepted-state payload digest.
	bool BuildCheckpointAcceptedStatePayload(const MethaneRunCheckpoint& checkpoint,
		FireProductionProjectionShape& shape,std::vector<float>& conservative,
		std::array<std::vector<float>,3>& momentum,
		std::array<std::vector<float>,3>& velocity,std::uint64_t& digest)
	{
		digest=0u;shape=FireProductionProjectionShape();conservative.clear();
		for(unsigned int axis=0u;axis<3u;++axis){momentum[axis].clear();velocity[axis].clear();}
		if(checkpoint.dimensions[0u]==0u||checkpoint.dimensions[1u]==0u||
			checkpoint.dimensions[2u]==0u||!std::isfinite(checkpoint.cellWidthM)||
			checkpoint.cellWidthM<=0.0)return false;
		shape.nx=checkpoint.dimensions[0u];shape.ny=checkpoint.dimensions[1u];
		shape.nz=checkpoint.dimensions[2u];shape.cellWidthM=static_cast<float>(checkpoint.cellWidthM);
		if(shape.CellCount()!=checkpoint.states.size())return false;
		for(unsigned int axis=0u;axis<3u;++axis){
			const std::size_t expectedFaces=FireProductionProjectionFaceCount(shape,axis);
			if(expectedFaces==0u||checkpoint.momentum.component[axis].size()!=expectedFaces||
				checkpoint.velocity.component[axis].size()!=expectedFaces){
				if(!CheckpointTestHooks().suppressExpectedCheckpointStateDiagnostic){
					std::fprintf(stderr,
					"checkpoint accepted face shape mismatch axis=%u expected=%zu momentum=%zu velocity=%zu\n",
					axis,expectedFaces,checkpoint.momentum.component[axis].size(),
					checkpoint.velocity.component[axis].size());}
				return false;
			}
		}
		conservative.assign(checkpoint.states.size()*9u,0.0f);
		std::vector<ConservativeVector> conservativeByCell(checkpoint.states.size());
		for(std::size_t cell=0u;cell<checkpoint.states.size();++cell){
			const MethaneCellState& state=checkpoint.states[cell];
			const ConservativeVector values=ToConservativeVector(state);
			conservativeByCell[cell]=values;
			for(std::size_t component=0u;component<9u;++component){
				const double value=values[component];const float represented=static_cast<float>(value);
				if(!std::isfinite(represented)||static_cast<double>(represented)!=value){
					std::fprintf(stderr,"checkpoint state is not exact binary32 cell=%zu component=%zu value=%.17g represented=%.17g\n",
						cell,component,value,static_cast<double>(represented));return false;}
				conservative[component*checkpoint.states.size()+cell]=represented;}
		}
		std::vector<double> reconstructedTemperature;
		std::string inversionError;
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		if(!InvertPeriodicTemperaturesWithinBounds(conservativeByCell,fuel,
			fuel.TemperatureMinK(),fuel.TemperatureMaxK(),
			FireStateProducerPrecision::Binary32,reconstructedTemperature,&inversionError,1u,false)||
			reconstructedTemperature.size()!=checkpoint.states.size()){
			if(!CheckpointTestHooks().suppressExpectedCheckpointStateDiagnostic){
				std::fprintf(stderr,
				"checkpoint accepted temperature reconstruction failed: %s\n",inversionError.c_str());}
			return false;
		}
		for(std::size_t cell=0u;cell<checkpoint.states.size();++cell){
			if(reconstructedTemperature[cell]!=checkpoint.states[cell].temperatureK){
				if(!CheckpointTestHooks().suppressExpectedCheckpointStateDiagnostic){
				std::fprintf(stderr,
				"checkpoint accepted temperature mismatch cell=%zu stored=%.17g reconstructed=%.17g\n",
				cell,checkpoint.states[cell].temperatureK,reconstructedTemperature[cell]);}
				return false;
			}
		}
		for(unsigned int axis=0u;axis<3u;++axis){
			momentum[axis].reserve(checkpoint.momentum.component[axis].size());
			velocity[axis].reserve(checkpoint.velocity.component[axis].size());
			for(const double value:checkpoint.momentum.component[axis]){
				const float represented=static_cast<float>(value);
				if(!std::isfinite(represented)||static_cast<double>(represented)!=value){
					std::fprintf(stderr,"checkpoint momentum is not exact binary32 axis=%u value=%.17g represented=%.17g\n",
						axis,value,static_cast<double>(represented));return false;}
				momentum[axis].push_back(represented);}
			for(const double value:checkpoint.velocity.component[axis]){
				const float represented=static_cast<float>(value);
				if(!std::isfinite(represented)||static_cast<double>(represented)!=value){
					std::fprintf(stderr,"checkpoint velocity is not exact binary32 axis=%u value=%.17g represented=%.17g\n",
						axis,value,static_cast<double>(represented));return false;}
				velocity[axis].push_back(represented);}
		}
		digest=FireProductionAcceptedStatePayloadDigest(shape,conservative,momentum,velocity);
		return digest!=0u;
	}

	bool CheckpointAcceptedStateDigest(const MethaneRunCheckpoint& checkpoint,
		std::uint64_t& digest)
	{
		FireProductionProjectionShape shape;std::vector<float> conservative;
		std::array<std::vector<float>,3> momentum,velocity;
		return BuildCheckpointAcceptedStatePayload(checkpoint,shape,conservative,
			momentum,velocity,digest);
	}

	// Computes canonical little-endian SHA-256 over case, timeline, manifold observation, and fields.
	bool CheckpointProductionBeginningSHA256(const MethaneRunCheckpoint& checkpoint,
		std::string& digest)
	{
		RISECBOR64::Bytes bytes;
		auto appendU64=[&](const std::uint64_t value){
			for(unsigned int byte=0u;byte<8u;++byte)
				bytes.push_back(static_cast<unsigned char>((value>>(8u*byte))&0xffu));};
		auto appendDouble=[&](const double value){appendU64(DoubleBits(value));};
		auto appendText=[&](const std::string& value){appendU64(value.size());
			bytes.insert(bytes.end(),value.begin(),value.end());};
		auto appendDoubles=[&](const std::vector<double>& values){appendU64(values.size());
			for(const double value:values)appendDouble(value);};
		const char schema[]="rise.fire.production.beginning.v2";
		bytes.insert(bytes.end(),schema,schema+sizeof(schema));
		appendText(checkpoint.caseRecordId);
		appendU64(checkpoint.dimensions[0]);appendU64(checkpoint.dimensions[1]);
		appendU64(checkpoint.dimensions[2]);appendDouble(checkpoint.cellWidthM);
		appendDouble(checkpoint.values.characteristicDiameterM);
		appendDouble(checkpoint.simulationTimeS);appendDouble(checkpoint.previousStepS);
		appendDouble(checkpoint.lastAcceptedStepS);appendU64(checkpoint.acceptedSteps);
		appendU64(checkpoint.values.acceptedTimeStepHistoryS.size());
		for(const double value:checkpoint.values.acceptedTimeStepHistoryS)appendDouble(value);
		appendU64(checkpoint.productionManifoldObservation.Available()?1u:0u);
		appendDouble(checkpoint.productionManifoldObservation.TimeStepS());
		appendDouble(checkpoint.productionManifoldObservation.MaximumGeneration());
		appendDouble(checkpoint.productionManifoldObservation.RestorationDrainFraction());
		appendU64(checkpoint.productionManifoldObservation.SerializedAcceptedStateDigest());
		appendU64(checkpoint.states.size());
		for(const MethaneCellState& state:checkpoint.states){
			appendU64(static_cast<std::uint64_t>(state.producerPrecision));
			appendDouble(state.temperatureK);
			const ConservativeVector conservative=ToConservativeVector(state);
			for(std::size_t component=0u;component<MethaneConservativeDimension;++component)
				appendDouble(conservative[component]);
		}
		for(unsigned int axis=0u;axis<3u;++axis)
			appendDoubles(checkpoint.momentum.component[axis]);
		for(unsigned int axis=0u;axis<3u;++axis)
			appendDoubles(checkpoint.velocity.component[axis]);
		digest=RISECBOR64::SHA256Hex(bytes);return digest.size()==64u;
	}

	bool CheckpointAcceptedStateMatchesObservation(const MethaneRunCheckpoint& checkpoint,
		const FireProductionAcceptedManifoldObservation& observation,
		std::uint64_t& legacyDigest)
	{
		FireProductionProjectionShape shape;std::vector<float> conservative;
		std::array<std::vector<float>,3> momentum,velocity;
		return BuildCheckpointAcceptedStatePayload(checkpoint,shape,conservative,
			momentum,velocity,legacyDigest)&&observation.MatchesAcceptedStatePayload(
				shape,conservative,momentum,velocity);
	}

	bool HomogeneousStateProducerPrecision(const std::vector<MethaneCellState>& states,
		FireStateProducerPrecision& precision)
	{
		if(states.empty())return false;
		precision=states.front().producerPrecision;
		if(precision!=FireStateProducerPrecision::Binary64&&
			precision!=FireStateProducerPrecision::Binary32)return false;
		return std::all_of(states.begin(),states.end(),[precision](const MethaneCellState& state){
			return state.producerPrecision==precision;
		});
	}

	bool WriteCellStates(CheckpointWriter& writer,const std::vector<MethaneCellState>& states,
		const std::uint64_t version)
	{
		FireStateProducerPrecision precision=FireStateProducerPrecision::Unknown;
		if(!HomogeneousStateProducerPrecision(states,precision)||
			(version<9u&&precision!=FireStateProducerPrecision::Binary64))return false;
		const std::uint64_t count=states.size();if(!writer.Pod(count))return false;
		for(const MethaneCellState& state:states){
			if(version>=9u){const unsigned char producerPrecision=
				static_cast<unsigned char>(state.producerPrecision);
				if(!writer.Pod(producerPrecision))return false;}
			if(!writer.Pod(state.rhoTotalZ))return false;
			for(const double value:state.constituent)if(!writer.Pod(value))return false;
			if(!writer.Pod(state.sensibleEnergyJPerM3)||!writer.Pod(state.temperatureK))return false;
		}
		return true;
	}

	bool ReadCellStates(CheckpointReader& reader,std::vector<MethaneCellState>& states,
		const std::uint64_t version)
	{
		std::uint64_t count=0u;if(!reader.Pod(count)||count>100000000u)return false;
		states.resize(static_cast<std::size_t>(count));
		for(MethaneCellState& state:states){
			if(version>=9u){unsigned char producerPrecision=0u;
				if(!reader.Pod(producerPrecision)||(producerPrecision!=
					static_cast<unsigned char>(FireStateProducerPrecision::Binary64)&&
					producerPrecision!=static_cast<unsigned char>(
						FireStateProducerPrecision::Binary32)))return false;
				state.producerPrecision=static_cast<FireStateProducerPrecision>(producerPrecision);
			}else state.producerPrecision=FireStateProducerPrecision::Binary64;
			if(!reader.Pod(state.rhoTotalZ))return false;
			for(double& value:state.constituent)if(!reader.Pod(value))return false;
			if(!reader.Pod(state.sensibleEnergyJPerM3)||!reader.Pod(state.temperatureK))return false;
		}
		FireStateProducerPrecision precision=FireStateProducerPrecision::Unknown;
		return HomogeneousStateProducerPrecision(states,precision);
	}


	// ------------------------------------------------------------------ versioned checkpoint payload (v5..v13)

	// Serializes schema prefix fields across checkpoint versions (base v5, migration v6, active-set v7-8).
	bool WriteCheckpointSerializedPrefix(CheckpointWriter& writer,
		const MethaneRunCheckpoint& checkpoint,const std::uint64_t version)
	{
		if(!writer.String(checkpoint.caseRecordId)||!writer.String(checkpoint.producerBuildId))return false;
		for(const std::size_t dimension:checkpoint.dimensions){const std::uint64_t encoded=dimension;
			if(!writer.Pod(encoded))return false;}
		const bool baseWritten=writer.Pod(checkpoint.cellWidthM)&&
			WriteCellStates(writer,checkpoint.states,version)&&
			WriteMACField(writer,checkpoint.momentum)&&WriteMACField(writer,checkpoint.velocity)&&
			WriteSolverFrameValues(writer,checkpoint.values)&&
			WriteArithmeticVector(writer,checkpoint.centerlineTemperatureIntegral)&&
			WriteArithmeticVector(writer,checkpoint.centerlineVelocityIntegral)&&
			WriteArithmeticVector(writer,checkpoint.planeHeatReleaseIntegral)&&
			writer.Pod(checkpoint.centerlineStatisticsDurationS)&&
			writer.Pod(checkpoint.simulationTimeS)&&writer.Pod(checkpoint.previousStepS)&&
			writer.Pod(checkpoint.lastAcceptedStepS)&&writer.Pod(checkpoint.acceptedSteps);
		if(!baseWritten||version<6u)return baseWritten;
		const bool migrationWritten=
			WriteArithmeticVector(writer,checkpoint.values.acceptedMaximumTemperatureHistoryK)&&
			writer.String(checkpoint.values.migrationCertificateId)&&
			writer.String(checkpoint.values.migrationOldBuildId)&&
			writer.String(checkpoint.values.migrationNewBuildId)&&
			writer.Pod(checkpoint.values.migrationAcceptedStepCount)&&
			writer.Pod(checkpoint.values.migrationResumedFromStep);
		if(!migrationWritten||version<7u)return migrationWritten;
		const bool activeSetWritten=writer.String(checkpoint.values.activeSetAlgorithmVersion)&&
			writer.Pod(checkpoint.values.maximumActiveSetComplementarityDiscrepancyMPerS)&&
			writer.Pod(checkpoint.values.discontinuousActiveSetEvents)&&
			writer.Pod(checkpoint.values.maximumActiveSetCycleLength)&&
			writer.Pod(checkpoint.values.maximumActiveSetDifferingFaceCount)&&
			writer.Pod(checkpoint.values.activeSetThreadIdentity)&&
			writer.Pod(checkpoint.values.activeSetThreadIdentityChecked);
		if(!activeSetWritten)return false;
		return version<8u||writer.String(checkpoint.values.priorActiveSetAlgorithmVersion);
	}

	std::uint64_t Binary64CheckpointPayloadDigest(const MethaneRunCheckpoint& checkpoint)
	{
		CheckpointWriter writer;
		if(!WriteCheckpointSerializedPrefix(writer,checkpoint,13u))return 0u;
		const std::uint64_t digest=writer.Checksum();
		return digest==0u?1u:digest;
	}

	// Validates timeline, precision/authority, and manifold lifecycle invariants, then
	// writes the versioned checkpoint payload and v12+ state/checksum binding trailer.
	bool WriteCheckpointPayload(CheckpointWriter& writer,const MethaneRunCheckpoint& checkpoint,
		const std::uint64_t version)
	{
		FireStateProducerPrecision precision=FireStateProducerPrecision::Unknown;
		if(!HomogeneousStateProducerPrecision(checkpoint.states,precision))return false;
		if(checkpoint.acceptedSteps==0u&&!CheckpointTestHooks().forceMalformedManifoldLifecycleWrite)
			return false;
		if(checkpoint.acceptedSteps>0u&&
			(!AcceptedCheckpointTimelineValid(checkpoint)||
				(precision==FireStateProducerPrecision::Binary64&&
					(!Binary64CheckpointStateAdmissible(checkpoint)||
						!checkpoint.binary64CheckpointAuthority.Available()||
						checkpoint.binary64CheckpointAuthority.Digest()!=
							Binary64CheckpointPayloadDigest(checkpoint))))&&
			!CheckpointTestHooks().forceMalformedManifoldLifecycleWrite)return false;
		if(version<12u&&precision==FireStateProducerPrecision::Binary32&&
			!CheckpointTestHooks().forceMalformedManifoldLifecycleWrite)return false;
		if(version>=9u&&version<13u&&precision==FireStateProducerPrecision::Binary64&&
			checkpoint.acceptedSteps>0u&&!CheckpointTestHooks().forceMalformedManifoldLifecycleWrite)return false;
		if(!WriteCheckpointSerializedPrefix(writer,checkpoint,version)){
			if(!CheckpointTestHooks().suppressExpectedCheckpointStateDiagnostic)
				std::fprintf(stderr,"checkpoint serialization refused at serialized-prefix\n");
			return false;
		}
		if(version<10u){
			if(version==9u&&precision==FireStateProducerPrecision::Binary32&&
				(checkpoint.acceptedSteps!=0u||checkpoint.simulationTimeS!=0.0||
				checkpoint.previousStepS!=0.0||checkpoint.lastAcceptedStepS!=0.0||
				!checkpoint.values.acceptedTimeStepHistoryS.empty())&&
				!CheckpointTestHooks().forceMalformedManifoldLifecycleWrite)return false;
			return true;
		}
		const FireProductionAcceptedManifoldObservation& observation=
			checkpoint.productionManifoldObservation;
		const bool productionState=precision==FireStateProducerPrecision::Binary32;
		const bool acceptedProductionState=productionState&&checkpoint.acceptedSteps>0u;
		const bool invalidManifoldLifecycle=(observation.Available()&&(!std::isfinite(observation.TimeStepS())||
			observation.TimeStepS()<=0.0||!std::isfinite(observation.MaximumGeneration())||
			observation.MaximumGeneration()<0.0||
			!std::isfinite(observation.RestorationDrainFraction())||
			observation.RestorationDrainFraction()<0.0||
				observation.RestorationDrainFraction()>1.0||!acceptedProductionState||
				observation.TimeStepS()!=checkpoint.previousStepS||
				observation.TimeStepS()!=checkpoint.lastAcceptedStepS||
				checkpoint.values.acceptedTimeStepHistoryS.empty()||
				checkpoint.values.acceptedTimeStepHistoryS.back()!=checkpoint.previousStepS))||
			(!observation.Available()&&(observation.TimeStepS()!=0.0||
				observation.MaximumGeneration()!=0.0||
				observation.RestorationDrainFraction()!=0.0||
				(version>=12u&&acceptedProductionState)))||
			(productionState&&checkpoint.acceptedSteps==0u&&
				(checkpoint.previousStepS!=0.0||checkpoint.lastAcceptedStepS!=0.0||
				!checkpoint.values.acceptedTimeStepHistoryS.empty()))||
			(productionState&&checkpoint.acceptedSteps>0u&&
				!AcceptedCheckpointTimelineValid(checkpoint));
		if(invalidManifoldLifecycle&&!CheckpointTestHooks().forceMalformedManifoldLifecycleWrite){
			if(!CheckpointTestHooks().suppressExpectedCheckpointStateDiagnostic)
				std::fprintf(stderr,"checkpoint serialization refused at manifold-lifecycle\n");
			return false;
		}
		const unsigned char manifoldAvailable=
			observation.Available()?1u:0u;
		const bool observationWritten=writer.Pod(manifoldAvailable)&&
			writer.Pod(observation.TimeStepS())&&
			writer.Pod(observation.MaximumGeneration())&&
			writer.Pod(observation.RestorationDrainFraction());
		if(!observationWritten)return false;
		if(version==11u){const std::uint64_t retiredTupleSeal=0u;
			return writer.Pod(retiredTupleSeal);}
		if(version<12u)return true;
		std::uint64_t stateDigest=0u;
		if(observation.Available()&&!CheckpointAcceptedStateMatchesObservation(
			checkpoint,observation,stateDigest)){
			if(!CheckpointTestHooks().suppressExpectedCheckpointStateDiagnostic)
				std::fprintf(stderr,"checkpoint serialization refused at accepted-state binding\n");
			return false;
		}
		if(version>=13u&&precision==FireStateProducerPrecision::Binary64&&
			checkpoint.acceptedSteps>0u){
			stateDigest=checkpoint.binary64CheckpointAuthority.Digest();
			if((stateDigest==0u||stateDigest!=Binary64CheckpointPayloadDigest(checkpoint))&&
				!CheckpointTestHooks().forceMalformedManifoldLifecycleWrite)return false;
		}
		if(!writer.Pod(stateDigest))return false;
		const std::uint64_t payloadBinding=writer.Checksum()^
			UINT64_C(0x63b96d44f1a72ec8);
		return writer.Pod(payloadBinding);
	}

	bool ReadCheckpointPayload(CheckpointReader& reader,MethaneRunCheckpoint& checkpoint,
		const std::uint64_t version)
	{
		checkpoint.productionManifoldObservation=
			FireProductionAcceptedManifoldObservation();
		if(!reader.String(checkpoint.caseRecordId)||!reader.String(checkpoint.producerBuildId))return false;
		for(std::size_t& dimension:checkpoint.dimensions){std::uint64_t encoded=0u;
			if(!reader.Pod(encoded)||encoded>std::numeric_limits<std::size_t>::max())return false;
			dimension=static_cast<std::size_t>(encoded);}
		const bool decoded=reader.Pod(checkpoint.cellWidthM)&&
			ReadCellStates(reader,checkpoint.states,version)&&
			ReadMACField(reader,checkpoint.momentum)&&ReadMACField(reader,checkpoint.velocity)&&
			ReadSolverFrameValues(reader,checkpoint.values)&&
			ReadArithmeticVector(reader,checkpoint.centerlineTemperatureIntegral)&&
			ReadArithmeticVector(reader,checkpoint.centerlineVelocityIntegral)&&
			ReadArithmeticVector(reader,checkpoint.planeHeatReleaseIntegral)&&
			reader.Pod(checkpoint.centerlineStatisticsDurationS)&&
			reader.Pod(checkpoint.simulationTimeS)&&reader.Pod(checkpoint.previousStepS)&&
			reader.Pod(checkpoint.lastAcceptedStepS)&&reader.Pod(checkpoint.acceptedSteps);
		if(!decoded)return false;
		if(version>=6u&&(!ReadArithmeticVector(reader,
			checkpoint.values.acceptedMaximumTemperatureHistoryK,1000000u)||
			!reader.String(checkpoint.values.migrationCertificateId)||
			!reader.String(checkpoint.values.migrationOldBuildId)||
			!reader.String(checkpoint.values.migrationNewBuildId)||
			!reader.Pod(checkpoint.values.migrationAcceptedStepCount)||
			!reader.Pod(checkpoint.values.migrationResumedFromStep)))return false;
		if(version>=7u&&(!reader.String(checkpoint.values.activeSetAlgorithmVersion)||
			!reader.Pod(checkpoint.values.maximumActiveSetComplementarityDiscrepancyMPerS)||
			!reader.Pod(checkpoint.values.discontinuousActiveSetEvents)||
			!reader.Pod(checkpoint.values.maximumActiveSetCycleLength)||
			!reader.Pod(checkpoint.values.maximumActiveSetDifferingFaceCount)||
			!reader.Pod(checkpoint.values.activeSetThreadIdentity)||
			!reader.Pod(checkpoint.values.activeSetThreadIdentityChecked)))return false;
		if(version>=8u&&!reader.String(checkpoint.values.priorActiveSetAlgorithmVersion))return false;
		if(version>=10u){unsigned char manifoldAvailable=0u;double timeStepS=0.0;
			double maximumGeneration=0.0,restorationDrainFraction=0.0;
			if(!reader.Pod(manifoldAvailable)||manifoldAvailable>1u||!reader.Pod(timeStepS)||
				!reader.Pod(maximumGeneration)||!reader.Pod(restorationDrainFraction))return false;
			std::uint64_t retiredOrStateDigest=0u;
			if(version>=11u&&!reader.Pod(retiredOrStateDigest))return false;
			if(version>=12u){std::uint64_t payloadBinding=0u;
				if(!reader.Pod(payloadBinding))return false;}
			if(version>=13u&&!manifoldAvailable&&checkpoint.acceptedSteps>0u&&
				!RestoreBinary64CheckpointAuthority(checkpoint,retiredOrStateDigest))return false;
		}
		checkpoint.checkpointFormatVersion=version;
		if(version<7u)checkpoint.values.activeSetAlgorithmVersion=
			LegacyActiveSetAlgorithmVersion();
		return true;
	}

	// ------------------------------------------------------------------ durable sync and Merkle-sealed publication

	// Flushes file contents (and parent directory on POSIX) to durable storage.
	bool DurableSyncFileAndDirectory(const std::filesystem::path& path,std::string& error)
	{
#if defined(_WIN32)
		HANDLE file=CreateFileW(path.wstring().c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,
			OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
		if(file==INVALID_HANDLE_VALUE){error="cannot open durable run file";return false;}
		const bool flushed=FlushFileBuffers(file)!=0;
		const bool closed=CloseHandle(file)!=0;
		if(!flushed||!closed)error="cannot flush durable run file";
		return flushed&&closed;
#else
		const int fd=::open(path.c_str(),O_RDONLY);
		if(fd<0){error="cannot open durable run file";return false;}
		const bool fileSynced=::fsync(fd)==0;
		const bool fileClosed=::close(fd)==0;
		if(!fileSynced||!fileClosed){error="cannot fsync durable run file";return false;}
		const std::filesystem::path directory=path.has_parent_path()?path.parent_path():".";
		const int directoryFd=::open(directory.c_str(),O_RDONLY);
		if(directoryFd<0){error="cannot open durable run directory";return false;}
		const bool directorySynced=::fsync(directoryFd)==0;
		const bool directoryClosed=::close(directoryFd)==0;
		if(!directorySynced||!directoryClosed){error="cannot fsync durable run directory";return false;}
		return true;
#endif
	}

	bool AtomicReplaceCheckpoint(const std::filesystem::path& temporary,
		const std::filesystem::path& target,std::string& error)
	{
#if defined(_WIN32)
		if(!MoveFileExW(temporary.wstring().c_str(),target.wstring().c_str(),
			MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
			error="cannot atomically replace run checkpoint";return false;}
		if(CheckpointTestHooks().forcePostRenameDirectorySyncFailure){
			error="cannot fsync replaced-run directory";return false;}
		return true;
#else
		if(::rename(temporary.c_str(),target.c_str())!=0){error="cannot atomically replace run checkpoint";return false;}
		const std::filesystem::path directory=target.has_parent_path()?target.parent_path():".";
		const int directoryFd=::open(directory.c_str(),O_RDONLY);
		if(directoryFd<0){error="cannot open replaced-run directory";return false;}
		const bool directorySynced=!CheckpointTestHooks().forcePostRenameDirectorySyncFailure&&
			::fsync(directoryFd)==0;
		const bool directoryClosed=::close(directoryFd)==0;
		if(!directorySynced||!directoryClosed){error="cannot fsync replaced-run directory";return false;}
		return true;
#endif
	}

	bool BuildPayloadMerkleCertificateBytes(const RISECBOR64::Bytes& bytes,
		const std::string& caseRecordId,std::string& certificateText,std::string& error)
	{
		if(caseRecordId.size()!=64u||!std::all_of(caseRecordId.begin(),caseRecordId.end(),
			[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');})){
			error="payload publication case identity is invalid";return false;}
		FireProductionPayloadDigestV2 digest;
		if(!FireProductionPayloadDigestCPU(bytes.data(),bytes.size(),8u,digest,&error))return false;
		std::ostringstream certificate;
		const std::string prefix="{\"schema\":\"rise.fire.published-payload.v2\",\"case_record_id\":\""+caseRecordId+"\"";
		certificate<<prefix
			<<",\"sha256_v1\":\""<<RISECBOR64::SHA256Hex(bytes)<<"\",\"v2\":{"
			<<"\"digest_format\":\"rise-payload-sha256-merkle\",\"digest_version\":2,"
			<<"\"chunk_bytes\":4096,\"fan_in\":16,\"payload_bytes\":"<<bytes.size()
			<<",\"root_sha256\":\""<<digest.rootSHA256<<"\"}}\n";
		certificateText=certificate.str();return true;
	}

	bool BuildPayloadMerkleCertificate(const std::filesystem::path& path,
		const std::string& caseRecordId,std::string& certificateText,std::string& error)
	{
		const auto bytes=ReadFileBytes(path);
		if(!std::filesystem::is_regular_file(path)||bytes.size()!=std::filesystem::file_size(path)){
			error="payload publication is incomplete";return false;}
		return BuildPayloadMerkleCertificateBytes(bytes,caseRecordId,certificateText,error);
	}

	bool PublishPayloadMerkleSidecar(const std::filesystem::path& path,
		const std::string& caseRecordId,std::string& error)
	{
		std::string certificate;if(!BuildPayloadMerkleCertificate(path,caseRecordId,certificate,error))return false;
		const auto sidecar=std::filesystem::path(path.string()+".payload-v2.json");
		const auto temporary=std::filesystem::path(sidecar.string()+".pending");
		if(std::filesystem::is_symlink(sidecar)||std::filesystem::exists(temporary)||
			std::filesystem::is_symlink(temporary)){
			error="publication refuses occupied seal preparation";return false;}
		if(std::filesystem::exists(sidecar)){
			const auto priorBytes=ReadFileBytes(sidecar);const std::string prior(priorBytes.begin(),priorBytes.end());
			if(prior==certificate)return true;
			error="publication refuses to replace an existing artifact seal";return false;
		}
		std::ofstream output(temporary,std::ios::binary|std::ios::trunc);output<<certificate;
		output.close();if(!output){error="payload publication seal write failed";return false;}
		return DurableSyncFileAndDirectory(temporary,error)&&AtomicReplaceCheckpoint(temporary,sidecar,error);
	}


	bool ExistingPayloadCertificateValid(const std::filesystem::path& path,
		const std::string& caseRecordId,std::string& error)
	{
		const auto pending=std::filesystem::path(path.string()+".preparation.pending.payload-v2.json");
		const auto sidecar=std::filesystem::path(path.string()+".payload-v2.json");
		if(std::filesystem::exists(pending)||std::filesystem::is_symlink(pending)){
			error="payload publication pair is incomplete";return false;}
		// Historical checkpoints without v2 certificates retain their existing
		// versioned admission path; an extant v2 certificate must verify.
		if(!std::filesystem::exists(sidecar)&&!std::filesystem::is_symlink(sidecar))return true;
		if(std::filesystem::is_symlink(sidecar)){error="payload certificate is aliased";return false;}
		std::string expected;if(!BuildPayloadMerkleCertificate(path,caseRecordId,expected,error))return false;
		const auto bytes=ReadFileBytes(sidecar);
		if(std::string(bytes.begin(),bytes.end())!=expected){error="payload certificate does not match";return false;}
		return true;
	}
	void CleanupPreparedPayload(const std::filesystem::path& prepared,const bool published)
	{
		const bool unrenamed=std::filesystem::exists(prepared);
		std::error_code cleanupError;std::filesystem::remove(prepared,cleanupError);
		// If the payload rename occurred but the pair did not finish, keep the
		// prepared certificate as an explicit incomplete-publication marker.
		if(published||unrenamed)std::filesystem::remove(prepared.string()+".payload-v2.json",cleanupError);
	}
	// Atomically publishes a staged payload and its .payload-v2.json Merkle certificate
	// pair, enforcing immutability unless policy is MutableCurrentCheckpoint.
	bool PublishPreparedPayload(const std::filesystem::path& prepared,
		const std::filesystem::path& target,const std::string& caseRecordId,
		const PayloadPublicationPolicy policy,std::string& error)
	{
		const std::filesystem::path sidecar=target.string()+".payload-v2.json";
		const std::filesystem::path preparedSeal=prepared.string()+".payload-v2.json";
		const bool exists=std::filesystem::exists(target),sealed=std::filesystem::exists(sidecar);
		if(prepared==target||std::filesystem::is_symlink(prepared)||exists!=sealed||std::filesystem::is_symlink(target)||
			std::filesystem::is_symlink(sidecar)){
			error="publication refuses incomplete or aliased prior output";return false;}
		std::string nextCertificate;
		if(!BuildPayloadMerkleCertificate(prepared,caseRecordId,nextCertificate,error))return false;
		if(exists){
			std::string priorCertificate;
			if(!BuildPayloadMerkleCertificate(target,caseRecordId,priorCertificate,error))return false;
			const auto priorBytes=ReadFileBytes(sidecar);
			if(std::string(priorBytes.begin(),priorBytes.end())!=priorCertificate){
				error="publication refuses corrupt or foreign prior output";return false;}
			if(priorCertificate==nextCertificate)return true;
			if(policy!=PayloadPublicationPolicy::MutableCurrentCheckpoint){
				error="publication refuses changed immutable payload";return false;}
		}
		// Finish and sync both new files before either public name changes. An
		// interrupted pair is refused by its digest; no mismatched pair is accepted.
		if(!PublishPayloadMerkleSidecar(prepared,caseRecordId,error))return false;
		return AtomicReplaceCheckpoint(prepared,target,error)&&
			AtomicReplaceCheckpoint(preparedSeal,sidecar,error);
	}

	// A resident continuation needs a complete published pair at its mutable
	// cursor before the first replacement. Copy raw accepted bytes, never
	// reserialize them, and use the same two-file publication boundary as steps.
	bool InitializePublishedCheckpointCopy(const std::filesystem::path& source,
		const std::filesystem::path& target,const std::string& caseRecordId,std::string& error)
	{
		const std::filesystem::path sourceSeal=source.string()+".payload-v2.json";
		const std::filesystem::path prepared=target.string()+".preparation.pending";
		if(source==target||std::filesystem::is_symlink(source)||
			!std::filesystem::is_regular_file(sourceSeal)||std::filesystem::is_symlink(sourceSeal)||
			std::filesystem::exists(target)||std::filesystem::is_symlink(target)||
			std::filesystem::exists(prepared)||std::filesystem::is_symlink(prepared)||
			std::filesystem::exists(prepared.string()+".payload-v2.json")||
			std::filesystem::is_symlink(prepared.string()+".payload-v2.json")||
			!ExistingPayloadCertificateValid(source,caseRecordId,error)){
			error="continuation requires a sealed source and fresh publication cursor";return false;}
		std::error_code copyError;
		if(!std::filesystem::copy_file(source,prepared,std::filesystem::copy_options::none,copyError)){
			error="cannot stage accepted checkpoint bytes";return false;}
		const bool identical=DigestFile(prepared)==DigestFile(source);
		const bool published=identical&&DurableSyncFileAndDirectory(prepared,error)&&
			PublishPreparedPayload(prepared,target,caseRecordId,PayloadPublicationPolicy::Immutable,error);
		CleanupPreparedPayload(prepared,published);
		if(!identical)error="continuation checkpoint byte copy mismatch";
		return published;
	}

	// Finalization boundary for a newly created run directory. Mutable working
	// logs are not published evidence until this succeeds; sidecars are terminal
	// certificates, not inputs to an infinitely recursive sidecar tree.
	bool SealPublishedRunDirectory(const std::filesystem::path& directory,
		const std::string& caseRecordId,std::string& error)
	{
		std::vector<std::filesystem::path> files;
		for(const auto& entry:std::filesystem::recursive_directory_iterator(directory)){
			if(entry.is_symlink()){error="run publication refuses symlinks";return false;}
			const std::string name=entry.path().filename().string();
			if(name.find(".pending")!=std::string::npos){error="run publication has a pending output";return false;}
			if(name.size()>=16u&&name.compare(name.size()-16u,16u,".payload-v2.json")==0){
				const auto base=std::filesystem::path(entry.path().string().substr(0u,
					entry.path().string().size()-16u));
				if(!entry.is_regular_file()||!std::filesystem::is_regular_file(base)||std::filesystem::is_symlink(base)){
					error="run publication refuses orphan sidecar";return false;}
				continue;
			}
			if(!entry.is_regular_file())continue;
			files.push_back(entry.path());
		}
		std::sort(files.begin(),files.end());
		if(files.empty()){error="run publication has no payloads";return false;}
		for(const auto& file:files)if(!PublishPayloadMerkleSidecar(file,caseRecordId,error))return false;
		return true;
	}

	// ------------------------------------------------------------------ checkpoint save and validated load

	// Writes a 48-byte header ("RISEFIRECHKPT1", version, endian, length, FNV-1a)
	// plus payload to a PID-scoped temp file, syncs, and atomically replaces target.
	bool SaveMethaneRunCheckpoint(const std::filesystem::path& path,
		const MethaneRunCheckpoint& checkpoint,std::string& error,
		const std::uint64_t version)
	{
		if(version<5u||version>13u){error="run checkpoint output version is invalid";return false;}
		if(path.has_parent_path())std::filesystem::create_directories(path.parent_path());
#if defined(_WIN32)
		const long long processId=static_cast<long long>(::_getpid());
#else
		const long long processId=static_cast<long long>(::getpid());
#endif
		const std::filesystem::path temporary=path.string()+".tmp."+std::to_string(processId);
		CheckpointWriter writer(temporary);if(!writer.Good()){error="cannot open run checkpoint";return false;}
		const char magic[16]={'R','I','S','E','F','I','R','E','C','H','K','P','T','1',0,0};
		const std::uint64_t endian=0x0102030405060708ull,zero=0u;
		auto rejectTemporary=[&temporary](){std::error_code ignored;
			std::filesystem::remove(temporary,ignored);};
		if(!writer.HeaderBytes(magic,sizeof(magic))||!writer.HeaderBytes(&version,sizeof(version))||
			!writer.HeaderBytes(&endian,sizeof(endian))||!writer.HeaderBytes(&zero,sizeof(zero))||
			!writer.HeaderBytes(&zero,sizeof(zero))||!WriteCheckpointPayload(writer,checkpoint,version)){
			error="cannot serialize complete run checkpoint";rejectTemporary();return false;}
		const std::uint64_t payloadBytes=writer.PayloadBytes(),checksum=writer.Checksum();
		if(!writer.SeekHeader(32)||!writer.HeaderBytes(&payloadBytes,sizeof(payloadBytes))||
			!writer.HeaderBytes(&checksum,sizeof(checksum))||!writer.Finish()){
			error="cannot finalize run checkpoint";rejectTemporary();return false;}
		if(!DurableSyncFileAndDirectory(temporary,error)||
			!AtomicReplaceCheckpoint(temporary,path,error)){rejectTemporary();return false;}
		return true;
	}

	// Same writes, in the same order, as SaveMethaneRunCheckpoint, into an
	// in-memory file instead of a temporary path (no publication).
	bool SerializeMethaneRunCheckpoint(const MethaneRunCheckpoint& checkpoint,
		RISECBOR64::Bytes& file,std::string& error,const std::uint64_t version)
	{
		if(version<5u||version>13u){error="run checkpoint output version is invalid";return false;}
		RISECBOR64::Bytes candidate;
		CheckpointWriter writer(InMemoryCheckpointFile{},candidate);
		const char magic[16]={'R','I','S','E','F','I','R','E','C','H','K','P','T','1',0,0};
		const std::uint64_t endian=0x0102030405060708ull,zero=0u;
		if(!writer.HeaderBytes(magic,sizeof(magic))||!writer.HeaderBytes(&version,sizeof(version))||
			!writer.HeaderBytes(&endian,sizeof(endian))||!writer.HeaderBytes(&zero,sizeof(zero))||
			!writer.HeaderBytes(&zero,sizeof(zero))||!WriteCheckpointPayload(writer,checkpoint,version)){
			error="cannot serialize complete run checkpoint";return false;}
		const std::uint64_t payloadBytes=writer.PayloadBytes(),checksum=writer.Checksum();
		if(!writer.SeekHeader(32)||!writer.HeaderBytes(&payloadBytes,sizeof(payloadBytes))||
			!writer.HeaderBytes(&checksum,sizeof(checksum))||!writer.Finish()){
			error="cannot finalize run checkpoint";return false;}
		file.swap(candidate);
		return true;
	}

	PayloadPublicationPolicy CheckpointPublicationPolicy(const std::filesystem::path& output,
		const std::filesystem::path& current,const bool finalDue,const bool equivalenceDue)
	{
		return !finalDue&&!equivalenceDue&&!current.empty()&&output==current?
			PayloadPublicationPolicy::MutableCurrentCheckpoint:PayloadPublicationPolicy::Immutable;
	}

	bool SavePublishedCheckpoint(const std::filesystem::path& path,
		const MethaneRunCheckpoint& checkpoint,const PayloadPublicationPolicy policy,std::string& error)
	{
		const std::filesystem::path prepared=path.string()+".preparation.pending";
		const std::filesystem::path preparedSeal=prepared.string()+".payload-v2.json";
		if(std::filesystem::exists(prepared)||std::filesystem::exists(preparedSeal)||
			std::filesystem::is_symlink(prepared)||std::filesystem::is_symlink(preparedSeal)){
			error="checkpoint preparation already exists";return false;}
		const bool saved=SaveMethaneRunCheckpoint(prepared,checkpoint,error)&&
			PublishPreparedPayload(prepared,path,checkpoint.caseRecordId,policy,error);
		CleanupPreparedPayload(prepared,saved);return saved;
	}

	bool VerifyCheckpointPayloadChecksum(const std::filesystem::path& path,
		const std::uint64_t payloadBytes,const std::uint64_t expected,std::string& error)
	{
		std::ifstream input(path,std::ios::binary);
		if(!input){error="cannot open run checkpoint for checksum";return false;}
		input.seekg(48,std::ios::beg);
		if(!input){error="cannot seek run checkpoint payload";return false;}
		std::array<unsigned char,65536u> buffer={{0u}};
		std::uint64_t remaining=payloadBytes;
		std::uint64_t checksum=1469598103934665603ull;
		while(remaining>0u){
			const std::size_t chunk=static_cast<std::size_t>(std::min<std::uint64_t>(
				remaining,buffer.size()));
			input.read(reinterpret_cast<char*>(buffer.data()),static_cast<std::streamsize>(chunk));
			if(!input){error="run checkpoint payload is incomplete";return false;}
			for(std::size_t i=0;i<chunk;++i){checksum^=buffer[i];checksum*=1099511628211ull;}
			remaining-=chunk;
		}
		if(checksum!=expected){error="run checkpoint checksum mismatch";return false;}
		return true;
	}

	// Loads and validates a v5..v13 checkpoint: checks exact size, FNV-1a checksum,
	// timeline/precision/authority invariants, manifold access, and optional v2 Merkle sidecar.
	bool LoadMethaneRunCheckpoint(const std::filesystem::path& path,
		MethaneRunCheckpoint& checkpoint,std::string& error)
	{
		std::error_code sizeError;
		const std::uintmax_t exactFileBytes=std::filesystem::file_size(path,sizeError);
		CheckpointReader reader(path);if(!reader.Good()){error="cannot open run checkpoint";return false;}
		char magic[16]={};std::uint64_t version=0u,endian=0u,payloadBytes=0u,checksum=0u;
		const char expected[16]={'R','I','S','E','F','I','R','E','C','H','K','P','T','1',0,0};
		if(!reader.HeaderBytes(magic,sizeof(magic))||std::memcmp(magic,expected,sizeof(magic))!=0||
			!reader.HeaderBytes(&version,sizeof(version))||
				(version!=5u&&version!=6u&&version!=7u&&version!=8u&&version!=9u&&
					version!=10u&&version!=11u&&version!=12u&&version!=13u)||
			!reader.HeaderBytes(&endian,sizeof(endian))||endian!=0x0102030405060708ull||
			!reader.HeaderBytes(&payloadBytes,sizeof(payloadBytes))||
			!reader.HeaderBytes(&checksum,sizeof(checksum))||payloadBytes>64ull*1024ull*1024ull*1024ull||
			sizeError||exactFileBytes!=48ull+payloadBytes){
			error="run checkpoint header is invalid";return false;}
		if(!VerifyCheckpointPayloadChecksum(path,payloadBytes,checksum,error))return false;
		reader.SetLimit(payloadBytes);MethaneRunCheckpoint decoded;
		if(!ReadCheckpointPayload(reader,decoded,version)||!reader.Finished(checksum)){
			error="run checkpoint payload is incomplete or corrupt";return false;}
		FireStateProducerPrecision precision=FireStateProducerPrecision::Unknown;
		if(!HomogeneousStateProducerPrecision(decoded.states,precision)){
			error="run checkpoint producer precision is invalid";return false;}
		const bool productionState=precision==FireStateProducerPrecision::Binary32;
		if(version<=8u&&precision==FireStateProducerPrecision::Binary64&&
			!CanonicalizeLegacyBinary64CheckpointTemperature(decoded)){
			error="legacy binary64 checkpoint temperature cannot be canonicalized";return false;}
		if(decoded.acceptedSteps==0u){
			error="zero-step checkpoint state is not resumable";return false;}
		if(!AcceptedCheckpointTimelineValid(decoded)){
			error="run checkpoint accepted timeline is invalid";return false;}
		if(precision==FireStateProducerPrecision::Binary64&&
			!Binary64CheckpointStateAdmissible(decoded)){
			error="binary64 checkpoint state is not admissible in its producer class";return false;}
		if(version<12u&&productionState&&(decoded.acceptedSteps>0u||
			decoded.simulationTimeS!=0.0||decoded.previousStepS!=0.0||
			decoded.lastAcceptedStepS!=0.0||
			!decoded.values.acceptedTimeStepHistoryS.empty())){
			error="legacy production checkpoint lacks accepted manifold authority";return false;}
		if(version>=9u&&version<13u&&!productionState){
			error="modern binary64 checkpoint lacks producer authority";return false;}
		if(version>=13u&&!productionState&&
			(!decoded.binary64CheckpointAuthority.Available()||
				decoded.binary64CheckpointAuthority.Digest()!=
					Binary64CheckpointPayloadDigest(decoded))){
			error="binary64 checkpoint producer authority is invalid";return false;}
		if(version>=12u&&productionState){std::uint64_t acceptedStateDigest=0u;
			FireProductionProjectionShape acceptedShape;std::vector<float> acceptedConservative;
			std::array<std::vector<float>,3> acceptedMomentum,acceptedVelocity;
			if(productionState&&decoded.acceptedSteps>0u&&
				!BuildCheckpointAcceptedStatePayload(decoded,acceptedShape,
				acceptedConservative,acceptedMomentum,acceptedVelocity,acceptedStateDigest)){
				error="production checkpoint accepted state is not canonical binary32";return false;}
			FireProductionAcceptedCheckpointStateView stateView;stateView.shape=acceptedShape;
			stateView.conservativeValues=&acceptedConservative;stateView.momentum=&acceptedMomentum;
			stateView.velocity=&acceptedVelocity;
			FireProductionAcceptedCheckpointLifecycleView lifecycle;
			lifecycle.simulationTimeS=decoded.simulationTimeS;
			lifecycle.previousStepS=decoded.previousStepS;
			lifecycle.lastAcceptedStepS=decoded.lastAcceptedStepS;
			lifecycle.acceptedSteps=decoded.acceptedSteps;
			lifecycle.productionState=productionState;
			lifecycle.acceptedTimeStepHistoryS=&decoded.values.acceptedTimeStepHistoryS;
			if(!FireProductionCheckpointManifoldAccess::RestoreValidatedCheckpointFile(
				path.string(),payloadBytes,checksum,version,stateView,lifecycle,
				decoded.productionManifoldObservation,&error))return false;
		}
		if(!ExistingPayloadCertificateValid(path,decoded.caseRecordId,error))return false;
		checkpoint=std::move(decoded);
		return true;
	}

	std::string DigestFile( const std::filesystem::path& path )
	{
		std::ifstream input(path,std::ios::binary);
		if(!input)return std::string();
		input.seekg(0,std::ios::end);
		const std::streampos end = input.tellg();
		if(end<0)return std::string();
		input.seekg(0,std::ios::beg);
		RISECBOR64::Bytes bytes(static_cast<std::size_t>(end));
		if( end > 0 ) input.read(reinterpret_cast<char*>(bytes.data()),end);
		return RISECBOR64::SHA256Hex(bytes);
	}

	RISECBOR64::Bytes ReadFileBytes( const std::filesystem::path& path )
	{
		std::ifstream input(path,std::ios::binary);
		if(!input) return RISECBOR64::Bytes();
		input.seekg(0,std::ios::end);
		const std::streampos end=input.tellg();
		if(end<0) return RISECBOR64::Bytes();
		input.seekg(0,std::ios::beg);
		RISECBOR64::Bytes bytes(static_cast<std::size_t>(end));
		if(end>0) input.read(reinterpret_cast<char*>(bytes.data()),end);
		if(!input&&end>0) return RISECBOR64::Bytes();
		return bytes;
	}
	}
}
