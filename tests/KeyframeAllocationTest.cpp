// DL-532: linear interpolation eliminates two temporary clones; zero-allocation pin remains.
#include <atomic>
#include <cstdlib>
#include <new>
#include <cstdio>
#include <cstring>
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
int main(int argc, char** argv) {
    Element* e = new Element();
    Timeline t(e, "value");
    e->release();
    if (!t.InsertKeyframe("1.125",0,nullptr) || !t.InsertKeyframe("9.625",1,nullptr)) return 2;
    // Independent legacy expression evaluator, outside the allocation window.
    std::vector<Scalar> reference(5000);
    Parameter<Scalar>* a = new Parameter<Scalar>(1.125,1);
    Parameter<Scalar>* b = new Parameter<Scalar>(9.625,1);
    std::vector<KeyframeParameterDispatch> values;
    values.emplace_back(*a); values.emplace_back(*b); a->release(); b->release();
    auto* legacy = new LinearInterpolator<KeyframeParameterDispatch>();
    for(int i=1;i<5000;++i) {
        auto first=values.cbegin(),second=first+1;
        auto result=legacy->Interpolate2Values(values,first,second,Scalar(i)/5000);
        reference[i]=*static_cast<Scalar*>(result.GetParam().getValue());
    }
    legacy->release();
    size_t mismatches = 0;
    allocations = 0; counting = true;
    for (int i=1; i<5000; ++i) {
        const Scalar x = Scalar(i)/5000;
        t.EvaluateAtTime(x);
        const Scalar expected = reference[i];
        if (std::memcmp(&e->value,&expected,sizeof(expected))) ++mismatches;
    }
    counting = false;
    std::printf("samples=4999 allocations=%zu mismatches=%zu\n",allocations.load(),mismatches);
    // A target may retain a result past later evaluations and Timeline lifetime.
    // This catches replacing the owned result with borrowed stack scratch.
    RetainingElement* holder = new RetainingElement();
    const IKeyframeParameter* earlier = nullptr;
    Scalar oldExpected = 0, newExpected = 0;
    {
        Timeline retainedTimeline(holder, "value");
        if (!retainedTimeline.InsertKeyframe("1.125",0,nullptr) ||
            !retainedTimeline.InsertKeyframe("9.625",1,nullptr)) return 2;
        retainedTimeline.EvaluateAtTime(Scalar(1500)/5000);
        oldExpected = holder->value;
        earlier = holder->retained;
        earlier->addref();
        retainedTimeline.EvaluateAtTime(Scalar(3500)/5000);
        newExpected = holder->value;
    }
    const Scalar oldValue = *static_cast<Scalar*>(earlier->getValue());
    const Scalar newValue = *static_cast<Scalar*>(holder->retained->getValue());
    const bool retention = std::memcmp(&oldValue,&oldExpected,sizeof(Scalar)) == 0 &&
                           std::memcmp(&newValue,&newExpected,sizeof(Scalar)) == 0;
    safe_release(earlier);
    holder->release();
    std::printf("retained results survive: %s\n", retention ? "yes" : "no");
    const bool strict = argc == 2 && std::strcmp(argv[1],"--zero-allocation-pin") == 0;
    return allocations <= (strict ? 0u : 2u*4999u) && mismatches == 0 && retention ? 0 : 1;
}
