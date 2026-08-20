#include "FireProductionCalibrationMath.h"
#include "FireProductionCalibrationMirror.h"
#include "Utilities/FireProductionAdvection.h"
#include "Utilities/FireProductionProjection.h"
#include "fire_production_fp64/FireProductionAdvection.h"
#include "fire_production_fp64/FireProductionProjection.h"
#include "fire_production_fp64/SourceManifest.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
	int failures=0;
	void Check(const bool condition,const char* message)
	{
		if(!condition){std::fprintf(stderr,"FAIL: %s\n",message);++failures;}
	}

	std::size_t FaceCount(const std::size_t n,const std::size_t lines)
	{
		return (n+1u)*lines;
	}
}

int main()
{
	Check(std::strlen(RISEFireProductionFP64::SourceManifest::FireProductionAdvectionSource)==64u&&
		std::strlen(RISEFireProductionFP64::SourceManifest::FireProductionProjectionSource)==64u&&
		std::strlen(RISEFireProductionFP64::SourceManifest::FireProductionTransportSource)==64u&&
		std::strlen(RISEFireProductionFP64::SourceManifest::FireProductionForceSource)==64u&&
		std::strlen(RISEFireProductionFP64::SourceManifest::Generator)==64u,
		"fp64 mirror carries source and generator SHA-256 identities");
	using namespace FireProductionCalibration;
	double radius=0.0;
	const RoundoffStage stages[]={{1.25,0x1p-22,24u},{2.0,0x1p-21,48u}};
	Check(ComposeRoundoffBound(stages,2u,radius)&&radius>
		2.0*(1.25*0.0+0x1p-22)+0x1p-21,"outward roundoff recurrence");
	const RoundoffStage invalid[]={{1.0,1.0,16777216u}};
	Check(!ComposeRoundoffBound(invalid,1u,radius)&&radius==0.0,
		"roundoff derivation rejects n*u >= 1");

	double order=0.0,distance=0.0;
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
	Check(TemporalRichardson(0.75,0.1875,2.0,order,distance)&&order==2.0&&
		distance>=1.0,"three-level temporal Richardson uses the baseline distance");
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
	FireProductionCalibration::ResidentStep64Result step64;
	Check(FireProductionCalibration::AdvanceResidentStep64(step,0.0,step64,&error)&&
		step64.force.schedule.substepCount==1u&&step64.cell.executedSubmapCount==5u&&
		step64.dual.executedSubmapCount==15u&&
		step64.projection.executedVCycleCount==12u&&
		step64.projection.maximumPostProjectionResidualPerS==0.0,
		"binary64 mirror composes force, five cell maps, fifteen dual maps, sources, and one P2");

	if(failures){std::fprintf(stderr,"FireProductionCalibrationTest: %d failure(s)\n",failures);return 1;}
	std::printf("FireProductionCalibrationTest passed\n");
	return 0;
}
