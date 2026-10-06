// Extended SMS estimator A: complete conditional proposals, zero trials,
// independent reciprocal retries, native R/T walks and root identity.
#include "SMSRenderTestSupport.h"
#include "../src/Library/Utilities/ManifoldSolver.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/IORStackSeeding.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/ILightManager.h"
#include "../src/Library/Interfaces/IBSDF.h"
#include "../src/Library/Interfaces/IUVGenerator.h"
#include "../src/Library/Geometry/SphericalUVGenerator.h"
#include "../src/Library/Geometry/TriangleMeshGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Geometry/CylindricalUVGenerator.h"
#include "../src/Library/Interfaces/IRayIntersectionModifier.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Interfaces/ITriangleMeshGeometry.h"
#include "../src/Library/Interfaces/IGeometryManager.h"
#include "../src/Library/Utilities/Optics.h"
#include "../src/Library/Shaders/PathTracingIntegrator.h"
#include "../src/Library/Shaders/PathTracingShaderOp.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Rendering/PathTracingPelRasterizer.h"
#include "../src/Library/Utilities/PathGuidingField.h"
#include <array>
#include <sstream>
#include <iomanip>
#include <thread>

struct Moments {
    double mean=0, sd=0;
    explicit Moments(const std::vector<double>& values) {
        for(double x:values) mean+=x;
        mean/=values.size();
        for(double x:values) sd+=(x-mean)*(x-mean);
        sd=std::sqrt(sd/(values.size()-1));
    }
};
struct ScriptSampler : ISampler {
    std::vector<Scalar> values; std::size_t next=0; Scalar remainder;
    explicit ScriptSampler(std::vector<Scalar> v, Scalar rest=.001) : values(std::move(v)), remainder(rest) {}
    Scalar Get1D() override { return next<values.size() ? values[next++] : remainder; }
    Point2 Get2D() override { const Scalar x=Get1D(); return Point2(x,Get1D()); }
};
struct Fixture {
    IJobPriv* job=nullptr;
    explicit Fixture(const std::string& text) {
        const auto path=TestTempPath("sms_reference_"+std::to_string(::getpid())+".RISEscene");
        {std::ofstream out(path);out<<text;}
        if(RISE_CreateJobPriv(&job)) Check(job->LoadAsciiSceneViaCst(path.c_str()),"reference scene parses");
        std::remove(path.c_str());
        if(job) job->GetScene()->GetObjects()->PrepareForRendering();
    }
    ~Fixture() { safe_release(job); }
    const IScene& Scene() const { return *job->GetScene(); }
    const IObject* Object(const char* name) const { return Scene().GetObjects()->GetItem(name); }
};
static std::string Materials() {
    return "RISE ASCII SCENE 7\nuniformcolor_painter\n{\n name white\n color 1 1 1\n}\n"
        "scalar_painter\n{\n name triple\n values 1.3 1.5 1.9\n}\n"
        "perfectrefractor_material\n{\n name glass\n refractance white\n ior triple\n}\n"
        "perfectreflector_material\n{\n name mirror\n reflectance white\n}\n";
}
static std::string Mesh(bool closed, bool reverse) {
    std::ostringstream s;
    s<<"indexedmesh_geometry\n{\n name shape\n double_sided TRUE\n face_normals TRUE\n";
    const Point3 p[]={Point3(-1,-1,-1),Point3(1,-1,-1),Point3(1,1,-1),Point3(-1,1,-1),
        Point3(-1,-1,1),Point3(1,-1,1),Point3(1,1,1),Point3(-1,1,1)};
    for(const auto& v:p) s<<" vertex "<<v.x<<' '<<v.y<<' '<<v.z<<'\n';
    for(int i=0;i<8;++i) s<<" uv "<<(i%2)<<' '<<((i/2)%2)<<'\n';
    const int t[][3]={{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
        {3,7,6},{3,6,2},{0,4,7},{0,7,3},{1,2,6},{1,6,5}};
    for(int i=0;i<(closed?12:2);++i) s<<" triangle "<<t[i][0]<<' '<<t[i][reverse?2:1]<<' '<<t[i][reverse?1:2]<<'\n';
    return s.str()+"}\n";
}
static std::string PlaneScene(const std::string& name, double z, double x0=-2, double x1=2) {
    std::ostringstream s;
    s<<"clippedplane_geometry\n{\n name "<<name<<"\n pta "<<x0<<" -2 "<<z
        <<"\n ptb "<<x0<<" 2 "<<z<<"\n ptc "<<x1<<" 2 "<<z<<"\n ptd "<<x1<<" -2 "<<z<<"\n doublesided TRUE\n}\n";
    return s.str();
}
static std::string SceneObject(const std::string& name,const std::string& geometry,const std::string& material,
    const std::string& extra="") {
    return "standard_object\n{\n name "+name+"\n geometry "+geometry+"\n material "+material+"\n"+extra+"}\n";
}

// Detect the optional interface itself: committed headers may predate its
// feature macro. The test-only fallback is invisible to older solvers.
namespace RISE { struct SMSIntersectionDifferential; class ISMSModifierDifferential; class ISMSUVDifferential; }
template<class T,class=void> struct SMSCompleteType : std::false_type {};
template<class T> struct SMSCompleteType<T,std::void_t<decltype(sizeof(T))>> : std::true_type {};
struct SMSTestDifferentialFallback {
    Vector3 worldPoint{0,0,0},objectPoint{0,0,0},normal{0,0,0},geometricNormal{0,0,0};
    Vector3 frameU{0,0,0},frameV{0,0,0},frameW{0,0,0},rayOrigin{0,0,0},rayDirection{0,0,0};
    Point2 uv{0,0};
};
using TestSMSIntersectionDifferential=std::conditional_t<SMSCompleteType<RISE::SMSIntersectionDifferential>::value,
    RISE::SMSIntersectionDifferential,SMSTestDifferentialFallback>;
class SMSTestModifierFallback {
public:
    virtual ~SMSTestModifierFallback()=default;
    virtual bool HasSMSDifferentialContract()const=0;
    virtual bool SMSFrameDifferential(const RayIntersectionGeometric&,
        const TestSMSIntersectionDifferential&,Vector3&,Vector3&)const=0;
};
using TestSMSModifierDifferential=std::conditional_t<SMSCompleteType<RISE::ISMSModifierDifferential>::value,
    RISE::ISMSModifierDifferential,SMSTestModifierFallback>;
class SMSTestUVDifferentialFallback {
public:
    virtual ~SMSTestUVDifferentialFallback()=default;
    virtual bool HasSMSUVDifferentialContract()const=0;
    virtual bool SMSUVDifferential(const Point3&,const Vector3&,const Vector3&,const Vector3&,Point2&)const=0;
};
using TestSMSUVDifferential=std::conditional_t<SMSCompleteType<RISE::ISMSUVDifferential>::value,
    RISE::ISMSUVDifferential,SMSTestUVDifferentialFallback>;
// Older geometry headers still compile the native tessellation witness.
template<class T> static auto OrientationAudits(const T* g,int)->decltype(g->SMSOrientationAudits()) {return g->SMSOrientationAudits();}
template<class T> static unsigned long long OrientationAudits(const T*,long) {return 0;}
template<class T> static auto OrientationVisits(const T* g,int)->decltype(g->SMSOrientationTriangleVisits()) {return g->SMSOrientationTriangleVisits();}
template<class T> static unsigned long long OrientationVisits(const T*,long) {return 0;}

static std::string QuadMesh(const std::string& name,Scalar z,Scalar x0,Scalar x1,bool reverse) {
    std::ostringstream s;s<<std::setprecision(17);
    s<<"indexedmesh_geometry\n{\n name "<<name<<"\n double_sided TRUE\n face_normals TRUE\n";
    s<<" vertex "<<x0<<" -2 "<<z<<"\n vertex "<<x1<<" -2 "<<z
        <<"\n vertex "<<x1<<" 2 "<<z<<"\n vertex "<<x0<<" 2 "<<z
        <<"\n uv 0 0\n uv 1 0\n uv 1 1\n uv 0 1\n";
    s<<(reverse?" triangle 0 2 1\n triangle 0 3 2\n":" triangle 0 1 2\n triangle 0 2 3\n");
    return s.str()+"}\n";
}

#ifdef RISE_SMS_REFERENCE_A
struct LawRoot { int id=-1; unsigned channel=0; };
static LawRoot ThreeEventLaw(ISampler& sampler,unsigned c,bool rare) {
    if(rare) {
        const Scalar draw=sampler.Get1D();
        return {draw<1./3 ? 0 : draw<1./3+1./512 ? 1 : -1,c};
    }
    const bool firstCaster=sampler.Get1D()<.3;
    const Scalar pr[]={.2,.4,.8}, pt[]={.6,.8,.3};
    const bool r=sampler.Get1D()<pr[c];
    if(r) return {sampler.Get1D()<pt[c] && firstCaster ? 0 : -1,c};
    if(sampler.Get1D()>=.3) return {-1,c};
    return {sampler.Get1D()<.7 && !firstCaster ? 1 : -1,c};
}
static void Synthetic() {
    for(bool rare:{false,true}) for(bool roulette:{false,true}) {
        std::array<std::vector<double>,3> samples;
        unsigned long long totalTails=0, totalStops=0;
        for(unsigned salt=0;salt<4;++salt) {
            RandomNumberGenerator dr(SobolSequence::HashCombine(4000+salt,rare?0x52415245:0x52544c57));
            RandomNumberGenerator rr(SobolSequence::HashCombine(8000+salt,0x4b4c4f50));
            IndependentSampler discovery(dr), retry(rr);
            SMSReferenceCounters counters;
            const unsigned N=rare && roulette ? 100000000 : 2000000;
            std::array<double,3> sums{}; Scalar maxReciprocal=0, maxDeposit=0;
            for(unsigned n=0;n<N;++n) {
                const unsigned c=std::min(2u,static_cast<unsigned>(discovery.Get1D()*3));
                const auto proposal=[c,rare](ISampler& s){return ThreeEventLaw(s,c,rare);};
                const LawRoot root=proposal(discovery);
                if(root.id<0) continue; // still included in N
                const Scalar k=SMSRootReference::Reciprocal(root,retry,proposal,
                    [](const LawRoot& a,const LawRoot& b){return b.id>=0&&a.id==b.id&&a.channel==b.channel;},
                    64,roulette,&counters);
                const Scalar physical=root.id==0 ? Scalar(1+c) : Scalar(4+2*c);
                const Scalar deposit=SMSRootReference::Deposit(physical,k,Scalar(1)/3,.4,N);
                sums[c]+=deposit; maxReciprocal=std::max(maxReciprocal,k); maxDeposit=std::max(maxDeposit,deposit);
            }
            for(unsigned c=0;c<3;++c) samples[c].push_back(sums[c]);
            totalTails+=counters.tailTrials.load();totalStops+=counters.rouletteStops.load();
            std::cout<<"A-retries rare="<<rare<<" roulette="<<roulette<<" salt="<<salt<<" N="<<N
                <<" trials="<<counters.retryTrials.load()<<" tails="<<counters.tailTrials.load()
                <<" stops="<<counters.rouletteStops.load()<<" maxReciprocal="<<maxReciprocal<<" maxDeposit="<<maxDeposit<<" histogram=";
            for(const auto& bucket:counters.retryHistogram) std::cout<<bucket.load()<<',';
            std::cout<<'\n';
        }
        for(unsigned c=0;c<3;++c) {
            const Moments m(samples[c]); const double reference=(5+3*c)/.4;
            std::cout<<"A-law rare="<<rare<<" roulette="<<roulette<<" c="<<c<<" mean="<<m.mean
                <<" sd="<<m.sd<<" n=4 analytic="<<reference<<" reference sd=0\n";
            Check(std::fabs(m.mean-reference)<=3*m.sd,"two-root/three-event law includes zeros, channel and emitter factors within 3 sd");
            Check(std::fabs(m.mean/reference-1)<.06,"independent accuracy bound for synthetic accounting");
        }
        if(roulette && rare) Check(totalTails>0&&totalStops>0,"rare-root weighted tails are exercised");
    }
    Check(SMSRootReference::Deposit(6,2,.5,.25,4)==24,"reference accounting has exactly channel/emitter/N factors");
    Check(SMSRootReference::Deposit(-6,2,.5,.25,4)==-24,"reference accounting preserves signed native emission");
    Check(SMSRootReference::Deposit(0,2,.5,.25,4)==0,"zero physical root remains zero");
    Check(ManifoldSolver::ExtendedReflectionProbability(false,true,.6,false,.05)==0,"transmission-only proposal");
    Check(ManifoldSolver::ExtendedReflectionProbability(true,false,.6,false,.05)==1,"mirror-only proposal");
    Check(ManifoldSolver::ExtendedReflectionProbability(true,true,0,false,.05)==.05,"seed zero does not remove reflection mass");
    Check(ManifoldSolver::ExtendedReflectionProbability(true,true,1,false,.05)==.95,"seed zero does not remove transmission mass");
    Check(ManifoldSolver::ExtendedReflectionProbability(true,true,.2,true,.05)==1,"TIR has only reflection");
    Check(!std::isfinite(ManifoldSolver::ExtendedReflectionProbability(false,true,1,true,.05)),"TIR never relabels an unsupported reflection");
}

// Two nearby, regular reflection roots on one disconnected indexed mesh.
// Their positions and normals fit the legacy one-percent equivalence band.
static std::string CloseRootMesh(bool reverse, Scalar scale, Scalar separation=.001, Scalar offset=0) {
    std::ostringstream s; s<<std::setprecision(17);
    s<<"indexedmesh_geometry\n{\n name shape\n double_sided TRUE\n face_normals TRUE\n";
    for(int side:{-1,1}) {
        const Scalar x=side*separation;
        const Point3 center(x,0,0), a(-.5,0,-3), b(.5,0,-3);
        const Vector3 n=Vector3Ops::Normalize(Vector3Ops::Normalize(Vector3Ops::mkVector3(a,center))
            +Vector3Ops::Normalize(Vector3Ops::mkVector3(b,center)));
        for(const Point2& p:{Point2(-.4*separation,-1),Point2(.4*separation,-1),Point2(.4*separation,2),Point2(-.4*separation,2)})
            s<<" vertex "<<offset+scale*(x+p.x)<<' '<<scale*p.y<<' '<<scale*(-n.x*p.x/n.z)<<'\n';
    }
    for(int i=0;i<2;++i) s<<" uv 0 0\n uv 1 0\n uv 1 1\n uv 0 1\n";
    for(int offset:{0,4}) {
        s<<" triangle "<<offset<<' '<<offset+(reverse?2:1)<<' '<<offset+(reverse?1:2)<<'\n';
        s<<" triangle "<<offset<<' '<<offset+(reverse?3:2)<<' '<<offset+(reverse?2:3)<<'\n';
    }
    return s.str()+"}\n";
}
static void CloseRoots() {
    for(bool winding:{false,true}) for(Scalar scale:{.01,1.,100.}) for(Scalar separation:{.001,1e-9}) {
        if(separation < .001 && scale < 1) continue;
        Fixture f(Materials()+CloseRootMesh(winding,scale,separation)+SceneObject("caster","shape","mirror"));
        ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=1;cfg.solverThreshold=1e-10;
        auto* solver=new ManifoldSolver(cfg);IORStack air(1);
        ScriptSampler left({.2,winding?5./9:11./36,winding?.25:.4,.1}),right({.2,winding?5./9:11./36,winding?.25:.4,.6});
        const Point3 start(-.5*scale,0,-3*scale),end(.5*scale,0,-3*scale);
        const auto a=solver->ProposeExtendedRoot(start,Vector3(0,0,1),end,f.Scene(),air,SMSQueryDomain::RGB(0),left);
        const auto b=solver->ProposeExtendedRoot(start,Vector3(0,0,1),end,f.Scene(),air,SMSQueryDomain::RGB(0),right);
        std::cout<<"close solve scale="<<scale<<" input separation="<<separation<<" winding="<<winding<<" accepted="<<a.accepted<<','<<b.accepted<<" solved="<<a.result.valid<<','<<b.result.valid<<'\n';
        if(separation >= .001)
            Check(a.accepted&&b.accepted,"nearby mesh roots solve independently at changed scene scales");
        else Check(!a.accepted&&!b.accepted,"unresolved thin native mesh patches are consistent zero proposals");
        if(a.accepted&&b.accepted) {
            std::cout<<"close roots scale="<<scale<<" input separation="<<separation<<" winding="<<winding<<" separation="
                <<Point3Ops::Distance(a.vertices[0].geometry.position,b.vertices[0].geometry.position)<<'\n';
            Check(!ManifoldSolver::SameExtendedRoot(a,b,cfg.uniquenessThreshold*a.scale),
                "production root identity separates nearby physical roots");
        }
        solver->release();
    }
}
// Regular native sphere/ellipsoid/torus/cylinder roots lie at periodic
// chart seams. Independent seeds approach from both sides. Varying seam
// contexts are uncertain; authored UV-generator endpoints are preserved.
class SeamTint final : public UniformColorPainter {
public:
    SeamTint() : UniformColorPainter(RISEPel(1)) {}
    RISEPel GetColor(const RayIntersectionGeometric& hit) const override {
        return RISEPel(hit.ptCoord.x<.5?.25:.75);
    }
    Scalar GetColorNM(const RayIntersectionGeometric& hit,Scalar) const override {
        return hit.ptCoord.x<.5?.25:.75;
    }
};
class FixedEndpointUV final : public IUVGenerator, public Reference {
public:
    void GenerateUV(const Point3&,const Vector3&,Point2& uv) const override { uv=Point2(1,.5); }
};
static std::string UVSeamMesh(bool reverse, bool interior=false) {
    std::ostringstream s;
    s<<"indexedmesh_geometry\n{\n name sphere\n double_sided TRUE\n face_normals TRUE\n";
    for(Scalar x:{-2.,0.}) s<<" vertex "<<x<<" -2 0\n vertex "<<x+2<<" -2 0\n vertex "<<x+2<<" 2 0\n vertex "<<x<<" 2 0\n";
    for(unsigned i=0;i<2;++i) {
        const Scalar lo=interior?.25:0,hi=interior?.75:1;
        s<<" uv "<<lo<<" 0\n uv "<<hi<<" 0\n uv "<<hi<<" 1\n uv "<<lo<<" 1\n";
    }
    for(unsigned offset:{0u,4u}) {
        s<<" triangle "<<offset<<' '<<offset+(reverse?2:1)<<' '<<offset+(reverse?1:2)<<'\n';
        s<<" triangle "<<offset<<' '<<offset+(reverse?3:2)<<' '<<offset+(reverse?2:3)<<'\n';
    }
    return s.str()+"}\n";
}
static void NativePeriodicRoots() {
    for(unsigned pricing:{0u,1u,2u}) for(unsigned shape:{0u,1u,2u,3u,4u,5u}) {
    const bool textured=pricing!=0;
    const char* geometry[]={"sphere_geometry\n{\n name sphere\n radius 1\n}\n",
        "ellipsoid_geometry\n{\n name sphere\n radii 1 1 1\n}\n",
        "torus_geometry\n{\n name sphere\n majorradius 1\n minorratio 0.5\n}\n",
        "cylinder_geometry\n{\n name sphere\n axis y\n radius 1\n height 4\n capped TRUE\n}\n"};
    Fixture f(Materials()+(shape<4?geometry[shape]:UVSeamMesh(shape==5))+SceneObject("caster","sphere","mirror"));
    ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=1;
    auto* solver=new ManifoldSolver(cfg);IORStack air(1);
    if(textured) {
        auto* tint=new SeamTint();IMaterial* material=nullptr;
        Check(RISE_API_CreatePerfectReflectorMaterial(&material,*tint),"discontinuous seam tint material created");
        if(material) f.job->GetObjects()->GetItem("caster")->AssignMaterial(*material);
        safe_release(material);tint->release();
    }
    if(pricing==2) {
        auto* mapping=new FixedEndpointUV();f.job->GetObjects()->GetItem("caster")->SetUVGenerator(*mapping);mapping->release();
    }
    RandomNumberGenerator random(217);IndependentSampler sampler(random);
    const Scalar sign=shape<2?-1:1, radius=shape==2?1.5:1;
    const Point3 start=shape<4?Point3(3*sign,.5,0):Point3(-.5,0,-3);
    const Point3 end=shape<4?Point3(3*sign,-.5,0):Point3(.5,0,-3);
    const Point3 expected=shape<4?Point3(sign*radius,0,0):Point3(0,0,0);
    std::vector<SMSDomainRoot> roots;
    for(unsigned i=0;i<4096&&roots.size()<20;++i) {
        auto root=solver->ProposeExtendedRoot(start,shape<4?Vector3(-sign,0,0):Vector3(0,0,1),end,
            f.Scene(),air,SMSQueryDomain::RGB(0),sampler);
        if(root.accepted) roots.push_back(std::move(root));
    }
    std::cout<<"native periodic seam shape="<<shape<<" pricing="<<pricing<<" accepted="<<roots.size()<<'\n';
    if(pricing==1) {
        Check(roots.empty(),"varying periodic seam contexts are uncertain zero proposals");
        solver->release();continue;
    }
    Check(roots.size()==20,"native periodic seam has positive regular root coverage");
    bool positiveZ=false,negativeZ=false;
    for(const auto& root:roots) {
        const auto& vertex=root.vertices[0].geometry;
        const Scalar side=shape<4?vertex.position.z:vertex.position.x;
        positiveZ|=side>0;negativeZ|=side<0;
        Check(Point3Ops::Distance(vertex.position,expected)<1e-8,"periodic seam root matches analytic reflection point");
        Check(vertex.uv.x==root.vertices[0].context.ptCoord.x && (pricing!=2 || vertex.uv.x==1),
            "periodic matching preserves actual native and override material coordinates");
        Check(root.result.contributionNM==(textured?.75:1),
            "periodic seam throughput preserves the actual native or overridden context");
        Check(ManifoldSolver::SameExtendedRoot(roots[0],root,1e-6),
            "opposite native periodic chart sides identify the same physical root");
    }
    Check(positiveZ&&negativeZ,"independent seeds exercise both sides of a native periodic seam");
    if(roots.size()>1) {
        std::vector<double> values;
        for(unsigned salt=0;salt<4;++salt) {
            RandomNumberGenerator discoveryRandom(SobolSequence::HashCombine(17000+salt,0x5345414d));
            RandomNumberGenerator retryRandom(SobolSequence::HashCombine(17000+salt,0x52455452));
            IndependentSampler discovery(discoveryRandom),retry(retryRandom);
            const auto proposal=[&](ISampler& stream){return roots[stream.Get1D()<.5?0:1];};
            Scalar sum=0;constexpr unsigned N=65536;
            for(unsigned i=0;i<N;++i) {
                const auto root=proposal(discovery);
                const Scalar k=SMSRootReference::Reciprocal(root,retry,proposal,
                    [](const SMSDomainRoot& a,const SMSDomainRoot& b){return ManifoldSolver::SameExtendedRoot(a,b,1e-6);},64,false,nullptr);
                sum+=SMSRootReference::Deposit(1,k,1,1,N);
            }
            values.push_back(sum);
        }
        const Moments m(values);
        std::cout<<"native periodic seam two-seed accounting shape="<<shape<<" pricing="<<pricing<<" mean="<<m.mean<<" sd="<<m.sd<<" n=4 analytic root count=1 reference sd=0\n";
        Check(std::fabs(m.mean-1)<=3*m.sd+64*std::numeric_limits<Scalar>::epsilon(),
            "native chart aliases contribute one physical root in reciprocal accounting");
        auto changed=roots[0];changed.vertices[0].geometry.uv.x=.5;
        Check(!ManifoldSolver::SameExtendedRoot(roots[0],changed,1e-6),
            "UV records must remain coherent with their real material context");
        changed.vertices[0].context.ptCoord.x=.5;
        Check(ManifoldSolver::SameExtendedRoot(roots[0],changed,1e-6)==(pricing==0),
            "UV equivalence follows the audited constant law; varying override contexts remain distinct");
    }
    solver->release();
    }
}
// A smooth price does not make two authored atlas records two roots.
// Until context equivalence is certified, an interior-valued chart boundary
// must be rejected just like an authored 0/1 boundary.
class WorldTint final : public UniformColorPainter {
public:
    WorldTint() : UniformColorPainter(RISEPel(1)) {}
    RISEPel GetColor(const RayIntersectionGeometric& hit) const override { return RISEPel(.5+.1*hit.ptIntersection.x); }
    Scalar GetColorNM(const RayIntersectionGeometric& hit,Scalar) const override { return .5+.1*hit.ptIntersection.x; }
};
// Generated charts have their own seams, even where native geometry UVs
// are continuous. Fixed generated coordinates remain a positive control.
class InteriorStepUV final : public IUVGenerator, public Reference {
public:
    void GenerateUV(const Point3& point,const Vector3&,Point2& uv) const override {
        uv=Point2(point.x<0?.25:.75,.5);
    }
};
// Audited fixture providers implement the same optional production contract.
static Vector3 FixtureNormalizedDifferential(const Vector3& value,const Vector3& d) {
    const Scalar length=Vector3Ops::Magnitude(value);const Vector3 n=value*(1/length);
    return (d-n*Vector3Ops::Dot(n,d))*(1/length);
}
class AuditedIdentityFrameModifier : public IRayIntersectionModifier, public Reference,
    public TestSMSModifierDifferential {
public:
    bool HasSMSDifferentialContract() const override {return true;}
    virtual bool SMSFrameDependsOnUV() const
#ifdef RISE_SMS_COMPOSED_DIFFERENTIAL
        override
#endif
        {return false;}
    bool SMSFrameDifferential(const RayIntersectionGeometric&,const TestSMSIntersectionDifferential& d,
        Vector3& n,Vector3& w) const override {n=d.normal;w=d.frameW;return true;}
};
class PostModifierUV final : public AuditedIdentityFrameModifier {
    const bool discontinuous;
public:
    explicit PostModifierUV(bool step) : discontinuous(step) {}
    bool HasSMSDifferentialContract() const override {return !discontinuous;}
    void Modify(RayIntersectionGeometric& hit) const override {
        hit.ptCoord=Point2(discontinuous ? (hit.ptObjIntersec.x<0?.25:.75) : .5,.5);
    }
};
class PostModifierNormal final : public AuditedIdentityFrameModifier {
    const bool discontinuous;
public:
    explicit PostModifierNormal(bool step) : discontinuous(step) {}
    bool HasSMSDifferentialContract() const override {return !discontinuous;}
    bool SMSFrameDifferential(const RayIntersectionGeometric& raw,const TestSMSIntersectionDifferential& d,
        Vector3& n,Vector3& w) const override {
        if(discontinuous) return false;
        n=FixtureNormalizedDifferential(raw.vNormal*.5+Vector3(std::sqrt(Scalar(3))*.5,0,0),d.normal*.5);
        w=n;return true;
    }
    void Modify(RayIntersectionGeometric& hit) const override {
        const Scalar sign=discontinuous && hit.ptObjIntersec.x<0?-1:1;
        hit.vNormal=Vector3Ops::Normalize(hit.vNormal*.5+Vector3(sign*std::sqrt(Scalar(3))*.5,0,0));
        hit.onb.CreateFromW(hit.vNormal);hit.ptCoord=Point2(.5,.5);
    }
};
static void PostModifierChartRoots() {
    for(bool reverse:{false,true}) for(int side:{-1,1}) for(bool transformed:{false,true})
        for(bool step:{false,true}) for(bool normalJump:{false,true}) {
        const Scalar scale=transformed?1.5:1,offset=transformed?3:0;
        const std::string transform=transformed?" scale 1.5 1.5 1.5\n position 3 0 0\n":"";
        Fixture f(Materials()+QuadMesh("patch",0,-2,2,reverse)+SceneObject("caster","patch","mirror",transform));
        auto* tint=new WorldTint();IMaterial* material=nullptr;
        Check(RISE_API_CreatePerfectReflectorMaterial(&material,*tint),"post-modifier world tint created");
        if(material) f.job->GetObjects()->GetItem("caster")->AssignMaterial(*material);
        safe_release(material);tint->release();
        IRayIntersectionModifier* modifier=normalJump
            ? static_cast<IRayIntersectionModifier*>(new PostModifierNormal(step))
            : static_cast<IRayIntersectionModifier*>(new PostModifierUV(step));
        f.job->GetObjects()->GetItem("caster")->AssignModifier(*modifier);modifier->release();
        const Point3 start(offset-.5*scale,.3*scale,side*3*scale),end(offset+.5*scale,.3*scale,side*3*scale);
        ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=1;
        auto* solver=new ManifoldSolver(cfg);IORStack air(1);
        for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),SMSQueryDomain::NM(450),SMSQueryDomain::NM(650)}) {
            RandomNumberGenerator random(217);IndependentSampler sampler(random);
            unsigned accepted=0;bool lower=false,upper=false;
            for(unsigned i=0;i<512;++i) {
                const auto root=solver->ProposeExtendedRoot(start,Vector3(0,0,-side),end,f.Scene(),air,domain,sampler);
                if(!root.accepted) continue;
                ++accepted;const auto& vertex=root.vertices[0];
                Check(std::fabs(root.result.contributionNM-(.5+.1*offset))<1e-8,
                    "post-modifier families retain the same physical event price");
                if(normalJump && accepted==1) {
                    ScatteredRayContainer rays;
                    if(domain.kind==SMSQueryDomain::Wavelength) vertex.geometry.pMaterial->GetSPF()->ScatterNM(
                        vertex.context,sampler,domain.nm,rays,air);
                    else vertex.geometry.pMaterial->GetSPF()->Scatter(vertex.context,sampler,rays,air);
                    Check(rays.Count()==1 && Vector3Ops::Magnitude(rays[0].ray.Dir()-Vector3Ops::Normalize(
                        Vector3Ops::mkVector3(end,vertex.geometry.position)))<1e-8,
                        "native geometric-horizon fallback reaches the same reflection endpoint");
                }
                lower|=normalJump?vertex.context.vNormal.x<0:vertex.context.ptCoord.x==.25;
                upper|=normalJump?vertex.context.vNormal.x>0:vertex.context.ptCoord.x==.75;
                Check(Point3Ops::Distance(vertex.geometry.position,Point3(offset,.3*scale,0))<1e-8,
                    "post-modifier chart proposals converge to the same physical reflection point");
                Check(vertex.geometry.uv.x==vertex.context.ptCoord.x,
                    "post-modifier chart stores actual material coordinates");
                if(!step || normalJump) Check(vertex.context.ptCoord.x==.5,"continuous UV modifier retains its actual coordinates");
            }
            std::cout<<"post-modifier chart winding="<<reverse<<" side="<<side<<" transformed="<<transformed
                <<" step="<<step<<" normal_jump="<<normalJump<<" accepted="<<accepted<<" lower="<<lower<<" upper="<<upper<<'\n';
            Check(step?accepted==0:accepted>0,
                "post-modifier context discontinuity is uncertain while continuous modifier retains positive coverage");
        }
        solver->release();
    }
}
static void GeneratedChartRoots() {
    for(unsigned kind:{0u,1u,2u,3u,4u}) for(bool reverse:{false,true}) {
        const std::string geometry=kind==0 || kind==3
            ? "sphere_geometry\n{\n name sphere\n radius 1\n}\n"
            : kind==1 ? "cylinder_geometry\n{\n name sphere\n axis y\n radius 1\n height 4\n capped TRUE\n}\n"
            : UVSeamMesh(reverse,true);
        Fixture f(Materials()+geometry+SceneObject("caster","sphere","mirror"));
        auto* tint=new WorldTint();IMaterial* material=nullptr;
        Check(RISE_API_CreatePerfectReflectorMaterial(&material,*tint),"generated-chart world tint created");
        if(material) f.job->GetObjects()->GetItem("caster")->AssignMaterial(*material);
        safe_release(material);tint->release();
        IUVGenerator* mapping=kind==0 ? static_cast<IUVGenerator*>(new SphericalUVGenerator(1))
            : kind==1 ? static_cast<IUVGenerator*>(new CylindricalUVGenerator(1,'y',4))
            : kind==2 ? static_cast<IUVGenerator*>(new InteriorStepUV())
            : static_cast<IUVGenerator*>(new FixedEndpointUV());
        f.job->GetObjects()->GetItem("caster")->SetUVGenerator(*mapping);mapping->release();
        const Scalar sign=kind==1?1:-1;
        const Point3 start=kind==2?Point3(-.5,0,-3):Point3(sign*3,.5,0);
        const Point3 end=kind==2?Point3(.5,0,-3):Point3(sign*3,-.5,0);
        ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=1;
        auto* solver=new ManifoldSolver(cfg);IORStack air(1);
        for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),SMSQueryDomain::NM(450),SMSQueryDomain::NM(650)}) {
            RandomNumberGenerator random(217);IndependentSampler sampler(random);unsigned accepted=0;
            for(unsigned i=0;i<512;++i) {
                const auto root=solver->ProposeExtendedRoot(start,kind==2?Vector3(0,0,1):Vector3(-sign,0,0),end,f.Scene(),air,domain,sampler);
                accepted+=root.accepted;
                if(root.accepted && kind==3) Check(root.vertices[0].context.ptCoord.x==1,
                    "continuous generated-chart control preserves actual endpoint UV");
            }
            std::cout<<"generated chart kind="<<kind<<" winding="<<reverse<<" accepted="<<accepted<<'\n';
            Check(kind==3?accepted>0:accepted==0,
                "generated seam is uncertain while continuous generated chart retains positive coverage");
        }
        solver->release();
    }
}
static void InteriorAtlasRoots() {
    for(bool reverse:{false,true}) for(int side:{-1,1}) for(bool varying:{false,true}) for(unsigned geometryKind:{0u,1u,2u}) {
        Fixture f(Materials()+UVSeamMesh(reverse,true)+SceneObject("caster","sphere","mirror"));
        if(geometryKind==1) {
            const auto* object=f.Object("caster");
            IndexTriangleListType indices;VerticesListType positions;NormalsListType normals;TexCoordsListType coords;
            Check(object->GetGeometry()->TessellateToMesh(indices,positions,normals,coords,1),"atlas indexed source tessellates");
            ITriangleMeshGeometry* geometry=nullptr;
            Check(RISE_API_CreateTriangleMeshGeometry(&geometry,true),"non-indexed atlas sibling created");
            if(geometry) {
                geometry->BeginTriangles();
                for(const auto& index:indices) {
                    Triangle triangle;
                    for(unsigned k=0;k<3;++k) {triangle.vertices[k]=positions[index.iVertices[k]];triangle.normals[k]=normals[index.iNormals[k]];triangle.coords[k]=coords[index.iCoords[k]];}
                    geometry->AddTriangle(triangle);
                }
                geometry->DoneTriangles();f.job->GetObjects()->GetItem("caster")->AssignGeometry(*geometry);
                safe_release(geometry);f.job->GetObjects()->PrepareForRendering();
            }
        }
        if(geometryKind==2) {
            IGeometry* displaced=nullptr;
            auto* height=new UniformScalarPainter(.1);
            Check(RISE_API_CreateDisplacedGeometry(&displaced,f.job->GetGeometries()->GetItem("sphere"),
                1,nullptr,1,true,true,false,height),"displaced atlas sibling created");
            if(displaced) {
                displaced->Realize();f.job->GetObjects()->GetItem("caster")->AssignGeometry(*displaced);
                safe_release(displaced);f.job->GetObjects()->PrepareForRendering();
            }
            height->release();
        }
        if(varying) {
            auto* tint=new WorldTint(); IMaterial* material=nullptr;
            Check(RISE_API_CreatePerfectReflectorMaterial(&material,*tint),"smooth world tint material created");
            if(material) f.job->GetObjects()->GetItem("caster")->AssignMaterial(*material);
            safe_release(material);tint->release();
        }
        ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=1;
        auto* solver=new ManifoldSolver(cfg); IORStack air(1);
        const Point3 start(-.5,0,side*3),end(.5,0,side*3);
        RandomNumberGenerator random(217); IndependentSampler sampler(random);
        for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),SMSQueryDomain::NM(450),SMSQueryDomain::NM(650)}) {
            unsigned accepted=0;
            for(unsigned i=0;i<512;++i) {
                auto root=solver->ProposeExtendedRoot(start,Vector3(0,0,-side),end,f.Scene(),air,domain,sampler);
                accepted+=root.accepted;
            }
            std::cout<<"interior atlas winding="<<reverse<<" side="<<side<<" varying="<<varying<<" geometry="<<geometryKind<<" accepted="<<accepted<<'\n';
            Check(varying?accepted==0:accepted>0,"interior atlas uncertainty rejects variable law while audited constant law remains supported");
        }
        solver->release();
    }
}
class TiltNormal final : public AuditedIdentityFrameModifier {
public:
    explicit TiltNormal(int side, bool valid=false, bool varying=false, unsigned fields=0) : side(side), valid(valid), varying(varying), fields(fields) {}
    void Modify(RayIntersectionGeometric& hit) const override {
        const Scalar angle=(valid?-10:60)*PI/180+(varying?.01*(hit.ptIntersection.x-5)+.025*hit.ray.Dir().x+.0001*hit.rast.x:0);
        const Vector3 normal(std::sin(angle),0,side*std::cos(angle));
        if(fields!=1) hit.vNormal=normal;
        if(fields!=2) hit.onb.CreateFromW(normal);
    }
    bool SMSFrameDifferential(const RayIntersectionGeometric& raw,const TestSMSIntersectionDifferential& d,
        Vector3& n,Vector3& w) const override {
        const Scalar angle=(valid?-10:60)*PI/180+(varying?.01*(raw.ptIntersection.x-5)+.025*raw.ray.Dir().x+.0001*raw.rast.x:0);
        const Scalar da=varying?.01*d.worldPoint.x+.025*d.rayDirection.x:0;
        const Vector3 derivative(std::cos(angle)*da,0,-side*std::sin(angle)*da);
        n=fields==1?d.normal:derivative;w=fields==2?d.frameW:derivative;return true;
    }

private:
    int side;bool valid,varying;unsigned fields;
};

class NativeConstraintOracle final : public ManifoldSolver {
public:
    NativeConstraintOracle(const ManifoldSolverConfig& cfg,const std::vector<SMSDomainVertex>* contexts)
        #ifdef RISE_SMS_NATIVE_EVENT_NORMALS
        : ManifoldSolver(cfg,true,contexts)
#else
        : ManifoldSolver(cfg)
#endif
        { (void)contexts; }
    using ManifoldSolver::EvaluateConstraint;
    using ManifoldSolver::BuildJacobian;
    using ManifoldSolver::ComputeLightToFirstVertexJacobianDet;
    using ManifoldSolver::ComputeBlockTridiagonalDeterminant;
    using ManifoldSolver::SolveBlockTridiagonal;
    using ManifoldSolver::UpdateVertexOnSurface;
    template<class T> static auto Project(const T& oracle,ManifoldVertex& v,Scalar du,Scalar dv,int)
        ->decltype(oracle.UpdateVertexOnSurface(v,du,dv,0,true)) {
        return oracle.UpdateVertexOnSurface(v,du,dv,0,true);
    }
    template<class T> static auto Project(const T& oracle,ManifoldVertex& v,Scalar du,Scalar dv,long)
        ->decltype(oracle.UpdateVertexOnSurface(v,du,dv,0)) {
        return oracle.UpdateVertexOnSurface(v,du,dv,0);
    }
    bool ProjectIndependent(ManifoldVertex& v,Scalar du,Scalar dv) const {return Project(*this,v,du,dv,0);}
};

#ifdef RISE_SMS_SCRATCH_COUNTERS
class AuditedFactorizationProbe final : public AuditedIdentityFrameModifier {
    const NativeConstraintOracle& oracle;
    SMSReferenceCounters& counters;
    unsigned maximum;
    mutable bool tested=false;
    void Exercise(unsigned k) const {
        std::vector<Scalar> diagonal(4*k,0),upper(4*(k-1),0),lower(4*(k-1),0),rhs(2*k),delta;
        for(unsigned i=0;i<k;++i) {diagonal[4*i]=1;diagonal[4*i+3]=1;}
        for(unsigned i=0;i<2*k;++i) rhs[i]=i+1;
        Check(oracle.ComputeBlockTridiagonalDeterminant(diagonal,upper,lower,k)==1,
            "growing block systems retain the independently known determinant");
        Check(oracle.SolveBlockTridiagonal(diagonal,upper,lower,rhs,k,delta)&&delta==rhs,
            "growing block systems retain the independently known solution");
    }
public:
    AuditedFactorizationProbe(const NativeConstraintOracle& o,SMSReferenceCounters& c,unsigned depth)
        : oracle(o),counters(c),maximum(depth) {}
    void Modify(RayIntersectionGeometric&) const override {
        if(tested||counters.scratchFrames.load()==0) return;
        tested=true;
        Exercise(1);
        const auto warm=counters.scratchBufferGrowths.load();
        for(unsigned k=2;k<=maximum;++k) {
            Exercise(k);
            std::cout<<"R8 factorization k="<<k<<" max="<<maximum
                <<" warmedGrowths="<<warm<<" currentGrowths="<<counters.scratchBufferGrowths.load()<<'\n';
            Check(counters.scratchBufferGrowths.load()==warm,
                "both factorization helpers reserve configured maximum on their first native trial");
        }
    }
    bool Ran() const {return tested;}
};
static void NativeFactorizationReservation() {
    for(bool reverse:{false,true}) for(int side:{-1,1}) {
        // A new native worker prevents preceding modes from warming the TLS pool.
        std::thread worker([=] {
            Fixture f(Materials()+QuadMesh("patch",0,-2,3,reverse)+SceneObject("caster","patch","mirror"));
            SMSReferenceCounters counters;
            ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=1;cfg.maxChainDepth=8;
            cfg.referenceCounters=&counters;
            NativeConstraintOracle oracle(cfg,nullptr);
            auto* modifier=new AuditedFactorizationProbe(oracle,counters,cfg.maxChainDepth);
            f.job->GetObjects()->GetItem("caster")->AssignModifier(*modifier);
            const Point3 start(-.1,.2,side),center(.3,.2,0),end(.7,.2,side);IORStack air(1);
            RayIntersection hit(Ray(start,Vector3Ops::Normalize(Vector3Ops::mkVector3(center,start))),nullRasterizerState);
            f.Scene().GetObjects()->IntersectRay(hit,true,true,false);
            Check(hit.geometric.bHit,"cold-worker factorization probe uses an actual indexed hit");
            if(hit.geometric.bHit) {
                SMSDomainVertex vertex(hit.geometric);
                vertex.geometry.position=center;vertex.geometry.normal=hit.geometric.vNormal;
                vertex.geometry.geomNormal=hit.geometric.UnflippedGeomNormal();vertex.geometry.pObject=hit.pObject;
                vertex.geometry.pMaterial=hit.pMaterial;vertex.geometry.isReflection=true;
                std::vector<SMSDomainVertex> records{vertex};RandomNumberGenerator random(798);IndependentSampler sampler(random);
                auto* solver=new ManifoldSolver(cfg);
                const auto result=solver->SolveDomain(start,Vector3(0,0,-side),end,Vector3(0,0,-side),
                    f.Scene(),air,SMSQueryDomain::RGB(0),records,sampler,1e-7,1e-10);
                Check(result.valid,"native trial retains its mirror root around factorization probes");
                Check(modifier->Ran(),"factorization probes execute inside native worker scratch nesting");
                solver->release();
            }
            modifier->release();
        });
        worker.join();
    }
}
#endif

static void CheckNativeHorizonJacobian(const ManifoldSolverConfig& cfg,const ManifoldResult& result,
    const std::vector<SMSDomainVertex>& vertices,const Point3& start,const Point3& end,
    const IScene& scene,const IORStack& stack,SMSQueryDomain domain,ISampler& sampler,bool curved=false) {
    NativeConstraintOracle oracle(cfg,&vertices);
    const auto& chain=result.specularChain;
    std::vector<Scalar> diagonal,upper,lower;
    oracle.BuildJacobian(chain,start,end,diagonal,upper,lower,true);
    const Scalar h=1e-4*Point3Ops::Distance(start,end);
    for(std::size_t column=0;column<2*chain.size();++column) {
        auto plus=chain,minus=chain;const std::size_t j=column/2;
        const Vector3 tangent=column%2?chain[j].dpdv:chain[j].dpdu;
        if(curved) {
            Check(oracle.ProjectIndependent(plus[j],column%2?0:h,column%2?h:0)
                &&oracle.ProjectIndependent(minus[j],column%2?0:-h,column%2?-h:0),
                "curved constraint oracle projects independent native displacements");
        } else {
            plus[j].position=Point3Ops::mkPoint3(plus[j].position,tangent*h);
            minus[j].position=Point3Ops::mkPoint3(minus[j].position,-tangent*h);
        }
        std::vector<Scalar> a,b;
        oracle.EvaluateConstraint(plus,start,end,a);oracle.EvaluateConstraint(minus,start,end,b);
        for(std::size_t row=0;row<2*chain.size();++row) {
            const std::size_t i=row/2,index=4*i+2*(row%2)+column%2;
            const Scalar expected=i==j?diagonal[index]:j==i+1?upper[index]:i==j+1?lower[4*(i-1)+2*(row%2)+column%2]:0;
            const Scalar observed=(a[row]-b[row])/(2*h);
            Check(std::fabs(observed-expected)<1e-5*std::max(Scalar(1),std::fabs(expected)),
                "every native shading/fallback Jacobian block agrees at an independent displacement scale");
        }
    }
    const Vector3 lightNormal=Vector3Ops::Normalize(Vector3Ops::mkVector3(end,chain.back().position));
    OrthonormalBasis3D lightFrame;lightFrame.CreateFromW(lightNormal);
    Scalar firstError=0;
    for(unsigned refinement=0;refinement<3;++refinement) {
        const Scalar step=h/std::pow(Scalar(2),refinement);
        Scalar observed[4]{};bool valid=true;
        for(unsigned column=0;column<2;++column) {
            const Vector3 tangent=column?lightFrame.v():lightFrame.u();
            const Point3 plusEnd=Point3Ops::mkPoint3(end,tangent*step),minusEnd=Point3Ops::mkPoint3(end,-tangent*step);
            auto plus=vertices,minus=vertices;
            const auto a=oracle.SolveDomain(start,Vector3(1,0,0),plusEnd,lightNormal,scene,stack,domain,plus,sampler,1e-7,1e-10);
            const auto b=oracle.SolveDomain(start,Vector3(1,0,0),minusEnd,lightNormal,scene,stack,domain,minus,sampler,1e-7,1e-10);
            valid=valid&&a.valid&&b.valid;
            if(a.valid&&b.valid) {
                const Vector3 delta=Vector3Ops::mkVector3(plus[0].geometry.position,minus[0].geometry.position)*(1/(2*step));
                observed[column]=Vector3Ops::Dot(delta,chain[0].dpdu);
                observed[2+column]=Vector3Ops::Dot(delta,chain[0].dpdv);
            }
        }
        Check(valid,"independent native endpoint perturbations retain the physical root");
        if(valid) {
            const Scalar measured=std::fabs(observed[0]*observed[3]-observed[1]*observed[2]);
            const Scalar predicted=oracle.ComputeLightToFirstVertexJacobianDet(chain,start,end,lightNormal);
            const Scalar error=std::fabs(measured-predicted);
            if(!refinement) firstError=error;
            if(refinement==2) {
                if(firstError>1e-5*std::max(Scalar(1),measured)) {
                    std::cout<<"native endpoint step convergence predicted="<<std::setprecision(17)<<predicted<<" observed="<<measured<<" first error="<<firstError<<" final error="<<error<<" step="<<step<<'\n';
                    Check(error<firstError/2,"independent endpoint differences converge under two step halvings");
                }
                Check(error<1e-5*std::max(Scalar(1),measured),
                    "native light-to-root Jacobian agrees with independently solved endpoint displacements");
            }
        }
    }

}
// Fresh Round 4 numerical witnesses use actual native proposals/contexts.
class SteepContinuousUV final : public AuditedIdentityFrameModifier {
public:
    void Modify(RayIntersectionGeometric& hit) const override {
        hit.ptCoord=Point2(.5+1e6*hit.ptObjIntersec.x,.5);
    }
};
class AliasedNormal final : public AuditedIdentityFrameModifier {
    Scalar periodDivisor;
public:
    explicit AliasedNormal(Scalar divisor=1):periodDivisor(divisor) {}
    bool SMSFrameDifferential(const RayIntersectionGeometric& raw,const TestSMSIntersectionDifferential& d,
        Vector3& n,Vector3& w) const override {
        if(raw.vNormal.z==0) return false;
        const Scalar period=std::cbrt(std::numeric_limits<Scalar>::epsilon())/periodDivisor;
        const Scalar sign=raw.vNormal.z<0?-1:1,phase=2*PI*raw.ptObjIntersec.x/period;
        const Vector3 value(sign*.2*period/(2*PI)*std::sin(phase),0,sign);
        n=FixtureNormalizedDifferential(value,Vector3(sign*.2*std::cos(phase)*d.objectPoint.x,0,0));
        w=n;return true;
    }
    void Modify(RayIntersectionGeometric& hit) const override {
        const Scalar h=std::cbrt(std::numeric_limits<Scalar>::epsilon())/periodDivisor;
        const Scalar a=.2*h/(2*PI);
        const Scalar sign=Vector3Ops::Dot(hit.vNormal,Vector3(0,0,1))<0?-1:1;
        hit.vNormal=Vector3Ops::Normalize(Vector3(sign*a*std::sin(2*PI*hit.ptObjIntersec.x/h),0,sign));
        hit.onb.CreateFromW(hit.vNormal);
    }
};
static unsigned preparationTessellations=0;
class PreparationIndexedMesh final : public TriangleMeshGeometryIndexed {
public:
    explicit PreparationIndexedMesh(bool face):TriangleMeshGeometryIndexed(true,face) {}
    bool WatertightForTest() const {return m_bWatertight;}
    bool TessellateToMesh(IndexTriangleListType& t,VerticesListType& p,NormalsListType& n,
        TexCoordsListType& uv,unsigned detail) const override {
        ++preparationTessellations;return TriangleMeshGeometryIndexed::TessellateToMesh(t,p,n,uv,detail);
    }
};
class PreparationPlainMesh final : public TriangleMeshGeometry {
public:
    PreparationPlainMesh():TriangleMeshGeometry(true) {}
    bool TessellateToMesh(IndexTriangleListType& t,VerticesListType& p,NormalsListType& n,
        TexCoordsListType& uv,unsigned detail) const override {
        ++preparationTessellations;return TriangleMeshGeometry::TessellateToMesh(t,p,n,uv,detail);
    }
};
static void NativeRebuildMesh(Fixture& f, bool indexed, Scalar translation=0, unsigned normalsMode=0, Scalar yScale=1, bool preparationProbe=false) {
    IndexTriangleListType indices;VerticesListType positions;NormalsListType normals;TexCoordsListType coords;
    Check(f.Object("caster")->GetGeometry()->TessellateToMesh(indices,positions,normals,coords,1),"R4 native mesh tessellation");
    for(auto& p:positions) {p.x+=translation;p.y*=yScale;}
    ITriangleMeshGeometryIndexed* mesh=nullptr;ITriangleMeshGeometry* plain=nullptr;
    if(preparationProbe) {
        if(indexed) mesh=new PreparationIndexedMesh(normalsMode==0);
        else plain=new PreparationPlainMesh;
    } else if(indexed) Check(RISE_API_CreateTriangleMeshGeometryIndexed(&mesh,true,normalsMode==0),"R4 double-precision indexed mesh");
    else Check(RISE_API_CreateTriangleMeshGeometry(&plain,true),"R4 non-indexed mesh");
    if(mesh) {mesh->BeginIndexedTriangles();mesh->AddVertices(positions);}
    if(plain) plain->BeginTriangles();
    for(const auto& index:indices) {
        Triangle t;
        for(unsigned k=0;k<3;++k) {t.vertices[k]=positions[index.iVertices[k]];t.coords[k]=coords[index.iCoords[k]];}
        const Vector3 face=Vector3Ops::Normalize(Vector3Ops::Cross(Vector3Ops::mkVector3(t.vertices[1],t.vertices[0]),Vector3Ops::mkVector3(t.vertices[2],t.vertices[0])));
        const Scalar cx=(t.vertices[0].x+t.vertices[1].x+t.vertices[2].x)/3;
        for(unsigned k=0;k<3;++k) t.normals[k]=normalsMode==2?-face:normalsMode==1
            ? Vector3Ops::Normalize(face*.5+Vector3((cx<0?-1:1)*std::sqrt(Scalar(3))*.5,0,0)):face;
        if(mesh) {
            IndexedTriangle out=index;
            for(unsigned k=0;k<3;++k) {out.iNormals[k]=mesh->numNormals();mesh->AddNormal(t.normals[k]);out.iCoords[k]=mesh->numCoords();mesh->AddTexCoord(t.coords[k]);}
            mesh->AddIndexedTriangle(out);
        }
        if(plain) plain->AddTriangle(t);
    }
    if(mesh) {mesh->DoneIndexedTriangles();f.job->GetObjects()->GetItem("caster")->AssignGeometry(*mesh);safe_release(mesh);}
    if(plain) {plain->DoneTriangles();f.job->GetObjects()->GetItem("caster")->AssignGeometry(*plain);safe_release(plain);}
    dynamic_cast<const ObjectManager*>(f.Scene().GetObjects())->InvalidateSpatialStructure();
    f.job->GetObjects()->PrepareForRendering();
}
static void NativeTriangleProvenance() {
    for(bool indexed:{false,true}) for(bool reverse:{false,true}) for(unsigned count:{1u,1024u}) {
        Fixture f(Materials()+QuadMesh("patch",0,-2,2,reverse)+SceneObject("caster","patch","mirror"));
        ITriangleMeshGeometryIndexed* mesh=nullptr;ITriangleMeshGeometry* plain=nullptr;
        if(indexed) Check(RISE_API_CreateTriangleMeshGeometryIndexed(&mesh,true,true),"primitive provenance indexed control");
        else Check(RISE_API_CreateTriangleMeshGeometry(&plain,true),"primitive provenance non-indexed control");
        if(mesh) mesh->BeginIndexedTriangles();if(plain) plain->BeginTriangles();
        for(unsigned primitive=0;primitive<count;++primitive) {
            Triangle t;
            const Scalar shift=primitive?1000+primitive*4:0;
            t.vertices[0]=Point3(shift-2,-2,0);t.vertices[1]=Point3(shift+2,-2,0);t.vertices[2]=Point3(shift+2,2,0);
            if(reverse) std::swap(t.vertices[1],t.vertices[2]);
            const Vector3 n(0,0,reverse?-1:1);
            for(unsigned k=0;k<3;++k) {t.normals[k]=n;t.coords[k]=Point2(k==0?0:1,k==2?1:0);}
            if(plain) plain->AddTriangle(t);
            if(mesh) {
                IndexedTriangle index;
                for(unsigned k=0;k<3;++k) {
                    index.iVertices[k]=mesh->numPoints();index.iNormals[k]=0;index.iCoords[k]=mesh->numCoords();
                    mesh->AddVertex(t.vertices[k]);mesh->AddTexCoord(t.coords[k]);
                }
                mesh->AddIndexedTriangle(index);
            }
        }
        if(mesh) {mesh->DoneIndexedTriangles();f.job->GetObjects()->GetItem("caster")->AssignGeometry(*mesh);mesh->release();}
        if(plain) {plain->DoneTriangles();f.job->GetObjects()->GetItem("caster")->AssignGeometry(*plain);plain->release();}
        RayIntersection hit(Ray(Point3(.2,.1,-2),Vector3(0,0,1)),nullRasterizerState);
        f.Object("caster")->IntersectRay(hit,RISE_INFINITY,true,true,false);
        Check(hit.geometric.bHit,"native primitive witness hits the visible triangle");
        const auto& signal=hit.geometric.signals;
        Check(signal.primId==0,"native hit retains the actual primitive for constant-time edge classification");
        const Scalar edge=std::min({signal.baryA,signal.baryB,1-signal.baryA-signal.baryB});
        Check(std::isfinite(edge)&&std::fabs(edge-.025)<1e-12,"native primitive barycentrics match independent planar edge oracle");
    }
}
static void NativePreparationAudits() {
    for(bool indexed:{false,true}) for(bool reverse:{false,true}) {
        Fixture f(Materials()+Mesh(true,reverse)+SceneObject("caster","shape","glass")
            +SceneObject("shared","shape","glass"," position 1000 0 0\n"));
        NativeRebuildMesh(f,indexed,0,2,1,true);
        auto* object=f.job->GetObjects()->GetItem("caster");
        f.job->GetObjects()->GetItem("shared")->AssignGeometry(*object->GetGeometry());
        const auto* cachedIndexed=dynamic_cast<const PreparationIndexedMesh*>(object->GetGeometry());
        const auto* cachedPlain=dynamic_cast<const PreparationPlainMesh*>(object->GetGeometry());
        const auto audits=cachedIndexed?OrientationAudits(cachedIndexed,0):OrientationAudits(cachedPlain,0);
        const auto visits=cachedIndexed?OrientationVisits(cachedIndexed,0):OrientationVisits(cachedPlain,0);
        preparationTessellations=0;
        for(unsigned frame=0;frame<4;++frame) f.job->GetObjects()->PrepareForRendering();
        Check(preparationTessellations==0,"repeated extended-off preparation audits shared meshes without tessellation or array copies");
        Check((cachedIndexed?OrientationAudits(cachedIndexed,0):OrientationAudits(cachedPlain,0))==audits,
            "shared repeated preparation performs no additional orientation audits");
        Check((cachedIndexed?OrientationVisits(cachedIndexed,0):OrientationVisits(cachedPlain,0))==visits,
            "shared repeated preparation visits no mesh triangles");
        IORStack inside(1);inside.SetCurrentObject(object);inside.push(1.3);SMSStartingMedia captured;
        Check(!SMSDomainReplay::Capture(f.Scene(),Point3(0,0,0),inside,captured),
            "cached opposing winding normals reject start-inside membership");
        if(indexed) {
            auto* native=dynamic_cast<PreparationIndexedMesh*>(const_cast<IGeometry*>(object->GetGeometry()));
            IndexTriangleListType t;VerticesListType positions;NormalsListType normals;TexCoordsListType uv;
            native->TriangleMeshGeometryIndexed::TessellateToMesh(t,positions,normals,uv,1);
            for(auto& n:normals) n=-n;
            native->UpdateVertices(positions,normals);
            Check(OrientationAudits(native,0)==audits+1 && OrientationVisits(native,0)>visits,
                "actual indexed mutation runs exactly one replacement audit");
            f.job->GetObjects()->PrepareForRendering();
            Check(SMSDomainReplay::Capture(f.Scene(),Point3(0,0,0),inside,captured),
                "real indexed normal mutation refreshes the cached orientation audit");
            Check(preparationTessellations==0,"normal mutation does not reintroduce preparation tessellation");
        }
    }
}
static void RoundFourMaterials() {
    const std::string coat="uniformcolor_painter\n{\n name black\n color 0 0 0\n}\npolished_material\n{\n name polish\n reflectance black\n tau 0.7\n ior triple\n scattering 1000000\n}\n";
    for(int side:{-1,1}) for(Scalar outer:{1.,1.2}) for(bool invalid:{false,true}) {
        Fixture f(Materials()+coat+PlaneScene("patch",0,-10,10)+SceneObject("caster","patch","polish"));
        if(invalid) {auto* modifier=new TiltNormal(side,false);f.job->GetObjects()->GetItem("caster")->AssignModifier(*modifier);modifier->release();}
        const Point3 start(-std::sqrt(Scalar(3)),0,side),center(0,0,0);
        RayIntersection hit(Ray(start,Vector3Ops::Normalize(Vector3Ops::mkVector3(center,start))),nullRasterizerState);
        f.Object("caster")->IntersectRay(hit,RISE_INFINITY,true,true,false);Check(hit.geometric.bHit,"R4 polished clipped-plane hit");
        if(!hit.geometric.bHit) continue;
        if(hit.pModifier) hit.pModifier->Modify(hit.geometric);
        for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),SMSQueryDomain::NM(450),SMSQueryDomain::NM(650)}) {
            IORStack air(outer);air.SetCurrentObject(hit.pObject);RandomNumberGenerator random(12);IndependentSampler sampler(random);ScatteredRayContainer rays;
            if(domain.kind==SMSQueryDomain::Wavelength) hit.pMaterial->GetSPF()->ScatterNM(hit.geometric,sampler,domain.nm,rays,air);
            else hit.pMaterial->GetSPF()->Scatter(hit.geometric,sampler,rays,air);
            Scalar native=0;unsigned count=0;
            for(unsigned k=0;k<rays.Count();++k) if(rays[k].isDelta&&rays[k].type==ScatteredRay::eRayReflection) {++count;native+=domain.kind==SMSQueryDomain::Wavelength?rays[k].krayNM:rays[k].kray[domain.component];}
            IORStack replay(air);Scalar ei=0,et=0,weight=0;bool exiting=false;
            const bool crossed=SMSDomainReplay::Cross(*hit.pMaterial,hit.pObject,hit.geometric,domain,true,replay,ei,et,exiting);
            const bool priced=crossed&&SMSDomainReplay::EventWeight(*hit.pMaterial,hit.geometric,air,domain,true,exiting,ei,et,1,weight);
            std::cout<<"R4 polished side="<<side<<" outer="<<outer<<" invalid="<<invalid<<" domain="<<domain.kind<<":"<<domain.component<<":"<<domain.nm<<" native rays="<<count<<" native="<<native<<" priced="<<priced<<" weight="<<weight<<'\n';
            if(count) Check(priced&&std::fabs(weight-native)<1e-8,"polished coat prices native ambient-to-coat reflection on either sheet face");
            else Check(!priced||weight==0,"native empty polished support cannot become a positive extended delta event");
        }
    }
    for(bool indexed:{false,true}) for(bool reverse:{false,true}) {
        Fixture f(Materials()+UVSeamMesh(reverse,true)+SceneObject("caster","sphere","mirror"));NativeRebuildMesh(f,indexed,0,1);
        ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=1;auto* solver=new ManifoldSolver(cfg);IORStack air(1);
        for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),SMSQueryDomain::NM(450),SMSQueryDomain::NM(650)}) {
            unsigned accepted=0;RandomNumberGenerator random(217);IndependentSampler sampler(random);
            for(unsigned k=0;k<64;++k) accepted+=solver->ProposeExtendedRoot(Point3(-.5,.3,-3),Vector3(0,0,1),Point3(.5,.3,-3),f.Scene(),air,domain,sampler).accepted;
            std::cout<<"R4 native normal seam indexed="<<indexed<<" winding="<<reverse<<" accepted="<<accepted<<'\n';
            Check(accepted==0,"discontinuous native corner-normal root has uncertain reciprocal identity");
        }
        solver->release();
        Fixture closed(Materials()+Mesh(true,reverse)+SceneObject("caster","shape","glass"));NativeRebuildMesh(closed,indexed,0,2);
        SMSStartingMedia capture;const bool captured=SMSDomainReplay::Capture(closed.Scene(),Point3(0,0,0),air,capture);
        std::cout<<"R4 opposing mesh start-inside indexed="<<indexed<<" winding="<<reverse<<" captured="<<captured<<" members="<<capture.enclosing.size()<<'\n';
        Check(!captured,"unaudited opposing corner normals cannot certify empty starting membership inside a closed mesh");
    }
}
static void NativeClosePatches(Fixture& f,bool reverse,Scalar offset) {
    ITriangleMeshGeometryIndexed* mesh=nullptr;
    Check(RISE_API_CreateTriangleMeshGeometryIndexed(&mesh,true,true),"R4 native resolvable close patches");
    if(!mesh) return;
    mesh->BeginIndexedTriangles();
    for(int sign:{-1,1}) {
        const Scalar centerX=offset+sign*4e-9;
        const Point3 center(centerX,.3,0),start(offset,-.2,-30000),end(offset,.8,-30000);
        const Vector3 n=Vector3Ops::Normalize(Vector3Ops::Normalize(Vector3Ops::mkVector3(start,center))
            +Vector3Ops::Normalize(Vector3Ops::mkVector3(end,center)));
        // Leave clearance for native context probes on either side of each
        // root; the two patches remain disjoint across their central gap.
        const Scalar x0=sign<0?offset-1:offset+.5e-9,x1=sign<0?offset-.5e-9:offset+1;
        const unsigned base=mesh->numPoints();
        for(const Point2& p:{Point2(x0,-1),Point2(x1,-1),Point2(x1,2),Point2(x0,2)}) {
            mesh->AddVertex(Point3(p.x,p.y,-n.x*(p.x-centerX)/n.z));
            mesh->AddNormal(n);mesh->AddTexCoord(Point2(.5,.5));
        }
        for(const std::array<unsigned,3>& corners:{std::array<unsigned,3>{0,1,2},std::array<unsigned,3>{0,2,3}}) {
            IndexedTriangle triangle;
            for(unsigned k=0;k<3;++k) triangle.iVertices[k]=triangle.iNormals[k]=triangle.iCoords[k]=base+corners[reverse&&k?k==1?2:1:k];
            mesh->AddIndexedTriangle(triangle);
        }
    }
    mesh->DoneIndexedTriangles();f.job->GetObjects()->GetItem("caster")->AssignGeometry(*mesh);safe_release(mesh);
    dynamic_cast<const ObjectManager*>(f.Scene().GetObjects())->InvalidateSpatialStructure();f.job->GetObjects()->PrepareForRendering();
}
class IrrelevantOscillatoryUV final : public AuditedIdentityFrameModifier {
public:
    void Modify(RayIntersectionGeometric& hit) const override {
        hit.ptCoord=Point2(.5+1e-4*std::sin(2*PI*hit.ptObjIntersec.x/std::ldexp(Scalar(1),-30)),.5);
    }
};
static void RoundFourNumerics(bool irrelevantUVOnly=false) {
    for(bool reverse:{false,true}) {
        Fixture f(Materials()+QuadMesh("patch",0,-2,2,reverse)+SceneObject("caster","patch","mirror"));
        IRayIntersectionModifier* modifier=irrelevantUVOnly ? static_cast<IRayIntersectionModifier*>(new IrrelevantOscillatoryUV) : static_cast<IRayIntersectionModifier*>(new SteepContinuousUV);f.job->GetObjects()->GetItem("caster")->AssignModifier(*modifier);modifier->release();
        const Point3 start(-.5,.3,-3),end(.5,.3,-3);IORStack air(1);
        ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=1;auto* solver=new ManifoldSolver(cfg);
        for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),SMSQueryDomain::NM(450),SMSQueryDomain::NM(650)}) {
            const auto proposal=[&](Scalar x) {
                const Scalar alpha=reverse?.075-x/4:.5+x/4;
                const Scalar beta=reverse?.5+x/4:.075-x/4;
                const Scalar a=1-alpha;
                ScriptSampler sampler({.2,1-a*a,beta/a,.75});
                return solver->ProposeExtendedRoot(start,Vector3(0,0,1),end,f.Scene(),air,domain,sampler);
            };
            const auto a=proposal(-1e-10),b=proposal(1e-10);
            const bool same=ManifoldSolver::SameExtendedRoot(a,b,1);
            std::cout<<std::setprecision(17)<<"R4 steep UV winding="<<reverse<<" domain="<<domain.kind<<":"<<domain.component<<":"<<domain.nm
                <<" accepted="<<a.accepted<<','<<b.accepted<<" solved="<<a.result.valid<<','<<b.result.valid<<" same="<<same;
            if(a.accepted&&b.accepted) std::cout<<" x="<<a.vertices[0].geometry.position.x<<','<<b.vertices[0].geometry.position.x
                <<" uv="<<a.vertices[0].geometry.uv.x<<','<<b.vertices[0].geometry.uv.x;
            std::cout<<'\n';
            Check(!(a.accepted&&b.accepted)||same,"one smooth steep-chart root cannot create two accepted reciprocal families");
            if(a.accepted&&b.accepted) {
                std::vector<double> samples;
                for(unsigned salt=0;salt<4;++salt) {
                    RandomNumberGenerator dr(SobolSequence::HashCombine(9031+salt,0x523441)),rr(SobolSequence::HashCombine(9127+salt,0x52344b));
                    IndependentSampler discovery(dr),retry(rr);Scalar sum=0;
                    const auto propose=[&](ISampler& sampler){return sampler.Get1D()<.5?a:b;};
                    const unsigned N=4096;
                    for(unsigned n=0;n<N;++n) {
                        const auto root=propose(discovery);
                        const Scalar k=SMSRootReference::Reciprocal(root,retry,propose,
                            [](const SMSDomainRoot& x,const SMSDomainRoot& y){return ManifoldSolver::SameExtendedRoot(x,y,1);},64,true);
                        sum+=SMSRootReference::Deposit(root.result.contributionNM,k,1,1,N);
                    }
                    samples.push_back(sum);
                }
                const Moments m(samples);
                std::cout<<"R4 steep UV two-seed reciprocal mean="<<m.mean<<" sd="<<m.sd<<" n=4 N=4096 analytic=1 reference sd=0\n";
                Check(std::fabs(m.mean-1)<=3*m.sd,"native two-seed same-root accounting contributes one physical event price");
            }
        }
        solver->release();
    }
    if(irrelevantUVOnly) return;
    for(bool reverse:{false,true}) for(Scalar offset:{Scalar(1000000),Scalar(5000000)}) {
        Fixture f(Materials()+CloseRootMesh(reverse,10000,1e-9)+SceneObject("caster","shape","mirror"));NativeClosePatches(f,reverse,offset);
        ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=1;cfg.solverThreshold=1e-10;
        auto* solver=new ManifoldSolver(cfg);IORStack air(1);
        for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),SMSQueryDomain::NM(450),SMSQueryDomain::NM(650)}) {
            ScriptSampler left({.2,reverse?5./9:11./36,reverse?.25:.4,.1}),right({.2,reverse?5./9:11./36,reverse?.25:.4,.6});
            const Point3 start(offset,-.2,-30000),end(offset,.8,-30000);
            ScriptSampler seedSampler({.2,reverse?5./9:11./36,reverse?.25:.4,.1});
            std::vector<SMSDomainVertex> seed;
            const bool walked=solver->BuildExtendedSeed(start,end,f.Scene(),air,domain,seedSampler,seed);
            std::cout<<"R4 close walk="<<walked<<" vertices="<<seed.size();
            if(!seed.empty()) std::cout<<" position="<<seed[0].geometry.position.x<<":"<<seed[0].geometry.position.y<<":"<<seed[0].geometry.position.z
                <<" native derivative="<<seed[0].context.derivatives.valid<<" bary="<<seed[0].context.signals.baryA<<":"<<seed[0].context.signals.baryB;
            std::cout<<'\n';
            const auto a=solver->ProposeExtendedRoot(start,Vector3(0,0,1),end,f.Scene(),air,domain,left);
            const auto b=solver->ProposeExtendedRoot(start,Vector3(0,0,1),end,f.Scene(),air,domain,right);
            const bool same=ManifoldSolver::SameExtendedRoot(a,b,1);
            std::cout<<"R4 translated close offset="<<offset<<" winding="<<reverse<<" domain="<<domain.kind<<":"<<domain.component<<":"<<domain.nm
                <<" accepted="<<a.accepted<<','<<b.accepted<<" solved="<<a.result.valid<<','<<b.result.valid<<" same="<<same;
            if(a.accepted&&b.accepted) std::cout<<" separation="<<Point3Ops::Distance(a.vertices[0].geometry.position,b.vertices[0].geometry.position);
            std::cout<<'\n';
            Check(a.accepted&&b.accepted,"both natively reachable disjoint close roots retain positive acceptance");
            Check(!(a.accepted&&b.accepted)||!same,"translated distinct regular patches cannot share an accepted reciprocal family");
            if(a.accepted&&b.accepted) {
                std::vector<double> samples;
                for(unsigned salt=0;salt<4;++salt) {
                    RandomNumberGenerator dr(SobolSequence::HashCombine(10031+salt,0x523441)),rr(SobolSequence::HashCombine(10127+salt,0x52344b));
                    IndependentSampler discovery(dr),retry(rr);Scalar sum=0;
                    const auto propose=[&](ISampler& sampler){return sampler.Get1D()<.5?a:b;};
                    const unsigned N=4096;
                    for(unsigned n=0;n<N;++n) {
                        const auto root=propose(discovery);
                        const Scalar k=SMSRootReference::Reciprocal(root,retry,propose,
                            [](const SMSDomainRoot& x,const SMSDomainRoot& y){return ManifoldSolver::SameExtendedRoot(x,y,1);},64,true);
                        sum+=SMSRootReference::Deposit(root.result.contributionNM,k,1,1,N);
                    }
                    samples.push_back(sum);
                }
                const Moments m(samples);
                std::cout<<"R4 close-root two-seed reciprocal mean="<<m.mean<<" sd="<<m.sd<<" n=4 N=4096 analytic=2 reference sd=0\n";
                Check(std::fabs(m.mean-2)<=3*m.sd,"native two-root accounting retains both physical event prices");
            }
        }
        solver->release();
    }
    for(bool reverse:{false,true}) for(Scalar divisor:{Scalar(1),Scalar(4)}) {
        Fixture f(Materials()+QuadMesh("patch",0,-2,2,reverse)+SceneObject("caster","patch","mirror"));
        auto* modifier=new AliasedNormal(divisor);f.job->GetObjects()->GetItem("caster")->AssignModifier(*modifier);modifier->release();
        const Point3 start(-.5,.3,3),end(.5,.3,3),center(0,.3,0);IORStack air(1);
        RayIntersection hit(Ray(start,Vector3Ops::Normalize(Vector3Ops::mkVector3(center,start))),nullRasterizerState);
        f.Object("caster")->IntersectRay(hit,RISE_INFINITY,true,true,false);Check(hit.geometric.bHit,"R4 continuous normal native hit");
        if(!hit.geometric.bHit) continue;
        hit.pModifier->Modify(hit.geometric);
        SMSDomainVertex vertex(hit.geometric);
        // The scene parser stores mesh coordinates at native precision. Seed
        // the actual traced surface, rather than an ideal decimal z=.01.
        const auto* native=dynamic_cast<const RISE::Implementation::Object*>(hit.pObject);
        Check(native!=nullptr,"stacked patch uses a native object surface convention");
        if(!native) continue;
        const Vector3 localDirection=Vector3Ops::Normalize(Vector3Ops::Transform(
            hit.pObject->GetFinalInverseTransformMatrix(),hit.geometric.ray.Dir()));
        vertex.geometry.position=Point3Ops::Transform(hit.pObject->GetFinalTransformMatrix(),
            Point3Ops::mkPoint3(hit.geometric.ptObjIntersec,localDirection*native->GetSurfaceIntersecError()));
        vertex.geometry.normal=hit.geometric.vNormal;
        vertex.geometry.geomNormal=hit.geometric.UnflippedGeomNormal();vertex.geometry.pObject=hit.pObject;
        vertex.geometry.pMaterial=hit.pMaterial;vertex.geometry.isReflection=true;
        std::vector<SMSDomainVertex> records{vertex};ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=1;
        for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),SMSQueryDomain::NM(450),SMSQueryDomain::NM(650)}) {
        auto domainRecords=records;
        NativeConstraintOracle oracle(cfg,&domainRecords);RandomNumberGenerator random(21);IndependentSampler sampler(random);
        const auto result=oracle.SolveDomain(start,Vector3(0,0,-1),end,Vector3(0,0,-1),f.Scene(),air,domain,domainRecords,sampler,1e-7,1e-10);
        std::cout<<"R4 oscillatory normal divisor="<<divisor<<" domain="<<domain.kind<<":"<<domain.component<<":"<<domain.nm<<" winding="<<reverse<<" valid="<<result.valid<<'\n';
        Check(result.valid,"continuous normal positive control solves");
        if(result.valid) {
            std::vector<Scalar> d,u,l;oracle.BuildJacobian(result.specularChain,start,end,d,u,l,true);
            const Scalar h=std::cbrt(std::numeric_limits<Scalar>::epsilon())/(1024*divisor);
            for(unsigned column=0;column<2;++column) {
                auto plus=result.specularChain,minus=plus;const Vector3 t=column?plus[0].dpdv:plus[0].dpdu;
                plus[0].position=Point3Ops::mkPoint3(center,t*h);minus[0].position=Point3Ops::mkPoint3(center,-t*h);
                std::vector<Scalar>a,b;oracle.EvaluateConstraint(plus,start,end,a);oracle.EvaluateConstraint(minus,start,end,b);
                for(unsigned row=0;row<2;++row) {
                    const Scalar actual=(a[row]-b[row])/(2*h),value=d[2*row+column];
                    std::cout<<"R4 normal derivative column="<<column<<" row="<<row<<" coarse="<<value<<" refined="<<actual<<'\n';
                    Check(std::isfinite(value)&&std::fabs(value-actual)<1e-5,"accepted native normal Jacobian is finite and resolves the continuous modifier at refined scale");
                }
            }
        }
        }
    }
}
class HarmonicGeneratedUV : public IUVGenerator, public Reference {
public:
    void GenerateUV(const Point3& p,const Vector3&,Point2& uv) const override {
        const Scalar period=std::cbrt(std::numeric_limits<Scalar>::epsilon())/(4*114243);
        uv=Point2(.2*period/(2*PI)*std::sin(2*PI*p.x/period),0);
    }
};
class AuditedHarmonicGeneratedUV final : public HarmonicGeneratedUV, public TestSMSUVDifferential {
public:
    bool HasSMSUVDifferentialContract() const override {return true;}
    bool SMSUVDifferential(const Point3& p,const Vector3&,const Vector3& dp,const Vector3&,Point2& d) const override {
        const Scalar period=std::cbrt(std::numeric_limits<Scalar>::epsilon())/(4*114243);
        d=Point2(.2*std::cos(2*PI*p.x/period)*dp.x,0);return true;
    }
};
class AnalyticUVNormal final : public AuditedIdentityFrameModifier {
public:
    bool SMSFrameDependsOnUV() const override {return true;}
    void Modify(RayIntersectionGeometric& hit) const override {
        hit.vNormal=Vector3Ops::Normalize(hit.vNormal+Vector3(hit.ptCoord.x,0,0));
        hit.onb.CreateFromW(hit.vNormal);
    }
    bool SMSFrameDifferential(const RayIntersectionGeometric& raw,const TestSMSIntersectionDifferential& d,
        Vector3& n,Vector3& w) const override {
        n=FixtureNormalizedDifferential(raw.vNormal+Vector3(raw.ptCoord.x,0,0),d.normal+Vector3(d.uv.x,0,0));
        w=n;return true;
    }
};
class AuditedLinearNormalUV final : public IUVGenerator, public Reference, public TestSMSUVDifferential {
    const bool omitNormalDerivative;
public:
    mutable Scalar maximumNormalTerm=0;
    explicit AuditedLinearNormalUV(bool omit=false) : omitNormalDerivative(omit) {}
    bool HasSMSUVDifferentialContract() const override {return true;}
    void GenerateUV(const Point3& p,const Vector3& n,Point2& uv) const override {uv=Point2(.2*p.x*n.z,0);}
    bool SMSUVDifferential(const Point3& p,const Vector3& n,const Vector3& dp,const Vector3& dn,Point2& duv) const override {
        maximumNormalTerm=std::max(maximumNormalTerm,std::fabs(p.x*dn.z));
        duv=Point2(.2*(dp.x*n.z+(omitNormalDerivative?0:p.x*dn.z)),0);return true;
    }
};
static void NativeTransformedUVComposition() {
    for(bool reverse:{false,true}) for(int side:{-1,1}) {
        Fixture f(Materials()+QuadMesh("patch",0,-2,3,reverse)
            +SceneObject("caster","patch","mirror"," scale 2 0.8 1.5\n orientation 0 45 0\n"));
        auto* modifier=new AnalyticUVNormal;f.job->GetObjects()->GetItem("caster")->AssignModifier(*modifier);modifier->release();
        auto* mapping=new AuditedLinearNormalUV;f.job->GetObjects()->GetItem("caster")->SetUVGenerator(*mapping);mapping->release();
        const auto& transform=f.Object("caster")->GetFinalTransformMatrix();
        const Point3 start=Point3Ops::Transform(transform,Point3(0,0,side*3));
        const Point3 end=Point3Ops::Transform(transform,Point3(0,0,side*4));
        const Point3 center=Point3Ops::Transform(transform,Point3(0,0,0));
        const Vector3 incoming=Vector3Ops::Normalize(Vector3Ops::mkVector3(center,start));
        RayIntersection hit(Ray(start,incoming),nullRasterizerState);f.Object("caster")->IntersectRay(hit,RISE_INFINITY,true,true,false);
        Check(hit.geometric.bHit,"transformed composed-UV native surface is reachable");
        if(!hit.geometric.bHit) continue;
        hit.pModifier->Modify(hit.geometric);SMSDomainVertex vertex(hit.geometric);
        vertex.geometry.position=center;vertex.geometry.normal=hit.geometric.vNormal;
        vertex.geometry.geomNormal=hit.geometric.UnflippedGeomNormal();vertex.geometry.pObject=hit.pObject;
        vertex.geometry.pMaterial=hit.pMaterial;vertex.geometry.isReflection=true;
        ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=1;IORStack air(1);
        for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),SMSQueryDomain::NM(450),SMSQueryDomain::NM(650)}) {
            std::vector<SMSDomainVertex> records{vertex};NativeConstraintOracle oracle(cfg,&records);
            RandomNumberGenerator random(77);IndependentSampler sampler(random);
            const auto result=oracle.SolveDomain(start,incoming,end,incoming,f.Scene(),air,domain,records,sampler,1e-7,1e-10);
            Check(result.valid,"audited object-normal-dependent UV composition retains transformed positive roots");
            if(result.valid) CheckNativeHorizonJacobian(cfg,result,records,start,end,f.Scene(),air,domain,sampler);
        }
    }
}
static void NativeCurvedUVComposition() {
    for(bool transformed:{false,true}) for(int side:{-1,1}) for(bool fault:{false,true}) {
        Fixture f(Materials()+"sphere_geometry\n{\n name curved\n radius 1\n}\n"
            +SceneObject("caster","curved","mirror",transformed?" scale 2 0.8 1.5\n orientation 0 45 0\n":""));
        auto* modifier=new AnalyticUVNormal;f.job->GetObjects()->GetItem("caster")->AssignModifier(*modifier);modifier->release();
        auto* mapping=new AuditedLinearNormalUV(fault);f.job->GetObjects()->GetItem("caster")->SetUVGenerator(*mapping);
        const Point3 local(.6,.2,std::sqrt(Scalar(.6)));
        const auto& transform=f.Object("caster")->GetFinalTransformMatrix();
        const Point3 center=Point3Ops::Transform(transform,local);
        const Vector3 outward=Vector3Ops::Normalize(Vector3Ops::Transform(Matrix4Ops::Transpose(f.Object("caster")->GetFinalInverseTransformMatrix()),Vector3(local.x,local.y,local.z)));
        const Point3 start=Point3Ops::mkPoint3(center,outward*(side>0?3:-.3));
        RayIntersection hit(Ray(start,Vector3Ops::Normalize(Vector3Ops::mkVector3(center,start))),nullRasterizerState);
        f.Scene().GetObjects()->IntersectRay(hit,true,true,false);
        Check(hit.geometric.bHit&&hit.pObject==f.Object("caster"),"curved normal-dependent UV oracle reaches its native surface");
        if(!hit.geometric.bHit) {mapping->release();continue;}
        hit.pModifier->Modify(hit.geometric);IORStack air(1);
        RandomNumberGenerator rng(81);IndependentSampler sampler(rng);ScatteredRayContainer rays;
        hit.pMaterial->GetSPF()->Scatter(hit.geometric,sampler,rays,air);
        Check(rays.Count()>0,"curved UV fixture has an actual native reflection");
        if(!rays.Count()) {mapping->release();continue;}
        const Point3 end=Point3Ops::mkPoint3(center,rays[0].ray.Dir()*(side>0?3:.2));
        ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=1;
        for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),SMSQueryDomain::NM(450),SMSQueryDomain::NM(650)}) {
            mapping->maximumNormalTerm=0;
            SMSDomainVertex vertex(hit.geometric);vertex.geometry.position=center;vertex.geometry.normal=hit.geometric.vNormal;
            vertex.geometry.geomNormal=hit.geometric.UnflippedGeomNormal();vertex.geometry.pObject=hit.pObject;
            vertex.geometry.pMaterial=hit.pMaterial;vertex.geometry.isReflection=true;
            std::vector<SMSDomainVertex> records{vertex};NativeConstraintOracle oracle(cfg,&records);
            const auto result=oracle.SolveDomain(start,hit.geometric.vNormal,end,-rays[0].ray.Dir(),f.Scene(),air,domain,records,sampler,1e-7,1e-10);
            Check(result.valid,"off-axis curved UV composition has a regular solved root");
            if(!result.valid) continue;
            std::vector<Scalar> d,u,l;oracle.BuildJacobian(result.specularChain,start,end,d,u,l,true);
            Scalar error=0;const Scalar h=1e-6;
            for(unsigned column=0;column<2;++column) {
                auto plus=result.specularChain,minus=plus;const Vector3 tangent=column?plus[0].dpdv:plus[0].dpdu;
                Check(oracle.ProjectIndependent(plus[0],column?0:h,column?h:0)
                    &&oracle.ProjectIndependent(minus[0],column?0:-h,column?-h:0),"curved oracle independently projects shifted native points");
                std::vector<Scalar> a,b;oracle.EvaluateConstraint(plus,start,end,a);oracle.EvaluateConstraint(minus,start,end,b);
                for(unsigned row=0;row<2;++row) error=std::max(error,std::fabs(d[2*row+column]-(a[row]-b[row])/(2*h)));
            }
            Check(mapping->maximumNormalTerm>1e-3,"curved UV differential consumes a measurably nonzero p.x*dn.z input");
            std::cout<<"R7 curved UV transformed="<<transformed<<" side="<<side<<" fault="<<fault
                <<" domain="<<domain.kind<<":"<<domain.component<<":"<<domain.nm
                <<" normal_term="<<mapping->maximumNormalTerm<<" jacobian_error="<<error<<'\n';
            Check(fault?error>1e-4:error<1e-5,"independent curved Jacobian detects an omitted normal derivative and validates full transport");
            if(!fault) CheckNativeHorizonJacobian(cfg,result,records,start,end,f.Scene(),air,domain,sampler,true);
        }
        mapping->release();
    }
}
static void NativeNearCommensurateNormal(unsigned generatedUV=0) {
    const Scalar divisor=4*114243;
    for(bool reverse:{false,true}) {
        Fixture f(Materials()+QuadMesh("patch",0,-2,3,reverse)+SceneObject("caster","patch","mirror"));
        IRayIntersectionModifier* modifier=generatedUV && generatedUV!=3 ? static_cast<IRayIntersectionModifier*>(new AnalyticUVNormal)
            : static_cast<IRayIntersectionModifier*>(new AliasedNormal(divisor));
        f.job->GetObjects()->GetItem("caster")->AssignModifier(*modifier);modifier->release();
        if(generatedUV) {
            IUVGenerator* mapping=generatedUV==2 ? static_cast<IUVGenerator*>(new AuditedHarmonicGeneratedUV)
                : static_cast<IUVGenerator*>(new HarmonicGeneratedUV);
            f.job->GetObjects()->GetItem("caster")->SetUVGenerator(*mapping);mapping->release();
        }
        const Point3 start(0,0,3),end(0,0,4),center(0,0,0);IORStack air(1);
        RayIntersection hit(Ray(start,Vector3(0,0,-1)),nullRasterizerState);
        f.Object("caster")->IntersectRay(hit,RISE_INFINITY,true,true,false);
        Check(hit.geometric.bHit,"near-commensurate normal native surface is reachable");
        if(!hit.geometric.bHit) continue;
        hit.pModifier->Modify(hit.geometric);
        SMSDomainVertex vertex(hit.geometric);vertex.geometry.position=center;
        vertex.geometry.normal=hit.geometric.vNormal;vertex.geometry.geomNormal=hit.geometric.UnflippedGeomNormal();
        vertex.geometry.pObject=hit.pObject;vertex.geometry.pMaterial=hit.pMaterial;vertex.geometry.isReflection=true;
        ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=1;
        for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),SMSQueryDomain::NM(450),SMSQueryDomain::NM(650)}) {
            std::vector<SMSDomainVertex> records{vertex};NativeConstraintOracle oracle(cfg,&records);
            RandomNumberGenerator random(32);IndependentSampler sampler(random);
            const auto result=oracle.SolveDomain(start,Vector3(0,0,-1),end,Vector3(0,0,-1),f.Scene(),air,domain,records,sampler,1e-7,1e-10);
            if(generatedUV) Check(result.valid==(generatedUV!=1),
                "uncertified UV dependency rejects while certified composition and UV-independent modifiers retain positive roots");
            std::cout<<"R5 near-commensurate generated_uv="<<generatedUV<<" domain="<<domain.kind<<":"<<domain.component<<":"<<domain.nm<<" winding="<<reverse<<" solved="<<result.valid<<'\n';
            if(result.valid) {
                std::vector<Scalar> d,u,l;oracle.BuildJacobian(result.specularChain,start,end,d,u,l,true);
                const Scalar h=std::cbrt(std::numeric_limits<Scalar>::epsilon())/(1024*divisor);
                for(unsigned column=0;column<2;++column) {
                    auto plus=result.specularChain,minus=plus;const Vector3 tangent=column?plus[0].dpdv:plus[0].dpdu;
                    plus[0].position=Point3Ops::mkPoint3(center,tangent*h);minus[0].position=Point3Ops::mkPoint3(center,-tangent*h);
                    std::vector<Scalar>a,b;oracle.EvaluateConstraint(plus,start,end,a);oracle.EvaluateConstraint(minus,start,end,b);
                    for(unsigned row=0;row<2;++row) {
                        const Scalar actual=(a[row]-b[row])/(2*h),value=d[2*row+column];
                        std::cout<<"R5 near-commensurate derivative="<<value<<" reference="<<actual<<'\n';
                        Check(std::isfinite(value)&&std::fabs(value-actual)<1e-5,"accepted near-commensurate root has an accurate finite native Jacobian");
                    }
                }
            } else {
                ScriptSampler seed({.2,.25,reverse?0.:1.,.1});
                const auto root=oracle.ProposeExtendedRoot(start,Vector3(0,0,-1),end,f.Scene(),air,domain,seed);
                Check(!root.accepted,"unresolved native derivative is a production zero proposal");
            }
        }
    }
}
static void NativeWeldCoordinateRange() {
    for(bool closed:{false,true}) for(bool reverse:{false,true}) for(Scalar translation:{Scalar(0),Scalar(1e14),Scalar(-1e14)}) {
        Fixture f(Materials()+Mesh(closed,reverse)+SceneObject("caster","shape","glass"));
        IndexTriangleListType indices;VerticesListType points;NormalsListType normals;TexCoordsListType uv;
        Check(f.Object("caster")->GetGeometry()->TessellateToMesh(indices,points,normals,uv,1),"weld-range native mesh tessellates");
        auto* mesh=new PreparationIndexedMesh(true);mesh->BeginIndexedTriangles();
        for(const auto& t:indices) {
            IndexedTriangle out;
            const Vector3 face=Vector3Ops::Normalize(Vector3Ops::Cross(
                Vector3Ops::mkVector3(points[t.iVertices[1]],points[t.iVertices[0]]),
                Vector3Ops::mkVector3(points[t.iVertices[2]],points[t.iVertices[0]])));
            for(unsigned k=0;k<3;++k) {
                Point3 p=points[t.iVertices[k]];p.x+=translation;
                out.iVertices[k]=mesh->numPoints();mesh->AddVertex(p);
                out.iNormals[k]=mesh->numNormals();mesh->AddNormal(face);
                out.iCoords[k]=mesh->numCoords();mesh->AddTexCoord(uv[t.iCoords[k]]);
            }
            mesh->AddIndexedTriangle(out);
        }
        mesh->DoneIndexedTriangles();
        Check(mesh->WatertightForTest()==closed,"position welding certifies the same closed/open mesh at huge positive and negative translations");
        std::cout<<"R6 weld range closed="<<closed<<" winding="<<reverse<<" translation="<<translation
            <<" watertight="<<mesh->WatertightForTest()<<'\n';
        mesh->release();
    }
}
class AuditedSignalContextFrame final : public AuditedIdentityFrameModifier {
public:
    mutable unsigned inspected=0;
    mutable bool missing=false;
    bool Inspect(const RayIntersectionGeometric& hit) const {
        ++inspected;
        const bool valid=hit.signals.pScene&&hit.signals.pSelf
            &&Point3Ops::Distance(hit.signals.ptWorld,hit.ptIntersection)==0;
        missing=missing||!valid;return valid;
    }
    void Modify(RayIntersectionGeometric& hit) const override {Inspect(hit);}
    bool SMSFrameDifferential(const RayIntersectionGeometric& raw,const TestSMSIntersectionDifferential& d,
        Vector3& n,Vector3& w) const override {
        n=d.normal;w=d.frameW;return Inspect(raw);
    }
};
// The reference price comes from native SPF scattering at an independent
// scene-manager hit, including the consumer's separate radiance eta factor.
static void NativeCrossObjectPrices(bool auditModifier=false) {
    for(bool interior:{false,true}) for(bool reflection:{false,true})
        for(bool reverse:{false,true}) for(bool transformed:{false,true}) {
        std::string text=Materials();
        text.replace(text.find("values 1.3 1.5 1.9"),std::string("values 1.3 1.5 1.9").size(),"values 1.5 1.5 1.5");
        text+="expression_painter\n{\n name signal\n expr vec3(.2+.8*"+std::string(interior?"interior(1)":"proximity(4)")+",.2+.8*"+(interior?"interior(1)":"proximity(4)")+",.2+.8*"+(interior?"interior(1)":"proximity(4)")+")\n}\n"
            "perfectreflector_material\n{\n name signal_mirror\n reflectance signal\n}\n"
            "perfectrefractor_material\n{\n name signal_glass\n refractance signal\n ior triple\n}\n"
            "lambertian_material\n{\n name neighbour_mat\n reflectance white\n}\n"
            "sphere_geometry\n{\n name neighbour_geo\n radius "+std::string(interior?"20":".1")+"\n}\n"
            +QuadMesh("patch",0,-2,3,reverse)+SceneObject("caster","patch",reflection?"signal_mirror":"signal_glass",
                transformed?" scale 2 0.8 1.5\n orientation 0 45 0\n":"");
        Fixture f(text);
        const auto& transform=f.Object("caster")->GetFinalTransformMatrix();
        const Point3 center=Point3Ops::Transform(transform,Point3(0,.3,0));
        const Point3 start=Point3Ops::Transform(transform,Point3(-.5,.3,-3));
        // Add the neighbour through the scene parser before testing any price.
        const Point3 neighbour=interior?center:Point3Ops::Transform(transform,Point3(1,.3,0));
        std::ostringstream location;location<<std::setprecision(17)<<" position "<<neighbour.x<<' '<<neighbour.y<<' '<<neighbour.z<<"\n";
        Fixture scene(text+SceneObject("neighbour","neighbour_geo","neighbour_mat",location.str()));
        auto* modifier=auditModifier?new AuditedSignalContextFrame:nullptr;
        if(modifier) scene.job->GetObjects()->GetItem("caster")->AssignModifier(*modifier);
        IORStack air(1);ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=1;
        for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),SMSQueryDomain::NM(450),SMSQueryDomain::NM(650)}) {
            RayIntersection hit(Ray(start,Vector3Ops::Normalize(Vector3Ops::mkVector3(center,start))),nullRasterizerState);
            scene.Scene().GetObjects()->IntersectRay(hit,true,true,false);
            Check(hit.geometric.bHit&&hit.pObject==scene.Object("caster"),"signal-price independent scene hit reaches the caster");
            if(!hit.geometric.bHit||hit.pObject!=scene.Object("caster")) continue;
            Check(hit.geometric.signals.pScene==scene.Scene().GetObjects()&&hit.geometric.signals.pSelf==hit.pObject,
                "signal-price native oracle has scene and self provenance");
            if(hit.pModifier) hit.pModifier->Modify(hit.geometric);
            air.SetCurrentObject(hit.pObject);
            RandomNumberGenerator random(71);IndependentSampler sampler(random);ScatteredRayContainer rays;
            if(domain.kind==SMSQueryDomain::Wavelength) hit.pMaterial->GetSPF()->ScatterNM(hit.geometric,sampler,domain.nm,rays,air);
            else hit.pMaterial->GetSPF()->Scatter(hit.geometric,sampler,rays,air);
            bool tested=false;
            for(unsigned j=0;j<rays.Count();++j) {
                const auto& ray=rays[j];
                if(!ray.isDelta || (ray.type==ScatteredRay::eRayReflection)!=reflection) continue;
                const Scalar native=domain.kind==SMSQueryDomain::Wavelength?ray.krayNM:ray.kray[domain.component];
                if(native==0) continue;
                tested=true;
                const Point3 seedPoint=Point3Ops::Transform(transform,Point3(.2,.3,0));
                RayIntersection seed(Ray(start,Vector3Ops::Normalize(Vector3Ops::mkVector3(seedPoint,start))),nullRasterizerState);
                scene.Scene().GetObjects()->IntersectRay(seed,true,true,false);
                Check(seed.geometric.bHit&&seed.pObject==hit.pObject,"signal refresh starts from a different real native seed point");
                if(!seed.geometric.bHit||seed.pObject!=hit.pObject) continue;
                if(seed.pModifier) seed.pModifier->Modify(seed.geometric);
                SMSDomainVertex vertex(seed.geometric);
                vertex.geometry.position=seedPoint;vertex.geometry.normal=seed.geometric.vNormal;
                vertex.geometry.geomNormal=seed.geometric.UnflippedGeomNormal();vertex.geometry.pObject=hit.pObject;
                vertex.geometry.pMaterial=hit.pMaterial;vertex.geometry.isReflection=reflection;
                std::vector<SMSDomainVertex> records{vertex};NativeConstraintOracle oracle(cfg,&records);
                const Point3 end=Point3Ops::mkPoint3(center,ray.ray.Dir()*3);
                const auto result=oracle.SolveDomain(start,hit.geometric.vNormal,end,-ray.ray.Dir(),scene.Scene(),air,domain,records,sampler,1e-7,1e-10);
                Check(result.valid,"cross-object painter retains a regular native R/T root");
                const Scalar expected=native*(reflection?1:RadianceEtaScale(air,ray.ior_stack));
                std::cout<<"R7 signal modifier="<<auditModifier<<" interior="<<interior<<" reflection="<<reflection<<" winding="<<reverse<<" transformed="<<transformed
                    <<" domain="<<domain.kind<<":"<<domain.component<<":"<<domain.nm<<" root_price="<<result.contributionNM<<" native_price="<<expected<<'\n';
                if(result.valid) {
                    Check(Point3Ops::Distance(records[0].context.ptIntersection,seed.geometric.ptIntersection)>.1,
                        "signal root refresh replaces a genuinely different seed context");
                    Check(records[0].context.signals.pScene==scene.Scene().GetObjects()&&records[0].context.signals.pSelf==hit.pObject,
                        "refreshed root retains scene/self signal provenance");
                    Check(Point3Ops::Distance(records[0].context.signals.ptWorld,records[0].context.ptIntersection)==0,
                        "refreshed signal world point belongs to the final native context");
                    Check(std::fabs(result.contributionNM-expected)<1e-7,"final RGB/NM cross-object attenuation agrees with scene-stamped native SPF and eta price");
                }
            }
            Check(tested,"native signal-price event is present");
            if(modifier) {
                Check(modifier->inspected>1,"native modifier/differential probes inspect actual scene contexts");
                Check(!modifier->missing,"all direct-object modifier/differential inputs retain current scene signals");
            }
        }
        safe_release(modifier);
    }
}
class InertFrameModifier final : public AuditedIdentityFrameModifier {
public:
    void Modify(RayIntersectionGeometric&) const override {}
};
static void NativeStackedPatchFrames() {
    for(bool reverse:{false,true}) for(bool indexed:{false,true}) for(int side:{-1,1}) {
        std::ostringstream mesh;
        mesh<<"indexedmesh_geometry\n{\n name patch\n double_sided TRUE\n face_normals TRUE\n";
        for(Scalar z:{Scalar(0),Scalar(.01)}) for(const Point2& xy:{Point2(-2,-2),Point2(2,-2),Point2(2,2),Point2(-2,2)})
            mesh<<" vertex "<<xy.x<<' '<<xy.y<<' '<<z<<"\n";
        mesh<<" uv 0 0\n uv 1 0\n uv 1 1\n uv 0 1\n uv 0 0\n uv 1 0\n uv 1 1\n uv 0 1\n";
        mesh<<(reverse?" triangle 0 2 1\n triangle 0 3 2\n triangle 4 6 5\n triangle 4 7 6\n":" triangle 0 1 2\n triangle 0 2 3\n triangle 4 5 6\n triangle 4 6 7\n")<<"}\n";
        Fixture f(Materials()+mesh.str()+SceneObject("caster","patch","mirror"));NativeRebuildMesh(f,indexed);
        auto* modifier=new InertFrameModifier;f.job->GetObjects()->GetItem("caster")->AssignModifier(*modifier);modifier->release();
        const Scalar z=side<0?0:.01;const Point3 start(0,.3,z+side*3),end(0,.3,z+side*4);IORStack air(1);
        RayIntersection hit(Ray(start,Vector3(0,0,-side)),nullRasterizerState);
        f.Object("caster")->IntersectRay(hit,RISE_INFINITY,true,true,false);
        Check(hit.geometric.bHit,"stacked patch native closest surface is reachable");
        if(!hit.geometric.bHit) continue;
        SMSDomainVertex vertex(hit.geometric);
        // The scene parser stores mesh coordinates at native precision. Seed
        // the actual traced surface, rather than an ideal decimal z=.01.
        const auto* native=dynamic_cast<const RISE::Implementation::Object*>(hit.pObject);
        Check(native!=nullptr,"stacked patch uses a native object surface convention");
        if(!native) continue;
        const Vector3 localDirection=Vector3Ops::Normalize(Vector3Ops::Transform(
            hit.pObject->GetFinalInverseTransformMatrix(),hit.geometric.ray.Dir()));
        vertex.geometry.position=Point3Ops::Transform(hit.pObject->GetFinalTransformMatrix(),
            Point3Ops::mkPoint3(hit.geometric.ptObjIntersec,localDirection*native->GetSurfaceIntersecError()));
        vertex.geometry.normal=hit.geometric.vNormal;
        vertex.geometry.geomNormal=hit.geometric.UnflippedGeomNormal();vertex.geometry.pObject=hit.pObject;
        vertex.geometry.pMaterial=hit.pMaterial;vertex.geometry.isReflection=true;
        ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=1;
        auto* solver=new ManifoldSolver(cfg);
        for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),SMSQueryDomain::NM(450),SMSQueryDomain::NM(650)}) {
            RandomNumberGenerator random(483);IndependentSampler sampler(random);std::vector<SMSDomainVertex> vertices{vertex};
            const auto result=solver->SolveDomain(start,Vector3(0,0,-side),end,Vector3(0,0,-side),f.Scene(),air,domain,vertices,sampler,1e-7,1e-10);
            std::cout<<"R5 stacked patches indexed="<<indexed<<" winding="<<reverse<<" side="<<side<<" solved="<<result.valid<<'\n';
            Check(result.valid,"inert-modifier native frame retains the reachable stacked-patch root");
            if(result.valid) Check(std::fabs(result.specularChain[0].position.z-z)<1e-7,"frame reconstruction cannot substitute another patch of the object");
            unsigned accepted=0;
            for(unsigned trial=0;trial<64 && accepted<2;++trial) {
                const auto root=solver->ProposeExtendedRoot(start,Vector3(0,0,-side),end,f.Scene(),air,domain,sampler);
                if(root.accepted) {++accepted;Check(std::fabs(root.vertices[0].geometry.position.z-z)<1e-7,"native stacked-patch proposal preserves its actual visible root");}
            }
            Check(accepted==2,"reachable stacked-patch root has positive complete proposal support");
        }
        solver->release();
    }
}
static void NativeHorizonFallbacks(unsigned fields=0) {
    for(bool reverse:{false,true}) for(int side:{-1,1}) for(const char* material:{"glass","mirror","coated","polished"})
        for(bool valid:{false,true}) for(bool varying:{false,true}) for(bool transformed:{false,true}) {
        const Scalar scale=transformed?1.5:1,offset=transformed?3:0;
        const std::string transform=transformed?" scale 1.5 1.5 1.5\n position 3 0 0\n":"";
        Fixture f(Materials()+
            "dielectric_material\n{\n name coated\n tau 1\n ior triple\n scattering 1000000\n ar_layer 1.224744871391589 112.26827987812466 0\n}\n"
            "uniformcolor_painter\n{\n name black\n color 0 0 0\n}\n"
            "polished_material\n{\n name polished\n reflectance black\n tau 0.7\n ior triple\n scattering 1000000\n}\n"
            +QuadMesh("patch",0,4.8,5.2,reverse)+SceneObject("caster","patch",material,transform));
        auto* modifier=new TiltNormal(side,valid,varying,fields);
        f.job->GetObjects()->GetItem("caster")->AssignModifier(*modifier);
        Check(f.Object("caster")->GetModifier()==modifier,"native tilt modifier is retained");modifier->release();
        const Point3 start(offset,.3*scale,side*scale),center(offset+5*scale,.3*scale,0);
        IORStack air(1);air.SetCurrentObject(f.Object("caster"));
        RasterizerState raster=nullRasterizerState;raster.x=17;raster.y=23;
        RayIntersection hit(Ray(start,Vector3Ops::Normalize(Vector3Ops::mkVector3(center,start))),raster);
        f.Object("caster")->IntersectRay(hit,RISE_INFINITY,true,true,false);
        Check(hit.geometric.bHit,"tilted native patch hit");
        if(!hit.geometric.bHit) continue;
        hit.pModifier->Modify(hit.geometric);
        ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=1;
        auto* solver=new ManifoldSolver(cfg);
        RandomNumberGenerator random(182);IndependentSampler sampler(random);
        for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),SMSQueryDomain::NM(450),SMSQueryDomain::NM(650)}) {
            ScatteredRayContainer rays;
            if(domain.kind==SMSQueryDomain::Wavelength) hit.pMaterial->GetSPF()->ScatterNM(hit.geometric,sampler,domain.nm,rays,air);
            else hit.pMaterial->GetSPF()->Scatter(hit.geometric,sampler,rays,air);
            for(unsigned j=0;j<rays.Count();++j) {
                const auto& ray=rays[j];
                if(!ray.isDelta || (ray.type!=ScatteredRay::eRayReflection && ray.type!=ScatteredRay::eRayRefraction)) continue;
                const Scalar nativeWeight=domain.kind==SMSQueryDomain::Wavelength?ray.krayNM:ray.kray[domain.component];
                if(nativeWeight<=0) continue;
                const bool reflection=ray.type==ScatteredRay::eRayReflection;
                const Point3 end=Point3Ops::mkPoint3(center,ray.ray.Dir()*5);
                SMSDomainVertex record(hit.geometric);
                record.geometry.position=center;record.geometry.normal=hit.geometric.vNormal;
                record.geometry.geomNormal=hit.geometric.UnflippedGeomNormal();
                record.geometry.pObject=hit.pObject;record.geometry.pMaterial=hit.pMaterial;
                record.geometry.isReflection=reflection;
                std::vector<SMSDomainVertex> vertices;vertices.push_back(record);
                const auto result=solver->SolveDomain(start,Vector3(1,0,0),end,-ray.ray.Dir(),f.Scene(),air,domain,vertices,sampler,1e-7,1e-10);
                std::cout<<"native horizon winding="<<reverse<<" side="<<side<<" material="<<material<<" valid="<<valid<<" varying="<<varying<<" transformed="<<transformed<<" R="<<reflection<<" domain="<<domain.kind<<":"<<domain.component<<":"<<domain.nm<<" native="<<nativeWeight<<" accepted="<<result.valid<<'\n';
                Check(result.valid,"extended solve represents native corrected delta direction");
                unsigned proposals=0;
                for(unsigned trial=0;trial<512 && proposals<8;++trial) {
                    auto root=solver->ProposeExtendedRoot(start,Vector3(1,0,0),end,f.Scene(),air,domain,sampler,raster);
                    if(root.accepted) {
                        ++proposals;
                        Check(Point3Ops::Distance(root.vertices[0].geometry.position,center)<1e-7,"native corrected root proposals reach the SPF endpoint");
                    }
                }
                Check(proposals==8,"native horizon fallback has positive complete-proposal coverage");
                if(result.valid) {
                    CheckNativeHorizonJacobian(cfg,result,vertices,start,end,f.Scene(),air,domain,sampler);
                    const Scalar etaScale=reflection?1:RadianceEtaScale(air,ray.ior_stack);
                    Check(std::fabs(result.contributionNM-nativeWeight*etaScale)<1e-8,"native corrected Fresnel and eta scaling agree at solved root");
                }
            }
        }
        solver->release();
    }
}

class AuditedScaledNormal final : public AuditedIdentityFrameModifier {
    bool varying;
public:
    explicit AuditedScaledNormal(bool varying_) : varying(varying_) {}
    static Vector3 Rotate(const Vector3& n,Scalar a) {
        return Vector3(std::cos(a)*n.x+std::sin(a)*n.z,n.y,-std::sin(a)*n.x+std::cos(a)*n.z);
    }
    void Modify(RayIntersectionGeometric& hit) const override {
        hit.vNormal=Rotate(hit.vNormal,varying?.025*hit.ptIntersection.x:0)
            *(2+(varying?.2*hit.ptIntersection.x:0));
    }
    bool SMSFrameDifferential(const RayIntersectionGeometric& raw,const TestSMSIntersectionDifferential& d,
        Vector3& normal,Vector3& frameW) const override {
        const Scalar angle=varying?.025*raw.ptIntersection.x:0;
        const Vector3 base=Rotate(raw.vNormal,angle);
        const Vector3 derivative=Rotate(d.normal,angle)
            +Vector3(base.z,0,-base.x)*(varying?.025*d.worldPoint.x:0);
        normal=derivative*(2+(varying?.2*raw.ptIntersection.x:0))
            +base*(varying?.2*d.worldPoint.x:0);
        frameW=d.frameW;return true;
    }
};
static void NativeScaledPolishedNormals() {
    for(bool reverse:{false,true}) for(int side:{-1,1}) for(bool transformed:{false,true}) for(bool varying:{false,true}) {
        const std::string transform=transformed?" scale 2 0.8 1.5\n orientation 0 45 0\n":"";
        Fixture f(Materials()+"uniformcolor_painter\n{\n name black\n color 0 0 0\n}\n"
            "polished_material\n{\n name polished\n reflectance black\n tau 1\n ior triple\n scattering 1000000\n}\n"
            +QuadMesh("patch",0,-8,8,reverse)+SceneObject("caster","patch","polished",transform));
        auto* modifier=new AuditedScaledNormal(varying);
        f.job->GetObjects()->GetItem("caster")->AssignModifier(*modifier);modifier->release();
        const auto* object=f.Object("caster");
        const Point3 center=Point3Ops::Transform(object->GetFinalTransformMatrix(),Point3(.3,.2,0));
        const Vector3 n=Vector3Ops::Normalize(Vector3Ops::Transform(
            Matrix4Ops::Transpose(object->GetFinalInverseTransformMatrix()),Vector3(0,0,side)));
        const Vector3 u=Vector3Ops::Normalize(Vector3Ops::Transform(object->GetFinalTransformMatrix(),Vector3(1,0,0)));
        const Point3 start=Point3Ops::mkPoint3(center,n+u*std::sqrt(Scalar(3)));
        IORStack air(1);air.SetCurrentObject(object);
        RayIntersection hit(Ray(start,Vector3Ops::Normalize(Vector3Ops::mkVector3(center,start))),nullRasterizerState);
        f.Scene().GetObjects()->IntersectRay(hit,true,true,false);
        Check(hit.geometric.bHit && hit.pObject==object,"scaled polished normal fixture traces its actual indexed surface");
        if(!hit.geometric.bHit) continue;
        modifier->Modify(hit.geometric);
        Check(Vector3Ops::Magnitude(hit.geometric.vNormal)>1.5,"audited provider genuinely supplies a nonunit shading normal");
        ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=1;
        auto* solver=new ManifoldSolver(cfg);RandomNumberGenerator random(893);IndependentSampler sampler(random);
        for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),SMSQueryDomain::NM(450),SMSQueryDomain::NM(650)}) {
            ScatteredRayContainer rays;
            if(domain.kind==SMSQueryDomain::Wavelength) hit.pMaterial->GetSPF()->ScatterNM(hit.geometric,sampler,domain.nm,rays,air);
            else hit.pMaterial->GetSPF()->Scatter(hit.geometric,sampler,rays,air);
            unsigned reflected=0;
            for(unsigned j=0;j<rays.Count();++j) {
                const auto& ray=rays[j];
                if(!ray.isDelta || ray.type!=ScatteredRay::eRayReflection) continue;
                ++reflected;
                const Scalar nativeWeight=domain.kind==SMSQueryDomain::Wavelength?ray.krayNM:ray.kray[domain.component];
                IORStack replay(air);Scalar etaI=0,etaT=0,price=0;bool exiting=false;
                const bool priced=SMSDomainReplay::Cross(*hit.pMaterial,object,hit.geometric,domain,true,
                    replay,etaI,etaT,exiting)
                    &&SMSDomainReplay::EventWeight(*hit.pMaterial,hit.geometric,air,domain,true,exiting,etaI,etaT,0,price);
                std::cout<<"R8 scaled polished winding="<<reverse<<" side="<<side<<" transformed="<<transformed
                    <<" varying="<<varying<<" domain="<<domain.kind<<":"<<domain.component<<":"<<domain.nm
                    <<" native="<<std::setprecision(17)<<nativeWeight<<" replay="<<price<<'\n';
                Check(priced&&std::fabs(price-nativeWeight)<1e-9,"replay matches native polished normalization and Fresnel");
                const Point3 end=Point3Ops::mkPoint3(center,ray.ray.Dir()*4);
                SMSDomainVertex record(hit.geometric);
                record.geometry.position=center;record.geometry.normal=hit.geometric.vNormal;
                record.geometry.geomNormal=hit.geometric.UnflippedGeomNormal();
                record.geometry.pObject=object;record.geometry.pMaterial=hit.pMaterial;record.geometry.isReflection=true;
                std::vector<SMSDomainVertex> vertices{record};
                const auto result=solver->SolveDomain(start,n,end,-ray.ray.Dir(),f.Scene(),air,domain,vertices,sampler,1e-7,1e-10);
                Check(result.valid,"scaled-normal polished replay retains the actual native reflected root");
                if(result.valid) {
                    Check(std::fabs(result.contributionNM-nativeWeight)<1e-9,"solved polished root matches native nonunit-normal price");
                    CheckNativeHorizonJacobian(cfg,result,vertices,start,end,f.Scene(),air,domain,sampler);
                }
            }
            Check(reflected==1,"both indexed windings/incidences provide one native polished delta lobe");
        }
        solver->release();
    }
}

static void NativeClosedHorizonFallbacks() {
    for(bool reverse:{false,true}) for(bool transformed:{false,true}) for(bool nested:{false,true}) for(bool valid:{false,true}) {
        const Scalar scale=transformed?1.5:1,offset=transformed?3:0;
        std::ostringstream transform;
        transform<<" scale "<<.2*scale<<' '<<2*scale<<' '<<.05*scale
            <<"\n position "<<offset+5*scale<<" 0 "<<-.05*scale<<"\n";
        std::ostringstream enclosure;
        if(nested) enclosure<<"perfectrefractor_material\n{\n name outer_mat\n refractance white\n ior 1.1\n}\n"
            <<"sphere_geometry\n{\n name outer_shape\n radius "<<10*scale<<"\n}\n"
            <<SceneObject("outer","outer_shape","outer_mat"," position "+std::to_string(offset+5*scale)+" 0 0\n");
        Fixture f(Materials()+Mesh(true,reverse)+SceneObject("caster","shape","glass",transform.str())+enclosure.str());
        auto* modifier=new TiltNormal(-1,valid,true);
        f.job->GetObjects()->GetItem("caster")->AssignModifier(*modifier);modifier->release();
        const Point3 start(offset+4.9*scale,.3*scale,-.02*scale),center(offset+5*scale,.3*scale,0);
        RasterizerState raster=nullRasterizerState;raster.x=17;raster.y=23;
        RayIntersection hit(Ray(start,Vector3Ops::Normalize(Vector3Ops::mkVector3(center,start))),raster);
        f.Object("caster")->IntersectRay(hit,RISE_INFINITY,true,true,false);
        Check(hit.geometric.bHit && !hit.geometric.bProvablyNoInterior,"closed tilted start-inside fixture has a real solid exit");
        if(!hit.geometric.bHit) continue;
        hit.pModifier->Modify(hit.geometric);
        ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=1;
        auto* solver=new ManifoldSolver(cfg);RandomNumberGenerator random(984);IndependentSampler sampler(random);
        for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),SMSQueryDomain::NM(450),SMSQueryDomain::NM(650)}) {
            IORStack stack(1);
            if(nested) {stack.SetCurrentObject(f.Object("outer"));stack.push(1.1);}
            stack.SetCurrentObject(f.Object("caster"));
            SMSNativeMaterialQuery query;
            Check(SMSDomainReplay::Query(*hit.pMaterial,hit.geometric,stack,domain,query),"closed native index query succeeds");
            stack.push(query.index);
            ScatteredRayContainer rays;
            if(domain.kind==SMSQueryDomain::Wavelength) hit.pMaterial->GetSPF()->ScatterNM(hit.geometric,sampler,domain.nm,rays,stack);
            else hit.pMaterial->GetSPF()->Scatter(hit.geometric,sampler,rays,stack);
            for(unsigned j=0;j<rays.Count();++j) {
                const auto& ray=rays[j];
                const Scalar native=domain.kind==SMSQueryDomain::Wavelength?ray.krayNM:ray.kray[domain.component];
                if(native<=0 || !ray.isDelta) continue;
                const bool reflection=ray.type==ScatteredRay::eRayReflection;
                const Point3 end=Point3Ops::mkPoint3(center,ray.ray.Dir()*(.02*scale));
                SMSDomainVertex record(hit.geometric);
                record.geometry.position=center;record.geometry.normal=hit.geometric.vNormal;
                record.geometry.geomNormal=hit.geometric.UnflippedGeomNormal();
                record.geometry.pObject=hit.pObject;record.geometry.pMaterial=hit.pMaterial;
                record.geometry.isReflection=reflection;
                std::vector<SMSDomainVertex> vertices;vertices.push_back(record);
                const auto result=solver->SolveDomain(start,Vector3(1,0,0),end,-ray.ray.Dir(),f.Scene(),stack,domain,vertices,sampler,1e-7,1e-10);
                std::cout<<"closed native horizon winding="<<reverse<<" transformed="<<transformed<<" nested="<<nested<<" valid="<<valid<<" R="<<reflection<<" accepted="<<result.valid<<'\n';
                Check(result.valid,"native horizon law replays a closed start-inside and nested exit");
                auto shortVertices=vertices;
                const Point3 shortEnd=Point3Ops::mkPoint3(center,ray.ray.Dir()*.005);
                const auto shortResult=solver->SolveDomain(start,Vector3(1,0,0),shortEnd,-ray.ray.Dir(),f.Scene(),stack,domain,shortVertices,sampler,1e-7,1e-10);
                Check(!shortResult.valid,"native event correction preserves the shared minimum-segment acceptance filter");
                if(result.valid) {
                    Check(vertices[0].geometry.isExiting,"closed start-inside root retains native exit membership");
                    Check(std::fabs(result.contributionNM-native*(reflection?1:RadianceEtaScale(stack,ray.ior_stack)))<1e-8,
                        "closed and nested native event price agrees with SPF and radiance eta scaling");
                    CheckNativeHorizonJacobian(cfg,result,vertices,start,end,f.Scene(),stack,domain,sampler);
                }
            }
        }
        solver->release();
    }
}
// Non-top identity removal occurs in real overlapping closed solids.
// The native direction/Fresnel and the consumer radiance scale are separate
// oracles: native SPF kray deliberately excludes the latter.
static void NativeOverlapExits() {
    for(bool reverse:{false,true}) for(bool transformed:{false,true})
        for(const char* material:{"overlap_glass","overlap_dielectric","overlap_coated"}) for(bool tir:{false,true}) {
        const Scalar scale=transformed?1.5:1,offset=transformed?3:0;
        std::ostringstream aTransform,bTransform;
        aTransform<<" scale "<<scale<<' '<<scale<<' '<<scale<<"\n position "<<offset<<" 0 0\n";
        bTransform<<" scale "<<scale<<' '<<scale<<' '<<scale<<"\n position "<<offset<<" 0 "<<.6*scale<<"\n";
        Fixture f(Materials()+
            "perfectrefractor_material\n{\n name overlap_glass\n refractance white\n ior 1.5\n}\n"
            "dielectric_material\n{\n name overlap_dielectric\n tau 1\n ior 1.5\n scattering 1000000\n}\n"
            "dielectric_material\n{\n name overlap_coated\n tau 1\n ior 1.5\n scattering 1000000\n ar_layer 1.224744871391589 112.26827987812466 0\n}\n"
            "perfectrefractor_material\n{\n name overlap_outer\n refractance white\n ior 1.3\n}\n"
            +Mesh(true,reverse)+SceneObject("a","shape",material,aTransform.str())+SceneObject("b","shape","overlap_outer",bTransform.str()));
        const Point3 center(offset+.25*scale,.3*scale,scale);
        const Point3 start(offset+(.25-(tir?.55:.2))*scale,.3*scale,.8*scale);
        RayIntersection hit(Ray(start,Vector3Ops::Normalize(Vector3Ops::mkVector3(center,start))),nullRasterizerState);
        f.Scene().GetObjects()->IntersectRay(hit,true,true,false);
        Check(hit.geometric.bHit && hit.pObject==f.Object("a") && !hit.geometric.bProvablyNoInterior,
            "overlapping real solids exit the non-top member first");
        if(!hit.geometric.bHit || hit.pObject!=f.Object("a")) continue;
        for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),SMSQueryDomain::NM(450),SMSQueryDomain::NM(650)}) {
            IORStack stack(1);stack.SetCurrentObject(f.Object("a"));stack.push(1.5);
            stack.SetCurrentObject(f.Object("b"));stack.push(1.3);stack.SetCurrentObject(f.Object("a"));
            RandomNumberGenerator random(984);IndependentSampler sampler(random);ScatteredRayContainer rays;
            if(domain.kind==SMSQueryDomain::Wavelength) hit.pMaterial->GetSPF()->ScatterNM(hit.geometric,sampler,domain.nm,rays,stack);
            else hit.pMaterial->GetSPF()->Scatter(hit.geometric,sampler,rays,stack);
            unsigned nativeTransmissions=0;
            for(unsigned j=0;j<rays.Count();++j) {
                const auto& ray=rays[j];
                const Scalar native=domain.kind==SMSQueryDomain::Wavelength?ray.krayNM:ray.kray[domain.component];
                if(native<=0 || !ray.isDelta) continue;
                const bool reflection=ray.type==ScatteredRay::eRayReflection;nativeTransmissions+=!reflection;
                IORStack replay(stack);Scalar etaI=0,etaT=0;bool exiting=false;
                const bool crossed=SMSDomainReplay::Cross(*hit.pMaterial,hit.pObject,hit.geometric,domain,reflection,replay,etaI,etaT,exiting);
                Check(crossed && exiting && etaI==1.5 && etaT==1.3,
                    "overlap exit directions query the exiting object and remaining destination");
                Scalar weight=0;
                Check(crossed && SMSDomainReplay::EventWeight(*hit.pMaterial,hit.geometric,stack,domain,reflection,exiting,etaI,etaT,
                    Point3Ops::Distance(start,center),weight),"overlap native event weight is available");
                Check(std::fabs(weight-native*(reflection?1:RadianceEtaScale(stack,ray.ior_stack)))<1e-8,
                    "overlap event price matches native SPF and consumer eta scale independently");
                if(!tir) {
                    ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=1;auto* solver=new ManifoldSolver(cfg);
                    SMSDomainVertex record(hit.geometric);record.geometry.position=center;
                    record.geometry.normal=hit.geometric.vNormal;record.geometry.geomNormal=hit.geometric.UnflippedGeomNormal();
                    record.geometry.pObject=hit.pObject;record.geometry.pMaterial=hit.pMaterial;record.geometry.isReflection=reflection;
                    std::vector<SMSDomainVertex> vertices{record};
                    const Point3 end=Point3Ops::mkPoint3(center,ray.ray.Dir()*(.1*scale));
                    const auto result=solver->SolveDomain(start,Vector3(0,0,1),end,-ray.ray.Dir(),f.Scene(),stack,domain,vertices,sampler,1e-7,1e-10);
                    Check(result.valid && Point3Ops::Distance(vertices[0].geometry.position,center)<1e-8*scale,
                        "overlap solve reproduces the actual native R/T root");
                    if(result.valid) Check(std::fabs(result.contributionNM-native*(reflection?1:RadianceEtaScale(stack,ray.ior_stack)))<1e-8,
                        "overlap solved native event price agrees with the separate consumer oracle");
                    const Scalar u=reverse?.049375:.859375,v=reverse?.625/.975:.025/.375;
                    Point3 sampled;Vector3 sampledNormal;Point2 sampledUV;
                    hit.pObject->UniformRandomPoint(&sampled,&sampledNormal,&sampledUV,Point3(u,v,.29));
                    Check(Point3Ops::Distance(sampled,center)<1e-12*scale,
                        "scripted overlap surface draw reaches the actual native endpoint");
                    ScriptSampler rootSampler({.01,u,v,.29,reflection?.001:.999},.999);
                    const auto root=solver->ProposeExtendedRoot(start,Vector3(0,0,1),end,f.Scene(),stack,domain,rootSampler);
                    Check(root.accepted && Point3Ops::Distance(root.vertices[0].geometry.position,center)<1e-8*scale,
                        "complete overlap proposal retains the native solved R/T root and visibility");
                    if(root.accepted) Check(std::fabs(root.result.contributionNM-native*(reflection?1:RadianceEtaScale(stack,ray.ior_stack)))<1e-8,
                        "complete overlap proposal matches native price and consumer eta scale");
                    solver->release();
                }
            }
            Check(tir?nativeTransmissions==0:nativeTransmissions>0,"native overlap fixture covers transmission and TIR");
            if(tir) {IORStack replay(stack);Scalar etaI,etaT;bool exiting;
                Check(!SMSDomainReplay::Cross(*hit.pMaterial,hit.pObject,hit.geometric,domain,false,replay,etaI,etaT,exiting),
                    "overlap geometric TIR cannot be proposed as transmission");}
        }
    }
}
class ConstraintOracle : public ManifoldSolver {
public:
    using ManifoldSolver::ManifoldSolver;
    using ManifoldSolver::EvaluateConstraint;
    using ManifoldSolver::BuildJacobian;
};
static void CheckJacobian(ConstraintOracle& solver,const SMSDomainRoot& root,const Point3& start,const Point3& end) {
    const auto& chain=root.result.specularChain;
    std::vector<Scalar> diagonal,upper,lower;
    solver.BuildJacobian(chain,start,end,diagonal,upper,lower,true);
    const std::size_t count=chain.size();const Scalar h=1e-5*root.scale;
    for(std::size_t column=0;column<2*count;++column) {
        auto plus=chain,minus=chain;const std::size_t j=column/2;
        const Vector3 tangent=column%2?chain[j].dpdv:chain[j].dpdu;
        plus[j].position=Point3Ops::mkPoint3(plus[j].position,tangent*h);
        minus[j].position=Point3Ops::mkPoint3(minus[j].position,-tangent*h);
        std::vector<Scalar> a,b;solver.EvaluateConstraint(plus,start,end,a);solver.EvaluateConstraint(minus,start,end,b);
        for(std::size_t row=0;row<2*count;++row) {
            const std::size_t i=row/2,offset=4*i+2*(row%2)+column%2;
            const Scalar expected=i==j?diagonal[offset]:j==i+1?upper[offset]:i==j+1?lower[4*(i-1)+2*(row%2)+column%2]:0;
            const Scalar observed=(a[row]-b[row])/(2*h);
            if(std::fabs(observed-expected)>1e-5*std::max(Scalar(1),std::fabs(expected)))
                std::cout<<"Jacobian row="<<row<<" col="<<column<<" expected="<<expected<<" observed="<<observed<<" count="<<count<<" domain="<<root.domain.component<<'\n';
            Check(std::fabs(observed-expected)<=1e-5*std::max(Scalar(1),std::fabs(expected)),
                "native mixed-event constraint Jacobian agrees with central differences");
        }
    }
}
class ArrivingRayNormal final : public AuditedIdentityFrameModifier {
public:
    bool SMSFrameDifferential(const RayIntersectionGeometric& raw,const TestSMSIntersectionDifferential& d,
        Vector3& n,Vector3& w) const override {
        n=FixtureNormalizedDifferential(raw.vNormal+Vector3(.025*raw.ray.Dir().x,.02*raw.ray.Dir().y,0),
            d.normal+Vector3(.025*d.rayDirection.x,.02*d.rayDirection.y,0));w=n;return true;
    }
    void Modify(RayIntersectionGeometric& hit) const override {
        hit.vNormal=Vector3Ops::Normalize(hit.vNormal+Vector3(.025*hit.ray.Dir().x,.02*hit.ray.Dir().y,0));
        hit.onb.CreateFromW(hit.vNormal);
    }
};
static void ModifiedWalkJacobians() {
    for(bool reverse:{false,true}) for(int side:{-1,1}) for(bool transformed:{false,true})
        for(unsigned count:{2u,3u}) {
        const Scalar scale=transformed?1.5:1,offset=transformed?3:0;
        const std::string transform=transformed?" scale 1.5 1.5 1.5\n position 3 0 0\n":"";
        Fixture f(Materials()+QuadMesh("first",0,-2,2,reverse)+QuadMesh("gate",-side,.3,2,reverse)
            +QuadMesh("last",-2*side,.6,4,reverse)+SceneObject("a_first","first","mirror",transform)
            +SceneObject("b_gate","gate","glass",transform)+SceneObject("c_last","last","mirror",transform));
        auto* modifier=new ArrivingRayNormal();
        for(const char* name:{"a_first","b_gate","c_last"}) {
            f.job->GetObjects()->GetItem(name)->AssignModifier(*modifier);
            Check(f.Object(name)->GetModifier()==modifier,"mixed-event arriving-ray modifier retained");
        }
        modifier->release();
        const Point3 start(offset-scale,0,-2*side*scale);
        for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),SMSQueryDomain::NM(450),SMSQueryDomain::NM(650)}) {
            ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=count;
            auto* solver=new ManifoldSolver(cfg);IORStack air(1);
            const auto draws=[&](){return std::vector<Scalar>{.01,reverse?.0975:.75,reverse?10./19:.1,.6,.99};};
            ScriptSampler sampler(draws(),.99);std::vector<SMSDomainVertex> vertices;
            const bool built=solver->BuildExtendedSeed(start,Point3(offset,0,-1.5*side*scale),f.Scene(),air,domain,sampler,vertices);
            Check(built&&vertices.size()==count,"native double-sided transformed R-T and R-T-R walk has its exact event count");
            if(built&&vertices.size()==count) {
                Check(vertices[0].geometry.isReflection&&!vertices[1].geometry.isReflection
                    &&(count==2||vertices[2].geometry.isReflection),"native proposal records R-T or R-T-R without event relabelling");
                const Point3 previous=count==2?vertices[0].geometry.position:vertices[1].geometry.position;
                const auto& last=vertices.back().geometry;
                Vector3 outgoing=Vector3Ops::Normalize(Vector3Ops::mkVector3(last.position,previous));
                if(last.isReflection) outgoing=Optics::CalculateReflectedRay(outgoing,last.normal);
                else {const Vector3 normal=Vector3Ops::Dot(outgoing,last.normal)<0?last.normal:-last.normal;
                    Check(Optics::CalculateRefractedRay(normal,last.etaI,last.etaT,outgoing),"chosen native transmission is below TIR");}
                const Point3 end=Point3Ops::mkPoint3(last.position,outgoing*(.3*scale));
                ScriptSampler rootSampler(draws(),.99);
                const auto root=solver->ProposeExtendedRoot(start,Vector3(0,0,side),end,f.Scene(),air,domain,rootSampler);
                Check(root.accepted,"native mixed-event root passes full ordered scene visibility");
                if(root.accepted) CheckNativeHorizonJacobian(cfg,root.result,root.vertices,start,end,f.Scene(),air,domain,rootSampler);
            }
            solver->release();
        }
    }
}
static void WalkEvents() {
    for(bool reverse:{false,true}) for(int side:{-1,1}) for(bool transformed:{false,true})
        for(unsigned count:{2u,3u}) {
        const Scalar scale=transformed?1.5:1,offset=transformed?3:0;
        const std::string transform=transformed?" scale 1.5 1.5 1.5\n position 3 0 0\n":"";
        Fixture f(Materials()+QuadMesh("first",0,-2,2,reverse)+QuadMesh("gate",-side,.3,2,reverse)
            +QuadMesh("last",-2*side,.6,4,reverse)+SceneObject("a_first","first","mirror",transform)
            +SceneObject("b_gate","gate","glass",transform)+SceneObject("c_last","last","mirror",transform));
        const Point3 start(offset-scale,0,-2*side*scale);
        for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),SMSQueryDomain::NM(450),SMSQueryDomain::NM(650)}) {
            ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=count;
            auto* solver=new ConstraintOracle(cfg);IORStack air(1);
            const auto draws=[&](){return std::vector<Scalar>{.01,reverse?.0975:.75,reverse?10./19:.1,.6,.99};};
            ScriptSampler sampler(draws(),.99);std::vector<SMSDomainVertex> vertices;
            const bool built=solver->BuildExtendedSeed(start,Point3(offset,0,-1.5*side*scale),f.Scene(),air,domain,sampler,vertices);
            Check(built&&vertices.size()==count,"native double-sided transformed R-T and R-T-R walk has its exact event count");
            if(built&&vertices.size()==count) {
                Check(vertices[0].geometry.isReflection&&!vertices[1].geometry.isReflection
                    &&(count==2||vertices[2].geometry.isReflection),"native proposal records R-T or R-T-R without event relabelling");
                const Point3 previous=count==2?vertices[0].geometry.position:vertices[1].geometry.position;
                const auto& last=vertices.back().geometry;
                Vector3 outgoing=Vector3Ops::Normalize(Vector3Ops::mkVector3(last.position,previous));
                if(last.isReflection) outgoing=Optics::CalculateReflectedRay(outgoing,last.normal);
                else {const Vector3 normal=Vector3Ops::Dot(outgoing,last.normal)<0?last.normal:-last.normal;
                    Check(Optics::CalculateRefractedRay(normal,last.etaI,last.etaT,outgoing),"chosen native transmission is below TIR");}
                const Point3 end=Point3Ops::mkPoint3(last.position,outgoing*(.3*scale));
                ScriptSampler rootSampler(draws(),.99);
                const auto root=solver->ProposeExtendedRoot(start,Vector3(0,0,side),end,f.Scene(),air,domain,rootSampler);
                Check(root.accepted,"native mixed-event root passes full ordered scene visibility");
                if(root.accepted) CheckJacobian(*solver,root,start,end);
            }
            solver->release();
        }
    }
    for(bool reverse:{false,true}) for(bool transformed:{false,true}) {
        const Scalar scale=transformed?1.5:1,offset=transformed?3:0;
        const std::string transform=transformed?" scale 1.5 1.5 1.5\n position 3 0 0\n":"";
        Fixture f(Materials()+"perfectrefractor_material\n{\n name dense\n refractance white\n ior 2\n}\n"
            +Mesh(true,reverse)+SceneObject("a_inner","shape","dense",transform)
            +SceneObject("z_outer","shape","glass",transformed?" scale 3 3 3\n position 3 0 0\n":" scale 2 2 2\n"));
        const Point3 start(offset,0,0),end(offset,0,4*scale);
        IORStack live(1);live.SetCurrentObject(f.Object("z_outer"));live.push(1.3);
        live.SetCurrentObject(f.Object("a_inner"));live.push(2);const IORStack before(live);
        for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),SMSQueryDomain::NM(450),SMSQueryDomain::NM(650)}) {
            ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=2;auto* solver=new ConstraintOracle(cfg);
            const auto draws=[&](){return std::vector<Scalar>{.01,reverse?.19:.75,reverse?5./9:.2,.29,.99,.99};};
            ScriptSampler sampler(draws(),.99);std::vector<SMSDomainVertex> vertices;
            const bool built=solver->BuildExtendedSeed(start,end,f.Scene(),live,domain,sampler,vertices);
            Check(built&&vertices.size()==2,"native nested start-inside proposal exits both closed objects");
            if(built&&vertices.size()==2) {
                Check(!vertices[0].geometry.isReflection&&!vertices[1].geometry.isReflection
                    &&vertices[0].geometry.isExiting&&vertices[1].geometry.isExiting,"nested seed events are traced transmissions and exits");
                Check(vertices[0].geometry.etaI==2&&vertices[0].geometry.etaT==vertices[1].geometry.etaI
                    &&vertices[1].geometry.etaT==1,"nested proposal restores enclosing-domain index then environment");
            }
            ScriptSampler rootSampler(draws(),.99);const auto root=solver->ProposeExtendedRoot(start,Vector3(0,0,1),end,f.Scene(),live,domain,rootSampler);
            Check(root.accepted,"nested start-inside root passes solve and ordered acceptance");
            if(root.accepted) CheckJacobian(*solver,root,start,end);
            Check(live.SameInterfaces(before),"nested proposals do not mutate the live anchor membership");
            solver->release();
        }
        // At a dense closed-object exit the native TIR law overrides a high
        // transmission draw, retaining reflection and the same membership.
        ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=1;auto* solver=new ManifoldSolver(cfg);
        IORStack dense(1);dense.SetCurrentObject(f.Object("a_inner"));dense.push(2);
        // Use the inner-only scene so the enclosing medium is genuinely air.
        Fixture tir(Materials()+"perfectrefractor_material\n{\n name dense\n refractance white\n ior 2\n}\n"
            +Mesh(true,reverse)+SceneObject("caster","shape","dense",transform));
        dense=IORStack(1);dense.SetCurrentObject(tir.Object("caster"));dense.push(2);
        ScriptSampler sampler({.01,reverse?.19:.75,reverse?5./9:.2,.29,.999},.999);
        std::vector<SMSDomainVertex> vertices;
        Check(solver->BuildExtendedSeed(Point3(offset+.8*scale,0,0),Point3(offset,0,3*scale),tir.Scene(),dense,
            SMSQueryDomain::RGB(2),sampler,vertices)&&vertices.size()==1&&vertices[0].geometry.isReflection,
            "real transformed closed-mesh TIR is R only even with a transmission draw");
        solver->release();
    }
}
static void Geometry() {
    for(bool closed:{false,true}) for(bool winding:{false,true}) for(int side:{-1,1})
        for(bool transformed:{false,true}) for(bool inside:{false,true}) {
            if(inside&&!closed) continue;
            Fixture f(Materials()+Mesh(closed,winding)+SceneObject("caster","shape","glass",
                transformed?" scale 1.5 1.5 1.5\n position 2 0 0\n":""));
            if(!f.job||!f.Object("caster")) {Check(false,"mesh fixture object");continue;}
            const Point3 start(transformed?2:0,0,inside?0:side*4);
            IORStack stack(1);
            if(inside) {stack.SetCurrentObject(f.Object("caster"));stack.push(1.3);}
            for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),SMSQueryDomain::NM(450),SMSQueryDomain::NM(650)}) {
                ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=1;
                auto* solver=new ManifoldSolver(cfg);
                const IORStack before(stack);
                ScriptSampler sampler({.2,.36,.51,.7,.001});std::vector<SMSDomainVertex> vertices;
                Check(solver->BuildExtendedSeed(start,Point3(start.x+.2,0,start.z),f.Scene(),stack,domain,sampler,vertices),
                    "native R seed on real double-sided open/closed both-side/winding/transform/start-inside mesh");
                Check(vertices.size()==1&&vertices[0].geometry.isReflection,"below-TIR reflection is sampled instead of fixed transmission");
                Check(stack.SameInterfaces(before),"proposal leaves anchor stack unchanged");
                solver->release();
            }
        }
    for(bool sphere:{false,true}) {
        const std::string geometry=sphere?"sphere_geometry\n{\n name shape\n radius 1\n}\n":PlaneScene("shape",0);
        Fixture f(Materials()+geometry+SceneObject("caster","shape","glass"));
        ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=1;cfg.solverThreshold=1e-10;
        auto* solver=new ManifoldSolver(cfg);IORStack air(1);
        ScriptSampler s({.2,sphere?.99:.5,.5,.5,.001});
        const auto root=solver->ProposeExtendedRoot(Point3(-.5,0,-3),Vector3(0,0,1),Point3(.5,0,-3),f.Scene(),air,SMSQueryDomain::RGB(1),s);
        std::cout<<"reflection control sphere="<<sphere<<" accepted="<<root.accepted<<" solved="<<root.result.valid<<" vertices="<<root.vertices.size()<<"\n";
        Check(root.accepted,"sphere/plane reflection root passes native solve and ordered scene acceptance");
        if(root.accepted) {
            Check(ManifoldSolver::SameExtendedRoot(root,root,1e-7),"root identity reflexive on actual solved geometry");
            Check(!ManifoldSolver::SameExtendedRoot(root,root,0),"zero numerical resolution is uncertain and rejected");
            auto changed=root;changed.scale=0;
            Check(!ManifoldSolver::SameExtendedRoot(root,changed,1e-7),"invalid scene scale is uncertain and rejected");
            changed=root;changed.vertices[0].geometry.normal.x=std::numeric_limits<Scalar>::quiet_NaN();
            Check(!ManifoldSolver::SameExtendedRoot(root,changed,1e-7),"nonfinite geometry is uncertain and rejected");
            changed=root;changed.vertices[0].geometry.uv.x+=.1;
            Check(!ManifoldSolver::SameExtendedRoot(root,changed,1e-7),"root identity includes native UV context");
            changed=root;changed.domain=SMSQueryDomain::RGB(2);
            Check(!ManifoldSolver::SameExtendedRoot(root,changed,1e-7),"root identity includes channel");
            changed=root;changed.vertices[0].geometry.isReflection=false;
            Check(!ManifoldSolver::SameExtendedRoot(root,changed,1e-7),"root identity includes event");
            changed=root;changed.startingStack.SetCurrentObject(f.Object("caster"));changed.startingStack.push(1.5);
            Check(!ManifoldSolver::SameExtendedRoot(root,changed,1e-7),"root identity includes starting membership");
            changed=root;changed.vertices[0].geometry.position.x+=.001;
            Check(!ManifoldSolver::SameExtendedRoot(root,changed,1e-7),"close separated geometry is not the same root");
        }
        solver->release();
    }
}

struct SSSTestShaderOp : PathTracingShaderOp {
    SSSTestShaderOp(const ManifoldSolverConfig& cfg,const StabilityConfig& stability,
        const std::vector<const IObject*>& objects) : PathTracingShaderOp(cfg,stability) {
        SetMaxPathDepth(5);if(pIntegrator->GetSolver()) pIntegrator->GetSolver()->SetSpecularCasters(objects);
    }
};
static void NativeSSSReferenceClamps() {
    for(bool randomWalk:{false,true}) for(bool winding:{false,true}) for(bool reference:{true,false}) {
        std::string text=Materials()+"lambertian_material\n{\n name diffuse\n reflectance white\n}\n"
            +(randomWalk?"randomwalk_sss_material\n":"subsurfacescattering_material\n")
            +"{\n name sss\n ior 1.3\n absorption .1\n scattering "
            +(randomWalk?"10":"1")+"\n g 0\n roughness .3\n}\n"
            +"sphere_geometry\n{\n name ball\n radius .5\n}\n"+SceneObject("sss","ball","sss")
            +QuadMesh("floor_geo",-2,-20,20,winding)+SceneObject("floor","floor_geo","diffuse")
            +QuadMesh("mirror_geo",4,-20,20,winding)+SceneObject("mirror","mirror_geo","mirror")
            +(reference?"spot_light\n{\n name source\n position 3 0 1\n target 3 0 3\n color 1 1 1\n power 1000000\n inner 80\n outer 85\n}\n":
                "omni_light\n{\n name source\n position 3 0 1\n color 1 1 1\n power 1000000\n}\n");
        Fixture fixture(text);if(!fixture.job) {Check(false,"SSS clamp scene prepared");continue;}
        std::vector<const IObject*> objects;ManifoldSolver::EnumerateSpecularCasters(fixture.Scene(),objects);
        for(unsigned mode=0;mode<3;++mode) {
            std::array<std::vector<double>,2> means;
            for(unsigned salt=0;salt<4;++salt) {
                const unsigned seed=SobolSequence::HashCombine(8731+salt,0x535353);
                SobolSamplerTestHooks::ValueSalt().store(seed);
                for(unsigned clamped=0;clamped<2;++clamped) {
                    ManifoldSolverConfig cfg;cfg.enabled=reference;cfg.extendedMode=true;cfg.targetBounces=1;
                    cfg.biased=true;cfg.multiTrials=1;cfg.seedingMode=ManifoldSolverConfig::eSeedingUniform;
                    SMSReferenceCounters counters;cfg.referenceCounters=&counters;
                    StabilityConfig stability;stability.rrMinDepth=20;stability.indirectClamp=clamped?1e-9:0;
                    auto* op=new SSSTestShaderOp(cfg,stability,objects);
                    std::vector<IShaderOp*> ops{op};IShader* shader=nullptr;
                    Check(RISE_API_CreateStandardShader(&shader,ops),"SSS native PT shader created");
                    if(!shader) continue;
                    auto* caster=new RayCaster(false,16,*shader,true);caster->AttachScene(&fixture.Scene());
                    auto* integrator=new PathTracingIntegrator(cfg,stability);integrator->SetMaxPathDepth(5);
                    if(integrator->GetSolver()) integrator->GetSolver()->SetSpecularCasters(objects);
                    double sum=0;
                    for(unsigned sample=0;sample<128;++sample) {
                        SobolSampler sampler(sample,47);RandomNumberGenerator random(seed+sample);
                        RuntimeContext context(random,RuntimeContext::PASS_NORMAL,false);
                        context.pSampler=&sampler;context.pStabilityConfig=&stability;
                        RayIntersection hit(Ray(Point3(0,0,1),Vector3(0,0,-1)),nullRasterizerState);
                        fixture.Scene().GetObjects()->IntersectRay(hit,true,true,false);
                        IORStack air(1);
                        if(mode==0) {
                            auto v=integrator->IntegrateFromHit(context,nullRasterizerState,hit,fixture.Scene(),*caster,
                                sampler,nullptr,1,air,1,RISEPel(1),true,1,IRayCaster::RAY_STATE::eRayDiffuse,
                                0,0,0,0,0,0,false,false);sum+=v[0];
                        } else sum+=integrator->IntegrateFromHitNM(context,nullRasterizerState,hit,mode==1?450:650,
                            fixture.Scene(),*caster,sampler,nullptr,1,air,1,1,true,1,
                            IRayCaster::RAY_STATE::eRayDiffuse,0,0,0,0,0,0);
                    }
                    means[clamped].push_back(sum/128);
                    std::cout<<"SSS clamp rw="<<randomWalk<<" winding="<<winding<<" reference="<<reference
                        <<" mode="<<mode<<" salt="<<salt<<" clamped="<<clamped<<" mean="<<sum/128
                        <<" trials="<<counters.proposalTrials.load()<<std::endl;
                    if(reference) Check(counters.proposalTrials.load()>0,"SSS shader dispatch reaches reference proposals");
                    integrator->release();caster->release();shader->release();op->release();
                }
            }
            for(unsigned salt=0;salt<means[0].size();++salt) {
                Check(means[0][salt]>0,"SSS clamp witness has positive native radiance");
                if(reference) Check(means[0][salt]==means[1][salt],"SSS reference radiance survives caller indirect clamp");
                else Check(means[1][salt]<means[0][salt],"ordinary SSS continuation retains indirect clamp");
            }
        }
    }
    SobolSamplerTestHooks::ValueSalt().store(0);
}

#endif
// Native analytic virtual-image reference for an upward spot reflected by
// one plane: f * F * Le / (anchor-to-plane + light-to-plane)^2.
static void DeltaLights(bool production=false, bool signedEmitter=false, bool uniform=false,unsigned signalKind=0) {
    for(bool point:{false,true}) for(bool glass:{false,true}) for(bool winding:{false,true}) for(int mode:{0,1,2}) {
        if(signedEmitter&&mode!=0) continue;
        if(signalKind&&glass) continue;
        std::string text=Materials();
        text.replace(text.find("values 1.3 1.5 1.9"),std::string("values 1.3 1.5 1.9").size(),"values 1.5 1.5 1.5");
        text+=(signalKind?QuadMesh("shape",-1,-2,3,winding):Mesh(false,winding))+SceneObject("caster","shape",glass?"glass":"mirror"," position 0 0 1\n")
            +"lambertian_material\n{\n name diffuse\n reflectance white\n}\n"
            +PlaneScene("receiver_geo",-2)+SceneObject("receiver","receiver_geo","diffuse")
            +(point?"omni_light\n{\n name source\n position 0 0 -1\n color 1 1 1\n power 40\n}\n":
              "spot_light\n{\n name source\n position 0 0 -1\n target 0 0 1\n color 1 1 1\n power 40\n inner 10\n outer 30\n}\n");
        if(signalKind) {
            const std::string signal=signalKind==1?"proximity(4)":"interior(1)";
            const std::string painter="expression_painter\n{\n name signal_tint\n expr vec3(.2+.8*"+signal+",.2+.8*"+signal+",.2+.8*"+signal+")\n}\n";
            text.insert(text.find("perfectreflector_material"),painter);
            text+="sphere_geometry\n{\n name neighbour_geo\n radius "+std::string(signalKind==1?".2":"10")+"\n}\n"
                +SceneObject("neighbour","neighbour_geo","diffuse",signalKind==1?" position 2 0 0\n":" position 0 0 0\n");
            const std::string original="name mirror\n reflectance white";
            text.replace(text.find(original),original.size(),"name mirror\n reflectance signal_tint");
        }
        if(signedEmitter) text.replace(text.rfind("color 1 1 1"),std::string("color 1 1 1").size(),"color 1 -0.5 0.2");
        Fixture fixture(text);
        if(!fixture.job||!fixture.Object("receiver")) {Check(false,"delta fixture prepared");continue;}
        std::vector<IShaderOp*> ops; IShader* shader=nullptr;
        Check(RISE_API_CreateStandardShader(&shader,ops),"delta query shader created");
        if(!shader) continue;
        auto* caster=new RayCaster(false,16,*shader,true);caster->AttachScene(&fixture.Scene());
        ManifoldSolverConfig cfg;cfg.enabled=true;cfg.extendedMode=true;cfg.targetBounces=1;
        cfg.biased=true;cfg.multiTrials=1;cfg.maxBernoulliTrials=64;
        cfg.seedingMode=uniform?ManifoldSolverConfig::eSeedingUniform:ManifoldSolverConfig::eSeedingSnell;
#ifdef RISE_SMS_REFERENCE_A
        SMSReferenceCounters counters;SMSDomainCounters domainCounters;
        cfg.referenceCounters=&counters;cfg.domainCounters=&domainCounters;
#endif
        auto* solver=new ManifoldSolver(cfg);
        std::vector<const IObject*> casters;ManifoldSolver::EnumerateSpecularCasters(fixture.Scene(),casters);
        solver->SetSpecularCasters(casters);
        StabilityConfig stability;stability.rrMinDepth=20;
        // Match the analytic single receiver bounce; repeated floor/mirror
        // interreflection is outside this reference, not estimator bias.
        auto* integrator=new PathTracingIntegrator(cfg,stability);integrator->SetMaxPathDepth(1);
        integrator->GetSolver()->SetSpecularCasters(casters);
        IORStack air(1);
        RayIntersection hit(Ray(Point3(0,0,-1.9),Vector3(0,0,-1)),nullRasterizerState);
        fixture.Object("receiver")->IntersectRay(hit,RISE_INFINITY,true,true,false);
        Check(hit.geometric.bHit,"delta receiver context is an actual intersection");
        const Scalar nm=mode==1?450:650;
        const auto* light=fixture.Scene().GetLights()->GetItem("source");
        const Vector3 direction(0,0,1);
        const auto* bsdf=hit.pMaterial->GetBSDF();
        const RISEPel f=bsdf->valueStateful(direction,hit.geometric,&air);
        const Scalar fnm=bsdf->valueStatefulNM(direction,hit.geometric,nm,&air);
        const RISEPel le=light->emittedRadiance(direction);
        const Scalar lenm=light->emittedRadianceNM(direction,nm);
        if(signedEmitter) Check(le[1]<0,"native light preserves authored signed green emission");
        RayIntersection mirrorHit(Ray(Point3(0,0,-1),direction),nullRasterizerState);
        if(signalKind) fixture.Scene().GetObjects()->IntersectRay(mirrorHit,true,true,false);
        else fixture.Object("caster")->IntersectRay(mirrorHit,RISE_INFINITY,true,true,false);
        Check(mirrorHit.geometric.bHit,"analytic caster context is an independent native ray intersection");
        const RISEPel mirrorRGB=glass?RISEPel(1):mirrorHit.pMaterial->GetSpecularInfo(mirrorHit.geometric,air).attenuation;
        const Scalar mirrorNM=glass?1:mirrorHit.pMaterial->GetSpecularInfoNM(mirrorHit.geometric,air,nm).attenuationNM;
        const Scalar fresnel=glass?Optics::CalculateDielectricReflectanceCosine(1,1,1.5):1;
        std::array<std::vector<double>,3> samples;
        constexpr unsigned N=16384;
#ifdef RISE_SMS_SCRATCH_COUNTERS
        unsigned long long warmGrowths=0, warmFrames=0;
#endif
        for(unsigned salt=0;salt<4;++salt) {
            const unsigned renderSalt=SobolSequence::HashCombine(9100+salt,0x44454c54);
            SobolSamplerTestHooks::ValueSalt().store(renderSalt);
            std::array<Scalar,3> sums{}, corrections{};
            const auto add=[&](unsigned c,Scalar value) {
                // Keep each rounding step observable under make/Opto's
                // reassociation; otherwise fast-math erases compensation.
                const volatile Scalar adjusted=value-corrections[c];
                const volatile Scalar next=sums[c]+adjusted;
                const volatile Scalar recovered=next-sums[c];
                corrections[c]=recovered-adjusted;sums[c]=next;
            };
            for(unsigned sample=0;sample<N;++sample) {
                SobolSampler sampler(sample,29);
                if(production) {
                    RandomNumberGenerator random(renderSalt+sample);
                    RuntimeContext context(random,RuntimeContext::PASS_NORMAL,false);context.pSampler=&sampler;
                    const Ray camera(Point3(0,0,-1.9),Vector3(0,0,-1));
                    if(mode==0) {
                        const auto value=integrator->IntegrateRay(context,nullRasterizerState,camera,
                            fixture.Scene(),*caster,sampler,nullptr,nullptr);
                        for(unsigned c=0;c<3;++c) add(c,value[c]);
                    } else add(0,integrator->IntegrateRayNM(context,nullRasterizerState,camera,nm,
                        fixture.Scene(),*caster,sampler,nullptr,nullptr));
                } else if(mode==0) {
                    const auto value=solver->EvaluateAtShadingPoint(hit.geometric.ptIntersection,
                        hit.geometric.UnflippedGeomNormal(),hit.geometric.vNormal,hit.geometric.onb,
                        hit.pMaterial,direction,fixture.Scene(),*caster,sampler,&air);
                    for(unsigned c=0;c<3;++c) add(c,value.contribution[c]);
                } else {
                    const auto value=solver->EvaluateAtShadingPointNM(hit.geometric.ptIntersection,
                        hit.geometric.UnflippedGeomNormal(),hit.geometric.vNormal,hit.geometric.onb,
                        hit.pMaterial,direction,fixture.Scene(),*caster,sampler,nm,&air);
                    add(0,value.contribution);
                }
            }
            for(unsigned c=0;c<(mode==0?3u:1u);++c) samples[c].push_back(static_cast<double>(sums[c]/N));
#ifdef RISE_SMS_SCRATCH_COUNTERS
            if(salt==0) {
                warmGrowths=counters.scratchBufferGrowths.load();
                warmFrames=counters.scratchFrames.load();
            } else {
                Check(counters.scratchBufferGrowths.load()==warmGrowths,
                    "production trial scratch buffers stop growing after warm-up");
                Check(counters.scratchFrames.load()==warmFrames,
                    "production trial nesting reuses warmed worker frames");
            }
#endif
        }
        for(unsigned c=0;c<(mode==0?3u:1u);++c) {
            const Scalar imageDistance=1-hit.geometric.ptIntersection.z;
            const Scalar expected=fresnel*(mode==0?f[c]*le[c]*mirrorRGB[c]:fnm*lenm*mirrorNM)/(imageDistance*imageDistance)
                +(production&&point?(mode==0?f[c]*le[c]:fnm*lenm)/std::pow(-1-hit.geometric.ptIntersection.z,2):0);
            const Moments m(samples[c]);
            std::cout<<std::setprecision(17)<<"delta uniform="<<uniform<<" signed="<<signedEmitter<<" signal="<<signalKind<<" production="<<production<<" point="<<point<<" glass="<<glass<<" winding="<<winding<<" mode="<<mode<<" c="<<c
                <<" mean="<<m.mean<<" sd="<<m.sd<<" n=4 N="<<N<<" analytic="<<expected<<" error="<<m.mean-expected<<" mirrorNM="<<mirrorNM<<" reference sd=0\n";
            Check(std::isfinite(m.mean)&&std::fabs(m.mean)>0,"point/spot reference activation is nonzero and finite");
            Check(std::fabs(m.mean-expected)<=3*m.sd+64*std::numeric_limits<Scalar>::epsilon()*std::fabs(expected),
                "native-domain upward spot agrees with analytic virtual image within 3 sd and floating-point roundoff");
        }
#ifdef RISE_SMS_REFERENCE_A
        std::cout<<"delta counters proposals="<<counters.proposalTrials<<" zeros="<<counters.zeroTrials
            <<" newton="<<domainCounters.newtonIterations<<" retries="<<counters.retryTrials
            <<" tails="<<counters.tailTrials<<" roulette="<<counters.rouletteStops
            <<" owned="<<counters.ownedRoots<<" rejected="<<counters.rejectedRoots
#ifdef RISE_SMS_SCRATCH_COUNTERS
            <<" sceneQueries="<<counters.sceneIntersectionQueries
            <<" objectQueries="<<counters.objectIntersectionQueries
            <<" domainMaterialQueries="<<counters.materialQueries
            <<" scratchGrowths="<<counters.scratchBufferGrowths
            <<" scratchFrames="<<counters.scratchFrames
            <<" scratchPeakBytes="<<counters.scratchPeakBytes
#endif
            <<'\n';
#ifdef RISE_SMS_SCRATCH_COUNTERS
        Check(counters.sceneIntersectionQueries>0 && counters.objectIntersectionQueries>0
            && counters.materialQueries>0,"production extended trials record actual scene, object, and domain material queries");
        Check(counters.scratchPeakBytes>0,"production extended trials report retained worker scratch capacity");
        SMSReferenceCounters legacyCounters;
        ManifoldSolverConfig legacyConfig=cfg;legacyConfig.extendedMode=false;
        legacyConfig.referenceCounters=&legacyCounters;
        auto* legacySolver=new ManifoldSolver(legacyConfig);
        legacySolver->SetSpecularCasters(casters);
        SobolSampler legacySampler(31,29);
        if(mode==0) legacySolver->EvaluateAtShadingPoint(hit.geometric.ptIntersection,
            hit.geometric.UnflippedGeomNormal(),hit.geometric.vNormal,hit.geometric.onb,
            hit.pMaterial,direction,fixture.Scene(),*caster,legacySampler,&air);
        else legacySolver->EvaluateAtShadingPointNM(hit.geometric.ptIntersection,
            hit.geometric.UnflippedGeomNormal(),hit.geometric.vNormal,hit.geometric.onb,
            hit.pMaterial,direction,fixture.Scene(),*caster,legacySampler,nm,&air);
        Check(legacyCounters.scratchBufferGrowths==0 && legacyCounters.scratchFrames==0
            && legacyCounters.scratchPeakBytes==0,"legacy evaluation does not lease extended worker scratch");
        legacySolver->release();
#else
        Check(false,"production extended trials provide measured scratch and query diagnostics");
#endif
#endif
        SobolSamplerTestHooks::ValueSalt().store(0);
        integrator->release();solver->release();caster->release();shader->release();
    }
}
// Its values happen to be one, but its declaration cannot certify the
// position-independent IOR contract. Rejection must preserve PT's clear hit.
class UncertifiedIndex final : public UniformScalarPainter {
public:
    UncertifiedIndex() : UniformScalarPainter(1) {}
    bool IsPositionIndependent() const override { return false; }
};
// Legacy metadata extensions can advertise clear transmission without
// implementing a native extended-domain provider or a transmission hint.
class LegacyMetadataOnly final : public IMaterial, public Reference {
    const IMaterial& native;
protected:
    ~LegacyMetadataOnly() override { native.release(); }
public:
    explicit LegacyMetadataOnly(const IMaterial& m) : native(m) { native.addref(); }
    IBSDF* GetBSDF() const override { return nullptr; }
    ISPF* GetSPF() const override { return nullptr; }
    IEmitter* GetEmitter() const override { return nullptr; }
    SpecularInfo GetSpecularInfo(const RayIntersectionGeometric& ri,const IORStack& stack) const override {
        return native.GetSpecularInfo(ri,stack);
    }
    SpecularInfo GetSpecularInfoNM(const RayIntersectionGeometric& ri,const IORStack& stack,Scalar nm) const override {
        return native.GetSpecularInfoNM(ri,stack,nm);
    }
};
class UncertifiedFrameModifier final : public IRayIntersectionModifier, public Reference {
public:
    void Modify(RayIntersectionGeometric&) const override {}
};
static void UnsupportedCasterSwitches() {
    for(unsigned kind:{0u,1u,2u,3u,4u,5u}) for(bool reverse:{false,true}) for(bool remote:{false,true}) {
        const bool csg=kind==1;
        Fixture f(Materials()+Mesh(csg,reverse)+SceneObject("pane","shape","glass",csg?" scale 1 1 0.25\n":" position 0 0 1\n")
            +(csg?"sphere_geometry\n{\n name tiny\n radius 0.1\n}\n"+SceneObject("other","tiny","glass"," position 4 0 0\n")
                +"csg_object\n{\n name inherited\n obja pane\n objb other\n operation union\n}\n":"")
            +(kind==2?"weave_material\n{\n name gap\n fabric custom\n transmission thin\n gap 1\n}\n"+PlaneScene("gap_geo",-.5)+SceneObject("gap","gap_geo","gap"):"")
            +"lambertian_material\n{\n name diffuse\n reflectance white\n}\n"
            +PlaneScene("floor",-2)+SceneObject("receiver","floor","diffuse")
            +"omni_light\n{\n name source\n position 0 0 1\n color 1 1 1\n power 40\n}\n"
            +(remote?SceneObject("remote_mirror","shape","mirror"," position 1000 0 1\n"):""));
        IScalarPainter* index=kind?static_cast<IScalarPainter*>(new UniformScalarPainter(1)):new UncertifiedIndex();auto* white=new UniformColorPainter(RISEPel(1));IMaterial* material=nullptr;
        Check(RISE_API_CreatePerfectRefractorMaterial(&material,*white,*index),"unsupported native refractor created");
        if(material) {
            IMaterial* assigned=kind==3 ? static_cast<IMaterial*>(new LegacyMetadataOnly(*material)) : material;
            f.job->GetObjects()->GetItem("pane")->AssignMaterial(*assigned);
            if(assigned!=material) assigned->release();
        }
        safe_release(material);index->release();white->release();
        std::vector<IShaderOp*> ops;IShader* shader=nullptr;
        Check(RISE_API_CreateStandardShader(&shader,ops),"unsupported caster shader created");
        if(!shader) continue;
        auto* caster=new RayCaster(false,16,*shader,true);caster->SetTransparentShadows(true);
        f.Scene().GetObjects()->PrepareForRendering();caster->AttachScene(&f.Scene());
        if(kind==4) {
            auto* modifier=new UncertifiedFrameModifier;
            f.job->GetObjects()->GetItem("pane")->AssignModifier(*modifier);modifier->release();
        }
        if(kind==5) {
#ifdef RISE_SMS_REFERENCE_A
            auto* modifier=new AnalyticUVNormal;
            f.job->GetObjects()->GetItem("pane")->AssignModifier(*modifier);modifier->release();
            auto* mapping=new HarmonicGeneratedUV;
            f.job->GetObjects()->GetItem("pane")->SetUVGenerator(*mapping);mapping->release();
#else
            Check(false,"composed UV rejection helper unavailable on committed baseline");
#endif
        }
        ManifoldSolverConfig config;config.enabled=true;config.extendedMode=true;config.targetBounces=1;
        config.multiTrials=1;StabilityConfig stability;stability.rrMinDepth=20;
        auto* on=new PathTracingIntegrator(config,stability);on->SetMaxPathDepth(8);
        ManifoldSolverConfig plain;auto* off=new PathTracingIntegrator(plain,stability);off->SetMaxPathDepth(8);
        IORStack air(1);
        Check(!on->GetSolver()->ExtendedAnchorEligible(f.Scene(),*caster,Point3(0,0,-2),air),
            "unsupported transmissive caster rejects the anchor, including inherited CSG and a mixed supported caster set");
        for(Scalar nm:{0.,450.,650.}) {
        Check(!on->GetSolver()->ExtendedAnchorEligible(f.Scene(),*caster,Point3(0,0,-2),air,nm),
            "unsupported caster disables the complete anchor in RGB and NM");
        if(kind>=4) {
#ifdef RISE_SMS_REFERENCE_A
            const auto domain=nm==0?SMSQueryDomain::RGB(0):SMSQueryDomain::NM(nm);
            RandomNumberGenerator random(76);IndependentSampler sampler(random);
            const Point3 start(0,0,-2),end(0,0,2);
            const auto root=on->GetSolver()->ProposeExtendedRoot(start,Vector3(0,0,1),end,f.Scene(),air,domain,sampler);
            Check(!root.accepted,"post-preparation unaudited modifier or composed UV dependency rejects a direct proposal");
            RayIntersection hit(Ray(start,Vector3(0,0,1)),nullRasterizerState);
            f.Object("pane")->IntersectRay(hit,RISE_INFINITY,true,true,false);
            Check(hit.geometric.bHit,"unaudited direct solve uses an actual native pane hit");
            SMSDomainVertex vertex(hit.geometric);vertex.geometry.pObject=hit.pObject;vertex.geometry.pMaterial=hit.pMaterial;
            std::vector<SMSDomainVertex> records{vertex};
            const auto solved=on->GetSolver()->SolveDomain(start,Vector3(0,0,1),end,Vector3(0,0,-1),f.Scene(),air,domain,records,sampler,1e-7);
            Check(!solved.valid,"post-preparation unaudited input rejects the direct domain solve");
#else
            Check(false,"direct proposal rejection helper unavailable on committed baseline");
#endif
        }
        for(unsigned trial=0;trial<4;++trial) {
            const unsigned salt=SobolSequence::HashCombine(12000+trial,0x554e4345);
            SobolSamplerTestHooks::ValueSalt().store(salt);
            std::array<RISEPel,2> values;
            for(unsigned mode=0;mode<2;++mode) {
                RandomNumberGenerator random(salt);SobolSampler sampler(0,7);
                RuntimeContext context(random,RuntimeContext::PASS_NORMAL,false);context.pSampler=&sampler;
                const Ray ray(Point3(0,0,-1.9),Vector3(0,0,-1));
                if(nm==0) values[mode]=(mode?on:off)->IntegrateRay(context,nullRasterizerState,ray,f.Scene(),*caster,sampler,nullptr,nullptr);
                else values[mode]=RISEPel((mode?on:off)->IntegrateRayNM(context,nullRasterizerState,ray,nm,f.Scene(),*caster,sampler,nullptr,nullptr));
            }
            Check(std::isfinite(values[0].r)&&values[0].r>0,"PT clear-transmission control is finite and lit");
            Check(std::memcmp(&values[0],&values[1],sizeof(RISEPel))==0,
                "ineligible extended anchor preserves PT light with all three switches off");
            std::cout<<"unsupported caster kind="<<kind<<" winding="<<reverse<<" remote="<<remote<<" salt="<<salt<<" PT="<<values[0].r<<" extended="<<values[1].r<<'\n';
        }
        }
        SobolSamplerTestHooks::ValueSalt().store(0);
        on->release();off->release();caster->release();shader->release();
    }
}
static void ImpossibleSolverSwitches() {
    for(bool reverse:{false,true}) for(unsigned invalid:{0u,1u,2u,3u}) {
        std::string text=Materials();
        text.replace(text.find("values 1.3 1.5 1.9"),std::string("values 1.3 1.5 1.9").size(),"values 1 1 1");
        Fixture f(text+Mesh(false,reverse)+SceneObject("pane","shape","glass"," position 0 0 1\n")
            +"lambertian_material\n{\n name diffuse\n reflectance white\n}\n"
            +PlaneScene("floor",-2)+SceneObject("receiver","floor","diffuse")
            +"omni_light\n{\n name source\n position 0 0 1\n color 1 1 1\n power 40\n}\n");
        std::vector<IShaderOp*> ops;IShader* shader=nullptr;
        Check(RISE_API_CreateStandardShader(&shader,ops),"solver configuration control shader created");
        if(!shader) continue;
        auto* caster=new RayCaster(false,16,*shader,true);caster->SetTransparentShadows(true);caster->AttachScene(&f.Scene());
        ManifoldSolverConfig cfg;cfg.enabled=true;cfg.extendedMode=true;
        cfg.maxChainDepth=invalid==0?0:1;cfg.targetBounces=invalid==1?2:1;
        if(invalid>=2) cfg.solverThreshold=invalid==2?Scalar(0):Scalar(-1);
        StabilityConfig stability;stability.rrMinDepth=20;
        auto* on=new PathTracingIntegrator(cfg,stability);on->SetMaxPathDepth(8);
        ManifoldSolverConfig plain;auto* off=new PathTracingIntegrator(plain,stability);off->SetMaxPathDepth(8);IORStack air(1);
        Check(on->GetSolver()->ExtendedModeActive(f.Scene()),"solver configuration control is composite-free extended mode");
        for(Scalar nm:{0.,450.,650.}) {
            Check(!on->GetSolver()->ExtendedAnchorEligible(f.Scene(),*caster,Point3(0,0,-2),air,nm),
                "impossible depth or nonpositive threshold disables the complete anchor in RGB and NM");
            for(unsigned trial=0;trial<4;++trial) {
                const unsigned salt=SobolSequence::HashCombine(18000+trial,0x44455054);SobolSamplerTestHooks::ValueSalt().store(salt);
                std::array<RISEPel,2> values;
                for(unsigned mode=0;mode<2;++mode) {
                    RandomNumberGenerator random(salt);SobolSampler sampler(0,7);
                    RuntimeContext context(random,RuntimeContext::PASS_NORMAL,false);context.pSampler=&sampler;
                    const Ray ray(Point3(0,0,-1.9),Vector3(0,0,-1));
                    if(nm==0) values[mode]=(mode?on:off)->IntegrateRay(context,nullRasterizerState,ray,f.Scene(),*caster,sampler,nullptr,nullptr);
                    else values[mode]=RISEPel((mode?on:off)->IntegrateRayNM(context,nullRasterizerState,ray,nm,f.Scene(),*caster,sampler,nullptr,nullptr));
                }
                Check(std::isfinite(values[0].r)&&values[0].r>0,"impossible solver configuration plain PT control remains lit");
                Check(std::memcmp(&values[0],&values[1],sizeof(RISEPel))==0,
                    "impossible solver configuration keeps PT light bit-identically with all switches off");
                std::cout<<"depth winding="<<reverse<<" max="<<cfg.maxChainDepth<<" target="<<cfg.targetBounces
                    <<" threshold="<<cfg.solverThreshold<<" nm="<<nm<<" salt="<<salt<<" PT="<<values[0].r<<" extended="<<values[1].r<<'\n';
            }
        }
        SobolSamplerTestHooks::ValueSalt().store(0);on->release();off->release();caster->release();shader->release();
    }
}
// An extension can forward the actual native composite walker without
// inheriting CompositeMaterial or any of the built-in wrapper classes.
class ForwardingMaterial final : public IMaterial, public Reference {
    const IMaterial& base;
protected:
    ~ForwardingMaterial() override { base.release(); }
public:
    explicit ForwardingMaterial(const IMaterial& m) : base(m) { base.addref(); }
    IBSDF* GetBSDF() const override { return base.GetBSDF(); }
    ISPF* GetSPF() const override { return base.GetSPF(); }
    IEmitter* GetEmitter() const override { return base.GetEmitter(); }
};
static void CompositeProxyPolicy() {
    for(bool reverse:{false,true}) {
        Fixture fixture(Materials()+Mesh(true,reverse)
            +"composite_material\n{\n name layers\n top glass\n bottom glass\n}\n"
            +SceneObject("remote_composite","shape","layers"," position 1000 0 0\n")
            +PlaneScene("mirror_plane",0)+SceneObject("mirror","mirror_plane","mirror")
            +"lambertian_material\n{\n name diffuse\n reflectance white\n}\n"
            +PlaneScene("floor",-2)+SceneObject("receiver","floor","diffuse")
            +"omni_light\n{\n name source\n position 0 0 -1\n color 1 1 1\n power 40\n}\n");
        auto* object=fixture.job->GetObjects()->GetItem("remote_composite");
        auto* proxy=new ForwardingMaterial(*object->GetMaterial());object->AssignMaterial(*proxy);proxy->release();
        fixture.Scene().GetObjects()->PrepareForRendering();
        std::vector<IShaderOp*> ops;IShader* shader=nullptr;
        Check(RISE_API_CreateStandardShader(&shader,ops),"composite proxy shader created");
        if(!shader) continue;
        auto* caster=new RayCaster(false,16,*shader,true);caster->SetTransparentShadows(true);caster->AttachScene(&fixture.Scene());
        ManifoldSolverConfig config;config.enabled=true;config.extendedMode=true;config.targetBounces=1;
        StabilityConfig stability;stability.rrMinDepth=20;
        auto* on=new PathTracingIntegrator(config,stability);on->SetMaxPathDepth(1);
        config.extendedMode=false;auto* off=new PathTracingIntegrator(config,stability);off->SetMaxPathDepth(1);
        Check(!on->GetSolver()->ExtendedModeActive(fixture.Scene()),"forwarded native composite SPF selects scene-wide legacy mode");
        const auto* objects=dynamic_cast<const ObjectManager*>(fixture.Scene().GetObjects());
        Check(objects&&objects->FirstCompositeObject()=="remote_composite","forwarded composite policy names its scene object");
        for(unsigned trial=0;trial<4;++trial) {
            const unsigned salt=SobolSequence::HashCombine(13000+trial,0x43505258);SobolSamplerTestHooks::ValueSalt().store(salt);
            std::array<RISEPel,2> values;
            for(unsigned mode=0;mode<2;++mode) {
                RandomNumberGenerator random(salt);SobolSampler sampler(0,7);
                RuntimeContext context(random,RuntimeContext::PASS_NORMAL,false);context.pSampler=&sampler;
                values[mode]=(mode?on:off)->IntegrateRay(context,nullRasterizerState,
                    Ray(Point3(0,0,-1.9),Vector3(0,0,-1)),fixture.Scene(),*caster,sampler,nullptr,nullptr);
            }
            Check(std::isfinite(values[0].r)&&values[0].r>0,"composite proxy legacy control is finite and lit");
            Check(std::memcmp(&values[0],&values[1],sizeof(RISEPel))==0,"composite proxy preserves legacy output bit-identically");
        }
        SobolSamplerTestHooks::ValueSalt().store(0);on->release();off->release();caster->release();shader->release();
    }
}
static std::string SlabScene(bool reverse, unsigned inner, unsigned outer) {
    std::string text=Materials();
    text.replace(text.find("values 1.3 1.5 1.9"),std::string("values 1.3 1.5 1.9").size(),"values 1.5 1.5 1.5");
    text+="standard_shader\n{\n name global\n shaderop DefaultPathTracing\n}\n"
        "film\n{\n width 16\n height 16\n}\n"
        "pinhole_camera\n{\n location 0 0 -1.9\n lookat 0 0 -2\n up 0 1 0\n fov 174.275189547777\n}\n"
        "lambertian_material\n{\n name diffuse\n reflectance white\n}\n";
    text+=Mesh(true,reverse)+SceneObject("slab","shape","glass"," scale 3 3 0.25\n")
        +PlaneScene("floor",-2,-4,4)+SceneObject("receiver","floor","diffuse")
        +"spot_light\n{\n name source\n position 0.5 0 2\n target 0.5 0 -2\n color 1 1 1\n power 40\n inner "+std::to_string(inner)
        +"\n outer "+std::to_string(outer)+"\n}\n";
    return text;
}
static std::vector<RISEColor> SlabExtendedRender(const std::string& text, unsigned salt) {
    Fixture fixture(text);
    std::vector<IShaderOp*> ops;IShader* shader=nullptr;
    if(!RISE_API_CreateStandardShader(&shader,ops)) return {};
    auto* caster=new RayCaster(false,16,*shader,true);caster->SetTransparentShadows(true);
    caster->AttachScene(&fixture.Scene());
    ManifoldSolverConfig cfg;cfg.enabled=true;cfg.extendedMode=true;cfg.targetBounces=2;
    cfg.maxBernoulliTrials=64;cfg.multiTrials=1;
#ifdef RISE_SMS_REFERENCE_A
    SMSReferenceCounters counters;SMSDomainCounters domainCounters;
    cfg.referenceCounters=&counters;cfg.domainCounters=&domainCounters;
#endif
    StabilityConfig stability;stability.rrMinDepth=20;
    auto* rasterizer=new PathTracingPelRasterizer(caster,cfg,PathGuidingConfig(),AdaptiveSamplingConfig(),stability,false);
    rasterizer->SetMaxPathDepth(1);rasterizer->SetInteractiveDenoiseSuppressed(true);
    ISampling2D* samples=nullptr;IPixelFilter* filter=nullptr;
    RISE_API_CreateMultiJitteredSampling2D(&samples,1,1);
    RISE_API_CreateBoxPixelFilter(&filter,1,1);
    if(!samples||!filter) {safe_release(samples);safe_release(filter);rasterizer->release();caster->release();shader->release();return {};}
    samples->SetNumSamples(4096);rasterizer->SubSampleRays(samples,filter);
    auto* capture=new CapturingRasterizerOutput();rasterizer->AddRasterizerOutput(capture);
    rasterizer->AttachToScene(&fixture.Scene());
    SobolSamplerTestHooks::ValueSalt().store(salt);
    rasterizer->RasterizeScene(fixture.Scene(),nullptr,nullptr);
    SobolSamplerTestHooks::ValueSalt().store(0);
    const auto pixels=capture->pixels;
#ifdef RISE_SMS_REFERENCE_A
    std::cout<<"slab counters salt="<<salt<<" proposals="<<counters.proposalTrials
        <<" zeros="<<counters.zeroTrials<<" newton="<<domainCounters.newtonIterations
        <<" retries="<<counters.retryTrials<<" tails="<<counters.tailTrials
        <<" roulette="<<counters.rouletteStops<<" owned="<<counters.ownedRoots
        <<" rejected="<<counters.rejectedRoots<<'\n';
#endif
    rasterizer->DetachFromScene(&fixture.Scene());
    capture->release();samples->release();filter->release();rasterizer->release();caster->release();shader->release();
    return pixels;
}
static void SlabRenders() {
    Check(ConfigureTestWorker(),"slab render uses one configured worker");
    for(bool reverse:{false,true}) for(const auto& cone:{std::array<unsigned,2>{30,45},std::array<unsigned,2>{44,45},std::array<unsigned,2>{80,85}}) {
        std::array<std::vector<double>,3> tested,reference;
        for(unsigned trial=0;trial<4;++trial) {
            const unsigned seed=46000+trial;const unsigned salt=SobolSequence::HashCombine(seed,kSaltTag);
            const auto text=SlabScene(reverse,cone[0],cone[1]);
            const auto a=SlabExtendedRender(text,salt);
            const auto referenceText=text+"bdpt_pel_rasterizer\n{\n samples 4096\n max_eye_depth 2\n max_light_depth 4\n rr_min_depth 20\n pixel_filter box\n oidn_denoise FALSE\n pathguiding FALSE\n adaptive_max_samples 0\n}\n";
            g_seedBase=seed;g_renderIndex=0;const auto b=Render(referenceText,"extended_slab_bdpt");
            Check(a.size()==256&&b.ok&&b.pixels.size()==256,"extended and matching native BDPT slab renders complete");
            for(unsigned c=0;c<3;++c) {
                double av=0,bv=0;
                for(const auto& pixel:a) av+=pixel.base[c];
                for(const auto& pixel:b.pixels) bv+=pixel.base[c];
                std::cout<<std::setprecision(17)<<"slab trial="<<trial<<" winding="<<reverse<<" inner="<<cone[0]<<" c="<<c<<" extended="<<av/256<<" BDPT="<<bv/256<<std::endl;
                tested[c].push_back(av/256);reference[c].push_back(bv/256);
            }
        }
        for(unsigned c=0;c<3;++c) {
            const Moments a(tested[c]),b(reference[c]);
            std::cout<<std::setprecision(17)<<"slab winding="<<reverse<<" inner="<<cone[0]<<" outer="<<cone[1]<<" c="<<c
                <<" extended="<<a.mean<<" sd="<<a.sd<<" BDPT="<<b.mean<<" reference sd="<<b.sd<<" n=4 spp=4096\n";
            Check(a.mean>0&&b.mean>0&&std::isfinite(a.mean)&&std::isfinite(b.mean),"slab native-domain controls are positive and finite");
            const bool withinBand=std::fabs(a.mean-b.mean)<=3*std::hypot(a.sd,b.sd);
            // The user accepted the measured wide-cone discrepancy as variance
            // and authorized continuation. Keep its original band visible;
            // this fixture remains a measurement, not a claimed 3-sd pass.
            if(cone[0]==80) std::cout<<"slab wide-cone measurement within_3_sd="<<withinBand
                <<" user accepted variance; no accuracy gate or DL-420 closure claimed"<<std::endl;
            else Check(withinBand,"extended spot slab agrees with matching BDPT within 3 combined sd");
        }
    }
}

int main(int argc,char** argv) {
#ifdef RISE_SMS_REFERENCE_A
    std::cout<<"SMS record bytes vertex="<<sizeof(ManifoldVertex)
        <<" domainVertex="<<sizeof(SMSDomainVertex)<<" root="<<sizeof(SMSDomainRoot)
        <<" referenceCounters="<<sizeof(SMSReferenceCounters)<<'\n';
#endif
    bool synthetic=true,geometry=true,delta=true,unsupported=true,production=false,slab=true;
    Check(ConfigureTestWorker(),"reference renderer uses one configured worker");
    if(argc==2&&std::string(argv[1])=="--synthetic-only") {geometry=false;delta=false;unsupported=false;slab=false;}
    if(argc==2&&std::string(argv[1])=="--geometry-only") {synthetic=false;delta=false;unsupported=false;slab=false;}
    if(argc==2&&std::string(argv[1])=="--delta-only") {synthetic=false;geometry=false;unsupported=false;slab=false;}
    if(argc==2&&std::string(argv[1])=="--unsupported-only") {synthetic=false;geometry=false;delta=false;slab=false;}
    if(argc==2&&std::string(argv[1])=="--production-only") {synthetic=false;geometry=false;delta=true;unsupported=false;production=true;slab=false;}
    if(argc==2&&std::string(argv[1])=="--slab-only") {synthetic=false;geometry=false;delta=false;unsupported=false;}
    if(argc==2&&std::string(argv[1])=="--modifier-chart-only") {
#ifdef RISE_SMS_REFERENCE_A
        PostModifierChartRoots();
#else
        std::cout<<"Estimator A modifier helper unavailable on committed baseline.\n";
#endif
        std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
    }
    if(argc==2&&std::string(argv[1])=="--r5-provenance-only") {
#ifdef RISE_SMS_REFERENCE_A
        NativeTriangleProvenance();
#endif
        std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
    }
    if(argc==2&&std::string(argv[1])=="--r5-preparation-only") {
#ifdef RISE_SMS_REFERENCE_A
        NativePreparationAudits();
#endif
        std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
    }
    if(argc==2&&std::string(argv[1])=="--r5-normal-only") {
#ifdef RISE_SMS_REFERENCE_A
        NativeNearCommensurateNormal();
#else
        std::cout<<"Round 5 normal helper unavailable on committed baseline.\n";
#endif
        std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
    }
    if(argc==2&&std::string(argv[1])=="--r6-generated-uv-only") {
#ifdef RISE_SMS_REFERENCE_A
        for(unsigned mode:{1u,2u,3u}) NativeNearCommensurateNormal(mode);
        NativeTransformedUVComposition();
#else
        Check(false,"generated UV differential witness requires native domain support");
#endif
        std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
    }
    if(argc==2&&std::string(argv[1])=="--r7-signal-production-only") {
        DeltaLights(true,false,false,1);DeltaLights(true,false,false,2);
        std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
    }
    if(argc==2&&std::string(argv[1])=="--r7-curved-uv-only") {
#ifdef RISE_SMS_REFERENCE_A
        NativeCurvedUVComposition();
#else
        Check(false,"curved UV differential witness requires native domain support");
#endif
        std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
    }
    if(argc==2&&std::string(argv[1])=="--r7-signals-only") {
#ifdef RISE_SMS_REFERENCE_A
        NativeCrossObjectPrices();NativeCrossObjectPrices(true);
#else
        Check(false,"signal price witness requires native domain support");
#endif
        std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
    }
    if(argc==2&&std::string(argv[1])=="--r7-uv-root-only") {
#ifdef RISE_SMS_REFERENCE_A
        RoundFourNumerics(true);
#else
        Check(false,"UV physical-root regression requires native domain support");
#endif
        std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
    }
    if(argc==2&&std::string(argv[1])=="--r6-weld-range-only") {
#ifdef RISE_SMS_REFERENCE_A
        NativeWeldCoordinateRange();
#endif
        std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
    }
    if(argc==2&&std::string(argv[1])=="--r8-sss-clamp-only") {
#ifdef RISE_SMS_REFERENCE_A
        NativeSSSReferenceClamps();
#endif
        std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
    }
    if(argc==2&&std::string(argv[1])=="--r8-scratch-depth-only") {
#if defined(RISE_SMS_REFERENCE_A) && defined(RISE_SMS_SCRATCH_COUNTERS)
        NativeFactorizationReservation();
#else
        Check(false,"factorization reservation regression requires native scratch diagnostics");
#endif
        std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
    }
    if(argc==2&&std::string(argv[1])=="--r8-polished-normal-only") {
#ifdef RISE_SMS_REFERENCE_A
        NativeScaledPolishedNormals();
#else
        Check(false,"polished normal regression requires native domain support");
#endif
        std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
    }
    if(argc==2&&std::string(argv[1])=="--r5-only") {
#ifdef RISE_SMS_REFERENCE_A
        NativeNearCommensurateNormal();NativeStackedPatchFrames();NativeHorizonFallbacks(1);NativeHorizonFallbacks(2);
#else
        std::cout<<"Round 5 helpers unavailable on committed baseline.\n";
#endif
        std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
    }
    if(argc==2&&std::string(argv[1])=="--r4-only") {
#ifdef RISE_SMS_REFERENCE_A
        RoundFourNumerics();RoundFourMaterials();NativeNearCommensurateNormal();
#else
        std::cout<<"Round 4 helpers unavailable on committed baseline.\n";
#endif
        std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
    }
    if(argc==2&&std::string(argv[1])=="--review-only") {
#ifdef RISE_SMS_REFERENCE_A
        RoundFourNumerics();RoundFourMaterials();NativeNearCommensurateNormal();
        NativePeriodicRoots();
        InteriorAtlasRoots();
        GeneratedChartRoots();
        PostModifierChartRoots();
        NativeOverlapExits();
        ModifiedWalkJacobians();
        NativeHorizonFallbacks();
        NativeClosedHorizonFallbacks();
#else
        std::cout<<"Estimator A seam helper unavailable on committed baseline.\n";
#endif
        ImpossibleSolverSwitches();
        std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
    }
    if(argc==2&&std::string(argv[1])=="--signed-only") {
        for(bool uniform:{false,true}) {DeltaLights(false,true,uniform);DeltaLights(true,true,uniform);}
        std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
    }
    std::cout<<std::setprecision(12);
#ifdef RISE_SMS_REFERENCE_A
    std::cout<<"reference bytes config="<<sizeof(ManifoldSolverConfig)<<" root="<<sizeof(SMSDomainRoot)
        <<" counters="<<sizeof(SMSReferenceCounters)<<" vertex="<<sizeof(SMSDomainVertex)
        <<" solver="<<sizeof(ManifoldSolver)<<std::endl;
    if(synthetic) Synthetic();if(geometry) {::Geometry();CloseRoots();WalkEvents();NativePeriodicRoots();}
#else
    if(synthetic||geometry) std::cout<<"Estimator A is absent on this committed baseline; new helper tests are unavailable, not a numerical red proof.\n";
#endif
    if(delta) DeltaLights(production);
    if(argc==1) for(bool uniform:{false,true}) {DeltaLights(false,true,uniform);DeltaLights(true,true,uniform);}
    if(unsupported) {UnsupportedCasterSwitches();CompositeProxyPolicy();ImpossibleSolverSwitches();}
    if(slab) SlabRenders();
    std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
}
