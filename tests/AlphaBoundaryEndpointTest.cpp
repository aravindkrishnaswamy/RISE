// DL-214: a single sampled boundary decision controls both visibility and media.
#include <cmath>
#include <iostream>
#include <vector>
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Scene.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Shaders/BDPTIntegrator.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Materials/HomogeneousMedium.h"
#include "../src/Library/Materials/IsotropicPhaseFunction.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Utilities/ISampler.h"
#include "../src/Library/RISE_API.h"
using namespace RISE;
using namespace RISE::Implementation;
static int passed=0, failed=0;
static void Check(bool b,const char* label) { (b ? passed : failed)++; if(!b) std::cout<<"FAIL "<<label<<'\n'; }
struct Decisions : ISampler {
    std::vector<Scalar> values; size_t count=0; int normalDraws=0;
    explicit Decisions(std::initializer_list<Scalar> v):values(v){}
    Scalar Get1D() override { ++normalDraws; return .5; }
    Point2 Get2D() override { return Point2(Get1D(),Get1D()); }
    Scalar GetAlpha1D() override { return count<values.size()?values[count++]: (++count,.9); }
};
int main() {
 auto* color=new UniformColorPainter(RISEPel(.5));auto* alpha=new UniformScalarPainter(.5);
 auto* mat=new LambertianMaterial(*color);mat->SetAlpha(alpha,eAlphaBlend,.5);
 auto* phase=new IsotropicPhaseFunction();auto* medium=new HomogeneousMedium(RISEPel(1.),RISEPel(0.),*phase);
 for(int nested=0;nested<4;++nested)for(int front=0;front<2;++front)for(int snapshot=0;snapshot<2;++snapshot){
  auto* sphere=new SphereGeometry(1);auto* object=new Object(sphere);object->SetStretch(Vector3(2,4,2));object->FinalizeTransformations();sphere->release();
  Object* top=object;
  if(nested==1)for(int layer=0;layer<2;++layer){
   auto* g=new SphereGeometry(1);auto* remote=new Object(g);remote->SetPosition(Point3(100,0,0));remote->FinalizeTransformations();
   auto* csg=new CSGObject(CSG_UNION);csg->AssignObjects(top,remote);csg->SetStretch(Vector3(2,4,.5));csg->FinalizeTransformations();top->release();remote->release();g->release();top=csg;
  }
  if(nested>=2){
   auto* g=new SphereGeometry(1);auto* inner=new Object(g);inner->FinalizeTransformations();
   auto* csg=new CSGObject(nested==2?CSG_SUBTRACTION:CSG_INTERSECTION);csg->AssignObjects(top,inner);csg->FinalizeTransformations();
   top->release();inner->release();g->release();top=csg;
  }
  top->AssignMaterial(*mat);top->AssignInteriorMedium(*medium);top->SetShadowParams(false,true);
  if(snapshot){Object* clone=top->CloneSnapshot();top->release();top=clone;}
  auto* manager=new ObjectManager(false,false,4,8);manager->AddItem(top,"endpoint");
  const Scalar radius=nested==1?.5:nested>=2?1.:2.;
  const Ray ray(Point3(0,0,front?(nested==2?-3.:-2*radius):0),Vector3(0,0,1));
  const Scalar end=radius;MediumBoundaryHits exact,near;Decisions a{.1,.1},b{.1,.1};
  manager->IntersectShadowRaySampled(ray,end,a,&exact);
  manager->IntersectShadowRaySampled(ray,std::nextafter(end,RISE_INFINITY),b,&near);
  std::cout<<"nested="<<nested<<" front="<<front<<" snapshot="<<snapshot<<" exact="<<exact.size()<<" draws="<<a.count<<" near="<<near.size()<<" draws="<<b.count<<std::endl;
  Check(exact.empty()&&a.count==0,"exact physical endpoint is excluded before alpha draw");
  Check(near.size()==1&&b.count==1,"nearby real interior boundary retained with one draw");
  if(near.size()==1){
   const RayIntersection copy(near[0]);
   Check(copy.hasBoundaryRange&&std::fabs(copy.boundaryRange-end)<1e-12,"copy preserves raw caller-frame boundary parameter");
   Check(copy.geometric.range<copy.boundaryRange,"published shading backoff remains unchanged");
  }
  top->release();manager->release();
 }
 medium->release();phase->release();mat->release();alpha->release();color->release();
 std::cout<<passed<<" passed / "<<failed<<" failed"<<std::endl;return failed?1:0;
}
