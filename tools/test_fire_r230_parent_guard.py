#!/usr/bin/env python3
"""Exercise the extracted private encoder guard on native Metal fixture objects.

This checks guard branches, not the issuance of production capabilities.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
text = (ROOT / 'src/Library/Utilities/FireProductionAdvectionMac.mm').read_text()
start = text.index('const auto* reuse=authority.reuseParent;')
end = text.index('auto make=', start)
guard = text[start:end]
prefix = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
struct MetalResidentEOSParameters { unsigned char bytes[64]; };
struct Candidate {
 std::shared_ptr<unsigned char> capability,parentCandidateCapability;
 id<MTLCommandBuffer> parentCommand;
 id<MTLBuffer> publicationIdentity,parentPhysicalPublicationIdentity,
  parentTransportPublicationIdentity,parentEOSThermochemistry,
  conservative,parentEOSParameters;
 unsigned cells;
};
struct Authority {
 const Authority* reuseParent=nullptr;const Candidate* reuseCandidate=nullptr;
 id<MTLCommandBuffer> parentCommand;
 id<MTLBuffer> parentCandidatePublicationIdentity,parentPhysicalPublicationIdentity,
  parentTransportPublicationIdentity,temperature,publicationIdentity;
};
struct Context { id<MTLDevice> device; };
struct Metadata { unsigned cells; };
bool check(const Authority& authority,const Candidate& candidateAuthority,
 const Context& context,id<MTLBuffer> eosThermochemistry,
 const Metadata& metadata,std::string* error) {
'''
suffix = r'''
 return true;
}
int main(){@autoreleasepool {
 Context context{MTLCreateSystemDefaultDevice()};if(!context.device)return 91;
 auto buffer=[&](unsigned n){return [context.device newBufferWithLength:n options:MTLResourceStorageModePrivate];};
 auto queue=[context.device newCommandQueue];auto command=[queue commandBuffer];
 auto thermo=buffer(4);Metadata metadata{1};
 Candidate parent{};parent.capability=std::make_shared<unsigned char>(1);
 parent.parentCommand=command;parent.publicationIdentity=buffer(8);
 parent.parentPhysicalPublicationIdentity=buffer(8);parent.parentTransportPublicationIdentity=buffer(8);
 parent.parentEOSThermochemistry=thermo;parent.conservative=buffer(36);
 parent.parentEOSParameters=buffer(64);parent.cells=1;
 Candidate current{};current.parentCandidateCapability=parent.capability;
 Authority cached{};cached.parentCommand=command;
 cached.parentCandidatePublicationIdentity=parent.publicationIdentity;
 cached.parentPhysicalPublicationIdentity=parent.parentPhysicalPublicationIdentity;
 cached.parentTransportPublicationIdentity=parent.parentTransportPublicationIdentity;
 cached.temperature=buffer(8);cached.publicationIdentity=buffer(8);
 auto run=[&](const char* name,Candidate p,Authority c,bool expected){
  Authority output{};output.reuseCandidate=&p;output.reuseParent=&c;
  std::string error;bool observed=check(output,current,context,thermo,metadata,&error);
  std::printf("R230_PARENT_GUARD synthetic=1 issuance_proof=0 name=%s accepted=%d passed=%d error=%s\n",
   name,observed,observed==expected,error.c_str());return observed==expected;};
 if(!run("linked",parent,cached,true))return 92;
 auto p=parent;p.capability.reset();if(!run("unsealed",p,cached,false))return 92;
 p=parent;p.capability=std::make_shared<unsigned char>(1);if(!run("forged_capability",p,cached,false))return 92;
 p=parent;p.parentCommand=[queue commandBuffer];if(!run("foreign_command",p,cached,false))return 92;
 p=parent;p.parentEOSThermochemistry=buffer(4);if(!run("foreign_thermo",p,cached,false))return 92;
 p=parent;p.publicationIdentity=buffer(8);if(!run("foreign_candidate",p,cached,false))return 92;
 p=parent;p.parentPhysicalPublicationIdentity=buffer(8);if(!run("foreign_physical",p,cached,false))return 92;
 p=parent;p.parentTransportPublicationIdentity=buffer(8);if(!run("foreign_transport",p,cached,false))return 92;
 p=parent;p.conservative=[context.device newBufferWithLength:36 options:MTLResourceStorageModeShared];
 if(!run("cpu_surface",p,cached,false))return 92;
 auto c=cached;c.temperature=buffer(4);if(!run("short_temperature",parent,c,false))return 92;
 Authority partial{};partial.reuseParent=&cached;std::string error;
 if(check(partial,current,context,thermo,metadata,&error))return 92;
 std::puts("R230_PARENT_GUARD synthetic=1 issuance_proof=0 name=incomplete_pair passed=1");
 return 0;
}}
'''
with tempfile.TemporaryDirectory(prefix='rise-r230-parent-') as directory:
    source = Path(directory) / 'guard.mm'
    binary = Path(directory) / 'guard'
    source.write_text(prefix + guard + suffix)
    subprocess.run(['clang++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fobjc-arc', '-fsanitize=address,undefined', str(source),
                    '-framework', 'Foundation', '-framework', 'Metal', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
