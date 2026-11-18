// Focused r239 raw-resident seam qualification, not a live-owner qualification.
#include "../src/Library/Utilities/FireProductionProjection.h"
#include <iostream>
#include <algorithm>
#include <cmath>

int main()
{
	using namespace RISE;
	@autoreleasepool {
		id<MTLDevice> device=MTLCreateSystemDefaultDevice();
		if(!device){std::cerr<<"Metal device unavailable\n";return 2;}
		id<MTLCommandQueue> queue=[device newCommandQueue];
		auto upload=[&](const void* data,std::size_t bytes)->id<MTLBuffer>{
			id<MTLBuffer> shared=[device newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared];
			id<MTLBuffer> result=[device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
			if(!shared||!result)return nil;
			id<MTLCommandBuffer> command=[queue commandBuffer];
			id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];
			[blit copyFromBuffer:shared sourceOffset:0 toBuffer:result destinationOffset:0 size:bytes];
			[blit endEncoding];[command commit];[command waitUntilCompleted];
			return [command status]==MTLCommandBufferStatusCompleted?result:nil;
		};
		int failures=0;
		auto check=[&](bool ok,const char* name){
			std::cout<<(ok?"PASS: ":"FAIL: ")<<name<<'\n';if(!ok)++failures;};
		FireProductionProjectionRequest request;
		request.shape.nx=7u;request.shape.ny=5u;request.shape.nz=4u;
		request.shape.cellWidthM=0.125f;request.timeStepS=0.001f;
		request.ambientDensityKGPerM3=1.0f;
		request.boundary.fill(FireProductionProjectionPressureOpen);
		request.boundary[4]=FireProductionProjectionWall;
		request.injectedGasDensityKGPerM3=0.5f;
		request.bottomFuelMask.assign(35u,0u);
		request.bottomFuelMassFluxKGPerM2S.assign(35u,0.0f);
		request.gasDensityKGPerM3.resize(140u);
		request.divergenceTargetPerS.assign(140u,0.0f);
		for(std::size_t cell=0u;cell<140u;++cell)
			request.gasDensityKGPerM3[cell]=0.5f+static_cast<float>(cell%13u)*0.0625f;
		for(unsigned int axis=0u;axis<3u;++axis)
			request.provisionalMomentumKGPerM2S[axis].assign(
				FireProductionProjectionFaceCount(request.shape,axis),0.0f);
		const std::size_t faces[]={0u,11u,26u,34u};
		for(std::size_t i=0u;i<4u;++i){
			const std::size_t face=faces[i];const float flux=static_cast<float>(i)*0.015625f;
			request.bottomFuelMask[face]=1u;request.bottomFuelMassFluxKGPerM2S[face]=flux;
			request.provisionalMomentumKGPerM2S[2][face]=-0.25f;
			request.divergenceTargetPerS[face]=-(flux/(0.5f*request.gasDensityKGPerM3[face]+0.25f))/0.125f;
		}
		std::vector<float> operand(request.bottomFuelMask.begin(),request.bottomFuelMask.end());
		operand.insert(operand.end(),request.bottomFuelMassFluxKGPerM2S.begin(),request.bottomFuelMassFluxKGPerM2S.end());
		FireProductionMetalProjectionResidentInput input;
		input.gasDensityKGPerM3=upload(request.gasDensityKGPerM3.data(),140u*sizeof(float));
		input.divergenceTargetPerS=upload(request.divergenceTargetPerS.data(),140u*sizeof(float));
		for(unsigned int axis=0u;axis<3u;++axis)input.provisionalMomentumKGPerM2S[axis]=
			upload(request.provisionalMomentumKGPerM2S[axis].data(),request.provisionalMomentumKGPerM2S[axis].size()*sizeof(float));
		input.bottomFuelMaskAndMassFlux=upload(operand.data(),operand.size()*sizeof(float));
		const auto original=input;
		FireProductionProjectionResult actual,reference;std::string error;
		const bool cpu=ProjectFireProductionResidentPhysicalCPU(request,reference,&error);
		const bool ok=ProjectFireProductionMetalResident(request,input,actual,&error);
		if(!ok)std::cerr<<error<<'\n';
		check(cpu&&ok&&actual.validationPassed&&actual.maximumPreProjectionResidualPerS==0.0f&&
			actual.maximumPostProjectionResidualPerS==0.0f&&actual.pressurePa==reference.pressurePa&&
			actual.velocityMPerS==reference.velocityMPerS&&actual.momentumKGPerM2S==reference.momentumKGPerM2S&&
			actual.faceDensityKGPerM3==reference.faceDensityKGPerM3,"analytic zero-pressure resident solution, all fields");
		check(ok&&actual.residentInterstageDeviceToHostTransferCount==0u&&
			actual.residentActualMetalAllocationBytes<=actual.residentCertifiedWorkingSetBytes,
			"zero interstage reads and bounded actual allocation");
		const auto canonical=actual;
		const auto hostFlux=request.bottomFuelMassFluxKGPerM2S;
		request.bottomFuelMassFluxKGPerM2S.assign(35u,0.0f);
		const bool placeholderOK=ProjectFireProductionMetalResident(request,input,actual,&error);
		check(ok&&placeholderOK&&actual.velocityMPerS==canonical.velocityMPerS&&
			actual.momentumKGPerM2S==canonical.momentumKGPerM2S,
			"host structural placeholders do not reconstruct resident flux");
		request.bottomFuelMassFluxKGPerM2S=hostFlux;
		std::vector<float> zeroOperand(operand.size(),0.0f);
		input.bottomFuelMaskAndMassFlux=upload(zeroOperand.data(),zeroOperand.size()*sizeof(float));
		const bool mutantOK=ProjectFireProductionMetalResident(request,input,actual,&error);
		check(ok&&(!mutantOK||actual.velocityMPerS!=canonical.velocityMPerS||
			actual.momentumKGPerM2S!=canonical.momentumKGPerM2S),
			"RED: removing resident inlet payload breaks the analytic solution");
		input=original;
		FireProductionProjectionResult restorationA,restorationB;
		const bool restoreA=ProjectFireProductionMetalRestorationResident(request,input,
			input.divergenceTargetPerS,restorationA,&error);
		const auto hostTarget=request.divergenceTargetPerS;
		request.divergenceTargetPerS.assign(140u,0.0f);
		const bool restoreB=ProjectFireProductionMetalRestorationResident(request,input,
			input.divergenceTargetPerS,restorationB,&error);
		float maximumTarget=0.0f;
		for(float value:hostTarget)maximumTarget=std::max(maximumTarget,std::fabs(value));
		check(restoreA&&restoreB&&restorationA.validationBandPerS==0.005f*maximumTarget&&
			restorationA.validationBandPerS==restorationB.validationBandPerS&&
			restorationA.validationPassed==restorationB.validationPassed&&
			restorationA.velocityMPerS==restorationB.velocityMPerS&&
			restorationA.momentumKGPerM2S==restorationB.momentumKGPerM2S,
			"restoration band and solution consume only the device target");
		request.divergenceTargetPerS=hostTarget;
		FireProductionMetalProjectionResidentState stateA,stateB;
		const bool stateOKA=ProjectFireProductionMetalRestorationResidentState(request,input,
			input.divergenceTargetPerS,stateA,restorationA,&error);
		request.divergenceTargetPerS.assign(140u,0.0f);
		const bool stateOKB=ProjectFireProductionMetalRestorationResidentState(request,input,
			input.divergenceTargetPerS,stateB,restorationB,&error);
		check(stateOKA&&stateOKB&&restorationA.validationBandPerS==0.005f*maximumTarget&&
			restorationA.validationBandPerS==restorationB.validationBandPerS&&
			restorationA.validationPassed==restorationB.validationPassed&&
			restorationA.maximumPostProjectionResidualPerS==restorationB.maximumPostProjectionResidualPerS&&
			restorationA.residentInterstageDeviceToHostTransferCount==0u&&
			restorationB.residentInterstageDeviceToHostTransferCount==0u,
			"state-only restoration band is device-derived without interstage reads");
		request.divergenceTargetPerS=hostTarget;
		auto refuses=[&](const char* name){
			const bool accepted=ProjectFireProductionMetalResident(request,input,actual,&error);
			check(!accepted&&actual.pressurePa.empty()&&actual.momentumKGPerM2S[2].empty(),name);
			input=original;
		};
		input.bottomFuelMaskAndMassFlux=nil;refuses("missing operand refuses");
		input.bottomFuelMaskAndMassFlux=[device newBufferWithBytes:operand.data() length:operand.size()*sizeof(float)
			options:MTLResourceStorageModeShared];refuses("Shared operand refuses");
		input.bottomFuelMaskAndMassFlux=[device newBufferWithLength:(operand.size()-1u)*sizeof(float)
			options:MTLResourceStorageModePrivate];refuses("short operand refuses");
		input.bottomFuelMaskAndMassFlux=[device newBufferWithLength:(operand.size()+1u)*sizeof(float)
			options:MTLResourceStorageModePrivate];refuses("oversized operand refuses");
		input.qualifiedOwnerStageTokens=true;refuses("owner mode without target authority refuses");
		bool testedOtherDevice=false;
		for(id<MTLDevice> other in MTLCopyAllDevices())if(other!=device){
			testedOtherDevice=true;
			input.gasDensityKGPerM3=[other newBufferWithLength:140u*sizeof(float) options:MTLResourceStorageModePrivate];
			refuses("foreign density device refuses");
			input.divergenceTargetPerS=[other newBufferWithLength:140u*sizeof(float) options:MTLResourceStorageModePrivate];
			refuses("foreign target device refuses");
			for(unsigned int axis=0u;axis<3u;++axis){
				input.provisionalMomentumKGPerM2S[axis]=[other newBufferWithLength:
					request.provisionalMomentumKGPerM2S[axis].size()*sizeof(float) options:MTLResourceStorageModePrivate];
				refuses("foreign momentum device refuses");
			}
			input.bottomFuelMaskAndMassFlux=[other newBufferWithLength:operand.size()*sizeof(float)
				options:MTLResourceStorageModePrivate];refuses("foreign burner device refuses");
		}
		if(!testedOtherDevice)std::cout<<"NOT EXERCISED: foreign-device REDs (one Metal device)\n";
		request.bottomFuelMask.clear();request.bottomFuelMassFluxKGPerM2S.clear();
		request.injectedGasDensityKGPerM3=0.0f;refuses("unexpected operand refuses");
		for(id<MTLDevice> other in MTLCopyAllDevices())if(other!=device){
			input.bottomFuelMaskAndMassFlux=nil;
			input.gasDensityKGPerM3=[other newBufferWithLength:140u*sizeof(float) options:MTLResourceStorageModePrivate];
			refuses("no-inlet foreign density device refuses");
			input.bottomFuelMaskAndMassFlux=nil;
			input.divergenceTargetPerS=[other newBufferWithLength:140u*sizeof(float) options:MTLResourceStorageModePrivate];
			refuses("no-inlet foreign target device refuses");
			for(unsigned int axis=0u;axis<3u;++axis){
				input.bottomFuelMaskAndMassFlux=nil;
				input.provisionalMomentumKGPerM2S[axis]=[other newBufferWithLength:
					request.provisionalMomentumKGPerM2S[axis].size()*sizeof(float) options:MTLResourceStorageModePrivate];
				refuses("no-inlet foreign momentum device refuses");
			}
		}
		return failures?1:0;
	}
}
