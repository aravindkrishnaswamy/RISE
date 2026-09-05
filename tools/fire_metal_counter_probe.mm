#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>

int main()
{
    @autoreleasepool {
        id<MTLDevice> device=MTLCreateSystemDefaultDevice();
        if(!device){std::fprintf(stderr,"Metal context has no default device\n");return 1;}
        std::printf("device=%s dispatch_boundary=%d stage_boundary=%d\n",
            [[device name] UTF8String],
            [device supportsCounterSampling:MTLCounterSamplingPointAtDispatchBoundary],
            [device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]);
        for(id<MTLCounterSet> counterSet in [device counterSets])
            std::printf("counter_set=%s\n",[[counterSet name] UTF8String]);
        NSError* error=nil;
        id<MTLLibrary> library=[device newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal; kernel void counter_probe(uint i [[thread_position_in_grid]]) {}"
            options:nil error:&error];
        id<MTLComputePipelineState> pipeline=[device newComputePipelineStateWithFunction:
            [library newFunctionWithName:@"counter_probe"] error:&error];
        if(!pipeline)return 2;
        std::printf("pipeline_label=%s\n",[[pipeline label] UTF8String]);
    }
    return 0;
}
