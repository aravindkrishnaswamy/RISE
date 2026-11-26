// P6a source evaluation/construction controls. This does not certify band-aware
// source selection, a radiometric film, or out-of-visible rendering.
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/ILightManager.h"
#include "../src/Library/Interfaces/IKeyframable.h"
#include "../src/Library/Lights/PointLight.h"
#include "../src/Library/Lights/SpotLight.h"
#include "../src/Library/Lights/DirectionalLight.h"
#include "../src/Library/Lights/PhysicalLightSpectrum.h"
#include "../src/Library/Objects/SnapshotLeafClone.h"
#include "../src/Library/SceneEditor/LightIntrospection.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Cst/Cst.h"
#include "TestTempDir.h"
#include "CstRenderEquivalence.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>

using namespace RISE;
using namespace RISE::Implementation;
namespace RISE { bool RISE_CreateJobPriv(IJobPriv** ppi); }
namespace {
int failures = 0;
void Check(bool ok, const char* label) { if (!ok) { ++failures; std::printf("FAIL: %s\n",label); } }
void Near(double a, double b, const char* label) { Check(std::isfinite(a) && std::fabs(a-b) < 1e-11*std::fmax(1.0,std::fabs(b)),label); }
class UnitBSDF final : public IBSDF, public Reference {
public:
    RISEPel value(const Vector3&, const RayIntersectionGeometric&) const override { return RISEPel(.25,.25,.25); }
    Scalar valueNM(const Vector3&, const RayIntersectionGeometric&, Scalar) const override { return .25; }
};
IJobPriv* Load(const std::string& body) {
    const std::string path = TestSupport::ScratchBaseDir()+"/physical_source.RISEscene";
    { std::ofstream output(path); output << "RISE ASCII SCENE 7\n" << body; }
    IJobPriv* job = nullptr;
    if (!RISE_CreateJobPriv(&job)) return nullptr;
    const bool ok = job->LoadAsciiSceneViaCst(path.c_str());
    std::remove(path.c_str());
    if (!ok) { job->release(); return nullptr; }
    return job;
}
const double nm[] = {1100,1200,1300};
const double values[] = {2,4,6};
void SourceControls() {
    double wavelengths[] = {1100,1200,1300}, samples[] = {2,4,6};
    const auto source = PhysicalLightSpectrum::Create(wavelengths,samples,3,"zero");
    Check(bool(source),"valid checked source");
    if (!source) return;
    wavelengths[1] = 9; samples[1] = 99;
    Near(source->Eval(1150),3,"copies input arrays and interpolates");
    Near(source->Eval(1300),6,"closed upper endpoint");
    Near(source->Eval(1099),0,"explicit zero below domain");
    Near(source->Eval(1301),0,"explicit zero above domain");
    Near(source->Eval(std::numeric_limits<double>::quiet_NaN()),0,"nonfinite query"); // HYGIENE-OK: intentional invalid spectral input, never a not-found sentinel.
    Near(source->VisibleRGB().r+source->VisibleRGB().g+source->VisibleRGB().b,0,"pure SWIR preview is black");
    Check(!PhysicalLightSpectrum::Create(nm,values,1,"zero"),"reject singleton");
    Check(!PhysicalLightSpectrum::Create(nm,values,3,nullptr),"outside policy explicit");
    Check(!PhysicalLightSpectrum::Create(nm,values,3,"hold"),"reject endpoint hold");
    double bad[] = {1100,1100,1300};
    Check(!PhysicalLightSpectrum::Create(bad,values,3,"zero"),"reject repeated wavelength");
    bad[1] = 1000;
    Check(!PhysicalLightSpectrum::Create(bad,values,3,"zero"),"reject reversed grid");
    bad[1] = std::numeric_limits<double>::infinity(); // HYGIENE-OK: intentional invalid spectral input, never a not-found sentinel.
    Check(!PhysicalLightSpectrum::Create(bad,values,3,"zero"),"reject infinite wavelength");
    bad[0] = 2; bad[1] = -1; bad[2] = 6;
    Check(!PhysicalLightSpectrum::Create(nm,bad,3,"zero"),"reject negative sample");
    bad[1] = std::numeric_limits<double>::quiet_NaN(); // HYGIENE-OK: intentional invalid spectral input, never a not-found sentinel.
    Check(!PhysicalLightSpectrum::Create(nm,bad,3,"zero"),"reject NaN sample");
    // Exact/relative oracles: an absolute tolerance would falsely accept zero.
    const double sub = std::numeric_limits<double>::denorm_min();
    const double constantValues[] = {sub, sub};
    const double constantNm[] = {1100,1300};
    const auto constant = PhysicalLightSpectrum::Create(constantNm,constantValues,2,"zero");
    Check(constant && constant->Eval(1200) == sub && constant->Eval(1200) > 0,
        "constant subnormal source survives midpoint interpolation [physical-spd-denorm]");
    const double wideNm[] = {1e-100,1e300}, risingValues[] = {0,1e300}, fallingValues[] = {1e300,0};
    const auto rising = PhysicalLightSpectrum::Create(wideNm,risingValues,2,"zero");
    const auto falling = PhysicalLightSpectrum::Create(wideNm,fallingValues,2,"zero");
    Check(rising && std::fabs(rising->Eval(1e-99)/9e-100-1) < 1e-14,
        "recoverable contribution survives underflowing wavelength ratio");
    const double nearEnd = std::nextafter(1e300,0.0);
    Check(falling && falling->Eval(nearEnd) == 1e300-nearEnd,
        "descending extreme source uses nearby endpoint without cancellation");
    Check(!source->CanScale(-1) && !source->CanScale(std::numeric_limits<double>::max()),
        "checked scalar scale rejects sign and overflow");
    const double visNm[] = {380,780}, one[] = {1,1}, two[] = {2,2};
    const auto a = PhysicalLightSpectrum::Create(visNm,one,2,"zero");
    const auto b = PhysicalLightSpectrum::Create(visNm,two,2,"zero");
    Check(bool(a)&&bool(b),"visible projections available");
    if (a && b) { Near(b->VisibleRGB().r,2*a->VisibleRGB().r,"preview preserves absolute amplitude"); Near(b->VisibleRGB().g,2*a->VisibleRGB().g,"preview linear green"); }
}
void DeltaControls() {
    IShader* shader = nullptr;
    RISE_API_CreateStandardShader(&shader,{});
    IRayCaster* caster = nullptr;
    RISE_API_CreateRayCaster(&caster,false,4,*shader,false);
    auto* bsdf = new UnitBSDF;
    RayIntersectionGeometric ri(Ray(Point3(0,0,1),Vector3(0,0,-1)),nullRasterizerState);
    ri.ptIntersection = Point3(0,0,0); ri.vNormal = Vector3(0,0,1); ri.vGeomNormal = ri.vNormal;
    for (int kind=0; kind<3; ++kind) {
        ILightPriv* light = nullptr;
        const bool ok = kind==0 ? RISE_API_CreatePhysicalPointOmniLight(&light,3,nm,values,"zero",3,false) :
            kind==1 ? RISE_API_CreatePhysicalPointSpotLight(&light,3,nm,values,"zero",3,Point3(0,0,2),Point3(0,0,0),.4,.8,false) :
            RISE_API_CreatePhysicalDirectionalLight(&light,3,nm,values,"zero",3,Vector3(0,0,1));
        Check(ok && light,"physical delta factory"); if (!light) continue;
        if (kind != 2) { light->SetPosition(Point3(0,0,2)); light->FinalizeTransformations(); }
        Near(light->emittedRadianceNM(Vector3(0,0,-1),1200),12,"absolute NM emission");
        Near(light->ComputeDirectLightingNM(ri,*caster,*bsdf,false,1200),kind==2 ? 3 : .75,"actual direct NM analytic irradiance");
        if (kind!=2) {
            light->SetPosition(Point3(0,0,4)); light->FinalizeTransformations();
            Near(light->ComputeDirectLightingNM(ri,*caster,*bsdf,false,1200),.1875,"inverse square physical source");
        }
        if (kind == 1) {
            const double theta = .3;
            const Vector3 out(std::sin(theta),0,-std::cos(theta));
            Near(light->emittedRadianceNM(out,1200),3,"spot transition cone has quadratic attenuation");
            Near(light->emittedRadianceNM(Vector3(std::sin(.1),0,-std::cos(.1)),1200),12,"spot inner cone full intensity");
            Near(light->emittedRadianceNM(Vector3(std::sin(.5),0,-std::cos(.5)),1200),0,"spot outer cone rejects direction");
            light->SetPosition(Point3(0,0,2)); light->FinalizeTransformations();
            ri.ptIntersection = Point3(2*std::tan(theta),0,0);
            ri.vNormal = -out; ri.vGeomNormal = ri.vNormal;
            // Make the camera ray oppose the receiver normal, matching a visible front face.
            RayIntersectionGeometric angled(Ray(Point3(0,0,1),out),nullRasterizerState);
            angled.ptIntersection=ri.ptIntersection; angled.vNormal=ri.vNormal; angled.vGeomNormal=ri.vNormal;
            Near(light->ComputeDirectLightingNM(angled,*caster,*bsdf,false,1200),
                .1875*std::cos(theta)*std::cos(theta),"spot direct NM matches independent cone and distance law");
            ri.ptIntersection=Point3(0,0,0); ri.vNormal=Vector3(0,0,1); ri.vGeomNormal=ri.vNormal;
        }
        Check(!light->KeyframeFromParameters("color","1 0 0"),"physical color keyframe refused");
        Check(!light->KeyframeFromParameters("energy","-1"),"negative physical scale refused");
        Check(!light->KeyframeFromParameters("energy","1e308"),"overflowing physical scale keyframe refused");
        const ILight* snapshot = CloneLightForSnapshot(light);
        Check(snapshot && GetPhysicalLightSpectrum(*snapshot)==GetPhysicalLightSpectrum(*light),"snapshot retains immutable source");
        IKeyframeParameter* key = light->KeyframeFromParameters("energy","2");
        Check(key!=nullptr,"physical scale animation allowed");
        if (key) { light->SetIntermediateValue(*key); key->release(); }
        Near(light->emittedRadianceNM(Vector3(0,0,-1),1200),8,"updated scale NM emission");
        if (snapshot) { Near(snapshot->emittedRadianceNM(Vector3(0,0,-1),1200),12,"snapshot isolated from live scale"); snapshot->release(); }
        const ILight* timeClone = kind==0 ? static_cast<const ILight*>(dynamic_cast<PointLight*>(light)->CloneForTimeView()) :
            kind==1 ? static_cast<const ILight*>(dynamic_cast<SpotLight*>(light)->CloneForTimeView()) :
            static_cast<const ILight*>(dynamic_cast<DirectionalLight*>(light)->CloneForTimeView());
        light->release();
        Near(timeClone->emittedRadianceNM(Vector3(0,0,-1),1200),8,"time clone survives original destruction");
        timeClone->release();
    }
    ILightPriv* invalid = reinterpret_cast<ILightPriv*>(1);
    Check(!RISE_API_CreatePhysicalDirectionalLight(&invalid,3,nm,values,"zero",1,Vector3(0,0,0)) && !invalid,"invalid factory clears output");
    Check(!RISE_API_CreatePhysicalPointSpotLight(&invalid,3,nm,values,"zero",1,
        Point3(0,0,2),Point3(0,0,2),.4,.8,false) && !invalid,
        "physical spot rejects coincident position and target [physical-spot-axis]");
    Check(!RISE_API_CreatePhysicalPointSpotLight(&invalid,3,nm,values,"zero",1,
        Point3(0,0,2),Point3(0,0,0),.4,4,false) && !invalid,
        "physical spot rejects cones exceeding its forward hemisphere");
    ILightPriv* legacy = nullptr;
    Check(RISE_API_CreatePointOmniLight(&legacy,2,RISEPel(.25,.5,.75),false),"legacy RGB factory remains available");
    if (legacy) {
        Check(!GetPhysicalLightSpectrum(*legacy),"RGB source does not become a physical spectrum");
        Near(legacy->emissionColor().g,.5,"legacy color remains exact");
        Near(legacy->emittedRadiance(Vector3(0,0,-1)).b,1.5,"legacy energy remains exact");
        legacy->release();
    }
    bsdf->release(); caster->release(); shader->release();
}
void PhysicalRuntimeControls() {
    const double visibleNm[] = {380,780}, unit[] = {1,1}, faint[] = {1e-308,1e-308};
    ILightPriv* light = nullptr;
    Check(!RISE_API_CreatePhysicalPointOmniLight(&light,2,visibleNm,unit,"zero",1e308,false) && !light,
        "point constructor rejects overflowing 4pi RGB power proxy [physical-proxy-overflow]");
    Check(!RISE_API_CreatePhysicalPointSpotLight(&light,2,visibleNm,unit,"zero",1e308,
        Point3(0,0,2),Point3(0,0,0),PI,PI,false) && !light,
        "spot constructor rejects overflowing power proxy");
    Check(RISE_API_CreatePhysicalPointOmniLight(&light,2,visibleNm,faint,"zero",1e308,false),
        "source-first scaling preserves faint spectrum with huge finite multiplier");
    if (light) {
        Near(light->emittedRadianceNM(Vector3(0,0,-1),500),1,"faint times huge scale NM result");
        const RISEPel power=light->radiantExitance();
        Check(std::isfinite(power.r)&&std::isfinite(power.g)&&std::isfinite(power.b),
            "actual positional power uses the same overflow-safe formulation as admission");
        light->release();
    }
    Check(RISE_API_CreatePhysicalDirectionalLight(&light,2,visibleNm,unit,"zero",1e308,Vector3(0,0,1)),
        "directional irradiance does not inherit a positional power bound");
    if (light) { Check(light->emittedRadianceNM(Vector3(0,0,-1),500)==1e308,"large finite directional irradiance retained"); light->release(); }
    for (int kind=0; kind<2; ++kind) {
        light=nullptr;
        const bool ok = kind==0 ? RISE_API_CreatePhysicalPointOmniLight(&light,2,visibleNm,unit,"zero",1,false) :
            RISE_API_CreatePhysicalPointSpotLight(&light,2,visibleNm,unit,"zero",1,Point3(0,0,2),Point3(0,0,0),.4,.8,false);
        Check(ok && light,"runtime power fixture"); if (!light) continue;
        Check(!light->KeyframeFromParameters("energy","1e308"),"power proxy overflow keyframe rejected");
        IKeyframeParameter* scale = light->KeyframeFromParameters("energy","2");
        if (scale) {
            double excessive=1e308; scale->setValue(&excessive);
            light->SetIntermediateValue(*scale); scale->release();
            Near(light->emissionEnergy(),1,"interpolated overflow scale retains previous value");
            const RISEPel power=light->radiantExitance();
            Check(std::isfinite(power.r)&&std::isfinite(power.g)&&std::isfinite(power.b),"retained power proxy stays finite");
        } else Check(false,"valid scalar keyframe fixture");
        light->release();
    }
    Check(RISE_API_CreatePhysicalPointSpotLight(&light,3,nm,values,"zero",3,
        Point3(0,0,2),Point3(0,0,0),.4,.8,true),"physical pose runtime fixture");
    if (!light) return;
    Check(!light->KeyframeFromParameters("target","0 0 2"),"coincident physical target keyframe rejected");
    Check(!light->KeyframeFromParameters("outer_angle","360"),"physical outer cone beyond hemisphere rejected");
    Check(!light->KeyframeFromParameters("inner_angle","90"),"physical inner cone beyond current outer cone rejected");
    IKeyframeParameter* target=light->KeyframeFromParameters("target","0 0 -1");
    if (target) {
        Point3 coincident(0,0,2); target->setValue(&coincident);
        light->SetIntermediateValue(*target); target->release();
        Near(light->emissionTarget().z,0,"invalid interpolated target retains previous target");
    } else Check(false,"valid target keyframe fixture");
    IKeyframeParameter* cone=light->KeyframeFromParameters("outer_angle","45");
    if (cone) {
        double invalidAngle=4; cone->setValue(&invalidAngle);
        light->SetIntermediateValue(*cone); cone->release();
        Near(light->emissionOuterAngle(),.8,"invalid interpolated cone retains previous cone");
    } else Check(false,"valid cone keyframe fixture");
    // Position is a generic void transform API: the finalized invalid pose is
    // diagnosed and inactive, rather than pretending that setter can reject it.
    IKeyframeParameter* position=light->KeyframeFromParameters("position","0 0 0");
    Check(position!=nullptr,"generic position keyframe remains available");
    if (position) { light->SetIntermediateValue(*position); position->release(); light->RegenerateData(); }
    Near(light->emittedRadianceNM(Vector3(0,0,-1),1200),0,"undefined finalized spot axis has no NM emission");
    const RISEPel disabled=light->emittedRadiance(Vector3(0,0,-1));
    Near(disabled.r+disabled.g+disabled.b,0,"undefined finalized spot axis has finite RGB emission");
    Near(light->pdfDirection(Vector3(0,0,-1)),0,"undefined spot axis has no direction PDF");
    const Vector3 axis=light->emissionDirection();
    Check(std::isfinite(axis.x)&&std::isfinite(axis.y)&&std::isfinite(axis.z),"undefined pose never exposes NaN axis");
    Check(light->CanGeneratePhotons(),"inactive pose preserves authored photon flag");
    const RISEPel inactivePower=light->radiantExitance();
    Near(inactivePower.r+inactivePower.g+inactivePower.b,0,"inactive spot has no power for photon-source selection");
    const ILight* inactiveClone=CloneLightForSnapshot(light);
    Check(inactiveClone && inactiveClone->CanGeneratePhotons(),"snapshot preserves inactive pose photon authoring flag");
    if (inactiveClone) { Near(inactiveClone->emittedRadianceNM(Vector3(0,0,-1),1200),0,"snapshot retains diagnosed inactive pose"); inactiveClone->release(); }
    light->SetPosition(Point3(0,0,2)); light->FinalizeTransformations();
    Near(light->emittedRadianceNM(Vector3(0,0,-1),1200),12,"restored valid spot pose resumes physical emission");
    Check(light->CanGeneratePhotons(),"valid pose restores authored photon eligibility");
    light->release();
}
void SceneControls() {
    const char* kinds[] = {"omni_light","spot_light","directional_light"};
    const char* extra[] = {"position 0 0 2\n","position 0 0 2\ntarget 0 0 0\ninner 20\nouter 40\n","direction 0 0 1\n"};
    for (int i=0;i<3;++i) {
        const std::string start = std::string(kinds[i])+"\n{\nname L\npower 3\n"+extra[i];
        const std::string samples = "spectral_outside zero\nspectral_samples 1100 2\nspectral_samples 1200 4\nspectral_samples 1300 6\n";
        IJobPriv* job = Load(start+samples+"}\n");
        Check(job!=nullptr,"CST physical source loads");
        if (job) {
            const ILight* light = job->GetLights()->GetItem("L");
            Check(light!=nullptr,"CST registers physical source");
            if (light) {
                Near(light->emittedRadianceNM(Vector3(0,0,-1),1200),12,"CST keeps actual spectral source");
                const auto rows = LightIntrospection::Inspect("L",*light,job->GetCstDocument());
                bool found = false;
                for (const auto& row : rows) {
                    Check(row.name!=String("color") && row.name!=String("colorspace"),"physical panel excludes RGB controls");
                    if (row.name==String("spectral_samples")) { found=true; Check(!row.editable && row.value.size()>1,"panel preserves read-only samples"); }
                }
                Check(found,"physical panel source information");
            }
            {
                SceneEditController controller(*job,0);
                controller.SetSelection(SceneEditController::Category::Light,"L");
                const std::string before = Cst::SerializeCst(*job->GetCstDocument());
                Check(!controller.SetPropertyForCategory(SceneEditController::Category::Light,"color","1 0 0"),
                    "physical editor color edit refused");
                Check(Cst::SerializeCst(*job->GetCstDocument()) == before,"refused physical color edit leaves CST unchanged");
                if (i==1) {
                    Check(!controller.SetPropertyForCategory(SceneEditController::Category::Light,"position","0 0 0"),
                        "CST physical spot position edit rejects coincident target atomically");
                    Check(Cst::SerializeCst(*job->GetCstDocument())==before,"refused spot pose edit preserves CST");
                }
                Check(controller.SetPropertyForCategory(SceneEditController::Category::Light,"power","2"),
                    "physical editor permits scalar power edit");
                Near(job->GetLights()->GetItem("L")->emittedRadianceNM(Vector3(0,0,-1),1200),8,"editor physical scale NM result");
                Check(controller.Editor().Undo(),"physical scalar edit undo");
                Near(job->GetLights()->GetItem("L")->emittedRadianceNM(Vector3(0,0,-1),1200),12,"undo restores physical source scale");
                Check(controller.Editor().Redo(),"physical scalar edit redo");
                Near(job->GetLights()->GetItem("L")->emittedRadianceNM(Vector3(0,0,-1),1200),8,"redo preserves spectral source");
                Check(controller.Editor().Undo(),"restore authored scale before roundtrip");
            }
            const std::string digest = risequiv::DumpJob(*dynamic_cast<Job*>(job));
            Check(digest.find("physical_spd(zero;1100=2;1200=4;1300=6)") != std::string::npos,
                "CST digest records exact physical samples and outside policy");
            const std::string differentSamples = "spectral_outside zero\nspectral_samples 1100 2\nspectral_samples 1200 5\nspectral_samples 1300 6\n";
            IJobPriv* different = Load(start+differentSamples+"}\n");
            Check(different!=nullptr,"distinct SWIR source fixture loads");
            if (different) {
                Check(risequiv::DumpJob(*dynamic_cast<Job*>(different)) != digest,
                    "generic CST equivalence distinguishes two RGB-black spectra");
                different->release();
            }
            const std::string serialized = Cst::SerializeCst(*job->GetCstDocument());
            job->release();
            IJobPriv* reloaded = Load(serialized.substr(serialized.find('\n')+1));
            Check(reloaded!=nullptr,"CST source roundtrip");
            if (reloaded) {
                Near(reloaded->GetLights()->GetItem("L")->emittedRadianceNM(Vector3(0,0,-1),1200),12,"roundtrip spectral equality");
                Check(risequiv::DumpJob(*dynamic_cast<Job*>(reloaded)) == digest,"physical source generic CST roundtrip equivalence");
                reloaded->release();
            }
        }
        IJobPriv* bad = Load(start+samples+"color 1 1 1\n}\n");
        Check(!bad,"reject ambiguous physical plus RGB authoring"); if (bad) bad->release();
        bad = Load(start+"spectral_samples 1100 2\nspectral_samples 1300 6\n}\n");
        Check(!bad,"reject implicit out-of-range policy"); if (bad) bad->release();
        bad = Load(start+samples+"spectral_samples 1400 1 extra\n}\n");
        Check(!bad,"reject malformed spectral tuple"); if (bad) bad->release();
    }
}
}
int main() { SourceControls(); DeltaControls(); PhysicalRuntimeControls(); SceneControls(); std::printf("PhysicalLightSpectrumTest: %d failures\n",failures); return failures ? 1 : 0; }
