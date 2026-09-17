// r239 raw-kernel qualification: retain the physical boundary closure and
// test its scalar-balance boundary term. Not a live-owner acceptance fixture.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "fire_simulator_core.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

struct Parameters
{
	std::uint32_t nx,ny,nz,cells,components,inequalities,nullity,affineRows;
	std::uint32_t boundary[6],sideOffset[6];
	float dx,dt,feasibility,reserve;
};
static_assert(sizeof(Parameters)==96u,"production FCT parameter layout");

int main(int argc,char** argv)
{
	if(argc<2){std::cerr<<"usage: diagnose_fire_inlet_commuting <source> [--zero] [--upper] [--unequal] [--omit-boundary] [--full-endpoint]\n";return 2;}
	bool zero=false,upper=false,unequal=false,omitBoundary=false,fullEndpoint=false;
	for(int i=2;i<argc;++i){
		if(std::strcmp(argv[i],"--zero")==0)zero=true;
		else if(std::strcmp(argv[i],"--upper")==0)upper=true;
		else if(std::strcmp(argv[i],"--unequal")==0)unequal=true;
		else if(std::strcmp(argv[i],"--omit-boundary")==0)omitBoundary=true;
		else if(std::strcmp(argv[i],"--full-endpoint")==0)fullEndpoint=true;
		else return 2;}
	std::ifstream input(argv[1]);
	const std::string source((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
	const auto kernel=source.find("kernel void fct_compatible_stage_rate");
	if(!input||kernel==std::string::npos)return 3;
	const auto begin=source.rfind("R\"METAL(",kernel),end=source.find(")METAL\"",kernel);
	if(begin==std::string::npos||end==std::string::npos||end<=begin+8u)return 3;
	std::string msl=source.substr(begin+8u,end-begin-8u);
	if(omitBoundary){const std::string correction="if(boundaryMass!=0.0f)advanced+=(p.dt/p.dx)*boundaryMass;";
		const auto found=msl.find(correction);if(found==std::string::npos)return 3;
		msl.erase(found,correction.size());}
	if(fullEndpoint){const std::string half="if(boundaryFace)boundaryMass*=0.5f;";
		const auto found=msl.find(half);if(found==std::string::npos)return 3;
		msl.erase(found,half.size());}
	@autoreleasepool {
		id<MTLDevice> device=MTLCreateSystemDefaultDevice();if(!device)return 4;
		MTLCompileOptions* options=[[MTLCompileOptions alloc] init];
		if(@available(macOS 15.0,*))options.mathMode=MTLMathModeSafe;else return 4;
		NSError* error=nil;
		id<MTLLibrary> library=[device newLibraryWithSource:
			[NSString stringWithUTF8String:msl.c_str()] options:options error:&error];
		if(!library){std::cerr<<[[error localizedDescription] UTF8String]<<'\n';return 5;}
		auto pipeline=[&](const char* name){return [device newComputePipelineStateWithFunction:
			[library newFunctionWithName:[NSString stringWithUTF8String:name]] error:&error];};
		id<MTLComputePipelineState> ratePipeline=pipeline("fct_compatible_stage_rate"),
			identityPipeline=pipeline("fct_commuting_identity");
		if(!ratePipeline||!identityPipeline)return 6;
		Parameters p={4u,4u,4u,64u,9u,11u,0u,0u,
			{1u,1u,1u,1u,2u,1u},{0u,16u,32u,48u,64u,80u},0.125f,0.03125f,0.0f,0.0f};
		constexpr std::size_t faces=240u,axisFaces=80u;
		const float j=zero?0.0f:0.0625f;
		if(upper){p.boundary[4]=1u;p.boundary[5]=2u;}
		std::vector<float> low(9u*faces,0.0f),delta(low.size(),0.0f),alpha(faces,0.0f),
			velocity(axisFaces,1.0f),state(9u*p.cells,0.0f),sourceDelta(state.size(),0.0f);
		for(std::size_t cell=0u;cell<p.cells;++cell){state[cell]=1.0f;state[p.cells+cell]=1.0f;}
		auto accepted=state;
		for(std::size_t patch=0u;patch<16u;++patch){const float flux=j*(unequal?
			static_cast<float>(1u+patch%4u):1.0f);
			for(std::size_t component=0u;component<2u;++component){
				low[component*faces+2u*axisFaces+(upper?64u:0u)+patch]=flux;
				accepted[component*p.cells+(upper?48u:0u)+patch]+=(upper?-1.0f:1.0f)*p.dt*flux/p.dx;}}
		RISE::FireSim::PeriodicMACShape oracleShape;
		oracleShape.nx=p.nx;oracleShape.ny=p.ny;oracleShape.nz=p.nz;oracleShape.cellWidthM=p.dx;
		RISE::FireSim::OpenFluxPair3D oracleFlux;
		RISE::FireSim::OpenMACField3D oracleVelocity;
		RISE::FireSim::OpenBoundaryConfig3D oracleBoundary;
		oracleBoundary.bottomFuelMask.assign(16u,true);
		if(upper){oracleBoundary.kind[4]=RISE::FireSim::PressureOpenBoundary3D;
			oracleBoundary.kind[5]=RISE::FireSim::AdiabaticWallBoundary3D;
			oracleBoundary.bottomFuelMask.clear();}
		std::array<std::vector<double>,3> oracleAlpha;
		for(unsigned int axis=0u;axis<3u;++axis){
			oracleFlux.low[axis].resize(axisFaces);oracleFlux.high[axis].resize(axisFaces);
			oracleFlux.nonadvectiveMass[axis].resize(axisFaces);
			oracleVelocity.component[axis].assign(axisFaces,1.0);oracleAlpha[axis].assign(axisFaces,0.0);
			for(std::size_t face=0u;face<axisFaces;++face){
				for(std::size_t component=0u;component<9u;++component)
					oracleFlux.low[axis][face][component]=0.0;
				oracleFlux.nonadvectiveMass[axis][face].fill(0.0);
				oracleFlux.low[axis][face][1]=low[faces+axis*axisFaces+face];
				oracleFlux.high[axis][face]=oracleFlux.low[axis][face];}}
		const auto oracleRate=RISE::FireSim::OpenCompatibleMomentumFluxDivergence3D(
			oracleShape,oracleFlux,oracleAlpha,oracleVelocity,&oracleBoundary);
		std::array<float,9> ambient={{1.0f,1.0f}};
		auto upload=[&](const void* data,std::size_t bytes){return [device newBufferWithBytes:data
			length:bytes options:MTLResourceStorageModeShared];};
		const std::uint32_t zero=0u;
		id<MTLBuffer> lowBuffer=upload(low.data(),low.size()*sizeof(float)),
			deltaBuffer=upload(delta.data(),delta.size()*sizeof(float)),
			alphaBuffer=upload(alpha.data(),alpha.size()*sizeof(float)),
			velocityBuffer=upload(velocity.data(),velocity.size()*sizeof(float)),
			stateBuffer=upload(state.data(),state.size()*sizeof(float)),
			sourceBuffer=upload(sourceDelta.data(),sourceDelta.size()*sizeof(float)),
			acceptedBuffer=upload(accepted.data(),accepted.size()*sizeof(float)),
			ambientBuffer=upload(ambient.data(),sizeof(ambient)),parameter=upload(&p,sizeof(p)),
			failure=upload(&zero,sizeof(zero)),residual=upload(&zero,sizeof(zero)),scale=upload(&zero,sizeof(zero)),
			rate=[device newBufferWithLength:faces*sizeof(float) options:MTLResourceStorageModeShared];
		const std::array<id<MTLBuffer>,13> allocated={{lowBuffer,deltaBuffer,alphaBuffer,
			velocityBuffer,stateBuffer,sourceBuffer,acceptedBuffer,ambientBuffer,parameter,
			failure,residual,scale,rate}};
		for(id<MTLBuffer> buffer:allocated)if(!buffer)return 7;
		id<MTLCommandQueue> queue=[device newCommandQueue];
		id<MTLCommandBuffer> command=[queue commandBuffer];
		auto encode=[&](id<MTLComputePipelineState> ps,const std::vector<id<MTLBuffer>>& buffers){
			id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];if(!encoder)return false;
			[encoder setComputePipelineState:ps];for(std::size_t i=0u;i<buffers.size();++i)
				[encoder setBuffer:buffers[i] offset:0 atIndex:i];
			[encoder dispatchThreads:MTLSizeMake(faces,1u,1u) threadsPerThreadgroup:MTLSizeMake(64u,1u,1u)];
			[encoder endEncoding];return true;};
		if(!command||!encode(ratePipeline,{lowBuffer,deltaBuffer,alphaBuffer,velocityBuffer,
			velocityBuffer,velocityBuffer,rate,failure,parameter})||
			!encode(identityPipeline,{stateBuffer,sourceBuffer,acceptedBuffer,ambientBuffer,
				rate,residual,scale,failure,parameter,lowBuffer,deltaBuffer,alphaBuffer}))return 8;
		[command commit];[command waitUntilCompleted];
		if([command status]!=MTLCommandBufferStatusCompleted)return 9;
		float measured=0.0f;std::memcpy(&measured,[residual contents],sizeof(measured));
		const auto* rates=static_cast<const float*>([rate contents]);
		const auto bits=*static_cast<const std::uint32_t*>([failure contents]);
		// Tangential interior x-face at the selected prescribed plane: mass
		// changes both adjacent cells, but the tangential product is zero.
		const std::size_t face=upper?66u:6u;
		const float expected=(upper?-1.0f:1.0f)*p.dt*j*(unequal?1.5f:1.0f)/p.dx;
		std::size_t oracleMismatches=0u;
		for(unsigned int axis=0u;axis<3u;++axis)for(std::size_t local=0u;local<axisFaces;++local)
			if(rates[axis*axisFaces+local]!=oracleRate.component[axis][local])++oracleMismatches;
		std::cout<<std::setprecision(17)<<"R239_INLET_COMMUTING diagnostic_only=1 "
			<<"upper="<<upper<<" unequal="<<unequal<<" omit_boundary="<<omitBoundary
			<<" full_endpoint="<<fullEndpoint
			<<" inlet_j_kg_m2_s="<<j<<" physical_tangential_rate_kg_m2_s2="<<rates[face]
			<<" restricted_scalar_increment_kg_m3="<<expected
			<<" maximum_identity_residual_kg_m3="<<measured
			<<" oracle_face_mismatches="<<oracleMismatches
			<<" kernel_failure_bits="<<bits<<'\n';
		return bits==0u&&rates[face]==0.0f&&measured==0.0f&&oracleMismatches==0u?0:10;
	}
}
