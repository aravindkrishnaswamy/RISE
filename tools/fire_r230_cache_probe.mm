// Synthetic native Metal kernel REDs. Shared fixture inputs carry NO production authority.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

struct Params {
    uint32_t cells,stage,precision,rows;
    float Tmin,Tmax,pressure,feasibility,bound,dt;
    uint32_t pad0,pad1;
    uint64_t attempt,caseIdentity;
};
static_assert(sizeof(Params)==64,"EOS parameter fixture ABI");

int main(int argc,char** argv){@autoreleasepool {
    if(argc!=2&&argc!=3)return 91;
    const bool parameterMutant=argc==3&&std::strcmp(argv[2],"--parameter-mutant")==0;
    if(argc==3&&!parameterMutant)return 91;
    std::ifstream input(argv[1]);
    const std::string text((std::istreambuf_iterator<char>(input)),{});
    const auto begin=text.find("struct ResidentTransportMetalContext");
    const auto end=text.find("explicit ResidentTransportMetalContext",begin);
    if(begin==std::string::npos||end==std::string::npos)return 92;
    std::string source;size_t at=begin;
    while((at=text.find("R\"METAL(",at))<end){at+=8;
        const auto close=text.find(")METAL\"",at);if(close==std::string::npos)return 92;
        source+=text.substr(at,close-at);at=close+7;}
    if(parameterMutant){
        const std::string predicate="currentBytes[byte]!=cachedBytes[byte]";
        const auto position=source.find(predicate);
        if(position==std::string::npos||source.find(predicate,position+1)!=std::string::npos)return 92;
        source.replace(position,predicate.size(),"(currentBytes[byte]^cachedBytes[byte])>255");}
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();if(!device)return 93;
    MTLCompileOptions* options=[MTLCompileOptions new];
    if(@available(macOS 15.0,*)){options.mathMode=MTLMathModeSafe;
        options.languageVersion=MTLLanguageVersion3_2;}else return 93;
    NSError* error=nil;
    id<MTLLibrary> library=[device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
        options:options error:&error];
    if(!library){std::fprintf(stderr,"%s\n",[[error description] UTF8String]);return 94;}
    id<MTLComputePipelineState> endpoint=[device newComputePipelineStateWithFunction:
        [library newFunctionWithName:@"build_eos_endpoint_table"] error:&error];
    id<MTLComputePipelineState> evaluate=[device newComputePipelineStateWithFunction:
        [library newFunctionWithName:@"evaluate_resident_eos_candidate"] error:&error];
    if(!endpoint||!evaluate)return 94;
    auto allocate=[&](size_t bytes){id<MTLBuffer> b=[device newBufferWithLength:bytes
        options:MTLResourceStorageModeShared];if(b)std::memset([b contents],0,bytes);return b;};
    std::array<id<MTLBuffer>,17> b;
    const size_t sizes[]={36,4000,4000,8,8,8,4,4,4,64,4,4,131168,36,8,64,8};
    for(size_t i=0;i<b.size();++i){b[i]=allocate(sizes[i]);if(!b[i])return 95;}
    auto* thermo=static_cast<float*>([b[2] contents]);
    for(unsigned s=0;s<7;++s){const unsigned base=96*s;thermo[base]=1;thermo[base+3]=1;
        thermo[base+4]=300;thermo[base+7]=2300;thermo[base+16]=1;}
    thermo[672]=1;thermo[675]=300;
    Params params={1,2,32,0,300,2300,300,0x1p-20f,0.25f,0.001f,0,0,1,1};
    std::memcpy([b[9] contents],&params,sizeof(params));
    id<MTLCommandQueue> queue=[device newCommandQueue];if(!queue)return 95;
    auto run=[&](const char* name,int mode,uint32_t expectedFailure,float expectedT){
        const float state[]={1,1,0,0,0,0,0,0,300};
        std::memcpy([b[0] contents],state,sizeof(state));std::memcpy([b[13] contents],state,sizeof(state));
        std::memcpy([b[15] contents],&params,sizeof(params));
        *static_cast<uint64_t*>([b[3] contents])=1;
        *static_cast<uint64_t*>([b[16] contents])=mode==3?0:1;
        auto* cache=static_cast<float*>([b[14] contents]);cache[0]=cache[1]=300;
        if(mode==1)cache[0]=std::nextafter(cache[0],301.0f);
        if(mode==2)cache[1]=std::nextafter(cache[1],301.0f);
        if(mode==4)static_cast<float*>([b[0] contents])[8]=301;
        if(mode==5){static_cast<Params*>([b[15] contents])->attempt=2;
            cache[0]=std::nextafter(cache[0],301.0f);}
        for(unsigned i:{4u,5u,6u,7u,8u,10u,11u})std::memset([b[i] contents],0,sizes[i]);
        id<MTLCommandBuffer> command=[queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
        [encoder setComputePipelineState:endpoint];
        const unsigned ep[]={2,9,12,8,7};
        for(unsigned i=0;i<5;++i)[encoder setBuffer:b[ep[i]] offset:0 atIndex:i];
        [encoder dispatchThreads:MTLSizeMake(1037,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
        [encoder endEncoding];encoder=[command computeCommandEncoder];
        [encoder setComputePipelineState:evaluate];
        for(unsigned i=0;i<17;++i)[encoder setBuffer:b[i] offset:0 atIndex:i];
        [encoder dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
        [encoder endEncoding];[command commit];[command waitUntilCompleted];
        const auto failure=*static_cast<uint32_t*>([b[7] contents]);
        const auto* output=static_cast<float*>([b[4] contents]);
        const bool passed=command.status==MTLCommandBufferStatusCompleted&&failure==expectedFailure&&
            (failure!=0||(output[0]==expectedT&&output[1]==expectedT));
        std::printf("R230_NATIVE_CACHE synthetic=1 production_authority=0 name=%s failure=%u T=%.9g alternate=%.9g passed=%d\n",
            name,failure,output[0],output[1],passed);return passed;};
    return run("identical_input",0,0,300)&&run("cached_T_corruption",1,128,0)&&
        run("cached_alternate_corruption",2,128,0)&&run("unsealed_cache",3,128,0)&&
        run("changed_energy_recomputes",4,0,301)&&run("changed_parameters_recomputes",5,0,300)?0:96;
}}
