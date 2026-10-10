#pragma once
// DL-417 independent constant-index oracle. DL-490 now refreshes production
// layer records too; the adapter remains an independent reference. A
// reflection-only substrate sees the live stack at each stateful call;
// stackless substrate queries use the known 1.5 gap. The explicit adapter
// lets the regression compare native production calls with the reference.
#include "../src/Library/Interfaces/IBSDF.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Utilities/Reference.h"
namespace CoatedReference {
using namespace RISE;
using RISE::Implementation::Reference;
inline RayIntersectionGeometric Interior(const RayIntersectionGeometric& ri,Scalar ambient=1.5) {
    auto copy=ri;copy.ambientIOR=ambient;return copy;
}
struct Observation {
    unsigned calls=0,mismatchCalls=0,valueCalls=0,valueMismatches=0,pdfCalls=0,pdfMismatches=0,gapValueCalls=0;
    Scalar record=0,stack=0;bool mismatch=false;
};
class AmbientBSDF : public IBSDF,public Reference {
    IBSDF& base;
    bool correct;
    Observation* observation;
    RayIntersectionGeometric Input(const RayIntersectionGeometric& r,const IORStack* st=nullptr) const {return correct?Interior(r,st?st->top():1.5):r;}
public:
    explicit AmbientBSDF(IBSDF& b,bool c,Observation* o):base(b),correct(c),observation(o){base.addref();}
    void Observe(const RayIntersectionGeometric& r,const IORStack* st) const {
        if(observation && st) {
            ++observation->valueCalls;
            observation->valueMismatches+=r.ambientIOR!=st->top();
            observation->gapValueCalls+=st->top()>1;
        }
    }
    ~AmbientBSDF(){base.release();}
    RISEPel value(const Vector3& w,const RayIntersectionGeometric& r) const override {return base.value(w,Input(r));}
    Scalar valueNM(const Vector3& w,const RayIntersectionGeometric& r,Scalar nm) const override {return base.valueNM(w,Input(r),nm);}
    RISEPel valueStateful(const Vector3& w,const RayIntersectionGeometric& r,const IORStack* st) const override {auto in=Input(r,st);Observe(in,st);return base.valueStateful(w,in,st);}
    Scalar valueStatefulNM(const Vector3& w,const RayIntersectionGeometric& r,Scalar nm,const IORStack* st) const override {auto in=Input(r,st);Observe(in,st);return base.valueStatefulNM(w,in,nm,st);}
    RISEPel albedo(const RayIntersectionGeometric& r) const override {return base.albedo(Input(r));}
    bool hemisphericalAlbedo(const RayIntersectionGeometric& r,RISEPel& out) const override {return base.hemisphericalAlbedo(Input(r),out);}
    bool hemisphericalAlbedoNM(const RayIntersectionGeometric& r,Scalar nm,Scalar& out) const override {return base.hemisphericalAlbedoNM(Input(r),nm,out);}
};
class AmbientSPF : public ISPF,public Reference {
    ISPF& base;
    bool correct;
    Observation* observation;
    RayIntersectionGeometric Input(const RayIntersectionGeometric& r,const IORStack& st) const {return correct?Interior(r,st.top()):r;}
    // Injected caller-owned bookkeeping; these standalone probes use one
    // thread and keep the observation alive for the entire scatter loop.
    void Observe(const RayIntersectionGeometric& r,const IORStack& st) const {
        if(observation) {
            ++observation->calls;observation->record=r.ambientIOR;observation->stack=st.top();
            observation->mismatch|=r.ambientIOR!=st.top();
            observation->mismatchCalls+=r.ambientIOR!=st.top();
        }
    }
public:
    explicit AmbientSPF(ISPF& b,bool c,Observation* o):base(b),correct(c),observation(o){base.addref();}
    ~AmbientSPF(){base.release();}
    void Scatter(const RayIntersectionGeometric& r,ISampler& s,ScatteredRayContainer& c,const IORStack& st) const override {auto in=Input(r,st);Observe(in,st);base.Scatter(in,s,c,st);}
    void ScatterNM(const RayIntersectionGeometric& r,ISampler& s,Scalar nm,ScatteredRayContainer& c,const IORStack& st) const override {auto in=Input(r,st);Observe(in,st);base.ScatterNM(in,s,nm,c,st);}
    Scalar Pdf(const RayIntersectionGeometric& r,const Vector3& w,const IORStack& st) const override {auto in=Input(r,st);if(observation){++observation->pdfCalls;observation->pdfMismatches+=in.ambientIOR!=st.top();}return base.Pdf(in,w,st);}
    Scalar PdfNM(const RayIntersectionGeometric& r,const Vector3& w,Scalar nm,const IORStack& st) const override {auto in=Input(r,st);if(observation){++observation->pdfCalls;observation->pdfMismatches+=in.ambientIOR!=st.top();}return base.PdfNM(in,w,nm,st);}
    SpecularInfo GetSpecularInfo(const RayIntersectionGeometric& r,const IORStack& st) const override {return base.GetSpecularInfo(Input(r,st),st);}
    SpecularInfo GetSpecularInfoNM(const RayIntersectionGeometric& r,const IORStack& st,Scalar nm) const override {return base.GetSpecularInfoNM(Input(r,st),st,nm);}
};
class AmbientMaterial : public IMaterial,public Reference {
    AmbientBSDF* bsdf;
    AmbientSPF* spf;
public:
    explicit AmbientMaterial(const IMaterial& b,bool correct=true,Observation* observation=nullptr) {
        bsdf=new AmbientBSDF(*b.GetBSDF(),correct,observation);bsdf->addref();
        spf=new AmbientSPF(*b.GetSPF(),correct,observation);spf->addref();
    }
    ~AmbientMaterial(){bsdf->release();spf->release();}
    IBSDF* GetBSDF() const override {return bsdf;}
    ISPF* GetSPF() const override {return spf;}
    IEmitter* GetEmitter() const override {return nullptr;}
};
}
