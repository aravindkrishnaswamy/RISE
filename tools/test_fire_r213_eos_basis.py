#!/usr/bin/env python3
"""CPU differential guard for the thread-local EOS primitive reuse prototype.

This extracts actual Metal arithmetic and the immutable pre-change arithmetic.
It is not a Metal qualification, a full owner gate, or a performance claim.
All mixture inputs below are synthetic and never published as solver output.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = "src/Library/Utilities/FireProductionAdvectionMac.mm"
REFERENCE = "9f1b80fb64a196b81b1f067833f77f4d7325e4b7"


def extract(text):
    parameters = text[text.index("struct EOSParams {"):text.index("struct EOSFCTParams {")]
    arithmetic = text[text.index("struct EOSDD {"):text.index("inline bool eos_within_positive_envelope(")]
    result = parameters + arithmetic
    result = re.sub(r"\b(?:device|constant|thread)\s+", "", result)
    # Metal source uses compact continuation indentation. Normalize comments
    # and whitespace for C++'s indentation diagnostic without changing tokens.
    result = re.sub(r"//[^\n]*", "", result)
    result = result.replace(";", ";\n").replace("}", "}\n")
    return "\n".join(line.lstrip() for line in result.splitlines())


PREAMBLE = r"""
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
using uint = std::uint32_t;
using ulong = std::uint64_t;
using atomic_uint = uint;
using std::abs; using std::max; using std::min; using std::fma;
using std::isfinite; using std::ldexp; using std::nextafter;
enum { memory_order_relaxed = 0 };
inline uint atomic_fetch_or_explicit(uint* value, uint bits, int) {
 uint previous = *value; *value |= bits; return previous;
}
template<class T, class U> T as_type(U value) {
 static_assert(sizeof(T)==sizeof(U), "bit cast size"); T result;
 std::memcpy(&result, &value, sizeof(T)); return result;
}
constexpr bool resident_full_payload_seals = false;
"""

DRIVER = r"""
int main() {
 std::vector<float> thermo(700u, 0.0f), state(9u, 0.0f);
 for(uint s=0u;s<7u;++s) {
  uint base=96u*s; thermo[base]=16.0f+float(s)*3.0f; thermo[base+3u]=2.0f;
  for(uint segment=0u;segment<2u;++segment) {
   uint offset=base+4u+30u*segment;
   thermo[offset]=segment ? 1000.0f : 300.0f;
   thermo[offset+3u]=segment ? 2300.0f : 1000.0f;
   const float terms[8]={-31.25f,0.125f,3.0f,0.001f,-0.0000001f,
    0.00000000001f,-0.000000000000001f,105.0f};
   for(uint term=0u;term<8u;++term) thermo[offset+6u+3u*term]=
    terms[term]*(1.0f+float(s+segment)*0.0625f);
  }
 }
 thermo[7u*96u]=8.3144626617431640625f;
 reference::EOSParams oldParams{}; oldParams.cells=1u;
 candidate::EOSParams newParams{}; newParams.cells=1u;
 uint random=0x712934abu; uint cases=0u, validCases=0u, refusedCases=0u;
 for(uint sample=0u;sample<128u;++sample) {
  random=1664525u*random+1013904223u;
  float T=sample<8u ? (sample<4u ? 300.0f : 1000.0f) :
    301.0f+float(random%1998000u)*0.001f;
  if(sample==0u)T=299.0f;
  if(sample==1u)T=2301.0f;
  if(sample==2u)T=2300.0f;
  if(sample==3u)T=nextafter(2300.0f,-INFINITY);
  if(sample==5u)T=nextafter(1000.0f,-INFINITY);
  if(sample==6u)T=nextafter(1000.0f,INFINITY);
  for(uint mixture=0u;mixture<3u;++mixture) {
   for(uint s=0u;s<7u;++s) {
    random=1664525u*random+1013904223u;
    state[1u+s]=mixture==0u ? (s==sample%7u ? 0.2f : 0.0f) :
      float(random%10001u)*0.00001f*(mixture==2u && (s&1u) ? -1.0f : 1.0f);
   }
   float lo=(sample&1u) ? (nextafter(T,INFINITY)-T)*0.5f : 0.0f;
   uint oldBits=0u,newBits=0u; bool oldValid=true,newValid=true;
   auto expected=reference::eos_energy_dd(state.data(),thermo.data(),oldParams,0u,
      reference::eos_dd(T,lo),&oldBits,oldValid);
   auto actual=candidate::eos_energy_dd(state.data(),thermo.data(),newParams,0u,
      candidate::eos_dd(T,lo),&newBits,newValid);
   const float oldWords[]={expected.hi,expected.lo,expected.tail,expected.bound};
   const float newWords[]={actual.hi,actual.lo,actual.tail,actual.bound};
   for(uint word=0u;word<4u;++word) if(as_type<uint>(oldWords[word])!=as_type<uint>(newWords[word])) {
    std::fprintf(stderr,"EOS basis word mismatch sample=%u mixture=%u word=%u\n",sample,mixture,word);
    return 1;
   }
   if(oldBits!=newBits || oldValid!=newValid) {
    std::fprintf(stderr,"EOS basis class/refusal mismatch sample=%u mixture=%u\n",sample,mixture);
    return 2;
   }
   ++cases; if(oldValid)++validCases; else ++refusedCases;
  }
 }
 if(validCases==0u || refusedCases==0u) return 3;
 std::printf("EOS_BASIS_CPU cases=%u valid=%u refused=%u words=4 bit_mismatches=0 class_mismatches=0 scope=synthetic_cpu_only\n",
   cases,validCases,refusedCases);
 return 0;
}
"""


class EOSBasisDifferential(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reference = extract(subprocess.check_output(
            ["git", "show", REFERENCE+":"+SOURCE], cwd=ROOT, text=True))
        cls.candidate = extract((ROOT / SOURCE).read_text())

    def run_candidate(self, source):
        unit = (PREAMBLE+"\nnamespace reference {\n"+self.reference+
                "\n}\nnamespace candidate {\n"+source+"\n}\n"+DRIVER)
        with tempfile.TemporaryDirectory(prefix="rise-r213-eos-basis-") as directory:
            path = Path(directory) / "basis.cpp"
            binary = Path(directory) / "basis"
            path.write_text(unit)
            build = subprocess.run([os.environ.get("CXX", "clang++"), "-std=c++17",
                                    "-O2", "-ffp-contract=off", "-fno-fast-math",
                                    "-Wall", "-Wextra", "-Werror", str(path), "-o", str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout+build.stderr)
            return subprocess.run([str(binary)], capture_output=True, text=True)

    def test_four_words_and_branch_classes_match_historical_arithmetic(self):
        result = self.run_candidate(self.candidate)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertIn("cases=384", result.stdout)
        print(result.stdout.strip())

    def test_wrong_shared_power_red(self):
        old = "t5=eos_mul(t4,temperature),energy=eos_dd(0.0f);"
        self.assertEqual(self.candidate.count(old), 1)
        mutant = self.candidate.replace(old,"t5=eos_mul(t3,temperature),energy=eos_dd(0.0f);")
        result = self.run_candidate(mutant)
        self.assertEqual(result.returncode, 1, result.stdout+result.stderr)
        self.assertIn("EOS basis word mismatch", result.stderr)
        print("EOS_BASIS_RED wrong_shared_power refused=1 scope=synthetic_cpu_only")

    def test_changed_segment_obligation_red(self):
        old = "1u<<(7u+min(segment,2u))"
        self.assertEqual(self.candidate.count(old), 1)
        result = self.run_candidate(self.candidate.replace(old, "1u<<(6u+min(segment,2u))"))
        self.assertEqual(result.returncode, 2, result.stdout+result.stderr)
        self.assertIn("EOS basis class/refusal mismatch", result.stderr)
        print("EOS_BASIS_RED changed_segment_obligation refused=1 scope=synthetic_cpu_only")


if __name__ == "__main__":
    unittest.main()
