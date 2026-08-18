//////////////////////////////////////////////////////////////////////
//
//  FireProductionComputeMac.mm - Metal production-fire capability
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "FireProductionCompute.h"

#include <cstring>

namespace RISE
{
	namespace
	{
		std::string UTF8String( NSString* value )
		{
			if( !value ) return std::string();
			const char* utf8=[value UTF8String];
			return utf8 ? std::string(utf8) : std::string();
		}

		std::string MetalError( const char* prefix, NSError* error )
		{
			std::string result(prefix);
			if( error ) {
				result += " [" + UTF8String([error domain]) + " ";
				result += std::to_string(static_cast<long>([error code])) + "]";
			}
			return result;
		}

		std::string DeviceFamily( id<MTLDevice> device )
		{
			if( @available(macOS 10.15,*) ) {
				for( int family=9; family>=1; --family ) {
					const MTLGPUFamily candidate=static_cast<MTLGPUFamily>(family);
					if( [device supportsFamily:candidate] )
						return "apple"+std::to_string(family);
				}
			}
			return "metal-family-unreported";
		}
	}

	bool QueryFireProductionComputeCapability(
		FireProductionComputeCapability& capability )
	{
		capability=FireProductionComputeCapability();
		capability.backend="metal";
		@autoreleasepool {
			id<MTLDevice> device=MTLCreateSystemDefaultDevice();
			if( !device ) {
				capability.structuredError="production fire compute capability unavailable: no Metal device";
				return true;
			}

			capability.deviceName=UTF8String([device name]);
			capability.registryId=static_cast<std::uint64_t>([device registryID]);
			capability.deviceFamily=DeviceFamily(device);
			if( @available(macOS 10.15,*) )
				capability.unifiedMemory=[device hasUnifiedMemory];

			static NSString* const source=
				@"#include <metal_stdlib>\n"
				 "using namespace metal;\n"
				 "kernel void rise_fire_identity(device const float* input [[buffer(0)]],"
				 " device float* output [[buffer(1)]], uint i [[thread_position_in_grid]])"
				 " { output[i]=input[i]; }\n";
			NSError* error=nil;
			MTLCompileOptions* options=[[MTLCompileOptions alloc] init];
			id<MTLLibrary> library=[device newLibraryWithSource:source options:options error:&error];
			if( !library ) {
				capability.structuredError=MetalError("production fire identity library compilation failed",error);
				return true;
			}
			id<MTLFunction> function=[library newFunctionWithName:@"rise_fire_identity"];
			if( !function ) {
				capability.structuredError="production fire identity function missing after compilation";
				return true;
			}
			id<MTLComputePipelineState> pipeline=
				[device newComputePipelineStateWithFunction:function error:&error];
			if( !pipeline ) {
				capability.structuredError=MetalError("production fire identity pipeline creation failed",error);
				return true;
			}
			capability.maximumThreadsPerThreadgroup=
				static_cast<std::size_t>([pipeline maxTotalThreadsPerThreadgroup]);

			const std::uint32_t inputBits[]={0x00000000u,0x80000000u,0x3f800000u,
				0xbf000000u,0x00800000u,0x7f7fffffu,0x3eaaaaabu,0x41200000u};
			id<MTLBuffer> input=[device newBufferWithBytes:inputBits length:sizeof(inputBits)
				options:MTLResourceStorageModeShared];
			id<MTLBuffer> output=[device newBufferWithLength:sizeof(inputBits)
				options:MTLResourceStorageModeShared];
			id<MTLCommandQueue> queue=[device newCommandQueue];
			if( !input || !output || !queue ) {
				capability.structuredError="production fire identity buffer or queue allocation failed";
				return true;
			}
			std::memset([output contents],0xa5,sizeof(inputBits));
			id<MTLCommandBuffer> command=[queue commandBuffer];
			id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
			if( !command || !encoder ) {
				capability.structuredError="production fire identity command encoding failed";
				return true;
			}
			[encoder setComputePipelineState:pipeline];
			[encoder setBuffer:input offset:0 atIndex:0];
			[encoder setBuffer:output offset:0 atIndex:1];
			[encoder dispatchThreads:MTLSizeMake(8,1,1) threadsPerThreadgroup:MTLSizeMake(8,1,1)];
			[encoder endEncoding];
			[command commit];
			[command waitUntilCompleted];
			if( [command status]!=MTLCommandBufferStatusCompleted ) {
				capability.structuredError=MetalError("production fire identity command failed",[command error]);
				return true;
			}
			if( std::memcmp([output contents],inputBits,sizeof(inputBits))!=0 ) {
				capability.structuredError="production fire identity command returned nonidentical fp32 bytes";
				return true;
			}
			capability.available=true;
			capability.identityKernelPassed=true;
			capability.structuredError.clear();
		}
		return true;
	}
}
