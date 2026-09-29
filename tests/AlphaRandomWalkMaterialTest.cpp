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
    RandomNumberGenerator ordinary,alpha;unsigned calls=0,alphaCalls=0;
    explicit PairedSampler(unsigned seed):ordinary(seed),alpha(seed+90123){}
    Scalar Get1D() override {++calls;return ordinary.CanonicalRandom();}
    Point2 Get2D() override {return Point2(Get1D(),Get1D());}
    Scalar GetAlpha1D() override {++alphaCalls;return alpha.CanonicalRandom();}
};
struct CountSphere:SphereGeometry {
    mutable unsigned count=0;CountSphere():SphereGeometry(1){}
    void IntersectRay(RayIntersectionGeometric& ri,bool f,bool b,bool e)const override {++count;SphereGeometry::IntersectRay(ri,f,b,e);}
};

#include "../src/Library/Objects/CSGObject.h"

class ShiftUV:public IRayIntersectionModifier,public Reference{public:void Modify(RayIntersectionGeometric& r)const override{r.ptCoord.x=2;}};
class UVAlpha:public IScalarPainter,public Reference{public:ScalarTriple GetValuesAt(const RayIntersectionGeometric& r)const override{return ScalarTriple(r.ptCoord.x>1.5?1:.5);}};
// Force the documented front-face fallback while retaining the same sphere
// boundary geometry. This is a branch-control fixture, not a physical mesh.
class FallbackObject:public Object {
public:
    explicit FallbackObject(IGeometry* g):Object(g){}
    void IntersectRay(RayIntersection& r,const Scalar d,const bool f,const bool b,const bool e)const override {
        if(!f && b)return;
        Object::IntersectRay(r,d,true,true,e);
    }
};
int main(){
 for(int kind=0;kind<8;++kind)for(Scalar nm:{0.,550.}){
    auto* ior=new UniformScalarPainter(1.3);auto* absorption=new RGBScalarPainter(.05,.05,.05);auto* scattering=new RGBScalarPainter(1,1,1);auto* coverage=new UVAlpha;
    auto* mat=new SubSurfaceScatteringMaterial(*ior,*absorption,*scattering,0,.2);
    auto* overrideMat=new SubSurfaceScatteringMaterial(*ior,*absorption,*scattering,0,.2);
    auto* geo=new CountSphere();Object* object=kind==7?new FallbackObject(geo):new Object(geo);
    object->AssignMaterial(*mat);object->FinalizeTransformations();
    auto* modifier=new ShiftUV;if(kind==2 || kind==7)object->AssignModifier(*modifier);
    Object* top=object;top->addref();
    // 0 plain; 1 inherited; 2 modified; 3 nested; 4 snapshot;
    // 5 opaque root override; 6 alpha root override; 7 modified fallback.
    if(kind==1 || (kind>=3 && kind<=6)){
        const int levels=kind==3?2:1;
        for(int level=0;level<levels;++level){
            auto* remote=new Object(geo);remote->AssignMaterial(*mat);remote->SetPosition(Point3(100*(level+1),0,0));remote->FinalizeTransformations();
            auto* composite=new CSGObject(CSG_UNION);composite->AssignObjects(top,remote);composite->FinalizeTransformations();top->release();remote->release();top=composite;
        }
    }
    if(kind==5 || kind==6)top->AssignMaterial(*overrideMat);
    // Snapshot clones scalar slots independently; change mode on both its
    // actual leaf binding and the live original for paired opaque/alpha runs.
    if(kind==4){auto* clone=top->CloneSnapshot();top->release();top=clone;}
    RayIntersectionGeometric ri(Ray(Point3(0,-3,0),Vector3(0,1,0)),nullRasterizerState);ri.bHit=true;ri.ptIntersection=Point3(0,-1,0);ri.ptObjIntersec=ri.ptIntersection;ri.vNormal=Vector3(0,-1,0);ri.vGeomNormal=ri.vNormal;ri.onb.CreateFromW(ri.vNormal);
    RayIntersection materialHit(Ray(Point3(0,-3,0),Vector3(0,1,0)),nullRasterizerState);top->IntersectRay(materialHit,10,true,true,false);
    auto* effective=const_cast<IMaterial*>(materialHit.pMaterial);
    Check(effective!=nullptr,"actual endpoint material exists");
    unsigned baseValid=0,alphaValid=0;bool proposals=true,weights=true,draws=true,modified=true;
    for(unsigned k=0;k<4000;++k){
        auto sample=[&](PairedSampler& sampler){return RandomWalkSSS::SampleExit(ri,top,RISEPel(.05),RISEPel(1),RISEPel(1.05),0,1.3,64,sampler,nm);};
        PairedSampler a(k+1),b(k+1);mat->SetAlpha(nullptr,eAlphaOpaque,.5);effective->SetAlpha(nullptr,eAlphaOpaque,.5);geo->count=0;const auto base=sample(a);const unsigned hits=geo->count;
        mat->SetAlpha(coverage,eAlphaBlend,.5);effective->SetAlpha(kind==5?nullptr:coverage,kind==5?eAlphaOpaque:eAlphaBlend,.5);geo->count=0;const auto alpha=sample(b);
        proposals &= geo->count==hits && a.calls==b.calls;
        draws &= a.alphaCalls==0 && b.alphaCalls==(base.valid && kind!=5?1u:0u);
        baseValid+=base.valid;alphaValid+=alpha.valid;
        if(alpha.valid){weights &= base.valid && base.pdfSurface==alpha.pdfSurface && base.cosinePdf==alpha.cosinePdf && base.weightNM==alpha.weightNM && base.weight.r==alpha.weight.r && alpha.acceptedAlphaCoverage==(kind==5?1:.5);if(kind==2 || kind==7)modified &= alpha.ptCoord.x==2;}
    }
    std::cout<<"kind="<<kind<<" nm="<<nm<<" opaque="<<baseValid<<" alpha="<<alphaValid<<std::endl;
    Check(baseValid>100,"valid opaque-domain endpoint control");
    Check(kind==5?alphaValid==baseValid:std::fabs(double(alphaValid)/baseValid-.5)<.035,"effective pre-modifier endpoint coverage including override");
    Check(proposals,"all geometric queries and ordinary proposal draws unchanged");
    Check(weights,"accepted coverage stamp PDFs and weights unchanged");
    Check(draws,"one final alpha draw only, no internal boundary draws");
    Check(modified,"shading modifier still reaches returned endpoint");
    top->release();object->release();modifier->release();geo->release();overrideMat->release();mat->release();coverage->release();scattering->release();absorption->release();ior->release();
 }
 std::cout<<pass<<" passed / "<<fail<<" failed"<<std::endl;return fail?1:0;
}
