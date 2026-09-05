//////////////////////////////////////////////////////////////////////
//
//  FireProductionAdvectionMac.mm - Metal conservative production PPM
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <TargetConditionals.h>

#include "FireProductionAdvection.h"
#include "FireProductionCompute.h"
#include "FireProductionForce.h"
#include "FireSimulationRecords.h"
#include "FireProductionTransport.h"
#include "RISECBOR64.h"
#include "FireCase.h"
#include "ThreadPool.h"
#include "../Interfaces/IOptions.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <iomanip>
#include <limits>
#include <memory>
#include <new>
#include <sstream>
#include <map>
#include <mutex>

namespace RISE
{
	// Internal behavioral seam: production and the calibration RED consume the
	// same recursion-safety decision without exposing it in the public API.
	bool FireProductionDualLayoutPackRequiresSerialOwner(
		const bool auditSerial,const bool legacyLowPriority )
	{
		return auditSerial||legacyLowPriority;
	}

	// Internal test-evidence preflight shared with FireProductionProjectionMac.mm.
	bool ValidateFireProductionRestorationCycleProbe(
		unsigned int& cycleCount,bool& enabled,std::string* error );

	namespace
	{
		id<MTLDevice> DiscoverProductionMetalDevice(const char* operation,
			std::string& error)
		{
			id<MTLDevice> device=MTLCreateSystemDefaultDevice();
			const bool defaultPresent=device!=nil;
			NSArray<id<MTLDevice> >* enumerated=MTLCopyAllDevices();
			const std::size_t enumeratedCount=static_cast<std::size_t>([enumerated count]);
#if TARGET_CPU_ARM64
			const bool appleSilicon=true;
#else
			const bool appleSilicon=false;
#endif
			const FireProductionDeviceDiscovery discovery=
				ClassifyFireProductionDeviceDiscovery(true,appleSilicon,defaultPresent,
					enumeratedCount);
			if(device)return device;
			error="production ";error+=operation;error+=" Metal discovery ";
			error+=FireProductionDeviceDiscoveryName(discovery);
			error+=" (default_present="+std::to_string(defaultPresent?1u:0u)+
				", enumerated_count="+std::to_string(enumeratedCount)+")";
			return nil;
		}

		std::string MetalString(NSString* value)
		{
			if(!value)return std::string();const char* utf8=[value UTF8String];
			return utf8?std::string(utf8):std::string();
		}

		// The payload format is independent of the older accepted-state token
		// digest versions. All framing integers are big endian; node bytes are
		// SHA-256 output bytes, not native-endian words.
		const char* PayloadMerkleSource()
		{
			return R"METAL(
#include <metal_stdlib>
using namespace metal;
constant uint merkle_k[64]={
0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
uint merkle_rotr(uint x,uint n){return (x>>n)|(x<<(32u-n));}
void merkle_be(thread uchar* h,thread uint& at,ulong v,uint width){
    for(uint i=width;i!=0u;--i)h[at++]=uchar(v>>((i-1u)*8u));
}
void merkle_sha(thread const uchar* header,uint headerSize,device const uchar* data,
    ulong start,uint dataSize,device uchar* out){
    uint s[8]={0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    uint size=headerSize+dataSize, padded=((size+9u+63u)/64u)*64u;
    for(uint block=0u;block<padded;block+=64u){
        uint w[64];
        for(uint j=0u;j<16u;++j){uint word=0u;
            for(uint b=0u;b<4u;++b){uint p=block+4u*j+b;uchar v=0u;
                if(p<headerSize)v=header[p];
                else if(p<size)v=data[start+p-headerSize];
                else if(p==size)v=0x80u;
                else if(p>=padded-8u)v=uchar((ulong(size)*8u)>>((padded-1u-p)*8u));
                word=(word<<8u)|uint(v);
            }w[j]=word;
        }
        for(uint j=16u;j<64u;++j){uint a=w[j-15u],b=w[j-2u];
            w[j]=w[j-16u]+(merkle_rotr(a,7u)^merkle_rotr(a,18u)^(a>>3u))+
                w[j-7u]+(merkle_rotr(b,17u)^merkle_rotr(b,19u)^(b>>10u));}
        uint a=s[0],b=s[1],c=s[2],d=s[3],e=s[4],f=s[5],g=s[6],h=s[7];
        for(uint j=0u;j<64u;++j){
            uint t1=h+(merkle_rotr(e,6u)^merkle_rotr(e,11u)^merkle_rotr(e,25u))+
                ((e&f)^((~e)&g))+merkle_k[j]+w[j];
            uint t2=(merkle_rotr(a,2u)^merkle_rotr(a,13u)^merkle_rotr(a,22u))+
                ((a&b)^(a&c)^(b&c));
            h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }s[0]+=a;s[1]+=b;s[2]+=c;s[3]+=d;s[4]+=e;s[5]+=f;s[6]+=g;s[7]+=h;
    }
    for(uint j=0u;j<8u;++j)for(uint b=0u;b<4u;++b)out[4u*j+b]=uchar(s[j]>>((3u-b)*8u));
}
// params: total payload bytes, input node count (or leaf count), level.
kernel void merkle_leaf(device const uchar* payload [[buffer(0)]],device uchar* nodes [[buffer(1)]],
    constant ulong* params [[buffer(2)]],uint tid [[thread_position_in_grid]]){
    ulong index=tid;if(index>=params[1])return;
    ulong start=index*4096u;uint length=uint(min(ulong(4096u),params[0]-start));
    uchar header[40];uint at=0u;merkle_be(header,at,0x524953454c454146ul,8u);
    merkle_be(header,at,2u,4u);merkle_be(header,at,params[0],8u);
    merkle_be(header,at,index,8u);merkle_be(header,at,length,4u);
    merkle_sha(header,at,payload,start,length,nodes+index*32u);
}
kernel void merkle_node(device const uchar* children [[buffer(0)]],device uchar* nodes [[buffer(1)]],
    constant ulong* params [[buffer(2)]],uint tid [[thread_position_in_grid]]){
    ulong index=tid,first=index*16u;if(first>=params[1])return;
    uint count=uint(min(ulong(16u),params[1]-first));
    uchar header[40];uint at=0u;merkle_be(header,at,0x524953454e4f4445ul,8u);
    merkle_be(header,at,2u,4u);merkle_be(header,at,params[0],8u);merkle_be(header,at,params[2],4u);
    merkle_be(header,at,index,8u);merkle_be(header,at,count,4u);
    merkle_sha(header,at,children,first*32u,count*32u,nodes+index*32u);
}
kernel void merkle_root(device const uchar* child [[buffer(0)]],device uchar* root [[buffer(1)]],
    constant ulong* params [[buffer(2)]],uint tid [[thread_position_in_grid]]){
    if(tid!=0u)return;uchar header[40];uint at=0u;
    merkle_be(header,at,0x52495345524f4f54ul,8u);merkle_be(header,at,2u,4u);
    merkle_be(header,at,4096u,4u);merkle_be(header,at,16u,4u);
    merkle_be(header,at,params[0],8u);merkle_be(header,at,params[1],8u);merkle_be(header,at,params[2],4u);
    merkle_sha(header,at,child,0u,32u,root);
}
)METAL";
		}

		struct PayloadMerkleContext
		{
			id<MTLDevice> device=nil;
			id<MTLCommandQueue> queue=nil;
			id<MTLComputePipelineState> leaf=nil,node=nil,root=nil;
			std::string error;
			PayloadMerkleContext(){
				device=DiscoverProductionMetalDevice("payload-merkle-v2",error);if(!device)return;
				queue=[device newCommandQueue];NSError* failure=nil;
				MTLCompileOptions* options=[[MTLCompileOptions alloc] init];
				options.languageVersion=MTLLanguageVersion3_2;
				id<MTLLibrary> library=[device newLibraryWithSource:
					[NSString stringWithUTF8String:PayloadMerkleSource()] options:options error:&failure];
				if(!library){error=MetalString([failure localizedDescription]);return;}
				auto pipeline=[&](NSString* name){return [device newComputePipelineStateWithFunction:
					[library newFunctionWithName:name] error:&failure];};
				leaf=pipeline(@"merkle_leaf");node=pipeline(@"merkle_node");root=pipeline(@"merkle_root");
				if(!queue||!leaf||!node||!root)error="payload-merkle-v2 pipeline: "+MetalString([failure localizedDescription]);
			}
		};

		// Hash a device payload without reading an intermediate tree node on the
		// host. The command owns every private level until its terminal root copy.
		bool EncodePayloadMerkle(PayloadMerkleContext& context,id<MTLCommandBuffer> command,
			id<MTLBuffer> payload,std::size_t bytes,unsigned int width,id<MTLBuffer> root,
			std::string& error,unsigned int qualificationFailTreeAllocation=0u,
			std::size_t rootOffset=0u)
		{
			if(!context.error.empty()){error=context.error;return false;}
			if(!command||!payload||bytes>[payload length]||!root||rootOffset>[root length]||[root length]-rootOffset<32u||
				width==0u||width>std::min({context.leaf.maxTotalThreadsPerThreadgroup,
				context.node.maxTotalThreadsPerThreadgroup,context.root.maxTotalThreadsPerThreadgroup})||
				bytes/4096u+(bytes%4096u!=0u)>UINT32_MAX){error="payload-merkle-v2 invalid device span or dispatch";return false;}
			std::size_t count=std::max<std::size_t>(1u,bytes/4096u+(bytes%4096u!=0u));
			const std::size_t leafCount=count;std::uint64_t level=0u;
			unsigned int allocationIndex=0u;
			auto allocate=[&](std::size_t size)->id<MTLBuffer>{
				if(++allocationIndex==qualificationFailTreeAllocation)return nil;
				return [context.device newBufferWithLength:size options:MTLResourceStorageModePrivate];};
			auto encode=[&](id<MTLComputePipelineState> pipeline,id<MTLBuffer> input,
				id<MTLBuffer> output,std::size_t inputCount,std::size_t outputCount){
				if(!output)return false;
				id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
				if(!encoder)return false;
				const std::uint64_t params[]={bytes,inputCount,level};
				[encoder setComputePipelineState:pipeline];[encoder setBuffer:input offset:0 atIndex:0];
				[encoder setBuffer:output offset:pipeline==context.root?rootOffset:0u atIndex:1];
				[encoder setBytes:params length:sizeof(params) atIndex:2];
				[encoder dispatchThreads:MTLSizeMake(outputCount,1,1) threadsPerThreadgroup:MTLSizeMake(width,1,1)];
				[encoder endEncoding];return true;};
			id<MTLBuffer> previous=allocate(count*32u);
			if(!encode(context.leaf,payload,previous,count,count)){error="payload-merkle-v2 leaf allocation/encoder";return false;}
			while(count>1u){++level;const std::size_t nextCount=count/16u+(count%16u!=0u);
				id<MTLBuffer> next=allocate(nextCount*32u);
				if(!encode(context.node,previous,next,count,nextCount)){error="payload-merkle-v2 node allocation/encoder";return false;}
				previous=next;count=nextCount;}
			if(!encode(context.root,previous,root,leafCount,1u)){error="payload-merkle-v2 root encoder";return false;}
			return true;
		}

		std::string ProductionMetalDeviceFamily(id<MTLDevice> device)
		{
			if(@available(macOS 10.15,*)){
				const MTLGPUFamily appleFamilies[]={MTLGPUFamilyApple10,MTLGPUFamilyApple9,
					MTLGPUFamilyApple8,MTLGPUFamilyApple7,MTLGPUFamilyApple6,
					MTLGPUFamilyApple5,MTLGPUFamilyApple4,MTLGPUFamilyApple3,
					MTLGPUFamilyApple2,MTLGPUFamilyApple1};
				for(std::size_t index=0u;index<sizeof(appleFamilies)/sizeof(appleFamilies[0]);
					++index)if([device supportsFamily:appleFamilies[index]])
						return "apple"+std::to_string(10-static_cast<int>(index));
				if([device supportsFamily:MTLGPUFamilyMac2])return "mac2";
				if([device supportsFamily:MTLGPUFamilyCommon3])return "common3";
				if([device supportsFamily:MTLGPUFamilyCommon2])return "common2";
				if([device supportsFamily:MTLGPUFamilyCommon1])return "common1";
			}
			return "metal-family-unreported";
		}

		thread_local bool CompatibleMomentumDiagnosticActive=false;
		std::uint64_t AvalancheAcceptedDigest(std::uint64_t word)
		{
			word^=word>>32u;word*=UINT64_C(0xd6e8feb86659fd93);
			word^=word>>32u;word*=UINT64_C(0xd6e8feb86659fd93);
			return word^(word>>32u);
		}

		std::uint64_t OrderedAcceptedFloatFieldDigest(
			const std::vector<float>& values,const std::uint64_t fieldTag )
		{
			std::uint64_t ordered=UINT64_C(0x243f6a8885a308d3)^fieldTag;
			for(std::size_t index=0u;index<values.size();++index){
				std::uint32_t bits=0u;std::memcpy(&bits,&values[index],sizeof(bits));
				ordered^=static_cast<std::uint64_t>(bits)+UINT64_C(0x9e3779b97f4a7c15)+
					(static_cast<std::uint64_t>(index)<<32u);
				ordered=((ordered<<27u)|(ordered>>37u))*UINT64_C(0x3c79ac492ba7b653)+
					UINT64_C(0x1c69b3f74ac4ae35);
			}
			return AvalancheAcceptedDigest(ordered);
		}

		std::uint64_t OrderedAcceptedByteFieldDigest(
			const std::vector<unsigned char>& values,const std::uint64_t fieldTag )
		{
			std::uint64_t ordered=UINT64_C(0x243f6a8885a308d3)^fieldTag;
			for(std::size_t index=0u;index<values.size();++index){
				ordered^=static_cast<std::uint64_t>(values[index])+
					UINT64_C(0x9e3779b97f4a7c15)+(static_cast<std::uint64_t>(index)<<32u);
				ordered=((ordered<<27u)|(ordered>>37u))*UINT64_C(0x3c79ac492ba7b653)+
					UINT64_C(0x1c69b3f74ac4ae35);
			}
			return AvalancheAcceptedDigest(ordered);
		}

		std::uint64_t ParallelAcceptedManifoldPayloadDigest(
			const FireProductionResidentStepResult& value,const bool serial )
		{
			std::array<const std::vector<float>*,27> floatFields;
			std::size_t field=0u;floatFields[field++]=&value.conservativeValues;
			for(unsigned int axis=0u;axis<3u;++axis){
				floatFields[field++]=&value.transportedDual.auxiliaryFaceDensity[axis];
				floatFields[field++]=&value.transportedDual.momentum[axis];
				floatFields[field++]=&value.physicalProjection.faceDensityKGPerM3[axis];
				floatFields[field++]=&value.physicalProjection.velocityMPerS[axis];
				floatFields[field++]=&value.physicalProjection.momentumKGPerM2S[axis];
				floatFields[field++]=&value.projection.faceDensityKGPerM3[axis];
				floatFields[field++]=&value.projection.velocityMPerS[axis];
				floatFields[field++]=&value.projection.momentumKGPerM2S[axis];
			}
			floatFields[field++]=&value.physicalProjection.pressurePa;
			floatFields[field++]=&value.projection.pressurePa;
			if(field!=floatFields.size())return 0u;
			std::array<const std::vector<unsigned char>*,12> byteFields;
			field=0u;
			for(const auto& side:value.physicalProjection.pressureOpenInflow)
				byteFields[field++]=&side;
			for(const auto& side:value.projection.pressureOpenInflow)byteFields[field++]=&side;
			if(field!=byteFields.size())return 0u;
			std::array<std::uint64_t,39> ordered;
			auto hashField=[&](const std::size_t index){
				if(index<floatFields.size())ordered[index]=OrderedAcceptedFloatFieldDigest(
					*floatFields[index],static_cast<std::uint64_t>(index+1u));
				else {const std::size_t byteIndex=index-floatFields.size();
					ordered[index]=OrderedAcceptedByteFieldDigest(*byteFields[byteIndex],
						static_cast<std::uint64_t>(index+1u));}
			};
			if(serial)for(std::size_t index=0u;index<ordered.size();++index)hashField(index);
			else Implementation::GlobalThreadPool().ParallelFor(ordered.size(),hashField);
			std::uint64_t digest=UINT64_C(0xd6e8feb86659fd93);
			auto appendWord=[&](const std::uint64_t word){digest^=word+
				UINT64_C(0x9e3779b97f4a7c15)+(digest<<6u)+(digest>>2u);};
			for(std::size_t index=0u;index<floatFields.size();++index){
				appendWord(index+1u);appendWord(floatFields[index]->size());appendWord(ordered[index]);}
			for(std::size_t index=0u;index<byteFields.size();++index){
				const std::size_t combined=index+floatFields.size();appendWord(combined+1u);
				appendWord(byteFields[index]->size());appendWord(ordered[combined]);}
			return AvalancheAcceptedDigest(digest^ordered.size());
		}

		std::uint64_t ParallelAcceptedStatePayloadDigestFast(
			const FireProductionProjectionShape& shape,
			const std::vector<float>& conservativeValues,
			const std::array<std::vector<float>,3>& momentum,
			const std::array<std::vector<float>,3>& velocity,const bool serial )
		{
			std::array<const std::vector<float>*,7> fields={{&conservativeValues,
				&momentum[0],&velocity[0],&momentum[1],&velocity[1],&momentum[2],&velocity[2]}};
			std::array<std::uint64_t,7> ordered;
			auto hashField=[&](const std::size_t index){ordered[index]=
				OrderedAcceptedFloatFieldDigest(*fields[index],static_cast<std::uint64_t>(index+1u));};
			if(serial)for(std::size_t index=0u;index<ordered.size();++index)hashField(index);
			else Implementation::GlobalThreadPool().ParallelFor(ordered.size(),hashField);
			std::uint64_t digest=UINT64_C(0x65f07b31c42a98de);
			auto appendWord=[&](const std::uint64_t word){digest^=word+
				UINT64_C(0x9e3779b97f4a7c15)+(digest<<6u)+(digest>>2u);};
			appendWord(UINT64_C(0x7265736964656e74));appendWord(shape.nx);appendWord(shape.ny);
			appendWord(shape.nz);std::uint32_t widthBits=0u;
			std::memcpy(&widthBits,&shape.cellWidthM,sizeof(widthBits));appendWord(widthBits);
			for(std::size_t index=0u;index<fields.size();++index){appendWord(index+1u);
				appendWord(fields[index]->size());appendWord(ordered[index]);}
			return AvalancheAcceptedDigest(digest^fields.size()^UINT64_C(0xd64b291e3fa5708c));
		}

		struct MetalManifoldParameters
		{
			std::uint32_t cellCount;
			std::uint32_t affineRowCount;
			std::uint32_t affineStateDimension;
			std::uint32_t reserved;
			float temperatureMinK;
			float temperatureMaxK;
			float pressurePa;
			float feasibilityFactor;
		};

		struct MetalSingleStageFCTParameters
		{
			std::uint32_t nx,ny,nz,cells,components,inequalities,nullity,affineRows;
			std::uint32_t boundary[6],sideOffset[6];
			float cellWidthM,timeStepS,feasibility,assemblyReserve;
		};

		struct MetalResidentTransportParameters
		{
			std::uint32_t nx,ny,nz,cells;
			std::uint32_t boundary[6],sideOffset[6];
			std::uint32_t faceOffset[3];
			std::uint32_t stage;
			float cellWidthM,turbulentPrandtl,turbulentSchmidt,vremanCoefficient;
			std::uint64_t attemptIdentity,parentCandidateIdentity,projectionIdentity;
			std::uint64_t thermochemistryIdentity,transportIdentity;
		};

		struct MetalResidentPhysicalFluxParameters
		{
			std::uint32_t advectiveNullity,physicalNullity,mutateHighNonadvective;
			float ambientTemperatureK;
		};

		struct MetalResidentEOSParameters
		{
			std::uint32_t cells,stage,precision,affineRowCount;
			float temperatureMinK,temperatureMaxK,pressurePa,feasibilityFactor;
			float dynamicsValidityBound,timeStepS;
			std::uint32_t padding[2];
			std::uint64_t attemptIdentity,caseIdentity;
		};

		struct MetalResidentTargetParameters
		{
			std::uint32_t nx,ny,nz,cells;
			std::uint32_t faceOffset[3];
			std::uint32_t boundary[6];
			float cellWidthM,timeStepS,tailThreshold;
			std::uint32_t preauthoredTarget,policyVersion,padding[2];
			std::uint64_t attemptIdentity,sourcePacketIdentity;
		};

		struct MetalResidentFrozenSourceParameters
		{
			std::uint32_t cells,padding[3];
			std::uint64_t attemptIdentity,sourcePacketIdentity;
		};

		struct MetalResidentProjectionConsumerParameters
		{
			std::uint32_t nx,ny,nz,cells;
			std::uint32_t boundary[6],padding[2];
			float cellWidthM,timeStepS;
			std::uint64_t attemptIdentity;
		};

		struct MetalProjectedHeunOwnerParameters
		{
			std::uint32_t cells,allFaces,faceOffset[3],stage;
			float timeStepS,cellWidthM,endpointVelocityToleranceMPerS;
			std::uint32_t forceActiveCycle,threeQuarterHeunWeighting;
			std::uint32_t identityPadding;
			std::uint64_t attemptIdentity;
		};

		constexpr std::size_t MetalResidentTransportMaximumKnots=128u;
		constexpr std::size_t MetalResidentTransportSpeciesStride=
			1u+5u*MetalResidentTransportMaximumKnots;
		constexpr std::size_t MetalResidentTransportSpeciesCount=6u;

		constexpr std::size_t MetalThermochemistrySpeciesStride=32u;
		constexpr std::size_t MetalThermochemistrySpeciesValues=
			7u*MetalThermochemistrySpeciesStride;
		constexpr std::size_t MetalEOSThermochemistrySpeciesStride=96u;
		constexpr std::size_t MetalEOSThermochemistryValues=
			7u*MetalEOSThermochemistrySpeciesStride+6u+3u*64u;
		constexpr std::size_t MetalManifoldCertificateValues=
			MetalThermochemistrySpeciesValues+64u;
		constexpr std::size_t MetalManifoldQuantileBins=65536u;
		constexpr std::size_t MetalManifoldQuantileControlWords=6u;
		constexpr std::size_t MetalManifoldQuantileScratchWords=
			3u*MetalManifoldQuantileBins+MetalManifoldQuantileControlWords;
		constexpr std::size_t MetalManifoldQuantileControlOffset=
			3u*MetalManifoldQuantileBins*sizeof(std::uint32_t);

		bool PackMetalMethaneThermochemistry(
			std::array<float,MetalManifoldCertificateValues>& packed,
			MetalManifoldParameters& parameters,
			std::array<double,7>& lowerEnthalpy,
			std::array<double,7>& upperEnthalpy,
			std::string* error )
		{
			packed.fill(0.0f);
			const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
			if(!fuel.IsValid()||fuel.SpeciesOrder().size()!=7u||
				!fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(fuel.TemperatureMinK(),
					lowerEnthalpy.data(),lowerEnthalpy.size(),error)||
				!fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(fuel.TemperatureMaxK(),
					upperEnthalpy.data(),upperEnthalpy.size(),error))return false;
			for(std::size_t speciesIndex=0u;speciesIndex<7u;++speciesIndex){
				const FireThermochemistrySpecies* species=fuel.FindSpecies(
					fuel.SpeciesOrder()[speciesIndex].c_str());
				if(!species||species->segments.empty()||species->segments.size()>3u)return false;
				float* destination=packed.data()+speciesIndex*MetalThermochemistrySpeciesStride;
				destination[0]=static_cast<float>(species->molecularWeightKGPerKMol);
				destination[1]=static_cast<float>(species->segments.size());
				for(std::size_t segmentIndex=0u;segmentIndex<species->segments.size();++segmentIndex){
					const FireThermochemistrySegment& segment=species->segments[segmentIndex];
					float* output=destination+2u+10u*segmentIndex;
					output[0]=static_cast<float>(segment.temperatureMinK);
					output[1]=static_cast<float>(segment.temperatureMaxK);
					for(std::size_t coefficient=0u;coefficient<7u;++coefficient)
						output[2u+coefficient]=static_cast<float>(segment.coefficients[coefficient]);
					output[9]=static_cast<float>(segment.sensibleEnthalpyOffsetJPerKG);
				}
			}
			const FireCertifiedNullspace& affine=fuel.ConservativeReconstruction();
			if(affine.stateDimension>8u||affine.constraintRows*affine.stateDimension>64u||
				affine.constraintMatrix.size()!=affine.constraintRows*affine.stateDimension)
				return false;
			parameters.affineRowCount=static_cast<std::uint32_t>(affine.constraintRows);
			parameters.affineStateDimension=static_cast<std::uint32_t>(affine.stateDimension);
			for(std::size_t coefficient=0u;coefficient<affine.constraintMatrix.size();++coefficient)
				packed[MetalThermochemistrySpeciesValues+coefficient]=
					static_cast<float>(affine.constraintMatrix[coefficient]);
			parameters.temperatureMinK=static_cast<float>(fuel.TemperatureMinK());
			parameters.temperatureMaxK=static_cast<float>(fuel.TemperatureMaxK());
			parameters.pressurePa=static_cast<float>(fuel.ThermodynamicPressurePa());
			parameters.feasibilityFactor=static_cast<float>(
				fuel.AcceptedStateFeasibilityEnvelope().kappaEpsilon32*
				static_cast<double>(std::numeric_limits<float>::epsilon()));
			return std::isfinite(parameters.temperatureMinK)&&
				std::isfinite(parameters.temperatureMaxK)&&
				std::isfinite(parameters.pressurePa)&&
				std::isfinite(parameters.feasibilityFactor)&&
				parameters.temperatureMinK>0.0f&&
				parameters.temperatureMaxK>parameters.temperatureMinK&&
				parameters.pressurePa>0.0f&&parameters.feasibilityFactor>0.0f;
		}

		bool PackMetalEOSDoubleDoubleThermochemistry(
			std::array<float,MetalEOSThermochemistryValues>& packed,std::string* error )
		{
			packed.fill(0.0f);const FireSimulationMethaneRecord& fuel=
				FireSimulationMethaneRecord::PhysicalV1();
			auto split=[](const double value,float* destination){const float high=
				static_cast<float>(value);destination[0]=high;
				const double firstResidual=value-static_cast<double>(high);
				destination[1]=static_cast<float>(firstResidual);
				const double secondResidual=firstResidual-static_cast<double>(destination[1]);
				destination[2]=static_cast<float>(secondResidual);
				const double retained=static_cast<double>(destination[2]);
				const double spacing=std::max(std::fabs(static_cast<double>(std::nextafter(
					destination[2],std::numeric_limits<float>::infinity()))-retained),
					std::fabs(retained-static_cast<double>(std::nextafter(destination[2],
					-std::numeric_limits<float>::infinity()))));
				return std::isfinite(destination[0])&&std::isfinite(destination[1])&&
					std::isfinite(destination[2])&&
					std::fabs(secondResidual-retained)<=spacing;};
			if(!fuel.IsValid()||fuel.SpeciesOrder().size()!=7u)return false;
			for(std::size_t speciesIndex=0u;speciesIndex<7u;++speciesIndex){
				const FireThermochemistrySpecies* species=fuel.FindSpecies(
					fuel.SpeciesOrder()[speciesIndex].c_str());
				if(!species||species->segments.empty()||species->segments.size()>3u)return false;
				float* destination=packed.data()+speciesIndex*MetalEOSThermochemistrySpeciesStride;
				if(!split(species->molecularWeightKGPerKMol,destination))return false;
				destination[3]=static_cast<float>(species->segments.size());
				for(std::size_t segmentIndex=0u;segmentIndex<species->segments.size();++segmentIndex){
					const FireThermochemistrySegment& segment=species->segments[segmentIndex];
					float* output=destination+4u+30u*segmentIndex;
					if(!split(segment.temperatureMinK,output)||
						!split(segment.temperatureMaxK,output+3u))return false;
					for(std::size_t coefficient=0u;coefficient<7u;++coefficient)
						if(!split(segment.coefficients[coefficient],output+6u+3u*coefficient))
							return false;
					if(!split(segment.sensibleEnthalpyOffsetJPerKG,output+27u))return false;
				}
			}
			const std::size_t constants=7u*MetalEOSThermochemistrySpeciesStride;
			if(!split(8314.46261815324,packed.data()+constants)||
				!split(fuel.ThermodynamicPressurePa(),packed.data()+constants+3u))return false;
			const FireCertifiedNullspace& affine=fuel.ConservativeReconstruction();
			if(affine.constraintMatrix.size()>64u)return false;
			for(std::size_t coefficient=0u;coefficient<affine.constraintMatrix.size();++coefficient)
				if(!split(affine.constraintMatrix[coefficient],packed.data()+constants+6u+3u*coefficient))
					return false;
			return true;
		}

		std::uint64_t ResidentRecordIdentity(const std::string& id)
		{
			std::uint64_t hash=UINT64_C(14695981039346656037);
			for(const unsigned char byte:id){hash^=byte;hash*=UINT64_C(1099511628211);}
			return hash==0u?1u:hash;
		}

		bool PackMetalResidentTransport(
			std::array<float,MetalResidentTransportSpeciesCount*
				MetalResidentTransportSpeciesStride>& packed,
			MetalResidentTransportParameters& parameters,std::string* error )
		{
			packed.fill(0.0f);
			const FireSimulationMethaneRecord& thermochemistry=
				FireSimulationMethaneRecord::PhysicalV1();
			const FireSimulationTransportRecord& transport=
				FireSimulationTransportRecord::OpenV1();
			if(!thermochemistry.IsValid()||!transport.IsValid()||
				thermochemistry.SpeciesOrder().size()<MetalResidentTransportSpeciesCount){
				if(error)*error="production resident transport records are unavailable";
				return false;
			}
			for(std::size_t speciesIndex=0u;
				speciesIndex<MetalResidentTransportSpeciesCount;++speciesIndex){
				const FireTransportSpecies* species=transport.FindSpecies(
					thermochemistry.SpeciesOrder()[speciesIndex].c_str());
				if(!species||!species->viscosity.IsValid()||!species->conductivity.IsValid()||
					species->viscosity.Wavelengths()!=species->conductivity.Wavelengths()||
					species->viscosity.Wavelengths().size()<2u||
					species->viscosity.Wavelengths().size()>MetalResidentTransportMaximumKnots||
					species->viscosity.Values().size()!=species->viscosity.Wavelengths().size()||
					species->viscosity.Slopes().size()!=species->viscosity.Wavelengths().size()||
					species->conductivity.Values().size()!=species->viscosity.Wavelengths().size()||
					species->conductivity.Slopes().size()!=species->viscosity.Wavelengths().size()){
					if(error)*error="production resident transport curve is malformed for "+
						thermochemistry.SpeciesOrder()[speciesIndex]+" (viscosity_knots="+
						std::to_string(species?species->viscosity.Wavelengths().size():0u)+
						", conductivity_knots="+std::to_string(species?
							species->conductivity.Wavelengths().size():0u)+")";
					return false;
				}
				const std::size_t count=species->viscosity.Wavelengths().size();
				float* destination=packed.data()+speciesIndex*MetalResidentTransportSpeciesStride;
				destination[0]=static_cast<float>(count);
				for(std::size_t knot=0u;knot<count;++knot){
					destination[1u+knot]=static_cast<float>(species->viscosity.Wavelengths()[knot]);
					destination[1u+MetalResidentTransportMaximumKnots+knot]=
					static_cast<float>(species->viscosity.Values()[knot]);
					destination[1u+2u*MetalResidentTransportMaximumKnots+knot]=
					static_cast<float>(species->viscosity.Slopes()[knot]);
					destination[1u+3u*MetalResidentTransportMaximumKnots+knot]=
					static_cast<float>(species->conductivity.Values()[knot]);
					destination[1u+4u*MetalResidentTransportMaximumKnots+knot]=
					static_cast<float>(species->conductivity.Slopes()[knot]);
				}
			}
			parameters.turbulentPrandtl=static_cast<float>(transport.TurbulentPrandtl());
			parameters.turbulentSchmidt=static_cast<float>(transport.TurbulentSchmidt());
			parameters.vremanCoefficient=static_cast<float>(transport.VremanCv());
			parameters.thermochemistryIdentity=ResidentRecordIdentity(thermochemistry.RecordId());
			parameters.transportIdentity=ResidentRecordIdentity(transport.RecordId());
			return std::isfinite(parameters.turbulentPrandtl)&&
				std::isfinite(parameters.turbulentSchmidt)&&
				std::isfinite(parameters.vremanCoefficient)&&parameters.turbulentPrandtl>0.0f&&
				parameters.turbulentSchmidt>0.0f&&parameters.vremanCoefficient>0.0f;
		}

		bool PackMetalSingleStageFCTCertificate(
			std::vector<float>& basis,std::vector<float>& coordinateProjector,
			std::array<float,14>& enthalpyBounds,std::vector<float>& affine,
			float& feasibility,float& assemblyReserve,std::string* error )
		{
			const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
			const FireCertifiedNullspace& reconstruction=fuel.ConservativeReconstruction();
			std::array<double,7> lower,upper;
			if(!fuel.IsValid()||reconstruction.stateDimension!=8u||
				reconstruction.nullity==0u||reconstruction.nullity>8u||
				reconstruction.orthonormalBasis.size()!=
					reconstruction.stateDimension*reconstruction.nullity||
				reconstruction.constraintMatrix.size()!=
					reconstruction.constraintRows*reconstruction.stateDimension||
				!fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(fuel.TemperatureMinK(),
					lower.data(),lower.size(),error)||
				!fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(fuel.TemperatureMaxK(),
					upper.data(),upper.size(),error))return false;
			basis.resize(reconstruction.orthonormalBasis.size());
			for(std::size_t index=0u;index<basis.size();++index){
				basis[index]=static_cast<float>(reconstruction.orthonormalBasis[index]);
				if(!std::isfinite(basis[index]))return false;
			}
			coordinateProjector.assign(reconstruction.nullity*reconstruction.nullity,0.0f);
			for(std::size_t index=0u;index<reconstruction.nullity;++index)
				coordinateProjector[index*reconstruction.nullity+index]=1.0f;
			affine.resize(reconstruction.constraintMatrix.size());
			for(std::size_t index=0u;index<affine.size();++index){
				affine[index]=static_cast<float>(reconstruction.constraintMatrix[index]);
				if(!std::isfinite(affine[index]))return false;
			}
			for(std::size_t species=0u;species<7u;++species){
				enthalpyBounds[species]=static_cast<float>(lower[species]);
				enthalpyBounds[7u+species]=static_cast<float>(upper[species]);
				if(!std::isfinite(enthalpyBounds[species])||
					!std::isfinite(enthalpyBounds[7u+species]))return false;
			}
			const FireAcceptedStateFeasibilityEnvelope& envelope=
				fuel.AcceptedStateFeasibilityEnvelope();
			const double epsilon=std::numeric_limits<float>::epsilon();
			feasibility=static_cast<float>(envelope.kappaEpsilon32*epsilon);
			assemblyReserve=static_cast<float>(envelope.remapFactorEpsilon32*epsilon);
			if(!std::isfinite(feasibility)||!std::isfinite(assemblyReserve)||
				!(feasibility>0.0f)||assemblyReserve<0.0f||
				!(assemblyReserve<feasibility)){
				if(error)*error="production single-stage FCT certificate reserve is invalid";
				return false;
			}
			return true;
		}

		std::uint64_t SingleStageFCTBoundaryIdentity(
			const FireProductionProjectionShape& shape,
			const FireProductionSingleStageFCTBoundaryState& state )
		{
			std::uint64_t digest=UINT64_C(0x5353464354424e44);
			auto append=[&](const std::uint64_t word){digest^=word+
				UINT64_C(0x9e3779b97f4a7c15)+(digest<<6u)+(digest>>2u);};
			append(shape.nx);append(shape.ny);append(shape.nz);
			append(state.statePayloadIdentity);
			for(std::size_t side=0u;side<state.pressureOpenInflow.size();++side){
				append(side+1u);append(state.pressureOpenInflow[side].size());
				append(OrderedAcceptedByteFieldDigest(state.pressureOpenInflow[side],side+1u));
			}
			return AvalancheAcceptedDigest(digest);
		}

		bool ValidateFireProductionCellSourceIncrement(
			const std::vector<float>& source,const std::size_t cells,
			const bool thermochemicalTerminalValidation,std::string* error )
		{
			if(source.size()!=9u*cells||cells==0u){
				if(error)*error="production resident step cell source shape is invalid";
				return false;
			}
			const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
			const FireCertifiedNullspace& closure=fuel.ConservativeReconstruction();
			if(!fuel.IsValid()||closure.stateDimension!=8u||
				closure.constraintMatrix.size()!=closure.constraintRows*closure.stateDimension){
				if(error)*error="production resident step cell source certificate is unavailable";
				return false;
			}
			const double factor=fuel.AcceptedStateFeasibilityEnvelope().
				sourcePacketFactorEpsilon32*std::numeric_limits<float>::epsilon();
			bool anyNonzero=false;
			for(std::size_t cell=0u;cell<cells;++cell){
				std::array<double,8> represented={{}};
				std::uint32_t rhoBits=0u;const float rhoSource=source[cell];
				std::memcpy(&rhoBits,&rhoSource,sizeof(rhoBits));
				if(rhoBits!=0u){
					if(error)*error="production resident step rho-total source is not positive zero";
					return false;
				}
				double massResidual=0.0,massScale=0.0;
				for(std::size_t species=0u;species<7u;++species){
					const float value=source[(1u+species)*cells+cell];
					std::uint32_t valueBits=0u;std::memcpy(&valueBits,&value,sizeof(valueBits));
					if(!std::isfinite(value)){
						if(error)*error="production resident step cell source is nonfinite";
						return false;
					}
					if(value==0.0f&&valueBits!=0u){
						if(error)*error="production resident step cell source has negative zero";
						return false;
					}
					represented[1u+species]=value;massResidual+=value;
					massScale+=std::fabs(static_cast<double>(value));anyNonzero|=value!=0.0f;
				}
				const float energy=source[8u*cells+cell];
				std::uint32_t energyBits=0u;std::memcpy(&energyBits,&energy,sizeof(energyBits));
				if(!std::isfinite(energy)){
					if(error)*error="production resident step cell source energy is nonfinite";
					return false;
				}
				if(energy==0.0f&&energyBits!=0u){
					if(error)*error="production resident step cell source energy has negative zero";
					return false;
				}
				anyNonzero|=energy!=0.0f;
				if(std::fabs(massResidual)>factor*std::max(1.0,massScale)){
					if(error)*error=
						"production resident step cell source violates mass conservation";
					return false;
				}
				for(std::size_t row=0u;row<closure.constraintRows;++row){
					double residual=0.0,scale=0.0;
					for(std::size_t column=0u;column<closure.stateDimension;++column){
						const double term=closure.constraintMatrix[row*closure.stateDimension+column]*
							represented[column];
						residual+=term;scale+=std::fabs(term);
					}
					if(!std::isfinite(residual)||std::fabs(residual)>
						factor*std::max(1.0,scale)){
						if(error)*error=
							"production resident step cell source violates an affine ledger";
						return false;
					}
				}
			}
			if(anyNonzero&&!thermochemicalTerminalValidation){if(error)*error=
				"production resident step nonzero source lacks terminal thermochemical validation";
				return false;}
			return true;
		}

		bool ValidateDualMomentumStaticOwnerMetadata(
			const FireProductionDualMomentumRequest& request,
			std::string* error )
		{
			const FireProductionProjectionShape& shape=request.shape;
			if(shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
				shape.nz<4u||shape.nz>1024u||!(shape.cellWidthM>0.0f)||
				request.timeStepS<0.0f||!std::isfinite(shape.cellWidthM)||
				!std::isfinite(request.timeStepS)||!(request.ambientDensityKGPerM3>0.0f)||
				!std::isfinite(request.ambientDensityKGPerM3))return false;
			auto faceIndex=[&](const unsigned int axis,const std::size_t x,
				const std::size_t y,const std::size_t z){
				if(axis==0u)return (z*shape.ny+y)*(shape.nx+1u)+x;
				if(axis==1u)return (z*(shape.ny+1u)+y)*shape.nx+x;
				return (z*shape.ny+y)*shape.nx+x;
			};
			for(unsigned int axis=0u;axis<3u;++axis){
				const FireProductionProjectionBoundary lower=request.boundary[2u*axis];
				const FireProductionProjectionBoundary upper=request.boundary[2u*axis+1u];
				const bool periodic=lower==FireProductionProjectionPeriodic;
				const std::size_t extent=axis==0u?shape.nx:(axis==1u?shape.ny:shape.nz);
				const std::size_t faces=FireProductionProjectionFaceCount(shape,axis);
				if(lower<FireProductionProjectionPeriodic||lower>FireProductionProjectionWall||
					upper<FireProductionProjectionPeriodic||upper>FireProductionProjectionWall||
					(periodic!=(upper==FireProductionProjectionPeriodic))||(!periodic&&extent<5u)||
					request.beginningFaceDensity[axis].size()!=faces||
					request.beginningMomentum[axis].size()!=faces||
					request.frozenVelocityMPerS[axis].size()!=faces)return false;
				for(const float density:request.beginningFaceDensity[axis])
					if(!(density>0.0f)||!std::isfinite(density))return false;
				for(const float momentum:request.beginningMomentum[axis])
					if(!std::isfinite(momentum))return false;
				for(const float velocity:request.frozenVelocityMPerS[axis])
					if(!std::isfinite(velocity))return false;
				if(periodic){
					const std::size_t firstExtent=axis==0u?shape.ny:shape.nx;
					const std::size_t secondExtent=axis==2u?shape.ny:shape.nz;
					for(std::size_t second=0u;second<secondExtent;++second)
						for(std::size_t first=0u;first<firstExtent;++first){
							const std::size_t lx=axis==0u?0u:first;
							const std::size_t ly=axis==0u?first:(axis==1u?0u:second);
							const std::size_t lz=axis==2u?0u:second;
							const std::size_t hx=axis==0u?extent:lx;
							const std::size_t hy=axis==1u?extent:ly;
							const std::size_t hz=axis==2u?extent:lz;
							const std::size_t low=faceIndex(axis,lx,ly,lz),high=faceIndex(axis,hx,hy,hz);
							if(request.beginningFaceDensity[axis][low]!=request.beginningFaceDensity[axis][high]||
								request.beginningMomentum[axis][low]!=request.beginningMomentum[axis][high]||
								request.frozenVelocityMPerS[axis][low]!=request.frozenVelocityMPerS[axis][high])
								return false;
						}
				}
			}
			if(error)error->clear();return true;
		}

		struct MetalParameters
		{
			std::uint32_t lineLength;
			std::uint32_t lineCount;
			std::uint32_t componentCount;
			std::uint32_t lowerBoundary;
			std::uint32_t upperBoundary;
			std::uint32_t ambientPerLine;
			float cellWidthM;
			float timeStepS;
		};

		struct MetalGridParameters
		{
			std::uint32_t nx,ny,nz,axis,componentCount;
		};

		struct MetalPeriodicDualParameters
		{
			std::uint32_t nx,ny,nz,component,sweepAxis;
		};

		struct MetalDualLineParameters
		{
			std::uint32_t nx,ny,nz,component,sweepAxis;
			std::uint32_t lineLength,lineCount,componentBeginning,componentPeriodic;
			std::uint32_t lowerComponentWall,upperComponentWall;
		};

		struct CompatibleDualParams
		{
			std::uint32_t nx,ny,nz,derivative;
			std::uint32_t lowerX,upperX,lowerY,upperY,lowerZ,upperZ;
			float cellWidthM;
		};

		thread_local std::uint64_t MetalCommandCommitCount=0u;
		thread_local std::uint64_t MetalHostBufferReadCount=0u;
		std::mutex ProducerKernelNameMutex;
		std::map<const void*,std::string> ProducerKernelNames;
		id<MTLComputePipelineState> NameProducerKernel(id<MTLComputePipelineState> pipeline,
			const char* name)
		{
			if(pipeline&&std::getenv("RISE_FIRE_PRODUCER_KERNEL_PROFILE")){
				std::lock_guard<std::mutex> lock(ProducerKernelNameMutex);
				ProducerKernelNames[(__bridge const void*)pipeline]=name;
			}
			return pipeline;
		}
		class ProducerKernelProfile;
		thread_local ProducerKernelProfile* ActiveProducerKernelProfile=nullptr;
		class ProducerKernelProfile
		{
			struct Row {id<MTLComputeCommandEncoder> encoder;std::string name;
				std::size_t threads=0u;unsigned int dispatches=0u;};
			id<MTLCommandBuffer> command_;
			id<MTLCounterSampleBuffer> samples_;
			std::vector<Row> rows_;
			MTLTimestamp cpuBegin_=0u,gpuBegin_=0u;
			unsigned int stage_,iteration_;
			bool enabled_,valid_;
		public:
			ProducerKernelProfile(id<MTLCommandBuffer> command,unsigned int stage,unsigned int iteration)
				:command_(command),samples_(nil),stage_(stage),iteration_(iteration),
				enabled_(std::getenv("RISE_FIRE_PRODUCER_KERNEL_PROFILE")!=nullptr),valid_(!enabled_)
			{
				if(!enabled_)return;
				if(std::strcmp(std::getenv("RISE_FIRE_PRODUCER_KERNEL_PROFILE"),"1")!=0||
					ActiveProducerKernelProfile||!command)return;
				id<MTLDevice> device=[command device];
				if(![device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary])return;
				id<MTLCounterSet> timestamps=nil;
				for(id<MTLCounterSet> candidate in [device counterSets])
					if([[candidate name] isEqualToString:MTLCommonCounterSetTimestamp])timestamps=candidate;
				if(!timestamps)return;
				MTLCounterSampleBufferDescriptor* descriptor=[MTLCounterSampleBufferDescriptor new];
				descriptor.counterSet=timestamps;descriptor.storageMode=MTLStorageModeShared;
				descriptor.sampleCount=1024u;NSError* error=nil;
				samples_=[device newCounterSampleBufferWithDescriptor:descriptor error:&error];
				if(!samples_)return;
				[device sampleTimestamps:&cpuBegin_ gpuTimestamp:&gpuBegin_];
				valid_=true;ActiveProducerKernelProfile=this;
			}
			~ProducerKernelProfile(){if(ActiveProducerKernelProfile==this)ActiveProducerKernelProfile=nullptr;}
			bool Valid()const{return valid_;}
			id<MTLComputeCommandEncoder> Encoder(id<MTLCommandBuffer> command)
			{
				if(command!=command_)return [command computeCommandEncoder];
				if(!valid_||2u*(rows_.size()+1u)>[samples_ sampleCount]){valid_=false;return nil;}
				MTLComputePassDescriptor* pass=[MTLComputePassDescriptor computePassDescriptor];
				pass.dispatchType=MTLDispatchTypeSerial;
				pass.sampleBufferAttachments[0].sampleBuffer=samples_;
				pass.sampleBufferAttachments[0].startOfEncoderSampleIndex=2u*rows_.size();
				pass.sampleBufferAttachments[0].endOfEncoderSampleIndex=2u*rows_.size()+1u;
				id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoderWithDescriptor:pass];
				if(encoder)rows_.push_back(Row{encoder,"",0u,0u});else valid_=false;
				return encoder;
			}
			void DispatchRecord(id<MTLComputeCommandEncoder> encoder,id<MTLComputePipelineState> pipeline,
				std::size_t threads)
			{
				for(Row& row:rows_)if(row.encoder==encoder){
					std::lock_guard<std::mutex> lock(ProducerKernelNameMutex);
					auto found=ProducerKernelNames.find((__bridge const void*)pipeline);
					if(found==ProducerKernelNames.end()){valid_=false;return;}
					row.name=found->second;row.threads=threads;++row.dispatches;return;
				}
			}
			bool Report()
			{
				if(!enabled_)return true;
				if(!valid_||[command_ status]!=MTLCommandBufferStatusCompleted||rows_.empty())return false;
				MTLTimestamp cpuEnd=0u,gpuEnd=0u;
				[[command_ device] sampleTimestamps:&cpuEnd gpuTimestamp:&gpuEnd];
				if(cpuEnd<=cpuBegin_||gpuEnd<=gpuBegin_)return false;
				NSData* data=[samples_ resolveCounterRange:NSMakeRange(0u,2u*rows_.size())];
				if(!data||[data length]!=2u*rows_.size()*sizeof(MTLCounterResultTimestamp))return false;
				const auto* times=static_cast<const MTLCounterResultTimestamp*>([data bytes]);
				const double millisecondsPerTick=static_cast<double>(cpuEnd-cpuBegin_)/
					static_cast<double>(gpuEnd-gpuBegin_)/1e6;
				// MTLDevice's correlated CPU timestamps are nanoseconds (not raw
				// mach_absolute_time ticks). Encoder intervals may overlap; their sum
				// is not an exclusive decomposition of command elapsed time.
				std::fprintf(stderr,"PRODUCER_COMMAND_V1 stage=%u raw_iteration=%u "
					"cpu_begin=%llu cpu_end=%llu gpu_begin=%llu gpu_end=%llu "
					"device_ms=%.17g encoders=%zu interval_scope=possibly_overlapping\n",
					stage_,iteration_,static_cast<unsigned long long>(cpuBegin_),
					static_cast<unsigned long long>(cpuEnd),static_cast<unsigned long long>(gpuBegin_),
					static_cast<unsigned long long>(gpuEnd),
					([command_ GPUEndTime]-[command_ GPUStartTime])*1000.0,rows_.size());
				for(std::size_t index=0u;index<rows_.size();++index){const Row& row=rows_[index];
					const std::uint64_t begin=times[2u*index].timestamp,end=times[2u*index+1u].timestamp;
					if(row.dispatches!=1u||begin==MTLCounterErrorValue||end==MTLCounterErrorValue||
						end<begin||begin<gpuBegin_||end>gpuEnd)return false;
					std::fprintf(stderr,"PRODUCER_KERNEL_V1 stage=%u raw_iteration=%u ordinal=%zu "
						"kernel=%s threads=%zu begin_tick=%llu end_tick=%llu ms_per_tick=%.17g device_ms=%.17g\n",
						stage_,iteration_,index,row.name.c_str(),row.threads,
						static_cast<unsigned long long>(begin),static_cast<unsigned long long>(end),
						millisecondsPerTick,(end-begin)*millisecondsPerTick);
				}
				return true;
			}
		};
		id<MTLComputeCommandEncoder> ProducerProfileEncoder(id<MTLCommandBuffer> command)
		{
			return ActiveProducerKernelProfile?ActiveProducerKernelProfile->Encoder(command):
				[command computeCommandEncoder];
		}

		id<MTLCommandBuffer> TrackedMetalCommandBuffer( id<MTLCommandQueue> queue )
		{
			return [queue commandBuffer];
		}

		void CommitTrackedMetalCommand( id<MTLCommandBuffer> command )
		{
			++MetalCommandCommitCount;[command commit];
		}

		void* ReadTrackedMetalBuffer( id<MTLBuffer> buffer )
		{
			++MetalHostBufferReadCount;return [buffer contents];
		}

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
struct Params { uint n; uint lines; uint comps; uint lowerBoundary; uint upperBoundary; uint ambientPerLine; float dx; float dt; };
struct GridParams { uint nx; uint ny; uint nz; uint axis; uint comps; };
inline uint value_index(constant Params& p,uint c,uint l,uint i){return (c*p.lines+l)*p.n+i;}
inline uint flux_index(constant Params& p,uint c,uint l,uint f){return (c*p.lines+l)*(p.n+1u)+f;}
inline uint grid_cell(constant GridParams& g,uint x,uint y,uint z){return (z*g.ny+y)*g.nx+x;}
inline uint grid_face(constant GridParams& g,uint axis,uint x,uint y,uint z){
 if(axis==0u)return (z*g.ny+y)*(g.nx+1u)+x;
 if(axis==1u)return (z*(g.ny+1u)+y)*g.nx+x;
 return (z*g.ny+y)*g.nx+x;
}
inline void axis_coordinates(constant GridParams& g,uint line,uint coordinate,
 thread uint& x,thread uint& y,thread uint& z){
 if(g.axis==0u){x=coordinate;y=line%g.ny;z=line/g.ny;return;}
 if(g.axis==1u){x=line%g.nx;y=coordinate;z=line/g.nx;return;}
 x=line%g.nx;y=line/g.nx;z=coordinate;
}
kernel void gather_grid_values(device const float* grid [[buffer(0)]],
 device float* lines [[buffer(1)]],constant GridParams& g [[buffer(2)]],
 uint gid [[thread_position_in_grid]]){
 uint cells=g.nx*g.ny*g.nz,total=g.comps*cells;if(gid>=total)return;
 uint c=gid/cells,cell=gid-c*cells,x=cell%g.nx,y=(cell/g.nx)%g.ny,z=cell/(g.nx*g.ny);
 uint length=g.axis==0u?g.nx:(g.axis==1u?g.ny:g.nz);
 uint line=g.axis==0u?z*g.ny+y:(g.axis==1u?z*g.nx+x:y*g.nx+x);
 uint coordinate=g.axis==0u?x:(g.axis==1u?y:z);
 lines[(c*(cells/length)+line)*length+coordinate]=grid[gid];
}
kernel void scatter_grid_values(device const float* lines [[buffer(0)]],
 device float* grid [[buffer(1)]],constant GridParams& g [[buffer(2)]],
 uint gid [[thread_position_in_grid]]){
 uint cells=g.nx*g.ny*g.nz,total=g.comps*cells;if(gid>=total)return;
 uint c=gid/cells,cell=gid-c*cells,x=cell%g.nx,y=(cell/g.nx)%g.ny,z=cell/(g.nx*g.ny);
 uint length=g.axis==0u?g.nx:(g.axis==1u?g.ny:g.nz);
 uint line=g.axis==0u?z*g.ny+y:(g.axis==1u?z*g.nx+x:y*g.nx+x);
 uint coordinate=g.axis==0u?x:(g.axis==1u?y:z);
 grid[gid]=lines[(c*(cells/length)+line)*length+coordinate];
}
kernel void gather_grid_velocity(device const float* ux [[buffer(0)]],
 device const float* uy [[buffer(1)]],device const float* uz [[buffer(2)]],
 device float* lines [[buffer(3)]],constant GridParams& g [[buffer(4)]],
 uint gid [[thread_position_in_grid]]){
 uint length=g.axis==0u?g.nx:(g.axis==1u?g.ny:g.nz);
 uint lineCount=(g.nx*g.ny*g.nz)/length,total=lineCount*(length+1u);if(gid>=total)return;
 uint line=gid/(length+1u),coordinate=gid-line*(length+1u),x,y,z;
 axis_coordinates(g,line,coordinate,x,y,z);
 device const float* velocity=g.axis==0u?ux:(g.axis==1u?uy:uz);
 lines[gid]=velocity[grid_face(g,g.axis,x,y,z)];
}
inline float ambient_value(device const float* ambient,constant Params& p,uint c,uint l){
 return ambient[p.ambientPerLine!=0u?c*p.lines+l:c];
}
inline float continuous_inflow(float nearest,float ambient,float velocity,bool positive,
 constant Params& p){
 if(!(p.dt>0.0f))return nearest;
 float scale=max(0x1p-126f,max(abs(velocity),abs(p.dx/p.dt)));
 float width=0x1p-24f*scale,signedVelocity=positive?velocity:-velocity;
 if(signedVelocity<=-width)return nearest;
 if(signedVelocity>=width)return ambient;
 float weight=(signedVelocity+width)/(2.0f*width);
 return nearest+weight*(ambient-nearest);
}
inline float sample_value(device const float* q,device const float* u,
 device const float* lowerAmbient,device const float* upperAmbient,
 constant Params& p,uint c,uint l,int i){
 if(p.lowerBoundary==0u&&p.upperBoundary==0u){int n=int(p.n);int w=i%n;if(w<0)w+=n;return q[value_index(p,c,l,uint(w))];}
 if(i<0){float nearest=q[value_index(p,c,l,0u)];if(p.lowerBoundary!=1u)return nearest;
  return continuous_inflow(nearest,ambient_value(lowerAmbient,p,c,l),
   u[l*(p.n+1u)],true,p);}
 if(i>=int(p.n)){float nearest=q[value_index(p,c,l,p.n-1u)];
  if(p.upperBoundary!=1u)return nearest;
  return continuous_inflow(nearest,ambient_value(upperAmbient,p,c,l),
   u[l*(p.n+1u)+p.n],false,p);}
 return q[value_index(p,c,l,uint(i))];
}
inline void unlimited_edges(device const float* q,device const float* u,
 device const float* lowerAmbient,device const float* upperAmbient,
 constant Params& p,uint c,uint l,uint cell,thread float& left,thread float& right){
 int i=int(cell);float im2=sample_value(q,u,lowerAmbient,upperAmbient,p,c,l,i-2);
 float im1=sample_value(q,u,lowerAmbient,upperAmbient,p,c,l,i-1);
 float center=sample_value(q,u,lowerAmbient,upperAmbient,p,c,l,i);
 float ip1=sample_value(q,u,lowerAmbient,upperAmbient,p,c,l,i+1);
 float ip2=sample_value(q,u,lowerAmbient,upperAmbient,p,c,l,i+2);
 if(im2==center&&im1==center&&ip1==center&&ip2==center){left=center;right=center;return;}
 left=(7.0f*(im1+center)-(im2+ip1))/12.0f;
 right=(7.0f*(center+ip1)-(im1+ip2))/12.0f;
}
inline void deviation_range(float dl,float dr,thread float& mn,thread float& mx){
 mn=min(dl,dr);mx=max(dl,dr);float a=3.0f*(dl+dr);float b=-4.0f*dl-2.0f*dr;
 if(a!=0.0f){float s=-b/(2.0f*a);if(s>0.0f&&s<1.0f){float v=(a*s+b)*s+dl;mn=min(mn,v);mx=max(mx,v);}}
}
inline float continuous_shared_alpha(float alpha,float headroom,float d,float center,float envelope){
 float scale=max(0x1p-126f,max(abs(center),max(abs(envelope),abs(d))));
 float width=0x1p-10f*scale;
 float numerator=headroom+max(0.0f,-d);
 float denominator=max(d,width);
 return min(alpha,min(1.0f,numerator/denominator));
}
kernel void reconstruct(device const float* q [[buffer(0)]],device const float* u [[buffer(1)]],
 device const float* lowerAmbient [[buffer(2)]],device const float* upperAmbient [[buffer(3)]],
 device float* left [[buffer(4)]],device float* right [[buffer(5)]],
 device float* alphaOut [[buffer(6)]],constant Params& p [[buffer(7)]],
 uint gid [[thread_position_in_grid]]){
 if(gid>=p.n*p.lines)return;uint l=gid/p.n;uint cell=gid-l*p.n;float alpha=1.0f;
 for(uint c=0;c<p.comps;++c){uint index=value_index(p,c,l,cell);float ql,qr;
  unlimited_edges(q,u,lowerAmbient,upperAmbient,p,c,l,cell,ql,qr);left[index]=ql;right[index]=qr;
  float center=q[index],mnDev,mxDev;deviation_range(ql-center,qr-center,mnDev,mxDev);
  float qm=sample_value(q,u,lowerAmbient,upperAmbient,p,c,l,int(cell)-1);
  float qp=sample_value(q,u,lowerAmbient,upperAmbient,p,c,l,int(cell)+1);
  float mn=min(center,min(qm,qp)),mx=max(center,max(qm,qp));
  alpha=continuous_shared_alpha(alpha,mx-center,mxDev,center,mx);
  alpha=continuous_shared_alpha(alpha,center-mn,-mnDev,center,mn);
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
 device const float* lowerAmbient,device const float* upperAmbient,
 constant Params& p,uint c,uint l,uint face,float courant,float velocity){
 float magnitude=abs(courant),leftNearest=q[value_index(p,c,l,0u)];
 float rightNearest=q[value_index(p,c,l,p.n-1u)];
 float leftExtension=p.lowerBoundary==1u?continuous_inflow(leftNearest,
  ambient_value(lowerAmbient,p,c,l),velocity,true,p):leftNearest;
 float rightExtension=p.upperBoundary==1u?continuous_inflow(rightNearest,
  ambient_value(upperAmbient,p,c,l),velocity,false,p):rightNearest;
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
 device const float* lowerAmbient [[buffer(2)]],device const float* upperAmbient [[buffer(3)]],
 device const float* left [[buffer(4)]],device const float* right [[buffer(5)]],
 device const float* prefix [[buffer(6)]],device float* flux [[buffer(7)]],
 constant Params& p [[buffer(8)]],
 uint gid [[thread_position_in_grid]]){
 bool periodic=p.lowerBoundary==0u&&p.upperBoundary==0u;
 uint faces=p.n+1u,activeFaces=periodic?p.n:faces,total=p.comps*p.lines*activeFaces;
 if(gid>=total)return;uint c=gid/(p.lines*activeFaces);uint rem=gid-c*p.lines*activeFaces;
 uint l=rem/activeFaces;uint f=rem-l*activeFaces;
 uint output=flux_index(p,c,l,f);
 if((f==0u&&p.lowerBoundary==2u)||(f==p.n&&p.upperBoundary==2u)){flux[output]=0.0f;return;}
 float velocity=u[l*faces+f],courant=p.dt*velocity/p.dx;
 float swept=periodic?periodic_swept(q,left,right,prefix,p,c,l,f,courant):
  open_swept(q,left,right,lowerAmbient,upperAmbient,p,c,l,f,courant,velocity);
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
kernel void extract_gas_mass_dose(device const float* flux [[buffer(0)]],
 device float* gasMassDose [[buffer(1)]],constant GridParams& g [[buffer(2)]],
 uint gid [[thread_position_in_grid]]){
 uint faceCount=g.axis==0u?(g.nx+1u)*g.ny*g.nz:
  (g.axis==1u?g.nx*(g.ny+1u)*g.nz:g.nx*g.ny*(g.nz+1u));
 if(gid>=faceCount)return;uint x,y,z,line,coordinate,length,lines;
 if(g.axis==0u){x=gid%(g.nx+1u);uint r=gid/(g.nx+1u);y=r%g.ny;z=r/g.ny;
  line=z*g.ny+y;coordinate=x;length=g.nx;lines=g.ny*g.nz;
 }else if(g.axis==1u){x=gid%g.nx;uint r=gid/g.nx;y=r%(g.ny+1u);z=r/(g.ny+1u);
  line=z*g.nx+x;coordinate=y;length=g.ny;lines=g.nx*g.nz;
 }else{x=gid%g.nx;uint r=gid/g.nx;y=r%g.ny;z=r/g.ny;
  line=y*g.nx+x;coordinate=z;length=g.nz;lines=g.nx*g.ny;}
 float dose=0.0f;for(uint c=1u;c<=6u;++c)
  dose+=flux[(c*lines+line)*(length+1u)+coordinate];
 gasMassDose[gid]=dose;
}
struct DualParams { uint nx; uint ny; uint nz; uint component; uint sweepAxis; };
struct DualLineParams { uint nx; uint ny; uint nz; uint component; uint sweepAxis;
 uint n; uint lines; uint componentBeginning; uint componentPeriodic;
 uint lowerComponentWall; uint upperComponentWall; };
inline uint dual_line_extent(constant DualLineParams& p,uint axis){return axis==0u?p.nx:(axis==1u?p.ny:p.nz);}
inline uint dual_line_face_index(constant DualLineParams& p,uint axis,uint x,uint y,uint z){
 if(axis==0u)return (z*p.ny+y)*(p.nx+1u)+x;
 if(axis==1u)return (z*(p.ny+1u)+y)*p.nx+x;
 return (z*p.ny+y)*p.nx+x;
}
inline void dual_line_face_coordinates(constant DualLineParams& p,uint axis,uint face,
 thread uint& x,thread uint& y,thread uint& z){
 if(axis==0u){x=face%(p.nx+1u);uint r=face/(p.nx+1u);y=r%p.ny;z=r/p.ny;return;}
 if(axis==1u){x=face%p.nx;uint r=face/p.nx;y=r%(p.ny+1u);z=r/(p.ny+1u);return;}
 x=face%p.nx;uint r=face/p.nx;y=r%p.ny;z=r/p.ny;
}
inline uint dual_extent(constant DualParams& p,uint axis){return axis==0u?p.nx:(axis==1u?p.ny:p.nz);}
inline uint dual_face_index(constant DualParams& p,uint axis,uint x,uint y,uint z){
 if(axis==0u)return (z*p.ny+y)*(p.nx+1u)+x;
 if(axis==1u)return (z*(p.ny+1u)+y)*p.nx+x;
 return (z*p.ny+y)*p.nx+x;
}
inline void dual_face_coordinates(constant DualParams& p,uint axis,uint face,
 thread uint& x,thread uint& y,thread uint& z){
 if(axis==0u){x=face%(p.nx+1u);uint r=face/(p.nx+1u);y=r%p.ny;z=r/p.ny;return;}
 if(axis==1u){x=face%p.nx;uint r=face/p.nx;y=r%(p.ny+1u);z=r/(p.ny+1u);return;}
 x=face%p.nx;uint r=face/p.nx;y=r%p.ny;z=r/p.ny;
}
inline void dual_set_coordinate(uint axis,uint value,thread uint& x,thread uint& y,thread uint& z){
 if(axis==0u)x=value;else if(axis==1u)y=value;else z=value;
}
inline uint dual_coordinate(uint axis,uint x,uint y,uint z){return axis==0u?x:(axis==1u?y:z);}
struct CompatibleDualParams { uint nx; uint ny; uint nz; uint derivative;
 uint lowerX; uint upperX; uint lowerY; uint upperY; uint lowerZ; uint upperZ; float dx; };
inline uint compatible_extent(constant CompatibleDualParams& p,uint axis){
 return axis==0u?p.nx:(axis==1u?p.ny:p.nz);}
inline uint compatible_boundary(constant CompatibleDualParams& p,uint side){
 if(side==0u)return p.lowerX;if(side==1u)return p.upperX;
 if(side==2u)return p.lowerY;if(side==3u)return p.upperY;
 return side==4u?p.lowerZ:p.upperZ;
}
inline uint compatible_face_index(constant CompatibleDualParams& p,uint axis,uint x,uint y,uint z){
 if(axis==0u)return (z*p.ny+y)*(p.nx+1u)+x;
 if(axis==1u)return (z*(p.ny+1u)+y)*p.nx+x;
 return (z*p.ny+y)*p.nx+x;
}
inline uint compatible_face_count(constant CompatibleDualParams& p,uint axis){
 return axis==0u?(p.nx+1u)*p.ny*p.nz:
  (axis==1u?p.nx*(p.ny+1u)*p.nz:p.nx*p.ny*(p.nz+1u));
}
inline uint compatible_offset(constant CompatibleDualParams& p,uint component){
 if(component==0u)return 0u;if(component==1u)return compatible_face_count(p,0u);
 return compatible_face_count(p,0u)+compatible_face_count(p,1u);
}
inline void compatible_face_coordinates(constant CompatibleDualParams& p,uint axis,uint face,
 thread uint& x,thread uint& y,thread uint& z){
 if(axis==0u){x=face%(p.nx+1u);uint r=face/(p.nx+1u);y=r%p.ny;z=r/p.ny;return;}
 if(axis==1u){x=face%p.nx;uint r=face/p.nx;y=r%(p.ny+1u);z=r/(p.ny+1u);return;}
 x=face%p.nx;uint r=face/p.nx;y=r%p.ny;z=r/p.ny;
}
inline void compatible_set_coordinate(uint axis,uint value,thread uint& x,thread uint& y,thread uint& z){
 if(axis==0u)x=value;else if(axis==1u)y=value;else z=value;
}
inline uint compatible_coordinate(uint axis,uint x,uint y,uint z){return axis==0u?x:(axis==1u?y:z);}
inline float compatible_velocity(device const float* density,device const float* momentum,
 constant CompatibleDualParams& p,uint component,uint x,uint y,uint z){
 uint offset=compatible_offset(p,component),face=compatible_face_index(p,component,x,y,z);
 return momentum[offset+face]/density[offset+face];
}
inline float compatible_dose(device const float* massDose,
 constant CompatibleDualParams& p,uint x,uint y,uint z){
 return massDose[compatible_face_index(p,p.derivative,x,y,z)];
}
inline float compatible_restricted_dose(device const float* massDose,
 constant CompatibleDualParams& p,uint component,uint componentLower,uint componentUpper,
 uint boundary,uint x,uint y,uint z){
 uint lx=x,ly=y,lz=z,ux=x,uy=y,uz=z;
 compatible_set_coordinate(component,componentLower,lx,ly,lz);
 compatible_set_coordinate(component,componentUpper,ux,uy,uz);
 compatible_set_coordinate(p.derivative,boundary,lx,ly,lz);
 compatible_set_coordinate(p.derivative,boundary,ux,uy,uz);
 return 0.5f*(compatible_dose(massDose,p,lx,ly,lz)+
  compatible_dose(massDose,p,ux,uy,uz));
}
kernel void compatible_dual_update(device const float* density [[buffer(0)]],
 device const float* momentum [[buffer(1)]],device const float* massDose [[buffer(2)]],
 device float* updatedDensity [[buffer(3)]],device float* updatedMomentum [[buffer(4)]],
 constant CompatibleDualParams& p [[buffer(5)]],uint gid [[thread_position_in_grid]]){
#pragma clang fp contract(off)
 uint count0=compatible_face_count(p,0u),count1=compatible_face_count(p,1u);
 uint total=count0+count1+compatible_face_count(p,2u);if(gid>=total)return;
 uint component=gid<count0?0u:(gid<count0+count1?1u:2u);
 uint offset=compatible_offset(p,component),local=gid-offset,x,y,z;
 compatible_face_coordinates(p,component,local,x,y,z);
 uint componentExtent=compatible_extent(p,component),normal=compatible_coordinate(component,x,y,z);
 bool componentPeriodic=compatible_boundary(p,2u*component)==0u&&
  compatible_boundary(p,2u*component+1u)==0u;
 bool seam=componentPeriodic&&normal==componentExtent;
 if(seam){normal=0u;compatible_set_coordinate(component,0u,x,y,z);}
 uint sourceFace=compatible_face_index(p,component,x,y,z),source=offset+sourceFace;
 bool wall=(normal==0u&&compatible_boundary(p,2u*component)==2u)||
  (normal==componentExtent&&compatible_boundary(p,2u*component+1u)==2u);
 float upperMass=0.0f,lowerMass=0.0f,upperVelocity=0.0f,lowerVelocity=0.0f;
 if(p.derivative==component){
  uint previous=componentPeriodic?(normal==0u?componentExtent-1u:normal-1u):
   (normal==0u?0u:normal-1u);
  uint next=componentPeriodic?(normal+1u==componentExtent?0u:normal+1u):
   (normal+1u<componentExtent+1u?normal+1u:normal);
  uint px=x,py=y,pz=z,nx=x,ny=y,nz=z;
  compatible_set_coordinate(component,previous,px,py,pz);
  compatible_set_coordinate(component,next,nx,ny,nz);
  lowerMass=0.5f*(compatible_dose(massDose,p,px,py,pz)+compatible_dose(massDose,p,x,y,z));
  upperMass=0.5f*(compatible_dose(massDose,p,x,y,z)+compatible_dose(massDose,p,nx,ny,nz));
  lowerVelocity=0.5f*(compatible_velocity(density,momentum,p,component,px,py,pz)+
   compatible_velocity(density,momentum,p,component,x,y,z));
  upperVelocity=0.5f*(compatible_velocity(density,momentum,p,component,x,y,z)+
   compatible_velocity(density,momentum,p,component,nx,ny,nz));
 }else{
  uint derivativeExtent=compatible_extent(p,p.derivative);
  uint position=compatible_coordinate(p.derivative,x,y,z);
  uint componentLower=componentPeriodic?(normal==0u?componentExtent-1u:normal-1u):
   (normal==0u?0u:normal-1u);
  uint componentUpper=componentPeriodic?normal:min(normal,componentExtent-1u);
  lowerMass=compatible_restricted_dose(massDose,p,component,componentLower,
   componentUpper,position,x,y,z);
  upperMass=compatible_restricted_dose(massDose,p,component,componentLower,
   componentUpper,position+1u,x,y,z);
  if(!componentPeriodic&&(normal==0u||normal==componentExtent)){
   lowerMass*=0.5f;upperMass*=0.5f;
  }
  if(position==0u&&compatible_boundary(p,2u*p.derivative)==2u)lowerMass=0.0f;
  if(position+1u==derivativeExtent&&
   compatible_boundary(p,2u*p.derivative+1u)==2u)upperMass=0.0f;
  bool derivativePeriodic=compatible_boundary(p,2u*p.derivative)==0u&&
   compatible_boundary(p,2u*p.derivative+1u)==0u;
  uint previous=derivativePeriodic?(position==0u?derivativeExtent-1u:position-1u):
   (position==0u?0u:position-1u);
  uint next=derivativePeriodic?(position+1u==derivativeExtent?0u:position+1u):
   min(position+1u,derivativeExtent-1u);
  uint px=x,py=y,pz=z,nx=x,ny=y,nz=z;
  compatible_set_coordinate(p.derivative,previous,px,py,pz);
  compatible_set_coordinate(p.derivative,next,nx,ny,nz);
  lowerVelocity=0.5f*(compatible_velocity(density,momentum,p,component,px,py,pz)+
   compatible_velocity(density,momentum,p,component,x,y,z));
  upperVelocity=0.5f*(compatible_velocity(density,momentum,p,component,x,y,z)+
   compatible_velocity(density,momentum,p,component,nx,ny,nz));
 }
 updatedDensity[gid]=density[source]-(upperMass-lowerMass)/p.dx;
 updatedMomentum[gid]=wall?0.0f:momentum[source]-(upperMass*upperVelocity-
  lowerMass*lowerVelocity)/p.dx;
}
kernel void gather_periodic_dual_values(device const float* density [[buffer(0)]],
 device const float* momentum [[buffer(1)]],device float* values [[buffer(2)]],
 constant DualParams& p [[buffer(3)]],uint gid [[thread_position_in_grid]]){
 uint cells=p.nx*p.ny*p.nz;if(gid>=2u*cells)return;uint channel=gid/cells,cell=gid-channel*cells;
 uint x=cell%p.nx,y=(cell/p.nx)%p.ny,z=cell/(p.nx*p.ny);
 uint face=dual_face_index(p,p.component,x,y,z);values[gid]=channel==0u?density[face]:momentum[face];
}
kernel void gather_periodic_dual_carrier(device const float* ux [[buffer(0)]],
 device const float* uy [[buffer(1)]],device const float* uz [[buffer(2)]],
 device float* carrier [[buffer(3)]],constant DualParams& p [[buffer(4)]],
 uint gid [[thread_position_in_grid]]){
 uint faceCount=p.sweepAxis==0u?(p.nx+1u)*p.ny*p.nz:
  (p.sweepAxis==1u?p.nx*(p.ny+1u)*p.nz:p.nx*p.ny*(p.nz+1u));
 if(gid>=faceCount)return;uint x,y,z;dual_face_coordinates(p,p.sweepAxis,gid,x,y,z);
 uint averageAxis=p.component==p.sweepAxis?p.sweepAxis:p.component;
 uint extent=dual_extent(p,averageAxis),coordinate=dual_coordinate(averageAxis,x,y,z);
 uint canonical=coordinate==extent?0u:coordinate,previous=canonical==0u?extent-1u:canonical-1u;
 uint lx=x,ly=y,lz=z,uxc=x,uyc=y,uzc=z;
 dual_set_coordinate(averageAxis,previous,lx,ly,lz);
 dual_set_coordinate(averageAxis,canonical,uxc,uyc,uzc);
 if(p.sweepAxis!=averageAxis){
  uint sweepExtent=dual_extent(p,p.sweepAxis);
  uint sweepCoordinate=dual_coordinate(p.sweepAxis,x,y,z);
  uint sweepCanonical=sweepCoordinate==sweepExtent?0u:sweepCoordinate;
  dual_set_coordinate(p.sweepAxis,sweepCanonical,lx,ly,lz);
  dual_set_coordinate(p.sweepAxis,sweepCanonical,uxc,uyc,uzc);
 }
 device const float* velocity=p.sweepAxis==0u?ux:(p.sweepAxis==1u?uy:uz);
 float lower=velocity[dual_face_index(p,p.sweepAxis,lx,ly,lz)];
 float upper=velocity[dual_face_index(p,p.sweepAxis,uxc,uyc,uzc)];
 carrier[gid]=0.5f*(lower+upper);
}
kernel void scatter_periodic_dual_values(device const float* values [[buffer(0)]],
 device float* density [[buffer(1)]],device float* momentum [[buffer(2)]],
 constant DualParams& p [[buffer(3)]],uint gid [[thread_position_in_grid]]){
 uint cells=p.nx*p.ny*p.nz;if(gid>=2u*cells)return;uint channel=gid/cells,cell=gid-channel*cells;
 uint x=cell%p.nx,y=(cell/p.nx)%p.ny,z=cell/(p.nx*p.ny);
 uint face=dual_face_index(p,p.component,x,y,z);
 if(channel==0u)density[face]=values[gid];else momentum[face]=values[gid];
}
kernel void publish_periodic_dual_seam(device float* density [[buffer(0)]],
 device float* momentum [[buffer(1)]],constant DualParams& p [[buffer(2)]],
 uint gid [[thread_position_in_grid]]){
 uint firstExtent=p.component==0u?p.ny:p.nx;
 uint secondExtent=p.component==2u?p.ny:p.nz;if(gid>=firstExtent*secondExtent)return;
 uint first=gid%firstExtent,second=gid/firstExtent;
 uint lx=p.component==0u?0u:first;
 uint ly=p.component==0u?first:(p.component==1u?0u:second);
 uint lz=p.component==2u?0u:second;uint hx=lx,hy=ly,hz=lz;
 dual_set_coordinate(p.component,dual_extent(p,p.component),hx,hy,hz);
 uint low=dual_face_index(p,p.component,lx,ly,lz),high=dual_face_index(p,p.component,hx,hy,hz);
 density[high]=density[low];momentum[high]=momentum[low];
}
kernel void gather_dual_line_values(device const float* density [[buffer(0)]],
 device const float* momentum [[buffer(1)]],device float* values [[buffer(2)]],
 constant DualLineParams& p [[buffer(3)]],uint gid [[thread_position_in_grid]]){
 uint valueCount=2u*p.lines*p.n;if(gid>=valueCount)return;
 uint channel=gid/(p.lines*p.n),rem=gid-channel*p.lines*p.n;
 uint line=rem/p.n,coordinate=rem-line*p.n,x=0u,y=0u,z=0u;
 if(p.component==p.sweepAxis){uint first=(p.component+1u)%3u,second=(p.component+2u)%3u;
  uint firstExtent=dual_line_extent(p,first);
  dual_set_coordinate(p.component,p.componentPeriodic!=0u?coordinate:coordinate+1u,x,y,z);
  dual_set_coordinate(first,line%firstExtent,x,y,z);dual_set_coordinate(second,line/firstExtent,x,y,z);
 }else{uint remainingAxis=3u-p.component-p.sweepAxis;
  uint remainingExtent=dual_line_extent(p,remainingAxis);
  dual_set_coordinate(p.component,p.componentBeginning+line/remainingExtent,x,y,z);
  dual_set_coordinate(p.sweepAxis,coordinate,x,y,z);
  dual_set_coordinate(remainingAxis,line%remainingExtent,x,y,z);
 }
 uint face=dual_line_face_index(p,p.component,x,y,z);
 values[gid]=channel==0u?density[face]:momentum[face];
}
kernel void scatter_dual_line_values(device const float* values [[buffer(0)]],
 device float* density [[buffer(1)]],device float* momentum [[buffer(2)]],
 constant DualLineParams& p [[buffer(3)]],uint gid [[thread_position_in_grid]]){
 uint valueCount=2u*p.lines*p.n;if(gid>=valueCount)return;
 uint channel=gid/(p.lines*p.n),rem=gid-channel*p.lines*p.n;
 uint line=rem/p.n,coordinate=rem-line*p.n,x=0u,y=0u,z=0u;
 if(p.component==p.sweepAxis){uint first=(p.component+1u)%3u,second=(p.component+2u)%3u;
  uint firstExtent=dual_line_extent(p,first);
  dual_set_coordinate(p.component,p.componentPeriodic!=0u?coordinate:coordinate+1u,x,y,z);
  dual_set_coordinate(first,line%firstExtent,x,y,z);dual_set_coordinate(second,line/firstExtent,x,y,z);
 }else{uint remainingAxis=3u-p.component-p.sweepAxis;
  uint remainingExtent=dual_line_extent(p,remainingAxis);
  dual_set_coordinate(p.component,p.componentBeginning+line/remainingExtent,x,y,z);
  dual_set_coordinate(p.sweepAxis,coordinate,x,y,z);
  dual_set_coordinate(remainingAxis,line%remainingExtent,x,y,z);
 }
 uint face=dual_line_face_index(p,p.component,x,y,z);
 if(channel==0u)density[face]=values[gid];else momentum[face]=values[gid];
}
kernel void prescribe_dual_component_walls(device float* momentum [[buffer(0)]],
 constant DualLineParams& p [[buffer(1)]],uint gid [[thread_position_in_grid]]){
 uint faceCount=p.component==0u?(p.nx+1u)*p.ny*p.nz:
  (p.component==1u?p.nx*(p.ny+1u)*p.nz:p.nx*p.ny*(p.nz+1u));
 if(gid>=faceCount)return;uint x,y,z;
 dual_line_face_coordinates(p,p.component,gid,x,y,z);
 uint coordinate=dual_coordinate(p.component,x,y,z),extent=dual_line_extent(p,p.component);
 if((coordinate==0u&&p.lowerComponentWall!=0u)||(coordinate==extent&&p.upperComponentWall!=0u))
  momentum[gid]=0.0f;
}
kernel void add_cell_sources(device float* conservative [[buffer(0)]],
 device const float* source [[buffer(1)]],constant GridParams& p [[buffer(2)]],
 uint gid [[thread_position_in_grid]]){
 uint cells=p.nx*p.ny*p.nz,total=p.comps*cells;if(gid>=total)return;
 conservative[gid]+=source[gid];
}
kernel void extract_gas_density(device const float* conservative [[buffer(0)]],
 device float* density [[buffer(1)]],constant GridParams& p [[buffer(2)]],
 uint gid [[thread_position_in_grid]]){
 uint cells=p.nx*p.ny*p.nz;if(gid>=cells)return;
 // The authoritative tuple is rho_tot Z, seven record-ordered constituent
 // densities, and sensible enthalpy.  Only CH4..CO (components 1..6) are gas.
 float gas=conservative[cells+gid];
 gas+=conservative[2u*cells+gid];gas+=conservative[3u*cells+gid];
 gas+=conservative[4u*cells+gid];gas+=conservative[5u*cells+gid];
 gas+=conservative[6u*cells+gid];density[gid]=gas;
}
kernel void add_face_sources(device float* momentum [[buffer(0)]],
 device const float* source [[buffer(1)]],constant uint& count [[buffer(2)]],
 uint gid [[thread_position_in_grid]]){if(gid<count)momentum[gid]+=source[gid];}
struct ManifoldParams { uint cells; uint affineRows; uint affineDimension; uint reserved;
 float Tmin; float Tmax; float pressure; float feasibility; };
inline float methane_enthalpy(device const float* thermo,uint species,float temperature){
 device const float* record=thermo+32u*species;uint segmentCount=uint(record[1]);
 device const float* selected=record+2u;
 for(uint segment=0u;segment<segmentCount;++segment){device const float* candidate=record+2u+10u*segment;
  if(temperature>=candidate[0]&&(temperature<candidate[1]||
   (segment+1u==segmentCount&&temperature==candidate[1])))selected=candidate;}
 float inverse=1.0f/temperature,logT=log(temperature),t2=temperature*temperature;
 float t3=t2*temperature,t4=t3*temperature,t5=t4*temperature;
 float primitive=-selected[2]*inverse+selected[3]*logT+selected[4]*temperature+
  selected[5]*t2/2.0f+selected[6]*t3/3.0f+selected[7]*t4/4.0f+
  selected[8]*t5/5.0f;
 return 8314.46261815324f/record[0]*primitive+selected[9];
}
inline bool methane_state_admissible(device const float* state,device const float* thermo,
 constant ManifoldParams& p,uint cell){
 float values[9];for(uint component=0u;component<9u;++component){
  values[component]=state[component*p.cells+cell];if(!isfinite(values[component]))return false;}
 float totalMass=0.0f,massScale=abs(values[0]);for(uint species=0u;species<7u;++species){
  totalMass+=values[species+1u];massScale+=abs(values[species+1u]);}
 if(!(totalMass>0.0f)||!isfinite(totalMass))return false;massScale=max(1.0f,massScale);
 if(-values[0]>p.feasibility*massScale)return false;float closure=values[0];
 for(uint species=0u;species<7u;++species){closure-=values[species+1u];
  if(-values[species+1u]>p.feasibility*massScale)return false;}
 if(closure>p.feasibility*massScale)return false;
 float below=-values[8],above=values[8],energyScale=abs(values[8]);
 for(uint species=0u;species<7u;++species){float lowerH=methane_enthalpy(thermo,species,p.Tmin);
  float upperH=methane_enthalpy(thermo,species,p.Tmax),density=values[species+1u];
  below+=lowerH*density;above-=upperH*density;
  energyScale+=abs(lowerH*density)+abs(upperH*density);}
 energyScale=max(1.0f,energyScale);
 if(below>p.feasibility*energyScale||above>p.feasibility*energyScale)return false;
 device const float* matrix=thermo+224u;
 for(uint row=0u;row<p.affineRows;++row){float residual=0.0f,scale=0.0f;
  for(uint column=0u;column<p.affineDimension;++column){
   float term=matrix[row*p.affineDimension+column]*values[column];
   residual+=term;scale+=abs(term);}
  if(!isfinite(residual)||abs(residual)>p.feasibility*max(1.0f,scale))return false;}
 return true;
}
inline bool methane_temperature(device const float* state,device const float* thermo,
 constant ManifoldParams& p,uint cell,thread float& temperature){
 float sensible=state[8u*p.cells+cell],lowerEnergy=0.0f,upperEnergy=0.0f,scale=abs(sensible);
 for(uint species=0u;species<7u;++species){float density=state[(species+1u)*p.cells+cell];
  float lowerH=methane_enthalpy(thermo,species,p.Tmin);
  float upperH=methane_enthalpy(thermo,species,p.Tmax);
  lowerEnergy+=density*lowerH;upperEnergy+=density*upperH;
  scale+=abs(density*lowerH)+abs(density*upperH);}
 scale=max(1.0f,scale);float tolerance=p.feasibility*scale;temperature=0.0f;
 if(sensible<=lowerEnergy+tolerance)temperature=p.Tmin;
 else if(sensible>=upperEnergy-tolerance)temperature=p.Tmax;
 else {float lower=p.Tmin,upper=p.Tmax;
  for(uint iteration=0u;iteration<32u;++iteration){float midpoint=0.5f*(lower+upper),energy=0.0f;
   for(uint species=0u;species<7u;++species)
    energy+=state[(species+1u)*p.cells+cell]*methane_enthalpy(thermo,species,midpoint);
   if(energy<sensible)lower=midpoint;else upper=midpoint;}
  temperature=0.5f*(lower+upper);}
 return isfinite(temperature)&&temperature>=p.Tmin&&temperature<=p.Tmax;
}
inline bool methane_volume_ratio(device const float* state,device const float* thermo,
 constant ManifoldParams& p,uint cell,thread float& ratio){
 float temperature=0.0f;if(!methane_temperature(state,thermo,p,cell,temperature))return false;
 float molar=0.0f;for(uint species=0u;species<6u;++species)
  molar+=max(0.0f,state[(species+1u)*p.cells+cell])/thermo[32u*species];
 ratio=molar*8314.46261815324f*temperature/p.pressure;
 return isfinite(ratio)&&ratio>0.0f;
}
kernel void measure_methane_manifold(device const float* beginningDeviation [[buffer(0)]],
 device const float* terminal [[buffer(1)]],device const float* thermo [[buffer(2)]],
 device float2* deviationMap [[buffer(3)]],device atomic_uint* reduction [[buffer(4)]],
 constant ManifoldParams& p [[buffer(5)]],device atomic_uint* highHistogram [[buffer(6)]],
 uint gid [[thread_position_in_grid]]){
 if(gid>=p.cells)return;float terminalRatio=0.0f;
 if(!methane_state_admissible(terminal,thermo,p,gid)||
  !methane_volume_ratio(terminal,thermo,p,gid,terminalRatio)){
  atomic_fetch_or_explicit(reduction+2u,1u,memory_order_relaxed);return;}
 float beginning=beginningDeviation[gid],terminalDeviation=terminalRatio-1.0f;
 if(!isfinite(beginning)){atomic_fetch_or_explicit(reduction+2u,1u,memory_order_relaxed);return;}
 deviationMap[gid]=float2(beginning,terminalDeviation);
 float generation=abs(terminalDeviation-beginning),field=abs(terminalDeviation);
 atomic_fetch_max_explicit(reduction,as_type<uint>(generation),memory_order_relaxed);
 atomic_fetch_max_explicit(reduction+1u,as_type<uint>(field),memory_order_relaxed);
 constexpr float dynamicsBound=0x1p-2f;
 if(field>dynamicsBound){
  float beginningMagnitude=abs(beginning),headroom=dynamicsBound-beginningMagnitude;
  float localDose=field-beginningMagnitude;
  if(!(headroom>0.0f)||!(localDose>0.0f)||!isfinite(headroom)||!isfinite(localDose))
   atomic_fetch_or_explicit(reduction+2u,1u,memory_order_relaxed);
  else atomic_fetch_max_explicit(reduction+3u,as_type<uint>(localDose/headroom),
   memory_order_relaxed);
 }
 atomic_fetch_add_explicit(highHistogram+(as_type<uint>(field)>>16u),1u,memory_order_relaxed);
}
kernel void select_methane_manifold_high_bins(device atomic_uint* histogram [[buffer(0)]],
 device uint* control [[buffer(1)]],constant ManifoldParams& p [[buffer(2)]],
 uint gid [[thread_position_in_grid]]){
 if(gid!=0u)return;
 uint ranks[2]={p.cells==0u?0u:(p.cells-1u)/2u,
  p.cells==0u?0u:((95u*p.cells+99u)/100u)-1u};
 for(uint quantile=0u;quantile<2u;++quantile){uint prefix=0u;
  for(uint bin=0u;bin<65536u;++bin){uint count=atomic_load_explicit(
    histogram+bin,memory_order_relaxed);
   if(ranks[quantile]<prefix+count){control[2u*quantile]=bin;
    control[2u*quantile+1u]=ranks[quantile]-prefix;break;}prefix+=count;}}
}
kernel void histogram_methane_manifold_low_bins(device const float2* deviationMap [[buffer(0)]],
 device const uint* control [[buffer(1)]],device atomic_uint* lowHistograms [[buffer(2)]],
 constant ManifoldParams& p [[buffer(3)]],uint gid [[thread_position_in_grid]]){
 if(gid>=p.cells)return;uint bits=as_type<uint>(abs(deviationMap[gid].y));uint high=bits>>16u;
 if(high==control[0])atomic_fetch_add_explicit(lowHistograms+(bits&65535u),1u,
  memory_order_relaxed);
 if(high==control[2])atomic_fetch_add_explicit(lowHistograms+65536u+(bits&65535u),1u,
  memory_order_relaxed);
}
kernel void select_methane_manifold_low_bins(device atomic_uint* lowHistograms [[buffer(0)]],
 device uint* control [[buffer(1)]],uint gid [[thread_position_in_grid]]){
 if(gid!=0u)return;
 for(uint quantile=0u;quantile<2u;++quantile){uint prefix=0u,target=control[2u*quantile+1u];
  for(uint bin=0u;bin<65536u;++bin){uint count=atomic_load_explicit(
    lowHistograms+quantile*65536u+bin,memory_order_relaxed);
   if(target<prefix+count){control[4u+quantile]=(control[2u*quantile]<<16u)|bin;break;}
   prefix+=count;}}
}
kernel void fold_methane_advective_anomaly_target(device const float2* deviationMap [[buffer(0)]],
 device float* restorationTarget [[buffer(1)]],constant float& inverseTimeStep [[buffer(2)]],
 uint gid [[thread_position_in_grid]]){
 float2 deviation=deviationMap[gid];
 restorationTarget[gid]+=(deviation.y-deviation.x)*inverseTimeStep;
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
			id<MTLComputePipelineState> extractGasMassDose;
			id<MTLComputePipelineState> compatibleDualUpdate;
			id<MTLComputePipelineState> gatherValues;
			id<MTLComputePipelineState> scatterValues;
			id<MTLComputePipelineState> gatherVelocity;
			id<MTLComputePipelineState> gatherPeriodicDualValues;
			id<MTLComputePipelineState> gatherPeriodicDualCarrier;
			id<MTLComputePipelineState> scatterPeriodicDualValues;
			id<MTLComputePipelineState> publishPeriodicDualSeam;
			id<MTLComputePipelineState> gatherDualLineValues;
			id<MTLComputePipelineState> scatterDualLineValues;
			id<MTLComputePipelineState> prescribeDualComponentWalls;
			id<MTLComputePipelineState> addCellSources;
			id<MTLComputePipelineState> extractGasDensity;
			id<MTLComputePipelineState> addFaceSources;
			id<MTLComputePipelineState> measureMethaneManifold;
			id<MTLComputePipelineState> selectMethaneManifoldHighBins;
			id<MTLComputePipelineState> histogramMethaneManifoldLowBins;
			id<MTLComputePipelineState> selectMethaneManifoldLowBins;
			id<MTLComputePipelineState> foldMethaneAdvectiveAnomalyTarget;
			std::string error;

			MetalRemapContext() : device(nil), queue(nil), reconstruct(nil), scan(nil),
				flux(nil), update(nil),extractGasMassDose(nil),compatibleDualUpdate(nil),
				gatherValues(nil),scatterValues(nil),gatherVelocity(nil),
				gatherPeriodicDualValues(nil),gatherPeriodicDualCarrier(nil),
				scatterPeriodicDualValues(nil),publishPeriodicDualSeam(nil),
				gatherDualLineValues(nil),scatterDualLineValues(nil),prescribeDualComponentWalls(nil),
				addCellSources(nil),extractGasDensity(nil),addFaceSources(nil),
				measureMethaneManifold(nil),selectMethaneManifoldHighBins(nil),
				histogramMethaneManifoldLowBins(nil),selectMethaneManifoldLowBins(nil),
				foldMethaneAdvectiveAnomalyTarget(nil)
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
						return function ? NameProducerKernel([device newComputePipelineStateWithFunction:function
							error:&metalError],name) : nil;
					};
					reconstruct=makePipeline("reconstruct");
					scan=makePipeline("scan_lines");
					flux=makePipeline("face_flux");
					update=makePipeline("update_cells");
					extractGasMassDose=makePipeline("extract_gas_mass_dose");
					compatibleDualUpdate=makePipeline("compatible_dual_update");
					gatherValues=makePipeline("gather_grid_values");
					scatterValues=makePipeline("scatter_grid_values");
					gatherVelocity=makePipeline("gather_grid_velocity");
					gatherPeriodicDualValues=makePipeline("gather_periodic_dual_values");
					gatherPeriodicDualCarrier=makePipeline("gather_periodic_dual_carrier");
					scatterPeriodicDualValues=makePipeline("scatter_periodic_dual_values");
					publishPeriodicDualSeam=makePipeline("publish_periodic_dual_seam");
					gatherDualLineValues=makePipeline("gather_dual_line_values");
					scatterDualLineValues=makePipeline("scatter_dual_line_values");
					prescribeDualComponentWalls=makePipeline("prescribe_dual_component_walls");
					addCellSources=makePipeline("add_cell_sources");
					extractGasDensity=makePipeline("extract_gas_density");
					addFaceSources=makePipeline("add_face_sources");
					measureMethaneManifold=makePipeline("measure_methane_manifold");
					selectMethaneManifoldHighBins=makePipeline(
						"select_methane_manifold_high_bins");
					histogramMethaneManifoldLowBins=makePipeline(
						"histogram_methane_manifold_low_bins");
					selectMethaneManifoldLowBins=makePipeline(
						"select_methane_manifold_low_bins");
					foldMethaneAdvectiveAnomalyTarget=
						makePipeline("fold_methane_advective_anomaly_target");
					if( !reconstruct||!scan||!flux||!update||!extractGasMassDose||
						!compatibleDualUpdate||!gatherValues||
						!scatterValues||!gatherVelocity||!gatherPeriodicDualValues||
						!gatherPeriodicDualCarrier||!scatterPeriodicDualValues||
						!publishPeriodicDualSeam||!gatherDualLineValues||
						!scatterDualLineValues||!prescribeDualComponentWalls||
						!addCellSources||!extractGasDensity||!addFaceSources||
						!measureMethaneManifold||!selectMethaneManifoldHighBins||
						!histogramMethaneManifoldLowBins||!selectMethaneManifoldLowBins||
						!foldMethaneAdvectiveAnomalyTarget ) {
						error=MetalError("production fire remap pipeline creation failed",metalError);
						return;
					}
					queue=[device newCommandQueue];
					if( !queue ) error="production fire remap command queue allocation failed";
				}
			}

			// Isolated r183 candidate source. Ordinary production does not compile or
			// create these pipelines; only the explicit diagnostic owner below can
			// instantiate this library.
			static const char* SingleStageFCTSource()
		{
			return R"METAL(
#include <metal_stdlib>
using namespace metal;
struct FCTParams {
 uint nx;uint ny;uint nz;uint cells;uint components;uint inequalities;uint nullity;uint affineRows;
 uint boundary[6];uint sideOffset[6];float dx;float dt;float feasibility;float assemblyReserve;
};
inline uint fct_cell(constant FCTParams& p,uint x,uint y,uint z){return (z*p.ny+y)*p.nx+x;}
inline uint fct_extent(constant FCTParams& p,uint a){return a==0u?p.nx:(a==1u?p.ny:p.nz);}
inline uint fct_face_count(constant FCTParams& p,uint a){return a==0u?(p.nx+1u)*p.ny*p.nz:
 (a==1u?p.nx*(p.ny+1u)*p.nz:p.nx*p.ny*(p.nz+1u));}
inline uint fct_face_offset(constant FCTParams& p,uint a){return a==0u?0u:
 (a==1u?fct_face_count(p,0u):fct_face_count(p,0u)+fct_face_count(p,1u));}
inline uint fct_all_faces(constant FCTParams& p){return fct_face_count(p,0u)+
 fct_face_count(p,1u)+fct_face_count(p,2u);}
inline uint fct_face(constant FCTParams& p,uint a,uint x,uint y,uint z){return a==0u?
 (z*p.ny+y)*(p.nx+1u)+x:(a==1u?(z*(p.ny+1u)+y)*p.nx+x:(z*p.ny+y)*p.nx+x);}
inline uint fct_packed_face(constant FCTParams& p,uint a,uint x,uint y,uint z){return
 fct_face_offset(p,a)+fct_face(p,a,x,y,z);}
inline uint fct_coordinate(uint a,uint x,uint y,uint z){return a==0u?x:(a==1u?y:z);}
inline void fct_set_coordinate(uint a,uint v,thread uint& x,thread uint& y,thread uint& z){
 if(a==0u)x=v;else if(a==1u)y=v;else z=v;}
inline void fct_decode_face(constant FCTParams& p,uint packed,thread uint& a,
 thread uint& x,thread uint& y,thread uint& z){
 uint n0=fct_face_count(p,0u),n1=fct_face_count(p,1u);a=packed<n0?0u:(packed<n0+n1?1u:2u);
 uint local=packed-fct_face_offset(p,a);if(a==0u){x=local%(p.nx+1u);uint r=local/(p.nx+1u);
  y=r%p.ny;z=r/p.ny;}else if(a==1u){x=local%p.nx;uint r=local/p.nx;
  y=r%(p.ny+1u);z=r/(p.ny+1u);}else{x=local%p.nx;uint r=local/p.nx;
  y=r%p.ny;z=r/p.ny;}}
inline uint fct_side_index(constant FCTParams& p,uint side,uint x,uint y,uint z){
 return p.sideOffset[side]+(side<2u?z*p.ny+y:(side<4u?z*p.nx+x:y*p.nx+x));}
inline float fct_stage_value(device const float* q,device const float* ambient,
 device const uchar* inflow,constant FCTParams& p,uint component,uint x,uint y,uint z,
 uint axis,int shift){
 int coordinate=int(fct_coordinate(axis,x,y,z))+shift,intExtent=int(fct_extent(p,axis));
 if(coordinate>=0&&coordinate<intExtent){fct_set_coordinate(axis,uint(coordinate),x,y,z);
  return q[component*p.cells+fct_cell(p,x,y,z)];}
 uint side=2u*axis+(coordinate>=intExtent?1u:0u),kind=p.boundary[side];
 if(kind==0u){uint wrapped=coordinate<0?fct_extent(p,axis)-1u:0u;
  fct_set_coordinate(axis,wrapped,x,y,z);return q[component*p.cells+fct_cell(p,x,y,z)];}
 uint nearest=coordinate<0?0u:fct_extent(p,axis)-1u;fct_set_coordinate(axis,nearest,x,y,z);
 float interior=q[component*p.cells+fct_cell(p,x,y,z)];
 return kind==1u&&inflow[fct_side_index(p,side,x,y,z)]!=0u?ambient[component]:interior;
}
inline float fct_mc(float backward,float forward){if(backward*forward<=0.0f)return 0.0f;
 float centered=0.5f*(backward+forward),sign=centered<0.0f?-1.0f:1.0f;
 return sign*min(abs(centered),2.0f*min(abs(backward),abs(forward)));}
inline float fct_mass_slope(device const float* q,device const float* ambient,
 device const uchar* inflow,device const float* basis,device const float* coordinateProjector,
 constant FCTParams& p,uint component,uint cellX,uint cellY,uint cellZ,uint axis){
 float coordinateSlope[8];for(uint b=0u;b<8u;++b)coordinateSlope[b]=0.0f;
 for(uint b=0u;b<p.nullity;++b){float backward=0.0f,forward=0.0f;
  for(uint row=0u;row<8u;++row){float center=q[row*p.cells+fct_cell(p,cellX,cellY,cellZ)];
   float previous=fct_stage_value(q,ambient,inflow,p,row,cellX,cellY,cellZ,axis,-1);
   float next=fct_stage_value(q,ambient,inflow,p,row,cellX,cellY,cellZ,axis,1);
   float coefficient=basis[row*p.nullity+b];volatile float backwardDifference=center-previous;
   volatile float forwardDifference=next-center;volatile float backwardProduct=
   coefficient*backwardDifference;volatile float forwardProduct=coefficient*forwardDifference;
   backward+=backwardProduct;forward+=forwardProduct;}coordinateSlope[b]=fct_mc(backward,forward);}
 float result=0.0f;for(uint b=0u;b<p.nullity;++b){float projected=0.0f;
  for(uint column=0u;column<p.nullity;++column){volatile float projectedProduct=
   coordinateProjector[b*p.nullity+column]*coordinateSlope[column];projected+=projectedProduct;}
  volatile float resultProduct=basis[component*p.nullity+b]*projected;
  result+=resultProduct;}return result;
}
kernel void fct_build_flux_pair(device const float* q [[buffer(0)]],
 device const float* ux [[buffer(1)]],device const float* uy [[buffer(2)]],
 device const float* uz [[buffer(3)]],device const float* ambient [[buffer(4)]],
 device const uchar* inflow [[buffer(5)]],device const float* basis [[buffer(6)]],
 device const float* coordinateProjector [[buffer(7)]],device float* low [[buffer(8)]],
 device float* delta [[buffer(9)]],constant FCTParams& p [[buffer(10)]],
 uint gid [[thread_position_in_grid]]){
 uint all=fct_all_faces(p),total=p.components*all;if(gid>=total)return;uint component=gid/all;
 uint packed=gid-component*all,axis,x,y,z;fct_decode_face(p,packed,axis,x,y,z);
 uint coordinate=fct_coordinate(axis,x,y,z),extent=fct_extent(p,axis);
 device const float* velocity=axis==0u?ux:(axis==1u?uy:uz);float u=velocity[packed-fct_face_offset(p,axis)];
 if((coordinate==0u||coordinate==extent)&&p.boundary[2u*axis+(coordinate==extent?1u:0u)]==2u){
  low[gid]=0.0f;delta[gid]=0.0f;return;}
 if((coordinate==0u||coordinate==extent)&&p.boundary[2u*axis+(coordinate==extent?1u:0u)]!=0u){
  int donorShift=u>=0.0f?-1:0;uint bx=x,by=y,bz=z;if(coordinate==extent)donorShift=u>=0.0f?-1:0;
  float donor=fct_stage_value(q,ambient,inflow,p,component,bx,by,bz,axis,donorShift);
  low[gid]=u*donor;delta[gid]=0.0f;return;}
 uint rightCoordinate=coordinate==extent?0u:coordinate;
 uint leftCoordinate=rightCoordinate==0u?extent-1u:rightCoordinate-1u;
 uint lx=x,ly=y,lz=z,rx=x,ry=y,rz=z;fct_set_coordinate(axis,leftCoordinate,lx,ly,lz);
 fct_set_coordinate(axis,rightCoordinate,rx,ry,rz);bool fromLeft=u>=0.0f;
 uint dx=fromLeft?lx:rx,dy=fromLeft?ly:ry,dz=fromLeft?lz:rz;
 float donor=q[component*p.cells+fct_cell(p,dx,dy,dz)],slope=component<8u?
  fct_mass_slope(q,ambient,inflow,basis,coordinateProjector,p,component,dx,dy,dz,axis):
  fct_mc(donor-fct_stage_value(q,ambient,inflow,p,component,dx,dy,dz,axis,-1),
   fct_stage_value(q,ambient,inflow,p,component,dx,dy,dz,axis,1)-donor);
 float high=donor+(fromLeft?0.5f:-0.5f)*slope;low[gid]=u*donor;delta[gid]=u*(high-donor);
}
kernel void fct_average_flux_pair(device const float* firstLow [[buffer(0)]],
 device const float* firstDelta [[buffer(1)]],device const float* secondLow [[buffer(2)]],
 device const float* secondDelta [[buffer(3)]],device float* averagedLow [[buffer(4)]],
 device float* averagedDelta [[buffer(5)]],constant uint& count [[buffer(6)]],
 uint gid [[thread_position_in_grid]]){
 if(gid>=count)return;averagedLow[gid]=0.5f*(firstLow[gid]+secondLow[gid]);
 averagedDelta[gid]=0.5f*(firstDelta[gid]+secondDelta[gid]);
}
kernel void fct_validate_flux_pair(device const float* low [[buffer(0)]],
 device const float* delta [[buffer(1)]],device atomic_uint* failure [[buffer(2)]],
 constant FCTParams& p [[buffer(3)]],uint gid [[thread_position_in_grid]]){
 uint all=fct_all_faces(p),total=p.components*all;if(gid>=total)return;
 float lowValue=low[gid],deltaValue=delta[gid];
 if(!isfinite(lowValue)||!isfinite(deltaValue)){
  atomic_fetch_or_explicit(failure,1024u,memory_order_relaxed);return;}
 uint component=gid/all,packed=gid-component*all,axis,x,y,z;
 fct_decode_face(p,packed,axis,x,y,z);uint coordinate=fct_coordinate(axis,x,y,z);
 if(p.boundary[2u*axis]==0u&&coordinate==fct_extent(p,axis)){
  fct_set_coordinate(axis,0u,x,y,z);uint lowPacked=fct_packed_face(p,axis,x,y,z);
  if(as_type<uint>(lowValue)!=as_type<uint>(low[component*all+lowPacked])||
   as_type<uint>(deltaValue)!=as_type<uint>(delta[component*all+lowPacked]))
   atomic_fetch_or_explicit(failure,2048u,memory_order_relaxed);
 }
}
inline float fct_inequality(thread const float* value,uint inequality,device const float* enthalpy){
 if(inequality==0u)return-value[0];if(inequality==1u){float result=value[0];
  for(uint species=0u;species<7u;++species)result-=value[1u+species];return result;}
 if(inequality<9u)return-value[inequality-1u];if(inequality==9u){float result=-value[8];
  for(uint species=0u;species<7u;++species)result+=enthalpy[species]*value[1u+species];return result;}
 float result=value[8];for(uint species=0u;species<7u;++species)
  result-=enthalpy[7u+species]*value[1u+species];return result;
}
inline uint fct_cell_face(constant FCTParams& p,uint cell,uint axis,bool upper){
 uint x=cell%p.nx,y=(cell/p.nx)%p.ny,z=cell/(p.nx*p.ny);
 if(upper)fct_set_coordinate(axis,fct_coordinate(axis,x,y,z)+1u,x,y,z);
 return fct_packed_face(p,axis,x,y,z);}
kernel void fct_build_ratios(device const float* beginning [[buffer(0)]],
 device const float* sourceDelta [[buffer(1)]],device const float* lowFlux [[buffer(2)]],
 device const float* fluxDelta [[buffer(3)]],device const float* enthalpyBounds [[buffer(4)]],
 device float* lowState [[buffer(5)]],device float* ratio [[buffer(6)]],
 device atomic_uint* failure [[buffer(7)]],constant FCTParams& p [[buffer(8)]],
 uint gid [[thread_position_in_grid]]){
 uint total=p.cells*p.inequalities;if(gid>=total)return;uint inequality=gid/p.cells,cell=gid-inequality*p.cells;
 uint all=fct_all_faces(p);float low[9],correction[6][9],scale=p.dt/p.dx;
 for(uint component=0u;component<9u;++component){low[component]=beginning[component*p.cells+cell]+
  sourceDelta[component*p.cells+cell];for(uint axis=0u;axis<3u;++axis){uint lower=fct_cell_face(p,cell,axis,false);
  uint upper=fct_cell_face(p,cell,axis,true);volatile float fluxDifference=
   lowFlux[component*all+lower]-lowFlux[component*all+upper];volatile float scaledFlux=
   scale*fluxDifference;low[component]+=scaledFlux;volatile float lowerCorrection=
   scale*fluxDelta[component*all+lower];volatile float upperCorrection=
   scale*fluxDelta[component*all+upper];correction[2u*axis][component]=lowerCorrection;
  correction[2u*axis+1u][component]=-upperCorrection;}
  if(!isfinite(low[component]))atomic_fetch_or_explicit(failure,1u,memory_order_relaxed);
  if(inequality==0u)lowState[component*p.cells+cell]=low[component];}
 float lowEnvelopeScale=1.0f;for(uint component=0u;component<9u;++component)
  lowEnvelopeScale+=abs(low[component]);
 float rowScale=0.0f;for(uint component=0u;component<9u;++component){float lower=low[component],upper=low[component];
  for(uint direction=0u;direction<6u;++direction){float d=correction[direction][component];
   if(d<0.0f)lower+=d;else upper+=d;}float minimum=lower<=0.0f&&upper>=0.0f?0.0f:min(abs(lower),abs(upper));
 if(inequality<9u){if(component<8u)rowScale+=minimum;}else if(component==8u)rowScale+=minimum;
  else if(component>0u&&component<8u)rowScale+=(abs(enthalpyBounds[component-1u])+
   abs(enthalpyBounds[7u+component-1u]))*minimum;}rowScale=max(1.0f,rowScale);
 float lowExcess=fct_inequality(low,inequality,enthalpyBounds);
 if(!isfinite(lowExcess)||lowExcess>p.feasibility*lowEnvelopeScale)
  atomic_fetch_or_explicit(failure,128u,memory_order_relaxed);
 float requested=0.0f;for(uint direction=0u;direction<6u;++direction)
  requested+=max(0.0f,fct_inequality(correction[direction],inequality,enthalpyBounds));
 float budget=max(0.0f,(p.feasibility-p.assemblyReserve)*rowScale-
  lowExcess);float value=requested>0.0f?min(1.0f,budget/requested):1.0f;
 if(!isfinite(value)||value<0.0f||value>1.0f)atomic_fetch_or_explicit(failure,2u,memory_order_relaxed);
 ratio[inequality*p.cells+cell]=clamp(value,0.0f,1.0f);
}
kernel void fct_build_face_alpha(device const float* fluxDelta [[buffer(0)]],
 device const float* ratio [[buffer(1)]],device const float* enthalpyBounds [[buffer(2)]],
 device float* alpha [[buffer(3)]],device atomic_uint* failure [[buffer(4)]],
 constant FCTParams& p [[buffer(5)]],
 uint gid [[thread_position_in_grid]]){
 uint all=fct_all_faces(p);if(gid>=all)return;uint axis,x,y,z;fct_decode_face(p,gid,axis,x,y,z);
 uint coordinate=fct_coordinate(axis,x,y,z),extent=fct_extent(p,axis);float accepted=1.0f,scale=p.dt/p.dx;
 bool periodic=p.boundary[2u*axis]==0u;uint leftCoordinate=coordinate?coordinate-1u:extent-1u;
 uint rightCoordinate=coordinate==extent?0u:coordinate;bool haveLeft=periodic||coordinate>0u;
 bool haveRight=periodic||coordinate<extent;uint lx=x,ly=y,lz=z,rx=x,ry=y,rz=z;
 fct_set_coordinate(axis,leftCoordinate,lx,ly,lz);fct_set_coordinate(axis,rightCoordinate,rx,ry,rz);
 uint left=haveLeft?fct_cell(p,lx,ly,lz):0u,right=haveRight?fct_cell(p,rx,ry,rz):0u;
 float correction[9];for(uint inequality=0u;inequality<p.inequalities;++inequality){
  for(uint component=0u;component<9u;++component)correction[component]=
   -scale*fluxDelta[component*all+gid];
  if(haveLeft&&fct_inequality(correction,inequality,enthalpyBounds)>0.0f)
   accepted=min(accepted,ratio[inequality*p.cells+left]);
  for(uint component=0u;component<9u;++component)correction[component]=-
   correction[component];
  if(haveRight&&fct_inequality(correction,inequality,enthalpyBounds)>0.0f)
   accepted=min(accepted,ratio[inequality*p.cells+right]);
 }
 if(!isfinite(accepted)||accepted<0.0f||accepted>1.0f)
  atomic_fetch_or_explicit(failure,4u,memory_order_relaxed);
 alpha[gid]=clamp(accepted,0.0f,1.0f);
}
kernel void fct_commit_scalar(device const float* lowState [[buffer(0)]],
 device const float* fluxDelta [[buffer(1)]],device const float* alpha [[buffer(2)]],
 device const float* enthalpyBounds [[buffer(3)]],device const float* affine [[buffer(4)]],
 device float* accepted [[buffer(5)]],device atomic_uint* failure [[buffer(6)]],
 constant FCTParams& p [[buffer(7)]],
 uint gid [[thread_position_in_grid]]){
 if(gid>=p.cells)return;uint all=fct_all_faces(p);float value[9],scale=p.dt/p.dx;
 for(uint component=0u;component<9u;++component){value[component]=lowState[component*p.cells+gid];
  for(uint axis=0u;axis<3u;++axis){uint lower=fct_cell_face(p,gid,axis,false);
   uint upper=fct_cell_face(p,gid,axis,true);volatile float lowerCorrection=
    alpha[lower]*fluxDelta[component*all+lower];volatile float upperCorrection=
    alpha[upper]*fluxDelta[component*all+upper];volatile float correction=
    lowerCorrection-upperCorrection;volatile float scaled=scale*correction;
    value[component]+=scaled;}
  accepted[component*p.cells+gid]=value[component];
  if(!isfinite(value[component]))atomic_fetch_or_explicit(failure,8u,memory_order_relaxed);}
 for(uint inequality=0u;inequality<p.inequalities;++inequality){float rowScale=1.0f;
  for(uint component=0u;component<9u;++component)rowScale+=abs(value[component]);
  float excess=fct_inequality(value,inequality,enthalpyBounds);
  if(!isfinite(excess)||excess>p.feasibility*rowScale)
   atomic_fetch_or_explicit(failure,16u,memory_order_relaxed);}
 for(uint row=0u;row<p.affineRows;++row){float residual=0.0f,rowScale=1.0f;
  for(uint component=0u;component<8u;++component){float coefficient=affine[row*8u+component];
   residual+=coefficient*value[component];rowScale+=abs(coefficient)*abs(value[component]);}
  if(!isfinite(residual)||abs(residual)>p.feasibility*rowScale)
   atomic_fetch_or_explicit(failure,256u,memory_order_relaxed);}
}
inline float fct_accepted_gas(device const float* low,device const float* delta,
 device const float* alpha,constant FCTParams& p,uint axis,uint x,uint y,uint z){
 uint packed=fct_packed_face(p,axis,x,y,z),all=fct_all_faces(p),coordinate=fct_coordinate(axis,x,y,z);
 uint extent=fct_extent(p,axis);if(p.boundary[2u*axis]==0u&&coordinate==extent){
  fct_set_coordinate(axis,0u,x,y,z);packed=fct_packed_face(p,axis,x,y,z);}
 float gas=0.0f;for(uint component=1u;component<=6u;++component)
  gas+=low[component*all+packed]+alpha[packed]*delta[component*all+packed];
 return gas;
}
inline float fct_velocity(device const float* ux,device const float* uy,device const float* uz,
 constant FCTParams& p,uint component,uint x,uint y,uint z){device const float* values=
 component==0u?ux:(component==1u?uy:uz);return values[fct_face(p,component,x,y,z)];}
inline bool fct_prescribed(constant FCTParams& p,uint side){return p.boundary[side]==2u;}
kernel void fct_compatible_stage_rate(device const float* low [[buffer(0)]],
 device const float* delta [[buffer(1)]],device const float* alpha [[buffer(2)]],
 device const float* ux [[buffer(3)]],device const float* uy [[buffer(4)]],
 device const float* uz [[buffer(5)]],device float* rate [[buffer(6)]],
 device atomic_uint* failure [[buffer(7)]],constant FCTParams& p [[buffer(8)]],
 uint gid [[thread_position_in_grid]]){
 uint all=fct_all_faces(p);if(gid>=all)return;uint component,x,y,z;
 fct_decode_face(p,gid,component,x,y,z);uint normal=fct_coordinate(component,x,y,z);
 uint componentExtent=fct_extent(p,component),normalCount=componentExtent+1u;
 bool anyPeriodic=false,allPeriodic=true;for(uint axis=0u;axis<3u;++axis){
  bool axisPeriodic=p.boundary[2u*axis]==0u&&p.boundary[2u*axis+1u]==0u;
  anyPeriodic=anyPeriodic||axisPeriodic;allPeriodic=allPeriodic&&axisPeriodic;}
 if(anyPeriodic&&!allPeriodic){atomic_fetch_or_explicit(failure,64u,memory_order_relaxed);
  rate[gid]=0.0f;return;}
 if(allPeriodic){
  if(normal==componentExtent){fct_set_coordinate(component,0u,x,y,z);normal=0u;}
  float divergence=0.0f;for(uint derivative=0u;derivative<3u;++derivative){
   uint componentCoordinate=fct_coordinate(component,x,y,z);
   uint derivativeCoordinate=fct_coordinate(derivative,x,y,z);
   uint nextComponent=componentCoordinate+1u==componentExtent?0u:componentCoordinate+1u;
   uint previousDerivative=derivativeCoordinate?derivativeCoordinate-1u:
    fct_extent(p,derivative)-1u;
   uint nextDerivative=derivativeCoordinate+1u==fct_extent(p,derivative)?0u:
    derivativeCoordinate+1u;
   uint ncx=x,ncy=y,ncz=z,pdx=x,pdy=y,pdz=z,pcx=x,pcy=y,pcz=z,
    ndx=x,ndy=y,ndz=z;
   fct_set_coordinate(component,nextComponent,ncx,ncy,ncz);
   fct_set_coordinate(derivative,previousDerivative,pdx,pdy,pdz);
   uint previousComponent=componentCoordinate?componentCoordinate-1u:componentExtent-1u;
   fct_set_coordinate(component,previousComponent,pcx,pcy,pcz);
   fct_set_coordinate(derivative,nextDerivative,ndx,ndy,ndz);
   float upper=0.0f,lower=0.0f;if(derivative==component){
    uint pcux=pcx,pcuy=pcy,pcuz=pcz,cux=x,cuy=y,cuz=z;
    fct_set_coordinate(derivative,nextDerivative,pcux,pcuy,pcuz);
    fct_set_coordinate(derivative,nextDerivative,cux,cuy,cuz);
    upper=0.25f*(fct_accepted_gas(low,delta,alpha,p,derivative,pcux,pcuy,pcuz)+
     fct_accepted_gas(low,delta,alpha,p,derivative,cux,cuy,cuz))*(
     fct_velocity(ux,uy,uz,p,component,x,y,z)+
     fct_velocity(ux,uy,uz,p,component,ncx,ncy,ncz));
    lower=0.25f*(fct_accepted_gas(low,delta,alpha,p,derivative,pdx,pdy,pdz)+
     fct_accepted_gas(low,delta,alpha,p,derivative,x,y,z))*(
     fct_velocity(ux,uy,uz,p,component,pdx,pdy,pdz)+
     fct_velocity(ux,uy,uz,p,component,x,y,z));
   }else{
    upper=0.25f*(fct_accepted_gas(low,delta,alpha,p,derivative,x,y,z)+
     fct_accepted_gas(low,delta,alpha,p,derivative,ncx,ncy,ncz))*(
     fct_velocity(ux,uy,uz,p,component,x,y,z)+
     fct_velocity(ux,uy,uz,p,component,ndx,ndy,ndz));
    lower=0.25f*(fct_accepted_gas(low,delta,alpha,p,derivative,pcx,pcy,pcz)+
     fct_accepted_gas(low,delta,alpha,p,derivative,x,y,z))*(
     fct_velocity(ux,uy,uz,p,component,pdx,pdy,pdz)+
     fct_velocity(ux,uy,uz,p,component,x,y,z));
   }divergence+=(upper-lower)/p.dx;
  }
  if(!isfinite(divergence))atomic_fetch_or_explicit(failure,32u,memory_order_relaxed);
  rate[gid]=divergence;return;
 }
 float divergence=0.0f;for(uint derivative=0u;derivative<3u;++derivative){
  if(derivative==component){uint previous=normal?normal-1u:normal;
   uint next=normal+1u<normalCount?normal+1u:normal;uint px=x,py=y,pz=z,nx=x,ny=y,nz=z;
   fct_set_coordinate(component,previous,px,py,pz);fct_set_coordinate(component,next,nx,ny,nz);
   float upper=0.25f*(fct_accepted_gas(low,delta,alpha,p,component,x,y,z)+
    fct_accepted_gas(low,delta,alpha,p,component,nx,ny,nz))*(
    fct_velocity(ux,uy,uz,p,component,x,y,z)+fct_velocity(ux,uy,uz,p,component,nx,ny,nz));
   float lower=0.25f*(fct_accepted_gas(low,delta,alpha,p,component,px,py,pz)+
    fct_accepted_gas(low,delta,alpha,p,component,x,y,z))*(
    fct_velocity(ux,uy,uz,p,component,px,py,pz)+fct_velocity(ux,uy,uz,p,component,x,y,z));
   float normalScale=normal==0u||normal+1u==normalCount?2.0f:1.0f;
   divergence+=normalScale*(upper-lower)/p.dx;
  }else{uint derivativeExtent=fct_extent(p,derivative),position=fct_coordinate(derivative,x,y,z);
   uint componentLower=normal?normal-1u:0u,componentUpper=normal<componentExtent?normal:componentExtent-1u;
   uint llx=x,lly=y,llz=z,lux=x,luy=y,luz=z,ulx=x,uly=y,ulz=z,uux=x,uuy=y,uuz=z;
   fct_set_coordinate(component,componentLower,llx,lly,llz);
   fct_set_coordinate(component,componentUpper,lux,luy,luz);
   fct_set_coordinate(component,componentLower,ulx,uly,ulz);
   fct_set_coordinate(component,componentUpper,uux,uuy,uuz);
   fct_set_coordinate(derivative,position,llx,lly,llz);
   fct_set_coordinate(derivative,position,lux,luy,luz);
   fct_set_coordinate(derivative,position+1u,ulx,uly,ulz);
   fct_set_coordinate(derivative,position+1u,uux,uuy,uuz);
   uint previous=position?position-1u:position,next=min(position+1u,derivativeExtent-1u);
   if(p.boundary[2u*derivative]==0u){previous=position?position-1u:derivativeExtent-1u;
    next=position+1u==derivativeExtent?0u:position+1u;}
   uint px=x,py=y,pz=z,nx=x,ny=y,nz=z;fct_set_coordinate(derivative,previous,px,py,pz);
   fct_set_coordinate(derivative,next,nx,ny,nz);
   float lowerMass=fct_accepted_gas(low,delta,alpha,p,derivative,llx,lly,llz)+
    fct_accepted_gas(low,delta,alpha,p,derivative,lux,luy,luz);
   float upperMass=fct_accepted_gas(low,delta,alpha,p,derivative,ulx,uly,ulz)+
    fct_accepted_gas(low,delta,alpha,p,derivative,uux,uuy,uuz);
   float lower=0.25f*lowerMass*(fct_velocity(ux,uy,uz,p,component,px,py,pz)+
    fct_velocity(ux,uy,uz,p,component,x,y,z));
   float upper=0.25f*upperMass*(fct_velocity(ux,uy,uz,p,component,x,y,z)+
    fct_velocity(ux,uy,uz,p,component,nx,ny,nz));
   if(position==0u&&fct_prescribed(p,2u*derivative))lower=0.0f;
   if(position+1u==derivativeExtent&&fct_prescribed(p,2u*derivative+1u))upper=0.0f;
   divergence+=(upper-lower)/p.dx;}}
 bool boundaryFace=normal==0u||normal==componentExtent;if(boundaryFace){uint side=2u*component+
  (normal==componentExtent?1u:0u);if(fct_prescribed(p,side))divergence=0.0f;
  else divergence*=0.5f;}
 if(!isfinite(divergence))atomic_fetch_or_explicit(failure,32u,memory_order_relaxed);
 rate[gid]=divergence;
}
kernel void fct_apply_momentum_rate(device float* momentum [[buffer(0)]],
 device const float* rate [[buffer(1)]],device atomic_uint* failure [[buffer(2)]],
 constant FCTParams& p [[buffer(3)]],uint gid [[thread_position_in_grid]]){
 if(gid>=fct_all_faces(p))return;float updated=momentum[gid]-p.dt*rate[gid];
 if(!isfinite(updated))atomic_fetch_or_explicit(failure,512u,memory_order_relaxed);
 momentum[gid]=updated;
}
kernel void fct_extract_gas_density(device const float* accepted [[buffer(0)]],
 device float* density [[buffer(1)]],constant FCTParams& p [[buffer(2)]],
 uint gid [[thread_position_in_grid]]){
 if(gid>=p.cells)return;float gas=accepted[p.cells+gid];
 for(uint component=2u;component<=6u;++component)gas+=accepted[component*p.cells+gid];
 density[gid]=gas;
}
kernel void fct_fill_one(device float* values [[buffer(0)]],
 constant uint& count [[buffer(1)]],uint gid [[thread_position_in_grid]]){
 if(gid<count)values[gid]=1.0f;
}
inline float fct_cell_gas(device const float* q,constant FCTParams& p,uint cell){
 float gas=q[p.cells+cell];for(uint component=2u;component<=6u;++component)
  gas+=q[component*p.cells+cell];return gas;
}
inline float fct_restricted_cell_gas(device const float* q,device const float* ambient,
 constant FCTParams& p,uint component,uint x,uint y,uint z){
 uint normal=fct_coordinate(component,x,y,z),extent=fct_extent(p,component);
 bool boundaryFace=normal==0u||normal==extent;
 uint lowX=x,lowY=y,lowZ=z,highX=x,highY=y,highZ=z;
 if(!boundaryFace){fct_set_coordinate(component,normal-1u,lowX,lowY,lowZ);
  return 0.5f*(fct_cell_gas(q,p,fct_cell(p,lowX,lowY,lowZ))+
   fct_cell_gas(q,p,fct_cell(p,highX,highY,highZ)));}
 uint side=2u*component+(normal==extent?1u:0u);
 if(p.boundary[side]==0u){fct_set_coordinate(component,extent-1u,lowX,lowY,lowZ);
  fct_set_coordinate(component,0u,highX,highY,highZ);
  return 0.5f*(fct_cell_gas(q,p,fct_cell(p,lowX,lowY,lowZ))+
   fct_cell_gas(q,p,fct_cell(p,highX,highY,highZ)));}
 fct_set_coordinate(component,normal==extent?extent-1u:0u,highX,highY,highZ);
 float ambientGas=ambient[1];for(uint species=2u;species<=6u;++species)
  ambientGas+=ambient[species];
 return 0.5f*(fct_cell_gas(q,p,fct_cell(p,highX,highY,highZ))+ambientGas);
}
kernel void fct_commuting_identity(device const float* beginning [[buffer(0)]],
 device const float* sourceDelta [[buffer(1)]],device const float* accepted [[buffer(2)]],
 device const float* ambient [[buffer(3)]],device const float* unitVelocityRate [[buffer(4)]],
 device atomic_uint* maximumResidualBits [[buffer(5)]],device atomic_uint* maximumScaleBits [[buffer(6)]],
 device atomic_uint* failure [[buffer(7)]],constant FCTParams& p [[buffer(8)]],
 uint gid [[thread_position_in_grid]]){
 uint all=fct_all_faces(p);if(gid>=all)return;uint component,x,y,z;
 fct_decode_face(p,gid,component,x,y,z);uint normal=fct_coordinate(component,x,y,z),
  extent=fct_extent(p,component);bool boundaryFace=normal==0u||normal==extent;
 uint side=2u*component+(normal==extent?1u:0u);
 if(boundaryFace&&p.boundary[side]==2u){
  if(as_type<uint>(unitVelocityRate[gid])!=0u)
   atomic_fetch_or_explicit(failure,4096u,memory_order_relaxed);return;}
 // Form the integrated-source restriction directly so the ambient boundary
 // state is not spuriously added to a delta field.
 uint lowX=x,lowY=y,lowZ=z,highX=x,highY=y,highZ=z;
 float sourceRestricted=0.0f;
 if(!boundaryFace){fct_set_coordinate(component,normal-1u,lowX,lowY,lowZ);
  sourceRestricted=0.5f*(fct_cell_gas(sourceDelta,p,fct_cell(p,lowX,lowY,lowZ))+
   fct_cell_gas(sourceDelta,p,fct_cell(p,highX,highY,highZ)));}
 else if(p.boundary[side]==0u){fct_set_coordinate(component,extent-1u,lowX,lowY,lowZ);
  fct_set_coordinate(component,0u,highX,highY,highZ);
  sourceRestricted=0.5f*(fct_cell_gas(sourceDelta,p,fct_cell(p,lowX,lowY,lowZ))+
   fct_cell_gas(sourceDelta,p,fct_cell(p,highX,highY,highZ)));}
 else {fct_set_coordinate(component,normal==extent?extent-1u:0u,highX,highY,highZ);
  sourceRestricted=0.5f*fct_cell_gas(sourceDelta,p,fct_cell(p,highX,highY,highZ));}
 float base=fct_restricted_cell_gas(beginning,ambient,p,component,x,y,z)+sourceRestricted;
 float acceptedRestricted=fct_restricted_cell_gas(accepted,ambient,p,component,x,y,z);
 float advanced=base-p.dt*unitVelocityRate[gid];
 float residual=abs(acceptedRestricted-advanced),scale=max(abs(base),abs(acceptedRestricted));
 if(!isfinite(residual)||!isfinite(scale)){
  atomic_fetch_or_explicit(failure,8192u,memory_order_relaxed);return;}
 atomic_fetch_max_explicit(maximumResidualBits,as_type<uint>(residual),memory_order_relaxed);
 atomic_fetch_max_explicit(maximumScaleBits,as_type<uint>(scale),memory_order_relaxed);
}
)METAL";
		}

			bool Valid() const
			{
				return device&&queue&&reconstruct&&scan&&flux&&update&&extractGasMassDose&&
					compatibleDualUpdate&&gatherValues&&
					scatterValues&&gatherVelocity&&gatherPeriodicDualValues&&
					gatherPeriodicDualCarrier&&scatterPeriodicDualValues&&
					publishPeriodicDualSeam&&gatherDualLineValues&&scatterDualLineValues&&
				prescribeDualComponentWalls&&addCellSources&&extractGasDensity&&addFaceSources&&
				measureMethaneManifold&&selectMethaneManifoldHighBins&&
				histogramMethaneManifoldLowBins&&selectMethaneManifoldLowBins&&
				foldMethaneAdvectiveAnomalyTarget&&
					error.empty();
			}
		};

		struct SingleStageFCTMetalContext
		{
			id<MTLDevice> device;
			id<MTLCommandQueue> queue;
			id<MTLComputePipelineState> buildFluxPair;
			id<MTLComputePipelineState> averageFluxPair;
			id<MTLComputePipelineState> validateFluxPair;
			id<MTLComputePipelineState> buildRatios;
			id<MTLComputePipelineState> buildFaceAlpha;
			id<MTLComputePipelineState> commitScalar;
			id<MTLComputePipelineState> compatibleStageRate;
			id<MTLComputePipelineState> applyMomentumRate;
			id<MTLComputePipelineState> extractGasDensity;
			id<MTLComputePipelineState> fillOne;
			id<MTLComputePipelineState> commutingIdentity;
			std::string error;

			SingleStageFCTMetalContext() : device(nil),queue(nil),buildFluxPair(nil),
				averageFluxPair(nil),validateFluxPair(nil),buildRatios(nil),
				buildFaceAlpha(nil),commitScalar(nil),compatibleStageRate(nil),applyMomentumRate(nil),
				extractGasDensity(nil),fillOne(nil),commutingIdentity(nil)
			{
				@autoreleasepool {
					device=MTLCreateSystemDefaultDevice();
					if(!device){error="production single-stage FCT diagnostic has no Metal device";return;}
					MTLCompileOptions* options=[[MTLCompileOptions alloc] init];
					if(@available(macOS 15.0,*))options.mathMode=MTLMathModeSafe;
					else{error="production single-stage FCT diagnostic requires Metal safe math mode";return;}
					NSError* metalError=nil;
					NSString* source=[NSString stringWithUTF8String:
						MetalRemapContext::SingleStageFCTSource()];
					id<MTLLibrary> library=[device newLibraryWithSource:source options:options error:&metalError];
					if(!library){error=MetalError(
						"production single-stage FCT diagnostic library compilation failed",
						metalError);return;}
					auto makePipeline=[&](const char* name)->id<MTLComputePipelineState>{
						id<MTLFunction> function=[library newFunctionWithName:
							[NSString stringWithUTF8String:name]];
						return function?NameProducerKernel([device newComputePipelineStateWithFunction:function
							error:&metalError],name):nil;};
					buildFluxPair=makePipeline("fct_build_flux_pair");
					averageFluxPair=makePipeline("fct_average_flux_pair");
					validateFluxPair=makePipeline("fct_validate_flux_pair");
					buildRatios=makePipeline("fct_build_ratios");
					buildFaceAlpha=makePipeline("fct_build_face_alpha");
					commitScalar=makePipeline("fct_commit_scalar");
					compatibleStageRate=makePipeline("fct_compatible_stage_rate");
					applyMomentumRate=makePipeline("fct_apply_momentum_rate");
				extractGasDensity=makePipeline("fct_extract_gas_density");
				fillOne=makePipeline("fct_fill_one");
				commutingIdentity=makePipeline("fct_commuting_identity");
					if(!buildFluxPair||!averageFluxPair||!validateFluxPair||!buildRatios||!buildFaceAlpha||!commitScalar||
						!compatibleStageRate||!applyMomentumRate||!extractGasDensity||!fillOne||
						!commutingIdentity){error=MetalError(
						"production single-stage FCT diagnostic pipeline creation failed",
						metalError);return;}
					queue=[device newCommandQueue];
					if(!queue)error=
						"production single-stage FCT diagnostic command queue allocation failed";
				}
			}

			bool Valid() const
			{
				return device&&queue&&buildFluxPair&&averageFluxPair&&validateFluxPair&&buildRatios&&buildFaceAlpha&&commitScalar&&
					compatibleStageRate&&applyMomentumRate&&extractGasDensity&&fillOne&&
					commutingIdentity&&error.empty();
			}
		};

		struct ResidentTransportMetalContext
		{
			id<MTLDevice> device;
			id<MTLCommandQueue> queue;
			id<MTLComputePipelineState> evaluate;
			id<MTLComputePipelineState> identify;
			id<MTLComputePipelineState> physicalFlux;
			id<MTLComputePipelineState> advectivePair;
			id<MTLComputePipelineState> finalizeAdvective;
			id<MTLComputePipelineState> composePair;
			id<MTLComputePipelineState> validatePhysical;
			id<MTLComputePipelineState> identifyPhysical;
			id<MTLComputePipelineState> identifyEOSCandidate;
			id<MTLComputePipelineState> evaluateEOSCandidate;
			id<MTLComputePipelineState> finalizeEOSCandidate;
			id<MTLComputePipelineState> identifyEOS;
			id<MTLComputePipelineState> produceFrozenSource;
			id<MTLComputePipelineState> identifyFrozenSource;
			id<MTLComputePipelineState> evaluateTargetTerms;
			id<MTLComputePipelineState> finalizeTarget;
			id<MTLComputePipelineState> identifyTarget;
			id<MTLComputePipelineState> identifyProjectionConsumer;
			id<MTLComputePipelineState> consumeTarget;
			id<MTLComputePipelineState> diagnoseEOSLog;
			id<MTLComputePipelineState> ownerIssueBootstrap,ownerPackFaces,ownerAverageField,
				ownerGasSource,ownerPredictMomentum,
				ownerHeunMomentum,ownerBindTransport,ownerAverageFlux,ownerBindAveragedFlux,
				ownerBindCandidate,ownerComposeTarget,ownerIdentifyTarget,ownerNextOpenClass,
				ownerIdentifyEndpointClass,ownerIdentifyProducedEndpointClass,
				ownerMinimumField,ownerHalfField,ownerResidual,
				ownerClassResidual,ownerIntegratedOpenHead,
				ownerIssueStageSeal,ownerIssuePublication;
			FireProductionEOSLogMetalQualificationIdentity eosLogIdentity;
			std::string error;
			bool productionStageTokens=false;
			std::string compiledSource;

			static const char* Source()
			{
				static const std::string source=std::string(R"METAL(
#include <metal_stdlib>
using namespace metal;
#ifndef RISE_STAGE_TOKENS
#define RISE_STAGE_TOKENS 0
#endif
constant bool resident_full_payload_seals=RISE_STAGE_TOKENS==0;
inline ulong resident_stage_token(uint domain,ulong4 a,ulong4 b){
 ulong hash=0x723230345f746f6bul;hash^=ulong(domain);hash*=1099511628211ul;
 for(uint i=0u;i<4u;++i){hash^=a[i];hash*=1099511628211ul;}
 for(uint i=0u;i<4u;++i){hash^=b[i];hash*=1099511628211ul;}
 return hash==0ul?1ul:hash;
}
inline uint resident_record_obligation(device atomic_uint* location,uint mask,memory_order){
 return resident_full_payload_seals?atomic_fetch_or_explicit(location,mask,memory_order_relaxed):0u;
}
struct TransportParams {uint nx;uint ny;uint nz;uint cells;uint boundary[6];uint sideOffset[6];uint faceOffset[3];
 uint stage;float dx;float Pr;float Sc;float Cv;ulong attempt;ulong parent;ulong projection;
 ulong thermochemistry;ulong transport;};
constant uint transport_max_knots=128u;
constant uint transport_stride=1u+5u*transport_max_knots;
inline uint tr_cell(constant TransportParams& p,uint x,uint y,uint z){return (z*p.ny+y)*p.nx+x;}
inline uint tr_extent(constant TransportParams& p,uint a){return a==0u?p.nx:(a==1u?p.ny:p.nz);}
inline uint tr_face(constant TransportParams& p,uint a,uint x,uint y,uint z){return p.faceOffset[a]+
 (a==0u?(z*p.ny+y)*(p.nx+1u)+x:(a==1u?(z*(p.ny+1u)+y)*p.nx+x:(z*p.ny+y)*p.nx+x));}
inline void tr_set(uint a,uint v,thread uint& x,thread uint& y,thread uint& z){if(a==0u)x=v;else if(a==1u)y=v;else z=v;}
inline float tr_cell_velocity(device const float* velocity,constant TransportParams& p,uint a,uint x,uint y,uint z){
 uint upperX=x+(a==0u),upperY=y+(a==1u),upperZ=z+(a==2u);
 return 0.5f*(velocity[tr_face(p,a,x,y,z)]+velocity[tr_face(p,a,upperX,upperY,upperZ)]);}
inline float tr_boundary_velocity(device const float* velocity,constant TransportParams& p,
 uint side,uint component,uint x,uint y,uint z){uint normal=side/2u;if(component!=normal)return 0.0f;
 if(normal==0u)x=(side&1u)?p.nx:0u;else if(normal==1u)y=(side&1u)?p.ny:0u;
 else z=(side&1u)?p.nz:0u;return velocity[tr_face(p,component,x,y,z)];}
inline bool tr_inlet(device const uchar* inflow,constant TransportParams& p,uint side,
 uint x,uint y,uint z){uint first=side<2u?y:x,second=side<4u?z:y;
 uint width=side<2u?p.ny:p.nx;return inflow[p.sideOffset[side]+second*width+first]!=0u;}
inline float tr_cp(device const float* thermo,uint species,float temperature,thread bool& valid,
 device atomic_uint* obligations){
 device const float* record=thermo+32u*species;uint segments=uint(record[1]);device const float* selected=record+2u;
 bool found=false;
 for(uint segment=0u;segment<segments;++segment){device const float* candidate=record+2u+10u*segment;
  if(temperature>=candidate[0]&&(temperature<candidate[1]||(segment+1u==segments&&temperature==candidate[1]))){
   selected=candidate;found=true;atomic_fetch_or_explicit(obligations,1u<<min(segment,2u),memory_order_relaxed);}}
 if(!found){valid=false;return 0.0f;}float inverse=1.0f/temperature,t2=temperature*temperature;
 float cp=selected[2]*inverse*inverse+selected[3]*inverse+selected[4]+selected[5]*temperature+
  selected[6]*t2+selected[7]*t2*temperature+selected[8]*t2*t2;
 cp*=8314.46261815324f/record[0];if(!(cp>0.0f)||!isfinite(cp))valid=false;return cp;}
inline float tr_curve(device const float* transport,uint species,uint field,float temperature,
 thread bool& valid,device atomic_uint* obligations){device const float* record=transport+species*transport_stride;
 uint count=uint(record[0]);if(count<2u||count>transport_max_knots){valid=false;return 0.0f;}uint knotOffset=1u;
 uint valueOffset=1u+transport_max_knots+field*2u*transport_max_knots;
 uint slopeOffset=valueOffset+transport_max_knots;
 if(temperature<record[knotOffset]||temperature>record[knotOffset+count-1u]){valid=false;return 0.0f;}
 if(temperature<=record[knotOffset]){atomic_fetch_or_explicit(obligations,1u<<3u,memory_order_relaxed);return record[valueOffset];}
 if(temperature>=record[knotOffset+count-1u]){atomic_fetch_or_explicit(obligations,1u<<4u,memory_order_relaxed);return record[valueOffset+count-1u];}
 atomic_fetch_or_explicit(obligations,1u<<5u,memory_order_relaxed);
 uint lower=0u,upper=count-1u;while(upper-lower>1u){uint middle=(lower+upper)/2u;
  if(temperature<record[knotOffset+middle]){upper=middle;
   atomic_fetch_or_explicit(obligations,1u<<16u,memory_order_relaxed);}else{lower=middle;
   atomic_fetch_or_explicit(obligations,1u<<17u,memory_order_relaxed);}}
 if(temperature==record[knotOffset+lower]||temperature==record[knotOffset+upper])
  atomic_fetch_or_explicit(obligations,1u<<6u,memory_order_relaxed);
 float h=record[knotOffset+upper]-record[knotOffset+lower];float t=(temperature-record[knotOffset+lower])/h;
 float t2=t*t,t3=t2*t;return (2.0f*t3-3.0f*t2+1.0f)*record[valueOffset+lower]+
  (t3-2.0f*t2+t)*h*record[slopeOffset+lower]+(-2.0f*t3+3.0f*t2)*record[valueOffset+upper]+
  (t3-t2)*h*record[slopeOffset+upper];}
inline float tr_phi(float mui,float muj,float wi,float wj){float numerator=1.0f+
 sqrt(mui/muj)*sqrt(sqrt(wj/wi));return numerator*numerator/sqrt(8.0f*(1.0f+wi/wj));}
kernel void evaluate_resident_transport(device const float* state [[buffer(0)]],
 device const float* temperature [[buffer(1)]],device const float* velocity [[buffer(2)]],
 device const float* thermo [[buffer(3)]],device const float* transport [[buffer(4)]],
 device float* output [[buffer(5)]],device atomic_uint* failure [[buffer(6)]],
 device atomic_uint* obligations [[buffer(7)]],constant TransportParams& p [[buffer(8)]],
 device const uchar* inflow [[buffer(9)]],
 uint gid [[thread_position_in_grid]]){if(gid>=p.cells)return;uint x=gid%p.nx,y=(gid/p.nx)%p.ny,z=gid/(p.nx*p.ny);
 float T=temperature[gid];bool valid=isfinite(T);float rho=0.0f,mass[6];
 for(uint species=0u;species<6u;++species){float q=state[(species+1u)*p.cells+gid];
  if(!isfinite(q))valid=false;mass[species]=max(0.0f,q);rho+=mass[species];}
 if(!(rho>0.0f)||!isfinite(rho))valid=false;float gradient[3][3];
 for(uint derivative=0u;derivative<3u;++derivative){uint coordinate=derivative==0u?x:(derivative==1u?y:z);
  uint extent=tr_extent(p,derivative);for(uint component=0u;component<3u;++component){
   uint px=x,py=y,pz=z,nx=x,ny=y,nz=z;uint previous=coordinate?coordinate-1u:0u;
   uint next=coordinate+1u<extent?coordinate+1u:extent-1u;
   if(coordinate>0u&&coordinate+1u<extent)atomic_fetch_or_explicit(obligations,1u<<7u,memory_order_relaxed);
   if(coordinate==0u&&p.boundary[2u*derivative]==0u){previous=extent-1u;
    atomic_fetch_or_explicit(obligations,1u<<8u,memory_order_relaxed);}
   if(coordinate+1u==extent&&p.boundary[2u*derivative+1u]==0u){next=0u;
    atomic_fetch_or_explicit(obligations,1u<<8u,memory_order_relaxed);}
   tr_set(derivative,previous,px,py,pz);tr_set(derivative,next,nx,ny,nz);
   float pv=tr_cell_velocity(velocity,p,component,px,py,pz),nv=tr_cell_velocity(velocity,p,component,nx,ny,nz);
   if(coordinate==0u&&p.boundary[2u*derivative]!=0u){uint side=2u*derivative;
    bool inlet=tr_inlet(inflow,p,side,x,y,z);uint kind=inlet?2u:p.boundary[side];
    atomic_fetch_or_explicit(obligations,1u<<(kind==1u?9u:10u),memory_order_relaxed);
    if(inlet)atomic_fetch_or_explicit(obligations,1u<<18u,memory_order_relaxed);
    pv=kind==1u?tr_cell_velocity(velocity,p,component,x,y,z):
     2.0f*tr_boundary_velocity(velocity,p,side,component,x,y,z)-tr_cell_velocity(velocity,p,component,x,y,z);}
   if(coordinate+1u==extent&&p.boundary[2u*derivative+1u]!=0u){uint side=2u*derivative+1u;
    bool inlet=tr_inlet(inflow,p,side,x,y,z);uint kind=inlet?2u:p.boundary[side];
    atomic_fetch_or_explicit(obligations,1u<<(kind==1u?9u:10u),memory_order_relaxed);
    if(inlet)atomic_fetch_or_explicit(obligations,1u<<18u,memory_order_relaxed);
    nv=kind==1u?tr_cell_velocity(velocity,p,component,x,y,z):
     2.0f*tr_boundary_velocity(velocity,p,side,component,x,y,z)-tr_cell_velocity(velocity,p,component,x,y,z);}
   gradient[derivative][component]=(nv-pv)/(2.0f*p.dx);if(!isfinite(gradient[derivative][component]))valid=false;}}
 float alpha2=0.0f,beta[3][3];for(uint i=0u;i<3u;++i)for(uint j=0u;j<3u;++j){
  alpha2+=gradient[i][j]*gradient[i][j];beta[i][j]=0.0f;}float width2=p.dx*p.dx;
 for(uint m=0u;m<3u;++m)for(uint i=0u;i<3u;++i)for(uint j=0u;j<3u;++j)
  beta[i][j]+=width2*gradient[m][i]*gradient[m][j];
 float b0=beta[0][0]*beta[1][1],b1=beta[0][1]*beta[0][1],b2=beta[0][0]*beta[2][2];
 float b3=beta[0][2]*beta[0][2],b4=beta[1][1]*beta[2][2],b5=beta[1][2]*beta[1][2];
 float rawB=b0-b1+b2-b3+b4-b5,rawScale=abs(b0)+abs(b1)+abs(b2)+abs(b3)+abs(b4)+abs(b5);
 if(abs(rawB)<=32.0f*0x1p-24f*max(1.0f,rawScale))atomic_fetch_or_explicit(obligations,1u<<11u,memory_order_relaxed);
 atomic_fetch_or_explicit(obligations,1u<<(alpha2==0.0f?12u:13u),memory_order_relaxed);
 atomic_fetch_or_explicit(obligations,1u<<(rawB<0.0f?14u:15u),memory_order_relaxed);
 float eddy=alpha2==0.0f?0.0f:p.Cv*sqrt(max(0.0f,rawB)/alpha2);if(!(eddy>=0.0f)||!isfinite(eddy))valid=false;
 float mole[6],mu[6],conductivity[6],moleTotal=0.0f,cp=0.0f;
 for(uint species=0u;species<6u;++species){float fraction=mass[species]/rho;
  float weight=thermo[32u*species];mole[species]=fraction/weight;moleTotal+=mole[species];
  cp+=fraction*tr_cp(thermo,species,T,valid,obligations);mu[species]=tr_curve(transport,species,0u,T,valid,obligations);
  conductivity[species]=tr_curve(transport,species,1u,T,valid,obligations);}
 if(!(moleTotal>0.0f)||!isfinite(moleTotal))valid=false;for(uint species=0u;species<6u;++species)mole[species]/=moleTotal;
 float mixtureMu=0.0f,mixtureK=0.0f;for(uint i=0u;i<6u;++i){float denominator=0.0f;
  for(uint j=0u;j<6u;++j)denominator+=mole[j]*tr_phi(mu[i],mu[j],thermo[32u*i],thermo[32u*j]);
  mixtureMu+=mole[i]*mu[i]/denominator;mixtureK+=mole[i]*conductivity[i]/denominator;}
 float volumetric=rho*cp;float totalDiff=mixtureK/volumetric+eddy/p.Sc;
 float effectiveK=mixtureK+volumetric*eddy/p.Pr;float molecularNu=mixtureMu/rho;
 if(!(totalDiff>0.0f)||!(effectiveK>0.0f)||!(molecularNu>0.0f)||!isfinite(totalDiff)||!isfinite(effectiveK)||!isfinite(molecularNu))valid=false;
 output[gid]=totalDiff;output[p.cells+gid]=effectiveK;output[2u*p.cells+gid]=molecularNu;
 if(!valid)atomic_fetch_or_explicit(failure,1u,memory_order_relaxed);}
kernel void identify_resident_transport(device const float* state [[buffer(0)]],
 device const float* temperature [[buffer(1)]],device const float* velocity [[buffer(2)]],
 device const float* output [[buffer(3)]],device const float* thermo [[buffer(4)]],
 device const float* transport [[buffer(5)]],device const uchar* inflow [[buffer(6)]],
 device ulong* identity [[buffer(7)]],device atomic_uint* failure [[buffer(8)]],
 constant TransportParams& p [[buffer(9)]],
 uint gid [[thread_position_in_grid]]){
 if(gid!=0u)return;if(atomic_load_explicit(failure,memory_order_relaxed)!=0u){identity[0]=0ul;return;}
 if(!resident_full_payload_seals){identity[0]=resident_stage_token(1u,
  ulong4(p.attempt,p.parent,p.projection,p.stage),ulong4(p.thermochemistry,p.transport,p.cells,as_type<uint>(p.dx)));return;}
 ulong hash=14695981039346656037ul;
 for(uint word=0u;word<9u*p.cells;++word){hash^=ulong(as_type<uint>(state[word]));hash*=1099511628211ul;}
 for(uint word=0u;word<p.cells;++word){hash^=ulong(as_type<uint>(temperature[word]));hash*=1099511628211ul;}
 uint faceWords=p.faceOffset[2]+p.nx*p.ny*(p.nz+1u);
 for(uint word=0u;word<faceWords;++word){hash^=ulong(as_type<uint>(velocity[word]));hash*=1099511628211ul;}
 for(uint word=0u;word<3u*p.cells;++word){
  hash^=ulong(as_type<uint>(output[word]));hash*=1099511628211ul;}hash^=p.attempt;hash*=1099511628211ul;
 for(uint word=0u;word<7u*32u;++word){hash^=ulong(as_type<uint>(thermo[word]));hash*=1099511628211ul;}
 for(uint word=0u;word<6u*transport_stride;++word){hash^=ulong(as_type<uint>(transport[word]));hash*=1099511628211ul;}
 uint inflowWords=p.sideOffset[5]+p.nx*p.ny;
 for(uint word=0u;word<inflowWords;++word){hash^=ulong(inflow[word]);hash*=1099511628211ul;}
 hash^=p.parent;hash*=1099511628211ul;hash^=p.projection;hash*=1099511628211ul;hash^=ulong(p.stage);
 hash*=1099511628211ul;hash^=ulong(p.nx);hash*=1099511628211ul;hash^=ulong(p.ny);
 hash*=1099511628211ul;hash^=ulong(p.nz);hash*=1099511628211ul;hash^=ulong(as_type<uint>(p.dx));
 for(uint side=0u;side<6u;++side){hash*=1099511628211ul;hash^=ulong(p.boundary[side]);}
 hash*=1099511628211ul;hash^=p.thermochemistry;hash*=1099511628211ul;hash^=p.transport;
 hash*=1099511628211ul;hash^=ulong(as_type<uint>(p.Pr));hash*=1099511628211ul;
 hash^=ulong(as_type<uint>(p.Sc));hash*=1099511628211ul;hash^=ulong(as_type<uint>(p.Cv));
 identity[0]=hash==0ul?1ul:hash;}
struct PhysicalParams {uint advectiveNullity;uint physicalNullity;uint mutateHigh;float ambientT;};
struct EOSParams {uint cells;uint stage;uint precision;uint affineRowCount;
 float Tmin;float Tmax;float pressure;float feasibility;
 float dynamicsBound;float timeStepS;uint pad0;uint pad1;ulong attempt;ulong caseIdentity;};
struct EOSFCTParams {
 uint nx;uint ny;uint nz;uint cells;uint components;uint inequalities;uint nullity;uint affineRows;
 uint boundary[6];uint sideOffset[6];float dx;float dt;float feasibility;float assemblyReserve;
};
struct TargetParams {uint nx;uint ny;uint nz;uint cells;uint faceOffset[3];uint boundary[6];
 float dx;float dt;float tailThreshold;uint preauthored;uint policyVersion;uint pad0;uint pad1;
 ulong attempt;ulong sourcePacket;};
struct FrozenSourceParams {uint cells;uint pad0;uint pad1;uint pad2;ulong attempt;ulong sourcePacket;};
struct ProjectionConsumerParams {uint nx;uint ny;uint nz;uint cells;uint boundary[6];uint pad0;uint pad1;
 float dx;float dt;ulong attempt;};
inline uint target_face(constant TargetParams& p,uint axis,uint x,uint y,uint z){return p.faceOffset[axis]+
 (axis==0u?(z*p.ny+y)*(p.nx+1u)+x:(axis==1u?(z*(p.ny+1u)+y)*p.nx+x:
  (z*p.ny+y)*p.nx+x));}
inline uint pf_all_faces(constant TransportParams& p){return p.faceOffset[2]+p.nx*p.ny*(p.nz+1u);}
inline void pf_decode_face(constant TransportParams& p,uint packed,thread uint& axis,
 thread uint& x,thread uint& y,thread uint& z){if(packed<p.faceOffset[1]){axis=0u;uint r=packed;
 x=r%(p.nx+1u);r/=p.nx+1u;y=r%p.ny;z=r/p.ny;}else if(packed<p.faceOffset[2]){
 axis=1u;uint r=packed-p.faceOffset[1];x=r%p.nx;r/=p.nx;y=r%(p.ny+1u);z=r/(p.ny+1u);
 }else{axis=2u;uint r=packed-p.faceOffset[2];x=r%p.nx;r/=p.nx;y=r%p.ny;z=r/p.ny;}}
inline uint pf_coordinate(uint axis,uint x,uint y,uint z){return axis==0u?x:(axis==1u?y:z);}
inline uint pf_side_index(constant TransportParams& p,uint side,uint x,uint y,uint z){
 return p.sideOffset[side]+(side<2u?z*p.ny+y:(side<4u?z*p.nx+x:y*p.nx+x));}
inline float pf_stage_value(device const float* q,device const float* ambient,
 device const uchar* inflow,constant TransportParams& p,uint component,uint x,uint y,uint z,
 uint axis,int shift){int coordinate=int(pf_coordinate(axis,x,y,z))+shift;
 int extent=int(tr_extent(p,axis));if(coordinate>=0&&coordinate<extent){tr_set(axis,uint(coordinate),x,y,z);
  return q[component*p.cells+tr_cell(p,x,y,z)];}uint side=2u*axis+(coordinate>=extent?1u:0u);
 if(p.boundary[side]==0u){tr_set(axis,coordinate<0?uint(extent-1):0u,x,y,z);
  return q[component*p.cells+tr_cell(p,x,y,z)];}tr_set(axis,coordinate<0?0u:uint(extent-1),x,y,z);
 float interior=q[component*p.cells+tr_cell(p,x,y,z)];return p.boundary[side]==1u&&
  inflow[pf_side_index(p,side,x,y,z)]!=0u?ambient[component]:interior;}
inline float pf_mc(float backward,float forward){if(backward*forward<=0.0f)return 0.0f;
 float centered=0.5f*(backward+forward),sign=centered<0.0f?-1.0f:1.0f;
 return sign*min(abs(centered),2.0f*min(abs(backward),abs(forward)));}
inline float pf_log(float value){uint bits=as_type<uint>(value);int exponent=int((bits>>23u)&255u)-127;
 uint mantissa=(bits&0x007fffffu)|0x3f800000u;float normalized=as_type<float>(mantissa);
 volatile float numerator=normalized-1.0f,denominator=normalized+1.0f;
 volatile float y=numerator/denominator,y2=y*y,power=y,sum=power;
 for(uint odd=3u;odd<=17u;odd+=2u){power=power*y2;volatile float term=power/float(odd);sum=sum+term;}
 volatile float exponentTerm=float(exponent)*0.6931471805599453f;
 volatile float series=2.0f*sum;volatile float result=exponentTerm+series;return result;}
inline float pf_mass_slope(device const float* q,device const float* ambient,
 device const uchar* inflow,device const float* basis,device const float* projector,
 constant TransportParams& p,constant PhysicalParams& extra,uint component,uint x,uint y,uint z,
 uint axis){float coordinateSlope[8];for(uint b=0u;b<8u;++b)coordinateSlope[b]=0.0f;
 for(uint b=0u;b<extra.advectiveNullity;++b){float backward=0.0f,forward=0.0f;
  for(uint row=0u;row<8u;++row){float center=q[row*p.cells+tr_cell(p,x,y,z)];
   float previous=pf_stage_value(q,ambient,inflow,p,row,x,y,z,axis,-1);
   float next=pf_stage_value(q,ambient,inflow,p,row,x,y,z,axis,1);
   volatile float backwardDifference=center-previous,forwardDifference=next-center;
   volatile float backwardProduct=basis[row*extra.advectiveNullity+b]*backwardDifference;
   volatile float forwardProduct=basis[row*extra.advectiveNullity+b]*forwardDifference;
   backward+=backwardProduct;forward+=forwardProduct;}coordinateSlope[b]=pf_mc(backward,forward);}
 float result=0.0f;for(uint b=0u;b<extra.advectiveNullity;++b){float projected=0.0f;
  for(uint column=0u;column<extra.advectiveNullity;++column){volatile float product=
   projector[b*extra.advectiveNullity+column]*coordinateSlope[column];projected+=product;}
  volatile float product=basis[component*extra.advectiveNullity+b]*projected;result+=product;}return result;}
inline float pf_enthalpy(device const float* thermo,uint species,float temperature,float logT,
 thread bool& valid,device atomic_uint* obligations){device const float* record=thermo+32u*species;
 uint segments=uint(record[1]);device const float* selected=record+2u;bool found=false;
 for(uint segment=0u;segment<segments;++segment){device const float* candidate=record+2u+10u*segment;
  if(temperature>=candidate[0]&&(temperature<candidate[1]||(segment+1u==segments&&temperature==candidate[1]))){
   selected=candidate;found=true;atomic_fetch_or_explicit(obligations,1u<<(7u+min(segment,2u)),memory_order_relaxed);}}
 if(!found){valid=false;return 0.0f;}float inverse=1.0f/temperature;
 float t2=temperature*temperature,t3=t2*temperature,t4=t3*temperature,t5=t4*temperature;
 float primitive=-selected[2]*inverse+selected[3]*logT+selected[4]*temperature+
  selected[5]*t2*0.5f+selected[6]*t3*(1.0f/3.0f)+selected[7]*t4*0.25f+
  selected[8]*t5*0.2f;float h=8314.46261815324f*primitive/record[0]+selected[9];
 if(!isfinite(h))valid=false;return h;}
kernel void evaluate_resident_physical_flux(device const float* state [[buffer(0)]],
 device const float* temperature [[buffer(1)]],device const float* coefficients [[buffer(2)]],
 device const float* thermo [[buffer(3)]],device const float* physicalBasis [[buffer(4)]],
 device const float* ambient [[buffer(5)]],device const uchar* inflow [[buffer(6)]],
 device float* physicalMass [[buffer(7)]],device float* physicalEnergy [[buffer(8)]],
 device float* physicalGas [[buffer(9)]],device float* faceLogTemperature [[buffer(10)]],
 device float* faceEnthalpy [[buffer(11)]],device atomic_uint* failure [[buffer(12)]],
 device atomic_uint* obligations [[buffer(13)]],constant TransportParams& p [[buffer(14)]],
 constant PhysicalParams& extra [[buffer(15)]],uint gid [[thread_position_in_grid]]){
 uint all=pf_all_faces(p);if(gid>=all)return;uint axis,x,y,z;pf_decode_face(p,gid,axis,x,y,z);
 for(uint component=0u;component<8u;++component)physicalMass[component*all+gid]=0.0f;
 physicalEnergy[gid]=0.0f;physicalGas[gid]=0.0f;faceLogTemperature[gid]=0.0f;
 for(uint species=0u;species<7u;++species)faceEnthalpy[species*all+gid]=0.0f;
 uint coordinate=pf_coordinate(axis,x,y,z),extent=tr_extent(p,axis);bool boundary=coordinate==0u||coordinate==extent;
 if(p.boundary[2u*axis]==0u){boundary=false;if(coordinate==extent){tr_set(axis,0u,x,y,z);
  atomic_fetch_or_explicit(obligations,1u<<1u,memory_order_relaxed);}}
 uint lx=x,ly=y,lz=z,rx=x,ry=y,rz=z,left=0u,right=0u;bool leftAmbient=false,rightAmbient=false;
 float leftT=0.0f,rightT=0.0f,rhoD=0.0f,k=0.0f,distance=p.dx;bool valid=true;
 if(boundary){bool upper=coordinate==extent;uint side=2u*axis+(upper?1u:0u);
  if(p.boundary[side]==2u){atomic_fetch_or_explicit(obligations,1u<<2u,memory_order_relaxed);return;}
  if(inflow[pf_side_index(p,side,x,y,z)]==0u){atomic_fetch_or_explicit(obligations,1u<<3u,memory_order_relaxed);return;}
  atomic_fetch_or_explicit(obligations,1u<<4u,memory_order_relaxed);uint normal=upper?extent-1u:0u;
  tr_set(axis,normal,x,y,z);left=right=tr_cell(p,x,y,z);leftAmbient=!upper;rightAmbient=upper;
  leftT=leftAmbient?extra.ambientT:temperature[left];rightT=rightAmbient?extra.ambientT:temperature[right];
  float total=0.0f;for(uint species=0u;species<7u;++species)total+=state[(1u+species)*p.cells+left];
  rhoD=total*coefficients[left];k=coefficients[p.cells+left];distance=0.5f*p.dx;
 }else{atomic_fetch_or_explicit(obligations,1u<<0u,memory_order_relaxed);
  uint rightCoordinate=coordinate==extent?0u:coordinate,leftCoordinate=rightCoordinate==0u?extent-1u:rightCoordinate-1u;
  tr_set(axis,leftCoordinate,lx,ly,lz);tr_set(axis,rightCoordinate,rx,ry,rz);
  left=tr_cell(p,lx,ly,lz);right=tr_cell(p,rx,ry,rz);leftT=temperature[left];rightT=temperature[right];
  float totalLeft=0.0f,totalRight=0.0f;for(uint species=0u;species<7u;++species){
   totalLeft+=state[(1u+species)*p.cells+left];totalRight+=state[(1u+species)*p.cells+right];}
  float a=totalLeft*coefficients[left],b=totalRight*coefficients[right];
  rhoD=a>0.0f&&b>0.0f?2.0f*a*b/(a+b):0.0f;a=coefficients[p.cells+left];b=coefficients[p.cells+right];
  k=a>0.0f&&b>0.0f?2.0f*a*b/(a+b):0.0f;
  atomic_fetch_or_explicit(obligations,1u<<((rhoD>0.0f&&k>0.0f)?5u:6u),memory_order_relaxed);}
 float totalLeft=0.0f,totalRight=0.0f;for(uint species=0u;species<7u;++species){
  totalLeft+=leftAmbient?ambient[1u+species]:state[(1u+species)*p.cells+left];
  totalRight+=rightAmbient?ambient[1u+species]:state[(1u+species)*p.cells+right];}
 if(!(totalLeft>0.0f)||!(totalRight>0.0f))valid=false;float raw[8],projected[8];
 for(uint component=0u;component<8u;++component){float ql=leftAmbient?ambient[component]:state[component*p.cells+left];
  float qr=rightAmbient?ambient[component]:state[component*p.cells+right];raw[component]=-rhoD*(qr/totalRight-ql/totalLeft)/distance;}
 for(uint component=0u;component<8u;++component){float value=0.0f;for(uint b=0u;b<extra.physicalNullity;++b){
  float coordinateValue=0.0f;for(uint row=0u;row<8u;++row){volatile float product=
   physicalBasis[row*extra.physicalNullity+b]*raw[row];coordinateValue+=product;}
  volatile float product=physicalBasis[component*extra.physicalNullity+b]*coordinateValue;value+=product;}
  projected[component]=value;physicalMass[component*all+gid]=value;}
 float gas=0.0f;for(uint component=1u;component<=6u;++component)gas+=projected[component];physicalGas[gid]=gas;
 float faceT=0.5f*(leftT+rightT),logT=pf_log(faceT),energy=0.0f;faceLogTemperature[gid]=logT;
 for(uint species=0u;species<7u;++species){
  float h=pf_enthalpy(thermo,species,faceT,logT,valid,obligations);
  faceEnthalpy[species*all+gid]=h;energy+=h*projected[1u+species];}
 energy-=k*(rightT-leftT)/distance;physicalEnergy[gid]=energy;
 for(uint component=0u;component<8u;++component)valid=valid&&isfinite(projected[component]);
 if(!valid||!isfinite(gas)||!isfinite(energy))atomic_fetch_or_explicit(failure,2u,memory_order_relaxed);}
kernel void evaluate_resident_advective_pair(device const float* state [[buffer(0)]],
 device const float* velocity [[buffer(1)]],device const float* ambient [[buffer(2)]],
 device const uchar* inflow [[buffer(3)]],device const float* basis [[buffer(4)]],
 device const float* projector [[buffer(5)]],device float* donorOutput [[buffer(6)]],
 device float* highOutput [[buffer(7)]],device atomic_uint* obligations [[buffer(8)]],
 constant TransportParams& p [[buffer(9)]],constant PhysicalParams& extra [[buffer(10)]],
 uint gid [[thread_position_in_grid]]){uint all=pf_all_faces(p);if(gid>=9u*all)return;
 uint component=gid/all,packed=gid-component*all,axis,x,y,z;pf_decode_face(p,packed,axis,x,y,z);
 uint coordinate=pf_coordinate(axis,x,y,z),extent=tr_extent(p,axis);float u=velocity[packed];
 if((coordinate==0u||coordinate==extent)&&p.boundary[2u*axis+(coordinate==extent?1u:0u)]==2u){
  donorOutput[gid]=0.0f;highOutput[gid]=0.0f;return;}
 if((coordinate==0u||coordinate==extent)&&p.boundary[2u*axis+(coordinate==extent?1u:0u)]!=0u){
  float donor=pf_stage_value(state,ambient,inflow,p,component,x,y,z,axis,u>=0.0f?-1:0);
  donorOutput[gid]=u*donor;highOutput[gid]=0.0f;
  atomic_fetch_or_explicit(obligations,1u<<11u,memory_order_relaxed);return;}
 uint rightCoordinate=coordinate==extent?0u:coordinate,leftCoordinate=rightCoordinate==0u?extent-1u:rightCoordinate-1u;
 uint lx=x,ly=y,lz=z,rx=x,ry=y,rz=z;tr_set(axis,leftCoordinate,lx,ly,lz);tr_set(axis,rightCoordinate,rx,ry,rz);
 bool fromLeft=u>=0.0f;uint dx=fromLeft?lx:rx,dy=fromLeft?ly:ry,dz=fromLeft?lz:rz;
 float donor=state[component*p.cells+tr_cell(p,dx,dy,dz)],slope=component<8u?
  pf_mass_slope(state,ambient,inflow,basis,projector,p,extra,component,dx,dy,dz,axis):
  pf_mc(donor-pf_stage_value(state,ambient,inflow,p,component,dx,dy,dz,axis,-1),
   pf_stage_value(state,ambient,inflow,p,component,dx,dy,dz,axis,1)-donor);
 volatile float slopeDose=(fromLeft?0.5f:-0.5f)*slope;
 volatile float high=donor+slopeDose,lowFlux=u*donor,highDifference=high-donor;
 volatile float deltaFlux=u*highDifference;
 donorOutput[gid]=lowFlux;highOutput[gid]=deltaFlux;
 atomic_fetch_or_explicit(obligations,1u<<12u,memory_order_relaxed);}
kernel void finalize_resident_advective_high(device const float* donor [[buffer(0)]],
 device const float* delta [[buffer(1)]],device float* high [[buffer(2)]],
 constant TransportParams& p [[buffer(3)]],
 uint gid [[thread_position_in_grid]]){uint total=9u*pf_all_faces(p);if(gid>=total)return;
 high[gid]=donor[gid]+delta[gid];}
kernel void compose_resident_flux_pair(device const float* donor [[buffer(0)]],
 device const float* highAdvective [[buffer(1)]],device const float* physicalMass [[buffer(2)]],
 device const float* physicalEnergy [[buffer(3)]],device float* low [[buffer(4)]],
 device float* high [[buffer(5)]],constant TransportParams& p [[buffer(6)]],
 constant PhysicalParams& extra [[buffer(7)]],uint gid [[thread_position_in_grid]]){
 uint all=pf_all_faces(p);if(gid>=9u*all)return;uint component=gid/all,packed=gid-component*all;
 float physical=component<8u?physicalMass[component*all+packed]:physicalEnergy[packed];
 low[gid]=donor[gid]+physical;high[gid]=highAdvective[gid]+physical;
 if(extra.mutateHigh!=0u&&gid==all+1u)high[gid]+=0.0009765625f;}
kernel void validate_resident_physical_flux(device const float* donor [[buffer(0)]],
 device const float* highAdvective [[buffer(1)]],device const float* physicalMass [[buffer(2)]],
 device const float* physicalEnergy [[buffer(3)]],device const float* physicalGas [[buffer(4)]],
 device const float* low [[buffer(5)]],device const float* high [[buffer(6)]],
 device atomic_uint* failure [[buffer(7)]],device atomic_uint* obligations [[buffer(8)]],
 constant TransportParams& p [[buffer(9)]],uint gid [[thread_position_in_grid]]){
 uint all=pf_all_faces(p);if(gid>=9u*all)return;uint component=gid/all,packed=gid-component*all;
 float physical=component<8u?physicalMass[component*all+packed]:physicalEnergy[packed];
 if(!isfinite(donor[gid])||!isfinite(highAdvective[gid])||!isfinite(physical)||
  !isfinite(low[gid])||!isfinite(high[gid])||!isfinite(physicalGas[packed]))
  atomic_fetch_or_explicit(failure,4u,memory_order_relaxed);
 if(as_type<uint>(low[gid])!=as_type<uint>(donor[gid]+physical)||
  as_type<uint>(high[gid])!=as_type<uint>(highAdvective[gid]+physical))
  atomic_fetch_or_explicit(failure,8u,memory_order_relaxed);
 else atomic_fetch_or_explicit(obligations,1u<<10u,memory_order_relaxed);}
// Scheduling-only gather: every SIMD lane replays exactly the original FNV
// word order. Loads are coalesced; no XOR reduction or changed digest scheme.
inline ulong payload_fnv_words(ulong hash,device const float* input,uint count,
 uint lane,uint width){for(uint base=0u;base<count;base+=width){uint word=base+lane;
 uint value=word<count?as_type<uint>(input[word]):0u;
 for(uint i=0u;i<min(width,count-base);++i){hash^=ulong(simd_shuffle(value,i));
  hash*=1099511628211ul;}}return hash;}
inline ulong payload_fnv_five(ulong hash,device const float* a,device const float* b,
 device const float* c,device const float* d,device const float* e,uint count,
 uint lane,uint width){for(uint base=0u;base<count;base+=width){uint word=base+lane;
 uint av=word<count?as_type<uint>(a[word]):0u,bv=word<count?as_type<uint>(b[word]):0u,
 cv=word<count?as_type<uint>(c[word]):0u,dv=word<count?as_type<uint>(d[word]):0u,
 ev=word<count?as_type<uint>(e[word]):0u;
 for(uint i=0u;i<min(width,count-base);++i){hash^=ulong(simd_shuffle(av,i));hash*=1099511628211ul;
  hash^=ulong(simd_shuffle(bv,i));hash*=1099511628211ul;
  hash^=ulong(simd_shuffle(cv,i));hash*=1099511628211ul;
  hash^=ulong(simd_shuffle(dv,i));hash*=1099511628211ul;
  hash^=ulong(simd_shuffle(ev,i));hash*=1099511628211ul;}}return hash;}
inline ulong payload_fnv_three(ulong hash,device const float* a,device const float* b,
 device const float* c,uint count,uint lane,uint width){
 for(uint base=0u;base<count;base+=width){uint word=base+lane;
 uint av=word<count?as_type<uint>(a[word]):0u,bv=word<count?as_type<uint>(b[word]):0u,
 cv=word<count?as_type<uint>(c[word]):0u;
 for(uint i=0u;i<min(width,count-base);++i){hash^=ulong(simd_shuffle(av,i));hash*=1099511628211ul;
  hash^=ulong(simd_shuffle(bv,i));hash*=1099511628211ul;
  hash^=ulong(simd_shuffle(cv,i));hash*=1099511628211ul;}}return hash;}
kernel void identify_resident_physical_flux(device const float* donor [[buffer(0)]],
 device const float* advectiveDelta [[buffer(1)]],device const float* highAdvective [[buffer(2)]],
 device const float* physicalMass [[buffer(3)]],device const float* physicalEnergy [[buffer(4)]],
 device const float* physicalGas [[buffer(5)]],device const float* low [[buffer(6)]],
 device const float* high [[buffer(7)]],device const float* faceLogTemperature [[buffer(8)]],
 device const float* faceEnthalpy [[buffer(9)]],device const ulong* transportIdentity [[buffer(10)]],
 device const float* ambient [[buffer(11)]],device const uchar* inflow [[buffer(12)]],
 device const float* physicalBasis [[buffer(13)]],device const float* advectiveBasis [[buffer(14)]],
 device const float* projector [[buffer(15)]],device const ulong* endpointClassIdentity [[buffer(16)]],
 device ulong* identity [[buffer(17)]],device atomic_uint* failure [[buffer(18)]],
 constant TransportParams& p [[buffer(19)]],constant PhysicalParams& extra [[buffer(20)]],
 uint gid [[thread_position_in_grid]],uint width [[threads_per_simdgroup]]){
 if(atomic_load_explicit(failure,memory_order_relaxed)!=0u||transportIdentity[0]==0ul||
  endpointClassIdentity[0]==0ul){if(gid==0u)identity[0]=0ul;return;}
 if(!resident_full_payload_seals){if(gid==0u)identity[0]=resident_stage_token(2u,
  ulong4(transportIdentity[0],endpointClassIdentity[0],p.attempt,p.stage),
  ulong4(extra.physicalNullity,extra.advectiveNullity,p.cells,as_type<uint>(extra.ambientT)));return;}
 ulong hash=14695981039346656037ul,all=pf_all_faces(p);hash^=transportIdentity[0];hash*=1099511628211ul;
 hash^=endpointClassIdentity[0];hash*=1099511628211ul;
 hash=payload_fnv_five(hash,donor,advectiveDelta,highAdvective,low,high,9u*all,gid,width);
 hash=payload_fnv_words(hash,physicalMass,8u*all,gid,width);
 hash=payload_fnv_three(hash,physicalEnergy,physicalGas,faceLogTemperature,all,gid,width);
 hash=payload_fnv_words(hash,faceEnthalpy,7u*all,gid,width);
 for(uint word=0u;word<9u;++word){hash^=ulong(as_type<uint>(ambient[word]));hash*=1099511628211ul;}
 uint inflowWords=p.sideOffset[5]+p.nx*p.ny;for(uint word=0u;word<inflowWords;++word){hash^=ulong(inflow[word]);hash*=1099511628211ul;}
 for(uint word=0u;word<8u*extra.physicalNullity;++word){hash^=ulong(as_type<uint>(physicalBasis[word]));hash*=1099511628211ul;}
 for(uint word=0u;word<8u*extra.advectiveNullity;++word){hash^=ulong(as_type<uint>(advectiveBasis[word]));hash*=1099511628211ul;}
 for(uint word=0u;word<extra.advectiveNullity*extra.advectiveNullity;++word){hash^=ulong(as_type<uint>(projector[word]));hash*=1099511628211ul;}
 hash^=ulong(as_type<uint>(extra.ambientT));hash*=1099511628211ul;
 if(gid==0u)identity[0]=hash==0ul?1ul:hash;}
kernel void identify_resident_eos_candidate(device const float* candidate [[buffer(0)]],
 device const float* sourceDelta [[buffer(1)]],device const float* alpha [[buffer(2)]],
 device const ulong* physicalIdentity [[buffer(3)]],device const ulong* transportIdentity [[buffer(4)]],
 device ulong* identity [[buffer(5)]],device atomic_uint* failure [[buffer(6)]],
 device atomic_uint* obligations [[buffer(7)]],constant EOSParams& p [[buffer(8)]],
 constant TransportParams& transport [[buffer(9)]],
 constant EOSFCTParams& fct [[buffer(10)]],
 uint gid [[thread_position_in_grid]]){if(gid!=0u)return;
 if(atomic_load_explicit(failure,memory_order_relaxed)!=0u||physicalIdentity[0]==0ul||
  transportIdentity[0]==0ul||p.precision!=2u||p.stage!=1u||p.cells!=transport.cells||
  p.cells!=fct.cells||p.attempt!=transport.attempt||
  as_type<uint>(p.timeStepS)!=as_type<uint>(fct.dt)){
  identity[0]=0ul;atomic_fetch_or_explicit(failure,16u,memory_order_relaxed);return;}
 // The parallel EOS admissibility kernel checks every candidate component;
 // only the redundant payload scan/hash is absent in production.
 if(!resident_full_payload_seals){identity[0]=resident_stage_token(3u,
  ulong4(transportIdentity[0],physicalIdentity[0],p.attempt,p.caseIdentity),
  ulong4(p.stage,p.precision,as_type<uint>(p.timeStepS),as_type<uint>(p.dynamicsBound)));return;}
 ulong hash=14695981039346656037ul;hash^=transportIdentity[0];hash*=1099511628211ul;
 hash^=physicalIdentity[0];hash*=1099511628211ul;
 for(uint word=0u;word<9u*p.cells;++word){float value=candidate[word];
  if(!isfinite(value)){identity[0]=0ul;atomic_fetch_or_explicit(failure,32u,memory_order_relaxed);return;}
  hash^=ulong(as_type<uint>(value));hash*=1099511628211ul;
  hash^=ulong(as_type<uint>(sourceDelta[word]));hash*=1099511628211ul;}
 uint allFaces=pf_all_faces(transport);for(uint word=0u;word<allFaces;++word){
  hash^=ulong(as_type<uint>(alpha[word]));hash*=1099511628211ul;}
 hash^=p.attempt;hash*=1099511628211ul;hash^=p.caseIdentity;hash*=1099511628211ul;
 hash^=ulong(p.stage);hash*=1099511628211ul;hash^=ulong(p.precision);hash*=1099511628211ul;
 hash^=ulong(as_type<uint>(p.Tmin));hash*=1099511628211ul;
 hash^=ulong(as_type<uint>(p.Tmax));hash*=1099511628211ul;
 hash^=ulong(as_type<uint>(p.pressure));hash*=1099511628211ul;
 hash^=ulong(as_type<uint>(p.feasibility));hash*=1099511628211ul;
 hash^=ulong(as_type<uint>(p.dynamicsBound));hash*=1099511628211ul;
 hash^=ulong(as_type<uint>(p.timeStepS));hash*=1099511628211ul;
 identity[0]=hash==0ul?1ul:hash;atomic_fetch_or_explicit(obligations,
  (1u<<0u)|(1u<<1u),memory_order_relaxed);}
kernel void finalize_resident_eos_candidate(device const ulong* producerIdentity [[buffer(0)]],
 device ulong* acceptedIdentity [[buffer(1)]],device atomic_uint* failure [[buffer(2)]],
 uint gid [[thread_position_in_grid]]){if(gid!=0u)return;
 acceptedIdentity[0]=atomic_load_explicit(failure,memory_order_relaxed)==0u?producerIdentity[0]:0ul;}
// Apple GPUs expose no binary64 arithmetic.  These non-overlapping float triples
// carry every binary64 thermochemistry bit through a compensated device-only
// evaluation.  The inversion searches the binary32 lattice and decides the
// final rounding at the exact midpoint between adjacent floats; it therefore
// validates the production surface against the binary64 mirror's one-time
// binary32 projection rather than against a looser numerical tolerance.
)METAL")+R"METAL(
struct EOSDD {float hi;float lo;float tail;float bound;};
inline EOSDD eos_dd(float hi,float lo=0.0f,float tail=0.0f,float bound=0.0f){
 EOSDD value={hi,lo,tail,bound};return value;}
inline float eos_up_add(float first,float second){float value=first+second;
 return isfinite(value)?nextafter(value,INFINITY):value;}
inline float eos_up_mul(float first,float second){float value=first*second;
 return isfinite(value)?nextafter(value,INFINITY):value;}
inline float eos_up_div(float first,float second){float value=first/second;
 return isfinite(value)?nextafter(value,INFINITY):value;}
inline float eos_down_sub(float first,float second){float value=first-second;
 return isfinite(value)?nextafter(value,-INFINITY):value;}
inline float eos_down_mul(float first,float second){float value=first*second;
 return isfinite(value)?nextafter(value,-INFINITY):value;}
inline float eos_local_spacing(float value){float upper=nextafter(value,INFINITY),
 lower=nextafter(value,-INFINITY);return max(abs(upper-value),abs(value-lower));}
inline float eos_component_abs_sum(EOSDD value){return eos_up_add(eos_up_add(abs(value.hi),
 abs(value.lo)),abs(value.tail));}
inline EOSDD eos_two_sum(float first,float second){float sum=first+second;
 float virtualSecond=sum-first,error=(first-(sum-virtualSecond))+(second-virtualSecond);
 return eos_dd(sum,error);}
inline EOSDD eos_renorm(float first,float second,float third,float bound=0.0f){EOSDD a=eos_two_sum(first,second);
 EOSDD b=eos_two_sum(a.lo,third),c=eos_two_sum(a.hi,b.hi),d=eos_two_sum(c.lo,b.lo);
 EOSDD e=eos_two_sum(c.hi,d.hi);return eos_dd(e.hi,e.lo,d.lo,bound);}
inline EOSDD eos_grow(EOSDD value,float term){EOSDD leading=eos_two_sum(value.hi,term),
 middle=eos_two_sum(value.lo,leading.lo),trailing=eos_two_sum(value.tail,middle.lo);
 float discarded=eos_up_add(abs(trailing.lo),16.0f*0x1p-149f);
 return eos_renorm(leading.hi,middle.hi,trailing.hi,eos_up_add(value.bound,discarded));}
inline EOSDD eos_add(EOSDD first,EOSDD second){EOSDD result=first;
 result.bound=eos_up_add(first.bound,second.bound);result=eos_grow(result,second.hi);
 result=eos_grow(result,second.lo);return eos_grow(result,second.tail);}
inline EOSDD eos_neg(EOSDD value){return eos_dd(-value.hi,-value.lo,-value.tail,value.bound);}
inline EOSDD eos_sub(EOSDD first,EOSDD second){return eos_add(first,eos_neg(second));}
inline EOSDD eos_abs(EOSDD value){return value.hi<0.0f||(value.hi==0.0f&&(value.lo<0.0f||
 (value.lo==0.0f&&value.tail<0.0f)))?
 eos_neg(value):value;}
inline EOSDD eos_mul(EOSDD first,EOSDD second){
 float firstScale=eos_component_abs_sum(first),secondScale=eos_component_abs_sum(second);
 float propagated=eos_up_add(eos_up_add(eos_up_mul(firstScale,second.bound),
  eos_up_mul(secondScale,first.bound)),eos_up_mul(first.bound,second.bound));
 EOSDD result=eos_dd(0.0f,0.0f,0.0f,propagated);float firstPart[3]={first.hi,first.lo,
  first.tail},secondPart[3]={second.hi,second.lo,second.tail};
 for(uint i=0u;i<3u;++i)for(uint j=0u;j<3u;++j){float product=firstPart[i]*secondPart[j];
  float error=fma(firstPart[i],secondPart[j],-product);result=eos_grow(result,product);
  result=eos_grow(result,error);}return result;}
inline float eos_center_abs_lower(EOSDD value){float leading=abs(value.hi),remainder=
 eos_up_add(abs(value.lo),abs(value.tail));if(leading==0.0f){leading=abs(value.lo);
 remainder=abs(value.tail);}float lower=leading-remainder;
 return lower>0.0f?max(0.0f,nextafter(lower,-INFINITY)):0.0f;}
inline float eos_abs_upper(EOSDD value){return eos_up_add(eos_component_abs_sum(value),value.bound);}
inline float eos_abs_lower(EOSDD value){float center=eos_center_abs_lower(value);
 float lower=center-value.bound;return lower>0.0f?max(0.0f,nextafter(lower,-INFINITY)):0.0f;}
inline EOSDD eos_div(EOSDD numerator,EOSDD denominator){float quotient=numerator.hi/denominator.hi;
 EOSDD remainder=eos_sub(numerator,eos_mul(denominator,eos_dd(quotient)));
 float correction=(remainder.hi+remainder.lo+remainder.tail)/denominator.hi;
 EOSDD secondRemainder=eos_sub(remainder,eos_mul(denominator,eos_dd(correction)));
 float tail=(secondRemainder.hi+secondRemainder.lo+secondRemainder.tail)/denominator.hi;
 EOSDD result=eos_renorm(quotient,correction,tail);EOSDD certifiedResidual=eos_sub(
  numerator,eos_mul(denominator,result));float denominatorLower=eos_abs_lower(denominator);
 result.bound=denominatorLower>0.0f?eos_up_div(eos_abs_upper(certifiedResidual),
  denominatorLower):INFINITY;return result;}
// Returns -1/0/+1 for a proved order and 2 when the propagated intervals overlap.
inline int eos_order(EOSDD first,EOSDD second){EOSDD difference=eos_sub(first,second);
 if(difference.hi==0.0f&&difference.lo==0.0f&&difference.tail==0.0f&&
  difference.bound==0.0f)return 0;float separation=eos_center_abs_lower(difference);
 if(!(separation>difference.bound))return 2;bool negative=difference.hi<0.0f||
  (difference.hi==0.0f&&(difference.lo<0.0f||
   (difference.lo==0.0f&&difference.tail<0.0f)));return negative?-1:1;}
inline bool eos_proved_leq(EOSDD first,EOSDD second){int order=eos_order(first,second);
 return order==-1||order==0;}
// Exact grow-expansion walker for the six signed face samples in a 3-D
// divergence numerator.  Every input is binary32 and no term is discarded;
// therefore an all-zero terminal expansion proves the real numerator is zero.
inline bool eos_six_term_sum_is_exact_zero(thread float* terms){
 float expansion[7],next[7];uint count=0u;
 for(uint term=0u;term<6u;++term){float q=terms[term];uint nextCount=0u;
  for(uint index=0u;index<count;++index){EOSDD pair=eos_two_sum(q,expansion[index]);
   if(pair.lo!=0.0f)next[nextCount++]=pair.lo;q=pair.hi;}
  if(q!=0.0f||nextCount==0u)next[nextCount++]=q;
  count=nextCount;for(uint index=0u;index<count;++index)expansion[index]=next[index];}
 for(uint index=0u;index<count;++index)if(expansion[index]!=0.0f)return false;
 return true;}
inline float eos_scale_tiny_by_2p126(float value){uint bits=as_type<uint>(value),
 magnitude=bits&0x7fffffffu;if(magnitude==0u)return value;
 if((magnitude&0x7f800000u)==0u){float scaled=float(magnitude)*0x1p-23f;
  return (bits&0x80000000u)!=0u?-scaled:scaled;}return ldexp(value,126);}
inline EOSDD eos_scale_power_of_two(EOSDD value,int exponent){return eos_renorm(
 ldexp(value.hi,exponent),ldexp(value.lo,exponent),ldexp(value.tail,exponent),
 ldexp(value.bound,exponent));}
inline EOSDD eos_load_dd(device const float* source,uint offset){float tail=source[offset+2u];
 return eos_dd(source[offset],source[offset+1u],tail,eos_local_spacing(tail));}
inline EOSDD eos_log_dd(EOSDD value){uint bits=as_type<uint>(value.hi);int exponent=int((bits>>23u)&255u)-127;
 float power=as_type<float>(uint(exponent+127)<<23u);EOSDD normalized=eos_div(value,eos_dd(power));
 EOSDD y=eos_div(eos_sub(normalized,eos_dd(1.0f)),eos_add(normalized,eos_dd(1.0f)));
 EOSDD y2=eos_mul(y,y),term=y,sum=y;
 for(uint odd=3u;odd<=49u;odd+=2u){term=eos_mul(term,y2);sum=eos_add(sum,eos_div(term,eos_dd(float(odd))));}
 EOSDD logarithm=eos_mul(eos_dd(2.0f),sum);
 float yMagnitude=eos_abs_upper(y),remainderPower=yMagnitude;
 for(uint factor=1u;factor<51u;++factor)remainderPower=eos_up_mul(remainderPower,yMagnitude);
 float denominatorFactor=eos_down_sub(1.0f,eos_up_mul(yMagnitude,yMagnitude));
 float denominator=denominatorFactor>0.0f?eos_down_mul(51.0f,denominatorFactor):0.0f;
 float truncation=denominator>0.0f?eos_up_div(eos_up_mul(2.0f,remainderPower),denominator):INFINITY;
 logarithm.bound=eos_up_add(logarithm.bound,truncation);
 EOSDD ln2=eos_dd(0.693147182464599609375f,-1.9046542121259336e-9f,
  -1.1102230246251565e-16f,0x1p-54f);
 EOSDD result=eos_add(logarithm,eos_mul(eos_dd(float(exponent)),ln2));
 // This is the interval for the mathematical log evaluated by the device.
 // Host-libm projection error is a qualification-comparator property and must
 // not inflate a live physical value's correct-rounding interval.
 return result;}
inline bool eos_proves_binary32_bin(EOSDD value,float rounded){
 if(!isfinite(rounded))return false;uint magnitude=as_type<uint>(rounded)&0x7fffffffu;
 bool tiny=magnitude<=0x00800000u;EOSDD normalized=tiny?eos_renorm(
  eos_scale_tiny_by_2p126(value.hi),eos_scale_tiny_by_2p126(value.lo),
  eos_scale_tiny_by_2p126(value.tail),eos_scale_tiny_by_2p126(value.bound)):
  eos_renorm(value.hi,value.lo,value.tail,value.bound);
 float lower=nextafter(rounded,-INFINITY),
  upper=nextafter(rounded,INFINITY),lowerHalf=(lower-rounded)*0.5f,
  upperHalf=(upper-rounded)*0.5f;EOSDD lowerMidpoint,upperMidpoint;
 if(tiny||lowerHalf==0.0f||upperHalf==0.0f){float scaledRounded=
   eos_scale_tiny_by_2p126(rounded);lower=eos_scale_tiny_by_2p126(lower);
  upper=eos_scale_tiny_by_2p126(upper);
  lowerMidpoint=eos_add(eos_dd(scaledRounded),eos_dd((lower-scaledRounded)*0.5f));
  upperMidpoint=eos_add(eos_dd(scaledRounded),eos_dd((upper-scaledRounded)*0.5f));
 }else{lowerMidpoint=eos_add(eos_dd(rounded),eos_dd(lowerHalf));
  upperMidpoint=eos_add(eos_dd(rounded),eos_dd(upperHalf));}
 // For small normal results, compare the certified expansion to its rounding
 // midpoints after an exact common power-of-two scale.  This prevents the
 // subtraction proof itself from collapsing to binary32 minimum-subnormal
 // resolution; it changes neither the value nor its rounding interval.
 uint exponentField=(magnitude>>23u)&255u;
 if(!tiny&&exponentField>0u&&exponentField<96u){int proofScale=127-int(exponentField);
  normalized=eos_scale_power_of_two(normalized,proofScale);
  lowerMidpoint=eos_scale_power_of_two(lowerMidpoint,proofScale);
  upperMidpoint=eos_scale_power_of_two(upperMidpoint,proofScale);}
 return eos_order(lowerMidpoint,normalized)==-1&&
  eos_order(normalized,upperMidpoint)==-1;}
inline bool eos_unique_binary32_round(EOSDD value,thread float& rounded){
 rounded=value.hi;if(eos_proves_binary32_bin(value,rounded))return true;
 // A renormalized triple's leading float can round a midpoint tie before its
 // trailing component decides the side. The interval, not hi, owns rounding.
 // Test adjacent bins with the SAME strict proof; a straddling interval still
 // fails all three. No radius, threshold, or accepted uncertainty is changed.
 float below=nextafter(value.hi,-INFINITY),above=nextafter(value.hi,INFINITY);
 if(eos_proves_binary32_bin(value,below)){rounded=below;return true;}
 if(eos_proves_binary32_bin(value,above)){rounded=above;return true;}
 return false;}
// Materialize an interval-certified binary32 value when correct-rounding is
// undecidable at a midpoint.  The complete propagated interval is the local
// acceptance contract: no fixed ULP cap, cancellation scale, or measured
// residual enters this decision.  Callers record every enclosure branch in the
// obligation bitmap, and qualification compares the materialized value with
// the fp64 owner against the independently propagated termwise enclosure.
inline bool eos_binary32_round_or_local_enclosure(EOSDD value,thread float& rounded,
 thread float& radius,thread bool& usedEnclosure){usedEnclosure=false;
 if(eos_unique_binary32_round(value,rounded)){radius=eos_up_mul(0.5f,
  eos_local_spacing(rounded));return isfinite(radius);}rounded=value.hi;
 float uncertainty=eos_up_add(eos_up_add(abs(value.lo),abs(value.tail)),value.bound);
 if(isfinite(rounded)&&isfinite(uncertainty)){radius=uncertainty;usedEnclosure=true;return true;}
 return false;}
inline bool eos_select_dd_segment(device const float* thermo,uint species,EOSDD temperature,
 thread uint& offset,device atomic_uint* obligations,thread bool& valid){uint base=96u*species,
 segments=uint(thermo[base+3u]);
 for(uint segment=0u;segment<segments;++segment){uint candidate=base+4u+30u*segment;
  EOSDD lower=eos_load_dd(thermo,candidate),upper=eos_load_dd(thermo,candidate+3u);
  int lowerOrder=eos_order(temperature,lower),upperOrder=eos_order(temperature,upper);
  if(lowerOrder==2||upperOrder==2){valid=false;return false;}
  bool above=lowerOrder>=0,below=upperOrder<0||(segment+1u==segments&&upperOrder==0);
  if(above&&below){offset=candidate;atomic_fetch_or_explicit(obligations,
    1u<<(7u+min(segment,2u)),memory_order_relaxed);return true;}}
 return false;}
inline EOSDD eos_enthalpy_dd(device const float* thermo,uint species,EOSDD temperature,
 device atomic_uint* obligations,thread bool& valid){uint offset=0u;
 if(!eos_select_dd_segment(thermo,species,temperature,offset,obligations,valid)){
  valid=false;return eos_dd(0.0f);}
 EOSDD inverse=eos_div(eos_dd(1.0f),temperature),logT=eos_log_dd(temperature);
 EOSDD t2=eos_mul(temperature,temperature),t3=eos_mul(t2,temperature),t4=eos_mul(t3,temperature),
  t5=eos_mul(t4,temperature);EOSDD primitive=eos_neg(eos_mul(eos_load_dd(thermo,offset+6u),inverse));
 primitive=eos_add(primitive,eos_mul(eos_load_dd(thermo,offset+9u),logT));
 primitive=eos_add(primitive,eos_mul(eos_load_dd(thermo,offset+12u),temperature));
 primitive=eos_add(primitive,eos_mul(eos_load_dd(thermo,offset+15u),eos_mul(t2,eos_dd(0.5f))));
 primitive=eos_add(primitive,eos_mul(eos_load_dd(thermo,offset+18u),eos_div(t3,eos_dd(3.0f))));
 primitive=eos_add(primitive,eos_mul(eos_load_dd(thermo,offset+21u),eos_mul(t4,eos_dd(0.25f))));
 primitive=eos_add(primitive,eos_mul(eos_load_dd(thermo,offset+24u),eos_mul(t5,eos_dd(0.2f))));
 EOSDD gasConstant=eos_load_dd(thermo,7u*96u),weight=eos_load_dd(thermo,96u*species);
 return eos_add(eos_mul(eos_div(gasConstant,weight),primitive),eos_load_dd(thermo,offset+27u));}
inline EOSDD eos_energy_dd(device const float* state,device const float* thermo,
 constant EOSParams& p,uint cell,EOSDD temperature,device atomic_uint* obligations,thread bool& valid){
 EOSDD energy=eos_dd(0.0f);for(uint species=0u;species<7u;++species)energy=eos_add(energy,
  eos_mul(eos_dd(state[(species+1u)*p.cells+cell]),eos_enthalpy_dd(thermo,species,
   temperature,obligations,valid)));return energy;}
inline bool eos_within_positive_envelope(EOSDD residual,EOSDD scale,float feasibility){
 int scaleOrder=eos_order(scale,eos_dd(1.0f));if(scaleOrder==-1||scaleOrder==2)scale=eos_dd(1.0f);
 EOSDD threshold=eos_mul(eos_dd(feasibility),scale);return eos_proved_leq(residual,threshold);}
inline bool eos_state_admissible(device const float* state,device const float* thermo,
 constant EOSParams& p,uint cell,device atomic_uint* obligations){
 float values[9];for(uint component=0u;component<9u;++component){values[component]=
  state[component*p.cells+cell];if(!isfinite(values[component]))return false;}
 EOSDD total=eos_dd(0.0f),massScale=eos_abs(eos_dd(values[0])),closure=eos_dd(values[0]);
 for(uint species=0u;species<7u;++species){EOSDD density=eos_dd(values[species+1u]);
  total=eos_add(total,density);massScale=eos_add(massScale,eos_abs(density));closure=eos_sub(closure,density);}
 if(eos_order(eos_dd(0.0f),total)!=-1||!eos_within_positive_envelope(eos_neg(eos_dd(values[0])),
  massScale,p.feasibility))return false;
 for(uint species=0u;species<7u;++species)if(!eos_within_positive_envelope(
  eos_neg(eos_dd(values[species+1u])),massScale,p.feasibility))return false;
 if(!eos_within_positive_envelope(closure,massScale,p.feasibility))return false;
 bool valid=true;EOSDD lowerT=eos_dd(p.Tmin),upperT=eos_dd(p.Tmax),sensible=eos_dd(values[8]),
  lowerEnergy=eos_dd(0.0f),upperEnergy=eos_dd(0.0f),energyScale=eos_abs(sensible);
 for(uint species=0u;species<7u;++species){EOSDD density=eos_dd(values[species+1u]),
  lowerTerm=eos_mul(density,eos_enthalpy_dd(thermo,species,lowerT,obligations,valid)),
  upperTerm=eos_mul(density,eos_enthalpy_dd(thermo,species,upperT,obligations,valid));
  lowerEnergy=eos_add(lowerEnergy,lowerTerm);upperEnergy=eos_add(upperEnergy,upperTerm);
  energyScale=eos_add(energyScale,eos_add(eos_abs(lowerTerm),eos_abs(upperTerm)));}
 if(!valid||!eos_within_positive_envelope(eos_sub(lowerEnergy,sensible),energyScale,p.feasibility)||
  !eos_within_positive_envelope(eos_sub(sensible,upperEnergy),energyScale,p.feasibility))return false;
 uint matrixOffset=7u*96u+6u;for(uint row=0u;row<p.affineRowCount;++row){EOSDD residual=eos_dd(0.0f),
  scale=eos_dd(0.0f);for(uint column=0u;column<8u;++column){EOSDD term=eos_mul(
   eos_load_dd(thermo,matrixOffset+3u*(row*8u+column)),eos_dd(values[column]));
   residual=eos_add(residual,term);scale=eos_add(scale,eos_abs(term));}
  if(!eos_within_positive_envelope(eos_abs(residual),scale,p.feasibility))return false;}
 return true;}
inline bool eos_temperature(device const float* state,device const float* thermo,
 constant EOSParams& p,uint cell,device atomic_uint* obligations,thread float& temperature){
 EOSDD sensible=eos_dd(state[8u*p.cells+cell]),lowerT=eos_dd(p.Tmin),upperT=eos_dd(p.Tmax);bool valid=true;
 EOSDD lowerEnergy=eos_energy_dd(state,thermo,p,cell,lowerT,obligations,valid),
  upperEnergy=eos_energy_dd(state,thermo,p,cell,upperT,obligations,valid);temperature=0.0f;if(!valid)return false;
 EOSDD scale=eos_abs(sensible);for(uint species=0u;species<7u;++species){EOSDD density=eos_dd(
  state[(species+1u)*p.cells+cell]),lowerTerm=eos_mul(density,eos_enthalpy_dd(thermo,species,
   lowerT,obligations,valid)),upperTerm=eos_mul(density,eos_enthalpy_dd(thermo,species,
   upperT,obligations,valid));scale=eos_add(scale,eos_add(eos_abs(lowerTerm),eos_abs(upperTerm)));}
 if(!valid)return false;int scaleOrder=eos_order(scale,eos_dd(1.0f));
 if(scaleOrder==-1||scaleOrder==2)scale=eos_dd(1.0f);
 EOSDD tolerance=eos_mul(eos_dd(p.feasibility),scale);
 int lowerOrder=eos_order(eos_add(lowerEnergy,tolerance),sensible);
 if(lowerOrder==2)return false;if(lowerOrder>=0){temperature=p.Tmin;
  atomic_fetch_or_explicit(obligations,1u<<3u,memory_order_relaxed);}
 else{int upperOrder=eos_order(sensible,eos_sub(upperEnergy,tolerance));
 if(upperOrder==2||upperOrder>=0)return false;
 atomic_fetch_or_explicit(obligations,1u<<4u,memory_order_relaxed);
 uint lowerBits=as_type<uint>(p.Tmin),upperBits=as_type<uint>(p.Tmax);
 while(upperBits-lowerBits>1u){uint midpointBits=lowerBits+(upperBits-lowerBits)/2u;
  EOSDD energy=eos_energy_dd(state,thermo,p,cell,eos_dd(as_type<float>(midpointBits)),obligations,valid);
  if(!valid)return false;int order=eos_order(energy,sensible);if(order==2)return false;
  if(order<0)lowerBits=midpointBits;else upperBits=midpointBits;}
 float lower=as_type<float>(lowerBits),upper=as_type<float>(upperBits);
 EOSDD midpoint=eos_add(eos_dd(lower),eos_dd((upper-lower)*0.5f));
 EOSDD midpointEnergy=eos_energy_dd(state,thermo,p,cell,midpoint,obligations,valid);if(!valid)return false;
 int midpointOrder=eos_order(midpointEnergy,sensible);if(midpointOrder==2)return false;
 if(midpointOrder<0)temperature=upper;
 else if(midpointOrder>0)temperature=lower;
 else temperature=(lowerBits&1u)==0u?lower:upper;}
 return isfinite(temperature)&&temperature>=p.Tmin&&temperature<p.Tmax;}
kernel void evaluate_resident_eos_candidate(device const float* candidate [[buffer(0)]],
 device const float* thermo [[buffer(1)]],device const float* eosThermo [[buffer(2)]],
 device const ulong* candidateIdentity [[buffer(3)]],device float* temperature [[buffer(4)]],
 device float* pressureRatio [[buffer(5)]],device float* deviation [[buffer(6)]],
 device atomic_uint* failure [[buffer(7)]],device atomic_uint* obligations [[buffer(8)]],
 constant EOSParams& p [[buffer(9)]],device atomic_uint* firstFailureCell [[buffer(10)]],
 device uint* failureTerm [[buffer(11)]],
 uint gid [[thread_position_in_grid]]){if(gid>=p.cells)return;
 if(candidateIdentity[0]==0ul||!eos_state_admissible(candidate,eosThermo,p,gid,obligations)){
  atomic_fetch_or_explicit(failure,64u,memory_order_relaxed);return;}
 atomic_fetch_or_explicit(obligations,1u<<2u,memory_order_relaxed);float T=0.0f;
 if(!eos_temperature(candidate,eosThermo,p,gid,obligations,T)){
  atomic_fetch_or_explicit(failure,128u,memory_order_relaxed);return;}
 EOSDD gas=eos_dd(0.0f),molar=eos_dd(0.0f);for(uint species=0u;species<6u;++species){float density=max(0.0f,
  candidate[(species+1u)*p.cells+gid]);gas=eos_add(gas,eos_dd(density));molar=eos_add(molar,
   eos_div(eos_dd(density),eos_load_dd(eosThermo,96u*species)));}
 if(eos_order(eos_dd(0.0f),gas)!=-1||eos_order(eos_dd(0.0f),molar)!=-1){
  atomic_fetch_or_explicit(failure,256u,memory_order_relaxed);return;}
 EOSDD meanWeight=eos_div(gas,molar),represented=eos_div(eos_mul(eos_mul(gas,
  eos_load_dd(eosThermo,7u*96u)),eos_dd(T)),meanWeight);
 EOSDD ratioDD=eos_div(represented,eos_load_dd(eosThermo,7u*96u+3u));
 EOSDD deviationDD=eos_abs(eos_sub(ratioDD,eos_dd(1.0f)));
 if((p.pad0&1u)!=0u){float adjacent=nextafter(1.0f,INFINITY);
  ratioDD=eos_add(eos_dd(1.0f),eos_dd((adjacent-1.0f)*0.5f));}
 if((p.pad0&2u)!=0u){float adjacent=nextafter(0.125f,INFINITY);
  deviationDD=eos_add(eos_dd(0.125f),eos_dd((adjacent-0.125f)*0.5f));}
 if((p.pad0&4u)!=0u)deviationDD=eos_dd(0.0f);
 if((p.pad0&8u)!=0u)deviationDD=eos_dd(as_type<float>(1u));
 if((p.pad0&16u)!=0u)deviationDD=eos_dd(0.0f,0.0f,0.0f,as_type<float>(1u));
 if((p.pad0&32u)!=0u)deviationDD=eos_dd(as_type<float>(1u),0.0f,0.0f,
  as_type<float>(1u));
 if((p.pad0&64u)!=0u){ratioDD=eos_dd(1.0625f);deviationDD=eos_dd(0.0625f);}
 if((p.pad0&128u)!=0u){ratioDD=eos_dd(0.9375f);deviationDD=eos_dd(0.0625f);}
 if((p.pad1&1u)!=0u&&gid==0u){float adjacent=nextafter(1.0f,INFINITY);
  ratioDD=eos_add(eos_dd(1.0f),eos_dd((adjacent-1.0f)*0.5f));}
 if((p.pad1&1u)!=0u&&gid==1u){float adjacent=nextafter(0.125f,INFINITY);
  deviationDD=eos_add(eos_dd(0.125f),eos_dd((adjacent-0.125f)*0.5f));}
 float ratio=0.0f,absoluteDeviation=0.0f;
 bool ratioRounded=eos_unique_binary32_round(ratioDD,ratio);
 // The represented ratio is the monitored-policy authority.  Derive its
 // diagnostic magnitude from that published binary32 value so the diagnostic
 // cannot become a second, independently rounded absolute-reference source.
 // Qualification mutations still exercise and refuse the retired independent
 // deviation-rounding path.
 bool independentDeviation=(p.pad0&254u)!=0u||((p.pad1&1u)!=0u&&gid<2u);
 bool deviationRounded=independentDeviation?
  eos_unique_binary32_round(deviationDD,absoluteDeviation):ratioRounded;
 if(!independentDeviation&&ratioRounded)absoluteDeviation=abs(ratio-1.0f);
 bool ratioPhysical=isfinite(ratio)&&ratio>0.0f;
 bool deviationPhysical=isfinite(absoluteDeviation);
 if(!ratioRounded||!deviationRounded||!ratioPhysical||!deviationPhysical){uint terms=
  (!ratioRounded?1u:0u)|(!deviationRounded?2u:0u)|(!ratioPhysical?4u:0u)|
  (!deviationPhysical?8u:0u);failureTerm[gid]=terms;
  atomic_fetch_min_explicit(firstFailureCell,gid,memory_order_relaxed);
  atomic_fetch_or_explicit(failure,512u,memory_order_relaxed);return;}
 temperature[gid]=T;pressureRatio[gid]=ratio;deviation[gid]=absoluteDeviation;
 atomic_fetch_or_explicit(obligations,1u<<5u,memory_order_relaxed);
 if(!eos_proved_leq(deviationDD,eos_dd(p.dynamicsBound)))
  atomic_fetch_or_explicit(failure,1024u,memory_order_relaxed);
 else atomic_fetch_or_explicit(obligations,1u<<6u,memory_order_relaxed);}
kernel void identify_resident_eos(device const float* temperature [[buffer(0)]],
 device const float* pressureRatio [[buffer(1)]],device const float* deviation [[buffer(2)]],
 device const ulong* candidateIdentity [[buffer(3)]],device const ulong* physicalIdentity [[buffer(4)]],
 device const ulong* transportIdentity [[buffer(5)]],device const float* eosThermo [[buffer(6)]],
 device ulong* identity [[buffer(7)]],device atomic_uint* failure [[buffer(8)]],constant EOSParams& p [[buffer(9)]],
 uint gid [[thread_position_in_grid]]){if(gid!=0u)return;
 if(atomic_load_explicit(failure,memory_order_relaxed)!=0u||candidateIdentity[0]==0ul||
  physicalIdentity[0]==0ul||transportIdentity[0]==0ul){identity[0]=0ul;return;}
 if(!resident_full_payload_seals){identity[0]=resident_stage_token(4u,
  ulong4(candidateIdentity[0],physicalIdentity[0],transportIdentity[0],p.attempt),
  ulong4(p.caseIdentity,p.stage,p.precision,as_type<uint>(p.dynamicsBound)));return;}
 ulong hash=14695981039346656037ul;hash^=transportIdentity[0];hash*=1099511628211ul;
 hash^=physicalIdentity[0];hash*=1099511628211ul;hash^=candidateIdentity[0];hash*=1099511628211ul;
 for(uint cell=0u;cell<p.cells;++cell){hash^=ulong(as_type<uint>(temperature[cell]));hash*=1099511628211ul;
  hash^=ulong(as_type<uint>(pressureRatio[cell]));hash*=1099511628211ul;
 hash^=ulong(as_type<uint>(deviation[cell]));hash*=1099511628211ul;}
 for(uint word=0u;word<870u;++word){hash^=ulong(as_type<uint>(eosThermo[word]));hash*=1099511628211ul;}
 hash^=p.attempt;hash*=1099511628211ul;hash^=p.caseIdentity;hash*=1099511628211ul;
 hash^=ulong(p.stage);hash*=1099511628211ul;hash^=ulong(p.precision);hash*=1099511628211ul;
 hash^=ulong(as_type<uint>(p.dynamicsBound));hash*=1099511628211ul;identity[0]=hash==0ul?1ul:hash;}
kernel void produce_resident_frozen_source(device const float* input [[buffer(0)]],
 device float* output [[buffer(1)]],device FrozenSourceParams* sealed [[buffer(2)]],
 device atomic_uint* failure [[buffer(3)]],constant TargetParams& p [[buffer(4)]],
 device const ulong* candidateIdentity [[buffer(5)]],uint gid [[thread_position_in_grid]]){
 if(gid>=p.cells)return;
 if(p.cells==0u||p.attempt==0ul||p.sourcePacket==0ul||candidateIdentity[0]==0ul||!isfinite(input[gid])||
  (input[gid]==0.0f&&signbit(input[gid]))){atomic_fetch_or_explicit(failure,2048u,
   memory_order_relaxed);return;}output[gid]=input[gid];if(gid==0u){FrozenSourceParams value;
  value.cells=p.cells;value.pad0=0u;value.pad1=0u;value.pad2=0u;value.attempt=p.attempt;
  value.sourcePacket=p.sourcePacket;sealed[0]=value;}}
kernel void identify_resident_frozen_source(device const float* source [[buffer(0)]],
 device const FrozenSourceParams* sealed [[buffer(1)]],device ulong* identity [[buffer(2)]],
 device atomic_uint* failure [[buffer(3)]],device const ulong* candidateIdentity [[buffer(4)]],
 uint gid [[thread_position_in_grid]]){if(gid!=0u)return;
 if(atomic_load_explicit(failure,memory_order_relaxed)!=0u||sealed[0].cells==0u||
  sealed[0].attempt==0ul||sealed[0].sourcePacket==0ul||candidateIdentity[0]==0ul){
  identity[0]=0ul;return;}
 if(!resident_full_payload_seals){identity[0]=resident_stage_token(5u,
  ulong4(sealed[0].cells,sealed[0].attempt,sealed[0].sourcePacket,candidateIdentity[0]),ulong4(0ul));return;}
 ulong hash=14695981039346656037ul;hash^=ulong(sealed[0].cells);hash*=1099511628211ul;
 hash^=sealed[0].attempt;hash*=1099511628211ul;hash^=sealed[0].sourcePacket;
 hash*=1099511628211ul;hash^=candidateIdentity[0];hash*=1099511628211ul;
 for(uint cell=0u;cell<sealed[0].cells;++cell){
  hash^=ulong(as_type<uint>(source[cell]));hash*=1099511628211ul;}
 identity[0]=hash==0ul?1ul:hash;}
inline EOSDD target_cp_dd(device const float* thermo,uint species,EOSDD temperature,
 device atomic_uint* obligations,thread bool& valid){uint offset=0u;
 if(!eos_select_dd_segment(thermo,species,temperature,offset,obligations,valid)){
  valid=false;return eos_dd(0.0f);}EOSDD inverse=eos_div(eos_dd(1.0f),temperature),
  inverse2=eos_mul(inverse,inverse),t2=eos_mul(temperature,temperature),
  t3=eos_mul(t2,temperature),t4=eos_mul(t3,temperature);
 EOSDD value=eos_mul(eos_load_dd(thermo,offset+6u),inverse2);
 value=eos_add(value,eos_mul(eos_load_dd(thermo,offset+9u),inverse));
 value=eos_add(value,eos_load_dd(thermo,offset+12u));
 value=eos_add(value,eos_mul(eos_load_dd(thermo,offset+15u),temperature));
 value=eos_add(value,eos_mul(eos_load_dd(thermo,offset+18u),t2));
 value=eos_add(value,eos_mul(eos_load_dd(thermo,offset+21u),t3));
 value=eos_add(value,eos_mul(eos_load_dd(thermo,offset+24u),t4));
 return eos_mul(eos_div(eos_load_dd(thermo,7u*96u),
  eos_load_dd(thermo,96u*species)),value);}
kernel void evaluate_resident_target_terms(device const float* state [[buffer(0)]],
 device const float* temperature [[buffer(1)]],device const float* physicalMass [[buffer(2)]],
 device const float* physicalEnergy [[buffer(3)]],device const float* eosThermo [[buffer(4)]],
 device const float* sourceTarget [[buffer(5)]],device const float* pressureRatio [[buffer(6)]],
 device const float* absoluteDeviation [[buffer(7)]],
 device const ulong* transportIdentity [[buffer(8)]],device const ulong* tangentPhysicalIdentity [[buffer(9)]],
 device const ulong* candidateIdentity [[buffer(10)]],device const ulong* eosIdentity [[buffer(11)]],
 device float* tangent [[buffer(12)]],device float* source [[buffer(13)]],
 device float* absoluteDiagnostic [[buffer(14)]],device float* monitoredAbsolute [[buffer(15)]],
 device float* assembled [[buffer(16)]],device atomic_uint* failure [[buffer(17)]],
 device atomic_uint* obligations [[buffer(18)]],constant TargetParams& p [[buffer(19)]],
	device const ulong* sourceIdentity [[buffer(20)]],
	device const FrozenSourceParams* frozen [[buffer(21)]],
	constant TransportParams& transportParameters [[buffer(22)]],
	constant EOSFCTParams& fctParameters [[buffer(23)]],
	constant EOSParams& eosParameters [[buffer(24)]],
	device const ulong* candidatePhysicalIdentity [[buffer(25)]],
	device float* baseAssembled [[buffer(26)]],
	device float* tangentRadius [[buffer(27)]],device float* assembledRadius [[buffer(28)]],
	uint gid [[thread_position_in_grid]]){if(gid>=p.cells)return;bool metadataMatches=
	 p.nx==transportParameters.nx&&p.ny==transportParameters.ny&&
	 p.nz==transportParameters.nz&&p.cells==transportParameters.cells&&
	 p.nx==fctParameters.nx&&p.ny==fctParameters.ny&&p.nz==fctParameters.nz&&
	 p.cells==fctParameters.cells&&p.cells==eosParameters.cells&&
	 as_type<uint>(p.dx)==as_type<uint>(transportParameters.dx)&&
	 as_type<uint>(p.dx)==as_type<uint>(fctParameters.dx)&&
	 as_type<uint>(p.dt)==as_type<uint>(fctParameters.dt)&&
	 as_type<uint>(p.dt)==as_type<uint>(eosParameters.timeStepS)&&
	 p.attempt==transportParameters.attempt&&p.attempt==eosParameters.attempt&&
	 p.cells==frozen[0].cells&&p.attempt==frozen[0].attempt&&
	 p.sourcePacket==frozen[0].sourcePacket&&p.policyVersion==0x72313730u;
	for(uint axis=0u;axis<3u;++axis)metadataMatches=metadataMatches&&
	 p.faceOffset[axis]==transportParameters.faceOffset[axis];
	for(uint side=0u;side<6u;++side)metadataMatches=metadataMatches&&
	 p.boundary[side]==transportParameters.boundary[side]&&
	 p.boundary[side]==fctParameters.boundary[side];
 if(transportIdentity[0]==0ul||tangentPhysicalIdentity[0]==0ul||
	 candidatePhysicalIdentity[0]==0ul||candidateIdentity[0]==0ul||
	 eosIdentity[0]==0ul||sourceIdentity[0]==0ul||!metadataMatches){atomic_fetch_or_explicit(failure,2048u,
   memory_order_relaxed);return;}uint x=gid%p.nx,y=(gid/p.nx)%p.ny,z=gid/(p.nx*p.ny);
 EOSDD rate[9],divergenceNumerator[9];for(uint component=0u;component<9u;++component){
  rate[component]=eos_dd(0.0f);divergenceNumerator[component]=eos_dd(0.0f);}
 // Exact-zero walker: bit-identical opposing face terms make every component
 // of D(f_N) exactly zero.  Resolve that algebraic branch before interval
 // rounding so a conservative DD enclosure around zero cannot manufacture an
 // ambiguity.  This is a device proof branch, not a tolerance or host fallback.
 float divergenceTerms[9][6];
 for(uint axis=0u;axis<3u;++axis){uint rx=x+(axis==0u),ry=y+(axis==1u),rz=z+(axis==2u);
  uint left=target_face(p,axis,x,y,z);uint right=target_face(p,axis,rx,ry,rz);
  for(uint component=0u;component<8u;++component){float leftMass=
   physicalMass[component*(p.faceOffset[2]+p.nx*p.ny*(p.nz+1u))+left],rightMass=
   physicalMass[component*(p.faceOffset[2]+p.nx*p.ny*(p.nz+1u))+right];
   divergenceTerms[component][2u*axis]=leftMass;
   divergenceTerms[component][2u*axis+1u]=-rightMass;
   divergenceNumerator[component]=eos_add(divergenceNumerator[component],
    eos_sub(eos_dd(leftMass),eos_dd(rightMass)));}
  float leftEnergy=physicalEnergy[left],rightEnergy=physicalEnergy[right];
  divergenceTerms[8u][2u*axis]=leftEnergy;
  divergenceTerms[8u][2u*axis+1u]=-rightEnergy;
  divergenceNumerator[8u]=eos_add(divergenceNumerator[8u],
   eos_sub(eos_dd(leftEnergy),eos_dd(rightEnergy)));}
 bool divergenceBitZero=true;for(uint component=0u;component<9u;++component)
  divergenceBitZero=divergenceBitZero&&eos_six_term_sum_is_exact_zero(divergenceTerms[component]);
 for(uint component=0u;component<9u;++component)
  rate[component]=eos_div(divergenceNumerator[component],eos_dd(p.dx));
 bool valid=true;EOSDD gas=eos_dd(0.0f),inverseWeight=eos_dd(0.0f),heatCapacity=eos_dd(0.0f);
 EOSDD enthalpy[7];EOSDD T=eos_dd(temperature[gid]);for(uint species=0u;species<7u;++species){
  EOSDD density=eos_dd(max(0.0f,state[(species+1u)*p.cells+gid]));
  enthalpy[species]=eos_enthalpy_dd(eosThermo,species,T,obligations,valid);
  heatCapacity=eos_add(heatCapacity,eos_mul(density,
   target_cp_dd(eosThermo,species,T,obligations,valid)));
  if(species<6u){gas=eos_add(gas,density);inverseWeight=eos_add(inverseWeight,
    eos_div(density,eos_load_dd(eosThermo,96u*species)));}}
 EOSDD capacityTemperature=eos_mul(heatCapacity,T),meanWeight=eos_div(gas,inverseWeight);
 EOSDD value=eos_div(rate[8u],capacityTemperature);
 for(uint species=0u;species<6u;++species){EOSDD coefficient=eos_sub(eos_div(meanWeight,
   eos_mul(gas,eos_load_dd(eosThermo,96u*species))),eos_div(enthalpy[species],capacityTemperature));
  value=eos_add(value,eos_mul(coefficient,rate[species+1u]));}
 value=eos_sub(value,eos_mul(eos_div(enthalpy[6u],capacityTemperature),rate[7u]));
 // Qualification-only: exercise the continuous enclosure obligation with a
 // finite radius wider than one local binary32 spacing.  The center and every
 // physical input remain unchanged; this flag is unavailable to the live owner.
 if((p.pad0&1u)!=0u)value.bound=eos_up_add(value.bound,
  eos_up_mul(4.0f,eos_local_spacing(value.hi)));
 EOSDD signedDeviation=eos_sub(eos_dd(pressureRatio[gid]),eos_dd(1.0f));
 // The resident policy consumes the represented pressure ratio exactly like
 // FireProductionMonitoredManifoldPolicy; the separately published absolute
 // deviation remains diagnostic and may not become a second policy source.
 EOSDD magnitude=eos_abs(signedDeviation);
 EOSDD tail=eos_dd(0.0f);int beyond=eos_order(magnitude,eos_dd(p.tailThreshold));
 if(beyond==2){atomic_fetch_or_explicit(failure,16384u,memory_order_relaxed);return;}
 if(beyond>0){EOSDD excess=eos_sub(magnitude,eos_dd(p.tailThreshold));
  tail=eos_div(excess,eos_dd(p.dt));if(eos_order(signedDeviation,eos_dd(0.0f))>0){tail=eos_neg(tail);
   atomic_fetch_or_explicit(obligations,1u<<21u,memory_order_relaxed);}
  else atomic_fetch_or_explicit(obligations,1u<<25u,memory_order_relaxed);}
 else atomic_fetch_or_explicit(obligations,1u<<20u,memory_order_relaxed);
 float tangentValue=0.0f,tangentBound=0.0f,tailValue=0.0f;
 float diagnosticValue=(pressureRatio[gid]-1.0f)/p.dt;
 bool tangentRounded=divergenceBitZero,tangentEnclosed=false;
 if(divergenceBitZero){tangentValue=0.0f;
  tangentBound=0.0f;
  atomic_fetch_or_explicit(obligations,1u<<26u,memory_order_relaxed);}
 else tangentRounded=eos_binary32_round_or_local_enclosure(value,tangentValue,tangentBound,
  tangentEnclosed);
 if(tangentEnclosed)atomic_fetch_or_explicit(obligations,1u<<27u,memory_order_relaxed);
 bool tailRounded=eos_unique_binary32_round(tail,tailValue);
 if(!valid)atomic_fetch_or_explicit(failure,32768u,memory_order_relaxed);
 if(!tangentRounded){atomic_fetch_or_explicit(failure,65536u,memory_order_relaxed);
  tangent[gid]=NAN;source[gid]=value.hi;absoluteDiagnostic[gid]=value.lo;
  monitoredAbsolute[gid]=value.bound;}
 if(!tailRounded)atomic_fetch_or_explicit(failure,262144u,memory_order_relaxed);
 if(!valid||!tangentRounded||!isfinite(diagnosticValue)||!tailRounded||
  !isfinite(sourceTarget[gid]))return;
 tangentRadius[gid]=tangentBound;
 // Qualification-only publication corruption.  The independently carried
 // radius must make this visible to the fp64 consumer-side RED.
 if((p.pad1&1u)!=0u)for(uint step=0u;step<8u;++step)
  tangentValue=nextafter(tangentValue,INFINITY);
 tangent[gid]=tangentValue;source[gid]=sourceTarget[gid];
 absoluteDiagnostic[gid]=diagnosticValue;monitoredAbsolute[gid]=tailValue;
 EOSDD baseDD=eos_add(value,eos_dd(sourceTarget[gid]));float baseValue=0.0f;
 bool baseEnclosed=false;float baseRadius=0.0f;
 if(!eos_binary32_round_or_local_enclosure(baseDD,baseValue,baseRadius,baseEnclosed)||!isfinite(baseValue)){
  atomic_fetch_or_explicit(failure,1048576u,memory_order_relaxed);return;}
 if(baseEnclosed)atomic_fetch_or_explicit(obligations,1u<<28u,memory_order_relaxed);
 baseAssembled[gid]=baseValue==0.0f?0.0f:baseValue;
 EOSDD combinedDD=eos_add(baseDD,tail);float combined=0.0f;
 bool combinedEnclosed=false;float combinedRadius=0.0f;
 if(!eos_binary32_round_or_local_enclosure(combinedDD,combined,combinedRadius,combinedEnclosed)||
  !isfinite(combined)){
  atomic_fetch_or_explicit(failure,1048576u,memory_order_relaxed);return;}assembled[gid]=combined;
 assembledRadius[gid]=combinedRadius;
 if(combinedEnclosed)atomic_fetch_or_explicit(obligations,1u<<29u,memory_order_relaxed);
 atomic_fetch_or_explicit(obligations,1u<<22u,memory_order_relaxed);}
kernel void finalize_resident_target(device float* target [[buffer(0)]],
 device atomic_uint* failure [[buffer(1)]],device atomic_uint* obligations [[buffer(2)]],
 constant TargetParams& p [[buffer(3)]],device float* radius [[buffer(4)]],
 uint gid [[thread_position_in_grid]]){if(gid!=0u||
 atomic_load_explicit(failure,memory_order_relaxed)!=0u)return;bool pressureOpen=false;
 for(uint side=0u;side<6u;++side)pressureOpen=pressureOpen||p.boundary[side]==1u;
 if(pressureOpen){atomic_fetch_or_explicit(obligations,1u<<23u,memory_order_relaxed);return;}
 EOSDD sum=eos_dd(0.0f);float radiusSum=0.0f;for(uint cell=0u;cell<p.cells;++cell){
  sum=eos_add(sum,eos_dd(target[cell]));radiusSum=eos_up_add(radiusSum,radius[cell]);}
 EOSDD mean=eos_div(sum,eos_dd(float(p.cells)));
 mean.bound=eos_up_add(mean.bound,eos_up_div(radiusSum,float(p.cells)));bool anyEnclosed=false;
 for(uint cell=0u;cell<p.cells;++cell){float value=0.0f;
  EOSDD center=eos_dd(target[cell]);center.bound=radius[cell];float localRadius=0.0f;
  bool enclosed=false;if(!eos_binary32_round_or_local_enclosure(
   eos_sub(center,mean),value,localRadius,enclosed)){
   atomic_fetch_or_explicit(failure,4096u,memory_order_relaxed);return;}
  anyEnclosed=anyEnclosed||enclosed;target[cell]=value==0.0f?0.0f:value;
  radius[cell]=localRadius;}
 // One device-wide obligation is sufficient: the target identity authenticates
 // every materialized cell, and qualification discharges each local enclosure.
 if(anyEnclosed)atomic_fetch_or_explicit(obligations,1u<<30u,memory_order_relaxed);
 atomic_fetch_or_explicit(obligations,1u<<24u,memory_order_relaxed);}
kernel void identify_resident_target(device const float* tangent [[buffer(0)]],
 device const float* source [[buffer(1)]],device const float* absoluteDiagnostic [[buffer(2)]],
 device const float* monitoredAbsolute [[buffer(3)]],device const float* assembled [[buffer(4)]],
 device const ulong* transportIdentity [[buffer(5)]],device const ulong* tangentPhysicalIdentity [[buffer(6)]],
 device const ulong* candidateIdentity [[buffer(7)]],device const ulong* eosIdentity [[buffer(8)]],
 device ulong* identity [[buffer(9)]],device atomic_uint* failure [[buffer(10)]],
 device const ulong* sourceIdentity [[buffer(11)]],constant TargetParams& p [[buffer(12)]],
	device const ulong* candidatePhysicalIdentity [[buffer(13)]],
	device const float* tangentRadius [[buffer(14)]],device const float* assembledRadius [[buffer(15)]],
 uint gid [[thread_position_in_grid]]){if(gid!=0u)return;
 if(atomic_load_explicit(failure,memory_order_relaxed)!=0u||transportIdentity[0]==0ul||
  tangentPhysicalIdentity[0]==0ul||candidatePhysicalIdentity[0]==0ul||
	 candidateIdentity[0]==0ul||eosIdentity[0]==0ul||
  sourceIdentity[0]==0ul){identity[0]=0ul;return;}
 if(!resident_full_payload_seals){identity[0]=resident_stage_token(6u,
  ulong4(transportIdentity[0],tangentPhysicalIdentity[0],candidatePhysicalIdentity[0],candidateIdentity[0]),
  ulong4(eosIdentity[0],sourceIdentity[0],p.attempt,p.policyVersion));return;}
 ulong hash=14695981039346656037ul;hash^=transportIdentity[0];hash*=1099511628211ul;
 hash^=tangentPhysicalIdentity[0];hash*=1099511628211ul;
	 hash^=candidatePhysicalIdentity[0];hash*=1099511628211ul;
	 hash^=candidateIdentity[0];hash*=1099511628211ul;
 hash^=eosIdentity[0];hash*=1099511628211ul;hash^=sourceIdentity[0];hash*=1099511628211ul;
 for(uint cell=0u;cell<p.cells;++cell){hash^=ulong(as_type<uint>(tangent[cell]));hash*=1099511628211ul;
  hash^=ulong(as_type<uint>(source[cell]));hash*=1099511628211ul;
  hash^=ulong(as_type<uint>(absoluteDiagnostic[cell]));hash*=1099511628211ul;
  hash^=ulong(as_type<uint>(monitoredAbsolute[cell]));hash*=1099511628211ul;
  hash^=ulong(as_type<uint>(assembled[cell]));hash*=1099511628211ul;
  hash^=ulong(as_type<uint>(tangentRadius[cell]));hash*=1099511628211ul;
  hash^=ulong(as_type<uint>(assembledRadius[cell]));hash*=1099511628211ul;}
 hash^=ulong(p.nx);hash*=1099511628211ul;hash^=ulong(p.ny);hash*=1099511628211ul;
 hash^=ulong(p.nz);hash*=1099511628211ul;hash^=ulong(p.cells);hash*=1099511628211ul;
 for(uint axis=0u;axis<3u;++axis){hash^=ulong(p.faceOffset[axis]);hash*=1099511628211ul;}
 hash^=p.attempt;hash*=1099511628211ul;hash^=ulong(as_type<uint>(p.dt));hash*=1099511628211ul;
 hash^=ulong(as_type<uint>(p.dx));hash*=1099511628211ul;
	 hash^=ulong(as_type<uint>(p.tailThreshold));hash*=1099511628211ul;
	 hash^=ulong(p.policyVersion);hash*=1099511628211ul;
	 hash^=ulong(p.pad0);hash*=1099511628211ul;hash^=ulong(p.pad1);hash*=1099511628211ul;
 for(uint side=0u;side<6u;++side){
  hash^=ulong(p.boundary[side]);hash*=1099511628211ul;}identity[0]=hash==0ul?1ul:hash;}
kernel void issue_resident_projection_consumer(constant ProjectionConsumerParams& p [[buffer(0)]],
 device const ulong* targetIdentity [[buffer(1)]],device ProjectionConsumerParams* sealed [[buffer(2)]],
 device ulong* identity [[buffer(3)]],device atomic_uint* failure [[buffer(4)]],
 uint gid [[thread_position_in_grid]]){if(gid!=0u)return;
 if(targetIdentity[0]==0ul||p.nx<4u||p.ny<4u||p.nz<4u||p.cells!=p.nx*p.ny*p.nz||!isfinite(p.dx)||!(p.dx>0.0f)||
  !isfinite(p.dt)||!(p.dt>0.0f)||p.attempt==0ul){identity[0]=0ul;
  atomic_fetch_or_explicit(failure,8192u,memory_order_relaxed);return;}
 sealed[0]=p;ulong hash=14695981039346656037ul;hash^=targetIdentity[0];hash*=1099511628211ul;
 hash^=ulong(p.nx);hash*=1099511628211ul;
 hash^=ulong(p.ny);hash*=1099511628211ul;hash^=ulong(p.nz);hash*=1099511628211ul;
 hash^=ulong(p.cells);hash*=1099511628211ul;hash^=ulong(as_type<uint>(p.dx));hash*=1099511628211ul;
 hash^=ulong(as_type<uint>(p.dt));hash*=1099511628211ul;hash^=p.attempt;hash*=1099511628211ul;
 for(uint side=0u;side<6u;++side){if(p.boundary[side]>2u){identity[0]=0ul;
   atomic_fetch_or_explicit(failure,8192u,memory_order_relaxed);return;}
  hash^=ulong(p.boundary[side]);hash*=1099511628211ul;}identity[0]=hash==0ul?1ul:hash;}
kernel void consume_resident_target(device const float* target [[buffer(0)]],
 device const ulong* targetIdentity [[buffer(1)]],device const ulong* transportIdentity [[buffer(2)]],
 device const ulong* physicalIdentity [[buffer(3)]],device const ulong* candidateIdentity [[buffer(4)]],
 device const ulong* eosIdentity [[buffer(5)]],device ulong* consumerIdentity [[buffer(6)]],
 device const ulong* projectionIdentity [[buffer(7)]],device atomic_uint* failure [[buffer(8)]],
 constant TargetParams& p [[buffer(9)]],constant ProjectionConsumerParams& projection [[buffer(10)]],
 uint gid [[thread_position_in_grid]]){if(gid!=0u)return;bool metadataMatches=
 p.nx==projection.nx&&p.ny==projection.ny&&p.nz==projection.nz&&p.cells==projection.cells&&
 as_type<uint>(p.dx)==as_type<uint>(projection.dx)&&as_type<uint>(p.dt)==as_type<uint>(projection.dt)&&
 p.attempt==projection.attempt;for(uint side=0u;side<6u;++side)
  metadataMatches=metadataMatches&&p.boundary[side]==projection.boundary[side];
 if(p.preauthored!=0u||!metadataMatches||projectionIdentity[0]==0ul||
 targetIdentity[0]==0ul||transportIdentity[0]==0ul||physicalIdentity[0]==0ul||
 candidateIdentity[0]==0ul||eosIdentity[0]==0ul){consumerIdentity[0]=0ul;
 atomic_fetch_or_explicit(failure,8192u,memory_order_relaxed);return;}ulong hash=targetIdentity[0];
 hash^=projectionIdentity[0];hash*=1099511628211ul;
 if(resident_full_payload_seals)for(uint cell=0u;cell<p.cells;++cell){hash^=ulong(as_type<uint>(target[cell]));hash*=1099511628211ul;}
 consumerIdentity[0]=hash==0ul?1ul:hash;}
kernel void diagnose_eos_log_enclosure(device const float* input [[buffer(0)]],
 device float* output [[buffer(1)]],uint gid [[thread_position_in_grid]]){
 EOSDD value=eos_log_dd(eos_dd(input[2u*gid],input[2u*gid+1u]));output[4u*gid]=value.hi;
 output[4u*gid+1u]=value.lo;output[4u*gid+2u]=value.tail;
 // The host image and OS build are sealed qualification identities.  Add the
 // predeclared binary64 projection allowance only on this comparison surface.
 float hostProjectionAllowance=eos_up_mul(0x1p-52f,max(1.0f,eos_abs_upper(value)));
 output[4u*gid+3u]=eos_up_add(value.bound,hostProjectionAllowance);}
// r201 live-owner glue. These kernels only bind qualified resident surfaces,
// compose the Heun operands, and apply the stage algebra. No physical term is
// recomputed here.
struct OwnerParams {uint cells;uint allFaces;uint faceOffset[3];uint stage;
 float dt;float dx;float endpointTolerance;uint forceActiveCycle;uint threeQuarterHeunWeighting;
 uint identityPadding;
 ulong attempt;};
kernel void owner_issue_bootstrap_target(device const float* target [[buffer(0)]],
 device const float* state [[buffer(1)]],device const float* source [[buffer(2)]],
 device ulong* targetIdentity [[buffer(3)]],device ulong* rootCandidateIdentity [[buffer(4)]],
 device ulong* targetConsumerIdentity [[buffer(5)]],constant OwnerParams& p [[buffer(6)]],
 device const uchar* inputRoot [[buffer(7)]],
 uint gid [[thread_position_in_grid]]){if(gid!=0u)return;ulong hash=14695981039346656037ul;
 if(!resident_full_payload_seals){for(uint i=0u;i<32u;++i){hash^=ulong(inputRoot[i]);hash*=1099511628211ul;}}
 if(resident_full_payload_seals)for(uint cell=0u;cell<p.cells;++cell){hash^=ulong(as_type<uint>(target[cell]));hash*=1099511628211ul;}
 if(resident_full_payload_seals)for(uint word=0u;word<9u*p.cells;++word){hash^=ulong(as_type<uint>(state[word]));hash*=1099511628211ul;
  hash^=ulong(as_type<uint>(source[word]));hash*=1099511628211ul;}
 hash^=p.attempt;hash*=1099511628211ul;hash^=ulong(p.stage);hash*=1099511628211ul;
 targetIdentity[0]=hash==0ul?1ul:hash;hash^=0x726f6f745f71306eul;hash*=1099511628211ul;
 rootCandidateIdentity[0]=hash==0ul?1ul:hash;hash^=0x6273745f63617031ul;
 hash*=1099511628211ul;targetConsumerIdentity[0]=hash==0ul?1ul:hash;}
kernel void owner_pack_faces(device const float* x [[buffer(0)]],device const float* y [[buffer(1)]],
 device const float* z [[buffer(2)]],device float* packed [[buffer(3)]],
 constant OwnerParams& p [[buffer(4)]],uint gid [[thread_position_in_grid]]){
 if(gid>=p.allFaces)return;uint axis=gid>=p.faceOffset[2]?2u:(gid>=p.faceOffset[1]?1u:0u);
 uint local=gid-p.faceOffset[axis];packed[gid]=axis==0u?x[local]:(axis==1u?y[local]:z[local]);}
kernel void owner_average_field(device const float* first [[buffer(0)]],
 device const float* second [[buffer(1)]],device float* output [[buffer(2)]],
 constant uint& count [[buffer(3)]],constant float& scale [[buffer(4)]],
 uint gid [[thread_position_in_grid]]){if(gid<count)output[gid]=scale*(first[gid]+second[gid]);}
kernel void owner_gas_source(device const float* sourceDelta [[buffer(0)]],
 device float* rate [[buffer(1)]],constant OwnerParams& p [[buffer(2)]],
 uint gid [[thread_position_in_grid]]){if(gid>=p.cells)return;float value=sourceDelta[p.cells+gid];
 for(uint component=2u;component<=6u;++component)value+=sourceDelta[component*p.cells+gid];
 rate[gid]=value/p.dt;}
kernel void owner_predict_momentum(device const float* beginning [[buffer(0)]],
 device const float* nonpressure [[buffer(1)]],device const float* advection [[buffer(2)]],
 device float* output [[buffer(3)]],device atomic_uint* failure [[buffer(4)]],
 constant OwnerParams& p [[buffer(5)]],uint gid [[thread_position_in_grid]]){
 if(gid>=p.allFaces)return;float value=beginning[gid]+p.dt*(nonpressure[gid]-advection[gid]);
 if(!isfinite(value))atomic_fetch_or_explicit(failure,1u<<20u,memory_order_relaxed);output[gid]=value;}
kernel void owner_heun_momentum(device const float* beginning [[buffer(0)]],
 device const float* nonpressure0 [[buffer(1)]],device const float* nonpressure1 [[buffer(2)]],
 device const float* advection0 [[buffer(3)]],device const float* advection1 [[buffer(4)]],
 device float* output [[buffer(5)]],device atomic_uint* failure [[buffer(6)]],
 constant OwnerParams& p [[buffer(7)]],uint gid [[thread_position_in_grid]]){
 if(gid>=p.allFaces)return;float rate0=nonpressure0[gid]-advection0[gid];
 float rate1=nonpressure1[gid]-advection1[gid];
 float value=beginning[gid]+p.dt*(p.threeQuarterHeunWeighting!=0u?
  (0.75f*rate0+0.25f*rate1):0.5f*(rate0+rate1));
 if(!isfinite(value))atomic_fetch_or_explicit(failure,1u<<21u,memory_order_relaxed);output[gid]=value;}
kernel void owner_bind_transport(device const ulong* candidate [[buffer(0)]],
 device const ulong* projection [[buffer(1)]],device ulong* transport [[buffer(2)]],
 device atomic_uint* failure [[buffer(3)]],constant OwnerParams& p [[buffer(4)]],
 uint gid [[thread_position_in_grid]]){if(gid!=0u)return;if(candidate[0]==0ul||projection[0]==0ul||
 transport[0]==0ul||p.attempt==0ul||p.stage>2u){transport[0]=0ul;
 atomic_fetch_or_explicit(failure,1u<<22u,memory_order_relaxed);return;}ulong hash=transport[0];
 hash^=candidate[0];hash*=1099511628211ul;hash^=projection[0];hash*=1099511628211ul;
 hash^=p.attempt;hash*=1099511628211ul;hash^=ulong(p.stage);hash*=1099511628211ul;
 transport[0]=hash==0ul?1ul:hash;}
kernel void owner_average_flux(device const float* low0 [[buffer(0)]],
 device const float* delta0 [[buffer(1)]],device const float* low1 [[buffer(2)]],
 device const float* delta1 [[buffer(3)]],device float* low [[buffer(4)]],
 device float* delta [[buffer(5)]],device float* high [[buffer(6)]],
 constant OwnerParams& p [[buffer(7)]],uint gid [[thread_position_in_grid]]){
 uint count=9u*p.allFaces;if(gid>=count)return;float l=0.5f*(low0[gid]+low1[gid]);
 float d=0.5f*(delta0[gid]+delta1[gid]);low[gid]=l;delta[gid]=d;high[gid]=l+d;}
kernel void owner_bind_averaged_flux(device const float* low [[buffer(0)]],
 device const float* delta [[buffer(1)]],device const ulong* r0 [[buffer(2)]],
 device const ulong* r1 [[buffer(3)]],device const ulong* transport [[buffer(4)]],
 device ulong* identity [[buffer(5)]],device atomic_uint* failure [[buffer(6)]],
 constant OwnerParams& p [[buffer(7)]],uint gid [[thread_position_in_grid]]){if(gid!=0u)return;
 if(r0[0]==0ul||r1[0]==0ul||transport[0]==0ul){identity[0]=0ul;
 atomic_fetch_or_explicit(failure,1u<<23u,memory_order_relaxed);return;}ulong hash=14695981039346656037ul;
 if(!resident_full_payload_seals){identity[0]=resident_stage_token(7u,
  ulong4(r0[0],r1[0],transport[0],p.attempt),ulong4(p.stage,p.cells,p.allFaces,0ul));return;}
 hash^=r0[0];hash*=1099511628211ul;hash^=r1[0];hash*=1099511628211ul;
 hash^=transport[0];hash*=1099511628211ul;for(uint word=0u;word<9u*p.allFaces;++word){
  hash^=ulong(as_type<uint>(low[word]));hash*=1099511628211ul;
  hash^=ulong(as_type<uint>(delta[word]));hash*=1099511628211ul;}
 identity[0]=hash==0ul?1ul:hash;}
kernel void owner_bind_candidate(device const float* state [[buffer(0)]],
 device const float* alpha [[buffer(1)]],device const ulong* transport [[buffer(2)]],
 device const ulong* flux [[buffer(3)]],device const ulong* parent [[buffer(4)]],
 device ulong* identity [[buffer(5)]],device atomic_uint* failure [[buffer(6)]],
 constant OwnerParams& p [[buffer(7)]],uint gid [[thread_position_in_grid]]){if(gid!=0u)return;
 if(transport[0]==0ul||flux[0]==0ul||parent[0]==0ul||p.attempt==0ul){identity[0]=0ul;
 atomic_fetch_or_explicit(failure,1u<<24u,memory_order_relaxed);return;}ulong hash=14695981039346656037ul;
 if(!resident_full_payload_seals){identity[0]=resident_stage_token(8u,
  ulong4(transport[0],flux[0],parent[0],p.attempt),ulong4(p.stage,p.cells,p.allFaces,0ul));return;}
 hash^=transport[0];hash*=1099511628211ul;hash^=flux[0];hash*=1099511628211ul;
 hash^=parent[0];hash*=1099511628211ul;for(uint word=0u;word<9u*p.cells;++word){
  hash^=ulong(as_type<uint>(state[word]));hash*=1099511628211ul;}
 for(uint face=0u;face<p.allFaces;++face){hash^=ulong(as_type<uint>(alpha[face]));hash*=1099511628211ul;}
 hash^=p.attempt;hash*=1099511628211ul;hash^=ulong(p.stage);hash*=1099511628211ul;
 identity[0]=hash==0ul?1ul:hash;}
kernel void owner_compose_target(device const float* base [[buffer(0)]],
 device const float* tail [[buffer(1)]],device const float* parent [[buffer(2)]],
 device float* output [[buffer(3)]],constant OwnerParams& p [[buffer(4)]],
 constant uint& correction [[buffer(5)]],uint gid [[thread_position_in_grid]]){
 if(gid>=p.cells)return;if(correction==0u){output[gid]=base[gid];return;}
 EOSDD value=eos_add(eos_dd(parent[gid]),eos_dd(tail[gid]));float rounded=0.0f;
 if(!eos_unique_binary32_round(value,rounded)||!isfinite(rounded))rounded=NAN;
 output[gid]=rounded==0.0f?0.0f:rounded;}
kernel void owner_identify_target(device const float* tangent [[buffer(0)]],
 device const float* source [[buffer(1)]],device const float* diagnostic [[buffer(2)]],
 device const float* tail [[buffer(3)]],device const float* assembled [[buffer(4)]],
 device const ulong* transport [[buffer(5)]],device const ulong* physical [[buffer(6)]],
 device const ulong* candidate [[buffer(7)]],device const ulong* eos [[buffer(8)]],
 device const ulong* frozen [[buffer(9)]],device const ulong* parentTarget [[buffer(10)]],
 device ulong* output [[buffer(11)]],constant OwnerParams& p [[buffer(12)]],
 constant uint& correction [[buffer(13)]],uint gid [[thread_position_in_grid]]){
 if(gid!=0u)return;if(transport[0]==0ul||physical[0]==0ul||candidate[0]==0ul||
  eos[0]==0ul||frozen[0]==0ul||parentTarget[0]==0ul||p.attempt==0ul){output[0]=0ul;return;}
 if(!resident_full_payload_seals){output[0]=resident_stage_token(9u,
  ulong4(transport[0],physical[0],candidate[0],eos[0]),ulong4(frozen[0],parentTarget[0],p.attempt,
   (ulong(p.stage)<<32u)|ulong(correction)));return;}
 ulong hash=14695981039346656037ul;ulong parents[6]={transport[0],physical[0],candidate[0],
  eos[0],frozen[0],parentTarget[0]};for(uint i=0u;i<6u;++i){hash^=parents[i];hash*=1099511628211ul;}
 for(uint cell=0u;cell<p.cells;++cell){float values[5]={tangent[cell],source[cell],diagnostic[cell],
  tail[cell],assembled[cell]};for(uint i=0u;i<5u;++i){hash^=ulong(as_type<uint>(values[i]));
  hash*=1099511628211ul;}}hash^=ulong(correction);hash*=1099511628211ul;
 hash^=ulong(p.stage);hash*=1099511628211ul;hash^=p.attempt;hash*=1099511628211ul;
 output[0]=hash==0ul?1ul:hash;}
kernel void owner_next_open_class(device const uchar* used [[buffer(0)]],
 device const float* vx [[buffer(1)]],device const float* vy [[buffer(2)]],
 device const float* vz [[buffer(3)]],device uchar* next [[buffer(4)]],
 constant TransportParams& t [[buffer(5)]],constant OwnerParams& p [[buffer(6)]],
 uint gid [[thread_position_in_grid]]){if(gid>=t.sideOffset[5]+t.nx*t.ny)return;
 uint side=0u;for(uint s=1u;s<6u;++s)if(gid>=t.sideOffset[s])side=s;
 next[gid]=used[gid];if(t.boundary[side]!=1u)return;uint axis=side/2u;
 bool positive=(side&1u)!=0u;uint local=gid-t.sideOffset[side];uint firstCount=axis==0u?t.ny:t.nx;
 uint first=local%firstCount,second=local/firstCount,x=0u,y=0u,z=0u;
 if(axis==0u){x=positive?t.nx:0u;y=first;z=second;}else if(axis==1u){x=first;y=positive?t.ny:0u;z=second;}
 else{x=first;y=second;z=positive?t.nz:0u;}uint face=tr_face(t,axis,x,y,z)-t.faceOffset[axis];
 float velocity=axis==0u?vx[face]:(axis==1u?vy[face]:vz[face]);float outward=(positive?1.0f:-1.0f)*velocity;
 if(outward < -p.endpointTolerance)next[gid]=1u;else if(outward > p.endpointTolerance)next[gid]=0u;
 bool firstOpen=true;for(uint earlier=0u;earlier<side;++earlier)
  firstOpen=firstOpen&&t.boundary[earlier]!=1u;
 if(p.forceActiveCycle!=0u&&firstOpen&&gid==t.sideOffset[side])next[gid]=uchar(used[gid]^1u);}
kernel void identify_resident_endpoint_class(device const uchar* classes [[buffer(0)]],
 device const ulong* projectionIdentity [[buffer(1)]],
 device const ulong* transportIdentity [[buffer(2)]],device ulong* identity [[buffer(3)]],
 device atomic_uint* failure [[buffer(4)]],constant TransportParams& t [[buffer(5)]],
 uint gid [[thread_position_in_grid]]){if(gid!=0u)return;
 if(projectionIdentity[0]==0ul||transportIdentity[0]==0ul||t.attempt==0ul){identity[0]=0ul;
  atomic_fetch_or_explicit(failure,1u<<26u,memory_order_relaxed);return;}
 ulong hash=14695981039346656037ul;hash^=projectionIdentity[0];hash*=1099511628211ul;
 hash^=transportIdentity[0];hash*=1099511628211ul;uint count=t.sideOffset[5]+t.nx*t.ny;
		if(resident_full_payload_seals)for(uint word=0u;word<count;++word){hash^=ulong(classes[word]);hash*=1099511628211ul;}
 hash^=ulong(t.stage);hash*=1099511628211ul;hash^=t.attempt;hash*=1099511628211ul;
 for(uint side=0u;side<6u;++side){hash^=ulong(t.boundary[side]);hash*=1099511628211ul;
  hash^=ulong(t.sideOffset[side]);hash*=1099511628211ul;}
 identity[0]=hash==0ul?1ul:hash;}
kernel void identify_resident_owner_endpoint_class(device const uchar* classes [[buffer(0)]],
 device const ulong* projectionIdentity [[buffer(1)]],
 device const ulong* transportIdentity [[buffer(2)]],device ulong* identity [[buffer(3)]],
 device atomic_uint* failure [[buffer(4)]],constant TransportParams& t [[buffer(5)]],
 constant OwnerParams& p [[buffer(6)]],uint gid [[thread_position_in_grid]]){if(gid!=0u)return;
 if(projectionIdentity[0]==0ul||transportIdentity[0]==0ul||t.attempt==0ul||p.attempt!=t.attempt||
  p.stage!=t.stage){identity[0]=0ul;atomic_fetch_or_explicit(failure,1u<<26u,memory_order_relaxed);return;}
 ulong hash=14695981039346656037ul;hash^=projectionIdentity[0];hash*=1099511628211ul;
 hash^=transportIdentity[0];hash*=1099511628211ul;uint count=t.sideOffset[5]+t.nx*t.ny;
		if(resident_full_payload_seals)for(uint word=0u;word<count;++word){hash^=ulong(classes[word]);hash*=1099511628211ul;}
 hash^=ulong(t.stage);hash*=1099511628211ul;hash^=t.attempt;hash*=1099511628211ul;
 for(uint side=0u;side<6u;++side){hash^=ulong(t.boundary[side]);hash*=1099511628211ul;
  hash^=ulong(t.sideOffset[side]);hash*=1099511628211ul;}
 hash^=ulong(as_type<uint>(p.endpointTolerance));hash*=1099511628211ul;
 hash^=ulong(p.forceActiveCycle);hash*=1099511628211ul;
 identity[0]=hash==0ul?1ul:hash;}
kernel void owner_minimum_field(device const float* a [[buffer(0)]],device const float* b [[buffer(1)]],
 device float* output [[buffer(2)]],constant uint& count [[buffer(3)]],uint gid [[thread_position_in_grid]]){
 if(gid<count)output[gid]=min(a[gid],b[gid]);}
kernel void owner_half_field(device const float* input [[buffer(0)]],device float* output [[buffer(1)]],
 constant uint& count [[buffer(2)]],uint gid [[thread_position_in_grid]]){
 if(gid<count)output[gid]=0.5f*input[gid];}
kernel void owner_residual(device const float* current [[buffer(0)]],
 device const float* prior [[buffer(1)]],device atomic_uint* maximumBits [[buffer(2)]],
 constant uint& count [[buffer(3)]],constant float& scale [[buffer(4)]],
 uint gid [[thread_position_in_grid]]){if(gid>=count)return;float value=abs(current[gid]-prior[gid])*scale;
 if(!isfinite(value))value=INFINITY;atomic_fetch_max_explicit(maximumBits,as_type<uint>(value),memory_order_relaxed);}
kernel void owner_class_residual(device const uchar* current [[buffer(0)]],
 device const uchar* prior [[buffer(1)]],device atomic_uint* changed [[buffer(2)]],
 constant uint& count [[buffer(3)]],uint gid [[thread_position_in_grid]]){
 if(gid<count&&current[gid]!=prior[gid])atomic_store_explicit(changed,1u,memory_order_relaxed);}
kernel void owner_integrated_open_head(device const float* velocity0 [[buffer(0)]],
 device const float* velocity1 [[buffer(1)]],device const uchar* inflow0 [[buffer(2)]],
 device const uchar* inflow1 [[buffer(3)]],device float* head [[buffer(4)]],
 constant TransportParams& p [[buffer(5)]],constant float& ambientDensity [[buffer(6)]],
 uint gid [[thread_position_in_grid]]){uint side=0u;for(uint s=1u;s<6u;++s)if(gid>=p.sideOffset[s])side=s;
 uint local=gid-p.sideOffset[side],axis=side/2u;bool positive=(side&1u)!=0u;
 uint firstCount=axis==0u?p.ny:p.nx;uint first=local%firstCount,second=local/firstCount;
 if(p.boundary[side]!=1u){head[gid]=0.0f;return;}uint x=0u,y=0u,z=0u,cx=0u,cy=0u,cz=0u;
 if(axis==0u){x=positive?p.nx:0u;y=first;z=second;cx=positive?p.nx-1u:0u;cy=y;cz=z;}
 else if(axis==1u){x=first;y=positive?p.ny:0u;z=second;cx=x;cy=positive?p.ny-1u:0u;cz=z;}
 else{x=first;y=second;z=positive?p.nz:0u;cx=x;cy=y;cz=positive?p.nz-1u:0u;}
 uint face=tr_face(p,axis,x,y,z);float u0=velocity0[face],u1=velocity1[face];
 float speed0=u0*u0,speed1=u1*u1;for(uint tangent=0u;tangent<3u;++tangent)if(tangent!=axis){
  float t0=tr_cell_velocity(velocity0,p,tangent,cx,cy,cz);
  float t1=tr_cell_velocity(velocity1,p,tangent,cx,cy,cz);speed0+=t0*t0;speed1+=t1*t1;}
 float value=-0.25f*ambientDensity*((inflow0[gid]!=0u?speed0:0.0f)+
  (inflow1[gid]!=0u?speed1:0.0f));head[gid]=isfinite(value)?value:0.0f;}
inline ulong owner_stage_lineage_hash(ulong projection,ulong transport,ulong physical,
 ulong candidatePhysical,ulong candidate,ulong eos,ulong source,ulong target,
 uint stage,ulong attempt){ulong hash=14695981039346656037ul;ulong words[8]={projection,
 transport,physical,candidatePhysical,candidate,eos,source,target};
 for(uint i=0u;i<8u;++i){if(words[i]==0ul)return 0ul;hash^=words[i];hash*=1099511628211ul;}
 hash^=ulong(stage);hash*=1099511628211ul;hash^=attempt;hash*=1099511628211ul;
 return hash==0ul?1ul:hash;}
kernel void owner_issue_stage_seal(device const ulong* projection [[buffer(0)]],
 device const ulong* transport [[buffer(1)]],device const ulong* physical [[buffer(2)]],
 device const ulong* candidatePhysical [[buffer(3)]],device const ulong* candidate [[buffer(4)]],
 device const ulong* eos [[buffer(5)]],device const ulong* source [[buffer(6)]],
 device const ulong* target [[buffer(7)]],device ulong* output [[buffer(8)]],
 device atomic_uint* failure [[buffer(9)]],constant OwnerParams& p [[buffer(10)]],
 uint gid [[thread_position_in_grid]]){if(gid!=0u)return;
 ulong value=owner_stage_lineage_hash(projection[0],transport[0],physical[0],candidatePhysical[0],
  candidate[0],eos[0],source[0],target[0],p.stage,p.attempt);
 if(value==0ul||p.stage>2u||p.attempt==0ul){output[0]=0ul;
  atomic_fetch_or_explicit(failure,1u<<25u,memory_order_relaxed);return;}output[0]=value;}
kernel void owner_issue_publication(device const ulong* p0 [[buffer(0)]],
 device const ulong* p1 [[buffer(1)]],device const ulong* p2 [[buffer(2)]],
 device const ulong* t0 [[buffer(3)]],device const ulong* t1 [[buffer(4)]],
 device const ulong* t2 [[buffer(5)]],device const ulong* f0 [[buffer(6)]],
 device const ulong* f1 [[buffer(7)]],device const ulong* f2 [[buffer(8)]],
 device const ulong* a0 [[buffer(9)]],device const ulong* a1 [[buffer(10)]],
 device const ulong* a2 [[buffer(11)]],device const ulong* c0 [[buffer(12)]],
 device const ulong* c1 [[buffer(13)]],device const ulong* c2 [[buffer(14)]],
 device const ulong* e0 [[buffer(15)]],device const ulong* e1 [[buffer(16)]],
 device const ulong* e2 [[buffer(17)]],device const ulong* s0 [[buffer(18)]],
 device const ulong* s1 [[buffer(19)]],device const ulong* s2 [[buffer(20)]],
 device const ulong* d0 [[buffer(21)]],device const ulong* d1 [[buffer(22)]],
 device const ulong* d2 [[buffer(23)]],device const ulong* l0 [[buffer(24)]],
 device const ulong* l1 [[buffer(25)]],device const ulong* l2 [[buffer(26)]],
 device ulong* output [[buffer(27)]],device atomic_uint* failure [[buffer(28)]],
 constant OwnerParams& p [[buffer(29)]],uint gid [[thread_position_in_grid]]){if(gid!=0u)return;
 device const ulong* words[24]={p0,p1,p2,t0,t1,t2,f0,f1,f2,a0,a1,a2,
  c0,c1,c2,e0,e1,e2,s0,s1,s2,d0,d1,d2};
 if(p.stage!=2u||p.attempt==0ul||atomic_load_explicit(failure,memory_order_relaxed)!=0u){
  output[0]=0ul;atomic_fetch_or_explicit(failure,1u<<25u,memory_order_relaxed);return;}
 ulong expected[3]={owner_stage_lineage_hash(p0[0],t0[0],f0[0],a0[0],c0[0],e0[0],s0[0],d0[0],0u,p.attempt),
  owner_stage_lineage_hash(p1[0],t1[0],f1[0],a1[0],c1[0],e1[0],s1[0],d1[0],1u,p.attempt),
  owner_stage_lineage_hash(p2[0],t2[0],f2[0],a2[0],c2[0],e2[0],s2[0],d2[0],2u,p.attempt)};
 if(l0[0]!=expected[0]||l1[0]!=expected[1]||l2[0]!=expected[2]){output[0]=0ul;
  atomic_fetch_or_explicit(failure,1u<<25u,memory_order_relaxed);return;}
 ulong hash=14695981039346656037ul;for(uint i=0u;i<24u;++i){if(words[i][0]==0ul){output[0]=0ul;
  atomic_fetch_or_explicit(failure,1u<<25u,memory_order_relaxed);return;}hash^=words[i][0];hash*=1099511628211ul;}
 hash^=l0[0];hash*=1099511628211ul;hash^=l1[0];hash*=1099511628211ul;
 hash^=l2[0];hash*=1099511628211ul;
 hash^=p.attempt;hash*=1099511628211ul;output[0]=hash==0ul?1ul:hash;}
)METAL";
				return source.c_str();
			}

			explicit ResidentTransportMetalContext(bool stageTokens=false) : device(nil),queue(nil),evaluate(nil),identify(nil),
				physicalFlux(nil),advectivePair(nil),finalizeAdvective(nil),composePair(nil),validatePhysical(nil),
				identifyPhysical(nil),identifyEOSCandidate(nil),
				evaluateEOSCandidate(nil),finalizeEOSCandidate(nil),identifyEOS(nil),
				produceFrozenSource(nil),identifyFrozenSource(nil),
				evaluateTargetTerms(nil),finalizeTarget(nil),identifyTarget(nil),
				identifyProjectionConsumer(nil),consumeTarget(nil),
				diagnoseEOSLog(nil),ownerIssueBootstrap(nil),ownerPackFaces(nil),ownerAverageField(nil),ownerGasSource(nil),
				ownerPredictMomentum(nil),ownerHeunMomentum(nil),ownerBindTransport(nil),
				ownerAverageFlux(nil),ownerBindAveragedFlux(nil),ownerBindCandidate(nil),
				ownerComposeTarget(nil),ownerIdentifyTarget(nil),ownerNextOpenClass(nil),
				ownerIdentifyEndpointClass(nil),ownerIdentifyProducedEndpointClass(nil),ownerMinimumField(nil),
				ownerHalfField(nil),
				ownerResidual(nil),ownerClassResidual(nil),ownerIntegratedOpenHead(nil),
				ownerIssueStageSeal(nil),ownerIssuePublication(nil)
			{
				productionStageTokens=stageTokens;
				compiledSource=std::string("#define RISE_STAGE_TOKENS ")+(stageTokens?"1\n":"0\n")+Source();
				// Qualification and production compile the same arithmetic. Only
				// diagnostic obligation writes get a constant-folded no-op in the
				// latter; failure/admissibility atomics are never redirected.
				const std::string from="atomic_fetch_or_explicit(obligations,",
					to="resident_record_obligation(obligations,";
				std::size_t position=0u;while((position=compiledSource.find(from,position))!=std::string::npos){
					compiledSource.replace(position,from.size(),to);position+=to.size();}
				@autoreleasepool {
					device=DiscoverProductionMetalDevice("resident transport",error);
					if(!device)return;
					MTLCompileOptions* options=[[MTLCompileOptions alloc] init];
					if(@available(macOS 15.0,*)){
						options.mathMode=MTLMathModeSafe;
						options.languageVersion=MTLLanguageVersion3_2;
					}
					else{error="production resident transport requires Metal safe math mode";return;}
					NSError* metalError=nil;NSString* source=[NSString stringWithUTF8String:compiledSource.c_str()];
					id<MTLLibrary> library=[device newLibraryWithSource:source options:options error:&metalError];
					if(!library){error=MetalError("production resident transport library compilation failed",metalError);return;}
					auto pipeline=[&](const char* name)->id<MTLComputePipelineState>{id<MTLFunction> function=
						[library newFunctionWithName:[NSString stringWithUTF8String:name]];
						return function?NameProducerKernel([device newComputePipelineStateWithFunction:function error:&metalError],name):nil;};
					evaluate=pipeline("evaluate_resident_transport");identify=pipeline("identify_resident_transport");
					physicalFlux=pipeline("evaluate_resident_physical_flux");
					advectivePair=pipeline("evaluate_resident_advective_pair");
					finalizeAdvective=pipeline("finalize_resident_advective_high");
					composePair=pipeline("compose_resident_flux_pair");
					validatePhysical=pipeline("validate_resident_physical_flux");
					identifyPhysical=pipeline("identify_resident_physical_flux");
					identifyEOSCandidate=pipeline("identify_resident_eos_candidate");
					evaluateEOSCandidate=pipeline("evaluate_resident_eos_candidate");
					finalizeEOSCandidate=pipeline("finalize_resident_eos_candidate");
					identifyEOS=pipeline("identify_resident_eos");
					produceFrozenSource=pipeline("produce_resident_frozen_source");
					identifyFrozenSource=pipeline("identify_resident_frozen_source");
					evaluateTargetTerms=pipeline("evaluate_resident_target_terms");
					finalizeTarget=pipeline("finalize_resident_target");
					identifyTarget=pipeline("identify_resident_target");
					identifyProjectionConsumer=pipeline("issue_resident_projection_consumer");
					consumeTarget=pipeline("consume_resident_target");
					diagnoseEOSLog=pipeline("diagnose_eos_log_enclosure");
					ownerIssueBootstrap=pipeline("owner_issue_bootstrap_target");
					ownerPackFaces=pipeline("owner_pack_faces");
					ownerAverageField=pipeline("owner_average_field");
					ownerGasSource=pipeline("owner_gas_source");
					ownerPredictMomentum=pipeline("owner_predict_momentum");
					ownerHeunMomentum=pipeline("owner_heun_momentum");
					ownerBindTransport=pipeline("owner_bind_transport");
					ownerAverageFlux=pipeline("owner_average_flux");
					ownerBindAveragedFlux=pipeline("owner_bind_averaged_flux");
					ownerBindCandidate=pipeline("owner_bind_candidate");
					ownerComposeTarget=pipeline("owner_compose_target");
					ownerIdentifyTarget=pipeline("owner_identify_target");
					ownerNextOpenClass=pipeline("owner_next_open_class");
					ownerIdentifyEndpointClass=pipeline("identify_resident_endpoint_class");
					ownerIdentifyProducedEndpointClass=pipeline("identify_resident_owner_endpoint_class");
					ownerMinimumField=pipeline("owner_minimum_field");
					ownerHalfField=pipeline("owner_half_field");
					ownerResidual=pipeline("owner_residual");
					ownerClassResidual=pipeline("owner_class_residual");
					ownerIntegratedOpenHead=pipeline("owner_integrated_open_head");
					ownerIssueStageSeal=pipeline("owner_issue_stage_seal");
					ownerIssuePublication=pipeline("owner_issue_publication");
					if(!evaluate||!identify||!physicalFlux||!advectivePair||!finalizeAdvective||!composePair||
						!validatePhysical||!identifyPhysical||
						!identifyEOSCandidate||!evaluateEOSCandidate||!finalizeEOSCandidate||!identifyEOS||
						!produceFrozenSource||!identifyFrozenSource||
						!evaluateTargetTerms||!finalizeTarget||!identifyTarget||
						!identifyProjectionConsumer||!consumeTarget||
						!diagnoseEOSLog||!ownerIssueBootstrap||!ownerPackFaces||!ownerAverageField||!ownerGasSource||
						!ownerPredictMomentum||!ownerHeunMomentum||!ownerBindTransport||
						!ownerAverageFlux||!ownerBindAveragedFlux||!ownerBindCandidate||
						!ownerComposeTarget||!ownerIdentifyTarget||!ownerNextOpenClass||
						!ownerIdentifyEndpointClass||!ownerIdentifyProducedEndpointClass||!ownerMinimumField||
						!ownerHalfField||
						!ownerResidual||!ownerClassResidual||!ownerIntegratedOpenHead||
						!ownerIssueStageSeal||
						!ownerIssuePublication){error=MetalError(
						"production resident authority pipeline creation failed",metalError);return;}
					queue=[device newCommandQueue];if(!queue)error="production resident transport queue allocation failed";
					const char* sourceBytes=compiledSource.c_str();eosLogIdentity.deviceRegistryId=
						static_cast<std::uint64_t>([device registryID]);
					eosLogIdentity.deviceName=MetalString([device name]);
					eosLogIdentity.deviceFamily=ProductionMetalDeviceFamily(device);
					eosLogIdentity.metalLanguageVersion="3.2";
					eosLogIdentity.metalMathMode="safe";
					eosLogIdentity.librarySourceSHA256=RISECBOR64::SHA256Hex(
						reinterpret_cast<const unsigned char*>(sourceBytes),std::strlen(sourceBytes));
					NSArray<NSString*>* functionNames=[[library functionNames]
						sortedArrayUsingSelector:@selector(compare:)];std::string functionSet;
					for(NSString* name in functionNames){functionSet+=MetalString(name);functionSet+='\n';}
					eosLogIdentity.libraryFunctionSetSHA256=RISECBOR64::SHA256Hex(
						reinterpret_cast<const unsigned char*>(functionSet.data()),functionSet.size());
					eosLogIdentity.kernelName="diagnose_eos_log_enclosure";
					eosLogIdentity.threadExecutionWidth=static_cast<std::size_t>(
						[diagnoseEOSLog threadExecutionWidth]);
					eosLogIdentity.maximumThreadsPerThreadgroup=static_cast<std::size_t>(
						[diagnoseEOSLog maxTotalThreadsPerThreadgroup]);
					eosLogIdentity.staticThreadgroupMemoryBytes=static_cast<std::size_t>(
						[diagnoseEOSLog staticThreadgroupMemoryLength]);
					void* runtimeSymbol=dlsym(RTLD_DEFAULT,"MTLCreateSystemDefaultDevice");
					Dl_info runtimeInfo={};if(runtimeSymbol&&dladdr(runtimeSymbol,&runtimeInfo)!=0&&
						runtimeInfo.dli_fname)eosLogIdentity.metalRuntimeImage=runtimeInfo.dli_fname;
					NSBundle* metalBundle=[NSBundle bundleWithPath:
						@"/System/Library/Frameworks/Metal.framework"];
					eosLogIdentity.metalRuntimeBundleIdentifier=MetalString(
						[metalBundle bundleIdentifier]);
					eosLogIdentity.metalRuntimeBundleVersion=MetalString(
						[metalBundle objectForInfoDictionaryKey:@"CFBundleVersion"]);
				}
			}

			bool Valid() const {return device&&queue&&evaluate&&identify&&physicalFlux&&
				advectivePair&&finalizeAdvective&&composePair&&validatePhysical&&identifyPhysical&&
				identifyEOSCandidate&&evaluateEOSCandidate&&
				finalizeEOSCandidate&&identifyEOS&&produceFrozenSource&&identifyFrozenSource&&
				evaluateTargetTerms&&finalizeTarget&&
				identifyTarget&&identifyProjectionConsumer&&consumeTarget&&diagnoseEOSLog&&
				eosLogIdentity.deviceRegistryId!=0u&&!eosLogIdentity.deviceName.empty()&&
				!eosLogIdentity.deviceFamily.empty()&&!eosLogIdentity.metalRuntimeImage.empty()&&
				!eosLogIdentity.metalRuntimeBundleIdentifier.empty()&&
				!eosLogIdentity.metalRuntimeBundleVersion.empty()&&
				!eosLogIdentity.librarySourceSHA256.empty()&&
				!eosLogIdentity.libraryFunctionSetSHA256.empty()&&error.empty();}
		};

		MetalRemapContext& Context()
		{
			static MetalRemapContext context;
			return context;
		}

		SingleStageFCTMetalContext& SingleStageFCTContext()
		{
			static SingleStageFCTMetalContext context;
			return context;
		}

		ResidentTransportMetalContext& ResidentTransportContext(bool productionStageTokens=false)
		{
			static ResidentTransportMetalContext context;
			if(productionStageTokens){static ResidentTransportMetalContext production(true);return production;}
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
			if(ActiveProducerKernelProfile)ActiveProducerKernelProfile->DispatchRecord(encoder,pipeline,count);
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
		try {
		FireProductionRemapResult computed;
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
			const std::vector<float>& lowerAmbientValues=request.lineSpecificAmbientValues?
				request.lowerAmbientValues:request.ambientValues;
			const std::vector<float>& upperAmbientValues=request.lineSpecificAmbientValues?
				request.upperAmbientValues:request.ambientValues;
			const std::size_t lowerAmbientBytes=lowerAmbientValues.size()*sizeof(float);
			const std::size_t upperAmbientBytes=upperAmbientValues.size()*sizeof(float);
			const std::size_t alphaCount=request.lineCount*request.lineLength;
			const std::size_t fluxCount=request.componentCount*request.lineCount*
				(request.lineLength+1u);
			id<MTLBuffer> values=[context.device newBufferWithBytes:request.values.data()
				length:valueBytes options:MTLResourceStorageModeShared];
			id<MTLBuffer> velocity=[context.device newBufferWithBytes:request.faceVelocityMPerS.data()
				length:velocityBytes options:MTLResourceStorageModeShared];
			id<MTLBuffer> lowerAmbient=[context.device newBufferWithBytes:lowerAmbientValues.data()
				length:lowerAmbientBytes options:MTLResourceStorageModeShared];
			id<MTLBuffer> upperAmbient=[context.device newBufferWithBytes:upperAmbientValues.data()
				length:upperAmbientBytes options:MTLResourceStorageModeShared];
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
					request.boundary),request.lineSpecificAmbientValues?1u:0u,
				request.cellWidthM,request.timeStepS};
			id<MTLBuffer> parameterBuffer=[context.device newBufferWithBytes:&parameters
				length:sizeof(parameters) options:MTLResourceStorageModeShared];
			if( !values||!velocity||!lowerAmbient||!upperAmbient||!left||!right||!alpha||!prefix||!flux||
				!updated||!parameterBuffer ) {
				if( structuredError ) *structuredError="production fire remap buffer allocation failed";
				return false;
			}
			std::uint64_t certifiedWorkingSetBytes=0u,actualWorkingSetBytes=0u;
			if( !FireProductionRemapWorkingSetBytes(request,certifiedWorkingSetBytes) ) {
				if( structuredError ) *structuredError=
					"production fire remap working-set certificate failed";
				return false;
			}
			auto recordBuffer=[&](id<MTLBuffer> buffer) -> bool {
				const std::uint64_t allocated=static_cast<std::uint64_t>([buffer allocatedSize]);
				if( actualWorkingSetBytes>
					std::numeric_limits<std::uint64_t>::max()-allocated ) return false;
				actualWorkingSetBytes+=allocated;return true;
			};
			if( !recordBuffer(values)||!recordBuffer(velocity)||!recordBuffer(lowerAmbient)||
				!recordBuffer(upperAmbient)||!recordBuffer(left)||!recordBuffer(right)||
				!recordBuffer(alpha)||!recordBuffer(prefix)||!recordBuffer(flux)||
				!recordBuffer(updated)||!recordBuffer(parameterBuffer)||
				actualWorkingSetBytes>certifiedWorkingSetBytes||
				actualWorkingSetBytes>(std::uint64_t(2u)<<30u) ) {
				if( structuredError ) *structuredError=
					"production fire remap actual allocation exceeds certificate";
				return false;
			}
			id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context.queue);
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
			[encoder setBuffer:lowerAmbient offset:0 atIndex:2];
			[encoder setBuffer:upperAmbient offset:0 atIndex:3];
			[encoder setBuffer:left offset:0 atIndex:4];[encoder setBuffer:right offset:0 atIndex:5];
			[encoder setBuffer:alpha offset:0 atIndex:6];
			[encoder setBuffer:parameterBuffer offset:0 atIndex:7];
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
			[encoder setBuffer:lowerAmbient offset:0 atIndex:2];
			[encoder setBuffer:upperAmbient offset:0 atIndex:3];
			[encoder setBuffer:left offset:0 atIndex:4];[encoder setBuffer:right offset:0 atIndex:5];
			[encoder setBuffer:prefix offset:0 atIndex:6];[encoder setBuffer:flux offset:0 atIndex:7];
			[encoder setBuffer:parameterBuffer offset:0 atIndex:8];
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
			CommitTrackedMetalCommand(command);[command waitUntilCompleted];
			if( [command status]!=MTLCommandBufferStatusCompleted ) {
				if( structuredError ) *structuredError=MetalError(
					"production fire remap command failed",[command error]);
				return false;
			}
			const float* updatedValues=static_cast<const float*>(ReadTrackedMetalBuffer(updated));
			const float* faceFluxes=static_cast<const float*>(ReadTrackedMetalBuffer(flux));
			const float* limiter=static_cast<const float*>(ReadTrackedMetalBuffer(alpha));
			computed.updatedValues.assign(updatedValues,updatedValues+request.values.size());
			computed.faceFluxes.assign(faceFluxes,faceFluxes+fluxCount);
			computed.sharedLimiterAlpha.assign(limiter,limiter+alphaCount);
			computed.deviceElapsedMS=([command GPUEndTime]-[command GPUStartTime])*1000.0;
		}
		if( !AllFinite(computed.updatedValues)||!AllFinite(computed.faceFluxes)||
			!AllFinite(computed.sharedLimiterAlpha)||!std::isfinite(computed.deviceElapsedMS) ) {
			if( structuredError )
				*structuredError="production fire remap produced nonfinite device output";
			return false;
		}
		if( structuredError ) structuredError->clear();
		result=std::move(computed);
		return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionRemapResult();
			if( structuredError ) try {
				*structuredError="production fire remap allocation failed";
			} catch( const std::bad_alloc& ) {}
			return false;
		}
	}

	bool RemapFireProductionCellPalindromeMetal(
		const FireProductionCellPalindromeRequest& request,
		FireProductionCellPalindromeResult& result, std::string* structuredError )
	{
		result=FireProductionCellPalindromeResult();
		try {
		if( !ValidateFireProductionCellPalindromeRequest(request,structuredError) ) return false;
		MetalRemapContext& context=Context();
		if( !context.Valid() ) {
			if( structuredError ) *structuredError=context.error;
			return false;
		}
		@autoreleasepool {
			const std::uint64_t beginningCommandCommitCount=MetalCommandCommitCount;
			const std::uint64_t beginningHostBufferReadCount=MetalHostBufferReadCount;
			const std::size_t cells=request.shape.CellCount();
			const std::size_t valueCount=request.componentCount*cells;
			const std::size_t valueBytes=valueCount*sizeof(float);
			const std::size_t xFaces=request.frozenVelocityMPerS[0].size();
			const std::size_t yFaces=request.frozenVelocityMPerS[1].size();
			const std::size_t zFaces=request.frozenVelocityMPerS[2].size();
			const std::size_t maximumLineFaces=std::max(xFaces,std::max(yFaces,zFaces));
			const std::size_t maximumFluxCount=request.componentCount*maximumLineFaces;
			std::uint64_t certifiedWorkingSetBytes=0u;
			if( !FireProductionCellPalindromeWorkingSetBytes(request.shape,
				request.componentCount,certifiedWorkingSetBytes,
				request.retainAcceptedGasMassDose) ) {
				if( structuredError ) *structuredError="production palindrome working-set certificate failed";
				return false;
			}
			id<MTLBuffer> inputStage=[context.device newBufferWithBytes:
				request.conservativeValues.data() length:valueBytes
				options:MTLResourceStorageModeShared];
			id<MTLBuffer> xStage=[context.device newBufferWithBytes:
				request.frozenVelocityMPerS[0].data() length:xFaces*sizeof(float)
				options:MTLResourceStorageModeShared];
			id<MTLBuffer> yStage=[context.device newBufferWithBytes:
				request.frozenVelocityMPerS[1].data() length:yFaces*sizeof(float)
				options:MTLResourceStorageModeShared];
			id<MTLBuffer> zStage=[context.device newBufferWithBytes:
				request.frozenVelocityMPerS[2].data() length:zFaces*sizeof(float)
				options:MTLResourceStorageModeShared];
			id<MTLBuffer> gridA=[context.device newBufferWithLength:valueBytes
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> gridB=[context.device newBufferWithLength:valueBytes
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> xVelocity=[context.device newBufferWithLength:xFaces*sizeof(float)
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> yVelocity=[context.device newBufferWithLength:yFaces*sizeof(float)
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> zVelocity=[context.device newBufferWithLength:zFaces*sizeof(float)
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> lineValues=[context.device newBufferWithLength:valueBytes
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> lineUpdated=[context.device newBufferWithLength:valueBytes
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> left=[context.device newBufferWithLength:valueBytes
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> right=[context.device newBufferWithLength:valueBytes
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> lineVelocity=[context.device newBufferWithLength:
				maximumLineFaces*sizeof(float) options:MTLResourceStorageModePrivate];
			id<MTLBuffer> alpha=[context.device newBufferWithLength:cells*sizeof(float)
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> prefix=[context.device newBufferWithLength:
				maximumFluxCount*sizeof(float) options:MTLResourceStorageModePrivate];
			id<MTLBuffer> flux=[context.device newBufferWithLength:
				maximumFluxCount*sizeof(float) options:MTLResourceStorageModePrivate];
			id<MTLBuffer> ambient=[context.device newBufferWithBytes:request.ambientValues.data()
				length:request.ambientValues.size()*sizeof(float)
				options:MTLResourceStorageModeShared];
			id<MTLBuffer> outputStage=[context.device newBufferWithLength:valueBytes
				options:MTLResourceStorageModeShared];
			if( !inputStage||!xStage||!yStage||!zStage||!gridA||!gridB||!xVelocity||
				!yVelocity||!zVelocity||!lineValues||!lineUpdated||!left||!right||
				!lineVelocity||!alpha||!prefix||!flux||!ambient||!outputStage ) {
				if( structuredError ) *structuredError="production palindrome buffer allocation failed";
				return false;
			}
			std::uint64_t actualTrackedWorkingSetBytes=
				static_cast<std::uint64_t>(2u*valueBytes)+
				static_cast<std::uint64_t>(xFaces+yFaces+zFaces)*sizeof(float)+
				static_cast<std::uint64_t>(request.componentCount)*sizeof(float);
			std::uint32_t privateResidentBufferCount=0u,sharedBufferCount=0u;
			auto recordBuffer=[&](id<MTLBuffer> buffer, MTLStorageMode expected,
				std::uint32_t& count) -> bool {
				if( [buffer storageMode]!=expected ) return false;
				const std::uint64_t allocated=static_cast<std::uint64_t>([buffer allocatedSize]);
				if( actualTrackedWorkingSetBytes>
					std::numeric_limits<std::uint64_t>::max()-allocated ) return false;
				actualTrackedWorkingSetBytes+=allocated;++count;return true;
			};
			if( !recordBuffer(inputStage,MTLStorageModeShared,sharedBufferCount)||
				!recordBuffer(xStage,MTLStorageModeShared,sharedBufferCount)||
				!recordBuffer(yStage,MTLStorageModeShared,sharedBufferCount)||
				!recordBuffer(zStage,MTLStorageModeShared,sharedBufferCount)||
				!recordBuffer(ambient,MTLStorageModeShared,sharedBufferCount)||
				!recordBuffer(outputStage,MTLStorageModeShared,sharedBufferCount)||
				!recordBuffer(gridA,MTLStorageModePrivate,privateResidentBufferCount)||
				!recordBuffer(gridB,MTLStorageModePrivate,privateResidentBufferCount)||
				!recordBuffer(xVelocity,MTLStorageModePrivate,privateResidentBufferCount)||
				!recordBuffer(yVelocity,MTLStorageModePrivate,privateResidentBufferCount)||
				!recordBuffer(zVelocity,MTLStorageModePrivate,privateResidentBufferCount)||
				!recordBuffer(lineValues,MTLStorageModePrivate,privateResidentBufferCount)||
				!recordBuffer(lineUpdated,MTLStorageModePrivate,privateResidentBufferCount)||
				!recordBuffer(left,MTLStorageModePrivate,privateResidentBufferCount)||
				!recordBuffer(right,MTLStorageModePrivate,privateResidentBufferCount)||
				!recordBuffer(lineVelocity,MTLStorageModePrivate,privateResidentBufferCount)||
				!recordBuffer(alpha,MTLStorageModePrivate,privateResidentBufferCount)||
				!recordBuffer(prefix,MTLStorageModePrivate,privateResidentBufferCount)||
				!recordBuffer(flux,MTLStorageModePrivate,privateResidentBufferCount) ) {
				if( structuredError ) *structuredError="production palindrome resource mode is invalid";
				return false;
			}
			id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context.queue);
			if( !command ) {
				if( structuredError ) *structuredError="production palindrome command allocation failed";
				return false;
			}
			id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];
			if( !blit ) {
				if( structuredError ) *structuredError="production palindrome upload encoder failed";
				return false;
			}
			[blit copyFromBuffer:inputStage sourceOffset:0 toBuffer:gridA destinationOffset:0
				size:valueBytes];
			[blit copyFromBuffer:xStage sourceOffset:0 toBuffer:xVelocity destinationOffset:0
				size:xFaces*sizeof(float)];
			[blit copyFromBuffer:yStage sourceOffset:0 toBuffer:yVelocity destinationOffset:0
				size:yFaces*sizeof(float)];
			[blit copyFromBuffer:zStage sourceOffset:0 toBuffer:zVelocity destinationOffset:0
				size:zFaces*sizeof(float)];
			[blit endEncoding];
			const unsigned int axes[]={0u,1u,2u,1u,0u};
			const float steps[]={0.5f*request.timeStepS,0.5f*request.timeStepS,
				request.timeStepS,0.5f*request.timeStepS,0.5f*request.timeStepS};
			auto boundaryValue=[](FireProductionProjectionBoundary boundary) -> std::uint32_t {
				return boundary==FireProductionProjectionPeriodic?0u:
					(boundary==FireProductionProjectionPressureOpen?1u:2u);
			};
			for( unsigned int pass=0u;pass<5u;++pass ) {
				const unsigned int axis=axes[pass];
				const std::size_t length=axis==0u?request.shape.nx:
					(axis==1u?request.shape.ny:request.shape.nz);
				const std::size_t lines=cells/length;
				const std::size_t lineFaces=lines*(length+1u);
				const std::size_t passFluxCount=request.componentCount*lineFaces;
				const MetalGridParameters gridParameters={
					static_cast<std::uint32_t>(request.shape.nx),
					static_cast<std::uint32_t>(request.shape.ny),
					static_cast<std::uint32_t>(request.shape.nz),axis,
					static_cast<std::uint32_t>(request.componentCount)};
				const MetalParameters parameters={static_cast<std::uint32_t>(length),
					static_cast<std::uint32_t>(lines),
					static_cast<std::uint32_t>(request.componentCount),
					boundaryValue(request.boundary[2u*axis]),
					boundaryValue(request.boundary[2u*axis+1u]),0u,request.shape.cellWidthM,
					steps[pass]};
				id<MTLBuffer> gridParameterBuffer=[context.device newBufferWithBytes:&gridParameters
					length:sizeof(gridParameters) options:MTLResourceStorageModeShared];
				id<MTLBuffer> parameterBuffer=[context.device newBufferWithBytes:&parameters
					length:sizeof(parameters) options:MTLResourceStorageModeShared];
				if( !gridParameterBuffer||!parameterBuffer ) {
					if( structuredError ) *structuredError="production palindrome parameter allocation failed";
					return false;
				}
				if( !recordBuffer(gridParameterBuffer,MTLStorageModeShared,sharedBufferCount)||
					!recordBuffer(parameterBuffer,MTLStorageModeShared,sharedBufferCount) ) {
					if( structuredError ) *structuredError="production palindrome parameter mode is invalid";
					return false;
				}
				id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
				if( !encoder ) {
					if( structuredError ) *structuredError="production palindrome gather encoder failed";
					return false;
				}
				[encoder setBuffer:gridA offset:0 atIndex:0];[encoder setBuffer:lineValues offset:0 atIndex:1];
				[encoder setBuffer:gridParameterBuffer offset:0 atIndex:2];
				Dispatch(encoder,context.gatherValues,valueCount);[encoder endEncoding];
				encoder=[command computeCommandEncoder];
				if( !encoder ) {
					if( structuredError ) *structuredError="production palindrome velocity gather failed";
					return false;
				}
				[encoder setBuffer:xVelocity offset:0 atIndex:0];[encoder setBuffer:yVelocity offset:0 atIndex:1];
				[encoder setBuffer:zVelocity offset:0 atIndex:2];[encoder setBuffer:lineVelocity offset:0 atIndex:3];
				[encoder setBuffer:gridParameterBuffer offset:0 atIndex:4];
				Dispatch(encoder,context.gatherVelocity,lineFaces);[encoder endEncoding];
				encoder=[command computeCommandEncoder];
				if( !encoder ) {
					if( structuredError ) *structuredError="production palindrome reconstruct encoder failed";
					return false;
				}
				[encoder setBuffer:lineValues offset:0 atIndex:0];[encoder setBuffer:lineVelocity offset:0 atIndex:1];
				[encoder setBuffer:ambient offset:0 atIndex:2];
				[encoder setBuffer:ambient offset:0 atIndex:3];
				[encoder setBuffer:left offset:0 atIndex:4];[encoder setBuffer:right offset:0 atIndex:5];
				[encoder setBuffer:alpha offset:0 atIndex:6];
				[encoder setBuffer:parameterBuffer offset:0 atIndex:7];
				Dispatch(encoder,context.reconstruct,cells);[encoder endEncoding];
				const std::size_t padded=NextPowerOfTwo(length);
				encoder=[command computeCommandEncoder];
				if( !encoder||padded>static_cast<std::size_t>(
					[context.scan maxTotalThreadsPerThreadgroup]) ) {
					if( structuredError ) *structuredError="production palindrome scan encoder failed";
					return false;
				}
				[encoder setComputePipelineState:context.scan];
				[encoder setBuffer:lineValues offset:0 atIndex:0];[encoder setBuffer:prefix offset:0 atIndex:1];
				[encoder setBuffer:parameterBuffer offset:0 atIndex:2];
				[encoder setThreadgroupMemoryLength:padded*sizeof(float) atIndex:0];
				[encoder dispatchThreadgroups:MTLSizeMake(request.componentCount*lines,1,1)
					threadsPerThreadgroup:MTLSizeMake(padded,1,1)];[encoder endEncoding];
				encoder=[command computeCommandEncoder];if( !encoder ) {
					if( structuredError ) *structuredError="production palindrome flux encoder failed";
					return false;
				}
				[encoder setBuffer:lineValues offset:0 atIndex:0];[encoder setBuffer:lineVelocity offset:0 atIndex:1];
				[encoder setBuffer:ambient offset:0 atIndex:2];
				[encoder setBuffer:ambient offset:0 atIndex:3];
				[encoder setBuffer:left offset:0 atIndex:4];[encoder setBuffer:right offset:0 atIndex:5];
				[encoder setBuffer:prefix offset:0 atIndex:6];[encoder setBuffer:flux offset:0 atIndex:7];
				[encoder setBuffer:parameterBuffer offset:0 atIndex:8];
				const bool periodic=request.boundary[2u*axis]==FireProductionProjectionPeriodic;
				Dispatch(encoder,context.flux,periodic?request.componentCount*lines*length:
					passFluxCount);[encoder endEncoding];
				encoder=[command computeCommandEncoder];if( !encoder ) {
					if( structuredError ) *structuredError="production palindrome update encoder failed";
					return false;
				}
				[encoder setBuffer:lineValues offset:0 atIndex:0];[encoder setBuffer:flux offset:0 atIndex:1];
				[encoder setBuffer:lineUpdated offset:0 atIndex:2];[encoder setBuffer:parameterBuffer offset:0 atIndex:3];
				Dispatch(encoder,context.update,valueCount);[encoder endEncoding];
				encoder=[command computeCommandEncoder];if( !encoder ) {
					if( structuredError ) *structuredError="production palindrome scatter encoder failed";
					return false;
				}
				[encoder setBuffer:lineUpdated offset:0 atIndex:0];[encoder setBuffer:gridB offset:0 atIndex:1];
				[encoder setBuffer:gridParameterBuffer offset:0 atIndex:2];
				Dispatch(encoder,context.scatterValues,valueCount);[encoder endEncoding];
				std::swap(gridA,gridB);
			}
			blit=[command blitCommandEncoder];
			if( !blit ) {
				if( structuredError ) *structuredError="production palindrome publication encoder failed";
				return false;
			}
			[blit copyFromBuffer:gridA sourceOffset:0 toBuffer:outputStage destinationOffset:0
				size:valueBytes];[blit endEncoding];
			if( actualTrackedWorkingSetBytes>certifiedWorkingSetBytes||
				actualTrackedWorkingSetBytes>(std::uint64_t(2u)<<30u) ) {
				if( structuredError ) *structuredError="production palindrome actual allocation exceeds certificate";
				return false;
			}
			CommitTrackedMetalCommand(command);[command waitUntilCompleted];
			if( [command status]!=MTLCommandBufferStatusCompleted ) {
				if( structuredError ) *structuredError=MetalError(
					"production palindrome command failed",[command error]);
				return false;
			}
			const float* output=static_cast<const float*>(ReadTrackedMetalBuffer(outputStage));
			const std::uint64_t commandCommitCount=
				MetalCommandCommitCount-beginningCommandCommitCount;
			const std::uint64_t hostBufferReadCount=
				MetalHostBufferReadCount-beginningHostBufferReadCount;
			if( commandCommitCount!=1u||hostBufferReadCount!=1u ) {
				if( structuredError ) *structuredError=
					"production palindrome command or host-read topology changed";
				return false;
			}
			result.conservativeValues.assign(output,output+valueCount);
			result.executedSubmapCount=5u;
			result.privateResidentBufferCount=privateResidentBufferCount;
			result.sharedBufferCount=sharedBufferCount;
			result.commandCommitCount=static_cast<std::uint32_t>(commandCommitCount);
			result.interstageFullGridReadbackCount=
				static_cast<std::uint32_t>(hostBufferReadCount-1u);
			result.certifiedWorkingSetBytes=certifiedWorkingSetBytes;
			result.actualTrackedWorkingSetBytes=actualTrackedWorkingSetBytes;
			result.deviceElapsedMS=([command GPUEndTime]-[command GPUStartTime])*1000.0;
		}
		if( !AllFinite(result.conservativeValues)||!std::isfinite(result.deviceElapsedMS) ) {
			result=FireProductionCellPalindromeResult();
			if( structuredError ) *structuredError="production palindrome produced nonfinite output";
			return false;
		}
		if( structuredError ) structuredError->clear();
		return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionCellPalindromeResult();
			if( structuredError ) try {
				*structuredError="production palindrome allocation failed";
			} catch( const std::bad_alloc& ) {}
			return false;
		}
	}

	static bool RemapFireProductionCellPalindromeMetalResidentImpl(
		const FireProductionCellPalindromeRequest& request,
		const FireProductionMetalCellPalindromeResidentInput& input,
		FireProductionMetalCellPalindromeResidentResult& result,
		const bool retainAcceptedGasMassDoseOverride,
		std::string* structuredError )
	{
		result=FireProductionMetalCellPalindromeResidentResult();
		try {
			if( !ValidateFireProductionCellPalindromeRequest(request,structuredError) ) return false;
			MetalRemapContext& context=Context();
			if( !context.Valid() ) {
				if( structuredError ) *structuredError=context.error;
				return false;
			}
			@autoreleasepool {
				const std::size_t cells=request.shape.CellCount();
				const std::size_t valueCount=request.componentCount*cells;
				const std::size_t valueBytes=valueCount*sizeof(float);
				const std::size_t faceCounts[]={
					FireProductionProjectionFaceCount(request.shape,0u),
					FireProductionProjectionFaceCount(request.shape,1u),
					FireProductionProjectionFaceCount(request.shape,2u)};
				if( !input.conservativeValues||!input.ambientValues||
					[input.conservativeValues storageMode]!=MTLStorageModePrivate||
					[input.conservativeValues length]<valueBytes||
					[input.ambientValues length]<request.componentCount*sizeof(float) ) {
					if( structuredError ) *structuredError=
						"production resident palindrome input ownership is invalid";
					return false;
				}
				for( unsigned int axis=0u;axis<3u;++axis ) if(
					!input.frozenVelocityMPerS[axis]||
					[input.frozenVelocityMPerS[axis] storageMode]!=MTLStorageModePrivate||
					[input.frozenVelocityMPerS[axis] length]<faceCounts[axis]*sizeof(float) ) {
					if( structuredError ) *structuredError=
						"production resident palindrome carrier ownership is invalid";
					return false;
				}
				const std::size_t maximumLineFaces=std::max(faceCounts[0],
					std::max(faceCounts[1],faceCounts[2]));
				const std::size_t maximumFluxCount=request.componentCount*maximumLineFaces;
				auto privateBuffer=[&](std::size_t bytes) {
					return [context.device newBufferWithLength:bytes
						options:MTLResourceStorageModePrivate];
				};
				id<MTLBuffer> gridA=privateBuffer(valueBytes),gridB=privateBuffer(valueBytes);
				id<MTLBuffer> lineValues=privateBuffer(valueBytes),
					lineUpdated=privateBuffer(valueBytes),left=privateBuffer(valueBytes),
					right=privateBuffer(valueBytes),
					lineVelocity=privateBuffer(maximumLineFaces*sizeof(float)),
					alpha=privateBuffer(cells*sizeof(float)),
					prefix=privateBuffer(maximumFluxCount*sizeof(float)),
					flux=privateBuffer(maximumFluxCount*sizeof(float));
				const unsigned int axes[]={0u,1u,2u,1u,0u};
				const bool retainAcceptedGasMassDose=request.componentCount==9u&&
					(request.retainAcceptedGasMassDose||retainAcceptedGasMassDoseOverride);
				std::array<id<MTLBuffer>,5> acceptedGasMassDose;
				acceptedGasMassDose.fill(nil);
				if( retainAcceptedGasMassDose )
					for( unsigned int pass=0u;pass<5u;++pass )
					acceptedGasMassDose[pass]=privateBuffer(faceCounts[axes[pass]]*sizeof(float));
				const id<MTLBuffer> privateWork[]={gridA,gridB,lineValues,lineUpdated,left,right,
					lineVelocity,alpha,prefix,flux};
				std::uint64_t actualBytes=0u;
				auto record=[&](id<MTLBuffer> buffer, MTLStorageMode mode) {
					if( !buffer||[buffer storageMode]!=mode ) return false;
					const std::uint64_t bytes=static_cast<std::uint64_t>([buffer allocatedSize]);
					if( actualBytes>std::numeric_limits<std::uint64_t>::max()-bytes ) return false;
					actualBytes+=bytes;return true;
				};
				if( !record(input.conservativeValues,MTLStorageModePrivate)||
					!record(input.frozenVelocityMPerS[0],MTLStorageModePrivate)||
					!record(input.frozenVelocityMPerS[1],MTLStorageModePrivate)||
					!record(input.frozenVelocityMPerS[2],MTLStorageModePrivate)||
					!record(input.ambientValues,[input.ambientValues storageMode]) ) {
					if( structuredError ) *structuredError=
						"production resident palindrome borrowed allocation is invalid";
					return false;
				}
				for( id<MTLBuffer> buffer : privateWork ) if(
					!record(buffer,MTLStorageModePrivate) ) {
					if( structuredError ) *structuredError=
						"production resident palindrome work allocation failed";
					return false;
				}
				if( retainAcceptedGasMassDose )
					for( id<MTLBuffer> buffer : acceptedGasMassDose )
					if( !record(buffer,MTLStorageModePrivate) ) {
						if( structuredError ) *structuredError=
							"production resident palindrome gas-flux allocation failed";
						return false;
					}
				const std::uint64_t beginningCommandCommitCount=MetalCommandCommitCount;
				const std::uint64_t beginningHostBufferReadCount=MetalHostBufferReadCount;
				id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context.queue);
				if( !command ) {
					if( structuredError ) *structuredError=
						"production resident palindrome command allocation failed";
					return false;
				}
				id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];
				if( !blit ) {
					if( structuredError ) *structuredError=
						"production resident palindrome initialization encoder failed";
					return false;
				}
				[blit copyFromBuffer:input.conservativeValues sourceOffset:0 toBuffer:gridA
					destinationOffset:0 size:valueBytes];[blit endEncoding];
				const float steps[]={0.5f*request.timeStepS,0.5f*request.timeStepS,
					request.timeStepS,0.5f*request.timeStepS,0.5f*request.timeStepS};
				auto boundaryValue=[](FireProductionProjectionBoundary boundary) -> std::uint32_t {
					return boundary==FireProductionProjectionPeriodic?0u:
						(boundary==FireProductionProjectionPressureOpen?1u:2u);
				};
				for( unsigned int pass=0u;pass<5u;++pass ) {
					const unsigned int axis=axes[pass];
					const std::size_t length=axis==0u?request.shape.nx:
						(axis==1u?request.shape.ny:request.shape.nz);
					const std::size_t lines=cells/length,lineFaces=lines*(length+1u);
					const MetalGridParameters gridParameters={
						static_cast<std::uint32_t>(request.shape.nx),
						static_cast<std::uint32_t>(request.shape.ny),
						static_cast<std::uint32_t>(request.shape.nz),axis,
						static_cast<std::uint32_t>(request.componentCount)};
					const MetalParameters parameters={static_cast<std::uint32_t>(length),
						static_cast<std::uint32_t>(lines),
						static_cast<std::uint32_t>(request.componentCount),
						boundaryValue(request.boundary[2u*axis]),
						boundaryValue(request.boundary[2u*axis+1u]),0u,
						request.shape.cellWidthM,steps[pass]};
					id<MTLBuffer> gridParameterBuffer=[context.device newBufferWithBytes:&gridParameters
						length:sizeof(gridParameters) options:MTLResourceStorageModeShared];
					id<MTLBuffer> parameterBuffer=[context.device newBufferWithBytes:&parameters
						length:sizeof(parameters) options:MTLResourceStorageModeShared];
					if( !record(gridParameterBuffer,MTLStorageModeShared)||
						!record(parameterBuffer,MTLStorageModeShared) ) {
						if( structuredError ) *structuredError=
							"production resident palindrome parameter allocation failed";
						return false;
					}
					auto encoder=[command computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:gridA offset:0 atIndex:0];
					[encoder setBuffer:lineValues offset:0 atIndex:1];
					[encoder setBuffer:gridParameterBuffer offset:0 atIndex:2];
					Dispatch(encoder,context.gatherValues,valueCount);[encoder endEncoding];
					encoder=[command computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:input.frozenVelocityMPerS[0] offset:0 atIndex:0];
					[encoder setBuffer:input.frozenVelocityMPerS[1] offset:0 atIndex:1];
					[encoder setBuffer:input.frozenVelocityMPerS[2] offset:0 atIndex:2];
					[encoder setBuffer:lineVelocity offset:0 atIndex:3];
					[encoder setBuffer:gridParameterBuffer offset:0 atIndex:4];
					Dispatch(encoder,context.gatherVelocity,lineFaces);[encoder endEncoding];
					encoder=[command computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:lineValues offset:0 atIndex:0];
					[encoder setBuffer:lineVelocity offset:0 atIndex:1];
					[encoder setBuffer:input.ambientValues offset:0 atIndex:2];
					[encoder setBuffer:input.ambientValues offset:0 atIndex:3];
					[encoder setBuffer:left offset:0 atIndex:4];
					[encoder setBuffer:right offset:0 atIndex:5];
					[encoder setBuffer:alpha offset:0 atIndex:6];
					[encoder setBuffer:parameterBuffer offset:0 atIndex:7];
					Dispatch(encoder,context.reconstruct,cells);[encoder endEncoding];
					const std::size_t padded=NextPowerOfTwo(length);
					encoder=[command computeCommandEncoder];
					if( !encoder||padded>static_cast<std::size_t>(
						[context.scan maxTotalThreadsPerThreadgroup]) ) return false;
					[encoder setComputePipelineState:context.scan];
					[encoder setBuffer:lineValues offset:0 atIndex:0];
					[encoder setBuffer:prefix offset:0 atIndex:1];
					[encoder setBuffer:parameterBuffer offset:0 atIndex:2];
					[encoder setThreadgroupMemoryLength:padded*sizeof(float) atIndex:0];
					[encoder dispatchThreadgroups:MTLSizeMake(request.componentCount*lines,1,1)
						threadsPerThreadgroup:MTLSizeMake(padded,1,1)];[encoder endEncoding];
					encoder=[command computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:lineValues offset:0 atIndex:0];
					[encoder setBuffer:lineVelocity offset:0 atIndex:1];
					[encoder setBuffer:input.ambientValues offset:0 atIndex:2];
					[encoder setBuffer:input.ambientValues offset:0 atIndex:3];
					[encoder setBuffer:left offset:0 atIndex:4];
					[encoder setBuffer:right offset:0 atIndex:5];
					[encoder setBuffer:prefix offset:0 atIndex:6];
					[encoder setBuffer:flux offset:0 atIndex:7];
					[encoder setBuffer:parameterBuffer offset:0 atIndex:8];
					const bool periodic=request.boundary[2u*axis]==FireProductionProjectionPeriodic;
					Dispatch(encoder,context.flux,periodic?request.componentCount*lines*length:
						request.componentCount*lineFaces);[encoder endEncoding];
					encoder=[command computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:lineValues offset:0 atIndex:0];
					[encoder setBuffer:flux offset:0 atIndex:1];
					[encoder setBuffer:lineUpdated offset:0 atIndex:2];
					[encoder setBuffer:parameterBuffer offset:0 atIndex:3];
					Dispatch(encoder,context.update,valueCount);[encoder endEncoding];
					if( retainAcceptedGasMassDose ) {
						encoder=[command computeCommandEncoder];if( !encoder ) return false;
						[encoder setBuffer:flux offset:0 atIndex:0];
						[encoder setBuffer:acceptedGasMassDose[pass] offset:0 atIndex:1];
						[encoder setBuffer:gridParameterBuffer offset:0 atIndex:2];
						Dispatch(encoder,context.extractGasMassDose,faceCounts[axis]);
						[encoder endEncoding];
					}
					encoder=[command computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:lineUpdated offset:0 atIndex:0];
					[encoder setBuffer:gridB offset:0 atIndex:1];
					[encoder setBuffer:gridParameterBuffer offset:0 atIndex:2];
					Dispatch(encoder,context.scatterValues,valueCount);[encoder endEncoding];
					std::swap(gridA,gridB);
				}
				if( actualBytes>(UINT64_C(1)<<31u) ) {
					if( structuredError ) *structuredError=
						"production resident palindrome working set exceeds two GiB";
					return false;
				}
				CommitTrackedMetalCommand(command);[command waitUntilCompleted];
				if( [command status]!=MTLCommandBufferStatusCompleted ) {
					if( structuredError ) *structuredError=MetalError(
						"production resident palindrome command failed",[command error]);
					return false;
				}
				const std::uint64_t commits=MetalCommandCommitCount-beginningCommandCommitCount;
				const std::uint64_t reads=MetalHostBufferReadCount-beginningHostBufferReadCount;
				if( commits!=1u||reads!=0u ) {
					if( structuredError ) *structuredError=
						"production resident palindrome transfer topology changed";
					return false;
				}
				FireProductionMetalCellPalindromeResidentResult computed;
				computed.conservativeValues=gridA;
				computed.acceptedGasMassDoseKGPerM2=acceptedGasMassDose;
				computed.executedSubmapCount=5u;
				computed.commandCommitCount=1u;computed.interstageFullGridTransferCount=0u;
				computed.actualMetalAllocationBytes=actualBytes;
				computed.deviceElapsedMS=([command GPUEndTime]-[command GPUStartTime])*1000.0;
				computed.deviceStartTimeS=[command GPUStartTime];
				computed.deviceEndTimeS=[command GPUEndTime];
				if( !std::isfinite(computed.deviceElapsedMS) ) return false;
				result=std::move(computed);
			}
			if( structuredError ) structuredError->clear();return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionMetalCellPalindromeResidentResult();
			if( structuredError ) try {
				*structuredError="production resident palindrome allocation failed";
			} catch( const std::bad_alloc& ) {}
			return false;
		}
	}

	bool RemapFireProductionCellPalindromeMetalResident(
		const FireProductionCellPalindromeRequest& request,
		const FireProductionMetalCellPalindromeResidentInput& input,
		FireProductionMetalCellPalindromeResidentResult& result,
		std::string* structuredError )
	{
		return RemapFireProductionCellPalindromeMetalResidentImpl(
			request,input,result,false,structuredError);
	}

	bool RemapFireProductionCellPalindromeMetalResidentComparator(
		const FireProductionCellPalindromeRequest& request,
		FireProductionCellPalindromeResult& result, std::string* structuredError )
	{
		result=FireProductionCellPalindromeResult();
		try {
			if( !ValidateFireProductionCellPalindromeRequest(request,structuredError) ) return false;
			MetalRemapContext& context=Context();
			if( !context.Valid() ) {
				if( structuredError ) *structuredError=context.error;
				return false;
			}
			@autoreleasepool {
				const std::size_t cells=request.shape.CellCount();
				const std::size_t valueCount=request.componentCount*cells;
				const std::size_t valueBytes=valueCount*sizeof(float);
				std::array<std::size_t,3> faceCounts;
				for( unsigned int axis=0u;axis<3u;++axis )
					faceCounts[axis]=FireProductionProjectionFaceCount(request.shape,axis);
				id<MTLBuffer> inputStage=[context.device newBufferWithBytes:
					request.conservativeValues.data() length:valueBytes
					options:MTLResourceStorageModeShared];
				std::array<id<MTLBuffer>,3> velocityStage,velocityPrivate;
				for( unsigned int axis=0u;axis<3u;++axis ) {
					velocityStage[axis]=[context.device newBufferWithBytes:
						request.frozenVelocityMPerS[axis].data()
						length:faceCounts[axis]*sizeof(float)
						options:MTLResourceStorageModeShared];
					velocityPrivate[axis]=[context.device newBufferWithLength:
						faceCounts[axis]*sizeof(float) options:MTLResourceStorageModePrivate];
				}
				id<MTLBuffer> inputPrivate=[context.device newBufferWithLength:valueBytes
					options:MTLResourceStorageModePrivate];
				id<MTLBuffer> ambient=[context.device newBufferWithBytes:request.ambientValues.data()
					length:request.componentCount*sizeof(float)
					options:MTLResourceStorageModeShared];
				if( !inputStage||!inputPrivate||!ambient ) return false;
				for( unsigned int axis=0u;axis<3u;++axis )
					if( !velocityStage[axis]||!velocityPrivate[axis] ) return false;
				id<MTLCommandBuffer> upload=TrackedMetalCommandBuffer(context.queue);
				id<MTLBlitCommandEncoder> blit=upload?[upload blitCommandEncoder]:nil;
				if( !blit ) return false;
				[blit copyFromBuffer:inputStage sourceOffset:0 toBuffer:inputPrivate
					destinationOffset:0 size:valueBytes];
				for( unsigned int axis=0u;axis<3u;++axis )
					[blit copyFromBuffer:velocityStage[axis] sourceOffset:0
						toBuffer:velocityPrivate[axis] destinationOffset:0
						size:faceCounts[axis]*sizeof(float)];
				[blit endEncoding];CommitTrackedMetalCommand(upload);[upload waitUntilCompleted];
				if( [upload status]!=MTLCommandBufferStatusCompleted ) return false;
				FireProductionMetalCellPalindromeResidentInput residentInput;
				residentInput.conservativeValues=inputPrivate;
				residentInput.frozenVelocityMPerS=velocityPrivate;
				residentInput.ambientValues=ambient;
				FireProductionMetalCellPalindromeResidentResult resident;
				if( !RemapFireProductionCellPalindromeMetalResident(request,residentInput,
					resident,structuredError) ) return false;
				id<MTLBuffer> outputStage=[context.device newBufferWithLength:valueBytes
					options:MTLResourceStorageModeShared];
				id<MTLCommandBuffer> staging=TrackedMetalCommandBuffer(context.queue);
				blit=staging?[staging blitCommandEncoder]:nil;
				if( !outputStage||!blit ) return false;
				[blit copyFromBuffer:resident.conservativeValues sourceOffset:0
					toBuffer:outputStage destinationOffset:0 size:valueBytes];
				[blit endEncoding];CommitTrackedMetalCommand(staging);[staging waitUntilCompleted];
				if( [staging status]!=MTLCommandBufferStatusCompleted ) return false;
				const float* output=static_cast<const float*>(ReadTrackedMetalBuffer(outputStage));
				FireProductionCellPalindromeResult computed;
				computed.conservativeValues.assign(output,output+valueCount);
				computed.executedSubmapCount=resident.executedSubmapCount;
				computed.commandCommitCount=resident.commandCommitCount;
				computed.interstageFullGridReadbackCount=
					resident.interstageFullGridTransferCount;
				computed.actualTrackedWorkingSetBytes=resident.actualMetalAllocationBytes;
				if( !FireProductionCellPalindromeWorkingSetBytes(request.shape,
					request.componentCount,computed.certifiedWorkingSetBytes,
					request.retainAcceptedGasMassDose) ) return false;
				computed.deviceElapsedMS=resident.deviceElapsedMS;
				if( !AllFinite(computed.conservativeValues)||
					!std::isfinite(computed.deviceElapsedMS) ) return false;
				result=std::move(computed);
			}
			if( structuredError ) structuredError->clear();return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionCellPalindromeResult();
			if( structuredError ) try {
				*structuredError="production resident palindrome comparator allocation failed";
			} catch( const std::bad_alloc& ) {}
			return false;
		}
	}

	bool RemapFireProductionPeriodicDualMomentumMetalResident(
		const FireProductionPeriodicDualMomentumRequest& request,
		const FireProductionMetalPeriodicDualMomentumResidentInput& input,
		FireProductionMetalPeriodicDualMomentumResidentResult& result,
		std::string* structuredError )
	{
		result=FireProductionMetalPeriodicDualMomentumResidentResult();
		try {
			std::uint64_t certifiedBytes=0u;
			if( !FireProductionPeriodicDualMomentumResidentWorkingSetBytes(request.shape,
				certifiedBytes)||certifiedBytes>(UINT64_C(1)<<31u) ) {
				if( structuredError ) *structuredError=
					"production resident periodic dual working set exceeds two GiB";
				return false;
			}
			std::array<FireProductionCellPalindromeRequest,3> dualRequest;
			for( unsigned int component=0u;component<3u;++component ) if(
				!BuildFireProductionPeriodicDualCellRequest(request,component,
					dualRequest[component],structuredError) ) return false;
			MetalRemapContext& context=Context();
			if( !context.Valid() ) {
				if( structuredError ) *structuredError=context.error;
				return false;
			}
			@autoreleasepool {
				std::array<std::size_t,3> faceCounts;
				for( unsigned int axis=0u;axis<3u;++axis ) {
					faceCounts[axis]=FireProductionProjectionFaceCount(request.shape,axis);
					const id<MTLBuffer> buffers[]={input.beginningFaceDensity[axis],
						input.beginningMomentum[axis],input.frozenVelocityMPerS[axis]};
					for( id<MTLBuffer> buffer : buffers ) if( !buffer||
						[buffer storageMode]!=MTLStorageModePrivate||
						[buffer length]<faceCounts[axis]*sizeof(float) ) {
						if( structuredError ) *structuredError=
							"production resident periodic dual input ownership is invalid";
						return false;
					}
				}
				const std::size_t cells=request.shape.CellCount();
				const std::size_t dualBytes=2u*cells*sizeof(float);
				std::uint64_t borrowedBytes=0u,outputBytes=0u,maximumActual=0u;
				auto addAllocation=[](id<MTLBuffer> buffer,std::uint64_t& total) {
					const std::uint64_t bytes=static_cast<std::uint64_t>([buffer allocatedSize]);
					if( total>std::numeric_limits<std::uint64_t>::max()-bytes ) return false;
					total+=bytes;return true;
				};
				for( unsigned int axis=0u;axis<3u;++axis ) if(
					!addAllocation(input.beginningFaceDensity[axis],borrowedBytes)||
					!addAllocation(input.beginningMomentum[axis],borrowedBytes)||
					!addAllocation(input.frozenVelocityMPerS[axis],borrowedBytes) ) return false;
				FireProductionMetalPeriodicDualMomentumResidentResult computed;
				for( unsigned int component=0u;component<3u;++component ) {
					computed.auxiliaryFaceDensity[component]=[context.device newBufferWithLength:
						faceCounts[component]*sizeof(float) options:MTLResourceStorageModePrivate];
					computed.momentum[component]=[context.device newBufferWithLength:
						faceCounts[component]*sizeof(float) options:MTLResourceStorageModePrivate];
					if( !computed.auxiliaryFaceDensity[component]||!computed.momentum[component]||
						!addAllocation(computed.auxiliaryFaceDensity[component],outputBytes)||
						!addAllocation(computed.momentum[component],outputBytes) ) return false;
				}
				const std::uint64_t beginningCommits=MetalCommandCommitCount;
				const std::uint64_t beginningReads=MetalHostBufferReadCount;
				double deviceMS=0.0;
				for( unsigned int component=0u;component<3u;++component ) {
					id<MTLBuffer> dualInput=[context.device newBufferWithLength:dualBytes
						options:MTLResourceStorageModePrivate];
					std::array<id<MTLBuffer>,3> carrier;
					std::array<id<MTLBuffer>,3> parameter;
					for( unsigned int sweep=0u;sweep<3u;++sweep ) {
						carrier[sweep]=[context.device newBufferWithLength:
							faceCounts[sweep]*sizeof(float) options:MTLResourceStorageModePrivate];
						const MetalPeriodicDualParameters values={
							static_cast<std::uint32_t>(request.shape.nx),
							static_cast<std::uint32_t>(request.shape.ny),
							static_cast<std::uint32_t>(request.shape.nz),component,sweep};
						parameter[sweep]=[context.device newBufferWithBytes:&values length:sizeof(values)
							options:MTLResourceStorageModeShared];
					}
					const float ambientValues[]={0.0f,0.0f};
					id<MTLBuffer> ambient=[context.device newBufferWithBytes:ambientValues
						length:sizeof(ambientValues) options:MTLResourceStorageModeShared];
					if( !dualInput||!ambient ) return false;
					std::uint64_t packingBytes=0u;
					if( !addAllocation(dualInput,packingBytes)||!addAllocation(ambient,packingBytes) )
						return false;
					for( unsigned int sweep=0u;sweep<3u;++sweep ) if( !carrier[sweep]||
						!parameter[sweep]||!addAllocation(carrier[sweep],packingBytes)||
						!addAllocation(parameter[sweep],packingBytes) ) return false;
					id<MTLCommandBuffer> pack=TrackedMetalCommandBuffer(context.queue);
					id<MTLBlitCommandEncoder> blit=pack?[pack blitCommandEncoder]:nil;
					if( !blit ) return false;
					[blit copyFromBuffer:input.beginningFaceDensity[component] sourceOffset:0
						toBuffer:computed.auxiliaryFaceDensity[component] destinationOffset:0
						size:faceCounts[component]*sizeof(float)];
					[blit copyFromBuffer:input.beginningMomentum[component] sourceOffset:0
						toBuffer:computed.momentum[component] destinationOffset:0
						size:faceCounts[component]*sizeof(float)];[blit endEncoding];
					id<MTLComputeCommandEncoder> encoder=[pack computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:computed.auxiliaryFaceDensity[component] offset:0 atIndex:0];
					[encoder setBuffer:computed.momentum[component] offset:0 atIndex:1];
					[encoder setBuffer:dualInput offset:0 atIndex:2];
					[encoder setBuffer:parameter[0] offset:0 atIndex:3];
					Dispatch(encoder,context.gatherPeriodicDualValues,2u*cells);[encoder endEncoding];
					for( unsigned int sweep=0u;sweep<3u;++sweep ) {
						encoder=[pack computeCommandEncoder];if( !encoder ) return false;
						[encoder setBuffer:input.frozenVelocityMPerS[0] offset:0 atIndex:0];
						[encoder setBuffer:input.frozenVelocityMPerS[1] offset:0 atIndex:1];
						[encoder setBuffer:input.frozenVelocityMPerS[2] offset:0 atIndex:2];
						[encoder setBuffer:carrier[sweep] offset:0 atIndex:3];
						[encoder setBuffer:parameter[sweep] offset:0 atIndex:4];
						Dispatch(encoder,context.gatherPeriodicDualCarrier,faceCounts[sweep]);
						[encoder endEncoding];
					}
					CommitTrackedMetalCommand(pack);[pack waitUntilCompleted];
					if( [pack status]!=MTLCommandBufferStatusCompleted ) return false;
					deviceMS+=([pack GPUEndTime]-[pack GPUStartTime])*1000.0;
					FireProductionMetalCellPalindromeResidentInput cellInput;
					cellInput.conservativeValues=dualInput;cellInput.frozenVelocityMPerS=carrier;
					cellInput.ambientValues=ambient;
					FireProductionMetalCellPalindromeResidentResult cellResult;
					if( !RemapFireProductionCellPalindromeMetalResident(dualRequest[component],
						cellInput,cellResult,structuredError) ) return false;
					deviceMS+=cellResult.deviceElapsedMS;
					id<MTLCommandBuffer> scatter=TrackedMetalCommandBuffer(context.queue);
					encoder=scatter?[scatter computeCommandEncoder]:nil;if( !encoder ) return false;
					[encoder setBuffer:cellResult.conservativeValues offset:0 atIndex:0];
					[encoder setBuffer:computed.auxiliaryFaceDensity[component] offset:0 atIndex:1];
					[encoder setBuffer:computed.momentum[component] offset:0 atIndex:2];
					[encoder setBuffer:parameter[0] offset:0 atIndex:3];
					Dispatch(encoder,context.scatterPeriodicDualValues,2u*cells);[encoder endEncoding];
					encoder=[scatter computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:computed.auxiliaryFaceDensity[component] offset:0 atIndex:0];
					[encoder setBuffer:computed.momentum[component] offset:0 atIndex:1];
					[encoder setBuffer:parameter[0] offset:0 atIndex:2];
					const std::size_t seamCount=component==0u?request.shape.ny*request.shape.nz:
						(component==1u?request.shape.nx*request.shape.nz:
						 request.shape.nx*request.shape.ny);
					Dispatch(encoder,context.publishPeriodicDualSeam,seamCount);[encoder endEncoding];
					CommitTrackedMetalCommand(scatter);[scatter waitUntilCompleted];
					if( [scatter status]!=MTLCommandBufferStatusCompleted ) return false;
					deviceMS+=([scatter GPUEndTime]-[scatter GPUStartTime])*1000.0;
					if( borrowedBytes>std::numeric_limits<std::uint64_t>::max()-outputBytes||
						borrowedBytes+outputBytes>std::numeric_limits<std::uint64_t>::max()-
							packingBytes||borrowedBytes+outputBytes+packingBytes>
							std::numeric_limits<std::uint64_t>::max()-cellResult.actualMetalAllocationBytes )
						return false;
					maximumActual=std::max(maximumActual,borrowedBytes+outputBytes+packingBytes+
						cellResult.actualMetalAllocationBytes);
				}
				const std::uint64_t commits=MetalCommandCommitCount-beginningCommits;
				const std::uint64_t reads=MetalHostBufferReadCount-beginningReads;
				if( commits!=9u||reads!=0u||maximumActual>certifiedBytes||
					maximumActual>(UINT64_C(1)<<31u) ) return false;
				computed.executedSubmapCount=15u;computed.commandCommitCount=9u;
				computed.interstageFullGridTransferCount=0u;
				computed.actualMetalAllocationBytes=maximumActual;
				computed.deviceElapsedMS=deviceMS;
				result=std::move(computed);
			}
			if( structuredError ) structuredError->clear();return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionMetalPeriodicDualMomentumResidentResult();
			if( structuredError ) try {
				*structuredError="production resident periodic dual allocation failed";
			} catch( const std::bad_alloc& ) {}
			return false;
		}
	}

	bool RemapFireProductionPeriodicDualMomentumMetal(
		const FireProductionPeriodicDualMomentumRequest& request,
		FireProductionPeriodicDualMomentumResult& result, std::string* structuredError )
	{
		result=FireProductionPeriodicDualMomentumResult();
		try {
			std::uint64_t certifiedBytes=0u;
			if( !FireProductionPeriodicDualMomentumResidentWorkingSetBytes(request.shape,
				certifiedBytes)||certifiedBytes>(UINT64_C(1)<<31u) ) {
				if( structuredError ) *structuredError=
					"production resident periodic dual working set exceeds two GiB";
				return false;
			}
			FireProductionCellPalindromeRequest validated;
			if( !BuildFireProductionPeriodicDualCellRequest(request,0u,validated,
				structuredError) ) return false;
			MetalRemapContext& context=Context();if( !context.Valid() ) return false;
			@autoreleasepool {
				std::array<std::size_t,3> faceCounts;
				FireProductionMetalPeriodicDualMomentumResidentInput residentInput;
				std::array<std::array<id<MTLBuffer>,3>,3> stage;
				for( unsigned int axis=0u;axis<3u;++axis ) {
					faceCounts[axis]=FireProductionProjectionFaceCount(request.shape,axis);
					const std::vector<float>* values[]={&request.beginningFaceDensity[axis],
						&request.beginningMomentum[axis],&request.frozenVelocityMPerS[axis]};
					for( unsigned int role=0u;role<3u;++role ) {
						stage[axis][role]=[context.device newBufferWithBytes:values[role]->data()
							length:faceCounts[axis]*sizeof(float) options:MTLResourceStorageModeShared];
						id<MTLBuffer> privateValue=[context.device newBufferWithLength:
							faceCounts[axis]*sizeof(float)
							options:MTLResourceStorageModePrivate];
						if( role==0u ) residentInput.beginningFaceDensity[axis]=privateValue;
						else if( role==1u ) residentInput.beginningMomentum[axis]=privateValue;
						else residentInput.frozenVelocityMPerS[axis]=privateValue;
						if( !stage[axis][role]||!privateValue ) return false;
					}
				}
				id<MTLCommandBuffer> upload=TrackedMetalCommandBuffer(context.queue);
				id<MTLBlitCommandEncoder> blit=upload?[upload blitCommandEncoder]:nil;if( !blit ) return false;
				for( unsigned int axis=0u;axis<3u;++axis ) {
					id<MTLBuffer> resident[]={residentInput.beginningFaceDensity[axis],
						residentInput.beginningMomentum[axis],residentInput.frozenVelocityMPerS[axis]};
					for( unsigned int role=0u;role<3u;++role )
						[blit copyFromBuffer:stage[axis][role] sourceOffset:0 toBuffer:resident[role]
							destinationOffset:0 size:faceCounts[axis]*sizeof(float)];
				}
				[blit endEncoding];CommitTrackedMetalCommand(upload);[upload waitUntilCompleted];
				if( [upload status]!=MTLCommandBufferStatusCompleted ) return false;
				FireProductionMetalPeriodicDualMomentumResidentResult resident;
				if( !RemapFireProductionPeriodicDualMomentumMetalResident(request,residentInput,
					resident,structuredError) ) return false;
				std::array<std::array<id<MTLBuffer>,2>,3> outputStage;
				id<MTLCommandBuffer> staging=TrackedMetalCommandBuffer(context.queue);
				blit=staging?[staging blitCommandEncoder]:nil;if( !blit ) return false;
				for( unsigned int axis=0u;axis<3u;++axis ) {
					outputStage[axis][0]=[context.device newBufferWithLength:
						faceCounts[axis]*sizeof(float) options:MTLResourceStorageModeShared];
					outputStage[axis][1]=[context.device newBufferWithLength:
						faceCounts[axis]*sizeof(float) options:MTLResourceStorageModeShared];
					if( !outputStage[axis][0]||!outputStage[axis][1] ) return false;
					[blit copyFromBuffer:resident.auxiliaryFaceDensity[axis] sourceOffset:0
						toBuffer:outputStage[axis][0] destinationOffset:0
						size:faceCounts[axis]*sizeof(float)];
					[blit copyFromBuffer:resident.momentum[axis] sourceOffset:0
						toBuffer:outputStage[axis][1] destinationOffset:0
						size:faceCounts[axis]*sizeof(float)];
				}
				[blit endEncoding];CommitTrackedMetalCommand(staging);[staging waitUntilCompleted];
				if( [staging status]!=MTLCommandBufferStatusCompleted ) return false;
				FireProductionPeriodicDualMomentumResult computed;
				for( unsigned int axis=0u;axis<3u;++axis ) {
					const float* density=static_cast<const float*>(
						ReadTrackedMetalBuffer(outputStage[axis][0]));
					const float* momentum=static_cast<const float*>(
						ReadTrackedMetalBuffer(outputStage[axis][1]));
					computed.auxiliaryFaceDensity[axis].assign(density,density+faceCounts[axis]);
					computed.momentum[axis].assign(momentum,momentum+faceCounts[axis]);
				}
				computed.executedSubmapCount=resident.executedSubmapCount;
				computed.canonicalSeamCopyCount=static_cast<std::uint32_t>(2u*(
					request.shape.ny*request.shape.nz+request.shape.nx*request.shape.nz+
					request.shape.nx*request.shape.ny));
				computed.commandCommitCount=resident.commandCommitCount;
				computed.interstageFullGridTransferCount=resident.interstageFullGridTransferCount;
				computed.actualMetalAllocationBytes=resident.actualMetalAllocationBytes;
				computed.deviceElapsedMS=resident.deviceElapsedMS;
				for( unsigned int axis=0u;axis<3u;++axis ) if(
					!AllFinite(computed.auxiliaryFaceDensity[axis])||
					!AllFinite(computed.momentum[axis]) ) return false;
				if( !std::isfinite(computed.deviceElapsedMS) ) return false;
				result=std::move(computed);
			}
			if( structuredError ) structuredError->clear();return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionPeriodicDualMomentumResult();
			if( structuredError ) try {
				*structuredError="production periodic dual Metal allocation failed";
			} catch( const std::bad_alloc& ) {}
			return false;
		}
	}

	bool PrepareFireProductionDualMomentumMetalStaticState(
		const FireProductionDualMomentumRequest& request,
		FireProductionMetalDualMomentumStaticState& state,
		std::string* structuredError )
	{
		state=FireProductionMetalDualMomentumStaticState();
		try {
			std::uint64_t certifiedBytes=0u;
			if( !FireProductionDualMomentumResidentWorkingSetBytes(request.shape,
				request.boundary,certifiedBytes)||certifiedBytes>(UINT64_C(1)<<31u) ) {
				if( structuredError ) *structuredError=
					"production resident dual working set exceeds two GiB";
				return false;
			}
			if( !ValidateDualMomentumStaticOwnerMetadata(request,structuredError) ) {
				if( structuredError ) *structuredError=
					"production resident dual static metadata is invalid";
				return false;
			}
			MetalRemapContext& context=Context();
			if( !context.Valid() ) {
				if( structuredError ) *structuredError=context.error;return false;
			}
			@autoreleasepool {
				const float steps[]={0.5f*request.timeStepS,0.5f*request.timeStepS,
					request.timeStepS};
				std::array<std::array<FireProductionRemapRequest,3>,3> packed;
				std::array<std::string,9> packErrors;
				std::array<bool,9> packSucceeded={{false,false,false,false,false,false,false,false,false}};
				auto packTask=[&](const unsigned int task){
					const unsigned int component=task/3u,sweep=task%3u;
					packSucceeded[task]=BuildFireProductionDualAxisRequest(request,component,sweep,
						steps[sweep],request.beginningFaceDensity[component],
						request.beginningMomentum[component],packed[component][sweep],
						&packErrors[task]);
				};
				const char* audit=std::getenv("RISE_FIRE_TIMESTEP_VELOCITY_AUDIT");
				const char* packMode=std::getenv("RISE_FIRE_TIMESTEP_VELOCITY_PACK_MODE");
				const bool serialPack=audit&&std::strcmp(audit,"1")==0&&packMode&&
					std::strcmp(packMode,"serial")==0;
				// Legacy low-priority workers do not steal while waiting, so nested
				// ParallelFor is not recursion-safe when a render pool is saturated.
				const bool legacyLowPriority=GlobalOptions().ReadBool(
					"force_all_threads_low_priority",false);
				if( FireProductionDualLayoutPackRequiresSerialOwner(
					serialPack,legacyLowPriority) )
					for(unsigned int task=0u;task<9u;++task)packTask(task);
				else Implementation::GlobalThreadPool().ParallelFor(9u,packTask);
				for(unsigned int task=0u;task<packSucceeded.size();++task)if(!packSucceeded[task]){
					if(structuredError)*structuredError=packErrors[task];return false;}
				std::array<std::array<id<MTLBuffer>,3>,3> velocityStage,lowerStage,upperStage;
				FireProductionMetalDualMomentumStaticState computed;
				computed.shape=request.shape;computed.timeStepS=request.timeStepS;
				computed.boundary=request.boundary;
				std::uint64_t actual=0u;
				auto add=[&](id<MTLBuffer> buffer) {
					if( !buffer ) return false;
					const std::uint64_t bytes=static_cast<std::uint64_t>([buffer allocatedSize]);
					if( actual>std::numeric_limits<std::uint64_t>::max()-bytes ) return false;
					actual+=bytes;return true;
				};
				for( unsigned int component=0u;component<3u;++component )
					for( unsigned int sweep=0u;sweep<3u;++sweep ) {
						const FireProductionRemapRequest& line=packed[component][sweep];
						FireProductionMetalDualAxisStaticState& output=computed.axis[component][sweep];
						output.lineLength=static_cast<std::uint32_t>(line.lineLength);
						output.lineCount=static_cast<std::uint32_t>(line.lineCount);
						output.lowerBoundary=static_cast<std::uint32_t>(line.lowerBoundary);
						output.upperBoundary=static_cast<std::uint32_t>(line.upperBoundary);
						output.ambientPerLine=line.lineSpecificAmbientValues?1u:0u;
						const std::vector<float>& lower=line.lineSpecificAmbientValues?
							line.lowerAmbientValues:line.ambientValues;
						const std::vector<float>& upper=line.lineSpecificAmbientValues?
							line.upperAmbientValues:line.ambientValues;
						velocityStage[component][sweep]=[context.device newBufferWithBytes:
							line.faceVelocityMPerS.data() length:line.faceVelocityMPerS.size()*sizeof(float)
							options:MTLResourceStorageModeShared];
						lowerStage[component][sweep]=[context.device newBufferWithBytes:lower.data()
							length:lower.size()*sizeof(float) options:MTLResourceStorageModeShared];
						upperStage[component][sweep]=[context.device newBufferWithBytes:upper.data()
							length:upper.size()*sizeof(float) options:MTLResourceStorageModeShared];
						output.faceVelocityMPerS=[context.device newBufferWithLength:
							line.faceVelocityMPerS.size()*sizeof(float) options:MTLResourceStorageModePrivate];
						output.lowerAmbientValues=[context.device newBufferWithLength:
							lower.size()*sizeof(float) options:MTLResourceStorageModePrivate];
						output.upperAmbientValues=[context.device newBufferWithLength:
							upper.size()*sizeof(float) options:MTLResourceStorageModePrivate];
						if( !velocityStage[component][sweep]||!lowerStage[component][sweep]||
							!upperStage[component][sweep]||!add(output.faceVelocityMPerS)||
							!add(output.lowerAmbientValues)||!add(output.upperAmbientValues) ) return false;
					}
				id<MTLCommandBuffer> upload=TrackedMetalCommandBuffer(context.queue);
				id<MTLBlitCommandEncoder> blit=upload?[upload blitCommandEncoder]:nil;
				if( !blit ) return false;
				for( unsigned int component=0u;component<3u;++component )
					for( unsigned int sweep=0u;sweep<3u;++sweep ) {
						const FireProductionRemapRequest& line=packed[component][sweep];
						const std::vector<float>& lower=line.lineSpecificAmbientValues?
							line.lowerAmbientValues:line.ambientValues;
						const std::vector<float>& upper=line.lineSpecificAmbientValues?
							line.upperAmbientValues:line.ambientValues;
						const FireProductionMetalDualAxisStaticState& output=computed.axis[component][sweep];
						[blit copyFromBuffer:velocityStage[component][sweep] sourceOffset:0
							toBuffer:output.faceVelocityMPerS destinationOffset:0
							size:line.faceVelocityMPerS.size()*sizeof(float)];
						[blit copyFromBuffer:lowerStage[component][sweep] sourceOffset:0
							toBuffer:output.lowerAmbientValues destinationOffset:0
							size:lower.size()*sizeof(float)];
						[blit copyFromBuffer:upperStage[component][sweep] sourceOffset:0
							toBuffer:output.upperAmbientValues destinationOffset:0
							size:upper.size()*sizeof(float)];
					}
				[blit endEncoding];CommitTrackedMetalCommand(upload);[upload waitUntilCompleted];
				if( [upload status]!=MTLCommandBufferStatusCompleted ) return false;
				const char* velocityAudit=std::getenv("RISE_FIRE_TIMESTEP_VELOCITY_AUDIT");
				if( velocityAudit&&std::strcmp(velocityAudit,"1")==0 ) std::fprintf(stderr,
					"TIMESTEP_VELOCITY_DEVICE dual_static_upload=%.9g\n",
					([upload GPUEndTime]-[upload GPUStartTime])*1000.0);
				computed.uploadCommandCommitCount=1u;computed.actualMetalAllocationBytes=actual;
				state=std::move(computed);
			}
			if( structuredError ) structuredError->clear();return true;
		} catch( const std::bad_alloc& ) {
			state=FireProductionMetalDualMomentumStaticState();
			if( structuredError ) try { *structuredError=
				"production dual static-state allocation failed"; } catch( const std::bad_alloc& ) {}
			return false;
		}
	}

	bool RemapFireProductionDualMomentumMetalResident(
		const FireProductionDualMomentumRequest& request,
		const FireProductionMetalDualMomentumStaticState& staticState,
		const FireProductionMetalDualMomentumResidentInput& input,
		FireProductionMetalDualMomentumResidentResult& result,
		std::string* structuredError )
	{
		result=FireProductionMetalDualMomentumResidentResult();
		try {
			std::uint64_t certifiedBytes=0u;
			if( !FireProductionDualMomentumResidentWorkingSetBytes(request.shape,
				request.boundary,certifiedBytes)||certifiedBytes>(UINT64_C(1)<<31u) ) {
				if( structuredError ) *structuredError=
					"production resident dual working set exceeds two GiB";
				return false;
			}
			if( staticState.shape.nx!=request.shape.nx||staticState.shape.ny!=request.shape.ny||
				staticState.shape.nz!=request.shape.nz||
				staticState.shape.cellWidthM!=request.shape.cellWidthM||
				staticState.timeStepS!=request.timeStepS||staticState.boundary!=request.boundary ) {
				if( structuredError ) *structuredError=
					"production resident dual static ownership does not match the request";
				return false;
			}
			MetalRemapContext& context=Context();
			if( !context.Valid() ) { if( structuredError ) *structuredError=context.error;return false; }
			@autoreleasepool {
				std::array<std::size_t,3> faceCounts,canonicalOffset;
				std::size_t allFaces=0u;
				for( unsigned int axis=0u;axis<3u;++axis ) {
					faceCounts[axis]=FireProductionProjectionFaceCount(request.shape,axis);
					canonicalOffset[axis]=allFaces*sizeof(float);allFaces+=faceCounts[axis];
				}
				const std::size_t packedBytes=allFaces*sizeof(float);
				if( !input.packedFaceDensity||!input.packedMomentum||
					[input.packedFaceDensity storageMode]!=MTLStorageModePrivate||
					[input.packedMomentum storageMode]!=MTLStorageModePrivate||
					[input.packedFaceDensity length]<packedBytes||[input.packedMomentum length]<packedBytes||
					input.faceByteOffset!=canonicalOffset ) return false;
				std::size_t maximumValues=0u,maximumFaces=0u,maximumCells=0u;
				for( unsigned int component=0u;component<3u;++component )
					for( unsigned int sweep=0u;sweep<3u;++sweep ) {
						const FireProductionMetalDualAxisStaticState& axis=staticState.axis[component][sweep];
						const std::size_t values=2u*axis.lineLength*axis.lineCount;
						const std::size_t faces=axis.lineCount*(axis.lineLength+1u);
						const std::size_t ambient=axis.ambientPerLine?2u*axis.lineCount:2u;
						if( axis.lineLength<4u||axis.lineCount==0u||!axis.faceVelocityMPerS||
							!axis.lowerAmbientValues||!axis.upperAmbientValues||
							[axis.faceVelocityMPerS storageMode]!=MTLStorageModePrivate||
							[axis.lowerAmbientValues storageMode]!=MTLStorageModePrivate||
							[axis.upperAmbientValues storageMode]!=MTLStorageModePrivate||
							[axis.faceVelocityMPerS length]<faces*sizeof(float)||
							[axis.lowerAmbientValues length]<ambient*sizeof(float)||
							[axis.upperAmbientValues length]<ambient*sizeof(float) ) return false;
						maximumValues=std::max(maximumValues,values);
						maximumFaces=std::max(maximumFaces,faces);
						maximumCells=std::max(maximumCells,
							static_cast<std::size_t>(axis.lineLength)*axis.lineCount);
					}
				auto privateBuffer=[&](std::size_t bytes) { return [context.device
					newBufferWithLength:bytes options:MTLResourceStorageModePrivate]; };
				id<MTLBuffer> density=privateBuffer(packedBytes),momentum=privateBuffer(packedBytes);
				id<MTLBuffer> lineValues=privateBuffer(maximumValues*sizeof(float));
				id<MTLBuffer> lineUpdated=privateBuffer(maximumValues*sizeof(float));
				id<MTLBuffer> left=privateBuffer(maximumValues*sizeof(float));
				id<MTLBuffer> right=privateBuffer(maximumValues*sizeof(float));
				id<MTLBuffer> alpha=privateBuffer(maximumCells*sizeof(float));
				id<MTLBuffer> prefix=privateBuffer(2u*maximumFaces*sizeof(float));
				id<MTLBuffer> flux=privateBuffer(2u*maximumFaces*sizeof(float));
				const id<MTLBuffer> owned[]={density,momentum,lineValues,lineUpdated,left,right,
					alpha,prefix,flux};
				std::uint64_t actual=staticState.actualMetalAllocationBytes;
				auto add=[&](id<MTLBuffer> buffer,MTLStorageMode mode) {
					if( !buffer||[buffer storageMode]!=mode ) return false;
					const std::uint64_t bytes=static_cast<std::uint64_t>([buffer allocatedSize]);
					if( actual>std::numeric_limits<std::uint64_t>::max()-bytes ) return false;
					actual+=bytes;return true;
				};
				if( !add(input.packedFaceDensity,MTLStorageModePrivate)||
					!add(input.packedMomentum,MTLStorageModePrivate) ) return false;
				for( id<MTLBuffer> buffer : owned ) if( !add(buffer,MTLStorageModePrivate) ) return false;
				const std::uint64_t beginningCommits=MetalCommandCommitCount;
				const std::uint64_t beginningReads=MetalHostBufferReadCount;
				id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context.queue);
				id<MTLBlitCommandEncoder> blit=command?[command blitCommandEncoder]:nil;
				if( !blit ) return false;
				[blit copyFromBuffer:input.packedFaceDensity sourceOffset:0 toBuffer:density
					destinationOffset:0 size:packedBytes];
				[blit copyFromBuffer:input.packedMomentum sourceOffset:0 toBuffer:momentum
					destinationOffset:0 size:packedBytes];[blit endEncoding];
				const unsigned int axes[]={0u,1u,2u,1u,0u};
				const float step[]={0.5f*request.timeStepS,0.5f*request.timeStepS,request.timeStepS};
				for( unsigned int component=0u;component<3u;++component ) {
					const bool periodic=request.boundary[2u*component]==FireProductionProjectionPeriodic;
					const std::size_t componentBeginning=periodic?0u:
						(request.boundary[2u*component]==FireProductionProjectionWall?1u:0u);
					const MetalDualLineParameters wallParameters={
						static_cast<std::uint32_t>(request.shape.nx),static_cast<std::uint32_t>(request.shape.ny),
						static_cast<std::uint32_t>(request.shape.nz),component,component,0u,0u,
						static_cast<std::uint32_t>(componentBeginning),periodic?1u:0u,
						request.boundary[2u*component]==FireProductionProjectionWall?1u:0u,
						request.boundary[2u*component+1u]==FireProductionProjectionWall?1u:0u};
					id<MTLBuffer> wallParameter=[context.device newBufferWithBytes:&wallParameters
						length:sizeof(wallParameters) options:MTLResourceStorageModeShared];
					if( !add(wallParameter,MTLStorageModeShared) ) return false;
					id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:momentum offset:canonicalOffset[component] atIndex:0];
					[encoder setBuffer:wallParameter offset:0 atIndex:1];
					Dispatch(encoder,context.prescribeDualComponentWalls,faceCounts[component]);
					[encoder endEncoding];
					for( unsigned int pass=0u;pass<5u;++pass ) {
						const unsigned int sweep=axes[pass];
						const FireProductionMetalDualAxisStaticState& axis=staticState.axis[component][sweep];
						const MetalDualLineParameters grid={
							static_cast<std::uint32_t>(request.shape.nx),static_cast<std::uint32_t>(request.shape.ny),
							static_cast<std::uint32_t>(request.shape.nz),component,sweep,axis.lineLength,
							axis.lineCount,static_cast<std::uint32_t>(componentBeginning),periodic?1u:0u,
							request.boundary[2u*component]==FireProductionProjectionWall?1u:0u,
							request.boundary[2u*component+1u]==FireProductionProjectionWall?1u:0u};
						const MetalParameters parameters={axis.lineLength,axis.lineCount,2u,
							axis.lowerBoundary,axis.upperBoundary,axis.ambientPerLine,
							request.shape.cellWidthM,step[sweep]};
						id<MTLBuffer> gridParameter=[context.device newBufferWithBytes:&grid length:sizeof(grid)
							options:MTLResourceStorageModeShared];
						id<MTLBuffer> parameter=[context.device newBufferWithBytes:&parameters length:sizeof(parameters)
							options:MTLResourceStorageModeShared];
						if( !add(gridParameter,MTLStorageModeShared)||!add(parameter,MTLStorageModeShared) ) return false;
						const std::size_t valueCount=2u*axis.lineLength*axis.lineCount;
						const std::size_t cellCount=axis.lineLength*axis.lineCount;
						const std::size_t faceCount=axis.lineCount*(axis.lineLength+1u);
						encoder=[command computeCommandEncoder];if( !encoder ) return false;
						[encoder setBuffer:density offset:canonicalOffset[component] atIndex:0];
						[encoder setBuffer:momentum offset:canonicalOffset[component] atIndex:1];
						[encoder setBuffer:lineValues offset:0 atIndex:2];[encoder setBuffer:gridParameter offset:0 atIndex:3];
						Dispatch(encoder,context.gatherDualLineValues,valueCount);[encoder endEncoding];
						encoder=[command computeCommandEncoder];if( !encoder ) return false;
						[encoder setBuffer:lineValues offset:0 atIndex:0];[encoder setBuffer:axis.faceVelocityMPerS offset:0 atIndex:1];
						[encoder setBuffer:axis.lowerAmbientValues offset:0 atIndex:2];[encoder setBuffer:axis.upperAmbientValues offset:0 atIndex:3];
						[encoder setBuffer:left offset:0 atIndex:4];[encoder setBuffer:right offset:0 atIndex:5];
						[encoder setBuffer:alpha offset:0 atIndex:6];[encoder setBuffer:parameter offset:0 atIndex:7];
						Dispatch(encoder,context.reconstruct,cellCount);[encoder endEncoding];
						const std::size_t padded=NextPowerOfTwo(axis.lineLength);
						encoder=[command computeCommandEncoder];
						if( !encoder||padded>static_cast<std::size_t>([context.scan maxTotalThreadsPerThreadgroup]) ) return false;
						[encoder setComputePipelineState:context.scan];[encoder setBuffer:lineValues offset:0 atIndex:0];
						[encoder setBuffer:prefix offset:0 atIndex:1];[encoder setBuffer:parameter offset:0 atIndex:2];
						[encoder setThreadgroupMemoryLength:padded*sizeof(float) atIndex:0];
						[encoder dispatchThreadgroups:MTLSizeMake(2u*axis.lineCount,1,1)
							threadsPerThreadgroup:MTLSizeMake(padded,1,1)];[encoder endEncoding];
						encoder=[command computeCommandEncoder];if( !encoder ) return false;
						[encoder setBuffer:lineValues offset:0 atIndex:0];[encoder setBuffer:axis.faceVelocityMPerS offset:0 atIndex:1];
						[encoder setBuffer:axis.lowerAmbientValues offset:0 atIndex:2];[encoder setBuffer:axis.upperAmbientValues offset:0 atIndex:3];
						[encoder setBuffer:left offset:0 atIndex:4];[encoder setBuffer:right offset:0 atIndex:5];
						[encoder setBuffer:prefix offset:0 atIndex:6];[encoder setBuffer:flux offset:0 atIndex:7];
						[encoder setBuffer:parameter offset:0 atIndex:8];
						const bool passPeriodic=axis.lowerBoundary==0u&&axis.upperBoundary==0u;
						Dispatch(encoder,context.flux,passPeriodic?2u*axis.lineCount*axis.lineLength:2u*faceCount);[encoder endEncoding];
						encoder=[command computeCommandEncoder];if( !encoder ) return false;
						[encoder setBuffer:lineValues offset:0 atIndex:0];[encoder setBuffer:flux offset:0 atIndex:1];
						[encoder setBuffer:lineUpdated offset:0 atIndex:2];[encoder setBuffer:parameter offset:0 atIndex:3];
						Dispatch(encoder,context.update,valueCount);[encoder endEncoding];
						encoder=[command computeCommandEncoder];if( !encoder ) return false;
						[encoder setBuffer:lineUpdated offset:0 atIndex:0];
						[encoder setBuffer:density offset:canonicalOffset[component] atIndex:1];
						[encoder setBuffer:momentum offset:canonicalOffset[component] atIndex:2];
						[encoder setBuffer:gridParameter offset:0 atIndex:3];
						Dispatch(encoder,context.scatterDualLineValues,valueCount);[encoder endEncoding];
					}
					if( periodic ) {
						const MetalPeriodicDualParameters seam={static_cast<std::uint32_t>(request.shape.nx),
							static_cast<std::uint32_t>(request.shape.ny),static_cast<std::uint32_t>(request.shape.nz),
							component,component};
						id<MTLBuffer> parameter=[context.device newBufferWithBytes:&seam length:sizeof(seam)
							options:MTLResourceStorageModeShared];if( !add(parameter,MTLStorageModeShared) ) return false;
						encoder=[command computeCommandEncoder];if( !encoder ) return false;
						[encoder setBuffer:density offset:canonicalOffset[component] atIndex:0];
						[encoder setBuffer:momentum offset:canonicalOffset[component] atIndex:1];
						[encoder setBuffer:parameter offset:0 atIndex:2];
						const std::size_t seams=component==0u?request.shape.ny*request.shape.nz:
							(component==1u?request.shape.nx*request.shape.nz:request.shape.nx*request.shape.ny);
						Dispatch(encoder,context.publishPeriodicDualSeam,seams);[encoder endEncoding];
					}
				}
				if( actual>certifiedBytes||actual>(UINT64_C(1)<<31u) ) return false;
				CommitTrackedMetalCommand(command);[command waitUntilCompleted];
				if( [command status]!=MTLCommandBufferStatusCompleted ) return false;
				const std::uint64_t commits=MetalCommandCommitCount-beginningCommits;
				const std::uint64_t reads=MetalHostBufferReadCount-beginningReads;
				if( commits!=1u||reads!=0u ) return false;
				FireProductionMetalDualMomentumResidentResult computed;
				computed.packedAuxiliaryFaceDensity=density;computed.packedMomentum=momentum;
				computed.faceByteOffset=canonicalOffset;computed.executedSubmapCount=15u;
				computed.commandCommitCount=1u;computed.interstageFullGridTransferCount=0u;
				computed.actualMetalAllocationBytes=actual;
				computed.deviceElapsedMS=([command GPUEndTime]-[command GPUStartTime])*1000.0;
				computed.deviceStartTimeS=[command GPUStartTime];
				computed.deviceEndTimeS=[command GPUEndTime];
				if( !std::isfinite(computed.deviceElapsedMS) ) return false;
				result=std::move(computed);
			}
			if( structuredError ) structuredError->clear();return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionMetalDualMomentumResidentResult();
			if( structuredError ) try { *structuredError=
				"production resident dual allocation failed"; } catch( const std::bad_alloc& ) {}
			return false;
		}
	}

	bool RemapFireProductionCompatibleDualMomentumMetalResident(
		const FireProductionDualMomentumRequest& request,
		const FireProductionMetalDualMomentumResidentInput& input,
		FireProductionMetalDualMomentumResidentResult& result,
		std::string* structuredError )
	{
		result=FireProductionMetalDualMomentumResidentResult();
		try {
			if( !ValidateFireProductionCompatibleDualMomentumRequest(request,structuredError) ) return false;
			std::uint64_t certifiedBytes=0u;
			if( !FireProductionCompatibleDualMomentumResidentWorkingSetBytes(
				request.shape,certifiedBytes)||certifiedBytes>(UINT64_C(1)<<31u) ) {
				if( structuredError ) *structuredError=
					"production compatible dual working set exceeds two GiB";
				return false;
			}
			MetalRemapContext& context=Context();
			if( !context.Valid() ) { if( structuredError ) *structuredError=context.error;return false; }
			@autoreleasepool {
				const std::size_t faceCounts[]={
					FireProductionProjectionFaceCount(request.shape,0u),
					FireProductionProjectionFaceCount(request.shape,1u),
					FireProductionProjectionFaceCount(request.shape,2u)};
				std::array<std::size_t,3> canonicalOffset;
				canonicalOffset[0]=0u;canonicalOffset[1]=faceCounts[0]*sizeof(float);
				canonicalOffset[2]=(faceCounts[0]+faceCounts[1])*sizeof(float);
				const std::size_t allFaces=faceCounts[0]+faceCounts[1]+faceCounts[2];
				const std::size_t packedBytes=allFaces*sizeof(float);
				const unsigned int axes[]={0u,1u,2u,1u,0u};
				if( !input.packedFaceDensity||!input.packedMomentum||
					[input.packedFaceDensity storageMode]!=MTLStorageModePrivate||
					[input.packedMomentum storageMode]!=MTLStorageModePrivate||
					[input.packedFaceDensity length]<packedBytes||
					[input.packedMomentum length]<packedBytes||
					input.faceByteOffset!=canonicalOffset ) return false;
				for( unsigned int pass=0u;pass<5u;++pass ) if(
					!input.acceptedGasMassDoseKGPerM2[pass]||
					[input.acceptedGasMassDoseKGPerM2[pass] storageMode]!=MTLStorageModePrivate||
					[input.acceptedGasMassDoseKGPerM2[pass] length]<
						faceCounts[axes[pass]]*sizeof(float) ) return false;
				auto privateBuffer=[&](std::size_t bytes) { return [context.device
					newBufferWithLength:bytes options:MTLResourceStorageModePrivate]; };
				id<MTLBuffer> densityA=privateBuffer(packedBytes),momentumA=privateBuffer(packedBytes),
					densityB=privateBuffer(packedBytes),momentumB=privateBuffer(packedBytes);
				std::uint64_t actual=0u;
				auto record=[&](id<MTLBuffer> buffer,MTLStorageMode mode) {
					if( !buffer||[buffer storageMode]!=mode ) return false;
					const std::uint64_t bytes=static_cast<std::uint64_t>([buffer allocatedSize]);
					if( actual>std::numeric_limits<std::uint64_t>::max()-bytes ) return false;
					actual+=bytes;return true;
				};
				if( !record(input.packedFaceDensity,MTLStorageModePrivate)||
					!record(input.packedMomentum,MTLStorageModePrivate)||
					!record(densityA,MTLStorageModePrivate)||!record(momentumA,MTLStorageModePrivate)||
					!record(densityB,MTLStorageModePrivate)||!record(momentumB,MTLStorageModePrivate) )
					return false;
				for( id<MTLBuffer> buffer : input.acceptedGasMassDoseKGPerM2 )
					if( !record(buffer,MTLStorageModePrivate) ) return false;
				auto boundaryValue=[](FireProductionProjectionBoundary boundary) -> std::uint32_t {
					return boundary==FireProductionProjectionPeriodic?0u:
						(boundary==FireProductionProjectionPressureOpen?1u:2u);
				};
				id<MTLBuffer> currentDensity=input.packedFaceDensity;
				id<MTLBuffer> currentMomentum=input.packedMomentum;
				id<MTLBuffer> nextDensity=densityA,nextMomentum=momentumA;
				const std::uint64_t beginningCommits=MetalCommandCommitCount;
				const std::uint64_t beginningReads=MetalHostBufferReadCount;
				id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context.queue);
				if( !command ) return false;
				for( unsigned int pass=0u;pass<5u;++pass ) {
					const CompatibleDualParams parameters={
						static_cast<std::uint32_t>(request.shape.nx),
						static_cast<std::uint32_t>(request.shape.ny),
						static_cast<std::uint32_t>(request.shape.nz),axes[pass],
						boundaryValue(request.boundary[0]),boundaryValue(request.boundary[1]),
						boundaryValue(request.boundary[2]),boundaryValue(request.boundary[3]),
						boundaryValue(request.boundary[4]),boundaryValue(request.boundary[5]),
						request.shape.cellWidthM};
					id<MTLBuffer> parameter=[context.device newBufferWithBytes:&parameters
						length:sizeof(parameters) options:MTLResourceStorageModeShared];
					if( !record(parameter,MTLStorageModeShared) ) return false;
					id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
					if( !encoder ) return false;
					[encoder setBuffer:currentDensity offset:0 atIndex:0];
					[encoder setBuffer:currentMomentum offset:0 atIndex:1];
					[encoder setBuffer:input.acceptedGasMassDoseKGPerM2[pass] offset:0 atIndex:2];
					[encoder setBuffer:nextDensity offset:0 atIndex:3];
					[encoder setBuffer:nextMomentum offset:0 atIndex:4];
					[encoder setBuffer:parameter offset:0 atIndex:5];
					Dispatch(encoder,context.compatibleDualUpdate,allFaces);[encoder endEncoding];
					currentDensity=nextDensity;currentMomentum=nextMomentum;
					if( pass==0u ) {nextDensity=densityB;nextMomentum=momentumB;}
					else if( pass+1u<5u ) {
						nextDensity=nextDensity==densityA?densityB:densityA;
						nextMomentum=nextMomentum==momentumA?momentumB:momentumA;
					}
				}
				if( actual>certifiedBytes||actual>(UINT64_C(1)<<31u) ) return false;
				CommitTrackedMetalCommand(command);[command waitUntilCompleted];
				if( [command status]!=MTLCommandBufferStatusCompleted ) return false;
				const std::uint64_t commits=MetalCommandCommitCount-beginningCommits;
				const std::uint64_t reads=MetalHostBufferReadCount-beginningReads;
				if( commits!=1u||reads!=0u ) return false;
				FireProductionMetalDualMomentumResidentResult computed;
				computed.packedAuxiliaryFaceDensity=currentDensity;
				computed.packedMomentum=currentMomentum;
				computed.faceByteOffset=canonicalOffset;computed.executedSubmapCount=15u;
				computed.commandCommitCount=1u;computed.interstageFullGridTransferCount=0u;
				computed.actualMetalAllocationBytes=actual;
				computed.deviceElapsedMS=([command GPUEndTime]-[command GPUStartTime])*1000.0;
				computed.deviceStartTimeS=[command GPUStartTime];
				computed.deviceEndTimeS=[command GPUEndTime];
				if( !std::isfinite(computed.deviceElapsedMS) ) return false;
				result=std::move(computed);
			}
			if( structuredError ) structuredError->clear();return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionMetalDualMomentumResidentResult();
			if( structuredError ) try {
				*structuredError="production compatible dual allocation failed";
			} catch( const std::bad_alloc& ) {}
			return false;
		}
	}

	bool RemapFireProductionDualMomentumMetalResidentComparator(
		const FireProductionDualMomentumRequest& request,
		FireProductionDualMomentumResult& result,
		std::string* structuredError )
	{
		result=FireProductionDualMomentumResult();
		try {
			FireProductionMetalDualMomentumStaticState staticState;
			if( !PrepareFireProductionDualMomentumMetalStaticState(request,staticState,
				structuredError) ) return false;
			MetalRemapContext& context=Context();
			@autoreleasepool {
				std::array<std::size_t,3> offsets,counts;std::size_t allFaces=0u;
				for( unsigned int axis=0u;axis<3u;++axis ) { offsets[axis]=allFaces*sizeof(float);
					counts[axis]=FireProductionProjectionFaceCount(request.shape,axis);allFaces+=counts[axis]; }
				std::vector<float> density(allFaces),momentum(allFaces);
				for( unsigned int axis=0u;axis<3u;++axis ) {
					std::copy(request.beginningFaceDensity[axis].begin(),request.beginningFaceDensity[axis].end(),
						density.begin()+offsets[axis]/sizeof(float));
					std::copy(request.beginningMomentum[axis].begin(),request.beginningMomentum[axis].end(),
						momentum.begin()+offsets[axis]/sizeof(float));
				}
				id<MTLBuffer> densityStage=[context.device newBufferWithBytes:density.data()
					length:allFaces*sizeof(float) options:MTLResourceStorageModeShared];
				id<MTLBuffer> momentumStage=[context.device newBufferWithBytes:momentum.data()
					length:allFaces*sizeof(float) options:MTLResourceStorageModeShared];
				id<MTLBuffer> densityPrivate=[context.device newBufferWithLength:allFaces*sizeof(float)
					options:MTLResourceStorageModePrivate];
				id<MTLBuffer> momentumPrivate=[context.device newBufferWithLength:allFaces*sizeof(float)
					options:MTLResourceStorageModePrivate];
				id<MTLCommandBuffer> upload=TrackedMetalCommandBuffer(context.queue);
				id<MTLBlitCommandEncoder> blit=upload?[upload blitCommandEncoder]:nil;
				if( !densityStage||!momentumStage||!densityPrivate||!momentumPrivate||!blit ) return false;
				[blit copyFromBuffer:densityStage sourceOffset:0 toBuffer:densityPrivate destinationOffset:0
					size:allFaces*sizeof(float)];
				[blit copyFromBuffer:momentumStage sourceOffset:0 toBuffer:momentumPrivate destinationOffset:0
					size:allFaces*sizeof(float)];[blit endEncoding];CommitTrackedMetalCommand(upload);
				[upload waitUntilCompleted];if( [upload status]!=MTLCommandBufferStatusCompleted ) return false;
				FireProductionMetalDualMomentumResidentInput input;input.packedFaceDensity=densityPrivate;
				input.packedMomentum=momentumPrivate;input.faceByteOffset=offsets;
				FireProductionMetalDualMomentumResidentResult resident;
				if( !RemapFireProductionDualMomentumMetalResident(request,staticState,input,resident,
					structuredError) ) return false;
				id<MTLBuffer> output=[context.device newBufferWithLength:2u*allFaces*sizeof(float)
					options:MTLResourceStorageModeShared];
				id<MTLCommandBuffer> stage=TrackedMetalCommandBuffer(context.queue);
				blit=stage?[stage blitCommandEncoder]:nil;if( !output||!blit ) return false;
				[blit copyFromBuffer:resident.packedAuxiliaryFaceDensity sourceOffset:0 toBuffer:output
					destinationOffset:0 size:allFaces*sizeof(float)];
				[blit copyFromBuffer:resident.packedMomentum sourceOffset:0 toBuffer:output
					destinationOffset:allFaces*sizeof(float) size:allFaces*sizeof(float)];
				[blit endEncoding];CommitTrackedMetalCommand(stage);[stage waitUntilCompleted];
				if( [stage status]!=MTLCommandBufferStatusCompleted ) return false;
				const float* values=static_cast<const float*>(ReadTrackedMetalBuffer(output));
				FireProductionDualMomentumResult computed;
				for( unsigned int axis=0u;axis<3u;++axis ) {
					const std::size_t beginning=offsets[axis]/sizeof(float);
					computed.auxiliaryFaceDensity[axis].assign(values+beginning,values+beginning+counts[axis]);
					computed.momentum[axis].assign(values+allFaces+beginning,
						values+allFaces+beginning+counts[axis]);
				}
				computed.executedSubmapCount=resident.executedSubmapCount;
				computed.commandCommitCount=resident.commandCommitCount;
				computed.interstageFullGridTransferCount=resident.interstageFullGridTransferCount;
				computed.actualMetalAllocationBytes=resident.actualMetalAllocationBytes;
				computed.deviceElapsedMS=resident.deviceElapsedMS;result=std::move(computed);
			}
			if( structuredError ) structuredError->clear();return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionDualMomentumResult();
			if( structuredError ) try { *structuredError=
				"production resident dual comparator allocation failed"; } catch( const std::bad_alloc& ) {}
			return false;
		}
	}

	bool RemapFireProductionCompatibleDualMomentumMetalComparator(
		const FireProductionDualMomentumRequest& request,
		const std::array<std::vector<float>,5>& acceptedGasMassDoseKGPerM2,
		FireProductionDualMomentumResult& result,
		std::string* structuredError )
	{
		result=FireProductionDualMomentumResult();
		try {
			if( !ValidateFireProductionCompatibleDualMomentumRequest(request,structuredError) )
				return false;
			MetalRemapContext& context=Context();
			if( !context.Valid() ) {
				if( structuredError ) *structuredError=context.error;
				return false;
			}
			@autoreleasepool {
				const unsigned int axes[]={0u,1u,2u,1u,0u};
				std::array<std::size_t,3> offsets,counts;
				std::size_t allFaces=0u;
				for( unsigned int axis=0u;axis<3u;++axis ) {
					offsets[axis]=allFaces*sizeof(float);
					counts[axis]=FireProductionProjectionFaceCount(request.shape,axis);
					allFaces+=counts[axis];
				}
				for( unsigned int pass=0u;pass<5u;++pass ) if(
					acceptedGasMassDoseKGPerM2[pass].size()!=counts[axes[pass]] ) {
					if( structuredError ) *structuredError=
						"production compatible dual Metal comparator mass-dose shape is invalid";
					return false;
				}
				std::vector<float> density(allFaces),momentum(allFaces);
				for( unsigned int axis=0u;axis<3u;++axis ) {
					std::copy(request.beginningFaceDensity[axis].begin(),
						request.beginningFaceDensity[axis].end(),
						density.begin()+offsets[axis]/sizeof(float));
					std::copy(request.beginningMomentum[axis].begin(),
						request.beginningMomentum[axis].end(),
						momentum.begin()+offsets[axis]/sizeof(float));
				}
				id<MTLBuffer> densityStage=[context.device newBufferWithBytes:density.data()
					length:allFaces*sizeof(float) options:MTLResourceStorageModeShared];
				id<MTLBuffer> momentumStage=[context.device newBufferWithBytes:momentum.data()
					length:allFaces*sizeof(float) options:MTLResourceStorageModeShared];
				id<MTLBuffer> densityPrivate=[context.device newBufferWithLength:
					allFaces*sizeof(float) options:MTLResourceStorageModePrivate];
				id<MTLBuffer> momentumPrivate=[context.device newBufferWithLength:
					allFaces*sizeof(float) options:MTLResourceStorageModePrivate];
				std::array<id<MTLBuffer>,5> doseStage,dosePrivate;
				for( unsigned int pass=0u;pass<5u;++pass ) {
					const std::size_t bytes=counts[axes[pass]]*sizeof(float);
					doseStage[pass]=[context.device newBufferWithBytes:
						acceptedGasMassDoseKGPerM2[pass].data() length:bytes
						options:MTLResourceStorageModeShared];
					dosePrivate[pass]=[context.device newBufferWithLength:bytes
						options:MTLResourceStorageModePrivate];
				}
				id<MTLCommandBuffer> upload=TrackedMetalCommandBuffer(context.queue);
				id<MTLBlitCommandEncoder> blit=upload?[upload blitCommandEncoder]:nil;
				if( !densityStage||!momentumStage||!densityPrivate||!momentumPrivate||!blit )
					return false;
				[blit copyFromBuffer:densityStage sourceOffset:0 toBuffer:densityPrivate
					destinationOffset:0 size:allFaces*sizeof(float)];
				[blit copyFromBuffer:momentumStage sourceOffset:0 toBuffer:momentumPrivate
					destinationOffset:0 size:allFaces*sizeof(float)];
				for( unsigned int pass=0u;pass<5u;++pass ) {
					if( !doseStage[pass]||!dosePrivate[pass] ) return false;
					[blit copyFromBuffer:doseStage[pass] sourceOffset:0
						toBuffer:dosePrivate[pass] destinationOffset:0
						size:counts[axes[pass]]*sizeof(float)];
				}
				[blit endEncoding];CommitTrackedMetalCommand(upload);[upload waitUntilCompleted];
				if( [upload status]!=MTLCommandBufferStatusCompleted ) return false;
				FireProductionMetalDualMomentumResidentInput input;
				input.packedFaceDensity=densityPrivate;input.packedMomentum=momentumPrivate;
				input.acceptedGasMassDoseKGPerM2=dosePrivate;input.faceByteOffset=offsets;
				FireProductionMetalDualMomentumResidentResult resident;
				if( !RemapFireProductionCompatibleDualMomentumMetalResident(request,input,
					resident,structuredError) ) return false;
				id<MTLBuffer> output=[context.device newBufferWithLength:
					2u*allFaces*sizeof(float) options:MTLResourceStorageModeShared];
				id<MTLCommandBuffer> stage=TrackedMetalCommandBuffer(context.queue);
				blit=stage?[stage blitCommandEncoder]:nil;
				if( !output||!blit ) return false;
				[blit copyFromBuffer:resident.packedAuxiliaryFaceDensity sourceOffset:0
					toBuffer:output destinationOffset:0 size:allFaces*sizeof(float)];
				[blit copyFromBuffer:resident.packedMomentum sourceOffset:0 toBuffer:output
					destinationOffset:allFaces*sizeof(float) size:allFaces*sizeof(float)];
				[blit endEncoding];CommitTrackedMetalCommand(stage);[stage waitUntilCompleted];
				if( [stage status]!=MTLCommandBufferStatusCompleted ) return false;
				const float* values=static_cast<const float*>(ReadTrackedMetalBuffer(output));
				if( !values ) return false;
				FireProductionDualMomentumResult computed;
				for( unsigned int axis=0u;axis<3u;++axis ) {
					const std::size_t beginning=offsets[axis]/sizeof(float);
					computed.auxiliaryFaceDensity[axis].assign(values+beginning,
						values+beginning+counts[axis]);
					computed.momentum[axis].assign(values+allFaces+beginning,
						values+allFaces+beginning+counts[axis]);
				}
				computed.executedSubmapCount=resident.executedSubmapCount;
				computed.commandCommitCount=resident.commandCommitCount;
				computed.interstageFullGridTransferCount=
					resident.interstageFullGridTransferCount;
				computed.actualMetalAllocationBytes=resident.actualMetalAllocationBytes;
				computed.deviceElapsedMS=resident.deviceElapsedMS;
				result=std::move(computed);
			}
			if( structuredError ) structuredError->clear();
			return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionDualMomentumResult();
			if( structuredError ) try {
				*structuredError="production compatible dual Metal comparator allocation failed";
			} catch( const std::bad_alloc& ) { structuredError->clear(); }
			return false;
		}
	}

	bool AttemptFireProductionResidentStepMetal(
		const FireProductionResidentStepRequest& request,
		FireProductionResidentStepResult& result,
		std::string* structuredError )
	{
		result=FireProductionResidentStepResult();
		bool plateauRefused=false;
		try {
			if( request.cellTransport.retainAcceptedGasMassDose&&
				!CompatibleMomentumDiagnosticActive ) {
				if( structuredError ) *structuredError=
					"production retained gas-mass dose requires the compatible-momentum diagnostic owner";
				return false;
			}
			const char* activeFailurePhase=
				"production resident step failed during preflight";
			bool attemptCompleted=false;
			struct FailurePhaseGuard
			{
				std::string* error;
				const char*& phase;
				bool& completed;
				~FailurePhaseGuard(){
					if(!completed&&error&&error->empty())*error=phase;
				}
			} failurePhaseGuard={structuredError,activeFailurePhase,attemptCompleted};
			auto markPhase=[&](const char* phase){activeFailurePhase=phase;
				if(structuredError)*structuredError=phase;};
			const char* manifoldProbeActivation=std::getenv(
				"RISE_FIRE_MANIFOLD_TIMESTEP_PROBE");
			if( manifoldProbeActivation&&std::strcmp(manifoldProbeActivation,"1")!=0 ) {
				if( structuredError ) *structuredError=
					"production manifold timestep probe activation is invalid";
				return false;
			}
			const char* manifoldStageBudgetActivation=std::getenv(
				"RISE_FIRE_MANIFOLD_STAGE_BUDGET_PROBE");
			if( manifoldStageBudgetActivation&&
				std::strcmp(manifoldStageBudgetActivation,"1")!=0 ) {
				if( structuredError ) *structuredError=
					"production manifold stage-budget probe activation is invalid";
				return false;
			}
			const char* timestepVelocityAuditActivation=std::getenv(
				"RISE_FIRE_TIMESTEP_VELOCITY_AUDIT");
			if( timestepVelocityAuditActivation&&
				std::strcmp(timestepVelocityAuditActivation,"1")!=0 ) {
				if( structuredError ) *structuredError=
					"production timestep velocity audit activation is invalid";
				return false;
			}
			const bool timestepVelocityAuditEnabled=timestepVelocityAuditActivation!=0;
			const char* goldenLongShadowActivation=std::getenv(
				"RISE_FIRE_GOLDEN_LONG_SHADOW");
			if( goldenLongShadowActivation&&
				std::strcmp(goldenLongShadowActivation,"1")!=0 ) {
				if( structuredError ) *structuredError=
					"production golden long-shadow activation is invalid";
				return false;
			}
			const char* anomalyClosureTestActivation=std::getenv(
				"RISE_FIRE_ADVECTIVE_ANOMALY_CLOSURE_TEST");
			if( anomalyClosureTestActivation&&(!goldenLongShadowActivation||
				(std::strcmp(anomalyClosureTestActivation,"disabled")!=0&&
				 std::strcmp(anomalyClosureTestActivation,"limited")!=0)) ) {
				if( structuredError ) *structuredError=
					"production advective anomaly closure test activation is invalid";
				return false;
			}
			const bool anomalyClosureTestDisabled=anomalyClosureTestActivation&&
				std::strcmp(anomalyClosureTestActivation,"disabled")==0;
			const char* tailThresholdREDActivation=std::getenv(
				"RISE_FIRE_MANIFOLD_TAIL_THRESHOLD_RED");
			if( tailThresholdREDActivation&&(!goldenLongShadowActivation||
				std::strcmp(tailThresholdREDActivation,"1")!=0) ) {
				if( structuredError ) *structuredError=
					"production manifold tail-threshold RED activation is invalid";
				return false;
			}
			const bool tailThresholdREDEnabled=tailThresholdREDActivation!=0;
			const char* anomalyConvergenceProbeActivation=std::getenv(
				"RISE_FIRE_ADVECTIVE_ANOMALY_CONVERGENCE_PROBE");
			if( anomalyConvergenceProbeActivation&&
				std::strcmp(anomalyConvergenceProbeActivation,"1")!=0 ) {
				if( structuredError ) *structuredError=
					"production advective anomaly convergence probe activation is invalid";
				return false;
			}
			const char* anomalyConvergencePassValue=std::getenv(
				"RISE_FIRE_ADVECTIVE_ANOMALY_CONVERGENCE_PASSES");
			unsigned long parsedAnomalyConvergencePasses=0u;
			if( anomalyConvergencePassValue ) {
				char* end=0;errno=0;
				parsedAnomalyConvergencePasses=std::strtoul(
					anomalyConvergencePassValue,&end,10);
				if( !anomalyConvergenceProbeActivation||errno!=0||!end||*end!='\0'||
					parsedAnomalyConvergencePasses<1u||parsedAnomalyConvergencePasses>8u||
					std::to_string(parsedAnomalyConvergencePasses)!=
						anomalyConvergencePassValue ) {
					if( structuredError ) *structuredError=
						"production advective anomaly convergence pass count is invalid";
					return false;
				}
			} else if( anomalyConvergenceProbeActivation ) {
				if( structuredError ) *structuredError=
					"production advective anomaly convergence pass count is missing";
				return false;
			}
			const std::uint32_t anomalyClosurePassLimit=anomalyConvergenceProbeActivation?
				static_cast<std::uint32_t>(parsedAnomalyConvergencePasses):2u;
			const char* hostResidualProbeActivation=std::getenv(
				"RISE_FIRE_HOST_RESIDUAL_PROBE");
			if( hostResidualProbeActivation&&(!goldenLongShadowActivation||
				!anomalyClosureTestActivation||
				std::strcmp(anomalyClosureTestActivation,"limited")!=0||
				std::strcmp(hostResidualProbeActivation,"1")!=0) ) {
				if( structuredError ) *structuredError=
					"production host-residual probe activation is invalid";
				return false;
			}
			const bool hostResidualProbeEnabled=hostResidualProbeActivation!=0;
			const char* timestepVelocityPackMode=std::getenv(
				"RISE_FIRE_TIMESTEP_VELOCITY_PACK_MODE");
			if( timestepVelocityPackMode&&(!(timestepVelocityAuditEnabled||hostResidualProbeEnabled)||
				(std::strcmp(timestepVelocityPackMode,"serial")!=0&&
				 std::strcmp(timestepVelocityPackMode,"parallel")!=0)) ) {
				if( structuredError ) *structuredError=
					"production timestep velocity pack mode is invalid";
				return false;
			}
			const bool timestepVelocityAuditSerial=timestepVelocityPackMode&&
				std::strcmp(timestepVelocityPackMode,"serial")==0;
			const auto timestepVelocityAuditStart=std::chrono::steady_clock::now();
			auto timestepVelocityAuditMS=[&](){return std::chrono::duration<double,std::milli>(
				std::chrono::steady_clock::now()-timestepVelocityAuditStart).count();};
				double timestepVelocityAuditValidatedMS=0.0,timestepVelocityAuditDualStaticMS=0.0,
					timestepVelocityAuditUploadMS=0.0,timestepVelocityAuditForceMS=0.0,
					timestepVelocityAuditCellMS=0.0,timestepVelocityAuditDualMS=0.0,
					timestepVelocityAuditSourceMS=0.0,timestepVelocityAuditPhysicalMS=0.0,
					timestepVelocityAuditRestorationMS=0.0,timestepVelocityAuditTerminalMS=0.0,
					timestepVelocityAuditTerminalValidationMS=0.0,
					timestepVelocityAuditResultCopyMS=0.0,
					timestepVelocityAuditMetadataMS=0.0,
					timestepVelocityAuditAuthorityMS=0.0;
			const char* plateauEvidenceActivation=std::getenv(
				"RISE_FIRE_RESTORATION_PLATEAU_PROBE");
			if( plateauEvidenceActivation&&std::strcmp(plateauEvidenceActivation,"1")!=0 ) {
				if( structuredError ) *structuredError=
					"production restoration plateau probe activation is invalid";
				return false;
			}
			const bool plateauEvidenceEnabled=plateauEvidenceActivation!=0;
			const char* physicalValidationProbeActivation=std::getenv(
				"RISE_FIRE_PHYSICAL_PROJECTION_VALIDATION_PROBE");
			if( physicalValidationProbeActivation&&
				std::strcmp(physicalValidationProbeActivation,"1")!=0 ) {
				if( structuredError ) *structuredError=
					"production physical projection validation probe activation is invalid";
				return false;
			}
			const bool physicalValidationProbeEnabled=physicalValidationProbeActivation!=0;
			unsigned int restorationProbeCycles=0u;bool restorationProbeEnabled=false;
			if( !ValidateFireProductionRestorationCycleProbe(restorationProbeCycles,
				restorationProbeEnabled,structuredError) ) return false;
			const FireProductionProjectionShape& shape=request.force.shape;
			auto sameShape=[](const FireProductionProjectionShape& a,
				const FireProductionProjectionShape& b) {
				return a.nx==b.nx&&a.ny==b.ny&&a.nz==b.nz&&a.cellWidthM==b.cellWidthM;
			};
			if( !sameShape(shape,request.cellTransport.shape)||
				!sameShape(shape,request.dualTransport.shape)||
				request.force.timeStepS!=request.cellTransport.timeStepS||
				request.force.timeStepS!=request.dualTransport.timeStepS||
				request.force.boundary!=request.cellTransport.boundary||
				request.force.boundary!=request.dualTransport.boundary||
				request.cellTransport.componentCount!=9u ) {
				if( structuredError ) *structuredError=
					"production resident step ownership metadata does not match";
				return false;
			}
			std::uint64_t certified=0u;
			if( !FireProductionResidentStepWorkingSetBytes(shape,request.force.boundary,certified)||
				certified>(UINT64_C(1)<<31u) ) {
				if( structuredError ) *structuredError=
					"production resident step working set exceeds two GiB";
				return false;
			}
			if( CompatibleMomentumDiagnosticActive ) {
				std::uint64_t ordinaryCell=0u,retainedCell=0u;
				if( !FireProductionCellPalindromeWorkingSetBytes(shape,9u,ordinaryCell)||
					!FireProductionCellPalindromeWorkingSetBytes(shape,9u,retainedCell,true)||
					retainedCell<ordinaryCell||certified>
						std::numeric_limits<std::uint64_t>::max()-(retainedCell-ordinaryCell) ) {
					if( structuredError ) *structuredError=
						"compatible-momentum diagnostic working-set certificate failed";
					return false;
				}
				certified+=retainedCell-ordinaryCell;
				if( certified>(UINT64_C(1)<<31u) ) {
					if( structuredError ) *structuredError=
						"compatible-momentum diagnostic working set exceeds two GiB";
					return false;
				}
			}
			const std::size_t cells=shape.CellCount();
			if(request.enforceManifoldPlateau&&!request.monitorManifoldDiagnostics){
				if(structuredError)*structuredError=
					"production manifold enforcement requires diagnostic monitoring";
				return false;
			}
			const bool measureManifold=request.monitorManifoldDiagnostics&&
				!plateauEvidenceEnabled;
			const bool closeAdvectiveAnomaly=request.enforceManifoldPlateau&&measureManifold&&
				!anomalyClosureTestDisabled&&
				!manifoldProbeActivation&&!manifoldStageBudgetActivation&&
				!timestepVelocityAuditActivation;
			if( measureManifold&&request.beginningManifoldDeviationPerCell.size()!=cells ) {
				if( structuredError ) *structuredError=
					"production resident step lacks beginning manifold metadata";
				return false;
			}
			std::array<float,MetalManifoldCertificateValues> packedThermochemistry;
			std::vector<float> representedBeginningDeviation;
			FireProductionManifoldTailTarget manifoldTailTarget;
			std::vector<float> effectiveRestorationTarget=
				request.restorationDivergenceTargetPerS;
			if( measureManifold ) {
				representedBeginningDeviation.resize(cells);
				for(std::size_t cell=0u;cell<cells;++cell)
					representedBeginningDeviation[cell]=static_cast<float>(
						request.beginningManifoldDeviationPerCell[cell]);
				if(request.restoreManifoldOutliers&&!request.enforceManifoldPlateau){
					if(!DeriveFireProductionManifoldTailTarget(
						request.beginningManifoldDeviationPerCell,
						static_cast<double>(request.force.timeStepS),
						static_cast<double>(shape.cellWidthM),manifoldTailTarget,
						structuredError,tailThresholdREDEnabled?0x1p-3:0x1p-4))return false;
					effectiveRestorationTarget=manifoldTailTarget.divergenceTargetPerS;
				}
			}
			const bool targetedRestorationActive=request.restoreManifoldOutliers&&
				!request.enforceManifoldPlateau&&manifoldTailTarget.outlierCellCount>0u;
			std::array<double,7> lowerEnthalpy,upperEnthalpy;
			MetalManifoldParameters manifoldParameters={static_cast<std::uint32_t>(cells),
				0u,0u,0u,0.0f,0.0f,0.0f,0.0f};
			if( measureManifold&&!PackMetalMethaneThermochemistry(packedThermochemistry,
				manifoldParameters,lowerEnthalpy,upperEnthalpy,structuredError) ) return false;
			std::array<std::size_t,3> faceCounts,faceOffsets;std::size_t allFaces=0u;
			for( unsigned int axis=0u;axis<3u;++axis ) {
				faceOffsets[axis]=allFaces*sizeof(float);
				faceCounts[axis]=FireProductionProjectionFaceCount(shape,axis);
				allFaces+=faceCounts[axis];
			}
			auto positiveZero=[](float value) {
				std::uint32_t bits=0u;std::memcpy(&bits,&value,sizeof(bits));return bits==0u;
			};
			std::array<bool,10> ownerValidationSucceeded;
			ownerValidationSucceeded.fill(false);
			std::array<std::string,10> ownerValidationError;
			auto validateOwnerPayload=[&](const unsigned int task) {
				bool valid=true;std::string& taskError=ownerValidationError[task];
				if( task==0u ) valid=ValidateFireProductionFrozenForceRequest(request.force,&taskError);
				else if( task==1u ) {
					valid=request.force.cellGasDensityKGPerM3.size()==cells&&
						request.cellTransport.conservativeValues.size()==9u*cells&&
						request.cellSourceIncrement.size()==9u*cells&&
						request.divergenceTargetPerS.size()==cells&&
						request.restorationDivergenceTargetPerS.size()==cells;
					if( valid ) valid=ValidateFireProductionCellPalindromeRequest(
						request.cellTransport,&taskError);
					else taskError="production resident step payload shape is invalid";
				} else if( task==2u ) {
					if( measureManifold )
						for( const double value:request.beginningManifoldDeviationPerCell )
							if( !std::isfinite(value) ) { valid=false;break; }
					if( !valid ) taskError=
						"production resident step beginning manifold metadata is nonfinite";
				} else if( task==3u ) {
					for( const float value:request.divergenceTargetPerS )
						if( !std::isfinite(value) ) { valid=false;break; }
					if( !valid ) taskError=
						"production resident step physical divergence target is nonfinite";
				} else if( task==4u ) {
					for( const float value:effectiveRestorationTarget )
						if( !std::isfinite(value) ) { valid=false;break; }
					if( !valid ) taskError=
						"production resident step restoration divergence target is nonfinite";
				} else if( task==5u ) {
					if( request.cellTransport.conservativeValues.size()==9u*cells&&
						request.force.cellGasDensityKGPerM3.size()==cells )
						for( std::size_t cell=0u;cell<cells;++cell ) {
							float gas=request.cellTransport.conservativeValues[cells+cell];
							for( std::size_t component=2u;component<=6u;++component )
								gas+=request.cellTransport.conservativeValues[component*cells+cell];
							if( gas!=request.force.cellGasDensityKGPerM3[cell] ) {valid=false;break;}
						}
					else valid=false;
					if( !valid ) taskError=
						"production resident step packed gas density does not match force input";
				} else if( task==6u ) {
					valid=ValidateFireProductionCellSourceIncrement(request.cellSourceIncrement,
						cells,measureManifold,&taskError);
				} else {
					const unsigned int axis=task-7u;
					valid=request.momentumSourceIncrement[axis].size()==faceCounts[axis]&&
						request.dualTransport.beginningFaceDensity[axis]==
							request.force.faceDensityKGPerM3[axis]&&
						request.dualTransport.beginningMomentum[axis]==
							request.force.beginningMomentumKGPerM2S[axis]&&
						request.dualTransport.frozenVelocityMPerS[axis]==
							request.cellTransport.frozenVelocityMPerS[axis];
					if( !valid ) taskError=
						"production resident step dual ownership does not match force/cell input";
					else for( const float value:request.momentumSourceIncrement[axis] )
						if( !positiveZero(value) ) {valid=false;taskError=
							"production resident step momentum source is not positive zero";break;}
				}
				ownerValidationSucceeded[task]=valid;
			};
			const bool serialOwnerValidation=timestepVelocityAuditSerial||
				GlobalOptions().ReadBool("force_all_threads_low_priority",false);
			if( serialOwnerValidation )
				for( unsigned int task=0u;task<ownerValidationSucceeded.size();++task )
					validateOwnerPayload(task);
			else Implementation::GlobalThreadPool().ParallelFor(
				ownerValidationSucceeded.size(),validateOwnerPayload);
			for( unsigned int task=0u;task<ownerValidationSucceeded.size();++task )
				if( !ownerValidationSucceeded[task] ) {
					if( structuredError ) *structuredError=ownerValidationError[task];
					return false;
				}
			timestepVelocityAuditValidatedMS=timestepVelocityAuditMS();
			FireProductionMetalDualMomentumStaticState dualStatic;
			timestepVelocityAuditDualStaticMS=timestepVelocityAuditValidatedMS;
			markPhase("production resident step failed while acquiring Metal context");
			MetalRemapContext& context=Context();
			if( !context.Valid() ) {
				if( structuredError ) *structuredError=context.error.empty()?
					"production resident step failed while acquiring Metal context":context.error;
				return false;
			}
			@autoreleasepool {
				markPhase("production resident step failed while allocating resident buffers");
				const std::size_t cellValueBytes=9u*cells*sizeof(float);
				const std::size_t packedFaceBytes=allFaces*sizeof(float);
				std::vector<float> packedFaceSource(allFaces,0.0f);
				for( unsigned int axis=0u;axis<3u;++axis ) std::copy(
					request.momentumSourceIncrement[axis].begin(),request.momentumSourceIncrement[axis].end(),
					packedFaceSource.begin()+faceOffsets[axis]/sizeof(float));
				auto stage=[&](const float* values,std::size_t bytes) {
					return [context.device newBufferWithBytes:values length:bytes
						options:MTLResourceStorageModeShared];
				};
				auto privateBuffer=[&](std::size_t bytes) { return [context.device
					newBufferWithLength:bytes options:MTLResourceStorageModePrivate]; };
				id<MTLBuffer> cellStage=stage(request.cellTransport.conservativeValues.data(),cellValueBytes);
				id<MTLBuffer> cellPrivate=privateBuffer(cellValueBytes);
				std::array<id<MTLBuffer>,3> velocityStage,velocityPrivate;
				for( unsigned int axis=0u;axis<3u;++axis ) {
					velocityStage[axis]=stage(request.cellTransport.frozenVelocityMPerS[axis].data(),
						faceCounts[axis]*sizeof(float));
					velocityPrivate[axis]=privateBuffer(faceCounts[axis]*sizeof(float));
				}
				id<MTLBuffer> ambientStage=stage(request.cellTransport.ambientValues.data(),9u*sizeof(float));
				id<MTLBuffer> ambientPrivate=privateBuffer(9u*sizeof(float));
				id<MTLBuffer> cellSourceStage=stage(request.cellSourceIncrement.data(),cellValueBytes);
				id<MTLBuffer> cellSourcePrivate=privateBuffer(cellValueBytes);
				id<MTLBuffer> faceSourceStage=stage(packedFaceSource.data(),packedFaceBytes);
				id<MTLBuffer> faceSourcePrivate=privateBuffer(packedFaceBytes);
				id<MTLBuffer> targetStage=stage(request.divergenceTargetPerS.data(),cells*sizeof(float));
				id<MTLBuffer> targetPrivate=privateBuffer(cells*sizeof(float));
				id<MTLBuffer> restorationTargetStage=stage(
					effectiveRestorationTarget.data(),cells*sizeof(float));
				id<MTLBuffer> restorationTargetPrivate=privateBuffer(cells*sizeof(float));
				id<MTLBuffer> manifoldThermochemistry=measureManifold?
					stage(packedThermochemistry.data(),packedThermochemistry.size()*sizeof(float)):nil;
				id<MTLBuffer> manifoldBeginningDeviation=measureManifold?
					stage(representedBeginningDeviation.data(),cells*sizeof(float)):nil;
				id<MTLBuffer> manifoldParametersBuffer=measureManifold?
					[context.device newBufferWithBytes:&manifoldParameters
						length:sizeof(manifoldParameters) options:MTLResourceStorageModeShared]:nil;
				id<MTLBuffer> manifoldMap=measureManifold?privateBuffer(2u*cells*sizeof(float)):nil;
				id<MTLBuffer> manifoldPredictorReduction=closeAdvectiveAnomaly?
					[context.device newBufferWithLength:3u*sizeof(std::uint32_t)
						options:MTLResourceStorageModeShared]:nil;
				id<MTLBuffer> manifoldQuantileScratch=measureManifold?
					[context.device newBufferWithLength:
						MetalManifoldQuantileScratchWords*sizeof(std::uint32_t)
						options:MTLResourceStorageModeShared]:nil;
				if( !cellStage||!cellPrivate||!ambientStage||!ambientPrivate||!cellSourceStage||
					!cellSourcePrivate||!faceSourceStage||!faceSourcePrivate||!targetStage||!targetPrivate||
					!restorationTargetStage||!restorationTargetPrivate||
					(measureManifold&&(!manifoldThermochemistry||!manifoldBeginningDeviation||
						!manifoldParametersBuffer||!manifoldMap))||
					(closeAdvectiveAnomaly&&!manifoldPredictorReduction)||
					(measureManifold&&!manifoldQuantileScratch) )
					return false;
				for( unsigned int axis=0u;axis<3u;++axis )
					if( !velocityStage[axis]||!velocityPrivate[axis] ) return false;
				std::uint64_t ownerActualMetalBytes=0u;
				auto recordOwner=[&](id<MTLBuffer> buffer) {
					if( !buffer ) return false;
					const std::uint64_t bytes=static_cast<std::uint64_t>([buffer allocatedSize]);
					if( ownerActualMetalBytes>std::numeric_limits<std::uint64_t>::max()-bytes ) return false;
					ownerActualMetalBytes+=bytes;return ownerActualMetalBytes<=certified;
				};
				if( !recordOwner(cellStage)||!recordOwner(cellPrivate)||!recordOwner(ambientStage)||
					!recordOwner(ambientPrivate)||!recordOwner(cellSourceStage)||
					!recordOwner(cellSourcePrivate)||!recordOwner(faceSourceStage)||
					!recordOwner(faceSourcePrivate)||!recordOwner(targetStage)||!recordOwner(targetPrivate)||
					!recordOwner(restorationTargetStage)||!recordOwner(restorationTargetPrivate) )
					return false;
				if( measureManifold&&(!recordOwner(manifoldThermochemistry)||
					!recordOwner(manifoldBeginningDeviation)||
					!recordOwner(manifoldParametersBuffer)||!recordOwner(manifoldMap)) ) return false;
				if( closeAdvectiveAnomaly&&!recordOwner(manifoldPredictorReduction) ) return false;
				if( measureManifold&&!recordOwner(manifoldQuantileScratch) ) return false;
				for( unsigned int axis=0u;axis<3u;++axis ) if(
					!recordOwner(velocityStage[axis])||!recordOwner(velocityPrivate[axis]) ) return false;
				markPhase("production resident step failed while uploading resident inputs");
				id<MTLCommandBuffer> upload=TrackedMetalCommandBuffer(context.queue);
				id<MTLBlitCommandEncoder> blit=upload?[upload blitCommandEncoder]:nil;if( !blit ) return false;
				[blit copyFromBuffer:cellStage sourceOffset:0 toBuffer:cellPrivate destinationOffset:0 size:cellValueBytes];
				for( unsigned int axis=0u;axis<3u;++axis ) [blit copyFromBuffer:velocityStage[axis]
					sourceOffset:0 toBuffer:velocityPrivate[axis] destinationOffset:0
					size:faceCounts[axis]*sizeof(float)];
				[blit copyFromBuffer:ambientStage sourceOffset:0 toBuffer:ambientPrivate destinationOffset:0 size:9u*sizeof(float)];
				[blit copyFromBuffer:cellSourceStage sourceOffset:0 toBuffer:cellSourcePrivate destinationOffset:0 size:cellValueBytes];
				[blit copyFromBuffer:faceSourceStage sourceOffset:0 toBuffer:faceSourcePrivate destinationOffset:0 size:packedFaceBytes];
				[blit copyFromBuffer:targetStage sourceOffset:0 toBuffer:targetPrivate destinationOffset:0 size:cells*sizeof(float)];
				[blit copyFromBuffer:restorationTargetStage sourceOffset:0 toBuffer:
					restorationTargetPrivate destinationOffset:0 size:cells*sizeof(float)];
				[blit endEncoding];CommitTrackedMetalCommand(upload);[upload waitUntilCompleted];
				if( [upload status]!=MTLCommandBufferStatusCompleted ) return false;
				const double timestepVelocityAuditOwnerUploadDeviceMS=
					([upload GPUEndTime]-[upload GPUStartTime])*1000.0;
				timestepVelocityAuditUploadMS=timestepVelocityAuditMS();
				FireProductionMetalCellPalindromeResidentInput cellInput;cellInput.conservativeValues=cellPrivate;
				cellInput.frozenVelocityMPerS=velocityPrivate;cellInput.ambientValues=ambientPrivate;
				FireProductionMetalFrozenForceResidentState force;
				FireProductionMetalCellPalindromeResidentResult cell;
				const bool compatibleMomentumDiagnostic=CompatibleMomentumDiagnosticActive;
				std::array<bool,3> preparationSucceeded={{false,false,false}};
				std::array<std::string,3> preparationError;
				std::array<double,3> preparationWallMS={{0.0,0.0,0.0}};
				markPhase("production resident step failed while preparing independent resident stages");
				auto prepareIndependent=[&](const unsigned int task) {
					const auto start=std::chrono::steady_clock::now();
					if(task==0u)preparationSucceeded[task]=
						PrepareFireProductionDualMomentumMetalStaticState(request.dualTransport,
							dualStatic,&preparationError[task]);
					else if(task==1u)preparationSucceeded[task]=
						AdvanceFireProductionFrozenForceMetalResidentState(request.force,force,
							&preparationError[task]);
					else preparationSucceeded[task]=RemapFireProductionCellPalindromeMetalResidentImpl(
						request.cellTransport,cellInput,cell,compatibleMomentumDiagnostic,
						&preparationError[task]);
					preparationWallMS[task]=std::chrono::duration<double,std::milli>(
						std::chrono::steady_clock::now()-start).count();
				};
				const bool serialIndependentPreparation=timestepVelocityAuditSerial||
					GlobalOptions().ReadBool("force_all_threads_low_priority",false);
				if(serialIndependentPreparation)
					for(unsigned int task=0u;task<preparationSucceeded.size();++task)
						prepareIndependent(task);
				else Implementation::GlobalThreadPool().ParallelFor(
					preparationSucceeded.size(),prepareIndependent);
				for(unsigned int task=0u;task<preparationSucceeded.size();++task)
					if(!preparationSucceeded[task]){
						if(structuredError)*structuredError=preparationError[task].empty()?
							("production independent preparation task "+std::to_string(task)+
							 " rejected without a diagnostic"):preparationError[task];
						return false;
					}
				timestepVelocityAuditForceMS=timestepVelocityAuditMS();
				const double predictorCellDeviceMS=cell.deviceElapsedMS;
				const double predictorCellDeviceStartTimeS=cell.deviceStartTimeS;
				const double predictorCellDeviceEndTimeS=cell.deviceEndTimeS;
				const std::uint64_t predictorCellActualMetalBytes=cell.actualMetalAllocationBytes;
				timestepVelocityAuditCellMS=timestepVelocityAuditForceMS;
				FireProductionMetalDualMomentumResidentInput dualInput;
				dualInput.packedFaceDensity=force.packedFaceDensityKGPerM3;
				dualInput.packedMomentum=force.packedMomentumKGPerM2S;
				dualInput.faceByteOffset=force.faceByteOffset;
				markPhase("production resident step failed during dual-momentum remap");
				FireProductionMetalDualMomentumResidentResult dual;
				if( CompatibleMomentumDiagnosticActive ) {
					dualInput.acceptedGasMassDoseKGPerM2=cell.acceptedGasMassDoseKGPerM2;
					if( !RemapFireProductionCompatibleDualMomentumMetalResident(
						request.dualTransport,dualInput,dual,structuredError) ) return false;
				} else if( !RemapFireProductionDualMomentumMetalResident(
					request.dualTransport,dualStatic,dualInput,dual,structuredError) ) return false;
				timestepVelocityAuditDualMS=timestepVelocityAuditMS();
				id<MTLBuffer> projectedDensity=privateBuffer(cells*sizeof(float));
				const MetalGridParameters sourceGrid={static_cast<std::uint32_t>(shape.nx),
					static_cast<std::uint32_t>(shape.ny),static_cast<std::uint32_t>(shape.nz),0u,9u};
				const std::uint32_t packedFaceCount=static_cast<std::uint32_t>(allFaces);
				id<MTLBuffer> sourceGridParameter=[context.device newBufferWithBytes:&sourceGrid
					length:sizeof(sourceGrid) options:MTLResourceStorageModeShared];
				id<MTLBuffer> sourceFaceParameter=[context.device newBufferWithBytes:&packedFaceCount
					length:sizeof(packedFaceCount) options:MTLResourceStorageModeShared];
				if( !recordOwner(projectedDensity)||!recordOwner(sourceGridParameter)||
					!recordOwner(sourceFaceParameter) ) return false;
				markPhase("production resident step failed while applying resident sources");
				const std::uint64_t beginningCommits=MetalCommandCommitCount;
				const std::uint64_t beginningReads=MetalHostBufferReadCount;
				id<MTLCommandBuffer> sourceCommand=TrackedMetalCommandBuffer(context.queue);
				id<MTLComputeCommandEncoder> encoder=sourceCommand?[sourceCommand computeCommandEncoder]:nil;
				if( !encoder ) return false;
				[encoder setBuffer:cell.conservativeValues offset:0 atIndex:0];
				[encoder setBuffer:cellSourcePrivate offset:0 atIndex:1];
				[encoder setBuffer:sourceGridParameter offset:0 atIndex:2];
				Dispatch(encoder,context.addCellSources,9u*cells);[encoder endEncoding];
				encoder=[sourceCommand computeCommandEncoder];if( !encoder ) return false;
				[encoder setBuffer:cell.conservativeValues offset:0 atIndex:0];
				[encoder setBuffer:projectedDensity offset:0 atIndex:1];
				[encoder setBuffer:sourceGridParameter offset:0 atIndex:2];
				Dispatch(encoder,context.extractGasDensity,cells);[encoder endEncoding];
				encoder=[sourceCommand computeCommandEncoder];if( !encoder ) return false;
				[encoder setBuffer:dual.packedMomentum offset:0 atIndex:0];
				[encoder setBuffer:faceSourcePrivate offset:0 atIndex:1];
				[encoder setBuffer:sourceFaceParameter offset:0 atIndex:2];
				Dispatch(encoder,context.addFaceSources,allFaces);[encoder endEncoding];
				CommitTrackedMetalCommand(sourceCommand);[sourceCommand waitUntilCompleted];
				std::uint64_t sourceCommits=MetalCommandCommitCount-beginningCommits;
				if( [sourceCommand status]!=MTLCommandBufferStatusCompleted||sourceCommits!=1u||
					MetalHostBufferReadCount-beginningReads!=0u ) return false;
				timestepVelocityAuditSourceMS=timestepVelocityAuditMS();
				float maximumPredictedAdvectiveAnomalyFloat=0.0f;
				std::uint32_t anomalyPredictorMeasurementCount=0u;
				double anomalyPredictorDeviceMS=0.0,anomalyCorrectorSourceDeviceMS=0.0,
					anomalyPredictorDeviceStartTimeS=0.0,anomalyPredictorDeviceEndTimeS=0.0,
					anomalyCorrectorSourceDeviceStartTimeS=0.0,
					anomalyCorrectorSourceDeviceEndTimeS=0.0;
				auto predictAdvectiveAnomaly=[&](){
					markPhase("production resident step failed during advective-anomaly prediction");
					std::memset([manifoldPredictorReduction contents],0,3u*sizeof(std::uint32_t));
					id<MTLCommandBuffer> predictorCommand=TrackedMetalCommandBuffer(context.queue);
					id<MTLBlitCommandEncoder> predictorBlit=
						predictorCommand?[predictorCommand blitCommandEncoder]:nil;
					if( !predictorBlit ) return false;
					[predictorBlit copyFromBuffer:restorationTargetStage sourceOffset:0
						toBuffer:restorationTargetPrivate destinationOffset:0 size:cells*sizeof(float)];
					[predictorBlit endEncoding];
					id<MTLComputeCommandEncoder> predictorEncoder=
						predictorCommand?[predictorCommand computeCommandEncoder]:nil;
					if( !predictorEncoder ) return false;
					[predictorEncoder setBuffer:manifoldBeginningDeviation offset:0 atIndex:0];
					[predictorEncoder setBuffer:cell.conservativeValues offset:0 atIndex:1];
					[predictorEncoder setBuffer:manifoldThermochemistry offset:0 atIndex:2];
					[predictorEncoder setBuffer:manifoldMap offset:0 atIndex:3];
					[predictorEncoder setBuffer:manifoldPredictorReduction offset:0 atIndex:4];
					[predictorEncoder setBuffer:manifoldParametersBuffer offset:0 atIndex:5];
					[predictorEncoder setBuffer:manifoldQuantileScratch offset:0 atIndex:6];
					Dispatch(predictorEncoder,context.measureMethaneManifold,cells);
					[predictorEncoder endEncoding];
					predictorEncoder=[predictorCommand computeCommandEncoder];
					if( !predictorEncoder ) return false;
					const float inverseTimeStep=1.0f/request.force.timeStepS;
					[predictorEncoder setBuffer:manifoldMap offset:0 atIndex:0];
					[predictorEncoder setBuffer:restorationTargetPrivate offset:0 atIndex:1];
					[predictorEncoder setBytes:&inverseTimeStep length:sizeof(inverseTimeStep) atIndex:2];
					Dispatch(predictorEncoder,context.foldMethaneAdvectiveAnomalyTarget,cells);
					[predictorEncoder endEncoding];
					CommitTrackedMetalCommand(predictorCommand);[predictorCommand waitUntilCompleted];
					if( [predictorCommand status]!=MTLCommandBufferStatusCompleted ) return false;
					anomalyPredictorDeviceMS+=
						([predictorCommand GPUEndTime]-[predictorCommand GPUStartTime])*1000.0;
					const double predictorStart=[predictorCommand GPUStartTime];
					const double predictorEnd=[predictorCommand GPUEndTime];
					anomalyPredictorDeviceStartTimeS=anomalyPredictorDeviceStartTimeS>0.0?
						std::min(anomalyPredictorDeviceStartTimeS,predictorStart):predictorStart;
					anomalyPredictorDeviceEndTimeS=
						std::max(anomalyPredictorDeviceEndTimeS,predictorEnd);
					const std::uint32_t* predictorReduction=static_cast<const std::uint32_t*>(
						ReadTrackedMetalBuffer(manifoldPredictorReduction));
					++anomalyPredictorMeasurementCount;
					if( !predictorReduction||predictorReduction[2u]!=0u ) return false;
					std::memcpy(&maximumPredictedAdvectiveAnomalyFloat,predictorReduction,sizeof(float));
					return std::isfinite(maximumPredictedAdvectiveAnomalyFloat);
				};
				FireProductionProjectionRequest projectionRequest;
				projectionRequest.shape=shape;projectionRequest.timeStepS=request.force.timeStepS;
				projectionRequest.ambientDensityKGPerM3=request.force.ambientDensityKGPerM3;
				projectionRequest.boundary=request.force.boundary;
				projectionRequest.gasDensityKGPerM3=request.force.cellGasDensityKGPerM3;
				projectionRequest.provisionalMomentumKGPerM2S=request.dualTransport.beginningMomentum;
				projectionRequest.divergenceTargetPerS=request.divergenceTargetPerS;
				projectionRequest.residentPhysicalOpenVCycleCount=
					request.physicalOpenProjectionVCycleCount;
				FireProductionMetalProjectionResidentInput projectionInput;
				projectionInput.gasDensityKGPerM3=projectedDensity;
				projectionInput.provisionalMomentumKGPerM2S.fill(dual.packedMomentum);
				projectionInput.provisionalMomentumByteOffset=dual.faceByteOffset;
				projectionInput.divergenceTargetPerS=targetPrivate;
				const char* restorationTest=std::getenv("RISE_FIRE_PRODUCTION_RESTORATION_TEST");
				const bool restorationRemoved=!(request.enforceManifoldPlateau||
					targetedRestorationActive)||
					(restorationTest&&std::strcmp(restorationTest,"removed")==0);
				const bool restorationFullTarget=restorationTest&&
					std::strcmp(restorationTest,"full-target")==0;
				FireProductionProjectionResult physicalProjection,projection;
				std::uint32_t executedAnomalyClosurePasses=1u;
				double anomalyCorrectorCellDeviceMS=0.0,
					anomalyCorrectorCellDeviceStartTimeS=0.0,
					anomalyCorrectorCellDeviceEndTimeS=0.0,
					anomalyRestorationDeviceMS=0.0,
					anomalyRestorationDeviceStartTimeS=0.0,
					anomalyRestorationDeviceEndTimeS=0.0;
				if( restorationRemoved ) {
					if( !ProjectFireProductionMetalResident(projectionRequest,projectionInput,
						projection,structuredError) ) return false;
					timestepVelocityAuditPhysicalMS=timestepVelocityAuditMS();
					timestepVelocityAuditRestorationMS=timestepVelocityAuditPhysicalMS;
				} else {
					FireProductionMetalProjectionResidentState physicalState;
					markPhase("production resident step failed during physical projection");
					if( !ProjectFireProductionMetalResidentState(projectionRequest,projectionInput,
						physicalState,physicalProjection,structuredError) ) return false;
					timestepVelocityAuditPhysicalMS=timestepVelocityAuditMS();
					FireProductionProjectionRequest restorationRequest=projectionRequest;
					restorationRequest.divergenceTargetPerS=effectiveRestorationTarget;
					FireProductionMetalProjectionResidentInput restorationInput;
					restorationInput.gasDensityKGPerM3=projectedDensity;
					restorationInput.provisionalMomentumKGPerM2S=physicalState.momentumKGPerM2S;
					restorationInput.provisionalMomentumByteOffset=physicalState.momentumByteOffset;
					restorationInput.divergenceTargetPerS=restorationFullTarget?
						targetPrivate:restorationTargetPrivate;
					for(std::uint32_t pass=1u;closeAdvectiveAnomaly&&
						pass<anomalyClosurePassLimit;++pass){
						if(!predictAdvectiveAnomaly())return false;
						if(maximumPredictedAdvectiveAnomalyFloat==0.0f)break;
						FireProductionMetalProjectionResidentState restorationState;
						if( !ProjectFireProductionMetalRestorationResidentState(restorationRequest,
							restorationInput,restorationTargetPrivate,restorationState,projection,
							structuredError) ) return false;
						anomalyRestorationDeviceMS+=projection.deviceElapsedMS;
						anomalyRestorationDeviceStartTimeS=
							anomalyRestorationDeviceStartTimeS>0.0?
							std::min(anomalyRestorationDeviceStartTimeS,
								projection.deviceStartTimeS):projection.deviceStartTimeS;
						anomalyRestorationDeviceEndTimeS=
							std::max(anomalyRestorationDeviceEndTimeS,projection.deviceEndTimeS);
						FireProductionMetalCellPalindromeResidentInput correctorInput;
						correctorInput.conservativeValues=cellPrivate;
						correctorInput.frozenVelocityMPerS=restorationState.velocityMPerS;
						correctorInput.ambientValues=ambientPrivate;
						FireProductionMetalCellPalindromeResidentResult corrector;
						std::array<bool,2> correctorSucceeded={{false,false}};
						std::array<std::string,2> correctorError;
						auto finishCorrector=[&](const std::size_t task){
							if(task==0u){
								if(!RemapFireProductionCellPalindromeMetalResident(request.cellTransport,
									correctorInput,corrector,&correctorError[task]))return;
								anomalyCorrectorCellDeviceMS+=corrector.deviceElapsedMS;
								anomalyCorrectorCellDeviceStartTimeS=
									anomalyCorrectorCellDeviceStartTimeS>0.0?
									std::min(anomalyCorrectorCellDeviceStartTimeS,
										corrector.deviceStartTimeS):corrector.deviceStartTimeS;
								anomalyCorrectorCellDeviceEndTimeS=
									std::max(anomalyCorrectorCellDeviceEndTimeS,corrector.deviceEndTimeS);
								id<MTLCommandBuffer> correctorSource=TrackedMetalCommandBuffer(context.queue);
								id<MTLComputeCommandEncoder> correctorEncoder=
									correctorSource?[correctorSource computeCommandEncoder]:nil;
								if(!correctorEncoder){correctorError[task]=
									"production anomaly corrector source encoder failed";return;}
								[correctorEncoder setBuffer:corrector.conservativeValues offset:0 atIndex:0];
								[correctorEncoder setBuffer:cellSourcePrivate offset:0 atIndex:1];
								[correctorEncoder setBuffer:sourceGridParameter offset:0 atIndex:2];
								Dispatch(correctorEncoder,context.addCellSources,9u*cells);
								[correctorEncoder endEncoding];CommitTrackedMetalCommand(correctorSource);
								[correctorSource waitUntilCompleted];
								if([correctorSource status]!=MTLCommandBufferStatusCompleted){
									correctorError[task]="production anomaly corrector source failed";return;}
								anomalyCorrectorSourceDeviceMS+=([correctorSource GPUEndTime]-
									[correctorSource GPUStartTime])*1000.0;
								const double correctorSourceStart=[correctorSource GPUStartTime];
								const double correctorSourceEnd=[correctorSource GPUEndTime];
								anomalyCorrectorSourceDeviceStartTimeS=
									anomalyCorrectorSourceDeviceStartTimeS>0.0?
									std::min(anomalyCorrectorSourceDeviceStartTimeS,
										correctorSourceStart):correctorSourceStart;
								anomalyCorrectorSourceDeviceEndTimeS=
									std::max(anomalyCorrectorSourceDeviceEndTimeS,correctorSourceEnd);
								correctorSucceeded[task]=true;
							}else correctorSucceeded[task]=
								PublishFireProductionMetalRestorationResidentState(restorationRequest,
									restorationState,projection,&correctorError[task]);
						};
						if(serialIndependentPreparation){finishCorrector(0u);finishCorrector(1u);}
						else Implementation::GlobalThreadPool().ParallelFor(
							correctorSucceeded.size(),finishCorrector);
						for(std::size_t task=0u;task<correctorSucceeded.size();++task)
							if(!correctorSucceeded[task]){
								if(structuredError)*structuredError=correctorError[task];
								return false;
							}
						++sourceCommits;cell=std::move(corrector);++executedAnomalyClosurePasses;
					}
					if(executedAnomalyClosurePasses==1u&&
						!ProjectFireProductionMetalRestorationResident(restorationRequest,
						restorationInput,restorationTargetPrivate,projection,structuredError) ) return false;
					if(executedAnomalyClosurePasses==1u){
						anomalyRestorationDeviceMS=projection.deviceElapsedMS;
						anomalyRestorationDeviceStartTimeS=projection.deviceStartTimeS;
						anomalyRestorationDeviceEndTimeS=projection.deviceEndTimeS;
					}
					timestepVelocityAuditRestorationMS=timestepVelocityAuditMS();
				}
				markPhase("production resident step failed while publishing resident terminal state");
				const std::size_t manifoldReductionOffset=cellValueBytes+2u*packedFaceBytes;
				const std::size_t manifoldReductionBytes=4u*sizeof(std::uint32_t);
				id<MTLBuffer> terminal=[context.device newBufferWithLength:
					(manifoldReductionOffset+(measureManifold?manifoldReductionBytes:0u))
					options:MTLResourceStorageModeShared];
				id<MTLCommandBuffer> terminalCommand=TrackedMetalCommandBuffer(context.queue);
				if( !recordOwner(terminal)||!terminalCommand ) return false;
				if( measureManifold ) {
					std::memset(static_cast<unsigned char*>([terminal contents])+
						manifoldReductionOffset,0,manifoldReductionBytes);
					std::memset([manifoldQuantileScratch contents],0,
						MetalManifoldQuantileScratchWords*sizeof(std::uint32_t));
					encoder=[terminalCommand computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:manifoldBeginningDeviation offset:0 atIndex:0];
					[encoder setBuffer:cell.conservativeValues offset:0 atIndex:1];
					[encoder setBuffer:manifoldThermochemistry offset:0 atIndex:2];
					[encoder setBuffer:manifoldMap offset:0 atIndex:3];
					[encoder setBuffer:terminal offset:manifoldReductionOffset atIndex:4];
					[encoder setBuffer:manifoldParametersBuffer offset:0 atIndex:5];
					[encoder setBuffer:manifoldQuantileScratch offset:0 atIndex:6];
					Dispatch(encoder,context.measureMethaneManifold,cells);[encoder endEncoding];
					encoder=[terminalCommand computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:manifoldQuantileScratch offset:0 atIndex:0];
					[encoder setBuffer:manifoldQuantileScratch
						offset:MetalManifoldQuantileControlOffset atIndex:1];
					[encoder setBuffer:manifoldParametersBuffer offset:0 atIndex:2];
					Dispatch(encoder,context.selectMethaneManifoldHighBins,1u);[encoder endEncoding];
					encoder=[terminalCommand computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:manifoldMap offset:0 atIndex:0];
					[encoder setBuffer:manifoldQuantileScratch
						offset:MetalManifoldQuantileControlOffset atIndex:1];
					[encoder setBuffer:manifoldQuantileScratch
						offset:MetalManifoldQuantileBins*sizeof(std::uint32_t) atIndex:2];
					[encoder setBuffer:manifoldParametersBuffer offset:0 atIndex:3];
					Dispatch(encoder,context.histogramMethaneManifoldLowBins,cells);
					[encoder endEncoding];
					encoder=[terminalCommand computeCommandEncoder];if( !encoder ) return false;
					[encoder setBuffer:manifoldQuantileScratch
						offset:MetalManifoldQuantileBins*sizeof(std::uint32_t) atIndex:0];
					[encoder setBuffer:manifoldQuantileScratch
						offset:MetalManifoldQuantileControlOffset atIndex:1];
					Dispatch(encoder,context.selectMethaneManifoldLowBins,1u);[encoder endEncoding];
				}
				blit=[terminalCommand blitCommandEncoder];if( !blit ) return false;
				[blit copyFromBuffer:cell.conservativeValues sourceOffset:0 toBuffer:terminal
					destinationOffset:0 size:cellValueBytes];
				[blit copyFromBuffer:dual.packedAuxiliaryFaceDensity sourceOffset:0 toBuffer:terminal
					destinationOffset:cellValueBytes size:packedFaceBytes];
				[blit copyFromBuffer:dual.packedMomentum sourceOffset:0 toBuffer:terminal
					destinationOffset:cellValueBytes+packedFaceBytes size:packedFaceBytes];
				[blit endEncoding];CommitTrackedMetalCommand(terminalCommand);[terminalCommand waitUntilCompleted];
				if( [terminalCommand status]!=MTLCommandBufferStatusCompleted ) return false;
				timestepVelocityAuditTerminalMS=timestepVelocityAuditMS();
				float* values=static_cast<float*>(ReadTrackedMetalBuffer(terminal));
				const std::uint32_t* manifoldReduction=measureManifold?
					reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const unsigned char*>(values)+
						manifoldReductionOffset):0;
				const char* injected=std::getenv("RISE_FIRE_PRODUCTION_STEP_FAILURE");
				if( values&&injected&&std::strcmp(injected,"terminal-nonfinite")==0 )
					values[0]=std::numeric_limits<float>::quiet_NaN();
				bool terminalValid=values!=0;
				std::size_t invalidTerminalCell=cells,invalidTerminalFace=allFaces;
				for( std::size_t cell=0u;terminalValid&&cell<cells;++cell ) {
					float gas=values[cells+cell];
					for( std::size_t component=0u;component<9u;++component )
						terminalValid=terminalValid&&std::isfinite(values[component*cells+cell]);
					for( std::size_t component=2u;component<=6u;++component )
						gas+=values[component*cells+cell];
					terminalValid=terminalValid&&gas>0.0f&&std::isfinite(gas);
					if(!terminalValid)invalidTerminalCell=cell;
				}
				for( std::size_t face=0u;terminalValid&&face<allFaces;++face ){
					terminalValid=std::isfinite(values[9u*cells+face])&&
						values[9u*cells+face]>0.0f&&
						std::isfinite(values[9u*cells+allFaces+face]);
					if(!terminalValid)invalidTerminalFace=face;
				}
				if( !terminalValid ) {
					if(structuredError){
						if(!values)*structuredError="production resident terminal readback is unavailable";
						else if(invalidTerminalCell<cells)*structuredError=
							"production resident terminal cell is invalid at "+
							std::to_string(invalidTerminalCell);
						else *structuredError="production resident terminal face is invalid at "+
							std::to_string(invalidTerminalFace);
					}
					if( manifoldStageBudgetActivation&&values ) {
						for(std::size_t cellIndex=0u;cellIndex<cells;++cellIndex){
							float gas=values[cells+cellIndex];bool finite=true;
							for(std::size_t component=0u;component<9u;++component)
								finite=finite&&std::isfinite(values[component*cells+cellIndex]);
							for(std::size_t component=2u;component<=6u;++component)
								gas+=values[component*cells+cellIndex];
							if(!finite||!(gas>0.0f)||!std::isfinite(gas)){std::fprintf(stderr,
								"MANIFOLD_OWNER invalid_cell=%zu gas=%.9g energy=%.9g\n",
								cellIndex,gas,values[8u*cells+cellIndex]);break;}
						}
						for(std::size_t face=0u;face<allFaces;++face)if(
							!std::isfinite(values[9u*cells+face])||
							!(values[9u*cells+face]>0.0f)||
							!std::isfinite(values[9u*cells+allFaces+face])){
							std::fprintf(stderr,"MANIFOLD_OWNER invalid_face=%zu density=%.9g momentum=%.9g\n",
								face,values[9u*cells+face],values[9u*cells+allFaces+face]);break;}
					}
					return false;
				}
				timestepVelocityAuditTerminalValidationMS=timestepVelocityAuditMS();
				markPhase("production resident step failed while validating resident terminal state");
				const bool enforcePlateau=request.enforceManifoldPlateau&&
					!restorationRemoved&&!plateauEvidenceEnabled;
				float maximumManifoldGenerationFloat=0.0f,maximumTerminalDeviationFloat=0.0f;
				float maximumDynamicsDoseScaleFloat=0.0f;
				float terminalDeviationP50Float=0.0f,terminalDeviationP95Float=0.0f;
				if( measureManifold&&manifoldReduction ) {
					std::memcpy(&maximumManifoldGenerationFloat,manifoldReduction,sizeof(float));
					std::memcpy(&maximumTerminalDeviationFloat,manifoldReduction+1u,sizeof(float));
					std::memcpy(&maximumDynamicsDoseScaleFloat,manifoldReduction+3u,sizeof(float));
					const std::uint32_t* quantileControl=reinterpret_cast<const std::uint32_t*>(
						static_cast<const unsigned char*>([manifoldQuantileScratch contents])+
						MetalManifoldQuantileControlOffset);
					std::memcpy(&terminalDeviationP50Float,quantileControl+4u,sizeof(float));
					std::memcpy(&terminalDeviationP95Float,quantileControl+5u,sizeof(float));
				}
				const double maximumManifoldGeneration=maximumManifoldGenerationFloat;
				const double maximumTerminalDeviation=maximumTerminalDeviationFloat;
				const double terminalDeviationP50=terminalDeviationP50Float;
				const double terminalDeviationP95=terminalDeviationP95Float;
				double maximumDynamicsDoseScale=maximumDynamicsDoseScaleFloat;
				double canonicalDynamicsMaximum=0.0;
				const FireSimulationMethaneRecord& dynamicsFuel=
					FireSimulationMethaneRecord::PhysicalV1();
				for(std::size_t cell=0u;cell<cells;++cell){
					std::array<double,9> terminal;
					for(std::size_t component=0u;component<9u;++component)
						terminal[component]=values[component*cells+cell];
					double ratio=0.0;
					if(!dynamicsFuel.AcceptedConservativeVolumeRatioByComponentOrder(
						terminal.data(),terminal.size(),FireStateProducerPrecision::Binary32,
						ratio,structuredError)){
						if(structuredError){const std::string detail=*structuredError;
							*structuredError="production canonical dynamics recheck rejected cell "+
								std::to_string(cell)+(detail.empty()?"":": "+detail);}
						return false;
					}
					const double magnitude=std::fabs(ratio-1.0);
					canonicalDynamicsMaximum=std::max(canonicalDynamicsMaximum,magnitude);
					if(magnitude>0x1p-2){
						const double beginningMagnitude=std::fabs(
							request.beginningManifoldDeviationPerCell[cell]);
						const double headroom=0x1p-2-beginningMagnitude;
						const double localDose=magnitude-beginningMagnitude;
						if(!(headroom>0.0)||!(localDose>0.0)||!std::isfinite(headroom)||
							!std::isfinite(localDose)){
							if(structuredError)*structuredError=
								"production canonical dynamics-bound dose is not reducible";
							return false;
						}
						maximumDynamicsDoseScale=std::max(maximumDynamicsDoseScale,
							localDose/headroom);
					}
				}
				FireProductionRestorationPlateauValidation plateauValidation;
				if( measureManifold&&(!manifoldReduction||manifoldReduction[2u]!=0u||
					!std::isfinite(maximumManifoldGeneration)||
					!std::isfinite(maximumTerminalDeviation)||
					!std::isfinite(terminalDeviationP50)||!std::isfinite(terminalDeviationP95)||
					terminalDeviationP50<0.0||terminalDeviationP95<terminalDeviationP50||
					maximumTerminalDeviation<terminalDeviationP95||
					(enforcePlateau&&!FireProductionRestorationPlateauWithinBand(maximumManifoldGeneration,
						projection.maximumPreProjectionResidualPerS,
						projection.maximumPostProjectionResidualPerS,plateauValidation))) ) return false;
				constexpr double lowMachValidityCeiling=0x1p-5;
				constexpr double lowMachPlateauAllowance=(1.0-0x1p-2)*
					lowMachValidityCeiling;
				constexpr double monitoredDynamicsValidityBound=0x1p-2;
				const bool dynamicsBoundPassed=!measureManifold||
					(maximumTerminalDeviation<=monitoredDynamicsValidityBound&&
					 canonicalDynamicsMaximum<=monitoredDynamicsValidityBound);
				if(measureManifold&&!dynamicsBoundPassed&&
					(!std::isfinite(maximumDynamicsDoseScale)||maximumDynamicsDoseScale<=1.0))
					return false;
				const bool plateauPassed=dynamicsBoundPassed&&(!enforcePlateau||
					(plateauValidation.mechanismPassed&&
					maximumTerminalDeviation<=lowMachPlateauAllowance));
				FireProductionResidentStepResult computed;
				computed.conservativeValues.assign(values,values+9u*cells);
				for( unsigned int axis=0u;axis<3u;++axis ) {
					const std::size_t beginning=faceOffsets[axis]/sizeof(float);
					computed.transportedDual.auxiliaryFaceDensity[axis].assign(
						values+9u*cells+beginning,values+9u*cells+beginning+faceCounts[axis]);
					computed.transportedDual.momentum[axis].assign(values+9u*cells+allFaces+beginning,
						values+9u*cells+allFaces+beginning+faceCounts[axis]);
				}
				timestepVelocityAuditResultCopyMS=timestepVelocityAuditMS();
				computed.physicalProjection=std::move(physicalProjection);
				computed.projection=std::move(projection);computed.forceSchedule=force.schedule;
				computed.forceDiagnostics=force.diagnostics;computed.cellSubmapCount=
					5u*executedAnomalyClosurePasses;
				computed.dualSubmapCount=dual.executedSubmapCount;computed.sourceCommandCommitCount=
					static_cast<std::uint32_t>(sourceCommits);
				computed.residentProjectionInvocationCount=
					restorationRemoved?computed.projection.residentProjectionInvocationCount:
					computed.physicalProjection.residentProjectionInvocationCount+
						std::max(1u,executedAnomalyClosurePasses-1u);
				computed.interstageFullGridTransferCount=
					force.diagnostics.substepLoopDeviceToHostTransferCount+
					cell.interstageFullGridTransferCount+dual.interstageFullGridTransferCount+
					(restorationRemoved?computed.projection.residentInterstageDeviceToHostTransferCount:
						computed.physicalProjection.residentInterstageDeviceToHostTransferCount+
							computed.projection.residentInterstageDeviceToHostTransferCount);
				computed.terminalStagingCount=
					(restorationRemoved?computed.projection.residentTerminalStagingCount:
						computed.physicalProjection.residentTerminalStagingCount+
							std::max(1u,executedAnomalyClosurePasses-1u))+1u;
				computed.combinedCertifiedWorkingSetBytes=certified;
				computed.combinedActualMetalAllocationBytes=ownerActualMetalBytes+
					force.diagnostics.actualMetalAllocationBytes+
					cell.actualMetalAllocationBytes+
					(executedAnomalyClosurePasses>1u?predictorCellActualMetalBytes:0u)+
					dual.actualMetalAllocationBytes+
					(restorationRemoved?computed.projection.residentActualMetalAllocationBytes:
						computed.physicalProjection.residentActualMetalAllocationBytes+
							computed.projection.residentActualMetalAllocationBytes);
				computed.deviceElapsedMS=force.diagnostics.advanceDeviceElapsedMS+
					predictorCellDeviceMS+anomalyCorrectorCellDeviceMS+
					dual.deviceElapsedMS+([sourceCommand GPUEndTime]-[sourceCommand GPUStartTime])*1000.0+
					anomalyPredictorDeviceMS+anomalyCorrectorSourceDeviceMS+
					(restorationRemoved?computed.projection.deviceElapsedMS:
						computed.physicalProjection.deviceElapsedMS+anomalyRestorationDeviceMS)+
					([terminalCommand GPUEndTime]-[terminalCommand GPUStartTime])*1000.0;
				double deviceStartTimeS=[upload GPUStartTime],deviceEndTimeS=[terminalCommand GPUEndTime];
				auto includeDeviceWindow=[&](const double beginning,const double end) {
					if(!(beginning>0.0)||!std::isfinite(beginning)||end<beginning||!std::isfinite(end))
						return false;
					deviceStartTimeS=std::min(deviceStartTimeS,beginning);
					deviceEndTimeS=std::max(deviceEndTimeS,end);return true;
				};
				const bool completeDeviceWindow=
					includeDeviceWindow(force.diagnostics.deviceStartTimeS,
						force.diagnostics.deviceEndTimeS)&&
					includeDeviceWindow(predictorCellDeviceStartTimeS,predictorCellDeviceEndTimeS)&&
					includeDeviceWindow(dual.deviceStartTimeS,dual.deviceEndTimeS)&&
					includeDeviceWindow([sourceCommand GPUStartTime],[sourceCommand GPUEndTime])&&
					includeDeviceWindow(restorationRemoved?computed.projection.deviceStartTimeS:
						anomalyRestorationDeviceStartTimeS,
						restorationRemoved?computed.projection.deviceEndTimeS:
						anomalyRestorationDeviceEndTimeS)&&
					(restorationRemoved||includeDeviceWindow(computed.physicalProjection.deviceStartTimeS,
						computed.physicalProjection.deviceEndTimeS))&&
					(!closeAdvectiveAnomaly||executedAnomalyClosurePasses==1u||
						(includeDeviceWindow(anomalyPredictorDeviceStartTimeS,
							anomalyPredictorDeviceEndTimeS)&&
						  (includeDeviceWindow(anomalyCorrectorCellDeviceStartTimeS,
							anomalyCorrectorCellDeviceEndTimeS)&&
						   includeDeviceWindow(anomalyCorrectorSourceDeviceStartTimeS,
							anomalyCorrectorSourceDeviceEndTimeS))));
				if(!completeDeviceWindow)return false;
				computed.deviceMakespanMS=(deviceEndTimeS-deviceStartTimeS)*1000.0;
				computed.representedTimeStepS=request.force.timeStepS;
				computed.maximumManifoldGeneration=maximumManifoldGeneration;
				computed.maximumAcceptedManifoldDeviation=!dynamicsBoundPassed?
					std::max(maximumTerminalDeviation,canonicalDynamicsMaximum):
					maximumTerminalDeviation;
				computed.acceptedManifoldDeviationP95=terminalDeviationP95;
				computed.acceptedManifoldDeviationP50=terminalDeviationP50;
				computed.maximumPredictedAdvectiveManifoldAnomaly=
					maximumPredictedAdvectiveAnomalyFloat;
				computed.manifoldMapCellCount=measureManifold?
					static_cast<std::uint32_t>(cells):0u;
				computed.manifoldScalarDeviceToHostTransferCount=measureManifold?
					(1u+anomalyPredictorMeasurementCount):0u;
				computed.manifoldFullGridDeviceToHostTransferCount=0u;
				computed.advectiveAnomalyClosurePassCount=closeAdvectiveAnomaly?
					executedAnomalyClosurePasses:0u;
				computed.manifoldStageGeneration[0]=maximumManifoldGeneration;
				computed.manifoldStageGeneration[1]=0.0;
				computed.manifoldStageGeneration[2]=0.0;
				computed.requiredRestorationDrainFraction=
					plateauValidation.requiredDrainFraction;
				computed.deliveredRestorationDrainFraction=
					plateauValidation.deliveredDrainFraction;
				computed.restorationResidualBandPerS=
					plateauValidation.maximumPostResidualPerS;
				double suggestedManifoldTimeStepS=0.0;
				if(enforcePlateau)
					computed.manifoldNextTimeStepAvailable=DeriveFireProductionManifoldTimeStep(
						static_cast<double>(request.force.timeStepS),maximumManifoldGeneration,
						plateauValidation.deliveredDrainFraction,suggestedManifoldTimeStepS,0);
				else if(measureManifold&&!dynamicsBoundPassed){
					const float reduced=std::nextafter(static_cast<float>(
						static_cast<double>(request.force.timeStepS)/maximumDynamicsDoseScale),0.0f);
					computed.manifoldNextTimeStepAvailable=std::isfinite(reduced)&&reduced>0.0f&&
						reduced<request.force.timeStepS;
					suggestedManifoldTimeStepS=computed.manifoldNextTimeStepAvailable?
						static_cast<double>(reduced):0.0;
				}
				computed.suggestedManifoldTimeStepS=computed.manifoldNextTimeStepAvailable?
					suggestedManifoldTimeStepS:0.0;
				computed.manifoldDiagnosticsMonitored=measureManifold;
				computed.manifoldPlateauEnforced=enforcePlateau;
				computed.manifoldAllowanceExceeded=measureManifold&&
					maximumTerminalDeviation>lowMachPlateauAllowance;
				computed.manifoldCeilingExceeded=measureManifold&&
					maximumTerminalDeviation>lowMachValidityCeiling;
				computed.manifoldPlateauPassed=measureManifold&&plateauPassed;
				plateauRefused=enforcePlateau&&!plateauPassed&&
					!manifoldProbeActivation&&!manifoldStageBudgetActivation;
				if( enforcePlateau )
					computed.projection.validationPassed=plateauValidation.mechanismPassed;
				if( physicalValidationProbeEnabled )
					computed.physicalProjection.validationPassed=false;
				computed.conservativeProducerPrecision=FireStateProducerPrecision::Binary32;
				computed.acceptedShape=request.force.shape;
				timestepVelocityAuditMetadataMS=timestepVelocityAuditMS();
				const bool materialGenerationAuthority=
					FireProductionEulerianGenerationHasMaterialAuthority(
						request.beginningManifoldDeviationPerCell,
						request.cellTransport.frozenVelocityMPerS);
				computed.manifoldGenerationAuthoritative=materialGenerationAuthority;
				computed.manifoldTailRestorationApplied=targetedRestorationActive;
				computed.manifoldTailCellCount=manifoldTailTarget.outlierCellCount;
				computed.manifoldTailExcessSum=manifoldTailTarget.excessSum;
				computed.manifoldTailDrainedVolumeM3=manifoldTailTarget.drainedVolumeM3;
				computed.manifoldDynamicsBoundPassed=dynamicsBoundPassed;
				if( measureManifold&&!anomalyConvergenceProbeActivation&&
					FireProductionResidentStepEligibleForAcceptedManifoldToken(computed) ) {
					std::array<std::uint64_t,2> authorityDigests={{0u,0u}};
					auto deriveAuthorityDigest=[&](const std::size_t digestIndex){
						if(digestIndex==0u)authorityDigests[digestIndex]=
							ParallelAcceptedManifoldPayloadDigest(computed,serialOwnerValidation);
						else authorityDigests[digestIndex]=ParallelAcceptedStatePayloadDigestFast(
							computed.acceptedShape,computed.conservativeValues,
							computed.projection.momentumKGPerM2S,
							computed.projection.velocityMPerS,serialOwnerValidation);
					};
					if(serialOwnerValidation){deriveAuthorityDigest(0u);deriveAuthorityDigest(1u);}
					else Implementation::GlobalThreadPool().ParallelFor(
						authorityDigests.size(),deriveAuthorityDigest);
					computed.acceptedManifoldToken_.available_=true;
					computed.acceptedManifoldToken_.representedTimeStepS_=
						static_cast<double>(request.force.timeStepS);
					computed.acceptedManifoldToken_.maximumGeneration_=maximumManifoldGeneration;
					computed.acceptedManifoldToken_.maximumAcceptedDeviation_=maximumTerminalDeviation;
					computed.acceptedManifoldToken_.acceptedDeviationP95_=terminalDeviationP95;
					computed.acceptedManifoldToken_.acceptedDeviationP50_=terminalDeviationP50;
					computed.acceptedManifoldToken_.tailRestorationApplied_=
						targetedRestorationActive;
					computed.acceptedManifoldToken_.tailCellCount_=
						manifoldTailTarget.outlierCellCount;
					computed.acceptedManifoldToken_.tailExcessSum_=manifoldTailTarget.excessSum;
					computed.acceptedManifoldToken_.tailDrainedVolumeM3_=
						manifoldTailTarget.drainedVolumeM3;
					computed.acceptedManifoldToken_.dynamicsBoundPassed_=dynamicsBoundPassed;
					computed.acceptedManifoldToken_.requiredDrainFraction_=
						plateauValidation.requiredDrainFraction;
					computed.acceptedManifoldToken_.deliveredDrainFraction_=
						plateauValidation.deliveredDrainFraction;
					computed.acceptedManifoldToken_.maximumPostResidualPerS_=
						plateauValidation.maximumPostResidualPerS;
					const FireProductionProjectionResult& authorityProjection=
						(enforcePlateau||targetedRestorationActive)?
						computed.physicalProjection:computed.projection;
					computed.acceptedManifoldToken_.physicalMaximumPreResidualPerS_=
						authorityProjection.maximumPreProjectionResidualPerS;
					computed.acceptedManifoldToken_.physicalMaximumPostResidualPerS_=
						authorityProjection.maximumPostProjectionResidualPerS;
					computed.acceptedManifoldToken_.payloadDigest_=authorityDigests[0u];
					computed.acceptedManifoldToken_.acceptedStateDigest_=authorityDigests[1u];
					computed.acceptedManifoldToken_.acceptedStateDigestVersion_=2u;
					computed.acceptedManifoldToken_.generationAuthoritative_=
						materialGenerationAuthority;
					computed.acceptedManifoldToken_.plateauEnforced_=enforcePlateau;
				}
				timestepVelocityAuditAuthorityMS=timestepVelocityAuditMS();
				if( computed.cellSubmapCount!=5u*executedAnomalyClosurePasses||
					computed.dualSubmapCount!=15u||
					computed.sourceCommandCommitCount!=executedAnomalyClosurePasses||
					computed.residentProjectionInvocationCount!=(restorationRemoved?1u:
						1u+std::max(1u,executedAnomalyClosurePasses-1u))||
					computed.interstageFullGridTransferCount!=0u||
					MetalHostBufferReadCount-beginningReads!=
						1u+anomalyPredictorMeasurementCount||
					(measureManifold&&(computed.manifoldMapCellCount!=cells||
						computed.manifoldScalarDeviceToHostTransferCount!=
							1u+anomalyPredictorMeasurementCount||
						computed.manifoldFullGridDeviceToHostTransferCount!=0u||
						computed.manifoldStageGeneration[0]!=computed.maximumManifoldGeneration||
						computed.manifoldStageGeneration[1]!=0.0||
						computed.manifoldStageGeneration[2]!=0.0))||
					computed.combinedActualMetalAllocationBytes>certified||
					!std::isfinite(computed.deviceElapsedMS)||
					!std::isfinite(computed.deviceMakespanMS)||computed.deviceMakespanMS<0.0 ) return false;
				result=std::move(computed);
				if( timestepVelocityAuditEnabled||hostResidualProbeEnabled ) {
					const double completeMS=timestepVelocityAuditMS();
					if(hostResidualProbeEnabled)std::fprintf(stderr,
						"HOST_RESIDUAL_PREP dual_static=%.9g force=%.9g cell=%.9g parallel=%d\n",
						preparationWallMS[0u],preparationWallMS[1u],preparationWallMS[2u],
						serialIndependentPreparation?0:1);
					const char* wallLabel=hostResidualProbeEnabled?
						"HOST_RESIDUAL_WALL":"TIMESTEP_VELOCITY_WALL";
					std::fprintf(stderr,"%s preflight=%.9g dual_static=%.9g "
						"upload=%.9g force=%.9g cell=%.9g dual=%.9g source=%.9g "
						"physical_projection=%.9g restoration_projection=%.9g terminal=%.9g "
						"postprocess=%.9g total=%.9g\n",wallLabel,timestepVelocityAuditValidatedMS,
						timestepVelocityAuditDualStaticMS-timestepVelocityAuditValidatedMS,
						timestepVelocityAuditUploadMS-timestepVelocityAuditDualStaticMS,
						timestepVelocityAuditForceMS-timestepVelocityAuditUploadMS,
						timestepVelocityAuditCellMS-timestepVelocityAuditForceMS,
						timestepVelocityAuditDualMS-timestepVelocityAuditCellMS,
						timestepVelocityAuditSourceMS-timestepVelocityAuditDualMS,
						timestepVelocityAuditPhysicalMS-timestepVelocityAuditSourceMS,
						timestepVelocityAuditRestorationMS-timestepVelocityAuditPhysicalMS,
						timestepVelocityAuditTerminalMS-timestepVelocityAuditRestorationMS,
						completeMS-timestepVelocityAuditTerminalMS,completeMS);
					std::fprintf(stderr,"%s_POST validation=%.9g result_copy=%.9g "
						"metadata=%.9g authority=%.9g final_checks=%.9g\n",wallLabel,
						timestepVelocityAuditTerminalValidationMS-timestepVelocityAuditTerminalMS,
						timestepVelocityAuditResultCopyMS-timestepVelocityAuditTerminalValidationMS,
						timestepVelocityAuditMetadataMS-timestepVelocityAuditResultCopyMS,
						timestepVelocityAuditAuthorityMS-timestepVelocityAuditMetadataMS,
						completeMS-timestepVelocityAuditAuthorityMS);
					const double sourceDeviceMS=([sourceCommand GPUEndTime]-
						[sourceCommand GPUStartTime])*1000.0;
					const double terminalDeviceMS=([terminalCommand GPUEndTime]-
						[terminalCommand GPUStartTime])*1000.0;
					const char* deviceLabel=hostResidualProbeEnabled?
						"HOST_RESIDUAL_DEVICE":"TIMESTEP_VELOCITY_DEVICE";
					std::fprintf(stderr,"%s owner_upload=%.9g force=%.9g "
						"cell=%.9g dual=%.9g source=%.9g physical_projection=%.9g "
						"restoration_projection=%.9g terminal=%.9g work_total=%.9g makespan=%.9g\n",
						deviceLabel,timestepVelocityAuditOwnerUploadDeviceMS,
						force.diagnostics.advanceDeviceElapsedMS,cell.deviceElapsedMS,
						dual.deviceElapsedMS,sourceDeviceMS,
						restorationRemoved?projection.deviceElapsedMS:physicalProjection.deviceElapsedMS,
						restorationRemoved?0.0:projection.deviceElapsedMS,terminalDeviceMS,
						result.deviceElapsedMS,result.deviceMakespanMS);
				}
			}
			if(measureManifold&&!result.manifoldDynamicsBoundPassed){
				if(structuredError)*structuredError=
					"production realized manifold deviation exceeds the dynamics-validity bound";
				return false;
			}
			if(plateauRefused){
				if(structuredError)*structuredError=
					result.maximumAcceptedManifoldDeviation>0x1.8p-6?
						"production realized manifold deviation exceeds the low-Mach headroom allowance":
						"production restoration residual is amplified";
				return false;
			}
			if( structuredError ) structuredError->clear();attemptCompleted=true;return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionResidentStepResult();
			if( structuredError ) try { *structuredError=
				"production resident step allocation failed"; } catch( const std::bad_alloc& ) {}
			return false;
		}
	}

	bool AttemptFireProductionCompatibleMomentumDiagnosticMetal(
		const FireProductionResidentStepRequest& request,
		FireProductionResidentStepResult& result,
		std::string* structuredError )
	{
		result=FireProductionResidentStepResult();
		if( CompatibleMomentumDiagnosticActive ) {
			if( structuredError ) *structuredError=
				"compatible-momentum diagnostic recursion is invalid";
			return false;
		}
		struct ScopedDiagnostic
		{
			ScopedDiagnostic(){CompatibleMomentumDiagnosticActive=true;}
			~ScopedDiagnostic(){CompatibleMomentumDiagnosticActive=false;}
		} scoped;
		return AttemptFireProductionResidentStepMetal(request,result,structuredError);
	}

	namespace
	{
		class ResidentTransportMetalAuthority;
		class ResidentEndpointClassProducerAuthority;
		struct ResidentEndpointClassMetalAuthority;
		class ResidentPhysicalFluxMetalAuthority;
		class ResidentEOSCandidateMetalAuthority;
		class ResidentEOSMetalAuthority;
		class ResidentFrozenSourceMetalAuthority;
		class ResidentProjectionMetadataMetalAuthority;
		class ResidentProjectionTargetMetalAuthority;
		class ResidentProjectedHeunMetalOwner;
		bool EncodeResidentEOSQualificationCandidate(
			ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,id<MTLBuffer>,
			id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,
			id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,const ResidentTransportMetalAuthority&,
			const ResidentPhysicalFluxMetalAuthority&,const MetalResidentEOSParameters&,
			bool,bool,bool,bool,ResidentEOSCandidateMetalAuthority&,std::string*);
		bool EncodeResidentTransportAuthority(
			ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,
			id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,
			id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,std::size_t,std::size_t,
			std::size_t,ResidentTransportMetalAuthority&,std::string*);

		class ResidentTransportMetalAuthority
		{
			friend class ResidentProjectedHeunMetalOwner;
			friend bool EncodeResidentTransportAuthority(
				ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,std::size_t,std::size_t,
				std::size_t,ResidentTransportMetalAuthority&,std::string*);
			friend bool ::RISE::EvaluateFireProductionResidentTransportMetalComparator(
				const FireProductionResidentTransportComparatorRequest&,
				FireProductionResidentTransportComparatorResult&,std::string*);
			friend bool EncodeResidentPhysicalFluxAuthority(
				ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,const ResidentTransportMetalAuthority&,
				const ResidentEndpointClassMetalAuthority&,
				std::size_t,std::size_t,const MetalResidentPhysicalFluxParameters&,
				ResidentPhysicalFluxMetalAuthority&,std::string*);
			friend bool EncodeResidentEndpointClassAuthority(
				ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				const ResidentTransportMetalAuthority&,std::size_t,std::size_t,std::size_t,
				bool,const ResidentEndpointClassProducerAuthority*,
				ResidentEndpointClassMetalAuthority&,std::string*);
			friend bool ::RISE::EvaluateFireProductionResidentPhysicalFluxMetalComparator(
				const FireProductionResidentPhysicalFluxComparatorRequest&,
				FireProductionResidentPhysicalFluxComparatorResult&,std::string*);
			friend bool ::RISE::EvaluateFireProductionResidentEOSCandidateMetalComparator(
				const FireProductionResidentEOSCandidateComparatorRequest&,
				FireProductionResidentEOSCandidateComparatorResult&,std::string*);
			friend bool EncodeResidentEOSAuthority(
				ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,const ResidentTransportMetalAuthority&,
				const ResidentPhysicalFluxMetalAuthority&,
				const ResidentEOSCandidateMetalAuthority&,const MetalResidentEOSParameters&,
				ResidentEOSMetalAuthority&,std::string*);
			friend bool ::RISE::EvaluateFireProductionResidentTargetLineageMetalComparator(
				const FireProductionResidentTargetLineageComparatorRequest&,
				FireProductionResidentTargetLineageComparatorResult&,std::string*);
			friend bool EncodeResidentProjectionTargetAuthority(
				ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				const ResidentTransportMetalAuthority&,
				const ResidentPhysicalFluxMetalAuthority&,const ResidentPhysicalFluxMetalAuthority&,
				const ResidentEOSCandidateMetalAuthority&,
				const ResidentEOSMetalAuthority&,const ResidentFrozenSourceMetalAuthority&,
				const MetalResidentTargetParameters&,
				ResidentProjectionTargetMetalAuthority&,std::string*);
			friend bool EncodeResidentEOSQualificationCandidate(
				ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,const ResidentTransportMetalAuthority&,
				const ResidentPhysicalFluxMetalAuthority&,const MetalResidentEOSParameters&,
				bool,bool,bool,bool,ResidentEOSCandidateMetalAuthority&,std::string*);
		private:
			id<MTLBuffer> coefficients;
			id<MTLBuffer> publicationIdentity;
			id<MTLCommandBuffer> parentCommand;
			id<MTLBuffer> parentState;
			id<MTLBuffer> parentTemperature;
			id<MTLBuffer> parentVelocity;
			id<MTLBuffer> parentThermochemistry;
			id<MTLBuffer> parentParameters;
			id<MTLBuffer> parentFailure;
			id<MTLBuffer> parentObligations;
			std::size_t parentCells,parentAllFaces,parentBoundaryFaces;
			std::uint64_t allocationBytes;
			ResidentTransportMetalAuthority() : coefficients(nil),publicationIdentity(nil),
				parentCommand(nil),parentState(nil),parentTemperature(nil),parentVelocity(nil),
				parentThermochemistry(nil),parentParameters(nil),parentFailure(nil),
				parentObligations(nil),parentCells(0u),parentAllFaces(0u),parentBoundaryFaces(0u),
				allocationBytes(0u) {}
			ResidentTransportMetalAuthority(const ResidentTransportMetalAuthority&)=delete;
			ResidentTransportMetalAuthority& operator=(
				const ResidentTransportMetalAuthority&)=delete;
		};

		class ResidentEndpointClassProducerAuthority
		{
			friend class ResidentProjectedHeunMetalOwner;
			friend bool EncodeResidentEndpointClassAuthority(
				ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				const ResidentTransportMetalAuthority&,std::size_t,std::size_t,std::size_t,
				bool,const ResidentEndpointClassProducerAuthority*,
				ResidentEndpointClassMetalAuthority&,std::string*);
		private:
			id<MTLCommandBuffer> producingCommand;
			id<MTLBuffer> classes;
			id<MTLBuffer> projectionPublicationIdentity;
			id<MTLBuffer> transportParameters;
			id<MTLBuffer> ownerParameters;
			std::shared_ptr<unsigned char> capability;
			ResidentEndpointClassProducerAuthority() : producingCommand(nil),classes(nil),
				projectionPublicationIdentity(nil),transportParameters(nil),ownerParameters(nil) {}
			ResidentEndpointClassProducerAuthority(
				const ResidentEndpointClassProducerAuthority&)=delete;
			ResidentEndpointClassProducerAuthority& operator=(
				const ResidentEndpointClassProducerAuthority&)=delete;
		};

		struct ResidentEndpointClassMetalAuthority
		{
			id<MTLBuffer> classes;
			id<MTLBuffer> sealedClasses;
			id<MTLBuffer> publicationIdentity;
			id<MTLCommandBuffer> parentCommand;
			id<MTLBuffer> parentProjectionPublicationIdentity;
			id<MTLBuffer> parentTransportPublicationIdentity;
			id<MTLBuffer> parentTransportParameters;
			id<MTLBuffer> parentOwnerParameters;
			std::shared_ptr<unsigned char> producerCapability;
			std::size_t parentBoundaryFaces;
			std::size_t parentCells;
			std::size_t parentAllFaces;
			bool projectionBound;
			std::uint64_t allocationBytes;
			ResidentEndpointClassMetalAuthority() : classes(nil),sealedClasses(nil),
				publicationIdentity(nil),parentCommand(nil),
				parentProjectionPublicationIdentity(nil),
				parentTransportPublicationIdentity(nil),parentTransportParameters(nil),parentOwnerParameters(nil),
				parentBoundaryFaces(0u),parentCells(0u),parentAllFaces(0u),
				projectionBound(false),allocationBytes(0u) {}
			ResidentEndpointClassMetalAuthority(
				const ResidentEndpointClassMetalAuthority&)=delete;
			ResidentEndpointClassMetalAuthority& operator=(
				const ResidentEndpointClassMetalAuthority&)=delete;
		};

		bool EncodeResidentEndpointClassAuthority(
			ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,
			id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,
			const ResidentTransportMetalAuthority&,std::size_t,std::size_t,std::size_t,
			bool,const ResidentEndpointClassProducerAuthority*,
			ResidentEndpointClassMetalAuthority&,std::string*);

		bool EncodeResidentPhysicalFluxAuthority(
			ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,
			id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,
			id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,
			id<MTLBuffer>,id<MTLBuffer>,const ResidentTransportMetalAuthority&,
			const ResidentEndpointClassMetalAuthority&,
			std::size_t,std::size_t,const MetalResidentPhysicalFluxParameters&,
			ResidentPhysicalFluxMetalAuthority&,std::string*);

		class ResidentPhysicalFluxMetalAuthority
		{
			friend class ResidentProjectedHeunMetalOwner;
			friend bool EncodeResidentPhysicalFluxAuthority(
				ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,const ResidentTransportMetalAuthority&,
				const ResidentEndpointClassMetalAuthority&,
				std::size_t,std::size_t,const MetalResidentPhysicalFluxParameters&,
				ResidentPhysicalFluxMetalAuthority&,std::string*);
			friend bool ::RISE::EvaluateFireProductionResidentPhysicalFluxMetalComparator(
				const FireProductionResidentPhysicalFluxComparatorRequest&,
				FireProductionResidentPhysicalFluxComparatorResult&,std::string*);
			friend bool ::RISE::EvaluateFireProductionResidentEOSCandidateMetalComparator(
				const FireProductionResidentEOSCandidateComparatorRequest&,
				FireProductionResidentEOSCandidateComparatorResult&,std::string*);
			friend bool EncodeResidentEOSAuthority(
				ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,const ResidentTransportMetalAuthority&,
				const ResidentPhysicalFluxMetalAuthority&,
				const ResidentEOSCandidateMetalAuthority&,const MetalResidentEOSParameters&,
				ResidentEOSMetalAuthority&,std::string*);
			friend bool ::RISE::EvaluateFireProductionResidentTargetLineageMetalComparator(
				const FireProductionResidentTargetLineageComparatorRequest&,
				FireProductionResidentTargetLineageComparatorResult&,std::string*);
			friend bool EncodeResidentProjectionTargetAuthority(
				ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				const ResidentTransportMetalAuthority&,
				const ResidentPhysicalFluxMetalAuthority&,const ResidentPhysicalFluxMetalAuthority&,
				const ResidentEOSCandidateMetalAuthority&,
				const ResidentEOSMetalAuthority&,const ResidentFrozenSourceMetalAuthority&,
				const MetalResidentTargetParameters&,
				ResidentProjectionTargetMetalAuthority&,std::string*);
			friend bool EncodeResidentEOSQualificationCandidate(
				ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,const ResidentTransportMetalAuthority&,
				const ResidentPhysicalFluxMetalAuthority&,const MetalResidentEOSParameters&,
				bool,bool,bool,bool,ResidentEOSCandidateMetalAuthority&,std::string*);
		private:
			id<MTLBuffer> donorAdvective;
			id<MTLBuffer> advectiveDelta;
			id<MTLBuffer> mcMusclAdvective;
			id<MTLBuffer> physicalMass;
			id<MTLBuffer> physicalEnergy;
			id<MTLBuffer> physicalGas;
			id<MTLBuffer> faceLogTemperature;
			id<MTLBuffer> faceSensibleEnthalpy;
			id<MTLBuffer> lowComposite;
			id<MTLBuffer> highComposite;
			id<MTLBuffer> publicationIdentity;
			id<MTLCommandBuffer> parentCommand;
			id<MTLBuffer> parentState;
			id<MTLBuffer> parentThermochemistry;
			id<MTLBuffer> parentTransportPublicationIdentity;
			id<MTLBuffer> parentEndpointClassPublicationIdentity;
			std::size_t parentCells,parentAllFaces;
			std::uint64_t allocationBytes;
			ResidentPhysicalFluxMetalAuthority() : donorAdvective(nil),advectiveDelta(nil),
				mcMusclAdvective(nil),physicalMass(nil),physicalEnergy(nil),physicalGas(nil),
				faceLogTemperature(nil),faceSensibleEnthalpy(nil),lowComposite(nil),
				highComposite(nil),publicationIdentity(nil),parentCommand(nil),parentState(nil),
				parentThermochemistry(nil),parentTransportPublicationIdentity(nil),
				parentEndpointClassPublicationIdentity(nil),
				parentCells(0u),parentAllFaces(0u),
				allocationBytes(0u) {}
			ResidentPhysicalFluxMetalAuthority(
				const ResidentPhysicalFluxMetalAuthority&)=delete;
			ResidentPhysicalFluxMetalAuthority& operator=(
				const ResidentPhysicalFluxMetalAuthority&)=delete;
		};

		class ResidentEOSCandidateMetalAuthority
		{
			friend class ResidentProjectedHeunMetalOwner;
			friend bool EncodeResidentEOSAuthority(
				ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,const ResidentTransportMetalAuthority&,
				const ResidentPhysicalFluxMetalAuthority&,
				const ResidentEOSCandidateMetalAuthority&,const MetalResidentEOSParameters&,
				ResidentEOSMetalAuthority&,std::string*);
			friend bool ::RISE::EvaluateFireProductionResidentEOSCandidateMetalComparator(
				const FireProductionResidentEOSCandidateComparatorRequest&,
				FireProductionResidentEOSCandidateComparatorResult&,std::string*);
			friend bool EncodeResidentEOSQualificationCandidate(
				ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,const ResidentTransportMetalAuthority&,
				const ResidentPhysicalFluxMetalAuthority&,const MetalResidentEOSParameters&,
				bool,bool,bool,bool,ResidentEOSCandidateMetalAuthority&,std::string*);
			friend bool ::RISE::EvaluateFireProductionResidentTargetLineageMetalComparator(
				const FireProductionResidentTargetLineageComparatorRequest&,
				FireProductionResidentTargetLineageComparatorResult&,std::string*);
			friend bool EncodeResidentFrozenSourceAuthority(
				ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				const FireProductionFrozenSourcePacketSeal&,
				const ResidentEOSCandidateMetalAuthority&,const MetalResidentTargetParameters&,
				id<MTLBuffer>,id<MTLBuffer>,
				ResidentFrozenSourceMetalAuthority&,std::string*);
			friend bool EncodeResidentProjectionTargetAuthority(
				ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				const ResidentTransportMetalAuthority&,
				const ResidentPhysicalFluxMetalAuthority&,const ResidentPhysicalFluxMetalAuthority&,
				const ResidentEOSCandidateMetalAuthority&,
				const ResidentEOSMetalAuthority&,const ResidentFrozenSourceMetalAuthority&,
				const MetalResidentTargetParameters&,
				ResidentProjectionTargetMetalAuthority&,std::string*);
		private:
			id<MTLBuffer> conservative;
			id<MTLBuffer> sourceDelta;
			id<MTLBuffer> faceAlpha;
			id<MTLBuffer> producerIdentity;
			id<MTLBuffer> publicationIdentity;
			id<MTLCommandBuffer> parentCommand;
			id<MTLBuffer> parentPhysicalPublicationIdentity;
			id<MTLBuffer> parentTransportPublicationIdentity;
			id<MTLBuffer> parentEOSThermochemistry;
			id<MTLBuffer> parentEOSParameters;
			id<MTLBuffer> parentFCTParameters;
			std::shared_ptr<unsigned char> capability,parentCandidateCapability;
			bool rootParent;
			std::size_t cells;
			std::uint64_t allocationBytes;
			ResidentEOSCandidateMetalAuthority() : conservative(nil),sourceDelta(nil),faceAlpha(nil),
				producerIdentity(nil),publicationIdentity(nil),
				parentCommand(nil),parentPhysicalPublicationIdentity(nil),
				parentTransportPublicationIdentity(nil),parentEOSThermochemistry(nil),
				parentEOSParameters(nil),parentFCTParameters(nil),
				rootParent(false),cells(0u),allocationBytes(0u) {}
			ResidentEOSCandidateMetalAuthority(const ResidentEOSCandidateMetalAuthority&)=delete;
			ResidentEOSCandidateMetalAuthority& operator=(
				const ResidentEOSCandidateMetalAuthority&)=delete;
		};

		bool EncodeResidentEOSAuthority(
			ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,
			id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,const ResidentTransportMetalAuthority&,
			const ResidentPhysicalFluxMetalAuthority&,
			const ResidentEOSCandidateMetalAuthority&,const MetalResidentEOSParameters&,
			ResidentEOSMetalAuthority&,std::string*);

		class ResidentEOSMetalAuthority
		{
			friend class ResidentProjectedHeunMetalOwner;
			friend bool EncodeResidentEOSAuthority(
				ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,const ResidentTransportMetalAuthority&,
				const ResidentPhysicalFluxMetalAuthority&,
				const ResidentEOSCandidateMetalAuthority&,const MetalResidentEOSParameters&,
				ResidentEOSMetalAuthority&,std::string*);
			friend bool ::RISE::EvaluateFireProductionResidentEOSCandidateMetalComparator(
				const FireProductionResidentEOSCandidateComparatorRequest&,
				FireProductionResidentEOSCandidateComparatorResult&,std::string*);
			friend bool ::RISE::EvaluateFireProductionResidentTargetLineageMetalComparator(
				const FireProductionResidentTargetLineageComparatorRequest&,
				FireProductionResidentTargetLineageComparatorResult&,std::string*);
			friend bool EncodeResidentProjectionTargetAuthority(
				ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				const ResidentTransportMetalAuthority&,
				const ResidentPhysicalFluxMetalAuthority&,const ResidentPhysicalFluxMetalAuthority&,
				const ResidentEOSCandidateMetalAuthority&,
				const ResidentEOSMetalAuthority&,const ResidentFrozenSourceMetalAuthority&,
				const MetalResidentTargetParameters&,
				ResidentProjectionTargetMetalAuthority&,std::string*);
			id<MTLBuffer> temperature;
			id<MTLBuffer> representedPressureRatio;
			id<MTLBuffer> absoluteDeviation;
			id<MTLBuffer> publicationIdentity;
			id<MTLBuffer> firstFailureCell;
			id<MTLBuffer> failureTerm;
			id<MTLCommandBuffer> parentCommand;
			id<MTLBuffer> parentTransportPublicationIdentity;
			id<MTLBuffer> parentPhysicalPublicationIdentity;
			id<MTLBuffer> parentCandidatePublicationIdentity;
			std::uint64_t allocationBytes;
			ResidentEOSMetalAuthority() : temperature(nil),representedPressureRatio(nil),
				absoluteDeviation(nil),publicationIdentity(nil),firstFailureCell(nil),failureTerm(nil),
				parentCommand(nil),
				parentTransportPublicationIdentity(nil),parentPhysicalPublicationIdentity(nil),
				parentCandidatePublicationIdentity(nil),allocationBytes(0u) {}
			ResidentEOSMetalAuthority(const ResidentEOSMetalAuthority&)=delete;
			ResidentEOSMetalAuthority& operator=(const ResidentEOSMetalAuthority&)=delete;
		};

		bool EncodeResidentFrozenSourceAuthority(
			ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,id<MTLBuffer>,
			const FireProductionFrozenSourcePacketSeal&,
			const ResidentEOSCandidateMetalAuthority&,const MetalResidentTargetParameters&,
			id<MTLBuffer>,id<MTLBuffer>,
			ResidentFrozenSourceMetalAuthority&,std::string*);

		class ResidentFrozenSourceMetalAuthority
		{
			friend class ResidentProjectedHeunMetalOwner;
			friend bool EncodeResidentFrozenSourceAuthority(
				ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				const FireProductionFrozenSourcePacketSeal&,
				const ResidentEOSCandidateMetalAuthority&,const MetalResidentTargetParameters&,
				id<MTLBuffer>,id<MTLBuffer>,
				ResidentFrozenSourceMetalAuthority&,std::string*);
			friend bool EncodeResidentProjectionTargetAuthority(
				ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				const ResidentTransportMetalAuthority&,const ResidentPhysicalFluxMetalAuthority&,
				const ResidentPhysicalFluxMetalAuthority&,const ResidentEOSCandidateMetalAuthority&,
				const ResidentEOSMetalAuthority&,
				const ResidentFrozenSourceMetalAuthority&,const MetalResidentTargetParameters&,
				ResidentProjectionTargetMetalAuthority&,std::string*);
			friend bool ::RISE::EvaluateFireProductionResidentTargetLineageMetalComparator(
				const FireProductionResidentTargetLineageComparatorRequest&,
				FireProductionResidentTargetLineageComparatorResult&,std::string*);
		private:
			id<MTLBuffer> inputUpload;
			id<MTLBuffer> parameterUpload;
			id<MTLBuffer> values;
			id<MTLBuffer> issuedValues;
			id<MTLBuffer> metadata;
			id<MTLBuffer> issuedMetadata;
			id<MTLBuffer> publicationIdentity;
			id<MTLCommandBuffer> parentCommand;
			id<MTLBuffer> parentCandidatePublicationIdentity;
			std::size_t cells;
			std::uint64_t allocationBytes,liveAllocationBytes;
			ResidentFrozenSourceMetalAuthority() : inputUpload(nil),parameterUpload(nil),values(nil),
				issuedValues(nil),metadata(nil),
				issuedMetadata(nil),publicationIdentity(nil),parentCommand(nil),
				parentCandidatePublicationIdentity(nil),cells(0u),allocationBytes(0u),
				liveAllocationBytes(0u) {}
			ResidentFrozenSourceMetalAuthority(const ResidentFrozenSourceMetalAuthority&)=delete;
			ResidentFrozenSourceMetalAuthority& operator=(
				const ResidentFrozenSourceMetalAuthority&)=delete;
		};

		bool EncodeResidentProjectionTargetAuthority(
			ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,id<MTLBuffer>,
			id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,
			const ResidentTransportMetalAuthority&,
			const ResidentPhysicalFluxMetalAuthority&,const ResidentPhysicalFluxMetalAuthority&,
			const ResidentEOSCandidateMetalAuthority&,
			const ResidentEOSMetalAuthority&,const ResidentFrozenSourceMetalAuthority&,
			const MetalResidentTargetParameters&,
			ResidentProjectionTargetMetalAuthority&,std::string*);

		class ResidentProjectionMetadataMetalAuthority
		{
			friend bool EncodeResidentProjectionTargetAuthority(
				ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				const ResidentTransportMetalAuthority&,
				const ResidentPhysicalFluxMetalAuthority&,const ResidentPhysicalFluxMetalAuthority&,
				const ResidentEOSCandidateMetalAuthority&,
				const ResidentEOSMetalAuthority&,const ResidentFrozenSourceMetalAuthority&,
				const MetalResidentTargetParameters&,
				ResidentProjectionTargetMetalAuthority&,std::string*);
		private:
			id<MTLBuffer> metadata;
			id<MTLBuffer> publicationIdentity;
			id<MTLCommandBuffer> parentCommand;
			id<MTLBuffer> parentTargetPublicationIdentity;
			ResidentProjectionMetadataMetalAuthority() : metadata(nil),publicationIdentity(nil),
				parentCommand(nil),parentTargetPublicationIdentity(nil) {}
			ResidentProjectionMetadataMetalAuthority(
				const ResidentProjectionMetadataMetalAuthority&)=delete;
			ResidentProjectionMetadataMetalAuthority& operator=(
				const ResidentProjectionMetadataMetalAuthority&)=delete;
		};

		class ResidentProjectionTargetMetalAuthority
		{
			friend class ResidentProjectedHeunMetalOwner;
			friend bool EncodeResidentProjectionTargetAuthority(
				ResidentTransportMetalContext&,id<MTLCommandBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,id<MTLBuffer>,
				const ResidentTransportMetalAuthority&,
				const ResidentPhysicalFluxMetalAuthority&,const ResidentPhysicalFluxMetalAuthority&,
				const ResidentEOSCandidateMetalAuthority&,
				const ResidentEOSMetalAuthority&,const ResidentFrozenSourceMetalAuthority&,
				const MetalResidentTargetParameters&,
				ResidentProjectionTargetMetalAuthority&,std::string*);
			friend bool ::RISE::EvaluateFireProductionResidentTargetLineageMetalComparator(
				const FireProductionResidentTargetLineageComparatorRequest&,
				FireProductionResidentTargetLineageComparatorResult&,std::string*);
		private:
			id<MTLBuffer> tangent;
			id<MTLBuffer> frozenSource;
			id<MTLBuffer> absoluteDiagnostic;
			id<MTLBuffer> monitoredAbsolute;
			id<MTLBuffer> baseAssembled;
			id<MTLBuffer> assembled;
			id<MTLBuffer> tangentEnclosure;
			id<MTLBuffer> assembledEnclosure;
			id<MTLBuffer> publicationIdentity;
			id<MTLBuffer> projectionConsumerIdentity;
			id<MTLBuffer> consumerIdentity;
			id<MTLBuffer> projectionMetadata;
			id<MTLCommandBuffer> parentCommand;
			id<MTLBuffer> parentTransportPublicationIdentity;
			id<MTLBuffer> parentTangentPhysicalPublicationIdentity;
			id<MTLBuffer> parentPhysicalPublicationIdentity;
			id<MTLBuffer> parentCandidatePublicationIdentity;
			id<MTLBuffer> parentEOSPublicationIdentity;
			id<MTLBuffer> parentFrozenSourcePublicationIdentity;
			std::shared_ptr<unsigned char> capability,parentTargetCapability;
			bool bootstrapAuthority;
			std::uint32_t correctionIteration;
			std::size_t cells;
			std::uint64_t allocationBytes;
			ResidentProjectionTargetMetalAuthority() : tangent(nil),frozenSource(nil),
				absoluteDiagnostic(nil),monitoredAbsolute(nil),baseAssembled(nil),assembled(nil),
				tangentEnclosure(nil),assembledEnclosure(nil),publicationIdentity(nil),
				projectionConsumerIdentity(nil),consumerIdentity(nil),projectionMetadata(nil),parentCommand(nil),
				parentTransportPublicationIdentity(nil),
				parentTangentPhysicalPublicationIdentity(nil),
				parentPhysicalPublicationIdentity(nil),parentCandidatePublicationIdentity(nil),
				parentEOSPublicationIdentity(nil),parentFrozenSourcePublicationIdentity(nil),
				bootstrapAuthority(false),correctionIteration(0u),
				cells(0u),allocationBytes(0u) {}
			ResidentProjectionTargetMetalAuthority(
				const ResidentProjectionTargetMetalAuthority&)=delete;
			ResidentProjectionTargetMetalAuthority& operator=(
				const ResidentProjectionTargetMetalAuthority&)=delete;
		};

		bool ValidResidentTransportStage(const FireProductionProjectedHeunStage stage)
		{
			return stage==FireProductionProjectedHeunStage::R0||
				stage==FireProductionProjectedHeunStage::R1||
				stage==FireProductionProjectedHeunStage::R2;
		}

		bool PrepareResidentTransportRequest(
			const FireProductionResidentTransportComparatorRequest& request,
			MetalResidentTransportParameters& parameters,
			std::array<std::size_t,3>& faceOffset,std::size_t& allFaces,
			std::vector<unsigned char>& packedInflow,
			std::string* error )
		{
			const FireProductionProjectionShape& shape=request.shape;
			if(shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
				shape.nz<4u||shape.nz>1024u||!std::isfinite(shape.cellWidthM)||
				!(shape.cellWidthM>0.0f)||!ValidResidentTransportStage(request.stage)||
				request.attemptIdentity==0u||request.parentCandidateIdentity==0u||
				request.projectionIdentity==0u){
				if(error)*error="production resident transport protocol metadata is invalid";
				return false;
			}
			const std::size_t cells=shape.CellCount();
			if(cells==0u||cells>std::numeric_limits<std::uint32_t>::max()||
				request.conservativeValues.size()!=9u*cells||
				request.temperatureK.size()!=cells){
				if(error)*error="production resident transport cell tuple is invalid";
				return false;
			}
			allFaces=0u;
			for(unsigned int axis=0u;axis<3u;++axis){
				faceOffset[axis]=allFaces;
				const std::size_t faces=FireProductionProjectionFaceCount(shape,axis);
				if(request.projectedVelocityMPerS[axis].size()!=faces||
					allFaces>std::numeric_limits<std::size_t>::max()-faces){
					if(error)*error="production resident transport velocity tuple is invalid";
					return false;
				}
				allFaces+=faces;
			}
			for(unsigned int axis=0u;axis<3u;++axis){
				const FireProductionProjectionBoundary lower=request.boundary[2u*axis];
				const FireProductionProjectionBoundary upper=request.boundary[2u*axis+1u];
				if(lower<FireProductionProjectionPeriodic||lower>FireProductionProjectionWall||
					upper<FireProductionProjectionPeriodic||upper>FireProductionProjectionWall||
					((lower==FireProductionProjectionPeriodic)!=(upper==
						FireProductionProjectionPeriodic))){
					if(error)*error="production resident transport boundary tuple is invalid";
					return false;
				}
				if(lower==FireProductionProjectionPeriodic){
					const std::size_t extent=axis==0u?shape.nx:(axis==1u?shape.ny:shape.nz);
					const std::size_t firstEnd=axis==0u?shape.ny:shape.nx;
					const std::size_t secondEnd=axis==2u?shape.ny:shape.nz;
					auto faceIndex=[&](const std::size_t x,const std::size_t y,
						const std::size_t z){return axis==0u?(z*shape.ny+y)*(shape.nx+1u)+x:
						(axis==1u?(z*(shape.ny+1u)+y)*shape.nx+x:
							(z*shape.ny+y)*shape.nx+x);};
					for(std::size_t second=0u;second<secondEnd;++second)
						for(std::size_t first=0u;first<firstEnd;++first){
							std::size_t lx=0u,ly=0u,lz=0u,hx=0u,hy=0u,hz=0u;
							if(axis==0u){ly=hy=first;lz=hz=second;hx=extent;}
							if(axis==1u){lx=hx=first;lz=hz=second;hy=extent;}
							if(axis==2u){lx=hx=first;ly=hy=second;hz=extent;}
							const float lowValue=request.projectedVelocityMPerS[axis][
								faceIndex(lx,ly,lz)],highValue=request.projectedVelocityMPerS[axis][
								faceIndex(hx,hy,hz)];
							std::uint32_t lowBits=0u,highBits=0u;
							std::memcpy(&lowBits,&lowValue,sizeof(lowBits));
							std::memcpy(&highBits,&highValue,sizeof(highBits));
							if(lowBits!=highBits){
								if(error)*error=
									"production resident transport periodic velocity seam is invalid";
								return false;
							}
						}
				}
			}
			if(!AllFinite(request.conservativeValues)||!AllFinite(request.temperatureK)){
				if(error)*error="production resident transport cell input is nonfinite";
				return false;
			}
			for(const std::vector<float>& velocity:request.projectedVelocityMPerS)
				if(!AllFinite(velocity)){
					if(error)*error="production resident transport velocity is nonfinite";
					return false;
				}
			parameters={};parameters.nx=static_cast<std::uint32_t>(shape.nx);
			parameters.ny=static_cast<std::uint32_t>(shape.ny);
			parameters.nz=static_cast<std::uint32_t>(shape.nz);
			parameters.cells=static_cast<std::uint32_t>(cells);
			packedInflow.clear();std::size_t sideOffset=0u;
			for(unsigned int side=0u;side<6u;++side){
				const std::size_t expected=side<2u?shape.ny*shape.nz:
					(side<4u?shape.nx*shape.nz:shape.nx*shape.ny);
				if(request.fuelInletBoundaryFace[side].size()!=expected){if(error)*error=
					"production resident transport fuel-inlet classification shape is invalid";
					return false;}
				for(const unsigned char value:request.fuelInletBoundaryFace[side])if(value>1u||
					(value!=0u&&(side!=4u||request.boundary[side]!=
						FireProductionProjectionWall))){
					if(error)*error=
						"production resident transport fuel-inlet classification value is invalid";
					return false;}
				parameters.boundary[side]=static_cast<std::uint32_t>(request.boundary[side]);
				parameters.sideOffset[side]=static_cast<std::uint32_t>(sideOffset);
				packedInflow.insert(packedInflow.end(),request.fuelInletBoundaryFace[side].begin(),
					request.fuelInletBoundaryFace[side].end());sideOffset+=expected;
			}
			for(unsigned int axis=0u;axis<3u;++axis)parameters.faceOffset[axis]=
				static_cast<std::uint32_t>(faceOffset[axis]);
			parameters.stage=static_cast<std::uint32_t>(request.stage);
			parameters.cellWidthM=shape.cellWidthM;
			parameters.attemptIdentity=request.attemptIdentity;
			parameters.parentCandidateIdentity=request.parentCandidateIdentity;
			parameters.projectionIdentity=request.projectionIdentity;
			return true;
		}

		bool PrepareResidentEOSCandidateRequest(
			const FireProductionResidentEOSCandidateComparatorRequest&,
			MetalResidentTransportParameters&,MetalResidentPhysicalFluxParameters&,
			MetalResidentEOSParameters&,std::array<std::size_t,3>&,std::size_t&,
			std::vector<unsigned char>&,std::vector<unsigned char>&,
			std::vector<float>&,std::string*);

		// R201_RESIDENT_OWNER_TRANSFER_SURFACE_BEGIN
		class ResidentProjectedHeunMetalOwner
		{
			struct Stage
			{
				FireProductionMetalProjectionResidentState projection;
				FireProductionProjectionResult projectionDiagnostics;
				std::unique_ptr<ResidentTransportMetalAuthority> transport;
				std::unique_ptr<ResidentEndpointClassProducerAuthority> endpointClassProducer,
					sealedEndpointClassProducer;
				std::unique_ptr<ResidentEndpointClassMetalAuthority> endpointClass;
				std::unique_ptr<ResidentPhysicalFluxMetalAuthority> physical;
				std::unique_ptr<ResidentPhysicalFluxMetalAuthority> averagedPhysical;
				std::unique_ptr<ResidentEOSCandidateMetalAuthority> candidate;
				std::unique_ptr<ResidentEOSMetalAuthority> eos;
				std::unique_ptr<ResidentFrozenSourceMetalAuthority> source;
				std::unique_ptr<ResidentProjectionTargetMetalAuthority> target;
				FireProductionMetalNonpressureMomentumRHSResidentResult nonpressure;
				id<MTLBuffer> packedVelocity,packedDensity,packedMomentum,gasDensity,
					gasSource,advectionRate,heunR0AdvectionRate,nextMomentum,nextOpenClass,
					projectionTargetAssembled,
					lineageSeal,issuedLineageSeal;
				std::shared_ptr<unsigned char> projectionTargetCapability;
				double deviceMS;
				std::uint64_t allocationBytes;
				std::uint32_t acceptedIterationCount;
				bool terminalReprojectionVerified;
				std::uint32_t activeSetCycleLength,activeSetCanonicalProjectionCount,
					projectionTargetCorrectionIteration;
				bool activeSetDiscontinuousClass,limiterDiscontinuousClass;
				std::vector<float> picardResidualPerS;
				Stage() : packedVelocity(nil),packedDensity(nil),packedMomentum(nil),
					gasDensity(nil),gasSource(nil),advectionRate(nil),heunR0AdvectionRate(nil),nextMomentum(nil),
					nextOpenClass(nil),projectionTargetAssembled(nil),lineageSeal(nil),issuedLineageSeal(nil),
					deviceMS(0.0),allocationBytes(0u),acceptedIterationCount(0u),
					terminalReprojectionVerified(false),activeSetCycleLength(0u),
					activeSetCanonicalProjectionCount(0u),projectionTargetCorrectionIteration(0u),
					activeSetDiscontinuousClass(false),
					limiterDiscontinuousClass(false) {}
			};

			const FireProductionProjectedHeunMetalOwnerRequest& request_;
			ResidentTransportMetalContext& context_;
			id<MTLBuffer> inputPayloadDigest_=nil;
			SingleStageFCTMetalContext& fct_;
			FireProductionProjectionShape shape_;
			std::size_t cells_,allFaces_,boundaryFaces_;
			std::array<std::size_t,3> faceOffset_;
			MetalResidentTransportParameters transportMetadata_;
			MetalResidentPhysicalFluxParameters physicalMetadata_;
			MetalResidentEOSParameters eosMetadata_;
			MetalSingleStageFCTParameters fctMetadata_;
			MetalResidentTargetParameters targetMetadata_;
			MetalResidentProjectionConsumerParameters projectionMetadata_;
			MetalProjectedHeunOwnerParameters ownerMetadata_;
			id<MTLBuffer> q0_,t0_,m0_,sourceDelta_,frozenSource_,fuel_,initialInflow_,thermo_,eosThermo_,
				transportData_,ambient_,physicalBasis_,advectiveBasis_,projector_,enthalpy_,
				affine_,fctParameters_[3],transportParameters_[3],physicalParameters_,
				eosParameters_[3],targetParameters_[3],projectionParameters_,ownerParameters_[3],
				failure_,transportObligations_,physicalObligations_,eosObligations_,
				targetObligations_,zeroTarget_,zeroTargetIdentity_,zeroTargetConsumerIdentity_,rootCandidateIdentity_,
				integratedOpenHead_;
			std::unique_ptr<ResidentProjectionTargetMetalAuthority> bootstrapTarget_;
			std::vector<std::shared_ptr<unsigned char> > issuedTargetCapabilities_;
			std::array<std::vector<FireProductionProjectedHeunIterationTrace>,3>
				qualificationTrace_;
			std::uint32_t commits_,projectionInvocations_,interstageFullGridTransfers_;
			std::uint32_t qualificationTraceStagingCount_;
			bool qualificationInterstageUploadInjected_,qualificationInterstageSourceUploadInjected_;
			enum class OwnerTransferPhase { Setup,Interstage,Publication } transferPhase_;
			std::uint64_t actualBytes_,deviceAllocationBaseline_,deviceAllocationPeak_;
			double deviceMS_,projectionDeviceMS_;
			// Observer-only phase accounting. Inclusive rows form a tree; exclusive
			// rows subtract immediate children. Device time is the existing completed
			// command sum, including the projection adapter. wall-device is elapsed
			// residual, not a claim that all of it is CPU computation. Owner commits
			// exclude the adapter's internal commits, counted separately as invocations.
			class ProfileScope;
			bool profileEnabled_;
			ProfileScope* profileParent_;
			std::uint64_t profileSequence_;
			std::uint32_t profileStage_,profileIteration_;
			class ProfileScope
			{
				ResidentProjectedHeunMetalOwner& owner_;
				ProfileScope* parent_;
				const char* phase_;
				std::uint64_t sequence_;
				std::uint32_t stage_,iteration_,commits_,projections_;
				double device_,childWall_,childDevice_,childObserverWall_;
				std::chrono::steady_clock::time_point start_;
			public:
				ProfileScope(ResidentProjectedHeunMetalOwner& owner,const char* phase)
					: owner_(owner),parent_(nullptr),phase_(phase),sequence_(0u),stage_(0u),
					iteration_(0u),commits_(0u),projections_(0u),device_(0.0),
					childWall_(0.0),childDevice_(0.0),childObserverWall_(0.0)
				{
					if(!owner_.profileEnabled_)return;
					parent_=owner_.profileParent_;sequence_=++owner_.profileSequence_;
					stage_=owner_.profileStage_;iteration_=owner_.profileIteration_;
					commits_=owner_.commits_;projections_=owner_.projectionInvocations_;
					device_=owner_.deviceMS_;start_=std::chrono::steady_clock::now();
					owner_.profileParent_=this;
				}
				~ProfileScope()
				{
					if(!owner_.profileEnabled_)return;
					const double wall=std::chrono::duration<double,std::milli>(
						std::chrono::steady_clock::now()-start_).count();
					const double device=owner_.deviceMS_-device_;
					owner_.profileParent_=parent_;
					const char* kind=stage_==UINT32_MAX?"owner":
						(iteration_==UINT32_MAX?"bootstrap":
						((iteration_&UINT32_C(0x80000000))?"terminal":"picard"));
					std::fprintf(stderr,"RISE_FIRE_OWNER_PROFILE_V1 {\"scope\":%llu,"
						"\"parent\":%llu,\"phase\":\"%s\",\"stage\":%u,"
						"\"raw_iteration\":%u,\"iteration_kind\":\"%s\","
						"\"wall_ms\":%.9f,\"device_sum_ms\":%.9f,"
						"\"wall_minus_device_ms\":%.9f,\"exclusive_wall_ms\":%.9f,"
						"\"exclusive_device_sum_ms\":%.9f,\"child_observer_wall_ms\":%.9f,"
						"\"count_scope\":\"inclusive\",\"owner_commits\":%u,"
						"\"projection_invocations\":%u}\n",
						static_cast<unsigned long long>(sequence_),
						static_cast<unsigned long long>(parent_?parent_->sequence_:0u),
						phase_,stage_,iteration_,kind,wall,device,wall-device,
						wall-childWall_-childObserverWall_,device-childDevice_,childObserverWall_,
						owner_.commits_-commits_,
						owner_.projectionInvocations_-projections_);
					// Attribute the observer's own output cost to the parent's children,
					// so it cannot be misreported as exclusive work in the parent phase.
					if(parent_){parent_->childWall_+=wall;
						parent_->childObserverWall_+=std::chrono::duration<double,std::milli>(
							std::chrono::steady_clock::now()-start_).count()-wall;
						parent_->childDevice_+=device;}
				}
				ProfileScope(const ProfileScope&)=delete;
				ProfileScope& operator=(const ProfileScope&)=delete;
			};

			void ObserveDeviceAllocation()
			{
				if(!context_.device)return;
				const std::uint64_t current=static_cast<std::uint64_t>(
					[context_.device currentAllocatedSize]);
				if(current>=deviceAllocationBaseline_)deviceAllocationPeak_=std::max(
					deviceAllocationPeak_,current-deviceAllocationBaseline_);
			}

			id<MTLBuffer> Private(const std::size_t bytes)
			{
				id<MTLBuffer> value=[context_.device newBufferWithLength:bytes
					options:MTLResourceStorageModePrivate];
				if(value){actualBytes_+=[value allocatedSize];ObserveDeviceAllocation();}
				return value;
			}
			bool HashPayload(id<MTLCommandBuffer> command,id<MTLBuffer> payload,
				std::size_t bytes,id<MTLBuffer> root,std::size_t rootOffset,std::string* error)
			{
				static PayloadMerkleContext merkle;std::string failure;
				if(!merkle.error.empty()||[merkle.device registryID]!=[context_.device registryID]){
					if(error)*error="owner payload seal device/kernel mismatch: "+merkle.error;return false;}
				if(!EncodePayloadMerkle(merkle,command,payload,bytes,256u,root,failure,0u,rootOffset)){
					if(error)*error=failure;return false;}
				ObserveDeviceAllocation();return true;
			}
			id<MTLBuffer> Upload(const void* bytes,const std::size_t length)
			{
				id<MTLBuffer> value=[context_.device newBufferWithBytes:bytes length:length
					options:MTLResourceStorageModeShared];
				if(value){
					actualBytes_+=[value allocatedSize];ObserveDeviceAllocation();
					const std::size_t fullFieldBytes=cells_<=
						std::numeric_limits<std::size_t>::max()/sizeof(float)?
						cells_*sizeof(float):std::numeric_limits<std::size_t>::max();
					// newBufferWithBytes is itself a host-to-Metal transfer; it has no
					// blit for Copy() to observe.  A stage-sized upload is therefore
					// publication-blocking under the same ledger.
					if(transferPhase_==OwnerTransferPhase::Interstage&&cells_!=0u&&
						length>=fullFieldBytes)++interstageFullGridTransfers_;
				}
				return value;
			}
			enum class TransferKind { Upload,Internal,Control,QualificationTrace,Terminal };
			void Copy(id<MTLBlitCommandEncoder> encoder,id<MTLBuffer> source,
				const std::size_t sourceOffset,id<MTLBuffer> destination,
				const std::size_t destinationOffset,const std::size_t bytes,
				const TransferKind kind)
			{
				[encoder copyFromBuffer:source sourceOffset:sourceOffset toBuffer:destination
					destinationOffset:destinationOffset size:bytes];
				const bool sourcePrivate=source&&[source storageMode]==MTLStorageModePrivate;
				const bool destinationPrivate=destination&&
					[destination storageMode]==MTLStorageModePrivate;
				const bool crossesHostBoundary=source&&destination&&
					sourcePrivate!=destinationPrivate;
				const std::size_t fullFieldBytes=cells_<=
					std::numeric_limits<std::size_t>::max()/sizeof(float)?
					cells_*sizeof(float):std::numeric_limits<std::size_t>::max();
				// The requested qualification trace has its own explicit staging ledger;
				// it is not a production-stage authority transfer.  Every other full
				// field crossing in either direction during R0/R1/R2 is publication-
				// blocking, including a host-authored substitution into Private memory.
				if(transferPhase_==OwnerTransferPhase::Interstage&&crossesHostBoundary&&
					bytes>=fullFieldBytes&&kind!=TransferKind::QualificationTrace)
					++interstageFullGridTransfers_;
			}
			const void* Read(id<MTLBuffer> buffer,const TransferKind kind)
			{
				(void)kind;
				return ReadTrackedMetalBuffer(buffer);
			}
			bool ReadOpenClass(id<MTLBuffer> source,std::vector<unsigned char>& bytes,
				std::string* error)
			{
				ProfileScope profile(*this,"ReadOpenClass");
				id<MTLBuffer> staging=[context_.device newBufferWithLength:boundaryFaces_
					options:MTLResourceStorageModeShared];
				id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context_.queue);
				id<MTLBlitCommandEncoder> encoder=command?[command blitCommandEncoder]:nil;
				if(!staging||!encoder)return false;
				Copy(encoder,source,0u,staging,0u,boundaryFaces_,TransferKind::Control);
				[encoder endEncoding];if(!Commit(command,error))return false;
				const unsigned char* value=static_cast<const unsigned char*>(
					Read(staging,TransferKind::Control));if(!value)return false;
				bytes.assign(value,value+boundaryFaces_);return true;
			}
			bool Encode(id<MTLCommandBuffer> command,id<MTLComputePipelineState> pipeline,
				const std::initializer_list<id<MTLBuffer> >& buffers,const std::size_t threads)
			{
				id<MTLComputeCommandEncoder> encoder=command?ProducerProfileEncoder(command):nil;
				if(!encoder)return false;[encoder setComputePipelineState:pipeline];
				std::size_t index=0u;for(id<MTLBuffer> buffer:buffers)
					[encoder setBuffer:buffer offset:0 atIndex:index++];
				Dispatch(encoder,pipeline,threads);[encoder endEncoding];return true;
			}
			bool ResetControls(id<MTLCommandBuffer> command)
			{
				id<MTLBlitCommandEncoder> blit=command?[command blitCommandEncoder]:nil;
				if(!blit)return false;const id<MTLBuffer> controls[]={failure_,transportObligations_,
					physicalObligations_,eosObligations_,targetObligations_};
				for(id<MTLBuffer> value:controls){
					[blit fillBuffer:value range:NSMakeRange(0,[value length]) value:0u];
				}
				[blit endEncoding];return true;
			}
			void ReportEOSRefusalInputs(const Stage& stageResult,id<MTLBuffer> parentState,
				unsigned int stage,std::uint32_t cell)
			{
				const char* enabled=std::getenv("RISE_FIRE_EOS_REFUSAL_INPUTS");
				if(!enabled||std::strcmp(enabled,"1")!=0)return;
				if(cell>=cells_||!stageResult.candidate)return;
				id<MTLBuffer> staging=[context_.device newBufferWithLength:36u*sizeof(float)
					options:MTLResourceStorageModeShared];
				id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context_.queue);
				id<MTLBlitCommandEncoder> blit=command?[command blitCommandEncoder]:nil;
				if(!staging||!blit)return;
				const id<MTLBuffer> fields[]={stageResult.candidate->conservative,parentState,q0_,sourceDelta_};
				for(std::size_t field=0u;field<4u;++field)for(std::size_t component=0u;component<9u;++component)
					Copy(blit,fields[field],(component*cells_+cell)*sizeof(float),staging,
						(field*9u+component)*sizeof(float),sizeof(float),TransferKind::Control);
				[blit endEncoding];if(!Commit(command,nullptr))return;
				const auto* values=static_cast<const float*>(Read(staging,TransferKind::Control));
				if(!values)return;
				std::fprintf(stderr,"EOS_REFUSAL_INPUT_V1 stage=%u raw_iteration=%u cell=%u "
					"nx=%zu ny=%zu nz=%zu Tmin=%.9g Tmax=%.9g pressure=%.9g dynamics_bound=%.9g "
					"scope=failed_candidate_only publication=false\n",stage,profileIteration_,cell,
					shape_.nx,shape_.ny,shape_.nz,eosMetadata_.temperatureMinK,
					eosMetadata_.temperatureMaxK,eosMetadata_.pressurePa,eosMetadata_.dynamicsValidityBound);
				for(std::size_t field=0u;field<4u;++field)for(std::size_t component=0u;component<9u;++component){
					std::uint32_t bits=0u;std::memcpy(&bits,&values[field*9u+component],sizeof(bits));
					std::fprintf(stderr,"EOS_REFUSAL_COMPONENT field=%zu component=%zu bits=%u value=%.17g\n",
						field,component,bits,static_cast<double>(values[field*9u+component]));
				}
				std::array<double,9> candidate;
				for(std::size_t component=0u;component<candidate.size();++component)
					candidate[component]=values[component];
				const FireSimulationMethaneRecord& record=FireSimulationMethaneRecord::PhysicalV1();
				double temperature=0.0,ratio=0.0;std::string mirrorError;
				bool mirrorAccepted=record.InvertAcceptedConservativeStateByComponentOrder(
					candidate.data(),candidate.size(),eosMetadata_.temperatureMinK,
					eosMetadata_.temperatureMaxK,FireStateProducerPrecision::Binary32,
					temperature,ratio,&mirrorError);
				const float publishedTemperature=static_cast<float>(temperature);
				if(mirrorAccepted)mirrorAccepted=
					record.AcceptedConservativePressureRatioAtTemperatureByComponentOrder(
						candidate.data(),candidate.size(),publishedTemperature,
						FireStateProducerPrecision::Binary32,ratio,&mirrorError);
				const float projectedRatio=static_cast<float>(ratio);
				const double lowerMidpoint=(static_cast<double>(std::nextafter(projectedRatio,
					-std::numeric_limits<float>::infinity()))+projectedRatio)*0.5;
				const double upperMidpoint=(static_cast<double>(std::nextafter(projectedRatio,
					std::numeric_limits<float>::infinity()))+projectedRatio)*0.5;
				std::fprintf(stderr,"EOS_REFUSAL_FP64 accepted=%d temperature_K=%.17g "
					"pressure_ratio=%.17g projected_ratio=%.17g lower_midpoint=%.17g "
					"upper_midpoint=%.17g lower_distance_ratio=%.17g upper_distance_ratio=%.17g "
					"error=%s\n",mirrorAccepted?1:0,temperature,ratio,
					static_cast<double>(projectedRatio),lowerMidpoint,upperMidpoint,
					ratio-lowerMidpoint,upperMidpoint-ratio,mirrorError.c_str());
				// Refusal-only witness: consume the original private candidate/table,
				// never a CPU reconstruction, and issue no authority or publication.
				const std::string witnessSource=std::string(ResidentTransportMetalContext::Source())+R"METAL(
kernel void refusal_ratio_witness(device const float* candidate [[buffer(0)]],
 device const float* thermo [[buffer(1)]],constant EOSParams& p [[buffer(2)]],
 constant uint& cell [[buffer(3)]],device float* output [[buffer(4)]],
 device atomic_uint* obligations [[buffer(5)]],uint gid [[thread_position_in_grid]]){
 if(gid!=0u)return;float T=0.0f;bool ok=eos_temperature(candidate,thermo,p,cell,obligations,T);
 EOSDD gas=eos_dd(0.0f),molar=eos_dd(0.0f);
 for(uint species=0u;species<6u;++species){float density=max(0.0f,candidate[(species+1u)*p.cells+cell]);
 gas=eos_add(gas,eos_dd(density));molar=eos_add(molar,eos_div(eos_dd(density),eos_load_dd(thermo,96u*species)));}
 EOSDD meanWeight=eos_div(gas,molar),represented=eos_div(eos_mul(eos_mul(gas,
 eos_load_dd(thermo,7u*96u)),eos_dd(T)),meanWeight);
 EOSDD ratio=eos_div(represented,eos_load_dd(thermo,7u*96u+3u));float rounded=0.0f;
 bool unique=eos_unique_binary32_round(ratio,rounded);
 output[0]=ok?T:NAN;output[1]=ratio.hi;output[2]=ratio.lo;output[3]=ratio.tail;
 output[4]=ratio.bound;output[5]=rounded;output[6]=unique?1.0f:0.0f;
 float below=nextafter(ratio.hi,-INFINITY),above=nextafter(ratio.hi,INFINITY);
 EOSDD lower=eos_add(eos_dd(ratio.hi),eos_dd((below-ratio.hi)*0.5f));
 EOSDD upper=eos_add(eos_dd(ratio.hi),eos_dd((above-ratio.hi)*0.5f));
 output[7]=float(eos_order(lower,ratio));output[8]=float(eos_order(ratio,upper));
}
)METAL";
				MTLCompileOptions* options=[MTLCompileOptions new];
				if(@available(macOS 15.0,*)){
					options.mathMode=MTLMathModeSafe;options.languageVersion=MTLLanguageVersion3_2;
				}else return;
				NSError* witnessError=nil;
				id<MTLLibrary> library=[context_.device newLibraryWithSource:
					[NSString stringWithUTF8String:witnessSource.c_str()] options:options error:&witnessError];
				id<MTLFunction> function=[library newFunctionWithName:@"refusal_ratio_witness"];
				id<MTLComputePipelineState> pipeline=function?[context_.device
					newComputePipelineStateWithFunction:function error:&witnessError]:nil;
				id<MTLBuffer> witness=[context_.device newBufferWithLength:9u*sizeof(float)
					options:MTLResourceStorageModeShared];
				id<MTLCommandBuffer> witnessCommand=TrackedMetalCommandBuffer(context_.queue);
				id<MTLComputeCommandEncoder> encoder=[witnessCommand computeCommandEncoder];
				if(!pipeline||!witness||!encoder)return;
				[encoder setComputePipelineState:pipeline];
				[encoder setBuffer:stageResult.candidate->conservative offset:0 atIndex:0];
				[encoder setBuffer:eosThermo_ offset:0 atIndex:1];
				[encoder setBuffer:eosParameters_[stage] offset:0 atIndex:2];
				[encoder setBytes:&cell length:sizeof(cell) atIndex:3];
				[encoder setBuffer:witness offset:0 atIndex:4];
				[encoder setBuffer:eosObligations_ offset:0 atIndex:5];
				[encoder dispatchThreads:MTLSizeMake(1u,1u,1u)
					threadsPerThreadgroup:MTLSizeMake(1u,1u,1u)];[encoder endEncoding];
				if(!Commit(witnessCommand,nullptr))return;
				const auto* expansion=static_cast<const float*>(Read(witness,TransferKind::Control));
				if(!expansion)return;
				std::fprintf(stderr,"EOS_REFUSAL_EXPANSION temperature=%.17g hi=%.17g lo=%.17g "
					"tail=%.17g bound=%.17g rounded=%.17g unique=%g lower_order=%g upper_order=%g\n",
					double(expansion[0]),double(expansion[1]),double(expansion[2]),double(expansion[3]),
					double(expansion[4]),double(expansion[5]),double(expansion[6]),
					double(expansion[7]),double(expansion[8]));
			}
			bool Commit(id<MTLCommandBuffer> command,std::string* error)
			{
				CommitTrackedMetalCommand(command);[command waitUntilCompleted];++commits_;
				if([command status]!=MTLCommandBufferStatusCompleted){
					if(error)*error=MetalError("projected-Heun resident owner command failed",
						[command error]);
					return false;
				}
				deviceMS_+=([command GPUEndTime]-[command GPUStartTime])*1000.0;
				ObserveDeviceAllocation();return true;
			}
			bool Project(const unsigned int stage,id<MTLBuffer> state,id<MTLBuffer> momentum,
				const ResidentProjectionTargetMetalAuthority& targetAuthority,
				const std::shared_ptr<unsigned char>& expectedParentCapability,
				id<MTLBuffer> sealedInflow,id<MTLBuffer> sealedHead,
				Stage& output,std::string* error)
			{
				ProfileScope profile(*this,"Project");
				const bool authenticatedBootstrap=targetAuthority.bootstrapAuthority&&
					bootstrapTarget_.get()==&targetAuthority&&targetAuthority.assembled==zeroTarget_&&
					targetAuthority.publicationIdentity==zeroTargetIdentity_&&
					targetAuthority.consumerIdentity==zeroTargetConsumerIdentity_&&
					targetAuthority.capability;
				const bool issuedCapability=std::find(issuedTargetCapabilities_.begin(),
					issuedTargetCapabilities_.end(),targetAuthority.capability)!=
					issuedTargetCapabilities_.end();
				const bool authenticatedDerived=!targetAuthority.bootstrapAuthority&&
					targetAuthority.parentCommand&&targetAuthority.assembled&&
					targetAuthority.publicationIdentity&&targetAuthority.consumerIdentity&&
					targetAuthority.projectionConsumerIdentity&&targetAuthority.capability&&
					issuedCapability&&targetAuthority.parentTargetCapability&&expectedParentCapability&&
					targetAuthority.parentTargetCapability==expectedParentCapability;
				if(!authenticatedBootstrap&&!authenticatedDerived){
					if(error)*error="projected-Heun projection refuses unsealed target capability";
					return false;
				}
				FireProductionProjectionRequest projection;
				projection.shape=shape_;projection.timeStepS=ownerMetadata_.timeStepS;
				projection.ambientDensityKGPerM3=request_.ambientDensityKGPerM3;
				projection.boundary=request_.lineage.eos.physicalFlux.transport.boundary;
				projection.gasDensityKGPerM3.assign(cells_,request_.ambientDensityKGPerM3);
				projection.divergenceTargetPerS.assign(cells_,0.0f);
				projection.openClassificationMode=sealedInflow?
					FireProductionProjectionUseSealedOpenClassification:
					FireProductionProjectionDeriveOpenClassification;
				projection.openHeadMode=sealedHead?FireProductionProjectionUseSealedOpenHead:
					FireProductionProjectionDeriveCurrentOpenHead;
				projection.outputClassificationMode=FireProductionProjectionPreserveOpenClassification;
				projection.endpointVelocityToleranceMPerS=request_.endpointVelocityToleranceMPerS;
				if(sealedInflow||sealedHead)for(unsigned int side=0u;side<6u;++side){
					const std::size_t count=side<2u?shape_.ny*shape_.nz:
						(side<4u?shape_.nx*shape_.nz:shape_.nx*shape_.ny);
					if(sealedInflow)projection.sealedPressureOpenInflow[side].assign(count,0u);
					if(sealedHead)projection.sealedPressureOpenDynamicPressurePa[side].assign(count,0.0f);}
				for(unsigned int axis=0u;axis<3u;++axis)
					projection.provisionalMomentumKGPerM2S[axis].assign(
						FireProductionProjectionFaceCount(shape_,axis),0.0f);
				id<MTLBuffer> gasDensity=Private(cells_*sizeof(float));
				id<MTLCommandBuffer> densityCommand=TrackedMetalCommandBuffer(context_.queue);
				if(!densityCommand||!Encode(densityCommand,fct_.extractGasDensity,
					{state,gasDensity,fctParameters_[0]},cells_)){
					if(error)*error="projected-Heun resident density encoder failed";return false;}
				if(!Commit(densityCommand,error))return false;
				FireProductionMetalProjectionResidentInput input;input.gasDensityKGPerM3=gasDensity;
				input.provisionalMomentumKGPerM2S.fill(momentum);
				for(unsigned int axis=0u;axis<3u;++axis)
					input.provisionalMomentumByteOffset[axis]=faceOffset_[axis]*sizeof(float);
				input.divergenceTargetPerS=targetAuthority.assembled;
				input.targetPublicationIdentity=targetAuthority.publicationIdentity;
				input.targetConsumerIdentity=targetAuthority.consumerIdentity;
				input.qualifiedOwnerStageTokens=context_.productionStageTokens;
				input.sealedPressureOpenInflow=sealedInflow;
				input.sealedPressureOpenDynamicPressurePa=sealedHead;
				const std::uint64_t allocationBeforeProjection=static_cast<std::uint64_t>(
					[context_.device currentAllocatedSize]);
				const std::uint64_t outerLiveBeforeProjection=
					allocationBeforeProjection>=deviceAllocationBaseline_?
					allocationBeforeProjection-deviceAllocationBaseline_:0u;
				{
					ProfileScope adapterProfile(*this,"ProjectionAdapter");
					if(!ProjectFireProductionMetalResidentState(projection,input,output.projection,
						output.projectionDiagnostics,error))return false;
					++projectionInvocations_;deviceMS_+=output.projectionDiagnostics.deviceElapsedMS;
					projectionDeviceMS_+=output.projectionDiagnostics.deviceElapsedMS;
				}
				output.projectionTargetCapability=targetAuthority.capability;
				interstageFullGridTransfers_+=
					output.projectionDiagnostics.residentInterstageDeviceToHostTransferCount;
				if(outerLiveBeforeProjection<=std::numeric_limits<std::uint64_t>::max()-
					output.projectionDiagnostics.residentActualMetalAllocationBytes)
					deviceAllocationPeak_=std::max(deviceAllocationPeak_,outerLiveBeforeProjection+
						output.projectionDiagnostics.residentActualMetalAllocationBytes);
				else deviceAllocationPeak_=std::numeric_limits<std::uint64_t>::max();
				output.nextOpenClass=Private(boundaryFaces_);
				id<MTLCommandBuffer> classCommand=TrackedMetalCommandBuffer(context_.queue);
				if(!output.nextOpenClass||!classCommand||!Encode(classCommand,context_.ownerNextOpenClass,
					{output.projection.pressureOpenInflow,output.projection.velocityMPerS[0],
					 output.projection.velocityMPerS[1],output.projection.velocityMPerS[2],
					 output.nextOpenClass,transportParameters_[stage],ownerParameters_[stage]},
					 boundaryFaces_)||!Commit(classCommand,error))return false;
				output.endpointClassProducer.reset(new ResidentEndpointClassProducerAuthority);
				output.endpointClassProducer->producingCommand=classCommand;
				output.endpointClassProducer->classes=output.nextOpenClass;
				output.endpointClassProducer->projectionPublicationIdentity=
					output.projection.publicationIdentity;
				output.endpointClassProducer->transportParameters=transportParameters_[stage];
				output.endpointClassProducer->ownerParameters=ownerParameters_[stage];
				output.endpointClassProducer->capability=std::make_shared<unsigned char>(0u);
				output.sealedEndpointClassProducer.reset(new ResidentEndpointClassProducerAuthority);
				output.sealedEndpointClassProducer->producingCommand=classCommand;
				output.sealedEndpointClassProducer->classes=output.projection.pressureOpenInflow;
				output.sealedEndpointClassProducer->projectionPublicationIdentity=
					output.projection.publicationIdentity;
				output.sealedEndpointClassProducer->transportParameters=transportParameters_[stage];
				output.sealedEndpointClassProducer->ownerParameters=ownerParameters_[stage];
				output.sealedEndpointClassProducer->capability=std::make_shared<unsigned char>(0u);
				return output.projection.publicationIdentity!=nil&&
					[output.projection.pressureOpenInflow storageMode]==MTLStorageModePrivate&&
					[output.nextOpenClass storageMode]==MTLStorageModePrivate;
			}
			bool CustomCandidate(id<MTLCommandBuffer> command,const unsigned int stage,
				const ResidentTransportMetalAuthority& transport,
				ResidentPhysicalFluxMetalAuthority& physical,id<MTLBuffer> parentIdentity,
				const ResidentEOSCandidateMetalAuthority* parentCandidateAuthority,
				id<MTLBuffer> selectedAlpha,
				ResidentEOSCandidateMetalAuthority& candidate,std::string* error)
			{
				const std::size_t stateBytes=9u*cells_*sizeof(float);
				candidate.conservative=Private(stateBytes);candidate.sourceDelta=sourceDelta_;
				candidate.faceAlpha=Private(allFaces_*sizeof(float));
				candidate.producerIdentity=Private(sizeof(std::uint64_t));
				candidate.publicationIdentity=Private(sizeof(std::uint64_t));
				id<MTLBuffer> lowState=Private(stateBytes),ratio=Private(11u*cells_*sizeof(float));
				if(!candidate.conservative||!candidate.faceAlpha||!candidate.producerIdentity||
					!candidate.publicationIdentity||!lowState||!ratio)return false;
				candidate.parentCommand=command;candidate.parentPhysicalPublicationIdentity=
					physical.publicationIdentity;candidate.parentTransportPublicationIdentity=
					transport.publicationIdentity;candidate.parentEOSThermochemistry=eosThermo_;
				candidate.parentEOSParameters=eosParameters_[stage];
				candidate.parentFCTParameters=fctParameters_[stage];candidate.cells=cells_;
				candidate.capability=std::make_shared<unsigned char>(0u);
				candidate.parentCandidateCapability=parentCandidateAuthority?
					parentCandidateAuthority->capability:std::shared_ptr<unsigned char>();
				candidate.rootParent=stage==0u;
				candidate.allocationBytes=[candidate.conservative allocatedSize]+
					[candidate.faceAlpha allocatedSize]+[candidate.producerIdentity allocatedSize]+
					[candidate.publicationIdentity allocatedSize]+[lowState allocatedSize]+[ratio allocatedSize];
				if(stage==2u){
					id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];if(!blit)return false;
					Copy(blit,transport.parentState,0u,candidate.conservative,0u,stateBytes,
						TransferKind::Internal);
					[blit fillBuffer:candidate.faceAlpha range:NSMakeRange(0,
						allFaces_*sizeof(float)) value:0u];[blit endEncoding];
				}else if(!Encode(command,fct_.buildRatios,{stage==1u?q0_:transport.parentState,sourceDelta_,
					physical.lowComposite,physical.advectiveDelta,enthalpy_,lowState,ratio,failure_,
					fctParameters_[stage]},11u*cells_)||(!selectedAlpha&&
					!Encode(command,fct_.buildFaceAlpha,{physical.advectiveDelta,ratio,enthalpy_,
						candidate.faceAlpha,failure_,fctParameters_[stage]},allFaces_))||
					(!selectedAlpha&&!Encode(command,fct_.commitScalar,{lowState,physical.advectiveDelta,
						candidate.faceAlpha,enthalpy_,affine_,candidate.conservative,failure_,
						fctParameters_[stage]},cells_))){
					if(error)*error="projected-Heun resident candidate FCT encoder failed";
					return false;
				}
				if(stage!=2u&&selectedAlpha){id<MTLBlitCommandEncoder> selected=[command blitCommandEncoder];
					if(!selected)return false;Copy(selected,selectedAlpha,0u,candidate.faceAlpha,0u,
						allFaces_*sizeof(float),TransferKind::Internal);
					[selected endEncoding];
					// The selected alpha must be installed before the scalar commit.
					if(!Encode(command,fct_.commitScalar,{lowState,physical.advectiveDelta,
						candidate.faceAlpha,enthalpy_,affine_,candidate.conservative,failure_,
						fctParameters_[stage]},cells_))return false;}
				if(
					!Encode(command,context_.ownerBindCandidate,{candidate.conservative,
						candidate.faceAlpha,transport.publicationIdentity,physical.publicationIdentity,
						parentIdentity,candidate.producerIdentity,failure_,ownerParameters_[stage]},1u)||
					!Encode(command,context_.finalizeEOSCandidate,{candidate.producerIdentity,
						candidate.publicationIdentity,failure_},1u)){
					if(error)*error="projected-Heun resident candidate encoder failed";return false;}
				return true;
			}
			bool CorrectTargetAuthority(id<MTLCommandBuffer> command,const unsigned int stage,
				const ResidentProjectionTargetMetalAuthority& parent,const bool correction,
				const ResidentTransportMetalAuthority& transport,
				const ResidentPhysicalFluxMetalAuthority& physical,
				const ResidentEOSCandidateMetalAuthority& candidate,
				const ResidentEOSMetalAuthority& eos,const ResidentFrozenSourceMetalAuthority& source,
				ResidentProjectionTargetMetalAuthority& target,std::string* error)
			{
				const std::uint32_t correctionValue=correction?parent.correctionIteration+1u:0u;
				id<MTLBuffer> correctionBuffer=Upload(&correctionValue,sizeof(correctionValue));
				if(!correctionBuffer||!Encode(command,context_.ownerComposeTarget,
					{target.baseAssembled,target.monitoredAbsolute,parent.assembled,target.assembled,
					 ownerParameters_[stage],correctionBuffer},cells_)||
					!Encode(command,context_.finalizeTarget,{target.assembled,failure_,targetObligations_,
					 targetParameters_[stage]},1u)||
					!Encode(command,context_.ownerIdentifyTarget,{target.tangent,target.frozenSource,
					 target.absoluteDiagnostic,target.monitoredAbsolute,target.assembled,
					 transport.publicationIdentity,physical.publicationIdentity,candidate.publicationIdentity,
					 eos.publicationIdentity,source.publicationIdentity,parent.publicationIdentity,
					 target.publicationIdentity,ownerParameters_[stage],correctionBuffer},1u)||
					!Encode(command,context_.identifyProjectionConsumer,{projectionParameters_,
					 target.publicationIdentity,target.projectionMetadata,
					 target.projectionConsumerIdentity,failure_},1u)||
					!Encode(command,context_.consumeTarget,{target.assembled,target.publicationIdentity,
					 transport.publicationIdentity,physical.publicationIdentity,candidate.publicationIdentity,
					 eos.publicationIdentity,target.consumerIdentity,target.projectionConsumerIdentity,
					 failure_,targetParameters_[stage],target.projectionMetadata},1u)){
					if(error)*error="projected-Heun owner target correction encoder failed";return false;}
				target.capability=std::make_shared<unsigned char>(0u);
				issuedTargetCapabilities_.push_back(target.capability);
				target.parentTargetCapability=parent.capability;
				target.correctionIteration=correctionValue;
				return true;
			}

			bool BuildStage(const unsigned int stage,id<MTLBuffer> state,id<MTLBuffer> temperature,
				id<MTLBuffer> parentIdentity,const ResidentEOSCandidateMetalAuthority* parentCandidate,
				const ResidentProjectionTargetMetalAuthority& projectionTarget,
				const bool applyTailCorrection,id<MTLBuffer> selectedAlpha,
				Stage& output,const Stage* r0,std::string* error)
			{
				ProfileScope profile(*this,"BuildStageProducerGroup");
				id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context_.queue);
				if(!command||!ResetControls(command))return false;
				ProducerKernelProfile kernelProfile(command,stage,profileIteration_);
				if(!kernelProfile.Valid()){if(error)*error="producer kernel counters unavailable";return false;}
				output.packedVelocity=Private(allFaces_*sizeof(float));
				output.packedDensity=Private(allFaces_*sizeof(float));
				output.packedMomentum=Private(allFaces_*sizeof(float));
				output.gasDensity=Private(cells_*sizeof(float));
				output.gasSource=Private(cells_*sizeof(float));
				output.advectionRate=Private(allFaces_*sizeof(float));
				output.heunR0AdvectionRate=stage==1u&&r0?Private(allFaces_*sizeof(float)):nil;
				output.nextMomentum=Private(allFaces_*sizeof(float));
				if(!output.packedVelocity||!output.packedDensity||!output.packedMomentum||
					!output.gasDensity||!output.gasSource||!output.advectionRate||!output.nextMomentum)
					return false;
				if(!Encode(command,context_.ownerPackFaces,{output.projection.velocityMPerS[0],
					output.projection.velocityMPerS[1],output.projection.velocityMPerS[2],
					output.packedVelocity,ownerParameters_[stage]},allFaces_)||
					!Encode(command,context_.ownerPackFaces,{output.projection.faceDensityKGPerM3[0],
						output.projection.faceDensityKGPerM3[1],output.projection.faceDensityKGPerM3[2],
						output.packedDensity,ownerParameters_[stage]},allFaces_)||
					!Encode(command,context_.ownerPackFaces,{output.projection.momentumKGPerM2S[0],
						output.projection.momentumKGPerM2S[1],output.projection.momentumKGPerM2S[2],
						output.packedMomentum,ownerParameters_[stage]},allFaces_)||
					!Encode(command,fct_.extractGasDensity,{state,output.gasDensity,
						fctParameters_[stage]},cells_)||
					!Encode(command,context_.ownerGasSource,{sourceDelta_,output.gasSource,
						ownerParameters_[stage]},cells_))return false;
				output.transport.reset(new ResidentTransportMetalAuthority);
				if(!EncodeResidentTransportAuthority(context_,command,state,temperature,
					output.packedVelocity,thermo_,transportData_,fuel_,transportParameters_[stage],
					failure_,transportObligations_,cells_,allFaces_,boundaryFaces_,
					*output.transport,error)||!Encode(command,context_.ownerBindTransport,
					{parentIdentity,output.projection.publicationIdentity,
					output.transport->publicationIdentity,failure_,ownerParameters_[stage]},1u))return false;
				output.physical.reset(new ResidentPhysicalFluxMetalAuthority);
				// R2 derives the endpoint class from the force-inclusive projected
				// velocity.  Its physical flux must consume that immediate class, as
				// the reviewed fp64 owner does; the sealed Picard class is only the
				// projection solve's input.  R0/R1 retain their sealed-stage class.
				id<MTLBuffer> physicalInflow=stage==2u&&
					!request_.qualificationR2SealedClassPhysicalFlux?output.nextOpenClass:
					output.projection.pressureOpenInflow;
				output.endpointClass.reset(new ResidentEndpointClassMetalAuthority);
				if(!EncodeResidentEndpointClassAuthority(context_,command,physicalInflow,
					output.projection.publicationIdentity,transportParameters_[stage],failure_,
					*output.transport,cells_,allFaces_,boundaryFaces_,true,
					stage==2u?output.endpointClassProducer.get():
						output.sealedEndpointClassProducer.get(),*output.endpointClass,error))
					return false;
				if(request_.qualificationUnverifiedEndpointClassBuffer&&stage==2u)
					output.endpointClass->classes=Private(boundaryFaces_);
				if(!EncodeResidentPhysicalFluxAuthority(context_,command,state,temperature,
					output.packedVelocity,thermo_,ambient_,
					physicalBasis_,advectiveBasis_,projector_,transportParameters_[stage],
					physicalParameters_,failure_,physicalObligations_,*output.transport,
					*output.endpointClass,cells_,allFaces_,
					physicalMetadata_,*output.physical,error))return false;
				if(stage==1u&&r0){
					id<MTLBuffer> low=Private(9u*allFaces_*sizeof(float));
					id<MTLBuffer> delta=Private(9u*allFaces_*sizeof(float));
					id<MTLBuffer> high=Private(9u*allFaces_*sizeof(float));
					id<MTLBuffer> identity=Private(sizeof(std::uint64_t));
					id<MTLBuffer> firstLow=request_.qualificationWrongAveragedFluxParent?
						output.physical->highComposite:r0->physical->lowComposite;
					id<MTLBuffer> firstDelta=request_.qualificationWrongAveragedFluxParent?
						output.physical->advectiveDelta:r0->physical->advectiveDelta;
					if(!low||!delta||!high||!identity||!Encode(command,context_.ownerAverageFlux,
						{firstLow,firstDelta,
						 output.physical->lowComposite,output.physical->advectiveDelta,low,delta,high,
						 ownerParameters_[stage]},9u*allFaces_)||
						!Encode(command,context_.ownerBindAveragedFlux,{low,delta,
						 r0->physical->publicationIdentity,output.physical->publicationIdentity,
						 output.transport->publicationIdentity,identity,failure_,ownerParameters_[stage]},1u))
						return false;
					output.averagedPhysical.reset(new ResidentPhysicalFluxMetalAuthority);
					output.averagedPhysical->lowComposite=low;
					output.averagedPhysical->advectiveDelta=delta;
					output.averagedPhysical->highComposite=high;
					output.averagedPhysical->publicationIdentity=identity;
					output.averagedPhysical->parentCommand=command;
					output.averagedPhysical->parentState=state;
					output.averagedPhysical->parentThermochemistry=thermo_;
					output.averagedPhysical->parentTransportPublicationIdentity=
						output.transport->publicationIdentity;
					output.averagedPhysical->parentCells=cells_;
					output.averagedPhysical->parentAllFaces=allFaces_;
					output.averagedPhysical->allocationBytes=[low allocatedSize]+[delta allocatedSize]+
						[high allocatedSize]+[identity allocatedSize];
				}
				ResidentPhysicalFluxMetalAuthority& candidatePhysical=output.averagedPhysical?
					*output.averagedPhysical:*output.physical;
				std::vector<float> uploadedAlpha;
				if(request_.qualificationInjectInterstageTransfer&&stage==1u&&
					!qualificationInterstageUploadInjected_){
					// RED-only direct CPU upload: it is consumed as the selected limiter
					// field while all transport/physical parent seals remain valid-looking.
					// Upload() and its installation Copy() must both reach the common
					// residency ledger; neither is allowed to publish.
					uploadedAlpha.assign(allFaces_,0.0f);
					selectedAlpha=Upload(uploadedAlpha.data(),uploadedAlpha.size()*sizeof(float));
					if(!selectedAlpha)return false;
					qualificationInterstageUploadInjected_=true;
				}
				output.candidate.reset(new ResidentEOSCandidateMetalAuthority);
				if(!CustomCandidate(command,stage,*output.transport,candidatePhysical,parentIdentity,
					parentCandidate,selectedAlpha,
					*output.candidate,error))return false;
				output.eos.reset(new ResidentEOSMetalAuthority);
				if(!EncodeResidentEOSAuthority(context_,command,thermo_,eosThermo_,eosParameters_[stage],
					failure_,eosObligations_,*output.transport,candidatePhysical,*output.candidate,
					eosMetadata_,*output.eos,error))return false;
				output.source.reset(new ResidentFrozenSourceMetalAuthority);
				const bool injectDirectSourceUpload=request_.qualificationInjectInterstageTransfer&&
					stage==1u&&!qualificationInterstageSourceUploadInjected_;
				if(!EncodeResidentFrozenSourceAuthority(context_,command,failure_,targetObligations_,
					request_.lineage.frozenSource,*output.candidate,targetMetadata_,
					injectDirectSourceUpload?nil:frozenSource_,
					injectDirectSourceUpload?nil:targetParameters_[stage],*output.source,error))
					return false;
				if(injectDirectSourceUpload){
					qualificationInterstageSourceUploadInjected_=true;
					// The fallback comparator path has just consumed one CPU-authored
					// Shared source field.  It is legal only as this RED and must arrive
					// at the owner's common atomic publication gate as a counted transfer.
					if([output.source->inputUpload storageMode]!=MTLStorageModePrivate&&
						[output.source->inputUpload length]>=cells_*sizeof(float))
						++interstageFullGridTransfers_;
				}
				output.target.reset(new ResidentProjectionTargetMetalAuthority);
				if(!EncodeResidentProjectionTargetAuthority(context_,command,eosThermo_,
					targetParameters_[stage],projectionParameters_,nil,nil,failure_,targetObligations_,
					*output.transport,*output.physical,candidatePhysical,
					*output.candidate,*output.eos,*output.source,
					targetMetadata_,*output.target,error))return false;
				if(!CorrectTargetAuthority(command,stage,projectionTarget,applyTailCorrection,
					*output.transport,candidatePhysical,*output.candidate,*output.eos,*output.source,
					*output.target,error))return false;
				output.lineageSeal=Private(sizeof(std::uint64_t));
				output.issuedLineageSeal=output.lineageSeal;
				if(!output.lineageSeal||!Encode(command,context_.ownerIssueStageSeal,
					{output.projection.publicationIdentity,output.transport->publicationIdentity,
					 output.physical->publicationIdentity,candidatePhysical.publicationIdentity,
					 output.candidate->publicationIdentity,output.eos->publicationIdentity,
					 output.source->publicationIdentity,output.target->publicationIdentity,
					 output.lineageSeal,failure_,ownerParameters_[stage]},1u))return false;
				FireProductionMetalNonpressureMomentumRHSResidentInput rhs;
				rhs.shape=shape_;rhs.ambientDensityKGPerM3=request_.ambientDensityKGPerM3;
				rhs.vremanCoefficient=request_.vremanCoefficient;rhs.gravityMPerS2=request_.gravityMPerS2;
				rhs.boundary=request_.lineage.eos.physicalFlux.transport.boundary;
				rhs.commandBuffer=command;rhs.cellGasDensityKGPerM3=output.gasDensity;
				rhs.molecularKinematicViscosityM2PerS=output.transport->coefficients;
				rhs.cellGasPhaseSourceRateKGPerM3S=output.gasSource;
				rhs.packedFaceDensityKGPerM3=output.packedDensity;
				rhs.packedMomentumKGPerM2S=output.packedMomentum;
				FireProductionNonpressureMomentumRHSMetalDiagnostics rhsDiagnostics;
				// The resident RHS API currently has no byte offset, so copy the third
				// coefficient plane into a private view without exposing it to the host.
				id<MTLBuffer> molecular=Private(cells_*sizeof(float));
				id<MTLBlitCommandEncoder> coefficientBlit=[command blitCommandEncoder];
				if(!molecular||!coefficientBlit)return false;
				Copy(coefficientBlit,output.transport->coefficients,2u*cells_*sizeof(float),
					molecular,0u,cells_*sizeof(float),TransferKind::Internal);
				[coefficientBlit endEncoding];
				rhs.molecularKinematicViscosityM2PerS=molecular;
				if(!EvaluateFireProductionNonpressureMomentumRHSMetalResident(rhs,output.nonpressure,
					rhsDiagnostics,error)||!Encode(command,fct_.compatibleStageRate,
					{output.physical->lowComposite,output.physical->advectiveDelta,
					 output.candidate->faceAlpha,output.projection.velocityMPerS[0],
					 output.projection.velocityMPerS[1],output.projection.velocityMPerS[2],
					 output.advectionRate,failure_,fctParameters_[stage]},allFaces_)||
					(stage==1u&&r0&&!Encode(command,fct_.compatibleStageRate,
					 {r0->physical->lowComposite,r0->physical->advectiveDelta,
					  output.candidate->faceAlpha,r0->projection.velocityMPerS[0],
					  r0->projection.velocityMPerS[1],r0->projection.velocityMPerS[2],
					  output.heunR0AdvectionRate,failure_,fctParameters_[stage]},allFaces_)))return false;
				if(stage==0u){if(!Encode(command,context_.ownerPredictMomentum,{m0_,
					output.nonpressure.combinedMomentumRateKGPerM2S2,output.advectionRate,
					output.nextMomentum,failure_,ownerParameters_[stage]},allFaces_))return false;}
				else if(r0){id<MTLBuffer> r0HeunAdvection=
					request_.qualificationReuseR0LimiterAlpha?r0->advectionRate:output.heunR0AdvectionRate;
					if(!Encode(command,context_.ownerHeunMomentum,{m0_,
					r0->nonpressure.combinedMomentumRateKGPerM2S2,
					output.nonpressure.combinedMomentumRateKGPerM2S2,r0HeunAdvection,
					output.advectionRate,output.nextMomentum,failure_,ownerParameters_[stage]},allFaces_))
					return false;}
				id<MTLBuffer> stageFailure=[context_.device newBufferWithLength:sizeof(std::uint32_t)
					options:MTLResourceStorageModeShared];
				id<MTLBlitCommandEncoder> stageBlit=command?[command blitCommandEncoder]:nil;
				if(!stageFailure||!stageBlit)return false;
				Copy(stageBlit,failure_,0u,stageFailure,0u,sizeof(std::uint32_t),TransferKind::Control);
				[stageBlit endEncoding];
				if(!Commit(command,error))return false;
				if(!kernelProfile.Report()){if(error)*error="producer kernel counter qualification failed";return false;}
				const std::uint32_t stageFailureBitmap=
					*static_cast<const std::uint32_t*>(Read(stageFailure,TransferKind::Control));
				if(stageFailureBitmap!=0u){
					if(error)*error="projected-Heun resident stage device validation failed: stage="+
						std::to_string(stage)+" bitmap="+std::to_string(stageFailureBitmap);
					if((stageFailureBitmap&512u)!=0u&&output.eos&&output.eos->firstFailureCell&&
						output.eos->failureTerm){
						id<MTLBuffer> witness=[context_.device newBufferWithLength:
							2u*sizeof(std::uint32_t) options:MTLResourceStorageModeShared];
						id<MTLCommandBuffer> witnessCommand=TrackedMetalCommandBuffer(context_.queue);
						id<MTLBlitCommandEncoder> witnessBlit=witnessCommand?
							[witnessCommand blitCommandEncoder]:nil;
						if(witness&&witnessBlit){Copy(witnessBlit,output.eos->firstFailureCell,0u,
							witness,0u,sizeof(std::uint32_t),TransferKind::Control);[witnessBlit endEncoding];
							if(Commit(witnessCommand,0)){const std::uint32_t* values=
								static_cast<const std::uint32_t*>(Read(witness,TransferKind::Control));
								const std::uint32_t cell=values?values[0]:std::numeric_limits<std::uint32_t>::max();
								ReportEOSRefusalInputs(output,state,stage,cell);
								if(values&&cell<cells_){id<MTLCommandBuffer> termCommand=
									TrackedMetalCommandBuffer(context_.queue);id<MTLBlitCommandEncoder> termBlit=
									termCommand?[termCommand blitCommandEncoder]:nil;
									if(termBlit){Copy(termBlit,output.eos->failureTerm,
										cell*sizeof(std::uint32_t),witness,sizeof(std::uint32_t),
										sizeof(std::uint32_t),TransferKind::Control);[termBlit endEncoding];
									if(Commit(termCommand,0)){values=static_cast<const std::uint32_t*>(
											Read(witness,TransferKind::Control));if(values&&error)*error+=
											" eos_failure_cell="+std::to_string(values[0])+
											" eos_failure_term_bitmap="+std::to_string(values[1]);}}}}}
					}
					if((stageFailureBitmap&65536u)!=0u&&output.target){
						id<MTLBuffer> diagnostic=[context_.device newBufferWithLength:
							4u*cells_*sizeof(float) options:MTLResourceStorageModeShared];
						id<MTLCommandBuffer> diagnosticCommand=TrackedMetalCommandBuffer(context_.queue);
						id<MTLBlitCommandEncoder> diagnosticBlit=diagnosticCommand?
							[diagnosticCommand blitCommandEncoder]:nil;
						if(diagnostic&&diagnosticBlit){const id<MTLBuffer> fields[]={output.target->tangent,
							output.target->frozenSource,output.target->absoluteDiagnostic,
							output.target->monitoredAbsolute};
							for(unsigned int field=0u;field<4u;++field)Copy(diagnosticBlit,
								fields[field],0u,diagnostic,field*cells_*sizeof(float),
								cells_*sizeof(float),TransferKind::Terminal);
							[diagnosticBlit endEncoding];
							if(Commit(diagnosticCommand,0)){const float* values=
									static_cast<const float*>(Read(diagnostic,TransferKind::Terminal));
								for(std::size_t cell=0u;cell<cells_;++cell)if(std::isnan(values[cell])){
									std::uint32_t hiBits=0u,loBits=0u,boundBits=0u;
									std::memcpy(&hiBits,&values[cells_+cell],sizeof(hiBits));
									std::memcpy(&loBits,&values[2u*cells_+cell],sizeof(loBits));
									std::memcpy(&boundBits,&values[3u*cells_+cell],sizeof(boundBits));
									if(error)*error+=" tangent_cell="+std::to_string(cell)+
										" hi_bits="+std::to_string(hiBits)+
										" lo_bits="+std::to_string(loBits)+
										" bound_bits="+std::to_string(boundBits);
									id<MTLBuffer> terms=[context_.device newBufferWithLength:
										64u*sizeof(float) options:MTLResourceStorageModeShared];
									id<MTLCommandBuffer> termCommand=TrackedMetalCommandBuffer(context_.queue);
									id<MTLBlitCommandEncoder> termBlit=termCommand?
										[termCommand blitCommandEncoder]:nil;
									if(terms&&termBlit){std::size_t word=0u;
										for(std::size_t component=0u;component<9u;++component){Copy(termBlit,
											output.candidate->conservative,(component*cells_+cell)*sizeof(float),
											terms,word++*sizeof(float),sizeof(float),TransferKind::Control);}
										Copy(termBlit,output.eos->temperature,cell*sizeof(float),terms,
											word++*sizeof(float),sizeof(float),TransferKind::Control);
										const std::size_t x=cell%shape_.nx,y=(cell/shape_.nx)%shape_.ny,
											z=cell/(shape_.nx*shape_.ny);
										auto face=[&](const unsigned int axis,const std::size_t fx,
											const std::size_t fy,const std::size_t fz){return faceOffset_[axis]+(
											axis==0u?(fz*shape_.ny+fy)*(shape_.nx+1u)+fx:
											(axis==1u?(fz*(shape_.ny+1u)+fy)*shape_.nx+fx:
											(fz*shape_.ny+fy)*shape_.nx+fx));};
										for(std::size_t component=0u;component<9u;++component)
											for(unsigned int axis=0u;axis<3u;++axis){std::size_t rx=x,ry=y,rz=z;
												if(axis==0u)++rx;else if(axis==1u)++ry;else ++rz;
												const std::size_t endpoints[]={face(axis,x,y,z),face(axis,rx,ry,rz)};
												for(const std::size_t endpoint:endpoints){id<MTLBuffer> field=
													component<8u?output.physical->physicalMass:
													output.physical->physicalEnergy;
													const std::size_t sourceWord=component<8u?
														component*allFaces_+endpoint:endpoint;
													Copy(termBlit,field,sourceWord*sizeof(float),terms,
														word++*sizeof(float),sizeof(float),TransferKind::Control);}}
										[termBlit endEncoding];if(Commit(termCommand,0)){const float* termValues=
											static_cast<const float*>(Read(terms,TransferKind::Control));
											if(termValues&&error){std::ostringstream stream;stream<<std::setprecision(9)
												<<" tangent_state_and_face_terms=";for(std::size_t index=0u;
													index<64u;++index)stream<<(index?",":"")<<termValues[index];
												*error+=stream.str();}}}
									break;}}
						}
					}
					return false;
				}
				output.deviceMS=([command GPUEndTime]-[command GPUStartTime])*1000.0;return true;
			}
			bool CaptureIterationTrace(const unsigned int stage,const std::uint32_t iteration,
				id<MTLBuffer> state,id<MTLBuffer> temperature,
				id<MTLBuffer> sealedHead,
				const ResidentProjectionTargetMetalAuthority& projectionTarget,
				const Stage& value,std::string* error)
			{
				if(!request_.qualificationCaptureIterationTrace)return true;
				ProfileScope profile(*this,"QualificationTraceTransfers");
				const std::size_t floatCount=26u*cells_+36u*allFaces_+boundaryFaces_;
				const std::size_t byteCount=floatCount*sizeof(float)+2u*boundaryFaces_+
					sizeof(std::uint64_t);
				id<MTLBuffer> staging=[context_.device newBufferWithLength:byteCount
					options:MTLResourceStorageModeShared];
				id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context_.queue);
				id<MTLBlitCommandEncoder> blit=command?[command blitCommandEncoder]:nil;
				if(!staging||!blit)return false;
				std::size_t offset=0u;
				auto copy=[&](id<MTLBuffer> source,const std::size_t bytes){
					Copy(blit,source,0u,staging,offset,bytes,TransferKind::QualificationTrace);
					offset+=bytes;};
				auto copyRange=[&](id<MTLBuffer> source,const std::size_t sourceOffset,
					const std::size_t bytes){Copy(blit,source,sourceOffset,staging,offset,bytes,
						TransferKind::QualificationTrace);offset+=bytes;};
				copy(projectionTarget.assembled,cells_*sizeof(float));
				copy(value.target->assembled,cells_*sizeof(float));
				copy(value.projection.pressureOpenInflow,boundaryFaces_);
				copy(value.nextOpenClass,boundaryFaces_);
				copy(value.candidate->faceAlpha,allFaces_*sizeof(float));
				copy(value.packedVelocity,allFaces_*sizeof(float));
				for(unsigned int axis=0u;axis<3u;++axis)copyRange(
					value.projection.provisionalMomentumKGPerM2S[axis],
					value.projection.provisionalMomentumByteOffset[axis],
					FireProductionProjectionFaceCount(shape_,axis)*sizeof(float));
				if(sealedHead)copy(sealedHead,boundaryFaces_*sizeof(float));
				else{[blit fillBuffer:staging range:NSMakeRange(offset,
					boundaryFaces_*sizeof(float)) value:0u];offset+=boundaryFaces_*sizeof(float);}
				copy(state,9u*cells_*sizeof(float));copy(temperature,cells_*sizeof(float));
				copy(value.transport->coefficients,3u*cells_*sizeof(float));
				copy(value.gasDensity,cells_*sizeof(float));
				const ResidentPhysicalFluxMetalAuthority* candidatePhysical=
					value.averagedPhysical?value.averagedPhysical.get():value.physical.get();
				copy(candidatePhysical->lowComposite,9u*allFaces_*sizeof(float));
				copy(candidatePhysical->advectiveDelta,9u*allFaces_*sizeof(float));
				copy(value.physical->physicalMass,8u*allFaces_*sizeof(float));
				copy(value.physical->physicalEnergy,allFaces_*sizeof(float));
				copy(value.eos->representedPressureRatio,cells_*sizeof(float));
				copy(value.packedDensity,allFaces_*sizeof(float));
				copy(value.packedMomentum,allFaces_*sizeof(float));
				copy(value.advectionRate,allFaces_*sizeof(float));
				copy(value.nonpressure.buoyancyMomentumRateKGPerM2S2,allFaces_*sizeof(float));
				copy(value.nonpressure.stressMomentumRateKGPerM2S2,allFaces_*sizeof(float));
				copy(value.nonpressure.phaseSourceMomentumRateKGPerM2S2,allFaces_*sizeof(float));
				copy(value.candidate->conservative,9u*cells_*sizeof(float));
				copy(value.candidate->publicationIdentity,sizeof(std::uint64_t));
				[blit endEncoding];if(!Commit(command,error))return false;
				const unsigned char* bytes=static_cast<const unsigned char*>(
					Read(staging,TransferKind::Terminal));if(!bytes)return false;
				FireProductionProjectedHeunIterationTrace trace;trace.iteration=iteration;offset=0u;
				auto floats=[&](std::vector<float>& destination,const std::size_t count){
					const float* source=reinterpret_cast<const float*>(bytes+offset);
					destination.assign(source,source+count);offset+=count*sizeof(float);};
				auto classes=[&](std::array<std::vector<unsigned char>,6>& destination){
					for(unsigned int side=0u;side<6u;++side){const std::size_t count=side<2u?
						shape_.ny*shape_.nz:(side<4u?shape_.nx*shape_.nz:shape_.nx*shape_.ny);
						destination[side].assign(bytes+offset,bytes+offset+count);offset+=count;}};
				auto faces=[&](std::array<std::vector<float>,3>& destination){
					for(unsigned int axis=0u;axis<3u;++axis)
						floats(destination[axis],FireProductionProjectionFaceCount(shape_,axis));};
				floats(trace.projectionTargetPerS,cells_);floats(trace.producedTargetPerS,cells_);
				classes(trace.activeClass);classes(trace.nextActiveClass);
				faces(trace.sharedFaceAlpha);faces(trace.projectedVelocityMPerS);
				faces(trace.provisionalMomentumKGPerM2S);
				for(unsigned int side=0u;side<6u;++side){const std::size_t count=side<2u?
					shape_.ny*shape_.nz:(side<4u?shape_.nx*shape_.nz:shape_.nx*shape_.ny);
					floats(trace.sealedPressureOpenDynamicPressurePa[side],count);}
				if(stage<2u)for(auto& side:trace.sealedPressureOpenDynamicPressurePa)side.clear();
				floats(trace.transportConservativeValues,9u*cells_);
				floats(trace.transportTemperatureK,cells_);
				floats(trace.diffusivityM2PerS,cells_);floats(trace.conductivityWPerMK,cells_);
				floats(trace.molecularKinematicViscosityM2PerS,cells_);
				floats(trace.gasDensityKGPerM3,cells_);
				floats(trace.scalarLowFlux,9u*allFaces_);
				floats(trace.scalarFluxDelta,9u*allFaces_);
				floats(trace.physicalMassFluxKGPerM2S,8u*allFaces_);
				floats(trace.physicalEnergyFluxWPerM2,allFaces_);
				floats(trace.representedPressureRatio,cells_);faces(trace.faceDensityKGPerM3);
				faces(trace.projectedMomentumKGPerM2S);faces(trace.advectionMomentumRateKGPerM2S2);
				faces(trace.buoyancyMomentumRateKGPerM2S2);
				faces(trace.stressMomentumRateKGPerM2S2);
				faces(trace.phaseSourceMomentumRateKGPerM2S2);
				floats(trace.acceptedConservativeValues,9u*cells_);
				std::memcpy(&trace.acceptedCandidateIdentity,bytes+offset,sizeof(std::uint64_t));
				offset+=sizeof(std::uint64_t);
				// R0/R1 bootstrap has no accepted shared limiter yet; R2 projects a
				// frozen accepted state and owns neither a new alpha nor a force RHS.
				// Keep those non-applicable surfaces empty in both owners so trace
				// scope disagreement is a refusal rather than an infinity-shaped log.
				if(iteration==std::numeric_limits<std::uint32_t>::max()||stage==2u)
					for(auto& axis:trace.sharedFaceAlpha)axis.clear();
				// R0/R1 bootstrap precedes an accepted scalar candidate.  Its EOS
				// buffer is private scratch, not a represented-pressure authority;
				// exposing it would compare NaN scratch against the fp64 owner's
				// deliberately absent surface.
				if(iteration==std::numeric_limits<std::uint32_t>::max()&&stage<2u)
					trace.representedPressureRatio.clear();
				if(stage==2u){for(auto& axis:trace.advectionMomentumRateKGPerM2S2)axis.clear();
					for(auto& axis:trace.buoyancyMomentumRateKGPerM2S2)axis.clear();
					for(auto& axis:trace.stressMomentumRateKGPerM2S2)axis.clear();
					for(auto& axis:trace.phaseSourceMomentumRateKGPerM2S2)axis.clear();}
				trace.maximumPostProjectionResidualPerS=
					value.projectionDiagnostics.maximumPostProjectionResidualPerS;
				qualificationTrace_[stage].push_back(std::move(trace));
				++qualificationTraceStagingCount_;return true;
			}
			bool Prepare(std::string* error);
			bool SolveStage(unsigned int stage,id<MTLBuffer> state,id<MTLBuffer> temperature,
				id<MTLBuffer> momentum,id<MTLBuffer> parentIdentity,
				const ResidentEOSCandidateMetalAuthority* parentCandidate,const Stage* r0,
				id<MTLBuffer> initialSealedInflow,id<MTLBuffer> sealedHead,
				std::unique_ptr<Stage>& accepted,std::string* error);
		public:
			explicit ResidentProjectedHeunMetalOwner(
				const FireProductionProjectedHeunMetalOwnerRequest& request,bool productionStageTokens=false) : request_(request),
				context_(ResidentTransportContext(productionStageTokens)),fct_(SingleStageFCTContext()),cells_(0u),
				allFaces_(0u),boundaryFaces_(0u),q0_(nil),t0_(nil),m0_(nil),sourceDelta_(nil),
				frozenSource_(nil),
				fuel_(nil),initialInflow_(nil),thermo_(nil),eosThermo_(nil),transportData_(nil),
				ambient_(nil),physicalBasis_(nil),advectiveBasis_(nil),projector_(nil),
				enthalpy_(nil),affine_(nil),physicalParameters_(nil),projectionParameters_(nil),
				failure_(nil),transportObligations_(nil),physicalObligations_(nil),
				eosObligations_(nil),targetObligations_(nil),zeroTarget_(nil),
				zeroTargetIdentity_(nil),zeroTargetConsumerIdentity_(nil),rootCandidateIdentity_(nil),
				integratedOpenHead_(nil),commits_(0u),
				projectionInvocations_(0u),interstageFullGridTransfers_(0u),
				qualificationTraceStagingCount_(0u),qualificationInterstageUploadInjected_(false),
				qualificationInterstageSourceUploadInjected_(false),
				transferPhase_(OwnerTransferPhase::Setup),actualBytes_(0u),
				deviceAllocationBaseline_(context_.device?static_cast<std::uint64_t>(
					[context_.device currentAllocatedSize]):0u),deviceAllocationPeak_(0u),
				deviceMS_(0.0),projectionDeviceMS_(0.0),profileEnabled_(false),
				profileParent_(nullptr),profileSequence_(0u),profileStage_(UINT32_MAX),
				profileIteration_(UINT32_MAX)
			{fctParameters_[0]=fctParameters_[1]=fctParameters_[2]=nil;
			 const char* profileEnvironment=std::getenv("RISE_FIRE_OWNER_PROFILE");
			 profileEnabled_=profileEnvironment&&std::strcmp(profileEnvironment,"1")==0;
			 transportParameters_[0]=transportParameters_[1]=transportParameters_[2]=nil;
			 eosParameters_[0]=eosParameters_[1]=eosParameters_[2]=nil;
			 targetParameters_[0]=targetParameters_[1]=targetParameters_[2]=nil;
			 ownerParameters_[0]=ownerParameters_[1]=ownerParameters_[2]=nil;}
			bool Run(FireProductionProjectedHeunMetalOwnerResult& result,std::string* error);
		};

		bool ResidentProjectedHeunMetalOwner::Prepare(std::string* error)
		{
			ProfileScope profile(*this,"Prepare");
			if(!context_.Valid()||!fct_.Valid()||context_.device!=fct_.device){
				if(error)*error="projected-Heun resident owner Metal context is unavailable";
				return false;
			}
			std::vector<unsigned char> packedFuel,packedInflow;std::vector<float> physicalBasis;
			if(!PrepareResidentEOSCandidateRequest(request_.lineage.eos,transportMetadata_,
				physicalMetadata_,eosMetadata_,faceOffset_,allFaces_,packedFuel,packedInflow,
				physicalBasis,error))return false;
			shape_=request_.lineage.eos.physicalFlux.transport.shape;cells_=shape_.CellCount();
			boundaryFaces_=packedFuel.size();
			if(request_.lineage.frozenSource.Shape().nx!=shape_.nx||
				!FireProductionFrozenSourcePacketSealMatches(request_.lineage.frozenSource,error)||
				request_.beginningMomentumKGPerM2S[0].size()!=
					FireProductionProjectionFaceCount(shape_,0u)||
				request_.beginningMomentumKGPerM2S[1].size()!=
					FireProductionProjectionFaceCount(shape_,1u)||
				request_.beginningMomentumKGPerM2S[2].size()!=
					FireProductionProjectionFaceCount(shape_,2u)||
				request_.maximumPicardIterations<2u||request_.maximumPicardIterations>64u||
				request_.qualificationForcedActiveCycleStage>3u||
				!std::isfinite(request_.projectionTolerancePerS)||
				request_.projectionTolerancePerS<0.0f)return false;
			std::array<float,MetalResidentTransportSpeciesCount*
				MetalResidentTransportSpeciesStride> packedTransport;
			std::array<float,MetalManifoldCertificateValues> packedThermo;
			std::array<float,MetalEOSThermochemistryValues> packedEOSThermo;
			MetalManifoldParameters manifold={};std::array<double,7> lower,upper;
			std::vector<float> fctBasis,fctProjector,affine;
			std::array<float,14> enthalpy={{}};float feasibility=0.0f,reserve=0.0f;
			if(!PackMetalResidentTransport(packedTransport,transportMetadata_,error)||
				!PackMetalMethaneThermochemistry(packedThermo,manifold,lower,upper,error)||
				!PackMetalEOSDoubleDoubleThermochemistry(packedEOSThermo,error)||
				!PackMetalSingleStageFCTCertificate(fctBasis,fctProjector,enthalpy,affine,
					feasibility,reserve,error))return false;
			const auto& flux=request_.lineage.eos.physicalFlux;
			fctMetadata_={static_cast<std::uint32_t>(shape_.nx),static_cast<std::uint32_t>(shape_.ny),
				static_cast<std::uint32_t>(shape_.nz),static_cast<std::uint32_t>(cells_),9u,11u,
				static_cast<std::uint32_t>(flux.nullity),static_cast<std::uint32_t>(affine.size()/8u),
				{},{},shape_.cellWidthM,request_.lineage.eos.candidateTimeStepS,feasibility,reserve};
			std::size_t sideOffset=0u;for(unsigned int side=0u;side<6u;++side){
				fctMetadata_.boundary[side]=static_cast<std::uint32_t>(flux.transport.boundary[side]);
				fctMetadata_.sideOffset[side]=static_cast<std::uint32_t>(sideOffset);
				sideOffset+=flux.pressureOpenInflow[side].size();}
			targetMetadata_={static_cast<std::uint32_t>(shape_.nx),static_cast<std::uint32_t>(shape_.ny),
				static_cast<std::uint32_t>(shape_.nz),static_cast<std::uint32_t>(cells_),{},{},
				shape_.cellWidthM,request_.lineage.eos.candidateTimeStepS,
				static_cast<float>(FireProductionMonitoredManifoldPolicy::EngagementThreshold),0u,
				FireProductionMonitoredManifoldPolicy::IdentityVersion,{0u,0u},
				flux.transport.attemptIdentity,request_.lineage.frozenSource.PacketIdentity()};
			if(request_.qualificationDivergentManifoldPolicy)targetMetadata_.tailThreshold=
				std::nextafter(targetMetadata_.tailThreshold,std::numeric_limits<float>::infinity());
			const float qualifiedTailThreshold=static_cast<float>(
				FireProductionMonitoredManifoldPolicy::EngagementThreshold);
			if(targetMetadata_.tailThreshold!=qualifiedTailThreshold||
				targetMetadata_.policyVersion!=FireProductionMonitoredManifoldPolicy::IdentityVersion){
				if(error)*error="projected-Heun owner qualification refuses manifold-policy divergence";
				return false;
			}
			for(unsigned int axis=0u;axis<3u;++axis)targetMetadata_.faceOffset[axis]=
				static_cast<std::uint32_t>(faceOffset_[axis]);
			for(unsigned int side=0u;side<6u;++side)targetMetadata_.boundary[side]=
				static_cast<std::uint32_t>(flux.transport.boundary[side]);
			projectionMetadata_={static_cast<std::uint32_t>(shape_.nx),
				static_cast<std::uint32_t>(shape_.ny),static_cast<std::uint32_t>(shape_.nz),
				static_cast<std::uint32_t>(cells_),{},{0u,0u},shape_.cellWidthM,
				request_.lineage.eos.candidateTimeStepS,flux.transport.attemptIdentity};
			for(unsigned int side=0u;side<6u;++side)projectionMetadata_.boundary[side]=
				targetMetadata_.boundary[side];
			ownerMetadata_={static_cast<std::uint32_t>(cells_),static_cast<std::uint32_t>(allFaces_),
				{static_cast<std::uint32_t>(faceOffset_[0]),static_cast<std::uint32_t>(faceOffset_[1]),
				 static_cast<std::uint32_t>(faceOffset_[2])},0u,request_.lineage.eos.candidateTimeStepS,
				shape_.cellWidthM,request_.endpointVelocityToleranceMPerS,0u,
				request_.qualificationThreeQuarterHeunWeighting?1u:0u,
				0u,
				flux.transport.attemptIdentity};
			std::vector<float> packedMomentum;packedMomentum.reserve(allFaces_);
			for(const auto& axis:request_.beginningMomentumKGPerM2S)
				packedMomentum.insert(packedMomentum.end(),axis.begin(),axis.end());
			const std::size_t stateBytes=9u*cells_*sizeof(float),fieldBytes=cells_*sizeof(float);
			struct Pair{id<MTLBuffer> upload,device;std::size_t bytes;};std::vector<Pair> copies;
			auto copyIn=[&](const void* values,const std::size_t bytes){Pair pair={Upload(values,bytes),
				Private(bytes),bytes};copies.push_back(pair);return pair.device;};
			q0_=copyIn(flux.transport.conservativeValues.data(),stateBytes);
			t0_=copyIn(flux.transport.temperatureK.data(),fieldBytes);
			m0_=copyIn(packedMomentum.data(),allFaces_*sizeof(float));
			sourceDelta_=copyIn(request_.lineage.eos.sourceDelta.data(),stateBytes);
			frozenSource_=copyIn(request_.lineage.frozenSource.DivergenceTargetPerS().data(),
				fieldBytes);
			fuel_=copyIn(packedFuel.data(),packedFuel.size());
			initialInflow_=copyIn(packedInflow.data(),packedInflow.size());
			thermo_=copyIn(packedThermo.data(),packedThermo.size()*sizeof(float));
			eosThermo_=copyIn(packedEOSThermo.data(),packedEOSThermo.size()*sizeof(float));
			transportData_=copyIn(packedTransport.data(),packedTransport.size()*sizeof(float));
			ambient_=copyIn(flux.ambient.data(),9u*sizeof(float));
			physicalBasis_=copyIn(physicalBasis.data(),physicalBasis.size()*sizeof(float));
			advectiveBasis_=copyIn(flux.nullspaceBasis.data(),flux.nullspaceBasis.size()*sizeof(float));
			projector_=copyIn(flux.coordinateProjector.data(),flux.coordinateProjector.size()*sizeof(float));
			enthalpy_=copyIn(enthalpy.data(),enthalpy.size()*sizeof(float));
			affine_=copyIn(affine.data(),affine.size()*sizeof(float));
			physicalParameters_=copyIn(&physicalMetadata_,sizeof(physicalMetadata_));
			projectionParameters_=copyIn(&projectionMetadata_,sizeof(projectionMetadata_));
			for(unsigned int stage=0u;stage<3u;++stage){
				MetalResidentTransportParameters transport=transportMetadata_;transport.stage=stage;
				transport.parentCandidateIdentity=transport.projectionIdentity=flux.transport.attemptIdentity;
				transportParameters_[stage]=copyIn(&transport,sizeof(transport));
				fctParameters_[stage]=copyIn(&fctMetadata_,sizeof(fctMetadata_));
				MetalResidentEOSParameters eos=eosMetadata_;eos.stage=stage==0u?1u:2u;
				eosParameters_[stage]=copyIn(&eos,sizeof(eos));
				targetParameters_[stage]=copyIn(&targetMetadata_,sizeof(targetMetadata_));
				MetalProjectedHeunOwnerParameters owner=ownerMetadata_;owner.stage=stage;
				owner.forceActiveCycle=request_.qualificationForcedActiveCycleStage==stage+1u?1u:0u;
				ownerParameters_[stage]=copyIn(&owner,sizeof(owner));}
			failure_=Private(sizeof(std::uint32_t));transportObligations_=Private(sizeof(std::uint32_t));
			physicalObligations_=Private(sizeof(std::uint32_t));eosObligations_=Private(sizeof(std::uint32_t));
			targetObligations_=Private(sizeof(std::uint32_t));zeroTarget_=Private(fieldBytes);
			zeroTargetIdentity_=Private(sizeof(std::uint64_t));
			zeroTargetConsumerIdentity_=Private(sizeof(std::uint64_t));
			rootCandidateIdentity_=Private(sizeof(std::uint64_t));
			inputPayloadDigest_=Private(32u);
			if(!q0_||!t0_||!m0_||!sourceDelta_||!frozenSource_||!fuel_||!initialInflow_||!thermo_||
				!eosThermo_||!transportData_||!ambient_||!physicalBasis_||!advectiveBasis_||
				!projector_||!enthalpy_||!affine_||!failure_||!zeroTarget_||
				!zeroTargetIdentity_||!zeroTargetConsumerIdentity_||!rootCandidateIdentity_||!inputPayloadDigest_)return false;
			// Canonical descriptor fixes input segment order and lengths and binds
			// the actually compiled producer kernel set. Payload bytes are copied
			// from the private buffers consumed by the owner, not reserialized Q.
			std::string descriptor="rise.owner.device-inputs.v2\n"+
				context_.eosLogIdentity.librarySourceSHA256+"\n"+
				context_.eosLogIdentity.libraryFunctionSetSHA256+"\n";
			std::size_t inputBytes=0u;for(const Pair& pair:copies){
				if(pair.bytes>std::numeric_limits<std::size_t>::max()-inputBytes)return false;
				inputBytes+=pair.bytes;descriptor+=std::to_string(pair.bytes)+"\n";}
			descriptor+="end\n";
			if(descriptor.size()>std::numeric_limits<std::size_t>::max()-inputBytes)return false;
			inputBytes+=descriptor.size();
			const std::size_t inputBound=80u*cells_+4u*allFaces_+2u*boundaryFaces_+(std::size_t(1u)<<20u);
			if(inputBytes>inputBound||descriptor.size()>(std::size_t(1u)<<20u)){
				if(error)*error="owner input digest exceeds its working-set certificate";return false;}
			id<MTLBuffer> inputPayload=Private(inputBytes);
			id<MTLBuffer> inputDescriptor=Upload(descriptor.data(),descriptor.size());
			if(!inputPayload||!inputDescriptor)return false;
			id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context_.queue);
			id<MTLBlitCommandEncoder> blit=command?[command blitCommandEncoder]:nil;if(!blit)return false;
			for(const Pair& pair:copies)Copy(blit,pair.upload,0u,pair.device,0u,pair.bytes,
				TransferKind::Upload);
			Copy(blit,inputDescriptor,0u,inputPayload,0u,descriptor.size(),TransferKind::Upload);
			std::size_t sealOffset=descriptor.size();for(const Pair& pair:copies){
				Copy(blit,pair.device,0u,inputPayload,sealOffset,pair.bytes,TransferKind::Internal);sealOffset+=pair.bytes;}
			[blit fillBuffer:zeroTarget_ range:NSMakeRange(0,fieldBytes) value:0u];
			const id<MTLBuffer> controls[]={failure_,transportObligations_,physicalObligations_,
				eosObligations_,targetObligations_};
			for(id<MTLBuffer> value:controls){
				[blit fillBuffer:value range:NSMakeRange(0,[value length]) value:0u];
			}
			[blit endEncoding];
			if(!HashPayload(command,inputPayload,inputBytes,inputPayloadDigest_,0u,error))return false;
			if(!Encode(command,context_.ownerIssueBootstrap,{zeroTarget_,q0_,sourceDelta_,
				zeroTargetIdentity_,rootCandidateIdentity_,zeroTargetConsumerIdentity_,
				ownerParameters_[0],inputPayloadDigest_},1u)||
				!Commit(command,error))return false;
			bootstrapTarget_.reset(new ResidentProjectionTargetMetalAuthority);
			bootstrapTarget_->assembled=zeroTarget_;
			bootstrapTarget_->publicationIdentity=zeroTargetIdentity_;
			bootstrapTarget_->consumerIdentity=zeroTargetConsumerIdentity_;
			bootstrapTarget_->projectionConsumerIdentity=zeroTargetConsumerIdentity_;
			bootstrapTarget_->capability=std::make_shared<unsigned char>(0u);
			issuedTargetCapabilities_.push_back(bootstrapTarget_->capability);
			bootstrapTarget_->bootstrapAuthority=true;bootstrapTarget_->cells=cells_;
			return true;
		}

		bool ResidentProjectedHeunMetalOwner::SolveStage(const unsigned int stage,
			id<MTLBuffer> state,id<MTLBuffer> temperature,id<MTLBuffer> momentum,
			id<MTLBuffer> parentIdentity,
			const ResidentEOSCandidateMetalAuthority* parentCandidate,const Stage* r0,
			id<MTLBuffer> initialSealedInflow,
			id<MTLBuffer> sealedHead,std::unique_ptr<Stage>& accepted,
			std::string* error)
		{
			profileStage_=stage;profileIteration_=UINT32_MAX;
			ProfileScope stageProfile(*this,"SolveStage");
			using OpenClassBytes=std::vector<unsigned char>;
			auto classBytes=[&](id<MTLBuffer> value,OpenClassBytes& bytes){
				return ReadOpenClass(value,bytes,error);};
			auto residual=[&](const Stage& current,const Stage& prior,
				const bool includeIterationHistory,float& value,
				std::array<float,3>* components)->bool{
				ProfileScope profile(*this,"Residual");
				id<MTLBuffer> cellCount=Upload(&ownerMetadata_.cells,sizeof(std::uint32_t));
				id<MTLBuffer> faceCount=Upload(&ownerMetadata_.allFaces,sizeof(std::uint32_t));
				const std::uint32_t coefficientCount=3u*ownerMetadata_.cells;
				id<MTLBuffer> coefficientCountBuffer=Upload(&coefficientCount,sizeof(coefficientCount));
				const float one=1.0f,inverseDX=1.0f/shape_.cellWidthM;
				id<MTLBuffer> unit=Upload(&one,sizeof(one));
				id<MTLBuffer> momentumScale=Upload(&inverseDX,sizeof(inverseDX));
				std::array<id<MTLBuffer>,3> maximum={{Private(sizeof(std::uint32_t)),
					Private(sizeof(std::uint32_t)),Private(sizeof(std::uint32_t))}};
				id<MTLBuffer> terminal=[context_.device newBufferWithLength:3u*sizeof(std::uint32_t)
					options:MTLResourceStorageModeShared];
				id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context_.queue);
				id<MTLBlitCommandEncoder> clear=command?[command blitCommandEncoder]:nil;
				if(!cellCount||!faceCount||!coefficientCountBuffer||!unit||!momentumScale||
					!maximum[0]||!maximum[1]||!maximum[2]||!terminal||!command||!clear)return false;
				for(id<MTLBuffer> buffer:maximum)
					[clear fillBuffer:buffer range:NSMakeRange(0,sizeof(std::uint32_t)) value:0u];
				[clear endEncoding];
				if(!Encode(command,context_.ownerResidual,{current.target->assembled,
					prior.target->assembled,maximum[0],cellCount,unit},cells_))return false;
				if(includeIterationHistory&&(!Encode(command,context_.ownerResidual,
					{current.packedMomentum,prior.packedMomentum,maximum[1],faceCount,momentumScale},
					allFaces_)||!Encode(command,context_.ownerResidual,
					{current.transport->coefficients,prior.transport->coefficients,maximum[2],
					 coefficientCountBuffer,unit},3u*cells_)))return false;
				id<MTLBlitCommandEncoder> copy=[command blitCommandEncoder];if(!copy)return false;
				for(std::size_t index=0u;index<maximum.size();++index)Copy(copy,maximum[index],0u,
					terminal,index*sizeof(std::uint32_t),sizeof(std::uint32_t),TransferKind::Control);
				[copy endEncoding];if(!Commit(command,error))return false;
				const std::uint32_t* bits=static_cast<const std::uint32_t*>(
					Read(terminal,TransferKind::Control));std::array<float,3> local;
				for(std::size_t index=0u;index<local.size();++index)
					std::memcpy(&local[index],bits+index,sizeof(std::uint32_t));
				value=std::max({local[0],local[1],local[2]});if(components)*components=local;return true;
			};
			auto alphaResidual=[&](id<MTLBuffer> a,id<MTLBuffer> b,float& value)->bool{
				ProfileScope profile(*this,"AlphaResidual");
				id<MTLBuffer> maximum=Private(sizeof(std::uint32_t));
				id<MTLBuffer> terminal=[context_.device newBufferWithLength:sizeof(std::uint32_t)
					options:MTLResourceStorageModeShared];
				const std::uint32_t count=static_cast<std::uint32_t>(allFaces_);const float one=1.0f;
				id<MTLBuffer> countBuffer=Upload(&count,sizeof(count));id<MTLBuffer> unit=Upload(&one,sizeof(one));
				id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context_.queue);
				id<MTLBlitCommandEncoder> clear=command?[command blitCommandEncoder]:nil;
				if(!maximum||!terminal||!countBuffer||!unit||!clear)return false;
				[clear fillBuffer:maximum range:NSMakeRange(0,sizeof(std::uint32_t)) value:0u];[clear endEncoding];
				if(!Encode(command,context_.ownerResidual,{a,b,maximum,countBuffer,unit},allFaces_))return false;
				id<MTLBlitCommandEncoder> copy=[command blitCommandEncoder];if(!copy)return false;
				Copy(copy,maximum,0u,terminal,0u,sizeof(std::uint32_t),TransferKind::Control);[copy endEncoding];
				if(!Commit(command,error))return false;const std::uint32_t bits=
					*static_cast<const std::uint32_t*>(Read(terminal,TransferKind::Control));
				std::memcpy(&value,&bits,sizeof(bits));return true;
			};
			if(stage==0u&&request_.qualificationUnverifiedPrivateLineageBuffer){
				ResidentProjectionTargetMetalAuthority forged;forged.assembled=Private(cells_*sizeof(float));
				forged.publicationIdentity=Private(sizeof(std::uint64_t));
				forged.consumerIdentity=Private(sizeof(std::uint64_t));
				forged.projectionConsumerIdentity=Private(sizeof(std::uint64_t));
				id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context_.queue);
				id<MTLBlitCommandEncoder> copy=command?[command blitCommandEncoder]:nil;
				if(!forged.assembled||!forged.publicationIdentity||!forged.consumerIdentity||
					!forged.projectionConsumerIdentity||!copy)return false;
				Copy(copy,zeroTarget_,0u,forged.assembled,0u,cells_*sizeof(float),TransferKind::Internal);
				Copy(copy,zeroTargetIdentity_,0u,forged.publicationIdentity,0u,sizeof(std::uint64_t),
					TransferKind::Internal);Copy(copy,zeroTargetConsumerIdentity_,0u,forged.consumerIdentity,
					0u,sizeof(std::uint64_t),TransferKind::Internal);Copy(copy,
					zeroTargetConsumerIdentity_,0u,forged.projectionConsumerIdentity,0u,
					sizeof(std::uint64_t),TransferKind::Internal);[copy endEncoding];
				forged.parentCommand=command;forged.capability=std::make_shared<unsigned char>(0u);
				forged.parentTargetCapability=bootstrapTarget_->capability;
				if(!Commit(command,error))return false;Stage refused;
				return Project(stage,state,momentum,forged,bootstrapTarget_->capability,
					initialSealedInflow,sealedHead,refused,error);
			}
			std::unique_ptr<Stage> seed(new Stage);
			if(!Project(stage,state,momentum,*bootstrapTarget_,std::shared_ptr<unsigned char>(),
				initialSealedInflow,sealedHead,
				*seed,error)||!BuildStage(stage,state,temperature,parentIdentity,parentCandidate,
				*bootstrapTarget_,false,nil,*seed,r0,error))return false;
			if(!CaptureIterationTrace(stage,std::numeric_limits<std::uint32_t>::max(),state,temperature,
				sealedHead,*bootstrapTarget_,*seed,error))return false;
			id<MTLBuffer> activeClass=seed->projection.pressureOpenInflow;
			std::unique_ptr<Stage> prior=std::move(seed);bool havePrior=false,frozenCycle=false;
			float lastResidual=std::numeric_limits<float>::infinity();
			std::array<float,3> lastResidualComponents={{0.0f,0.0f,0.0f}};
			std::vector<float> residualHistory;
			bool lastClassStable=false;
			std::vector<id<MTLBuffer> > activeHistory,cycleBranches;
			std::vector<OpenClassBytes> activeHistoryBytes,cycleBranchBytes;
			std::uint32_t canonicalCount=0u;
			auto addHistory=[&](id<MTLBuffer> buffer,const OpenClassBytes& bytes){
				if(activeHistoryBytes.empty()||activeHistoryBytes.back()!=bytes){
					activeHistory.push_back(buffer);activeHistoryBytes.push_back(bytes);}};
			auto addBranch=[&](id<MTLBuffer> buffer,const OpenClassBytes& bytes){
				if(std::find(cycleBranchBytes.begin(),cycleBranchBytes.end(),bytes)==cycleBranchBytes.end()){
					cycleBranches.push_back(buffer);cycleBranchBytes.push_back(bytes);}};
			auto canonicalProjection=[&](const ResidentProjectionTargetMetalAuthority& target,
				const std::shared_ptr<unsigned char>& expectedTargetParent,
				std::unique_ptr<Stage>& selected)->bool{
				if(cycleBranches.empty())return false;
				if(request_.qualificationDisableCanonicalCycle){
					if(error)*error="projected-Heun two-class canonical selection mutant refused";
					return false;
				}
				std::size_t best=0u;float bestDiscrepancy=0.0f;bool haveBest=false;
				for(std::size_t index=0u;index<cycleBranches.size();++index){
					std::unique_ptr<Stage> branch(new Stage);
					if(!Project(stage,state,momentum,target,expectedTargetParent,
						cycleBranches[index],sealedHead,
						*branch,error))return false;
					++canonicalCount;
					const float discrepancy=branch->projectionDiagnostics.
						maximumOpenComplementarityDiscrepancyMPerS;
					if(!haveBest||discrepancy<bestDiscrepancy||(discrepancy==bestDiscrepancy&&
						cycleBranchBytes[index]<cycleBranchBytes[best])){
						best=index;bestDiscrepancy=discrepancy;selected=std::move(branch);haveBest=true;}}
				activeClass=cycleBranches[best];return haveBest;
			};
			for(std::uint32_t iteration=0u;iteration<request_.maximumPicardIterations;++iteration){
				profileIteration_=iteration;
				ProfileScope iterationProfile(*this,"PicardIteration");
				if(stage==2u&&havePrior&&request_.qualificationStaleTargetPublication)
					prior->target->parentTargetCapability=bootstrapTarget_->capability;
				std::unique_ptr<Stage> current(new Stage);
				if(!Project(stage,state,momentum,*prior->target,prior->projectionTargetCapability,
					activeClass,sealedHead,*current,error))return false;
				OpenClassBytes usedBytes,nextBytes;if(!classBytes(current->projection.pressureOpenInflow,
					usedBytes)||!classBytes(current->nextOpenClass,nextBytes))return false;
				bool classStable=frozenCycle||usedBytes==nextBytes;
				if(frozenCycle){addBranch(current->projection.pressureOpenInflow,usedBytes);
					addBranch(current->nextOpenClass,nextBytes);}
				if(!frozenCycle&&!classStable){addHistory(current->projection.pressureOpenInflow,usedBytes);
					auto repeated=std::find(activeHistoryBytes.begin(),activeHistoryBytes.end(),nextBytes);
					if(repeated!=activeHistoryBytes.end()){
						const std::size_t first=static_cast<std::size_t>(repeated-activeHistoryBytes.begin());
						for(std::size_t index=first;index<activeHistory.size();++index)
							addBranch(activeHistory[index],activeHistoryBytes[index]);
						if(!canonicalProjection(*prior->target,prior->projectionTargetCapability,
							current))return false;
						frozenCycle=true;classStable=true;
					}else activeClass=current->nextOpenClass;
				}else if(!frozenCycle)activeClass=current->nextOpenClass;
				if(!BuildStage(stage,state,temperature,parentIdentity,parentCandidate,*prior->target,
					stage!=2u,nil,*current,r0,error))return false;
				if(!CaptureIterationTrace(stage,iteration,state,temperature,sealedHead,
					*prior->target,*current,error))
					return false;
				float currentResidual=0.0f;if(!residual(*current,*prior,havePrior,currentResidual,
					&lastResidualComponents))return false;
				lastResidual=currentResidual;lastClassStable=classStable;
				residualHistory.push_back(currentResidual);
				if(havePrior&&classStable&&currentResidual<=request_.projectionTolerancePerS){
					profileIteration_=iteration|UINT32_C(0x80000000);
					ProfileScope terminalProfile(*this,"TerminalVerification");
					std::unique_ptr<Stage> verified(new Stage);
					if(!Project(stage,state,momentum,*current->target,
						current->projectionTargetCapability,
						current->projection.pressureOpenInflow,sealedHead,
						*verified,error))return false;
					OpenClassBytes terminalUsed,terminalNext;if(!classBytes(
						verified->projection.pressureOpenInflow,terminalUsed)||
						!classBytes(verified->nextOpenClass,terminalNext))return false;
					bool terminalTransition=!frozenCycle&&terminalUsed!=terminalNext;
					if(frozenCycle){addBranch(verified->projection.pressureOpenInflow,terminalUsed);
						addBranch(verified->nextOpenClass,terminalNext);
						if(!canonicalProjection(*current->target,current->projectionTargetCapability,
							verified))return false;
						terminalTransition=false;}
					if(!BuildStage(stage,state,temperature,parentIdentity,parentCandidate,*current->target,
						stage!=2u,nil,*verified,r0,error))return false;
					id<MTLBuffer> comparisonAlpha=verified->candidate->faceAlpha;
					id<MTLBuffer> forcedAlpha=nil;
					if(stage==1u&&request_.qualificationForceLimiterDiscontinuity){
						forcedAlpha=Private(allFaces_*sizeof(float));
						const std::uint32_t count=static_cast<std::uint32_t>(allFaces_);
						id<MTLBuffer> countBuffer=Upload(&count,sizeof(count));
						id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context_.queue);
						if(!forcedAlpha||!countBuffer||!command||!Encode(command,context_.ownerHalfField,
							{comparisonAlpha,forcedAlpha,countBuffer},allFaces_)||!Commit(command,error))return false;
						comparisonAlpha=forcedAlpha;}
					float limiterResidual=0.0f;if(stage!=2u&&!alphaResidual(current->candidate->faceAlpha,
						comparisonAlpha,limiterResidual))return false;
					const bool limiterDiscontinuous=stage!=2u&&
						limiterResidual>request_.projectionTolerancePerS;
					std::unique_ptr<Stage> certified;
					if(limiterDiscontinuous){
						if(request_.qualificationDisableLimiterCertification){
							if(error)*error="projected-Heun limiter-discontinuity mutant refused";
							return false;
						}
						id<MTLBuffer> selectedAlpha=Private(allFaces_*sizeof(float));
						const std::uint32_t count=static_cast<std::uint32_t>(allFaces_);
						id<MTLBuffer> countBuffer=Upload(&count,sizeof(count));
						id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context_.queue);
						if(!selectedAlpha||!countBuffer||!command||!Encode(command,context_.ownerMinimumField,
							{current->candidate->faceAlpha,comparisonAlpha,selectedAlpha,countBuffer},allFaces_)||
							!Commit(command,error))return false;
						certified.reset(new Stage);certified->projection=verified->projection;
						certified->projectionDiagnostics=verified->projectionDiagnostics;
						certified->nextOpenClass=verified->nextOpenClass;
						certified->endpointClassProducer=std::move(verified->endpointClassProducer);
						certified->sealedEndpointClassProducer=
							std::move(verified->sealedEndpointClassProducer);
						certified->projectionTargetCapability=verified->projectionTargetCapability;
						if(!BuildStage(stage,state,temperature,parentIdentity,parentCandidate,*current->target,
							stage!=2u,selectedAlpha,*certified,r0,error))return false;
						certified->limiterDiscontinuousClass=true;
					}else certified=std::move(verified);
					if(!CaptureIterationTrace(stage,iteration|UINT32_C(0x80000000),state,
						temperature,sealedHead,*current->target,*certified,error))return false;
					float verificationResidual=0.0f;if(!residual(*certified,*current,true,
						verificationResidual,0))return false;
					if(terminalTransition||verificationResidual>request_.projectionTolerancePerS){
						activeClass=certified->nextOpenClass;prior=std::move(certified);havePrior=true;continue;}
					certified->acceptedIterationCount=iteration+1u;
					certified->terminalReprojectionVerified=true;
					certified->projectionTargetAssembled=current->target->assembled;
					certified->projectionTargetCorrectionIteration=
						current->target->correctionIteration;
					certified->activeSetDiscontinuousClass=frozenCycle;
					certified->activeSetCycleLength=static_cast<std::uint32_t>(cycleBranches.size());
					certified->activeSetCanonicalProjectionCount=canonicalCount;
					certified->picardResidualPerS=residualHistory;
					accepted=std::move(certified);return true;
				}
				prior=std::move(current);
				havePrior=true;
			}
			if(error){*error="projected-Heun resident Picard stage did not converge: stage="+
				std::to_string(stage)+" residual="+std::to_string(lastResidual)+
				" target="+std::to_string(lastResidualComponents[0])+
				" momentum="+std::to_string(lastResidualComponents[1])+
				" transport="+std::to_string(lastResidualComponents[2])+
				" class_stable="+std::to_string(lastClassStable?1u:0u)+
				" frozen_cycle="+std::to_string(frozenCycle?1u:0u)+" history=";
				for(const float residualValue:residualHistory)*error+=
					std::to_string(residualValue)+",";}
			return false;
		}

		bool ResidentProjectedHeunMetalOwner::Run(
			FireProductionProjectedHeunMetalOwnerResult& result,std::string* error)
		{
			result=FireProductionProjectedHeunMetalOwnerResult();
			ProfileScope ownerProfile(*this,"OwnerRun");
			const auto wallStart=std::chrono::steady_clock::now();
			std::uint64_t preflightBytes=0u;
			const FireProductionProjectionShape& preflightShape=
				request_.lineage.eos.physicalFlux.transport.shape;
			if(!FireProductionProjectedHeunMetalOwnerWorkingSetBytes(preflightShape,preflightBytes)||
				(request_.qualificationWorkingSetLimitBytes!=0u&&
				 preflightBytes>request_.qualificationWorkingSetLimitBytes)){
				if(error)*error="projected-Heun complete-owner working-set preflight refused";
				return false;
			}
			const std::uint64_t deviceWorkingSetBytes=context_.device?
				static_cast<std::uint64_t>([context_.device recommendedMaxWorkingSetSize]):0u;
			if(deviceWorkingSetBytes==0u||preflightBytes>deviceWorkingSetBytes){
				if(error)*error="projected-Heun complete-owner working set exceeds the Metal device limit";
				return false;
			}
			result.certifiedWorkingSetBytes=preflightBytes;
			if(!Prepare(error))return false;
			transferPhase_=OwnerTransferPhase::Interstage;
			std::unique_ptr<Stage> r0,r1,r2;
			if(!SolveStage(0u,q0_,t0_,m0_,rootCandidateIdentity_,0,0,nil,nil,r0,error))return false;
			if(!SolveStage(1u,r0->candidate->conservative,r0->eos->temperature,r0->nextMomentum,
				r0->candidate->publicationIdentity,r0->candidate.get(),r0.get(),nil,nil,r1,error))return false;
			integratedOpenHead_=Private(boundaryFaces_*sizeof(float));
			id<MTLBuffer> ambientDensity=Upload(&request_.ambientDensityKGPerM3,
				sizeof(request_.ambientDensityKGPerM3));
			id<MTLCommandBuffer> headCommand=TrackedMetalCommandBuffer(context_.queue);
			if(!integratedOpenHead_||!ambientDensity||!headCommand||!Encode(headCommand,
				context_.ownerIntegratedOpenHead,{r0->packedVelocity,r1->packedVelocity,
				r0->projection.pressureOpenInflow,r1->projection.pressureOpenInflow,
				integratedOpenHead_,transportParameters_[0],ambientDensity},boundaryFaces_)||
				!Commit(headCommand,error))return false;
			if(!SolveStage(2u,r1->candidate->conservative,r1->eos->temperature,r1->nextMomentum,
				r1->candidate->publicationIdentity,r1->candidate.get(),r0.get(),
				r1->projection.pressureOpenInflow,
				integratedOpenHead_,r2,error))return false;
			profileStage_=UINT32_MAX;profileIteration_=UINT32_MAX;
			ProfileScope publicationProfile(*this,"TerminalPublication");
			if(!r0->terminalReprojectionVerified||!r1->terminalReprojectionVerified||
				!r2->terminalReprojectionVerified){
				if(error)*error="projected-Heun publication requires current-target terminal projection";
				return false;
			}
			if(request_.qualificationInjectInterstageTransfer){
				id<MTLBuffer> staged=[context_.device newBufferWithLength:cells_*sizeof(float)
					options:MTLResourceStorageModeShared];
				id<MTLBuffer> substituted=Private(cells_*sizeof(float));
				id<MTLCommandBuffer> transferCommand=TrackedMetalCommandBuffer(context_.queue);
				id<MTLBlitCommandEncoder> transfer=transferCommand?
					[transferCommand blitCommandEncoder]:nil;
				if(!staged||!substituted||!transfer)return false;
				Copy(transfer,r1->candidate->conservative,0u,staged,0u,cells_*sizeof(float),
					TransferKind::Internal);
				Copy(transfer,staged,0u,substituted,0u,cells_*sizeof(float),
					TransferKind::Internal);
				[transfer endEncoding];if(!Commit(transferCommand,error))return false;
			}
			if(interstageFullGridTransfers_!=0u){
				if(error)*error="projected-Heun atomic publication refuses interstage full-grid transfer: count="+
					std::to_string(interstageFullGridTransfers_);
				return false;
			}
			if(request_.qualificationStaleCandidate)
				r2->candidate->parentCandidateCapability=r0->candidate->capability;
			auto sealedStage=[&](const Stage& value,
				const ResidentEOSCandidateMetalAuthority* expectedCandidateParent){
				const ResidentPhysicalFluxMetalAuthority* candidatePhysical=
					value.averagedPhysical?value.averagedPhysical.get():value.physical.get();
				return value.lineageSeal&&value.lineageSeal==value.issuedLineageSeal&&
					value.projection.publicationIdentity&&value.transport&&value.physical&&
					candidatePhysical&&value.candidate&&value.eos&&value.source&&value.target&&
					value.candidate->capability&&value.target->capability&&
					((value.candidate->rootParent&&expectedCandidateParent==0&&
						!value.candidate->parentCandidateCapability)||
					 (!value.candidate->rootParent&&expectedCandidateParent&&
						value.candidate->parentCandidateCapability==expectedCandidateParent->capability))&&
					value.projectionTargetCapability&&
					value.target->parentTargetCapability==value.projectionTargetCapability&&
					value.endpointClass&&value.endpointClass->projectionBound&&
					value.endpointClass->classes==value.endpointClass->sealedClasses&&
					value.endpointClass->parentProjectionPublicationIdentity==
						value.projection.publicationIdentity&&
					value.endpointClass->parentTransportPublicationIdentity==
						value.transport->publicationIdentity&&
					value.physical->parentEndpointClassPublicationIdentity==
						value.endpointClass->publicationIdentity&&
					value.physical->parentTransportPublicationIdentity==
						value.transport->publicationIdentity&&
					candidatePhysical->parentTransportPublicationIdentity==
						value.transport->publicationIdentity&&
					value.candidate->parentTransportPublicationIdentity==
						value.transport->publicationIdentity&&
					value.candidate->parentPhysicalPublicationIdentity==
						candidatePhysical->publicationIdentity&&
					value.eos->parentTransportPublicationIdentity==
						value.transport->publicationIdentity&&
					value.eos->parentPhysicalPublicationIdentity==
						candidatePhysical->publicationIdentity&&
					value.eos->parentCandidatePublicationIdentity==
						value.candidate->publicationIdentity&&
					value.source->parentCandidatePublicationIdentity==
						value.candidate->publicationIdentity&&
					value.target->parentTransportPublicationIdentity==
						value.transport->publicationIdentity&&
					value.target->parentTangentPhysicalPublicationIdentity==
						value.physical->publicationIdentity&&
					value.target->parentPhysicalPublicationIdentity==
						candidatePhysical->publicationIdentity&&
					value.target->parentCandidatePublicationIdentity==
						value.candidate->publicationIdentity&&
					value.target->parentEOSPublicationIdentity==value.eos->publicationIdentity&&
					value.target->parentFrozenSourcePublicationIdentity==
						value.source->publicationIdentity;
			};
			if(!sealedStage(*r0,0)||!sealedStage(*r1,r0->candidate.get())||
				!sealedStage(*r2,r1->candidate.get())){
				if(error)*error="projected-Heun publication refuses unsealed parent relationship";
				return false;
			}
			transferPhase_=OwnerTransferPhase::Publication;
			id<MTLBuffer> ownerIdentity=Private(sizeof(std::uint64_t));
			id<MTLBuffer> unitVelocity=Private(allFaces_*sizeof(float));
			id<MTLBuffer> identityRate=Private(allFaces_*sizeof(float));
			id<MTLBuffer> commutingResidual=Private(sizeof(std::uint32_t));
			id<MTLBuffer> commutingScale=Private(sizeof(std::uint32_t));
			id<MTLBuffer> heunAdvection=Private(allFaces_*sizeof(float));
			id<MTLBuffer> heunBuoyancy=Private(allFaces_*sizeof(float));
			id<MTLBuffer> heunStress=Private(allFaces_*sizeof(float));
			id<MTLBuffer> heunPhaseSource=Private(allFaces_*sizeof(float));
			id<MTLBuffer> heunEddy=Private(cells_*sizeof(float));
			id<MTLBuffer> faceCount=Upload(&ownerMetadata_.allFaces,sizeof(std::uint32_t));
			const std::uint32_t cellCountValue=static_cast<std::uint32_t>(cells_);
			id<MTLBuffer> cellCount=Upload(&cellCountValue,sizeof(cellCountValue));
			const float positiveHalf=0.5f,negativeHalf=-0.5f;
			id<MTLBuffer> positiveHalfBuffer=Upload(&positiveHalf,sizeof(positiveHalf));
			id<MTLBuffer> negativeHalfBuffer=Upload(&negativeHalf,sizeof(negativeHalf));
			const std::size_t stateBytes=9u*cells_*sizeof(float),fieldBytes=cells_*sizeof(float);
			// Nine packed face fields are actually published. The legacy terminal
			// allocation reserved thirteen; unwritten capacity is not payload.
			const std::size_t publicationBytes=stateBytes+9u*allFaces_*sizeof(float)+10u*fieldBytes+
				2u*sizeof(std::uint32_t)+22u*sizeof(std::uint64_t)+32u;
			const std::size_t terminalBytes=publicationBytes+32u;
			id<MTLBuffer> terminal=[context_.device newBufferWithLength:terminalBytes
				options:MTLResourceStorageModeShared];
			id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context_.queue);
			if(!ownerIdentity||!unitVelocity||!identityRate||!commutingResidual||
				!commutingScale||!heunAdvection||!heunBuoyancy||!heunStress||!heunPhaseSource||
				!heunEddy||!faceCount||!cellCount||!positiveHalfBuffer||!negativeHalfBuffer||
				!terminal||!ResetControls(command))return false;
			id<MTLBlitCommandEncoder> commutingClear=[command blitCommandEncoder];
			if(!commutingClear)return false;
			[commutingClear fillBuffer:commutingResidual range:NSMakeRange(0,sizeof(std::uint32_t))
				value:0u];
			[commutingClear fillBuffer:commutingScale range:NSMakeRange(0,sizeof(std::uint32_t))
				value:0u];
			[commutingClear endEncoding];
			if(!Encode(command,context_.ownerAverageField,{r1->heunR0AdvectionRate,r1->advectionRate,
				heunAdvection,faceCount,negativeHalfBuffer},allFaces_)||
				!Encode(command,context_.ownerAverageField,
					{r0->nonpressure.buoyancyMomentumRateKGPerM2S2,
					r1->nonpressure.buoyancyMomentumRateKGPerM2S2,heunBuoyancy,faceCount,
					positiveHalfBuffer},allFaces_)||
				!Encode(command,context_.ownerAverageField,
					{r0->nonpressure.stressMomentumRateKGPerM2S2,
					r1->nonpressure.stressMomentumRateKGPerM2S2,heunStress,faceCount,
					positiveHalfBuffer},allFaces_)||
				!Encode(command,context_.ownerAverageField,
					{r0->nonpressure.phaseSourceMomentumRateKGPerM2S2,
					r1->nonpressure.phaseSourceMomentumRateKGPerM2S2,heunPhaseSource,faceCount,
					positiveHalfBuffer},allFaces_)||
				!Encode(command,context_.ownerAverageField,
					{r0->nonpressure.eddyKinematicViscosityM2PerS,
					r1->nonpressure.eddyKinematicViscosityM2PerS,heunEddy,cellCount,
					positiveHalfBuffer},cells_)||
				!Encode(command,fct_.fillOne,{unitVelocity,faceCount},allFaces_)||
				!r1->averagedPhysical||!Encode(command,fct_.compatibleStageRate,
					{r1->averagedPhysical->lowComposite,r1->averagedPhysical->advectiveDelta,
					r1->candidate->faceAlpha,unitVelocity,
					unitVelocity,unitVelocity,identityRate,failure_,fctParameters_[1]},allFaces_)||
				!Encode(command,fct_.commutingIdentity,{q0_,sourceDelta_,
					r1->candidate->conservative,ambient_,identityRate,commutingResidual,
					commutingScale,failure_,fctParameters_[1]},allFaces_))return false;
			if(request_.qualificationForgedLineage||
				request_.qualificationCallbackMutation||request_.qualificationAtomicPublicationFailure){
				id<MTLBlitCommandEncoder> red=[command blitCommandEncoder];if(!red)return false;
				if(request_.qualificationForgedLineage)[red fillBuffer:r2->physical->publicationIdentity
					range:NSMakeRange(0,sizeof(std::uint64_t)) value:0u];
				if(request_.qualificationCallbackMutation)[red fillBuffer:r0->transport->publicationIdentity
					range:NSMakeRange(0,sizeof(std::uint64_t)) value:0u];
				if(request_.qualificationAtomicPublicationFailure){[red fillBuffer:failure_
					range:NSMakeRange(0,sizeof(std::uint32_t)) value:1u];}
				[red endEncoding];
			}
			MetalProjectedHeunOwnerParameters publicationMetadata=ownerMetadata_;
			publicationMetadata.stage=request_.qualificationOutOfOrderStage?3u:2u;
			id<MTLBuffer> publicationParameters=Upload(&publicationMetadata,
				sizeof(publicationMetadata));
			if(!publicationParameters||!Encode(command,context_.ownerIssuePublication,
				{r0->projection.publicationIdentity,r1->projection.publicationIdentity,
				r2->projection.publicationIdentity,r0->transport->publicationIdentity,
				r1->transport->publicationIdentity,r2->transport->publicationIdentity,
				r0->physical->publicationIdentity,r1->physical->publicationIdentity,
				r2->physical->publicationIdentity,r0->physical->publicationIdentity,
				r1->averagedPhysical->publicationIdentity,r2->physical->publicationIdentity,
				r0->candidate->publicationIdentity,
				r1->candidate->publicationIdentity,r2->candidate->publicationIdentity,
				r0->eos->publicationIdentity,r1->eos->publicationIdentity,r2->eos->publicationIdentity,
				r0->source->publicationIdentity,r1->source->publicationIdentity,
				r2->source->publicationIdentity,r0->target->publicationIdentity,
				r1->target->publicationIdentity,r2->target->publicationIdentity,
				r0->lineageSeal,r1->lineageSeal,r2->lineageSeal,
				ownerIdentity,failure_,publicationParameters},1u))return false;
			id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];if(!blit)return false;
			std::size_t offset=0u;auto publish=[&](id<MTLBuffer> source,std::size_t bytes,
				std::size_t sourceOffset=0u){Copy(blit,source,sourceOffset,terminal,offset,bytes,
					TransferKind::Terminal);offset+=bytes;};
			publish(r1->candidate->conservative,stateBytes);
			for(unsigned int axis=0u;axis<3u;++axis)publish(r2->projection.momentumKGPerM2S[axis],
				FireProductionProjectionFaceCount(shape_,axis)*sizeof(float));
			for(unsigned int axis=0u;axis<3u;++axis)publish(r2->projection.velocityMPerS[axis],
				FireProductionProjectionFaceCount(shape_,axis)*sizeof(float));
			for(unsigned int axis=0u;axis<3u;++axis)publish(r2->projection.faceDensityKGPerM3[axis],
				FireProductionProjectionFaceCount(shape_,axis)*sizeof(float));
			publish(r1->nextMomentum,allFaces_*sizeof(float));
			publish(heunAdvection,allFaces_*sizeof(float));publish(heunBuoyancy,allFaces_*sizeof(float));
			publish(heunStress,allFaces_*sizeof(float));publish(heunPhaseSource,allFaces_*sizeof(float));
			publish(r1->candidate->faceAlpha,allFaces_*sizeof(float));
			publish(r1->eos->temperature,fieldBytes);publish(r1->eos->representedPressureRatio,fieldBytes);
			publish(r1->eos->absoluteDeviation,fieldBytes);publish(heunEddy,fieldBytes);
			const Stage* acceptedStages[3]={r0.get(),r1.get(),r2.get()};
			for(const Stage* acceptedStage:acceptedStages)
				publish(acceptedStage->projectionTargetAssembled,fieldBytes);
			for(const Stage* acceptedStage:acceptedStages)
				publish(acceptedStage->target->assembled,fieldBytes);
			publish(commutingResidual,sizeof(std::uint32_t));
			publish(commutingScale,sizeof(std::uint32_t));
			const id<MTLBuffer> identities[]={r0->projection.publicationIdentity,
				r1->projection.publicationIdentity,r2->projection.publicationIdentity,
				r0->transport->publicationIdentity,r1->transport->publicationIdentity,
				r2->transport->publicationIdentity,r0->physical->publicationIdentity,
				r1->physical->publicationIdentity,r2->physical->publicationIdentity,
				r0->candidate->publicationIdentity,r1->candidate->publicationIdentity,
				r2->candidate->publicationIdentity,r0->eos->publicationIdentity,
				r1->eos->publicationIdentity,r2->eos->publicationIdentity,
				r0->source->publicationIdentity,r1->source->publicationIdentity,
				r2->source->publicationIdentity,r0->target->publicationIdentity,
				r1->target->publicationIdentity,r2->target->publicationIdentity};
			for(id<MTLBuffer> identity:identities)publish(identity,sizeof(std::uint64_t));
			publish(ownerIdentity,sizeof(std::uint64_t));publish(inputPayloadDigest_,32u);[blit endEncoding];
			if(offset!=publicationBytes){if(error)*error="owner publication payload layout differs from its seal";return false;}
			if(!HashPayload(command,terminal,publicationBytes,terminal,publicationBytes,error))return false;
			if(!Commit(command,error))return false;const unsigned char* bytes=
				static_cast<const unsigned char*>(Read(terminal,TransferKind::Terminal));if(!bytes)return false;
			auto hexRoot=[](const unsigned char* root){const char* alphabet="0123456789abcdef";std::string text;
				for(unsigned int i=0u;i<32u;++i){text+=alphabet[root[i]>>4u];text+=alphabet[root[i]&15u];}return text;};
			result.intermediateSealFormat=context_.productionStageTokens?"qualified-kernel-stage-token":"legacy-resident-fnv64";
			result.intermediateDigestVersion=context_.productionStageTokens?2u:1u;
			result.qualifiedKernelSetSHA256=context_.eosLogIdentity.librarySourceSHA256;
			result.inputPayloadRootSHA256=hexRoot(bytes+publicationBytes-32u);
			result.publicationPayloadRootSHA256=hexRoot(bytes+publicationBytes);
			result.publicationPayloadBytes=publicationBytes;
			offset=0u;auto floats=[&](std::vector<float>& destination,std::size_t count){const float* value=
				reinterpret_cast<const float*>(bytes+offset);destination.assign(value,value+count);
				offset+=count*sizeof(float);};floats(result.conservativeValues,9u*cells_);
			for(unsigned int axis=0u;axis<3u;++axis)floats(result.momentumKGPerM2S[axis],
				FireProductionProjectionFaceCount(shape_,axis));
			for(unsigned int axis=0u;axis<3u;++axis)floats(result.velocityMPerS[axis],
				FireProductionProjectionFaceCount(shape_,axis));
			std::array<std::vector<float>,3> finalFaceDensity;for(unsigned int axis=0u;axis<3u;++axis)
				floats(finalFaceDensity[axis],FireProductionProjectionFaceCount(shape_,axis));
			auto packedFaces=[&](std::array<std::vector<float>,3>& destination){
				for(unsigned int axis=0u;axis<3u;++axis)floats(destination[axis],
					FireProductionProjectionFaceCount(shape_,axis));};
			packedFaces(result.provisionalMomentumKGPerM2S);
			packedFaces(result.heunAdvectionMomentumRateKGPerM2S2);
			packedFaces(result.heunBuoyancyMomentumRateKGPerM2S2);
			packedFaces(result.heunStressMomentumRateKGPerM2S2);
			packedFaces(result.heunPhaseSourceMomentumRateKGPerM2S2);
			floats(result.acceptedFaceAlpha,allFaces_);
			floats(result.temperatureK,cells_);floats(result.representedPressureRatio,cells_);
			floats(result.absoluteEOSDeviation,cells_);floats(result.heunEddyKinematicViscosityM2PerS,cells_);
			for(unsigned int stage=0u;stage<3u;++stage)floats(result.projectionTargetPerS[stage],cells_);
			for(unsigned int stage=0u;stage<3u;++stage)floats(result.acceptedTargetPerS[stage],cells_);
			const std::uint32_t* commutingBits=reinterpret_cast<const std::uint32_t*>(bytes+offset);
			std::memcpy(&result.maximumCommutingResidualKGPerM3,&commutingBits[0],sizeof(float));
			std::memcpy(&result.commutingIdentityScaleKGPerM3,&commutingBits[1],sizeof(float));
			offset+=2u*sizeof(std::uint32_t);
			// Odd face counts need not place this byte packet on an eight-byte
			// boundary. memcpy preserves the representation without an unaligned
			// uint64_t typed load.
			std::array<std::uint64_t,22> ids;
			std::memcpy(ids.data(),bytes+offset,sizeof(ids));
			for(unsigned int index=0u;index<3u;++index)result.projectionPublicationIdentity[index]=ids[index];
			for(unsigned int index=0u;index<3u;++index)result.transportPublicationIdentity[index]=ids[3u+index];
			for(unsigned int index=0u;index<3u;++index){
				result.physicalFluxPublicationIdentity[index]=ids[6u+index];
				result.candidatePublicationIdentity[index]=ids[9u+index];
				result.EOSPublicationIdentity[index]=ids[12u+index];
				result.frozenSourcePublicationIdentity[index]=ids[15u+index];
				result.targetPublicationIdentity[index]=ids[18u+index];}
			result.ownerPublicationIdentity=ids[21];
			if(request_.qualificationAtomicPublicationFailure||result.ownerPublicationIdentity==0u){
				result=FireProductionProjectedHeunMetalOwnerResult();
				if(error)*error="projected-Heun resident owner atomic publication refused";
				return false;
			}
			result.acceptedPicardIterations={{r0->acceptedIterationCount,
				r1->acceptedIterationCount,r2->acceptedIterationCount}};
			result.picardResidualPerS={{r0->picardResidualPerS,r1->picardResidualPerS,
				r2->picardResidualPerS}};
			result.qualificationIterationTrace=qualificationTrace_;
			result.qualificationTraceStagingCount=qualificationTraceStagingCount_;
			result.activeSetCycleLength={{r0->activeSetCycleLength,r1->activeSetCycleLength,
				r2->activeSetCycleLength}};
			result.activeSetCanonicalProjectionCount={{r0->activeSetCanonicalProjectionCount,
				r1->activeSetCanonicalProjectionCount,r2->activeSetCanonicalProjectionCount}};
			result.activeSetDiscontinuousClass={{r0->activeSetDiscontinuousClass,
				r1->activeSetDiscontinuousClass,r2->activeSetDiscontinuousClass}};
			result.limiterDiscontinuousClass={{r0->limiterDiscontinuousClass,
				r1->limiterDiscontinuousClass,r2->limiterDiscontinuousClass}};
			result.projectionTargetCorrectionIteration={{r0->projectionTargetCorrectionIteration,
				r1->projectionTargetCorrectionIteration,r2->projectionTargetCorrectionIteration}};
			result.acceptedTargetCorrectionIteration={{r0->target->correctionIteration,
				r1->target->correctionIteration,r2->target->correctionIteration}};
			result.projection=r2->projectionDiagnostics;
			result.projection.momentumKGPerM2S=result.momentumKGPerM2S;
			result.projection.velocityMPerS=result.velocityMPerS;
			result.projection.faceDensityKGPerM3=std::move(finalFaceDensity);
			result.commandCommitCount=commits_;
			result.residentProjectionInvocationCount=projectionInvocations_;
			result.terminalStagingCount=1u;
			result.interstageFullGridTransferCount=interstageFullGridTransfers_;
			ObserveDeviceAllocation();result.actualMetalAllocationBytes=deviceAllocationPeak_;
			result.certifiedWorkingSetBytes=preflightBytes;
			result.deviceElapsedMS=deviceMS_;result.wallElapsedMS=std::chrono::duration<double,std::milli>(
				std::chrono::steady_clock::now()-wallStart).count();
			result.residentProjectionDeviceElapsedMS=projectionDeviceMS_;
			result.residentNonprojectionDeviceElapsedMS=std::max(0.0,deviceMS_-projectionDeviceMS_);
			const float unitRoundoff=0x1p-24f;
			const float gamma128=(128.0f*unitRoundoff)/(1.0f-128.0f*unitRoundoff);
			result.commutingIdentityBoundKGPerM3=
				gamma128*result.commutingIdentityScaleKGPerM3;
			result.commutingIdentityPassed=result.maximumCommutingResidualKGPerM3<=
				result.commutingIdentityBoundKGPerM3;
			result.accepted=result.actualMetalAllocationBytes<=result.certifiedWorkingSetBytes;
			result.accepted=result.accepted&&result.commutingIdentityPassed;
			if(!result.accepted){if(error)*error="projected-Heun resident owner certificate failed";return false;}
			if(error)error->clear();return true;
		}

		// R201_RESIDENT_OWNER_TRANSFER_SURFACE_END

		bool EncodeResidentEndpointClassAuthority(
			ResidentTransportMetalContext& context,id<MTLCommandBuffer> command,
			id<MTLBuffer> classes,id<MTLBuffer> projectionPublicationIdentity,
			id<MTLBuffer> transportParameters,id<MTLBuffer> failure,
			const ResidentTransportMetalAuthority& transportAuthority,
			const std::size_t cells,const std::size_t allFaces,
			const std::size_t boundaryFaces,const bool projectionBound,
			const ResidentEndpointClassProducerAuthority* producer,
			ResidentEndpointClassMetalAuthority& authority,std::string* error )
		{
			if(!command||!classes||!projectionPublicationIdentity||!transportParameters||!failure||
				cells==0u||allFaces==0u||boundaryFaces==0u){
				if(error)*error="production resident endpoint-class authority input is absent";
				return false;
			}
			if(command!=transportAuthority.parentCommand||
				transportParameters!=transportAuthority.parentParameters||
				failure!=transportAuthority.parentFailure||cells!=transportAuthority.parentCells||
				allFaces!=transportAuthority.parentAllFaces||
				boundaryFaces!=transportAuthority.parentBoundaryFaces){
				if(error)*error="production resident endpoint-class parent lineage is stale";
				return false;
			}
			if(projectionBound&&(!producer||!producer->capability||
				producer->classes!=classes||producer->projectionPublicationIdentity!=
					projectionPublicationIdentity||producer->transportParameters!=transportParameters||
				!producer->ownerParameters||!producer->producingCommand||
				[producer->producingCommand status]!=MTLCommandBufferStatusCompleted)){
				if(error)*error="production resident endpoint-class producer capability is unverified";
				return false;
			}
			if(!projectionBound&&producer){
				if(error)*error="preauthored endpoint class cannot carry an owner producer capability";
				return false;
			}
			const std::array<id<MTLBuffer>,6> inputs={{classes,projectionPublicationIdentity,
				transportAuthority.publicationIdentity,transportParameters,failure,
				transportAuthority.coefficients}};
			for(id<MTLBuffer> buffer:inputs)if(!buffer||[buffer device]!=context.device||
				[buffer storageMode]!=MTLStorageModePrivate){if(error)*error=
					"production resident endpoint-class authority requires device-private lineage";
				return false;}
			if([classes length]!=boundaryFaces*sizeof(unsigned char)||
				[projectionPublicationIdentity length]!=sizeof(std::uint64_t)||
				[transportAuthority.publicationIdentity length]!=sizeof(std::uint64_t)||
				[transportParameters length]!=sizeof(MetalResidentTransportParameters)||
				[failure length]!=sizeof(std::uint32_t)){
				if(error)*error="production resident endpoint-class authority extent is invalid";
				return false;
			}
			authority.classes=classes;authority.sealedClasses=classes;
			authority.publicationIdentity=[context.device newBufferWithLength:sizeof(std::uint64_t)
				options:MTLResourceStorageModePrivate];
			if(!authority.publicationIdentity){
				if(error)*error="production resident endpoint-class identity allocation failed";
				return false;
			}
			authority.parentCommand=command;
			authority.parentProjectionPublicationIdentity=projectionPublicationIdentity;
			authority.parentTransportPublicationIdentity=transportAuthority.publicationIdentity;
			authority.parentTransportParameters=transportParameters;
			authority.parentOwnerParameters=producer?producer->ownerParameters:nil;
			authority.producerCapability=producer?producer->capability:std::shared_ptr<unsigned char>();
			authority.parentBoundaryFaces=boundaryFaces;authority.parentCells=cells;
			authority.parentAllFaces=allFaces;authority.projectionBound=projectionBound;
			authority.allocationBytes=[authority.publicationIdentity allocatedSize];
			id<MTLComputeCommandEncoder> encoder=ProducerProfileEncoder(command);
			if(!encoder){if(error)*error="production resident endpoint-class identity encoder failed";
				return false;}
			[encoder setComputePipelineState:producer?context.ownerIdentifyProducedEndpointClass:
				context.ownerIdentifyEndpointClass];
			[encoder setBuffer:classes offset:0 atIndex:0];
			[encoder setBuffer:projectionPublicationIdentity offset:0 atIndex:1];
			[encoder setBuffer:transportAuthority.publicationIdentity offset:0 atIndex:2];
			[encoder setBuffer:authority.publicationIdentity offset:0 atIndex:3];
			[encoder setBuffer:failure offset:0 atIndex:4];
			[encoder setBuffer:transportParameters offset:0 atIndex:5];
			if(producer)[encoder setBuffer:producer->ownerParameters offset:0 atIndex:6];
			Dispatch(encoder,producer?context.ownerIdentifyProducedEndpointClass:
				context.ownerIdentifyEndpointClass,1u);[encoder endEncoding];
			return true;
		}

		bool EncodeResidentPhysicalFluxAuthority(
			ResidentTransportMetalContext& context,id<MTLCommandBuffer> command,
			id<MTLBuffer> state,id<MTLBuffer> temperature,id<MTLBuffer> velocity,
			id<MTLBuffer> thermochemistry,id<MTLBuffer> ambient,
			id<MTLBuffer> physicalBasis,id<MTLBuffer> advectiveBasis,id<MTLBuffer> projector,
			id<MTLBuffer> transportParameters,id<MTLBuffer> physicalParameters,
			id<MTLBuffer> failure,id<MTLBuffer> obligations,
			const ResidentTransportMetalAuthority& transportAuthority,
			const ResidentEndpointClassMetalAuthority& endpointClassAuthority,
			const std::size_t cells,const std::size_t allFaces,
			const MetalResidentPhysicalFluxParameters& metadata,
			ResidentPhysicalFluxMetalAuthority& authority,std::string* error )
		{
			id<MTLBuffer> inflow=endpointClassAuthority.classes;
			const std::array<id<MTLBuffer>,16> inputs={{state,temperature,velocity,
				thermochemistry,ambient,inflow,physicalBasis,advectiveBasis,projector,
				transportParameters,physicalParameters,failure,obligations,
				transportAuthority.coefficients,transportAuthority.publicationIdentity,
				endpointClassAuthority.publicationIdentity}};
			if(!command||cells==0u||allFaces==0u||metadata.advectiveNullity==0u||
				metadata.advectiveNullity>8u||metadata.physicalNullity==0u||
				metadata.physicalNullity>8u){
				if(error)*error="production resident physical-flux authority input is absent";
				return false;
			}
			if(command!=transportAuthority.parentCommand||state!=transportAuthority.parentState||
				temperature!=transportAuthority.parentTemperature||
				velocity!=transportAuthority.parentVelocity||
				thermochemistry!=transportAuthority.parentThermochemistry||
				transportParameters!=transportAuthority.parentParameters||
				failure!=transportAuthority.parentFailure||
				cells!=transportAuthority.parentCells||allFaces!=transportAuthority.parentAllFaces){
				if(error)*error="production resident physical-flux parent candidate lineage is stale";
				return false;
			}
			if(command!=endpointClassAuthority.parentCommand||
				endpointClassAuthority.classes!=endpointClassAuthority.sealedClasses||
				endpointClassAuthority.parentTransportPublicationIdentity!=
					transportAuthority.publicationIdentity||
				endpointClassAuthority.parentTransportParameters!=transportParameters||
				endpointClassAuthority.parentCells!=cells||
				endpointClassAuthority.parentAllFaces!=allFaces||
				endpointClassAuthority.parentBoundaryFaces!=transportAuthority.parentBoundaryFaces){
				if(error)*error="production resident physical-flux endpoint-class lineage is stale";
				return false;
			}
			if(endpointClassAuthority.projectionBound&&
				(!endpointClassAuthority.producerCapability||
				 !endpointClassAuthority.parentOwnerParameters)){
				if(error)*error="production resident physical flux refuses an unissued endpoint class";
				return false;
			}
			for(id<MTLBuffer> buffer:inputs)if(!buffer||[buffer device]!=context.device||
				[buffer storageMode]!=MTLStorageModePrivate){if(error)*error=
				"production resident physical-flux authority requires device-private lineage";
				return false;}
			if([state length]!=9u*cells*sizeof(float)||[temperature length]!=cells*sizeof(float)||
				[velocity length]!=allFaces*sizeof(float)||
				[thermochemistry length]!=MetalManifoldCertificateValues*sizeof(float)||
				[ambient length]!=9u*sizeof(float)||
				[inflow length]!=transportAuthority.parentBoundaryFaces*sizeof(unsigned char)||
				[physicalBasis length]!=8u*metadata.physicalNullity*sizeof(float)||
				[advectiveBasis length]!=8u*metadata.advectiveNullity*sizeof(float)||
				[projector length]!=metadata.advectiveNullity*metadata.advectiveNullity*sizeof(float)||
				[transportParameters length]!=sizeof(MetalResidentTransportParameters)||
				[physicalParameters length]!=sizeof(MetalResidentPhysicalFluxParameters)||
				[failure length]!=sizeof(std::uint32_t)||
				[obligations length]!=sizeof(std::uint32_t)||
				[transportAuthority.coefficients length]!=3u*cells*sizeof(float)||
				[transportAuthority.publicationIdentity length]!=sizeof(std::uint64_t)){
				if(error)*error="production resident physical-flux authority extent is invalid";
				return false;
			}
			auto make=[&](const std::size_t bytes){return [context.device newBufferWithLength:bytes
				options:MTLResourceStorageModePrivate];};
			authority.donorAdvective=make(9u*allFaces*sizeof(float));
			authority.advectiveDelta=make(9u*allFaces*sizeof(float));
			authority.mcMusclAdvective=make(9u*allFaces*sizeof(float));
			authority.physicalMass=make(8u*allFaces*sizeof(float));
			authority.physicalEnergy=make(allFaces*sizeof(float));
			authority.physicalGas=make(allFaces*sizeof(float));
			authority.faceLogTemperature=make(allFaces*sizeof(float));
			authority.faceSensibleEnthalpy=make(7u*allFaces*sizeof(float));
			authority.lowComposite=make(9u*allFaces*sizeof(float));
			authority.highComposite=make(9u*allFaces*sizeof(float));
			authority.publicationIdentity=make(sizeof(std::uint64_t));
			const std::array<id<MTLBuffer>,11> outputs={{authority.donorAdvective,
				authority.advectiveDelta,authority.mcMusclAdvective,authority.physicalMass,authority.physicalEnergy,
				authority.physicalGas,authority.faceLogTemperature,
				authority.faceSensibleEnthalpy,authority.lowComposite,
				authority.highComposite,authority.publicationIdentity}};
			for(id<MTLBuffer> buffer:outputs)if(!buffer){
				if(error)*error="production resident physical-flux authority allocation failed";
				return false;
			}
			authority.allocationBytes=0u;for(id<MTLBuffer> buffer:outputs){
				const std::uint64_t allocation=[buffer allocatedSize];
				if(authority.allocationBytes>std::numeric_limits<std::uint64_t>::max()-allocation){
					if(error)*error="production resident physical-flux allocation overflowed";
					return false;
				}
				authority.allocationBytes+=allocation;
			}
			authority.parentCommand=command;authority.parentState=state;
			authority.parentThermochemistry=thermochemistry;
			authority.parentTransportPublicationIdentity=transportAuthority.publicationIdentity;
			authority.parentEndpointClassPublicationIdentity=
				endpointClassAuthority.publicationIdentity;
			authority.parentCells=cells;authority.parentAllFaces=allFaces;
			id<MTLComputeCommandEncoder> encoder=ProducerProfileEncoder(command);
			if(!encoder){if(error)*error="production resident physical-flux encoder failed";return false;}
			[encoder setComputePipelineState:context.physicalFlux];
			[encoder setBuffer:state offset:0 atIndex:0];[encoder setBuffer:temperature offset:0 atIndex:1];
			[encoder setBuffer:transportAuthority.coefficients offset:0 atIndex:2];
			[encoder setBuffer:thermochemistry offset:0 atIndex:3];
			[encoder setBuffer:physicalBasis offset:0 atIndex:4];[encoder setBuffer:ambient offset:0 atIndex:5];
			[encoder setBuffer:inflow offset:0 atIndex:6];[encoder setBuffer:authority.physicalMass offset:0 atIndex:7];
			[encoder setBuffer:authority.physicalEnergy offset:0 atIndex:8];
			[encoder setBuffer:authority.physicalGas offset:0 atIndex:9];
			[encoder setBuffer:authority.faceLogTemperature offset:0 atIndex:10];
			[encoder setBuffer:authority.faceSensibleEnthalpy offset:0 atIndex:11];
			[encoder setBuffer:failure offset:0 atIndex:12];
			[encoder setBuffer:obligations offset:0 atIndex:13];
			[encoder setBuffer:transportParameters offset:0 atIndex:14];
			[encoder setBuffer:physicalParameters offset:0 atIndex:15];
			Dispatch(encoder,context.physicalFlux,allFaces);[encoder endEncoding];
			encoder=ProducerProfileEncoder(command);if(!encoder){
				if(error)*error="production resident advective-flux encoder failed";
				return false;
			}
			[encoder setComputePipelineState:context.advectivePair];[encoder setBuffer:state offset:0 atIndex:0];
			[encoder setBuffer:velocity offset:0 atIndex:1];[encoder setBuffer:ambient offset:0 atIndex:2];
			[encoder setBuffer:inflow offset:0 atIndex:3];[encoder setBuffer:advectiveBasis offset:0 atIndex:4];
			[encoder setBuffer:projector offset:0 atIndex:5];[encoder setBuffer:authority.donorAdvective offset:0 atIndex:6];
			[encoder setBuffer:authority.advectiveDelta offset:0 atIndex:7];
			[encoder setBuffer:obligations offset:0 atIndex:8];[encoder setBuffer:transportParameters offset:0 atIndex:9];
			[encoder setBuffer:physicalParameters offset:0 atIndex:10];
			Dispatch(encoder,context.advectivePair,9u*allFaces);[encoder endEncoding];
			encoder=ProducerProfileEncoder(command);if(!encoder){
				if(error)*error="production resident MC-MUSCL finalization encoder failed";
				return false;
			}
			[encoder setComputePipelineState:context.finalizeAdvective];
			[encoder setBuffer:authority.donorAdvective offset:0 atIndex:0];
			[encoder setBuffer:authority.advectiveDelta offset:0 atIndex:1];
			[encoder setBuffer:authority.mcMusclAdvective offset:0 atIndex:2];
			[encoder setBuffer:transportParameters offset:0 atIndex:3];
			Dispatch(encoder,context.finalizeAdvective,9u*allFaces);[encoder endEncoding];
			encoder=ProducerProfileEncoder(command);if(!encoder){
				if(error)*error="production resident flux composition encoder failed";
				return false;
			}
			[encoder setComputePipelineState:context.composePair];
			[encoder setBuffer:authority.donorAdvective offset:0 atIndex:0];
			[encoder setBuffer:authority.mcMusclAdvective offset:0 atIndex:1];
			[encoder setBuffer:authority.physicalMass offset:0 atIndex:2];
			[encoder setBuffer:authority.physicalEnergy offset:0 atIndex:3];
			[encoder setBuffer:authority.lowComposite offset:0 atIndex:4];
			[encoder setBuffer:authority.highComposite offset:0 atIndex:5];
			[encoder setBuffer:transportParameters offset:0 atIndex:6];
			[encoder setBuffer:physicalParameters offset:0 atIndex:7];
			Dispatch(encoder,context.composePair,9u*allFaces);[encoder endEncoding];
			encoder=ProducerProfileEncoder(command);if(!encoder){
				if(error)*error="production resident shared-f_N validation encoder failed";
				return false;
			}
			[encoder setComputePipelineState:context.validatePhysical];
			[encoder setBuffer:authority.donorAdvective offset:0 atIndex:0];
			[encoder setBuffer:authority.mcMusclAdvective offset:0 atIndex:1];
			[encoder setBuffer:authority.physicalMass offset:0 atIndex:2];
			[encoder setBuffer:authority.physicalEnergy offset:0 atIndex:3];
			[encoder setBuffer:authority.physicalGas offset:0 atIndex:4];
			[encoder setBuffer:authority.lowComposite offset:0 atIndex:5];
			[encoder setBuffer:authority.highComposite offset:0 atIndex:6];
			[encoder setBuffer:failure offset:0 atIndex:7];[encoder setBuffer:obligations offset:0 atIndex:8];
			[encoder setBuffer:transportParameters offset:0 atIndex:9];
			Dispatch(encoder,context.validatePhysical,9u*allFaces);[encoder endEncoding];
			encoder=ProducerProfileEncoder(command);if(!encoder){
				if(error)*error="production resident physical-flux identity encoder failed";
				return false;
			}
			[encoder setComputePipelineState:context.identifyPhysical];
			[encoder setBuffer:authority.donorAdvective offset:0 atIndex:0];
			[encoder setBuffer:authority.advectiveDelta offset:0 atIndex:1];
			[encoder setBuffer:authority.mcMusclAdvective offset:0 atIndex:2];
			[encoder setBuffer:authority.physicalMass offset:0 atIndex:3];
			[encoder setBuffer:authority.physicalEnergy offset:0 atIndex:4];
			[encoder setBuffer:authority.physicalGas offset:0 atIndex:5];
			[encoder setBuffer:authority.lowComposite offset:0 atIndex:6];
			[encoder setBuffer:authority.highComposite offset:0 atIndex:7];
			[encoder setBuffer:authority.faceLogTemperature offset:0 atIndex:8];
			[encoder setBuffer:authority.faceSensibleEnthalpy offset:0 atIndex:9];
			[encoder setBuffer:transportAuthority.publicationIdentity offset:0 atIndex:10];
			[encoder setBuffer:ambient offset:0 atIndex:11];[encoder setBuffer:inflow offset:0 atIndex:12];
			[encoder setBuffer:physicalBasis offset:0 atIndex:13];
			[encoder setBuffer:advectiveBasis offset:0 atIndex:14];[encoder setBuffer:projector offset:0 atIndex:15];
			[encoder setBuffer:endpointClassAuthority.publicationIdentity offset:0 atIndex:16];
			[encoder setBuffer:authority.publicationIdentity offset:0 atIndex:17];
			[encoder setBuffer:failure offset:0 atIndex:18];[encoder setBuffer:transportParameters offset:0 atIndex:19];
			[encoder setBuffer:physicalParameters offset:0 atIndex:20];
			Dispatch(encoder,context.identifyPhysical,[context.identifyPhysical threadExecutionWidth]);
			[encoder endEncoding];
			return true;
		}

		bool EncodeResidentEOSQualificationCandidate(
			ResidentTransportMetalContext& context,id<MTLCommandBuffer> command,
			id<MTLBuffer> forbiddenCPUCandidate,id<MTLBuffer> sourceDelta,
			id<MTLBuffer> enthalpyBounds,id<MTLBuffer> affine,id<MTLBuffer> fctParameters,
			id<MTLBuffer> transportParameters,id<MTLBuffer> eosThermochemistry,
			id<MTLBuffer> eosParameters,id<MTLBuffer> failure,id<MTLBuffer> obligations,
			const ResidentTransportMetalAuthority& transportAuthority,
			const ResidentPhysicalFluxMetalAuthority& physicalAuthority,
			const MetalResidentEOSParameters& metadata,const bool unsealedParent,
			const bool mismatchedParent,const bool cpuSubstitution,const bool shortSurface,
			ResidentEOSCandidateMetalAuthority& authority,std::string* error )
		{
			if(unsealedParent){
				if(error)*error="production resident EOS candidate refuses an unsealed parent flux";
				return false;
			}
			if(cpuSubstitution||forbiddenCPUCandidate){
				if(error)*error="production resident EOS candidate refuses CPU substitution";
				return false;
			}
			if(!command||!sourceDelta||!enthalpyBounds||!affine||!fctParameters||!transportParameters||
				!eosThermochemistry||!eosParameters||!failure||!obligations||metadata.cells==0u||
				command!=physicalAuthority.parentCommand||command!=transportAuthority.parentCommand||
				physicalAuthority.parentTransportPublicationIdentity!=
					transportAuthority.publicationIdentity||
				physicalAuthority.parentState!=transportAuthority.parentState||
				physicalAuthority.parentThermochemistry!=transportAuthority.parentThermochemistry||
				physicalAuthority.parentCells!=transportAuthority.parentCells||
				transportParameters!=transportAuthority.parentParameters||
				metadata.cells!=transportAuthority.parentCells){
				if(error)*error="production resident EOS candidate parent lineage is stale";
				return false;
			}
			const std::array<id<MTLBuffer>,16> inputs={{sourceDelta,enthalpyBounds,affine,fctParameters,
				transportParameters,eosThermochemistry,eosParameters,failure,obligations,
				physicalAuthority.lowComposite,physicalAuthority.advectiveDelta,
				physicalAuthority.highComposite,physicalAuthority.publicationIdentity,
				transportAuthority.parentState,transportAuthority.publicationIdentity,
				transportAuthority.parentThermochemistry}};
			for(id<MTLBuffer> buffer:inputs)if(!buffer||[buffer device]!=context.device||
				[buffer storageMode]!=MTLStorageModePrivate){
				if(error)*error="production resident EOS candidate requires device-private inputs";
				return false;
			}
			if([transportAuthority.parentState length]!=9u*metadata.cells*sizeof(float)||
				[sourceDelta length]!=9u*metadata.cells*sizeof(float)||
				[enthalpyBounds length]!=14u*sizeof(float)||
				[fctParameters length]!=sizeof(MetalSingleStageFCTParameters)||
				[eosThermochemistry length]!=MetalEOSThermochemistryValues*sizeof(float)||
				[physicalAuthority.lowComposite length]!=9u*physicalAuthority.parentAllFaces*sizeof(float)||
				[physicalAuthority.advectiveDelta length]!=9u*physicalAuthority.parentAllFaces*sizeof(float)||
				[physicalAuthority.highComposite length]!=9u*physicalAuthority.parentAllFaces*sizeof(float)||
				[transportParameters length]!=sizeof(MetalResidentTransportParameters)||
				[eosParameters length]!=sizeof(MetalResidentEOSParameters)||
				[failure length]!=sizeof(std::uint32_t)||
				[obligations length]!=sizeof(std::uint32_t)||
				[physicalAuthority.publicationIdentity length]!=sizeof(std::uint64_t)||
				[transportAuthority.publicationIdentity length]!=sizeof(std::uint64_t)){
				if(error)*error="production resident EOS candidate extent is invalid";return false;}
			SingleStageFCTMetalContext& fct=SingleStageFCTContext();
			if(!fct.Valid()||fct.device!=context.device){
				if(error)*error="production resident EOS FCT producer device identity is stale";
				return false;
			}
			const std::size_t stateBytes=9u*metadata.cells*sizeof(float);
			authority.conservative=[context.device newBufferWithLength:shortSurface?
				stateBytes-sizeof(float):stateBytes options:MTLResourceStorageModePrivate];
			authority.sourceDelta=sourceDelta;
			authority.faceAlpha=[context.device newBufferWithLength:
				physicalAuthority.parentAllFaces*sizeof(float) options:MTLResourceStorageModePrivate];
			id<MTLBuffer> lowState=[context.device newBufferWithLength:stateBytes
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> ratio=[context.device newBufferWithLength:11u*metadata.cells*sizeof(float)
				options:MTLResourceStorageModePrivate];
			authority.producerIdentity=[context.device newBufferWithLength:sizeof(std::uint64_t)
				options:MTLResourceStorageModePrivate];
			authority.publicationIdentity=[context.device newBufferWithLength:sizeof(std::uint64_t)
				options:MTLResourceStorageModePrivate];
			if(!authority.conservative||!authority.faceAlpha||!lowState||!ratio||
				!authority.producerIdentity||!authority.publicationIdentity){
				if(error)*error="production resident EOS candidate allocation failed";
				return false;
			}
			if([authority.conservative length]!=stateBytes){
				if(error)*error="production resident EOS candidate extent is invalid";
				return false;
			}
			authority.parentCommand=command;
			authority.parentPhysicalPublicationIdentity=mismatchedParent?
				transportAuthority.publicationIdentity:physicalAuthority.publicationIdentity;
			authority.parentTransportPublicationIdentity=transportAuthority.publicationIdentity;
			authority.parentEOSThermochemistry=eosThermochemistry;
			authority.parentEOSParameters=eosParameters;
			authority.parentFCTParameters=fctParameters;
			authority.cells=metadata.cells;authority.allocationBytes=
				[authority.conservative allocatedSize]+[authority.faceAlpha allocatedSize]+
				[lowState allocatedSize]+[ratio allocatedSize]+
				[authority.producerIdentity allocatedSize]+[authority.publicationIdentity allocatedSize];
			auto encoderFor=[&](id<MTLComputePipelineState> pipeline){
				id<MTLComputeCommandEncoder> value=ProducerProfileEncoder(command);
				if(value)[value setComputePipelineState:pipeline];return value;};
			id<MTLComputeCommandEncoder> encoder=encoderFor(fct.buildRatios);if(!encoder){
				if(error)*error="production resident EOS candidate ratio encoder failed";
				return false;
			}
			[encoder setBuffer:transportAuthority.parentState offset:0 atIndex:0];
			[encoder setBuffer:sourceDelta offset:0 atIndex:1];
			[encoder setBuffer:physicalAuthority.lowComposite offset:0 atIndex:2];
			[encoder setBuffer:physicalAuthority.advectiveDelta offset:0 atIndex:3];
			[encoder setBuffer:enthalpyBounds offset:0 atIndex:4];[encoder setBuffer:lowState offset:0 atIndex:5];
			[encoder setBuffer:ratio offset:0 atIndex:6];[encoder setBuffer:failure offset:0 atIndex:7];
			[encoder setBuffer:fctParameters offset:0 atIndex:8];
			Dispatch(encoder,fct.buildRatios,11u*metadata.cells);[encoder endEncoding];
			encoder=encoderFor(fct.buildFaceAlpha);if(!encoder){
				if(error)*error="production resident EOS candidate alpha encoder failed";
				return false;
			}
			[encoder setBuffer:physicalAuthority.advectiveDelta offset:0 atIndex:0];
			[encoder setBuffer:ratio offset:0 atIndex:1];
			[encoder setBuffer:enthalpyBounds offset:0 atIndex:2];[encoder setBuffer:authority.faceAlpha offset:0 atIndex:3];
			[encoder setBuffer:failure offset:0 atIndex:4];[encoder setBuffer:fctParameters offset:0 atIndex:5];
			Dispatch(encoder,fct.buildFaceAlpha,physicalAuthority.parentAllFaces);[encoder endEncoding];
			encoder=encoderFor(fct.commitScalar);if(!encoder){
				if(error)*error="production resident EOS candidate commit encoder failed";
				return false;
			}
			[encoder setBuffer:lowState offset:0 atIndex:0];
			[encoder setBuffer:physicalAuthority.advectiveDelta offset:0 atIndex:1];
			[encoder setBuffer:authority.faceAlpha offset:0 atIndex:2];[encoder setBuffer:enthalpyBounds offset:0 atIndex:3];
			[encoder setBuffer:affine offset:0 atIndex:4];[encoder setBuffer:authority.conservative offset:0 atIndex:5];
			[encoder setBuffer:failure offset:0 atIndex:6];[encoder setBuffer:fctParameters offset:0 atIndex:7];
			Dispatch(encoder,fct.commitScalar,metadata.cells);[encoder endEncoding];
			encoder=ProducerProfileEncoder(command);
			if(!encoder){
				if(error)*error="production resident EOS candidate identity encoder failed";
				return false;
			}
			[encoder setComputePipelineState:context.identifyEOSCandidate];
			[encoder setBuffer:authority.conservative offset:0 atIndex:0];
			[encoder setBuffer:sourceDelta offset:0 atIndex:1];
			[encoder setBuffer:authority.faceAlpha offset:0 atIndex:2];
			[encoder setBuffer:authority.parentPhysicalPublicationIdentity offset:0 atIndex:3];
			[encoder setBuffer:authority.parentTransportPublicationIdentity offset:0 atIndex:4];
			[encoder setBuffer:authority.producerIdentity offset:0 atIndex:5];
			[encoder setBuffer:failure offset:0 atIndex:6];
			[encoder setBuffer:obligations offset:0 atIndex:7];
			[encoder setBuffer:eosParameters offset:0 atIndex:8];
			[encoder setBuffer:transportParameters offset:0 atIndex:9];
			[encoder setBuffer:fctParameters offset:0 atIndex:10];
			Dispatch(encoder,context.identifyEOSCandidate,1u);[encoder endEncoding];return true;
		}

		bool EncodeResidentEOSAuthority(
			ResidentTransportMetalContext& context,id<MTLCommandBuffer> command,
			id<MTLBuffer> thermochemistry,id<MTLBuffer> eosThermochemistry,
			id<MTLBuffer> eosParameters,
			id<MTLBuffer> failure,id<MTLBuffer> obligations,
			const ResidentTransportMetalAuthority& transportAuthority,
			const ResidentPhysicalFluxMetalAuthority& physicalAuthority,
			const ResidentEOSCandidateMetalAuthority& candidateAuthority,
			const MetalResidentEOSParameters& metadata,ResidentEOSMetalAuthority& authority,
			std::string* error )
		{
			if(!command||!thermochemistry||!eosThermochemistry||!eosParameters||!failure||!obligations||
				candidateAuthority.parentCommand!=command||physicalAuthority.parentCommand!=command||
				transportAuthority.parentCommand!=command||
				candidateAuthority.parentPhysicalPublicationIdentity!=physicalAuthority.publicationIdentity||
				candidateAuthority.parentTransportPublicationIdentity!=
					transportAuthority.publicationIdentity||
				physicalAuthority.parentTransportPublicationIdentity!=
					transportAuthority.publicationIdentity||
				candidateAuthority.parentEOSThermochemistry!=eosThermochemistry||
				candidateAuthority.parentEOSParameters!=eosParameters||
				!candidateAuthority.parentFCTParameters||
				candidateAuthority.cells!=metadata.cells||metadata.cells!=transportAuthority.parentCells){
				if(error)*error="production resident EOS authority lineage is stale";return false;}
			const std::array<id<MTLBuffer>,12> inputs={{thermochemistry,eosThermochemistry,eosParameters,
				failure,obligations,
				candidateAuthority.conservative,candidateAuthority.producerIdentity,
				candidateAuthority.publicationIdentity,
				physicalAuthority.publicationIdentity,transportAuthority.publicationIdentity,
				transportAuthority.parentObligations,candidateAuthority.parentFCTParameters}};
			for(id<MTLBuffer> buffer:inputs)if(!buffer||[buffer device]!=context.device||
				[buffer storageMode]!=MTLStorageModePrivate){
				if(error)*error="production resident EOS authority requires device-private lineage";
				return false;
			}
			if([thermochemistry length]!=MetalManifoldCertificateValues*sizeof(float)||
				[eosThermochemistry length]!=MetalEOSThermochemistryValues*sizeof(float)||
				[eosParameters length]!=sizeof(MetalResidentEOSParameters)||
				[failure length]!=sizeof(std::uint32_t)||
				[obligations length]!=sizeof(std::uint32_t)||
				[candidateAuthority.conservative length]!=9u*metadata.cells*sizeof(float)||
				[candidateAuthority.producerIdentity length]!=sizeof(std::uint64_t)||
				[candidateAuthority.publicationIdentity length]!=sizeof(std::uint64_t)){
				if(error)*error="production resident EOS authority extent is invalid";return false;}
			auto make=[&](const std::size_t bytes){return [context.device newBufferWithLength:bytes
				options:MTLResourceStorageModePrivate];};const std::size_t fieldBytes=metadata.cells*sizeof(float);
			authority.temperature=make(fieldBytes);authority.representedPressureRatio=make(fieldBytes);
			authority.absoluteDeviation=make(fieldBytes);authority.publicationIdentity=make(sizeof(std::uint64_t));
			authority.firstFailureCell=make(sizeof(std::uint32_t));
			authority.failureTerm=make(fieldBytes);
			authority.parentCommand=command;
			authority.parentTransportPublicationIdentity=transportAuthority.publicationIdentity;
			authority.parentPhysicalPublicationIdentity=physicalAuthority.publicationIdentity;
			authority.parentCandidatePublicationIdentity=candidateAuthority.publicationIdentity;
			const std::array<id<MTLBuffer>,6> outputs={{authority.temperature,
				authority.representedPressureRatio,authority.absoluteDeviation,
				authority.publicationIdentity,authority.firstFailureCell,authority.failureTerm}};
			for(id<MTLBuffer> buffer:outputs)if(!buffer){
				if(error)*error="production resident EOS authority allocation failed";
				return false;
			}
			authority.allocationBytes=0u;for(id<MTLBuffer> buffer:outputs)
				authority.allocationBytes+=[buffer allocatedSize];
			id<MTLBlitCommandEncoder> clear=[command blitCommandEncoder];
			if(!clear){if(error)*error="production resident EOS witness clear encoder failed";return false;}
			[clear fillBuffer:authority.firstFailureCell range:NSMakeRange(0,sizeof(std::uint32_t)) value:0xffu];
			[clear fillBuffer:authority.failureTerm range:NSMakeRange(0,fieldBytes) value:0u];
			[clear endEncoding];
			id<MTLComputeCommandEncoder> encoder=ProducerProfileEncoder(command);
			if(!encoder){
				if(error)*error="production resident EOS evaluation encoder failed";
				return false;
			}
			[encoder setComputePipelineState:context.evaluateEOSCandidate];
			[encoder setBuffer:candidateAuthority.conservative offset:0 atIndex:0];
			[encoder setBuffer:thermochemistry offset:0 atIndex:1];
			[encoder setBuffer:eosThermochemistry offset:0 atIndex:2];
			[encoder setBuffer:candidateAuthority.producerIdentity offset:0 atIndex:3];
			[encoder setBuffer:authority.temperature offset:0 atIndex:4];
			[encoder setBuffer:authority.representedPressureRatio offset:0 atIndex:5];
			[encoder setBuffer:authority.absoluteDeviation offset:0 atIndex:6];
			[encoder setBuffer:failure offset:0 atIndex:7];
			[encoder setBuffer:obligations offset:0 atIndex:8];
			[encoder setBuffer:eosParameters offset:0 atIndex:9];
			[encoder setBuffer:authority.firstFailureCell offset:0 atIndex:10];
			[encoder setBuffer:authority.failureTerm offset:0 atIndex:11];
			Dispatch(encoder,context.evaluateEOSCandidate,metadata.cells);[encoder endEncoding];
			encoder=ProducerProfileEncoder(command);
			if(!encoder){
				if(error)*error="production resident EOS candidate finalization encoder failed";
				return false;
			}
			[encoder setComputePipelineState:context.finalizeEOSCandidate];
			[encoder setBuffer:candidateAuthority.producerIdentity offset:0 atIndex:0];
			[encoder setBuffer:candidateAuthority.publicationIdentity offset:0 atIndex:1];
			[encoder setBuffer:failure offset:0 atIndex:2];
			Dispatch(encoder,context.finalizeEOSCandidate,1u);[encoder endEncoding];
			encoder=ProducerProfileEncoder(command);
			if(!encoder){
				if(error)*error="production resident EOS publication encoder failed";
				return false;
			}
			[encoder setComputePipelineState:context.identifyEOS];
			[encoder setBuffer:authority.temperature offset:0 atIndex:0];
			[encoder setBuffer:authority.representedPressureRatio offset:0 atIndex:1];
			[encoder setBuffer:authority.absoluteDeviation offset:0 atIndex:2];
			[encoder setBuffer:candidateAuthority.publicationIdentity offset:0 atIndex:3];
			[encoder setBuffer:physicalAuthority.publicationIdentity offset:0 atIndex:4];
			[encoder setBuffer:transportAuthority.publicationIdentity offset:0 atIndex:5];
			[encoder setBuffer:eosThermochemistry offset:0 atIndex:6];
			[encoder setBuffer:authority.publicationIdentity offset:0 atIndex:7];
			[encoder setBuffer:failure offset:0 atIndex:8];
			[encoder setBuffer:eosParameters offset:0 atIndex:9];
			Dispatch(encoder,context.identifyEOS,1u);[encoder endEncoding];return true;
		}

		bool EncodeResidentFrozenSourceAuthority(
			ResidentTransportMetalContext& context,id<MTLCommandBuffer> command,
			id<MTLBuffer> failure,id<MTLBuffer> obligations,
			const FireProductionFrozenSourcePacketSeal& source,
			const ResidentEOSCandidateMetalAuthority& candidateAuthority,
			const MetalResidentTargetParameters& metadata,
			id<MTLBuffer> residentValues,id<MTLBuffer> residentParameters,
			ResidentFrozenSourceMetalAuthority& authority,std::string* error )
		{
			const FireProductionProjectionShape& sourceShape=source.Shape();
			if(!command||!failure||!obligations||metadata.cells==0u||
				metadata.attemptIdentity==0u||metadata.sourcePacketIdentity==0u){
				if(error)*error="production resident frozen source authority input is absent";return false;}
			if(!FireProductionFrozenSourcePacketSealMatches(source,error))return false;
			if(sourceShape.nx!=metadata.nx||sourceShape.ny!=metadata.ny||
				sourceShape.nz!=metadata.nz||sourceShape.cellWidthM!=metadata.cellWidthM){
				if(error)*error="production resident frozen source shape metadata differs";return false;}
			if(source.TimeStepS()!=metadata.timeStepS){
				if(error)*error="production resident frozen source timestep metadata differs";return false;}
			if(source.AttemptIdentity()!=metadata.attemptIdentity){
				if(error)*error="production resident frozen source attempt metadata differs";return false;}
			if(source.PacketIdentity()!=metadata.sourcePacketIdentity){
				if(error)*error="production resident frozen source packet metadata differs";return false;}
			if(source.DivergenceTargetPerS().size()!=metadata.cells||
				source.SourceDelta().size()!=9u*metadata.cells){
				if(error)*error="production resident frozen source extent metadata differs";return false;}
			if(candidateAuthority.parentCommand!=command||candidateAuthority.cells!=metadata.cells||
				candidateAuthority.sourceDelta==nil||candidateAuthority.publicationIdentity==nil){
				if(error)*error="production resident frozen source candidate parent differs";return false;}
			const std::array<id<MTLBuffer>,4> inputs={{candidateAuthority.sourceDelta,
				candidateAuthority.publicationIdentity,failure,obligations}};
			for(id<MTLBuffer> buffer:inputs)if([buffer device]!=context.device||
				[buffer storageMode]!=MTLStorageModePrivate){
				if(error)*error="production resident frozen source requires a resident candidate parent";
				return false;
			}
			const std::size_t fieldBytes=metadata.cells*sizeof(float);
			if([candidateAuthority.sourceDelta length]!=9u*fieldBytes||
				[candidateAuthority.publicationIdentity length]!=sizeof(std::uint64_t)||
				[failure length]!=sizeof(std::uint32_t)||[obligations length]!=sizeof(std::uint32_t)){
				if(error)*error="production resident frozen source extent is invalid";return false;}
			const bool residentInputs=residentValues||residentParameters;
			if(residentInputs&&(!residentValues||!residentParameters||
				[residentValues device]!=context.device||[residentParameters device]!=context.device||
				[residentValues storageMode]!=MTLStorageModePrivate||
				[residentParameters storageMode]!=MTLStorageModePrivate||
				[residentValues length]!=fieldBytes||[residentParameters length]!=sizeof(metadata))){
				if(error)*error="production resident frozen source setup authority is invalid";
				return false;
			}
			authority.inputUpload=residentInputs?residentValues:
				[context.device newBufferWithBytes:source.DivergenceTargetPerS().data()
					length:fieldBytes options:MTLResourceStorageModeShared];
			authority.values=[context.device newBufferWithLength:fieldBytes
				options:MTLResourceStorageModePrivate];
			authority.metadata=[context.device newBufferWithLength:
				sizeof(MetalResidentFrozenSourceParameters) options:MTLResourceStorageModePrivate];
			authority.publicationIdentity=[context.device newBufferWithLength:sizeof(std::uint64_t)
				options:MTLResourceStorageModePrivate];
			if(!authority.inputUpload||!authority.values||!authority.metadata||
				!authority.publicationIdentity){
				if(error)*error="production resident frozen source allocation failed";return false;}
			authority.issuedValues=authority.values;authority.issuedMetadata=authority.metadata;
			authority.parentCommand=command;
			authority.parentCandidatePublicationIdentity=candidateAuthority.publicationIdentity;
			authority.cells=metadata.cells;
			authority.parameterUpload=residentInputs?residentParameters:
				[context.device newBufferWithBytes:&metadata length:sizeof(metadata)
					options:MTLResourceStorageModeShared];
			if(!authority.parameterUpload){
				if(error)*error="production resident frozen source parameter allocation failed";
				return false;}
			authority.allocationBytes=(residentInputs?0u:[authority.inputUpload allocatedSize])+
				(residentInputs?0u:[authority.parameterUpload allocatedSize])+
				[authority.values allocatedSize]+[authority.metadata allocatedSize]+
				[authority.publicationIdentity allocatedSize];
			authority.liveAllocationBytes=[authority.values allocatedSize]+
				[authority.metadata allocatedSize]+[authority.publicationIdentity allocatedSize];
			id<MTLComputeCommandEncoder> encoder=ProducerProfileEncoder(command);
			if(!encoder){if(error)*error="production resident frozen source encoder failed";return false;}
			[encoder setComputePipelineState:context.produceFrozenSource];
			[encoder setBuffer:authority.inputUpload offset:0 atIndex:0];
			[encoder setBuffer:authority.values offset:0 atIndex:1];
			[encoder setBuffer:authority.metadata offset:0 atIndex:2];
			[encoder setBuffer:failure offset:0 atIndex:3];
			[encoder setBuffer:authority.parameterUpload offset:0 atIndex:4];
			[encoder setBuffer:candidateAuthority.publicationIdentity offset:0 atIndex:5];
			Dispatch(encoder,context.produceFrozenSource,metadata.cells);[encoder endEncoding];
			encoder=ProducerProfileEncoder(command);
			if(!encoder){if(error)*error="production resident frozen source identity encoder failed";
				return false;}
			[encoder setComputePipelineState:context.identifyFrozenSource];
			[encoder setBuffer:authority.values offset:0 atIndex:0];
			[encoder setBuffer:authority.metadata offset:0 atIndex:1];
			[encoder setBuffer:authority.publicationIdentity offset:0 atIndex:2];
			[encoder setBuffer:failure offset:0 atIndex:3];
			[encoder setBuffer:candidateAuthority.publicationIdentity offset:0 atIndex:4];
			Dispatch(encoder,context.identifyFrozenSource,1u);[encoder endEncoding];return true;
		}

		bool EncodeResidentProjectionTargetAuthority(
			ResidentTransportMetalContext& context,id<MTLCommandBuffer> command,
			id<MTLBuffer> eosThermochemistry,
			id<MTLBuffer> targetParameters,id<MTLBuffer> projectionParameters,
			id<MTLBuffer> qualificationCPUTarget,id<MTLBuffer> qualificationCPUMetadata,
			id<MTLBuffer> failure,id<MTLBuffer> obligations,
			const ResidentTransportMetalAuthority& transportAuthority,
			const ResidentPhysicalFluxMetalAuthority& tangentPhysicalAuthority,
			const ResidentPhysicalFluxMetalAuthority& candidatePhysicalAuthority,
			const ResidentEOSCandidateMetalAuthority& candidateAuthority,
			const ResidentEOSMetalAuthority& eosAuthority,
			const ResidentFrozenSourceMetalAuthority& sourceAuthority,
			const MetalResidentTargetParameters& metadata,
			ResidentProjectionTargetMetalAuthority& authority,std::string* error )
		{
			if(!command||!eosThermochemistry||!targetParameters||
				!projectionParameters||!failure||
				!obligations||metadata.cells==0u||metadata.sourcePacketIdentity==0u||
				transportAuthority.parentCommand!=command||
				tangentPhysicalAuthority.parentCommand!=command||
				candidatePhysicalAuthority.parentCommand!=command||
				candidateAuthority.parentCommand!=command||eosAuthority.parentCommand!=command||
				sourceAuthority.parentCommand!=command||
				tangentPhysicalAuthority.parentTransportPublicationIdentity!=
					transportAuthority.publicationIdentity||
				candidatePhysicalAuthority.parentTransportPublicationIdentity!=
					transportAuthority.publicationIdentity||
				candidateAuthority.parentTransportPublicationIdentity!=
					transportAuthority.publicationIdentity||
				candidateAuthority.parentPhysicalPublicationIdentity!=
					candidatePhysicalAuthority.publicationIdentity||
				eosAuthority.parentTransportPublicationIdentity!=transportAuthority.publicationIdentity||
				eosAuthority.parentPhysicalPublicationIdentity!=
					candidatePhysicalAuthority.publicationIdentity||
				eosAuthority.parentCandidatePublicationIdentity!=candidateAuthority.publicationIdentity||
				sourceAuthority.parentCandidatePublicationIdentity!=candidateAuthority.publicationIdentity||
				eosThermochemistry!=candidateAuthority.parentEOSThermochemistry||
				candidateAuthority.cells!=metadata.cells||transportAuthority.parentCells!=metadata.cells||
				tangentPhysicalAuthority.parentCells!=metadata.cells||
				candidatePhysicalAuthority.parentCells!=metadata.cells||
				sourceAuthority.cells!=metadata.cells||
				sourceAuthority.values!=sourceAuthority.issuedValues||
				sourceAuthority.metadata!=sourceAuthority.issuedMetadata){
				if(error)*error="production resident target parent lineage is not immediate";return false;}
			const std::array<id<MTLBuffer>,24> inputs={{eosThermochemistry,
				targetParameters,projectionParameters,failure,obligations,transportAuthority.parentState,
				transportAuthority.parentTemperature,transportAuthority.publicationIdentity,
				tangentPhysicalAuthority.physicalMass,tangentPhysicalAuthority.physicalEnergy,
				tangentPhysicalAuthority.publicationIdentity,
				candidatePhysicalAuthority.publicationIdentity,candidateAuthority.conservative,
				candidateAuthority.publicationIdentity,eosAuthority.temperature,
				eosAuthority.representedPressureRatio,eosAuthority.absoluteDeviation,
				eosAuthority.publicationIdentity,sourceAuthority.values,sourceAuthority.metadata,
				sourceAuthority.publicationIdentity,transportAuthority.parentParameters,
				candidateAuthority.parentFCTParameters,candidateAuthority.parentEOSParameters}};
			for(id<MTLBuffer> buffer:inputs)if(!buffer||[buffer device]!=context.device||
				[buffer storageMode]!=MTLStorageModePrivate){
				if(error)*error="production resident target requires device-private authority";return false;}
			const std::size_t cells=metadata.cells,fieldBytes=cells*sizeof(float);
			if([eosThermochemistry length]!=MetalEOSThermochemistryValues*sizeof(float)||
				[targetParameters length]!=sizeof(MetalResidentTargetParameters)||
				[projectionParameters length]!=sizeof(MetalResidentProjectionConsumerParameters)||
				[failure length]!=sizeof(std::uint32_t)||[obligations length]!=sizeof(std::uint32_t)||
				[transportAuthority.parentState length]!=9u*fieldBytes||
				[transportAuthority.parentTemperature length]!=fieldBytes||
				[tangentPhysicalAuthority.physicalMass length]!=
					8u*tangentPhysicalAuthority.parentAllFaces*sizeof(float)||
				[tangentPhysicalAuthority.physicalEnergy length]!=
					tangentPhysicalAuthority.parentAllFaces*sizeof(float)||
				[candidateAuthority.conservative length]!=9u*fieldBytes||
				[eosAuthority.temperature length]!=fieldBytes||
				[eosAuthority.representedPressureRatio length]!=fieldBytes||
				[eosAuthority.absoluteDeviation length]!=fieldBytes||
				[sourceAuthority.values length]!=fieldBytes||
				[sourceAuthority.metadata length]!=sizeof(MetalResidentFrozenSourceParameters)||
				[sourceAuthority.publicationIdentity length]!=sizeof(std::uint64_t)||
				[transportAuthority.parentParameters length]!=sizeof(MetalResidentTransportParameters)||
				[candidateAuthority.parentFCTParameters length]!=sizeof(MetalSingleStageFCTParameters)||
				[candidateAuthority.parentEOSParameters length]!=sizeof(MetalResidentEOSParameters)){
				if(error)*error="production resident target extent is invalid";return false;}
			auto make=[&](const std::size_t bytes){return [context.device newBufferWithLength:bytes
				options:MTLResourceStorageModePrivate];};
			authority.tangent=make(fieldBytes);authority.frozenSource=make(fieldBytes);
			authority.absoluteDiagnostic=make(fieldBytes);authority.monitoredAbsolute=make(fieldBytes);
			authority.baseAssembled=make(fieldBytes);authority.assembled=make(fieldBytes);
			authority.tangentEnclosure=make(fieldBytes);authority.assembledEnclosure=make(fieldBytes);
			authority.publicationIdentity=make(sizeof(std::uint64_t));
			ResidentProjectionMetadataMetalAuthority projectionAuthority;
			projectionAuthority.metadata=make(sizeof(MetalResidentProjectionConsumerParameters));
			projectionAuthority.publicationIdentity=make(sizeof(std::uint64_t));
			authority.projectionConsumerIdentity=projectionAuthority.publicationIdentity;
			authority.projectionMetadata=projectionAuthority.metadata;
			authority.consumerIdentity=make(sizeof(std::uint64_t));
			const std::array<id<MTLBuffer>,12> outputs={{authority.tangent,authority.frozenSource,
				authority.absoluteDiagnostic,authority.monitoredAbsolute,authority.baseAssembled,authority.assembled,
				authority.tangentEnclosure,authority.assembledEnclosure,
				authority.publicationIdentity,projectionAuthority.metadata,
				authority.projectionConsumerIdentity,
				authority.consumerIdentity}};
			for(id<MTLBuffer> buffer:outputs)if(!buffer){
				if(error)*error="production resident target allocation failed";return false;}
			authority.parentCommand=command;
			authority.parentTransportPublicationIdentity=transportAuthority.publicationIdentity;
			authority.parentTangentPhysicalPublicationIdentity=
				tangentPhysicalAuthority.publicationIdentity;
			authority.parentPhysicalPublicationIdentity=candidatePhysicalAuthority.publicationIdentity;
			authority.parentCandidatePublicationIdentity=candidateAuthority.publicationIdentity;
			authority.parentEOSPublicationIdentity=eosAuthority.publicationIdentity;
			authority.parentFrozenSourcePublicationIdentity=sourceAuthority.publicationIdentity;
			projectionAuthority.parentCommand=command;
			projectionAuthority.parentTargetPublicationIdentity=authority.publicationIdentity;
			authority.cells=cells;authority.allocationBytes=0u;
			for(id<MTLBuffer> buffer:outputs)authority.allocationBytes+=[buffer allocatedSize];
			auto encoderFor=[&](id<MTLComputePipelineState> pipeline){
				id<MTLComputeCommandEncoder> value=ProducerProfileEncoder(command);
				if(value)[value setComputePipelineState:pipeline];return value;};
			id<MTLComputeCommandEncoder> encoder=encoderFor(context.evaluateTargetTerms);
			if(!encoder){if(error)*error="production resident target term encoder failed";return false;}
			[encoder setBuffer:transportAuthority.parentState offset:0 atIndex:0];
			[encoder setBuffer:transportAuthority.parentTemperature offset:0 atIndex:1];
			[encoder setBuffer:tangentPhysicalAuthority.physicalMass offset:0 atIndex:2];
			[encoder setBuffer:tangentPhysicalAuthority.physicalEnergy offset:0 atIndex:3];
			[encoder setBuffer:eosThermochemistry offset:0 atIndex:4];
			[encoder setBuffer:sourceAuthority.values offset:0 atIndex:5];
			[encoder setBuffer:eosAuthority.representedPressureRatio offset:0 atIndex:6];
			[encoder setBuffer:eosAuthority.absoluteDeviation offset:0 atIndex:7];
			[encoder setBuffer:transportAuthority.publicationIdentity offset:0 atIndex:8];
			[encoder setBuffer:tangentPhysicalAuthority.publicationIdentity offset:0 atIndex:9];
			[encoder setBuffer:candidateAuthority.publicationIdentity offset:0 atIndex:10];
			[encoder setBuffer:eosAuthority.publicationIdentity offset:0 atIndex:11];
			[encoder setBuffer:authority.tangent offset:0 atIndex:12];
			[encoder setBuffer:authority.frozenSource offset:0 atIndex:13];
			[encoder setBuffer:authority.absoluteDiagnostic offset:0 atIndex:14];
			[encoder setBuffer:authority.monitoredAbsolute offset:0 atIndex:15];
			[encoder setBuffer:authority.assembled offset:0 atIndex:16];
			[encoder setBuffer:failure offset:0 atIndex:17];[encoder setBuffer:obligations offset:0 atIndex:18];
			[encoder setBuffer:targetParameters offset:0 atIndex:19];
			[encoder setBuffer:sourceAuthority.publicationIdentity offset:0 atIndex:20];
			[encoder setBuffer:sourceAuthority.metadata offset:0 atIndex:21];
			[encoder setBuffer:transportAuthority.parentParameters offset:0 atIndex:22];
			[encoder setBuffer:candidateAuthority.parentFCTParameters offset:0 atIndex:23];
			[encoder setBuffer:candidateAuthority.parentEOSParameters offset:0 atIndex:24];
			[encoder setBuffer:candidatePhysicalAuthority.publicationIdentity offset:0 atIndex:25];
			[encoder setBuffer:authority.baseAssembled offset:0 atIndex:26];
			[encoder setBuffer:authority.tangentEnclosure offset:0 atIndex:27];
			[encoder setBuffer:authority.assembledEnclosure offset:0 atIndex:28];
			Dispatch(encoder,context.evaluateTargetTerms,cells);[encoder endEncoding];
			encoder=encoderFor(context.finalizeTarget);if(!encoder){
				if(error)*error="production resident target compatibility encoder failed";return false;}
			[encoder setBuffer:authority.assembled offset:0 atIndex:0];[encoder setBuffer:failure offset:0 atIndex:1];
			[encoder setBuffer:obligations offset:0 atIndex:2];[encoder setBuffer:targetParameters offset:0 atIndex:3];
			[encoder setBuffer:authority.assembledEnclosure offset:0 atIndex:4];
			Dispatch(encoder,context.finalizeTarget,1u);[encoder endEncoding];
			encoder=encoderFor(context.identifyTarget);if(!encoder){
				if(error)*error="production resident target identity encoder failed";return false;}
			[encoder setBuffer:authority.tangent offset:0 atIndex:0];
			[encoder setBuffer:authority.frozenSource offset:0 atIndex:1];
			[encoder setBuffer:authority.absoluteDiagnostic offset:0 atIndex:2];
			[encoder setBuffer:authority.monitoredAbsolute offset:0 atIndex:3];
			[encoder setBuffer:authority.assembled offset:0 atIndex:4];
			[encoder setBuffer:transportAuthority.publicationIdentity offset:0 atIndex:5];
			[encoder setBuffer:tangentPhysicalAuthority.publicationIdentity offset:0 atIndex:6];
			[encoder setBuffer:candidateAuthority.publicationIdentity offset:0 atIndex:7];
			[encoder setBuffer:eosAuthority.publicationIdentity offset:0 atIndex:8];
			[encoder setBuffer:authority.publicationIdentity offset:0 atIndex:9];
			[encoder setBuffer:failure offset:0 atIndex:10];
			[encoder setBuffer:sourceAuthority.publicationIdentity offset:0 atIndex:11];
			[encoder setBuffer:targetParameters offset:0 atIndex:12];
			[encoder setBuffer:candidatePhysicalAuthority.publicationIdentity offset:0 atIndex:13];
			[encoder setBuffer:authority.tangentEnclosure offset:0 atIndex:14];
			[encoder setBuffer:authority.assembledEnclosure offset:0 atIndex:15];
			Dispatch(encoder,context.identifyTarget,1u);[encoder endEncoding];
			encoder=encoderFor(context.identifyProjectionConsumer);if(!encoder){
				if(error)*error="production resident projection-consumer identity encoder failed";return false;}
			[encoder setBuffer:projectionParameters offset:0 atIndex:0];
			[encoder setBuffer:authority.publicationIdentity offset:0 atIndex:1];
			[encoder setBuffer:projectionAuthority.metadata offset:0 atIndex:2];
			[encoder setBuffer:projectionAuthority.publicationIdentity offset:0 atIndex:3];
			[encoder setBuffer:failure offset:0 atIndex:4];
			Dispatch(encoder,context.identifyProjectionConsumer,1u);[encoder endEncoding];
			if(qualificationCPUTarget)authority.assembled=qualificationCPUTarget;
			if(qualificationCPUMetadata)projectionAuthority.metadata=qualificationCPUMetadata;
			if(authority.assembled!=outputs[5]||[authority.assembled storageMode]!=MTLStorageModePrivate){
				if(error)*error="production resident target consumer refuses CPU-produced target";
				return false;
			}
			if(projectionAuthority.parentCommand!=command||
				projectionAuthority.parentTargetPublicationIdentity!=authority.publicationIdentity||
				projectionAuthority.metadata!=outputs[9]||
				[projectionAuthority.metadata storageMode]!=MTLStorageModePrivate){
				if(error)*error="production resident target consumer refuses CPU-produced projection metadata";
				return false;
			}
			encoder=encoderFor(context.consumeTarget);if(!encoder){
				if(error)*error="production resident target consumer encoder failed";return false;}
			[encoder setBuffer:authority.assembled offset:0 atIndex:0];
			[encoder setBuffer:authority.publicationIdentity offset:0 atIndex:1];
			[encoder setBuffer:transportAuthority.publicationIdentity offset:0 atIndex:2];
			[encoder setBuffer:candidatePhysicalAuthority.publicationIdentity offset:0 atIndex:3];
			[encoder setBuffer:candidateAuthority.publicationIdentity offset:0 atIndex:4];
			[encoder setBuffer:eosAuthority.publicationIdentity offset:0 atIndex:5];
			[encoder setBuffer:authority.consumerIdentity offset:0 atIndex:6];
			[encoder setBuffer:authority.projectionConsumerIdentity offset:0 atIndex:7];
			[encoder setBuffer:failure offset:0 atIndex:8];
			[encoder setBuffer:targetParameters offset:0 atIndex:9];
			[encoder setBuffer:projectionAuthority.metadata offset:0 atIndex:10];
			Dispatch(encoder,context.consumeTarget,1u);[encoder endEncoding];return true;
		}

		bool PrepareResidentPhysicalFluxRequest(
			const FireProductionResidentPhysicalFluxComparatorRequest& request,
			MetalResidentTransportParameters& transportParameters,
			MetalResidentPhysicalFluxParameters& physicalParameters,
			std::array<std::size_t,3>& faceOffset,std::size_t& allFaces,
			std::vector<unsigned char>& packedFuelInlet,
			std::vector<unsigned char>& packedPressureInflow,
			std::vector<float>& physicalBasis,std::string* error )
		{
			if(!PrepareResidentTransportRequest(request.transport,transportParameters,
				faceOffset,allFaces,packedFuelInlet,error))return false;
			const FireProductionProjectionShape& shape=request.transport.shape;
			const FireSimulationMethaneRecord& record=FireSimulationMethaneRecord::PhysicalV1();
			const FireCertifiedNullspace& physical=record.NonadvectiveFluxProjection();
			if(!record.IsValid()||physical.stateDimension!=8u||physical.nullity==0u||
				physical.nullity>8u||physical.orthonormalBasis.size()!=8u*physical.nullity||
				request.nullity==0u||request.nullity>8u||
				request.nullspaceBasis.size()!=8u*request.nullity||
				request.coordinateProjector.size()!=request.nullity*request.nullity||
				!std::isfinite(request.ambientTemperatureK)||
				request.ambientTemperatureK<record.TemperatureMinK()||
				request.ambientTemperatureK>record.TemperatureMaxK()){
				if(error)*error="production resident physical-flux certificate is invalid";
				return false;
			}
			for(const float value:request.ambient)if(!std::isfinite(value)){
				if(error)*error="production resident physical-flux ambient is nonfinite";
				return false;
			}
			for(const float value:request.nullspaceBasis)if(!std::isfinite(value)){
				if(error)*error="production resident physical-flux basis is nonfinite";
				return false;
			}
			for(const float value:request.coordinateProjector)if(!std::isfinite(value)){
				if(error)*error="production resident physical-flux projector is nonfinite";
				return false;
			}
			packedPressureInflow.clear();
			for(unsigned int side=0u;side<6u;++side){
				const std::size_t expected=side<2u?shape.ny*shape.nz:
					(side<4u?shape.nx*shape.nz:shape.nx*shape.ny);
				if(request.pressureOpenInflow[side].size()!=expected){
					if(error)*error="production resident physical-flux inflow shape is invalid";
					return false;
				}
				for(const unsigned char value:request.pressureOpenInflow[side])if(value>1u||
					(request.transport.boundary[side]!=FireProductionProjectionPressureOpen&&
						value!=0u)){
					if(error)*error="production resident physical-flux inflow class is invalid";
					return false;
				}
				packedPressureInflow.insert(packedPressureInflow.end(),
					request.pressureOpenInflow[side].begin(),
					request.pressureOpenInflow[side].end());
			}
			physicalBasis.resize(physical.orthonormalBasis.size());
			for(std::size_t index=0u;index<physicalBasis.size();++index){
				physicalBasis[index]=static_cast<float>(physical.orthonormalBasis[index]);
				if(!std::isfinite(physicalBasis[index])){
					if(error)*error="production resident physical-flux projection is nonfinite";
					return false;
				}
			}
			physicalParameters={static_cast<std::uint32_t>(request.nullity),
				static_cast<std::uint32_t>(physical.nullity),
				request.qualificationMutateHighNonadvective?1u:0u,
				request.ambientTemperatureK};
			return true;
		}

		bool PrepareResidentEOSCandidateRequest(
			const FireProductionResidentEOSCandidateComparatorRequest& request,
			MetalResidentTransportParameters& transportParameters,
			MetalResidentPhysicalFluxParameters& physicalParameters,
			MetalResidentEOSParameters& eosParameters,
			std::array<std::size_t,3>& faceOffset,std::size_t& allFaces,
			std::vector<unsigned char>& packedFuelInlet,
			std::vector<unsigned char>& packedPressureInflow,
			std::vector<float>& physicalBasis,std::string* error )
		{
			if(!PrepareResidentPhysicalFluxRequest(request.physicalFlux,transportParameters,
				physicalParameters,faceOffset,allFaces,packedFuelInlet,packedPressureInflow,
				physicalBasis,error))return false;
			const FireSimulationMethaneRecord& record=FireSimulationMethaneRecord::PhysicalV1();
			RISE::FireCase::RecordV1 sealedCase;std::string caseError;
			const std::size_t cells=request.physicalFlux.transport.shape.CellCount();
			if(request.producingStage!=FireProductionScalarEOSStage::QStar||
				request.physicalFlux.transport.stage!=FireProductionProjectedHeunStage::R0){
				if(error)*error="production resident EOS Q* requires the sealed R0 producer";
				return false;
			}
			if(request.producerPrecision!=FireStateProducerPrecision::Binary32){
				if(error)*error="production resident EOS candidate precision class is not binary32";
				return false;
			}
			if(request.sourceDelta.size()!=9u*cells||
				!std::all_of(request.sourceDelta.begin(),request.sourceDelta.end(),
					[](const float value){return std::isfinite(value);})||
				!record.IsValid()||!RISE::FireCase::ValidateMethaneEnvelopeV1(
				request.caseRecordEnvelope,record,sealedCase,caseError)||
				sealedCase.authored.fuelRecordId!=record.RecordId()||
				!(request.candidateTimeStepS>0.0f)||!std::isfinite(request.candidateTimeStepS)||
				!std::isfinite(sealedCase.derived.pilotAmbientTemperatureK)||
				!std::isfinite(sealedCase.derived.maximumAcceptedTemperatureK)||
				sealedCase.derived.pilotAmbientTemperatureK<record.TemperatureMinK()||
				sealedCase.derived.maximumAcceptedTemperatureK>record.TemperatureMaxK()||
				sealedCase.derived.pilotAmbientTemperatureK>=
					sealedCase.derived.maximumAcceptedTemperatureK){
				if(error)*error="production resident EOS candidate metadata or payload is invalid";
				return false;
			}
			const FireCertifiedNullspace& affine=record.ConservativeReconstruction();
			if(affine.stateDimension!=8u||affine.constraintRows>8u){
				if(error)*error="production resident EOS affine certificate is invalid";
				return false;
			}
			eosParameters={};eosParameters.cells=static_cast<std::uint32_t>(cells);
			eosParameters.stage=static_cast<std::uint32_t>(request.producingStage);
			eosParameters.precision=static_cast<std::uint32_t>(request.producerPrecision);
			eosParameters.affineRowCount=static_cast<std::uint32_t>(affine.constraintRows);
			eosParameters.temperatureMinK=static_cast<float>(
				sealedCase.derived.pilotAmbientTemperatureK);
			eosParameters.temperatureMaxK=static_cast<float>(
				sealedCase.derived.maximumAcceptedTemperatureK);
			eosParameters.pressurePa=static_cast<float>(record.ThermodynamicPressurePa());
			eosParameters.feasibilityFactor=static_cast<float>(
				record.AcceptedStateFeasibilityEnvelope().kappaEpsilon32*
				static_cast<double>(std::numeric_limits<float>::epsilon()));
			eosParameters.dynamicsValidityBound=0x1p-2f;
			eosParameters.timeStepS=request.candidateTimeStepS;
			eosParameters.attemptIdentity=request.physicalFlux.transport.attemptIdentity;
			eosParameters.caseIdentity=ResidentRecordIdentity(sealedCase.caseRecordId);
			return std::isfinite(eosParameters.temperatureMinK)&&
				std::isfinite(eosParameters.temperatureMaxK)&&
				std::isfinite(eosParameters.pressurePa)&&
				std::isfinite(eosParameters.feasibilityFactor)&&
				eosParameters.temperatureMinK>0.0f&&
				eosParameters.temperatureMaxK>eosParameters.temperatureMinK&&
				eosParameters.pressurePa>0.0f&&eosParameters.feasibilityFactor>0.0f;
		}

		bool EncodeResidentTransportAuthority(
			ResidentTransportMetalContext& context,id<MTLCommandBuffer> command,
			id<MTLBuffer> state,id<MTLBuffer> temperature,id<MTLBuffer> velocity,
			id<MTLBuffer> thermochemistry,id<MTLBuffer> transport,id<MTLBuffer> fuelInlet,
			id<MTLBuffer> parameters,id<MTLBuffer> failure,id<MTLBuffer> obligations,
			const std::size_t cells,const std::size_t allFaces,
			const std::size_t boundaryFaces,
			ResidentTransportMetalAuthority& authority,
			std::string* error )
		{
			if(!command||!state||!temperature||!velocity||!thermochemistry||!transport||!fuelInlet||
				!parameters||!failure||!obligations||cells==0u){
				if(error)*error="production resident transport authority input is absent";
				return false;
			}
			const std::array<id<MTLBuffer>,9> privateInputs={{state,temperature,velocity,
				thermochemistry,transport,fuelInlet,parameters,failure,obligations}};
			for(id<MTLBuffer> buffer:privateInputs)if([buffer device]!=context.device||
				[buffer storageMode]!=MTLStorageModePrivate){if(error)*error=
				"production resident transport authority requires device-private inputs";
				return false;}
			if([state length]!=9u*cells*sizeof(float)||
				[temperature length]!=cells*sizeof(float)||
				[velocity length]!=allFaces*sizeof(float)||
				[fuelInlet length]!=boundaryFaces*sizeof(unsigned char)||
				[thermochemistry length]!=MetalManifoldCertificateValues*sizeof(float)||
				[transport length]!=MetalResidentTransportSpeciesCount*
					MetalResidentTransportSpeciesStride*sizeof(float)||
				[parameters length]!=sizeof(MetalResidentTransportParameters)||
				[failure length]!=sizeof(std::uint32_t)||
				[obligations length]!=sizeof(std::uint32_t)){if(error)*error=
				"production resident transport authority buffer extent is invalid";
				return false;}
			authority.coefficients=[context.device newBufferWithLength:3u*cells*sizeof(float)
				options:MTLResourceStorageModePrivate];
			authority.publicationIdentity=[context.device newBufferWithLength:sizeof(std::uint64_t)
				options:MTLResourceStorageModePrivate];
			if(!authority.coefficients||!authority.publicationIdentity){
				if(error)*error="production resident transport authority allocation failed";
				return false;
			}
			authority.allocationBytes=[authority.coefficients allocatedSize]+
				[authority.publicationIdentity allocatedSize];
			authority.parentCommand=command;authority.parentState=state;
			authority.parentTemperature=temperature;authority.parentVelocity=velocity;
			authority.parentThermochemistry=thermochemistry;
			authority.parentParameters=parameters;authority.parentFailure=failure;
			authority.parentObligations=obligations;authority.parentCells=cells;
			authority.parentAllFaces=allFaces;authority.parentBoundaryFaces=boundaryFaces;
			id<MTLComputeCommandEncoder> encoder=ProducerProfileEncoder(command);
			if(!encoder){
				if(error)*error="production resident transport evaluation encoder failed";
				return false;
			}
			[encoder setBuffer:state offset:0 atIndex:0];
			[encoder setBuffer:temperature offset:0 atIndex:1];
			[encoder setBuffer:velocity offset:0 atIndex:2];
			[encoder setBuffer:thermochemistry offset:0 atIndex:3];
			[encoder setBuffer:transport offset:0 atIndex:4];
			[encoder setBuffer:authority.coefficients offset:0 atIndex:5];
			[encoder setBuffer:failure offset:0 atIndex:6];
			[encoder setBuffer:obligations offset:0 atIndex:7];
			[encoder setBuffer:parameters offset:0 atIndex:8];
			[encoder setBuffer:fuelInlet offset:0 atIndex:9];
			Dispatch(encoder,context.evaluate,cells);[encoder endEncoding];
			encoder=ProducerProfileEncoder(command);
			if(!encoder){
				if(error)*error="production resident transport identity encoder failed";
				return false;
			}
			[encoder setBuffer:state offset:0 atIndex:0];
			[encoder setBuffer:temperature offset:0 atIndex:1];
			[encoder setBuffer:velocity offset:0 atIndex:2];
			[encoder setBuffer:authority.coefficients offset:0 atIndex:3];
			[encoder setBuffer:thermochemistry offset:0 atIndex:4];
			[encoder setBuffer:transport offset:0 atIndex:5];
			[encoder setBuffer:fuelInlet offset:0 atIndex:6];
			[encoder setBuffer:authority.publicationIdentity offset:0 atIndex:7];
			[encoder setBuffer:failure offset:0 atIndex:8];
			[encoder setBuffer:parameters offset:0 atIndex:9];
			Dispatch(encoder,context.identify,1u);[encoder endEncoding];
			return true;
		}

		bool ValidateScalarFCTMetalStageRequest(
			const FireProductionScalarFCTRequest& request,std::string* error )
		{
			const FireProductionProjectionShape& shape=request.shape;
			if(shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
				shape.nz<4u||shape.nz>1024u||!(shape.cellWidthM>0.0f)||
				!std::isfinite(shape.cellWidthM)||!(request.timeStepS>0.0f)||
				!std::isfinite(request.timeStepS)||request.nullity==0u||request.nullity>8u||
				request.nullspaceBasis.size()!=8u*request.nullity||
				request.coordinateProjector.size()!=request.nullity*request.nullity||
				!std::isfinite(request.feasibilityFactor)||request.feasibilityFactor<=0.0f||
				!std::isfinite(request.assemblyReserveFactor)||request.assemblyReserveFactor<0.0f||
				request.assemblyReserveFactor>request.feasibilityFactor){if(error)*error=
					"production scalar FCT Metal stage schedule is invalid";
				return false;
			}
			const std::size_t cells=shape.CellCount();
			if(request.beginning.size()!=9u*cells||request.sourceDelta.size()!=9u*cells){
				if(error)*error="production scalar FCT Metal stage cell tuple is invalid";
				return false;
			}
			auto faceIndex=[&](const unsigned int axis,const std::size_t x,
				const std::size_t y,const std::size_t z){return axis==0u?
				(z*shape.ny+y)*(shape.nx+1u)+x:(axis==1u?
				(z*(shape.ny+1u)+y)*shape.nx+x:(z*shape.ny+y)*shape.nx+x);};
			bool anyPeriodic=false,allPeriodic=true;
			for(unsigned int axis=0u;axis<3u;++axis){
				const FireProductionProjectionBoundary lower=request.boundary[2u*axis],
					upper=request.boundary[2u*axis+1u];
				if(lower<FireProductionProjectionPeriodic||lower>FireProductionProjectionWall||
					upper<FireProductionProjectionPeriodic||upper>FireProductionProjectionWall||
					((lower==FireProductionProjectionPeriodic)!=(upper==
						FireProductionProjectionPeriodic))||request.frozenVelocityMPerS[axis].size()!=
					FireProductionProjectionFaceCount(shape,axis)){if(error)*error=
					"production scalar FCT Metal stage boundary or velocity shape is invalid";
					return false;
				}
				anyPeriodic=anyPeriodic||lower==FireProductionProjectionPeriodic;
				allPeriodic=allPeriodic&&lower==FireProductionProjectionPeriodic;
				for(const float value:request.frozenVelocityMPerS[axis])if(!std::isfinite(value)){
					if(error)*error="production scalar FCT Metal stage velocity is nonfinite";
					return false;
				}
				if(lower==FireProductionProjectionPeriodic){const std::size_t extent=axis==0u?
					shape.nx:(axis==1u?shape.ny:shape.nz),firstEnd=axis==0u?shape.ny:shape.nx,
					secondEnd=axis==2u?shape.ny:shape.nz;
					for(std::size_t second=0u;second<secondEnd;++second)
						for(std::size_t first=0u;first<firstEnd;++first){std::size_t lx=0u,ly=0u,lz=0u,
							hx=0u,hy=0u,hz=0u;if(axis==0u){ly=hy=first;lz=hz=second;hx=extent;}
							if(axis==1u){lx=hx=first;lz=hz=second;hy=extent;}
							if(axis==2u){lx=hx=first;ly=hy=second;hz=extent;}
							const float low=request.frozenVelocityMPerS[axis][faceIndex(axis,lx,ly,lz)],
								high=request.frozenVelocityMPerS[axis][faceIndex(axis,hx,hy,hz)];
							std::uint32_t lowBits=0u,highBits=0u;std::memcpy(&lowBits,&low,sizeof(lowBits));
							std::memcpy(&highBits,&high,sizeof(highBits));if(lowBits!=highBits){if(error)*error=
								"production scalar FCT Metal stage periodic velocity seam is invalid";
								return false;}}
				}
			}
			if(anyPeriodic&&!allPeriodic){if(error)*error=
				"production scalar FCT Metal stage hybrid periodic topology has no commuting oracle";
				return false;
			}
			for(unsigned int side=0u;side<6u;++side){const std::size_t expected=side<2u?
				shape.ny*shape.nz:(side<4u?shape.nx*shape.nz:shape.nx*shape.ny);
				if(request.pressureOpenInflow[side].size()!=expected){if(error)*error=
					"production scalar FCT Metal stage inflow shape is invalid";
					return false;
				}
				for(const unsigned char value:request.pressureOpenInflow[side])if(value>1u||
					(request.boundary[side]!=FireProductionProjectionPressureOpen&&value!=0u)){
					if(error)*error="production scalar FCT Metal stage inflow value is invalid";
					return false;
				}
			}
			for(const float value:request.beginning)if(!std::isfinite(value)){
				if(error)*error="production scalar FCT Metal stage beginning is nonfinite";
				return false;
			}
			for(const float value:request.sourceDelta)if(!std::isfinite(value)){
				if(error)*error="production scalar FCT Metal stage source is nonfinite";
				return false;
			}
			for(const float value:request.ambient)if(!std::isfinite(value)){
				if(error)*error="production scalar FCT Metal stage ambient is nonfinite";
				return false;
			}
			for(const float value:request.nullspaceBasis)if(!std::isfinite(value)){
				if(error)*error="production scalar FCT Metal stage basis is nonfinite";
				return false;
			}
			for(const float value:request.coordinateProjector)if(!std::isfinite(value)){
				if(error)*error="production scalar FCT Metal stage projector is nonfinite";
				return false;
			}
			for(const float value:request.enthalpyBoundsJPerKG)if(!std::isfinite(value)){
				if(error)*error="production scalar FCT Metal stage enthalpy bound is nonfinite";
				return false;
			}
			return true;
		}

		bool PrepareScalarFCTMetalStageMetadata(
			const FireProductionScalarFCTRequest& request,
			MetalSingleStageFCTParameters& parameters,
			std::vector<unsigned char>& packedInflow,
			std::array<std::size_t,3>& faceOffset,
			std::size_t& allFaces,std::string* error )
		{
			if(!ValidateScalarFCTMetalStageRequest(request,error))return false;
			const FireProductionProjectionShape& shape=request.shape;
			const std::size_t cells=shape.CellCount();allFaces=0u;
			for(unsigned int axis=0u;axis<3u;++axis){faceOffset[axis]=allFaces;
				allFaces+=FireProductionProjectionFaceCount(shape,axis);}
			parameters={static_cast<std::uint32_t>(shape.nx),
				static_cast<std::uint32_t>(shape.ny),static_cast<std::uint32_t>(shape.nz),
				static_cast<std::uint32_t>(cells),9u,11u,
				static_cast<std::uint32_t>(request.nullity),0u,{},{},shape.cellWidthM,
				request.timeStepS,request.feasibilityFactor,request.assemblyReserveFactor};
			std::size_t sideOffset=0u;packedInflow.clear();
			for(unsigned int side=0u;side<6u;++side){
				parameters.boundary[side]=static_cast<std::uint32_t>(request.boundary[side]);
				parameters.sideOffset[side]=static_cast<std::uint32_t>(sideOffset);
				packedInflow.insert(packedInflow.end(),request.pressureOpenInflow[side].begin(),
					request.pressureOpenInflow[side].end());
				sideOffset+=request.pressureOpenInflow[side].size();
			}
			return allFaces>0u&&sideOffset==packedInflow.size();
		}

		bool SameScalarFCTMetalPairIdentity(
			const FireProductionMetalScalarFCTFluxPairDiagnostic& first,
			const FireProductionMetalScalarFCTFluxPairDiagnostic& second )
		{
			std::array<std::size_t,3> expected={{0u,
				FireProductionProjectionFaceCount(first.shape,0u),0u}};
			expected[2]=expected[1]+FireProductionProjectionFaceCount(first.shape,1u);
			return first.shape.nx==second.shape.nx&&first.shape.ny==second.shape.ny&&
				first.shape.nz==second.shape.nz&&
				first.shape.cellWidthM==second.shape.cellWidthM&&
				first.timeStepS==second.timeStepS&&first.boundary==second.boundary&&
				first.packedFaceOffset==expected&&second.packedFaceOffset==expected&&first.lowFlux&&
				first.fluxDelta&&second.lowFlux&&second.fluxDelta;
		}

		MetalSingleStageFCTParameters ScalarFCTMetalPairParameters(
			const FireProductionMetalScalarFCTFluxPairDiagnostic& pair )
		{
			const FireProductionProjectionShape& shape=pair.shape;
			MetalSingleStageFCTParameters parameters={static_cast<std::uint32_t>(shape.nx),
				static_cast<std::uint32_t>(shape.ny),static_cast<std::uint32_t>(shape.nz),
				static_cast<std::uint32_t>(shape.CellCount()),9u,11u,0u,0u,{},{},
				shape.cellWidthM,pair.timeStepS,0.0f,0.0f};
			std::size_t sideOffset=0u;
			for(unsigned int side=0u;side<6u;++side){
				parameters.boundary[side]=static_cast<std::uint32_t>(pair.boundary[side]);
				parameters.sideOffset[side]=static_cast<std::uint32_t>(sideOffset);
				sideOffset+=side<2u?shape.ny*shape.nz:
					(side<4u?shape.nx*shape.nz:shape.nx*shape.ny);
			}
			return parameters;
		}
	}

	bool FireProductionProjectedHeunMetalOwnerWorkingSetBytes(
		const FireProductionProjectionShape& shape,std::uint64_t& bytes )
	{
		bytes=0u;std::uint64_t projection=0u,target=0u,nonpressure=0u;
		if(!FireProductionProjectionWorkingSetBytes(shape,projection)||
			!FireProductionResidentTargetLineageMetalWorkingSetBytes(shape,target)||
			!FireProductionNonpressureMomentumRHSMetalWorkingSetBytes(shape,nonpressure))return false;
		auto add=[&](const std::uint64_t value){const std::uint64_t rounded=
			(value+UINT64_C(16383))&~UINT64_C(16383);
			if(rounded<value||bytes>std::numeric_limits<std::uint64_t>::max()-rounded)return false;
			bytes+=rounded;return true;};
		// Two adjacent Picard candidates must coexist for the residual proof; R1
		// additionally retains the accepted R0 flux/RHS until its Heun publication.
		// The face term also covers the qualification staging of both exact resident
		// scalar-flux candidates used by the producer-lineage proof.
		if(!add(4u*projection)||!add(4u*target)||!add(3u*nonpressure)||
			!add((55u*shape.CellCount()+66u*(FireProductionProjectionFaceCount(shape,0u)+
				FireProductionProjectionFaceCount(shape,1u)+
				FireProductionProjectionFaceCount(shape,2u)))*sizeof(float))||
			!add(3u*sizeof(std::uint64_t)))return false;
		// r204 input binding: 20 cell fields, one face field, both boundary
		// classifications, plus <1 MiB of fixed tables/parameters/descriptor.
		// Each 4-KiB leaf produces 32 bytes; all interior levels add <1/15 of
		// the leaf storage. Reserve 8 allocation quanta per tree for rounding.
		const std::uint64_t cells=shape.CellCount(),faces=
			FireProductionProjectionFaceCount(shape,0u)+FireProductionProjectionFaceCount(shape,1u)+
			FireProductionProjectionFaceCount(shape,2u),boundary=2u*(shape.nx*shape.ny+
			shape.nx*shape.nz+shape.ny*shape.nz);
		const std::uint64_t input=80u*cells+4u*faces+2u*boundary+(UINT64_C(1)<<20u);
		const std::uint64_t publication=76u*cells+52u*faces+256u;
		if(!add(input)||!add(input/120u+8u*16384u)||
			!add(publication/120u+8u*16384u)||!add(64u)||!add(UINT64_C(1)<<20u))return false;
		// The complete owner intentionally retains adjacent Picard candidates.  Its
		// peak can exceed the historical process-agnostic two-GiB fixture ceiling at
		// production grids even when it is comfortably inside the active Metal
		// device's advertised working set.  Run() binds this certificate to that
		// device limit before allocating; qualificationWorkingSetLimitBytes remains
		// the deterministic under-statement RED.
		return true;
	}

	bool AttemptFireProductionProjectedHeunMetalOwner(
		const FireProductionProjectedHeunMetalOwnerRequest& request,
		FireProductionProjectedHeunMetalOwnerResult& result,std::string* error )
	{
		try {
			ResidentProjectedHeunMetalOwner owner(request,request.qualificationProductionStageTokens);
			return owner.Run(result,error);
		} catch(const std::bad_alloc&) {
			result=FireProductionProjectedHeunMetalOwnerResult();
			if(error)try{*error="projected-Heun resident owner allocation failed";}
				catch(const std::bad_alloc&){}
			return false;
		}
	}

	bool AttemptFireProductionProjectedHeunResidentStepMetal(
		const FireProductionProjectedHeunMetalOwnerRequest& request,
		FireProductionResidentStepResult& result,
		FireProductionProjectedHeunMetalOwnerResult* diagnostics,std::string* error )
	{
		result=FireProductionResidentStepResult();
		const bool qualificationSelected=request.qualificationStaleCandidate||
			request.qualificationOutOfOrderStage||request.qualificationForgedLineage||
			request.qualificationCallbackMutation||request.qualificationAtomicPublicationFailure||
			request.qualificationInjectInterstageTransfer||
			request.qualificationDivergentManifoldPolicy||
			request.qualificationStaleTargetPublication||
			request.qualificationUnverifiedPrivateLineageBuffer||
			request.qualificationForcedActiveCycleStage!=0u||
			request.qualificationDisableCanonicalCycle||
			request.qualificationForceLimiterDiscontinuity||
			request.qualificationDisableLimiterCertification||
			request.qualificationThreeQuarterHeunWeighting||
			request.qualificationReuseR0LimiterAlpha||
			request.qualificationWrongAveragedFluxParent||
			request.qualificationR2SealedClassPhysicalFlux||
			request.qualificationUnverifiedEndpointClassBuffer||
			request.qualificationCaptureIterationTrace||
			request.qualificationProductionStageTokens||
			request.qualificationWorkingSetLimitBytes!=0u;
		if(qualificationSelected){
			if(diagnostics)*diagnostics=FireProductionProjectedHeunMetalOwnerResult();
			if(error)*error="production resident step refuses qualification-only owner controls";
			return false;
		}
		FireProductionProjectedHeunMetalOwnerResult owner;
		try {ResidentProjectedHeunMetalOwner residentOwner(request,true);
			if(!residentOwner.Run(owner,error))return false;
		}catch(const std::bad_alloc&){if(error)*error="production owner allocation failed";return false;}
		const FireProductionProjectionShape& shape=request.lineage.eos.physicalFlux.transport.shape;
		const std::size_t cells=shape.CellCount();
		if(!owner.accepted||owner.ownerPublicationIdentity==0u||
			owner.conservativeValues.size()!=9u*cells||owner.absoluteEOSDeviation.size()!=cells||
			owner.representedPressureRatio.size()!=cells){
			if(error)*error="projected-Heun resident owner terminal payload is invalid";
			return false;
		}
		FireProductionResidentStepResult computed;
		computed.conservativeValues=owner.conservativeValues;
		computed.projection=owner.projection;computed.physicalProjection=owner.projection;
		computed.transportedDual.momentum=owner.provisionalMomentumKGPerM2S;
		computed.transportedDual.auxiliaryFaceDensity=owner.projection.faceDensityKGPerM3;
		computed.cellSubmapCount=2u;computed.dualSubmapCount=2u;
		computed.sourceCommandCommitCount=3u;
		computed.residentProjectionInvocationCount=owner.residentProjectionInvocationCount;
		computed.interstageFullGridTransferCount=owner.interstageFullGridTransferCount;
		computed.terminalStagingCount=owner.terminalStagingCount;
		computed.combinedCertifiedWorkingSetBytes=owner.certifiedWorkingSetBytes;
		computed.combinedActualMetalAllocationBytes=owner.actualMetalAllocationBytes;
		computed.projectedHeunOwnerIdentity=owner.ownerPublicationIdentity;
		computed.deviceElapsedMS=owner.deviceElapsedMS;computed.deviceMakespanMS=owner.deviceElapsedMS;
		computed.representedTimeStepS=request.lineage.eos.candidateTimeStepS;
		std::vector<double> signedDeviation(cells);std::vector<double> ordered(cells);
		for(std::size_t cell=0u;cell<cells;++cell){const double magnitude=
			static_cast<double>(owner.absoluteEOSDeviation[cell]);ordered[cell]=magnitude;
			signedDeviation[cell]=std::copysign(magnitude,
				static_cast<double>(owner.representedPressureRatio[cell])-1.0);}
		std::sort(ordered.begin(),ordered.end());
		computed.maximumAcceptedManifoldDeviation=ordered.empty()?0.0:ordered.back();
		computed.acceptedManifoldDeviationP50=ordered[(ordered.size()-1u)/2u];
		computed.acceptedManifoldDeviationP95=ordered[static_cast<std::size_t>(
			std::floor(0.95*static_cast<double>(ordered.size()-1u)))];
		FireProductionManifoldTailTarget tail;
		if(!DeriveFireProductionManifoldTailTarget(signedDeviation,
			static_cast<double>(computed.representedTimeStepS),shape.cellWidthM,tail,error))return false;
		computed.manifoldTailRestorationApplied=tail.outlierCellCount>0u;
		computed.manifoldTailCellCount=tail.outlierCellCount;
		computed.manifoldTailExcessSum=tail.excessSum;
		computed.manifoldTailDrainedVolumeM3=tail.drainedVolumeM3;
		computed.manifoldDynamicsBoundPassed=computed.maximumAcceptedManifoldDeviation<=0x1p-2;
		computed.maximumManifoldGeneration=0.0;computed.maximumPredictedAdvectiveManifoldAnomaly=0.0;
		computed.manifoldStageGeneration={{0.0,0.0,0.0}};
		computed.manifoldMapCellCount=static_cast<std::uint32_t>(cells);
		computed.manifoldScalarDeviceToHostTransferCount=1u;
		computed.manifoldFullGridDeviceToHostTransferCount=0u;
		computed.advectiveAnomalyClosurePassCount=0u;
		computed.manifoldDiagnosticsMonitored=true;computed.manifoldPlateauEnforced=false;
		computed.manifoldAllowanceExceeded=computed.maximumAcceptedManifoldDeviation>
			((1.0-0x1p-2)*0x1p-5);
		computed.manifoldCeilingExceeded=computed.maximumAcceptedManifoldDeviation>0x1p-5;
		computed.manifoldPlateauPassed=computed.manifoldDynamicsBoundPassed;
		computed.manifoldGenerationAuthoritative=false;
		computed.requiredRestorationDrainFraction=0.0;computed.deliveredRestorationDrainFraction=0.0;
		computed.restorationResidualBandPerS=0.0;computed.manifoldNextTimeStepAvailable=false;
		computed.suggestedManifoldTimeStepS=0.0;
		computed.conservativeProducerPrecision=FireStateProducerPrecision::Binary32;
		computed.acceptedShape=shape;
		if(!FireProductionResidentStepEligibleForAcceptedManifoldToken(computed)){
			if(error)*error="projected-Heun resident owner acceptance contract failed";return false;}
		computed.acceptedManifoldToken_.available_=true;
		computed.acceptedManifoldToken_.representedTimeStepS_=computed.representedTimeStepS;
		computed.acceptedManifoldToken_.maximumGeneration_=computed.maximumManifoldGeneration;
		computed.acceptedManifoldToken_.maximumAcceptedDeviation_=
			computed.maximumAcceptedManifoldDeviation;
		computed.acceptedManifoldToken_.acceptedDeviationP95_=computed.acceptedManifoldDeviationP95;
		computed.acceptedManifoldToken_.acceptedDeviationP50_=computed.acceptedManifoldDeviationP50;
		computed.acceptedManifoldToken_.tailRestorationApplied_=computed.manifoldTailRestorationApplied;
		computed.acceptedManifoldToken_.tailCellCount_=computed.manifoldTailCellCount;
		computed.acceptedManifoldToken_.tailExcessSum_=computed.manifoldTailExcessSum;
		computed.acceptedManifoldToken_.tailDrainedVolumeM3_=computed.manifoldTailDrainedVolumeM3;
		computed.acceptedManifoldToken_.dynamicsBoundPassed_=computed.manifoldDynamicsBoundPassed;
		computed.acceptedManifoldToken_.requiredDrainFraction_=0.0;
		computed.acceptedManifoldToken_.deliveredDrainFraction_=0.0;
		computed.acceptedManifoldToken_.maximumPostResidualPerS_=0.0;
		computed.acceptedManifoldToken_.physicalMaximumPreResidualPerS_=
			computed.physicalProjection.maximumPreProjectionResidualPerS;
		computed.acceptedManifoldToken_.physicalMaximumPostResidualPerS_=
			computed.physicalProjection.maximumPostProjectionResidualPerS;
		computed.acceptedManifoldToken_.payloadDigest_=
			FireProductionAcceptedManifoldPayloadDigest(computed);
		computed.acceptedManifoldToken_.acceptedStateDigest_=FireProductionAcceptedStatePayloadDigestFast(
			shape,computed.conservativeValues,computed.projection.momentumKGPerM2S,
			computed.projection.velocityMPerS);
		computed.acceptedManifoldToken_.acceptedStateDigestVersion_=2u;
		computed.acceptedManifoldToken_.publicationRoot_=owner.publicationPayloadRootSHA256;
		computed.acceptedManifoldToken_.generationAuthoritative_=false;
		computed.acceptedManifoldToken_.plateauEnforced_=false;
		if(!computed.AcceptedManifoldTokenMatchesCurrentPayload()){
			if(error)*error="projected-Heun resident owner token publication failed";return false;}
		result=std::move(computed);if(diagnostics)*diagnostics=std::move(owner);
		if(error)error->clear();return true;
	}

	bool FireProductionResidentTransportMetalWorkingSetBytes(
		const FireProductionProjectionShape& shape,std::uint64_t& bytes )
	{
		bytes=0u;
		if(shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
			shape.nz<4u||shape.nz>1024u||!std::isfinite(shape.cellWidthM)||
			!(shape.cellWidthM>0.0f))return false;
		const std::size_t cells=shape.CellCount();
		if(cells==0u||cells>std::numeric_limits<std::uint32_t>::max())return false;
		std::size_t faces=0u;
		for(unsigned int axis=0u;axis<3u;++axis){
			const std::size_t count=FireProductionProjectionFaceCount(shape,axis);
			if(faces>std::numeric_limits<std::size_t>::max()-count)return false;
			faces+=count;
		}
		const std::uint64_t stateBytes=9u*static_cast<std::uint64_t>(cells)*sizeof(float);
		const std::uint64_t temperatureBytes=static_cast<std::uint64_t>(cells)*sizeof(float);
		const std::uint64_t velocityBytes=static_cast<std::uint64_t>(faces)*sizeof(float);
		const std::uint64_t inflowBytes=2u*(static_cast<std::uint64_t>(shape.ny)*shape.nz+
			static_cast<std::uint64_t>(shape.nx)*shape.nz+
			static_cast<std::uint64_t>(shape.nx)*shape.ny)*sizeof(unsigned char);
		const std::uint64_t thermoBytes=MetalManifoldCertificateValues*sizeof(float);
		const std::uint64_t transportBytes=MetalResidentTransportSpeciesCount*
			MetalResidentTransportSpeciesStride*sizeof(float);
		const std::uint64_t parameterBytes=sizeof(MetalResidentTransportParameters);
		const std::uint64_t controlBytes=2u*sizeof(std::uint32_t);
		const std::uint64_t coefficientBytes=3u*static_cast<std::uint64_t>(cells)*sizeof(float);
		const std::uint64_t identityBytes=sizeof(std::uint64_t);
		const std::uint64_t terminalBytes=(coefficientBytes+7u)/8u*8u+
			identityBytes+controlBytes;
		auto add=[&](const std::uint64_t value){
			if(bytes>std::numeric_limits<std::uint64_t>::max()-value)return false;
			bytes+=value;return true;
		};
		// Fixture upload + resident copy for every immutable/input surface.
		if(!add(2u*stateBytes)||!add(2u*temperatureBytes)||!add(2u*velocityBytes)||
			!add(2u*inflowBytes)||
			!add(2u*thermoBytes)||!add(2u*transportBytes)||!add(2u*parameterBytes)||
			!add(2u*controlBytes)||!add(coefficientBytes)||!add(identityBytes)||
			!add(terminalBytes))return false;
		return bytes<=(UINT64_C(1)<<31u);
	}

	bool FireProductionResidentPhysicalFluxMetalWorkingSetBytes(
		const FireProductionProjectionShape& shape,std::uint64_t& bytes )
	{
		bytes=0u;
		if(shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
			shape.nz<4u||shape.nz>1024u||!std::isfinite(shape.cellWidthM)||
			!(shape.cellWidthM>0.0f))return false;
		const std::uint64_t cells=shape.CellCount();
		const std::uint64_t faces=FireProductionProjectionFaceCount(shape,0u)+
			FireProductionProjectionFaceCount(shape,1u)+
			FireProductionProjectionFaceCount(shape,2u);
		const std::uint64_t boundaryFaces=2u*(static_cast<std::uint64_t>(shape.ny)*shape.nz+
			static_cast<std::uint64_t>(shape.nx)*shape.nz+
			static_cast<std::uint64_t>(shape.nx)*shape.ny);
		auto add=[&](std::uint64_t requested){
			if(requested==0u)return false;const std::uint64_t quantum=UINT64_C(16384);
			const std::uint64_t remainder=requested%quantum;
			if(remainder){const std::uint64_t increment=quantum-remainder;
				if(requested>std::numeric_limits<std::uint64_t>::max()-increment)return false;
				requested+=increment;}
			if(bytes>std::numeric_limits<std::uint64_t>::max()-requested)return false;
			bytes+=requested;return true;};
		auto addPair=[&](const std::uint64_t requested){return add(requested)&&add(requested);};
		// Shared fixture upload + private resident copy for immutable inputs.
		if(!addPair(9u*cells*sizeof(float))||!addPair(cells*sizeof(float))||
			!addPair(faces*sizeof(float))||!addPair(boundaryFaces*sizeof(unsigned char))||
			!addPair(boundaryFaces*sizeof(unsigned char))||
			!addPair(MetalManifoldCertificateValues*sizeof(float))||
			!addPair(MetalResidentTransportSpeciesCount*MetalResidentTransportSpeciesStride*
				sizeof(float))||!addPair(9u*sizeof(float))||!addPair(64u*sizeof(float))||
			!addPair(64u*sizeof(float))||!addPair(64u*sizeof(float))||
			!addPair(sizeof(MetalResidentTransportParameters))||
			!addPair(sizeof(MetalResidentPhysicalFluxParameters))||
			!addPair(3u*sizeof(std::uint32_t)))return false;
		// Private transport publication, then the physical/advective authority.
		if(!add(3u*cells*sizeof(float))||!add(sizeof(std::uint64_t))||
			!add(9u*faces*sizeof(float))||!add(9u*faces*sizeof(float))||!add(9u*faces*sizeof(float))||
			!add(8u*faces*sizeof(float))||!add(faces*sizeof(float))||
			!add(faces*sizeof(float))||!add(faces*sizeof(float))||
			!add(7u*faces*sizeof(float))||!add(9u*faces*sizeof(float))||
			!add(9u*faces*sizeof(float))||!add(sizeof(std::uint64_t)))return false;
		// One terminal staging allocation for all published fields, identities,
		// and refusal/obligation words.
		const std::uint64_t terminal=(9u+9u+9u+8u+1u+1u+9u+9u+1u+7u)*faces*sizeof(float)+
			2u*cells*sizeof(float)+
			2u*sizeof(std::uint64_t)+3u*sizeof(std::uint32_t)+64u;
		return add(terminal)&&bytes<=(UINT64_C(1)<<31u);
	}

	bool FireProductionResidentEOSCandidateMetalWorkingSetBytes(
		const FireProductionProjectionShape& shape,std::uint64_t& bytes )
	{
		if(!FireProductionResidentPhysicalFluxMetalWorkingSetBytes(shape,bytes))return false;
		const std::uint64_t cells=shape.CellCount();
		const std::uint64_t faces=FireProductionProjectionFaceCount(shape,0u)+
			FireProductionProjectionFaceCount(shape,1u)+
			FireProductionProjectionFaceCount(shape,2u);
		auto add=[&](std::uint64_t requested){if(requested==0u)return false;
			const std::uint64_t quantum=UINT64_C(16384),remainder=requested%quantum;
			if(remainder){const std::uint64_t increment=quantum-remainder;
				if(requested>std::numeric_limits<std::uint64_t>::max()-increment)return false;
				requested+=increment;}
			if(bytes>std::numeric_limits<std::uint64_t>::max()-requested)return false;
			bytes+=requested;return true;};
		// Source/FCT-certificate upload/private pairs, EOS metadata upload/private
		// copy, compensated EOS-record upload/private copy, complete FCT working
		// fields, device-produced candidate and its producer/accepted identities,
		// three EOS fields and identity, isolated EOS obligation
		// word, and one terminal staging allocation.
		return add(9u*cells*sizeof(float))&&add(9u*cells*sizeof(float))&&
			add(14u*sizeof(float))&&add(14u*sizeof(float))&&add(64u*sizeof(float))&&
			add(64u*sizeof(float))&&add(sizeof(MetalSingleStageFCTParameters))&&
			add(sizeof(MetalSingleStageFCTParameters))&&
			add(sizeof(MetalResidentEOSParameters))&&add(sizeof(MetalResidentEOSParameters))&&
			add(MetalEOSThermochemistryValues*sizeof(float))&&
			add(MetalEOSThermochemistryValues*sizeof(float))&&
			add(9u*cells*sizeof(float))&&
			add(11u*cells*sizeof(float))&&add(faces*sizeof(float))&&
			add(9u*cells*sizeof(float))&&add(sizeof(std::uint64_t))&&add(sizeof(std::uint64_t))&&
			add(cells*sizeof(float))&&add(cells*sizeof(float))&&add(cells*sizeof(float))&&
			add(sizeof(std::uint64_t))&&add(sizeof(std::uint32_t))&&
			add(sizeof(std::uint32_t))&&add(cells*sizeof(std::uint32_t))&&
			add((12u*cells+faces)*sizeof(float)+cells*sizeof(std::uint32_t)+
				4u*sizeof(std::uint64_t)+3u*sizeof(std::uint32_t)+64u)&&
			bytes<=(UINT64_C(1)<<31u);
	}

	bool FireProductionResidentTargetLineageLiveIncrementWorkingSetBytes(
		const FireProductionProjectionShape& shape,std::uint64_t& bytes )
	{
		bytes=0u;
		if(shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
			shape.nz<4u||shape.nz>1024u||!std::isfinite(shape.cellWidthM)||
			!(shape.cellWidthM>0.0f))return false;
		const std::uint64_t cells=shape.CellCount();if(cells==0u)return false;
		auto add=[&](std::uint64_t requested){if(requested==0u)return false;
			const std::uint64_t quantum=UINT64_C(16384),remainder=requested%quantum;
			if(remainder){const std::uint64_t increment=quantum-remainder;
				if(requested>std::numeric_limits<std::uint64_t>::max()-increment)return false;
				requested+=increment;}
			if(bytes>std::numeric_limits<std::uint64_t>::max()-requested)return false;
			bytes+=requested;return true;};
		// Device-produced frozen-source field/metadata/identity, six separate
		// target term/base/final fields plus two authenticated enclosure fields,
		// device-issued projection metadata, the target/
		// projection-metadata identities, and the consumer identity.
		return add(cells*sizeof(float))&&add(sizeof(MetalResidentFrozenSourceParameters))&&
			add(sizeof(std::uint64_t))&&
			add(cells*sizeof(float))&&add(cells*sizeof(float))&&add(cells*sizeof(float))&&
			add(cells*sizeof(float))&&add(cells*sizeof(float))&&add(cells*sizeof(float))&&
			add(cells*sizeof(float))&&add(cells*sizeof(float))&&
			add(sizeof(MetalResidentProjectionConsumerParameters))&&add(sizeof(std::uint64_t))&&
			add(sizeof(std::uint64_t))&&add(sizeof(std::uint64_t));
	}

	bool FireProductionResidentTargetLineageMetalWorkingSetBytes(
		const FireProductionProjectionShape& shape,std::uint64_t& bytes )
	{
		if(!FireProductionResidentEOSCandidateMetalWorkingSetBytes(shape,bytes))return false;
		std::uint64_t increment=0u;
		if(!FireProductionResidentTargetLineageLiveIncrementWorkingSetBytes(shape,increment)||
			bytes>std::numeric_limits<std::uint64_t>::max()-increment)return false;
		bytes+=increment;const std::uint64_t cells=shape.CellCount();
		auto add=[&](std::uint64_t requested){const std::uint64_t quantum=UINT64_C(16384);
			const std::uint64_t remainder=requested%quantum;if(remainder)requested+=quantum-remainder;
			if(bytes>std::numeric_limits<std::uint64_t>::max()-requested)return false;
			bytes+=requested;return true;};
		// Canonical source-target upload plus private-substitution RED buffer, one
		// source-metadata upload, an alternate private EOS table for the handle-
		// lineage RED, target/projection metadata upload-private pairs, isolated
		// target obligation word, the sole terminal tap, and a qualification-only
		// shared full-grid transfer sink used by the observed transfer-ledger RED.
		// The resident source
		// authority itself is included in the live incremental certificate above.
		return add(cells*sizeof(float))&&add(cells*sizeof(float))&&
			add(sizeof(MetalResidentTargetParameters))&&
			add(MetalEOSThermochemistryValues*sizeof(float))&&
			add(sizeof(MetalResidentTargetParameters))&&add(sizeof(MetalResidentTargetParameters))&&
			add(sizeof(MetalResidentProjectionConsumerParameters))&&
			add(sizeof(MetalResidentProjectionConsumerParameters))&&
			add(sizeof(std::uint32_t))&&
			add(6u*cells*sizeof(float)+7u*sizeof(std::uint64_t)+
				2u*sizeof(std::uint32_t)+64u)&&add(cells*sizeof(float))&&
			bytes<=(UINT64_C(1)<<31u);
	}

	bool EvaluateFireProductionResidentTransportMetalComparator(
		const FireProductionResidentTransportComparatorRequest& request,
		FireProductionResidentTransportComparatorResult& result,std::string* error )
	{
		result=FireProductionResidentTransportComparatorResult();
		try {
			MetalResidentTransportParameters parameters={};
			std::array<std::size_t,3> faceOffset={{}};std::size_t allFaces=0u;
			std::vector<unsigned char> packedInflow;
			if(!PrepareResidentTransportRequest(request,parameters,faceOffset,allFaces,
				packedInflow,error))
				return false;
			if(allFaces>std::numeric_limits<std::uint32_t>::max()){
				if(error)*error="production resident transport face tuple is too large";
				return false;
			}
			std::array<float,MetalResidentTransportSpeciesCount*
				MetalResidentTransportSpeciesStride> packedTransport;
			if(!PackMetalResidentTransport(packedTransport,parameters,error))return false;
			std::array<float,MetalManifoldCertificateValues> packedThermochemistry;
			MetalManifoldParameters manifoldParameters={};
			std::array<double,7> lowerEnthalpy,upperEnthalpy;
			if(!PackMetalMethaneThermochemistry(packedThermochemistry,manifoldParameters,
				lowerEnthalpy,upperEnthalpy,error))return false;
			std::vector<float> packedVelocity;packedVelocity.reserve(allFaces);
			for(unsigned int axis=0u;axis<3u;++axis)packedVelocity.insert(
				packedVelocity.end(),request.projectedVelocityMPerS[axis].begin(),
				request.projectedVelocityMPerS[axis].end());
			ResidentTransportMetalContext& context=ResidentTransportContext();
			if(!context.Valid()){if(error)*error=context.error;return false;}
			const std::size_t cells=request.shape.CellCount();
			const std::size_t stateBytes=9u*cells*sizeof(float);
			const std::size_t temperatureBytes=cells*sizeof(float);
			const std::size_t velocityBytes=allFaces*sizeof(float);
			const std::size_t inflowBytes=packedInflow.size()*sizeof(unsigned char);
			const std::size_t thermoBytes=packedThermochemistry.size()*sizeof(float);
			const std::size_t transportBytes=packedTransport.size()*sizeof(float);
			const std::size_t coefficientBytes=3u*cells*sizeof(float);
			const std::size_t identityOffset=(coefficientBytes+7u)/8u*8u;
			const std::size_t controlOffset=identityOffset+sizeof(std::uint64_t);
			const std::size_t terminalBytes=controlOffset+2u*sizeof(std::uint32_t);
			std::uint64_t certified=0u;
			if(!FireProductionResidentTransportMetalWorkingSetBytes(request.shape,certified)){
				if(error)*error="production resident transport working-set certificate failed";
				return false;
			}
			const std::uint64_t beginningCommits=MetalCommandCommitCount;
			const std::uint64_t beginningReads=MetalHostBufferReadCount;
			@autoreleasepool {
				id<MTLBuffer> stateUpload=[context.device newBufferWithBytes:
					request.conservativeValues.data() length:stateBytes
					options:MTLResourceStorageModeShared];
				id<MTLBuffer> temperatureUpload=[context.device newBufferWithBytes:
					request.temperatureK.data() length:temperatureBytes
					options:MTLResourceStorageModeShared];
				id<MTLBuffer> velocityUpload=[context.device newBufferWithBytes:packedVelocity.data()
					length:velocityBytes options:MTLResourceStorageModeShared];
				id<MTLBuffer> inflowUpload=[context.device newBufferWithBytes:packedInflow.data()
					length:inflowBytes options:MTLResourceStorageModeShared];
				id<MTLBuffer> thermochemistryUpload=[context.device newBufferWithBytes:
					packedThermochemistry.data() length:thermoBytes options:MTLResourceStorageModeShared];
				id<MTLBuffer> transportUpload=[context.device newBufferWithBytes:
					packedTransport.data() length:transportBytes options:MTLResourceStorageModeShared];
				id<MTLBuffer> parameterUpload=[context.device newBufferWithBytes:&parameters
					length:sizeof(parameters) options:MTLResourceStorageModeShared];
				std::array<std::uint32_t,2> zeros={{0u,0u}};
				id<MTLBuffer> controlUpload=[context.device newBufferWithBytes:zeros.data()
					length:sizeof(zeros) options:MTLResourceStorageModeShared];
				auto privateBuffer=[&](const std::size_t size){return
					[context.device newBufferWithLength:size options:MTLResourceStorageModePrivate];};
				id<MTLBuffer> state=privateBuffer(stateBytes);
				id<MTLBuffer> temperature=privateBuffer(temperatureBytes);
				id<MTLBuffer> velocity=privateBuffer(velocityBytes);
				id<MTLBuffer> inflow=privateBuffer(inflowBytes);
				id<MTLBuffer> thermochemistry=privateBuffer(thermoBytes);
				id<MTLBuffer> transport=privateBuffer(transportBytes);
				id<MTLBuffer> parameter=privateBuffer(sizeof(parameters));
				id<MTLBuffer> failure=privateBuffer(sizeof(std::uint32_t));
				id<MTLBuffer> obligations=privateBuffer(sizeof(std::uint32_t));
				id<MTLBuffer> terminal=[context.device newBufferWithLength:terminalBytes
					options:MTLResourceStorageModeShared];
				const std::array<id<MTLBuffer>,18> preAuthority={{stateUpload,temperatureUpload,
					velocityUpload,inflowUpload,thermochemistryUpload,transportUpload,parameterUpload,
					controlUpload,state,temperature,velocity,inflow,thermochemistry,transport,parameter,
					failure,obligations,terminal}};
				for(id<MTLBuffer> buffer:preAuthority)if(!buffer){
					if(error)*error="production resident transport buffer allocation failed";
					return false;
				}
				id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context.queue);
				id<MTLBlitCommandEncoder> blit=command?[command blitCommandEncoder]:nil;
				if(!blit){
					if(error)*error="production resident transport upload encoder failed";
					return false;
				}
				auto upload=[&](id<MTLBuffer> source,id<MTLBuffer> destination,
					const std::size_t size){[blit copyFromBuffer:source sourceOffset:0
						toBuffer:destination destinationOffset:0 size:size];};
				upload(stateUpload,state,stateBytes);upload(temperatureUpload,temperature,
					temperatureBytes);upload(velocityUpload,velocity,velocityBytes);
				upload(inflowUpload,inflow,inflowBytes);
				upload(thermochemistryUpload,thermochemistry,thermoBytes);
				upload(transportUpload,transport,transportBytes);
				upload(parameterUpload,parameter,sizeof(parameters));
				[blit copyFromBuffer:controlUpload sourceOffset:0 toBuffer:failure
					destinationOffset:0 size:sizeof(std::uint32_t)];
				[blit copyFromBuffer:controlUpload sourceOffset:sizeof(std::uint32_t)
					toBuffer:obligations destinationOffset:0 size:sizeof(std::uint32_t)];
				[blit endEncoding];
				ResidentTransportMetalAuthority authority;
				if(!EncodeResidentTransportAuthority(context,command,state,temperature,velocity,
					thermochemistry,transport,inflow,parameter,failure,obligations,cells,allFaces,
					packedInflow.size(),authority,error))
					return false;
				blit=[command blitCommandEncoder];
				if(!blit){
					if(error)*error="production resident transport terminal encoder failed";
					return false;
				}
				[blit copyFromBuffer:authority.coefficients sourceOffset:0 toBuffer:terminal
					destinationOffset:0 size:coefficientBytes];
				[blit copyFromBuffer:authority.publicationIdentity sourceOffset:0 toBuffer:terminal
					destinationOffset:identityOffset size:sizeof(std::uint64_t)];
				[blit copyFromBuffer:failure sourceOffset:0 toBuffer:terminal
					destinationOffset:controlOffset size:sizeof(std::uint32_t)];
				[blit copyFromBuffer:obligations sourceOffset:0 toBuffer:terminal
					destinationOffset:controlOffset+sizeof(std::uint32_t)
					size:sizeof(std::uint32_t)];[blit endEncoding];
				CommitTrackedMetalCommand(command);[command waitUntilCompleted];
				if([command status]!=MTLCommandBufferStatusCompleted){
					if(error)*error="production resident transport command failed";
					return false;
				}
				const unsigned char* bytes=static_cast<const unsigned char*>(
					ReadTrackedMetalBuffer(terminal));
				if(!bytes){
					if(error)*error=
						"production resident transport terminal payload is unavailable";
					return false;
				}
				const std::uint32_t* controls=reinterpret_cast<const std::uint32_t*>(
					bytes+controlOffset);
				if(controls[0]!=0u){
					if(error)*error="production resident transport device validation failed";
					return false;
				}
				FireProductionResidentTransportComparatorResult computed;
				const float* values=reinterpret_cast<const float*>(bytes);
				computed.diffusivityM2PerS.assign(values,values+cells);
				computed.conductivityWPerMK.assign(values+cells,values+2u*cells);
				computed.molecularKinematicViscosityM2PerS.assign(values+2u*cells,
					values+3u*cells);
				std::memcpy(&computed.devicePublicationIdentity,bytes+identityOffset,
					sizeof(computed.devicePublicationIdentity));
				computed.stage=request.stage;computed.attemptIdentity=request.attemptIdentity;
				computed.parentCandidateIdentity=request.parentCandidateIdentity;
				computed.projectionIdentity=request.projectionIdentity;
				const std::uint64_t observedCommits=MetalCommandCommitCount-beginningCommits;
				const std::uint64_t observedReads=MetalHostBufferReadCount-beginningReads;
				computed.commandCommitCount=static_cast<std::uint32_t>(observedCommits);
				computed.interstageFullGridTransferCount=static_cast<std::uint32_t>(
					observedReads>0u?observedReads-1u:0u);
				computed.terminalStagingCount=static_cast<std::uint32_t>(observedReads);
				computed.branchObligationBitmap=controls[1];
				computed.certifiedWorkingSetBytes=certified;
				std::uint64_t actual=authority.allocationBytes;
				for(id<MTLBuffer> buffer:preAuthority){const std::uint64_t allocation=
					[buffer allocatedSize];if(actual>std::numeric_limits<std::uint64_t>::max()-
					allocation){
						if(error)*error=
							"production resident transport allocation count overflowed";
						return false;
					}
					actual+=allocation;}
				computed.actualMetalAllocationBytes=actual;
				computed.deviceElapsedMS=([command GPUEndTime]-[command GPUStartTime])*1000.0;
				computed.deviceProduced=computed.devicePublicationIdentity!=0u;
				if(observedCommits!=1u||observedReads!=1u||!computed.deviceProduced||
					!AllFinite(computed.diffusivityM2PerS)||
					!AllFinite(computed.conductivityWPerMK)||
					!AllFinite(computed.molecularKinematicViscosityM2PerS)||
					!std::isfinite(computed.deviceElapsedMS)||computed.deviceElapsedMS<0.0||
					actual>certified){if(error)*error=
					"production resident transport publication failed its certificate";
					return false;}
				result=std::move(computed);if(error)error->clear();return true;
			}
		} catch(const std::bad_alloc&){
			result=FireProductionResidentTransportComparatorResult();
			if(error)try{*error="production resident transport allocation failed";}
				catch(const std::bad_alloc&){}
			return false;
		}
	}

	bool EvaluateFireProductionResidentPhysicalFluxMetalComparator(
		const FireProductionResidentPhysicalFluxComparatorRequest& request,
		FireProductionResidentPhysicalFluxComparatorResult& result,std::string* error )
	{
		result=FireProductionResidentPhysicalFluxComparatorResult();
		try {
			MetalResidentTransportParameters transportParameters={};
			MetalResidentPhysicalFluxParameters physicalParameters={};
			std::array<std::size_t,3> faceOffset={{}};std::size_t allFaces=0u;
			std::vector<unsigned char> packedFuelInlet,packedPressureInflow;
			std::vector<float> physicalBasis;
			if(!PrepareResidentPhysicalFluxRequest(request,transportParameters,
				physicalParameters,faceOffset,allFaces,packedFuelInlet,packedPressureInflow,
				physicalBasis,error))return false;
			std::array<float,MetalResidentTransportSpeciesCount*
				MetalResidentTransportSpeciesStride> packedTransport;
			if(!PackMetalResidentTransport(packedTransport,transportParameters,error))return false;
			std::array<float,MetalManifoldCertificateValues> packedThermochemistry;
			std::array<float,MetalEOSThermochemistryValues> packedEOSThermochemistry;
			MetalManifoldParameters manifoldParameters={};std::array<double,7> lower,upper;
			if(!PackMetalMethaneThermochemistry(packedThermochemistry,manifoldParameters,
				lower,upper,error)||!PackMetalEOSDoubleDoubleThermochemistry(
					packedEOSThermochemistry,error))return false;
			std::vector<float> packedVelocity;packedVelocity.reserve(allFaces);
			for(const std::vector<float>& axis:request.transport.projectedVelocityMPerS)
				packedVelocity.insert(packedVelocity.end(),axis.begin(),axis.end());
			ResidentTransportMetalContext& context=ResidentTransportContext();
			if(!context.Valid()){if(error)*error=context.error;return false;}
			const std::size_t cells=request.transport.shape.CellCount();
			const std::size_t stateBytes=9u*cells*sizeof(float),temperatureBytes=cells*sizeof(float),
				velocityBytes=allFaces*sizeof(float),fuelBytes=packedFuelInlet.size(),
				inflowBytes=packedPressureInflow.size(),thermoBytes=packedThermochemistry.size()*sizeof(float),
				transportBytes=packedTransport.size()*sizeof(float),ambientBytes=9u*sizeof(float),
				physicalBasisBytes=physicalBasis.size()*sizeof(float),
				advectiveBasisBytes=request.nullspaceBasis.size()*sizeof(float),
				projectorBytes=request.coordinateProjector.size()*sizeof(float),
				controlBytes=3u*sizeof(std::uint32_t);
			const std::size_t donorBytes=9u*allFaces*sizeof(float),massBytes=8u*allFaces*sizeof(float),
				scalarFaceBytes=allFaces*sizeof(float);
			std::size_t terminalBytes=0u;
			auto terminalAdd=[&](const std::size_t value){
				if(terminalBytes>std::numeric_limits<std::size_t>::max()-value)return false;
				terminalBytes+=value;return true;
			};
			std::array<std::size_t,13> terminalOffset={{}};unsigned int terminalField=0u;
			auto reserveTerminal=[&](const std::size_t value){terminalOffset[terminalField++]=terminalBytes;
				return terminalAdd(value);};
			if(!reserveTerminal(donorBytes)||!reserveTerminal(donorBytes)||!reserveTerminal(donorBytes)||
				!reserveTerminal(massBytes)||!reserveTerminal(scalarFaceBytes)||
				!reserveTerminal(scalarFaceBytes)||!reserveTerminal(donorBytes)||
				!reserveTerminal(donorBytes)||!reserveTerminal(scalarFaceBytes)||
				!reserveTerminal(7u*scalarFaceBytes)||
				!reserveTerminal(2u*cells*sizeof(float))){
				if(error)*error="production resident physical-flux terminal layout overflowed";
				return false;
			}
			terminalBytes=(terminalBytes+7u)&~std::size_t(7u);
			terminalOffset[terminalField++]=terminalBytes;terminalBytes+=2u*sizeof(std::uint64_t);
			terminalOffset[terminalField++]=terminalBytes;terminalBytes+=controlBytes;
			std::uint64_t certified=0u;
			if(!FireProductionResidentPhysicalFluxMetalWorkingSetBytes(request.transport.shape,
				certified)){if(error)*error="production resident physical-flux working-set certificate failed";
				return false;}
			if(certified>request.qualificationWorkingSetLimitBytes){if(error)*error=
				"production resident physical-flux working-set limit is understated";
				return false;}
			const std::uint64_t beginningCommits=MetalCommandCommitCount,
				beginningReads=MetalHostBufferReadCount;
			@autoreleasepool {
				auto upload=[&](const void* bytes,const std::size_t length){return
					[context.device newBufferWithBytes:bytes length:length options:MTLResourceStorageModeShared];};
				auto privateBuffer=[&](const std::size_t length){return
					[context.device newBufferWithLength:length options:MTLResourceStorageModePrivate];};
				id<MTLBuffer> stateUpload=upload(request.transport.conservativeValues.data(),stateBytes);
				id<MTLBuffer> temperatureUpload=upload(request.transport.temperatureK.data(),temperatureBytes);
				id<MTLBuffer> velocityUpload=upload(packedVelocity.data(),velocityBytes);
				id<MTLBuffer> fuelUpload=upload(packedFuelInlet.data(),fuelBytes);
				id<MTLBuffer> inflowUpload=upload(packedPressureInflow.data(),inflowBytes);
				id<MTLBuffer> thermoUpload=upload(packedThermochemistry.data(),thermoBytes);
				id<MTLBuffer> transportUpload=upload(packedTransport.data(),transportBytes);
				id<MTLBuffer> ambientUpload=upload(request.ambient.data(),ambientBytes);
				id<MTLBuffer> physicalBasisUpload=upload(physicalBasis.data(),physicalBasisBytes);
				id<MTLBuffer> advectiveBasisUpload=upload(request.nullspaceBasis.data(),advectiveBasisBytes);
				id<MTLBuffer> projectorUpload=upload(request.coordinateProjector.data(),projectorBytes);
				id<MTLBuffer> transportParameterUpload=upload(&transportParameters,sizeof(transportParameters));
				id<MTLBuffer> physicalParameterUpload=upload(&physicalParameters,sizeof(physicalParameters));
				std::array<std::uint32_t,3> zeros={{0u,0u,0u}};
				id<MTLBuffer> controlUpload=upload(zeros.data(),controlBytes);
				const std::size_t privateInflowBytes=request.qualificationShortInflowSurface?
					inflowBytes-1u:inflowBytes;
				const std::size_t privatePhysicalBasisBytes=
					request.qualificationOversizedPhysicalBasisSurface?
					physicalBasisBytes+sizeof(float):physicalBasisBytes;
				id<MTLBuffer> state=privateBuffer(stateBytes),temperature=privateBuffer(temperatureBytes),
					velocity=privateBuffer(velocityBytes),fuel=privateBuffer(fuelBytes),
					inflow=privateBuffer(privateInflowBytes),thermo=privateBuffer(thermoBytes),
					transport=privateBuffer(transportBytes),ambient=privateBuffer(ambientBytes),
					physicalBasisBuffer=privateBuffer(privatePhysicalBasisBytes),
					advectiveBasis=privateBuffer(advectiveBasisBytes),projector=privateBuffer(projectorBytes),
					transportParameter=privateBuffer(sizeof(transportParameters)),
					physicalParameter=privateBuffer(sizeof(physicalParameters)),
					failure=privateBuffer(sizeof(std::uint32_t)),
					transportObligations=privateBuffer(sizeof(std::uint32_t)),
					physicalObligations=privateBuffer(sizeof(std::uint32_t));
				id<MTLBuffer> mismatchedState=request.qualificationMismatchedParentCandidate?
					privateBuffer(stateBytes):nil;
				id<MTLBuffer> terminal=[context.device newBufferWithLength:terminalBytes
					options:MTLResourceStorageModeShared];
				const std::array<id<MTLBuffer>,31> buffers={{stateUpload,temperatureUpload,
					velocityUpload,fuelUpload,inflowUpload,thermoUpload,transportUpload,ambientUpload,
					physicalBasisUpload,advectiveBasisUpload,projectorUpload,transportParameterUpload,
					physicalParameterUpload,controlUpload,state,temperature,velocity,fuel,inflow,thermo,
					transport,ambient,physicalBasisBuffer,advectiveBasis,projector,transportParameter,
					physicalParameter,failure,transportObligations,physicalObligations,terminal}};
				for(id<MTLBuffer> buffer:buffers)if(!buffer){
					if(error)*error="production resident physical-flux buffer allocation failed";
					return false;
				}
				id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context.queue);
				id<MTLBlitCommandEncoder> blit=command?[command blitCommandEncoder]:nil;
				if(!blit){if(error)*error="production resident physical-flux upload encoder failed";return false;}
				auto copy=[&](id<MTLBuffer> source,id<MTLBuffer> destination,const std::size_t length){
					[blit copyFromBuffer:source sourceOffset:0 toBuffer:destination destinationOffset:0 size:length];};
				copy(stateUpload,state,stateBytes);copy(temperatureUpload,temperature,temperatureBytes);
				copy(velocityUpload,velocity,velocityBytes);copy(fuelUpload,fuel,fuelBytes);
				copy(inflowUpload,inflow,privateInflowBytes);copy(thermoUpload,thermo,thermoBytes);
				copy(transportUpload,transport,transportBytes);copy(ambientUpload,ambient,ambientBytes);
				copy(physicalBasisUpload,physicalBasisBuffer,physicalBasisBytes);
				copy(advectiveBasisUpload,advectiveBasis,advectiveBasisBytes);
				copy(projectorUpload,projector,projectorBytes);
				copy(transportParameterUpload,transportParameter,sizeof(transportParameters));
				copy(physicalParameterUpload,physicalParameter,sizeof(physicalParameters));
				[blit copyFromBuffer:controlUpload sourceOffset:0 toBuffer:failure destinationOffset:0
					size:sizeof(std::uint32_t)];
				[blit copyFromBuffer:controlUpload sourceOffset:sizeof(std::uint32_t)
					toBuffer:transportObligations destinationOffset:0 size:sizeof(std::uint32_t)];
				[blit copyFromBuffer:controlUpload sourceOffset:2u*sizeof(std::uint32_t)
					toBuffer:physicalObligations destinationOffset:0 size:sizeof(std::uint32_t)];
				if(mismatchedState)copy(stateUpload,mismatchedState,stateBytes);[blit endEncoding];
				ResidentTransportMetalAuthority transportAuthority;
				if(!EncodeResidentTransportAuthority(context,command,state,temperature,velocity,
					thermo,transport,fuel,transportParameter,failure,transportObligations,cells,allFaces,
					packedFuelInlet.size(),transportAuthority,error))return false;
				ResidentPhysicalFluxMetalAuthority physicalAuthority;
				ResidentEndpointClassMetalAuthority endpointClassAuthority;
				if(!EncodeResidentEndpointClassAuthority(context,command,inflow,
					transportAuthority.publicationIdentity,transportParameter,failure,
					transportAuthority,cells,allFaces,packedPressureInflow.size(),false,
					nullptr,endpointClassAuthority,error))return false;
				id<MTLBuffer> childState=mismatchedState?mismatchedState:state;
				if(!EncodeResidentPhysicalFluxAuthority(context,command,childState,temperature,velocity,
					thermo,ambient,physicalBasisBuffer,advectiveBasis,projector,
					transportParameter,physicalParameter,failure,physicalObligations,transportAuthority,
					endpointClassAuthority,cells,allFaces,physicalParameters,physicalAuthority,error))return false;
				blit=[command blitCommandEncoder];if(!blit){
					if(error)*error="production resident physical-flux terminal encoder failed";
					return false;
				}
				const std::array<id<MTLBuffer>,10> fields={{physicalAuthority.donorAdvective,
					physicalAuthority.advectiveDelta,physicalAuthority.mcMusclAdvective,physicalAuthority.physicalMass,
					physicalAuthority.physicalEnergy,physicalAuthority.physicalGas,
					physicalAuthority.lowComposite,physicalAuthority.highComposite,
					physicalAuthority.faceLogTemperature,
					physicalAuthority.faceSensibleEnthalpy}};
				const std::array<std::size_t,10> fieldBytes={{donorBytes,donorBytes,donorBytes,massBytes,
					scalarFaceBytes,scalarFaceBytes,donorBytes,donorBytes,scalarFaceBytes,
					7u*scalarFaceBytes}};
				for(unsigned int field=0u;field<fields.size();++field)
					[blit copyFromBuffer:fields[field] sourceOffset:0 toBuffer:terminal
						destinationOffset:terminalOffset[field] size:fieldBytes[field]];
				[blit copyFromBuffer:transportAuthority.publicationIdentity sourceOffset:0
					toBuffer:terminal destinationOffset:terminalOffset[11] size:sizeof(std::uint64_t)];
				[blit copyFromBuffer:transportAuthority.coefficients sourceOffset:0
					toBuffer:terminal destinationOffset:terminalOffset[10] size:2u*cells*sizeof(float)];
				[blit copyFromBuffer:physicalAuthority.publicationIdentity sourceOffset:0
					toBuffer:terminal destinationOffset:terminalOffset[11]+sizeof(std::uint64_t)
					size:sizeof(std::uint64_t)];
				[blit copyFromBuffer:failure sourceOffset:0 toBuffer:terminal
					destinationOffset:terminalOffset[12] size:sizeof(std::uint32_t)];
				[blit copyFromBuffer:physicalObligations sourceOffset:0 toBuffer:terminal
					destinationOffset:terminalOffset[12]+sizeof(std::uint32_t)
					size:sizeof(std::uint32_t)];[blit endEncoding];
				CommitTrackedMetalCommand(command);[command waitUntilCompleted];
				if([command status]!=MTLCommandBufferStatusCompleted){
					if(error)*error="production resident physical-flux command failed";
					return false;
				}
				const unsigned char* bytes=static_cast<const unsigned char*>(ReadTrackedMetalBuffer(terminal));
				if(!bytes){if(error)*error="production resident physical-flux terminal is unavailable";return false;}
				const std::uint32_t* controls=reinterpret_cast<const std::uint32_t*>(bytes+terminalOffset[12]);
				if(controls[0]!=0u){if(error)*error="production resident physical-flux device validation failed";return false;}
				FireProductionResidentPhysicalFluxComparatorResult computed;
				auto assign=[&](std::vector<float>& destination,const unsigned int field,
					const std::size_t count){const float* values=reinterpret_cast<const float*>(bytes+
						terminalOffset[field]);destination.assign(values,values+count);};
				assign(computed.donorAdvectiveFlux,0u,9u*allFaces);
				assign(computed.advectiveFluxDelta,1u,9u*allFaces);
				assign(computed.mcMusclAdvectiveFlux,2u,9u*allFaces);
				assign(computed.physicalMassFluxKGPerM2S,3u,8u*allFaces);
				assign(computed.physicalEnergyFluxWPerM2,4u,allFaces);
				assign(computed.faceLogTemperature,8u,allFaces);
				assign(computed.faceSensibleEnthalpyJPerKG,9u,7u*allFaces);
				const float* gas=reinterpret_cast<const float*>(bytes+terminalOffset[5]);
				for(unsigned int axis=0u;axis<3u;++axis)computed.physicalGasFluxKGPerM2S[axis].assign(
					gas+faceOffset[axis],gas+faceOffset[axis]+FireProductionProjectionFaceCount(
						request.transport.shape,axis));
				assign(computed.lowCompositeFlux,6u,9u*allFaces);
				assign(computed.highCompositeFlux,7u,9u*allFaces);
				const float* coefficients=reinterpret_cast<const float*>(bytes+terminalOffset[10]);
				computed.diffusivityM2PerS.assign(coefficients,coefficients+cells);
				computed.conductivityWPerMK.assign(coefficients+cells,coefficients+2u*cells);
				const std::uint64_t* identities=reinterpret_cast<const std::uint64_t*>(bytes+terminalOffset[11]);
				computed.transportPublicationIdentity=identities[0];computed.devicePublicationIdentity=identities[1];
				computed.packedFaceOffset=faceOffset;computed.branchObligationBitmap=controls[1];
				const std::uint64_t observedCommits=MetalCommandCommitCount-beginningCommits,
					observedReads=MetalHostBufferReadCount-beginningReads;
				computed.commandCommitCount=static_cast<std::uint32_t>(observedCommits);
				computed.terminalStagingCount=static_cast<std::uint32_t>(observedReads);
				computed.interstageFullGridTransferCount=static_cast<std::uint32_t>(
					observedReads>0u?observedReads-1u:0u);computed.certifiedWorkingSetBytes=certified;
				computed.actualMetalAllocationBytes=transportAuthority.allocationBytes+
					physicalAuthority.allocationBytes;
				computed.liveAuthorityAllocationBytes=physicalAuthority.allocationBytes;
				for(id<MTLBuffer> buffer:buffers)computed.actualMetalAllocationBytes+=[buffer allocatedSize];
				computed.deviceElapsedMS=([command GPUEndTime]-[command GPUStartTime])*1000.0;
				computed.deviceProduced=computed.transportPublicationIdentity!=0u&&
					computed.devicePublicationIdentity!=0u;
				if(observedCommits!=1u||observedReads!=1u||!computed.deviceProduced||
					computed.actualMetalAllocationBytes>computed.certifiedWorkingSetBytes||
					!AllFinite(computed.donorAdvectiveFlux)||!AllFinite(computed.advectiveFluxDelta)||
					!AllFinite(computed.mcMusclAdvectiveFlux)||
					!AllFinite(computed.physicalMassFluxKGPerM2S)||
					!AllFinite(computed.physicalEnergyFluxWPerM2)||
					!AllFinite(computed.faceLogTemperature)||
					!AllFinite(computed.faceSensibleEnthalpyJPerKG)||
					!AllFinite(computed.lowCompositeFlux)||!AllFinite(computed.highCompositeFlux)){
					if(error)*error="production resident physical-flux publication failed its certificate";
					return false;
				}
				for(const std::vector<float>& axis:computed.physicalGasFluxKGPerM2S)
					if(!AllFinite(axis)){
						if(error)*error="production resident physical gas flux is nonfinite";
						return false;
					}
				result=std::move(computed);if(error)error->clear();return true;
			}
		} catch(const std::bad_alloc&){
			result=FireProductionResidentPhysicalFluxComparatorResult();
			if(error)try{*error="production resident physical-flux allocation failed";}
				catch(const std::bad_alloc&){}
			return false;
		}
	}

	bool EvaluateFireProductionResidentEOSCandidateMetalComparator(
		const FireProductionResidentEOSCandidateComparatorRequest& request,
		FireProductionResidentEOSCandidateComparatorResult& result,std::string* error )
	{
		result=FireProductionResidentEOSCandidateComparatorResult();
		try {
			MetalResidentTransportParameters transportParameters={};
			MetalResidentPhysicalFluxParameters physicalParameters={};
			MetalResidentEOSParameters eosParameters={};
			std::array<std::size_t,3> faceOffset={{}};std::size_t allFaces=0u;
			std::vector<unsigned char> packedFuelInlet,packedPressureInflow;
			std::vector<float> physicalBasis;
			if(!PrepareResidentEOSCandidateRequest(request,transportParameters,physicalParameters,
				eosParameters,faceOffset,allFaces,packedFuelInlet,packedPressureInflow,
				physicalBasis,error))return false;
			const bool splitMetadata=request.qualificationMismatchedDeviceCase;
			MetalResidentEOSParameters deviceEOSParameters=eosParameters;
			if(request.qualificationMismatchedDeviceStage)deviceEOSParameters.stage=2u;
			if(request.qualificationMismatchedDevicePrecision)deviceEOSParameters.precision=1u;
			if(request.qualificationMismatchedDeviceAttempt)++deviceEOSParameters.attemptIdentity;
			if(request.qualificationMismatchedDeviceCells)++deviceEOSParameters.cells;
			if(request.qualificationMismatchedDeviceTimeStep)
				deviceEOSParameters.timeStepS=std::nextafter(deviceEOSParameters.timeStepS,
					std::numeric_limits<float>::infinity());
			if(request.qualificationAmbiguousPressureRounding)deviceEOSParameters.padding[0]|=1u;
			if(request.qualificationAmbiguousDeviationRounding)deviceEOSParameters.padding[0]|=2u;
			if(request.qualificationExactZeroDeviationRounding)deviceEOSParameters.padding[0]|=4u;
			if(request.qualificationMinimumSubnormalDeviationRounding)
				deviceEOSParameters.padding[0]|=8u;
			if(request.qualificationAmbiguousZeroDeviationRounding)
				deviceEOSParameters.padding[0]|=16u;
			if(request.qualificationAmbiguousSubnormalDeviationRounding)
				deviceEOSParameters.padding[0]|=32u;
			if(request.qualificationTwoCellDistinctEOSFailures)
				deviceEOSParameters.padding[1]|=1u;
			MetalResidentEOSParameters candidateEOSParameters=deviceEOSParameters;
			if(request.qualificationMismatchedDeviceCase)++candidateEOSParameters.caseIdentity;
			std::array<float,MetalResidentTransportSpeciesCount*
				MetalResidentTransportSpeciesStride> packedTransport;
			if(!PackMetalResidentTransport(packedTransport,transportParameters,error))return false;
			std::array<float,MetalManifoldCertificateValues> packedThermochemistry;
			std::array<float,MetalEOSThermochemistryValues> packedEOSThermochemistry;
			MetalManifoldParameters manifoldParameters={};std::array<double,7> lower,upper;
			if(!PackMetalMethaneThermochemistry(packedThermochemistry,manifoldParameters,
				lower,upper,error)||!PackMetalEOSDoubleDoubleThermochemistry(
					packedEOSThermochemistry,error))return false;
			std::vector<float> fctBasis,fctProjector,affine;
			std::array<float,14> enthalpyBounds={{}};float feasibility=0.0f,assemblyReserve=0.0f;
			if(!PackMetalSingleStageFCTCertificate(fctBasis,fctProjector,enthalpyBounds,affine,
				feasibility,assemblyReserve,error))return false;
			MetalSingleStageFCTParameters fctParameters={static_cast<std::uint32_t>(
				request.physicalFlux.transport.shape.nx),static_cast<std::uint32_t>(
				request.physicalFlux.transport.shape.ny),static_cast<std::uint32_t>(
				request.physicalFlux.transport.shape.nz),static_cast<std::uint32_t>(
				request.physicalFlux.transport.shape.CellCount()),9u,11u,
				static_cast<std::uint32_t>(request.physicalFlux.nullity),
				static_cast<std::uint32_t>(affine.size()/8u),{},{},
				request.physicalFlux.transport.shape.cellWidthM,request.candidateTimeStepS,
				feasibility,assemblyReserve};
			std::size_t fctSideOffset=0u;for(unsigned int side=0u;side<6u;++side){
				fctParameters.boundary[side]=static_cast<std::uint32_t>(
					request.physicalFlux.transport.boundary[side]);
				fctParameters.sideOffset[side]=static_cast<std::uint32_t>(fctSideOffset);
				fctSideOffset+=request.physicalFlux.pressureOpenInflow[side].size();}
			std::vector<float> packedVelocity;packedVelocity.reserve(allFaces);
			for(const std::vector<float>& axis:request.physicalFlux.transport.projectedVelocityMPerS)
				packedVelocity.insert(packedVelocity.end(),axis.begin(),axis.end());
			ResidentTransportMetalContext& context=ResidentTransportContext();
			if(!context.Valid()){if(error)*error=context.error;return false;}
			const FireProductionProjectionShape& shape=request.physicalFlux.transport.shape;
			const std::size_t cells=shape.CellCount(),stateBytes=9u*cells*sizeof(float),
				temperatureBytes=cells*sizeof(float),velocityBytes=allFaces*sizeof(float),
				fuelBytes=packedFuelInlet.size(),inflowBytes=packedPressureInflow.size(),
				thermoBytes=packedThermochemistry.size()*sizeof(float),
				eosThermoBytes=packedEOSThermochemistry.size()*sizeof(float),
				transportBytes=packedTransport.size()*sizeof(float),ambientBytes=9u*sizeof(float),
				physicalBasisBytes=physicalBasis.size()*sizeof(float),
				advectiveBasisBytes=request.physicalFlux.nullspaceBasis.size()*sizeof(float),
				projectorBytes=request.physicalFlux.coordinateProjector.size()*sizeof(float),
				enthalpyBytes=enthalpyBounds.size()*sizeof(float),affineBytes=affine.size()*sizeof(float),
				controlBytes=4u*sizeof(std::uint32_t),fieldBytes=cells*sizeof(float);
			const std::size_t alphaOffset=stateBytes+3u*fieldBytes,
				identityOffset=(alphaOffset+allFaces*sizeof(float)+7u)&~std::size_t(7u),
				controlOffset=identityOffset+4u*sizeof(std::uint64_t),
				failureMapOffset=controlOffset+3u*sizeof(std::uint32_t),
				terminalBytes=failureMapOffset+fieldBytes;
			std::uint64_t certified=0u;
			if(!FireProductionResidentEOSCandidateMetalWorkingSetBytes(shape,certified)){
				if(error)*error="production resident EOS working-set certificate failed";return false;}
			if(certified>request.qualificationWorkingSetLimitBytes){
				if(error)*error="production resident EOS working-set limit is understated";
				return false;
			}
			const std::uint64_t beginningCommits=MetalCommandCommitCount,
				beginningReads=MetalHostBufferReadCount;
			@autoreleasepool {
				auto upload=[&](const void* bytes,const std::size_t length){return
					[context.device newBufferWithBytes:bytes length:length options:MTLResourceStorageModeShared];};
				auto privateBuffer=[&](const std::size_t length){return
					[context.device newBufferWithLength:length options:MTLResourceStorageModePrivate];};
				id<MTLBuffer> stateUpload=upload(request.physicalFlux.transport.conservativeValues.data(),stateBytes);
				id<MTLBuffer> temperatureUpload=upload(request.physicalFlux.transport.temperatureK.data(),temperatureBytes);
				id<MTLBuffer> velocityUpload=upload(packedVelocity.data(),velocityBytes);
				id<MTLBuffer> fuelUpload=upload(packedFuelInlet.data(),fuelBytes);
				id<MTLBuffer> inflowUpload=upload(packedPressureInflow.data(),inflowBytes);
				id<MTLBuffer> thermoUpload=upload(packedThermochemistry.data(),thermoBytes);
				id<MTLBuffer> eosThermoUpload=upload(packedEOSThermochemistry.data(),eosThermoBytes);
				id<MTLBuffer> transportUpload=upload(packedTransport.data(),transportBytes);
				id<MTLBuffer> ambientUpload=upload(request.physicalFlux.ambient.data(),ambientBytes);
				id<MTLBuffer> physicalBasisUpload=upload(physicalBasis.data(),physicalBasisBytes);
				id<MTLBuffer> advectiveBasisUpload=upload(request.physicalFlux.nullspaceBasis.data(),advectiveBasisBytes);
				id<MTLBuffer> projectorUpload=upload(request.physicalFlux.coordinateProjector.data(),projectorBytes);
				id<MTLBuffer> transportParameterUpload=upload(&transportParameters,sizeof(transportParameters));
				id<MTLBuffer> physicalParameterUpload=upload(&physicalParameters,sizeof(physicalParameters));
				id<MTLBuffer> eosParameterUpload=upload(&deviceEOSParameters,sizeof(deviceEOSParameters));
				id<MTLBuffer> candidateEOSParameterUpload=splitMetadata?
					upload(&candidateEOSParameters,sizeof(candidateEOSParameters)):nil;
				id<MTLBuffer> sourceUpload=upload(request.sourceDelta.data(),stateBytes);
				id<MTLBuffer> enthalpyUpload=upload(enthalpyBounds.data(),enthalpyBytes);
				id<MTLBuffer> affineUpload=upload(affine.data(),affineBytes);
				id<MTLBuffer> fctParameterUpload=upload(&fctParameters,sizeof(fctParameters));
				std::array<std::uint32_t,4> zeros={{0u,0u,0u,0u}};
				id<MTLBuffer> controlUpload=upload(zeros.data(),controlBytes);
				const std::size_t privateInflowBytes=request.physicalFlux.qualificationShortInflowSurface?
					inflowBytes-1u:inflowBytes;
				const std::size_t privatePhysicalBasisBytes=
					request.physicalFlux.qualificationOversizedPhysicalBasisSurface?
					physicalBasisBytes+sizeof(float):physicalBasisBytes;
				id<MTLBuffer> state=privateBuffer(stateBytes),temperature=privateBuffer(temperatureBytes),
					velocity=privateBuffer(velocityBytes),fuel=privateBuffer(fuelBytes),
					inflow=privateBuffer(privateInflowBytes),thermo=privateBuffer(thermoBytes),
					eosThermo=privateBuffer(eosThermoBytes),
					transport=privateBuffer(transportBytes),ambient=privateBuffer(ambientBytes),
					physicalBasisBuffer=privateBuffer(privatePhysicalBasisBytes),
					advectiveBasis=privateBuffer(advectiveBasisBytes),projector=privateBuffer(projectorBytes),
					transportParameter=privateBuffer(sizeof(transportParameters)),
					physicalParameter=privateBuffer(sizeof(physicalParameters)),
					eosParameter=privateBuffer(sizeof(eosParameters)),
					candidateEOSParameter=splitMetadata?privateBuffer(sizeof(candidateEOSParameters)):nil,
					source=privateBuffer(stateBytes),enthalpy=privateBuffer(enthalpyBytes),
					affineBuffer=privateBuffer(affineBytes),
					fctParameter=privateBuffer(sizeof(fctParameters)),
					failure=privateBuffer(sizeof(std::uint32_t)),
					transportObligations=privateBuffer(sizeof(std::uint32_t)),
					physicalObligations=privateBuffer(sizeof(std::uint32_t)),
					eosObligations=privateBuffer(sizeof(std::uint32_t));
				id<MTLBuffer> mismatchedState=request.physicalFlux.qualificationMismatchedParentCandidate?
					privateBuffer(stateBytes):nil;
				id<MTLBuffer> mismatchedEOSThermo=request.qualificationMismatchedEOSThermochemistry?
					privateBuffer(eosThermoBytes):nil;
				id<MTLBuffer> terminal=[context.device newBufferWithLength:terminalBytes
					options:MTLResourceStorageModeShared];
				const std::array<id<MTLBuffer>,44> buffers={{stateUpload,temperatureUpload,velocityUpload,
					fuelUpload,inflowUpload,thermoUpload,eosThermoUpload,transportUpload,ambientUpload,physicalBasisUpload,
					advectiveBasisUpload,projectorUpload,transportParameterUpload,physicalParameterUpload,
					eosParameterUpload,sourceUpload,enthalpyUpload,affineUpload,fctParameterUpload,
					controlUpload,state,temperature,velocity,fuel,inflow,
					thermo,transport,ambient,physicalBasisBuffer,advectiveBasis,projector,transportParameter,
					physicalParameter,eosParameter,source,enthalpy,affineBuffer,fctParameter,eosThermo,failure,transportObligations,
					physicalObligations,eosObligations,terminal}};
				for(id<MTLBuffer> buffer:buffers)if(!buffer){
					if(error)*error="production resident EOS buffer allocation failed";
					return false;
				}
				if(splitMetadata&&(!candidateEOSParameterUpload||!candidateEOSParameter)){
					if(error)*error="production resident EOS split-metadata RED allocation failed";
					return false;
				}
				id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context.queue);
				id<MTLBlitCommandEncoder> blit=command?[command blitCommandEncoder]:nil;
				if(!blit){if(error)*error="production resident EOS upload encoder failed";return false;}
				auto copy=[&](id<MTLBuffer> source,id<MTLBuffer> destination,const std::size_t length){
					[blit copyFromBuffer:source sourceOffset:0 toBuffer:destination destinationOffset:0 size:length];};
				copy(stateUpload,state,stateBytes);copy(temperatureUpload,temperature,temperatureBytes);
				copy(velocityUpload,velocity,velocityBytes);copy(fuelUpload,fuel,fuelBytes);
				copy(inflowUpload,inflow,privateInflowBytes);copy(thermoUpload,thermo,thermoBytes);
				copy(eosThermoUpload,eosThermo,eosThermoBytes);
				copy(transportUpload,transport,transportBytes);copy(ambientUpload,ambient,ambientBytes);
				copy(physicalBasisUpload,physicalBasisBuffer,physicalBasisBytes);
				copy(advectiveBasisUpload,advectiveBasis,advectiveBasisBytes);
				copy(projectorUpload,projector,projectorBytes);
				copy(transportParameterUpload,transportParameter,sizeof(transportParameters));
				copy(physicalParameterUpload,physicalParameter,sizeof(physicalParameters));
				copy(eosParameterUpload,eosParameter,sizeof(eosParameters));
				if(splitMetadata)copy(candidateEOSParameterUpload,candidateEOSParameter,
					sizeof(candidateEOSParameters));
				copy(sourceUpload,source,stateBytes);copy(enthalpyUpload,enthalpy,enthalpyBytes);
				copy(affineUpload,affineBuffer,affineBytes);
				copy(fctParameterUpload,fctParameter,sizeof(fctParameters));
				copy(controlUpload,failure,sizeof(std::uint32_t));
				[blit copyFromBuffer:controlUpload sourceOffset:sizeof(std::uint32_t)
					toBuffer:transportObligations destinationOffset:0 size:sizeof(std::uint32_t)];
				[blit copyFromBuffer:controlUpload sourceOffset:2u*sizeof(std::uint32_t)
					toBuffer:physicalObligations destinationOffset:0 size:sizeof(std::uint32_t)];
				[blit copyFromBuffer:controlUpload sourceOffset:3u*sizeof(std::uint32_t)
					toBuffer:eosObligations destinationOffset:0 size:sizeof(std::uint32_t)];
				if(mismatchedState)copy(stateUpload,mismatchedState,stateBytes);[blit endEncoding];
				ResidentTransportMetalAuthority transportAuthority;
				if(!EncodeResidentTransportAuthority(context,command,state,temperature,velocity,thermo,
					transport,fuel,transportParameter,failure,transportObligations,cells,allFaces,
					packedFuelInlet.size(),transportAuthority,error))return false;
				ResidentPhysicalFluxMetalAuthority physicalAuthority;
				ResidentEndpointClassMetalAuthority endpointClassAuthority;
				if(!EncodeResidentEndpointClassAuthority(context,command,inflow,
					transportAuthority.publicationIdentity,transportParameter,failure,
					transportAuthority,cells,allFaces,packedPressureInflow.size(),false,
					nullptr,endpointClassAuthority,error))return false;
				id<MTLBuffer> childState=mismatchedState?mismatchedState:state;
				if(!EncodeResidentPhysicalFluxAuthority(context,command,childState,temperature,velocity,
					thermo,ambient,physicalBasisBuffer,advectiveBasis,projector,transportParameter,
					physicalParameter,failure,physicalObligations,transportAuthority,
					endpointClassAuthority,cells,allFaces,physicalParameters,physicalAuthority,error))return false;
				ResidentEOSCandidateMetalAuthority candidateAuthority;
				id<MTLBuffer> forbiddenCPUCandidate=request.qualificationCPUProducedCandidate?
					stateUpload:nil;
				if(!EncodeResidentEOSQualificationCandidate(context,command,forbiddenCPUCandidate,
					source,enthalpy,affineBuffer,fctParameter,transportParameter,eosThermo,
					splitMetadata?candidateEOSParameter:eosParameter,failure,eosObligations,transportAuthority,
					physicalAuthority,eosParameters,
					request.qualificationUnsealedParentFlux,request.qualificationMismatchedParentFlux,
					request.qualificationCPUProducedCandidate,request.qualificationShortCandidateSurface,
					candidateAuthority,error))return false;
				ResidentEOSMetalAuthority eosAuthority;
				if(!EncodeResidentEOSAuthority(context,command,thermo,
					mismatchedEOSThermo?mismatchedEOSThermo:eosThermo,eosParameter,failure,eosObligations,
					transportAuthority,physicalAuthority,candidateAuthority,eosParameters,eosAuthority,error))
					return false;
				blit=[command blitCommandEncoder];
				if(!blit){
					if(error)*error="production resident EOS terminal encoder failed";
					return false;
				}
				[blit copyFromBuffer:candidateAuthority.conservative sourceOffset:0 toBuffer:terminal
					destinationOffset:0 size:stateBytes];
				[blit copyFromBuffer:eosAuthority.temperature sourceOffset:0 toBuffer:terminal
					destinationOffset:stateBytes size:fieldBytes];
				[blit copyFromBuffer:eosAuthority.representedPressureRatio sourceOffset:0 toBuffer:terminal
					destinationOffset:stateBytes+fieldBytes size:fieldBytes];
				[blit copyFromBuffer:eosAuthority.absoluteDeviation sourceOffset:0 toBuffer:terminal
					destinationOffset:stateBytes+2u*fieldBytes size:fieldBytes];
				[blit copyFromBuffer:candidateAuthority.faceAlpha sourceOffset:0 toBuffer:terminal
					destinationOffset:alphaOffset size:allFaces*sizeof(float)];
				const std::array<id<MTLBuffer>,4> identities={{transportAuthority.publicationIdentity,
					physicalAuthority.publicationIdentity,candidateAuthority.publicationIdentity,
					eosAuthority.publicationIdentity}};
				for(unsigned int index=0u;index<identities.size();++index)
					[blit copyFromBuffer:identities[index] sourceOffset:0 toBuffer:terminal
						destinationOffset:identityOffset+index*sizeof(std::uint64_t)
						size:sizeof(std::uint64_t)];
				[blit copyFromBuffer:failure sourceOffset:0 toBuffer:terminal
					destinationOffset:controlOffset size:sizeof(std::uint32_t)];
				[blit copyFromBuffer:eosObligations sourceOffset:0 toBuffer:terminal
					destinationOffset:controlOffset+sizeof(std::uint32_t) size:sizeof(std::uint32_t)];
				[blit copyFromBuffer:eosAuthority.firstFailureCell sourceOffset:0 toBuffer:terminal
					destinationOffset:controlOffset+2u*sizeof(std::uint32_t) size:sizeof(std::uint32_t)];
				[blit copyFromBuffer:eosAuthority.failureTerm sourceOffset:0 toBuffer:terminal
					destinationOffset:failureMapOffset size:fieldBytes];
				[blit endEncoding];result.deviceAttempted=true;CommitTrackedMetalCommand(command);
				result.commandCommitCount=1u;[command waitUntilCompleted];
				if([command status]!=MTLCommandBufferStatusCompleted){
					if(error)*error="production resident EOS command failed";
					return false;
				}
				const unsigned char* bytes=static_cast<const unsigned char*>(ReadTrackedMetalBuffer(terminal));
				if(!bytes){if(error)*error="production resident EOS terminal is unavailable";return false;}
				result.terminalRead=true;
				const std::uint32_t* controls=reinterpret_cast<const std::uint32_t*>(bytes+controlOffset);
				FireProductionResidentEOSCandidateComparatorResult computed;
				const std::uint64_t* identityValues=reinterpret_cast<const std::uint64_t*>(bytes+identityOffset);
				computed.transportPublicationIdentity=identityValues[0];
				computed.physicalFluxPublicationIdentity=identityValues[1];
				computed.candidatePublicationIdentity=identityValues[2];
				computed.EOSPublicationIdentity=identityValues[3];
				computed.deviceAttempted=true;computed.terminalRead=true;
				computed.deviceFailureBitmap=controls[0];
				computed.firstEOSFailureCell=controls[2];
				const std::uint32_t* failureTerms=
					reinterpret_cast<const std::uint32_t*>(bytes+failureMapOffset);
				if(computed.firstEOSFailureCell<cells){
					computed.firstEOSFailureTermBitmap=failureTerms[computed.firstEOSFailureCell];
					for(std::size_t cell=computed.firstEOSFailureCell+1u;cell<cells;++cell){
						if(failureTerms[cell]==0u)continue;
						computed.secondEOSFailureCell=static_cast<std::uint32_t>(cell);
						computed.secondEOSFailureTermBitmap=failureTerms[cell];break;
					}
				}
				computed.commandCommitCount=static_cast<std::uint32_t>(
					MetalCommandCommitCount-beginningCommits);
				computed.terminalStagingCount=static_cast<std::uint32_t>(
					MetalHostBufferReadCount-beginningReads);
				if(controls[0]!=0u){result=std::move(computed);
					if(error)*error="production resident EOS device validation failed";return false;}
				const float* values=reinterpret_cast<const float*>(bytes);
				computed.candidateConservativeValues.assign(values,values+9u*cells);
				computed.temperatureK.assign(values+9u*cells,values+10u*cells);
				computed.representedPressureRatio.assign(values+10u*cells,values+11u*cells);
				computed.absoluteEOSDeviation.assign(values+11u*cells,values+12u*cells);
				const float* alpha=reinterpret_cast<const float*>(bytes+alphaOffset);
				for(unsigned int axis=0u;axis<3u;++axis)computed.sharedFaceAlpha[axis].assign(
					alpha+faceOffset[axis],alpha+faceOffset[axis]+FireProductionProjectionFaceCount(shape,axis));
				computed.producingStage=request.producingStage;
				computed.producerPrecision=request.producerPrecision;
				computed.branchObligationBitmap=controls[1];
				const std::uint64_t observedCommits=MetalCommandCommitCount-beginningCommits,
					observedReads=MetalHostBufferReadCount-beginningReads;
				computed.commandCommitCount=static_cast<std::uint32_t>(observedCommits);
				computed.terminalStagingCount=static_cast<std::uint32_t>(observedReads);
				computed.interstageFullGridTransferCount=static_cast<std::uint32_t>(
					observedReads>0u?observedReads-1u:0u);computed.certifiedWorkingSetBytes=certified;
				computed.actualMetalAllocationBytes=transportAuthority.allocationBytes+
					physicalAuthority.allocationBytes+candidateAuthority.allocationBytes+
					eosAuthority.allocationBytes;
				computed.liveAuthorityAllocationBytes=candidateAuthority.allocationBytes+
					eosAuthority.allocationBytes;
				for(id<MTLBuffer> buffer:buffers)computed.actualMetalAllocationBytes+=[buffer allocatedSize];
				if(mismatchedState)computed.actualMetalAllocationBytes+=[mismatchedState allocatedSize];
				if(mismatchedEOSThermo)computed.actualMetalAllocationBytes+=[mismatchedEOSThermo allocatedSize];
				computed.deviceElapsedMS=([command GPUEndTime]-[command GPUStartTime])*1000.0;
				computed.deviceProduced=computed.transportPublicationIdentity!=0u&&
					computed.physicalFluxPublicationIdentity!=0u&&
					computed.candidatePublicationIdentity!=0u&&computed.EOSPublicationIdentity!=0u;
				if(observedCommits!=1u||observedReads!=1u||!computed.deviceProduced||
					computed.actualMetalAllocationBytes>computed.certifiedWorkingSetBytes||
					!AllFinite(computed.candidateConservativeValues)||!AllFinite(computed.temperatureK)||
					!AllFinite(computed.representedPressureRatio)||
					!AllFinite(computed.absoluteEOSDeviation)){
					if(error)*error="production resident EOS publication failed its certificate";
					return false;
				}
				result=std::move(computed);if(error)error->clear();return true;
			}
		} catch(const std::bad_alloc&){
			result=FireProductionResidentEOSCandidateComparatorResult();
			if(error)try{*error="production resident EOS allocation failed";}
			catch(const std::bad_alloc&){}
			return false;
		}
	}

	bool EvaluateFireProductionResidentTargetLineageMetalComparator(
		const FireProductionResidentTargetLineageComparatorRequest& request,
		FireProductionResidentTargetLineageComparatorResult& result,std::string* error )
	{
		result=FireProductionResidentTargetLineageComparatorResult();
		try {
			MetalResidentTransportParameters transportParameters={};
			MetalResidentPhysicalFluxParameters physicalParameters={};
			MetalResidentEOSParameters eosParameters={};
			std::array<std::size_t,3> faceOffset={{}};std::size_t allFaces=0u;
			std::vector<unsigned char> packedFuelInlet,packedPressureInflow;
			std::vector<float> physicalBasis;
			if(!PrepareResidentEOSCandidateRequest(request.eos,transportParameters,
				physicalParameters,eosParameters,faceOffset,allFaces,packedFuelInlet,
				packedPressureInflow,physicalBasis,error))return false;
			if(request.qualificationExactPositiveTailThreshold)eosParameters.padding[0]|=64u;
			if(request.qualificationExactNegativeTailThreshold)eosParameters.padding[0]|=128u;
			const FireProductionProjectionShape& shape=request.eos.physicalFlux.transport.shape;
			const std::size_t cells=shape.CellCount();
			const FireProductionFrozenSourcePacketSeal& frozenSource=request.frozenSource;
			const FireProductionProjectionShape& sourceShape=frozenSource.Shape();
			const FireSimulationMethaneRecord& methane=FireSimulationMethaneRecord::PhysicalV1();
			RISE::FireCase::RecordV1 targetCase;std::string caseError;
			if(!FireProductionFrozenSourcePacketSealMatches(frozenSource,error)||
				!FireProductionFrozenSourcePacketSealMatchesBeginningState(frozenSource,shape,
					request.eos.physicalFlux.transport.conservativeValues,
					request.eos.physicalFlux.transport.temperatureK,error)||
				!RISE::FireCase::ValidateMethaneEnvelopeV1(request.eos.caseRecordEnvelope,
					methane,targetCase,caseError)||
				frozenSource.CaseRecordId()!=targetCase.caseRecordId||
				frozenSource.MethaneRecordId()!=methane.RecordId()||
				sourceShape.nx!=shape.nx||sourceShape.ny!=shape.ny||sourceShape.nz!=shape.nz||
				sourceShape.cellWidthM!=shape.cellWidthM||
				frozenSource.TimeStepS()!=request.eos.candidateTimeStepS||
				frozenSource.AttemptIdentity()!=request.eos.physicalFlux.transport.attemptIdentity||
				frozenSource.SourceDelta().size()!=request.eos.sourceDelta.size()||
				std::memcmp(frozenSource.SourceDelta().data(),request.eos.sourceDelta.data(),
					request.eos.sourceDelta.size()*sizeof(float))!=0){
				if(error)*error="production resident target frozen-source authority is invalid";
				return false;
			}
			for(const float value:frozenSource.DivergenceTargetPerS())
				if(!std::isfinite(value)||(value==0.0f&&std::signbit(value))){
					if(error)*error="production resident target frozen-source term is noncanonical";
					return false;
				}
			std::array<float,MetalResidentTransportSpeciesCount*
				MetalResidentTransportSpeciesStride> packedTransport;
			if(!PackMetalResidentTransport(packedTransport,transportParameters,error))return false;
			std::array<float,MetalManifoldCertificateValues> packedThermochemistry;
			std::array<float,MetalEOSThermochemistryValues> packedEOSThermochemistry;
			MetalManifoldParameters manifoldParameters={};std::array<double,7> lower,upper;
			if(!PackMetalMethaneThermochemistry(packedThermochemistry,manifoldParameters,
				lower,upper,error)||!PackMetalEOSDoubleDoubleThermochemistry(
					packedEOSThermochemistry,error))return false;
			std::vector<float> fctBasis,fctProjector,affine;
			std::array<float,14> enthalpyBounds={{}};float feasibility=0.0f,assemblyReserve=0.0f;
			if(!PackMetalSingleStageFCTCertificate(fctBasis,fctProjector,enthalpyBounds,affine,
				feasibility,assemblyReserve,error))return false;
			MetalSingleStageFCTParameters fctParameters={static_cast<std::uint32_t>(shape.nx),
				static_cast<std::uint32_t>(shape.ny),static_cast<std::uint32_t>(shape.nz),
				static_cast<std::uint32_t>(cells),9u,11u,
				static_cast<std::uint32_t>(request.eos.physicalFlux.nullity),
				static_cast<std::uint32_t>(affine.size()/8u),{},{},shape.cellWidthM,
				request.eos.candidateTimeStepS,feasibility,assemblyReserve};
			std::size_t fctSideOffset=0u;for(unsigned int side=0u;side<6u;++side){
				fctParameters.boundary[side]=static_cast<std::uint32_t>(
					request.eos.physicalFlux.transport.boundary[side]);
				fctParameters.sideOffset[side]=static_cast<std::uint32_t>(fctSideOffset);
				fctSideOffset+=request.eos.physicalFlux.pressureOpenInflow[side].size();}
			MetalResidentTargetParameters targetParameters={static_cast<std::uint32_t>(shape.nx),
				static_cast<std::uint32_t>(shape.ny),static_cast<std::uint32_t>(shape.nz),
				static_cast<std::uint32_t>(cells),{},{},shape.cellWidthM,
				request.eos.candidateTimeStepS,
				request.qualificationAlternateDormantTailThreshold?
					std::nextafter(0x1p-4f,std::numeric_limits<float>::infinity()):0x1p-4f,
				request.qualificationPreauthoredProjectionTarget?1u:0u,
				FireProductionMonitoredManifoldPolicy::IdentityVersion,{0u,0u},
				request.eos.physicalFlux.transport.attemptIdentity,frozenSource.PacketIdentity()};
			for(unsigned int axis=0u;axis<3u;++axis)
				targetParameters.faceOffset[axis]=static_cast<std::uint32_t>(faceOffset[axis]);
			for(unsigned int side=0u;side<6u;++side)targetParameters.boundary[side]=
				static_cast<std::uint32_t>(request.eos.physicalFlux.transport.boundary[side]);
			const MetalResidentTargetParameters sourceParameters=targetParameters;
			if(request.qualificationCertifiedContinuousEnclosure)
				targetParameters.padding[0]=1u;
			if(request.qualificationCorruptContinuousEnclosurePublication)
				targetParameters.padding[1]=1u;
			if(request.qualificationMismatchedFrozenSourcePacket)
				targetParameters.sourcePacketIdentity+=1u;
			switch(request.qualificationStaleTargetMetadataField){
				case 0u:break;
				case 1u:targetParameters.nx+=1u;break;
				case 2u:targetParameters.faceOffset[1]+=1u;break;
				case 3u:targetParameters.cellWidthM=std::nextafter(targetParameters.cellWidthM,
					std::numeric_limits<float>::infinity());break;
				case 4u:targetParameters.timeStepS=std::nextafter(targetParameters.timeStepS,
					std::numeric_limits<float>::infinity());break;
				case 5u:targetParameters.attemptIdentity+=1u;break;
				case 6u:targetParameters.boundary[0]=targetParameters.boundary[0]==
					static_cast<std::uint32_t>(FireProductionProjectionWall)?
					static_cast<std::uint32_t>(FireProductionProjectionPressureOpen):
					static_cast<std::uint32_t>(FireProductionProjectionWall);break;
				default:if(error)*error="production resident target metadata mutation is invalid";
					return false;
			}
			MetalResidentProjectionConsumerParameters projectionParameters={
				static_cast<std::uint32_t>(shape.nx),static_cast<std::uint32_t>(shape.ny),
				static_cast<std::uint32_t>(shape.nz),static_cast<std::uint32_t>(cells),{},
				{0u,0u},shape.cellWidthM,request.eos.candidateTimeStepS,
				request.eos.physicalFlux.transport.attemptIdentity};
			for(unsigned int side=0u;side<6u;++side)projectionParameters.boundary[side]=
				targetParameters.boundary[side];
			if(request.qualificationMismatchedProjectionTopology)
				projectionParameters.boundary[0]=projectionParameters.boundary[0]==
					static_cast<std::uint32_t>(FireProductionProjectionWall)?
					static_cast<std::uint32_t>(FireProductionProjectionPressureOpen):
					static_cast<std::uint32_t>(FireProductionProjectionWall);
			std::vector<float> packedVelocity;packedVelocity.reserve(allFaces);
			for(const std::vector<float>& axis:request.eos.physicalFlux.transport.projectedVelocityMPerS)
				packedVelocity.insert(packedVelocity.end(),axis.begin(),axis.end());
			ResidentTransportMetalContext& context=ResidentTransportContext();
			if(!context.Valid()){if(error)*error=context.error;return false;}
			const std::size_t stateBytes=9u*cells*sizeof(float),fieldBytes=cells*sizeof(float),
				velocityBytes=allFaces*sizeof(float),fuelBytes=packedFuelInlet.size(),
				inflowBytes=packedPressureInflow.size(),
				thermoBytes=packedThermochemistry.size()*sizeof(float),
				eosThermoBytes=packedEOSThermochemistry.size()*sizeof(float),
				transportBytes=packedTransport.size()*sizeof(float),ambientBytes=9u*sizeof(float),
				physicalBasisBytes=physicalBasis.size()*sizeof(float),
				advectiveBasisBytes=request.eos.physicalFlux.nullspaceBasis.size()*sizeof(float),
				projectorBytes=request.eos.physicalFlux.coordinateProjector.size()*sizeof(float),
				enthalpyBytes=enthalpyBounds.size()*sizeof(float),affineBytes=affine.size()*sizeof(float);
			const std::size_t identityOffset=7u*fieldBytes,
				controlOffset=identityOffset+8u*sizeof(std::uint64_t),
				terminalBytes=controlOffset+2u*sizeof(std::uint32_t);
			std::uint64_t certified=0u;
			if(!FireProductionResidentTargetLineageMetalWorkingSetBytes(shape,certified)){
				if(error)*error="production resident target working-set certificate failed";return false;}
			if(certified>request.qualificationWorkingSetLimitBytes){
				if(error)*error="production resident target working-set limit is understated";
				return false;
			}
			const std::uint64_t beginningCommits=MetalCommandCommitCount,
				beginningReads=MetalHostBufferReadCount;
			@autoreleasepool {
				auto upload=[&](const void* bytes,const std::size_t length){return
					[context.device newBufferWithBytes:bytes length:length options:MTLResourceStorageModeShared];};
				auto privateBuffer=[&](const std::size_t length){return
					[context.device newBufferWithLength:length options:MTLResourceStorageModePrivate];};
				const FireProductionResidentPhysicalFluxComparatorRequest& flux=request.eos.physicalFlux;
				id<MTLBuffer> stateUpload=upload(flux.transport.conservativeValues.data(),stateBytes),
					temperatureUpload=upload(flux.transport.temperatureK.data(),fieldBytes),
					velocityUpload=upload(packedVelocity.data(),velocityBytes),
					fuelUpload=upload(packedFuelInlet.data(),fuelBytes),
					inflowUpload=upload(packedPressureInflow.data(),inflowBytes),
					thermoUpload=upload(packedThermochemistry.data(),thermoBytes),
					eosThermoUpload=upload(packedEOSThermochemistry.data(),eosThermoBytes),
					transportUpload=upload(packedTransport.data(),transportBytes),
					ambientUpload=upload(flux.ambient.data(),ambientBytes),
					physicalBasisUpload=upload(physicalBasis.data(),physicalBasisBytes),
					advectiveBasisUpload=upload(flux.nullspaceBasis.data(),advectiveBasisBytes),
					projectorUpload=upload(flux.coordinateProjector.data(),projectorBytes),
					transportParameterUpload=upload(&transportParameters,sizeof(transportParameters)),
					physicalParameterUpload=upload(&physicalParameters,sizeof(physicalParameters)),
					eosParameterUpload=upload(&eosParameters,sizeof(eosParameters)),
					targetParameterUpload=upload(&targetParameters,sizeof(targetParameters)),
					projectionParameterUpload=upload(&projectionParameters,sizeof(projectionParameters)),
					sourceDeltaUpload=upload(request.eos.sourceDelta.data(),stateBytes),
					enthalpyUpload=upload(enthalpyBounds.data(),enthalpyBytes),
					affineUpload=upload(affine.data(),affineBytes),
					fctParameterUpload=upload(&fctParameters,sizeof(fctParameters));
				std::array<std::uint32_t,5> zeros={{0u,0u,0u,0u,0u}};
				id<MTLBuffer> controlUpload=upload(zeros.data(),zeros.size()*sizeof(std::uint32_t));
				id<MTLBuffer> state=privateBuffer(stateBytes),temperature=privateBuffer(fieldBytes),
					velocity=privateBuffer(velocityBytes),fuel=privateBuffer(fuelBytes),
					inflow=privateBuffer(inflowBytes),thermo=privateBuffer(thermoBytes),
					eosThermo=privateBuffer(eosThermoBytes),eosThermoAlternate=privateBuffer(eosThermoBytes),
					transport=privateBuffer(transportBytes),
					ambient=privateBuffer(ambientBytes),physicalBasisBuffer=privateBuffer(physicalBasisBytes),
					advectiveBasis=privateBuffer(advectiveBasisBytes),projector=privateBuffer(projectorBytes),
					transportParameter=privateBuffer(sizeof(transportParameters)),
					physicalParameter=privateBuffer(sizeof(physicalParameters)),
					eosParameter=privateBuffer(sizeof(eosParameters)),
					targetParameter=privateBuffer(sizeof(targetParameters)),
					projectionParameter=privateBuffer(sizeof(projectionParameters)),
					sourceDelta=privateBuffer(stateBytes),
					sourceSubstitution=privateBuffer(fieldBytes),enthalpy=privateBuffer(enthalpyBytes),
					affineBuffer=privateBuffer(affineBytes),fctParameter=privateBuffer(sizeof(fctParameters)),
					failure=privateBuffer(sizeof(std::uint32_t)),
					transportObligations=privateBuffer(sizeof(std::uint32_t)),
					physicalObligations=privateBuffer(sizeof(std::uint32_t)),
					eosObligations=privateBuffer(sizeof(std::uint32_t)),
					targetObligations=privateBuffer(sizeof(std::uint32_t));
				id<MTLBuffer> terminal=[context.device newBufferWithLength:terminalBytes
					options:MTLResourceStorageModeShared],
					interstageTransferMutation=[context.device newBufferWithLength:fieldBytes
					options:MTLResourceStorageModeShared];
				const std::array<id<MTLBuffer>,52> buffers={{stateUpload,temperatureUpload,velocityUpload,
					fuelUpload,inflowUpload,thermoUpload,eosThermoUpload,transportUpload,ambientUpload,
					physicalBasisUpload,advectiveBasisUpload,projectorUpload,transportParameterUpload,
					physicalParameterUpload,eosParameterUpload,targetParameterUpload,
					projectionParameterUpload,sourceDeltaUpload,
					enthalpyUpload,affineUpload,fctParameterUpload,controlUpload,state,
					temperature,velocity,fuel,inflow,thermo,eosThermo,eosThermoAlternate,transport,ambient,
					physicalBasisBuffer,advectiveBasis,projector,transportParameter,physicalParameter,
					eosParameter,targetParameter,
					projectionParameter,
					sourceDelta,sourceSubstitution,enthalpy,affineBuffer,fctParameter,failure,transportObligations,
					physicalObligations,eosObligations,targetObligations,terminal,
					interstageTransferMutation}};
				for(id<MTLBuffer> buffer:buffers)if(!buffer){
					if(error)*error="production resident target buffer allocation failed";
					return false;
				}
				id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context.queue);
				id<MTLBlitCommandEncoder> blit=command?[command blitCommandEncoder]:nil;
				if(!blit){if(error)*error="production resident target upload encoder failed";return false;}
				enum class TransferPurpose { Internal,Terminal,InterstageFullGrid };
				std::uint32_t interstageFullGridTransfers=0u,terminalStagingDestinations=0u;
				id<MTLBuffer> terminalDestination=nil;
				auto copy=[&](id<MTLBuffer> source,id<MTLBuffer> destination,
					const std::size_t length,const TransferPurpose purpose=TransferPurpose::Internal,
					const std::size_t destinationOffset=0u){
					if([source storageMode]==MTLStorageModePrivate&&
						[destination storageMode]==MTLStorageModeShared){
						if(purpose==TransferPurpose::Terminal){
							if(destination!=terminalDestination){terminalDestination=destination;
								++terminalStagingDestinations;}
						}else if(length>=fieldBytes)++interstageFullGridTransfers;
					}
					[blit copyFromBuffer:source sourceOffset:0 toBuffer:destination
						destinationOffset:destinationOffset size:length];};
				copy(stateUpload,state,stateBytes);copy(temperatureUpload,temperature,fieldBytes);
				copy(velocityUpload,velocity,velocityBytes);copy(fuelUpload,fuel,fuelBytes);
				copy(inflowUpload,inflow,inflowBytes);copy(thermoUpload,thermo,thermoBytes);
				copy(eosThermoUpload,eosThermo,eosThermoBytes);
				copy(eosThermoUpload,eosThermoAlternate,eosThermoBytes);
				copy(transportUpload,transport,transportBytes);
				copy(ambientUpload,ambient,ambientBytes);copy(physicalBasisUpload,physicalBasisBuffer,physicalBasisBytes);
				copy(advectiveBasisUpload,advectiveBasis,advectiveBasisBytes);copy(projectorUpload,projector,projectorBytes);
				copy(transportParameterUpload,transportParameter,sizeof(transportParameters));
				copy(physicalParameterUpload,physicalParameter,sizeof(physicalParameters));
				copy(eosParameterUpload,eosParameter,sizeof(eosParameters));
				copy(targetParameterUpload,targetParameter,sizeof(targetParameters));
				copy(projectionParameterUpload,projectionParameter,sizeof(projectionParameters));
				copy(sourceDeltaUpload,sourceDelta,stateBytes);
				copy(enthalpyUpload,enthalpy,enthalpyBytes);copy(affineUpload,affineBuffer,affineBytes);
				copy(fctParameterUpload,fctParameter,sizeof(fctParameters));
				copy(controlUpload,failure,sizeof(std::uint32_t));
				copy(controlUpload,transportObligations,sizeof(std::uint32_t));
				copy(controlUpload,physicalObligations,sizeof(std::uint32_t));
				copy(controlUpload,eosObligations,sizeof(std::uint32_t));
				copy(controlUpload,targetObligations,sizeof(std::uint32_t));[blit endEncoding];
				ResidentTransportMetalAuthority transportAuthority;
				if(!EncodeResidentTransportAuthority(context,command,state,temperature,velocity,thermo,
					transport,fuel,transportParameter,failure,transportObligations,cells,allFaces,
					packedFuelInlet.size(),transportAuthority,error))return false;
				ResidentPhysicalFluxMetalAuthority physicalAuthority;
				ResidentEndpointClassMetalAuthority endpointClassAuthority;
				if(!EncodeResidentEndpointClassAuthority(context,command,inflow,
					transportAuthority.publicationIdentity,transportParameter,failure,
					transportAuthority,cells,allFaces,packedPressureInflow.size(),false,
					nullptr,endpointClassAuthority,error))return false;
				if(!EncodeResidentPhysicalFluxAuthority(context,command,state,temperature,velocity,thermo,
					ambient,physicalBasisBuffer,advectiveBasis,projector,transportParameter,
					physicalParameter,failure,physicalObligations,transportAuthority,
					endpointClassAuthority,cells,allFaces,
					physicalParameters,physicalAuthority,error))return false;
				ResidentEOSCandidateMetalAuthority candidateAuthority;
				if(!EncodeResidentEOSQualificationCandidate(context,command,nil,sourceDelta,enthalpy,
					affineBuffer,fctParameter,transportParameter,eosThermo,eosParameter,failure,
					eosObligations,transportAuthority,physicalAuthority,eosParameters,false,false,false,
					false,candidateAuthority,error))return false;
				ResidentEOSMetalAuthority eosAuthority;
				if(!EncodeResidentEOSAuthority(context,command,thermo,eosThermo,eosParameter,failure,
					eosObligations,transportAuthority,physicalAuthority,candidateAuthority,eosParameters,
					eosAuthority,error))return false;
				ResidentFrozenSourceMetalAuthority sourceAuthority;
				if(!EncodeResidentFrozenSourceAuthority(context,command,failure,targetObligations,
					frozenSource,candidateAuthority,sourceParameters,nil,nil,sourceAuthority,error))
					return false;
				if(request.qualificationCPUProducedFrozenSource)
					sourceAuthority.values=sourceAuthority.inputUpload;
				if(request.qualificationCPUPrivateBlitFrozenSource){
					blit=[command blitCommandEncoder];if(!blit){
						if(error)*error="production resident source-substitution RED encoder failed";
						return false;}
					copy(sourceAuthority.inputUpload,sourceSubstitution,fieldBytes);[blit endEncoding];
					sourceAuthority.values=sourceSubstitution;
				}
				if(request.qualificationNonImmediateCandidate)
					candidateAuthority.parentPhysicalPublicationIdentity=transportAuthority.publicationIdentity;
				if(request.qualificationEOSAcceptedButUnlinked)
					eosAuthority.parentCandidatePublicationIdentity=physicalAuthority.publicationIdentity;
				if(request.qualificationUnsealedTransportParent||
					request.qualificationUnsealedPhysicalFluxParent||
					request.qualificationUnsealedEOSCandidateParent||
					request.qualificationUnsealedEOSParent||
					request.qualificationUnsealedFrozenSourceParent){
					blit=[command blitCommandEncoder];if(!blit){
						if(error)*error="production resident target unsealed-parent RED encoder failed";
						return false;
					}
					id<MTLBuffer> identity=request.qualificationUnsealedTransportParent?
						transportAuthority.publicationIdentity:(request.qualificationUnsealedPhysicalFluxParent?
							physicalAuthority.publicationIdentity:(request.qualificationUnsealedEOSCandidateParent?
								candidateAuthority.publicationIdentity:(request.qualificationUnsealedEOSParent?
									eosAuthority.publicationIdentity:sourceAuthority.publicationIdentity)));
					[blit fillBuffer:identity range:NSMakeRange(0,sizeof(std::uint64_t)) value:0u];[blit endEncoding];}
				ResidentProjectionTargetMetalAuthority targetAuthority;
				id<MTLBuffer> admittedEOSThermochemistry=
					request.qualificationMismatchedEOSThermochemistry?eosThermoAlternate:eosThermo;
				if(!EncodeResidentProjectionTargetAuthority(context,command,admittedEOSThermochemistry,
					targetParameter,projectionParameter,
					request.qualificationCPUProducedTarget?sourceAuthority.inputUpload:nil,
					request.qualificationCPUForgedProjectionMetadata?projectionParameterUpload:nil,
					failure,targetObligations,
					transportAuthority,physicalAuthority,physicalAuthority,
					candidateAuthority,eosAuthority,sourceAuthority,targetParameters,targetAuthority,error))return false;
				if(request.qualificationInjectInterstageFullGridTransfer){
					blit=[command blitCommandEncoder];if(!blit){
						if(error)*error="production resident target transfer RED encoder failed";
						return false;}
					copy(targetAuthority.assembled,interstageTransferMutation,fieldBytes,
						TransferPurpose::InterstageFullGrid);[blit endEncoding];
				}
				blit=[command blitCommandEncoder];if(!blit){
					if(error)*error="production resident target terminal encoder failed";
					return false;
				}
				const std::array<id<MTLBuffer>,7> fields={{targetAuthority.tangent,
					targetAuthority.frozenSource,targetAuthority.absoluteDiagnostic,
					targetAuthority.monitoredAbsolute,targetAuthority.assembled,
					targetAuthority.tangentEnclosure,targetAuthority.assembledEnclosure}};
				for(unsigned int field=0u;field<fields.size();++field)copy(fields[field],terminal,
					fieldBytes,TransferPurpose::Terminal,field*fieldBytes);
				const std::array<id<MTLBuffer>,8> identities={{transportAuthority.publicationIdentity,
					physicalAuthority.publicationIdentity,candidateAuthority.publicationIdentity,
					eosAuthority.publicationIdentity,sourceAuthority.publicationIdentity,
					targetAuthority.publicationIdentity,
					targetAuthority.projectionConsumerIdentity,
					targetAuthority.consumerIdentity}};
				for(unsigned int index=0u;index<identities.size();++index)copy(identities[index],terminal,
					sizeof(std::uint64_t),TransferPurpose::Terminal,identityOffset+
					index*sizeof(std::uint64_t));
				copy(failure,terminal,sizeof(std::uint32_t),TransferPurpose::Terminal,controlOffset);
				copy(targetObligations,terminal,sizeof(std::uint32_t),TransferPurpose::Terminal,
					controlOffset+sizeof(std::uint32_t));[blit endEncoding];
				result.deviceAttempted=true;CommitTrackedMetalCommand(command);[command waitUntilCompleted];
				result.commandCommitCount=1u;if([command status]!=MTLCommandBufferStatusCompleted){
					if(error)*error="production resident target command failed";return false;}
				const unsigned char* bytes=static_cast<const unsigned char*>(ReadTrackedMetalBuffer(terminal));
				if(!bytes){if(error)*error="production resident target terminal is unavailable";return false;}
				const std::uint32_t* controls=reinterpret_cast<const std::uint32_t*>(bytes+controlOffset);
				const std::uint64_t* identityValues=reinterpret_cast<const std::uint64_t*>(bytes+identityOffset);
				FireProductionResidentTargetLineageComparatorResult computed;computed.deviceAttempted=true;
				computed.terminalRead=true;computed.deviceFailureBitmap=controls[0];
				computed.branchObligationBitmap=controls[1];
				computed.transportPublicationIdentity=identityValues[0];
				computed.physicalFluxPublicationIdentity=identityValues[1];
				computed.candidatePublicationIdentity=identityValues[2];computed.EOSPublicationIdentity=identityValues[3];
				computed.frozenSourcePublicationIdentity=identityValues[4];
				computed.targetPublicationIdentity=identityValues[5];
				computed.projectionMetadataIdentity=identityValues[6];
				computed.projectionConsumerIdentity=identityValues[7];
				computed.commandCommitCount=static_cast<std::uint32_t>(MetalCommandCommitCount-beginningCommits);
				computed.terminalStagingCount=terminalStagingDestinations;
				computed.interstageFullGridTransferCount=interstageFullGridTransfers;
				computed.certifiedWorkingSetBytes=certified;
				computed.actualMetalAllocationBytes=transportAuthority.allocationBytes+
					physicalAuthority.allocationBytes+candidateAuthority.allocationBytes+
					eosAuthority.allocationBytes+sourceAuthority.allocationBytes+
					targetAuthority.allocationBytes;
				for(id<MTLBuffer> buffer:buffers)computed.actualMetalAllocationBytes+=[buffer allocatedSize];
				computed.liveAuthorityAllocationBytes=sourceAuthority.liveAllocationBytes+
					targetAuthority.allocationBytes;
				computed.deviceElapsedMS=([command GPUEndTime]-[command GPUStartTime])*1000.0;
				if(controls[0]!=0u){
					result=std::move(computed);
					if(error)*error="production resident target device validation failed";
					return false;
				}
				const float* values=reinterpret_cast<const float*>(bytes);
				computed.tangentTargetPerS.assign(values,values+cells);
				computed.frozenSourceTargetPerS.assign(values+cells,values+2u*cells);
				computed.absoluteReferenceDiagnosticPerS.assign(values+2u*cells,values+3u*cells);
				computed.monitoredAbsoluteReferenceTargetPerS.assign(values+3u*cells,values+4u*cells);
				computed.assembledTargetPerS.assign(values+4u*cells,values+5u*cells);
				computed.tangentEnclosurePerS.assign(values+5u*cells,values+6u*cells);
				computed.assembledEnclosurePerS.assign(values+6u*cells,values+7u*cells);
				computed.deviceProduced=computed.transportPublicationIdentity!=0u&&
					computed.physicalFluxPublicationIdentity!=0u&&computed.candidatePublicationIdentity!=0u&&
					computed.EOSPublicationIdentity!=0u&&computed.frozenSourcePublicationIdentity!=0u&&
					computed.targetPublicationIdentity!=0u&&
					computed.projectionMetadataIdentity!=0u&&
					computed.projectionConsumerIdentity!=0u;
				bool enclosureNonnegative=true;for(const float value:computed.tangentEnclosurePerS)
					enclosureNonnegative=enclosureNonnegative&&value>=0.0f;
				for(const float value:computed.assembledEnclosurePerS)
					enclosureNonnegative=enclosureNonnegative&&value>=0.0f;
				const std::uint64_t observedReads=MetalHostBufferReadCount-beginningReads;
				if(computed.commandCommitCount!=1u||observedReads!=1u||
					computed.terminalStagingCount!=1u||
					computed.interstageFullGridTransferCount!=0u||!computed.deviceProduced||
					computed.actualMetalAllocationBytes>computed.certifiedWorkingSetBytes||
					!AllFinite(computed.tangentTargetPerS)||!AllFinite(computed.frozenSourceTargetPerS)||
					!AllFinite(computed.absoluteReferenceDiagnosticPerS)||
					!AllFinite(computed.monitoredAbsoluteReferenceTargetPerS)||
					!AllFinite(computed.assembledTargetPerS)||
					!AllFinite(computed.tangentEnclosurePerS)||
					!AllFinite(computed.assembledEnclosurePerS)||!enclosureNonnegative||
					!std::isfinite(computed.deviceElapsedMS)){
					result=std::move(computed);
					if(error)*error="production resident target publication failed its certificate";return false;}
				result=std::move(computed);if(error)error->clear();return true;
			}
		} catch(const std::bad_alloc&){result=FireProductionResidentTargetLineageComparatorResult();
			if(error)try{*error="production resident target allocation failed";}catch(const std::bad_alloc&){}
			return false;}
	}

	bool BuildFireProductionScalarPhysicalFluxPrerequisiteMetal(
		const FireProductionScalarPhysicalFluxPrerequisiteRequest&,
		FireProductionScalarPhysicalFluxPrerequisiteResult& result,std::string* error )
	{
		result=FireProductionScalarPhysicalFluxPrerequisiteResult();
		if(error)*error=
			"production scalar physical-flux Metal prerequisite lacks fp64 identity qualification";
		return false;
	}

	bool BuildFireProductionScalarFCTFluxPairMetalResidentDiagnostic(
		const FireProductionScalarFCTRequest& request,id<MTLBuffer> beginning,
		const std::array<id<MTLBuffer>,3>& frozenVelocityMPerS,
		FireProductionMetalScalarFCTFluxPairDiagnostic& result,std::string* error )
	{
		result=FireProductionMetalScalarFCTFluxPairDiagnostic();
		MetalSingleStageFCTParameters parameters;std::vector<unsigned char> inflow;
		std::array<std::size_t,3> faceOffset;std::size_t allFaces=0u;
		if(!PrepareScalarFCTMetalStageMetadata(request,parameters,inflow,faceOffset,allFaces,error)){
			if(error)*error="production scalar FCT Metal flux metadata is invalid";return false;}
		const std::size_t cellBytes=9u*request.shape.CellCount()*sizeof(float);
		if(!beginning||[beginning length]<cellBytes){
			if(error)*error="production scalar FCT Metal beginning buffer is invalid";
			return false;
		}
		for(unsigned int axis=0u;axis<3u;++axis)if(!frozenVelocityMPerS[axis]||
			[frozenVelocityMPerS[axis] length]<FireProductionProjectionFaceCount(
				request.shape,axis)*sizeof(float)){
				if(error)*error="production scalar FCT Metal velocity buffer is invalid";
				return false;
			}
		SingleStageFCTMetalContext& context=SingleStageFCTContext();
		if(!context.Valid()){if(error)*error=context.error;return false;}
		@autoreleasepool {
			id<MTLBuffer> ambient=[context.device newBufferWithBytes:request.ambient.data()
				length:request.ambient.size()*sizeof(float) options:MTLResourceStorageModeShared];
			id<MTLBuffer> inflowBuffer=[context.device newBufferWithBytes:inflow.data()
				length:inflow.size() options:MTLResourceStorageModeShared];
			id<MTLBuffer> basis=[context.device newBufferWithBytes:request.nullspaceBasis.data()
				length:request.nullspaceBasis.size()*sizeof(float)
				options:MTLResourceStorageModeShared];
			id<MTLBuffer> projector=[context.device newBufferWithBytes:
				request.coordinateProjector.data()
				length:request.coordinateProjector.size()*sizeof(float)
				options:MTLResourceStorageModeShared];
			id<MTLBuffer> parameter=[context.device newBufferWithBytes:&parameters
				length:sizeof(parameters) options:MTLResourceStorageModeShared];
			const std::size_t fluxBytes=9u*allFaces*sizeof(float);
			id<MTLBuffer> low=[context.device newBufferWithLength:fluxBytes
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> delta=[context.device newBufferWithLength:fluxBytes
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> failure=[context.device newBufferWithLength:sizeof(std::uint32_t)
				options:MTLResourceStorageModeShared];
			if(!ambient||!inflowBuffer||!basis||!projector||!parameter||!low||!delta||!failure){
				if(error)*error="production scalar FCT Metal flux allocation failed";return false;}
			std::memset([failure contents],0,sizeof(std::uint32_t));
			id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context.queue);
			id<MTLComputeCommandEncoder> encoder=command?ProducerProfileEncoder(command):nil;
			if(!encoder){if(error)*error="production scalar FCT Metal flux encoder failed";return false;}
			[encoder setComputePipelineState:context.buildFluxPair];
			[encoder setBuffer:beginning offset:0 atIndex:0];
			for(unsigned int axis=0u;axis<3u;++axis)
				[encoder setBuffer:frozenVelocityMPerS[axis] offset:0 atIndex:1u+axis];
			[encoder setBuffer:ambient offset:0 atIndex:4];[encoder setBuffer:inflowBuffer offset:0 atIndex:5];
			[encoder setBuffer:basis offset:0 atIndex:6];[encoder setBuffer:projector offset:0 atIndex:7];
			[encoder setBuffer:low offset:0 atIndex:8];[encoder setBuffer:delta offset:0 atIndex:9];
			[encoder setBuffer:parameter offset:0 atIndex:10];Dispatch(encoder,
				context.buildFluxPair,9u*allFaces);[encoder endEncoding];
			encoder=ProducerProfileEncoder(command);
			if(!encoder){if(error)*error="production scalar FCT Metal flux validation encoder failed";
				return false;}
			[encoder setComputePipelineState:context.validateFluxPair];
			[encoder setBuffer:low offset:0 atIndex:0];[encoder setBuffer:delta offset:0 atIndex:1];
			[encoder setBuffer:failure offset:0 atIndex:2];[encoder setBuffer:parameter offset:0 atIndex:3];
			Dispatch(encoder,context.validateFluxPair,9u*allFaces);[encoder endEncoding];
			CommitTrackedMetalCommand(command);[command waitUntilCompleted];
			const std::uint32_t failureWord=*static_cast<const std::uint32_t*>([failure contents]);
			if([command status]!=MTLCommandBufferStatusCompleted||failureWord!=0u){
				if(error)*error="production scalar FCT Metal flux command failed";
				return false;
			}
			result.shape=request.shape;result.timeStepS=request.timeStepS;
			result.boundary=request.boundary;result.packedFaceOffset=faceOffset;
			result.lowFlux=low;result.fluxDelta=delta;
			result.actualMetalAllocationBytes=[low allocatedSize]+[delta allocatedSize]+
				[failure allocatedSize]+[parameter allocatedSize];
			result.commandCommitCount=1u;if(error)error->clear();return true;
		}
	}

	bool AverageFireProductionScalarFCTFluxPairsMetalResidentDiagnostic(
		const FireProductionMetalScalarFCTFluxPairDiagnostic& first,
		const FireProductionMetalScalarFCTFluxPairDiagnostic& second,
		FireProductionMetalScalarFCTFluxPairDiagnostic& result,std::string* error )
	{
		result=FireProductionMetalScalarFCTFluxPairDiagnostic();
		if(!SameScalarFCTMetalPairIdentity(first,second)){
			if(error)*error="production scalar FCT Metal average identity is invalid";
			return false;
		}
		const std::size_t allFaces=first.packedFaceOffset[2]+
			FireProductionProjectionFaceCount(first.shape,2u);
		const std::size_t values=9u*allFaces,bytes=values*sizeof(float);
		if([first.lowFlux length]<bytes||[first.fluxDelta length]<bytes||
			[second.lowFlux length]<bytes||[second.fluxDelta length]<bytes||
			values>std::numeric_limits<std::uint32_t>::max()){
			if(error)*error="production scalar FCT Metal average buffer is invalid";
			return false;
		}
		SingleStageFCTMetalContext& context=SingleStageFCTContext();
		if(!context.Valid()){if(error)*error=context.error;return false;}
		@autoreleasepool {
			const std::uint32_t count=static_cast<std::uint32_t>(values);
			const MetalSingleStageFCTParameters parameters=
				ScalarFCTMetalPairParameters(first);
			id<MTLBuffer> countBuffer=[context.device newBufferWithBytes:&count length:sizeof(count)
				options:MTLResourceStorageModeShared];
			id<MTLBuffer> parameter=[context.device newBufferWithBytes:&parameters
				length:sizeof(parameters) options:MTLResourceStorageModeShared];
			id<MTLBuffer> failure=[context.device newBufferWithLength:sizeof(std::uint32_t)
				options:MTLResourceStorageModeShared];
			id<MTLBuffer> low=[context.device newBufferWithLength:bytes
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> delta=[context.device newBufferWithLength:bytes
				options:MTLResourceStorageModePrivate];
			if(!countBuffer||!parameter||!failure||!low||!delta){
				if(error)*error="production scalar FCT Metal average allocation failed";
				return false;
			}
			std::memset([failure contents],0,sizeof(std::uint32_t));
			id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context.queue);
			auto validate=[&](id<MTLBuffer> pairLow,id<MTLBuffer> pairDelta){
				id<MTLComputeCommandEncoder> check=command?ProducerProfileEncoder(command):nil;
				if(!check)return false;[check setComputePipelineState:context.validateFluxPair];
				[check setBuffer:pairLow offset:0 atIndex:0];[check setBuffer:pairDelta offset:0 atIndex:1];
				[check setBuffer:failure offset:0 atIndex:2];[check setBuffer:parameter offset:0 atIndex:3];
				Dispatch(check,context.validateFluxPair,values);[check endEncoding];return true;};
			if(!validate(first.lowFlux,first.fluxDelta)||
				!validate(second.lowFlux,second.fluxDelta)){
				if(error)*error="production scalar FCT Metal average validation encoder failed";
				return false;
			}
			id<MTLComputeCommandEncoder> encoder=command?ProducerProfileEncoder(command):nil;
			if(!encoder){if(error)*error="production scalar FCT Metal average encoder failed";return false;}
			[encoder setComputePipelineState:context.averageFluxPair];
			[encoder setBuffer:first.lowFlux offset:0 atIndex:0];
			[encoder setBuffer:first.fluxDelta offset:0 atIndex:1];
			[encoder setBuffer:second.lowFlux offset:0 atIndex:2];
			[encoder setBuffer:second.fluxDelta offset:0 atIndex:3];
			[encoder setBuffer:low offset:0 atIndex:4];[encoder setBuffer:delta offset:0 atIndex:5];
			[encoder setBuffer:countBuffer offset:0 atIndex:6];Dispatch(encoder,
				context.averageFluxPair,values);[encoder endEncoding];
			if(!validate(low,delta)){
				if(error)*error="production scalar FCT Metal average output validation encoder failed";
				return false;
			}
			CommitTrackedMetalCommand(command);[command waitUntilCompleted];
			const std::uint32_t failureWord=*static_cast<const std::uint32_t*>([failure contents]);
			if([command status]!=MTLCommandBufferStatusCompleted||failureWord!=0u){
				if(error)*error="production scalar FCT Metal average command failed";
				return false;
			}
			result.shape=first.shape;result.timeStepS=first.timeStepS;
			result.boundary=first.boundary;result.packedFaceOffset=first.packedFaceOffset;
			result.lowFlux=low;result.fluxDelta=delta;
			result.actualMetalAllocationBytes=[low allocatedSize]+[delta allocatedSize]+
				[countBuffer allocatedSize]+[parameter allocatedSize]+[failure allocatedSize];
			result.commandCommitCount=1u;if(error)error->clear();return true;
		}
	}

	bool SolveFireProductionScalarFCTFluxPairMetalResidentDiagnostic(
		const FireProductionScalarFCTRequest& request,id<MTLBuffer> beginning,
		id<MTLBuffer> sourceDelta,
		const FireProductionMetalScalarFCTFluxPairDiagnostic& fluxPair,
		FireProductionMetalScalarFCTSolveDiagnostic& result,std::string* error )
	{
		result=FireProductionMetalScalarFCTSolveDiagnostic();
		MetalSingleStageFCTParameters parameters;std::vector<unsigned char> inflow;
		std::array<std::size_t,3> faceOffset;std::size_t allFaces=0u;
		if(!PrepareScalarFCTMetalStageMetadata(request,parameters,inflow,faceOffset,allFaces,error)||
			fluxPair.shape.nx!=request.shape.nx||fluxPair.shape.ny!=request.shape.ny||
			fluxPair.shape.nz!=request.shape.nz||
			fluxPair.shape.cellWidthM!=request.shape.cellWidthM||
			fluxPair.timeStepS!=request.timeStepS||fluxPair.boundary!=request.boundary||
			fluxPair.packedFaceOffset!=faceOffset||!fluxPair.lowFlux||!fluxPair.fluxDelta){
			if(error)*error="production scalar FCT Metal solve identity is invalid";return false;}
		const std::size_t cells=request.shape.CellCount(),cellBytes=9u*cells*sizeof(float),
			fluxBytes=9u*allFaces*sizeof(float);
		if(!beginning||!sourceDelta||[beginning length]<cellBytes||[sourceDelta length]<cellBytes||
			[fluxPair.lowFlux length]<fluxBytes||[fluxPair.fluxDelta length]<fluxBytes){
			if(error)*error="production scalar FCT Metal solve buffer is invalid";
			return false;
		}
		SingleStageFCTMetalContext& context=SingleStageFCTContext();
		if(!context.Valid()){if(error)*error=context.error;return false;}
		@autoreleasepool {
			id<MTLBuffer> enthalpy=[context.device newBufferWithBytes:
				request.enthalpyBoundsJPerKG.data()
				length:request.enthalpyBoundsJPerKG.size()*sizeof(float)
				options:MTLResourceStorageModeShared];
			const float zero=0.0f;
			id<MTLBuffer> affine=[context.device newBufferWithBytes:&zero length:sizeof(zero)
				options:MTLResourceStorageModeShared];
			id<MTLBuffer> parameter=[context.device newBufferWithBytes:&parameters
				length:sizeof(parameters) options:MTLResourceStorageModeShared];
			id<MTLBuffer> lowState=[context.device newBufferWithLength:cellBytes
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> ratio=[context.device newBufferWithLength:11u*cells*sizeof(float)
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> alpha=[context.device newBufferWithLength:allFaces*sizeof(float)
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> accepted=[context.device newBufferWithLength:cellBytes
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> failure=[context.device newBufferWithLength:sizeof(std::uint32_t)
				options:MTLResourceStorageModeShared];
			if(!enthalpy||!affine||!parameter||!lowState||!ratio||!alpha||!accepted||!failure){
				if(error)*error="production scalar FCT Metal solve allocation failed";return false;}
			std::memset([failure contents],0,sizeof(std::uint32_t));
			id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context.queue);
			auto encoderFor=[&](id<MTLComputePipelineState> pipeline){
				id<MTLComputeCommandEncoder> encoder=command?ProducerProfileEncoder(command):nil;
				if(encoder)[encoder setComputePipelineState:pipeline];return encoder;};
			id<MTLComputeCommandEncoder> encoder=encoderFor(context.validateFluxPair);
			if(!encoder){if(error)*error="production scalar FCT Metal solve validation encoder failed";
				return false;}
			[encoder setBuffer:fluxPair.lowFlux offset:0 atIndex:0];
			[encoder setBuffer:fluxPair.fluxDelta offset:0 atIndex:1];
			[encoder setBuffer:failure offset:0 atIndex:2];[encoder setBuffer:parameter offset:0 atIndex:3];
			Dispatch(encoder,context.validateFluxPair,9u*allFaces);[encoder endEncoding];
			encoder=encoderFor(context.buildRatios);
			if(!encoder){if(error)*error="production scalar FCT Metal ratio encoder failed";return false;}
			[encoder setBuffer:beginning offset:0 atIndex:0];[encoder setBuffer:sourceDelta offset:0 atIndex:1];
			[encoder setBuffer:fluxPair.lowFlux offset:0 atIndex:2];
			[encoder setBuffer:fluxPair.fluxDelta offset:0 atIndex:3];
			[encoder setBuffer:enthalpy offset:0 atIndex:4];[encoder setBuffer:lowState offset:0 atIndex:5];
			[encoder setBuffer:ratio offset:0 atIndex:6];[encoder setBuffer:failure offset:0 atIndex:7];
			[encoder setBuffer:parameter offset:0 atIndex:8];Dispatch(encoder,
				context.buildRatios,11u*cells);[encoder endEncoding];
			encoder=encoderFor(context.buildFaceAlpha);if(!encoder){
				if(error)*error="production scalar FCT Metal alpha encoder failed";
				return false;
			}
			[encoder setBuffer:fluxPair.fluxDelta offset:0 atIndex:0];
			[encoder setBuffer:ratio offset:0 atIndex:1];[encoder setBuffer:enthalpy offset:0 atIndex:2];
			[encoder setBuffer:alpha offset:0 atIndex:3];[encoder setBuffer:failure offset:0 atIndex:4];
			[encoder setBuffer:parameter offset:0 atIndex:5];Dispatch(encoder,
				context.buildFaceAlpha,allFaces);[encoder endEncoding];
			encoder=encoderFor(context.commitScalar);if(!encoder){
				if(error)*error="production scalar FCT Metal commit encoder failed";
				return false;
			}
			[encoder setBuffer:lowState offset:0 atIndex:0];
			[encoder setBuffer:fluxPair.fluxDelta offset:0 atIndex:1];
			[encoder setBuffer:alpha offset:0 atIndex:2];[encoder setBuffer:enthalpy offset:0 atIndex:3];
			[encoder setBuffer:affine offset:0 atIndex:4];[encoder setBuffer:accepted offset:0 atIndex:5];
			[encoder setBuffer:failure offset:0 atIndex:6];[encoder setBuffer:parameter offset:0 atIndex:7];
			Dispatch(encoder,context.commitScalar,cells);[encoder endEncoding];
			CommitTrackedMetalCommand(command);[command waitUntilCompleted];
			if([command status]!=MTLCommandBufferStatusCompleted){
				if(error)*error="production scalar FCT Metal solve command failed";
				return false;
			}
			result.shape=request.shape;result.packedFaceOffset=faceOffset;
			result.lowState=lowState;result.limiterRatio=ratio;result.packedFaceAlpha=alpha;
			result.accepted=accepted;result.failureBitmap=failure;
			result.actualMetalAllocationBytes=[lowState allocatedSize]+[ratio allocatedSize]+
				[alpha allocatedSize]+[accepted allocatedSize]+[failure allocatedSize];
			result.commandCommitCount=1u;if(error)error->clear();return true;
		}
	}

	bool EvaluateFireProductionScalarFCTMetalStageDiagnostic(
		const FireProductionScalarFCTRequest& firstStage,
		const FireProductionScalarFCTRequest& secondStage,
		FireProductionScalarFCTMetalStageDiagnosticResult& result,std::string* error )
	{
		result=FireProductionScalarFCTMetalStageDiagnosticResult();
		try {
			FireProductionScalarFCTFluxPair firstCPU,secondCPU,averagedCPU;
			if(!BuildFireProductionScalarFCTFluxPairCPU(firstStage,firstCPU,error)||
				!BuildFireProductionScalarFCTFluxPairCPU(secondStage,secondCPU,error)||
				!AverageFireProductionScalarFCTFluxPairsCPU(firstCPU,secondCPU,averagedCPU,error))
				return false;
			SingleStageFCTMetalContext& context=SingleStageFCTContext();
			if(!context.Valid()){if(error)*error=context.error;return false;}
			const std::size_t cells=firstStage.shape.CellCount();
			if(secondStage.shape.nx!=firstStage.shape.nx||
				secondStage.shape.ny!=firstStage.shape.ny||
				secondStage.shape.nz!=firstStage.shape.nz||
				secondStage.shape.cellWidthM!=firstStage.shape.cellWidthM){
				if(error)*error="production scalar FCT Metal diagnostic stage shape differs";
				return false;
			}
			@autoreleasepool {
				auto stage=[&](const void* data,const std::size_t bytes){return
					[context.device newBufferWithBytes:data length:bytes
						options:MTLResourceStorageModeShared];};
				const std::size_t cellBytes=9u*cells*sizeof(float);
				id<MTLBuffer> firstQ=stage(firstStage.beginning.data(),cellBytes);
				id<MTLBuffer> secondQ=stage(secondStage.beginning.data(),cellBytes);
				id<MTLBuffer> source=stage(firstStage.sourceDelta.data(),cellBytes);
				std::array<id<MTLBuffer>,3> firstVelocity,secondVelocity;
				for(unsigned int axis=0u;axis<3u;++axis){const std::size_t bytes=
					FireProductionProjectionFaceCount(firstStage.shape,axis)*sizeof(float);
					firstVelocity[axis]=stage(firstStage.frozenVelocityMPerS[axis].data(),bytes);
					secondVelocity[axis]=stage(secondStage.frozenVelocityMPerS[axis].data(),bytes);}
				if(!firstQ||!secondQ||!source){
					if(error)*error="production scalar FCT Metal diagnostic input allocation failed";
					return false;
				}
				for(unsigned int axis=0u;axis<3u;++axis)
					if(!firstVelocity[axis]||!secondVelocity[axis]){
						if(error)*error=
							"production scalar FCT Metal diagnostic velocity allocation failed";
						return false;
					}
				FireProductionMetalScalarFCTFluxPairDiagnostic firstPair,secondPair,averagedPair;
				FireProductionMetalScalarFCTSolveDiagnostic firstSolve,secondSolve,averagedSolve;
				if(!BuildFireProductionScalarFCTFluxPairMetalResidentDiagnostic(
					firstStage,firstQ,firstVelocity,firstPair,error)||
					!BuildFireProductionScalarFCTFluxPairMetalResidentDiagnostic(
						secondStage,secondQ,secondVelocity,secondPair,error)||
					!AverageFireProductionScalarFCTFluxPairsMetalResidentDiagnostic(
						firstPair,secondPair,averagedPair,error)||
					!SolveFireProductionScalarFCTFluxPairMetalResidentDiagnostic(
						firstStage,firstQ,source,firstPair,firstSolve,error)||
					!SolveFireProductionScalarFCTFluxPairMetalResidentDiagnostic(
						firstStage,firstQ,source,secondPair,secondSolve,error)||
					!SolveFireProductionScalarFCTFluxPairMetalResidentDiagnostic(
						firstStage,firstQ,source,averagedPair,averagedSolve,error))return false;
				const std::size_t allFaces=firstPair.packedFaceOffset[2]+
					FireProductionProjectionFaceCount(firstStage.shape,2u);
				const std::size_t fluxValues=9u*allFaces;
				const std::size_t publishedFloatValues=6u*fluxValues+
					3u*(29u*cells+allFaces);
				const std::size_t publishedBytes=publishedFloatValues*sizeof(float)+
					3u*sizeof(std::uint32_t);
				id<MTLBuffer> terminal=[context.device newBufferWithLength:publishedBytes
					options:MTLResourceStorageModeShared];
				id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context.queue);
				id<MTLBlitCommandEncoder> blit=command?[command blitCommandEncoder]:nil;
				if(!terminal||!blit){
					if(error)*error=
						"production scalar FCT Metal diagnostic publication allocation failed";
					return false;
				}
				std::size_t offset=0u;
				auto publish=[&](id<MTLBuffer> buffer,const std::size_t bytes){
					[blit copyFromBuffer:buffer sourceOffset:0 toBuffer:terminal
						destinationOffset:offset size:bytes];offset+=bytes;};
				const std::array<FireProductionMetalScalarFCTFluxPairDiagnostic*,3> pairs={{
					&firstPair,&secondPair,&averagedPair}};
				for(const auto* pair:pairs){publish(pair->lowFlux,fluxValues*sizeof(float));
					publish(pair->fluxDelta,fluxValues*sizeof(float));}
				const std::array<FireProductionMetalScalarFCTSolveDiagnostic*,3> solves={{
					&firstSolve,&secondSolve,&averagedSolve}};
				for(const auto* solve:solves){publish(solve->lowState,9u*cells*sizeof(float));
					publish(solve->limiterRatio,11u*cells*sizeof(float));
					publish(solve->packedFaceAlpha,allFaces*sizeof(float));
					publish(solve->accepted,9u*cells*sizeof(float));
					publish(solve->failureBitmap,sizeof(std::uint32_t));}
				[blit endEncoding];CommitTrackedMetalCommand(command);[command waitUntilCompleted];
				if([command status]!=MTLCommandBufferStatusCompleted||offset!=publishedBytes){
					if(error)*error="production scalar FCT Metal diagnostic publication failed";
					return false;
				}
				const unsigned char* bytes=static_cast<const unsigned char*>(
					ReadTrackedMetalBuffer(terminal));
				if(!bytes){
					if(error)*error="production scalar FCT Metal diagnostic payload is unavailable";
					return false;
				}
				offset=0u;
				auto readFloats=[&](std::vector<float>& output,const std::size_t count){
					const float* values=reinterpret_cast<const float*>(bytes+offset);
					output.assign(values,values+count);offset+=count*sizeof(float);};
				std::array<FireProductionScalarFCTFluxPair*,3> publishedPairs={{
					&result.firstFluxPair,&result.secondFluxPair,&result.averagedFluxPair}};
				for(auto* pair:publishedPairs){pair->shape=firstStage.shape;
					pair->timeStepS=firstStage.timeStepS;pair->boundary=firstStage.boundary;
					pair->packedFaceOffset=firstPair.packedFaceOffset;
					readFloats(pair->lowFlux,fluxValues);readFloats(pair->fluxDelta,fluxValues);}
				std::array<FireProductionScalarFCTMetalAcceptanceStageResult*,3> publishedSolves={{
					&result.firstSolve,&result.secondSolve,&result.averagedSolve}};
				for(std::size_t solveIndex=0u;solveIndex<publishedSolves.size();++solveIndex){
					FireProductionScalarFCTMetalAcceptanceStageResult& solve=
						*publishedSolves[solveIndex];
					solve.packedFaceOffset=firstPair.packedFaceOffset;
					solve.lowFlux=publishedPairs[solveIndex]->lowFlux;
					solve.fluxDelta=publishedPairs[solveIndex]->fluxDelta;
					readFloats(solve.lowState,9u*cells);readFloats(solve.limiterRatio,11u*cells);
					std::vector<float> alpha;readFloats(alpha,allFaces);
					for(unsigned int axis=0u;axis<3u;++axis){const std::size_t count=
						FireProductionProjectionFaceCount(firstStage.shape,axis);
						solve.sharedFaceAlpha[axis].assign(alpha.begin()+firstPair.packedFaceOffset[axis],
							alpha.begin()+firstPair.packedFaceOffset[axis]+count);}
					readFloats(solve.accepted,9u*cells);
					std::memcpy(&result.failureBitmap[solveIndex],bytes+offset,
						sizeof(std::uint32_t));offset+=sizeof(std::uint32_t);
				}
				if(offset!=publishedBytes){
					if(error)*error="production scalar FCT Metal diagnostic payload shape is invalid";
					return false;
				}
				result.commandCommitCount=firstPair.commandCommitCount+
					secondPair.commandCommitCount+averagedPair.commandCommitCount+
					firstSolve.commandCommitCount+secondSolve.commandCommitCount+
					averagedSolve.commandCommitCount+1u;
				if(error)error->clear();return true;
			}
		} catch(const std::bad_alloc&){result=FireProductionScalarFCTMetalStageDiagnosticResult();
			if(error)*error="production scalar FCT Metal diagnostic allocation failed";return false;}
	}

	bool EvaluateFireProductionScalarFCTCommitAdmissibilityMetalDiagnostic(
		const FireProductionScalarFCTRequest& request,const std::vector<float>& candidate,
		std::uint32_t& failureBitmap,std::string* error )
	{
		failureBitmap=0u;
		try {
			MetalSingleStageFCTParameters parameters;std::vector<unsigned char> inflow;
			std::array<std::size_t,3> faceOffset;std::size_t allFaces=0u;
			if(!PrepareScalarFCTMetalStageMetadata(request,parameters,inflow,faceOffset,
				allFaces,error))return false;
			const std::size_t cells=request.shape.CellCount();
			if(candidate.size()!=9u*cells||!AllFinite(candidate)){
				if(error)*error="production scalar FCT commit candidate is invalid";return false;}
			parameters.affineRows=0u;SingleStageFCTMetalContext& context=SingleStageFCTContext();
			if(!context.Valid()){if(error)*error=context.error;return false;}
			@autoreleasepool {
				const std::size_t stateBytes=9u*cells*sizeof(float),
					fluxBytes=9u*allFaces*sizeof(float),alphaBytes=allFaces*sizeof(float);
				std::vector<float> zeroFlux(9u*allFaces,0.0f),unitAlpha(allFaces,1.0f);
				const float zero=0.0f;
				auto shared=[&](const void* bytes,const std::size_t length){return
					[context.device newBufferWithBytes:bytes length:length
						options:MTLResourceStorageModeShared];};
				id<MTLBuffer> lowState=shared(candidate.data(),stateBytes),
					delta=shared(zeroFlux.data(),fluxBytes),alpha=shared(unitAlpha.data(),alphaBytes),
					enthalpy=shared(request.enthalpyBoundsJPerKG.data(),14u*sizeof(float)),
					affine=shared(&zero,sizeof(zero)),parameter=shared(&parameters,sizeof(parameters)),
					accepted=[context.device newBufferWithLength:stateBytes
						options:MTLResourceStorageModePrivate],
					failure=[context.device newBufferWithLength:sizeof(std::uint32_t)
						options:MTLResourceStorageModeShared];
				if(!lowState||!delta||!alpha||!enthalpy||!affine||!parameter||!accepted||!failure){
					if(error)*error="production scalar FCT commit diagnostic allocation failed";
					return false;}
				std::memset([failure contents],0,sizeof(std::uint32_t));
				id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context.queue);
				id<MTLComputeCommandEncoder> encoder=command?ProducerProfileEncoder(command):nil;
				if(!encoder){if(error)*error="production scalar FCT commit diagnostic encoder failed";
					return false;}
				[encoder setComputePipelineState:context.commitScalar];
				[encoder setBuffer:lowState offset:0 atIndex:0];
				[encoder setBuffer:delta offset:0 atIndex:1];[encoder setBuffer:alpha offset:0 atIndex:2];
				[encoder setBuffer:enthalpy offset:0 atIndex:3];[encoder setBuffer:affine offset:0 atIndex:4];
				[encoder setBuffer:accepted offset:0 atIndex:5];[encoder setBuffer:failure offset:0 atIndex:6];
				[encoder setBuffer:parameter offset:0 atIndex:7];Dispatch(encoder,
					context.commitScalar,cells);[encoder endEncoding];CommitTrackedMetalCommand(command);
				[command waitUntilCompleted];if([command status]!=MTLCommandBufferStatusCompleted){
					if(error)*error="production scalar FCT commit diagnostic command failed";return false;}
				failureBitmap=*static_cast<const std::uint32_t*>([failure contents]);
				if(error)error->clear();return true;
			}
		} catch(const std::bad_alloc&){failureBitmap=0u;
			if(error)*error="production scalar FCT commit diagnostic allocation failed";return false;}
	}

	bool EvaluateFireProductionEOSLogEnclosureMetalDiagnostic(
		const std::vector<std::array<float,2> >& input,
		std::vector<std::array<float,4> >& expansionAndBound,
		FireProductionEOSLogMetalQualificationIdentity& identity,
		std::string* error )
	{
		expansionAndBound.clear();identity=FireProductionEOSLogMetalQualificationIdentity();
		try {
			if(input.empty()){
				if(error)*error="production EOS log diagnostic input is invalid";return false;}
			for(const std::array<float,2>& value:input)if(!std::isfinite(value[0])||
				!std::isfinite(value[1])||!(static_cast<double>(value[0])+value[1]>0.0)){
				if(error)*error="production EOS log diagnostic input is invalid";return false;}
			ResidentTransportMetalContext& context=ResidentTransportContext();
			if(!context.Valid()){if(error)*error=context.error;return false;}
			identity=context.eosLogIdentity;
			@autoreleasepool {
				id<MTLBuffer> deviceInput=[context.device newBufferWithBytes:input.data()
					length:input.size()*sizeof(std::array<float,2>)
					options:MTLResourceStorageModeShared],
					deviceOutput=[context.device newBufferWithLength:4u*input.size()*sizeof(float)
						options:MTLResourceStorageModeShared];
				if(!deviceInput||!deviceOutput){
					if(error)*error="production EOS log diagnostic allocation failed";return false;}
				id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context.queue);
				id<MTLComputeCommandEncoder> encoder=command?ProducerProfileEncoder(command):nil;
				if(!encoder){if(error)*error="production EOS log diagnostic encoder failed";
					return false;}
				[encoder setComputePipelineState:context.diagnoseEOSLog];
				[encoder setBuffer:deviceInput offset:0 atIndex:0];
				[encoder setBuffer:deviceOutput offset:0 atIndex:1];
				Dispatch(encoder,context.diagnoseEOSLog,input.size());[encoder endEncoding];
				CommitTrackedMetalCommand(command);[command waitUntilCompleted];
				if([command status]!=MTLCommandBufferStatusCompleted){
					if(error)*error="production EOS log diagnostic command failed";return false;}
				const float* output=static_cast<const float*>(ReadTrackedMetalBuffer(deviceOutput));
				if(!output){if(error)*error="production EOS log diagnostic output is unavailable";
					return false;}
				expansionAndBound.resize(input.size());
				for(std::size_t sample=0u;sample<input.size();++sample)
					for(std::size_t component=0u;component<4u;++component)
						expansionAndBound[sample][component]=output[4u*sample+component];
				if(error)error->clear();return true;
			}
		} catch(const std::bad_alloc&){expansionAndBound.clear();
			if(error)*error="production EOS log diagnostic allocation failed";return false;}
	}

	bool SealFireProductionSingleStageFCTBoundaryState(
		const FireProductionProjectionShape& shape,
		const std::array<FireProductionProjectionBoundary,6>& boundary,
		FireProductionSingleStageFCTBoundaryState& state,
		std::string* error )
	{
		try {
			if(shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
				shape.nz<4u||shape.nz>1024u||!(shape.cellWidthM>0.0f)||
				!std::isfinite(shape.cellWidthM)||state.statePayloadIdentity==0u){
				state.identity=0u;
				if(error)*error="production single-stage FCT boundary-state owner is invalid";
				return false;
			}
			for(unsigned int axis=0u;axis<3u;++axis){
				const FireProductionProjectionBoundary lower=boundary[2u*axis],
					upper=boundary[2u*axis+1u];
				if(lower<FireProductionProjectionPeriodic||lower>FireProductionProjectionWall||
					upper<FireProductionProjectionPeriodic||upper>FireProductionProjectionWall||
					((lower==FireProductionProjectionPeriodic)!=(upper==
					FireProductionProjectionPeriodic))){
					state.identity=0u;
					if(error)*error="production single-stage FCT boundary-state topology is invalid";
					return false;
				}
			}
			for(unsigned int side=0u;side<6u;++side){const std::size_t expected=side<2u?
				shape.ny*shape.nz:(side<4u?shape.nx*shape.nz:shape.nx*shape.ny);
				if(state.pressureOpenInflow[side].size()!=expected){state.identity=0u;
					if(error)*error="production single-stage FCT boundary-state shape is invalid";
					return false;}
				for(const unsigned char value:state.pressureOpenInflow[side])
					if(value>1u||(boundary[side]!=FireProductionProjectionPressureOpen&&value!=0u)){
						state.identity=0u;
						if(error)*error="production single-stage FCT boundary-state byte is invalid";
						return false;
					}
			}
			state.identity=SingleStageFCTBoundaryIdentity(shape,state);
			if(state.identity==0u){
				if(error)*error="production single-stage FCT boundary-state identity is unavailable";
				return false;
			}
			if(error)error->clear();return true;
		} catch(const std::bad_alloc&){state.identity=0u;
			if(error)*error="production single-stage FCT boundary-state sealing failed";return false;}
	}

	bool AttemptFireProductionSingleStageFCTDiagnosticMetal(
		const FireProductionResidentStepRequest& request,
		const FireProductionSingleStageFCTBoundaryState& boundaryState,
		FireProductionSingleStageFCTDiagnosticResult& result,
		std::string* structuredError )
	{
		result=FireProductionSingleStageFCTDiagnosticResult();
		try {
		result.phase=FireProductionSingleStageFCTDiagnosticPhase::Preflight;
		result.operatorVersion=1u;
		result.zeroPhysicalGasFluxIdentity=UINT64_C(0x4a675f6578616374);
		const FireProductionProjectionShape& shape=request.force.shape;
		auto fail=[&](const char* message){if(structuredError)*structuredError=message;return false;};
		auto sameShape=[](const FireProductionProjectionShape& first,
			const FireProductionProjectionShape& second){return first.nx==second.nx&&
			first.ny==second.ny&&first.nz==second.nz&&first.cellWidthM==second.cellWidthM;};
		if(!sameShape(shape,request.cellTransport.shape)||
			!sameShape(shape,request.dualTransport.shape)||
			request.force.timeStepS!=request.cellTransport.timeStepS||
			request.force.timeStepS!=request.dualTransport.timeStepS||
			request.force.boundary!=request.cellTransport.boundary||
			request.force.boundary!=request.dualTransport.boundary||
			request.cellTransport.componentCount!=9u||
			request.cellTransport.retainAcceptedGasMassDose||
			!request.monitorManifoldDiagnostics||request.enforceManifoldPlateau)
			return fail("production single-stage FCT ownership metadata is invalid");
		std::string validationError;
		if(!ValidateFireProductionFrozenForceRequest(request.force,&validationError)||
			!ValidateFireProductionCellPalindromeRequest(request.cellTransport,&validationError)||
			!ValidateFireProductionDualMomentumRequest(request.dualTransport,&validationError)){
			if(structuredError)*structuredError=validationError.empty()?
				"production single-stage FCT operand validation failed":validationError;
			return false;
		}
		const std::size_t cells=shape.CellCount();
		std::array<std::size_t,3> faceCount,faceOffset;std::size_t allFaces=0u;
		for(unsigned int axis=0u;axis<3u;++axis){faceOffset[axis]=allFaces;
			faceCount[axis]=FireProductionProjectionFaceCount(shape,axis);allFaces+=faceCount[axis];}
		if(request.cellSourceIncrement.size()!=9u*cells||
			request.divergenceTargetPerS.size()!=cells||
			request.restorationDivergenceTargetPerS.size()!=cells||
			request.beginningManifoldDeviationPerCell.size()!=cells||
			!ValidateFireProductionCellSourceIncrement(request.cellSourceIncrement,cells,
				request.monitorManifoldDiagnostics,
				&validationError))return fail(validationError.empty()?
				"production single-stage FCT source shape is invalid":validationError.c_str());
		bool anyPeriodic=false,allPeriodic=true;
		std::vector<unsigned char> packedInflow;
		for(unsigned int side=0u;side<6u;++side){
			const std::size_t expected=side<2u?shape.ny*shape.nz:
				(side<4u?shape.nx*shape.nz:shape.nx*shape.ny);
			if(boundaryState.pressureOpenInflow[side].size()!=expected)
				return fail("production single-stage FCT boundary-state shape is invalid");
			for(const unsigned char value:boundaryState.pressureOpenInflow[side])
				if(value>1u||(request.force.boundary[side]!=
					FireProductionProjectionPressureOpen&&value!=0u))
					return fail("production single-stage FCT boundary-state byte is invalid");
			packedInflow.insert(packedInflow.end(),boundaryState.pressureOpenInflow[side].begin(),
				boundaryState.pressureOpenInflow[side].end());
		}
		result.beginningStateIdentity=FireProductionAcceptedStatePayloadDigestFast(shape,
			request.cellTransport.conservativeValues,request.force.beginningMomentumKGPerM2S,
			request.cellTransport.frozenVelocityMPerS);
		if(boundaryState.statePayloadIdentity==0u||
			boundaryState.statePayloadIdentity!=result.beginningStateIdentity)return fail(
			"production single-stage FCT boundary-state predecessor is stale");
		result.boundaryStateIdentity=SingleStageFCTBoundaryIdentity(shape,boundaryState);
		if(boundaryState.identity==0u||boundaryState.identity!=result.boundaryStateIdentity)
			return fail("production single-stage FCT boundary-state identity is stale");
		result.operatorIdentity=AvalancheAcceptedDigest(UINT64_C(0x70726f645f666374)^
			result.boundaryStateIdentity^result.zeroPhysicalGasFluxIdentity);
		for(unsigned int axis=0u;axis<3u;++axis){
			const bool periodic=request.force.boundary[2u*axis]==
				FireProductionProjectionPeriodic;
			anyPeriodic=anyPeriodic||periodic;allPeriodic=allPeriodic&&periodic;
			if(request.momentumSourceIncrement[axis].size()!=faceCount[axis]||
				request.dualTransport.beginningFaceDensity[axis]!=
					request.force.faceDensityKGPerM3[axis]||
				request.dualTransport.beginningMomentum[axis]!=
					request.force.beginningMomentumKGPerM2S[axis]||
				request.dualTransport.frozenVelocityMPerS[axis]!=
					request.cellTransport.frozenVelocityMPerS[axis])
				return fail("production single-stage FCT dual ownership is invalid");
			for(const float increment:request.momentumSourceIncrement[axis]){
				std::uint32_t bits=0u;std::memcpy(&bits,&increment,sizeof(bits));
				if(bits!=0u)return fail(
					"production single-stage FCT momentum source is not positive zero");
			}
		}
		if(anyPeriodic&&!allPeriodic)return fail(
			"production single-stage FCT hybrid periodic topology has no authoritative oracle");
		for(std::size_t cell=0u;cell<cells;++cell){
			float gas=request.cellTransport.conservativeValues[cells+cell];
			for(std::size_t component=2u;component<=6u;++component)
				gas+=request.cellTransport.conservativeValues[component*cells+cell];
			if(gas!=request.force.cellGasDensityKGPerM3[cell])return fail(
				"production single-stage FCT packed gas density does not match force input");
			if(!std::isfinite(request.beginningManifoldDeviationPerCell[cell]))return fail(
				"production single-stage FCT beginning manifold metadata is nonfinite");
		}
		FireProductionManifoldTailTarget tail;
		if(request.restoreManifoldOutliers&&!DeriveFireProductionManifoldTailTarget(
			request.beginningManifoldDeviationPerCell,request.force.timeStepS,shape.cellWidthM,
			tail,structuredError,0x1p-4))return false;
		const bool tailActive=request.restoreManifoldOutliers&&tail.outlierCellCount>0u;
		result.tailCellCount=tail.outlierCellCount;result.tailDrainedVolumeM3=tail.drainedVolumeM3;

		std::vector<float> basis,coordinateProjector,affine;std::array<float,14> enthalpy;
		float feasibility=0.0f,assemblyReserve=0.0f;
		if(!PackMetalSingleStageFCTCertificate(basis,coordinateProjector,enthalpy,affine,
			feasibility,assemblyReserve,structuredError))return false;
		const FireCertifiedNullspace& reconstruction=
			FireSimulationMethaneRecord::PhysicalV1().ConservativeReconstruction();
		FireProductionScalarFCTRequest scalarRequest;
		scalarRequest.shape=shape;scalarRequest.timeStepS=request.force.timeStepS;
		scalarRequest.boundary=request.force.boundary;
		scalarRequest.beginning=request.cellTransport.conservativeValues;
		scalarRequest.sourceDelta=request.cellSourceIncrement;
		scalarRequest.frozenVelocityMPerS=request.cellTransport.frozenVelocityMPerS;
		std::copy(request.cellTransport.ambientValues.begin(),
			request.cellTransport.ambientValues.end(),scalarRequest.ambient.begin());
		scalarRequest.pressureOpenInflow=boundaryState.pressureOpenInflow;
		scalarRequest.nullity=reconstruction.nullity;scalarRequest.nullspaceBasis=basis;
		scalarRequest.coordinateProjector=coordinateProjector;
		scalarRequest.enthalpyBoundsJPerKG=enthalpy;
		scalarRequest.feasibilityFactor=feasibility;
		scalarRequest.assemblyReserveFactor=assemblyReserve;
		FireProductionScalarFCTResult scalarReference;
		if(!EvaluateFireProductionScalarFCTCPU(scalarRequest,scalarReference,structuredError))
			return false;
		MetalSingleStageFCTParameters parameters={static_cast<std::uint32_t>(shape.nx),
			static_cast<std::uint32_t>(shape.ny),static_cast<std::uint32_t>(shape.nz),
			static_cast<std::uint32_t>(cells),9u,11u,
			static_cast<std::uint32_t>(reconstruction.nullity),
			static_cast<std::uint32_t>(reconstruction.constraintRows),{},{},
			shape.cellWidthM,request.force.timeStepS,feasibility,assemblyReserve};
		std::size_t sideOffset=0u;for(unsigned int side=0u;side<6u;++side){
			parameters.boundary[side]=static_cast<std::uint32_t>(request.force.boundary[side]);
			parameters.sideOffset[side]=static_cast<std::uint32_t>(sideOffset);
			sideOffset+=boundaryState.pressureOpenInflow[side].size();
		}
		std::uint64_t certified=0u,forceCertified=0u,projectionCertified=0u;
		if(!FireProductionResidentForceMetalWorkingSetBytes(shape,false,forceCertified)||
			!FireProductionProjectionWorkingSetBytes(shape,projectionCertified))
			return fail("production single-stage FCT working-set base is unavailable");
		certified=forceCertified;
		auto addCertified=[&](std::uint64_t bytes){const std::uint64_t rounded=
			(bytes+UINT64_C(16383))&~UINT64_C(16383);if(bytes==0u||rounded<bytes||
			certified>std::numeric_limits<std::uint64_t>::max()-rounded)return false;
			certified+=rounded;return true;};
		if(!addCertified(projectionCertified)||
			(tailActive&&!addCertified(projectionCertified)))return fail(
			"production single-stage FCT projection certificate overflowed");
		const std::uint64_t cellValueBytes=9u*cells*sizeof(float),
			packedFaceBytes=allFaces*sizeof(float),fluxBytes=9u*packedFaceBytes,
			terminalBytes=(29u*cells+20u*allFaces)*sizeof(float);
		auto addCopies=[&](const std::uint64_t bytes,const unsigned int count){
			for(unsigned int copy=0u;copy<count;++copy)if(!addCertified(bytes))return false;
			return true;};
		if(!addCopies(cellValueBytes,6u)||!addCopies(9u*sizeof(float),1u)||
			!addCopies(packedInflow.size(),2u)||!addCopies(cells*sizeof(float),5u)||
			!addCopies(basis.size()*sizeof(float),1u)||
			!addCopies(coordinateProjector.size()*sizeof(float),1u)||
			!addCopies(enthalpy.size()*sizeof(float),1u)||
			!addCopies(affine.size()*sizeof(float),1u)||!addCopies(sizeof(parameters),1u)||
			!addCopies(fluxBytes,2u)||!addCopies(11u*cells*sizeof(float),1u)||
			!addCopies(packedFaceBytes,2u)||!addCopies(sizeof(std::uint32_t),1u)||
			!addCopies(terminalBytes,1u))return fail(
			"production single-stage FCT working-set certificate overflowed");
		for(unsigned int axis=0u;axis<3u;++axis)if(!addCopies(
			faceCount[axis]*sizeof(float),2u))return fail(
			"production single-stage FCT velocity working-set certificate overflowed");
		if(certified>(UINT64_C(1)<<31u))return fail(
			"production single-stage FCT working set exceeds two GiB");
		result.certifiedWorkingSetBytes=certified;
		SingleStageFCTMetalContext& context=SingleStageFCTContext();
		if(!context.Valid()){if(structuredError)*structuredError=context.error;return false;}
		result.pipelineIdentityComplete=true;
		@autoreleasepool {
			std::uint64_t ownBytes=0u;std::vector<id<MTLBuffer> > owned;
			auto record=[&](id<MTLBuffer> buffer){if(!buffer)return false;
				const std::uint64_t bytes=[buffer allocatedSize];
				if(ownBytes>std::numeric_limits<std::uint64_t>::max()-bytes)return false;
				ownBytes+=bytes;owned.push_back(buffer);return true;};
			auto stage=[&](const void* data,const std::size_t bytes){id<MTLBuffer> buffer=
				[context.device newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared];
				return record(buffer)?buffer:nil;};
			auto privateBuffer=[&](const std::size_t bytes){id<MTLBuffer> buffer=
				[context.device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
				return record(buffer)?buffer:nil;};
			id<MTLBuffer> qStage=stage(request.cellTransport.conservativeValues.data(),cellValueBytes),
				q=privateBuffer(cellValueBytes),sourceStage=stage(request.cellSourceIncrement.data(),
				cellValueBytes),source=privateBuffer(cellValueBytes),ambient=stage(
				request.cellTransport.ambientValues.data(),9u*sizeof(float));
			std::array<id<MTLBuffer>,3> velocityStage,velocity;
			for(unsigned int axis=0u;axis<3u;++axis){velocityStage[axis]=stage(
				request.cellTransport.frozenVelocityMPerS[axis].data(),faceCount[axis]*sizeof(float));
				velocity[axis]=privateBuffer(faceCount[axis]*sizeof(float));}
			id<MTLBuffer> inflowStage=stage(packedInflow.data(),packedInflow.size()),
				inflow=privateBuffer(packedInflow.size()),targetStage=stage(
				request.divergenceTargetPerS.data(),cells*sizeof(float)),target=privateBuffer(cells*sizeof(float));
			const std::vector<float>& tailTarget=tailActive?tail.divergenceTargetPerS:
				request.restorationDivergenceTargetPerS;
			id<MTLBuffer> tailStage=stage(tailTarget.data(),cells*sizeof(float)),
				tailPrivate=privateBuffer(cells*sizeof(float)),basisBuffer=stage(basis.data(),
				basis.size()*sizeof(float)),projectorBuffer=stage(coordinateProjector.data(),
				coordinateProjector.size()*sizeof(float)),enthalpyBuffer=stage(enthalpy.data(),
				enthalpy.size()*sizeof(float)),affineBuffer=stage(affine.data(),affine.size()*sizeof(float)),
				parameterBuffer=stage(&parameters,sizeof(parameters));
			id<MTLBuffer> low=privateBuffer(fluxBytes),delta=privateBuffer(fluxBytes),
				lowState=privateBuffer(cellValueBytes),ratio=privateBuffer(11u*cells*sizeof(float)),
				alpha=privateBuffer(packedFaceBytes),accepted=privateBuffer(cellValueBytes),
				rate=privateBuffer(packedFaceBytes),gasDensity=privateBuffer(cells*sizeof(float));
			id<MTLBuffer> failure=[context.device newBufferWithLength:sizeof(std::uint32_t)
				options:MTLResourceStorageModeShared],terminal=[context.device newBufferWithLength:
				terminalBytes options:MTLResourceStorageModeShared];
			if(!record(failure)||!record(terminal)||!qStage||!q||!sourceStage||!source||!ambient||
				!inflowStage||!inflow||!targetStage||!target||!tailStage||!tailPrivate||!basisBuffer||
				!projectorBuffer||!enthalpyBuffer||!affineBuffer||!parameterBuffer||!low||!delta||
				!lowState||!ratio||!alpha||!accepted||!rate||!gasDensity)
				return fail("production single-stage FCT allocation failed");
			for(unsigned int axis=0u;axis<3u;++axis)if(!velocityStage[axis]||!velocity[axis])
				return fail("production single-stage FCT velocity allocation failed");
			std::memset([failure contents],0,sizeof(std::uint32_t));
			id<MTLCommandBuffer> upload=TrackedMetalCommandBuffer(context.queue);
			id<MTLBlitCommandEncoder> blit=upload?[upload blitCommandEncoder]:nil;
			if(!blit)return fail("production single-stage FCT upload encoder failed");
			auto copy=[&](id<MTLBuffer> from,id<MTLBuffer> to,std::size_t bytes){
				[blit copyFromBuffer:from sourceOffset:0 toBuffer:to destinationOffset:0 size:bytes];};
			copy(qStage,q,cellValueBytes);copy(sourceStage,source,cellValueBytes);
			for(unsigned int axis=0u;axis<3u;++axis)copy(velocityStage[axis],velocity[axis],
				faceCount[axis]*sizeof(float));
			copy(inflowStage,inflow,packedInflow.size());copy(targetStage,target,cells*sizeof(float));
			copy(tailStage,tailPrivate,cells*sizeof(float));[blit endEncoding];
			CommitTrackedMetalCommand(upload);[upload waitUntilCompleted];
			if([upload status]!=MTLCommandBufferStatusCompleted)return fail(
				"production single-stage FCT upload failed");

			result.phase=FireProductionSingleStageFCTDiagnosticPhase::ForceAdvance;
			FireProductionMetalFrozenForceResidentState force;
			if(!AdvanceFireProductionFrozenForceMetalResidentState(request.force,force,structuredError))
				return false;
			result.forceSchedule=force.schedule;result.forceDiagnostics=force.diagnostics;
			result.phase=FireProductionSingleStageFCTDiagnosticPhase::FCTSolve;
			id<MTLCommandBuffer> command=TrackedMetalCommandBuffer(context.queue);
			auto encoderFor=[&](id<MTLComputePipelineState> pipeline){
				id<MTLComputeCommandEncoder> encoder=command?ProducerProfileEncoder(command):nil;
				if(encoder)[encoder setComputePipelineState:pipeline];return encoder;};
			auto finishKernel=[](id<MTLComputeCommandEncoder> encoder,
				id<MTLComputePipelineState> pipeline,const std::size_t count){
				const std::size_t width=std::min<std::size_t>(256u,
					[pipeline maxTotalThreadsPerThreadgroup]);
				[encoder dispatchThreads:MTLSizeMake(count,1,1)
					threadsPerThreadgroup:MTLSizeMake(width,1,1)];[encoder endEncoding];};
			id<MTLComputeCommandEncoder> encoder=encoderFor(context.buildFluxPair);
			if(!encoder)return fail("production single-stage FCT flux encoder failed");
			[encoder setBuffer:q offset:0 atIndex:0];for(unsigned int axis=0u;axis<3u;++axis)
				[encoder setBuffer:velocity[axis] offset:0 atIndex:1u+axis];
			[encoder setBuffer:ambient offset:0 atIndex:4];[encoder setBuffer:inflow offset:0 atIndex:5];
			[encoder setBuffer:basisBuffer offset:0 atIndex:6];[encoder setBuffer:projectorBuffer offset:0 atIndex:7];
			[encoder setBuffer:low offset:0 atIndex:8];[encoder setBuffer:delta offset:0 atIndex:9];
			[encoder setBuffer:parameterBuffer offset:0 atIndex:10];
			finishKernel(encoder,context.buildFluxPair,9u*allFaces);
			encoder=encoderFor(context.buildRatios);if(!encoder)return fail(
				"production single-stage FCT ratio encoder failed");
			[encoder setBuffer:q offset:0 atIndex:0];[encoder setBuffer:source offset:0 atIndex:1];
			[encoder setBuffer:low offset:0 atIndex:2];[encoder setBuffer:delta offset:0 atIndex:3];
			[encoder setBuffer:enthalpyBuffer offset:0 atIndex:4];[encoder setBuffer:lowState offset:0 atIndex:5];
			[encoder setBuffer:ratio offset:0 atIndex:6];[encoder setBuffer:failure offset:0 atIndex:7];
			[encoder setBuffer:parameterBuffer offset:0 atIndex:8];
			finishKernel(encoder,context.buildRatios,11u*cells);
			encoder=encoderFor(context.buildFaceAlpha);if(!encoder)return fail(
				"production single-stage FCT alpha encoder failed");
			[encoder setBuffer:delta offset:0 atIndex:0];[encoder setBuffer:ratio offset:0 atIndex:1];
			[encoder setBuffer:enthalpyBuffer offset:0 atIndex:2];[encoder setBuffer:alpha offset:0 atIndex:3];
			[encoder setBuffer:failure offset:0 atIndex:4];[encoder setBuffer:parameterBuffer offset:0 atIndex:5];
			finishKernel(encoder,context.buildFaceAlpha,allFaces);
			encoder=encoderFor(context.commitScalar);if(!encoder)return fail(
				"production single-stage FCT scalar encoder failed");
			[encoder setBuffer:lowState offset:0 atIndex:0];[encoder setBuffer:delta offset:0 atIndex:1];
			[encoder setBuffer:alpha offset:0 atIndex:2];[encoder setBuffer:enthalpyBuffer offset:0 atIndex:3];
			[encoder setBuffer:affineBuffer offset:0 atIndex:4];[encoder setBuffer:accepted offset:0 atIndex:5];
			[encoder setBuffer:failure offset:0 atIndex:6];[encoder setBuffer:parameterBuffer offset:0 atIndex:7];
			finishKernel(encoder,context.commitScalar,cells);
			encoder=encoderFor(context.compatibleStageRate);
			if(!encoder)return fail("production single-stage FCT compatible-rate encoder failed");
			[encoder setBuffer:low offset:0 atIndex:0];[encoder setBuffer:delta offset:0 atIndex:1];
			[encoder setBuffer:alpha offset:0 atIndex:2];for(unsigned int axis=0u;axis<3u;++axis)
			[encoder setBuffer:velocity[axis] offset:0 atIndex:3u+axis];
			[encoder setBuffer:rate offset:0 atIndex:6];[encoder setBuffer:failure offset:0 atIndex:7];
			[encoder setBuffer:parameterBuffer offset:0 atIndex:8];
			finishKernel(encoder,context.compatibleStageRate,allFaces);
			encoder=encoderFor(context.applyMomentumRate);if(!encoder)return fail(
				"production single-stage FCT momentum encoder failed");
			[encoder setBuffer:force.packedMomentumKGPerM2S offset:0 atIndex:0];
			[encoder setBuffer:rate offset:0 atIndex:1];[encoder setBuffer:failure offset:0 atIndex:2];
			[encoder setBuffer:parameterBuffer offset:0 atIndex:3];
			finishKernel(encoder,context.applyMomentumRate,allFaces);
			encoder=encoderFor(context.extractGasDensity);if(!encoder)return fail(
				"production single-stage FCT density encoder failed");
			[encoder setBuffer:accepted offset:0 atIndex:0];[encoder setBuffer:gasDensity offset:0 atIndex:1];
			[encoder setBuffer:parameterBuffer offset:0 atIndex:2];
			finishKernel(encoder,context.extractGasDensity,cells);
			CommitTrackedMetalCommand(command);[command waitUntilCompleted];
			if([command status]!=MTLCommandBufferStatusCompleted)return fail(
				"production single-stage FCT command failed");
			const std::uint32_t* failureWord=static_cast<const std::uint32_t*>(
				ReadTrackedMetalBuffer(failure));if(!failureWord)return fail(
				"production single-stage FCT failure word is unavailable");
			result.failureBitmap=*failureWord;if(result.failureBitmap!=0u)return fail(
				"production single-stage FCT admissibility gate refused");
			result.fluxPairBuildCount=1u;result.fctSolveCount=1u;
			result.compatibleRateApplicationCount=1u;result.sourceApplicationCount=1u;
			result.scalarAdmissible=true;result.affineIdentityPassed=true;

			FireProductionProjectionRequest projectionRequest;
			projectionRequest.shape=shape;projectionRequest.timeStepS=request.force.timeStepS;
			projectionRequest.ambientDensityKGPerM3=request.force.ambientDensityKGPerM3;
			projectionRequest.boundary=request.force.boundary;
			projectionRequest.gasDensityKGPerM3=request.force.cellGasDensityKGPerM3;
			projectionRequest.provisionalMomentumKGPerM2S=request.force.beginningMomentumKGPerM2S;
			projectionRequest.divergenceTargetPerS=request.divergenceTargetPerS;
			projectionRequest.residentPhysicalOpenVCycleCount=request.physicalOpenProjectionVCycleCount;
			FireProductionMetalProjectionResidentInput projectionInput;
			projectionInput.gasDensityKGPerM3=gasDensity;
			projectionInput.provisionalMomentumKGPerM2S.fill(force.packedMomentumKGPerM2S);
			projectionInput.provisionalMomentumByteOffset=force.faceByteOffset;
			projectionInput.divergenceTargetPerS=target;
			auto appendProvisionalAudit=[&](){
				id<MTLCommandBuffer> auditCommand=TrackedMetalCommandBuffer(context.queue);
				id<MTLBlitCommandEncoder> auditBlit=auditCommand?
					[auditCommand blitCommandEncoder]:nil;
				if(!auditBlit)return;
				[auditBlit copyFromBuffer:gasDensity sourceOffset:0 toBuffer:terminal
					destinationOffset:0 size:cells*sizeof(float)];
				[auditBlit copyFromBuffer:force.packedMomentumKGPerM2S sourceOffset:0
					toBuffer:terminal destinationOffset:cells*sizeof(float)
					size:allFaces*sizeof(float)];
				[auditBlit endEncoding];CommitTrackedMetalCommand(auditCommand);
				[auditCommand waitUntilCompleted];
				const float* audit=static_cast<const float*>(ReadTrackedMetalBuffer(terminal));
				if(!audit)return;
				float minimumGas=std::numeric_limits<float>::infinity(),maximumGas=0.0f,
					maximumMomentum=0.0f;
				for(std::size_t cell=0u;cell<cells;++cell){minimumGas=std::min(minimumGas,
					audit[cell]);maximumGas=std::max(maximumGas,audit[cell]);}
				for(std::size_t face=0u;face<allFaces;++face)maximumMomentum=std::max(
					maximumMomentum,std::fabs(audit[cells+face]));
				if(structuredError)*structuredError+=" [accepted_gas_min="+
					std::to_string(minimumGas)+" accepted_gas_max="+
					std::to_string(maximumGas)+" provisional_momentum_abs_max="+
					std::to_string(maximumMomentum)+"]";
			};
			result.phase=FireProductionSingleStageFCTDiagnosticPhase::PhysicalProjection;
			if(tailActive){
				FireProductionMetalProjectionResidentState physicalState;
				if(!ProjectFireProductionMetalResidentState(projectionRequest,projectionInput,
					physicalState,result.physicalProjection,structuredError)){
					appendProvisionalAudit();return false;}
				FireProductionProjectionRequest restorationRequest=projectionRequest;
				restorationRequest.divergenceTargetPerS=tailTarget;
				FireProductionMetalProjectionResidentInput restorationInput;
				restorationInput.gasDensityKGPerM3=gasDensity;
				restorationInput.provisionalMomentumKGPerM2S=physicalState.momentumKGPerM2S;
				restorationInput.provisionalMomentumByteOffset=physicalState.momentumByteOffset;
				restorationInput.divergenceTargetPerS=tailPrivate;
				result.phase=FireProductionSingleStageFCTDiagnosticPhase::TailProjection;
				if(!ProjectFireProductionMetalRestorationResident(restorationRequest,restorationInput,
					tailPrivate,result.projection,structuredError))return false;
			}else if(!ProjectFireProductionMetalResident(projectionRequest,projectionInput,
				result.projection,structuredError)){appendProvisionalAudit();return false;}

			id<MTLCommandBuffer> terminalCommand=TrackedMetalCommandBuffer(context.queue);
			blit=terminalCommand?[terminalCommand blitCommandEncoder]:nil;
			if(!blit)return fail("production single-stage FCT terminal encoder failed");
			std::size_t terminalOffset=0u;auto publish=[&](id<MTLBuffer> buffer,std::size_t bytes){
				[blit copyFromBuffer:buffer sourceOffset:0 toBuffer:terminal
					destinationOffset:terminalOffset size:bytes];terminalOffset+=bytes;};
			publish(accepted,cellValueBytes);publish(lowState,cellValueBytes);
			publish(ratio,11u*cells*sizeof(float));publish(alpha,packedFaceBytes);
			publish(low,fluxBytes);publish(delta,fluxBytes);publish(rate,packedFaceBytes);
			[blit endEncoding];
			CommitTrackedMetalCommand(terminalCommand);[terminalCommand waitUntilCompleted];
			if([terminalCommand status]!=MTLCommandBufferStatusCompleted||terminalOffset!=terminalBytes)
				return fail("production single-stage FCT terminal publication failed");
			const float* published=static_cast<const float*>(ReadTrackedMetalBuffer(terminal));
			if(!published)return fail("production single-stage FCT terminal payload is unavailable");
			result.conservativeValues.assign(published,published+9u*cells);
			const float* publishedLowState=published+9u*cells;
			const float* publishedRatio=publishedLowState+9u*cells;
			const float* publishedAlpha=publishedRatio+11u*cells;
			const float* publishedLow=publishedAlpha+allFaces;
			const float* publishedDelta=publishedLow+9u*allFaces;
			const float* publishedRate=publishedDelta+9u*allFaces;
			const std::size_t scalarWordCount=29u*cells+19u*allFaces;
			std::vector<float> scalarWords(published,published+scalarWordCount);
			result.scalarStageIdentity=OrderedAcceptedFloatFieldDigest(scalarWords,
				UINT64_C(0x7363616c61725f31));
			const float unitRoundoff=0.5f*std::numeric_limits<float>::epsilon();
			const float gamma128=128.0f*unitRoundoff/(1.0f-128.0f*unitRoundoff);
			result.scalarStageGamma128=gamma128;
			bool scalarExact=true,scalarExactZero=true;std::string scalarComparisonError;
			auto compareGrouped=[&](const char* field,const float* metal,
				const std::vector<float>& cpu,const std::size_t groups,
				const std::size_t valuesPerGroup,float& maximumAbsolute,
				float& maximumNormalized){
				if(cpu.size()!=groups*valuesPerGroup){scalarComparisonError=
					std::string(field)+" shape";return false;}
				for(std::size_t group=0u;group<groups;++group){float scale=0.0f;
					for(std::size_t local=0u;local<valuesPerGroup;++local){const std::size_t index=
						group*valuesPerGroup+local;scale=std::max(scale,std::max(
						std::fabs(metal[index]),std::fabs(cpu[index])));}
					const float bound=gamma128*scale;
					for(std::size_t local=0u;local<valuesPerGroup;++local){const std::size_t index=
						group*valuesPerGroup+local;const float first=metal[index],second=cpu[index];
						std::uint32_t metalBits=0u,cpuBits=0u;std::memcpy(&metalBits,&first,sizeof(metalBits));
						std::memcpy(&cpuBits,&second,sizeof(cpuBits));scalarExact&=metalBits==cpuBits;
						if((cpuBits&UINT32_C(0x7fffffff))==0u)scalarExactZero&=metalBits==cpuBits;
						const float difference=std::fabs(first-second);maximumAbsolute=
							std::max(maximumAbsolute,difference);if(scale>0.0f)maximumNormalized=
							std::max(maximumNormalized,difference/scale);
						if(!std::isfinite(first)||!std::isfinite(second)||!std::isfinite(difference)||
							difference>bound){scalarComparisonError=std::string(field)+"["+
								std::to_string(index)+"] difference="+std::to_string(difference)+
								" bound="+std::to_string(bound);return false;}
					}
				}
				return true;
			};
			bool scalarEquivalent=scalarReference.packedFaceOffset==faceOffset&&
				scalarReference.accepted.size()==9u*cells&&
				scalarReference.lowState.size()==9u*cells&&
				scalarReference.limiterRatio.size()==11u*cells&&
				scalarReference.lowFlux.size()==9u*allFaces&&
				scalarReference.fluxDelta.size()==9u*allFaces&&
				compareGrouped("low_state",publishedLowState,scalarReference.lowState,9u,cells,
					result.maximumScalarStageAbsoluteDifference,
					result.maximumScalarStageNormalizedDifference)&&
				compareGrouped("ratio",publishedRatio,scalarReference.limiterRatio,11u,cells,
					result.maximumScalarStageAbsoluteDifference,
					result.maximumScalarStageNormalizedDifference)&&
				compareGrouped("low_flux",publishedLow,scalarReference.lowFlux,9u,allFaces,
					result.maximumScalarStageAbsoluteDifference,
					result.maximumScalarStageNormalizedDifference)&&
				compareGrouped("flux_delta",publishedDelta,scalarReference.fluxDelta,9u,allFaces,
					result.maximumScalarStageAbsoluteDifference,
					result.maximumScalarStageNormalizedDifference);
			for(unsigned int axis=0u;axis<3u&&scalarEquivalent;++axis){
				const std::vector<float>& cpuAlpha=scalarReference.sharedFaceAlpha[axis];
				scalarEquivalent=compareGrouped((std::string("alpha_")+std::to_string(axis)).c_str(),
					publishedAlpha+faceOffset[axis],cpuAlpha,1u,faceCount[axis],
					result.maximumScalarStageAbsoluteDifference,
					result.maximumScalarStageNormalizedDifference);
			}
			if(scalarEquivalent)scalarEquivalent=compareGrouped("accepted",published,
				scalarReference.accepted,9u,cells,result.maximumScalarStageAbsoluteDifference,
				result.maximumScalarStageNormalizedDifference);
			result.scalarStageIdentityPassed=scalarExact;
			result.scalarStageExactZeroPassed=scalarExactZero;
			result.scalarStageProducerEquivalencePassed=scalarEquivalent&&scalarExactZero;
			if(!result.scalarStageProducerEquivalencePassed){
				if(structuredError)*structuredError=
					"production single-stage FCT scalar-stage producer equivalence failed: "+
					(scalarComparisonError.empty()?"exact-zero identity":scalarComparisonError);
				return false;
			}
			result.scalarAdmissible=true;result.affineIdentityPassed=true;
			std::vector<float> alphaWords(publishedAlpha,publishedAlpha+allFaces);
			result.alphaIdentity=OrderedAcceptedFloatFieldDigest(alphaWords,
				UINT64_C(0x616c7068615f7631));
			FireProductionCompatibleFCTMomentumRequest comparator;
			comparator.shape=shape;comparator.boundary=request.force.boundary;
			for(unsigned int axis=0u;axis<3u;++axis){
				comparator.lowGasFluxKGPerM2S[axis]=
					scalarReference.acceptedGasFluxKGPerM2S[axis];
				comparator.highGasFluxKGPerM2S[axis]=
					scalarReference.acceptedGasFluxKGPerM2S[axis];
				comparator.sharedFaceAlpha[axis]=scalarReference.sharedFaceAlpha[axis];
				comparator.frozenVelocityMPerS[axis]=request.cellTransport.frozenVelocityMPerS[axis];
			}
			FireProductionCompatibleFCTMomentumResult comparison;
			if(!EvaluateFireProductionCompatibleFCTMomentumCPU(comparator,comparison,structuredError))
				return false;
			bool rateEquivalent=true;bool rateExactZeroBefore=scalarExactZero;
			for(unsigned int axis=0u;axis<3u&&rateEquivalent;++axis)rateEquivalent=compareGrouped(
				(std::string("compatible_rate_")+std::to_string(axis)).c_str(),
				publishedRate+faceOffset[axis],comparison.advectionRateKGPerM2S2[axis],1u,
				faceCount[axis],result.maximumCompatibleRateAbsoluteDifference,
				result.maximumCompatibleRateNormalizedDifference);
			result.compatibleRateExactZeroPassed=scalarExactZero;
			result.compatibleRateProducerEquivalencePassed=rateEquivalent&&scalarExactZero;
			scalarExactZero=rateExactZeroBefore;
			result.maximumCommutingResidual=std::max(result.maximumCompatibleRateAbsoluteDifference,
				scalarReference.maximumCommutingResidualKGPerM3);
			const float commutingBound=gamma128*scalarReference.commutingIdentityScaleKGPerM3;
			const bool scalarCommuting=scalarReference.commutingIdentityAvailable&&
				std::isfinite(commutingBound)&&
				scalarReference.maximumCommutingResidualKGPerM3<=commutingBound;
			result.commutingIdentityPassed=scalarCommuting;
			if(!result.compatibleRateProducerEquivalencePassed)return fail(
				"production single-stage FCT compatible-rate producer equivalence failed");
			if(!result.commutingIdentityPassed){if(structuredError)*structuredError=
				"production single-stage FCT compatible-rate identity failed: residual="+
				std::to_string(scalarReference.maximumCommutingResidualKGPerM3)+" bound="+
				std::to_string(commutingBound)+" component="+
				std::to_string(scalarReference.commutingIdentityComponent)+" face="+
				std::to_string(scalarReference.commutingIdentityFace)+" accepted="+
				std::to_string(scalarReference.commutingIdentityRestrictedAcceptedKGPerM3)+
				" advanced="+std::to_string(scalarReference.commutingIdentityAdvancedKGPerM3);
				return false;
			}
			result.residentProjectionInvocationCount=result.projection.residentProjectionInvocationCount+
				result.physicalProjection.residentProjectionInvocationCount;
			result.interstageFullGridTransferCount=force.diagnostics.substepLoopDeviceToHostTransferCount+
				result.projection.residentInterstageDeviceToHostTransferCount+
				result.physicalProjection.residentInterstageDeviceToHostTransferCount;
			result.terminalStagingCount=1u+result.projection.residentTerminalStagingCount+
				result.physicalProjection.residentTerminalStagingCount;
			std::uint64_t actualBytes=ownBytes;auto addActual=[&](const std::uint64_t bytes){
				if(actualBytes>std::numeric_limits<std::uint64_t>::max()-bytes)return false;
				actualBytes+=bytes;return true;};
			if(!addActual(force.diagnostics.actualMetalAllocationBytes)||
				!addActual(result.projection.residentActualMetalAllocationBytes)||
				!addActual(result.physicalProjection.residentActualMetalAllocationBytes))return fail(
				"production single-stage FCT actual working set overflowed");
			result.actualMetalAllocationBytes=actualBytes;
			if(result.actualMetalAllocationBytes>result.certifiedWorkingSetBytes||
				result.actualMetalAllocationBytes>(UINT64_C(1)<<31u))return fail(
				"production single-stage FCT actual working set exceeds its certificate");
			result.phase=FireProductionSingleStageFCTDiagnosticPhase::Accepted;
			result.accepted=true;if(structuredError)structuredError->clear();return true;
		}
		} catch(const std::bad_alloc&){
			result=FireProductionSingleStageFCTDiagnosticResult();
			if(structuredError)*structuredError="production single-stage FCT allocation failed";
			return false;
		}
	}

	bool AdvanceFireProductionResidentStepMetal(
		const FireProductionResidentStepRequest& request,
		FireProductionResidentStepResult& result,
		std::string* structuredError )
	{
		result=FireProductionResidentStepResult();
		FireProductionResidentStepResult attempted;
		if(!AttemptFireProductionResidentStepMetal(request,attempted,structuredError))return false;
		if(request.monitorManifoldDiagnostics&&attempted.manifoldPlateauPassed&&
			!attempted.AcceptedManifoldTokenMatchesCurrentPayload()){
			result=FireProductionResidentStepResult();
			if(structuredError)*structuredError=
				"production resident step lacks material manifold generation authority";
			return false;
		}
		result=std::move(attempted);return true;
	}

	bool FireProductionPayloadDigestMetal(const std::vector<unsigned char>& bytes,
		unsigned int dispatchWidth,FireProductionPayloadDigestV2& result,double& deviceMS,
		std::string* error,unsigned int qualificationFailTreeAllocation)
	{
		result=FireProductionPayloadDigestV2();deviceMS=0.0;
		@autoreleasepool {
			static PayloadMerkleContext context;std::string failure=context.error;
			auto fail=[&](){if(error)*error=failure;return false;};
			if(!failure.empty())return fail();
			id<MTLBuffer> upload=[context.device newBufferWithLength:std::max<std::size_t>(1u,bytes.size())
				options:MTLResourceStorageModeShared];
			id<MTLBuffer> payload=[context.device newBufferWithLength:std::max<std::size_t>(1u,bytes.size())
				options:MTLResourceStorageModePrivate];
			id<MTLBuffer> root=[context.device newBufferWithLength:32u options:MTLResourceStorageModePrivate];
			id<MTLBuffer> terminal=[context.device newBufferWithLength:32u options:MTLResourceStorageModeShared];
			if(!upload||!payload||!root||!terminal){failure="payload-merkle-v2 qualification allocation";return fail();}
			if(!bytes.empty())std::memcpy([upload contents],bytes.data(),bytes.size());
			id<MTLCommandBuffer> command=[context.queue commandBuffer];
			id<MTLBlitCommandEncoder> ingress=[command blitCommandEncoder];
			if(!ingress){failure="payload-merkle-v2 qualification ingress";return fail();}
			[ingress copyFromBuffer:upload sourceOffset:0 toBuffer:payload destinationOffset:0 size:[payload length]];
			[ingress endEncoding];
			if(!EncodePayloadMerkle(context,command,payload,bytes.size(),dispatchWidth,root,failure,
				qualificationFailTreeAllocation))return fail();
			id<MTLBlitCommandEncoder> egress=[command blitCommandEncoder];
			if(!egress){failure="payload-merkle-v2 qualification egress";return fail();}
			[egress copyFromBuffer:root sourceOffset:0 toBuffer:terminal destinationOffset:0 size:32u];
			[egress endEncoding];[command commit];[command waitUntilCompleted];
			if(command.status!=MTLCommandBufferStatusCompleted){failure="payload-merkle-v2 device failure: "+
				MetalString([command.error localizedDescription]);return fail();}
			const auto* digest=static_cast<const unsigned char*>([terminal contents]);
			const char* hex="0123456789abcdef";
			for(unsigned int i=0u;i<32u;++i){result.rootSHA256+=hex[digest[i]>>4u];result.rootSHA256+=hex[digest[i]&15u];}
			result.payloadBytes=bytes.size();deviceMS=1000.0*(command.GPUEndTime-command.GPUStartTime);
			if(error)error->clear();return true;
		}
	}

	std::uint64_t FireProductionResidentStepMetalCommandCommitCount()
	{
		return MetalCommandCommitCount;
	}
}
