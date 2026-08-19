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
#include <cmath>
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
			std::uint32_t lowerBoundary;
			std::uint32_t upperBoundary;
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
struct Params { uint n; uint lines; uint comps; uint lowerBoundary; uint upperBoundary; float dx; float dt; };
inline uint value_index(constant Params& p,uint c,uint l,uint i){return (c*p.lines+l)*p.n+i;}
inline uint flux_index(constant Params& p,uint c,uint l,uint f){return (c*p.lines+l)*(p.n+1u)+f;}
inline float sample_value(device const float* q,device const float* u,device const float* ambient,
 constant Params& p,uint c,uint l,int i){
 if(p.lowerBoundary==0u&&p.upperBoundary==0u){int n=int(p.n);int w=i%n;if(w<0)w+=n;return q[value_index(p,c,l,uint(w))];}
 if(i<0){bool inflow=p.lowerBoundary==1u&&u[l*(p.n+1u)]>0.0f;
  return inflow?ambient[c]:q[value_index(p,c,l,0u)];}
 if(i>=int(p.n)){bool inflow=p.upperBoundary==1u&&u[l*(p.n+1u)+p.n]<0.0f;
  return inflow?ambient[c]:q[value_index(p,c,l,p.n-1u)];}
 return q[value_index(p,c,l,uint(i))];
}
inline void unlimited_edges(device const float* q,device const float* u,device const float* ambient,
 constant Params& p,uint c,uint l,uint cell,thread float& left,thread float& right){
 int i=int(cell);float im2=sample_value(q,u,ambient,p,c,l,i-2);
 float im1=sample_value(q,u,ambient,p,c,l,i-1);float center=sample_value(q,u,ambient,p,c,l,i);
 float ip1=sample_value(q,u,ambient,p,c,l,i+1);float ip2=sample_value(q,u,ambient,p,c,l,i+2);
 if(im2==center&&im1==center&&ip1==center&&ip2==center){left=center;right=center;return;}
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
inline float cell_interval(float center,float left,float right,float beginning,float end){
 if(left==center&&right==center)return (end-beginning)*center;
 float q6=6.0f*center-3.0f*(left+right),delta=end-beginning;
 return delta*(left+0.5f*(right-left+q6)*(beginning+end)-
  (q6/3.0f)*(beginning*beginning+beginning*end+end*end));
}
inline float cell_trailing(float center,float left,float right,float length){
 if(left==center&&right==center)return length*center;
 float q6=6.0f*center-3.0f*(left+right);
 return length*(right-0.5f*(right-left-q6)*length-(q6/3.0f)*length*length);
}
inline uint wrapped_cell(int cell,uint count){int n=int(count),wrapped=cell%n;
 if(wrapped<0)wrapped+=n;return uint(wrapped);}
inline float periodic_local_forward(device const float* q,device const float* left,
 device const float* right,constant Params& p,uint c,uint l,int beginningCell,
 float beginningFraction,float length){
 float remaining=length,result=0.0f,fraction=beginningFraction;int cell=beginningCell;
 while(remaining>0.0f){float span=min(remaining,1.0f-fraction);
  uint index=value_index(p,c,l,wrapped_cell(cell,p.n));
  result+=cell_interval(q[index],left[index],right[index],fraction,fraction+span);
  remaining-=span;fraction=0.0f;++cell;}
 return result;
}
inline float periodic_swept(device const float* q,device const float* left,
 device const float* right,device const float* prefix,constant Params& p,uint c,uint l,
 uint face,float courant){
 float count=float(p.n),magnitude=abs(courant),localLength=fmod(magnitude,count);
 float cycles=floor((magnitude-localLength)/count);uint base=(c*p.lines+l)*(p.n+1u);
 float result=cycles*prefix[base+p.n];
 if(courant>=0.0f){float whole=floor(localLength),fractional=localLength-whole;
  int wholeBeginning=int(face)-int(whole);
  if(fractional>0.0f){uint index=value_index(p,c,l,wrapped_cell(wholeBeginning-1,p.n));
   result+=cell_trailing(q[index],left[index],right[index],fractional);}
  result+=periodic_local_forward(q,left,right,p,c,l,wholeBeginning,0.0f,whole);return result;}
 result+=periodic_local_forward(q,left,right,p,c,l,int(face),0.0f,localLength);return -result;
}
inline float open_local_forward(device const float* q,device const float* left,
 device const float* right,constant Params& p,uint c,uint l,uint beginningCell,
 float beginningFraction,float length){
 float remaining=length,result=0.0f,fraction=beginningFraction;uint cell=beginningCell;
 while(remaining>0.0f&&cell<p.n){float span=min(remaining,1.0f-fraction);
  uint index=value_index(p,c,l,cell);
  result+=cell_interval(q[index],left[index],right[index],fraction,fraction+span);
  remaining-=span;fraction=0.0f;++cell;}
 return result;
}
inline float open_swept(device const float* q,device const float* left,device const float* right,
 device const float* ambient,constant Params& p,uint c,uint l,uint face,float courant,float velocity){
 float magnitude=abs(courant);float leftExtension=p.lowerBoundary==1u&&velocity>0.0f?ambient[c]:
  q[value_index(p,c,l,0u)];float rightExtension=p.upperBoundary==1u&&velocity<0.0f?ambient[c]:
  q[value_index(p,c,l,p.n-1u)];
 if(courant>=0.0f){float interiorLength=min(magnitude,float(face));
  float whole=floor(interiorLength),fractional=interiorLength-whole;
  uint wholeBeginning=face-uint(whole);float result=(magnitude-interiorLength)*leftExtension;
  if(fractional>0.0f){uint index=value_index(p,c,l,wholeBeginning-1u);
   result+=cell_trailing(q[index],left[index],right[index],fractional);}
  return result+open_local_forward(q,left,right,p,c,l,wholeBeginning,0.0f,whole);}
 float interiorLength=min(magnitude,float(p.n-face));
 return -(open_local_forward(q,left,right,p,c,l,face,0.0f,interiorLength)+
  (magnitude-interiorLength)*rightExtension);
}
kernel void face_flux(device const float* q [[buffer(0)]],device const float* u [[buffer(1)]],
 device const float* ambient [[buffer(2)]],device const float* left [[buffer(3)]],
 device const float* right [[buffer(4)]],device const float* prefix [[buffer(5)]],
 device float* flux [[buffer(6)]],constant Params& p [[buffer(7)]],
 uint gid [[thread_position_in_grid]]){
 bool periodic=p.lowerBoundary==0u&&p.upperBoundary==0u;
 uint faces=p.n+1u,activeFaces=periodic?p.n:faces,total=p.comps*p.lines*activeFaces;
 if(gid>=total)return;uint c=gid/(p.lines*activeFaces);uint rem=gid-c*p.lines*activeFaces;
 uint l=rem/activeFaces;uint f=rem-l*activeFaces;
 uint output=flux_index(p,c,l,f);
 if((f==0u&&p.lowerBoundary==2u)||(f==p.n&&p.upperBoundary==2u)){flux[output]=0.0f;return;}
 float velocity=u[l*faces+f],courant=p.dt*velocity/p.dx;
 float swept=periodic?periodic_swept(q,left,right,prefix,p,c,l,f,courant):
  open_swept(q,left,right,ambient,p,c,l,f,courant,velocity);
 flux[output]=p.dx*swept;
}
kernel void update_cells(device const float* q [[buffer(0)]],device float* flux [[buffer(1)]],
 device float* updated [[buffer(2)]],constant Params& p [[buffer(3)]],
 uint gid [[thread_position_in_grid]]){
 uint total=p.comps*p.lines*p.n;if(gid>=total)return;uint c=gid/(p.lines*p.n);
 uint rem=gid-c*p.lines*p.n;uint l=rem/p.n;uint cell=rem-l*p.n;
 uint base=flux_index(p,c,l,0u),right=cell+1u;
 if(p.lowerBoundary==0u&&p.upperBoundary==0u&&right==p.n){right=0u;flux[base+p.n]=flux[base];}
 updated[gid]=q[gid]-(flux[base+right]-flux[base+cell])/p.dx;
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

		bool AllFinite( const std::vector<float>& values )
		{
			return std::all_of(values.begin(),values.end(),
				[](float value){return std::isfinite(value);});
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
				static_cast<std::uint32_t>(request.asymmetricBoundaries ? request.lowerBoundary :
					request.boundary),
				static_cast<std::uint32_t>(request.asymmetricBoundaries ? request.upperBoundary :
					request.boundary),request.cellWidthM,request.timeStepS};
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
			const FireProductionRemapBoundary lower=request.asymmetricBoundaries ?
				request.lowerBoundary : request.boundary;
			const FireProductionRemapBoundary upper=request.asymmetricBoundaries ?
				request.upperBoundary : request.boundary;
			const std::size_t activeFluxCount=lower==FireProductionRemapPeriodic&&
				upper==FireProductionRemapPeriodic ?
				request.componentCount*request.lineCount*request.lineLength : fluxCount;
			Dispatch(encoder,context.flux,activeFluxCount);[encoder endEncoding];

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
		if( !AllFinite(result.updatedValues)||!AllFinite(result.faceFluxes)||
			!AllFinite(result.sharedLimiterAlpha)||!std::isfinite(result.deviceElapsedMS) ) {
			result=FireProductionRemapResult();
			if( structuredError )
				*structuredError="production fire remap produced nonfinite device output";
			return false;
		}
		if( structuredError ) structuredError->clear();
		return true;
	}
}
