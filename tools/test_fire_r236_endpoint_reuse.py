#!/usr/bin/env python3
"""Synthetic extraction gate, not native qualification or run evidence."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_fire_r213_eos_basis import PREAMBLE
from test_fire_r234_eos_prefix import SOURCE

DRIVER = r'''
int main(){
 std::vector<float> thermo(700u,0.0f),state(9u,0.0f);
 for(uint s=0;s<7;++s){uint b=96*s;thermo[b]=16.0f;thermo[b+3]=1;
  thermo[b+4]=299;thermo[b+7]=2301;thermo[b+16]=3.0f;}
 thermo[672]=8.0f;
 EOSParams p{};p.cells=1;p.Tmin=300;p.Tmax=2300;p.feasibility=0x1p-20f;
 std::vector<EOSDD> table(14+1023*8);uint bits=0,fail=0;
 for(uint i=0;i<1037;++i)build_eos_endpoint_table(thermo.data(),p,table.data(),&bits,&fail,i);
 if(fail)return 1;
 uint admitted=0,rejected=0,temperatures=0,endpoint=0,interior=0;
 for(uint sample=0;sample<40;++sample){
  state[0]=0;for(uint s=0;s<7;++s){state[s+1]=float(s+1)*0.015625f;state[0]+=state[s+1];}
  if(sample%4==2)state[4]=-0.25f;
  float T=sample%4==0?300.0f:400.0f+float(sample)*40.0f;
  bool valid=true;state[8]=eos_energy_dd_direct(state.data(),thermo.data(),p,0,eos_dd(T),&bits,valid).hi;
  EOSEndpointAggregates actual{{-99,0,0,0},{-99,0,0,0},{-99,0,0,0}},direct;
  const EOSEndpointAggregates poison=actual;
  uint oldBits=1u<<20,newBits=oldBits;
  bool a=eos_state_admissible(state.data(),thermo.data(),p,0,&newBits,table.data(),&actual);
  bool b=eos_state_admissible(state.data(),thermo.data(),p,0,&oldBits,table.data());
  if(a!=b||oldBits!=newBits)return 2;
  if(!a){if(std::memcmp(&actual,&poison,sizeof(actual)))return 3;++rejected;continue;}
  ++admitted;
  if(!eos_uncached_endpoint_aggregates(state.data(),thermo.data(),p,0,&oldBits,table.data(),direct))return 4;
  if(std::memcmp(&actual,&direct,sizeof(actual)))return 5;
  float t1=0,t2=0,alt1=0,alt2=0;
  a=eos_temperature(state.data(),thermo.data(),p,0,&newBits,t1,table.data(),&alt1,&actual);
  b=eos_temperature(state.data(),thermo.data(),p,0,&oldBits,t2,table.data(),&alt2);
  if(a!=b||as_type<uint>(t1)!=as_type<uint>(t2)||as_type<uint>(alt1)!=as_type<uint>(alt2)||oldBits!=newBits)return 6;
  if(a){++temperatures;if(t1==p.Tmin)++endpoint;else ++interior;}
  if(resident_full_payload_seals){
   for(uint field=0;field<3;++field)for(uint word=0;word<4;++word){
    auto mutant=actual;
    EOSDD* f=field==0?&mutant.lowerEnergy:(field==1?&mutant.upperEnergy:&mutant.scale);
    float* w=word==0?&f->hi:(word==1?&f->lo:(word==2?&f->tail:&f->bound));
    *w=nextafter(*w,INFINITY);
    if(eos_temperature(state.data(),thermo.data(),p,0,&newBits,t1,table.data(),&alt1,&mutant))return 7;
   }
  }
 }
 if(!admitted||!rejected||!endpoint||!interior)return 8;
 std::printf("ENDPOINT_REUSE_CPU admitted=%u rejected=%u temperatures=%u endpoint=%u interior=%u qualification=%u words=12\n",
  admitted,rejected,temperatures,endpoint,interior,uint(resident_full_payload_seals));
 return 0;
}
'''


class EndpointReuse(unittest.TestCase):
    def check_mode(self, qualification, driver=DRIVER, expected_exit=0):
        text = SOURCE.read_text()
        code = text[text.index("struct EOSParams {"):text.index("struct EOSFCTParams {")]
        code += text[text.index("struct EOSDD {"):text.index("inline bool eos_exact_cached_input(")]
        code = re.sub(r"\[\[[^]]+\]\]", "", code)
        code = re.sub(r"\b(?:device|constant|thread|kernel)\s+", "", code)
        code = re.sub(r"//[^\n]*", "", code).replace(";", ";\n").replace("}", "}\n")
        pre = PREAMBLE.replace("resident_full_payload_seals = false",
                               "resident_full_payload_seals = " + str(qualification).lower())
        with tempfile.TemporaryDirectory(prefix="rise-r236-") as directory:
            src, exe = Path(directory)/"test.cpp", Path(directory)/"test"
            src.write_text(pre + code + driver)
            result = subprocess.run(["clang++", "-std=c++17", "-O1", "-ffp-contract=off",
                "-fno-fast-math", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                str(src), "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, expected_exit, result.stdout+result.stderr)
            print(result.stdout.strip())

    def test_production(self):
        self.check_mode(False)

    def test_qualification(self):
        self.check_mode(True)

    def test_rejected_nonleading_word_mutation_red(self):
        needle = "if(!a){if(std::memcmp"
        self.assertEqual(DRIVER.count(needle), 1)
        self.check_mode(False, DRIVER.replace(needle,
            "if(!a){actual.upperEnergy.lo=1.0f;if(std::memcmp"), 3)


if __name__ == "__main__":
    unittest.main()
