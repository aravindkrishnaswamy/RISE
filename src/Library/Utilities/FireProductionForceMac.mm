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
		enum ResidentForceHostAccessKind
		{
			ResidentForceScalarAccess,
			ResidentForceInterstageFullGridAccess,
			ResidentForceTerminalAccess
		};
		enum ResidentForceTransferPhase
		{
			ResidentForceUploadTransfer,
			ResidentForceInterstageTransfer,
			ResidentForceTerminalTransfer
		};

		thread_local std::uint64_t residentForceCommandCommitCount=0u;
		thread_local std::uint64_t residentForceInterstageFullGridReadCount=0u;
		thread_local ResidentForceTransferPhase residentForceTransferPhase=
			ResidentForceInterstageTransfer;

		struct ResidentForceTransferScope
		{
			ResidentForceTransferPhase previous;
			explicit ResidentForceTransferScope( ResidentForceTransferPhase phase ) :
				previous(residentForceTransferPhase) {residentForceTransferPhase=phase;}
			~ResidentForceTransferScope() {residentForceTransferPhase=previous;}
		};

		void CommitResidentForceCommand( id<MTLCommandBuffer> command )
		{
			[command commit];++residentForceCommandCommitCount;
		}

		void* ResidentForceBufferContents( id<MTLBuffer> buffer,
			ResidentForceHostAccessKind kind )
		{
			if( kind==ResidentForceInterstageFullGridAccess )
				++residentForceInterstageFullGridReadCount;
			return [buffer contents];
		}

		void CopyResidentForceBuffer( id<MTLBlitCommandEncoder> encoder,
			id<MTLBuffer> source,std::size_t sourceOffset,id<MTLBuffer> destination,
			std::size_t destinationOffset,std::size_t size )
		{
			[encoder copyFromBuffer:source sourceOffset:sourceOffset toBuffer:destination
				destinationOffset:destinationOffset size:size];
			const bool sourceVisible=[source storageMode]!=MTLStorageModePrivate;
			const bool destinationVisible=[destination storageMode]!=MTLStorageModePrivate;
			if( residentForceTransferPhase==ResidentForceInterstageTransfer&&
				sourceVisible!=destinationVisible )
				++residentForceInterstageFullGridReadCount;
		}
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
kernel void build_stress_fixed_mu(device const float* cellVelocity [[buffer(0)]],
 device const float* mu [[buffer(1)]],device float* stress [[buffer(2)]],
 constant Params& p [[buffer(3)]],uint gid [[thread_position_in_grid]]){
 uint cells=p.nx*p.ny*p.nz;if(gid>=cells)return;uint x=gid%p.nx,r=gid/p.nx,y=r%p.ny,z=r/p.ny;
 float gradient[3][3];float divergence=0.0f;
 for(uint d=0u;d<3u;++d){for(uint c=0u;c<3u;++c){
  float previous=neighbor_velocity(cellVelocity,p,c,x,y,z,d,-1);
  float next=neighbor_velocity(cellVelocity,p,c,x,y,z,d,1);
  gradient[d][c]=(next-previous)/(2.0f*p.dx);
 } divergence+=gradient[d][d];}
 float dynamicViscosity=mu[gid];
 for(uint c=0u;c<3u;++c)for(uint d=0u;d<3u;++d)
  stress[(3u*c+d)*cells+gid]=dynamicViscosity*(gradient[d][c]+gradient[c][d]-
   (c==d?(2.0f/3.0f)*divergence:0.0f));
}
kernel void prepare_max_mu(device const float* mu [[buffer(0)]],device float* scratch [[buffer(1)]],
 constant Params& p [[buffer(2)]],uint gid [[thread_position_in_grid]]){
 uint cells=p.nx*p.ny*p.nz;scratch[gid]=gid<cells?mu[gid]:0.0f;
}
kernel void prepare_max_inverse_density(device const float* density [[buffer(0)]],
 device float* scratch [[buffer(1)]],constant Params& p [[buffer(2)]],
 uint gid [[thread_position_in_grid]]){
 scratch[gid]=gid<p.totalFaces?nextafter(1.0f/density[gid],INFINITY):0.0f;
}
kernel void reduce_max_pair(device const float* source [[buffer(0)]],device float* target [[buffer(1)]],
 constant uint& sourceCount [[buffer(2)]],uint gid [[thread_position_in_grid]]){
 uint first=2u*gid;if(first>=sourceCount)return;float value=source[first];
 if(first+1u<sourceCount)value=max(value,source[first+1u]);target[gid]=value;
}
kernel void store_maximum(device const float* source [[buffer(0)]],device float* maxima [[buffer(1)]],
 constant uint& slot [[buffer(2)]]){maxima[slot]=source[0];}
inline float outward_multiply(float a,float b){return nextafter(a*b,INFINITY);}
kernel void build_lambda(device const float* maxima [[buffer(0)]],device float* diagnostic [[buffer(1)]],
 constant Params& p [[buffer(2)]]){
 float invDx=nextafter(1.0f/p.dx,INFINITY);float invDx2=outward_multiply(invDx,invDx);
 float value=outward_multiply(maxima[0],maxima[1]);value=outward_multiply(value,invDx2);
 diagnostic[0]=outward_multiply(24.0f,value);diagnostic[1]=maxima[0];diagnostic[2]=maxima[1];
}
kernel void update_viscous_momentum(device float* momentum [[buffer(0)]],
 device const float* rate [[buffer(1)]],constant Params& p [[buffer(2)]],
 constant float& dtSub [[buffer(3)]],uint gid [[thread_position_in_grid]]){
 if(gid>=p.totalFaces)return;uint axis=face_axis(p,gid),local=gid-p.faceOffset[axis];
 uint x,y,z;face_coordinate(p,axis,local,x,y,z);uint coordinate=axis==0u?x:(axis==1u?y:z);
 uint n=extent(p,axis);bool periodic=p.boundary[2u*axis]==0u;if(periodic&&coordinate==n)return;
 bool wall=(coordinate==0u&&p.boundary[2u*axis]==2u)||(coordinate==n&&p.boundary[2u*axis+1u]==2u);
 float value=wall?0.0f:momentum[gid]+dtSub*rate[gid];momentum[gid]=value;
 if(periodic&&coordinate==0u){uint xyz[3]={x,y,z};xyz[axis]=n;
  momentum[face_index(p,axis,xyz[0],xyz[1],xyz[2])]=value;}
}
kernel void add_gravity_momentum(device float* momentum [[buffer(0)]],
 device const float* gravity [[buffer(1)]],constant Params& p [[buffer(2)]],
 uint gid [[thread_position_in_grid]]){
 if(gid>=p.totalFaces)return;uint axis=face_axis(p,gid),local=gid-p.faceOffset[axis];
 uint x,y,z;face_coordinate(p,axis,local,x,y,z);uint coordinate=axis==0u?x:(axis==1u?y:z);
 uint n=extent(p,axis);bool periodic=p.boundary[2u*axis]==0u;if(periodic&&coordinate==n)return;
 bool wall=(coordinate==0u&&p.boundary[2u*axis]==2u)||(coordinate==n&&p.boundary[2u*axis+1u]==2u);
 float value=wall?0.0f:momentum[gid]+gravity[gid];momentum[gid]=value;
 if(periodic&&coordinate==0u){uint xyz[3]={x,y,z};xyz[axis]=n;
  momentum[face_index(p,axis,xyz[0],xyz[1],xyz[2])]=value;}
}
kernel void snapshot_momentum(device const float* momentum [[buffer(0)]],device float* snapshots [[buffer(1)]],
 constant Params& p [[buffer(2)]],constant uint& slot [[buffer(3)]],
 uint gid [[thread_position_in_grid]]){if(gid<p.totalFaces)snapshots[slot*p.totalFaces+gid]=momentum[gid];}
)METAL";
		}

		struct ForceContext
		{
			id<MTLDevice> device;
			id<MTLCommandQueue> queue;
			id<MTLComputePipelineState> setupFace,cellVelocity,stress,faceForce,fixedStress,
				prepareMu,prepareInverseDensity,reduceMax,storeMaximum,buildLambda,
				updateViscous,addGravity,snapshot;
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
				fixedStress=pipeline("build_stress_fixed_mu");prepareMu=pipeline("prepare_max_mu");
				prepareInverseDensity=pipeline("prepare_max_inverse_density");
				reduceMax=pipeline("reduce_max_pair");storeMaximum=pipeline("store_maximum");
				buildLambda=pipeline("build_lambda");updateViscous=pipeline("update_viscous_momentum");
				addGravity=pipeline("add_gravity_momentum");snapshot=pipeline("snapshot_momentum");
			}
			bool Valid() const {return device&&queue&&setupFace&&cellVelocity&&stress&&faceForce&&
				fixedStress&&prepareMu&&prepareInverseDensity&&reduceMax&&storeMaximum&&buildLambda&&
				updateViscous&&addGravity&&snapshot;}
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

		std::size_t NextPowerOfTwo( std::size_t value )
		{
			std::size_t result=1u;while( result<value ) result*=2u;return result;
		}

		std::uint64_t PackedMomentumByteDigest( const float* values, std::size_t count )
		{
			std::uint64_t digest=UINT64_C(14695981039346656037);
			for( std::size_t index=0u;index<count;++index ) {
				std::uint32_t bits=0u;std::memcpy(&bits,values+index,sizeof(bits));
				for( unsigned int byte=0u;byte<4u;++byte ) {
					digest^=static_cast<unsigned char>(bits>>(8u*byte));
					digest*=UINT64_C(1099511628211);
				}
			}
			return digest;
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

	bool AdvanceFireProductionFrozenForceMetalImpl(
		const FireProductionFrozenForceRequest& request,
		bool captureIntermediateStates,
		const std::vector<float>* projectionTarget,
		FireProductionResidentForceProjectionResult* composedResult,
		FireProductionMetalFrozenForceResidentState* residentState,
		FireProductionFrozenForceAdvanceResult& result,
		FireProductionResidentForceDiagnostics& diagnostics,
		std::string* error )
	{
		result=FireProductionFrozenForceAdvanceResult();
		diagnostics=FireProductionResidentForceDiagnostics();
		if( composedResult ) *composedResult=FireProductionResidentForceProjectionResult();
		if( residentState ) *residentState=FireProductionMetalFrozenForceResidentState();
		try {
			const char* activeFailurePhase="production resident force preflight failed";
			bool forceCompleted=false;
			struct ForceFailurePhaseGuard
			{
				std::string* error;
				const char*& phase;
				bool& completed;
				~ForceFailurePhaseGuard(){if(!completed&&error&&error->empty())*error=phase;}
			} failurePhaseGuard={error,activeFailurePhase,forceCompleted};
			auto markPhase=[&](const char* phase){activeFailurePhase=phase;
				if(error)*error=phase;};
			FireProductionResidentForceDiagnostics observed;
			FireProductionViscousSchedule selectedSchedule;
			const std::uint64_t beginningCommandCommits=residentForceCommandCommitCount;
			const std::uint64_t beginningInterstageReads=residentForceInterstageFullGridReadCount;
			const FireProductionProjectionShape& shape=request.shape;
			if( shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
				shape.nz<4u||shape.nz>1024u||!(shape.cellWidthM>0.0f)||
				!std::isfinite(shape.cellWidthM)||!(request.timeStepS>0.0f)||
				!std::isfinite(request.timeStepS)||!(request.ambientDensityKGPerM3>0.0f)||
				!std::isfinite(request.ambientDensityKGPerM3)||
				!(request.vremanCoefficient>=0.0f)||!std::isfinite(request.vremanCoefficient) )
				return Fail(error,"production resident force shape or scalar is invalid");
			for( const float gravity : request.gravityMPerS2 ) if( !std::isfinite(gravity) )
				return Fail(error,"production resident force gravity is nonfinite");
			for( unsigned int axis=0u;axis<3u;++axis ) {
				const FireProductionProjectionBoundary lower=request.boundary[2u*axis];
				const FireProductionProjectionBoundary upper=request.boundary[2u*axis+1u];
				if( lower<FireProductionProjectionPeriodic||lower>FireProductionProjectionWall||
					upper<FireProductionProjectionPeriodic||upper>FireProductionProjectionWall||
					((lower==FireProductionProjectionPeriodic)!=(upper==FireProductionProjectionPeriodic)) )
					return Fail(error,"production resident force boundary pairing is invalid");
			}
			const std::size_t cells=shape.CellCount();
			std::uint64_t combinedPreflightCertificate=0u;
			if( projectionTarget&&(!FireProductionResidentForceProjectionWorkingSetBytes(
				shape,combinedPreflightCertificate)||
				combinedPreflightCertificate>(UINT64_C(1)<<31u)) )
				return Fail(error,
					"production resident force-projection working set exceeds two GiB");
			if( projectionTarget&&(projectionTarget->size()!=cells||
				!AllFinite(*projectionTarget)) )
				return Fail(error,"production resident projection target is invalid");
			std::array<std::size_t,3> faceCounts={};std::size_t faces=0u;
			for( unsigned int axis=0u;axis<3u;++axis ) {
				faceCounts[axis]=FireProductionProjectionFaceCount(shape,axis);faces+=faceCounts[axis];
			}
			std::uint64_t certifiedBytes=0u;
			if( !FireProductionResidentForceMetalWorkingSetBytes(
				shape,captureIntermediateStates,certifiedBytes)||certifiedBytes>(UINT64_C(1)<<31u) )
				return Fail(error,"production resident force working set exceeds two GiB");
			if( projectionTarget ) {
				const std::uint64_t cellBytes=static_cast<std::uint64_t>(cells)*sizeof(float);
				const std::uint64_t allocation=(cellBytes+UINT64_C(16383))&~UINT64_C(16383);
				if( certifiedBytes>std::numeric_limits<std::uint64_t>::max()-2u*allocation )
					return Fail(error,"production resident projection target certificate overflowed");
				certifiedBytes+=2u*allocation;
			}
			if( request.cellGasDensityKGPerM3.size()!=cells||
				request.molecularKinematicViscosityM2PerS.size()!=cells )
				return Fail(error,"production resident force cell shape is invalid");
			for( std::size_t cell=0u;cell<cells;++cell ) if(
				!(request.cellGasDensityKGPerM3[cell]>0.0f)||
				!std::isfinite(request.cellGasDensityKGPerM3[cell])||
				!(request.molecularKinematicViscosityM2PerS[cell]>=0.0f)||
				!std::isfinite(request.molecularKinematicViscosityM2PerS[cell]) )
				return Fail(error,"production resident force cell state is invalid");
			std::vector<float> packedDensity(faces),packedMomentum(faces);std::size_t offset=0u;
			for( unsigned int axis=0u;axis<3u;++axis ) {
				if( request.faceDensityKGPerM3[axis].size()!=faceCounts[axis]||
					request.beginningMomentumKGPerM2S[axis].size()!=faceCounts[axis] )
					return Fail(error,"production resident force face shape is invalid");
				for( std::size_t face=0u;face<faceCounts[axis];++face ) {
					const float density=request.faceDensityKGPerM3[axis][face];
					const float momentum=request.beginningMomentumKGPerM2S[axis][face];
					if( !(density>0.0f)||!std::isfinite(density)||!std::isfinite(momentum) )
						return Fail(error,"production resident force face state is invalid");
					packedDensity[offset+face]=density;packedMomentum[offset+face]=momentum;
				}
				if( request.boundary[2u*axis]==FireProductionProjectionPeriodic ) {
					const std::size_t plane=faceCounts[axis]/(axis==0u?shape.nx+1u:
						(axis==1u?shape.ny+1u:shape.nz+1u));
					const std::size_t extent=axis==0u?shape.nx:(axis==1u?shape.ny:shape.nz);
					for( std::size_t line=0u;line<plane;++line ) {
						const std::size_t low=axis==0u?line*(extent+1u):
							(axis==1u?(line/shape.nx)*(extent+1u)*shape.nx+line%shape.nx:line);
						const std::size_t high=axis==0u?low+extent:
							(axis==1u?low+extent*shape.nx:low+extent*shape.nx*shape.ny);
						if( std::memcmp(&request.faceDensityKGPerM3[axis][low],
							&request.faceDensityKGPerM3[axis][high],sizeof(float))!=0||
							std::memcmp(&request.beginningMomentumKGPerM2S[axis][low],
								&request.beginningMomentumKGPerM2S[axis][high],sizeof(float))!=0 )
							return Fail(error,"production resident force periodic seam differs");
					}
				}
				offset+=faceCounts[axis];
			}
			ForceContext& context=Context();if( !context.Valid() ) return Fail(error,context.error.c_str());
			@autoreleasepool {
				const std::size_t cellBytes=cells*sizeof(float),faceBytes=faces*sizeof(float);
				const std::size_t padded=NextPowerOfTwo(std::max(cells,faces));
				ForceParameters p={static_cast<std::uint32_t>(shape.nx),static_cast<std::uint32_t>(shape.ny),
					static_cast<std::uint32_t>(shape.nz),0u,shape.cellWidthM,request.timeStepS,
					request.ambientDensityKGPerM3,request.vremanCoefficient,
					{request.gravityMPerS2[0],request.gravityMPerS2[1],request.gravityMPerS2[2]},0.0f,
					{}, {0u,static_cast<std::uint32_t>(faceCounts[0]),
						static_cast<std::uint32_t>(faceCounts[0]+faceCounts[1])},
					static_cast<std::uint32_t>(faces)};
				for( unsigned int side=0u;side<6u;++side ) p.boundary[side]=request.boundary[side];
				std::uint64_t trackedAllocationBytes=0u,trackedAllocationCount=0u;
				auto recordAllocation=[&](id<MTLBuffer> buffer) -> id<MTLBuffer> {
					if( !buffer ) return nil;
					const std::uint64_t bytes=[buffer allocatedSize];
					if( trackedAllocationCount==std::numeric_limits<std::uint64_t>::max()||
						trackedAllocationBytes>std::numeric_limits<std::uint64_t>::max()-bytes )
						return nil;
					++trackedAllocationCount;trackedAllocationBytes+=bytes;return buffer;};
				auto sharedBytes=[&](const void* source,std::size_t bytes) -> id<MTLBuffer> {
					id<MTLBuffer> buffer=[context.device newBufferWithBytes:source length:bytes
						options:MTLResourceStorageModeShared];
					return buffer&&[buffer storageMode]==MTLStorageModeShared?
						recordAllocation(buffer):nil;};
				auto privateBuffer=[&](std::size_t bytes) -> id<MTLBuffer> {
					id<MTLBuffer> buffer=[context.device newBufferWithLength:bytes
						options:MTLResourceStorageModePrivate];
					return buffer&&[buffer storageMode]==MTLStorageModePrivate?
						recordAllocation(buffer):nil;};
				auto sharedBuffer=[&](std::size_t bytes) -> id<MTLBuffer> {
					id<MTLBuffer> buffer=[context.device newBufferWithLength:bytes
						options:MTLResourceStorageModeShared];
					return buffer&&[buffer storageMode]==MTLStorageModeShared?
						recordAllocation(buffer):nil;};
				id<MTLBuffer> rhoUpload=sharedBytes(request.cellGasDensityKGPerM3.data(),cellBytes);
				id<MTLBuffer> nuUpload=sharedBytes(request.molecularKinematicViscosityM2PerS.data(),cellBytes);
				id<MTLBuffer> faceRhoUpload=sharedBytes(packedDensity.data(),faceBytes);
				id<MTLBuffer> momentumUpload=sharedBytes(packedMomentum.data(),faceBytes);
				id<MTLBuffer> projectionTargetUpload=projectionTarget?
					sharedBytes(projectionTarget->data(),cellBytes):nil;
				id<MTLBuffer> rho=privateBuffer(cellBytes),nu=privateBuffer(cellBytes);
				id<MTLBuffer> faceRho=privateBuffer(faceBytes),momentum=privateBuffer(faceBytes);
				id<MTLBuffer> faceVelocity=privateBuffer(faceBytes),cellVelocity=privateBuffer(3u*cellBytes);
				id<MTLBuffer> stress=InjectedFailure("resident-local-shared")?
					sharedBuffer(9u*cellBytes):privateBuffer(9u*cellBytes);
				id<MTLBuffer> eddy=privateBuffer(cellBytes);
				id<MTLBuffer> mu=privateBuffer(cellBytes),viscous=privateBuffer(faceBytes);
				id<MTLBuffer> beginningViscous=privateBuffer(faceBytes);
				id<MTLBuffer> gravity=privateBuffer(faceBytes),scratchA=privateBuffer(padded*sizeof(float));
				id<MTLBuffer> scratchB=privateBuffer(padded*sizeof(float)),maxima=privateBuffer(2u*sizeof(float));
				id<MTLBuffer> lambda=sharedBuffer(3u*sizeof(float)),parameters=sharedBytes(&p,sizeof(p));
				id<MTLBuffer> privateProjectionTarget=projectionTarget?privateBuffer(cellBytes):nil;
				id<MTLBuffer> snapshots=nil;
				const id<MTLBuffer> uploads[]={rhoUpload,nuUpload,faceRhoUpload,momentumUpload};
				const id<MTLBuffer> privateRequired[]={rho,nu,faceRho,momentum,faceVelocity,
					cellVelocity,stress,eddy,mu,viscous,beginningViscous,gravity,scratchA,scratchB,maxima};
				const id<MTLBuffer> sharedRequired[]={lambda,parameters};
				for( id<MTLBuffer> buffer : uploads ) if( !buffer||
					[buffer storageMode]!=MTLStorageModeShared )
					return Fail(error,"production resident force upload allocation failed");
				if( projectionTarget&&(!projectionTargetUpload||!privateProjectionTarget) )
					return Fail(error,"production resident projection target allocation failed");
				if( projectionTarget&&([projectionTargetUpload storageMode]!=MTLStorageModeShared||
					[privateProjectionTarget storageMode]!=MTLStorageModePrivate) )
					return Fail(error,"production resident projection target storage mode changed");
				for( id<MTLBuffer> buffer : privateRequired ) if( !buffer||
					[buffer storageMode]!=MTLStorageModePrivate )
					return Fail(error,"production resident force buffer allocation failed");
				for( id<MTLBuffer> buffer : sharedRequired ) if( !buffer||
					[buffer storageMode]!=MTLStorageModeShared )
					return Fail(error,"production resident force diagnostic storage mode changed");
				const std::uint64_t hostBytes=(4u*cells+7u*faces)*sizeof(float);
				auto allocatedSum=[&](const id<MTLBuffer>* buffers,std::size_t count,
					std::uint64_t& sum)->bool {sum=0u;for( std::size_t i=0u;i<count;++i ) {
					const std::uint64_t allocation=[buffers[i] allocatedSize];
					if( sum>std::numeric_limits<std::uint64_t>::max()-allocation ) return false;
					sum+=allocation;}return true;};
				std::uint64_t residentBytes=0u,uploadBytes=0u;
				if( !allocatedSum(privateRequired,sizeof(privateRequired)/sizeof(privateRequired[0]),
					residentBytes)||!allocatedSum(uploads,sizeof(uploads)/sizeof(uploads[0]),uploadBytes) )
					return Fail(error,"production resident force allocation overflowed");
				for( id<MTLBuffer> buffer:sharedRequired ) {
					const std::uint64_t allocation=[buffer allocatedSize];
					if( residentBytes>std::numeric_limits<std::uint64_t>::max()-allocation )
						return Fail(error,"production resident force allocation overflowed");
					residentBytes+=allocation;
				}
				if( projectionTarget ) {
					const std::uint64_t residentAllocation=[privateProjectionTarget allocatedSize];
					const std::uint64_t uploadAllocation=[projectionTargetUpload allocatedSize];
					if( residentBytes>std::numeric_limits<std::uint64_t>::max()-residentAllocation||
						uploadBytes>std::numeric_limits<std::uint64_t>::max()-uploadAllocation )
						return Fail(error,"production resident projection target allocation overflowed");
					residentBytes+=residentAllocation;uploadBytes+=uploadAllocation;
				}
				if( hostBytes>std::numeric_limits<std::uint64_t>::max()-residentBytes||
					hostBytes+residentBytes>std::numeric_limits<std::uint64_t>::max()-uploadBytes )
					return Fail(error,"production resident force allocation overflowed");
				std::uint64_t actual=hostBytes+residentBytes+uploadBytes;
				if( actual>certifiedBytes||actual>(UINT64_C(1)<<31u) )
					return Fail(error,"production resident force preflight allocation exceeds certificate");
				observed.certifiedWorkingSetBytes=certifiedBytes;
				observed.actualMetalAllocationBytes=actual;
				auto begin=[&](id<MTLCommandBuffer> command,id<MTLComputePipelineState> pipeline,
					const id<MTLBuffer>* buffers,std::size_t bufferCount,std::size_t count,
					const void* bytes=0,std::size_t byteCount=0u)->bool {
					id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];if( !encoder ) return false;
					for( std::size_t i=0u;i<bufferCount;++i ) [encoder setBuffer:buffers[i] offset:0 atIndex:i];
					if( bytes&&byteCount ) [encoder setBytes:bytes length:byteCount atIndex:bufferCount];
					Dispatch(encoder,pipeline,count);[encoder endEncoding];return true;};
				id<MTLCommandBuffer> preflight=[context.queue commandBuffer];if( !preflight )
					return Fail(error,"production resident force preflight command allocation failed");
				id<MTLBlitCommandEncoder> upload=[preflight blitCommandEncoder];if( !upload )
					return Fail(error,"production resident force upload encoder failed");
				{
					ResidentForceTransferScope transferScope(ResidentForceUploadTransfer);
					CopyResidentForceBuffer(upload,rhoUpload,0u,rho,0u,cellBytes);
					CopyResidentForceBuffer(upload,nuUpload,0u,nu,0u,cellBytes);
					CopyResidentForceBuffer(upload,faceRhoUpload,0u,faceRho,0u,faceBytes);
					CopyResidentForceBuffer(upload,momentumUpload,0u,momentum,0u,faceBytes);
					if( projectionTarget ) CopyResidentForceBuffer(upload,projectionTargetUpload,0u,
						privateProjectionTarget,0u,cellBytes);
				}
				[upload endEncoding];
				const id<MTLBuffer> setupBuffers[]={faceRho,momentum,faceVelocity,parameters};
				const id<MTLBuffer> cellBuffers[]={faceVelocity,cellVelocity,parameters};
				const id<MTLBuffer> stressBuffers[]={cellVelocity,rho,nu,eddy,mu,stress,parameters};
				const id<MTLBuffer> forceBuffers[]={faceRho,stress,viscous,gravity,parameters};
				if( !begin(preflight,context.setupFace,setupBuffers,4u,faces)||
					!begin(preflight,context.cellVelocity,cellBuffers,3u,cells)||
					!begin(preflight,context.stress,stressBuffers,7u,cells)||
					!begin(preflight,context.faceForce,forceBuffers,5u,faces) )
					return Fail(error,"production resident force preflight encoder failed");
				const std::uint32_t beginningSlot=0u;
				const id<MTLBuffer> beginningBuffers[]={viscous,beginningViscous,parameters};
				if( !begin(preflight,context.snapshot,beginningBuffers,3u,faces,
					&beginningSlot,sizeof(beginningSlot)) )
					return Fail(error,"production resident force beginning-rate copy failed");
				auto reduce=[&](id<MTLBuffer> input,id<MTLBuffer> other,
					std::size_t count,std::uint32_t slot)->bool {
					id<MTLBuffer> source=input,target=other;std::uint32_t sourceCount=static_cast<std::uint32_t>(count);
					while( sourceCount>1u ) {const std::uint32_t targetCount=(sourceCount+1u)/2u;
						const id<MTLBuffer> pair[]={source,target};
						if( !begin(preflight,context.reduceMax,pair,2u,targetCount,&sourceCount,sizeof(sourceCount)) ) return false;
						std::swap(source,target);sourceCount=targetCount;}
					const id<MTLBuffer> store[]={source,maxima};
					return begin(preflight,context.storeMaximum,store,2u,1u,&slot,sizeof(slot));};
				const id<MTLBuffer> muPrepare[]={mu,scratchA,parameters};
				if( !begin(preflight,context.prepareMu,muPrepare,3u,padded)||!reduce(scratchA,scratchB,padded,0u) )
					return Fail(error,"production resident force viscosity reduction failed");
				const id<MTLBuffer> rhoPrepare[]={faceRho,scratchA,parameters};
				if( !begin(preflight,context.prepareInverseDensity,rhoPrepare,3u,padded)||
					!reduce(scratchA,scratchB,padded,1u) )
					return Fail(error,"production resident force density reduction failed");
				const id<MTLBuffer> lambdaBuffers[]={maxima,lambda,parameters};
				if( !begin(preflight,context.buildLambda,lambdaBuffers,3u,1u) )
					return Fail(error,"production resident force lambda encoder failed");
				CommitResidentForceCommand(preflight);[preflight waitUntilCompleted];
				if( [preflight status]!=MTLCommandBufferStatusCompleted )
					return Fail(error,"production resident force preflight command failed");
				observed.preflightDeviceElapsedMS=([preflight GPUEndTime]-[preflight GPUStartTime])*1000.0;
				observed.deviceStartTimeS=[preflight GPUStartTime];
				observed.outwardLambdaPerS=*static_cast<const float*>(ResidentForceBufferContents(
					lambda,ResidentForceScalarAccess));
				observed.scalarDiagnosticTransferCount=1u;
				rhoUpload=nil;nuUpload=nil;faceRhoUpload=nil;momentumUpload=nil;
				projectionTargetUpload=nil;
				markPhase("production resident force schedule selection failed");
				if(!(observed.outwardLambdaPerS>=0.0f)||
					!std::isfinite(observed.outwardLambdaPerS)){
					const float* diagnostic=static_cast<const float*>(ResidentForceBufferContents(
						lambda,ResidentForceScalarAccess));
					const std::string message="production resident force outward lambda is invalid: max_mu="+
						std::to_string(diagnostic[1])+" max_inverse_density="+
						std::to_string(diagnostic[2]);
					return Fail(error,message.c_str());
				}
				if(!SelectFireProductionViscousSchedule(request.timeStepS,
					observed.outwardLambdaPerS,selectedSchedule,error))return false;
				if( captureIntermediateStates ) {snapshots=InjectedFailure("resident-snapshot")?
					nil:privateBuffer(8u*faceBytes);
					if( !snapshots ) return Fail(error,"production resident force snapshot allocation failed");
					const std::uint64_t snapshotBytes=[snapshots allocatedSize];
					if( hostBytes+residentBytes>std::numeric_limits<std::uint64_t>::max()-snapshotBytes )
						return Fail(error,"production resident force allocation overflowed");
					actual=std::max(actual,hostBytes+residentBytes+snapshotBytes);}
				if( actual>certifiedBytes||actual>(UINT64_C(1)<<31u) )
					return Fail(error,"production resident force advance allocation exceeds certificate");
				markPhase("production resident force advance failed");
				id<MTLCommandBuffer> advance=InjectedFailure("resident-command-allocation")?
					nil:[context.queue commandBuffer];if( !advance )
					return Fail(error,"production resident force advance command allocation failed");
				for( std::uint32_t substep=0u;substep<selectedSchedule.substepCount;++substep ) {
					if( !begin(advance,context.setupFace,setupBuffers,4u,faces)||
						!begin(advance,context.cellVelocity,cellBuffers,3u,cells) )
						return Fail(error,"production resident force substep velocity encoder failed");
					const id<MTLBuffer> fixedStressBuffers[]={cellVelocity,mu,stress,parameters};
					if( !begin(advance,context.fixedStress,fixedStressBuffers,4u,cells)||
						!begin(advance,context.faceForce,forceBuffers,5u,faces) )
						return Fail(error,"production resident force substep operator encoder failed");
					const id<MTLBuffer> updateBuffers[]={momentum,viscous,parameters};
					if( !begin(advance,context.updateViscous,updateBuffers,3u,faces,
						&selectedSchedule.substepTimeS,sizeof(float)) )
						return Fail(error,"production resident force substep update encoder failed");
					if( captureIntermediateStates ) {const id<MTLBuffer> snapshotBuffers[]={momentum,snapshots,parameters};
						if( !begin(advance,context.snapshot,snapshotBuffers,3u,faces,&substep,sizeof(substep)) )
							return Fail(error,"production resident force snapshot encoder failed");}
				}
				const id<MTLBuffer> gravityBuffers[]={momentum,gravity,parameters};
				if( !begin(advance,context.addGravity,gravityBuffers,3u,faces) )
					return Fail(error,"production resident force gravity encoder failed");
				id<MTLBuffer> injectedInterstageStage=nil;
				if( InjectedFailure("resident-interstage-transfer") ) {
					injectedInterstageStage=sharedBuffer(faceBytes);
					id<MTLBlitCommandEncoder> injectedBlit=injectedInterstageStage?
						[advance blitCommandEncoder]:nil;
					if( !injectedBlit ) return Fail(error,
						"production resident force injected staging allocation failed");
					CopyResidentForceBuffer(injectedBlit,momentum,0u,injectedInterstageStage,0u,
						faceBytes);
					[injectedBlit endEncoding];
				}
				id<MTLBuffer> injectedInterstageUpload=nil;
				if( InjectedFailure("resident-interstage-upload") ) {
					injectedInterstageUpload=sharedBuffer(faceBytes);
					id<MTLBlitCommandEncoder> injectedBlit=injectedInterstageUpload?
						[advance blitCommandEncoder]:nil;
					if( !injectedBlit ) return Fail(error,
						"production resident force injected upload allocation failed");
					CopyResidentForceBuffer(injectedBlit,injectedInterstageUpload,0u,momentum,0u,
						faceBytes);
					[injectedBlit endEncoding];
				}
				CommitResidentForceCommand(advance);[advance waitUntilCompleted];
				if( InjectedFailure("resident-command")||
					[advance status]!=MTLCommandBufferStatusCompleted )
					return Fail(error,"production resident force advance command failed");
				observed.advanceDeviceElapsedMS=([advance GPUEndTime]-[advance GPUStartTime])*1000.0;
				observed.deviceEndTimeS=[advance GPUEndTime];
				if( residentForceCommandCommitCount<beginningCommandCommits||
					residentForceInterstageFullGridReadCount<beginningInterstageReads )
					return Fail(error,"production resident force audit counter regressed");
				observed.commandCommitCount=static_cast<std::uint32_t>(
					residentForceCommandCommitCount-beginningCommandCommits);
				observed.substepLoopDeviceToHostTransferCount=static_cast<std::uint32_t>(
					residentForceInterstageFullGridReadCount-beginningInterstageReads);
				if( observed.substepLoopDeviceToHostTransferCount!=0u )
					return Fail(error,"production resident force interstage transfer observed");
				markPhase("production resident force-state publication failed");
				if( residentState ) {
					if( trackedAllocationCount!=21u||
						residentBytes>std::numeric_limits<std::uint64_t>::max()-uploadBytes||
						trackedAllocationBytes!=residentBytes+uploadBytes )
						return Fail(error,"production resident force-state allocation topology changed");
					FireProductionMetalFrozenForceResidentState computed;
					computed.cellGasDensityKGPerM3=rho;
					computed.packedFaceDensityKGPerM3=faceRho;
					computed.packedMomentumKGPerM2S=momentum;
					std::size_t faceOffset=0u;
					for( unsigned int axis=0u;axis<3u;++axis ) {
						computed.faceByteOffset[axis]=faceOffset*sizeof(float);
						faceOffset+=faceCounts[axis];
					}
					computed.schedule=selectedSchedule;computed.diagnostics=observed;
					computed.diagnostics.terminalStagingCount=0u;
					computed.diagnostics.actualMetalAllocationBytes=actual;
					*residentState=std::move(computed);
					diagnostics=observed;if( error ) error->clear();forceCompleted=true;return true;
				}
				if( composedResult ) {
					if( trackedAllocationCount!=23u||
						residentBytes>std::numeric_limits<std::uint64_t>::max()-uploadBytes||
						trackedAllocationBytes!=residentBytes+uploadBytes )
						return Fail(error,"production resident force allocation topology changed");
					FireProductionProjectionRequest projectionRequest;
					projectionRequest.shape=request.shape;projectionRequest.timeStepS=request.timeStepS;
					projectionRequest.ambientDensityKGPerM3=request.ambientDensityKGPerM3;
					projectionRequest.boundary=request.boundary;
					projectionRequest.gasDensityKGPerM3=request.cellGasDensityKGPerM3;
					projectionRequest.provisionalMomentumKGPerM2S=request.beginningMomentumKGPerM2S;
					projectionRequest.divergenceTargetPerS=*projectionTarget;
					FireProductionMetalProjectionResidentInput residentProjection;
					residentProjection.gasDensityKGPerM3=rho;
					residentProjection.divergenceTargetPerS=privateProjectionTarget;
					std::size_t momentumOffset=0u;
					for( unsigned int axis=0u;axis<3u;++axis ) {
						residentProjection.provisionalMomentumKGPerM2S[axis]=momentum;
						residentProjection.provisionalMomentumByteOffset[axis]=
							momentumOffset*sizeof(float);
						momentumOffset+=faceCounts[axis];
					}
					id<MTLBuffer> injectedResidentInput=nil;
					if( InjectedFailure("resident-shared-input") ) {
						injectedResidentInput=sharedBuffer(cellBytes);
						residentProjection.divergenceTargetPerS=injectedResidentInput;
					} else if( InjectedFailure("resident-short-input") ) {
						injectedResidentInput=privateBuffer(sizeof(float));
						residentProjection.divergenceTargetPerS=injectedResidentInput;
					} else if( InjectedFailure("resident-alias-input") ) {
						residentProjection.provisionalMomentumKGPerM2S[1]=faceRho;
					}
					FireProductionProjectionResult projected;
					if( !ProjectFireProductionMetalResident(projectionRequest,residentProjection,
						projected,error) ) return false;
					if( certifiedBytes>std::numeric_limits<std::uint64_t>::max()-
						projected.residentCertifiedWorkingSetBytes||
						actual>std::numeric_limits<std::uint64_t>::max()-
						projected.residentActualMetalAllocationBytes )
						return Fail(error,"production resident force-projection accounting overflowed");
					FireProductionResidentForceProjectionResult computed;
					computed.forceSchedule=selectedSchedule;computed.projection=std::move(projected);
					computed.forceDiagnostics=observed;
					computed.forceToProjectionDeviceToHostTransferCount=
						observed.substepLoopDeviceToHostTransferCount;
					computed.residentProjectionInvocationCount=
						computed.projection.residentProjectionInvocationCount;
					computed.combinedCertifiedWorkingSetBytes=combinedPreflightCertificate;
					computed.combinedActualMetalAllocationBytes=actual+
						computed.projection.residentActualMetalAllocationBytes;
					if( computed.combinedActualMetalAllocationBytes>
						computed.combinedCertifiedWorkingSetBytes||
						computed.combinedActualMetalAllocationBytes>(UINT64_C(1)<<31u) )
						return Fail(error,"production resident force-projection working set exceeds two GiB");
					if( trackedAllocationCount!=23u||
						residentBytes>std::numeric_limits<std::uint64_t>::max()-uploadBytes||
						trackedAllocationBytes!=residentBytes+uploadBytes )
						return Fail(error,"production resident force allocation topology changed");
					*composedResult=std::move(computed);diagnostics=observed;
					if( error ) error->clear();forceCompleted=true;return true;
				}
				id<MTLBuffer> stageEddy=InjectedFailure("resident-staging")?
					nil:sharedBuffer(cellBytes),stageMu=sharedBuffer(cellBytes);
				id<MTLBuffer> stageViscous=sharedBuffer(faceBytes),stageGravity=sharedBuffer(faceBytes);
				id<MTLBuffer> stageMomentum=sharedBuffer(faceBytes);
				id<MTLBuffer> stageSnapshots=captureIntermediateStates?sharedBuffer(8u*faceBytes):nil;
				const id<MTLBuffer> stages[]={stageEddy,stageMu,stageViscous,stageGravity,stageMomentum};
				for( id<MTLBuffer> buffer : stages ) if( !buffer||
					[buffer storageMode]!=MTLStorageModeShared )
					return Fail(error,"production resident force staging allocation failed");
				if( captureIntermediateStates&&(!stageSnapshots||
					[stageSnapshots storageMode]!=MTLStorageModeShared||
					[snapshots storageMode]!=MTLStorageModePrivate) )
					return Fail(error,"production resident force snapshot staging allocation failed");
				std::uint64_t stageBytes=0u;
				if( !allocatedSum(stages,sizeof(stages)/sizeof(stages[0]),stageBytes) )
					return Fail(error,"production resident force staging allocation overflowed");
				if( captureIntermediateStates ) {const std::uint64_t allocation=[stageSnapshots allocatedSize];
					if( stageBytes>std::numeric_limits<std::uint64_t>::max()-allocation )
						return Fail(error,"production resident force staging allocation overflowed");
					stageBytes+=allocation;}
				const std::uint64_t snapshotBytes=captureIntermediateStates?[snapshots allocatedSize]:0u;
				if( hostBytes+residentBytes>std::numeric_limits<std::uint64_t>::max()-snapshotBytes||
					hostBytes+residentBytes+snapshotBytes>std::numeric_limits<std::uint64_t>::max()-stageBytes )
					return Fail(error,"production resident force staging allocation overflowed");
				actual=std::max(actual,hostBytes+residentBytes+snapshotBytes+stageBytes);
				if( actual>certifiedBytes||actual>(UINT64_C(1)<<31u) )
					return Fail(error,"production resident force staging allocation exceeds certificate");
				const std::uint64_t expectedAllocationCount=captureIntermediateStates?28u:26u;
				if( residentBytes>std::numeric_limits<std::uint64_t>::max()-uploadBytes||
					residentBytes+uploadBytes>std::numeric_limits<std::uint64_t>::max()-snapshotBytes||
					residentBytes+uploadBytes+snapshotBytes>
						std::numeric_limits<std::uint64_t>::max()-stageBytes||
					trackedAllocationCount!=expectedAllocationCount||
					trackedAllocationBytes!=residentBytes+uploadBytes+snapshotBytes+stageBytes )
					return Fail(error,"production resident force allocation topology changed");
				id<MTLCommandBuffer> staging=InjectedFailure("resident-staging-command")?
					nil:[context.queue commandBuffer];if( !staging )
					return Fail(error,"production resident force staging command allocation failed");
				id<MTLBlitCommandEncoder> blit=[staging blitCommandEncoder];if( !blit )
					return Fail(error,"production resident force terminal staging encoder failed");
				{
					ResidentForceTransferScope transferScope(ResidentForceTerminalTransfer);
					CopyResidentForceBuffer(blit,eddy,0u,stageEddy,0u,cellBytes);
					CopyResidentForceBuffer(blit,mu,0u,stageMu,0u,cellBytes);
					CopyResidentForceBuffer(blit,beginningViscous,0u,stageViscous,0u,faceBytes);
					CopyResidentForceBuffer(blit,gravity,0u,stageGravity,0u,faceBytes);
					CopyResidentForceBuffer(blit,momentum,0u,stageMomentum,0u,faceBytes);
					if( captureIntermediateStates ) CopyResidentForceBuffer(blit,snapshots,0u,
						stageSnapshots,0u,8u*faceBytes);
				}
				[blit endEncoding];CommitResidentForceCommand(staging);[staging waitUntilCompleted];
				observed.terminalStagingCount=1u;
				if( [staging status]!=MTLCommandBufferStatusCompleted )
					return Fail(error,"production resident force staging command failed");
				observed.commandCommitCount=static_cast<std::uint32_t>(
					residentForceCommandCommitCount-beginningCommandCommits);
				observed.actualMetalAllocationBytes=actual;
				FireProductionFrozenForceAdvanceResult computed;computed.schedule=selectedSchedule;
				const float* eddyValues=static_cast<const float*>(ResidentForceBufferContents(
					stageEddy,ResidentForceTerminalAccess));
				const float* muValues=static_cast<const float*>(ResidentForceBufferContents(
					stageMu,ResidentForceTerminalAccess));
				computed.frozenFields.eddyKinematicViscosityM2PerS.assign(eddyValues,eddyValues+cells);
				computed.frozenFields.effectiveDynamicViscosityPaS.assign(muValues,muValues+cells);
				const float* viscousValues=static_cast<const float*>(ResidentForceBufferContents(
					stageViscous,ResidentForceTerminalAccess));
				const float* gravityValues=static_cast<const float*>(ResidentForceBufferContents(
					stageGravity,ResidentForceTerminalAccess));
				const float* momentumValues=static_cast<const float*>(ResidentForceBufferContents(
					stageMomentum,ResidentForceTerminalAccess));
				offset=0u;for( unsigned int axis=0u;axis<3u;++axis ) {
					computed.frozenFields.beginningViscousMomentumRateKGPerM2S2[axis].assign(
						viscousValues+offset,viscousValues+offset+faceCounts[axis]);
					computed.frozenFields.gravityMomentumIncrementKGPerM2S[axis].assign(
						gravityValues+offset,gravityValues+offset+faceCounts[axis]);
					computed.momentumKGPerM2S[axis].assign(momentumValues+offset,
						momentumValues+offset+faceCounts[axis]);offset+=faceCounts[axis];
				}
				computed.executedViscousSubstepCount=selectedSchedule.substepCount;
				if( captureIntermediateStates ) {const float* values=static_cast<const float*>(
					ResidentForceBufferContents(stageSnapshots,ResidentForceTerminalAccess));
					computed.intermediateMomentumDigestCount=selectedSchedule.substepCount;
					for( std::uint32_t step=0u;step<selectedSchedule.substepCount;++step )
						computed.intermediateMomentumByteDigests[step]=
							PackedMomentumByteDigest(values+step*faces,faces);}
				if( !AllFinite(computed.frozenFields.eddyKinematicViscosityM2PerS)||
					!AllFinite(computed.frozenFields.effectiveDynamicViscosityPaS) )
					return Fail(error,"production resident force output is nonfinite");
				for( unsigned int axis=0u;axis<3u;++axis ) if( !AllFinite(
					computed.frozenFields.beginningViscousMomentumRateKGPerM2S2[axis])||!AllFinite(
					computed.frozenFields.gravityMomentumIncrementKGPerM2S[axis])||
					!AllFinite(computed.momentumKGPerM2S[axis]) )
					return Fail(error,"production resident force output is nonfinite");
				result=std::move(computed);diagnostics=observed;
			}
			if( error ) error->clear();forceCompleted=true;return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionFrozenForceAdvanceResult();
			diagnostics=FireProductionResidentForceDiagnostics();
			return Fail(error,"production resident force allocation failed");
		}
	}

	bool AdvanceFireProductionFrozenForceMetal(
		const FireProductionFrozenForceRequest& request,
		bool captureIntermediateStates,
		FireProductionFrozenForceAdvanceResult& result,
		FireProductionResidentForceDiagnostics& diagnostics,
		std::string* error )
	{
		return AdvanceFireProductionFrozenForceMetalImpl(request,captureIntermediateStates,
			0,0,0,result,diagnostics,error);
	}

	bool AdvanceFireProductionFrozenForceMetalResidentState(
		const FireProductionFrozenForceRequest& request,
		FireProductionMetalFrozenForceResidentState& state,
		std::string* error )
	{
		FireProductionFrozenForceAdvanceResult unpublished;
		FireProductionResidentForceDiagnostics diagnostics;
		return AdvanceFireProductionFrozenForceMetalImpl(request,false,0,0,&state,
			unpublished,diagnostics,error);
	}

	bool AdvanceFireProductionFrozenForceMetalResidentStateComparator(
		const FireProductionFrozenForceRequest& request,
		FireProductionFrozenForceAdvanceResult& result,
		FireProductionResidentForceDiagnostics& diagnostics,
		std::string* error )
	{
		result=FireProductionFrozenForceAdvanceResult();
		diagnostics=FireProductionResidentForceDiagnostics();
		try {
			FireProductionMetalFrozenForceResidentState state;
			if( !AdvanceFireProductionFrozenForceMetalResidentState(request,state,error) ) return false;
			ForceContext& context=Context();
			const std::size_t faces=FireProductionProjectionFaceCount(request.shape,0u)+
				FireProductionProjectionFaceCount(request.shape,1u)+
				FireProductionProjectionFaceCount(request.shape,2u);
			@autoreleasepool {
				id<MTLBuffer> stage=[context.device newBufferWithLength:faces*sizeof(float)
					options:MTLResourceStorageModeShared];
				id<MTLCommandBuffer> command=[context.queue commandBuffer];
				id<MTLBlitCommandEncoder> blit=command?[command blitCommandEncoder]:nil;
				if( !stage||!blit ) return Fail(error,
					"production resident force-state comparator staging allocation failed");
				{
					ResidentForceTransferScope transferScope(ResidentForceTerminalTransfer);
					CopyResidentForceBuffer(blit,state.packedMomentumKGPerM2S,0u,stage,0u,
						faces*sizeof(float));
				}
				[blit endEncoding];CommitResidentForceCommand(command);[command waitUntilCompleted];
				if( [command status]!=MTLCommandBufferStatusCompleted ) return Fail(error,
					"production resident force-state comparator staging command failed");
				const float* values=static_cast<const float*>(ResidentForceBufferContents(
					stage,ResidentForceTerminalAccess));
				FireProductionFrozenForceAdvanceResult computed;computed.schedule=state.schedule;
				std::size_t offset=0u;
				for( unsigned int axis=0u;axis<3u;++axis ) {
					const std::size_t count=FireProductionProjectionFaceCount(request.shape,axis);
					computed.momentumKGPerM2S[axis].assign(values+offset,values+offset+count);
					offset+=count;
				}
				computed.executedViscousSubstepCount=state.schedule.substepCount;
				result=std::move(computed);diagnostics=state.diagnostics;
			}
			if( error ) error->clear();return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionFrozenForceAdvanceResult();
			diagnostics=FireProductionResidentForceDiagnostics();
			return Fail(error,"production resident force-state comparator allocation failed");
		}
	}

	bool AdvanceFireProductionForceProjectionMetal(
		const FireProductionFrozenForceRequest& forceRequest,
		const std::vector<float>& divergenceTargetPerS,
		FireProductionResidentForceProjectionResult& result,
		std::string* error )
	{
		FireProductionFrozenForceAdvanceResult unpublishedForce;
		FireProductionResidentForceDiagnostics unpublishedDiagnostics;
		return AdvanceFireProductionFrozenForceMetalImpl(forceRequest,false,&divergenceTargetPerS,
			&result,0,unpublishedForce,unpublishedDiagnostics,error);
	}
}
