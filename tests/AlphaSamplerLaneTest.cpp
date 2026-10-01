// DL-214: variable alpha work cannot alias bounce dimensions or replay a lane.
#include <iostream>
#include <set>
#include "../src/Library/Utilities/SobolSampler.h"
#include "../src/Library/Utilities/PSSMLTSampler.h"
using namespace RISE;
using namespace RISE::Implementation;
static int pass=0,fail=0;
static void Check(bool b,const char* s){(b?pass:fail)++;if(!b)std::cout<<"FAIL "<<s<<'\n';}
struct Probe : PSSMLTSampler {
    Probe():PSSMLTSampler(73,.3){}
    unsigned Index() const{return sampleIndex;}
    int Stream() const{return streamIndex;}
    Scalar Stored(unsigned i){return FindOrCreateExtraStream(3072)[i].value;}
    size_t AlphaSize(){return FindOrCreateExtraStream(3072).size();}
};
int main(){
    SobolSampler sobol(29,73),reference(29,73);
    std::set<Scalar> values;
    bool match=true;
    for(unsigned i=0;i<8192;++i){
        sobol.StartStream(int(i%139264)); reference.StartStream(int(i%139264));
        const auto a=sobol.GetAlpha1D();values.insert(a);
        match &= a==SobolSequence::Sample(29,(1u<<29)+i,73);
        match &= sobol.Get1D()==reference.Get1D();
    }
    Check(match,"Sobol alpha is reserved dimension; normal stream unchanged");
    Check(values.size()==8192,"StartStream never reuses alpha dimensions");
    Probe p; p.StartIteration();p.StartStream(17);p.Get1D();
    const unsigned index=p.Index();const int stream=p.Stream();
    values.clear();for(int i=0;i<2048;++i){p.StartStream(17);values.insert(p.GetAlpha1D());}
    Check(p.AlphaSize()==2048 && values.size()==2048,"PSSMLT stream resets do not reset alpha counter");
    p.StartStream(stream);for(unsigned i=0;i<index;++i)p.Get1D();
    p.GetAlpha1D();Check(p.Index()==index && p.Stream()==stream,"alpha preserves normal primary vector cursor");
    p.Accept();const Scalar accepted=p.Stored(0);
    p.StartIteration();p.GetAlpha1D();Check(p.AlphaSize()==2049,"new mutation resets alpha cursor without allocating a new lane");
    p.Reject();Check(p.Stored(0)==accepted,"rejected alpha mutation restores accepted primary state");
    std::cout<<pass<<" passed / "<<fail<<" failed\n";return fail?1:0;
}
