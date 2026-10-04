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
#include "../src/Library/Interfaces/IRayIntersectionModifier.h"
#include "../src/Library/Interfaces/ITriangleMeshGeometry.h"
#include "../src/Library/Interfaces/IGeometryManager.h"
#include "../src/Library/Utilities/Optics.h"
#include "../src/Library/Shaders/PathTracingIntegrator.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Rendering/PathTracingPelRasterizer.h"
#include "../src/Library/Utilities/PathGuidingField.h"
#include <array>
#include <sstream>
#include <iomanip>

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
static std::string Object(const std::string& name,const std::string& geometry,const std::string& material,
    const std::string& extra="") {
    return "standard_object\n{\n name "+name+"\n geometry "+geometry+"\n material "+material+"\n"+extra+"}\n";
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
static std::string CloseRootMesh(bool reverse, Scalar scale, Scalar separation=.001) {
    std::ostringstream s; s<<std::setprecision(17);
    s<<"indexedmesh_geometry\n{\n name shape\n double_sided TRUE\n face_normals TRUE\n";
    for(int side:{-1,1}) {
        const Scalar x=side*separation;
        const Point3 center(x,0,0), a(-.5,0,-3), b(.5,0,-3);
        const Vector3 n=Vector3Ops::Normalize(Vector3Ops::Normalize(Vector3Ops::mkVector3(a,center))
            +Vector3Ops::Normalize(Vector3Ops::mkVector3(b,center)));
        for(const Point2& p:{Point2(-.4*separation,-1),Point2(.4*separation,-1),Point2(.4*separation,2),Point2(-.4*separation,2)})
            s<<" vertex "<<scale*(x+p.x)<<' '<<scale*p.y<<' '<<scale*(-n.x*p.x/n.z)<<'\n';
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
        Fixture f(Materials()+CloseRootMesh(winding,scale,separation)+Object("caster","shape","mirror"));
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
    Fixture f(Materials()+(shape<4?geometry[shape]:UVSeamMesh(shape==5))+Object("caster","sphere","mirror"));
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
static void InteriorAtlasRoots() {
    for(bool reverse:{false,true}) for(int side:{-1,1}) for(bool varying:{false,true}) for(unsigned geometryKind:{0u,1u,2u}) {
        Fixture f(Materials()+UVSeamMesh(reverse,true)+Object("caster","sphere","mirror"));
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
class TiltNormal final : public IRayIntersectionModifier, public Reference {
public:
    explicit TiltNormal(int side, bool valid=false, bool varying=false) : side(side), valid(valid), varying(varying) {}
    void Modify(RayIntersectionGeometric& hit) const override {
        const Scalar angle=(valid?-10:60)*PI/180+(varying?.01*(hit.ptIntersection.x-5)+.025*hit.ray.Dir().x+.0001*hit.rast.x:0);
        hit.vNormal=Vector3(std::sin(angle),0,side*std::cos(angle));hit.onb.CreateFromW(hit.vNormal);
    }
private:
    int side;bool valid,varying;
};
static std::string QuadMesh(const std::string& name,Scalar z,Scalar x0,Scalar x1,bool reverse) {
    std::ostringstream s;s<<std::setprecision(17);
    s<<"indexedmesh_geometry\n{\n name "<<name<<"\n double_sided TRUE\n face_normals TRUE\n";
    s<<" vertex "<<x0<<" -2 "<<z<<"\n vertex "<<x1<<" -2 "<<z
        <<"\n vertex "<<x1<<" 2 "<<z<<"\n vertex "<<x0<<" 2 "<<z
        <<"\n uv 0 0\n uv 1 0\n uv 1 1\n uv 0 1\n";
    s<<(reverse?" triangle 0 2 1\n triangle 0 3 2\n":" triangle 0 1 2\n triangle 0 2 3\n");
    return s.str()+"}\n";
}
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
};
static void CheckNativeHorizonJacobian(const ManifoldSolverConfig& cfg,const ManifoldResult& result,
    const std::vector<SMSDomainVertex>& vertices,const Point3& start,const Point3& end,
    const IScene& scene,const IORStack& stack,SMSQueryDomain domain,ISampler& sampler) {
    NativeConstraintOracle oracle(cfg,&vertices);
    const auto& chain=result.specularChain;
    std::vector<Scalar> diagonal,upper,lower;
    oracle.BuildJacobian(chain,start,end,diagonal,upper,lower,true);
    const Scalar h=1e-4*Point3Ops::Distance(start,end);
    for(unsigned column=0;column<2;++column) {
        auto plus=chain,minus=chain;
        const Vector3 tangent=column?chain[0].dpdv:chain[0].dpdu;
        plus[0].position=Point3Ops::mkPoint3(plus[0].position,tangent*h);
        minus[0].position=Point3Ops::mkPoint3(minus[0].position,-tangent*h);
        std::vector<Scalar> a,b;
        oracle.EvaluateConstraint(plus,start,end,a);oracle.EvaluateConstraint(minus,start,end,b);
        for(unsigned row=0;row<2;++row) {
            const Scalar observed=(a[row]-b[row])/(2*h),expected=diagonal[2*row+column];
            Check(std::fabs(observed-expected)<1e-5*std::max(Scalar(1),std::fabs(expected)),
                "native shading/fallback constraint Jacobian agrees at an independent displacement scale");
        }
    }
    const Vector3 lightNormal=Vector3Ops::Normalize(Vector3Ops::mkVector3(end,chain[0].position));
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
static void NativeHorizonFallbacks() {
    for(bool reverse:{false,true}) for(int side:{-1,1}) for(const char* material:{"glass","mirror","coated","polished"})
        for(bool valid:{false,true}) for(bool varying:{false,true}) for(bool transformed:{false,true}) {
        const Scalar scale=transformed?1.5:1,offset=transformed?3:0;
        const std::string transform=transformed?" scale 1.5 1.5 1.5\n position 3 0 0\n":"";
        Fixture f(Materials()+
            "dielectric_material\n{\n name coated\n tau 1\n ior triple\n scattering 1000000\n ar_layer 1.224744871391589 112.26827987812466 0\n}\n"
            "uniformcolor_painter\n{\n name black\n color 0 0 0\n}\n"
            "polished_material\n{\n name polished\n reflectance black\n tau 0.7\n ior triple\n scattering 1000000\n}\n"
            +QuadMesh("patch",0,4.8,5.2,reverse)+Object("caster","patch",material,transform));
        auto* modifier=new TiltNormal(side,valid,varying);
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
static void NativeClosedHorizonFallbacks() {
    for(bool reverse:{false,true}) for(bool transformed:{false,true}) for(bool nested:{false,true}) for(bool valid:{false,true}) {
        const Scalar scale=transformed?1.5:1,offset=transformed?3:0;
        std::ostringstream transform;
        transform<<" scale "<<.2*scale<<' '<<2*scale<<' '<<.05*scale
            <<"\n position "<<offset+5*scale<<" 0 "<<-.05*scale<<"\n";
        std::ostringstream enclosure;
        if(nested) enclosure<<"perfectrefractor_material\n{\n name outer_mat\n refractance white\n ior 1.1\n}\n"
            <<"sphere_geometry\n{\n name outer_shape\n radius "<<10*scale<<"\n}\n"
            <<Object("outer","outer_shape","outer_mat"," position "+std::to_string(offset+5*scale)+" 0 0\n");
        Fixture f(Materials()+Mesh(true,reverse)+Object("caster","shape","glass",transform.str())+enclosure.str());
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
static void WalkEvents() {
    for(bool reverse:{false,true}) for(int side:{-1,1}) for(bool transformed:{false,true})
        for(unsigned count:{2u,3u}) {
        const Scalar scale=transformed?1.5:1,offset=transformed?3:0;
        const std::string transform=transformed?" scale 1.5 1.5 1.5\n position 3 0 0\n":"";
        Fixture f(Materials()+QuadMesh("first",0,-2,2,reverse)+QuadMesh("gate",-side,.3,2,reverse)
            +QuadMesh("last",-2*side,.6,4,reverse)+Object("a_first","first","mirror",transform)
            +Object("b_gate","gate","glass",transform)+Object("c_last","last","mirror",transform));
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
            +Mesh(true,reverse)+Object("a_inner","shape","dense",transform)
            +Object("z_outer","shape","glass",transformed?" scale 3 3 3\n position 3 0 0\n":" scale 2 2 2\n"));
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
            +Mesh(true,reverse)+Object("caster","shape","dense",transform));
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
            Fixture f(Materials()+Mesh(closed,winding)+Object("caster","shape","glass",
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
        Fixture f(Materials()+geometry+Object("caster","shape","glass"));
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
#endif
// Native analytic virtual-image reference for an upward spot reflected by
// one plane: f * F * Le / (anchor-to-plane + light-to-plane)^2.
static void DeltaLights(bool production=false, bool signedEmitter=false, bool uniform=false) {
    for(bool point:{false,true}) for(bool glass:{false,true}) for(bool winding:{false,true}) for(int mode:{0,1,2}) {
        if(signedEmitter&&mode!=0) continue;
        std::string text=Materials();
        text.replace(text.find("values 1.3 1.5 1.9"),std::string("values 1.3 1.5 1.9").size(),"values 1.5 1.5 1.5");
        text+=Mesh(false,winding)+Object("caster","shape",glass?"glass":"mirror"," position 0 0 1\n")
            +"lambertian_material\n{\n name diffuse\n reflectance white\n}\n"
            +PlaneScene("receiver_geo",-2)+Object("receiver","receiver_geo","diffuse")
            +(point?"omni_light\n{\n name source\n position 0 0 -1\n color 1 1 1\n power 40\n}\n":
              "spot_light\n{\n name source\n position 0 0 -1\n target 0 0 1\n color 1 1 1\n power 40\n inner 10\n outer 30\n}\n");
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
        fixture.Object("caster")->IntersectRay(mirrorHit,RISE_INFINITY,true,true,false);
        Check(mirrorHit.geometric.bHit,"analytic caster context is an independent native ray intersection");
        const Scalar mirrorNM=glass?1:mirrorHit.pMaterial->GetSpecularInfoNM(mirrorHit.geometric,air,nm).attenuationNM;
        const Scalar fresnel=glass?Optics::CalculateDielectricReflectanceCosine(1,1,1.5):1;
        std::array<std::vector<double>,3> samples;
        constexpr unsigned N=16384;
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
        }
        for(unsigned c=0;c<(mode==0?3u:1u);++c) {
            const Scalar imageDistance=1-hit.geometric.ptIntersection.z;
            const Scalar expected=fresnel*(mode==0?f[c]*le[c]:fnm*lenm*mirrorNM)/(imageDistance*imageDistance)
                +(production&&point?(mode==0?f[c]*le[c]:fnm*lenm)/std::pow(-1-hit.geometric.ptIntersection.z,2):0);
            const Moments m(samples[c]);
            std::cout<<std::setprecision(17)<<"delta uniform="<<uniform<<" signed="<<signedEmitter<<" production="<<production<<" point="<<point<<" glass="<<glass<<" winding="<<winding<<" mode="<<mode<<" c="<<c
                <<" mean="<<m.mean<<" sd="<<m.sd<<" n=4 N="<<N<<" analytic="<<expected<<" error="<<m.mean-expected<<" mirrorNM="<<mirrorNM<<" reference sd=0\n";
            Check(std::isfinite(m.mean)&&std::fabs(m.mean)>0,"point/spot reference activation is nonzero and finite");
            Check(std::fabs(m.mean-expected)<=3*m.sd+64*std::numeric_limits<Scalar>::epsilon()*std::fabs(expected),
                "native-domain upward spot agrees with analytic virtual image within 3 sd and floating-point roundoff");
        }
#ifdef RISE_SMS_REFERENCE_A
        std::cout<<"delta counters proposals="<<counters.proposalTrials<<" zeros="<<counters.zeroTrials
            <<" newton="<<domainCounters.newtonIterations<<" retries="<<counters.retryTrials
            <<" tails="<<counters.tailTrials<<" roulette="<<counters.rouletteStops
            <<" owned="<<counters.ownedRoots<<" rejected="<<counters.rejectedRoots<<'\n';
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
static void UnsupportedCasterSwitches() {
    for(unsigned kind:{0u,1u,2u,3u}) for(bool reverse:{false,true}) for(bool remote:{false,true}) {
        const bool csg=kind==1;
        Fixture f(Materials()+Mesh(csg,reverse)+Object("pane","shape","glass",csg?" scale 1 1 0.25\n":" position 0 0 1\n")
            +(csg?"sphere_geometry\n{\n name tiny\n radius 0.1\n}\n"+Object("other","tiny","glass"," position 4 0 0\n")
                +"csg_object\n{\n name inherited\n obja pane\n objb other\n operation union\n}\n":"")
            +(kind==2?"weave_material\n{\n name gap\n fabric custom\n transmission thin\n gap 1\n}\n"+PlaneScene("gap_geo",-.5)+Object("gap","gap_geo","gap"):"")
            +"lambertian_material\n{\n name diffuse\n reflectance white\n}\n"
            +PlaneScene("floor",-2)+Object("receiver","floor","diffuse")
            +"omni_light\n{\n name source\n position 0 0 1\n color 1 1 1\n power 40\n}\n"
            +(remote?Object("remote_mirror","shape","mirror"," position 1000 0 1\n"):""));
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
        ManifoldSolverConfig config;config.enabled=true;config.extendedMode=true;config.targetBounces=1;
        config.multiTrials=1;StabilityConfig stability;stability.rrMinDepth=20;
        auto* on=new PathTracingIntegrator(config,stability);on->SetMaxPathDepth(8);
        ManifoldSolverConfig plain;auto* off=new PathTracingIntegrator(plain,stability);off->SetMaxPathDepth(8);
        IORStack air(1);
        Check(!on->GetSolver()->ExtendedAnchorEligible(f.Scene(),*caster,Point3(0,0,-2),air),
            "unsupported transmissive caster rejects the anchor, including inherited CSG and a mixed supported caster set");
        for(unsigned trial=0;trial<4;++trial) {
            const unsigned salt=SobolSequence::HashCombine(12000+trial,0x554e4345);
            SobolSamplerTestHooks::ValueSalt().store(salt);
            std::array<RISEPel,2> values;
            for(unsigned mode=0;mode<2;++mode) {
                RandomNumberGenerator random(salt);SobolSampler sampler(0,7);
                RuntimeContext context(random,RuntimeContext::PASS_NORMAL,false);context.pSampler=&sampler;
                values[mode]=(mode?on:off)->IntegrateRay(context,nullRasterizerState,
                    Ray(Point3(0,0,-1.9),Vector3(0,0,-1)),f.Scene(),*caster,sampler,nullptr,nullptr);
            }
            Check(std::isfinite(values[0].r)&&values[0].r>0,"PT clear-transmission control is finite and lit");
            Check(std::memcmp(&values[0],&values[1],sizeof(RISEPel))==0,
                "ineligible extended anchor preserves PT light with all three switches off");
            std::cout<<"unsupported caster kind="<<kind<<" winding="<<reverse<<" remote="<<remote<<" salt="<<salt<<" PT="<<values[0].r<<" extended="<<values[1].r<<'\n';
        }
        SobolSamplerTestHooks::ValueSalt().store(0);
        on->release();off->release();caster->release();shader->release();
    }
}
static void ImpossibleSolverSwitches() {
    for(bool reverse:{false,true}) for(unsigned invalid:{0u,1u,2u,3u}) {
        std::string text=Materials();
        text.replace(text.find("values 1.3 1.5 1.9"),std::string("values 1.3 1.5 1.9").size(),"values 1 1 1");
        Fixture f(text+Mesh(false,reverse)+Object("pane","shape","glass"," position 0 0 1\n")
            +"lambertian_material\n{\n name diffuse\n reflectance white\n}\n"
            +PlaneScene("floor",-2)+Object("receiver","floor","diffuse")
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
            +Object("remote_composite","shape","layers"," position 1000 0 0\n")
            +PlaneScene("mirror_plane",0)+Object("mirror","mirror_plane","mirror")
            +"lambertian_material\n{\n name diffuse\n reflectance white\n}\n"
            +PlaneScene("floor",-2)+Object("receiver","floor","diffuse")
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
    text+=Mesh(true,reverse)+Object("slab","shape","glass"," scale 3 3 0.25\n")
        +PlaneScene("floor",-2,-4,4)+Object("receiver","floor","diffuse")
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
    bool synthetic=true,geometry=true,delta=true,unsupported=true,production=false,slab=true;
    Check(ConfigureTestWorker(),"reference renderer uses one configured worker");
    if(argc==2&&std::string(argv[1])=="--synthetic-only") {geometry=false;delta=false;unsupported=false;slab=false;}
    if(argc==2&&std::string(argv[1])=="--geometry-only") {synthetic=false;delta=false;unsupported=false;slab=false;}
    if(argc==2&&std::string(argv[1])=="--delta-only") {synthetic=false;geometry=false;unsupported=false;slab=false;}
    if(argc==2&&std::string(argv[1])=="--unsupported-only") {synthetic=false;geometry=false;delta=false;slab=false;}
    if(argc==2&&std::string(argv[1])=="--production-only") {synthetic=false;geometry=false;delta=true;unsupported=false;production=true;slab=false;}
    if(argc==2&&std::string(argv[1])=="--slab-only") {synthetic=false;geometry=false;delta=false;unsupported=false;}
    if(argc==2&&std::string(argv[1])=="--review-only") {
#ifdef RISE_SMS_REFERENCE_A
        NativePeriodicRoots();
        InteriorAtlasRoots();
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
    if(synthetic) Synthetic();if(geometry) {Geometry();CloseRoots();WalkEvents();NativePeriodicRoots();}
#else
    if(synthetic||geometry) std::cout<<"Estimator A is absent on this committed baseline; new helper tests are unavailable, not a numerical red proof.\n";
#endif
    if(delta) DeltaLights(production);
    if(argc==1) for(bool uniform:{false,true}) {DeltaLights(false,true,uniform);DeltaLights(true,true,uniform);}
    if(unsupported) {UnsupportedCasterSwitches();CompositeProxyPolicy();ImpossibleSolverSwitches();}
    if(slab) SlabRenders();
    std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
}
