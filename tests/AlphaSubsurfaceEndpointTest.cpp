// DL-214: coverage multiplies the selected nonlocal kernel endpoint;
// its geometric chord/domain proposal and PDF remain unchanged.
#include <cmath>
#include <iostream>
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Materials/SubSurfaceScatteringMaterial.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Utilities/BSSRDFSampling.h"
#include "../src/Library/Utilities/RandomWalkSSS.h"
using namespace RISE;using namespace RISE::Implementation;
static int pass=0,fail=0;
static void Check(bool b,const char* s){(b?pass:fail)++;if(!b)std::cout<<"FAIL "<<s<<'\n';}
struct PairedSampler:ISampler {
    RandomNumberGenerator ordinary,alpha;unsigned calls=0;
    explicit PairedSampler(unsigned seed):ordinary(seed),alpha(seed+90123){}
    Scalar Get1D() override {++calls;return ordinary.CanonicalRandom();}
    Point2 Get2D() override {return Point2(Get1D(),Get1D());}
    Scalar GetAlpha1D() override {return alpha.CanonicalRandom();}
};
struct CountSphere:SphereGeometry {
    mutable unsigned count=0;CountSphere():SphereGeometry(1){}
    void IntersectRay(RayIntersectionGeometric& ri,bool f,bool b,bool e)const override {++count;SphereGeometry::IntersectRay(ri,f,b,e);}
};
int main(){
    auto* ior=new UniformScalarPainter(1.3);auto* absorption=new RGBScalarPainter(.05,.05,.05);auto* scattering=new RGBScalarPainter(1,1,1);auto* coverage=new UniformScalarPainter(.5);
    auto* mat=new SubSurfaceScatteringMaterial(*ior,*absorption,*scattering,0,.2);auto* geo=new CountSphere();auto* object=new Object(geo);object->AssignMaterial(*mat);object->FinalizeTransformations();
    RayIntersectionGeometric ri(Ray(Point3(0,-3,0),Vector3(0,1,0)),nullRasterizerState);ri.bHit=true;ri.ptIntersection=Point3(0,-1,0);ri.ptObjIntersec=ri.ptIntersection;ri.vNormal=Vector3(0,-1,0);ri.vGeomNormal=ri.vNormal;ri.onb.CreateFromW(ri.vNormal);
    for(bool walk:{false,true})for(Scalar nm:{0.,550.}){
        unsigned baseValid=0,alphaValid=0;bool same=true;
        for(unsigned k=0;k<4000;++k){
            auto sample=[&](PairedSampler& sampler){return walk?RandomWalkSSS::SampleExit(ri,object,RISEPel(.05),RISEPel(1),RISEPel(1.05),0,1.3,64,sampler,nm):BSSRDFSampling::SampleEntryPoint(ri,object,mat,sampler,nm);};
            PairedSampler a(k+1),b(k+1);mat->SetAlpha(nullptr,eAlphaOpaque,.5);geo->count=0;const auto base=sample(a);const unsigned hits=geo->count;
            mat->SetAlpha(coverage,eAlphaBlend,.5);geo->count=0;const auto alpha=sample(b);
            same &= geo->count==hits;
            baseValid+=base.valid;alphaValid+=alpha.valid;
            if(alpha.valid){same &= base.valid && base.pdfSurface==alpha.pdfSurface && base.weightNM==alpha.weightNM && base.weight.r==alpha.weight.r && a.calls==b.calls && alpha.acceptedAlphaCoverage==.5;}
        }
        std::cout<<"walk="<<walk<<" nm="<<nm<<" base="<<baseValid<<" alpha="<<alphaValid<<'\n';
        Check(baseValid>100,"kernel produces enough valid endpoint proposals");
        Check(std::fabs(double(alphaValid)/baseValid-.5)<.035,"selected endpoint coverage has one factor");
        Check(same,"geometric candidate counts, proposal PDF and accepted weights unchanged");
    }
    safe_release(object);safe_release(geo);safe_release(mat);safe_release(coverage);safe_release(scattering);safe_release(absorption);safe_release(ior);
    std::cout<<pass<<" passed / "<<fail<<" failed\n";return fail?1:0;
}
