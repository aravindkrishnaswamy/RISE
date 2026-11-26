#!/usr/bin/env python3
"""Actual prefix builder/consumer extraction; synthetic inputs, no run authority.

ASan/UBSan checks every heap node against an independently enumerated midpoint
tree and the unchanged direct arithmetic. Native owner qualification is separate.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_fire_r213_eos_basis import PREAMBLE

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/Library/Utilities/FireProductionAdvectionMac.mm"

DRIVER = r'''
int main() {
 std::vector<float> thermo(700u,0.0f),state(9u,0.0f);
 for(uint s=0;s<7;++s){uint base=96*s;thermo[base]=16+3*s;thermo[base+3]=3;
  for(uint segment=0;segment<3;++segment){uint o=base+4+30*segment;
   // Keep case endpoints strictly inside the synthetic thermo domain: CPU
   // extraction retains subnormal record bounds that native Metal flushes.
   const float boundaries[]={299,600,1500,2301};
   thermo[o]=boundaries[segment];thermo[o+3]=boundaries[segment+1];
   const float terms[]={-31.25f,0.125f,3.0f,0.001f,-0.0000001f,
    0.00000000001f,-0.000000000000001f,105.0f};
   for(uint i=0;i<8;++i)thermo[o+6+3*i]=terms[i]*(1+float(s+segment)*0.0625f);
  }}
 thermo[672]=8.3144626617431640625f;
 EOSParams p{};p.cells=1;p.Tmin=300;p.Tmax=2300;
 std::vector<EOSDD> table(14+1023*8);
 uint obligations=0,failure=0;
 for(uint gid=0;gid<1037;++gid){build_eos_endpoint_table(thermo.data(),p,table.data(),&obligations,&failure,gid);
  if(failure){std::fprintf(stderr,"builder refusal gid=%u\n",gid);return 1;}}
 if(failure)return 1;
 // Independent breadth-first enumeration, not the builder's path-bit method.
 std::vector<uint> low(2047),high(2047);low[0]=as_type<uint>(p.Tmin);high[0]=as_type<uint>(p.Tmax);
 for(uint node=0;node<1023;++node){uint middle=low[node]+(high[node]-low[node])/2;
  low[2*node+1]=low[node];high[2*node+1]=middle;
  low[2*node+2]=middle;high[2*node+2]=high[node];
  for(uint mixture=0;mixture<3;++mixture){
   for(uint s=0;s<7;++s)state[s+1]=mixture==0?(s==node%7?0.2f:0.0f):
    float((node*31+s*17)%1001)*0.0001f*(mixture==2&&(s&1)?-1.0f:1.0f);
   bool expectedValid=true,actualValid=true;uint expectedBits=0,actualBits=0;
   auto expected=eos_energy_dd_direct(state.data(),thermo.data(),p,0,eos_dd(as_type<float>(middle)),&expectedBits,expectedValid);
   auto actual=eos_prefix_energy(state.data(),thermo.data(),p,0,node,middle,&actualBits,actualValid,table.data());
   if(expectedValid!=actualValid||expectedBits!=actualBits||std::memcmp(&expected,&actual,sizeof(expected)))return 2;
  }
 }
 // Corruption must not disappear inside a blended sum: single-species input.
 for(uint s=0;s<7;++s)state[s+1]=s==0?1.0f:0.0f;
 uint middle=low[0]+(high[0]-low[0])/2;
 EOSDD saved=table[14];table[14].hi+=1.0f;bool valid=true;
 eos_prefix_energy(state.data(),thermo.data(),p,0,0,middle,&obligations,valid,table.data());
 if(resident_full_payload_seals&&valid)return 3;
 table[14]=saved;valid=true;
 EOSDD savedStatus=table[21];table[21].lo=as_type<float>(as_type<uint>(savedStatus.lo)^(1u<<7));
 eos_prefix_energy(state.data(),thermo.data(),p,0,0,middle,&obligations,valid,table.data());
 if(resident_full_payload_seals&&valid)return 10;
 table[21]=savedStatus;valid=true;
 eos_prefix_energy(state.data(),thermo.data(),p,0,1,middle,&obligations,valid,table.data());
 if(valid)return 4;
 // An invalid node does not poison a different visited node.
 table[14+8+7].hi=0;valid=true;obligations=0;
 eos_prefix_energy(state.data(),thermo.data(),p,0,0,middle,&obligations,valid,table.data());
 if(!valid)return 5;
 valid=true;uint other=low[1]+(high[1]-low[1])/2;
 eos_prefix_energy(state.data(),thermo.data(),p,0,1,other,&obligations,valid,table.data());
 if(valid)return 6;
 // Actual invalid interior segment, both endpoints still valid. Unused invalid
 // nodes may not refuse during table construction or add new class obligations.
 for(uint s=0;s<7;++s){thermo[96*s+7]=500;thermo[96*s+34]=800;
  thermo[96*s+37]=900;thermo[96*s+64]=2000;}
 failure=0;obligations=0;
 for(uint gid=0;gid<1037;++gid)build_eos_endpoint_table(thermo.data(),p,table.data(),&obligations,&failure,gid);
 if(failure||obligations!=((1u<<7)|(1u<<9)))return 7;
 uint invalidNodes=0;
 for(uint node=0;node<1023;++node){uint mid=low[node]+(high[node]-low[node])/2;
  bool a=true,b=true;uint x=0,y=0;
  auto expected=eos_energy_dd_direct(state.data(),thermo.data(),p,0,eos_dd(as_type<float>(mid)),&x,a);
  auto actual=eos_prefix_energy(state.data(),thermo.data(),p,0,node,mid,&y,b,table.data());
  if(a!=b||x!=y||std::memcmp(&expected,&actual,sizeof(actual)))return 8;
  if(!a)++invalidNodes;
 }
 if(!invalidNodes)return 9;
 for(uint mode=0;mode<2;++mode){bool a=true,b=true;uint x=0,y=0;
  auto expected=eos_energy_dd_direct(state.data(),thermo.data(),p,0,eos_dd(450.0f),&x,a);
  auto actual=eos_prefix_energy(state.data(),thermo.data(),p,0,mode?1023:0,
   as_type<uint>(450.0f),&y,b,mode?table.data():nullptr);
  if(a!=b||x!=y||std::memcmp(&expected,&actual,sizeof(actual)))return 11;
 }
 std::printf("prefix_nodes=1023 mixtures=3 invalid_nodes=%u qualification=%d\n",invalidNodes,resident_full_payload_seals);
 return 0;
}
'''


class PrefixArithmetic(unittest.TestCase):
    def test_histogram_copy_excludes_following_helpers(self):
        text = SOURCE.read_text()
        generator = text[text.index("bool ReportEOSIterationDistribution("):
                         text.index("kernel void eos_iteration_histogram(")]
        marker = re.search(r'const std::size_t first=base.find\(begin\),last=base.find\(\s*'
                           r'"([^"\n]+)",first\)', generator)
        self.assertIsNotNone(marker)
        endpoint = bytes(marker.group(1), "utf-8").decode("unicode_escape")
        first = text.index("inline bool eos_temperature(")
        copied = text[first:text.index(endpoint, first)]
        self.assertEqual(re.findall(r"inline bool (\w+)\(", copied), ["eos_temperature"])
        # The old endpoint duplicates the following exact-input helper.
        old = text[first:text.index("\nkernel void evaluate_resident_eos_candidate(", first)]
        self.assertIn("inline bool eos_exact_cached_input(", old)

    def check_mode(self, qualification):
        text = SOURCE.read_text()
        params = text[text.index("struct EOSParams {"):text.index("struct EOSFCTParams {")]
        arithmetic = text[text.index("struct EOSDD {"):text.index("inline EOSDD eos_endpoint_enthalpy(")]
        code = params + arithmetic
        code = re.sub(r"\[\[[^]]+\]\]", "", code)
        code = re.sub(r"\b(?:device|constant|thread|kernel)\s+", "", code)
        code = re.sub(r"//[^\n]*", "", code)
        code = code.replace(";", ";\n").replace("}", "}\n")
        preamble = PREAMBLE.replace("resident_full_payload_seals = false",
                                    "resident_full_payload_seals = " + str(qualification).lower())
        with tempfile.TemporaryDirectory(prefix="rise-r234-prefix-") as directory:
            source, binary = Path(directory) / "test.cpp", Path(directory) / "test"
            source.write_text(preamble + code + DRIVER)
            build = subprocess.run(["clang++", "-std=c++17", "-O1", "-Wall", "-Wextra",
                                    "-Werror", "-fsanitize=address,undefined", str(source),
                                    "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())

    def test_qualification(self):
        self.check_mode(True)

    def test_production(self):
        self.check_mode(False)


if __name__ == "__main__":
    unittest.main()
