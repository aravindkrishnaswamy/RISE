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
int main() {
    auto* manager=new ObjectManager(false,false,4,8);
    auto* phase=new IsotropicPhaseFunction();
    auto* color=new UniformColorPainter(RISEPel(.5,.5,.5));
    auto* coverage=new UniformScalarPainter(.5);
    auto* mat=new LambertianMaterial(*color); mat->SetAlpha(coverage,eAlphaBlend,.5);
    for(int i=0;i<2;++i) {
        auto* geom=new SphereGeometry(i==0?2:1);
        auto* object=new Object(geom); object->FinalizeTransformations();
        object->AssignMaterial(*mat); object->SetShadowParams(false,true);
        const Scalar sigma=i==0?.25:.75;
        auto* medium=new HomogeneousMedium(RISEPel(sigma,sigma,sigma),RISEPel(0,0,0),*phase);
        object->AssignInteriorMedium(*medium);
        manager->AddItem(object,i==0?"outer":"inner");
        safe_release(medium); safe_release(object); safe_release(geom);
    }
    auto* scene=new Scene(); scene->SetObjectManager(manager);
    std::vector<IShaderOp*> ops; IShader* shader=nullptr;
    RISE_API_CreateStandardShader(&shader,ops);
    IRayCaster* caster=nullptr; RISE_API_CreateRayCaster(&caster,false,10,*shader,true); caster->AttachScene(scene);
    const StabilityConfig cfg; auto* integrator=new BDPTIntegrator(8,8,cfg);
    const Ray ray(Point3(0,0,-3),Vector3(0,0,1));
    auto run=[&](Decisions& sampler,Scalar distance,size_t expectedHits,Scalar expected,const char* label) {
        MediumBoundaryHits hits;
        Check(!caster->CastShadowRaySampled(ray,distance,sampler,&hits),"non-shadow boundary remains visible");
        Check(hits.size()==expectedHits,"only accepted medium boundaries recorded");
        const size_t draws=sampler.count;
        const RISEPel tr=integrator->EvalConnectionTransmittance(ray,distance,*scene,*caster,nullptr,nullptr,&hits);
        const Scalar nm=integrator->EvalConnectionTransmittanceNM(ray,distance,*scene,*caster,550,nullptr,nullptr,&hits);
        Check(std::fabs(tr.r-expected)<1e-5,label); Check(std::fabs(nm-expected)<1e-5,"spectral attenuation matches");
        Check(sampler.count==draws && sampler.normalDraws==0,"attenuation never resamples alpha or normal lane");
        std::cout<<label<<" draws="<<draws<<" accepted="<<hits.size()<<" Tr="<<tr.r<<" expected="<<expected<<" stack: air";
        for(const auto& h:hits) std::cout<<(h.geometric.TrueGeomFacing(ray.Dir())<0?" enter":" exit");
        std::cout<<'\n';
        if(distance==6) {
            const Ray reverse(Point3(0,0,3),Vector3(0,0,-1));
            const auto rev=integrator->EvalConnectionTransmittance(reverse,distance,*scene,*caster,nullptr,nullptr,&hits);
            Check(std::fabs(rev.r-expected)<1e-5,"reverse connection consumes same events in reverse order");
        }
    };
    Decisions reject{.9,.9,.9,.9}; run(reject,6,0,1,"all boundaries rejected: no medium state");
    Decisions outer{.1,.9,.9,.1}; run(outer,6,2,std::exp(-1.),"outer accepted inner rejected");
    Decisions nested{.1,.1,.1,.1}; run(nested,6,4,std::exp(-2.),"nested accepted boundaries unwind");
    Decisions finite{.1,.1}; run(finite,3,2,std::exp(-1.),"finite endpoint stays inside nested medium");
    Check(finite.count==2,"finite endpoint does not sample boundaries beyond it");
    safe_release(integrator);safe_release(caster);safe_release(shader);safe_release(scene);safe_release(manager);safe_release(mat);safe_release(coverage);safe_release(color);safe_release(phase);
    std::cout<<passed<<" passed / "<<failed<<" failed\n";return failed?1:0;
}
