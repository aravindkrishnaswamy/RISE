// Two-sided compensated-furnace, adjoint, sampler/PDF and collapse oracles.
#include "../src/Library/Utilities/RoughDielectricEnergy.h"
#include <iostream>
#include <random>
#include <cmath>
using namespace RISE;
namespace RD=MicrofacetUtils::RoughDielectric;
namespace MS=RoughDielectricEnergy;
static int passed=0,failed=0;
static void Check(bool ok,const char* name) { if(ok) ++passed; else { ++failed; if(failed<20) std::cout<<"FAIL: "<<name<<'\n'; } }
static bool Close(Scalar a,Scalar b,Scalar e=1e-9) { return std::isfinite(a) && std::isfinite(b) && std::abs(a-b)<=e*std::max(Scalar(1),std::max(std::abs(a),std::abs(b))); }
static Vector3 Unit(Scalar x,Scalar y,Scalar z) { return RD::NormalizeScaled(Vector3(x,y,z),Vector3(1,1,1)); }
static Vector3 Other(const Vector3& w) { return Vector3(-w.x,w.y,-w.z); }
static std::mt19937_64 rng(123456);
static Scalar Random() { return std::generate_canonical<Scalar,53>(rng); }

static void Furnace() {
    Scalar maxError=0;
    for(Scalar ax : {.03,.2,.7,1.}) for(Scalar ratio : {.04,.4,1.})
    for(Scalar mu : {.05,.3,1.}) for(Scalar eta : {1./3,2./3,1.25,1.5,3.}) {
        const Vector3 wi(std::sqrt((1-mu)*(1+mu))*.8,std::sqrt((1-mu)*(1+mu))*.6,mu);
        MS::Model model(wi,ax,ax*ratio,eta);
        Scalar single=0;
        constexpr int n=16384;
        for(int i=0;i<n;++i) {
            const auto draw=RD::Draw(wi,ax,ax*ratio,eta,Random(),Random(),Random(),false);
            if(draw.valid) single+=draw.weight;
        }
        const Scalar total=single/n+model.MissingEnergy();
        maxError=std::max(maxError,std::abs(total-1));
        if(std::abs(total-1)>.035) std::cout<<"furnace ax="<<ax<<" ratio="<<ratio<<" mu="<<mu<<" eta="<<eta<<" energy="<<total<<'\n';
        Check(std::abs(total-1)<.035,"combined R+T furnace against independent SS trials");
    }
    std::cout<<"maximum furnace error "<<maxError<<'\n';
    for(Scalar eta : {1-1e-8,1+1e-8,1.01,1/1.01,100.,.01}) for(Scalar mu : {1e-8,.001,.05}) {
        const Vector3 wi(std::sqrt((1-mu)*(1+mu)),0,mu);
        MS::Model model(wi,.7,.7,eta);
        Scalar sum=0;
        for(int i=0;i<65536;++i) {
            const auto s=RD::Draw(wi,.7,.7,eta,Random(),Random(),Random(),false);
            if(s.valid) sum+=s.weight;
        }
        const Scalar energy=sum/65536+model.MissingEnergy();
        if(std::abs(energy-1)>.05) std::cout<<"boundary eta "<<eta<<" mu "<<mu<<" energy "<<energy<<'\n';
        Check(std::abs(energy-1)<.05,"near-matched/grazing and high-contrast furnace calibration");
    }
}
static void Identities() {
    const Vector3 wi=Unit(.7,.2,1);
    for(Scalar eta : {1./3,2./3,1.25,1.5,3.}) {
        MS::Model m(wi,.2,.7,eta);
        for(int i=0;i<400;++i) {
            const Vector3 wo=Unit(2*Random()-1,2*Random()-1,2*Random()-1);
            const auto f=m.Evaluate(wo,true);
            const Vector3 reverseWi=wo.z>0 ? wo : Other(wo);
            const Vector3 reverseWo=wo.z>0 ? wi : Other(wi);
            const auto reverse=MS::Model(reverseWi,.2,.7,wo.z>0 ? eta : 1/eta).Evaluate(reverseWo,false);
            Check(Close(f.f,reverse.f),"adjoint reciprocity, including opposite-side tail");
            const auto axes=MS::Model(Vector3(wi.y,wi.x,wi.z),.7,.2,eta).Evaluate(Vector3(wo.y,wo.x,wo.z),true);
            Check(Close(f.f,axes.f) && Close(f.pdf,axes.pdf),"axis exchange symmetry");
            const auto raw=RD::Evaluate(wi,wo,.2,.7,eta,true);
            Check(f.f>=raw.f,"compensation adds nonnegative energy");
        }
    }
    for(Scalar u : {Scalar(0),Scalar(.5),std::nextafter(Scalar(1),Scalar(0))}) {
        const auto equal=MS::Model(wi,.2,.7,1).Draw(u,u,u,u,true);
        Check(equal.valid && equal.delta && equal.transmission && equal.weight==1,"equal-index delta remains exact");
        const auto smooth=MS::Model(wi,0,0,1.5).Draw(u,u,u,u,false);
        Check(smooth.valid && smooth.delta,"smooth delta has no compensation");
        const auto tail=MS::Model(wi,.4,.7,1.5).Draw(u,u,.5,0,false);
        Check(tail.valid && !tail.delta && tail.pdf>0 && std::isfinite(tail.weight),"tail inverse CDF endpoints are valid");
    }
}
static void Sampler() {
    const Vector3 wi=Unit(.6,.2,1);
    for(Scalar eta : {2./3,1.5}) {
        MS::Model m(wi,.4,.7,eta);
        Scalar integralPdf=0,integralEnergy=0;
        constexpr int nMu=512,nPhi=256,n=100000;
        for(int side : {-1,1}) for(int c=0;c<nMu;++c) for(int p=0;p<nPhi;++p) {
            const Scalar mu=(c+.5)/nMu,phi=TWO_PI*(p+.5)/nPhi,r=std::sqrt(1-mu*mu);
            const Vector3 wo(r*std::cos(phi),r*std::sin(phi),side*mu);
            const auto f=m.Evaluate(wo,false);
            integralPdf+=f.pdf*TWO_PI/(nMu*nPhi);
            integralEnergy+=f.f*mu*TWO_PI/(nMu*nPhi);
        }
        Scalar weight=0,accepted=0;
        for(int k=0;k<n;++k) {
            const auto s=m.Draw(Random(),Random(),Random(),Random(),false);
            if(!s.valid) continue;
            ++accepted; weight+=s.weight;
            const auto e=m.Evaluate(s.wo,false);
            Check(Close(s.pdf,e.pdf) && Close(s.f,e.f),"sample/evaluation aggregate density and value");
            Check(Close(s.weight,s.f*std::abs(s.wo.z)/s.pdf),"sample f*cos/pdf includes the tail exactly once");
        }
        std::cout<<"eta "<<eta<<" accepted "<<accepted/n<<" pdf "<<integralPdf<<" sampled energy "<<weight/n<<" integrated "<<integralEnergy<<'\n';
        Check(std::abs(accepted/n-integralPdf)<.008,"accepted sample mass vs independent spherical quadrature");
        Check(std::abs(weight/n-integralEnergy)<.012,"sampled energy vs independent spherical quadrature");
        Check(std::abs(weight/n-1)<.025,"compensated sampling furnace");
    }
}
static void ScaledLimits() {
    for(Scalar alpha : {1e-200,1e-100,.2}) for(Scalar eta : {1e-150,1.5,1e150}) {
        const auto draw=MS::Model(Vector3(0,0,1),alpha,alpha,eta).Draw(.25,.25,0,.9,true);
        Check(draw.valid && std::isfinite(draw.weight),"scaled mixture retains representable radiance throughput");
        Check(!std::isnan(draw.f) && !std::isnan(draw.pdf),"scaled mixture density/value do not form inf/inf");
    }
    const auto invalid=MS::Model(Vector3(0,0,1),.2,.2,0).Evaluate(Vector3(0,0,-1),true);
    Check(invalid.f==0 && invalid.pdf==0,"invalid indices remain a null evaluator");
    for(Scalar alpha : {1e-300,1e-200,1e-100}) {
        const auto draw=MS::Model(Unit(10,0,1),alpha,alpha,1.5).Draw(0,0,.5,0,false);
        Check(draw.valid && std::isfinite(draw.weight),"tiny-profile CDF retains a finite valid sample");
    }
}
int main() {
    Identities(); Furnace(); Sampler(); ScaledLimits();
    std::cout<<passed<<" passed, "<<failed<<" failed\n";
    return failed ? 1 : 0;
}
