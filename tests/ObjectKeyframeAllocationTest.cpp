// DL-532: exact Object linear targets consume borrowed scratch without allocations.
#include <atomic>
#include <cstdlib>
#include <new>
#include <cstdio>
#include <cstring>
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Animation/Timeline.h"
#include "../src/Library/Animation/KeyframableHelper.h"
#include "../src/Library/Utilities/SimpleInterpolators.h"
using namespace RISE;
using namespace RISE::Implementation;
static std::atomic<size_t> allocations{0};
static bool counting = false;
void* operator new(std::size_t n) {
    if (counting) ++allocations;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void* p) noexcept { std::free(p); }
class Element : public virtual IKeyframable, public virtual Reference {
public:
    Scalar value = 0;
    IKeyframeParameter* KeyframeFromParameters(const String&, const String& s) override {
        return new Parameter<Scalar>(std::atof(s.c_str()), 1);
    }
    void SetIntermediateValue(const IKeyframeParameter& p) override { value = *static_cast<Scalar*>(p.getValue()); }
    void RegenerateData() override {}
};
class RetainingElement final : public Element {
public:
    const IKeyframeParameter* retained = nullptr;
    ~RetainingElement() override { safe_release(retained); }
    void SetIntermediateValue(const IKeyframeParameter& p) override {
        p.addref();
        safe_release(retained);
        retained = &p;
        Element::SetIntermediateValue(p);
    }
};
int main() {
    Object* object=new Object();
    Timeline timeline(object,"position"); object->release();
    if(!timeline.InsertKeyframe("1.125 2.25 3.5",0,nullptr) || !timeline.InsertKeyframe("9.625 8.75 6.5",1,nullptr)) return 2;
    std::vector<Point3> reference(5000);
    Parameter<Point3>* a=new Parameter<Point3>(Point3(1.125,2.25,3.5),1000);
    Parameter<Point3>* b=new Parameter<Point3>(Point3(9.625,8.75,6.5),1000);
    std::vector<KeyframeParameterDispatch> values;
    values.emplace_back(*a);values.emplace_back(*b);a->release();b->release();
    auto* legacy=new LinearInterpolator<KeyframeParameterDispatch>();
    for(int i=1;i<5000;++i) { auto first=values.cbegin(),second=first+1;
        auto result=legacy->Interpolate2Values(values,first,second,Scalar(i)/5000);
        reference[i]=*static_cast<Point3*>(result.GetParam().getValue()); }
    legacy->release();size_t mismatches=0;
    allocations=0;
    for(int i=1;i<5000;++i) { counting=true;timeline.EvaluateAtTime(Scalar(i)/5000);counting=false;
        object->FinalizeTransformations();
        const Point3 got=Point3Ops::Transform(object->GetFinalTransformMatrix(),Point3(0,0,0));
        if(std::memcmp(&got,&reference[i],sizeof(Point3))) ++mismatches; }
    counting=false;
    std::printf("n=4999 allocations=%zu bit mismatches=%zu\n",allocations.load(),mismatches);
    return allocations==0 && mismatches==0 ? 0:1;
}
