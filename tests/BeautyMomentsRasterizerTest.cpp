// Calls the real IntegratePixel seam directly with controlled camera/caster
// trials. No image render, scene parser, worker pool, or file output.
#include "../src/Library/Rendering/PixelBasedPelRasterizer.h"
#include "../src/Library/Rendering/PixelBasedSpectralIntegratingRasterizer.h"
#include "../src/Library/Rendering/ProgressiveFilm.h"
#include "../src/Library/Cameras/PinholeCamera.h"
#include "../src/Library/Managers/CameraManager.h"
#include "../src/Library/Scene.h"
#include "../src/Library/RISE_API.h"
#include <cstdio>
#include <limits>

using namespace RISE;
using namespace RISE::Implementation;
namespace {
unsigned int passed=0,failed=0;
void Check(bool ok,const char* message) {
    std::printf("%s: beauty-producer %s\n",ok?"PASS":"FAIL",message);
    if(ok) ++passed; else ++failed;
}
class ControlledCamera final : public PinholeCamera {
public:
    bool reject=false;
    ControlledCamera():CameraCommon(Point3(0,0,1),Point3(0,0,0),Vector3(0,1,0),1,0,0,0,Vector3(0,0,0),Vector2(0,0)),PinholeCamera(Point3(0,0,1),Point3(0,0,0),Vector3(0,1,0),1,1,1,1,0,0,0,Vector3(0,0,0),Vector2(0,0)){}
    bool GenerateRay(const RuntimeContext& rc,Ray& ray,const Point2& point) const override {
        return !reject && PinholeCamera::GenerateRay(rc,ray,point);
    }
};
class ControlledCaster final : public IRayCaster,public Reference {
    const IScene* scene;
public:
    bool hit=true;
    double luminance=1+0x1p-51;
    explicit ControlledCaster(const IScene* value):scene(value){}
    bool CastRay(const RuntimeContext&,const RasterizerState&,const Ray&,RISEPel&,
        const RAY_STATE&,Scalar*,const IRadianceMap*) const override {return false;}
    bool CastRayNM(const RuntimeContext&,const RasterizerState&,const Ray&,Scalar&,
        const RAY_STATE&,Scalar,Scalar*,const IRadianceMap*) const override {return false;}
    bool CastRay(const RuntimeContext&,const RasterizerState&,const Ray&,RISEPel& color,
        const RAY_STATE&,Scalar*,const IRadianceMap*,const IORStack&) const override {
        color=RISEPel(luminance);return hit;
    }
    bool CastRayNM(const RuntimeContext&,const RasterizerState&,const Ray&,Scalar& value,
        const RAY_STATE&,Scalar,Scalar*,const IRadianceMap*,const IORStack&) const override {
        value=luminance;return hit;
    }
    bool CastShadowRay(const Ray&,Scalar) const override {return false;}
    bool CastOcclusionRay(const Ray&,Scalar) const override {return false;}
    void AttachScene(const IScene*) override {}
    const IScene* GetAttachedScene() const override {return scene;}
    void SetLuminaireSampling(ISampling2D*) override {}
    const ILuminaryManager* GetLuminaries() const override {return nullptr;}
    const LightSampler* GetLightSampler() const override {return nullptr;}
    void SetRISCandidates(unsigned int) override {}
    void SetLightSampleRRThreshold(Scalar) override {}
    void SetUseLightBVH(bool) override {}
    bool IsRadianceMapVisibleAsBackground() const override {return false;}
};
class PelProbe final : public PixelBasedPelRasterizer {
public:
    PelProbe(IRayCaster* caster,const AdaptiveSamplingConfig& config):Rasterizer(nullptr),
        PixelBasedRasterizerHelper(caster,nullptr),PixelBasedPelRasterizer(caster,PathGuidingConfig(),config,StabilityConfig(),false){}
    using PixelBasedPelRasterizer::IntegratePixel;
};
class SpectralProbe final : public PixelBasedSpectralIntegratingRasterizer {
public:
    explicit SpectralProbe(IRayCaster* caster):Rasterizer(nullptr),PixelBasedRasterizerHelper(caster,nullptr),
        PixelBasedSpectralIntegratingRasterizer(caster,400,700,1,1,StabilityConfig(),false,false){}
    using PixelBasedSpectralIntegratingRasterizer::IntegratePixel;
};
}
int main() {
    auto* scene=new Scene;
    auto* manager=new CameraManager;
    scene->SetCameraManager(manager);manager->release();
    auto* camera=new ControlledCamera;
    Check(scene->AddCamera("controlled",camera) && scene->SetActiveCamera("controlled"),"camera seam setup");
    auto* caster=new ControlledCaster(scene);
    ISampling2D* sampling=nullptr;IPixelFilter* filter=nullptr;
    Check(RISE_API_CreateUniformSampling2D(&sampling,1,1) && RISE_API_CreateBoxPixelFilter(&filter,1,1),"sampling seam setup");
    if(!sampling || !filter) {caster->release();camera->release();scene->release();return 1;}
    sampling->SetNumSamples(1);
    RandomNumberGenerator random;
    RuntimeContext rc(random,RuntimeContext::PASS_NORMAL,false);
    ProgressiveFilm film(1,1);
    rc.pProgressiveFilm=&film;rc.totalProgressiveSPP=128;
    AdaptiveSamplingConfig config;config.maxSamples=128;config.threshold=1e-30;
    auto* pel=new PelProbe(caster,config);pel->SubSampleRays(sampling,filter);
    RISEColor color;
    caster->luminance=1+0x1p-52;
    pel->IntegratePixel(rc,0,0,1,*scene,color,false,0,0);
    Check(color.base.r==caster->luminance && color.base.g==caster->luminance && color.base.b==caster->luminance,
        "moment correction leaves actual Pel sample color unchanged");
    caster->luminance=1+0x1p-51;
    pel->IntegratePixel(rc,0,0,1,*scene,color,false,0,0);
    Check(film.Get(0,0).beautyMoments.count==2 && film.Get(0,0).beautyMoments.m2==0x1p-105 &&
        film.Get(0,0).beautyMoments.meanCorrection==-0x1p-53,
        "actual Pel producer preserves adjacent-float positive variance");
    film.Clear();caster->hit=false;
    pel->IntegratePixel(rc,0,0,1,*scene,color,false,0,0);
    Check(film.Get(0,0).beautyMoments.count==1 && film.Get(0,0).beautyMoments.mean==0,
        "actual Pel miss contributes a zero trial");
    film.Clear();camera->reject=true;
    for(unsigned int i=0;i<64;++i) pel->IntegratePixel(rc,0,0,1,*scene,color,false,0,0);
    Check(film.Get(0,0).beautyMoments.count==64 && film.Get(0,0).converged,
        "actual Pel camera rejection zeros survive passes and black convergence minimum");
    film.Clear();film.Get(0,0).beautyMoments.valid=false;
    for(unsigned int i=0;i<65;++i) pel->IntegratePixel(rc,0,0,1,*scene,color,false,0,0);
    Check(!film.Get(0,0).converged && film.Get(0,0).sampleIndex==65,
        "actual Pel sticky invalid variance cannot terminate black early");
    pel->release();
    film.Clear();config.maxSamples=0;
    pel=new PelProbe(caster,config);pel->SubSampleRays(sampling,filter);
    for(unsigned int i=0;i<65;++i) pel->IntegratePixel(rc,0,0,1,*scene,color,false,0,0);
    Check(film.Get(0,0).beautyMoments.count==0 && !film.Get(0,0).converged && film.Get(0,0).sampleIndex==65,
        "actual nonadaptive progressive Pel preserves fixed sample budget gating");
    pel->release();
    auto* spectral=new SpectralProbe(caster);spectral->SubSampleRays(sampling,filter);
    film.Clear();camera->reject=false;
    spectral->IntegratePixel(rc,0,0,1,*scene,color,false,0,0);
    Check(film.Get(0,0).beautyMoments.count==1 && film.Get(0,0).beautyMoments.mean==0,
        "actual legacy spectral miss contributes a zero trial");
    film.Clear();camera->reject=true;
    for(unsigned int i=0;i<64;++i) spectral->IntegratePixel(rc,0,0,1,*scene,color,false,0,0);
    Check(film.Get(0,0).beautyMoments.count==64 && film.Get(0,0).converged,
        "actual legacy spectral retains progressive-only moment and convergence policy");
    spectral->release();sampling->release();filter->release();caster->release();camera->release();scene->release();
    std::printf("%u passed, %u failed\n",passed,failed);
    return failed?1:0;
}
