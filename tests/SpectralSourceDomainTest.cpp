// Scoped P6a proposal controls. No production raw output/coverage claim.
#include "../src/Library/RISE_API.h"
#include "../src/Library/Scene.h"
#include "../src/Library/Interfaces/IObjectPriv.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IGeometry.h"
#include "../src/Library/Interfaces/IEmitter.h"
#include "../src/Library/Managers/LightManager.h"
#include "../src/Library/Lights/PointLight.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace RISE;
using namespace RISE::Implementation;
namespace {
int failures=0;
void Check(bool ok,const char* label) { if (!ok) { ++failures; std::printf("FAIL %s\n",label); } }
void Near(double a,double b,const char* label,double tolerance=1e-11) {
    Check(std::isfinite(a) && std::abs(a-b)<=tolerance*std::max(1.0,std::abs(b)),label);
}
class Samples final: public ISampler {
    std::mt19937_64 rng{1234567};
public:
    Scalar Get1D() override { return std::generate_canonical<double,53>(rng); }
    Point2 Get2D() override { return Point2(Get1D(),Get1D()); }
};
class FixedSamples final: public ISampler {
public:
    double first=.5,rest=.37;
    Scalar Get1D() override { const auto r=first; first=rest; return r; }
    Point2 Get2D() override { return Point2(Get1D(),Get1D()); }
};
class PairSamples final: public ISampler {
    unsigned int index=0;
    double first,second;
public:
    PairSamples(double a,double b):first(a),second(b) {}
    Scalar Get1D() override {++index;return index==1?first:index==2?second:.37;}
    Point2 Get2D() override {return Point2(Get1D(),Get1D());}
};
class UnitBSDF final: public IBSDF, public Reference {
public:
    RISEPel value(const Vector3&,const RayIntersectionGeometric&) const override {return RISEPel(.25,.25,.25);}
    Scalar valueNM(const Vector3&,const RayIntersectionGeometric&,Scalar) const override {return .25;}
};
class SpyPoint final: public PointLight {
public:
    mutable unsigned int deterministic=0;
    SpyPoint(std::shared_ptr<const PhysicalLightSpectrum> spd):PointLight(1,RISEPel(0,0,0),false,std::move(spd)) {}
    Scalar ComputeDirectLightingSampledNM(const RayIntersectionGeometric& ri,const IRayCaster& caster,
        const IBSDF& bsdf,bool shadows,Scalar nm,bool fullSphere,bool volume,const IORStack* stack,
        ISampler& sampler,MediumBoundaryHits* boundaries) const override {
        ++deterministic;
        return PointLight::ComputeDirectLightingSampledNM(ri,caster,bsdf,shadows,nm,fullSphere,volume,stack,sampler,boundaries);
    }
};
class SpectralPainter final: public UniformColorPainter {
public:
    double lo=1100,hi=1300,amplitude=4;
    bool patch=false;
    bool requireRecords=false;
    mutable unsigned int incomplete=0;
    explicit SpectralPainter(double a=4):UniformColorPainter(RISEPel(0,0,0)),amplitude(a) {}
    Scalar GetColorNM(const RayIntersectionGeometric&,Scalar) const override {return .8;}
    Scalar GetRadianceNM(const RayIntersectionGeometric& ri,Scalar nm) const override {
        if (nm<lo || nm>hi) return 0;
        if (patch && !(ri.ptCoord.x>.001 && ri.ptCoord.x<.002)) return 0;
        if (requireRecords && (!ri.pmxObjectToWorld || !ri.pmxWorldToObject || !ri.pObjectInfo ||
            std::abs(ri.ptIntersection.z-2)>1e-8 || std::abs(ri.ptObjIntersec.z)>1e-8)) ++incomplete;
        return amplitude;
    }
};
class WorldMap final: public IRadianceMap, public Reference {
    const IPainter& painter;
    Matrix4 transform;
    double scale=3;
public:
    bool patch=false;
    enum class Fault { None, Negative, Nonfinite } fault=Fault::None;
    double time=1;
    mutable unsigned int queries=0;
    explicit WorldMap(const IPainter& p):painter(p),transform(Matrix4Ops::Identity()) {painter.addref();}
    ~WorldMap() override {painter.release();}
    RISEPel GetRadiance(const Ray&,const RasterizerState&) const override {return RISEPel(0,0,0);}
    Scalar GetRadianceNM(const Ray& r,const RasterizerState&,Scalar nm) const override {
        ++queries;
        if (fault==Fault::Negative) return -1;
        if (fault==Fault::Nonfinite) return std::numeric_limits<double>::quiet_NaN(); // HYGIENE-OK: intentional rejected emission input, not a sentinel.
        if (nm<1100 || nm>1300) return 0;
        // Narrow world-space patch intentionally missed by the 8x16 grid.
        if (patch && !(r.Dir().x>.99999)) return 0;
        const Vector3 local=Vector3Ops::Transform(transform,r.Dir());
        return scale*time*(patch ? 7 : (2+local.x));
    }
    void SetOrientation(const Vector3&) override {}
    void SetTransformation(const Matrix4& t) override {transform=t;}
    const IPainter& GetPainter() const override {return painter;}
    Scalar GetScale() const override {return scale;}
    const Matrix4& GetTransform() const override {return transform;}
    void SetScale(Scalar v) override {scale=v;}
};
struct Fixture {
    Scene* scene=new Scene;
    IObjectManager* objects=nullptr;
    LightManager* lights=new LightManager;
    IShader* shader=nullptr;
    Fixture() {
        RISE_API_CreateObjectManager(&objects,false,false,4,32);
        scene->SetObjectManager(objects); scene->SetLightManager(lights);
        RISE_API_CreateStandardShader(&shader,{});
    }
    ~Fixture() {shader->release();objects->release();lights->release();scene->release();}
    RayCaster* Caster(std::shared_ptr<const SpectralSourceDomain> domain) {
        auto* c=new RayCaster(false,4,*shader,false);
        c->SetRISCandidates(16);c->SetUseLightBVH(true);c->SetLightSampleRRThreshold(1e9);
        c->SetSpectralSourceDomain(std::move(domain));c->AttachScene(scene);return c;
    }
    ILightPriv* Point(const char* name,double lower,double upper,double intensity) {
        const double nm[]={lower,upper},values[]={intensity,intensity};
        ILightPriv* l=nullptr; Check(RISE_API_CreatePhysicalPointOmniLight(&l,2,nm,values,"zero",1,false),"point fixture");
        if(l) { l->SetPosition(Point3(0,0,2)); l->FinalizeTransformations();lights->AddItem(l,name);l->release(); }
        return l;
    }
    IObjectPriv* Area(const char* name,SpectralPainter& painter) {
        const Point3 corners[]={Point3(-1,-1,0),Point3(1,-1,0),Point3(1,1,0),Point3(-1,1,0)};
        IGeometry* geometry=nullptr;IMaterial* nullMaterial=nullptr;IMaterial* material=nullptr;IObjectPriv* object=nullptr;
        RISE_API_CreateClippedPlaneGeometry(&geometry,corners,true);RISE_API_CreateNullMaterial(&nullMaterial);
        RISE_API_CreateLambertianLuminaireMaterial(&material,painter,*nullMaterial,1);
        RISE_API_CreateObject(&object,geometry);object->AssignMaterial(*material);object->SetPosition(Point3(0,0,2));object->FinalizeTransformations();
        objects->AddItem(object,name);geometry->release();material->release();nullMaterial->release();object->release();return object;
    }
};
std::shared_ptr<const SpectralSourceDomain> Domain(double nm) {return SpectralSourceDomain::Create({{nm,1}});}
RayIntersectionGeometric Receiver() {
    RayIntersectionGeometric ri(Ray(Point3(0,0,1),Vector3(0,0,-1)),nullRasterizerState);
    ri.ptIntersection=Point3(0,0,0);ri.vNormal=Vector3(0,0,1);ri.vGeomNormal=ri.vNormal;ri.onb.CreateFromW(ri.vNormal);
    return ri;
}
void DomainControls() {
    bool rejected=false;try {SpectralSourceDomain::Create({{1200,0}});} catch(const std::invalid_argument&){rejected=true;}
    Check(rejected,"reject zero quadrature weight");
    rejected=false;try {SpectralSourceDomain::Create({{1200,1},{1100,1}});} catch(const std::invalid_argument&){rejected=true;}
    Check(rejected,"reject unordered domain");
    rejected=false;try {SpectralSourceDomain::Create({{1200,1}},1e-20);} catch(const std::invalid_argument&){rejected=true;}
    Check(rejected,"reject tiny mixture that loses 32-bit Sobol/root support");
    const auto boundary=SpectralSourceDomain::Create({{1200,1}},2*SpectralSourceDomain::MinimumSourceProbability());
    SpectralSourceDomain::Mass empty,bright;bright.Add(1);
    const auto edge=boundary->Probabilities({empty,bright});
    Near(edge[0]/SpectralSourceDomain::MinimumSourceProbability(),1,"accepted two-cell root boundary floor");
    Check(edge[1]<1 && 1-edge[1]>0,"dominant class complement stays positive at supported boundary");
    AliasTable alias;alias.Build(edge);
    Check(alias.Sample(0)==0,"alias first Sobol endpoint reaches small first source");
    alias.Build({edge[1],edge[0]});
    Check(alias.Sample(.5)==1,"alias bin endpoint reaches small last source");
    Check(alias.Sample(1-1.0/4294967296.0)<2,"alias last Sobol endpoint remains in bounds");
    rejected=false;try {boundary->Probabilities({empty,bright,empty});} catch(const std::invalid_argument&){rejected=true;}
    Check(rejected,"reject actual source count with fewer than two Sobol cells per fallback");
    auto d=Domain(1200);
    SpectralSourceDomain::Mass tiny,huge,zero;
    tiny.Add(1e-300,1e-300);huge.Add(1e300,1e300);
    auto q=d->Probabilities({tiny,huge,zero});
    Near(q[0],.05/3,"tiny source retains actual uniform share");Near(q[1],.95+.05/3,"huge source stable normalization");
    Near(q[2],.05/3,"zero quadrature source retains support");Near(q[0]+q[1]+q[2],1,"PMF sums to unity");
}
double RootPMF(const LightSampler* ls,const IScene& scene,const ILight& light) {
    LuminaryManager::LuminariesList empty;
    return ls->PdfSelectLight(scene,empty,light,Point3(0,0,0),Vector3(0,0,1));
}
void DeltaAndContextControls() {
    Fixture f; const double nm[]={1100,1300},values[]={4,4};
    auto* spy=new SpyPoint(PhysicalLightSpectrum::Create(nm,values,2,"zero"));
    spy->SetPosition(Point3(0,0,2));spy->FinalizeTransformations();f.lights->AddItem(spy,"swir");spy->release();
    auto* c=f.Caster(Domain(1200)); auto* brdf=new UnitBSDF; Samples random;auto ri=Receiver();
    const auto* ls=c->GetLightSampler();
    Near(ls->EvaluateDirectLightingNM(ri,*brdf,nullptr,1200,*c,random,nullptr,nullptr,false,nullptr),.25,"SWIR point NEE inverse-square exactly once");
    Check(spy->deterministic==0,"selected RGB-black point never enters deterministic direct pass");
    Near(RootPMF(ls,*f.scene,*spy),1,"pure SWIR point selectable");
    LuminaryManager::LuminariesList empty;LightSample sample;
    Check(ls->SampleLight(*f.scene,empty,random,sample) && sample.pLight==spy,"root retains actual source pointer");
    Near(sample.pLight->emittedRadianceNM(sample.direction,1200),4,"root source recovers real NM emission");
    c->release();
    {
        const double directionalValues[]={2,2};ILightPriv* directional=nullptr;
        RISE_API_CreatePhysicalDirectionalLight(&directional,2,nm,directionalValues,"zero",1,Vector3(0,0,1));
        f.lights->AddItem(directional,"directional");directional->release();
        c=f.Caster(Domain(1200));
        Near(c->GetLightSampler()->EvaluateDirectLightingNM(ri,*brdf,nullptr,1200,*c,random,nullptr,nullptr,false,nullptr),.75,
            "NM selected delta adds to deterministic directional contribution");
        c->release();f.lights->RemoveItem("directional");
    }
    brdf->release();
    const auto* visible=f.Point("visible",400,700,8);
    auto* a=f.Caster(Domain(550));auto* b=f.Caster(Domain(1200));
    const double qa=RootPMF(a->GetLightSampler(),*f.scene,*visible),qb=RootPMF(b->GetLightSampler(),*f.scene,*visible);
    Near(qa,.975,"visible pass favors visible source");Near(qb,.025,"SWIR pass preserves dark estimate fallback");
    for (unsigned int i=0;i<5;++i) Near(RootPMF(a->GetLightSampler(),*f.scene,*visible),qa,"other context queries never alter first table");
    a->SetSpectralSourceDomain(Domain(280));
    Near(RootPMF(a->GetLightSampler(),*f.scene,*visible),.5,"domain replacement rebuilds even without scene-generation change");
    Near(RootPMF(b->GetLightSampler(),*f.scene,*visible),qb,"other context stays stable across domain replacement");
    a->release();Near(RootPMF(b->GetLightSampler(),*f.scene,*visible),qb,"second context survives first destruction");
    unsigned int hits=0; const unsigned int trials=100000;
    for(unsigned int i=0;i<trials;++i) {
        Check(b->GetLightSampler()->SampleLight(*f.scene,empty,random,sample),"mixed source root sample");
        if(sample.pLight==visible) ++hits;
        Near(sample.pdfSelect,RootPMF(b->GetLightSampler(),*f.scene,*sample.pLight),"returned/query root PMF match");
    }
    Check(std::abs(double(hits)/trials-qb)<.002,"empirical source frequencies match PMF");
    b->SetUseLightBVH(true);b->SetRISCandidates(32);b->SetLightSampleRRThreshold(1e30);
    for(int i=0;i<10;++i){char name[30];std::snprintf(name,sizeof(name),"more%d",i);f.Point(name,1100,1300,1);}
    b->InvalidateLightSamplers();b->AttachScene(f.scene);
    Check(!b->GetLightSampler()->IsRISActive() && !b->GetLightSampler()->IsLightBVHActive(),"pending accelerators stay off after >8-source rebuild");
    Near(b->GetLightSampler()->LightSampleRRThreshold(),0,"pending RR stays off after rebuild");
    b->AccumulateLightMotionSample();b->RefreshLightSamplers();
    Check(!b->GetLightSampler()->IsRISActive() && !b->GetLightSampler()->IsLightBVHActive(),"motion refresh suppresses Prepare auto RIS");
    Near(b->GetLightSampler()->LightSampleRRThreshold(),0,"motion refresh keeps RR off");
    b->SetSpectralSourceDomain(nullptr);
    Check(b->GetLightSampler()->IsRISActive(),"clearing domain restores pending legacy RIS setting");
    Near(b->GetLightSampler()->LightSampleRRThreshold(),1e30,"clearing domain restores pending legacy RR setting");
    b->release();
    RayCaster* survivor=nullptr;const ILight* retained=nullptr;const IScene* retainedScene=nullptr;
    {
        Fixture temporary;retained=temporary.Point("retained",1100,1300,9);retainedScene=temporary.scene;
        survivor=temporary.Caster(Domain(1200));
    }
    Near(RootPMF(survivor->GetLightSampler(),*retainedScene,*retained),1,"caster retains scene and source assets after authoring owner destruction");
    Near(retained->emittedRadianceNM(Vector3(0,0,-1),1200),9,"retained source emission remains alive");survivor->release();
}
void UVAndMagnitudeControls() {
    Fixture f;auto* uv=f.Point("uv",250,300,6);auto* c=f.Caster(Domain(280));
    auto* bsdf=new UnitBSDF;Samples random;auto ri=Receiver();
    Near(RootPMF(c->GetLightSampler(),*f.scene,*uv),1,"pure UV point selectable");
    Near(c->GetLightSampler()->EvaluateDirectLightingNM(ri,*bsdf,nullptr,280,*c,random,nullptr,nullptr,false,nullptr),.375,"UV direct irradiance oracle");
    c->release();f.lights->RemoveItem("uv");
    auto* tiny=f.Point("tiny",1100,1300,1e-300);auto* huge=f.Point("huge",1100,1300,1e300);
    c=f.Caster(Domain(1200));Near(RootPMF(c->GetLightSampler(),*f.scene,*tiny),.025,"actual tiny-source proposal floor");
    Near(RootPMF(c->GetLightSampler(),*f.scene,*huge),.975,"actual huge-source stable proposal");
    c->release();bsdf->release();
    Fixture tailFixture;tailFixture.Point("visible",400,700,8);
    const double tailNm[]={400,700,1099,1100,1300},tailValues[]={1e-200,1e-200,0,4,4};ILightPriv* tail=nullptr;
    Check(RISE_API_CreatePhysicalPointOmniLight(&tail,5,tailNm,tailValues,"zero",1,false),"tiny visible-tail SWIR source fixture");
    tail->SetPosition(Point3(0,0,2));tail->FinalizeTransformations();tailFixture.lights->AddItem(tail,"tail");tail->release();
    Check(ColorMath::MaxValue(tail->radiantExitance())>0,"tail fixture really has positive visible energy");
    auto* visibleContext=tailFixture.Caster(Domain(550));auto* swirContext=tailFixture.Caster(Domain(1200));
    Near(RootPMF(visibleContext->GetLightSampler(),*tailFixture.scene,*tail),.025,"tiny visible tail does not remove support in visible context");
    Near(RootPMF(swirContext->GetLightSampler(),*tailFixture.scene,*tail),.975,"SWIR proposal follows spectral emission despite tiny visible tail");
    Near(tail->emittedRadianceNM(Vector3(0,0,-1),1200),4,"tiny-tail source retains actual SWIR emission");
    visibleContext->release();swirContext->release();
}
void AreaControls() {
    Fixture f;auto* painter=new SpectralPainter;auto* area=f.Area("area",*painter);
    Check(area->GetMaterial()->GetEmitter()->averageRadiantExitanceNM(1200)==0,"legacy packet is zero for SWIR emitter");
    painter->requireRecords=true;
    auto* c=f.Caster(Domain(1200));const auto* ls=c->GetLightSampler();
    Near(ls->CachedPdfSelectLuminary(*area,Point3(0,0,0),Vector3(0,0,1)),1,"SWIR area hit-side finite NEE PMF");
    auto* lm=new LuminaryManager;lm->AttachScene(f.scene);Samples rng;LightSample sample;
    Check(ls->SampleLight(*f.scene,lm->getLuminaries(),rng,sample) && sample.pLuminary==area,"SWIR area root sample");
    RayIntersectionGeometric rig(Ray(),nullRasterizerState);
    LightSampler::FillEmitterRecord(rig,sample.position,sample.normal,sample.ptCoord,sample.ptObjIntersec,sample.pLuminary);
    Near(area->GetMaterial()->GetEmitter()->emittedRadianceNM(rig,sample.direction,sample.normal,1200),4/PI,"area NM emission recovered from complete source record");
    Check(painter->incomplete==0,"spectral surface quadrature preserves world/object coordinates");
    c->release();
    area->SetShadowParams(false,false);
    const double dirNm[]={1100,1300},dirValues[]={2,2};ILightPriv* directional=nullptr;
    RISE_API_CreatePhysicalDirectionalLight(&directional,2,dirNm,dirValues,"zero",1,Vector3(0,0,1));
    auto* bsdf=new UnitBSDF;auto receiver=Receiver();
    c=f.Caster(Domain(1200));FixedSamples controlRng;
    const double areaOnly=c->GetLightSampler()->EvaluateDirectLightingNM(receiver,*bsdf,nullptr,1200,*c,controlRng,nullptr,nullptr,false,nullptr);
    Check(areaOnly>0,"directional+area test area contribution is nonzero");c->release();
    f.lights->AddItem(directional,"directional");directional->release();c=f.Caster(Domain(1200));FixedSamples mixedRng;
    const double mixed=c->GetLightSampler()->EvaluateDirectLightingNM(receiver,*bsdf,nullptr,1200,*c,mixedRng,nullptr,nullptr,false,nullptr);
    Near(mixed,areaOnly+.5,"NM selected area adds to deterministic directional contribution");
    c->release();f.lights->RemoveItem("directional");bsdf->release();
    painter->requireRecords=false;
    // Both wavelength and spatial quadrature miss. Geometry remains supported.
    painter->lo=1200.123;painter->hi=1200.124;painter->patch=true;
    c=f.Caster(Domain(1200));Near(c->GetLightSampler()->CachedPdfSelectLuminary(*area,Point3(0,0,0),Vector3(0,0,1)),1,"missed line and patch retain selectable source");
    rig.ptCoord.x=.0015;Near(painter->GetRadianceNM(rig,1200.1235),4,"missed spectral/spatial patch really emits");
    c->release();c=f.Caster(Domain(1200.1235));
    Near(c->GetLightSampler()->CachedPdfSelectLuminary(*area,Point3(0,0,0),Vector3(0,0,1)),1,"spatial patch missed independently of wavelength nodes retains support");
    c->release();painter->patch=false;c=f.Caster(Domain(1200));
    Near(c->GetLightSampler()->CachedPdfSelectLuminary(*area,Point3(0,0,0),Vector3(0,0,1)),1,"spectral line missed independently of surface nodes retains support");
    c->release();
    auto* bright=new SpectralPainter(8);auto* competitor=f.Area("bright",*bright);
    painter->patch=true;c=f.Caster(Domain(1200.1235));
    Near(c->GetLightSampler()->CachedPdfSelectLuminary(*area,Point3(0,0,0),Vector3(0,0,1)),.025,"missed patch retains actual mixture PMF beside bright competitor");
    FixedSamples forced;forced.first=0;forced.rest=.0015;
    auto* freshLuminaries=new LuminaryManager;freshLuminaries->AttachScene(f.scene);
    Check(c->GetLightSampler()->SampleLight(*f.scene,freshLuminaries->getLuminaries(),forced,sample) && sample.pLuminary==area && sample.pLuminary!=competitor,"forced root draw reaches missed emitter beside bright competitor");
    LightSampler::FillEmitterRecord(rig,sample.position,sample.normal,sample.ptCoord,sample.ptObjIntersec,sample.pLuminary);
    Near(area->GetMaterial()->GetEmitter()->emittedRadianceNM(rig,sample.direction,sample.normal,1200.1235),4/PI,"forced missed-patch root recovers real NM emission");
    c->release();freshLuminaries->release();bright->release();lm->release();painter->release();
}
void EnvironmentControls() {
    Fixture f;auto* painter=new SpectralPainter;auto* map=new WorldMap(*painter);map->patch=true;
    f.scene->SetGlobalRadianceMap(map);auto* c=f.Caster(Domain(1200));
    const auto* es=c->GetLightSampler()->GetEnvironmentSampler();Check(es&&es->IsValid(),"spectral patch missed by grid still has environment proposal");
    Check(!es->SpectralPower().positive,"fixture intentionally misses map patch in quadrature");
    Near(es->Pdf(Vector3(1,0,0)),1/(4*PI),"missed patch gets world sphere density");Samples random;
    double sphereIntegral=0,mx=0,my=0,mz=0,mzz=0;unsigned int cap=0;
    for(unsigned int i=0;i<10000;++i){Vector3 dir;double pdf;es->Sample(random.Get1D(),random.Get1D(),dir,pdf);Near(pdf,es->Pdf(dir),"environment sample/query agreement");sphereIntegral+=pdf*4*PI;mx+=dir.x;my+=dir.y;mz+=dir.z;mzz+=dir.z*dir.z;if(dir.z>.5)++cap;}
    Near(mx/10000,0,"world-sphere sampled x moment",.025);Near(my/10000,0,"world-sphere sampled y moment",.025);Near(mz/10000,0,"world-sphere sampled z moment",.025);
    Near(mzz/10000,1.0/3,"world-sphere sampled quadratic moment",.02);Near(double(cap)/10000,.25,"world-sphere cap probability",.02);
    Vector3 pole;double polePdf;es->Sample(0,0,pole,polePdf);Near(pole.z,1,"uniform sphere north endpoint");
    es->Sample(.5,.25,pole,polePdf);Near(pole.y,1,"uniform sphere world azimuth mapping");
    es->Sample(1-1.0/4294967296.0,.5,pole,polePdf);Check(pole.z<-.999999,"uniform sphere last Sobol endpoint reaches south cap");
    Near(sphereIntegral/10000,1,"sphere PDF integrates to unity");
    Near(map->GetRadianceNM(Ray(Point3(0,0,0),Vector3(1,0,0)),nullRasterizerState,1200),21,"actual map scale retained");
    // Three distinct PDF families: finite NEE, independent environment NEE,
    // and source-root class selection. A missed environment remains a class.
    c->release();auto* area=f.Area("area",*painter);c=f.Caster(Domain(1200));
    const auto* lights=c->GetLightSampler();es=lights->GetEnvironmentSampler();
    Near(lights->EnvSelectProbability(),.025,"missed environment class retains uniform fallback");
    Near(lights->CachedPdfSelectLuminary(*area,Point3(0,0,0),Vector3(0,0,1)),1,"finite PT NEE excludes environment root class share");
    auto* lm=new LuminaryManager;lm->AttachScene(f.scene);
    Near(lights->PdfSelectLuminary(*f.scene,lm->getLuminaries(),*area,Point3(0,0,0),Vector3(0,0,1)),.975,"root finite PMF includes complementary environment class share");
    Near(es->Pdf(Vector3(1,0,0)),1/(4*PI),"PT environment NEE excludes root class share");
    LightSample root;unsigned int environmentRoots=0;
    for(unsigned int i=0;i<5000;++i) {
        Check(lights->SampleLight(*f.scene,lm->getLuminaries(),random,root),"mixed env/area root selection");
        if(root.pEnvLight) { ++environmentRoots;Near(root.pdfSelect,.025,"returned environment root selection PMF"); }
        else Near(root.pdfSelect,.975,"returned area root selection PMF");
    }
    Check(environmentRoots>50 && environmentRoots<200,"world environment root remains reachable");lm->release();
    const unsigned int before=map->queries;map->patch=false;map->time=2;map->SetScale(5);
    map->SetTransformation(Matrix4Ops::ZRotation(PI));
    c->RefreshLightSamplers();es=c->GetLightSampler()->GetEnvironmentSampler();Check(map->queries>before,"time refresh rebuilds actual NM map estimate");
    Near(std::exp(es->SpectralPower().logarithm),20*4*PI,"NM map time/scale integral retained");
    Near(map->GetRadianceNM(Ray(Point3(0,0,0),Vector3(1,0,0)),nullRasterizerState,1200),10,"actual map orientation remains authoritative");
    c->SetRadianceScale(7);bool scaleRejected=false;
    try {c->RefreshLightSamplers();} catch(const std::invalid_argument&) {scaleRejected=true;}
    Check(scaleRejected && !c->GetLightSampler(),"caster-only scale override is refused instead of silently ignored in spectral mode");
    map->SetScale(7);c->RefreshLightSamplers();
    Near(std::exp(c->GetLightSampler()->GetEnvironmentSampler()->SpectralPower().logarithm),28*4*PI,"matching map and caster scale restores spectral preparation");
    map->SetScale(0);scaleRejected=false;
    try {c->RefreshLightSamplers();} catch(const std::invalid_argument&) {scaleRejected=true;}
    Check(scaleRejected && !c->GetLightSampler(),"zero map cannot be reconstructed from a nonzero caster-only override");
    c->SetRadianceScale(-1);c->RefreshLightSamplers();
    Check(c->GetLightSampler()->GetEnvironmentSampler()->IsValid() && !c->GetLightSampler()->GetEnvironmentSampler()->SpectralPower().positive,"cleared override restores full-support black environment");
    c->release();map->release();painter->release();
}
void FailureAndRootBoundaryControls() {
    Fixture f;f.Point("point",1100,1300,1);auto* painter=new SpectralPainter;auto* area=f.Area("area",*painter);auto* map=new WorldMap(*painter);
    f.scene->SetGlobalRadianceMap(map);auto domain=Domain(1200);auto* c=f.Caster(domain);
    for(const auto fault:{WorldMap::Fault::Negative,WorldMap::Fault::Nonfinite}) {
        map->fault=fault;bool failed=false;try {c->RefreshLightSamplers();} catch(const std::invalid_argument&) {failed=true;}
        Check(failed && !c->GetLightSampler(),"failed environment preparation invalidates caster sampler coherently");
        Check(c->GetSpectralSourceDomain()==domain.get(),"failed preparation retains requested domain for retry");
        Check(map->refcount()==2 && painter->refcount()==3,"failed map build releases all temporary sampler refs");
    }
    map->fault=WorldMap::Fault::None;c->RefreshLightSamplers();Check(c->GetLightSampler()&&c->GetLightSampler()->IsPrepared(),"environment retry publishes complete prepared sampler");
    painter->amplitude=-1;bool failed=false;try {c->RefreshLightSamplers();} catch(const std::invalid_argument&) {failed=true;}
    Check(failed&&!c->GetLightSampler(),"finite surface failure invalidates caster before stale alias can be queried");
    painter->amplitude=4;c->RefreshLightSamplers();Check(c->GetLightSampler()&&c->GetLightSampler()->IsPrepared(),"finite-source retry recovers fully prepared context");
    auto* lm=new LuminaryManager;lm->AttachScene(f.scene);auto* standalone=new LightSampler;standalone->SetSpectralSourceDomain(domain);standalone->Prepare(*f.scene,lm->getLuminaries());
    painter->amplitude=-1;failed=false;try {standalone->Prepare(*f.scene,lm->getLuminaries());} catch(const std::invalid_argument&) {failed=true;}
    Check(failed&&!standalone->IsPrepared(),"direct sampler failure clears prepared-scene state");
    Samples random;LightSample sample;Check(!standalone->SampleLight(*f.scene,lm->getLuminaries(),random,sample),"failed sampler cannot sample stale alias or environment");
    Near(standalone->CachedPdfSelectLuminary(*area,Point3(0,0,0),Vector3(0,0,1)),0,"failed sampler hit PDF is zero without stale indices");
    painter->amplitude=4;standalone->Prepare(*f.scene,lm->getLuminaries());Check(standalone->IsPrepared(),"standalone sampler retry succeeds");standalone->release();
    f.lights->RemoveItem("point");
    const auto edge=SpectralSourceDomain::Create({{1200,1}},2*SpectralSourceDomain::MinimumSourceProbability());
    // Finite estimate misses a live narrow line; dominant environment must not
    // round its complementary finite root class to zero.
    painter->lo=1200.123;painter->hi=1200.124;c->SetSpectralSourceDomain(edge);
    Check(c->GetLightSampler()->EnvSelectProbability()<1,"dominant environment leaves finite root support");
    FixedSamples finiteDraw;finiteDraw.first=1-1.0/4294967296.0;
    Check(c->GetLightSampler()->SampleLight(*f.scene,lm->getLuminaries(),finiteDraw,sample)&&sample.pLuminary==area&&sample.pdfSelect>0,"last Sobol cell reaches finite root under environment dominance");
    painter->lo=1100;painter->hi=1300;map->patch=true;c->RefreshLightSamplers();
    FixedSamples envDraw;envDraw.first=0;
    Check(c->GetLightSampler()->SampleLight(*f.scene,lm->getLuminaries(),envDraw,sample)&&sample.pEnvLight==map&&sample.pdfSelect>0,"first Sobol cell reaches environment root under finite dominance");
    c->release();lm->release();map->release();painter->release();
}
void RootConditionalGridControl() {
    Fixture f;auto* bright=f.Point("bright",1100,1300,1);auto* missed=f.Point("missed",1200.123,1200.124,4);f.Point("missed2",1200.123,1200.124,4);
    IGeometry* geometry=nullptr;IObjectPriv* bounds=nullptr;RISE_API_CreateSphereGeometry(&geometry,1);RISE_API_CreateObject(&bounds,geometry);
    bounds->FinalizeTransformations();f.objects->AddItem(bounds,"bounds");bounds->release();geometry->release();
    auto* painter=new SpectralPainter;auto* map=new WorldMap(*painter);map->time=1e200;f.scene->SetGlobalRadianceMap(map);
    const double alpha=4*SpectralSourceDomain::MinimumSourceProbability();auto* c=f.Caster(SpectralSourceDomain::Create({{1200,1}},alpha));
    // A class roll in the last Sobol cell and a separate conditional roll
    // just inside alias bin 1. The old remapping produced u_alias=.75,
    // which always selected the bright alias instead of this missed light.
    const double h=1.0/4294967296.0;
    PairSamples forced(1-h,1431655766.0*h);LuminaryManager::LuminariesList empty;LightSample root;
    Check(c->GetLightSampler()->SampleLight(*f.scene,empty,forced,root) && root.pLight==missed && root.pLight!=bright,
        "independent conditional Sobol draw reaches missed source under tiny finite root class");
    Check(root.pdfSelect>0,"joint root PMF stays positive with independent class/conditional draws");
    c->release();map->release();painter->release();
}
void OneBounceEnvironmentOracle() {
    Fixture f;auto* painter=new SpectralPainter;auto* map=new WorldMap(*painter);
    f.scene->SetGlobalRadianceMap(map);auto* c=f.Caster(SpectralSourceDomain::Create({{1100,1},{1200,1},{1300,1}}));
    IMaterial* material=nullptr;RISE_API_CreateLambertianMaterial(&material,*painter);
    auto ri=Receiver();Samples random;double nee=0,combined=0,continuation=0;
    const unsigned int n=60000;
    for(unsigned int i=0;i<n;++i) {
        const double nm=1100+100*(i%3);
        // Actual NM NEE consumer; no BSDF partner for the NEE-only control.
        nee+=c->GetLightSampler()->EvaluateDirectLightingNM(ri,*material->GetBSDF(),nullptr,nm,*c,random,nullptr,nullptr,false,nullptr);
        combined+=c->GetLightSampler()->EvaluateDirectLightingNM(ri,*material->GetBSDF(),material,nm,*c,random,nullptr,nullptr,false,nullptr);
        const double u=random.Get1D(),phi=TWO_PI*random.Get1D(),z=sqrt(1-u),r=sqrt(u);
        const Vector3 direction(r*cos(phi),r*sin(phi),z);
        const double source=map->GetRadianceNM(Ray(ri.ptIntersection,direction),nullRasterizerState,nm);
        const double value=.8*source;
        continuation+=value;
        const double pb=z/PI,pe=c->GetLightSampler()->GetEnvironmentSampler()->Pdf(direction);
        combined+=value*(pb*pb)/(pb*pb+pe*pe);
    }
    // The x-varying term integrates to zero over the +z hemisphere.
    const double expected=.8*2*3;
    Near(nee/n,expected,"one-bounce NM NEE matches analytic Lambertian world map",.012);
    Near(continuation/n,expected,"independent cosine continuation matches analytic oracle",.005);
    Near(combined/n,expected,"NM environment NEE/escape-PDF MIS partition matches oracle",.008);
    c->release();material->release();map->release();painter->release();
}
}
int main(){DomainControls();DeltaAndContextControls();UVAndMagnitudeControls();AreaControls();EnvironmentControls();FailureAndRootBoundaryControls();RootConditionalGridControl();OneBounceEnvironmentOracle();std::printf("SpectralSourceDomainTest: %d failures\n",failures);return failures?1:0;}
