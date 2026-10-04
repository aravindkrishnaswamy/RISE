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
                <<" stops="<<counters.rouletteStops.load()<<" maxK="<<maxReciprocal<<" maxDeposit="<<maxDeposit<<" histogram=";
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
static std::string CloseRootMesh(bool reverse, Scalar scale) {
    std::ostringstream s; s<<std::setprecision(17);
    s<<"indexedmesh_geometry\n{\n name shape\n double_sided TRUE\n face_normals TRUE\n";
    for(int side:{-1,1}) {
        const Scalar x=side*.001;
        const Point3 center(x,0,0), a(-.5,0,-3), b(.5,0,-3);
        const Vector3 n=Vector3Ops::Normalize(Vector3Ops::Normalize(Vector3Ops::mkVector3(a,center))
            +Vector3Ops::Normalize(Vector3Ops::mkVector3(b,center)));
        for(const Point2& p:{Point2(-.0004,-1),Point2(.0004,-1),Point2(.0004,1),Point2(-.0004,1)})
            s<<" vertex "<<scale*(x+p.x)<<' '<<scale*p.y<<' '<<scale*(-n.x*p.x/n.z)<<'\n';
    }
    for(int i=0;i<8;++i) s<<" uv "<<(i%2)<<' '<<((i/2)%2)<<'\n';
    for(int offset:{0,4}) {
        s<<" triangle "<<offset<<' '<<offset+(reverse?2:1)<<' '<<offset+(reverse?1:2)<<'\n';
        s<<" triangle "<<offset<<' '<<offset+(reverse?3:2)<<' '<<offset+(reverse?2:3)<<'\n';
    }
    return s.str()+"}\n";
}
static void CloseRoots() {
    for(bool winding:{false,true}) for(Scalar scale:{.01,1.,100.}) {
        Fixture f(Materials()+CloseRootMesh(winding,scale)+Object("caster","shape","mirror"));
        ManifoldSolverConfig cfg;cfg.extendedMode=true;cfg.targetBounces=1;cfg.solverThreshold=1e-10;
        auto* solver=new ManifoldSolver(cfg);IORStack air(1);
        ScriptSampler left({.2,.64,.4,.1}),right({.2,.64,.4,.6});
        const Point3 start(-.5*scale,0,-3*scale),end(.5*scale,0,-3*scale);
        const auto a=solver->ProposeExtendedRoot(start,Vector3(0,0,1),end,f.Scene(),air,SMSQueryDomain::RGB(0),left);
        const auto b=solver->ProposeExtendedRoot(start,Vector3(0,0,1),end,f.Scene(),air,SMSQueryDomain::RGB(0),right);
        Check(a.accepted&&b.accepted,"nearby mesh roots solve independently at changed scene scales");
        if(a.accepted&&b.accepted) {
            std::cout<<"close roots scale="<<scale<<" winding="<<winding<<" separation="
                <<Point3Ops::Distance(a.vertices[0].geometry.position,b.vertices[0].geometry.position)<<'\n';
            Check(!ManifoldSolver::SameExtendedRoot(a,b,cfg.uniquenessThreshold*a.scale),
                "production root identity separates nearby physical roots");
        }
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
            auto changed=root;changed.domain=SMSQueryDomain::RGB(2);
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
int main(int argc,char** argv) {
    bool synthetic=true,geometry=true;
    if(argc==2&&std::string(argv[1])=="--synthetic-only") geometry=false;
    if(argc==2&&std::string(argv[1])=="--geometry-only") synthetic=false;
    std::cout<<std::setprecision(12);
#ifdef RISE_SMS_REFERENCE_A
    if(synthetic) Synthetic();if(geometry) {Geometry();CloseRoots();}
#else
    std::cout<<"Estimator A is absent on this committed baseline; new helper tests are unavailable, not a numerical red proof.\n";
#endif
    std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
}
