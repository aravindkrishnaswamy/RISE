//////////////////////////////////////////////////////////////////////
//
//  FireRunDriver.cpp - Production fire run driver
//
//  Moved verbatim from tests/FireSequenceTest.cpp (simulation M2, track B),
//  with the production request builder and result application from
//  tests/FireProductionDyadicCalibrationFixture.h.  Test-only flags read
//  FireRunTestHooks (null in production).  Compiled without fast-math or
//  FP contraction (see the per-file rules in build/make/rise/Makefile).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FireRunDriver.h"
#include "FireProductionAcceptedFuelBudget.h"
#include "FireProductionAcceptedStageBudget.h"
#include "FireProductionOnsetEvidence.h"
#include "FireProductionOwnerConvergenceProbe.h"
#include "FireProductionStaggeredColumnBudget.h"
#include "../../Utilities/FireSequence.h"
#include "../../Utilities/FireOptics.h"
#include "../../Utilities/FireFastMode.h"
#include "../../Utilities/FireProductionAdvection.h"
#include "../../Utilities/FireProductionTransport.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <system_error>
#include <thread>

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	namespace FireRun
	{
	namespace
	{
		// Invariant checks carried over verbatim from the test translation unit.
		void Check(const bool condition,const char* message)
		{
			ReportFireRunCheck(condition,message);
		}
	}

	namespace ProductionStep
	{
	// Moved from tests/FireProductionDyadicCalibrationFixture.h; the fixture now uses these.
	static constexpr double Gravity=9.80665;
	bool AmbientAndInjected(MethaneCellState& ambient,MethaneCellState& injected,
		std::string& error)
	{
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		auto close=[&](MethaneCellState& state,const std::vector<double>& mass,
			const bool tagged){
			state=MethaneCellState();state.temperatureK=300.0;double inverseWeight=0.0;
			for(std::size_t species=0u;species<MethaneSpeciesCount;++species){
				state.constituent[species]=mass[species];
				if(species<MethaneCarbon){const FireThermochemistrySpecies* record=
					fuel.FindSpecies(fuel.SpeciesOrder()[species].c_str());
					if(record)inverseWeight+=mass[species]/record->molecularWeightKGPerKMol;}
			}
			if(!(inverseWeight>0.0))return false;
			const double density=fuel.ThermodynamicPressurePa()/(8314.46261815324*
				state.temperatureK*inverseWeight);
			for(double& value:state.constituent)value*=density;
			state.rhoTotalZ=tagged?state.TotalDensity():0.0;
			return fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(state),
				state.temperatureK,state.sensibleEnergyJPerM3,&error);
		};
		return close(ambient,fuel.AmbientMassFractions(),false)&&
			close(injected,fuel.InjectedMassFractions(),true);
	}

	bool ResidentStepRequestsExactlyEqual(
		const RISE::FireProductionResidentStepRequest& a,
		const RISE::FireProductionResidentStepRequest& b)
	{
		auto sameShape=[](const RISE::FireProductionProjectionShape& x,
			const RISE::FireProductionProjectionShape& y){return x.nx==y.nx&&x.ny==y.ny&&
			x.nz==y.nz&&x.cellWidthM==y.cellWidthM;};
		return sameShape(a.force.shape,b.force.shape)&&
			a.force.timeStepS==b.force.timeStepS&&
			a.force.ambientDensityKGPerM3==b.force.ambientDensityKGPerM3&&
			a.force.vremanCoefficient==b.force.vremanCoefficient&&
			a.force.gravityMPerS2==b.force.gravityMPerS2&&a.force.boundary==b.force.boundary&&
			a.force.cellGasDensityKGPerM3==b.force.cellGasDensityKGPerM3&&
			a.force.molecularKinematicViscosityM2PerS==
				b.force.molecularKinematicViscosityM2PerS&&
			a.force.faceDensityKGPerM3==b.force.faceDensityKGPerM3&&
			a.force.beginningMomentumKGPerM2S==b.force.beginningMomentumKGPerM2S&&
			sameShape(a.cellTransport.shape,b.cellTransport.shape)&&
			a.cellTransport.componentCount==b.cellTransport.componentCount&&
			a.cellTransport.timeStepS==b.cellTransport.timeStepS&&
			a.cellTransport.boundary==b.cellTransport.boundary&&
			a.cellTransport.conservativeValues==b.cellTransport.conservativeValues&&
			a.cellTransport.frozenVelocityMPerS==b.cellTransport.frozenVelocityMPerS&&
			a.cellTransport.ambientValues==b.cellTransport.ambientValues&&
			sameShape(a.dualTransport.shape,b.dualTransport.shape)&&
			a.dualTransport.timeStepS==b.dualTransport.timeStepS&&
			a.dualTransport.ambientDensityKGPerM3==b.dualTransport.ambientDensityKGPerM3&&
			a.dualTransport.boundary==b.dualTransport.boundary&&
			a.dualTransport.beginningFaceDensity==b.dualTransport.beginningFaceDensity&&
			a.dualTransport.beginningMomentum==b.dualTransport.beginningMomentum&&
			a.dualTransport.frozenVelocityMPerS==b.dualTransport.frozenVelocityMPerS&&
			a.cellSourceIncrement==b.cellSourceIncrement&&
			a.momentumSourceIncrement==b.momentumSourceIncrement&&
			a.divergenceTargetPerS==b.divergenceTargetPerS&&
			a.restorationDivergenceTargetPerS==b.restorationDivergenceTargetPerS&&
			a.beginningManifoldDeviationPerCell==b.beginningManifoldDeviationPerCell&&
			a.physicalOpenProjectionVCycleCount==b.physicalOpenProjectionVCycleCount&&
			a.monitorManifoldDiagnostics==b.monitorManifoldDiagnostics&&
			a.enforceManifoldPlateau==b.enforceManifoldPlateau&&
			a.restoreManifoldOutliers==b.restoreManifoldOutliers;
	}

	bool BuildProductionRequest(const MethaneRunCheckpoint& state,
		const std::vector<double>& divergenceTarget,const double timeStepS,
		RISE::FireProductionResidentStepRequest& request,std::string& error,
		const unsigned int workerCount,
		const std::vector<CellTransportEvaluation>* reusableTransport,
		const std::vector<unsigned char>* invalidReusableTransport)
	{
		request=RISE::FireProductionResidentStepRequest();
		const std::size_t cells=state.states.size();
		if(cells!=state.dimensions[0]*state.dimensions[1]*state.dimensions[2]||
			divergenceTarget.size()!=cells||
			(reusableTransport&&reusableTransport->size()!=cells)||
			(invalidReusableTransport&&(!reusableTransport||
				invalidReusableTransport->size()!=cells)))return false;
		MethaneCellState ambient,injected;if(!AmbientAndInjected(ambient,injected,error))return false;
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		const FireSimulationTransportRecord transport=FireSimulationTransportRecord::OpenV1();
		request.force.shape.nx=state.dimensions[0];request.force.shape.ny=state.dimensions[1];
		request.force.shape.nz=state.dimensions[2];
		request.force.shape.cellWidthM=static_cast<float>(state.cellWidthM);
		request.force.timeStepS=static_cast<float>(timeStepS);
		request.force.ambientDensityKGPerM3=static_cast<float>(ambient.GasDensity());
		request.force.vremanCoefficient=0.07f;
		request.force.gravityMPerS2={{0.0f,0.0f,-static_cast<float>(Gravity)}};
		request.force.boundary.fill(RISE::FireProductionProjectionPressureOpen);
		request.force.cellGasDensityKGPerM3.resize(cells);
		request.force.molecularKinematicViscosityM2PerS.resize(cells);
		request.cellTransport.shape=request.force.shape;request.cellTransport.componentCount=9u;
		request.cellTransport.timeStepS=request.force.timeStepS;
		request.cellTransport.boundary=request.force.boundary;
		request.cellTransport.conservativeValues.resize(9u*cells);
		const ConservativeVector ambientVector=ToConservativeVector(ambient);
		request.cellTransport.ambientValues.resize(9u);
		for(std::size_t component=0u;component<9u;++component)
			request.cellTransport.ambientValues[component]=static_cast<float>(ambientVector[component]);
		request.divergenceTargetPerS.resize(cells);
		request.restorationDivergenceTargetPerS.resize(cells);
		request.beginningManifoldDeviationPerCell.resize(cells);
		if(cells>static_cast<std::size_t>(std::numeric_limits<unsigned int>::max())){
			error="production request cell count exceeds the worker partition range";return false;
		}
		const unsigned int workers=std::max(1u,std::min(workerCount,
			static_cast<unsigned int>(cells)));
		const std::size_t noFailure=std::numeric_limits<std::size_t>::max();
		std::vector<std::size_t> failureCell(workers,noFailure);
		std::vector<std::string> failureMessage(workers);
		auto buildCellRange=[&](const unsigned int worker){
			const std::size_t first=cells*worker/workers,last=cells*(worker+1u)/workers;
			for(std::size_t cell=first;cell<last;++cell){
			std::string cellError;
			const ConservativeVector conservative=ToConservativeVector(state.states[cell]);
			const double gas=state.states[cell].GasDensity();CellMolecularTransportEvaluation molecular;
			if(!(gas>0.0)){
				failureCell[worker]=cell;
				failureMessage[worker]="production request gas density is nonpositive";break;
			}
			const bool reusable=reusableTransport&&(!invalidReusableTransport||
				(*invalidReusableTransport)[cell]==0u);
			if(reusable){
				molecular.molecularViscosityPaS=(*reusableTransport)[cell].molecularViscosityPaS;
			}else if(!EvaluateCellMolecularTransport(state.states[cell],fuel,transport,
				state.states[cell].producerPrecision,molecular,&cellError)){
				failureCell[worker]=cell;failureMessage[worker]=cellError;break;}
			request.force.molecularKinematicViscosityM2PerS[cell]=
				static_cast<float>(molecular.molecularViscosityPaS/gas);
			for(std::size_t component=0u;component<9u;++component)
				request.cellTransport.conservativeValues[component*cells+cell]=
					static_cast<float>(conservative[component]);
			float packedGas=request.cellTransport.conservativeValues[cells+cell];
			for(std::size_t component=2u;component<=6u;++component)
				packedGas+=request.cellTransport.conservativeValues[component*cells+cell];
			request.force.cellGasDensityKGPerM3[cell]=packedGas;
			double volumeRatio=0.0;
			if(!AcceptedConservativeVolumeRatio(conservative,fuel,
				state.states[cell].producerPrecision,volumeRatio,&cellError)){
				failureCell[worker]=cell;failureMessage[worker]=cellError;break;}
			request.divergenceTargetPerS[cell]=static_cast<float>(divergenceTarget[cell]);
			request.beginningManifoldDeviationPerCell[cell]=volumeRatio-1.0;
			request.restorationDivergenceTargetPerS[cell]=static_cast<float>(
				(volumeRatio-1.0)/static_cast<double>(request.force.timeStepS));
			}
		};
		if(workers==1u)buildCellRange(0u);
		else FireWorkerPool().Run(workers,buildCellRange);
		std::size_t firstFailure=noFailure;unsigned int failedWorker=0u;
		for(unsigned int worker=0u;worker<workers;++worker)if(failureCell[worker]<firstFailure){
			firstFailure=failureCell[worker];failedWorker=worker;}
		if(firstFailure!=noFailure){error=failureMessage[failedWorker];return false;}
		request.dualTransport.shape=request.force.shape;
		request.dualTransport.timeStepS=request.force.timeStepS;
		request.dualTransport.ambientDensityKGPerM3=request.force.ambientDensityKGPerM3;
		request.dualTransport.boundary=request.force.boundary;
		auto cellIndex=[&](const std::size_t x,const std::size_t y,const std::size_t z){
			return (z*state.dimensions[1]+y)*state.dimensions[0]+x;};
		for(unsigned int axis=0u;axis<3u;++axis){
			const std::size_t nx=state.dimensions[0],ny=state.dimensions[1],nz=state.dimensions[2];
			const std::size_t ex=axis==0u?nx+1u:nx,ey=axis==1u?ny+1u:ny,
				ez=axis==2u?nz+1u:nz,faces=ex*ey*ez;
			if(state.momentum.component[axis].size()!=faces||
				state.velocity.component[axis].size()!=faces)return false;
			request.force.faceDensityKGPerM3[axis].resize(faces);
			request.force.beginningMomentumKGPerM2S[axis].resize(faces);
			request.cellTransport.frozenVelocityMPerS[axis].resize(faces);
			ParallelFireSlices(ez,workerCount,[&](const std::size_t z){for(std::size_t y=0u;y<ey;++y)
				for(std::size_t x=0u;x<ex;++x){
					const std::size_t face=(z*ey+y)*ex+x;
					const std::size_t coordinate=axis==0u?x:(axis==1u?y:z);
					const std::size_t extent=axis==0u?nx:(axis==1u?ny:nz);
					std::array<std::size_t,3> low={{x,y,z}},high=low;
					if(coordinate>0u&&coordinate<extent)low[axis]=coordinate-1u;
					else {low[axis]=coordinate==0u?0u:extent-1u;high[axis]=low[axis];}
					const float lowDensity=request.force.cellGasDensityKGPerM3[
						cellIndex(low[0],low[1],low[2])];
					float density=0.0f;
					if(coordinate==0u||coordinate==extent)density=0.5f*lowDensity+
						0.5f*request.force.ambientDensityKGPerM3;
					else {high[axis]=coordinate;density=0.5f*lowDensity+0.5f*
						request.force.cellGasDensityKGPerM3[cellIndex(high[0],high[1],high[2])];}
					request.force.faceDensityKGPerM3[axis][face]=density;
					request.force.beginningMomentumKGPerM2S[axis][face]=
						static_cast<float>(state.momentum.component[axis][face]);
					request.cellTransport.frozenVelocityMPerS[axis][face]=
						static_cast<float>(state.velocity.component[axis][face]);
				}});
			request.dualTransport.beginningFaceDensity[axis]=request.force.faceDensityKGPerM3[axis];
			request.dualTransport.beginningMomentum[axis]=request.force.beginningMomentumKGPerM2S[axis];
			request.dualTransport.frozenVelocityMPerS[axis]=request.cellTransport.frozenVelocityMPerS[axis];
			request.momentumSourceIncrement[axis].assign(faces,0.0f);
		}
		request.cellSourceIncrement.assign(9u*cells,0.0f);
		return RISE::ValidateFireProductionFrozenForceRequest(request.force,&error)&&
			RISE::ValidateFireProductionCellPalindromeRequest(request.cellTransport,&error)&&
			RISE::ValidateFireProductionDualMomentumRequest(request.dualTransport,&error);
	}

	bool ApplyProductionResultUnchecked(const RISE::FireProductionResidentStepResult& production,
		MethaneRunCheckpoint& state,std::string& error,
		const bool enforceOracleEOSValidityDetector,
		const unsigned int temperatureWorkerCount)
	{
		const std::size_t cells=state.states.size();if(production.conservativeValues.size()!=9u*cells)
			return false;
		if(production.conservativeProducerPrecision!=FireStateProducerPrecision::Binary32)
			return Fail(&error,"production resident state lacks binary32 producer metadata");
		std::vector<ConservativeVector> conservative(cells);
		for(std::size_t cell=0u;cell<cells;++cell){
			for(std::size_t component=0u;component<9u;++component)
				conservative[cell][component]=production.conservativeValues[component*cells+cell];
			state.states[cell]=FromConservativeVector(conservative[cell],
				production.conservativeProducerPrecision);
		}
		std::vector<double> temperature;
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		if(enforceOracleEOSValidityDetector){
			if(!InvertPeriodicTemperaturesWithinBounds(conservative,fuel,fuel.TemperatureMinK(),
				fuel.TemperatureMaxK(),production.conservativeProducerPrecision,temperature,&error,
				temperatureWorkerCount)||
				temperature.size()!=cells)return false;
		}else{
			if(!InvertPeriodicTemperaturesWithinBounds(conservative,fuel,fuel.TemperatureMinK(),
				fuel.TemperatureMaxK(),production.conservativeProducerPrecision,temperature,&error,
				temperatureWorkerCount,false)||temperature.size()!=cells)return false;
		}
		for(std::size_t cell=0u;cell<cells;++cell)state.states[cell].temperatureK=temperature[cell];
		for(unsigned int axis=0u;axis<3u;++axis){
			state.momentum.component[axis].assign(production.projection.momentumKGPerM2S[axis].begin(),
				production.projection.momentumKGPerM2S[axis].end());
			state.velocity.component[axis].assign(production.projection.velocityMPerS[axis].begin(),
				production.projection.velocityMPerS[axis].end());
		}
		return true;
	}

	bool ApplyAcceptedProductionResult(const RISE::FireProductionResidentStepResult& production,
		const RISE::FireProductionAcceptedManifoldObservation& acceptedObservation,
		MethaneRunCheckpoint& state,std::string& error,
		const unsigned int temperatureWorkerCount)
	{
		if(!acceptedObservation.MatchesAcceptedResidentPayload(production))
			return Fail(&error,"accepted manifold observation no longer matches resident payload");
		// The 1e-3 pressure-deviation check in the binary64 core is an oracle
		// validity detector, not a production thermochemistry-domain consumer.
		// Accepted production publication retains the r60 conservative envelope
		// and table-bounded temperature inversion without inheriting that detector.
		return ApplyProductionResultUnchecked(production,state,error,false,
			temperatureWorkerCount);
	}
	}

	bool DiscontinuousThreadIdentityAccepted(const bool identical,
		const bool activeSetChecked,std::string& error)
	{
		if(identical)return true;
		error=activeSetChecked?"active_set_thread_identity_mismatch":
			"limiter_thread_identity_mismatch";
		return false;
	}

	bool AppendOracleMomentumBudget(const std::filesystem::path& path,
		const PeriodicMACShape& shape,const PeriodicMACField& beginningMomentum,
		const ConservativeAdvance3DConfig& config,const ConservativeAdvance3DResult& advanced,
		const double beginningTimeS,std::string& error,
		const std::size_t columnX,const std::size_t columnY)
	{
		if(path.empty()||config.periodicBoundaries||!(config.transport.deltaTimeS>0.0)||
			shape.nx<=columnX||shape.ny<=columnY||
			advanced.r0.openFlux.low[0].empty()||advanced.r1.openFlux.low[0].empty()||
			advanced.r0.openProjection.velocityMPerS.component[0].empty()||
			advanced.r1.openProjection.velocityMPerS.component[0].empty()||
			advanced.r0.openNonpressureMomentumRHS.component[2].size()!=
				OpenMACFaceCount3D(shape,2u)||
			advanced.r1.openNonpressureMomentumRHS.component[2].size()!=
				OpenMACFaceCount3D(shape,2u)||
			advanced.r0.openBuoyancyMomentumRHS.component[2].size()!=
				OpenMACFaceCount3D(shape,2u)||
			advanced.r1.openBuoyancyMomentumRHS.component[2].size()!=
				OpenMACFaceCount3D(shape,2u)||
			advanced.r0.openStressMomentumRHS.component[2].size()!=
				OpenMACFaceCount3D(shape,2u)||
			advanced.r1.openStressMomentumRHS.component[2].size()!=
				OpenMACFaceCount3D(shape,2u)||
			advanced.r0.openPhaseSourceMomentumRHS.component[2].size()!=
				OpenMACFaceCount3D(shape,2u)||
			advanced.r1.openPhaseSourceMomentumRHS.component[2].size()!=
				OpenMACFaceCount3D(shape,2u)||
			advanced.faceAlpha[2].size()!=OpenMACFaceCount3D(shape,2u)){
			error="oracle momentum diagnostic operands are unavailable";return false;
		}
		const OpenMACField3D advection0=OpenCompatibleMomentumFluxDivergence3D(shape,
			advanced.r0.openFlux,advanced.faceAlpha,
			advanced.r0.openProjection.velocityMPerS,&config.openBoundary);
		const OpenMACField3D advection1=OpenCompatibleMomentumFluxDivergence3D(shape,
			advanced.r1.openFlux,advanced.faceAlpha,
			advanced.r1.openProjection.velocityMPerS,&config.openBoundary);
		std::ofstream column(path.string()+".column.csv",std::ios::app);
		const bool writeHeader=column.tellp()==std::streampos(0);
		if(writeHeader)column<<"beginning_time_s,dt_s,x,y,z_face,beginning_momentum,"
			"stress_rate,buoyancy_rate,advection_rate,source_rate,pressure_gradient_rate,"
			"total_rate,closure_residual,nonpressure_split_residual,"
			"accepted_scalar_face_alpha\n";
		if(!column){error="oracle momentum column diagnostic cannot be opened";return false;}
		double maximumStress=0.0,maximumBuoyancy=0.0,maximumAdvection=0.0,
			maximumSource=0.0,maximumPressure=0.0,maximumTotal=0.0,maximumClosure=0.0,
			maximumNonpressureSplitResidual=0.0;
		double minimumAcceptedAlpha=1.0,maximumAcceptedAlpha=0.0;
		std::size_t maximumFace=OpenMACFaceIndex3D(shape,2u,columnX,columnY,0u);
		for(std::size_t z=0u;z<=shape.nz;++z){
			const std::size_t face=OpenMACFaceIndex3D(shape,2u,columnX,columnY,z);
			const double stress=0.5*(advanced.r0.openStressMomentumRHS.component[2][face]+
				advanced.r1.openStressMomentumRHS.component[2][face]);
			const double buoyancy=0.5*(advanced.r0.openBuoyancyMomentumRHS.component[2][face]+
				advanced.r1.openBuoyancyMomentumRHS.component[2][face]);
			const double source=0.5*(advanced.r0.openPhaseSourceMomentumRHS.component[2][face]+
				advanced.r1.openPhaseSourceMomentumRHS.component[2][face]);
			const double advection=-0.5*(advection0.component[2][face]+advection1.component[2][face]);
			const double nonpressure=0.5*(advanced.r0.openNonpressureMomentumRHS.component[2][face]+
				advanced.r1.openNonpressureMomentumRHS.component[2][face]);
			const double nonpressureSplitResidual=nonpressure-(stress+buoyancy+source);
			const double provisional=beginningMomentum.component[2][face]+
				config.transport.deltaTimeS*(nonpressure+advection);
			const double pressure=(advanced.momentumKGPerM2S.component[2][face]-provisional)/
				config.transport.deltaTimeS;
			const double total=stress+buoyancy+advection+source+pressure;
			const double closure=(advanced.momentumKGPerM2S.component[2][face]-
				beginningMomentum.component[2][face])/config.transport.deltaTimeS-total;
			const double acceptedAlpha=advanced.faceAlpha[2][face];
			if(!std::isfinite(acceptedAlpha)||acceptedAlpha<0.0||acceptedAlpha>1.0){
				error="oracle momentum diagnostic alpha is invalid";return false;}
			column<<std::setprecision(17)<<beginningTimeS<<','<<config.transport.deltaTimeS<<
				','<<columnX<<','<<columnY<<','<<z<<','<<beginningMomentum.component[2][face]<<','<<stress<<','<<
				buoyancy<<','<<advection<<','<<source<<','<<pressure<<','<<total<<','<<closure<<','<<
				nonpressureSplitResidual<<','<<acceptedAlpha<<'\n';
			minimumAcceptedAlpha=std::min(minimumAcceptedAlpha,acceptedAlpha);
			maximumAcceptedAlpha=std::max(maximumAcceptedAlpha,acceptedAlpha);
			auto update=[&](const double value,double& maximum){maximum=std::max(maximum,
				std::fabs(value));};
			update(stress,maximumStress);update(buoyancy,maximumBuoyancy);
			update(advection,maximumAdvection);update(source,maximumSource);
			update(pressure,maximumPressure);update(total,maximumTotal);update(closure,maximumClosure);
			update(nonpressureSplitResidual,maximumNonpressureSplitResidual);
			if(std::fabs(advanced.velocityMPerS.component[2][face])>
				std::fabs(advanced.velocityMPerS.component[2][maximumFace]))maximumFace=face;
		}
		if(!ClosePublishedStream(column)){
			error="oracle momentum column diagnostic close failed";return false;}
		std::ofstream summary(path,std::ios::app);
		const bool writeSummaryHeader=summary.tellp()==std::streampos(0);
		if(writeSummaryHeader)summary<<"beginning_time_s,dt_s,column_x,column_y,maximum_face,"
			"maximum_vertical_velocity_m_per_s,stress_rate_abs_max,buoyancy_rate_abs_max,"
			"advection_rate_abs_max,source_rate_abs_max,pressure_rate_abs_max,total_rate_abs_max,"
			"closure_residual_abs_max,nonpressure_split_residual_abs_max,"
			"accepted_scalar_face_alpha_min,"
			"accepted_scalar_face_alpha_max,accepted_scalar_face_alpha_at_velocity_max\n";
		if(!summary){error="oracle momentum summary diagnostic cannot be opened";return false;}
		summary<<std::setprecision(17)<<beginningTimeS<<','<<config.transport.deltaTimeS<<
			','<<columnX<<','<<columnY<<','<<maximumFace<<','<<advanced.velocityMPerS.component[2][maximumFace]<<','<<
			maximumStress<<','<<maximumBuoyancy<<','<<maximumAdvection<<','<<maximumSource<<','<<
			maximumPressure<<','<<maximumTotal<<','<<maximumClosure<<','<<
			maximumNonpressureSplitResidual<<','<<minimumAcceptedAlpha<<','<<
			maximumAcceptedAlpha<<','<<advanced.faceAlpha[2][maximumFace]<<'\n';
		if(!ClosePublishedStream(summary)){
			error="oracle momentum summary diagnostic close failed";return false;}
		return true;
	}

	bool RetainOracleMomentumOperands(const bool productionMetal,
		const bool composition,const bool legacyMomentumAudit)
	{
		return !productionMetal&&(composition||legacyMomentumAudit);
	}

	// r208: persistence is a projection of an authenticated publication, never
	// another source producer. There is deliberately no raw-dose/callback input.
	bool CarryCanonicalSourceForPersistence(
		const RISE::FireProductionFrozenSourcePacketSeal& source,
		std::vector<MethaneSourcePacket>& packets,RadiationEscapeFactor& escape,
		std::string& error)
	{
		if(!RISE::FireProductionFrozenSourcePacketSealMatches(source,&error))return false;
		const std::size_t cells=source.Shape().CellCount();
		packets.assign(cells,MethaneSourcePacket());
		for(std::size_t cell=0u;cell<cells;++cell){
			MethaneSourcePacket& packet=packets[cell];
			for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
				packet.constituentDelta[species]=source.SourceDelta()[(1u+species)*cells+cell];
			packet.sensibleEnergyDeltaJPerM3=source.SourceDelta()[8u*cells+cell];
			packet.reactedFuelKGPerM3=source.ReactedFuelKGPerM3()[cell];
			packet.oxidizedCarbonKGPerM3=source.OxidizedCarbonKGPerM3()[cell];
			packet.grossCarbonFormedKGPerM3=source.GrossCarbonFormedKGPerM3()[cell];
			packet.gasHeatReleaseWPerM3=source.GasHeatReleaseWPerM3()[cell];
			packet.sootHeatReleaseWPerM3=source.SootHeatReleaseWPerM3()[cell];
			packet.pilotEnergyDeltaJPerM3=source.PilotEnergyDeltaJPerM3()[cell];
			packet.pilotExpansionIntegral=source.PilotExpansionIntegral()[cell];
			packet.radiativeCoolingWPerM3=source.RadiativeCoolingWPerM3()[cell];
		}
		escape.beta=source.RadiationBeta();escape.gamma=source.RadiationGamma();
		escape.accepted=source.RadiationEscapeFactor();return true;
	}

	bool BindProductionBurnerBoundary(const OpenBoundaryConfig3D& boundary,
		RISE::FireProductionProjectedHeunMetalOwnerRequest& owner,std::string& error)
	{
		auto& flux=owner.lineage.eos.physicalFlux;
		const std::size_t count=flux.transport.shape.nx*flux.transport.shape.ny;
		if(count==0u||boundary.bottomFuelMask.size()!=count||
			boundary.bottomFuelMassFluxKGPerM2S.size()!=count||
			boundary.kind[4]!=AdiabaticWallBoundary3D){error="production burner boundary shape or bed is invalid";return false;}
		for(const auto kind:boundary.kind)if(kind!=PressureOpenBoundary3D&&kind!=AdiabaticWallBoundary3D){
			error="production burner boundary kind is unsupported";return false;}
		std::array<float,9> injected={};double gas=0.0;
		for(std::size_t component=0u;component<9u;++component){
			injected[component]=static_cast<float>(boundary.injectedState[component]);
			if(!std::isfinite(injected[component])||(component<8u&&injected[component]<0.0f)){
				error="production burner injected tuple is invalid";return false;}
			if(component>0u&&component<7u)gas+=injected[component];}
		if(!(gas>0.0)||injected[7]!=0.0f){error="production burner requires positive gas-only injection";return false;}
		std::vector<float> representedFlux(count);std::vector<std::uint8_t> mask(count);
		for(std::size_t face=0u;face<count;++face){
			representedFlux[face]=static_cast<float>(boundary.bottomFuelMassFluxKGPerM2S[face]);
			if(!std::isfinite(boundary.bottomFuelMassFluxKGPerM2S[face])||
				boundary.bottomFuelMassFluxKGPerM2S[face]<0.0||!std::isfinite(representedFlux[face])||
				(!boundary.bottomFuelMask[face]&&boundary.bottomFuelMassFluxKGPerM2S[face]!=0.0)){
				error="production burner prescribed flux is invalid";return false;}
			mask[face]=boundary.bottomFuelMask[face]?1u:0u;}
		// Publish only after validation; no source/state bytes are reconstructed.
		for(unsigned int side=0u;side<6u;++side)flux.transport.boundary[side]=
			boundary.kind[side]==AdiabaticWallBoundary3D?RISE::FireProductionProjectionWall:
				RISE::FireProductionProjectionPressureOpen;
		flux.transport.fuelInletBoundaryFace[4]=std::move(mask);
		flux.bottomFuelMassFluxKGPerM2S=std::move(representedFlux);
		flux.injectedConservative=injected;error.clear();return true;
	}


	double ProductionBurnerFuelDoseKG(const RISE::FireProductionProjectedHeunMetalOwnerRequest& owner)
	{
		const auto& flux=owner.lineage.eos.physicalFlux;
		const double width=flux.transport.shape.cellWidthM;
		const double step=owner.lineage.eos.candidateTimeStepS;
		double mass=0.0;
		for(const float representedFlux:flux.bottomFuelMassFluxKGPerM2S)
			mass+=static_cast<double>(representedFlux)*step*width*width;
		return mass;
	}

	bool ProductionCandidateTemperatureAccepted(const std::vector<MethaneCellState>& candidate,
		const double ceilingK,std::string& error)
	{
		if(std::any_of(candidate.begin(),candidate.end(),
			[ceilingK](const MethaneCellState& state){return !std::isfinite(state.temperatureK)||
				state.temperatureK>=ceilingK;})){
			error="accepted_physical_temperature_ceiling_violation";return false;}
		return true;
	}


	bool RefuseProductionCallerIngressBeforeCopy(
		const FireProductionResidentStepRequest& ordinary,
		const FireProductionFrozenSourcePacketSeal& source,
		ProductionCallerIngressProbe& observed,std::string& error)
	{
		const auto limit=observed.copyElementLimitBytes;
		const auto policy=observed.hostPolicy;
		observed=ProductionCallerIngressProbe();observed.copyElementLimitBytes=limit;
		observed.hostPolicy=policy;
		auto add=[](std::uint64_t value,std::uint64_t& total){
			if(total>UINT64_MAX-value)return false;total+=value;return true;};
		auto multiply=[](std::uint64_t a,std::uint64_t b,std::uint64_t& total){
			if(b!=0u&&a>UINT64_MAX/b)return false;total=a*b;return true;};
		try{
#if defined(__APPLE__)
			const bool sourceKnown=CaptureFireProductionPreconstructionResources(source,observed.source);
#else
			const bool sourceKnown=false;
#endif
			const auto& shape=ordinary.force.shape;
			const auto& sourceShape=source.Shape();
			bool layout=sourceKnown&&shape.nx==sourceShape.nx&&shape.ny==sourceShape.ny&&
				shape.nz==sourceShape.nz&&shape.cellWidthM==sourceShape.cellWidthM;
			const std::array<const std::vector<float>*,8> fields={{
				&ordinary.cellTransport.conservativeValues,&ordinary.cellSourceIncrement,
				&ordinary.cellTransport.frozenVelocityMPerS[0],&ordinary.cellTransport.frozenVelocityMPerS[1],
				&ordinary.cellTransport.frozenVelocityMPerS[2],&ordinary.force.beginningMomentumKGPerM2S[0],
				&ordinary.force.beginningMomentumKGPerM2S[1],&ordinary.force.beginningMomentumKGPerM2S[2]}};
			std::array<std::uint64_t,3> faces{};std::uint64_t allFaces=0u;
			const std::array<std::uint64_t,3> dims={{sourceShape.nx,sourceShape.ny,sourceShape.nz}};
			for(unsigned int axis=0u;axis<3u;++axis){
				std::uint64_t plane=0u;
				if(dims[axis]==UINT64_MAX||!multiply(dims[(axis+1u)%3u],dims[(axis+2u)%3u],plane)||
					!multiply(dims[axis]+1u,plane,faces[axis])||!add(faces[axis],allFaces))layout=false;
			}
			std::uint64_t nineCells=0u;
			if(!multiply(observed.source.cells,9u,nineCells))layout=false;
			std::uint64_t elements=0u,capacity=0u;
			for(std::size_t i=0u;i<fields.size();++i){
				observed.ordinarySize[i]=fields[i]->size();observed.ordinaryCapacity[i]=fields[i]->capacity();
				std::uint64_t bytes=0u,capacityBytes=0u;
				if(fields[i]->size()!=(i<2u?nineCells:faces[(i-2u)%3u])||
					fields[i]->capacity()<fields[i]->size()||
					!multiply(fields[i]->size(),sizeof(float),bytes)||!add(bytes,elements)||
					!multiply(fields[i]->capacity(),sizeof(float),capacityBytes)||!add(capacityBytes,capacity))layout=false;
			}
			// Request copies source seal, temperature, sourceDelta, conservative,
			// projected velocity and beginning momentum. Boundary/tables/envelope/
			// allocator/capacity/other families are deliberately NOT in this lower bound.
			std::uint64_t extraCells=0u,extraFaces=0u,lower=observed.source.sourceElementBytes;
			if(!multiply(observed.source.cells,76u,extraCells)||!multiply(allFaces,8u,extraFaces)||
				!add(extraCells,lower)||!add(extraFaces,lower))layout=false;
			observed.ordinaryLayoutKnown=layout;
			if(layout){observed.ordinaryElementBytes=elements;observed.ordinaryCapacityBytes=capacity;
				observed.requestCopyElementLowerBoundBytes=lower;}
			const FireRunHostMemory::Sample sample={observed.source.currentRSSBytes,
				observed.source.physicalFootprintBytes,observed.source.compressedBytes,observed.source.hostTelemetryKnown};
			observed.hostPolicyStop=policy.Stop(sample);
			observed.refusalSite=!layout?1u:(observed.hostPolicyStop?2u:
				(limit!=0u&&lower>limit?3u:4u));
			error=observed.refusalSite==1u?"caller ingress layout or telemetry unavailable":
				(observed.refusalSite==2u?"caller ingress host policy refused":
				(observed.refusalSite==3u?"caller ingress copy element budget refused":
				"caller ingress complete inventory unqualified"));
		}catch(const std::bad_alloc&){observed.refusalSite=5u;error.clear();}
		return false;
	}

	bool BuildProductionProjectedHeunOwnerRequest(
		const RISE::FireProductionResidentStepRequest& ordinary,
		const RISE::FireProductionFrozenSourcePacketSeal& source,
		const RISE::RISECBOR64::Bytes& caseRecordEnvelope,
		const float projectionTolerancePerS,const float endpointVelocityToleranceMPerS,
		RISE::FireProductionProjectedHeunMetalOwnerRequest& owner,std::string& error,
		const OpenBoundaryConfig3D* burnerBoundary,
		ProductionCallerIngressProbe* ingressProbe)
	{
		if(ingressProbe)return RefuseProductionCallerIngressBeforeCopy(ordinary,source,*ingressProbe,error);
		owner=RISE::FireProductionProjectedHeunMetalOwnerRequest();
		if(!source.IsSealed()){error="projected-Heun production source authority is unsealed";
			return false;}
		if(source.SourceDelta().size()!=ordinary.cellSourceIncrement.size()||
			std::memcmp(source.SourceDelta().data(),ordinary.cellSourceIncrement.data(),
				source.SourceDelta().size()*sizeof(float))!=0){
			std::size_t mismatch=0u;while(mismatch<ordinary.cellSourceIncrement.size()&&
				mismatch<source.SourceDelta().size()&&std::memcmp(&source.SourceDelta()[mismatch],
					&ordinary.cellSourceIncrement[mismatch],sizeof(float))==0)++mismatch;
			std::ostringstream message;message<<std::setprecision(17)
				<<"projected-Heun production source authority dose changed after construction: source_size="
				<<source.SourceDelta().size()<<" ordinary_size="<<ordinary.cellSourceIncrement.size()
				<<" index="<<mismatch;
			if(mismatch<source.SourceDelta().size()&&mismatch<ordinary.cellSourceIncrement.size())
				message<<" source="<<source.SourceDelta()[mismatch]<<" ordinary="
					<<ordinary.cellSourceIncrement[mismatch];
			error=message.str();return false;
		}
		if(source.TimeStepS()!=ordinary.force.timeStepS){std::ostringstream mismatch;
			mismatch<<std::setprecision(17)<<"projected-Heun production source timestep mismatch: source="
				<<source.TimeStepS()<<" ordinary="<<ordinary.force.timeStepS;
			error=mismatch.str();return false;}
		auto& eos=owner.lineage.eos;auto& flux=eos.physicalFlux;auto& transport=flux.transport;
		transport.shape=ordinary.force.shape;transport.boundary=ordinary.force.boundary;
		transport.stage=RISE::FireProductionProjectedHeunStage::R0;
		transport.attemptIdentity=source.AttemptIdentity();
		transport.parentCandidateIdentity=source.BeginningStateIdentity();
		transport.projectionIdentity=source.PacketIdentity();
		if(transport.parentCandidateIdentity==0u)transport.parentCandidateIdentity=UINT64_C(1);
		if(transport.projectionIdentity==0u)transport.projectionIdentity=UINT64_C(2);
		transport.conservativeValues=ordinary.cellTransport.conservativeValues;
		transport.temperatureK=source.BeginningTemperatureK();
		transport.projectedVelocityMPerS=ordinary.cellTransport.frozenVelocityMPerS;
		for(unsigned int side=0u;side<6u;++side){const std::size_t count=side<2u?
			transport.shape.ny*transport.shape.nz:(side<4u?transport.shape.nx*transport.shape.nz:
			transport.shape.nx*transport.shape.ny);transport.fuelInletBoundaryFace[side].assign(count,0u);
			flux.pressureOpenInflow[side].assign(count,0u);}
		if(ordinary.cellTransport.ambientValues.size()!=9u){error=
			"projected-Heun production ambient tuple is invalid";return false;}
		for(std::size_t component=0u;component<9u;++component)
			flux.ambient[component]=ordinary.cellTransport.ambientValues[component];
		flux.ambientTemperatureK=300.0f;
		const FireCertifiedNullspace& reconstruction=
			FireSimulationMethaneRecord::PhysicalV1().ConservativeReconstruction();
		flux.nullity=reconstruction.nullity;
		flux.nullspaceBasis.resize(reconstruction.orthonormalBasis.size());
		for(std::size_t index=0u;index<flux.nullspaceBasis.size();++index)
			flux.nullspaceBasis[index]=static_cast<float>(reconstruction.orthonormalBasis[index]);
		flux.coordinateProjector.assign(flux.nullity*flux.nullity,0.0f);
		for(std::size_t index=0u;index<flux.nullity;++index)
			flux.coordinateProjector[index*flux.nullity+index]=1.0f;
		eos.sourceDelta=source.SourceDelta();eos.producingStage=RISE::FireProductionScalarEOSStage::QStar;
		eos.producerPrecision=FireStateProducerPrecision::Binary32;
		eos.caseRecordEnvelope=caseRecordEnvelope;eos.candidateTimeStepS=ordinary.force.timeStepS;
		owner.lineage.frozenSource=source;
		owner.beginningMomentumKGPerM2S=ordinary.force.beginningMomentumKGPerM2S;
		owner.ambientDensityKGPerM3=ordinary.force.ambientDensityKGPerM3;
		owner.vremanCoefficient=ordinary.force.vremanCoefficient;
		owner.gravityMPerS2=ordinary.force.gravityMPerS2;
		owner.projectionTolerancePerS=projectionTolerancePerS;
		owner.endpointVelocityToleranceMPerS=endpointVelocityToleranceMPerS;
		owner.maximumPicardIterations=64u;
		return !burnerBoundary||BindProductionBurnerBoundary(*burnerBoundary,owner,error);
	}

	bool WriteProductionCrossingConvergence(const std::filesystem::path& auditPath,
		const RISE::FireProductionProjectedHeunMetalOwnerRequest& liveRequest,
		const RISE::FireProductionProjectedHeunMetalOwnerResult& accepted,
		const double beginningTimeS,const double representedStepS,const std::size_t acceptedStep,
		const std::size_t columnX,const std::size_t columnY,std::string& error,
		const bool syntheticFixture,const std::vector<double>& bottomSourcePattern)
	{
		using namespace FireProductionOwnerConvergenceProbe;
		if(!MatchesAcceptedEvent(liveRequest,beginningTimeS,representedStepS,error))return false;
		const std::string ledgerSHA=DigestFile(auditPath.string()+".source_ledger.v1");
		const std::string inputSHA=DigestFile(auditPath.string()+".source_observation_inputs.bin");
		const std::string eventSHA=DigestFile(auditPath.string()+".event.v1");
		if(!IsSHA256(ledgerSHA)||!IsSHA256(inputSHA)||!IsSHA256(eventSHA)){
			error="crossing convergence requires complete persisted source/event bindings";return false;}
		// Keep the originally accepted output untouched. This second owner has no
		// resident-step token/publication interface and cannot advance the solver.
		auto diagnosticRequest=liveRequest;
		diagnosticRequest.qualificationCaptureIterationTrace=true;
		diagnosticRequest.qualificationProductionStageTokens=true;
		RISE::FireProductionProjectedHeunMetalOwnerResult observed;
		if(!RISE::AttemptFireProductionProjectedHeunMetalOwner(diagnosticRequest,observed,&error)||
			!SameAcceptedOwner(accepted,observed,error))return false;
		const auto& shape=liveRequest.lineage.eos.physicalFlux.transport.shape;
		const Geometry geometry={shape.nx,shape.ny,shape.nz,columnX,columnY};
		Context context;context.caseSHA256=RISECBOR64::SHA256Hex(liveRequest.lineage.eos.caseRecordEnvelope);
		context.source=syntheticFixture?"r207_synthetic_fixture_no_physics_claim":
			"r207_accepted_crossing_observational_rerun";
		context.ownerIdentity=observed.ownerPublicationIdentity;
		context.acceptedIterations=observed.acceptedPicardIterations;
		const auto digest=[](const std::string& value){return RISECBOR64::SHA256Hex(
			RISECBOR64::Bytes(value.begin(),value.end()));};
		const std::array<const std::vector<RISE::FireProductionProjectedHeunIterationTrace>*,3> traces={{
			&observed.qualificationIterationTrace[0],&observed.qualificationIterationTrace[1],
			&observed.qualificationIterationTrace[2]}};
		std::string text,binding,csv;
		if(!SerializeV2(traces,geometry,context,digest,text,binding,error,&csv))return false;
		FireProductionAcceptedFuelBudget::Result fuelBudget;
		std::vector<unsigned char> footprint;
		for(const double value:bottomSourcePattern){
			if(!std::isfinite(value)||value<0.0){error="fuel budget source pattern invalid";return false;}
			footprint.push_back(value>0.0?1u:0u);
		}
		if(!FireProductionAcceptedFuelBudget::Reduce(shape,representedStepS,
			liveRequest.lineage.eos.physicalFlux.transport.conservativeValues,
			liveRequest.lineage.frozenSource.SourceDelta(),observed,fuelBudget,error,footprint))return false;
		RISECBOR64::Bytes tailBytes;
		std::ostringstream tailRecords;tailRecords<<std::setprecision(17);
		for(unsigned int stage=0u;stage<3u;++stage)
		for(std::size_t index=0u;index<traces[stage]->size();++index){const auto& trace=(*traces[stage])[index];
			std::size_t count=0u;double volume=0.0;
			if(!ConsumedTailDemand(trace.consumedTargetTailIncrementPerS,shape.CellCount(),
				trace.consumedProjectionTargetIdentity,trace.consumedTargetCorrectionIteration,
				representedStepS,shape.cellWidthM,count,volume,error))return false;
			const std::size_t offset=tailBytes.size();
			for(const float value:trace.consumedTargetTailIncrementPerS){std::uint32_t bits=0u;
				std::memcpy(&bits,&value,sizeof(bits));for(unsigned int byte=0u;byte<4u;++byte)
					tailBytes.push_back(static_cast<unsigned char>((bits>>(8u*byte))&255u));}
			tailRecords<<"OWNER_CONSUMED_TAIL_TARGET stage=R"<<stage<<" trace_index="<<index
				<<" raw_iteration_tag="<<trace.iteration
				<<" target_identity="<<trace.consumedProjectionTargetIdentity
				<<" correction_iteration="<<trace.consumedTargetCorrectionIteration
				<<" nonzero_cells="<<count<<" requested_increment_volume_m3="<<volume
				<<" payload_offset_bytes="<<offset<<" payload_cells="<<shape.CellCount()
				<<" semantics=last_consumed_target_increment_not_realized_drain\n";
		}
		std::ostringstream artifact;artifact<<std::setprecision(17)
			<<"OWNER_CONVERGENCE_EVENT version=2 accepted_step_beginning="<<acceptedStep
			<<" beginning_time_s="<<beginningTimeS<<" dt_s="<<representedStepS
			<<" cell_width_m="<<static_cast<double>(shape.cellWidthM)
			<<" end_time_s="<<beginningTimeS+representedStepS
			<<" input_payload_root_sha256="<<accepted.inputPayloadRootSHA256
			<<" output_payload_root_sha256="<<accepted.publicationPayloadRootSHA256
			<<" kernel_set_sha256="<<accepted.qualifiedKernelSetSHA256
			<<" source_packet_identity="<<liveRequest.lineage.frozenSource.PacketIdentity()
			<<" source_ledger_sha256="<<ledgerSHA
			<<" source_observation_inputs_sha256="<<inputSHA
			<<" event_sha256="<<eventSHA
			<<" terminal_fields_bit_identical=1 stage_identities_identical=1 solver_publications=0"
			<<" diagnostic_device_ms="<<observed.deviceElapsedMS
			<<" diagnostic_wall_ms="<<observed.wallElapsedMS
			<<" qualification_trace_staging_count="<<observed.qualificationTraceStagingCount
			<<" certificate_mode=qualification_observer"
			<<" actual_metal_bytes="<<observed.actualMetalAllocationBytes
			<<" certified_metal_bytes="<<observed.certifiedWorkingSetBytes
			<<" commuting_residual_kg_m-3="<<observed.maximumCommutingResidualKGPerM3
			<<" commuting_bound_kg_m-3="<<observed.commutingIdentityBoundKGPerM3
			<<" fixed_k_selection=not_authorized\n";
		if(!WriteV2(artifact,traces,geometry,context,digest,binding,error))return false;
		artifact<<"OWNER_CONVERGENCE_CSV sha256="<<digest(csv)<<"\n";
		artifact<<"ACCEPTED_R1_CH4_BUDGET scope=observed_step_not_window_integral"
			<<" boundary_transport_kg="<<fuelBudget.boundaryTransportKG
			<<" divergence_transport_kg="<<fuelBudget.divergenceTransportKG
			<<" state_change_kg="<<fuelBudget.stateChangeKG
			<<" frozen_source_kg="<<fuelBudget.frozenSourceKG
			<<" closure_residual_kg="<<fuelBudget.closureResidualKG<<'\n';
		for(unsigned int side=0u;side<6u;++side)artifact<<"ACCEPTED_R1_CH4_BOUNDARY side="<<side
			<<" low_outward_kg_per_s="<<fuelBudget.lowOutwardKGPerS[side]
			<<" correction_outward_kg_per_s="<<fuelBudget.correctionOutwardKGPerS[side]<<'\n';
		if(fuelBudget.bottomFootprintAvailable){
			artifact<<"ACCEPTED_R1_BOTTOM_SOURCE_MASK faces="<<fuelBudget.bottomFootprintFaces
				<<" sha256="<<RISECBOR64::SHA256Hex(footprint)<<" predicate=canonical_source_pattern_gt_zero\n";
			for(unsigned int region=0u;region<2u;++region)artifact<<"ACCEPTED_R1_BOTTOM_REGION source_footprint="<<region
				<<" low_outward_kg_per_s="<<fuelBudget.bottomFootprintLowKGPerS[region]
				<<" correction_outward_kg_per_s="<<fuelBudget.bottomFootprintCorrectionKGPerS[region]<<'\n';
		}
		artifact<<tailRecords.str()<<"OWNER_CONSUMED_TAIL_PAYLOAD encoding=binary32_little_endian sha256="
			<<RISECBOR64::SHA256Hex(tailBytes)<<" bytes="<<tailBytes.size()<<"\n"
			<<"OWNER_PROJECTION_BUDGET pressure_impulse=combined_tangent_source_tail_projection "
			<<"standalone_restoration_projection=absent separated_tail_impulse=unavailable "
			<<"legacy_tail_columns=terminal_remaining_demand_not_realized_drain\n";
		const std::filesystem::path transcriptPath=auditPath.string()+".convergence.v2";
		const std::filesystem::path csvPath=transcriptPath.string()+".csv";
		const std::filesystem::path tailPath=transcriptPath.string()+".tail_targets.bin";
		std::ofstream tailOutput(tailPath,std::ios::binary);
		tailOutput.write(reinterpret_cast<const char*>(tailBytes.data()),tailBytes.size());tailOutput.close();
		std::ofstream csvOutput(csvPath,std::ios::binary);csvOutput<<csv;csvOutput.close();
		std::ofstream output(transcriptPath,std::ios::binary);output<<artifact.str();output.close();
		if(!tailOutput||DigestFile(tailPath)!=RISECBOR64::SHA256Hex(tailBytes)||
			!output||!csvOutput||DigestFile(transcriptPath)!=digest(artifact.str())||
			DigestFile(csvPath)!=digest(csv)){error="crossing convergence artifact write failed";return false;}
		std::fprintf(stderr,"OWNER_CONVERGENCE_CROSSING path=%s sha256=%s csv_sha256=%s "
			"terminal_bit_identity=1 diagnostic_wall_ms=%.17g passed=1\n",
			transcriptPath.string().c_str(),DigestFile(transcriptPath).c_str(),digest(csv).c_str(),
			observed.wallElapsedMS);return true;
	}

	void AppendReplayUInt64(RISECBOR64::Bytes& bytes,const std::uint64_t value)
	{
		for(unsigned int byte=0u;byte<8u;++byte)
			bytes.push_back(static_cast<unsigned char>((value>>(8u*byte))&0xffu));
	}

	void AppendReplayDouble(RISECBOR64::Bytes& bytes,const double value)
	{
		std::uint64_t bits=0u;std::memcpy(&bits,&value,sizeof(bits));
		AppendReplayUInt64(bytes,bits);
	}
	void AppendReplayFloat(RISECBOR64::Bytes& bytes,const float value)
	{
		std::uint32_t bits=0u;std::memcpy(&bits,&value,sizeof(bits));
		for(unsigned int byte=0u;byte<4u;++byte)
			bytes.push_back(static_cast<unsigned char>((bits>>(8u*byte))&0xffu));
	}

	RISECBOR64::Bytes ProductionSourcePacketFieldBytes(
		const std::vector<MethaneSourcePacket>& packets)
	{
		RISECBOR64::Bytes bytes;
		const char schema[]="rise.fire.production.source_packet_field.v1";
		bytes.insert(bytes.end(),schema,schema+sizeof(schema));
		AppendReplayUInt64(bytes,packets.size());
		for(const MethaneSourcePacket& packet:packets){
			for(const double value:packet.constituentDelta)AppendReplayDouble(bytes,value);
			AppendReplayDouble(bytes,packet.sensibleEnergyDeltaJPerM3);
			AppendReplayDouble(bytes,packet.reactedFuelKGPerM3);
			AppendReplayDouble(bytes,packet.oxidizedCarbonKGPerM3);
			AppendReplayDouble(bytes,packet.grossCarbonFormedKGPerM3);
			AppendReplayDouble(bytes,packet.gasHeatReleaseWPerM3);
			AppendReplayDouble(bytes,packet.sootHeatReleaseWPerM3);
			AppendReplayDouble(bytes,packet.pilotEnergyDeltaJPerM3);
			AppendReplayDouble(bytes,packet.pilotExpansionIntegral);
			AppendReplayDouble(bytes,packet.radiativeCoolingWPerM3);
		}
		return bytes;
	}

	std::string ProductionSourcePacketFieldSHA256(
		const std::vector<MethaneSourcePacket>& packets)
	{
		return RISECBOR64::SHA256Hex(ProductionSourcePacketFieldBytes(packets));
	}

	bool WriteProductionSourcePacketFieldEvidence(const std::filesystem::path& base,
		const std::vector<MethaneSourcePacket>& packets,
		const std::vector<MethaneCellState>& unstagedBeginning,
		const std::vector<MethaneCellState>& stagedBeginning,
		const PeriodicMACField& unstagedMomentum,
		const FireProductionResidentStepRequest& request,std::string& error)
	{
		const RISECBOR64::Bytes bytes=ProductionSourcePacketFieldBytes(packets);
		const std::filesystem::path payloadPath=base.string()+".source_packets.bin";
		std::ofstream payload(payloadPath,std::ios::binary|std::ios::trunc);
		if(!bytes.empty())payload.write(reinterpret_cast<const char*>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
		payload.close();
		if(!payload){error="production onset source-packet field write failed";return false;}
		RISECBOR64::Bytes complete;
		const char completeSchema[]="rise.fire.production.source_observation_inputs.v1";
		complete.insert(complete.end(),completeSchema,completeSchema+sizeof(completeSchema));
		complete.insert(complete.end(),bytes.begin(),bytes.end());
		AppendReplayUInt64(complete,unstagedBeginning.size());
		if(stagedBeginning.size()!=unstagedBeginning.size()){
			error="production complete source input state shape mismatch";return false;}
		for(std::size_t cell=0u;cell<unstagedBeginning.size();++cell){
			const ConservativeVector unstaged=ToConservativeVector(unstagedBeginning[cell]);
			const ConservativeVector staged=ToConservativeVector(stagedBeginning[cell]);
			for(std::size_t component=0u;component<MethaneConservativeDimension;++component){
				AppendReplayDouble(complete,unstaged[component]);
				AppendReplayDouble(complete,staged[component]);
			}
		}
		for(unsigned int axis=0u;axis<3u;++axis){
			AppendReplayUInt64(complete,unstagedMomentum.component[axis].size());
			if(request.force.beginningMomentumKGPerM2S[axis].size()!=
				unstagedMomentum.component[axis].size()){
				error="production complete source input momentum shape mismatch";return false;}
			for(std::size_t face=0u;face<unstagedMomentum.component[axis].size();++face){
				AppendReplayDouble(complete,unstagedMomentum.component[axis][face]);
				AppendReplayFloat(complete,request.force.beginningMomentumKGPerM2S[axis][face]);
			}
		}
		for(unsigned int axis=0u;axis<3u;++axis){
			AppendReplayUInt64(complete,request.cellTransport.frozenVelocityMPerS[axis].size());
			for(const float value:request.cellTransport.frozenVelocityMPerS[axis])
				AppendReplayFloat(complete,value);
			AppendReplayUInt64(complete,request.dualTransport.frozenVelocityMPerS[axis].size());
			for(const float value:request.dualTransport.frozenVelocityMPerS[axis])
				AppendReplayFloat(complete,value);
		}
		AppendReplayUInt64(complete,request.cellSourceIncrement.size());
		for(const float value:request.cellSourceIncrement)AppendReplayFloat(complete,value);
		for(unsigned int axis=0u;axis<3u;++axis){
			AppendReplayUInt64(complete,request.momentumSourceIncrement[axis].size());
			for(const float value:request.momentumSourceIncrement[axis])
				AppendReplayFloat(complete,value);
		}
		const std::filesystem::path completePath=base.string()+".source_observation_inputs.bin";
		std::ofstream completeOutput(completePath,std::ios::binary|std::ios::trunc);
		if(!complete.empty())completeOutput.write(reinterpret_cast<const char*>(complete.data()),
			static_cast<std::streamsize>(complete.size()));
		completeOutput.close();
		if(!completeOutput){error="production complete source input write failed";return false;}
		std::array<long double,MethaneSpeciesCount> speciesTotal{};
		long double energy=0.0L,reacted=0.0L,oxidized=0.0L,grossCarbon=0.0L,
			gasHRR=0.0L,sootHRR=0.0L,pilotEnergy=0.0L,pilotExpansion=0.0L,
			radiativeCooling=0.0L;
		for(const MethaneSourcePacket& packet:packets){
			for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
				speciesTotal[species]+=packet.constituentDelta[species];
			energy+=packet.sensibleEnergyDeltaJPerM3;reacted+=packet.reactedFuelKGPerM3;
			oxidized+=packet.oxidizedCarbonKGPerM3;
			grossCarbon+=packet.grossCarbonFormedKGPerM3;
			gasHRR+=packet.gasHeatReleaseWPerM3;sootHRR+=packet.sootHeatReleaseWPerM3;
			pilotEnergy+=packet.pilotEnergyDeltaJPerM3;
			pilotExpansion+=packet.pilotExpansionIntegral;
			radiativeCooling+=packet.radiativeCoolingWPerM3;
		}
		std::ofstream ledger(base.string()+".source_ledger.v1",std::ios::trunc);
		const long double cellVolume=static_cast<long double>(request.force.shape.cellWidthM)*
			request.force.shape.cellWidthM*request.force.shape.cellWidthM;
		ledger<<std::setprecision(21)
			<<"schema rise.fire.production.source_packet_ledger.v1\n"
			<<"payload_sha256 "<<RISECBOR64::SHA256Hex(bytes)<<"\n"
			<<"source_observation_inputs_sha256 "<<RISECBOR64::SHA256Hex(complete)<<"\n"
			<<"packet_count "<<packets.size()<<"\n";
		for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
			ledger<<"constituent_delta_kg_"<<species<<' '<<speciesTotal[species]*cellVolume<<'\n';
		ledger<<"sensible_energy_delta_j "<<energy*cellVolume<<"\nreacted_fuel_kg "<<
			reacted*cellVolume<<"\noxidized_carbon_kg "<<oxidized*cellVolume<<
			"\ngross_carbon_kg "<<grossCarbon*cellVolume<<"\ngas_hrr_w "<<gasHRR*cellVolume<<
			"\nsoot_hrr_w "<<sootHRR*cellVolume<<"\npilot_energy_j "<<pilotEnergy*cellVolume<<
			"\npilot_expansion_m3 "<<pilotExpansion*cellVolume<<
			"\nradiative_cooling_w "<<radiativeCooling*cellVolume<<'\n';
		ledger.close();
		if(!ledger||DigestFile(payloadPath)!=RISECBOR64::SHA256Hex(bytes)||
			DigestFile(completePath)!=RISECBOR64::SHA256Hex(complete)){
			error="production onset source-packet evidence verification failed";return false;
		}
		return true;
	}

	bool WriteProductionSourceAndAdjacentStateContext(const std::filesystem::path& path,
		const FireProductionProjectionShape& shape,const std::size_t centerX,
		const std::size_t centerY,const std::size_t centerZ,
		const std::vector<MethaneCellState>& beginning,
		const std::vector<MethaneSourcePacket>& packets,
		const std::vector<float>& acceptedConservative,
		const FireSimulationMethaneRecord& fuel,std::string& error)
	{
		const std::size_t cells=shape.CellCount();
		if(beginning.size()!=cells||packets.size()!=cells||
			acceptedConservative.size()!=MethaneConservativeDimension*cells||
			centerX>=shape.nx||centerY>=shape.ny||centerZ>=shape.nz){
			error="production onset source/state context shape mismatch";return false;
		}
		std::vector<std::array<std::size_t,3>> locations;
		auto add=[&](const std::size_t x,const std::size_t y,const std::size_t z){
			const std::array<std::size_t,3> location={{x,y,z}};
			if(std::find(locations.begin(),locations.end(),location)==locations.end())
				locations.push_back(location);};
		add(centerX,centerY,centerZ);
		if(centerX>0u)add(centerX-1u,centerY,centerZ);
		if(centerX+1u<shape.nx)add(centerX+1u,centerY,centerZ);
		if(centerY>0u)add(centerX,centerY-1u,centerZ);
		if(centerY+1u<shape.ny)add(centerX,centerY+1u,centerZ);
		if(centerZ>0u)add(centerX,centerY,centerZ-1u);
		if(centerZ+1u<shape.nz)add(centerX,centerY,centerZ+1u);
		std::ofstream context(path,std::ios::trunc);
		context<<"relation,cell,x,y,z,beginning_temperature_k,beginning_eos_deviation,"
			"accepted_temperature_k,accepted_eos_deviation";
		for(std::size_t component=0u;component<MethaneConservativeDimension;++component)
			context<<",beginning_q"<<component;
		for(std::size_t component=0u;component<MethaneConservativeDimension;++component)
			context<<",accepted_q"<<component;
		for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
			context<<",source_species"<<species;
		context<<",source_sensible_energy,source_reacted_fuel,source_oxidized_carbon,"
			"source_gross_carbon,source_gas_hrr,source_soot_hrr,source_pilot_energy,"
			"source_pilot_expansion,source_radiative_cooling\n";
		for(std::size_t locationIndex=0u;locationIndex<locations.size();++locationIndex){
			const std::size_t x=locations[locationIndex][0],y=locations[locationIndex][1],
				z=locations[locationIndex][2];
			const std::size_t cell=x+shape.nx*(y+shape.ny*z);
			ConservativeVector acceptedVector{};
			for(std::size_t component=0u;component<MethaneConservativeDimension;++component)
				acceptedVector[component]=acceptedConservative[component*cells+cell];
			MethaneCellState accepted=FromConservativeVector(acceptedVector,
				FireStateProducerPrecision::Binary32);
			double acceptedTemperature=0.0;
			if(!InvertMethaneTemperatureWithinAcceptedEnvelope(accepted,
				fuel.TemperatureMinK(),fuel.TemperatureMaxK(),fuel,acceptedTemperature,&error))
				return false;
			accepted.temperatureK=acceptedTemperature;
			double beginningDeviation=0.0,acceptedDeviation=0.0;
			if(!EquationOfStateResidual(beginning[cell],fuel,beginningDeviation,&error)||
				!EquationOfStateResidual(accepted,fuel,acceptedDeviation,&error))return false;
			const ConservativeVector beginningVector=ToConservativeVector(beginning[cell]);
			const MethaneSourcePacket& packet=packets[cell];
			context<<std::setprecision(17)<<(locationIndex==0u?"center":"adjacent")<<','<<
				cell<<','<<x<<','<<y<<','<<z<<','<<beginning[cell].temperatureK<<','<<
				beginningDeviation<<','<<acceptedTemperature<<','<<acceptedDeviation;
			for(std::size_t component=0u;component<MethaneConservativeDimension;++component)
				context<<','<<beginningVector[component];
			for(std::size_t component=0u;component<MethaneConservativeDimension;++component)
				context<<','<<acceptedVector[component];
			for(const double value:packet.constituentDelta)context<<','<<value;
			context<<','<<packet.sensibleEnergyDeltaJPerM3<<','<<packet.reactedFuelKGPerM3<<','<<
				packet.oxidizedCarbonKGPerM3<<','<<packet.grossCarbonFormedKGPerM3<<','<<
				packet.gasHeatReleaseWPerM3<<','<<packet.sootHeatReleaseWPerM3<<','<<
				packet.pilotEnergyDeltaJPerM3<<','<<packet.pilotExpansionIntegral<<','<<
				packet.radiativeCoolingWPerM3<<'\n';
		}
		context.close();
		if(!context){error="production onset source/state context write failed";return false;}
		return true;
	}

	std::string ProductionBeginningFieldSHA256(const std::string& caseRecordId,
		const PeriodicMACShape& shape,const std::vector<MethaneCellState>& states,
		const PeriodicMACField& momentum,const PeriodicMACField& velocity)
	{
		RISECBOR64::Bytes bytes;
		const char schema[]="rise.fire.production.from_zero_state.v1";
		bytes.insert(bytes.end(),schema,schema+sizeof(schema));
		AppendReplayUInt64(bytes,caseRecordId.size());
		bytes.insert(bytes.end(),caseRecordId.begin(),caseRecordId.end());
		AppendReplayUInt64(bytes,shape.nx);AppendReplayUInt64(bytes,shape.ny);
		AppendReplayUInt64(bytes,shape.nz);AppendReplayDouble(bytes,shape.cellWidthM);
		AppendReplayUInt64(bytes,states.size());
		for(const MethaneCellState& state:states){
			AppendReplayUInt64(bytes,static_cast<std::uint64_t>(state.producerPrecision));
			AppendReplayDouble(bytes,state.temperatureK);
			const ConservativeVector conservative=ToConservativeVector(state);
			for(std::size_t component=0u;component<MethaneConservativeDimension;++component)
				AppendReplayDouble(bytes,conservative[component]);
		}
		for(unsigned int axis=0u;axis<3u;++axis){
			AppendReplayUInt64(bytes,momentum.component[axis].size());
			for(const double value:momentum.component[axis])AppendReplayDouble(bytes,value);
			AppendReplayUInt64(bytes,velocity.component[axis].size());
			for(const double value:velocity.component[axis])AppendReplayDouble(bytes,value);
		}
		return RISECBOR64::SHA256Hex(bytes);
	}

	bool LimitBinary32ProductionStepToEvent(const double simulationTimeS,
		const double eventTimeS,double& timeStepS)
	{
		if(!std::isfinite(simulationTimeS)||!std::isfinite(eventTimeS)||
			!std::isfinite(timeStepS)||!(timeStepS>0.0)||!(simulationTimeS<eventTimeS))return false;
		const double remaining=eventTimeS-simulationTimeS;
		// If one full step would leave a sliver, divide the remaining interval
		// evenly over two steps. Output alignment must not force a microscopic
		// step (and hence a microscopic growth-limited successor) into the EOS solve.
		if(remaining>timeStepS&&remaining<2.0*timeStepS){
			float half=static_cast<float>(0.5*remaining);
			if(static_cast<double>(half)>0.5*remaining)half=std::nextafter(half,0.0f);
			if(!(half>0.0f))return false;
			timeStepS=static_cast<double>(half);return true;
		}
		if(!(remaining<timeStepS))return false;
		float represented=static_cast<float>(remaining);
		if(static_cast<double>(represented)<remaining)represented=std::nextafter(represented,
			std::numeric_limits<float>::infinity());
		if(!std::isfinite(represented)||!(represented>0.0f))return false;
		timeStepS=static_cast<double>(represented);return true;
	}

	bool ProductionSourceBoundaryContactCell(const std::size_t cell,const std::size_t nx,
		const std::size_t ny,const std::vector<std::uint8_t>& canonicalPilotMask,
		const std::vector<double>& sourcePattern)
	{
		if(cell>=canonicalPilotMask.size()||nx==0u||ny==0u||cell>=nx*ny||
			cell>=sourcePattern.size()||sourcePattern[cell]==0.0)return false;
		const std::size_t x=cell%nx,y=cell/nx;
		const std::size_t missing=canonicalPilotMask.size();
		const std::size_t neighbors[4]={x?cell-1u:missing,x+1u<nx?cell+1u:missing,
			y?cell-nx:missing,y+1u<ny?cell+nx:missing};
		for(const std::size_t neighbor:neighbors)
			if(neighbor<canonicalPilotMask.size()&&canonicalPilotMask[neighbor]!=0u)return true;
		return false;
	}

	bool ProductionPilotCommandCell(const std::size_t cell,const std::size_t nx,const std::size_t ny,
		const std::vector<std::uint8_t>& canonicalPilotMask,
		const std::vector<double>& sourcePattern)
	{
		if(cell>=canonicalPilotMask.size())return false;
		return canonicalPilotMask[cell]!=0u||ProductionSourceBoundaryContactCell(
			cell,nx,ny,canonicalPilotMask,sourcePattern);
	}

	bool ProductionEstablishedFlameHolderEligible(const std::size_t cell,
		const std::size_t nx,const std::size_t ny,
		const std::vector<std::uint8_t>& canonicalPilotMask,
		const std::vector<double>& sourcePattern,const bool pilotEstablished,
		const MethaneCellState& candidate,
		const FireSimulationMethaneRecord& fuel,
		const FireSimulationTransportRecord& transport,bool& eligible,std::string* error)
	{
		eligible=false;
		if(!ProductionSourceBoundaryContactCell(cell,nx,ny,canonicalPilotMask,
			sourcePattern))return true;
		const std::vector<double>& ambientMassFraction=fuel.AmbientMassFractions();
		const double ambientProductMassFraction=ambientMassFraction[MethaneCO2]+
			ambientMassFraction[MethaneH2O];
		const double productWitness=std::nextafter(ambientProductMassFraction,
			std::numeric_limits<double>::infinity());
		const double gasDensity=candidate.GasDensity();
		if(!(gasDensity>0.0)||candidate.constituent[MethaneCH4]<=0.0||
			candidate.constituent[MethaneO2]<=0.0)return true;
		if(!pilotEstablished&&
			(candidate.constituent[MethaneCO2]+candidate.constituent[MethaneH2O])/
				gasDensity<productWitness)return true;
		if(!StoichiometricParcelPassesCFT(candidate,fuel,fuel,
			transport.CriticalFlameTemperatureK(),eligible,error))return false;
		return true;
	}


	bool ComputeUniformHannSpectrum( const std::vector<double>& time,
		const std::vector<double>& signal,const std::size_t sampleCount,
		UniformHannSpectrum& spectrum )
	{
		spectrum=UniformHannSpectrum();
		if(time.size()<8u||time.size()!=signal.size()||sampleCount<8u||
			(sampleCount&1u)!=0u||!(time.back()>time.front()))return false;
		spectrum.uniformTimeS.resize(sampleCount);
		spectrum.uniformSignal.resize(sampleCount);
		const double duration=time.back()-time.front();
		std::size_t right=1u;
		for(std::size_t sample=0;sample<sampleCount;++sample) {
			const double target=time.front()+duration*static_cast<double>(sample)/
				static_cast<double>(sampleCount-1u);
			spectrum.uniformTimeS[sample]=target;
			while(right<time.size()&&time[right]<target) ++right;
			if(right>=time.size()) spectrum.uniformSignal[sample]=signal.back();
			else {
				const double span=time[right]-time[right-1u];
				const double fraction=span>0.0?(target-time[right-1u])/span:0.0;
				spectrum.uniformSignal[sample]=signal[right-1u]+
					fraction*(signal[right]-signal[right-1u]);
			}
		}
		// Remove the least-squares affine trend before applying a Hann window.  The
		// direct DFT below is an independent harness calculation, not a solver path.
		double sumX=0.0,sumY=0.0,sumXX=0.0,sumXY=0.0;
		for(std::size_t sample=0;sample<sampleCount;++sample) {
			const double x=static_cast<double>(sample);
			sumX+=x;sumY+=spectrum.uniformSignal[sample];sumXX+=x*x;
			sumXY+=x*spectrum.uniformSignal[sample];
		}
		const double denominator=static_cast<double>(sampleCount)*sumXX-sumX*sumX;
		spectrum.affineSlopePerSample=denominator!=0.0?
			(static_cast<double>(sampleCount)*sumXY-sumX*sumY)/denominator:0.0;
		spectrum.affineIntercept=(sumY-spectrum.affineSlopePerSample*sumX)/
			static_cast<double>(sampleCount);
		spectrum.detrendedSignal.resize(sampleCount);
		double detrendedEnergy=0.0,signalScale=0.0;
		for(std::size_t sample=0;sample<sampleCount;++sample) {
			const double residual=spectrum.uniformSignal[sample]-spectrum.affineIntercept-
				spectrum.affineSlopePerSample*static_cast<double>(sample);
			spectrum.detrendedSignal[sample]=residual;
			detrendedEnergy+=residual*residual;
			signalScale=std::max(signalScale,std::fabs(spectrum.uniformSignal[sample]));
		}
		if(!(detrendedEnergy>64.0*std::numeric_limits<double>::epsilon()*
			std::max(1.0,signalScale*signalScale)))return false;
		const std::size_t binCount=sampleCount/2u+1u;
		spectrum.frequencyHz.resize(binCount);spectrum.amplitude.resize(binCount);
		spectrum.power.resize(binCount);spectrum.powerFraction.resize(binCount);
		double bestPower=-1.0,totalPositivePower=0.0,windowSum=0.0;
		for(std::size_t sample=0;sample<sampleCount;++sample)windowSum+=
			0.5-0.5*std::cos(2.0*3.14159265358979323846*
				static_cast<double>(sample)/static_cast<double>(sampleCount-1u));
		for(std::size_t bin=0u;bin<binCount;++bin) {
			double real=0.0,imaginary=0.0;
			for(std::size_t sample=0;sample<sampleCount;++sample) {
				const double window=0.5-0.5*std::cos(2.0*3.14159265358979323846*
					static_cast<double>(sample)/static_cast<double>(sampleCount-1u));
				const double value=spectrum.detrendedSignal[sample]*window;
				const double angle=2.0*3.14159265358979323846*static_cast<double>(bin*sample)/
					static_cast<double>(sampleCount);
				real+=value*std::cos(angle);imaginary-=value*std::sin(angle);
			}
			const double power=real*real+imaginary*imaginary;
			spectrum.frequencyHz[bin]=static_cast<double>(bin)*
				static_cast<double>(sampleCount-1u)/(static_cast<double>(sampleCount)*duration);
			const double oneSidedScale=(bin==0u||bin+1u==binCount)?1.0:2.0;
			spectrum.amplitude[bin]=windowSum>0.0?oneSidedScale*std::sqrt(power)/windowSum:0.0;
			spectrum.power[bin]=power;
			if(bin>0u){totalPositivePower+=power;if(power>bestPower){bestPower=power;
				spectrum.dominantBin=bin;}}
		}
		if(!(totalPositivePower>0.0))return false;
		for(std::size_t bin=0u;bin<binCount;++bin)
			spectrum.powerFraction[bin]=spectrum.power[bin]/totalPositivePower;
		return spectrum.dominantBin>0u;
	}

	double DominantUniformResampledFrequency( const std::vector<double>& time,
		const std::vector<double>& signal )
	{
		UniformHannSpectrum spectrum;
		return ComputeUniformHannSpectrum(time,signal,512u,spectrum)?
			spectrum.frequencyHz[spectrum.dominantBin]:0.0;
	}

	void FitCenterlineTemperaturePowerLaw( SolverFrameValues& values )
	{
		if(values.centerlineHeightM.size()!=values.centerlineTemperatureK.size()) return;
		double bestError=std::numeric_limits<double>::infinity(),bestSlope=0.0;
		const double spacing=values.cellWidthM;
		for(unsigned int originStep=0;originStep<32u;++originStep) {
			const double origin=-2.0*spacing+4.0*spacing*static_cast<double>(originStep)/31.0;
			double sx=0.0,sy=0.0,sxx=0.0,sxy=0.0,syy=0.0;std::size_t count=0u;
			for(std::size_t i=0;i<values.centerlineHeightM.size();++i) {
				const double distance=values.centerlineHeightM[i]-origin;
				const double excess=values.centerlineTemperatureK[i]-300.0;
				if(values.centerlineHeightM[i]<=values.mccaffreyFlameTipHeightM||
					distance<=2.0*spacing||excess<=1.0) continue;
				const double x=std::log(distance),y=std::log(excess);
				sx+=x;sy+=y;sxx+=x*x;sxy+=x*y;syy+=y*y;++count;
			}
			if(count<4u) continue;
			const double divisor=static_cast<double>(count)*sxx-sx*sx;
			if(divisor==0.0) continue;
			const double fittedSlope=(static_cast<double>(count)*sxy-sx*sy)/divisor;
			const double fittedIntercept=(sy-fittedSlope*sx)/static_cast<double>(count);
			const double squared=syy+static_cast<double>(count)*fittedIntercept*fittedIntercept+
				fittedSlope*fittedSlope*sxx+2.0*fittedIntercept*fittedSlope*sx-
				2.0*fittedIntercept*sy-2.0*fittedSlope*sxy;
			const double error=std::sqrt(std::max(0.0,squared/static_cast<double>(count)));
			if(error<bestError) {bestError=error;bestSlope=fittedSlope;}
		}
		if(std::isfinite(bestError)) {
			values.centerlineTemperatureExponent=bestSlope;
			values.centerlineFitRMSE=bestError;
		}
	}

	void EvaluateMcCaffreyPlumeStations( SolverFrameValues& values,
		const double heatReleaseRateKW )
	{
		if(values.centerlineHeightM.size()!=values.centerlineTemperatureK.size()||
			values.centerlineHeightM.size()!=values.centerlineVelocityMPerS.size()||
			!(heatReleaseRateKW>0.0)) return;
		// McCaffrey, NBSIR 79-1910: compare to the adopted absolute 33 kW
		// plume row, rather than re-deriving rounded constants from one another.
		const double qTwoFifths=std::pow(heatReleaseRateKW,0.4);
		const double qOneFifth=std::pow(heatReleaseRateKW,0.2);
		const double top=values.centerlineHeightM.empty()?0.0:
			values.centerlineHeightM.back()+0.5*values.cellWidthM;
		const double maximumUncontaminatedHeight=top-0.5*values.characteristicDiameterM;
		const double minimumHeight=std::max(values.mccaffreyFlameTipHeightM,
			0.2*qTwoFifths);
		for(std::size_t station=0;station<values.centerlineHeightM.size();++station){
			const double height=values.centerlineHeightM[station];
			if(!(height>minimumHeight&&height<=maximumUncontaminatedHeight)) continue;
			const double normalizedHeight=height/qTwoFifths;
			const double expectedVelocity=McCaffrey33KWVelocityCoefficient*qOneFifth*
				std::pow(normalizedHeight,-1.0/3.0);
			const double expectedTemperatureRise=McCaffrey33KWTemperatureCoefficientK*
				std::pow(normalizedHeight,-5.0/3.0);
			const double observedTemperatureRise=values.centerlineTemperatureK[station]-300.0;
			values.mccaffreyMaximumTemperatureRelativeError=std::max(
				values.mccaffreyMaximumTemperatureRelativeError,
				std::fabs(observedTemperatureRise-expectedTemperatureRise)/
					expectedTemperatureRise);
			values.mccaffreyMaximumVelocityRelativeError=std::max(
				values.mccaffreyMaximumVelocityRelativeError,
				std::fabs(values.centerlineVelocityMPerS[station]-expectedVelocity)/
					expectedVelocity);
			++values.mccaffreyPlumeStationCount;
		}
	}

	bool OwnerEOSDiagnosticSnapshotDirectoryAccepted(const FireRunPersistenceOptions& persistence)
	{
		if(persistence.equivalenceSnapshotDirectory.empty())return true;
		return persistence.projectedHeunEOSDiagnostic&&!persistence.projectedHeunCertificateDiagnostic&&
			persistence.productionMetal&&persistence.isolatedEquivalenceProbe&&
			!persistence.portedProductionTransport&&
			persistence.productionOnsetDiagnosticDirectory.filename()=="budgets"&&
			persistence.equivalenceSnapshotDirectory==
				persistence.productionOnsetDiagnosticDirectory.parent_path()/"checkpoints";
	}

	bool R226RecoveryEndpointAccepted(const std::size_t steps,const double dt,const double end)
	{
		return steps==6532u&&dt==R226RefusalStepS&&end==R226RefusalBeginningS+R226RefusalStepS;
	}
	bool WindowRefusalDiagnosticInput(const std::string& digest,const std::string& build)
	{
		return (digest==R225CheckpointSHA&&build==R225ProducerBuild)||
			(digest==R226CheckpointSHA&&build==R226ProducerBuild);
	}

	bool R216KernelAccepted(const std::string& kernel)
	{
		return kernel=="d580d078ea083ee18032abfb027271ada9d4d1dabab50b35cb6eb7a503258a0e";
	}
	bool R216RetainedScopeAccepted(const double tier,const bool resume,
		const std::string& checkpointSHA,const bool continuation,const bool probe)
	{
		return tier==10.0&&resume&&checkpointSHA==R216CheckpointSHA&&(continuation||probe);
	}
	bool R216AttemptPublicationAccepted(const bool scoped,const bool computed,const std::string& kernel)
	{
		return !scoped||!computed||R216KernelAccepted(kernel);
	}
	// RISE_FIRE_FAST_MODE=1: exact per-step ledger of the accepted resident step (state change =
	// source dose + boundary net inflow, per row), the safety net that replaces certification.
	// RISE_FIRE_FAST_CONSERVATION_EVERY=<n> (default 1; 0 disables) sets the check cadence in
	// accepted steps; one FAST_CONSERVATION line is logged every 10 checks (RISE_FIRE_FAST_CONSERVATION_LOG_EVERY)
	// and on any failure.
	unsigned int FastModeConservationEvery()
	{
		static const unsigned int every=[](){const char* value=std::getenv("RISE_FIRE_FAST_CONSERVATION_EVERY");
			if(!value||!*value)return 1u;char* end=nullptr;const unsigned long parsed=std::strtoul(value,&end,10);
			return end&&!*end&&parsed<=100000u?static_cast<unsigned int>(parsed):1u;}();
		return every;
	}
	std::uint64_t& FastModeConservationCheckCount()
	{
		static std::uint64_t checks=0u;return checks;
	}
	bool FastModeConservationDue(const std::uint64_t acceptedSteps)
	{
		return RISE::FireSim::FireFastMode()&&FastModeConservationEvery()!=0u&&
			(acceptedSteps+1u)%FastModeConservationEvery()==0u;
	}
	bool FastModeConservationCheck(const FireProductionProjectedHeunMetalOwnerRequest& request,
		FireProductionProjectedHeunMetalOwnerResult& owner,const std::uint64_t stepIndex,
		const std::vector<double>& sourcePattern,std::string& error)
	{
		std::vector<unsigned char> footprint;footprint.reserve(sourcePattern.size());
		for(const double value:sourcePattern)footprint.push_back(value>0.0?1u:0u);
		FireProductionAcceptedStageBudget::Step step;
		if(!FireProductionAcceptedStageBudget::Reduce(request,owner,stepIndex,footprint,step,error)){
			error="fast-mode conservation ledger unavailable: "+error;return false;}
		// Relative closure |end - begin - source + out - in| / (|begin| + |source| + in + out).
		auto relative=[](const long double closure,const long double scale){
			return static_cast<double>(scale>0.0L?std::fabs(closure)/scale:std::fabs(closure));};
		auto scaleOf=[](const FireProductionAcceptedStageBudget::Row& row){long double scale=
			std::fabs(row.beginning)+std::fabs(row.source);
			for(unsigned int side=0u;side<6u;++side)scale+=row.inflow[side]+row.outflow[side];return scale;};
		// Elements C, H, O, N from species rows 1..7 (CH4, O2, N2, CO2, H2O, CO, C(s)).
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		static const double atoms[4][7]={{1,0,0,1,0,1,1},{4,0,0,0,2,0,0},{0,2,0,2,1,1,0},{0,0,2,0,0,0,0}};
		static const double atomicMass[4]={12.011,1.008,15.999,14.007};
		std::array<long double,4> elementClosure{},elementScale{};
		for(std::size_t species=0u;species<7u;++species){
			const auto* record=fuel.FindSpecies(fuel.SpeciesOrder()[species].c_str());
			if(!record||!(record->molecularWeightKGPerKMol>0.0)){error="fast-mode conservation species record missing";return false;}
			const FireProductionAcceptedStageBudget::Row& row=step.rows[1u+species];
			for(unsigned int element=0u;element<4u;++element){
				const long double fraction=atoms[element][species]*atomicMass[element]/record->molecularWeightKGPerKMol;
				elementClosure[element]+=fraction*row.closure;elementScale[element]+=fraction*scaleOf(row);}
		}
		const double massRelative=relative(step.rows[9].closure,scaleOf(step.rows[9]));
		const double energyRelative=relative(step.rows[8].closure,scaleOf(step.rows[8]));
		// Species closures are measured against the total-mass throughput: a trace species (soot or CO
		// early in a run) carries binary32 mixture rounding far above its own tiny scale.
		double speciesRelative=0.0;unsigned int worstSpecies=0u;
		for(unsigned int row=1u;row<=7u;++row){const double value=relative(step.rows[row].closure,scaleOf(step.rows[9]));
			if(value>speciesRelative){speciesRelative=value;worstSpecies=row-1u;}}
		std::array<double,4> elementRelative{};double worstElement=0.0;
		for(unsigned int element=0u;element<4u;++element){
			elementRelative[element]=relative(elementClosure[element],elementScale[element]);
			worstElement=std::max(worstElement,elementRelative[element]);}
		// Binary32 state rounding alone gives about 1e-7; 1e-5 flags a real leak, not noise.
		constexpr double tolerance=1.0e-5;
		const bool passed=massRelative<=tolerance&&energyRelative<=tolerance&&speciesRelative<=tolerance&&
			worstElement<=tolerance&&std::isfinite(massRelative+energyRelative+speciesRelative+worstElement);
		const std::uint64_t checks=++FastModeConservationCheckCount();
		static const std::uint64_t logEvery=[](){const char* value=std::getenv("RISE_FIRE_FAST_CONSERVATION_LOG_EVERY");
			const unsigned long parsed=value?std::strtoul(value,nullptr,10):10u;return parsed>0u?parsed:10u;}();
		if(!passed||checks%logEvery==1u%logEvery)std::fprintf(stderr,"FAST_CONSERVATION step=%llu time=%.9f dt=%.9g mass_rel=%.3e "
			"species_max_rel=%.3e (species %u) C_rel=%.3e H_rel=%.3e O_rel=%.3e N_rel=%.3e energy_rel=%.3e "
			"mass_kg=%.9e energy_j=%.9e tolerance=%.1e %s\n",static_cast<unsigned long long>(stepIndex),
			step.beginTime+step.dt,step.dt,massRelative,speciesRelative,worstSpecies,elementRelative[0],elementRelative[1],
			elementRelative[2],elementRelative[3],energyRelative,static_cast<double>(step.rows[9].ending),
			static_cast<double>(step.rows[8].ending),tolerance,passed?"ok":"FAILED");
		if(!passed){std::ostringstream message;message<<std::setprecision(6)<<"fast-mode conservation check failed at step "
			<<stepIndex<<": mass_rel="<<massRelative<<" species_max_rel="<<speciesRelative<<" (species "<<worstSpecies<<") element_max_rel="
			<<worstElement<<" energy_rel="<<energyRelative<<" (tolerance "<<tolerance<<")";error=message.str();}
		return passed;
	}


	bool PrepareR245Budget(const FireProductionProjectedHeunMetalOwnerRequest& request,
		FireProductionProjectedHeunMetalOwnerResult& owner,std::uint64_t stepIndex,
		const std::vector<double>& sourcePattern,bool frameDue,std::uint64_t frameIndex,
		R245PendingBudget& pending,std::string& error)
	{
		pending=R245PendingBudget();
		std::vector<unsigned char> footprint;footprint.reserve(sourcePattern.size());
		for(double value:sourcePattern){if(!std::isfinite(value)||value<0){error="r245 invalid source mask";return false;}
			footprint.push_back(value>0?1u:0u);}
		if(!FireProductionAcceptedStageBudget::Reduce(request,owner,stepIndex,footprint,pending.step,error))return false;
		pending.frameDue=frameDue;pending.frame=frameIndex;
		if(frameDue){
			// Independent CPU checks cover both GPU payload roots at retained frames.
			FireProductionPayloadDigestV2 nativeSeal,fluxSeal;
			if(!FireProductionPayloadDigestCPU(owner.acceptedBudgetPayload.data(),owner.acceptedBudgetPayload.size(),8u,fluxSeal,&error)||
				fluxSeal.rootSHA256!=owner.acceptedBudgetPayloadRootSHA256||
				!FireProductionPayloadDigestCPU(owner.acceptedBudgetNativePublication.data(),owner.acceptedBudgetNativePublication.size(),8u,nativeSeal,&error)||
				nativeSeal.rootSHA256!=owner.publicationPayloadRootSHA256){error="r245 GPU/CPU observation seal mismatch";return false;}
			const auto& transport=request.lineage.eos.physicalFlux.transport;
			const auto& shape=transport.shape;
			const FireProductionStaggeredColumn::Inputs inputs={request.beginningMomentumKGPerM2S,
				owner.provisionalMomentumKGPerM2S,owner.momentumKGPerM2S,
				owner.heunStressMomentumRateKGPerM2S2,owner.heunBuoyancyMomentumRateKGPerM2S2,
				owner.heunAdvectionMomentumRateKGPerM2S2,owner.heunPhaseSourceMomentumRateKGPerM2S2,
				owner.heunEddyKinematicViscosityM2PerS};
			std::ostringstream columns;bool header=true;
			if(shape.nx<2||shape.ny<2||shape.nx%2||shape.ny%2){error="r245 central columns require even extents";return false;}
			for(std::size_t y=shape.ny/2u-1u;y<=shape.ny/2u;++y)
				for(std::size_t x=shape.nx/2u-1u;x<=shape.nx/2u;++x){
					std::vector<FireProductionStaggeredColumn::Row> rows;
					std::ostringstream oneColumn;
					if(!FireProductionStaggeredColumn::Build({{shape.nx,shape.ny,shape.nz}},x,y,pending.step.dt,inputs,rows)||
						!FireProductionStaggeredColumn::Write(oneColumn,header,pending.step.beginTime,0u,pending.step.dt,rows)){
						error="r245 accepted Heun column operands incomplete";return false;}
					std::istringstream lines(oneColumn.str());std::string line;
					while(std::getline(lines,line)){
						if(header){const auto label=line.find("candidate,");
							if(label!=std::string::npos)line.replace(label,10u,"observation_index,");
							columns<<"column_x,column_y,"<<line<<'\n';header=false;}
						else columns<<x<<','<<y<<','<<line<<'\n';}

				}
			pending.columns=columns.str();
			// Little-endian binary32 wire order: Q0[9*cells], source delta[9*cells],
			// M0[allFaces], five binary64 frozen-source arrays, then burner-mask bytes.
			// Native publication contains final Q, momentum, actual
			// Heun rates and alpha; flux packet binds the input and native roots.
			const std::size_t cells=shape.nx*shape.ny*shape.nz;
			std::size_t faces=0;for(const auto& axis:request.beginningMomentumKGPerM2S)faces+=axis.size();
			pending.beginning.reserve(112u*cells+4u*faces+footprint.size());
			auto append=[&](const auto& values){const auto* bytes=
				reinterpret_cast<const unsigned char*>(values.data());
				pending.beginning.insert(pending.beginning.end(),bytes,bytes+values.size()*sizeof(values[0]));};
			append(transport.conservativeValues);append(request.lineage.frozenSource.SourceDelta());
			for(const auto& axis:request.beginningMomentumKGPerM2S)append(axis);
			const auto& frozen=request.lineage.frozenSource;
			append(frozen.GasHeatReleaseWPerM3());append(frozen.SootHeatReleaseWPerM3());
			append(frozen.RadiativeCoolingWPerM3());append(frozen.PilotEnergyDeltaJPerM3());append(frozen.ReactedFuelKGPerM3());
			pending.beginning.insert(pending.beginning.end(),footprint.begin(),footprint.end());
			pending.flux=std::move(owner.acceptedBudgetPayload);
			pending.native=std::move(owner.acceptedBudgetNativePublication);
		}
		pending.ready=true;return true;
	}

	void WriteR245Rows(std::ostream& stream,const char* scope,const FireProductionAcceptedStageBudget::Rows& rows)
	{
		for(unsigned field=0;field<10;++field){const auto& row=rows[field];
			stream<<std::setprecision(21)<<scope<<','<<field<<','<<(field==8?"J":"kg")<<','
				<<row.beginning<<','<<row.ending<<','<<row.source<<','<<row.closure<<','<<row.interstepChange;
			for(unsigned side=0;side<6;++side)stream<<','<<row.inflow[side]<<','<<row.outflow[side];
			for(unsigned region=0;region<2;++region)stream<<','<<row.bottomInflow[region]<<','<<row.bottomOutflow[region];
			stream<<'\n';
		}
	}

	bool PublishR245Budget(const std::filesystem::path& directory,const std::string& caseId,
		const std::string& producer,const std::string& executable,double statisticsStart,
		const std::array<std::size_t,3>& dimensions,double solverDx,R245PendingBudget& pending,
		FireProductionAcceptedStageBudget::Ledger& ledger,std::string& error)
	{
		if(!pending.ready){error="r245 accepted step lacks pending observation";return false;}
		// Commit a local copy; failed I/O never advances the in-memory ledger.
		auto next=ledger;
		if(!next.Commit(pending.step,true,statisticsStart,error))return false;
		std::ostringstream number;number<<"step_"<<std::setw(6)<<std::setfill('0')<<pending.step.index;
		const auto path=directory/(number.str()+".csv");
		if(std::filesystem::exists(path)){error="r245 step evidence already exists";return false;}
		std::ofstream record(path);
		record<<std::setprecision(17)<<"# schema rise.fire.accepted-stage-budget.v1\n# case_record_id "<<caseId
			<<"\n# producer_build_id "<<producer<<"\n# executable_sha256 "<<executable
			<<"\n# accepted_step "<<pending.step.index<<"\n# beginning_time_s "<<pending.step.beginTime
			<<"\n# dt_s "<<pending.step.dt<<"\n# input_root "<<pending.step.inputRoot
			<<"\n# native_root "<<pending.step.publicationRoot<<"\n# flux_root "<<pending.step.fluxRoot
			<<"\n# R1_candidate "<<pending.step.candidate<<"\n# dimensions "<<dimensions[0]<<' '<<dimensions[1]<<' '<<dimensions[2]
			<<"\n# binary32_solver_dx_m "<<solverDx<<"\n# statistics_start_s "<<statisticsStart
			<<"\n# statistics_initial_inventory clipped_linear_increment_not_interior_state"
			<<"\n# field_order Z,CH4,O2,N2,CO2,H2O,CO,carbon,sensible_enthalpy,total_species_mass"
			<<"\n# side_order x_min,x_max,y_min,y_max,z_min,z_max; bottom_regions nonburner,burner"
			<<"\n# enthalpy_source_components chemical_J,radiated_J,pilot_J,reacted_fuel_kg\n# step_source "
			<<pending.step.chemicalJ<<','<<pending.step.radiatedJ<<','<<pending.step.pilotJ<<','<<pending.step.reactedFuelKG
			<<"\n# cumulative_source "<<next.chemicalJ<<','<<next.radiatedJ<<','<<next.pilotJ<<','<<next.reactedFuelKG
			<<"\n# statistics_source "<<next.statisticsChemicalJ<<','<<next.statisticsRadiatedJ<<','<<next.statisticsPilotJ<<','<<next.statisticsReactedFuelKG
			<<"\nscope,field,units,beginning_inventory,ending_inventory,source_increment,closure_residual,interstep_inventory_change,xmin_in,xmin_out,xmax_in,xmax_out,ymin_in,ymin_out,ymax_in,ymax_out,bottom_in,bottom_out,top_in,top_out,bottom_nonburner_in,bottom_nonburner_out,bottom_burner_in,bottom_burner_out\n";
		WriteR245Rows(record,"step",pending.step.rows);
		WriteR245Rows(record,"full",next.full);
		WriteR245Rows(record,"statistics",next.statistics);
		if(!ClosePublishedStream(record)||!PublishPayloadMerkleSidecar(path,caseId,error))return false;
		if(pending.frameDue){
			std::ostringstream frame;frame<<"frame_"<<std::setw(5)<<std::setfill('0')<<pending.frame;
			const auto base=directory/frame.str();
			std::ofstream binding(base.string()+".v1");
			binding<<"schema rise.fire.accepted-stage-frame.v1\naccepted_step "<<pending.step.index
				<<"\nframe "<<pending.frame<<"\nstep_record_sha256 "<<DigestFile(path)<<'\n';
			const std::array<std::pair<const char*,const std::vector<unsigned char>*>,3> payloads={{{".flux.bin",&pending.flux},{".native.bin",&pending.native},{".beginning.bin",&pending.beginning}}};
			for(const auto& payload:payloads){const std::filesystem::path target=base.string()+payload.first;
				if(std::filesystem::exists(target)){error="r245 frame evidence already exists";return false;}
				std::ofstream binary(target,std::ios::binary);binary.write(reinterpret_cast<const char*>(payload.second->data()),payload.second->size());
				if(!ClosePublishedStream(binary)||!PublishPayloadMerkleSidecar(target,caseId,error))return false;
				binding<<payload.first<<' '<<DigestFile(target)<<'\n';}
			const std::filesystem::path columnPath=base.string()+".columns.csv";
			std::ofstream columns(columnPath);columns<<pending.columns;
			if(!ClosePublishedStream(columns)||!PublishPayloadMerkleSidecar(columnPath,caseId,error))return false;
			binding<<"columns_sha256 "<<DigestFile(columnPath)<<'\n';
			if(!ClosePublishedStream(binding)||!PublishPayloadMerkleSidecar(base.string()+".v1",caseId,error))return false;
		}
		ledger=next;pending=R245PendingBudget();return true;
	}

	bool ProductionOwnerObservationReserveBytes(const PeriodicMACShape& shape,
		const bool observe,std::uint64_t& bytes)
	{
		FireProductionProjectionShape memoryShape;memoryShape.nx=shape.nx;
		memoryShape.ny=shape.ny;memoryShape.nz=shape.nz;
		memoryShape.cellWidthM=static_cast<float>(shape.cellWidthM);
		// All launcher cases retain the prescribed source plane, including
		// observer-off runs. Its mask/flux bytes are independent of observation.
		if(!FireProductionProjectedHeunMetalObserverWorkingSetBytes(memoryShape,bytes,true,observe))
			return false;
		if(observe){
			const std::uint64_t cells=memoryShape.CellCount(),faces=
				FireProductionProjectionFaceCount(memoryShape,0u)+FireProductionProjectionFaceCount(memoryShape,1u)+
				FireProductionProjectionFaceCount(memoryShape,2u);
			const std::uint64_t extra=(72u+104u*faces)+(76u*cells+36u*faces+216u)+
				(112u*cells+4u*faces)+memoryShape.nx*memoryShape.ny;
			if(bytes>UINT64_MAX-extra)return false;
			bytes+=extra;
		}
		return true;
	}

	bool R246PreallocationPolicy(const PeriodicMACShape& shape,FireRunHostMemory::Policy& policy,
		std::uint64_t& initialFields)
	{
		// This experiment has a single reviewed geometry; refuse arbitrary grids
		// before count arithmetic or field allocation. The owner certificate includes
		// full device work and observation capsules. Enumerate additionally live host
		// initial fields; the existing general scratch reserve remains unchanged.
		if(shape.nx!=104u||shape.ny!=104u||shape.nz!=160u)return false;
		std::uint64_t owner=0u;
		if(!ProductionOwnerObservationReserveBytes(shape,true,owner)){
			FireProductionProjectionShape diagnostic;diagnostic.nx=shape.nx;diagnostic.ny=shape.ny;
			diagnostic.nz=shape.nz;diagnostic.cellWidthM=static_cast<float>(shape.cellWidthM);
			std::uint64_t projection=0u,target=0u,nonpressure=0u;
			const bool projectionOK=FireProductionProjectionWorkingSetBytes(diagnostic,projection,true);
			const bool targetOK=FireProductionResidentTargetLineageMetalWorkingSetBytes(diagnostic,target,true);
			const bool nonpressureOK=FireProductionNonpressureMomentumRHSMetalWorkingSetBytes(diagnostic,nonpressure,true);
			std::fprintf(stdout,"R246_COMPONENT_ADMISSION projection_ok=%d projection_bytes=%llu target_ok=%d target_bytes=%llu nonpressure_ok=%d nonpressure_bytes=%llu\n",
				projectionOK?1:0,static_cast<unsigned long long>(projection),targetOK?1:0,
				static_cast<unsigned long long>(target),nonpressureOK?1:0,static_cast<unsigned long long>(nonpressure));
			return false;
		}
		const std::uint64_t cells=shape.CellCount(),plane=shape.nx*shape.ny,
			faces=OpenMACFaceCount3D(shape,0u)+OpenMACFaceCount3D(shape,1u)+OpenMACFaceCount3D(shape,2u);
		initialFields=cells*(sizeof(MethaneCellState)+sizeof(MethaneReactionStep)+
			3u*sizeof(float)+sizeof(std::array<float,3>)+sizeof(std::uint8_t))+
			2u*faces*sizeof(double)+2u*plane*sizeof(double)+(plane+7u)/8u+
			3u*shape.nz*sizeof(double)+2u*(shape.nx*shape.ny+shape.nx*shape.nz+shape.ny*shape.nz);
		if(owner>UINT64_MAX-initialFields)return false;
		policy=FireRunHostMemory::ProductionPolicy(owner+initialFields);
		return policy.Valid();
	}



	// r275 opt-in: RISE_FIRE_WORKERS=<n> (1 <= n <= hardware threads) replaces the eight CPU
	// workers of the temporal capstone and the r273 A/B drivers. Every worker-parallel reduction
	// is fixed-order (r61/r112), so the state is unchanged; only workerCountHistory records n.
	// Unset or invalid keeps the default.
	unsigned int FireRunWorkerCount(const unsigned int defaultWorkers)
	{
		const char* option=std::getenv("RISE_FIRE_WORKERS");
		if(!option)return defaultWorkers;
		char* end=nullptr;const unsigned long value=std::strtoul(option,&end,10);
		return end!=option&&*end=='\0'&&value>=1ul&&value<=FireWorkerCapacity()?
			static_cast<unsigned int>(value):defaultWorkers;
	}

	// r275 opt-in, UNCERTIFIED: RISE_FIRE_SOURCE_STEP_CAP_S=<s> (s > 0, "inf" allowed) replaces the
	// r175 source-step ceiling wherever a production run applies one. r175 certified the Binary32
	// source producer only at the 1.646 ms step measured on one sealed tier-10 beginning; it is
	// not a formula and does not re-derive for another grid. The selector (CFL, buoyancy,
	// diffusion, 1.1x growth), the per-step gates and the halving retries stay in force.
	// Unset or invalid keeps the certified ceiling, so the default path is unchanged.
	double FireProductionSourceStepCapS(const double certifiedCapS)
	{
		static const double option=[](){const char* text=std::getenv("RISE_FIRE_SOURCE_STEP_CAP_S");
			if(!text)return 0.0;char* end=nullptr;const double value=std::strtod(text,&end);
			return end!=text&&*end=='\0'&&!std::isnan(value)&&value>0.0?value:0.0;}();
		return option>0.0?option:certifiedCapS;
	}

	SolverFrameValues RunFireMethaneDriver( const unsigned int workerCount,
		const unsigned int minimumStepCount,const double targetTimeS,
		const double caseDurationS,const double caseFramesPerS,
		const double resolutionTier,const double poolDiameterM,
		const double heatReleaseRateKW,
		const FireRunPersistenceOptions& persistence )
	{
		const bool injectSolverFailure=persistence.Test().injectSolverFailure;
		SolverFrameValues values;
		if((persistence.Test().r246MemoryAdmissionRED||persistence.Test().r246DeviceAdmissionRED)&&!persistence.r246TopDouble){
			values.structuredError="r246_memory_fixture_scope_conflict";return values;}
		if(persistence.r246FullWindow&&!persistence.r246TopDouble){
			values.structuredError="r246_full_window_selector_conflict";return values;}
		if(persistence.r246TopDouble&&(!persistence.r244LateralDouble||
			!persistence.productionMetal||!persistence.portedProductionTransport||persistence.resume||
			resolutionTier!=6.0||caseDurationS!=25.156864278900095||caseFramesPerS!=16.0||
			(persistence.r246FullWindow?
				(minimumStepCount!=1u||targetTimeS!=caseDurationS||!persistence.r245AcceptedBudget||
				persistence.checkpointPath.empty()||persistence.finalCheckpointPath.empty()||
				persistence.retainedCheckpointDirectory.empty()||persistence.checkpointCadenceWallS!=300.0||
				persistence.maximumProductionSourceStepS!=static_cast<double>(static_cast<float>(0.0016462659696117043))||
				persistence.Test().killAfterFirstCheckpoint||persistence.Test().forceZeroSourceForTest||injectSolverFailure):
				(minimumStepCount!=0u||targetTimeS!=0.0)))){
			values.structuredError="r246_engineering_probe_scope_conflict";return values;}
		if(persistence.Test().r245ObservationFixture&&!persistence.Test().burnerLauncherFixture){
			values.structuredError="r245_observer_fixture_scope_conflict";return values;}
		if(persistence.r245AcceptedBudget&&((!persistence.r244LateralDouble&&resolutionTier!=8.0)||persistence.resume||
			!persistence.portedProductionTransport||!persistence.productionMetal||
			persistence.productionOnsetDiagnosticDirectory.empty()||persistence.temporalSnapshotDirectory.empty()||
			persistence.temporalSnapshotCadenceS!=0.0625||targetTimeS<=0||
			persistence.stopAfterAdditionalAcceptedSteps||persistence.isolatedEquivalenceProbe)){
			values.structuredError="r245_budget_scope_conflict";return values;}
		if(persistence.r244LateralDouble&&(resolutionTier!=6.0||persistence.resume||
			poolDiameterM!=CapstonePoolDiameterM||heatReleaseRateKW!=CapstoneHeatReleaseRateKW||
			(minimumStepCount>0u&&(!persistence.productionMetal||!persistence.portedProductionTransport)))){
			values.structuredError="r244_domain_study_scope_conflict";return values;}
		if((persistence.Test().burnerLauncherFixture||persistence.Test().burnerLauncherRejectFirstCandidate)&&
			(!persistence.Test().burnerLauncherFixture||!persistence.portedProductionTransport||
			minimumStepCount!=1u||targetTimeS!=0.0||persistence.resume||
			!persistence.checkpointPath.empty()||!persistence.finalCheckpointPath.empty()||
			!persistence.productionOnsetDiagnosticDirectory.empty()||
			!persistence.temporalSnapshotDirectory.empty())){
			values.structuredError="burner_launcher_fixture_scope_conflict";return values;}
		if(persistence.acceptedFuelBudgetDiagnostic&&(!persistence.productionMetal||
			!persistence.resume||!persistence.isolatedEquivalenceProbe||
			persistence.stopAfterAdditionalAcceptedSteps!=8u||resolutionTier!=10.0||
			caseDurationS!=25.05154563611109||caseFramesPerS!=16.0||targetTimeS!=0.0||
			persistence.portedProductionTransport||persistence.compatibleMomentumDiagnostic||
			persistence.singleStageFCTDiagnostic||persistence.sealedLegacyMomentumReplay||
			persistence.sealedProjectedHeunReplay||persistence.sealedProjectedHeunContinuation||
			persistence.projectedHeunCostDiagnostic||persistence.projectedHeunEOSDiagnostic||
			persistence.projectedHeunCertificateDiagnostic||persistence.Test().forceZeroSourceForTest||
			persistence.Test().killAfterFirstCheckpoint||!persistence.resumeEquivalenceCertificatePath.empty()||
			!persistence.finalCheckpointPath.empty()||!persistence.retainedCheckpointDirectory.empty()||
			!persistence.equivalenceSnapshotDirectory.empty()||!persistence.temporalSnapshotDirectory.empty()||
			persistence.productionOnsetDiagnosticDirectory.empty()||
			persistence.checkpointCadenceWallS!=std::numeric_limits<double>::max()||
			persistence.isolatedExpectedCheckpointDigest!=
				"68421f97b1035a6788b134c5c1e1ce65fddc9330aeb20ed929298c27c77345ae"||
			DigestFile(persistence.checkpointPath)!=persistence.isolatedExpectedCheckpointDigest||
			persistence.isolatedExpectedCheckpointBuildId!=
				"fdb9eaf08be4ec8e9a49b1e4ea2283a0e27360226bdec786f7bdb4e591addc41")){
			values.structuredError="accepted_fuel_budget_diagnostic_scope_refused";return values;}
		if(persistence.r221Retained&&(!persistence.productionMetal||!persistence.resume||
			resolutionTier!=10.0||DigestFile(persistence.checkpointPath)!=R221CheckpointSHA||
			persistence.r220ResumeProbe||persistence.r217RefusalDiagnostic||persistence.tier10RetainedContinuation||
			(persistence.projectedHeunCertificateDiagnostic?
				(!persistence.isolatedEquivalenceProbe||persistence.isolatedExpectedCheckpointBuildId!=R220OldBuild):
				(!persistence.sealedProjectedHeunContinuation||persistence.replayProtocolDigest!=R221ProtocolSHA)))){
			values.structuredError="r221_retained_scope_refused";return values;}
		if(persistence.r220ResumeProbe&&(!persistence.projectedHeunEOSDiagnostic||
			!persistence.projectedHeunCertificateDiagnostic||!persistence.isolatedEquivalenceProbe||
			persistence.tier10RetainedContinuation||persistence.r217RefusalDiagnostic||
			DigestFile(persistence.checkpointPath)!=R220CheckpointSHA||
			persistence.isolatedExpectedCheckpointBuildId!=R220OldBuild)){
			values.structuredError="r220_resume_probe_scope_refused";return values;}
		if(persistence.Test().hostMemoryResumeStopRED&&!persistence.r220ResumeProbe){
			values.structuredError="host_memory_resume_stop_red_scope_refused";return values;}
		if(persistence.Test().hostMemoryRetentionRED&&(!persistence.portedProductionTransport||
			!persistence.productionMetal||persistence.resume||persistence.sealedProjectedHeunReplay||
			persistence.sealedLegacyMomentumReplay||persistence.productionOnsetDiagnosticDirectory.empty()||
			persistence.finalCheckpointPath.empty()||resolutionTier!=8.0||caseDurationS!=3.0||
			caseFramesPerS!=1.0||minimumStepCount!=1u||targetTimeS!=
			24.0*static_cast<double>(static_cast<float>(0.0016462659696117043)))){
			values.structuredError="host_memory_retention_red_scope_refused";return values;}
		const char* refusalOption=std::getenv("RISE_FIRE_REFUSAL_CLAUSES");
		if((refusalOption&&!persistence.r217RefusalDiagnostic&&!persistence.r222RefusalDiagnostic&&!persistence.r225RefusalDiagnostic&&!persistence.r241RefusalDiagnostic)||
			(persistence.r217RefusalDiagnostic&&(!refusalOption||std::strcmp(refusalOption,"1")!=0||
			!persistence.projectedHeunCertificateDiagnostic||!persistence.projectedHeunEOSDiagnostic||
			persistence.tier10RetainedContinuation||persistence.sealedProjectedHeunContinuation||
			resolutionTier!=10.0||DigestFile(persistence.checkpointPath)!=R217CheckpointSHA||
			persistence.isolatedExpectedCheckpointBuildId!=R216ProducerBuild))){
			values.structuredError="r217_refusal_diagnostic_scope_conflict";return values;
		}
		if(persistence.r241RefusalDiagnostic&&(!refusalOption||std::strcmp(refusalOption,"1")!=0||
			persistence.resume||!persistence.productionMetal||!persistence.portedProductionTransport||
			resolutionTier!=8.0||caseDurationS!=25.091040127156969||caseFramesPerS!=16.0||targetTimeS!=0.35)){
			values.structuredError="r241_refusal_diagnostic_scope_conflict";return values;
		}
		if(persistence.r222RefusalDiagnostic&&(!refusalOption||std::strcmp(refusalOption,"1")!=0||
			!persistence.projectedHeunCertificateDiagnostic||!persistence.projectedHeunEOSDiagnostic||
			persistence.r217RefusalDiagnostic||persistence.r220ResumeProbe||persistence.r221Retained||
			persistence.tier10RetainedContinuation||persistence.sealedProjectedHeunContinuation||
			resolutionTier!=10.0||DigestFile(persistence.checkpointPath)!=R222CheckpointSHA||
			persistence.isolatedExpectedCheckpointBuildId!=R221ProducerBuild)){
			values.structuredError="r222_refusal_diagnostic_scope_conflict";return values;
		}
		if(persistence.r225RefusalDiagnostic&&(!refusalOption||std::strcmp(refusalOption,"1")!=0||
			!persistence.projectedHeunCertificateDiagnostic||!persistence.projectedHeunEOSDiagnostic||
			persistence.r217RefusalDiagnostic||persistence.r222RefusalDiagnostic||persistence.r220ResumeProbe||
			persistence.r221Retained||persistence.tier10RetainedContinuation||persistence.sealedProjectedHeunContinuation||
			resolutionTier!=8.0||!WindowRefusalDiagnosticInput(DigestFile(persistence.checkpointPath),
			persistence.isolatedExpectedCheckpointBuildId))){
			values.structuredError="r225_refusal_diagnostic_scope_conflict";return values;
		}
		const bool oracleComposition=!persistence.oracleCompositionDirectory.empty();
		if(oracleComposition&&(persistence.productionMetal||persistence.UsesProjectedHeunOwner()||
			!persistence.resume||!persistence.isolatedEquivalenceProbe||
			persistence.stopAfterAdditionalAcceptedSteps!=8u||
			persistence.isolatedExpectedCheckpointDigest!=
				"ab91898e0279a347e85983853e8fe4167113a2364cdcffb6bad78e327d4e03bf"||
			persistence.isolatedExpectedCheckpointBuildId!=
				"35c4f59e4d603eebcad2c37d67860a018b0e13266b6ff7f4807f0c3667d30feb"||
			persistence.checkpointCadenceWallS!=std::numeric_limits<double>::max()||
			persistence.equivalenceSnapshotDirectory!=persistence.oracleCompositionDirectory/"checkpoints"||
			!persistence.finalCheckpointPath.empty()||!persistence.retainedCheckpointDirectory.empty()||
			!persistence.temporalSnapshotDirectory.empty()||persistence.Test().forceZeroSourceForTest||
			persistence.Test().killAfterFirstCheckpoint||resolutionTier!=8.0||caseDurationS!=3.0||
			caseFramesPerS!=1.0||targetTimeS!=2.1096706851385534||
			!(persistence.maximumOracleCompositionStepS>0.0)||
			persistence.maximumOracleCompositionStepS>
				(2.1096706851385534-2.1080244191689417)/8.0)){
			values.structuredError="oracle_composition_diagnostic_scope_conflict";return values;
		}
		if(!oracleComposition&&persistence.maximumOracleCompositionStepS!=0.0){
			values.structuredError="oracle_composition_step_without_scope";return values;
		}
		// M2 track C: the solver adapter's options (rendezvous, absolute window
		// end, general resume) are admitted only on the plain production
		// transport; every historical mode keeps its own scope.
		if((persistence.generalResume||persistence.acceptedStepRendezvous||persistence.restoreValues||
			!persistence.generalResumeCheckpointPath.empty()||persistence.windowEndTimeS!=0.0||
			persistence.finalCheckpointAtWindowEnd)&&
			(!persistence.productionMetal||!persistence.portedProductionTransport||persistence.resume||
			persistence.isolatedEquivalenceProbe||persistence.stopAfterAdditionalAcceptedSteps!=0u||
			persistence.r244LateralDouble||persistence.r245AcceptedBudget||persistence.r246TopDouble||
			persistence.r246FullWindow||persistence.windowS!=0.0||!persistence.checkpointPath.empty()||
			!persistence.resumeEquivalenceCertificatePath.empty()||!persistence.equivalenceSnapshotDirectory.empty()||
			!persistence.oracleCompositionDirectory.empty()||persistence.testHooks||
			!(persistence.windowEndTimeS>=0.0)||!std::isfinite(persistence.windowEndTimeS)||
			(persistence.finalCheckpointAtWindowEnd&&persistence.windowEndTimeS==0.0)||
			persistence.generalResume!=!persistence.generalResumeCheckpointPath.empty()||
			persistence.generalResume!=static_cast<bool>(persistence.restoreValues))){
			values.structuredError="general_resume_scope_conflict";return values;
		}
		if(persistence.generalResume&&!std::filesystem::is_regular_file(persistence.generalResumeCheckpointPath)){
			values.structuredError="checkpoint_resume_failure:general resume checkpoint is missing";return values;
		}
		// A bounded isolated diagnostic may reproduce a retained production state;
		// it grants no empirical continuation or checkpoint migration authority.
		const bool retainedProductionProbe=persistence.portedProductionTransport&&persistence.resume&&
			persistence.isolatedEquivalenceProbe&&targetTimeS==0.0&&persistence.stopAfterAdditionalAcceptedSteps>=8u&&
			persistence.stopAfterAdditionalAcceptedSteps<=persistence.maximumRetainedProbeSteps&&
			persistence.finalCheckpointPath.empty()&&persistence.retainedCheckpointDirectory.empty();
		if(persistence.portedProductionTransport&&persistence.resume&&!retainedProductionProbe){
			values.structuredError="production_transport_resume_requires_operator_lineage";return values;
		}
		if(persistence.portedProductionTransport&&(!persistence.productionMetal||
			persistence.compatibleMomentumDiagnostic||persistence.singleStageFCTDiagnostic||
			persistence.sealedLegacyMomentumReplay||persistence.sealedProjectedHeunReplay||
			persistence.sealedProjectedHeunContinuation||persistence.projectedHeunCostDiagnostic||
			persistence.projectedHeunEOSDiagnostic||(persistence.isolatedEquivalenceProbe&&!retainedProductionProbe)||
			persistence.tier10RetainedContinuation)){
			values.structuredError="production_transport_owner_scope_conflict";return values;
		}
		if(persistence.tier10RetainedContinuation&&!R216RetainedScopeAccepted(
			resolutionTier,persistence.resume,DigestFile(persistence.checkpointPath),
			persistence.sealedProjectedHeunContinuation,persistence.projectedHeunCertificateDiagnostic)){
			values.structuredError="r216_retained_scope_conflict";return values;
		}
		if(persistence.sealedProjectedHeunContinuation&&(!persistence.productionMetal||
			!persistence.sealedProjectedHeunReplay||!persistence.resume||
			persistence.isolatedEquivalenceProbe||persistence.resumeEquivalenceCertificatePath.empty()||
			persistence.sealedLegacyMomentumReplay||persistence.projectedHeunEOSDiagnostic||
			persistence.projectedHeunCostDiagnostic||persistence.compatibleMomentumDiagnostic||
			persistence.singleStageFCTDiagnostic||persistence.stopAfterAdditionalAcceptedSteps!=0u||
			targetTimeS!=3.5||caseDurationS!=3.0||
			resolutionTier!=((persistence.tier10RetainedContinuation||persistence.r221Retained)?10.0:8.0)||
			(persistence.tier10RetainedContinuation&&persistence.replayProtocolDigest!=R216ProtocolSHA)||
			persistence.productionOnsetStopVelocityMPerS!=15.0)){
			values.structuredError="resident_r78_continuation_scope_conflict";return values;
		}
		if(persistence.projectedHeunCertificateDiagnostic&&!persistence.projectedHeunEOSDiagnostic){
			values.structuredError="owner_certificate_diagnostic_scope_conflict";return values;
		}
		if(persistence.projectedHeunEOSDiagnostic&&(!persistence.productionMetal||
			!persistence.resume||!persistence.isolatedEquivalenceProbe||
			persistence.isolatedExpectedCheckpointBuildId.size()!=64u||
			persistence.isolatedExpectedCheckpointDigest.size()!=64u||
			persistence.stopAfterAdditionalAcceptedSteps!=(persistence.r225RefusalDiagnostic?
				(persistence.isolatedExpectedCheckpointDigest==R226CheckpointSHA?38u:20u):
				(persistence.r221Retained?9u:8u))||persistence.checkpointPath.empty()||
			persistence.checkpointCadenceWallS!=std::numeric_limits<double>::max()||
			persistence.Test().killAfterFirstCheckpoint||
			!persistence.finalCheckpointPath.empty()||!persistence.retainedCheckpointDirectory.empty()||
			!OwnerEOSDiagnosticSnapshotDirectoryAccepted(persistence)||!persistence.temporalSnapshotDirectory.empty()||
			persistence.sealedProjectedHeunReplay||persistence.sealedLegacyMomentumReplay||
			persistence.projectedHeunCostDiagnostic||persistence.singleStageFCTDiagnostic||
			persistence.compatibleMomentumDiagnostic||persistence.Test().forceZeroSourceForTest||
			resolutionTier!=((persistence.tier10RetainedContinuation||persistence.r217RefusalDiagnostic||persistence.r222RefusalDiagnostic||persistence.r220ResumeProbe||persistence.r221Retained)?10.0:8.0)||caseDurationS!=(persistence.r225RefusalDiagnostic?R225CaseDurationS:3.0)||caseFramesPerS!=(persistence.r225RefusalDiagnostic?16.0:1.0))){
			values.structuredError="owner_eos_diagnostic_scope_conflict";return values;
		}
		if(persistence.projectedHeunCostDiagnostic&&(!persistence.productionMetal||
			persistence.resume||persistence.sealedProjectedHeunReplay||
			persistence.sealedLegacyMomentumReplay||persistence.singleStageFCTDiagnostic||
			persistence.compatibleMomentumDiagnostic||!persistence.checkpointPath.empty()||
			!persistence.finalCheckpointPath.empty()||!persistence.retainedCheckpointDirectory.empty()||
			!persistence.equivalenceSnapshotDirectory.empty()||!persistence.temporalSnapshotDirectory.empty()||
			persistence.productionOnsetDiagnosticDirectory.empty()||persistence.Test().forceZeroSourceForTest||
			persistence.isolatedEquivalenceProbe||resolutionTier!=8.0||caseDurationS!=3.0||
			caseFramesPerS!=1.0||minimumStepCount!=3u)){
			values.structuredError="owner_cost_diagnostic_scope_conflict";return values;
		}
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		MethaneCellState state; state.temperatureK=300.0;
		for(std::size_t i=0;i<MethaneSpeciesCount;++i)
			state.constituent[i]=fuel.AmbientMassFractions()[i];
		double invW=0.0;
		for(std::size_t i=0;i<MethaneCarbon;++i) {
			const FireThermochemistrySpecies* s=fuel.FindSpecies(fuel.SpeciesOrder()[i].c_str());
			if(s) invW+=state.constituent[i]/s->molecularWeightKGPerKMol;
		}
		const double rho=fuel.ThermodynamicPressurePa()/(8314.46261815324*state.temperatureK*invW);
		for(double& x:state.constituent) x*=rho;
		state.rhoTotalZ=0.0;
		std::string error;
		Check(fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(state),state.temperatureK,
			state.sensibleEnergyJPerM3,&error),"capstone initial methane state closes thermochemistry");
		FireCase::AuthoredV1 authored;
		authored.fuelRecordId=fuel.RecordId(); authored.poolDiameterM=poolDiameterM;
		authored.heatReleaseRateKW=heatReleaseRateKW; authored.envelope={{0.0,1.0}};
		authored.durationS=caseDurationS; authored.quality="dstar";
		authored.numericDStarTier=resolutionTier; authored.seed=1234;
		authored.outputFramesPerS=caseFramesPerS;
		authored.plumeLaw=true;
		if(persistence.r244LateralDouble)authored.domainStudy=persistence.r246TopDouble?
			"r246-lateral-double-top-double-v1":"r244-lateral-double-v1";
		// Only the canonical source authority implements the r270 floor.
		if(!ApplyFloorEmissivityOption(authored)){
			values.structuredError="case_derivation_failure:floor emissivity is invalid";
			return values;
		}
		FireCase::RecordV1 caseRecord;
		const RISECBOR64::Bytes aerosol=AerosolRecord();
		const RISECBOR64::Bytes chem=SyntheticChemRecord();
		std::vector<std::string> caseReferences={RISECBOR64::SHA256Hex(fuel.RecordBytes()),
			RISECBOR64::SHA256Hex(FireSimulationThermochemistryRecord::OpenSubsetV1().RecordBytes()),
			RISECBOR64::SHA256Hex(FireSimulationTransportRecord::OpenV1().RecordBytes()),
			RISECBOR64::SHA256Hex(FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1().RecordBytes()),
			RISECBOR64::SHA256Hex(FireOpticsPreset::PredictiveV1().RecordBytes()),RISECBOR64::SHA256Hex(aerosol),
			RISECBOR64::SHA256Hex(chem)};
		if(persistence.portedProductionTransport)caseReferences.push_back(ProductionBurnerPolicyId());
		const bool caseBuilt=FireCase::BuildMethaneV1(authored,fuel,caseReferences,caseRecord,error);
		Check(caseBuilt,
			"capstone solver consumes the canonical r57 methane case");
		if(!caseBuilt) {
			std::fprintf(stderr,"capstone case derivation rejected: %s\n",error.c_str());
			values.structuredError="case_derivation_failure:"+error;
			return values;
		}
		// Refused diagnostic attempts still publish evidence under the case that
		// supplied their inputs; successful state publication remains separate.
		values.caseRecordId=caseRecord.caseRecordId;
		RISECBOR64::Bytes currentBuildBytes;
		std::string currentBuildId,currentExecutableDigest;
		if((!persistence.checkpointPath.empty()||!persistence.finalCheckpointPath.empty()||
			!persistence.temporalSnapshotDirectory.empty()||persistence.projectedHeunCostDiagnostic)&&
			(!CurrentRendererBuildIdentity(currentBuildBytes,currentBuildId)||
			!CurrentExecutableDigest(currentBuildBytes,currentExecutableDigest,error))){
			values.structuredError="checkpoint_build_identity_failure";return values;
		}
		PeriodicMACShape shape; shape.nx=caseRecord.derived.nx;shape.ny=caseRecord.derived.ny;
		shape.nz=caseRecord.derived.nz;shape.cellWidthM=caseRecord.derived.cellWidthM;
		const std::array<std::size_t,2> centerXIndex={{(shape.nx-1u)/2u,shape.nx/2u}},
			centerYIndex={{(shape.ny-1u)/2u,shape.ny/2u}};
		const std::size_t centerXCount=centerXIndex[0]==centerXIndex[1]?1u:2u,
			centerYCount=centerYIndex[0]==centerYIndex[1]?1u:2u;
		const double centerSampleCount=static_cast<double>(centerXCount*centerYCount);
		values.effectiveRadiativeFraction=caseRecord.derived.effectiveRadiativeFraction;
		const bool reportCapstoneProgress=std::getenv("RISE_FIRE_CAPSTONE_OUTPUT")!=nullptr;
		if(reportCapstoneProgress) std::fprintf(stderr,
			"capstone probe workers=%u target=%.9g grid=%zux%zux%zu dx=%.9g t_ft=%.9g\n",
			workerCount,targetTimeS,shape.nx,shape.ny,shape.nz,shape.cellWidthM,
			caseRecord.derived.flowThroughTimeS);
		const double zeroGradient[3][3]={{0.0,0.0,0.0},{0.0,0.0,0.0},{0.0,0.0,0.0}};
		const double widths[3]={shape.cellWidthM,shape.cellWidthM,shape.cellWidthM};
		CellTransportEvaluation transportEvaluation;
		Check(EvaluateCellTransport(state,zeroGradient,widths,false,fuel,
			FireSimulationTransportRecord::OpenV1(),transportEvaluation,&error),
			"capstone evaluates physical methane transport from the adopted record");
		StableTimeStep selectedStep;
		const double maximumTransport=std::max(transportEvaluation.totalDiffusivityM2PerS,
			transportEvaluation.effectiveViscosityPaS/state.GasDensity());
		const double positiveReducedGravity=std::max(0.0,9.80665*(rho-state.GasDensity())/
			state.GasDensity());
		Check(ComputeStableTimeStep(shape.cellWidthM,0.0,positiveReducedGravity,
			maximumTransport,3,0.0,selectedStep,&error),
			"capstone consumes the production r54 timestep selector");
		MethaneReactionStep reaction; reaction.deltaTimeS=selectedStep.seconds;
		reaction.maximumAcceptedTemperatureK=caseRecord.derived.maximumAcceptedTemperatureK;
		Check(ComputeMixingTimeS(state,transportEvaluation,FireSimulationTransportRecord::OpenV1(),
			shape.cellWidthM,rho,9.80665,false,reaction.mixingTimeS,&error),
			"capstone derives mixing time instead of authoring a closure constant");
		if(persistence.r246TopDouble){
			FireRunHostMemory::Policy preallocationPolicy;std::uint64_t initialFields=0u;
			if(!R246PreallocationPolicy(shape,preallocationPolicy,initialFields)){
				values.structuredError="r246_preallocation_certificate_refused";return values;}
			FireProductionProjectionShape deviceShape;
			deviceShape.nx=shape.nx;deviceShape.ny=shape.ny;deviceShape.nz=shape.nz;
			deviceShape.cellWidthM=static_cast<float>(shape.cellWidthM);
			std::uint64_t deviceCertificate=0u,deviceLimit=0u;
			if(!FireProductionProjectedHeunMetalObserverWorkingSetBytes(deviceShape,deviceCertificate,true,true)||
				!FireProductionProjectedHeunMetalDeviceWorkingSetLimitBytes(deviceLimit,&error)){
				values.structuredError="r246_preallocation_device_unavailable:"+error;return values;}
			// Test-only refusal never changes the queried device limit or real certificate.
			const std::uint64_t admissionLimit=persistence.Test().r246DeviceAdmissionRED?
				deviceCertificate-1u:deviceLimit;
			std::fprintf(stdout,"R246_DEVICE_PREFLIGHT certificate_bytes=%llu device_limit_bytes=%llu admission_limit_bytes=%llu fixture_red=%d before_initial_fields=1\n",
				static_cast<unsigned long long>(deviceCertificate),static_cast<unsigned long long>(deviceLimit),
				static_cast<unsigned long long>(admissionLimit),persistence.Test().r246DeviceAdmissionRED?1:0);
			if(deviceCertificate>admissionLimit){
				values.structuredError="r246_preallocation_device_refused";return values;}
			values.r246PreallocationReserveBytes=preallocationPolicy.reserve;
			auto sample=FireRunHostMemory::Read();
			if(persistence.Test().r246MemoryAdmissionRED){sample.valid=true;sample.rss=preallocationPolicy.ceiling;}
			if(preallocationPolicy.Stop(sample)){
				values.structuredError="r246_preallocation_memory_refused";return values;}
			std::fprintf(stdout,"R246_PREALLOCATION cells=%zu initial_host_fields_bytes=%llu reserve_bytes=%llu ceiling_bytes=%llu before_initial_fields=1\n",
				shape.CellCount(),static_cast<unsigned long long>(initialFields),
				static_cast<unsigned long long>(preallocationPolicy.reserve),
				static_cast<unsigned long long>(preallocationPolicy.ceiling));
			values.r246InitialFieldsAllocated=true;
		}
		std::vector<MethaneCellState> states(shape.CellCount(),state);
		auto canonicalizeProductionBinary32State=[&](MethaneCellState& candidate)->bool{
			ConservativeVector conservative=ToConservativeVector(candidate);
			for(std::size_t component=0u;component<MethaneMassStateDimension;++component)
				conservative[component]=static_cast<double>(static_cast<float>(
					conservative[component]));
			MethaneCellState represented=FromConservativeVector(conservative,
				FireStateProducerPrecision::Binary32);
			represented.temperatureK=candidate.temperatureK;
			if(!fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(represented),
				represented.temperatureK,represented.sensibleEnergyJPerM3,&error))return false;
			represented.sensibleEnergyJPerM3=static_cast<double>(static_cast<float>(
				represented.sensibleEnergyJPerM3));
			candidate=represented;return true;
		};
		if(persistence.productionMetal)for(MethaneCellState& cell:states)
			if(!canonicalizeProductionBinary32State(cell)){
				values.structuredError="production_initial_state_failure:"+error;return values;}
		std::vector<MethaneReactionStep> reactions(shape.CellCount(),reaction);
		std::vector<MethaneSourcePacket> packets;
		RadiationEscapeFactor escape;
		const double cellVolume=shape.cellWidthM*shape.cellWidthM*shape.cellWidthM;
		PeriodicMACField momentum; for(unsigned int axis=0;axis<3;++axis)
			momentum.component[axis].assign(OpenMACFaceCount3D(shape,axis),0.0);
		ConservativeAdvance3DConfig config; config.transport.cellWidthM=shape.cellWidthM;
		config.transport.deltaTimeS=reaction.deltaTimeS; config.transport.ambientTemperatureK=300.0;
		config.transport.adiabaticTemperatureK=caseRecord.derived.maximumAcceptedTemperatureK;
		config.transport.ambientGasDensityKGPerM3=rho;
		config.transport.producerPrecision=persistence.productionMetal?
			FireStateProducerPrecision::Binary32:FireStateProducerPrecision::Binary64;
		Check(config.transport.adiabaticTemperatureK==2300.0&&
			config.transport.adiabaticTemperatureK<
				FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1().TemperatureMaxK(),
			"r74 capstone owner separates the case physical ceiling from the opacity domain");
		config.gravityMPerS2={{0.0,0.0,-9.80665}};
		Check(config.gravityMPerS2[0]==0.0&&config.gravityMPerS2[1]==0.0&&
			config.gravityMPerS2[2]==-9.80665,
			"capstone owning advance receives the pinned vertical gravitational acceleration");
		const double projectionReferenceVelocityMPerS=std::sqrt(9.80665*
			caseRecord.derived.characteristicDiameterM);
		const double projectionReferenceLengthM=std::max({caseRecord.derived.extentXM,
			caseRecord.derived.extentYM,caseRecord.derived.extentZM});
		config.projectionTolerancePerS=1.0e-3*projectionReferenceVelocityMPerS/
			projectionReferenceLengthM;
		config.dns=false; config.workerCount=workerCount;
		config.periodicBoundaries=false;
		config.retainStageDiagnostics=RetainOracleMomentumOperands(persistence.productionMetal,
			oracleComposition,std::getenv("RISE_FIRE_ORACLE_MOMENTUM_AUDIT_PATH")!=nullptr);
		config.injectedTemperatureK=300.0;
		MethaneCellState ambient; ambient.temperatureK=300.0;
		for(std::size_t i=0;i<MethaneSpeciesCount;++i)
			ambient.constituent[i]=fuel.AmbientMassFractions()[i];
		double ambientInvW=0.0;
		for(std::size_t i=0;i<MethaneCarbon;++i) {
			const FireThermochemistrySpecies* species=fuel.FindSpecies(fuel.SpeciesOrder()[i].c_str());
			if(species) ambientInvW+=ambient.constituent[i]/species->molecularWeightKGPerKMol;
		}
		const double ambientRho=fuel.ThermodynamicPressurePa()/(8314.46261815324*
			ambient.temperatureK*ambientInvW);
		for(double& x:ambient.constituent) x*=ambientRho;
		ambient.rhoTotalZ=0.0;
		Check(fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(ambient),
			ambient.temperatureK,ambient.sensibleEnergyJPerM3,&error),
			"capstone ambient state closes thermochemistry");
		MethaneCellState injected; injected.temperatureK=300.0;
		for(std::size_t i=0;i<MethaneSpeciesCount;++i)
			injected.constituent[i]=fuel.InjectedMassFractions()[i];
		double injectedInvW=0.0;
		for(std::size_t i=0;i<MethaneCarbon;++i) {
			const FireThermochemistrySpecies* species=fuel.FindSpecies(fuel.SpeciesOrder()[i].c_str());
			if(species) injectedInvW+=injected.constituent[i]/species->molecularWeightKGPerKMol;
		}
		const double injectedRho=fuel.ThermodynamicPressurePa()/(8314.46261815324*
			injected.temperatureK*injectedInvW);
		for(double& x:injected.constituent) x*=injectedRho;
		injected.rhoTotalZ=injected.TotalDensity();
		Check(fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(injected),
			injected.temperatureK,injected.sensibleEnergyJPerM3,&error),
			"capstone injected methane state closes thermochemistry");
		config.openBoundary.ambientDensityKGPerM3=ambient.GasDensity();
		config.openBoundary.injectedGasDensityKGPerM3=injected.GasDensity();
		config.openBoundary.ambientState=ToConservativeVector(ambient);
		config.openBoundary.injectedState=ToConservativeVector(injected);
		config.openBoundary.fuelMassFluxKGPerM2S=0.0;
		std::vector<double> sourcePattern;
		Check(FireCase::BuildSourcePattern(authored,caseRecord.derived,sourcePattern,error),
			"capstone solver installs the case's exact SplitMix64 source pattern");
		config.openBoundary.bottomFuelMask.resize(shape.nx*shape.ny,false);
		config.openBoundary.bottomFuelMassFluxKGPerM2S.resize(shape.nx*shape.ny,0.0);
		for(std::size_t face=0;face<sourcePattern.size();++face) if(sourcePattern[face]!=0.0) {
			config.openBoundary.bottomFuelMask[face]=true;
			config.openBoundary.bottomFuelMassFluxKGPerM2S[face]=
				caseRecord.derived.nominalFuelFluxKGPerM2S*sourcePattern[face];
		}
		std::vector<std::uint8_t> canonicalPilotMask;
		Check(FireCase::BuildPilotMask(authored,caseRecord.derived,canonicalPilotMask,error),
			"capstone installs the canonical r57 intensive pilot annulus");
		const double scheduleFlowThroughTimeS=FireCase::ScheduleFlowThroughTimeS(caseRecord.derived);
		values.matchedScheduleFlowThroughTimeS=scheduleFlowThroughTimeS;
		const double pilotEndS=caseRecord.derived.pilotDurationMultiplier*scheduleFlowThroughTimeS;
		const double pilotRampEndS=scheduleFlowThroughTimeS/
			caseRecord.derived.pilotRampExponentPerFlowThrough;
		const double pilotCommandMaximumStepS=FireCase::PilotCommandMaximumStepS(
			caseRecord.derived);
		Check(pilotCommandMaximumStepS>0.0,
			"capstone derives the r70 command-ramp step ceiling from the canonical cap");
		values.maximumTemperatureK=state.temperatureK;
		values.statisticsStartS=caseRecord.derived.preRollOrDiscardS;
		values.characteristicDiameterM=caseRecord.derived.characteristicDiameterM;
		std::vector<double> centerlineTemperatureIntegral(shape.nz,0.0),
			centerlineVelocityIntegral(shape.nz,0.0),
			planeHeatReleaseIntegral(shape.nz,0.0);
		double centerlineStatisticsDurationS=0.0;
		config.openBoundary.velocityToleranceMPerS=1.0e-3*projectionReferenceVelocityMPerS;
		config.openBoundary.pressureTolerancePa=ambient.GasDensity()*
			projectionReferenceVelocityMPerS*config.openBoundary.velocityToleranceMPerS;
		values.projectionReferenceLengthM=projectionReferenceLengthM;
		values.projectionTolerancePerS=config.projectionTolerancePerS;
		values.velocityToleranceMPerS=config.openBoundary.velocityToleranceMPerS;
		values.pressureTolerancePa=config.openBoundary.pressureTolerancePa;
		Check(config.openBoundary.velocityToleranceMPerS==
			config.projectionTolerancePerS*projectionReferenceLengthM&&
			config.openBoundary.pressureTolerancePa==ambient.GasDensity()*
				projectionReferenceVelocityMPerS*config.openBoundary.velocityToleranceMPerS,
			"capstone pressure-open deadband derives from the pinned epsilon_abs scene scale");
		ConservativeAdvance3DResult advanced;
		if(persistence.productionMetal)for(unsigned int axis=0u;axis<3u;++axis)
			advanced.velocityMPerS.component[axis].assign(OpenMACFaceCount3D(shape,axis),0.0);
		if(persistence.projectedHeunCostDiagnostic){
			const std::string initialStateDigest=ProductionBeginningFieldSHA256(
				caseRecord.caseRecordId,shape,states,momentum,advanced.velocityMPerS);
			std::ofstream identity(persistence.productionOnsetDiagnosticDirectory.parent_path()/
				"diagnostic_from_zero_identity.v1",std::ios::trunc);
			identity<<std::setprecision(17)<<"schema rise.fire.owner_cost.from_zero_identity.v1\n"
				<<"scope diagnostic_prefix\nfull_verdict unavailable\n"
				<<"operator section_3_7_projected_heun_resident_owner\n"
				<<"resolution_tier "<<resolutionTier<<"\nseed "<<authored.seed<<"\n"
				<<"case_record_id "<<caseRecord.caseRecordId<<"\ncase_duration_s "<<caseDurationS<<"\n"
				<<"initial_time_s 0\ninitial_state_sha256 "<<initialStateDigest<<"\n"
				<<"producer_build_id "<<currentBuildId<<"\n"
				<<"producer_executable_sha256 "<<currentExecutableDigest<<"\n"
				<<"resume_forbidden true\ncheckpoint_publication unavailable\n";
			identity.close();if(initialStateDigest.size()!=64u||!identity){
				values.structuredError="owner_cost_from_zero_identity_failure";return values;}
		}
		if(persistence.sealedLegacyMomentumReplay||persistence.sealedProjectedHeunReplay){
			if((persistence.resume&&!persistence.sealedProjectedHeunContinuation)||persistence.replayProtocolPath.empty()||
				persistence.replayProtocolDigest.size()!=64u||
				DigestFile(persistence.replayProtocolPath)!=persistence.replayProtocolDigest||
				persistence.productionOnsetDiagnosticDirectory.empty()){
				values.structuredError="sealed_legacy_replay_identity_failure";return values;
			}
			const std::string initialStateDigest=ProductionBeginningFieldSHA256(
				caseRecord.caseRecordId,shape,states,momentum,advanced.velocityMPerS);
			std::ofstream identity(persistence.productionOnsetDiagnosticDirectory.parent_path()/
				(persistence.sealedProjectedHeunContinuation?"continuation_identity.v2":"from_zero_identity.v1"),std::ios::trunc);
			identity<<std::setprecision(17)
				<<"schema "<<(persistence.sealedProjectedHeunContinuation?
					(persistence.tier10RetainedContinuation?"rise.fire.production.r216_continuation_identity.v2":
					(persistence.r221Retained?"rise.fire.production.r221_continuation_identity.v2":
					"rise.fire.production.r211_continuation_identity.v2")):"rise.fire.production.r193_from_zero_identity.v1")<<"\n"
				<<"operator "<<(persistence.sealedProjectedHeunReplay?
					"section_3_7_projected_heun_resident_owner":
					"ordinary_independent_dual_momentum_resident_step")<<"\n"
				<<"resolution_tier "<<resolutionTier<<"\nseed "<<authored.seed<<"\n"
				<<"case_record_id "<<caseRecord.caseRecordId<<"\n"
				<<(persistence.sealedProjectedHeunContinuation?"construction_seed_state_sha256 ":"initial_state_sha256 ")<<initialStateDigest<<"\n"
				<<"producer_build_id "<<currentBuildId<<"\n"
				<<"producer_executable_sha256 "<<currentExecutableDigest<<"\n"
				<<"protocol_path "<<persistence.replayProtocolPath.string()<<"\n"
				<<"protocol_sha256 "<<persistence.replayProtocolDigest<<"\n"
				<<"resume_forbidden "<<(persistence.sealedProjectedHeunContinuation?"false":"true")<<"\n";
			if(persistence.sealedProjectedHeunContinuation)identity<<
				"checkpoint_sha256 "<<DigestFile(persistence.checkpointPath)<<"\n"<<
				"migration_certificate_sha256 "<<DigestFile(persistence.resumeEquivalenceCertificatePath)<<"\n";
			identity.close();
			if(initialStateDigest.size()!=64u||!identity){
				values.structuredError="sealed_legacy_replay_identity_publication_failure";
				return values;
			}
		}
		bool advancedOK=minimumStepCount>0u || targetTimeS==0.0;
		double simulationTimeS=0.0,previousStepS=0.0;
		unsigned int acceptedSteps=0u;
		unsigned int effectiveMinimumStepCount=minimumStepCount;
		FireProductionAcceptedManifoldObservation productionManifoldObservation;
		values.checkpointCadenceWallS=persistence.checkpointCadenceWallS;
		values.streamedFrameCount=persistence.streamedFrameCountAtStart;
		values.workerCountHistory.push_back(workerCount);
		// The general resume (M2 track C) loads its own path; every other resume
		// reads checkpointPath as before.
		const std::filesystem::path& resumeCheckpointPath=persistence.generalResume?
			persistence.generalResumeCheckpointPath:persistence.checkpointPath;
		if((persistence.resume||persistence.generalResume)&&!resumeCheckpointPath.empty()&&
			std::filesystem::exists(resumeCheckpointPath)){
			MethaneRunCheckpoint checkpoint;
			if(!LoadMethaneRunCheckpoint(resumeCheckpointPath,checkpoint,error)){
				values.structuredError="checkpoint_resume_failure:"+error;return values;
			}
			const bool sameBuild=checkpoint.producerBuildId==currentBuildId;
			const bool isolatedProbe=!sameBuild&&persistence.isolatedEquivalenceProbe&&
				persistence.isolatedExpectedCheckpointBuildId==checkpoint.producerBuildId&&
				persistence.stopAfterAdditionalAcceptedSteps>=8u;
			const std::string loadedCheckpointDigest=persistence.isolatedEquivalenceProbe?
				DigestFile(persistence.checkpointPath):std::string();
			if(oracleComposition&&loadedCheckpointDigest!=persistence.isolatedExpectedCheckpointDigest){
				values.structuredError="oracle_composition_checkpoint_identity_mismatch";return values;
			}
			const bool exactDiagnosticCaseReplay=persistence.isolatedEquivalenceProbe&&
				(persistence.stopAfterAdditionalAcceptedSteps==8u||retainedProductionProbe)&&
				persistence.isolatedExpectedCheckpointBuildId==checkpoint.producerBuildId&&
				!loadedCheckpointDigest.empty()&&
				persistence.isolatedExpectedCheckpointDigest==loadedCheckpointDigest;
			ResumeEquivalenceCertificate migration;
			bool certifiedMigration=false;
			if(!sameBuild&&!persistence.resumeEquivalenceCertificatePath.empty()&&
				LoadResumeEquivalenceCertificate(persistence.resumeEquivalenceCertificatePath,
					migration,error)){
				const std::string checkpointDigest=DigestFile(persistence.checkpointPath);
				certifiedMigration=!checkpointDigest.empty()&&
					(migration.schemaVersion!=2u||(persistence.productionMetal&&persistence.UsesProjectedHeunOwner()))&&
					(migration.schemaVersion!=2u||migration.beginningTimeBits==DoubleBits(checkpoint.simulationTimeS))&&
					(!persistence.sealedProjectedHeunContinuation||migration.schemaVersion==2u)&&
					migration.checkpointDigest==checkpointDigest&&
					migration.oldBuildId==checkpoint.producerBuildId&&
					migration.newBuildId==currentBuildId&&
					migration.newExecutableDigest==currentExecutableDigest&&
					migration.resumedFromStep==checkpoint.acceptedSteps;
			}
			const bool currentActiveSetCheckpoint=checkpoint.checkpointFormatVersion>=7u&&
				checkpoint.values.activeSetAlgorithmVersion==CurrentActiveSetAlgorithmVersion();
			const bool legacyActiveSetCheckpoint=checkpoint.checkpointFormatVersion<7u&&
				checkpoint.values.activeSetAlgorithmVersion==LegacyActiveSetAlgorithmVersion();
			const bool priorActiveSetHistoryValid=checkpoint.checkpointFormatVersion<8u?
				checkpoint.values.priorActiveSetAlgorithmVersion.empty():
				(checkpoint.values.priorActiveSetAlgorithmVersion.empty()||
					checkpoint.values.priorActiveSetAlgorithmVersion==
						LegacyActiveSetAlgorithmVersion());
			FireStateProducerPrecision checkpointPrecision=FireStateProducerPrecision::Unknown;
			if((checkpoint.caseRecordId!=caseRecord.caseRecordId&&!exactDiagnosticCaseReplay)||
				(!sameBuild&&!isolatedProbe&&!certifiedMigration)||
				checkpoint.values.reductionMode!="fixed_order_tree_v1"||
				(!currentActiveSetCheckpoint&&!legacyActiveSetCheckpoint)||
				!priorActiveSetHistoryValid||
				checkpoint.dimensions!=std::array<std::size_t,3>{{shape.nx,shape.ny,shape.nz}}||
				checkpoint.cellWidthM!=shape.cellWidthM||checkpoint.states.size()!=shape.CellCount()||
				checkpoint.acceptedSteps>std::numeric_limits<unsigned int>::max()||
				checkpoint.centerlineTemperatureIntegral.size()!=shape.nz||
				checkpoint.centerlineVelocityIntegral.size()!=shape.nz||
				checkpoint.planeHeatReleaseIntegral.size()!=shape.nz||
				!HomogeneousStateProducerPrecision(checkpoint.states,checkpointPrecision)){
				if(persistence.isolatedEquivalenceProbe)std::fprintf(stderr,
					"ISOLATED_CHECKPOINT_ADMISSION same=%d isolated=%d exact_case=%d "
					"case=%d reduction=%d active_current=%d "
					"active_legacy=%d prior=%d shape=%d width=%d states=%d temperature=%d "
					"velocity=%d heat=%d precision=%d\n",
					sameBuild?1:0,isolatedProbe?1:0,exactDiagnosticCaseReplay?1:0,
					checkpoint.caseRecordId==caseRecord.caseRecordId?1:0,
					checkpoint.values.reductionMode=="fixed_order_tree_v1"?1:0,
					currentActiveSetCheckpoint?1:0,legacyActiveSetCheckpoint?1:0,
					priorActiveSetHistoryValid?1:0,
					checkpoint.dimensions==std::array<std::size_t,3>{{shape.nx,shape.ny,shape.nz}}?1:0,
					checkpoint.cellWidthM==shape.cellWidthM?1:0,
					checkpoint.states.size()==shape.CellCount()?1:0,
					checkpoint.centerlineTemperatureIntegral.size()==shape.nz?1:0,
					checkpoint.centerlineVelocityIntegral.size()==shape.nz?1:0,
					checkpoint.planeHeatReleaseIntegral.size()==shape.nz?1:0,
					HomogeneousStateProducerPrecision(checkpoint.states,checkpointPrecision)?1:0);
				if(error.empty())error="checkpoint build migration is not certified";
				values.structuredError="checkpoint_resume_failure:"+error;return values;
			}
			for(unsigned int axis=0;axis<3;++axis)if(
				checkpoint.momentum.component[axis].size()!=OpenMACFaceCount3D(shape,axis)||
				checkpoint.velocity.component[axis].size()!=OpenMACFaceCount3D(shape,axis)){
				values.structuredError="checkpoint_resume_failure:checkpoint MAC shape mismatch";
				return values;
			}
			states=std::move(checkpoint.states);momentum=std::move(checkpoint.momentum);
			productionManifoldObservation=checkpoint.productionManifoldObservation;
			config.transport.producerPrecision=checkpointPrecision;
			advanced.velocityMPerS=std::move(checkpoint.velocity);
			values=std::move(checkpoint.values);
			// A retained success flag describes the old run, never this attempt.
			values.succeeded=false;
			// Historical retained checkpoints may have no nested frame case ID.
			// Reattach this admitted case even when the next step refuses.
			values.caseRecordId=caseRecord.caseRecordId;
			if(legacyActiveSetCheckpoint)values.priorActiveSetAlgorithmVersion=
				LegacyActiveSetAlgorithmVersion();
			values.activeSetAlgorithmVersion=CurrentActiveSetAlgorithmVersion();
			// The general resume continues the run's lineage: a worker count is
			// recorded again only when it changes.
			if(!persistence.generalResume||values.workerCountHistory.empty()||
				values.workerCountHistory.back()!=workerCount)values.workerCountHistory.push_back(workerCount);
			centerlineTemperatureIntegral=std::move(checkpoint.centerlineTemperatureIntegral);
			centerlineVelocityIntegral=std::move(checkpoint.centerlineVelocityIntegral);
			planeHeatReleaseIntegral=std::move(checkpoint.planeHeatReleaseIntegral);
			centerlineStatisticsDurationS=checkpoint.centerlineStatisticsDurationS;
			simulationTimeS=checkpoint.simulationTimeS;previousStepS=checkpoint.previousStepS;
			reaction.deltaTimeS=checkpoint.lastAcceptedStepS;
			acceptedSteps=static_cast<unsigned int>(checkpoint.acceptedSteps);
			if(persistence.stopAfterAdditionalAcceptedSteps>0u){
				const std::uint64_t requested=checkpoint.acceptedSteps+
					persistence.stopAfterAdditionalAcceptedSteps;
				if(requested>std::numeric_limits<unsigned int>::max()){
					values.structuredError="checkpoint_resume_failure:step target overflow";return values;
				}
				effectiveMinimumStepCount=static_cast<unsigned int>(requested);
			}
			if(!persistence.generalResume){values.resumedFromCheckpoint=true;values.resumedFromStep=acceptedSteps;}
			if(certifiedMigration){
				values.migrationCertificateId=migration.certificateId;
				values.migrationOldBuildId=migration.oldBuildId;
				values.migrationNewBuildId=migration.newBuildId;
				values.migrationAcceptedStepCount=migration.acceptedStepCount;
				values.migrationResumedFromStep=migration.resumedFromStep;
			}
			values.checkpointCadenceWallS=persistence.checkpointCadenceWallS;
			if(persistence.generalResume&&!persistence.restoreValues(values,error)){
				values.structuredError="checkpoint_resume_failure:"+error;return values;
			}
			if(reportCapstoneProgress)std::fprintf(stderr,
				"capstone resumed checkpoint step=%u time=%.17g path=%s\n",acceptedSteps,
				simulationTimeS,resumeCheckpointPath.string().c_str());
		}
		auto lastCheckpointWall=std::chrono::steady_clock::now();
		const std::array<double,3> productionOnsetVelocityThresholds={{15.0,30.0,60.0}};
		std::array<bool,3> productionOnsetThresholdCaptured={{false,false,false}};
		bool productionMomentumObservationCaptured=false;
		bool productionOnsetStopReached=false;
		std::array<std::vector<unsigned char>,6> singleStageFCTAcceptedInflow;
		for(unsigned int side=0u;side<6u;++side){const std::size_t count=side<2u?
			shape.ny*shape.nz:(side<4u?shape.nx*shape.nz:shape.nx*shape.ny);
			singleStageFCTAcceptedInflow[side].assign(count,0u);}
		if(!persistence.productionOnsetDiagnosticDirectory.empty()){
			std::error_code onsetDirectoryError;
			std::filesystem::create_directories(
				persistence.productionOnsetDiagnosticDirectory,onsetDirectoryError);
			if(onsetDirectoryError){values.structuredError="production_onset_directory_failure";
				return values;}
			// A general resume appends to the logs of the run it continues, and an
			// onset threshold whose evidence the run already published stays captured.
			const bool continueOnsetLogs=persistence.generalResume&&std::filesystem::exists(
				persistence.productionOnsetDiagnosticDirectory/"maximum_velocity_trajectory.csv");
			if(persistence.generalResume)for(std::size_t threshold=0u;threshold<productionOnsetVelocityThresholds.size();
				++threshold){std::ostringstream name;name<<"threshold_"<<static_cast<unsigned int>(
					productionOnsetVelocityThresholds[threshold])<<".raw.csv";
				productionOnsetThresholdCaptured[threshold]=std::filesystem::exists(
					persistence.productionOnsetDiagnosticDirectory/name.str());}
			if(!continueOnsetLogs){
			std::ofstream trajectory(persistence.productionOnsetDiagnosticDirectory/
				"maximum_velocity_trajectory.csv",std::ios::trunc);
			trajectory<<FireProductionOnsetEvidence::TrajectoryHeader()<<'\n';
			if(!ClosePublishedStream(trajectory)){values.structuredError="production_onset_trajectory_failure";
				return values;}
			std::ofstream retries(persistence.productionOnsetDiagnosticDirectory/
				"retry_attempt_trajectory.csv",std::ios::trunc);
			retries<<"beginning_time_s,candidate,represented_dt_s,maximum_deviation,p95,tail_cells,"
				"tail_drained_m3,dynamics_passed,physical_valid,terminal_valid,token,next_available,"
				"suggested_dt_s,disposition,error\n";
			if(!ClosePublishedStream(retries)){values.structuredError="production_onset_retry_trajectory_failure";
				return values;}
			}
		}
		FireProductionAcceptedStageBudget::Ledger r245Ledger;
		std::uint64_t temporalSnapshotIndex=values.streamedFrameCount;
		double nextTemporalSnapshotS=persistence.hasTemporalFrameOrigin?
			persistence.temporalFrameOriginS:values.statisticsStartS;
		// The general resume continues the frame clock exactly as the loop
		// advances it after each frame (origin + index * cadence).
		if(persistence.generalResume&&persistence.temporalSnapshotCadenceS>0.0)
			nextTemporalSnapshotS=nextTemporalSnapshotS+static_cast<double>(temporalSnapshotIndex)*
				persistence.temporalSnapshotCadenceS;
		else if(persistence.temporalSnapshotCadenceS>0.0&&simulationTimeS>=nextTemporalSnapshotS)
			nextTemporalSnapshotS+=persistence.temporalSnapshotCadenceS*(1.0+
				std::floor((simulationTimeS-nextTemporalSnapshotS)/
					persistence.temporalSnapshotCadenceS));
		auto captureTemporalFrame=[&](){
			values.dimensions={{shape.nx,shape.ny,shape.nz}};values.cellWidthM=shape.cellWidthM;
			values.caseRecordId=caseRecord.caseRecordId;values.temperature.resize(shape.CellCount());
			values.reaction.resize(shape.CellCount());values.carbon.resize(shape.CellCount());
			values.velocity.resize(shape.CellCount());
			for(std::size_t cell=0u;cell<shape.CellCount();++cell){
				values.temperature[cell]=static_cast<float>(states[cell].temperatureK);
				values.reaction[cell]=static_cast<float>(packets.empty()?0.0:
					packets[cell].gasHeatReleaseWPerM3);
				values.carbon[cell]=static_cast<float>(std::max(0.0,
					states[cell].constituent[MethaneCarbon]));
				for(unsigned int axis=0u;axis<3u;++axis){
					const std::size_t lower=OpenLowerFaceForCell3D(shape,cell,axis);
					const std::size_t upper=OpenUpperFaceForCell3D(shape,cell,axis);
					values.velocity[cell][axis]=static_cast<float>(0.5*(
						advanced.velocityMPerS.component[axis][lower]+
						advanced.velocityMPerS.component[axis][upper]));
				}
			}
		};
		auto writeTemporalFrame=[&]()->bool{
			captureTemporalFrame();
			std::ostringstream frameName;frameName<<"frame_"<<std::setw(5)<<
				std::setfill('0')<<temporalSnapshotIndex;
			const std::filesystem::path framePath=persistence.temporalSnapshotDirectory/
				(frameName.str()+".vdb");
			if(!WriteProductionTemporalFrame(framePath,values)){
				error="production temporal VDB publication failed";return false;
			}
			const std::string digest=DigestFile(framePath);
			std::ofstream timeRecord(persistence.temporalSnapshotDirectory/
				(frameName.str()+".time"),std::ios::trunc);
			timeRecord<<std::setprecision(17)<<simulationTimeS<<'\n'<<digest<<'\n';
			timeRecord.close();
			if(digest.empty()||!timeRecord){error="production temporal frame record failed";
				return false;}
			const std::uint64_t publishedFrameIndex=temporalSnapshotIndex;
			++temporalSnapshotIndex;++values.streamedFrameCount;
			if(persistence.temporalFrameObserver&&!persistence.temporalFrameObserver(values,publishedFrameIndex))
				std::fprintf(stderr,"temporal frame observer failed for frame %llu (ignored)\n",
					static_cast<unsigned long long>(publishedFrameIndex));
			return true;
		};
		// M2 track C: the accepted state as the checkpoint block below assembles
		// it, serialized in memory without recording a checkpoint step.  Called
		// only while the driver thread is blocked in the rendezvous; the state
		// is moved out and back exactly as that block does.
		const std::function<bool(RISECBOR64::Bytes&,std::string&)> serializeAcceptedState=
			[&](RISECBOR64::Bytes& bytes,std::string& serializeError)->bool{
			MethaneRunCheckpoint checkpoint;
			checkpoint.caseRecordId=caseRecord.caseRecordId;
			checkpoint.producerBuildId=currentBuildId;
			checkpoint.dimensions={{shape.nx,shape.ny,shape.nz}};
			checkpoint.cellWidthM=shape.cellWidthM;checkpoint.states=std::move(states);
			checkpoint.momentum=std::move(momentum);
			checkpoint.velocity=std::move(advanced.velocityMPerS);
			checkpoint.values=std::move(values);
			checkpoint.centerlineTemperatureIntegral=std::move(centerlineTemperatureIntegral);
			checkpoint.centerlineVelocityIntegral=std::move(centerlineVelocityIntegral);
			checkpoint.planeHeatReleaseIntegral=std::move(planeHeatReleaseIntegral);
			checkpoint.centerlineStatisticsDurationS=centerlineStatisticsDurationS;
			checkpoint.simulationTimeS=simulationTimeS;
			checkpoint.previousStepS=previousStepS;
			checkpoint.lastAcceptedStepS=reaction.deltaTimeS;
			checkpoint.acceptedSteps=acceptedSteps;
			checkpoint.productionManifoldObservation=productionManifoldObservation;
			bool serialized=false;
			try{
				FireStateProducerPrecision precision=FireStateProducerPrecision::Unknown;
				const bool authorized=HomogeneousStateProducerPrecision(checkpoint.states,precision)&&
					(precision!=FireStateProducerPrecision::Binary64||IssueBinary64CheckpointAuthority(checkpoint));
				if(!authorized)serializeError="checkpoint state is not authorized for publication";
				serialized=authorized&&SerializeMethaneRunCheckpoint(checkpoint,bytes,serializeError);
			}catch(const std::exception& exception){serializeError=exception.what();serialized=false;}
			states=std::move(checkpoint.states);momentum=std::move(checkpoint.momentum);
			advanced.velocityMPerS=std::move(checkpoint.velocity);
			values=std::move(checkpoint.values);
			centerlineTemperatureIntegral=std::move(checkpoint.centerlineTemperatureIntegral);
			centerlineVelocityIntegral=std::move(checkpoint.centerlineVelocityIntegral);
			planeHeatReleaseIntegral=std::move(checkpoint.planeHeatReleaseIntegral);
			return serialized;
		};
		if(FireProfileEnabled())FireProfileReportAndReset("preloop");
		const bool hostMemoryMonitored=persistence.UsesProjectedHeunOwner();
		FireRunHostMemory::Policy hostMemoryPolicy;
		std::vector<std::vector<unsigned char> > syntheticRetainedPayloads;
		std::filesystem::path hostMemoryPath;
		if(hostMemoryMonitored){
			std::uint64_t ownerBytes=0u;
			if(!ProductionOwnerObservationReserveBytes(shape,
				persistence.r245AcceptedBudget||persistence.Test().r245ObservationFixture,ownerBytes)){
				values.structuredError="host_memory_certificate_unavailable";return values;}
			hostMemoryPolicy=FireRunHostMemory::ProductionPolicy(ownerBytes);
			const auto memory=FireRunHostMemory::Read();
			if(hostMemoryPolicy.Stop(memory)){
				values.structuredError="host_memory_preflight_refused";return values;}
			// A general resume continues the run's host-memory log and keeps its
			// admission record (the continued run was admitted at its start).
			if(!persistence.productionOnsetDiagnosticDirectory.empty()&&persistence.generalResume&&
				std::filesystem::exists(persistence.productionOnsetDiagnosticDirectory/"host_memory_trajectory.csv")&&
				std::filesystem::exists(persistence.productionOnsetDiagnosticDirectory.parent_path()/"host_memory_admission.v1"))
				hostMemoryPath=persistence.productionOnsetDiagnosticDirectory/"host_memory_trajectory.csv";
			else if(!persistence.productionOnsetDiagnosticDirectory.empty()){
				hostMemoryPath=persistence.productionOnsetDiagnosticDirectory/"host_memory_trajectory.csv";
				std::ofstream output(hostMemoryPath);
				output<<"accepted_step,time_s,process_rss_bytes,physical_footprint_bytes,compressed_bytes,host_ceiling_bytes,next_step_reserve_bytes,stop\n";
				output<<std::setprecision(17)<<acceptedSteps<<','<<simulationTimeS<<','<<memory.rss<<','
					<<memory.footprint<<','<<memory.compressed<<','<<hostMemoryPolicy.ceiling<<','
					<<hostMemoryPolicy.reserve<<",0\n";
				if(!ClosePublishedStream(output)){values.structuredError="host_memory_evidence_failure";return values;}
				const auto admissionPath=persistence.productionOnsetDiagnosticDirectory.parent_path()/"host_memory_admission.v1";
				if(std::filesystem::exists(admissionPath)||std::filesystem::is_symlink(admissionPath)){
					values.structuredError="host_memory_admission_destination_occupied";return values;}
				std::ofstream admission(admissionPath);
				admission<<std::setprecision(17)<<"schema rise.fire.host-memory-admission.v1\n"
					<<"case_record_id "<<caseRecord.caseRecordId<<"\nproducer_build_id "<<currentBuildId<<'\n'
					<<"producer_executable_sha256 "<<currentExecutableDigest<<'\n'
					<<"beginning_step "<<acceptedSteps<<"\nbeginning_time_s "<<simulationTimeS<<'\n'
					<<"rss_ceiling_bytes "<<hostMemoryPolicy.ceiling<<"\nfootprint_ceiling_bytes "<<hostMemoryPolicy.ceiling<<'\n'
					<<"next_step_reserve_bytes "<<hostMemoryPolicy.reserve<<'\n'
					<<"stop_rule max_rss_footprint_ge_ceiling_minus_reserve\n"
					<<"stop_exit_code "<<FireRunHostMemory::GracefulStopExitCode<<'\n'
					<<"exclusive_host_rule no_full_builds_no_opto_lto_no_exhaustive_qualification_no_oracle_regeneration\n"
					<<"enforcement accepted_boundary_monitor_and_operator_launch_serialization\n"
					<<"synthetic_retention_fixture "<<(persistence.Test().hostMemoryRetentionRED?"true":"false")<<'\n'
					<<"forced_stop_fixture "<<(persistence.Test().hostMemoryResumeStopRED?"true":"false")<<'\n';
				if(!ClosePublishedStream(admission)||
					!PublishPayloadMerkleSidecar(admissionPath,caseRecord.caseRecordId,error)){
					values.structuredError="host_memory_admission_publication_failed";return values;}
			}
		}
		const char* solverPhase="initialization";
		// M2: optional bounded window and cooperative stop (both off by default).
		const double windowEndTimeS=persistence.windowEndTimeS>0.0?persistence.windowEndTimeS:
			(persistence.windowS>0.0?simulationTimeS+persistence.windowS:
			std::numeric_limits<double>::infinity());
		bool cooperativeStopObserved=false;
		while(advancedOK&&!productionOnsetStopReached&&
			(!persistence.projectedHeunCostDiagnostic||acceptedSteps<3u)&&
			(acceptedSteps<effectiveMinimumStepCount||simulationTimeS<targetTimeS)&&
			simulationTimeS<windowEndTimeS&&!cooperativeStopObserved) {
			solverPhase="ignition eligibility";
			const auto profileStepStart=std::chrono::steady_clock::now();
			double profileEligibilityMS=0.0,profileTransportMS=0.0,
				profileControlMS=0.0,profileSourceMS=0.0,profileTargetMS=0.0,
				profileLayoutMS=0.0,profileResidentMS=0.0,profilePostMS=0.0,
				profileStatisticsMS=0.0;
			auto profileStageStart=profileStepStart;
			if(acceptedSteps>=65536u) { advancedOK=false;error="capstone exceeded its deterministic step cap";break; }
			IgnitionGrid eligibilityGrid;
			eligibilityGrid.nx=shape.nx;eligibilityGrid.ny=shape.ny;eligibilityGrid.nz=shape.nz;
			eligibilityGrid.cells=states;eligibilityGrid.pilotMask.resize(shape.CellCount(),false);
			for(std::size_t cell=0;cell<shape.CellCount();++cell){
				eligibilityGrid.pilotMask[cell]=!persistence.Test().forceZeroSourceForTest&&
					ProductionPilotCommandCell(cell,shape.nx,shape.ny,canonicalPilotMask,
						sourcePattern);
			}
			std::vector<bool> eligibility;
			advancedOK=BuildIgnitionEligibility(eligibilityGrid,fuel,fuel,
				FireSimulationTransportRecord::OpenV1(),eligibility,&error);
			profileEligibilityMS=std::chrono::duration<double,std::milli>(
				std::chrono::steady_clock::now()-profileStageStart).count();
			if(!advancedOK) break;
			solverPhase="transport evaluation";
			profileStageStart=std::chrono::steady_clock::now();
			double maximumSpeed=0.0;
			for(unsigned int axis=0;axis<3;++axis) for(const double velocity:
				(advanced.velocityMPerS.component[axis].empty()?momentum.component[axis]:
					advanced.velocityMPerS.component[axis])) maximumSpeed=std::max(maximumSpeed,
					std::fabs(velocity));
			std::vector<ConservativeVector> currentConservative(shape.CellCount());
			std::vector<double> currentTemperature(shape.CellCount());
			ParallelFireSlices(shape.CellCount(),workerCount,[&](const std::size_t cell){
				currentConservative[cell]=ToConservativeVector(states[cell]);
				currentTemperature[cell]=states[cell].temperatureK;
			});
			OpenMACField3D currentVelocity;
			for(unsigned int axis=0;axis<3;++axis) currentVelocity.component[axis]=
				advanced.velocityMPerS.component[axis].empty()?momentum.component[axis]:
				advanced.velocityMPerS.component[axis];
			std::vector<CellTransportEvaluation> cellTransportEvaluations;
			advancedOK=BuildOpenStageTransportEvaluations3D(shape,currentConservative,
				currentTemperature,currentVelocity,config.openBoundary,config.dns,fuel,
				FireSimulationTransportRecord::OpenV1(),config.transport.producerPrecision,
				cellTransportEvaluations,&error,workerCount,0,
				// r271: mixing time and step control see the same nu_t as the Metal transport.
				persistence.productionMetal&&FireProductionForceMetalCompactViscousStencil());
			if(!advancedOK) break;
			solverPhase="timestep control";
			profileTransportMS=std::chrono::duration<double,std::milli>(
				std::chrono::steady_clock::now()-profileStageStart).count();
			profileStageStart=std::chrono::steady_clock::now();
			double maximumReducedGravity=0.0,maximumActiveDiffusivity=0.0;
			const unsigned int controlWorkers=std::max(1u,std::min(workerCount,
				static_cast<unsigned int>(shape.CellCount())));
			std::vector<double> workerGravity(controlWorkers,0.0),
				workerDiffusivity(controlWorkers,0.0);
			const std::size_t noControlFailure=std::numeric_limits<std::size_t>::max();
			std::vector<std::size_t> controlFailureCell(controlWorkers,noControlFailure);
			std::vector<std::string> controlFailureMessage(controlWorkers);
			auto evaluateControlRange=[&](const unsigned int worker){
				const std::size_t first=shape.CellCount()*worker/controlWorkers;
				const std::size_t last=shape.CellCount()*(worker+1u)/controlWorkers;
				for(std::size_t cell=first;cell<last;++cell){
					const CellTransportEvaluation& cellTransport=cellTransportEvaluations[cell];
					std::string cellError;
					if(!ComputeMixingTimeS(states[cell],cellTransport,
						FireSimulationTransportRecord::OpenV1(),shape.cellWidthM,
						ambient.GasDensity(),9.80665,false,reactions[cell].mixingTimeS,&cellError)){
						controlFailureCell[worker]=cell;
						controlFailureMessage[worker]=cellError;break;
					}
					const double gasDensity=states[cell].GasDensity();
					workerDiffusivity[worker]=std::max({workerDiffusivity[worker],
						cellTransport.totalDiffusivityM2PerS,
						cellTransport.effectiveViscosityPaS/gasDensity,
						cellTransport.effectiveConductivityWPerMK/
							(gasDensity*cellTransport.gasCpJPerKGK)});
					workerGravity[worker]=std::max(workerGravity[worker],
						std::max(0.0,9.80665*(ambient.GasDensity()-gasDensity)/gasDensity));
				}
			};
			if(controlWorkers==1u)evaluateControlRange(0u);
			else FireWorkerPool().Run(controlWorkers,evaluateControlRange);
			std::size_t firstControlFailure=noControlFailure;unsigned int failedControlWorker=0u;
			for(unsigned int worker=0u;worker<controlWorkers;++worker){
				maximumReducedGravity=std::max(maximumReducedGravity,workerGravity[worker]);
				maximumActiveDiffusivity=std::max(maximumActiveDiffusivity,workerDiffusivity[worker]);
				if(controlFailureCell[worker]<firstControlFailure){
					firstControlFailure=controlFailureCell[worker];failedControlWorker=worker;
				}
			}
			if(firstControlFailure!=noControlFailure){advancedOK=false;
				error=controlFailureMessage[failedControlWorker];}
			if(!advancedOK) break;
			// This owner is the binary64 oracle trajectory. Resident production owns
			// and persists its separate (dt,G,r) selector in the r118/r143 path.
			const double chosenStep=FireCase::SelectTimeStepS(shape.cellWidthM,maximumSpeed,
				maximumReducedGravity,maximumActiveDiffusivity,previousStepS);
			if(reportCapstoneProgress && acceptedSteps<4u) std::fprintf(stderr,
				"capstone step-start=%u speed=%.9g gprime=%.9g nu=%.9g selected=%.9g previous=%.9g\n",
				acceptedSteps,maximumSpeed,maximumReducedGravity,maximumActiveDiffusivity,
				chosenStep,previousStepS);
			if(!(chosenStep>0.0)) {advancedOK=false;error="capstone pinned timestep is unbounded";break;}
			double eventStep=chosenStep;
			if(persistence.productionMetal&&persistence.maximumProductionSourceStepS>0.0)
				eventStep=std::min(eventStep,
					FireProductionSourceStepCapS(persistence.maximumProductionSourceStepS));
			if(oracleComposition)eventStep=std::min(eventStep,persistence.maximumOracleCompositionStepS);
			if(simulationTimeS<pilotRampEndS){
				eventStep=std::min(eventStep,pilotCommandMaximumStepS);
				if(persistence.productionMetal)
					LimitBinary32ProductionStepToEvent(simulationTimeS,pilotRampEndS,eventStep);
				else if(pilotRampEndS-simulationTimeS<eventStep)
					eventStep=pilotRampEndS-simulationTimeS;
			}
			auto limitToEvent=[&](const double event){
				if(persistence.productionMetal)
					LimitBinary32ProductionStepToEvent(simulationTimeS,event,eventStep);
				else if(simulationTimeS<event&&event-simulationTimeS<eventStep)
					eventStep=event-simulationTimeS;
			};
			limitToEvent(pilotEndS);
			limitToEvent(values.statisticsStartS);
			if((!persistence.temporalSnapshotDirectory.empty()||
				(persistence.r225RefusalDiagnostic&&
				 persistence.isolatedExpectedCheckpointDigest==R226CheckpointSHA))&&
				persistence.temporalSnapshotCadenceS>0.0)
				limitToEvent(nextTemporalSnapshotS);
			limitToEvent(targetTimeS);
			if(reportCapstoneProgress&&acceptedSteps<4u)std::fprintf(stderr,
				"capstone event-step=%0.9g pilot_ceiling=%0.9g ramp_end=%0.9g\n",
				eventStep,pilotCommandMaximumStepS,pilotRampEndS);
			ParallelFireSlices(shape.CellCount(),workerCount,[&](const std::size_t cell){
				reactions[cell].primaryEligible=eligibility[cell];
				reactions[cell].sootOxidationEnabled=true;
			});
			std::vector<ConservativeVector> beginning=currentConservative;
			advancedOK=false;
			std::string lastAdvanceError;
			R245PendingBudget pendingBudget;
			double trialStep=eventStep;
			const FireStateProducerPrecision advanceOutputPrecision=
				persistence.productionMetal?FireStateProducerPrecision::Binary32:
					FireStateProducerPrecision::Binary64;
			std::vector<double> pilotSetpointTemperatureK(shape.CellCount(),0.0);
			std::vector<MethaneCellState> acceptedBeginningStates=states;
			std::vector<double> acceptedProductionTemperatureK;
			double acceptedExternalFuelMassKG=0.0;
			bool fatalOnsetAuditFailure=false;
			bool mandatoryEvidenceFailure=false,observationEvidencePublished=false;
			if(persistence.Test().burnerLauncherFixture)values.burnerFixtureQ0SHA=ProductionBeginningFieldSHA256(
				caseRecord.caseRecordId,shape,states,momentum,advanced.velocityMPerS);
			solverPhase="resident step attempt";
			for(unsigned int reduction=0;
				reduction<RISE::FireStepRejectionRetryCap&&!advancedOK;++reduction) {
				solverPhase="step setup";
				pendingBudget=R245PendingBudget();
				if(injectSolverFailure){lastAdvanceError="injected_solver_failure";break;}
				if(persistence.productionMetal)
					trialStep=static_cast<double>(static_cast<float>(trialStep));
				if(!std::isfinite(trialStep)||trialStep<=0.0){
					lastAdvanceError="production represented timestep is invalid";break;}
				bool commandOK=true;
				for(std::size_t cell=0;cell<shape.CellCount();++cell) {
					reactions[cell].primaryEligible=eligibility[cell];
					commandOK=commandOK&&(persistence.Test().forceZeroSourceForTest||
						FireCase::EvaluatePilotSetpointTemperatureK(caseRecord.derived,
							ProductionPilotCommandCell(cell,shape.nx,shape.ny,canonicalPilotMask,
								sourcePattern),simulationTimeS,
							simulationTimeS+trialStep,pilotSetpointTemperatureK[cell],error));
					reactions[cell].deltaTimeS=trialStep;
					reactions[cell].pilotSetpointTemperatureK=pilotSetpointTemperatureK[cell];
					reactions[cell].pilotExpansionVolumeRatioCap=
						pilotSetpointTemperatureK[cell]>0.0?
						caseRecord.derived.pilotExpansionVolumeRatioCap:0.0;
				}
				if(!commandOK){lastAdvanceError=error;break;}
				config.transport.deltaTimeS=trialStep;
				std::vector<MethaneCellState> packetBeginning=states;
				PeriodicMACField productionMomentum=momentum;
				PeriodicMACField productionVelocity=advanced.velocityMPerS;
				std::vector<unsigned char> invalidReusableTransport(shape.CellCount(),0u);
				double externalFuelMassKG=0.0;
				solverPhase="production source staging";
				if(persistence.productionMetal&&!persistence.portedProductionTransport&&
					!persistence.Test().forceZeroSourceForTest){
					const double injectedGasDensity=injected.GasDensity();
					const double injectedSpecificEnergy=injected.sensibleEnergyJPerM3/
						injected.TotalDensity();
					std::vector<ConservativeVector> staged(packetBeginning.size());
					ParallelFireSlices(packetBeginning.size(),workerCount,[&](const std::size_t cell){
						staged[cell]=ToConservativeVector(packetBeginning[cell]);
					});
					for(std::size_t y=0u;y<shape.ny;++y)for(std::size_t x=0u;x<shape.nx;++x){
						const std::size_t sourceFace=y*shape.nx+x;
						if(sourceFace>=sourcePattern.size()||sourcePattern[sourceFace]==0.0)continue;
						const double massFlux=caseRecord.derived.nominalFuelFluxKGPerM2S*
							sourcePattern[sourceFace];
						const double densityDelta=massFlux*trialStep/shape.cellWidthM;
						const std::size_t cell=x+shape.nx*y;
						invalidReusableTransport[cell]=1u;
						staged[cell][0]+=densityDelta;
						for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
							staged[cell][1u+species]+=densityDelta*
								injected.constituent[species]/injectedGasDensity;
						staged[cell][8]+=densityDelta*injectedSpecificEnergy;
						externalFuelMassKG+=massFlux*trialStep*shape.cellWidthM*shape.cellWidthM;
						const std::size_t lowerFace=(y*shape.nx+x);
						productionMomentum.component[2][lowerFace]=massFlux;
						productionVelocity.component[2][lowerFace]=massFlux/injectedGasDensity;
					}
					ParallelFireSlices(staged.size(),workerCount,[&](const std::size_t cell){
						for(std::size_t component=0u;component<MethaneConservativeDimension;++component)
							staged[cell][component]=static_cast<double>(
								static_cast<float>(staged[cell][component]));
					});
					std::vector<double> stagedTemperature;
					if(!InvertPeriodicTemperaturesWithinBounds(staged,fuel,fuel.TemperatureMinK(),
						fuel.TemperatureMaxK(),
						FireStateProducerPrecision::Binary32,stagedTemperature,&error,
						workerCount,false)){lastAdvanceError=error;trialStep*=0.5;error.clear();continue;}
					bool stagedOK=true;
					std::vector<unsigned char> stagedValid(packetBeginning.size(),1u);
					ParallelFireSlices(packetBeginning.size(),workerCount,[&](const std::size_t cell){
						MethaneCellState represented=FromConservativeVector(staged[cell],
							FireStateProducerPrecision::Binary32);
						represented.temperatureK=stagedTemperature[cell];
						std::string cellError;
						if(!SignedMixtureSensibleEnergy(represented,represented.temperatureK,fuel,
							represented.sensibleEnergyJPerM3,&cellError)){
							stagedValid[cell]=0u;return;
						}
						represented.sensibleEnergyJPerM3=static_cast<double>(static_cast<float>(
							represented.sensibleEnergyJPerM3));
						packetBeginning[cell]=represented;
					});
					const auto invalid=std::find(stagedValid.begin(),stagedValid.end(),0u);
					if(invalid!=stagedValid.end()){
						const std::size_t cell=static_cast<std::size_t>(invalid-stagedValid.begin());
						MethaneCellState represented=FromConservativeVector(staged[cell],
							FireStateProducerPrecision::Binary32);
						represented.temperatureK=stagedTemperature[cell];
						std::string cellError;double representedEnergy=0.0;
						SignedMixtureSensibleEnergy(represented,represented.temperatureK,fuel,
							representedEnergy,&cellError);
						std::ostringstream sourceFailure;sourceFailure<<std::setprecision(17)
							<<"production source staging failed Binary32 canonicalization cell="<<cell
							<<" error="<<cellError<<" rho_total="<<staged[cell][0]<<" species="
							<<staged[cell][1]<<','<<staged[cell][2]<<','<<staged[cell][3]<<','
							<<staged[cell][4]<<','<<staged[cell][5]<<','<<staged[cell][6]<<','
							<<staged[cell][7];lastAdvanceError=sourceFailure.str();
						stagedOK=false;
					}
					if(!stagedOK){trialStep*=0.5;error.clear();continue;}
				}
				if(persistence.productionMetal&&!persistence.Test().forceZeroSourceForTest){
					for(std::size_t cell=0u;cell<shape.nx*shape.ny;++cell){
						bool flameHolderEligible=false;
						commandOK=ProductionEstablishedFlameHolderEligible(cell,shape.nx,shape.ny,
							canonicalPilotMask,sourcePattern,simulationTimeS>=pilotEndS,
							packetBeginning[cell],fuel,FireSimulationTransportRecord::OpenV1(),
							flameHolderEligible,&error);
						if(!commandOK)break;
						if(flameHolderEligible)reactions[cell].primaryEligible=true;
					}
					if(!commandOK){lastAdvanceError=error;break;}
				}
				profileControlMS=std::chrono::duration<double,std::milli>(
					std::chrono::steady_clock::now()-profileStageStart).count();
				solverPhase="production source packet construction";
				if(persistence.Test().burnerLauncherFixture)values.burnerFixtureInputSHA.push_back(
					ProductionBeginningFieldSHA256(caseRecord.caseRecordId,shape,packetBeginning,
						productionMomentum,productionVelocity));
				bool packetOK=true;profileStageStart=std::chrono::steady_clock::now();
				RISE::FireProductionFrozenSourcePacketSeal projectedHeunSource;
				double floorInterceptedW=-1.0,floorConvectedW=0.0,floorMaximumWallK=0.0;
				// First-layer gas temperature rise outside the burner, r bands 0.15-0.3,
				// 0.3-0.5, 0.5-0.7 and 0.7-1.0 m from the domain centre (r269 comparison).
				std::array<double,4> floorBandRiseK={{0.0,0.0,0.0,0.0}};
				if(persistence.UsesProjectedHeunOwner()){
					// The canonical producer owns the source bytes AND their diagnostics.
					// Neither the tangent preparation nor persistence calls the legacy
					// packet producer on this path. Pack the same beginning inputs that
					// BuildProductionRequest later carries to the resident owner.
					RISE::FireProductionFrozenMethaneSourceRequest sourceRequest;
					sourceRequest.shape.nx=shape.nx;sourceRequest.shape.ny=shape.ny;
					sourceRequest.shape.nz=shape.nz;
					sourceRequest.shape.cellWidthM=static_cast<float>(shape.cellWidthM);
					sourceRequest.timeStepS=static_cast<float>(trialStep);
					sourceRequest.beginningTimeS=simulationTimeS;
					sourceRequest.caseRecordEnvelope=caseRecord.envelopeBytes;
					sourceRequest.beginningConservativeValues.resize(9u*shape.CellCount());
					// The ported owner uses canonical inversion, including accepted
					// endpoint states that need not have an exact forward-roundtrip root.
					// Historical replay keeps its explicitly qualified legacy override.
					if(!persistence.portedProductionTransport)
						sourceRequest.sourceEvaluationTemperatureK.resize(shape.CellCount());
					sourceRequest.eligibilityBeginningConservativeValues.resize(9u*shape.CellCount());
					sourceRequest.pilotCommandMask.resize(shape.CellCount());
					sourceRequest.sourceBoundaryContactMask.resize(shape.CellCount());
					sourceRequest.mixingTimeS.resize(shape.CellCount());
					for(std::size_t cell=0u;cell<shape.CellCount();++cell){
						const ConservativeVector tuple=ToConservativeVector(packetBeginning[cell]);
						for(std::size_t component=0u;component<9u;++component){
							sourceRequest.beginningConservativeValues[component*shape.CellCount()+cell]=
								static_cast<float>(tuple[component]);
							sourceRequest.eligibilityBeginningConservativeValues[component*shape.CellCount()+cell]=
								static_cast<float>(beginning[cell][component]);
						}
						if(!persistence.portedProductionTransport)
							sourceRequest.sourceEvaluationTemperatureK[cell]=packetBeginning[cell].temperatureK;
						sourceRequest.pilotCommandMask[cell]=ProductionPilotCommandCell(cell,shape.nx,
							shape.ny,canonicalPilotMask,sourcePattern)?1u:0u;
						sourceRequest.sourceBoundaryContactMask[cell]=ProductionSourceBoundaryContactCell(
							cell,shape.nx,shape.ny,canonicalPilotMask,sourcePattern)?1u:0u;
						sourceRequest.mixingTimeS[cell]=reactions[cell].mixingTimeS;
					}
					std::array<std::vector<float>,3> sourceMomentum,sourceVelocity;
					for(unsigned int axis=0u;axis<3u;++axis){
						sourceMomentum[axis].assign(productionMomentum.component[axis].begin(),
							productionMomentum.component[axis].end());
						sourceVelocity[axis].assign(productionVelocity.component[axis].begin(),
							productionVelocity.component[axis].end());
					}
					sourceRequest.attemptIdentity=RISE::FireProductionAcceptedStatePayloadDigestFast(
						sourceRequest.shape,sourceRequest.beginningConservativeValues,sourceMomentum,sourceVelocity);
					std::uint64_t timeBits=0u;std::memcpy(&timeBits,&simulationTimeS,sizeof(timeBits));
					std::uint32_t dtBits=0u;std::memcpy(&dtBits,&sourceRequest.timeStepS,sizeof(dtBits));
					sourceRequest.attemptIdentity^=timeBits^(static_cast<std::uint64_t>(dtBits)<<32u);
					if(sourceRequest.attemptIdentity==0u)sourceRequest.attemptIdentity=1u;
					sourceRequest.pilotEstablished=simulationTimeS>=pilotEndS;
					sourceRequest.predictiveRadiation=false;sourceRequest.workerCount=workerCount;
					if(caseRecord.derived.floorEmissivity>0.0){
						const std::size_t floorFaces=shape.nx*shape.ny;
						sourceRequest.floorAbsorbingFaceMask.resize(floorFaces);
						sourceRequest.floorTangentialSpeedMPerS.resize(floorFaces);
						for(std::size_t y=0u;y<shape.ny;++y)for(std::size_t x=0u;x<shape.nx;++x){
							const std::size_t face=y*shape.nx+x;
							sourceRequest.floorAbsorbingFaceMask[face]=sourcePattern[face]!=0.0?0u:1u;
							const double u=0.5*(productionVelocity.component[0][y*(shape.nx+1u)+x]+
								productionVelocity.component[0][y*(shape.nx+1u)+x+1u]);
							const double v=0.5*(productionVelocity.component[1][y*shape.nx+x]+
								productionVelocity.component[1][(y+1u)*shape.nx+x]);
							sourceRequest.floorTangentialSpeedMPerS[face]=
								static_cast<float>(std::sqrt(u*u+v*v));
						}
					}
					packetOK=!persistence.Test().forceZeroSourceForTest&&
						FireSim::FireProductionCanonicalSourceAuthority::Build(sourceRequest,projectedHeunSource,&error)&&
						CarryCanonicalSourceForPersistence(projectedHeunSource,packets,escape,error);
					if(packetOK&&!projectedHeunSource.FloorHeatingWPerM3().empty()){
						const double faceArea=shape.cellWidthM*shape.cellWidthM;
						floorInterceptedW=0.0;floorConvectedW=0.0;floorMaximumWallK=0.0;
						for(std::size_t face=0u;face<shape.nx*shape.ny;++face){
							if(sourceRequest.floorAbsorbingFaceMask[face]==0u)continue;
							floorInterceptedW+=caseRecord.derived.floorEmissivity*
								projectedHeunSource.FloorIrradianceWPerM2()[face]*faceArea;
							floorConvectedW+=projectedHeunSource.FloorConvectiveFluxWPerM2()[face]*faceArea;
							floorMaximumWallK=std::max(floorMaximumWallK,
								projectedHeunSource.FloorWallTemperatureK()[face]);
						}
						static const double bandEdgeM[5]={0.15,0.3,0.5,0.7,1.0};
						std::array<double,4> bandCount={{0.0,0.0,0.0,0.0}};
						for(std::size_t face=0u;face<shape.nx*shape.ny;++face){
							if(sourceRequest.floorAbsorbingFaceMask[face]==0u)continue;
							const double x=(static_cast<double>(face%shape.nx)+0.5-0.5*shape.nx)*shape.cellWidthM;
							const double y=(static_cast<double>(face/shape.nx)+0.5-0.5*shape.ny)*shape.cellWidthM;
							const double r=std::sqrt(x*x+y*y);
							for(unsigned int band=0u;band<4u;++band)if(r>=bandEdgeM[band]&&r<bandEdgeM[band+1u]){
								floorBandRiseK[band]+=static_cast<double>(
									projectedHeunSource.BeginningTemperatureK()[face])-
									caseRecord.derived.pilotAmbientTemperatureK;
								bandCount[band]+=1.0;}
						}
						for(unsigned int band=0u;band<4u;++band)
							if(bandCount[band]>0.0)floorBandRiseK[band]/=bandCount[band];
					}
				}else if(caseRecord.derived.floorEmissivity>0.0){
					// Only the canonical source authority implements the r270 floor.
					packetOK=false;error="r270 floor radiation requires the canonical source authority";
				}else if(FireCase::InertPlumeSelected()){
					packetOK=false;error="r271 inert plume requires the canonical source authority";
				}else if(persistence.Test().forceZeroSourceForTest){
					packets.assign(shape.CellCount(),MethaneSourcePacket());escape=RadiationEscapeFactor();
				}else packetOK=BuildFrozenMethaneSourcePackets(packetBeginning,reactions,
					std::vector<double>(shape.CellCount(),cellVolume),300.0,
					caseRecord.derived.referenceHeatReleaseRateW,
					caseRecord.derived.effectiveRadiativeFraction,false,fuel,fuel,
					FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1(),packets,escape,&error,
					workerCount);
				if(packetOK&&persistence.productionMetal&&!persistence.UsesProjectedHeunOwner())
					for(MethaneSourcePacket& packet:packets){
					RepresentMethaneSourcePacketBinary32(packet);
					if(!CertifiedBinary32SourcePacket(packet,fuel)){packetOK=false;
						 error="production source packet failed its Binary32 certificate";break;}
				}
				profileSourceMS=std::chrono::duration<double,std::milli>(
					std::chrono::steady_clock::now()-profileStageStart).count();
				if(packetOK&&persistence.productionMetal){
					solverPhase="production tangent-target construction";
					profileStageStart=std::chrono::steady_clock::now();
					std::vector<ConservativeVector> stagedConservative(packetBeginning.size());
					std::vector<ConservativeVector> sourceDelta(packetBeginning.size());
					ParallelFireSlices(packetBeginning.size(),workerCount,[&](const std::size_t cell){
						stagedConservative[cell]=ToConservativeVector(packetBeginning[cell]);
						for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
							sourceDelta[cell][1u+species]=packets[cell].constituentDelta[species];
						sourceDelta[cell][8]=packets[cell].sensibleEnergyDeltaJPerM3;
					});
					OpenMACField3D stagedVelocity;stagedVelocity.component=productionVelocity.component;
					ConservativeAdvance3DConfig tangentConfig=config;
					tangentConfig.transport.producerPrecision=FireStateProducerPrecision::Binary32;
					std::vector<double> canonicalTargetTemperature(packetBeginning.size());
					std::vector<CellMolecularTransportEvaluation> targetMolecular(
						packetBeginning.size());
					ParallelFireSlices(packetBeginning.size(),workerCount,[&](const std::size_t cell){
						canonicalTargetTemperature[cell]=packetBeginning[cell].temperatureK;
						targetMolecular[cell].gasDensityKGPerM3=states[cell].GasDensity();
						targetMolecular[cell].gasCpJPerKGK=cellTransportEvaluations[cell].gasCpJPerKGK;
						targetMolecular[cell].molecularViscosityPaS=
							cellTransportEvaluations[cell].molecularViscosityPaS;
						targetMolecular[cell].molecularConductivityWPerMK=
							cellTransportEvaluations[cell].molecularConductivityWPerMK;
					});
					for(std::size_t cell=0u;packetOK&&cell<packetBeginning.size();++cell)
						if(invalidReusableTransport[cell]!=0u)packetOK=EvaluateCellMolecularTransport(
							packetBeginning[cell],fuel,FireSimulationTransportRecord::OpenV1(),
							FireStateProducerPrecision::Binary32,targetMolecular[cell],&error);
					if(std::getenv("RISE_FIRE_TARGET_TEMPERATURE_IDENTITY_RED")){
						std::vector<double> reinvertedTemperature;
						packetOK=InvertPeriodicTemperaturesWithinBounds(stagedConservative,fuel,
							tangentConfig.transport.ambientTemperatureK,
							tangentConfig.transport.adiabaticTemperatureK,
							FireStateProducerPrecision::Binary32,reinvertedTemperature,&error,
							workerCount,false)&&reinvertedTemperature.size()==packetBeginning.size();
						if(packetOK)for(std::size_t cell=0u;cell<packetBeginning.size();++cell)
							if(reinvertedTemperature[cell]!=packetBeginning[cell].temperatureK){
								packetOK=false;error="stored production temperature differs from reinversion";
								break;
							}
						if(packetOK)std::fprintf(stderr,"PRODUCTION_TARGET_TEMPERATURE_IDENTITY cells=%zu\n",
							packetBeginning.size());
					}
					std::vector<double> divergenceTarget;
					// Fast mode: the projected-Heun owner derives its divergence target on the device and never
					// reads this host target (BuildProductionProjectedHeunOwnerRequest does not carry it).
					if(packetOK&&RISE::FireSim::FireFastMode()&&persistence.UsesProjectedHeunOwner())
						divergenceTarget.assign(packetBeginning.size(),0.0);
					else
					packetOK=packetOK&&MonitoredProductionTangentDivergenceTarget3D(shape,stagedConservative,
						stagedVelocity,sourceDelta,tangentConfig,fuel,fuel,
						FireSimulationTransportRecord::OpenV1(),divergenceTarget,&error,
						&canonicalTargetTemperature,&targetMolecular);
					if(packetOK&&std::getenv("RISE_FIRE_TARGET_MOLECULAR_IDENTITY_RED")){
						std::vector<double> recomputedTarget;std::string recomputedError;
						packetOK=MonitoredProductionTangentDivergenceTarget3D(shape,stagedConservative,
							stagedVelocity,sourceDelta,tangentConfig,fuel,fuel,
							FireSimulationTransportRecord::OpenV1(),recomputedTarget,&recomputedError,
							&canonicalTargetTemperature)&&recomputedTarget==divergenceTarget;
						if(!packetOK)error=recomputedError.empty()?
							"reused molecular transport changed the tangent target":recomputedError;
						else std::fprintf(stderr,"PRODUCTION_TARGET_MOLECULAR_IDENTITY cells=%zu\n",
							packetBeginning.size());
					}
					profileTargetMS=std::chrono::duration<double,std::milli>(
						std::chrono::steady_clock::now()-profileStageStart).count();
					profileStageStart=std::chrono::steady_clock::now();
					MethaneRunCheckpoint productionState;
					productionState.dimensions={{shape.nx,shape.ny,shape.nz}};
					productionState.cellWidthM=shape.cellWidthM;
					productionState.states=packetBeginning;
					productionState.momentum=productionMomentum;
					productionState.velocity=productionVelocity;
					RISE::FireProductionResidentStepRequest request;
					packetOK=packetOK&&ProductionStep::BuildProductionRequest(
						productionState,divergenceTarget,trialStep,request,error,workerCount,
						&cellTransportEvaluations,&invalidReusableTransport);
					if(packetOK&&std::getenv("RISE_FIRE_REQUEST_LAYOUT_IDENTITY_RED")){
						RISE::FireProductionResidentStepRequest serialRequest;
						std::string serialError;
						packetOK=ProductionStep::BuildProductionRequest(
							productionState,divergenceTarget,trialStep,serialRequest,serialError,1u)&&
							ProductionStep::ResidentStepRequestsExactlyEqual(
								request,serialRequest);
						if(!packetOK)error=serialError.empty()?
							"parallel production request differs from the serial request":serialError;
					}
					if(packetOK){
						solverPhase="production resident request construction";
						request.physicalOpenProjectionVCycleCount=19u;
						if(persistence.UsesProjectedHeunOwner())
							request.cellSourceIncrement=projectedHeunSource.SourceDelta();
						else ParallelFireSlices(shape.CellCount(),workerCount,[&](const std::size_t cell){
							for(std::size_t component=0u;component<9u;++component){
								const double value=sourceDelta[cell][component];
								request.cellSourceIncrement[component*shape.CellCount()+cell]=
									static_cast<float>(value);
							}
						});
						profileLayoutMS=std::chrono::duration<double,std::milli>(
							std::chrono::steady_clock::now()-profileStageStart).count();
						profileStageStart=std::chrono::steady_clock::now();
						solverPhase="production resident attempt";
						RISE::FireProductionResidentStepResult production;
						RISE::FireProductionProjectedHeunMetalOwnerResult projectedHeunDiagnostics;
						RISE::FireProductionProjectedHeunMetalOwnerRequest projectedHeunRequest;
						RISE::FireProductionSingleStageFCTDiagnosticResult singleStageFCT;
						bool attemptComputed=false;double wallMS=0.0,deviceMS=0.0;
						for(;;){
							const auto wallStart=std::chrono::steady_clock::now();
							if(persistence.singleStageFCTDiagnostic){
								RISE::FireProductionSingleStageFCTBoundaryState boundaryState;
								boundaryState.pressureOpenInflow=singleStageFCTAcceptedInflow;
								boundaryState.statePayloadIdentity=
									RISE::FireProductionAcceptedStatePayloadDigestFast(
										request.force.shape,request.cellTransport.conservativeValues,
										request.force.beginningMomentumKGPerM2S,
										request.cellTransport.frozenVelocityMPerS);
								attemptComputed=RISE::SealFireProductionSingleStageFCTBoundaryState(
									request.force.shape,request.force.boundary,boundaryState,&error)&&
									RISE::AttemptFireProductionSingleStageFCTDiagnosticMetal(
										request,boundaryState,singleStageFCT,&error);
								if(attemptComputed){
									production.conservativeValues=singleStageFCT.conservativeValues;
									production.physicalProjection=singleStageFCT.physicalProjection;
									production.projection=singleStageFCT.projection;
									production.forceSchedule=singleStageFCT.forceSchedule;
									production.forceDiagnostics=singleStageFCT.forceDiagnostics;
									production.residentProjectionInvocationCount=
										singleStageFCT.residentProjectionInvocationCount;
									production.interstageFullGridTransferCount=
										singleStageFCT.interstageFullGridTransferCount;
									production.terminalStagingCount=singleStageFCT.terminalStagingCount;
									production.combinedCertifiedWorkingSetBytes=
										singleStageFCT.certifiedWorkingSetBytes;
									production.combinedActualMetalAllocationBytes=
										singleStageFCT.actualMetalAllocationBytes;
									production.representedTimeStepS=request.force.timeStepS;
									production.manifoldDynamicsBoundPassed=true;
									production.manifoldDiagnosticsMonitored=true;
									production.manifoldPlateauPassed=true;
									production.conservativeProducerPrecision=
										FireStateProducerPrecision::Binary32;
									production.acceptedShape=request.force.shape;
								}
							}else if(persistence.UsesProjectedHeunOwner()){
								if(std::getenv("RISE_FIRE_PICARD_TRACE"))std::fprintf(stderr,
									"PICARD_ATTEMPT step=%u beginning=%.17g reduction=%u dt=%.17g\n",
									acceptedSteps,simulationTimeS,reduction,trialStep);
								attemptComputed=packetOK&&BuildProductionProjectedHeunOwnerRequest(request,
									projectedHeunSource,caseRecord.envelopeBytes,
									static_cast<float>(config.projectionTolerancePerS),
									static_cast<float>(config.openBoundary.velocityToleranceMPerS),
									projectedHeunRequest,error,persistence.portedProductionTransport?
										&config.openBoundary:nullptr);
								projectedHeunRequest.captureAcceptedBudgetFlux=persistence.r245AcceptedBudget||persistence.Test().r245ObservationFixture||
									FastModeConservationDue(acceptedSteps);
								attemptComputed=attemptComputed&&
									RISE::AttemptFireProductionProjectedHeunResidentStepMetal(projectedHeunRequest,
										production,&projectedHeunDiagnostics,&error);
							}else attemptComputed=persistence.compatibleMomentumDiagnostic?
								RISE::AttemptFireProductionCompatibleMomentumDiagnosticMetal(
									request,production,&error):
								RISE::AttemptFireProductionResidentStepMetal(request,production,&error);
							wallMS+=std::chrono::duration<double,std::milli>(
								std::chrono::steady_clock::now()-wallStart).count();
							if(std::isfinite(production.deviceMakespanMS)&&
								production.deviceMakespanMS>=0.0)deviceMS+=production.deviceMakespanMS;
							const bool twoProjectionAttempt=
								production.residentProjectionInvocationCount==2u;
							const RISE::FireProductionProjectionResult& retryProjection=
								twoProjectionAttempt?production.physicalProjection:production.projection;
							if(persistence.singleStageFCTDiagnostic||retryProjection.validationPassed||
								production.HasAcceptedManifoldToken())break;
							const double pre=retryProjection.
								maximumPreProjectionResidualPerS;
							const double post=retryProjection.
								maximumPostProjectionResidualPerS;
							const double band=retryProjection.validationBandPerS;
							const std::uint32_t cycles=retryProjection.executedVCycleCount;
							if((twoProjectionAttempt&&!production.projection.validationPassed)||cycles!=
								request.physicalOpenProjectionVCycleCount||cycles<1u||cycles>=64u||
								!std::isfinite(pre)||!std::isfinite(post)||!std::isfinite(band)||
								!(pre>post)||!(post>0.0)||!(band>0.0)||!(post>band))break;
							const double contraction=std::pow(post/pre,1.0/static_cast<double>(cycles));
							const double requiredReal=(contraction>0.0&&contraction<1.0)?
								std::ceil(std::log(band/pre)/std::log(contraction)):0.0;
							if(!std::isfinite(requiredReal)||requiredReal<=cycles||requiredReal>64.0)break;
							request.physicalOpenProjectionVCycleCount=
								static_cast<std::uint32_t>(requiredReal);
							++values.productionPhysicalProjectionRetryCount;
							production=RISE::FireProductionResidentStepResult();error.clear();
						}
						if((persistence.r220ResumeProbe||persistence.r221Retained)&&attemptComputed&&
							projectedHeunDiagnostics.qualifiedKernelSetSHA256!=R220KernelSHA){
							error=lastAdvanceError="r220 original kernel set changed";break;}
						if(!R216AttemptPublicationAccepted(persistence.tier10RetainedContinuation,attemptComputed,
							projectedHeunDiagnostics.qualifiedKernelSetSHA256)){
							mandatoryEvidenceFailure=true;advancedOK=false;
							error=lastAdvanceError="r216 original kernel set changed";break;
						}
						unsigned int nextCandidate=0u;double nextTimeStepS=0.0;
						const RISE::FireProductionResidentStepAttemptDisposition disposition=
							persistence.singleStageFCTDiagnostic?
								(attemptComputed&&singleStageFCT.accepted?
									RISE::FireProductionResidentStepAttemptDisposition::Accepted:
									RISE::FireProductionResidentStepAttemptDisposition::Rejected):
								RISE::ClassifyFireProductionResidentStepAttempt(
									reduction,production,nextCandidate,nextTimeStepS);
						if(!persistence.productionOnsetDiagnosticDirectory.empty()&&
							disposition!=RISE::FireProductionResidentStepAttemptDisposition::Accepted){
							std::ofstream retryTrajectory(persistence.productionOnsetDiagnosticDirectory/
								"retry_attempt_trajectory.csv",std::ios::app);
							retryTrajectory<<std::setprecision(17)<<simulationTimeS<<','<<reduction<<','<<
								static_cast<double>(production.representedTimeStepS)<<','<<
								production.maximumAcceptedManifoldDeviation<<','<<
								production.acceptedManifoldDeviationP95<<','<<
								production.manifoldTailCellCount<<','<<
								production.manifoldTailDrainedVolumeM3<<','<<
								(production.manifoldDynamicsBoundPassed?1:0)<<','<<
								(production.physicalProjection.validationPassed?1:0)<<','<<
								(production.projection.validationPassed?1:0)<<','<<
								(production.HasAcceptedManifoldToken()?1:0)<<','<<
								(production.manifoldNextTimeStepAvailable?1:0)<<','<<
								production.suggestedManifoldTimeStepS<<','<<
								static_cast<unsigned int>(disposition)<<','<<error<<'\n';
							if(!ClosePublishedStream(retryTrajectory)){mandatoryEvidenceFailure=true;
								advancedOK=false;lastAdvanceError=
								"production onset retry trajectory publication failed";break;}
						}
						double attemptMaximumVelocity=0.0;unsigned int attemptMaximumAxis=0u;
						std::size_t attemptMaximumFace=0u,attemptMaximumX=0u,
							attemptMaximumY=0u,attemptMaximumZ=0u;
						for(unsigned int axis=0u;axis<3u;++axis)
							for(std::size_t face=0u;face<production.projection.velocityMPerS[axis].size();++face){
								const double magnitude=std::fabs(static_cast<double>(
									production.projection.velocityMPerS[axis][face]));
								if(magnitude>attemptMaximumVelocity){attemptMaximumVelocity=magnitude;
									attemptMaximumAxis=axis;attemptMaximumFace=face;}
							}
						if(!FireProductionStaggeredColumn::DecodeFace(
							{{request.force.shape.nx,request.force.shape.ny,request.force.shape.nz}},
							attemptMaximumAxis,attemptMaximumFace,attemptMaximumX,attemptMaximumY,attemptMaximumZ)){
							mandatoryEvidenceFailure=true;advancedOK=false;
							error=lastAdvanceError="production onset extreme face index invalid";break;}
						advancedOK=attemptComputed;
						std::filesystem::path effectiveMomentumAuditPath;
						std::size_t onsetThresholdIndex=productionOnsetVelocityThresholds.size();
						std::size_t auditColumnX=38u,auditColumnY=42u;
						bool exactTimeObservation=false;
						if(const char* auditPath=std::getenv("RISE_FIRE_MOMENTUM_AUDIT_PATH"))
							effectiveMomentumAuditPath=auditPath;
						if(disposition==RISE::FireProductionResidentStepAttemptDisposition::Accepted&&
							!persistence.productionOnsetDiagnosticDirectory.empty())
							for(std::size_t threshold=0u;threshold<productionOnsetVelocityThresholds.size();
								++threshold)if(!productionOnsetThresholdCaptured[threshold]&&
								attemptMaximumVelocity>=productionOnsetVelocityThresholds[threshold]){
								if(!FireProductionStaggeredColumn::AdjacentColumn(
									{{request.force.shape.nx,request.force.shape.ny,request.force.shape.nz}},
									attemptMaximumAxis,attemptMaximumX,attemptMaximumY,attemptMaximumZ,
									auditColumnX,auditColumnY)){lastAdvanceError=
									"production onset extreme has invalid staggered coordinates";
									fatalOnsetAuditFailure=true;advancedOK=false;break;}
								onsetThresholdIndex=threshold;std::ostringstream name;
								name<<"threshold_"<<static_cast<unsigned int>(
									productionOnsetVelocityThresholds[threshold])<<".raw.csv";
								effectiveMomentumAuditPath=
									persistence.productionOnsetDiagnosticDirectory/name.str();break;
							}
						if(fatalOnsetAuditFailure)break;
						if(onsetThresholdIndex==productionOnsetVelocityThresholds.size()&&
							disposition==RISE::FireProductionResidentStepAttemptDisposition::Accepted&&
							!productionMomentumObservationCaptured&&
							persistence.productionMomentumObservationTimeS>0.0&&
							simulationTimeS>=persistence.productionMomentumObservationTimeS){
							exactTimeObservation=true;
							if(!(persistence.productionMomentumObservationReferenceTier>0.0)){
								lastAdvanceError="production matched observation lacks reference tier";
								mandatoryEvidenceFailure=true;advancedOK=false;break;}
							const double referenceCellWidth=caseRecord.derived.
								characteristicDiameterM/
								persistence.productionMomentumObservationReferenceTier;
							auditColumnX=static_cast<std::size_t>(std::floor(
								(static_cast<double>(persistence.productionMomentumObservationColumnX)+0.5)*
								referenceCellWidth/shape.cellWidthM));
							auditColumnY=static_cast<std::size_t>(std::floor(
								(static_cast<double>(persistence.productionMomentumObservationColumnY)+0.5)*
								referenceCellWidth/shape.cellWidthM));
							effectiveMomentumAuditPath=
								persistence.productionOnsetDiagnosticDirectory/
								"reference_composition_candidate_fixed_column.raw.csv";
						}
						if(attemptComputed&&
							disposition==RISE::FireProductionResidentStepAttemptDisposition::Accepted&&
							!effectiveMomentumAuditPath.empty()){
							RISE::FireProductionResidentStepResult physicalOnly;
							RISE::FireProductionFrozenForceAdvanceResult forceCPU;
							RISE::FireProductionFrozenForceResult forceFieldsCPU;
							RISE::FireProductionResidentForceDiagnostics forceAuditDiagnostics;
							RISE::FireProductionCellPalindromeResult cellCPU;
							RISE::FireProductionDualMomentumResult dualCPU;
							std::string physicalOnlyError;
							const bool requestRetainedPhysicalProjection=
								!persistence.productionOnsetDiagnosticDirectory.empty()||
								persistence.compatibleMomentumDiagnostic||
								persistence.singleStageFCTDiagnostic;
							const bool restorationModeClear=
								std::getenv("RISE_FIRE_PRODUCTION_RESTORATION_TEST")==nullptr;
							const bool retainedPhysicalIsTerminal=requestRetainedPhysicalProjection&&
								!production.manifoldTailRestorationApplied&&
								production.physicalProjection.velocityMPerS[0].empty();
							const bool useRetainedPhysicalProjection=requestRetainedPhysicalProjection&&
								(retainedPhysicalIsTerminal||
								 !production.physicalProjection.velocityMPerS[0].empty());
							bool physicalOnlyComputed=false;
							if(useRetainedPhysicalProjection){
								// The accepted attempt already retains the exact on-device physical
								// projection. Re-solving it on the CPU would measure a different
								// implementation and, near onset, can move the pressure budget.
								physicalOnlyComputed=retainedPhysicalIsTerminal?
									!production.projection.velocityMPerS[0].empty():
									!production.physicalProjection.velocityMPerS[0].empty();
							}else if(attemptComputed&&restorationModeClear&&
								setenv("RISE_FIRE_PRODUCTION_RESTORATION_TEST","removed",1)==0){
								physicalOnlyComputed=persistence.compatibleMomentumDiagnostic?
									RISE::AttemptFireProductionCompatibleMomentumDiagnosticMetal(
										request,physicalOnly,&physicalOnlyError):
									RISE::AttemptFireProductionResidentStepMetal(
										request,physicalOnly,&physicalOnlyError);
								unsetenv("RISE_FIRE_PRODUCTION_RESTORATION_TEST");
							}
							const RISE::FireProductionProjectionResult& physical=useRetainedPhysicalProjection?
								(retainedPhysicalIsTerminal?production.projection:
								 production.physicalProjection):physicalOnly.projection;
							double physicalVelocityMaximum=0.0,terminalVelocityMaximum=0.0;
							double restorationVelocityMaximum=0.0,physicalImpulseMaximum=0.0;
							double restorationImpulseMaximum=0.0,compatibilityResidualMaximum=0.0;
							unsigned int terminalAxis=0u,restorationAxis=0u;
							std::size_t terminalFace=0u,restorationFace=0u;
							const char* auditPhase="paired projection attempt";
							bool forceInclusiveProvisionalByteIdentity=false;
							double forceInclusiveProvisionalDifferenceMaximum=0.0;
							std::size_t forceInclusiveProvisionalDifferenceCount=0u;
							const bool attemptPayloadAvailable=
								!production.projection.velocityMPerS[0].empty();
							const bool physicalOnlyPayloadAvailable=
								!physical.velocityMPerS[0].empty();
							const bool physicalDiagnosticsIdentity=
								!useRetainedPhysicalProjection||physical.validationPassed;
							const bool pairedTransportIdentity=attemptPayloadAvailable&&
								physicalOnlyPayloadAvailable&&physicalDiagnosticsIdentity&&
								(useRetainedPhysicalProjection||
								physicalOnly.transportedDual.momentum==production.transportedDual.momentum);
							bool auditValid=attemptPayloadAvailable&&physicalOnlyPayloadAvailable&&
								pairedTransportIdentity;
							for(unsigned int axis=0u;axis<3u&&auditValid;++axis){
								const std::size_t faces=production.projection.velocityMPerS[axis].size();
								auditValid=faces>0u&&physical.velocityMPerS[axis].size()==faces&&
									production.projection.momentumKGPerM2S[axis].size()==faces&&
									physical.momentumKGPerM2S[axis].size()==faces&&
									production.projection.faceDensityKGPerM3[axis].size()==faces&&
									production.transportedDual.momentum[axis].size()==faces;
								for(std::size_t face=0u;face<faces&&auditValid;++face){
									const double physicalVelocity=physical.velocityMPerS[axis][face];
									const double terminalVelocity=production.projection.velocityMPerS[axis][face];
									const double restorationVelocity=terminalVelocity-physicalVelocity;
									const double physicalImpulse=physical.momentumKGPerM2S[axis][face]-
										production.transportedDual.momentum[axis][face];
									const double restorationImpulse=
										production.projection.momentumKGPerM2S[axis][face]-
										physical.momentumKGPerM2S[axis][face];
									const double compatibility=std::fabs(
										production.projection.momentumKGPerM2S[axis][face]-
										production.projection.faceDensityKGPerM3[axis][face]*terminalVelocity);
									physicalVelocityMaximum=std::max(physicalVelocityMaximum,
										std::fabs(physicalVelocity));
									if(std::fabs(terminalVelocity)>terminalVelocityMaximum){
										terminalVelocityMaximum=std::fabs(terminalVelocity);
										terminalAxis=axis;terminalFace=face;}
									if(std::fabs(restorationVelocity)>restorationVelocityMaximum){
										restorationVelocityMaximum=std::fabs(restorationVelocity);
										restorationAxis=axis;restorationFace=face;}
									physicalImpulseMaximum=std::max(physicalImpulseMaximum,
										std::fabs(physicalImpulse));
									restorationImpulseMaximum=std::max(restorationImpulseMaximum,
										std::fabs(restorationImpulse));
									compatibilityResidualMaximum=std::max(compatibilityResidualMaximum,
										compatibility);
								}
							}
							const std::size_t columnX=auditColumnX,columnY=auditColumnY;
							std::vector<float> columnPhysicalMomentum(request.force.shape.nz+1u,0.0f);
							for(std::size_t z=0u;z<columnPhysicalMomentum.size()&&auditValid;++z){
								const std::size_t face=(z*request.force.shape.ny+columnY)*
									request.force.shape.nx+columnX;
								if(face>=physical.momentumKGPerM2S[2].size())auditValid=false;
								else columnPhysicalMomentum[z]=physical.momentumKGPerM2S[2][face];
							}
							const double terminalPhysicalMomentum=auditValid?
								physical.momentumKGPerM2S[terminalAxis][terminalFace]:0.0;
							const double terminalPhysicalVelocity=auditValid?
								physical.velocityMPerS[terminalAxis][terminalFace]:0.0;
							physicalOnly=RISE::FireProductionResidentStepResult();
							auditPhase="independent resident force reconstruction";
							const bool forceComparatorOK=auditValid&&
								RISE::AdvanceFireProductionFrozenForceMetalResidentStateComparator(
									request.force,forceCPU,forceAuditDiagnostics,&physicalOnlyError);
							const bool forceDiagnosticsIdentity=forceComparatorOK&&
								(persistence.UsesProjectedHeunOwner()||
								 forceAuditDiagnostics.outwardLambdaPerS==
									production.forceDiagnostics.outwardLambdaPerS);
							const bool forceScheduleIdentity=forceDiagnosticsIdentity&&
								(persistence.UsesProjectedHeunOwner()||
								 (forceCPU.schedule.substepCount==production.forceSchedule.substepCount&&
								  forceCPU.schedule.substepTimeS==production.forceSchedule.substepTimeS));
							const bool forceFieldsOK=forceScheduleIdentity&&
								RISE::BuildFireProductionFrozenForceFieldsCPU(
									request.force,forceFieldsCPU,&physicalOnlyError);
							RISE::FireProductionCellPalindromeRequest auditCellTransport=
								request.cellTransport;
							auditCellTransport.retainAcceptedGasMassDose=true;
							const bool cellTransportOK=forceFieldsOK&&
								RISE::RemapFireProductionCellPalindromeCPU(
									auditCellTransport,cellCPU,&physicalOnlyError);
							auditValid=cellTransportOK;
							if(auditValid){
								auditPhase="CPU compatible dual remap from force-inclusive momentum";
								RISE::FireProductionDualMomentumRequest forceInclusive=request.dualTransport;
								forceInclusive.beginningMomentum=forceCPU.momentumKGPerM2S;
								auditValid=RISE::RemapFireProductionCompatibleDualMomentumCPU(
									forceInclusive,cellCPU.acceptedGasMassDoseKGPerM2,
									dualCPU,&physicalOnlyError);
								for(unsigned int axis=0u;axis<3u&&auditValid;++axis){
									auditValid=dualCPU.momentum[axis].size()==
										request.momentumSourceIncrement[axis].size();
									for(std::size_t face=0u;face<dualCPU.momentum[axis].size()&&
										auditValid;++face)dualCPU.momentum[axis][face]+=
										request.momentumSourceIncrement[axis][face];
								}
							}
							auditPhase="force-inclusive provisional comparison";
							if(auditValid)
								for(unsigned int axis=0u;axis<3u;++axis)
								for(std::size_t face=0u;face<dualCPU.momentum[axis].size();++face){
									const double difference=std::fabs(static_cast<double>(dualCPU.momentum[axis][face])-
										production.transportedDual.momentum[axis][face]);
									if(difference!=0.0)++forceInclusiveProvisionalDifferenceCount;
									forceInclusiveProvisionalDifferenceMaximum=std::max(
										forceInclusiveProvisionalDifferenceMaximum,difference);
								}
							forceInclusiveProvisionalByteIdentity=
								auditValid&&
								forceInclusiveProvisionalDifferenceCount==0u;
							if(!auditValid){
								mandatoryEvidenceFailure=true;
								lastAdvanceError=std::string("production momentum audit failed at ")+auditPhase+
									" [attempt="+(attemptComputed?"1":"0")+
									" physical_only="+(physicalOnlyComputed?"1":"0")+
									" attempt_payload="+(attemptPayloadAvailable?"1":"0")+
									" physical_payload="+(physicalOnlyPayloadAvailable?"1":"0")+
									" paired_transport="+(pairedTransportIdentity?"1":"0")+
									" force="+(forceComparatorOK?"1":"0")+
									" diagnostics="+(forceDiagnosticsIdentity?"1":"0")+
									" schedule="+(forceScheduleIdentity?"1":"0")+
									" fields="+(forceFieldsOK?"1":"0")+
									" cell="+(cellTransportOK?"1":"0")+"]"+
									(physicalOnlyError.empty()?"":": "+physicalOnlyError);
								std::fprintf(stderr,"%s\n",lastAdvanceError.c_str());advancedOK=false;break;
							}
							if(auditValid){
								if(auditColumnX>=request.force.shape.nx||
									auditColumnY>=request.force.shape.ny){
									mandatoryEvidenceFailure=true;
									lastAdvanceError="production momentum audit column is out of range";
									advancedOK=false;break;
								}
								const double representedStep=static_cast<double>(production.representedTimeStepS);
								double columnStressMaximum=0.0,columnBuoyancyMaximum=0.0,
									columnAdvectionMaximum=0.0,columnSourceMaximum=0.0,
									columnPressureMaximum=0.0,columnRestorationMaximum=0.0,
									columnTotalMaximum=0.0,columnClosureMaximum=0.0,
									columnVremanMinimum=std::numeric_limits<double>::infinity(),
									columnVremanMaximum=0.0;
								std::size_t columnAdvectionMaximumZ=0u;
								const std::filesystem::path columnPath=
									effectiveMomentumAuditPath.string()+".column.csv";
								std::error_code columnSizeError;const bool writeColumnHeader=
									!std::filesystem::exists(columnPath)||
									std::filesystem::file_size(columnPath,columnSizeError)==0u;
								std::ofstream columnAudit(columnPath,std::ios::app);
								if(writeColumnHeader)columnAudit<<"beginning_time_s,candidate,dt_s,x,y,z_face,"
									"beginning_momentum,stress_rate,buoyancy_rate,advection_rate,source_rate,"
									<<(persistence.UsesProjectedHeunOwner()?
									 "combined_projection_rate,standalone_restoration_rate,":
									 "pressure_gradient_rate,restoration_rate,")
									<<"total_rate,closure_residual,"
									"lower_vreman_m2_per_s,upper_vreman_m2_per_s\n";
								for(std::size_t z=0u;z<=request.force.shape.nz;++z){
									const std::size_t face=(z*request.force.shape.ny+columnY)*
										request.force.shape.nx+columnX;
									const double beginning=request.force.beginningMomentumKGPerM2S[2][face];
									const double gravity=forceFieldsCPU.
										gravityMomentumIncrementKGPerM2S[2][face];
									const double stress=persistence.UsesProjectedHeunOwner()?
										projectedHeunDiagnostics.heunStressMomentumRateKGPerM2S2[2][face]:
										(forceCPU.momentumKGPerM2S[2][face]-beginning-gravity)/representedStep;
									const double buoyancy=persistence.UsesProjectedHeunOwner()?
										projectedHeunDiagnostics.heunBuoyancyMomentumRateKGPerM2S2[2][face]:
										gravity/representedStep;
									const double source=persistence.UsesProjectedHeunOwner()?
										projectedHeunDiagnostics.heunPhaseSourceMomentumRateKGPerM2S2[2][face]:
										request.momentumSourceIncrement[2][face]/representedStep;
									const double advection=persistence.UsesProjectedHeunOwner()?
										projectedHeunDiagnostics.heunAdvectionMomentumRateKGPerM2S2[2][face]:
										(production.transportedDual.momentum[2][face]-
										 request.momentumSourceIncrement[2][face]-
										 forceCPU.momentumKGPerM2S[2][face])/representedStep;
									const double pressure=(production.projection.momentumKGPerM2S[2][face]-
										production.transportedDual.momentum[2][face])/representedStep;
									const double restoration=0.0;
									const double total=(production.projection.momentumKGPerM2S[2][face]-beginning)/
										representedStep;
									const double closure=total-(stress+buoyancy+advection+source+pressure+restoration);
									const std::size_t lowerZ=z==0u?0u:z-1u;
									const std::size_t upperZ=std::min(z,request.force.shape.nz-1u);
									const std::size_t lowerCell=columnX+request.force.shape.nx*(columnY+
										request.force.shape.ny*lowerZ);
									const std::size_t upperCell=columnX+request.force.shape.nx*(columnY+
										request.force.shape.ny*upperZ);
									const double lowerVreman=persistence.UsesProjectedHeunOwner()?
										projectedHeunDiagnostics.heunEddyKinematicViscosityM2PerS[lowerCell]:
										forceFieldsCPU.eddyKinematicViscosityM2PerS[lowerCell];
									const double upperVreman=persistence.UsesProjectedHeunOwner()?
										projectedHeunDiagnostics.heunEddyKinematicViscosityM2PerS[upperCell]:
										forceFieldsCPU.eddyKinematicViscosityM2PerS[upperCell];
									columnStressMaximum=std::max(columnStressMaximum,std::fabs(stress));
									columnBuoyancyMaximum=std::max(columnBuoyancyMaximum,std::fabs(buoyancy));
									if(std::fabs(advection)>columnAdvectionMaximum){
										columnAdvectionMaximum=std::fabs(advection);
										columnAdvectionMaximumZ=z;}
									columnSourceMaximum=std::max(columnSourceMaximum,std::fabs(source));
									columnPressureMaximum=std::max(columnPressureMaximum,std::fabs(pressure));
									columnRestorationMaximum=std::max(columnRestorationMaximum,std::fabs(restoration));
									columnTotalMaximum=std::max(columnTotalMaximum,std::fabs(total));
									columnClosureMaximum=std::max(columnClosureMaximum,std::fabs(closure));
									columnVremanMinimum=std::min({columnVremanMinimum,lowerVreman,upperVreman});
									columnVremanMaximum=std::max({columnVremanMaximum,lowerVreman,upperVreman});
									columnAudit<<std::setprecision(17)<<simulationTimeS<<','<<reduction<<','<<
										representedStep<<','<<columnX<<','<<columnY<<','<<z<<','<<beginning<<','<<
										stress<<','<<buoyancy<<','<<advection<<','<<source<<','<<pressure<<','<<
										restoration<<','<<total<<','<<closure<<','<<lowerVreman<<','<<upperVreman<<'\n';
								}
								columnAudit.close();
								if(!columnAudit){lastAdvanceError="production momentum column audit write failed";
									mandatoryEvidenceFailure=true;advancedOK=false;break;}
								// Keep the historical vertical file unchanged. This companion includes
								// both horizontal sides of each column cell and every vertical face.
								// The resident owner's coupled projection is not a separately measured
								// restoration impulse; report only the actual projected-minus-provisional rate.
								FireProductionStaggeredColumn::FaceRates64 legacyStress,legacyBuoyancy,
									legacyAdvection,legacySource;
								if(!persistence.UsesProjectedHeunOwner())for(unsigned int axis=0u;axis<3u;++axis){
									const std::size_t faces=request.force.beginningMomentumKGPerM2S[axis].size();
									legacyStress[axis].resize(faces);legacyBuoyancy[axis].resize(faces);
									legacyAdvection[axis].resize(faces);legacySource[axis].resize(faces);
									for(std::size_t face=0u;face<faces;++face){
										const double beginning=request.force.beginningMomentumKGPerM2S[axis][face];
										const double gravity=forceFieldsCPU.gravityMomentumIncrementKGPerM2S[axis][face];
										legacyStress[axis][face]=(forceCPU.momentumKGPerM2S[axis][face]-
											beginning-gravity)/representedStep;
										legacyBuoyancy[axis][face]=gravity/representedStep;
										legacySource[axis][face]=request.momentumSourceIncrement[axis][face]/representedStep;
										legacyAdvection[axis][face]=(production.transportedDual.momentum[axis][face]-
											request.momentumSourceIncrement[axis][face]-forceCPU.momentumKGPerM2S[axis][face])/representedStep;
									}
								}
								const FireProductionStaggeredColumn::Inputs staggeredInput={
									request.force.beginningMomentumKGPerM2S,
									persistence.UsesProjectedHeunOwner()?projectedHeunDiagnostics.provisionalMomentumKGPerM2S:
										production.transportedDual.momentum,
									production.projection.momentumKGPerM2S,
									persistence.UsesProjectedHeunOwner()?FireProductionStaggeredColumn::RateView(projectedHeunDiagnostics.heunStressMomentumRateKGPerM2S2):FireProductionStaggeredColumn::RateView(legacyStress),
									persistence.UsesProjectedHeunOwner()?FireProductionStaggeredColumn::RateView(projectedHeunDiagnostics.heunBuoyancyMomentumRateKGPerM2S2):FireProductionStaggeredColumn::RateView(legacyBuoyancy),
									persistence.UsesProjectedHeunOwner()?FireProductionStaggeredColumn::RateView(projectedHeunDiagnostics.heunAdvectionMomentumRateKGPerM2S2):FireProductionStaggeredColumn::RateView(legacyAdvection),
									persistence.UsesProjectedHeunOwner()?FireProductionStaggeredColumn::RateView(projectedHeunDiagnostics.heunPhaseSourceMomentumRateKGPerM2S2):FireProductionStaggeredColumn::RateView(legacySource),
									persistence.UsesProjectedHeunOwner()?projectedHeunDiagnostics.heunEddyKinematicViscosityM2PerS:
										forceFieldsCPU.eddyKinematicViscosityM2PerS};
								std::vector<FireProductionStaggeredColumn::Row> staggeredRows;
								if(!FireProductionStaggeredColumn::Build(
									{{request.force.shape.nx,request.force.shape.ny,request.force.shape.nz}},
									columnX,columnY,representedStep,staggeredInput,staggeredRows)){
									lastAdvanceError="production staggered column authority or geometry invalid";
									mandatoryEvidenceFailure=true;advancedOK=false;break;}
								const std::filesystem::path staggeredColumnPath=
									effectiveMomentumAuditPath.string()+".staggered_column.csv";
								std::error_code staggeredSizeError;const bool staggeredHeader=
									!std::filesystem::exists(staggeredColumnPath)||
									std::filesystem::file_size(staggeredColumnPath,staggeredSizeError)==0u;
								std::ofstream staggeredOutput(staggeredColumnPath,std::ios::app);
								const bool staggeredWritten=FireProductionStaggeredColumn::Write(staggeredOutput,
									staggeredHeader,simulationTimeS,reduction,representedStep,staggeredRows);
								staggeredOutput.close();
								if(!staggeredWritten||!staggeredOutput){
									lastAdvanceError="production staggered column audit write failed";
									mandatoryEvidenceFailure=true;advancedOK=false;break;}
								std::error_code sizeError;const bool writeHeader=
									!std::filesystem::exists(effectiveMomentumAuditPath)||
									std::filesystem::file_size(effectiveMomentumAuditPath,sizeError)==0u;
								std::ofstream audit(effectiveMomentumAuditPath,std::ios::app);
								if(writeHeader)audit<<"beginning_time_s,candidate,dt_s,"
									<<(persistence.UsesProjectedHeunOwner()?
									 "terminal_tail_demand_cells,terminal_tail_demand_volume_m3,":
									 "tail_cells,tail_drained_m3,")
									<<
									"physical_velocity_max_m_per_s,restoration_delta_velocity_max_m_per_s,"
									"terminal_velocity_max_m_per_s,physical_impulse_max_kg_per_m2_s,"
									"restoration_impulse_max_kg_per_m2_s,terminal_axis,terminal_face,"
									"terminal_provisional_momentum,terminal_physical_momentum,"
									"terminal_momentum,terminal_face_density,terminal_physical_velocity,"
									"terminal_velocity,restoration_axis,restoration_face,"
									"compatibility_residual_max,column_stress_rate_max,column_buoyancy_rate_max,"
									"column_advection_rate_max,column_source_rate_max,column_pressure_rate_max,"
									"column_restoration_rate_max,column_total_rate_max,column_closure_residual_max,"
									"column_vreman_min_m2_per_s,column_vreman_max_m2_per_s,"
									"attempt_velocity_max_m_per_s,attempt_velocity_axis,attempt_velocity_face,"
									"attempt_velocity_x,attempt_velocity_y,attempt_velocity_z,"
									"force_inclusive_provisional_byte_identity,force_inclusive_provisional_"
									"difference_count,force_inclusive_provisional_difference_max,"
									"source_packet_field_sha256\n";
								const std::string sourcePacketFieldDigest=
									ProductionSourcePacketFieldSHA256(packets);
								audit<<std::setprecision(17)<<simulationTimeS<<','<<reduction<<','<<
									static_cast<double>(production.representedTimeStepS)<<','<<
									production.manifoldTailCellCount<<','<<
									production.manifoldTailDrainedVolumeM3<<','<<physicalVelocityMaximum<<','<<
									restorationVelocityMaximum<<','<<terminalVelocityMaximum<<','<<
									physicalImpulseMaximum<<','<<restorationImpulseMaximum<<','<<terminalAxis<<','<<
									terminalFace<<','<<production.transportedDual.momentum[terminalAxis][terminalFace]<<','<<
									terminalPhysicalMomentum<<','<<
									production.projection.momentumKGPerM2S[terminalAxis][terminalFace]<<','<<
									production.projection.faceDensityKGPerM3[terminalAxis][terminalFace]<<','<<
									terminalPhysicalVelocity<<','<<
									production.projection.velocityMPerS[terminalAxis][terminalFace]<<','<<
									restorationAxis<<','<<restorationFace<<','<<compatibilityResidualMaximum<<','<<
									columnStressMaximum<<','<<columnBuoyancyMaximum<<','<<columnAdvectionMaximum<<','<<
									columnSourceMaximum<<','<<columnPressureMaximum<<','<<columnRestorationMaximum<<','<<
									columnTotalMaximum<<','<<columnClosureMaximum<<','<<columnVremanMinimum<<','<<
									columnVremanMaximum<<','<<attemptMaximumVelocity<<','<<attemptMaximumAxis<<','<<
									attemptMaximumFace<<','<<attemptMaximumX<<','<<attemptMaximumY<<','<<attemptMaximumZ<<','<<
									(forceInclusiveProvisionalByteIdentity?1:0)<<','<<
									forceInclusiveProvisionalDifferenceCount<<','<<
									forceInclusiveProvisionalDifferenceMaximum<<','<<
									sourcePacketFieldDigest<<'\n';
								audit.close();
								if(!audit){lastAdvanceError="production momentum audit write failed";
									mandatoryEvidenceFailure=true;advancedOK=false;break;}
								const std::filesystem::path sourceContextPath=
									effectiveMomentumAuditPath.string()+".source_context.csv";
								const std::size_t sourceContextX=exactTimeObservation?
									columnX:std::min(attemptMaximumX,request.force.shape.nx-1u);
								const std::size_t sourceContextY=exactTimeObservation?
									columnY:std::min(attemptMaximumY,request.force.shape.ny-1u);
								const std::size_t sourceContextZ=exactTimeObservation?
									std::min(columnAdvectionMaximumZ,request.force.shape.nz-1u):
									std::min(attemptMaximumZ,request.force.shape.nz-1u);
								if(!WriteProductionSourceAndAdjacentStateContext(sourceContextPath,
									request.force.shape,sourceContextX,sourceContextY,
									sourceContextZ,packetBeginning,packets,
									production.conservativeValues,fuel,lastAdvanceError)){
									mandatoryEvidenceFailure=true;advancedOK=false;break;
								}
								if(!WriteProductionSourcePacketFieldEvidence(
									effectiveMomentumAuditPath,packets,states,packetBeginning,momentum,
									request,lastAdvanceError)){
									mandatoryEvidenceFailure=true;advancedOK=false;break;
								}
								std::ofstream event(effectiveMomentumAuditPath.string()+".event.v1",
									std::ios::trunc);
								event<<std::setprecision(17)
									<<"schema rise.fire.production.onset_observation_event.v1\n"
									<<"accepted_step_beginning "<<acceptedSteps<<"\n"
									<<"beginning_time_s "<<simulationTimeS<<"\nend_time_s "<<
										simulationTimeS+representedStep<<"\n"
									<<"realized_max_velocity_m_per_s "<<attemptMaximumVelocity<<"\n"
									<<"max_axis "<<attemptMaximumAxis<<"\nmax_face "<<attemptMaximumFace<<"\n"
									<<"source_packet_field_sha256 "<<sourcePacketFieldDigest<<"\n"
									<<"source_event_threshold "<<(onsetThresholdIndex<
										productionOnsetVelocityThresholds.size()?
										productionOnsetVelocityThresholds[onsetThresholdIndex]:0.0)<<"\n";
								for(const double threshold:productionOnsetVelocityThresholds)
									if(attemptMaximumVelocity>=threshold)event<<"crossed_threshold "<<
										threshold<<'\n';
								event
									<<"exact_reference_composition_endpoint_s "<<simulationTimeS<<"\n";
								event.close();
								if(!event){lastAdvanceError="production onset event write failed";
									mandatoryEvidenceFailure=true;advancedOK=false;break;}
								if((persistence.sealedProjectedHeunReplay||
									persistence.projectedHeunCertificateDiagnostic||persistence.acceptedFuelBudgetDiagnostic)&&
									!WriteProductionCrossingConvergence(effectiveMomentumAuditPath,
										projectedHeunRequest,projectedHeunDiagnostics,simulationTimeS,
										representedStep,acceptedSteps,columnX,columnY,lastAdvanceError,false,sourcePattern)){
									mandatoryEvidenceFailure=true;advancedOK=false;break;}
								observationEvidencePublished=true;
								if(exactTimeObservation)productionMomentumObservationCaptured=true;
								if(onsetThresholdIndex<productionOnsetThresholdCaptured.size()){
									productionOnsetThresholdCaptured[onsetThresholdIndex]=true;
									for(std::size_t skipped=onsetThresholdIndex+1u;
										skipped<productionOnsetThresholdCaptured.size()&&attemptMaximumVelocity>=
											productionOnsetVelocityThresholds[skipped];++skipped){
										std::ostringstream skippedName;skippedName<<"threshold_"<<
											static_cast<unsigned int>(productionOnsetVelocityThresholds[skipped])<<
											".raw.csv";
										const std::filesystem::path skippedPath=
											persistence.productionOnsetDiagnosticDirectory/skippedName.str();
										std::error_code copyError;
										const bool summaryCopied=std::filesystem::copy_file(
											effectiveMomentumAuditPath,skippedPath,
											std::filesystem::copy_options::overwrite_existing,copyError);
										const bool columnCopied=summaryCopied&&std::filesystem::copy_file(
											columnPath,skippedPath.string()+".column.csv",
											std::filesystem::copy_options::overwrite_existing,copyError);
										const bool staggeredCopied=columnCopied&&std::filesystem::copy_file(
											staggeredColumnPath,skippedPath.string()+".staggered_column.csv",
											std::filesystem::copy_options::overwrite_existing,copyError);
										const bool contextCopied=staggeredCopied&&std::filesystem::copy_file(
											sourceContextPath,skippedPath.string()+".source_context.csv",
											std::filesystem::copy_options::overwrite_existing,copyError);
										const bool payloadCopied=contextCopied&&std::filesystem::copy_file(
											effectiveMomentumAuditPath.string()+".source_packets.bin",
											skippedPath.string()+".source_packets.bin",
											std::filesystem::copy_options::overwrite_existing,copyError);
										const bool ledgerCopied=payloadCopied&&std::filesystem::copy_file(
											effectiveMomentumAuditPath.string()+".source_ledger.v1",
											skippedPath.string()+".source_ledger.v1",
											std::filesystem::copy_options::overwrite_existing,copyError);
										const bool completeCopied=ledgerCopied&&std::filesystem::copy_file(
							effectiveMomentumAuditPath.string()+".source_observation_inputs.bin",
							skippedPath.string()+".source_observation_inputs.bin",
											std::filesystem::copy_options::overwrite_existing,copyError);
										if(!completeCopied){lastAdvanceError=
											"production onset skipped-threshold publication failed";
											mandatoryEvidenceFailure=true;advancedOK=false;break;}
										if(persistence.sealedProjectedHeunReplay){
											bool convergenceCopied=true;
											for(const char* suffix:{".convergence.v2",".convergence.v2.csv",
												".convergence.v2.tail_targets.bin"})
												convergenceCopied=convergenceCopied&&std::filesystem::copy_file(
													effectiveMomentumAuditPath.string()+suffix,skippedPath.string()+suffix,
													std::filesystem::copy_options::overwrite_existing,copyError);
											if(!convergenceCopied){lastAdvanceError=
												"production onset convergence alias publication failed";
												mandatoryEvidenceFailure=true;advancedOK=false;break;}
										}
										std::ofstream alias(skippedPath.string()+".event.v1",std::ios::trunc);
										alias<<std::setprecision(17)
											<<"schema rise.fire.production.onset_observation_alias.v1\n"
											<<"threshold "<<productionOnsetVelocityThresholds[skipped]<<"\n"
											<<"source_threshold "<<productionOnsetVelocityThresholds[
												onsetThresholdIndex]<<"\nsource_event_sha256 "<<
												DigestFile(effectiveMomentumAuditPath.string()+".event.v1")<<"\n"
											<<"beginning_time_s "<<simulationTimeS<<"\nend_time_s "<<
												simulationTimeS+representedStep<<"\nrealized_max_velocity_m_per_s "<<
												attemptMaximumVelocity<<'\n';
										alias.close();
										if(!alias){lastAdvanceError=
											"production onset skipped-threshold alias write failed";
											mandatoryEvidenceFailure=true;advancedOK=false;break;}
										productionOnsetThresholdCaptured[skipped]=true;
									}
									if(!advancedOK)break;
								}
							}
						}
						if(!persistence.temporalSnapshotDirectory.empty()&&
							disposition!=RISE::FireProductionResidentStepAttemptDisposition::Accepted)
							std::fprintf(stderr,
							"production attempt candidate=%u computed=%d dt=%.17g dev=%.9g p95=%.9g "
							"tail=%u drain=%.9g dynamics=%d physical=%d terminal=%d token=%d "
							"next=%d suggested=%.17g disposition=%u error=%s\n",
							reduction,attemptComputed?1:0,
							static_cast<double>(production.representedTimeStepS),
							production.maximumAcceptedManifoldDeviation,
							production.acceptedManifoldDeviationP95,
							production.manifoldTailCellCount,
							production.manifoldTailDrainedVolumeM3,
							production.manifoldDynamicsBoundPassed?1:0,
							production.physicalProjection.validationPassed?1:0,
							production.projection.validationPassed?1:0,
							production.HasAcceptedManifoldToken()?1:0,
							production.manifoldNextTimeStepAvailable?1:0,
							production.suggestedManifoldTimeStepS,
							static_cast<unsigned int>(disposition),error.c_str());
						if(disposition==RISE::FireProductionResidentStepAttemptDisposition::
							RetryAtSuggestedTimeStep){
							++values.productionHardBoundRetryCount;
							lastAdvanceError="production monitored dynamics bound requested a reduced step";
							trialStep=nextTimeStepS;error.clear();continue;
						}
						if(disposition==RISE::FireProductionResidentStepAttemptDisposition::Accepted){
							// Publication consumes the one-use capability below. Preserve its
							// authenticated root before that move, never from a copied result.
							const std::string acceptedPayloadRoot=production.AcceptedPublicationPayloadRootSHA256();
							RISE::FireProductionAcceptedManifoldObservation observation;
							advancedOK=persistence.singleStageFCTDiagnostic?
								ProductionStep::ApplyProductionResultUnchecked(
									production,productionState,error,false,workerCount):
								(RISE::PublishFireProductionAcceptedManifoldObservation(
									static_cast<double>(production.representedTimeStepS),production,
									observation,&error)&&
								ProductionStep::ApplyAcceptedProductionResult(
									production,observation,productionState,error,workerCount));
							// Reject before replacing any accepted velocity, boundary classes,
							// observation or history. A retry must start entirely from Q0.
							if(advancedOK&&persistence.Test().burnerLauncherRejectFirstCandidate&&reduction==0u){
								values.burnerFixtureVelocityMutantDetected=ProductionBeginningFieldSHA256(
									caseRecord.caseRecordId,shape,states,momentum,productionState.velocity)!=
									values.burnerFixtureQ0SHA;
								productionState.states.front().temperatureK=caseRecord.derived.maximumAcceptedTemperatureK;
							}
							if(advancedOK)advancedOK=ProductionCandidateTemperatureAccepted(
								productionState.states,caseRecord.derived.maximumAcceptedTemperatureK,error);
							if(advancedOK){
								if(persistence.Test().burnerLauncherFixture)values.burnerFixtureAcceptedSHA=
									ProductionBeginningFieldSHA256(caseRecord.caseRecordId,shape,productionState.states,
										productionState.momentum,productionState.velocity);
								if(persistence.singleStageFCTDiagnostic){
									productionManifoldObservation=FireProductionAcceptedManifoldObservation();
									singleStageFCTAcceptedInflow=singleStageFCT.projection.pressureOpenInflow;
								}else productionManifoldObservation=observation;
								advanced.conservative.resize(productionState.states.size());
								acceptedProductionTemperatureK.resize(productionState.states.size());
								ParallelFireSlices(productionState.states.size(),workerCount,
									[&](const std::size_t cell){
									advanced.conservative[cell]=ToConservativeVector(
										productionState.states[cell]);
									acceptedProductionTemperatureK[cell]=
										productionState.states[cell].temperatureK;
								});
								advanced.momentumKGPerM2S=productionState.momentum;
								advanced.velocityMPerS=productionState.velocity;
								values.monitoredManifoldMaximumHistory.push_back(
									production.maximumAcceptedManifoldDeviation);
								values.monitoredManifoldP95History.push_back(
									production.acceptedManifoldDeviationP95);
								values.monitoredManifoldP50History.push_back(
									production.acceptedManifoldDeviationP50);
								values.monitoredTailCellHistory.push_back(
									production.manifoldTailCellCount);
								values.monitoredTailDrainedVolumeHistoryM3.push_back(
									production.manifoldTailDrainedVolumeM3);
								values.productionDeviceHistoryMS.push_back(deviceMS);
								values.productionWallHistoryMS.push_back(wallMS);
								if(floorInterceptedW>=0.0&&!persistence.productionOnsetDiagnosticDirectory.empty()){
									const std::filesystem::path floorPath=
										persistence.productionOnsetDiagnosticDirectory/"floor_radiation.csv";
									const bool floorHeader=!std::filesystem::exists(floorPath);
									std::ofstream floorLog(floorPath,std::ios::app);
									if(floorHeader)floorLog<<"step,beginning_time_s,absorbed_W,convected_W,maximum_wall_K,"
										"gas_rise_r015_030_K,gas_rise_r030_050_K,gas_rise_r050_070_K,gas_rise_r070_100_K\n";
									floorLog<<std::setprecision(17)<<values.productionWallHistoryMS.size()<<','<<
										simulationTimeS<<','<<floorInterceptedW<<','<<floorConvectedW<<','<<
										floorMaximumWallK;
									for(const double rise:floorBandRiseK)floorLog<<','<<rise;
									floorLog<<'\n';
								}
								if(!persistence.productionOnsetDiagnosticDirectory.empty()){
									std::ofstream trajectory(persistence.productionOnsetDiagnosticDirectory/
										"maximum_velocity_trajectory.csv",std::ios::app);
									trajectory<<std::setprecision(17)<<acceptedSteps+1u<<','<<
										simulationTimeS+trialStep<<','<<trialStep<<','<<attemptMaximumVelocity<<','<<
										attemptMaximumAxis<<','<<attemptMaximumFace<<','<<attemptMaximumX<<','<<
										attemptMaximumY<<','<<attemptMaximumZ<<','<<
										production.maximumAcceptedManifoldDeviation<<','<<
										production.acceptedManifoldDeviationP95<<','<<
										production.acceptedManifoldDeviationP50<<','<<
										production.manifoldTailCellCount<<','<<
										production.manifoldTailDrainedVolumeM3<<','<<deviceMS<<','<<wallMS<<','<<
										projectedHeunDiagnostics.ownerPublicationIdentity<<','<<
										projectedHeunDiagnostics.commandCommitCount<<','<<
										projectedHeunDiagnostics.residentProjectionInvocationCount<<','<<
										projectedHeunDiagnostics.acceptedPicardIterations[0]<<','<<
										projectedHeunDiagnostics.acceptedPicardIterations[1]<<','<<
										projectedHeunDiagnostics.acceptedPicardIterations[2]<<','<<
										projectedHeunDiagnostics.actualMetalAllocationBytes<<','<<
										projectedHeunDiagnostics.certifiedWorkingSetBytes<<','<<
										projectedHeunDiagnostics.residentProjectionDeviceElapsedMS<<','<<
										projectedHeunDiagnostics.residentNonprojectionDeviceElapsedMS<<','<<
										projectedHeunDiagnostics.intermediateSealFormat<<','<<
										projectedHeunDiagnostics.intermediateDigestVersion<<','<<
										projectedHeunDiagnostics.payloadDigestFormat<<','<<
										projectedHeunDiagnostics.payloadDigestVersion<<','<<
										projectedHeunDiagnostics.qualifiedKernelSetSHA256<<','<<
										projectedHeunDiagnostics.inputPayloadRootSHA256<<','<<
										acceptedPayloadRoot<<','<<
										projectedHeunDiagnostics.publicationPayloadBytes<<'\n';
									if(!ClosePublishedStream(trajectory)){advancedOK=false;mandatoryEvidenceFailure=true;
										error="production onset trajectory publication failed";}
									if(advancedOK&&persistence.productionOnsetStopVelocityMPerS>0.0&&
										attemptMaximumVelocity>=persistence.productionOnsetStopVelocityMPerS)
										productionOnsetStopReached=true;
								}
								if(!persistence.temporalSnapshotDirectory.empty()&&
									(acceptedSteps+1u)%256u==0u){
									double maximumVelocity=0.0,maximumTemperature=0.0,
										minimumGasDensity=std::numeric_limits<double>::infinity(),
										maximumGasDensity=0.0,
										minimumAuxiliaryFaceDensity=std::numeric_limits<double>::infinity(),
										maximumTransportedMomentum=0.0;
									for(const MethaneCellState& accepted:productionState.states){
										maximumTemperature=std::max(maximumTemperature,
											accepted.temperatureK);
										minimumGasDensity=std::min(minimumGasDensity,accepted.GasDensity());
										maximumGasDensity=std::max(maximumGasDensity,accepted.GasDensity());
									}
									for(const std::vector<double>& component:productionState.velocity.component)
										for(const double velocity:component)maximumVelocity=std::max(
											maximumVelocity,std::fabs(velocity));
									for(const std::vector<float>& component:
										production.transportedDual.auxiliaryFaceDensity)
										for(const float density:component)minimumAuxiliaryFaceDensity=
											std::min(minimumAuxiliaryFaceDensity,static_cast<double>(density));
									for(const std::vector<float>& component:production.transportedDual.momentum)
										for(const float value:component)maximumTransportedMomentum=
											std::max(maximumTransportedMomentum,std::fabs(static_cast<double>(value)));
									std::fprintf(stderr,"production accepted step=%u time=%.17g dt=%.17g "
										"dev=%.9g p95=%.9g tail=%u drain=%.9g velocity=%.9g "
										"temperature=%.9g min_gas=%.9g max_gas=%.9g lambda=%.9g min_aux_rho=%.9g "
										"max_transport_momentum=%.9g\n",
										acceptedSteps+1u,simulationTimeS+trialStep,trialStep,
										production.maximumAcceptedManifoldDeviation,
										production.acceptedManifoldDeviationP95,
										production.manifoldTailCellCount,
										production.manifoldTailDrainedVolumeM3,maximumVelocity,
										maximumTemperature,minimumGasDensity,maximumGasDensity,
										static_cast<double>(production.forceDiagnostics.outwardLambdaPerS),
										minimumAuxiliaryFaceDensity,maximumTransportedMomentum);
								}
								if(persistence.Test().r245ObservationFixture&&!persistence.Test().burnerLauncherFixture){
			values.structuredError="r245_observer_fixture_scope_conflict";return values;}
		// Ordinary tier 8 uses the same exact ledger without lateral-study frame
		// capsules or even-grid central columns.
		if(persistence.r245AcceptedBudget&&
									(!FireProductionOwnerConvergenceProbe::MatchesAcceptedEvent(projectedHeunRequest,
										simulationTimeS,trialStep,error)||
									!PrepareR245Budget(projectedHeunRequest,projectedHeunDiagnostics,acceptedSteps+1u,
										sourcePattern,persistence.r244LateralDouble&&simulationTimeS+trialStep>=nextTemporalSnapshotS,temporalSnapshotIndex,pendingBudget,error))){
									mandatoryEvidenceFailure=true;advancedOK=false;lastAdvanceError=error;break;}
								if(FastModeConservationDue(acceptedSteps)&&projectedHeunRequest.captureAcceptedBudgetFlux&&
									!FastModeConservationCheck(projectedHeunRequest,projectedHeunDiagnostics,
										acceptedSteps+1u,sourcePattern,error)){
									advancedOK=false;lastAdvanceError=error;break;}
								acceptedBeginningStates=std::move(packetBeginning);
								// Recompute the pilot ledger at the source producer's actual
								// evaluation temperature, not the legacy binary64 override.
								if(persistence.portedProductionTransport)
									for(std::size_t cell=0u;cell<acceptedBeginningStates.size();++cell)
										acceptedBeginningStates[cell].temperatureK=
											projectedHeunSource.BeginningTemperatureK()[cell];
								acceptedExternalFuelMassKG=persistence.portedProductionTransport?
									ProductionBurnerFuelDoseKG(projectedHeunRequest):externalFuelMassKG;
							}
						}else{
							advancedOK=false;
							if(reportCapstoneProgress||
								!persistence.temporalSnapshotDirectory.empty())std::fprintf(stderr,
								"production rejection candidate=%u physical_valid=%d terminal_valid=%d "
								"token=%d plateau_passed=%d dynamics_passed=%d next_available=%d "
								"represented_dt=%.17g suggested_dt=%.17g field_max=%.17g "
								"invocations=%u cycles=%u pre=%.17g post=%.17g band=%.17g error=%s\n",
								reduction,production.physicalProjection.validationPassed?1:0,
								production.projection.validationPassed?1:0,
								production.HasAcceptedManifoldToken()?1:0,
								production.manifoldPlateauPassed?1:0,
								production.manifoldDynamicsBoundPassed?1:0,
								production.manifoldNextTimeStepAvailable?1:0,
								static_cast<double>(production.representedTimeStepS),
								production.suggestedManifoldTimeStepS,
								production.maximumAcceptedManifoldDeviation,
								production.residentProjectionInvocationCount,
								production.projection.executedVCycleCount,
								production.projection.maximumPreProjectionResidualPerS,
								production.projection.maximumPostProjectionResidualPerS,
								production.projection.validationBandPerS,error.c_str());
							if(error.empty())error="production resident attempt was rejected";
							lastAdvanceError=error;break;
						}
						profileResidentMS=std::chrono::duration<double,std::milli>(
							std::chrono::steady_clock::now()-profileStageStart).count();
					}
					}else{
						advancedOK=packetOK&&AdvanceConservative3D(shape,beginning,momentum,packets,
							config,fuel,fuel,FireSimulationTransportRecord::OpenV1(),advanced,&error);
						if(advancedOK&&oracleComposition){
							std::ostringstream name;name<<"attempt_"<<acceptedSteps+1u<<'_'<<reduction;
							const auto base=persistence.oracleCompositionDirectory/name.str();
							const auto packetBytes=ProductionSourcePacketFieldBytes(packets);
							std::ofstream packetFile(base.string()+".source_packets.bin",std::ios::binary);
							packetFile.write(reinterpret_cast<const char*>(packetBytes.data()),packetBytes.size());
							packetFile.close();
							advancedOK=static_cast<bool>(packetFile)&&AppendOracleMomentumBudget(
								base.string()+".budget.csv",shape,momentum,config,advanced,simulationTimeS,error,30u,33u);
							if(!advancedOK){mandatoryEvidenceFailure=true;
								if(error.empty())error="oracle composition source/budget evidence failure";}
						}
						if(advancedOK)if(const char* oracleAuditPath=
							std::getenv("RISE_FIRE_ORACLE_MOMENTUM_AUDIT_PATH"))
							advancedOK=AppendOracleMomentumBudget(oracleAuditPath,shape,momentum,
								config,advanced,simulationTimeS,error);
					}
				solverPhase="production accepted-temperature validation";
				if(advancedOK) {
					std::vector<double> trialTemperature;
					if(persistence.productionMetal){
						trialTemperature=acceptedProductionTemperatureK;
						advancedOK=trialTemperature.size()==advanced.conservative.size();
					}else advancedOK=InvertPeriodicTemperaturesWithinBounds(advanced.conservative,fuel,
						config.transport.ambientTemperatureK,
						caseRecord.derived.maximumAcceptedTemperatureK,
						advanceOutputPrecision,trialTemperature,&error,workerCount);
					if(advancedOK&&std::any_of(trialTemperature.begin(),trialTemperature.end(),
						[&caseRecord](const double temperatureK){return !std::isfinite(temperatureK)||
							temperatureK>=caseRecord.derived.maximumAcceptedTemperatureK;})) {
						advancedOK=false;
						error="accepted_physical_temperature_ceiling_violation";
					}
				}
				if(!advancedOK) {
					if(fatalOnsetAuditFailure||mandatoryEvidenceFailure||
						observationEvidencePublished){lastAdvanceError=error;break;}
					if(error.empty())error=std::string("production owner failed during ")+
						solverPhase;
					lastAdvanceError=error;
					if(reportCapstoneProgress) std::fprintf(stderr,
						"capstone retry reduction=%u dt=%.9g reason=%s\n",reduction,
						trialStep,error.c_str());
					trialStep*=0.5; error.clear();
				}
			}
			if(!advancedOK) error=lastAdvanceError;
			else solverPhase="accepted-state accounting";
			profileStageStart=std::chrono::steady_clock::now();
			reaction.deltaTimeS=config.transport.deltaTimeS;
			if(advancedOK) {
				solverPhase="thread-identity validation";
				const bool checkLimiterIdentity=workerCount>1u&&
					advanced.discontinuousLimiterClassCount>0u&&
					!values.discontinuousClassThreadIdentityChecked;
				const bool checkActiveSetIdentity=workerCount>1u&&
					(advanced.discontinuousActiveSetClassCount>0u||
						persistence.Test().forceActiveSetIdentityCheckForTest)&&
					!values.activeSetThreadIdentityChecked;
				if(checkLimiterIdentity||checkActiveSetIdentity){
					ConservativeAdvance3DConfig serialConfig=config;
					serialConfig.workerCount=1u;
					ConservativeAdvance3DResult serialAdvanced;
					std::string serialError;
					const bool serialOK=AdvanceConservative3D(shape,beginning,momentum,packets,
						serialConfig,fuel,fuel,FireSimulationTransportRecord::OpenV1(),
						serialAdvanced,&serialError);
					bool identical=serialOK&&serialAdvanced.conservative.size()==
						advanced.conservative.size()&&serialAdvanced.faceAlpha==advanced.faceAlpha&&
						serialAdvanced.divergenceHeunPerS==advanced.divergenceHeunPerS&&
						serialAdvanced.maximumLimiterClassDiscrepancy==
							advanced.maximumLimiterClassDiscrepancy&&
						serialAdvanced.discontinuousLimiterClassCount==
							advanced.discontinuousLimiterClassCount&&
						serialAdvanced.maximumActiveSetComplementarityDiscrepancyMPerS==
							advanced.maximumActiveSetComplementarityDiscrepancyMPerS&&
						serialAdvanced.discontinuousActiveSetClassCount==
							advanced.discontinuousActiveSetClassCount&&
						serialAdvanced.maximumActiveSetCycleLength==
							advanced.maximumActiveSetCycleLength&&
						serialAdvanced.maximumActiveSetDifferingFaceCount==
							advanced.maximumActiveSetDifferingFaceCount;
					for(std::size_t cell=0;identical&&cell<advanced.conservative.size();++cell)
						for(std::size_t component=0;component<MethaneConservativeDimension;++component)
							identical=identical&&serialAdvanced.conservative[cell][component]==
								advanced.conservative[cell][component];
					for(unsigned int axis=0;axis<3;++axis)identical=identical&&
						serialAdvanced.momentumKGPerM2S.component[axis]==
							advanced.momentumKGPerM2S.component[axis]&&
						serialAdvanced.velocityMPerS.component[axis]==
							advanced.velocityMPerS.component[axis];
					if(checkActiveSetIdentity&&
						persistence.Test().injectActiveSetIdentityMismatchForTest)identical=false;
					if(checkLimiterIdentity){
						values.discontinuousClassThreadIdentity=identical;
						values.discontinuousClassThreadIdentityChecked=true;
					}
					if(checkActiveSetIdentity){
						values.activeSetThreadIdentity=identical;
						values.activeSetThreadIdentityChecked=true;
					}
					if(!identical&&reportCapstoneProgress)std::fprintf(stderr,
						"capstone discontinuous-class 1-vs-N mismatch: %s\n",
						serialError.c_str());
					advancedOK=DiscontinuousThreadIdentityAccepted(identical,
						checkActiveSetIdentity,error);
				}
				if(!advancedOK)break;
				values.maximumLimiterClassDiscrepancy=std::max(
					values.maximumLimiterClassDiscrepancy,
					advanced.maximumLimiterClassDiscrepancy);
				values.discontinuousLimiterClassSteps+=
					advanced.discontinuousLimiterClassCount;
				values.maximumActiveSetComplementarityDiscrepancyMPerS=std::max(
					values.maximumActiveSetComplementarityDiscrepancyMPerS,
					advanced.maximumActiveSetComplementarityDiscrepancyMPerS);
				values.discontinuousActiveSetEvents+=
					advanced.discontinuousActiveSetClassCount;
				values.maximumActiveSetCycleLength=std::max(
					values.maximumActiveSetCycleLength,advanced.maximumActiveSetCycleLength);
				values.maximumActiveSetDifferingFaceCount=std::max(
					values.maximumActiveSetDifferingFaceCount,
					advanced.maximumActiveSetDifferingFaceCount);
				solverPhase="pilot-ledger recomputation";
				double expectedStepPilotEnergyJ=0.0;
				for(std::size_t cell=0;cell<shape.CellCount();++cell){
					double pilotEnergyJPerM3=0.0;
					if(!ComputeMethanePilotEnergyDeltaJPerM3(acceptedBeginningStates[cell],fuel,
						pilotSetpointTemperatureK[cell],
						reactions[cell].pilotExpansionVolumeRatioCap,
						pilotEnergyJPerM3,&error)){
						advancedOK=false;break;
					}
					expectedStepPilotEnergyJ+=pilotEnergyJPerM3*cellVolume;
				}
				if(!advancedOK)break;
				solverPhase="accepted-state reconstruction";
				const double priorMaximumTemperatureK=values.maximumTemperatureK;
				std::vector<double> acceptedTemperature;
				if(persistence.productionMetal){
					acceptedTemperature=acceptedProductionTemperatureK;
					advancedOK=acceptedTemperature.size()==advanced.conservative.size();
				}else advancedOK=InvertPeriodicTemperaturesWithinBounds(advanced.conservative,fuel,
					config.transport.ambientTemperatureK,
					caseRecord.derived.maximumAcceptedTemperatureK,
					advanceOutputPrecision,acceptedTemperature,&error,workerCount);
				const bool measurePilotApproach=!values.pilotApproachComplete&&
					simulationTimeS<pilotEndS;
				const bool holdPhase=simulationTimeS>=pilotRampEndS&&simulationTimeS<pilotEndS;
				bool allActiveHoldCellsQualified=true,activeHoldCellObserved=false;
				bool allPilotLedgerCeilingsQualified=true;
				double stepAcceptedEOSMaximum=0.0;
				double stepAcceptedMaximumTemperatureK=0.0;
				double heldPilotMinimumTemperatureK=std::numeric_limits<double>::infinity();
				double heldPilotMaximumTemperatureK=0.0;
				std::size_t heldPilotMinimumCell=0u,heldPilotMaximumCell=0u;
				std::vector<MethaneCellState> acceptedStates;
				acceptedStates.reserve(shape.CellCount());
				for(std::size_t acceptedCell=0;advancedOK&&acceptedCell<advanced.conservative.size();
					++acceptedCell) {
					const ConservativeVector& conservative=advanced.conservative[acceptedCell];
					MethaneCellState accepted=FromConservativeVector(conservative,
						advanceOutputPrecision);
					accepted.temperatureK=acceptedTemperature[acceptedCell];
					double acceptedEOSResidual=0.0;
					if(!EquationOfStateResidual(accepted,fuel,acceptedEOSResidual,&error)){
						advancedOK=false;break;
					}
					stepAcceptedEOSMaximum=std::max(stepAcceptedEOSMaximum,acceptedEOSResidual);
					stepAcceptedMaximumTemperatureK=std::max(
						stepAcceptedMaximumTemperatureK,accepted.temperatureK);
					if(measurePilotApproach)values.maximumPilotApproachEOSResidual=std::max(
						values.maximumPilotApproachEOSResidual,acceptedEOSResidual);
					values.maximumTemperatureK=std::max(values.maximumTemperatureK,
						accepted.temperatureK);
					if(pilotSetpointTemperatureK[acceptedCell]>0.0){
						MethanePilotProjectionMap pilotMap;
						if(!ComputeMethanePilotProjectionMap(acceptedBeginningStates[acceptedCell],fuel,
							pilotSetpointTemperatureK[acceptedCell],
							reactions[acceptedCell].pilotExpansionVolumeRatioCap,pilotMap,&error)){
							advancedOK=false;break;
						}
						const bool abovePilotCeiling=
							acceptedBeginningStates[acceptedCell].temperatureK>=
								caseRecord.derived.pilotSetpointTemperatureK;
						allPilotLedgerCeilingsQualified=allPilotLedgerCeilingsQualified&&
							packets[acceptedCell].pilotEnergyDeltaJPerM3==
								pilotMap.sensibleEnergyDeltaJPerM3&&
							packets[acceptedCell].pilotExpansionIntegral==pilotMap.expansionIntegral&&
							(abovePilotCeiling?
								packets[acceptedCell].pilotEnergyDeltaJPerM3==0.0:
								pilotMap.targetTemperatureK<=caseRecord.derived.pilotSetpointTemperatureK);
						if(holdPhase&&pilotMap.targetTemperatureK==
							caseRecord.derived.pilotSetpointTemperatureK){
							activeHoldCellObserved=true;
							if(accepted.temperatureK<heldPilotMinimumTemperatureK){
								heldPilotMinimumTemperatureK=accepted.temperatureK;
								heldPilotMinimumCell=acceptedCell;
							}
							if(accepted.temperatureK>heldPilotMaximumTemperatureK){
								heldPilotMaximumTemperatureK=accepted.temperatureK;
								heldPilotMaximumCell=acceptedCell;
							}
							allActiveHoldCellsQualified=allActiveHoldCellsQualified&&
								accepted.temperatureK>fuel.PilotTemperatureK();
						}
					}
					acceptedStates.push_back(accepted);
				}
				if(advancedOK){
					states=std::move(acceptedStates);
					config.transport.producerPrecision=advanceOutputPrecision;
					values.maximumAcceptedEOSResidual=std::max(
						values.maximumAcceptedEOSResidual,stepAcceptedEOSMaximum);
					values.acceptedMaximumEOSResidualHistory.push_back(stepAcceptedEOSMaximum);
					values.acceptedMaximumTemperatureHistoryK.push_back(
						stepAcceptedMaximumTemperatureK);
					if(activeHoldCellObserved){
						values.pilotApproachComplete=true;
						values.pilotHoldBandObserved=true;
						values.pilotHoldBandSatisfied=values.pilotHoldBandSatisfied&&
							allActiveHoldCellsQualified;
						if(values.minimumActiveHoldTemperatureK==0.0)
							values.minimumActiveHoldTemperatureK=heldPilotMinimumTemperatureK;
						else values.minimumActiveHoldTemperatureK=std::min(
							values.minimumActiveHoldTemperatureK,heldPilotMinimumTemperatureK);
						values.maximumActiveHoldTemperatureK=std::max(
							values.maximumActiveHoldTemperatureK,heldPilotMaximumTemperatureK);
					}
				}
				if(advancedOK&&!allPilotLedgerCeilingsQualified){
					advancedOK=false;error="pilot_ledger_ceiling_violation";break;
				}
				if(advancedOK&&!values.pilotHoldBandSatisfied){
					std::ostringstream message;message.precision(17);
					message<<"pilot_hold_gate_violation:min="<<heldPilotMinimumTemperatureK<<
						" min_cell="<<heldPilotMinimumCell<<" min_xyz="<<heldPilotMinimumCell%shape.nx<<
						','<<(heldPilotMinimumCell/shape.nx)%shape.ny<<','<<
						heldPilotMinimumCell/(shape.nx*shape.ny)<<
						" max="<<heldPilotMaximumTemperatureK<<" max_cell="<<heldPilotMaximumCell<<
						" max_xyz="<<heldPilotMaximumCell%shape.nx<<','<<
						(heldPilotMaximumCell/shape.nx)%shape.ny<<','<<
						heldPilotMaximumCell/(shape.nx*shape.ny)<<
						" max_T0="<<currentTemperature[heldPilotMaximumCell]<<
						" max_command="<<pilotSetpointTemperatureK[heldPilotMaximumCell]<<
						" max_reacted="<<packets[heldPilotMaximumCell].reactedFuelKGPerM3<<
						" max_qgas="<<packets[heldPilotMaximumCell].gasHeatReleaseWPerM3<<
						" max_pilot_dH="<<packets[heldPilotMaximumCell].pilotEnergyDeltaJPerM3;
					advancedOK=false;error=message.str();break;
				}
				values.expectedPilotEnergyJ+=expectedStepPilotEnergyJ;
				values.externalFuelMassKG+=acceptedExternalFuelMassKG;
				if(reportCapstoneProgress&&values.maximumTemperatureK>
					std::max(2250.0,priorMaximumTemperatureK)) {
					const std::size_t hottest=static_cast<std::size_t>(std::max_element(
						acceptedTemperature.begin(),acceptedTemperature.end())-
						acceptedTemperature.begin());
					std::fprintf(stderr,"capstone new physical peak cell=%zu xyz=%zu,%zu,%zu "
						"T0=%.9g T1=%.9g pilot=%.9g reacted=%.9g qgas=%.9g dt=%.9g\n",
						hottest,hottest%shape.nx,(hottest/shape.nx)%shape.ny,
						hottest/(shape.nx*shape.ny),currentTemperature[hottest],
						acceptedTemperature[hottest],pilotSetpointTemperatureK[hottest],
						packets[hottest].reactedFuelKGPerM3,
						packets[hottest].gasHeatReleaseWPerM3,reaction.deltaTimeS);
				}
				profilePostMS=std::chrono::duration<double,std::milli>(
					std::chrono::steady_clock::now()-profileStageStart).count();
				profileStageStart=std::chrono::steady_clock::now();
				solverPhase="accepted-state statistics";
				momentum=advanced.momentumKGPerM2S;
				double stepHeatReleaseW=0.0,stepRadiativeLossW=0.0;
				double stepFuelConsumptionKGPerS=0.0,centerlineHeatReleaseW=0.0;
				for(const MethaneSourcePacket& acceptedPacket:packets) {
					values.pilotEnergyJ+=acceptedPacket.pilotEnergyDeltaJPerM3*cellVolume;
					stepHeatReleaseW+=acceptedPacket.gasHeatReleaseWPerM3*cellVolume;
					stepRadiativeLossW+=acceptedPacket.radiativeCoolingWPerM3*cellVolume;
					stepFuelConsumptionKGPerS+=-acceptedPacket.constituentDelta[MethaneCH4]*
						cellVolume/reaction.deltaTimeS;
					if(acceptedPacket.reactedFuelKGPerM3>0.0) {
						if(simulationTimeS<pilotEndS) values.ignitedDuringPilot=true;
						else values.sustainedAfterPilot=true;
					}
				}
				for(std::size_t z=0;z<shape.nz;++z)for(std::size_t cy=0;cy<centerYCount;++cy)
					for(std::size_t cx=0;cx<centerXCount;++cx){const std::size_t center=
						centerXIndex[cx]+shape.nx*(centerYIndex[cy]+shape.ny*z);
						centerlineHeatReleaseW+=packets[center].gasHeatReleaseWPerM3*cellVolume/
							centerSampleCount;}
				const double stepEndS=simulationTimeS+reaction.deltaTimeS;
				// The r271 inert plume never ignites.
				if(stepEndS>=pilotEndS&&!values.ignitedDuringPilot&&!FireCase::InertPlumeSelected()) {
					advancedOK=false;
					error="pilot_window_expired_without_ignition";
					break;
				}
				if(persistence.Test().r245ObservationFixture&&!persistence.Test().burnerLauncherFixture){
			values.structuredError="r245_observer_fixture_scope_conflict";return values;}
		if(persistence.r245AcceptedBudget&&!PublishR245Budget(persistence.productionOnsetDiagnosticDirectory,
					caseRecord.caseRecordId,currentBuildId,currentExecutableDigest,values.statisticsStartS,
					{{shape.nx,shape.ny,shape.nz}},static_cast<float>(shape.cellWidthM),pendingBudget,r245Ledger,error)){
					advancedOK=false;break;}
				const double statisticsDuration=std::max(0.0,stepEndS-
					std::max(simulationTimeS,values.statisticsStartS));
				if(statisticsDuration>0.0) {
					if(!values.statisticsBoundaryObserved) {
						values.statisticsBoundaryObserved=true;
						values.firstStatisticsStepStartS=simulationTimeS;
					}
					values.integratedHeatReleaseJ+=stepHeatReleaseW*statisticsDuration;
					values.integratedRadiativeLossJ+=stepRadiativeLossW*statisticsDuration;
					values.integratedFuelConsumptionKG+=stepFuelConsumptionKGPerS*statisticsDuration;
					values.probeTimeS.push_back(stepEndS);
					values.probeCenterlineHeatReleaseW.push_back(centerlineHeatReleaseW);
					centerlineStatisticsDurationS+=statisticsDuration;
					for(std::size_t z=0;z<shape.nz;++z){
						double stationTemperatureK=0.0,stationVelocityMPerS=0.0;
						double stationReactionWPerM3=0.0;
						for(std::size_t cy=0;cy<centerYCount;++cy)for(std::size_t cx=0;
							cx<centerXCount;++cx){const std::size_t center=centerXIndex[cx]+
								shape.nx*(centerYIndex[cy]+shape.ny*z);
							stationTemperatureK+=states[center].temperatureK/centerSampleCount;
							stationReactionWPerM3+=packets[center].gasHeatReleaseWPerM3/
								centerSampleCount;
							const std::size_t stationLower=OpenLowerFaceForCell3D(shape,center,2),
								stationUpper=OpenUpperFaceForCell3D(shape,center,2);
							stationVelocityMPerS+=0.5*(advanced.velocityMPerS.component[2][stationLower]+
								advanced.velocityMPerS.component[2][stationUpper])/centerSampleCount;
							centerlineTemperatureIntegral[z]+=states[center].temperatureK*
								statisticsDuration/centerSampleCount;
							const std::size_t lower=OpenLowerFaceForCell3D(shape,center,2),
								upper=OpenUpperFaceForCell3D(shape,center,2);
							centerlineVelocityIntegral[z]+=0.5*(advanced.velocityMPerS.component[2][lower]+
								advanced.velocityMPerS.component[2][upper])*statisticsDuration/
								centerSampleCount;
						}
						values.stationProbeTimeS.push_back(stepEndS);
						values.stationProbeHeightM.push_back((static_cast<double>(z)+0.5)*
							shape.cellWidthM);
						values.stationProbeTemperatureK.push_back(stationTemperatureK);
						values.stationProbeReactionWPerM3.push_back(stationReactionWPerM3);
						values.stationProbeVerticalVelocityMPerS.push_back(stationVelocityMPerS);
						for(std::size_t y=0;y<shape.ny;++y)for(std::size_t x=0;x<shape.nx;++x){
							const std::size_t planeCell=x+shape.nx*(y+shape.ny*z);
							planeHeatReleaseIntegral[z]+=packets[planeCell].gasHeatReleaseWPerM3*
								cellVolume*statisticsDuration;
						}
					}
				}
				profileStatisticsMS=std::chrono::duration<double,std::milli>(
					std::chrono::steady_clock::now()-profileStageStart).count();
				values.acceptedTimeStepHistoryS.push_back(reaction.deltaTimeS);
				if(FireProfileEnabled()){
					std::fprintf(stderr,"FIREPROFSTEP step=%u dt=%.17g wall_ms=%.3f "
						"eligibility_ms=%.3f transport_ms=%.3f control_ms=%.3f source_ms=%.3f "
						"target_ms=%.3f layout_ms=%.3f resident_ms=%.3f post_ms=%.3f statistics_ms=%.3f\n",
						acceptedSteps+1u,reaction.deltaTimeS,std::chrono::duration<double,std::milli>(
							std::chrono::steady_clock::now()-profileStepStart).count(),profileEligibilityMS,
						profileTransportMS,profileControlMS,profileSourceMS,profileTargetMS,
						profileLayoutMS,profileResidentMS,profilePostMS,profileStatisticsMS);
					FireProfileReportAndReset("step");
				}
				simulationTimeS+=reaction.deltaTimeS;previousStepS=reaction.deltaTimeS;++acceptedSteps;
				values.acceptedTimeStepS=reaction.deltaTimeS;
				values.simulatedTimeS=simulationTimeS;
				if(persistence.acceptedStepObserver)persistence.acceptedStepObserver(values,acceptedSteps);
				bool hostMemoryStop=false;
				if(hostMemoryMonitored){
					auto memory=FireRunHostMemory::Read();
					if(persistence.Test().hostMemoryRetentionRED&&acceptedSteps>=4u){
						if(acceptedSteps==4u){hostMemoryPolicy.ceiling=
							std::max(memory.rss,memory.footprint)+(UINT64_C(96)<<20u);
							hostMemoryPolicy.reserve=UINT64_C(32)<<20u;}
						syntheticRetainedPayloads.emplace_back(std::size_t(16u)<<20u,
							static_cast<unsigned char>(acceptedSteps));
						const auto& retained=syntheticRetainedPayloads.back();
						const std::string retainedSHA=RISECBOR64::SHA256Hex(retained);
						std::fprintf(stderr,"HOST_MEMORY_SYNTHETIC_RETENTION step=%u blocks=%zu block_bytes=%zu block_sha256=%s\n",
							acceptedSteps,syntheticRetainedPayloads.size(),retained.size(),retainedSHA.c_str());
						memory=FireRunHostMemory::Read();
					}
					hostMemoryStop=hostMemoryPolicy.Stop(memory)||persistence.Test().hostMemoryResumeStopRED;
					if(!hostMemoryPath.empty()){
						std::ofstream output(hostMemoryPath,std::ios::app);
						output<<std::setprecision(17)<<acceptedSteps<<','<<simulationTimeS<<','<<memory.rss<<','
							<<memory.footprint<<','<<memory.compressed<<','<<hostMemoryPolicy.ceiling<<','
							<<hostMemoryPolicy.reserve<<','<<(hostMemoryStop?1:0)<<'\n';
						if(!ClosePublishedStream(output)){advancedOK=false;error="host_memory_evidence_failure";break;}
					}
					std::fprintf(stderr,"HOST_MEMORY step=%u time=%.17g rss_bytes=%llu footprint_bytes=%llu compressed_bytes=%llu ceiling_bytes=%llu reserve_bytes=%llu stop=%d\n",
						acceptedSteps,simulationTimeS,static_cast<unsigned long long>(memory.rss),
						static_cast<unsigned long long>(memory.footprint),static_cast<unsigned long long>(memory.compressed),
						static_cast<unsigned long long>(hostMemoryPolicy.ceiling),
						static_cast<unsigned long long>(hostMemoryPolicy.reserve),hostMemoryStop?1:0);
				}
				if(persistence.stopRequested&&persistence.stopRequested->load())cooperativeStopObserved=true;
				const bool moreWork=!productionOnsetStopReached&&!cooperativeStopObserved&&
					(acceptedSteps<effectiveMinimumStepCount||simulationTimeS<targetTimeS);
				const bool checkpointRequestedNow=persistence.checkpointRequested&&
					persistence.checkpointRequested->exchange(false);
				const double checkpointElapsedS=std::chrono::duration<double>(
					std::chrono::steady_clock::now()-lastCheckpointWall).count();
				const bool retainedSnapshotDue=persistence.retainedSnapshotEvery>0u&&
					!persistence.retainedSnapshotDirectory.empty()&&
					(acceptedSteps-values.resumedFromStep)%persistence.retainedSnapshotEvery==0u;
				const bool equivalenceSnapshotDue=retainedSnapshotDue||
					!persistence.equivalenceSnapshotDirectory.empty();
				const bool temporalSnapshotDue=
					!persistence.temporalSnapshotDirectory.empty()&&
					persistence.temporalSnapshotCadenceS>0.0&&
					simulationTimeS>=nextTemporalSnapshotS;
				const bool finalCheckpointDue=(!moreWork||hostMemoryStop||(persistence.finalCheckpointAtWindowEnd&&
					simulationTimeS>=windowEndTimeS))&&!persistence.finalCheckpointPath.empty();
				if(temporalSnapshotDue){
					solverPhase="temporal-frame publication";
					if(!writeTemporalFrame()){advancedOK=false;break;}
					nextTemporalSnapshotS=(persistence.hasTemporalFrameOrigin?
						persistence.temporalFrameOriginS:values.statisticsStartS)+
						static_cast<double>(temporalSnapshotIndex)*
						persistence.temporalSnapshotCadenceS;
				}
				// M2 track C: the solver adapter's rendezvous (may block; see FireRunDriver.h).
				if(persistence.acceptedStepRendezvous){
					FireRunStepRendezvous rendezvous;
					rendezvous.acceptedSteps=acceptedSteps;rendezvous.simulationTimeS=simulationTimeS;
					rendezvous.framePublished=temporalSnapshotDue;
					rendezvous.frameIndex=temporalSnapshotDue?temporalSnapshotIndex-1u:0u;
					rendezvous.lastStep=!moreWork||simulationTimeS>=windowEndTimeS;
					rendezvous.values=&values;rendezvous.checkpoint=&serializeAcceptedState;
					if(!persistence.acceptedStepRendezvous(rendezvous)){
						advancedOK=false;error="run abandoned by its host";break;}
				}
				const bool isolatedResourceCheckpoint=hostMemoryStop&&persistence.isolatedEquivalenceProbe&&
					!persistence.productionOnsetDiagnosticDirectory.empty();
				const bool checkpointDue=equivalenceSnapshotDue||finalCheckpointDue||isolatedResourceCheckpoint||
					(hostMemoryStop&&!persistence.isolatedEquivalenceProbe&&!persistence.checkpointPath.empty())||
					(moreWork&&!persistence.isolatedEquivalenceProbe&&!persistence.checkpointPath.empty()&&
						(persistence.checkpointCadenceWallS<=0.0||
							checkpointElapsedS>=persistence.checkpointCadenceWallS||checkpointRequestedNow));
				if(checkpointDue){
					solverPhase="checkpoint publication";
					values.checkpointStepIndices.push_back(acceptedSteps);
					MethaneRunCheckpoint checkpoint;
					checkpoint.caseRecordId=caseRecord.caseRecordId;
					checkpoint.producerBuildId=currentBuildId;
					checkpoint.dimensions={{shape.nx,shape.ny,shape.nz}};
					checkpoint.cellWidthM=shape.cellWidthM;checkpoint.states=std::move(states);
					checkpoint.momentum=std::move(momentum);
					checkpoint.velocity=std::move(advanced.velocityMPerS);
					checkpoint.values=std::move(values);
					checkpoint.centerlineTemperatureIntegral=std::move(centerlineTemperatureIntegral);
					checkpoint.centerlineVelocityIntegral=std::move(centerlineVelocityIntegral);
					checkpoint.planeHeatReleaseIntegral=std::move(planeHeatReleaseIntegral);
					checkpoint.centerlineStatisticsDurationS=centerlineStatisticsDurationS;
					checkpoint.simulationTimeS=simulationTimeS;
					checkpoint.previousStepS=previousStepS;
					checkpoint.lastAcceptedStepS=reaction.deltaTimeS;
					checkpoint.acceptedSteps=acceptedSteps;
					checkpoint.productionManifoldObservation=productionManifoldObservation;
					std::filesystem::path checkpointOutput=finalCheckpointDue?
						persistence.finalCheckpointPath:persistence.checkpointPath;
					if(equivalenceSnapshotDue){
						std::ostringstream snapshotName;snapshotName<<"step_"<<std::setw(2)<<
							std::setfill('0')<<(acceptedSteps-values.resumedFromStep)<<".checkpoint";
						checkpointOutput=(retainedSnapshotDue?persistence.retainedSnapshotDirectory:
							persistence.equivalenceSnapshotDirectory)/snapshotName.str();
					}
					if(isolatedResourceCheckpoint)checkpointOutput=
						persistence.productionOnsetDiagnosticDirectory.parent_path()/"host_memory_stop.checkpoint";
					FireStateProducerPrecision checkpointOutputPrecision=
						FireStateProducerPrecision::Unknown;
					const bool checkpointAuthorized=
						HomogeneousStateProducerPrecision(checkpoint.states,checkpointOutputPrecision)&&
						(checkpointOutputPrecision!=FireStateProducerPrecision::Binary64||
							IssueBinary64CheckpointAuthority(checkpoint));
					const bool checkpointSaved=checkpointAuthorized&&
						(!persistence.isolatedEquivalenceProbe||checkpointOutput!=persistence.checkpointPath)&&
						(persistence.UsesProjectedHeunOwner()?SavePublishedCheckpoint(checkpointOutput,
							checkpoint,isolatedResourceCheckpoint?PayloadPublicationPolicy::Immutable:
							CheckpointPublicationPolicy(checkpointOutput,persistence.checkpointPath,
								finalCheckpointDue,equivalenceSnapshotDue),error):
							SaveMethaneRunCheckpoint(checkpointOutput,checkpoint,error));
					bool retainedCheckpointSaved=true;
					std::filesystem::path retainedCheckpointOutput;
					if(checkpointSaved&&!persistence.retainedCheckpointDirectory.empty()&&
						checkpointOutput==persistence.checkpointPath){
						std::error_code retentionDirectoryError;
						std::filesystem::create_directories(
							persistence.retainedCheckpointDirectory,retentionDirectoryError);
						std::ostringstream retainedName;retainedName<<"step_"<<std::setw(10)<<
							std::setfill('0')<<acceptedSteps<<".checkpoint";
						retainedCheckpointOutput=
							persistence.retainedCheckpointDirectory/retainedName.str();
						if(retentionDirectoryError){error="cannot create retained checkpoint directory";
							retainedCheckpointSaved=false;
						}else if(persistence.UsesProjectedHeunOwner()){
							retainedCheckpointSaved=SavePublishedCheckpoint(retainedCheckpointOutput,
								checkpoint,PayloadPublicationPolicy::Immutable,error);
						}else if(std::filesystem::exists(retainedCheckpointOutput)){
							retainedCheckpointSaved=DigestFile(retainedCheckpointOutput)==
								DigestFile(checkpointOutput);
							if(!retainedCheckpointSaved)error=
								"retained checkpoint step identity already has different bytes";
						}else retainedCheckpointSaved=SaveMethaneRunCheckpoint(
							retainedCheckpointOutput,checkpoint,error);
					}
					if((!checkpointSaved||!retainedCheckpointSaved)&&(reportCapstoneProgress||
						!persistence.temporalSnapshotDirectory.empty())){
						std::uint64_t diagnosticStateDigest=0u;
						const bool stateMatches=checkpoint.productionManifoldObservation.Available()&&
							CheckpointAcceptedStateMatchesObservation(checkpoint,
								checkpoint.productionManifoldObservation,diagnosticStateDigest);
						std::fprintf(stderr,"checkpoint refusal authorized=%d timeline=%d observation=%d "
							"state_matches=%d state_digest=%llu error=%s\n",checkpointAuthorized?1:0,
							AcceptedCheckpointTimelineValid(checkpoint)?1:0,
							checkpoint.productionManifoldObservation.Available()?1:0,
							stateMatches?1:0,
							static_cast<unsigned long long>(diagnosticStateDigest),error.c_str());
					}
					states=std::move(checkpoint.states);momentum=std::move(checkpoint.momentum);
					advanced.velocityMPerS=std::move(checkpoint.velocity);
					values=std::move(checkpoint.values);
					centerlineTemperatureIntegral=std::move(checkpoint.centerlineTemperatureIntegral);
					centerlineVelocityIntegral=std::move(checkpoint.centerlineVelocityIntegral);
					planeHeatReleaseIntegral=std::move(checkpoint.planeHeatReleaseIntegral);
					if(!checkpointSaved||!retainedCheckpointSaved){
						advancedOK=false;break;
					}
					lastCheckpointWall=std::chrono::steady_clock::now();
					if(reportCapstoneProgress)std::fprintf(stderr,
						"capstone durable checkpoint count=%zu step=%u time=%.17g path=%s\n",
						values.checkpointStepIndices.size(),acceptedSteps,simulationTimeS,
						checkpointOutput.string().c_str());
					if(reportCapstoneProgress&&!retainedCheckpointOutput.empty())std::fprintf(stderr,
						"capstone retained checkpoint step=%u time=%.17g path=%s\n",acceptedSteps,
						simulationTimeS,retainedCheckpointOutput.string().c_str());
					if(persistence.Test().killAfterFirstCheckpoint&&persistence.Test().killProcess)persistence.Test().killProcess();
				}
				if(hostMemoryStop){advancedOK=false;
					error=checkpointDue?"host_memory_ceiling_checkpoint_sealed":"host_memory_ceiling_no_checkpoint_destination";
					break;}
				if(reportCapstoneProgress && (acceptedSteps<=4u || acceptedSteps%10u==0u ||
					simulationTimeS>=targetTimeS)) {
					double maximumTemperatureK=0.0,maximumReactionWPerM3=0.0;
					for(std::size_t diagnosticCell=0;diagnosticCell<states.size();++diagnosticCell) {
						maximumTemperatureK=std::max(maximumTemperatureK,
							states[diagnosticCell].temperatureK);
						maximumReactionWPerM3=std::max(maximumReactionWPerM3,
							packets[diagnosticCell].gasHeatReleaseWPerM3);
					}
					std::fprintf(stderr,"capstone accepted step=%u time=%.9g dt=%.9g Tmax=%.9g "
						"qmax=%.9g eos_max=%.9g approach_eos_max=%.9g limiter_class=%s "
						"limiter_discrepancy=%.9g active_set_class=%s active_set_discrepancy=%.9g "
						"active_set_cycle=%zu active_set_faces=%zu\n",acceptedSteps,
						simulationTimeS,reaction.deltaTimeS,maximumTemperatureK,
						maximumReactionWPerM3,values.maximumAcceptedEOSResidual,
						values.maximumPilotApproachEOSResidual,
						advanced.discontinuousLimiterClassCount?
						"discontinuous":"continuous",advanced.maximumLimiterClassDiscrepancy,
						advanced.discontinuousActiveSetClassCount?
						"discontinuous":"continuous",
						advanced.maximumActiveSetComplementarityDiscrepancyMPerS,
						advanced.maximumActiveSetCycleLength,
						advanced.maximumActiveSetDifferingFaceCount);
				}
			}
		}
		if(!advancedOK) {
			if(error.empty())error=std::string("production solver failed during ")+solverPhase;
			values.structuredError="solver_failure:"+error;
			std::fprintf(stderr,"capstone solver diagnostic steps=%u time=%.17g: %s\n",
				acceptedSteps,simulationTimeS,values.structuredError.c_str());
			if(!values.acceptedMaximumEOSResidualHistory.empty()){
				std::fprintf(stderr,"capstone accepted EOS drift history tail=");
				const std::size_t first=values.acceptedMaximumEOSResidualHistory.size()>8u?
					values.acceptedMaximumEOSResidualHistory.size()-8u:0u;
				for(std::size_t sample=first;sample<values.acceptedMaximumEOSResidualHistory.size();
					++sample)std::fprintf(stderr,"%s%.9g",sample==first?"":",",
						values.acceptedMaximumEOSResidualHistory[sample]);
				std::fprintf(stderr,"\n");
			}
			return values;
		}
		if(reportCapstoneProgress&&advancedOK) {
			double pilotMaximumTemperatureK=0.0,pilotMaximumMethaneKGPerM3=0.0,
				pilotMaximumOxygenKGPerM3=0.0;
			for(std::size_t cell=0;cell<states.size();++cell) if(canonicalPilotMask[cell]) {
				pilotMaximumTemperatureK=std::max(pilotMaximumTemperatureK,
					states[cell].temperatureK);
				pilotMaximumMethaneKGPerM3=std::max(pilotMaximumMethaneKGPerM3,
					states[cell].constituent[MethaneCH4]);
				pilotMaximumOxygenKGPerM3=std::max(pilotMaximumOxygenKGPerM3,
					states[cell].constituent[MethaneO2]);
			}
			std::fprintf(stderr,"capstone pilot diagnostic Tmax=%.9g CH4max=%.9g O2max=%.9g "
				"eos_max=%.9g approach_eos_max=%.9g\n",
				pilotMaximumTemperatureK,pilotMaximumMethaneKGPerM3,
				pilotMaximumOxygenKGPerM3,values.maximumAcceptedEOSResidual,
				values.maximumPilotApproachEOSResidual);
		}
		values.succeeded=true;
		if(!persistence.productionMetal)Check(advanced.conservative.empty() ||
			advanced.effectiveWorkerCount==std::max(1u,std::min(workerCount,
			static_cast<unsigned int>(shape.CellCount()))),
			"capstone one-vs-N fixture selects the owning solver worker path");
		if(advancedOK) {
			values.dimensions={{shape.nx,shape.ny,shape.nz}}; values.cellWidthM=shape.cellWidthM;
			values.caseRecordId=caseRecord.caseRecordId;
			values.temperature.resize(shape.CellCount());
			values.reaction.resize(shape.CellCount());
			values.carbon.resize(shape.CellCount());
			values.velocity.resize(shape.CellCount());
			for(std::size_t cell=0;cell<shape.CellCount();++cell) {
				const MethaneCellState& accepted=states[cell];
				values.temperature[cell]=static_cast<float>(accepted.temperatureK);
				values.reaction[cell]=static_cast<float>(packets.empty()?0.0:
					packets[cell].gasHeatReleaseWPerM3);
				values.carbon[cell]=static_cast<float>(std::max(0.0,
					accepted.constituent[MethaneCarbon]));
				std::array<float,3> velocity={{0.0f,0.0f,0.0f}};
				for(unsigned int axis=0;axis<3&&!advanced.velocityMPerS.component[axis].empty();++axis) {
					const std::size_t lower=OpenLowerFaceForCell3D(shape,cell,axis);
					const std::size_t upper=OpenUpperFaceForCell3D(shape,cell,axis);
					velocity[axis]=static_cast<float>(0.5*(advanced.velocityMPerS.component[axis][lower]+
						advanced.velocityMPerS.component[axis][upper]));
				}
				values.velocity[cell]=velocity;
				if(!packets.empty()) {
					values.realizedHeatReleaseW+=packets[cell].gasHeatReleaseWPerM3*cellVolume;
					values.fuelConsumptionKGPerS+=-packets[cell].constituentDelta[MethaneCH4]*
						cellVolume/reaction.deltaTimeS;
				}
			}
			values.temperatureK=*std::max_element(values.temperature.begin(),values.temperature.end());
			values.reactionWPerM3=*std::max_element(values.reaction.begin(),values.reaction.end());
			values.acceptedEscapeFactor=escape.accepted;
			values.selectedTimeStepS=selectedStep.seconds;
			values.acceptedTimeStepS=reaction.deltaTimeS;
			values.simulatedTimeS=simulationTimeS;
			values.flowThroughTimeS=caseRecord.derived.flowThroughTimeS;
			double coolingW=0.0;
			for(const MethaneSourcePacket& packet:packets)
				coolingW+=packet.radiativeCoolingWPerM3*cellVolume;
			values.realizedRadiativeFraction=values.realizedHeatReleaseW>0.0?
				coolingW/values.realizedHeatReleaseW:0.0;
			values.integratedRadiativeFraction=values.integratedHeatReleaseJ>0.0?
				values.integratedRadiativeLossJ/values.integratedHeatReleaseJ:0.0;
			values.puffingFrequencyHz=DominantUniformResampledFrequency(values.probeTimeS,
				values.probeCenterlineHeatReleaseW);
			const double expectedPuffing=1.5/std::sqrt(CapstonePoolDiameterM);
			values.puffingRelativeError=expectedPuffing>0.0?
				std::fabs(values.puffingFrequencyHz-expectedPuffing)/expectedPuffing:0.0;
			// The existing plane integral covers every accepted source packet in the
			// statistics window. Export it directly, without sampling VDB frames.
			if(persistence.r245AcceptedBudget&&centerlineStatisticsDurationS>0.0){
				std::ofstream profile(persistence.productionOnsetDiagnosticDirectory/"heat_release_height.csv");
				profile<<std::setprecision(17)<<"z,lower_m,upper_m,center_m,statistics_duration_s,integrated_heat_J,mean_heat_W\n";
				for(std::size_t z=0;z<shape.nz;++z)
					profile<<z<<','<<z*shape.cellWidthM<<','<<(z+1)*shape.cellWidthM<<','
						<<(z+0.5)*shape.cellWidthM<<','<<centerlineStatisticsDurationS<<','
						<<planeHeatReleaseIntegral[z]<<','<<planeHeatReleaseIntegral[z]/centerlineStatisticsDurationS<<'\n';
				if(!ClosePublishedStream(profile)){
					values.succeeded=false;values.structuredError="heat_release_height_write_failed";return values;
				}
			}
			double peakMeanPlaneHeatReleaseW=0.0;
			for(const double integral:planeHeatReleaseIntegral)peakMeanPlaneHeatReleaseW=
				std::max(peakMeanPlaneHeatReleaseW,centerlineStatisticsDurationS>0.0?
					integral/centerlineStatisticsDurationS:0.0);
			if(peakMeanPlaneHeatReleaseW>0.0)for(std::size_t z=0;z<shape.nz;++z)if(
				planeHeatReleaseIntegral[z]/centerlineStatisticsDurationS>=
					0.01*peakMeanPlaneHeatReleaseW)values.mccaffreyFlameTipHeightM=
					(static_cast<double>(z)+0.5)*shape.cellWidthM;
			for(std::size_t z=0;z<shape.nz;++z) {
				double fallbackTemperature=0.0,fallbackVelocity=0.0;
				for(std::size_t cy=0;cy<centerYCount;++cy)for(std::size_t cx=0;
					cx<centerXCount;++cx){const std::size_t center=centerXIndex[cx]+
						shape.nx*(centerYIndex[cy]+shape.ny*z);
					fallbackTemperature+=states[center].temperatureK/centerSampleCount;
					fallbackVelocity+=values.velocity[center][2]/centerSampleCount;}
				values.centerlineHeightM.push_back((static_cast<double>(z)+0.5)*shape.cellWidthM);
				values.centerlineTemperatureK.push_back(centerlineStatisticsDurationS>0.0?
					centerlineTemperatureIntegral[z]/centerlineStatisticsDurationS:
					fallbackTemperature);
				values.centerlineVelocityMPerS.push_back(centerlineStatisticsDurationS>0.0?
					centerlineVelocityIntegral[z]/centerlineStatisticsDurationS:
					fallbackVelocity);
			}
			FitCenterlineTemperaturePowerLaw(values);
			if(heatReleaseRateKW==CapstoneHeatReleaseRateKW)
				EvaluateMcCaffreyPlumeStations(values,heatReleaseRateKW);
		}
		return values;
	}

	bool ReadTextMember(const RISECBOR64::Value& map,const char* key,std::string& value)
	{
		const RISECBOR64::Value* found=map.Find(key);
		if(!found||found->GetType()!=RISECBOR64::Value::Text)return false;
		value=found->GetText();return true;
	}
	bool ReadUnsignedMember(const RISECBOR64::Value& map,const char* key,std::uint64_t& value)
	{
		const RISECBOR64::Value* found=map.Find(key);
		if(!found||found->GetType()!=RISECBOR64::Value::UnsignedInteger)return false;
		value=found->GetIntegerArgument();return true;
	}
	bool ReadUnsignedArray(const RISECBOR64::Value& map,const char* key,
		std::vector<std::uint64_t>& values)
	{
		const RISECBOR64::Value* found=map.Find(key);
		if(!found||found->GetType()!=RISECBOR64::Value::Array)return false;
		values.clear();values.reserve(found->GetArray().size());
		for(const RISECBOR64::Value& item:found->GetArray()){
			if(item.GetType()!=RISECBOR64::Value::UnsignedInteger)return false;
			values.push_back(item.GetIntegerArgument());
		}
		return true;
	}
	bool ReadTextArray(const RISECBOR64::Value& map,const char* key,
		std::vector<std::string>& values)
	{
		const RISECBOR64::Value* found=map.Find(key);
		if(!found||found->GetType()!=RISECBOR64::Value::Array)return false;
		values.clear();values.reserve(found->GetArray().size());
		for(const RISECBOR64::Value& item:found->GetArray()){
			if(item.GetType()!=RISECBOR64::Value::Text)return false;
			values.push_back(item.GetText());
		}
		return true;
	}

	bool ParseResumeCertificatePayload(const RISECBOR64::Value& payload,
		ResumeEquivalenceCertificate& certificate)
	{
		std::string kind;std::uint64_t schema=0u;
		return payload.GetType()==RISECBOR64::Value::Map&&payload.GetMap().size()==13u&&
			ReadUnsignedMember(payload,"accepted_step_count",certificate.acceptedStepCount)&&
			ReadTextMember(payload,"checkpoint_sha256",certificate.checkpointDigest)&&
			ReadTextArray(payload,"frame_sha256",certificate.frameDigests)&&
			ReadUnsignedArray(payload,"maximum_eos_residual_bits",certificate.maximumEOSResidualBits)&&
			ReadUnsignedArray(payload,"maximum_temperature_K_bits",certificate.maximumTemperatureBits)&&
			ReadTextMember(payload,"new_build_id",certificate.newBuildId)&&
			ReadTextMember(payload,"new_executable_sha256",certificate.newExecutableDigest)&&
			ReadTextMember(payload,"old_build_id",certificate.oldBuildId)&&
			ReadTextMember(payload,"old_executable_sha256",certificate.oldExecutableDigest)&&
			ReadTextMember(payload,"record_kind",kind)&&kind=="fire-resume-equivalence-certificate-v1"&&
			ReadUnsignedMember(payload,"resumed_from_step",certificate.resumedFromStep)&&
			ReadUnsignedMember(payload,"schema_version",schema)&&schema==1u&&
			ReadUnsignedArray(payload,"time_step_s_bits",certificate.timeStepBits);
	}
	bool LoadResumeEquivalenceCertificate(const std::filesystem::path& path,
		ResumeEquivalenceCertificate& certificate,std::string& error)
	{
		certificate=ResumeEquivalenceCertificate();
		const RISECBOR64::Bytes bytes=ReadFileBytes(path);RISECBOR64::Value envelope;
		if(bytes.empty()||!RISECBOR64::DecodeCanonical(bytes,envelope,&error)||
			envelope.GetType()!=RISECBOR64::Value::Map||envelope.GetMap().size()!=2u){
			error="resume-equivalence certificate envelope is invalid";return false;
		}
		const RISECBOR64::Value* payload=envelope.Find("payload");
		const RISECBOR64::Value* id=envelope.Find("certificate_id");RISECBOR64::Bytes payloadBytes;
		if(!payload||!id||id->GetType()!=RISECBOR64::Value::Text||
			!RISECBOR64::Encode(*payload,payloadBytes,&error)||
			id->GetText()!=RISECBOR64::SHA256Hex(payloadBytes)||
			!(ParseResumeCertificatePayload(*payload,certificate)||
				ParseResidentResumeCertificateV2(*payload,certificate,error))){
			error="resume-equivalence certificate is not canonical or self-consistent";return false;
		}
		certificate.certificateId=id->GetText();
		const bool valid=certificate.schemaVersion==2u||(certificate.acceptedStepCount>=8u&&
			certificate.timeStepBits.size()==certificate.acceptedStepCount&&
			certificate.maximumTemperatureBits.size()==certificate.acceptedStepCount&&
			certificate.maximumEOSResidualBits.size()==certificate.acceptedStepCount&&
			certificate.frameDigests.size()==certificate.acceptedStepCount);
		if(!valid)error="resume-equivalence certificate evidence is incomplete";
		return valid;
	}
	}
}
