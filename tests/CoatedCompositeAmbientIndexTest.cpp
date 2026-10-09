// DL-490 exact record contract, independent of a rendered energy estimate.
// --gate exposes the production defect; the default measures it and gates
// only the explicitly corrected constant-index reference used by DL-417.
#define main CompositeFixtureMain
#include "CompositeEnergyConservationTest.cpp"
#undef main
#include "CoatedAmbientReference.h"
int main(int argc,char** argv) {
    const bool gate=argc>1 && std::string(argv[1])=="--gate";
    g_stub=new StubObject();g_stub->addref();Fixtures f=MakeFixtures();
    auto* delta=new UniformScalarPainter(1000000);delta->addref();
    auto* top=new DielectricMaterial(*f.s1,*f.s15,*delta,false);top->addref();
    bool failed=false;
    for(bool corrected:{false,true}) {
        CoatedReference::Observation observation;
        auto* bottom=new CoatedReference::AmbientMaterial(*f.lamb,corrected,&observation);bottom->addref();
        auto* composite=MakeComposite(*top,*bottom,3,3,3,3,3,1,*f.s0);
        RandomNumberGenerator rng(490);IndependentSampler sampler(rng);
        auto ri=MakeIntersection(0);auto stack=MakeTestIORStack(g_stub);
        for(unsigned sample=0;sample<32;++sample) {
            ri.ptIntersection=Point3(sample*.1,.2,0);
            ScatteredRayContainer rays;composite->GetSPF()->Scatter(ri,sampler,rays,stack);
        }
        std::cout<<"DL490 corrected="<<corrected<<" calls="<<observation.calls
            <<" record="<<observation.record<<" stack="<<observation.stack
            <<" mismatched_calls="<<observation.mismatchCalls<<" mismatch="<<observation.mismatch<<std::endl;
        if(!observation.calls || observation.stack!=1.5) failed=true;
        if((gate || corrected) && observation.mismatch) failed=true;
        composite->release();bottom->release();
    }
    top->release();delta->release();return failed?1:0;
}
