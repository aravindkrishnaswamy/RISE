// DL-37: passive dispersive film must never create negative diffuse transport.
// Primary forced-diffuse case was committed and executed before the RGB
// interface range correction. Guide quadrature and NM controls were added
// afterward and are consistency coverage, not additional red proof.
#include <cmath>
#include <cstdio>
#include "../src/Library/Materials/GGXSPF.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "TestStubObject.h"
using namespace RISE;
using namespace RISE::Implementation;
class NotchedIOR : public UniformScalarPainter {
public:
    NotchedIOR() : UniformScalarPainter(2) {}
    Scalar GetValueAtNM(const RayIntersectionGeometric&, Scalar nm) const override {
        return nm >= 505 && nm <= 530 ? Scalar(1) : Scalar(2);
    }
};
class DiffuseDraw : public ISampler {
    unsigned int count = 0;
public:
    Scalar Get1D() override { return count++ == 0 ? Scalar(0) : Scalar(0.5); }
    Point2 Get2D() override { const Scalar u = Get1D(); const Scalar v = Get1D(); return Point2(u,v); }
    void StartStream(int) override {}
};
int main() {
    auto* white = new UniformColorPainter(RISEPel(1));
    auto* alpha = new UniformScalarPainter(.16);
    auto* eta = new NotchedIOR();
    auto* zero = new UniformScalarPainter(0);
    auto* film = new UniformScalarPainter(1.5);
    auto* spf = new GGXSPF(*white,*white,*alpha,*alpha,*eta,*zero,eFresnelThinFilmConductor,nullptr,film,zero,zero);
    StubObject* object = new StubObject();
    const Scalar mu = .001;
    Ray ray(Point3(0,0,1),Vector3(std::sqrt(1-mu*mu),0,-mu));
    RasterizerState state = {0,0}; RayIntersectionGeometric ri(ray,state);
    ri.bHit=true; ri.ptIntersection=Point3(0,0,0); ri.onb.CreateFromW(Vector3(0,0,1));
    ri.vNormal=Vector3(0,0,1); ri.vGeomNormal=ri.vNormal; ri.ambientIOR=1;
    DiffuseDraw sampler; ScatteredRayContainer rays;
    IORStack stack = MakeTestIORStack(object);
    spf->Scatter(ri,sampler,rays,stack);
    bool good = rays.Count()==1;
    for(unsigned int i=0;i<rays.Count();++i) {
        std::printf("diffuse RGB %.12g %.12g %.12g\n",rays[i].kray[0],rays[i].kray[1],rays[i].kray[2]);
        for(int c=0;c<3;++c) if(!std::isfinite(rays[i].kray[c]) || rays[i].kray[c]<0) good=false;
    }
    // The public zero-diffuse albedo guide exposes the directional interface
    // estimate; integrate it independently and compare its hemispherical guide.
    auto* black = new UniformColorPainter(RISEPel(0.0));
    auto* bare = new GGXBRDF(*black,*white,*alpha,*alpha,*eta,*zero,eFresnelThinFilmConductor,nullptr,film,zero,zero);
    auto* mixed = new GGXBRDF(*white,*white,*alpha,*alpha,*eta,*zero,eFresnelThinFilmConductor,nullptr,film,zero,zero);
    RISEPel integrated(0.0), mean(0.0);
    const int quadrature = 4096;
    for(int i=0;i<quadrature;++i) {
        const Scalar cosine=(i+.5)/quadrature;
        RayIntersectionGeometric hit=ri;
        hit.ray.Set(Point3(0,0,1),Vector3(std::sqrt(1-cosine*cosine),0,-cosine));
        const RISEPel value=bare->albedo(hit);
        for(int c=0;c<3;++c) if(!std::isfinite(value[c]) || value[c]<0 || value[c]>1) good=false;
        integrated=integrated+value*(2*cosine/quadrature);
    }
    if(!bare->hemisphericalAlbedo(ri,mean)) good=false;
    Scalar maxGuideError=0;
    for(int c=0;c<3;++c) {
        const Scalar error=std::fabs(mean[c]-integrated[c]);
        if(!std::isfinite(error) || error>2e-4) good=false;
        maxGuideError=r_max(maxGuideError,error);
    }
    std::printf("guide midpoint-vs-GL max error %.12g\n",maxGuideError);
    if(rays.Count()==1) {
        const Vector3 light=rays[0].ray.Dir();
        const RISEPel diffuse=mixed->value(light,ri)-bare->value(light,ri);
        for(int c=0;c<3;++c) if(!std::isfinite(diffuse[c]) || diffuse[c]<-1e-12) good=false;
        for(Scalar nm : {450.0,517.5,650.0}) {
            DiffuseDraw nmSampler; ScatteredRayContainer nmRays;
            spf->ScatterNM(ri,nmSampler,nm,nmRays,stack);
            if(nmRays.Count()!=1) good=false;
            for(unsigned int i=0;i<nmRays.Count();++i)
                if(!std::isfinite(nmRays[i].krayNM) || nmRays[i].krayNM<0) good=false;
        }
    }
    mixed->release(); bare->release(); black->release();
    spf->release(); object->release(); film->release(); zero->release(); eta->release(); alpha->release(); white->release();
    std::printf("GGXFilmTransmissionRangeTest: %s\n",good?"PASS":"FAIL");
    return good?0:1;
}
