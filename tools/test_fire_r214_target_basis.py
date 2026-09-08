#!/usr/bin/env python3
"""Synthetic CPU comparison of extracted target Metal against immutable r213.

This is candidate preparation, not Metal/owner qualification or a cost claim.
Every helper compares all four EOSDD words, and target outputs compare by field.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_fire_r213_eos_basis import PREAMBLE

ROOT = Path(__file__).resolve().parents[1]
SOURCE = "src/Library/Utilities/FireProductionAdvectionMac.mm"
REFERENCE = "6f444137733ac01c3236d83a665aeef961550fe6"


def extract(text):
    parts = [
        text[text.index("struct TransportParams {"):text.index("constant uint transport_max_knots")],
        text[text.index("struct EOSParams {"):text.index("inline uint pf_all_faces(")],
        text[text.index("struct EOSDD {"):text.index("inline bool eos_within_positive_envelope(")],
        text[text.index("inline EOSDD target_cp_dd("):text.index("kernel void finalize_resident_target(")],
    ]
    result = "\n".join(parts)
    result = re.sub(r"\b(?:device|constant|thread|kernel)\s+", "", result)
    result = re.sub(r"\[\[[^\]]+\]\]", "", result)
    # The Metal ABI retains this deliberately unread diagnostic input. Remove
    # only that parameter in this CPU adapter, not a numerical operation.
    result = re.sub(r"const float\* absoluteDeviation\s*,", "", result)
    result = re.sub(r"//[^\n]*", "", result)
    result = result.replace(";", ";\n").replace("}", "}\n")
    return "\n".join(line.lstrip() for line in result.splitlines())


DRIVER = r"""
using std::signbit;
template<class A,class B> bool same_words(A a,B b) {
 const float aw[]={a.hi,a.lo,a.tail,a.bound},bw[]={b.hi,b.lo,b.tail,b.bound};
 for(uint w=0u;w<4u;++w)if(as_type<uint>(aw[w])!=as_type<uint>(bw[w]))return false;
 return true;
}
template<class A> void init_target(A& p) {
 p.nx=p.ny=p.nz=p.cells=1u;p.faceOffset[0]=0u;p.faceOffset[1]=2u;p.faceOffset[2]=4u;
 p.dx=0.030593115836381912f;p.dt=0.0016462659696117043f;
 p.tailThreshold=0.0625f;p.policyVersion=0x72313730u;p.attempt=17ul;p.sourcePacket=19ul;
}
template<class T,class F,class E,class S,class P> void init_metadata(T& t,F& f,E& e,S& s,const P& p) {
 t.nx=t.ny=t.nz=t.cells=1u;t.dx=p.dx;t.attempt=p.attempt;
 f.nx=f.ny=f.nz=f.cells=1u;f.dx=p.dx;f.dt=p.dt;
 e.cells=1u;e.timeStepS=p.dt;e.attempt=p.attempt;
 s.cells=1u;s.attempt=p.attempt;s.sourcePacket=p.sourcePacket;
 for(uint a=0u;a<3u;++a)t.faceOffset[a]=p.faceOffset[a];
}
int main() {
 std::vector<float> thermo(700u,0.0f),state(9u,0.0f);
 for(uint s=0u;s<7u;++s){uint base=96u*s;thermo[base]=16.0f+float(s)*3.0f;
  thermo[base+3u]=2.0f;
  for(uint segment=0u;segment<2u;++segment){uint offset=base+4u+30u*segment;
   thermo[offset]=segment?1000.0f:300.0f;thermo[offset+3u]=segment?2300.0f:1000.0f;
   const float terms[8]={-31.25f,0.125f,3.0f,0.001f,-0.0000001f,
    0.00000000001f,-0.000000000000001f,105.0f};
   for(uint term=0u;term<8u;++term)thermo[offset+6u+3u*term]=
    terms[term]*(1.0f+float(s+segment)*0.0625f);
  }
 }
 thermo[7u*96u]=8.3144626617431640625f;
 uint random=0x714934abu,helperCases=0u,targetCases=0u,validCases=0u,refusedCases=0u;
 uint zeroCases=0u,nonzeroCases=0u,positiveTailCases=0u,negativeTailCases=0u;
 for(uint sample=0u;sample<128u;++sample){
  random=1664525u*random+1013904223u;
  float T=301.0f+float(random%1998000u)*0.001f;
  const float edges[]={299.0f,300.0f,nextafter(300.0f,INFINITY),
   nextafter(1000.0f,-INFINITY),1000.0f,nextafter(1000.0f,INFINITY),
   nextafter(2300.0f,-INFINITY),2300.0f,2301.0f};
  if(sample<9u)T=edges[sample];
  for(uint midpoint=0u;midpoint<2u;++midpoint){
   float lo=midpoint?(nextafter(T,INFINITY)-T)*0.5f:0.0f;
   auto oldT=reference::eos_dd(T,lo);auto newT=candidate::eos_dd(T,lo);
   auto basis=candidate::target_temperature_basis(newT);
   for(uint species=0u;species<7u;++species){
    uint oldBits=0u,newBits=0u;bool oldValid=true,newValid=true;
    auto oldH=reference::eos_enthalpy_dd(thermo.data(),species,oldT,&oldBits,oldValid);
    auto newH=candidate::target_enthalpy_basis_dd(thermo.data(),species,newT,basis,&newBits,newValid);
    if(!same_words(oldH,newH)){std::fprintf(stderr,"enthalpy four-word mismatch sample=%u midpoint=%u species=%u\n",sample,midpoint,species);return 1;}
    if(oldValid!=newValid||oldBits!=newBits){std::fprintf(stderr,"enthalpy class/refusal mismatch sample=%u midpoint=%u species=%u\n",sample,midpoint,species);return 2;}
    auto oldCP=reference::target_cp_dd(thermo.data(),species,oldT,&oldBits,oldValid);
    auto newCP=candidate::target_cp_basis_dd(thermo.data(),species,newT,basis,&newBits,newValid);
    if(!same_words(oldCP,newCP)){std::fprintf(stderr,"cp four-word mismatch sample=%u midpoint=%u species=%u\n",sample,midpoint,species);return 1;}
    if(oldValid!=newValid||oldBits!=newBits){std::fprintf(stderr,"cp class/refusal mismatch sample=%u midpoint=%u species=%u\n",sample,midpoint,species);return 2;}
    ++helperCases;
   }
  }
  for(uint mode=0u;mode<4u;++mode){
   for(uint s=0u;s<7u;++s){random=1664525u*random+1013904223u;
    state[s+1u]=0.02f+float(random%10001u)*0.00001f;}
   float physicalMass[48],physicalEnergy[6];
   for(uint c=0u;c<8u;++c)for(uint axis=0u;axis<3u;++axis){
    physicalMass[c*6u+2u*axis]=float(c+axis+1u)*0.00001f;
    physicalMass[c*6u+2u*axis+1u]=mode==0u?physicalMass[c*6u+2u*axis]:
     -float(c+axis+2u)*0.000001f;}
   for(uint axis=0u;axis<3u;++axis){physicalEnergy[2u*axis]=float(axis+1u)*0.01f;
    physicalEnergy[2u*axis+1u]=mode==0u?physicalEnergy[2u*axis]:-float(axis+2u)*0.002f;}
   const float ratios[]={1.0f,1.07f,0.93f,1.24f};float ratio=ratios[mode],source=0.03125f;
   reference::TargetParams op{};candidate::TargetParams np{};init_target(op);init_target(np);
   reference::TransportParams ot{};candidate::TransportParams nt{};
   reference::EOSFCTParams of{};candidate::EOSFCTParams nf{};
   reference::EOSParams oe{};candidate::EOSParams ne{};
   reference::FrozenSourceParams os{};candidate::FrozenSourceParams ns{};
   init_metadata(ot,of,oe,os,op);init_metadata(nt,nf,ne,ns,np);
   uint oldFailure=0u,newFailure=0u,oldBits=0u,newBits=0u;ulong identity=29ul;
   float oldOut[8],newOut[8];for(uint f=0u;f<8u;++f)oldOut[f]=newOut[f]=-9876.0f;
   reference::evaluate_resident_target_terms(state.data(),&T,physicalMass,physicalEnergy,thermo.data(),
    &source,&ratio,&identity,&identity,&identity,&identity,&oldOut[0],&oldOut[1],&oldOut[2],
    &oldOut[3],&oldOut[4],&oldFailure,&oldBits,op,&identity,&os,ot,of,oe,&identity,
    &oldOut[5],&oldOut[6],&oldOut[7],0u);
   candidate::evaluate_resident_target_terms(state.data(),&T,physicalMass,physicalEnergy,thermo.data(),
    &source,&ratio,&identity,&identity,&identity,&identity,&newOut[0],&newOut[1],&newOut[2],
    &newOut[3],&newOut[4],&newFailure,&newBits,np,&identity,&ns,nt,nf,ne,&identity,
    &newOut[5],&newOut[6],&newOut[7],0u);
   // WITNESS_PROBE
   const char* names[]={"tangent","source","absolute_diagnostic","tail","assembled","base","tangent_radius","assembled_radius"};
   for(uint field=0u;field<8u;++field)if(as_type<uint>(oldOut[field])!=as_type<uint>(newOut[field])){
    std::fprintf(stderr,"target field mismatch sample=%u mode=%u field=%s\n",sample,mode,names[field]);return 1;}
   if(oldFailure!=newFailure||oldBits!=newBits){std::fprintf(stderr,"target class/refusal mismatch sample=%u mode=%u\n",sample,mode);return 2;}
   ++targetCases;if(oldFailure==0u){++validCases;
    if((oldBits&(1u<<26u))!=0u)++zeroCases;else ++nonzeroCases;
    if((oldBits&(1u<<21u))!=0u)++positiveTailCases;
    if((oldBits&(1u<<25u))!=0u)++negativeTailCases;
   }else ++refusedCases;
  }
 }
 if(!validCases||!refusedCases||!zeroCases||!nonzeroCases||!positiveTailCases||!negativeTailCases)return 3;
 std::printf("TARGET_BASIS_CPU helper_pairs=%u target_cells=%u accepted=%u refused=%u exact_zero=%u nonzero=%u positive_tail=%u negative_tail=%u bit_mismatches=0 class_mismatches=0 scope=synthetic_cpu_only\n",
  helperCases,targetCases,validCases,refusedCases,zeroCases,nonzeroCases,positiveTailCases,negativeTailCases);
 return 0;
}
"""


class TargetBasisDifferential(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reference = extract(subprocess.check_output(
            ["git", "show", REFERENCE+":"+SOURCE], cwd=ROOT, text=True))
        cls.candidate = extract((ROOT / SOURCE).read_text())

    def run_candidate(self, source, diagnostic=False, witness=False):
        preamble = PREAMBLE.replace(
            "constexpr bool resident_full_payload_seals = false;",
            "constexpr bool resident_full_payload_seals = " + ("true;" if diagnostic else "false;"))
        driver = DRIVER
        if witness:
            driver = driver.replace("// WITNESS_PROBE", r'''
   if(oldFailure==0u){
    std::printf("TARGET_BASIS_WITNESS helper_pairs=%u sample=%u mode=%u failure=%u\n",
     helperCases,sample,mode,newFailure);
    return (newFailure&32768u)!=0u?0:4;
   }
   continue;
''')
        unit = (preamble+"\nusing std::signbit;\nnamespace reference {\n"+self.reference+
                "\n}\nnamespace candidate {\n"+source+"\n}\n"+driver)
        with tempfile.TemporaryDirectory(prefix="rise-r214-target-basis-") as directory:
            path = Path(directory) / "basis.cpp"
            binary = Path(directory) / "basis"
            path.write_text(unit)
            build = subprocess.run([os.environ.get("CXX", "clang++"), "-std=c++17",
                                    "-O2", "-ffp-contract=off", "-fno-fast-math",
                                    "-Wall", "-Wextra", "-Werror", str(path), "-o", str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout+build.stderr)
            return subprocess.run([str(binary)], capture_output=True, text=True)

    def test_separate_helpers_and_target_fields(self):
        for diagnostic in (False, True):
            with self.subTest(diagnostic=diagnostic):
                result = self.run_candidate(self.candidate, diagnostic)
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                self.assertIn("helper_pairs=1792 target_cells=512", result.stdout)
                print(result.stdout.strip(), "diagnostic="+str(int(diagnostic)))

    def test_wrong_shared_power_red(self):
        old = "basis.t5=eos_mul(basis.t4,temperature);"
        self.assertEqual(self.candidate.count(old), 1)
        result = self.run_candidate(self.candidate.replace(old, "basis.t5=eos_mul(basis.t3,temperature);"))
        self.assertEqual(result.returncode, 1, result.stdout+result.stderr)
        self.assertIn("enthalpy four-word mismatch", result.stderr)
        print("TARGET_BASIS_RED wrong_shared_power refused=1")

    def test_changed_segment_obligation_red(self):
        old = "1u<<(7u+min(segment,2u))"
        self.assertEqual(self.candidate.count(old), 1)
        result = self.run_candidate(self.candidate.replace(old, "1u<<(6u+min(segment,2u))"))
        self.assertEqual(result.returncode, 2, result.stdout+result.stderr)
        self.assertIn("enthalpy class/refusal mismatch", result.stderr)
        print("TARGET_BASIS_RED changed_segment_obligation refused=1")

    def test_shared_inverse_square_red(self):
        old = "basis.inverse2=eos_mul(basis.inverse,basis.inverse);"
        self.assertEqual(self.candidate.count(old), 1)
        result = self.run_candidate(self.candidate.replace(old, "basis.inverse2=basis.inverse;"))
        self.assertEqual(result.returncode, 1, result.stdout+result.stderr)
        self.assertIn("cp four-word mismatch", result.stderr)
        print("TARGET_BASIS_RED wrong_inverse_square refused=1")

    def test_segment_interval_branch_red(self):
        old = "if(above&&below)"
        self.assertEqual(self.candidate.count(old), 1)
        result = self.run_candidate(self.candidate.replace(
            old, "if(above||below)"))
        self.assertEqual(result.returncode, 1, result.stdout+result.stderr)
        self.assertIn("enthalpy four-word mismatch sample=0 midpoint=0", result.stderr)
        print("TARGET_BASIS_RED changed_segment_interval refused=1")

    def test_target_sum_cannot_hide_separate_component_mismatch_red(self):
        old = "source[gid]=sourceTarget[gid];"
        self.assertEqual(self.candidate.count(old), 1)
        result = self.run_candidate(self.candidate.replace(old, "source[gid]=sourceTarget[gid]*2.0f;"))
        self.assertEqual(result.returncode, 1, result.stdout+result.stderr)
        self.assertIn("field=source", result.stderr)
        print("TARGET_BASIS_RED separate_source_corruption refused=1")

    def test_qualification_witness_refusal_is_load_bearing_red(self):
        # Perturb a local word only after the standalone helper comparison.
        # A one-ulp bound change need not move a materialized target float;
        # the four-word qualification witness must nevertheless refuse it.
        site = "if(resident_full_payload_seals){EOSDD directEnthalpy="
        self.assertEqual(self.candidate.count(site), 1)
        corrupted = self.candidate.replace(
            site, "cp.bound=nextafter(cp.bound,INFINITY);\n"+site)
        result = self.run_candidate(corrupted, diagnostic=True, witness=True)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertIn("failure=32768", result.stdout)
        witness = "!target_dd_words_match(cp,directCP)"
        self.assertEqual(corrupted.count(witness), 1)
        bypassed = self.run_candidate(corrupted.replace(witness, "cp.hi!=directCP.hi"),
                                      diagnostic=True, witness=True)
        self.assertEqual(bypassed.returncode, 4, bypassed.stdout+bypassed.stderr)
        self.assertIn("failure=0", bypassed.stdout)
        # The ordinary field comparator remains independent of that witness.
        changed_value = self.candidate.replace(site, "cp.hi*=2.0f;\n"+site)
        production = self.run_candidate(changed_value, diagnostic=False)
        self.assertEqual(production.returncode, 1, production.stdout+production.stderr)
        self.assertIn("target field mismatch", production.stderr)
        print("TARGET_BASIS_RED qualification_word_corruption refused=1 witness_bypass_caught=1 production_field_comparator=1")


if __name__ == "__main__":
    unittest.main()
