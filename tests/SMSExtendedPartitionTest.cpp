// Extended SMS Phase 3: estimator B (topology-level reciprocal) and the
// area-emitter ownership partition shared by SMS and PT.
//
// Sections (all by default; `--section <name>` runs one; `--quick` lowers
// the sample counts for a smoke run, never for the recorded gate):
//   synthetic  estimator B's expectation on a known topology distribution
//   predicate  the canonical predicate on real scenes: determinism and
//              independence of N, budget, emitter-normal sign and prior
//              activity; owned(PT record of r) == (r in O(T,y)) for every
//              canonical AND every random-proposal root; filters and bounds
//   render     point renders through the production PT entry: full ==
//              SMS-off PT, SMS-owned (full - kept, paired PT paths) ==
//              PT-owned (ref - kept), mirror closed form, both windings of
//              real double-sided indexed meshes, plane/sphere controls,
//              transformed instance, start-inside, RGB per-component
//              ownership, NM, emitter sidedness, trial budget, scale
//   switches   HWSS ignores extended mode; dropping SMS's deposit leaves
//              PT's kept paths untouched
//   fixtures   DL-372/DL-379 ball lens images vs PT and VCM, DL-336
//              immersed ball, DL-376/DL-421 seeding-mode independence
//
// Pre-Phase-3 sources lack RISE_SMS_EXTENDED_PARTITION: the partition-API
// checks then compile out and the production render comparisons alone run
// (the committed red proof).
#include "SMSRenderTestSupport.h"
#include "../src/Library/Utilities/ManifoldSolver.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/IORStackSeeding.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/ILuminaryManager.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Rendering/LuminaryManager.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IEmitter.h"
#include "../src/Library/Interfaces/IBSDF.h"
#include "../src/Library/Shaders/PathTracingIntegrator.h"
#include "../src/Library/Shaders/PathTracingShaderOp.h"
#include "../src/Library/Rendering/PathTracingPelRasterizer.h"
#include "../src/Library/Utilities/PathGuidingField.h"
#include "../src/Library/Utilities/RuntimeContext.h"
#include <array>
#include <sstream>
#include <iomanip>
#include <map>
#include <chrono>

namespace
{
    bool g_quick=false;

    struct Moments {
        double mean=0, sd=0, se=0; std::size_t n=0;
        explicit Moments(const std::vector<double>& values) {
            n=values.size();
            for(double x:values) mean+=x;
            mean/=double(n);
            for(double x:values) sd+=(x-mean)*(x-mean);
            sd=n>1?std::sqrt(sd/double(n-1)):0;
            se=n>0?sd/std::sqrt(double(n)):0;
        }
    };

    struct Fixture {
        IJobPriv* job=nullptr;
        explicit Fixture(const std::string& text) {
            const auto path=TestTempPath("sms_partition_"+std::to_string(::getpid())+".RISEscene");
            {std::ofstream out(path);out<<text;}
            if(RISE_CreateJobPriv(&job)) Check(job->LoadAsciiSceneViaCst(path.c_str()),"partition scene parses");
            std::remove(path.c_str());
            if(job) job->GetScene()->GetObjects()->PrepareForRendering();
        }
        ~Fixture() { safe_release(job); }
        bool Ok() const { return job!=nullptr; }
        const IScene& Scene() const { return *job->GetScene(); }
        const IObject* Object(const char* name) const { return Scene().GetObjects()->GetItem(name); }
    };

    std::string Header() {
        return "RISE ASCII SCENE 7\n"
            "uniformcolor_painter\n{\n name white\n color 1 1 1\n}\n"
            "uniformcolor_painter\n{\n name light_color\n color 1 1 1\n}\n"
            "scalar_painter\n{\n name flat\n values 1.5 1.5 1.5\n}\n"
            "scalar_painter\n{\n name triple\n values 1.3 1.5 1.9\n}\n"
            "lambertian_material\n{\n name diffuse\n reflectance white\n}\n"
            "perfectreflector_material\n{\n name mirror\n reflectance white\n}\n"
            "perfectrefractor_material\n{\n name glass\n refractance white\n ior flat\n}\n"
            "perfectrefractor_material\n{\n name prism\n refractance white\n ior triple\n}\n"
            "dielectric_material\n{\n name medium14\n ior 1.4\n tau 1.0\n scattering 1000000\n}\n"
            "lambertian_luminaire_material\n{\n name lum\n exitance light_color\n scale 10\n material none\n}\n";
    }
    std::string Obj(const std::string& name,const std::string& geometry,const std::string& material,
        const std::string& extra="") {
        return "standard_object\n{\n name "+name+"\n geometry "+geometry+"\n material "+material+"\n"+extra+"}\n";
    }
    // Horizontal clipped plane. The corner order as written (flip false)
    // gives a -z normal; flip reverses it. A single-sided clipped plane is
    // pushed 1e-5 along its normal by UniformRandomPoint (SampleLight).
    std::string ClippedQuad(const std::string& name,double z,double x0,double x1,double y0,double y1,
        bool flip,bool doubleSided) {
        std::ostringstream s;s<<std::setprecision(17);
        const double xs[4]={x0,x0,x1,x1}, ys[4]={y0,y1,y1,y0};
        const char* labels[4]={"pta","ptb","ptc","ptd"};
        s<<"clippedplane_geometry\n{\n name "<<name<<"\n";
        for(int i=0;i<4;++i) {const int j=flip?3-i:i;s<<" "<<labels[i]<<" "<<xs[j]<<" "<<ys[j]<<" "<<z<<"\n";}
        s<<" doublesided "<<(doubleSided?"TRUE":"FALSE")<<"\n}\n";
        return s.str();
    }
    // Real double-sided indexed mesh quad, both windings.
    std::string MeshQuad(const std::string& name,double z,double x0,double x1,double y0,double y1,bool reverse) {
        std::ostringstream s;s<<std::setprecision(17);
        s<<"indexedmesh_geometry\n{\n name "<<name<<"\n double_sided TRUE\n face_normals TRUE\n";
        s<<" vertex "<<x0<<" "<<y0<<" "<<z<<"\n vertex "<<x1<<" "<<y0<<" "<<z
            <<"\n vertex "<<x1<<" "<<y1<<" "<<z<<"\n vertex "<<x0<<" "<<y1<<" "<<z
            <<"\n uv 0 0\n uv 1 0\n uv 1 1\n uv 0 1\n";
        s<<(reverse?" triangle 0 2 1\n triangle 0 3 2\n":" triangle 0 1 2\n triangle 0 2 3\n");
        return s.str()+"}\n";
    }
    // Closed double-sided indexed-mesh unit box [-1,1]^3, both windings.
    std::string MeshBox(const std::string& name,bool reverse) {
        std::ostringstream s;
        s<<"indexedmesh_geometry\n{\n name "<<name<<"\n double_sided TRUE\n face_normals TRUE\n";
        const int p[8][3]={{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
        for(const auto& v:p) s<<" vertex "<<v[0]<<' '<<v[1]<<' '<<v[2]<<'\n';
        for(int i=0;i<8;++i) s<<" uv "<<(i%2)<<' '<<((i/2)%2)<<'\n';
        const int t[12][3]={{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
            {3,7,6},{3,6,2},{0,4,7},{0,7,3},{1,2,6},{1,6,5}};
        for(const auto& f:t) s<<" triangle "<<f[0]<<' '<<f[reverse?2:1]<<' '<<f[reverse?1:2]<<'\n';
        return s.str()+"}\n";
    }

    // ---------------------------------------------------------------
    // Point scenes. The receiver point is the origin, normal +z; the
    // camera ray starts 0.3 above it and looks straight down.
    // ---------------------------------------------------------------
    enum class EmitterFacing { TowardCaster, AwayFromCaster, DoubleSided };
    struct SceneSpec {
        std::string text;
        Ray camera = Ray(Point3(0,0,0.3),Vector3(0,0,-1));
        unsigned maxDepth = 16;
        std::string label;
        double closedForm = -1;
    };
    // k = 1 planar mirror at z = 2s over a receiver patch at z = 0; a 1x1
    // (s^2) emitter at z = s, x in [s,2s]. The receiver cannot see itself
    // in the mirror: the transport is direct + one mirror bounce.
    SceneSpec MirrorScene(bool meshMirror,bool reverse,EmitterFacing facing,double scale=1) {
        SceneSpec s;
        std::ostringstream label;
        label<<"mirror mesh="<<meshMirror<<" reverse="<<reverse<<" facing="<<int(facing)<<" scale="<<scale;
        s.label=label.str();
        s.camera=Ray(Point3(0,0,0.3*scale),Vector3(0,0,-1));
        const double k=scale;
        s.text=Header()
            +ClippedQuad("receiver_geo",0,-0.5*k,0.5*k,-0.5*k,0.5*k,false,true)+Obj("receiver","receiver_geo","diffuse")
            +(meshMirror?MeshQuad("mirror_geo",2*k,0.4*k,4*k,-2*k,2*k,reverse):ClippedQuad("mirror_geo",2*k,0.4*k,4*k,-2*k,2*k,reverse,true))
            +Obj("caster","mirror_geo","mirror")
            +ClippedQuad("emitter_geo",k,k,2*k,-0.5*k,0.5*k,facing!=EmitterFacing::AwayFromCaster,facing==EmitterFacing::DoubleSided)
            +Obj("emitter","emitter_geo","lum");
        return s;
    }
    // k = 2 refraction through a closed glass slab (z in [1.75,2.25]) with
    // genuine below-TIR reflection roots; an emitter above it facing down.
    SceneSpec SlabScene(bool reverse,const std::string& material,bool transformed) {
        SceneSpec s;s.label="slab reverse="+std::to_string(reverse)+" material="+material+" transformed="+std::to_string(transformed);
        const std::string placement=transformed
            ? " position 0.1 -0.05 2\n orientation 4 -3 7\n scale 1.6 1.3 0.25\n"
            : " position 0 0 2\n scale 1.5 1.5 0.25\n";
        s.text=Header()
            +ClippedQuad("receiver_geo",0,-0.5,0.5,-0.5,0.5,false,true)+Obj("receiver","receiver_geo","diffuse")
            +MeshBox("slab_geo",reverse)+Obj("caster","slab_geo",material,placement)
            +ClippedQuad("emitter_geo",3.5,-0.6,0.6,-0.6,0.6,false,true)+Obj("emitter","emitter_geo","lum");
        return s;
    }
    // Analytic glass sphere control.
    SceneSpec SphereScene() {
        SceneSpec s;s.label="sphere";
        s.text=Header()
            +ClippedQuad("receiver_geo",0,-0.5,0.5,-0.5,0.5,false,true)+Obj("receiver","receiver_geo","diffuse")
            +"sphere_geometry\n{\n name ball_geo\n radius 0.6\n}\n"+Obj("caster","ball_geo","glass"," position 0.15 0 1.4\n")
            +ClippedQuad("emitter_geo",3.2,-0.7,0.7,-0.7,0.7,false,true)+Obj("emitter","emitter_geo","lum");
        return s;
    }
    // Start-inside: receiver and camera inside a closed glass mesh box.
    SceneSpec InsideScene(bool reverse) {
        SceneSpec s;s.label="inside reverse="+std::to_string(reverse);
        s.text=Header()
            +ClippedQuad("receiver_geo",0,-0.4,0.4,-0.4,0.4,false,true)+Obj("receiver","receiver_geo","diffuse")
            +MeshBox("box_geo",reverse)+Obj("caster","box_geo","glass"," position 0 0 0.5\n scale 1.2 1.2 1\n")
            +ClippedQuad("emitter_geo",2.5,-0.7,0.7,-0.7,0.7,false,true)+Obj("emitter","emitter_geo","lum");
        return s;
    }
    // Delegations. A random-walk SSS receiver (closed box, top face at the
    // origin) under the slab: its BSDF vertex is an anchor and its BSSRDF
    // continuation re-enters PT through the shader-op boundary. A wrapped
    // caster (a zero-exitance luminaire over glass) crosses PT's SPF-only
    // delta branch, which the extended domain cannot replay.
    SceneSpec SSSReceiverScene() {
        SceneSpec s;s.label="SSS receiver under slab";
        s.text=Header()
            +"randomwalk_sss_material\n{\n name rw\n ior 1.3\n absorption 0.8 0.4 0.04\n scattering 3 3.5 4\n g 0\n roughness 0.1\n max_bounces 64\n}\n"
            +MeshBox("receiver_geo",false)+Obj("receiver","receiver_geo","rw"," position 0 0 -0.1\n scale 0.5 0.5 0.1\n")
            +MeshBox("slab_geo",false)+Obj("caster","slab_geo","glass"," position 0 0 2\n scale 1.5 1.5 0.25\n")
            +ClippedQuad("emitter_geo",3.5,-0.6,0.6,-0.6,0.6,false,true)+Obj("emitter","emitter_geo","lum");
        return s;
    }
    SceneSpec WrappedCasterScene() {
        SceneSpec s;s.label="wrapped caster (SPF-only delta)";
        s.text=Header()
            +"uniformcolor_painter\n{\n name black\n color 0 0 0\n}\n"
            +"lambertian_luminaire_material\n{\n name glasswrap\n exitance black\n scale 1\n material glass\n}\n"
            +ClippedQuad("receiver_geo",0,-0.5,0.5,-0.5,0.5,false,true)+Obj("receiver","receiver_geo","diffuse")
            +MeshBox("slab_geo",false)+Obj("caster","slab_geo","glasswrap"," position 0 0 2\n scale 1.5 1.5 0.25\n")
            +ClippedQuad("emitter_geo",3.5,-0.6,0.6,-0.6,0.6,false,true)+Obj("emitter","emitter_geo","lum");
        return s;
    }
    // DL-336: a glass ball immersed in a constant ior-1.4 enclosure (camera,
    // receiver, ball and emitter all inside it): a weak lens with two
    // images of a receiver point. `air` swaps the enclosure out.
    SceneSpec ImmersedBallScene(bool air) {
        SceneSpec s;s.label=std::string("DL-336 immersed ball ")+(air?"air":"ior 1.4");
        s.text=Header()
            +ClippedQuad("receiver_geo",0,-0.5,0.5,-0.5,0.5,false,true)+Obj("receiver","receiver_geo","diffuse")
            +"sphere_geometry\n{\n name ball_geo\n radius 0.15\n}\n"+Obj("caster","ball_geo","glass"," position 0.3 0 0.55\n")
            +ClippedQuad("emitter_geo",1,0.5,0.7,-0.1,0.1,false,false)+Obj("emitter","emitter_geo","lum"," position 0 0 0\n")
            +(air?std::string():MeshBox("enclosure_geo",false)+Obj("enclosure","enclosure_geo","medium14"," position 0 0 0.5\n scale 3 3 1\n"));
        // The 0.04-area emitter needs more power than the shared one.
        const std::string from="name lum\n exitance light_color\n scale 10";
        s.text.replace(s.text.find(from),from.size(),"name lum\n exitance light_color\n scale 400");
        return s;
    }

    // ---------------------------------------------------------------
    // Point renders: IntegrateRay on one camera ray, N samples, n salts.
    // ---------------------------------------------------------------
    enum class Mode { Ref, Full, Kept, Legacy };
    struct RenderOptions {
        Scalar nm=0; unsigned N=16384, salts=4, saltBase=61000, trials=2;
        bool hwss=false; ManifoldSolverConfig::SeedingMode seeding=ManifoldSolverConfig::eSeedingSnell;
        unsigned targetBounces=0; bool biased=true; SMSReferenceCounters* counters=nullptr;
        // Shade CastRay re-entries (BSSRDF continuations) with a
        // PathTracingShaderOp carrying the same SMS configuration.
        bool reentryShaderOp=false;
    };
    struct PointResult {
        std::array<std::vector<double>,3> c; // per-salt means
        bool ok=false;
    };
    PointResult RenderPoint(const SceneSpec& spec,Mode mode,const RenderOptions& o) {
        PointResult out;
        Fixture fixture(spec.text);
        if(!fixture.Ok()) return out;
        ManifoldSolverConfig cfg;cfg.enabled=mode!=Mode::Ref;cfg.extendedMode=mode!=Mode::Legacy;
        cfg.biased=o.biased;cfg.multiTrials=o.trials;cfg.maxBernoulliTrials=64;
        cfg.seedingMode=o.seeding;cfg.targetBounces=o.targetBounces;
#ifdef RISE_SMS_EXTENDED_PARTITION
        cfg.extendedDropAreaContributions=mode==Mode::Kept;
#endif
        cfg.referenceCounters=o.counters;
        StabilityConfig stability;stability.rrMinDepth=4;
        std::vector<IShaderOp*> ops;IShader* shader=nullptr;
        PathTracingShaderOp* reentry=nullptr;
        if(o.reentryShaderOp) {reentry=new PathTracingShaderOp(cfg,stability);ops.push_back(reentry);}
        const bool shaderOk=RISE_API_CreateStandardShader(&shader,ops)&&shader;
        if(!shaderOk) {if(reentry) reentry->release();return out;}
        auto* caster=new RayCaster(false,16,*shader,true);caster->AttachScene(&fixture.Scene());
        auto* integrator=new PathTracingIntegrator(cfg,stability);integrator->SetMaxPathDepth(spec.maxDepth);
        if(integrator->GetSolver()) {
            std::vector<const IObject*> casters;ManifoldSolver::EnumerateSpecularCasters(fixture.Scene(),casters);
            integrator->GetSolver()->SetSpecularCasters(casters);
        }
        for(unsigned salt=0;salt<o.salts;++salt) {
            const unsigned value=SobolSequence::HashCombine(o.saltBase+salt,0x50415254u);
            SobolSamplerTestHooks::ValueSalt().store(value);
            std::array<double,3> sum{};
            for(unsigned sample=0;sample<o.N;++sample) {
                SobolSampler sampler(sample,29);
                RandomNumberGenerator random(value+sample);
                RuntimeContext context(random,RuntimeContext::PASS_NORMAL,false);context.pSampler=&sampler;
                if(o.nm>0 && o.hwss) {
                    SampledWavelengths swl=SampledWavelengths::SampleEquidistant(sampler.Get1D(),380,780);
                    Scalar values[SampledWavelengths::N];
                    integrator->IntegrateRayHWSS(context,nullRasterizerState,spec.camera,swl,fixture.Scene(),*caster,sampler,nullptr,values);
                    sum[0]+=values[0];
                } else if(o.nm>0) {
                    sum[0]+=integrator->IntegrateRayNM(context,nullRasterizerState,spec.camera,o.nm,
                        fixture.Scene(),*caster,sampler,nullptr,nullptr);
                } else {
                    const RISEPel v=integrator->IntegrateRay(context,nullRasterizerState,spec.camera,
                        fixture.Scene(),*caster,sampler,nullptr,nullptr);
                    for(unsigned c=0;c<3;++c) sum[c]+=v[c];
                }
            }
            for(unsigned c=0;c<3;++c) out.c[c].push_back(sum[c]/o.N);
        }
        SobolSamplerTestHooks::ValueSalt().store(0);
        integrator->release();caster->release();shader->release();
        if(reentry) reentry->release();
        out.ok=true;
        return out;
    }

    bool Agree(const Moments& a,const Moments& b) {
        return std::fabs(a.mean-b.mean)<=3*std::hypot(a.se,b.se);
    }
#ifdef RISE_SMS_EXTENDED_PARTITION
    std::vector<double> Diff(const std::vector<double>& a,const std::vector<double>& b) {
        std::vector<double> d;for(std::size_t i=0;i<a.size()&&i<b.size();++i) d.push_back(a[i]-b[i]);return d;
    }
#endif

    void PrintCounters(const std::string& label,const SMSReferenceCounters& counters) {
        std::cout<<label<<" counters proposals="<<counters.proposalTrials<<" zeros="<<counters.zeroTrials
            <<" retries="<<counters.retryTrials<<" tails="<<counters.tailTrials<<" roulette="<<counters.rouletteStops
            <<" ownedRoots="<<counters.ownedRoots;
#ifdef RISE_SMS_EXTENDED_PARTITION
        std::cout<<" topologyRetries="<<counters.topologyRetryTrials<<" canonicalSolves="<<counters.canonicalSolves
            <<" canonicalRoots="<<counters.canonicalRoots<<" partitionQueries="<<counters.partitionQueries
            <<" partitionOwned="<<counters.partitionOwned<<" uncertain="<<counters.partitionUncertain
            <<" samplerDraws="<<counters.canonicalSamplerDraws;
        Check(counters.canonicalSamplerDraws==0,label+": the canonical predicate drew no random number");
#endif
        unsigned long long median=0, total=0;
        for(unsigned b=0;b<32;++b) total+=counters.retryHistogram[b];
        for(unsigned b=0, acc=0;b<32;++b) { acc+=unsigned(counters.retryHistogram[b]); if(acc*2>=total) {median=1ull<<b;break;} }
        std::cout<<" retryMedian~"<<median<<"\n";
    }
}

//////////////////////////////////////////////////////////////////////
// synthetic: estimator B on a known topology distribution
//////////////////////////////////////////////////////////////////////
static void SyntheticTopology()
{
    // Walk outcomes: -1 a zero trial, else a topology id. T2's owned set is
    // EMPTY: it contributes zero and enters no reciprocal loop. The
    // expectation is sum_T S(T) / pL, independent of P(T) and of N.
    struct Law { double p[4]; double owned[4][3]; };
    for(const Law& law:{Law{{0.05,0.5,0.3,0.15},{{0,0,0},{3.0,2.0,1.0},{0,0,0},{1.5,0.25,4.0}}},
                       Law{{0.6,0.37,0.02,0.01},{{0,0,0},{0,0,0},{2.0,1.0,3.0},{0.5,0.5,0.5}}}}) {
    const double pL=0.37;
    const auto draw=[&](ISampler& s){
        const Scalar u=s.Get1D(); double acc=0;
        for(int t=0;t<4;++t) { acc+=law.p[t]; if(u<acc) return t==0?-1:t; }
        return 3;
    };
    for(unsigned N:{1u,4u}) for(bool rgb:{false,true}) {
        std::array<std::vector<double>,3> batches;
        RandomNumberGenerator rng(0x51ABCDu+N*7+rgb);IndependentSampler master(rng);
        for(unsigned batch=0;batch<16;++batch) {
            std::array<double,3> sum{};
            const unsigned outer=20000;
            for(unsigned o=0;o<outer;++o) {
                for(unsigned trial=0;trial<N;++trial) {
                    const unsigned c=rgb?std::min(2u,unsigned(master.Get1D()*3)):0;
                    const int t=draw(master);
                    if(t<0 || (law.owned[t][0]==0&&law.owned[t][1]==0&&law.owned[t][2]==0)) continue;
                    RandomNumberGenerator retryRng(static_cast<unsigned int>(rng.CanonicalRandom()*4294967295.0));
                    IndependentSampler retry(retryRng);
                    int candidate=0;
                    const Scalar k=SMSRootReference::Reciprocal(t,retry,
                        [&](ISampler& s)->const int& {candidate=draw(s);return candidate;},
                        [](int a,int b){return a==b;},64,true,nullptr);
                    sum[c]+=SMSRootReference::Deposit(law.owned[t][c],k,rgb?Scalar(1)/3:Scalar(1),pL,N);
                }
            }
            for(unsigned c=0;c<3;++c) batches[c].push_back(sum[c]/outer);
        }
        for(unsigned c=0;c<(rgb?3u:1u);++c) {
            const Moments m(batches[c]);
            const double expected=(law.owned[1][c]+law.owned[2][c]+law.owned[3][c])/pL;
            std::cout<<std::setprecision(10)<<"synthetic N="<<N<<" rgb="<<rgb<<" c="<<c<<" mean="<<m.mean
                <<" se="<<m.se<<" expected="<<expected<<" n=16\n";
            Check(std::fabs(m.mean-expected)<=4*m.se,"estimator B synthetic expectation is sum_T S(T)/pL within 4 se");
        }
    }
    }
}

#ifdef RISE_SMS_EXTENDED_PARTITION
//////////////////////////////////////////////////////////////////////
// predicate: the canonical ownership predicate on real scenes
//////////////////////////////////////////////////////////////////////
namespace
{
    struct Anchor {
        Point3 pos; Vector3 geomNormal, shadingNormal; IORStack stack{1.0};
    };
    bool MakeAnchor(const Fixture& f,const Ray& camera,Anchor& a) {
        RayIntersection hit(camera,nullRasterizerState);
        f.Scene().GetObjects()->IntersectRay(hit,true,true,false);
        if(!hit.geometric.bHit) return false;
        a.pos=hit.geometric.ptIntersection;a.geomNormal=hit.geometric.vGeomNormal;
        a.shadingNormal=hit.geometric.vNormal;
        a.stack=IORStack(1.0);
        IORStackSeeding::SeedFromPoint(a.stack,camera.origin,f.Scene());
        return true;
    }
    // The record PT builds while tracing a chain through `positions` to y.
    bool RecordFromPositions(const Fixture& f,const Anchor& a,const std::vector<Point3>& positions,
        const Point3& y,SMSChainRecord& rec) {
        rec=SMSChainRecord();
        rec.SetAnchor(a.pos,a.geomNormal,a.shadingNormal,a.stack,true);
        Point3 previous=a.pos;
        for(std::size_t i=0;i<positions.size();++i) {
            Vector3 dIn=Vector3Ops::mkVector3(positions[i],previous);
            if(Vector3Ops::NormalizeMag(dIn)<=0) return false;
            Ray ray(previous,dIn);ray.Advance(1e-8);
            RayIntersection hit(ray,nullRasterizerState);
            f.Scene().GetObjects()->IntersectRay(hit,true,true,false);
            if(!hit.geometric.bHit) return false;
            const Point3 next=i+1<positions.size()?positions[i+1]:y;
            Vector3 dOut=Vector3Ops::mkVector3(next,hit.geometric.ptIntersection);
            if(Vector3Ops::NormalizeMag(dOut)<=0) return false;
            rec.Append(hit.geometric,hit.pObject,dIn,dOut);
            previous=hit.geometric.ptIntersection;
        }
        return true;
    }
    std::vector<Point3> Positions(const SMSDomainRoot& r) {
        std::vector<Point3> p;for(const auto& v:r.result.specularChain) p.push_back(v.position);return p;
    }
}

static void PredicateOn(const SceneSpec& spec,unsigned samples,bool expectOwned=true)
{
    Fixture f(spec.text);
    if(!f.Ok()) {Check(false,"predicate fixture loads");return;}
    std::vector<IShaderOp*> ops;IShader* shader=nullptr;
    if(!RISE_API_CreateStandardShader(&shader,ops)||!shader) {Check(false,"shader");return;}
    auto* caster=new RayCaster(false,16,*shader,true);caster->AttachScene(&f.Scene());
    SMSReferenceCounters counters;
    ManifoldSolverConfig cfg;cfg.enabled=true;cfg.extendedMode=true;cfg.biased=true;cfg.multiTrials=1;
    cfg.maxBernoulliTrials=64;cfg.referenceCounters=&counters;
    ManifoldSolverConfig other=cfg;other.multiTrials=16;other.maxBernoulliTrials=3;other.referenceCounters=nullptr;
    auto* solver=new ManifoldSolver(cfg);auto* solver2=new ManifoldSolver(other);
    Anchor a;
    Check(MakeAnchor(f,spec.camera,a),spec.label+": anchor intersects the receiver");
    const IObject* luminary=f.Object("emitter");
    Check(luminary!=nullptr,"emitter object present");
    Check(solver->ExtendedModeActive(f.Scene()),spec.label+": extended mode active (composite-free prepared scene)");
    const LightSampler* ls=caster->GetLightSampler();
    const auto* manager=dynamic_cast<const LuminaryManager*>(caster->GetLuminaries());
    unsigned walks=0,owned=0,proposals=0,proposalOwned=0,mismatch=0,recordFailures=0;
    std::map<unsigned,unsigned> rootCounts;std::map<std::string,unsigned> topologyCounts;
    for(unsigned c=0;c<3 && luminary && ls && manager;++c) {
        const SMSQueryDomain domain=SMSQueryDomain::RGB(c);
        RandomNumberGenerator rng(0x7A11u+c);IndependentSampler sampler(rng);
        for(unsigned i=0;i<samples;++i) {
            LightSample light;
            if(!ls->SampleLight(f.Scene(),const_cast<LuminaryManager*>(manager)->getLuminaries(),sampler,light)) continue;
            if(light.isDelta || light.pLuminary!=luminary) continue;
            const Point3 y=ManifoldSolver::ExtendedLuminaryPoint(*luminary,light.position,light.normal);
            SMSStartingMedia media;IORStack domainStack(a.stack.EnvironmentIOR());
            if(!SMSDomainReplay::Capture(f.Scene(),a.pos,a.stack,media)
                || !SMSDomainReplay::BuildStack(media,domain,domainStack)) {Check(false,"anchor media replay");continue;}
            SMSDomainRoot topology(domain,domainStack);
            if(!solver->BuildExtendedSeed(a.pos,light.position,f.Scene(),domainStack,domain,sampler,
                topology.vertices,nullRasterizerState,luminary)) continue;
            ++walks;
            std::vector<SMSDomainRoot> roots,again,viaOther;
            solver->CanonicalExtendedRoots(a.pos,a.shadingNormal,*luminary,y,light.normal,
                f.Scene(),topology,nullRasterizerState,roots);
            // Determinism: repeat, opposite emitter-normal sign, another
            // trial budget N and retry budget, after intervening activity.
            solver->CanonicalExtendedRoots(a.pos,a.shadingNormal,*luminary,y,-light.normal,
                f.Scene(),topology,nullRasterizerState,again);
            solver2->CanonicalExtendedRoots(a.pos,a.shadingNormal,*luminary,y,light.normal,
                f.Scene(),topology,nullRasterizerState,viaOther);
            bool same=roots.size()==again.size()&&roots.size()==viaOther.size();
            const Scalar tol=std::sqrt(std::numeric_limits<Scalar>::epsilon())*Point3Ops::Distance(a.pos,y);
            for(std::size_t r=0;same&&r<roots.size();++r)
                same=ManifoldSolver::SameExtendedRoot(roots[r],again[r],tol,true)
                    &&ManifoldSolver::SameExtendedRoot(roots[r],viaOther[r],tol,true);
            if(!same) Check(false,spec.label+": canonical roots identical across repeat, emitter-normal sign, N and budget");
            rootCounts[unsigned(roots.size())]++;
            {
                std::string sig;
                for(const auto& v:topology.vertices) sig+=v.geometry.isReflection?'R':'T';
                if(sig.size()>8) sig=sig.substr(0,8)+"..";
                topologyCounts[sig+(roots.empty()?"/0":"/+")]++;
            }
            // Every owned root re-classifies as owned from PT's record of it.
            for(const auto& r:roots) {
                SMSChainRecord rec;
                if(!RecordFromPositions(f,a,Positions(r),y,rec)) {++recordFailures;continue;}
                const bool o1=solver->ExtendedEmitterHitOwned(rec,*luminary,y,light.normal,f.Scene(),*caster,domain);
                const bool o2=solver2->ExtendedEmitterHitOwned(rec,*luminary,y,-light.normal,f.Scene(),*caster,domain);
                owned+=o1;
                if(!o1||!o2) Check(false,spec.label+": every canonical root is owned when PT records it (both configs)");
                // Another domain's record is answered in ITS domain.
                SMSChainRecord legacy=rec;legacy.extendedAnchor=false;
                if(solver->ExtendedEmitterHitOwned(legacy,*luminary,y,light.normal,f.Scene(),*caster,domain))
                    Check(false,"a legacy-mode anchor record is never extended-owned");
                SMSChainRecord broken=rec;broken.broken=true;
                if(solver->ExtendedEmitterHitOwned(broken,*luminary,y,light.normal,f.Scene(),*caster,domain))
                    Check(false,"a broken record is never owned");
            }
            // Random-seed proposal roots (no ownership filter): owned exactly
            // when the canonical set of THEIR topology contains them.
            for(unsigned p=0;p<2;++p) {
                SMSDomainRoot root=solver->ProposeExtendedRoot(a.pos,a.shadingNormal,y,f.Scene(),
                    a.stack,domain,sampler,nullRasterizerState);
                if(!root.accepted || root.result.specularChain.empty()) continue;
                const auto& last=root.result.specularChain.back();
                Vector3 d=Vector3Ops::mkVector3(y,last.position);
                if(Vector3Ops::NormalizeMag(d)<=0) continue;
                Ray ray(last.position,d);ray.Advance(1e-8);RayIntersection hit(ray,nullRasterizerState);
                f.Scene().GetObjects()->IntersectRay(hit,true,true,false);
                if(!hit.geometric.bHit||hit.pObject!=luminary) continue;
                ++proposals;
                std::vector<SMSDomainRoot> canon;
                SMSDomainRoot topo(domain,root.startingStack);topo.vertices=root.vertices;
                solver->CanonicalExtendedRoots(a.pos,a.shadingNormal,*luminary,y,light.normal,
                    f.Scene(),topo,nullRasterizerState,canon);
                bool member=false;
                for(const auto& r:canon) member=member||ManifoldSolver::SameExtendedRoot(r,root,tol,true);
                SMSChainRecord rec;
                if(!RecordFromPositions(f,a,Positions(root),y,rec)) {++recordFailures;continue;}
                const bool o=solver->ExtendedEmitterHitOwned(rec,*luminary,y,light.normal,f.Scene(),*caster,domain);
                proposalOwned+=o;
                if(o!=member) {
                    ++mismatch;
                    std::cout<<"  MISMATCH owned="<<o<<" member="<<member<<" canon="<<canon.size()<<"\n";
                }
            }
        }
    }
    std::cout<<spec.label<<" predicate walks="<<walks<<" ownedRecords="<<owned<<" proposals="<<proposals
        <<" proposalOwned="<<proposalOwned<<" mismatch="<<mismatch<<" recordFailures="<<recordFailures
        <<" canonicalSolves="<<counters.canonicalSolves<<" canonicalRoots="<<counters.canonicalRoots
        <<" samplerDraws="<<counters.canonicalSamplerDraws;
    for(const auto& [k,v]:rootCounts) std::cout<<" |O|="<<k<<":"<<v;
    for(const auto& [k,v]:topologyCounts) std::cout<<" "<<k<<":"<<v;
    std::cout<<"\n";
    Check(walks>0,spec.label+": proposal walks reach the emitter");
    if(expectOwned) Check(owned>0,spec.label+": the canonical predicate owns roots");
    Check(counters.canonicalSamplerDraws==0,spec.label+": canonical predicate draws no random number");
    Check(mismatch==0,spec.label+": owned(record of proposal root) == membership in canonical O(T,y)");
    Check(recordFailures==0,spec.label+": PT-style records rebuild for every root");
    solver->release();solver2->release();caster->release();shader->release();
}

static void PredicateFilters()
{
    const SceneSpec spec=MirrorScene(true,false,EmitterFacing::TowardCaster);
    Fixture f(spec.text);
    std::vector<IShaderOp*> ops;IShader* shader=nullptr;
    if(!f.Ok()||!RISE_API_CreateStandardShader(&shader,ops)||!shader) {Check(false,"filter fixture");return;}
    auto* caster=new RayCaster(false,16,*shader,true);caster->AttachScene(&f.Scene());
    ManifoldSolverConfig cfg;cfg.enabled=true;cfg.extendedMode=true;
    auto* solver=new ManifoldSolver(cfg);
    Anchor a;MakeAnchor(f,spec.camera,a);
    const IObject* luminary=f.Object("emitter");
    // Analytic root for y = (1.5,0,1): image (1.5,0,3), mirror point 2/3 along.
    const Point3 y(1.5,0,1),m(1.0,0,2);
    const auto domain=SMSQueryDomain::RGB(1);
    SMSChainRecord rec;
    Check(RecordFromPositions(f,a,{m},y,rec),"analytic mirror record");
    Check(solver->ExtendedEmitterHitOwned(rec,*luminary,y,Vector3(0,0,1),f.Scene(),*caster,domain),
        "the analytic planar-mirror chain is owned");
    SMSChainRecord tooLong=rec;tooLong.count=SMSChainRecord::kMaxVertices+1;
    Check(!solver->ExtendedEmitterHitOwned(tooLong,*luminary,y,Vector3(0,0,1),f.Scene(),*caster,domain),
        "a chain beyond the 16-vertex record bound stays with PT");
    Check(!solver->ExtendedEmitterHitOwned(rec,*luminary,Point3(2.5,0,1),Vector3(0,0,1),f.Scene(),*caster,domain),
        "an emitter point off the luminary is not owned");
    // The sampler's 1e-5 push of a single-sided plane maps back to the surface.
    const Point3 pushed=ManifoldSolver::ExtendedLuminaryPoint(*luminary,Point3(1.5,0,1.00001),Vector3(0,0,1));
    Check(std::fabs(pushed.z-1)<1e-12,"the partition's emitter point is projected onto the luminary");
    ManifoldSolverConfig off=cfg;off.extendedMode=false;auto* legacy=new ManifoldSolver(off);
    Check(!legacy->ExtendedEmitterHitOwned(rec,*luminary,y,Vector3(0,0,1),f.Scene(),*caster,domain),
        "extended mode off owns nothing");
    ManifoldSolverConfig two=cfg;two.targetBounces=2;auto* targeted=new ManifoldSolver(two);
    Check(!targeted->ExtendedEmitterHitOwned(rec,*luminary,y,Vector3(0,0,1),f.Scene(),*caster,domain),
        "sms_target_bounces filters ownership");
    ManifoldSolverConfig one=cfg;one.targetBounces=1;one.maxChainDepth=1;auto* exact=new ManifoldSolver(one);
    Check(exact->ExtendedEmitterHitOwned(rec,*luminary,y,Vector3(0,0,1),f.Scene(),*caster,domain),
        "a chain within sms_max_chain_depth and the target is owned");
    ManifoldSolverConfig photons=cfg;photons.photonCount=1000;auto* withPhotons=new ManifoldSolver(photons);
    // Eligibility is recorded on the anchor; the PT predicate also refuses
    // a configuration the area estimator refuses (photons are excluded).
    RayIntersection hit(spec.camera,nullRasterizerState);f.Scene().GetObjects()->IntersectRay(hit,true,true,false);
    Check(!withPhotons->ExtendedAnchorEligible(f.Scene(),*caster,a.pos,a.stack),
        "a photon configuration makes the anchor ineligible (all three switches stay legacy)");
    withPhotons->release();legacy->release();targeted->release();exact->release();solver->release();caster->release();shader->release();
}
#endif

//////////////////////////////////////////////////////////////////////
// render: partition checks on point renders
//////////////////////////////////////////////////////////////////////
static unsigned g_saltBase=61000;

enum class Owned { Required, Forbidden, Reported };

// Per-salt series of the reported quantity: the channel average for an
// achromatic scene, else each channel separately.
static std::vector<std::vector<double>> Channels(const PointResult& r,bool perChannel,unsigned channels)
{
    std::vector<std::vector<double>> out;
    if(perChannel||channels==1) {for(unsigned c=0;c<channels;++c) out.push_back(r.c[c]);return out;}
    std::vector<double> avg;
    for(std::size_t i=0;i<r.c[0].size();++i) avg.push_back((r.c[0][i]+r.c[1][i]+r.c[2][i])/3);
    out.push_back(avg);
    return out;
}

static void PartitionCase(const SceneSpec& spec,RenderOptions o,Owned owned=Owned::Required,bool expectKept=false,
    bool perChannel=false)
{
    o.saltBase=g_saltBase;g_saltBase+=100;
    SMSReferenceCounters counters;
    const auto ref=RenderPoint(spec,Mode::Ref,o);
    RenderOptions fo=o;fo.saltBase=o.saltBase+50;fo.counters=&counters;
    const auto full=RenderPoint(spec,Mode::Full,fo);
    if(!ref.ok||!full.ok) {Check(false,spec.label+": renders complete");return;}
    const unsigned channels=o.nm>0?1:3;
    const auto R=Channels(ref,perChannel,channels), F=Channels(full,perChannel,channels);
#ifdef RISE_SMS_EXTENDED_PARTITION
    RenderOptions ko=o;ko.saltBase=o.saltBase+50;
    const auto kept=RenderPoint(spec,Mode::Kept,ko);
    if(!kept.ok) {Check(false,spec.label+": kept render completes");return;}
    const auto K=Channels(kept,perChannel,channels);
#endif
    std::ostringstream tag;tag<<spec.label<<" nm="<<o.nm<<" N="<<o.trials<<" seeding="<<int(o.seeding)<<" target="<<o.targetBounces;
    for(std::size_t c=0;c<R.size();++c) {
        const Moments r(R[c]),fu(F[c]);
        std::cout<<std::setprecision(8)<<tag.str()<<(R.size()>1?" c="+std::to_string(c):std::string(" avg"))
            <<" ref="<<r.mean<<"+-"<<r.se<<" full="<<fu.mean<<"+-"<<fu.se<<" full/ref="<<fu.mean/r.mean;
        Check(std::isfinite(fu.mean)&&std::isfinite(r.mean),tag.str()+": finite");
        Check(Agree(fu,r),tag.str()+": extended full render = SMS-off PT within 3 combined se");
        if(spec.closedForm>=0) {
            std::cout<<" closedForm="<<spec.closedForm;
            Check(std::fabs(fu.mean-spec.closedForm)<=3*fu.se+1e-3*spec.closedForm,tag.str()+": full = closed form within 3 se");
            Check(std::fabs(r.mean-spec.closedForm)<=3*r.se+1e-3*spec.closedForm,tag.str()+": SMS-off PT = closed form within 3 se");
        }
#ifdef RISE_SMS_EXTENDED_PARTITION
        const Moments k(K[c]);
        const Moments smsOwned(Diff(F[c],K[c])); // paired: identical PT paths
        const double ptOwned=r.mean-k.mean, ptOwnedSe=std::hypot(r.se,k.se);
        std::cout<<" kept="<<k.mean<<"+-"<<k.se<<" smsOwned="<<smsOwned.mean<<"+-"<<smsOwned.se
            <<" ptOwned="<<ptOwned<<"+-"<<ptOwnedSe;
        Check(std::fabs(smsOwned.mean-ptOwned)<=3*std::hypot(smsOwned.se,ptOwnedSe),
            tag.str()+": SMS-owned (full-kept) = PT-owned (ref-kept) within 3 combined se");
        if(owned==Owned::Required) Check(smsOwned.mean>3*smsOwned.se&&smsOwned.mean>0,tag.str()+": SMS owns a resolvable share");
        if(owned==Owned::Forbidden) Check(smsOwned.mean==0,tag.str()+": SMS owns nothing here");
        if(owned==Owned::Reported) std::cout<<" (owned share reported, not gated)";
        if(expectKept) Check(k.mean>3*k.se,tag.str()+": PT keeps a resolvable share");
#endif
        std::cout<<" n="<<o.salts<<" spp="<<o.N<<"\n";
    }
    PrintCounters(tag.str(),counters);
}

static double MirrorClosedForm(const SceneSpec& spec,double scale=1,Scalar nm=0)
{
    // L = (1/pi) * Le * Int over the emitter's mirror image (z = 3s) of
    // cos_x cos_y / r^2, receiver albedo 1, mirror reflectance 1; the
    // integral is scale-invariant (a solid-angle quantity).
    Fixture f(spec.text);
    const IObject* e=f.Object("emitter");
    if(!e||!e->GetMaterial()||!e->GetMaterial()->GetEmitter()) return -1;
    RayIntersection hit(Ray(Point3(1.5*scale,0,2*scale),Vector3(0,0,-1)),nullRasterizerState);
    e->IntersectRay(hit,RISE_INFINITY,true,true,false);
    if(!hit.geometric.bHit) return -1;
    const IEmitter* emitter=e->GetMaterial()->GetEmitter();
    const double le=nm>0?emitter->emittedRadianceNM(hit.geometric,Vector3(0,0,1),Vector3(0,0,1),nm)
        :emitter->emittedRadiance(hit.geometric,Vector3(0,0,1),Vector3(0,0,1))[0];
    const unsigned n=1000;double sum=0;
    for(unsigned i=0;i<n;++i) for(unsigned j=0;j<n;++j) {
        const double x=1+(i+.5)/n, y=-.5+(j+.5)/n, r2=x*x+y*y+9;
        sum+=9/(r2*r2);
    }
    return le/PI*sum/(double(n)*n);
}

static void RenderSection()
{
    RenderOptions o;o.salts=16;o.N=g_quick?1024:4096;
    // Planar mirror: real double-sided indexed mesh (both windings, which
    // swaps the incidence side) and the clipped-plane control, against a
    // closed form; scale 1000 and 1/1000 move nothing.
    for(bool reverse:{false,true}) {
        auto spec=MirrorScene(true,reverse,EmitterFacing::TowardCaster);spec.closedForm=MirrorClosedForm(spec);
        PartitionCase(spec,o);
    }
    {
        auto spec=MirrorScene(false,false,EmitterFacing::TowardCaster);spec.closedForm=MirrorClosedForm(spec);
        PartitionCase(spec,o);
    }
    // At 1/1000 the native solver accepts no root (an absolute floor
    // inside the Phase 1/2 solve, measured); the partition then gives
    // every path to PT and stays exact, which is what is gated there.
    for(double scale:{1000.0,0.001}) {
        auto spec=MirrorScene(true,false,EmitterFacing::TowardCaster,scale);spec.closedForm=MirrorClosedForm(spec,scale);
        PartitionCase(spec,o,scale<1?Owned::Reported:Owned::Required);
    }
    // Emitter sidedness: facing away (no caustic, direct only), double-sided.
    PartitionCase(MirrorScene(true,false,EmitterFacing::AwayFromCaster),o,Owned::Forbidden);
    PartitionCase(MirrorScene(true,true,EmitterFacing::DoubleSided),o);
    // Trial budget: N = 1 and N = 4 estimate the same owned set.
    for(unsigned trials:{1u,4u}) {
        auto spec=MirrorScene(true,false,EmitterFacing::TowardCaster);spec.closedForm=MirrorClosedForm(spec);
        RenderOptions t=o;t.trials=trials;PartitionCase(spec,t);
    }
    // NM.
    {
        auto spec=MirrorScene(true,true,EmitterFacing::TowardCaster);spec.closedForm=MirrorClosedForm(spec,1,550);
        RenderOptions nm=o;nm.nm=550;PartitionCase(spec,nm);
    }
    // Closed glass slab, both windings; transformed instance; per-component
    // RGB indices (PT's dispersive RGB split needs more samples) and NM.
    for(bool reverse:{false,true}) PartitionCase(SlabScene(reverse,"glass",false),o,Owned::Required,true);
    PartitionCase(SlabScene(true,"glass",true),o,Owned::Required,true);
    {RenderOptions p=o;p.N=g_quick?4096:16384;PartitionCase(SlabScene(false,"prism",false),p,Owned::Required,true,true);}
    {RenderOptions p=o;p.nm=450;PartitionCase(SlabScene(false,"prism",false),p,Owned::Required,true);}
    // Sphere control; start inside a closed glass box, both windings.
    PartitionCase(SphereScene(),o,Owned::Required,true);
    for(bool reverse:{false,true}) PartitionCase(InsideScene(reverse),o);
    // Delegations: SSS receiver (BSDF anchor + BSSRDF re-entry), RGB and
    // NM; an SPF-only wrapped caster SMS can never own.
    {
        RenderOptions r=o;r.reentryShaderOp=true;
        PartitionCase(SSSReceiverScene(),r,Owned::Reported);
        RenderOptions nm=r;nm.nm=600;PartitionCase(SSSReceiverScene(),nm,Owned::Reported);
    }
    PartitionCase(WrappedCasterScene(),o,Owned::Forbidden);
}

//////////////////////////////////////////////////////////////////////
// switches: legacy inertness and the shared-path diagnostic
//////////////////////////////////////////////////////////////////////
static void SwitchSection()
{
    // HWSS ignores extended mode: bit-identical to the legacy solver.
    {
        RenderOptions h;h.nm=550;h.hwss=true;h.N=1024;h.salts=2;h.saltBase=90000;
        const auto spec=SlabScene(false,"glass",false);
        const auto ext=RenderPoint(spec,Mode::Full,h);
        const auto leg=RenderPoint(spec,Mode::Legacy,h);
        bool identical=ext.ok&&leg.ok;
        for(unsigned s=0;identical&&s<ext.c[0].size();++s) identical=ext.c[0][s]==leg.c[0][s];
        Check(identical,"HWSS render is bit-identical with extended mode on and off");
    }
    // A forced-legacy loop (the HWSS NM hand-off's flag) is legacy too:
    // the RGB extended-mode-off render equals the legacy solver's.
    {
        RenderOptions l;l.N=1024;l.salts=2;l.saltBase=90500;
        const auto spec=MirrorScene(true,false,EmitterFacing::TowardCaster);
        const auto a=RenderPoint(spec,Mode::Legacy,l), b=RenderPoint(spec,Mode::Legacy,l);
        bool same=a.ok&&b.ok;
        for(unsigned c=0;same&&c<3;++c) for(unsigned s=0;same&&s<a.c[c].size();++s) same=a.c[c][s]==b.c[c][s];
        Check(same,"legacy renders are deterministic under a fixed salt (precondition of the identity checks)");
    }
#ifdef RISE_SMS_EXTENDED_PARTITION
    // Kept and full renders share every PT path: where SMS owns nothing
    // (emitter facing away) they are bit-identical.
    {
        RenderOptions k;k.N=1024;k.salts=2;k.saltBase=91000;
        const auto away=MirrorScene(true,false,EmitterFacing::AwayFromCaster);
        const auto a=RenderPoint(away,Mode::Full,k), b=RenderPoint(away,Mode::Kept,k);
        bool same=a.ok&&b.ok;
        for(unsigned c=0;same&&c<3;++c) for(unsigned s=0;same&&s<a.c[c].size();++s) same=a.c[c][s]==b.c[c][s];
        Check(same,"dropping SMS's area deposit changes nothing where SMS owns nothing (shared PT paths)");
    }
#endif
}

//////////////////////////////////////////////////////////////////////
// fixtures: shipped-defect scenes (DL-372, DL-379, DL-336, DL-376, DL-421)
//////////////////////////////////////////////////////////////////////
namespace
{
    std::string BallLensScene(bool dielectric)
    {
        // WeaveGapShadowTransmittanceTest's DL-372 / DL-379 split fixture.
        std::ostringstream ss;
        ss << "RISE ASCII SCENE 7\nstandard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
              "film\n{\n\twidth 24\n\theight 24\n}\n\n"
              "pinhole_camera\n{\n\tlocation 0 4 8\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 6.0\n}\n\n"
              "uniformcolor_painter\n{\n\tname pnt_floor\n\tcolor 0.7 0.7 0.7\n}\n\n"
              "uniformcolor_painter\n{\n\tname pnt_light\n\tcolor 1.0 1.0 1.0\n}\n\n"
              "uniformcolor_painter\n{\n\tname pnt_glass_tau\n\tcolor 0.999 0.999 0.999\n}\n\n"
              "lambertian_material\n{\n\tname floor_mat\n\treflectance pnt_floor\n}\n\n"
              "lambertian_luminaire_material\n{\n\tname light_mat\n\texitance pnt_light\n\tscale 500.0\n\tmaterial none\n}\n\n";
        if( dielectric ) ss << "dielectric_material\n{\n\tname glass_mat\n\ttau 0.999\n\tior 1.5\n\tscattering 100000\n}\n\n";
        else ss << "perfectrefractor_material\n{\n\tname glass_mat\n\trefractance pnt_glass_tau\n\tior 1.5\n}\n\n";
        ss << "sphere_geometry\n{\n\tname sphere_geom\n\tradius 1.0\n}\n\n"
              "clippedplane_geometry\n{\n\tname floor_geom\n\tpta -5.0 0.0 -5.0\n\tptb -5.0 0.0 5.0\n\tptc 5.0 0.0 5.0\n\tptd 5.0 0.0 -5.0\n}\n\n"
              "clippedplane_geometry\n{\n\tname light_geom\n\tpta -1.5 0.0 -1.5\n\tptb 1.5 0.0 -1.5\n\tptc 1.5 0.0 1.5\n\tptd -1.5 0.0 1.5\n}\n\n"
              "standard_object\n{\n\tname floor\n\tgeometry floor_geom\n\tmaterial floor_mat\n}\n\n"
              "standard_object\n{\n\tname glass_sphere\n\tgeometry sphere_geom\n\tposition 0 1.5 0\n\tmaterial glass_mat\n}\n\n"
              "standard_object\n{\n\tname area_light\n\tgeometry light_geom\n\tposition 0 5.0 0\n\tmaterial light_mat\n}\n\n";
        return ss.str();
    }
    // Mean of (r+g+b)/3 over the image, extended PT with an internal config.
    double ExtendedImage(const std::string& text,unsigned spp,unsigned salt,bool extended,bool drop,
        SMSReferenceCounters* counters) {
        Fixture fixture(text);
        if(!fixture.Ok()) return -1;
        std::vector<IShaderOp*> ops;IShader* shader=nullptr;
        if(!RISE_API_CreateStandardShader(&shader,ops)) return -1;
        auto* caster=new RayCaster(false,16,*shader,true);
        caster->AttachScene(&fixture.Scene());
        ManifoldSolverConfig cfg;cfg.enabled=true;cfg.extendedMode=extended;cfg.biased=true;
        cfg.maxBernoulliTrials=64;cfg.multiTrials=2;cfg.referenceCounters=counters;
#ifdef RISE_SMS_EXTENDED_PARTITION
        cfg.extendedDropAreaContributions=drop;
#else
        (void)drop;
#endif
        StabilityConfig stability;stability.rrMinDepth=8;
        auto* rasterizer=new PathTracingPelRasterizer(caster,cfg,PathGuidingConfig(),AdaptiveSamplingConfig(),stability,false);
        rasterizer->SetInteractiveDenoiseSuppressed(true);
        ISampling2D* samples=nullptr;IPixelFilter* filter=nullptr;
        RISE_API_CreateMultiJitteredSampling2D(&samples,1,1);
        RISE_API_CreateBoxPixelFilter(&filter,1,1);
        double mean=-1;
        if(samples&&filter) {
            samples->SetNumSamples(spp);rasterizer->SubSampleRays(samples,filter);
            auto* capture=new CapturingRasterizerOutput();rasterizer->AddRasterizerOutput(capture);
            rasterizer->AttachToScene(&fixture.Scene());
            SobolSamplerTestHooks::ValueSalt().store(salt);
            rasterizer->RasterizeScene(fixture.Scene(),nullptr,nullptr);
            SobolSamplerTestHooks::ValueSalt().store(0);
            double sum=0;
            for(const auto& p:capture->pixels) sum+=(p.base.r+p.base.g+p.base.b)*p.a/3;
            if(!capture->pixels.empty()) mean=sum/double(capture->pixels.size());
            rasterizer->DetachFromScene(&fixture.Scene());
            capture->release();
        }
        safe_release(samples);safe_release(filter);rasterizer->release();caster->release();shader->release();
        return mean;
    }
    std::string RasterizerPT(unsigned spp) {
        return "pathtracing_pel_rasterizer\n{\n\tsamples "+std::to_string(spp)
            +"\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n\tsms_enabled FALSE\n}\n\n";
    }
    std::string RasterizerVCM(unsigned spp) {
        return "vcm_pel_rasterizer\n{\n\tsamples "+std::to_string(spp)
            +"\n\tmax_eye_depth 8\n\tmax_light_depth 8\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled true\n"
              "\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
    }
}

static void BallLensFixture(bool dielectric)
{
    const std::string label=dielectric?"DL-379 ball lens dielectric scattering 1e5":"DL-372 ball lens perfect refractor";
    const unsigned spp=g_quick?16:64, n=8;
    std::vector<double> ext,legacy,kept,pt,vcm,extSeconds,legacySeconds;
    SMSReferenceCounters counters;
    for(unsigned i=0;i<n;++i) {
        const unsigned seed=73000+i;const unsigned salt=SobolSequence::HashCombine(seed,kSaltTag);
        auto t0=std::chrono::steady_clock::now();
        ext.push_back(ExtendedImage(BallLensScene(dielectric),spp,salt,true,false,&counters));
        auto t1=std::chrono::steady_clock::now();
        legacy.push_back(ExtendedImage(BallLensScene(dielectric),spp,salt,false,false,nullptr));
        auto t2=std::chrono::steady_clock::now();
        extSeconds.push_back(std::chrono::duration<double>(t1-t0).count());
        legacySeconds.push_back(std::chrono::duration<double>(t2-t1).count());
#ifdef RISE_SMS_EXTENDED_PARTITION
        kept.push_back(ExtendedImage(BallLensScene(dielectric),spp,salt,true,true,nullptr));
#endif
        g_seedBase=seed;g_renderIndex=0;
        const auto a=Render(BallLensScene(dielectric)+RasterizerPT(spp),"ball_pt");
        g_seedBase=seed+500;g_renderIndex=0;
        const auto b=Render(BallLensScene(dielectric)+RasterizerVCM(spp),"ball_vcm");
        Check(a.ok&&b.ok,label+": reference renders complete");
        pt.push_back(a.mean);vcm.push_back(b.mean);
    }
    const Moments e(ext),l(legacy),p(pt),v(vcm);
    std::cout<<std::setprecision(8)<<label<<" extended="<<e.mean<<"+-"<<e.se<<" legacySMS="<<l.mean<<"+-"<<l.se
        <<" PT(SMS off)="<<p.mean<<"+-"<<p.se<<" VCM="<<v.mean<<"+-"<<v.se
        <<" ext/PT="<<e.mean/p.mean<<" ext/VCM="<<e.mean/v.mean<<" PT/VCM="<<p.mean/v.mean<<" n="<<n<<" spp="<<spp;
#ifdef RISE_SMS_EXTENDED_PARTITION
    const Moments k(kept),own(Diff(ext,kept));
    std::cout<<" kept="<<k.mean<<"+-"<<k.se<<" smsOwnedShare="<<own.mean/e.mean;
#endif
    std::cout<<"\n";
    const Moments es(extSeconds),ls(legacySeconds);
    std::cout<<label<<" wall seconds per image (1 worker): extended="<<es.mean<<"+-"<<es.se
        <<" legacySMS="<<ls.mean<<"+-"<<ls.se<<" ratio="<<es.mean/ls.mean<<"\n";
    PrintCounters(label,counters);
    if(dielectric) {
        // DL-379 stays open: the delta-limit partition's bias is reported,
        // not gated beyond a sanity band.
        std::cout<<label<<" DL-379 measured bias ext/VCM-1="<<(e.mean/v.mean-1)
            <<" +- "<<(e.mean/v.mean)*std::hypot(e.se/e.mean,v.se/v.mean)<<" (reported, not gated)\n";
        Check(std::fabs(e.mean/v.mean-1)<0.2,label+": delta-limit bias within a 20% sanity band");
    } else {
        Check(Agree(e,p),label+": extended image = PT SMS-off within 3 combined se");
    }
}

static SceneSpec BallLensPoint(bool dielectric,double x,double z)
{
    SceneSpec s;
    std::ostringstream label;label<<(dielectric?"DL-379":"DL-372")<<" ball lens point ("<<x<<",0,"<<z<<")";
    s.label=label.str();
    s.text=BallLensScene(dielectric);
    const Point3 target(x,0,z), eye(0,4,8);
    Vector3 d=Vector3Ops::mkVector3(target,eye);Vector3Ops::NormalizeMag(d);
    s.camera=Ray(eye,d);
    return s;
}

static void FixtureSection()
{
    BallLensFixture(false);
    BallLensFixture(true);
    RenderOptions o;o.salts=16;o.N=g_quick?1024:8192;
    // DL-336: the weak lens in a constant 1.4 enclosure, and its air control.
    PartitionCase(ImmersedBallScene(false),o);
    PartitionCase(ImmersedBallScene(true),o);
    // DL-376 / DL-421: the extended estimator ignores the legacy seeding
    // mode; uniform seeding with a bounce target reads the same as snell.
    {RenderOptions u=o;u.seeding=ManifoldSolverConfig::eSeedingUniform;u.targetBounces=2;
     PartitionCase(SlabScene(false,"glass",false),u,Owned::Required,true);}
    {RenderOptions u=o;u.seeding=ManifoldSolverConfig::eSeedingUniform;u.biased=false;
     PartitionCase(SphereScene(),u,Owned::Required,true);}
}

int main(int argc,char** argv)
{
    std::string section="all";
    for(int i=1;i<argc;++i) {
        const std::string arg=argv[i];
        if(arg=="--section"&&i+1<argc) section=argv[++i];
        else if(arg=="--quick") g_quick=true;
    }
    Check(ConfigureTestWorker(),"partition test uses one configured worker");
#ifndef RISE_SMS_EXTENDED_PARTITION
    std::cout<<"NOTE: RISE_SMS_EXTENDED_PARTITION absent: partition-API checks compiled out\n";
#endif
    if(section=="all"||section=="synthetic") SyntheticTopology();
#ifdef RISE_SMS_EXTENDED_PARTITION
    if(section=="all"||section=="predicate") {
        PredicateFilters();
        for(bool reverse:{false,true}) PredicateOn(MirrorScene(true,reverse,EmitterFacing::TowardCaster),200);
        for(bool reverse:{false,true}) PredicateOn(MirrorScene(true,reverse,EmitterFacing::DoubleSided),200);
        PredicateOn(MirrorScene(false,false,EmitterFacing::TowardCaster),100);
        PredicateOn(MirrorScene(true,false,EmitterFacing::TowardCaster,1000),100);
        for(bool reverse:{false,true}) PredicateOn(SlabScene(reverse,"glass",false),200);
        PredicateOn(SlabScene(false,"prism",true),200);
        PredicateOn(SphereScene(),200);
        for(bool reverse:{false,true}) PredicateOn(InsideScene(reverse),200);
        PredicateOn(ImmersedBallScene(false),400);
    }
#endif
    if(section=="ballpoint") {
        RenderOptions o;o.salts=16;o.N=g_quick?1024:4096;
        for(const auto& p:{std::array<double,2>{0,0},std::array<double,2>{0.3,0.2},std::array<double,2>{0.8,0}})
            PartitionCase(BallLensPoint(false,p[0],p[1]),o,Owned::Reported,false);
    }
    if(section=="delegation") {
        RenderOptions o;o.salts=16;o.N=g_quick?1024:4096;
        RenderOptions r=o;r.reentryShaderOp=true;
        PartitionCase(SSSReceiverScene(),r,Owned::Reported);
        {RenderOptions nm=r;nm.nm=600;PartitionCase(SSSReceiverScene(),nm,Owned::Reported);}
        PartitionCase(WrappedCasterScene(),o,Owned::Forbidden);
    }
    if(section=="focus") {
        RenderOptions o;o.salts=64;o.N=4096;o.saltBase=777000;
        const auto spec=BallLensPoint(false,0,0);
        const auto full=RenderPoint(spec,Mode::Full,o);
        std::vector<double> avg;for(std::size_t i=0;i<full.c[0].size();++i) avg.push_back((full.c[0][i]+full.c[1][i]+full.c[2][i])/3);
        const Moments m(avg);double mx=0,mn=1e30;for(double v:avg){mx=std::max(mx,v);mn=std::min(mn,v);}
        std::cout<<"focus full mean="<<m.mean<<" se="<<m.se<<" sd="<<m.sd<<" min="<<mn<<" max="<<mx<<" n=64\n";
    }
#ifdef RISE_SMS_EXTENDED_PARTITION
    if(section=="tiny") PredicateOn(MirrorScene(true,false,EmitterFacing::TowardCaster,0.001),100);
#endif
    if(section=="all"||section=="render") RenderSection();
    if(section=="all"||section=="switches") SwitchSection();
    if(section=="all"||section=="fixtures") FixtureSection();
    std::cout<<"SMSExtendedPartitionTest: "<<passCount<<" passed, "<<failCount<<" failed\n";
    return failCount?1:0;
}
