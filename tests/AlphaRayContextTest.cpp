// Null alpha traversal preserves camera ray lines, UV footprints and coverage context.
#include <cmath>
#include <iostream>
#include "../src/Library/Geometry/InfinitePlaneGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/CheckerPainter.h"
#include "../src/Library/Utilities/ISampler.h"
using namespace RISE;
using namespace RISE::Implementation;
static int passed=0,failed=0;
static void Check(bool ok,const char* name) { (ok?passed:failed)++; if(!ok) std::cout<<"FAIL "<<name<<'\n'; }
struct FixedSampler : ISampler {
    Scalar Get1D() override { return .5; }
    Point2 Get2D() override { return Point2(.5,.5); }
};
class ContextCoverage : public UniformScalarPainter {
public:
    ContextCoverage():UniformScalarPainter(0){}
    ScalarTriple GetValuesAt(const RayIntersectionGeometric& ri) const override {
        return ScalarTriple(ri.range>9.9 && std::fabs(ri.ray.origin.z+10)<1e-12 ? .8 : .2);
    }
};
static void Run(bool originOffsets) {
    auto* manager=new ObjectManager(false,false,4,8);
    auto* white=new UniformColorPainter(RISEPel(1,1,1));
    auto* black=new UniformColorPainter(RISEPel(0,0,0));
    auto* texture=new CheckerPainter(.1,*white,*black);
    auto* receiverMat=new LambertianMaterial(*texture);
    auto* holeMat=new LambertianMaterial(*white);
    auto* zero=new UniformScalarPainter(0); holeMat->SetAlpha(zero,eAlphaMask,.5);
    auto* plane=new InfinitePlaneGeometry(1,1);
    auto* sphere=new SphereGeometry(1);
    auto* receiver=new Object(sphere); receiver->AssignMaterial(*receiverMat); receiver->SetPosition(Point3(0,0,1));receiver->FinalizeTransformations();
    manager->AddItem(receiver,"receiver");
    Ray ray(Point3(.07,.09,-10),Vector3(0,0,1)); ray.hasDifferentials=true;
    ray.diffs.rxOrigin=originOffsets?Vector3(.03,0,0):Vector3(0,0,0);
    ray.diffs.ryOrigin=originOffsets?Vector3(0,.04,0):Vector3(0,0,0);
    ray.diffs.rxDir=Vector3Ops::Normalize(Vector3(.01,0,1))-ray.Dir();
    ray.diffs.ryDir=Vector3Ops::Normalize(Vector3(0,.02,1))-ray.Dir();
    RasterizerState rast={0}; FixedSampler sampler;
    RayIntersection baseline(ray,rast);manager->IntersectRaySampled(baseline,sampler);
    Check(baseline.geometric.bHit && baseline.geometric.txFootprint.valid,"unobstructed textured receiver has valid UV footprint");
    for(int i=0;i<2;++i) {
        auto* sheet=new Object(plane);sheet->AssignMaterial(*holeMat);sheet->SetPosition(Point3(0,0,i==0?-5:-2));sheet->FinalizeTransformations();
        manager->AddItem(sheet,i==0?"hole1":"hole2");safe_release(sheet);
        RayIntersection hit(ray,rast);manager->IntersectRaySampled(hit,sampler);
        Check(hit.geometric.bHit && hit.pObject==receiver,"MASK0 sheets retain receiver hit");
        Check(std::fabs(hit.geometric.range-baseline.geometric.range)<1e-9,"null traversal preserves full camera distance");
        const auto& a=baseline.geometric.txFootprint;const auto& b=hit.geometric.txFootprint;
        std::cout<<"originOffsets="<<originOffsets<<" sheets="<<i+1<<" width="<<b.worldWidth<<" reference="<<a.worldWidth<<" dudx="<<b.dudx<<" reference="<<a.dudx<<'\n';
        Check(b.valid && std::fabs(b.worldWidth-a.worldWidth)<1e-9,"null traversal preserves world texture filter width");
        Check(std::fabs(b.dudx-a.dudx)<1e-9 && std::fabs(b.dudy-a.dudy)<1e-9 && std::fabs(b.dvdx-a.dvdx)<1e-9 && std::fabs(b.dvdy-a.dvdy)<1e-9,"null traversal preserves entire UV Jacobian");
        Check(hit.geometric.ray.origin.z==ray.origin.z && hit.geometric.ray.diffs.rxOrigin.x==ray.diffs.rxOrigin.x,"published ray retains original differential context");
    }
    auto* context=new ContextCoverage();receiverMat->SetAlpha(context,eAlphaBlend,.5);
    RayIntersection hit(ray,rast);manager->IntersectRaySampled(hit,sampler);
    Check(hit.geometric.bHit && hit.pObject==receiver && hit.acceptedAlphaCoverage==.8,"coverage painter observes full original ray and distance");
    safe_release(context);safe_release(receiver);safe_release(sphere);safe_release(plane);safe_release(manager);safe_release(receiverMat);safe_release(holeMat);safe_release(zero);safe_release(texture);safe_release(white);safe_release(black);
}
int main() { Run(false);Run(true);std::cout<<passed<<" passed / "<<failed<<" failed\n";return failed?1:0; }
