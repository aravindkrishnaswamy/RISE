// DL-214: alpha seed thinning needs an unbiased reciprocal beyond the old cap.
#include <cmath>
#include <iostream>
#include "../src/Library/Utilities/ManifoldSolver.h"
#include "../src/Library/Utilities/IndependentSampler.h"
using namespace RISE; using namespace RISE::Implementation;
static int pass=0,fail=0;
static void Check(bool b,const char* label){(b?pass:fail)++;if(!b)std::cout<<"FAIL "<<label<<'\n';}
struct EndTail : ISampler {
    Scalar Get1D() override {return .999999;}
    Point2 Get2D() override {return Point2(Get1D(),Get1D());}
};
int main(){
    EndTail end;SMSReciprocalTail first(3);
    Check(first.Estimate()==1,"initial K term is one");
    Check(first.ContinueAfterFailure(end)&&first.ContinueAfterFailure(end),"prefix retained through B-1 failures");
    Check(!first.ContinueAfterFailure(end)&&first.Estimate()==3,"roulette termination returns prefix, never discards it");
    for(Scalar p:{.5,.1,.01,1./1024.})for(unsigned B:{64u,1024u}){
        const int N=(p<.001 && B==64)?6000000:100000;
        RandomNumberGenerator random(214u+B);IndependentSampler sampler(random);
        double sum=0,sq=0,work=0;
        for(int i=0;i<N;++i){
            SMSReciprocalTail tail(B);
            for(;;){++work;if(random.CanonicalRandom()<p)break;if(!tail.ContinueAfterFailure(sampler))break;}
            const double x=tail.Estimate();sum+=x;sq+=x*x;
        }
        const double mean=sum/N,variance=(sq-sum*sum/N)/(N-1),se=std::sqrt(variance/N);
        std::cout<<"p="<<p<<" B="<<B<<" mean="<<mean<<" expected="<<1/p<<" SE="<<se<<" meanTrials="<<work/N<<'\n';
        Check(std::fabs(mean-1/p)<6*se+.001,"reciprocal mean matches 1/p within six standard errors");
        Check(std::fabs(mean*p-1)<.08,"absolute relative accuracy floor, independent of estimated variance");
        Check(work/N<2*B+1,"mean work bounded by tail survival budget");
    }
    std::cout<<pass<<" passed / "<<fail<<" failed\n";return fail?1:0;
}
