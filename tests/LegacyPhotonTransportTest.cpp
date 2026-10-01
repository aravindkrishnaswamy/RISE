// Exact fixed-deposit and live-consumer oracles for DL239 / DL271 / DL272.
// No rasterizer: synthetic intersections isolate the real transport consumers.
#include <cstdio>
#include <cmath>
#include "../src/Library/PhotonMapping/GlobalPelPhotonTracer.h"
#include "../src/Library/PhotonMapping/GlobalSpectralPhotonTracer.h"
#include "../src/Library/PhotonMapping/CausticPelPhotonMap.h"
#include "../src/Library/PhotonMapping/CausticPelPhotonTracer.h"
#include "../src/Library/PhotonMapping/CausticSpectralPhotonTracer.h"
#include "../src/Library/PhotonMapping/TranslucentPelPhotonTracer.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Materials/IsotropicPhongMaterial.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/SpectralColorPainter.h"
#include "../src/Library/DetectorSpheres/CircularDiskDetector.h"
#include "../src/Library/DetectorSpheres/DetectorSphere.h"
#include "../src/Library/DetectorSpheres/AdaptiveDetectorSphere.h"
#include "../src/Library/Materials/LambertianBRDF.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/DetectorSpheres/IsotropicRGBDetectorSphere.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Scene.h"
#include "TestStubObject.h"
#include "../src/Library/Shaders/PathTracingIntegrator.h"
#include "../src/Library/Shaders/BDPTIntegrator.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Lights/PointLight.h"
#include "../src/Library/Managers/LightManager.h"
#include "../src/Library/Materials/LambertianLuminaireMaterial.h"
#include "../src/Library/Shaders/FinalGatherShaderOp.h"
#include "../src/Library/Shaders/DistributionTracingShaderOp.h"
#ifdef RISE_TEST_SMS_PRIVATE_PROBE
// Isolated actual-source probe; its link omits SMSPhotonMap.o.
#include "../src/Library/Utilities/SMSPhotonMap.cpp"
#endif
// Circular/adaptive implementations are Xcode/VS sidecars, absent from
// make/Android. The isolated sidecar probe explicitly compiles their real
// sources; the ordinary named target keeps the production inventory intact.
#ifdef RISE_TEST_LEGACY_DETECTOR_SIDECARS
#include "../src/Library/DetectorSpheres/CircularDiskDetector.cpp"
#include "../src/Library/DetectorSpheres/AdaptiveDetectorSphere.cpp"
#endif
using namespace RISE;
using namespace RISE::Implementation;
namespace {
int checks=0,failures=0;
void Check(bool ok,const char* label,double actual,double expected) {
    ++checks; if(!ok) ++failures;
    std::printf("%s %s actual=%.17g expected=%.17g\n",ok?"PASS":"FAIL",label,actual,expected);
}
void Near(double a,double b,const char* label) { Check(std::isfinite(a)&&std::fabs(a-b)<1e-10,label,a,b); }
// One response of1 versus two responses whose sum is1. Both physical
// directions coincide so K_i/q_i must be1 on EVERY selection, not merely
// in a stochastic mean. NM leaves RGB fields zero, as real NM SPFs do.
class SplitMaterial : public IMaterial, public ISPF, public Reference {
    int count;
    IBSDF* response;
    bool sink;
    ScatteredRay::ScatRayType rayType;
    void Emit(const RayIntersectionGeometric& ri,ScatteredRayContainer& out,bool nm) const {
        for(int i=0;i<count;++i) {
            ScatteredRay r;
            r.type=rayType;
            r.ray=Ray(ri.ptIntersection,Vector3(.6,0,.8));
            const double w=sink?0:count==1?1:nm?(i==0?.3:.7):(i==0?.2:.8);
            if(nm) r.krayNM=w; else r.kray=RISEPel(w);
            out.AddScatteredRay(r);
        }
    }
public:
    SplitMaterial(int n,bool stop=false,ScatteredRay::ScatRayType type=ScatteredRay::eRayDiffuse,IBSDF* bsdf=nullptr):count(n),response(bsdf),sink(stop),rayType(type) {}
    IBSDF* GetBSDF() const override { return response; }
    ISPF* GetSPF() const override { return const_cast<SplitMaterial*>(this); }
    IEmitter* GetEmitter() const override { return nullptr; }
    void Scatter(const RayIntersectionGeometric& ri,ISampler&,ScatteredRayContainer& out,const IORStack&) const override { Emit(ri,out,false); }
    void ScatterNM(const RayIntersectionGeometric& ri,ISampler&,Scalar,ScatteredRayContainer& out,const IORStack&) const override { Emit(ri,out,true); }
};
class TwoPlaneManager : public ObjectManager {
    const IObject& object;
    const IMaterial& split;
    const IMaterial& sink;
    double tilt;
public:
    TwoPlaneManager(const IObject& o,const IMaterial& a,const IMaterial& b,double degrees=0):ObjectManager(false,false,4,8),object(o),split(a),sink(b),tilt(degrees*PI/180) {}
    void IntersectRay(RayIntersection& ri,bool,bool,bool) const override {
        const bool first=ri.geometric.ray.Dir().z<0;
        ri.geometric.bHit=true;ri.geometric.range=1;
        ri.geometric.ptIntersection=Point3(0,0,first?0:1);
        ri.geometric.vNormal=Vector3(0,0,first?1:-1);
        ri.geometric.vGeomNormal=ri.geometric.vNormal;
        if(first)ri.geometric.vNormal=Vector3(std::sin(tilt),0,std::cos(tilt));
        ri.geometric.onb.CreateFromW(ri.geometric.vNormal);
        ri.pMaterial=first?&split:&sink;ri.pObject=&object;
    }
};
class PelTracer : public GlobalPelPhotonTracer {
public:
    PelTracer(bool branch):PhotonTracer<GlobalPelPhotonMap>(true,1,1,false),GlobalPelPhotonTracer(2,1e-12,branch,true,1,1,false) {}
    void Run(GlobalPelPhotonMap& map) { const IORStack stack(1);TracePhoton(Ray(Point3(0,0,1),Vector3(0,0,-1)),RISEPel(1),map,false,stack,0); }
};
class NMTracer : public GlobalSpectralPhotonTracer {
public:
    NMTracer(bool branch):SpectralPhotonTracer<GlobalSpectralPhotonMap>(400,700,160,1,1,false),GlobalSpectralPhotonTracer(2,1e-12,400,700,160,branch,1,1,false) {}
    void Run(GlobalSpectralPhotonMap& map) { const IORStack stack(1);TracePhoton(Ray(Point3(0,0,1),Vector3(0,0,-1)),1,550,false,map,stack,0); }
};
class PelMap : public GlobalPelPhotonMap {
public:
    PelMap(unsigned n=128):GlobalPelPhotonMap(n,nullptr) {}
    double Sum() const { double total=0;for(const auto& p:vphotons)total+=p.power.r;return total; }
};
class NMMap : public GlobalSpectralPhotonMap {
public:
    NMMap(unsigned n=128):GlobalSpectralPhotonMap(n,nullptr) {ConfigureWavelengthSampling(400,700,160);}
    double Sum() const { double total=0;for(const auto& p:vphotons)total+=p.power;return total; }
};
void TestLiveSelection(double degrees=0) {
    const double angle=degrees*PI/180;const double expected=std::cos(angle)*.8/(.6*std::sin(angle)+.8*std::cos(angle));
    std::printf("LIVE_GLOBAL tilt=%.17g expectedAdjoint=%.17g\n",degrees,expected);
    auto* split=new SplitMaterial(2);auto* sink=new SplitMaterial(1,true);auto* object=new StubObject();
    auto* manager=new TwoPlaneManager(*object,*split,*sink,degrees);auto* scene=new Scene();scene->SetObjectManager(manager);
    for(bool branch:{false,true}) {
        auto* tracer=new PelTracer(branch);tracer->AttachScene(scene);PelMap map;
        for(int i=0;i<16;++i)tracer->Run(map);
        Near(map.Sum()/16,expected,branch?"DL271 live Pel branch sum":"DL271 live Pel selected response");tracer->release();
        auto* spectral=new NMTracer(branch);spectral->AttachScene(scene);NMMap nm;
        for(int i=0;i<16;++i)spectral->Run(nm);
        Near(nm.Sum()/16,expected,branch?"DL271 live NM branch sum":"DL271 live NM selected response");spectral->release();
    }
    scene->release();manager->release();object->release();sink->release();split->release();
}
class CausticPelTracer : public CausticPelPhotonTracer {
public:
    CausticPelTracer(bool branch):PhotonTracer<CausticPelPhotonMap>(true,1,1,false),CausticPelPhotonTracer(2,1e-12,branch,true,true,true,1,1,false) {}
    void Run(CausticPelPhotonMap& map) { const IORStack stack(1);TracePhoton(Ray(Point3(0,0,1),Vector3(0,0,-1)),RISEPel(1),false,map,stack,0); }
};
class CausticNMTracer : public CausticSpectralPhotonTracer {
public:
    CausticNMTracer(bool branch):SpectralPhotonTracer<CausticSpectralPhotonMap>(400,700,160,1,1,false),CausticSpectralPhotonTracer(2,1e-12,400,700,160,branch,true,true,1,1,false) {}
    void Run(CausticSpectralPhotonMap& map) { const IORStack stack(1);TracePhoton(Ray(Point3(0,0,1),Vector3(0,0,-1)),1,550,false,map,stack,0); }
};
class CausticPelMap : public CausticPelPhotonMap {
public: CausticPelMap(unsigned n=128):CausticPelPhotonMap(n,nullptr) {}
    double Sum() const {double s=0;for(const auto& p:vphotons)s+=p.power.r;return s;}
};
class CausticNMMap : public CausticSpectralPhotonMap {
public: CausticNMMap(unsigned n=128):CausticSpectralPhotonMap(n,nullptr) {ConfigureWavelengthSampling(400,700,160);}
    double Sum() const {double s=0;for(const auto& p:vphotons)s+=p.power;return s;}
};
void TestCausticSelection(double degrees=0) {
    const double angle=degrees*PI/180;const double expected=std::cos(angle)*.8/(.6*std::sin(angle)+.8*std::cos(angle));
    std::printf("LIVE_CAUSTIC tilt=%.17g expectedAdjoint=%.17g\n",degrees,expected);
    auto* split=new SplitMaterial(2,false,ScatteredRay::eRayReflection);auto* paint=new UniformColorPainter(RISEPel(.5));auto* sink=new LambertianMaterial(*paint);auto* object=new StubObject();
    auto* manager=new TwoPlaneManager(*object,*split,*sink,degrees);auto* scene=new Scene();scene->SetObjectManager(manager);
    for(bool branch:{false,true}) {
        auto* tracer=new CausticPelTracer(branch);tracer->AttachScene(scene);CausticPelMap map;
        for(int i=0;i<16;++i)tracer->Run(map);
        Near(map.Sum()/16,expected,branch?"DL271 caustic Pel branch sum":"DL271 caustic Pel filtered selected response");tracer->release();
        auto* spectral=new CausticNMTracer(branch);spectral->AttachScene(scene);CausticNMMap nm;
        for(int i=0;i<16;++i)spectral->Run(nm);
        Near(nm.Sum()/16,expected,branch?"DL271 caustic NM branch sum":"DL271 caustic NM filtered selected response");spectral->release();
    }
    scene->release();manager->release();object->release();sink->release();paint->release();split->release();
}
double OtherDetectorSum(int count,int kind) {
    SplitMaterial material(count);PointSample emitter(Point3(0,0,1)),specimen(Point3(0,0,0));double sum=0;
#ifdef RISE_TEST_LEGACY_DETECTOR_SIDECARS
    if(kind==0) {
        auto* d=new CircularDiskDetector();d->InitPatches(16,1,.4);
        d->PerformMeasurement(emitter,specimen,1,material,16,1,nullptr,1);
        for(unsigned i=0;i<d->numPatches();++i)sum+=d->getPatches()[i].dRatio*d->getPatches()[i].dSolidProjectedAngle;
        d->release();
    } else
#endif
    if(kind==1) {
        auto* d=new DetectorSphere();d->InitPatches(8,8,1,DetectorSphere::eEqualAngles);
        d->PerformMeasurement(emitter,specimen,1,material,16,1,nullptr,1);
        for(unsigned i=0;i<d->numPatches()/2;++i)sum+=d->getTopPatches()[i].dRatio*d->getTopPatches()[i].dSolidProjectedAngle+d->getBottomPatches()[i].dRatio*d->getBottomPatches()[i].dSolidProjectedAngle;
        d->release();
    }
#ifdef RISE_TEST_LEGACY_DETECTOR_SIDECARS
    else {
        auto* d=new AdaptiveDetectorSphere();d->InitPatches(32,1,.1);
        d->PerformMeasurement(emitter,specimen,1,material,16,1,nullptr,1);
        for(const auto& patch:d->getTopPatches())sum+=patch.dRatio*patch.dSolidProjectedAngle;
        d->release();
    }
#endif
    return sum;
}
void TestOtherDetectors() {
    const char* names[]={"DL271 circular detector","DL271 sphere detector","DL271 adaptive detector"};
#ifdef RISE_TEST_LEGACY_DETECTOR_SIDECARS
    const int kinds[]={0,1,2};
#else
    const int kinds[]={1};
#endif
    for(int kind:kinds){const double one=OtherDetectorSum(1,kind),two=OtherDetectorSum(2,kind);Check(one>0,"other detector positive control",one,1);Near(two/one,1,names[kind]);}
}
double DetectorSum(int count,bool spectral) {
    SplitMaterial material(count);
    IsotropicRGBDetectorSphere detector;detector.InitPatches(8,IsotropicRGBDetectorSphere::eEqualAngles);
    detector.PerformMeasurement(0,1,material,16,1,spectral,550,550,nullptr,1);
    double sum=0;
    for(unsigned i=0;i<detector.numPatches()/2;++i) {
        const auto& top=detector.getTopPatches()[i];const auto& bottom=detector.getBottomPatches()[i];
        sum+=top.dRatio[1]*top.dSolidProjectedAngle+bottom.dRatio[1]*bottom.dSolidProjectedAngle;
    }
    return sum;
}
void TestDetector() {
    for(bool nm:{false,true}) {
        const double single=DetectorSum(1,nm),split=DetectorSum(2,nm);
        Check(single>0,"detector positive single-ray control",single,1);
        Near(split/single,1,nm?"DL272 live spectral selection mode and compensation":"DL271 live RGB detector compensation");
    }
}
void TestRealSpectralDetector(){
    auto* diffuse=new UniformColorPainter(RISEPel(.7,.3,.2));
    Scalar zero=0;SpectralPacket zeroSpectrum(400,700,31,zero);
    auto* black=new SpectralColorPainter(zeroSpectrum,1.);auto* exponent=new UniformScalarPainter(12.);
    auto* lambert=new LambertianMaterial(*diffuse);auto* phong=new IsotropicPhongMaterial(*diffuse,*black,*exponent);
    double measured[2];int mode=0;
    for(const IMaterial* material:{static_cast<IMaterial*>(lambert),static_cast<IMaterial*>(phong)}){
        IsotropicRGBDetectorSphere detector;detector.InitPatches(8,IsotropicRGBDetectorSphere::eEqualAngles);
        detector.PerformMeasurement(0,1,*material,256,1,true,550,550,nullptr,1);
        double sum=0;for(unsigned i=0;i<detector.numPatches()/2;++i){const auto& a=detector.getTopPatches()[i];const auto& b=detector.getBottomPatches()[i];sum+=a.dRatio[1]*a.dSolidProjectedAngle+b.dRatio[1]*b.dSolidProjectedAngle;}
        measured[mode++]=sum;
    }
    // At normal incidence black Rs gives a zero-response specular record,
    // which Phong still stores. Its diffuse energy must equal Lambertian's;
    // using RGB selection after ScatterNM drops the two-record population.
    Check(measured[0]>0,"real Lambertian spectral detector positive control",measured[0],1);
    Near(measured[1]/measured[0],1,"DL272 real Phong spectral detector uses NM selection domain");
    phong->release();lambert->release();exponent->release();black->release();diffuse->release();
}

	class ConstantCaster :
		public virtual IRayCaster,
		public virtual Reference
	{
	const IScene* scene;
	public:
		ConstantCaster(const IScene* s):scene(s) {}
		virtual ~ConstantCaster() {}

	public:
		bool CastRay( const RuntimeContext&, const RasterizerState&, const Ray&, RISEPel&,
			const RAY_STATE&, Scalar*, const IRadianceMap* ) const override { return false; }

		bool CastRayNM( const RuntimeContext&, const RasterizerState&, const Ray&, Scalar&,
			const RAY_STATE&, const Scalar, Scalar*, const IRadianceMap* ) const override { return false; }

		bool CastRay( const RuntimeContext&, const RasterizerState&, const Ray&, RISEPel& c,
			const RAY_STATE&, Scalar* distance, const IRadianceMap*, const IORStack& ) const override { c=RISEPel(1);if(distance)*distance=1;return true; }

		bool CastRayNM( const RuntimeContext&, const RasterizerState&, const Ray&, Scalar& c,
			const RAY_STATE&, const Scalar, Scalar* distance, const IRadianceMap*, const IORStack& ) const override { c=1;if(distance)*distance=1;return true; }

		bool CastShadowRay( const Ray&, const Scalar ) const override
		{
			// Unoccluded -- see class comment.
			return false;
		}

		bool CastOcclusionRay( const Ray&, const Scalar ) const override
		{
			// Not exercised by this test (AreaLightShaderOp only calls
			// CastShadowRay); stub to satisfy the pure-virtual interface.
			return false;
		}

		void AttachScene( const IScene* ) override {}
		const IScene* GetAttachedScene() const override { return scene; }
		void SetLuminaireSampling( ISampling2D* ) override {}
		const ILuminaryManager* GetLuminaries() const override { return nullptr; }
		const Implementation::LightSampler* GetLightSampler() const override { return nullptr; }
		void SetRISCandidates( const unsigned int ) override {}
		void SetLightSampleRRThreshold( const Scalar ) override {}
		void SetUseLightBVH( const bool ) override {}
		bool IsRadianceMapVisibleAsBackground() const override { return false; }
	};

void TestShaderSelection() {
    auto* scene=new Scene();auto* map=new GlobalPelPhotonMap(1,nullptr);scene->SetGlobalPelMap(map);
    auto* paint=new UniformColorPainter(RISEPel(.5));auto* bsdf=new LambertianBRDF(*paint);
    ConstantCaster caster(scene);RandomNumberGenerator rng;RuntimeContext rc(rng,RuntimeContext::PASS_NORMAL,false);
    IRayCaster::RAY_STATE rs;IORStack stack(1);
    RayIntersection ri(Ray(Point3(0,0,1),Vector3(0,0,-1)),nullRasterizerState);
    ri.geometric.vNormal=ri.geometric.vGeomNormal=Vector3(0,0,1);ri.geometric.onb.CreateFromW(ri.geometric.vNormal);
    for(int n:{1,2}) {
        SplitMaterial material(n,false,ScatteredRay::eRayDiffuse,bsdf);ri.pMaterial=&material;
        auto* gather=new FinalGatherShaderOp(4,4,false,1,1,false);RISEPel result;
        gather->PerformOperation(rc,ri,caster,rs,result,stack,nullptr);
        Near(result.r,1,n==1?"FinalGather single response control":"DL271 actual FinalGather filtered response");gather->release();
        auto* distribution=new DistributionTracingShaderOp(1,false,false,true,true,true,true);
        distribution->PerformOperation(rc,ri,caster,rs,result,stack,nullptr);
        Near(result.r,1,n==1?"DistributionTracing single selector control":"DistributionTracing branch-all negative control");
        const Scalar nmResult=distribution->PerformOperationNM(rc,ri,caster,rs,0,550,stack,nullptr);
        Near(nmResult,1,n==1?"DistributionTracing NM single selector control":"DistributionTracing NM branch-all negative control");distribution->release();
    }
    auto* realMaterial=new LambertianMaterial(*paint);ri.pMaterial=realMaterial;
    auto* realDistribution=new DistributionTracingShaderOp(1,false,false,true,true,true,true);
    for(double wavelength:{450.,650.}){
        const Scalar got=realDistribution->PerformOperationNM(rc,ri,caster,rs,0,wavelength,stack,nullptr);
        Near(got,paint->GetColorNM(ri.geometric,wavelength),"DL272 actual Lambertian spectral distribution response");
    }
    SampledWavelengths swl=SampledWavelengths::SampleEquidistant(.5,400,700);
    Scalar accumulated[SampledWavelengths::N]={},bundle[SampledWavelengths::N];
    realDistribution->PerformOperationHWSS(rc,ri,caster,rs,accumulated,swl,stack,nullptr,bundle);
    for(unsigned i=0;i<SampledWavelengths::N;++i)Near(bundle[i],paint->GetColorNM(ri.geometric,swl.lambda[i]),"DL272 actual Lambertian HWSS distribution fallback");
    realDistribution->release();realMaterial->release();
    scene->release();map->release();bsdf->release();paint->release();
}
#ifdef RISE_TEST_SMS_PRIVATE_PROBE
void TestSMSSelection(double degrees=0) {
    const double angle=degrees*PI/180;const double expected=std::cos(angle)*.8/(.6*std::sin(angle)+.8*std::cos(angle));
    std::printf("LIVE_SMS tilt=%.17g expectedAdjoint=%.17g\n",degrees,expected);
    auto* split=new SplitMaterial(2,false,ScatteredRay::eRayReflection);auto* paint=new UniformColorPainter(RISEPel(.5));auto* sink=new LambertianMaterial(*paint);auto* object=new StubObject();
    auto* manager=new TwoPlaneManager(*object,*split,*sink,degrees);auto* scene=new Scene();scene->SetObjectManager(manager);
    RandomNumberGenerator rng;IORStack stack(1);double sum=0;bool structure=true;
    for(int i=0;i<16;++i){SMSPhoton out;bool hit=TraceSMSPhoton(*scene,Ray(Point3(0,0,1),Vector3(0,0,-1)),RISEPel(1),rng,stack,out);structure=structure&&hit&&out.chainLen==1&&out.entryObject==object&&out.chain[0].flags==2;if(hit)sum+=out.power.r;}
    Check(structure,"SMS actual walk keeps reflection chain and hit",structure,1);Near(sum/16,expected,"DL271 SMS actual walk selected flux");
    scene->release();manager->release();object->release();sink->release();paint->release();split->release();
}
#endif


class ZeroSampler : public ISampler {
public: Scalar Get1D() override {return 0;} Point2 Get2D() override {return Point2(0,0);}
};
class LightPrefixSampler : public ISampler {
    unsigned n=0;
public: Scalar Get1D() override {return n++<4?.75:0;} Point2 Get2D() override {return Point2(0,0);}
};
class RareMaterial : public SplitMaterial {
    double first,second;bool delta;
    void EmitRare(const RayIntersectionGeometric& ri,ScatteredRayContainer& out,bool nm) const {
        for(int i=0;i<2;++i){ScatteredRay r;r.type=delta?ScatteredRay::eRayReflection:ScatteredRay::eRayDiffuse;r.isDelta=delta;r.pdf=1;r.ray=Ray(ri.ptIntersection,Vector3(i==0?.6:-.6,0,.8));if(nm)r.krayNM=i==0?first:second;else r.kray=RISEPel(i==0?first:second);out.AddScatteredRay(r);}
    }
public:
    RareMaterial(double a,double b,IBSDF* response):SplitMaterial(2,false,ScatteredRay::eRayDiffuse,response),first(a),second(b),delta(response==nullptr){}
    void Scatter(const RayIntersectionGeometric& ri,ISampler&,ScatteredRayContainer& out,const IORStack&) const override {EmitRare(ri,out,false);}
    void ScatterNM(const RayIntersectionGeometric& ri,ISampler&,Scalar,ScatteredRayContainer& out,const IORStack&) const override {EmitRare(ri,out,true);}
    Scalar EvaluateKrayNM(const RayIntersectionGeometric&,const Vector3& dir,ScatteredRay::ScatRayType,Scalar,const IORStack&) const override {return dir.x>0?first:second;}
};
void TestRareIntegratorSelection() {
    auto* object=new StubObject();auto* paint=new UniformColorPainter(RISEPel(1));auto* bsdf=new LambertianBRDF(*paint);
    auto* nullMat=new SplitMaterial(0);auto* emitter=new LambertianLuminaireMaterial(*paint,1,*nullMat);
    RandomNumberGenerator rng;RuntimeContext rc(rng,RuntimeContext::PASS_NORMAL,false);ZeroSampler sampler;IORStack stack(1);
    auto* pt=new PathTracingIntegrator(ManifoldSolverConfig(),StabilityConfig());pt->SetMaxPathDepth(2);
    auto* bdpt=new BDPTIntegrator(2,2,StabilityConfig());
    const Ray ray(Point3(0,0,1),Vector3(0,0,-1));
    for(bool hasBSDF:{false,true})for(int extreme:{0,1,2}) {
        // Control, tiny selected weight, and selected weight above NEARZERO with
        // tiny probability (total100 stays below PT's independent 1e6 cap). xi=0 selects the first nonzero interval exactly.
        double a=extreme==1?1e-14:extreme==2?1e-11:1,b=extreme==2?100:1;
        bdpt->SetLightSampler(nullptr);
        RareMaterial material(a,b,hasBSDF?bsdf:nullptr);
        auto* manager=new TwoPlaneManager(*object,material,*emitter);auto* scene=new Scene();scene->SetObjectManager(manager);ConstantCaster caster(scene);
        RayIntersection hit(ray,nullRasterizerState);manager->IntersectRay(hit,true,true,false);
        RISEPel pel=pt->IntegrateFromHit(rc,nullRasterizerState,hit,*scene,caster,sampler,nullptr,0,stack,0,RISEPel(1),true,1,IRayCaster::RAY_STATE::eRayView,0,0,0,0,0,0,false,false);
        Scalar nm=pt->IntegrateFromHitNM(rc,nullRasterizerState,hit,550,*scene,caster,sampler,nullptr,0,stack,0,1,true,1,IRayCaster::RAY_STATE::eRayView,0,0,0,0,0,0,false,false);
        const Scalar emittedNM=emitter->GetEmitter()->emittedRadianceNM(hit.geometric,Vector3(0,0,1),Vector3(0,0,1),550);
        std::printf("rare hasBSDF=%d first=%.17g second=%.17g NMsource=%.17g\n",hasBSDF,a,b,emittedNM);
        Near(pel.r/((a+b)*INV_PI),1,"DL271 actual PT Pel selected conditional response");Near(nm/((a+b)*emittedNM),1,"DL271 actual PT NM selected conditional response");
        SampledWavelengths swl=SampledWavelengths::SampleEquidistant(.5,400,700);Scalar bundle[SampledWavelengths::N];
        pt->IntegrateFromHitHWSS(rc,nullRasterizerState,hit,swl,*scene,caster,sampler,nullptr,0,stack,0,true,1,IRayCaster::RAY_STATE::eRayView,0,0,0,0,0,0,bundle);
        for(unsigned w=0;w<SampledWavelengths::N;++w){const Scalar e=emitter->GetEmitter()->emittedRadianceNM(hit.geometric,Vector3(0,0,1),Vector3(0,0,1),swl.lambda[w]);Near(bundle[w]/((a+b)*e),1,"DL271 actual PT HWSS selected conditional response");}
        std::vector<BDPTVertex> verts;std::vector<uint32_t> starts;
        bdpt->GenerateEyeSubpath(rc,ray,Point2(0,0),*scene,caster,sampler,verts,starts);
        Check(verts.size()>=3,"BDPT eye reaches receiver",verts.size(),3);if(verts.size()>=3)Near(verts[2].throughput.r/(a+b),1,"DL271 actual BDPT Pel selected conditional response");
        bdpt->GenerateEyeSubpathNM(rc,ray,Point2(0,0),*scene,caster,sampler,verts,starts,550,nullptr);
        Check(verts.size()>=3,"BDPT NM eye reaches receiver",verts.size(),3);if(verts.size()>=3)Near(verts[2].throughputNM/(a+b),1,"DL271 actual BDPT NM selected conditional response");
        auto* light=new PointLight(1,RISEPel(1),true);light->SetPosition(Point3(0,0,1));light->FinalizeTransformations();
        auto* lights=new LightManager();lights->AddItem(light,"conditional-point");scene->SetLightManager(lights);
        auto* lightSampler=new LightSampler();LuminaryManager::LuminariesList luminaries;lightSampler->Prepare(*scene,luminaries);bdpt->SetLightSampler(lightSampler);
        LightPrefixSampler lightSamples;
        bdpt->GenerateLightSubpath(*scene,caster,lightSamples,verts,starts,rng);
        Check(verts.size()>=3,"BDPT light reaches receiver",verts.size(),3);if(verts.size()>=3)Near(verts[2].throughput.r/(verts[1].throughput.r*(a+b)),1,"DL271 actual BDPT Pel light selected conditional response");
        LightPrefixSampler nmLightSamples;
        bdpt->GenerateLightSubpathNM(*scene,caster,nmLightSamples,verts,starts,550,rng,nullptr);
        Check(verts.size()>=3,"BDPT NM light reaches receiver",verts.size(),3);if(verts.size()>=3)Near(verts[2].throughputNM/(verts[1].throughputNM*(a+b)),1,"DL271 actual BDPT NM light selected conditional response");
        bdpt->SetLightSampler(nullptr);lightSampler->release();lights->release();light->release();
        scene->release();manager->release();
    }
    bdpt->release();pt->release();emitter->release();nullMat->release();bsdf->release();paint->release();object->release();
}


class TranslucentTracerProbe:public TranslucentPelPhotonTracer {
public:
 TranslucentTracerProbe():PhotonTracer<TranslucentPelPhotonMap>(true,1,1,false),TranslucentPelPhotonTracer(2,1e-12,true,true,true,true,1,1,false){}
 void Run(TranslucentPelPhotonMap& map){IORStack stack(1);TracePhoton(Ray(Point3(0,0,1),Vector3(0,0,-1)),RISEPel(1),false,map,stack,0);}
};
class TranslucentMapProbe:public TranslucentPelPhotonMap {
public:TranslucentMapProbe():TranslucentPelPhotonMap(128,nullptr){} double Sum()const{double s=0;for(const auto& p:vphotons)s+=p.power.r;return s;}
};
void TestTranslucentContinuation(){
 auto* split=new SplitMaterial(2,false,ScatteredRay::eRayTranslucent);auto* paint=new UniformColorPainter(RISEPel(.5));auto* sink=new LambertianMaterial(*paint);auto* object=new StubObject();
 for(double degrees:{0.,-30.,30.}){const double angle=degrees*PI/180;const double expected=std::cos(angle)*.8/(.6*std::sin(angle)+.8*std::cos(angle));auto* manager=new TwoPlaneManager(*object,*split,*sink,degrees);auto* scene=new Scene();scene->SetObjectManager(manager);auto* tracer=new TranslucentTracerProbe();tracer->AttachScene(scene);TranslucentMapProbe map;tracer->Run(map);std::printf("LIVE_TRANSLUCENT tilt=%.17g expectedAdjoint=%.17g\n",degrees,expected);Near(map.Sum(),expected,"DL239 translucent tracer branch continuation flux");tracer->release();scene->release();manager->release();}
 object->release();sink->release();paint->release();split->release();
}

void TestGather() {
    auto* map=new CausticPelPhotonMap(4096,nullptr);
    auto* paint=new UniformColorPainter(RISEPel(.8));auto* bsdf=new LambertianBRDF(*paint);
    for(int y=-25;y<=25;++y)for(int x=-25;x<=25;++x)map->Store(RISEPel(1),Point3(x*.01,y*.01,0),Vector3(0,0,1));
    map->Balance();map->SetGatherParams(.2,.05,10,400,nullptr);
    RayIntersectionGeometric ri(Ray(Point3(0,0,1),Vector3(0,0,-1)),nullRasterizerState);
    ri.ptIntersection=Point3(0,0,0);ri.vGeomNormal=Vector3(0,0,1);ri.vNormal=ri.vGeomNormal;ri.onb.CreateFromW(ri.vNormal);
    RISEPel flat;map->RadianceEstimate(flat,ri,*bsdf);Check(flat.r>0,"DL239 positive flat gather",flat.r,0);
    for(double degrees:{0.,10.,20.,45.}) {
        const double angle=degrees*PI/180;
        ri.vNormal=Vector3(std::sin(angle),0,std::cos(angle));ri.onb.CreateFromW(ri.vNormal);
        RISEPel value;map->RadianceEstimate(value,ri,*bsdf);
        std::printf("gather tilt=%.1f flat=%.17g tilted=%.17g\n",degrees,flat.r,value.r);
        Near(value.r/flat.r,std::cos(angle),"DL239 fixed geometric-flux projected-area ratio");
    }
    map->release();bsdf->release();paint->release();
}
}

// DL-214: a physical planar hit is offered once, so rejecting coverage
// escapes rather than repeatedly offering the same synthetic intersection.
class AlphaPlaneManager : public ObjectManager {
    const IObject& object;const IMaterial& first;const IMaterial& second;
public:
    AlphaPlaneManager(const IObject& o,const IMaterial& a,const IMaterial& b):ObjectManager(false,false,4,8),object(o),first(a),second(b){}
    void IntersectRay(RayIntersection& ri,bool,bool,bool) const override {
        const bool front=ri.geometric.ray.Dir().z<0;
        const Scalar t=((front?0.:1.)-ri.geometric.ray.origin.z)/ri.geometric.ray.Dir().z;
        ri.geometric.bHit=t>1e-8;if(!ri.geometric.bHit)return;
        ri.geometric.range=t;ri.geometric.ptIntersection=ri.geometric.ray.PointAtLength(t);
        ri.geometric.vNormal=Vector3(0,0,front?1:-1);ri.geometric.vGeomNormal=ri.geometric.vNormal;
        ri.geometric.onb.CreateFromW(ri.geometric.vNormal);ri.pMaterial=front?&first:&second;ri.pObject=&object;
    }
};
void TestAlphaPhotonDeposits() {
    const int N=20000;
    auto* coverage=new UniformScalarPainter(.5);auto* object=new StubObject();
    for(bool caustic:{false,true}){
        auto* split=new SplitMaterial(1,false,caustic?ScatteredRay::eRayReflection:ScatteredRay::eRayDiffuse);
        auto* paint=new UniformColorPainter(RISEPel(.5));IMaterial* sink=caustic?static_cast<IMaterial*>(new LambertianMaterial(*paint)):static_cast<IMaterial*>(new SplitMaterial(1,true));sink->SetAlpha(coverage,eAlphaBlend,.5);
        auto* manager=new AlphaPlaneManager(*object,*split,*sink);auto* scene=new Scene();scene->SetObjectManager(manager);
        if(caustic){
            CausticPelMap map(N*2);auto* tracer=new CausticPelTracer(false);tracer->AttachScene(scene);for(int i=0;i<N;++i)tracer->Run(map);
            Check(std::fabs(map.Sum()/N-1)<.03,"DL214 caustic Pel deposit is conditional incident flux",map.Sum()/N,1);tracer->release();
            CausticNMMap nm(N*2);auto* spectral=new CausticNMTracer(false);spectral->AttachScene(scene);for(int i=0;i<N;++i)spectral->Run(nm);
            Check(std::fabs(nm.Sum()/N-1)<.03,"DL214 caustic NM deposit is conditional incident flux",nm.Sum()/N,1);spectral->release();
        }else{
            PelMap map(N*2);auto* tracer=new PelTracer(false);tracer->AttachScene(scene);for(int i=0;i<N;++i)tracer->Run(map);
            Check(std::fabs(map.Sum()/N-1)<.03,"DL214 global Pel deposit is conditional incident flux",map.Sum()/N,1);tracer->release();
            NMMap nm(N*2);auto* spectral=new NMTracer(false);spectral->AttachScene(scene);for(int i=0;i<N;++i)spectral->Run(nm);
            Check(std::fabs(nm.Sum()/N-1)<.03,"DL214 global NM deposit is conditional incident flux",nm.Sum()/N,1);spectral->release();
        }
        scene->release();manager->release();sink->release();paint->release();split->release();
    }
    object->release();coverage->release();
}
int main() { TestAlphaPhotonDeposits(); TestLiveSelection();TestCausticSelection();for(double tilt:{-30.,30.}){TestLiveSelection(tilt);TestCausticSelection(tilt);}TestDetector();TestRealSpectralDetector();TestOtherDetectors();TestShaderSelection();TestTranslucentContinuation();TestRareIntegratorSelection();
#ifdef RISE_TEST_SMS_PRIVATE_PROBE
for(double tilt:{0.,-30.,30.})TestSMSSelection(tilt);
#endif
TestGather();std::printf("LegacyPhotonTransportTest checks=%d failures=%d\n",checks,failures);return failures?1:0; }
