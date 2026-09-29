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
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Painters/ExpressionPainter.h"
class RasterCoverage:public IScalarPainter,public Reference {
public:ScalarTriple GetValuesAt(const RayIntersectionGeometric& r)const override {
 return ScalarTriple(r.rast.x==17 && r.rast.y==29 ? .5 : 1);
}
};
int main(){
 auto* ior=new UniformScalarPainter(1.3);auto* absorption=new RGBScalarPainter(.05,.05,.05);auto* scattering=new RGBScalarPainter(1,1,1);
 auto* mat=new SubSurfaceScatteringMaterial(*ior,*absorption,*scattering,0,.2);auto* geo=new CountSphere();auto* object=new Object(geo);object->AssignMaterial(*mat);object->FinalizeTransformations();
 auto* enclosingGeo=new SphereGeometry(5);auto* enclosing=new Object(enclosingGeo);enclosing->FinalizeTransformations();
 auto* manager=new ObjectManager(false,false,4,8);manager->AddItem(object,"sss");manager->AddItem(enclosing,"enclosing");
 ExpressionProgram prog=ExpressionProgram::Invalid();ExpressionProgram::Builder builder;builder.EnableContextVars(true);
 Check(builder.Finalize("1-interior(2)",prog),"production interior expression compiles");
 std::vector<ParamSpec> params;auto* expression=new ExpressionScalarPainter(prog,params);auto* raster=new RasterCoverage;
 const RasterizerState rasterState={17,29};RayIntersection entry(Ray(Point3(0,-3,0),Vector3(0,1,0)),rasterState);manager->IntersectRay(entry,true,true,false);
 Check(entry.geometric.bHit&&entry.pObject==object,"manager hit reaches SSS sphere inside enclosing sphere");
 Check(expression->GetValuesAt(entry.geometric).v[0]==0,"manager scene context yields closed-form zero coverage");
 Check(raster->GetValuesAt(entry.geometric).v[0]==.5,"manager raster context yields half coverage");
 for(bool walk:{false,true})for(bool rasterMode:{false,true})for(Scalar nm:{0.,550.}){
  unsigned baseValid=0,alphaValid=0;bool proposals=true,stamps=true,draws=true,weights=true;
  for(unsigned k=0;k<4000;++k){
   auto sample=[&](PairedSampler& sampler){return walk?RandomWalkSSS::SampleExit(entry.geometric,object,RISEPel(.05),RISEPel(1),RISEPel(1.05),0,1.3,64,sampler,nm):BSSRDFSampling::SampleEntryPoint(entry.geometric,object,mat,sampler,nm);};
   PairedSampler a(k+1),b(k+1);mat->SetAlpha(nullptr,eAlphaOpaque,.5);geo->count=0;const auto base=sample(a);const unsigned hits=geo->count;
   mat->SetAlpha(rasterMode?static_cast<IScalarPainter*>(raster):expression,rasterMode?eAlphaBlend:eAlphaMask,.5);geo->count=0;const auto alpha=sample(b);
   proposals &= geo->count==hits;
   draws &= a.alphaCalls==0 && b.alphaCalls==(rasterMode&&base.valid?1u:0u);
   if(alpha.valid)weights &= base.valid && base.pdfSurface==alpha.pdfSurface && base.weight.r==alpha.weight.r && base.weightNM==alpha.weightNM && a.calls==b.calls;baseValid+=base.valid;alphaValid+=alpha.valid;
   if(alpha.valid)stamps &= alpha.acceptedAlphaCoverage==.5;
  }
  std::cout<<"walk="<<walk<<" raster="<<rasterMode<<" nm="<<nm<<" opaque="<<baseValid<<" alpha="<<alphaValid<<std::endl;
  Check(baseValid>100,"valid opaque proposal control");Check(proposals,"coverage leaves geometric query count unchanged");
  Check(rasterMode?std::fabs(double(alphaValid)/baseValid-.5)<.035:alphaValid==0,"endpoint alpha sees forwarded scene and raster context");
  Check(draws,"only selected endpoint draws alpha");Check(weights,"accepted ordinary draws PDF and weights unchanged");
  Check(stamps,"accepted context-sensitive coverage stamp is exact");
 }
 manager->release();enclosing->release();enclosingGeo->release();object->release();geo->release();mat->release();expression->release();raster->release();scattering->release();absorption->release();ior->release();
 std::cout<<pass<<" passed / "<<fail<<" failed"<<std::endl;return fail?1:0;
}
