//////////////////////////////////////////////////////////////////////
//
//  FireCPUBackend.cpp - the CPU backend of the fire.methane production step
//  (simulation M8, part A).  See FireCPUBackend.h.
//
//  Compiled without fast-math or FP contraction like the fire driver and the
//  CPU owner it calls (build/make/rise/Makefile per-file rules).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FireCPUBackend.h"
#include "FireRunHostMemory.h"
#include "../../Utilities/FireCase.h"
#include "../../Utilities/FireFastMode.h"
#include "../../Utilities/FireProductionTransport.h"
#include "../../Utilities/FireSimulationRecords.h"
#include "../../Utilities/RISECBOR64.h"
#include "../../../../tools/fire_simulator_core.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <iomanip>
#include <string>
#include <thread>
#include <vector>

namespace RISE
{
	namespace
	{
		bool Fail(std::string* error,const std::string& message)
		{
			if(error)*error=message;
			return false;
		}

		bool EnvironmentIs(const char* name,const char* value)
		{
			const char* text=std::getenv(name);
			return text&&std::strcmp(text,value)==0;
		}

		// Runs body(begin,end) over [0,count) in `workers` contiguous slices; every
		// slice writes disjoint outputs, so the result does not depend on `workers`.
		template<class Body>
		void ParallelSlices(const std::size_t count,const unsigned int workers,const Body& body)
		{
			const unsigned int threads=static_cast<unsigned int>(std::max<std::size_t>(1u,
				std::min<std::size_t>(workers==0u?1u:workers,count/4096u+1u)));
			if(threads<=1u){body(std::size_t(0),count);return;}
			std::vector<std::thread> pool;pool.reserve(threads-1u);
			const std::size_t slice=(count+threads-1u)/threads;
			for(unsigned int t=1u;t<threads;++t){
				const std::size_t begin=std::min(count,t*slice),end=std::min(count,begin+slice);
				pool.emplace_back([&body,begin,end](){body(begin,end);});
			}
			body(std::size_t(0),std::min(count,slice));
			for(std::thread& thread:pool)thread.join();
		}

		// The resident transport authority on the CPU: Vreman eddy viscosity and the
		// effective diffusivity/conductivity of the stage state, plus the molecular
		// kinematic viscosity the stress operator consumes.  Same evaluation as the
		// r190 calibration reference (tests/FireSequenceTest.cpp ResidentOwnerTransportFP32),
		// which the Metal transport authority is qualified against.
		class CPUTransportProvider final : public FireProductionProjectedHeunTransportProvider
		{
		public:
			CPUTransportProvider(const FireProductionResidentPhysicalFluxComparatorRequest& fixture,
				const unsigned int workers) : fixture_(fixture),workers_(workers==0u?1u:workers) {}
			bool Evaluate(const FireProductionProjectedHeunTransportContext& context,
				FireProductionProjectedHeunTransportCoefficients& result,std::string* error) const override
			{
				using namespace RISE::FireSim;
				const FireProductionProjectionShape& sourceShape=fixture_.transport.shape;
				const std::size_t cells=sourceShape.CellCount();
				if(!context.conservativeValues||context.conservativeValues->size()!=9u*cells||
					!context.temperatureK||context.temperatureK->size()!=cells||
					!context.projectedVelocityMPerS)return Fail(error,"CPU fire transport context is malformed");
				PeriodicMACShape shape;shape.nx=sourceShape.nx;shape.ny=sourceShape.ny;
				shape.nz=sourceShape.nz;shape.cellWidthM=sourceShape.cellWidthM;
				std::vector<ConservativeVector> state(cells);
				ParallelSlices(cells,workers_,[&](const std::size_t begin,const std::size_t end){
					for(std::size_t cell=begin;cell<end;++cell)for(std::size_t component=0u;component<9u;++component)
						state[cell][component]=(*context.conservativeValues)[component*cells+cell];});
				std::vector<double> temperature(context.temperatureK->begin(),context.temperatureK->end());
				OpenMACField3D velocity;for(unsigned int axis=0u;axis<3u;++axis)
					velocity.component[axis].assign((*context.projectedVelocityMPerS)[axis].begin(),
						(*context.projectedVelocityMPerS)[axis].end());
				OpenBoundaryConfig3D boundary;
				for(unsigned int side=0u;side<6u;++side)boundary.kind[side]=
					fixture_.transport.boundary[side]==FireProductionProjectionPressureOpen?
						PressureOpenBoundary3D:AdiabaticWallBoundary3D;
				for(std::size_t component=0u;component<9u;++component){
					boundary.ambientState[component]=fixture_.ambient[component];
					boundary.injectedState[component]=fixture_.ambient[component];}
				boundary.ambientDensityKGPerM3=0.0;for(std::size_t component=1u;component<=6u;++component)
					boundary.ambientDensityKGPerM3+=fixture_.ambient[component];
				boundary.injectedGasDensityKGPerM3=boundary.ambientDensityKGPerM3;
				boundary.fuelMassFluxKGPerM2S=0.0;boundary.velocityToleranceMPerS=0.0;
				boundary.pressureTolerancePa=1.0e-3;
				if(boundary.kind[4]==AdiabaticWallBoundary3D){
					boundary.bottomFuelMask.resize(fixture_.transport.fuelInletBoundaryFace[4].size());
					for(std::size_t face=0u;face<boundary.bottomFuelMask.size();++face)
						boundary.bottomFuelMask[face]=fixture_.transport.fuelInletBoundaryFace[4][face]!=0u;
					boundary.bottomFuelMassFluxKGPerM2S.assign(boundary.bottomFuelMask.size(),0.0);}
				if(!fixture_.bottomFuelMassFluxKGPerM2S.empty()){
					boundary.bottomFuelMassFluxKGPerM2S.assign(fixture_.bottomFuelMassFluxKGPerM2S.begin(),
						fixture_.bottomFuelMassFluxKGPerM2S.end());
					boundary.injectedGasDensityKGPerM3=0.0;
					for(std::size_t component=0u;component<9u;++component){
						boundary.injectedState[component]=fixture_.injectedConservative[component];
						if(component>=1u&&component<=6u)boundary.injectedGasDensityKGPerM3+=
							fixture_.injectedConservative[component];}
				}
				std::vector<CellMolecularTransportEvaluation> molecular;
				std::vector<double> diffusivity,conductivity,effectiveViscosity;
				if(!BuildCellMolecularTransportEvaluations3D(state,temperature,
					FireSimulationMethaneRecord::PhysicalV1(),FireSimulationTransportRecord::OpenV1(),
					FireStateProducerPrecision::Binary32,molecular,error,workers_))return false;
				if(!BuildOpenStageTransport3D(shape,state,temperature,velocity,boundary,false,
					FireSimulationMethaneRecord::PhysicalV1(),FireSimulationTransportRecord::OpenV1(),
					FireStateProducerPrecision::Binary32,diffusivity,conductivity,effectiveViscosity,error,
					workers_,&molecular))return false;
				result.stage=context.stage;result.attemptIdentity=context.attemptIdentity;
				result.parentCandidateIdentity=context.parentCandidateIdentity;
				result.projectionIdentity=context.projectionIdentity;
				result.diffusivityM2PerS.assign(diffusivity.begin(),diffusivity.end());
				result.conductivityWPerMK.assign(conductivity.begin(),conductivity.end());
				result.molecularKinematicViscosityM2PerS.resize(cells);
				for(std::size_t cell=0u;cell<cells;++cell){double density=0.0;
					for(std::size_t component=1u;component<=6u;++component)density+=state[cell][component];
					result.molecularKinematicViscosityM2PerS[cell]=static_cast<float>(
						molecular[cell].molecularViscosityPaS/density);}
				result.publicationIdentity=FireProductionProjectedHeunTransportPublicationIdentity(
					context,result);
				if(error)error->clear();return true;
			}
		private:
			const FireProductionResidentPhysicalFluxComparatorRequest& fixture_;
			unsigned int workers_;
		};

		// The Metal owner request (what the driver builds) -> the CPU owner request.
		// The scalar, physical and force contracts carry the same boundary, ambient,
		// burner and record-derived operands the Metal owner derives internally.
		bool BuildCPUOwnerRequest(const FireProductionProjectedHeunMetalOwnerRequest& metal,
			FireProductionProjectedHeunOwnerRequest& owner,std::string* error)
		{
			owner=FireProductionProjectedHeunOwnerRequest();
			const auto& eos=metal.lineage.eos;const auto& flux=eos.physicalFlux;
			const FireProductionProjectionShape& shape=flux.transport.shape;
			const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
			owner.attemptIdentity=metal.lineage.frozenSource.AttemptIdentity();
			owner.source=metal.lineage.frozenSource;
			owner.caseRecordEnvelope=eos.caseRecordEnvelope;
			owner.beginningConservativeValues=flux.transport.conservativeValues;
			owner.scalarContract.shape=shape;owner.physicalContract.shape=shape;owner.forceContract.shape=shape;
			owner.scalarContract.timeStepS=eos.candidateTimeStepS;owner.forceContract.timeStepS=eos.candidateTimeStepS;
			owner.scalarContract.boundary=flux.transport.boundary;
			owner.physicalContract.boundary=flux.transport.boundary;
			owner.forceContract.boundary=flux.transport.boundary;
			owner.scalarContract.ambient=flux.ambient;owner.physicalContract.ambient=flux.ambient;
			owner.scalarContract.nullity=flux.nullity;
			owner.scalarContract.nullspaceBasis=flux.nullspaceBasis;
			owner.scalarContract.coordinateProjector=flux.coordinateProjector;
			std::array<double,7> minimum{},maximum{};std::string recordError;
			if(!fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(fuel.TemperatureMinK(),minimum.data(),
				minimum.size(),&recordError)||!fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(
				fuel.TemperatureMaxK(),maximum.data(),maximum.size(),&recordError))
				return Fail(error,"CPU fire owner enthalpy bounds are unavailable: "+recordError);
			for(std::size_t species=0u;species<7u;++species){
				owner.scalarContract.enthalpyBoundsJPerKG[species]=static_cast<float>(minimum[species]);
				owner.scalarContract.enthalpyBoundsJPerKG[7u+species]=static_cast<float>(maximum[species]);}
			const FireAcceptedStateFeasibilityEnvelope& envelope=fuel.AcceptedStateFeasibilityEnvelope();
			owner.scalarContract.feasibilityFactor=static_cast<float>(envelope.kappaEpsilon32*
				static_cast<double>(std::numeric_limits<float>::epsilon()));
			owner.scalarContract.assemblyReserveFactor=static_cast<float>(envelope.remapFactorEpsilon32*
				static_cast<double>(std::numeric_limits<float>::epsilon()));
			owner.physicalContract.ambientTemperatureK=flux.ambientTemperatureK;
			owner.forceContract.ambientDensityKGPerM3=metal.ambientDensityKGPerM3;
			owner.forceContract.vremanCoefficient=metal.vremanCoefficient;
			owner.forceContract.gravityMPerS2=metal.gravityMPerS2;
			owner.beginningMomentumKGPerM2S=metal.beginningMomentumKGPerM2S;
			owner.projectionTolerancePerS=metal.projectionTolerancePerS;
			owner.endpointVelocityToleranceMPerS=metal.endpointVelocityToleranceMPerS;
			owner.maximumPicardIterations=metal.maximumPicardIterations;
			if(!flux.bottomFuelMassFluxKGPerM2S.empty()){
				owner.scalarContract.bottomFuelMask=flux.transport.fuelInletBoundaryFace[4];
				owner.physicalContract.bottomFuelMask=owner.scalarContract.bottomFuelMask;
				owner.scalarContract.bottomFuelMassFluxKGPerM2S=flux.bottomFuelMassFluxKGPerM2S;
				owner.physicalContract.bottomFuelMassFluxKGPerM2S=flux.bottomFuelMassFluxKGPerM2S;
				owner.scalarContract.injectedConservative=flux.injectedConservative;
				owner.physicalContract.injectedConservative=flux.injectedConservative;
			}
			// r264: the Metal owner's default carrier; RISE_FIRE_LIMITED_MOMENTUM_CARRIER=1
			// selects the accepted (limited) carrier on both backends.
			owner.centeredMomentumCarrier=!FireSim::FireLimitedMomentumCarrier();
			// The resident owner's projection schedule (17 V-cycles with an open boundary,
			// doubled while validation fails), not the standalone 16.
			owner.residentProjectionSchedule=true;
			owner.workingSetLimitBytes=FireRun::FireCPUBackendHostCeilingBytes();
			if(owner.workingSetLimitBytes==0u)return Fail(error,"CPU fire owner: host memory size is unknown");
			return true;
		}

		bool HexToBytes(const std::string& hex,unsigned char* out)
		{
			if(hex.size()!=64u)return false;
			auto nibble=[](const char c)->int{return c>='0'&&c<='9'?c-'0':(c>='a'&&c<='f'?c-'a'+10:
				(c>='A'&&c<='F'?c-'A'+10:-1));};
			for(std::size_t i=0u;i<32u;++i){const int high=nibble(hex[2u*i]),low=nibble(hex[2u*i+1u]);
				if(high<0||low<0)return false;out[i]=static_cast<unsigned char>((high<<4)|low);}
			return true;
		}

		std::string FloatsSHA256(const std::vector<float>& values)
		{
			return RISECBOR64::SHA256Hex(reinterpret_cast<const unsigned char*>(values.data()),
				values.size()*sizeof(float));
		}
	}

	namespace FireRun
	{
		bool FireCPUBackendRequested()
		{
			return EnvironmentIs("RISE_FIRE_BACKEND","cpu");
		}

		bool FireCPUBackendOptionsSupported(std::string& reason)
		{
			// The Metal parsers' own enabling values (FireProductionForceMac.mm,
			// FireProductionAdvectionMac.mm); any other value leaves Metal at its default.
			if(EnvironmentIs("RISE_FIRE_COMPACT_VISCOUS_STENCIL","1")){
				reason="the CPU fire backend does not implement the r271 compact viscous stencil "
					"(compact_viscous_stencil, and near_wall_wale, hyper_vreman, vreman_zone that need it)";
				return false;}
			if(FireCase::ProductionScalarVremanOverride()>=0.0f){
				reason="the CPU fire backend does not implement the scalar Vreman coefficient override (scalar_vreman_cv)";
				return false;}
			if(EnvironmentIs("RISE_FIRE_FCT_ALPHA_ONE","1")||EnvironmentIs("RISE_FIRE_FCT_ALPHA_ONE","0")){
				reason="the CPU fire backend does not implement the forced FCT alpha (fct_alpha)";return false;}
			if(EnvironmentIs("RISE_FIRE_SCALAR_SLOPE","central")||EnvironmentIs("RISE_FIRE_SCALAR_SLOPE","superbee")){
				reason="the CPU fire backend does not implement the scalar slope variants (scalar_slope)";return false;}
			if(EnvironmentIs("RISE_FIRE_WALL_SLOPE","linear")){
				reason="the CPU fire backend does not implement the linear wall slope (wall_slope)";return false;}
			reason.clear();return true;
		}

		bool FireCPUBackendWorkingSetBytes(const FireProductionProjectionShape& shape,std::uint64_t& bytes)
		{
			bytes=0u;std::uint64_t owner=0u;
			if(!FireProductionProjectedHeunCPUOwnerWorkingSetBytes(shape,owner,true))return false;
			const std::uint64_t cells=shape.CellCount();
			const std::uint64_t faces=FireProductionProjectionFaceCount(shape,0u)+
				FireProductionProjectionFaceCount(shape,1u)+FireProductionProjectionFaceCount(shape,2u);
			// The adapter's copy of the owner request (state, source seal and momentum), the
			// transport provider's binary64 stage copies (state, temperature, velocity,
			// three coefficient fields and the per-cell transport records), the published
			// result and the fast-mode budget packet.
			const std::uint64_t request=(9u*cells+40u*cells+3u*faces)*sizeof(float);
			const std::uint64_t transport=(9u+1u+3u)*cells*sizeof(double)+3u*faces*sizeof(double)+
				cells*256u;
			const std::uint64_t publication=(9u*cells+12u*faces+4u*cells)*sizeof(float)+
				(26u*faces)*sizeof(float);
			if(owner>UINT64_MAX-request-transport-publication)return false;
			bytes=owner+request+transport+publication;
			return true;
		}

		std::uint64_t FireCPUBackendHostCeilingBytes()
		{
			const std::uint64_t physical=FireRunHostMemory::PhysicalBytes();
			return physical==0u?0u:std::min(physical/2u,UINT64_C(32)<<30u);
		}

		bool FireCPUBackendAdmits(const FireProductionProjectionShape& shape,std::string& reason)
		{
			std::uint64_t bytes=0u;
			if(!FireCPUBackendWorkingSetBytes(shape,bytes)){reason="the CPU fire step working set cannot be "
				"computed for this grid";return false;}
			const std::uint64_t ceiling=FireCPUBackendHostCeilingBytes();
			if(ceiling==0u){reason="the CPU fire backend needs the host memory size, unknown here";return false;}
			// The production host policy also reserves 2 GiB for scratch and checkpoints.
			if(bytes>ceiling||ceiling-bytes<(UINT64_C(2)<<30u)){std::ostringstream out;
				out<<std::setprecision(3)<<"the CPU fire step needs "<<static_cast<double>(bytes)/1073741824.0
					<<" GiB plus 2 GiB of scratch, above this host's "<<static_cast<double>(ceiling)/1073741824.0
					<<" GiB ceiling (half of physical memory, at most 32 GiB)";
				reason=out.str();return false;}
			reason.clear();return true;
		}
	}

	bool AttemptFireProductionProjectedHeunResidentStepCPU(
		const FireProductionProjectedHeunMetalOwnerRequest& request,
		FireProductionResidentStepResult& result,
		FireProductionProjectedHeunMetalOwnerResult* diagnostics,
		const unsigned int workers,std::string* error )
	{
		result=FireProductionResidentStepResult();
		if(diagnostics)*diagnostics=FireProductionProjectedHeunMetalOwnerResult();
		const auto wallStart=std::chrono::steady_clock::now();
		try{
			std::string reason;
			if(!FireRun::FireCPUBackendOptionsSupported(reason))return Fail(error,reason);
			const FireProductionProjectionShape& shape=request.lineage.eos.physicalFlux.transport.shape;
			const std::size_t cells=shape.CellCount();
			if(!FireRun::FireCPUBackendAdmits(shape,reason))return Fail(error,reason);
			FireProductionProjectedHeunOwnerRequest ownerRequest;
			if(!BuildCPUOwnerRequest(request,ownerRequest,error))return false;
			CPUTransportProvider transport(request.lineage.eos.physicalFlux,workers);
			FireProductionProjectedHeunOwnerResult owner;
			{
				FireProductionProjectedHeunCPUOwner cpu;
				FireProductionCPUParallelScope parallel(workers);
				if(!cpu.Begin(ownerRequest,error)||!cpu.SolveR0(transport,error)||
					!cpu.SolveR1(transport,error)||!cpu.SolveR2(transport,owner,error))
					return false;
			}
			ownerRequest=FireProductionProjectedHeunOwnerRequest();
			if(!owner.accepted||owner.OwnerIdentity()==0u||owner.conservativeValues.size()!=9u*cells)
				return Fail(error,"CPU projected-Heun owner terminal payload is invalid");
			// Represented pressure ratio and its absolute deviation from one, per cell, as the
			// Metal owner publishes them for the monitored manifold diagnostics.
			std::vector<float> ratio(cells),deviation(cells);bool ratioValid=true;
			std::vector<unsigned char> ratioOK(cells,1u);
			ParallelSlices(cells,workers,[&](const std::size_t begin,const std::size_t end){
				for(std::size_t cell=begin;cell<end;++cell){std::array<double,9> tuple={{}};
					for(std::size_t component=0u;component<9u;++component)tuple[component]=
						static_cast<double>(owner.conservativeValues[component*cells+cell]);
					double value=0.0;
					if(!FireSimulationMethaneRecord::PhysicalV1().AcceptedConservativeVolumeRatioByComponentOrder(
						tuple.data(),tuple.size(),FireStateProducerPrecision::Binary32,value,0)||!std::isfinite(value)){
						ratioOK[cell]=0u;continue;}
					ratio[cell]=static_cast<float>(value);deviation[cell]=std::fabs(ratio[cell]-1.0f);}});
			for(const unsigned char ok:ratioOK)ratioValid=ratioValid&&ok!=0u;
			if(!ratioValid)return Fail(error,"CPU projected-Heun owner represented pressure ratio is invalid");

			FireProductionResidentStepResult computed;
			computed.conservativeValues=owner.conservativeValues;
			computed.projection=owner.r2.projection;computed.physicalProjection=owner.r2.projection;
			computed.transportedDual.momentum=owner.heunProvisionalMomentumKGPerM2S;
			computed.transportedDual.auxiliaryFaceDensity=owner.r2.projection.faceDensityKGPerM3;
			computed.cellSubmapCount=2u;computed.dualSubmapCount=2u;computed.sourceCommandCommitCount=0u;
			// Bootstrap plus accepted iterations of R0, R1 and R2 (and any canonical cycle projections).
			computed.residentProjectionInvocationCount=3u+owner.r0.acceptedIterationCount+
				owner.r1.acceptedIterationCount+owner.r2.acceptedIterationCount+
				owner.r0.activeSetCanonicalProjectionCount+owner.r1.activeSetCanonicalProjectionCount+
				owner.r2.activeSetCanonicalProjectionCount;
			computed.interstageFullGridTransferCount=0u;computed.terminalStagingCount=1u;
			FireRun::FireCPUBackendWorkingSetBytes(shape,computed.combinedCertifiedWorkingSetBytes);
			computed.combinedActualMetalAllocationBytes=0u;
			computed.projectedHeunOwnerIdentity=owner.OwnerIdentity();
			computed.deviceElapsedMS=0.0;computed.deviceMakespanMS=0.0;
			computed.representedTimeStepS=request.lineage.eos.candidateTimeStepS;
			std::vector<double> signedDeviation(cells),ordered(cells);
			for(std::size_t cell=0u;cell<cells;++cell){const double magnitude=static_cast<double>(deviation[cell]);
				ordered[cell]=magnitude;signedDeviation[cell]=std::copysign(magnitude,
					static_cast<double>(ratio[cell])-1.0);}
			std::sort(ordered.begin(),ordered.end());
			computed.maximumAcceptedManifoldDeviation=ordered.empty()?0.0:ordered.back();
			computed.acceptedManifoldDeviationP50=ordered[(ordered.size()-1u)/2u];
			computed.acceptedManifoldDeviationP95=ordered[static_cast<std::size_t>(
				std::floor(0.95*static_cast<double>(ordered.size()-1u)))];
			FireProductionManifoldTailTarget tail;
			if(!DeriveFireProductionManifoldTailTarget(signedDeviation,
				static_cast<double>(computed.representedTimeStepS),shape.cellWidthM,tail,error))return false;
			computed.manifoldTailRestorationApplied=tail.outlierCellCount>0u;
			computed.manifoldTailCellCount=tail.outlierCellCount;
			computed.manifoldTailExcessSum=tail.excessSum;
			computed.manifoldTailDrainedVolumeM3=tail.drainedVolumeM3;
			computed.manifoldDynamicsBoundPassed=computed.maximumAcceptedManifoldDeviation<=0x1p-2;
			computed.maximumManifoldGeneration=0.0;computed.maximumPredictedAdvectiveManifoldAnomaly=0.0;
			computed.manifoldStageGeneration={{0.0,0.0,0.0}};
			computed.manifoldMapCellCount=static_cast<std::uint32_t>(cells);
			computed.manifoldScalarDeviceToHostTransferCount=1u;
			computed.manifoldFullGridDeviceToHostTransferCount=0u;
			computed.advectiveAnomalyClosurePassCount=0u;
			computed.manifoldDiagnosticsMonitored=true;computed.manifoldPlateauEnforced=false;
			computed.manifoldAllowanceExceeded=computed.maximumAcceptedManifoldDeviation>((1.0-0x1p-2)*0x1p-5);
			computed.manifoldCeilingExceeded=computed.maximumAcceptedManifoldDeviation>0x1p-5;
			computed.manifoldPlateauPassed=computed.manifoldDynamicsBoundPassed;
			computed.manifoldGenerationAuthoritative=false;
			computed.requiredRestorationDrainFraction=0.0;computed.deliveredRestorationDrainFraction=0.0;
			computed.restorationResidualBandPerS=0.0;computed.manifoldNextTimeStepAvailable=false;
			computed.suggestedManifoldTimeStepS=0.0;
			computed.conservativeProducerPrecision=FireStateProducerPrecision::Binary32;
			computed.acceptedShape=shape;
			if(!FireProductionResidentStepEligibleForAcceptedManifoldToken(computed)){
				std::ostringstream diagnostic;diagnostic<<std::setprecision(17)
					<<"CPU projected-Heun owner acceptance contract failed"
					<<" projection_valid="<<computed.projection.validationPassed
					<<" dynamics_valid="<<computed.manifoldDynamicsBoundPassed
					<<" max_deviation="<<computed.maximumAcceptedManifoldDeviation
					<<" tail_cells="<<computed.manifoldTailCellCount;
				return Fail(error,diagnostic.str());}
			// The publication root binds the accepted state the token carries.
			std::vector<float> published=computed.conservativeValues;
			for(unsigned int axis=0u;axis<3u;++axis){
				published.insert(published.end(),computed.projection.momentumKGPerM2S[axis].begin(),
					computed.projection.momentumKGPerM2S[axis].end());
				published.insert(published.end(),computed.projection.velocityMPerS[axis].begin(),
					computed.projection.velocityMPerS[axis].end());}
			const std::string publicationRoot=FloatsSHA256(published);
			published.clear();published.shrink_to_fit();
			const std::string inputRoot=FloatsSHA256(request.lineage.eos.physicalFlux.transport.conservativeValues);
			FireProductionAcceptedManifoldToken& token=computed.acceptedManifoldToken_;
			token.available_=true;
			token.representedTimeStepS_=computed.representedTimeStepS;
			token.maximumGeneration_=computed.maximumManifoldGeneration;
			token.maximumAcceptedDeviation_=computed.maximumAcceptedManifoldDeviation;
			token.acceptedDeviationP95_=computed.acceptedManifoldDeviationP95;
			token.acceptedDeviationP50_=computed.acceptedManifoldDeviationP50;
			token.tailRestorationApplied_=computed.manifoldTailRestorationApplied;
			token.tailCellCount_=computed.manifoldTailCellCount;
			token.tailExcessSum_=computed.manifoldTailExcessSum;
			token.tailDrainedVolumeM3_=computed.manifoldTailDrainedVolumeM3;
			token.dynamicsBoundPassed_=computed.manifoldDynamicsBoundPassed;
			token.requiredDrainFraction_=0.0;token.deliveredDrainFraction_=0.0;token.maximumPostResidualPerS_=0.0;
			token.physicalMaximumPreResidualPerS_=computed.physicalProjection.maximumPreProjectionResidualPerS;
			token.physicalMaximumPostResidualPerS_=computed.physicalProjection.maximumPostProjectionResidualPerS;
			token.payloadDigest_=FireSim::FireFastMode()?UINT64_C(1):FireProductionAcceptedManifoldPayloadDigest(computed);
			token.acceptedStateDigest_=FireProductionAcceptedStatePayloadDigestFast(shape,computed.conservativeValues,
				computed.projection.momentumKGPerM2S,computed.projection.velocityMPerS);
			token.acceptedStateDigestVersion_=2u;
			token.publicationRoot_=publicationRoot;
			token.generationAuthoritative_=false;token.plateauEnforced_=false;
			if(!computed.AcceptedManifoldTokenMatchesCurrentPayload())
				return Fail(error,"CPU projected-Heun owner token publication failed");
			if(diagnostics){
				FireProductionProjectedHeunMetalOwnerResult& d=*diagnostics;
				d.intermediateSealFormat="cpu-owner-fnv64";d.intermediateDigestVersion=1u;
				d.qualifiedKernelSetSHA256="";d.inputPayloadRootSHA256=inputRoot;
				d.publicationPayloadRootSHA256=publicationRoot;
				d.conservativeValues=owner.conservativeValues;
				d.momentumKGPerM2S=owner.momentumKGPerM2S;d.velocityMPerS=owner.velocityMPerS;
				d.provisionalMomentumKGPerM2S=owner.heunProvisionalMomentumKGPerM2S;
				d.projection=owner.r2.projection;d.temperatureK=owner.committedEOS.temperatureK;
				d.representedPressureRatio=std::move(ratio);d.absoluteEOSDeviation=std::move(deviation);
				d.acceptedPicardIterations={{owner.r0.acceptedIterationCount,owner.r1.acceptedIterationCount,
					owner.r2.acceptedIterationCount}};
				d.activeSetCycleLength={{owner.r0.activeSetCycleLength,owner.r1.activeSetCycleLength,
					owner.r2.activeSetCycleLength}};
				d.activeSetCanonicalProjectionCount={{owner.r0.activeSetCanonicalProjectionCount,
					owner.r1.activeSetCanonicalProjectionCount,owner.r2.activeSetCanonicalProjectionCount}};
				d.activeSetDiscontinuousClass={{owner.r0.activeSetDiscontinuousClass,
					owner.r1.activeSetDiscontinuousClass,owner.r2.activeSetDiscontinuousClass}};
				d.limiterDiscontinuousClass={{owner.r0.limiterDiscontinuousClass,
					owner.r1.limiterDiscontinuousClass,owner.r2.limiterDiscontinuousClass}};
				d.candidatePublicationIdentity={{owner.r0.acceptedCandidateIdentity,
					owner.r1.acceptedCandidateIdentity,owner.r2.acceptedCandidateIdentity}};
				d.ownerPublicationIdentity=owner.OwnerIdentity();
				d.residentProjectionInvocationCount=computed.residentProjectionInvocationCount;
				d.terminalStagingCount=1u;d.certifiedWorkingSetBytes=computed.combinedCertifiedWorkingSetBytes;
				// The accepted scalar state is the Heun solve over the averaged stage fluxes
				// under the R1 alpha; its boundary fluxes are the fast-mode conservation ledger's
				// operands (FireProductionAcceptedStageBudget), in the Metal packet layout.
				const auto& pair=owner.averagedFlux.compositeFluxPair;
				const std::size_t allFaces=FireProductionProjectionFaceCount(shape,0u)+
					FireProductionProjectionFaceCount(shape,1u)+FireProductionProjectionFaceCount(shape,2u);
				for(unsigned int axis=0u;axis<3u;++axis)d.acceptedFaceAlpha.insert(d.acceptedFaceAlpha.end(),
					owner.heunSolve.scalar.sharedFaceAlpha[axis].begin(),owner.heunSolve.scalar.sharedFaceAlpha[axis].end());
				if(request.captureAcceptedBudgetFlux&&pair.lowFlux.size()==9u*allFaces&&
					pair.fluxDelta.size()==9u*allFaces&&d.acceptedFaceAlpha.size()==allFaces){
					std::vector<unsigned char>& payload=d.acceptedBudgetPayload;
					payload.assign(72u+26u*allFaces*sizeof(float),0u);
					if(!HexToBytes(inputRoot,payload.data())||!HexToBytes(publicationRoot,payload.data()+32u))
						return Fail(error,"CPU fire budget packet root is invalid");
					std::memcpy(payload.data()+64u,&d.candidatePublicationIdentity[1],sizeof(std::uint64_t));
					std::memcpy(payload.data()+72u,pair.lowFlux.data(),9u*allFaces*sizeof(float));
					std::memcpy(payload.data()+72u+9u*allFaces*sizeof(float),pair.fluxDelta.data(),
						9u*allFaces*sizeof(float));
					d.acceptedBudgetPayloadRootSHA256=RISECBOR64::SHA256Hex(payload.data(),payload.size());
				}
				d.wallElapsedMS=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-wallStart).count();
				d.accepted=true;
			}
			result=std::move(computed);
			if(error)error->clear();return true;
		}catch(const std::bad_alloc&){
			result=FireProductionResidentStepResult();
			if(error)try{*error="CPU projected-Heun step allocation failed";}catch(...){}
			return false;
		}
	}
}
