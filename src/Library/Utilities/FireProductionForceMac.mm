//////////////////////////////////////////////////////////////////////
//
//  FireProductionForceMac.mm - Metal frozen-force comparison wrapper
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "FireProductionForce.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

namespace RISE
{
	namespace
	{
		struct ForceParameters
		{
			std::uint32_t nx,ny,nz,padding0;
			float cellWidthM,timeStepS,ambientDensityKGPerM3,vremanCoefficient;
			float gravity[3],padding1;
			std::uint32_t boundary[6],faceOffset[3],totalFaces;
		};
		static_assert(sizeof(ForceParameters)==88u,
			"force Metal resource certificate binds the parameter bytes");

		bool Fail( std::string* error, const char* message ) noexcept
		{
			if( error ) try { *error=message; } catch( const std::bad_alloc& ) {}
			return false;
		}

		std::string MetalError( const char* prefix, NSError* error )
		{
			std::string result(prefix);
			if( error ) result += " ["+std::string([[error domain] UTF8String])+" "+
				std::to_string(static_cast<long>([error code]))+"] "+
				std::string([[error localizedDescription] UTF8String]);
			return result;
		}

		const char* ForceSource()
		{
			return R"METAL(
#include <metal_stdlib>
using namespace metal;
struct Params {
 uint nx,ny,nz,pad0;float dx,dt,ambient,cv;float3 gravity;
 uint boundary[6];uint faceOffset[3];uint totalFaces;
};
inline uint cell_index(constant Params& p,uint x,uint y,uint z){return (z*p.ny+y)*p.nx+x;}
inline uint extent(constant Params& p,uint axis){return axis==0u?p.nx:(axis==1u?p.ny:p.nz);}
inline uint face_count(constant Params& p,uint axis){
 return axis==0u?(p.nx+1u)*p.ny*p.nz:(axis==1u?p.nx*(p.ny+1u)*p.nz:p.nx*p.ny*(p.nz+1u));
}
inline uint face_index(constant Params& p,uint axis,uint x,uint y,uint z){
 uint local=axis==0u?(z*p.ny+y)*(p.nx+1u)+x:
  (axis==1u?(z*(p.ny+1u)+y)*p.nx+x:(z*p.ny+y)*p.nx+x);
 return p.faceOffset[axis]+local;
}
inline void face_coordinate(constant Params& p,uint axis,uint local,
 thread uint& x,thread uint& y,thread uint& z){
 if(axis==0u){x=local%(p.nx+1u);uint r=local/(p.nx+1u);y=r%p.ny;z=r/p.ny;return;}
 if(axis==1u){x=local%p.nx;uint r=local/p.nx;y=r%(p.ny+1u);z=r/(p.ny+1u);return;}
 x=local%p.nx;uint r=local/p.nx;y=r%p.ny;z=r/p.ny;
}
inline uint face_axis(constant Params& p,uint packed){
 return packed<p.faceOffset[1]?0u:(packed<p.faceOffset[2]?1u:2u);
}
kernel void setup_face_velocity(device const float* density [[buffer(0)]],
 device const float* momentum [[buffer(1)]],device float* velocity [[buffer(2)]],
 constant Params& p [[buffer(3)]],uint gid [[thread_position_in_grid]]){
 if(gid>=p.totalFaces)return;uint axis=face_axis(p,gid),local=gid-p.faceOffset[axis];
 uint x,y,z;face_coordinate(p,axis,local,x,y,z);uint c=axis==0u?x:(axis==1u?y:z);
 bool wall=(c==0u&&p.boundary[2u*axis]==2u)||(c==extent(p,axis)&&p.boundary[2u*axis+1u]==2u);
 velocity[gid]=wall?0.0f:momentum[gid]/density[gid];
}
kernel void build_cell_velocity(device const float* faceVelocity [[buffer(0)]],
 device float* cellVelocity [[buffer(1)]],constant Params& p [[buffer(2)]],
 uint gid [[thread_position_in_grid]]){
 uint cells=p.nx*p.ny*p.nz;if(gid>=cells)return;
 uint x=gid%p.nx,r=gid/p.nx,y=r%p.ny,z=r/p.ny;
 cellVelocity[gid]=0.5f*(faceVelocity[face_index(p,0u,x,y,z)]+faceVelocity[face_index(p,0u,x+1u,y,z)]);
 cellVelocity[cells+gid]=0.5f*(faceVelocity[face_index(p,1u,x,y,z)]+faceVelocity[face_index(p,1u,x,y+1u,z)]);
 cellVelocity[2u*cells+gid]=0.5f*(faceVelocity[face_index(p,2u,x,y,z)]+faceVelocity[face_index(p,2u,x,y,z+1u)]);
}
inline float neighbor_velocity(device const float* velocity,constant Params& p,uint component,
 uint x,uint y,uint z,uint derivative,int direction){
 uint coordinate=derivative==0u?x:(derivative==1u?y:z),n=extent(p,derivative);
 if(direction<0&&coordinate>0u){if(derivative==0u)--x;if(derivative==1u)--y;if(derivative==2u)--z;}
 else if(direction>0&&coordinate+1u<n){if(derivative==0u)++x;if(derivative==1u)++y;if(derivative==2u)++z;}
 else {uint side=2u*derivative+(direction>0?1u:0u),b=p.boundary[side];
  uint cell=cell_index(p,x,y,z),cells=p.nx*p.ny*p.nz;
  if(b==2u)return -velocity[component*cells+cell];if(b==1u)return velocity[component*cells+cell];
  if(derivative==0u)x=direction<0?p.nx-1u:0u;if(derivative==1u)y=direction<0?p.ny-1u:0u;
  if(derivative==2u)z=direction<0?p.nz-1u:0u;}
 return velocity[component*(p.nx*p.ny*p.nz)+cell_index(p,x,y,z)];
}
kernel void build_stress(device const float* cellVelocity [[buffer(0)]],
 device const float* cellDensity [[buffer(1)]],device const float* molecularNu [[buffer(2)]],
 device float* eddy [[buffer(3)]],device float* muOut [[buffer(4)]],
 device float* stress [[buffer(5)]],constant Params& p [[buffer(6)]],
 uint gid [[thread_position_in_grid]]){
 uint cells=p.nx*p.ny*p.nz;if(gid>=cells)return;uint x=gid%p.nx,r=gid/p.nx,y=r%p.ny,z=r/p.ny;
 float gradient[3][3];float divergence=0.0f;
 for(uint d=0u;d<3u;++d){for(uint c=0u;c<3u;++c){
  float previous=neighbor_velocity(cellVelocity,p,c,x,y,z,d,-1);
  float next=neighbor_velocity(cellVelocity,p,c,x,y,z,d,1);
  gradient[d][c]=(next-previous)/(2.0f*p.dx);
 } divergence+=gradient[d][d];}
 float alphaSquared=0.0f,beta[3][3]={{0.0f,0.0f,0.0f},{0.0f,0.0f,0.0f},{0.0f,0.0f,0.0f}};
 for(uint i=0u;i<3u;++i)for(uint j=0u;j<3u;++j)alphaSquared+=gradient[i][j]*gradient[i][j];
 float widthSquared=p.dx*p.dx;
 for(uint m=0u;m<3u;++m)for(uint i=0u;i<3u;++i)for(uint j=0u;j<3u;++j)
  beta[i][j]+=widthSquared*gradient[m][i]*gradient[m][j];
 float raw=beta[0][0]*beta[1][1]-beta[0][1]*beta[0][1]+
  beta[0][0]*beta[2][2]-beta[0][2]*beta[0][2]+
  beta[1][1]*beta[2][2]-beta[1][2]*beta[1][2];
 float nuEddy=alphaSquared==0.0f?0.0f:p.cv*sqrt(max(0.0f,raw)/alphaSquared);
 float mu=cellDensity[gid]*(molecularNu[gid]+nuEddy);eddy[gid]=nuEddy;muOut[gid]=mu;
 for(uint c=0u;c<3u;++c)for(uint d=0u;d<3u;++d)
  stress[(3u*c+d)*cells+gid]=mu*(gradient[d][c]+gradient[c][d]-(c==d?(2.0f/3.0f)*divergence:0.0f));
}
inline uint shifted_cell(constant Params& p,uint x,uint y,uint z,uint axis,int direction,
 thread bool& exists){
 uint coordinate=axis==0u?x:(axis==1u?y:z),n=extent(p,axis);exists=true;
 if(direction<0&&coordinate>0u){if(axis==0u)--x;if(axis==1u)--y;if(axis==2u)--z;}
 else if(direction>0&&coordinate+1u<n){if(axis==0u)++x;if(axis==1u)++y;if(axis==2u)++z;}
 else {uint b=p.boundary[2u*axis+(direction>0?1u:0u)];if(b!=0u){exists=false;return cell_index(p,x,y,z);}
  if(axis==0u)x=direction<0?p.nx-1u:0u;if(axis==1u)y=direction<0?p.ny-1u:0u;
  if(axis==2u)z=direction<0?p.nz-1u:0u;}
 return cell_index(p,x,y,z);
}
kernel void build_face_force(device const float* faceDensity [[buffer(0)]],
 device const float* stress [[buffer(1)]],device float* viscous [[buffer(2)]],
 device float* gravity [[buffer(3)]],constant Params& p [[buffer(4)]],
 uint gid [[thread_position_in_grid]]){
 if(gid>=p.totalFaces)return;uint component=face_axis(p,gid),local=gid-p.faceOffset[component];
 uint x,y,z;face_coordinate(p,component,local,x,y,z);uint xyz[3]={x,y,z};
 uint normal=xyz[component],n=extent(p,component);bool periodic=p.boundary[2u*component]==0u;
 if(periodic&&normal==n)return;bool wall=(normal==0u&&p.boundary[2u*component]==2u)||
  (normal==n&&p.boundary[2u*component+1u]==2u);
 float gravityValue=wall?0.0f:p.dt*(faceDensity[gid]-p.ambient)*p.gravity[component];gravity[gid]=gravityValue;
 float value=0.0f;
 if((normal!=0u||periodic)&&normal!=n){uint leftXYZ[3]={x,y,z};leftXYZ[component]=normal==0u?n-1u:normal-1u;
  uint left=cell_index(p,leftXYZ[0],leftXYZ[1],leftXYZ[2]),right=cell_index(p,x,y,z),cells=p.nx*p.ny*p.nz;
  value=(stress[(3u*component+component)*cells+right]-stress[(3u*component+component)*cells+left])/p.dx;
  for(uint d=0u;d<3u;++d)if(d!=component){bool lpExists,rpExists,lnExists,rnExists;
   uint lp=shifted_cell(p,leftXYZ[0],leftXYZ[1],leftXYZ[2],d,-1,lpExists);
   uint rp=shifted_cell(p,x,y,z,d,-1,rpExists);
   uint ln=shifted_cell(p,leftXYZ[0],leftXYZ[1],leftXYZ[2],d,1,lnExists);
   uint rn=shifted_cell(p,x,y,z,d,1,rnExists);
   uint leftPrevious=lpExists?lp:left,rightPrevious=rpExists?rp:right;
   uint leftNext=lnExists?ln:left,rightNext=rnExists?rn:right;
   float distance=(lpExists&&rpExists&&lnExists&&rnExists)?4.0f*p.dx:2.0f*p.dx;
   uint base=(3u*component+d)*cells;
   value+=(stress[base+leftNext]+stress[base+rightNext]-stress[base+leftPrevious]-stress[base+rightPrevious])/distance;
  }
 }
 viscous[gid]=value;
 if(periodic&&normal==0u){uint highXYZ[3]={x,y,z};highXYZ[component]=n;
  uint high=face_index(p,component,highXYZ[0],highXYZ[1],highXYZ[2]);viscous[high]=value;gravity[high]=gravityValue;}
}
)METAL";
		}

		struct ForceContext
		{
			id<MTLDevice> device;
			id<MTLCommandQueue> queue;
			id<MTLComputePipelineState> setupFace,cellVelocity,stress,faceForce;
			std::string error;

			ForceContext() : device(MTLCreateSystemDefaultDevice())
			{
				if( !device ) {error="production frozen-force has no Metal device";return;}
				queue=[device newCommandQueue];if( !queue ) {error="production frozen-force queue failed";return;}
				MTLCompileOptions* options=[[MTLCompileOptions alloc] init];
				if( [options respondsToSelector:@selector(setMathMode:)] ) options.mathMode=MTLMathModeSafe;
				NSError* libraryError=nil;id<MTLLibrary> library=[device newLibraryWithSource:
					[NSString stringWithUTF8String:ForceSource()] options:options error:&libraryError];
				if( !library ) {error=MetalError("production frozen-force library failed",libraryError);return;}
				auto pipeline=[&](const char* name) -> id<MTLComputePipelineState> {
					id<MTLFunction> function=[library newFunctionWithName:[NSString stringWithUTF8String:name]];
					if( !function ) return nil;NSError* pipelineError=nil;
					id<MTLComputePipelineState> result=[device newComputePipelineStateWithFunction:
						function error:&pipelineError];
					if( !result&&error.empty() ) error=MetalError("production frozen-force pipeline failed",pipelineError);
					return result;
				};
				setupFace=pipeline("setup_face_velocity");cellVelocity=pipeline("build_cell_velocity");
				stress=pipeline("build_stress");faceForce=pipeline("build_face_force");
			}
			bool Valid() const {return device&&queue&&setupFace&&cellVelocity&&stress&&faceForce;}
		};

		ForceContext& Context() {static ForceContext context;return context;}

		void Dispatch( id<MTLComputeCommandEncoder> encoder,
			id<MTLComputePipelineState> pipeline, std::size_t count )
		{
			[encoder setComputePipelineState:pipeline];
			const NSUInteger width=std::min<NSUInteger>([pipeline maxTotalThreadsPerThreadgroup],256u);
			[encoder dispatchThreads:MTLSizeMake(count,1u,1u)
				threadsPerThreadgroup:MTLSizeMake(width,1u,1u)];
		}

		bool AllFinite( const std::vector<float>& values )
		{
			return std::all_of(values.begin(),values.end(),[](float value){return std::isfinite(value);});
		}

		bool InjectedFailure( const char* stage )
		{
			const char* value=std::getenv("RISE_FIRE_FORCE_TEST_FAILURE");
			return value&&std::strcmp(value,stage)==0;
		}
	}

	bool BuildFireProductionFrozenForceFieldsMetal(
		const FireProductionFrozenForceRequest& request,
		FireProductionFrozenForceResult& result,
		double& deviceElapsedMS,
		std::string* error )
	{
		result=FireProductionFrozenForceResult();deviceElapsedMS=0.0;
		try {
			const FireProductionProjectionShape& shape=request.shape;
			if( shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
				shape.nz<4u||shape.nz>1024u||!(shape.cellWidthM>0.0f)||
				!std::isfinite(shape.cellWidthM)||!(request.timeStepS>0.0f)||
				!std::isfinite(request.timeStepS)||!(request.ambientDensityKGPerM3>0.0f)||
				!std::isfinite(request.ambientDensityKGPerM3)||
				!(request.vremanCoefficient>=0.0f)||!std::isfinite(request.vremanCoefficient) )
				return Fail(error,"production frozen-force Metal shape or scalar is invalid");
			for( const float gravity : request.gravityMPerS2 ) if( !std::isfinite(gravity) )
				return Fail(error,"production frozen-force Metal gravity is nonfinite");
			for( unsigned int axis=0u;axis<3u;++axis ) {
				const FireProductionProjectionBoundary lower=request.boundary[2u*axis];
				const FireProductionProjectionBoundary upper=request.boundary[2u*axis+1u];
				if( lower<FireProductionProjectionPeriodic||lower>FireProductionProjectionWall||
					upper<FireProductionProjectionPeriodic||upper>FireProductionProjectionWall||
					((lower==FireProductionProjectionPeriodic)!=(upper==FireProductionProjectionPeriodic)) )
					return Fail(error,"production frozen-force Metal boundary pairing is invalid");
			}
			const std::size_t cells=shape.CellCount();
			std::array<std::size_t,3> faceCounts={};std::size_t faces=0u;
			for( unsigned int axis=0u;axis<3u;++axis ) {faceCounts[axis]=
				FireProductionProjectionFaceCount(shape,axis);faces+=faceCounts[axis];}
			std::uint64_t certifiedBytes=0u;
			if( !FireProductionFrozenForceMetalWorkingSetBytes(shape,certifiedBytes)||
				certifiedBytes>(UINT64_C(1)<<31u) )
				return Fail(error,"production frozen-force Metal working set exceeds two GiB");
			if( request.cellGasDensityKGPerM3.size()!=cells||
				request.molecularKinematicViscosityM2PerS.size()!=cells )
				return Fail(error,"production frozen-force Metal cell shape is invalid");
			for( const float value : request.cellGasDensityKGPerM3 )
				if( !(value>0.0f)||!std::isfinite(value) ) return Fail(error,
					"production frozen-force Metal cell density is invalid");
			for( const float value : request.molecularKinematicViscosityM2PerS )
				if( !(value>=0.0f)||!std::isfinite(value) ) return Fail(error,
					"production frozen-force Metal viscosity is invalid");
			std::vector<float> packedDensity(faces),packedMomentum(faces);
			std::size_t offset=0u;
			for( unsigned int axis=0u;axis<3u;++axis ) {
				if( request.faceDensityKGPerM3[axis].size()!=faceCounts[axis]||
					request.beginningMomentumKGPerM2S[axis].size()!=faceCounts[axis] )
					return Fail(error,"production frozen-force Metal face shape is invalid");
				for( std::size_t face=0u;face<faceCounts[axis];++face ) {
					const float density=request.faceDensityKGPerM3[axis][face];
					const float momentum=request.beginningMomentumKGPerM2S[axis][face];
					if( !(density>0.0f)||!std::isfinite(density)||!std::isfinite(momentum) )
						return Fail(error,"production frozen-force Metal face state is invalid");
					packedDensity[offset+face]=density;packedMomentum[offset+face]=momentum;
				}
				if( request.boundary[2u*axis]==FireProductionProjectionPeriodic ) {
					const std::size_t plane=faceCounts[axis]/(axis==0u?shape.nx+1u:
						(axis==1u?shape.ny+1u:shape.nz+1u));
					const std::size_t extent=axis==0u?shape.nx:(axis==1u?shape.ny:shape.nz);
					for( std::size_t line=0u;line<plane;++line ) {
						const std::size_t low=axis==0u?line*(extent+1u):
							(axis==1u?(line/shape.nx)*(extent+1u)*shape.nx+line%shape.nx:
								line);
						const std::size_t high=axis==0u?low+extent:
							(axis==1u?low+extent*shape.nx:low+extent*shape.nx*shape.ny);
						if( std::memcmp(&request.faceDensityKGPerM3[axis][low],
							&request.faceDensityKGPerM3[axis][high],sizeof(float))!=0||
							std::memcmp(&request.beginningMomentumKGPerM2S[axis][low],
								&request.beginningMomentumKGPerM2S[axis][high],sizeof(float))!=0 )
							return Fail(error,"production frozen-force Metal periodic seam differs");
					}
				}
				offset+=faceCounts[axis];
			}
			ForceContext& context=Context();if( !context.Valid() ) return Fail(error,context.error.c_str());
			@autoreleasepool {
				const std::size_t cellBytes=cells*sizeof(float),faceBytes=faces*sizeof(float);
				ForceParameters p={static_cast<std::uint32_t>(shape.nx),static_cast<std::uint32_t>(shape.ny),
					static_cast<std::uint32_t>(shape.nz),0u,shape.cellWidthM,request.timeStepS,
					request.ambientDensityKGPerM3,request.vremanCoefficient,
					{request.gravityMPerS2[0],request.gravityMPerS2[1],request.gravityMPerS2[2]},0.0f,
					{}, {0u,static_cast<std::uint32_t>(faceCounts[0]),
						static_cast<std::uint32_t>(faceCounts[0]+faceCounts[1])},
					static_cast<std::uint32_t>(faces)};
				for( unsigned int side=0u;side<6u;++side ) p.boundary[side]=request.boundary[side];
				id<MTLBuffer> rho=[context.device newBufferWithBytes:request.cellGasDensityKGPerM3.data()
					length:cellBytes options:MTLResourceStorageModeShared];
				id<MTLBuffer> nu=[context.device newBufferWithBytes:request.molecularKinematicViscosityM2PerS.data()
					length:cellBytes options:MTLResourceStorageModeShared];
				id<MTLBuffer> faceRho=[context.device newBufferWithBytes:packedDensity.data()
					length:faceBytes options:MTLResourceStorageModeShared];
				id<MTLBuffer> momentum=[context.device newBufferWithBytes:packedMomentum.data()
					length:faceBytes options:MTLResourceStorageModeShared];
				id<MTLBuffer> faceVelocity=[context.device newBufferWithLength:faceBytes
					options:MTLResourceStorageModePrivate];
				id<MTLBuffer> cellVelocity=[context.device newBufferWithLength:3u*cellBytes
					options:MTLResourceStorageModePrivate];
				id<MTLBuffer> stress=[context.device newBufferWithLength:9u*cellBytes
					options:MTLResourceStorageModePrivate];
				id<MTLBuffer> eddy=[context.device newBufferWithLength:cellBytes
					options:MTLResourceStorageModeShared];
				id<MTLBuffer> mu=[context.device newBufferWithLength:cellBytes
					options:MTLResourceStorageModeShared];
				id<MTLBuffer> viscous=[context.device newBufferWithLength:faceBytes
					options:MTLResourceStorageModeShared];
				id<MTLBuffer> gravity=[context.device newBufferWithLength:faceBytes
					options:MTLResourceStorageModeShared];
				id<MTLBuffer> parameters=[context.device newBufferWithBytes:&p length:sizeof(p)
					options:MTLResourceStorageModeShared];
				if( InjectedFailure("buffer") ) faceVelocity=nil;
				if( !rho||!nu||!faceRho||!momentum||!faceVelocity||!cellVelocity||!stress||
					!eddy||!mu||!viscous||!gravity||!parameters )
					return Fail(error,"production frozen-force Metal buffer allocation failed");
				std::uint64_t actual=(4u*cells+6u*faces)*sizeof(float);
				const id<MTLBuffer> buffers[]={rho,nu,faceRho,momentum,faceVelocity,cellVelocity,
					stress,eddy,mu,viscous,gravity,parameters};
				static_assert(sizeof(buffers)/sizeof(buffers[0])==12u,
					"force Metal actual-allocation certificate binds every live buffer");
				for( id<MTLBuffer> buffer : buffers ) {
					const std::uint64_t allocated=[buffer allocatedSize];
					if( actual>std::numeric_limits<std::uint64_t>::max()-allocated )
						return Fail(error,"production frozen-force Metal allocation overflowed");
					actual+=allocated;
				}
				if( actual>certifiedBytes||actual>(UINT64_C(1)<<31u) )
					return Fail(error,"production frozen-force Metal actual allocation exceeds certificate");
				id<MTLCommandBuffer> command=InjectedFailure("command-allocation")?
					nil:[context.queue commandBuffer];
				if( !command ) return Fail(error,"production frozen-force Metal command allocation failed");
				auto encode=[&](id<MTLComputePipelineState> pipeline,
					const id<MTLBuffer>* bound,std::size_t boundCount,std::size_t count) -> bool {
					id<MTLComputeCommandEncoder> encoder=InjectedFailure("encoder")?
						nil:[command computeCommandEncoder];if( !encoder ) return false;
					for( std::size_t i=0u;i<boundCount;++i ) [encoder setBuffer:bound[i] offset:0 atIndex:i];
					Dispatch(encoder,pipeline,count);[encoder endEncoding];return true;
				};
				const id<MTLBuffer> setupBuffers[]={faceRho,momentum,faceVelocity,parameters};
				const id<MTLBuffer> cellBuffers[]={faceVelocity,cellVelocity,parameters};
				const id<MTLBuffer> stressBuffers[]={cellVelocity,rho,nu,eddy,mu,stress,parameters};
				const id<MTLBuffer> forceBuffers[]={faceRho,stress,viscous,gravity,parameters};
				if( !encode(context.setupFace,setupBuffers,4u,faces)||
					!encode(context.cellVelocity,cellBuffers,3u,cells)||
					!encode(context.stress,stressBuffers,7u,cells)||
					!encode(context.faceForce,forceBuffers,5u,faces) )
					return Fail(error,"production frozen-force Metal encoder allocation failed");
				[command commit];[command waitUntilCompleted];
				if( InjectedFailure("command")||[command status]!=MTLCommandBufferStatusCompleted ) {
					const std::string detail=MetalError("production frozen-force Metal command failed",[command error]);
					return Fail(error,detail.c_str());
				}
				FireProductionFrozenForceResult computed;
				const float* eddyValues=static_cast<const float*>([eddy contents]);
				const float* muValues=static_cast<const float*>([mu contents]);
				const float* viscousValues=static_cast<const float*>([viscous contents]);
				const float* gravityValues=static_cast<const float*>([gravity contents]);
				computed.eddyKinematicViscosityM2PerS.assign(eddyValues,eddyValues+cells);
				computed.effectiveDynamicViscosityPaS.assign(muValues,muValues+cells);
				offset=0u;for( unsigned int axis=0u;axis<3u;++axis ) {
					computed.beginningViscousMomentumRateKGPerM2S2[axis].assign(
						viscousValues+offset,viscousValues+offset+faceCounts[axis]);
					computed.gravityMomentumIncrementKGPerM2S[axis].assign(
						gravityValues+offset,gravityValues+offset+faceCounts[axis]);offset+=faceCounts[axis];
				}
				if( !AllFinite(computed.eddyKinematicViscosityM2PerS)||
					!AllFinite(computed.effectiveDynamicViscosityPaS) )
					return Fail(error,"production frozen-force Metal output is nonfinite");
				for( unsigned int axis=0u;axis<3u;++axis ) if( !AllFinite(
					computed.beginningViscousMomentumRateKGPerM2S2[axis])||!AllFinite(
					computed.gravityMomentumIncrementKGPerM2S[axis]) )
					return Fail(error,"production frozen-force Metal output is nonfinite");
				if( InjectedFailure("output")&&
					!computed.beginningViscousMomentumRateKGPerM2S2[0].empty() )
					computed.beginningViscousMomentumRateKGPerM2S2[0][0]=
						std::numeric_limits<float>::quiet_NaN();
				if( !AllFinite(computed.beginningViscousMomentumRateKGPerM2S2[0]) )
					return Fail(error,"production frozen-force Metal output is nonfinite");
				deviceElapsedMS=([command GPUEndTime]-[command GPUStartTime])*1000.0;
				if( !std::isfinite(deviceElapsedMS) ) return Fail(error,
					"production frozen-force Metal timing is nonfinite");
				result=std::move(computed);
			}
			if( error ) error->clear();return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionFrozenForceResult();deviceElapsedMS=0.0;
			return Fail(error,"production frozen-force Metal allocation failed");
		}
	}
}
