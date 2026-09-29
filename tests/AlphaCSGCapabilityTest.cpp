// DL-214: a single sampled boundary decision controls both visibility and media.
#include <cmath>
#include <iostream>
#include <vector>
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Objects/Object.h"
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

#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Lights/LightSampler.h"

int main() {
 for(int nested=0;nested<2;++nested)for(int snapshot=0;snapshot<2;++snapshot)for(int overrideMode=0;overrideMode<4;++overrideMode){
 auto* manager=new ObjectManager(false,false,4,8);auto* color=new UniformColorPainter(RISEPel(.5));auto* alpha=new UniformScalarPainter(0);
 auto* cut=new LambertianMaterial(*color);cut->SetAlpha(alpha,eAlphaMask,.5);auto* opaque=new LambertianMaterial(*color);
 auto* geo=new SphereGeometry(1);auto* left=new Object(geo);auto* right=new Object(geo);
 left->AssignMaterial(*cut);left->FinalizeTransformations();right->AssignMaterial(*opaque);right->SetPosition(Point3(100,0,0));right->FinalizeTransformations();
 auto* csg=new CSGObject(CSG_UNION);csg->AssignObjects(left,right);csg->FinalizeTransformations();
 if(overrideMode==3)csg->AssignMaterial(*opaque);
 Object* top=csg;
 if(nested){auto* remote=new Object(geo);remote->AssignMaterial(*opaque);remote->SetPosition(Point3(200,0,0));remote->FinalizeTransformations();auto* outer=new CSGObject(CSG_UNION);outer->AssignObjects(top,remote);outer->FinalizeTransformations();top->release();remote->release();top=outer;}
 if(overrideMode==1 || overrideMode==2)top->AssignMaterial(overrideMode==1?*opaque:*cut);
 auto* phase=new IsotropicPhaseFunction;auto* medium=new HomogeneousMedium(RISEPel(1.),RISEPel(0.),*phase);top->AssignInteriorMedium(*medium);top->SetShadowParams(false,true);
 if(snapshot){auto* clone=top->CloneSnapshot();top->release();top=clone;}
 // Hidden, unused alpha and foreign scene roots must not affect this scan.
 auto* unused=new Object(geo);unused->AssignMaterial(*cut);unused->FinalizeTransformations();unused->SetWorldVisible(false);manager->AddItem(unused,"unused");
 auto* foreignObjects=new ObjectManager(false,false,4,8);auto* foreign=new Object(geo);foreign->AssignMaterial(*cut);foreign->FinalizeTransformations();foreignObjects->AddItem(foreign,"foreign");auto* foreignScene=new Scene;foreignScene->SetObjectManager(foreignObjects);
 manager->AddItem(top,"top");auto* scene=new Scene;scene->SetObjectManager(manager);
 std::vector<IShaderOp*> ops;IShader* shader=nullptr;RISE_API_CreateStandardShader(&shader,ops);IRayCaster* caster=nullptr;RISE_API_CreateRayCaster(&caster,false,10,*shader,true);caster->AttachScene(scene);
 const bool sceneAlpha=caster->GetLightSampler()->SceneHasAlphaCoverage();const Ray ray(Point3(0,0,-2),Vector3(0,0,1));
 RayIntersection raw(ray,nullRasterizerState);manager->IntersectRay(raw,true,true,false);
 Decisions decisions{.1,.1};MediumBoundaryHits hits;caster->CastShadowRaySampled(ray,4,decisions,&hits);
 auto* integrator=new BDPTIntegrator(8,8,StabilityConfig());
 const auto conditional=integrator->EvalConnectionTransmittance(ray,4,*scene,*caster,nullptr,nullptr,sceneAlpha?&hits:nullptr);
 const auto reference=integrator->EvalConnectionTransmittance(ray,4,*scene,*caster,nullptr,nullptr,&hits);
 const auto conditionalNM=integrator->EvalConnectionTransmittanceNM(ray,4,*scene,*caster,550,nullptr,nullptr,sceneAlpha?&hits:nullptr);
 std::cout<<"nested="<<nested<<" snapshot="<<snapshot<<" override="<<overrideMode<<" sceneAlpha="<<sceneAlpha<<" hitAlpha="<<raw.pMaterial->AlphaCoverage(raw.geometric)<<" records="<<hits.size()<<" selectedRGB="<<conditional.r<<" selectedNM="<<conditionalNM<<" authoritativeRGB="<<reference.r<<std::endl;
 Check(sceneAlpha==(overrideMode==0 || overrideMode==2),"capability matches effective CSG material");Check(std::fabs(conditional.r-reference.r)<1e-5,"selected RGB medium estimator matches accepted events");Check(std::fabs(conditionalNM-reference.r)<1e-5,"selected NM medium estimator matches accepted events");
 foreignScene->release();foreignObjects->release();foreign->release();unused->release();integrator->release();caster->release();shader->release();scene->release();manager->release();top->release();left->release();right->release();geo->release();medium->release();phase->release();cut->release();opaque->release();alpha->release();color->release();
 }
 std::cout<<passed<<" passed / "<<failed<<" failed"<<std::endl;return failed?1:0;
}
