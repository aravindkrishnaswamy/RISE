// DL-313: independent diffusion-equation Green function, not an image sum.
#include "../src/Library/Materials/MultipoleDiffusion.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace RISE;
static int passed=0,failed=0;
static void Check(bool b,const char* label) { if(b) ++passed; else { ++failed; std::printf("FAIL %s\n",label); } }
static LayerParams Layer(double a,double s,double d,double n)
{ LayerParams p={};p.sigma_a=a;p.sigma_sp=s;p.thickness=d;p.ior=n;ComputeLayerDerivedParams(p);return p; }
// Integrate exact unpolarized Fresnel over internal cosine, independently.
static double Fdr(double eta)
{
    double sum=0;
    const int N=200000;
    for(int i=0;i<N;++i) {
        double mu=(i+0.5)/N, t2=1-eta*eta*(1-mu*mu),f=1;
        if(t2>0) {
            double t=std::sqrt(t2),s=(eta*mu-t)/(eta*mu+t),p=(mu-eta*t)/(mu+eta*t);
            f=(s*s+p*p)/2;
        }
        sum+=2*mu*f;
    }
    return sum/N;
}
static double A(double eta) { double f=Fdr(eta); return (1+f)/(1-f); }
struct Response { double r,t; };
// Solution of (-d^2/dz^2 + q^2)G=delta(z-zr), with G(-a)=G(d+b)=0.
// Integrating its outward flux at the physical slab faces gives these values.
static Response Ode(const LayerParams& p,double topEta,double bottomEta,double frequency)
{
    double a=2*A(topEta)*p.D,b=2*A(bottomEta)*p.D,d=p.thickness,z=p.z_r;
    if(d<z) return {0,std::exp(-p.sigma_a*d)};
    double q=std::sqrt(p.sigma_tr*p.sigma_tr+frequency*frequency),L=d+a+b;
    if(q==0) return {p.alpha_prime*(d+b-z)/L,p.alpha_prime*(z+a)/L};
    return {p.alpha_prime*std::cosh(q*a)*std::sinh(q*(d+b-z))/std::sinh(q*L),
            p.alpha_prime*std::sinh(q*(z+a))*std::cosh(q*b)/std::sinh(q*L)};
}
int main()
{
    const double frequency[]={0,0.1,1,4};
    LayerParams layers[]={Layer(0.1,20,0.12,1.4),Layer(0.2,18,0.3,1.38)};
    double actual[4]={};ComputeCompositeProfileHankel(layers,2,frequency,4,64,actual);
    for(int i=0;i<4;++i) {
        const Response up=Ode(layers[0],1.4,1.4/1.38,frequency[i]);
        const Response down=Ode(layers[0],1.4/1.38,1.4,frequency[i]);
        const Response lower=Ode(layers[1],1.38/1.4,1.38,frequency[i]);
        const double f=(1-Fdr(1.4/1.38))*(1-Fdr(1.38/1.4));
        const double expected=up.r+up.t*f*lower.r*down.t/(1-down.r*f*lower.r);
        std::printf("internal f=%g actual=%.10f expected=%.10f relative=%g\n",frequency[i],actual[i],expected,actual[i]/expected);
        // Egan/Hilgeman is an approximation to the independently integrated law.
        Check(std::fabs(actual[i]-expected)<0.006*expected,"per-face ODE and reverse-face return");
    }
    LayerParams thick[]={Layer(0.1,20,100,1.4),Layer(0.2,18,0.3,1.38)};
    ComputeCompositeProfileHankel(thick,2,frequency,4,64,actual);
    for(int i=0;i<4;++i) {
        const double q=std::sqrt(thick[0].sigma_tr*thick[0].sigma_tr+frequency[i]*frequency[i]);
        const double expected=thick[0].alpha_prime/2*(std::exp(-thick[0].z_r*q)
            +std::exp(-(thick[0].z_r+4*A(1.4)*thick[0].D)*q));
        Check(std::fabs(actual[i]-expected)<0.006*expected,"thick epidermis semi-infinite exterior dipole");
    }
    LayerParams split[]={Layer(0.1,20,0.12,1.4),Layer(0.1,20,0.3,1.4)};
    const LayerParams combined=Layer(0.1,20,0.42,1.4);
    ComputeCompositeProfileHankel(split,2,frequency,4,64,actual);
    for(int i=0;i<4;++i) {
        const double expected=Ode(combined,1.4,1.4,frequency[i]).r;
        std::printf("matched f=%g actual=%.10f expected=%.10f\n",frequency[i],actual[i],expected);
        Check(std::fabs(actual[i]-expected)<0.006*expected,"matched homogeneous internal interface disappears");
    }
    const LayerParams conservative=Layer(0,20,0.2,1.4);
    double r=0,t=0,s=0;
    EvaluateMultipoleReflectanceHankel(conservative,&s,1,&r,&t,64);
    const Response expected=Ode(conservative,1.4,1.4,0);
    Check(std::fabs(r-expected.r)<0.006,"zero-frequency conservative finite-slab reflectance");
    Check(std::fabs(t-expected.t)<0.006,"zero-frequency conservative finite-slab transmission");
    std::printf("Passed: %d\nFailed: %d\n",passed,failed);
    return failed?1:0;
}
