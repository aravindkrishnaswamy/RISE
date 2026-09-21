// Exact fixed-deposit and live-consumer oracles for DL239 / DL271 / DL272.
// No rasterizer: synthetic intersections isolate the real transport consumers.
#include <cstdio>
#include <cmath>
#include "../src/Library/PhotonMapping/GlobalPelPhotonTracer.h"
#include "../src/Library/PhotonMapping/GlobalSpectralPhotonTracer.h"
#include "../src/Library/PhotonMapping/CausticPelPhotonMap.h"
#include "../src/Library/Materials/LambertianBRDF.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/DetectorSpheres/IsotropicRGBDetectorSphere.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Scene.h"
#include "TestStubObject.h"
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
    bool sink;
    void Emit(const RayIntersectionGeometric& ri,ScatteredRayContainer& out,bool nm) const {
        for(int i=0;i<count;++i) {
            ScatteredRay r;
            r.type=ScatteredRay::eRayDiffuse;
            r.ray=Ray(ri.ptIntersection,Vector3(.6,0,.8));
            const double w=sink?0:count==1?1:nm?(i==0?.3:.7):(i==0?.2:.8);
            if(nm) r.krayNM=w; else r.kray=RISEPel(w);
            out.AddScatteredRay(r);
        }
    }
public:
    SplitMaterial(int n,bool stop=false):count(n),sink(stop) {}
    IBSDF* GetBSDF() const override { return nullptr; }
    ISPF* GetSPF() const override { return const_cast<SplitMaterial*>(this); }
    IEmitter* GetEmitter() const override { return nullptr; }
    void Scatter(const RayIntersectionGeometric& ri,ISampler&,ScatteredRayContainer& out,const IORStack&) const override { Emit(ri,out,false); }
    void ScatterNM(const RayIntersectionGeometric& ri,ISampler&,Scalar,ScatteredRayContainer& out,const IORStack&) const override { Emit(ri,out,true); }
};
class TwoPlaneManager : public ObjectManager {
    const IObject& object;
    const IMaterial& split;
    const IMaterial& sink;
public:
    TwoPlaneManager(const IObject& o,const IMaterial& a,const IMaterial& b):ObjectManager(false,false,4,8),object(o),split(a),sink(b) {}
    void IntersectRay(RayIntersection& ri,bool,bool,bool) const override {
        const bool first=ri.geometric.ray.Dir().z<0;
        ri.geometric.bHit=true;ri.geometric.range=1;
        ri.geometric.ptIntersection=Point3(0,0,first?0:1);
        ri.geometric.vNormal=Vector3(0,0,first?1:-1);
        ri.geometric.vGeomNormal=ri.geometric.vNormal;
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
    PelMap():GlobalPelPhotonMap(128,nullptr) {}
    double Sum() const { double total=0;for(const auto& p:vphotons)total+=p.power.r;return total; }
};
class NMMap : public GlobalSpectralPhotonMap {
public:
    NMMap():GlobalSpectralPhotonMap(128,nullptr) {}
    double Sum() const { double total=0;for(const auto& p:vphotons)total+=p.power;return total; }
};
void TestLiveSelection() {
    auto* split=new SplitMaterial(2);auto* sink=new SplitMaterial(1,true);auto* object=new StubObject();
    auto* manager=new TwoPlaneManager(*object,*split,*sink);auto* scene=new Scene();scene->SetObjectManager(manager);
    for(bool branch:{false,true}) {
        auto* tracer=new PelTracer(branch);tracer->AttachScene(scene);PelMap map;
        for(int i=0;i<16;++i)tracer->Run(map);
        Near(map.Sum()/16,1,branch?"DL271 live Pel branch sum":"DL271 live Pel selected response");tracer->release();
        auto* spectral=new NMTracer(branch);spectral->AttachScene(scene);NMMap nm;
        for(int i=0;i<16;++i)spectral->Run(nm);
        Near(nm.Sum()/16,1,branch?"DL271 live NM branch sum":"DL271 live NM selected response");spectral->release();
    }
    scene->release();manager->release();object->release();sink->release();split->release();
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
int main() { TestLiveSelection();TestDetector();TestGather();std::printf("LegacyPhotonTransportTest checks=%d failures=%d\n",checks,failures);return failures?1:0; }
