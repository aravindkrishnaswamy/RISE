#!/usr/bin/env python3
"""CPU extraction of the exact-input selector; not device lineage proof."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'src/Library/Utilities/FireProductionAdvectionMac.mm'
DRIVER = r'''
#include <cstdint>
#include <cstring>
#include <array>
using uint=std::uint32_t;using ulong=std::uint64_t;using uchar=unsigned char;
template<class T> T as_type(float value){T result;std::memcpy(&result,&value,sizeof(result));return result;}
'''
CHECK = r'''
int main(){
 EOSParams p{};p.cells=3;p.stage=2;p.precision=32;p.Tmin=300;p.Tmax=2300;
 EOSParams prior=p;std::array<float,27> a{},b{};
 for(uint i=0;i<a.size();++i)a[i]=b[i]=float(i+1);
 for(uint cell=0;cell<3;++cell){
  if(!eos_exact_cached_input(a.data(),b.data(),p,&prior,cell))return 1;
  if(eos_exact_cached_input(a.data(),nullptr,p,&prior,cell)||
     eos_exact_cached_input(a.data(),b.data(),p,nullptr,cell))return 2;
  for(uint field=0;field<9;++field){const uint i=field*3+cell;
   b[i]+=1;if(eos_exact_cached_input(a.data(),b.data(),p,&prior,cell))return 3;b[i]=a[i];}
  for(uint byte=0;byte<sizeof(prior);++byte){auto* bytes=reinterpret_cast<uchar*>(&prior);
   bytes[byte]^=1;if(eos_exact_cached_input(a.data(),b.data(),p,&prior,cell))return 4;
   bytes[byte]^=1;}
 }
 a[0]=0.0f;b[0]=-0.0f;
 if(eos_exact_cached_input(a.data(),b.data(),p,&prior,0))return 5;
 return 0;
}
'''


class ExactEOSInput(unittest.TestCase):
    def run_code(self, old=None, new=None):
        text=SOURCE.read_text()
        params=text[text.index('struct EOSParams {'):text.index('struct EOSFCTParams {')]
        helper=text[text.index('inline bool eos_exact_cached_input('):text.index('kernel void evaluate_resident_eos_candidate(')]
        if old:
            self.assertEqual(helper.count(old),1)
            helper=helper.replace(old,new)
        code=DRIVER+params+re.sub(r'\b(?:device|constant)\s+','',helper)+CHECK
        with tempfile.TemporaryDirectory(prefix='rise-r230-cache-') as directory:
            source=Path(directory)/'test.cpp';binary=Path(directory)/'test';source.write_text(code)
            build=subprocess.run(['clang++','-std=c++17','-O1','-Wall','-Wextra','-Werror',
                                  '-fsanitize=address,undefined',str(source),'-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            return subprocess.run([str(binary)],capture_output=True).returncode

    def test_all_input_words(self):
        self.assertEqual(self.run_code(),0)

    def test_missing_energy_field_red(self):
        self.assertEqual(self.run_code('field<9u','field<8u'),3)

    def test_omitted_parameters_red(self):
        self.assertEqual(self.run_code('currentBytes[byte]!=cachedBytes[byte]',
                                       '(currentBytes[byte]^cachedBytes[byte])>255'),4)


if __name__=='__main__':
    unittest.main()
