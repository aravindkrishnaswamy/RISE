#!/usr/bin/env python3
"""Synthetic extracted-arithmetic guard; not live Metal or whole-owner proof."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from test_fire_r213_eos_basis import PREAMBLE, extract as eos_extract

ROOT = Path(__file__).resolve().parents[1]
SOURCE = 'src/Library/Utilities/FireProductionAdvectionMac.mm'
REFERENCE = '39723122'


def extract(text):
    cp = text[text.index('inline EOSDD target_cp_dd('):text.index('// Hull of two discrete successors')]
    basis = ''
    if 'struct TargetTemperatureBasis {' in text:
        basis = text[text.index('struct TargetTemperatureBasis {'):text.index('inline EOSDD target_tangent_at_temperature(')]
    suffix = re.sub(r'\b(?:device|thread|constant)\s+', '', cp+basis)
    suffix = re.sub(r'//[^\n]*', '', suffix).replace(';', ';\n').replace('}', '}\n')
    return eos_extract(text)+'\n'+'\n'.join(x.lstrip() for x in suffix.splitlines())


DRIVER = r'''
template<class A,class B> bool words(A a,B b){
 const float x[]={a.hi,a.lo,a.tail,a.bound},y[]={b.hi,b.lo,b.tail,b.bound};
 for(uint i=0;i<4;++i)if(as_type<uint>(x[i])!=as_type<uint>(y[i]))return false;
 return true;
}
int main(){
 std::vector<float> thermo(700,0.0f);
 for(uint s=0;s<7;++s){uint base=96*s;thermo[base]=16.0f+3.0f*s;thermo[base+3]=2;
  for(uint seg=0;seg<2;++seg){uint o=base+4+30*seg;thermo[o]=seg?1000:300;thermo[o+3]=seg?2300:1000;
   const float c[]={-31.25f,.125f,3.0f,.001f,-1e-7f,1e-11f,-1e-15f,105.0f};
   for(uint j=0;j<8;++j)thermo[o+6+3*j]=c[j]*(1.0f+float(s+seg)*.0625f);}}
 thermo[672]=8.3144626617431640625f;
 uint cases=0,accepted=0,refused=0;
 for(uint i=0;i<256;++i){float t=299.0f+float(i)*8.0f;
  if(i<12){t=i<4?300.0f:i<8?1000.0f:2300.0f;
   if(i%4==1)t=nextafter(t,-INFINITY);if(i%4==2)t=nextafter(t,INFINITY);}
  for(uint s=0;s<7;++s){uint ob=0,nb=0;bool ov=true,nv=true;
   auto ot=reference::eos_dd(t);auto nt=candidate::eos_dd(t);
   auto basis=candidate::target_temperature_basis(nt);
   auto oh=reference::eos_enthalpy_dd(thermo.data(),s,ot,&ob,ov);
   auto oc=reference::target_cp_dd(thermo.data(),s,ot,&ob,ov);
   auto nh=candidate::target_enthalpy_basis_dd(thermo.data(),s,nt,basis,&nb,nv);
   auto nc=candidate::target_cp_basis_dd(thermo.data(),s,nt,basis,&nb,nv);
   if(!words(oh,nh)||!words(oc,nc))return 1;
   if(ob!=nb||ov!=nv)return 2;
   auto corrupted=nc;corrupted.bound=nextafter(corrupted.bound,INFINITY);
   if(candidate::target_dd_words_match(nc,corrupted))return 3;
   ++cases;if(ov)++accepted;else ++refused;
  }}
 std::printf("R228_HELPERS cases=%u accepted=%u refused=%u fields=enthalpy,cp words=4 mismatches=0\n",cases,accepted,refused);
 return 0;
}
'''


class TargetBasis(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reference = extract(subprocess.check_output(['git','show',REFERENCE+':'+SOURCE], cwd=ROOT,text=True))
        cls.candidate = extract((ROOT/SOURCE).read_text())

    def run_source(self, source):
        unit=PREAMBLE+'\nnamespace reference {\n'+self.reference+'\n}\nnamespace candidate {\n'+source+'\n}\n'+DRIVER
        with tempfile.TemporaryDirectory(prefix='rise-r228-helper-') as d:
            p=Path(d)/'test.cpp';p.write_text(unit);binary=Path(d)/'test'
            build=subprocess.run([os.environ.get('CXX','clang++'),'-std=c++17','-O2','-fno-fast-math','-ffp-contract=off','-Wall','-Wextra','-Werror',str(p),'-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stdout+build.stderr)
            return subprocess.run([str(binary)],capture_output=True,text=True)

    def test_current_reference(self):
        r=self.run_source(self.candidate);self.assertEqual(r.returncode,0,r.stdout+r.stderr);print(r.stdout.strip())

    def test_wrong_power(self):
        old='basis.fifthT5=eos_div(eos_mul(basis.t4,temperature),eos_dd(5.0f))';self.assertEqual(self.candidate.count(old),1)
        self.assertEqual(self.run_source(self.candidate.replace(old,'basis.fifthT5=eos_div(eos_mul(basis.t3,temperature),eos_dd(5.0f))')).returncode,1)

    def test_inexact_fifth(self):
        old='eos_div(eos_mul(basis.t4,temperature),eos_dd(5.0f))';self.assertEqual(self.candidate.count(old),1)
        self.assertEqual(self.run_source(self.candidate.replace(old,'eos_mul(eos_mul(basis.t4,temperature),eos_dd(0.2f))')).returncode,1)

    def test_bound_word_witness(self):
        old='as_type<uint>(a.bound)==as_type<uint>(b.bound)';self.assertEqual(self.candidate.count(old),1)
        self.assertEqual(self.run_source(self.candidate.replace(old,'true')).returncode,3)


if __name__=='__main__':
    unittest.main()
