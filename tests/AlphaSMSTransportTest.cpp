// DL-214: SMS proposal thinning, final coverage, and scene-local mode selection.
#include <chrono>
#include <cmath>
#include <iostream>
#include "../src/Library/Geometry/ClippedPlaneGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Managers/LightManager.h"
#include "../src/Library/Lights/PointLight.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Scene.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Materials/PerfectReflectorMaterial.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Utilities/ManifoldSolver.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/RISE_API.h"
using namespace RISE;using namespace RISE::Implementation;
static int pass=0,fail=0;
static void Check(bool b,const char* s){(b?pass:fail)++;if(!b)std::cout<<"FAIL "<<s<<'\n';}
static double Measure(bool alpha,bool requestedBiased,bool requestedSnell,bool nm,bool inherited=false){
    auto* white=new UniformColorPainter(RISEPel(1,1,1));
    auto* mirror=new PerfectReflectorMaterial(*white);auto* receiver=new LambertianMaterial(*white);
    auto* coverage=new UniformScalarPainter(.5);if(alpha)mirror->SetAlpha(coverage,eAlphaBlend,.5);
    const Point3 points[4]={Point3(-2,1,-2),Point3(2,1,-2),Point3(2,1,2),Point3(-2,1,2)};
    auto* geometry=new ClippedPlaneGeometry(points,true);auto* object=new Object(geometry);
    object->AssignMaterial(*mirror);object->FinalizeTransformations();
    auto* objects=new ObjectManager(false,false,4,8);objects->AddItem(object,"mirror");
    // An off-path inherited-alpha composite must still select the accounted
    // SMS mode. The mirror itself stays opaque to isolate capability dispatch.
    auto* remoteGeo=new SphereGeometry(1);auto* remoteMat=new LambertianMaterial(*white);
    auto* remoteA=new Object(remoteGeo);auto* remoteB=new Object(remoteGeo);auto* composite=new CSGObject(CSG_UNION);
    if(inherited)remoteMat->SetAlpha(coverage,eAlphaBlend,.5);
    remoteA->AssignMaterial(*remoteMat);remoteB->AssignMaterial(*remoteMat);
    remoteA->SetPosition(Point3(100,100,100));remoteB->SetPosition(Point3(200,100,100));remoteA->FinalizeTransformations();remoteB->FinalizeTransformations();
    composite->AssignObjects(remoteA,remoteB);composite->FinalizeTransformations();objects->AddItem(composite,"inherited_alpha");
    auto* light=new PointLight(1,RISEPel(1,1,1),true);light->SetPosition(Point3(1,0,0));light->FinalizeTransformations();
    auto* lights=new LightManager();lights->AddItem(light,"point");
    auto* scene=new Scene();scene->SetObjectManager(objects);scene->SetLightManager(lights);
    std::vector<IShaderOp*> ops;IShader* shader=nullptr;RISE_API_CreateStandardShader(&shader,ops);
    IRayCaster* caster=nullptr;RISE_API_CreateRayCaster(&caster,false,8,*shader,true);caster->AttachScene(scene);
    Check(caster->GetLightSampler()->SceneHasAlphaCoverage()==(alpha||inherited),"SMS alpha policy is scene-local");
    ManifoldSolverConfig cfg;cfg.biased=requestedBiased;cfg.seedingMode=requestedSnell?ManifoldSolverConfig::eSeedingSnell:ManifoldSolverConfig::eSeedingUniform;
    cfg.targetBounces=1;cfg.maxChainDepth=1;cfg.maxBernoulliTrials=2;
    auto* solver=new ManifoldSolver(cfg);std::vector<const IObject*> casters;ManifoldSolver::EnumerateSpecularCasters(*scene,casters);solver->SetSpecularCasters(casters);
    RandomNumberGenerator rng(214);IndependentSampler sampler(rng);OrthonormalBasis3D onb;onb.CreateFromW(Vector3(0,1,0));
    const auto start=std::chrono::steady_clock::now();double total=0;const int N=20000;
    for(int i=0;i<N;++i){
        if(nm)total+=solver->EvaluateAtShadingPointNM(Point3(0,0,0),Vector3(0,1,0),Vector3(0,1,0),onb,receiver,Vector3(0,1,0),*scene,*caster,sampler,550).contribution;
        else total+=solver->EvaluateAtShadingPoint(Point3(0,0,0),Vector3(0,1,0),Vector3(0,1,0),onb,receiver,Vector3(0,1,0),*scene,*caster,sampler).contribution.r;
    }
    const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::cout<<"alpha="<<alpha<<" requestedBiased="<<requestedBiased<<" requestedSnell="<<requestedSnell<<" nm="<<nm<<" mean="<<total/N<<" seconds="<<seconds<<'\n';
    safe_release(composite);safe_release(remoteA);safe_release(remoteB);safe_release(remoteGeo);safe_release(remoteMat);safe_release(solver);safe_release(caster);safe_release(shader);safe_release(scene);safe_release(objects);safe_release(lights);safe_release(light);safe_release(object);safe_release(geometry);safe_release(mirror);safe_release(receiver);safe_release(coverage);safe_release(white);
    return total/N;
}
int main(){
    // A separate live alpha material must not influence the opaque scene.
    auto* unrelatedPaint=new UniformColorPainter(RISEPel(1,1,1));auto* unrelated=new LambertianMaterial(*unrelatedPaint);auto* a=new UniformScalarPainter(.2);unrelated->SetAlpha(a,eAlphaBlend,.5);
    for(bool nm:{false,true}){
        const double base=Measure(false,false,false,nm);
        const double uniform=Measure(true,false,false,nm);
        const double requestedFast=Measure(true,true,true,nm);
        const double inheritedUniform=Measure(false,false,false,nm,true);
        const double inheritedFast=Measure(false,true,true,nm,true);
        Check(inheritedUniform>0,"inherited alpha scene SMS control produces contribution");
        Check(inheritedUniform==inheritedFast,"inherited CSG alpha selects identical accounted SMS estimator");
        Check(base>0,"opaque SMS control produces physical contribution");
        Check(std::fabs(uniform/base-.5)<.035,"alpha SMS has exactly one physical coverage factor");
        Check(uniform==requestedFast,"alpha requested modes select identical accounted estimator");
    }
    safe_release(unrelated);safe_release(unrelatedPaint);safe_release(a);
    std::cout<<pass<<" passed / "<<fail<<" failed\n";return fail?1:0;
}
