// P6a source evaluation/construction controls. This does not certify band-aware
// source selection, a radiometric film, or out-of-visible rendering.
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/ILightManager.h"
#include "../src/Library/Interfaces/IKeyframable.h"
#include "../src/Library/Lights/PointLight.h"
#include "../src/Library/Lights/SpotLight.h"
#include "../src/Library/Lights/DirectionalLight.h"
#include "../src/Library/Lights/AmbientLight.h"
#include "../src/Library/Lights/PhysicalLightSpectrum.h"
#include "../src/Library/Objects/SnapshotLeafClone.h"
#include "../src/Library/SceneEditor/LightIntrospection.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Scene.h"
#include "../src/Library/Managers/LightManager.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Cst/Cst.h"
#include "TestTempDir.h"
#include "CstRenderEquivalence.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>
#include <utility>

using namespace RISE;
using namespace RISE::Implementation;
namespace RISE { bool RISE_CreateJobPriv(IJobPriv** ppi); }
namespace {
int failures = 0;
void Check(bool ok, const char* label) { if (!ok) { ++failures; std::printf("FAIL: %s\n",label); } }
void Near(double a, double b, const char* label) {
    const bool ok=std::isfinite(a) && std::fabs(a-b)<1e-11*std::fmax(1.0,std::fabs(b));
    if(!ok) std::printf("actual %.17g expected %.17g: ",a,b);
    Check(ok,label);
}
// Never compare subnormals by a potentially overflowed fast-math reciprocal.
void Relative(double a,double b,const char* label) {
    int ae=0,be=0;
    const double af=std::frexp(a,&ae),bf=std::frexp(b,&be);
    Near(std::ldexp(af/bf,ae-be),1,label);
}
class UnitBSDF final : public IBSDF, public Reference {
    Scalar response;
public:
    explicit UnitBSDF(Scalar r=.25):response(r) {}
    RISEPel value(const Vector3&, const RayIntersectionGeometric&) const override { return RISEPel(response,response,response); }
    Scalar valueNM(const Vector3&, const RayIntersectionGeometric&, Scalar) const override { return response; }
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
void ScaledSourceReviewControls() {
    IShader* shader=nullptr; RISE_API_CreateStandardShader(&shader,{});
    IRayCaster* caster=nullptr; RISE_API_CreateRayCaster(&caster,false,4,*shader,false);
    auto* bsdf=new UnitBSDF;
    RayIntersectionGeometric ri(Ray(Point3(0,0,1),Vector3(0,0,-1)),nullRasterizerState);
    ri.ptIntersection=Point3(0,0,0);ri.vNormal=Vector3(0,0,1);ri.vGeomNormal=ri.vNormal;
    const double rampNm[]={1100,1300};
    const double sub=std::numeric_limits<double>::denorm_min(), ramp[]={0,sub}, huge[]={1e308,1e308};
    const double expected=(sub*1e308)/100;
    for(int kind=0;kind<3;++kind) {
        ILightPriv* light=nullptr;
        const bool ok=kind==0 ? RISE_API_CreatePhysicalPointOmniLight(&light,2,rampNm,ramp,"zero",1e308,false) :
            kind==1 ? RISE_API_CreatePhysicalPointSpotLight(&light,2,rampNm,ramp,"zero",1e308,Point3(0,0,2),Point3(0,0,0),.4,.8,false) :
            RISE_API_CreatePhysicalDirectionalLight(&light,2,rampNm,ramp,"zero",1e308,Vector3(0,0,1));
        Check(ok&&light,"subnormal-ramp physical source fixture");
        if(light) {
            if(kind==0){light->SetPosition(Point3(0,0,2));light->FinalizeTransformations();}
            Near(light->emittedRadianceNM(Vector3(0,0,-1),1102)/expected,1,
                "scale precedes interpolation rounding at every NM emitter [physical-spd-scaled-interpolation]");
            Near(light->ComputeDirectLightingNM(ri,*caster,*bsdf,false,1102)/(expected*(kind==2 ? .25:.0625)),1,
                "direct NM preserves interpolated subnormal source before scaling");
            light->release();
        }
        light=nullptr;
        const bool bright=kind==0 ? RISE_API_CreatePhysicalPointOmniLight(&light,2,rampNm,huge,"zero",1e-308,false) :
            kind==1 ? RISE_API_CreatePhysicalPointSpotLight(&light,2,rampNm,huge,"zero",1e-308,Point3(0,0,.1),Point3(0,0,0),.4,.8,false) :
            RISE_API_CreatePhysicalDirectionalLight(&light,2,rampNm,huge,"zero",1e-308,Vector3(0,0,1));
        Check(bright&&light,"huge spectrum with compensating source scale fixture");
        if(light) {
            if(kind==0){light->SetPosition(Point3(0,0,.1));light->FinalizeTransformations();}
            Near(light->ComputeDirectLightingNM(ri,*caster,*bsdf,false,1200),kind==2 ? .25:25,
                "physical source scale precedes BRDF and inverse-square factors [physical-spd-direct-order]");
            light->release();
        }
    }
    const double visibleNm[]={380,780}, unit[]={1,1}, visibleRamp[]={0,sub}, scaledRamp[]={0,sub*1e308};
    ILightPriv *faint=nullptr,*reference=nullptr;
    Check(RISE_API_CreatePhysicalPointOmniLight(&faint,2,visibleNm,visibleRamp,"zero",1e308,false)&&
        RISE_API_CreatePhysicalPointOmniLight(&reference,2,visibleNm,scaledRamp,"zero",1,false),"scaled visible ramp fixture");
    if(faint&&reference) {
        const RISEPel actual=faint->emittedRadiance(Vector3(0,0,-1)), expect=reference->emittedRadiance(Vector3(0,0,-1));
        Check(expect.r>0&&expect.g>0&&expect.b>0,"visible ramp has independently scaled positive RGB projection");
        Near(actual.r/expect.r,1,"RGB source projection preserves scaled subnormal ramp red");
        Near(actual.g/expect.g,1,"RGB source projection preserves scaled subnormal ramp green");
        Near(actual.b/expect.b,1,"RGB source projection preserves scaled subnormal ramp blue");
    }
    safe_release(faint);safe_release(reference);
    const double fullNm[]={380,780,1100}, fullValues[]={1e-308,1e-308,1e308}, faintFlat[]={1e-308,1e-308};
    Check(RISE_API_CreatePhysicalPointOmniLight(&faint,3,fullNm,fullValues,"zero",1,false)&&
        RISE_API_CreatePhysicalPointOmniLight(&reference,2,visibleNm,faintFlat,"zero",1,false),"huge SWIR with faint visible component fixture");
    if(faint&&reference) {
        const RISEPel actual=faint->emittedRadiance(Vector3(0,0,-1)),expect=reference->emittedRadiance(Vector3(0,0,-1));
        Check(actual.r>0&&expect.r>0,"out-of-visible maximum does not erase faint visible source");
        Near(actual.r/expect.r,1,"visible projection normalization excludes unrelated SWIR maximum");
    }
    safe_release(faint);safe_release(reference);
    ILightPriv* spot=nullptr;
    Check(RISE_API_CreatePhysicalPointSpotLight(&spot,2,visibleNm,unit,"zero",1,Point3(0,0,0),Point3(1,1,1),.4,.8,false),"diagonal physical spot fixture");
    if(spot) {
        const Vector3 axis=spot->emissionDirection();
        RayIntersectionGeometric diagonal(Ray(Point3(0,0,0),Vector3(1,1,1)),nullRasterizerState);
        diagonal.ptIntersection=Point3(1,1,1);diagonal.vNormal=-axis;diagonal.vGeomNormal=-axis;
        RISEPel actual;spot->ComputeDirectLighting(diagonal,*caster,*bsdf,false,actual);
        const RISEPel emitted=spot->emittedRadiance(axis);
        Near(actual.r/emitted.r,1.0/12,"physical RGB spot direct agrees with emission on rounded diagonal axis [physical-spot-rgb-angle]");
        Near(spot->ComputeDirectLightingNM(diagonal,*caster,*bsdf,false,500),1.0/12,"diagonal spot NM analytic control");
        spot->release();
    }
    ILightPriv* directional=nullptr;
    Check(RISE_API_CreatePhysicalDirectionalLight(&directional,2,visibleNm,unit,"zero",1,Vector3(0,0,1)),"directional edit fixture");
    if(directional) {
        for(const char* invalid : {"0 0 0","inf 0 0"}) {
            auto* key=directional->KeyframeFromParameters("direction",invalid);
            Check(!key,"physical directional rejects invalid direction key [physical-directional-key]");
            if(key)key->release();
        }
        for(const Vector3& invalid : {Vector3(0,0,0),Vector3(std::numeric_limits<double>::infinity(),0,0)}) { // HYGIENE-OK: intentional invalid direction input.
            auto* key=directional->KeyframeFromParameters("direction","0 1 0");
            Check(key!=nullptr,"valid directional key fixture");
            if(key) {Vector3 value=invalid;key->setValue(&value);directional->SetIntermediateValue(*key);key->release();}
            Near(directional->emissionDirection().z,1,"invalid interpolated physical direction retains prior axis");
            Near(directional->ComputeDirectLightingNM(ri,*caster,*bsdf,false,500),.25,"retained directional axis keeps analytic NM response");
        }
        directional->release();
    }
    bsdf->release();caster->release();shader->release();
}
void PhysicalRangeReviewControls() {
    IShader* shader=nullptr;RISE_API_CreateStandardShader(&shader,{});
    IRayCaster* caster=nullptr;RISE_API_CreateRayCaster(&caster,false,4,*shader,false);
    auto* bsdf=new UnitBSDF;
    const double swir[]={1100,1300},one[]={1,1};
    const double diagonal=1/std::sqrt(3.0);
    for(const double magnitude : {1.0,1.6e-162,std::numeric_limits<double>::denorm_min(),1e308}) {
        ILightPriv* light=nullptr;
        const Vector3 authored(magnitude,magnitude,magnitude);
        Check(RISE_API_CreatePhysicalDirectionalLight(&light,2,swir,one,"zero",1,authored),"finite nonzero physical axis magnitude accepted [physical-axis-scale]");
        if(light) {
            const Vector3 axis=light->emissionDirection();
            Near(axis.x,diagonal,"directional normalization invariant to finite axis magnitude");
            Near(Vector3Ops::Dot(axis,axis),1,"directional physical axis remains unit length");
            RayIntersectionGeometric ri(Ray(Point3(0,0,0),-Vector3(diagonal,diagonal,diagonal)),nullRasterizerState);
            ri.vNormal=Vector3(diagonal,diagonal,diagonal);ri.vGeomNormal=ri.vNormal;
            Near(light->ComputeDirectLightingNM(ri,*caster,*bsdf,false,1200),.25,"directional lighting invariant to axis magnitude");
            char value[128];std::snprintf(value,sizeof(value),"%.17g %.17g %.17g",magnitude,magnitude,magnitude);
            auto* key=light->KeyframeFromParameters("direction",value);
            Check(key!=nullptr,"magnitude-invariant physical direction key accepted");
            if(key){light->SetIntermediateValue(*key);key->release();}
            Near(light->emissionDirection().x,diagonal,"physical direction key uses scaled normalization");
            light->release();
        }
        light=nullptr;
        Check(RISE_API_CreatePhysicalPointSpotLight(&light,2,swir,one,"zero",1,Point3(0,0,0),Point3(authored.x,authored.y,authored.z),.4,.8,false),
            "spot axis accepts finite nonzero magnitude");
        if(light) {
            Near(light->emissionDirection().x,diagonal,"spot physical axis uses shared scaled normalization");
            char value[128];std::snprintf(value,sizeof(value),"%.17g %.17g %.17g",magnitude,magnitude,magnitude);
            auto* key=light->KeyframeFromParameters("target",value);Check(key!=nullptr,"scaled spot target key accepted");
            if(key){light->SetIntermediateValue(*key);key->release();light->RegenerateData();}
            Near(light->emissionDirection().x,diagonal,"spot target key preserves unit axis");light->release();
        }
    }
    const double visible[]={380,780},large[]={1.5e308,1.5e308},referenceValues[]={1.5,1.5};
    for(int kind=0;kind<3;++kind) {
        ILightPriv *light=nullptr,*reference=nullptr;
        const auto create=[&](ILightPriv** out,const double* values,double scale) {
            return kind==0 ? RISE_API_CreatePhysicalPointOmniLight(out,2,visible,values,"zero",scale,false) :
                kind==1 ? RISE_API_CreatePhysicalPointSpotLight(out,2,visible,values,"zero",scale,Point3(0,0,2),Point3(0,0,0),.4,.8,false) :
                RISE_API_CreatePhysicalDirectionalLight(out,2,visible,values,"zero",scale,Vector3(0,0,1));
        };
        Check(create(&light,large,1e-308)&&create(&reference,referenceValues,1),
            "overflowing unscaled visible projection does not reject finite physical source [physical-visible-overflow]");
        if(light&&reference) {
            const RISEPel actual=light->emittedRadiance(Vector3(0,0,-1)),expected=reference->emittedRadiance(Vector3(0,0,-1));
            Near(actual.r/expected.r,1,"compensating visible source RGB projection matches finite reference");
            Near(light->emittedRadianceNM(Vector3(0,0,-1),500),1.5,"compensating visible source NM amplitude");
            const RISEPel color=light->emissionColor();
            Check(std::isfinite(color.r)&&std::isfinite(color.g)&&std::isfinite(color.b),"physical bookkeeping preview always finite");
            Near(color.r/actual.r,1,"physical emissionColor is scale-applied preview");
            const ILight* clone=CloneLightForSnapshot(light);
            Check(clone!=nullptr,"compensating source snapshot clones");
            if(clone){Near(clone->emittedRadiance(Vector3(0,0,-1)).r/expected.r,1,"snapshot never resolves unscaled overflow preview");clone->release();}
        }
        safe_release(light);safe_release(reference);
    }
    RayIntersectionGeometric ri(Ray(Point3(0,0,1),Vector3(0,0,-1)),nullRasterizerState);
    ri.ptIntersection=Point3(0,0,0);ri.vNormal=Vector3(0,0,1);ri.vGeomNormal=ri.vNormal;
    const double faint[]={1e-300,1e-300};
    for(int kind=0;kind<2;++kind) for(int control=0;control<3;++control) {
        const double distance=control==0 ? 1e-10 : control==1 ? 1e-200 : 1e200;
        const double scale=control==0 ? 1e-30 : control==1 ? 1e-100 : 1;
        const double huge[]={1e308,1e308};const double* values=control==2 ? huge:faint;
        const double expected=control==0 ? 2.5e-311 : control==1 ? .25:2.5e-93;
        ILightPriv* light=nullptr;
        const bool ok=kind==0 ? RISE_API_CreatePhysicalPointOmniLight(&light,2,swir,values,"zero",scale,false) :
            RISE_API_CreatePhysicalPointSpotLight(&light,2,swir,values,"zero",scale,Point3(0,0,distance),Point3(0,0,0),.4,.8,false);
        Check(ok&&light,"physical extreme distance fixture");
        if(light) {
            if(kind==0){light->SetPosition(Point3(0,0,distance));light->FinalizeTransformations();}
            const double actual=light->ComputeDirectLightingNM(ri,*caster,*bsdf,false,1200);
            Check(actual>0&&std::isfinite(actual),"recoverable physical direct NM stays finite and nonzero [physical-direct-product]");
            Relative(actual,expected,"source, scale and separate distance divisors round only at final direct result");
            light->release();
        }
    }
    for(int kind=0;kind<3;++kind) {
        ILightPriv *light=nullptr,*reference=nullptr;
        const auto create=[&](ILightPriv** out,const double* samples,double scale) {
            return kind==0 ? RISE_API_CreatePhysicalPointOmniLight(out,2,visible,samples,"zero",scale,false) :
                kind==1 ? RISE_API_CreatePhysicalPointSpotLight(out,2,visible,samples,"zero",scale,Point3(0,0,1e-10),Point3(0,0,0),.4,.8,false) :
                RISE_API_CreatePhysicalDirectionalLight(out,2,visible,samples,"zero",scale,Vector3(0,0,1));
        };
        Check(create(&light,faint,1e-30)&&create(&reference,one,1),"RGB recoverable direct product fixture");
        auto* response=kind==2 ? new UnitBSDF(1e20):new UnitBSDF;
        if(light&&reference) {
            if(kind==0){light->SetPosition(Point3(0,0,1e-10));light->FinalizeTransformations();}
            RISEPel actual;light->ComputeDirectLighting(ri,*caster,*response,false,actual);
            const double multiplier=kind==2 ? 1e-310 : 2.5e-311;
            const RISEPel expected=reference->emittedRadiance(Vector3(0,0,-1))*multiplier;
            Check(actual.r>0&&actual.g>0&&actual.b>0,"RGB direct projection is not rounded before response/geometric amplification");
            Relative(actual.r,expected.r,"RGB direct complete product red");Relative(actual.g,expected.g,"RGB direct complete product green");Relative(actual.b,expected.b,"RGB direct complete product blue");
            if(kind==2) {
                const double actualNM=light->ComputeDirectLightingNM(ri,*caster,*response,false,500);
                Check(actualNM>0,"directional direct NM keeps source parts through response");
                Relative(actualNM,1e-310,"directional direct product analytic oracle");
            }
        }
        response->release();safe_release(light);safe_release(reference);
    }
    bsdf->release();caster->release();shader->release();
}
void SelectedPhysicalProductControls() {
    class Samples final: public ISampler {
    public:
        Scalar Get1D() override {return .37;}
        Point2 Get2D() override {return Point2(.37,.37);}
    } samples;
    auto* scene=new Scene;
    auto* lights=new LightManager;
    IObjectManager* objects=nullptr;RISE_API_CreateObjectManager(&objects,false,false,4,32);
    scene->SetObjectManager(objects);scene->SetLightManager(lights);
    IShader* shader=nullptr;RISE_API_CreateStandardShader(&shader,{});
    auto* bsdf=new UnitBSDF;
    RayIntersectionGeometric ri(Ray(Point3(0,0,1),Vector3(0,0,-1)),nullRasterizerState);
    ri.ptIntersection=Point3(0,0,0);ri.vNormal=Vector3(0,0,1);ri.vGeomNormal=ri.vNormal;ri.onb.CreateFromW(ri.vNormal);
    const double swir[]={1100,1300},faint[]={1e-300,1e-300};
    for(int kind=0;kind<2;++kind) for(int count : {1,2}) {
        for(int i=0;i<count;++i) {
            ILightPriv* light=nullptr;
            const bool ok=kind==0 ? RISE_API_CreatePhysicalPointOmniLight(&light,2,swir,faint,"zero",1e-30,false) :
                RISE_API_CreatePhysicalPointSpotLight(&light,2,swir,faint,"zero",1e-30,Point3(0,0,1e-10),Point3(0,0,0),.4,.8,false);
            Check(ok&&light,"actual selected physical source fixture");
            if(light) {
                if(kind==0) {light->SetPosition(Point3(0,0,1e-10));light->FinalizeTransformations();}
                lights->AddItem(light,i==0 ? "first":"second");light->release();
            }
        }
        auto* caster=new RayCaster(false,4,*shader,false);
        caster->SetLightSampleRRThreshold(1e9);
        caster->SetSpectralSourceDomain(SpectralSourceDomain::Create({{1200,1}}));
        caster->AttachScene(scene);
        const auto* sampler=caster->GetLightSampler();
        const double actual=sampler->EvaluateDirectLightingNM(ri,*bsdf,nullptr,1200,*caster,samples,nullptr,nullptr,false,nullptr);
        Check(actual>0&&std::isfinite(actual),"selected physical NEE retains subnormal source until geometry and selection [physical-selected-product]");
        Relative(actual,count*2.5e-311,"selected physical NEE analytic product including exact source probability");
        caster->release();lights->RemoveItem("first");if(count==2) lights->RemoveItem("second");
    }
    bsdf->release();shader->release();objects->release();lights->release();scene->release();
}
// Unknown derived lights follow the documented shared snapshot policy: exact
// object identity preserves overrides, and the acquired reference owns lifetime.
template<class Base> class SnapshotOverrideLight final: public Base {
    unsigned int& destroyed;
public:
    template<class... Args> SnapshotOverrideLight(unsigned int& count,Args&&... args)
        : Base(std::forward<Args>(args)...),destroyed(count) {}
    ~SnapshotOverrideLight() override {++destroyed;}
    RISEPel emittedRadiance(const Vector3&) const override {return RISEPel(37,41,43);}
    Scalar emittedRadianceNM(const Vector3&,Scalar) const override {return 37;}
};
void SnapshotLightPolicyControls() {
    const double wavelengths[]={1100,1300},samples[]={4,4};
    const auto spectrum=PhysicalLightSpectrum::Create(wavelengths,samples,2,"zero");
    for(int kind=0;kind<4;++kind) {
        unsigned int destroyed=0;
        ILightPriv* original=kind==0 ? static_cast<ILightPriv*>(new SnapshotOverrideLight<PointLight>(destroyed,1,RISEPel(0,0,0),false,spectrum)) :
            kind==1 ? static_cast<ILightPriv*>(new SnapshotOverrideLight<SpotLight>(destroyed,1,Point3(0,0,-1),.4,.8,RISEPel(0,0,0),false,spectrum)) :
            kind==2 ? static_cast<ILightPriv*>(new SnapshotOverrideLight<DirectionalLight>(destroyed,1,RISEPel(0,0,0),Vector3(0,0,1),spectrum)) :
            static_cast<ILightPriv*>(new SnapshotOverrideLight<AmbientLight>(destroyed,1,RISEPel(1,1,1)));
        Check(original->refcount()==1,"custom light starts with one owned reference");
        const ILight* snapshot=CloneLightForSnapshot(original);
        Check(snapshot==original,"snapshot retains exact custom light identity [physical-snapshot-subclass]");
        Check(original->refcount()==2,"unknown light snapshot acquires exactly one reference");
        original->release();
        Check(destroyed==0,"snapshot keeps custom light alive after live owner releases");
        if(snapshot) {
            Check(snapshot->refcount()==1,"snapshot owns remaining custom light reference");
            const RISEPel rgb=snapshot->emittedRadiance(Vector3(0,0,-1));
            Near(rgb.r,37,"snapshot preserves derived red emission");Near(rgb.g,41,"snapshot preserves derived green emission");Near(rgb.b,43,"snapshot preserves derived blue emission");
            Near(snapshot->emittedRadianceNM(Vector3(0,0,-1),1200),37,"snapshot preserves derived NM emission instead of stored physical SPD");
            snapshot->release();
        }
        Check(destroyed==1,"last snapshot reference destroys custom light exactly once");
    }
    for(int kind=0;kind<3;++kind) {
        ILightPriv* original=nullptr;
        const bool ok=kind==0 ? RISE_API_CreatePhysicalPointOmniLight(&original,2,wavelengths,samples,"zero",2,false) :
            kind==1 ? RISE_API_CreatePhysicalPointSpotLight(&original,2,wavelengths,samples,"zero",2,Point3(0,0,2),Point3(0,0,0),.4,.8,false) :
            RISE_API_CreatePhysicalDirectionalLight(&original,2,wavelengths,samples,"zero",2,Vector3(0,0,1));
        Check(ok&&original,"ordinary physical snapshot fixture");if(!original) continue;
        const auto authored=GetPhysicalLightSpectrum(*original);
        const ILight* snapshot=CloneLightForSnapshot(original);
        Check(snapshot&&snapshot!=original,"exact built-in physical light still gets independent snapshot");
        Check(original->refcount()==1,"ordinary light clone does not retain mutable live light");
        original->release();
        if(snapshot) {
            Check(snapshot->refcount()==1&&GetPhysicalLightSpectrum(*snapshot)==authored,"independent built-in snapshot retains immutable spectrum");
            Near(snapshot->emittedRadianceNM(Vector3(0,0,-1),1200),8,"ordinary physical snapshot emission survives live owner destruction");
            snapshot->release();
        }
    }
    ILightPriv* ambient=nullptr;RISE_API_CreateAmbientLight(&ambient,2,RISEPel(.2,.3,.4));
    const ILight* snapshot=CloneLightForSnapshot(ambient);
    Check(snapshot&&snapshot!=ambient,"exact built-in ambient still gets independent snapshot");
    if(snapshot) {
        const Vector3 direction(0,0,1);
        Near(snapshot->emittedRadiance(direction).r,ambient->emittedRadiance(direction).r,"ordinary ambient RGB snapshot unchanged");
        Near(snapshot->emittedRadianceNM(direction,500),ambient->emittedRadianceNM(direction,500),"ordinary ambient NM snapshot unchanged");
        snapshot->release();
    }
    ambient->release();
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
int main() { SourceControls(); DeltaControls(); PhysicalRuntimeControls(); ScaledSourceReviewControls(); PhysicalRangeReviewControls();SelectedPhysicalProductControls(); SnapshotLightPolicyControls(); SceneControls(); std::printf("PhysicalLightSpectrumTest: %d failures\n",failures); return failures ? 1 : 0; }
