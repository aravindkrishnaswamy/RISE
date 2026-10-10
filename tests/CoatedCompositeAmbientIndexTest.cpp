// DL-490 exact record contract, independent of a rendered energy estimate.
// Production and the independent test adapter must satisfy the same live
// record/stack contract. RGB, NM and HWSS companion evaluation are gated.
#define main CompositeFixtureMain
#include "CompositeEnergyConservationTest.cpp"
#undef main
#include "CoatedAmbientReference.h"
int main(int argc,char** argv) {
    if(argc>2 || (argc>1 && std::string(argv[1])!="--gate")) return 1;
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
        if(observation.mismatch) failed=true;
        const Vector3 outgoing=Vector3Ops::Normalize(Vector3(.3,.2,1));
        for(Scalar nm:{450.,550.,650.}) {
            for(unsigned sample=0;sample<32;++sample) {
                ri.ptIntersection=Point3(10+sample*.1,.2,0);
                ScatteredRayContainer rays;composite->GetSPF()->ScatterNM(ri,sampler,nm,rays,stack);
                const Scalar pdf=composite->GetSPF()->PdfNM(ri,outgoing,nm,stack);
                const Scalar value=composite->GetBSDF()->valueStatefulNM(outgoing,ri,nm,&stack);
                // HWSS companion reconstruction uses this same NM path,
                // divided by the hero density supplied by its caller.
                const Scalar companion=composite->GetSPF()->EvaluateKrayNM(ri,outgoing,ScatteredRay::eRayReflection,nm,stack,pdf);
                if(!(pdf>0 && value>0 && companion>=0) || !std::isfinite(pdf+value+companion)) failed=true;
            }
        }
        for(unsigned sample=0;sample<32;++sample) {
            ri.ptIntersection=Point3(20+sample*.1,.2,0);
            composite->GetSPF()->Pdf(ri,outgoing,stack);
            composite->GetBSDF()->valueStateful(outgoing,ri,&stack);
        }
        std::cout<<"DL490 extended corrected="<<corrected<<" scatter="<<observation.calls<<" scatter_bad="<<observation.mismatchCalls
            <<" value="<<observation.valueCalls<<" value_bad="<<observation.valueMismatches<<" gap_value="<<observation.gapValueCalls
            <<" pdf="<<observation.pdfCalls<<" pdf_bad="<<observation.pdfMismatches<<std::endl;
        if(observation.mismatch || !observation.gapValueCalls || !observation.pdfCalls || observation.valueMismatches || observation.pdfMismatches) failed=true;
        composite->release();bottom->release();
    }
    top->release();delta->release();return failed?1:0;
}
