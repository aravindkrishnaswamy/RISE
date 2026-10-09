//////////////////////////////////////////////////////////////////////
//
//  ManifoldSolver.cpp - Specular Manifold Sampling solver
//
//    Implements the Newton iteration method from Zeltner et al. 2020
//    for finding valid specular paths connecting two non-specular
//    endpoints through a chain of specular surfaces.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ManifoldSolver.h"
#include "../Managers/ObjectManager.h"
#include "../Geometry/SphereGeometry.h"
#include "../Geometry/EllipsoidGeometry.h"
#include "../Geometry/TorusGeometry.h"
#include "../Geometry/CylinderGeometry.h"
#include "../Geometry/TriangleMeshGeometryIndexed.h"
#include "../Geometry/TriangleMeshGeometry.h"
#include "../Painters/UniformColorPainter.h"
#include "../Interfaces/IRayIntersectionModifier.h"
#include "../Interfaces/IUVGenerator.h"
#include "../Interfaces/IGeometry.h"		// CanBeAreaLight(): SMS surface seeding shares the sampling contract
#include "SMSPhotonMap.h"
#include "Optics.h"
#include "BDPTUtilities.h"
#include "../Interfaces/IScalarPainter.h"

#include "../Materials/DielectricMaterial.h"
#include "../Materials/PerfectRefractorMaterial.h"
#include "../Materials/PerfectReflectorMaterial.h"
#include "../Materials/PolishedMaterial.h"
#include "../Materials/CompositeMaterial.h"
#include "../Objects/CSGObject.h"
#include "../Objects/Object.h"
#include "IORStackSeeding.h"
#include <typeinfo>
#include <optional>
#include <array>
#include <cstring>

namespace {
    using RISE::Implementation::SMSReferenceCounters;
    thread_local SMSReferenceCounters* smsActiveDiagnostics=nullptr;
    thread_local unsigned smsScratchDepth=0;

    // Memoized dynamic type queries (DL-449). On Apple arm64 a failed
    // type_info comparison falls back to strcmp of mangled names, and the
    // extended predicate asks the same few questions of the same few
    // materials/objects at every walk event and Newton iteration (42 % of
    // its time was RTTI). Both answers below are exact functions of the key:
    // dynamic_cast<To*>(p) depends only on the most-derived type of *p and
    // on WHICH From subobject p addresses, and the latter is identified by
    // p's offset from the complete object (distinct subobjects of one type
    // have distinct offsets). The cached entry stores the result's offset
    // from the complete object, so a hit reproduces dynamic_cast exactly.
    // type_info objects have static storage, so a key never dangles; a type
    // with several type_info copies merely occupies several entries.
    // Each <To,From> instantiation has its own table, so the source static
    // type is part of the key. Precondition: never call on an object under
    // construction or destruction (its construction-vtable layout could share
    // a key with a complete object of that class but need another offset).
    constexpr unsigned kSMSTypeCacheSize=8;
    template<class To,class From>
    const To* SMSDynamicCast(const From* p) {
        static_assert(std::is_polymorphic<From>::value,"SMSDynamicCast needs a polymorphic source");
        if(!p) return nullptr;
        const char* complete=static_cast<const char*>(dynamic_cast<const void*>(p));
        const std::type_info* type=&typeid(*p);
        const std::ptrdiff_t subobject=reinterpret_cast<const char*>(p)-complete;
        struct Entry { const std::type_info* type=nullptr; std::ptrdiff_t subobject=0, offset=0; bool castable=false; };
        thread_local Entry table[kSMSTypeCacheSize];
        thread_local unsigned cursor=0;
        for(const Entry& e:table) if(e.type==type && e.subobject==subobject)
            return e.castable?reinterpret_cast<const To*>(complete+e.offset):nullptr;
        const To* result=dynamic_cast<const To*>(p);
        Entry& e=table[cursor++%kSMSTypeCacheSize];
        e.type=type;e.subobject=subobject;e.castable=result!=nullptr;
        e.offset=result?reinterpret_cast<const char*>(result)-complete:0;
        return result;
    }
    // Capture memo (DL-449). SMSDomainReplay::Capture is a deterministic
    // function of (scene, anchor, environment index, ordered stack object
    // keys): it reads nothing else from the live stack, and the prepared
    // scene is immutable while a render evaluates. A memo exists only
    // inside an SMSCaptureMemoScope (one extended evaluation on one thread)
    // and matches the anchor bit for bit, so a hit returns exactly what the
    // uncached Capture would have computed.
    struct SMSCaptureMemo {
        struct Entry {
            const RISE::IScene* scene=nullptr; RISE::Point3 anchor; RISE::Scalar environment=0;
            std::vector<const RISE::IObject*> keys; bool captured=false;
            RISE::Implementation::SMSStartingMedia media;
        };
        static constexpr std::size_t kEntries=8;
        std::vector<Entry> entries;
    };
    thread_local SMSCaptureMemo* smsCaptureMemo=nullptr;
    struct SMSCaptureMemoScope {
        SMSCaptureMemo memo; bool owner=false;
        SMSCaptureMemoScope() { if(!smsCaptureMemo) {smsCaptureMemo=&memo;owner=true;} }
        ~SMSCaptureMemoScope() { if(owner) smsCaptureMemo=nullptr; }
        SMSCaptureMemoScope(const SMSCaptureMemoScope&)=delete;
        SMSCaptureMemoScope& operator=(const SMSCaptureMemoScope&)=delete;
    };

    // typeid(x)==typeid(T), memoized by the dynamic type_info address.
    template<class T,class From>
    bool SMSSameType(const From& x) {
        static_assert(std::is_polymorphic<From>::value,"SMSSameType needs a polymorphic source");
        const std::type_info* type=&typeid(x);
        struct Entry { const std::type_info* type=nullptr; bool same=false; };
        thread_local Entry table[kSMSTypeCacheSize];
        thread_local unsigned cursor=0;
        for(const Entry& e:table) if(e.type==type) return e.same;
        const bool same=*type==typeid(T);
        Entry& e=table[cursor++%kSMSTypeCacheSize];
        e.type=type;e.same=same;
        return same;
    }

    void SMSResetResult(RISE::Implementation::ManifoldResult& result) {
        std::vector<RISE::Implementation::ManifoldVertex> buffer;
        buffer.swap(result.specularChain);result=RISE::Implementation::ManifoldResult();
        buffer.clear();result.specularChain.swap(buffer);
    }
    struct SMSWorkerScratchFrame {
        std::array<std::vector<RISE::Implementation::ManifoldVertex>,4> vertices;
        std::array<std::vector<RISE::Implementation::SMSDomainVertex>,2> records;
        std::array<std::vector<RISE::Scalar>,16> scalars;
        std::array<std::vector<RISE::Scalar>,4> derivatives;
        std::array<std::unique_ptr<RISE::Implementation::SMSDomainRoot>,2> roots;
        void Clear() {
            for(auto& v:vertices) v.clear();for(auto& v:records) v.clear();
            for(auto& v:scalars) v.clear();for(auto& v:derivatives) v.clear();
            for(auto& root:roots) if(root) {
                root->vertices.clear();SMSResetResult(root->result);
                root->startingStack=RISE::IORStack(1);root->accepted=false;
            }
        }
    };
    struct SMSWorkerScratchPool {
        std::vector<std::unique_ptr<SMSWorkerScratchFrame>> frames;
        std::size_t depth=0,bytes=0;
    };
    thread_local SMSWorkerScratchPool smsWorkerScratch;
    class SMSWorkerScratchLease {
        SMSWorkerScratchFrame* frame=nullptr;
        SMSReferenceCounters* previous=nullptr;
        void Peak() {
            if(!smsActiveDiagnostics) return;
            auto& peak=smsActiveDiagnostics->scratchPeakBytes;
            auto old=peak.load(std::memory_order_relaxed);
            while(old<smsWorkerScratch.bytes && !peak.compare_exchange_weak(old,smsWorkerScratch.bytes,std::memory_order_relaxed)) {}
        }
        template<class T> std::vector<T>& Reserve(std::vector<T>& vector,std::size_t count) {
            const auto before=vector.capacity();
            if(before<count) {
                vector.reserve(count);
                smsWorkerScratch.bytes+=(vector.capacity()-before)*sizeof(T);
                if(smsActiveDiagnostics) smsActiveDiagnostics->scratchBufferGrowths.fetch_add(1,std::memory_order_relaxed);
            }
            Peak();return vector;
        }
    public:
        SMSWorkerScratchLease(bool enabled,SMSReferenceCounters* counters) {
            if(!enabled) return;
            auto& pool=smsWorkerScratch;
            if(pool.depth==pool.frames.size()) {
                const auto before=pool.frames.capacity();
                pool.frames.emplace_back(std::make_unique<SMSWorkerScratchFrame>());
                pool.bytes+=sizeof(SMSWorkerScratchFrame)+(pool.frames.capacity()-before)*sizeof(pool.frames[0]);
                if(counters) counters->scratchFrames.fetch_add(1,std::memory_order_relaxed);
            }
            frame=pool.frames[pool.depth++].get();++smsScratchDepth;previous=smsActiveDiagnostics;
            if(counters) smsActiveDiagnostics=counters;
            Peak();
        }
        ~SMSWorkerScratchLease() {
            if(frame) {frame->Clear();--smsWorkerScratch.depth;--smsScratchDepth;smsActiveDiagnostics=previous;}
        }
        SMSWorkerScratchLease(const SMSWorkerScratchLease&)=delete;
        SMSWorkerScratchLease& operator=(const SMSWorkerScratchLease&)=delete;
        bool Enabled() const {return frame!=nullptr;}
        auto& Vertices(unsigned slot,std::size_t count) {return Reserve(frame->vertices[slot],count);}
        auto& Records(unsigned slot,std::size_t count) {return Reserve(frame->records[slot],count);}
        auto& Scalars(unsigned slot,std::size_t count) {return Reserve(frame->scalars[slot],count);}
        auto& Derivatives(std::size_t count) {
            for(auto& vector:frame->derivatives) Reserve(vector,count);
            return frame->derivatives;
        }
        auto& Root(unsigned slot,RISE::Implementation::SMSQueryDomain domain,const RISE::IORStack& stack,std::size_t count) {
            auto& root=frame->roots[slot];
            if(!root) {root=std::make_unique<RISE::Implementation::SMSDomainRoot>(domain,stack);smsWorkerScratch.bytes+=sizeof(*root);}
            Reserve(root->vertices,count);Reserve(root->result.specularChain,count);Peak();return *root;
        }
    };
}
void RISE::Implementation::SMSRecordSceneIntersection() {
    if(smsActiveDiagnostics) smsActiveDiagnostics->sceneIntersectionQueries.fetch_add(1,std::memory_order_relaxed);
}
void RISE::Implementation::SMSRecordObjectIntersection() {
    if(smsActiveDiagnostics) smsActiveDiagnostics->objectIntersectionQueries.fetch_add(1,std::memory_order_relaxed);
}


namespace {
    // Only audited native charts establish periodic equivalence. Authored
    // mesh UVs and Object UV generators retain ordinary context equality.
    RISE::Point2 SMSPeriodicTextureAxes(const RISE::IObject& object, const RISE::Vector3& normal) {
        using namespace RISE;
        using namespace RISE::Implementation;
        const auto* native = SMSDynamicCast<Object>(&object);
        const IGeometry* geometry = object.GetGeometry();
        if(!native || !SMSSameType<Object>(object) || !native->UsesNativeTextureChart() || !geometry) return Point2(0,0);
        if(SMSSameType<SphereGeometry>(*geometry) || SMSSameType<EllipsoidGeometry>(*geometry)) return Point2(1,0);
        if(SMSSameType<TorusGeometry>(*geometry)) return Point2(1,1);
        if(SMSSameType<CylinderGeometry>(*geometry)) {
            const Vector3 localNormal = Vector3Ops::Normalize(Vector3Ops::Transform(
                Matrix4Ops::Transpose(object.GetFinalTransformMatrix()),normal));
            return Point2(SMSDynamicCast<CylinderGeometry>(geometry)->TextureLongitudeIsPeriodic(localNormal) ? 1 : 0,0);
        }
        return Point2(0,0);
    }
    bool SMSConstantSeamMaterial(const RISE::IMaterial& material) {
        using namespace RISE;
        using namespace RISE::Implementation;
        if(const auto* m = SMSDynamicCast<PerfectReflectorMaterial>(&material)) {
            const IPainter& tint=m->GetReflectance();
            return SMSSameType<UniformColorPainter>(tint);
        }
        if(const auto* m = SMSDynamicCast<PerfectRefractorMaterial>(&material)) {
            const IPainter& tint=m->GetRefractivity();
            return SMSSameType<UniformColorPainter>(tint);
        }
        if(const auto* m = SMSDynamicCast<DielectricMaterial>(&material))
            return m->GetTransmittance().IsPositionIndependent() && m->GetScattering().IsPositionIndependent();
        if(const auto* m = SMSDynamicCast<PolishedMaterial>(&material))
            return m->GetTransmittance().IsPositionIndependent() && m->GetScattering().IsPositionIndependent();
        return false;
    }
    bool SMSContextIgnoresUV(const RISE::IObject& object,const RISE::IMaterial& material) {
        if(!SMSConstantSeamMaterial(material)) return false;
        const auto* modifier=object.GetModifier();
        if(!modifier) return true;
        const auto* audited=SMSDynamicCast<RISE::ISMSModifierDifferential>(modifier);
        return audited && audited->HasSMSDifferentialContract() && !audited->SMSFrameDependsOnUV();
    }
    void SMSCompleteNativeHit(RISE::RayIntersection& hit,const RISE::IObjectManager* objects) {
        if(const auto* native=SMSDynamicCast<RISE::Implementation::ObjectManager>(objects))
            native->CompleteShadingSignals(hit.geometric,hit.pObject);
    }
    struct SMSDeltaDirections {
        RISE::Vector3 reflected, transmitted, reflectionNormal, transmissionNormal;
        RISE::Scalar fresnelCosine=0;
        bool hasTransmission=false,totalInternalReflection=false,reflectionFallback=false,transmissionFallback=false;
    };
    // Native DL-111 event directions. Reflection changes its direction at
    // the geometric horizon; transmission fallback (only when ref < 1) changes BOTH lobe
    // prices to the geometric incidence. Reflection's normal alone does not
    // determine its Fresnel incidence.
    SMSDeltaDirections SMSNativeDirections(const RISE::Vector3& incoming,
        const RISE::Vector3& shading, const RISE::Vector3& geometric,
        RISE::Scalar etaI, RISE::Scalar etaT, bool transmission, bool rawFresnel=false,
        const RISE::IMaterial* material=nullptr,bool exiting=false,RISE::Scalar wavelength=550) {
        using namespace RISE;
        SMSDeltaDirections law;
        const Vector3 raw=Vector3Ops::SquaredModulus(geometric)>Scalar(1e-12)?geometric:shading;
        const Vector3 geom=Vector3Ops::Dot(raw,incoming)<0?raw:-raw;
        // Match Optics' native input normalization branch. Raw contexts remain
        // available to coating providers, whose native cosine uses raw W.
        const Vector3 eventNormal=std::fabs(Vector3Ops::Magnitude(shading)-1)>Scalar(1e-6)
            ? Vector3Ops::Normalize(shading):shading;
        law.reflectionNormal=eventNormal;law.transmissionNormal=eventNormal;
        law.fresnelCosine=std::fabs(Vector3Ops::Dot(incoming,rawFresnel?shading:eventNormal));
        law.reflected=Optics::CalculateReflectedRay(incoming,shading);
        if(Vector3Ops::Dot(law.reflected,geom)<=0) {
            law.reflectionFallback=true;
            law.reflectionNormal=geom;
            law.reflected=Optics::CalculateReflectedRay(incoming,geom);
        }
        law.transmitted=incoming;
        if(transmission) {
            law.hasTransmission=Optics::CalculateRefractedRay(shading,etaI,etaT,law.transmitted);
            law.totalInternalReflection=!law.hasTransmission;
            bool fallbackAllowed=true;
            if(law.hasTransmission && (SMSDynamicCast<Implementation::DielectricMaterial>(material)
                    || SMSDynamicCast<Implementation::PerfectRefractorMaterial>(material))) {
                // Native dielectric/refractor SPFs gate their DL-111 fallback on ref < 1,
                // before changing the incidence. Film saturation is not TIR:
                // retain proposal exploration mass, but reject this zero lobe.
                Scalar reflectance=Optics::CalculateDielectricReflectanceCosine(
                    std::fabs(Vector3Ops::Dot(incoming,eventNormal)),etaI,etaT);
                const auto* dielectric=SMSDynamicCast<Implementation::DielectricSPF>(material->GetSPF());
                if(dielectric && dielectric->EvaluateSpecularFresnelAfterRefraction(
                    std::fabs(Vector3Ops::Dot(incoming,shading)),etaI,etaT,exiting,wavelength,reflectance))
                    law.fresnelCosine=std::fabs(Vector3Ops::Dot(incoming,shading));
                fallbackAllowed=reflectance<1;
                if(reflectance>=1) law.hasTransmission=false;
            }
            if(law.hasTransmission && fallbackAllowed && Vector3Ops::Dot(law.transmitted,-geom)<=0) {
                law.transmissionFallback=true;
                law.transmissionNormal=geom;
                law.fresnelCosine=std::fabs(Vector3Ops::Dot(incoming,geom));
                law.transmitted=incoming;
                law.hasTransmission=Optics::CalculateRefractedRay(geom,etaI,etaT,law.transmitted);
                law.totalInternalReflection=!law.hasTransmission;
                if(law.hasTransmission && (SMSDynamicCast<Implementation::DielectricMaterial>(material)
                    || SMSDynamicCast<Implementation::PerfectRefractorMaterial>(material))) {
                    Scalar reflectance=Optics::CalculateDielectricReflectanceCosine(law.fresnelCosine,etaI,etaT);
                    if(const auto* dielectric=SMSDynamicCast<Implementation::DielectricSPF>(material->GetSPF()))
                        dielectric->EvaluateSpecularFresnelAfterRefraction(law.fresnelCosine,etaI,etaT,exiting,wavelength,reflectance);
                    if(reflectance>=1) law.hasTransmission=false;
                }
            }
        }
        return law;
    }
    RISE::Scalar SMSDomainWavelength(RISE::Implementation::SMSQueryDomain domain) {
        if(!domain.Valid()) return 550; // The subsequent replay declines invalid domains.
        return domain.kind==RISE::Implementation::SMSQueryDomain::Wavelength?domain.nm
            :RISE::ScalarPainterRGB::kChannelNM[domain.component];
    }
    // These native SPFs read the modified ONB, whereas PolishedBRDF
    // resolves its coat from normalized vNormal. Preserve both raw records
    // in the context and normalize only the native consumed event field.
    RISE::Vector3 SMSNativeEventNormal(const RISE::IMaterial& material,
        const RISE::RayIntersectionGeometric& hit) {
        return SMSDynamicCast<RISE::Implementation::PolishedMaterial>(&material)
            ? RISE::Vector3Ops::Normalize(hit.vNormal) : hit.onb.w();
    }
    bool SMSNeedsNativeFrame(const RISE::Implementation::ManifoldVertex& v) {
        return v.pObject && (v.pObject->GetModifier()
            || RISE::Vector3Ops::SquaredModulus(RISE::Vector3Ops::Cross(v.normal,v.geomNormal))
                > std::numeric_limits<RISE::Scalar>::epsilon());
    }
    RISE::Point3 SMSReferenceSurfacePoint(const RISE::IObject&,const RISE::RayIntersectionGeometric&);
    RISE::Implementation::ManifoldVertex SMSNativeConstraintVertex(
        const RISE::Implementation::ManifoldVertex& original,const RISE::Point3& previous,
        const RISE::RasterizerState& raster=RISE::nullRasterizerState,unsigned* branch=nullptr,
        const RISE::IObjectManager* objects=nullptr,RISE::Scalar wavelength=550) {
        using namespace RISE;
        auto v=original;
        // This local flag describes native event/context validity, not the
        // seed's derivative readiness (derivatives are computed after the
        // initial constraint check). Unmodified seeds already carry the frame.
        v.valid=true;
        if(SMSNeedsNativeFrame(v)) {
            RayIntersection hit(Ray(previous,Vector3Ops::Normalize(Vector3Ops::mkVector3(v.position,previous))),raster);
            v.pObject->IntersectRay(hit,RISE_INFINITY,true,true,false);
            SMSCompleteNativeHit(hit,objects);
            const Scalar tolerance=std::sqrt(std::numeric_limits<Scalar>::epsilon())*Point3Ops::Distance(previous,v.position);
            if(!hit.geometric.bHit || hit.pMaterial!=v.pMaterial
                || Point3Ops::Distance(SMSReferenceSurfacePoint(*v.pObject,hit.geometric),v.position)>tolerance) {
                v.valid=false;return v;
            }
            if(hit.pModifier) hit.pModifier->Modify(hit.geometric);
            v.normal=SMSNativeEventNormal(*v.pMaterial,hit.geometric);v.geomNormal=hit.geometric.UnflippedGeomNormal();v.valid=true;
        }
        const auto law=SMSNativeDirections(Vector3Ops::Normalize(Vector3Ops::mkVector3(v.position,previous)),
            v.normal,v.geomNormal,v.etaI,v.etaT,v.canRefract && v.pMaterial
                && !SMSSameType<Implementation::PolishedMaterial>(*v.pMaterial),false,v.pMaterial,v.isExiting,wavelength);
        if(!v.isReflection && !law.hasTransmission) v.valid=false;
        if(branch) *branch=(v.isReflection?unsigned(law.reflectionFallback)
            :(unsigned(law.transmissionFallback)<<1)|(unsigned(law.hasTransmission)<<2)
                |(unsigned(law.totalInternalReflection)<<4))
            |(unsigned(std::fabs(Vector3Ops::Magnitude(v.normal)-1)>Scalar(1e-6))<<3);
        v.normal=v.isReflection?law.reflectionNormal:law.transmissionNormal;
        return v;
    }

    // Native Optics tolerates slightly nonunit event normals. Its outgoing
    // vector then need not have unit length. Matching a unit outgoing ray
    // requires this length in the generalized half vector, including its
    // derivative; normalizing the normal would change the native event.
    bool SMSNativeHalfVector(const RISE::Vector3& wi,const RISE::Vector3& wo,
        const RISE::Vector3& normal,RISE::Scalar etaI,RISE::Scalar etaT,bool reflection,
        const RISE::Vector3& dwi,const RISE::Vector3& dwo,const RISE::Vector3& dn,
        RISE::Vector3& half,RISE::Vector3& derivative) {
        using namespace RISE;
        Vector3 direction,delta;
        if(reflection) {
            const Scalar cosine=Vector3Ops::Dot(normal,wi);
            const Scalar dc=Vector3Ops::Dot(dn,wi)+Vector3Ops::Dot(normal,dwi);
            direction=Optics::CalculateReflectedRay(-wi,normal);
            delta=-dwi+(normal*dc+dn*cosine)*2;
        } else {
            direction=-wi;
            if(!Optics::CalculateRefractedRay(normal,etaI,etaT,direction)) return false;
            if(etaI==etaT) delta=-dwi;
            else {
                const bool flip=Vector3Ops::Dot(normal,wi)<0;
                const Vector3 n=flip?-normal:normal,dnOriented=flip?-dn:dn;
                const Scalar cosine=Vector3Ops::Dot(n,wi);
                const Scalar dc=Vector3Ops::Dot(dnOriented,wi)+Vector3Ops::Dot(n,dwi);
                Scalar cosT;
                if(!Optics::CalculateRefractedCosine(cosine,etaI,etaT,cosT) || !(cosT>0)) return false;
                const Scalar ratio=etaI/etaT;
                const Scalar dcosT=cosine<1?ratio*ratio*cosine*dc/cosT:0;
                delta=-dwi*ratio+n*(ratio*dc-dcosT)+dnOriented*(ratio*cosine-cosT);
            }
        }
        const Scalar length=Vector3Ops::Magnitude(direction);
        if(!(length>0) || !std::isfinite(length)) return false;
        const Scalar dlength=Vector3Ops::Dot(direction,delta)/length;
        half=reflection?wi+wo*length:-(wi*etaI+wo*(etaT*length));
        derivative=reflection?dwi+dwo*length+wo*dlength
            :-(dwi*etaI+(dwo*length+wo*dlength)*etaT);
        return std::isfinite(dlength);
    }

    bool SMSAuditedModifier(const RISE::IObject& object) {
        const auto* modifier=object.GetModifier();
        const auto* differential=SMSDynamicCast<RISE::ISMSModifierDifferential>(modifier);
        if(!modifier) return true;
        if(!differential || !differential->HasSMSDifferentialContract()) return false;
        if(!differential->SMSFrameDependsOnUV()) return true;
        const auto* native=SMSDynamicCast<RISE::Implementation::Object>(&object);
        if(!native) return false;
        const auto* generator=native->SMSUVGenerator();
        if(!generator) return true; // Audited native geometry chart.
        const auto* uv=SMSDynamicCast<RISE::ISMSUVDifferential>(generator);
        return uv && uv->HasSMSUVDifferentialContract();
    }
    RISE::Vector3 SMSNormalizedDifferential(const RISE::Vector3& value,
        const RISE::Vector3& derivative) {
        const RISE::Scalar length=RISE::Vector3Ops::Magnitude(value);
        if(!(length>0)) return RISE::Vector3(std::numeric_limits<RISE::Scalar>::quiet_NaN(),0,0);
        const auto unit=value*(1/length);
        return (derivative-unit*RISE::Vector3Ops::Dot(unit,derivative))*(1/length);
    }
    // Differentiate the residual using the audited modifier differential.
    // Probe differences describe only the native geometry/input transport;
    // no difference of post-modifier values prices the Jacobian.
    bool SMSNativeConstraintDifferential(const RISE::Implementation::ManifoldVertex& center,
        const RISE::Implementation::ManifoldVertex& plus,
        const RISE::Implementation::ManifoldVertex& minus,
        const RISE::Point3& previous,const RISE::Point3& previousPlus,const RISE::Point3& previousMinus,
        const RISE::Point3& next,const RISE::Point3& nextPlus,const RISE::Point3& nextMinus,
        RISE::Scalar step,const RISE::RasterizerState& raster,RISE::Scalar& c0,RISE::Scalar& c1,
        const RISE::IObjectManager* objects=nullptr,RISE::Scalar wavelength=550) {
        using namespace RISE;
        if(!(step>0) || !center.pObject || !SMSAuditedModifier(*center.pObject)) return false;
        const auto difference=[&](const auto& a,const auto& b) {return Vector3(a.x-b.x,a.y-b.y,a.z-b.z)*(1/(2*step));};
        unsigned branch=0,plusBranch=0,minusBranch=0;
        const auto effective=SMSNativeConstraintVertex(center,previous,raster,&branch,objects,wavelength);
        const auto ep=SMSNativeConstraintVertex(plus,previousPlus,raster,&plusBranch,objects,wavelength);
        const auto em=SMSNativeConstraintVertex(minus,previousMinus,raster,&minusBranch,objects,wavelength);
        if(!effective.valid || !ep.valid || !em.valid || branch!=plusBranch || branch!=minusBranch) return false;
        Vector3 dn=difference(plus.normal,minus.normal);
        Vector3 dg=difference(plus.geomNormal,minus.geomNormal);
        if(SMSNeedsNativeFrame(center)) {
            RayIntersection raw(Ray(previous,Vector3Ops::Normalize(Vector3Ops::mkVector3(center.position,previous))),raster);
            RayIntersection rp(Ray(previousPlus,Vector3Ops::Normalize(Vector3Ops::mkVector3(plus.position,previousPlus))),raster);
            RayIntersection rm(Ray(previousMinus,Vector3Ops::Normalize(Vector3Ops::mkVector3(minus.position,previousMinus))),raster);
            center.pObject->IntersectRay(raw,RISE_INFINITY,true,true,false);
            center.pObject->IntersectRay(rp,RISE_INFINITY,true,true,false);
            center.pObject->IntersectRay(rm,RISE_INFINITY,true,true,false);
            SMSCompleteNativeHit(raw,objects);SMSCompleteNativeHit(rp,objects);SMSCompleteNativeHit(rm,objects);
            if(!raw.geometric.bHit || !rp.geometric.bHit || !rm.geometric.bHit) return false;
            const auto* geometry=center.pObject->GetGeometry();
            const bool nativeMesh=SMSDynamicCast<Implementation::TriangleMeshGeometryIndexed>(raw.geometric.signals.pProvider)
                || (geometry && (SMSSameType<Implementation::TriangleMeshGeometryIndexed>(*geometry)
                    || SMSSameType<Implementation::TriangleMeshGeometry>(*geometry)));
            if(nativeMesh) {
                const auto& a=raw.geometric.signals;const auto& b=rp.geometric.signals;const auto& c=rm.geometric.signals;
                const bool samePrimitive=a.primId>=0 && a.primId==b.primId && a.primId==c.primId
                    && a.pProvider==b.pProvider && a.pProvider==c.pProvider;
                if(!samePrimitive) {
                    // Crossing triangles cannot establish a local derivative by
                    // agreement of distant probes. A constant native frame is
                    // the exception: zero primitive derivatives and identical
                    // sampled frames establish the same flat input transport.
                    // Native UV charts can change independently of a flat
                    // normal. Only an audited generated mapping supplies its
                    // own derivative across a primitive boundary.
                    const auto* modifier=SMSDynamicCast<ISMSModifierDifferential>(raw.pModifier);
                    const auto* native=SMSDynamicCast<Implementation::Object>(center.pObject);
                    if(modifier && modifier->SMSFrameDependsOnUV()
                        && (!native || !native->SMSUVGenerator())) return false;
                    const auto flat=[](const RayIntersectionGeometric& hit) {
                        return hit.derivatives.valid && Vector3Ops::SquaredModulus(hit.derivatives.dndu)==0
                            && Vector3Ops::SquaredModulus(hit.derivatives.dndv)==0;
                    };
                    const auto same=[](const Vector3& x,const Vector3& y) {
                        return x.x==y.x && x.y==y.y && x.z==y.z;
                    };
                    const auto sameFrame=[&](const RayIntersectionGeometric& hit) {
                        return same(hit.vNormal,raw.geometric.vNormal)
                            && same(hit.UnflippedGeomNormal(),raw.geometric.UnflippedGeomNormal())
                            && same(hit.onb.u(),raw.geometric.onb.u()) && same(hit.onb.v(),raw.geometric.onb.v())
                            && same(hit.onb.w(),raw.geometric.onb.w());
                    };
                    if(!flat(raw.geometric) || !flat(rp.geometric) || !flat(rm.geometric)
                        || !sameFrame(rp.geometric) || !sameFrame(rm.geometric)) return false;
                }
            }
            SMSIntersectionDifferential input;
            input.worldPoint=difference(rp.geometric.ptIntersection,rm.geometric.ptIntersection);
            input.objectPoint=difference(rp.geometric.ptObjIntersec,rm.geometric.ptObjIntersec);
            input.normal=difference(rp.geometric.vNormal,rm.geometric.vNormal);
            input.geometricNormal=difference(rp.geometric.UnflippedGeomNormal(),rm.geometric.UnflippedGeomNormal());
            input.frameU=difference(rp.geometric.onb.u(),rm.geometric.onb.u());
            input.frameV=difference(rp.geometric.onb.v(),rm.geometric.onb.v());
            input.frameW=difference(rp.geometric.onb.w(),rm.geometric.onb.w());
            input.rayOrigin=difference(previousPlus,previousMinus);
            input.rayDirection=difference(rp.geometric.ray.Dir(),rm.geometric.ray.Dir());
            input.uv=Point2((rp.geometric.ptCoord.x-rm.geometric.ptCoord.x)/(2*step),
                (rp.geometric.ptCoord.y-rm.geometric.ptCoord.y)/(2*step));
            Vector3 normal=input.normal,frameW=input.frameW;
            if(raw.pModifier) {
                const auto* provider=SMSDynamicCast<ISMSModifierDifferential>(raw.pModifier);
                if(!provider) return false;
                if(provider->SMSFrameDependsOnUV()) {
                    const auto* native=SMSDynamicCast<Implementation::Object>(center.pObject);
                    if(!native) return false;
                    if(const auto* generator=native->SMSUVGenerator()) {
                        const auto* uv=SMSDynamicCast<ISMSUVDifferential>(generator);
                        // Object::IntersectRay generates UVs before promoting
                        // normals to world space. Reconstruct that exact input
                        // convention, including the normalization chain rule.
                        const Matrix4 normalToObject=Matrix4Ops::Transpose(native->GetFinalTransformMatrix());
                        const Vector3 localNormalValue=Vector3Ops::Transform(normalToObject,raw.geometric.UnflippedGeomNormal());
                        const Vector3 localNormalDerivative=SMSNormalizedDifferential(localNormalValue,
                            Vector3Ops::Transform(normalToObject,input.geometricNormal));
                        if(!uv || !uv->HasSMSUVDifferentialContract()
                            || !uv->SMSUVDifferential(raw.geometric.ptObjIntersec,
                                Vector3Ops::Normalize(localNormalValue),input.objectPoint,
                                localNormalDerivative,input.uv)) return false;
                    }
                } else input.uv=Point2(0,0);
                if(!provider->SMSFrameDifferential(raw.geometric,input,normal,frameW)) return false;
            }
            auto modified=raw.geometric;
            if(raw.pModifier) raw.pModifier->Modify(modified);
            if(SMSDynamicCast<Implementation::PolishedMaterial>(center.pMaterial)) {
                dn=SMSNormalizedDifferential(modified.vNormal,normal);
            } else {
                dn=std::fabs(Vector3Ops::Magnitude(modified.onb.w())-1)>Scalar(1e-6)
                    ? SMSNormalizedDifferential(modified.onb.w(),frameW):frameW;
            }
            dg=input.geometricNormal;
        }
        const bool fallback=center.isReflection?(branch&1):(branch&2);
        if(fallback) {
            const Vector3 incoming=Vector3Ops::Normalize(Vector3Ops::mkVector3(center.position,previous));
            dn=Vector3Ops::Dot(center.geomNormal,incoming)<0?dg:-dg;
        }
        const Vector3 dx=difference(plus.position,minus.position);
        const Vector3 dp=difference(previousPlus,previousMinus),dy=difference(nextPlus,nextMinus);
        const Vector3 vi=Vector3Ops::mkVector3(previous,center.position),vo=Vector3Ops::mkVector3(next,center.position);
        const Vector3 wi=Vector3Ops::Normalize(vi),wo=Vector3Ops::Normalize(vo);
        const Vector3 dwi=SMSNormalizedDifferential(vi,dp-dx),dwo=SMSNormalizedDifferential(vo,dy-dx);
        Vector3 h=wi+wo,dh=dwi+dwo;
        bool rawHalf=false;
        if(!center.isReflection) {
            const Scalar ei=center.etaI,et=center.etaT;
            h=-(wi*ei+wo*et);dh=-(dwi*ei+dwo*et);
            // Native replay populated both physical etas. The same residual
            // convention uses unnormalized half vectors for refraction.
            rawHalf=true;
        }
        if(!SMSNativeHalfVector(wi,wo,effective.normal,center.etaI,center.etaT,
            center.isReflection,dwi,dwo,dn,h,dh)) return false;
        if(!rawHalf) {dh=SMSNormalizedDifferential(h,dh);h=Vector3Ops::Normalize(h);}
        // Projection bases are unit directions; the unnormalized native
        // normal remains in the event equation above.
        dn=SMSNormalizedDifferential(effective.normal,dn);
        const Vector3 n=Vector3Ops::Normalize(effective.normal),u=center.dpdu,du=difference(plus.dpdu,minus.dpdu);
        const Scalar dot=Vector3Ops::Dot(u,n);
        const Vector3 tangent=u-n*dot;
        if(Vector3Ops::Magnitude(tangent)<=NEARZERO) return false;
        const Vector3 dtangent=du-dn*dot-n*(Vector3Ops::Dot(du,n)+Vector3Ops::Dot(u,dn));
        const Vector3 a=Vector3Ops::Normalize(tangent),da=SMSNormalizedDifferential(tangent,dtangent);
        const Vector3 b=Vector3Ops::Cross(n,a),db=Vector3Ops::Cross(dn,a)+Vector3Ops::Cross(n,da);
        c0=Vector3Ops::Dot(da,h)+Vector3Ops::Dot(a,dh);
        c1=Vector3Ops::Dot(db,h)+Vector3Ops::Dot(b,dh);
        return std::isfinite(c0)&&std::isfinite(c1);
    }
    // Object stores a launch point backed off in object space. Newton and
    // Jacobian pricing use the actual surface; material contexts retain the
    // native hit record. Undo the KNOWN native convention only in reference
    // solves, rather than widening their residual or matching tolerances.
    RISE::Point3 SMSReferenceSurfacePoint(const RISE::IObject& object,
        const RISE::RayIntersectionGeometric& hit) {
        using namespace RISE;
        const auto* native = SMSDynamicCast<Implementation::Object>(&object);
        if(!native) return hit.ptIntersection;
        const Vector3 localDirection = Vector3Ops::Normalize(Vector3Ops::Transform(
            object.GetFinalInverseTransformMatrix(),hit.ray.Dir()));
        return Point3Ops::Transform(object.GetFinalTransformMatrix(),
            Point3Ops::mkPoint3(hit.ptObjIntersec,localDirection*native->GetSurfaceIntersecError()));
    }
}

bool RISE::Implementation::SMSQueryDomain::Valid() const
{
    return kind == RGBComponent ? component < 3
        : kind == Wavelength && std::isfinite(nm) && nm > 0;
}

bool RISE::Implementation::SMSDomainReplay::PotentialCaster(const IMaterial& material)
{
    if(!SMSSameType<DielectricMaterial>(material) && !SMSSameType<PerfectRefractorMaterial>(material)
        && !SMSSameType<PolishedMaterial>(material) && !SMSSameType<PerfectReflectorMaterial>(material)) return false;
    const IScalarPainter* index = nullptr;
    if(const auto* m = SMSDynamicCast<DielectricMaterial>(&material)) index = &m->GetIOR();
    else if(const auto* m = SMSDynamicCast<PerfectRefractorMaterial>(&material)) index = &m->GetIOR();
    else if(const auto* m = SMSDynamicCast<PolishedMaterial>(&material)) index = &m->GetIOR();
    else return SMSDynamicCast<PerfectReflectorMaterial>(&material) != nullptr;
    return index->IsPositionIndependent();
}

RISE::Scalar RISE::Implementation::SMSRootReference::Deposit(Scalar physicalContribution,
    Scalar reciprocal, Scalar channelProbability, Scalar emitterProbability, unsigned int originalTrials)
{
    if(!std::isfinite(physicalContribution) || !std::isfinite(reciprocal)
        || reciprocal < 1 || !originalTrials
        || !std::isfinite(channelProbability) || channelProbability <= 0
        || !std::isfinite(emitterProbability) || emitterProbability <= 0) return 0;
    return physicalContribution * reciprocal / (channelProbability * emitterProbability * originalTrials);
}

bool RISE::Implementation::SMSDomainReplay::Query(const IMaterial& material,
    const RayIntersectionGeometric& hit, const IORStack& stack,
    SMSQueryDomain domain, SMSNativeMaterialQuery& result)
{
    if(smsActiveDiagnostics) smsActiveDiagnostics->materialQueries.fetch_add(1,std::memory_order_relaxed);
    result = SMSNativeMaterialQuery();
    if(!domain.Valid() || !PotentialCaster(material)) return false;
    const IScalarPainter* index = nullptr;
    const IScalarPainter* scattering = nullptr;
    const IScalarPainter* coatTint = nullptr;
    bool hg = false;
    bool finiteDielectric = false;
    bool polishedReflectionOnly = false;
    if(const auto* dielectric = SMSDynamicCast<DielectricMaterial>(&material)) {
        index = &dielectric->GetIOR();
        scattering = &dielectric->GetScattering();
        hg = dielectric->GetHG();
        finiteDielectric = true;
    }
    else if(const auto* refractor = SMSDynamicCast<PerfectRefractorMaterial>(&material))
        index = &refractor->GetIOR();
    else if(const auto* polished = SMSDynamicCast<PolishedMaterial>(&material)) {
        index = &polished->GetIOR();
        scattering = &polished->GetScattering();
        hg = polished->GetHG();
        coatTint = &polished->GetTransmittance();
        polishedReflectionOnly = true; // the substrate has no clear delta transmission
    }
    else if(!SMSDynamicCast<PerfectReflectorMaterial>(&material))
        return false;
    // A successful point query does not certify constant IOR throughout a root basin.
    if(index && !index->IsPositionIndependent()) return false;
    const SpecularInfo info = domain.kind == SMSQueryDomain::RGBComponent
        ? material.GetSpecularInfo(hit, stack)
        : material.GetSpecularInfoNM(hit, stack, domain.nm);
    const Scalar s = scattering ? (domain.kind == SMSQueryDomain::RGBComponent
        ? scattering->GetValuesAt(hit)[domain.component] : scattering->GetValueAtNM(hit, domain.nm)) : 0;
    // Finite-Phong dielectrics remain eligible at their adopted delta
    // limit. Polished's finite coat is genuinely glossy and has no
    // delta-tagged transmission; it needs the native exact-delta limit.
    if(!info.valid || (scattering ? (!std::isfinite(s)
        || !(hg ? s >= 1 : finiteDielectric ? s > -1 : s >= 1000000)) : !info.isSpecular)) return false;
    result.index = index ? (domain.kind == SMSQueryDomain::RGBComponent
        ? index->GetValuesAt(hit)[domain.component] : index->GetValueAtNM(hit, domain.nm)) : info.ior;
    result.attenuation = domain.kind == SMSQueryDomain::RGBComponent
        ? info.attenuation[domain.component] : info.attenuationNM;
    if(coatTint) result.attenuation = domain.kind == SMSQueryDomain::RGBComponent
        ? coatTint->GetValuesAt(hit)[domain.component] : coatTint->GetValueAtNM(hit, domain.nm);
    if(polishedReflectionOnly) {
        // Match PolishedBRDF::Resolve before advertising the delta coat.
        // Its side convention uses the reported geometric normal, while
        // its incident support uses the corresponding shading normal.
        const bool backface=Vector3Ops::Dot(hit.vGeomNormal,hit.ray.Dir())>0;
        const Vector3 normal=Vector3Ops::Normalize(backface?-hit.vNormal:hit.vNormal);
        if(!(Vector3Ops::Dot(Vector3Ops::Normalize(-hit.ray.Dir()),normal)>0)) return false;
    }
    result.reflection = true;
    result.transmission = info.canRefract && !polishedReflectionOnly;
    result.dielectricInterface = index != nullptr;
    result.deltaLimitProxy = finiteDielectric && !hg && s < 1000000;
    result.interiorTransmittance = info.attenuationIsInteriorTransmittance;
    result.reflectionTint = info.attenuationAppliesToReflection;
    result.customFresnel = info.hasCustomSpecularFresnel;
    return std::isfinite(result.index) && result.index > 0
        && std::isfinite(result.attenuation) && result.attenuation >= 0;
}

namespace
{
    bool SMSCaptureUncached(const RISE::IScene& scene,const RISE::Point3& anchor,
        const RISE::IORStack& live,RISE::Implementation::SMSStartingMedia& result);
}

bool RISE::Implementation::SMSDomainReplay::Capture(const IScene& scene,
    const Point3& anchor, const IORStack& live, SMSStartingMedia& result)
{
    SMSCaptureMemo* memo=smsCaptureMemo;
    if(!memo) return SMSCaptureUncached(scene,anchor,live,result);
    const Scalar environment=live.EnvironmentIOR();
    const std::vector<const IObject*> keys=live.ObjectKeys();
    for(const auto& entry:memo->entries)
        if(entry.scene==&scene && !std::memcmp(&entry.anchor,&anchor,sizeof(Point3))
            && !std::memcmp(&entry.environment,&environment,sizeof(Scalar)) && entry.keys==keys) {
            result=entry.media;
            return entry.captured;
        }
    const bool captured=SMSCaptureUncached(scene,anchor,live,result);
    if(memo->entries.size()<SMSCaptureMemo::kEntries) {
        SMSCaptureMemo::Entry entry;
        entry.scene=&scene;entry.anchor=anchor;entry.environment=environment;
        entry.keys=keys;entry.captured=captured;entry.media=result;
        memo->entries.push_back(std::move(entry));
    }
    return captured;
}

namespace
{
bool SMSCaptureUncached(const RISE::IScene& scene,
    const RISE::Point3& anchor, const RISE::IORStack& live, RISE::Implementation::SMSStartingMedia& result)
{
    using namespace RISE;
    using namespace RISE::Implementation;
    result = SMSStartingMedia();
    result.environmentIndex = live.EnvironmentIOR();
    if(!std::isfinite(result.environmentIndex) || result.environmentIndex <= 0
        || !scene.GetObjects() || scene.GetGlobalMedium()) return false;
    const auto* preparedObjects = SMSDynamicCast<ObjectManager>(scene.GetObjects());
    if(!preparedObjects || !preparedObjects->ExtendedSMSAllowed()
        || preparedObjects->HasUncertainSMSNormalOrientation()) return false;
    struct Objects : IEnumCallback<IObject> {
        std::vector<const IObject*> items;
        bool operator()(const IObject& object) override { items.push_back(&object); return true; }
    } objects;
    scene.GetObjects()->EnumerateObjects(objects);
    const Vector3 directions[] = {Vector3(1,0,0), Vector3(-1,0,0),
        Vector3(0,1,0), Vector3(0,-1,0), Vector3(0,0,1), Vector3(0,0,-1)};
    const std::vector<const IObject*> keys = live.ObjectKeys();
    IORStack geometricMembership(live.EnvironmentIOR());
    IORStackSeeding::SeedFromPoint(geometricMembership, anchor, scene);
    for(const IObject* required : geometricMembership.ObjectKeys())
        if(std::find(keys.begin(), keys.end(), required) == keys.end()) return false;
    // Live open-sheet membership can legitimately exceed geometric containment;
    // preserve it rather than replacing the walk's state with a fresh seed.
    // Participating local media are excluded, including uncertain containment
    // when their boundary does not provide reconstructible IOR membership.
    // Composite scenes already declined through the prepared policy above.
    // Bounds conservatively reject uncertain participating-medium containment.
    for(const IObject* object : objects.items) {
        if(!object->IsWorldVisible()
            || !object->GetInteriorMedium()) continue;
        if(std::find(keys.begin(), keys.end(), object) != keys.end()) return false;
        const BoundingBox bounds = object->getBoundingBox();
        const bool outside = anchor.x < bounds.ll.x || anchor.x > bounds.ur.x
            || anchor.y < bounds.ll.y || anchor.y > bounds.ur.y
            || anchor.z < bounds.ll.z || anchor.z > bounds.ur.z;
        if(!outside) return false;
    }
    for(const IObject* key : keys) {
        if(std::find(objects.items.begin(), objects.items.end(), key) == objects.items.end())
            return false; // opaque non-scene identities must never be dereferenced
        if(!key->GetMaterial() || key->GetInteriorMedium()) return false;
        bool captured = false;
        for(const Vector3& direction : directions) {
            RayIntersection hit(Ray(anchor, direction), nullRasterizerState);
            key->IntersectRay(hit, RISE_INFINITY, true, true, false);
            SMSCompleteNativeHit(hit,scene.GetObjects());
            if(!hit.geometric.bHit) continue;
            if(hit.pModifier) hit.pModifier->Modify(hit.geometric);
            result.enclosing.emplace_back(key, hit.pMaterial, hit.geometric);
            captured = true;
            break;
        }
        if(!captured) return false;
    }
    result.reconstructible = true;
    return true;
}

}

bool RISE::Implementation::SMSDomainReplay::BuildStack(const SMSStartingMedia& media,
    SMSQueryDomain domain, IORStack& result)
{
    if(!media.reconstructible || !domain.Valid()
        || !std::isfinite(media.environmentIndex) || media.environmentIndex <= 0) return false;
    IORStack replay(media.environmentIndex);
    for(const SMSMediumCapture& entry : media.enclosing) {
        if(!entry.identity || !entry.material || entry.identity->GetInteriorMedium()) return false;
        replay.SetCurrentObject(entry.identity);
        if(replay.containsCurrent()) return false;
        SMSNativeMaterialQuery query;
        if(!Query(*entry.material, entry.context, replay, domain, query)
            || !query.transmission) return false;
        replay.push(query.index);
    }
    result = replay;
    return true;
}

bool RISE::Implementation::SMSDomainReplay::Cross(const IMaterial& material,
    const IObject* identity, const RayIntersectionGeometric& hit,
    SMSQueryDomain domain, bool reflection, IORStack& stack,
    Scalar& etaI, Scalar& etaT, bool& exiting)
{
    if(!identity || identity->GetInteriorMedium()) return false;
    IORStack next(stack);
    next.SetCurrentObject(identity);
    SMSNativeMaterialQuery query;
    if(!Query(material, hit, next, domain, query)
        || (reflection ? !query.reflection : !query.transmission)) return false;
    if(!std::isfinite(next.top()) || next.top() <= 0) return false;
    if(SMSDynamicCast<PolishedMaterial>(&material)) {
        // The coat is a reflection on the incident ambient side, not a
        // transmissive open-sheet crossing. Native polished never pops
        // membership or reverses ambient/coat indices on a back face.
        exiting=false;etaI=next.top();etaT=query.index;
    } else if(hit.bProvablyNoInterior) {
        const auto crossing = IORStackSeeding::ResolveOpenSheetCrossing(hit, query.index, next);
        etaI = crossing.etaFrom;
        etaT = crossing.etaTo;
        exiting = !crossing.bEntering;
    } else {
        exiting = next.containsCurrent();
        // Native closed-solid SPFs use the exiting object index even
        // when pop removes a non-top identity in overlapping volumes.
        etaI = exiting ? query.index : next.top();
        if(exiting) next.pop();
        etaT = exiting ? next.top() : query.index;
        if(!reflection && !exiting) next.push(query.index);
    }
    if(!std::isfinite(etaI) || etaI <= 0 || !std::isfinite(etaT) || etaT <= 0) return false;
    if(!reflection && !SMSNativeDirections(hit.ray.Dir(),SMSNativeEventNormal(material,hit),hit.UnflippedGeomNormal(),
        etaI,etaT,true,query.customFresnel,&material,exiting,SMSDomainWavelength(domain)).hasTransmission)
        return false; // impossible transmission is a zero trial, never relabeled
    if(!reflection) stack = next;
    return true;
}

bool RISE::Implementation::SMSDomainReplay::EventWeight(const IMaterial& material,
    const RayIntersectionGeometric& hit, const IORStack& stack, SMSQueryDomain domain,
    bool reflection, bool exiting, Scalar etaI, Scalar etaT, Scalar distance, Scalar& weight)
{
    weight = 0;
    SMSNativeMaterialQuery query;
    if(!Query(material, hit, stack, domain, query) || !std::isfinite(distance) || distance < 0
        || !std::isfinite(etaI) || etaI <= 0 || !std::isfinite(etaT) || etaT <= 0
        || (reflection ? !query.reflection : !query.transmission)) return false;
    const auto law=SMSNativeDirections(hit.ray.Dir(),SMSNativeEventNormal(material,hit),hit.UnflippedGeomNormal(),etaI,etaT,query.transmission,query.customFresnel,&material,exiting,SMSDomainWavelength(domain));
    const Scalar cosine=law.fresnelCosine;
    Scalar fresnel = 1;
    if(query.dielectricInterface) {
        fresnel = query.transmission && !law.hasTransmission ? Scalar(1)
            : Optics::CalculateDielectricReflectanceCosine(cosine, etaI, etaT);
        const Scalar wavelength = domain.kind == SMSQueryDomain::Wavelength ? domain.nm
            : ScalarPainterRGB::kChannelNM[domain.component];
        if(query.customFresnel && !(query.transmission && !law.hasTransmission)) {
            const auto* dielectric=SMSDynamicCast<DielectricSPF>(material.GetSPF());
            if(!dielectric || !dielectric->EvaluateSpecularFresnelAfterRefraction(cosine,etaI,etaT,exiting,wavelength,fresnel))
                return false;
        }
    }
    Scalar attenuation = query.attenuation;
    if(reflection && !query.reflectionTint) attenuation = 1;
    if(query.interiorTransmittance)
        attenuation = exiting && !reflection ? std::pow(attenuation, distance) : Scalar(1);
    // SPF direction/Fresnel endpoints and the native consumer radiance
    // scale differ at a non-top closed exit. Reproduce both conventions;
    // do not alter the shared native walker to hide the distinction.
    const Scalar radianceEtaI = hit.bProvablyNoInterior ? etaI : stack.top();
    const Scalar etaRatio = radianceEtaI/etaT;
    weight = attenuation * (reflection ? fresnel : (1-fresnel)*etaRatio*etaRatio);
    return std::isfinite(weight) && weight >= 0;
}

RISE::Implementation::ManifoldResult RISE::Implementation::ManifoldSolver::SolveDomain(
    const Point3& start, const Vector3& startNormal, const Point3& end, const Vector3& endNormal,
    const IScene& scene, const IORStack& startingStack, SMSQueryDomain domain,
    std::vector<SMSDomainVertex>& vertices, ISampler& sampler, Scalar positionTolerance, Scalar convergenceThreshold) const
{
    ManifoldSolver native(config,true,&vertices,SMSDomainWavelength(domain));
    return native.SolveDomainCore(start,startNormal,end,endNormal,scene,startingStack,domain,
        vertices,sampler,positionTolerance,convergenceThreshold);
}

RISE::Implementation::ManifoldResult RISE::Implementation::ManifoldSolver::SolveDomainCore(
    const Point3& start, const Vector3& startNormal, const Point3& end, const Vector3& endNormal,
    const IScene& scene, const IORStack& startingStack, SMSQueryDomain domain,
    std::vector<SMSDomainVertex>& vertices, ISampler& sampler, Scalar positionTolerance, Scalar convergenceThreshold) const
{
    ManifoldResult result;
    SolveDomainCoreInto(start,startNormal,end,endNormal,scene,startingStack,domain,vertices,
        sampler,positionTolerance,convergenceThreshold,result);
    return result;
}

void RISE::Implementation::ManifoldSolver::SolveDomainCoreInto(
    const Point3& start, const Vector3& startNormal, const Point3& end, const Vector3& endNormal,
    const IScene& scene, const IORStack& startingStack, SMSQueryDomain domain,
    std::vector<SMSDomainVertex>& vertices, ISampler& sampler, Scalar positionTolerance, Scalar convergenceThreshold, ManifoldResult& result) const
{
    SMSWorkerScratchLease scratch(true,config.referenceCounters);
    SMSResetResult(result);
    struct Attempt {
        SMSDomainCounters* counters;
        ManifoldResult& result;
        bool accepted = false;
        ~Attempt() {
            if(!accepted) SMSResetResult(result);
            if(counters) (accepted ? counters->acceptedRoots : counters->rejectedRoots)
                .fetch_add(1, std::memory_order_relaxed);
        }
    } attempt{config.domainCounters,result};
    if(attempt.counters) attempt.counters->attempts.fetch_add(1, std::memory_order_relaxed);
    if(vertices.empty() || vertices.size() > config.maxChainDepth
        || !std::isfinite(positionTolerance) || positionTolerance < 0) { return; }
    SMSStartingMedia media;
    IORStack replay(startingStack.EnvironmentIOR());
    if(!SMSDomainReplay::Capture(scene, start, startingStack, media)
        || !SMSDomainReplay::BuildStack(media, domain, replay)) { return; }
    auto& chain=scratch.Vertices(0,config.maxChainDepth);
    chain.reserve(vertices.size());
    for(SMSDomainVertex& record : vertices) {
        if(const auto* objects=SMSDynamicCast<ObjectManager>(scene.GetObjects()))
            objects->CompleteShadingSignals(record.context,record.geometry.pObject);
        ManifoldVertex vertex = record.geometry;
        if(!vertex.pMaterial || !vertex.pObject || !SMSAuditedModifier(*vertex.pObject)) { return; }
        SMSNativeMaterialQuery query;
        if(!SMSDomainReplay::Query(*vertex.pMaterial, record.context, replay, domain, query)
            || !SMSDomainReplay::Cross(*vertex.pMaterial, vertex.pObject, record.context,
                domain, vertex.isReflection, replay, vertex.etaI, vertex.etaT, vertex.isExiting)) { return; }
        vertex.eta = query.index;
        vertex.canRefract = query.dielectricInterface;
        vertex.normal = SMSNativeEventNormal(*vertex.pMaterial,record.context);
        chain.push_back(vertex);
    }
    SolveCoreInto(start,startNormal,end,endNormal,chain,sampler,false,convergenceThreshold,result);
    if(!result.valid || chain.size() != vertices.size()) { return; }
    if(!SMSDomainReplay::BuildStack(media, domain, replay)) { return; }
    Scalar throughput = 1;
    auto& refreshed=scratch.Records(0,config.maxChainDepth);
    refreshed.reserve(vertices.size());
    Point3 previous = start;
    for(std::size_t i=0; i<chain.size(); ++i) {
        ManifoldVertex& vertex = chain[i];
        if(vertex.isReflection != vertices[i].geometry.isReflection) { return; }
        const Vector3 direction = Vector3Ops::Normalize(Vector3Ops::mkVector3(vertex.position, previous));
        RayIntersection hit(Ray(previous, direction), vertices[i].context.rast);
        if(i != 0) hit.geometric.ray.Advance(1e-8); // native surface-walk self-hit offset
        vertex.pObject->IntersectRay(hit, RISE_INFINITY, true, true, false);
        SMSCompleteNativeHit(hit,scene.GetObjects());
        if(!hit.geometric.bHit || hit.pObject != vertex.pObject || hit.pMaterial != vertex.pMaterial
            || Point3Ops::Distance(convergenceThreshold > 0 ? SMSReferenceSurfacePoint(*vertex.pObject,hit.geometric) : hit.geometric.ptIntersection, vertex.position) > positionTolerance) { return; }
        if(hit.pModifier) hit.pModifier->Modify(hit.geometric);
        const auto finite3=[](const auto& value) {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        };
        if(!finite3(hit.geometric.ptIntersection) || !finite3(hit.geometric.ptObjIntersec)
            || !finite3(hit.geometric.vNormal) || !finite3(hit.geometric.UnflippedGeomNormal())
            || !finite3(hit.geometric.onb.u()) || !finite3(hit.geometric.onb.v()) || !finite3(hit.geometric.onb.w())
            || !std::isfinite(hit.geometric.ptCoord.x) || !std::isfinite(hit.geometric.ptCoord.y)) return;
        // A periodic chart seam is one physical root for an audited
        // UV-independent event/frame law. A varying or unaudited UV-dependent
        // seam context cannot establish a unique limiting value: it is
        // an uncertain zero trial. Never invent a UV for a material query.
        Scalar contextSlope=0;
        if(convergenceThreshold > 0) {
            Point2 axes = SMSPeriodicTextureAxes(*vertex.pObject,hit.geometric.UnflippedGeomNormal());
            const auto* nativeObject=SMSDynamicCast<Object>(vertex.pObject);
            const IGeometry* geometry=vertex.pObject->GetGeometry();
            // Authored mesh chart boundaries are NOT periodic aliases. A
            // varying boundary context remains uncertain, while a certified
            // constant event law does not depend on its UV representation.
            if(nativeObject && nativeObject->UsesNativeTextureChart() && geometry
                && (SMSSameType<TriangleMeshGeometryIndexed>(*geometry) || SMSSameType<TriangleMeshGeometry>(*geometry))) axes=Point2(1,1);
            const Scalar band = std::sqrt(std::numeric_limits<Scalar>::epsilon());
            bool meshEdge=false;
            if(nativeObject && nativeObject->UsesNativeTextureChart() && geometry) {
                const auto& signal=hit.geometric.signals;
                // DisplacedGeometry forwards its realized indexed mesh's
                // actual hit payload. The provider, not the outer recipe,
                // identifies the triangle whose chart boundary was hit.
                const auto* indexed=SMSDynamicCast<TriangleMeshGeometryIndexed>(signal.pProvider);
                if(indexed && SMSSameType<TriangleMeshGeometryIndexed>(*indexed)) {
                    meshEdge=signal.primId<0
                        || std::min({signal.baryA,signal.baryB,1-signal.baryA-signal.baryB})<=band;
                } else if(SMSSameType<TriangleMeshGeometryIndexed>(*geometry)) {
                    meshEdge=true; // missing native triangle provenance is uncertain
                } else if(SMSSameType<TriangleMeshGeometry>(*geometry)) {
                    meshEdge=SMSDynamicCast<TriangleMeshGeometry>(geometry)->NativeTriangleEdgeDistance(hit.geometric)<=band;
                }
            }
            const bool seam = meshEdge || (axes.x && std::min(std::fabs(hit.geometric.ptCoord.x),std::fabs(1-hit.geometric.ptCoord.x)) <= band)
                || (axes.y && std::min(std::fabs(hit.geometric.ptCoord.y),std::fabs(1-hit.geometric.ptCoord.y)) <= band);
            const bool uvIndependent=SMSContextIgnoresUV(*vertex.pObject,*vertex.pMaterial);
            if(seam && !uvIndependent) return;
            const bool nativeOpticalPrice=SMSDynamicCast<DielectricSPF>(vertex.pMaterial->GetSPF())
                || SMSDynamicCast<PerfectRefractorMaterial>(vertex.pMaterial);
            if(nativeOpticalPrice || meshEdge || hit.pModifier || (nativeObject && !nativeObject->UsesNativeTextureChart()
                && !SMSConstantSeamMaterial(*vertex.pMaterial))) {
                // Authored normals can switch transmission fallback and its
                // reflection price even inside an unmodified native triangle.
                // Generated charts and modifier-written contexts can also jump.
                // Probe their actual contexts within the physical matching
                // band, instead of assuming the geometry's chart describes
                // the generator. This is a local numerical ambiguity check,
                // not a global continuity certificate. Continuous fixed UVs
                // remain supported and material coordinates are never edited.
                const Scalar displacement=band*Point3Ops::Distance(start,end)/8;
                if(!(displacement>0) || !std::isfinite(displacement)) return;
                const auto eventBranch=[&](const RayIntersectionGeometric& context) {
                    const auto law=SMSNativeDirections(context.ray.Dir(),SMSNativeEventNormal(*vertex.pMaterial,context),
                        context.UnflippedGeomNormal(),vertex.etaI,vertex.etaT,vertex.canRefract
                            &&!SMSSameType<PolishedMaterial>(*vertex.pMaterial),false,vertex.pMaterial,
                        vertex.isExiting,SMSDomainWavelength(domain));
                    return (vertex.isReflection?unsigned(law.reflectionFallback)
                        :(unsigned(law.transmissionFallback)<<1)|(unsigned(law.hasTransmission)<<2))
                        |(unsigned(std::fabs(Vector3Ops::Magnitude(SMSNativeEventNormal(*vertex.pMaterial,context))-1)>Scalar(1e-6))<<3);
                };
                const unsigned matchedBranch=eventBranch(hit.geometric);
                // Test the optical discontinuity that selects the native
                // fallback. Attenuation painters may use float spectral
                // reconstruction; their numeric quantization is not a change
                // in Fresnel support. Their input contexts are checked below.
                const auto opticalWeight=[&](const RayIntersectionGeometric& context,Scalar& weight) {
                    const auto* dielectric=SMSDynamicCast<DielectricSPF>(vertex.pMaterial->GetSPF());
                    if(!dielectric && !SMSDynamicCast<PerfectRefractorMaterial>(vertex.pMaterial)) {weight=1;return true;}
                    const bool custom=dielectric && dielectric->GetARLayerCount();
                    const auto law=SMSNativeDirections(context.ray.Dir(),SMSNativeEventNormal(*vertex.pMaterial,context),
                        context.UnflippedGeomNormal(),vertex.etaI,vertex.etaT,vertex.canRefract,custom,
                        vertex.pMaterial,vertex.isExiting,SMSDomainWavelength(domain));
                    Scalar fresnel=law.hasTransmission?Optics::CalculateDielectricReflectanceCosine(
                        law.fresnelCosine,vertex.etaI,vertex.etaT):1;
                    if(custom && law.hasTransmission && !dielectric->EvaluateSpecularFresnelAfterRefraction(law.fresnelCosine,
                        vertex.etaI,vertex.etaT,vertex.isExiting,SMSDomainWavelength(domain),fresnel)) return false;
                    weight=vertex.isReflection?fresnel:1-fresnel;return std::isfinite(weight);
                };
                Scalar matchedWeight;
                if(!opticalWeight(hit.geometric,matchedWeight)) return;
                for(const Vector3& tangent : {vertex.dpdu,vertex.dpdv}) for(int sign : {-1,1}) {
                    Point2 coords[2];Point3 positions[2];Vector3 normals[2],geomNormals[2];
                    OrthonormalBasis3D frames[2];Scalar weights[2];
                    for(unsigned probeIndex=0;probeIndex<2;++probeIndex) {
                        const Scalar fraction=probeIndex?.5:1;
                        const Point3 target=Point3Ops::mkPoint3(vertex.position,tangent*(sign*displacement*fraction));
                        Ray ray(previous,Vector3Ops::Normalize(Vector3Ops::mkVector3(target,previous)));
                        if(i) ray.Advance(1e-8);
                        RayIntersection probe(ray,vertices[i].context.rast);
                        vertex.pObject->IntersectRay(probe,RISE_INFINITY,true,true,false);
                        SMSCompleteNativeHit(probe,scene.GetObjects());
                        if(probe.geometric.bHit && probe.pModifier) probe.pModifier->Modify(probe.geometric);
                        if(probe.geometric.bHit && eventBranch(probe.geometric)!=matchedBranch) return;
                        if(!probe.geometric.bHit || probe.pObject!=vertex.pObject
                            || probe.pMaterial!=vertex.pMaterial
                            || Point3Ops::Distance(SMSReferenceSurfacePoint(*vertex.pObject,probe.geometric),target)>displacement
                            || !finite3(probe.geometric.ptIntersection) || !finite3(probe.geometric.ptObjIntersec)
                            || !finite3(probe.geometric.vNormal) || !finite3(probe.geometric.UnflippedGeomNormal())
                            || !std::isfinite(probe.geometric.ptCoord.x) || !std::isfinite(probe.geometric.ptCoord.y)) return;
                        if(!opticalWeight(probe.geometric,weights[probeIndex])) return;
                        coords[probeIndex]=probe.geometric.ptCoord;
                        positions[probeIndex]=probe.geometric.ptObjIntersec;
                        normals[probeIndex]=probe.geometric.vNormal;
                        geomNormals[probeIndex]=probe.geometric.UnflippedGeomNormal();
                        frames[probeIndex]=probe.geometric.onb;
                    }
                    const Scalar uvChange=uvIndependent?0:std::hypot(coords[0].x-hit.geometric.ptCoord.x,coords[0].y-hit.geometric.ptCoord.y);
                    const Scalar normalChange=std::max(Vector3Ops::Magnitude(normals[0]-hit.geometric.vNormal),
                        Vector3Ops::Magnitude(geomNormals[0]-hit.geometric.UnflippedGeomNormal()));
                    contextSlope=std::max(contextSlope,std::max(uvChange,normalChange)/displacement);
                    // Audited isotropic events consume W, not the UV-driven
                    // U/V orientation, which can change across a mesh diagonal.
                    for(unsigned axis=uvIndependent?2u:0u;axis<3;++axis) {
                        const Vector3 a=axis==0?frames[0].u():axis==1?frames[0].v():frames[0].w();
                        const Vector3 b=axis==0?frames[1].u():axis==1?frames[1].v():frames[1].w();
                        const Vector3 center=axis==0?hit.geometric.onb.u():axis==1?hit.geometric.onb.v():hit.geometric.onb.w();
                        if(!finite3(a)||!finite3(b)||Vector3Ops::Magnitude(b*2-a-center)>band/8) return;
                        contextSlope=std::max(contextSlope,Vector3Ops::Magnitude(a-center)/displacement);
                    }
                    // A smooth context's first-order change cancels at the
                    // midpoint, independently of its UV scale. A finite
                    // context jump does not. Reserve the corresponding root-matching
                    // bands; do not bound the context's first derivative.
                    if((!uvIndependent && std::hypot(2*coords[1].x-coords[0].x-hit.geometric.ptCoord.x,
                        2*coords[1].y-coords[0].y-hit.geometric.ptCoord.y)>band/8)
                        || Vector3Ops::Magnitude(normals[1]*2-normals[0]-hit.geometric.vNormal)>band/8
                        || Vector3Ops::Magnitude(geomNormals[1]*2-geomNormals[0]-hit.geometric.UnflippedGeomNormal())>band/8) return;
                    const Scalar priceScale=std::max({Scalar(1),std::fabs(matchedWeight),std::fabs(weights[0]),std::fabs(weights[1])});
                    if(std::fabs(2*weights[1]-weights[0]-matchedWeight)>band*priceScale/8) return;
                    contextSlope=std::max(contextSlope,std::fabs(weights[0]-matchedWeight)/(displacement*priceScale));
                    const Point3& center=hit.geometric.ptObjIntersec;
                    const Scalar objectScale=std::max({Scalar(1),std::fabs(center.x),std::fabs(center.y),std::fabs(center.z),
                        std::fabs(positions[0].x),std::fabs(positions[0].y),std::fabs(positions[0].z),
                        std::fabs(positions[1].x),std::fabs(positions[1].y),std::fabs(positions[1].z)});
                    const Vector3 positionDifference(2*positions[1].x-positions[0].x-center.x,
                        2*positions[1].y-positions[0].y-center.y,2*positions[1].z-positions[0].z-center.z);
                    if(Vector3Ops::Magnitude(positionDifference)>band*objectScale/8) return;
                    contextSlope=std::max(contextSlope,Point3Ops::Distance(positions[0],center)/(displacement*objectScale));
                }
            }
        }
        const IORStack before(replay);
        Scalar etaI, etaT; bool exiting;
        if(!SMSDomainReplay::Cross(*vertex.pMaterial, vertex.pObject, hit.geometric, domain,
            vertex.isReflection, replay, etaI, etaT, exiting)
            || exiting != vertex.isExiting || etaI != vertex.etaI || etaT != vertex.etaT) { return; }
        Scalar eventWeight;
        if(!SMSDomainReplay::EventWeight(*vertex.pMaterial, hit.geometric, before, domain,
            vertex.isReflection, exiting, etaI, etaT, Point3Ops::Distance(previous, vertex.position), eventWeight)) { return; }
        throughput *= eventWeight;
        if(!std::isfinite(throughput)) { return; }
        SMSNativeMaterialQuery query;
        if(!SMSDomainReplay::Query(*vertex.pMaterial, hit.geometric, before, domain, query)) { return; }
        vertex.attenuation = RISEPel(query.attenuation);
        vertex.attenuationNM = query.attenuation;
        vertex.attenuationAppliesToReflection = query.reflectionTint;
        vertex.attenuationIsInteriorTransmittance = query.interiorTransmittance;
        vertex.hasCustomSpecularFresnel = query.customFresnel;
        vertex.normal = SMSNativeEventNormal(*vertex.pMaterial,hit.geometric);
        vertex.geomNormal = hit.geometric.UnflippedGeomNormal();
        vertex.uv = hit.geometric.ptCoord;
        vertex.objectPosition = hit.geometric.ptObjIntersec;
        refreshed.emplace_back(hit.geometric);
        refreshed.back().geometry = vertex;
        refreshed.back().contextSlope = contextSlope;
        previous = vertex.position;
    }
    auto& refreshedResidual=scratch.Scalars(0,2*config.maxChainDepth);
    EvaluateConstraint(chain, start, end, refreshedResidual);
    Scalar residualSquared = 0;
    for(Scalar component : refreshedResidual) residualSquared += component*component;
    if(!std::isfinite(residualSquared) || std::sqrt(residualSquared) > (convergenceThreshold > 0 ? convergenceThreshold : config.solverThreshold)
        || !ValidateChainPhysics(chain, start, end)) { return; }
    result.specularChain = chain;
    result.contribution = RISEPel(0,0,0);
    result.contributionNM = throughput;
    if(domain.kind == SMSQueryDomain::RGBComponent) result.contribution[domain.component] = throughput;
    vertices.assign(std::make_move_iterator(refreshed.begin()),std::make_move_iterator(refreshed.end()));
    attempt.accepted = true;
    return;
}

// File-scope diagnostic gate.  Set to 1 to enable targeted per-pixel
// SMS/Solve/BuildSeedChain trace logging (used while debugging the
// torus-intersection accuracy issue that led to the OQS replacement).
// Leave at 0 for production — the instrumentation stays in-source as
// a regression aid but is compiled out.
#define SMS_TRACE_DIAGNOSTIC 0

// Lightweight Solve()-failure-mode counters.  When enabled, dumps an
// [SMS-SOLVE-DIAG] + [SMS-NEWTONFAIL-RES] pair at process exit so the
// share of solves rejected by each early-out path is visible alongside
// PathTracingIntegrator's per-evaluation SMS-DIAG line.  Used to attribute
// the energy drop on heavy-displacement scenes to Newton plateau-stalling
// rather than seed quality or iteration budget — see the displaced-Veach-
// egg sweep results.  Leave at 0 in production; flip to 1 when re-
// auditing SMS energy ratios across disp / multi-trial / photon configs.
#define SMS_SOLVE_DIAG 0

// EXPERIMENT E: perturbed-seed restart on phys-fail.  When a Newton-
// converged chain is rejected by ValidateChainPhysics, retry up to N
// times with a small Gaussian-style perturbation on every vertex's
// tangent-plane position.  Tests whether the spurious-minimum basin
// is "near" a physically-valid basin (perturbation rescues) or is
// topologically separated (perturbation always lands in the same bad
// basin).  Set kPhysFailRetries=0 to disable.
#define SMS_PHYSFAIL_RESTART_ENABLED 1
namespace { constexpr unsigned int kPhysFailRetries  = 0; }	// E disabled for G's clean apples-to-apples; revert to 4 to re-enable.
namespace { constexpr RISE::Scalar kPhysFailPerturb  = RISE::Scalar( 0.10 ); } ///< Half-range of uniform tangent-plane perturbation (mirrors the Bernoulli loop's 0.1 range).

// EXPERIMENT T2: pre-Newton topology check.  Validate the seed chain's
// physics BEFORE running Newton.  If the seed is already wrong-topology,
// skip the Solve entirely (don't waste the Newton iters; we know they
// can't help).  Default 0 (off); flip to 1 to evaluate.
#define SMS_T2_PRE_NEWTON_TOPOLOGY_CHECK 0

// EXPERIMENT T3: post-Newton failing-vertex retry.  When Newton
// converges but ValidateChainPhysics rejects, perturb ONLY the failing
// vertex (not all vertices like E did) by a random tangent offset, and
// retry Newton up to N times.  More targeted than E.  Default 0 (off).
#define SMS_T3_POST_NEWTON_FAILING_VERTEX_RETRY 0
#if SMS_T3_POST_NEWTON_FAILING_VERTEX_RETRY
namespace { constexpr unsigned int kT3FailingVertexRetries = 4; }
namespace { constexpr RISE::Scalar kT3PerturbMag           = RISE::Scalar( 0.10 ); }
#endif

// Newton step-norm cap.  Bounds each Newton iter's max world-space
// vertex displacement to <kNewtonStepNormCapFrac> × (mean inter-vertex
// segment length).  Default 0 disables the cap (the existing 10-halving
// backtracking line search already handles oversize Newton steps —
// measured: at disp=0 the cap fires 76 % of the time on raw Newton
// steps that average 262× oversize, but the line search converges to
// the same end point; Newton-fail rate is unchanged within MC noise).
//
// Code retained as inactive instrumentation.  Flip to a positive value
// (e.g. 0.25) only when investigating perf — fewer line-search attempts
// per iter — not for convergence on heavy displacement (the failure
// mode there is wrong step DIRECTION, not wrong step SIZE; LM/trust-
// region damping is the relevant fix).  See
// docs/SMS_PHOTON_DECOUPLING_AND_DISPLACEMENT_LIMIT.md for the full
// measurement.
namespace { constexpr RISE::Scalar kNewtonStepNormCapFrac = RISE::Scalar( 0.0 ); }

// Levenberg-Marquardt damping for the Newton solver.  When enabled,
// `NewtonSolve` damps the Jacobian's diagonal by `λ × mean(|J_ii|)`
// before each solve, and adapts `λ` between iterations: shrink on
// accepted steps (back toward pure Newton, fast convergence near a
// root), grow on rejected line-search attempts (more gradient-descent-
// like, escape from plateaus where the Newton direction is unreliable).
//
// Variant: damped Newton on the original J, not the full Marquardt-style
// `(JᵀJ + λ·diag(JᵀJ))Δ = JᵀC` normal equations.  We modify diag(J)
// in place and reuse the existing block-tridiagonal solver, which keeps
// the bandwidth structure intact.  Full LM via normal equations would
// double the bandwidth (block-tridiag × block-tridiag = block-pentadiag)
// and need a different solver — escalate to that variant only if this
// damped-Newton form is insufficient.
//
// Runtime-toggleable via `ManifoldSolverConfig::useLevenbergMarquardt`
// (default FALSE — opt-in).  LM is ~50-100% slower than pure Newton on
// heavy-displacement scenes (escalation iterations consume the iter
// budget) for a 4-10 percentage-point Newton-fail-rate improvement
// across the displaced-egg sweep.  Off by default because the cost is
// substantial relative to the gain; enable explicitly when a scene's
// caustics on a heavily-displaced mesh need the extra robustness.
// See docs/SMS_LEVENBERG_MARQUARDT.md for the full A/B measurement.
// Edge-aware Newton step.  The Jacobian's `dndu`/`dndv` predict how the
// normal rotates for a step (du, dv).  Within one triangle, Phong-
// interpolated normals are linear in barycentric coords so the
// prediction is accurate.  ACROSS a triangle edge, the per-triangle
// linearization changes — so a Newton step that crosses an edge
// produces an actual normal rotation that differs from what the saved
// Jacobian predicted.  Reject the step when actual rotation exceeds a
// relative threshold and let the line search halve.
//
// Measured to NOT help on the displaced Veach egg: rejection rate
// climbs cleanly with displacement (0% at disp=0 → 12% at disp=10),
// confirming the check correctly identifies "untrusted linearization,"
// but Newton-fail rate is unchanged or marginally worse (-0.0 to
// -0.4 pp ok rate).  The C¹ defect manifests as "predicted descent
// direction is noisy" rather than "Newton overshoots into bad
// territory" — the line search's ||C|| decrease test was already
// accepting the cross-edge steps that decreased residual; pre-
// rejecting on linearization-trust just blocks useful moves.
//
// Code retained as inactive instrumentation (gate at 0 = compiled
// out).  See docs/SMS_LEVENBERG_MARQUARDT.md for the negative-result
// measurement.
#define SMS_EDGE_AWARE_NEWTON 0

// Smooth-base Jacobian throughout the solve.  When enabled, even at
// `smoothing == 0` (the default solve path) ComputeVertexDerivatives
// queries the underlying analytical surface for `dpdu/dpdv/dndu/dndv`,
// while keeping the bumpy mesh position and normal from the ray-cast.
// The intent: Newton's descent direction comes from a C∞ smooth
// curvature model rather than the per-triangle Phong-interpolated J,
// while the residual `C` is still evaluated against the real bumpy
// normals.
//
// Compile-time toggle, default 0 (legacy mesh-J path).  Flip to 1 to
// reproduce the smooth-J experiment on the displaced Veach egg.
// ⚠ Before enabling: the override replaces vertex.dndu/dndv with a fresh
// uv-keyed ComputeAnalyticalDerivatives() answer while KEEPING
// vertex.normal from the probe raycast -- and the raycast record's
// normal may be back-face-flipped (double-sided mesh hit from inside),
// in which case its dndu/dndv were negated to match
// (TriangleMeshGeometry(Indexed)::IntersectRay).  The uv-keyed analytic
// answer knows nothing of that flip, so on a back-facing DisplacedGeometry
// vertex the override would pair a flipped normal with an unflipped
// dndu/dndv -- a sign-inconsistent Newton Jacobian.  Reconcile the sign
// (e.g. negate the analytic dndu/dndv when Dot(analyticN, vertex.normal)
// < 0) before turning this on.
#if SMS_EDGE_AWARE_NEWTON
namespace { constexpr RISE::Scalar kEdgeTrustAbsFloor = RISE::Scalar( 0.05 ); }	///< below this absolute rotation magnitude, no test (any change OK)
namespace { constexpr RISE::Scalar kEdgeTrustRelRatio = RISE::Scalar( 3.0  ); }	///< actual must not exceed predicted × this when both > floor
#endif
// LM damping schedule (used when `config.useLevenbergMarquardt` is true).
namespace { constexpr RISE::Scalar kLM_LambdaInit = RISE::Scalar( 1e-3 ); }
namespace { constexpr RISE::Scalar kLM_LambdaMin  = RISE::Scalar( 1e-9 ); }
namespace { constexpr RISE::Scalar kLM_LambdaMax  = RISE::Scalar( 1e9  ); }
namespace { constexpr RISE::Scalar kLM_LambdaUp   = RISE::Scalar( 10.0 ); }
namespace { constexpr RISE::Scalar kLM_LambdaDown = RISE::Scalar( 0.1  ); }
#if SMS_SOLVE_DIAG
#include <atomic>
#include <cstdio>
namespace {
	std::atomic<uint64_t> g_solveDiag_calls{0};
	std::atomic<uint64_t> g_solveDiag_seedTooFar{0};
	std::atomic<uint64_t> g_solveDiag_derivFail{0};
	std::atomic<uint64_t> g_solveDiag_newtonFail{0};
	std::atomic<uint64_t> g_solveDiag_physicsFail{0};
	std::atomic<uint64_t> g_solveDiag_shortSeg{0};
	std::atomic<uint64_t> g_solveDiag_ok{0};
	// Newton-fail final ||C|| residual buckets — distinguishes "stuck very
	// close" (basin-too-narrow / line-search starved) from "diverged far"
	// (seed in wrong basin or line-search exits early).
	std::atomic<uint64_t> g_solveDiag_newtonFail_lt1e3{0};	///< ||C|| ∈ [threshold, 1e-3)
	std::atomic<uint64_t> g_solveDiag_newtonFail_lt1e2{0};	///< [1e-3, 1e-2)
	std::atomic<uint64_t> g_solveDiag_newtonFail_lt1e1{0};	///< [1e-2, 1e-1)
	std::atomic<uint64_t> g_solveDiag_newtonFail_lt1e0{0};	///< [1e-1, 1)
	std::atomic<uint64_t> g_solveDiag_newtonFail_ge1e0{0};	///< [1, ∞)
	// Step-norm cap diagnostics: how often the cap fires, and the
	// average pre-cap step ratio (max_step / cap) — indicates whether
	// the cap actually intercepts Newton steps or sits idle.
	std::atomic<uint64_t> g_solveDiag_capChecks{0};
	std::atomic<uint64_t> g_solveDiag_capFired{0};
	std::atomic<uint64_t> g_solveDiag_capRatioSum{0};	///< Σ (max_step/cap × 1000), only when fired

	// Levenberg-Marquardt diagnostics.
	std::atomic<uint64_t> g_solveDiag_lmTotalIters{0};      ///< Newton iterations across all Solve calls
	std::atomic<uint64_t> g_solveDiag_lmDamped{0};          ///< Iters where λ > 0 was applied
	std::atomic<uint64_t> g_solveDiag_lmEscalated{0};       ///< Iters where line search failed → λ increased
	std::atomic<uint64_t> g_solveDiag_lmRecovered{0};       ///< Solve calls where LM rescued an iter that pure Newton would have failed

	// Edge-aware Newton-step diagnostics.
	std::atomic<uint64_t> g_solveDiag_edgeChecks{0};        ///< Line-search attempts where the trust check ran
	std::atomic<uint64_t> g_solveDiag_edgeRejects{0};       ///< Line-search attempts rejected due to untrusted linearization

	// PHYS-FAIL ANATOMY (Direction A in the displaced-caustic
	// investigation): characterise each vertex that fails
	// ValidateChainPhysics by chain-position, operation, grazing
	// magnitude, shading-vs-geom disagreement, and Phong-tilt
	// magnitude — so we can attribute spurious rejections to specific
	// geometric regimes rather than a single black-box "physicsFail"
	// counter.
	std::atomic<uint64_t> g_physFail_byVtxPos[4]{};         ///< 0=sole(k=1), 1=first, 2=middle, 3=last
	std::atomic<uint64_t> g_physFail_byOp[2]{};             ///< 0=refraction, 1=reflection
	std::atomic<uint64_t> g_physFail_minCos[6]{};           ///< min(|wi·n|,|wo·n|): <1e-4, 1e-4..1e-3, 1e-3..1e-2, 1e-2..1e-1, 1e-1..0.5, >=0.5
	std::atomic<uint64_t> g_physFail_shadingAgrees{0};      ///< Shading-normal test ALSO rejects (chain genuinely wrong)
	std::atomic<uint64_t> g_physFail_shadingMasked{0};      ///< Shading would have ACCEPTED — geom test caught it (audit win)
	std::atomic<uint64_t> g_physFail_phongTilt[5]{};        ///< dot(geomN, shadingN): >.999, .99..999, .95..99, .8..95, <.8
	std::atomic<uint64_t> g_physFail_geomFallback{0};       ///< Failures where vertex had no geomNormal (used shading proxy)

	// CASCADE anatomy: when a chain's first vertex passes physics but a
	// later vertex fails, how many vertices got through before the
	// rejection?  And how far on the surface did the chain travel from
	// the (good) entry vertex to the (bad) failing vertex?
	std::atomic<uint64_t> g_physFail_firstFailIdx[6]{};     ///< 0=fails-at-vertex-0, 1, 2, 3, 4, 5+
	std::atomic<uint64_t> g_physFail_v0_to_fail_angle[6]{}; ///< angle(geomN[0], geomN[fail]): <5°, 5..15°, 15..45°, 45..90°, 90..135°, >=135°

	// EXPERIMENT E: perturbed-restart counters.
	std::atomic<uint64_t> g_physFailRestart_attempted{0};   ///< Solve calls that reached the phys-fail-retry loop
	std::atomic<uint64_t> g_physFailRestart_rescued{0};     ///< Calls where SOME perturbed seed produced a phys-valid converged chain
	std::atomic<uint64_t> g_physFailRestart_iters{0};       ///< Total perturbed Newton attempts (across all calls)

	// EXPERIMENT T2: pre-Newton topology check counters.
	std::atomic<uint64_t> g_t2_seedValidated{0};            ///< Solves where pre-Newton topology check ran
	std::atomic<uint64_t> g_t2_seedSkipped{0};              ///< Solves skipped because seed was invalid

	// EXPERIMENT T3: post-Newton failing-vertex-only retry counters.
	std::atomic<uint64_t> g_t3_attempted{0};                ///< Phys-failed Solves that entered retry loop
	std::atomic<uint64_t> g_t3_rescued{0};                  ///< Retries that produced a phys-valid chain
	std::atomic<uint64_t> g_t3_iters{0};                    ///< Total Newton solves spent in retries

	// PUSHBACK ANATOMY: deeper instrumentation to attribute phys-fails to
	// either "Newton dragged a valid seed into a bad basin" (solver bug)
	// vs "seed was already invalid" (seeding bug) vs "the constraint
	// landscape genuinely has multi-modal roots reachable from the seed".
	std::atomic<uint64_t> g_physFail_signProd_bothPos{0};   ///< wi·n > 0 AND wo·n > 0 at failing vertex (refraction-labeled, geometry is reflection-from-above)
	std::atomic<uint64_t> g_physFail_signProd_bothNeg{0};   ///< wi·n < 0 AND wo·n < 0 at failing vertex (geometry is reflection-from-below)
	std::atomic<uint64_t> g_physFail_seedWasValid{0};       ///< The pre-Newton seed chain ALSO satisfied physics
	std::atomic<uint64_t> g_physFail_seedWasInvalid{0};     ///< The pre-Newton seed chain ALREADY failed physics (Newton can't fix bad seed)
	std::atomic<uint64_t> g_physFail_finalNorm_lt1e6{0};    ///< Post-Newton ‖C‖ buckets for rejected chains (true convergence quality)
	std::atomic<uint64_t> g_physFail_finalNorm_lt1e4{0};
	std::atomic<uint64_t> g_physFail_finalNorm_lt1e2{0};
	std::atomic<uint64_t> g_physFail_finalNorm_ge1e2{0};

	std::atomic<uint64_t> g_physFail_chainLen[6]{};         ///< 0=k=1, 1=k=2, 2=k=3, 3=k=4, 4=k=5, 5=k=6+
	std::atomic<uint64_t> g_physFail_anyReflectionInChain{0}; ///< Chain has at least one isReflection=true vertex (TIR or mirror)
	std::atomic<uint64_t> g_physFail_allRefractionInChain{0}; ///< Every vertex in chain is refraction

	// EXPERIMENT (c) NEWTON-STALL TOPOLOGY: 16×16 histogram of (u, v) on
	// vertex 0 of every Newton-failed chain.  Vertex 0 is the entry hit
	// on the displaced outer surface, so its (u, v) tells us WHERE on
	// the underlying ellipsoid Newton stalled.  Compare across painter
	// modes (gaussian / quadratic / heaviside) at the same disp value
	// to localise which surface regions drive plateau-stalling.
	constexpr unsigned int kStallBins = 16;
	std::atomic<uint64_t> g_newtonFail_uv[kStallBins][kStallBins]{};
	std::atomic<uint64_t> g_newtonOk_uv[kStallBins][kStallBins]{};
	std::atomic<uint64_t> g_physFail_uv[kStallBins][kStallBins]{};   ///< Phys-fail (wrong-topology) localisation on the underlying surface

	inline void StallTopology_BinFail( const RISE::Point2& uv ) {
		const RISE::Scalar u = uv.x < 0.0 ? 0.0 : (uv.x > 1.0 ? 0.99999 : uv.x);
		const RISE::Scalar v = uv.y < 0.0 ? 0.0 : (uv.y > 1.0 ? 0.99999 : uv.y);
		const unsigned int iu = (unsigned int)(u * kStallBins);
		const unsigned int iv = (unsigned int)(v * kStallBins);
		g_newtonFail_uv[iu][iv].fetch_add( 1, std::memory_order_relaxed );
	}
	inline void StallTopology_BinOk( const RISE::Point2& uv ) {
		const RISE::Scalar u = uv.x < 0.0 ? 0.0 : (uv.x > 1.0 ? 0.99999 : uv.x);
		const RISE::Scalar v = uv.y < 0.0 ? 0.0 : (uv.y > 1.0 ? 0.99999 : uv.y);
		const unsigned int iu = (unsigned int)(u * kStallBins);
		const unsigned int iv = (unsigned int)(v * kStallBins);
		g_newtonOk_uv[iu][iv].fetch_add( 1, std::memory_order_relaxed );
	}
	inline void StallTopology_BinPhysFail( const RISE::Point2& uv ) {
		const RISE::Scalar u = uv.x < 0.0 ? 0.0 : (uv.x > 1.0 ? 0.99999 : uv.x);
		const RISE::Scalar v = uv.y < 0.0 ? 0.0 : (uv.y > 1.0 ? 0.99999 : uv.y);
		const unsigned int iu = (unsigned int)(u * kStallBins);
		const unsigned int iv = (unsigned int)(v * kStallBins);
		g_physFail_uv[iu][iv].fetch_add( 1, std::memory_order_relaxed );
	}

	inline void PhysFail_BinFirstFailIdx( unsigned int i ) {
		const unsigned int slot = i >= 5 ? 5 : i;
		g_physFail_firstFailIdx[slot].fetch_add( 1, std::memory_order_relaxed );
	}
	inline void PhysFail_BinV0FailAngle( double dotN0Nf ) {
		// dotN0Nf in [-1, 1].  cos(5°)≈0.996, cos(15°)≈0.966, cos(45°)≈0.707,
		// cos(90°)=0, cos(135°)≈-0.707.  Sign matters: a negative dot means
		// the chain crosses to the OPPOSITE-facing side of the surface
		// between v0 and v_fail (e.g. front→back of a thin shell).
		unsigned int slot;
		if      ( dotN0Nf >  0.996 ) slot = 0;       // <5°
		else if ( dotN0Nf >  0.966 ) slot = 1;       // 5..15°
		else if ( dotN0Nf >  0.707 ) slot = 2;       // 15..45°
		else if ( dotN0Nf >  0.0   ) slot = 3;       // 45..90°
		else if ( dotN0Nf > -0.707 ) slot = 4;       // 90..135°
		else                         slot = 5;       // >=135°
		g_physFail_v0_to_fail_angle[slot].fetch_add( 1, std::memory_order_relaxed );
	}

	inline void PhysFail_BinPos( unsigned int i, unsigned int k ) {
		unsigned int slot;
		if( k == 1 )            slot = 0;
		else if( i == 0 )       slot = 1;
		else if( i == k - 1 )   slot = 3;
		else                    slot = 2;
		g_physFail_byVtxPos[slot].fetch_add( 1, std::memory_order_relaxed );
	}
	inline void PhysFail_BinMinCos( double minAbsCos ) {
		unsigned int slot;
		if      ( minAbsCos < 1e-4 ) slot = 0;
		else if ( minAbsCos < 1e-3 ) slot = 1;
		else if ( minAbsCos < 1e-2 ) slot = 2;
		else if ( minAbsCos < 1e-1 ) slot = 3;
		else if ( minAbsCos < 0.5  ) slot = 4;
		else                         slot = 5;
		g_physFail_minCos[slot].fetch_add( 1, std::memory_order_relaxed );
	}
	inline void PhysFail_BinPhongTilt( double dotGS ) {
		const double a = dotGS < 0 ? -dotGS : dotGS;
		unsigned int slot;
		if      ( a > 0.999 ) slot = 0;
		else if ( a > 0.99  ) slot = 1;
		else if ( a > 0.95  ) slot = 2;
		else if ( a > 0.8   ) slot = 3;
		else                  slot = 4;
		g_physFail_phongTilt[slot].fetch_add( 1, std::memory_order_relaxed );
	}

	struct SolveDiagAtExitInstaller {
		SolveDiagAtExitInstaller() {
			std::atexit([](){
				const uint64_t calls   = g_solveDiag_calls.load();
				const uint64_t farSeed = g_solveDiag_seedTooFar.load();
				const uint64_t deriv   = g_solveDiag_derivFail.load();
				const uint64_t newton  = g_solveDiag_newtonFail.load();
				const uint64_t physics = g_solveDiag_physicsFail.load();
				const uint64_t shrt    = g_solveDiag_shortSeg.load();
				const uint64_t ok      = g_solveDiag_ok.load();
				std::fprintf( stderr,
					"[SMS-SOLVE-DIAG] calls=%llu  ok=%llu  seedTooFar=%llu  derivFail=%llu  newtonFail=%llu  physicsFail=%llu  shortSeg=%llu",
					(unsigned long long)calls, (unsigned long long)ok,
					(unsigned long long)farSeed, (unsigned long long)deriv,
					(unsigned long long)newton, (unsigned long long)physics,
					(unsigned long long)shrt );
				if( calls > 0 ) {
					std::fprintf( stderr,
						"  pcts: ok=%.1f%% farSeed=%.1f%% newton=%.1f%% physics=%.1f%% short=%.1f%%",
						100.0 * double(ok)      / double(calls),
						100.0 * double(farSeed) / double(calls),
						100.0 * double(newton)  / double(calls),
						100.0 * double(physics) / double(calls),
						100.0 * double(shrt)    / double(calls) );
				}
				std::fprintf( stderr, "\n" );
				const uint64_t b1 = g_solveDiag_newtonFail_lt1e3.load();
				const uint64_t b2 = g_solveDiag_newtonFail_lt1e2.load();
				const uint64_t b3 = g_solveDiag_newtonFail_lt1e1.load();
				const uint64_t b4 = g_solveDiag_newtonFail_lt1e0.load();
				const uint64_t b5 = g_solveDiag_newtonFail_ge1e0.load();
				const uint64_t btot = b1 + b2 + b3 + b4 + b5;
				if( btot > 0 ) {
					std::fprintf( stderr,
						"[SMS-NEWTONFAIL-RES] ||C|| histogram: <1e-3=%llu(%.1f%%) 1e-3..1e-2=%llu(%.1f%%) 1e-2..1e-1=%llu(%.1f%%) 1e-1..1=%llu(%.1f%%) >=1=%llu(%.1f%%)\n",
						(unsigned long long)b1, 100.0*double(b1)/double(btot),
						(unsigned long long)b2, 100.0*double(b2)/double(btot),
						(unsigned long long)b3, 100.0*double(b3)/double(btot),
						(unsigned long long)b4, 100.0*double(b4)/double(btot),
						(unsigned long long)b5, 100.0*double(b5)/double(btot) );
				}
				const uint64_t capChecks = g_solveDiag_capChecks.load();
				const uint64_t capFired  = g_solveDiag_capFired.load();
				if( capChecks > 0 ) {
					const double avgRatio = capFired > 0
						? double( g_solveDiag_capRatioSum.load() ) / 1000.0 / double( capFired )
						: 0.0;
					std::fprintf( stderr,
						"[SMS-STEPCAP] checks=%llu fired=%llu (%.2f%%)  avg(max_step/cap when fired)=%.2f\n",
						(unsigned long long)capChecks,
						(unsigned long long)capFired,
						100.0 * double( capFired ) / double( capChecks ),
						avgRatio );
				}
				const uint64_t lmIters     = g_solveDiag_lmTotalIters.load();
				const uint64_t lmDamped    = g_solveDiag_lmDamped.load();
				const uint64_t lmEscalated = g_solveDiag_lmEscalated.load();
				const uint64_t lmRecovered = g_solveDiag_lmRecovered.load();
				if( lmIters > 0 ) {
					std::fprintf( stderr,
						"[SMS-LM] iters=%llu damped=%llu(%.2f%%) escalations=%llu rescues=%llu\n",
						(unsigned long long)lmIters,
						(unsigned long long)lmDamped,
						100.0 * double( lmDamped ) / double( lmIters ),
						(unsigned long long)lmEscalated,
						(unsigned long long)lmRecovered );
				}
				const uint64_t edgeChecks  = g_solveDiag_edgeChecks.load();
				const uint64_t edgeRejects = g_solveDiag_edgeRejects.load();
				if( edgeChecks > 0 ) {
					std::fprintf( stderr,
						"[SMS-EDGE] checks=%llu rejects=%llu (%.2f%%)\n",
						(unsigned long long)edgeChecks,
						(unsigned long long)edgeRejects,
						100.0 * double( edgeRejects ) / double( edgeChecks ) );
				}
				// PHYS-FAIL ANATOMY dump
				const uint64_t pf_pos[4] = {
					g_physFail_byVtxPos[0].load(), g_physFail_byVtxPos[1].load(),
					g_physFail_byVtxPos[2].load(), g_physFail_byVtxPos[3].load(),
				};
				const uint64_t pf_op[2] = {
					g_physFail_byOp[0].load(), g_physFail_byOp[1].load(),
				};
				const uint64_t pf_cos[6] = {
					g_physFail_minCos[0].load(), g_physFail_minCos[1].load(),
					g_physFail_minCos[2].load(), g_physFail_minCos[3].load(),
					g_physFail_minCos[4].load(), g_physFail_minCos[5].load(),
				};
				const uint64_t pf_tilt[5] = {
					g_physFail_phongTilt[0].load(), g_physFail_phongTilt[1].load(),
					g_physFail_phongTilt[2].load(), g_physFail_phongTilt[3].load(),
					g_physFail_phongTilt[4].load(),
				};
				const uint64_t pf_shAgree  = g_physFail_shadingAgrees.load();
				const uint64_t pf_shMasked = g_physFail_shadingMasked.load();
				const uint64_t pf_fallback = g_physFail_geomFallback.load();
				const uint64_t pf_total = pf_pos[0]+pf_pos[1]+pf_pos[2]+pf_pos[3];
				if( pf_total > 0 ) {
					auto pct = [pf_total]( uint64_t x ){ return 100.0 * double(x) / double(pf_total); };
					std::fprintf( stderr,
						"[SMS-PHYSFAIL-ANATOMY] total=%llu  pos: sole=%llu(%.1f%%) first=%llu(%.1f%%) middle=%llu(%.1f%%) last=%llu(%.1f%%)\n",
						(unsigned long long)pf_total,
						(unsigned long long)pf_pos[0], pct(pf_pos[0]),
						(unsigned long long)pf_pos[1], pct(pf_pos[1]),
						(unsigned long long)pf_pos[2], pct(pf_pos[2]),
						(unsigned long long)pf_pos[3], pct(pf_pos[3]) );
					std::fprintf( stderr,
						"[SMS-PHYSFAIL-ANATOMY] op: refract=%llu(%.1f%%) reflect=%llu(%.1f%%)\n",
						(unsigned long long)pf_op[0], pct(pf_op[0]),
						(unsigned long long)pf_op[1], pct(pf_op[1]) );
					std::fprintf( stderr,
						"[SMS-PHYSFAIL-ANATOMY] min|cos| histogram: <1e-4=%llu(%.1f%%) 1e-4..1e-3=%llu(%.1f%%) 1e-3..1e-2=%llu(%.1f%%) 1e-2..1e-1=%llu(%.1f%%) 1e-1..0.5=%llu(%.1f%%) >=0.5=%llu(%.1f%%)\n",
						(unsigned long long)pf_cos[0], pct(pf_cos[0]),
						(unsigned long long)pf_cos[1], pct(pf_cos[1]),
						(unsigned long long)pf_cos[2], pct(pf_cos[2]),
						(unsigned long long)pf_cos[3], pct(pf_cos[3]),
						(unsigned long long)pf_cos[4], pct(pf_cos[4]),
						(unsigned long long)pf_cos[5], pct(pf_cos[5]) );
					std::fprintf( stderr,
						"[SMS-PHYSFAIL-ANATOMY] phong-tilt |dot(geomN,shadeN)|: >.999=%llu(%.1f%%) .99..999=%llu(%.1f%%) .95..99=%llu(%.1f%%) .8..95=%llu(%.1f%%) <.8=%llu(%.1f%%)\n",
						(unsigned long long)pf_tilt[0], pct(pf_tilt[0]),
						(unsigned long long)pf_tilt[1], pct(pf_tilt[1]),
						(unsigned long long)pf_tilt[2], pct(pf_tilt[2]),
						(unsigned long long)pf_tilt[3], pct(pf_tilt[3]),
						(unsigned long long)pf_tilt[4], pct(pf_tilt[4]) );
					std::fprintf( stderr,
						"[SMS-PHYSFAIL-ANATOMY] shading-vs-geom: shading-also-rejects=%llu(%.1f%%) shading-would-have-passed=%llu(%.1f%%) geom-fallback(no-geomN)=%llu(%.1f%%)\n",
						(unsigned long long)pf_shAgree,  pct(pf_shAgree),
						(unsigned long long)pf_shMasked, pct(pf_shMasked),
						(unsigned long long)pf_fallback, pct(pf_fallback) );

					// PUSHBACK ANATOMY dump.
					{
						const uint64_t spP = g_physFail_signProd_bothPos.load();
						const uint64_t spN = g_physFail_signProd_bothNeg.load();
						const uint64_t spT = spP + spN;
						if( spT > 0 ) {
							std::fprintf( stderr,
								"[SMS-PHYSFAIL-PUSHBACK] sign-product at fail: both-positive=%llu(%.1f%%) both-negative=%llu(%.1f%%)\n",
								(unsigned long long)spP, 100.0*double(spP)/double(spT),
								(unsigned long long)spN, 100.0*double(spN)/double(spT) );
						}
						const uint64_t svV = g_physFail_seedWasValid.load();
						const uint64_t svI = g_physFail_seedWasInvalid.load();
						const uint64_t svT = svV + svI;
						if( svT > 0 ) {
							std::fprintf( stderr,
								"[SMS-PHYSFAIL-PUSHBACK] seed-was-valid=%llu(%.1f%%)  seed-was-already-invalid=%llu(%.1f%%)  (\"valid\" = Newton dragged a good seed into a bad basin)\n",
								(unsigned long long)svV, 100.0*double(svV)/double(svT),
								(unsigned long long)svI, 100.0*double(svI)/double(svT) );
						}
						const uint64_t f6 = g_physFail_finalNorm_lt1e6.load();
						const uint64_t f4 = g_physFail_finalNorm_lt1e4.load();
						const uint64_t f2 = g_physFail_finalNorm_lt1e2.load();
						const uint64_t fL = g_physFail_finalNorm_ge1e2.load();
						const uint64_t fT = f6 + f4 + f2 + fL;
						if( fT > 0 ) {
							std::fprintf( stderr,
								"[SMS-PHYSFAIL-PUSHBACK] post-Newton ‖C‖ at rejected chain: <1e-6=%llu(%.1f%%) <1e-4=%llu(%.1f%%) <1e-2=%llu(%.1f%%) >=1e-2=%llu(%.1f%%)\n",
								(unsigned long long)f6, 100.0*double(f6)/double(fT),
								(unsigned long long)f4, 100.0*double(f4)/double(fT),
								(unsigned long long)f2, 100.0*double(f2)/double(fT),
								(unsigned long long)fL, 100.0*double(fL)/double(fT) );
						}

						const uint64_t cl[6] = {
							g_physFail_chainLen[0].load(), g_physFail_chainLen[1].load(),
							g_physFail_chainLen[2].load(), g_physFail_chainLen[3].load(),
							g_physFail_chainLen[4].load(), g_physFail_chainLen[5].load(),
						};
						const uint64_t clT = cl[0]+cl[1]+cl[2]+cl[3]+cl[4]+cl[5];
						if( clT > 0 ) {
							auto pctC = [clT]( uint64_t x ){ return 100.0*double(x)/double(clT); };
							std::fprintf( stderr,
								"[SMS-PHYSFAIL-PUSHBACK] chain-length: k=1=%llu(%.1f%%) k=2=%llu(%.1f%%) k=3=%llu(%.1f%%) k=4=%llu(%.1f%%) k=5=%llu(%.1f%%) k=6+=%llu(%.1f%%)\n",
								(unsigned long long)cl[0], pctC(cl[0]),
								(unsigned long long)cl[1], pctC(cl[1]),
								(unsigned long long)cl[2], pctC(cl[2]),
								(unsigned long long)cl[3], pctC(cl[3]),
								(unsigned long long)cl[4], pctC(cl[4]),
								(unsigned long long)cl[5], pctC(cl[5]) );
						}
						const uint64_t aR = g_physFail_anyReflectionInChain.load();
						const uint64_t aF = g_physFail_allRefractionInChain.load();
						const uint64_t arT = aR + aF;
						if( arT > 0 ) {
							std::fprintf( stderr,
								"[SMS-PHYSFAIL-PUSHBACK] chain has TIR/mirror=%llu(%.1f%%)  all-refraction=%llu(%.1f%%)\n",
								(unsigned long long)aR, 100.0*double(aR)/double(arT),
								(unsigned long long)aF, 100.0*double(aF)/double(arT) );
						}
					}

					// EXPERIMENT (c): NEWTON-STALL TOPOLOGY heatmap.
					{
						uint64_t failTotal = 0, okTotal = 0;
						for( unsigned int iu = 0; iu < kStallBins; iu++ ) {
							for( unsigned int iv = 0; iv < kStallBins; iv++ ) {
								failTotal += g_newtonFail_uv[iu][iv].load();
								okTotal   += g_newtonOk_uv[iu][iv].load();
							}
						}
						if( failTotal + okTotal > 0 ) {
							std::fprintf( stderr,
								"[SMS-NEWTON-STALL-TOPOLOGY] %ux%u (u, v) heatmap, vertex 0 of every Newton-failed and OK chain.\n"
								"[SMS-NEWTON-STALL-TOPOLOGY] u increases L→R, v increases T→B; cells: fail-rate %% = 100*fails/(fails+ok)\n",
								kStallBins, kStallBins );
							for( unsigned int iv = 0; iv < kStallBins; iv++ ) {
								std::fprintf( stderr, "[SMS-NEWTON-STALL-TOPOLOGY] row v=%2u  ", iv );
								for( unsigned int iu = 0; iu < kStallBins; iu++ ) {
									const uint64_t f = g_newtonFail_uv[iu][iv].load();
									const uint64_t o = g_newtonOk_uv[iu][iv].load();
									const uint64_t t = f + o;
									if( t == 0 ) std::fprintf( stderr, "  --   " );
									else         std::fprintf( stderr, " %5.1f ", 100.0 * double(f) / double(t) );
								}
								std::fprintf( stderr, "\n" );
							}
							std::fprintf( stderr,
								"[SMS-NEWTON-STALL-TOPOLOGY] totals: fails=%llu  ok=%llu  overall-fail-rate=%.2f%%\n",
								(unsigned long long)failTotal, (unsigned long long)okTotal,
								100.0 * double(failTotal) / double(failTotal + okTotal) );

							// PHYS-FAIL topology — same grid, fraction of chains
							// at each cell that converge to a wrong-topology
							// root (denominator: ok + phys-fail at that cell,
							// since a chain that's Newton-fail never reaches
							// the validator).
							uint64_t physFailTotal = 0;
							for( unsigned int iu = 0; iu < kStallBins; iu++ ) {
								for( unsigned int iv = 0; iv < kStallBins; iv++ ) {
									physFailTotal += g_physFail_uv[iu][iv].load();
								}
							}
							if( physFailTotal > 0 ) {
								std::fprintf( stderr,
									"[SMS-PHYSFAIL-TOPOLOGY] %ux%u (u, v) heatmap, vertex 0 of every phys-failed chain.\n"
									"[SMS-PHYSFAIL-TOPOLOGY] cells: phys-fail-rate %% = 100*phys-fails/(phys-fails+ok)  (Newton-fails excluded since they never reach the validator)\n",
									kStallBins, kStallBins );
								for( unsigned int iv = 0; iv < kStallBins; iv++ ) {
									std::fprintf( stderr, "[SMS-PHYSFAIL-TOPOLOGY] row v=%2u  ", iv );
									for( unsigned int iu = 0; iu < kStallBins; iu++ ) {
										const uint64_t pf = g_physFail_uv[iu][iv].load();
										const uint64_t o  = g_newtonOk_uv[iu][iv].load();
										const uint64_t t  = pf + o;
										if( t == 0 ) std::fprintf( stderr, "  --   " );
										else         std::fprintf( stderr, " %5.1f ", 100.0 * double(pf) / double(t) );
									}
									std::fprintf( stderr, "\n" );
								}
								std::fprintf( stderr,
									"[SMS-PHYSFAIL-TOPOLOGY] totals: phys-fails=%llu  (overall phys-fail-rate vs ok = %.2f%%)\n",
									(unsigned long long)physFailTotal,
									100.0 * double(physFailTotal) / double(physFailTotal + okTotal) );
							}
						}
					}
					// EXPERIMENT T2: pre-Newton topology check effectiveness.
					const uint64_t t2_v = g_t2_seedValidated.load();
					const uint64_t t2_s = g_t2_seedSkipped.load();
					if( t2_v > 0 ) {
						std::fprintf( stderr,
							"[SMS-T2-PRE-NEWTON-CHECK] validated=%llu  skipped-as-invalid=%llu (%.2f%% of validated)\n",
							(unsigned long long)t2_v,
							(unsigned long long)t2_s,
							100.0 * double(t2_s) / double(t2_v) );
					}
					// EXPERIMENT T3: post-Newton failing-vertex retry effectiveness.
					const uint64_t t3_a = g_t3_attempted.load();
					const uint64_t t3_r = g_t3_rescued.load();
					const uint64_t t3_i = g_t3_iters.load();
					if( t3_a > 0 ) {
						std::fprintf( stderr,
							"[SMS-T3-FAILING-VERTEX-RETRY] attempted=%llu rescued=%llu (%.2f%%)  total-newton-iters=%llu  avg-iters/attempt=%.2f\n",
							(unsigned long long)t3_a,
							(unsigned long long)t3_r,
							100.0 * double(t3_r) / double(t3_a),
							(unsigned long long)t3_i,
							double(t3_i) / double(t3_a) );
					}

					// EXPERIMENT E: perturbed-seed restart effectiveness.
					const uint64_t pr_att = g_physFailRestart_attempted.load();
					const uint64_t pr_res = g_physFailRestart_rescued.load();
					const uint64_t pr_it  = g_physFailRestart_iters.load();
					if( pr_att > 0 ) {
						std::fprintf( stderr,
							"[SMS-PHYSFAIL-RESTART] attempted=%llu rescued=%llu (%.2f%%)  total-perturbed-newton-iters=%llu  avg-iters/attempt=%.2f\n",
							(unsigned long long)pr_att,
							(unsigned long long)pr_res,
							100.0 * double(pr_res) / double(pr_att),
							(unsigned long long)pr_it,
							double(pr_it) / double(pr_att) );
					}

					// Cascade-anatomy: first-failing-vertex index distribution.
					const uint64_t pf_idx[6] = {
						g_physFail_firstFailIdx[0].load(), g_physFail_firstFailIdx[1].load(),
						g_physFail_firstFailIdx[2].load(), g_physFail_firstFailIdx[3].load(),
						g_physFail_firstFailIdx[4].load(), g_physFail_firstFailIdx[5].load(),
					};
					const uint64_t pf_idxTot = pf_idx[0]+pf_idx[1]+pf_idx[2]+pf_idx[3]+pf_idx[4]+pf_idx[5];
					if( pf_idxTot > 0 ) {
						auto pctI = [pf_idxTot]( uint64_t x ){ return 100.0 * double(x) / double(pf_idxTot); };
						std::fprintf( stderr,
							"[SMS-PHYSFAIL-CASCADE] first-fail-idx: i0=%llu(%.1f%%) i1=%llu(%.1f%%) i2=%llu(%.1f%%) i3=%llu(%.1f%%) i4=%llu(%.1f%%) i5+=%llu(%.1f%%)\n",
							(unsigned long long)pf_idx[0], pctI(pf_idx[0]),
							(unsigned long long)pf_idx[1], pctI(pf_idx[1]),
							(unsigned long long)pf_idx[2], pctI(pf_idx[2]),
							(unsigned long long)pf_idx[3], pctI(pf_idx[3]),
							(unsigned long long)pf_idx[4], pctI(pf_idx[4]),
							(unsigned long long)pf_idx[5], pctI(pf_idx[5]) );
					}

					// Cascade-anatomy: angle between vertex 0 geomN and
					// failing-vertex geomN.  Only populated for cascade
					// failures (i > 0); seeding failures (i = 0) skip this.
					const uint64_t pf_ang[6] = {
						g_physFail_v0_to_fail_angle[0].load(), g_physFail_v0_to_fail_angle[1].load(),
						g_physFail_v0_to_fail_angle[2].load(), g_physFail_v0_to_fail_angle[3].load(),
						g_physFail_v0_to_fail_angle[4].load(), g_physFail_v0_to_fail_angle[5].load(),
					};
					const uint64_t pf_angTot = pf_ang[0]+pf_ang[1]+pf_ang[2]+pf_ang[3]+pf_ang[4]+pf_ang[5];
					if( pf_angTot > 0 ) {
						auto pctA = [pf_angTot]( uint64_t x ){ return 100.0 * double(x) / double(pf_angTot); };
						std::fprintf( stderr,
							"[SMS-PHYSFAIL-CASCADE] v0-to-fail-angle: <5°=%llu(%.1f%%) 5..15°=%llu(%.1f%%) 15..45°=%llu(%.1f%%) 45..90°=%llu(%.1f%%) 90..135°=%llu(%.1f%%) >=135°=%llu(%.1f%%)\n",
							(unsigned long long)pf_ang[0], pctA(pf_ang[0]),
							(unsigned long long)pf_ang[1], pctA(pf_ang[1]),
							(unsigned long long)pf_ang[2], pctA(pf_ang[2]),
							(unsigned long long)pf_ang[3], pctA(pf_ang[3]),
							(unsigned long long)pf_ang[4], pctA(pf_ang[4]),
							(unsigned long long)pf_ang[5], pctA(pf_ang[5]) );
					}
				}
			});
		}
	};
	SolveDiagAtExitInstaller g_solveDiag_installer;
}
#endif
#include "../Interfaces/ILog.h"
#include "../Interfaces/IScene.h"
#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IEnumCallback.h"
#include "IndependentSampler.h"
#include "RandomNumbers.h"
#include "../Intersection/RayIntersection.h"
#include "../Lights/LightSampler.h"
#include "Color/RGBSpectra.h"		// Stage C slice 2: SMS source terms carry the reference illuminant
#include <cmath>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	//////////////////////////////////////////////////////////////////
	// DL-290: the SMS evaluation rigs rebuild a receiver record from
	// scratch, and a fresh `RayIntersectionGeometric` carries the
	// default `ambientIOR = 1.0` (air).  Every integrator stamps that
	// field from the IOR-stack top at a real hit, and G6 consumers
	// (GGX conductor / thin-film Fresnel, `coated_material`, the DL-49
	// SSS boundary, Cook-Torrance and the fibre models since DL-290)
	// read it, so a receiver in water or glass priced air under SMS
	// only.  The caller hands the receiver's live stack in (the same
	// pointer `valueStateful` already gets); its top IS the receiver's
	// exterior.  No stack, or an invalid top, is air -- bit-identical to
	// the pre-DL-290 record.
	//////////////////////////////////////////////////////////////////
	inline Scalar SMSReceiverAmbientIOR( const IORStack* pIorStack )
	{
		if( !pIorStack ) {
			return Scalar( 1.0 );
		}
		const Scalar n = pIorStack->top();
		return ( n > 0 && n < RISE_INFINITY ) ? n : Scalar( 1.0 );
	}

	// DL-347: a sampled emission point must be re-evaluated toward the
	// solved chain. Its sampled light-subpath direction is unrelated.
	inline RayIntersectionGeometric SMSEmitterContext(
		const LightSample& sample, const Vector3& out, Vector3& normal )
	{
		normal = EmitterSides::FaceToward(
			LightSampler::LuminaryIsTwoSided( sample.pLuminary ), sample.normal, out );
		RayIntersectionGeometric rig( Ray( sample.position, out ), nullRasterizerState );
		rig.bHit = true;
		rig.ptIntersection = sample.position;
		rig.vNormal = normal;
		rig.vGeomNormal = normal;
		rig.ptCoord = sample.ptCoord;
		rig.ptObjIntersec = sample.ptObjIntersec;
		rig.onb.CreateFromW( normal );
		LightSampler::ApplyEmitterSurface( rig, sample.surface );
		return rig;
	}

	inline RISEPel SMSAreaLe( const LightSample& sample, const Vector3& out )
	{
		if( !sample.pLuminary ) return sample.Le;
		if( !sample.pLuminary->GetMaterial() ) return RISEPel( 0, 0, 0 );
		const IEmitter* emitter = sample.pLuminary->GetMaterial()->GetEmitter();
		if( !emitter ) return RISEPel( 0, 0, 0 );
		Vector3 normal;
		const auto rig = SMSEmitterContext( sample, out, normal );
		return emitter->emittedRadiance( rig, out, normal );
	}

	inline Scalar SMSAreaLeNM( const LightSample& sample, const Vector3& out, const Scalar nm )
	{
		if( !sample.pLuminary || !sample.pLuminary->GetMaterial() ) return 0;
		const IEmitter* emitter = sample.pLuminary->GetMaterial()->GetEmitter();
		if( !emitter ) return 0;
		Vector3 normal;
		const auto rig = SMSEmitterContext( sample, out, normal );
		return emitter->emittedRadianceNM( rig, out, normal, nm );
	}

	// Legacy delta samples without an ILight pointer have only RGB Le.
	// Preserve the illuminant uplift for that fallback and environment
	// samples; real area emitters now use their own emittedRadianceNM.
	inline Scalar SMSLeNM( const RISEPel& Le, const Scalar nm )
	{
		RISEPel c = Le;
		// FromRGB scales by the max channel; one negative component
		// would flip that scale and corrupt every wavelength.
		ColorMath::EnsurePositve( c );
		return RGBIlluminantSpectrum::FromRGB( c ).Eval( nm );
	}
}

//////////////////////////////////////////////////////////////////////
// Construction / Destruction
//////////////////////////////////////////////////////////////////////

ManifoldSolver::ManifoldSolver(const ManifoldSolverConfig& cfg) : ManifoldSolver(cfg,false) {}

ManifoldSolver::ManifoldSolver( const ManifoldSolverConfig& cfg, bool nativeEvents, const std::vector<SMSDomainVertex>* contexts, Scalar wavelength ) :
config( cfg ),
    nativeEventConstraints(nativeEvents),
    nativeContexts(contexts),
    nativeWavelength(wavelength),
pLightSampler( 0 ),
pPhotonMap( 0 ),
mHasPureMirrorCaster( false )
{
    if(!nativeEvents) pLightSampler = new LightSampler();
}

namespace
{
	//! Snell mode's pure-mirror classification of a caster (the probe the
	//! supplemental-seed loop in EvaluateAtShadingPoint has always used):
	//! a deterministic mid-surface sample's specular info, canRefract
	//! false.  Shared so SetSpecularCasters' exactness flag and the loop
	//! classify casters identically.
	bool ProbeIsPureMirrorCaster( const IObject* pCaster )
	{
		if( !pCaster ) return false;
		const IMaterial* pMat = pCaster->GetMaterial();
		if( !pMat ) return false;
		Point3 probePos;
		Vector3 probeNormal;
		Point2 probeUv;
		pCaster->UniformRandomPoint(
			&probePos, &probeNormal, &probeUv,
			Point3( 0.5, 0.5, 0.5 ) );
		Ray probeRay( probePos, probeNormal );
		RayIntersectionGeometric probeRig( probeRay, nullRasterizerState );
		probeRig.bHit          = true;
		probeRig.ptIntersection = probePos;
		probeRig.vNormal       = probeNormal;
		probeRig.ptCoord       = probeUv;
		IORStack probeIor( 1.0 );
		const SpecularInfo probeSpec = pMat->GetSpecularInfo( probeRig, probeIor );
		return !probeSpec.canRefract;
	}
}

void ManifoldSolver::SetSpecularCasters( std::vector<const IObject*> list )
{
    hwssExtendedWarningEmitted.store(false, std::memory_order_relaxed);
	mSpecularCasters = std::move( list );
	mHasPureMirrorCaster = false;
	for( const IObject* pCaster : mSpecularCasters ) {
		if( ProbeIsPureMirrorCaster( pCaster ) ) {
			mHasPureMirrorCaster = true;
			break;
		}
	}
}

ManifoldSolver::~ManifoldSolver()
{
	safe_release( pLightSampler );
}

//////////////////////////////////////////////////////////////////////
// ComputeSpecularDirection
//
//   Convention for the manifold solver:
//     wi = unit direction from vertex TOWARD the previous vertex
//          (pointing away from surface on the incoming side)
//     wo = unit direction from vertex TOWARD the next vertex
//          (pointing away from surface on the outgoing side)
//     normal = outward surface normal
//////////////////////////////////////////////////////////////////////

namespace
{
	// Resolve the (η_i, η_t) pair to use for a vertex's half-vector /
	// Snell / Fresnel math.  Two paths:
	//
	//   1. Modern: BuildSeedChain (RGB and NM) populates v.etaI and
	//      v.etaT explicitly using the IOR stack.  This works for both
	//      single dielectric in air AND nested dielectric scenes.
	//
	//   2. Back-compat: hand-constructed chains in the existing test
	//      corpus (ManifoldSolverTest.cpp) often only set v.eta and
	//      leave (etaI, etaT) at their default (1.0, 1.0).  Those
	//      tests assume "the other side of every interface is air"
	//      — which is exactly what the OLD `eta_eff = isExiting ?
	//      1/eta : eta` formula computed.  When we detect this case
	//      (both etaI and etaT at default 1.0 BUT v.eta != 1.0), we
	//      fall back to the air-as-other-side assumption so no
	//      pre-existing test breaks.
	//
	// A consequence: a test that explicitly wants etaI=1.0 AND
	// etaT=1.0 AND eta != 1.0 (a degenerate / pathological case)
	// would get the back-compat path instead, but no such test
	// exists in the current corpus.  If one is added, populate
	// (etaI, etaT) explicitly.
	inline void GetEffectiveEtas(
		const RISE::Implementation::ManifoldVertex& v,
		Scalar& eta_i,
		Scalar& eta_t )
	{
		const bool backCompat =
			( v.etaI == Scalar( 1.0 ) ) &&
			( v.etaT == Scalar( 1.0 ) ) &&
			( v.eta  != Scalar( 1.0 ) );
		if( backCompat ) {
			if( v.isExiting ) {
				eta_i = v.eta;
				eta_t = Scalar( 1.0 );
			} else {
				eta_i = Scalar( 1.0 );
				eta_t = v.eta;
			}
		} else {
			eta_i = v.etaI;
			eta_t = v.etaT;
		}
	}

	// DL-290 review P1-1 and round 2 (P2-2): which form of Walter's
	// generalized half-vector a REFRACTION vertex's constraint uses.
	//
	// The constraint is C = P_t(h), the projection of
	// h = -(eta_i wi + eta_t wo) onto the vertex's tangent plane.  The
	// solver used to project the NORMALIZED h / |h|.  Both vanish on the
	// same set wherever h != 0, but |h| -> 0 as the two indices approach
	// each other on a nearly straight path, and at an index-MATCHED
	// vertex (the same index on both sides -- two same-index objects
	// entered in turn, the second of two open glass sheets, a re-entered
	// tessellated caster) h is IDENTICALLY zero at the solution: the
	// normalized form is 0/0 there and has no root at all, and for a
	// NEAR-matched vertex it is a root of arbitrarily steep slope.  Round
	// 1 special-cased exact matches (|eta_i - eta_t| <= 1e-9 relative),
	// which left a cliff one ulp-scale step away: on two concentric
	// spheres 2.2 / 2.2 + dn, Newton converged 100 % at dn = 0 and
	// 0 % at dn = 1e-8 .. 1e-3 (ExteriorIndexInvarianceTest A8).
	//
	// So every refraction vertex uses the UNNORMALIZED h.  Why that is
	// the right constraint, including at a match:
	//   * Away from h = 0 it has the same zero set as the normalized
	//     form, and at a root the two Jacobians differ only by the row
	//     scaling 1/|h| of that vertex's two rows (the derivative of the
	//     1/|h| factor multiplies P_t(h) = 0).  A per-vertex row scaling
	//     D leaves the Newton root and the chain-to-light sensitivity
	//     dx/dy = -(DA)^-1 (DB) = -A^-1 B -- the only thing the
	//     generalized geometric term reads -- unchanged.  Newton's
	//     ITERATES do differ (only the root is shared).
	//   * At a matched vertex the row-scaling argument does NOT apply
	//     (1/|h| is undefined at the root).  There the justification is
	//     direct: C = P_t(-eta (wi + wo)) vanishes exactly on the
	//     straight-through path and is a full-rank local defining
	//     function of it (moving the vertex along the surface bends the
	//     segment), so the implicit-function tangent it yields is the
	//     physical one.  Measured: the light-to-first-vertex Jacobian
	//     determinant matches finite differences at a matched vertex to
	//     ~1e-5 relative.
	//   * Its magnitude is ~eta x (angular error), so it avoids the
	//     normalized form's matched-index singularity. Absolute IOR still
	//     scales this residual: fixed solver and seed-rejection thresholds
	//     are not invariant under a common scaling of both indices. The
	//     seed-rejection limitation is recorded in DL49 doc section 11.
	// Reflection vertices keep the normalized h = wi + wo, which never
	// vanishes on a physical path.
	inline bool UseUnnormalizedHalfVector( const RISE::Implementation::ManifoldVertex& v )
	{
		return !v.isReflection;
	}
}

bool ManifoldSolver::ComputeSpecularDirection(
	const Vector3& wi,
	const Vector3& normal,
	Scalar eta,
	bool isReflection,
	Vector3& wo
	) const
{
	if( isReflection )
	{
		// Reflection: wo = wi - 2*(wi.n)*n
		// Using Optics::CalculateReflectedRay expects vIn pointing toward
		// the surface.  Our wi points away from surface, so negate it.
		// reflected = vIn - 2*(vIn.n)*n
		// We want: wo = (-wi) - 2*((-wi).n)*n = -wi + 2*(wi.n)*n
		const Scalar d = Vector3Ops::Dot( wi, normal );
		wo = Vector3(
			-wi.x + 2.0 * d * normal.x,
			-wi.y + 2.0 * d * normal.y,
			-wi.z + 2.0 * d * normal.z
			);
		wo = Vector3Ops::Normalize( wo );
		return true;
	}
	else
	{
		// Refraction via Snell's law
		// wi points away from surface toward incoming side
		// We compute the refracted direction on the other side
		//
		// The standard formula uses eta_ratio = n_incoming / n_transmitted.
		// When entering glass (cos_i > 0, wi on normal side):
		//   n_incoming = 1 (air), n_transmitted = eta (glass)
		//   eta_ratio = 1/eta
		// When exiting glass (cos_i < 0, wi opposite to normal):
		//   n_incoming = eta (glass), n_transmitted = 1 (air)
		//   eta_ratio = eta
		const Scalar cos_i = Vector3Ops::Dot( wi, normal );

		Vector3 n = normal;
		Scalar eta_ratio = 1.0 / eta;   // Default: entering glass
		Scalar ci = cos_i;

		if( ci < 0.0 )
		{
			// wi is on the opposite side of the normal — exiting glass
			n = Vector3( -normal.x, -normal.y, -normal.z );
			ci = -ci;
			eta_ratio = eta;  // glass → air: n_glass / n_air
		}

		Scalar cos_t;
		if( !Optics::CalculateRefractedCosine( ci, eta_ratio, 1.0, cos_t ) ) return false;

		// Refracted direction: wt = -eta_ratio * wi + (eta_ratio * ci - cos_t) * n
		// This gives a direction pointing away from the surface on the transmitted side
		wo = Vector3(
			-eta_ratio * wi.x + (eta_ratio * ci - cos_t) * n.x,
			-eta_ratio * wi.y + (eta_ratio * ci - cos_t) * n.y,
			-eta_ratio * wi.z + (eta_ratio * ci - cos_t) * n.z
			);
		wo = Vector3Ops::Normalize( wo );
		return true;
	}
}

//////////////////////////////////////////////////////////////////////
// ComputeSpecularDirectionDerivativeWrtNormal
//
//   Computes the 3x3 Jacobian d(wo)/d(n), stored row-major.
//
//   Used by the test-only angle-difference Jacobian. The production
//   half-vector Jacobian (BuildJacobian) captures surface curvature
//   via the Weingarten map without calling this derivative.
//
//   NOTE: this derivative assumes unnormalized wo (the raw output
//   of ComputeSpecularDirection before normalization).  If used with
//   the normalized version, apply the DeriveNormalized correction.
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::ComputeSpecularDirectionDerivativeWrtNormal(
	const Vector3& wi,
	const Vector3& normal,
	Scalar eta,
	bool isReflection,
	Scalar dwo_dn[9]
	) const
{
	if( isReflection )
	{
		// wo = -wi + 2*(wi.n)*n
		// d(wo)/d(n) = 2*(wi.n)*I + 2*outer(n, wi)
		// where outer(a,b)_ij = a_i * b_j
		const Scalar d = Vector3Ops::Dot( wi, normal );

		// Row 0
		dwo_dn[0] = 2.0 * d + 2.0 * normal.x * wi.x;		// d(wo.x)/d(n.x)
		dwo_dn[1] = 2.0 * normal.x * wi.y;				// d(wo.x)/d(n.y)
		dwo_dn[2] = 2.0 * normal.x * wi.z;				// d(wo.x)/d(n.z)
		// Row 1
		dwo_dn[3] = 2.0 * normal.y * wi.x;				// d(wo.y)/d(n.x)
		dwo_dn[4] = 2.0 * d + 2.0 * normal.y * wi.y;		// d(wo.y)/d(n.y)
		dwo_dn[5] = 2.0 * normal.y * wi.z;				// d(wo.y)/d(n.z)
		// Row 2
		dwo_dn[6] = 2.0 * normal.z * wi.x;				// d(wo.z)/d(n.x)
		dwo_dn[7] = 2.0 * normal.z * wi.y;				// d(wo.z)/d(n.y)
		dwo_dn[8] = 2.0 * d + 2.0 * normal.z * wi.z;		// d(wo.z)/d(n.z)
	}
	else
	{
		// With no index boundary, transmission is independent of the normal.
		if( eta == 1.0 ) {
			for( unsigned int i=0; i<9; ++i ) dwo_dn[i] = 0.0;
			return;
		}
		// Match ComputeSpecularDirection: orient the normal toward wi and
		// use the incoming/transmitted ratio for that side of the interface.
		const Scalar signedCos = Vector3Ops::Dot( wi, normal );
		const Scalar side = signedCos < 0.0 ? -1.0 : 1.0;
		const Scalar ratio = side < 0.0 ? eta : 1.0 / eta;
		const Scalar cos_i = fabs(signedCos);
		Scalar cos_t = 0.0;
		Optics::CalculateRefractedCosine( cos_i, ratio, 1.0, cos_t );
		// Retain the existing finite regularization at the critical boundary.
		// Callers classify TIR using the direction function before using this.
		if( cos_t < NEARZERO ) cos_t = NEARZERO;

		// rawWo = -ratio*wi + (ratio*cos_i-cos_t)*(side*normal).
		// Differentiating with respect to the ORIGINAL normal puts side
		// on the identity term; its two factors cancel in the outer product.
		const Scalar mu = side * (ratio * cos_i - cos_t);
		const Scalar factor = ratio * (1.0 - ratio * cos_i / cos_t);

		// Row 0
		dwo_dn[0] = factor * wi.x * normal.x + mu;
		dwo_dn[1] = factor * wi.y * normal.x;
		dwo_dn[2] = factor * wi.z * normal.x;
		// Row 1
		dwo_dn[3] = factor * wi.x * normal.y;
		dwo_dn[4] = factor * wi.y * normal.y + mu;
		dwo_dn[5] = factor * wi.z * normal.y;
		// Row 2
		dwo_dn[6] = factor * wi.x * normal.z;
		dwo_dn[7] = factor * wi.y * normal.z;
		dwo_dn[8] = factor * wi.z * normal.z + mu;
	}
}

//////////////////////////////////////////////////////////////////////
// DirectionToSpherical
//
//   Converts a direction to spherical coords (theta, phi) in the
//   local tangent frame defined by dpdu, dpdv, normal.
//////////////////////////////////////////////////////////////////////

Point2 ManifoldSolver::DirectionToSpherical(
	const Vector3& dir,
	const Vector3& dpdu,
	const Vector3& dpdv,
	const Vector3& normal
	) const
{
	const Vector3 u = Vector3Ops::Normalize( dpdu );
	const Vector3 v = Vector3Ops::Normalize( dpdv );

	const Scalar x = Vector3Ops::Dot( dir, u );
	const Scalar y = Vector3Ops::Dot( dir, v );
	const Scalar z = Vector3Ops::Dot( dir, normal );

	// Clamp z to [-1, 1] for numerical safety
	Scalar cz = z;
	if( cz > 1.0 ) cz = 1.0;
	if( cz < -1.0 ) cz = -1.0;

	const Scalar theta = acos( cz );
	const Scalar phi = atan2( y, x );

	return Point2( theta, phi );
}

//////////////////////////////////////////////////////////////////////
// EvaluateConstraint
//
//   Half-vector constraint (Zeltner et al. 2020).
//
//   For each specular vertex i, construct the generalized half-vector:
//     Refraction: h = -(wi + eta_eff * wo),  normalized
//     Reflection: h =   wi + wo,              normalized
//   where eta_eff accounts for entering vs exiting the medium.
//
//   When the specular constraint is exactly satisfied, h is parallel
//   to the surface normal.  The constraint measures the tangent-plane
//   projection of h, which must be zero:
//     C[2i]   = dot(dpdu, h)
//     C[2i+1] = dot(dpdv, h)
//
//   Audited native-frame constraints additionally retain the native outgoing
//   vector length in wo, since Optics tolerates nearly unit normals whose
//   scattered vectors are not exactly unit. Their projection basis is unit.
//   This avoids the angular wrapping issues of the spherical-coordinate
//   formulation and yields a well-conditioned Jacobian.
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::EvaluateConstraint(
	const std::vector<ManifoldVertex>& chain,
	const Point3& fixedStart,
	const Point3& fixedEnd,
	std::vector<Scalar>& C
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );
	C.resize( 2 * k, 0.0 );

	for( unsigned int i = 0; i < k; i++ )
	{
        // Legacy evaluations retain the original reference, with no copy
        // or allocation of a vertex or its optional endpoint record.
        std::optional<ManifoldVertex> nativeVertex;
        if(nativeEventConstraints) nativeVertex.emplace(SMSNativeConstraintVertex(
            chain[i],i?chain[i-1].position:fixedStart,NativeRaster(i),nullptr,NativeSceneObjects(i),nativeWavelength));
        const ManifoldVertex& v=nativeVertex?*nativeVertex:chain[i];
        if(nativeEventConstraints && !v.valid) { C[2*i]=C[2*i+1]=1;continue; }

		// Previous and next positions
		Point3 prevPos = (i == 0) ? fixedStart : chain[i-1].position;
		Point3 nextPos = (i == k-1) ? fixedEnd : chain[i+1].position;

		// wi = direction from vertex toward previous vertex
		Vector3 wi = Vector3Ops::mkVector3( prevPos, v.position );
		wi = Vector3Ops::Normalize( wi );

		// wo = direction from vertex toward next vertex
		Vector3 wo = Vector3Ops::mkVector3( nextPos, v.position );
		wo = Vector3Ops::Normalize( wo );

		// Construct the generalized half-vector
		Vector3 h;
		bool rawHalfVector = false;	// DL-290: see UseUnnormalizedHalfVector

		if( v.isReflection )
		{
			// Reflection: h = wi + wo
			h = Vector3( wi.x + wo.x, wi.y + wo.y, wi.z + wo.z );
		}
		else
		{
			// Refraction: Walter et al. 2007 generalized half-vector
			//   h ∝ -(η_i wi + η_t wo)
			// where η_i is the IOR on the wi side (incoming medium) and
			// η_t is the IOR on the wo side (outgoing medium).  Pulled
			// from BuildSeedChain's per-vertex (etaI, etaT) population.
			//
			// The OLD formula `h = -(wi + eta_eff*wo)` with `eta_eff =
			// isExiting ? 1/eta : eta` is mathematically equivalent for
			// the air-on-the-other-side case (it differs by a constant
			// factor of η_i, which the normalization downstream washes
			// out).  But it silently hardcodes "the other side is air"
			// — wrong for nested dielectrics like an air-cavity inside
			// glass, where the inner-cavity vertex has η=1.5 on one
			// side and η=1.0 on the other.  Walter's form handles
			// nested dielectrics correctly.
			//
			// GetEffectiveEtas falls back to the old air-as-other-side
			// behaviour when (etaI, etaT) are at default-1.0 — this
			// preserves bit-exact behaviour for the existing test
			// corpus that hand-constructs vertices with only `eta`
			// set.  See GetEffectiveEtas docstring for full rationale.
			Scalar eta_i, eta_t;
			GetEffectiveEtas( v, eta_i, eta_t );
			rawHalfVector = UseUnnormalizedHalfVector( v );

			h = Vector3(
				-(eta_i * wi.x + eta_t * wo.x),
				-(eta_i * wi.y + eta_t * wo.y),
				-(eta_i * wi.z + eta_t * wo.z)
			);
		}

        if(nativeEventConstraints && SMSNeedsNativeFrame(chain[i])) {
            Vector3 unusedDerivative;
            if(!SMSNativeHalfVector(wi,wo,v.normal,v.etaI,v.etaT,v.isReflection,
                Vector3(0,0,0),Vector3(0,0,0),Vector3(0,0,0),h,unusedDerivative)) {
                C[2*i]=C[2*i+1]=1;continue;
            }
        }

		// Normalize h -- reflection vertices only; a refraction vertex's
		// constraint is the UNNORMALIZED h (see UseUnnormalizedHalfVector).
		// The hLen < NEARZERO bail therefore guards reflections only.
		if( !rawHalfVector )
		{
			Scalar hLen = Vector3Ops::Magnitude( h );
			if( hLen < NEARZERO )
			{
				// Degenerate — set large constraint
				C[2*i]   = 1.0;
				C[2*i+1] = 1.0;
				continue;
			}
			h = h * (1.0 / hLen);
		}

		// Tangent-plane projection: when Snell's law is satisfied,
		// h is parallel to the normal, so these projections are zero.
		//
		// Use the Gram-Schmidt-projected tangent basis
		//     s = normalize(dpdu - (dpdu·n) n)
		//     t = n × s
		// (same convention as Cycles MNEE and BuildJacobian below).
		// This matters for curved surfaces: the BuildJacobian's ds/du
		// and dt/du analytical terms assume s is defined this way.  If
		// EvaluateConstraint used plain Normalize(dpdu) instead, the
		// Jacobian wouldn't match dC/dx and Newton would fail to
		// converge on non-flat surfaces.
		const Vector3 projectionNormal=nativeEventConstraints && SMSNeedsNativeFrame(chain[i])
            ? Vector3Ops::Normalize(v.normal):v.normal;
        const Scalar dpdu_dot_n = Vector3Ops::Dot( v.dpdu, projectionNormal );
		Vector3 s_unnorm(
			v.dpdu.x - dpdu_dot_n * projectionNormal.x,
			v.dpdu.y - dpdu_dot_n * projectionNormal.y,
			v.dpdu.z - dpdu_dot_n * projectionNormal.z );
		const Scalar s_len = Vector3Ops::Magnitude( s_unnorm );
		Vector3 s = (s_len > NEARZERO) ? (s_unnorm * (1.0 / s_len)) :
			Vector3Ops::Normalize( v.dpdu );
		Vector3 t = Vector3Ops::Cross( projectionNormal, s );

		C[2*i]   = Vector3Ops::Dot( s, h );
		C[2*i+1] = Vector3Ops::Dot( t, h );
	}
}

//////////////////////////////////////////////////////////////////////
// EvaluateConstraintAtVertex
//
//   Angle-difference constraint (Zeltner et al. 2020).
//
//   For a specular vertex, the constraint measures the angular
//   deviation between the actual outgoing direction wo and the
//   specularly scattered direction wo_spec:
//
//     C0 = theta(wo) - theta(wo_spec)
//     C1 = wrapToPi( phi(wo) - phi(wo_spec) )
//
//   where theta and phi are spherical coordinates in the local
//   tangent frame (s, t, normal).
//
//   This avoids the back-facing degeneracies of the half-vector
//   projection and provides larger convergence basins for Newton
//   iteration with distant initial guesses.
//////////////////////////////////////////////////////////////////////

// NOTE: EvaluateConstraintAtVertex (this function), BuildJacobianAngleDiff,
// and BuildJacobianAngleDiffNumerical use the angle-difference constraint
// form via ComputeSpecularDirection.  ComputeSpecularDirection silently
// assumes "the other side of the interface is air" — same nested-dielectric
// bug as the OLD eta_eff formula in EvaluateConstraint.  Production code
// (ManifoldSolver::Solve) uses the half-vector form via EvaluateConstraint
// and BuildJacobian, both of which have been fixed via GetEffectiveEtas to
// use the per-vertex (etaI, etaT) populated by BuildSeedChain.
//
// The angle-diff functions are TEST-ONLY (called from ManifoldSolverTest.cpp
// to validate the analytical-vs-numerical Jacobian agreement on flat single-
// IOR test geometry).  Their air-on-other-side assumption holds for the
// existing test corpus.  If a future test wants to exercise nested-
// dielectric topology via the angle-diff path, ComputeSpecularDirection
// would need an (eta_i, eta_t) overload — left as a future cleanup.
void ManifoldSolver::EvaluateConstraintAtVertex(
	const Point3& vertexPos,
	const Vector3& vertexNormal,
	const Vector3& vertexDpdu,
	const Vector3& vertexDpdv,
	Scalar vertexEta,
	bool vertexIsReflection,
	const Point3& prevPos,
	const Point3& nextPos,
	Scalar& C0,
	Scalar& C1
	) const
{
	// Direction from vertex toward previous vertex (incoming)
	Vector3 wi = Vector3Ops::mkVector3( prevPos, vertexPos );
	const Scalar wiLen = Vector3Ops::NormalizeMag( wi );

	// Direction from vertex toward next vertex (actual outgoing)
	Vector3 wo_actual = Vector3Ops::mkVector3( nextPos, vertexPos );
	const Scalar woLen = Vector3Ops::NormalizeMag( wo_actual );

	if( wiLen < NEARZERO || woLen < NEARZERO )
	{
		C0 = 1.0;
		C1 = 1.0;
		return;
	}

	// Compute the specularly scattered direction
	Vector3 wo_specular;
	if( !ComputeSpecularDirection( wi, vertexNormal, vertexEta,
		vertexIsReflection, wo_specular ) )
	{
		// Total internal reflection for a refraction vertex — degenerate
		C0 = 1.0;
		C1 = 1.0;
		return;
	}

	// Convert both directions to spherical coordinates in the
	// local tangent frame
	const Point2 angles_actual = DirectionToSpherical(
		wo_actual, vertexDpdu, vertexDpdv, vertexNormal );
	const Point2 angles_specular = DirectionToSpherical(
		wo_specular, vertexDpdu, vertexDpdv, vertexNormal );

	// Theta difference (no periodicity issue)
	C0 = angles_actual.x - angles_specular.x;

	// Phi difference — must wrap to [-pi, pi] to handle periodicity
	// (Zeltner errata: without this, the solver does not realize that
	// a value near 2*pi is close to a zero and fails to converge)
	Scalar phi_diff = angles_actual.y - angles_specular.y;
	if( phi_diff > PI ) phi_diff -= 2.0 * PI;
	if( phi_diff < -PI ) phi_diff += 2.0 * PI;
	C1 = phi_diff;
}

//////////////////////////////////////////////////////////////////////
// BuildJacobianNumerical
//
//   Builds the block-tridiagonal Jacobian dC/dx via central finite
//   differences on the constraint function.  This matches whichever
//   constraint formulation EvaluateConstraint uses (angle-difference
//   or half-vector).
//
//   For each vertex i and each surface parameter p (u or v):
//     - Diagonal block: perturb vertex i position and normal,
//       evaluate C_i at (pos ± dp*eps, normal ± dn*eps)
//     - Upper block (i+1): perturb nextPos, evaluate C_i
//     - Lower block (i-1): perturb prevPos, evaluate C_i
//
//   Uses central differences: dC/dp ≈ (C(+eps) - C(-eps)) / (2*eps)
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::BuildJacobianNumerical(
	const std::vector<ManifoldVertex>& chain,
	const Point3& fixedStart,
	const Point3& fixedEnd,
	std::vector<Scalar>& diag,
	std::vector<Scalar>& upper,
	std::vector<Scalar>& lower
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );
	const Scalar eps = 1e-5;
	const Scalar inv2eps = 1.0 / (2.0 * eps);

	diag.resize( k * 4, 0.0 );
	if( k > 1 )
	{
		upper.resize( (k-1) * 4, 0.0 );
		lower.resize( (k-1) * 4, 0.0 );
	}
	else
	{
		upper.clear();
		lower.clear();
	}

	// Numerically differentiates the SAME half-vector constraint
	// that BuildJacobian uses analytically.  For each vertex j and
	// each surface parameter p (u or v), perturb vertex j's position
	// and normal, evaluate the full chain constraint, and extract
	// the finite-difference derivative for all constraint components
	// that depend on vertex j.

	for( unsigned int j = 0; j < k; j++ )
	{
		for( unsigned int p = 0; p < 2; p++ )
		{
			const Vector3& dp = (p == 0) ? chain[j].dpdu : chain[j].dpdv;
			const Vector3& dn = (p == 0) ? chain[j].dndu : chain[j].dndv;

			// Build perturbed chains
			std::vector<ManifoldVertex> chainPlus( chain );
			std::vector<ManifoldVertex> chainMinus( chain );

			chainPlus[j].position = Point3Ops::mkPoint3( chain[j].position, dp * eps );
			chainPlus[j].normal = Vector3Ops::Normalize( Vector3(
				chain[j].normal.x + dn.x * eps,
				chain[j].normal.y + dn.y * eps,
				chain[j].normal.z + dn.z * eps ) );

			chainMinus[j].position = Point3Ops::mkPoint3( chain[j].position, dp * (-eps) );
			chainMinus[j].normal = Vector3Ops::Normalize( Vector3(
				chain[j].normal.x - dn.x * eps,
				chain[j].normal.y - dn.y * eps,
				chain[j].normal.z - dn.z * eps ) );

			std::vector<Scalar> Cp, Cm;
			EvaluateConstraint( chainPlus, fixedStart, fixedEnd, Cp );
			EvaluateConstraint( chainMinus, fixedStart, fixedEnd, Cm );

			// Extract derivatives for constraint i w.r.t. vertex j
			// Diagonal: i == j
			diag[j*4 + 0 + p] = (Cp[2*j]   - Cm[2*j])   * inv2eps;
			diag[j*4 + 2 + p] = (Cp[2*j+1] - Cm[2*j+1]) * inv2eps;

			// Upper: vertex j affects constraint j-1 (if j > 0)
			// lower[(j-1)] maps vertex j to constraint j-1
			if( j > 0 )
			{
				unsigned int ci = j - 1;  // constraint index
				upper[ci*4 + 0 + p] = (Cp[2*ci]   - Cm[2*ci])   * inv2eps;
				upper[ci*4 + 2 + p] = (Cp[2*ci+1] - Cm[2*ci+1]) * inv2eps;
			}

			// Lower: vertex j affects constraint j+1 (if j < k-1)
			// lower[j] maps vertex j to constraint j+1
			if( j < k - 1 )
			{
				unsigned int ci = j + 1;  // constraint index
				lower[j*4 + 0 + p] = (Cp[2*ci]   - Cm[2*ci])   * inv2eps;
				lower[j*4 + 2 + p] = (Cp[2*ci+1] - Cm[2*ci+1]) * inv2eps;
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////
// BuildJacobian
//
//   Analytical Jacobian for the half-vector constraint
//   (Zeltner et al. 2020, adapted from their reference code).
//
//   C_i = (dot(s_i, h_i),  dot(t_i, h_i))
//
//   The Jacobian is block-tridiagonal because C_i depends only on
//   vertices i-1, i, i+1.  Each block is 2x2.
//
//   For each vertex i, we compute dC_i / dX_j analytically by
//   differentiating h with respect to vertex positions and
//   accounting for the tangent-frame variation (ds, dt) due to
//   normal curvature (dndu, dndv).
//////////////////////////////////////////////////////////////////////

Vector3 ManifoldSolver::DeriveNormalized( const Vector3& h, const Vector3& dv, Scalar vLen )
{
	if( vLen < NEARZERO ) return Vector3( 0, 0, 0 );
	const Scalar invLen = 1.0 / vLen;
	// (dv - h * dot(h, dv)) / |v|
	const Scalar proj = Vector3Ops::Dot( h, dv );
	return Vector3(
		(dv.x - h.x * proj) * invLen,
		(dv.y - h.y * proj) * invLen,
		(dv.z - h.z * proj) * invLen );
}

void ManifoldSolver::BuildJacobian(
	const std::vector<ManifoldVertex>& chain,
	const Point3& fixedStart,
	const Point3& fixedEnd,
	std::vector<Scalar>& diag,
	std::vector<Scalar>& upper,
	std::vector<Scalar>& lower,
	bool includeCurvature
	) const
{
    if(nativeEventConstraints && std::any_of(chain.begin(),chain.end(),SMSNeedsNativeFrame)) {
        const std::size_t k=chain.size();
        SMSWorkerScratchLease scratch(true,config.referenceCounters);
        const auto capacity=std::max<std::size_t>(config.maxChainDepth,k);
        auto& plus=scratch.Vertices(0,capacity);auto& minus=scratch.Vertices(1,capacity);
        auto& derivatives=scratch.Derivatives(2*capacity);
        diag.assign(4*k,0);upper.assign(k>1?4*(k-1):0,0);lower=upper;
        const Scalar h=std::cbrt(std::numeric_limits<Scalar>::epsilon())*Point3Ops::Distance(fixedStart,fixedEnd);
        for(std::size_t j=0;j<k;++j) for(unsigned column=0;column<2;++column) {
            bool regular=false;
            Scalar finestStep=h;

            // Three halved scales plus a noncommensurate scale detect
            // truncation and harmonic aliasing. Unresolved derivatives invalidate the solve;
            // reference pricing cannot silently use the coarse aliased value.
            for(unsigned base=0;base<16 && !regular;++base) {
                regular=h>0 && std::isfinite(h);
            for(unsigned refinement=0;refinement<4 && regular;++refinement) {
                const Scalar step=h/(std::pow(Scalar(2),base+std::min(refinement,2u))
                    *(refinement==3?std::sqrt(Scalar(2)):Scalar(1)));
                finestStep=step;
                plus=chain;minus=chain;
                regular=UpdateVertexOnSurface(plus[j],column?0:step,column?step:0,0,true)
                    && UpdateVertexOnSurface(minus[j],column?0:-step,column?-step:0,0,true);
                if(regular) {
                    derivatives[refinement].resize(2*k);
                    for(std::size_t i=0;i<k && regular;++i) {
                        regular=SMSNativeConstraintDifferential(chain[i],plus[i],minus[i],
                            i?chain[i-1].position:fixedStart,i?plus[i-1].position:fixedStart,i?minus[i-1].position:fixedStart,
                            i+1<k?chain[i+1].position:fixedEnd,i+1<k?plus[i+1].position:fixedEnd,i+1<k?minus[i+1].position:fixedEnd,
                            step,NativeRaster(i),derivatives[refinement][2*i],derivatives[refinement][2*i+1],NativeSceneObjects(i),nativeWavelength);
                    }
                }
            }
            if(regular) for(std::size_t i=0;i<2*k;++i) {
                const Scalar a=derivatives[0][i],b=derivatives[1][i],c=derivatives[2][i],d=derivatives[3][i];
                const Scalar coordinateScale=std::max({Scalar(1),std::fabs(chain[j].position.x),
                    std::fabs(chain[j].position.y),std::fabs(chain[j].position.z)});
                const Scalar tolerance=std::sqrt(std::numeric_limits<Scalar>::epsilon())*std::max({Scalar(1),std::fabs(a),std::fabs(b),std::fabs(c),std::fabs(d)})
                    +std::numeric_limits<Scalar>::epsilon()*coordinateScale/finestStep;
                if(!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c)
                    || !std::isfinite(d) || std::fabs(a-b)>tolerance || std::fabs(b-c)>tolerance
                    || std::fabs(c-d)>tolerance) regular=false;
            }
            }
            for(std::size_t i=0;i<k;++i) for(unsigned row=0;row<2;++row) {
                const Scalar value=regular?derivatives[3][2*i+row]:std::numeric_limits<Scalar>::quiet_NaN();
                if(i==j) diag[4*i+2*row+column]=value;
                else if(j==i+1) upper[4*i+2*row+column]=value;
                else if(i==j+1) lower[4*j+2*row+column]=value;
            }
        }
        return;
    }

	const unsigned int k = static_cast<unsigned int>( chain.size() );

	diag.resize( k * 4, 0.0 );
	if( k > 1 )
	{
		upper.resize( (k-1) * 4, 0.0 );
		lower.resize( (k-1) * 4, 0.0 );
	}
	else
	{
		upper.clear();
		lower.clear();
	}

	for( unsigned int i = 0; i < k; i++ )
	{
		const ManifoldVertex& v = chain[i];

		// Previous and next positions
		const Point3 prevPos = (i == 0) ? fixedStart : chain[i-1].position;
		const Point3 nextPos = (i == k-1) ? fixedEnd : chain[i+1].position;

		// Directions and distances
		Vector3 d_wi = Vector3Ops::mkVector3( prevPos, v.position );
		const Scalar dist_i = Vector3Ops::NormalizeMag( d_wi );
		const Vector3 wi = d_wi;

		Vector3 d_wo = Vector3Ops::mkVector3( nextPos, v.position );
		const Scalar dist_o = Vector3Ops::NormalizeMag( d_wo );
		const Vector3 wo = d_wo;

		if( dist_i < NEARZERO || dist_o < NEARZERO ) continue;

		const Scalar inv_li = 1.0 / dist_i;
		const Scalar inv_lo = 1.0 / dist_o;

		// Tangent frame (normalized)
		Vector3 s = Vector3Ops::Normalize( v.dpdu );
		Vector3 t = Vector3Ops::Normalize( v.dpdv );

		// Walter et al. 2007 generalized half-vector (η_i, η_t form).
		// Resolved from the per-vertex (etaI, etaT) populated by
		// BuildSeedChain's IOR-stack tracking; falls back to the
		// "air-on-the-other-side" assumption for hand-constructed test
		// chains that only set v.eta.  See GetEffectiveEtas docstring.
		//
		// The OLD `eta_eff` form (`isExiting ? 1/eta : eta`) is
		// equivalent to Walter's form divided by η_i — same direction
		// after normalization, same Jacobian after the chain rule (the
		// constant scaling cancels in DeriveNormalized).  But it
		// silently hardcodes "the other side is air" — wrong for
		// nested dielectrics like an air-cavity inside glass.
		Scalar eta_i_v = Scalar( 1.0 );
		Scalar eta_t_v = Scalar( 1.0 );
		if( !v.isReflection ) {
			GetEffectiveEtas( v, eta_i_v, eta_t_v );
		}
		// DL-290: a refraction vertex's constraint is the UNNORMALIZED h
		// (see UseUnnormalizedHalfVector), so its derivatives are the raw
		// ones, not DeriveNormalized's.
		const bool rawHalfVector = UseUnnormalizedHalfVector( v );

		// Half-vector (unnormalized)
		Vector3 h_raw;
		if( v.isReflection )
		{
			h_raw = Vector3( wi.x + wo.x, wi.y + wo.y, wi.z + wo.z );
		}
		else
		{
			h_raw = Vector3(
				-(eta_i_v * wi.x + eta_t_v * wo.x),
				-(eta_i_v * wi.y + eta_t_v * wo.y),
				-(eta_i_v * wi.z + eta_t_v * wo.z) );
		}

		Scalar h_len = Vector3Ops::Magnitude( h_raw );
		if( !rawHalfVector && h_len < NEARZERO ) continue;
		Vector3 h = rawHalfVector ? h_raw : h_raw * (1.0 / h_len);

		// ---- Derivative of h w.r.t. moving vertex i ----
		//
		// Moving vertex i by dp changes both wi and wo:
		//   dwi/dp = -(I - wi⊗wi) / dist_i * dp  (wi moves toward prev)
		//   dwo/dp = -(I - wo⊗wo) / dist_o * dp  (wo moves toward next)
		//
		// For h = sign * (wi + eta*wo) / |...|, with dp = dpdu:
		//   dh_raw/dp = sign * (dwi/dp + eta * dwo/dp)
		//   dh/dp = (dh_raw/dp - h * dot(h, dh_raw/dp)) / h_len
		//
		// We compute dh for dp = dpdu and dp = dpdv separately.

		// For each surface parameter (u, v):
		Vector3 dh_du, dh_dv;
		{
			// dp/du = dpdu, dp/dv = dpdv
			// dwi/du = -(dpdu - wi * dot(wi, dpdu)) / dist_i
			const Vector3 dwi_du = Vector3(
				-(v.dpdu.x - wi.x * Vector3Ops::Dot( wi, v.dpdu )) * inv_li,
				-(v.dpdu.y - wi.y * Vector3Ops::Dot( wi, v.dpdu )) * inv_li,
				-(v.dpdu.z - wi.z * Vector3Ops::Dot( wi, v.dpdu )) * inv_li );
			const Vector3 dwo_du = Vector3(
				-(v.dpdu.x - wo.x * Vector3Ops::Dot( wo, v.dpdu )) * inv_lo,
				-(v.dpdu.y - wo.y * Vector3Ops::Dot( wo, v.dpdu )) * inv_lo,
				-(v.dpdu.z - wo.z * Vector3Ops::Dot( wo, v.dpdu )) * inv_lo );

			Vector3 dh_raw_du;
			if( v.isReflection )
			{
				dh_raw_du = Vector3( dwi_du.x + dwo_du.x, dwi_du.y + dwo_du.y, dwi_du.z + dwo_du.z );
			}
			else
			{
				// Walter form: ∂h/∂p = -(η_i ∂wi/∂p + η_t ∂wo/∂p)
				dh_raw_du = Vector3(
					-(eta_i_v * dwi_du.x + eta_t_v * dwo_du.x),
					-(eta_i_v * dwi_du.y + eta_t_v * dwo_du.y),
					-(eta_i_v * dwi_du.z + eta_t_v * dwo_du.z) );
			}
			dh_du = rawHalfVector ? dh_raw_du : DeriveNormalized( h, dh_raw_du, h_len );

			// Same for dv
			const Vector3 dwi_dv = Vector3(
				-(v.dpdv.x - wi.x * Vector3Ops::Dot( wi, v.dpdv )) * inv_li,
				-(v.dpdv.y - wi.y * Vector3Ops::Dot( wi, v.dpdv )) * inv_li,
				-(v.dpdv.z - wi.z * Vector3Ops::Dot( wi, v.dpdv )) * inv_li );
			const Vector3 dwo_dv = Vector3(
				-(v.dpdv.x - wo.x * Vector3Ops::Dot( wo, v.dpdv )) * inv_lo,
				-(v.dpdv.y - wo.y * Vector3Ops::Dot( wo, v.dpdv )) * inv_lo,
				-(v.dpdv.z - wo.z * Vector3Ops::Dot( wo, v.dpdv )) * inv_lo );

			Vector3 dh_raw_dv;
			if( v.isReflection )
			{
				dh_raw_dv = Vector3( dwi_dv.x + dwo_dv.x, dwi_dv.y + dwo_dv.y, dwi_dv.z + dwo_dv.z );	// reflection: η_i = η_t (irrelevant)
			}
			else
			{
				// Walter form: ∂h/∂p = -(η_i ∂wi/∂p + η_t ∂wo/∂p)
				dh_raw_dv = Vector3(
					-(eta_i_v * dwi_dv.x + eta_t_v * dwo_dv.x),
					-(eta_i_v * dwi_dv.y + eta_t_v * dwo_dv.y),
					-(eta_i_v * dwi_dv.z + eta_t_v * dwo_dv.z) );
			}
			dh_dv = rawHalfVector ? dh_raw_dv : DeriveNormalized( h, dh_raw_dv, h_len );
		}

		// Derivative of tangent frame w.r.t. surface parameters (u, v).
		//
		// Derivation (from Blender Cycles MNEE, which matches Zeltner 2020):
		// Let s = normalize(dpdu - (dpdu·n) n)  (Gram-Schmidt projection of
		// dpdu into the tangent plane).  Treating dpdu as fixed while only
		// n varies (the constraint function holds dpdu constant), the
		// product rule gives:
		//   ds/du = -1/|s| × [ (dpdu·dndu) n + (dpdu·n) dndu ]
		// Then re-orthogonalize against s so ds/du stays perpendicular to s:
		//   ds/du -= s × (s·ds/du)
		// The corresponding bitangent t = n × s, so:
		//   dt/du = dndu × s + n × ds/du
		//
		// CRITICAL: this formula produces a nonzero ds/du contribution at
		// the converged specular solution (h ≈ n) for curved surfaces, which
		// is essential for the Jacobian determinant to capture surface
		// curvature (the ingredient that creates caustic focusing).
		//
		// The previous formula (dndu - s*(s·dndu))/|dpdu| degenerated to
		// (dndu·t) t/|dpdu| because dndu·n = 0 identically.  Dotted with h=n
		// at convergence, that gave zero, silently eliminating the curvature
		// contribution from the Jacobian — the manifold then behaved as if
		// on a flat surface regardless of actual displacement.
		const Scalar dpdu_dot_n = Vector3Ops::Dot( v.dpdu, v.normal );
		const Vector3 s_unnorm = Vector3(
			v.dpdu.x - dpdu_dot_n * v.normal.x,
			v.dpdu.y - dpdu_dot_n * v.normal.y,
			v.dpdu.z - dpdu_dot_n * v.normal.z );
		const Scalar s_len = Vector3Ops::Magnitude( s_unnorm );
		const Scalar inv_s_len = 1.0 / fmax( s_len, NEARZERO );
		// (s may differ slightly from the previously-computed s because
		// that was Normalize(dpdu) without the tangent-plane projection;
		// here we use the Gram-Schmidt variant to stay consistent with
		// the product-rule derivation below.)
		const Vector3 s_proj = s_unnorm * inv_s_len;
		const Vector3 t_cross = Vector3Ops::Cross( v.normal, s_proj );

		Vector3 ds_du( 0, 0, 0 ), ds_dv( 0, 0, 0 );
		Vector3 dt_du( 0, 0, 0 ), dt_dv( 0, 0, 0 );
		if( includeCurvature ) {
			const Scalar dpdu_dot_dndu = Vector3Ops::Dot( v.dpdu, v.dndu );
			const Scalar dpdu_dot_dndv = Vector3Ops::Dot( v.dpdu, v.dndv );
			ds_du = Vector3(
				-inv_s_len * (dpdu_dot_dndu * v.normal.x + dpdu_dot_n * v.dndu.x),
				-inv_s_len * (dpdu_dot_dndu * v.normal.y + dpdu_dot_n * v.dndu.y),
				-inv_s_len * (dpdu_dot_dndu * v.normal.z + dpdu_dot_n * v.dndu.z) );
			ds_dv = Vector3(
				-inv_s_len * (dpdu_dot_dndv * v.normal.x + dpdu_dot_n * v.dndv.x),
				-inv_s_len * (dpdu_dot_dndv * v.normal.y + dpdu_dot_n * v.dndv.y),
				-inv_s_len * (dpdu_dot_dndv * v.normal.z + dpdu_dot_n * v.dndv.z) );
			// Re-orthogonalize against s so ds_du stays ⊥ s.
			const Scalar ds_du_dot_s = Vector3Ops::Dot( ds_du, s_proj );
			const Scalar ds_dv_dot_s = Vector3Ops::Dot( ds_dv, s_proj );
			ds_du = Vector3(
				ds_du.x - s_proj.x * ds_du_dot_s,
				ds_du.y - s_proj.y * ds_du_dot_s,
				ds_du.z - s_proj.z * ds_du_dot_s );
			ds_dv = Vector3(
				ds_dv.x - s_proj.x * ds_dv_dot_s,
				ds_dv.y - s_proj.y * ds_dv_dot_s,
				ds_dv.z - s_proj.z * ds_dv_dot_s );
			// t = n × s, so dt/d* = dn/d* × s + n × ds/d*.
			dt_du = Vector3Ops::Cross( v.dndu, s_proj )
				+ Vector3Ops::Cross( v.normal, ds_du );
			dt_dv = Vector3Ops::Cross( v.dndv, s_proj )
				+ Vector3Ops::Cross( v.normal, ds_dv );
		}

		// Use the Gram-Schmidt-projected s (consistent with the derivative
		// formula above) for the constraint-projection terms, overriding
		// the earlier s = Normalize(dpdu).  For well-conditioned tangent
		// frames these differ only by a tiny rotation.
		s = s_proj;
		t = t_cross;


		// ---- Diagonal block: dC_i / dX_i ----
		// dC_i_s / du = dot(ds_du, h) + dot(s, dh_du)
		// dC_i_s / dv = dot(ds_dv, h) + dot(s, dh_dv)
		// dC_i_t / du = dot(dt_du, h) + dot(t, dh_du)
		// dC_i_t / dv = dot(dt_dv, h) + dot(t, dh_dv)
		diag[i*4 + 0] = Vector3Ops::Dot( ds_du, h ) + Vector3Ops::Dot( s, dh_du );  // row0, col0
		diag[i*4 + 1] = Vector3Ops::Dot( ds_dv, h ) + Vector3Ops::Dot( s, dh_dv );  // row0, col1
		diag[i*4 + 2] = Vector3Ops::Dot( dt_du, h ) + Vector3Ops::Dot( t, dh_du );  // row1, col0
		diag[i*4 + 3] = Vector3Ops::Dot( dt_dv, h ) + Vector3Ops::Dot( t, dh_dv );  // row1, col1

		// ---- Off-diagonal blocks: dC_i / dX_{i-1}  and  dC_i / dX_{i+1} ----
		//
		// Moving vertex i-1 only affects wi (not wo).
		// Moving vertex i+1 only affects wo (not wi).
		// The tangent frame (s, t) of vertex i does NOT change when
		// neighboring vertices move — only h changes.

		// Upper block: dC_i / dX_{i+1}  (if i < k-1)
		// Moving vertex i+1 by dp changes wo:
		//   dwo/dp_next = +(dp_next - wo * dot(wo, dp_next)) / dist_o
		// Note the +sign: when the next vertex moves by +dp, wo changes positively.
		if( i < k - 1 )
		{
			const ManifoldVertex& vn = chain[i+1];
			for( unsigned int p = 0; p < 2; p++ )
			{
				const Vector3& dp = (p == 0) ? vn.dpdu : vn.dpdv;
				const Vector3 dwo = Vector3(
					(dp.x - wo.x * Vector3Ops::Dot( wo, dp )) * inv_lo,
					(dp.y - wo.y * Vector3Ops::Dot( wo, dp )) * inv_lo,
					(dp.z - wo.z * Vector3Ops::Dot( wo, dp )) * inv_lo );

				Vector3 dh_raw_next;
				if( v.isReflection )
					dh_raw_next = dwo;
				else
					// Walter form: ∂h/∂p_{i+1} = -η_t ∂wo/∂p (only wo
					// depends on next vertex)
					dh_raw_next = Vector3( -eta_t_v * dwo.x, -eta_t_v * dwo.y, -eta_t_v * dwo.z );

				const Vector3 dh_next = rawHalfVector ? dh_raw_next : DeriveNormalized( h, dh_raw_next, h_len );

				// upper[i] maps vertex i+1 to constraint i
				upper[i*4 + 0 + p] = Vector3Ops::Dot( s, dh_next );
				upper[i*4 + 2 + p] = Vector3Ops::Dot( t, dh_next );
			}
		}

		// Lower block: dC_i / dX_{i-1}  (if i > 0)
		// Moving vertex i-1 by dp changes wi:
		//   dwi/dp_prev = +(dp_prev - wi * dot(wi, dp_prev)) / dist_i
		if( i > 0 )
		{
			const ManifoldVertex& vp = chain[i-1];
			for( unsigned int p = 0; p < 2; p++ )
			{
				const Vector3& dp = (p == 0) ? vp.dpdu : vp.dpdv;
				const Vector3 dwi = Vector3(
					(dp.x - wi.x * Vector3Ops::Dot( wi, dp )) * inv_li,
					(dp.y - wi.y * Vector3Ops::Dot( wi, dp )) * inv_li,
					(dp.z - wi.z * Vector3Ops::Dot( wi, dp )) * inv_li );

				Vector3 dh_raw_prev;
				if( v.isReflection )
					dh_raw_prev = dwi;
				else
					// Walter form: ∂h/∂p_{i-1} = -η_i ∂wi/∂p (only wi
					// depends on previous vertex).  The OLD code had
					// no eta factor here — equivalent to assuming
					// η_i = 1 (the single-dielectric-in-air case
					// where the Walter form divided by η_i puts
					// 1·wi on the wi side).  For nested dielectrics
					// where the previous-side medium IS the object
					// (e.g. crossing into the air-cavity from
					// glass), η_i = 1.5 here and matters.
					dh_raw_prev = Vector3( -eta_i_v * dwi.x, -eta_i_v * dwi.y, -eta_i_v * dwi.z );

				const Vector3 dh_prev = rawHalfVector ? dh_raw_prev : DeriveNormalized( h, dh_raw_prev, h_len );

				// lower[i-1] maps vertex i-1 to constraint i
				lower[(i-1)*4 + 0 + p] = Vector3Ops::Dot( s, dh_prev );
				lower[(i-1)*4 + 2 + p] = Vector3Ops::Dot( t, dh_prev );
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////
// SolveBlockTridiagonal
//
//   Solves J * delta = rhs where J is block-tridiagonal.
//   Each 2x2 block is stored as 4 scalars [a,b,c,d] in row-major.
//   diag is modified in place.
//////////////////////////////////////////////////////////////////////

bool ManifoldSolver::Invert2x2( const Scalar* m, Scalar* inv )
{
	const Scalar det = m[0] * m[3] - m[1] * m[2];
	if( fabs(det) < NEARZERO )
	{
		return false;
	}
	const Scalar inv_det = 1.0 / det;
	inv[0] =  m[3] * inv_det;
	inv[1] = -m[1] * inv_det;
	inv[2] = -m[2] * inv_det;
	inv[3] =  m[0] * inv_det;
	return true;
}

void ManifoldSolver::Mul2x2( const Scalar* A, const Scalar* B, Scalar* C )
{
	C[0] = A[0]*B[0] + A[1]*B[2];
	C[1] = A[0]*B[1] + A[1]*B[3];
	C[2] = A[2]*B[0] + A[3]*B[2];
	C[3] = A[2]*B[1] + A[3]*B[3];
}

void ManifoldSolver::Mul2x2Vec( const Scalar* A, const Scalar* v, Scalar* r )
{
	r[0] = A[0]*v[0] + A[1]*v[1];
	r[1] = A[2]*v[0] + A[3]*v[1];
}

void ManifoldSolver::Sub2x2( const Scalar* A, const Scalar* B, Scalar* C )
{
	C[0] = A[0] - B[0];
	C[1] = A[1] - B[1];
	C[2] = A[2] - B[2];
	C[3] = A[3] - B[3];
}

//////////////////////////////////////////////////////////////////////
// ComputeDielectricFresnel
//
//   Exact dielectric Fresnel reflectance (unpolarized average of
//   s- and p-polarized components).  Returns 1.0 for TIR.
//////////////////////////////////////////////////////////////////////

Scalar ManifoldSolver::ComputeDielectricFresnel(
	Scalar cosI,
	Scalar eta_i,
	Scalar eta_t
	)
{
	return Optics::CalculateDielectricReflectanceCosine( cosI, eta_i, eta_t );
}

//////////////////////////////////////////////////////////////////////
// ComputeSphericalDerivatives
//
//   World-space gradients of theta and phi from DirectionToSpherical.
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::ComputeSphericalDerivatives(
	const Vector3& dir,
	const Vector3& s,
	const Vector3& t,
	const Vector3& normal,
	Vector3& dTheta_dDir,
	Vector3& dPhi_dDir
	)
{
	const Scalar x = Vector3Ops::Dot( dir, s );
	const Scalar y = Vector3Ops::Dot( dir, t );
	const Scalar z = Vector3Ops::Dot( dir, normal );

	// theta = acos(z), d(theta)/d(z) = -1/sin(theta)
	const Scalar sinTheta = sqrt( fmax( 1.0 - z * z, 0.0 ) );
	if( sinTheta > NEARZERO )
	{
		const Scalar invSin = -1.0 / sinTheta;
		dTheta_dDir = Vector3( invSin * normal.x, invSin * normal.y, invSin * normal.z );
	}
	else
	{
		// dir ≈ ±normal, theta gradient is degenerate
		dTheta_dDir = Vector3( 0, 0, 0 );
	}

	// phi = atan2(y, x), d(phi)/d(x) = -y/r², d(phi)/d(y) = x/r²
	const Scalar r2 = x * x + y * y;
	if( r2 > NEARZERO )
	{
		const Scalar invR2 = 1.0 / r2;
		// dPhi/d(dir) = (-y/r²) * s + (x/r²) * t
		dPhi_dDir = Vector3(
			(-y * invR2) * s.x + (x * invR2) * t.x,
			(-y * invR2) * s.y + (x * invR2) * t.y,
			(-y * invR2) * s.z + (x * invR2) * t.z );
	}
	else
	{
		// dir ≈ ±normal, phi gradient is degenerate
		dPhi_dDir = Vector3( 0, 0, 0 );
	}
}

//////////////////////////////////////////////////////////////////////
// ComputeSpecularDirectionDerivativeWrtWi
//
//   3x3 Jacobian d(wo)/d(wi) for reflection/refraction.
//   Assumes unnormalized wo (before normalization in
//   ComputeSpecularDirection).
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::ComputeSpecularDirectionDerivativeWrtWi(
	const Vector3& wi,
	const Vector3& normal,
	Scalar eta,
	bool isReflection,
	Scalar dwo_dwi[9]
	)
{
	if( isReflection )
	{
		// wo = -wi + 2*(wi·n)*n
		// d(wo)/d(wi) = -I + 2*outer(n, n)
		dwo_dwi[0] = -1.0 + 2.0 * normal.x * normal.x;
		dwo_dwi[1] =        2.0 * normal.x * normal.y;
		dwo_dwi[2] =        2.0 * normal.x * normal.z;
		dwo_dwi[3] =        2.0 * normal.y * normal.x;
		dwo_dwi[4] = -1.0 + 2.0 * normal.y * normal.y;
		dwo_dwi[5] =        2.0 * normal.y * normal.z;
		dwo_dwi[6] =        2.0 * normal.z * normal.x;
		dwo_dwi[7] =        2.0 * normal.z * normal.y;
		dwo_dwi[8] = -1.0 + 2.0 * normal.z * normal.z;
	}
	else
	{
		// Matched media give wo=-wi without a critical-angle singularity.
		if( eta == 1.0 ) {
			for( unsigned int i=0; i<9; ++i ) dwo_dwi[i] = i%4 == 0 ? -1.0 : 0.0;
			return;
		}
		// wo = -eta_ratio*wi + (eta_ratio*cos_i - cos_t)*n
		// For entering (cos_i > 0): eta_ratio = 1/eta
		// For exiting (cos_i < 0): eta_ratio = eta
		const Scalar cos_i = Vector3Ops::Dot( wi, normal );

		Vector3 n = normal;
		Scalar eta_ratio = 1.0 / eta;
		Scalar ci = cos_i;

		if( ci < 0.0 )
		{
			n = Vector3( -normal.x, -normal.y, -normal.z );
			ci = -ci;
			eta_ratio = eta;
		}

		const Scalar sin2_t = eta_ratio * eta_ratio * (1.0 - ci * ci);
		Scalar cos_t = 0.0;
		if( sin2_t < 1.0 )
			cos_t = sqrt( 1.0 - sin2_t );
		if( cos_t < NEARZERO )
			cos_t = NEARZERO;

		// d(wo)/d(wi) = -eta_ratio*I + eta_ratio*(1 - eta_ratio*ci/cos_t)*outer(n, n)
		const Scalar factor = eta_ratio * (1.0 - eta_ratio * ci / cos_t);

		dwo_dwi[0] = -eta_ratio + factor * n.x * n.x;
		dwo_dwi[1] =              factor * n.x * n.y;
		dwo_dwi[2] =              factor * n.x * n.z;
		dwo_dwi[3] =              factor * n.y * n.x;
		dwo_dwi[4] = -eta_ratio + factor * n.y * n.y;
		dwo_dwi[5] =              factor * n.y * n.z;
		dwo_dwi[6] =              factor * n.z * n.x;
		dwo_dwi[7] =              factor * n.z * n.y;
		dwo_dwi[8] = -eta_ratio + factor * n.z * n.z;
	}
}

//////////////////////////////////////////////////////////////////////
// BuildJacobianAngleDiffNumerical
//
//   Numerical Jacobian for the angle-difference constraint.
//   Perturbs vertices and evaluates EvaluateConstraintAtVertex.
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::BuildJacobianAngleDiffNumerical(
	const std::vector<ManifoldVertex>& chain,
	const Point3& fixedStart,
	const Point3& fixedEnd,
	std::vector<Scalar>& diag,
	std::vector<Scalar>& upper,
	std::vector<Scalar>& lower
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );
	const Scalar eps = 1e-5;
	const Scalar inv2eps = 1.0 / (2.0 * eps);

	diag.resize( k * 4, 0.0 );
	if( k > 1 )
	{
		upper.resize( (k-1) * 4, 0.0 );
		lower.resize( (k-1) * 4, 0.0 );
	}
	else
	{
		upper.clear();
		lower.clear();
	}

	// Helper lambda: evaluate angle-diff constraint at vertex ci
	// given modified chain positions
	auto evalAtVertex = [&]( unsigned int ci,
		const Point3& prevP, const Point3& nextP,
		const Point3& vPos, const Vector3& vNorm,
		Scalar& C0, Scalar& C1 )
	{
		const ManifoldVertex& v = chain[ci];
		EvaluateConstraintAtVertex(
			vPos, vNorm, v.dpdu, v.dpdv,
			v.eta, v.isReflection,
			prevP, nextP, C0, C1 );
	};

	for( unsigned int j = 0; j < k; j++ )
	{
		for( unsigned int p = 0; p < 2; p++ )
		{
			const Vector3& dp = (p == 0) ? chain[j].dpdu : chain[j].dpdv;
			const Vector3& dn = (p == 0) ? chain[j].dndu : chain[j].dndv;

			const Point3 posPlus = Point3Ops::mkPoint3( chain[j].position, dp * eps );
			const Point3 posMinus = Point3Ops::mkPoint3( chain[j].position, dp * (-eps) );
			Vector3 normPlus = Vector3Ops::Normalize( Vector3(
				chain[j].normal.x + dn.x * eps,
				chain[j].normal.y + dn.y * eps,
				chain[j].normal.z + dn.z * eps ) );
			Vector3 normMinus = Vector3Ops::Normalize( Vector3(
				chain[j].normal.x - dn.x * eps,
				chain[j].normal.y - dn.y * eps,
				chain[j].normal.z - dn.z * eps ) );

			// Diagonal: constraint j w.r.t. vertex j
			{
				const Point3 prevP = (j == 0) ? fixedStart : chain[j-1].position;
				const Point3 nextP = (j == k-1) ? fixedEnd : chain[j+1].position;

				Scalar Cp0, Cp1, Cm0, Cm1;
				evalAtVertex( j, prevP, nextP, posPlus, normPlus, Cp0, Cp1 );
				evalAtVertex( j, prevP, nextP, posMinus, normMinus, Cm0, Cm1 );

				Scalar dC1 = Cp1 - Cm1;
				if( dC1 > PI ) dC1 -= 2.0 * PI;
				if( dC1 < -PI ) dC1 += 2.0 * PI;

				diag[j*4 + 0 + p] = (Cp0 - Cm0) * inv2eps;
				diag[j*4 + 2 + p] = dC1 * inv2eps;
			}

			// Upper: constraint j-1 w.r.t. vertex j (if j > 0)
			if( j > 0 )
			{
				unsigned int ci = j - 1;
				const Point3 prevP = (ci == 0) ? fixedStart : chain[ci-1].position;

				Scalar Cp0, Cp1, Cm0, Cm1;
				evalAtVertex( ci, prevP, posPlus, chain[ci].position, chain[ci].normal, Cp0, Cp1 );
				evalAtVertex( ci, prevP, posMinus, chain[ci].position, chain[ci].normal, Cm0, Cm1 );

				Scalar dC1 = Cp1 - Cm1;
				if( dC1 > PI ) dC1 -= 2.0 * PI;
				if( dC1 < -PI ) dC1 += 2.0 * PI;

				upper[ci*4 + 0 + p] = (Cp0 - Cm0) * inv2eps;
				upper[ci*4 + 2 + p] = dC1 * inv2eps;
			}

			// Lower: constraint j+1 w.r.t. vertex j (if j < k-1)
			if( j < k - 1 )
			{
				unsigned int ci = j + 1;
				const Point3 nextP = (ci == k-1) ? fixedEnd : chain[ci+1].position;

				Scalar Cp0, Cp1, Cm0, Cm1;
				evalAtVertex( ci, posPlus, nextP, chain[ci].position, chain[ci].normal, Cp0, Cp1 );
				evalAtVertex( ci, posMinus, nextP, chain[ci].position, chain[ci].normal, Cm0, Cm1 );

				Scalar dC1 = Cp1 - Cm1;
				if( dC1 > PI ) dC1 -= 2.0 * PI;
				if( dC1 < -PI ) dC1 += 2.0 * PI;

				lower[j*4 + 0 + p] = (Cp0 - Cm0) * inv2eps;
				lower[j*4 + 2 + p] = dC1 * inv2eps;
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////
// BuildJacobianAngleDiff
//
//   Analytical Jacobian for the angle-difference constraint.
//   Chain rule through spherical coordinates, specular direction,
//   and surface curvature (Weingarten map).
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::BuildJacobianAngleDiff(
	const std::vector<ManifoldVertex>& chain,
	const Point3& fixedStart,
	const Point3& fixedEnd,
	std::vector<Scalar>& diag,
	std::vector<Scalar>& upper,
	std::vector<Scalar>& lower
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );

	diag.resize( k * 4, 0.0 );
	if( k > 1 )
	{
		upper.resize( (k-1) * 4, 0.0 );
		lower.resize( (k-1) * 4, 0.0 );
	}
	else
	{
		upper.clear();
		lower.clear();
	}

	for( unsigned int i = 0; i < k; i++ )
	{
		const ManifoldVertex& v = chain[i];

		const Point3 prevPos = (i == 0) ? fixedStart : chain[i-1].position;
		const Point3 nextPos = (i == k-1) ? fixedEnd : chain[i+1].position;

		// Directions and distances
		Vector3 d_wi = Vector3Ops::mkVector3( prevPos, v.position );
		const Scalar dist_i = Vector3Ops::NormalizeMag( d_wi );
		const Vector3 wi = d_wi;

		Vector3 d_wo = Vector3Ops::mkVector3( nextPos, v.position );
		const Scalar dist_o = Vector3Ops::NormalizeMag( d_wo );
		const Vector3 wo = d_wo;

		if( dist_i < NEARZERO || dist_o < NEARZERO ) continue;

		const Scalar inv_li = 1.0 / dist_i;
		const Scalar inv_lo = 1.0 / dist_o;

		// Tangent frame (normalized)
		const Vector3 s = Vector3Ops::Normalize( v.dpdu );
		const Vector3 t = Vector3Ops::Normalize( v.dpdv );

		// Specularly scattered direction
		Vector3 wo_spec;
		if( !ComputeSpecularDirection( wi, v.normal, v.eta, v.isReflection, wo_spec ) )
		{
			// TIR — degenerate, leave zero entries
			continue;
		}

		// Spherical derivatives for wo_actual and wo_specular
		Vector3 dThetaA_dDir, dPhiA_dDir;
		ComputeSphericalDerivatives( wo, s, t, v.normal, dThetaA_dDir, dPhiA_dDir );

		Vector3 dThetaS_dDir, dPhiS_dDir;
		ComputeSphericalDerivatives( wo_spec, s, t, v.normal, dThetaS_dDir, dPhiS_dDir );

		// d(wo_spec)/d(wi) and d(wo_spec)/d(n) — 3x3 Jacobians
		Scalar dwoSpec_dwi[9], dwoSpec_dn[9];
		ComputeSpecularDirectionDerivativeWrtWi( wi, v.normal, v.eta, v.isReflection, dwoSpec_dwi );
		ComputeSpecularDirectionDerivativeWrtNormal( wi, v.normal, v.eta, v.isReflection, dwoSpec_dn );

		// ---- Diagonal block: dC_i / dX_i ----
		for( unsigned int p = 0; p < 2; p++ )
		{
			const Vector3& dp = (p == 0) ? v.dpdu : v.dpdv;
			const Vector3& dn = (p == 0) ? v.dndu : v.dndv;

			// d(wo_actual)/du: moving vertex changes direction to next vertex
			const Vector3 dwo_du = Vector3(
				-(dp.x - wo.x * Vector3Ops::Dot( wo, dp )) * inv_lo,
				-(dp.y - wo.y * Vector3Ops::Dot( wo, dp )) * inv_lo,
				-(dp.z - wo.z * Vector3Ops::Dot( wo, dp )) * inv_lo );

			// d(wi)/du: moving vertex changes direction to previous vertex
			const Vector3 dwi_du = Vector3(
				-(dp.x - wi.x * Vector3Ops::Dot( wi, dp )) * inv_li,
				-(dp.y - wi.y * Vector3Ops::Dot( wi, dp )) * inv_li,
				-(dp.z - wi.z * Vector3Ops::Dot( wi, dp )) * inv_li );

			// d(wo_spec)/du = dwoSpec_dwi × dwi_du + dwoSpec_dn × dn
			Vector3 dwoSpec_du;
			dwoSpec_du.x = dwoSpec_dwi[0]*dwi_du.x + dwoSpec_dwi[1]*dwi_du.y + dwoSpec_dwi[2]*dwi_du.z
				         + dwoSpec_dn[0]*dn.x + dwoSpec_dn[1]*dn.y + dwoSpec_dn[2]*dn.z;
			dwoSpec_du.y = dwoSpec_dwi[3]*dwi_du.x + dwoSpec_dwi[4]*dwi_du.y + dwoSpec_dwi[5]*dwi_du.z
				         + dwoSpec_dn[3]*dn.x + dwoSpec_dn[4]*dn.y + dwoSpec_dn[5]*dn.z;
			dwoSpec_du.z = dwoSpec_dwi[6]*dwi_du.x + dwoSpec_dwi[7]*dwi_du.y + dwoSpec_dwi[8]*dwi_du.z
				         + dwoSpec_dn[6]*dn.x + dwoSpec_dn[7]*dn.y + dwoSpec_dn[8]*dn.z;

			// dC0/du = dTheta_actual/d(dir) · dwo_du - dTheta_spec/d(dir) · dwoSpec_du
			Scalar dC0_dp = Vector3Ops::Dot( dThetaA_dDir, dwo_du )
				          - Vector3Ops::Dot( dThetaS_dDir, dwoSpec_du );

			// dC1/du = dPhi_actual/d(dir) · dwo_du - dPhi_spec/d(dir) · dwoSpec_du
			Scalar dC1_dp = Vector3Ops::Dot( dPhiA_dDir, dwo_du )
				          - Vector3Ops::Dot( dPhiS_dDir, dwoSpec_du );

			// Frame rotation contribution: when the vertex moves, the
			// tangent frame (s, t, n) rotates, changing how the same
			// world-space direction maps to spherical coordinates.
			// For theta = acos(dir·n): d(theta)/d(n_pert) = -(1/sin(theta)) * dir
			// For phi: d(phi)/d(s_pert) and d(phi)/d(t_pert) via chain rule
			// These enter as additional terms for both wo_actual and wo_spec.
			//
			// theta_actual frame term: d(acos(wo·n))/d(n) × dn/du
			const Scalar sinThetaA = sqrt( fmax( 1.0 - Vector3Ops::Dot(wo, v.normal) * Vector3Ops::Dot(wo, v.normal), 0.0 ) );
			if( sinThetaA > NEARZERO )
			{
				const Scalar dthetaA_dn = -Vector3Ops::Dot( wo, dn ) / sinThetaA;
				const Scalar sinThetaS = sqrt( fmax( 1.0 - Vector3Ops::Dot(wo_spec, v.normal) * Vector3Ops::Dot(wo_spec, v.normal), 0.0 ) );
				const Scalar dthetaS_dn = (sinThetaS > NEARZERO) ?
					-Vector3Ops::Dot( wo_spec, dn ) / sinThetaS : 0.0;
				dC0_dp += dthetaA_dn - dthetaS_dn;
			}

			// phi frame terms (tangent rotation) — smaller effect, skip for now
			// to avoid excessive complexity.  The numerical Jacobian will
			// validate whether this omission matters.

			diag[i*4 + 0 + p] = dC0_dp;
			diag[i*4 + 2 + p] = dC1_dp;
		}

		// ---- Upper block: dC_i / dX_{i+1} ----
		if( i < k - 1 )
		{
			const ManifoldVertex& vn = chain[i+1];
			for( unsigned int p = 0; p < 2; p++ )
			{
				const Vector3& dp = (p == 0) ? vn.dpdu : vn.dpdv;

				// Moving next vertex only affects wo_actual
				const Vector3 dwo_next = Vector3(
					(dp.x - wo.x * Vector3Ops::Dot( wo, dp )) * inv_lo,
					(dp.y - wo.y * Vector3Ops::Dot( wo, dp )) * inv_lo,
					(dp.z - wo.z * Vector3Ops::Dot( wo, dp )) * inv_lo );

				upper[i*4 + 0 + p] = Vector3Ops::Dot( dThetaA_dDir, dwo_next );
				upper[i*4 + 2 + p] = Vector3Ops::Dot( dPhiA_dDir, dwo_next );
			}
		}

		// ---- Lower block: dC_i / dX_{i-1} ----
		if( i > 0 )
		{
			const ManifoldVertex& vp = chain[i-1];
			for( unsigned int p = 0; p < 2; p++ )
			{
				const Vector3& dp = (p == 0) ? vp.dpdu : vp.dpdv;

				// Moving previous vertex only affects wi → wo_specular
				const Vector3 dwi_prev = Vector3(
					(dp.x - wi.x * Vector3Ops::Dot( wi, dp )) * inv_li,
					(dp.y - wi.y * Vector3Ops::Dot( wi, dp )) * inv_li,
					(dp.z - wi.z * Vector3Ops::Dot( wi, dp )) * inv_li );

				// d(wo_spec)/d(wi_prev) = dwoSpec_dwi × dwi_prev
				Vector3 dwoSpec_prev;
				dwoSpec_prev.x = dwoSpec_dwi[0]*dwi_prev.x + dwoSpec_dwi[1]*dwi_prev.y + dwoSpec_dwi[2]*dwi_prev.z;
				dwoSpec_prev.y = dwoSpec_dwi[3]*dwi_prev.x + dwoSpec_dwi[4]*dwi_prev.y + dwoSpec_dwi[5]*dwi_prev.z;
				dwoSpec_prev.z = dwoSpec_dwi[6]*dwi_prev.x + dwoSpec_dwi[7]*dwi_prev.y + dwoSpec_dwi[8]*dwi_prev.z;

				lower[(i-1)*4 + 0 + p] = -Vector3Ops::Dot( dThetaS_dDir, dwoSpec_prev );
				lower[(i-1)*4 + 2 + p] = -Vector3Ops::Dot( dPhiS_dDir, dwoSpec_prev );
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////
// ValidateChainPhysics
//
//   Checks that converged specular vertices have physically
//   consistent geometry: refraction requires wi and wo on opposite
//   sides of the surface; reflection requires both on the same side.
//////////////////////////////////////////////////////////////////////

bool ManifoldSolver::ValidateChainPhysics(
	const std::vector<ManifoldVertex>& chain,
	const Point3& fixedStart,
	const Point3& fixedEnd
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );

	for( unsigned int i = 0; i < k; i++ )
	{
		const ManifoldVertex& v = chain[i];
		const Point3 prevPos = (i == 0) ? fixedStart : chain[i-1].position;
		const Point3 nextPos = (i == k-1) ? fixedEnd : chain[i+1].position;

		Vector3 wi = Vector3Ops::mkVector3( prevPos, v.position );
		wi = Vector3Ops::Normalize( wi );
		Vector3 wo = Vector3Ops::mkVector3( nextPos, v.position );
		wo = Vector3Ops::Normalize( wo );

		// Test against the GEOMETRIC normal (the actual surface plane),
		// not the Phong-interpolated shading normal.  On smooth analytic
		// primitives the two are identical, so this is a no-op there;
		// on triangle meshes (especially heavily displaced ones) the
		// Phong-tilted shading normal can disagree with the actual
		// triangle face for chains where wi/wo are nearly parallel to
		// the surface — leading to spurious physics-rejections of
		// chains that are perfectly valid against the real geometry.
		// Matches Zeltner 2020's reference SMS implementation, which
		// uses `vtx.gn` (geometric normal) at the equivalent test site.
		// Falls back to the shading normal if `geomNormal` was not
		// populated (e.g. photon-derived chains, hand-constructed
		// vertices) — degenerate-zero detection avoids a sign-product
		// of 0 that would silently disable the test.
		const bool hasGeom = ( Vector3Ops::SquaredModulus( v.geomNormal ) > NEARZERO );
		const Vector3& nForTest = hasGeom ? v.geomNormal : v.normal;
		const Scalar wiDotN = Vector3Ops::Dot( wi, nForTest );
		const Scalar woDotN = Vector3Ops::Dot( wo, nForTest );

		bool rejected = false;
		if( v.isReflection )
		{
			// Reflection: both directions on the same side of surface
			if( wiDotN * woDotN < 0.0 ) rejected = true;
		}
		else
		{
			// Refraction: directions on opposite sides of surface
			if( wiDotN * woDotN > 0.0 ) rejected = true;
		}

		if( rejected )
		{
#if SMS_SOLVE_DIAG
			// Anatomy: characterise this rejection.
			PhysFail_BinPos( i, k );
			g_physFail_byOp[ v.isReflection ? 1 : 0 ].fetch_add( 1, std::memory_order_relaxed );

			// PUSHBACK: sign-product breakdown.  For a refraction we
			// reject when (wi·n)(wo·n) > 0; that's either both > 0
			// (reflection-from-above) or both < 0 (reflection-from-
			// below).  For a reflection we reject when < 0 (one above,
			// one below — geometry is refraction).  Bin by sign pair
			// for refraction failures so we can tell which kind of
			// wrong-topology Newton is finding.
			if( !v.isReflection ) {
				if( wiDotN > 0.0 && woDotN > 0.0 )
					g_physFail_signProd_bothPos.fetch_add( 1, std::memory_order_relaxed );
				else if( wiDotN < 0.0 && woDotN < 0.0 )
					g_physFail_signProd_bothNeg.fetch_add( 1, std::memory_order_relaxed );
			}

			const double absWi = wiDotN < 0 ? -wiDotN : wiDotN;
			const double absWo = woDotN < 0 ? -woDotN : woDotN;
			PhysFail_BinMinCos( absWi < absWo ? absWi : absWo );

			if( hasGeom && Vector3Ops::SquaredModulus( v.normal ) > NEARZERO )
			{
				const Scalar dotGS = Vector3Ops::Dot(
					Vector3Ops::Normalize( v.geomNormal ),
					Vector3Ops::Normalize( v.normal ) );
				PhysFail_BinPhongTilt( dotGS );

				// Compare what the SHADING-normal test would have done.
				const Scalar wiDotS = Vector3Ops::Dot( wi, v.normal );
				const Scalar woDotS = Vector3Ops::Dot( wo, v.normal );
				const bool shadingRejects = v.isReflection
					? ( wiDotS * woDotS < 0.0 )
					: ( wiDotS * woDotS > 0.0 );
				if( shadingRejects ) g_physFail_shadingAgrees.fetch_add( 1, std::memory_order_relaxed );
				else                 g_physFail_shadingMasked.fetch_add( 1, std::memory_order_relaxed );
			}
			else
			{
				g_physFail_geomFallback.fetch_add( 1, std::memory_order_relaxed );
			}

			// Cascade anatomy.
			PhysFail_BinFirstFailIdx( i );
			if( i > 0 )
			{
				// Compare vertex 0's geom normal to the failing vertex's
				// geom normal.  Skip if either is unset (photon-derived
				// chains).  The dot product captures both how much the
				// chain has rotated around the surface and whether it has
				// crossed to a back-facing patch.
				const ManifoldVertex& v0 = chain[0];
				if( hasGeom && Vector3Ops::SquaredModulus( v0.geomNormal ) > NEARZERO )
				{
					const Scalar dot0F = Vector3Ops::Dot(
						Vector3Ops::Normalize( v0.geomNormal ),
						Vector3Ops::Normalize( v.geomNormal ) );
					PhysFail_BinV0FailAngle( dot0F );
				}
			}
#endif
			return false;
		}
	}

	return true;
}

//////////////////////////////////////////////////////////////////////
// ComputeBlockTridiagonalDeterminant
//
//   Computes det(J) of a block-tridiagonal matrix via LU forward
//   elimination.  det = product of det(Dp[i]) for each 2x2 block.
//////////////////////////////////////////////////////////////////////

Scalar ManifoldSolver::ComputeBlockTridiagonalDeterminant(
	const std::vector<Scalar>& diag,
	const std::vector<Scalar>& upper,
	const std::vector<Scalar>& lower,
	unsigned int k
	) const
{
	if( k == 0 ) return 1.0;

    SMSWorkerScratchLease scratch(smsScratchDepth!=0,smsActiveDiagnostics);
    const auto capacity=std::max<unsigned>(config.maxChainDepth,k);
    std::vector<Scalar> localDp;
    auto& Dp=scratch.Enabled()?scratch.Scalars(0,4*capacity):localDp;Dp.resize(4*k);
	Scalar detProduct = 1.0;

	for( unsigned int i = 0; i < k; i++ )
	{
		if( i == 0 )
		{
			for( int q = 0; q < 4; q++ )
				Dp[q] = diag[q];
		}
		else
		{
			Scalar invDp[4];
			if( !Invert2x2( &Dp[(i-1)*4], invDp ) )
			{
				return 0.0;
			}

			Scalar LiInvDp[4];
			Mul2x2( &lower[(i-1)*4], invDp, LiInvDp );

			Scalar LiInvDpUi[4];
			Mul2x2( LiInvDp, &upper[(i-1)*4], LiInvDpUi );

			Sub2x2( &diag[i*4], LiInvDpUi, &Dp[i*4] );
		}

		const Scalar blkDet = Dp[i*4+0] * Dp[i*4+3] - Dp[i*4+1] * Dp[i*4+2];
		detProduct *= blkDet;
	}

	return detProduct;
}

bool ManifoldSolver::SolveBlockTridiagonal(
	std::vector<Scalar>& diag,
	const std::vector<Scalar>& upper,
	const std::vector<Scalar>& lower,
	const std::vector<Scalar>& rhs,
	unsigned int k,
	std::vector<Scalar>& delta
	) const
{
	if( k == 0 )
	{
		return false;
	}

	delta.resize( 2 * k, 0.0 );

	// Modified diagonal blocks and modified rhs
	// We'll work with arrays of 2x2 blocks (4 scalars each) and 2-vectors
    SMSWorkerScratchLease scratch(smsScratchDepth!=0,smsActiveDiagnostics);
    const auto capacity=std::max<unsigned>(config.maxChainDepth,k);
    std::vector<Scalar> localDp;
    auto& Dp=scratch.Enabled()?scratch.Scalars(0,4*capacity):localDp;Dp.resize(4*k);
    std::vector<Scalar> localRp;
    auto& rp=scratch.Enabled()?scratch.Scalars(1,2*capacity):localRp;rp.resize(2*k);

	// Forward sweep
	for( unsigned int i = 0; i < k; i++ )
	{
		if( i == 0 )
		{
			// Dp[0] = D[0]
			for( int q = 0; q < 4; q++ ) {
				Dp[q] = diag[q];
			}
			rp[0] = rhs[0];
			rp[1] = rhs[1];
		}
		else
		{
			// Dp[i] = D[i] - L[i] * inv(Dp[i-1]) * U[i-1]
			Scalar invDp[4];
			if( !Invert2x2( &Dp[(i-1)*4], invDp ) )
			{
				return false;
			}

			Scalar LiInvDp[4];
			Mul2x2( &lower[(i-1)*4], invDp, LiInvDp );

			Scalar LiInvDpUi[4];
			Mul2x2( LiInvDp, &upper[(i-1)*4], LiInvDpUi );

			Sub2x2( &diag[i*4], LiInvDpUi, &Dp[i*4] );

			// rp[i] = rhs[i] - L[i] * inv(Dp[i-1]) * rp[i-1]
			Scalar LiInvDpRhs[2];
			Mul2x2Vec( LiInvDp, &rp[(i-1)*2], LiInvDpRhs );

			rp[i*2]   = rhs[i*2]   - LiInvDpRhs[0];
			rp[i*2+1] = rhs[i*2+1] - LiInvDpRhs[1];
		}
	}

	// Back substitution
	for( int i = static_cast<int>(k) - 1; i >= 0; i-- )
	{
		Scalar rhs_i[2];
		rhs_i[0] = rp[i*2];
		rhs_i[1] = rp[i*2+1];

		if( i < static_cast<int>(k) - 1 )
		{
			// rhs_i -= U[i] * delta[i+1]
			Scalar Ud[2];
			Mul2x2Vec( &upper[i*4], &delta[(i+1)*2], Ud );
			rhs_i[0] -= Ud[0];
			rhs_i[1] -= Ud[1];
		}

		// delta[i] = inv(Dp[i]) * rhs_i
		Scalar invDp[4];
		if( !Invert2x2( &Dp[i*4], invDp ) )
		{
			return false;
		}
		Mul2x2Vec( invDp, rhs_i, &delta[i*2] );
	}

	return true;
}

//////////////////////////////////////////////////////////////////////
// UpdateVertexOnSurface
//
//   Steps a vertex along the surface parameterization by (du, dv)
//   and re-snaps to the surface via ray intersection.
//////////////////////////////////////////////////////////////////////

bool ManifoldSolver::UpdateVertexOnSurface(
	ManifoldVertex& vertex,
	Scalar du,
	Scalar dv,
	Scalar smoothing, bool referenceRefinement
	) const
{
	if( !vertex.pObject )
	{
		return false;
	}

	// SMS two-stage Stage 1 path: take the step in (u, v) space directly
	// and re-evaluate the smoothing-aware analytical surface.  Bypasses
	// the mesh ray-cast snap below — at smoothing > 0 the chain lives on
	// a smoothed surface that the underlying mesh doesn't approximate, so
	// the snap would put us back on the actual surface and undo Stage 1's
	// reason for existing.  See docs/SMS_TWO_STAGE_SOLVER.md.
	if( smoothing > 0.0 )
	{
        vertex.alphaEndpoint.reset();
		Point2 newUv( vertex.uv.x + du, vertex.uv.y + dv );
		// Spherical-style pole wrap.  For sphere/ellipsoid parameter-
		// isations, crossing a pole (v < 0 or v > 1) reflects the v
		// coordinate and shifts u by half a turn — same surface point
		// reached by going the other way around.  Without this, a
		// clamp-to-[0,1] would put the vertex AT the pole, where
		// dpdu = sin(phi)·(...) collapses to zero and the Jacobian
		// becomes singular.  Empirically (SMS_NEWTON_STATS' singular
		// counter) clamping caused ≥17% of Newton calls to degenerate
		// on the displaced-egg scene's two-stage Stage 1.
		if( newUv.y < 0.0 ) {
			newUv.y = -newUv.y;
			newUv.x += 0.5;
		} else if( newUv.y > 1.0 ) {
			newUv.y = 2.0 - newUv.y;
			newUv.x += 0.5;
		}
		while( newUv.x < 0.0 ) newUv.x += 1.0;
		while( newUv.x >= 1.0 ) newUv.x -= 1.0;
		Point3  aP;
		Vector3 aN, aDpdu, aDpdv, aDndu, aDndv;
		if( vertex.pObject->ComputeAnalyticalDerivatives(
				newUv, smoothing, aP, aN, aDpdu, aDpdv, aDndu, aDndv ) )
		{
			vertex.uv       = newUv;
			vertex.position = aP;
			vertex.normal   = aN;
			vertex.geomNormal = aN;	// analytical primitive: shading == geometric
			vertex.dpdu     = aDpdu;
			vertex.dpdv     = aDpdv;
			vertex.dndu     = aDndu;
			vertex.dndv     = aDndv;
			OrthonormalizeTangentFrame( vertex );
			vertex.valid = true;
			return true;
		}
		// Analytical query failed — caller (NewtonSolve under two-stage
		// Stage 1) treats as a rejected step and halves β.
		return false;
	}

	// Linear approximation of new position using tangent derivatives
	const Point3 newPos = Point3Ops::mkPoint3(
		vertex.position,
		vertex.dpdu * du + vertex.dpdv * dv
		);

    if(nativeEventConstraints && SMSNeedsNativeFrame(vertex) && smoothing==0) {
        ManifoldVertex projected=vertex;projected.position=newPos;
        const Scalar distance=Point3Ops::Distance(newPos,vertex.position);
        if(!ComputeNativeVertexFrame(projected,distance)) return false;
        vertex=std::move(projected);return true;
    }

	// Also update normal using normal derivatives (first-order)
	Vector3 newNormal = Vector3(
		vertex.normal.x + vertex.dndu.x * du + vertex.dndv.x * dv,
		vertex.normal.y + vertex.dndu.y * du + vertex.dndv.y * dv,
		vertex.normal.z + vertex.dndu.z * du + vertex.dndv.z * dv
		);
	newNormal = Vector3Ops::Normalize( newNormal );

	// For small steps, use linear approximation for position/normal
	// but always re-snap to the actual surface via intersection so
	// we get accurate derivatives for the next Newton step.
	const Scalar stepSize = sqrt( du * du + dv * dv );
	if( !referenceRefinement && stepSize < 1e-8 && (!vertex.retainAlphaEndpoint || vertex.HasAlphaEndpoint()) )
	{
		// Negligible step — no change needed
		vertex.valid = true;
		return true;
	}

	// Project back onto the surface via ray intersection.
	// The probe offset should be small enough to stay near the current
	// surface but large enough to clear the local geometry.
	const Scalar probeOffset = fmin( fmax( stepSize * 2.0, 0.01 ), 0.5 );
	bool snapped = false;

	// Try from the normal side first
	{
		const Ray probeRay(
			Point3Ops::mkPoint3( newPos, newNormal * probeOffset ),
			Vector3( -newNormal.x, -newNormal.y, -newNormal.z )
			);

		RayIntersection ri( probeRay, nullRasterizerState );
		vertex.pObject->IntersectRay( ri, 2.0 * probeOffset, true, true, false );

		if( ri.geometric.bHit )
		{
            const auto endpoint = vertex.retainAlphaEndpoint ?
                std::make_shared<const RayIntersection>(ri) : nullptr;
			// Apply modifier — same reason as in BuildSeedChain.  Stage 2
			// of the two-stage solver (smoothing == 0) wants the perturbed
			// normal so SMS's chain matches PT's bumpy ray traversal.
			if( ri.pModifier ) {
				ri.pModifier->Modify( ri.geometric );
			}
			vertex.position = referenceRefinement ? SMSReferenceSurfacePoint(*vertex.pObject,ri.geometric) : ri.geometric.ptIntersection;
			vertex.normal = ri.geometric.vNormal;
			vertex.geomNormal = ri.geometric.UnflippedGeomNormal();	// DL-70: the TRUE outward normal (see ManifoldSolver.h)
			vertex.uv = ri.geometric.ptCoord;
			vertex.objectPosition = ri.geometric.ptObjIntersec;
            vertex.alphaEndpoint = endpoint;
            vertex.alphaEndpointPosition = vertex.position;
			snapped = true;
		}
	}

	if( !snapped )
	{
		// Try from the other side
		const Ray probeRay2(
			Point3Ops::mkPoint3( newPos, newNormal * (-probeOffset) ),
			newNormal
			);

		RayIntersection ri2( probeRay2, nullRasterizerState );
		vertex.pObject->IntersectRay( ri2, 2.0 * probeOffset, true, true, false );

		if( ri2.geometric.bHit )
		{
            const auto endpoint = vertex.retainAlphaEndpoint ?
                std::make_shared<const RayIntersection>(ri2) : nullptr;
			if( ri2.pModifier ) {
				ri2.pModifier->Modify( ri2.geometric );
			}
			vertex.position = referenceRefinement ? SMSReferenceSurfacePoint(*vertex.pObject,ri2.geometric) : ri2.geometric.ptIntersection;
			vertex.normal = ri2.geometric.vNormal;
			vertex.geomNormal = ri2.geometric.UnflippedGeomNormal();	// DL-70: the TRUE outward normal (see ManifoldSolver.h)
			vertex.uv = ri2.geometric.ptCoord;
			vertex.objectPosition = ri2.geometric.ptObjIntersec;
            vertex.alphaEndpoint = endpoint;
            vertex.alphaEndpointPosition = vertex.position;
			snapped = true;
		}
	}

	if( !snapped )
	{
        // Alpha requires a position-producing geometric hit. Reject this
        // Newton step instead of publishing an unverified linear endpoint.
        if (vertex.retainAlphaEndpoint) return false;
		// Fall back to the linear approximation (no re-snap)
		vertex.position = newPos;
		vertex.normal = newNormal;
		// Zero the geometric slot — `newNormal` is the linearised
		// SHADING-frame normal, not a geometric-face value.  Writing it
		// here would silently lie to ValidateChainPhysics and any other
		// downstream consumer.  The validator's `SquaredModulus > NEARZERO`
		// fallback then degrades to `vertex.normal`, exactly what an
		// honest shading proxy is.
		vertex.geomNormal = Vector3( 0, 0, 0 );
	}

	// Recompute surface derivatives at the new position
	vertex.valid = ComputeVertexDerivatives( vertex, 0, referenceRefinement );
	return vertex.valid;
}

//////////////////////////////////////////////////////////////////////
// ComputeVertexDerivatives
//
//   Fills in dpdu, dpdv, dndu, dndv by querying the object's
//   geometry with proper world/object space transforms.
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::OrthonormalizeTangentFrame(
	ManifoldVertex& vertex
	) const
{
	// Project dpdu and dpdv into the tangent plane.
	const Vector3& n = vertex.normal;
	Scalar d;

	d = Vector3Ops::Dot( vertex.dpdu, n );
	vertex.dpdu = Vector3(
		vertex.dpdu.x - n.x * d,
		vertex.dpdu.y - n.y * d,
		vertex.dpdu.z - n.z * d );

	d = Vector3Ops::Dot( vertex.dpdv, n );
	vertex.dpdv = Vector3(
		vertex.dpdv.x - n.x * d,
		vertex.dpdv.y - n.y * d,
		vertex.dpdv.z - n.z * d );

	// Gram-Schmidt: make dpdv perpendicular to dpdu.
	//
	// CRITICAL: apply the same projection to dndv (normal derivative)
	// as we apply to dpdv.  Gram-Schmidt with
	//     dpdv' = dpdv - α·dpdu     (α = (dpdv·dpdu)/|dpdu|²)
	// is equivalent to reparameterizing along a new v-direction
	// v' = v - α·u.  By the chain rule dn/dv' = dn/dv - α·dn/du, so
	// dndv must receive the same correction or it ends up representing
	// the rate of change in the OLD (non-orthogonal) v-direction while
	// dpdv and the subsequent 1/|dpdv| rescale below are in the NEW
	// (orthogonal) v-direction — inflating |dndv| by 1/sin(θ) where θ
	// is the angle between the raw dpdu and dpdv.
	// On analytic surfaces (sphere_geometry etc.) the raw dpdu, dpdv
	// are already orthogonal and both projections collapse to zero —
	// a no-op that costs a few flops but doesn't change results.
	// On triangle meshes (including displaced_geometry with disp_scale=0)
	// the raw derivatives come from e1=V1-V0, e2=V2-V0 which are NEVER
	// orthogonal, so the correction is essential.
	const Scalar uSq = Vector3Ops::SquaredModulus( vertex.dpdu );
	if( uSq > NEARZERO ) {
		const Scalar proj = Vector3Ops::Dot( vertex.dpdv, vertex.dpdu ) / uSq;
		vertex.dpdv = Vector3(
			vertex.dpdv.x - vertex.dpdu.x * proj,
			vertex.dpdv.y - vertex.dpdu.y * proj,
			vertex.dpdv.z - vertex.dpdu.z * proj );
		vertex.dndv = Vector3(
			vertex.dndv.x - vertex.dndu.x * proj,
			vertex.dndv.y - vertex.dndu.y * proj,
			vertex.dndv.z - vertex.dndu.z * proj );
	}

	// If dpdu collapsed, pick any tangent.
	const Scalar uLen = Vector3Ops::Magnitude( vertex.dpdu );
	if( uLen < NEARZERO ) {
		vertex.dpdu = Vector3Ops::Normalize( Vector3Ops::Perpendicular( n ) );
	} else {
		vertex.dpdu = vertex.dpdu * (1.0 / uLen);
	}

	// If dpdv collapsed, derive from cross product.
	const Scalar vLen = Vector3Ops::Magnitude( vertex.dpdv );
	if( vLen < NEARZERO ) {
		vertex.dpdv = Vector3Ops::Cross( n, vertex.dpdu );
	} else {
		vertex.dpdv = vertex.dpdv * (1.0 / vLen);
	}

	// Scale normal derivatives so they represent rate of change per unit
	// displacement in the NEW unit-length (dpdu, dpdv) basis.  The raw
	// dn*/d* entries from the geometry are in the ORIGINAL parameterization
	// scale (e.g. per triangle edge).  Dividing by the original magnitude
	// converts to per-unit-length.  For flat surfaces these are zero
	// either way and the rescale is a no-op.
	if( uLen > NEARZERO ) {
		const Scalar invU = 1.0 / uLen;
		vertex.dndu = Vector3(
			vertex.dndu.x * invU,
			vertex.dndu.y * invU,
			vertex.dndu.z * invU );
	}
	if( vLen > NEARZERO ) {
		const Scalar invV = 1.0 / vLen;
		vertex.dndv = Vector3(
			vertex.dndv.x * invV,
			vertex.dndv.y * invV,
			vertex.dndv.z * invV );
	}
}

bool ManifoldSolver::ComputeNativeVertexFrame(ManifoldVertex& vertex, Scalar projectionDistance) const
{
    const Vector3 n=Vector3Ops::Normalize(vertex.geomNormal);
    const Scalar roundoff=8*std::numeric_limits<Scalar>::epsilon()*std::max({Scalar(1),
        std::fabs(vertex.position.x),std::fabs(vertex.position.y),std::fabs(vertex.position.z)});
    const Scalar allowance=std::max(roundoff,projectionDistance);
    // Compare both projections before choosing a surface. A first acceptable
    // hit can be an occluding sheet even when the other ray reaches the
    // requested point exactly. Shorten occluded probes without moving it.
    Scalar offset=std::max(Scalar(.05),allowance*2);
    for(unsigned refinement=0;refinement<64 && offset>=roundoff;++refinement,offset*=.5) {
        ManifoldVertex closest=vertex;
        Scalar closestDistance=RISE_INFINITY;
        for(int side:{1,-1}) {
            RayIntersection hit(Ray(Point3Ops::mkPoint3(vertex.position,n*(side*offset)),n*(-side)),nullRasterizerState);
            vertex.pObject->IntersectRay(hit,offset*2,true,true,false);
            if(!hit.geometric.bHit) continue;
            const Point3 surface=SMSReferenceSurfacePoint(*vertex.pObject,hit.geometric);
            const Scalar distance=Point3Ops::Distance(surface,vertex.position);
            if(distance>allowance || distance>=closestDistance) continue;
            closestDistance=distance;
            closest.position=surface;
            closest.normal=SMSNativeEventNormal(*vertex.pMaterial,hit.geometric);
            closest.geomNormal=hit.geometric.UnflippedGeomNormal();
            closest.uv=hit.geometric.ptCoord;closest.objectPosition=hit.geometric.ptObjIntersec;
            OrthonormalBasis3D frame;frame.CreateFromW(closest.geomNormal);
            closest.dpdu=frame.u();closest.dpdv=frame.v();
            closest.dndu=Vector3(0,0,0);closest.dndv=Vector3(0,0,0);closest.valid=true;
        }
        if(closestDistance<=allowance) {vertex=std::move(closest);return true;}
    }
    return false;
}

bool ManifoldSolver::ComputeVertexDerivatives(
	ManifoldVertex& vertex,
	Scalar smoothing, bool referenceRefinement
	) const
{
	if(nativeEventConstraints && SMSNeedsNativeFrame(vertex) && smoothing==0)
	{
		return ComputeNativeVertexFrame(vertex,0);
	}

	if( !vertex.pObject )
	{
		return false;
	}

	// Smoothing-aware analytical path (SMS two-stage solver, Zeltner 2020 §5).
	// ONLY engaged at smoothing > 0 — at smoothing = 0 we want the
	// existing on-mesh derivatives (per-triangle UV-Jacobian for triangle
	// meshes, FD probe for smooth analytical primitives), because those
	// derivatives describe the *actual* surface that PT's emission-
	// suppression machinery has already paired with.  Replacing them with
	// the smooth analytical equivalent at smoothing = 0 was the rejected
	// "Fix C" — it gives Newton's J a different surface than the rendering
	// pipeline uses, and ΣL_sms/ΣL_supp collapses (see investigation
	// timeline in docs/SMS_TWO_STAGE_SOLVER.md).
	if( smoothing > 0.0 )
	{
        vertex.alphaEndpoint.reset();
		Point3  aP;
		Vector3 aN, aDpdu, aDpdv, aDndu, aDndv;
		if( vertex.pObject->ComputeAnalyticalDerivatives(
				vertex.uv, smoothing, aP, aN, aDpdu, aDpdv, aDndu, aDndv ) )
		{
			vertex.position = aP;
			vertex.normal   = aN;
			vertex.geomNormal = aN;	// analytical primitive: shading == geometric
			vertex.dpdu     = aDpdu;
			vertex.dpdv     = aDpdv;
			vertex.dndu     = aDndu;
			vertex.dndv     = aDndv;
			OrthonormalizeTangentFrame( vertex );
			vertex.valid = true;
			return true;
		}
		// No analytical path available — Stage 1 of the two-stage solver
		// can't proceed for this vertex.  Caller (Solve) sees failure
		// and falls back to single-stage Newton.
		return false;
	}

	// Fast path: try to get analytical derivatives at the current
	// position via a ray cast that the geometry can populate at
	// intersection time (triangle meshes do this; see
	// TriangleMeshGeometryIndexedSpecializations.h).  If the
	// geometry populates ri.derivatives, we skip the expensive
	// 4-probe FD walk below.
	//
	// This probe ALSO serves as the on-surface verification.  If the
	// vertex has been pushed off the specular geometry (e.g. Newton
	// stepping beyond a bounded plane's extent), the probe ray misses
	// and we reject the vertex.  Without this, the fallback FD path
	// would synthesize fake tangent frames from the stale normal and
	// Newton would converge to algebraically-satisfied but
	// geometrically-nonexistent paths, causing false contribution in
	// the penumbra of bounded refractors.
	bool onSurface = false;
	{
		const Scalar probeOffsetAnalytic = 0.05;
		const Ray probeRay(
			Point3Ops::mkPoint3( vertex.position, vertex.normal * probeOffsetAnalytic ),
			Vector3( -vertex.normal.x, -vertex.normal.y, -vertex.normal.z ) );
		RayIntersection ri( probeRay, nullRasterizerState );
		vertex.pObject->IntersectRay( ri, 2.0 * probeOffsetAnalytic, true, true, false );
		if( ri.geometric.bHit ) {
		    const bool initializeEndpoint = vertex.retainAlphaEndpoint && !vertex.HasAlphaEndpoint();
		    const auto endpoint = vertex.retainAlphaEndpoint ?
		        std::make_shared<const RayIntersection>(ri) : nullptr;
			// Apply intersection modifier (bump map / normal map) so SMS
			// sees the perturbed normal — same as RayCaster does for the
			// rendering pipeline.  Without this, SMS's chain operates on
			// the unperturbed surface while PT operates on the perturbed
			// one and energy doesn't balance.
			if( ri.pModifier ) {
				ri.pModifier->Modify( ri.geometric );
			}
			onSurface = true;
		    // Analytic Stage1 and synthetic seeds lack a physical record.
		    // Publish this actual hit BEFORE Newton uses the vertex; never
		    // associate a neighbouring FD hit with an unchanged endpoint.
		    if (initializeEndpoint) {
		        vertex.position = referenceRefinement ? SMSReferenceSurfacePoint(*vertex.pObject,ri.geometric) : ri.geometric.ptIntersection;
		        vertex.normal = ri.geometric.vNormal;
		        vertex.geomNormal = ri.geometric.UnflippedGeomNormal();
		        vertex.uv = ri.geometric.ptCoord;
		        vertex.objectPosition = ri.geometric.ptObjIntersec;
		        vertex.alphaEndpoint = endpoint;
		        vertex.alphaEndpointPosition = vertex.position;
		    }
			if( ri.geometric.derivatives.valid ) {
				vertex.position = referenceRefinement ? SMSReferenceSurfacePoint(*vertex.pObject,ri.geometric) : ri.geometric.ptIntersection;
		        vertex.alphaEndpoint = endpoint;
		        vertex.alphaEndpointPosition = vertex.position;
				vertex.normal = ri.geometric.vNormal;
				vertex.geomNormal = ri.geometric.UnflippedGeomNormal();	// DL-70: the TRUE outward normal (see ManifoldSolver.h)
				vertex.dpdu = ri.geometric.derivatives.dpdu;
				vertex.dpdv = ri.geometric.derivatives.dpdv;
				vertex.dndu = ri.geometric.derivatives.dndu;
				vertex.dndv = ri.geometric.derivatives.dndv;
#if SMS_SMOOTH_BASE_JACOBIAN
				// Override the just-populated mesh derivatives with the
				// smooth analytical surface's derivatives at the same uv,
				// when the geometry exposes them.  Position and normal
				// stay from the ray-cast (residual is evaluated against
				// the actual bumpy mesh); only the Newton-Jacobian inputs
				// dpdu/dpdv/dndu/dndv switch to the smooth model.
				{
					Point3 aP;
					Vector3 aN, aDpdu, aDpdv, aDndu, aDndv;
					if( vertex.pObject->ComputeAnalyticalDerivatives(
							vertex.uv, Scalar( 1.0 ),
							aP, aN, aDpdu, aDpdv, aDndu, aDndv ) )
					{
						vertex.dpdu = aDpdu;
						vertex.dpdv = aDpdv;
						vertex.dndu = aDndu;
						vertex.dndv = aDndv;
					}
				}
#endif
				OrthonormalizeTangentFrame( vertex );
				vertex.valid = true;
				return true;
			}
		} else {
			// Probe from the opposite side too before giving up
			const Ray probeRay2(
				Point3Ops::mkPoint3( vertex.position, vertex.normal * (-probeOffsetAnalytic) ),
				vertex.normal );
			RayIntersection ri2( probeRay2, nullRasterizerState );
			vertex.pObject->IntersectRay( ri2, 2.0 * probeOffsetAnalytic, true, true, false );
			if( ri2.geometric.bHit ) {
			    const bool initializeEndpoint = vertex.retainAlphaEndpoint && !vertex.HasAlphaEndpoint();
			    const auto endpoint = vertex.retainAlphaEndpoint ?
			        std::make_shared<const RayIntersection>(ri2) : nullptr;
				if( ri2.pModifier ) {
					ri2.pModifier->Modify( ri2.geometric );
				}
				onSurface = true;
			    // Analytic Stage1 and synthetic seeds lack a physical record.
			    // Publish this actual hit BEFORE Newton uses the vertex; never
			    // associate a neighbouring FD hit with an unchanged endpoint.
			    if (initializeEndpoint) {
			        vertex.position = referenceRefinement ? SMSReferenceSurfacePoint(*vertex.pObject,ri2.geometric) : ri2.geometric.ptIntersection;
			        vertex.normal = ri2.geometric.vNormal;
			        vertex.geomNormal = ri2.geometric.UnflippedGeomNormal();
			        vertex.uv = ri2.geometric.ptCoord;
			        vertex.objectPosition = ri2.geometric.ptObjIntersec;
			        vertex.alphaEndpoint = endpoint;
			        vertex.alphaEndpointPosition = vertex.position;
			    }
				if( ri2.geometric.derivatives.valid ) {
					vertex.position = referenceRefinement ? SMSReferenceSurfacePoint(*vertex.pObject,ri2.geometric) : ri2.geometric.ptIntersection;
			        vertex.alphaEndpoint = endpoint;
			        vertex.alphaEndpointPosition = vertex.position;
					vertex.normal = ri2.geometric.vNormal;
					vertex.geomNormal = ri2.geometric.UnflippedGeomNormal();	// DL-70: the TRUE outward normal (see ManifoldSolver.h)
					vertex.dpdu = ri2.geometric.derivatives.dpdu;
					vertex.dpdv = ri2.geometric.derivatives.dpdv;
					vertex.dndu = ri2.geometric.derivatives.dndu;
					vertex.dndv = ri2.geometric.derivatives.dndv;
#if SMS_SMOOTH_BASE_JACOBIAN
					// See companion override at the front-probe site.
					{
						Point3 aP;
						Vector3 aN, aDpdu, aDpdv, aDndu, aDndv;
						if( vertex.pObject->ComputeAnalyticalDerivatives(
								vertex.uv, Scalar( 1.0 ),
								aP, aN, aDpdu, aDpdv, aDndu, aDndv ) )
						{
							vertex.dpdu = aDpdu;
							vertex.dpdv = aDpdv;
							vertex.dndu = aDndu;
							vertex.dndv = aDndv;
						}
					}
#endif
					OrthonormalizeTangentFrame( vertex );
					vertex.valid = true;
					return true;
				}
			}
		}
	}

	if( !onSurface ) {
		// Vertex is not on the specular geometry.  This happens when
		// Newton steps beyond the object's extent or when a chain
		// goes through empty space.  Reject — fake synthesized
		// derivatives would mislead the solver into accepting
		// non-physical paths.
		return false;
	}

	// Fallback: central-difference FD probes.  Kept for geometries
	// that don't yet populate ri.derivatives during IntersectRay.
	// (Sphere, torus, ellipsoid, cylinder, box, disk, clipped plane,
	// infinite plane — eventual plan is to populate derivatives at
	// intersection time for all of them, then this FD path can be
	// retired.)
	const Scalar eps = 5e-4;

	Vector3 tangent_u, tangent_v;

	if( Vector3Ops::SquaredModulus( vertex.dpdu ) > NEARZERO &&
		Vector3Ops::SquaredModulus( vertex.dpdv ) > NEARZERO )
	{
		tangent_u = Vector3Ops::Normalize( vertex.dpdu );
		tangent_v = Vector3Ops::Normalize( vertex.dpdv );
	}
	else
	{
		tangent_u = Vector3Ops::Perpendicular( vertex.normal );
		tangent_u = Vector3Ops::Normalize( tangent_u );
		tangent_v = Vector3Ops::Cross( vertex.normal, tangent_u );
		tangent_v = Vector3Ops::Normalize( tangent_v );
	}

	// Probe a surface point near vertex.position along a tangent direction.
	// Returns true if successful, filling outPos and outNormal.
	//
	// The probe ray shoots along -normal from above the test position, and
	// must cover the local surface displacement amplitude to be reliable.
	// Too small: misses displaced-mesh vertices whose local surface is
	// higher/lower than the offset (observed: 85% failure rate with 0.01
	// offset on a slab with 0.1 displacement range).  Too large: can
	// punch through thin geometry and hit the far surface.
	//
	// Strategy: try a sequence of offsets from small to large, stopping
	// at the first successful hit.  The smallest offset that works gives
	// the most accurate hit (least tangential drift of the probe).
	struct ProbeResult { Point3 pos; Vector3 normal; bool ok; };
	auto probeAt = [&]( const Point3& testPos ) -> ProbeResult
	{
		ProbeResult r;
		r.ok = false;

		// Try a sequence of probe offsets.  Start small to avoid punching
		// through thin geometry; grow to handle displaced meshes.
		const Scalar probeOffsets[] = { 0.01, 0.05, 0.2, 1.0 };
		const unsigned int nOffsets = sizeof(probeOffsets) / sizeof(probeOffsets[0]);

		for( unsigned int oi = 0; oi < nOffsets; oi++ )
		{
			const Scalar probeOffset = probeOffsets[oi];

			// Probe from the normal side (ray goes -n toward surface)
			{
				const Ray probeRay(
					Point3Ops::mkPoint3( testPos, vertex.normal * probeOffset ),
					Vector3( -vertex.normal.x, -vertex.normal.y, -vertex.normal.z ) );
				RayIntersection ri( probeRay, nullRasterizerState );
				vertex.pObject->IntersectRay( ri, 2.0 * probeOffset, true, true, false );
				if( ri.geometric.bHit )
				{
					// Apply modifier (e.g. bump map) so the FD captures
					// derivatives of the *perturbed* normal field, not the
					// unperturbed one.  Without this, vertex.dndu/dndv
					// describe the smooth surface even when vertex.normal
					// is bumpy — Newton's Jacobian sees a different
					// surface than the constraint.
					if( ri.pModifier ) {
						ri.pModifier->Modify( ri.geometric );
					}
					r.pos = ri.geometric.ptIntersection;
					r.normal = ri.geometric.vNormal;
					r.ok = true;
					return r;
				}
			}

			// Try from the other side (ray goes +n, for when the local
			// surface is on the other side of testPos)
			{
				const Ray probeRay2(
					Point3Ops::mkPoint3( testPos, vertex.normal * (-probeOffset) ),
					vertex.normal );
				RayIntersection ri2( probeRay2, nullRasterizerState );
				vertex.pObject->IntersectRay( ri2, 2.0 * probeOffset, true, true, false );
				if( ri2.geometric.bHit )
				{
					if( ri2.pModifier ) {
						ri2.pModifier->Modify( ri2.geometric );
					}
					r.pos = ri2.geometric.ptIntersection;
					r.normal = ri2.geometric.vNormal;
					r.ok = true;
					return r;
				}
			}
		}

		return r;
	};

	// Central differences: probe at +eps and -eps in each tangent direction
	ProbeResult u_plus  = probeAt( Point3Ops::mkPoint3( vertex.position, tangent_u * eps ) );
	ProbeResult u_minus = probeAt( Point3Ops::mkPoint3( vertex.position, tangent_u * (-eps) ) );
	ProbeResult v_plus  = probeAt( Point3Ops::mkPoint3( vertex.position, tangent_v * eps ) );
	ProbeResult v_minus = probeAt( Point3Ops::mkPoint3( vertex.position, tangent_v * (-eps) ) );


	if( u_plus.ok && u_minus.ok )
	{
		const Scalar inv2eps = 1.0 / (2.0 * eps);
		vertex.dpdu = Vector3(
			(u_plus.pos.x - u_minus.pos.x) * inv2eps,
			(u_plus.pos.y - u_minus.pos.y) * inv2eps,
			(u_plus.pos.z - u_minus.pos.z) * inv2eps );
		vertex.dndu = Vector3(
			(u_plus.normal.x - u_minus.normal.x) * inv2eps,
			(u_plus.normal.y - u_minus.normal.y) * inv2eps,
			(u_plus.normal.z - u_minus.normal.z) * inv2eps );
	}
	else if( u_plus.ok )
	{
		// Fall back to forward difference
		const Scalar invEps = 1.0 / eps;
		vertex.dpdu = Vector3(
			(u_plus.pos.x - vertex.position.x) * invEps,
			(u_plus.pos.y - vertex.position.y) * invEps,
			(u_plus.pos.z - vertex.position.z) * invEps );
		vertex.dndu = Vector3(
			(u_plus.normal.x - vertex.normal.x) * invEps,
			(u_plus.normal.y - vertex.normal.y) * invEps,
			(u_plus.normal.z - vertex.normal.z) * invEps );
	}
	else
	{
		vertex.dpdu = tangent_u;
		vertex.dndu = Vector3( 0, 0, 0 );
	}

	if( v_plus.ok && v_minus.ok )
	{
		const Scalar inv2eps = 1.0 / (2.0 * eps);
		vertex.dpdv = Vector3(
			(v_plus.pos.x - v_minus.pos.x) * inv2eps,
			(v_plus.pos.y - v_minus.pos.y) * inv2eps,
			(v_plus.pos.z - v_minus.pos.z) * inv2eps );
		vertex.dndv = Vector3(
			(v_plus.normal.x - v_minus.normal.x) * inv2eps,
			(v_plus.normal.y - v_minus.normal.y) * inv2eps,
			(v_plus.normal.z - v_minus.normal.z) * inv2eps );
	}
	else if( v_plus.ok )
	{
		const Scalar invEps = 1.0 / eps;
		vertex.dpdv = Vector3(
			(v_plus.pos.x - vertex.position.x) * invEps,
			(v_plus.pos.y - vertex.position.y) * invEps,
			(v_plus.pos.z - vertex.position.z) * invEps );
		vertex.dndv = Vector3(
			(v_plus.normal.x - vertex.normal.x) * invEps,
			(v_plus.normal.y - vertex.normal.y) * invEps,
			(v_plus.normal.z - vertex.normal.z) * invEps );
	}
	else
	{
		vertex.dpdv = tangent_v;
		vertex.dndv = Vector3( 0, 0, 0 );
	}

	// Project dpdu and dpdv into the tangent plane and orthogonalize.
	// Finite difference probes on curved surfaces can produce tangent
	// vectors with a normal component, which makes the Jacobian
	// inconsistent with the constraint's tangent-plane projection.
	{
		const Vector3& n = vertex.normal;

		// Project dpdu into tangent plane
		Scalar d_n = Vector3Ops::Dot( vertex.dpdu, n );
		vertex.dpdu = Vector3(
			vertex.dpdu.x - n.x * d_n,
			vertex.dpdu.y - n.y * d_n,
			vertex.dpdu.z - n.z * d_n );

		// Project dpdv into tangent plane
		d_n = Vector3Ops::Dot( vertex.dpdv, n );
		vertex.dpdv = Vector3(
			vertex.dpdv.x - n.x * d_n,
			vertex.dpdv.y - n.y * d_n,
			vertex.dpdv.z - n.z * d_n );

		// Gram-Schmidt: make dpdv orthogonal to dpdu
		const Scalar dpdu_sq = Vector3Ops::SquaredModulus( vertex.dpdu );
		if( dpdu_sq > NEARZERO )
		{
			const Scalar proj = Vector3Ops::Dot( vertex.dpdv, vertex.dpdu ) / dpdu_sq;
			vertex.dpdv = Vector3(
				vertex.dpdv.x - vertex.dpdu.x * proj,
				vertex.dpdv.y - vertex.dpdu.y * proj,
				vertex.dpdv.z - vertex.dpdu.z * proj );
		}

		// If dpdv collapsed, reconstruct from cross product
		if( Vector3Ops::SquaredModulus( vertex.dpdv ) < NEARZERO )
		{
			vertex.dpdv = Vector3Ops::Cross( n, vertex.dpdu );
		}
	}

	return true;
}

//////////////////////////////////////////////////////////////////////
// NewtonSolve
//
//   Runs Newton iteration to solve C(x) = 0.
//   Modifies chain in-place.  Returns true if converged.
//////////////////////////////////////////////////////////////////////

bool ManifoldSolver::NewtonSolve(
	std::vector<ManifoldVertex>& chain,
	const Point3& fixedStart,
	const Point3& fixedEnd,
	Scalar smoothing, Scalar convergenceThreshold
	) const
{
    const Scalar threshold = convergenceThreshold > 0 ? convergenceThreshold : config.solverThreshold;
#if SMS_TRACE_DIAGNOSTIC
	// Per-failure-mode counters.  Each return-false (or accepted-soft) path
	// in this routine increments exactly one counter.  Periodic dump every
	// 200k Newton calls so the breakdown shows up in the render log.
	static std::atomic<int> g_newton_total{ 0 };
	static std::atomic<int> g_newton_empty{ 0 };
	static std::atomic<int> g_newton_singular{ 0 };
	static std::atomic<int> g_newton_no_progress{ 0 };
	static std::atomic<int> g_newton_update_failed{ 0 };
	static std::atomic<int> g_newton_iter_limit{ 0 };
	static std::atomic<int> g_newton_soft_converged{ 0 };
	static std::atomic<int> g_newton_strict_converged{ 0 };
	// D4 sub-counters: at the noProgress failure site, bucket by which
	// iteration the line search died on, and by the residual norm at that
	// point (relative to solverThreshold).  Discriminates H4a (Jacobian
	// wrong from iter 0) vs H4b (Newton made progress then hit a non-
	// smooth wall).
	static std::atomic<int> g_np_iter[5]{ {0}, {0}, {0}, {0}, {0} };
	// Buckets:  0=iter==0  1=iter==1  2=iter==2  3=iter in [3,5]  4=iter>=6
	static std::atomic<int> g_np_norm[5]{ {0}, {0}, {0}, {0}, {0} };
	// Buckets relative to solverThreshold (default 1e-4):
	//   0: norm < 10·thr  (would soft-converge after iter limit)
	//   1: norm < 100·thr
	//   2: norm < 1000·thr
	//   3: norm < 1
	//   4: norm >= 1
	const int nt = g_newton_total.fetch_add( 1, std::memory_order_relaxed );
	if( (nt & 0x3ffff) == 0 && nt > 0 ) {
		GlobalLog()->PrintEx( eLog_Event,
			"SMS_NEWTON_STATS: total=%d empty=%d singular=%d noProgress=%d updateFailed=%d iterLimit=%d softOk=%d strictOk=%d",
			nt, g_newton_empty.load(),
			g_newton_singular.load(), g_newton_no_progress.load(),
			g_newton_update_failed.load(), g_newton_iter_limit.load(),
			g_newton_soft_converged.load(), g_newton_strict_converged.load() );
		GlobalLog()->PrintEx( eLog_Event,
			"SMS_NP_ITER:  iter0=%d  iter1=%d  iter2=%d  iter[3-5]=%d  iter>=6=%d",
			g_np_iter[0].load(), g_np_iter[1].load(), g_np_iter[2].load(),
			g_np_iter[3].load(), g_np_iter[4].load() );
		GlobalLog()->PrintEx( eLog_Event,
			"SMS_NP_NORM:  norm<10thr=%d  <100thr=%d  <1000thr=%d  <1=%d  >=1=%d",
			g_np_norm[0].load(), g_np_norm[1].load(), g_np_norm[2].load(),
			g_np_norm[3].load(), g_np_norm[4].load() );
	}
#endif

	const unsigned int k = static_cast<unsigned int>( chain.size() );

	if( k == 0 )
	{
#if SMS_TRACE_DIAGNOSTIC
		g_newton_empty.fetch_add( 1, std::memory_order_relaxed );
#endif
		return false;
	}

	// Levenberg-Marquardt damping factor.  Persists across iterations of
	// THIS Solve call: shrunk on accepted line-search steps (toward pure
	// Newton, which converges quadratically near a root), grown on
	// rejected line-search steps (toward gradient descent, which can
	// escape plateaus where Newton's J⁻¹·C direction is unreliable).
	// Init 0 means the first iter is pure Newton — preserves baseline
	// behaviour on well-conditioned chains.  When
	// `config.useLevenbergMarquardt` is false, this stays 0 throughout
	// and every LM-related branch below is a no-op.
	Scalar lmLambda = 0;
#if SMS_SOLVE_DIAG
	bool lmDidEscalate = false;
#endif

	for( unsigned int iter = 0; iter < config.maxIterations; iter++ )
	{
        SMSWorkerScratchLease scratch(nativeEventConstraints,config.referenceCounters);
        const auto capacity=std::max<unsigned>(config.maxChainDepth,k);
        if(config.domainCounters) config.domainCounters->newtonIterations.fetch_add(1, std::memory_order_relaxed);
#if SMS_SOLVE_DIAG
		if( config.useLevenbergMarquardt ) {
			g_solveDiag_lmTotalIters.fetch_add( 1, std::memory_order_relaxed );
			if( lmLambda > 0 ) {
				g_solveDiag_lmDamped.fetch_add( 1, std::memory_order_relaxed );
			}
		}
#endif
		// Evaluate constraint
		std::vector<Scalar> localC;
        auto& C=scratch.Enabled()?scratch.Scalars(0,2*capacity):localC;
		EvaluateConstraint( chain, fixedStart, fixedEnd, C );

		// Compute norm of constraint
		Scalar norm2 = 0.0;
		for( unsigned int i = 0; i < 2 * k; i++ )
		{
			norm2 += C[i] * C[i];
		}
		const Scalar norm = sqrt( norm2 );

		if( norm < threshold )
		{
#if SMS_TRACE_DIAGNOSTIC
			g_newton_strict_converged.fetch_add( 1, std::memory_order_relaxed );
#endif
#if SMS_SOLVE_DIAG
			if( config.useLevenbergMarquardt && lmDidEscalate ) {
				g_solveDiag_lmRecovered.fetch_add( 1, std::memory_order_relaxed );
			}
#endif
			return true;  // Converged
		}

		// Build Jacobian for Newton step.  Include curvature terms
		// (dndu / dndv via Weingarten) — on analytical smooth surfaces
		// like spheres, they dominate the Jacobian and Newton diverges
		// without them.  Original rationale for excluding curvature
		// (tri-mesh per-triangle discontinuity) is handled by the
		// mesh Jacobian returning zero dndu/dndv in those cases.
		std::vector<Scalar> localDiag,localUpper,localLower;
        auto& diag=scratch.Enabled()?scratch.Scalars(1,4*capacity):localDiag;
        auto& upper_blocks=scratch.Enabled()?scratch.Scalars(2,4*capacity):localUpper;
        auto& lower_blocks=scratch.Enabled()?scratch.Scalars(3,4*capacity):localLower;
		BuildJacobian( chain, fixedStart, fixedEnd, diag, upper_blocks, lower_blocks,
			/*includeCurvature=*/true );

		// Levenberg-Marquardt diagonal damping.  When `lmLambda > 0`,
		// add `lmLambda × mean(|J_ii|)` to each diagonal block's (0,0)
		// and (1,1) entries.  Scaling by the mean diagonal magnitude
		// keeps the damping order-of-magnitude appropriate regardless
		// of the chain length / IOR / surface scale at this vertex —
		// raw `lmLambda · I` would be dominated by the original J entries
		// at well-conditioned vertices and have no effect, while at
		// ill-conditioned vertices a large enough λ moves the step
		// direction toward gradient descent.  Skipped when LM is
		// disabled (`lmLambda` stays 0 throughout).
		if( config.useLevenbergMarquardt && lmLambda > 0 )
		{
			Scalar diagMagSum = 0;
			for( unsigned int i = 0; i < k; i++ ) {
				diagMagSum += std::fabs( diag[i*4]   );  // J(0,0) of vertex i
				diagMagSum += std::fabs( diag[i*4+3] );  // J(1,1) of vertex i
			}
			const Scalar diagMagMean = ( k > 0 ) ? diagMagSum / Scalar( 2 * k ) : Scalar( 1.0 );
			const Scalar dampAmount = lmLambda * diagMagMean;
			for( unsigned int i = 0; i < k; i++ ) {
				diag[i*4]   += dampAmount;
				diag[i*4+3] += dampAmount;
			}
		}

		// Solve for Newton step: J * delta = C  (we solve J * delta = C, then subtract)
		std::vector<Scalar> localDelta;
        auto& delta=scratch.Enabled()?scratch.Scalars(4,2*capacity):localDelta;
		if( !SolveBlockTridiagonal( diag, upper_blocks, lower_blocks, C, k, delta ) )
		{
#if SMS_TRACE_DIAGNOSTIC
			g_newton_singular.fetch_add( 1, std::memory_order_relaxed );
#endif
			return false;  // Singular Jacobian
		}

		// World-space step-norm cap.  After computing the unconstrained
		// Newton step `delta` (in tangent-plane (du,dv) parameters per
		// vertex), convert each vertex's per-iter displacement to world
		// space via the local tangent basis, find the maximum across
		// vertices, and if it exceeds <cap_frac> × mean_segment_length,
		// rescale the entire delta vector by the same factor.  Preserves
		// step direction; only bounds magnitude.
		//
		// Why mean segment length is the right scale: on heavy-
		// displacement geometry, the Jacobian can become ill-conditioned
		// at a vertex where the local normal field has a near-singular
		// (Weingarten) curvature term.  The unconstrained Newton step
		// then pushes that vertex far off the local manifold; the
		// backtracking line search has to halve up to 10× to recover and
		// often gives up.  A cap proportional to local feature size keeps
		// every iter's per-vertex move conservative without sacrificing
		// correctness — when the Jacobian IS well-conditioned and the
		// step is small relative to feature size, the cap is a no-op.
		if( kNewtonStepNormCapFrac > 0 )
		{
			// Mean segment length covers the vertex chain plus the
			// shading-point and emitter-point bookend segments.
			Scalar segSum = 0;
			Scalar segCount = 0;
			for( unsigned int i = 0; i < k; i++ ) {
				const Point3 prev = ( i == 0 ) ? fixedStart : chain[i-1].position;
				segSum += Point3Ops::Distance( prev, chain[i].position );
				segCount += 1;
			}
			if( k > 0 ) {
				segSum += Point3Ops::Distance( chain[k-1].position, fixedEnd );
				segCount += 1;
			}
			const Scalar meanSeg = ( segCount > 0 ) ? segSum / segCount : Scalar(1.0);
			const Scalar capWorld = kNewtonStepNormCapFrac * meanSeg;

			// Find max per-vertex world-space step magnitude.
			Scalar maxStepWorld = 0;
			for( unsigned int i = 0; i < k; i++ ) {
				const Vector3 dWorld =
					chain[i].dpdu * delta[2*i] +
					chain[i].dpdv * delta[2*i+1];
				const Scalar mag = Vector3Ops::Magnitude( dWorld );
				if( mag > maxStepWorld ) maxStepWorld = mag;
			}

			if( maxStepWorld > capWorld && maxStepWorld > NEARZERO ) {
				const Scalar scale = capWorld / maxStepWorld;
				for( unsigned int i = 0; i < 2 * k; i++ ) {
					delta[i] *= scale;
				}
#if SMS_SOLVE_DIAG
				g_solveDiag_capFired.fetch_add( 1, std::memory_order_relaxed );
				const uint64_t fixedRatio = static_cast<uint64_t>(
					( maxStepWorld / std::max( capWorld, Scalar(1e-12) ) ) * 1000.0 );
				g_solveDiag_capRatioSum.fetch_add( fixedRatio, std::memory_order_relaxed );
#endif
			}
#if SMS_SOLVE_DIAG
			g_solveDiag_capChecks.fetch_add( 1, std::memory_order_relaxed );
#endif
		}

		// Apply update with line search: try decreasing step sizes
		// until the constraint norm actually decreases.
		Scalar beta = 1.0;
		bool allValid = false;
		bool improved = false;

		// Save chain state
		std::vector<ManifoldVertex> localSaved;
        auto& savedChain=scratch.Enabled()?scratch.Vertices(0,capacity):localSaved;
        savedChain=chain;

		for( unsigned int attempt = 0; attempt < 10; attempt++ )
		{
			// Restore chain from saved state
			chain = savedChain;

			// Apply step: x_new = x - beta * delta
			allValid = true;
			for( unsigned int i = 0; i < k; i++ )
			{
				const Scalar du = -beta * delta[2*i];
				const Scalar dv = -beta * delta[2*i+1];

				if( !UpdateVertexOnSurface( chain[i], du, dv, smoothing, convergenceThreshold > 0 ) )
				{
					allValid = false;
					break;
				}

				if( !chain[i].valid )
				{
					allValid = false;
					break;
				}
			}

#if SMS_EDGE_AWARE_NEWTON
			// Linearization-trust check (edge-aware Newton step).  After
			// the step is applied and the chain re-projected to the
			// surface, compare the actual normal rotation at each vertex
			// to what the saved Jacobian predicted.  When the ratio
			// exceeds `kEdgeTrustRelRatio` (and both magnitudes are
			// above the absolute floor), the linearization wasn't
			// trustworthy for this step — the typical cause on a
			// triangle mesh is the new vertex landing in a different
			// triangle whose Phong-interpolation slope differs from
			// the source triangle's.  Reject and let line search halve.
			//
			// On smooth analytical surfaces with continuous derivatives,
			// predicted ≈ actual within higher-order terms and the test
			// rarely fires (verified on sms_k1_refract / k2_glassblock /
			// k2_glasssphere regressions: <0.1% reject rate).
			if( allValid )
			{
#if SMS_SOLVE_DIAG
				g_solveDiag_edgeChecks.fetch_add( 1, std::memory_order_relaxed );
#endif
				bool linearizationTrusted = true;
				for( unsigned int i = 0; i < k; i++ )
				{
					const Scalar du = -beta * delta[2*i];
					const Scalar dv = -beta * delta[2*i+1];
					// Predicted normal change from the savedChain Jacobian.
					const Vector3 predicted_dn(
						savedChain[i].dndu.x * du + savedChain[i].dndv.x * dv,
						savedChain[i].dndu.y * du + savedChain[i].dndv.y * dv,
						savedChain[i].dndu.z * du + savedChain[i].dndv.z * dv );
					const Scalar predicted_mag = Vector3Ops::Magnitude( predicted_dn );
					// Actual change: chain[i].normal − savedChain[i].normal.
					const Vector3 actual_dn(
						chain[i].normal.x - savedChain[i].normal.x,
						chain[i].normal.y - savedChain[i].normal.y,
						chain[i].normal.z - savedChain[i].normal.z );
					const Scalar actual_mag = Vector3Ops::Magnitude( actual_dn );

					if( actual_mag > kEdgeTrustAbsFloor &&
						actual_mag > kEdgeTrustRelRatio *
							std::max( predicted_mag, kEdgeTrustAbsFloor ) )
					{
						linearizationTrusted = false;
						break;
					}
				}
				if( !linearizationTrusted )
				{
#if SMS_SOLVE_DIAG
					g_solveDiag_edgeRejects.fetch_add( 1, std::memory_order_relaxed );
#endif
					beta *= 0.5;
					continue;
				}
			}
#endif

			if( allValid )
			{
				// Check if the norm actually decreased
				std::vector<Scalar> localTest;
                auto& C_test=scratch.Enabled()?scratch.Scalars(5,2*capacity):localTest;
				EvaluateConstraint( chain, fixedStart, fixedEnd, C_test );
				Scalar testNorm2 = 0.0;
				for( unsigned int i = 0; i < 2*k; i++ )
				{
					testNorm2 += C_test[i] * C_test[i];
				}
				if( sqrt(testNorm2) < norm )
				{
					improved = true;
					break;
				}
			}

			beta *= 0.5;
		}

		if( !improved )
		{
			// Even the smallest step didn't improve — but if Newton already
			// brought ||C|| within the soft-convergence band (10× the strict
			// threshold), accept the chain rather than discarding it.  On
			// triangle meshes whose chord-vs-arc tessellation error sets a
			// floor on the achievable residual, Newton can iterate to this
			// floor and then stall; without this acceptance, those chains
			// (≈1% of total Newton calls on the displaced Veach-egg scene
			// per SMS_NP_NORM diagnostic) get silently discarded even
			// though their constraint is satisfied to within visual
			// precision.  Mirrors the existing post-iter-limit soft-converge
			// check at the bottom of NewtonSolve — same threshold, applied
			// at the earlier exit too.
			chain = savedChain;
			if( norm < threshold * 10.0 )
			{
#if SMS_TRACE_DIAGNOSTIC
				g_newton_soft_converged.fetch_add( 1, std::memory_order_relaxed );
#endif
				return true;
			}

			if( config.useLevenbergMarquardt )
			{
				// Levenberg-Marquardt escalation: line search couldn't find
				// a β that decreases ||C|| — Newton's J⁻¹·C step direction
				// is itself unreliable.  Increase damping and retry the
				// same iter slot with a more gradient-descent-like solve.
				// When damping reaches `kLM_LambdaMax` without progress,
				// fall through to the legacy fail path — at that point
				// the problem is genuinely beyond LM's reach (multiple
				// basins with no descent connection, etc.).
				lmLambda = ( lmLambda <= 0 )
					? kLM_LambdaInit
					: std::min( lmLambda * kLM_LambdaUp, kLM_LambdaMax );
#if SMS_SOLVE_DIAG
				g_solveDiag_lmEscalated.fetch_add( 1, std::memory_order_relaxed );
				lmDidEscalate = true;
#endif
				if( lmLambda < kLM_LambdaMax ) {
					continue;  // retry next iter with more damping
				}
			}

#if SMS_TRACE_DIAGNOSTIC
			g_newton_no_progress.fetch_add( 1, std::memory_order_relaxed );
			// Bucket by iteration index where line search died
			{
				int b = 0;
				if( iter == 0 )      b = 0;
				else if( iter == 1 ) b = 1;
				else if( iter == 2 ) b = 2;
				else if( iter <= 5 ) b = 3;
				else                 b = 4;
				g_np_iter[b].fetch_add( 1, std::memory_order_relaxed );
			}
			// Bucket by current iteration's residual norm (in units of
			// solverThreshold), so we can see if the failure happened
			// near-converged or far from any solution.
			{
				const Scalar thr = threshold;
				int b = 0;
				if( norm < 10.0   * thr ) b = 0;
				else if( norm < 100.0  * thr ) b = 1;
				else if( norm < 1000.0 * thr ) b = 2;
				else if( norm < 1.0          ) b = 3;
				else                            b = 4;
				g_np_norm[b].fetch_add( 1, std::memory_order_relaxed );
			}
#endif
			return false;
		}

		// Step accepted.  Shrink LM damping toward 0 (pure Newton) so the
		// next iter can take a quadratically-converging Newton step when
		// the chain has moved into a well-conditioned region.  No-op when
		// LM is disabled (lmLambda stays 0).
		if( config.useLevenbergMarquardt && lmLambda > 0 )
		{
			lmLambda *= kLM_LambdaDown;
			if( lmLambda < kLM_LambdaMin ) lmLambda = 0;
		}

		if( !allValid )
		{
			// Restore and report failure
			chain = savedChain;
#if SMS_TRACE_DIAGNOSTIC
			g_newton_update_failed.fetch_add( 1, std::memory_order_relaxed );
#endif
			return false;
		}
	}

	// Did not converge within maxIterations.  Check if the constraint
	// norm is close enough to accept as a soft convergence.  On meshes
	// with discontinuous normals (triangle edges), Newton may oscillate
	// near the solution without reaching the strict threshold.  A
	// relaxed threshold of 10× catches these near-solutions.
	{
		SMSWorkerScratchLease finalScratch(nativeEventConstraints,config.referenceCounters);
        std::vector<Scalar> localFinal;
        auto& C_final=finalScratch.Enabled()?finalScratch.Scalars(0,2*std::max<unsigned>(config.maxChainDepth,k)):localFinal;
		EvaluateConstraint( chain, fixedStart, fixedEnd, C_final );
		Scalar norm2 = 0.0;
		for( unsigned int i = 0; i < 2 * k; i++ )
			norm2 += C_final[i] * C_final[i];
		if( sqrt(norm2) < threshold * 10.0 )
		{
#if SMS_TRACE_DIAGNOSTIC
			g_newton_soft_converged.fetch_add( 1, std::memory_order_relaxed );
#endif
			return true;  // Soft convergence
		}
	}

#if SMS_TRACE_DIAGNOSTIC
	g_newton_iter_limit.fetch_add( 1, std::memory_order_relaxed );
#endif
	return false;
}

//////////////////////////////////////////////////////////////////////
// SMSLoopSampler — sampler-dimension-drift firewall
//
//   Wraps a fresh RandomNumberGenerator + IndependentSampler in one
//   stack-scoped object.  Construct from the parent sampler at the
//   top of each `EvaluateAtShadingPoint*` entry; pass `.sampler` into
//   variable-count internal work (M-trial loop, Bernoulli K-loop,
//   `EstimatePDF`, `Solve`).
//
//   The parent sampler advances by exactly TWO 1-D dimensions
//   (regardless of how many internal trials run) when constructing
//   this scope, so an LDS sampler (Sobol etc.) keeps a predictable
//   dimension stream — `EvaluateAtShadingPoint*` no longer pollutes
//   downstream call sites.  The internal RNG is seeded from those
//   two dimensions via a multiplicative hash, so each pixel sample
//   gets a different RNG state and the variable-count work stays
//   i.i.d.-uniform (matches Mitsuba's purely-RNG SMS implementation).
//
//   Member-init order is intentional: `rng` declared first so its
//   constructor runs before `sampler`'s reference is bound.
//////////////////////////////////////////////////////////////////////

namespace {
	struct SMSLoopSampler {
		RandomNumberGenerator rng;
		IndependentSampler    sampler;

		explicit SMSLoopSampler( ISampler& parent, unsigned int stream = 0 )
			: rng( deriveSeed( parent ) ^ stream ), sampler( rng ) {}

	private:
		static unsigned int deriveSeed( ISampler& parent ) {
			const Scalar s0 = parent.Get1D();
			const Scalar s1 = parent.Get1D();
			const unsigned int a = static_cast<unsigned int>( s0 * 4294967295.0 );
			const unsigned int b = static_cast<unsigned int>( s1 * 4294967295.0 );
			// Golden-ratio multiplicative hash combine — decorrelates
			// the two parent draws so adjacent LDS dimension pairs
			// produce well-spread RNG seeds.
			return a ^ ( b * 2654435761u );
		}
	};

	// Fisher-Yates partial shuffle that retains the first `cap` elements
	// of `seeds` as a uniform random subset.  Used to bound the per-
	// shading-point Newton-solve cost when QuerySeeds returns a dense
	// neighbour set (a focused caustic can return hundreds of photons
	// from a 1M-photon kd-tree).  `cap == 0` disables the cap.
	//
	// O(cap) work; the rest of the vector is left untouched at the tail
	// and resize() trims it.  Caller-provided sampler keeps the LDS
	// dimensions consistent with the rest of the SMS trial loop.
	inline void RandomSubsamplePhotonSeeds(
		std::vector<SMSPhoton>& seeds,
		unsigned int cap,
		ISampler& sampler )
	{
		if( cap == 0 || seeds.size() <= cap ) return;
		for( unsigned int i = 0; i < cap; i++ ) {
			const unsigned int range = static_cast<unsigned int>( seeds.size() - i );
			const unsigned int j = i + static_cast<unsigned int>(
				sampler.Get1D() * range );
			const std::vector<SMSPhoton>::size_type jj =
				( static_cast<std::vector<SMSPhoton>::size_type>( j ) >= seeds.size() ) ?
					( seeds.size() - 1 ) :
					static_cast<std::vector<SMSPhoton>::size_type>( j );
			if( jj != i ) std::swap( seeds[i], seeds[jj] );
		}
		seeds.resize( cap );
	}
}

//////////////////////////////////////////////////////////////////////
// EnumerateSpecularCasters (static)
//
//   Walk the scene's object manager once and collect every object
//   whose material reports `isSpecular`.  Used to build the cached
//   list consumed by uniform-on-shape SMS seeding (Mitsuba-faithful
//   single-/multi-scatter).  Each object is sampled at its
//   (0.5, 0.5, 0.5) parametric centre via UniformRandomPoint to get
//   a valid RayIntersectionGeometric for the GetSpecularInfo query
//   — this matters for materials whose painters are textured
//   functions of (u, v).
//////////////////////////////////////////////////////////////////////

namespace {
	class SpecularCasterCollector : public IEnumCallback<IObject>
	{
	public:
		std::vector<const IObject*>& out;
		explicit SpecularCasterCollector( std::vector<const IObject*>& o ) : out( o ) {}

		bool operator()( const IObject& obj ) override
		{
			const IMaterial* pMat = obj.GetMaterial();
			if( !pMat ) {
				return true;   // continue enumeration
			}

			// Sibling of the LuminaryManager::AddToLuminaryList / SSS null-geometry
			// crash fix: obj.GetGeometry() can legitimately be null (a CSGObject has
			// no single owned IGeometry* -- its shape is synthesized from its two
			// operand objects and it does not override UniformRandomPoint()).  The
			// probe loop below unconditionally calls obj.UniformRandomPoint(...) for
			// EVERY material'd object in the scene, so a null-geometry object with
			// ANY material (not just an emitter) reached this and null-derefed
			// (Object::UniformRandomPoint -> pGeometry->UniformRandomPoint with
			// pGeometry == 0) the moment SMS classification enumerated it -- before
			// the CanBeAreaLight() gate further down even runs.  Skip it up front
			// with the same refusal semantics as that gate.
			if( !obj.GetGeometry() ) {
				return true;   // continue enumeration
			}

			// Probe at several deterministic prands.  A `SwitchPel`-style
			// painter that keys off (u, v) and reports `isSpecular` only
			// in some patches must still classify the object as a caster
			// — single-point probes at (0.5, 0.5, 0.5) misclassify those.
			static const Point3 kProbes[] = {
				Point3( 0.5, 0.5, 0.5 ),
				Point3( 0.25, 0.5, 0.75 ),
				Point3( 0.75, 0.25, 0.25 )
			};

			for( const Point3& prand : kProbes )
			{
				Point3 p;
				Vector3 n;
				Point2 uv;
				obj.UniformRandomPoint( &p, &n, &uv, prand );

				Ray dummyRay( p, n );
				RayIntersectionGeometric rig( dummyRay, nullRasterizerState );
				rig.bHit          = true;
				rig.ptIntersection = p;
				rig.vNormal       = n;
				rig.ptCoord       = uv;

				IORStack iorStack( 1.0 );
				SpecularInfo specInfo = pMat->GetSpecularInfo( rig, iorStack );

				if( specInfo.isSpecular ) {
					// SMS caster seeding samples the caster's SURFACE through
					// UniformRandomPoint (uniform-on-shape mode AND snell
					// mode's pure-mirror supplemental loop), so an ACCEPTED
					// caster must honour the same sampling contract as area
					// lights: the sample structure covers the renderable
					// surface.  A geometry that withdrew the capability
					// (CanBeAreaLight() false -- an SDF whose sampling mesh
					// tessellated to nothing or PROVABLY missed renderable
					// surface) would seed only the surface it can see,
					// silently missing caustic basins -- or, in the
					// empty-mesh case, seed from the degenerate bbox-centre
					// fallback point.  Refuse it loudly: its caustics are
					// not SMS-seeded until sampling_detail resolves the
					// missing surface (2026-06-11 review P3).  The gate
					// sits AFTER specularity classification so diffuse /
					// emissive SDFs neither warn for a role they can never
					// hold nor consult the contract (review round 4).  NB
					// the kProbes UniformRandomPoint calls above already
					// build an SDF's sampling structure lazily for
					// classification -- a pre-existing one-time cost for
					// every material'd SDF in an SMS-enabled render; if it
					// ever matters, the lever is a material-level
					// "can-ever-be-specular" predicate so classification
					// can skip surface probes entirely (new IMaterial
					// virtual; deferred).
					const IGeometry* pGeom = obj.GetGeometry();
					if( pGeom && !pGeom->CanBeAreaLight() ) {
						GlobalLog()->PrintEx( eLog_Warning,
							"ManifoldSolver:: a specular caster's geometry cannot be uniformly "
							"surface-sampled (CanBeAreaLight() == false -- an infinite plane, which has "
							"no finite area density, or an SDF with an empty sampling mesh or provably "
							"missed surface).  It is refused as an SMS seeding caster: its caustics "
							"will not be SMS-seeded.  For an SDF, raise sampling_detail to restore "
							"coverage; for an infinite plane, use a bounded plane or mesh." );
						break;   // refused; don't probe further
					}
					out.push_back( &obj );
					break;   // any single-probe positive accepts; don't double-add
				}
			}
			return true;   // continue enumeration
		}
	};
}

void ManifoldSolver::EnumerateSpecularCasters(
	const IScene& scene,
	std::vector<const IObject*>& out
	)
{
	const IObjectManager* pObjMgr = scene.GetObjects();
	if( !pObjMgr ) {
		return;
	}

	SpecularCasterCollector collector( out );
	pObjMgr->EnumerateObjects( collector );
}

//////////////////////////////////////////////////////////////////////
// BuildSeedChain
//
//   Traces a ray from start toward end, following refraction at each
//   specular surface to build a seed chain that naturally discovers
//   multi-object specular paths.  At each glass surface, the ray is
//   refracted using Snell's law, allowing it to follow the physical
//   light path through interlocking glass objects rather than only
//   finding objects along the straight line.
//////////////////////////////////////////////////////////////////////

unsigned int ManifoldSolver::BuildSeedChain(
	const Point3& start,
	const Point3& end,
	const IScene& scene,
	const IRayCaster& caster,
	std::vector<ManifoldVertex>& chain,
	bool applyEmitterStop,
	const IORStack* pStartStack, ISampler* alphaSampler
	) const
{
    RandomNumberGenerator alphaRandom;
    IndependentSampler alphaFallback(alphaRandom);
    ISampler& sampler = alphaSampler ? *alphaSampler : static_cast<ISampler&>(alphaFallback);
	chain.clear();

	Vector3 dir = Vector3Ops::mkVector3( end, start );
	const Scalar totalDist = Vector3Ops::NormalizeMag( dir );

#if defined(SMS_TRACE_DIAGNOSTIC) && SMS_TRACE_DIAGNOSTIC
	static std::atomic<int> g_bsc{ 0 };
	const bool traceBSC =
		( std::fabs( start.x ) < 0.02 ) &&
		( std::fabs( start.z ) < 0.02 ) &&
		( start.y >= -0.02 && start.y <= 0.02 ) &&
		( g_bsc.fetch_add( 1, std::memory_order_relaxed ) < 5 );
	if( traceBSC ) {
		GlobalLog()->PrintEx( eLog_Event,
			"SMS_BSC: start=(%.4f,%.4f,%.4f) end=(%.4f,%.4f,%.4f) dir=(%.4f,%.4f,%.4f) totalDist=%.4f",
			start.x, start.y, start.z, end.x, end.y, end.z,
			dir.x, dir.y, dir.z, totalDist );
	}
#endif

	if( totalDist < NEARZERO )
	{
		return 0;
	}

	// Initial seed: the ray walks from the shading point toward the light
	// sample.  Snell-continue handles the per-vertex push/pop, vertex
	// creation, and ray refraction.
	//
	// DL-290: "air" is only right when the SHADING POINT is in air.  The
	// walk starts in the receiver's medium, so it starts from the
	// receiver's live IOR stack when the caller has one: the chain's
	// etaI/etaT (the Newton constraint, the chain Fresnel) then describe
	// the interfaces the light really crosses -- a glass caster under
	// water is priced glass-vs-water, not glass-vs-air.  No stack is air,
	// exactly as before (and an in-air stack's top is 1.0, so an in-air
	// render is bit-identical).
	Point3 currentOrigin = start;
	IORStack seedIor = pStartStack ? *pStartStack : IORStack( 1.0 );
	Scalar currentIOR = seedIor.top();
	if( !( currentIOR > 0 && currentIOR < RISE_INFINITY ) ) {
		currentIOR = 1.0;
	}

	const unsigned int produced = SnellContinueChain(
		currentOrigin, dir, totalDist,
		currentIOR, seedIor,
		scene, caster, chain, applyEmitterStop, &sampler );

	// TARGET BOUNCES: Mitsuba-faithful exact-length requirement.  When
	// `config.targetBounces > 0`, reject seeds whose final chain length
	// doesn't match the target.  Both snell and uniform modes go through
	// this check.
	if( config.targetBounces > 0 && chain.size() != config.targetBounces )
	{
		chain.clear();
		return 0;
	}

	return produced;
}

//////////////////////////////////////////////////////////////////////
// SnellContinueChain
//
//   Continues the seed-chain construction from a starting state
//   (currentOrigin, dir, currentIOR, seedIor) by ray-tracing forward
//   and Snell-refracting / mirror-reflecting at every specular hit.
//   Stops at the first non-specular hit, no-more-intersection, or
//   safety-distance cutoff.  See the header doc-comment for full
//   semantics.
//
//   Extracted from BuildSeedChain in Phase 3 of the Mitsuba-faithful
//   SMS port (docs/SMS_UNIFORM_SEEDING_PLAN.md): used by both the
//   legacy Snell-trace seed entry AND the uniform-on-shape Mitsuba-
//   faithful seed entry (where the first vertex is sampled and we
//   need to extend the chain through any subsequent specular hits).
//////////////////////////////////////////////////////////////////////

unsigned int ManifoldSolver::SnellContinueChain(
	Point3& currentOrigin,
	Vector3& dir,
	Scalar maxDist,
	Scalar& currentIOR,
	IORStack& seedIor,
	const IScene& scene,
	const IRayCaster& caster,
	std::vector<ManifoldVertex>& chain,
	bool applyEmitterStop, ISampler* alphaSampler
	) const
{
    RandomNumberGenerator alphaRandom;
    IndependentSampler alphaFallback(alphaRandom);
    ISampler& sampler = alphaSampler ? *alphaSampler : static_cast<ISampler&>(alphaFallback);
	(void)caster;   // reserved for future visibility queries

	const IObjectManager* pObjMgr = scene.GetObjects();
	if( !pObjMgr ) {
		return 0;
	}

	const Scalar offsetEps = 1e-2;
	const std::size_t startSize = chain.size();

	// EMITTER STOP: capture the original start position and original
	// trace direction so we can detect when a candidate specular hit
	// has put us PAST the emitter along the wall-to-light line.
	// Without this, the trace happily refracts through specular
	// surfaces past the emitter (e.g. through the FRONT of an egg
	// shell when the light is INSIDE the cavity), producing an
	// over-long seed chain whose downstream vertices have wi/wo on
	// the same side of the surface — algebraically a constraint root
	// but geometrically nonsense, and rejected by ValidateChainPhysics
	// at ~22 % of all Solve calls on the displaced Veach egg.
	// `maxDist` is the straight-line start→end distance; we project
	// each candidate hit onto the original direction and stop if the
	// projection exceeds `maxDist`.  Refractive bending makes the
	// actual path slightly longer than the straight line, so allow a
	// small margin (1.05×) before triggering the cutoff.
	const Point3  origStart = currentOrigin;
	const Vector3 origDir   = dir;
	const Scalar  emitterCutoff = maxDist * 1.05;

#if defined(SMS_TRACE_DIAGNOSTIC) && SMS_TRACE_DIAGNOSTIC
	// Trace gate: snapshot the entry origin once, mirror BuildSeedChain's
	// "shading-point near (0,0,0)" heuristic from before the refactor.
	const Point3 traceOrigin = currentOrigin;
	static std::atomic<int> g_scc{ 0 };
	const bool traceBSC =
		( std::fabs( traceOrigin.x ) < 0.02 ) &&
		( std::fabs( traceOrigin.z ) < 0.02 ) &&
		( traceOrigin.y >= -0.02 && traceOrigin.y <= 0.02 ) &&
		( g_scc.fetch_add( 1, std::memory_order_relaxed ) < 5 );
#endif

	// Medium tracking semantics: see BuildSeedChain doc-comment.  This
	// function mutates the IOR stack in place.

	for( unsigned int depth = 0; depth < config.maxChainDepth; depth++ )
	{
		// TARGET BOUNCES: Mitsuba `m_config.bounces` analogue.  Stop the
		// trace once the chain has reached the configured target length.
		// Applies in both snell and uniform modes — the post-trace check
		// in BuildSeedChain rejects any chain whose length doesn't match
		// the target, so this just bounds the work we do per Solve.
		if( config.targetBounces > 0 && chain.size() >= config.targetBounces )
		{
			break;
		}

		// Offset origin along ray direction to avoid
		// re-intersecting the surface we just left
		Point3 offsetOrigin = Point3Ops::mkPoint3(
			currentOrigin, dir * offsetEps );

		Ray ray( offsetOrigin, dir );
		RayIntersection ri( ray, nullRasterizerState );

		// Scene-wide intersection via the acceleration structure
		pObjMgr->IntersectRaySampled(ri, sampler);
        const bool retainAlphaEndpoint = caster.GetLightSampler() &&
            caster.GetLightSampler()->SceneHasAlphaCoverage();
        const std::shared_ptr<const RayIntersection> endpoint =
            retainAlphaEndpoint && ri.geometric.bHit ?
            std::make_shared<const RayIntersection>(ri) : nullptr;


		// Apply intersection modifier (e.g. bump map / normal map) so the
		// SMS chain's normals match what the rendering pipeline (RayCaster)
		// sees.  Without this, BuildSeedChain operates on the unperturbed
		// surface while PT operates on the perturbed surface, and SMS's
		// chain estimate won't align with PT's emission-suppression
		// (dimming, energy ratio collapse).  RayCaster::CastRay applies
		// the modifier at every hit (RayCaster.cpp:728-730); SMS bypasses
		// the RayCaster, so we re-apply here.
		if( ri.geometric.bHit && ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

#if defined(SMS_TRACE_DIAGNOSTIC) && SMS_TRACE_DIAGNOSTIC
		if( traceBSC ) {
			GlobalLog()->PrintEx( eLog_Event,
				"SMS_BSC:  depth=%u origin=(%.4f,%.4f,%.4f) dir=(%.4f,%.4f,%.4f) hit=%d range=%.4f  hitPos=(%.4f,%.4f,%.4f) obj=%p",
				depth, offsetOrigin.x, offsetOrigin.y, offsetOrigin.z,
				dir.x, dir.y, dir.z,
				int( ri.geometric.bHit ), ri.geometric.range,
				ri.geometric.ptIntersection.x, ri.geometric.ptIntersection.y, ri.geometric.ptIntersection.z,
				(void*)ri.pObject );
		}
#endif

		if( !ri.geometric.bHit )
		{
			break;
		}

		// Self-intersection guard: if we hit the same object extremely
		// close to where we just were, we are re-hitting the surface
		// we just left.  The threshold must be small enough to NOT
		// filter out the legitimate exit intersection from the far
		// side of the same object.
		if( ri.geometric.range < offsetEps * 2.0 && !chain.empty() &&
			ri.pObject == chain.back().pObject )
		{
			currentOrigin = Point3Ops::mkPoint3(
				ri.geometric.ptIntersection, dir * offsetEps );
			continue;
		}

		// Safety: don't trace forever
		if( ri.geometric.range > maxDist * 3.0 )
		{
			break;
		}

		// EMITTER STOP (snell-mode): if this candidate hit is past
		// the emitter along the original direction, we've over-traced.
		// In uniform mode, `end` is the sampled-on-caster point, NOT
		// the emitter — applying the cap would clip the chain to a
		// single vertex instead of letting it span the natural caustic
		// length.  Caller passes `applyEmitterStop = false` in that
		// case; behaviour falls through to the maxChainDepth + non-
		// specular-hit termination conditions below.
		if( applyEmitterStop )
		{
			const Vector3 hitOffset = Vector3Ops::mkVector3(
				ri.geometric.ptIntersection, origStart );
			const Scalar projAlongOrig = Vector3Ops::Dot( hitOffset, origDir );
			if( projAlongOrig > emitterCutoff )
			{
				break;
			}
		}

		// Check if the hit material is specular
		const IMaterial* pMat = ri.pMaterial;
		if( !pMat )
		{
			break;
		}

		SpecularInfo specInfo = pMat->GetSpecularInfo( ri.geometric, seedIor );
        const bool polishedCoat=SMSDynamicCast<PolishedMaterial>(pMat)!=nullptr;

		if( !specInfo.isSpecular )
		{
			// Hit a non-specular surface; stop tracing
			break;
		}

		// Create ManifoldVertex from this intersection.  mv.isReflection is
		// PROVISIONALLY set to !canRefract (true only for mirror-only
		// materials) and then promoted to true below if the Snell test for
		// a refractive material produces total internal reflection — the
		// geometric direction we follow then IS a reflection, and if we
		// left the flag as refraction Newton would solve the wrong
		// half-vector constraint (or reject the chain), and throughput
		// would use the wrong eta pair.
		ManifoldVertex mv;
		mv.position = ri.geometric.ptIntersection;
		mv.normal = ri.geometric.vNormal;
		mv.geomNormal = ri.geometric.UnflippedGeomNormal();	// DL-70: the TRUE outward normal (see ManifoldSolver.h)
		// Surface parameters at the hit.  Without this, vertex.uv stays at
		// its default (0, 0) — which is a parametric pole on most closed
		// surfaces.  Future SMS variants that key derivative computation
		// off (u, v) (analytical-derivative paths, photon-aided reseeding,
		// etc.) would silently see a degenerate parameterisation.  Cheap
		// and always-on; harmless for the FD-probe path which doesn't read
		// vertex.uv anyway.
		mv.uv = ri.geometric.ptCoord;
		mv.objectPosition = ri.geometric.ptObjIntersec;
		mv.pObject = ri.pObject;
		mv.pMaterial = pMat;
        mv.retainAlphaEndpoint = retainAlphaEndpoint;
        mv.alphaEndpoint = endpoint;
        mv.alphaEndpointPosition = mv.position;

		mv.eta = specInfo.ior;
		mv.attenuation = specInfo.attenuation;
		mv.attenuationNM = specInfo.attenuationNM;
		mv.attenuationAppliesToReflection = specInfo.attenuationAppliesToReflection;
		mv.hasCustomSpecularFresnel = specInfo.hasCustomSpecularFresnel;
		mv.attenuationIsInteriorTransmittance = specInfo.attenuationIsInteriorTransmittance;
		mv.isReflection = polishedCoat || !specInfo.canRefract;
		mv.canRefract = specInfo.canRefract;
		mv.valid = false;  // Derivatives not yet computed; Solve will handle it

		// Determine entering vs exiting:
		//   - If we've already pushed THIS IObject* onto the stack during
		//     a previous crossing, the ray MUST be exiting (we're currently
		//     inside this specific mesh).  Catches the thin-double-sided-
		//     mesh case where both crossings have the normal pointing the
		//     same way, so a raw cosI-sign test misses it.
		//     (DL-70: `mv.geomNormal` is now the TRUE, ray-independent
		//     outward normal -- see the invariant on the field in
		//     ManifoldSolver.h -- so the cosI test below is correct on a
		//     double-sided mesh in its own right, and this override is no
		//     longer load-bearing for that case.  DL-290: the walk now
		//     starts from the RECEIVER'S live stack when the caller has
		//     one, so the override CAN fire on the first crossing -- for
		//     a receiver whose own path pushed this object (inside a
		//     closed solid, or under an open sheet the camera path
		//     crossed); with no stack the walk starts empty and only the
		//     now-correct sign test answers.)
		//   - Else fall back to sign(dot(dir, normal)) < 0 ⇒ entering.
		//     This is the correct test for closed volumes (sphere) AND
		//     multi-object slabs-from-planes (each plane is a distinct
		//     IObject* with a distinct outward normal direction).
		seedIor.SetCurrentObject( ri.pObject );
		const bool sameObjectAgain = seedIor.containsCurrent();
		// Side-of-surface decision: use the GEOMETRIC normal so the
		// entering/exiting flag matches the real face orientation
		// regardless of bump / normal-map perturbation.  Mismatch here
		// drives etaI/etaT inversion downstream (the upstream half of
		// the bug ValidateChainPhysics catches).  Fall back to the
		// shading normal if the producer didn't populate vGeomNormal
		// (e.g. a hand-built ManifoldVertex from a probe site below).
		const Vector3& sideN = ( Vector3Ops::SquaredModulus( mv.geomNormal )
			> NEARZERO ) ? mv.geomNormal : mv.normal;
		const Scalar cosI = Vector3Ops::Dot( dir, sideN );
		// DL-345: a PROVABLY OPEN sheet is crossed by its FACE alone -- the
		// rule the transmissive SPFs apply (IORStackSeeding::
		// ResolveOpenSheetCrossing), so this walk and PT's / BDPT's / VCM's
		// walks bend at the same sheet.  A front hit while the sheet is
		// already on the stack (the walk left its back region around the
		// sheet's edge) re-enters: the stale entry is dropped first.
		const bool bOpenSheetHit = ri.geometric.bProvablyNoInterior;
		const bool bEntering = polishedCoat || (bOpenSheetHit ? ( cosI < 0 ) : ( sameObjectAgain ? false : (cosI < 0) ));
		const bool bStaleReentry = !polishedCoat && bEntering && sameObjectAgain;
		if( bStaleReentry ) {
			IORStack outer( seedIor );
			outer.pop();
			currentIOR = outer.top();
			if( !( currentIOR > 0 && currentIOR < RISE_INFINITY ) ) {
				currentIOR = 1.0;
			}
		}
		mv.isExiting = !bEntering;

		// Populate (etaI, etaT) — Walter et al. 2007 η_i / η_t for the
		// half-vector / Snell / Fresnel math downstream.  See the field
		// docs in ManifoldSolver.h for the rationale; in short, the old
		// `eta_eff = isExiting ? 1/eta : eta` formula assumed the OTHER
		// side of every interface was air (IOR=1.0), which is wrong
		// for nested dielectrics like the Veach Egg's air-cavity inside
		// glass (where the inner sphere's "other side" is glass at
		// IOR=1.5, not air).
		//
		// For ENTERING: the ray was in `currentIOR` (the surrounding
		// medium); after crossing this interface it'll be in
		// `specInfo.ior` (the object material).  η_i = surrounding,
		// η_t = object.
		//
		// For EXITING: the ray was inside the object (etaI = surface
		// material's IOR); after crossing it'll be back in whatever was
		// on the IOR stack BEFORE we entered this object.  We pop the
		// stack BELOW (in the canRefract / bEntering=false branch) and
		// READ THE NEW TOP — that's the post-pop surrounding medium.
		// We update mv.etaT after the pop using the stack's `top()`,
		// not the pre-existing hardcoded `currentIOR = 1.0` (which
		// silently assumed every exit lands in air).  For the egg
		// air-cavity exit back into the glass shell, this restores
		// etaT = 1.5 instead of 1.0.
		if( bEntering ) {
			mv.etaI = currentIOR;
			mv.etaT = specInfo.ior;
		} else {
			mv.etaI = specInfo.ior;
			mv.etaT = currentIOR;	// provisional; corrected after the pop below
		}

		const std::size_t idxJustPushed = chain.size();
		chain.push_back( mv );

		// Follow refraction/reflection to determine the next ray direction.
		if( specInfo.canRefract && !polishedCoat )
		{
			// SMS energy-loss push (DL-373): resolve the medium on the far
			// side of the interface BEFORE refracting.  The walk used to
			// refract an EXIT with `specInfo.ior / currentIOR`, and
			// `currentIOR` is the object's own index while the walk is
			// inside it, so every exit was refracted as index-matched
			// (ratio 1): the seed left a slab or sphere along its INSIDE
			// direction, missed the emitter it was aimed at and ran on
			// into the next caster (a k=2 chain seeded as k=4; Newton then
			// fails on it).  Since the first commit (9da9d6a43) -- DL-290
			// fixed which medium is on the far side, not this ratio.
			// The destination stack is resolved on a copy and committed
			// only when the refraction succeeds (TIR leaves the walk in
			// the medium it is in).  The containment probe below runs
			// along the INCIDENT direction: it asks whether the crossing
			// point is inside Y, which for a closed Y does not depend on
			// the direction, and both directions leave through this face.
			IORStack destIor = seedIor;
			Scalar destIOR;
			// The index the crossing refracts FROM.  For an entry and for a
			// pushed exit it is the walk's own medium; for an UNPUSHED exit
			// (an open sheet crossed against its normal) it is the sheet's
			// index -- what `mv.etaI` records and Newton solves with
			// (DL-290 review P3-1 / DL-345: the walk used to refract with
			// the stack's index there, so the seed and the constraint
			// described different interfaces).
			Scalar etaFrom = currentIOR;
			if( bEntering ) {
				destIOR = specInfo.ior;
				destIor.SetCurrentObject( ri.pObject );
				if( bStaleReentry ) {
					destIor.pop();
				}
				destIor.push( specInfo.ior );
			} else if( sameObjectAgain ) {
				// Only pop if we pushed earlier.  The legacy
				// slabs-from-planes pattern uses cosI-based
				// exiting without a matching stack entry.  After the
				// pop the stack's top() is the IOR of the medium the
				// walk re-enters (not 1.0 for nested dielectrics).
				destIor.SetCurrentObject( ri.pObject );
				destIor.pop();
				destIOR = destIor.top();
			} else {
				// No matching push for THIS object -- an open
				// sheet crossed against its normal.  DL-290
				// review P1-2: this used to be a hardcoded 1.0
				// ("back to air"), which priced an immersed
				// open-sheet caster against air.  Two geometries
				// reach this branch:
				//
				//  (a) slabs-from-planes: the walk (or the camera
				//      path that built the receiver stack) entered
				//      the slab through a SIBLING sheet Y, which is
				//      the stack top, and is now leaving through
				//      this one.  Leaving the slab leaves Y's entry:
				//      pop it; the far side is the medium beneath.
				//  (b) a sheet crossed while the walk is inside a
				//      medium Y that ENCLOSES the crossing (a lone
				//      sheet in a water box, or no Y at all: the
				//      root).  Nothing is popped; the far side is Y.
				//
				// Review round 2 (P1-A): the two are told apart by
				// CONTAINMENT, never by comparing indices.  The
				// round-1 rule ("pop iff Y's index equals this
				// sheet's") misread every slab of two DIFFERENT
				// indices as (b) and ended the chain inside the
				// sibling's glass, and turned an index mismatch of
				// 1e-9..1e-3 into a near-matched last vertex --
				// a continuity cliff (flatslab floor -94 % at a
				// top index of 2.2000002).  Here Y is probed along
				// the incident direction (see above): an EXIT hit on Y means
				// the walk is still inside Y after the crossing,
				// i.e. Y encloses it -- (b); a miss, or an ENTRY
				// hit, means Y was a sheet the walk has already
				// passed -- (a).  For a CLOSED, outward-wound Y the
				// first hit from inside is always an exit (concave
				// or not).  The side test uses the TRUE face
				// orientation (DL-70), so a double-sided closed
				// mesh reads correctly; a ray-derived (hair)
				// normal has no side and cannot enclose.  One ray
				// against ONE object, only on this rare branch.
				// In air with no enclosing object nothing
				// changes: the top is the root, 1.0.
				//
				// Known residuals (review round 3; the open-sheet /
				// winding convention family, DL-345, filed at
				// merge), each measured with BuildSeedChain:
				//  - Y is itself an OPEN sheet that bounds the
				//    walk's medium: a lone 1.5 sheet inside a
				//    two-sheet 2.2 slab reads [1.5 -> 1], correct
				//    [1.5 -> 2.2] -- the probe misses the slab's
				//    bottom sheet and pops it (A7-KF T6).
				//  - Y is a closed solid wound INWARD (a
				//    single-sided mesh) that the camera path
				//    pushed: the probe's exit hit reads as an
				//    entry, [2.2 -> 1][1 -> 1.33 entry] instead of
				//    [2.2 -> 1.33][1.33 -> 1] (A7-KF T5c).
				//  - Y is a closed solid with a HOLE and the walk
				//    leaves through it: the probe misses and pops,
				//    1.0 where the stack says 1.33 (ill-posed
				//    geometry, no right answer).
				//  - Two STACKED open slabs seen through their
				//    sheets: the camera path pushed all four
				//    sheets, and the walk reads the air gap between
				//    the slabs as glass ([2.2 -> 2.2] x3) in every
				//    post-DL-290 build; the pre-DL-290 walk, which
				//    started from air, got it right.  This is the
				//    receiver stack's own open-sheet convention
				//    (DL-345), not this branch.
				etaFrom = specInfo.ior;
				const IObject* pY = destIor.topObject();
				if( pY ) {
					bool yEnclosesCrossing = false;
					const Ray probe( Point3Ops::mkPoint3( ri.geometric.ptIntersection, dir * offsetEps ), dir );
					RayIntersection pri( probe, nullRasterizerState );
					pY->IntersectRay( pri, RISE_INFINITY, true, true, false );
					if( pri.geometric.bHit && pri.geometric.HasTrueGeomSide() ) {
						yEnclosesCrossing = Vector3Ops::Dot( dir, pri.geometric.UnflippedGeomNormal() ) > 0;
					}
					if( !yEnclosesCrossing ) {
						destIor.SetCurrentObject( pY );
						destIor.pop();
					}
				}
				destIOR = destIor.top();
			}
			if( !( destIOR > 0 && destIOR < RISE_INFINITY ) ) {
				destIOR = 1.0;
			}
			if( !( etaFrom > 0 && etaFrom < RISE_INFINITY ) ) {
				etaFrom = 1.0;
			}
			const Scalar etaRatio = etaFrom / destIOR;

			// Orient the normal against the incoming ray (required by the
			// Snell formula below) -- for an exit, and for the thin-sheet
			// "same object crossed again with same-side normal" scenario
			// coerced to exiting, plus any other grazing cases.
			Vector3 n = mv.normal;
			if( Vector3Ops::Dot( dir, n ) > 0 ) {
				n = n * (-1.0);
			}

			// Compute refracted direction using Snell's law
			const Scalar cosI2 = -Vector3Ops::Dot( dir, n );
			Scalar cosT;

			if( Optics::CalculateRefractedCosine( cosI2, etaRatio, 1.0, cosT ) )
			{
				// Refraction succeeds
				dir = dir * etaRatio + n * (etaRatio * cosI2 - cosT);
				dir = Vector3Ops::Normalize( dir );
				seedIor = destIor;
				currentIOR = destIOR;
				if( !bEntering ) {
					// Backfill the just-pushed vertex's etaT with the
					// post-pop surrounding-medium IOR (the provisional
					// value above was the inside-object IOR).
					chain[ idxJustPushed ].etaT = currentIOR;
				}
			}
			else
			{
				// Total internal reflection on a refractive material.  The
				// geometric direction we follow from here on IS a reflection,
				// so promote the just-pushed manifold vertex from its
				// provisional `isReflection=false` (refractive material) to
				// `isReflection=true` — otherwise Newton would solve the
				// refractive half-vector constraint at this vertex and
				// either fail or converge to a physically-wrong chain, and
				// EvaluateChainThroughput would apply the refraction (1-Fr)
				// factor instead of the reflection Fr factor.
				//
				// On TIR, etaI/etaT semantics for "reflection at this
				// interface" are still meaningful for the Fresnel call
				// below — they describe the INTERFACE the ray reflects
				// off, even though the ray doesn't transmit.  Leave
				// etaI/etaT as set above (per entering/exiting); the
				// reflection branch of EvaluateConstraint and
				// BuildJacobian doesn't read them anyway (it uses
				// h = wi + wo with no IOR weighting).  An EXIT's etaT is the
				// far medium's index, not the provisional inside index: the
				// interface's Fresnel reads it.
				chain[ idxJustPushed ].isReflection = true;
				if( !bEntering ) {
					chain[ idxJustPushed ].etaT = destIOR;
				}
				dir = dir + n * (2.0 * cosI2);
				dir = Vector3Ops::Normalize( dir );
			}
		}
		else
		{
			// Pure reflection (mirror) — no medium transition.
			const Scalar cosI_refl = -Vector3Ops::Dot( dir, mv.normal );
			dir = dir + mv.normal * (2.0 * cosI_refl);
			dir = Vector3Ops::Normalize( dir );
		}

		// Move origin past the surface along the refracted/reflected
		// direction.  We use just the new direction with the offset
		// applied at the top of the loop.
		currentOrigin = ri.geometric.ptIntersection;
	}

	return static_cast<unsigned int>( chain.size() - startSize );
}

//////////////////////////////////////////////////////////////////////
// BuildSeedChainBranching
//
//   Historical name retained as a thin wrapper around BuildSeedChain
//   after path-tree branching was excised in 2026-05.  This wrapper
//   packages the single chain into the legacy `SeedChainResult`
//   vector format with `proposalPdf = 1.0` so call sites don't
//   require restructuring.
//
//   ⚠ DELIBERATE SCOPE REDUCTION: The seed builder is DETERMINISTIC,
//   not stochastic.  At each dielectric vertex it follows refraction
//   when Snell succeeds; reflection is taken only on TIR.  This means
//   non-TIR Fresnel-reflection seed chains through dielectrics are
//   NO LONGER SEEDED by SMS.  Caustics that previously relied on the
//   Fresnel-branching seed builder (e.g. caustics inside a closed
//   dielectric shell where the entry vertex's reflection branch
//   contributes meaningful energy) are now MISSED by SMS.
//
//   What still works:
//     • Pure-refraction caustics through dielectric chains (snell
//       and uniform mode both find these).
//     • TIR caustics (BuildSeedChain follows TIR deterministically).
//     • Pure-mirror multi-bounce chains (diacaustic) — covered by
//       the pure-mirror supplemental loop in EvaluateAtShadingPoint.
//
//   Recommended for missed caustics: VCM (`vcm_pel_rasterizer` with
//   `vm_enabled true`) or photon mapping, both of which find Fresnel-
//   reflection caustics through dielectric chains via density
//   estimation rather than manifold seed construction.
//
//   This matches Mitsuba's SOTA SMS scope (Mitsuba's reference seed
//   builder is also pure-refraction; reflection caustics through
//   dielectrics are out of scope there too).
//////////////////////////////////////////////////////////////////////

unsigned int ManifoldSolver::BuildSeedChainBranching(
	const Point3& start,
	const Point3& end,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	std::vector<SeedChainResult>& out,
	bool applyEmitterStop,
	const IORStack* pStartStack
	) const
{
	(void)sampler;	// no longer needed since we don't RR-pick branches
	out.clear();

	std::vector<ManifoldVertex> chain;
	const unsigned int chainLen = BuildSeedChain(
		start, end, scene, caster, chain, applyEmitterStop, pStartStack, &sampler );
	if( chainLen > 0 && !chain.empty() ) {
		SeedChainResult sole;
		sole.chain      = std::move( chain );
		sole.proposalPdf = Scalar( 1 );
		out.push_back( std::move( sole ) );
	}

	return static_cast<unsigned int>( out.size() );
}


//////////////////////////////////////////////////////////////////////
// EvaluateChainThroughput
//
//   Computes Fresnel-weighted transmittance/reflectance product
//   along a converged specular chain, including Beer's law.
//////////////////////////////////////////////////////////////////////
// EvaluateChainGeometry
//
//   Computes the geometric coupling factor through a specular chain
//   for the path integral.  For delta BSDFs, integrating the delta
//   function cancels the outgoing cosine at each specular vertex.
//   What remains per segment is cos(θ_incoming) / dist².
//
//   For chain  x → v_1 → ... → v_k → y  the result is:
//
//     cos(θ_x) × ∏_{j=1}^{k} [cos(θ_{vj,in}) / dist(prev,vj)²]
//              × cos(θ_y) / dist(vk,y)²
//
//   The caller supplies cosAtShading (= cos θ_x) and cosAtLight
//   (= cos θ_y) separately, so this function returns the product
//   of the segment factors only:
//
//     ∏_{j=1}^{k} [cos(θ_{vj,in}) / dist(prev,vj)²]  ×  1/dist(vk,y)²
//
//   cosAtShading and cosAtLight are multiplied by the caller.
//////////////////////////////////////////////////////////////////////

Scalar ManifoldSolver::EvaluateChainGeometry(
	const Point3& startPoint,
	const Point3& endPoint,
	const std::vector<ManifoldVertex>& chain
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );
	if( k == 0 ) return 1.0;

	Scalar geom = 1.0;

	for( unsigned int i = 0; i < k; i++ )
	{
		const Point3 prevPos = (i == 0) ? startPoint : chain[i-1].position;

		Vector3 dir = Vector3Ops::mkVector3( chain[i].position, prevPos );
		const Scalar dist = Vector3Ops::Magnitude( dir );
		if( dist < 1e-8 ) return 0.0;
		dir = dir * (1.0 / dist);

		// Incoming cosine at this specular vertex.  Path-space geometry
		// term uses the GEOMETRIC normal — Veach §8.2 / PBRT 4e §13.6.4
		// (the dω→dA Jacobian depends on the actual surface element,
		// not Phong-perturbed shading).  Fall back to the chain vertex's
		// shading normal if geomNormal is the zero sentinel (legacy or
		// hand-built ManifoldVertex).
		const Vector3& sideN = ( Vector3Ops::SquaredModulus( chain[i].geomNormal ) > NEARZERO )
			? chain[i].geomNormal : chain[i].normal;
		const Scalar cosIn = fabs( Vector3Ops::Dot( sideN, dir ) );

		geom *= cosIn / (dist * dist);
	}

	// Last segment: chain[k-1] to endPoint (the light).
	// Only 1/dist² here — cosAtLight is supplied by the caller.
	{
		Vector3 dir = Vector3Ops::mkVector3( endPoint, chain[k-1].position );
		const Scalar dist = Vector3Ops::Magnitude( dir );
		if( dist < 1e-8 ) return 0.0;

		geom *= 1.0 / (dist * dist);
	}

	return geom;
}

//////////////////////////////////////////////////////////////////////
// EvaluateChainCosineProduct
//
//   Product of incoming cosines at specular vertices, WITHOUT
//   distance terms.  Used in the SMS contribution formula where
//   the 1/dist² factors are already in the Jacobian determinant.
//////////////////////////////////////////////////////////////////////

Scalar ManifoldSolver::EvaluateChainCosineProduct(
	const Point3& startPoint,
	const Point3& endPoint,
	const std::vector<ManifoldVertex>& chain
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );
	if( k == 0 ) return 1.0;

	Scalar cosProduct = 1.0;

	for( unsigned int i = 0; i < k; i++ )
	{
		const Point3 prevPos = (i == 0) ? startPoint : chain[i-1].position;

		Vector3 dir = Vector3Ops::mkVector3( chain[i].position, prevPos );
		const Scalar dist = Vector3Ops::Magnitude( dir );
		if( dist < 1e-8 ) return 0.0;
		dir = dir * (1.0 / dist);

		// Path-space cosine product: GEOMETRIC normal, matching
		// EvaluateChainGeometry above.
		const Vector3& sideN = ( Vector3Ops::SquaredModulus( chain[i].geomNormal ) > NEARZERO )
			? chain[i].geomNormal : chain[i].normal;
		cosProduct *= fabs( Vector3Ops::Dot( sideN, dir ) );
	}

	return cosProduct;
}

void ManifoldSolver::WarnHWSSLegacyMode()
{
    if(!hwssExtendedWarningEmitted.exchange(true, std::memory_order_relaxed))
        GlobalLog()->PrintEasyWarning("Extended SMS is ignored for HWSS inside a forced-legacy scope (the HWSS shader-op path, RayCaster::CastRayHWSS); legacy SMS is used there.");
}

bool ManifoldSolver::ExtendedModeActive(const IScene& scene) const
{
    if(!config.extendedMode) return false;
    const auto* objects = SMSDynamicCast<ObjectManager>(scene.GetObjects());
    return objects && objects->ExtendedSMSAllowed();
}

bool ManifoldSolver::ExtendedAnchorEligible(const IScene& scene, const IRayCaster& caster,
    const Point3& point, const IORStack& stack, Scalar nm) const
{
    if(!ExtendedModeActive(scene)) return true;
    if(!config.maxChainDepth || config.targetBounces > config.maxChainDepth
        || !std::isfinite(config.solverThreshold) || config.solverThreshold <= 0
        || !std::isfinite(config.extendedEventFloor) || config.extendedEventFloor <= 0
        || config.extendedEventFloor > 0.5 || config.photonCount || scene.GetGlobalMedium()
        || (caster.GetLightSampler() && caster.GetLightSampler()->SceneHasAlphaCoverage())) return false;
    const auto* objects = SMSDynamicCast<ObjectManager>(scene.GetObjects());
    if(!objects || objects->HasRejectedTransmissiveCaster()) return false;
    // HG eligibility is domain-dependent, unlike position independence.
    // Finite-Phong dielectric warps retain the adopted delta-limit policy.
    RayIntersectionGeometric context(Ray(point, Vector3(0,0,1)), nullRasterizerState);
    for(const IObject* object : objects->ExtendedSMSCasters()) {
        if(!SMSAuditedModifier(*object)) return false;
        const auto* dielectric = SMSDynamicCast<DielectricMaterial>(object->GetMaterial());
        if(!dielectric || !dielectric->GetHG()) continue;
        const IScalarPainter& scattering = dielectric->GetScattering();
        if(nm > 0) {
            const Scalar value = scattering.GetValueAtNM(context, nm);
            if(!std::isfinite(value) || value < 1) return false;
        } else {
            const auto values = scattering.GetValuesAt(context);
            for(unsigned c=0; c<3; ++c)
                if(!std::isfinite(values[c]) || values[c] < 1) return false;
        }
    }
    SMSStartingMedia media;
    if(!SMSDomainReplay::Capture(scene, point, stack, media)) return false;
    const unsigned int components = nm > 0 ? 1 : 3;
    for(unsigned int c=0; c<components; ++c) {
        const SMSQueryDomain domain = nm > 0 ? SMSQueryDomain::NM(nm) : SMSQueryDomain::RGB(c);
        IORStack evaluated(stack.EnvironmentIOR());
        if(!SMSDomainReplay::BuildStack(media, domain, evaluated)) return false;
    }
    return true;
}

void ManifoldSolver::ExtendedAnchorEligibleNM(const IScene& scene, const IRayCaster& caster,
    const Point3& point, const IORStack& stack, const Scalar* nm, const bool* evaluate,
    unsigned int count, bool* eligible) const
{
    // Same predicate as ExtendedAnchorEligible per lane: the early returns
    // below are wavelength-independent, so they decide every lane alike.
    for(unsigned int i=0; i<count; ++i) eligible[i] = false;
    if(!ExtendedModeActive(scene)) {
        for(unsigned int i=0; i<count; ++i) eligible[i] = evaluate[i];
        return;
    }
    bool any = false;
    for(unsigned int i=0; i<count; ++i) {
        eligible[i] = evaluate[i] && std::isfinite(nm[i]) && nm[i] > 0;
        any = any || eligible[i];
    }
    const auto none = [&]() { for(unsigned int i=0; i<count; ++i) eligible[i] = false; };
    if(!any) return;
    if(!config.maxChainDepth || config.targetBounces > config.maxChainDepth
        || !std::isfinite(config.solverThreshold) || config.solverThreshold <= 0
        || !std::isfinite(config.extendedEventFloor) || config.extendedEventFloor <= 0
        || config.extendedEventFloor > 0.5 || config.photonCount || scene.GetGlobalMedium()
        || (caster.GetLightSampler() && caster.GetLightSampler()->SceneHasAlphaCoverage())) { none(); return; }
    const auto* objects = SMSDynamicCast<ObjectManager>(scene.GetObjects());
    if(!objects || objects->HasRejectedTransmissiveCaster()) { none(); return; }
    RayIntersectionGeometric context(Ray(point, Vector3(0,0,1)), nullRasterizerState);
    for(const IObject* object : objects->ExtendedSMSCasters()) {
        if(!SMSAuditedModifier(*object)) { none(); return; }
        const auto* dielectric = SMSDynamicCast<DielectricMaterial>(object->GetMaterial());
        if(!dielectric || !dielectric->GetHG()) continue;
        const IScalarPainter& scattering = dielectric->GetScattering();
        for(unsigned int i=0; i<count; ++i) {
            if(!eligible[i]) continue;
            const Scalar value = scattering.GetValueAtNM(context, nm[i]);
            if(!std::isfinite(value) || value < 1) eligible[i] = false;
        }
    }
    SMSStartingMedia media;
    if(!SMSDomainReplay::Capture(scene, point, stack, media)) { none(); return; }
    for(unsigned int i=0; i<count; ++i) {
        if(!eligible[i]) continue;
        IORStack evaluated(stack.EnvironmentIOR());
        if(!SMSDomainReplay::BuildStack(media, SMSQueryDomain::NM(nm[i]), evaluated)) eligible[i] = false;
    }
}

//////////////////////////////////////////////////////////////////////

Scalar ManifoldSolver::EvaluateVertexFresnel( const ManifoldVertex& v,
	Scalar cosI, Scalar etaI, Scalar etaT, Scalar nm )
{
	if( v.hasCustomSpecularFresnel && v.pMaterial ) {
		const ISPF* spf = v.pMaterial->GetSPF();
		Scalar reflectance;
		if( spf && spf->EvaluateSpecularFresnel( cosI, etaI, etaT, v.isExiting, nm, reflectance ) )
			return reflectance;
	}
	return ComputeDielectricFresnel( cosI, etaI, etaT );
}

RISEPel ManifoldSolver::EvaluateVertexFresnelRGB( const ManifoldVertex& v,
	Scalar cosI, Scalar etaI, Scalar etaT )
{
	if( !v.hasCustomSpecularFresnel )
		return RISEPel( ComputeDielectricFresnel( cosI, etaI, etaT ) );
	return RISEPel(
		EvaluateVertexFresnel( v, cosI, etaI, etaT, ScalarPainterRGB::kChannelNM[0] ),
		EvaluateVertexFresnel( v, cosI, etaI, etaT, ScalarPainterRGB::kChannelNM[1] ),
		EvaluateVertexFresnel( v, cosI, etaI, etaT, ScalarPainterRGB::kChannelNM[2] ) );
}

namespace {
    // A polished coat has a delta reflection but no delta transmission.
    // Its SPF prices both mesh windings from the surrounding medium, rather
    // than interpreting a back-facing sheet as a glass-to-air crossing.
    bool LegacyPolishedKray(const ManifoldVertex& vertex,const Vector3& wi,
        Scalar etaI,Scalar etaT,Scalar nm,Scalar kray[3]) {
        if(!vertex.pMaterial || !SMSDynamicCast<PolishedMaterial>(vertex.pMaterial)) return false;
        kray[0]=kray[1]=kray[2]=0;
        if(!vertex.isReflection) return true;
        const auto* brdf=SMSDynamicCast<PolishedBRDF>(vertex.pMaterial->GetBSDF());
        if(!brdf) return false;
        RayIntersectionGeometric hit(Ray(vertex.position,-wi),nullRasterizerState);
        hit.bHit=true;hit.ptIntersection=vertex.position;hit.ptObjIntersec=vertex.objectPosition;
        hit.ptCoord=vertex.uv;hit.vNormal=vertex.normal;
        // The stored shading normal may be ray-oriented independently of the
        // winding; expose it as the geometric frame too, matching the SPF's
        // oriented incident frame for this ideal reflection.
        hit.vGeomNormal=vertex.normal;hit.onb.CreateFromW(vertex.normal);
        PolishedLobes lobes;
        brdf->Resolve(hit,vertex.isExiting?etaT:etaI,nm,lobes);
        PolishedBRDF::DeltaKray(lobes,kray);
        return true;
    }
}

RISEPel ManifoldSolver::EvaluateChainThroughput(
	const Point3& startPoint,
	const Point3& endPoint,
	const std::vector<ManifoldVertex>& chain
	) const
{
	RISEPel throughput( 1.0, 1.0, 1.0 );
	const unsigned int k = static_cast<unsigned int>( chain.size() );

	if( k == 0 )
	{
		return throughput;
	}

	for( unsigned int i = 0; i < k; i++ )
	{
		const ManifoldVertex& v = chain[i];

		// Compute incoming direction at this vertex
		const Point3 prevPos = (i == 0) ? startPoint : chain[i-1].position;

		Vector3 wi = Vector3Ops::mkVector3( prevPos, v.position );
		wi = Vector3Ops::Normalize( wi );

		// Exact dielectric Fresnel reflectance.  Use the chain-topological
		// flag `v.isExiting` (set by BuildSeedChain's IOR-stack bookkeeping)
		// to select eta_i / eta_t, NOT a local sign(dot(wi, n)) test.  For
		// a thin double-sided mesh the normal can point the same way at
		// entry and exit, so the sign-of-cosI test reports both crossings
		// as "entering" and the Fresnel factor comes out wrong.
		const Scalar cosI = fabs( Vector3Ops::Dot( wi, v.normal ) );
		// Convention: eta_i is the IOR on the prev (x-receiver) side,
		// eta_t on the next (y-source) side, for the photon's FORWARD
		// direction y -> v -> x.  In the chain-build (reverse) direction:
		//   v.isExiting = false ⇒ chain-ray enters glass at v ⇒
		//     prev side is outside (air)   → eta_i = 1
		//     next side is inside (glass)  → eta_t = v.eta
		//   v.isExiting = true  ⇒ chain-ray exits glass at v ⇒
		//     prev side is inside (glass)  → eta_i = v.eta
		//     next side is outside (air)   → eta_t = 1
		// (This matches the old sign(dot(wi, n)) convention where
		// cosI_signed >= 0 meant "wi points to air side".)
		// Pull (η_i, η_t) from the vertex's BuildSeedChain-populated
		// fields, with back-compat fallback to "air on the other side"
		// for hand-constructed test chains.  The OLD code had:
		//   const Scalar eta_i = v.isExiting ? v.eta : 1.0;
		//   const Scalar eta_t = v.isExiting ? 1.0 : v.eta;
		// which silently assumed every interface had air on the
		// non-material side — wrong for nested dielectrics (Veach Egg
		// air_cavity inside glass).  Fresnel reflectance at a
		// glass→air interface (Fr ≈ 0.04 for normal incidence) differs
		// substantially from a glass→glass interface (Fr = 0 if same
		// IOR, monotonic in |Δη| otherwise), so the bug shows up as
		// caustic energy that's the wrong intensity even when the
		// chain itself converged.
		Scalar eta_i, eta_t;
		GetEffectiveEtas( v, eta_i, eta_t );
        Scalar polishedKray[3];
        if(LegacyPolishedKray(v,wi,eta_i,eta_t,Scalar(-1),polishedKray)) {
            throughput=throughput*RISEPel(polishedKray[0],polishedKray[1],polishedKray[2]);
            continue;
        }


		// Mirrors use their painter reflectance without a dielectric factor.
		// Refracting interfaces use Fresnel (the native SPF's coating law when
		// advertised). PerfectRefractor tint applies to transmission only;
		// generic boundary multipliers may apply to both events. Dielectric
		// per-unit tau follows its SPF's exiting-transmission convention.
		RISEPel attenuation = v.attenuation;
		if( v.isReflection && !v.attenuationAppliesToReflection )
			attenuation = RISEPel(1,1,1);
		if( v.attenuationIsInteriorTransmittance ) {
			attenuation = RISEPel( 1, 1, 1 );
			if( v.isExiting && !v.isReflection ) {
				const Scalar distance = Point3Ops::Distance( prevPos, v.position );
				for( unsigned int c = 0; c < 3; ++c )
					attenuation[c] = v.attenuation[c] == 1 ? Scalar(1)
						: std::pow( r_max( Scalar(0), v.attenuation[c] ), distance );
			}
		}

		if( v.isReflection )
		{
			RISEPel R;
			if( v.canRefract )
			{
				R = EvaluateVertexFresnelRGB( v, cosI, eta_i, eta_t );
			}
			else
			{
				R = RISEPel(1,1,1);
			}
			throughput = throughput * attenuation * R;
		}
		else
		{
			// Radiance transport across a refracting interface:
			//   L_receiver = T * (n_receiver / n_source)^2 * L_source
			//
			// This (n_r/n_s)^2 factor is the standard radiance rescaling
			// across a dielectric boundary (radiance is NOT preserved;
			// L/n^2 is).
			//
			// UPDATED 2026-09-12 (debt 30).  This used to read "RISE's
			// PerfectRefractorSPF omits this factor in the forward path
			// tracer; including it here only in SMS gives SMS physically
			// correct radiance while leaving PT/VCM's 'all air' convention
			// intact."  That is no longer true and the asymmetry it
			// described is gone: every radiance-mode walk now applies the
			// factor at its kray consumer via RISE::RadianceEtaScale, and
			// this SMS site already used the SAME convention (eta_i on the
			// receiver side / eta_t on the source side is exactly
			// eta_before / eta_after for a walk running eye -> light), so
			// SMS was ahead of the rest of the tree rather than special.
			//
			// NOT A DOUBLE COUNT.  This loop prices the specular chain
			// between the shading point and the light; the PT walk that
			// delivered the eye ray TO the shading point prices its own
			// crossings.  The two cover disjoint segments.
			//
			// eta_i is the index on the x-receiver side, eta_t on the
			// y-source side, in the photon's FORWARD direction.  The
			// forward rescale is (n_receiver / n_source)^2 = (eta_i / eta_t)^2.
			const RISEPel fr = EvaluateVertexFresnelRGB( v, cosI, eta_i, eta_t );
			const Scalar eta_ratio = eta_i / eta_t;
			const Scalar radiance_rescale = eta_ratio * eta_ratio;
			throughput = throughput * attenuation * (RISEPel(1,1,1) - fr) * radiance_rescale;
		}
	}

	return throughput;
}

//////////////////////////////////////////////////////////////////////
// EvaluateChainThroughputNM
//
//   Scalar (spectral) variant of EvaluateChainThroughput.
//////////////////////////////////////////////////////////////////////

Scalar ManifoldSolver::EvaluateChainThroughputNM(
	const Point3& startPoint,
	const Point3& endPoint,
	const std::vector<ManifoldVertex>& chain,
	const Scalar nm
	) const
{
	Scalar throughput = 1.0;
	const unsigned int k = static_cast<unsigned int>( chain.size() );

	if( k == 0 )
	{
		return throughput;
	}

	for( unsigned int i = 0; i < k; i++ )
	{
		const ManifoldVertex& v = chain[i];

		const Point3 prevPos = (i == 0) ? startPoint : chain[i-1].position;
		Vector3 wi = Vector3Ops::mkVector3( prevPos, v.position );
		wi = Vector3Ops::Normalize( wi );

		// Exact dielectric Fresnel reflectance — use the chain-topological
		// flag (see EvaluateChainThroughput RGB variant for full comment).
		// Pull (η_i, η_t) from the per-vertex fields populated by
		// spectral seed/replay queries — same air-on-other-side fix as the
		// RGB variant.
		const Scalar cosI = fabs( Vector3Ops::Dot( wi, v.normal ) );
		Scalar eta_i, eta_t;
		GetEffectiveEtas( v, eta_i, eta_t );
        Scalar polishedKray[3];
        if(LegacyPolishedKray(v,wi,eta_i,eta_t,nm,polishedKray)) {
            throughput*=polishedKray[0];
            continue;
        }


		// Same three-case dispatch as the RGB variant: pure mirrors take
		// full reflectance, dielectric reflection (incl. TIR) and
		// refraction use Fresnel.  See EvaluateChainThroughput for the
		// full discussion of why ComputeDielectricFresnel(cosI, 1, 1)
		// would silently zero the throughput on a mirror.
		// Cached scalar is queried at this wavelength by the seed/replay
		// path, never selected from an RGB channel. No painter query or
		// allocation occurs in this throughput loop.
		Scalar attenuation = v.isReflection && !v.attenuationAppliesToReflection ? Scalar(1) : v.attenuationNM;
		if( v.attenuationIsInteriorTransmittance ) {
			attenuation = v.isExiting && !v.isReflection
				? ( attenuation == 1 ? Scalar(1)
					: std::pow( attenuation, Point3Ops::Distance( prevPos, v.position ) ) )
				: Scalar(1);
		}
		if( v.isReflection )
		{
			if( v.canRefract )
			{
				throughput *= attenuation * EvaluateVertexFresnel( v, cosI, eta_i, eta_t, nm );
			}
			else {
				throughput *= attenuation;
			}
		}
		else
		{
			const Scalar fr = EvaluateVertexFresnel( v, cosI, eta_i, eta_t, nm );
			const Scalar eta_ratio = eta_i / eta_t;
			throughput *= attenuation * (1.0 - fr) * eta_ratio * eta_ratio;
		}
	}

	return throughput;
}

//////////////////////////////////////////////////////////////////////
// ComputeManifoldGeometricTerm
//
//   Computes the generalized geometric term (Jacobian determinant
//   ratio) for MIS weighting.
//////////////////////////////////////////////////////////////////////

Scalar ManifoldSolver::ComputeManifoldGeometricTerm(
	const std::vector<ManifoldVertex>& chain,
	const Point3& fixedStart,
	const Point3& fixedEnd
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );

	if( k == 0 )
	{
		return 1.0;
	}

	// Build Jacobian and compute its determinant
	std::vector<Scalar> diag, upper_blocks, lower_blocks;
	BuildJacobian( chain, fixedStart, fixedEnd, diag, upper_blocks, lower_blocks );

	const Scalar detProduct = ComputeBlockTridiagonalDeterminant(
		diag, upper_blocks, lower_blocks, k );

	// Geometric distances
	Scalar distProduct = 1.0;
	for( unsigned int i = 0; i < k; i++ )
	{
		const Point3 prevPos = (i == 0) ? fixedStart : chain[i-1].position;
		const Point3 nextPos = (i == k-1) ? fixedEnd : chain[i+1].position;

		const Scalar d1 = Point3Ops::Distance( prevPos, chain[i].position );
		const Scalar d2 = Point3Ops::Distance( chain[i].position, nextPos );

		if( d1 > NEARZERO && d2 > NEARZERO )
		{
			distProduct *= d1 * d1 * d2 * d2;
		}
	}

	// Apply surface metric correction: convert from parameter-space
	// Jacobian to tangent-plane world-space Jacobian
	Scalar metricProduct = 1.0;
	for( unsigned int i = 0; i < k; i++ )
	{
		const Scalar dpduLen = Vector3Ops::Magnitude( chain[i].dpdu );
		const Scalar dpdvLen = Vector3Ops::Magnitude( chain[i].dpdv );
		if( dpduLen > NEARZERO && dpdvLen > NEARZERO )
		{
			metricProduct *= dpduLen * dpdvLen;
		}
	}

	Scalar geoTerm = fabs( detProduct ) / fmax( metricProduct, 1e-20 );
	if( distProduct > NEARZERO )
	{
		geoTerm /= distProduct;
	}

	// Clamp
	if( geoTerm > config.maxGeometricTerm )
	{
		geoTerm = config.maxGeometricTerm;
	}

	return geoTerm;
}

//////////////////////////////////////////////////////////////////////
// ComputeLastBlockLightJacobian
//
//   Computes the 2x2 block ∂C_at_vk / ∂y_tangent.  Mirrors the half-
//   vector setup in BuildJacobian, but differentiates with respect to
//   the LIGHT endpoint y instead of the specular vertex position.
//
//   Only wo (direction from v_k to y) depends on y; wi does not.
//   So ∂h_raw/∂y = (reflection ? ∂wo/∂y : -eta_eff * ∂wo/∂y).
//
//   Result is used with ComputeLightToFirstVertexJacobianDet to
//   propagate y-perturbations through the chain via block-tridiagonal
//   solve.
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::ComputeLastBlockLightJacobian(
	const ManifoldVertex& vk,
	const Point3& prevPos,
	const Point3& lightPos,
	const Vector3& lightNormal,
	Scalar Jy[4]
	) const
{
	Jy[0] = Jy[1] = Jy[2] = Jy[3] = 0;

	// wi: direction from vk toward prev (same sign convention as BuildJacobian)
	Vector3 d_wi = Vector3Ops::mkVector3( prevPos, vk.position );
	const Scalar dist_i = Vector3Ops::NormalizeMag( d_wi );
	const Vector3 wi = d_wi;

	// wo: direction from vk toward next (= y).  When y moves by δy,
	// new d_wo = lightPos + δy - vk.position.  So ∂wo/∂y applies
	// (I - wo⊗wo)/dist_o to δy (in world-space coordinates).
	Vector3 d_wo = Vector3Ops::mkVector3( lightPos, vk.position );
	const Scalar dist_o = Vector3Ops::NormalizeMag( d_wo );
	const Vector3 wo = d_wo;

	if( dist_i < NEARZERO || dist_o < NEARZERO ) return;

	// Tangent frame at vk (assumes OrthonormalizeTangentFrame has been
	// called upstream, so these are unit orthogonal).
	const Vector3 s_v = Vector3Ops::Normalize( vk.dpdu );
	const Vector3 t_v = Vector3Ops::Normalize( vk.dpdv );

	// Walter form half-vector (η_i, η_t) — same convention and back-
	// compat fallback as BuildJacobian / EvaluateConstraint.
	Scalar eta_i_v = Scalar( 1.0 );
	Scalar eta_t_v = Scalar( 1.0 );
	if( !vk.isReflection ) {
		GetEffectiveEtas( vk, eta_i_v, eta_t_v );
	}

	// Half-vector (sign follows BuildJacobian)
	Vector3 h_raw;
	if( vk.isReflection ) {
		h_raw = Vector3( wi.x + wo.x, wi.y + wo.y, wi.z + wo.z );
	} else {
		h_raw = Vector3(
			-(eta_i_v * wi.x + eta_t_v * wo.x),
			-(eta_i_v * wi.y + eta_t_v * wo.y),
			-(eta_i_v * wi.z + eta_t_v * wo.z) );
	}
	// DL-290: a refraction vertex's constraint is the UNNORMALIZED h
	// (see UseUnnormalizedHalfVector).
	const bool rawHalfVector = UseUnnormalizedHalfVector( vk );
	const Scalar h_len = Vector3Ops::Magnitude( h_raw );
	if( !rawHalfVector && h_len < NEARZERO ) return;
	const Vector3 h = rawHalfVector ? h_raw : h_raw * (1.0 / h_len);

	// Tangent basis at y (orthonormal, perpendicular to lightNormal)
	Vector3 y_s = Vector3Ops::Perpendicular( lightNormal );
	y_s = Vector3Ops::Normalize( y_s );
	Vector3 y_t = Vector3Ops::Cross( lightNormal, y_s );
	y_t = Vector3Ops::Normalize( y_t );

	const Scalar inv_lo = 1.0 / dist_o;

	// For each y tangent direction, compute ∂C/∂(that direction)
	for( int j = 0; j < 2; j++ ) {
		const Vector3& ydir = (j == 0) ? y_s : y_t;

		// ∂wo/∂y_dir = (I - wo⊗wo) * ydir / dist_o
		const Scalar wo_dot_ydir = Vector3Ops::Dot( wo, ydir );
		const Vector3 dwo(
			(ydir.x - wo.x * wo_dot_ydir) * inv_lo,
			(ydir.y - wo.y * wo_dot_ydir) * inv_lo,
			(ydir.z - wo.z * wo_dot_ydir) * inv_lo );

		// ∂h_raw/∂y.  wi is independent of y (depends only on prev and vk).
		// Walter form: ∂h/∂y = -η_t ∂wo/∂y (only wo depends on y).
		Vector3 dh_raw;
		if( vk.isReflection ) {
			dh_raw = dwo;
		} else {
			dh_raw = Vector3( -eta_t_v * dwo.x, -eta_t_v * dwo.y, -eta_t_v * dwo.z );
		}

		// ∂h/∂y = (dh_raw - h * dot(h, dh_raw)) / h_len at a reflection
		// vertex; the raw derivative itself at a refraction vertex, whose
		// constraint is the unnormalized h (UseUnnormalizedHalfVector).
		const Scalar h_dot = rawHalfVector ? Scalar( 0 ) : Vector3Ops::Dot( h, dh_raw );
		const Scalar inv_h = rawHalfVector ? Scalar( 1 ) : Scalar( 1.0 ) / h_len;
		const Vector3 dh(
			(dh_raw.x - h.x * h_dot) * inv_h,
			(dh_raw.y - h.y * h_dot) * inv_h,
			(dh_raw.z - h.z * h_dot) * inv_h );

		// Project onto (s_v, t_v) basis at vk.  Row index = {s_v, t_v}.
		const Scalar s_dot = Vector3Ops::Dot( s_v, dh );
		const Scalar t_dot = Vector3Ops::Dot( t_v, dh );
		Jy[ 0 * 2 + j ] = s_dot;  // ∂Cs/∂y_j
		Jy[ 1 * 2 + j ] = t_dot;  // ∂Ct/∂y_j
	}
}

//////////////////////////////////////////////////////////////////////
// JacobianLightNormal (DL-413)
//
//   The normal of the light-endpoint tangent plane the chain Jacobian
//   perturbs y in.  For an AREA light it is the emitter's surface normal:
//   G_x_v1 * |det dv1/dy| is then dw_x / dA_y in the light's own area
//   measure, which is what its area pdf divides.  A DELTA (point) light has
//   no area measure: the estimator is f * I(w) * cos_x * dw_x/dw_y-style,
//   i.e. a vanishing emitter of radiance I/A facing the chain, whose area
//   element is perpendicular to the direction it emits along.  So the
//   plane must be perpendicular to the last segment (v_k -> y): then the
//   (I - wo wo) projection in ComputeLastBlockLightJacobian is the
//   identity on it and no cosine at y enters, exactly as a point light's
//   NEE carries none.  LightSampler::SampleLight stores the light's
//   random photon direction in `normal` for a delta light, a tangent
//   plane at a random angle to the true one, which weighted every SMS
//   sample by |cos| of that angle -- 1/2 on average for an omni light
//   (row E1 of TransparentShadowPartitionTest read 0.4994).
//////////////////////////////////////////////////////////////////////

static Vector3 JacobianLightNormal(
	const LightSample& lightSample,
	const std::vector<ManifoldVertex>& chain
	)
{
	if( !lightSample.isDelta || chain.empty() ) {
		return lightSample.normal;
	}
	Vector3 d = Vector3Ops::mkVector3( lightSample.position, chain.back().position );
	const Scalar len = Vector3Ops::NormalizeMag( d );
	return len > NEARZERO ? d : lightSample.normal;
}

//////////////////////////////////////////////////////////////////////
// ComputeLightToFirstVertexJacobianDet
//
//   Implicit-function-theorem application: the chain constraint
//   C(v_1, ..., v_k, y) = 0 defines v as a function of y.  This
//   returns |det(δv_1_⊥ / δy_⊥)|.
//
//   For k=1 this degenerates to |det(∂C/∂y)| / |det(∂C/∂v_1)|.
//   For k>1 we must solve the block-tridiagonal system
//     J_v * δv = -J_y * δy
//   where J_y has only its last block nonzero.  We solve twice
//   (one per y tangent direction) to recover the 2x2 matrix δv_1/δy.
//////////////////////////////////////////////////////////////////////

Scalar ManifoldSolver::ComputeLightToFirstVertexJacobianDet(
	const std::vector<ManifoldVertex>& chain,
	const Point3& shadingPoint,
	const Point3& lightPos,
	const Vector3& lightNormal
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );
	if( k == 0 ) return 0;

	// Build full chain Jacobian J_v (block-tridiagonal).
    SMSWorkerScratchLease scratch(nativeEventConstraints,config.referenceCounters);
    const auto capacity=std::max<unsigned>(config.maxChainDepth,k);
    std::vector<Scalar> localDiag,localUpper,localLower;
    auto& diag=scratch.Enabled()?scratch.Scalars(0,4*capacity):localDiag;
    auto& upper=scratch.Enabled()?scratch.Scalars(1,4*capacity):localUpper;
    auto& lower=scratch.Enabled()?scratch.Scalars(2,4*capacity):localLower;
	BuildJacobian( chain, shadingPoint, lightPos, diag, upper, lower );

	// Build the light-side Jacobian block at v_k.
	const Point3 prevPos = (k == 1)
		? shadingPoint
		: chain[k-2].position;
	Scalar Jy[4];
    if(nativeEventConstraints && std::any_of(chain.begin(),chain.end(),SMSNeedsNativeFrame)) {
        OrthonormalBasis3D frame;frame.CreateFromW(lightNormal);
        const Scalar h=std::cbrt(std::numeric_limits<Scalar>::epsilon())*Point3Ops::Distance(shadingPoint,lightPos);
        for(unsigned column=0;column<2;++column) {
            const Vector3 tangent=column?frame.v():frame.u();
            if(!SMSNativeConstraintDifferential(chain[k-1],chain[k-1],chain[k-1],
                prevPos,prevPos,prevPos,lightPos,Point3Ops::mkPoint3(lightPos,tangent*h),
                Point3Ops::mkPoint3(lightPos,-tangent*h),h,NativeRaster(k-1),Jy[column],Jy[2+column],NativeSceneObjects(k-1),nativeWavelength)) return 0;
        }
    } else {
        ComputeLastBlockLightJacobian(chain[k-1],prevPos,lightPos,lightNormal,Jy);
    }

	// Solve J_v * δv = -RHS for each of two y-tangent directions.
	// RHS is zero except for the last 2 entries = Jy column j.
	Scalar dv1[4];  // row-major: [δv1_s for δy_s, δv1_s for δy_t; δv1_t for δy_s, δv1_t for δy_t]

	for( unsigned int j = 0; j < 2; j++ ) {
        std::vector<Scalar> localRhs;
        auto& rhs=scratch.Enabled()?scratch.Scalars(3,2*capacity):localRhs;rhs.assign(2*k,0);
		rhs[ 2 * (k - 1) + 0 ] = -Jy[ 0 * 2 + j ];
		rhs[ 2 * (k - 1) + 1 ] = -Jy[ 1 * 2 + j ];

        std::vector<Scalar> localDelta;
        auto& delta=scratch.Enabled()?scratch.Scalars(4,2*capacity):localDelta;
        std::vector<Scalar> localCopy;
        auto& diag_copy=scratch.Enabled()?scratch.Scalars(5,4*capacity):localCopy;diag_copy=diag;  // SolveBlockTridiagonal takes non-const ref
		if( !SolveBlockTridiagonal( diag_copy, upper, lower, rhs, k, delta ) ) {
			return 0;
		}

		dv1[ 0 * 2 + j ] = delta[0];
		dv1[ 1 * 2 + j ] = delta[1];
	}

	// 2x2 determinant
	const Scalar det = dv1[0] * dv1[3] - dv1[1] * dv1[2];
	return fabs( det );
}

//////////////////////////////////////////////////////////////////////
// EstimatePDF
//
//   Bernoulli trial estimator for unbiased PDF estimation.
//////////////////////////////////////////////////////////////////////

Scalar ManifoldSolver::EstimatePDF(
	const ManifoldResult& solution,
	const Point3& shadingPoint,
	const Point3& emitterPoint,
	const std::vector<ManifoldVertex>& seedTemplate,
	ISampler& sampler
	) const
{
	if( !solution.valid )
	{
		return 1.0;
	}

	unsigned int count = 0;
	unsigned int trials = 0;
	const unsigned int targetCount = 1;
	const unsigned int k = static_cast<unsigned int>( solution.specularChain.size() );

	while( count < targetCount && trials < config.maxBernoulliTrials )
	{
		// Generate a random seed chain by perturbing the template
		std::vector<ManifoldVertex> testChain( seedTemplate );

		for( unsigned int i = 0; i < testChain.size(); i++ )
		{
			// Random perturbation in tangent plane
			const Scalar ru = sampler.Get1D() * 0.2 - 0.1;
			const Scalar rv = sampler.Get1D() * 0.2 - 0.1;

			testChain[i].position = Point3Ops::mkPoint3(
				testChain[i].position,
				testChain[i].dpdu * ru + testChain[i].dpdv * rv
				);

			// Re-snap to surface
			UpdateVertexOnSurface( testChain[i], 0.0, 0.0 );
		}

		// Run Newton solve on this perturbed chain
		if( NewtonSolve( testChain, shadingPoint, emitterPoint ) )
		{
			// Check if this converged to the same solution
			bool same = true;
			for( unsigned int i = 0; i < k && i < testChain.size(); i++ )
			{
				const Scalar dist = Point3Ops::Distance(
					testChain[i].position,
					solution.specularChain[i].position
					);
				if( dist > config.uniquenessThreshold )
				{
					same = false;
					break;
				}
			}

			if( same )
			{
				count++;
			}
		}

		trials++;
	}

	if( count == 0 )
	{
		return 1.0;  // Fallback
	}

	return static_cast<Scalar>(trials) / static_cast<Scalar>(count);
}

//////////////////////////////////////////////////////////////////////
// Solve (main entry point)
//////////////////////////////////////////////////////////////////////

ManifoldResult ManifoldSolver::Solve(
	const Point3& shadingPoint,
	const Vector3& shadingNormal,
	const Point3& emitterPoint,
	const Vector3& emitterNormal,
	std::vector<ManifoldVertex>& specularChain,
	ISampler& sampler
	) const
{
    return SolveCore(shadingPoint, shadingNormal, emitterPoint, emitterNormal, specularChain, sampler, true);
}

ManifoldResult ManifoldSolver::SolveCore(const Point3& shadingPoint, const Vector3& shadingNormal,
    const Point3& emitterPoint, const Vector3& emitterNormal, std::vector<ManifoldVertex>& specularChain,
    ISampler& sampler, bool estimateLegacyPDF, Scalar convergenceThreshold) const
{
    ManifoldResult result;
    SolveCoreInto(shadingPoint,shadingNormal,emitterPoint,emitterNormal,specularChain,
        sampler,estimateLegacyPDF,convergenceThreshold,result);
    return result;
}

void ManifoldSolver::SolveCoreInto(const Point3& shadingPoint, const Vector3& shadingNormal,
    const Point3& emitterPoint, const Vector3& emitterNormal, std::vector<ManifoldVertex>& specularChain,
    ISampler& sampler, bool estimateLegacyPDF, Scalar convergenceThreshold,ManifoldResult& result) const
{
    SMSResetResult(result);
    SMSWorkerScratchLease scratch(nativeEventConstraints,config.referenceCounters);

#if SMS_TRACE_DIAGNOSTIC
	// Ungated global counters: every Solve call increments one of these
	// buckets so we can see the failure breakdown across the whole image
	// (the traceSolve-gated verbose logging only looks at one patch).
	static std::atomic<int> g_solveTotal{ 0 };
	static std::atomic<int> g_solveEmpty{ 0 };
	static std::atomic<int> g_solveSeedTooFar{ 0 };
	static std::atomic<int> g_solveDerivFail{ 0 };
	static std::atomic<int> g_solveNewtonFail{ 0 };
	static std::atomic<int> g_solvePhysicsFail{ 0 };
	static std::atomic<int> g_solveShortSeg{ 0 };
	static std::atomic<int> g_solveOk{ 0 };
	const int st = g_solveTotal.fetch_add( 1, std::memory_order_relaxed );
	// Periodic print (every 200k solves) so we see the rate without flooding
	if( (st & 0x3ffff) == 0 && st > 0 ) {
		GlobalLog()->PrintEx( eLog_Event,
			"SMS_SOLVE_STATS: total=%d empty=%d seedTooFar=%d derivFail=%d newtonFail=%d physicsFail=%d shortSeg=%d ok=%d",
			st,
			g_solveEmpty.load(), g_solveSeedTooFar.load(),
			g_solveDerivFail.load(), g_solveNewtonFail.load(),
			g_solvePhysicsFail.load(), g_solveShortSeg.load(),
			g_solveOk.load() );
	}

	static std::atomic<int> g_solveTraceCount{ 0 };
	const bool traceSolve =
		( std::fabs( shadingPoint.x ) < 0.02 ) &&
		( std::fabs( shadingPoint.z ) < 0.02 ) &&
		( shadingPoint.y >= -0.02 && shadingPoint.y <= 0.02 ) &&
		( g_solveTraceCount.fetch_add( 1, std::memory_order_relaxed ) < 10 );
	if( traceSolve ) {
		GlobalLog()->PrintEx( eLog_Event,
			"SMS_SOLVE: enter k=%zu  shading=(%.4f,%.4f,%.4f)  emitter=(%.4f,%.4f,%.4f)",
			specularChain.size(),
			shadingPoint.x, shadingPoint.y, shadingPoint.z,
			emitterPoint.x, emitterPoint.y, emitterPoint.z );
	}
#endif

#if SMS_SOLVE_DIAG
	g_solveDiag_calls.fetch_add( 1, std::memory_order_relaxed );
#endif

	if( specularChain.empty() )
	{
#if SMS_TRACE_DIAGNOSTIC
		g_solveEmpty.fetch_add( 1, std::memory_order_relaxed );
#endif
		return;
	}

	// Quick early-out: build minimal tangent frames from normals
	// and evaluate the initial constraint.  If the norm is too large,
	// bail before computing expensive surface derivatives.
	for( unsigned int i = 0; i < specularChain.size(); i++ )
	{
		ManifoldVertex& v = specularChain[i];
		if( Vector3Ops::SquaredModulus( v.dpdu ) < NEARZERO ||
			Vector3Ops::SquaredModulus( v.dpdv ) < NEARZERO )
		{
			v.dpdu = Vector3Ops::Perpendicular( v.normal );
			v.dpdu = Vector3Ops::Normalize( v.dpdu );
			v.dpdv = Vector3Ops::Cross( v.normal, v.dpdu );
			v.dpdv = Vector3Ops::Normalize( v.dpdv );
		}
	}

	// Evaluate constraint with minimal tangent frames.
	// Early-out if the seed is too far from any valid path.
	// Use a generous threshold — reflection seeds can be further
	// from the solution than refraction seeds because the straight-line
	// seed direction doesn't follow the reflected path.
	{
		std::vector<Scalar> localC0;
        auto& C0=scratch.Enabled()?scratch.Scalars(3,2*std::max<std::size_t>(config.maxChainDepth,specularChain.size())):localC0;
		EvaluateConstraint( specularChain, shadingPoint, emitterPoint, C0 );
		Scalar norm2 = 0.0;
		for( unsigned int i = 0; i < C0.size(); i++ )
			norm2 += C0[i] * C0[i];
#if SMS_TRACE_DIAGNOSTIC
		if( traceSolve ) {
			GlobalLog()->PrintEx( eLog_Event,
				"SMS_SOLVE:  initial ||C||=%.4f  threshold=2.0", sqrt(norm2) );
		}
#endif
		if( sqrt(norm2) > 2.0 )
		{
#if SMS_TRACE_DIAGNOSTIC
			g_solveSeedTooFar.fetch_add( 1, std::memory_order_relaxed );
			if( traceSolve ) {
				GlobalLog()->PrintEx( eLog_Event,
					"SMS_SOLVE:  EARLY-RETURN: seed too far from any valid path" );
			}
#endif
#if SMS_SOLVE_DIAG
			g_solveDiag_seedTooFar.fetch_add( 1, std::memory_order_relaxed );
#endif
			return;  // Seed too far from valid path
		}
	}

	// Now compute full surface derivatives (the expensive part)
	for( unsigned int i = 0; i < specularChain.size(); i++ )
	{
		ManifoldVertex& v = specularChain[i];
		if( !v.valid || (v.retainAlphaEndpoint && !v.HasAlphaEndpoint()) )
		{
			if( !ComputeVertexDerivatives( v, 0, convergenceThreshold > 0 ) )
			{
#if SMS_TRACE_DIAGNOSTIC
				g_solveDerivFail.fetch_add( 1, std::memory_order_relaxed );
				if( traceSolve ) {
					GlobalLog()->PrintEx( eLog_Event,
						"SMS_SOLVE:  REJECTED: ComputeVertexDerivatives failed at vertex %u (pos=(%.4f,%.4f,%.4f))",
						i, v.position.x, v.position.y, v.position.z );
				}
#endif
#if SMS_SOLVE_DIAG
				g_solveDiag_derivFail.fetch_add( 1, std::memory_order_relaxed );
#endif
				return;
			}
		}

		// Re-ensure tangent frame after derivative computation
		if( Vector3Ops::SquaredModulus( v.dpdu ) < NEARZERO ||
			Vector3Ops::SquaredModulus( v.dpdv ) < NEARZERO )
		{
			v.dpdu = Vector3Ops::Perpendicular( v.normal );
			v.dpdu = Vector3Ops::Normalize( v.dpdu );
			v.dpdv = Vector3Ops::Cross( v.normal, v.dpdu );
			v.dpdv = Vector3Ops::Normalize( v.dpdv );
		}
	}

	// Save seed template for Bernoulli trials
	const std::vector<ManifoldVertex> seedTemplate( specularChain );

#if SMS_T2_PRE_NEWTON_TOPOLOGY_CHECK
	// EXPERIMENT T2: validate seed-chain physics BEFORE Newton.  If
	// the seed is already wrong-topology, skip Solve — Newton can't
	// rescue an invalid seed via topology (per pushback diagnostics).
	{
#if SMS_SOLVE_DIAG
		g_t2_seedValidated.fetch_add( 1, std::memory_order_relaxed );
#endif
		if( !ValidateChainPhysics( specularChain, shadingPoint, emitterPoint ) )
		{
#if SMS_SOLVE_DIAG
			g_t2_seedSkipped.fetch_add( 1, std::memory_order_relaxed );
			// Count this as a phys-fail (the chain is wrong-topology;
			// Newton would have either rescued it OR preserved it as
			// phys-fail — we're betting most preserve).
			g_solveDiag_physicsFail.fetch_add( 1, std::memory_order_relaxed );
#endif
			return;
		}
	}
#endif

	// SMS two-stage solver (Zeltner 2020 §5).  Run Newton first on the
	// SMOOTHED reference surface (smoothing = 1: underlying analytical base,
	// no displacement / no high-frequency detail), then refine on the
	// actual surface (smoothing = 0).  Opt-in via `config.twoStage`; only
	// fires for chains whose vertices all support
	// `IObject::ComputeAnalyticalDerivatives`.
	//
	// SCOPE (verified empirically, see docs/SMS_TWO_STAGE_SOLVER.md):
	//
	//  - HELPS on smooth analytic primitives + normal-perturbing maps
	//    (relief_modifier / normal_map_modifier on
	//    sphere/ellipsoid/etc.).  Normal field is bumpy but POSITION is
	//    invariant under smoothing — Stage 1's converged uv is at the
	//    same world position as the bumpy caustic root, Stage 2's seed
	//    is in the basin of attraction.  Empirically reaches ΣL_sms /
	//    ΣL_supp ≈ 1.0 at sane bump amplitudes (~10° max perturbation).
	//
	//  - HURTS on heavily-displaced meshes (displaced_geometry with
	//    disp_scale > a few percent of the curvature radius).  Stage 1
	//    converges to the SMOOTH-surface caustic at uv `u*`, whose
	//    corresponding actual-mesh position is up to `disp_scale` units
	//    away from where the bumpy caustic actually lives.  Stage 2's
	//    seed at `u*` is FARTHER from the bumpy caustic than the
	//    original Snell-traced on-mesh seed — two-stage actively
	//    regresses convergence.
	//
	// Mitsuba's reference matches this scope: their Figure 9 (the
	// dedicated two-stage demonstration) uses only smooth analytic
	// primitives + `normalmap` BSDFs; their Figure 16 (displaced-mesh
	// comparison) never engages two-stage.  Their smoothing is BSDF-
	// driven via `lean()` (Olano-Baker LEAN moments), which is zero by
	// default — so two-stage is a no-op for non-normal-mapped BSDFs in
	// their codebase regardless.  Our geometry-side smoothing extends
	// further (we CAN smooth a displaced surface to its base), but the
	// extension hits a regime the original method wasn't designed to
	// handle.  See docs/SMS_TWO_STAGE_SOLVER.md for the data and
	// proof-from-source citations.
	if( config.twoStage )
	{
#if SMS_TRACE_DIAGNOSTIC
		static std::atomic<int> g_twostage_attempted{ 0 };
		static std::atomic<int> g_twostage_inputs_unavailable{ 0 };
		static std::atomic<int> g_twostage_stage1_failed{ 0 };
		static std::atomic<int> g_twostage_transition_failed{ 0 };
		static std::atomic<int> g_twostage_full_success{ 0 };
		const int ta = g_twostage_attempted.fetch_add( 1, std::memory_order_relaxed );
		if( (ta & 0x3ffff) == 0 && ta > 0 ) {
			GlobalLog()->PrintEx( eLog_Event,
				"SMS_TWOSTAGE: attempted=%d inputsUnavailable=%d stage1Failed=%d transitionFailed=%d fullSuccess=%d",
				ta,
				g_twostage_inputs_unavailable.load(),
				g_twostage_stage1_failed.load(),
				g_twostage_transition_failed.load(),
				g_twostage_full_success.load() );
		}
#endif

		// Snapshot the current state in case Stage 1 fails — we need to
		// fall back to the original on-mesh seed for Stage 2.
		const std::vector<ManifoldVertex> preStage1Snapshot( specularChain );

		// Re-evaluate every chain vertex's derivatives at smoothing = 1.
		// Bails out (cleanly, no scribble) if any vertex doesn't support
		// the smoothing-aware analytical query — those chains skip
		// two-stage and fall through to the original single-stage solve.
		bool stageInputsOk = true;
		for( unsigned int i = 0; i < specularChain.size(); i++ ) {
			if( !ComputeVertexDerivatives( specularChain[i], 1.0 ) ) {
				stageInputsOk = false;
				break;
			}
		}
		if( !stageInputsOk )
		{
#if SMS_TRACE_DIAGNOSTIC
			g_twostage_inputs_unavailable.fetch_add( 1, std::memory_order_relaxed );
#endif
			specularChain = preStage1Snapshot;
		}
		if( stageInputsOk )
		{
			const bool stage1Converged = NewtonSolve(
				specularChain, shadingPoint, emitterPoint, 1.0 );
			if( stage1Converged ) {
				// Stage 1 found a seed on the smooth (smoothing=1) surface.
				// Bridge to the actual mesh in two steps:
				//   (a) Re-evaluate analytical at smoothing=0 to land on
				//       the smooth-displaced surface — the underlying
				//       continuous bumpy surface that the tessellated mesh
				//       approximates.  Position differs from the mesh hit
				//       by chord-vs-arc tessellation error (~0.005 units
				//       for detail=128) — small.
				//   (b) Run `ComputeVertexDerivatives` at smoothing=0,
				//       which FD-probes from the now-close-to-mesh position
				//       to find the actual mesh hit and pull per-triangle
				//       UV-Jacobian derivatives.  The 0.05-unit probe
				//       offset is sufficient because step (a) put us within
				//       chord-vs-arc.
				// Without step (a), step (b) probes from the smoothing=1
				// position — which can be ~disp_scale units inside the
				// bumpy mesh, beyond the probe's reach.
				bool transitionOk = true;
				for( unsigned int i = 0; i < specularChain.size(); i++ ) {
					ManifoldVertex& v = specularChain[i];
					Point3  aP;
					Vector3 aN, aDpdu, aDpdv, aDndu, aDndv;
					if( v.pObject->ComputeAnalyticalDerivatives(
							v.uv, 0.0, aP, aN, aDpdu, aDpdv, aDndu, aDndv ) )
					{
						v.position = aP;
						v.normal   = aN;
						v.geomNormal = aN;	// analytical: shading == geometric
					}
					// Step (b): probe to actual mesh, pull mesh derivatives.
					if( !ComputeVertexDerivatives( v, 0.0 ) ) {
						transitionOk = false;
						break;
					}
				}
				if( !transitionOk ) {
#if SMS_TRACE_DIAGNOSTIC
					g_twostage_transition_failed.fetch_add( 1, std::memory_order_relaxed );
#endif
					specularChain = preStage1Snapshot;
				}
#if SMS_TRACE_DIAGNOSTIC
				else {
					g_twostage_full_success.fetch_add( 1, std::memory_order_relaxed );
				}
#endif
			} else {
				// Stage 1 didn't converge — restore pre-Stage-1 state and
				// fall through to single-stage Stage 2 with the original
				// on-mesh seed.
#if SMS_TRACE_DIAGNOSTIC
				g_twostage_stage1_failed.fetch_add( 1, std::memory_order_relaxed );
#endif
				specularChain = preStage1Snapshot;
			}
		}
	}

	// Run Newton solver — Stage 2 of the two-stage solver, or the single
	// stage when two-stage is disabled / inapplicable.
	const bool converged = NewtonSolve( specularChain, shadingPoint, emitterPoint, 0, convergenceThreshold );

#if SMS_TRACE_DIAGNOSTIC
	if( traceSolve ) {
		// Evaluate ||C|| after Newton to see how close it got
		std::vector<Scalar> localCfinal;
        auto& Cfinal=scratch.Enabled()?scratch.Scalars(4,2*std::max<std::size_t>(config.maxChainDepth,specularChain.size())):localCfinal;
		EvaluateConstraint( specularChain, shadingPoint, emitterPoint, Cfinal );
		Scalar fn2 = 0;
		for( std::size_t i = 0; i < Cfinal.size(); i++ ) fn2 += Cfinal[i] * Cfinal[i];
		GlobalLog()->PrintEx( eLog_Event,
			"SMS_SOLVE:  NewtonSolve converged=%d  final ||C||=%.4e  threshold=%.1e",
			int( converged ), sqrt( fn2 ), config.solverThreshold );
		for( std::size_t i = 0; i < specularChain.size(); i++ ) {
			GlobalLog()->PrintEx( eLog_Event,
				"SMS_SOLVE:   v[%zu] pos=(%.4f,%.4f,%.4f) n=(%.3f,%.3f,%.3f) eta=%.3f isRefl=%d isExit=%d",
				i,
				specularChain[i].position.x, specularChain[i].position.y, specularChain[i].position.z,
				specularChain[i].normal.x, specularChain[i].normal.y, specularChain[i].normal.z,
				specularChain[i].eta,
				int( specularChain[i].isReflection ), int( specularChain[i].isExiting ) );
		}
	}
#endif

	if( !converged )
	{
#if SMS_TRACE_DIAGNOSTIC
		g_solveNewtonFail.fetch_add( 1, std::memory_order_relaxed );
#endif
#if SMS_SOLVE_DIAG
		g_solveDiag_newtonFail.fetch_add( 1, std::memory_order_relaxed );
		// Bucket the post-Newton ||C|| so we know whether Newton was stuck
		// near the answer or diverged far.
		{
			std::vector<Scalar> localCend;
        auto& Cend=scratch.Enabled()?scratch.Scalars(5,2*std::max<std::size_t>(config.maxChainDepth,specularChain.size())):localCend;
			EvaluateConstraint( specularChain, shadingPoint, emitterPoint, Cend );
			Scalar n2 = 0;
			for( std::size_t ii = 0; ii < Cend.size(); ii++ ) n2 += Cend[ii] * Cend[ii];
			const Scalar nrm = std::sqrt( n2 );
			if(      nrm < 1e-3 ) g_solveDiag_newtonFail_lt1e3.fetch_add( 1, std::memory_order_relaxed );
			else if( nrm < 1e-2 ) g_solveDiag_newtonFail_lt1e2.fetch_add( 1, std::memory_order_relaxed );
			else if( nrm < 1e-1 ) g_solveDiag_newtonFail_lt1e1.fetch_add( 1, std::memory_order_relaxed );
			else if( nrm < 1.0  ) g_solveDiag_newtonFail_lt1e0.fetch_add( 1, std::memory_order_relaxed );
			else                  g_solveDiag_newtonFail_ge1e0.fetch_add( 1, std::memory_order_relaxed );
		}
		// EXPERIMENT (c): record vertex-0 (u, v) of the failing chain.
		if( !specularChain.empty() ) {
			StallTopology_BinFail( specularChain[0].uv );
		}
#endif
	}

	if( converged )
	{
		// Reject physically invalid converged solutions.
		if( !ValidateChainPhysics( specularChain, shadingPoint, emitterPoint ) )
		{
#if SMS_SOLVE_DIAG
			// PUSHBACK: was the SEED chain physics-valid?  If yes,
			// Newton dragged a valid seed into a wrong-topology basin
			// (solver-design issue).  If no, the seed was already bad
			// (Snell-trace bug or seed-builder logic error).
			if( ValidateChainPhysics( seedTemplate, shadingPoint, emitterPoint ) )
				g_physFail_seedWasValid.fetch_add( 1, std::memory_order_relaxed );
			else
				g_physFail_seedWasInvalid.fetch_add( 1, std::memory_order_relaxed );

			// PUSHBACK: post-Newton ‖C‖ at the rejected chain.
			{
				std::vector<Scalar> localCrej;
        auto& Crej=scratch.Enabled()?scratch.Scalars(6,2*std::max<std::size_t>(config.maxChainDepth,specularChain.size())):localCrej;
				EvaluateConstraint( specularChain, shadingPoint, emitterPoint, Crej );
				Scalar n2 = 0;
				for( std::size_t ii = 0; ii < Crej.size(); ii++ ) n2 += Crej[ii] * Crej[ii];
				const Scalar nrm = std::sqrt( n2 );
				if(      nrm < 1e-6 ) g_physFail_finalNorm_lt1e6.fetch_add( 1, std::memory_order_relaxed );
				else if( nrm < 1e-4 ) g_physFail_finalNorm_lt1e4.fetch_add( 1, std::memory_order_relaxed );
				else if( nrm < 1e-2 ) g_physFail_finalNorm_lt1e2.fetch_add( 1, std::memory_order_relaxed );
				else                  g_physFail_finalNorm_ge1e2.fetch_add( 1, std::memory_order_relaxed );
			}

			// PUSHBACK: chain length + reflection-mix.
			{
				const unsigned int kk = static_cast<unsigned int>( specularChain.size() );
				const unsigned int slot = (kk == 0) ? 0 : (kk >= 6 ? 5 : kk - 1);
				g_physFail_chainLen[slot].fetch_add( 1, std::memory_order_relaxed );
				bool anyRefl = false;
				for( const auto& vv : specularChain ) {
					if( vv.isReflection ) { anyRefl = true; break; }
				}
				if( anyRefl ) g_physFail_anyReflectionInChain.fetch_add( 1, std::memory_order_relaxed );
				else          g_physFail_allRefractionInChain.fetch_add( 1, std::memory_order_relaxed );
			}

			// EXPERIMENT (c): record vertex-0 (u, v) of the phys-failed chain.
			if( !specularChain.empty() ) {
				StallTopology_BinPhysFail( specularChain[0].uv );
			}
#endif
			bool rescued = false;

#if SMS_T3_POST_NEWTON_FAILING_VERTEX_RETRY
			// EXPERIMENT T3: targeted retry at the failing vertex only.
			// More surgical than E (which perturbs every vertex).  Find
			// which vertex's wi/wo sign-product was wrong, then sample a
			// small tangent-plane offset on JUST that vertex and rerun
			// Newton.  Tests whether the rejected basin has a phys-valid
			// neighbour reachable by a single-vertex perturbation.
			if( kT3FailingVertexRetries > 0 )
			{
				unsigned int failIdx = static_cast<unsigned int>( specularChain.size() );
				for( unsigned int i = 0; i < specularChain.size(); i++ )
				{
					const ManifoldVertex& vv = specularChain[i];
					const Point3 prevP = (i == 0) ? shadingPoint : specularChain[i-1].position;
					const Point3 nextP = (i == specularChain.size()-1) ? emitterPoint : specularChain[i+1].position;
					const Vector3 wiV = Vector3Ops::Normalize( Vector3Ops::mkVector3( prevP, vv.position ) );
					const Vector3 woV = Vector3Ops::Normalize( Vector3Ops::mkVector3( nextP, vv.position ) );
					const bool hasGeom = ( Vector3Ops::SquaredModulus( vv.geomNormal ) > NEARZERO );
					const Vector3& nT = hasGeom ? vv.geomNormal : vv.normal;
					const Scalar a = Vector3Ops::Dot( wiV, nT );
					const Scalar b = Vector3Ops::Dot( woV, nT );
					const bool fail = vv.isReflection ? ( a*b < 0.0 ) : ( a*b > 0.0 );
					if( fail ) { failIdx = i; break; }
				}
				if( failIdx < specularChain.size() )
				{
#if SMS_SOLVE_DIAG
					g_t3_attempted.fetch_add( 1, std::memory_order_relaxed );
#endif
					for( unsigned int retry = 0; retry < kT3FailingVertexRetries && !rescued; retry++ )
					{
#if SMS_SOLVE_DIAG
						g_t3_iters.fetch_add( 1, std::memory_order_relaxed );
#endif
						std::vector<ManifoldVertex> testChain( seedTemplate );
						const Scalar ru = sampler.Get1D() * (2.0 * kT3PerturbMag) - kT3PerturbMag;
						const Scalar rv = sampler.Get1D() * (2.0 * kT3PerturbMag) - kT3PerturbMag;
						testChain[failIdx].position = Point3Ops::mkPoint3(
							testChain[failIdx].position,
							testChain[failIdx].dpdu * ru + testChain[failIdx].dpdv * rv );
						if( !UpdateVertexOnSurface( testChain[failIdx], 0.0, 0.0 ) ) continue;
						if( !NewtonSolve( testChain, shadingPoint, emitterPoint ) ) continue;
						if( !ValidateChainPhysics( testChain, shadingPoint, emitterPoint ) ) continue;
						specularChain = testChain;
						rescued = true;
#if SMS_SOLVE_DIAG
						g_t3_rescued.fetch_add( 1, std::memory_order_relaxed );
#endif
					}
				}
			}
#endif

#if SMS_PHYSFAIL_RESTART_ENABLED
			// EXPERIMENT E: Gaussian-perturbed restart on EVERY vertex.
			// Coarser than T3.  Falls through to here if T3 didn't rescue
			// (or wasn't enabled).
			if( !rescued && kPhysFailRetries > 0 )
			{
#if SMS_SOLVE_DIAG
				g_physFailRestart_attempted.fetch_add( 1, std::memory_order_relaxed );
#endif
				for( unsigned int retry = 0; retry < kPhysFailRetries && !rescued; retry++ )
				{
#if SMS_SOLVE_DIAG
					g_physFailRestart_iters.fetch_add( 1, std::memory_order_relaxed );
#endif
					std::vector<ManifoldVertex> testChain( seedTemplate );
					bool perturbOk = true;
					for( unsigned int i = 0; i < testChain.size(); i++ )
					{
						const Scalar ru = sampler.Get1D() * (2.0 * kPhysFailPerturb) - kPhysFailPerturb;
						const Scalar rv = sampler.Get1D() * (2.0 * kPhysFailPerturb) - kPhysFailPerturb;
						testChain[i].position = Point3Ops::mkPoint3(
							testChain[i].position,
							testChain[i].dpdu * ru + testChain[i].dpdv * rv );
						if( !UpdateVertexOnSurface( testChain[i], 0.0, 0.0 ) ) {
							perturbOk = false;
							break;
						}
					}
					if( !perturbOk ) continue;
					if( !NewtonSolve( testChain, shadingPoint, emitterPoint ) ) continue;
					if( !ValidateChainPhysics( testChain, shadingPoint, emitterPoint ) ) continue;
					specularChain = testChain;
					rescued = true;
#if SMS_SOLVE_DIAG
					g_physFailRestart_rescued.fetch_add( 1, std::memory_order_relaxed );
#endif
				}
			}
#endif

			if( !rescued )
			{
#if SMS_TRACE_DIAGNOSTIC
				g_solvePhysicsFail.fetch_add( 1, std::memory_order_relaxed );
				if( traceSolve ) {
					GlobalLog()->PrintEx( eLog_Event,
						"SMS_SOLVE:  REJECTED by ValidateChainPhysics (wi/wo sidedness mismatch)" );
				}
#endif
#if SMS_SOLVE_DIAG
				g_solveDiag_physicsFail.fetch_add( 1, std::memory_order_relaxed );
#endif
				return;
			}
			// Fall through with the rescued chain.
		}

		// Reject chains with very short inter-vertex segments.
		//
		// The surface derivatives are computed via central finite
		// differences with probe radius derivProbeOffset ≈ 0.01.
		// When two adjacent vertices are closer than ~5× this radius,
		// the derivative stencils overlap — both vertices sample the
		// same surface patch, making the Jacobian entries correlated
		// and its determinant unreliable.  The resulting chainGeom/det
		// ratio can swing by 10-50× at nearby positions, causing
		// fireflies on displaced meshes with grazing-edge paths.
		{
			// Lowered from 0.05 to 0.01: for scenes with fine glass features
			// (torus tube radius 0.075, displaced mesh facets ~0.03), the
			// 0.05 threshold was rejecting a large fraction of legitimate
			// refraction chains — up to 85% of the rejections in the
			// torus_cross scene fell between 0.010 and 0.025.  These are
			// grazing paths through a thin tube that are entirely physical.
			// 0.01 still catches the truly pathological cases (e.g. < 1mm
			// separation where the derivative stencil radius 0.01 literally
			// overlaps the next vertex) without shedding good roots.
			// Native frames refine inside their own scale-relative matching
            // band; the legacy finite-difference stencil cutoff does not apply.
            const Scalar minReliableSegment = nativeEventConstraints
                ? std::sqrt(std::numeric_limits<Scalar>::epsilon()) * Point3Ops::Distance(shadingPoint,emitterPoint)
                : Scalar(0.01);
			const unsigned int k = static_cast<unsigned int>( specularChain.size() );
			bool tooShort = false;
			// tooShortIdx / tooShortDist exist only to feed the
			// SMS_TRACE_DIAGNOSTIC histogram + log.  Wrap declarations
			// AND assignments under the same #if so the variables don't
			// exist (and aren't unused-but-set) in production builds.
#if SMS_TRACE_DIAGNOSTIC
			unsigned int tooShortIdx = 0;
			Scalar tooShortDist = 0;
#endif
			for( unsigned int i = 0; i < k && !tooShort; i++ )
			{
				const Point3 prevPos = (i == 0) ? shadingPoint : specularChain[i-1].position;
				const Scalar d = Point3Ops::Distance( prevPos, specularChain[i].position );
				if( d < minReliableSegment ) {
					tooShort = true;
#if SMS_TRACE_DIAGNOSTIC
					tooShortIdx = i;
					tooShortDist = d;
#endif
				}
			}
			if( k > 0 && !tooShort )
			{
				const Scalar d = Point3Ops::Distance( specularChain[k-1].position, emitterPoint );
				if( d < minReliableSegment ) {
					tooShort = true;
#if SMS_TRACE_DIAGNOSTIC
					tooShortIdx = k;
					tooShortDist = d;
#endif
				}
			}
			if( tooShort )
			{
#if SMS_TRACE_DIAGNOSTIC
				g_solveShortSeg.fetch_add( 1, std::memory_order_relaxed );
				// Bucket the rejected distance to understand distribution
				static std::atomic<int> g_shortSegHist[5]{ {0}, {0}, {0}, {0}, {0} };
				int bucket = 0;
				if( tooShortDist < 0.001 ) bucket = 0;
				else if( tooShortDist < 0.005 ) bucket = 1;
				else if( tooShortDist < 0.010 ) bucket = 2;
				else if( tooShortDist < 0.025 ) bucket = 3;
				else bucket = 4;  // 0.025 - 0.05
				g_shortSegHist[bucket].fetch_add( 1, std::memory_order_relaxed );
				// Periodic dump
				if( (g_solveShortSeg.load() & 0x7fff) == 0 ) {
					GlobalLog()->PrintEx( eLog_Event,
						"SMS_SHORTSEG_HIST: [<0.001]=%d  [0.001-0.005]=%d  [0.005-0.010]=%d  [0.010-0.025]=%d  [0.025-0.050]=%d",
						g_shortSegHist[0].load(), g_shortSegHist[1].load(),
						g_shortSegHist[2].load(), g_shortSegHist[3].load(),
						g_shortSegHist[4].load() );
				}
				if( traceSolve ) {
					GlobalLog()->PrintEx( eLog_Event,
						"SMS_SOLVE:  REJECTED by minReliableSegment: segment %u too short (d=%.4f < %.4f)",
						tooShortIdx, tooShortDist, minReliableSegment );
				}
#endif
#if SMS_SOLVE_DIAG
				g_solveDiag_shortSeg.fetch_add( 1, std::memory_order_relaxed );
#endif
				return;
			}
		}

#if SMS_TRACE_DIAGNOSTIC
		g_solveOk.fetch_add( 1, std::memory_order_relaxed );
#endif
#if SMS_SOLVE_DIAG
		g_solveDiag_ok.fetch_add( 1, std::memory_order_relaxed );
		// EXPERIMENT (c): record vertex-0 (u, v) of OK chains too, so we
		// can compare fail vs ok density on the surface.
		if( !specularChain.empty() ) {
			StallTopology_BinOk( specularChain[0].uv );
		}
#endif
		result.valid = true;
		result.specularChain = specularChain;

		// Evaluate throughput (Fresnel * Beer's law along the chain)
		result.contribution = EvaluateChainThroughput( shadingPoint, emitterPoint, specularChain );

		// Compute constraint Jacobian determinant |det(∂C/∂x_⊥)| for the
		// converged chain.  Uses the half-vector analytical Jacobian because
		// the contribution formula was derived for that formulation.
		// Newton uses angle-difference for convergence, but the measure
		// conversion factor must match the derivation.
		{
			const unsigned int k = static_cast<unsigned int>( specularChain.size() );

			std::vector<Scalar> localDiag,localUpper,localLower;
            auto& diag=scratch.Enabled()?scratch.Scalars(0,4*std::max<unsigned>(config.maxChainDepth,k)):localDiag;
            auto& upper_blocks=scratch.Enabled()?scratch.Scalars(1,4*std::max<unsigned>(config.maxChainDepth,k)):localUpper;
            auto& lower_blocks=scratch.Enabled()?scratch.Scalars(2,4*std::max<unsigned>(config.maxChainDepth,k)):localLower;
			BuildJacobian( specularChain, shadingPoint, emitterPoint,
				diag, upper_blocks, lower_blocks );

			const Scalar detProduct = ComputeBlockTridiagonalDeterminant(
				diag, upper_blocks, lower_blocks, k );

			// Convert from parameter-space to tangent-plane world coordinates
			Scalar metricProduct = 1.0;
			for( unsigned int i = 0; i < k; i++ )
			{
				const Scalar dpduLen = Vector3Ops::Magnitude( specularChain[i].dpdu );
				const Scalar dpdvLen = Vector3Ops::Magnitude( specularChain[i].dpdv );
				if( dpduLen > NEARZERO && dpdvLen > NEARZERO )
				{
					metricProduct *= dpduLen * dpdvLen;
				}
			}

			result.jacobianDet = fabs( detProduct ) / fmax( metricProduct, 1e-20 );
			if( result.jacobianDet < 1e-20 )
				result.jacobianDet = 1e-20;
		}

		// PDF estimation
		if( !config.biased && estimateLegacyPDF )
		{
			result.pdf = EstimatePDF( result, shadingPoint, emitterPoint, seedTemplate, sampler );
		}
		else
		{
			result.pdf = 1.0;
		}
	}
	return;
}

//////////////////////////////////////////////////////////////////////
// EvaluateAtShadingPoint
//
//   Standalone SMS evaluation at a single shading point.
//   Samples a light, builds a seed chain, solves the manifold,
//   evaluates the BSDF, and assembles the full contribution.
//
//   This is the reusable core that both BDPTIntegrator and
//   SMSShaderOp call.
//////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////
// ReversePhotonChainForSeed
//
//   Converts a photon's recorded chain (light->diffuse) into an
//   SMS seed chain (receiver->light) by reversing the order and
//   flipping isExiting on refraction-only vertices.  Re-queries
//   each material to recover attenuation / canRefract flags that
//   the photon record doesn't carry.
//
//   See header doc-comment for caveats around `etaI`/`etaT` (left
//   at default 1.0 because SMSPhoton storage doesn't snapshot the
//   IOR stack).
//////////////////////////////////////////////////////////////////////

unsigned int ManifoldSolver::ReversePhotonChainForSeed(
	const SMSPhoton& photon,
	std::vector<ManifoldVertex>& chain,
	Scalar nm
	) const
{
	const unsigned int k = photon.chainLen;
	if( k == 0 || k > kSMSMaxPhotonChain ) {
		return 0;
	}

	chain.resize( k );
	IORStack queryIor( 1.0 );
	for( unsigned int i = 0; i < k; i++ )
	{
		const SMSPhotonChainVertex& pv = photon.chain[ k - 1 - i ];
		ManifoldVertex& mv = chain[i];
		// Output buffers may be reused: discarded solver/IOR/alpha state must
		// not override the photon reconstruction defaults (DL-439).
		mv = ManifoldVertex();
		mv.position    = pv.position;
		mv.objectPosition = pv.objectPosition;
		mv.uv = pv.uv;
		mv.normal      = pv.normal;
		// Photon record now stores geomNormal alongside shading (see
		// SMSPhoton.h::SMSPhotonChainVertex).  Fall back to shading only
		// for legacy photons whose geomNormal field is zero (sentinel).
		mv.geomNormal  = ( Vector3Ops::SquaredModulus( pv.geomNormal ) > NEARZERO )
			? pv.geomNormal : pv.normal;
		mv.pObject     = pv.pObject;
		mv.pMaterial   = pv.pMaterial;
		mv.eta         = pv.eta;

		if( pv.pMaterial ) {
			Ray dummyRay( pv.position, pv.normal );
			RayIntersectionGeometric rigLocal( dummyRay, nullRasterizerState );
			rigLocal.bHit          = true;
			rigLocal.ptIntersection = pv.position;
			rigLocal.ptObjIntersec = pv.objectPosition;
			rigLocal.ptCoord = pv.uv;
			rigLocal.vNormal       = pv.normal;
			// Mirror the geometric normal so any future GetSpecularInfo
			// implementation that consults vGeomNormal (dielectric side
			// selection per the Polished/Phong audit) sees a populated
			// value instead of the (0,0,0) sentinel — falling back to
			// shading on legacy photons whose geomNormal slot is zero.
			rigLocal.vGeomNormal   = mv.geomNormal;
			SpecularInfo spec = nm > 0 ?
				pv.pMaterial->GetSpecularInfoNM( rigLocal, queryIor, nm ) :
				pv.pMaterial->GetSpecularInfo( rigLocal, queryIor );
			if( nm > 0 ) mv.eta = spec.ior;
			mv.attenuation = spec.attenuation;
			mv.attenuationNM = spec.attenuationNM;
			mv.attenuationAppliesToReflection = spec.attenuationAppliesToReflection;
			mv.hasCustomSpecularFresnel = spec.hasCustomSpecularFresnel;
			mv.attenuationIsInteriorTransmittance = spec.attenuationIsInteriorTransmittance;
			mv.canRefract  = spec.canRefract;
		} else {
			mv.attenuation = RISEPel( 1, 1, 1 );
			mv.attenuationNM = 1;
			mv.attenuationAppliesToReflection = true;
			mv.hasCustomSpecularFresnel = false;
			mv.attenuationIsInteriorTransmittance = false;
			mv.canRefract  = true;
		}
		mv.isReflection = ( ( pv.flags & 0x2 ) != 0 );
		mv.isExiting    = mv.isReflection
			? ( ( pv.flags & 0x1 ) != 0 )
			: ( ( pv.flags & 0x1 ) == 0 );
		mv.valid = false;
	}
	return k;
}

//////////////////////////////////////////////////////////////////////
// ComputeTrialContribution
//
//   Per-trial contribution from a converged ManifoldResult.  See
//   the header doc-comment for the complete bail-out list.  Used by
//   both seeding modes and both photon-aided extension paths so the
//   contribution formula lives in exactly one place.
//////////////////////////////////////////////////////////////////////

bool ManifoldSolver::ComputeTrialContribution(
	const Point3& pos,
	const Vector3& geomNormal,
	const Vector3& shadingNormal,
	const OrthonormalBasis3D& onb,
	const Vector3& woOutgoing,
	const IBSDF* pBSDF,
	const LightSample& lightSample,
	const ManifoldResult& mResult,
	const IRayCaster& caster,
	Vector3& outDir,
	RISEPel& outContribution,
	bool clampGeometric,
	Scalar* outSmsGeometric,
	const IORStack* pIorStack, ISampler* alphaSampler
	) const
{
    RandomNumberGenerator alphaRandom;
    IndependentSampler alphaFallback(alphaRandom);
    ISampler& sampler = alphaSampler ? *alphaSampler : static_cast<ISampler&>(alphaFallback);
	outContribution = RISEPel( 0, 0, 0 );
	outDir = Vector3( 0, 0, 0 );

	if( !mResult.valid || mResult.specularChain.empty() || !pBSDF ) {
		return false;
	}

	// External-segment visibility (occluder between specular vertices,
	// or between last specular and the light).
	if( !CheckChainVisibility( pos, lightSample.position,
		mResult.specularChain, caster, &sampler ) ) {
		return false;
	}

	// Direction from shading point toward first specular vertex.
	const ManifoldVertex& firstSpec = mResult.specularChain[0];
	Vector3 dirToFirstSpec = Vector3Ops::mkVector3( firstSpec.position, pos );
	const Scalar distToFirstSpec = Vector3Ops::NormalizeMag( dirToFirstSpec );
	if( distToFirstSpec < 1e-8 ) {
		return false;
	}
	outDir = dirToFirstSpec;

	const Vector3 wiAtShading = dirToFirstSpec;

	// BSDF at shading point — SHADING frame (PBRT 4e §9.1, Veach §5.3.6).
	Ray evalRay( pos, Vector3( -woOutgoing.x, -woOutgoing.y, -woOutgoing.z ) );
	RayIntersectionGeometric rig( evalRay, nullRasterizerState );
	rig.bHit = true;
	rig.ptIntersection = pos;
	rig.vNormal     = shadingNormal;
	rig.vGeomNormal = geomNormal;
	rig.onb = onb;
	rig.ambientIOR = SMSReceiverAmbientIOR( pIorStack );	// DL-290

	RISEPel fBSDF = pBSDF->valueStateful( wiAtShading, rig, pIorStack );
	if( ColorMath::MaxValue( fBSDF ) <= 0 ) return false;

	// Receiver-side BSDF cosine — SHADING (the `cos θ` paired with
	// `f * cos / pdf` in Veach's energy-preserving shading-normals
	// trick, §5.3.6).
	const Scalar cosAtShading = fabs( Vector3Ops::Dot( shadingNormal, wiAtShading ) );
	if( cosAtShading <= 0 ) return false;

	// Light-side cos + Le for delta vs area lights.
	const ManifoldVertex& lastSpec = mResult.specularChain.back();
	Vector3 dirSpecToLight = Vector3Ops::mkVector3(
		lastSpec.position, lightSample.position );
	const Scalar distSpecToLight = Vector3Ops::NormalizeMag( dirSpecToLight );
	if( distSpecToLight < 1e-8 ) return false;

	Scalar cosAtLight;
	RISEPel actualLe;
	if( lightSample.isDelta ) {
		cosAtLight = 1.0;
		actualLe = lightSample.pLight
			? lightSample.pLight->emittedRadiance( dirSpecToLight )
			: lightSample.Le;
	} else {
		cosAtLight = fabs( Vector3Ops::Dot( lightSample.normal, dirSpecToLight ) );
		if( cosAtLight <= 0 ) return false;
		actualLe = SMSAreaLe( lightSample, dirSpecToLight );
		if( ColorMath::MaxValue(actualLe) <= 0 ) return false;
	}
	(void)cosAtLight;   // Implicit in detDvDy via (I - wo⊗wo) projection.

	// SMS measure-conversion factor (G_x_v1 * |det dv1/dy|).
	// G_x_v1 is a path-space geometry term (Veach §8.2 / PBRT 4e §13.6.4)
	// — uses the GEOMETRIC normal at the first specular vertex.  Falls
	// back to the chain-vertex shading normal on legacy / hand-built
	// chains where geomNormal is the zero sentinel.
	Vector3 dirXtoV1 = Vector3Ops::mkVector3( firstSpec.position, pos );
	const Scalar distXtoV1 = Vector3Ops::NormalizeMag( dirXtoV1 );
	if( distXtoV1 < 1e-8 ) return false;
	const Vector3& v1SideN = ( Vector3Ops::SquaredModulus( firstSpec.geomNormal ) > NEARZERO )
		? firstSpec.geomNormal : firstSpec.normal;
	const Scalar cosV1atX = fabs( Vector3Ops::Dot( v1SideN, dirXtoV1 ) );
	const Scalar G_x_v1 = cosV1atX / ( distXtoV1 * distXtoV1 );
	const Scalar detDvDy = ComputeLightToFirstVertexJacobianDet(
		mResult.specularChain, pos, lightSample.position, JacobianLightNormal( lightSample, mResult.specularChain ) );
	const Scalar smsGeometric = G_x_v1 * detDvDy;
	if( outSmsGeometric ) {
		*outSmsGeometric = smsGeometric;
	}
	const Scalar effectiveGeometric = clampGeometric && config.maxGeometricTerm > 0
		? std::fmin( smsGeometric, config.maxGeometricTerm )
		: smsGeometric;

	outContribution = fBSDF
		* mResult.contribution
		* actualLe * cosAtShading * effectiveGeometric
		/ ( lightSample.pdfPosition * lightSample.pdfSelect );

	return true;
}

//////////////////////////////////////////////////////////////////////
// ComputeTrialContributionNM
//
//   Spectral counterpart of ComputeTrialContribution.  Same logic,
//   per-wavelength throughput via EvaluateChainThroughputNM and
//   per-wavelength BSDF via valueNM. Area/delta emitters provide their
//   own wavelength radiance; legacy samples without an emitter pointer
//   retain the RGB illuminant-uplift fallback.
//////////////////////////////////////////////////////////////////////

bool ManifoldSolver::ComputeTrialContributionNM(
	const Point3& pos,
	const Vector3& geomNormal,
	const Vector3& shadingNormal,
	const OrthonormalBasis3D& onb,
	const Vector3& woOutgoing,
	const IBSDF* pBSDF,
	const LightSample& lightSample,
	const ManifoldResult& mResult,
	const IRayCaster& caster,
	const Scalar nm,
	Vector3& outDir,
	Scalar& outContribution,
	bool clampGeometric,
	Scalar* outSmsGeometric,
	const IORStack* pIorStack, ISampler* alphaSampler
	) const
{
    RandomNumberGenerator alphaRandom;
    IndependentSampler alphaFallback(alphaRandom);
    ISampler& sampler = alphaSampler ? *alphaSampler : static_cast<ISampler&>(alphaFallback);
	outContribution = 0;
	outDir = Vector3( 0, 0, 0 );

	if( !mResult.valid || mResult.specularChain.empty() || !pBSDF ) {
		return false;
	}

	if( !CheckChainVisibility( pos, lightSample.position,
		mResult.specularChain, caster, &sampler ) ) {
		return false;
	}

	const ManifoldVertex& firstSpec = mResult.specularChain[0];
	Vector3 dirToFirstSpec = Vector3Ops::mkVector3( firstSpec.position, pos );
	const Scalar distToFirstSpec = Vector3Ops::NormalizeMag( dirToFirstSpec );
	if( distToFirstSpec < 1e-8 ) {
		return false;
	}
	outDir = dirToFirstSpec;

	const Vector3 wiAtShading = dirToFirstSpec;

	// SHADING-frame BSDF eval — see RGB twin for rationale.
	Ray evalRay( pos, Vector3( -woOutgoing.x, -woOutgoing.y, -woOutgoing.z ) );
	RayIntersectionGeometric rig( evalRay, nullRasterizerState );
	rig.bHit = true;
	rig.ptIntersection = pos;
	rig.vNormal     = shadingNormal;
	rig.vGeomNormal = geomNormal;
	rig.onb = onb;
	rig.ambientIOR = SMSReceiverAmbientIOR( pIorStack );	// DL-290

	// DL-290: the stack goes to the BSDF exactly as the RGB twin's
	// valueStateful does (DL-157's plumbed entry/exit side).
	Scalar fBSDF = pBSDF->valueStatefulNM( wiAtShading, rig, nm, pIorStack );
	if( fBSDF <= 0 ) return false;

	// Receiver-side BSDF cosine: shading.
	Scalar cosAtShading = fabs( Vector3Ops::Dot( shadingNormal, wiAtShading ) );
	if( cosAtShading <= 0 ) return false;

	Scalar chainThroughput = EvaluateChainThroughputNM(
		pos, lightSample.position, mResult.specularChain, nm );

	const ManifoldVertex& lastSpec = mResult.specularChain.back();
	Vector3 dirSpecToLight = Vector3Ops::mkVector3(
		lastSpec.position, lightSample.position );
	const Scalar distSpecToLight = Vector3Ops::NormalizeMag( dirSpecToLight );
	if( distSpecToLight < 1e-8 ) return false;

	Scalar cosAtLight;
	Scalar Le;
	if( lightSample.isDelta ) {
		cosAtLight = 1.0;
		// Delta light: ask the light for its radiance AT `nm` -- it
		// carries its own illuminant spectrum (Stage C slice 2), same
		// row LightSampler's NM delta-light NEE takes.
		Le = lightSample.pLight
			? lightSample.pLight->emittedRadianceNM( dirSpecToLight, nm )
			: SMSLeNM( lightSample.Le, nm );
	} else {
		cosAtLight = fabs( Vector3Ops::Dot( lightSample.normal, dirSpecToLight ) );
		if( cosAtLight <= 0 ) return false;
		Le = lightSample.pLuminary ? SMSAreaLeNM( lightSample, dirSpecToLight, nm )
			: SMSLeNM( lightSample.Le, nm );
		if( Le <= 0 ) return false;
	}
	(void)cosAtLight;

	// G_x_v1 path-space geometry term — geometric (Veach §8.2).
	Vector3 dirXtoV1 = Vector3Ops::mkVector3( firstSpec.position, pos );
	const Scalar distXtoV1 = Vector3Ops::NormalizeMag( dirXtoV1 );
	if( distXtoV1 < 1e-8 ) return false;
	const Vector3& v1SideN = ( Vector3Ops::SquaredModulus( firstSpec.geomNormal ) > NEARZERO )
		? firstSpec.geomNormal : firstSpec.normal;
	const Scalar cosV1atX = fabs( Vector3Ops::Dot( v1SideN, dirXtoV1 ) );
	const Scalar G_x_v1 = cosV1atX / ( distXtoV1 * distXtoV1 );
	const Scalar detDvDy = ComputeLightToFirstVertexJacobianDet(
		mResult.specularChain, pos, lightSample.position, JacobianLightNormal( lightSample, mResult.specularChain ) );
	const Scalar smsGeometric = G_x_v1 * detDvDy;
	if( outSmsGeometric ) {
		*outSmsGeometric = smsGeometric;
	}
	const Scalar effectiveGeometric = clampGeometric && config.maxGeometricTerm > 0
		? std::fmin( smsGeometric, config.maxGeometricTerm )
		: smsGeometric;

	outContribution = fBSDF
		* chainThroughput
		* Le * cosAtShading * effectiveGeometric
		/ ( lightSample.pdfPosition * lightSample.pdfSelect );

	return true;
}

Scalar ManifoldSolver::ExtendedReflectionProbability(bool reflection, bool transmission,
    Scalar fresnel, bool tir, Scalar floor)
{
    if(!std::isfinite(floor) || floor <= 0 || floor > 0.5 || (!reflection && !transmission)
        || !std::isfinite(fresnel) || fresnel < 0 || fresnel > 1)
        return std::numeric_limits<Scalar>::quiet_NaN();
    if(tir) return reflection ? 1 : std::numeric_limits<Scalar>::quiet_NaN();
    if(!reflection) return 0;
    if(!transmission) return 1;
    // Even a zero at this seed retains exploration mass; it does not prove
    // that a spatial tint or a solved incidence makes the root's lobe zero.
    return std::max(floor, std::min(1-floor, fresnel));
}

bool ManifoldSolver::BuildExtendedSeed(const Point3& start, const Point3& end,
    const IScene& scene, const IORStack& live, SMSQueryDomain domain, ISampler& sampler,
    std::vector<SMSDomainVertex>& vertices, const RasterizerState& raster, const IObject* selectedEmitter) const
{
    return BuildExtendedWalk(start,end,scene,live,domain,sampler,vertices,raster,
        selectedEmitter,nullptr,nullptr);
}

bool ManifoldSolver::BuildExtendedWalk(const Point3& start,const Point3& end,
    const IScene& scene,const IORStack& live,SMSQueryDomain domain,ISampler& sampler,
    std::vector<SMSDomainVertex>& vertices,const RasterizerState& raster,
    const IObject* selectedEmitter,const std::vector<SMSDomainVertex>* topology,
    const Vector3* firstDirection) const
{
    SMSWorkerScratchLease scratch(true,config.referenceCounters);
    vertices.clear();
    const auto* objects = SMSDynamicCast<ObjectManager>(scene.GetObjects());
    if(!objects || !objects->ExtendedSMSAllowed() || !domain.Valid() || !config.maxChainDepth
        || !std::isfinite(config.extendedEventFloor) || config.extendedEventFloor <= 0
        || config.extendedEventFloor > 0.5) return false;
    const auto& casters = objects->ExtendedSMSCasters();
    if(casters.empty()) return false;
    for(const auto* caster:casters) if(!SMSAuditedModifier(*caster)) return false;
    SMSStartingMedia media; IORStack stack(live.EnvironmentIOR());
    if(!SMSDomainReplay::Capture(scene, start, live, media)
        || !SMSDomainReplay::BuildStack(media, domain, stack)) return false;
    if(topology && (topology->empty() || topology->size()>config.maxChainDepth
        || topology->size()>SMSChainRecord::kMaxVertices
        || (config.targetBounces && topology->size()!=config.targetBounces))) return false;
    const IObject* firstCaster = nullptr;
    Vector3 direction;
    if(topology) {
        firstCaster=topology->front().geometry.pObject;
        if(!firstDirection || std::find(casters.begin(),casters.end(),firstCaster)==casters.end()) return false;
        direction=*firstDirection;
    } else {
        const std::size_t chosen=std::min(casters.size()-1,
            static_cast<std::size_t>(sampler.Get1D()*casters.size()));
        firstCaster=casters[chosen];
        const Scalar u=sampler.Get1D(),v=sampler.Get1D(),w=sampler.Get1D();
        Point3 point;Vector3 normal;Point2 uv;
        firstCaster->UniformRandomPoint(&point,&normal,&uv,Point3(u,v,w));
        direction=Vector3Ops::mkVector3(point,start);
    }
    if(Vector3Ops::NormalizeMag(direction)<=0) return false;
    Point3 previous = start;
    for(unsigned int depth=0; depth<config.maxChainDepth; ++depth) {
        Ray ray(previous, direction);
        ray.Advance(1e-8); // shared native walk self-hit offset; not a root tolerance
        RayIntersection hit(ray, raster);
        objects->IntersectRay(hit, true, true, false);
        // With no exact bounce target, the emitter projection is the finite
        // walk boundary after the first caster. It is only a proposal filter;
        // a solved root must still pass complete ordered scene visibility.
        const Scalar emitterProjection = Vector3Ops::Dot(Vector3Ops::mkVector3(end, ray.origin), direction);
        if(!selectedEmitter && !config.targetBounces && !vertices.empty() && emitterProjection > 0
            && (!hit.geometric.bHit || hit.geometric.range > emitterProjection)) break;
        if(selectedEmitter && hit.geometric.bHit && hit.pObject==selectedEmitter) {
            if(vertices.empty()) return false;
            break;
        }
        if(topology && (vertices.size()>=topology->size()
            || hit.pObject!=(*topology)[vertices.size()].geometry.pObject
            || hit.pMaterial!=(*topology)[vertices.size()].geometry.pMaterial)) return false;
        if(!hit.geometric.bHit || !hit.pObject || !hit.pMaterial
            || (vertices.empty() && hit.pObject != firstCaster)) return false;
        if(hit.pModifier) hit.pModifier->Modify(hit.geometric);
        SMSNativeMaterialQuery query;
        if(!SMSDomainReplay::Query(*hit.pMaterial, hit.geometric, stack, domain, query)) return false;
        IORStack reflected(stack);
        Scalar etaI, etaT; bool exiting;
        if(!SMSDomainReplay::Cross(*hit.pMaterial, hit.pObject, hit.geometric, domain,
            true, reflected, etaI, etaT, exiting)) return false;
        const auto law=SMSNativeDirections(direction,SMSNativeEventNormal(*hit.pMaterial,hit.geometric),hit.geometric.UnflippedGeomNormal(),etaI,etaT,query.transmission,query.customFresnel,hit.pMaterial,exiting,SMSDomainWavelength(domain));
        const Scalar cosine=law.fresnelCosine;
        const bool tir=query.transmission && law.totalInternalReflection;
        Scalar fresnel = query.dielectricInterface
            ? Optics::CalculateDielectricReflectanceCosine(cosine, etaI, etaT) : Scalar(1);
        if(query.customFresnel && !tir) {
            const Scalar wavelength = domain.kind == SMSQueryDomain::Wavelength ? domain.nm
                : ScalarPainterRGB::kChannelNM[domain.component];
            const auto* dielectric=SMSDynamicCast<DielectricSPF>(hit.pMaterial->GetSPF());
            if(!dielectric || !dielectric->EvaluateSpecularFresnelAfterRefraction(
                cosine,etaI,etaT,exiting,wavelength,fresnel)) return false;
        }
        const Scalar probabilityR = ExtendedReflectionProbability(query.reflection, query.transmission,
            fresnel, tir, config.extendedEventFloor);
        if(!std::isfinite(probabilityR)) return false;
        const bool reflection = topology ? (*topology)[vertices.size()].geometry.isReflection
            : probabilityR == 1 || (probabilityR > 0 && sampler.Get1D() < probabilityR);
        if(topology && ((reflection && probabilityR<=0) || (!reflection && probabilityR>=1))) return false;
        if(!SMSDomainReplay::Cross(*hit.pMaterial, hit.pObject, hit.geometric, domain,
            reflection, stack, etaI, etaT, exiting)) return false;
        if(topology) {
            const auto& expected=(*topology)[vertices.size()].geometry;
            if(expected.isExiting!=exiting || expected.etaI!=etaI || expected.etaT!=etaT) return false;
        }
        vertices.emplace_back(hit.geometric);
        auto& vertex = vertices.back().geometry;
        vertex.position = SMSReferenceSurfacePoint(*hit.pObject,hit.geometric);
        vertex.normal = SMSNativeEventNormal(*hit.pMaterial,hit.geometric);
        vertex.geomNormal = hit.geometric.UnflippedGeomNormal();
        vertex.objectPosition = hit.geometric.ptObjIntersec;
        vertex.uv = hit.geometric.ptCoord;
        vertex.pObject = hit.pObject; vertex.pMaterial = hit.pMaterial;
        vertex.isReflection = reflection; vertex.isExiting = exiting;
        vertex.eta = query.index; vertex.etaI = etaI; vertex.etaT = etaT;
        vertex.canRefract = query.dielectricInterface;
        previous = vertex.position;
        direction=reflection?law.reflected:law.transmitted;
        if(!reflection && !law.hasTransmission) return false;
        if(Vector3Ops::NormalizeMag(direction) <= 0) return false;
        if((topology && vertices.size()==topology->size())
            || (config.targetBounces && vertices.size()==config.targetBounces)) break;
    }
    return !vertices.empty() && (!topology || vertices.size()==topology->size())
        && (!config.targetBounces || vertices.size() == config.targetBounces);
}

SMSDomainRoot ManifoldSolver::ProposeExtendedRoot(const Point3& start, const Vector3& startNormal,
    const Point3& end, const IScene& scene, const IORStack& stack, SMSQueryDomain domain,
    ISampler& sampler, const RasterizerState& raster) const
{
    SMSDomainRoot root(domain,stack);
    ProposeExtendedRootInto(start,startNormal,end,scene,stack,domain,sampler,raster,root);
    return root;
}

void ManifoldSolver::ProposeExtendedRootInto(const Point3& start, const Vector3& startNormal,
    const Point3& end, const IScene& scene, const IORStack& stack, SMSQueryDomain domain,
    ISampler& sampler, const RasterizerState& raster,SMSDomainRoot& root) const
{
    if(!nativeEventConstraints) {
        ManifoldSolver native(config,true,nullptr,SMSDomainWavelength(domain));
        native.ProposeExtendedRootInto(start,startNormal,end,scene,stack,domain,sampler,raster,root);
        return;
    }
    SMSWorkerScratchLease scratch(true,config.referenceCounters);
    root.domain=domain;root.startingStack=stack;root.vertices.clear();
    SMSResetResult(root.result);root.accepted=false;root.uncertainty=0;
    auto* counters = config.referenceCounters;
    if(counters) counters->proposalTrials.fetch_add(1, std::memory_order_relaxed);
    root.scale = Point3Ops::Distance(start,end);
    SMSStartingMedia startingMedia;
    if(!(root.scale > 0) || !std::isfinite(root.scale)
        || !SMSDomainReplay::Capture(scene,start,stack,startingMedia)
        || !SMSDomainReplay::BuildStack(startingMedia,domain,root.startingStack)
        || !BuildExtendedSeed(start, end, scene, root.startingStack, domain, sampler, root.vertices, raster)) {
        if(counters) counters->zeroTrials.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    FinalizeExtendedRootInto(start,startNormal,end,Vector3(0,0,1),scene,sampler,raster,root,nullptr);
}

void ManifoldSolver::FinalizeExtendedRootInto(const Point3& start,const Vector3& startNormal,
    const Point3& end,const Vector3& endNormal,const IScene& scene,ISampler& sampler,
    const RasterizerState& raster,SMSDomainRoot& root,const IObject* emitter,bool countRejection) const
{
    SMSWorkerScratchLease scratch(true,config.referenceCounters);
    auto* counters=config.referenceCounters;
    // Root matching resolves geometry at sqrt(machine epsilon) relative to
    // the endpoint scale. Polish the angular constraints more tightly, then
    // reject roots whose inverse-Jacobian correction cannot resolve that band.
    const Scalar tolerance = std::sqrt(std::numeric_limits<Scalar>::epsilon()) * root.scale;
    const Scalar polish = std::min(config.solverThreshold, std::sqrt(std::numeric_limits<Scalar>::epsilon())/64);
    ManifoldSolver replay(config,true,&root.vertices,SMSDomainWavelength(root.domain));
    replay.SolveDomainCoreInto(start,startNormal,end,endNormal,scene,root.startingStack,
        root.domain,root.vertices,sampler,tolerance,polish,root.result);
    if(root.result.valid) {
        Scalar contextGain=1;
        for(const auto& vertex:root.vertices) contextGain=std::max(contextGain,vertex.contextSlope*root.scale);
        if(contextGain>1 && std::isfinite(contextGain)) {
            // Resolve a smooth but steep context in its own matching band.
            // This deterministic refinement is part of every proposal and
            // retry, not an extra discovery or a changed event topology.
            const Scalar contextPolish=std::max(std::numeric_limits<Scalar>::epsilon(),polish/contextGain);
            replay.SolveDomainCoreInto(start,startNormal,end,endNormal,scene,root.startingStack,
                root.domain,root.vertices,sampler,tolerance,contextPolish,root.result);
        }
    }
    bool visible = root.result.valid;
    if(visible) {
        const auto& chain = root.result.specularChain;
        auto& residual=scratch.Scalars(0,2*config.maxChainDepth);
        auto& diagonal=scratch.Scalars(1,4*config.maxChainDepth);
        auto& upper=scratch.Scalars(2,4*config.maxChainDepth);
        auto& lower=scratch.Scalars(3,4*config.maxChainDepth);
        auto& correction=scratch.Scalars(4,2*config.maxChainDepth);
        ManifoldSolver native(config,true,&root.vertices,SMSDomainWavelength(root.domain));
        native.EvaluateConstraint(chain,start,end,residual);
        native.BuildJacobian(chain,start,end,diagonal,upper,lower,true);
        visible = SolveBlockTridiagonal(diagonal,upper,lower,residual,
            static_cast<unsigned int>(chain.size()),correction);
        if(visible) for(std::size_t i=0; i<chain.size(); ++i) {
            const auto& vertex = chain[i];
            const Scalar lastStep = Vector3Ops::Magnitude(vertex.dpdu*correction[2*i]+vertex.dpdv*correction[2*i+1]);
            const Scalar roundoff = std::numeric_limits<Scalar>::epsilon()
                * std::max({std::fabs(vertex.position.x),std::fabs(vertex.position.y),std::fabs(vertex.position.z),root.scale});
            root.uncertainty = std::max(root.uncertainty,lastStep+roundoff);
            // Geometry alone cannot resolve root equality on a steep native
            // context chart. Apply the same matching reserve to measured UV,
            // normal and object-position variation at this correction scale.
            const Scalar contextUncertainty=(lastStep+roundoff)*root.vertices[i].contextSlope;
            if(!std::isfinite(lastStep) || root.uncertainty > tolerance/8
                || !std::isfinite(contextUncertainty) || contextUncertainty > tolerance/(8*root.scale)) {
                visible=false;break;
            }
        }
    }
    Point3 previous = start;
    if(visible) for(const auto& record : root.vertices) {
        const auto& vertex = record.geometry;
        const Vector3 direction = Vector3Ops::Normalize(Vector3Ops::mkVector3(vertex.position,previous));
        Ray ray(previous,direction); ray.Advance(1e-8);
        RayIntersection hit(ray,raster);
        scene.GetObjects()->IntersectRay(hit,true,true,false);
        if(!hit.geometric.bHit || hit.pObject != vertex.pObject || hit.pMaterial != vertex.pMaterial
            || Point3Ops::Distance(SMSReferenceSurfacePoint(*hit.pObject,hit.geometric),vertex.position) > tolerance) {
            visible = false; break;
        }
        previous = vertex.position;
    }
    if(visible) {
        const Vector3 direction = Vector3Ops::Normalize(Vector3Ops::mkVector3(end,previous));
        Ray ray(previous,direction); ray.Advance(1e-8);
        RayIntersection hit(ray,raster);
        scene.GetObjects()->IntersectRay(hit,true,true,false);
        if(emitter) {
            visible=hit.geometric.bHit && hit.pObject==emitter
                && hit.pMaterial==emitter->GetMaterial()
                && Point3Ops::Distance(SMSReferenceSurfacePoint(*hit.pObject,hit.geometric),end)<=tolerance;
        } else if(hit.geometric.bHit && hit.geometric.range < Point3Ops::Distance(ray.origin,end)-tolerance)
            visible = false;
    }
    root.accepted = visible;
    if(!visible && counters && countRejection) {
        counters->zeroTrials.fetch_add(1, std::memory_order_relaxed);
        counters->rejectedRoots.fetch_add(1, std::memory_order_relaxed);
    }
    return;
}

bool ManifoldSolver::SameExtendedTopology(const SMSDomainRoot& a,const SMSDomainRoot& b)
{
    if(!a.domain.Valid() || !b.domain.Valid() || a.domain.kind!=b.domain.kind
        || a.domain.component!=b.domain.component || a.domain.nm!=b.domain.nm
        || !a.startingStack.SameInterfaces(b.startingStack) || a.vertices.empty()
        || a.vertices.size()!=b.vertices.size()) return false;
    for(std::size_t i=0;i<a.vertices.size();++i) {
        const auto& x=a.vertices[i].geometry;const auto& y=b.vertices[i].geometry;
        if(!x.pObject || !x.pMaterial || x.pObject!=y.pObject || x.pMaterial!=y.pMaterial
            || x.isReflection!=y.isReflection || x.isExiting!=y.isExiting
            || x.etaI!=y.etaI || x.etaT!=y.etaT) return false;
    }
    return true;
}

namespace
{
    // The canonical predicate must not depend on any random number. Native
    // Newton with reference PDF estimation off and a topology-constrained
    // walk draw nothing; this guard records a draw if that ever changes, and
    // the caller then treats the answer as uncertain (unowned) instead of
    // letting a fixed stream decide ownership.
    struct SMSCanonicalGuardSampler final : public ISampler {
        unsigned long long draws = 0;
        Scalar Get1D() override { ++draws; return Scalar(0.5); }
        Point2 Get2D() override { draws += 2; return Point2(0.5,0.5); }
    };
}

// The area partition's emitter point. SampleLight may return a point
// pushed off the surface (a single-sided clipped plane offsets 1e-5 along
// its normal) while PT's hit lies on it. Both sides map their point
// through this one projection onto the luminary, so the predicate's y is
// the same function of the surface point.
std::atomic<bool>& SMSExtendedTestHooks::DropAreaContributions()
{
    static std::atomic<bool> drop{false};
    return drop;
}

bool ManifoldSolver::ExtendedLuminaryPoint(const IObject& luminary,const Point3& p,const Vector3& n,Point3& out)
{
    out=p;
    Vector3 axis=n;
    if(Vector3Ops::NormalizeMag(axis)<=0) return false;
    // A single-sided sampler pushes its point 1e-5 along the OBJECT-space
    // normal (ClippedPlaneGeometry); bound that push in world space by the
    // transform's column lengths. The reach and the acceptance band stay
    // tied to it, so a thin or concave luminary, or large coordinates,
    // cannot project onto a different part of the same luminary.
    const Matrix4 m=luminary.GetFinalTransformMatrix();
    Scalar stretch=0;
    for(const Vector3& e:{Vector3(1,0,0),Vector3(0,1,0),Vector3(0,0,1)})
        stretch=std::max(stretch,Vector3Ops::Magnitude(Vector3Ops::Transform(m,e)));
    if(!(stretch>0) || !std::isfinite(stretch)) return false;
    const Scalar roundoff=64*std::numeric_limits<Scalar>::epsilon()
        *std::max<Scalar>(1,std::max({std::fabs(p.x),std::fabs(p.y),std::fabs(p.z)}));
    const Scalar push=Scalar(1e-5)*std::sqrt(Scalar(3))*stretch;
    const Scalar reach=2*push+roundoff;
    // Approach from the given side first, then from the other: a
    // single-sided surface is not hit from behind, the caller's normal may
    // name either face (B passes the sampled face normal, PT the hit's
    // ray-facing geometric normal), and on a luminary thinner than the
    // reach, or at a crease, the first approach can meet ANOTHER part
    // beyond the push band. Only a hit within the band is accepted.
    for(const Scalar side:{Scalar(1),Scalar(-1)}) {
        RayIntersection hit(Ray(Point3Ops::mkPoint3(p,axis*(side*reach)),axis*(-side)),nullRasterizerState);
        luminary.IntersectRay(hit,2*reach,true,true,false);
        if(!hit.geometric.bHit) continue;
        const Point3 q=SMSReferenceSurfacePoint(luminary,hit.geometric);
        if(!(Point3Ops::Distance(q,p)<=push+roundoff)) continue;
        out=q;
        return true;
    }
    return false;
}

bool ManifoldSolver::ExtendedAreaPartitionApplies(const IScene& scene,const IRayCaster& caster,
    const IObject* luminary,Scalar* pdfSelect) const
{
    if(pdfSelect) *pdfSelect=0;
    const LightSampler* ls=caster.GetLightSampler();
    if(!ls || ls->IsSoloActive() || !luminary || !luminary->GetGeometry()
        || !luminary->GetGeometry()->CanBeAreaLight() || !luminary->GetMaterial()
        || !luminary->GetMaterial()->GetEmitter()) return false;
    if(!pdfSelect) return true;
    const ILuminaryManager* manager=caster.GetLuminaries();
    const auto* luminaryManager=SMSDynamicCast<LuminaryManager>(manager);
    if(!luminaryManager) return false;
    const Scalar pmf=ls->PdfSelectLuminary(scene,
        const_cast<LuminaryManager*>(luminaryManager)->getLuminaries(),*luminary,Point3(0,0,0),Vector3(0,0,1));
    if(!(pmf>0) || !std::isfinite(pmf)) return false;
    *pdfSelect=pmf;
    return true;
}

void ManifoldSolver::CanonicalExtendedRoots(const Point3& start,const Vector3& startNormal,
    const IObject& luminary,const Point3& y,const Vector3& yNormal,const IScene& scene,
    const SMSDomainRoot& topology,const RasterizerState& raster,std::vector<SMSDomainRoot>& roots,
    const SMSDomainRoot* stopAt) const
{
    SMSCaptureMemoScope captureMemo;
    roots.clear();
    auto* counters=config.referenceCounters;
    const std::size_t k=topology.vertices.size();
    // Every walk acceptance filter: depth cap, chain-record bound and exact
    // bounce target. Emitter-stop and the first-caster rule are applied by
    // the constrained walk itself.
    if(!k || k>SMSChainRecord::kMaxVertices || k>config.maxChainDepth
        || (config.targetBounces && k!=config.targetBounces)
        || !topology.vertices.front().geometry.pObject) return;
    const Scalar scale=Point3Ops::Distance(start,y);
    if(!(scale>0) || !std::isfinite(scale)) return;
    // A fixed, bounded seed set keyed only by (anchor, emitter point,
    // topology, domain): an endpoint-aware trace toward y, then fixed
    // surface coordinates of the first caster (the trace toward y cannot
    // seed a reflection, and any one surface point can be occluded from
    // the anchor). A static policy: never sampled positions, salts,
    // caches, retry history or which paths PT happened to trace.
    static const Point3 kSurfaceSeeds[kExtendedCanonicalSurfaceSeeds]={
        Point3(.5,.5,.5),Point3(.25,.25,.25),Point3(.75,.25,.5),Point3(.25,.75,.5),Point3(.75,.75,.75)};
    std::vector<Vector3> directions(1+kExtendedCanonicalSurfaceSeeds);
    directions[0]=Vector3Ops::mkVector3(y,start);
    for(unsigned int i=0;i<kExtendedCanonicalSurfaceSeeds;++i) {
        Point3 point;Vector3 normal;Point2 uv;
        topology.vertices.front().geometry.pObject->UniformRandomPoint(&point,&normal,&uv,kSurfaceSeeds[i]);
        directions[1+i]=Vector3Ops::mkVector3(point,start);
    }
    // A refract/reflect/refract topology can reach a side of a closed
    // mesh that the endpoint ray and five surface seeds never visit.
    // Reflect y about each local bounding plane of its internal reflector.
    // These bounded directions depend only on the topology's object, its fixed
    // transform and the endpoints, never on a sampled walk's hit positions.
    for(std::size_t i=1;i+1<k;++i) {
        const auto& vertex=topology.vertices[i].geometry;
        const IObject* object=vertex.pObject;
        if(!vertex.isReflection || !object) continue;
        const IGeometry* geometry=object->GetGeometry();
        if(!geometry || (!SMSDynamicCast<TriangleMeshGeometryIndexed>(geometry)
            && !SMSDynamicCast<TriangleMeshGeometry>(geometry))) continue;
        const auto box=geometry->GenerateBoundingBox();
        const auto transform=object->GetFinalTransformMatrix();
        const Point3 localY=Point3Ops::Transform(object->GetFinalInverseTransformMatrix(),y);
        for(unsigned axis=0;axis<3;++axis) for(bool upper:{false,true}) {
            const Point3 bound=upper?box.ur:box.ll;Point3 image=localY;
            if(axis==0) image.x=2*bound.x-localY.x;
            else if(axis==1) image.y=2*bound.y-localY.y;
            else image.z=2*bound.z-localY.z;
            const Point3 localStart=Point3Ops::Transform(object->GetFinalInverseTransformMatrix(),start);
            // The virtual-image ray ignores refraction at the two other
            // faces. Bracket its incidence angle with two fixed corrections.
            for(Scalar factor:{Scalar(1),Scalar(.9),Scalar(1.1)}) {
                Point3 aim=image;
                if(axis==0) aim.x=localStart.x+factor*(image.x-localStart.x);
                else if(axis==1) aim.y=localStart.y+factor*(image.y-localStart.y);
                else aim.z=localStart.z+factor*(image.z-localStart.z);
                directions.push_back(Vector3Ops::mkVector3(Point3Ops::Transform(transform,aim),start));
            }
        }
        break; // bounded: the first internal reflector only
    }
    for(const Vector3& direction:directions) {
        SMSCanonicalGuardSampler guard;
        SMSDomainRoot root(topology.domain,topology.startingStack);
        root.scale=scale;
        if(!BuildExtendedWalk(start,y,scene,root.startingStack,root.domain,guard,root.vertices,
                raster,&luminary,&topology.vertices,&direction)
            || !SameExtendedTopology(root,topology)) continue;
        if(counters) counters->canonicalSolves.fetch_add(1,std::memory_order_relaxed);
        FinalizeExtendedRootInto(start,startNormal,y,yNormal,scene,guard,raster,root,&luminary,false);
        if(guard.draws) {
            if(counters) counters->canonicalSamplerDraws.fetch_add(guard.draws,std::memory_order_relaxed);
            continue; // uncertain: never owned
        }
        if(!root.accepted) continue;
        const Scalar tolerance=std::sqrt(std::numeric_limits<Scalar>::epsilon())*root.scale;
        bool duplicate=false;
        for(const auto& prior:roots) if(SameExtendedRoot(prior,root,tolerance,true)) {duplicate=true;break;}
        if(duplicate) continue;
        // A membership query stops at the first canonical root that IS the
        // queried chain; the answer (member or not) is unchanged.
        const bool member=stopAt && SameExtendedRoot(root,*stopAt,tolerance,true);
        roots.push_back(std::move(root));
        if(member) break;
    }
    if(counters) counters->canonicalRoots.fetch_add(roots.size(),std::memory_order_relaxed);
}

bool ManifoldSolver::ReplayExtendedChain(const SMSChainRecord& rec,const IObject& luminary,
    const IScene& scene,SMSQueryDomain domain,const RasterizerState& raster,SMSDomainRoot& root,
    bool* customFresnel) const
{
    if(customFresnel) *customFresnel=false;
    root.vertices.clear();root.domain=domain;root.accepted=false;root.uncertainty=0;
    SMSResetResult(root.result);
    const unsigned int k=rec.count;
    if(!rec.anchorValid || rec.broken || !k || k>SMSChainRecord::kMaxVertices
        || k>config.maxChainDepth || (config.targetBounces && k!=config.targetBounces)
        || !domain.Valid() || !std::isfinite(config.extendedEventFloor)
        || config.extendedEventFloor<=0 || config.extendedEventFloor>0.5) return false;
    const auto* objects=SMSDynamicCast<ObjectManager>(scene.GetObjects());
    if(!objects || !objects->ExtendedSMSAllowed()) return false;
    const auto& casters=objects->ExtendedSMSCasters();
    if(casters.empty()) return false;
    for(const auto* caster:casters) if(!SMSAuditedModifier(*caster)) return false;
    // The wrong-first-caster rule: the proposal can only start on a caster.
    if(std::find(casters.begin(),casters.end(),rec.v[0].pObject)==casters.end()) return false;
    // Same two-step start as estimator B: the receiver stack evaluated in
    // the domain, then the walk's own capture from that stack.
    SMSStartingMedia anchorMedia;IORStack domainStack(rec.anchorStack.EnvironmentIOR());
    if(!SMSDomainReplay::Capture(scene,rec.anchorPos,rec.anchorStack,anchorMedia)
        || !SMSDomainReplay::BuildStack(anchorMedia,domain,domainStack)) return false;
    root.startingStack=domainStack;
    SMSStartingMedia media;IORStack stack(domainStack.EnvironmentIOR());
    if(!SMSDomainReplay::Capture(scene,rec.anchorPos,domainStack,media)
        || !SMSDomainReplay::BuildStack(media,domain,stack)) return false;
    const Scalar scale=Point3Ops::Distance(rec.anchorPos,rec.v[k-1].position);
    Point3 previous=rec.anchorPos;
    for(unsigned int i=0;i<k;++i) {
        const SMSChainRecord::Vertex& recorded=rec.v[i];
        Vector3 direction=Vector3Ops::mkVector3(recorded.position,previous);
        if(Vector3Ops::NormalizeMag(direction)<=0) return false;
        Ray ray(previous,direction);
        ray.Advance(1e-8); // the walk's self-hit offset
        RayIntersection hit(ray,raster);
        objects->IntersectRay(hit,true,true,false);
        // The replayed ray must meet PT's own vertex; a closer hit or the
        // luminary means the chain is not one the walk can produce.
        if(!hit.geometric.bHit || !hit.pObject || !hit.pMaterial || hit.pObject!=recorded.pObject
            || hit.pObject==&luminary
            || Point3Ops::Distance(hit.geometric.ptIntersection,recorded.position)
                > Scalar(1e-6)*std::max(scale,Scalar(1))) return false;
        if(hit.pModifier) hit.pModifier->Modify(hit.geometric);
        SMSNativeMaterialQuery query;
        if(!SMSDomainReplay::Query(*hit.pMaterial,hit.geometric,stack,domain,query)) return false;
        if(customFresnel && query.customFresnel) *customFresnel=true;
        IORStack reflected(stack);
        Scalar etaI,etaT;bool exiting;
        if(!SMSDomainReplay::Cross(*hit.pMaterial,hit.pObject,hit.geometric,domain,
            true,reflected,etaI,etaT,exiting)) return false;
        const auto law=SMSNativeDirections(direction,SMSNativeEventNormal(*hit.pMaterial,hit.geometric),
            hit.geometric.UnflippedGeomNormal(),etaI,etaT,query.transmission,query.customFresnel,
            hit.pMaterial,exiting,SMSDomainWavelength(domain));
        const bool tir=query.transmission && law.totalInternalReflection;
        Scalar fresnel=query.dielectricInterface
            ? Optics::CalculateDielectricReflectanceCosine(law.fresnelCosine,etaI,etaT) : Scalar(1);
        if(query.customFresnel && !tir) {
            const Scalar wavelength=domain.kind==SMSQueryDomain::Wavelength ? domain.nm
                : ScalarPainterRGB::kChannelNM[domain.component];
            const auto* dielectric=SMSDynamicCast<DielectricSPF>(hit.pMaterial->GetSPF());
            if(!dielectric || !dielectric->EvaluateSpecularFresnelAfterRefraction(
                law.fresnelCosine,etaI,etaT,exiting,wavelength,fresnel)) return false;
        }
        const Scalar probabilityR=ExtendedReflectionProbability(query.reflection,query.transmission,
            fresnel,tir,config.extendedEventFloor);
        if(!std::isfinite(probabilityR)) return false;
        const bool reflection=recorded.isReflection;
        // An event the proposal could not sample here has zero walk mass.
        if((reflection && probabilityR<=0) || (!reflection && probabilityR>=1)) return false;
        if(!reflection && !law.hasTransmission) return false;
        if(!SMSDomainReplay::Cross(*hit.pMaterial,hit.pObject,hit.geometric,domain,
            reflection,stack,etaI,etaT,exiting)) return false;
        root.vertices.emplace_back(hit.geometric);
        auto& vertex=root.vertices.back().geometry;
        vertex.position=SMSReferenceSurfacePoint(*hit.pObject,hit.geometric);
        vertex.normal=SMSNativeEventNormal(*hit.pMaterial,hit.geometric);
        vertex.geomNormal=hit.geometric.UnflippedGeomNormal();
        vertex.objectPosition=hit.geometric.ptObjIntersec;
        vertex.uv=hit.geometric.ptCoord;
        vertex.pObject=hit.pObject;vertex.pMaterial=hit.pMaterial;
        vertex.isReflection=reflection;vertex.isExiting=exiting;
        vertex.eta=query.index;vertex.etaI=etaI;vertex.etaT=etaT;
        vertex.canRefract=query.dielectricInterface;
        previous=vertex.position;
    }
    return true;
}

int ManifoldSolver::ClassifyExtendedChain(const SMSChainRecord& rec,const IObject& luminary,
    const Point3& y,const Vector3& yNormal,const IScene& scene,SMSQueryDomain domain,
    const RasterizerState& raster,const std::vector<SMSDomainRoot>* ownedSet) const
{
    SMSCaptureMemoScope captureMemo;
    auto* counters=config.referenceCounters;
    // PT's chain as a topology T in this domain.
    SMSDomainRoot topology(domain,IORStack(rec.anchorStack.EnvironmentIOR()));
    if(!ReplayExtendedChain(rec,luminary,scene,domain,raster,topology)) return 0;
    // PT's chain projected onto the manifold by Newton from its own
    // vertices: itself for an exact delta chain; for a finite-scattering
    // warp, the root it is near (the adopted delta-limit treatment, DL-379).
    SMSDomainRoot actual(topology);
    actual.scale=Point3Ops::Distance(rec.anchorPos,y);
    if(!(actual.scale>0) || !std::isfinite(actual.scale)) return -1;
    SMSCanonicalGuardSampler guard;
    FinalizeExtendedRootInto(rec.anchorPos,rec.anchorShadingNormal,y,yNormal,scene,guard,raster,actual,&luminary,false);
    if(!actual.accepted && !guard.draws) {
        // A warped chain far from its delta-limit root can exhaust plain
        // Newton (measured: ~8 % of DL-379 ball-lens queries at the focus,
        // none on the exact-delta twin). A deterministic damped solve with
        // a larger budget extends the same "root Newton reaches from PT's
        // vertices" rule; it never changes the canonical set.
        ManifoldSolverConfig robust=config;
        robust.useLevenbergMarquardt=true;
        robust.maxIterations=std::max(4*config.maxIterations,60u);
        robust.referenceCounters=nullptr;robust.domainCounters=nullptr;
        ManifoldSolver projector(robust,true,nullptr,SMSDomainWavelength(domain));
        SMSDomainRoot retry(topology);retry.scale=actual.scale;
        projector.FinalizeExtendedRootInto(rec.anchorPos,rec.anchorShadingNormal,y,yNormal,scene,guard,raster,retry,&luminary,false);
        if(retry.accepted && !guard.draws) {
            if(counters) counters->robustProjections.fetch_add(1,std::memory_order_relaxed);
            actual=std::move(retry);
        }
    }
    if(guard.draws) {
        if(counters) counters->canonicalSamplerDraws.fetch_add(guard.draws,std::memory_order_relaxed);
        return -1;
    }
    std::vector<SMSDomainRoot> local;
    if(!actual.accepted) {
        if(counters) {
            if(!actual.result.valid) counters->projectionNewtonFailures.fetch_add(1,std::memory_order_relaxed);
            const std::vector<SMSDomainRoot>* roots=ownedSet;
            if(!roots) {
                CanonicalExtendedRoots(rec.anchorPos,rec.anchorShadingNormal,luminary,y,yNormal,scene,topology,raster,local);
                roots=&local;
            }
            Scalar nearest=RISE_INFINITY;
            for(const auto& root:*roots) {
                Scalar worst=0;
                const auto& chain=root.result.specularChain;
                for(std::size_t i=0;i<chain.size()&&i<rec.count;++i)
                    worst=std::max(worst,Point3Ops::Distance(chain[i].position,rec.v[i].position));
                nearest=std::min(nearest,worst);
            }
            if(!roots->empty()) counters->uncertainWithOwnedSet.fetch_add(1,std::memory_order_relaxed);
            if(nearest<=Scalar(1e-3)*actual.scale) counters->uncertainNearOwned.fetch_add(1,std::memory_order_relaxed);
            if(nearest<=Scalar(1e-8)*actual.scale) counters->uncertainAtOwned.fetch_add(1,std::memory_order_relaxed);
        }
        return -1;
    }
    const Scalar tolerance=std::sqrt(std::numeric_limits<Scalar>::epsilon())*actual.scale;
    if(!ownedSet) {
        CanonicalExtendedRoots(rec.anchorPos,rec.anchorShadingNormal,luminary,y,yNormal,scene,topology,raster,local,&actual);
        ownedSet=&local;
    }
    for(const auto& root:*ownedSet) if(SameExtendedRoot(root,actual,tolerance,true)) return 1;
    return 0;
}

bool ManifoldSolver::ExtendedEmitterHitOwned(const SMSChainRecord& rec,const IObject& luminary,
    const Point3& hitPoint,const Vector3& yNormal,const IScene& scene,const IRayCaster& caster,
    SMSQueryDomain domain,const RasterizerState& raster) const
{
    SMSCaptureMemoScope captureMemo;
    if(domain.kind==SMSQueryDomain::RGBComponent) {
        bool evaluate[3]={false,false,false},owned[3]={false,false,false};
        if(domain.component<3) evaluate[domain.component]=true;
        ExtendedEmitterHitOwnedRGB(rec,luminary,hitPoint,yNormal,scene,caster,evaluate,owned,raster);
        return domain.component<3 && owned[domain.component];
    }
    auto* counters=config.referenceCounters;
    if(counters) counters->partitionQueries.fetch_add(1,std::memory_order_relaxed);
    const auto uncertain=[counters]() {
        if(counters) counters->partitionUncertain.fetch_add(1,std::memory_order_relaxed);
        return false;
    };
    if(!ExtendedModeActive(scene) || !rec.anchorValid || !rec.extendedAnchor || rec.broken
        || !domain.Valid()) return uncertain();
    Scalar pdfSelect=0;
    if(!ExtendedAreaPartitionApplies(scene,caster,&luminary,&pdfSelect)) return false;
    SMSWorkerScratchLease scratch(true,config.referenceCounters);
    Point3 y;
    if(!ExtendedLuminaryPoint(luminary,hitPoint,yNormal,y)) return uncertain();
    const int answer=ClassifyExtendedChain(rec,luminary,y,yNormal,scene,domain,raster,nullptr);
    if(answer<0) return uncertain();
    if(answer>0 && counters) counters->partitionOwned.fetch_add(1,std::memory_order_relaxed);
    return answer>0;
}

namespace
{
    // Same replayed topology in two RGB components, and nothing in the
    // chain whose acceptance depends on the component's wavelength.
    bool SMSReplayComponentEquivalent(const SMSDomainRoot& a,const SMSDomainRoot& b)
    {
        if(a.vertices.size()!=b.vertices.size() || a.vertices.empty()
            || !a.startingStack.SameInterfaces(b.startingStack)
            || a.startingStack.top()!=b.startingStack.top()) return false;
        for(std::size_t i=0;i<a.vertices.size();++i) {
            const auto& x=a.vertices[i].geometry;const auto& y=b.vertices[i].geometry;
            if(x.pObject!=y.pObject || x.pMaterial!=y.pMaterial || x.isReflection!=y.isReflection
                || x.isExiting!=y.isExiting || x.etaI!=y.etaI || x.etaT!=y.etaT || x.eta!=y.eta
                || x.canRefract!=y.canRefract) return false;
        }
        return true;
    }
}

void ManifoldSolver::ExtendedEmitterHitOwnedRGB(const SMSChainRecord& rec,const IObject& luminary,
    const Point3& hitPoint,const Vector3& yNormal,const IScene& scene,const IRayCaster& caster,
    const bool evaluate[3],bool owned[3],const RasterizerState& raster) const
{
    SMSCaptureMemoScope captureMemo;
    auto* counters=config.referenceCounters;
    owned[0]=owned[1]=owned[2]=false;
    unsigned int queries=0;
    for(unsigned int c=0;c<3;++c) queries+=evaluate[c]?1u:0u;
    if(!queries) return;
    if(counters) counters->partitionQueries.fetch_add(queries,std::memory_order_relaxed);
    const auto uncertain=[counters,queries]() {
        if(counters) counters->partitionUncertain.fetch_add(queries,std::memory_order_relaxed);
    };
    if(!ExtendedModeActive(scene) || !rec.anchorValid || !rec.extendedAnchor || rec.broken) {uncertain();return;}
    Scalar pdfSelect=0;
    if(!ExtendedAreaPartitionApplies(scene,caster,&luminary,&pdfSelect)) return;
    SMSWorkerScratchLease scratch(true,config.referenceCounters);
    Point3 y;
    if(!ExtendedLuminaryPoint(luminary,hitPoint,yNormal,y)) {uncertain();return;}
    // Collapse: identical replays in every evaluated component, no coating
    // (wavelength-dependent) Fresnel anywhere -> one classification.
    int shared=-2;
    if(queries>1) {
        SMSDomainRoot first(SMSQueryDomain::RGB(0),IORStack(rec.anchorStack.EnvironmentIOR()));
        bool equivalent=true;bool firstSet=false;unsigned int firstComponent=0;
        for(unsigned int c=0;c<3 && equivalent;++c) {
            if(!evaluate[c]) continue;
            SMSDomainRoot replay(SMSQueryDomain::RGB(c),IORStack(rec.anchorStack.EnvironmentIOR()));
            bool coated=false;
            if(!ReplayExtendedChain(rec,luminary,scene,SMSQueryDomain::RGB(c),raster,replay,&coated) || coated) {equivalent=false;break;}
            if(!firstSet) {first=std::move(replay);firstSet=true;firstComponent=c;}
            else equivalent=SMSReplayComponentEquivalent(first,replay);
        }
        if(equivalent && firstSet) {
            shared=ClassifyExtendedChain(rec,luminary,y,yNormal,scene,SMSQueryDomain::RGB(firstComponent),raster,nullptr);
            if(counters) counters->componentReuse.fetch_add(queries-1,std::memory_order_relaxed);
        }
    }
    for(unsigned int c=0;c<3;++c) {
        if(!evaluate[c]) continue;
        const int answer=shared!=-2?shared
            :ClassifyExtendedChain(rec,luminary,y,yNormal,scene,SMSQueryDomain::RGB(c),raster,nullptr);
        if(answer<0) {if(counters) counters->partitionUncertain.fetch_add(1,std::memory_order_relaxed);continue;}
        owned[c]=answer>0;
        if(owned[c] && counters) counters->partitionOwned.fetch_add(1,std::memory_order_relaxed);
    }
}

bool ManifoldSolver::SameRediscoveredRoot(const SMSDomainRoot& a, const SMSDomainRoot& b, Scalar tolerance)
{
    // DL-455: estimator A's rediscovery test compares two converged solves
    // of one root from different seeds -- exactly the partition's case
    // below. The last-correction band does not bound their separation, so
    // the resolution-limited identity failed ~0.4 % of genuine
    // rediscoveries on the glass-slab side-TIR chains, every failure one
    // extra trial in K: the omni slab read +1.08 % (paired, 29 sigma).
    return SameExtendedRoot(a,b,tolerance,true);
}

bool ManifoldSolver::SameExtendedRoot(const SMSDomainRoot& a, const SMSDomainRoot& b, Scalar tolerance,
    bool partitionBand)
{
    if(!a.accepted || !b.accepted || !a.domain.Valid() || !b.domain.Valid()
        || !std::isfinite(a.scale) || !std::isfinite(b.scale) || a.scale <= 0 || b.scale <= 0
        || !std::isfinite(tolerance) || tolerance <= 0
        || a.domain.kind != b.domain.kind || a.domain.component != b.domain.component
        || a.domain.nm != b.domain.nm || !a.startingStack.SameInterfaces(b.startingStack)
        || a.vertices.size() != b.vertices.size() || a.vertices.empty()) return false;
    const Scalar scale = std::max(a.scale,b.scale);
    if(!std::isfinite(scale) || scale <= 0) return false;
    // A caller cannot widen reference equality to the legacy deduplication
    // band. Unresolved roots are zero trials in both discovery and retries.
    tolerance = std::min(tolerance,std::sqrt(std::numeric_limits<Scalar>::epsilon())*scale);
    if(!std::isfinite(a.uncertainty) || !std::isfinite(b.uncertainty)
        || a.uncertainty < 0 || b.uncertainty < 0
        || a.uncertainty > tolerance/8 || b.uncertainty > tolerance/8) return false;
    // The endpoint-scale band is a ceiling, not evidence that nearby
    // regular roots coincide. Compare positions at the resolution actually
    // established by both final corrections and coordinate roundoff.
    //
    // The area partition (partitionBand) instead compares two converged
    // solves of ONE canonical topology at the full band: two Newton runs
    // from different seeds end anywhere within their roundoff of the root,
    // which the last-correction band does not bound (measured: canonical
    // vs proposal solves of one ball-lens root 3e-15..3e-12 apart against
    // a 2e-15 band). Distinct regular roots closer than the band are a
    // fold, outside the supported regular-root domain.
    const Scalar positionTolerance=partitionBand?tolerance:std::min(tolerance,a.uncertainty+b.uncertainty);
    const Scalar normalTolerance = tolerance / scale;
    for(std::size_t i=0; i<a.vertices.size(); ++i) {
        const auto& x = a.vertices[i].geometry; const auto& y = b.vertices[i].geometry;
        const auto finiteGeometry=[](const ManifoldVertex& v) {
            return std::isfinite(v.position.x)&&std::isfinite(v.position.y)&&std::isfinite(v.position.z)
                &&std::isfinite(v.normal.x)&&std::isfinite(v.normal.y)&&std::isfinite(v.normal.z)
                &&std::isfinite(v.geomNormal.x)&&std::isfinite(v.geomNormal.y)&&std::isfinite(v.geomNormal.z)
                &&std::isfinite(v.objectPosition.x)&&std::isfinite(v.objectPosition.y)&&std::isfinite(v.objectPosition.z)
                &&std::isfinite(v.uv.x)&&std::isfinite(v.uv.y);
        };
        const bool uvIndependent=x.pMaterial && x.pObject
            && SMSContextIgnoresUV(*x.pObject,*x.pMaterial);
        const auto& xc=a.vertices[i].context;const auto& yc=b.vertices[i].context;
        const auto finiteFrame=[](const Vector3& value) {return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z);};
        for(unsigned axis=0;axis<3;++axis) {
            const Vector3 xv=axis==0?xc.onb.u():axis==1?xc.onb.v():xc.onb.w();
            const Vector3 yv=axis==0?yc.onb.u():axis==1?yc.onb.v():yc.onb.w();
            if(!finiteFrame(xv)||!finiteFrame(yv)
                ||((!uvIndependent || axis==2) && Vector3Ops::Magnitude(xv-yv)>normalTolerance)) return false;
        }
        if(!finiteFrame(xc.vNormal)||!finiteFrame(yc.vNormal)
            ||Vector3Ops::Magnitude(xc.vNormal-yc.vNormal)>normalTolerance) return false;
        const Point2 axes = x.pObject ? SMSPeriodicTextureAxes(*x.pObject,x.geomNormal) : Point2(0,0);
        const auto coordinateDistance = [](Scalar u, Scalar v, bool periodic) {
            const Scalar distance = std::fabs(u-v);
            return periodic ? std::min(distance,std::fabs(1-distance)) : distance;
        };
        // Keep the actual UV records. Under this audited material/modifier
        // dependency law, UV cannot alter any event/index/price
        // input. Different mesh charts then do not create physical roots.
        const Scalar uvDistance = uvIndependent ? 0 : std::hypot(coordinateDistance(x.uv.x,y.uv.x,axes.x != 0),
            coordinateDistance(x.uv.y,y.uv.y,axes.y != 0));
        if(!finiteGeometry(x) || !finiteGeometry(y) || !x.pObject || !x.pMaterial
            || x.uv.x != a.vertices[i].context.ptCoord.x || x.uv.y != a.vertices[i].context.ptCoord.y
            || y.uv.x != b.vertices[i].context.ptCoord.x || y.uv.y != b.vertices[i].context.ptCoord.y
            || x.pObject != y.pObject || x.pMaterial != y.pMaterial || x.isReflection != y.isReflection
            || x.isExiting != y.isExiting || x.etaI != y.etaI || x.etaT != y.etaT
            || Point3Ops::Distance(x.position,y.position) > positionTolerance
            || Vector3Ops::Magnitude(x.normal-y.normal) > normalTolerance
            || Vector3Ops::Magnitude(x.geomNormal-y.geomNormal) > normalTolerance
            || uvDistance > normalTolerance
            || Point3Ops::Distance(x.objectPosition,y.objectPosition)
                > normalTolerance*std::max({Scalar(1),std::fabs(x.objectPosition.x),std::fabs(x.objectPosition.y),
                    std::fabs(x.objectPosition.z),std::fabs(y.objectPosition.x),std::fabs(y.objectPosition.y),
                    std::fabs(y.objectPosition.z)})) return false;
    }
    return true;
}

RISEPel ManifoldSolver::EvaluateExtendedAreaReference(const Point3& pos,const Vector3& geomNormal,
    const Vector3& shadingNormal,const OrthonormalBasis3D& onb,const IMaterial& material,
    const Vector3& outgoing,const IScene& scene,const IRayCaster& caster,ISampler& parent,
    const LightSample& light,const IORStack& stack,const RayIntersectionGeometric* context,
    Scalar nm,int rgbComponent) const
{
    SMSCaptureMemoScope captureMemo;
    // Estimator B: one proposal walk yields a topology T; its owned roots
    // O(T,y) are the canonical solves (the same predicate PT suppresses
    // with); K counts independent walks until T recurs. The deposit
    // [sum_{r in O(T)} C(r,c)] * K / (q(c) pL) has expectation
    // sum_T C(O(T)) / pL with pL = selection pmf * area density, the
    // measure the light-to-chain Jacobian below converts from.
    RISEPel total(0,0,0);
    const IBSDF* bsdf=material.GetBSDF();
    if(!bsdf || light.isDelta || light.pEnvLight || !light.pLuminary
        || !(light.pdfSelect>0) || !(light.pdfPosition>0)
        || !std::isfinite(light.pdfSelect) || !std::isfinite(light.pdfPosition)
        || rgbComponent>2
        || !ExtendedAreaPartitionApplies(scene,caster,light.pLuminary,nullptr)
        || !ExtendedAnchorEligible(scene,caster,pos,stack,nm)) return total;
    SMSWorkerScratchLease scratch(true,config.referenceCounters);
    // Independently seeded discovery and retry streams; each consumes a
    // fixed two parent dimensions whatever N, K or the outcome.
    SMSLoopSampler discovery(parent,0x42304449u),retry(parent,0x42305254u);
    if(SMSExtendedTestHooks::DropAreaContributions().load(std::memory_order_relaxed)) return total; // test hook: PT-kept set only
    const unsigned int trials=config.multiTrials?config.multiTrials:1;
    const RasterizerState& raster=context?context->rast:nullRasterizerState;
    RayIntersectionGeometric receiver(Ray(pos,-outgoing),raster);
    if(context) receiver=*context;
    receiver.bHit=true;receiver.ptIntersection=pos;receiver.vNormal=shadingNormal;
    receiver.vGeomNormal=geomNormal;receiver.onb=onb;
    // The predicate's emitter point (on the luminary); the sampled point
    // keeps its area density: the projection only removes the sampler's
    // sub-1e-4 push. Uncertain -> SMS owns nothing here (PT's hit at the
    // same surface point is classified through the same projection).
    Point3 y;
    if(!ExtendedLuminaryPoint(*light.pLuminary,light.position,light.normal,y)) return total;
    // Canonical sets are a deterministic function of (anchor, y, T,
    // domain); anchor and y are fixed for this evaluation, so a topology
    // that recurs across the N trials reuses its set. Never across anchors.
    struct CachedTopology { SMSDomainRoot topology; std::vector<SMSDomainRoot> roots; Scalar physical; };
    std::vector<CachedTopology> cache;
    for(unsigned int trial=0;trial<trials;++trial) {
        const unsigned int component=nm>0?0:rgbComponent>=0?static_cast<unsigned>(rgbComponent)
            :std::min(2u,static_cast<unsigned>(discovery.sampler.Get1D()*3));
        const SMSQueryDomain domain=nm>0?SMSQueryDomain::NM(nm):SMSQueryDomain::RGB(component);
        if(config.referenceCounters) config.referenceCounters->proposalTrials.fetch_add(1,std::memory_order_relaxed);
        SMSStartingMedia media;IORStack domainStack(stack.EnvironmentIOR());
        bool proposed=SMSDomainReplay::Capture(scene,pos,stack,media)
            && SMSDomainReplay::BuildStack(media,domain,domainStack);
        SMSDomainRoot topology(domain,domainStack);
        proposed=proposed && BuildExtendedSeed(pos,light.position,scene,domainStack,domain,
            discovery.sampler,topology.vertices,raster,light.pLuminary);
        if(!proposed) {
            if(config.referenceCounters) config.referenceCounters->zeroTrials.fetch_add(1,std::memory_order_relaxed);
            continue;
        }
        // Ownership is decided here, before and independently of the K loop.
        const CachedTopology* entry=nullptr;
        for(const auto& cached:cache) if(SameExtendedTopology(cached.topology,topology)) {entry=&cached;break;}
        if(entry) {
            if(config.referenceCounters) config.referenceCounters->canonicalCacheHits.fetch_add(1,std::memory_order_relaxed);
        } else {
            CachedTopology fresh{topology,{},0};
            std::vector<SMSDomainRoot> canonical;
            CanonicalExtendedRoots(pos,shadingNormal,*light.pLuminary,y,light.normal,
                scene,topology,raster,canonical);
            std::vector<char> admitted(canonical.size(),0);
            for(std::size_t r=0;r<canonical.size();++r) {
                const auto& chain=canonical[r].result.specularChain;
                if(chain.empty() || chain.size()>SMSChainRecord::kMaxVertices) continue;
                // Symmetric rule: deposit a root only if PT, having traced
                // exactly this chain, would classify it owned. Otherwise a
                // canonical root whose own record is uncertain or unowned
                // would be counted by both estimators.
                SMSChainRecord record;
                record.SetAnchor(pos,geomNormal,shadingNormal,stack,true);
                record.count=static_cast<unsigned int>(chain.size());
                for(std::size_t i=0;i<chain.size();++i) {
                    auto& v=record.v[i];
                    v.position=chain[i].position;v.normal=chain[i].normal;v.geomNormal=chain[i].geomNormal;
                    v.objectPosition=chain[i].objectPosition;v.uv=chain[i].uv;v.pObject=chain[i].pObject;
                    v.isReflection=chain[i].isReflection;
                }
                admitted[r]=ClassifyExtendedChain(record,*light.pLuminary,y,light.normal,scene,domain,raster,&canonical)==1;
                if(!admitted[r] && config.referenceCounters)
                    config.referenceCounters->asymmetricRootsDeclined.fetch_add(1,std::memory_order_relaxed);
            }
            for(std::size_t r=0;r<canonical.size();++r) {
                if(!admitted[r]) continue;
                auto& root=canonical[r];
                const auto& chain=root.result.specularChain;
                Vector3 incoming=Vector3Ops::mkVector3(chain.front().position,pos);
                const Scalar distance=Vector3Ops::NormalizeMag(incoming);
                if(!(distance>0)) continue;
                receiver.ambientIOR=root.startingStack.top();
                const Scalar f=nm>0?bsdf->valueStatefulNM(incoming,receiver,nm,&root.startingStack)
                    :bsdf->valueStateful(incoming,receiver,&root.startingStack)[component];
                const Vector3 lightToChain=Vector3Ops::Normalize(Vector3Ops::mkVector3(chain.back().position,y));
                const Scalar le=nm>0?SMSAreaLeNM(light,lightToChain,nm):SMSAreaLe(light,lightToChain)[component];
                ManifoldSolver native(config,true,&root.vertices,SMSDomainWavelength(root.domain));
                const Scalar geometry=std::fabs(Vector3Ops::Dot(chain.front().geomNormal,incoming))/(distance*distance)
                    *native.ComputeLightToFirstVertexJacobianDet(chain,pos,y,light.normal);
                const Scalar value=f*std::fabs(Vector3Ops::Dot(shadingNormal,incoming))
                    *root.result.contributionNM*le*geometry;
                if(std::isfinite(value)) fresh.physical+=value;
                fresh.roots.push_back(std::move(root));
            }
            cache.push_back(std::move(fresh));
            entry=&cache.back();
        }
        if(entry->roots.empty()) {
            if(config.referenceCounters) config.referenceCounters->zeroTrials.fetch_add(1,std::memory_order_relaxed);
            continue; // an empty owned set enters no reciprocal loop
        }
        const Scalar physical=entry->physical;
        if(config.referenceCounters) config.referenceCounters->ownedRoots.fetch_add(entry->roots.size(),std::memory_order_relaxed);
        if(physical==0 || !std::isfinite(physical)) continue;
        if(config.referenceCounters) config.referenceCounters->acceptedDiscoveries.fetch_add(1,std::memory_order_relaxed);
        SMSDomainRoot retryTopology(domain,domainStack);
        const auto walk=[&](ISampler& sampler)->const SMSDomainRoot& {
            if(config.referenceCounters) config.referenceCounters->topologyRetryTrials.fetch_add(1,std::memory_order_relaxed);
            // The exact discovery walk law, failures included; no Newton.
            if(!BuildExtendedSeed(pos,light.position,scene,domainStack,domain,sampler,
                retryTopology.vertices,raster,light.pLuminary)) retryTopology.vertices.clear();
            return retryTopology;
        };
        const Scalar reciprocal=SMSRootReference::Reciprocal(topology,retry.sampler,walk,
            [](const SMSDomainRoot& a,const SMSDomainRoot& b){return SameExtendedTopology(a,b);},
            config.maxBernoulliTrials,true,config.referenceCounters);
        total[component]+=SMSRootReference::Deposit(physical,reciprocal,
            nm>0 || rgbComponent>=0?Scalar(1):Scalar(1)/3,
            light.pdfSelect*light.pdfPosition,trials);
    }
    return total;
}

RISEPel ManifoldSolver::EvaluateExtendedDelta(const Point3& pos, const Vector3& geomNormal,
    const Vector3& shadingNormal, const OrthonormalBasis3D& onb, const IMaterial& material,
    const Vector3& outgoing, const IScene& scene, const IRayCaster& caster, ISampler& parent,
    const LightSample& light, const IORStack& stack, const RayIntersectionGeometric* context, Scalar nm) const
{
    RISEPel total(0,0,0);
    const IBSDF* bsdf = material.GetBSDF();
    if(!bsdf || !light.isDelta || !light.pLight || light.pdfSelect <= 0
        || (light.pLight->lightType() != ILight::LightType::Point && light.pLight->lightType() != ILight::LightType::Spot)
        || !ExtendedAnchorEligible(scene,caster,pos,stack,nm)) return total;
    SMSWorkerScratchLease scratch(true,config.referenceCounters);
    auto& live=scratch.Root(0,SMSQueryDomain::RGB(0),stack,config.maxChainDepth);
    auto& retryRoot=scratch.Root(1,SMSQueryDomain::RGB(0),stack,config.maxChainDepth);
    SMSLoopSampler discovery(parent,0x41304449u), retry(parent,0x41305254u);
    const unsigned int trials = config.multiTrials ? config.multiTrials : 1;
    for(unsigned int trial=0; trial<trials; ++trial) {
        const unsigned int component = nm > 0 ? 0 : std::min(2u,static_cast<unsigned int>(discovery.sampler.Get1D()*3));
        const auto domain = nm > 0 ? SMSQueryDomain::NM(nm) : SMSQueryDomain::RGB(component);
        const auto propose = [&](ISampler& sampler) -> const SMSDomainRoot& {
            ProposeExtendedRootInto(pos,shadingNormal,light.position,scene,stack,domain,sampler,
                context ? context->rast : nullRasterizerState,retryRoot);
            return retryRoot;
        };
        ProposeExtendedRootInto(pos,shadingNormal,light.position,scene,stack,domain,discovery.sampler,
            context ? context->rast : nullRasterizerState,live);
        const SMSDomainRoot& root=live;
        if(!root.accepted) continue;
        const auto& chain = root.result.specularChain;
        Vector3 incoming = Vector3Ops::mkVector3(chain.front().position,pos);
        const Scalar distance = Vector3Ops::NormalizeMag(incoming);
        if(!(distance > 0)) continue;
        RayIntersectionGeometric hit(Ray(pos,-outgoing),context ? context->rast : nullRasterizerState);
        if(context) hit = *context;
        hit.bHit = true; hit.ptIntersection = pos; hit.vNormal = shadingNormal;
        hit.vGeomNormal = geomNormal; hit.onb = onb; hit.ambientIOR = root.startingStack.top();
        const Scalar f = nm > 0 ? bsdf->valueStatefulNM(incoming,hit,nm,&root.startingStack)
            : bsdf->valueStateful(incoming,hit,&root.startingStack)[component];
        const Vector3 lightToChain = Vector3Ops::Normalize(Vector3Ops::mkVector3(chain.back().position,light.position));
        const Scalar le = nm > 0 ? light.pLight->emittedRadianceNM(lightToChain,nm)
            : light.pLight->emittedRadiance(lightToChain)[component];
        ManifoldSolver native(config,true,&root.vertices,SMSDomainWavelength(root.domain));
        const Scalar geometry = std::fabs(Vector3Ops::Dot(chain.front().geomNormal,incoming)) / (distance*distance)
            * native.ComputeLightToFirstVertexJacobianDet(chain,pos,light.position,JacobianLightNormal(light,chain));
        const Scalar physical = f * std::fabs(Vector3Ops::Dot(shadingNormal,incoming))
            * root.result.contributionNM * le * geometry;
        if(!std::isfinite(physical) || physical == 0) continue;
        if(config.referenceCounters) {
            config.referenceCounters->acceptedDiscoveries.fetch_add(1,std::memory_order_relaxed);
            config.referenceCounters->ownedRoots.fetch_add(1,std::memory_order_relaxed);
        }
        const Scalar tolerance = std::sqrt(std::numeric_limits<Scalar>::epsilon()) * root.scale;
        const Scalar reciprocal = SMSRootReference::Reciprocal(root,retry.sampler,propose,
            [tolerance](const SMSDomainRoot& a, const SMSDomainRoot& b) { return SameRediscoveredRoot(a,b,tolerance); },
            config.maxBernoulliTrials,true,config.referenceCounters);
        total[component] += SMSRootReference::Deposit(physical,reciprocal,nm > 0 ? Scalar(1) : Scalar(1)/3,
            light.pdfSelect,trials);
    }
    return total;
}

ManifoldSolver::SMSContribution ManifoldSolver::EvaluateAtShadingPoint(
	const Point3& pos,
	const Vector3& geomNormal,
	const Vector3& shadingNormal,
	const OrthonormalBasis3D& onb,
	const IMaterial* pMaterial,
	const Vector3& woOutgoing,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	const IORStack* pIorStack, const RayIntersectionGeometric* anchorContext, bool forceLegacy
	) const
{
	// Mitsuba-faithful uniform-on-shape seeding (opt-in via
	// `sms_seeding "uniform"`).  The two seeding strategies are
	// structurally different enough — per-caster iteration vs single
	// Snell-traced seed — that they live in separate functions.  See
	// `docs/SMS_UNIFORM_SEEDING_PLAN.md`.
	if( config.seedingMode == ManifoldSolverConfig::eSeedingUniform ||
        (caster.GetLightSampler() && caster.GetLightSampler()->SceneHasAlphaCoverage()) )
	{
		// DL-290: forward the receiver's live IOR stack -- it was dropped
		// here, so uniform mode's receiver record priced air (and its
		// `valueStateful` never saw the stack DL-157 plumbed for it).
		return EvaluateAtShadingPointUniform(
			pos, geomNormal, shadingNormal, onb, pMaterial, woOutgoing,
			scene, caster, sampler, pIorStack, anchorContext, forceLegacy );
	}

	SMSContribution result;

	if( !pMaterial ) return result;

	const IBSDF* pBSDF = pMaterial->GetBSDF();
	if( !pBSDF ) return result;

	// Use the caster's prepared LightSampler (which has the alias table
	// built during scene preparation) rather than our own uninitialized one.
	const LightSampler* pLS = caster.GetLightSampler();
	if( !pLS ) return result;

	const ILuminaryManager* pLumMgr = caster.GetLuminaries();
	LuminaryManager::LuminariesList emptyList;
	const LuminaryManager* pLumManager = SMSDynamicCast<LuminaryManager>( pLumMgr );
	const LuminaryManager::LuminariesList& luminaries = pLumManager ?
		const_cast<LuminaryManager*>(pLumManager)->getLuminaries() : emptyList;

	LightSample lightSample;
	if( !pLS->SampleLight( scene, luminaries, sampler, lightSample ) )
		return result;

	// Env-light gate (continuous-PMF follow-up — adversarial-review
	// round 4 K.1, 2026-05-29).  Pre-fix env was selected by SampleLight
	// only in env-only scenes (binary EnvSelectProbability returned 0
	// in mixed scenes), so SMS never had to handle an env light vertex.
	// Post-fix env is selected at `cachedEnvSelectProb` rate in mixed
	// scenes, and `lightSample.position` is then a synthetic disc point
	// on the scene bounding sphere — NOT a real specular-chain target.
	// Building a Snell-trace seed toward the disc and Newton-solving
	// against a fictitious chain endpoint produces fireflies / silent
	// power-loss bias.  Skip here; env IBL specular caustics are out of
	// SMS scope (VCM / photon mapping handle them via merging).
	if( lightSample.pEnvLight ) {
		return result;
	}
    // Phase 3: a non-delta area emitter at an extended anchor is estimator
    // B's, under the canonical ownership predicate PT suppresses with.
    if(!forceLegacy && ExtendedModeActive(scene) && !lightSample.isDelta && lightSample.pLuminary) {
        const IORStack air(1);
        const IORStack& stack = pIorStack ? *pIorStack : air;
        const RISEPel value = EvaluateExtendedAreaReference(pos,geomNormal,shadingNormal,onb,*pMaterial,
            woOutgoing,scene,caster,sampler,lightSample,stack,anchorContext);
        result.contribution = value;
        result.valid = std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2])
            && (value[0] != 0 || value[1] != 0 || value[2] != 0);
        result.referenceA = true; // reference estimator: unclamped
        return result;
    }
    if(!forceLegacy && ExtendedModeActive(scene) && lightSample.isDelta && lightSample.pLight
        && (lightSample.pLight->lightType() == ILight::LightType::Point
            || lightSample.pLight->lightType() == ILight::LightType::Spot)) {
        const IORStack air(1);
        const IORStack& stack = pIorStack ? *pIorStack : air;
        const RISEPel value = EvaluateExtendedDelta(pos,geomNormal,shadingNormal,onb,*pMaterial,
            woOutgoing,scene,caster,sampler,lightSample,stack,anchorContext);
        result.contribution = value;
        result.valid = std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2])
            && (value[0] != 0 || value[1] != 0 || value[2] != 0);
        result.referenceA = true;
        return result;
    }


	// Sampler-dimension-drift firewall — see EvaluateAtShadingPointUniform
	// for the full rationale.  Variable-count internal work (multi-trial
	// loop, Solve→EstimatePDF) below uses `loopSampler`; the parent
	// sampler advances by exactly two dimensions for the seed.
	SMSLoopSampler loopScope( sampler );
	ISampler& loopSampler = loopScope.sampler;

	// Single-chain Snell-trace seed.  Path-tree branching was excised
	// in 2026-05 (matches Mitsuba SOTA: single stochastic seed per
	// trial; multi-trial averaging via `multiTrials` for variance
	// reduction on multi-modal scenes).
	// The deterministic base seed (BuildSeedChain toward the light, then
	// the normal-target and midpoint fallbacks) -- shared with PT's DL-372
	// split-suppression classifier, ClassifyEmitterHitCoverage.
	std::vector<SeedChainResult> baseSeeds;
	std::vector<ManifoldVertex> seedChain;
	unsigned int chainLen = BuildSnellBaseSeed(
		pos, geomNormal, lightSample.position, scene, caster,
		seedChain, pIorStack, &sampler, 0 );
	if( chainLen > 0 && !seedChain.empty() ) {
		SeedChainResult lone;
		lone.chain = seedChain;
		lone.proposalPdf = 1.0;
		baseSeeds.push_back( std::move( lone ) );
	}

	// Pure-mirror caster supplemental seeds.
	//
	// The Snell-trace from shading-point toward light cannot find
	// pure-mirror multi-bounce chains (e.g. diacaustic) — the
	// reflection law that determines the true seed positions doesn't
	// lie on the shading→light line.  Snell-mode therefore historically
	// misses cardioid / diacaustic patterns unless we supplement.
	//
	// Supplement: for each pure-mirror caster (canRefract=false), draw
	// `multi_trials` uniform-area samples on the caster and call
	// BuildSeedChain with end = sampled point.  The resulting chain
	// Snell-continues from the mirror hit (so a 2-bounce diacaustic
	// gets v0 from uniform sampling and v1 from the reflected ray's
	// next specular hit).  Append the resulting chains to `baseSeeds`
	// so the trial loop runs Newton on them.  No-op when
	// `mSpecularCasters` is empty (rasterizer didn't populate it).
	//
	// `applyEmitterStop = false`: the uniform-area sample point is a
	// direction probe, NOT the emitter — applying the projection cap
	// would silently truncate diacaustic chains to k=1.
	for( const IObject* pMirrorCaster : mSpecularCasters )
	{
		// Probe whether this caster is a pure mirror (canRefract=false).
		// A deterministic prand here is fine — the result is a binary
		// caster classification, not a sampling step.  Dielectrics are
		// skipped in the snell-mode supplement.
		if( !ProbeIsPureMirrorCaster( pMirrorCaster ) ) {
			continue;
		}

		const unsigned int M = std::max( config.multiTrials, 1u );
		for( unsigned int m = 0; m < M; m++ )
		{
			Point3 sp;
			Vector3 sn;
			Point2 sc;
			pMirrorCaster->UniformRandomPoint(
				&sp, &sn, &sc,
				Point3( loopSampler.Get1D(),
				        loopSampler.Get1D(),
				        loopSampler.Get1D() ) );

			std::vector<ManifoldVertex> mirrorChain;
			const unsigned int mirrorLen = BuildSeedChain(
				pos, sp, scene, caster, mirrorChain,
				/*applyEmitterStop=*/ false, pIorStack, &sampler );	// DL-290
			if( mirrorLen > 0 && !mirrorChain.empty() &&
			    mirrorChain[0].pObject == pMirrorCaster ) {
				SeedChainResult mc;
				mc.chain      = std::move( mirrorChain );
				mc.proposalPdf = Scalar( 1 );
				baseSeeds.push_back( std::move( mc ) );
			}
		}
	}

	// ========================================================================
	// DIAGNOSTIC: conditional per-pixel trace of the SMS solver.
	// Gated by the file-scope SMS_TRACE_DIAGNOSTIC at the top; default 0.
	// ========================================================================
#if SMS_TRACE_DIAGNOSTIC
	static std::atomic<int> g_smsTraceCount{ 0 };
	// Wider trace window: the entire caustic region.  Gate to only fire
	// on samples that END UP failing (or on the first few successes for
	// comparison).  Uses a second static counter that only increments
	// when valid_trials==0, so we see what's actually broken.
	const bool traceHere =
		( std::fabs( pos.x ) < 0.3 ) &&
		( std::fabs( pos.z ) < 0.3 ) &&
		( pos.y >= -0.02 && pos.y <= 0.02 ) &&
		( g_smsTraceCount.fetch_add( 1, std::memory_order_relaxed ) < 40 );
	if( traceHere ) {
		GlobalLog()->PrintEx( eLog_Event,
			"SMS_TRACE: pos=(%.4f,%.4f,%.4f) baseSeedLen=%u baseSeedEmpty=%d",
			pos.x, pos.y, pos.z,
			chainLen, int( seedChain.empty() ) );
		for( std::size_t i = 0; i < seedChain.size(); i++ ) {
			GlobalLog()->PrintEx( eLog_Event,
				"SMS_TRACE:  v%zu obj=%p pos=(%.4f,%.4f,%.4f) n=(%.3f,%.3f,%.3f) eta=%.3f isExit=%d valid=%d",
				i, (void*)seedChain[i].pObject,
				seedChain[i].position.x, seedChain[i].position.y, seedChain[i].position.z,
				seedChain[i].normal.x, seedChain[i].normal.y, seedChain[i].normal.z,
				seedChain[i].eta, int( seedChain[i].isExiting ), int( seedChain[i].valid ) );
		}
	}
#endif

	if( baseSeeds.empty() ) {
		// No seed chains from any source — Snell-trace, fallbacks,
		// AND pure-mirror caster supplement all came up empty.
		// Pre-fix this check looked only at `seedChain` (the legacy
		// single-chain variable), which would early-return even when
		// the mirror-caster supplement had populated `baseSeeds` —
		// that broke diacaustic-style scenes where the Snell-trace
		// from shading-point to light doesn't hit the mirror at all
		// but uniform-area sampling on the mirror does find chains.
#if SMS_TRACE_DIAGNOSTIC
		if( traceHere ) {
			GlobalLog()->PrintEx( eLog_Event,
				"SMS_TRACE:  EARLY-RETURN: no seed chain" );
		}
#endif
		return result;
	}

	// Synchronise legacy state with the first available base seed.
	// `seedChain`, `chainLen`, `pFirstCaster`, and the surface-sample-
	// fallback heuristic below all read from the legacy variables;
	// repopulating them from baseSeeds[0] ensures the rest of the
	// function (multi-trial loop, photon-aided seeding, etc.) sees a
	// coherent state when the seed came from the supplement path.
	if( seedChain.empty() && !baseSeeds[0].chain.empty() ) {
		seedChain = baseSeeds[0].chain;
		chainLen = static_cast<unsigned int>( seedChain.size() );
	}
	(void)chainLen;

	// Multi-trial Specular Manifold Sampling with PHOTON-AIDED first-vertex
	// seeding (Zeltner et al. 2020 §4.3 biased estimator [Eq. 8] + photon-
	// driven manifold sampling à la Weisstein, Jhang, Chang.
	// "Photon-Driven Manifold Sampling." HPG 2024.  DOI 10.1145/3675375.
	// https://dl.acm.org/doi/10.1145/3675375).
	//
	// A single Snell-traced seed chain + one Newton solve finds ONE root of
	// the specular-manifold constraint.  On a smooth convex refractor that is
	// the unique physically admissible caustic path; on a bumpy / displaced
	// surface there are typically several local manifold solutions per
	// (shading-point, emitter) pair and each is a legitimate caustic path
	// contribution.
	//
	// Local perturbation (first-pass attempt) proved inadequate: Newton's
	// basin around the "direct" Snell root is large enough that perturbed
	// seeds always fall back.  Uniform surface sampling (second-pass) DID
	// uncover more roots but cost O(N) Newton solves per shading point per
	// sample, most of which land on back-faces or non-caustic regions and
	// return zero contribution.  Photon-aided seeding solves both problems:
	// a one-time light-pass at scene-prep time deposits photons on diffuse
	// surfaces AFTER traversing specular chains, and each deposit records
	// the photon's FIRST specular-caster entry.  A render-time fixed-radius
	// kd-tree query "photons whose landing is near this shading point"
	// returns entry points that are KNOWN to produce valid caustic paths
	// ending in the shading-point neighborhood.
	//
	// Seed construction per trial:
	//   - Trial 0: the deterministic Snell-traced baseSeedChain (for
	//     N==1 backward compatibility and as a "guaranteed" good seed).
	//   - Trials 1..N-1: pull up to N-1 photons from the photon map,
	//     filter to those whose entryObject matches the base chain's
	//     first caster, and rebuild the seed chain toward each photon's
	//     recorded entryPoint.  If the photon map is null (no photon pass
	//     was run — sms_photon_count == 0) or the query returns nothing,
	//     the remaining trials are no-ops.
	//
	// Dedupe by first-vertex world position, sum contributions of unique
	// converged chains.  No /N division: contributions per root are
	// well-defined; we're not computing E[f(random seed)] but rather
	// Σ_r f_r restricted to discovered roots.
	//
	// Unbiased mode (config.biased==false) still applies the Bernoulli 1/p
	// weighting per-trial before the dedupe.
	const unsigned int N = ( config.multiTrials > 0 ) ? config.multiTrials : 1;
	// Dedupe threshold: roots whose first specular vertices are within this
	// world-space distance are treated as the same root.  Use the config
	// uniquenessThreshold (default 1e-4) — tight enough that distinct
	// bumps on a displaced surface count as separate roots, loose enough
	// that Newton-iteration round-off doesn't declare the same root
	// different across trials.
	const Scalar dedupeThr = ( config.uniquenessThreshold > 0.0 )
		? config.uniquenessThreshold : 1e-4;
	const std::vector<ManifoldVertex> baseSeedChain = seedChain;

	// The specular caster the base seed belongs to.  We restrict photon
	// seeds to the same object so the Newton solve operates on the chain
	// topology the caller built — crossing to a different caster would
	// require rebuilding the chain from scratch with potentially different
	// k, and changes the IOR stack semantics.
	const IObject* pFirstCaster = baseSeedChain[0].pObject;

	// All Fresnel-branched base seeds run UNCONDITIONALLY — each is a
	// distinct caustic chain that contributes its own energy.  The
	// `multi_trials = N` budget governs only the photon-aided trials
	// that run on top.  Total trials = numBaseSeeds + (N - 1)  where
	// the `-1` accounts for the legacy "trial 0 was the base seed"
	// semantic in the photon budget.
	//
	// Pre-fix bug: the loop iterated 0..N-1 and used baseSeeds[trial]
	// inside, so when N=1 (default `multi_trials=1`) only the first
	// branched chain ran and the rest of Branching's output was
	// silently dropped.  That produced the "diagnostic numbers move
	// but the rendered image doesn't change" symptom — the SMS
	// contribution was only ever counting one branch.
	const std::size_t numBaseSeeds = baseSeeds.size();

	// Pull photon-aided seeds from the rasterizer-owned photon map, if one
	// was built.  Query radius: the config-supplied value if positive,
	// otherwise the map's auto-computed value (bbox-diagonal * 0.01 —
	// analogous to VCM's merge-radius auto-fallback).
	//
	// Photon retrieval is independent of `multi_trials` (N): a user who
	// configures `sms_photon_count > 0` expects photons to be USED, even
	// at the default M=1.  The previous `N > 1` gate coupled the two
	// budgets so that scenes setting only `sms_photon_count` got an
	// empty photon-seed list at the consumption site below — photons
	// stored in the kd-tree but never reaching Newton.
	std::vector<SMSPhoton> photonSeeds;
	if( pPhotonMap && pPhotonMap->IsBuilt() )
	{
		Scalar r = config.photonSearchRadius;
		if( r <= 0 ) {
			r = pPhotonMap->GetAutoRadius();
		}
		if( r > 0 ) {
			pPhotonMap->QuerySeeds( pos, r * r, photonSeeds );
			RandomSubsamplePhotonSeeds( photonSeeds,
				config.maxPhotonSeedsPerShadingPoint, loopSampler );
		}
	}

	// Store the first specular vertex position of every accepted root so we
	// can dedupe later trials that converge to the same basin.  First-vertex
	// position is a unique enough identifier in practice: distinct roots of
	// the manifold constraint have distinct entry points on the specular
	// surface.
	std::vector<Point3> acceptedRootPositions;
	// Parallel vector of isReflection bitmasks (one bit per chain
	// vertex, lsb = vertex 0).  Two trials with the SAME first-vertex
	// position but DIFFERENT Fresnel-branch patterns (e.g. one chain
	// is refract-refract, sibling is refract-reflect) are distinct
	// roots — without this, multi-trial sibling chains at the same
	// first-vertex collapse under position-only dedupe.
	std::vector<unsigned long long> acceptedRootReflectMasks;
	auto buildReflectMask = []( const std::vector<ManifoldVertex>& ch ) -> unsigned long long {
		unsigned long long mask = 0;
		const std::size_t k = std::min<std::size_t>( ch.size(), 64 );
		for( std::size_t i = 0; i < k; i++ ) {
			if( ch[i].isReflection ) mask |= ( 1ull << i );
		}
		return mask;
	};
	RISEPel totalContribution( 0, 0, 0 );
	unsigned int validTrials = 0;

	// Track per-trial geometric term (G_x_v1 × |det dv/dy|) UNCLAMPED so
	// we can apply the config.maxGeometricTerm cap to the SUM across all
	// accepted distinct preimages, instead of to each one independently.
	//
	// Rationale: fold caustics produce multiple preimages at the same
	// pixel, each with a near-singular Jacobian.  A per-trial clamp then
	// saturates every preimage at the cap and sums them — so the clamp
	// scales with the number of trials instead of bounding the pixel.
	// Capping the sum is the well-posed version of the same safeguard:
	// "don't let the sum of specular-transport factors exceed a maximum
	// physical density at any one receiver point".
	std::vector<Scalar> acceptedGeoTerm;   ///< unclamped G·|det|
	std::vector<RISEPel> acceptedPreGeo;   ///< trialContribution / smsGeoUsed

	// photonCursor walks the photonSeeds list (filtering by entryObject as
	// we go) so trial indices don't directly map to photon indices.
	std::size_t photonCursor = 0;

	// Total trials: all branched base seeds + the photon budget + the
	// extra multi-trial budget.  When photons are present we want EVERY
	// queried photon to drive a Newton trial; when they aren't, we fall
	// back to N-1 extra-trial slots that the surface-sample fallback can
	// fill (k=1 mirror-chain case).  Without `+ photonSeeds.size()` here,
	// the trial loop's `trial >= numBaseSeeds` branch never sees most of
	// the queried photons even when QuerySeeds returned hundreds.
	//
	// Single-seed parity: with no photon map, `numBaseSeeds == 1` and
	// `photonSeeds.size() == 0`, so totalTrials degenerates to N
	// (the snell-mode default; mirror-supplemental and photon-aided
	// regimes can produce numBaseSeeds > 1 / photonSeeds non-empty).
	const unsigned int totalTrials = static_cast<unsigned int>( numBaseSeeds )
		+ static_cast<unsigned int>( photonSeeds.size() )
		+ ( N > 0 ? N - 1 : 0 );

	// Surface-sampling fallback for purely-reflective single-vertex chains.
	//
	// For a k=1 chain on a perfect-reflector, BuildSeedChain shoots a ray
	// from `pos` toward `lightSample.position` and records its first
	// specular hit.  That hit lies on the straight line shade↔light by
	// construction, so wi (mirror→shade) and wo (mirror→light) are exactly
	// anti-parallel and the half-vector h = wi + wo is identically zero
	// — the constraint is degenerate (||C|| = √2 from the hLen<NEARZERO
	// fallback in EvaluateConstraint), and Newton has no descent direction.
	//
	// Refraction doesn't have this problem because Snell's law bends the
	// ray inside the medium, so a typical refractive caustic chain is k=2
	// (entry+exit) and the second vertex lies OFF the shade↔light line.
	// Reflection produces no such bend, so k=1 reflection always degenerates.
	//
	// The principled remedy: seed each trial by sampling a uniform random
	// point on the reflective caster's surface.  On a smooth curved mirror
	// the basin of attraction around each reflection root is wide, so most
	// surface samples Newton-converge to a valid root — for the diacaustic
	// (a curved tube), the heart-shaped cardioid roots are reachable from
	// generic surface samples.  Photon-aided seeding (trials with photons
	// available) takes precedence; surface sampling fills the remainder.
	//
	// NULL-GEOMETRY GUARD (crash-fix sibling, mirrors SpecularCasterCollector::
	// operator()'s `if( !obj.GetGeometry() ) return true;` gate at ~line 3580):
	// pFirstCaster is `baseSeedChain[0].pObject`, sourced from a LIVE ray hit
	// (CSGObject::IntersectRay publishes `ri.pObject = this`), NOT from the
	// already-null-geometry-filtered `mSpecularCasters` cache -- so an
	// on-screen REFLECTIVE csg_object driving a k=1 chain reaches here with a
	// non-null `pFirstCaster` whose GetGeometry() is null.  Object::
	// UniformRandomPoint() has its own base-layer null guard now (Object.cpp)
	// so this no longer crashes, but silently falling through would feed
	// Object::UniformRandomPoint's fabricated (object-origin, +Y-normal)
	// fallback point into Newton as a manifold seed -- a WRONG-POSITION seed,
	// not a zero-measure/refused one (Newton would either fail to converge
	// from a bogus point or, worse, converge to a spurious root far from any
	// real reflection).  Refuse the fallback the same way a null-geometry
	// object is refused everywhere else in the SMS/luminary/SSS null-geometry
	// audit: no surface to uniformly sample, so no surface-sample fallback.
	// (baseSeedChain[0] itself -- the Snell-traced hit -- is still used
	// as-is when this is false; only the SUPPLEMENTAL surface-sample fallback
	// is refused.)
	const bool surfaceSampleReflectionFallback =
		( baseSeedChain.size() == 1 ) &&
		( !baseSeedChain[0].canRefract ) &&
		( pFirstCaster != nullptr ) &&
		( pFirstCaster->GetGeometry() != nullptr );

	// Log-once (process-wide) diagnostic + red-proof hook: fires exactly
	// when every OTHER surfaceSampleReflectionFallback condition holds but
	// the null-geometry guard above is what refused it -- i.e. an on-screen
	// reflective csg_object driving a k=1 chain.  Distinguishing this from
	// "the fallback just doesn't apply" (wrong chain length / refractive
	// material / no caster) is deliberate: it is the one case where the
	// refusal is a NEW behavior change (crash-fix round 3) rather than the
	// fallback's pre-existing scope, so tests can assert on it directly
	// (see ManifoldSolverNullGeometryReflectionFallbackTest.cpp).  Same
	// log-once idiom as SplatFilm.cpp's EvaluateFilter-support warning.
	if( ( baseSeedChain.size() == 1 ) &&
		( !baseSeedChain[0].canRefract ) &&
		( pFirstCaster != nullptr ) &&
		( pFirstCaster->GetGeometry() == nullptr ) )
	{
		static std::atomic<bool> warnedNullGeomReflectionFallback{ false };
		bool expected = false;
		if( warnedNullGeomReflectionFallback.compare_exchange_strong( expected, true ) ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"ManifoldSolver:: a k=1 reflective specular caster has no directly-owned "
				"geometry (e.g. a csg_object, whose shape comes from its two operand "
				"objects rather than a single geometry chunk) -- the surface-sample "
				"fallback (NULL_GEOM_REFLECTION_FALLBACK_REFUSED) is refused rather than "
				"fabricating a seed point; affected trials are skipped instead." );
		}
	}

	for( unsigned int trial = 0; trial < totalTrials; trial++ )
	{
		std::vector<ManifoldVertex> trialSeed = baseSeedChain;
		bool useSurfaceSample = false;

		// Trials 0..numBaseSeeds-1 consume the base seeds (Fresnel-
		// branched at trial 0 in legacy code; since the 2026-05 path-
		// tree branching excision there is exactly one base seed and
		// its `proposalPdf` is always 1.0, so this snell-mode path no
		// longer divides by it).
		// Subsequent trials consume photon seeds; if photons run out,
		// the surface-sample fallback fires for k=1 mirror chains.
		//
		// NOTE on entryObject: the photon's entryObject is the FIRST
		// caster the LIGHT'S ray hit — for a k>1 chain (e.g. a slab:
		// light hits top plane, then bottom, then floor) that's the caster
		// NEAREST the light.  SMS's chain is built in the opposite
		// direction (shading point -> light), so its first caster is the
		// one NEAREST the receiver.  Those are DIFFERENT objects for any
		// k>1 chain.  We therefore do NOT filter by caster identity; the
		// photon's entryPoint is useful simply as an aim-point: a ray from
		// the shading point toward that world position crosses the chain
		// in the right direction regardless of which side of the chain
		// the photon happened to enter from.
		if( trial < numBaseSeeds )
		{
			trialSeed = baseSeeds[trial].chain;
			// Surface-sample fallback only fires on the very first base
			// seed when it's the degenerate k=1 mirror case.
			if( trial == 0 && surfaceSampleReflectionFallback ) {
				useSurfaceSample = true;
			}
		}
		else
		{
			if( photonCursor >= photonSeeds.size() ) {
				if( surfaceSampleReflectionFallback ) {
					useSurfaceSample = true;
				} else {
					continue;  // no more photon seeds this trial round
				}
			}
			else
			{
			const SMSPhoton& ph = photonSeeds[photonCursor];
			photonCursor++;

			// Construct the SMS seed chain directly from the photon's
			// recorded chain — this is what fixes the topology-loss
			// problem.  The photon's chain is in photon-direction order
			// (v[0] nearest light, v[k-1] nearest diffuse); SMS walks
			// receiver→light, so we REVERSE the order and FLIP each
			// vertex's exit flag.  Re-tracing via BuildSeedChain from
			// the shading point would force the chain topology back to
			// whatever Snell's law dictates for a straight-line seed
			// target — typically k=2 even when the true caustic path is
			// k=4.  Using the photon's recorded chain preserves the
			// topology the photon itself followed.
			const unsigned int k = ph.chainLen;
			if( k == 0 || k > kSMSMaxPhotonChain ) {
				continue;
			}
			// TARGET BOUNCES (Mitsuba-parity) — inline photon-chain
			// reconstruction must apply the same length filter as
			// snell/uniform seeds.  Without this, photon-aided seeds
			// can supply chains of any length while the main snell
			// trace is constrained to K, producing inconsistent chain
			// lengths in the same Solve loop.
			if( config.targetBounces > 0 && k != config.targetBounces ) {
				continue;
			}

			std::vector<ManifoldVertex> newChain( k );
			IORStack queryIor( 1.0 );
			for( unsigned int i = 0; i < k; i++ )
			{
				const SMSPhotonChainVertex& pv = ph.chain[ k - 1 - i ];
				ManifoldVertex& mv = newChain[i];
				mv.position    = pv.position;
				mv.objectPosition = pv.objectPosition;
				mv.uv = pv.uv;
				mv.normal      = pv.normal;
				mv.geomNormal  = ( Vector3Ops::SquaredModulus( pv.geomNormal ) > NEARZERO )
					? pv.geomNormal : pv.normal;
				mv.pObject     = pv.pObject;
				mv.pMaterial   = pv.pMaterial;
				mv.eta         = pv.eta;
				// Query the material at this vertex for its actual specular
				// attenuation (a.k.a. refractance color for dielectrics,
				// reflectance for mirrors).  The previous hardcoded white
				// was correct for the typical white-glass test scenes but
				// silently dropped material colour for any coloured specular
				// — a colour / absorption bias in the caustic that only
				// photon-aided multi-trial SMS would expose as an occasional
				// bright-channel-mismatched trial.  BuildSeedChain already
				// does this lookup at line 2303; parity restores it here.
				if( pv.pMaterial ) {
					Ray dummyRay( pv.position, pv.normal );
					RayIntersectionGeometric rigLocal( dummyRay, nullRasterizerState );
					rigLocal.bHit = true;
					rigLocal.ptIntersection = pv.position;
					rigLocal.ptObjIntersec = pv.objectPosition;
					rigLocal.ptCoord = pv.uv;
					rigLocal.vNormal = pv.normal;
					rigLocal.vGeomNormal = mv.geomNormal;
					SpecularInfo spec = pv.pMaterial->GetSpecularInfo( rigLocal, queryIor );
					mv.attenuation = spec.attenuation;
					mv.attenuationNM = spec.attenuationNM;
					mv.attenuationAppliesToReflection = spec.attenuationAppliesToReflection;
					mv.hasCustomSpecularFresnel = spec.hasCustomSpecularFresnel;
					mv.attenuationIsInteriorTransmittance = spec.attenuationIsInteriorTransmittance;
					mv.canRefract  = spec.canRefract;
				} else {
					mv.attenuation = RISEPel( 1, 1, 1 );
					mv.attenuationNM = 1;
					mv.attenuationAppliesToReflection = true;
					mv.hasCustomSpecularFresnel = false;
					mv.attenuationIsInteriorTransmittance = false;
					mv.canRefract  = true;   // safe default: dielectric Fresnel path
				}
				// Chain-vertex semantics recovered from the photon record:
				//   flags bit 0 = photon-direction isExiting (refractions only)
				//   flags bit 1 = isReflection (scatter picked reflection, not
				//                  refraction — no medium change)
				// Photon-direction isExiting flag is FLIPPED: what was
				// ENTERING for the photon is EXITING for the receiver
				// ray going the other way through the same surface.
				// Reflection vertices are direction-independent so no flip.
				mv.isReflection = ( ( pv.flags & 0x2 ) != 0 );
				mv.isExiting    = mv.isReflection
				                ? ( ( pv.flags & 0x1 ) != 0 )    // preserve
				                : ( ( pv.flags & 0x1 ) == 0 );   // flip
				mv.valid     = false;        // derivatives will be re-computed by Solve
				// SMS photon storage doesn't carry the IOR-stack
				// snapshot at each vertex — only `eta` (the surface
				// material's IOR).  mv.etaI and mv.etaT stay at the
				// default 1.0 here, and downstream half-vector /
				// Fresnel math falls back to the air-on-other-side
				// assumption via GetEffectiveEtas.  Correct for
				// single-dielectric-in-air photon caustics; WRONG
				// for nested-dielectric photon-seeded chains.  See
				// the matching comment in EvaluateAtShadingPointNM
				// for the full rationale and the path to fix it
				// (extend SMSPhotonChainVertex storage at emission).
			}
			trialSeed = newChain;
			}  // end photon-aided branch (else of "no more photons")
		}

		// Build a single-vertex seed by uniform-sampling the reflective
		// caster's surface.  Inherits material data from the original
		// baseSeedChain[0]; only position/normal change.  Newton then
		// walks from this random surface point to a true reflection root.
		if( useSurfaceSample )
		{
			Point3 sp;
			Vector3 sn;
			Point2 sc;
			pFirstCaster->UniformRandomPoint(
				&sp, &sn, &sc,
				Point3( loopSampler.Get1D(), loopSampler.Get1D(), loopSampler.Get1D() ) );
			ManifoldVertex mv = baseSeedChain[0];
			mv.position = sp;
			mv.normal   = sn;
			mv.geomNormal = sn;	// uniform-area sample: best-available proxy
			mv.uv       = sc;
			mv.dpdu = Vector3(0,0,0);  // Solve will compute via ComputeVertexDerivatives
			mv.dpdv = Vector3(0,0,0);
			mv.dndu = Vector3(0,0,0);
			mv.dndv = Vector3(0,0,0);
			mv.valid = false;
			trialSeed.clear();
			trialSeed.push_back( mv );
		}

		ManifoldResult mResult = Solve(
			pos, shadingNormal,
			lightSample.position, lightSample.normal,
			trialSeed, loopSampler );

#if SMS_TRACE_DIAGNOSTIC
		if( traceHere ) {
			if( mResult.valid ) {
				GlobalLog()->PrintEx( eLog_Event,
					"SMS_TRACE:  trial=%u Solve VALID  v0=(%.4f,%.4f,%.4f) jacDet=%.3e contrib=(%.4f,%.4f,%.4f) pdf=%.4f",
					trial,
					mResult.specularChain[0].position.x,
					mResult.specularChain[0].position.y,
					mResult.specularChain[0].position.z,
					mResult.jacobianDet,
					mResult.contribution.r, mResult.contribution.g, mResult.contribution.b,
					mResult.pdf );
			} else {
				GlobalLog()->PrintEx( eLog_Event,
					"SMS_TRACE:  trial=%u Solve INVALID", trial );
			}
		}
#endif

		if( !mResult.valid ) continue;

		// Dedupe by (first-vertex world position, isReflection bitmask
		// over all chain vertices).  Two multi-trial chains can share
		// a first-vertex but differ on a later vertex's reflect/refract
		// label — those are physically distinct caustic paths and must
		// contribute separately.
		const Point3& firstPos = mResult.specularChain[0].position;
		const unsigned long long reflectMask =
			buildReflectMask( mResult.specularChain );
		bool duplicate = false;
		for( unsigned int r = 0; r < acceptedRootPositions.size(); r++ )
		{
			if( reflectMask == acceptedRootReflectMasks[r] &&
				Point3Ops::Distance( firstPos, acceptedRootPositions[r] ) < dedupeThr )
			{
				duplicate = true;
				break;
			}
		}
#if SMS_TRACE_DIAGNOSTIC
		if( traceHere && duplicate ) {
			GlobalLog()->PrintEx( eLog_Event,
				"SMS_TRACE:  trial=%u DUPLICATE -> skipped", trial );
		}
#endif
		if( duplicate ) continue;

		// Visibility: check external segments of the specular chain
		const bool visible = CheckChainVisibility( pos, lightSample.position,
			mResult.specularChain, caster, &sampler );
#if SMS_TRACE_DIAGNOSTIC
		{
			static std::atomic<int> g_visTotal{ 0 };
			static std::atomic<int> g_visBlocked{ 0 };
			const int tot = g_visTotal.fetch_add( 1, std::memory_order_relaxed );
			if( !visible ) g_visBlocked.fetch_add( 1, std::memory_order_relaxed );
			if( (tot & 0x3ffff) == 0 && tot > 0 ) {
				GlobalLog()->PrintEx( eLog_Event,
					"SMS_VIS_STATS: total=%d blocked=%d (%.2f%%)",
					tot, g_visBlocked.load(),
					100.0 * g_visBlocked.load() / tot );
			}
		}
#endif
#if SMS_TRACE_DIAGNOSTIC
		if( traceHere && !visible ) {
			GlobalLog()->PrintEx( eLog_Event,
				"SMS_TRACE:  trial=%u VISIBILITY FAIL", trial );
		}
#endif
		if( !visible ) continue;

		// Direction from shading point toward first specular vertex
		const ManifoldVertex& firstSpec = mResult.specularChain[0];
		Vector3 dirToFirstSpec = Vector3Ops::mkVector3(
			firstSpec.position, pos );
		Scalar distToFirstSpec = Vector3Ops::Magnitude( dirToFirstSpec );
		if( distToFirstSpec < 1e-8 ) continue;
		dirToFirstSpec = dirToFirstSpec * (1.0 / distToFirstSpec);

		const Vector3 wiAtShading = dirToFirstSpec;

		// Evaluate BSDF at shading point — SHADING frame.
		// RISE convention: ri.ray.Dir() is toward the surface (negate woOutgoing)
		Ray evalRay( pos, Vector3( -woOutgoing.x, -woOutgoing.y, -woOutgoing.z ) );
		RayIntersectionGeometric rig( evalRay, nullRasterizerState );
		rig.bHit = true;
		rig.ptIntersection = pos;
		rig.vNormal     = shadingNormal;
		rig.vGeomNormal = geomNormal;
		rig.onb = onb;
		rig.ambientIOR = SMSReceiverAmbientIOR( pIorStack );	// DL-290

		RISEPel fBSDF = pBSDF->valueStateful( wiAtShading, rig, pIorStack );
		if( ColorMath::MaxValue( fBSDF ) <= 0 ) continue;

		// Cosine at shading point: SHADING frame paired with `f * cos / pdf`.
		const Scalar cosAtShading = fabs( Vector3Ops::Dot( shadingNormal, wiAtShading ) );
		if( cosAtShading <= 0 ) continue;

		// Direction from light to last specular vertex and their distance
		// (needed for cosine evaluation at the light surface).
		const ManifoldVertex& lastSpec = mResult.specularChain.back();
		Vector3 dirSpecToLight = Vector3Ops::mkVector3(
			lastSpec.position, lightSample.position );
		Scalar distSpecToLight = Vector3Ops::Magnitude( dirSpecToLight );
		if( distSpecToLight < 1e-8 ) continue;
		dirSpecToLight = dirSpecToLight * (1.0 / distSpecToLight);

		// For delta-position lights (point/spot), there is no surface at the
		// light — the geometric coupling has no cosine term.  The emitted
		// radiance must also be re-evaluated for the actual direction from the
		// light toward the last specular vertex, since the LightSample's Le
		// was evaluated at a random photon direction.
		Scalar cosAtLight;
		RISEPel actualLe;
		if( lightSample.isDelta ) {
			cosAtLight = 1.0;
			if( lightSample.pLight ) {
				actualLe = lightSample.pLight->emittedRadiance( dirSpecToLight );
			} else {
				actualLe = lightSample.Le;
			}
		} else {
			cosAtLight = fabs( Vector3Ops::Dot( lightSample.normal, dirSpecToLight ) );
			if( cosAtLight <= 0 ) continue;
			actualLe = SMSAreaLe( lightSample, dirSpecToLight );
			if( ColorMath::MaxValue(actualLe) <= 0 ) continue;
		}

		// SMS measure-conversion via implicit function theorem.
		//
		// For a k-vertex specular chain x -> v_1 -> ... -> v_k -> y, we
		// sample the light endpoint y on its area and need the Jacobian
		// |dω_x / dA_y|.  Following Zeltner et al. 2020, applying the
		// implicit function theorem to C(v_1, ..., v_k, y) = 0 gives:
		//
		//   |dω_x / dA_y| = G(x, v_1) · |det(δv_1_⊥ / δy_⊥)|
		//
		// where G(x, v_1) = cos(θ_v1_at_x) / dist²(x, v_1) is the standard
		// solid-angle-from-area factor at the first specular vertex, and
		// |det(δv_1/δy)| is the 2x2 Jacobian computed by solving the
		// block-tridiagonal system J_v · δv = -J_y · δy.
		//
		// For k=1 this reduces to G(x, v_1) · |det(∂C/∂y)| / |det(∂C/∂v_1)|,
		// which matches the analytical apparent-depth formula
		// |dω_x/dA_y| = 1/(d_1 + n·d_2)² for a flat refractor at normal
		// incidence.  (d_1 = x-to-v_1, d_2 = v_1-to-y, n = relative IOR.)
		//
		// cos(θ_y) is IMPLICIT in |det(∂C/∂y)| — y-tangent perturbations
		// project onto the wo direction via (I - wo⊗wo), which naturally
		// includes the cos_y factor.  Do NOT multiply by cosAtLight again.
		//
		// The previous formula used 1/dist²(v_k, y) in place of |det(∂C/∂y)|
		// and was off by an eta-dependent factor (e.g. 1.44x for ior=2.2 at
		// normal incidence), producing systematically over-bright caustics
		// except for ior→1.
		const ManifoldVertex& firstSpecForG = mResult.specularChain[0];
		Vector3 dirXtoV1 = Vector3Ops::mkVector3( firstSpecForG.position, pos );
		const Scalar distXtoV1 = Vector3Ops::NormalizeMag( dirXtoV1 );
		if( distXtoV1 < 1e-8 ) continue;
		// G_x_v1 is path-space (Veach §8.2) — geometric with shading fallback.
		const Vector3& v1SideN = ( Vector3Ops::SquaredModulus( firstSpecForG.geomNormal ) > NEARZERO )
			? firstSpecForG.geomNormal : firstSpecForG.normal;
		const Scalar cosV1atX = fabs( Vector3Ops::Dot( v1SideN, dirXtoV1 ) );
		const Scalar G_x_v1 = cosV1atX / (distXtoV1 * distXtoV1);

		const Scalar detDvDy = ComputeLightToFirstVertexJacobianDet(
			mResult.specularChain, pos, lightSample.position, JacobianLightNormal( lightSample, mResult.specularChain ) );

		const Scalar smsGeometric = G_x_v1 * detDvDy;

		// Defer clamping: we cap the SUM across distinct preimages, not
		// each preimage individually, so fold-caustic pixels that produce
		// many near-singular Jacobians don't accumulate clamp×N fireflies.
		// The per-trial contribution carries the RAW smsGeometric; the
		// final scale-down (if any) is applied post-loop below.
		const Scalar clampedGeometric = smsGeometric;

		RISEPel trialContribution = fBSDF
			* mResult.contribution
			* actualLe * cosAtShading * clampedGeometric
			/ (lightSample.pdfPosition * lightSample.pdfSelect);

		// Silence unused-variable warning if cosAtLight ends up unused;
		// keep its computation above for backface culling (non-delta lights).
		(void)cosAtLight;

		// Fold the SMS sampler's own PDF into the per-trial contribution.
		//   Biased: mResult.pdf == 1.0, no change.
		//   Unbiased: mResult.pdf carries the Bernoulli-estimated 1/p, so
		//   dividing here yields a correctly re-weighted unbiased estimate
		//   for this trial.
		if( mResult.pdf > 1e-20 )
		{
			trialContribution = trialContribution / mResult.pdf;
		}

#if SMS_TRACE_DIAGNOSTIC
		if( traceHere ) {
			GlobalLog()->PrintEx( eLog_Event,
				"SMS_TRACE:  trial=%u ACCEPTED  G_x_v1=%.3e detDvDy=%.3e smsGeo=%.3e clamp=%.2f "
				"cosAtShade=%.3f Le=(%.3f,%.3f,%.3f) fBSDF=(%.3e,%.3e,%.3e) trialContrib=(%.3e,%.3e,%.3e)",
				trial, G_x_v1, detDvDy, smsGeometric, config.maxGeometricTerm,
				cosAtShading,
				actualLe.r, actualLe.g, actualLe.b,
				fBSDF.r, fBSDF.g, fBSDF.b,
				trialContribution.r, trialContribution.g, trialContribution.b );
		}
#endif

#if SMS_TRACE_DIAGNOSTIC
		// Firefly-triggered diagnostic: log when per-trial contribution
		// exceeds 5.0 luminance, regardless of positional gate, capped
		// by an atomic counter to avoid log flooding.  This caught the
		// reflection-vs-refraction photon-chain-flag bug — chainLen=1
		// "refraction" chains whose photon had actually picked the
		// Fresnel reflection branch carry a spurious n²·T ≈ 4.16
		// throughput factor.
		{
			const Scalar trialLum = 0.2126 * trialContribution.r
			                      + 0.7152 * trialContribution.g
			                      + 0.0722 * trialContribution.b;
			if( trialLum > 5.0 ) {
				static std::atomic<int> g_smsFireflyCount{ 0 };
				const int idx = g_smsFireflyCount.fetch_add( 1, std::memory_order_relaxed );
				if( idx < 2000 ) {
					GlobalLog()->PrintEx( eLog_Event,
						"SMS_FIREFLY[%d]: trial=%u pos=(%.4f,%.4f,%.4f) lightP=(%.4f,%.4f,%.4f) "
						"chainLen=%zu G_x_v1=%.3e detDvDy=%.3e smsGeoPre=%.3e smsGeoPost=%.3e "
						"distX_V1=%.4f cosV1atX=%.3f cosAtShade=%.3f "
						"fBSDF=(%.3e,%.3e,%.3e) attenChain=(%.3e,%.3e,%.3e) "
						"Le=(%.1f,%.1f,%.1f) mPdf=%.3e lightPdf=%.3e trialLum=%.3f "
						"trialContrib=(%.3e,%.3e,%.3e)",
						idx, trial,
						pos.x, pos.y, pos.z,
						lightSample.position.x, lightSample.position.y, lightSample.position.z,
						mResult.specularChain.size(),
						G_x_v1, detDvDy, smsGeometric, clampedGeometric,
						distXtoV1, cosV1atX, cosAtShading,
						fBSDF.r, fBSDF.g, fBSDF.b,
						mResult.contribution.r, mResult.contribution.g, mResult.contribution.b,
						actualLe.r, actualLe.g, actualLe.b,
						mResult.pdf, lightSample.pdfPosition,
						trialLum,
						trialContribution.r, trialContribution.g, trialContribution.b );
					for( std::size_t vi = 0; vi < mResult.specularChain.size(); vi++ ) {
						const ManifoldVertex& mv = mResult.specularChain[vi];
						GlobalLog()->PrintEx( eLog_Event,
							"SMS_FIREFLY[%d]:   v%zu obj=%p mat=%p pos=(%.4f,%.4f,%.4f) "
							"n=(%.3f,%.3f,%.3f) eta=%.3f isExit=%d isRefl=%d atten=(%.3f,%.3f,%.3f)",
							idx, vi, (void*)mv.pObject, (void*)mv.pMaterial,
							mv.position.x, mv.position.y, mv.position.z,
							mv.normal.x, mv.normal.y, mv.normal.z,
							mv.eta, int(mv.isExiting), int(mv.isReflection),
							mv.attenuation.r, mv.attenuation.g, mv.attenuation.b );
					}
				}
			}
		}
#endif

		totalContribution = totalContribution + trialContribution;
		acceptedGeoTerm.push_back( smsGeometric );
		acceptedPreGeo.push_back( trialContribution * ( smsGeometric > 1e-20 ? (1.0 / smsGeometric) : 0.0 ) );
		acceptedRootPositions.push_back( firstPos );
		acceptedRootReflectMasks.push_back( reflectMask );
		validTrials++;
	}

	// ------------------------------------------------------------------
	// Sum-level clamp: if the total geometric term across accepted
	// preimages exceeds config.maxGeometricTerm, scale all per-trial
	// contributions down so the sum equals the cap.  This preserves
	// the RELATIVE weighting between preimages (so the caustic "shape"
	// is still resolved) while bounding the pixel from fold-caustic
	// firefly accumulation.
	// ------------------------------------------------------------------
	if( validTrials > 0 && config.maxGeometricTerm > 0 )
	{
		Scalar sumGeoTerm = 0;
		for( Scalar g : acceptedGeoTerm ) sumGeoTerm += g;

		if( sumGeoTerm > config.maxGeometricTerm )
		{
			const Scalar scale = config.maxGeometricTerm / sumGeoTerm;
			totalContribution = RISEPel( 0, 0, 0 );
			for( std::size_t i = 0; i < acceptedPreGeo.size(); i++ )
			{
				totalContribution = totalContribution +
					acceptedPreGeo[i] * ( acceptedGeoTerm[i] * scale );
			}
		}
	}

#if SMS_TRACE_DIAGNOSTIC
	if( traceHere ) {
		GlobalLog()->PrintEx( eLog_Event,
			"SMS_TRACE: FINAL validTrials=%u acceptedRoots=%zu totalContrib=(%.4e,%.4e,%.4e)",
			validTrials, acceptedRootPositions.size(),
			totalContribution.r, totalContribution.g, totalContribution.b );
	}

	// Post-clamp firefly diagnostic: log when the FINAL per-SMS-call
	// contribution exceeds 5.0 luminance.  This catches any remaining
	// SMS-side fireflies that survive the sum-level clamp.
	{
		const Scalar totalLum = 0.2126 * totalContribution.r
		                      + 0.7152 * totalContribution.g
		                      + 0.0722 * totalContribution.b;
		if( totalLum > 5.0 ) {
			static std::atomic<int> g_smsFinalFF{ 0 };
			const int idx = g_smsFinalFF.fetch_add( 1, std::memory_order_relaxed );
			if( idx < 400 ) {
				Scalar sumGeo = 0;
				for( Scalar g : acceptedGeoTerm ) sumGeo += g;
				GlobalLog()->PrintEx( eLog_Event,
					"SMS_FINAL_FF[%d]: pos=(%.4f,%.4f,%.4f) validTrials=%u "
					"sumGeoPre=%.3e cap=%.2f totalLum=%.3f totalContrib=(%.3e,%.3e,%.3e)",
					idx, pos.x, pos.y, pos.z, validTrials,
					sumGeo, config.maxGeometricTerm, totalLum,
					totalContribution.r, totalContribution.g, totalContribution.b );
			}
		}
	}
#endif

	if( validTrials == 0 ) return result;

	// No /N division — we've deduped by root so totalContribution is the
	// sum Σ_r f_r over the distinct roots discovered by the N trials.
	// This is what we want for multi-root caustics; in the single-root
	// case (smooth specular surface, N=1) it reduces exactly to the
	// pre-multi-trial single-solve contribution.
	result.contribution = totalContribution;
	result.misWeight = 1.0;  // PDF weighting already folded into contribution
	result.valid = true;

	return result;
}

//////////////////////////////////////////////////////////////////////
// EvaluateAtShadingPointUniform
//
//   Mitsuba-faithful uniform-on-shape SMS seeding.  Iterates the
//   cached `mSpecularCasters`; per caster, draws a uniform-area
//   sample on the surface, builds a Snell-traced seed chain from
//   the shading point through the sampled point, and (if the chain
//   converges) accumulates the per-caster contribution.  Sums one
//   independent estimate per caster shape — no MIS over caster
//   choice (manifold_ss.cpp lines 32-42 explicitly disclaim
//   shape-picking).
//
//   Biased mode draws config.multiTrials uniform-area seeds per caster,
//   sums unique solved roots, and may supplement them with photon seeds.
//   Unbiased mode uses the geometric Bernoulli reciprocal-probability
//   estimator with the same uniform-area proposal in every trial. Alpha
//   coverage also selects that Bernoulli path rather than biased dedupe.
//
//   See `docs/SMS_UNIFORM_SEEDING_PLAN.md` for the full plan.
//////////////////////////////////////////////////////////////////////

ManifoldSolver::SMSContribution ManifoldSolver::EvaluateAtShadingPointUniform(
	const Point3& pos,
	const Vector3& geomNormal,
	const Vector3& shadingNormal,
	const OrthonormalBasis3D& onb,
	const IMaterial* pMaterial,
	const Vector3& woOutgoing,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	const IORStack* pIorStack, const RayIntersectionGeometric* anchorContext, bool forceLegacy
	) const
{
    // Alpha changes seed-discovery probability. Bernoulli trials repeat that
    // same proposal and account for it; heuristic deduplication does not.
    const bool alphaCoverage = caster.GetLightSampler() && caster.GetLightSampler()->SceneHasAlphaCoverage();
    const bool biased = config.biased && !alphaCoverage;
	SMSContribution result;

	if( !pMaterial ) return result;

	const IBSDF* pBSDF = pMaterial->GetBSDF();
	if( !pBSDF ) return result;

	if( mSpecularCasters.empty() && (forceLegacy || !ExtendedModeActive(scene)) ) {
		// Mitsuba-style uniform mode requires a caster list; without
		// it we have no surfaces to sample on.  Caller already enabled
		// `sms_seeding "uniform"` so silent return-zero is the right
		// behaviour (matches Mitsuba's "no caustic_caster shape" case).
		return result;
	}

	// Light sampling — same as snell mode.
	const LightSampler* pLS = caster.GetLightSampler();
	if( !pLS ) return result;

	const ILuminaryManager* pLumMgr = caster.GetLuminaries();
	LuminaryManager::LuminariesList emptyList;
	const LuminaryManager* pLumManager = SMSDynamicCast<LuminaryManager>( pLumMgr );
	const LuminaryManager::LuminariesList& luminaries = pLumManager ?
		const_cast<LuminaryManager*>(pLumManager)->getLuminaries() : emptyList;

	LightSample lightSample;
	if( !pLS->SampleLight( scene, luminaries, sampler, lightSample ) ) {
		return result;
	}

	// Env-light gate — see EvaluateAtShadingPoint (snell-mode RGB) for
	// the full rationale.  SMS doesn't handle env-disc emission as a
	// specular-chain target; skip when SampleLight returns an env sample
	// in mixed scenes (post continuous-PMF fix 2026-05-29).
	if( lightSample.pEnvLight ) {
		return result;
	}
    // Phase 3: a non-delta area emitter at an extended anchor is estimator
    // B's, under the canonical ownership predicate PT suppresses with.
    if(!forceLegacy && ExtendedModeActive(scene) && !lightSample.isDelta && lightSample.pLuminary) {
        const IORStack air(1);
        const IORStack& stack = pIorStack ? *pIorStack : air;
        const RISEPel value = EvaluateExtendedAreaReference(pos,geomNormal,shadingNormal,onb,*pMaterial,
            woOutgoing,scene,caster,sampler,lightSample,stack,anchorContext);
        result.contribution = value;
        result.valid = std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2])
            && (value[0] != 0 || value[1] != 0 || value[2] != 0);
        result.referenceA = true; // reference estimator: unclamped
        return result;
    }
    if(!forceLegacy && ExtendedModeActive(scene) && lightSample.isDelta && lightSample.pLight
        && (lightSample.pLight->lightType() == ILight::LightType::Point
            || lightSample.pLight->lightType() == ILight::LightType::Spot)) {
        const IORStack air(1);
        const IORStack& stack = pIorStack ? *pIorStack : air;
        const RISEPel value = EvaluateExtendedDelta(pos,geomNormal,shadingNormal,onb,*pMaterial,
            woOutgoing,scene,caster,sampler,lightSample,stack,anchorContext);
        result.contribution = value;
        result.valid = std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2])
            && (value[0] != 0 || value[1] != 0 || value[2] != 0);
        result.referenceA = true;
        return result;
    }


	// Sampler-dimension-drift firewall: variable-count internal work
	// (M-trial loop, Bernoulli K-loop, Solve→EstimatePDF) below uses
	// `loopSampler`; the parent sampler advances by a fixed two
	// dimensions for the SMSLoopSampler seed, leaving its LDS stream
	// predictable for downstream callers.
	SMSLoopSampler loopScope( sampler );
	ISampler& loopSampler = loopScope.sampler;

	// Cross-trial dedupe key: (first-vertex world-space position,
	// chain length, reflectMask).  Position+length alone is not
	// sufficient when multi-trial sibling chains have the SAME
	// first-vertex but DIFFERENT chain topology (e.g. one refracts
	// at v1, sibling reflects at v1) — without the mask, the sibling
	// is silently dropped as a duplicate.
	struct RootKey { Point3 pos; unsigned int chainLen; unsigned long long reflectMask; };
	std::vector<RootKey> acceptedRoots;
	acceptedRoots.reserve( mSpecularCasters.size() * 2 + 16 );

	const Scalar dedupeThr = ( config.uniquenessThreshold > 0.0 )
		? config.uniquenessThreshold : 1e-4;

	auto buildReflectMask = []( const std::vector<ManifoldVertex>& ch ) -> unsigned long long {
		unsigned long long mask = 0;
		const std::size_t k = std::min<std::size_t>( ch.size(), 64 );
		for( std::size_t i = 0; i < k; i++ ) {
			if( ch[i].isReflection ) mask |= ( 1ull << i );
		}
		return mask;
	};

	auto isDuplicate = [&]( const Point3& fp, unsigned int cl, unsigned long long mask ) -> bool {
		for( const RootKey& rk : acceptedRoots ) {
			if( rk.chainLen == cl &&
				rk.reflectMask == mask &&
				Point3Ops::Distance( rk.pos, fp ) < dedupeThr ) {
				return true;
			}
		}
		return false;
	};

	// Helper: uniform-area sample on `pCasterObj`, build a Snell-traced
	// seed chain via BuildSeedChain.  Rejects when the visibility ray
	// doesn't actually hit the sampled caster as the FIRST specular
	// hit (matches Mitsuba's `si_init.shape != shape` rejection).
	// Single-chain seed (refraction-only, deterministic).  Used by the
	// unbiased Bernoulli branch where the geometric estimator requires
	// the trial-proposal distribution to match the main solve's.
	auto buildSeedFromUniformOnCaster = [&](
		const IObject* pCasterObj,
		std::vector<ManifoldVertex>& trialSeed) -> bool
	{
		Point3 sp;
		Vector3 sn;
		Point2 sc;
		pCasterObj->UniformRandomPoint(
			&sp, &sn, &sc,
			Point3( loopSampler.Get1D(), loopSampler.Get1D(), loopSampler.Get1D() ) );
		trialSeed.clear();
		// Uniform mode: sp is the direction probe, NOT the emitter.
		// Disable BuildSeedChain's emitter-projection stop so the
		// trace can capture the full natural caustic chain instead
		// of clipping at the caster's projection.  Mitsuba's
		// `m_config.bounces` cap is approximated by maxChainDepth +
		// the non-specular-hit terminator.
		const unsigned int chainLen = BuildSeedChain(
			pos, sp, scene, caster, trialSeed,
			/*applyEmitterStop=*/ false, pIorStack, &sampler );	// DL-290
		if( chainLen == 0 || trialSeed.empty() ) return false;
		if( trialSeed[0].pObject != pCasterObj ) return false;
		return true;
	};

	// Single-chain seed: uniform-area sample on caster, then
	// `BuildSeedChainBranching` (now a thin wrapper around
	// `BuildSeedChain`) produces one Snell-traced seed chain.  Per
	// Mitsuba SOTA convention; multi-modal scenes get coverage via
	// the `multi_trials` outer loop (M independent uniform-area
	// samples per caster).
	auto buildBranchedSeedsOnCaster = [&](
		const IObject* pCasterObj,
		std::vector<SeedChainResult>& outSeeds) -> bool
	{
		Point3 sp;
		Vector3 sn;
		Point2 sc;
		pCasterObj->UniformRandomPoint(
			&sp, &sn, &sc,
			Point3( loopSampler.Get1D(), loopSampler.Get1D(), loopSampler.Get1D() ) );
		outSeeds.clear();
		BuildSeedChainBranching( pos, sp, scene, caster, loopSampler, outSeeds,
			/*applyEmitterStop=*/ false, pIorStack );	// DL-290
		// Filter to chains whose first specular hit is the sampled caster
		// (matches Mitsuba's `si_init.shape != shape` rejection).
		outSeeds.erase(
			std::remove_if( outSeeds.begin(), outSeeds.end(),
				[&]( const SeedChainResult& r ) {
					return r.chain.empty() || r.chain[0].pObject != pCasterObj;
				} ),
			outSeeds.end() );
		return !outSeeds.empty();
	};

	RISEPel totalContribution( 0, 0, 0 );
	unsigned int validContributions = 0;

	// Per-trial unclamped geometric term + pre-geometric contribution,
	// matching the snell-mode pattern at `EvaluateAtShadingPoint`
	// line ~6536-6548.  Allows applying the `config.maxGeometricTerm`
	// cap to the SUM across all unique preimages instead of to each
	// trial individually.  Per-trial clamping (the prior behaviour)
	// would let a fold caustic with N preimages emit up to
	// N × maxGeometricTerm; sum-level clamp bounds total at one cap.
	std::vector<Scalar>  acceptedGeoTerm;   ///< unclamped G_x_v1 × |det dv/dy|
	std::vector<RISEPel> acceptedPreGeo;    ///< trialContribution / smsGeoUsed (post proposalPdf division)

	// Per-caster Mitsuba-style sum (manifold_ss / manifold_ms iterate
	// every caster and accrue one independent estimate per shape).
	for( const IObject* pCasterObj : mSpecularCasters )
	{
		if( !pCasterObj ) continue;

		if( biased )
		{
			// M-trial biased mode (Zeltner 2020 §4.3 Algorithm 3 / Eq. 8;
			// Mitsuba `manifold_ss.cpp:142-197`).  Per caster, run M
			// independent uniform-area samples; accept each unique
			// converged solution; sum unweighted.  Cross-caster dedupe
			// key is (first-vertex-pos, chainLen, reflectMask) so a chain
			// rediscovered by a sibling caster contributes exactly once
			// AND a multi-trial sibling that flipped reflect/refract on
			// a later vertex counts as distinct (see commit e853ab8).
			//
			// Each `buildBranchedSeedsOnCaster` call produces 0 or 1
			// seed chains (path-tree branching was excised in 2026-05;
			// the legacy multi-output shape is preserved for ABI
			// compatibility).  The `proposalPdf` is always 1.0 so the
			// `1/proposalPdf` divide is a no-op.
			const unsigned int M = std::max( config.multiTrials, 1u );

			for( unsigned int m = 0; m < M; m++ )
			{
				std::vector<SeedChainResult> seeds;
				if( !buildBranchedSeedsOnCaster( pCasterObj, seeds ) ) continue;

				for( SeedChainResult& seedResult : seeds )
				{
					ManifoldResult mResult = Solve(
						pos, shadingNormal,
						lightSample.position, lightSample.normal,
						seedResult.chain, loopSampler );
					if( !mResult.valid ) continue;

					const Point3 firstPos = mResult.specularChain[0].position;
					const unsigned int chainLen = static_cast<unsigned int>( mResult.specularChain.size() );
					if( isDuplicate( firstPos, chainLen, buildReflectMask( mResult.specularChain ) ) ) continue;

					Vector3 trialDir;
					RISEPel trialContrib;
					Scalar smsGeometric = 0;
					if( !ComputeTrialContribution( pos, geomNormal, shadingNormal, onb, woOutgoing,
						pBSDF, lightSample, mResult, caster, trialDir, trialContrib,
						/*clampGeometric=*/ false, &smsGeometric, pIorStack, &sampler ) )	// DL-290: the receiver's live stack
						continue;

					if( seedResult.proposalPdf > 1e-20 ) {
						trialContrib = trialContrib * ( 1.0 / seedResult.proposalPdf );
					}

					totalContribution = totalContribution + trialContrib;
					acceptedGeoTerm.push_back( smsGeometric );
					acceptedPreGeo.push_back( trialContrib *
						( smsGeometric > 1e-20 ? (1.0 / smsGeometric) : 0.0 ) );
					acceptedRoots.push_back( RootKey{ firstPos, chainLen, buildReflectMask( mResult.specularChain ) } );
					validContributions++;
				}
			}
		}
		else
		{
			// Unbiased mode: 1 main trial + geometric-distribution
			// Bernoulli loop (`K = first-success-index`, `E[K] = 1/p`,
			// contribution scaled by K).  This is the strict Mitsuba-
			// faithful Bernoulli estimator (Zeltner 2020 §4.3 Algorithm
			// 2): for the K count to correspond to 1/p_uniform,
			// EVERY trial in the K-loop must use the SAME proposal
			// distribution as the main solve — uniform-area sampling
			// on the caster shape.  Mixing in photon-aided seeds
			// would bias the estimator because their proposal is
			// different (light-emit-trace + kd-tree query, not
			// uniform-on-shape).
			//
			// Snell mode (`EvaluateAtShadingPoint`) IS a different
			// estimator: its unbiased path uses `EstimatePDF`'s
			// tangent-perturbation basin-width heuristic, which is
			// proposal-agnostic and therefore can integrate any seed
			// source including photons.  See `docs/SMS_PHOTON_REGIME.md`
			// "Mode/photon asymmetry" for the analysis.
			//
			// Practical: if you need photons + unbiased, use
			// `sms_seeding "snell"`.  If you need strict Mitsuba
			// uniform-mode unbiased + photons, switch to biased
			// (the photon-aided extension below at line ~7421
			// runs in biased uniform mode).
			std::vector<ManifoldVertex> trialSeed;
			if( !buildSeedFromUniformOnCaster( pCasterObj, trialSeed ) ) continue;

			ManifoldResult mResult = Solve(
				pos, shadingNormal,
				lightSample.position, lightSample.normal,
				trialSeed, loopSampler );
			if( !mResult.valid ) continue;

			const Point3 firstPos = mResult.specularChain[0].position;
			const unsigned int chainLen = static_cast<unsigned int>( mResult.specularChain.size() );
			if( isDuplicate( firstPos, chainLen, buildReflectMask( mResult.specularChain ) ) ) continue;

			Vector3 dirMain;
			RISEPel mainContrib;
			if( !ComputeTrialContribution( pos, geomNormal, shadingNormal, onb, woOutgoing,
				pBSDF, lightSample, mResult, caster, dirMain, mainContrib,
				/*clampGeometric=*/ true, nullptr, pIorStack, &sampler ) )	// DL-290: the receiver's live stack
				continue;

			// Geometric Bernoulli K-loop.  Cap on `maxBernoulliTrials`,
			// hard-cap fallback at 1024 if config is 0 (prevents render
			// hangs on casters Newton can never re-discover).
			unsigned int K = 1;
            SMSReciprocalTail alphaTail(config.maxBernoulliTrials);
			bool capHit = false;
			const unsigned int hardCap = config.maxBernoulliTrials > 0
				? config.maxBernoulliTrials : 1024u;

			while( true )
			{
				Point3 sp_t;
				Vector3 sn_t;
				Point2 sc_t;
				pCasterObj->UniformRandomPoint(
					&sp_t, &sn_t, &sc_t,
					Point3( loopSampler.Get1D(), loopSampler.Get1D(), loopSampler.Get1D() ) );

				std::vector<ManifoldVertex> trialChain;
				bool match = false;
				if( BuildSeedChain( pos, sp_t, scene, caster, trialChain,
						/*applyEmitterStop=*/ false, pIorStack, &sampler ) > 0 &&	// DL-290
					!trialChain.empty() && trialChain[0].pObject == pCasterObj )
				{
					ManifoldResult tResult = Solve(
						pos, shadingNormal,
						lightSample.position, lightSample.normal,
						trialChain, loopSampler );
					if( tResult.valid ) {
						Vector3 dirT = Vector3Ops::mkVector3(
							tResult.specularChain[0].position, pos );
						if( Vector3Ops::NormalizeMag( dirT ) > 1e-8 ) {
							const Scalar dotProd = Vector3Ops::Dot( dirMain, dirT );
							if( std::fabs( dotProd - 1.0 ) < dedupeThr ) {
								match = true;
							}
						}
					}
				}

				if( match ) break;
                if (alphaCoverage) {
                    if (!alphaTail.ContinueAfterFailure(sampler)) break;
                    continue;
                }
				K++;
				if( K > hardCap ) {
					capHit = true;
					break;
				}
			}

			if( capHit ) continue;   // bias toward zero when cap fires

			mainContrib = mainContrib * (alphaCoverage ? alphaTail.Estimate() : static_cast<Scalar>( K ));
			totalContribution = totalContribution + mainContrib;
			acceptedRoots.push_back( RootKey{ firstPos, chainLen, buildReflectMask( mResult.specularChain ) } );
			validContributions++;
		}
	}

	// Photon-aided trial extension (biased mode only).
	// Weisstein, Jhang, Chang. "Photon-Driven Manifold Sampling."
	// HPG 2024. DOI 10.1145/3675375.  Each photon's recorded chain is
	// reversed (light→diffuse → receiver→light) via the helper, run
	// through Newton, deduped against the per-caster set, and summed
	// unweighted (paper Eq. 8 form: `Σ_l f(x₂⁽ˡ⁾)` is consistent for
	// any seed distribution that covers basins with positive density).
	if( biased && pPhotonMap && pPhotonMap->IsBuilt() )
	{
		Scalar r = config.photonSearchRadius;
		if( r <= 0 ) {
			r = pPhotonMap->GetAutoRadius();
		}

		if( r > 0 )
		{
			std::vector<SMSPhoton> photonSeeds;
			pPhotonMap->QuerySeeds( pos, r * r, photonSeeds );
			RandomSubsamplePhotonSeeds( photonSeeds,
				config.maxPhotonSeedsPerShadingPoint, loopSampler );

			for( const SMSPhoton& ph : photonSeeds )
			{
				std::vector<ManifoldVertex> photonChain;
				if( ReversePhotonChainForSeed( ph, photonChain ) == 0 ) continue;

				// TARGET BOUNCES: Mitsuba-parity exact-length requirement.
				// Photon chains have lengths determined at photon-trace time
				// from random scattering — many won't match the user-
				// specified target K.  Reject mismatches here so the photon
				// path is consistent with snell + uniform seeds (which both
				// enforce length == K via the BuildSeedChain post-check).
				if( config.targetBounces > 0 &&
					photonChain.size() != config.targetBounces )
				{
					continue;
				}

				ManifoldResult mResult = Solve(
					pos, shadingNormal,
					lightSample.position, lightSample.normal,
					photonChain, loopSampler );
				if( !mResult.valid ) continue;

				const Point3 firstPos = mResult.specularChain[0].position;
				const unsigned int chainLen = static_cast<unsigned int>( mResult.specularChain.size() );
				if( isDuplicate( firstPos, chainLen, buildReflectMask( mResult.specularChain ) ) ) continue;

				Vector3 trialDir;
				RISEPel trialContrib;
				Scalar smsGeometric = 0;
				if( !ComputeTrialContribution( pos, geomNormal, shadingNormal, onb, woOutgoing,
					pBSDF, lightSample, mResult, caster, trialDir, trialContrib,
					/*clampGeometric=*/ false, &smsGeometric, pIorStack, &sampler ) )	// DL-290: the receiver's live stack
					continue;

				totalContribution = totalContribution + trialContrib;
				acceptedGeoTerm.push_back( smsGeometric );
				acceptedPreGeo.push_back( trialContrib *
					( smsGeometric > 1e-20 ? (1.0 / smsGeometric) : 0.0 ) );
				acceptedRoots.push_back( RootKey{ firstPos, chainLen, buildReflectMask( mResult.specularChain ) } );
				validContributions++;
			}
		}
	}

	if( validContributions == 0 ) return result;

	// Sum-level geometric clamp.  Mirror snell-mode's pattern at line
	// ~7050: if total geometric term across all unique preimages
	// exceeds `config.maxGeometricTerm`, scale every per-trial
	// contribution down so the sum equals the cap.  Preserves the
	// relative weighting between preimages (caustic shape is still
	// resolved) while bounding the pixel from fold-caustic firefly
	// accumulation.  Without this, a fold caustic with N preimages
	// could emit up to `N × maxGeometricTerm`.
	if( config.maxGeometricTerm > 0 )
	{
		Scalar sumGeoTerm = 0;
		for( Scalar g : acceptedGeoTerm ) sumGeoTerm += g;

		if( sumGeoTerm > config.maxGeometricTerm )
		{
			const Scalar scale = config.maxGeometricTerm / sumGeoTerm;
			totalContribution = RISEPel( 0, 0, 0 );
			for( std::size_t i = 0; i < acceptedPreGeo.size(); i++ )
			{
				totalContribution = totalContribution +
					acceptedPreGeo[i] * ( acceptedGeoTerm[i] * scale );
			}
		}
	}

	result.contribution = totalContribution;
	result.misWeight = 1.0;
	result.valid = true;
	return result;
}

//////////////////////////////////////////////////////////////////////
// EvaluateAtShadingPointNMUniform
//
//   Spectral counterpart of EvaluateAtShadingPointUniform.
//   Mirrors the RGB structure: per-caster Mitsuba-style sum, M-trial
//   biased mode with cross-trial dedupe by (first-vertex-pos,
//   chainLen), geometric Bernoulli when biased=false, photon-aided
//   trial extension on biased mode.  Per-vertex eta is overridden
//   with `GetSpecularInfoNM(nm)` AFTER the chain is built and BEFORE
//   `Solve` runs so dispersive glass converges to the wavelength-
//   specific caustic root.
//////////////////////////////////////////////////////////////////////

ManifoldSolver::SMSContributionNM ManifoldSolver::EvaluateAtShadingPointNMUniform(
	const Point3& pos,
	const Vector3& geomNormal,
	const Vector3& shadingNormal,
	const OrthonormalBasis3D& onb,
	const IMaterial* pMaterial,
	const Vector3& woOutgoing,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	const Scalar nm,
	const IORStack* pIorStack, const RayIntersectionGeometric* anchorContext, bool forceLegacy
	) const
{
    // Alpha changes seed-discovery probability. Bernoulli trials repeat that
    // same proposal and account for it; heuristic deduplication does not.
    const bool alphaCoverage = caster.GetLightSampler() && caster.GetLightSampler()->SceneHasAlphaCoverage();
    const bool biased = config.biased && !alphaCoverage;
	SMSContributionNM result;

	if( !pMaterial ) return result;

	const IBSDF* pBSDF = pMaterial->GetBSDF();
	if( !pBSDF ) return result;

	if( mSpecularCasters.empty() && (forceLegacy || !ExtendedModeActive(scene)) ) return result;

	const LightSampler* pLS = caster.GetLightSampler();
	if( !pLS ) return result;

	const ILuminaryManager* pLumMgr = caster.GetLuminaries();
	LuminaryManager::LuminariesList emptyList;
	const LuminaryManager* pLumManager = SMSDynamicCast<LuminaryManager>( pLumMgr );
	const LuminaryManager::LuminariesList& luminaries = pLumManager ?
		const_cast<LuminaryManager*>(pLumManager)->getLuminaries() : emptyList;

	LightSample lightSample;
	if( !pLS->SampleLight( scene, luminaries, sampler, lightSample ) ) {
		return result;
	}

	// Env-light gate — see EvaluateAtShadingPoint (snell-mode RGB) for
	// the full rationale.  SMS doesn't handle env-disc emission as a
	// specular-chain target; skip when SampleLight returns an env sample
	// in mixed scenes (post continuous-PMF fix 2026-05-29).
	if( lightSample.pEnvLight ) {
		return result;
	}
    // Phase 3: a non-delta area emitter at an extended anchor is estimator
    // B's, under the canonical ownership predicate PT suppresses with.
    if(!forceLegacy && ExtendedModeActive(scene) && !lightSample.isDelta && lightSample.pLuminary) {
        const IORStack air(1);
        const IORStack& stack = pIorStack ? *pIorStack : air;
        const RISEPel value = EvaluateExtendedAreaReference(pos,geomNormal,shadingNormal,onb,*pMaterial,
            woOutgoing,scene,caster,sampler,lightSample,stack,anchorContext,nm);
        result.contribution = value[0];
        result.valid = std::isfinite(value[0]) && value[0] != 0;
        result.referenceA = true; // reference estimator: unclamped
        return result;
    }
    if(!forceLegacy && ExtendedModeActive(scene) && lightSample.isDelta && lightSample.pLight
        && (lightSample.pLight->lightType() == ILight::LightType::Point
            || lightSample.pLight->lightType() == ILight::LightType::Spot)) {
        const IORStack air(1);
        const IORStack& stack = pIorStack ? *pIorStack : air;
        const RISEPel value = EvaluateExtendedDelta(pos,geomNormal,shadingNormal,onb,*pMaterial,
            woOutgoing,scene,caster,sampler,lightSample,stack,anchorContext,nm);
        result.contribution = value[0];
        result.valid = std::isfinite(value[0]) && value[0] != 0;
        result.referenceA = true;
        return result;
    }


	// Sampler-dimension-drift firewall — see EvaluateAtShadingPointUniform
	// for the full rationale.
	SMSLoopSampler loopScope( sampler );
	ISampler& loopSampler = loopScope.sampler;

	// Cross-trial dedupe key: (first-vertex pos, chainLen, reflectMask).
	// See RGB variant for the full rationale.
	struct RootKey { Point3 pos; unsigned int chainLen; unsigned long long reflectMask; };
	std::vector<RootKey> acceptedRoots;
	acceptedRoots.reserve( mSpecularCasters.size() * 2 + 16 );

	const Scalar dedupeThr = ( config.uniquenessThreshold > 0.0 )
		? config.uniquenessThreshold : 1e-4;

	auto buildReflectMask = []( const std::vector<ManifoldVertex>& ch ) -> unsigned long long {
		unsigned long long mask = 0;
		const std::size_t k = std::min<std::size_t>( ch.size(), 64 );
		for( std::size_t i = 0; i < k; i++ ) {
			if( ch[i].isReflection ) mask |= ( 1ull << i );
		}
		return mask;
	};

	auto isDuplicate = [&]( const Point3& fp, unsigned int cl, unsigned long long mask ) -> bool {
		for( const RootKey& rk : acceptedRoots ) {
			if( rk.chainLen == cl &&
				rk.reflectMask == mask &&
				Point3Ops::Distance( rk.pos, fp ) < dedupeThr ) {
				return true;
			}
		}
		return false;
	};

	// Per-wavelength eta override: re-query each vertex's material with
	// `GetSpecularInfoNM` so the chain `Solve` converges to the correct
	// wavelength-specific caustic root (essential for dispersive glass).
	auto applyNMEtaToChain = [&]( std::vector<ManifoldVertex>& chain ) {
		IORStack queryIor( 1.0 );
		for( ManifoldVertex& v : chain ) {
			if( v.pMaterial ) {
				Ray dummyRay( v.position, v.normal );
				RayIntersectionGeometric rigLocal( dummyRay, nullRasterizerState );
				rigLocal.bHit          = true;
				rigLocal.ptIntersection = v.position;
				rigLocal.vNormal       = v.normal;
				rigLocal.vGeomNormal   = v.geomNormal;
				rigLocal.ptCoord       = v.uv;
				rigLocal.ptObjIntersec = v.objectPosition;
				SpecularInfo specNM = v.pMaterial->GetSpecularInfoNM( rigLocal, queryIor, nm );
				v.eta         = specNM.ior;
				// DL-353: the solve reads the explicit interface pair, not
				// the legacy single-index field. Match companion replay:
				// replace the material side; retain the seeded exterior.
				if( v.isExiting ) v.etaI = specNM.ior;
				else v.etaT = specNM.ior;
				v.attenuation = specNM.attenuation;
				v.attenuationNM = specNM.attenuationNM;
				v.attenuationAppliesToReflection = specNM.attenuationAppliesToReflection;
				v.hasCustomSpecularFresnel = specNM.hasCustomSpecularFresnel;
				v.attenuationIsInteriorTransmittance = specNM.attenuationIsInteriorTransmittance;
				v.canRefract  = specNM.canRefract;
			}
			v.valid = false;
		}
	};

	auto buildSeedFromUniformOnCaster = [&](
		const IObject* pCasterObj,
		std::vector<ManifoldVertex>& trialSeed) -> bool
	{
		Point3 sp;
		Vector3 sn;
		Point2 sc;
		pCasterObj->UniformRandomPoint(
			&sp, &sn, &sc,
			Point3( loopSampler.Get1D(), loopSampler.Get1D(), loopSampler.Get1D() ) );
		trialSeed.clear();
		// Uniform mode — see RGB variant comment.  applyEmitterStop=false.
		const unsigned int chainLen = BuildSeedChain(
			pos, sp, scene, caster, trialSeed,
			/*applyEmitterStop=*/ false, pIorStack, &sampler );	// DL-290
		if( chainLen == 0 || trialSeed.empty() ) return false;
		if( trialSeed[0].pObject != pCasterObj ) return false;
		applyNMEtaToChain( trialSeed );
		return true;
	};

	// Branched seed (PT-faithful split semantic) — see RGB variant
	// for the full rationale.
	auto buildBranchedSeedsOnCaster = [&](
		const IObject* pCasterObj,
		std::vector<SeedChainResult>& outSeeds) -> bool
	{
		Point3 sp;
		Vector3 sn;
		Point2 sc;
		pCasterObj->UniformRandomPoint(
			&sp, &sn, &sc,
			Point3( loopSampler.Get1D(), loopSampler.Get1D(), loopSampler.Get1D() ) );
		outSeeds.clear();
		BuildSeedChainBranching( pos, sp, scene, caster, loopSampler, outSeeds,
			/*applyEmitterStop=*/ false, pIorStack );	// DL-290
		outSeeds.erase(
			std::remove_if( outSeeds.begin(), outSeeds.end(),
				[&]( const SeedChainResult& r ) {
					return r.chain.empty() || r.chain[0].pObject != pCasterObj;
				} ),
			outSeeds.end() );
		// Per-wavelength eta override on each chain so dispersive
		// caustics converge to the correct wavelength-specific root.
		for( SeedChainResult& r : outSeeds ) {
			applyNMEtaToChain( r.chain );
		}
		return !outSeeds.empty();
	};

	Scalar totalContribution = 0.0;
	unsigned int validContributions = 0;

	// Per-trial unclamped geometric term + pre-geometric contribution
	// for sum-level clamp (see RGB EvaluateAtShadingPointUniform for
	// the rationale).
	std::vector<Scalar> acceptedGeoTerm;
	std::vector<Scalar> acceptedPreGeo;

	for( const IObject* pCasterObj : mSpecularCasters )
	{
		if( !pCasterObj ) continue;

		if( biased )
		{
			const unsigned int M = std::max( config.multiTrials, 1u );

			for( unsigned int m = 0; m < M; m++ )
			{
				std::vector<SeedChainResult> seeds;
				if( !buildBranchedSeedsOnCaster( pCasterObj, seeds ) ) continue;

				for( SeedChainResult& seedResult : seeds )
				{
					ManifoldResult mResult = Solve(
						pos, shadingNormal,
						lightSample.position, lightSample.normal,
						seedResult.chain, loopSampler );
					if( !mResult.valid ) continue;

					const Point3 firstPos = mResult.specularChain[0].position;
					const unsigned int chainLen = static_cast<unsigned int>( mResult.specularChain.size() );
					if( isDuplicate( firstPos, chainLen, buildReflectMask( mResult.specularChain ) ) ) continue;

					Vector3 trialDir;
					Scalar trialContrib;
					Scalar smsGeometric = 0;
					if( !ComputeTrialContributionNM( pos, geomNormal, shadingNormal, onb, woOutgoing,
						pBSDF, lightSample, mResult, caster, nm, trialDir, trialContrib,
						/*clampGeometric=*/ false, &smsGeometric, pIorStack, &sampler ) )	// DL-290: the receiver's live stack
						continue;

					if( seedResult.proposalPdf > 1e-20 ) {
						trialContrib *= ( 1.0 / seedResult.proposalPdf );
					}

					totalContribution += trialContrib;
					acceptedGeoTerm.push_back( smsGeometric );
					acceptedPreGeo.push_back(
						smsGeometric > 1e-20 ? ( trialContrib / smsGeometric ) : 0.0 );
					acceptedRoots.push_back( RootKey{ firstPos, chainLen, buildReflectMask( mResult.specularChain ) } );
					validContributions++;
				}
			}
		}
		else
		{
			// Unbiased mode (NM): same strict-Mitsuba Bernoulli K-loop
			// as the RGB variant.  Photon-aided seeds are deliberately
			// skipped because their proposal distribution doesn't match
			// uniform-on-shape — see the RGB variant's full comment for
			// the math + asymmetry vs. snell mode's basin-width
			// heuristic estimator.
			std::vector<ManifoldVertex> trialSeed;
			if( !buildSeedFromUniformOnCaster( pCasterObj, trialSeed ) ) continue;

			ManifoldResult mResult = Solve(
				pos, shadingNormal,
				lightSample.position, lightSample.normal,
				trialSeed, loopSampler );
			if( !mResult.valid ) continue;

			const Point3 firstPos = mResult.specularChain[0].position;
			const unsigned int chainLen = static_cast<unsigned int>( mResult.specularChain.size() );
			if( isDuplicate( firstPos, chainLen, buildReflectMask( mResult.specularChain ) ) ) continue;

			Vector3 dirMain;
			Scalar mainContrib;
			if( !ComputeTrialContributionNM( pos, geomNormal, shadingNormal, onb, woOutgoing,
				pBSDF, lightSample, mResult, caster, nm, dirMain, mainContrib,
				/*clampGeometric=*/ true, nullptr, pIorStack, &sampler ) )	// DL-290: the receiver's live stack
				continue;

			unsigned int K = 1;
            SMSReciprocalTail alphaTail(config.maxBernoulliTrials);
			bool capHit = false;
			const unsigned int hardCap = config.maxBernoulliTrials > 0
				? config.maxBernoulliTrials : 1024u;

			while( true )
			{
				Point3 sp_t;
				Vector3 sn_t;
				Point2 sc_t;
				pCasterObj->UniformRandomPoint(
					&sp_t, &sn_t, &sc_t,
					Point3( loopSampler.Get1D(), loopSampler.Get1D(), loopSampler.Get1D() ) );

				std::vector<ManifoldVertex> trialChain;
				bool match = false;
				if( BuildSeedChain( pos, sp_t, scene, caster, trialChain,
						/*applyEmitterStop=*/ false, pIorStack, &sampler ) > 0 &&	// DL-290
					!trialChain.empty() && trialChain[0].pObject == pCasterObj )
				{
					applyNMEtaToChain( trialChain );
					ManifoldResult tResult = Solve(
						pos, shadingNormal,
						lightSample.position, lightSample.normal,
						trialChain, loopSampler );
					if( tResult.valid ) {
						Vector3 dirT = Vector3Ops::mkVector3(
							tResult.specularChain[0].position, pos );
						if( Vector3Ops::NormalizeMag( dirT ) > 1e-8 ) {
							const Scalar dotProd = Vector3Ops::Dot( dirMain, dirT );
							if( std::fabs( dotProd - 1.0 ) < dedupeThr ) {
								match = true;
							}
						}
					}
				}

				if( match ) break;
                if (alphaCoverage) {
                    if (!alphaTail.ContinueAfterFailure(sampler)) break;
                    continue;
                }
				K++;
				if( K > hardCap ) {
					capHit = true;
					break;
				}
			}

			if( capHit ) continue;

			mainContrib *= alphaCoverage ? alphaTail.Estimate() : static_cast<Scalar>( K );
			totalContribution += mainContrib;
			acceptedRoots.push_back( RootKey{ firstPos, chainLen, buildReflectMask( mResult.specularChain ) } );
			validContributions++;
		}
	}

	// Photon-aided trial extension (biased only).  Photon chain is
	// reversed via the helper; per-vertex NM eta is then re-applied
	// for dispersion correctness before Solve.
	if( biased && pPhotonMap && pPhotonMap->IsBuilt() )
	{
		Scalar r = config.photonSearchRadius;
		if( r <= 0 ) {
			r = pPhotonMap->GetAutoRadius();
		}

		if( r > 0 )
		{
			std::vector<SMSPhoton> photonSeeds;
			pPhotonMap->QuerySeeds( pos, r * r, photonSeeds );
			RandomSubsamplePhotonSeeds( photonSeeds,
				config.maxPhotonSeedsPerShadingPoint, loopSampler );

			for( const SMSPhoton& ph : photonSeeds )
			{
				std::vector<ManifoldVertex> photonChain;
				if( ReversePhotonChainForSeed( ph, photonChain ) == 0 ) continue;

				// TARGET BOUNCES: see RGB EvaluateAtShadingPointUniform
				// for the rationale.  Mirror filter for the spectral path.
				if( config.targetBounces > 0 &&
					photonChain.size() != config.targetBounces )
				{
					continue;
				}

				applyNMEtaToChain( photonChain );

				ManifoldResult mResult = Solve(
					pos, shadingNormal,
					lightSample.position, lightSample.normal,
					photonChain, loopSampler );
				if( !mResult.valid ) continue;

				const Point3 firstPos = mResult.specularChain[0].position;
				const unsigned int chainLen = static_cast<unsigned int>( mResult.specularChain.size() );
				if( isDuplicate( firstPos, chainLen, buildReflectMask( mResult.specularChain ) ) ) continue;

				Vector3 trialDir;
				Scalar trialContrib;
				Scalar smsGeometric = 0;
				if( !ComputeTrialContributionNM( pos, geomNormal, shadingNormal, onb, woOutgoing,
					pBSDF, lightSample, mResult, caster, nm, trialDir, trialContrib,
					/*clampGeometric=*/ false, &smsGeometric, pIorStack, &sampler ) )	// DL-290: the receiver's live stack
					continue;

				totalContribution += trialContrib;
				acceptedGeoTerm.push_back( smsGeometric );
				acceptedPreGeo.push_back(
					smsGeometric > 1e-20 ? ( trialContrib / smsGeometric ) : 0.0 );
				acceptedRoots.push_back( RootKey{ firstPos, chainLen, buildReflectMask( mResult.specularChain ) } );
				validContributions++;
			}
		}
	}

	if( validContributions == 0 ) return result;

	// Sum-level geometric clamp (NM/spectral; mirrors RGB
	// EvaluateAtShadingPointUniform).
	if( config.maxGeometricTerm > 0 )
	{
		Scalar sumGeoTerm = 0;
		for( Scalar g : acceptedGeoTerm ) sumGeoTerm += g;

		if( sumGeoTerm > config.maxGeometricTerm )
		{
			const Scalar scale = config.maxGeometricTerm / sumGeoTerm;
			totalContribution = 0.0;
			for( std::size_t i = 0; i < acceptedPreGeo.size(); i++ )
			{
				totalContribution += acceptedPreGeo[i] * ( acceptedGeoTerm[i] * scale );
			}
		}
	}

	result.contribution = totalContribution;
	result.misWeight = 1.0;
	result.valid = true;
	return result;
}

//////////////////////////////////////////////////////////////////////
// EvaluateAtShadingPointNM
//
//   Spectral variant of EvaluateAtShadingPoint.
//   Uses per-wavelength IOR for dispersion and scalar evaluation
//   throughout.
//////////////////////////////////////////////////////////////////////

namespace {
void RefreshLegacySeedNM(std::vector<ManifoldVertex>& chain,Scalar nm) {

	if( nm > 0 )
	{
		// Override each vertex's IOR with the wavelength-dependent value.
		// This is what makes dispersion work — the Newton solver will find
		// a different position for each wavelength due to the different IOR.
		IORStack queryIor( 1.0 );
		for( unsigned int i = 0; i < chain.size(); i++ )
		{
			if( chain[i].pMaterial )
			{
				// Build a minimal RayIntersectionGeometric for the query
				Ray dummyRay( chain[i].position, chain[i].normal );
				RayIntersectionGeometric rig( dummyRay, nullRasterizerState );
				rig.bHit = true;
				rig.ptIntersection = chain[i].position;
				rig.vNormal = chain[i].normal;
				rig.vGeomNormal = chain[i].geomNormal;
				rig.ptCoord = chain[i].uv;
				rig.ptObjIntersec = chain[i].objectPosition;

				SpecularInfo specNM = chain[i].pMaterial->GetSpecularInfoNM(
					rig, queryIor, nm );
				chain[i].eta = specNM.ior;
				chain[i].attenuationNM = specNM.attenuationNM;
				chain[i].attenuationAppliesToReflection = specNM.attenuationAppliesToReflection;
				chain[i].hasCustomSpecularFresnel = specNM.hasCustomSpecularFresnel;
				chain[i].attenuationIsInteriorTransmittance = specNM.attenuationIsInteriorTransmittance;
				// Also update the wavelength-dependent side of the
				// (etaI, etaT) pair populated by BuildSeedChain.  The
				// vertex's "outgoing-medium IOR" for entering, or
				// "incoming-medium IOR" for exiting, IS the surface
				// material's IOR — which is what specNM.ior gives us
				// per wavelength (dispersion).  The OPPOSITE side's IOR
				// (the surrounding medium) is left as set by the RGB
				// BuildSeedChain pass; for typical SMS scenes (single
				// dielectric in air, surrounding = 1.0) this is
				// wavelength-independent and correct.  For doubly-
				// nested dispersive scenes (e.g. dispersive-glass inside
				// dispersive-glass) the surrounding side would also be
				// wavelength-dependent and needs a separate per-vertex
				// stack-of-NM-IORs to track exactly — left for a future
				// extension when such a scene exists.
				if( chain[i].isExiting ) {
					chain[i].etaI = specNM.ior;
				} else {
					chain[i].etaT = specNM.ior;
				}
			}
		}
	}

}
}
ManifoldSolver::SMSContributionNM ManifoldSolver::EvaluateAtShadingPointNM(
	const Point3& pos,
	const Vector3& geomNormal,
	const Vector3& shadingNormal,
	const OrthonormalBasis3D& onb,
	const IMaterial* pMaterial,
	const Vector3& woOutgoing,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	const Scalar nm,
	const IORStack* pIorStack, const RayIntersectionGeometric* anchorContext, bool forceLegacy
	) const
{
	// Mitsuba-faithful uniform-on-shape seeding (opt-in via
	// `sms_seeding "uniform"`).  Spectral-path counterpart of
	// `EvaluateAtShadingPointUniform`.
	if( config.seedingMode == ManifoldSolverConfig::eSeedingUniform ||
        (caster.GetLightSampler() && caster.GetLightSampler()->SceneHasAlphaCoverage()) )
	{
		// DL-290: forward the live stack (see the RGB dispatch above).
		return EvaluateAtShadingPointNMUniform(
			pos, geomNormal, shadingNormal, onb, pMaterial, woOutgoing,
			scene, caster, sampler, nm, pIorStack, anchorContext, forceLegacy );
	}

	SMSContributionNM result;

	if( !pMaterial ) return result;

	const IBSDF* pBSDF = pMaterial->GetBSDF();
	if( !pBSDF ) return result;

	// Use the caster's prepared LightSampler
	const LightSampler* pLS = caster.GetLightSampler();
	if( !pLS ) return result;

	const ILuminaryManager* pLumMgr = caster.GetLuminaries();
	LuminaryManager::LuminariesList emptyList;
	const LuminaryManager* pLumManager = SMSDynamicCast<LuminaryManager>( pLumMgr );
	const LuminaryManager::LuminariesList& luminaries = pLumManager ?
		const_cast<LuminaryManager*>(pLumManager)->getLuminaries() : emptyList;

	LightSample lightSample;
	if( !pLS->SampleLight( scene, luminaries, sampler, lightSample ) )
		return result;

	// Env-light gate — see EvaluateAtShadingPoint (snell-mode RGB) for
	// the full rationale.  SMS doesn't handle env-disc emission as a
	// specular-chain target; skip when SampleLight returns an env sample
	// in mixed scenes (post continuous-PMF fix 2026-05-29).
	if( lightSample.pEnvLight ) {
		return result;
	}
    // Phase 3: a non-delta area emitter at an extended anchor is estimator
    // B's, under the canonical ownership predicate PT suppresses with.
    if(!forceLegacy && ExtendedModeActive(scene) && !lightSample.isDelta && lightSample.pLuminary) {
        const IORStack air(1);
        const IORStack& stack = pIorStack ? *pIorStack : air;
        const RISEPel value = EvaluateExtendedAreaReference(pos,geomNormal,shadingNormal,onb,*pMaterial,
            woOutgoing,scene,caster,sampler,lightSample,stack,anchorContext,nm);
        result.contribution = value[0];
        result.valid = std::isfinite(value[0]) && value[0] != 0;
        result.referenceA = true; // reference estimator: unclamped
        return result;
    }
    if(!forceLegacy && ExtendedModeActive(scene) && lightSample.isDelta && lightSample.pLight
        && (lightSample.pLight->lightType() == ILight::LightType::Point
            || lightSample.pLight->lightType() == ILight::LightType::Spot)) {
        const IORStack air(1);
        const IORStack& stack = pIorStack ? *pIorStack : air;
        const RISEPel value = EvaluateExtendedDelta(pos,geomNormal,shadingNormal,onb,*pMaterial,
            woOutgoing,scene,caster,sampler,lightSample,stack,anchorContext,nm);
        result.contribution = value[0];
        result.valid = std::isfinite(value[0]) && value[0] != 0;
        result.referenceA = true;
        return result;
    }


	// Sampler-dimension-drift firewall — see EvaluateAtShadingPointUniform.
	SMSLoopSampler loopScope( sampler );
	ISampler& loopSampler = loopScope.sampler;

	// Build seed chain toward light, with fallbacks (see RGB variant), and
	// the per-wavelength eta override -- shared with PT's DL-372 split-
	// suppression classifier via BuildSnellBaseSeed.
	std::vector<ManifoldVertex> seedChain;
	const unsigned int chainLen = BuildSnellBaseSeed(
		pos, geomNormal, lightSample.position, scene, caster,
		seedChain, pIorStack, &sampler, nm );

    std::vector<std::vector<ManifoldVertex>> baseSeeds;
    if(chainLen>0&&!seedChain.empty()) baseSeeds.push_back(seedChain);
    for(const IObject* mirror:mSpecularCasters) {
        if(!ProbeIsPureMirrorCaster(mirror)) continue;
        for(unsigned m=0;m<std::max(config.multiTrials,1u);++m) {
            Point3 point;Vector3 normal;Point2 uv;
            mirror->UniformRandomPoint(&point,&normal,&uv,
                Point3(loopSampler.Get1D(),loopSampler.Get1D(),loopSampler.Get1D()));
            std::vector<ManifoldVertex> chain;
            if(BuildSeedChain(pos,point,scene,caster,chain,false,pIorStack,&sampler)>0
                && !chain.empty() && chain.front().pObject==mirror) {
                RefreshLegacySeedNM(chain,nm);
                baseSeeds.push_back(std::move(chain));
            }
        }
    }
    if(baseSeeds.empty()) return result;

	// Multi-trial SMS with photon-aided seeding + root-dedupe — see
	// EvaluateAtShadingPoint (RGB) for the full rationale.
	//
	// For NM (spectral): each trial's new seed chain is built by
	// BuildSeedChain WITHOUT the wavelength-dependent eta override.  We
	// re-apply the per-wavelength eta to the new chain inside the trial
	// loop so every converged root uses the correct dispersion IOR.
	const unsigned int N = ( config.multiTrials > 0 ) ? config.multiTrials : 1;
	const Scalar dedupeThr = ( config.uniquenessThreshold > 0.0 )
		? config.uniquenessThreshold : 1e-4;
	const std::vector<ManifoldVertex> baseSeedChain = baseSeeds.front();

	// See the RGB EvaluateAtShadingPoint companion: photon retrieval is
	// independent of `multi_trials` (N) so a scene that only sets
	// `sms_photon_count > 0` still gets photons fed into Newton.
	std::vector<SMSPhoton> photonSeeds;
	if( pPhotonMap && pPhotonMap->IsBuilt() )
	{
		Scalar r = config.photonSearchRadius;
		if( r <= 0 ) {
			r = pPhotonMap->GetAutoRadius();
		}
		if( r > 0 ) {
			pPhotonMap->QuerySeeds( pos, r * r, photonSeeds );
			RandomSubsamplePhotonSeeds( photonSeeds,
				config.maxPhotonSeedsPerShadingPoint, loopSampler );
		}
	}

	// Dedupe key: (firstPos, chainLen, reflectMask) — same three-field
	// scheme as RGB snell mode (`EvaluateAtShadingPoint`).  Position-
	// only dedupe (the prior NM-snell behaviour) silently collapsed
	// multi-trial sibling chains that share a first-vertex but flip
	// reflect/refract on a later vertex, AND distinct k-vs-k+2 chain
	// topologies that shared a first-vertex.
	std::vector<Point3>             acceptedRootPositions;
	std::vector<unsigned int>       acceptedRootChainLens;
	std::vector<unsigned long long> acceptedRootReflectMasks;
	auto buildReflectMask = []( const std::vector<ManifoldVertex>& ch ) -> unsigned long long {
		unsigned long long mask = 0;
		const std::size_t k = std::min<std::size_t>( ch.size(), 64 );
		for( std::size_t i = 0; i < k; i++ ) {
			if( ch[i].isReflection ) mask |= ( 1ull << i );
		}
		return mask;
	};
	// Per-trial unclamped geometric term + pre-geometric contribution
	// for the post-loop sum-clamp (matches the RGB snell-mode pattern at
	// `EvaluateAtShadingPoint` line ~6536).  Sum-level clamping bounds
	// the per-pixel total at `maxGeometricTerm` instead of
	// `N × maxGeometricTerm` (per-path clamping, the prior NM-snell
	// behaviour).
	std::vector<Scalar> acceptedGeoTerm;
	std::vector<Scalar> acceptedPreGeo;
	Scalar totalContribution = 0.0;
	unsigned int validTrials = 0;

	std::size_t photonCursor = 0;
    const IObject* pFirstCaster=baseSeedChain.front().pObject;
    const bool surfaceSampleReflectionFallback=baseSeedChain.size()==1
        && !baseSeedChain.front().canRefract && pFirstCaster && pFirstCaster->GetGeometry();

	// Total trial budget: 1 base seed + photon trials + (N - 1) extras.
	// Mirrors the RGB site's totalTrials.  The `trial > 0` branch below
	// consumes photons; without `+ photonSeeds.size()` here, only the
	// (N-1) extra slots could draw from the photon list — the rest of
	// the queried photons would be silently discarded.
	const unsigned int totalTrials = static_cast<unsigned int>(baseSeeds.size())
		+ static_cast<unsigned int>( photonSeeds.size() )
		+ ( N > 0 ? N - 1 : 0 );

	// Only a legacy delta sample without its light pointer uses RGB
	// uplift. Area emitters must be evaluated per solved-chain direction.
	Scalar meshLeNM = 0;
	if( lightSample.isDelta && !lightSample.pLight ) {
		meshLeNM = SMSLeNM( lightSample.Le, nm );
	}

	for( unsigned int trial = 0; trial < totalTrials; trial++ )
	{
		std::vector<ManifoldVertex> trialSeed = baseSeedChain;
        if(trial<baseSeeds.size()) trialSeed=baseSeeds[trial];
        bool useSurfaceSample=trial==0 && surfaceSampleReflectionFallback;

		// See RGB variant for the rationale: use photon's stored chain
		// directly (reversed and with flipped isExiting) to preserve
		// the topology the photon actually traversed.
		if( trial >= baseSeeds.size() )
		{
			if( photonCursor >= photonSeeds.size() ) {
                if(!surfaceSampleReflectionFallback) continue;
                useSurfaceSample=true;
            } else {
			const SMSPhoton& ph = photonSeeds[photonCursor];
			photonCursor++;

			const unsigned int k = ph.chainLen;
			if( k == 0 || k > kSMSMaxPhotonChain ) {
				continue;
			}
			// TARGET BOUNCES (Mitsuba-parity) — inline photon-chain
			// reconstruction must apply the same length filter as
			// snell/uniform seeds.  Without this, photon-aided seeds
			// can supply chains of any length while the main snell
			// trace is constrained to K, producing inconsistent chain
			// lengths in the same Solve loop.
			if( config.targetBounces > 0 && k != config.targetBounces ) {
				continue;
			}

			std::vector<ManifoldVertex> newChain;
			if( ReversePhotonChainForSeed( ph, newChain, nm ) == 0 ) continue;
			trialSeed = newChain;
            }
		}

		// Build a single-vertex seed by uniform-sampling the reflective
		// caster's surface.  Inherits material data from the original
		// baseSeedChain[0]; only position/normal change.  Newton then
		// walks from this random surface point to a true reflection root.
		if( useSurfaceSample )
		{
			Point3 sp;
			Vector3 sn;
			Point2 sc;
			pFirstCaster->UniformRandomPoint(
				&sp, &sn, &sc,
				Point3( loopSampler.Get1D(), loopSampler.Get1D(), loopSampler.Get1D() ) );
			ManifoldVertex mv = baseSeedChain[0];
			mv.position = sp;
			mv.normal   = sn;
			mv.geomNormal = sn;	// uniform-area sample: best-available proxy
			mv.uv       = sc;
			mv.dpdu = Vector3(0,0,0);  // Solve will compute via ComputeVertexDerivatives
			mv.dpdv = Vector3(0,0,0);
			mv.dndu = Vector3(0,0,0);
			mv.dndv = Vector3(0,0,0);
			mv.valid = false;
			trialSeed.clear();
			trialSeed.push_back( mv );
		}

		ManifoldResult mResult = Solve(
			pos, shadingNormal,
			lightSample.position, lightSample.normal,
			trialSeed, loopSampler );

		if( !mResult.valid ) continue;

		// Dedupe: skip if we've already accepted a root with the same
		// (firstPos, chainLen, reflectMask).  Three-field key mirrors
		// RGB snell mode — see commit log on this fix block.
		const Point3& firstPos = mResult.specularChain[0].position;
		const unsigned int chainLen = static_cast<unsigned int>( mResult.specularChain.size() );
		const unsigned long long reflectMask = buildReflectMask( mResult.specularChain );
		bool duplicate = false;
		for( unsigned int r = 0; r < acceptedRootPositions.size(); r++ )
		{
			if( acceptedRootChainLens[r] == chainLen &&
				acceptedRootReflectMasks[r] == reflectMask &&
				Point3Ops::Distance( firstPos, acceptedRootPositions[r] ) < dedupeThr )
			{
				duplicate = true;
				break;
			}
		}
		if( duplicate ) continue;

		// Visibility: check external segments of the specular chain
		if( !CheckChainVisibility( pos, lightSample.position,
			mResult.specularChain, caster, &sampler ) ) continue;

		// Direction from shading point toward first specular vertex
		const ManifoldVertex& firstSpec = mResult.specularChain[0];
		Vector3 dirToFirstSpec = Vector3Ops::mkVector3(
			firstSpec.position, pos );
		Scalar distToFirstSpec = Vector3Ops::Magnitude( dirToFirstSpec );
		if( distToFirstSpec < 1e-8 ) continue;
		dirToFirstSpec = dirToFirstSpec * (1.0 / distToFirstSpec);

		const Vector3 wiAtShading = dirToFirstSpec;

		// Evaluate BSDF at shading point (spectral)
		Ray evalRay( pos, Vector3( -woOutgoing.x, -woOutgoing.y, -woOutgoing.z ) );
		RayIntersectionGeometric rig( evalRay, nullRasterizerState );
		rig.bHit = true;
		rig.ptIntersection = pos;
		// SHADING-frame BSDF eval (Veach §5.3.6 / PBRT 4e §9.1).
		rig.vNormal     = shadingNormal;
		rig.vGeomNormal = geomNormal;
		rig.onb = onb;
		rig.ambientIOR = SMSReceiverAmbientIOR( pIorStack );	// DL-290

		// DL-290: stateful, as the RGB twin (see ComputeTrialContributionNM).
		Scalar fBSDF = pBSDF->valueStatefulNM( wiAtShading, rig, nm, pIorStack );
		if( fBSDF <= 0 ) continue;

		// Cosine at shading point — SHADING frame matches `f * cos / pdf`.
		Scalar cosAtShading = fabs( Vector3Ops::Dot( shadingNormal, wiAtShading ) );
		if( cosAtShading <= 0 ) continue;

		// Chain throughput (spectral — per-wavelength Fresnel)
		Scalar chainThroughput = EvaluateChainThroughputNM(
			pos, lightSample.position, mResult.specularChain, nm );

		// Direction from light to last specular vertex (for emission eval)
		const ManifoldVertex& lastSpec = mResult.specularChain.back();
		Vector3 dirSpecToLight = Vector3Ops::mkVector3(
			lastSpec.position, lightSample.position );
		Scalar distSpecToLight = Vector3Ops::Magnitude( dirSpecToLight );
		if( distSpecToLight < 1e-8 ) continue;
		dirSpecToLight = dirSpecToLight * (1.0 / distSpecToLight);

		Scalar cosAtLight;
		Scalar Le;
		if( lightSample.isDelta ) {
			cosAtLight = 1.0;
			if( lightSample.pLight ) {
				// Delta light: its own illuminant spectrum at `nm`
				// (Stage C slice 2).  See SMSLeNM's comment block.
				Le = lightSample.pLight->emittedRadianceNM( dirSpecToLight, nm );
			} else {
				Le = meshLeNM;	// hoisted -- see the comment above the trial loop
			}
		} else {
			cosAtLight = fabs( Vector3Ops::Dot( lightSample.normal, dirSpecToLight ) );
			if( cosAtLight <= 0 ) continue;
			Le = lightSample.pLuminary ? SMSAreaLeNM( lightSample, dirSpecToLight, nm )
			: SMSLeNM( lightSample.Le, nm );
			if( Le <= 0 ) continue;
		}

		// SMS measure-conversion factor — must match the RGB path exactly
		// (see the long comment in EvaluateAtShadingPoint for derivation).
		// The previous `cosAtLight * chainGeom / jacobianDet` formulation is
		// obsolete: it double-counts distance terms via the chainGeom
		// product and applies cos(θ_y) explicitly even though cos(θ_y) is
		// implicit in |det(∂C/∂y)|.  Replacing with G(x, v_1) · |det(δv_1/δy)|
		// keeps the spectral caustic intensity consistent with the RGB path
		// (important for HWSS regressions and spectral fireflies that would
		// otherwise leak in only at certain wavelengths).
		const ManifoldVertex& firstSpecForG = mResult.specularChain[0];
		Vector3 dirXtoV1 = Vector3Ops::mkVector3( firstSpecForG.position, pos );
		const Scalar distXtoV1 = Vector3Ops::NormalizeMag( dirXtoV1 );
		if( distXtoV1 < 1e-8 ) continue;
		// G_x_v1 is path-space (Veach §8.2) — geometric with shading fallback.
		const Vector3& v1SideN = ( Vector3Ops::SquaredModulus( firstSpecForG.geomNormal ) > NEARZERO )
			? firstSpecForG.geomNormal : firstSpecForG.normal;
		const Scalar cosV1atX = fabs( Vector3Ops::Dot( v1SideN, dirXtoV1 ) );
		const Scalar G_x_v1 = cosV1atX / (distXtoV1 * distXtoV1);
		const Scalar detDvDy = ComputeLightToFirstVertexJacobianDet(
			mResult.specularChain, pos, lightSample.position, JacobianLightNormal( lightSample, mResult.specularChain ) );
		const Scalar smsGeometric = G_x_v1 * detDvDy;
		// Sum-level clamp (matches RGB snell): leave the geometric term
		// UNCLAMPED here and apply the cap to the SUM across all unique
		// preimages after the trial loop.  Per-path clamping (the prior
		// behaviour) would let a fold caustic with N preimages emit up
		// to `N × maxGeometricTerm`.

		// cosAtLight is no longer multiplied here — it is implicit in the
		// Jacobian.  Keep its evaluation above for the backface-cull guard
		// on non-delta lights; silence the unused-variable warning:
		(void)cosAtLight;

		Scalar trialContribution = fBSDF
			* chainThroughput
			* Le * cosAtShading * smsGeometric
			/ (lightSample.pdfPosition * lightSample.pdfSelect);

		if( mResult.pdf > 1e-20 )
		{
			trialContribution = trialContribution / mResult.pdf;
		}

		totalContribution += trialContribution;
		acceptedGeoTerm.push_back( smsGeometric );
		acceptedPreGeo.push_back(
			smsGeometric > 1e-20 ? ( trialContribution / smsGeometric ) : 0.0 );
		acceptedRootPositions.push_back( firstPos );
		acceptedRootChainLens.push_back( chainLen );
		acceptedRootReflectMasks.push_back( reflectMask );
		validTrials++;
	}

	if( validTrials == 0 ) return result;

	// Sum-level geometric clamp (NM snell mode; mirrors RGB snell at
	// `EvaluateAtShadingPoint` line ~7050).
	if( config.maxGeometricTerm > 0 )
	{
		Scalar sumGeoTerm = 0;
		for( Scalar g : acceptedGeoTerm ) sumGeoTerm += g;

		if( sumGeoTerm > config.maxGeometricTerm )
		{
			const Scalar scale = config.maxGeometricTerm / sumGeoTerm;
			totalContribution = 0.0;
			for( std::size_t i = 0; i < acceptedPreGeo.size(); i++ )
			{
				totalContribution += acceptedPreGeo[i] * ( acceptedGeoTerm[i] * scale );
			}
		}
	}

	result.contribution = totalContribution;  // Σ_r f_r over unique roots
	result.misWeight = 1.0;  // PDF weighting already folded into contribution
	result.valid = true;

	return result;
}

//////////////////////////////////////////////////////////////////////
// BuildSnellBaseSeed -- the deterministic snell-mode base seed.
//////////////////////////////////////////////////////////////////////

unsigned int ManifoldSolver::BuildSnellBaseSeed(
	const Point3& pos,
	const Vector3& geomNormal,
	const Point3& lightPos,
	const IScene& scene,
	const IRayCaster& caster,
	std::vector<ManifoldVertex>& chain,
	const IORStack* pIorStack,
	ISampler* pSampler,
	const Scalar nm
	) const
{
	unsigned int chainLen = BuildSeedChain(
		pos, lightPos,
		scene, caster, chain,
		/*applyEmitterStop=*/ true, pIorStack, pSampler );	// DL-290: walk starts in the receiver's medium

	if( chainLen == 0 || chain.empty() )
	{
		// Fallback: trace along the surface normal — geometric so the
		// probe direction is the actual outward face direction.  Pass
		// `applyEmitterStop = false`: the 100-unit `normalTarget` is a
		// synthesized direction-probe, NOT the emitter — applying the
		// projection cap (which fires at `100 × 1.05 = 105`) would
		// silently reject every specular surface farther than 105 units
		// from the shading point.  Without the flag this fallback is
		// non-functional on any scene whose first specular surface is
		// > 105 units from the shading point (e.g. wall→egg ~141 units
		// in the cornell-box / Veach-egg layout).
		const Point3 normalTarget = Point3Ops::mkPoint3(
			pos, geomNormal * 100.0 );
		chainLen = BuildSeedChain(
			pos, normalTarget,
			scene, caster, chain,
			/*applyEmitterStop=*/ false, pIorStack, pSampler );	// DL-290
	}

	if( chainLen == 0 || chain.empty() )
	{
		// Fallback: try tracing toward the midpoint between pos and
		// the light.  For tilted geometry, the specular object may lie
		// between the two endpoints at a position that neither the
		// light-direction nor the normal-direction ray can reach.
		// `applyEmitterStop = false` for the same reason as the normal-
		// target fallback above (synthesized 100-unit probe direction,
		// not an actual emitter).
		const Point3 midpoint(
			(pos.x + lightPos.x) * 0.5,
			(pos.y + lightPos.y) * 0.5,
			(pos.z + lightPos.z) * 0.5 );
		const Point3 midTarget = Point3Ops::mkPoint3(
			pos, Vector3Ops::Normalize( Vector3Ops::mkVector3( midpoint, pos ) ) * 100.0 );
		chainLen = BuildSeedChain(
			pos, midTarget,
			scene, caster, chain,
			/*applyEmitterStop=*/ false, pIorStack, pSampler );	// DL-290
	}

	if( chainLen == 0 || chain.empty() )
	{
		chain.clear();
		return 0;
	}

	RefreshLegacySeedNM(chain,nm);

	return static_cast<unsigned int>( chain.size() );
}

//////////////////////////////////////////////////////////////////////
// DL-372 / DL-336 split suppression
//
//   PT used to drop EVERY BSDF-sampled emitter hit that followed an SMS
//   anchor and an all-caster delta chain, on the assumption that SMS had
//   estimated that path.  Snell-mode SMS seeds ONE deterministic chain per
//   light sample, so where an emitter point has several roots (a ball lens
//   near focus, a weak lens's second image) the roots the seed does not
//   reach were dropped by PT and never estimated by SMS.  With a
//   deterministic seed + solve, SMS's estimator at anchor x is
//   sum_{k in F(x,y)} w_k(y) / p(y), F(x,y) the roots the seed reaches; PT
//   keeps exactly the complement by re-running that seed + solve toward
//   its own hit point and suppressing only when the result is its chain.
//   docs/SMS_ENERGY_LOSS_INVESTIGATION.md section 7.
//////////////////////////////////////////////////////////////////////

bool ManifoldSolver::SplitSuppressionExact( const IRayCaster& caster, bool /*spectral*/ ) const
{
	// Uniform seeding samples its seeds; the unbiased estimator weights
	// each found root by an estimated 1/p (it estimates every root, so
	// suppressing them all is its partition); photon-aided trials and the
	// RGB/NM pure-mirror supplements add random seeds; alpha coverage routes
	// snell mode to the uniform evaluator.
	if( config.seedingMode != ManifoldSolverConfig::eSeedingSnell ) return false;
	if( !config.biased ) return false;
	if( pPhotonMap && pPhotonMap->IsBuilt() ) return false;
	const LightSampler* pLS = caster.GetLightSampler();
	if( !pLS || pLS->SceneHasAlphaCoverage() ) return false;
	if( mHasPureMirrorCaster ) return false;
	return true;
}

SMSChainCoverage ManifoldSolver::ClassifyEmitterHitCoverage(
	const SMSChainRecord& rec,
	const Point3& y,
	const Vector3& yNormal,
	const IScene& scene,
	const IRayCaster& caster,
	const Scalar nm
	) const
{
	if( !rec.anchorValid || rec.broken ) {
		return eSMSChainCoverageUnknown;
	}
	const unsigned int k = rec.count;
	if( k == 0 || k > SMSChainRecord::kMaxVertices ) {
		return eSMSChainCoverageUnknown;
	}

	// The exact mode draws nothing: no alpha coverage (the seed walk's
	// intersection draws only at partial coverage) and a biased Solve
	// (its sampler feeds only EstimatePDF and the disabled phys-fail
	// restart).  A local sampler keeps PT's own stream untouched anyway.
	RandomNumberGenerator localRng;
	IndependentSampler localSampler( localRng );

	// SMS's own seed for this (anchor, emitter point) pair.
	std::vector<ManifoldVertex> seed;
	BuildSnellBaseSeed( rec.anchorPos, rec.anchorGeomNormal, y, scene, caster,
		seed, &rec.anchorStack, &localSampler, nm );
	if( seed.empty() ) {
		return eSMSChainNotCovered;		// no seed: SMS estimates nothing here
	}
	// RGB/NM snell mode replaces a k = 1 mirror base seed by a RANDOM surface
	// sample (EvaluateAtShadingPoint's surface-sample fallback): no exact
	// answer in either evaluator.
	if( seed.size() == 1 && !seed[0].canRefract ) {
		return eSMSChainCoverageUnknown;
	}

	const ManifoldResult rs = Solve( rec.anchorPos, rec.anchorShadingNormal,
		y, yNormal, seed, localSampler );
	if( !rs.valid ) {
		return eSMSChainNotCovered;		// Newton failure / rejected root
	}
	const std::vector<ManifoldVertex>& R = rs.specularChain;
	if( R.size() != k ) {
		return eSMSChainNotCovered;
	}
	for( unsigned int i = 0; i < k; i++ ) {
		if( R[i].pObject != rec.v[i].pObject || R[i].isReflection != rec.v[i].isReflection ) {
			return eSMSChainNotCovered;	// a different chain topology
		}
	}

	// Root identity, relative to the path length: two converged solves of
	// the same root agree to Newton precision; distinct roots closer than
	// 1e-3 of the path are merging at a fold, where the partition's error
	// is that sliver.
	Scalar L = Point3Ops::Distance( rec.anchorPos, R[0].position ) + Point3Ops::Distance( R[k-1].position, y );
	for( unsigned int i = 0; i + 1 < k; i++ ) {
		L += Point3Ops::Distance( R[i].position, R[i+1].position );
	}
	const Scalar tol = 1e-3 * L;

	bool same = true;
	for( unsigned int i = 0; i < k && same; i++ ) {
		same = Point3Ops::Distance( R[i].position, rec.v[i].position ) <= tol;
	}

	if( !same )
	{
		// PT's chain through SMS's own vertex records, at PT's positions.
		std::vector<ManifoldVertex> proj( R );
		for( unsigned int i = 0; i < k; i++ ) {
			ManifoldVertex& mv = proj[i];
			mv.position = rec.v[i].position;
			mv.normal = rec.v[i].normal;
			mv.geomNormal = rec.v[i].geomNormal;
			mv.uv = rec.v[i].uv;
			mv.objectPosition = rec.v[i].objectPosition;
			mv.dpdu = Vector3Ops::Normalize( Vector3Ops::Perpendicular( mv.normal ) );
			mv.dpdv = Vector3Ops::Normalize( Vector3Ops::Cross( mv.normal, mv.dpdu ) );
			mv.dndu = Vector3( 0, 0, 0 );
			mv.dndv = Vector3( 0, 0, 0 );
			mv.valid = false;
		}
		// An exact delta chain (a perfect refractor) is already a root:
		// a different one from SMS's.
		std::vector<Scalar> C;
		EvaluateConstraint( proj, rec.anchorPos, y, C );
		Scalar norm2 = 0;
		for( std::size_t i = 0; i < C.size(); i++ ) norm2 += C[i] * C[i];
		if( std::sqrt( norm2 ) < config.solverThreshold ) {
			return eSMSChainNotCovered;
		}
		// A warped delta lobe (a dielectric with finite `scattering`):
		// PT's chain is near, not on, the manifold.  Assign it to the root
		// Newton reaches from it -- in the delta limit, the root it is on.
		for( unsigned int i = 0; i < k; i++ ) {
			proj[i].dpdu = Vector3( 0, 0, 0 );
			proj[i].dpdv = Vector3( 0, 0, 0 );
		}
		const ManifoldResult rp = Solve( rec.anchorPos, rec.anchorShadingNormal,
			y, yNormal, proj, localSampler );
		if( !rp.valid || rp.specularChain.size() != k ) {
			return eSMSChainNotCovered;
		}
		for( unsigned int i = 0; i < k; i++ ) {
			if( rp.specularChain[i].isReflection != R[i].isReflection ||
				Point3Ops::Distance( rp.specularChain[i].position, R[i].position ) > tol ) {
				return eSMSChainNotCovered;
			}
		}
	}

	// SMS drops a root whose external segments are occluded.
	if( !CheckChainVisibility( rec.anchorPos, y, R, caster, &localSampler ) ) {
		return eSMSChainNotCovered;
	}
	return eSMSChainCovered;
}

//////////////////////////////////////////////////////////////////////
// CheckChainVisibility
//
//   Tests whether the external segments of an SMS specular chain
//   are unoccluded.  Only the two external segments are tested:
//
//     1. Shading point  ->  first specular vertex
//     2. Last specular vertex  ->  light source
//
//   Inter-specular segments (through refractive geometry) are not
//   tested because CastShadowRay tests all objects, including the
//   glass surfaces themselves, which would always report a hit.
//   The external-segment checks catch the dominant occlusion cases
//   (opaque walls between receiver and glass, or between glass
//   and light).
//////////////////////////////////////////////////////////////////////

namespace
{
	// Chain-aware visibility: the segment is considered BLOCKED if the ray
	// passes through a specular caster that is NOT in the allowed set
	// (the chain's caster objects).  Specular hits on allowed casters
	// are traversed like above (the chain already accounts for them);
	// hits on *unrelated* specular objects mean the physical photon
	// path would also refract there, making the k-vertex chain an
	// incomplete description of the transport.  Rejecting them prevents
	// computing light through a chain topology shorter than the actual
	// physics would demand — a major firefly source on
	// intersecting-glass scenes (torus_cross) where shadow rays through
	// the caustic center graze the other torus.
	bool SegmentOccludedByNonChainSpeculars(
		const Point3& start,
		const Vector3& dir,
		const Scalar maxDist,
		const IRayCaster& caster,
		const std::vector<const IObject*>& allowedSpecularObjects, ISampler& sampler )
	{
		const IScene* pScene = caster.GetAttachedScene();
		if( !pScene ) {
			return caster.CastShadowRaySampled( Ray( start, dir ), maxDist, sampler );
		}
		const IObjectManager* pObjMgr = pScene->GetObjects();
		if( !pObjMgr ) {
			return caster.CastShadowRaySampled( Ray( start, dir ), maxDist, sampler );
		}

		const unsigned int kMaxSpecularTraversals = 8;
		Point3 curOrigin = start;
		Scalar distRemaining = maxDist;

		for( unsigned int it = 0; it < kMaxSpecularTraversals; it++ )
		{
			Ray ray( curOrigin, dir );
			RayIntersection ri( ray, nullRasterizerState );
			// Do not sample endpoint coverage beyond this finite visibility
            // interval; final endpoint acceptance is handled exactly once.
            pObjMgr->IntersectRaySampled(ri, sampler, true, true, false, distRemaining);

			if( !ri.geometric.bHit ) return false;
			if( ri.geometric.range > distRemaining ) return false;

			const IMaterial* pMat = ri.pMaterial;
			bool isPureSpecular = false;
			if( pMat ) {
				IORStack dummyStack( 1.0 );
				SpecularInfo specInfo = pMat->GetSpecularInfo( ri.geometric, dummyStack );
				isPureSpecular = specInfo.isSpecular;
			}
			if( !isPureSpecular ) {
				return true;  // Opaque blocker
			}

			// Is this specular hit on one of the chain's casters?
			bool inChain = false;
			for( std::size_t a = 0; a < allowedSpecularObjects.size(); a++ )
			{
				if( ri.pObject == allowedSpecularObjects[a] ) { inChain = true; break; }
			}
			if( !inChain ) {
				// Specular object not in the chain — the physical path
				// through here requires additional refractions we don't
				// have.  Treat as blocked.
				return true;
			}

			// Allowed specular: traverse past and keep going.
			const Scalar step = ri.geometric.range + 1e-4;
			if( step >= distRemaining ) return false;
			curOrigin = Point3Ops::mkPoint3( curOrigin, dir * step );
			distRemaining -= step;
		}
		return false;
	}
}

bool ManifoldSolver::CheckChainVisibility(
	const Point3& shadingPoint,
	const Point3& lightPoint,
	const std::vector<ManifoldVertex>& chain,
	const IRayCaster& caster, ISampler* alphaSampler
	) const
{
    RandomNumberGenerator alphaRandom;
    IndependentSampler alphaFallback(alphaRandom);
    ISampler& sampler = alphaSampler ? *alphaSampler : static_cast<ISampler&>(alphaFallback);
	if( chain.empty() ) return true;
    // Physical endpoint coverage belongs to the solved path, outside the
    // seed-proposal Bernoulli normalization. Projection is geometry-only;
    // exactly one alpha draw follows at the validated final surface point.
    const bool sceneAlpha = caster.GetLightSampler() && caster.GetLightSampler()->SceneHasAlphaCoverage();
    for (size_t i=0;i<chain.size();++i) {
        const auto& v=chain[i];
        // Newton can move a seed across an inherited CSG material boundary.
        // Validate retained records in alpha scenes, even an opaque seed.
        if (!sceneAlpha && (!v.pMaterial || v.pMaterial->GetAlphaMode()==eAlphaOpaque)) continue;
        // Visibility consumes the physical hit that produced the solved
        // endpoint. A second normal probe can hit a different SDF/CSG face.
        // Publicly fabricated or moved vertices have no such certificate.
        if (!v.HasAlphaEndpoint()) return false;
        RayIntersection hit(*v.alphaEndpoint);
        const Point3 prev=i?chain[i-1].position:shadingPoint;
        hit.geometric.ray=Ray(prev,Vector3Ops::Normalize(Vector3Ops::mkVector3(v.position,prev)));
        hit.geometric.range=Point3Ops::Distance(prev,v.position);
        if (hit.pMaterial && hit.pMaterial->GetAlphaMode()!=eAlphaOpaque) {
            RayIntersectionGeometric alphaRI(hit.geometric);
            alphaRI.ptIntersection = v.position;
            alphaRI.signals.pScene = caster.GetAttachedScene() ? caster.GetAttachedScene()->GetObjects() : nullptr;
            alphaRI.signals.pSelf = v.pObject;
            alphaRI.signals.ptWorld = v.position;
            if (!hit.pMaterial->AcceptAlpha(alphaRI,sampler)) return false;
        }
    }


#if SMS_TRACE_DIAGNOSTIC
	static std::atomic<int> g_visTraceCount{ 0 };
	const bool visTrace =
		( std::fabs( shadingPoint.x ) < 0.3 ) &&
		( std::fabs( shadingPoint.z ) < 0.3 ) &&
		( shadingPoint.y >= -0.02 && shadingPoint.y <= 0.02 ) &&
		( g_visTraceCount.fetch_add( 1, std::memory_order_relaxed ) < 30 );
#endif

	// Build the "allowed specular objects" list from the chain's vertex
	// casters.  Any specular hit on an object NOT in this list on an
	// external segment means the straight-line shadow path would refract
	// there — the k-vertex chain is incomplete and should be rejected.
	// See the block comment on SegmentOccludedByNonChainSpeculars.
	std::vector<const IObject*> chainCasters;
	chainCasters.reserve( chain.size() );
	for( std::size_t i = 0; i < chain.size(); i++ ) {
		const IObject* o = chain[i].pObject;
		if( o ) {
			bool have = false;
			for( std::size_t j = 0; j < chainCasters.size(); j++ ) {
				if( chainCasters[j] == o ) { have = true; break; }
			}
			if( !have ) chainCasters.push_back( o );
		}
	}

	// Segment 1: shading point to first specular vertex
	//
	// The shading point sits on a non-specular surface.  The first
	// specular vertex is on the glass surface facing the shading
	// point.  We offset the ray endpoint by a small amount to
	// avoid hitting the glass surface at the target vertex.
	{
		const ManifoldVertex& vFirst = chain.front();

		// Determine the outward normal at the first vertex (pointing
		// toward the shading point, i.e. away from the glass interior)
		Vector3 dirToShading = Vector3Ops::mkVector3( shadingPoint, vFirst.position );
		const Scalar nDotDir = Vector3Ops::Dot( vFirst.normal, dirToShading );
		const Vector3 outwardN = (nDotDir >= 0)
			? vFirst.normal
			: Vector3( -vFirst.normal.x, -vFirst.normal.y, -vFirst.normal.z );

		// Biased endpoint: pull back from the glass surface along its
		// outward normal.  This prevents the shadow ray from hitting
		// the glass mesh at or near the target vertex.  Use a generous
		// bias to clear displacement bumps on the glass surface.
		const Scalar endBias = 5e-2;
		const Point3 biasedEnd = Point3Ops::mkPoint3(
			vFirst.position, outwardN * endBias );

		Vector3 dir = Vector3Ops::mkVector3( biasedEnd, shadingPoint );
		Scalar dist = Vector3Ops::NormalizeMag( dir );
		if( dist > 1e-4 )
		{
			Point3 origin = Point3Ops::mkPoint3( shadingPoint, dir * 1e-4 );
			const bool blocked = SegmentOccludedByNonChainSpeculars(
				origin, dir, dist - 2e-4, caster, chainCasters, sampler );
#if SMS_TRACE_DIAGNOSTIC
			if( visTrace ) {
				GlobalLog()->PrintEx( eLog_Event,
					"VIS_SEG1: shading=(%.4f,%.4f,%.4f) biasedEnd=(%.4f,%.4f,%.4f) dist=%.4f blocked=%d",
					shadingPoint.x, shadingPoint.y, shadingPoint.z,
					biasedEnd.x, biasedEnd.y, biasedEnd.z, dist, int( blocked ) );
			}
#endif
			if( blocked )
				return false;
		}
	}

	// Segment 2: last specular vertex to light source
	//
	// The last specular vertex sits on the glass surface facing the
	// light.  On a displaced mesh, nearby bumps can easily block a
	// ray launched with only a tiny directional bias.  We offset the
	// ray origin along the surface normal at the vertex to clear the
	// local surface geometry before testing for external occlusion.
	{
		const ManifoldVertex& vLast = chain.back();

		// Outward normal: pointing toward the light (away from glass)
		Vector3 dirToLight = Vector3Ops::mkVector3( lightPoint, vLast.position );
		const Scalar nDotDir = Vector3Ops::Dot( vLast.normal, dirToLight );
		const Vector3 outwardN = (nDotDir >= 0)
			? vLast.normal
			: Vector3( -vLast.normal.x, -vLast.normal.y, -vLast.normal.z );

		// Offset start along outward normal to clear displaced surface.
		// Use a generous bias for rough displacement.
		const Scalar normalBias = 5e-2;
		const Point3 biasedStart = Point3Ops::mkPoint3(
			vLast.position, outwardN * normalBias );

		Vector3 newDir = Vector3Ops::mkVector3( lightPoint, biasedStart );
		Scalar newDist = Vector3Ops::NormalizeMag( newDir );
		if( newDist > 1e-4 )
		{
			const bool blocked = SegmentOccludedByNonChainSpeculars(
				biasedStart, newDir, newDist - 1e-4, caster, chainCasters, sampler );
#if SMS_TRACE_DIAGNOSTIC
			if( visTrace ) {
				GlobalLog()->PrintEx( eLog_Event,
					"VIS_SEG2: biasedStart=(%.4f,%.4f,%.4f) light=(%.4f,%.4f,%.4f) dist=%.4f blocked=%d",
					biasedStart.x, biasedStart.y, biasedStart.z,
					lightPoint.x, lightPoint.y, lightPoint.z, newDist, int( blocked ) );
			}
#endif
			if( blocked )
				return false;
		}
	}

	// Segment I: inter-specular AIR segments between consecutive
	// chain vertices.  A chain whose topology traverses two or more
	// DISTINCT casters (e.g. the k=2×2 path through two different
	// toruses) has air segments in between each pair of casters:
	//
	//   shading  -[air]-  v_i (enter cast A)  -[A glass]-  v_{i+1}
	//              (exit A)  -[air]-  v_{i+2} (enter cast B)  -[B glass]-
	//              v_{i+3} (exit B)  -[air]-  light
	//
	// A segment v_i → v_{i+1} is in AIR when v_i.isExiting==true
	// (receiver ray leaves glass at v_i) AND v_{i+1}.isExiting==false
	// (receiver ray enters glass at v_{i+1}).  These segments need
	// the same chain-aware visibility check as the outer two: if they
	// cross a specular caster NOT in the chain, the k-vertex
	// description is missing refractions that the physical photon
	// would have to undergo, and the chain should be rejected.
	// (The complementary topology — v_i entering glass, v_{i+1}
	// exiting same object — is an IN-GLASS segment and intentionally
	// skipped: the chain accounts for the refractions bracketing it.)
	for( std::size_t i = 0; i + 1 < chain.size(); i++ )
	{
		const ManifoldVertex& a = chain[i];
		const ManifoldVertex& b = chain[i+1];
		if( !a.isExiting ) continue;   // segment starts INSIDE glass
		if( b.isExiting )  continue;   // segment ends INSIDE glass

		// Both endpoints are on the AIR side of their surface —
		// segment is in air.  Bias the origin out along a's outward
		// normal (away from its glass), endpoint back along b's
		// outward normal (away from its glass).
		Vector3 outN_a = a.normal;
		{
			Vector3 dirOutA = Vector3Ops::mkVector3( b.position, a.position );
			if( Vector3Ops::Dot( outN_a, dirOutA ) < 0 )
				outN_a = Vector3( -outN_a.x, -outN_a.y, -outN_a.z );
		}
		Vector3 outN_b = b.normal;
		{
			Vector3 dirOutB = Vector3Ops::mkVector3( a.position, b.position );
			if( Vector3Ops::Dot( outN_b, dirOutB ) < 0 )
				outN_b = Vector3( -outN_b.x, -outN_b.y, -outN_b.z );
		}

		const Scalar biasEps = 5e-2;
		const Point3 biasedStart = Point3Ops::mkPoint3(
			a.position, outN_a * biasEps );
		const Point3 biasedEnd = Point3Ops::mkPoint3(
			b.position, outN_b * biasEps );

		Vector3 segDir = Vector3Ops::mkVector3( biasedEnd, biasedStart );
		const Scalar segDist = Vector3Ops::NormalizeMag( segDir );
		if( segDist < 1e-4 ) continue;

		const bool blocked = SegmentOccludedByNonChainSpeculars(
			biasedStart, segDir, segDist - 1e-4, caster, chainCasters, sampler );
#if SMS_TRACE_DIAGNOSTIC
		if( visTrace ) {
			GlobalLog()->PrintEx( eLog_Event,
				"VIS_SEGI[%zu-%zu]: a=(%.4f,%.4f,%.4f) b=(%.4f,%.4f,%.4f) dist=%.4f blocked=%d",
				i, i+1,
				a.position.x, a.position.y, a.position.z,
				b.position.x, b.position.y, b.position.z,
				segDist, int( blocked ) );
		}
#endif
		if( blocked )
			return false;
	}

	return true;
}
