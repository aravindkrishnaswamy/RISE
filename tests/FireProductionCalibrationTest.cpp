#include "FireProductionCalibrationMath.h"
#include "FireProductionCalibrationMirror.h"
#include "FireProductionRoundoffWalker.h"
#include "FireProductionRoundoffTraceAdapter.h"
#include "Utilities/FireCase.h"
#include "Utilities/FireProductionAdvection.h"
#include "Utilities/FireProductionForce.h"
#include "Utilities/FireProductionProjection.h"
#include "Utilities/FireProductionTransport.h"
#include "fire_production_fp64/FireProductionAdvection.h"
#include "fire_production_fp64/FireProductionForce.h"
#include "fire_production_fp64/FireProductionProjection.h"
#include "fire_production_fp64/SourceManifest.h"
#include "fire_production_trace/FireProductionAdvection.h"
#include "fire_production_trace/FireProductionForce.h"
#include "fire_production_trace/FireProductionTransport.h"
#include "fire_production_trace/SourceManifest.h"
#include "../tools/fire_simulator_core.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

extern "C" bool RISEProjectedHeunOwnerTestFailureProbe(const char* name)
{
	const char* requested=std::getenv("RISE_FIRE_PROJECTED_HEUN_OWNER_TEST_FAILURE");
	if(!requested)return false;
	const std::size_t nameLength=std::strlen(name);
	for(const char* token=requested;*token;){
		const char* end=std::strchr(token,',');
		const std::size_t length=end?static_cast<std::size_t>(end-token):std::strlen(token);
		if(length==nameLength&&std::memcmp(token,name,length)==0)return true;
		if(!end)break;token=end+1;
	}
	return false;
}

#if defined(__APPLE__)
namespace RISE
{
	bool FireProductionDualLayoutPackRequiresSerialOwner(bool,bool);
}
#endif

namespace
{
	template<typename T,typename=void>
	struct HasPublicOwnerIdentity : std::false_type {};
	template<typename T>
	struct HasPublicOwnerIdentity<T,std::void_t<decltype(
		std::declval<T&>().ownerIdentity)> > : std::true_type {};

	int failures=0;

	void Check(const bool condition,const char* message)
	{
		if(!condition){std::fprintf(stderr,"FAIL: %s\n",message);++failures;}
	}

	std::size_t FaceCount(const std::size_t n,const std::size_t lines)
	{
		return (n+1u)*lines;
	}

	bool IndependentR60AndPeriodicCommuting(
		const RISE::FireProductionScalarFCTRequest& request,
		const RISE::FireProductionScalarFCTResult& result)
	{
		const RISE::FireProductionProjectionShape& shape=request.shape;
		const std::size_t cells=shape.CellCount();
		if(result.accepted.size()!=9u*cells)return false;
		for(const RISE::FireProductionProjectionBoundary boundary:request.boundary)
			if(boundary!=RISE::FireProductionProjectionPeriodic)return false;
		auto inequality=[&](const std::array<float,9>& value,const std::size_t row){
			if(row==0u)return -value[0u];
			if(row==1u){
				float closure=value[0u];
				for(std::size_t species=0u;species<7u;++species)
					closure-=value[1u+species];
				return closure;
			}
			if(row<9u)return -value[row-1u];
			if(row==9u){
				float lower=-value[8u];
				for(std::size_t species=0u;species<7u;++species)
					lower+=request.enthalpyBoundsJPerKG[species]*value[1u+species];
				return lower;
			}
			float upper=value[8u];for(std::size_t species=0u;species<7u;++species)
				upper-=request.enthalpyBoundsJPerKG[7u+species]*value[1u+species];
			return upper;
		};
		for(std::size_t cell=0u;cell<cells;++cell){
			std::array<float,9> value={{}};float scale=1.0f;
			for(std::size_t component=0u;component<9u;++component){
				value[component]=result.accepted[component*cells+cell];
				scale+=std::fabs(value[component]);
			}
			for(std::size_t row=0u;row<11u;++row)
				if(!std::isfinite(inequality(value,row))||
					inequality(value,row)>request.feasibilityFactor*scale)return false;
		}
		std::vector<float> baseGas(cells,0.0f),acceptedGas(cells,0.0f);
		for(std::size_t component=1u;component<=6u;++component)
			for(std::size_t cell=0u;cell<cells;++cell){
				baseGas[cell]+=request.beginning[component*cells+cell]+
					request.sourceDelta[component*cells+cell];
				acceptedGas[cell]+=result.accepted[component*cells+cell];
			}
		RISE::FireProductionCompatibleFCTMomentumRequest identity;
		identity.shape=shape;identity.boundary=request.boundary;
		for(unsigned int axis=0u;axis<3u;++axis){
			const std::size_t faces=RISE::FireProductionProjectionFaceCount(shape,axis);
			identity.lowGasFluxKGPerM2S[axis]=result.acceptedGasFluxKGPerM2S[axis];
			identity.highGasFluxKGPerM2S[axis]=result.acceptedGasFluxKGPerM2S[axis];
			identity.sharedFaceAlpha[axis].assign(faces,0.0f);
			identity.frozenVelocityMPerS[axis].assign(faces,1.0f);
		}
		RISE::FireProductionCompatibleFCTMomentumResult rate;std::string error;
		if(!RISE::EvaluateFireProductionCompatibleFCTMomentumCPU(identity,rate,&error))
			return false;
		auto cellIndex=[&](const std::size_t x,const std::size_t y,const std::size_t z){
			return (z*shape.ny+y)*shape.nx+x;};
		float maximum=0.0f,scale=1.0f;
		for(unsigned int axis=0u;axis<3u;++axis)
			for(std::size_t face=0u;face<rate.advectionRateKGPerM2S2[axis].size();++face){
				std::size_t x=0u,y=0u,z=0u,coordinate=0u,extent=0u;
				if(axis==0u){x=face%(shape.nx+1u);const std::size_t line=face/(shape.nx+1u);
					y=line%shape.ny;z=line/shape.ny;coordinate=x;extent=shape.nx;}
				if(axis==1u){x=face%shape.nx;const std::size_t line=face/shape.nx;
					y=line%(shape.ny+1u);z=line/(shape.ny+1u);coordinate=y;extent=shape.ny;}
				if(axis==2u){x=face%shape.nx;const std::size_t line=face/shape.nx;
					y=line%shape.ny;z=line/shape.ny;coordinate=z;extent=shape.nz;}
				std::size_t lx=x,ly=y,lz=z,hx=x,hy=y,hz=z;
				const std::size_t lowCoordinate=coordinate==0u?extent-1u:coordinate-1u;
				const std::size_t highCoordinate=coordinate==extent?0u:coordinate;
				if(axis==0u){lx=lowCoordinate;hx=highCoordinate;}
				if(axis==1u){ly=lowCoordinate;hy=highCoordinate;}
				if(axis==2u){lz=lowCoordinate;hz=highCoordinate;}
				const float base=0.5f*(baseGas[cellIndex(lx,ly,lz)]+
					baseGas[cellIndex(hx,hy,hz)]);
				const float accepted=0.5f*(acceptedGas[cellIndex(lx,ly,lz)]+
					acceptedGas[cellIndex(hx,hy,hz)]);
				const float advanced=base-request.timeStepS*
					rate.advectionRateKGPerM2S2[axis][face];
				maximum=std::max(maximum,std::fabs(accepted-advanced));
				scale=std::max(scale,std::max(std::fabs(accepted),std::fabs(advanced)));
			}
		return result.commutingIdentityAvailable&&std::fabs(maximum-
			result.maximumCommutingResidualKGPerM3)<=
			8.0f*std::numeric_limits<float>::epsilon()*scale;
	}

	class ConstantProjectedHeunTransport32 final :
		public RISE::FireProductionProjectedHeunTransportProvider
	{
	public:
		bool Evaluate(const RISE::FireProductionProjectedHeunTransportContext& context,
			RISE::FireProductionProjectedHeunTransportCoefficients& result,
			std::string* error) const override
		{
			if(!context.conservativeValues||context.conservativeValues->size()%9u){
				if(error)*error="constant transport context is malformed";return false;
			}
			const std::size_t cells=context.conservativeValues->size()/9u;
			result.stage=context.stage;result.attemptIdentity=context.attemptIdentity;
			result.parentCandidateIdentity=context.parentCandidateIdentity;
			result.projectionIdentity=context.projectionIdentity;
			result.diffusivityM2PerS.assign(cells,0.0f);
			result.conductivityWPerMK.assign(cells,0.0f);
			result.molecularKinematicViscosityM2PerS.assign(cells,0.0f);
			result.publicationIdentity=
				RISE::FireProductionProjectedHeunTransportPublicationIdentity(context,result);
			if(error)error->clear();return true;
		}
	};

	class ForgedProjectedHeunTransport32 final :
		public RISE::FireProductionProjectedHeunTransportProvider
	{
	public:
		explicit ForgedProjectedHeunTransport32(const bool staleProjection) :
			staleProjection_(staleProjection),firstProjection_(0u) {}
		bool Evaluate(const RISE::FireProductionProjectedHeunTransportContext& context,
			RISE::FireProductionProjectedHeunTransportCoefficients& result,
			std::string* error) const override
		{
			ConstantProjectedHeunTransport32 valid;
			if(!valid.Evaluate(context,result,error))return false;
			if(staleProjection_){
				if(firstProjection_==0u)firstProjection_=context.projectionIdentity;
				result.projectionIdentity=firstProjection_;
			}else ++result.parentCandidateIdentity;
			return true;
		}
	private:
		bool staleProjection_;
		mutable std::uint64_t firstProjection_;
	};

	class ReplayedProjectedHeunTransport32 final :
		public RISE::FireProductionProjectedHeunTransportProvider
	{
	public:
		ReplayedProjectedHeunTransport32() : firstIdentity_(0u) {}
		bool Evaluate(const RISE::FireProductionProjectedHeunTransportContext& context,
			RISE::FireProductionProjectedHeunTransportCoefficients& result,
			std::string* error) const override
		{
			ConstantProjectedHeunTransport32 valid;
			if(!valid.Evaluate(context,result,error))return false;
			if(firstIdentity_==0u)firstIdentity_=result.publicationIdentity;
			else result.publicationIdentity=firstIdentity_;
			return true;
		}
	private:
		mutable std::uint64_t firstIdentity_;
	};

	enum class ProjectedHeunContextOperand
	{
		ConservativeState,
		Temperature,
		ProjectedVelocity,
		Stage,
		AttemptIdentity,
		ParentCandidateIdentity,
		ProjectionIdentity,
		ConservativeStatePointer,
		TemperaturePointer,
		ProjectedVelocityPointer
	};

	class MutatingProjectedHeunTransport32 final :
		public RISE::FireProductionProjectedHeunTransportProvider
	{
	public:
		MutatingProjectedHeunTransport32(const RISE::FireProductionProjectedHeunStage stage,
			const ProjectedHeunContextOperand operand,const bool returnSuccess) :
			stage_(stage),operand_(operand),returnSuccess_(returnSuccess),mutated_(false) {}
		bool Evaluate(const RISE::FireProductionProjectedHeunTransportContext& context,
			RISE::FireProductionProjectedHeunTransportCoefficients& result,
			std::string* error) const override
		{
			if(context.stage==stage_&&!mutated_){
				ConstantProjectedHeunTransport32 valid;
				if(!valid.Evaluate(context,result,error))return false;
				float* value=0;
				if(operand_==ProjectedHeunContextOperand::ConservativeState&&
					context.conservativeValues&&!context.conservativeValues->empty())
					value=&const_cast<std::vector<float>*>(context.conservativeValues)->front();
				else if(operand_==ProjectedHeunContextOperand::Temperature&&
					context.temperatureK&&!context.temperatureK->empty())
					value=&const_cast<std::vector<float>*>(context.temperatureK)->front();
				else if(operand_==ProjectedHeunContextOperand::ProjectedVelocity&&
					context.projectedVelocityMPerS)
					for(unsigned int axis=0u;axis<3u&&!value;++axis)if(
						!(*context.projectedVelocityMPerS)[axis].empty())value=&const_cast<
							std::array<std::vector<float>,3>*>(context.projectedVelocityMPerS)->
							at(axis).front();
				auto& mutableContext=const_cast<
					RISE::FireProductionProjectedHeunTransportContext&>(context);
				if(value)*value=std::nextafter(*value,std::numeric_limits<float>::infinity());
				else if(operand_==ProjectedHeunContextOperand::Stage)
					mutableContext.stage=stage_==RISE::FireProductionProjectedHeunStage::R0?
						RISE::FireProductionProjectedHeunStage::R1:
						RISE::FireProductionProjectedHeunStage::R0;
				else if(operand_==ProjectedHeunContextOperand::AttemptIdentity)
					++mutableContext.attemptIdentity;
				else if(operand_==ProjectedHeunContextOperand::ParentCandidateIdentity)
					++mutableContext.parentCandidateIdentity;
				else if(operand_==ProjectedHeunContextOperand::ProjectionIdentity)
					++mutableContext.projectionIdentity;
				else if(operand_==ProjectedHeunContextOperand::ConservativeStatePointer)
					mutableContext.conservativeValues=context.temperatureK;
				else if(operand_==ProjectedHeunContextOperand::TemperaturePointer)
					mutableContext.temperatureK=context.conservativeValues;
				else if(operand_==ProjectedHeunContextOperand::ProjectedVelocityPointer)
					mutableContext.projectedVelocityMPerS=&alternateVelocity_;
				else { if(error)*error="mutation context is malformed";return false; }
				mutated_=true;
				if(!returnSuccess_){
					if(error)*error="injected mutated transport refusal";return false;
				}
				if(error)error->clear();return true;
			}
			ConstantProjectedHeunTransport32 valid;
			return valid.Evaluate(context,result,error);
		}
		bool Mutated() const { return mutated_; }
	private:
		RISE::FireProductionProjectedHeunStage stage_;
		ProjectedHeunContextOperand operand_;
		bool returnSuccess_;
		mutable bool mutated_;
		mutable std::array<std::vector<float>,3> alternateVelocity_;
	};

	class MutatingOuterResultProjectedHeunTransport32 final :
		public RISE::FireProductionProjectedHeunTransportProvider
	{
	public:
		MutatingOuterResultProjectedHeunTransport32(
			RISE::FireProductionProjectedHeunOwnerResult& outer,
			const RISE::FireProductionProjectedHeunOwnerResult& stale) :
			outer_(outer),stale_(stale),mutated_(false) {}
		bool Evaluate(const RISE::FireProductionProjectedHeunTransportContext& context,
			RISE::FireProductionProjectedHeunTransportCoefficients& result,
			std::string* error) const override
		{
			if(context.stage==RISE::FireProductionProjectedHeunStage::R2&&!mutated_){
				outer_=stale_;mutated_=true;
				if(error)*error="injected outer-result mutation";return false;
			}
			ConstantProjectedHeunTransport32 valid;
			return valid.Evaluate(context,result,error);
		}
		bool Mutated() const { return mutated_; }
	private:
		RISE::FireProductionProjectedHeunOwnerResult& outer_;
		const RISE::FireProductionProjectedHeunOwnerResult& stale_;
		mutable bool mutated_;
	};

	class ReentrantProjectedHeunTransport32 final :
		public RISE::FireProductionProjectedHeunTransportProvider
	{
	public:
		ReentrantProjectedHeunTransport32(
			RISE::FireProductionProjectedHeunCPUOwner& owner,
			const RISE::FireProductionProjectedHeunOwnerResult& staleResult) :
			owner_(owner),attempted_(false),r0Rejected_(false),r1Rejected_(false),
			r2Rejected_(false),nestedResult_(staleResult) {}
		bool Evaluate(const RISE::FireProductionProjectedHeunTransportContext& context,
			RISE::FireProductionProjectedHeunTransportCoefficients& result,
			std::string* error) const override
		{
			ConstantProjectedHeunTransport32 valid;
			if(!attempted_){
				attempted_=true;std::string nestedError;
				r0Rejected_=!owner_.SolveR0(valid,&nestedError)&&
					nestedError=="projected-Heun R0 is out of order";
				r1Rejected_=!owner_.SolveR1(valid,&nestedError)&&
					nestedError=="projected-Heun R1 is out of order";
				r2Rejected_=!owner_.SolveR2(valid,nestedResult_,&nestedError)&&
					nestedError=="projected-Heun R2 is out of order";
			}
			return valid.Evaluate(context,result,error);
		}
		bool NestedStagesRejected() const
			{ return attempted_&&r0Rejected_&&r1Rejected_&&r2Rejected_; }
		const RISE::FireProductionProjectedHeunOwnerResult& NestedResult() const
			{ return nestedResult_; }
	private:
		RISE::FireProductionProjectedHeunCPUOwner& owner_;
		mutable bool attempted_,r0Rejected_,r1Rejected_,r2Rejected_;
		mutable RISE::FireProductionProjectedHeunOwnerResult nestedResult_;
	};

	class ConstantProjectedHeunTransport64 final :
		public RISEFireProductionFP64::FireProductionProjectedHeunTransportProvider
	{
	public:
		bool Evaluate(
			const RISEFireProductionFP64::FireProductionProjectedHeunTransportContext& context,
			RISEFireProductionFP64::FireProductionProjectedHeunTransportCoefficients& result,
			std::string* error) const override
		{
			if(!context.conservativeValues||context.conservativeValues->size()%9u){
				if(error)*error="constant fp64 transport context is malformed";return false;
			}
			const std::size_t cells=context.conservativeValues->size()/9u;
			result.stage=context.stage;result.attemptIdentity=context.attemptIdentity;
			result.parentCandidateIdentity=context.parentCandidateIdentity;
			result.projectionIdentity=context.projectionIdentity;
			result.diffusivityM2PerS.assign(cells,0.0);
			result.conductivityWPerMK.assign(cells,0.0);
			result.molecularKinematicViscosityM2PerS.assign(cells,0.0);
			result.publicationIdentity=
				RISEFireProductionFP64::FireProductionProjectedHeunTransportPublicationIdentity(
					context,result);
			if(error)error->clear();return true;
		}
	};

	class OracleOpenProjectedHeunTransport32 final :
		public RISE::FireProductionProjectedHeunTransportProvider
	{
	public:
		OracleOpenProjectedHeunTransport32(const RISE::FireProductionProjectionShape& shape,
			const RISE::FireSim::OpenBoundaryConfig3D& boundary) :
			shape_(shape),boundary_(boundary) {}
		bool Evaluate(const RISE::FireProductionProjectedHeunTransportContext& context,
			RISE::FireProductionProjectedHeunTransportCoefficients& result,
			std::string* error) const override
		{
			const std::size_t cells=shape_.CellCount();
			if(!context.conservativeValues||context.conservativeValues->size()!=9u*cells||
				!context.temperatureK||context.temperatureK->size()!=cells||
				!context.projectedVelocityMPerS)return false;
			RISE::FireSim::PeriodicMACShape shape;
			shape.nx=shape_.nx;shape.ny=shape_.ny;shape.nz=shape_.nz;
			shape.cellWidthM=shape_.cellWidthM;
			std::vector<RISE::FireSim::ConservativeVector> state(cells);
			std::vector<double> temperature(cells);
			for(std::size_t cell=0u;cell<cells;++cell){
				for(std::size_t component=0u;component<9u;++component)
					state[cell][component]=(*context.conservativeValues)[component*cells+cell];
				temperature[cell]=(*context.temperatureK)[cell];
			}
			RISE::FireSim::OpenMACField3D velocity;
			for(unsigned int axis=0u;axis<3u;++axis)
				velocity.component[axis].assign((*context.projectedVelocityMPerS)[axis].begin(),
					(*context.projectedVelocityMPerS)[axis].end());
			std::vector<double> diffusivity,conductivity,dynamicViscosity;
			if(!RISE::FireSim::BuildOpenStageTransport3D(shape,state,temperature,velocity,
				boundary_,false,RISE::FireSimulationMethaneRecord::PhysicalV1(),
				RISE::FireSimulationTransportRecord::OpenV1(),
				RISE::FireStateProducerPrecision::Binary32,diffusivity,conductivity,
				dynamicViscosity,error))return false;
			result.stage=context.stage;result.attemptIdentity=context.attemptIdentity;
			result.parentCandidateIdentity=context.parentCandidateIdentity;
			result.projectionIdentity=context.projectionIdentity;
			result.diffusivityM2PerS.resize(cells);
			result.conductivityWPerMK.resize(cells);
			result.molecularKinematicViscosityM2PerS.resize(cells);
			for(std::size_t cell=0u;cell<cells;++cell){
				double density=0.0;for(std::size_t component=1u;component<=6u;++component)
					density+=state[cell][component];
				result.diffusivityM2PerS[cell]=static_cast<float>(diffusivity[cell]);
				result.conductivityWPerMK[cell]=static_cast<float>(conductivity[cell]);
				result.molecularKinematicViscosityM2PerS[cell]=
					static_cast<float>(dynamicViscosity[cell]/density);
			}
			result.publicationIdentity=
				RISE::FireProductionProjectedHeunTransportPublicationIdentity(context,result);
			if(error)error->clear();return true;
		}
	private:
		RISE::FireProductionProjectionShape shape_;
		RISE::FireSim::OpenBoundaryConfig3D boundary_;
	};
}

int main()
{
	const RISE::FireProductionResidentStepRequest monitoredDefaultRequest;
	Check(monitoredDefaultRequest.monitorManifoldDiagnostics&&
		!monitoredDefaultRequest.enforceManifoldPlateau&&
		monitoredDefaultRequest.restoreManifoldOutliers,
		"production defaults to monitored diagnostics with conditional tail restoration");
	RISE::FireProductionResidentStepResult monitoredEligibility;
	monitoredEligibility.acceptedShape.nx=1u;monitoredEligibility.acceptedShape.ny=1u;
	monitoredEligibility.acceptedShape.nz=1u;monitoredEligibility.acceptedShape.cellWidthM=1.0f;
	monitoredEligibility.conservativeProducerPrecision=
		RISE::FireStateProducerPrecision::Binary32;
	monitoredEligibility.projection.validationPassed=true;
	monitoredEligibility.manifoldDiagnosticsMonitored=true;
	monitoredEligibility.manifoldPlateauPassed=true;
	monitoredEligibility.manifoldDynamicsBoundPassed=true;
	monitoredEligibility.residentProjectionInvocationCount=1u;
	monitoredEligibility.manifoldMapCellCount=1u;
	monitoredEligibility.manifoldScalarDeviceToHostTransferCount=1u;
	monitoredEligibility.maximumManifoldGeneration=0.085895776748657227;
	monitoredEligibility.manifoldStageGeneration[0]=
		monitoredEligibility.maximumManifoldGeneration;
	monitoredEligibility.maximumAcceptedManifoldDeviation=0.085895776748657227;
	monitoredEligibility.acceptedManifoldDeviationP95=0.00071418285369873047;
	monitoredEligibility.acceptedManifoldDeviationP50=1.1920928955078125e-07;
	monitoredEligibility.manifoldAllowanceExceeded=true;
	monitoredEligibility.manifoldCeilingExceeded=true;
	Check(FireProductionResidentStepEligibleForAcceptedManifoldToken(monitoredEligibility),
		"monitored threshold crossings remain eligible after conservation and projection gates");
	RISE::FireProductionResidentStepResult malformedDistribution=monitoredEligibility;
	malformedDistribution.acceptedManifoldDeviationP95=
		malformedDistribution.maximumAcceptedManifoldDeviation+1.0;
	Check(!FireProductionResidentStepEligibleForAcceptedManifoldToken(malformedDistribution),
		"monitored token eligibility rejects a malformed deviation distribution");
	RISE::FireProductionResidentStepResult forgedCrossing=monitoredEligibility;
	forgedCrossing.manifoldCeilingExceeded=false;
	Check(!FireProductionResidentStepEligibleForAcceptedManifoldToken(forgedCrossing),
		"monitored token eligibility binds threshold-crossing identity");
	RISE::FireProductionResidentStepResult targetedEligibility=monitoredEligibility;
	targetedEligibility.physicalProjection.validationPassed=true;
	targetedEligibility.residentProjectionInvocationCount=2u;
	targetedEligibility.manifoldTailRestorationApplied=true;
	targetedEligibility.manifoldTailCellCount=1u;
	targetedEligibility.manifoldTailExcessSum=0.01;
	targetedEligibility.manifoldTailDrainedVolumeM3=1.0e-6;
	Check(FireProductionResidentStepEligibleForAcceptedManifoldToken(targetedEligibility),
		"targeted monitored restoration remains eligible only with two validating projections");
	targetedEligibility.manifoldTailRestorationApplied=false;
	Check(!FireProductionResidentStepEligibleForAcceptedManifoldToken(targetedEligibility),
		"tail population cannot be published without its conditional restoration identity");
	std::array<std::vector<float>,3> zeroTransportVelocity;
	for(std::vector<float>& axis:zeroTransportVelocity)axis.assign(2u,0.0f);
	std::array<std::vector<float>,3> movingTransportVelocity=zeroTransportVelocity;
	movingTransportVelocity[1][0]=1.0f;
	Check(RISE::FireProductionEulerianGenerationHasMaterialAuthority(
			std::vector<double>{0.0,0.0,0.0},movingTransportVelocity)&&
		RISE::FireProductionEulerianGenerationHasMaterialAuthority(
			std::vector<double>{0.0,0.125,0.0},zeroTransportVelocity)&&
		!RISE::FireProductionEulerianGenerationHasMaterialAuthority(
			std::vector<double>{0.0,0.125,0.0},movingTransportVelocity)&&
		!RISE::FireProductionEulerianGenerationHasMaterialAuthority(
			std::vector<double>(),zeroTransportVelocity),
		"Eulerian generation is material only from zero beginning or exact rest");
	unsigned int retryCandidate=0u;
	double retryStep=0.0;
	RISE::FireProductionResidentStepResult syntheticRetry;
	syntheticRetry.representedTimeStepS=0.00055692793102934957f;
	syntheticRetry.suggestedManifoldTimeStepS=0.00055690890514272363;
	syntheticRetry.manifoldNextTimeStepAvailable=true;
	RISE::FireProductionResidentStepResult nonReducingRetry=syntheticRetry;
	nonReducingRetry.suggestedManifoldTimeStepS=static_cast<double>(
		nonReducingRetry.representedTimeStepS);
	RISE::FireProductionResidentStepResult unauthoritativeAcceptance=syntheticRetry;
	unauthoritativeAcceptance.manifoldPlateauPassed=true;
	Check(RISE::ClassifyFireProductionResidentStepAttempt(1u,syntheticRetry,retryCandidate,
			retryStep)==RISE::FireProductionResidentStepAttemptDisposition::
			RetryAtSuggestedTimeStep&&retryCandidate==2u&&
		retryStep==syntheticRetry.suggestedManifoldTimeStepS&&
		RISE::ClassifyFireProductionResidentStepAttempt(
			RISE::FireStepRejectionRetryCap-2u,syntheticRetry,retryCandidate,retryStep)==
			RISE::FireProductionResidentStepAttemptDisposition::RetryAtSuggestedTimeStep&&
		retryCandidate==RISE::FireStepRejectionRetryCap-1u&&
		retryStep==syntheticRetry.suggestedManifoldTimeStepS&&
		RISE::ClassifyFireProductionResidentStepAttempt(
			RISE::FireStepRejectionRetryCap-1u,syntheticRetry,retryCandidate,retryStep)==
			RISE::FireProductionResidentStepAttemptDisposition::Rejected&&
		retryCandidate==0u&&retryStep==0.0&&
		RISE::ClassifyFireProductionResidentStepAttempt(
			RISE::FireStepRejectionRetryCap,syntheticRetry,retryCandidate,retryStep)==
			RISE::FireProductionResidentStepAttemptDisposition::Rejected&&
		RISE::ClassifyFireProductionResidentStepAttempt(
			std::numeric_limits<unsigned int>::max(),syntheticRetry,retryCandidate,retryStep)==
			RISE::FireProductionResidentStepAttemptDisposition::Rejected&&
		RISE::ClassifyFireProductionResidentStepAttempt(0u,nonReducingRetry,
			retryCandidate,retryStep)==
			RISE::FireProductionResidentStepAttemptDisposition::Rejected&&
		RISE::ClassifyFireProductionResidentStepAttempt(0u,unauthoritativeAcceptance,
			retryCandidate,retryStep)==
			RISE::FireProductionResidentStepAttemptDisposition::Rejected,
		"drain-aware plateau retry is a strict reduction bounded by the existing cap");
	auto floatFromBits=[](const std::uint32_t bits){float value=0.0f;
		std::memcpy(&value,&bits,sizeof(value));return value;};
	RISE::FireProductionResidentStepResult digestCollisionLeft,digestCollisionRight;
	digestCollisionLeft.conservativeValues.assign(3u,1.0f);
	digestCollisionRight.conservativeValues={{floatFromBits(UINT32_C(0x3f800001)),
		floatFromBits(UINT32_C(0x3f800006)),floatFromBits(UINT32_C(0x3f800001))}};
	Check(RISE::FireProductionAcceptedManifoldPayloadDigest(digestCollisionLeft)!=
		RISE::FireProductionAcceptedManifoldPayloadDigest(digestCollisionRight),
		"accepted resident authority digest rejects the retired two-moment three-word collision");
	const double equalTimeProduction=static_cast<double>(
		static_cast<float>(0.000579539999762149));
	const double equalTimeSubstep=equalTimeProduction*0.125;
	std::vector<double> equalTimeSchedule(8u,equalTimeSubstep);
	equalTimeSchedule.back()=equalTimeProduction-
		equalTimeSubstep*static_cast<double>(equalTimeSchedule.size()-1u);
	const std::string equalTimeTerminalTarget="terminal-target";
	const std::string equalTimePenultimateTarget="penultimate-target";
	std::vector<double> mismatchedEqualTimeSchedule=equalTimeSchedule;
	mismatchedEqualTimeSchedule.back()+=equalTimeSubstep;
	Check(FireProductionCalibration::EqualTimeReferenceSchedule(equalTimeProduction,
		equalTimeSchedule,equalTimeProduction,equalTimeProduction,
		equalTimeTerminalTarget,equalTimeTerminalTarget),
		"equal-time reference accepts the exact shared endpoint");
	Check(!FireProductionCalibration::EqualTimeReferenceSchedule(equalTimeProduction,
		mismatchedEqualTimeSchedule,equalTimeProduction,equalTimeProduction,
		equalTimeTerminalTarget,equalTimeTerminalTarget),
		"equal-time reference rejects a mismatched endpoint");
	Check(!FireProductionCalibration::EqualTimeReferenceSchedule(equalTimeProduction,
		equalTimeSchedule,equalTimeProduction,equalTimeSubstep,
		equalTimeTerminalTarget,equalTimeTerminalTarget),
		"equal-time reference refuses a stale-dt terminal target");
	Check(!FireProductionCalibration::EqualTimeReferenceSchedule(equalTimeProduction,
		equalTimeSchedule,equalTimeProduction,equalTimeProduction,
		equalTimeTerminalTarget,equalTimePenultimateTarget),
		"equal-time reference refuses the penultimate target at the current endpoint");
	double zeroAnomalyTarget=0.0,activeAnomalyTarget=0.0;
	const bool zeroAnomalyDerived=RISE::DeriveFireProductionAdvectiveAnomalyTarget(
		0.125,0.125,0.5,0.25,zeroAnomalyTarget);
	const bool activeAnomalyDerived=RISE::DeriveFireProductionAdvectiveAnomalyTarget(
		0.125,0.375,0.5,0.25,activeAnomalyTarget);
	const double missingRampTarget=0.25;
	const double halfRampTarget=0.25+0.5*(0.375-0.125)/0.5;
	const double reversedRampTarget=0.25-(0.375-0.125)/0.5;
	Check(zeroAnomalyDerived&&zeroAnomalyTarget==0.25&&activeAnomalyDerived&&
		activeAnomalyTarget==0.75&&missingRampTarget!=activeAnomalyTarget&&
		halfRampTarget!=activeAnomalyTarget&&reversedRampTarget!=activeAnomalyTarget,
		"advective-anomaly target is an exact zero-anomaly no-op and rejects missing, half, and reversed ramps");
	std::vector<double> longShadowFlat(FireProductionCalibration::LongShadowSteps,0.0025);
	std::vector<double> longShadowSecular(FireProductionCalibration::LongShadowSteps,0.0);
	std::vector<double> longShadowMaskedSecular(
		FireProductionCalibration::LongShadowSteps,0.01);
	for(std::size_t step=0u;step<FireProductionCalibration::LongShadowSteps;++step)
		longShadowSecular[step]=0.001+1.0e-6*static_cast<double>(step);
	const std::size_t longShadowFirst=FireProductionCalibration::LongShadowSteps-
		2u*FireProductionCalibration::LongShadowWindow;
	longShadowMaskedSecular[longShadowFirst]=0.03;
	for(std::size_t step=0u;step<FireProductionCalibration::LongShadowWindow;++step)
		longShadowMaskedSecular[FireProductionCalibration::LongShadowSteps-
			FireProductionCalibration::LongShadowWindow+step]=
			0.01+0.0003*static_cast<double>(step);
	Check(FireProductionCalibration::LongShadowNonsecular(longShadowFlat)&&
		!FireProductionCalibration::LongShadowNonsecular(longShadowSecular)&&
		!FireProductionCalibration::LongShadowNonsecular(longShadowMaskedSecular),
		"long-shadow detector rejects monotone and prior-outlier-masked secular growth");
	auto appendDistributionStatistics=[](std::vector<double> field,
		std::vector<double>& maximum,std::vector<double>& p95,std::vector<double>& p50){
		std::sort(field.begin(),field.end());
		maximum.push_back(field.back());p50.push_back(field[(field.size()-1u)/2u]);
		p95.push_back(field[(95u*field.size()+99u)/100u-1u]);
	};
	std::vector<double> motionPattern(100u,0.0);
	for(std::size_t index=0u;index<motionPattern.size();++index)
		motionPattern[index]=0.001+0.00019*static_cast<double>(index);
	std::vector<double> motionOnlyMaximum,motionOnlyP95,motionOnlyP50;
	std::vector<double> hiddenSecularMaximum,hiddenSecularP95,hiddenSecularP50;
	for(std::size_t step=0u;step<FireProductionCalibration::LongShadowSteps;++step){
		std::rotate(motionPattern.begin(),motionPattern.begin()+1u,motionPattern.end());
		appendDistributionStatistics(motionPattern,motionOnlyMaximum,motionOnlyP95,
			motionOnlyP50);
		std::vector<double> injected(100u,0.002);
		const double growing=0.005+0.00004*static_cast<double>(step);
		std::fill(injected.begin()+50u,injected.begin()+95u,growing);
		std::fill(injected.begin()+95u,injected.end(),0.02);
		appendDistributionStatistics(injected,hiddenSecularMaximum,hiddenSecularP95,
			hiddenSecularP50);
	}
	Check(FireProductionCalibration::LongShadowDistributionNonsecular(
			motionOnlyMaximum,motionOnlyP95,motionOnlyP50)&&
		!FireProductionCalibration::LongShadowDistributionNonsecular(
			hiddenSecularMaximum,hiddenSecularP95,hiddenSecularP50)&&
		FireProductionCalibration::LongShadowNonsecular(hiddenSecularMaximum)&&
		FireProductionCalibration::LongShadowNonsecular(hiddenSecularP50)&&
		!FireProductionCalibration::LongShadowNonsecular(hiddenSecularP95),
		"motion-invariant distribution gate accepts an advected fixed pattern and p95 catches "
		"secular redistribution hidden beneath a flat maximum");
#if defined(__APPLE__)
	Check(!RISE::FireProductionDualLayoutPackRequiresSerialOwner(false,false)&&
		RISE::FireProductionDualLayoutPackRequiresSerialOwner(true,false)&&
		RISE::FireProductionDualLayoutPackRequiresSerialOwner(false,true)&&
		RISE::FireProductionDualLayoutPackRequiresSerialOwner(true,true),
		"dual-layout packing routes around recursive low-priority pool waits");
#endif
	Check(std::strlen(RISEFireProductionFP64::SourceManifest::FireProductionAdvectionSource)==64u&&
		std::strlen(RISEFireProductionFP64::SourceManifest::FireProductionProjectionSource)==64u&&
		std::strlen(RISEFireProductionFP64::SourceManifest::FireProductionTransportSource)==64u&&
		std::strlen(RISEFireProductionFP64::SourceManifest::FireProductionForceSource)==64u&&
		std::strlen(RISEFireProductionFP64::SourceManifest::Generator)==64u,
		"fp64 mirror carries source and generator SHA-256 identities");
	Check(std::strlen(RISEFireProductionTrace::SourceManifest::FireProductionAdvectionSource)==64u&&
		std::strlen(RISEFireProductionTrace::SourceManifest::FireProductionProjectionSource)==64u&&
		std::strlen(RISEFireProductionTrace::SourceManifest::FireProductionTransportSource)==64u&&
		std::strlen(RISEFireProductionTrace::SourceManifest::FireProductionForceSource)==64u&&
		std::strlen(RISEFireProductionTrace::SourceManifest::TraceAdapter)==64u&&
		std::strlen(RISEFireProductionTrace::SourceManifest::TraceCore)==64u&&
		std::strlen(RISEFireProductionTrace::SourceManifest::IndependentWalker)==64u&&
		std::strlen(RISEFireProductionTrace::SourceManifest::Generator)==64u,
		"roundoff trace carries source and generator SHA-256 identities");
	{
		double derivedFactor=0.0,derivedWidth=0.0,mutantFactor=0.0,mutantWidth=0.0;
		Check(FireProductionRoundoffWalker::DeriveInflowTransitionWidth(
			1.7632415612658968e-38,22.033558699237727,1.0,
			derivedFactor,derivedWidth)&&derivedFactor==1.0&&
			derivedWidth==0x1p-24*22.033558699237727&&
			!FireProductionRoundoffWalker::DeriveInflowTransitionWidth(
				1.7632415612658968e-38,22.033558699237727,0.5,
				mutantFactor,mutantWidth)&&mutantFactor==1.0,
			"independent inflow walker derives the one-unit power-of-two width and rejects its half-width mutant");
		FireProductionRoundoffTrace::Counters counters;
		FireProductionRoundoffTrace::TraceFloat sum;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			const FireProductionRoundoffTrace::TraceFloat one(1.0f),halfULP(0x1p-24f);
			sum=one+halfULP;
		}
		Check(counters.operation[static_cast<unsigned int>(
			FireProductionRoundoffTrace::Operation::Add)]==1u&&
			std::fabs(static_cast<double>(sum.Rounded())-sum.Center())<=sum.Radius(),
			"roundoff trace outward radius contains a binary32 tie-to-even addition");
	}
	{
		FireProductionRoundoffAdapter::ResidentStepTraceResult::ProjectionStreamingEvidence
			densityEvidence;
		FireProductionRoundoffAdapter::IncludeProjectionDensityEnvelope(
			FireProductionRoundoffTrace::TraceFloat::Raw(1.0,0.25,1.125f,0u),
			densityEvidence);
		Check(densityEvidence.densityLower<0.75&&densityEvidence.densityUpper>1.25,
			"projection spectral density envelope uses outward exact center-radius bounds");
	}
	{
		using B64=FireProductionRoundoffWalker::Binary64Interval;
		const auto normal=B64::Exact(0.125)/B64::Exact(1.1);
		const auto tangent=B64::Exact(0.5)*(B64::Exact(-0.25)+B64::Exact(0.75));
		const auto totalHead=B64::Exact(-0.5)*B64::Exact(1.2)*
			(normal*normal+tangent*tangent+tangent*tangent);
		const auto gradient=B64::Exact(2.0)*(B64::Exact(0.03125)-totalHead)/
			B64::Exact(0.04);
		const auto terminal=(B64::Exact(0.2)-B64::Exact(0.002)*gradient)/B64::Exact(1.1);
		Check(totalHead.Radius()>0.0&&gradient.Radius()>totalHead.Radius()&&
			terminal.Radius()>0.0,
			"independent binary64 interval walks total-head, gradient, and terminal division");
	}
	{
		const double radius=1.0;
		const double faceL2PerCell=std::sqrt((radius*radius+radius*radius)/2.0);
		const double cellCenteredRMS=std::sqrt(
			(0.25*radius*radius+0.25*radius*radius)/2.0);
		Check(faceL2PerCell==1.0&&cellCenteredRMS==0.5&&
			faceL2PerCell>cellCenteredRMS,
			"unique-face norm retains an endpoint-only divergence-free error hidden by cell centering");
	}
	{
		const std::array<std::size_t,3> extent={{24u,24u,36u}};
		const std::array<unsigned int,6> allOpen={{1u,1u,1u,1u,1u,1u}};
		FireProductionRoundoffWalker::ProjectionAposterioriCertificate certified,
			doublePoincare,missingCross,missingGate,missingFeedback,missingFace,
			missingTerminal,missingBeginning,zeroTarget,swappedDensity;
		const double h=0x1.4e288ep-5;
		Check(FireProductionRoundoffWalker::DeriveProjectionAposterioriBound(
			extent,allOpen,h,0.97449040412902832,1.1348450183868408,
			8.9943569037131965e-7,1.3748435749320591e-7,
			1.1e-6,0.08,0.03,1.0e-7,false,5.0652099990520917e-9,
			1.0e-12,0.000262468064,1.0e-10,
			certified)&&certified.dimensionlessEigenvalueLower==0.016975308641975297&&
			certified.operatorEigenvalueLower==8.9899277588426756&&
			certified.inverseOperatorNormUpper==0.11123559908658663&&
			certified.velocityGainUpper==0.47780216517115232&&
			certified.velocityRMSUpper>0.000125&&
			certified.validationPredicateSeparated&&certified.validationPredicateMarginLower>0.0&&
			certified.pressureOpenAnchor&&certified.allConstantsStructural,
			"independent Poincare/density/residual derivation pins the physical projection bound");
		Check(FireProductionRoundoffWalker::DeriveProjectionAposterioriBound(
			extent,allOpen,h,0.97449040412902832,1.1348450183868408,
			8.9943569037131965e-7,1.3748435749320591e-7,
			1.1e-6,0.08,0.03,1.0e-7,false,5.0652099990520917e-9,
			1.0e-12,0.000262468064,1.0e-10,
			doublePoincare,FireProductionRoundoffWalker::ProjectionAposterioriGraphVariant::
				DoublePoincare)&&
			FireProductionRoundoffWalker::DeriveProjectionAposterioriBound(
			extent,allOpen,h,0.97449040412902832,1.1348450183868408,
			8.9943569037131965e-7,1.3748435749320591e-7,
			1.1e-6,0.08,0.03,1.0e-7,false,5.0652099990520917e-9,
			1.0e-12,0.000262468064,1.0e-10,
			missingCross,FireProductionRoundoffWalker::ProjectionAposterioriGraphVariant::
				MissingCrossPrecisionResidual)&&
			FireProductionRoundoffWalker::DeriveProjectionAposterioriBound(
			extent,allOpen,h,0.97449040412902832,1.1348450183868408,
			8.9943569037131965e-7,1.3748435749320591e-7,
			1.1e-6,0.08,0.03,1.0e-7,false,5.0652099990520917e-9,
			1.0e-12,0.000262468064,1.0e-10,
			missingGate,FireProductionRoundoffWalker::ProjectionAposterioriGraphVariant::
				MissingFP64ResidualGate)&&
			FireProductionRoundoffWalker::DeriveProjectionAposterioriBound(
			extent,allOpen,h,0.97449040412902832,1.1348450183868408,
			8.9943569037131965e-7,1.3748435749320591e-7,
			1.1e-6,0.08,0.03,1.0e-7,false,5.0652099990520917e-9,
			1.0e-12,0.000262468064,1.0e-10,
			missingFeedback,FireProductionRoundoffWalker::ProjectionAposterioriGraphVariant::
				MissingFP64Feedback)&&
			FireProductionRoundoffWalker::DeriveProjectionAposterioriBound(
			extent,allOpen,h,0.97449040412902832,1.1348450183868408,
			8.9943569037131965e-7,1.3748435749320591e-7,
			1.1e-6,0.08,0.03,1.0e-7,false,5.0652099990520917e-9,
			1.0e-12,0.000262468064,1.0e-10,
			missingFace,FireProductionRoundoffWalker::ProjectionAposterioriGraphVariant::
				MissingFaceStreaming)&&
			FireProductionRoundoffWalker::DeriveProjectionAposterioriBound(
			extent,allOpen,h,0.97449040412902832,1.1348450183868408,
			8.9943569037131965e-7,1.3748435749320591e-7,
			1.1e-6,0.08,0.03,1.0e-7,false,5.0652099990520917e-9,
			1.0e-12,0.000262468064,1.0e-10,
			missingTerminal,FireProductionRoundoffWalker::ProjectionAposterioriGraphVariant::
				MissingFP64Terminal)&&
			FireProductionRoundoffWalker::DeriveProjectionAposterioriBound(
			extent,allOpen,h,0.97449040412902832,1.1348450183868408,
			8.9943569037131965e-7,1.3748435749320591e-7,
			1.1e-6,0.08,0.03,1.0e-7,false,5.0652099990520917e-9,
			1.0e-12,0.000262468064,1.0e-10,
			missingBeginning,FireProductionRoundoffWalker::ProjectionAposterioriGraphVariant::
				MissingBeginningVelocity)&&
			FireProductionRoundoffWalker::DeriveProjectionAposterioriBound(
			extent,allOpen,h,0.97449040412902832,1.1348450183868408,
			8.9943569037131965e-7,1.3748435749320591e-7,
			1.1e-6,0.08,0.0,1.0e-7,false,5.0652099990520917e-9,
			1.0e-12,0.000262468064,1.0e-10,zeroTarget)&&
			FireProductionRoundoffWalker::DeriveProjectionAposterioriBound(
			extent,allOpen,h,0.97449040412902832,1.1348450183868408,
			8.9943569037131965e-7,1.3748435749320591e-7,
			1.1e-6,0.08,0.03,1.0e-7,false,5.0652099990520917e-9,
			1.0e-12,0.000262468064,1.0e-10,
			swappedDensity,FireProductionRoundoffWalker::ProjectionAposterioriGraphVariant::
				SwappedDensityEnvelope)&&
			doublePoincare.velocityRMSUpper<certified.velocityRMSUpper&&
			missingCross.velocityRMSUpper<certified.velocityRMSUpper&&
			missingGate.velocityRMSUpper<certified.velocityRMSUpper&&
			missingFeedback.velocityRMSUpper<certified.velocityRMSUpper&&
			missingFace.velocityRMSUpper<certified.velocityRMSUpper&&
			missingTerminal.velocityRMSUpper<certified.velocityRMSUpper&&
			missingBeginning.velocityRMSUpper<certified.velocityRMSUpper&&
			zeroTarget.velocityRMSUpper<certified.velocityRMSUpper&&
			swappedDensity.velocityRMSUpper<certified.velocityRMSUpper,
			"projection derivation rejects spectrum, cross-residual, fp64-gate, face-norm, and density underbounds");
		FireProductionRoundoffTrace::Counters accepted;
		{
			FireProductionRoundoffTrace::Scope scope(accepted);
			FireProductionRoundoffTrace::ProjectionSolveDependencyScope solveScope;
			FireProductionRoundoffTrace::RecordArithmeticInvalidDomain();
		}
		FireProductionRoundoffTrace::BranchObligation obligation;
		obligation.site=FireProductionRoundoffTrace::BranchSite::ProjectionValidationBand;
		obligation.roundedResult=true;accepted.branchObligations.push_back(obligation);
		accepted.unresolvedBranch=true;
		for(unsigned int channel=9u;channel<12u;++channel){
			accepted.metricOutputCount[channel]=1u;
			accepted.nonfiniteMetricOutputRadiusCount[channel]=1u;}
		Check(FireProductionRoundoffTrace::ApplyProjectionAposterioriCertificate(
			accepted,0.001f,0.002f,0.0001,0.0001,certified.velocityRMSUpper)&&
			accepted.branchObligations.front().certificate==
				FireProductionRoundoffTrace::BranchCertificate::Aposteriori&&
			accepted.aposterioriProjectionCertified&&!accepted.unresolvedBranch&&
			!accepted.invalidDomain&&accepted.aposterioriMetricBound[9]==
				certified.velocityRMSUpper,
			"accepted validation path consumes the structural residual certificate");
		FireProductionRoundoffTrace::Observation unrelatedInvalid=accepted;
		unrelatedInvalid.branchObligations.front().certificate=
			FireProductionRoundoffTrace::BranchCertificate::None;
		unrelatedInvalid.dischargedBranchObligationCount=0u;
		unrelatedInvalid.arithmeticInvalidDomainCount=2u;
		Check(!FireProductionRoundoffTrace::ApplyProjectionAposterioriCertificate(
			unrelatedInvalid,0.001f,0.002f,0.0001,0.0001,certified.velocityRMSUpper),
			"projection certificate cannot erase an unrelated arithmetic-domain failure");
		FireProductionRoundoffTrace::Observation rejected=accepted;
		rejected.branchObligations.front().certificate=
			FireProductionRoundoffTrace::BranchCertificate::None;
		rejected.branchObligations.front().roundedResult=false;
		rejected.dischargedBranchObligationCount=0u;
		Check(!FireProductionRoundoffTrace::ApplyProjectionAposterioriCertificate(
			rejected,0.001f,0.002f,0.0001,0.0001,certified.velocityRMSUpper)&&
			!FireProductionRoundoffTrace::ApplyProjectionAposterioriCertificate(
				rejected,0.001f,0.0011f,0.0002,0.0,certified.velocityRMSUpper),
			"rejected and over-band validation paths cannot borrow the accepted-solution anchor");
	}
	{
		const auto appendManifestText=[](RISE::RISECBOR64::Bytes& bytes,
			const char* value){
			const std::size_t length=std::strlen(value);
			for(unsigned int shift=0u;shift<64u;shift+=8u)
				bytes.push_back(static_cast<unsigned char>(
					(static_cast<std::uint64_t>(length)>>shift)&0xffu));
			bytes.insert(bytes.end(),value,value+length);
		};
		const auto manifestFields=
			FireProductionRoundoffAdapter::TraceSourceManifestFields();
		RISE::RISECBOR64::Bytes canonicalManifest;
		for(const char* field:manifestFields)appendManifestText(canonicalManifest,field);
		const std::string canonicalManifestDigest=
			RISE::RISECBOR64::SHA256Hex(canonicalManifest);
		bool everyManifestFieldBound=true;
		for(std::size_t field=0u;field<manifestFields.size();++field){
			RISE::RISECBOR64::Bytes mutatedManifest;
			for(std::size_t index=0u;index<manifestFields.size();++index)
				appendManifestText(mutatedManifest,index==field?
					"r136-dependency-mutation":manifestFields[index]);
			everyManifestFieldBound=everyManifestFieldBound&&
				RISE::RISECBOR64::SHA256Hex(mutatedManifest)!=canonicalManifestDigest;
		}
		Check(everyManifestFieldBound&&manifestFields.size()==
			FireProductionRoundoffAdapter::TraceSourceManifestFieldCount,
			"r136 trace identity changes for every primary, dependency, and support manifest field");
	}
	{
		FireProductionRoundoffWalker::FullStepAssumptionRefusal refusal;
		Check(FireProductionRoundoffWalker::RefuteFullStepCandidateAssumptions(refusal)&&
			refusal.conservativeCompressionL2Gain>1.0&&
			refusal.localizedProductRMS>refusal.productOfRMS&&
			refusal.sharedAlphaCrossComponentResponse>0.0,
			"r136 independently rejects r135's unit FCT gain, RMS product, and diagonal-component assumptions");
		std::array<FireProductionRoundoffWalker::FullStepMetricStage,24> stages={};
		for(const unsigned int stage:{1u,2u,3u,4u,5u,21u})for(unsigned int component=0u;
			component<9u;++component){stages[stage].count[component]=8u;
			stages[stage].radiusSum[component]=0.008;stages[stage].radiusSquareSum[component]=
				8.0e-6;stages[stage].maximumRadius[component]=0.001;}
		for(unsigned int axis=0u;axis<3u;++axis){const unsigned int channel=9u+axis;
			stages[0].count[channel]=8u;stages[0].radiusSquareSum[channel]=8.0e-8;
			for(unsigned int offset=0u;offset<5u;++offset){const unsigned int stage=6u+5u*axis+offset;
				stages[stage].count[channel]=8u;stages[stage].radiusSquareSum[channel]=8.0e-8;}}
		FireProductionRoundoffWalker::FullStepRoundoffCertificate certified,missingCell,
			missingDensity,missingPhysical,missingRestoration;
		const auto derive=[&](FireProductionRoundoffWalker::FullStepRoundoffCertificate& output,
			const FireProductionRoundoffWalker::FullStepGraphVariant variant){return
			FireProductionRoundoffWalker::DeriveFullStepRoundoffBound(stages,8u,0.9,1.2,0.04,
				0.002,1.1,0.2,1.0e-7,0.01,0.02,0.001,0.002,output,variant);};
		Check(!derive(certified,FireProductionRoundoffWalker::FullStepGraphVariant::Certified)&&
			!derive(missingCell,FireProductionRoundoffWalker::FullStepGraphVariant::MissingCellStage)&&
			!derive(missingDensity,FireProductionRoundoffWalker::FullStepGraphVariant::
				MissingDensityInteraction)&&!derive(missingPhysical,
				FireProductionRoundoffWalker::FullStepGraphVariant::MissingPhysicalFeedthrough)&&
			!derive(missingRestoration,FireProductionRoundoffWalker::FullStepGraphVariant::
				MissingRestorationFeedthrough)&&certified.proofGapBitmap==0xffu&&
			!certified.proofComplete&&missingCell.scalarFilteredL1Upper[0]<
				certified.scalarFilteredL1Upper[0]&&missingDensity.finalVelocityRMSUpper<
				certified.finalVelocityRMSUpper&&missingPhysical.finalVelocityRMSUpper<
				certified.finalVelocityRMSUpper&&missingRestoration.finalVelocityRMSUpper<
				certified.finalVelocityRMSUpper,
			"r135 candidate remains reproducible but fail-closes all eight proof gaps");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		bool copiedEqual=false,recomputedEqual=false;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			const auto source=FireProductionRoundoffTrace::TraceFloat::Raw(
				1.0,0.25,1.0f,1u);
			const auto copied=source;
			const auto recomputed=FireProductionRoundoffTrace::TraceFloat::Raw(
				1.0,0.25,1.0f,1u);
			copiedEqual=copied==source;recomputedEqual=recomputed==source;
		}
		Check(copiedEqual&&recomputedEqual&&counters.comparisonCount==2u&&
			counters.branchObligations.size()==1u,
			"trace identity proves a canonical publication copy while retaining an obligation for independently recomputed equal bytes");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		FireProductionRoundoffTrace::TraceFloat converted;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			converted=FireProductionRoundoffTrace::TraceFloat(std::uint32_t(16777217u));
		}
		Check(counters.operation[static_cast<unsigned int>(
			FireProductionRoundoffTrace::Operation::Convert)]==1u&&
			converted.Rounded()==16777216.0f&&converted.Radius()>=1.0,
			"roundoff trace records and encloses an inexact integer conversion");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		const auto numerator=FireProductionRoundoffTrace::TraceFloat::Raw(1.0,0.0,1.0f,0u);
		const auto uncertainZero=FireProductionRoundoffTrace::TraceFloat::Raw(0.0,1.0,0.0f,0u);
		bool branchResult=false;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			const auto quotient=numerator/uncertainZero;
			branchResult=uncertainZero<numerator;
			Check(!std::isfinite(quotient.Radius()),
				"invalid traced division publishes an infinite diagnostic radius");
		}
		Check(branchResult&&counters.invalidDomain&&counters.unresolvedBranch&&
			counters.operation[static_cast<unsigned int>(
				FireProductionRoundoffTrace::Operation::Divide)]==1u&&
			counters.maximumDepth==1u&&
			counters.minimumDenominatorLowerBound<=0.0&&
			counters.unresolvedWitnessRecorded&&counters.invalidDenominatorWitnessRecorded&&
			!FireProductionRoundoffWalker::IntervalsAreSeparated(
				counters.unresolvedLeftCenter,counters.unresolvedLeftRadius,
				counters.unresolvedRightCenter,counters.unresolvedRightRadius)&&
			counters.invalidDenominatorCenter==0.0&&
			counters.invalidDenominatorRadius>=1.0,
			"roundoff trace rejects denominator and branch intervals that cross a decision surface");
	}
	{
		FireProductionRoundoffWalker::BranchWitness witness;
		witness.leftCenter=-7.7486038219110043e-7;
		witness.leftRadius=1.2337798327030971e-6;
		witness.rightCenter=0.0;witness.rightRadius=0.0;
		FireProductionRoundoffWalker::PPMQuadraticZeroCertificate certificate;
		const bool certified=FireProductionRoundoffWalker::CertifyPPMQuadraticZero(
			witness,certificate);
		bool samplesContained=true;
		for(unsigned int sample=0u;sample<=64u;++sample){
			const double s=static_cast<double>(sample)/64.0;
			const double positive=FireProductionRoundoffWalker::PPMQuadraticPathDifference(
				certificate.ambiguityWidth,s);
			const double negative=FireProductionRoundoffWalker::PPMQuadraticPathDifference(
				-certificate.ambiguityWidth,s);
			samplesContained=samplesContained&&std::fabs(positive)<=
				certificate.divergenceBound&&std::fabs(negative)<=
				certificate.divergenceBound;
		}
		const double underBound=0.249*certificate.ambiguityWidth;
		Check(certified&&certificate.continuousAtSwitch&&samplesContained&&
			certificate.arithmeticResidualBound>0.0&&certificate.divergenceBound>
				0.25*certificate.ambiguityWidth&&
			FireProductionRoundoffWalker::PPMQuadraticPathDifference(0.0,0.375)==0.0&&
			std::fabs(FireProductionRoundoffWalker::PPMQuadraticPathDifference(
				certificate.ambiguityWidth,0.5))>underBound,
			"independent PPM branch certificate proves continuity and adds a four-operation rounding residual to the one-quarter envelope");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		auto q=FireProductionRoundoffTrace::TraceFloat::Raw(
			-7.7486038219110043e-7,1.2337798327030971e-6,
			-7.152557373046875e-7f,1u);
		auto minimum=FireProductionRoundoffTrace::TraceFloat(-1.0f);
		auto maximum=FireProductionRoundoffTrace::TraceFloat(1.0f);
		const auto linear=FireProductionRoundoffTrace::TraceFloat(0.0f);
		const auto endpointMinimum=minimum,endpointMaximum=maximum;
		bool nonzero=false;
		{
			FireProductionRoundoffTrace::Scope traceScope(counters);
			FireProductionRoundoffTrace::BeginPPMBranchEnvelope();
			{
				FireProductionRoundoffTrace::BranchSiteScope branchScope(
					FireProductionRoundoffTrace::BranchSite::PPMQuadraticZero);
				nonzero=q!=0.0f;
			}
			FireProductionRoundoffTrace::ApplyPPMQuadraticZeroCertificate(
				q,linear,endpointMinimum,endpointMaximum,minimum,maximum);
		}
		Check(nonzero&&counters.branchObligations.size()==1u&&
			counters.dischargedBranchObligationCount==1u&&
			!counters.unresolvedBranch&&
			counters.branchObligations[0].site==
				FireProductionRoundoffTrace::BranchSite::PPMQuadraticZero&&
			counters.branchObligations[0].certificate==
				FireProductionRoundoffTrace::BranchCertificate::Equivalence&&
			counters.branchObligations[0].divergenceBound>0.0&&
			minimum.Radius()>0.0&&maximum.Radius()>0.0,
			"traced PPM obligation folds the independent equivalence envelope into both path outputs");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		auto q=FireProductionRoundoffTrace::TraceFloat::Raw(
			1.0e-7,2.0e-7,1.0e-7f,1u);
		auto linear=FireProductionRoundoffTrace::TraceFloat::Raw(
			-1.0e-7,2.0e-7,-1.0e-7f,1u);
		auto minimum=FireProductionRoundoffTrace::TraceFloat(-1.0f);
		auto maximum=FireProductionRoundoffTrace::TraceFloat(1.0f);
		const auto endpointMinimum=minimum,endpointMaximum=maximum;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			FireProductionRoundoffTrace::BeginPPMBranchEnvelope();
			bool nonzero=false;
			{ FireProductionRoundoffTrace::BranchSiteScope site(
				FireProductionRoundoffTrace::BranchSite::PPMQuadraticZero);
				nonzero=q!=0.0f; }
			FireProductionRoundoffTrace::SetPPMQuadraticAmbiguous(
				FireProductionRoundoffTrace::PPMQuadraticZeroObligationPending());
			if(nonzero){FireProductionRoundoffTrace::CoveredBranchScope covered(true,true);
				auto stationary=-linear/(2.0f*q);
				bool lower=FireProductionRoundoffTrace::EvaluateBranch(
					FireProductionRoundoffTrace::BranchSite::PPMStationaryLower,
					[&](){return stationary>0.0f;});
				const bool upper=lower&&FireProductionRoundoffTrace::EvaluateBranch(
					FireProductionRoundoffTrace::BranchSite::PPMStationaryUpper,
					[&](){return stationary<1.0f;});
				if(upper){const auto value=(q*stationary+linear)*stationary;
					minimum=std::min(minimum,value);maximum=std::max(maximum,value);}}
			FireProductionRoundoffTrace::ApplyPPMQuadraticZeroCertificate(q,linear,
				endpointMinimum,endpointMaximum,minimum,maximum);
		}
		Check(counters.branchObligations.size()==5u&&
			counters.dischargedBranchObligationCount==5u&&
			counters.branchObligations[0].site==
				FireProductionRoundoffTrace::BranchSite::PPMQuadraticZero&&
			counters.branchObligations[1].site==
				FireProductionRoundoffTrace::BranchSite::PPMStationaryLower&&
			counters.branchObligations[2].site==
				FireProductionRoundoffTrace::BranchSite::PPMStationaryUpper&&
			counters.branchObligations[3].site==
				FireProductionRoundoffTrace::BranchSite::MinimumSelection&&
			counters.branchObligations[4].site==
				FireProductionRoundoffTrace::BranchSite::MaximumSelection&&
			std::isfinite(minimum.Radius())&&std::isfinite(maximum.Radius()),
			"ambiguous PPM path emits and parent-discharges its quadratic, stationary, and extrema-selector obligations with a finite envelope");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		auto q=FireProductionRoundoffTrace::TraceFloat::Raw(
			1.0e-4,1.0e-8,1.0e-4f,1u);
		auto linear=FireProductionRoundoffTrace::TraceFloat::Raw(
			0.0,1.0e-8,0.0f,1u);
		auto minimum=FireProductionRoundoffTrace::TraceFloat(-1.0f);
		auto maximum=FireProductionRoundoffTrace::TraceFloat(1.0f);
		const auto endpointMinimum=minimum,endpointMaximum=maximum;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			FireProductionRoundoffTrace::BeginPPMBranchEnvelope();
			bool nonzero=false;
			{ FireProductionRoundoffTrace::BranchSiteScope site(
				FireProductionRoundoffTrace::BranchSite::PPMQuadraticZero);
				nonzero=q!=0.0f; }
			FireProductionRoundoffTrace::SetPPMQuadraticAmbiguous(
				FireProductionRoundoffTrace::PPMQuadraticZeroObligationPending());
			if(nonzero){FireProductionRoundoffTrace::CoveredBranchScope covered(true,true);
				auto stationary=-linear/(2.0f*q);
				FireProductionRoundoffTrace::EvaluateBranch(
					FireProductionRoundoffTrace::BranchSite::PPMStationaryLower,
					[&](){return stationary>0.0f;});}
			FireProductionRoundoffTrace::ApplyPPMQuadraticZeroCertificate(q,linear,
				endpointMinimum,endpointMaximum,minimum,maximum);
		}
		Check(counters.branchObligations.size()==1u&&
			counters.dischargedBranchObligationCount==1u&&
			counters.branchObligations[0].site==
				FireProductionRoundoffTrace::BranchSite::PPMStationaryLower&&
			counters.branchObligations[0].certificate==
				FireProductionRoundoffTrace::BranchCertificate::Equivalence,
			"resolved nonzero PPM parent still emits and explicitly discharges an ambiguous stationary predicate");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		auto q=FireProductionRoundoffTrace::TraceFloat::Raw(4.0,1.0e-6,4.0f,1u);
		auto linear=FireProductionRoundoffTrace::TraceFloat::Raw(-4.0,1.0e-6,-4.0f,1u);
		const auto left=FireProductionRoundoffTrace::TraceFloat(1.0f);
		auto minimum=FireProductionRoundoffTrace::TraceFloat(0.0f);
		auto maximum=FireProductionRoundoffTrace::TraceFloat(1.0f);
		const auto endpointMinimum=minimum,endpointMaximum=maximum;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			FireProductionRoundoffTrace::BeginPPMBranchEnvelope();
			bool nonzero=false;
			{ FireProductionRoundoffTrace::BranchSiteScope site(
				FireProductionRoundoffTrace::BranchSite::PPMQuadraticZero);
				nonzero=q!=0.0f; }
			FireProductionRoundoffTrace::SetPPMQuadraticAmbiguous(
				FireProductionRoundoffTrace::PPMQuadraticZeroObligationPending());
			if(nonzero){FireProductionRoundoffTrace::CoveredBranchScope covered(true,true);
				auto stationary=-linear/(2.0f*q);
				const bool lower=FireProductionRoundoffTrace::EvaluateBranch(
					FireProductionRoundoffTrace::BranchSite::PPMStationaryLower,
					[&](){return stationary>0.0f;});
				const bool upper=lower&&FireProductionRoundoffTrace::EvaluateBranch(
					FireProductionRoundoffTrace::BranchSite::PPMStationaryUpper,
					[&](){return stationary<1.0f;});
				if(upper){const auto value=(q*stationary+linear)*stationary+left;
					minimum=std::min(minimum,value);maximum=std::max(maximum,value);}}
			FireProductionRoundoffTrace::ApplyPPMQuadraticZeroCertificate(q,linear,
				endpointMinimum,endpointMaximum,minimum,maximum);
		}
		Check(counters.comparisonCount==5u&&counters.branchObligations.size()==1u&&
			counters.dischargedBranchObligationCount==1u&&
			counters.branchObligations[0].site==
				FireProductionRoundoffTrace::BranchSite::MinimumSelection&&
			counters.branchObligations[0].certificate==
				FireProductionRoundoffTrace::BranchCertificate::Equivalence&&
			std::isfinite(minimum.Radius())&&std::isfinite(maximum.Radius()),
			"resolved PPM parent and stationary predicates still emit and parent-discharge an ambiguous extrema selector");
	}
	{
		double lower=0.0,upper=0.0,divergence=0.0;
		const bool walked=FireProductionRoundoffWalker::ContinuousSelectionHull(
			1.0,0.25,1.1,0.2,true,lower,upper,divergence);
		FireProductionRoundoffTrace::Counters counters;
		FireProductionRoundoffTrace::TraceFloat selected;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			selected=std::min(FireProductionRoundoffTrace::TraceFloat::Raw(
				1.0,0.25,1.0f,1u),FireProductionRoundoffTrace::TraceFloat::Raw(
				1.1,0.2,1.1f,1u));
		}
		Check(walked&&lower==0.75&&upper==1.25&&divergence>0.45&&
			counters.branchObligations.size()==1u&&
			counters.dischargedBranchObligationCount==1u&&
			counters.branchObligations[0].site==
				FireProductionRoundoffTrace::BranchSite::MinimumSelection&&
			counters.branchObligations[0].certificate==
				FireProductionRoundoffTrace::BranchCertificate::Equivalence&&
			selected.Center()-selected.Radius()<=lower&&
			selected.Center()+selected.Radius()>=upper,
			"continuous min selector is independently hulled and discharged across an ambiguous predicate");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			FireProductionRoundoffTrace::CoveredBranchScope covered(true);
			const auto resolved=std::min(FireProductionRoundoffTrace::TraceFloat(1.0f),
				FireProductionRoundoffTrace::TraceFloat(2.0f));
			const auto ambiguous=std::max(
				FireProductionRoundoffTrace::TraceFloat::Raw(1.0,0.25,1.0f,1u),
				FireProductionRoundoffTrace::TraceFloat::Raw(1.1,0.2,1.1f,1u));
			Check(resolved.Rounded()==1.0f&&ambiguous.Radius()>0.0,
				"covered-path selector fixtures execute both resolved and ambiguous cases");
		}
		Check(counters.comparisonCount==2u&&counters.branchObligations.size()==1u&&
			counters.dischargedBranchObligationCount==1u&&
			counters.branchObligations[0].site==
				FireProductionRoundoffTrace::BranchSite::MaximumSelection,
			"covered PPM path counts every selector and emits the continuous hull obligation");
	}
	Check(FireProductionRoundoffWalker::CertifyInactiveLimiter(0.75,0.0031,0.004)&&
		!FireProductionRoundoffWalker::CertifyInactiveLimiter(0.75,0.0029,0.004),
		"independent limiter certificate requires the envelope ratio to dominate the shared limiter");
	{
		FireProductionRoundoffTrace::Counters counters;
		FireProductionRoundoffTrace::TraceFloat limited;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			limited=FireProductionRoundoffTrace::ApplyLimiterBranch(
				FireProductionRoundoffTrace::TraceFloat::Raw(0.5,0.1,0.5f,1u),
				FireProductionRoundoffTrace::TraceFloat(2.0f),
				FireProductionRoundoffTrace::TraceFloat(1.0f),
				FireProductionRoundoffTrace::TraceFloat::Raw(
					1.0e-7,2.0e-7,1.0e-7f,1u),true);
		}
		Check(limited.Rounded()==0.5f&&!counters.invalidDomain&&
			counters.comparisonCount==2u&&counters.branchObligations.size()==2u&&
			counters.dischargedBranchObligationCount==2u&&
			counters.branchObligations[0].site==
				FireProductionRoundoffTrace::BranchSite::LimiterPositive&&
			counters.branchObligations[1].site==
				FireProductionRoundoffTrace::BranchSite::MinimumSelection&&
			counters.branchObligations[1].divergenceBound==0.0,
			"limiter no-effect proof owns and explicitly discharges its nested selector without an infinite hull");
	}
	{
		const float positiveScale=22372.0f,negativeScale=1.002f;
		const double positiveAmbiguity=9292.2824737527444*0x1p-24*
			static_cast<double>(positiveScale);
		const double negativeAmbiguity=8645.4622206683161*0x1p-24*
			static_cast<double>(negativeScale);
		FireProductionRoundoffWalker::ContinuousLimiterCertificate positive,negative;
		const bool positiveCertified=FireProductionRoundoffWalker::
			CertifyContinuousLimiterTransition(positiveAmbiguity,0.0,
				positiveScale,positiveScale,0.0f,positive);
		const bool negativeCertified=FireProductionRoundoffWalker::
			CertifyContinuousLimiterTransition(negativeAmbiguity,0.0,
				negativeScale,negativeScale,0.0f,negative);
		Check(positiveCertified&&negativeCertified&&
			positive.requiredUnitFactor<16384.0&&
			negative.requiredUnitFactor<16384.0&&
			positive.continuousAtNegativeWidth&&positive.continuousAtZero&&
			positive.continuousAtPositiveWidth&&
			positive.monotoneForPositiveConsumption,
			"independent r59 limiter walker proves the power-of-two transition contains both frozen site classes and preserves every join");
		FireProductionRoundoffWalker::ContinuousLimiterCertificate undercount;
		Check(!FireProductionRoundoffWalker::CertifyContinuousLimiterTransition(
			positive.width,positive.width,positiveScale,positiveScale,0.0f,undercount),
			"half-width undercount mutant fails the independent continuous-limiter certificate");
		FireProductionRoundoffWalker::ContinuousLimiterCertificate missingRamp,fixedWidth;
		Check(!FireProductionRoundoffWalker::CertifyContinuousLimiterTransition(
			positiveAmbiguity,0.0,positiveScale,positiveScale,0.0f,missingRamp,
			FireProductionRoundoffWalker::LimiterGraphVariant::MissingNegativeRamp)&&
			!FireProductionRoundoffWalker::CertifyContinuousLimiterTransition(
			positiveAmbiguity,0.0,positiveScale,positiveScale,0.0f,fixedWidth,
			FireProductionRoundoffWalker::LimiterGraphVariant::FixedWidthDenominator),
			"independent limiter graph rejects discontinuous-ramp and nonlegacy denominator mutants");
	}
	{
		std::vector<FireProductionRoundoffTrace::TraceFloat> values(4u,
			FireProductionRoundoffTrace::TraceFloat(3.0f)),left(values),right(values);
		FireProductionRoundoffTrace::Counters counters;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			FireProductionRoundoffTrace::TransportProfileScope profile(values,left,right,
				0u,4u,FireProductionRoundoffTrace::TraceFloat(0.0f),
				FireProductionRoundoffTrace::TraceFloat(0.0f));
			const auto partition=FireProductionRoundoffTrace::floor(
				FireProductionRoundoffTrace::TraceFloat::Raw(2.0,0.125,2.0f,1u));
			Check(partition.Rounded()==2.0f,"floor partition fixture executes the upper path");
		}
		FireProductionRoundoffWalker::FloorPartitionCertificate certificate;
		const double profileUpper=std::nextafter(3.0,
			std::numeric_limits<double>::infinity());
		const double predicateCenter=counters.branchObligations.empty()?0.0:
			counters.branchObligations[0].predicateCenter;
		const double predicateRadius=counters.branchObligations.empty()?0.0:
			counters.branchObligations[0].predicateRadius;
		const bool certified=FireProductionRoundoffWalker::CertifyFloorPartition(
			predicateCenter,predicateRadius,profileUpper,4u,certificate);
		if(!(certified&&counters.branchObligations.size()==1u&&
			counters.branchObligations[0].divergenceBound==certificate.totalEnvelope))
			std::fprintf(stderr,"floor certificate diagnostic certified=%d obligations=%zu "
				"trace=%.17g walker=%.17g profile=%.17g\n",certified?1:0,
				counters.branchObligations.size(),counters.branchObligations.empty()?0.0:
				counters.branchObligations[0].divergenceBound,certificate.totalEnvelope,
				profileUpper);
		Check(certified&&counters.branchObligations.size()==1u&&
			counters.dischargedBranchObligationCount==1u&&
			counters.branchObligations[0].site==
				FireProductionRoundoffTrace::BranchSite::FloorBoundary&&
			counters.branchObligations[0].certificate==
				FireProductionRoundoffTrace::BranchCertificate::Equivalence&&
			counters.branchObligations[0].divergenceBound==certificate.totalEnvelope&&
			certificate.continuousAtInteger,
			"independent floor walker matches the traced exact, rounded, and FTZ envelope");
		FireProductionRoundoffWalker::FloorPartitionCertificate half,missingRound,missingCycle;
		Check(!FireProductionRoundoffWalker::CertifyFloorPartition(predicateCenter,
			predicateRadius,profileUpper,4u,
			half,FireProductionRoundoffWalker::FloorGraphVariant::HalfAmbiguity)&&
			!FireProductionRoundoffWalker::CertifyFloorPartition(predicateCenter,
				predicateRadius,profileUpper,4u,
				missingRound,FireProductionRoundoffWalker::FloorGraphVariant::MissingRoundedTerm)&&
			!FireProductionRoundoffWalker::CertifyFloorPartition(predicateCenter,
				predicateRadius,profileUpper,4u,
				missingCycle,FireProductionRoundoffWalker::FloorGraphVariant::MissingCyclePath),
			"floor certificate rejects ambiguity, rounded-path, and cycle-topology undercounts");
	}
	{
		std::vector<FireProductionRoundoffTrace::TraceFloat> values(3u,
			FireProductionRoundoffTrace::TraceFloat(2.0f)),left(values),right(values);
		FireProductionRoundoffTrace::Counters counters;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			FireProductionRoundoffTrace::TransportProfileScope profile(values,left,right,
				0u,3u,FireProductionRoundoffTrace::TraceFloat(0.0f),
				FireProductionRoundoffTrace::TraceFloat(0.0f));
			const auto remaining=FireProductionRoundoffTrace::TraceFloat::Raw(
				0.0,0.0625,0.0f,1u);
			Check(!FireProductionRoundoffTrace::EvaluateRemainingPositiveBranch(
				[&](){return remaining>0.0f;}),
				"remaining-positive fixture executes the rounded inactive path");
		}
		Check(counters.branchObligations.size()==1u,
			"remaining-positive fixture emits exactly one ambiguous obligation");
		const FireProductionRoundoffTrace::BranchObligation obligation=
			counters.branchObligations.empty()?FireProductionRoundoffTrace::BranchObligation():
				counters.branchObligations.front();
		const double profileUpper=std::nextafter(2.0,
			std::numeric_limits<double>::infinity());
		FireProductionRoundoffWalker::RemainingPositiveCertificate certificate;
		const bool certified=FireProductionRoundoffWalker::CertifyRemainingPositive(
			obligation.predicateCenter,obligation.predicateRadius,profileUpper,certificate);
		Check(certified&&counters.branchObligations.size()==1u&&
			counters.dischargedBranchObligationCount==1u&&
			obligation.site==FireProductionRoundoffTrace::BranchSite::RemainingPositive&&
			obligation.certificate==FireProductionRoundoffTrace::BranchCertificate::Equivalence&&
			obligation.divergenceBound==certificate.totalEnvelope&&
			certificate.zeroWidthAtSwitch,
			"remaining-positive walker matches the zero-width exact, rounded, and FTZ envelope");
		FireProductionRoundoffWalker::RemainingPositiveCertificate half,missingCell,missingFTZ;
		Check(!FireProductionRoundoffWalker::CertifyRemainingPositive(
			obligation.predicateCenter,obligation.predicateRadius,profileUpper,half,
			FireProductionRoundoffWalker::RemainingGraphVariant::HalfAmbiguity)&&
			!FireProductionRoundoffWalker::CertifyRemainingPositive(
				obligation.predicateCenter,obligation.predicateRadius,profileUpper,missingCell,
				FireProductionRoundoffWalker::RemainingGraphVariant::MissingCellIntegral)&&
			!FireProductionRoundoffWalker::CertifyRemainingPositive(
				obligation.predicateCenter,obligation.predicateRadius,profileUpper,missingFTZ,
				FireProductionRoundoffWalker::RemainingGraphVariant::MissingFTZ),
			"remaining-positive certificate rejects width, body-topology, and FTZ omissions");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			const std::vector<FireProductionRoundoffTrace::TraceFloat> values={
				FireProductionRoundoffTrace::TraceFloat(3.0f)};
			const std::vector<FireProductionRoundoffTrace::TraceFloat> left=values,right=values;
			FireProductionRoundoffTrace::TransportProfileScope profile(
				values,left,right,0u,1u,0.0f,0.0f);
			const FireProductionRoundoffTrace::TraceFloat courant=
				FireProductionRoundoffTrace::TraceFloat::Raw(0.0,0.01,0.0f,1u);
			Check(FireProductionRoundoffTrace::EvaluateCourantSignBranch(
				[&](){return courant>=FireProductionRoundoffTrace::TraceFloat(0.0f);}),
				"Courant fixture executes the canonical nonnegative signed-zero path");
		}
		Check(counters.branchObligations.size()==1u&&
			counters.dischargedBranchObligationCount==1u&&
			counters.branchObligations.front().site==
				FireProductionRoundoffTrace::BranchSite::CourantNonnegative&&
			counters.branchObligations.front().certificate==
				FireProductionRoundoffTrace::BranchCertificate::Equivalence,
			"trace consumes the Courant two-path equivalence envelope");
		const auto obligation=counters.branchObligations.front();
		FireProductionRoundoffWalker::CourantSignCertificate certificate,half,missingPath,
			missingFTZ,signedZero;
		const double courantProfileUpper=std::nextafter(3.0,
			std::numeric_limits<double>::infinity());
		const bool courantCertified=FireProductionRoundoffWalker::CertifyCourantSign(
			obligation.predicateCenter,obligation.predicateRadius,courantProfileUpper,
			certificate);
		Check(courantCertified&&
			obligation.divergenceBound==certificate.totalEnvelope&&
			certificate.continuousAtZero&&certificate.signedZeroCanonical&&
			certificate.commonSetupOperationCount==8u&&
			certificate.positiveSuccessorOperationCount==32u&&
			certificate.negativeSuccessorOperationCount==40u&&
			certificate.alternatePathOperationCount==48u,
			"independent Courant walker matches the traced two-path envelope");
		Check(!FireProductionRoundoffWalker::CertifyCourantSign(obligation.predicateCenter,
				obligation.predicateRadius,courantProfileUpper,half,
				FireProductionRoundoffWalker::CourantGraphVariant::HalfAmbiguity),
			"Courant certificate rejects a half-ambiguity mutant");
		Check(
			!FireProductionRoundoffWalker::CertifyCourantSign(obligation.predicateCenter,
				obligation.predicateRadius,courantProfileUpper,missingPath,
				FireProductionRoundoffWalker::CourantGraphVariant::MissingNegativePath),
			"Courant certificate rejects a missing-negative-path mutant");
		Check(
			!FireProductionRoundoffWalker::CertifyCourantSign(obligation.predicateCenter,
				obligation.predicateRadius,courantProfileUpper,missingFTZ,
				FireProductionRoundoffWalker::CourantGraphVariant::MissingFTZ),
			"Courant certificate rejects a missing-FTZ mutant");
		Check(
			!FireProductionRoundoffWalker::CertifyCourantSign(obligation.predicateCenter,
				obligation.predicateRadius,courantProfileUpper,signedZero,
				FireProductionRoundoffWalker::CourantGraphVariant::NoncanonicalSignedZero),
			"Courant certificate rejects a noncanonical signed-zero mutant");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			const std::vector<FireProductionRoundoffTrace::TraceFloat> values={
				FireProductionRoundoffTrace::TraceFloat(2.0f)};
			const std::vector<FireProductionRoundoffTrace::TraceFloat> left=values,right=values;
			FireProductionRoundoffTrace::TransportProfileScope profile(
				values,left,right,0u,1u,0.0f,0.0f);
			const FireProductionRoundoffTrace::TraceFloat fraction=
				FireProductionRoundoffTrace::TraceFloat::Raw(0.0,0.01,0.0f,1u);
			Check(!FireProductionRoundoffTrace::EvaluateFractionPositiveBranch(
				[&](){return fraction>FireProductionRoundoffTrace::TraceFloat(0.0f);}),
				"fraction fixture executes the rounded zero-width path");
		}
		Check(counters.branchObligations.size()==1u&&
			counters.dischargedBranchObligationCount==1u&&
			counters.branchObligations.front().site==
				FireProductionRoundoffTrace::BranchSite::FractionPositive&&
			counters.branchObligations.front().certificate==
				FireProductionRoundoffTrace::BranchCertificate::Equivalence,
			"trace consumes the fractional-tail equivalence envelope");
		const auto obligation=counters.branchObligations.front();
		const double profileUpper=std::nextafter(2.0,
			std::numeric_limits<double>::infinity());
		FireProductionRoundoffWalker::FractionPositiveCertificate certificate,half,
			missingTail,missingFTZ;
		Check(FireProductionRoundoffWalker::CertifyFractionPositive(
			obligation.predicateCenter,obligation.predicateRadius,profileUpper,certificate)&&
			obligation.divergenceBound==certificate.totalEnvelope&&
			certificate.zeroWidthAtSwitch&&certificate.tailOperationCount==24u,
			"independent fraction walker matches the traced trailing-slice envelope");
		Check(!FireProductionRoundoffWalker::CertifyFractionPositive(
			obligation.predicateCenter,obligation.predicateRadius,profileUpper,half,
			FireProductionRoundoffWalker::FractionGraphVariant::HalfAmbiguity)&&
			!FireProductionRoundoffWalker::CertifyFractionPositive(
				obligation.predicateCenter,obligation.predicateRadius,profileUpper,missingTail,
				FireProductionRoundoffWalker::FractionGraphVariant::MissingTrailingIntegral)&&
			!FireProductionRoundoffWalker::CertifyFractionPositive(
				obligation.predicateCenter,obligation.predicateRadius,profileUpper,missingFTZ,
				FireProductionRoundoffWalker::FractionGraphVariant::MissingFTZ),
			"fraction certificate rejects half-width, missing-tail, and FTZ mutants");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		const auto center=FireProductionRoundoffTrace::TraceFloat(1.0f);
		const auto left=FireProductionRoundoffTrace::TraceFloat::Raw(1.0,0.01,1.0f,1u);
		const auto right=FireProductionRoundoffTrace::TraceFloat::Raw(1.0,0.02,1.0f,1u);
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			const std::vector<FireProductionRoundoffTrace::TraceFloat> values={center};
			const std::vector<FireProductionRoundoffTrace::TraceFloat> leftValues={left};
			const std::vector<FireProductionRoundoffTrace::TraceFloat> rightValues={right};
			FireProductionRoundoffTrace::TransportProfileScope profile(
				values,leftValues,rightValues,0u,1u,0.0f,0.0f);
			Check(FireProductionRoundoffTrace::EvaluateFlatIntegralBranch(left,center,right),
				"flat-integral fixture executes the rounded shortcut with two ambiguous equalities");
		}
		Check(counters.branchObligations.size()==2u&&
			counters.dischargedBranchObligationCount==2u&&
			counters.branchObligations[0].certificate==
				FireProductionRoundoffTrace::BranchCertificate::Equivalence&&
			counters.branchObligations[1].certificate==
				FireProductionRoundoffTrace::BranchCertificate::Equivalence,
			"trace discharges both executed flat-profile equality obligations");
		const double profileUpper=std::nextafter(std::fabs(right.Center())+right.Radius(),
			std::numeric_limits<double>::infinity());
		FireProductionRoundoffWalker::FlatIntegralCertificate certificate,half,
			missingPolynomial,missingFTZ,discontinuous,mutatedCoefficient;
		Check(FireProductionRoundoffWalker::CertifyFlatIntegral(left.Center(),left.Radius(),
			center.Center(),center.Radius(),right.Center(),right.Radius(),profileUpper,certificate)&&
			counters.branchObligations[0].divergenceBound==certificate.totalEnvelope&&
			counters.branchObligations[1].divergenceBound==certificate.totalEnvelope&&
			certificate.curvedPathOperationCount==32u&&certificate.continuousAtFlatProfile&&
			certificate.productionGraphMatched,
			"independent flat-integral walker matches both traced two-path envelopes");
		Check(!FireProductionRoundoffWalker::CertifyFlatIntegral(left.Center(),left.Radius(),
			center.Center(),center.Radius(),right.Center(),right.Radius(),profileUpper,half,
			FireProductionRoundoffWalker::FlatIntegralGraphVariant::HalfDeviation)&&
			!FireProductionRoundoffWalker::CertifyFlatIntegral(left.Center(),left.Radius(),
				center.Center(),center.Radius(),right.Center(),right.Radius(),profileUpper,missingPolynomial,
				FireProductionRoundoffWalker::FlatIntegralGraphVariant::MissingCurvedPolynomial)&&
			!FireProductionRoundoffWalker::CertifyFlatIntegral(left.Center(),left.Radius(),
				center.Center(),center.Radius(),right.Center(),right.Radius(),profileUpper,missingFTZ,
				FireProductionRoundoffWalker::FlatIntegralGraphVariant::MissingFTZ)&&
			!FireProductionRoundoffWalker::CertifyFlatIntegral(left.Center(),left.Radius(),
				center.Center(),center.Radius(),right.Center(),right.Radius(),profileUpper,discontinuous,
				FireProductionRoundoffWalker::FlatIntegralGraphVariant::DiscontinuousFlatPath)&&
			!FireProductionRoundoffWalker::CertifyFlatIntegral(left.Center(),left.Radius(),
				center.Center(),center.Radius(),right.Center(),right.Radius(),profileUpper,
				mutatedCoefficient,FireProductionRoundoffWalker::FlatIntegralGraphVariant::
					MutatedQuadraticCoefficient),
			"flat-integral certificate rejects deviation, graph, FTZ, discontinuity, and coefficient mutants");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		Check(!FireProductionRoundoffTrace::TraceFloat::Raw(-1.0,0.0,-1.0f,1u).
			NonnegativeByConstruction()&&
			!(FireProductionRoundoffTrace::TraceFloat(1.0f)-
				FireProductionRoundoffTrace::TraceFloat(2.0f)).NonnegativeByConstruction(),
			"raw and subtractive signed arithmetic cannot inherit positive-zero provenance");
		bool negative=false;{
			FireProductionRoundoffTrace::Scope scope(counters);
			FireProductionRoundoffTrace::TraceFloat maximum=0.0f;
			const std::array<FireProductionRoundoffTrace::TraceFloat,3u> residuals={{
				FireProductionRoundoffTrace::TraceFloat::Raw(-0x1p-24,0x1p-22,-0x1p-24f,1u),
				FireProductionRoundoffTrace::TraceFloat::Raw(0.0,0x1p-23,0.0f,1u),
				FireProductionRoundoffTrace::TraceFloat::Raw(0x1p-25,0x1p-22,0x1p-25f,1u)}};
			for(const auto& residual:residuals)maximum=std::max(maximum,std::fabs(residual));
			negative=FireProductionRoundoffTrace::EvaluateNonnegativeReductionGuard(maximum);
		}
		Check(!negative&&!counters.branchObligations.empty()&&
			counters.branchObligations.back().site==
				FireProductionRoundoffTrace::BranchSite::NonnegativeReductionGuard&&
			counters.branchObligations.back().certificate==
				FireProductionRoundoffTrace::BranchCertificate::Equivalence&&
			counters.branchObligations.back().divergenceBound==0.0,
			"trace discharges the negative projection-reduction guard from abs/max provenance");
		const std::vector<double> residuals={-3.0,2.0,-0.5};
		FireProductionRoundoffWalker::NonnegativeReductionCertificate certificate,
			signedLeaf,negativeSeed,subtractive;
		Check(FireProductionRoundoffWalker::CertifyNonnegativeMaximumReduction(
			residuals,certificate)&&certificate.leafCount==3u&&
			certificate.absoluteCount==3u&&certificate.maximumCount==3u&&
			certificate.positiveZeroSeed&&certificate.allLeavesAbsolute&&
			certificate.maximumOnly&&certificate.totalEnvelope==0.0,
			"independent walker proves the positive-zero abs/max reduction topology");
		Check(!FireProductionRoundoffWalker::CertifyNonnegativeMaximumReduction(
			residuals,signedLeaf,
			FireProductionRoundoffWalker::NonnegativeReductionGraphVariant::SignedLeaf)&&
			!FireProductionRoundoffWalker::CertifyNonnegativeMaximumReduction(
				residuals,negativeSeed,
				FireProductionRoundoffWalker::NonnegativeReductionGraphVariant::NegativeSeed)&&
			!FireProductionRoundoffWalker::CertifyNonnegativeMaximumReduction(
				residuals,subtractive,
				FireProductionRoundoffWalker::NonnegativeReductionGraphVariant::SubtractiveReduction),
			"projection-reduction proof rejects signed-leaf, negative-seed, and subtract mutants");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		{
			FireProductionRoundoffTrace::Scope scope(counters);
			Check(FireProductionRoundoffTrace::EvaluateContinuousInflowJoin(
				FireProductionRoundoffTrace::TraceFloat::Raw(-0.25,0.01,-0.25f,1u),
				FireProductionRoundoffTrace::TraceFloat(0.25f),
				FireProductionRoundoffTrace::TraceFloat(1.0f),
				FireProductionRoundoffTrace::TraceFloat(3.0f),true),
				"continuous-inflow fixture executes the rounded nearest-donor join");
		}
		Check(counters.branchObligations.size()==1u,
			"continuous-inflow join emits one ambiguous obligation");
		const FireProductionRoundoffTrace::BranchObligation obligation=
			counters.branchObligations.empty()?FireProductionRoundoffTrace::BranchObligation():
				counters.branchObligations.front();
		FireProductionRoundoffWalker::InflowTransitionCertificate certificate;
		const bool certified=FireProductionRoundoffWalker::CertifyInflowTransition(
			obligation.predicateCenter,obligation.predicateRadius,0.25,0.0,
			1.0,0.0,3.0,0.0,certificate);
		Check(certified&&counters.dischargedBranchObligationCount==1u&&
			obligation.site==FireProductionRoundoffTrace::BranchSite::InflowSign&&
			obligation.certificate==FireProductionRoundoffTrace::BranchCertificate::Reformulation&&
			obligation.divergenceBound==certificate.totalEnvelope&&
			certificate.continuousAtJoin&&certificate.convexDonorBlend&&
			certificate.legacyOutsideWidth,
			"independent inflow walker matches the traced convex join envelope");
		FireProductionRoundoffTrace::Counters upperCounters;
		{
			FireProductionRoundoffTrace::Scope scope(upperCounters);
			Check(!FireProductionRoundoffTrace::EvaluateContinuousInflowJoin(
				FireProductionRoundoffTrace::TraceFloat::Raw(0.25,0.01,0.249f,1u),
				FireProductionRoundoffTrace::TraceFloat(0.25f),
				FireProductionRoundoffTrace::TraceFloat(1.0f),
				FireProductionRoundoffTrace::TraceFloat(3.0f),false),
				"continuous-inflow fixture executes the rounded ramp side of the ambient join");
		}
		Check(upperCounters.branchObligations.size()==1u&&
			upperCounters.dischargedBranchObligationCount==1u&&
			upperCounters.branchObligations.front().certificate==
				FireProductionRoundoffTrace::BranchCertificate::Reformulation&&
			upperCounters.branchObligations.front().divergenceBound==
				certificate.totalEnvelope,
			"both continuous donor joins consume the same independently derived envelope");
		FireProductionRoundoffWalker::InflowTransitionCertificate half,missingContrast,
			missingRounded,binary;
		Check(!FireProductionRoundoffWalker::CertifyInflowTransition(
			obligation.predicateCenter,obligation.predicateRadius,0.25,0.0,1.0,0.0,
			3.0,0.0,half,FireProductionRoundoffWalker::InflowGraphVariant::HalfAmbiguity)&&
			!FireProductionRoundoffWalker::CertifyInflowTransition(
				obligation.predicateCenter,obligation.predicateRadius,0.25,0.0,1.0,0.0,
				3.0,0.0,missingContrast,
				FireProductionRoundoffWalker::InflowGraphVariant::MissingDonorContrast)&&
			!FireProductionRoundoffWalker::CertifyInflowTransition(
				obligation.predicateCenter,obligation.predicateRadius,0.25,0.0,1.0,0.0,
				3.0,0.0,missingRounded,
				FireProductionRoundoffWalker::InflowGraphVariant::MissingRoundedTerm)&&
			!FireProductionRoundoffWalker::CertifyInflowTransition(
				obligation.predicateCenter,obligation.predicateRadius,0.25,0.0,1.0,0.0,
				3.0,0.0,binary,
				FireProductionRoundoffWalker::InflowGraphVariant::DiscontinuousBinary),
			"inflow certificate rejects width, donor, rounding, and binary-branch mutants");
	}
	{
		FireProductionRoundoffWalker::ProjectionInterpolationCertificate certificate,
			shifted,reassociated;
		const float exactBoundary=(4.0f+0.5f)*5.0f/9.0f-0.5f;
		std::uint64_t physical=0u,restoration=0u;
		Check(exactBoundary==2.0f&&
			FireProductionRoundoffWalker::CertifyProjectionInterpolationFloor(
				4u,9u,5u,2,exactBoundary,certificate)&&
			certificate.exactNumerator==36&&certificate.exactDenominator==18&&
			certificate.exactAndRoundedSameSide&&
			FireProductionRoundoffWalker::CountProjectionInterpolationObligations(
				24u,24u,36u,17u,physical)&&physical==765u&&
			FireProductionRoundoffWalker::CountProjectionInterpolationObligations(
				24u,24u,36u,16u,restoration)&&restoration==720u,
			"independent projection interpolation walker proves fixed-grid floor topology");
		Check(!FireProductionRoundoffWalker::CertifyProjectionInterpolationFloor(
				4u,9u,5u,2,exactBoundary,shifted,
				FireProductionRoundoffWalker::ProjectionInterpolationGraphVariant::ShiftedFine)&&
			!FireProductionRoundoffWalker::CertifyProjectionInterpolationFloor(
				2u,3u,2u,1,(2.0f+0.5f)*2.0f/3.0f-0.5f,reassociated,
				FireProductionRoundoffWalker::ProjectionInterpolationGraphVariant::
					ReassociatedDivision),
			"projection interpolation certificate rejects coordinate and association mutants");
	}
	{
		FireProductionRoundoffTrace::Counters counters;
		FireProductionRoundoffTrace::TraceFloat affected,unrelated;
		{
			FireProductionRoundoffTrace::Scope traceScope(counters);
			{
				FireProductionRoundoffTrace::LocalTransportBranchScope localScope;
				FireProductionRoundoffTrace::RecordBranchDivergence(
					FireProductionRoundoffTrace::BranchSite::FlatIntegral,0.25);
				affected=FireProductionRoundoffTrace::FinalizeTransportBranchEnvelope(
					FireProductionRoundoffTrace::TraceFloat(1.0f));
			}
			unrelated=FireProductionRoundoffTrace::FinalizeTransportBranchEnvelope(
				FireProductionRoundoffTrace::TraceFloat(1.0f));
		}
		Check(affected.Radius()>0.25&&unrelated.Radius()==0.0,
			"local branch envelope affects only the swept integral that owns it");
		std::vector<FireProductionRoundoffTrace::TraceFloat> nonfinite={
			FireProductionRoundoffTrace::TraceFloat::Raw(1.0,
				std::numeric_limits<double>::quiet_NaN(),1.0f,1u)};
		FireProductionRoundoffTrace::Counters metricCounters;{
			FireProductionRoundoffTrace::Scope traceScope(metricCounters);
			FireProductionRoundoffTrace::ObserveMetricRangeAndReset(nonfinite,0u,1u,9u);
		}
		Check(std::isinf(metricCounters.firstNonfiniteMetricOutputCenter[9])==false&&
			metricCounters.nonfiniteMetricOutputRadiusCount[9]==1u&&
			metricCounters.invalidDomain,
			"NaN interval radii canonicalize to a fail-closed unbounded metric");
	}
	using namespace FireProductionCalibration;
	double radius=0.0;
	const RoundoffStage stages[]={{1.25,0x1p-22,24u},{2.0,0x1p-21,48u}};
	Check(ComposeRoundoffBound(stages,2u,radius)&&radius>
		2.0*(1.25*0.0+0x1p-22)+0x1p-21,"outward roundoff recurrence");
	const RoundoffStage invalid[]={{1.0,1.0,16777216u}};
	Check(!ComposeRoundoffBound(invalid,1u,radius)&&radius==0.0,
		"roundoff derivation rejects n*u >= 1");

	double order=0.0,distance=0.0;
	double subdominance=0.0;
	Check(Tier6DistanceFromDyadicPairs(0.005031015340016892,
		0.0041988689735742426,1.8,distance,subdominance)&&
		distance==0.005890459160549289&&subdominance==0.0007363073950686612&&
		!Tier6DistanceFromDyadicPairs(0.0,0.0041988689735742426,1.8,
			distance,subdominance),
		"r137 derives the tier-6 production distance and exact one-eighth subdominance bound");
	const double h5=0.05,h6=1.0/24.0,h7=1.0/28.0;
	const double d56=h5*h5-h6*h6,d67=h6*h6-h7*h7;
	const bool gridRichardson=GeneralizedGridRichardson(d56,d67,h5,h6,h7,2.0,order,distance);
	const double measuredGridEstimate=d67/(1.0-std::pow(h7/h6,2.0));
	if(!(gridRichardson&&std::fabs(order-2.0)<1.0e-12&&distance>measuredGridEstimate))
		std::fprintf(stderr,"grid diagnostic ok=%d p=%.17g E=%.17g expected=%.17g\n",
			gridRichardson?1:0,order,distance,h6*h6);
	Check(gridRichardson&&std::fabs(order-2.0)<1.0e-12&&distance>measuredGridEstimate,
		"generalized three-grid Richardson recovers capped order and tier-6 distance");
	Check(!GeneralizedGridRichardson(d67,d56,h5,h6,h7,2.0,order,distance),
		"non-asymptotic grid differences block calibration");
	const double positiveOrderFloor=std::log(h5/h6)/std::log(h6/h7);
	Check(!GeneralizedGridRichardson(positiveOrderFloor*0.99,1.0,h5,h6,h7,2.0,
		order,distance),"ratio below the positive-order limit blocks calibration");
	Check(GeneralizedGridRichardson(2.0,1.0,h5,h6,h7,2.0,order,distance)&&
		order==2.0,"apparent superconvergence is capped at formal order");
	Check(!GeneralizedGridRichardson(0.95,1.0,h5,h6,h7,2.0,order,distance)&&
		GeneralizedGridRichardson(d56,d67,h5,h6,h7,2.0,order,distance),
		"a non-asymptotic input family cannot stand in for an evolved-output gate");
	DyadicDistanceEstimate pairA,pairB;
	Check(DyadicDistanceAtVerifiedOrder(0.25,1.8,pairA)&&
		DyadicDistanceAtVerifiedOrder(0.20,1.8,pairB)&&
		pairA.coarseDistance>pairA.fineRadius&&
		DyadicLimitBallsOverlap(pairA.fineRadius+pairB.fineRadius,pairA,pairB)&&
		!DyadicLimitBallsOverlap(std::nextafter(NextUp(pairA.fineRadius+pairB.fineRadius),
			std::numeric_limits<double>::infinity()),pairA,pairB),
		"dyadic verified-order estimates own an exact independent-limit overlap rule");
	Check(TemporalRichardson(0.75,0.1875,2.0,order,distance)&&order==2.0&&
		distance>=1.0,"three-level temporal Richardson uses the baseline distance");
	FilteredTemporalDistanceMode temporalMode=FilteredTemporalDistanceMode::Rejected;
	Check(FilteredTemporalDistance(0.75,0.1875,2.0,nullptr,order,distance,temporalMode)&&
		temporalMode==FilteredTemporalDistanceMode::Richardson&&order==2.0&&distance>=1.0,
		"filtered temporal contraction retains the registered Richardson rule");
	const double floorCoarse=0.0012312438866646748;
	const double floorFine=0.001273209006325096;
	const FilteredTemporalPlateauAuthority floorAuthority={floorCoarse,floorFine};
	Check(FilteredTemporalDistance(floorCoarse,floorFine,2.0,&floorAuthority,order,distance,
			temporalMode)&&temporalMode==FilteredTemporalDistanceMode::MeasuredFloorUpperBound&&
		order==0.0&&distance==NextUp(floorFine)&&distance>floorFine&&
		!FilteredTemporalDistance(floorCoarse,floorFine,2.0,nullptr,order,distance,temporalMode)&&
		!FilteredTemporalDistance(floorCoarse,2.0*floorFine,2.0,&floorAuthority,order,
			distance,temporalMode),
		"filtered temporal plateau needs its exact evidence authority and uses the outward floor");
	double tolerance=0.0;
	Check(TriangleTolerance(1.0,2.0,3.0,4.0,5.0,tolerance)&&tolerance>15.0,
		"triangle tolerance is an outward sum rather than a maximum");

	RISE::FireProductionRemapRequest fp32;
	fp32.lineLength=5u;fp32.lineCount=2u;fp32.componentCount=2u;
	fp32.cellWidthM=0.25f;fp32.timeStepS=0.125f;
	fp32.boundary=RISE::FireProductionRemapPeriodic;
	fp32.values.assign(20u,0.125f);fp32.faceVelocityMPerS.assign(12u,0.5f);
	fp32.ambientValues.assign(2u,0.125f);
	RISE::FireProductionRemapResult fp32Result;std::string error;
	Check(RISE::RemapFireProductionCPU(fp32,fp32Result,&error),
		"binary32 remap comparator accepts exact free stream");
	RISEFireProductionFP64::FireProductionRemapRequest fp64;
	fp64.lineLength=fp32.lineLength;fp64.lineCount=fp32.lineCount;
	fp64.componentCount=fp32.componentCount;fp64.cellWidthM=fp32.cellWidthM;
	fp64.timeStepS=fp32.timeStepS;
	fp64.boundary=RISEFireProductionFP64::FireProductionRemapPeriodic;
	fp64.values.assign(fp32.values.begin(),fp32.values.end());
	fp64.faceVelocityMPerS.assign(fp32.faceVelocityMPerS.begin(),fp32.faceVelocityMPerS.end());
	fp64.ambientValues.assign(fp32.ambientValues.begin(),fp32.ambientValues.end());
	RISEFireProductionFP64::FireProductionRemapResult fp64Result;
	Check(RISEFireProductionFP64::RemapFireProductionCPU(fp64,fp64Result,&error)&&
		fp64Result.updatedValues.size()==fp32Result.updatedValues.size()&&
		std::all_of(fp64Result.updatedValues.begin(),fp64Result.updatedValues.end(),
			[](double value){return value==0.125;}),
		"generated binary64 remap preserves the production free-stream topology");

	RISEFireProductionTrace::FireProductionRemapRequest traced;
	traced.lineLength=fp32.lineLength;traced.lineCount=fp32.lineCount;
	traced.componentCount=fp32.componentCount;traced.cellWidthM=fp32.cellWidthM;
	traced.timeStepS=fp32.timeStepS;
	traced.boundary=RISEFireProductionTrace::FireProductionRemapPeriodic;
	traced.values.assign(fp32.values.begin(),fp32.values.end());
	traced.faceVelocityMPerS.assign(fp32.faceVelocityMPerS.begin(),
		fp32.faceVelocityMPerS.end());
	traced.ambientValues.assign(fp32.ambientValues.begin(),fp32.ambientValues.end());
	RISEFireProductionTrace::FireProductionRemapResult tracedResult;
	FireProductionRoundoffTrace::Counters tracedCounters;
	bool tracedOK=false;{
		FireProductionRoundoffTrace::Scope scope(tracedCounters);
		tracedOK=RISEFireProductionTrace::RemapFireProductionCPU(traced,tracedResult,&error);
	}
	bool tracedBytes=tracedResult.updatedValues.size()==fp32Result.updatedValues.size();
	for(std::size_t i=0u;tracedBytes&&i<tracedResult.updatedValues.size();++i)
		tracedBytes=tracedResult.updatedValues[i].Rounded()==fp32Result.updatedValues[i]&&
			tracedResult.updatedValues[i].Radius()>=0.0&&
			std::isfinite(tracedResult.updatedValues[i].Radius());
	std::uint64_t tracedOperations=0u;
	for(const std::uint64_t count:tracedCounters.operation)tracedOperations+=count;
	const std::array<std::uint64_t,13u> expectedTraceKinds={{
		20u,196u,220u,232u,112u,0u,140u,0u,0u,40u,0u,20u,0u}};
	FireProductionRoundoffWalker::Topology walkedTopology;
	const bool walked=FireProductionRoundoffWalker::
		WalkPeriodicPositiveSubcellFreeStreamRemap(fp32.lineLength,fp32.lineCount,
			fp32.componentCount,walkedTopology);
	if(!(walked&&tracedOperations==walkedTopology.operationCount&&
		tracedCounters.maximumDepth==walkedTopology.maximumDepth))
		std::fprintf(stderr,"roundoff topology diagnostic traced=%llu/%u walked=%llu/%u\n",
			static_cast<unsigned long long>(tracedOperations),tracedCounters.maximumDepth,
			static_cast<unsigned long long>(walkedTopology.operationCount),
			walkedTopology.maximumDepth);
	Check(tracedOK&&tracedBytes&&tracedOperations==980u&&
		std::equal(expectedTraceKinds.begin(),expectedTraceKinds.end(),
			std::begin(tracedCounters.operation))&&tracedCounters.maximumDepth==14u&&
		tracedCounters.comparisonCount==644u&&!tracedCounters.unresolvedBranch&&
		tracedCounters.branchObligations.size()==260u&&
		tracedCounters.dischargedBranchObligationCount==260u&&
		tracedCounters.minimumDenominatorLowerBound>0.0&&
		*std::max_element(std::begin(tracedCounters.maximumAbsoluteOperand),
			std::end(tracedCounters.maximumAbsoluteOperand))==5.0&&
		!tracedCounters.invalidDomain&&walked&&
		tracedOperations==walkedTopology.operationCount&&
		std::equal(std::begin(tracedCounters.operation),std::end(tracedCounters.operation),
			std::begin(walkedTopology.operation))&&
		tracedCounters.maximumDepth==walkedTopology.maximumDepth,
		"independent remap graph walk reproduces traced operation count and depth while the trace reproduces fp32 bytes");
	if(!(tracedOK&&tracedBytes&&tracedOperations==980u&&
		tracedCounters.comparisonCount==644u&&!tracedCounters.unresolvedBranch&&
		tracedCounters.branchObligations.size()==260u&&
		tracedCounters.dischargedBranchObligationCount==260u&&
		!tracedCounters.invalidDomain))std::fprintf(stderr,
		"roundoff remap detail ok=%d bytes=%d ops=%llu comparisons=%llu obligations=%zu "
		"discharged=%llu unresolved=%d invalid=%d denominator=%.17g max_operand=%.17g\n",
		tracedOK?1:0,tracedBytes?1:0,static_cast<unsigned long long>(tracedOperations),
		static_cast<unsigned long long>(tracedCounters.comparisonCount),
		tracedCounters.branchObligations.size(),static_cast<unsigned long long>(
			tracedCounters.dischargedBranchObligationCount),
		tracedCounters.unresolvedBranch?1:0,tracedCounters.invalidDomain?1:0,
		tracedCounters.minimumDenominatorLowerBound,
		*std::max_element(std::begin(tracedCounters.maximumAbsoluteOperand),
			std::end(tracedCounters.maximumAbsoluteOperand)));
	FireProductionRoundoffWalker::Topology omittedAdd=walkedTopology;
	--omittedAdd.operation[FireProductionRoundoffWalker::Add];--omittedAdd.operationCount;
	Check(!std::equal(std::begin(tracedCounters.operation),std::end(tracedCounters.operation),
		std::begin(omittedAdd.operation))&&omittedAdd.operationCount!=tracedOperations,
		"independent topology gate rejects an omitted arithmetic operation");

	RISE::FireProductionCellPalindromeRequest cell32;
	cell32.shape.nx=5u;cell32.shape.ny=6u;cell32.shape.nz=7u;
	cell32.shape.cellWidthM=0.2f;cell32.componentCount=9u;cell32.timeStepS=0.01f;
	cell32.boundary.fill(RISE::FireProductionProjectionPressureOpen);
	const std::size_t cellCount=cell32.shape.CellCount();
	cell32.conservativeValues.resize(cell32.componentCount*cellCount);
	cell32.ambientValues.resize(cell32.componentCount);
	for(std::size_t component=0u;component<cell32.componentCount;++component){
		const float base=component==8u?300000.0f:static_cast<float>(component+1u);
		cell32.ambientValues[component]=base;
		for(std::size_t z=0u;z<cell32.shape.nz;++z)
			for(std::size_t y=0u;y<cell32.shape.ny;++y)
				for(std::size_t x=0u;x<cell32.shape.nx;++x){
					const std::size_t cell=(z*cell32.shape.ny+y)*cell32.shape.nx+x;
					cell32.conservativeValues[component*cellCount+cell]=base*(1.0f+
						0.02f*static_cast<float>(x+2u*y+3u*z));
				}
	}
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t faces=RISE::FireProductionProjectionFaceCount(cell32.shape,axis);
		cell32.frozenVelocityMPerS[axis].resize(faces);
		for(std::size_t face=0u;face<faces;++face)
			cell32.frozenVelocityMPerS[axis][face]=0.01f*static_cast<float>(axis+1u);
	}
	RISE::FireProductionCellPalindromeResult cell32Result;
	Check(RISE::RemapFireProductionCellPalindromeCPU(cell32,cell32Result,&error),
		"binary32 cell palindrome accepts the stage-reset witness");
	RISEFireProductionTrace::FireProductionCellPalindromeRequest tracedCell;
	tracedCell.shape.nx=cell32.shape.nx;tracedCell.shape.ny=cell32.shape.ny;
	tracedCell.shape.nz=cell32.shape.nz;tracedCell.shape.cellWidthM=cell32.shape.cellWidthM;
	tracedCell.componentCount=cell32.componentCount;tracedCell.timeStepS=cell32.timeStepS;
	for(unsigned int side=0u;side<6u;++side)tracedCell.boundary[side]=
		static_cast<RISEFireProductionTrace::FireProductionProjectionBoundary>(
			cell32.boundary[side]);
	tracedCell.conservativeValues.assign(cell32.conservativeValues.begin(),
		cell32.conservativeValues.end());
	tracedCell.ambientValues.assign(cell32.ambientValues.begin(),cell32.ambientValues.end());
	for(unsigned int axis=0u;axis<3u;++axis)tracedCell.frozenVelocityMPerS[axis].assign(
		cell32.frozenVelocityMPerS[axis].begin(),cell32.frozenVelocityMPerS[axis].end());
	RISEFireProductionTrace::FireProductionCellPalindromeResult tracedCellResult;
	FireProductionRoundoffTrace::Counters tracedCellCounters;bool tracedCellOK=false;{
		FireProductionRoundoffTrace::Scope scope(tracedCellCounters);
		tracedCellOK=RISEFireProductionTrace::RemapFireProductionCellPalindromeCPU(
			tracedCell,tracedCellResult,&error);
	}
	bool tracedCellBytes=tracedCellResult.conservativeValues.size()==
		cell32Result.conservativeValues.size();
	for(std::size_t value=0u;tracedCellBytes&&value<tracedCellResult.conservativeValues.size();++value)
		tracedCellBytes=tracedCellResult.conservativeValues[value].Rounded()==
			cell32Result.conservativeValues[value]&&
			tracedCellResult.conservativeValues[value].Radius()==0.0;
	bool completeStages=tracedCellCounters.sealedStages.size()==5u;
	for(const FireProductionRoundoffTrace::Observation& stage:tracedCellCounters.sealedStages){
		std::uint64_t operations=0u;for(const std::uint64_t count:stage.operation)operations+=count;
		completeStages=completeStages&&operations>0u&&stage.maximumDepth>0u&&
			stage.maximumOutputRadius>0.0&&std::isfinite(stage.maximumOutputRadius)&&
			std::isfinite(stage.maximumAbsoluteOutput);
	}
	Check(tracedCellOK&&tracedCellBytes&&completeStages,
		"cell palindrome trace seals five local certificates and resets radii without changing fp32 bytes");

	// Exact Section 3.7 bootstrap.  Every arithmetic operand is dyadic, so the
	// strict-binary32 production seam must equal the live binary64 oracle without
	// a tolerance.  The asymmetric pattern distinguishes low/high/shared-alpha,
	// normal and transverse I_i, frozen carrier velocity, and wall/open handling.
	auto productionFaceIndex=[]( const RISE::FireProductionProjectionShape& shape,
		const unsigned int axis, const std::size_t x, const std::size_t y,
		const std::size_t z ) {
		if(axis==0u)return (z*shape.ny+y)*(shape.nx+1u)+x;
		if(axis==1u)return (z*(shape.ny+1u)+y)*shape.nx+x;
		return (z*shape.ny+y)*shape.nx+x;
	};
	auto sameDoubleBits=[]( const double first, const double second ) {
		std::uint64_t firstBits=0u,secondBits=0u;
		std::memcpy(&firstBits,&first,sizeof(firstBits));
		std::memcpy(&secondBits,&second,sizeof(secondBits));
		return firstBits==secondBits;
	};
	auto sameFloatBits=[]( const float first, const float second ) {
		std::uint32_t firstBits=0u,secondBits=0u;
		std::memcpy(&firstBits,&first,sizeof(firstBits));
		std::memcpy(&secondBits,&second,sizeof(secondBits));
		return firstBits==secondBits;
	};
	RISE::FireProductionCompatibleFCTMomentumRequest openFCT32;
	openFCT32.shape.nx=4u;openFCT32.shape.ny=4u;openFCT32.shape.nz=4u;
	openFCT32.shape.cellWidthM=0.5f;
	openFCT32.boundary={{RISE::FireProductionProjectionWall,
		RISE::FireProductionProjectionPressureOpen,
		RISE::FireProductionProjectionPressureOpen,RISE::FireProductionProjectionWall,
		RISE::FireProductionProjectionWall,RISE::FireProductionProjectionPressureOpen}};
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t faces=RISE::FireProductionProjectionFaceCount(openFCT32.shape,axis);
		openFCT32.lowGasFluxKGPerM2S[axis].assign(faces,0.0f);
		openFCT32.highGasFluxKGPerM2S[axis].assign(faces,0.0f);
		openFCT32.sharedFaceAlpha[axis].assign(faces,0.0f);
		openFCT32.frozenVelocityMPerS[axis].assign(faces,0.0f);
	}
	const auto setOpenX=[&](const std::size_t x,const float low,const float high,
		const float alpha,const float velocity){
		const std::size_t face=productionFaceIndex(openFCT32.shape,0u,x,1u,1u);
		openFCT32.lowGasFluxKGPerM2S[0][face]=low;
		openFCT32.highGasFluxKGPerM2S[0][face]=high;
		openFCT32.sharedFaceAlpha[0][face]=alpha;
		openFCT32.frozenVelocityMPerS[0][face]=velocity;
	};
	setOpenX(0u,0.0f,0.0f,0.0f,0.0f);setOpenX(1u,1.0f,3.0f,0.5f,1.0f);
	setOpenX(2u,2.0f,6.0f,0.25f,3.0f);setOpenX(3u,4.0f,0.0f,1.0f,2.0f);
	setOpenX(4u,8.0f,16.0f,0.0f,4.0f);
	// Duplicate the normal pattern on y=3 so its analytic 9/80 sentinels are
	// isolated from the transverse y-flux pattern used at y=1.
	for(std::size_t x=0u;x<=4u;++x){
		const std::size_t source=productionFaceIndex(openFCT32.shape,0u,x,1u,1u);
		const std::size_t destination=productionFaceIndex(openFCT32.shape,0u,x,3u,1u);
		openFCT32.lowGasFluxKGPerM2S[0][destination]=
			openFCT32.lowGasFluxKGPerM2S[0][source];
		openFCT32.highGasFluxKGPerM2S[0][destination]=
			openFCT32.highGasFluxKGPerM2S[0][source];
		openFCT32.sharedFaceAlpha[0][destination]=openFCT32.sharedFaceAlpha[0][source];
		openFCT32.frozenVelocityMPerS[0][destination]=
			openFCT32.frozenVelocityMPerS[0][source];
	}
	openFCT32.frozenVelocityMPerS[0][productionFaceIndex(
		openFCT32.shape,0u,2u,0u,1u)]=1.0f;
	openFCT32.frozenVelocityMPerS[0][productionFaceIndex(
		openFCT32.shape,0u,2u,2u,1u)]=5.0f;
	const auto setOpenY=[&](const std::size_t x,const std::size_t y,const float low,
		const float high,const float alpha){
		const std::size_t face=productionFaceIndex(openFCT32.shape,1u,x,y,1u);
		openFCT32.lowGasFluxKGPerM2S[1][face]=low;
		openFCT32.highGasFluxKGPerM2S[1][face]=high;
		openFCT32.sharedFaceAlpha[1][face]=alpha;
	};
	setOpenY(1u,1u,0.0f,2.0f,0.5f);setOpenY(2u,1u,2.0f,4.0f,0.5f);
	setOpenY(1u,2u,2.0f,8.0f,0.0f);setOpenY(2u,2u,0.0f,6.0f,1.0f);
	RISE::FireProductionCompatibleFCTMomentumResult openFCTResult;
	const bool openFCTOK=RISE::EvaluateFireProductionCompatibleFCTMomentumCPU(
		openFCT32,openFCTResult,&error);
	RISE::FireSim::PeriodicMACShape openOracleShape;
	openOracleShape.nx=4u;openOracleShape.ny=4u;openOracleShape.nz=4u;
	openOracleShape.cellWidthM=0.5;
	RISE::FireSim::OpenFluxPair3D openOracleFlux;
	std::array<std::vector<double>,3> openOracleAlpha;
	RISE::FireSim::OpenMACField3D openOracleVelocity;
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t faces=RISE::FireSim::OpenMACFaceCount3D(openOracleShape,axis);
		openOracleFlux.low[axis].assign(faces,RISE::FireSim::ConservativeVector());
		openOracleFlux.high[axis].assign(faces,RISE::FireSim::ConservativeVector());
		openOracleFlux.nonadvectiveMass[axis].assign(faces,
			std::array<double,RISE::FireSim::MethaneMassStateDimension>());
		openOracleFlux.nonadvectiveEnergy[axis].assign(faces,0.0);
		openOracleAlpha[axis].resize(faces);
		openOracleVelocity.component[axis].resize(faces);
		for(std::size_t face=0u;face<faces;++face){
			const double physical=openFCT32.physicalGasFluxKGPerM2S[axis].empty()?0.0:
				openFCT32.physicalGasFluxKGPerM2S[axis][face];
			openOracleFlux.low[axis][face][1u+RISE::FireSim::MethaneCH4]=
				openFCT32.lowGasFluxKGPerM2S[axis][face]+physical;
			openOracleFlux.high[axis][face][1u+RISE::FireSim::MethaneCH4]=
				openFCT32.highGasFluxKGPerM2S[axis][face]+physical;
			openOracleFlux.nonadvectiveMass[axis][face][1u+RISE::FireSim::MethaneCH4]=
				physical;
			openOracleAlpha[axis][face]=openFCT32.sharedFaceAlpha[axis][face];
			openOracleVelocity.component[axis][face]=
				openFCT32.frozenVelocityMPerS[axis][face];
		}
	}
	RISE::FireSim::OpenBoundaryConfig3D openOracleBoundary;
	for(unsigned int side=0u;side<6u;++side)openOracleBoundary.kind[side]=
		openFCT32.boundary[side]==RISE::FireProductionProjectionPressureOpen?
		RISE::FireSim::PressureOpenBoundary3D:RISE::FireSim::AdiabaticWallBoundary3D;
	const RISE::FireSim::OpenMACField3D openOracleRate=
		RISE::FireSim::OpenCompatibleMomentumFluxDivergence3D(openOracleShape,
			openOracleFlux,openOracleAlpha,openOracleVelocity,&openOracleBoundary);
	bool openFCTExact=openFCTOK;
	for(unsigned int axis=0u;axis<3u&&openFCTExact;++axis){
		const std::size_t faces=RISE::FireProductionProjectionFaceCount(openFCT32.shape,axis);
		openFCTExact=openFCTResult.advectionRateKGPerM2S2[axis].size()==faces&&
			openFCTResult.acceptedGasFluxKGPerM2S[axis].size()==faces;
		for(std::size_t face=0u;face<faces&&openFCTExact;++face){
			const double physical=openFCT32.physicalGasFluxKGPerM2S[axis].empty()?0.0:
				openFCT32.physicalGasFluxKGPerM2S[axis][face];
			const double accepted=openFCT32.lowGasFluxKGPerM2S[axis][face]+
				openFCT32.sharedFaceAlpha[axis][face]*(
				openFCT32.highGasFluxKGPerM2S[axis][face]-
				openFCT32.lowGasFluxKGPerM2S[axis][face])+physical;
			openFCTExact=sameDoubleBits(static_cast<double>(
				openFCTResult.acceptedGasFluxKGPerM2S[axis][face]),accepted)&&
				sameDoubleBits(static_cast<double>(
				openFCTResult.advectionRateKGPerM2S2[axis][face]),
				openOracleRate.component[axis][face]);
		}
	}
	const std::size_t openX1=productionFaceIndex(openFCT32.shape,0u,1u,3u,1u);
	const std::size_t openX2=productionFaceIndex(openFCT32.shape,0u,2u,1u,1u);
	const std::size_t openX4=productionFaceIndex(openFCT32.shape,0u,4u,3u,1u);
	const std::size_t openX0=productionFaceIndex(openFCT32.shape,0u,0u,3u,1u);
	Check(openFCTExact&&openFCTResult.advectionRateKGPerM2S2[0][openX1]==9.0f&&
		openFCTResult.advectionRateKGPerM2S2[0][openX2]==21.5f&&
		openFCTResult.advectionRateKGPerM2S2[0][openX4]==40.0f&&
		openFCTResult.advectionRateKGPerM2S2[0][openX0]==0.0f&&
		!std::signbit(openFCTResult.advectionRateKGPerM2S2[0][openX0]),
		"compatible FCT open/wall operator is word-exact to the live oracle and analytic sentinels");

	// Periodic storage has a publication plane while the live oracle owns one
	// canonical face per cell.  Compare every canonical face and the exact seam.
	RISE::FireProductionCompatibleFCTMomentumRequest periodicFCT32;
	periodicFCT32.shape=openFCT32.shape;
	periodicFCT32.boundary.fill(RISE::FireProductionProjectionPeriodic);
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t faces=RISE::FireProductionProjectionFaceCount(periodicFCT32.shape,axis);
		periodicFCT32.lowGasFluxKGPerM2S[axis].assign(faces,0.0f);
		periodicFCT32.highGasFluxKGPerM2S[axis].assign(faces,0.0f);
		periodicFCT32.sharedFaceAlpha[axis].assign(faces,0.0f);
		periodicFCT32.frozenVelocityMPerS[axis].assign(faces,0.0f);
	}
	periodicFCT32.physicalGasFluxKGPerM2S[0].assign(
		RISE::FireProductionProjectionFaceCount(periodicFCT32.shape,0u),0.0f);
	const float periodicLow[4]={0.5f,0.5f,1.5f,7.5f};
	const float periodicHigh[4]={2.5f,2.5f,5.5f,0.0f};
	const float periodicAlpha[4]={0.0f,0.5f,0.5f,0.0f};
	const float periodicVelocity[4]={1.0f,3.0f,2.0f,4.0f};
	for(std::size_t x=0u;x<4u;++x){
		const std::size_t face=productionFaceIndex(periodicFCT32.shape,0u,x,1u,1u);
		periodicFCT32.lowGasFluxKGPerM2S[0][face]=periodicLow[x];
		periodicFCT32.highGasFluxKGPerM2S[0][face]=periodicHigh[x];
		periodicFCT32.physicalGasFluxKGPerM2S[0][face]=0.5f;
		periodicFCT32.sharedFaceAlpha[0][face]=periodicAlpha[x];
		periodicFCT32.frozenVelocityMPerS[0][face]=periodicVelocity[x];
	}
	const std::size_t periodicLowSeam=productionFaceIndex(periodicFCT32.shape,0u,0u,1u,1u);
	const std::size_t periodicHighSeam=productionFaceIndex(periodicFCT32.shape,0u,4u,1u,1u);
	periodicFCT32.lowGasFluxKGPerM2S[0][periodicHighSeam]=
		periodicFCT32.lowGasFluxKGPerM2S[0][periodicLowSeam];
	periodicFCT32.highGasFluxKGPerM2S[0][periodicHighSeam]=
		periodicFCT32.highGasFluxKGPerM2S[0][periodicLowSeam];
	periodicFCT32.physicalGasFluxKGPerM2S[0][periodicHighSeam]=
		periodicFCT32.physicalGasFluxKGPerM2S[0][periodicLowSeam];
	periodicFCT32.sharedFaceAlpha[0][periodicHighSeam]=
		periodicFCT32.sharedFaceAlpha[0][periodicLowSeam];
	periodicFCT32.frozenVelocityMPerS[0][periodicHighSeam]=
		periodicFCT32.frozenVelocityMPerS[0][periodicLowSeam];
	RISE::FireProductionCompatibleFCTMomentumResult periodicFCTResult;
	const bool periodicFCTOK=RISE::EvaluateFireProductionCompatibleFCTMomentumCPU(
		periodicFCT32,periodicFCTResult,&error);
	RISE::FireSim::PeriodicMACShape periodicOracleShape=openOracleShape;
	std::array<std::vector<double>,3> periodicAcceptedAdvection,periodicPhysical;
	RISE::FireSim::PeriodicMACField periodicOracleVelocity;
	for(unsigned int axis=0u;axis<3u;++axis){
		periodicAcceptedAdvection[axis].assign(periodicOracleShape.CellCount(),0.0);
		periodicPhysical[axis].assign(periodicOracleShape.CellCount(),0.0);
		periodicOracleVelocity.component[axis].assign(periodicOracleShape.CellCount(),0.0);
		for(std::size_t z=0u;z<4u;++z)for(std::size_t y=0u;y<4u;++y)
			for(std::size_t x=0u;x<4u;++x){
				const std::size_t cell=(z*4u+y)*4u+x;
				const std::size_t face=productionFaceIndex(periodicFCT32.shape,axis,x,y,z);
				periodicAcceptedAdvection[axis][cell]=
					periodicFCT32.lowGasFluxKGPerM2S[axis][face]+
					periodicFCT32.sharedFaceAlpha[axis][face]*(
					periodicFCT32.highGasFluxKGPerM2S[axis][face]-
					periodicFCT32.lowGasFluxKGPerM2S[axis][face]);
				periodicPhysical[axis][cell]=periodicFCT32.physicalGasFluxKGPerM2S[axis].empty()?0.0:
					periodicFCT32.physicalGasFluxKGPerM2S[axis][face];
				periodicOracleVelocity.component[axis][cell]=
					periodicFCT32.frozenVelocityMPerS[axis][face];
			}
	}
	const std::array<std::vector<double>,3> periodicOracleRate=
		RISE::FireSim::CompatibleMomentumFluxDivergence3D(periodicOracleShape,
			periodicAcceptedAdvection,periodicPhysical,periodicOracleVelocity);
	bool periodicFCTExact=periodicFCTOK;
	for(unsigned int axis=0u;axis<3u&&periodicFCTExact;++axis){
		for(std::size_t z=0u;z<4u&&periodicFCTExact;++z)
			for(std::size_t y=0u;y<4u&&periodicFCTExact;++y)
				for(std::size_t x=0u;x<4u&&periodicFCTExact;++x){
					const std::size_t cell=(z*4u+y)*4u+x;
					const std::size_t face=productionFaceIndex(periodicFCT32.shape,axis,x,y,z);
					periodicFCTExact=sameDoubleBits(static_cast<double>(periodicFCTResult.
						acceptedGasFluxKGPerM2S[axis][face]),
						periodicAcceptedAdvection[axis][cell]+periodicPhysical[axis][cell])&&
						sameDoubleBits(static_cast<double>(periodicFCTResult.
						advectionRateKGPerM2S2[axis][face]),periodicOracleRate[axis][cell]);
				}
		const std::size_t firstEnd=axis==0u?periodicFCT32.shape.ny:
			periodicFCT32.shape.nx;
		const std::size_t secondEnd=axis==2u?periodicFCT32.shape.ny:
			periodicFCT32.shape.nz;
		for(std::size_t second=0u;second<secondEnd&&periodicFCTExact;++second)
			for(std::size_t first=0u;first<firstEnd&&periodicFCTExact;++first){
				std::size_t lx=axis==0u?0u:first;
				std::size_t ly=axis==0u?first:(axis==1u?0u:second);
				std::size_t lz=axis==2u?0u:second;
				std::size_t hx=lx,hy=ly,hz=lz;
				if(axis==0u)hx=4u;if(axis==1u)hy=4u;if(axis==2u)hz=4u;
				const std::size_t low=productionFaceIndex(
					periodicFCT32.shape,axis,lx,ly,lz);
				const std::size_t high=productionFaceIndex(
					periodicFCT32.shape,axis,hx,hy,hz);
				periodicFCTExact=sameFloatBits(
					periodicFCTResult.acceptedGasFluxKGPerM2S[axis][low],
					periodicFCTResult.acceptedGasFluxKGPerM2S[axis][high])&&
					sameFloatBits(periodicFCTResult.advectionRateKGPerM2S2[axis][low],
					periodicFCTResult.advectionRateKGPerM2S2[axis][high]);
			}
	}
	const std::size_t periodicX0=productionFaceIndex(periodicFCT32.shape,0u,0u,1u,1u);
	const std::size_t periodicX1=productionFaceIndex(periodicFCT32.shape,0u,1u,1u,1u);
	Check(periodicFCTExact&&periodicFCTResult.advectionRateKGPerM2S2[0][periodicX0]==-16.5f&&
		periodicFCTResult.advectionRateKGPerM2S2[0][periodicX1]==9.0f,
		"compatible FCT periodic operator is word-exact to the live oracle, analytic sentinels, and seam");

	// Fail closed before publication, and prove the fixture distinguishes the
	// rejected all-high (shared-alpha ignored) mutation from the admitted form.
	RISE::FireProductionCompatibleFCTMomentumRequest malformedFCT=periodicFCT32;
	malformedFCT.sharedFaceAlpha[0][periodicX1]=1.25f;
	RISE::FireProductionCompatibleFCTMomentumResult refusedFCT;
	refusedFCT.acceptedGasFluxKGPerM2S[0].assign(1u,123.0f);
	const bool invalidAlphaRefused=!RISE::EvaluateFireProductionCompatibleFCTMomentumCPU(
		malformedFCT,refusedFCT,&error)&&refusedFCT.acceptedGasFluxKGPerM2S[0].empty()&&
		refusedFCT.advectionRateKGPerM2S2[0].empty();
	malformedFCT=periodicFCT32;
	malformedFCT.lowGasFluxKGPerM2S[0][periodicHighSeam]+=1.0f;
	const bool badSeamRefused=!RISE::EvaluateFireProductionCompatibleFCTMomentumCPU(
		malformedFCT,refusedFCT,&error);
	malformedFCT=periodicFCT32;
	malformedFCT.lowGasFluxKGPerM2S[1][productionFaceIndex(
		malformedFCT.shape,1u,0u,4u,0u)]=-0.0f;
	const bool signedZeroSeamRefused=
		!RISE::EvaluateFireProductionCompatibleFCTMomentumCPU(
			malformedFCT,refusedFCT,&error);
	malformedFCT=periodicFCT32;
	malformedFCT.boundary[2]=RISE::FireProductionProjectionWall;
	malformedFCT.boundary[3]=RISE::FireProductionProjectionWall;
	const bool hybridRefused=!RISE::EvaluateFireProductionCompatibleFCTMomentumCPU(
		malformedFCT,refusedFCT,&error);
	malformedFCT=periodicFCT32;
	malformedFCT.physicalGasFluxKGPerM2S[1].assign(1u,0.0f);
	const bool badPhysicalShapeRefused=!RISE::EvaluateFireProductionCompatibleFCTMomentumCPU(
		malformedFCT,refusedFCT,&error);
	RISE::FireProductionCompatibleFCTMomentumRequest allHighFCT=periodicFCT32;
	for(unsigned int axis=0u;axis<3u;++axis)allHighFCT.lowGasFluxKGPerM2S[axis]=
		allHighFCT.highGasFluxKGPerM2S[axis];
	RISE::FireProductionCompatibleFCTMomentumResult allHighResult;
	const bool allHighComputed=RISE::EvaluateFireProductionCompatibleFCTMomentumCPU(
		allHighFCT,allHighResult,&error);
	Check(invalidAlphaRefused&&badSeamRefused&&signedZeroSeamRefused&&hybridRefused&&
		badPhysicalShapeRefused&&allHighComputed&&
		allHighResult.advectionRateKGPerM2S2[0][periodicX1]!=
		periodicFCTResult.advectionRateKGPerM2S2[0][periodicX1],
		"compatible FCT bootstrap refuses malformed identity inputs and detects an all-high limiter mutant");

	// Full scalar FCT bootstrap.  The synthetic one-coordinate nullspace uses
	// B=(1/2,1/2) and coordinate projector 2, so rho-total and CH4 reconstruct
	// together with exact dyadic arithmetic while all nine tuple rows traverse
	// the same donor/MC, shared-alpha, source-commit stages.
	RISE::FireProductionScalarFCTRequest scalarFCT;
	scalarFCT.shape=periodicFCT32.shape;scalarFCT.timeStepS=0.25f;
	scalarFCT.boundary.fill(RISE::FireProductionProjectionPeriodic);
	const std::size_t scalarCells=scalarFCT.shape.CellCount();
	scalarFCT.beginning.assign(9u*scalarCells,0.0f);
	scalarFCT.sourceDelta.assign(9u*scalarCells,0.0f);
	const float scalarPattern[4]={1.0f,1.25f,1.5f,1.25f};
	for(std::size_t z=0u;z<4u;++z)for(std::size_t y=0u;y<4u;++y)
		for(std::size_t x=0u;x<4u;++x){
			const std::size_t cell=(z*4u+y)*4u+x;
			scalarFCT.beginning[cell]=scalarPattern[x];
			scalarFCT.beginning[scalarCells+cell]=scalarPattern[x];
		}
	scalarFCT.sourceDelta[0u]=0.125f;
	scalarFCT.sourceDelta[scalarCells]=0.125f;
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t faces=RISE::FireProductionProjectionFaceCount(scalarFCT.shape,axis);
		scalarFCT.frozenVelocityMPerS[axis].assign(faces,axis==0u?1.0f:0.0f);
	}
	for(unsigned int side=0u;side<6u;++side){
		const std::size_t faces=side<2u?scalarFCT.shape.ny*scalarFCT.shape.nz:
			(side<4u?scalarFCT.shape.nx*scalarFCT.shape.nz:
				scalarFCT.shape.nx*scalarFCT.shape.ny);
		scalarFCT.pressureOpenInflow[side].assign(faces,0u);
	}
	scalarFCT.ambient[0]=1.0f;scalarFCT.ambient[1]=1.0f;
	scalarFCT.nullity=1u;scalarFCT.nullspaceBasis.assign(8u,0.0f);
	scalarFCT.nullspaceBasis[0]=0.5f;scalarFCT.nullspaceBasis[1]=0.5f;
	scalarFCT.coordinateProjector.assign(1u,2.0f);
	scalarFCT.feasibilityFactor=1.0f/1024.0f;
	scalarFCT.assemblyReserveFactor=1.0f/2048.0f;
	RISE::FireProductionScalarFCTResult scalarFCTResult;
	const bool scalarFCTOK=RISE::EvaluateFireProductionScalarFCTCPU(
		scalarFCT,scalarFCTResult,&error);
	const std::size_t scalarAllFaces=scalarFCTOK?scalarFCTResult.lowFlux.size()/9u:0u;
	const std::size_t scalarFace2=productionFaceIndex(scalarFCT.shape,0u,2u,0u,0u);
	const std::size_t scalarPackedFace2=scalarFCTResult.packedFaceOffset[0]+scalarFace2;
	bool allScalarAlphaOne=scalarFCTOK;
	for(unsigned int axis=0u;axis<3u&&allScalarAlphaOne;++axis)
		for(const float alpha:scalarFCTResult.sharedFaceAlpha[axis])
			allScalarAlphaOne=allScalarAlphaOne&&alpha==1.0f;
	Check(scalarFCTOK&&allScalarAlphaOne&&scalarFCTResult.fluxDelta[
		scalarPackedFace2]==0.125f&&scalarFCTResult.fluxDelta[
		scalarAllFaces+scalarPackedFace2]==0.125f&&
		scalarFCTResult.commutingIdentityAvailable&&
		scalarFCTResult.maximumCommutingResidualKGPerM3==0.0f,
		"strict fp32 scalar FCT reconstructs the dyadic nullspace, shares alpha, and commutes exactly");

	// Prerequisite-only f_N/J_g producer. It consumes fixed binary32 Q,T,D,k,u,
	// verifies the canonical fp64 N_C N_C^T reference envelope, and retains the
	// binary32 publication residual separately. It is not a projected-Heun owner.
	RISE::FireProductionProjectionShape physicalUnderCap,physicalOverCap;
	physicalUnderCap.nx=76u;physicalUnderCap.ny=279u;physicalUnderCap.nz=560u;
	physicalUnderCap.cellWidthM=1.0f;
	physicalOverCap.nx=132u;physicalOverCap.ny=224u;physicalOverCap.nz=402u;
	physicalOverCap.cellWidthM=1.0f;
	std::uint64_t physicalUnderBytes=0u,physicalOverBytes=0u;
	const bool physicalAdmissionQuery=
		RISE::QueryFireProductionScalarPhysicalFluxPrerequisiteCPUWorkingSetBytes(
			physicalUnderCap,physicalUnderBytes,&error)&&
		RISE::QueryFireProductionScalarPhysicalFluxPrerequisiteCPUWorkingSetBytes(
			physicalOverCap,physicalOverBytes,&error);
	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest emptyOverCap;
	emptyOverCap.shape=physicalOverCap;
	RISE::FireProductionScalarPhysicalFluxPrerequisiteResult rejectedOverCap;
	rejectedOverCap.physicalMassFluxKGPerM2S.assign(1u,1.0f);
	rejectedOverCap.fp64ReferenceIdentityVerified=true;
	const bool physicalOverCapCallRejected=
		!RISE::BuildFireProductionScalarPhysicalFluxPrerequisiteCPU(
			emptyOverCap,rejectedOverCap,&error);
	const bool physicalOverCapCleared=
		rejectedOverCap.physicalMassFluxKGPerM2S.empty()&&
		!rejectedOverCap.fp64ReferenceIdentityVerified;
	const bool physicalOverCapError=
		error.find("working set exceeds two GiB")!=std::string::npos;
	Check(physicalAdmissionQuery&&physicalUnderBytes==UINT64_C(2147483384)&&
		physicalOverBytes==UINT64_C(2147483760)&&
		physicalUnderBytes<=(UINT64_C(2)<<30u)&&
		physicalOverBytes>(UINT64_C(2)<<30u),
		"physical scalar-flux prerequisite admits and rejects the exact two-GiB working-set neighbors without allocation");
	Check(physicalOverCapCallRejected&&physicalOverCapCleared&&physicalOverCapError,
		"physical scalar-flux prerequisite rejects an empty over-cap request before payload validation");

	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest physicalFluxRequest;
	physicalFluxRequest.shape=scalarFCT.shape;
	physicalFluxRequest.boundary.fill(RISE::FireProductionProjectionPeriodic);
	physicalFluxRequest.conservativeValues.assign(9u*scalarCells,0.0f);
	physicalFluxRequest.temperatureK.assign(scalarCells,300.0f);
	physicalFluxRequest.diffusivityM2PerS.assign(scalarCells,0.01f);
	physicalFluxRequest.conductivityWPerMK.assign(scalarCells,0.03f);
	physicalFluxRequest.ambientTemperatureK=300.0f;
	for(std::size_t z=0u;z<4u;++z)for(std::size_t y=0u;y<4u;++y)
		for(std::size_t x=0u;x<4u;++x){const std::size_t cell=(z*4u+y)*4u+x;
			const float methane=(x&1u)?0.2f:0.1f;
			physicalFluxRequest.conservativeValues[cell]=methane;
			physicalFluxRequest.conservativeValues[scalarCells+cell]=methane;
			physicalFluxRequest.conservativeValues[2u*scalarCells+cell]=0.2f;
			physicalFluxRequest.conservativeValues[3u*scalarCells+cell]=0.8f-methane;
			physicalFluxRequest.temperatureK[cell]=300.0f+25.0f*static_cast<float>(x);
		}
	physicalFluxRequest.ambient[0]=0.1f;physicalFluxRequest.ambient[1]=0.1f;
	physicalFluxRequest.ambient[2]=0.2f;physicalFluxRequest.ambient[3]=0.7f;
	for(unsigned int axis=0u;axis<3u;++axis){
		physicalFluxRequest.frozenVelocityMPerS[axis].assign(
			RISE::FireProductionProjectionFaceCount(physicalFluxRequest.shape,axis),0.0f);
	}
	for(unsigned int side=0u;side<6u;++side)
		physicalFluxRequest.pressureOpenInflow[side].assign(16u,0u);
	RISE::FireProductionScalarPhysicalFluxPrerequisiteResult physicalFluxResult;
	const bool physicalFluxOK=RISE::BuildFireProductionScalarPhysicalFluxPrerequisiteCPU(
		physicalFluxRequest,physicalFluxResult,&error);
	const std::size_t physicalAllFaces=physicalFluxOK?
		physicalFluxResult.physicalEnergyFluxWPerM2.size():0u;
	bool physicalNonzero=false,physicalSeamExact=physicalFluxOK,
		physicalGasExact=physicalFluxOK;
	for(float value:physicalFluxResult.physicalMassFluxKGPerM2S)
		physicalNonzero=physicalNonzero||value!=0.0f;
	for(float value:physicalFluxResult.physicalEnergyFluxWPerM2)
		physicalNonzero=physicalNonzero||value!=0.0f;
	for(unsigned int axis=0u;axis<3u&&physicalGasExact;++axis)
		for(std::size_t face=0u;face<physicalFluxResult.physicalGasFluxKGPerM2S[axis].size();
			++face){float gas=0.0f;const std::size_t packed=
				physicalFluxResult.packedFaceOffset[axis]+face;
			for(std::size_t component=1u;component<=6u;++component)gas+=
				physicalFluxResult.physicalMassFluxKGPerM2S[component*physicalAllFaces+packed];
			physicalGasExact=physicalGasExact&&sameFloatBits(gas,
				physicalFluxResult.physicalGasFluxKGPerM2S[axis][face]);
		}
	for(unsigned int axis=0u;axis<3u&&physicalSeamExact;++axis){
		const std::size_t extent=4u,firstEnd=4u,secondEnd=4u;
		for(std::size_t second=0u;second<secondEnd&&physicalSeamExact;++second)
			for(std::size_t first=0u;first<firstEnd&&physicalSeamExact;++first){
				std::size_t lx=axis==0u?0u:first,ly=axis==0u?first:(axis==1u?0u:second),
					lz=axis==2u?0u:second,hx=lx,hy=ly,hz=lz;
				if(axis==0u)hx=extent;if(axis==1u)hy=extent;if(axis==2u)hz=extent;
				const std::size_t low=physicalFluxResult.packedFaceOffset[axis]+
					productionFaceIndex(physicalFluxRequest.shape,axis,lx,ly,lz);
				const std::size_t high=physicalFluxResult.packedFaceOffset[axis]+
					productionFaceIndex(physicalFluxRequest.shape,axis,hx,hy,hz);
				for(std::size_t component=0u;component<8u;++component)physicalSeamExact=
					physicalSeamExact&&sameFloatBits(physicalFluxResult.physicalMassFluxKGPerM2S[
						component*physicalAllFaces+low],physicalFluxResult.physicalMassFluxKGPerM2S[
						component*physicalAllFaces+high]);
				physicalSeamExact=physicalSeamExact&&sameFloatBits(
					physicalFluxResult.physicalEnergyFluxWPerM2[low],
					physicalFluxResult.physicalEnergyFluxWPerM2[high]);
			physicalSeamExact=physicalSeamExact&&sameFloatBits(
				physicalFluxResult.physicalGasFluxKGPerM2S[axis][
					productionFaceIndex(physicalFluxRequest.shape,axis,lx,ly,lz)],
				physicalFluxResult.physicalGasFluxKGPerM2S[axis][
					productionFaceIndex(physicalFluxRequest.shape,axis,hx,hy,hz)]);
		}
	}
	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest uniformPhysical=physicalFluxRequest;
	for(std::size_t component=0u;component<9u;++component)
		for(std::size_t cell=1u;cell<scalarCells;++cell)uniformPhysical.conservativeValues[
			component*scalarCells+cell]=uniformPhysical.conservativeValues[component*scalarCells];
	std::fill(uniformPhysical.temperatureK.begin(),uniformPhysical.temperatureK.end(),300.0f);
	RISE::FireProductionScalarPhysicalFluxPrerequisiteResult uniformPhysicalResult;
	const bool uniformPhysicalOK=RISE::BuildFireProductionScalarPhysicalFluxPrerequisiteCPU(
		uniformPhysical,uniformPhysicalResult,&error);
	const bool uniformPhysicalZero=uniformPhysicalOK&&std::all_of(
		uniformPhysicalResult.physicalMassFluxKGPerM2S.begin(),
		uniformPhysicalResult.physicalMassFluxKGPerM2S.end(),[](float value){return value==0.0f;})&&
		std::all_of(uniformPhysicalResult.physicalEnergyFluxWPerM2.begin(),
		uniformPhysicalResult.physicalEnergyFluxWPerM2.end(),[](float value){return value==0.0f;});
	Check(physicalFluxOK&&physicalNonzero&&physicalSeamExact&&physicalGasExact&&
		uniformPhysicalZero&&
		physicalFluxResult.fp64ReferenceIdentityVerified&&
		physicalFluxResult.maximumFP64ReferenceResidualKGPerM2S<=
			physicalFluxResult.fp64ReferenceForwardErrorBoundKGPerM2S&&
		physicalFluxResult.maximumConstraintResidualKGPerM2S<=
			physicalFluxResult.constraintForwardErrorBoundKGPerM2S,
		"physical scalar-flux prerequisite verifies fp64 N_C identity, publishes nonzero f_N/J_g, canonicalizes periodic seams, and collapses uniform data to zero");

	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest openPhysical=physicalFluxRequest;
	openPhysical.boundary[0]=RISE::FireProductionProjectionWall;
	openPhysical.boundary[1]=RISE::FireProductionProjectionPressureOpen;
	std::fill(openPhysical.pressureOpenInflow[1].begin(),
		openPhysical.pressureOpenInflow[1].end(),1u);
	RISE::FireProductionScalarPhysicalFluxPrerequisiteResult openPhysicalResult;
	const bool openPhysicalOK=RISE::BuildFireProductionScalarPhysicalFluxPrerequisiteCPU(
		openPhysical,openPhysicalResult,&error);
	bool wallPhysicalZero=openPhysicalOK,openInflowNonzero=false;
	for(std::size_t z=0u;z<4u;++z)for(std::size_t y=0u;y<4u;++y){
		const std::size_t lowFace=productionFaceIndex(openPhysical.shape,0u,0u,y,z),
			highFace=productionFaceIndex(openPhysical.shape,0u,4u,y,z),
			lowPacked=openPhysicalResult.packedFaceOffset[0]+lowFace,
			highPacked=openPhysicalResult.packedFaceOffset[0]+highFace;
		for(std::size_t component=0u;component<8u;++component){
			wallPhysicalZero=wallPhysicalZero&&
				openPhysicalResult.physicalMassFluxKGPerM2S[
					component*physicalAllFaces+lowPacked]==0.0f;
			openInflowNonzero=openInflowNonzero||
				openPhysicalResult.physicalMassFluxKGPerM2S[
					component*physicalAllFaces+highPacked]!=0.0f;
		}
		wallPhysicalZero=wallPhysicalZero&&
			openPhysicalResult.physicalEnergyFluxWPerM2[lowPacked]==0.0f&&
			openPhysicalResult.physicalGasFluxKGPerM2S[0][lowFace]==0.0f;
		openInflowNonzero=openInflowNonzero||
			openPhysicalResult.physicalEnergyFluxWPerM2[highPacked]!=0.0f||
			openPhysicalResult.physicalGasFluxKGPerM2S[0][highFace]!=0.0f;
	}
	std::fill(openPhysical.pressureOpenInflow[1].begin(),
		openPhysical.pressureOpenInflow[1].end(),0u);
	RISE::FireProductionScalarPhysicalFluxPrerequisiteResult outflowPhysicalResult;
	const bool outflowPhysicalOK=RISE::BuildFireProductionScalarPhysicalFluxPrerequisiteCPU(
		openPhysical,outflowPhysicalResult,&error);
	bool outflowPhysicalZero=outflowPhysicalOK;
	for(std::size_t z=0u;z<4u;++z)for(std::size_t y=0u;y<4u;++y){
		const std::size_t face=productionFaceIndex(openPhysical.shape,0u,4u,y,z),
			packed=outflowPhysicalResult.packedFaceOffset[0]+face;
		for(std::size_t component=0u;component<8u;++component)outflowPhysicalZero=
			outflowPhysicalZero&&outflowPhysicalResult.physicalMassFluxKGPerM2S[
				component*physicalAllFaces+packed]==0.0f;
		outflowPhysicalZero=outflowPhysicalZero&&
			outflowPhysicalResult.physicalEnergyFluxWPerM2[packed]==0.0f&&
			outflowPhysicalResult.physicalGasFluxKGPerM2S[0][face]==0.0f;
	}
	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest badPhysical=physicalFluxRequest;
	badPhysical.diffusivityM2PerS.back()=std::numeric_limits<float>::quiet_NaN();
	RISE::FireProductionScalarPhysicalFluxPrerequisiteResult failedPhysical=physicalFluxResult;
	const bool failedPhysicalRejected=!RISE::BuildFireProductionScalarPhysicalFluxPrerequisiteCPU(
		badPhysical,failedPhysical,&error)&&failedPhysical.physicalMassFluxKGPerM2S.empty()&&
		failedPhysical.physicalEnergyFluxWPerM2.empty()&&
		failedPhysical.physicalGasFluxKGPerM2S[0].empty()&&
		!failedPhysical.fp64ReferenceIdentityVerified&&!error.empty();
	auto rejectsPhysicalTemperature=[&](
		const RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest& candidate){
		RISE::FireProductionScalarPhysicalFluxPrerequisiteResult rejected=physicalFluxResult;
		return !RISE::BuildFireProductionScalarPhysicalFluxPrerequisiteCPU(
			candidate,rejected,&error)&&rejected.physicalMassFluxKGPerM2S.empty()&&
			rejected.physicalEnergyFluxWPerM2.empty()&&
			rejected.physicalGasFluxKGPerM2S[0].empty()&&
			!rejected.fp64ReferenceIdentityVerified;
	};
	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest belowCell=physicalFluxRequest,
		aboveCell=physicalFluxRequest,belowAmbient=physicalFluxRequest,
		aboveAmbient=physicalFluxRequest,maximumEndpoint=uniformPhysical;
	belowCell.temperatureK.back()=std::nextafter(300.0f,0.0f);
	aboveCell.temperatureK.back()=std::nextafter(5000.0f,
		std::numeric_limits<float>::infinity());
	belowAmbient.ambientTemperatureK=std::nextafter(300.0f,0.0f);
	aboveAmbient.ambientTemperatureK=std::nextafter(5000.0f,
		std::numeric_limits<float>::infinity());
	std::fill(maximumEndpoint.temperatureK.begin(),maximumEndpoint.temperatureK.end(),5000.0f);
	maximumEndpoint.ambientTemperatureK=5000.0f;
	RISE::FireProductionScalarPhysicalFluxPrerequisiteResult maximumEndpointResult;
	const bool maximumEndpointAccepted=
		RISE::BuildFireProductionScalarPhysicalFluxPrerequisiteCPU(
			maximumEndpoint,maximumEndpointResult,&error);
	Check(openPhysicalOK&&wallPhysicalZero&&openInflowNonzero&&outflowPhysicalZero&&
		failedPhysicalRejected&&rejectsPhysicalTemperature(belowCell)&&
		rejectsPhysicalTemperature(aboveCell)&&rejectsPhysicalTemperature(belowAmbient)&&
		rejectsPhysicalTemperature(aboveAmbient)&&maximumEndpointAccepted,
		"physical scalar-flux prerequisite seals boundaries, enforces the inclusive PhysicalV1 temperature domain, and fails atomically");

#if defined(__APPLE__)
	RISE::FireProductionScalarPhysicalFluxPrerequisiteResult unsupportedPhysical=physicalFluxResult;
	const bool unsupportedPhysicalRejected=
		!RISE::BuildFireProductionScalarPhysicalFluxPrerequisiteMetal(
			physicalFluxRequest,unsupportedPhysical,&error)&&
		unsupportedPhysical.physicalMassFluxKGPerM2S.empty()&&
		!unsupportedPhysical.fp64ReferenceIdentityVerified&&
		error.find("fp64 identity qualification")!=std::string::npos;
	Check(unsupportedPhysicalRejected,
		"physical scalar-flux Metal prerequisite fails closed until fp64 qualification exists");
#endif

	RISE::FireProductionScalarFCTRequest noSourceScalarFCT=scalarFCT;
	noSourceScalarFCT.sourceDelta.assign(9u*scalarCells,0.0f);
	RISE::FireProductionScalarFCTResult noSourceScalarResult;
	const bool noSourceScalarOK=RISE::EvaluateFireProductionScalarFCTCPU(
		noSourceScalarFCT,noSourceScalarResult,&error);
	RISE::FireProductionScalarFCTFluxPair sourcedFluxPair,noSourceFluxPair;
	const bool sourcedFluxPairOK=RISE::BuildFireProductionScalarFCTFluxPairCPU(
		scalarFCT,sourcedFluxPair,&error);
	const bool noSourceFluxPairOK=RISE::BuildFireProductionScalarFCTFluxPairCPU(
		noSourceScalarFCT,noSourceFluxPair,&error);
	RISE::FireProductionScalarFCTResult stagedSourceCommit;
	const bool stagedSourceCommitOK=sourcedFluxPairOK&&
		RISE::SolveFireProductionScalarFCTFluxPairCPU(
			scalarFCT,sourcedFluxPair,stagedSourceCommit,&error);
	Check(noSourceScalarOK&&sourcedFluxPairOK&&noSourceFluxPairOK&&
		sourcedFluxPair.lowFlux==noSourceFluxPair.lowFlux&&
		sourcedFluxPair.fluxDelta==noSourceFluxPair.fluxDelta&&stagedSourceCommitOK&&
		stagedSourceCommit.accepted==scalarFCTResult.accepted&&
		scalarFCTResult.accepted[0u]-
		noSourceScalarResult.accepted[0u]==0.125f&&
		scalarFCTResult.accepted[scalarCells]-
		noSourceScalarResult.accepted[scalarCells]==0.125f,
		"scalar FCT flux stages exclude source and the final commit applies its dose exactly once");
	// Live-oracle bootstrap: a constant dyadic tuple makes the physical record's
	// binary64 nullspace arithmetic collapse exactly to zero slope.  Production
	// publication face x+1 corresponds to the periodic oracle's upper face x.
	RISE::FireProductionScalarFCTRequest constantScalarFCT=scalarFCT;
	constantScalarFCT.sourceDelta.assign(9u*scalarCells,0.0f);
	for(std::size_t cell=0u;cell<scalarCells;++cell){
		constantScalarFCT.beginning[cell]=1.0f;
		constantScalarFCT.beginning[scalarCells+cell]=1.0f;
	}
	for(std::size_t component=2u;component<9u;++component)
		std::fill(constantScalarFCT.beginning.begin()+component*scalarCells,
			constantScalarFCT.beginning.begin()+(component+1u)*scalarCells,0.0f);
	RISE::FireProductionScalarFCTResult constantScalarResult;
	const bool constantScalarOK=RISE::EvaluateFireProductionScalarFCTCPU(
		constantScalarFCT,constantScalarResult,&error);
	RISE::FireProductionScalarFCTFluxPair firstStagePair,secondStagePair,averagedStagePair;
	const bool firstStagePairOK=RISE::BuildFireProductionScalarFCTFluxPairCPU(
		constantScalarFCT,firstStagePair,&error);
	RISE::FireProductionScalarFCTRequest nonfiniteStage=constantScalarFCT;
	nonfiniteStage.frozenVelocityMPerS[0][1u]=
		std::numeric_limits<float>::quiet_NaN();
	RISE::FireProductionScalarFCTFluxPair rejectedNonfinitePair=firstStagePair;
	const bool nonfinitePairRejected=!RISE::BuildFireProductionScalarFCTFluxPairCPU(
		nonfiniteStage,rejectedNonfinitePair,&error)&&rejectedNonfinitePair.lowFlux.empty();
	secondStagePair=firstStagePair;
	const std::size_t nonlinearFace=productionFaceIndex(
		constantScalarFCT.shape,0u,2u,0u,0u);
	const std::size_t nonlinearPacked=firstStagePair.packedFaceOffset[0]+nonlinearFace;
	const std::size_t nonlinearAllFaces=firstStagePair.lowFlux.size()/9u;
	secondStagePair.fluxDelta[nonlinearAllFaces+nonlinearPacked]=8.0f;
	const bool averagedStagePairOK=RISE::AverageFireProductionScalarFCTFluxPairsCPU(
		firstStagePair,secondStagePair,averagedStagePair,&error);
	RISE::FireProductionScalarFCTResult firstStageAccepted,secondStageAccepted,
		averagedStageAccepted;
	const bool firstStageAcceptedOK=firstStagePairOK&&
		RISE::SolveFireProductionScalarFCTFluxPairCPU(
			constantScalarFCT,firstStagePair,firstStageAccepted,&error);
	const bool secondStageAcceptedOK=firstStagePairOK&&
		RISE::SolveFireProductionScalarFCTFluxPairCPU(
			constantScalarFCT,secondStagePair,secondStageAccepted,&error);
	const bool averagedStageAcceptedOK=averagedStagePairOK&&
		RISE::SolveFireProductionScalarFCTFluxPairCPU(
			constantScalarFCT,averagedStagePair,averagedStageAccepted,&error);
	RISE::FireProductionScalarFCTRequest mismatchedStageTopology=constantScalarFCT;
	mismatchedStageTopology.boundary.fill(RISE::FireProductionProjectionWall);
	RISE::FireProductionScalarFCTResult rejectedMismatchedStage;
	const bool mismatchedTopologyRejected=!RISE::SolveFireProductionScalarFCTFluxPairCPU(
		mismatchedStageTopology,firstStagePair,rejectedMismatchedStage,&error)&&
		rejectedMismatchedStage.accepted.empty();
	RISE::FireProductionScalarFCTFluxPair mismatchedAveragePair=secondStagePair;
	mismatchedAveragePair.boundary.fill(RISE::FireProductionProjectionWall);
	RISE::FireProductionScalarFCTFluxPair rejectedAveragePair;
	const bool mismatchedAverageRejected=
		!RISE::AverageFireProductionScalarFCTFluxPairsCPU(
			firstStagePair,mismatchedAveragePair,rejectedAveragePair,&error)&&
		rejectedAveragePair.lowFlux.empty();
	RISE::FireProductionScalarFCTFluxPair brokenPeriodicPair=firstStagePair;
	const std::size_t periodicHighFace=firstStagePair.packedFaceOffset[0]+
		productionFaceIndex(constantScalarFCT.shape,0u,constantScalarFCT.shape.nx,0u,0u);
	brokenPeriodicPair.lowFlux[periodicHighFace]+=1.0f;
	RISE::FireProductionScalarFCTResult rejectedPeriodicPair;
	const bool periodicSeamRejected=!RISE::SolveFireProductionScalarFCTFluxPairCPU(
		constantScalarFCT,brokenPeriodicPair,rejectedPeriodicPair,&error)&&
		rejectedPeriodicPair.accepted.empty();
	const float independentlyAveragedAlpha=firstStageAcceptedOK&&secondStageAcceptedOK?
		0.5f*(firstStageAccepted.sharedFaceAlpha[0][nonlinearFace]+
		secondStageAccepted.sharedFaceAlpha[0][nonlinearFace]):0.0f;
	Check(firstStagePairOK&&nonfinitePairRejected&&averagedStagePairOK&&firstStageAcceptedOK&&
		secondStageAcceptedOK&&averagedStageAcceptedOK&&
		mismatchedTopologyRejected&&mismatchedAverageRejected&&periodicSeamRejected&&
		averagedStagePair.fluxDelta[nonlinearAllFaces+nonlinearPacked]==4.0f&&
		averagedStageAccepted.sharedFaceAlpha[0][nonlinearFace]!=
			independentlyAveragedAlpha,
		"projected-Heun scalar staging averages flux pairs and solves a fresh shared alpha, not an averaged alpha");

	// r185 granular composition: build each 30F stage from the same raw Q/u/
	// boundary operands, retain f_N/J_g/Phi_g separately, average every field,
	// and consume the retained advective delta without reconstructing high-low.
	RISE::FireProductionProjectionShape heunPayloadUnder,heunPayloadOver;
	heunPayloadUnder.nx=38u;heunPayloadUnder.ny=191u;heunPayloadUnder.nz=813u;
	heunPayloadUnder.cellWidthM=1.0f;
	heunPayloadOver.nx=18u;heunPayloadOver.ny=500u;heunPayloadOver.nz=650u;
	heunPayloadOver.cellWidthM=1.0f;
	std::uint64_t heunPayloadUnderBytes=0u,heunPayloadOverBytes=0u;
	const bool heunPayloadQuery=
		RISE::QueryFireProductionScalarHeunFluxStageCPUPayloadBytes(
			heunPayloadUnder,heunPayloadUnderBytes,&error)&&
		RISE::QueryFireProductionScalarHeunFluxStageCPUPayloadBytes(
			heunPayloadOver,heunPayloadOverBytes,&error);
	Check(heunPayloadQuery&&heunPayloadUnderBytes==UINT64_C(2147483640)&&
		heunPayloadOverBytes==UINT64_C(2147484000)&&
		heunPayloadUnderBytes<=(UINT64_C(2)<<30u)&&
		heunPayloadOverBytes>(UINT64_C(2)<<30u),
		"r185 Heun stage payload query exact-binds the 30F two-GiB neighbors");
	RISE::FireProductionScalarFCTRequest overComposeFCT;
	overComposeFCT.shape.nx=36u;overComposeFCT.shape.ny=115u;
	overComposeFCT.shape.nz=736u;overComposeFCT.shape.cellWidthM=1.0f;
	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest overComposePhysical;
	overComposePhysical.shape=overComposeFCT.shape;
	RISE::FireProductionScalarHeunFluxStage refusedHeunStage;
	refusedHeunStage.physicalEnergyFluxWPerM2.assign(1u,1.0f);
	const bool composeOverCapRejected=
		!RISE::ComposeFireProductionScalarHeunFluxStageCPU(UINT64_C(18501),
			RISE::FireProductionScalarHeunFluxRole::R0,overComposeFCT,
			overComposePhysical,refusedHeunStage,&error)&&
		refusedHeunStage.physicalEnergyFluxWPerM2.empty()&&
		error.find("working set exceeds two GiB")!=std::string::npos;
	RISE::FireProductionScalarHeunFluxStage overAverageFirst,overAverageSecond,
		refusedAverageStage;
	overAverageFirst.compositeFluxPair.shape.nx=25u;
	overAverageFirst.compositeFluxPair.shape.ny=140u;
	overAverageFirst.compositeFluxPair.shape.nz=559u;
	overAverageFirst.compositeFluxPair.shape.cellWidthM=1.0f;
	overAverageSecond.compositeFluxPair.shape=overAverageFirst.compositeFluxPair.shape;
	const bool averageOverCapRejected=
		!RISE::AverageFireProductionScalarHeunFluxStagesCPU(overAverageFirst,
			overAverageSecond,refusedAverageStage,&error)&&
		refusedAverageStage.compositeFluxPair.lowFlux.empty()&&
		error.find("average working set exceeds two GiB")!=std::string::npos;
	RISE::FireProductionScalarHeunFluxStage mismatchedAverageFirst,
		mismatchedAverageSecond;
	mismatchedAverageFirst.compositeFluxPair.shape.nx=4u;
	mismatchedAverageFirst.compositeFluxPair.shape.ny=4u;
	mismatchedAverageFirst.compositeFluxPair.shape.nz=4u;
	mismatchedAverageFirst.compositeFluxPair.shape.cellWidthM=1.0f;
	mismatchedAverageSecond.compositeFluxPair.shape=heunPayloadOver;
	const bool mismatchedLargeAverageRejected=
		!RISE::AverageFireProductionScalarHeunFluxStagesCPU(mismatchedAverageFirst,
			mismatchedAverageSecond,refusedAverageStage,&error)&&
		error.find("average working set exceeds two GiB")!=std::string::npos;
	Check(composeOverCapRejected&&averageOverCapRejected&&
		mismatchedLargeAverageRejected,
		"r185 composition and three-stage averaging reject their exact live-set over-cap shapes before payload access");

	RISE::FireProductionScalarFCTRequest heunR0=constantScalarFCT;
	heunR0.beginning=physicalFluxRequest.conservativeValues;
	heunR0.sourceDelta.assign(9u*scalarCells,0.0f);
	heunR0.ambient=physicalFluxRequest.ambient;
	for(std::size_t species=0u;species<7u;++species){
		heunR0.enthalpyBoundsJPerKG[species]=-1.0e8f;
		heunR0.enthalpyBoundsJPerKG[7u+species]=1.0e8f;
	}
	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest heunPhysicalR0=
		physicalFluxRequest;
	const float heunMethanePattern[4]={0.1f,0.125f,0.15f,0.125f};
	for(std::size_t z=0u;z<4u;++z)for(std::size_t y=0u;y<4u;++y)
		for(std::size_t x=0u;x<4u;++x){const std::size_t cell=(z*4u+y)*4u+x;
			const float methane=heunMethanePattern[x];
			heunPhysicalR0.conservativeValues[cell]=methane;
			heunPhysicalR0.conservativeValues[scalarCells+cell]=methane;
			heunPhysicalR0.conservativeValues[3u*scalarCells+cell]=0.8f-methane;
		}
	heunR0.beginning=heunPhysicalR0.conservativeValues;
	for(unsigned int axis=0u;axis<3u;++axis){
		const float velocity=axis==0u?0.1f:0.0f;
		std::fill(heunR0.frozenVelocityMPerS[axis].begin(),
			heunR0.frozenVelocityMPerS[axis].end(),velocity);
		heunPhysicalR0.frozenVelocityMPerS[axis]=heunR0.frozenVelocityMPerS[axis];
	}
	RISE::FireProductionScalarFCTRequest heunR1=heunR0;
	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest heunPhysicalR1=
		heunPhysicalR0;
	for(std::size_t cell=0u;cell<scalarCells;++cell)
		heunPhysicalR1.temperatureK[cell]+=5.0f;
	for(unsigned int axis=0u;axis<3u;++axis){
		const float velocity=axis==0u?0.075f:0.0f;
		std::fill(heunR1.frozenVelocityMPerS[axis].begin(),
			heunR1.frozenVelocityMPerS[axis].end(),velocity);
		heunPhysicalR1.frozenVelocityMPerS[axis]=heunR1.frozenVelocityMPerS[axis];
	}
	RISE::FireProductionScalarHeunFluxStage heunStageR0,heunStageR1,heunStageAverage;
	RISE::FireProductionScalarFCTFluxPair heunAdvectiveR0;
	const bool heunAdvectiveR0OK=RISE::BuildFireProductionScalarFCTFluxPairCPU(
		heunR0,heunAdvectiveR0,&error);
	const bool heunR0OK=RISE::ComposeFireProductionScalarHeunFluxStageCPU(
		UINT64_C(18501),RISE::FireProductionScalarHeunFluxRole::R0,heunR0,
		heunPhysicalR0,heunStageR0,&error);
	const bool heunR1OK=RISE::ComposeFireProductionScalarHeunFluxStageCPU(
		UINT64_C(18501),RISE::FireProductionScalarHeunFluxRole::R1,heunR1,
		heunPhysicalR1,heunStageR1,&error);
	const bool heunAverageOK=heunR0OK&&heunR1OK&&
		RISE::AverageFireProductionScalarHeunFluxStagesCPU(
			heunStageR0,heunStageR1,heunStageAverage,&error);
	RISE::FireProductionScalarHeunSolveResult heunSolve;
	const bool heunSolveOK=heunAverageOK&&
		RISE::SolveFireProductionScalarHeunFluxStageCPU(UINT64_C(18501),heunR0,
			heunStageAverage,heunSolve,&error);
	const RISE::FireProductionScalarFCTResult& heunAccepted=heunSolve.scalar;
	RISE::FireProductionCompatibleFCTMomentumResult heunMomentumR0,heunMomentumR1;
	const bool heunMomentumR0OK=heunSolveOK&&
		RISE::EvaluateFireProductionCompatibleHeunMomentumCPU(heunStageR0,
			heunSolve,heunR0.frozenVelocityMPerS,heunMomentumR0,&error);
	const bool heunMomentumR1OK=heunSolveOK&&
		RISE::EvaluateFireProductionCompatibleHeunMomentumCPU(heunStageR1,
			heunSolve,heunR1.frozenVelocityMPerS,heunMomentumR1,&error);
	bool heunCompositionExact=heunAdvectiveR0OK&&heunR0OK&&heunR1OK&&heunAverageOK;
	for(std::size_t component=0u;component<8u&&heunCompositionExact;++component)
		for(std::size_t face=0u;face<physicalAllFaces&&heunCompositionExact;++face){
			const std::size_t index=component*physicalAllFaces+face;
			heunCompositionExact=sameFloatBits(
				heunStageR0.compositeFluxPair.lowFlux[index],
				heunAdvectiveR0.lowFlux[index]+heunStageR0.physicalMassFluxKGPerM2S[index])&&
				sameFloatBits(heunStageR0.compositeFluxPair.fluxDelta[index],
					heunAdvectiveR0.fluxDelta[index]);
		}
	const std::size_t heunFace=productionFaceIndex(heunR0.shape,0u,2u,0u,0u);
	const float directExpected=heunStageR0.advectiveGasLowFluxKGPerM2S[0][heunFace]+
		heunAccepted.sharedFaceAlpha[0][heunFace]*
		heunStageR0.advectiveGasFluxDeltaKGPerM2S[0][heunFace]+
		heunStageR0.physicalGasFluxKGPerM2S[0][heunFace];
	const bool directDeltaExact=heunMomentumR0OK&&sameFloatBits(directExpected,
		heunMomentumR0.acceptedGasFluxKGPerM2S[0][heunFace]);
	RISE::FireProductionCompatibleFCTMomentumRequest reconstructedHeun;
	reconstructedHeun.shape=heunR0.shape;reconstructedHeun.boundary=heunR0.boundary;
	reconstructedHeun.lowGasFluxKGPerM2S=heunStageR0.advectiveGasLowFluxKGPerM2S;
	reconstructedHeun.physicalGasFluxKGPerM2S=heunStageR0.physicalGasFluxKGPerM2S;
	reconstructedHeun.sharedFaceAlpha=heunAccepted.sharedFaceAlpha;
	reconstructedHeun.frozenVelocityMPerS=heunR0.frozenVelocityMPerS;
	for(unsigned int axis=0u;axis<3u;++axis){
		reconstructedHeun.highGasFluxKGPerM2S[axis].resize(
			heunStageR0.advectiveGasLowFluxKGPerM2S[axis].size());
		for(std::size_t face=0u;face<reconstructedHeun.highGasFluxKGPerM2S[axis].size();
			++face)reconstructedHeun.highGasFluxKGPerM2S[axis][face]=
				heunStageR0.advectiveGasLowFluxKGPerM2S[axis][face]+
				heunStageR0.advectiveGasFluxDeltaKGPerM2S[axis][face];
	}
	RISE::FireProductionCompatibleFCTMomentumResult reconstructedMomentum;
	const bool reconstructedMomentumOK=RISE::EvaluateFireProductionCompatibleFCTMomentumCPU(
		reconstructedHeun,reconstructedMomentum,&error);
	bool cancellationWitness=false;
	for(unsigned int axis=0u;axis<3u;++axis)
		for(std::size_t face=0u;face<reconstructedHeun.highGasFluxKGPerM2S[axis].size();
			++face)cancellationWitness=cancellationWitness||!sameFloatBits(
				heunStageR0.advectiveGasFluxDeltaKGPerM2S[axis][face],
				reconstructedHeun.highGasFluxKGPerM2S[axis][face]-
					reconstructedHeun.lowGasFluxKGPerM2S[axis][face]);
	Check(heunR0OK&&heunR1OK&&heunAverageOK,
		"r185 authenticates and composes distinct R0/R1 flux stages");
	Check(heunCompositionExact,
		"r185 composite low adds f_N once while the antidiffusive delta remains advective");
	Check(heunSolveOK,
		"r185 averaged composite accepts one fresh shared-alpha solve");
	Check(heunMomentumR0OK&&heunMomentumR1OK,
		"r185 direct-delta compatible momentum accepts both original stage velocities");
	Check(heunCompositionExact&&heunSolveOK&&heunMomentumR0OK&&heunMomentumR1OK&&
		heunStageAverage.role==RISE::FireProductionScalarHeunFluxRole::HeunAverage&&
		heunStageAverage.attemptIdentity==UINT64_C(18501),
		"r185 retains and averages all 30F fields and solves one fresh alpha");
	Check(directDeltaExact,
		"r185 compatible momentum consumes retained DeltaPhi_g in its exact published operation order");
	Check(reconstructedMomentumOK&&cancellationWitness,
		"r185 cancellation fixture distinguishes direct DeltaPhi_g from reconstructed high-low");

	auto rejectsMutatedHeun=[&](RISE::FireProductionScalarHeunFluxStage candidate){
		RISE::FireProductionCompatibleFCTMomentumResult rejected;
		rejected.acceptedGasFluxKGPerM2S[0].assign(1u,1.0f);
		return !RISE::EvaluateFireProductionCompatibleHeunMomentumCPU(candidate,
			heunSolve,heunR0.frozenVelocityMPerS,rejected,&error)&&
			rejected.acceptedGasFluxKGPerM2S[0].empty();
	};
	RISE::FireProductionScalarHeunFluxStage mutatedHeun=heunStageR0;
	mutatedHeun.physicalMassFluxKGPerM2S[0]=std::nextafter(
		mutatedHeun.physicalMassFluxKGPerM2S[0],std::numeric_limits<float>::infinity());
	const bool mutatedMassRejected=rejectsMutatedHeun(mutatedHeun);
	mutatedHeun=heunStageR0;mutatedHeun.physicalEnergyFluxWPerM2[0]=std::nextafter(
		mutatedHeun.physicalEnergyFluxWPerM2[0],std::numeric_limits<float>::infinity());
	const bool mutatedEnergyRejected=rejectsMutatedHeun(mutatedHeun);
	mutatedHeun=heunStageR0;mutatedHeun.physicalGasFluxKGPerM2S[0][heunFace]=
		std::nextafter(mutatedHeun.physicalGasFluxKGPerM2S[0][heunFace],
			std::numeric_limits<float>::infinity());
	const bool mutatedJgRejected=rejectsMutatedHeun(mutatedHeun);
	mutatedHeun=heunStageR0;mutatedHeun.advectiveGasLowFluxKGPerM2S[0][heunFace]=
		std::nextafter(mutatedHeun.advectiveGasLowFluxKGPerM2S[0][heunFace],
			std::numeric_limits<float>::infinity());
	const bool mutatedPhiLowRejected=rejectsMutatedHeun(mutatedHeun);
	mutatedHeun=heunStageR0;mutatedHeun.advectiveGasFluxDeltaKGPerM2S[0][heunFace]=
		std::nextafter(mutatedHeun.advectiveGasFluxDeltaKGPerM2S[0][heunFace],
			std::numeric_limits<float>::infinity());
	const bool mutatedPhiDeltaRejected=rejectsMutatedHeun(mutatedHeun);
	mutatedHeun=heunStageR0;mutatedHeun.compositeFluxPair.lowFlux[0]=std::nextafter(
		mutatedHeun.compositeFluxPair.lowFlux[0],std::numeric_limits<float>::infinity());
	const bool mutatedCompositeLowRejected=rejectsMutatedHeun(mutatedHeun);
	mutatedHeun=heunStageR0;mutatedHeun.compositeFluxPair.fluxDelta[0]=std::nextafter(
		mutatedHeun.compositeFluxPair.fluxDelta[0],std::numeric_limits<float>::infinity());
	const bool mutatedCompositeDeltaRejected=rejectsMutatedHeun(mutatedHeun);
	std::array<std::vector<float>,3> staleHeunVelocity=heunR0.frozenVelocityMPerS;
	staleHeunVelocity[0][heunFace]=std::nextafter(staleHeunVelocity[0][heunFace],
		std::numeric_limits<float>::infinity());
	RISE::FireProductionCompatibleFCTMomentumResult staleVelocityResult;
	const bool staleVelocityRejected=!RISE::EvaluateFireProductionCompatibleHeunMomentumCPU(
		heunStageR0,heunSolve,staleHeunVelocity,
		staleVelocityResult,&error);
	RISE::FireProductionScalarHeunSolveResult staleAlphaSolve=heunSolve;
	staleAlphaSolve.scalar.sharedFaceAlpha[0][heunFace]=std::nextafter(
		staleAlphaSolve.scalar.sharedFaceAlpha[0][heunFace],0.0f);
	RISE::FireProductionCompatibleFCTMomentumResult staleAlphaResult;
	const bool staleAlphaRejected=!RISE::EvaluateFireProductionCompatibleHeunMomentumCPU(
		heunStageR0,staleAlphaSolve,heunR0.frozenVelocityMPerS,
		staleAlphaResult,&error)&&staleAlphaResult.acceptedGasFluxKGPerM2S[0].empty();
	RISE::FireProductionScalarHeunFluxStage mutatedAverageStage=heunStageAverage;
	mutatedAverageStage.compositeFluxPair.lowFlux[0]=std::nextafter(
		mutatedAverageStage.compositeFluxPair.lowFlux[0],
		std::numeric_limits<float>::infinity());
	RISE::FireProductionScalarHeunSolveResult refusedMutatedAverage;
	const bool mutatedAverageRejected=
		!RISE::SolveFireProductionScalarHeunFluxStageCPU(UINT64_C(18501),heunR0,
			mutatedAverageStage,refusedMutatedAverage,&error)&&
		refusedMutatedAverage.scalar.accepted.empty();
	RISE::FireProductionScalarFCTRequest incompatibleHeunR1=heunR1;
	incompatibleHeunR1.sourceDelta[0]=0.25f;
	RISE::FireProductionScalarHeunFluxStage incompatibleStageR1,
		refusedIncompatibleAverage;
	const bool incompatibleStageBuilt=
		RISE::ComposeFireProductionScalarHeunFluxStageCPU(UINT64_C(18501),
			RISE::FireProductionScalarHeunFluxRole::R1,incompatibleHeunR1,
			heunPhysicalR1,incompatibleStageR1,&error);
	const bool sharedContractRejected=incompatibleStageBuilt&&
		!RISE::AverageFireProductionScalarHeunFluxStagesCPU(heunStageR0,
			incompatibleStageR1,refusedIncompatibleAverage,&error)&&
		refusedIncompatibleAverage.compositeFluxPair.lowFlux.empty();
	RISE::FireProductionScalarHeunFluxStage swappedAverage;
	const bool swappedRolesRejected=!RISE::AverageFireProductionScalarHeunFluxStagesCPU(
		heunStageR1,heunStageR0,swappedAverage,&error);
	Check(mutatedMassRejected&&mutatedEnergyRejected&&mutatedJgRejected&&
		mutatedPhiLowRejected&&mutatedPhiDeltaRejected&&mutatedCompositeLowRejected&&
		mutatedCompositeDeltaRejected,
		"r185 all seven retained field classes are content-identity protected");
	Check(staleVelocityRejected&&staleAlphaRejected&&mutatedAverageRejected,
		"r185 stale velocity, stale alpha, and mutated averaged-stage consumers refuse");
	Check(sharedContractRejected&&swappedRolesRejected,
		"r185 shared FCT contract and R0/R1 ordering are identity-bearing");
	Check(incompatibleStageBuilt,
		"r185 can construct a valid R1 witness carrying a deliberately different shared source contract");
	Check(sharedContractRejected,
		"r185 refuses an R0/R1 shared-contract mismatch before averaging payloads");
	Check(swappedRolesRejected,
		"r185 refuses swapped R1/R0 stage roles");
	Check(mutatedMassRejected&&mutatedEnergyRejected&&mutatedJgRejected&&
		mutatedPhiLowRejected&&mutatedPhiDeltaRejected&&mutatedCompositeLowRejected&&
		mutatedCompositeDeltaRejected&&staleVelocityRejected&&staleAlphaRejected&&
		mutatedAverageRejected&&sharedContractRejected&&swappedRolesRejected,
		"r185 content, shared-contract, fresh-alpha, stage-role, and frozen-velocity identities reject every transplant atomically");
	RISE::FireProductionScalarFCTRequest openScalarFCT=constantScalarFCT;
	openScalarFCT.boundary={{RISE::FireProductionProjectionWall,
		RISE::FireProductionProjectionPressureOpen,RISE::FireProductionProjectionWall,
		RISE::FireProductionProjectionWall,RISE::FireProductionProjectionWall,
		RISE::FireProductionProjectionWall}};
	openScalarFCT.pressureOpenInflow[1].assign(16u,1u);
	openScalarFCT.ambient[0]=2.0f;openScalarFCT.ambient[1]=2.0f;
	for(std::size_t z=0u;z<4u;++z)for(std::size_t y=0u;y<4u;++y)
		openScalarFCT.frozenVelocityMPerS[0][productionFaceIndex(
			openScalarFCT.shape,0u,4u,y,z)]=-1.0f;
	RISE::FireProductionScalarFCTResult openScalarResult;
	const bool openScalarOK=RISE::EvaluateFireProductionScalarFCTCPU(
		openScalarFCT,openScalarResult,&error);
	const std::size_t openScalarWall=productionFaceIndex(openScalarFCT.shape,0u,0u,0u,0u);
	const std::size_t openScalarInflow=productionFaceIndex(openScalarFCT.shape,0u,4u,0u,0u);
	const std::size_t openScalarAllFaces=openScalarOK?openScalarResult.lowFlux.size()/9u:0u;
	Check(openScalarOK&&openScalarResult.lowFlux[openScalarWall]==0.0f&&
		openScalarResult.fluxDelta[openScalarWall]==0.0f&&
		openScalarResult.lowFlux[openScalarInflow]==-2.0f&&
		openScalarResult.lowFlux[openScalarAllFaces+openScalarInflow]==-2.0f&&
		openScalarResult.fluxDelta[openScalarInflow]==0.0f,
		"scalar FCT donor pair applies wall zero-flux and pressure-open inflow ghosts exactly");
	std::vector<RISE::FireSim::ConservativeVector> constantOracleState(scalarCells);
	for(RISE::FireSim::ConservativeVector& state:constantOracleState){state[0]=1.0;state[1]=1.0;}
	RISE::FireSim::PeriodicMACField constantOracleVelocity;
	for(unsigned int axis=0u;axis<3u;++axis)constantOracleVelocity.component[axis].assign(
		scalarCells,axis==0u?1.0:0.0);
	std::vector<double> constantTemperature(scalarCells,300.0),constantZero(scalarCells,0.0);
	RISE::FireSim::PeriodicFluxPair3D constantOraclePair;
	const RISE::FireSimulationMethaneRecord& physicalRecord=
		RISE::FireSimulationMethaneRecord::PhysicalV1();
	const bool constantOracleOK=RISE::FireSim::BuildPeriodicFluxPair3D(
		periodicOracleShape,constantOracleState,constantTemperature,constantOracleVelocity,
		constantZero,constantZero,physicalRecord,physicalRecord,constantOraclePair,&error);
	bool constantPairExact=constantScalarOK&&constantOracleOK;
	for(unsigned int axis=0u;axis<3u&&constantPairExact;++axis)
		for(std::size_t z=0u;z<4u&&constantPairExact;++z)
			for(std::size_t y=0u;y<4u&&constantPairExact;++y)
				for(std::size_t x=0u;x<4u&&constantPairExact;++x){
					const std::size_t oracleFace=(z*4u+y)*4u+x;
					std::size_t px=x,py=y,pz=z;
					if(axis==0u)++px;if(axis==1u)++py;if(axis==2u)++pz;
					const std::size_t productionFace=productionFaceIndex(
						constantScalarFCT.shape,axis,px,py,pz);
					const std::size_t packed=constantScalarResult.packedFaceOffset[axis]+
						productionFace;
					for(std::size_t component=0u;component<9u&&constantPairExact;++component){
						const double low=constantOraclePair.low[axis][oracleFace][component];
						const double delta=constantOraclePair.high[axis][oracleFace][component]-low;
						constantPairExact=sameDoubleBits(static_cast<double>(constantScalarResult.
							lowFlux[component*constantScalarResult.lowFlux.size()/9u+packed]),low)&&
							sameDoubleBits(static_cast<double>(constantScalarResult.fluxDelta[
							component*constantScalarResult.fluxDelta.size()/9u+packed]),delta);
					}
				}
	Check(constantPairExact,
		"strict fp32 donor/MC pair is word-exact to the live periodic oracle on the dyadic collapse");

	// The nonconstant live bootstrap uses the oracle's actual invariant-MC
	// routine with the exact one-row orthonormal basis e_CH4.  Thus the
	// binary64 operations collapse exactly to binary32 on a nonzero slope,
	// while the zero assembly budget makes the independently-known shared
	// limiter answer alpha=0 on precisely the two antidiffusive faces.
	RISE::FireProductionScalarFCTRequest liveFrontScalarFCT=scalarFCT;
	liveFrontScalarFCT.sourceDelta.assign(9u*scalarCells,0.0f);
	liveFrontScalarFCT.nullspaceBasis.assign(8u,0.0f);
	liveFrontScalarFCT.nullspaceBasis[1]=1.0f;
	liveFrontScalarFCT.coordinateProjector.assign(1u,1.0f);
	liveFrontScalarFCT.assemblyReserveFactor=liveFrontScalarFCT.feasibilityFactor;
	RISE::FireProductionScalarFCTResult liveFrontScalarResult;
	const bool liveFrontScalarOK=RISE::EvaluateFireProductionScalarFCTCPU(
		liveFrontScalarFCT,liveFrontScalarResult,&error);
	RISE::FireCertifiedNullspace liveFrontClosure;
	liveFrontClosure.stateDimension=8u;liveFrontClosure.nullity=1u;
	liveFrontClosure.orthonormalBasis.assign(8u,0.0);
	liveFrontClosure.orthonormalBasis[1]=1.0;
	std::vector<RISE::FireSim::ConservativeVector> liveFrontLine(4u);
	for(std::size_t x=0u;x<4u;++x){
		liveFrontLine[x][0]=static_cast<double>(scalarPattern[x]);
		liveFrontLine[x][1]=static_cast<double>(scalarPattern[x]);
	}
	std::vector<std::array<double,8> > liveFrontSlope;
	const bool liveFrontOracleOK=RISE::FireSim::InvariantMCMassSlopes(
		liveFrontLine,liveFrontClosure,liveFrontSlope,&error);
	bool liveFrontPairExact=liveFrontScalarOK&&liveFrontOracleOK;
	for(std::size_t z=0u;z<4u&&liveFrontPairExact;++z)
		for(std::size_t y=0u;y<4u&&liveFrontPairExact;++y)
			for(std::size_t x=0u;x<4u&&liveFrontPairExact;++x){
				const std::size_t face=productionFaceIndex(
					liveFrontScalarFCT.shape,0u,x+1u,y,z);
				const std::size_t packed=liveFrontScalarResult.packedFaceOffset[0]+face;
				for(std::size_t component=0u;component<8u&&liveFrontPairExact;++component){
					const double oracleLow=liveFrontLine[x][component];
					const double oracleDelta=0.5*liveFrontSlope[x][component];
					liveFrontPairExact=sameDoubleBits(static_cast<double>(liveFrontScalarResult.
						lowFlux[component*liveFrontScalarResult.lowFlux.size()/9u+packed]),
						oracleLow)&&sameDoubleBits(static_cast<double>(liveFrontScalarResult.
						fluxDelta[component*liveFrontScalarResult.fluxDelta.size()/9u+packed]),
						oracleDelta);
				}
				const bool antidiffusive=x==1u||x==3u;
				liveFrontPairExact=liveFrontPairExact&&liveFrontScalarResult.
					sharedFaceAlpha[0][face]==(antidiffusive?0.0f:1.0f)&&
					liveFrontScalarResult.acceptedGasFluxKGPerM2S[0][face]==
					liveFrontScalarResult.lowFlux[liveFrontScalarResult.lowFlux.size()/9u+packed];
			}
	for(std::size_t value=0u;value<liveFrontScalarResult.accepted.size()&&
		liveFrontPairExact;++value)liveFrontPairExact=liveFrontScalarResult.accepted[value]==
		liveFrontScalarResult.lowState[value];
	Check(liveFrontPairExact,
		"nonconstant dyadic MC slopes match the live oracle and bind shared-alpha acceptance");

	// The production PPM dose and an evolving carrier are rejected operator
	// mutations: this fixture must distinguish both from the frozen MC/FCT form.
	RISE::FireProductionRemapRequest ppmMutation;
	ppmMutation.lineLength=4u;ppmMutation.lineCount=1u;ppmMutation.componentCount=1u;
	ppmMutation.cellWidthM=1.0f;ppmMutation.timeStepS=scalarFCT.timeStepS;
	ppmMutation.boundary=RISE::FireProductionRemapPeriodic;
	ppmMutation.values.assign(scalarPattern,scalarPattern+4u);
	ppmMutation.faceVelocityMPerS.assign(5u,1.0f);
	ppmMutation.ambientValues.assign(1u,scalarPattern[0]);
	RISE::FireProductionRemapResult ppmMutationResult;
	const bool ppmMutationOK=RISE::RemapFireProductionCPU(
		ppmMutation,ppmMutationResult,&error);
	bool ppmDistinguished=false;
	for(std::size_t face=0u;face<4u&&ppmMutationOK;++face){
		const std::size_t productionFace=productionFaceIndex(scalarFCT.shape,0u,
			face+1u,0u,0u);
		const std::size_t packed=scalarFCTResult.packedFaceOffset[0]+productionFace;
		const float fctDose=scalarFCT.timeStepS*(scalarFCTResult.lowFlux[packed]+
			scalarFCTResult.sharedFaceAlpha[0][productionFace]*
			scalarFCTResult.fluxDelta[packed]);
		ppmDistinguished=ppmDistinguished||ppmMutationResult.faceFluxes[face]!=fctDose;
	}
	RISE::FireProductionCompatibleFCTMomentumRequest frozenCarrierFCT;
	frozenCarrierFCT.shape=scalarFCT.shape;frozenCarrierFCT.boundary=scalarFCT.boundary;
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t faces=RISE::FireProductionProjectionFaceCount(scalarFCT.shape,axis);
		frozenCarrierFCT.lowGasFluxKGPerM2S[axis].assign(faces,0.0f);
		frozenCarrierFCT.highGasFluxKGPerM2S[axis].assign(faces,0.0f);
		frozenCarrierFCT.sharedFaceAlpha[axis]=scalarFCTResult.sharedFaceAlpha[axis];
		frozenCarrierFCT.frozenVelocityMPerS[axis]=scalarFCT.frozenVelocityMPerS[axis];
		for(std::size_t face=0u;face<faces;++face){
			const std::size_t packed=scalarFCTResult.packedFaceOffset[axis]+face;
			for(std::size_t component=1u;component<=6u;++component){
				frozenCarrierFCT.lowGasFluxKGPerM2S[axis][face]+=
					scalarFCTResult.lowFlux[component*scalarFCTResult.lowFlux.size()/9u+packed];
				frozenCarrierFCT.highGasFluxKGPerM2S[axis][face]+=
					scalarFCTResult.lowFlux[component*scalarFCTResult.lowFlux.size()/9u+packed]+
					scalarFCTResult.fluxDelta[component*scalarFCTResult.fluxDelta.size()/9u+packed];
			}
		}
	}
	RISE::FireProductionCompatibleFCTMomentumResult frozenCarrierRate,evolvingCarrierRate;
	const bool frozenCarrierOK=RISE::EvaluateFireProductionCompatibleFCTMomentumCPU(
		frozenCarrierFCT,frozenCarrierRate,&error);
	RISE::FireProductionCompatibleFCTMomentumRequest evolvingCarrierFCT=frozenCarrierFCT;
	evolvingCarrierFCT.frozenVelocityMPerS[0][scalarFace2]=2.0f;
	const bool evolvingCarrierOK=RISE::EvaluateFireProductionCompatibleFCTMomentumCPU(
		evolvingCarrierFCT,evolvingCarrierRate,&error);
	bool evolvingCarrierDistinguished=false;
	for(unsigned int axis=0u;axis<3u;++axis)
		for(std::size_t face=0u;face<frozenCarrierRate.advectionRateKGPerM2S2[axis].size();++face)
			evolvingCarrierDistinguished=evolvingCarrierDistinguished||
				frozenCarrierRate.advectionRateKGPerM2S2[axis][face]!=
				evolvingCarrierRate.advectionRateKGPerM2S2[axis][face];
	Check(ppmMutationOK&&ppmDistinguished&&frozenCarrierOK&&evolvingCarrierOK&&
		evolvingCarrierDistinguished,
		"scalar FCT bootstrap detects the rejected PPM-dose and evolving-carrier mutations");

	RISE::FireProductionScalarFCTRequest malformedScalarFCT=scalarFCT;
	malformedScalarFCT.nullspaceBasis.pop_back();
	RISE::FireProductionScalarFCTResult refusedScalarFCT;
	refusedScalarFCT.accepted.assign(1u,123.0f);
	const bool badBasisRefused=!RISE::EvaluateFireProductionScalarFCTCPU(
		malformedScalarFCT,refusedScalarFCT,&error)&&refusedScalarFCT.accepted.empty();
	malformedScalarFCT=scalarFCT;malformedScalarFCT.sourceDelta.pop_back();
	const bool badSourceRefused=!RISE::EvaluateFireProductionScalarFCTCPU(
		malformedScalarFCT,refusedScalarFCT,&error);
	malformedScalarFCT=scalarFCT;malformedScalarFCT.pressureOpenInflow[0][0]=2u;
	const bool badInflowRefused=!RISE::EvaluateFireProductionScalarFCTCPU(
		malformedScalarFCT,refusedScalarFCT,&error);
	malformedScalarFCT=scalarFCT;malformedScalarFCT.pressureOpenInflow[0][0]=1u;
	const bool inactiveInflowRefused=!RISE::EvaluateFireProductionScalarFCTCPU(
		malformedScalarFCT,refusedScalarFCT,&error);
	malformedScalarFCT=constantScalarFCT;
	malformedScalarFCT.sourceDelta[scalarCells]=-2.0f;
	const bool infeasibleLowRefused=!RISE::EvaluateFireProductionScalarFCTCPU(
		malformedScalarFCT,refusedScalarFCT,&error);
	malformedScalarFCT=scalarFCT;malformedScalarFCT.frozenVelocityMPerS[0].back()=-0.0f;
	const bool badVelocitySeamRefused=!RISE::EvaluateFireProductionScalarFCTCPU(
		malformedScalarFCT,refusedScalarFCT,&error);
	Check(badBasisRefused&&badSourceRefused&&badInflowRefused&&inactiveInflowRefused&&
		infeasibleLowRefused&&badVelocitySeamRefused,
		"scalar FCT stages fail closed on malformed certificate, low state, inflow, and seam identity");

	RISE::FireProductionDualMomentumRequest dual32;
	dual32.shape=cell32.shape;dual32.timeStepS=cell32.timeStepS;
	dual32.ambientDensityKGPerM3=1.0f;dual32.boundary=cell32.boundary;
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t faces=RISE::FireProductionProjectionFaceCount(dual32.shape,axis);
		dual32.beginningFaceDensity[axis].resize(faces);
		dual32.beginningMomentum[axis].resize(faces);
		dual32.frozenVelocityMPerS[axis].resize(faces);
		for(std::size_t face=0u;face<faces;++face){
			const float density=1.0f+0.001f*static_cast<float>(face%17u);
			const float velocity=0.01f*static_cast<float>(axis+1u);
			dual32.beginningFaceDensity[axis][face]=density;
			dual32.beginningMomentum[axis][face]=density*velocity;
			dual32.frozenVelocityMPerS[axis][face]=velocity;
		}
	}
	RISE::FireProductionDualMomentumResult dual32Result;
	const bool dual32OK=RISE::RemapFireProductionDualMomentumCPU(dual32,dual32Result,&error);
	if(!dual32OK)std::fprintf(stderr,"dual stage witness failed: %s\n",error.c_str());
	Check(dual32OK,
		"binary32 dual palindrome accepts the stage-reset witness");
	RISEFireProductionTrace::FireProductionDualMomentumRequest tracedDual;
	tracedDual.shape=tracedCell.shape;tracedDual.timeStepS=dual32.timeStepS;
	tracedDual.ambientDensityKGPerM3=dual32.ambientDensityKGPerM3;
	for(unsigned int side=0u;side<6u;++side)tracedDual.boundary[side]=
		static_cast<RISEFireProductionTrace::FireProductionProjectionBoundary>(
			dual32.boundary[side]);
	for(unsigned int axis=0u;axis<3u;++axis){
		tracedDual.beginningFaceDensity[axis].assign(dual32.beginningFaceDensity[axis].begin(),
			dual32.beginningFaceDensity[axis].end());
		tracedDual.beginningMomentum[axis].assign(dual32.beginningMomentum[axis].begin(),
			dual32.beginningMomentum[axis].end());
		tracedDual.frozenVelocityMPerS[axis].assign(dual32.frozenVelocityMPerS[axis].begin(),
			dual32.frozenVelocityMPerS[axis].end());
	}
	RISEFireProductionTrace::FireProductionDualMomentumResult tracedDualResult;
	FireProductionRoundoffTrace::Counters tracedDualCounters;bool tracedDualOK=false;{
		FireProductionRoundoffTrace::Scope scope(tracedDualCounters);
		tracedDualOK=RISEFireProductionTrace::RemapFireProductionDualMomentumCPU(
			tracedDual,tracedDualResult,&error);
	}
	bool tracedDualBytes=true;
	for(unsigned int axis=0u;axis<3u&&tracedDualBytes;++axis){
		tracedDualBytes=tracedDualResult.auxiliaryFaceDensity[axis].size()==
			dual32Result.auxiliaryFaceDensity[axis].size()&&
			tracedDualResult.momentum[axis].size()==dual32Result.momentum[axis].size();
		for(std::size_t face=0u;tracedDualBytes&&
			face<tracedDualResult.momentum[axis].size();++face)
			tracedDualBytes=tracedDualResult.auxiliaryFaceDensity[axis][face].Rounded()==
				dual32Result.auxiliaryFaceDensity[axis][face]&&
				tracedDualResult.momentum[axis][face].Rounded()==dual32Result.momentum[axis][face]&&
				tracedDualResult.auxiliaryFaceDensity[axis][face].Radius()==0.0&&
				tracedDualResult.momentum[axis][face].Radius()==0.0;
	}
	bool completeDualStages=tracedDualCounters.sealedStages.size()==15u;
	for(const FireProductionRoundoffTrace::Observation& stage:tracedDualCounters.sealedStages){
		std::uint64_t operations=0u;for(const std::uint64_t count:stage.operation)operations+=count;
		completeDualStages=completeDualStages&&operations>0u&&stage.maximumDepth>0u&&
			stage.maximumOutputRadius>0.0&&std::isfinite(stage.maximumOutputRadius);
	}
	Check(tracedDualOK&&tracedDualBytes&&completeDualStages,
		"dual palindrome trace seals fifteen local certificates and resets radii without changing fp32 bytes");

	RISE::FireProductionProjectionRequest projection32;
	projection32.shape.nx=4u;projection32.shape.ny=4u;projection32.shape.nz=4u;
	projection32.shape.cellWidthM=0.5f;projection32.timeStepS=0.01f;
	projection32.ambientDensityKGPerM3=1.0f;
	projection32.boundary.fill(RISE::FireProductionProjectionPeriodic);
	projection32.gasDensityKGPerM3.assign(64u,1.0f);
	projection32.provisionalMomentumKGPerM2S[0].assign(FaceCount(4u,16u),0.0f);
	projection32.provisionalMomentumKGPerM2S[1].assign(FaceCount(4u,16u),0.0f);
	projection32.provisionalMomentumKGPerM2S[2].assign(FaceCount(4u,16u),0.0f);
	projection32.divergenceTargetPerS.assign(64u,0.0f);
	RISE::FireProductionProjectionResult projection32Result;
	Check(RISE::ProjectFireProductionCPU(projection32,projection32Result,&error)&&
		projection32Result.maximumPostProjectionResidualPerS==0.0f,
		"binary32 production projection exact-zero owner");
	RISEFireProductionFP64::FireProductionProjectionRequest projection64;
	projection64.shape.nx=4u;projection64.shape.ny=4u;projection64.shape.nz=4u;
	projection64.shape.cellWidthM=0.5;projection64.timeStepS=0.01;
	projection64.ambientDensityKGPerM3=1.0;
	projection64.boundary.fill(RISEFireProductionFP64::FireProductionProjectionPeriodic);
	projection64.gasDensityKGPerM3.assign(64u,1.0);
	for(unsigned int axis=0u;axis<3u;++axis)
		projection64.provisionalMomentumKGPerM2S[axis].assign(FaceCount(4u,16u),0.0);
	projection64.divergenceTargetPerS.assign(64u,0.0);
	RISEFireProductionFP64::FireProductionProjectionResult projection64Result;
	Check(RISEFireProductionFP64::ProjectFireProductionCPU(projection64,projection64Result,&error)&&
		projection64Result.maximumPostProjectionResidualPerS==0.0,
		"generated binary64 projection preserves exact zero and one fixed solve");

	RISE::FireProductionResidentStepRequest step;
	step.force.shape=projection32.shape;step.force.timeStepS=projection32.timeStepS;
	step.force.ambientDensityKGPerM3=1.0f;step.force.vremanCoefficient=0.0f;
	step.force.boundary=projection32.boundary;
	step.force.cellGasDensityKGPerM3.assign(64u,1.0f);
	step.force.molecularKinematicViscosityM2PerS.assign(64u,0.0f);
	step.cellTransport.shape=projection32.shape;step.cellTransport.componentCount=9u;
	step.cellTransport.timeStepS=projection32.timeStepS;step.cellTransport.boundary=projection32.boundary;
	step.cellTransport.conservativeValues.assign(9u*64u,0.0f);
	step.cellTransport.ambientValues.assign(9u,0.0f);
	for(std::size_t cell=0u;cell<64u;++cell){
		step.cellTransport.conservativeValues[64u+cell]=1.0f;
		step.cellTransport.conservativeValues[8u*64u+cell]=300000.0f;
	}
	step.dualTransport.shape=projection32.shape;step.dualTransport.timeStepS=projection32.timeStepS;
	step.dualTransport.ambientDensityKGPerM3=1.0f;step.dualTransport.boundary=projection32.boundary;
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t faces=RISE::FireProductionProjectionFaceCount(projection32.shape,axis);
		step.force.faceDensityKGPerM3[axis].assign(faces,1.0f);
		step.force.beginningMomentumKGPerM2S[axis].assign(faces,0.0f);
		step.cellTransport.frozenVelocityMPerS[axis].assign(faces,0.0f);
		step.dualTransport.beginningFaceDensity[axis].assign(faces,1.0f);
		step.dualTransport.beginningMomentum[axis].assign(faces,0.0f);
		step.dualTransport.frozenVelocityMPerS[axis].assign(faces,0.0f);
		step.momentumSourceIncrement[axis].assign(faces,0.0f);
	}
	step.cellSourceIncrement.assign(9u*64u,0.0f);
	step.divergenceTargetPerS.assign(64u,0.0f);
	step.restorationDivergenceTargetPerS.assign(64u,0.0f);
	step.monitorManifoldDiagnostics=true;
	step.enforceManifoldPlateau=true;
	FireProductionCalibration::ResidentStep64Result step64;
	Check(FireProductionCalibration::AdvanceResidentStep64(step,0.0,step64,&error)&&
		step64.force.schedule.substepCount==1u&&step64.cell.executedSubmapCount==5u&&
		step64.dual.executedSubmapCount==15u&&
		step64.projection.executedVCycleCount==12u&&
		step64.projection.maximumPostProjectionResidualPerS==0.0,
		"binary64 mirror composes force, five cell maps, fifteen dual maps, sources, and one P2");
	RISE::FireProductionResidentStepRequest nonDefaultCycleStep=step;
	nonDefaultCycleStep.physicalOpenProjectionVCycleCount=19u;
	nonDefaultCycleStep.force.shape.nx=5u;nonDefaultCycleStep.force.shape.ny=5u;
	nonDefaultCycleStep.force.shape.nz=5u;
	nonDefaultCycleStep.cellTransport.shape=nonDefaultCycleStep.force.shape;
	nonDefaultCycleStep.dualTransport.shape=nonDefaultCycleStep.force.shape;
	nonDefaultCycleStep.force.boundary[0]=RISE::FireProductionProjectionPressureOpen;
	nonDefaultCycleStep.force.boundary[1]=RISE::FireProductionProjectionPressureOpen;
	nonDefaultCycleStep.cellTransport.boundary[0]=RISE::FireProductionProjectionPressureOpen;
	nonDefaultCycleStep.cellTransport.boundary[1]=RISE::FireProductionProjectionPressureOpen;
	nonDefaultCycleStep.dualTransport.boundary[0]=RISE::FireProductionProjectionPressureOpen;
	nonDefaultCycleStep.dualTransport.boundary[1]=RISE::FireProductionProjectionPressureOpen;
	const std::size_t nonDefaultCells=nonDefaultCycleStep.force.shape.CellCount();
	nonDefaultCycleStep.force.cellGasDensityKGPerM3.assign(nonDefaultCells,1.0f);
	nonDefaultCycleStep.force.molecularKinematicViscosityM2PerS.assign(nonDefaultCells,0.0f);
	nonDefaultCycleStep.cellTransport.conservativeValues.assign(9u*nonDefaultCells,0.0f);
	for(std::size_t cell=0u;cell<nonDefaultCells;++cell){
		nonDefaultCycleStep.cellTransport.conservativeValues[nonDefaultCells+cell]=1.0f;
		nonDefaultCycleStep.cellTransport.conservativeValues[8u*nonDefaultCells+cell]=300000.0f;
	}
	nonDefaultCycleStep.cellSourceIncrement.assign(9u*nonDefaultCells,0.0f);
	nonDefaultCycleStep.divergenceTargetPerS.assign(nonDefaultCells,0.0f);
	nonDefaultCycleStep.restorationDivergenceTargetPerS.assign(nonDefaultCells,0.0f);
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t faces=RISE::FireProductionProjectionFaceCount(
			nonDefaultCycleStep.force.shape,axis);
		nonDefaultCycleStep.force.faceDensityKGPerM3[axis].assign(faces,1.0f);
		nonDefaultCycleStep.force.beginningMomentumKGPerM2S[axis].assign(faces,0.0f);
		nonDefaultCycleStep.cellTransport.frozenVelocityMPerS[axis].assign(faces,0.0f);
		nonDefaultCycleStep.dualTransport.beginningFaceDensity[axis].assign(faces,1.0f);
		nonDefaultCycleStep.dualTransport.beginningMomentum[axis].assign(faces,0.0f);
		nonDefaultCycleStep.dualTransport.frozenVelocityMPerS[axis].assign(faces,0.0f);
		nonDefaultCycleStep.momentumSourceIncrement[axis].assign(faces,0.0f);
	}
	FireProductionCalibration::ResidentStep64Result nonDefaultCycleStep64;
	const bool nonDefaultCycleMirrorOK=FireProductionCalibration::AdvanceResidentStep64(
		nonDefaultCycleStep,0.0,nonDefaultCycleStep64,&error);
	if(!nonDefaultCycleMirrorOK||nonDefaultCycleStep64.physicalProjection.executedVCycleCount!=19u||
		nonDefaultCycleStep64.projection.executedVCycleCount!=16u)
		std::fprintf(stderr,"non-default mirror detail ok=%d physical=%u restoration=%u error=%s\n",
			nonDefaultCycleMirrorOK?1:0,
			nonDefaultCycleStep64.physicalProjection.executedVCycleCount,
			nonDefaultCycleStep64.projection.executedVCycleCount,error.c_str());
	Check(nonDefaultCycleMirrorOK&&
		nonDefaultCycleStep64.physicalProjection.executedVCycleCount==19u&&
		nonDefaultCycleStep64.projection.executedVCycleCount==16u,
		"binary64 mirror propagates the request-owned non-default physical V-cycle count");
	RISE::FireProductionResidentStepRequest monitoredStep=nonDefaultCycleStep;
	monitoredStep.enforceManifoldPlateau=false;
	FireProductionCalibration::ResidentStep64Result monitoredStep64;
	const bool monitoredStepOK=FireProductionCalibration::AdvanceResidentStep64(
		monitoredStep,0.0,monitoredStep64,&error);
	if(!monitoredStepOK||monitoredStep64.physicalProjection.executedVCycleCount!=0u||
		monitoredStep64.projection.executedVCycleCount!=16u)
		std::fprintf(stderr,"monitored mirror detail ok=%d physical=%u terminal=%u error=%s\n",
			monitoredStepOK?1:0,
			monitoredStep64.physicalProjection.executedVCycleCount,
			monitoredStep64.projection.executedVCycleCount,error.c_str());
	Check(monitoredStepOK&&
		monitoredStep64.physicalProjection.executedVCycleCount==0u&&
		monitoredStep64.projection.executedVCycleCount==16u,
		"binary64 mirror executes the monitored standalone one-projection topology");
	FireProductionRoundoffAdapter::ResidentStepTraceResult tracedStep;
	const bool tracedStepOK=FireProductionRoundoffAdapter::AdvanceResidentStepTrace(
		step,0.0f,tracedStep,&error);
	bool tracedStepStages=tracedStep.stages.size()==24u;
	for(std::size_t stageIndex=0u;stageIndex<tracedStep.stages.size();++stageIndex){
		const FireProductionRoundoffTrace::Observation& stage=tracedStep.stages[stageIndex];
		std::uint64_t operations=0u;for(const std::uint64_t count:stage.operation)operations+=count;
		if(!(operations>0u&&stage.maximumDepth>0u&&stage.maximumOutputRadius>=0.0&&
			std::isfinite(stage.maximumOutputRadius)))std::fprintf(stderr,
			"roundoff stage detail index=%zu operations=%llu depth=%u radius=%.17g branch=%.17g\n",
			stageIndex,static_cast<unsigned long long>(operations),stage.maximumDepth,
			stage.maximumOutputRadius,stage.transportBranchDivergenceBound);
		tracedStepStages=tracedStepStages&&operations>0u&&stage.maximumDepth>0u&&
			stage.maximumOutputRadius>=0.0&&std::isfinite(stage.maximumOutputRadius);
	}
	const bool tracedStepBytes=tracedStep.conservativeValues.size()==
		step64.conservativeValues.size()&&std::all_of(tracedStep.conservativeValues.begin(),
		tracedStep.conservativeValues.end(),[](const FireProductionRoundoffTrace::TraceFloat& value){
			return value.Radius()==0.0&&std::isfinite(value.Rounded());});
	Check(tracedStepOK&&tracedStepStages&&tracedStepBytes&&
		tracedStep.force.schedule.substepCount==1u&&
		tracedStep.cell.executedSubmapCount==5u&&tracedStep.dual.executedSubmapCount==15u&&
		tracedStep.physicalProjection.maximumPostProjectionResidualPerS.Rounded()==0.0f&&
		tracedStep.projection.maximumPostProjectionResidualPerS.Rounded()==0.0f,
		"roundoff adapter seals the complete force-transport-source-two-projection graph before measurement");
	FireProductionRoundoffAdapter::ResidentStepTraceResult nonDefaultCycleTracedStep;
	const bool nonDefaultCycleTraceOK=FireProductionRoundoffAdapter::AdvanceResidentStepTrace(
		nonDefaultCycleStep,0.0f,nonDefaultCycleTracedStep,&error);
	if(!nonDefaultCycleTraceOK||nonDefaultCycleTracedStep.physicalProjection.executedVCycleCount!=19u||
		nonDefaultCycleTracedStep.projection.executedVCycleCount!=16u)
		std::fprintf(stderr,"non-default trace detail ok=%d physical=%u restoration=%u error=%s\n",
			nonDefaultCycleTraceOK?1:0,
			nonDefaultCycleTracedStep.physicalProjection.executedVCycleCount,
			nonDefaultCycleTracedStep.projection.executedVCycleCount,error.c_str());
	Check(nonDefaultCycleTraceOK&&
		nonDefaultCycleTracedStep.physicalProjection.executedVCycleCount==19u&&
		nonDefaultCycleTracedStep.projection.executedVCycleCount==16u,
		"roundoff adapter propagates the request-owned non-default physical V-cycle count");
	FireProductionRoundoffAdapter::ResidentStepTraceResult monitoredTrace;
	Check(FireProductionRoundoffAdapter::AdvanceResidentStepTrace(
		monitoredStep,0.0f,monitoredTrace,&error)&&
		monitoredTrace.projection.executedVCycleCount==19u&&
		monitoredTrace.conservativeValues.size()==
			nonDefaultCycleTracedStep.conservativeValues.size(),
		"roundoff trace executes the same monitored one-projection topology as production");
	if(!(tracedStepOK&&tracedStepStages&&tracedStepBytes))
		std::fprintf(stderr,"roundoff full-step detail ok=%d stages=%d bytes=%d count=%zu error=%s\n",
			tracedStepOK?1:0,tracedStepStages?1:0,tracedStepBytes?1:0,
			tracedStep.stages.size(),error.c_str());

	// r184 prerequisite: a completed Q* or Q^{n+1} is not accepted merely
	// because it passed r60.  Its signed inversion, EOS residual, case ceiling,
	// and exact state/attempt/role identity must all agree.
	const RISE::FireSimulationMethaneRecord& eosRecord=
		RISE::FireSimulationMethaneRecord::PhysicalV1();
	RISE::FireCase::AuthoredV1 eosAuthored;
	eosAuthored.fuelRecordId=eosRecord.RecordId();eosAuthored.poolDiameterM=0.03;
	eosAuthored.heatReleaseRateKW=0.10;
	eosAuthored.envelope={{0.0,0.0},{0.5,1.0},{1.0,1.0}};
	eosAuthored.durationS=1.0;eosAuthored.quality="draft";eosAuthored.seed=184u;
	eosAuthored.outputFramesPerS=4.0;
	RISE::FireCase::RecordV1 eosCase;
	const bool eosCaseOK=RISE::FireCase::BuildMethaneV1(eosAuthored,eosRecord,
		{eosRecord.RecordId()},eosCase,error);
	Check(eosCaseOK&&eosCase.derived.pilotAmbientTemperatureK==300.0&&
		eosCase.derived.maximumAcceptedTemperatureK==2300.0,
		"accepted-state EOS gate derives its bounds from an authenticated case record");
	auto eosTuple=[&](const double temperatureK){
		std::array<float,9> tuple={{}};
		const std::vector<double>& massFraction=eosRecord.AmbientMassFractions();
		double reciprocalWeight=0.0;
		for(std::size_t species=0u;species<7u;++species){
			const RISE::FireThermochemistrySpecies* property=eosRecord.FindSpecies(
				eosRecord.SpeciesOrder()[species].c_str());
			reciprocalWeight+=massFraction[species]/property->molecularWeightKGPerKMol;
		}
		const double density=eosRecord.ThermodynamicPressurePa()/
			(8314.46261815324*temperatureK*reciprocalWeight);
		for(std::size_t species=0u;species<7u;++species)
			tuple[1u+species]=static_cast<float>(density*massFraction[species]);
		std::array<double,7> enthalpy;
		eosRecord.SensibleEnthalpiesBySpeciesOrderJPerKG(temperatureK,
			enthalpy.data(),enthalpy.size(),&error);
		double energy=0.0;for(std::size_t species=0u;species<7u;++species)
			energy+=static_cast<double>(tuple[1u+species])*enthalpy[species];
		tuple[8u]=static_cast<float>(energy);return tuple;
	};
	auto eosRequestFromTuple=[&](const std::array<float,9>& tuple){
		RISE::FireProductionScalarEOSAcceptanceRequest request;
		request.shape.nx=4u;request.shape.ny=5u;request.shape.nz=4u;
		request.shape.cellWidthM=0.01f;request.timeStepS=0.0005f;
		request.attemptIdentity=UINT64_C(0x1840000000000001);
		request.stage=RISE::FireProductionScalarEOSStage::QStar;
		request.producerPrecision=RISE::FireStateProducerPrecision::Binary32;
		request.methaneRecordId=eosRecord.RecordId();
		request.caseRecordEnvelope=eosCase.envelopeBytes;
		const std::size_t cells=request.shape.CellCount();
		request.conservativeValues.resize(9u*cells);
		for(std::size_t component=0u;component<9u;++component)
			for(std::size_t cell=0u;cell<cells;++cell)
				request.conservativeValues[component*cells+cell]=tuple[component];
		return request;
	};
	const std::array<float,9> eosPhysicalTuple=eosTuple(900.0);
	RISE::FireProductionScalarEOSAcceptanceRequest eosRequest=
		eosRequestFromTuple(eosPhysicalTuple);
	std::vector<RISE::FireSim::ConservativeVector> eosOracleState(
		eosRequest.shape.CellCount());
	for(std::size_t cell=0u;cell<eosOracleState.size();++cell)
		for(std::size_t component=0u;component<9u;++component)
			eosOracleState[cell][component]=static_cast<double>(
				eosRequest.conservativeValues[component*eosOracleState.size()+cell]);
	std::vector<double> eosOracleTemperature;
	const bool eosOracleOK=RISE::FireSim::InvertPeriodicTemperaturesWithinBounds(
		eosOracleState,eosRecord,eosCase.derived.pilotAmbientTemperatureK,
		eosCase.derived.maximumAcceptedTemperatureK,
		RISE::FireStateProducerPrecision::Binary32,eosOracleTemperature,&error);
	RISE::FireProductionScalarEOSAcceptanceResult eosAccepted;
	const bool eosAcceptedOK=RISE::EvaluateFireProductionScalarEOSAcceptanceCPU(
		eosRequest,eosAccepted,&error);
	RISE::FireSim::MethaneCellState eosOracleCell=RISE::FireSim::FromConservativeVector(
		eosOracleState[0],RISE::FireStateProducerPrecision::Binary32);
	if(!eosAccepted.temperatureK.empty())eosOracleCell.temperatureK=
		static_cast<double>(eosAccepted.temperatureK[0]);
	double eosOracleResidual=0.0;
	const bool eosOracleResidualOK=!eosAccepted.temperatureK.empty()&&
		RISE::FireSim::EquationOfStateResidual(eosOracleCell,eosRecord,
			RISE::FireStateProducerPrecision::Binary32,eosOracleResidual,&error);
	bool eosTemperatureExact=eosAccepted.temperatureK.size()==eosOracleTemperature.size();
	for(std::size_t cell=0u;cell<eosAccepted.temperatureK.size()&&eosTemperatureExact;++cell)
		eosTemperatureExact=sameFloatBits(eosAccepted.temperatureK[cell],
			static_cast<float>(eosOracleTemperature[cell]));
	Check(eosOracleOK&&eosAcceptedOK&&eosOracleResidualOK&&eosTemperatureExact&&
		eosAccepted.accepted&&eosAccepted.maximumEOSResidual==eosOracleResidual&&
		eosAccepted.maximumEOSResidual<=1.0e-3&&
		RISE::FireProductionScalarEOSAcceptanceMatches(eosRequest,eosAccepted),
		"accepted-state EOS gate agrees exactly with the Section 3.7 binary32 oracle");

	RISE::FireProductionScalarEOSAcceptanceRequest eosFinalRequest=eosRequest;
	eosFinalRequest.stage=RISE::FireProductionScalarEOSStage::QNPlus1;
	RISE::FireProductionScalarEOSAcceptanceResult eosFinalAccepted;
	Check(RISE::EvaluateFireProductionScalarEOSAcceptanceCPU(eosFinalRequest,
		eosFinalAccepted,&error)&&
		RISE::FireProductionScalarEOSAcceptanceMatches(eosFinalRequest,eosFinalAccepted)&&
		eosFinalAccepted.acceptanceIdentity!=eosAccepted.acceptanceIdentity,
		"accepted-state EOS identity distinguishes QStar from QNPlus1");

	RISE::FireProductionScalarEOSAcceptanceRequest eosScaled=eosRequest;
	for(float& value:eosScaled.conservativeValues)value*=1.01f;
	RISE::FireProductionScalarEOSAcceptanceResult eosScaledAccepted;
	Check(RISE::EvaluateFireProductionScalarEOSAcceptanceCPU(eosScaled,
		eosScaledAccepted,&error)&&eosScaledAccepted.accepted&&
		eosScaledAccepted.maximumEOSResidual>0.009&&
		eosScaledAccepted.maximumEOSResidual<0x1p-5&&
		RISE::FireProductionScalarEOSAcceptanceMatches(eosScaled,eosScaledAccepted),
		"production monitors a one-percent P0 deviation without resurrecting the oracle 1e-3 detector");

	RISE::FireProductionScalarEOSAcceptanceRequest eosCeiling=
		eosRequestFromTuple(eosTuple(2300.0));
	RISE::FireProductionScalarEOSAcceptanceResult eosRejected=eosAccepted;
	Check(!RISE::EvaluateFireProductionScalarEOSAcceptanceCPU(eosCeiling,eosRejected,&error)&&
		error.find("case ceiling")!=std::string::npos&&!eosRejected.accepted&&
		eosRejected.temperatureK.empty(),
		"accepted-state EOS gate enforces the strict case temperature ceiling atomically");

	std::array<float,9> eosNegativeTuple=eosPhysicalTuple;
	double eosMassScale=0.0;for(std::size_t component=0u;component<8u;++component)
		eosMassScale+=std::fabs(static_cast<double>(eosNegativeTuple[component]));
	const double eosNegativeBound=eosRecord.AcceptedStateFeasibilityEnvelope().kappaEpsilon32*
		static_cast<double>(std::numeric_limits<float>::epsilon())*std::max(1.0,eosMassScale);
	eosNegativeTuple[7u]=static_cast<float>(-0.125*eosNegativeBound);
	std::array<double,7> eosNegativeEnthalpy;
	eosRecord.SensibleEnthalpiesBySpeciesOrderJPerKG(900.0,eosNegativeEnthalpy.data(),
		eosNegativeEnthalpy.size(),&error);
	double eosNegativeEnergy=0.0;for(std::size_t species=0u;species<7u;++species)
		eosNegativeEnergy+=static_cast<double>(eosNegativeTuple[1u+species])*
			eosNegativeEnthalpy[species];
	eosNegativeTuple[8u]=static_cast<float>(eosNegativeEnergy);
	RISE::FireProductionScalarEOSAcceptanceRequest eosNegative=
		eosRequestFromTuple(eosNegativeTuple);
	RISE::FireProductionScalarEOSAcceptanceResult eosNegativeAccepted;
	Check(RISE::EvaluateFireProductionScalarEOSAcceptanceCPU(eosNegative,
		eosNegativeAccepted,&error)&&eosNegativeAccepted.accepted,
		"accepted-state EOS gate preserves signed inversion for an envelope-negative tuple");

	bool eosMutationsRejected=true;
	auto eosMutationRejected=[&](RISE::FireProductionScalarEOSAcceptanceRequest mutation){
		return !RISE::FireProductionScalarEOSAcceptanceMatches(mutation,eosAccepted);
	};
	RISE::FireProductionScalarEOSAcceptanceRequest eosMutation=eosRequest;
	eosMutation.conservativeValues[0]=std::nextafter(eosMutation.conservativeValues[0],1.0f);
	eosMutationsRejected=eosMutationsRejected&&eosMutationRejected(eosMutation);
	eosMutation=eosRequest;eosMutation.timeStepS=std::nextafter(eosMutation.timeStepS,1.0f);
	eosMutationsRejected=eosMutationsRejected&&eosMutationRejected(eosMutation);
	eosMutation=eosRequest;++eosMutation.attemptIdentity;
	eosMutationsRejected=eosMutationsRejected&&eosMutationRejected(eosMutation);
	eosMutation=eosRequest;eosMutation.stage=RISE::FireProductionScalarEOSStage::QNPlus1;
	eosMutationsRejected=eosMutationsRejected&&eosMutationRejected(eosMutation);
	eosMutation=eosRequest;eosMutation.caseRecordEnvelope.back()^=1u;
	eosMutationsRejected=eosMutationsRejected&&eosMutationRejected(eosMutation);
	eosMutation=eosRequest;eosMutation.methaneRecordId.back()=
		eosMutation.methaneRecordId.back()=='a'?'b':'a';
	eosMutationsRejected=eosMutationsRejected&&eosMutationRejected(eosMutation);
	eosMutation=eosRequest;std::swap(eosMutation.shape.nx,eosMutation.shape.ny);
	eosMutationsRejected=eosMutationsRejected&&eosMutationRejected(eosMutation);
	RISE::FireProductionScalarEOSAcceptanceResult eosPublishedMutation=eosAccepted;
	eosPublishedMutation.temperatureK[0]=std::nextafter(eosPublishedMutation.temperatureK[0],
		1000.0f);
	eosMutationsRejected=eosMutationsRejected&&
		!RISE::FireProductionScalarEOSAcceptanceMatches(eosRequest,eosPublishedMutation);
	eosPublishedMutation=eosAccepted;
	eosPublishedMutation.maximumEOSResidual=std::nextafter(
		eosPublishedMutation.maximumEOSResidual,1.0);
	eosMutationsRejected=eosMutationsRejected&&
		!RISE::FireProductionScalarEOSAcceptanceMatches(eosRequest,eosPublishedMutation);
	eosPublishedMutation=eosAccepted;
	eosPublishedMutation.upperTemperatureK=2500.0;
	eosMutationsRejected=eosMutationsRejected&&
		!RISE::FireProductionScalarEOSAcceptanceMatches(eosRequest,eosPublishedMutation);
	Check(eosMutationsRejected,
		"accepted-state EOS identity rejects state, output, dt, attempt, role, record, bound, and shape mutations");

	RISE::FireProductionProjectionShape eosUnderCap,eosOverCap;
	eosUnderCap.nx=63u;eosUnderCap.ny=884u;eosUnderCap.nz=964u;
	eosUnderCap.cellWidthM=1.0f;eosOverCap.nx=116u;eosOverCap.ny=634u;
	eosOverCap.nz=730u;eosOverCap.cellWidthM=1.0f;
	std::uint64_t eosUnderBytes=0u,eosOverBytes=0u;
	const bool eosUnderQuery=RISE::QueryFireProductionScalarEOSAcceptanceCPUWorkingSetBytes(
		eosUnderCap,eosUnderBytes,&error);
	const bool eosOverQuery=RISE::QueryFireProductionScalarEOSAcceptanceCPUWorkingSetBytes(
		eosOverCap,eosOverBytes,&error);
	RISE::FireProductionScalarEOSAcceptanceRequest eosOversized=eosRequest;
	eosOversized.shape=eosOverCap;eosOversized.conservativeValues.clear();eosRejected=eosAccepted;
	Check(eosUnderQuery&&eosOverQuery&&eosUnderBytes==UINT64_C(2147483520)&&
		eosOverBytes==UINT64_C(2147484800)&&
		!RISE::EvaluateFireProductionScalarEOSAcceptanceCPU(eosOversized,eosRejected,&error)&&
		error.find("working set exceeds two GiB")!=std::string::npos&&
		!eosRejected.accepted&&eosRejected.temperatureK.empty(),
		"accepted-state EOS exact working-set query enforces the two-GiB cap before payload access");

	// r186 prerequisite: only the canonical grid source producer can mint the
	// opaque production packet seal.  Its input surface contains controls and a
	// beginning state, never a caller-authored conservative dose or pilot pair.
	const RISE::FireSimulationTransportRecord& sourceTransport=
		RISE::FireSimulationTransportRecord::OpenV1();
	const RISE::FireSimulationGasOpacityRecord& sourceOpacity=
		RISE::FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1();
	RISE::FireCase::RecordV1 sourceCase;
	const bool sourceCaseOK=RISE::FireCase::BuildMethaneV1(eosAuthored,eosRecord,
		{eosRecord.RecordId(),sourceTransport.RecordId(),sourceOpacity.RecordId()},
		sourceCase,error);
	RISE::FireProductionFrozenMethaneSourceRequest sourceRequest;
	sourceRequest.shape.nx=4u;sourceRequest.shape.ny=4u;sourceRequest.shape.nz=4u;
	sourceRequest.shape.cellWidthM=0.01f;sourceRequest.timeStepS=0.0005f;
	sourceRequest.beginningTimeS=0.0;
	sourceRequest.attemptIdentity=UINT64_C(0x1860000000000001);
	sourceRequest.caseRecordEnvelope=sourceCase.envelopeBytes;
	const std::size_t sourceCells=sourceRequest.shape.CellCount();
	const std::array<float,9> sourceBeginningTuple=eosTuple(300.0);
	sourceRequest.beginningConservativeValues.resize(9u*sourceCells);
	for(std::size_t component=0u;component<9u;++component)
		for(std::size_t cell=0u;cell<sourceCells;++cell)
			sourceRequest.beginningConservativeValues[component*sourceCells+cell]=
				sourceBeginningTuple[component];
	sourceRequest.pilotCommandMask.assign(sourceCells,0u);
	sourceRequest.pilotCommandMask[0u]=1u;
	sourceRequest.mixingTimeS.assign(sourceCells,sourceTransport.ChemicalTimeS());
	sourceRequest.predictiveRadiation=false;sourceRequest.workerCount=2u;
	RISE::FireProductionFrozenSourcePacketSeal sourceSeal;
	const bool sourceSealOK=sourceCaseOK&&
		RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
			sourceRequest,sourceSeal,&error);
	if(!sourceSealOK)std::fprintf(stderr,"canonical source detail: %s\n",error.c_str());
	RISE::FireSim::ConservativeVector sourceBeginningVector;
	for(std::size_t index=0u;index<9u;++index)sourceBeginningVector[index]=
		static_cast<double>(sourceBeginningTuple[index]);
	RISE::FireSim::MethaneCellState sourceBeginning=
		RISE::FireSim::FromConservativeVector(sourceBeginningVector,
			RISE::FireStateProducerPrecision::Binary32);
	sourceBeginning.temperatureK=300.0;
	double sourcePilotCommand=0.0;
	const bool sourcePilotCommandOK=RISE::FireCase::EvaluatePilotSetpointTemperatureK(
		sourceCase.derived,true,sourceRequest.beginningTimeS,
		sourceRequest.beginningTimeS+sourceRequest.timeStepS,sourcePilotCommand,error);
	const double sourcePilotTarget=std::min(sourcePilotCommand,
		sourceBeginning.temperatureK*sourceCase.derived.pilotExpansionVolumeRatioCap);
	double sourcePilotBeginningEnergy=0.0,sourcePilotTargetEnergy=0.0;
	const bool sourcePilotEnergyOK=RISE::FireSim::SignedMixtureSensibleEnergy(sourceBeginning,
		sourceBeginning.temperatureK,eosRecord,sourcePilotBeginningEnergy,&error)&&
		RISE::FireSim::SignedMixtureSensibleEnergy(sourceBeginning,sourcePilotTarget,
			eosRecord,sourcePilotTargetEnergy,&error);
	const double sourcePilotVolume=sourcePilotTarget/sourceBeginning.temperatureK;
	const double sourcePilotExpected=(sourcePilotTargetEnergy-sourcePilotBeginningEnergy)/
		sourcePilotVolume;
	bool sourceDivergenceExact=sourceSealOK&&
		sourceSeal.DivergenceTargetPerS().size()==sourceCells;
	for(std::size_t cell=0u;cell<sourceCells&&sourceDivergenceExact;++cell){
		RISE::FireSim::MethaneSourcePacket packet;
		for(std::size_t species=0u;species<7u;++species)
			packet.constituentDelta[species]=static_cast<double>(
				sourceSeal.SourceDelta()[(1u+species)*sourceCells+cell]);
		packet.sensibleEnergyDeltaJPerM3=static_cast<double>(
			sourceSeal.SourceDelta()[8u*sourceCells+cell]);
		packet.pilotEnergyDeltaJPerM3=sourceSeal.PilotEnergyDeltaJPerM3()[cell];
		packet.pilotExpansionIntegral=sourceSeal.PilotExpansionIntegral()[cell];
		double scaled=0.0;
		sourceDivergenceExact=RISE::FireSim::FrozenSourcePacketExpansionAdmissible(
			sourceBeginningVector,sourceBeginning.temperatureK,packet,
			sourceRequest.timeStepS,eosRecord,RISE::FireStateProducerPrecision::Binary32,
			&scaled,&error)&&sameFloatBits(sourceSeal.DivergenceTargetPerS()[cell],
				static_cast<float>(scaled/static_cast<double>(sourceRequest.timeStepS)));
	}
	Check(sourceSealOK&&sourceSeal.IsSealed()&&
		RISE::FireProductionFrozenSourcePacketSealMatches(sourceSeal,&error)&&
		sourceSeal.SourceDelta().size()==9u*sourceCells&&
		sourceDivergenceExact&&
		sourceSeal.PilotEnergyDeltaJPerM3().size()==sourceCells&&
		sourceSeal.PilotExpansionIntegral().size()==sourceCells&&
		sourceSeal.PacketIdentity()!=0u&&sourceSeal.PacketContentIdentity()!=0u&&
		sourceSeal.GlobalRadiationIdentity()!=0u,
		"canonical grid producer mints one complete identity-bearing frozen source seal");
	Check(sourceSealOK&&sourcePilotCommandOK&&sourcePilotEnergyOK&&sourcePilotVolume>1.0&&
		sourceSeal.PilotEnergyDeltaJPerM3()[0u]==sourcePilotExpected&&
		sourceSeal.PilotExpansionIntegral()[0u]==1.0-1.0/sourcePilotVolume,
		"canonical source seal retains the exact pilot 1/V-prime energy and expansion pair");
	// The trace mirror must compare represented binary32 values, never the wider
	// TraceFloat object's center/radius/identity storage.  These two REDs cover
	// both historical escape directions: distinct trace lineage with identical
	// rounded bytes passes, while a one-bit rounded mutation refuses.
	std::vector<FireProductionRoundoffTrace::TraceFloat> representedFirst={
		FireProductionRoundoffTrace::TraceFloat::Raw(300.0,0.0,300.0f,1u),
		FireProductionRoundoffTrace::TraceFloat::Raw(900.0,0.0,900.0f,2u)};
	std::vector<FireProductionRoundoffTrace::TraceFloat> representedSame={
		FireProductionRoundoffTrace::TraceFloat::Raw(301.0,2.0,300.0f,7u),
		FireProductionRoundoffTrace::TraceFloat::Raw(899.0,3.0,900.0f,9u)};
	std::vector<FireProductionRoundoffTrace::TraceFloat> representedOneBit=representedSame;
	const float changed=std::nextafter(representedOneBit[0u].Rounded(),1000.0f);
	representedOneBit[0u]=FireProductionRoundoffTrace::TraceFloat::Raw(
		static_cast<double>(changed),0.0,changed,0u);
	const bool equalRoundedDifferentTraceAccepted=
		RISEFireProductionTrace::CalibrationSameRepresentedFloatVectorBits(
			representedFirst,representedSame);
	const bool oneBitRoundedMutationRefused=
		!RISEFireProductionTrace::CalibrationSameRepresentedFloatVectorBits(
			representedFirst,representedOneBit);
	const bool equalRoundedForceAccepted=
		RISEFireProductionTrace::CalibrationSameRepresentedForceFloatBits(
			representedFirst[0u],representedSame[0u]);
	const bool oneBitForceRefused=
		!RISEFireProductionTrace::CalibrationSameRepresentedForceFloatBits(
			representedFirst[0u],representedOneBit[0u]);
	const bool equalRoundedOwnerAccepted=
		RISEFireProductionTrace::CalibrationSameRepresentedOwnerFloatVectorBits(
			representedFirst,representedSame);
	const bool oneBitOwnerRefused=
		!RISEFireProductionTrace::CalibrationSameRepresentedOwnerFloatVectorBits(
			representedFirst,representedOneBit);
	Check(equalRoundedDifferentTraceAccepted&&oneBitRoundedMutationRefused&&
		equalRoundedForceAccepted&&oneBitForceRefused&&equalRoundedOwnerAccepted&&
		oneBitOwnerRefused,
		"roundoff transport, force, and owner mirrors compare represented binary32 values, not TraceFloat storage");
	RISE::FireProductionFrozenMethaneSourceRequest sourceAttemptMutation=sourceRequest;
	++sourceAttemptMutation.attemptIdentity;
	RISE::FireProductionFrozenSourcePacketSeal sourceAttemptSeal;
	const bool sourceAttemptOK=RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
		sourceAttemptMutation,sourceAttemptSeal,&error);
	Check(sourceSealOK&&sourceAttemptOK&&
		sourceSeal.SourceDelta()==sourceAttemptSeal.SourceDelta()&&
		sourceSeal.PacketContentIdentity()==sourceAttemptSeal.PacketContentIdentity()&&
		sourceSeal.PacketIdentity()!=sourceAttemptSeal.PacketIdentity()&&
		sourceSeal.SourceInputIdentity()!=sourceAttemptSeal.SourceInputIdentity(),
		"canonical source seal binds attempt identity without perturbing physical packet bytes");
	RISE::FireProductionFrozenMethaneSourceRequest sourceSerialRequest=sourceRequest;
	sourceSerialRequest.workerCount=1u;
	RISE::FireProductionFrozenSourcePacketSeal sourceSerialSeal;
	const bool sourceSerialOK=RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
		sourceSerialRequest,sourceSerialSeal,&error);
	Check(sourceSealOK&&sourceSerialOK&&
		sourceSeal.SourceDelta()==sourceSerialSeal.SourceDelta()&&
		sourceSeal.PacketIdentity()==sourceSerialSeal.PacketIdentity()&&
		sourceSeal.GlobalRadiationIdentity()==sourceSerialSeal.GlobalRadiationIdentity(),
		"canonical source packet is byte-identical across admitted worker schedules");
	RISE::FireProductionFrozenMethaneSourceRequest sourceControlMutation=sourceRequest;
	sourceControlMutation.mixingTimeS[1u]=std::nextafter(
		sourceControlMutation.mixingTimeS[1u],1.0);
	RISE::FireProductionFrozenSourcePacketSeal sourceControlSeal;
	const bool sourceControlOK=RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
		sourceControlMutation,sourceControlSeal,&error);
	Check(sourceControlOK&&sourceControlSeal.ReactionControlIdentity()!=
		sourceSeal.ReactionControlIdentity()&&sourceControlSeal.PacketIdentity()!=
		sourceSeal.PacketIdentity(),
		"canonical source seal binds every stage-derived mixing-control byte");
	RISE::FireProductionFrozenMethaneSourceRequest badSourceRequest=sourceRequest;
	badSourceRequest.pilotCommandMask[0u]=2u;
	RISE::FireProductionFrozenSourcePacketSeal rejectedSource=sourceSeal;
	const bool badMaskRejected=!RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
		badSourceRequest,rejectedSource,&error)&&!rejectedSource.IsSealed();
	badSourceRequest=sourceRequest;badSourceRequest.caseRecordEnvelope.back()^=1u;
	const bool badCaseRejected=!RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
		badSourceRequest,rejectedSource,&error)&&!rejectedSource.IsSealed();
	RISE::FireProductionFrozenSourcePacketSeal emptySourceSeal;
	Check(badMaskRejected&&badCaseRejected&&
		!RISE::FireProductionFrozenSourcePacketSealMatches(emptySourceSeal,&error)&&
		!std::is_aggregate<RISE::FireProductionFrozenSourcePacketSeal>::value,
		"raw controls, corrupt case bytes, and public empty objects cannot mint source authority");
	RISE::FireProductionFrozenMethaneSourceRequest sourceCeiling=sourceRequest;
	const std::array<float,9> sourceCeilingTuple=eosTuple(2300.0);
	for(std::size_t component=0u;component<9u;++component)
		for(std::size_t cell=0u;cell<sourceCells;++cell)
			sourceCeiling.beginningConservativeValues[component*sourceCells+cell]=
				sourceCeilingTuple[component];
	Check(!RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
			sourceCeiling,rejectedSource,&error)&&!rejectedSource.IsSealed()&&
		error.find("strict case ceiling")!=std::string::npos,
		"canonical source producer derives temperature from Q and refuses the exact case ceiling");
	RISE::FireProductionFrozenMethaneSourceRequest sourceBelowCase=sourceRequest;
	std::array<float,9> sourceBelowCaseTuple=eosTuple(300.0);
	// The thermochemistry domain begins at 300 K, so constructing a 299 K tuple
	// through the public enthalpy API would itself be invalid.  Exercise the
	// promised below-endpoint roundoff clamp with the nearest binary32 energy
	// strictly below the exact 300 K energy for the encoded constituent tuple.
	std::array<double,7> sourceLowerEnthalpy;
	const bool sourceLowerEnthalpyOK=eosRecord.SensibleEnthalpiesBySpeciesOrderJPerKG(
		300.0,sourceLowerEnthalpy.data(),sourceLowerEnthalpy.size(),&error);
	double sourceLowerEnergy=0.0;
	for(std::size_t species=0u;species<7u;++species)
		sourceLowerEnergy+=static_cast<double>(sourceBelowCaseTuple[1u+species])*
			sourceLowerEnthalpy[species];
	float sourceBelowEnergy=static_cast<float>(sourceLowerEnergy);
	while(static_cast<double>(sourceBelowEnergy)>=sourceLowerEnergy)
		sourceBelowEnergy=std::nextafter(sourceBelowEnergy,
			-std::numeric_limits<float>::infinity());
	sourceBelowCaseTuple[8u]=sourceBelowEnergy;
	for(std::size_t component=0u;component<9u;++component)
		for(std::size_t cell=0u;cell<sourceCells;++cell)
			sourceBelowCase.beginningConservativeValues[component*sourceCells+cell]=
				sourceBelowCaseTuple[component];
	RISE::FireProductionFrozenSourcePacketSeal sourceBelowCaseSeal;
	RISE::FireProductionScalarEOSAcceptanceResult eosBelowCaseAccepted;
	const bool eosBelowCaseOK=RISE::EvaluateFireProductionScalarEOSAcceptanceCPU(
		eosRequestFromTuple(sourceBelowCaseTuple),eosBelowCaseAccepted,&error);
	const bool sourceBelowCaseOK=
		RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
			sourceBelowCase,sourceBelowCaseSeal,&error);
	Check(sourceLowerEnthalpyOK&&
		static_cast<double>(sourceBelowCaseTuple[8u])<sourceLowerEnergy&&
		eosBelowCaseOK&&eosBelowCaseAccepted.accepted&&
		eosBelowCaseAccepted.temperatureK[0u]==300.0f&&sourceBelowCaseOK&&
		sourceBelowCaseSeal.IsSealed(),
		"canonical source producer shares r184's inclusive lower-bound inversion semantics");
	std::uint64_t sourceWorkingSetBytes=0u,sourceAdjacentBytes=0u,sourceOversizedBytes=0u;
	RISE::FireProductionProjectionShape sourceAdjacentShape=sourceRequest.shape;
	++sourceAdjacentShape.nz;
	RISE::FireProductionProjectionShape sourceOversizedShape;
	sourceOversizedShape.nx=1024u;sourceOversizedShape.ny=1024u;
	sourceOversizedShape.nz=1024u;sourceOversizedShape.cellWidthM=0.01f;
	RISE::FireProductionFrozenMethaneSourceRequest sourceOversized=sourceRequest;
	sourceOversized.shape=sourceOversizedShape;
	sourceOversized.beginningConservativeValues.clear();
	sourceOversized.pilotCommandMask.clear();sourceOversized.mixingTimeS.clear();
	const bool sourceWorkingSetOK=
		RISE::FireSim::FireProductionCanonicalSourceAuthority::WorkingSetBytes(
			sourceRequest.shape,sourceRequest.workerCount,sourceWorkingSetBytes,&error);
	const bool sourceAdjacentQuery=
		RISE::FireSim::FireProductionCanonicalSourceAuthority::WorkingSetBytes(
			sourceAdjacentShape,sourceRequest.workerCount,sourceAdjacentBytes,&error);
	const bool sourceOversizedQuery=
		RISE::FireSim::FireProductionCanonicalSourceAuthority::WorkingSetBytes(
			sourceOversized.shape,sourceOversized.workerCount,sourceOversizedBytes,&error);
	const std::uint64_t sourceBytesPerCell=29u*sizeof(float)+12u*sizeof(double)+
		5u*sizeof(RISE::FireSim::MethaneCellState)+sizeof(RISE::FireSim::MethaneReactionStep)+
		3u*sizeof(RISE::FireSim::MethaneSourcePacket)+5u;
	const std::uint64_t sourceWorkerBytes=static_cast<std::uint64_t>(
		RISE::FireSim::FireWorkerCapacity())*(UINT64_C(8)<<20u);
	const std::uint64_t sourceExpected=UINT64_C(1048576)+sourceWorkerBytes+
		static_cast<std::uint64_t>(sourceCells)*sourceBytesPerCell;
	const std::uint64_t sourceAdjacentExpected=UINT64_C(1048576)+sourceWorkerBytes+
		static_cast<std::uint64_t>(sourceAdjacentShape.CellCount())*sourceBytesPerCell;
	RISE::FireProductionFrozenMethaneSourceRequest sourceHugeEnvelope=sourceRequest;
	sourceHugeEnvelope.caseRecordEnvelope.resize(UINT64_C(1048576)+1u,0u);
	RISE::FireProductionFrozenMethaneSourceRequest sourceTooManyWorkers=sourceRequest;
	sourceTooManyWorkers.workerCount=RISE::FireSim::FireWorkerCapacity()+1u;
	const bool sourceOversizedRejected=
		!RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
			sourceOversized,rejectedSource,&error)&&!rejectedSource.IsSealed()&&
		error.find("working set exceeds two GiB")!=std::string::npos;
	const bool sourceEnvelopeRejected=
		!RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
			sourceHugeEnvelope,rejectedSource,&error)&&!rejectedSource.IsSealed()&&
		error.find("case envelope exceeds one MiB")!=std::string::npos;
	const bool sourceWorkerCountRejected=
		!RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
			sourceTooManyWorkers,rejectedSource,&error)&&!rejectedSource.IsSealed();
	std::uint64_t sourceHighWorkerBytes=0u,sourceLowAfterHighBytes=0u;
	const bool sourceHighWorkerQuery=
		RISE::FireSim::FireProductionCanonicalSourceAuthority::WorkingSetBytes(
			sourceRequest.shape,RISE::FireSim::FireWorkerCapacity(),sourceHighWorkerBytes,&error);
	const bool sourceLowAfterHighQuery=
		RISE::FireSim::FireProductionCanonicalSourceAuthority::WorkingSetBytes(
			sourceRequest.shape,1u,sourceLowAfterHighBytes,&error);
	Check(sourceWorkingSetOK&&sourceAdjacentQuery&&sourceOversizedQuery&&
		sourceWorkingSetBytes==sourceExpected&&sourceAdjacentBytes==sourceAdjacentExpected&&
		sourceOversizedBytes>(UINT64_C(2)<<30u)&&
		sourceOversizedRejected&&sourceEnvelopeRejected&&sourceWorkerCountRejected&&
		sourceHighWorkerQuery&&sourceLowAfterHighQuery&&
		sourceHighWorkerBytes==sourceLowAfterHighBytes&&
		sourceLowAfterHighBytes==sourceWorkingSetBytes,
		"canonical source producer exactly accounts live storage and rejects oversized inputs early");

	// r188 prerequisite: the base projection target is rebuilt from one
	// authenticated source packet plus the same stage's retained f_N divergence.
	// It is opaque and deliberately has no projection-consumer overload.
	RISE::FireProductionFrozenMethaneSourceRequest baseTargetSourceRequest=sourceRequest;
	baseTargetSourceRequest.attemptIdentity=UINT64_C(0x1880000000000001);
	baseTargetSourceRequest.pilotCommandMask.assign(sourceCells,0u);
	baseTargetSourceRequest.pilotCommandMask[0u]=1u;
	std::vector<float> baseTargetTemperature(sourceCells,0.0f);
	for(std::size_t z=0u;z<4u;++z)for(std::size_t y=0u;y<4u;++y)
		for(std::size_t x=0u;x<4u;++x){
			const std::size_t cell=(z*4u+y)*4u+x;
			const std::array<float,9> tuple=eosTuple(300.0+25.0*static_cast<double>(x));
			std::array<double,9> represented={{}};
			for(std::size_t component=0u;component<9u;++component){
				baseTargetSourceRequest.beginningConservativeValues[component*sourceCells+cell]=
					tuple[component];
				represented[component]=static_cast<double>(tuple[component]);
			}
			double temperatureK=0.0,pressureRatio=0.0;
			if(eosRecord.InvertAcceptedConservativeStateByComponentOrder(represented.data(),
				represented.size(),eosRecord.TemperatureMinK(),eosRecord.TemperatureMaxK(),
				RISE::FireStateProducerPrecision::Binary32,temperatureK,pressureRatio,&error))
				baseTargetTemperature[cell]=static_cast<float>(temperatureK);
		}
	RISE::FireProductionFrozenSourcePacketSeal baseTargetSource;
	const bool baseTargetSourceOK=
		RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
			baseTargetSourceRequest,baseTargetSource,&error);
	if(baseTargetSourceOK)baseTargetTemperature=baseTargetSource.BeginningTemperatureK();
	RISE::FireProductionScalarFCTRequest baseTargetAdvective=constantScalarFCT;
	baseTargetAdvective.shape=baseTargetSourceRequest.shape;
	baseTargetAdvective.timeStepS=baseTargetSourceRequest.timeStepS;
	baseTargetAdvective.beginning=baseTargetSourceRequest.beginningConservativeValues;
	baseTargetAdvective.sourceDelta=baseTargetSource.SourceDelta();
	baseTargetAdvective.boundary.fill(RISE::FireProductionProjectionPeriodic);
	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest baseTargetPhysical=
		physicalFluxRequest;
	baseTargetPhysical.shape=baseTargetAdvective.shape;
	baseTargetPhysical.boundary=baseTargetAdvective.boundary;
	baseTargetPhysical.conservativeValues=baseTargetAdvective.beginning;
	baseTargetPhysical.temperatureK=baseTargetTemperature;
	baseTargetPhysical.diffusivityM2PerS.assign(sourceCells,0.01f);
	baseTargetPhysical.conductivityWPerMK.assign(sourceCells,0.03f);
	baseTargetPhysical.ambientTemperatureK=300.0f;
	const std::array<float,9> baseAmbientTuple=eosTuple(300.0);
	baseTargetAdvective.ambient=baseAmbientTuple;
	baseTargetPhysical.ambient=baseAmbientTuple;
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t faces=RISE::FireProductionProjectionFaceCount(
			baseTargetAdvective.shape,axis);
		baseTargetAdvective.frozenVelocityMPerS[axis].assign(faces,0.0f);
		baseTargetPhysical.frozenVelocityMPerS[axis]=
			baseTargetAdvective.frozenVelocityMPerS[axis];
	}
	for(unsigned int side=0u;side<6u;++side){
		const std::size_t count=16u;
		baseTargetAdvective.pressureOpenInflow[side].assign(count,0u);
		baseTargetPhysical.pressureOpenInflow[side]=
			baseTargetAdvective.pressureOpenInflow[side];
	}
	RISE::FireProductionScalarDivergenceTargetSeal baseTargetR0;
	const bool baseTargetR0OK=baseTargetSourceOK&&
		RISE::ComposeFireProductionBaseDivergenceTargetCPU(
			baseTargetSourceRequest.attemptIdentity,
			RISE::FireProductionScalarDivergenceTargetRole::R0Base,
			baseTargetAdvective,baseTargetPhysical,baseTargetSource,baseTargetR0,&error);
	RISE::FireProductionScalarHeunFluxStage baseTargetStage;
	const bool baseTargetStageOK=RISE::ComposeFireProductionScalarHeunFluxStageCPU(
		baseTargetSourceRequest.attemptIdentity,RISE::FireProductionScalarHeunFluxRole::R0,
		baseTargetAdvective,baseTargetPhysical,baseTargetStage,&error);
	auto targetMatchesIndependentTangent=[&](
		const RISE::FireProductionScalarFCTRequest& advective,
		const RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest& physical,
		const RISE::FireProductionScalarHeunFluxStage& stage,
		const RISE::FireProductionScalarDivergenceTargetSeal& target){
		bool exact=true;
		const std::size_t allFaces=stage.compositeFluxPair.packedFaceOffset[2]+
			RISE::FireProductionProjectionFaceCount(advective.shape,2u);
		const double inverseWidth=1.0/static_cast<double>(advective.shape.cellWidthM);
		for(std::size_t z=0u;z<4u&&exact;++z)
			for(std::size_t y=0u;y<4u&&exact;++y)
				for(std::size_t x=0u;x<4u&&exact;++x){
					const std::size_t cell=(z*4u+y)*4u+x;
					std::array<double,9> state={{}},physicalRate={{}};
					for(std::size_t component=0u;component<9u;++component)
						state[component]=advective.beginning[component*sourceCells+cell];
					for(unsigned int axis=0u;axis<3u;++axis){
						std::size_t rx=x,ry=y,rz=z;
						if(axis==0u)++rx;else if(axis==1u)++ry;else ++rz;
						const std::size_t left=stage.compositeFluxPair.packedFaceOffset[axis]+
							productionFaceIndex(advective.shape,axis,x,y,z);
						const std::size_t right=stage.compositeFluxPair.packedFaceOffset[axis]+
							productionFaceIndex(advective.shape,axis,rx,ry,rz);
						for(std::size_t component=0u;component<8u;++component)
							physicalRate[component]+=inverseWidth*(static_cast<double>(
								stage.physicalMassFluxKGPerM2S[component*allFaces+left])-
								static_cast<double>(stage.physicalMassFluxKGPerM2S[
									component*allFaces+right]));
						physicalRate[8u]+=inverseWidth*(static_cast<double>(
							stage.physicalEnergyFluxWPerM2[left])-static_cast<double>(
							stage.physicalEnergyFluxWPerM2[right]));
					}
					static const char* speciesNames[7]={
						"CH4","O2","N2","CO2","H2O","CO","C(gr)"};
					std::array<double,7> propertyDensity={{}},enthalpy={{}};
					double gasDensity=0.0,inverseMeanWeightSum=0.0,heatCapacity=0.0;
					for(std::size_t species=0u;species<7u;++species){
						propertyDensity[species]=std::max(0.0,state[1u+species]);
						double cp=0.0;
						exact=exact&&eosRecord.CpJPerKGK(speciesNames[species],
							physical.temperatureK[cell],cp,&error)&&
							eosRecord.SensibleEnthalpyJPerKG(speciesNames[species],
								physical.temperatureK[cell],enthalpy[species],&error);
						heatCapacity+=propertyDensity[species]*cp;
						if(species<6u){
							const RISE::FireThermochemistrySpecies* property=
								eosRecord.FindSpecies(speciesNames[species]);
							exact=exact&&property;
							gasDensity+=propertyDensity[species];
							inverseMeanWeightSum+=propertyDensity[species]/
								property->molecularWeightKGPerKMol;
						}
					}
					const double meanWeight=gasDensity/inverseMeanWeightSum;
					const double heatCapacityTemperature=heatCapacity*
						physical.temperatureK[cell];
					double physicalPerS=physicalRate[8u]/heatCapacityTemperature;
					for(std::size_t species=0u;species<6u;++species){
						const RISE::FireThermochemistrySpecies* property=
							eosRecord.FindSpecies(speciesNames[species]);
						physicalPerS+=(meanWeight/(gasDensity*
							property->molecularWeightKGPerKMol)-enthalpy[species]/
							heatCapacityTemperature)*physicalRate[1u+species];
					}
					physicalPerS-=enthalpy[6u]/heatCapacityTemperature*physicalRate[7u];
					exact=exact&&std::isfinite(physicalPerS);
					const float expected=static_cast<float>(physicalPerS+
						static_cast<double>(baseTargetSource.DivergenceTargetPerS()[cell]));
					exact=exact&&sameFloatBits(target.TargetPerS()[cell],expected);
				}
		return exact;
	};
	const bool baseTargetOracleExact=baseTargetR0OK&&baseTargetStageOK&&
		targetMatchesIndependentTangent(baseTargetAdvective,baseTargetPhysical,
			baseTargetStage,baseTargetR0);
	bool baseTargetIncludesPhysicalFlux=false;
	for(std::size_t cell=0u;cell<sourceCells;++cell)
		baseTargetIncludesPhysicalFlux=baseTargetIncludesPhysicalFlux||!sameFloatBits(
			baseTargetR0.TargetPerS()[cell],baseTargetSource.DivergenceTargetPerS()[cell]);
	Check(baseTargetR0OK&&baseTargetStageOK&&baseTargetOracleExact&&
		baseTargetIncludesPhysicalFlux&&baseTargetR0.IsSealed()&&
		RISE::FireProductionScalarDivergenceTargetSealMatches(baseTargetR0,&error)&&
		baseTargetR0.SourcePacketIdentity()==baseTargetSource.PacketIdentity()&&
		baseTargetR0.FluxCompositionIdentity()==baseTargetStage.compositionIdentity,
		"r188 base target adds the canonical f_N tangent to the frozen absolute source target");
	RISE::FireProductionScalarFCTRequest baseTargetR1Advective=baseTargetAdvective;
	for(float& value:baseTargetR1Advective.beginning)value*=1.001f;
	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest baseTargetR1Physical=
		baseTargetPhysical;
	baseTargetR1Physical.conservativeValues=baseTargetR1Advective.beginning;
	for(std::size_t cell=0u;cell<sourceCells;++cell){
		std::array<double,9> represented={{}};
		for(std::size_t component=0u;component<9u;++component)
			represented[component]=baseTargetR1Advective.beginning[component*sourceCells+cell];
		double temperatureK=0.0,pressureRatio=0.0;
		if(eosRecord.InvertAcceptedConservativeStateByComponentOrder(represented.data(),
			represented.size(),eosRecord.TemperatureMinK(),eosRecord.TemperatureMaxK(),
			RISE::FireStateProducerPrecision::Binary32,temperatureK,pressureRatio,&error))
			baseTargetR1Physical.temperatureK[cell]=static_cast<float>(temperatureK);
	}
	RISE::FireProductionScalarDivergenceTargetSeal baseTargetR1;
	const bool baseTargetR1OK=RISE::ComposeFireProductionBaseDivergenceTargetCPU(
		baseTargetSourceRequest.attemptIdentity,
		RISE::FireProductionScalarDivergenceTargetRole::R1Base,
		baseTargetR1Advective,baseTargetR1Physical,baseTargetSource,baseTargetR1,&error);
	RISE::FireProductionScalarHeunFluxStage baseTargetR1Stage;
	const bool baseTargetR1StageOK=RISE::ComposeFireProductionScalarHeunFluxStageCPU(
		baseTargetSourceRequest.attemptIdentity,RISE::FireProductionScalarHeunFluxRole::R1,
		baseTargetR1Advective,baseTargetR1Physical,baseTargetR1Stage,&error);
	const bool baseTargetR1OracleExact=baseTargetR1OK&&baseTargetR1StageOK&&
		targetMatchesIndependentTangent(baseTargetR1Advective,baseTargetR1Physical,
			baseTargetR1Stage,baseTargetR1);
	Check(baseTargetR1OracleExact&&baseTargetR1.TargetPerS()!=baseTargetR0.TargetPerS()&&
		baseTargetR1.TargetIdentity()!=baseTargetR0.TargetIdentity()&&
		baseTargetR1.SourcePacketIdentity()==baseTargetR0.SourcePacketIdentity(),
		"r188 R1 changes only the physical tangent while retaining the frozen source authority");
	RISE::FireProductionFrozenMethaneSourceRequest zeroSourceRequest=
		baseTargetSourceRequest;
	zeroSourceRequest.attemptIdentity=UINT64_C(0x1880000000000002);
	zeroSourceRequest.pilotCommandMask.assign(sourceCells,0u);
	for(float& value:zeroSourceRequest.beginningConservativeValues)value*=1.01f;
	RISE::FireProductionFrozenSourcePacketSeal zeroSource;
	const bool zeroSourceOK=RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
		zeroSourceRequest,zeroSource,&error);
	bool zeroSourceDeltaCanonical=zeroSourceOK;
	for(const float value:zeroSource.SourceDelta())
		zeroSourceDeltaCanonical=zeroSourceDeltaCanonical&&value==0.0f&&!std::signbit(value);
	RISE::FireProductionScalarFCTRequest zeroAdvective=baseTargetAdvective;
	zeroAdvective.timeStepS=zeroSourceRequest.timeStepS;
	zeroAdvective.beginning=zeroSourceRequest.beginningConservativeValues;
	zeroAdvective.sourceDelta=zeroSource.SourceDelta();
	for(auto& velocity:zeroAdvective.frozenVelocityMPerS)
		std::fill(velocity.begin(),velocity.end(),0.0f);
	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest zeroPhysical=
		baseTargetPhysical;
	zeroPhysical.conservativeValues=zeroAdvective.beginning;
	zeroPhysical.temperatureK=zeroSource.BeginningTemperatureK();
	zeroPhysical.diffusivityM2PerS.assign(sourceCells,0.0f);
	zeroPhysical.conductivityWPerMK.assign(sourceCells,0.0f);
	for(auto& velocity:zeroPhysical.frozenVelocityMPerS)
		std::fill(velocity.begin(),velocity.end(),0.0f);
	RISE::FireProductionScalarDivergenceTargetSeal zeroTarget;
	const bool zeroTargetOK=zeroSourceOK&&
		RISE::ComposeFireProductionBaseDivergenceTargetCPU(zeroSourceRequest.attemptIdentity,
			RISE::FireProductionScalarDivergenceTargetRole::R0Base,zeroAdvective,zeroPhysical,
			zeroSource,zeroTarget,&error);
	bool zeroSourceAbsoluteBranch=zeroTargetOK&&zeroSourceDeltaCanonical;
	bool zeroSourceNonzero=false;
	for(std::size_t cell=0u;cell<sourceCells&&zeroSourceAbsoluteBranch;++cell){
		zeroSourceAbsoluteBranch=sameFloatBits(zeroTarget.TargetPerS()[cell],
			zeroSource.DivergenceTargetPerS()[cell]);
		zeroSourceNonzero=zeroSourceNonzero||zeroTarget.TargetPerS()[cell]!=0.0f;
	}
	Check(zeroSourceAbsoluteBranch&&zeroSourceNonzero,
		"r188 zero source still carries the beginning-referenced absolute restoration branch");
	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest staleBaseTemperature=
		baseTargetPhysical;
	staleBaseTemperature.temperatureK[0u]=std::nextafter(
		staleBaseTemperature.temperatureK[0u],1000.0f);
	RISE::FireProductionScalarDivergenceTargetSeal refusedBaseTarget;
	const bool staleBaseTemperatureRejected=
		!RISE::ComposeFireProductionBaseDivergenceTargetCPU(
			baseTargetSourceRequest.attemptIdentity,
			RISE::FireProductionScalarDivergenceTargetRole::R0Base,
			baseTargetAdvective,staleBaseTemperature,baseTargetSource,refusedBaseTarget,&error)&&
		!refusedBaseTarget.IsSealed();
	RISE::FireProductionScalarFCTRequest staleBaseSource=baseTargetAdvective;
	staleBaseSource.sourceDelta[0u]=std::nextafter(staleBaseSource.sourceDelta[0u],1.0f);
	const bool staleBaseSourceRejected=
		!RISE::ComposeFireProductionBaseDivergenceTargetCPU(
			baseTargetSourceRequest.attemptIdentity,
			RISE::FireProductionScalarDivergenceTargetRole::R0Base,
			staleBaseSource,baseTargetPhysical,baseTargetSource,refusedBaseTarget,&error)&&
		!refusedBaseTarget.IsSealed();
	RISE::FireProductionScalarFCTRequest overBaseTarget;
	overBaseTarget.shape=heunPayloadUnder;
	RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest overBasePhysical;
	overBasePhysical.shape=overBaseTarget.shape;
	const bool overBaseRejected=!RISE::ComposeFireProductionBaseDivergenceTargetCPU(
		baseTargetSourceRequest.attemptIdentity,
		RISE::FireProductionScalarDivergenceTargetRole::R0Base,
		overBaseTarget,overBasePhysical,baseTargetSource,refusedBaseTarget,&error)&&
		error.find("working set exceeds two GiB")!=std::string::npos;
	std::uint64_t baseTargetPayloadBytes=0u;
	const FireProductionRoundoffTrace::TraceFloat traceTemperature(300.0f);
	const FireProductionRoundoffTrace::TraceFloat traceStaleTemperature(
		std::nextafter(300.0f,1000.0f));
	Check(staleBaseTemperatureRejected&&staleBaseSourceRejected&&overBaseRejected&&
		traceTemperature!=traceStaleTemperature&&
		RISE::QueryFireProductionBaseDivergenceTargetCPUPayloadBytes(
			baseTargetAdvective.shape,baseTargetPayloadBytes,&error)&&
		baseTargetPayloadBytes==sourceCells*sizeof(float)&&
		!std::is_aggregate<RISE::FireProductionScalarDivergenceTargetSeal>::value,
		"r188 refuses stale temperature/source parents and preflights the combined live set");

	// r189 projection-consumer prerequisite: the base target may enter only as
	// iteration zero of an authenticated chain. The r70 child requires the later
	// complete owner because only that owner can bind Q* to this exact projection.
	RISE::FireProductionScalarProjectionTargetSeal initialProjectionTarget;
	const bool initialProjectionTargetOK=
		RISE::ComposeFireProductionInitialProjectionTargetCPU(
			baseTargetR0,initialProjectionTarget,&error);
	double initialProjectionTargetMean=0.0;
	for(const float value:initialProjectionTarget.TargetPerS())
		initialProjectionTargetMean+=static_cast<double>(value);
	if(!initialProjectionTarget.TargetPerS().empty())initialProjectionTargetMean/=
		static_cast<double>(initialProjectionTarget.TargetPerS().size());
	Check(initialProjectionTargetOK&&
		std::fabs(initialProjectionTargetMean)<=std::numeric_limits<float>::epsilon()&&
		initialProjectionTarget.Boundary()==baseTargetR0.Boundary()&&
		initialProjectionTarget.BaseTargetIdentity()==baseTargetR0.TargetIdentity()&&
		RISE::FireProductionScalarProjectionTargetSealMatches(
			initialProjectionTarget,&error),
		"r189 initial projection target inherits r188 lineage and closes the periodic constant mode");

	RISE::FireProductionProjectionRequest authenticatedProjection;
	authenticatedProjection.shape=baseTargetAdvective.shape;
	authenticatedProjection.timeStepS=baseTargetAdvective.timeStepS;
	authenticatedProjection.ambientDensityKGPerM3=1.2f;
	authenticatedProjection.boundary=baseTargetAdvective.boundary;
	authenticatedProjection.gasDensityKGPerM3.assign(sourceCells,1.2f);
	authenticatedProjection.residentPhysicalOpenVCycleCount=1u;
	for(unsigned int axis=0u;axis<3u;++axis)
		authenticatedProjection.provisionalMomentumKGPerM2S[axis].assign(
			RISE::FireProductionProjectionFaceCount(authenticatedProjection.shape,axis),0.0f);
	RISE::FireProductionProjectionResult authenticatedProjectionResult,directProjectionResult;
	const bool authenticatedProjectionOK=RISE::ProjectFireProductionScalarTargetCPU(
		authenticatedProjection,initialProjectionTarget,authenticatedProjectionResult,&error);
	RISE::FireProductionProjectionRequest directProjection=authenticatedProjection;
	directProjection.divergenceTargetPerS=initialProjectionTarget.TargetPerS();
	const bool directProjectionOK=RISE::ProjectFireProductionCPU(
		directProjection,directProjectionResult,&error);
	bool projectionBitExact=authenticatedProjectionOK&&directProjectionOK&&
		authenticatedProjectionResult.pressurePa==directProjectionResult.pressurePa&&
		sameFloatBits(authenticatedProjectionResult.maximumPreProjectionResidualPerS,
			directProjectionResult.maximumPreProjectionResidualPerS)&&
		sameFloatBits(authenticatedProjectionResult.maximumPostProjectionResidualPerS,
			directProjectionResult.maximumPostProjectionResidualPerS);
	for(unsigned int axis=0u;axis<3u;++axis)projectionBitExact=projectionBitExact&&
		authenticatedProjectionResult.velocityMPerS[axis]==
			directProjectionResult.velocityMPerS[axis]&&
		authenticatedProjectionResult.momentumKGPerM2S[axis]==
			directProjectionResult.momentumKGPerM2S[axis];
	Check(projectionBitExact,
		"r189 authenticated projection consumer is bit-identical to the raw CPU oracle on matched inputs");

	RISE::FireProductionProjectionRequest staleAuthoredTarget=authenticatedProjection;
	staleAuthoredTarget.divergenceTargetPerS.assign(sourceCells,0.0f);
	RISE::FireProductionProjectionResult refusedAuthenticatedProjection;
	const bool staleAuthoredTargetRejected=
		!RISE::ProjectFireProductionScalarTargetCPU(staleAuthoredTarget,
			initialProjectionTarget,refusedAuthenticatedProjection,&error)&&
		refusedAuthenticatedProjection.pressurePa.empty();
	RISE::FireProductionScalarProjectionTargetSeal refusedProjectionTarget;
	const bool unsealedParentRejected=
		!RISE::ComposeFireProductionInitialProjectionTargetCPU(
			RISE::FireProductionScalarDivergenceTargetSeal(),refusedProjectionTarget,&error)&&
		!refusedProjectionTarget.IsSealed();
	RISE::FireProductionProjectionRequest wrongTopologyProjection=authenticatedProjection;
	wrongTopologyProjection.boundary.fill(RISE::FireProductionProjectionWall);
	const bool validDifferentTopologyRejected=
		!RISE::ProjectFireProductionScalarTargetCPU(wrongTopologyProjection,
			initialProjectionTarget,refusedAuthenticatedProjection,&error)&&
		refusedAuthenticatedProjection.pressurePa.empty();
	Check(staleAuthoredTargetRejected&&unsealedParentRejected&&
		validDifferentTopologyRejected&&
		!std::is_aggregate<RISE::FireProductionScalarProjectionTargetSeal>::value,
		"r189 capability refuses preauthored targets, valid-but-different topology, and unsealed parents");

	// r190 complete CPU owner: every mutable stage operand is produced after the
	// immediately preceding projection.  The no-source uniform witness makes the
	// expected result exact while still executing R0/R1 Picard, one fresh Heun
	// limiter, both compatible momentum evaluations, EOS gates, and R2.
	RISE::FireProductionFrozenMethaneSourceRequest ownerSourceRequest=sourceRequest;
	ownerSourceRequest.attemptIdentity=UINT64_C(0x1900000000000001);
	ownerSourceRequest.pilotCommandMask.assign(sourceCells,0u);
	RISE::FireProductionFrozenSourcePacketSeal ownerSource;
	const bool ownerSourceOK=RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
		ownerSourceRequest,ownerSource,&error);
	RISE::FireProductionProjectedHeunOwnerRequest ownerRequest;
	ownerRequest.attemptIdentity=ownerSourceRequest.attemptIdentity;
	ownerRequest.source=ownerSource;ownerRequest.caseRecordEnvelope=sourceCase.envelopeBytes;
	ownerRequest.beginningConservativeValues=ownerSourceRequest.beginningConservativeValues;
	ownerRequest.scalarContract=baseTargetAdvective;
	ownerRequest.scalarContract.shape=ownerSourceRequest.shape;
	ownerRequest.scalarContract.timeStepS=ownerSourceRequest.timeStepS;
	ownerRequest.scalarContract.beginning.clear();ownerRequest.scalarContract.sourceDelta.clear();
	for(auto& axis:ownerRequest.scalarContract.frozenVelocityMPerS)axis.clear();
	for(auto& side:ownerRequest.scalarContract.pressureOpenInflow)side.clear();
	ownerRequest.physicalContract=baseTargetPhysical;
	ownerRequest.physicalContract.shape=ownerSourceRequest.shape;
	ownerRequest.physicalContract.conservativeValues.clear();
	ownerRequest.physicalContract.temperatureK.clear();
	ownerRequest.physicalContract.diffusivityM2PerS.clear();
	ownerRequest.physicalContract.conductivityWPerMK.clear();
	for(auto& axis:ownerRequest.physicalContract.frozenVelocityMPerS)axis.clear();
	for(auto& side:ownerRequest.physicalContract.pressureOpenInflow)side.clear();
	ownerRequest.forceContract.shape=ownerSourceRequest.shape;
	ownerRequest.forceContract.timeStepS=ownerSourceRequest.timeStepS;
	ownerRequest.forceContract.ambientDensityKGPerM3=1.2f;
	ownerRequest.forceContract.vremanCoefficient=0.07f;
	ownerRequest.forceContract.boundary=ownerRequest.scalarContract.boundary;
	ownerRequest.forceContract.gravityMPerS2.fill(0.0f);
	ownerRequest.projectionTolerancePerS=0.0f;
	ownerRequest.endpointVelocityToleranceMPerS=0.0f;
	ownerRequest.maximumPicardIterations=4u;
	for(unsigned int axis=0u;axis<3u;++axis)
		ownerRequest.beginningMomentumKGPerM2S[axis].assign(
			RISE::FireProductionProjectionFaceCount(ownerSourceRequest.shape,axis),0.0f);
	ConstantProjectedHeunTransport32 ownerTransport;
	RISE::FireProductionProjectedHeunCPUOwner owner;
	RISE::FireProductionProjectedHeunOwnerResult ownerResult;
	const bool ownerBegin=ownerSourceOK&&owner.Begin(ownerRequest,&error);
	const bool ownerR0=ownerBegin&&owner.SolveR0(ownerTransport,&error);
	if(!ownerR0)std::fprintf(stderr,"projected-Heun R0 detail: %s\n",error.c_str());
	const bool ownerR1=ownerR0&&owner.SolveR1(ownerTransport,&error);
	if(!ownerR1)std::fprintf(stderr,"projected-Heun R1 detail: %s\n",error.c_str());
	const bool ownerR2=ownerR1&&owner.SolveR2(ownerTransport,ownerResult,&error);
	if(!ownerR2)std::fprintf(stderr,"projected-Heun R2 detail: %s\n",error.c_str());
	bool ownerExact=ownerR2&&ownerResult.accepted&&ownerResult.OwnerIdentity()!=0u&&
		RISE::FireProductionProjectedHeunOwnerResultMatches(ownerResult,ownerSource)&&
		ownerResult.conservativeValues==ownerRequest.beginningConservativeValues&&
		ownerResult.r0.target.CorrectionIteration()>=2u&&
		ownerResult.r1.target.CorrectionIteration()>=2u&&
		ownerResult.r2.target.CorrectionIteration()==0u&&
		ownerResult.r2.acceptedIterationCount>=2u&&
		!ownerResult.r2.endpointPhysicalFlux.physicalEnergyFluxWPerM2.empty()&&
		ownerResult.r0.target.AcceptedCandidateIdentity()==
			ownerResult.r0.acceptedCandidateIdentity&&
		ownerResult.r1.target.AcceptedCandidateIdentity()==
			ownerResult.r1.acceptedCandidateIdentity&&
		ownerResult.r0.target.ParentTargetIdentity()==
			ownerResult.r0.projectionTarget.TargetIdentity()&&
		ownerResult.r1.target.ParentTargetIdentity()==
			ownerResult.r1.projectionTarget.TargetIdentity()&&
		ownerResult.r0.projectionTarget.TargetIdentity()!=ownerResult.r0.target.TargetIdentity()&&
		ownerResult.r1.projectionTarget.TargetIdentity()!=ownerResult.r1.target.TargetIdentity()&&
		ownerResult.r2.projectionTarget.TargetPerS()==ownerResult.r2.target.TargetPerS();
	for(unsigned int axis=0u;axis<3u&&ownerExact;++axis)for(const float value:
		ownerResult.velocityMPerS[axis])ownerExact=ownerExact&&value==0.0f;
	RISE::FireProductionProjectedHeunOwnerResult forgedOwnerPublication=ownerResult;
	if(!forgedOwnerPublication.conservativeValues.empty())
		forgedOwnerPublication.conservativeValues[0u]=std::nextafter(
			forgedOwnerPublication.conservativeValues[0u],
			std::numeric_limits<float>::infinity());
	const bool publicResigningRejected=
		!HasPublicOwnerIdentity<RISE::FireProductionProjectedHeunOwnerResult>::value&&
		forgedOwnerPublication.OwnerIdentity()==ownerResult.OwnerIdentity()&&
		!RISE::FireProductionProjectedHeunOwnerResultMatches(
			forgedOwnerPublication,ownerSource);
	RISEFireProductionFP64::FireProductionProjectedHeunOwnerRequest ownerRequest64;
	ownerRequest64.attemptIdentity=ownerRequest.attemptIdentity;
	ownerRequest64.source=
		RISEFireProductionFP64::FireProductionFrozenSourcePacketSeal::CalibrationImport(
			ownerSource);
	ownerRequest64.caseRecordEnvelope=ownerRequest.caseRecordEnvelope;
	ownerRequest64.beginningConservativeValues.assign(
		ownerRequest.beginningConservativeValues.begin(),
		ownerRequest.beginningConservativeValues.end());
	auto copyShape64=[](const RISE::FireProductionProjectionShape& from,
		RISEFireProductionFP64::FireProductionProjectionShape& to){
		to.nx=from.nx;to.ny=from.ny;to.nz=from.nz;to.cellWidthM=from.cellWidthM;};
	copyShape64(ownerRequest.scalarContract.shape,ownerRequest64.scalarContract.shape);
	ownerRequest64.scalarContract.timeStepS=ownerRequest.scalarContract.timeStepS;
	for(unsigned int side=0u;side<6u;++side){
		ownerRequest64.scalarContract.boundary[side]=static_cast<
			RISEFireProductionFP64::FireProductionProjectionBoundary>(
				ownerRequest.scalarContract.boundary[side]);
		ownerRequest64.physicalContract.boundary[side]=
			ownerRequest64.scalarContract.boundary[side];
		ownerRequest64.forceContract.boundary[side]=
			ownerRequest64.scalarContract.boundary[side];
	}
	for(std::size_t component=0u;component<9u;++component){
		ownerRequest64.scalarContract.ambient[component]=
			ownerRequest.scalarContract.ambient[component];
		ownerRequest64.physicalContract.ambient[component]=
			ownerRequest.physicalContract.ambient[component];
	}
	ownerRequest64.scalarContract.nullity=ownerRequest.scalarContract.nullity;
	ownerRequest64.scalarContract.nullspaceBasis.assign(
		ownerRequest.scalarContract.nullspaceBasis.begin(),
		ownerRequest.scalarContract.nullspaceBasis.end());
	ownerRequest64.scalarContract.coordinateProjector.assign(
		ownerRequest.scalarContract.coordinateProjector.begin(),
		ownerRequest.scalarContract.coordinateProjector.end());
	for(std::size_t value=0u;value<14u;++value)
		ownerRequest64.scalarContract.enthalpyBoundsJPerKG[value]=
			ownerRequest.scalarContract.enthalpyBoundsJPerKG[value];
	ownerRequest64.scalarContract.feasibilityFactor=
		ownerRequest.scalarContract.feasibilityFactor;
	ownerRequest64.scalarContract.assemblyReserveFactor=
		ownerRequest.scalarContract.assemblyReserveFactor;
	copyShape64(ownerRequest.physicalContract.shape,ownerRequest64.physicalContract.shape);
	ownerRequest64.physicalContract.ambientTemperatureK=
		ownerRequest.physicalContract.ambientTemperatureK;
	copyShape64(ownerRequest.forceContract.shape,ownerRequest64.forceContract.shape);
	ownerRequest64.forceContract.timeStepS=ownerRequest.forceContract.timeStepS;
	ownerRequest64.forceContract.ambientDensityKGPerM3=
		ownerRequest.forceContract.ambientDensityKGPerM3;
	ownerRequest64.forceContract.vremanCoefficient=ownerRequest.forceContract.vremanCoefficient;
	for(unsigned int axis=0u;axis<3u;++axis){
		ownerRequest64.forceContract.gravityMPerS2[axis]=
			ownerRequest.forceContract.gravityMPerS2[axis];
		ownerRequest64.beginningMomentumKGPerM2S[axis].assign(
			ownerRequest.beginningMomentumKGPerM2S[axis].begin(),
			ownerRequest.beginningMomentumKGPerM2S[axis].end());
	}
	ownerRequest64.projectionTolerancePerS=ownerRequest.projectionTolerancePerS;
	ownerRequest64.endpointVelocityToleranceMPerS=
		ownerRequest.endpointVelocityToleranceMPerS;
	ownerRequest64.maximumPicardIterations=ownerRequest.maximumPicardIterations;
	ConstantProjectedHeunTransport64 ownerTransport64;
	RISEFireProductionFP64::FireProductionProjectedHeunCPUOwner owner64;
	RISEFireProductionFP64::FireProductionProjectedHeunOwnerResult ownerResult64;
	const bool owner64OK=owner64.Begin(ownerRequest64,&error)&&
		owner64.SolveR0(ownerTransport64,&error)&&owner64.SolveR1(ownerTransport64,&error)&&
		owner64.SolveR2(ownerTransport64,ownerResult64,&error);
	if(!owner64OK)std::fprintf(stderr,"projected-Heun fp64 mirror detail: %s\n",error.c_str());
	double ownerOracleMaximumDifference=0.0;
	auto compareOwnerVector=[&](const std::vector<float>& fp32,
		const std::vector<double>& fp64){
		if(fp32.size()!=fp64.size()){
			ownerOracleMaximumDifference=std::numeric_limits<double>::infinity();return;}
		for(std::size_t value=0u;value<fp32.size();++value)
			ownerOracleMaximumDifference=std::max(ownerOracleMaximumDifference,std::fabs(
				static_cast<double>(fp32[value])-fp64[value]));
	};
	if(ownerR2&&owner64OK){
		compareOwnerVector(ownerResult.conservativeValues,ownerResult64.conservativeValues);
		compareOwnerVector(ownerResult.stepAveragePressurePa,ownerResult64.stepAveragePressurePa);
		compareOwnerVector(ownerResult.r0.target.TargetPerS(),ownerResult64.r0.target.TargetPerS());
		compareOwnerVector(ownerResult.r1.target.TargetPerS(),ownerResult64.r1.target.TargetPerS());
		compareOwnerVector(ownerResult.r2.target.TargetPerS(),ownerResult64.r2.target.TargetPerS());
		compareOwnerVector(ownerResult.r0.projectionTarget.TargetPerS(),
			ownerResult64.r0.projectionTarget.TargetPerS());
		compareOwnerVector(ownerResult.r1.projectionTarget.TargetPerS(),
			ownerResult64.r1.projectionTarget.TargetPerS());
		compareOwnerVector(ownerResult.r2.projectionTarget.TargetPerS(),
			ownerResult64.r2.projectionTarget.TargetPerS());
		for(unsigned int axis=0u;axis<3u;++axis){
			compareOwnerVector(ownerResult.momentumKGPerM2S[axis],
				ownerResult64.momentumKGPerM2S[axis]);
			compareOwnerVector(ownerResult.velocityMPerS[axis],ownerResult64.velocityMPerS[axis]);
			compareOwnerVector(ownerResult.r0.scalarAcceptance.sharedFaceAlpha[axis],
				ownerResult64.r0.scalarAcceptance.sharedFaceAlpha[axis]);
			compareOwnerVector(ownerResult.r1.scalarAcceptance.sharedFaceAlpha[axis],
				ownerResult64.r1.scalarAcceptance.sharedFaceAlpha[axis]);
		}
	}
	RISE::FireProductionProjectedHeunOwnerResult mutatedOwnerResult=ownerResult;
	if(!mutatedOwnerResult.stepAveragePressurePa.empty())mutatedOwnerResult.stepAveragePressurePa[0u]=
		std::nextafter(mutatedOwnerResult.stepAveragePressurePa[0u],1.0f);
	RISE::FireProductionProjectedHeunOwnerResult mutatedOwnerVelocity=ownerResult;
	if(!mutatedOwnerVelocity.r1.projection.velocityMPerS[0u].empty())
		mutatedOwnerVelocity.r1.projection.velocityMPerS[0u][0u]=std::nextafter(
			mutatedOwnerVelocity.r1.projection.velocityMPerS[0u][0u],1.0f);
	RISE::FireProductionProjectedHeunOwnerResult mutatedOwnerAlpha=ownerResult;
	if(!mutatedOwnerAlpha.r0.scalarAcceptance.sharedFaceAlpha[0u].empty())
		mutatedOwnerAlpha.r0.scalarAcceptance.sharedFaceAlpha[0u][0u]=std::nextafter(
			mutatedOwnerAlpha.r0.scalarAcceptance.sharedFaceAlpha[0u][0u],0.0f);
	RISE::FireProductionProjectedHeunOwnerResult mutatedOwnerEOS=ownerResult;
	if(!mutatedOwnerEOS.committedEOS.temperatureK.empty())
		mutatedOwnerEOS.committedEOS.temperatureK[0u]=std::nextafter(
			mutatedOwnerEOS.committedEOS.temperatureK[0u],1.0f);
	RISE::FireProductionProjectedHeunOwnerResult mutatedOwnerFluxMetadata=ownerResult;
	mutatedOwnerFluxMetadata.averagedFlux.compositeFluxPair.timeStepS=std::nextafter(
		mutatedOwnerFluxMetadata.averagedFlux.compositeFluxPair.timeStepS,1.0f);
	RISE::FireProductionProjectedHeunOwnerResult mutatedOwnerPhysicalMetadata=ownerResult;
	++mutatedOwnerPhysicalMetadata.r2.endpointPhysicalFlux.shape.nx;
	RISE::FireProductionProjectedHeunOwnerResult mutatedOwnerEOSMetadata=ownerResult;
	++mutatedOwnerEOSMetadata.committedEOS.shape.nz;
	RISE::FireProductionProjectedHeunOwnerResult mutatedOwnerProjectionTarget=ownerResult;
	mutatedOwnerProjectionTarget.r0.projectionTarget=ownerResult.r0.target;
	RISE::FireProductionFrozenMethaneSourceRequest nextOwnerSourceRequest=ownerSourceRequest;
	nextOwnerSourceRequest.attemptIdentity=ownerSourceRequest.attemptIdentity+1u;
	RISE::FireProductionFrozenSourcePacketSeal nextOwnerSource;
	const bool nextOwnerSourceOK=RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
		nextOwnerSourceRequest,nextOwnerSource,&error);
	std::uint64_t ownerWorkingSetBytes=0u;
	RISE::FireProductionProjectionShape oversizedOwnerShape;
	oversizedOwnerShape.nx=1024u;oversizedOwnerShape.ny=1024u;
	oversizedOwnerShape.nz=1024u;oversizedOwnerShape.cellWidthM=0.01f;
	std::uint64_t oversizedOwnerWorkingSetBytes=0u;
	std::vector<float> r60SignedDensityState=ownerRequest.beginningConservativeValues;
	const float replacedConstituent=r60SignedDensityState[sourceCells];
	const float negativeRoundoff=-std::numeric_limits<float>::epsilon();
	r60SignedDensityState[sourceCells]=negativeRoundoff;
	r60SignedDensityState[2u*sourceCells]+=
		replacedConstituent-negativeRoundoff;
	std::vector<float> r60SignedDensity;
	const bool r60SignedDensityOK=RISE::ComputeFireProductionGasDensityCPU(
		ownerSourceRequest.shape,r60SignedDensityState,r60SignedDensity,&error);
	float r60SignedExpected=0.0f;
	for(std::size_t component=1u;component<=6u;++component)
		r60SignedExpected+=r60SignedDensityState[component*sourceCells];
	float r60ClampedCounterfactual=0.0f;
	for(std::size_t component=1u;component<=6u;++component)
		r60ClampedCounterfactual+=std::max(0.0f,
			r60SignedDensityState[component*sourceCells]);
	const std::uint64_t ownerFaceCount=
		static_cast<std::uint64_t>(ownerSourceRequest.shape.nx+1u)*
			ownerSourceRequest.shape.ny*ownerSourceRequest.shape.nz+
		static_cast<std::uint64_t>(ownerSourceRequest.shape.nx)*
			(ownerSourceRequest.shape.ny+1u)*ownerSourceRequest.shape.nz+
		static_cast<std::uint64_t>(ownerSourceRequest.shape.nx)*
			ownerSourceRequest.shape.ny*(ownerSourceRequest.shape.nz+1u);
	const std::uint64_t expectedOwnerWorkingSetBytes=(274u*sourceCells+
		97u*ownerFaceCount)*sizeof(float);
	Check(ownerExact&&publicResigningRejected&&owner64OK&&ownerOracleMaximumDifference<=
		32.0*std::numeric_limits<float>::epsilon()&&
		!RISE::FireProductionProjectedHeunOwnerResultMatches(mutatedOwnerResult,ownerSource)&&
		!RISE::FireProductionProjectedHeunOwnerResultMatches(mutatedOwnerVelocity,ownerSource)&&
		!RISE::FireProductionProjectedHeunOwnerResultMatches(mutatedOwnerAlpha,ownerSource)&&
		!RISE::FireProductionProjectedHeunOwnerResultMatches(mutatedOwnerEOS,ownerSource)&&
		!RISE::FireProductionProjectedHeunOwnerResultMatches(
			mutatedOwnerFluxMetadata,ownerSource)&&
		!RISE::FireProductionProjectedHeunOwnerResultMatches(
			mutatedOwnerPhysicalMetadata,ownerSource)&&
		!RISE::FireProductionProjectedHeunOwnerResultMatches(
			mutatedOwnerEOSMetadata,ownerSource)&&nextOwnerSourceOK&&
		!RISE::FireProductionProjectedHeunOwnerResultMatches(
			mutatedOwnerProjectionTarget,ownerSource)&&
		!RISE::FireProductionProjectedHeunOwnerResultMatches(ownerResult,nextOwnerSource)&&
		RISE::FireProductionProjectedHeunCPUOwnerWorkingSetBytes(
			ownerSourceRequest.shape,ownerWorkingSetBytes)&&
		ownerWorkingSetBytes==expectedOwnerWorkingSetBytes&&
		RISE::FireProductionProjectedHeunCPUOwnerWorkingSetBytes(
			oversizedOwnerShape,oversizedOwnerWorkingSetBytes)&&
		oversizedOwnerWorkingSetBytes>(UINT64_C(2)<<30u)&&r60SignedDensityOK&&
		r60SignedDensity[0u]==r60SignedExpected&&
		r60SignedDensity[0u]!=r60ClampedCounterfactual,
		"r190 owner binds every R0/R1/R2 publication and rejects mutated accepted output");

	// Independent oracle differential on the complete 4^3 pressure-open owner.
	// The exact canonical source packet is reconstructed from the sealed
	// production publication; fire_simulator_core owns the fp64 schedule.
	RISE::FireProductionFrozenMethaneSourceRequest differentialSourceRequest=
		ownerSourceRequest;
	differentialSourceRequest.attemptIdentity=UINT64_C(0x1900000000000002);
	differentialSourceRequest.timeStepS=1.0e-4f;
	differentialSourceRequest.beginningTimeS=std::max(0.0,
		sourceCase.derived.flowThroughTimeS-differentialSourceRequest.timeStepS);
	differentialSourceRequest.pilotCommandMask.assign(sourceCells,1u);
	differentialSourceRequest.predictiveRadiation=false;
	const double differentialPilotTemperatureK=900.0;
	std::array<float,9> differentialPilotBeginning=eosTuple(differentialPilotTemperatureK);
	differentialPilotBeginning[8u]=std::nextafter(differentialPilotBeginning[8u],
		-std::numeric_limits<float>::infinity());
	for(std::size_t component=0u;component<9u;++component)for(std::size_t cell=0u;
		cell<sourceCells;++cell)differentialSourceRequest.beginningConservativeValues[
			component*sourceCells+cell]=differentialPilotBeginning[component];
	RISE::FireProductionFrozenSourcePacketSeal differentialSource;
	const bool differentialSourceOK=
		RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
			differentialSourceRequest,differentialSource,&error);
	RISE::FireProductionProjectedHeunOwnerRequest differentialRequest=ownerRequest;
	differentialRequest.attemptIdentity=differentialSourceRequest.attemptIdentity;
	differentialRequest.source=differentialSource;
	differentialRequest.beginningConservativeValues=
		differentialSourceRequest.beginningConservativeValues;
	differentialRequest.scalarContract.timeStepS=differentialSourceRequest.timeStepS;
	differentialRequest.forceContract.timeStepS=differentialSourceRequest.timeStepS;
	differentialRequest.scalarContract.ambient=differentialPilotBeginning;
	differentialRequest.physicalContract.ambient=differentialPilotBeginning;
	differentialRequest.physicalContract.ambientTemperatureK=
		static_cast<float>(differentialPilotTemperatureK);
	for(std::size_t species=0u;species<7u;++species){
		differentialRequest.scalarContract.enthalpyBoundsJPerKG[species]=-1.0e8f;
		differentialRequest.scalarContract.enthalpyBoundsJPerKG[7u+species]=1.0e8f;
	}
	differentialRequest.projectionTolerancePerS=5.0e-3f;
	differentialRequest.maximumPicardIterations=64u;
	differentialRequest.scalarContract.boundary.fill(
		RISE::FireProductionProjectionPressureOpen);
	differentialRequest.scalarContract.boundary[4u]=RISE::FireProductionProjectionWall;
	differentialRequest.physicalContract.boundary=differentialRequest.scalarContract.boundary;
	differentialRequest.forceContract.boundary=differentialRequest.scalarContract.boundary;
	for(unsigned int axis=0u;axis<3u;++axis)
		std::fill(differentialRequest.beginningMomentumKGPerM2S[axis].begin(),
			differentialRequest.beginningMomentumKGPerM2S[axis].end(),0.0f);
	RISE::FireSim::OpenBoundaryConfig3D differentialBoundary;
	differentialBoundary.kind.fill(RISE::FireSim::PressureOpenBoundary3D);
	differentialBoundary.kind[4u]=RISE::FireSim::AdiabaticWallBoundary3D;
	differentialBoundary.ambientDensityKGPerM3=0.0;
	for(std::size_t component=1u;component<=6u;++component)
		differentialBoundary.ambientDensityKGPerM3+=
			differentialPilotBeginning[component];
	differentialBoundary.injectedGasDensityKGPerM3=
		differentialBoundary.ambientDensityKGPerM3;
	for(std::size_t component=0u;component<9u;++component){
		differentialBoundary.ambientState[component]=differentialPilotBeginning[component];
		differentialBoundary.injectedState[component]=differentialPilotBeginning[component];
	}
	differentialBoundary.velocityToleranceMPerS=
		differentialRequest.endpointVelocityToleranceMPerS;
	differentialBoundary.pressureTolerancePa=1.0e-4;
	OracleOpenProjectedHeunTransport32 differentialTransport(
		ownerSourceRequest.shape,differentialBoundary);
	RISE::FireProductionProjectedHeunCPUOwner differentialOwner;
	RISE::FireProductionProjectedHeunOwnerResult differentialResult;
	const bool differentialProductionOK=differentialSourceOK&&
		differentialOwner.Begin(differentialRequest,&error)&&
		differentialOwner.SolveR0(differentialTransport,&error)&&
		differentialOwner.SolveR1(differentialTransport,&error)&&
		differentialOwner.SolveR2(differentialTransport,differentialResult,&error);
	if(!differentialProductionOK)std::fprintf(stderr,
		"projected-Heun independent production detail: %s\n",error.c_str());
	RISE::FireSim::PeriodicMACShape differentialShape;
	differentialShape.nx=ownerSourceRequest.shape.nx;
	differentialShape.ny=ownerSourceRequest.shape.ny;
	differentialShape.nz=ownerSourceRequest.shape.nz;
	differentialShape.cellWidthM=ownerSourceRequest.shape.cellWidthM;
	std::vector<RISE::FireSim::ConservativeVector> differentialBeginning(sourceCells);
	std::vector<RISE::FireSim::MethaneSourcePacket> differentialPackets(sourceCells);
	for(std::size_t cell=0u;cell<sourceCells;++cell){
		for(std::size_t component=0u;component<9u;++component){
			differentialBeginning[cell][component]=
				differentialRequest.beginningConservativeValues[component*sourceCells+cell];
		}
		for(std::size_t species=0u;species<7u;++species)
			differentialPackets[cell].constituentDelta[species]=
				differentialSource.SourceDelta()[(1u+species)*sourceCells+cell];
		differentialPackets[cell].sensibleEnergyDeltaJPerM3=
			differentialSource.SourceDelta()[8u*sourceCells+cell];
		differentialPackets[cell].reactedFuelKGPerM3=
			differentialSource.ReactedFuelKGPerM3()[cell];
		differentialPackets[cell].oxidizedCarbonKGPerM3=
			differentialSource.OxidizedCarbonKGPerM3()[cell];
		differentialPackets[cell].grossCarbonFormedKGPerM3=
			differentialSource.GrossCarbonFormedKGPerM3()[cell];
		differentialPackets[cell].gasHeatReleaseWPerM3=
			differentialSource.GasHeatReleaseWPerM3()[cell];
		differentialPackets[cell].sootHeatReleaseWPerM3=
			differentialSource.SootHeatReleaseWPerM3()[cell];
		differentialPackets[cell].pilotEnergyDeltaJPerM3=
			differentialSource.PilotEnergyDeltaJPerM3()[cell];
		differentialPackets[cell].pilotExpansionIntegral=
			differentialSource.PilotExpansionIntegral()[cell];
		differentialPackets[cell].radiativeCoolingWPerM3=
			differentialSource.RadiativeCoolingWPerM3()[cell];
	}
	RISE::FireSim::PeriodicMACField differentialMomentum;
	for(unsigned int axis=0u;axis<3u;++axis)
		differentialMomentum.component[axis].assign(
			differentialRequest.beginningMomentumKGPerM2S[axis].begin(),
			differentialRequest.beginningMomentumKGPerM2S[axis].end());
	RISE::FireSim::ConservativeAdvance3DConfig differentialConfig;
	differentialConfig.transport.cellWidthM=ownerSourceRequest.shape.cellWidthM;
	differentialConfig.transport.deltaTimeS=differentialSourceRequest.timeStepS;
	differentialConfig.transport.ambientTemperatureK=differentialPilotTemperatureK;
	differentialConfig.transport.adiabaticTemperatureK=
		sourceCase.derived.maximumAcceptedTemperatureK;
	differentialConfig.transport.ambientGasDensityKGPerM3=
		differentialBoundary.ambientDensityKGPerM3;
	differentialConfig.transport.producerPrecision=RISE::FireStateProducerPrecision::Binary32;
	differentialConfig.projectionTolerancePerS=differentialRequest.projectionTolerancePerS;
	differentialConfig.periodicBoundaries=false;differentialConfig.retainStageDiagnostics=true;
	differentialConfig.workerCount=1u;differentialConfig.openBoundary=differentialBoundary;
	differentialConfig.injectedTemperatureK=differentialPilotTemperatureK;
	RISE::FireSim::ConservativeAdvance3DResult differentialOracle;
	const bool differentialOracleOK=RISE::FireSim::AdvanceConservative3D(
		differentialShape,differentialBeginning,differentialMomentum,differentialPackets,
		differentialConfig,eosRecord,eosRecord,RISE::FireSimulationTransportRecord::OpenV1(),
		differentialOracle,&error);
	if(!differentialOracleOK)std::fprintf(stderr,
		"projected-Heun independent oracle detail: %s\n",error.c_str());
	RISE::FireSim::ConservativeAdvance3DConfig differentialFineConfig=differentialConfig;
	differentialFineConfig.projectionTolerancePerS=
		differentialConfig.projectionTolerancePerS/16.0;
	RISE::FireSim::ConservativeAdvance3DConfig differentialFinerConfig=differentialConfig;
	differentialFinerConfig.projectionTolerancePerS=
		differentialConfig.projectionTolerancePerS/256.0;
	RISE::FireSim::ConservativeAdvance3DResult differentialFineOracle,
		differentialFinerOracle;
	const bool differentialFineOracleOK=RISE::FireSim::AdvanceConservative3D(
		differentialShape,differentialBeginning,differentialMomentum,differentialPackets,
		differentialFineConfig,eosRecord,eosRecord,
		RISE::FireSimulationTransportRecord::OpenV1(),differentialFineOracle,&error);
	const bool differentialFinerOracleOK=RISE::FireSim::AdvanceConservative3D(
		differentialShape,differentialBeginning,differentialMomentum,differentialPackets,
		differentialFinerConfig,eosRecord,eosRecord,
		RISE::FireSimulationTransportRecord::OpenV1(),differentialFinerOracle,&error);
	if(!differentialFineOracleOK||!differentialFinerOracleOK)std::fprintf(stderr,
		"projected-Heun converged oracle reference detail: fine=%u finer=%u %s\n",
		differentialFineOracleOK?1u:0u,differentialFinerOracleOK?1u:0u,error.c_str());
	double independentOwnerMaximumDifference=0.0;
	bool independentOwnerWithinFP32Bound=true;
	const double independentFP32ForwardFactor=64.0;
	double independentStageVelocityDifference=0.0;
	double independentStageMomentumDifference=0.0;
	double independentStagePressureDifference=0.0;
	double independentStagePressureOffset=0.0;
	double independentStagePressureAbsoluteDifference=0.0;
	double independentStagePressureReferenceBand=0.0;
	double independentStagePressureReferenceContraction=0.0;
	double independentStagePressureReferenceTail=0.0;
	double independentStagePressureConvergedDifference=0.0;
	bool independentStagePressureCertified=true;
	bool independentSourceActive=false;
	for(const float dose:differentialSource.SourceDelta())
		independentSourceActive=independentSourceActive||dose!=0.0f;
	if(differentialProductionOK&&differentialOracleOK)for(std::size_t cell=0u;
		cell<sourceCells;++cell){
		for(std::size_t component=0u;component<9u;++component){
			const double oracleValue=differentialOracle.conservative[cell][component];
			const double difference=std::fabs(static_cast<double>(differentialResult.
				conservativeValues[component*sourceCells+cell])-oracleValue);
			independentOwnerMaximumDifference=std::max(independentOwnerMaximumDifference,difference);
			independentOwnerWithinFP32Bound=independentOwnerWithinFP32Bound&&difference<=
				independentFP32ForwardFactor*std::numeric_limits<float>::epsilon()*
					std::max(1.0,std::fabs(oracleValue));
		}
	}
	if(differentialProductionOK&&differentialOracleOK)for(unsigned int axis=0u;axis<3u;++axis)
		for(std::size_t face=0u;face<differentialResult.velocityMPerS[axis].size();++face){
			const double oracleValue=differentialOracle.velocityMPerS.component[axis][face];
			const double difference=std::fabs(static_cast<double>(
				differentialResult.velocityMPerS[axis][face])-oracleValue);
			independentOwnerMaximumDifference=std::max(independentOwnerMaximumDifference,difference);
			independentOwnerWithinFP32Bound=independentOwnerWithinFP32Bound&&difference<=
				independentFP32ForwardFactor*std::numeric_limits<float>::epsilon()*
					std::max(1.0,std::fabs(oracleValue));
		}
	if(differentialProductionOK&&differentialOracleOK&&differentialFineOracleOK&&
		differentialFinerOracleOK){
		const auto compareStage=[&](
			const RISE::FireProductionProjectedHeunCoupledStageResult& production,
			const RISE::FireSim::ConservativeStage3D& oracle,
			const RISE::FireSim::ConservativeStage3D& fineOracle,
			const RISE::FireSim::ConservativeStage3D& finerOracle){
			for(unsigned int axis=0u;axis<3u;++axis){
				for(std::size_t face=0u;face<production.projection.velocityMPerS[axis].size();++face){
					const double oracleVelocity=oracle.openProjection.velocityMPerS.component[axis][face];
					const double velocityDifference=std::fabs(static_cast<double>(production.projection.
						velocityMPerS[axis][face])-oracleVelocity);
					const double oracleMomentum=oracle.openProjection.momentumKGPerM2S.component[axis][face];
					const double momentumDifference=std::fabs(static_cast<double>(production.projection.
						momentumKGPerM2S[axis][face])-oracleMomentum);
					independentStageVelocityDifference=std::max(independentStageVelocityDifference,
						velocityDifference);
					independentStageMomentumDifference=std::max(independentStageMomentumDifference,
						momentumDifference);
					independentOwnerWithinFP32Bound=independentOwnerWithinFP32Bound&&
						velocityDifference<=independentFP32ForwardFactor*
							std::numeric_limits<float>::epsilon()*
							std::max(1.0,std::fabs(oracleVelocity))&&
						momentumDifference<=independentFP32ForwardFactor*
							std::numeric_limits<float>::epsilon()*
							std::max(1.0,std::fabs(oracleMomentum));
				}
			}
			std::vector<double> pressureDifference(production.projection.pressurePa.size(),0.0);
			double pressureOffset=0.0;
			for(std::size_t cell=0u;cell<pressureDifference.size();++cell){
				const double coarse=oracle.openProjection.stepAverageDynamicPressurePa[cell];
				const double fine=fineOracle.openProjection.stepAverageDynamicPressurePa[cell];
				const double finer=finerOracle.openProjection.stepAverageDynamicPressurePa[cell];
				const double productionPressure=static_cast<double>(
					production.projection.pressurePa[cell]);
				pressureDifference[cell]=productionPressure-coarse;
				independentStagePressureAbsoluteDifference=std::max(
					independentStagePressureAbsoluteDifference,std::fabs(pressureDifference[cell]));
				const double contraction=std::fabs(coarse-fine),tail=std::fabs(fine-finer);
				const double convergedDifference=std::fabs(productionPressure-finer);
				const double referenceBand=std::ldexp(1.0,-10);
				independentStagePressureReferenceContraction=std::max(
					independentStagePressureReferenceContraction,contraction);
				independentStagePressureReferenceTail=std::max(
					independentStagePressureReferenceTail,tail);
				independentStagePressureConvergedDifference=std::max(
					independentStagePressureConvergedDifference,convergedDifference);
				independentStagePressureReferenceBand=std::max(
					independentStagePressureReferenceBand,referenceBand);
				independentStagePressureCertified=independentStagePressureCertified&&
					convergedDifference<=referenceBand;
				pressureOffset+=pressureDifference[cell];
			}
			if(!pressureDifference.empty())pressureOffset/=pressureDifference.size();
			independentStagePressureOffset=std::max(independentStagePressureOffset,
				std::fabs(pressureOffset));
			for(const double difference:pressureDifference)
				independentStagePressureDifference=std::max(independentStagePressureDifference,
					std::fabs(difference-pressureOffset));
		};
		compareStage(differentialResult.r0,differentialOracle.r0,
			differentialFineOracle.r0,differentialFinerOracle.r0);
		compareStage(differentialResult.r1,differentialOracle.r1,
			differentialFineOracle.r1,differentialFinerOracle.r1);
		compareStage(differentialResult.r2,differentialOracle.r2,
			differentialFineOracle.r2,differentialFinerOracle.r2);
		independentOwnerMaximumDifference=std::max({independentOwnerMaximumDifference,
			independentStageVelocityDifference,independentStageMomentumDifference});
	}
	const bool independentPressureReferenceConverged=
		independentStagePressureReferenceTail<=
		0.25*independentStagePressureReferenceContraction;
	if(differentialProductionOK&&differentialOracleOK&&
		(!independentOwnerWithinFP32Bound||!independentStagePressureCertified||
		!independentPressureReferenceConverged))std::fprintf(stderr,
		"projected-Heun independent stage max: all=%.17g velocity=%.17g momentum=%.17g pressure=%.17g pressure_gradient=%.17g pressure_offset=%.17g pressure_band=%.17g converged_pressure=%.17g oracle_coarse_fine=%.17g oracle_fine_finer=%.17g\n",
		independentOwnerMaximumDifference,independentStageVelocityDifference,
		independentStageMomentumDifference,independentStagePressureAbsoluteDifference,
		independentStagePressureDifference,independentStagePressureOffset,
		independentStagePressureReferenceBand,
		independentStagePressureConvergedDifference,
		independentStagePressureReferenceContraction,independentStagePressureReferenceTail);
	Check(differentialProductionOK&&differentialOracleOK&&differentialFineOracleOK&&
		differentialFinerOracleOK&&independentSourceActive&&independentOwnerWithinFP32Bound&&
		independentStagePressureCertified&&independentPressureReferenceConverged,
		"r190 source-active pressure-open owner matches every projected stage of the independent fp64 oracle schedule");
	const auto setOwnerFailure=[](const char* value){
#if defined(_WIN32)
		return _putenv_s("RISE_FIRE_PROJECTED_HEUN_OWNER_TEST_FAILURE",value?value:"")==0;
#else
		return value?setenv("RISE_FIRE_PROJECTED_HEUN_OWNER_TEST_FAILURE",value,1)==0:
			unsetenv("RISE_FIRE_PROJECTED_HEUN_OWNER_TEST_FAILURE")==0;
#endif
	};
	const auto vectorsEmpty=[](const auto& vectors){
		for(const auto& values:vectors)if(!values.empty())return false;
		return true;
	};
	const auto shapeDefault=[](const RISE::FireProductionProjectionShape& shape){
		return shape.nx==0u&&shape.ny==0u&&shape.nz==0u&&shape.cellWidthM==0.0f;
	};
	const auto boundariesDefault=[](const auto& boundary){
		for(const auto side:boundary)if(side!=RISE::FireProductionProjectionWall)return false;
		return true;
	};
	const auto offsetsDefault=[](const std::array<std::size_t,3>& offsets){
		return offsets[0]==0u&&offsets[1]==0u&&offsets[2]==0u;
	};
	const auto projectionDefault=[&](const RISE::FireProductionProjectionResult& projection){
		return vectorsEmpty(projection.faceDensityKGPerM3)&&
			vectorsEmpty(projection.velocityMPerS)&&vectorsEmpty(projection.momentumKGPerM2S)&&
			projection.pressurePa.empty()&&vectorsEmpty(projection.pressureOpenInflow)&&
			projection.maximumPreProjectionResidualPerS==0.0f&&
			projection.maximumPostProjectionResidualPerS==0.0f&&
			projection.validationBandPerS==0.0f&&
			projection.maximumOpenComplementarityDiscrepancyMPerS==0.0f&&
			projection.removedFineRightHandSideMean==0.0f&&
			projection.executedVCycleCount==0u&&projection.executedJacobiSweepCount==0u&&
			projection.residentUploadStagingCount==0u&&
			projection.residentInterstageDeviceToHostTransferCount==0u&&
			projection.residentTerminalStagingCount==0u&&
			projection.residentCommandCommitCount==0u&&
			projection.residentProjectionInvocationCount==0u&&
			projection.residentCertifiedWorkingSetBytes==0u&&
			projection.residentActualMetalAllocationBytes==0u&&!projection.validationPassed&&
			projection.deviceElapsedMS==0.0&&projection.deviceStartTimeS==0.0&&
			projection.deviceEndTimeS==0.0;
	};
	const auto physicalFluxDefault=[&](
		const RISE::FireProductionScalarPhysicalFluxPrerequisiteResult& flux){
		return shapeDefault(flux.shape)&&boundariesDefault(flux.boundary)&&
			offsetsDefault(flux.packedFaceOffset)&&flux.physicalMassFluxKGPerM2S.empty()&&
			flux.physicalEnergyFluxWPerM2.empty()&&vectorsEmpty(flux.physicalGasFluxKGPerM2S)&&
			flux.methaneRecordId.empty()&&flux.maximumFP64ReferenceResidualKGPerM2S==0.0&&
			flux.fp64ReferenceForwardErrorBoundKGPerM2S==0.0&&
			flux.maximumConstraintResidualKGPerM2S==0.0&&
			flux.constraintForwardErrorBoundKGPerM2S==0.0&&
			!flux.fp64ReferenceIdentityVerified;
	};
	const auto scalarAcceptanceDefault=[&](const RISE::FireProductionScalarFCTResult& scalar){
		return offsetsDefault(scalar.packedFaceOffset)&&scalar.lowFlux.empty()&&
			scalar.fluxDelta.empty()&&scalar.lowState.empty()&&scalar.limiterRatio.empty()&&
			vectorsEmpty(scalar.sharedFaceAlpha)&&scalar.accepted.empty()&&
			vectorsEmpty(scalar.acceptedGasFluxKGPerM2S)&&
			scalar.maximumCommutingResidualKGPerM3==0.0f&&
			scalar.commutingIdentityScaleKGPerM3==0.0f&&
			scalar.commutingIdentityRestrictedAcceptedKGPerM3==0.0f&&
			scalar.commutingIdentityAdvancedKGPerM3==0.0f&&
			scalar.commutingIdentityComponent==0u&&scalar.commutingIdentityFace==0u&&
			!scalar.commutingIdentityAvailable;
	};
	const auto fluxPairDefault=[&](const RISE::FireProductionScalarFCTFluxPair& pair){
		return shapeDefault(pair.shape)&&pair.timeStepS==0.0f&&
			boundariesDefault(pair.boundary)&&offsetsDefault(pair.packedFaceOffset)&&
			pair.lowFlux.empty()&&pair.fluxDelta.empty();
	};
	const auto heunFluxDefault=[&](const RISE::FireProductionScalarHeunFluxStage& flux){
		return fluxPairDefault(flux.compositeFluxPair)&&flux.physicalMassFluxKGPerM2S.empty()&&
			flux.physicalEnergyFluxWPerM2.empty()&&vectorsEmpty(flux.physicalGasFluxKGPerM2S)&&
			vectorsEmpty(flux.advectiveGasLowFluxKGPerM2S)&&
			vectorsEmpty(flux.advectiveGasFluxDeltaKGPerM2S)&&flux.methaneRecordId.empty()&&
			flux.attemptIdentity==0u&&static_cast<unsigned int>(flux.role)==0u&&
			flux.stageInputIdentity==0u&&flux.sharedFCTContractIdentity==0u&&
			flux.fctRequestIdentity==0u&&flux.frozenVelocityIdentity==0u&&
			flux.parentCompositionIdentity[0]==0u&&flux.parentCompositionIdentity[1]==0u&&
			flux.compositionIdentity==0u&&flux.physicalConstraintForwardErrorBoundKGPerM2S==0.0&&
			flux.physicalGasAveragingForwardErrorBoundKGPerM2S==0.0&&
			!flux.fp64ReferenceIdentityVerified;
	};
	const auto heunSolveDefault=[&](const RISE::FireProductionScalarHeunSolveResult& solve){
		return scalarAcceptanceDefault(solve.scalar)&&solve.attemptIdentity==0u&&
			solve.averageCompositionIdentity==0u&&solve.sharedFCTContractIdentity==0u&&
			solve.parentCompositionIdentity[0]==0u&&solve.parentCompositionIdentity[1]==0u&&
			solve.alphaIdentity==0u;
	};
	const auto eosDefault=[&](const RISE::FireProductionScalarEOSAcceptanceResult& eos){
		return shapeDefault(eos.shape)&&eos.timeStepS==0.0f&&eos.attemptIdentity==0u&&
			static_cast<unsigned int>(eos.stage)==0u&&
			eos.producerPrecision==RISE::FireStateProducerPrecision::Binary32&&
			eos.methaneRecordId.empty()&&eos.caseRecordId.empty()&&
			eos.lowerTemperatureK==0.0&&eos.upperTemperatureK==0.0&&
			eos.temperatureK.empty()&&eos.maximumEOSResidual==0.0&&eos.stateDigest==0u&&
			eos.temperatureDigest==0u&&eos.acceptanceIdentity==0u&&!eos.accepted;
	};
	const auto nonpressureDefault=[&](
		const RISE::FireProductionNonpressureMomentumRHSResult& nonpressure){
		return nonpressure.eddyKinematicViscosityM2PerS.empty()&&
			nonpressure.effectiveDynamicViscosityPaS.empty()&&
			vectorsEmpty(nonpressure.buoyancyMomentumRateKGPerM2S2)&&
			vectorsEmpty(nonpressure.stressMomentumRateKGPerM2S2)&&
			vectorsEmpty(nonpressure.phaseSourceMomentumRateKGPerM2S2)&&
			vectorsEmpty(nonpressure.combinedMomentumRateKGPerM2S2);
	};
	const auto targetDefault=[&](const RISE::FireProductionScalarProjectionTargetSeal& target){
		return shapeDefault(target.Shape())&&target.TimeStepS()==0.0f&&
			target.AttemptIdentity()==0u&&static_cast<unsigned int>(target.Role())==0u&&
			boundariesDefault(target.Boundary())&&target.TargetPerS().empty()&&
			target.BaseTargetIdentity()==0u&&target.ParentTargetIdentity()==0u&&
			target.AcceptedCandidateIdentity()==0u&&target.CorrectionIteration()==0u&&
			target.TargetIdentity()==0u&&!target.IsSealed();
	};
	const auto stageDefault=[&](const RISE::FireProductionProjectedHeunCoupledStageResult& stage){
		return static_cast<unsigned int>(stage.stage)==0u&&
			projectionDefault(stage.projection)&&heunFluxDefault(stage.flux)&&
			physicalFluxDefault(stage.endpointPhysicalFlux)&&
			scalarAcceptanceDefault(stage.scalarAcceptance)&&
			nonpressureDefault(stage.nonpressure)&&targetDefault(stage.projectionTarget)&&
			targetDefault(stage.target)&&
			stage.picardResidualPerS.empty()&&stage.parentCandidateIdentity==0u&&
			stage.acceptedCandidateIdentity==0u&&stage.acceptedIterationCount==0u&&
			stage.activeSetCycleLength==0u&&stage.activeSetDifferingFaceCount==0u&&
			stage.activeSetCanonicalProjectionCount==0u&&
			stage.maximumActiveSetComplementarityDiscrepancyMPerS==0.0f&&
			stage.maximumLimiterClassDiscrepancy==0.0f&&
			!stage.activeSetDiscontinuousClass&&!stage.limiterDiscontinuousClass;
	};
	const auto ownerResultDefault=[&](const RISE::FireProductionProjectedHeunOwnerResult& value){
		return value.conservativeValues.empty()&&vectorsEmpty(value.momentumKGPerM2S)&&
			vectorsEmpty(value.velocityMPerS)&&value.stepAveragePressurePa.empty()&&
			eosDefault(value.predictorEOS)&&eosDefault(value.committedEOS)&&
			heunFluxDefault(value.averagedFlux)&&heunSolveDefault(value.heunSolve)&&
			stageDefault(value.r0)&&stageDefault(value.r1)&&stageDefault(value.r2)&&
			value.sourcePacketIdentity==0u&&value.OwnerIdentity()==0u&&!value.accepted;
	};
	const auto mutatingTransportRefusalRetries=[&](
		const RISE::FireProductionProjectedHeunStage stage,
		const ProjectedHeunContextOperand operand,const bool callbackReturnsSuccess){
		RISE::FireProductionProjectedHeunCPUOwner mutationOwner;
		if(!mutationOwner.Begin(ownerRequest,&error))return false;
		if(stage!=RISE::FireProductionProjectedHeunStage::R0&&
			!mutationOwner.SolveR0(ownerTransport,&error))return false;
		if(stage==RISE::FireProductionProjectedHeunStage::R2&&
			!mutationOwner.SolveR1(ownerTransport,&error))return false;
		MutatingProjectedHeunTransport32 mutating(stage,operand,callbackReturnsSuccess);
		RISE::FireProductionProjectedHeunOwnerResult refused=ownerResult;
		bool rejected=false;
		if(stage==RISE::FireProductionProjectedHeunStage::R0)
			rejected=!mutationOwner.SolveR0(mutating,&error);
		else if(stage==RISE::FireProductionProjectedHeunStage::R1)
			rejected=!mutationOwner.SolveR1(mutating,&error);
		else rejected=!mutationOwner.SolveR2(mutating,refused,&error)&&
			ownerResultDefault(refused);
		if(!rejected||!mutating.Mutated()||
			error.find("transport context was mutated")==std::string::npos)return false;
		RISE::FireProductionProjectedHeunOwnerResult retried;
		if(stage==RISE::FireProductionProjectedHeunStage::R0&&
			!mutationOwner.SolveR0(ownerTransport,&error))return false;
		if(stage!=RISE::FireProductionProjectedHeunStage::R2&&
			!mutationOwner.SolveR1(ownerTransport,&error))return false;
		if(!mutationOwner.SolveR2(ownerTransport,retried,&error))return false;
		return RISE::FireProductionProjectedHeunOwnerResultMatches(retried,ownerSource)&&
			retried.OwnerIdentity()==ownerResult.OwnerIdentity();
	};
	bool mutatingTransportMatrix=true;
	const std::array<RISE::FireProductionProjectedHeunStage,3> mutatingStages={{
		RISE::FireProductionProjectedHeunStage::R0,
		RISE::FireProductionProjectedHeunStage::R1,
		RISE::FireProductionProjectedHeunStage::R2}};
	const std::array<ProjectedHeunContextOperand,10> mutatingOperands={{
		ProjectedHeunContextOperand::ConservativeState,
		ProjectedHeunContextOperand::Temperature,
		ProjectedHeunContextOperand::ProjectedVelocity,
		ProjectedHeunContextOperand::Stage,
		ProjectedHeunContextOperand::AttemptIdentity,
		ProjectedHeunContextOperand::ParentCandidateIdentity,
		ProjectedHeunContextOperand::ProjectionIdentity,
		ProjectedHeunContextOperand::ConservativeStatePointer,
		ProjectedHeunContextOperand::TemperaturePointer,
		ProjectedHeunContextOperand::ProjectedVelocityPointer}};
	for(const auto stage:mutatingStages)for(const auto operand:mutatingOperands)
		for(const bool callbackReturnsSuccess:{false,true})mutatingTransportMatrix=
			mutatingTransportMatrix&&mutatingTransportRefusalRetries(
				stage,operand,callbackReturnsSuccess);

	RISE::FireProductionProjectedHeunCPUOwner outOfOrderOwner;
	RISE::FireProductionProjectedHeunOwnerResult refusedOwnerResult=ownerResult;
	const bool outOfOrderBegun=outOfOrderOwner.Begin(ownerRequest,&error);
	const bool outOfOrderR1Rejected=outOfOrderBegun&&
		!outOfOrderOwner.SolveR1(ownerTransport,&error)&&
		error=="projected-Heun R1 is out of order";
	const bool outOfOrderR2Rejected=outOfOrderR1Rejected&&
		!outOfOrderOwner.SolveR2(ownerTransport,refusedOwnerResult,&error)&&
		error=="projected-Heun R2 is out of order"&&
		ownerResultDefault(refusedOwnerResult);
	const bool outOfOrderRejected=outOfOrderR2Rejected&&
		outOfOrderOwner.SolveR0(ownerTransport,&error)&&
		outOfOrderOwner.SolveR1(ownerTransport,&error)&&
		outOfOrderOwner.SolveR2(ownerTransport,refusedOwnerResult,&error)&&
		RISE::FireProductionProjectedHeunOwnerResultMatches(
			refusedOwnerResult,ownerSource);
	ForgedProjectedHeunTransport32 staleOwnerTransport(true),forgedOwnerTransport(false);
	ReplayedProjectedHeunTransport32 replayedOwnerTransport;
	RISE::FireProductionProjectedHeunCPUOwner staleOwner,forgedOwner,replayedOwner;
	const bool staleCandidateRejectedCore=staleOwner.Begin(ownerRequest,&error)&&
		!staleOwner.SolveR0(staleOwnerTransport,&error)&&
		error=="projected-Heun transport publication lineage differs";
	const bool staleCandidateRejected=staleCandidateRejectedCore&&
		staleOwner.SolveR0(ownerTransport,&error);
	const bool forgedLineageRejectedCore=forgedOwner.Begin(ownerRequest,&error)&&
		!forgedOwner.SolveR0(forgedOwnerTransport,&error)&&
		error=="projected-Heun transport publication lineage differs";
	const bool forgedLineageRejected=forgedLineageRejectedCore&&
		forgedOwner.SolveR0(ownerTransport,&error);
	const bool replayedPayloadRejectedCore=replayedOwner.Begin(ownerRequest,&error)&&
		!replayedOwner.SolveR0(replayedOwnerTransport,&error)&&
		error=="projected-Heun transport publication payload differs";
	const bool replayedPayloadRejected=replayedPayloadRejectedCore&&
		replayedOwner.SolveR0(ownerTransport,&error);
	const auto r2TransportRefusalRetries=[&](
		auto& refusedTransport,const char* expectedError){
		RISE::FireProductionProjectedHeunCPUOwner refusedOwner;
		RISE::FireProductionProjectedHeunOwnerResult refusedResult=ownerResult;
		const bool prepared=refusedOwner.Begin(ownerRequest,&error)&&
			refusedOwner.SolveR0(ownerTransport,&error)&&
			refusedOwner.SolveR1(ownerTransport,&error);
		const bool rejected=prepared&&
			!refusedOwner.SolveR2(refusedTransport,refusedResult,&error)&&
			error==expectedError&&ownerResultDefault(refusedResult);
		return rejected&&refusedOwner.SolveR2(ownerTransport,refusedResult,&error)&&
			RISE::FireProductionProjectedHeunOwnerResultMatches(
				refusedResult,ownerSource);
	};
	ForgedProjectedHeunTransport32 staleR2Transport(true),forgedR2Transport(false);
	ReplayedProjectedHeunTransport32 replayedR2Transport;
	const bool staleR2CandidateRejected=r2TransportRefusalRetries(
		staleR2Transport,"projected-Heun R2 transport lineage differs");
	const bool forgedR2LineageRejected=r2TransportRefusalRetries(
		forgedR2Transport,"projected-Heun R2 transport lineage differs");
	const bool replayedR2PayloadRejected=r2TransportRefusalRetries(
		replayedR2Transport,"projected-Heun R2 transport payload differs");
	RISE::FireProductionProjectedHeunCPUOwner reentrantOwner;
	RISE::FireProductionProjectedHeunOwnerResult reentrantResult;
	ReentrantProjectedHeunTransport32 reentrantTransport(reentrantOwner,ownerResult);
	const bool reentrantPrepared=reentrantOwner.Begin(ownerRequest,&error);
	const bool reentrantR0=reentrantPrepared&&
		reentrantOwner.SolveR0(reentrantTransport,&error);
	const bool reentrantRefused=reentrantR0&&reentrantTransport.NestedStagesRejected()&&
		ownerResultDefault(reentrantTransport.NestedResult());
	const bool reentrantRetryable=reentrantRefused&&
		reentrantOwner.SolveR1(ownerTransport,&error)&&
		reentrantOwner.SolveR2(ownerTransport,reentrantResult,&error)&&
		RISE::FireProductionProjectedHeunOwnerResultMatches(reentrantResult,ownerSource);
	const auto r0ProjectionValidationRejected=[&](const char* failure){
		RISE::FireProductionProjectedHeunCPUOwner validationOwner;
		const bool injected=setOwnerFailure(failure);
		const bool rejected=injected&&validationOwner.Begin(ownerRequest,&error)&&
			!validationOwner.SolveR0(ownerTransport,&error)&&
			error.find("projection validation")!=std::string::npos;
		return setOwnerFailure(0)&&rejected;
	};
	const bool initialValidationRejected=
		r0ProjectionValidationRejected("projection-validation-initial");
	const bool iterativeValidationRejected=
		r0ProjectionValidationRejected("projection-validation-iterative");
	const bool terminalValidationRejected=
		r0ProjectionValidationRejected("projection-validation-terminal");
	RISE::FireProductionProjectedHeunCPUOwner r2ValidationOwner;
	RISE::FireProductionProjectedHeunOwnerResult r2ValidationResult;
	const bool r2ValidationPrepared=r2ValidationOwner.Begin(ownerRequest,&error)&&
		r2ValidationOwner.SolveR0(ownerTransport,&error)&&
		r2ValidationOwner.SolveR1(ownerTransport,&error);
	const bool r2ValidationInjected=setOwnerFailure("projection-validation-r2-endpoint");
	const bool r2ValidationRejectedCore=r2ValidationPrepared&&r2ValidationInjected&&
		!r2ValidationOwner.SolveR2(ownerTransport,r2ValidationResult,&error)&&
		error.find("R2 projection validation")!=std::string::npos;
	const bool r2ValidationRejected=setOwnerFailure(0)&&r2ValidationRejectedCore;
	RISE::FireProductionProjectedHeunCPUOwner r2TerminalValidationOwner;
	RISE::FireProductionProjectedHeunOwnerResult r2TerminalValidationResult;
	const bool r2TerminalValidationPrepared=
		r2TerminalValidationOwner.Begin(ownerRequest,&error)&&
		r2TerminalValidationOwner.SolveR0(ownerTransport,&error)&&
		r2TerminalValidationOwner.SolveR1(ownerTransport,&error);
	const bool r2TerminalValidationInjected=setOwnerFailure(
		"projection-validation-r2-terminal");
	const bool r2TerminalValidationRejectedCore=r2TerminalValidationPrepared&&
		r2TerminalValidationInjected&&!r2TerminalValidationOwner.SolveR2(
			ownerTransport,r2TerminalValidationResult,&error)&&
		error.find("R2 terminal projection validation")!=std::string::npos&&
		ownerResultDefault(r2TerminalValidationResult);
	const bool r2TerminalValidationRejected=setOwnerFailure(0)&&
		r2TerminalValidationRejectedCore;
	RISE::FireProductionProjectedHeunCPUOwner r2BootstrapValidationOwner;
	RISE::FireProductionProjectedHeunOwnerResult r2BootstrapValidationResult;
	const bool r2BootstrapPrepared=r2BootstrapValidationOwner.Begin(ownerRequest,&error)&&
		r2BootstrapValidationOwner.SolveR0(ownerTransport,&error)&&
		r2BootstrapValidationOwner.SolveR1(ownerTransport,&error);
	const bool r2BootstrapInjected=setOwnerFailure("projection-validation-r2-bootstrap");
	const bool r2BootstrapRejectedCore=r2BootstrapPrepared&&r2BootstrapInjected&&
		!r2BootstrapValidationOwner.SolveR2(ownerTransport,r2BootstrapValidationResult,&error)&&
		error.find("bootstrap projection validation")!=std::string::npos;
	const bool r2BootstrapRejected=setOwnerFailure(0)&&r2BootstrapRejectedCore;
	const bool r2BootstrapRetry=r2BootstrapRejected&&
		r2BootstrapValidationOwner.SolveR2(ownerTransport,r2BootstrapValidationResult,&error)&&
		RISE::FireProductionProjectedHeunOwnerResultMatches(
			r2BootstrapValidationResult,ownerSource);
	RISE::FireProductionProjectedHeunCPUOwner workingSetOwner;
	const bool workingSetInjected=setOwnerFailure("working-set-preflight");
	const bool workingSetAdmissionRejectedCore=workingSetInjected&&
		!workingSetOwner.Begin(ownerRequest,&error)&&
		error.find("working set exceeds two GiB")!=std::string::npos;
	const bool workingSetAdmissionRejected=setOwnerFailure(0)&&
		workingSetAdmissionRejectedCore;
	RISE::FireProductionFrozenMethaneSourceRequest limiterSourceRequest=
		differentialSourceRequest;
	limiterSourceRequest.attemptIdentity=UINT64_C(0x1900000000000003);
	limiterSourceRequest.pilotCommandMask.assign(sourceCells,0u);
	for(std::size_t cell=0u;cell<sourceCells;++cell){
		const std::array<float,9> local=eosTuple(
			differentialPilotTemperatureK+0.1*static_cast<double>(cell%4u));
		for(std::size_t component=0u;component<9u;++component)
			limiterSourceRequest.beginningConservativeValues[component*sourceCells+cell]=
				local[component];
	}
	RISE::FireProductionFrozenSourcePacketSeal limiterSource;
	const bool limiterSourceOK=
		RISE::FireSim::FireProductionCanonicalSourceAuthority::Build(
			limiterSourceRequest,limiterSource,&error);
	RISE::FireProductionProjectedHeunOwnerRequest limiterRequest=differentialRequest;
	limiterRequest.attemptIdentity=limiterSourceRequest.attemptIdentity;
	limiterRequest.source=limiterSource;
	limiterRequest.beginningConservativeValues=
		limiterSourceRequest.beginningConservativeValues;
	limiterRequest.scalarContract.boundary.fill(RISE::FireProductionProjectionPeriodic);
	limiterRequest.physicalContract.boundary=limiterRequest.scalarContract.boundary;
	limiterRequest.forceContract.boundary=limiterRequest.scalarContract.boundary;
	for(unsigned int axis=0u;axis<3u;++axis)
		std::fill(limiterRequest.beginningMomentumKGPerM2S[axis].begin(),
			limiterRequest.beginningMomentumKGPerM2S[axis].end(),axis==0u?0.0002f:0.0f);
	limiterRequest.projectionTolerancePerS=5.0e-3f;
	limiterRequest.maximumPicardIterations=64u;
	RISE::FireProductionProjectedHeunCPUOwner limiterBaselineOwner;
	RISE::FireProductionProjectedHeunOwnerResult limiterBaselineResult;
	const bool limiterBaselineOK=limiterSourceOK&&
		limiterBaselineOwner.Begin(limiterRequest,&error)&&
		limiterBaselineOwner.SolveR0(ownerTransport,&error)&&
		limiterBaselineOwner.SolveR1(ownerTransport,&error)&&
		limiterBaselineOwner.SolveR2(ownerTransport,limiterBaselineResult,&error);
	RISEFireProductionFP64::FireProductionProjectedHeunOwnerRequest limiterRequest64=
		ownerRequest64;
	limiterRequest64.attemptIdentity=limiterRequest.attemptIdentity;
	limiterRequest64.source=
		RISEFireProductionFP64::FireProductionFrozenSourcePacketSeal::CalibrationImport(
			limiterSource);
	limiterRequest64.beginningConservativeValues.assign(
		limiterRequest.beginningConservativeValues.begin(),
		limiterRequest.beginningConservativeValues.end());
	limiterRequest64.scalarContract.timeStepS=limiterRequest.scalarContract.timeStepS;
	limiterRequest64.forceContract.timeStepS=limiterRequest.forceContract.timeStepS;
	for(unsigned int side=0u;side<6u;++side){
		limiterRequest64.scalarContract.boundary[side]=static_cast<
			RISEFireProductionFP64::FireProductionProjectionBoundary>(
				limiterRequest.scalarContract.boundary[side]);
		limiterRequest64.physicalContract.boundary[side]=
			limiterRequest64.scalarContract.boundary[side];
		limiterRequest64.forceContract.boundary[side]=
			limiterRequest64.scalarContract.boundary[side];
	}
	for(std::size_t component=0u;component<9u;++component){
		limiterRequest64.scalarContract.ambient[component]=
			limiterRequest.scalarContract.ambient[component];
		limiterRequest64.physicalContract.ambient[component]=
			limiterRequest.physicalContract.ambient[component];
	}
	for(std::size_t value=0u;value<14u;++value)
		limiterRequest64.scalarContract.enthalpyBoundsJPerKG[value]=
			limiterRequest.scalarContract.enthalpyBoundsJPerKG[value];
	limiterRequest64.physicalContract.ambientTemperatureK=
		limiterRequest.physicalContract.ambientTemperatureK;
	for(unsigned int axis=0u;axis<3u;++axis)
		limiterRequest64.beginningMomentumKGPerM2S[axis].assign(
			limiterRequest.beginningMomentumKGPerM2S[axis].begin(),
			limiterRequest.beginningMomentumKGPerM2S[axis].end());
	limiterRequest64.projectionTolerancePerS=limiterRequest.projectionTolerancePerS;
	limiterRequest64.endpointVelocityToleranceMPerS=
		limiterRequest.endpointVelocityToleranceMPerS;
	limiterRequest64.maximumPicardIterations=limiterRequest.maximumPicardIterations;
	RISEFireProductionFP64::FireProductionProjectedHeunCPUOwner limiterMirrorOwner;
	RISEFireProductionFP64::FireProductionProjectedHeunOwnerResult limiterMirrorResult;
	const bool limiterMirrorOK=limiterMirrorOwner.Begin(limiterRequest64,&error)&&
		limiterMirrorOwner.SolveR0(ownerTransport64,&error)&&
		limiterMirrorOwner.SolveR1(ownerTransport64,&error)&&
		limiterMirrorOwner.SolveR2(ownerTransport64,limiterMirrorResult,&error);
	RISE::FireProductionProjectedHeunOwnerRequest limiterFineRequest=limiterRequest;
	limiterFineRequest.projectionTolerancePerS=
		limiterRequest.projectionTolerancePerS/16.0f;
	RISE::FireProductionProjectedHeunCPUOwner limiterFineOwner;
	RISE::FireProductionProjectedHeunOwnerResult limiterFineResult;
	const bool limiterFineOK=limiterFineOwner.Begin(limiterFineRequest,&error)&&
		limiterFineOwner.SolveR0(ownerTransport,&error)&&
		limiterFineOwner.SolveR1(ownerTransport,&error)&&
		limiterFineOwner.SolveR2(ownerTransport,limiterFineResult,&error);
	RISEFireProductionFP64::FireProductionProjectedHeunOwnerRequest limiterFineRequest64=
		limiterRequest64;
	limiterFineRequest64.projectionTolerancePerS=
		limiterRequest64.projectionTolerancePerS/16.0;
	RISEFireProductionFP64::FireProductionProjectedHeunCPUOwner limiterFineOwner64;
	RISEFireProductionFP64::FireProductionProjectedHeunOwnerResult limiterFineResult64;
	const bool limiterFine64OK=limiterFineOwner64.Begin(limiterFineRequest64,&error)&&
		limiterFineOwner64.SolveR0(ownerTransport64,&error)&&
		limiterFineOwner64.SolveR1(ownerTransport64,&error)&&
		limiterFineOwner64.SolveR2(ownerTransport64,limiterFineResult64,&error);
	double limiterMirrorMaximumDifference=0.0;
	double limiterMirrorResidualDifference=0.0;
	double limiterMirrorFluxDifference=0.0,limiterMirrorFluxScale=1.0;
	double limiterMirrorTargetDifference=0.0,limiterProductionTargetRefinement=0.0,
		limiterFP64TargetRefinement=0.0,limiterFineTargetPrecision=0.0,
		limiterTargetScale=1.0;
	double limiterMirrorMaximumFP32=0.0,limiterMirrorMaximumFP64=0.0;
	bool limiterMirrorEndpointMetadata=true;
	const char* limiterMirrorMaximumField="none";
	auto compareLimiterMirror=[&](const char* field,const std::vector<float>& fp32,
		const std::vector<double>& fp64){
		if(fp32.size()!=fp64.size()){
			limiterMirrorMaximumDifference=std::numeric_limits<double>::infinity();return;}
		for(std::size_t value=0u;value<fp32.size();++value){
			const double difference=std::fabs(static_cast<double>(fp32[value])-
				fp64[value])/std::max(1.0,std::fabs(fp64[value]));
			if(difference>limiterMirrorMaximumDifference){
				limiterMirrorMaximumDifference=difference;
				limiterMirrorMaximumField=field;
				limiterMirrorMaximumFP32=fp32[value];limiterMirrorMaximumFP64=fp64[value];
			}
		}
	};
	auto compareLimiterFlux=[&](const std::vector<float>& fp32,
		const std::vector<double>& fp64){
		if(fp32.size()!=fp64.size()){
			limiterMirrorFluxDifference=std::numeric_limits<double>::infinity();return;}
		for(std::size_t value=0u;value<fp32.size();++value){
			limiterMirrorFluxDifference=std::max(limiterMirrorFluxDifference,
				std::fabs(static_cast<double>(fp32[value])-fp64[value]));
			limiterMirrorFluxScale=std::max(limiterMirrorFluxScale,
				std::max(std::fabs(static_cast<double>(fp32[value])),std::fabs(fp64[value])));
		}
	};
	auto compareLimiterFluxCertificate=[&](const double fp32,const double fp64){
		limiterMirrorFluxDifference=std::max(limiterMirrorFluxDifference,
			std::fabs(fp32-fp64));
		limiterMirrorFluxScale=std::max(limiterMirrorFluxScale,
			std::max(std::fabs(fp32),std::fabs(fp64)));
	};
	auto compareEndpointPhysicalFlux=[&](const auto& fp32,const auto& fp64){
		compareLimiterFlux(fp32.physicalMassFluxKGPerM2S,
			fp64.physicalMassFluxKGPerM2S);
		compareLimiterFlux(fp32.physicalEnergyFluxWPerM2,
			fp64.physicalEnergyFluxWPerM2);
		for(unsigned int axis=0u;axis<3u;++axis)
			compareLimiterFlux(fp32.physicalGasFluxKGPerM2S[axis],
				fp64.physicalGasFluxKGPerM2S[axis]);
		compareLimiterFluxCertificate(fp32.maximumFP64ReferenceResidualKGPerM2S,
			fp64.maximumFP64ReferenceResidualKGPerM2S);
		compareLimiterFluxCertificate(fp32.fp64ReferenceForwardErrorBoundKGPerM2S,
			fp64.fp64ReferenceForwardErrorBoundKGPerM2S);
		compareLimiterFluxCertificate(fp32.maximumConstraintResidualKGPerM2S,
			fp64.maximumConstraintResidualKGPerM2S);
		compareLimiterFluxCertificate(fp32.constraintForwardErrorBoundKGPerM2S,
			fp64.constraintForwardErrorBoundKGPerM2S);
		limiterMirrorEndpointMetadata=limiterMirrorEndpointMetadata&&
			fp32.shape.nx==fp64.shape.nx&&fp32.shape.ny==fp64.shape.ny&&
			fp32.shape.nz==fp64.shape.nz&&fp32.shape.cellWidthM==fp64.shape.cellWidthM&&
			fp32.packedFaceOffset==fp64.packedFaceOffset&&
			fp32.methaneRecordId==fp64.methaneRecordId&&
			fp32.fp64ReferenceIdentityVerified==fp64.fp64ReferenceIdentityVerified;
		for(unsigned int side=0u;side<6u;++side)
			limiterMirrorEndpointMetadata=limiterMirrorEndpointMetadata&&
				static_cast<unsigned int>(fp32.boundary[side])==
				static_cast<unsigned int>(fp64.boundary[side]);
	};
	auto targetDistance=[&](const auto& first,const auto& second){
		double maximum=0.0;
		if(first.size()!=second.size())return std::numeric_limits<double>::max();
		for(std::size_t value=0u;value<first.size();++value){
			maximum=std::max(maximum,std::fabs(static_cast<double>(first[value])-second[value]));
			limiterTargetScale=std::max(limiterTargetScale,std::max(
				std::fabs(static_cast<double>(first[value])),
				std::fabs(static_cast<double>(second[value]))));
		}
		return maximum;
	};
	if(limiterBaselineOK&&limiterMirrorOK){
		compareLimiterMirror("conservative",limiterBaselineResult.conservativeValues,
			limiterMirrorResult.conservativeValues);
		compareLimiterMirror("step_pressure",limiterBaselineResult.stepAveragePressurePa,
			limiterMirrorResult.stepAveragePressurePa);
		const RISE::FireProductionProjectedHeunCoupledStageResult* stages32[3]={
			&limiterBaselineResult.r0,&limiterBaselineResult.r1,&limiterBaselineResult.r2};
		const RISEFireProductionFP64::FireProductionProjectedHeunCoupledStageResult*
			stages64[3]={&limiterMirrorResult.r0,&limiterMirrorResult.r1,
				&limiterMirrorResult.r2};
		for(unsigned int stage=0u;stage<3u;++stage){
			limiterMirrorTargetDifference=std::max(limiterMirrorTargetDifference,
				targetDistance(stages32[stage]->target.TargetPerS(),
					stages64[stage]->target.TargetPerS()));
			compareLimiterMirror("stage_pressure",stages32[stage]->projection.pressurePa,
				stages64[stage]->projection.pressurePa);
			if(stages32[stage]->picardResidualPerS.size()!=
				stages64[stage]->picardResidualPerS.size())
				limiterMirrorResidualDifference=std::numeric_limits<double>::infinity();
			else for(std::size_t value=0u;
				value<stages32[stage]->picardResidualPerS.size();++value)
				limiterMirrorResidualDifference=std::max(limiterMirrorResidualDifference,
					std::fabs(static_cast<double>(stages32[stage]->picardResidualPerS[value])-
						stages64[stage]->picardResidualPerS[value]));
			for(unsigned int axis=0u;axis<3u;++axis){
				compareLimiterMirror("velocity",stages32[stage]->projection.velocityMPerS[axis],
					stages64[stage]->projection.velocityMPerS[axis]);
				compareLimiterMirror("momentum",stages32[stage]->projection.momentumKGPerM2S[axis],
					stages64[stage]->projection.momentumKGPerM2S[axis]);
				compareLimiterMirror("nonpressure",stages32[stage]->nonpressure.
					combinedMomentumRateKGPerM2S2[axis],stages64[stage]->nonpressure.
					combinedMomentumRateKGPerM2S2[axis]);
				compareLimiterMirror("alpha",stages32[stage]->scalarAcceptance.sharedFaceAlpha[axis],
					stages64[stage]->scalarAcceptance.sharedFaceAlpha[axis]);
			}
			compareLimiterFlux(stages32[stage]->flux.compositeFluxPair.lowFlux,
				stages64[stage]->flux.compositeFluxPair.lowFlux);
			compareLimiterFlux(stages32[stage]->flux.compositeFluxPair.fluxDelta,
				stages64[stage]->flux.compositeFluxPair.fluxDelta);
			compareEndpointPhysicalFlux(stages32[stage]->endpointPhysicalFlux,
				stages64[stage]->endpointPhysicalFlux);
		}
	}
	if(limiterBaselineOK&&limiterMirrorOK&&limiterFineOK&&limiterFine64OK){
		const RISE::FireProductionProjectedHeunCoupledStageResult* coarse32[3]={
			&limiterBaselineResult.r0,&limiterBaselineResult.r1,&limiterBaselineResult.r2};
		const RISE::FireProductionProjectedHeunCoupledStageResult* fine32[3]={
			&limiterFineResult.r0,&limiterFineResult.r1,&limiterFineResult.r2};
		const RISEFireProductionFP64::FireProductionProjectedHeunCoupledStageResult*
			coarse64[3]={&limiterMirrorResult.r0,&limiterMirrorResult.r1,
				&limiterMirrorResult.r2};
		const RISEFireProductionFP64::FireProductionProjectedHeunCoupledStageResult*
			fine64[3]={&limiterFineResult64.r0,&limiterFineResult64.r1,
				&limiterFineResult64.r2};
		for(unsigned int stage=0u;stage<3u;++stage){
			limiterProductionTargetRefinement=std::max(
				limiterProductionTargetRefinement,targetDistance(
					coarse32[stage]->target.TargetPerS(),fine32[stage]->target.TargetPerS()));
			limiterFP64TargetRefinement=std::max(limiterFP64TargetRefinement,
				targetDistance(coarse64[stage]->target.TargetPerS(),
					fine64[stage]->target.TargetPerS()));
			limiterFineTargetPrecision=std::max(limiterFineTargetPrecision,
				targetDistance(fine32[stage]->target.TargetPerS(),
					fine64[stage]->target.TargetPerS()));
		}
	}
	double limiterStateScale=1.0;
	for(const float value:limiterRequest.beginningConservativeValues)
		limiterStateScale=std::max(limiterStateScale,std::fabs(static_cast<double>(value)));
	const double limiterMirrorFluxBound=limiterStateScale*
		limiterRequest.scalarContract.shape.cellWidthM*
		limiterRequest.projectionTolerancePerS+128.0*
		std::numeric_limits<float>::epsilon()*limiterMirrorFluxScale;
	const double limiterTargetAdditiveBound=limiterProductionTargetRefinement+
		limiterFP64TargetRefinement+limiterFineTargetPrecision+128.0*
		std::numeric_limits<float>::epsilon()*limiterTargetScale;
	RISE::FireProductionProjectedHeunCPUOwner limiterDiscontinuousOwner;
	RISE::FireProductionProjectedHeunOwnerResult limiterDiscontinuousResult;
	const bool limiterDiscontinuousInjected=setOwnerFailure("limiter-discontinuity");
	const bool limiterDiscontinuousAcceptedCore=limiterBaselineOK&&
		limiterDiscontinuousInjected&&limiterDiscontinuousOwner.Begin(limiterRequest,&error)&&
		limiterDiscontinuousOwner.SolveR0(ownerTransport,&error)&&
		limiterDiscontinuousOwner.SolveR1(ownerTransport,&error)&&
		limiterDiscontinuousOwner.SolveR2(ownerTransport,limiterDiscontinuousResult,&error);
	if(!limiterDiscontinuousAcceptedCore)std::fprintf(stderr,
		"r190 limiter-discontinuity detail: %s\n",error.c_str());
	RISE::FireProductionScalarFCTRequest limiterCertifiedRequest=
		limiterRequest.scalarContract;
	limiterCertifiedRequest.beginning=limiterRequest.beginningConservativeValues;
	limiterCertifiedRequest.sourceDelta=limiterSource.SourceDelta();
	const bool limiterDiscontinuousAccepted=setOwnerFailure(0)&&
		limiterDiscontinuousAcceptedCore&&limiterMirrorOK&&limiterFineOK&&limiterFine64OK&&
		limiterMirrorMaximumDifference<=128.0*std::numeric_limits<float>::epsilon()&&
		limiterMirrorResidualDifference<=limiterRequest.projectionTolerancePerS&&
		limiterMirrorFluxDifference<=limiterMirrorFluxBound&&
		limiterMirrorEndpointMetadata&&
		limiterMirrorTargetDifference<=limiterTargetAdditiveBound&&
		limiterDiscontinuousResult.r1.limiterDiscontinuousClass&&
		limiterDiscontinuousResult.r1.scalarAcceptance.commutingIdentityAvailable&&
		IndependentR60AndPeriodicCommuting(limiterCertifiedRequest,
			limiterDiscontinuousResult.r1.scalarAcceptance)&&
		limiterDiscontinuousResult.r1.scalarAcceptance.sharedFaceAlpha!=
			limiterBaselineResult.r1.scalarAcceptance.sharedFaceAlpha&&
		limiterDiscontinuousResult.heunSolve.scalar.sharedFaceAlpha==
			limiterDiscontinuousResult.r1.scalarAcceptance.sharedFaceAlpha&&
		limiterDiscontinuousResult.heunSolve.scalar.accepted==
			limiterDiscontinuousResult.conservativeValues;
	if(limiterDiscontinuousAcceptedCore&&!limiterDiscontinuousAccepted)std::fprintf(stderr,
		"r190 limiter-discontinuity publication flags: class=%u commuting=%u alpha=%u state=%u r60=%u alpha_changed=%u mirror=%u mirror_max=%.17g\n",
		limiterDiscontinuousResult.r1.limiterDiscontinuousClass?1u:0u,
		limiterDiscontinuousResult.r1.scalarAcceptance.commutingIdentityAvailable?1u:0u,
		limiterDiscontinuousResult.heunSolve.scalar.sharedFaceAlpha==
			limiterDiscontinuousResult.r1.scalarAcceptance.sharedFaceAlpha?1u:0u,
		limiterDiscontinuousResult.heunSolve.scalar.accepted==
			limiterDiscontinuousResult.conservativeValues?1u:0u,
		IndependentR60AndPeriodicCommuting(limiterCertifiedRequest,
			limiterDiscontinuousResult.r1.scalarAcceptance)?1u:0u,
		limiterDiscontinuousResult.r1.scalarAcceptance.sharedFaceAlpha!=
			limiterBaselineResult.r1.scalarAcceptance.sharedFaceAlpha?1u:0u,
		limiterMirrorOK?1u:0u,limiterMirrorMaximumDifference);
	if(limiterDiscontinuousAcceptedCore&&!limiterDiscontinuousAccepted)
		std::fprintf(stderr,"r190 limiter mirror maximum field: %s residual_max=%.17g fp32=%.17g fp64=%.17g\n",
			limiterMirrorMaximumField,limiterMirrorResidualDifference,
			limiterMirrorMaximumFP32,limiterMirrorMaximumFP64);
	RISE::FireProductionProjectedHeunCPUOwner activeCycleOwner;
	RISE::FireProductionProjectedHeunOwnerResult activeCycleResult;
	const bool activeCycleInjected=setOwnerFailure("active-cycle");
	const bool activeCycleAcceptedCore=activeCycleInjected&&
		activeCycleOwner.Begin(differentialRequest,&error)&&
		activeCycleOwner.SolveR0(differentialTransport,&error)&&
		activeCycleOwner.SolveR1(differentialTransport,&error)&&
		activeCycleOwner.SolveR2(differentialTransport,activeCycleResult,&error);
	const bool activeCycleAccepted=setOwnerFailure(0)&&activeCycleAcceptedCore&&
		activeCycleResult.r0.activeSetDiscontinuousClass&&
		activeCycleResult.r0.activeSetCycleLength==2u&&
		activeCycleResult.r0.activeSetCanonicalProjectionCount==4u&&
		RISE::FireProductionProjectedHeunOwnerResultMatches(
			activeCycleResult,differentialSource);
	RISE::FireProductionProjectedHeunCPUOwner thirdClassOwner;
	RISE::FireProductionProjectedHeunOwnerResult thirdClassResult;
	const bool thirdClassInjected=setOwnerFailure(
		"active-cycle,post-freeze-third-class-r0");
	const bool thirdClassAcceptedCore=thirdClassInjected&&
		thirdClassOwner.Begin(differentialRequest,&error)&&
		thirdClassOwner.SolveR0(differentialTransport,&error)&&
		thirdClassOwner.SolveR1(differentialTransport,&error)&&
		thirdClassOwner.SolveR2(differentialTransport,thirdClassResult,&error);
	const bool thirdClassAccepted=setOwnerFailure(0)&&thirdClassAcceptedCore&&
		thirdClassResult.r0.activeSetDiscontinuousClass&&
		thirdClassResult.r0.activeSetCycleLength>=3u&&
		thirdClassResult.r0.activeSetCanonicalProjectionCount>=5u&&
		RISE::FireProductionProjectedHeunOwnerResultMatches(
			thirdClassResult,differentialSource);
	RISE::FireProductionProjectedHeunCPUOwner selectedCycleValidationOwner;
	const bool selectedCycleValidationInjected=setOwnerFailure(
		"active-cycle,projection-validation-selected-cycle");
	const bool selectedCycleValidationRejectedCore=selectedCycleValidationInjected&&
		selectedCycleValidationOwner.Begin(differentialRequest,&error)&&
		!selectedCycleValidationOwner.SolveR0(differentialTransport,&error)&&
		error.find("canonical active class validation")!=std::string::npos;
	const bool selectedCycleValidationRejected=setOwnerFailure(0)&&
		selectedCycleValidationRejectedCore;
	RISE::FireProductionProjectedHeunCPUOwner r2CycleValidationOwner;
	RISE::FireProductionProjectedHeunOwnerResult r2CycleValidationResult;
	const bool r2CycleValidationPrepared=
		r2CycleValidationOwner.Begin(differentialRequest,&error)&&
		r2CycleValidationOwner.SolveR0(differentialTransport,&error)&&
		r2CycleValidationOwner.SolveR1(differentialTransport,&error);
	const bool r2CycleValidationInjected=setOwnerFailure(
		"active-cycle-r2,projection-validation-selected-cycle");
	const bool r2CycleValidationRejectedCore=r2CycleValidationPrepared&&
		r2CycleValidationInjected&&
		!r2CycleValidationOwner.SolveR2(differentialTransport,
			r2CycleValidationResult,&error)&&
		error=="projected-Heun R2 canonical active class validation failed";
	const bool r2CycleValidationRejected=setOwnerFailure(0)&&
		r2CycleValidationRejectedCore;
	RISE::FireProductionProjectedHeunCPUOwner r2ThirdClassOwner;
	RISE::FireProductionProjectedHeunOwnerResult r2ThirdClassResult;
	const bool r2ThirdClassPrepared=r2ThirdClassOwner.Begin(differentialRequest,&error)&&
		r2ThirdClassOwner.SolveR0(differentialTransport,&error)&&
		r2ThirdClassOwner.SolveR1(differentialTransport,&error);
	const bool r2ThirdClassInjected=setOwnerFailure(
		"active-cycle-r2,post-freeze-third-class-r2");
	const bool r2ThirdClassAcceptedCore=r2ThirdClassPrepared&&r2ThirdClassInjected&&
		r2ThirdClassOwner.SolveR2(differentialTransport,r2ThirdClassResult,&error);
	const bool r2ThirdClassAccepted=setOwnerFailure(0)&&r2ThirdClassAcceptedCore&&
		r2ThirdClassResult.r2.activeSetDiscontinuousClass&&
		r2ThirdClassResult.r2.activeSetCycleLength>=3u&&
		r2ThirdClassResult.r2.activeSetCanonicalProjectionCount>=3u&&
		RISE::FireProductionProjectedHeunOwnerResultMatches(
			r2ThirdClassResult,differentialSource);
	if(!thirdClassAccepted||!r2ThirdClassAccepted)std::fprintf(stderr,
		"r190 post-freeze third-class RED: r0=%u core=%u cycle=%u count=%u r2=%u core=%u cycle=%u count=%u error=%s\n",
		thirdClassAccepted?1u:0u,thirdClassAcceptedCore?1u:0u,
		thirdClassResult.r0.activeSetCycleLength,
		thirdClassResult.r0.activeSetCanonicalProjectionCount,
		r2ThirdClassAccepted?1u:0u,r2ThirdClassAcceptedCore?1u:0u,
		r2ThirdClassResult.r2.activeSetCycleLength,
		r2ThirdClassResult.r2.activeSetCanonicalProjectionCount,error.c_str());
	if(!r2CycleValidationRejected)std::fprintf(stderr,
		"r190 R2 selected-cycle RED: prepared=%u injected=%u error=%s\n",
		r2CycleValidationPrepared?1u:0u,r2CycleValidationInjected?1u:0u,error.c_str());
	RISE::FireProductionProjectedHeunCPUOwner r2UsedClassOwner;
	RISE::FireProductionProjectedHeunOwnerResult r2UsedClassResult;
	const bool r2UsedClassPrepared=r2UsedClassOwner.Begin(differentialRequest,&error)&&
		r2UsedClassOwner.SolveR0(differentialTransport,&error)&&
		r2UsedClassOwner.SolveR1(differentialTransport,&error);
	const bool r2UsedClassInjected=setOwnerFailure(
		"active-cycle-r2,r2-used-class-discrepancy");
	const bool r2UsedClassAcceptedCore=r2UsedClassPrepared&&r2UsedClassInjected&&
		r2UsedClassOwner.SolveR2(differentialTransport,r2UsedClassResult,&error);
	const float r2KnownUsedClassDiscrepancy=
		std::nextafter(0.25f,-std::numeric_limits<float>::infinity());
	const bool r2UsedClassAccepted=setOwnerFailure(0)&&r2UsedClassAcceptedCore&&
		r2UsedClassResult.r2.activeSetDiscontinuousClass&&
		r2UsedClassResult.r2.activeSetCycleLength>=2u&&
		r2UsedClassResult.r2.maximumActiveSetComplementarityDiscrepancyMPerS>=
			r2KnownUsedClassDiscrepancy;
	if(!r2UsedClassAccepted)std::fprintf(stderr,
		"r190 R2 used-class RED: core=%u class=%u cycle=%u known=%.9g published=%.9g error=%s\n",
		r2UsedClassAcceptedCore?1u:0u,
		r2UsedClassResult.r2.activeSetDiscontinuousClass?1u:0u,
		r2UsedClassResult.r2.activeSetCycleLength,r2KnownUsedClassDiscrepancy,
		r2UsedClassResult.r2.maximumActiveSetComplementarityDiscrepancyMPerS,
		error.c_str());
	RISE::FireProductionProjectedHeunCPUOwner r2BootstrapUsedClassOwner;
	RISE::FireProductionProjectedHeunOwnerResult r2BootstrapUsedClassResult;
	const bool r2BootstrapUsedClassPrepared=
		r2BootstrapUsedClassOwner.Begin(differentialRequest,&error)&&
		r2BootstrapUsedClassOwner.SolveR0(differentialTransport,&error)&&
		r2BootstrapUsedClassOwner.SolveR1(differentialTransport,&error);
	const bool r2BootstrapUsedClassInjected=setOwnerFailure(
		"r2-bootstrap-used-class-discrepancy");
	const bool r2BootstrapUsedClassAcceptedCore=r2BootstrapUsedClassPrepared&&
		r2BootstrapUsedClassInjected&&r2BootstrapUsedClassOwner.SolveR2(
			differentialTransport,r2BootstrapUsedClassResult,&error);
	const bool r2BootstrapUsedClassAccepted=setOwnerFailure(0)&&
		r2BootstrapUsedClassAcceptedCore&&
		r2BootstrapUsedClassResult.r2.maximumActiveSetComplementarityDiscrepancyMPerS>=
			r2KnownUsedClassDiscrepancy;
	if(!r2BootstrapUsedClassAccepted)std::fprintf(stderr,
		"r190 R2 bootstrap used-class RED: core=%u known=%.9g published=%.9g error=%s\n",
		r2BootstrapUsedClassAcceptedCore?1u:0u,r2KnownUsedClassDiscrepancy,
		r2BootstrapUsedClassResult.r2.maximumActiveSetComplementarityDiscrepancyMPerS,
		error.c_str());
	RISE::FireProductionProjectedHeunCPUOwner biasedCycleOwner;
	RISE::FireProductionProjectedHeunOwnerResult biasedCycleResult;
	const bool biasedCycleInjected=setOwnerFailure(
		"active-cycle,active-cycle-selection-bias");
	const bool biasedCycleAcceptedCore=biasedCycleInjected&&
		biasedCycleOwner.Begin(differentialRequest,&error)&&
		biasedCycleOwner.SolveR0(differentialTransport,&error)&&
		biasedCycleOwner.SolveR1(differentialTransport,&error)&&
		biasedCycleOwner.SolveR2(differentialTransport,biasedCycleResult,&error);
	const bool biasedCycleAccepted=setOwnerFailure(0)&&biasedCycleAcceptedCore&&
		biasedCycleResult.r0.activeSetCanonicalProjectionCount==4u&&
		biasedCycleResult.r0.projection.pressureOpenInflow==
			differentialResult.r0.projection.pressureOpenInflow;
	if(!(activeCycleAccepted&&biasedCycleAccepted))std::fprintf(stderr,
		"r190 R0 cycle REDs: active=%u core=%u class=%u cycle=%u count=%u biased=%u core=%u count=%u\n",
		activeCycleAccepted?1u:0u,activeCycleAcceptedCore?1u:0u,
		activeCycleResult.r0.activeSetDiscontinuousClass?1u:0u,
		activeCycleResult.r0.activeSetCycleLength,
		activeCycleResult.r0.activeSetCanonicalProjectionCount,
		biasedCycleAccepted?1u:0u,biasedCycleAcceptedCore?1u:0u,
		biasedCycleResult.r0.activeSetCanonicalProjectionCount);
	const auto runDifferentialOwnerHook=[&](const char* hook,
		RISE::FireProductionProjectedHeunOwnerResult& hookResult){
		RISE::FireProductionProjectedHeunCPUOwner hookOwner;
		const bool injected=setOwnerFailure(hook);
		const bool core=injected&&hookOwner.Begin(differentialRequest,&error)&&
			hookOwner.SolveR0(differentialTransport,&error)&&
			hookOwner.SolveR1(differentialTransport,&error)&&
			hookOwner.SolveR2(differentialTransport,hookResult,&error);
		const bool reset=setOwnerFailure(0);
		return core&&reset&&RISE::FireProductionProjectedHeunOwnerResultMatches(
			hookResult,differentialSource);
	};
	RISE::FireProductionProjectedHeunOwnerResult terminalTransitionR0Result,
		terminalTransitionR1Result,terminalTransitionR2Result;
	const bool terminalTransitionR0=runDifferentialOwnerHook(
		"terminal-first-transition-r0",terminalTransitionR0Result)&&
		terminalTransitionR0Result.r0.acceptedIterationCount>
			differentialResult.r0.acceptedIterationCount&&
		!terminalTransitionR0Result.r0.activeSetDiscontinuousClass&&
		terminalTransitionR0Result.r0.activeSetCanonicalProjectionCount==0u;
	const bool terminalTransitionR1=runDifferentialOwnerHook(
		"terminal-first-transition-r1",terminalTransitionR1Result)&&
		terminalTransitionR1Result.r1.acceptedIterationCount>
			differentialResult.r1.acceptedIterationCount&&
		!terminalTransitionR1Result.r1.activeSetDiscontinuousClass&&
		terminalTransitionR1Result.r1.activeSetCanonicalProjectionCount==0u;
	const bool terminalTransitionR2=runDifferentialOwnerHook(
		"terminal-first-transition-r2",terminalTransitionR2Result)&&
		terminalTransitionR2Result.r2.acceptedIterationCount>
			differentialResult.r2.acceptedIterationCount&&
		!terminalTransitionR2Result.r2.activeSetDiscontinuousClass&&
		terminalTransitionR2Result.r2.activeSetCanonicalProjectionCount==0u;
	RISE::FireProductionProjectedHeunOwnerResult terminalWinnerR0Result,
		terminalWinnerR1Baseline,terminalWinnerR1Result,
		terminalWinnerR2Baseline,terminalWinnerR2Result;
	const bool terminalWinnerR0=runDifferentialOwnerHook(
		"active-cycle,terminal-canonical-winner-bias",terminalWinnerR0Result)&&
		terminalWinnerR0Result.r0.activeSetCanonicalProjectionCount>=2u&&
		terminalWinnerR0Result.r0.projection.pressureOpenInflow!=
			activeCycleResult.r0.projection.pressureOpenInflow;
	const bool terminalWinnerR1=runDifferentialOwnerHook(
		"active-cycle-r1",terminalWinnerR1Baseline)&&
		runDifferentialOwnerHook(
			"active-cycle-r1,terminal-canonical-winner-bias",terminalWinnerR1Result)&&
		terminalWinnerR1Result.r1.activeSetCanonicalProjectionCount>=2u&&
		terminalWinnerR1Result.r1.projection.pressureOpenInflow!=
			terminalWinnerR1Baseline.r1.projection.pressureOpenInflow;
	const bool terminalWinnerR2=runDifferentialOwnerHook(
		"active-cycle-r2",terminalWinnerR2Baseline)&&
		runDifferentialOwnerHook(
			"active-cycle-r2,terminal-canonical-winner-bias-r2",terminalWinnerR2Result)&&
		terminalWinnerR2Result.r2.activeSetCanonicalProjectionCount>=2u&&
		terminalWinnerR2Result.r2.projection.pressureOpenInflow!=
			terminalWinnerR2Baseline.r2.projection.pressureOpenInflow;
	if(!(terminalTransitionR0&&terminalTransitionR1&&terminalTransitionR2&&
		terminalWinnerR0&&terminalWinnerR1&&terminalWinnerR2))std::fprintf(stderr,
		"r190 terminal active-set REDs: transition=%u/%u/%u iterations=%u>%u,%u>%u,%u>%u class=%u/%u/%u counts=%u/%u/%u winners=%u/%u/%u counts=%u/%u/%u\n",
		terminalTransitionR0?1u:0u,terminalTransitionR1?1u:0u,
		terminalTransitionR2?1u:0u,terminalTransitionR0Result.r0.acceptedIterationCount,
		differentialResult.r0.acceptedIterationCount,
		terminalTransitionR1Result.r1.acceptedIterationCount,
		differentialResult.r1.acceptedIterationCount,
		terminalTransitionR2Result.r2.acceptedIterationCount,
		differentialResult.r2.acceptedIterationCount,
		terminalTransitionR0Result.r0.activeSetDiscontinuousClass?1u:0u,
		terminalTransitionR1Result.r1.activeSetDiscontinuousClass?1u:0u,
		terminalTransitionR2Result.r2.activeSetDiscontinuousClass?1u:0u,
		terminalTransitionR0Result.r0.activeSetCanonicalProjectionCount,
		terminalTransitionR1Result.r1.activeSetCanonicalProjectionCount,
		terminalTransitionR2Result.r2.activeSetCanonicalProjectionCount,
		terminalWinnerR0?1u:0u,terminalWinnerR1?1u:0u,terminalWinnerR2?1u:0u,
		terminalWinnerR0Result.r0.activeSetCanonicalProjectionCount,
		terminalWinnerR1Result.r1.activeSetCanonicalProjectionCount,
		terminalWinnerR2Result.r2.activeSetCanonicalProjectionCount);
	RISE::FireProductionProjectedHeunCPUOwner staleCorrectionOwner;
	RISE::FireProductionProjectedHeunOwnerResult staleCorrectionResult;
	const bool staleCorrectionInjected=setOwnerFailure("stale-candidate");
	const bool staleCorrectionRejectedCore=staleCorrectionInjected&&
		staleCorrectionOwner.Begin(ownerRequest,&error)&&
		!staleCorrectionOwner.SolveR0(ownerTransport,&error)&&
		error=="projected-Heun r70 candidate lineage is invalid";
	const bool staleCorrectionReset=setOwnerFailure(0);
	const bool staleCorrectionRejected=staleCorrectionReset&&
		staleCorrectionRejectedCore&&
		staleCorrectionOwner.SolveR0(ownerTransport,&error)&&
		staleCorrectionOwner.SolveR1(ownerTransport,&error)&&
		staleCorrectionOwner.SolveR2(ownerTransport,staleCorrectionResult,&error)&&
		RISE::FireProductionProjectedHeunOwnerResultMatches(
			staleCorrectionResult,ownerSource);
	RISE::FireProductionProjectedHeunCPUOwner outerMutationOwner;
	RISE::FireProductionProjectedHeunOwnerResult outerMutationResult;
	const bool outerMutationPrepared=outerMutationOwner.Begin(ownerRequest,&error)&&
		outerMutationOwner.SolveR0(ownerTransport,&error)&&
		outerMutationOwner.SolveR1(ownerTransport,&error);
	MutatingOuterResultProjectedHeunTransport32 outerMutationTransport(
		outerMutationResult,ownerResult);
	const bool outerMutationRejected=outerMutationPrepared&&
		!outerMutationOwner.SolveR2(outerMutationTransport,outerMutationResult,&error)&&
		outerMutationTransport.Mutated()&&ownerResultDefault(outerMutationResult);
	const bool outerMutationRetry=outerMutationRejected&&
		outerMutationOwner.SolveR2(ownerTransport,outerMutationResult,&error)&&
		RISE::FireProductionProjectedHeunOwnerResultMatches(
			outerMutationResult,ownerSource);
	RISE::FireProductionProjectedHeunCPUOwner atomicOwner;
	RISE::FireProductionProjectedHeunOwnerResult atomicResult=ownerResult;
	const bool atomicPrepared=atomicOwner.Begin(ownerRequest,&error)&&
		atomicOwner.SolveR0(ownerTransport,&error)&&atomicOwner.SolveR1(ownerTransport,&error);
	const bool atomicInjected=setOwnerFailure("result-copy-mid");
	const bool atomicFailure=atomicPrepared&&atomicInjected&&
		!atomicOwner.SolveR2(ownerTransport,atomicResult,&error)&&
		error=="projected-Heun R2 allocation failed"&&ownerResultDefault(atomicResult);
	const bool atomicReset=setOwnerFailure(0);
	const bool atomicRetry=atomicFailure&&atomicOwner.SolveR2(ownerTransport,atomicResult,&error)&&
		RISE::FireProductionProjectedHeunOwnerResultMatches(atomicResult,ownerSource);
	Check(outOfOrderRejected&&staleCandidateRejected&&forgedLineageRejected&&
		replayedPayloadRejected&&staleR2CandidateRejected&&forgedR2LineageRejected&&
		replayedR2PayloadRejected&&reentrantRetryable&&staleCorrectionRejected&&
		outerMutationRetry&&atomicReset&&atomicRetry&&mutatingTransportMatrix&&
		!std::is_aggregate<RISE::FireProductionProjectedHeunCPUOwner>::value,
		"r190 owner refuses stale payload/lineage and callback mutation, blocks reentry, preserves order, and retries atomically");
	Check(initialValidationRejected&&iterativeValidationRejected&&
		terminalValidationRejected&&r2ValidationRejected&&r2TerminalValidationRejected&&
		r2BootstrapRetry&&
		selectedCycleValidationRejected&&r2CycleValidationRejected&&
		workingSetAdmissionRejected,
		"r190 owner behaviorally refuses every projection-validation path and its live-set preflight");
	Check(limiterDiscontinuousAccepted,
		"r190 nonuniform owner revalidates the r59 selected alpha through r60 and compatible commuting identity");
	Check(activeCycleAccepted&&thirdClassAccepted&&r2ThirdClassAccepted&&
		biasedCycleAccepted&&terminalTransitionR0&&
		terminalTransitionR1&&terminalTransitionR2&&terminalWinnerR0&&
		terminalWinnerR1&&terminalWinnerR2,
		"r190 owner continues first terminal class transitions and reselects every cycle branch at the accepted target");
	Check(r2UsedClassAccepted&&r2BootstrapUsedClassAccepted,
		"r190 R2 trajectory scores bootstrap and iterative flips against the classes actually projected");
	if(failures){std::fprintf(stderr,"FireProductionCalibrationTest: %d failure(s)\n",failures);return 1;}
	std::printf("FireProductionCalibrationTest passed\n");
	return 0;
}
