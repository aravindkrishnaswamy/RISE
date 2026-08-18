//////////////////////////////////////////////////////////////////////
//
//  FireProductionAdvectionMac.mm - Metal conservative production PPM
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "FireProductionAdvection.h"

#include <algorithm>
#include <cstdint>

namespace RISE
{
	namespace
	{
		struct MetalParameters
		{
			std::uint32_t lineLength;
			std::uint32_t lineCount;
			std::uint32_t componentCount;
			std::uint32_t boundary;
			float cellWidthM;
			float timeStepS;
		};

		std::string MetalError( const char* prefix, NSError* error )
		{
			std::string result(prefix);
			if( error ) result += " ["+std::string([[error domain] UTF8String])+" "+
				std::to_string(static_cast<long>([error code]))+"] "+
				std::string([[error localizedDescription] UTF8String]);
			return result;
		}

		const char* RemapSource()
		{
			return R"METAL(
#include <metal_stdlib>
using namespace metal;
struct Params { uint n; uint lines; uint comps; uint boundary; float dx; float dt; };
inline uint value_index(constant Params& p,uint c,uint l,uint i){return (c*p.lines+l)*p.n+i;}
inline uint flux_index(constant Params& p,uint c,uint l,uint f){return (c*p.lines+l)*(p.n+1u)+f;}
inline float sample_value(device const float* q,device const float* u,device const float* ambient,
 constant Params& p,uint c,uint l,int i){
 if(p.boundary==0u){int n=int(p.n);int w=i%n;if(w<0)w+=n;return q[value_index(p,c,l,uint(w))];}
 if(i<0){bool inflow=p.boundary==1u&&u[l*(p.n+1u)]>0.0f;
  return inflow?ambient[c]:q[value_index(p,c,l,0u)];}
 if(i>=int(p.n)){bool inflow=p.boundary==1u&&u[l*(p.n+1u)+p.n]<0.0f;
  return inflow?ambient[c]:q[value_index(p,c,l,p.n-1u)];}
 return q[value_index(p,c,l,uint(i))];
}
inline void unlimited_edges(device const float* q,device const float* u,device const float* ambient,
 constant Params& p,uint c,uint l,uint cell,thread float& left,thread float& right){
 int i=int(cell);float im2=sample_value(q,u,ambient,p,c,l,i-2);
 float im1=sample_value(q,u,ambient,p,c,l,i-1);float center=sample_value(q,u,ambient,p,c,l,i);
 float ip1=sample_value(q,u,ambient,p,c,l,i+1);float ip2=sample_value(q,u,ambient,p,c,l,i+2);
 left=(7.0f*(im1+center)-(im2+ip1))/12.0f;
 right=(7.0f*(center+ip1)-(im1+ip2))/12.0f;
}
inline void deviation_range(float dl,float dr,thread float& mn,thread float& mx){
 mn=min(dl,dr);mx=max(dl,dr);float a=3.0f*(dl+dr);float b=-4.0f*dl-2.0f*dr;
 if(a!=0.0f){float s=-b/(2.0f*a);if(s>0.0f&&s<1.0f){float v=(a*s+b)*s+dl;mn=min(mn,v);mx=max(mx,v);}}
}
kernel void reconstruct(device const float* q [[buffer(0)]],device const float* u [[buffer(1)]],
 device const float* ambient [[buffer(2)]],device float* left [[buffer(3)]],
 device float* right [[buffer(4)]],device float* alphaOut [[buffer(5)]],
 constant Params& p [[buffer(6)]],uint gid [[thread_position_in_grid]]){
 if(gid>=p.n*p.lines)return;uint l=gid/p.n;uint cell=gid-l*p.n;float alpha=1.0f;
 for(uint c=0;c<p.comps;++c){uint index=value_index(p,c,l,cell);float ql,qr;
  unlimited_edges(q,u,ambient,p,c,l,cell,ql,qr);left[index]=ql;right[index]=qr;
  float center=q[index],mnDev,mxDev;deviation_range(ql-center,qr-center,mnDev,mxDev);
  float qm=sample_value(q,u,ambient,p,c,l,int(cell)-1);
  float qp=sample_value(q,u,ambient,p,c,l,int(cell)+1);
  float mn=min(center,min(qm,qp)),mx=max(center,max(qm,qp));
  if(mxDev>0.0f)alpha=min(alpha,(mx-center)/mxDev);
  if(mnDev<0.0f)alpha=min(alpha,(center-mn)/(-mnDev));
 }
 alpha=clamp(alpha,0.0f,1.0f);alphaOut[gid]=alpha;
 for(uint c=0;c<p.comps;++c){uint index=value_index(p,c,l,cell);float center=q[index];
  left[index]=center+alpha*(left[index]-center);right[index]=center+alpha*(right[index]-center);}
}
kernel void scan_lines(device const float* q [[buffer(0)]],device float* prefix [[buffer(1)]],
 constant Params& p [[buffer(2)]],threadgroup float* scratch [[threadgroup(0)]],
 uint3 threadPosition [[thread_position_in_threadgroup]],
 uint3 groupPosition [[threadgroup_position_in_grid]],uint3 threads [[threads_per_threadgroup]]){
 uint tid=threadPosition.x,group=groupPosition.x,width=threads.x;
 uint c=group/p.lines;uint l=group-c*p.lines;
 scratch[tid]=tid<p.n?q[value_index(p,c,l,tid)]:0.0f;threadgroup_barrier(mem_flags::mem_threadgroup);
 for(uint offset=1u;offset<width;offset<<=1u){uint index=(tid+1u)*offset*2u-1u;
  if(index<width)scratch[index]+=scratch[index-offset];threadgroup_barrier(mem_flags::mem_threadgroup);}
 uint base=(c*p.lines+l)*(p.n+1u);if(tid==0u){prefix[base+p.n]=scratch[width-1u];scratch[width-1u]=0.0f;}
 threadgroup_barrier(mem_flags::mem_threadgroup);
 for(uint offset=width>>1u;offset>0u;offset>>=1u){uint index=(tid+1u)*offset*2u-1u;
  if(index<width){float a=scratch[index-offset];scratch[index-offset]=scratch[index];scratch[index]+=a;}
  threadgroup_barrier(mem_flags::mem_threadgroup);}
 if(tid<p.n)prefix[base+tid]=scratch[tid];
}
inline float partial_integral(float center,float left,float right,float s){
 float q6=6.0f*center-3.0f*(left+right);float s2=s*s;
 return left*s+0.5f*(right-left+q6)*s2-(q6/3.0f)*s2*s;
}
inline float local_antiderivative(device const float* q,device const float* left,
 device const float* right,device const float* prefix,constant Params& p,uint c,uint l,float x){
 uint base=(c*p.lines+l)*(p.n+1u);if(x<=0.0f)return 0.0f;if(x>=float(p.n))return prefix[base+p.n];
 uint cell=min(p.n-1u,uint(floor(x)));float s=x-float(cell);uint index=value_index(p,c,l,cell);
 return prefix[base+cell]+partial_integral(q[index],left[index],right[index],s);
}
inline float periodic_forward(device const float* q,device const float* left,device const float* right,
 device const float* prefix,constant Params& p,uint c,uint l,float beginning,float end){
 float n=float(p.n),start=beginning-floor(beginning/n)*n;if(start>=n)start=0.0f;
 float remaining=end-beginning,first=min(remaining,n-start);
 float result=local_antiderivative(q,left,right,prefix,p,c,l,start+first)-
  local_antiderivative(q,left,right,prefix,p,c,l,start);remaining-=first;
 if(remaining>0.0f){uint base=(c*p.lines+l)*(p.n+1u);float cycles=floor(remaining/n);
  result+=cycles*prefix[base+p.n];remaining-=cycles*n;
  result+=local_antiderivative(q,left,right,prefix,p,c,l,remaining);}
 return result;
}
inline float periodic_interval(device const float* q,device const float* left,device const float* right,
 device const float* prefix,constant Params& p,uint c,uint l,float beginning,float end){
 return end>=beginning?periodic_forward(q,left,right,prefix,p,c,l,beginning,end):
  -periodic_forward(q,left,right,prefix,p,c,l,end,beginning);
}
inline float open_forward(device const float* q,device const float* left,device const float* right,
 device const float* prefix,device const float* ambient,constant Params& p,uint c,uint l,
 float beginning,float end,float velocity){
 float n=float(p.n);float leftExtension=p.boundary==1u&&velocity>0.0f?ambient[c]:
  q[value_index(p,c,l,0u)];float rightExtension=p.boundary==1u&&velocity<0.0f?ambient[c]:
  q[value_index(p,c,l,p.n-1u)];float result=0.0f;
 if(beginning<0.0f)result+=(min(end,0.0f)-beginning)*leftExtension;
 float interiorBeginning=max(beginning,0.0f),interiorEnd=min(end,n);
 if(interiorEnd>interiorBeginning)result+=
  local_antiderivative(q,left,right,prefix,p,c,l,interiorEnd)-
  local_antiderivative(q,left,right,prefix,p,c,l,interiorBeginning);
 if(end>n)result+=(end-max(beginning,n))*rightExtension;return result;
}
inline float open_interval(device const float* q,device const float* left,device const float* right,
 device const float* prefix,device const float* ambient,constant Params& p,uint c,uint l,
 float beginning,float end,float velocity){
 return end>=beginning?open_forward(q,left,right,prefix,ambient,p,c,l,beginning,end,velocity):
  -open_forward(q,left,right,prefix,ambient,p,c,l,end,beginning,velocity);
}
kernel void face_flux(device const float* q [[buffer(0)]],device const float* u [[buffer(1)]],
 device const float* ambient [[buffer(2)]],device const float* left [[buffer(3)]],
 device const float* right [[buffer(4)]],device const float* prefix [[buffer(5)]],
 device float* flux [[buffer(6)]],constant Params& p [[buffer(7)]],
 uint gid [[thread_position_in_grid]]){
 uint faces=p.n+1u,total=p.comps*p.lines*faces;if(gid>=total)return;
 uint c=gid/(p.lines*faces);uint rem=gid-c*p.lines*faces;uint l=rem/faces;uint f=rem-l*faces;
 if(p.boundary==2u&&(f==0u||f==p.n)){flux[gid]=0.0f;return;}
 float velocity=u[l*faces+f],arrival=float(f),departure=arrival-p.dt*velocity/p.dx;
 float swept=p.boundary==0u?periodic_interval(q,left,right,prefix,p,c,l,departure,arrival):
  open_interval(q,left,right,prefix,ambient,p,c,l,departure,arrival,velocity);
 flux[gid]=p.dx*swept;
}
kernel void update_cells(device const float* q [[buffer(0)]],device const float* flux [[buffer(1)]],
 device float* updated [[buffer(2)]],constant Params& p [[buffer(3)]],
 uint gid [[thread_position_in_grid]]){
 uint total=p.comps*p.lines*p.n;if(gid>=total)return;uint c=gid/(p.lines*p.n);
 uint rem=gid-c*p.lines*p.n;uint l=rem/p.n;uint cell=rem-l*p.n;
 updated[gid]=q[gid]-(flux[flux_index(p,c,l,cell+1u)]-flux[flux_index(p,c,l,cell)])/p.dx;
}
)METAL";
		}

		struct MetalRemapContext
		{
			id<MTLDevice> device;
			id<MTLCommandQueue> queue;
			id<MTLComputePipelineState> reconstruct;
			id<MTLComputePipelineState> scan;
			id<MTLComputePipelineState> flux;
			id<MTLComputePipelineState> update;
			std::string error;

			MetalRemapContext() : device(nil), queue(nil), reconstruct(nil), scan(nil),
				flux(nil), update(nil)
			{
				@autoreleasepool {
					device=MTLCreateSystemDefaultDevice();
					if( !device ) { error="production fire remap has no Metal device";return; }
					MTLCompileOptions* options=[[MTLCompileOptions alloc] init];
					if( @available(macOS 15.0,*) ) options.mathMode=MTLMathModeSafe;
					else { error="production fire remap requires Metal safe math mode";return; }
					NSError* metalError=nil;
					NSString* source=[NSString stringWithUTF8String:RemapSource()];
					id<MTLLibrary> library=[device newLibraryWithSource:source options:options error:&metalError];
					if( !library ) { error=MetalError("production fire remap library compilation failed",metalError);return; }
					auto makePipeline=[&](const char* name) -> id<MTLComputePipelineState> {
						id<MTLFunction> function=[library newFunctionWithName:
							[NSString stringWithUTF8String:name]];
						return function ? [device newComputePipelineStateWithFunction:function
							error:&metalError] : nil;
					};
					reconstruct=makePipeline("reconstruct");
					scan=makePipeline("scan_lines");
					flux=makePipeline("face_flux");
					update=makePipeline("update_cells");
					if( !reconstruct||!scan||!flux||!update ) {
						error=MetalError("production fire remap pipeline creation failed",metalError);
						return;
					}
					queue=[device newCommandQueue];
					if( !queue ) error="production fire remap command queue allocation failed";
				}
			}

			bool Valid() const
			{
				return device&&queue&&reconstruct&&scan&&flux&&update&&error.empty();
			}
		};

		MetalRemapContext& Context()
		{
			static MetalRemapContext context;
			return context;
		}

		std::size_t NextPowerOfTwo( std::size_t value )
		{
			std::size_t result=1u;
			while( result<value ) result<<=1u;
			return result;
		}

		void Dispatch( id<MTLComputeCommandEncoder> encoder,
			id<MTLComputePipelineState> pipeline, std::size_t count )
		{
			const std::size_t width=std::min<std::size_t>(256u,
				static_cast<std::size_t>([pipeline maxTotalThreadsPerThreadgroup]));
			[encoder setComputePipelineState:pipeline];
			[encoder dispatchThreads:MTLSizeMake(count,1,1)
				threadsPerThreadgroup:MTLSizeMake(width,1,1)];
		}
	}

	bool RemapFireProductionMetal( const FireProductionRemapRequest& request,
		FireProductionRemapResult& result, std::string* structuredError )
	{
		result=FireProductionRemapResult();
		if( !ValidateFireProductionRemapRequest(request,structuredError) ) return false;
		MetalRemapContext& context=Context();
		if( !context.Valid() ) {
			if( structuredError ) *structuredError=context.error;
			return false;
		}
		const std::size_t padded=NextPowerOfTwo(request.lineLength);
		if( padded>static_cast<std::size_t>([context.scan maxTotalThreadsPerThreadgroup]) ) {
			if( structuredError ) *structuredError="production fire remap line exceeds Metal scan width";
			return false;
		}

		@autoreleasepool {
			const std::size_t valueBytes=request.values.size()*sizeof(float);
			const std::size_t velocityBytes=request.faceVelocityMPerS.size()*sizeof(float);
			const std::size_t ambientBytes=request.ambientValues.size()*sizeof(float);
			const std::size_t alphaCount=request.lineCount*request.lineLength;
			const std::size_t fluxCount=request.componentCount*request.lineCount*
				(request.lineLength+1u);
			id<MTLBuffer> values=[context.device newBufferWithBytes:request.values.data()
				length:valueBytes options:MTLResourceStorageModeShared];
			id<MTLBuffer> velocity=[context.device newBufferWithBytes:request.faceVelocityMPerS.data()
				length:velocityBytes options:MTLResourceStorageModeShared];
			id<MTLBuffer> ambient=[context.device newBufferWithBytes:request.ambientValues.data()
				length:ambientBytes options:MTLResourceStorageModeShared];
			id<MTLBuffer> left=[context.device newBufferWithLength:valueBytes options:MTLResourceStorageModeShared];
			id<MTLBuffer> right=[context.device newBufferWithLength:valueBytes options:MTLResourceStorageModeShared];
			id<MTLBuffer> alpha=[context.device newBufferWithLength:alphaCount*sizeof(float)
				options:MTLResourceStorageModeShared];
			id<MTLBuffer> prefix=[context.device newBufferWithLength:fluxCount*sizeof(float)
				options:MTLResourceStorageModeShared];
			id<MTLBuffer> flux=[context.device newBufferWithLength:fluxCount*sizeof(float)
				options:MTLResourceStorageModeShared];
			id<MTLBuffer> updated=[context.device newBufferWithLength:valueBytes
				options:MTLResourceStorageModeShared];
			const MetalParameters parameters={static_cast<std::uint32_t>(request.lineLength),
				static_cast<std::uint32_t>(request.lineCount),
				static_cast<std::uint32_t>(request.componentCount),
				static_cast<std::uint32_t>(request.boundary),request.cellWidthM,request.timeStepS};
			id<MTLBuffer> parameterBuffer=[context.device newBufferWithBytes:&parameters
				length:sizeof(parameters) options:MTLResourceStorageModeShared];
			if( !values||!velocity||!ambient||!left||!right||!alpha||!prefix||!flux||
				!updated||!parameterBuffer ) {
				if( structuredError ) *structuredError="production fire remap buffer allocation failed";
				return false;
			}
			id<MTLCommandBuffer> command=[context.queue commandBuffer];
			if( !command ) {
				if( structuredError ) *structuredError="production fire remap command allocation failed";
				return false;
			}
			id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
			if( !encoder ) {
				if( structuredError ) *structuredError="production fire remap reconstruct encoder allocation failed";
				return false;
			}
			[encoder setBuffer:values offset:0 atIndex:0];[encoder setBuffer:velocity offset:0 atIndex:1];
			[encoder setBuffer:ambient offset:0 atIndex:2];[encoder setBuffer:left offset:0 atIndex:3];
			[encoder setBuffer:right offset:0 atIndex:4];[encoder setBuffer:alpha offset:0 atIndex:5];
			[encoder setBuffer:parameterBuffer offset:0 atIndex:6];
			Dispatch(encoder,context.reconstruct,request.lineCount*request.lineLength);[encoder endEncoding];

			encoder=[command computeCommandEncoder];
			if( !encoder ) {
				if( structuredError ) *structuredError="production fire remap scan encoder allocation failed";
				return false;
			}
			[encoder setComputePipelineState:context.scan];
			[encoder setBuffer:values offset:0 atIndex:0];[encoder setBuffer:prefix offset:0 atIndex:1];
			[encoder setBuffer:parameterBuffer offset:0 atIndex:2];
			[encoder setThreadgroupMemoryLength:padded*sizeof(float) atIndex:0];
			[encoder dispatchThreadgroups:MTLSizeMake(request.componentCount*request.lineCount,1,1)
				threadsPerThreadgroup:MTLSizeMake(padded,1,1)];[encoder endEncoding];

			encoder=[command computeCommandEncoder];
			if( !encoder ) {
				if( structuredError ) *structuredError="production fire remap flux encoder allocation failed";
				return false;
			}
			[encoder setBuffer:values offset:0 atIndex:0];[encoder setBuffer:velocity offset:0 atIndex:1];
			[encoder setBuffer:ambient offset:0 atIndex:2];[encoder setBuffer:left offset:0 atIndex:3];
			[encoder setBuffer:right offset:0 atIndex:4];[encoder setBuffer:prefix offset:0 atIndex:5];
			[encoder setBuffer:flux offset:0 atIndex:6];[encoder setBuffer:parameterBuffer offset:0 atIndex:7];
			Dispatch(encoder,context.flux,fluxCount);[encoder endEncoding];

			encoder=[command computeCommandEncoder];
			if( !encoder ) {
				if( structuredError ) *structuredError="production fire remap update encoder allocation failed";
				return false;
			}
			[encoder setBuffer:values offset:0 atIndex:0];[encoder setBuffer:flux offset:0 atIndex:1];
			[encoder setBuffer:updated offset:0 atIndex:2];[encoder setBuffer:parameterBuffer offset:0 atIndex:3];
			Dispatch(encoder,context.update,request.values.size());[encoder endEncoding];
			[command commit];[command waitUntilCompleted];
			if( [command status]!=MTLCommandBufferStatusCompleted ) {
				if( structuredError ) *structuredError=MetalError(
					"production fire remap command failed",[command error]);
				return false;
			}
			const float* updatedValues=static_cast<const float*>([updated contents]);
			const float* faceFluxes=static_cast<const float*>([flux contents]);
			const float* limiter=static_cast<const float*>([alpha contents]);
			result.updatedValues.assign(updatedValues,updatedValues+request.values.size());
			result.faceFluxes.assign(faceFluxes,faceFluxes+fluxCount);
			result.sharedLimiterAlpha.assign(limiter,limiter+alphaCount);
			result.deviceElapsedMS=([command GPUEndTime]-[command GPUStartTime])*1000.0;
		}
		if( structuredError ) structuredError->clear();
		return true;
	}
}
