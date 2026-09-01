//////////////////////////////////////////////////////////////////////
//
//  FireProductionComputeMac.mm - Metal production-fire capability
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <TargetConditionals.h>

#include "FireProductionCompute.h"

#include <cstring>

namespace RISE
{
	namespace
	{
		bool AppleSiliconHost()
		{
#if TARGET_CPU_ARM64
			return true;
#else
			return false;
#endif
		}

		id<MTLDevice> DiscoverMetalDevice( bool& defaultDevicePresent,
			std::size_t& enumeratedDeviceCount )
		{
			id<MTLDevice> device=MTLCreateSystemDefaultDevice();
			defaultDevicePresent=device!=nil;
			NSArray<id<MTLDevice> >* devices=MTLCopyAllDevices();
			enumeratedDeviceCount=static_cast<std::size_t>([devices count]);
			return device;
		}

		std::string DiscoveryFailure( const char* operation,
			FireProductionDeviceDiscovery discovery,std::size_t enumeratedDeviceCount )
		{
			std::string result="production fire compute ";result+=operation;
			if( discovery==FireProductionDeviceBlockedByExecutionContext ) {
				result+=" blocked by execution context: ";
				if( enumeratedDeviceCount>0u ) result+=
					"default-device access failed although Metal enumeration found "+
					std::to_string(enumeratedDeviceCount)+" device(s)";
				else if( AppleSiliconHost() ) result+=
					"Metal discovery returned no devices on Apple silicon";
				else result+="default-device access failed";
				return result;
			}
			result += operation[0]=='c'&&operation[1]=='a' ?
				" unavailable: no Metal device" : " has no Metal device";
			return result;
		}

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
				const MTLGPUFamily appleFamilies[]={MTLGPUFamilyApple10,MTLGPUFamilyApple9,
					MTLGPUFamilyApple8,MTLGPUFamilyApple7,MTLGPUFamilyApple6,
					MTLGPUFamilyApple5,MTLGPUFamilyApple4,MTLGPUFamilyApple3,
					MTLGPUFamilyApple2,MTLGPUFamilyApple1};
				for( std::size_t i=0;i<sizeof(appleFamilies)/sizeof(appleFamilies[0]);++i )
					if( [device supportsFamily:appleFamilies[i]] )
						return "apple"+std::to_string(10-static_cast<int>(i));
				if( [device supportsFamily:MTLGPUFamilyMac2] ) return "mac2";
				if( [device supportsFamily:MTLGPUFamilyCommon3] ) return "common3";
				if( [device supportsFamily:MTLGPUFamilyCommon2] ) return "common2";
				if( [device supportsFamily:MTLGPUFamilyCommon1] ) return "common1";
			}
			return "metal-family-unreported";
		}

		id<MTLLibrary> CompileLibrary( id<MTLDevice> device,
			std::string* structuredError )
		{
			static NSString* const source=
				@"#include <metal_stdlib>\n"
				 "using namespace metal;\n"
				 "kernel void rise_fire_identity(device const float* input [[buffer(0)]],"
				 " device float* output [[buffer(1)]], uint i [[thread_position_in_grid]])"
				 " { output[i]=input[i]; }\n"
				 "kernel void rise_fire_challenge(device const uint* input [[buffer(0)]],"
				 " device uint* output [[buffer(1)]], uint i [[thread_position_in_grid]])"
				 " { output[i]=input[i] ^ (0x9e3779b9u + i*0x85ebca6bu); }\n";
			NSError* error=nil;
			MTLCompileOptions* options=[[MTLCompileOptions alloc] init];
			if( @available(macOS 15.0,*) ) options.mathMode=MTLMathModeSafe;
			else {
				if( structuredError ) *structuredError=
					"production fire compute requires Metal safe math mode on macOS 15 or newer";
				return nil;
			}
			id<MTLLibrary> library=[device newLibraryWithSource:source options:options error:&error];
			if( library ) return library;
			if( structuredError ) *structuredError=
				MetalError("production fire compute library compilation failed",error);
			return nil;
		}
	}

	bool QueryFireProductionComputeCapability(
		FireProductionComputeCapability& capability )
	{
		capability=FireProductionComputeCapability();
		capability.backend="metal";
		@autoreleasepool {
			id<MTLDevice> device=DiscoverMetalDevice(capability.defaultDevicePresent,
				capability.enumeratedDeviceCount);
			capability.deviceDiscovery=ClassifyFireProductionDeviceDiscovery(true,
				AppleSiliconHost(),capability.defaultDevicePresent,
				capability.enumeratedDeviceCount);
			if( !device ) {
				capability.structuredError=DiscoveryFailure("capability",
					capability.deviceDiscovery,capability.enumeratedDeviceCount);
				return true;
			}

			capability.deviceName=UTF8String([device name]);
			capability.registryId=static_cast<std::uint64_t>([device registryID]);
			capability.deviceFamily=DeviceFamily(device);
			if( @available(macOS 10.15,*) )
				capability.unifiedMemory=[device hasUnifiedMemory];

			id<MTLLibrary> library=CompileLibrary(device,&capability.structuredError);
			if( !library ) {
				return true;
			}
			NSError* error=nil;
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

	bool RunFireProductionComputeChallenge(
		const std::uint32_t* inputBits, std::size_t count,
		std::vector<std::uint32_t>& outputBits, std::string* structuredError )
	{
		outputBits.clear();
		if( !inputBits || count==0u ) {
			if( structuredError ) *structuredError="production fire compute challenge is empty";
			return false;
		}
		@autoreleasepool {
			bool defaultDevicePresent=false;
			std::size_t enumeratedDeviceCount=0u;
			id<MTLDevice> device=DiscoverMetalDevice(defaultDevicePresent,
				enumeratedDeviceCount);
			const FireProductionDeviceDiscovery discovery=
				ClassifyFireProductionDeviceDiscovery(true,AppleSiliconHost(),
					defaultDevicePresent,enumeratedDeviceCount);
			if( !device ) {
				if( structuredError ) *structuredError=DiscoveryFailure(
					"challenge",discovery,enumeratedDeviceCount);
				return false;
			}
			id<MTLLibrary> library=CompileLibrary(device,structuredError);
			if( !library ) return false;
			id<MTLFunction> function=[library newFunctionWithName:@"rise_fire_challenge"];
			NSError* metalError=nil;
			id<MTLComputePipelineState> pipeline=function ?
				[device newComputePipelineStateWithFunction:function error:&metalError] : nil;
			if( !pipeline ) {
				if( structuredError ) *structuredError=MetalError(
					"production fire challenge pipeline creation failed",metalError);
				return false;
			}
			const std::size_t byteCount=count*sizeof(std::uint32_t);
			id<MTLBuffer> input=[device newBufferWithBytes:inputBits length:byteCount
				options:MTLResourceStorageModeShared];
			id<MTLBuffer> output=[device newBufferWithLength:byteCount
				options:MTLResourceStorageModeShared];
			id<MTLCommandQueue> queue=[device newCommandQueue];
			id<MTLCommandBuffer> command=[queue commandBuffer];
			id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
			if( !input || !output || !queue || !command || !encoder ) {
				if( structuredError ) *structuredError=
					"production fire challenge command allocation failed";
				return false;
			}
			[encoder setComputePipelineState:pipeline];
			[encoder setBuffer:input offset:0 atIndex:0];
			[encoder setBuffer:output offset:0 atIndex:1];
			const std::size_t width=std::min(count,
				static_cast<std::size_t>([pipeline maxTotalThreadsPerThreadgroup]));
			[encoder dispatchThreads:MTLSizeMake(count,1,1)
				threadsPerThreadgroup:MTLSizeMake(width,1,1)];
			[encoder endEncoding];
			[command commit];
			[command waitUntilCompleted];
			if( [command status]!=MTLCommandBufferStatusCompleted ) {
				if( structuredError ) *structuredError=MetalError(
					"production fire challenge command failed",[command error]);
				return false;
			}
			const std::uint32_t* returned=
				static_cast<const std::uint32_t*>([output contents]);
			outputBits.assign(returned,returned+count);
		}
		if( structuredError ) structuredError->clear();
		return true;
	}
}
