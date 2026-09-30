// DL-334: public painter/material spectral boundary regression.
// Oracle uses scalar Snell + polarized Fresnel and midpoint quadrature,
// independently of Optics and BSSRDFSampling's production boundary helpers.
#include <cmath>
#include <cstdio>
#include <vector>
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Interfaces/ISubSurfaceDiffusionProfile.h"
#include "../src/Library/Shaders/BSSRDFEntryAdapters.h"
#include "../src/Library/Utilities/PathVertexEval.h"
#include "../src/Library/Utilities/IndependentSampler.h"
using namespace RISE;
static int checks=0, failures=0;
static double Fresnel(double mu,double ni,double nt) {
    if(ni==nt) return 0;
    const double st2=(ni/nt)*(ni/nt)*(1-mu*mu);
    if(st2>=1) return 1;
    const double ct=std::sqrt(1-st2);
    const double rs=(ni*mu-nt*ct)/(ni*mu+nt*ct);
    const double rp=(nt*mu-ni*ct)/(nt*mu+ni*ct);
    return (rs*rs+rp*rp)/2;
}
static double Norm(double ni,double nt) {
    // Transform through Snell below unity to avoid integrating across TIR.
    if(nt<ni) return (nt/ni)*(nt/ni)*Norm(nt,ni);
    double s=0; const int n=100000;
    for(int i=0;i<n;i++){ double mu=(i+0.5)/n; s+=2*mu*(1-Fresnel(mu,ni,nt))/n; }
    return s;
}
static void Check(bool ok,const char* label,double nm,double actual,double expected) {
    checks++; if(!ok){failures++; std::printf("FAIL %s nm=%.1f actual=%.12g expected=%.12g\n",label,nm,actual,expected);}
}
int main() {
    IScalarPainter *sell=nullptr,*curve=nullptr,*constant=nullptr,*zero=nullptr,*scatter=nullptr;
    RISE_API_CreateSellmeierScalarPainter(&sell,1.03961212,0.231792344,1.01046945,0.00600069867,0.0200179144,103.560653);
    RISE_API_CreatePiecewiseLinearScalarPainter(&curve,{{380,1.8},{465,1.6},{549,1.35},{611,1.2},{780,1.1}});
    RISE_API_CreateUniformScalarPainter(&constant,1.5);
    RISE_API_CreateUniformScalarPainter(&zero,0);
    RISE_API_CreateUniformScalarPainter(&scatter,100);
    double worst=0,worstNM=0;
    for(IScalarPainter* raw:{sell,curve,constant}) {
        // Mirror the parser's single-scalar IOR slot binding for spectral curves.
        IScalarPainter* view=raw->MakeSingleScalarSlotView();
        const IScalarPainter& ior=view?*view:*raw;
        IMaterial *diff=nullptr,*rw=nullptr;
        RISE_API_CreateSubSurfaceScatteringMaterial(&diff,ior,*zero,*scatter,0,0);
        RISE_API_CreateRandomWalkSSSMaterial(&rw,ior,*zero,*scatter,0,0,512);
        for(double nm:{380.,420.,465.,500.,549.,611.,700.,780.}) {
            RayIntersectionGeometric ri(Ray(Point3(0,0,0),Vector3(0,0,-1)),nullRasterizerState);
            ri.bHit=true; ri.vNormal=ri.vGeomNormal=Vector3(0,0,1); ri.onb.CreateFromW(ri.vNormal);
            const double nt=ior.GetValueAtNM(ri,nm);
            // Air, immersed, matched and TIR controls. Ambient may itself be dispersive.
            for(double ni:{1.,curve->GetValueAtNM(ri,nm),nt,nt*1.2}) {
                const double c=Norm(ni,nt); ri.ambientIOR=ni;
                BSSRDFAdapters::BSSRDFEntryBSDF adapter(diff->GetDiffusionProfile(),0);
                for(double mu:{1.,0.8,0.5,0.1,0.001}) {
                    const Vector3 wi(std::sqrt(1-mu*mu),0,mu); ri.ray=Ray(Point3(0,0,0),-wi);
                    IORStack stack(ni); Implementation::RandomNumberGenerator rng; IndependentSampler sampler(rng);
                    ScatteredRayContainer rays; diff->GetSPF()->ScatterNM(ri,sampler,rays,nm,stack);
                    double r=0; for(unsigned j=0;j<rays.Count();j++)r+=rays[j].krayNM;
                    const double ref=Fresnel(mu,ni,nt);
                    Check(std::fabs(r-ref)<1e-6,"public SPF reflection",nm,r,ref);
                    const double sw=adapter.valueNM(wi,ri,nm);
                    const double t=sw*c*PI;
                    Check(std::fabs(t-(1-ref))<1e-6,"spectral adapter transmission",nm,t,1-ref);
                    const double err=std::fabs(r+t-1); if(err>worst){worst=err;worstNM=nm;}
                    Check(err<1e-6,"public NM R+T",nm,r+t,1);
                    BDPTVertex vertex; vertex.pMaterial=diff; vertex.type=BDPTVertex::SURFACE; vertex.isBSSRDFEntry=true;
                    vertex.position=Point3(0,0,0);vertex.normal=vertex.geomNormal=ri.vNormal;vertex.onb=ri.onb;vertex.mediumIOR=ni;
                    const double bsdf=PathVertexEval::EvalBSDFAtVertexNM(vertex,wi,wi,nm);
                    Check(std::fabs(bsdf-sw)<1e-10,"NM shared consumer",nm,bsdf,sw);
                }
                RandomWalkSSSParams params;
                const bool has=rw->GetRandomWalkSSSParamsNM(nm,params);
                Check(has,"public RW spectral params",nm,has,1);
                if(has) Check(std::fabs(params.ior-nt)<1e-12,"RW index at wavelength",nm,params.ior,nt);
            }
        }
        diff->release();rw->release();if(view)view->release();
    }
    std::printf("Worst public partition nm=%.1f error=%.12g\nChecks: %d Failures: %d\n",worstNM,worst,checks,failures);
    sell->release();curve->release();constant->release();zero->release();scatter->release();
    return failures?1:0;
}
