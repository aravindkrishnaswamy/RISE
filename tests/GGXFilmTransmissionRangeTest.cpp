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
    spf->release(); object->release(); film->release(); zero->release(); eta->release(); alpha->release(); white->release();
    std::printf("GGXFilmTransmissionRangeTest: %s\n",good?"PASS":"FAIL");
    return good?0:1;
}
