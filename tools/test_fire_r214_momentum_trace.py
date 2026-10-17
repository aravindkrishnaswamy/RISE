#!/usr/bin/env python3
"""CPU-only tests of actual observer serialization; not Metal qualification."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "tests/FireProductionOwnerConvergenceProbe.h"

FIXTURE = r'''
#include <iostream>
template<class T> struct Trace {
    std::uint32_t iteration=0;
    std::vector<T> projectionTargetPerS;
    std::array<std::vector<T>,3> provisionalMomentumKGPerM2S,projectedMomentumKGPerM2S,
        projectedVelocityMPerS,sharedFaceAlpha,nextCandidateMomentumKGPerM2S,
        heunR0AdvectionRateKGPerM2S2;
};
std::vector<std::string> columns(const std::string& line){
    std::istringstream input(line);std::string value;std::vector<std::string> result;
    while(std::getline(input,value,','))result.push_back(value);return result;
}
template<class T> bool fixture(){
    using namespace FireProductionOwnerConvergenceProbe;
    const Geometry shape={2,3,4,1,2};
    const std::array<std::size_t,3> faces={{36,32,30}};
    std::array<std::vector<Trace<T>>,3> storage;
    std::array<const std::vector<Trace<T>>*,3> stages={{&storage[0],&storage[1],&storage[2]}};
    for(unsigned int stage=0;stage<3;++stage)for(unsigned int index=0;index<4;++index){
        Trace<T> trace;trace.iteration=index==0?UINT32_MAX:(index==3?UINT32_C(0x80000001):index-1);
        trace.projectionTargetPerS.assign(24,0);
        for(unsigned int axis=0;axis<3;++axis){
            trace.provisionalMomentumKGPerM2S[axis].assign(faces[axis],7);
            trace.projectedMomentumKGPerM2S[axis].assign(faces[axis],T(index*3+1));
            trace.projectedVelocityMPerS[axis].assign(faces[axis],T(index*2+1));
            if(index>0&&stage<2)trace.sharedFaceAlpha[axis].assign(faces[axis],T(0.5));
            for(std::size_t face=0;face<faces[axis];++face){
                if(index>0&&stage<2)trace.nextCandidateMomentumKGPerM2S[axis].push_back(
                    T(1000*stage+100*index+10*axis+face));
                if(index>0&&stage==1)trace.heunR0AdvectionRateKGPerM2S2[axis].push_back(
                    T(-2000.0-200*index-20*axis-face));
            }
        }
        storage[stage].push_back(trace);
    }
    for(unsigned int stage=1;stage<3;++stage)for(auto& trace:storage[stage])
        trace.provisionalMomentumKGPerM2S=storage[stage-1].back().nextCandidateMomentumKGPerM2S;
    Context context;context.caseSHA256=std::string(64,'a');context.source="synthetic_cpu_only";
    context.ownerIdentity=42;context.acceptedIterations={{2,2,2}};
    // Synthetic deterministic digest used only to test binding changes. Not a
    // substitute for the native SHA-bound publisher or evidence authority.
    const auto digest=[](const std::string& text){std::uint64_t hash=UINT64_C(1469598103934665603);
        for(unsigned char value:text){hash^=value;hash*=UINT64_C(1099511628211);}
        std::ostringstream out;out<<std::hex<<std::setfill('0');
        for(unsigned int i=0;i<4;++i)out<<std::setw(16)<<hash;return out.str();};
    std::string text,binding,error,csv,legacy,legacyBinding,legacyCSV;
    if(!Serialize(stages,shape,context,digest,legacy,legacyBinding,error,&legacyCSV)||
       !SerializeV2(stages,shape,context,digest,text,binding,error,&csv))return false;
    if(text.find("OWNER_CONVERGENCE_PROBE version=2 ")!=0||
       legacy.find("OWNER_CONVERGENCE_PROBE version=1 ")!=0||
       legacy.find("OWNER_DYNAMICS_FACE")!=std::string::npos||binding==legacyBinding)return false;
    // Reject a duplicate terminal before binding: a changed hash alone is not
    // the oracle. Retain legitimate failed-terminal -> next-Picard ordering.
    const auto originalR0=storage[0];std::string orderText,orderBinding,orderError;
    storage[0].push_back(storage[0].back());
    if(SerializeV2(stages,shape,context,digest,orderText,orderBinding,orderError))return false;
    storage[0]=originalR0;
    auto nextPicard=storage[0].back();nextPicard.iteration=2;
    auto nextTerminal=nextPicard;nextTerminal.iteration=UINT32_C(0x80000002);
    storage[0].push_back(nextPicard);storage[0].push_back(nextTerminal);
    context.acceptedIterations[0]=3;
    if(!SerializeV2(stages,shape,context,digest,orderText,orderBinding,orderError))return false;
    storage[0]=originalR0;context.acceptedIterations[0]=2;
    std::istringstream rows(csv);std::string line;std::getline(rows,line);
    if(columns(line).size()!=26)return false;
    std::size_t count=0;
    while(std::getline(rows,line)){
        const auto values=columns(line);if(values.size()!=26)return false;
        const unsigned int stage=static_cast<unsigned int>(values[4][1]-'0');
        const auto index=std::stoul(values[8]),axis=std::stoul(values[9]),face=std::stoul(values[10]);
        const bool next=stage<2&&index>0,rate=stage==1&&index>0;
        const double wantNext=1000*stage+100*index+10*axis+face;
        const double wantRate=-2000.0-200*index-20*axis-face;
        if(next?(std::stod(values[22])!=wantNext):(values[22]!="unavailable"))return false;
        if(rate?(std::stod(values[23])!=wantRate):(values[23]!="unavailable"))return false;
        if(next&&index>1?(std::stod(values[24])!=100):(values[24]!="unavailable"))return false;
        if(rate&&index>1?(std::stod(values[25])!=-200):(values[25]!="unavailable"))return false;
        ++count;
    }
    if(count!=3*4*(5*shape.nz+1))return false;
    const auto refused=[&](){std::ostringstream out;std::string why;
        return !WriteV2(out,stages,shape,context,digest,binding,why)&&out.str().empty();};
    for(unsigned int field=0;field<2;++field){
        auto& values=field==0?storage[1][1].nextCandidateMomentumKGPerM2S[0]:
            storage[1][1].heunR0AdvectionRateKGPerM2S2[0];
        const auto saved=values;values[7]+=1;if(!refused())return false;values=saved;
        values.pop_back();if(!refused())return false;values=saved;
        values[0]=std::numeric_limits<T>::quiet_NaN();if(!refused())return false;values=saved;
    }
    storage[2][1].nextCandidateMomentumKGPerM2S[0].assign(faces[0],1);
    if(!refused())return false;storage[2][1].nextCandidateMomentumKGPerM2S[0].clear();
    storage[0][1].heunR0AdvectionRateKGPerM2S2[0].assign(faces[0],1);
    if(!refused())return false;storage[0][1].heunR0AdvectionRateKGPerM2S2[0].clear();
    storage[1][0].nextCandidateMomentumKGPerM2S[0].assign(faces[0],1);
    if(!refused())return false;storage[1][0].nextCandidateMomentumKGPerM2S[0].clear();
    storage[1][1].provisionalMomentumKGPerM2S[0][0]+=1;
    if(!refused())return false;storage[1][1].provisionalMomentumKGPerM2S[0][0]-=1;
    context.diagnosticOnly=false;if(!refused())return false;
    return true;
}
int main(){return fixture<float>()&&fixture<double>()?0:1;}
'''


class MomentumTraceTests(unittest.TestCase):
    def run_cpp(self, code):
        with tempfile.TemporaryDirectory(prefix="rise-r214-dynamics-") as directory:
            source, binary = Path(directory) / "fixture.cpp", Path(directory) / "fixture"
            source.write_text(code)
            result = subprocess.run(["clang++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
                                     "-fsanitize=address,undefined", str(source), "-o", str(binary)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            return subprocess.run([str(binary)], capture_output=True).returncode

    def run_fixture(self, old=None, new=None):
        code = HEADER.read_text()
        if old is not None:
            self.assertEqual(code.count(old), 1)
            code = code.replace(old, new)
        return self.run_cpp(code + FIXTURE)

    def packet(self, mutant=False):
        source = (ROOT / "src/Library/Utilities/FireProductionAdvectionMac.mm").read_text()
        begin = source.index("\t\tstd::uint64_t ProjectedHeunObserverPacketBytes(")
        end = source.index("\n\t\tid<MTLDevice> DiscoverProductionMetalDevice", begin)
        function = source[begin:end]
        if mutant:
            self.assertEqual(function.count("38u*faces"), 1)
            function = function.replace("38u*faces", "36u*faces")
        prefix = r'''
#include <cstdint>
#include <cstddef>
#include <array>
struct FireProductionProjectionShape {
    std::uint64_t nx,ny,nz;
    std::uint64_t CellCount()const{return nx*ny*nz;}
};
std::uint64_t FireProductionProjectionFaceCount(const FireProductionProjectionShape& s,unsigned int axis){
    return (s.nx+(axis==0))*(s.ny+(axis==1))*(s.nz+(axis==2));
}
'''
        main = r'''
int main(){for(const auto s:std::array<FireProductionProjectionShape,4>{{{1,1,1},{2,3,4},{69,69,106},{1024,1024,1024}}}){
    const std::uint64_t cells=s.nx*s.ny*s.nz,faces=(s.nx+1)*s.ny*s.nz+s.nx*(s.ny+1)*s.nz+s.nx*s.ny*(s.nz+1),
        boundary=2*(s.nx*s.ny+s.ny*s.nz+s.nx*s.nz);
    const std::uint64_t historical=(27*cells+36*faces+boundary)*4+2*boundary+16;
    if(ProjectedHeunObserverPacketBytes(s)!=historical+faces*8)return 1;
}return 0;}
'''
        return self.run_cpp(prefix + function + main)

    def test_exact_two_face_plane_observer_certificate(self):
        self.assertEqual(self.packet(), 0)

    def test_omitted_face_planes_certificate_mutant_red(self):
        self.assertEqual(self.packet(mutant=True), 1)

    def test_fields_scope_deltas_and_atomic_binding(self):
        self.assertEqual(self.run_fixture(), 0)

    def test_duplicate_terminal_guard_mutant_red(self):
        self.assertEqual(self.run_fixture(
            "if(terminal&&index>0u&&",
            "if(false&&index>0u&&"), 1)

    def test_frozen_projection_input_substitution_red(self):
        self.assertEqual(self.run_fixture(
            "nextPresent?trace.nextCandidateMomentumKGPerM2S[axis][face]:0.0",
            "nextPresent?trace.provisionalMomentumKGPerM2S[axis][face]:0.0"), 1)

    def test_swapped_rate_surface_red(self):
        self.assertEqual(self.run_fixture(
            "ratePresent?trace.heunR0AdvectionRateKGPerM2S2[axis][face]:0.0",
            "ratePresent?trace.nextCandidateMomentumKGPerM2S[axis][face]:0.0"), 1)

    def test_zero_dynamics_delta_mutant_red(self):
        self.assertEqual(self.run_fixture(
            "next-static_cast<double>(traces[index-1u].nextCandidateMomentumKGPerM2S[axis][face])",
            "next-static_cast<double>(traces[index].nextCandidateMomentumKGPerM2S[axis][face])"), 1)

    def test_missing_horizontal_face_mutant_red(self):
        self.assertEqual(self.run_fixture(
            "if(!emit(0u,geometry.columnX+side+(geometry.nx+1u)*(geometry.columnY+geometry.ny*z))||",
            "if(!emit(1u,geometry.columnX+geometry.nx*(geometry.columnY+side+(geometry.ny+1u)*z))||"), 1)


if __name__ == "__main__":
    unittest.main()
