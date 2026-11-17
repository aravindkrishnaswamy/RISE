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
class UVMask:public IScalarPainter,public Reference{public:ScalarTriple GetValuesAt(const RayIntersectionGeometric& r)const override{return ScalarTriple(r.ptCoord.x>1.5?1:0);}};
int main(){
 auto* ior=new UniformScalarPainter(1.3);auto* absorption=new RGBScalarPainter(.05,.05,.05);auto* scattering=new RGBScalarPainter(1,1,1);auto* coverage=new UVMask;
 auto* entryMat=new SubSurfaceScatteringMaterial(*ior,*absorption,*scattering,0,.2);auto* otherMat=new SubSurfaceScatteringMaterial(*ior,*absorption,*scattering,0,.2);
 auto* geo=new CountSphere();auto* a=new Object(geo);a->AssignMaterial(*entryMat);a->SetPosition(Point3(-.5,0,0));a->FinalizeTransformations();auto* b=new Object(geo);b->AssignMaterial(*otherMat);b->SetPosition(Point3(.5,0,0));b->FinalizeTransformations();
 auto* csg=new CSGObject(CSG_UNION);csg->AssignObjects(a,b);csg->FinalizeTransformations();
 const Ray ray(Point3(-.01,-3,0),Vector3(0,1,0));RayIntersection entry(ray,nullRasterizerState);csg->IntersectRay(entry,RISE_INFINITY,true,true,false);
 Check(entry.geometric.bHit&&entry.pMaterial==entryMat,"entry comes from opaque left operand");
 auto* modifier=new ShiftUV;
 for(bool modified:{false,true})for(Scalar nm:{0.,550.}){
 if(modified)b->AssignModifier(*modifier);
 bool proposals=true,weights=true,draws=true;
 unsigned baseRight=0,alphaRight=0,baseLeft=0,alphaLeft=0,identityMatches=0,baseIdentityMatches=0;double stamp=0;bool modifierPreserved=true;
 for(unsigned k=0;k<4000;++k){
 PairedSampler u(k+1),v(k+1);otherMat->SetAlpha(nullptr,eAlphaOpaque,.5);geo->count=0;const auto base=BSSRDFSampling::SampleEntryPoint(entry.geometric,csg,entryMat,u,nm);
 const unsigned baseHits=geo->count;geo->count=0;otherMat->SetAlpha(coverage,eAlphaMask,.5);const auto alpha=BSSRDFSampling::SampleEntryPoint(entry.geometric,csg,entryMat,v,nm);
 proposals &= geo->count==baseHits;draws &= u.alphaCalls==0 && v.alphaCalls==0;
 if(alpha.valid)weights &= base.valid && base.pdfSurface==alpha.pdfSurface && base.weightNM==alpha.weightNM && base.weight.r==alpha.weight.r && u.calls==v.calls && alpha.acceptedAlphaCoverage==1;
 if(base.valid){if(base.entryPoint.x>0){
 ++baseRight;
 const Ray probe(Point3Ops::mkPoint3(base.entryPoint,base.entryGeomNormal*1e-4),-base.entryGeomNormal);RayIntersection hit(probe,nullRasterizerState);csg->IntersectRay(hit,1e-3,true,true,false);
 if(hit.geometric.bHit&&hit.pMaterial==otherMat&&hit.pMaterial->AlphaCoverage(hit.geometric)==0)++baseIdentityMatches;
 if(modified)modifierPreserved &= base.ptCoord.x==2;
 }else ++baseLeft;}
 if(alpha.valid){if(alpha.entryPoint.x>0){++alphaRight;stamp=alpha.acceptedAlphaCoverage;const Ray probe(Point3Ops::mkPoint3(alpha.entryPoint,alpha.entryGeomNormal*1e-4),-alpha.entryGeomNormal);RayIntersection hit(probe,nullRasterizerState);csg->IntersectRay(hit,1e-3,true,true,false);if(hit.geometric.bHit&&hit.pMaterial==otherMat&&hit.pMaterial->AlphaCoverage(hit.geometric)==0)++identityMatches;}else ++alphaLeft;}
 }
 std::cout<<"modified="<<modified<<" nm="<<nm<<" opaqueRight="<<baseRight<<" maskedRight="<<alphaRight<<" opaqueLeft="<<baseLeft<<" maskedLeft="<<alphaLeft<<" confirmedRightMaterial="<<identityMatches<<" maskedStamp="<<stamp<<std::endl;
 Check(baseIdentityMatches==baseRight,"every opaque proposed right endpoint independently has MASK0 material");Check(modifierPreserved,"endpoint shading UV modifier remains active");
 Check(proposals,"all geometric chord candidate counts unchanged");Check(weights,"retained endpoint PDFs weights and ordinary draws unchanged");Check(draws,"deterministic MASK uses no random alpha draw");
 Check(identityMatches==alphaRight,"selected right endpoints really carry MASK0 right material");Check(baseRight>100,"geometric kernel reaches right endpoint material");Check(alphaRight==0,"MASK0 right endpoints rejected");Check(alphaLeft==baseLeft,"opaque left endpoints preserved");
 }
 modifier->release();csg->release();a->release();b->release();geo->release();entryMat->release();otherMat->release();coverage->release();scattering->release();absorption->release();ior->release();
 std::cout<<pass<<" passed / "<<fail<<" failed"<<std::endl;return fail?1:0;
}
