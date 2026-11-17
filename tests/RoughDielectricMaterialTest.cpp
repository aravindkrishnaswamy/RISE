// Material boundary tests: scene construction, nested media, transport-mode
// scaling, spectral hero densities, editor rebind and snapshot isolation.
#include "../src/Library/RISE_API.h"
#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Materials/RoughDielectricMaterial.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Objects/SnapshotLeafClone.h"
#include "../src/Library/SceneEditor/MaterialIntrospection.h"
#include "../src/Library/Utilities/MicrofacetUtils.h"
#include "../src/Library/Utilities/ISampler.h"
#include "../src/Library/Utilities/IORStackSeeding.h"
#include "TestTempDir.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <random>

using namespace RISE;
using namespace RISE::Implementation;
namespace RD = MicrofacetUtils::RoughDielectric;
static int passed=0,failed=0;
static void Check(bool ok,const char* name) { if(ok) ++passed; else { ++failed; if(failed<15) std::cout<<"FAIL: "<<name<<'\n'; } }
static bool Close(Scalar a,Scalar b) { return std::isfinite(a) && std::isfinite(b) && std::abs(a-b)<1e-9*std::max(Scalar(1),std::max(std::abs(a),std::abs(b))); }
class Samples : public ISampler {
    std::mt19937_64 rng{20261116};
public:
    Scalar Get1D() override { return std::generate_canonical<Scalar,53>(rng); }
    Point2 Get2D() override { const Scalar a=Get1D(),b=Get1D(); return Point2(a,b); }
};
class SpectralIndex : public virtual IScalarPainter, public virtual Reference {
protected:
    ~SpectralIndex() override = default;
public:
    ScalarTriple GetValuesAt(const RayIntersectionGeometric&) const override { return ScalarTriple(1.5); }
    Scalar GetValueAtNM(const RayIntersectionGeometric&,Scalar nm) const override { return 1.3+(nm-400)*0.0005; }
};

static void Sampling(IMaterial& material,RayIntersectionGeometric& ri,IORStack& stack,Scalar eta,Scalar nm)
{
    Samples sampler;
    int reflect=0,transmit=0;
    for(int i=0;i<2500;++i) {
        ScatteredRayContainer rays;
        if(nm>0) material.GetSPF()->ScatterNM(ri,sampler,nm,rays,stack);
        else material.GetSPF()->Scatter(ri,sampler,rays,stack);
        Check(rays.Count()<=1,"stochastic single-ray selection");
        if(!rays.Count()) continue;
        const auto& ray=rays[0];
        const Scalar pdf=nm>0 ? material.GetSPF()->PdfNM(ri,ray.ray.Dir(),nm,stack) : material.GetSPF()->Pdf(ri,ray.ray.Dir(),stack);
        Check(Close(pdf,ray.pdf) && pdf>0 && !ray.isDelta,"aggregate query PDF equals sample density");
        const Vector3 wi=Vector3Ops::Normalize(-ri.ray.Dir());
        const Vector3 wo=ray.ray.Dir();
        const auto expected=RD::Evaluate(wi,wo,0.2,0.4,eta,false);
        const Scalar weight=nm>0 ? ray.krayNM : ray.kray[0];
        Check(Close(weight,expected.weight),"sampler importance weight matches kernel");
        const Scalar cosine=std::abs(wo.z);
        Scalar importance=0,radiance=0;
        {
            BSDFImportanceScope mode(true);
            importance=nm>0 ? material.GetBSDF()->valueStatefulNM(wo,ri,nm,&stack) : material.GetBSDF()->valueStateful(wo,ri,&stack)[0];
        }
        radiance=nm>0 ? material.GetBSDF()->valueStatefulNM(wo,ri,nm,&stack) : material.GetBSDF()->valueStateful(wo,ri,&stack)[0];
        Check(Close(weight,importance*cosine/pdf),"BSDF/SPF agreement in importance mode");
        if(ray.type==ScatteredRay::eRayRefraction) {
            ++transmit;
            Check(ray.ior_stack && Close(ray.ior_stack->top(),1.5),"transmission pushes target IOR");
            Check(Close(radiance,importance/(eta*eta)),"radiance BSDF applies eta scaling once");
            Check(Close(weight*RadianceEtaScale(stack,ray.ior_stack),radiance*cosine/pdf),"integrator eta scaling matches BSDF");
        } else {
            ++reflect;
            Check(!ray.ior_stack && Close(radiance,importance),"reflection retains medium and transport value");
        }
        if(nm>0) {
            const Scalar heroPdf=pdf*0.7;
            Check(Close(material.GetSPF()->EvaluateKrayNM(ri,wo,ray.type,nm,stack,heroPdf),importance*cosine/heroPdf),"HWSS uses hero density, not companion density");
            Check(Close(material.GetSPF()->EvaluateLobeFNM(ri,wo,ray.type,nm,stack),importance),"BDPT spectral replay uses importance BSDF");
            Scalar split[5];
            Check(material.GetBSDF()->valueByScatterTypeNM(wo,ri,nm,&stack,split) && Close(split[ray.type],radiance),"spectral connection type split");
        } else {
            RISEPel split[5];
            Check(material.GetBSDF()->valueByScatterType(wo,ri,&stack,split) && Close(split[ray.type][0],radiance),"RGB connection type split");
        }
    }
    Check(reflect>0 && transmit>1500,"both Fresnel branches are exercised");
}

int main()
{
    auto* ior=new UniformScalarPainter(1.5);
    auto* ax=new UniformScalarPainter(0.2);
    auto* ay=new UniformScalarPainter(0.4);
    IMaterial* material=nullptr;
    Check(RISE_API_CreateRoughDielectricMaterial(&material,*ior,*ax,*ay),"public API creates material");
    Check(!RISE_API_CreateRoughDielectricMaterial(nullptr,*ior,*ax,*ay),"null output refused");
    RayIntersectionGeometric ri(Ray(Point3(0,0,1),Vector3(0,0,-1)),nullRasterizerState);
    ri.bHit=true; ri.ptIntersection=Point3(0,0,0); ri.vNormal=ri.vGeomNormal=Vector3(0,0,1);
    // Object identity keys are never dereferenced on this closed-interface path.
    const IObject* key=reinterpret_cast<const IObject*>(ior);
    IORStack air(1); air.SetCurrentObject(key);
    Sampling(*material,ri,air,1.5,0);
    IORStack water(1.33); water.SetCurrentObject(key);
    Sampling(*material,ri,water,1.5/1.33,0);
    Sampling(*material,ri,air,1.5,550);
    auto info=material->GetSpecularInfo(ri,air);
    Check(info.valid && info.canRefract && !info.isSpecular && !info.clearTransmission && info.ior==1.5,"rough interface advertises index without ballistic transmission");
    Check(material->ScattersFullSphere() && material->HasConnectionTypeSplit(),"transmitting NEE and connection type support");

    Samples sampler;
    IORStack inside(1.33); inside.SetCurrentObject(key); inside.push(1.5);
    bool exited=false;
    for(int i=0;i<100 && !exited;++i) {
        ScatteredRayContainer rays; material->GetSPF()->Scatter(ri,sampler,rays,inside);
        if(rays.Count() && rays[0].type==ScatteredRay::eRayRefraction) {
            exited=true;
            Check(rays[0].ior_stack && Close(rays[0].ior_stack->top(),1.33) && !rays[0].ior_stack->containsCurrent(),"exit restores enclosing medium");
            const auto expected=RD::Evaluate(Vector3(0,0,1),rays[0].ray.Dir(),0.2,0.4,1.33/1.5,false);
            Check(Close(rays[0].kray[0],expected.weight),"exit samples enclosing/material IOR ratio");
        }
    }
    Check(exited && inside.containsCurrent() && inside.top()==1.5,"sampling never mutates input stack");

    auto* spectral=new SpectralIndex;
    auto* rough=dynamic_cast<RoughDielectricMaterial*>(material);
    rough->SetIOR(*spectral);
    for(Scalar nm : {Scalar(450),Scalar(700)}) {
        ScatteredRayContainer rays; rough->GetSPF()->ScatterNM(ri,sampler,nm,rays,air);
        Check(rays.Count()==1,"spectral direction sampled");
        if(rays.Count()) {
            const auto& ray=rays[0];
            const Scalar index=spectral->GetValueAtNM(ri,nm);
            const auto expected=RD::Evaluate(Vector3(0,0,1),ray.ray.Dir(),0.2,0.4,index,false);
            Check(Close(ray.krayNM,expected.weight) && Close(ray.pdf,expected.pdf),"NM sampler preserves wavelength-dependent IOR");
            const Scalar companion=material->GetSPF()->EvaluateKrayNM(ri,ray.ray.Dir(),ray.type,600,air,ray.pdf);
            const Scalar f=RD::Evaluate(Vector3(0,0,1),ray.ray.Dir(),0.2,0.4,1.4,false).f;
            Check(Close(companion,f*std::abs(ray.ray.Dir().z)/ray.pdf),"HWSS dispersive companion is evaluated at its own IOR");
        }
    }
    rough->SetIOR(*ior);
    const IMaterial* snapshot=CloneMaterialForSnapshot(material);
    Check(snapshot!=material,"snapshot owns independent material");
    auto* other=new UniformScalarPainter(1.7);
    Check(MaterialIntrospection::SetSlot(*material,"ior",nullptr,other),"editor scalar rebind supported");
    Check(snapshot->GetSpecularInfo(ri,air).ior==1.5 && material->GetSpecularInfo(ri,air).ior==1.7,"snapshot preserves prior index");
    Check(MaterialIntrospection::GetSlot(*material,"alphay").scalarPainter==ay,"editor slot readback");
    Check(!MaterialIntrospection::SetSlot(*material,"tau",nullptr,other),"unknown slots refused");
    auto* rgb=new RGBScalarPainter(1.4,1.5,1.6);
    IMaterial* rejected=nullptr;
    Check(!RISE_API_CreateRoughDielectricMaterial(&rejected,*rgb,*ax,*ay) && !rejected,"per-channel IOR refused at API boundary");
    Check(!MaterialIntrospection::SetSlot(*material,"ior",nullptr,rgb),"editor enforces single-scalar IOR");
    IScalarPainter* measured=nullptr;
    RISE_API_CreateSellmeierScalarPainter(&measured,1.03961212,0.231792344,1.01046945,
        0.00600069867,0.0200179144,103.560653);
    Check(MaterialIntrospection::SetSlot(*material,"ior",nullptr,measured),"editor accepts spectral curve through a single-scalar view");
    Check(Close(material->GetSpecularInfoNM(ri,air,450).ior,measured->GetValueAtNM(ri,450)) &&
        Close(material->GetSpecularInfo(ri,air).ior,measured->GetValuesAt(ri).v[1]),"editor view retains spectral curve and representative RGB sample");
    safe_release(measured);

    // The geometric gate must be identical on the sampled and queried laws.
    rough->SetIOR(*ior);
    ri.vGeomNormal=Vector3Ops::Normalize(Vector3(1,0,0.1));
    const Vector3 blocked=Vector3Ops::Normalize(Vector3(-1,0,0.1));
    Check(material->GetSPF()->Pdf(ri,blocked,air)==0 && material->GetBSDF()->valueStateful(blocked,ri,&air)[0]==0,"geometric horizon gates value and PDF together");
    ri.vGeomNormal=Vector3(0,0,1);
    auto* equal=new UniformScalarPainter(1);
    rough->SetIOR(*equal);
    ScatteredRayContainer delta; rough->GetSPF()->Scatter(ri,sampler,delta,air);
    Check(delta.Count()==1 && delta[0].isDelta && delta[0].pdf==0 && delta[0].kray[0]==1,"equal indices retain straight-through unit delta");
    if(delta.Count()) Check(material->GetSPF()->EvaluateKrayNM(ri,delta[0].ray.Dir(),delta[0].type,550,air,0)==1,"equal-index HWSS delta replay");
    rough->SetIOR(*ior); safe_release(equal);

    IJob* jobBase=nullptr; RISE_CreateJob(&jobBase);
    auto* job=dynamic_cast<Job*>(jobBase);
    auto* priv=dynamic_cast<IJobPriv*>(jobBase);
    const std::string path=TestSupport::ScratchTestRoot("rough_dielectric_")+".RISEscene";
    {
        std::ofstream scene(path);
        scene<<"RISE ASCII SCENE 7\nrough_dielectric_material\n{\nname default_glass\n}\n"
            <<"rough_dielectric_material\n{\nname authored_glass\nior 1.5\nalphax 0.2\nalphay 0.4\n}\n"
            <<"sphere_geometry\n{\nname sphere\nradius 1\n}\nstandard_object\n{\nname solid\ngeometry sphere\nmaterial authored_glass\n}\n";
    }
    Check(priv && priv->LoadAsciiSceneViaCst(path.c_str()),"native chunk parses through CST and Job");
    std::remove(path.c_str());
    auto* parsed=job->GetMaterials()->GetItem("authored_glass");
    Check(parsed && parsed->GetSPF()->Pdf(ri,Vector3(0,0,1),air)>0,"parsed material reaches rough BSDF");
    auto* defaults=dynamic_cast<RoughDielectricMaterial*>(job->GetMaterials()->GetItem("default_glass"));
    Check(defaults && defaults->GetIOR().GetValuesAt(ri).v[0]==1.5 && defaults->GetAlphaX().GetValuesAt(ri).v[0]==0.2,"parser defaults match advertised physical values");
    IORStack seeded(1);
    IORStackSeeding::SeedFromPoint(seeded,Point3(0,0,0),*job->GetScene());
    Check(Close(seeded.top(),1.5) && seeded.Depth()==2,"camera/light origins inside rough solids seed the interior IOR");
    Check(!jobBase->AddRoughDielectricMaterial("bad","1.4 1.5 1.6","0.2","0.2"),"Job rejects ambiguous RGB scalar");
    auto* zero=new UniformScalarPainter(0);
    rough->SetAlphaX(*zero);
    ScatteredRayContainer invalid; rough->GetSPF()->Scatter(ri,sampler,invalid,air);
    Check(!invalid.Count() && rough->GetSPF()->Pdf(ri,Vector3(0,0,1),air)==0,"invalid per-hit width has null distribution");

    safe_release(zero); safe_release(jobBase); safe_release(rgb); safe_release(other);
    safe_release(snapshot); safe_release(spectral); safe_release(material);
    safe_release(ior); safe_release(ax); safe_release(ay);
    std::cout<<passed<<" passed, "<<failed<<" failed\n";
    return failed ? 1 : 0;
}
