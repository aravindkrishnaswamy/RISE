// DL-532: exact native cameras/lights may consume numeric scratch immediately.
#include <atomic>
#include <cstdlib>
#include <new>
#include <cstdio>
#include <cstring>
#include <vector>
#include <functional>
#include "../src/Library/RISE_API.h"
#include "../src/Library/Cameras/CameraCommon.h"
#include "../src/Library/Lights/PointLight.h"
#include "../src/Library/Lights/SpotLight.h"
#include "../src/Library/Lights/DirectionalLight.h"
#include "../src/Library/Animation/Timeline.h"
#include "../src/Library/Animation/KeyframableHelper.h"
#include "../src/Library/Utilities/SimpleInterpolators.h"
using namespace RISE;
using namespace RISE::Implementation;
static std::atomic<size_t> allocations{0};
static bool counting=false;
void* operator new(std::size_t n) {
    if(counting) ++allocations;
    if(void* p=std::malloc(n?n:1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
static int failures=0;
template<class T,class Read>
static void Measure(const char* label,IKeyframable& target,const char* parameter,
                    const char* firstText,const char* lastText,Read read,size_t expectedAlloc=0)
{
    IKeyframeParameter* a=target.KeyframeFromParameters(parameter,firstText);
    IKeyframeParameter* b=target.KeyframeFromParameters(parameter,lastText);
    if(!a || !b) { safe_release(a);safe_release(b);++failures;return; }
    std::vector<KeyframeParameterDispatch> values;
    values.emplace_back(*a);values.emplace_back(*b);a->release();b->release();
    std::vector<T> reference(5000);
    auto* legacy=new LinearInterpolator<KeyframeParameterDispatch>();
    for(int i=1;i<5000;++i) {
        auto first=values.cbegin(),last=first+1;
        auto value=legacy->Interpolate2Values(values,first,last,Scalar(i)/5000);
        reference[i]=*static_cast<T*>(value.GetParam().getValue());
    }
    legacy->release();
    size_t mismatches=0;
    {
        Timeline timeline(&target,parameter);
        if(!timeline.InsertKeyframe(firstText,0,nullptr) || !timeline.InsertKeyframe(lastText,1,nullptr)) {
            ++failures;return;
        }
        allocations=0;
        for(int i=1;i<5000;++i) {
            counting=true;timeline.EvaluateAtTime(Scalar(i)/5000);counting=false;
            const T got=read();
            if(std::memcmp(&got,&reference[i],sizeof(T))) ++mismatches;
        }
        std::printf("%s n=4999 allocations=%zu allocations/interpolation=%.6g bit mismatches=%zu SD=0 (counter)\n",
                    label,allocations.load(),double(allocations.load())/4999,mismatches);
        if(allocations!=expectedAlloc || mismatches) ++failures;
    }
    // Also read after Timeline and its keyframes are destroyed.
    const T last=read();
    if(std::memcmp(&last,&reference[4999],sizeof(T))) ++failures;
}
class RetainingPointLight final : public PointLight {
public:
    const IKeyframeParameter* retained=nullptr;
    RetainingPointLight():PointLight(1,RISEPel(1,1,1),false) {}
    ~RetainingPointLight() override { safe_release(retained); }
    void SetIntermediateValue(const IKeyframeParameter& p) override {
        PointLight::SetIntermediateValue(p);
        p.addref();safe_release(retained);retained=&p;
    }
};
int main() {
    const Point3 location(0,0,4),lookAt(0,0,0);
    const Vector3 up(0,1,0),orientation(0,0,0);
    const Vector2 target(0,0);
    for(int kind=0;kind<4;++kind) {
        ICamera* camera=nullptr;bool ok=false;
        const char* labels[]={"pinhole","thinlens","fisheye","orthographic"};
        if(kind==0) ok=RISE_API_CreatePinholeCamera(&camera,location,lookAt,up,1,32,32,1,1,0,0,orientation,target);
        if(kind==1) ok=RISE_API_CreateThinlensCamera(&camera,location,lookAt,up,36,50,4,5,1,32,32,1,1,0,0,orientation,target,0,0,1,0,0,0,0);
        if(kind==2) ok=RISE_API_CreateFisheyeCamera(&camera,location,lookAt,up,32,32,1,1,0,0,orientation,target,1);
        if(kind==3) ok=RISE_API_CreateOrthographicCamera(&camera,location,lookAt,up,32,32,Vector2(2,2),1,1,0,0,orientation,target);
        auto* common=dynamic_cast<CameraCommon*>(camera);
        if(!ok || !common) { ++failures;safe_release(camera);continue; }
        char label[64];std::snprintf(label,sizeof(label),"%s location",labels[kind]);
        Measure<Point3>(label,*camera,"location","1.125 2.25 3.5","9.625 8.75 6.5",[&]{return common->GetRestLocation();});
        std::snprintf(label,sizeof(label),"%s orientation",labels[kind]);
        Measure<Vector3>(label,*camera,"orientation","1.125 2.25 3.5","9.625 8.75 6.5",[&]{return common->GetEulerOrientation();});
        std::snprintf(label,sizeof(label),"%s target orientation",labels[kind]);
        Measure<Vector2>(label,*camera,"target_orientation","1.125 2.25","9.625 8.75",[&]{return common->GetTargetOrientation();});
        std::snprintf(label,sizeof(label),"%s exposure",labels[kind]);
        Measure<Scalar>(label,*camera,"exposure","1.125","9.625",[&]{return common->GetExposureTimeStored();});
        camera->release();
    }
    ILightPriv* lights[3]={new PointLight(1,RISEPel(1,1,1),false),
        new SpotLight(1,Point3(0,0,-1),.2,.4,RISEPel(1,1,1),false),
        new DirectionalLight(1,RISEPel(1,1,1),Vector3(0,0,1))};
    const char* names[]={"point","spot","directional"};
    for(int i=0;i<3;++i) {
        char label[64];std::snprintf(label,sizeof(label),"%s energy",names[i]);
        Measure<Scalar>(label,*lights[i],"energy","1.125","9.625",[&]{return lights[i]->emissionEnergy();});
        std::snprintf(label,sizeof(label),"%s colour",names[i]);
        Measure<RISEPel>(label,*lights[i],"color","0.125 0.25 0.375","0.625 0.75 0.875",[&]{return lights[i]->emissionColor();});
        lights[i]->release();
    }
    auto* holder=new RetainingPointLight();
    Measure<Scalar>("retaining point subclass",*holder,"energy","1.125","9.625",
        [&]{return holder->retained?*static_cast<Scalar*>(holder->retained->getValue()):Scalar(-1);},9998);
    holder->release();
    std::printf("%d failures\n",failures);return failures?1:0;
}
