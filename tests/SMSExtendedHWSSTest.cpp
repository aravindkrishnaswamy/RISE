// Extended SMS Phase 4: HWSS lane geometry and ownership.
//
// Every check renders through the production PT entry points
// (PathTracingIntegrator::IntegrateRayHWSS / IntegrateRayNM) on one camera
// ray with a FIXED wavelength bundle, so each HWSS lane w has an exact
// per-wavelength reference: an independent extended NM render at
// swl.lambda[w].  Per lane (no termination, see below):
//   full == ref         extended HWSS = SMS-off HWSS (the partition)
//   SMS-owned == PT-owned   (full - kept, paired) = (ref - kept)
//   full == NM(lambda_w)    the lane evaluates in its OWN NM domain
//   SMS-owned == NM SMS-owned(lambda_w)
// `kept` zeroes estimator B's deposit (SMSExtendedTestHooks) and shares
// every PT path with `full`.
//
// Sections (all by default; `--section <name>` runs one; `--quick` lowers
// the sample counts for a smoke run, never for the recorded gate):
//   lanes   area partition per lane through the NM delegations: planar
//           mirror (double-sided indexed mesh, both windings; clipped-plane
//           control; closed form), glass slab both windings, transformed
//           slab, dispersive slab (distinct companion indices: lanes land
//           on different roots), sphere, start-inside both windings, nested
//           exterior replay (a dispersive ball inside a constant-index
//           enclosure), a dispersive enclosure (camera-inside fallback),
//           SSS receiver (camera-entry delegation)
//   body    DL-378's body: a polished (BSDF + delta coat) caster; its
//           emitter hit through the NM delegation (`material none`
//           luminaire) and in the HWSS body itself (luminaire with a BSDF);
//           double-sided indexed mesh ceiling both windings, clipped-plane
//           control
//   tir     estimator A per lane: a right-angle prism reflecting a point
//           light by its hypotenuse; lanes with n > sqrt(2) are past the
//           critical angle (weight 1), the others are not (Fresnel); both
//           windings, two lane orders (TIR hero / non-TIR hero)
//   mask    per-lane anchor eligibility: an HG dielectric whose scattering
//           is < 1 at one lane's wavelength makes that lane's anchors
//           ineligible (all three switches off for that lane only);
//           delegated and body emitter hits
//   delta   estimator A per lane through a dispersive slab; DL-344's
//           shadow switch per lane (transparent shadows on/off identical)
//   modeoff a forced-legacy scope (the HWSS shader-op path) is bit-identical
//           to extended mode off; extended off is deterministic
//
// Termination: a dispersive delta vertex at a BSDF material terminates the
// companions (existing HWSS rule); the per-lane NM equality holds only for
// paths without termination, so every case reports the termination count
// and the lane comparisons gate only the cases where it is zero.
#include "SMSRenderTestSupport.h"
#include "../src/Library/Utilities/ManifoldSolver.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IEmitter.h"
#include "../src/Library/Shaders/PathTracingIntegrator.h"
#include "../src/Library/Shaders/PathTracingShaderOp.h"
#include "../src/Library/Utilities/RuntimeContext.h"
#include "../src/Library/Utilities/Color/SampledWavelengths.h"
#include <array>
#include <sstream>
#include <iomanip>
#include <chrono>

namespace
{
    bool g_quick=false;
    const unsigned kLanes=SampledWavelengths::N;
    using Lanes=std::array<Scalar,SampledWavelengths::N>;

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
    bool Agree(const Moments& a,const Moments& b,double k=3) {
        return std::fabs(a.mean-b.mean)<=k*std::hypot(a.se,b.se);
    }
    std::vector<double> Diff(const std::vector<double>& a,const std::vector<double>& b) {
        std::vector<double> d;for(std::size_t i=0;i<a.size()&&i<b.size();++i) d.push_back(a[i]-b[i]);return d;
    }

    struct DropScope {
        bool previous=false;
        explicit DropScope(bool on) { previous=SMSExtendedTestHooks::DropAreaContributions().exchange(on); }
        ~DropScope() { SMSExtendedTestHooks::DropAreaContributions().store(previous); }
    };

    struct Fixture {
        IJobPriv* job=nullptr;
        explicit Fixture(const std::string& text) {
            const auto path=TestTempPath("sms_hwss_"+std::to_string(::getpid())+".RISEscene");
            {std::ofstream out(path);out<<text;}
            if(RISE_CreateJobPriv(&job)) Check(job->LoadAsciiSceneViaCst(path.c_str()),"hwss scene parses");
            std::remove(path.c_str());
            if(job) job->GetScene()->GetObjects()->PrepareForRendering();
        }
        ~Fixture() { safe_release(job); }
        bool Ok() const { return job!=nullptr; }
        const IScene& Scene() const { return *job->GetScene(); }
        const IObject* Object(const char* name) const { return Scene().GetObjects()->GetItem(name); }
    };

    // The RGB scalar painters below interpolate linearly between their
    // 450/550/650 nm nodes (RGBScalarPainter): `triple` reads 1.9 at 450,
    // 1.5 at 550 and 1.3 at 650 nm.
    std::string Header() {
        return "RISE ASCII SCENE 7\n"
            "uniformcolor_painter\n{\n name white\n color 1 1 1\n}\n"
            "uniformcolor_painter\n{\n name black\n color 0 0 0\n}\n"
            "uniformcolor_painter\n{\n name light_color\n color 1 1 1\n}\n"
            "scalar_painter\n{\n name flat\n values 1.5 1.5 1.5\n}\n"
            "scalar_painter\n{\n name triple\n values 1.3 1.5 1.9\n}\n"
            "scalar_painter\n{\n name gentle\n values 1.45 1.5 1.6\n}\n"
            "lambertian_material\n{\n name diffuse\n reflectance white\n}\n"
            "perfectreflector_material\n{\n name mirror\n reflectance white\n}\n"
            "perfectrefractor_material\n{\n name glass\n refractance white\n ior flat\n}\n"
            "perfectrefractor_material\n{\n name prism\n refractance white\n ior triple\n}\n"
            "perfectrefractor_material\n{\n name gentleprism\n refractance white\n ior gentle\n}\n"
            "dielectric_material\n{\n name medium14\n ior 1.4\n tau 1.0\n scattering 1000000\n}\n"
            "dielectric_material\n{\n name dispersivemedium\n ior gentle\n tau 1.0\n scattering 1000000\n}\n"
            "lambertian_luminaire_material\n{\n name lum\n exitance light_color\n scale 10\n material none\n}\n"
            "lambertian_luminaire_material\n{\n name lumbsdf\n exitance light_color\n scale 10\n material diffuse\n}\n";
    }
    std::string Obj(const std::string& name,const std::string& geometry,const std::string& material,
        const std::string& extra="") {
        return "standard_object\n{\n name "+name+"\n geometry "+geometry+"\n material "+material+"\n"+extra+"}\n";
    }
    // Horizontal clipped plane: corner order as written gives a -z normal;
    // flip reverses it.
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
    // A clipped plane with four explicit corners (any orientation).
    std::string ClippedCorners(const std::string& name,const std::array<Point3,4>& p,bool doubleSided) {
        std::ostringstream s;s<<std::setprecision(17);
        const char* labels[4]={"pta","ptb","ptc","ptd"};
        s<<"clippedplane_geometry\n{\n name "<<name<<"\n";
        for(int i=0;i<4;++i) s<<" "<<labels[i]<<" "<<p[i].x<<" "<<p[i].y<<" "<<p[i].z<<"\n";
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
    // Closed double-sided indexed-mesh box, both windings.
    std::string MeshBox(const std::string& name,bool reverse,double hx=1,double hy=1,double hz=1) {
        std::ostringstream s;s<<std::setprecision(17);
        s<<"indexedmesh_geometry\n{\n name "<<name<<"\n double_sided TRUE\n face_normals TRUE\n";
        const int p[8][3]={{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
        for(const auto& v:p) s<<" vertex "<<v[0]*hx<<' '<<v[1]*hy<<' '<<v[2]*hz<<'\n';
        for(int i=0;i<8;++i) s<<" uv "<<(i%2)<<' '<<((i/2)%2)<<'\n';
        const int t[12][3]={{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
            {3,7,6},{3,6,2},{0,4,7},{0,7,3},{1,2,6},{1,6,5}};
        for(const auto& f:t) s<<" triangle "<<f[0]<<' '<<f[reverse?2:1]<<' '<<f[reverse?1:2]<<'\n';
        return s.str()+"}\n";
    }
    // Closed double-sided indexed-mesh right-angle prism: cross-section
    // (x,z) = (-1,0), (1,0), (0,1) (hypotenuse on z = 0, legs facing
    // up-left and up-right), extruded over y in [-1,1].  Both windings.
    std::string MeshPrism(const std::string& name,bool reverse) {
        std::ostringstream s;s<<std::setprecision(17);
        s<<"indexedmesh_geometry\n{\n name "<<name<<"\n double_sided TRUE\n face_normals TRUE\n";
        const double v[6][3]={{-1,-1,0},{1,-1,0},{0,-1,1},{-1,1,0},{1,1,0},{0,1,1}};
        for(const auto& p:v) s<<" vertex "<<p[0]<<' '<<p[1]<<' '<<p[2]<<'\n';
        for(int i=0;i<6;++i) s<<" uv "<<(i%2)<<' '<<(i/3)<<'\n';
        // Outward-wound (reverse = false): end caps, hypotenuse, two legs.
        const int t[8][3]={{0,1,2},{3,5,4},{0,4,1},{0,3,4},{0,2,5},{0,5,3},{1,5,2},{1,4,5}};
        for(const auto& f:t) s<<" triangle "<<f[0]<<' '<<f[reverse?2:1]<<' '<<f[reverse?1:2]<<'\n';
        return s.str()+"}\n";
    }

    // ---------------------------------------------------------------
    // Point scenes.  The receiver point is the origin, normal +z; the
    // camera ray starts 0.3 above it and looks straight down unless set.
    // ---------------------------------------------------------------
    struct SceneSpec {
        std::string text;
        Ray camera = Ray(Point3(0,0,0.3),Vector3(0,0,-1));
        unsigned maxDepth = 16;
        std::string label;
        bool closedForm = false;
    };
    // k = 1 planar caster at z = 2 over the receiver; a 1x1 emitter at
    // z = 1, x in [1,2] facing the caster.  `caster` is the material.
    SceneSpec CeilingScene(const std::string& caster,bool mesh,bool reverse,const std::string& lum="lum",
        const std::string& extra="") {
        SceneSpec s;
        s.label="ceiling "+caster+" mesh="+std::to_string(mesh)+" reverse="+std::to_string(reverse)+" emitter="+lum;
        // polished_material is deprecated (a load warning): defined only where used.
        const std::string polished=caster=="polished"
            ? "polished_material\n{\n name polished\n reflectance black\n tau 1.0\n ior 1.5\n scattering 1000000\n}\n" : "";
        s.text=Header()+polished+extra
            +ClippedQuad("receiver_geo",0,-0.5,0.5,-0.5,0.5,false,true)+Obj("receiver","receiver_geo","diffuse")
            +(mesh?MeshQuad("caster_geo",2,0.4,4,-2,2,reverse):ClippedQuad("caster_geo",2,0.4,4,-2,2,reverse,true))
            +Obj("caster","caster_geo",caster)
            +ClippedQuad("emitter_geo",1,1,2,-0.5,0.5,true,false)+Obj("emitter","emitter_geo",lum);
        return s;
    }
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
    SceneSpec SphereScene(const std::string& material) {
        SceneSpec s;s.label="sphere "+material;
        s.text=Header()
            +ClippedQuad("receiver_geo",0,-0.5,0.5,-0.5,0.5,false,true)+Obj("receiver","receiver_geo","diffuse")
            +"sphere_geometry\n{\n name ball_geo\n radius 0.6\n}\n"+Obj("caster","ball_geo",material," position 0.15 0 1.4\n")
            +ClippedQuad("emitter_geo",3.2,-0.7,0.7,-0.7,0.7,false,true)+Obj("emitter","emitter_geo","lum");
        return s;
    }
    SceneSpec InsideScene(bool reverse) {
        SceneSpec s;s.label="inside reverse="+std::to_string(reverse);
        s.text=Header()
            +ClippedQuad("receiver_geo",0,-0.4,0.4,-0.4,0.4,false,true)+Obj("receiver","receiver_geo","diffuse")
            +MeshBox("box_geo",reverse)+Obj("caster","box_geo","glass"," position 0 0 0.5\n scale 1.2 1.2 1\n")
            +ClippedQuad("emitter_geo",2.5,-0.7,0.7,-0.7,0.7,false,true)+Obj("emitter","emitter_geo","lum");
        return s;
    }
    // Nested exterior: camera, receiver, ball and emitter inside an
    // enclosure (constant 1.4 or dispersive); the ball is `ball`.
    SceneSpec ImmersedBallScene(const std::string& enclosure,const std::string& ball) {
        SceneSpec s;s.label="immersed ball enclosure="+enclosure+" ball="+ball;
        s.text=Header()
            +ClippedQuad("receiver_geo",0,-0.5,0.5,-0.5,0.5,false,true)+Obj("receiver","receiver_geo","diffuse")
            +"sphere_geometry\n{\n name ball_geo\n radius 0.15\n}\n"+Obj("caster","ball_geo",ball," position 0.3 0 0.55\n")
            +ClippedQuad("emitter_geo",1,0.5,0.7,-0.1,0.1,false,false)+Obj("emitter","emitter_geo","lum"," position 0 0 0\n")
            +MeshBox("enclosure_geo",false)+Obj("enclosure","enclosure_geo",enclosure," position 0 0 0.5\n scale 3 3 1\n");
        const std::string from="name lum\n exitance light_color\n scale 10";
        s.text.replace(s.text.find(from),from.size(),"name lum\n exitance light_color\n scale 400");
        return s;
    }
    SceneSpec SSSReceiverScene() {
        SceneSpec s;s.label="SSS receiver under slab";
        s.text=Header()
            +"randomwalk_sss_material\n{\n name rw\n ior 1.3\n absorption 0.8 0.4 0.04\n scattering 3 3.5 4\n g 0\n roughness 0.8\n max_bounces 64\n}\n"
            +MeshBox("receiver_geo",false)+Obj("receiver","receiver_geo","rw"," position 0 0 -0.1\n scale 0.5 0.5 0.1\n")
            +MeshBox("slab_geo",false)+Obj("caster","slab_geo","glass"," position 0 0 2\n scale 1.5 1.5 0.25\n")
            +ClippedQuad("emitter_geo",3.5,-0.6,0.6,-0.6,0.6,false,true)+Obj("emitter","emitter_geo","lum");
        return s;
    }
    // Per-lane TIR: a point light up-left of a right-angle prism (dispersive
    // `triple`), a receiver up-right; light enters the left leg, reflects
    // off the hypotenuse at ~45 degrees and leaves through the right leg.
    // A black wall over the apex blocks the direct line.
    SceneSpec TIRPrismScene(bool reverse) {
        SceneSpec s;s.label="TIR prism reverse="+std::to_string(reverse);
        const double r=1.5/std::sqrt(2.0);
        const Point3 rc(0.5+r,0,0.5+r);
        const Vector3 n=Vector3Ops::Normalize(Vector3(-1,0,-1));   // receiver normal, toward the prism
        const Vector3 u(0,1,0), v=Vector3Ops::Normalize(Vector3Ops::Cross(n,u));
        const double h=0.2;
        // Double-sided: the camera on the +n side sees its lit face.
        std::array<Point3,4> corners={Point3Ops::mkPoint3(rc,u*(-h)+v*(-h)),Point3Ops::mkPoint3(rc,u*(-h)+v*h),
            Point3Ops::mkPoint3(rc,u*h+v*h),Point3Ops::mkPoint3(rc,u*h+v*(-h))};
        std::ostringstream light;light<<std::setprecision(17)
            <<"omni_light\n{\n name source\n position "<<-(0.5+r)<<" 0 "<<(0.5+r)<<"\n color 1 1 1\n power 40\n}\n";
        s.text=Header()+"lambertian_material\n{\n name blackmat\n reflectance black\n}\n"
            +ClippedCorners("receiver_geo",corners,true)+Obj("receiver","receiver_geo","diffuse")
            +MeshPrism("prism_geo",reverse)+Obj("caster","prism_geo","prism")
            +ClippedCorners("wall_geo",{Point3(0,-3,1.1),Point3(0,3,1.1),Point3(0,3,4),Point3(0,-3,4)},true)
            +Obj("wall","wall_geo","blackmat")
            +light.str();
        s.camera=Ray(Point3Ops::mkPoint3(rc,n*0.3),-n);
        return s;
    }
    // Per-lane anchor eligibility: an HG dielectric somewhere in the scene
    // with scattering 2/2/0.5 at 450/550/650 nm is a delta caster at
    // wavelengths with s >= 1 and makes every anchor INELIGIBLE where
    // s < 1 (s = 1 at 616.7 nm).
    std::string HGChunks() {
        return "scalar_painter\n{\n name hgscat\n values 0.5 2 2\n}\n"
            "dielectric_material\n{\n name hgglass\n ior 1.5\n tau 1.0\n scattering hgscat\n henyey-greenstein TRUE\n}\n"
            "sphere_geometry\n{\n name hg_geo\n radius 0.2\n}\n"
            "standard_object\n{\n name hgball\n geometry hg_geo\n material hgglass\n position -3 0 3\n}\n";
    }
    SceneSpec DeltaSlabScene(const std::string& material) {
        SceneSpec s;s.label="omni through slab "+material;
        s.text=Header()
            +ClippedQuad("receiver_geo",0,-0.5,0.5,-0.5,0.5,false,true)+Obj("receiver","receiver_geo","diffuse")
            +MeshBox("slab_geo",false)+Obj("caster","slab_geo",material," position 0 0 2\n scale 1.5 1.5 0.25\n")
            +"omni_light\n{\n name source\n position 0.2 0 3.5\n color 1 1 1\n power 40\n}\n";
        return s;
    }

    // L = (1/pi) * Le(nm) * Int over the emitter's mirror image of
    // cos_x cos_y / r^2 (receiver albedo 1, mirror reflectance 1).
    double MirrorClosedForm(const SceneSpec& spec,Scalar nm) {
        Fixture f(spec.text);
        const IObject* e=f.Object("emitter");
        if(!e||!e->GetMaterial()||!e->GetMaterial()->GetEmitter()) return -1;
        RayIntersection hit(Ray(Point3(1.5,0,2),Vector3(0,0,-1)),nullRasterizerState);
        e->IntersectRay(hit,RISE_INFINITY,true,true,false);
        if(!hit.geometric.bHit) return -1;
        const double le=e->GetMaterial()->GetEmitter()->emittedRadianceNM(hit.geometric,Vector3(0,0,1),Vector3(0,0,1),nm);
        const unsigned n=1000;double sum=0;
        for(unsigned i=0;i<n;++i) for(unsigned j=0;j<n;++j) {
            const double x=1+(i+.5)/n, y=-.5+(j+.5)/n, r2=x*x+y*y+9;
            sum+=9/(r2*r2);
        }
        return le/PI*sum/(double(n)*n);
    }

    // ---------------------------------------------------------------
    // Point renders: N samples per salt, n salts.
    // ---------------------------------------------------------------
    enum class Mode { Ref, Full, Kept, Legacy };
    struct RenderOptions {
        Lanes lambdas{{450,530,600,650}};
        unsigned N=2048, salts=8, saltBase=71000, trials=2, targetBounces=0;
        bool transparentShadows=false;
        bool forceLegacyScope=false;     // run inside a forced-legacy scope
        bool reentryShaderOp=false;      // CastRay re-entries shaded by PT
        SMSReferenceCounters* counters=nullptr;
    };
    struct LaneResult {
        std::array<std::vector<double>,SampledWavelengths::N> lane; // per-salt means
        unsigned long long terminatedSamples=0;
        bool ok=false;
    };
    // nmLane < 0: HWSS bundle; else an NM render at lambdas[nmLane] stored
    // in lane[nmLane].
    LaneResult Render(const SceneSpec& spec,Mode mode,const RenderOptions& o,int nmLane=-1) {
        LaneResult out;
        Fixture fixture(spec.text);
        if(!fixture.Ok()) return out;
        ManifoldSolverConfig cfg;cfg.enabled=mode!=Mode::Ref;cfg.extendedMode=mode!=Mode::Legacy;
        cfg.biased=true;cfg.multiTrials=o.trials;cfg.maxBernoulliTrials=64;cfg.targetBounces=o.targetBounces;
        cfg.referenceCounters=o.counters;
        const DropScope drop(mode==Mode::Kept);
        StabilityConfig stability;stability.rrMinDepth=4;
        std::vector<IShaderOp*> ops;IShader* shader=nullptr;
        PathTracingShaderOp* reentry=nullptr;
        if(o.reentryShaderOp) {reentry=new PathTracingShaderOp(cfg,stability);ops.push_back(reentry);}
        if(!RISE_API_CreateStandardShader(&shader,ops)||!shader) {if(reentry) reentry->release();return out;}
        auto* caster=new RayCaster(false,16,*shader,true);caster->SetTransparentShadows(o.transparentShadows);
        caster->AttachScene(&fixture.Scene());
        auto* integrator=new PathTracingIntegrator(cfg,stability);integrator->SetMaxPathDepth(spec.maxDepth);
        if(integrator->GetSolver()) {
            std::vector<const IObject*> casters;ManifoldSolver::EnumerateSpecularCasters(fixture.Scene(),casters);
            integrator->GetSolver()->SetSpecularCasters(casters);
        }
        for(unsigned salt=0;salt<o.salts;++salt) {
            const unsigned value=SobolSequence::HashCombine(o.saltBase+salt,0x48575353u);
            SobolSamplerTestHooks::ValueSalt().store(value);
            std::array<double,SampledWavelengths::N> sum{};
            for(unsigned sample=0;sample<o.N;++sample) {
                SobolSampler sampler(sample,29);
                RandomNumberGenerator random(value+sample);
                RuntimeContext context(random,RuntimeContext::PASS_NORMAL,false);context.pSampler=&sampler;
                const SMSLegacyModeScope scope(context,o.forceLegacyScope);
                if(nmLane<0) {
                    SampledWavelengths swl;
                    for(unsigned w=0;w<kLanes;++w) {swl.lambda[w]=o.lambdas[w];swl.pdf[w]=Scalar(1)/400;swl.terminated[w]=false;}
                    Scalar values[SampledWavelengths::N];
                    integrator->IntegrateRayHWSS(context,nullRasterizerState,spec.camera,swl,fixture.Scene(),*caster,sampler,nullptr,values);
                    if(swl.SecondaryTerminated()) ++out.terminatedSamples;
                    for(unsigned w=0;w<kLanes;++w) sum[w]+=values[w];
                } else {
                    sum[nmLane]+=integrator->IntegrateRayNM(context,nullRasterizerState,spec.camera,o.lambdas[nmLane],
                        fixture.Scene(),*caster,sampler,nullptr,nullptr);
                }
            }
            for(unsigned w=0;w<kLanes;++w) if(nmLane<0||int(w)==nmLane) out.lane[w].push_back(sum[w]/o.N);
        }
        SobolSamplerTestHooks::ValueSalt().store(0);
        integrator->release();caster->release();shader->release();
        if(reentry) reentry->release();
        out.ok=true;
        return out;
    }
    unsigned g_saltBase=71000;

    enum class Owned { Required, Forbidden, Reported };
    struct LaneExpect {
        std::array<Owned,SampledWavelengths::N> owned{{Owned::Required,Owned::Required,Owned::Required,Owned::Required}};
        bool expectNoTermination=true;
        bool closedForm=false;
        // Gate: some pair of lanes' SMS-owned values differ by > 3 combined
        // se (lanes genuinely land on different roots / weights).
        bool expectLanesDiffer=false;
    };

    // The full per-lane partition case (area emitters).
    void LaneCase(const SceneSpec& spec,RenderOptions o,const LaneExpect& e=LaneExpect())
    {
        o.saltBase=g_saltBase;g_saltBase+=1000;
        SMSReferenceCounters counters;
        const auto ref=Render(spec,Mode::Ref,o);
        RenderOptions fo=o;fo.saltBase=o.saltBase+100;fo.counters=&counters;
        const auto full=Render(spec,Mode::Full,fo);
        RenderOptions ko=o;ko.saltBase=o.saltBase+100;
        const auto kept=Render(spec,Mode::Kept,ko);
        if(!ref.ok||!full.ok||!kept.ok) {Check(false,spec.label+": HWSS renders complete");return;}
        std::cout<<spec.label<<" terminated samples ref/full/kept="<<ref.terminatedSamples<<"/"
            <<full.terminatedSamples<<"/"<<kept.terminatedSamples<<" of "<<o.N*o.salts<<"\n";
        if(e.expectNoTermination)
            Check(ref.terminatedSamples==0&&full.terminatedSamples==0,spec.label+": no companion termination (lane references exact)");
        for(unsigned w=0;w<kLanes;++w) {
            RenderOptions nf=o;nf.saltBase=o.saltBase+200+10*w;
            const auto nmFull=Render(spec,Mode::Full,nf,int(w));
            const auto nmKept=Render(spec,Mode::Kept,nf,int(w));
            if(!nmFull.ok||!nmKept.ok) {Check(false,spec.label+": NM renders complete");return;}
            std::ostringstream tag;tag<<spec.label<<" lane="<<w<<" nm="<<o.lambdas[w];
            const Moments r(ref.lane[w]),f(full.lane[w]),k(kept.lane[w]),nf_(nmFull.lane[w]);
            const Moments smsOwned(Diff(full.lane[w],kept.lane[w]));
            const Moments nmOwned(Diff(nmFull.lane[w],nmKept.lane[w]));
            const double ptOwned=r.mean-k.mean, ptOwnedSe=std::hypot(r.se,k.se);
            std::cout<<std::setprecision(8)<<tag.str()<<" ref="<<r.mean<<"+-"<<r.se<<" full="<<f.mean<<"+-"<<f.se
                <<" full/ref="<<f.mean/r.mean<<" kept="<<k.mean<<"+-"<<k.se<<" smsOwned="<<smsOwned.mean<<"+-"<<smsOwned.se
                <<" ptOwned="<<ptOwned<<"+-"<<ptOwnedSe<<" NMfull="<<nf_.mean<<"+-"<<nf_.se
                <<" NMowned="<<nmOwned.mean<<"+-"<<nmOwned.se;
            Check(std::isfinite(f.mean)&&std::isfinite(r.mean),tag.str()+": finite");
            Check(Agree(f,r),tag.str()+": extended HWSS lane = SMS-off HWSS lane within 3 combined se");
            Check(std::fabs(smsOwned.mean-ptOwned)<=3*std::hypot(smsOwned.se,ptOwnedSe),
                tag.str()+": lane SMS-owned (full-kept) = PT-owned (ref-kept) within 3 combined se");
            Check(Agree(f,nf_),tag.str()+": HWSS lane = extended NM at the lane wavelength within 3 combined se");
            Check(Agree(smsOwned,nmOwned),tag.str()+": lane SMS-owned = NM SMS-owned at the lane wavelength within 3 combined se");
            if(e.owned[w]==Owned::Required) Check(smsOwned.mean>3*smsOwned.se&&smsOwned.mean>0,tag.str()+": SMS owns a resolvable share");
            if(e.owned[w]==Owned::Forbidden) Check(smsOwned.mean==0&&nmOwned.mean==0,tag.str()+": SMS owns nothing in this lane");
            if(e.closedForm) {
                const double cf=MirrorClosedForm(spec,o.lambdas[w]);
                std::cout<<" closedForm="<<cf;
                Check(std::fabs(f.mean-cf)<=3*f.se+1e-3*cf,tag.str()+": HWSS lane = closed form within 3 se");
            }
            std::cout<<" n="<<o.salts<<" spp="<<o.N<<"\n";
        }
        if(e.expectLanesDiffer) {
            // Fixture sensitivity: the lanes' own (SMS-off) transport
            // differs, so the per-lane NM equalities above could not hold
            // for a lane evaluated in the hero's domain.
            double best=0;
            for(unsigned a=0;a<kLanes;++a) for(unsigned b=a+1;b<kLanes;++b) {
                const Moments x(ref.lane[a]),y(ref.lane[b]);
                best=std::max(best,std::fabs(x.mean-y.mean)/std::hypot(x.se,y.se));
            }
            std::cout<<spec.label<<" largest lane-pair SMS-off separation z="<<best<<"\n";
            Check(best>3,spec.label+": lanes' transport differs (distinct per-lane geometry)");
        }
        std::cout<<spec.label<<" counters partitionQueries="<<counters.partitionQueries
            <<" owned="<<counters.partitionOwned<<" uncertain="<<counters.partitionUncertain
            <<" canonicalSolves="<<counters.canonicalSolves<<" samplerDraws="<<counters.canonicalSamplerDraws<<"\n";
        Check(counters.canonicalSamplerDraws==0,spec.label+": the canonical predicate drew no random number");
    }
}

//////////////////////////////////////////////////////////////////////
// lanes: the per-lane area partition through the NM delegations
//////////////////////////////////////////////////////////////////////
static void LanesSection()
{
    // Estimator B's reciprocal has a heavy tail: 16 salts (Phase 3's
    // render-section protocol), never fewer for the recorded gate.
    RenderOptions o;o.N=g_quick?512:4096;o.salts=g_quick?4:16;
    LaneExpect cf;cf.closedForm=true;
    // Planar mirror (no BSDF: every lane is handed to its own NM walk with
    // the HWSS anchor bit and record): indexed mesh both windings, plane.
    for(bool reverse:{false,true}) LaneCase(CeilingScene("mirror",true,reverse),o,cf);
    LaneCase(CeilingScene("mirror",false,false),o,cf);
    // Glass slab both windings; transformed instance.
    for(bool reverse:{false,true}) LaneCase(SlabScene(reverse,"glass",false),o);
    LaneCase(SlabScene(false,"glass",true),o);
    // Dispersive slab: distinct companion indices (1.9/1.62/1.4/1.3).
    LaneCase(SlabScene(false,"prism",false),o);
    LaneCase(SlabScene(true,"prism",true),o);
    // Sphere controls (constant and dispersive: a ball lens whose focus
    // moves with the lane's index).
    LaneCase(SphereScene("glass"),o);
    {LaneExpect d;d.expectLanesDiffer=true;LaneCase(SphereScene("prism"),o,d);}
    // Start-inside a closed glass box, both windings.
    for(bool reverse:{false,true}) LaneCase(InsideScene(reverse),o);
    // Nested exterior replay: a dispersive ball inside a constant 1.4
    // enclosure (the HWSS body's anchor stack holds the enclosure, replayed
    // per lane); a constant ball inside a DISPERSIVE enclosure (the camera
    // is inside it: per-lane NM from the camera).
    LaneCase(ImmersedBallScene("medium14","prism"),o);
    LaneCase(ImmersedBallScene("dispersivemedium","glass"),o);
    // SSS receiver (a camera-entry NM delegation per lane).  The receiver's
    // owned share is small (Phase 3: ~4 %); gated for agreement only.
    {RenderOptions r=o;r.reentryShaderOp=true;r.trials=4;
     LaneExpect s;s.owned.fill(Owned::Reported);
     LaneCase(SSSReceiverScene(),r,s);}
}

//////////////////////////////////////////////////////////////////////
// body: DL-378 -- a BSDF caster's delta lobe inside the HWSS body
//////////////////////////////////////////////////////////////////////
static void BodySection()
{
    RenderOptions o;o.N=g_quick?512:2048;o.salts=g_quick?4:8;
    for(const char* lum:{"lum","lumbsdf"}) {
        for(bool reverse:{false,true}) LaneCase(CeilingScene("polished",true,reverse,lum),o);
        LaneCase(CeilingScene("polished",false,false,lum),o);
    }
}

//////////////////////////////////////////////////////////////////////
// tir: estimator A per lane at the critical angle
//////////////////////////////////////////////////////////////////////
static void TIRSection()
{
    RenderOptions o;o.N=g_quick?512:2048;o.salts=g_quick?4:8;o.targetBounces=3;
    // n(450)=1.9, n(500)=1.7, n(620)=1.36, n(650)=1.3: the first two are
    // past the hypotenuse's ~45-degree critical angle (n > 1.414).
    const Lanes tirHero{{450,500,620,650}}, plainHero{{650,450,500,620}};
    for(bool reverse:{false,true}) for(const Lanes& lambdas:{tirHero,plainHero}) {
        RenderOptions p=o;p.lambdas=lambdas;p.saltBase=g_saltBase;g_saltBase+=1000;
        const auto spec=TIRPrismScene(reverse);
        SMSReferenceCounters counters;p.counters=&counters;
        const auto hwss=Render(spec,Mode::Full,p);
        p.counters=nullptr;
        if(!hwss.ok) {Check(false,spec.label+": renders complete");continue;}
        Check(hwss.terminatedSamples==0,spec.label+": no companion termination");
        std::array<double,SampledWavelengths::N> mean{};
        for(unsigned w=0;w<kLanes;++w) {
            RenderOptions n=p;n.saltBase=p.saltBase+100+10*w;
            const auto nm=Render(spec,Mode::Full,n,int(w));
            const Moments h(hwss.lane[w]),r(nm.lane[w]);
            mean[w]=h.mean;
            std::ostringstream tag;tag<<spec.label<<" hero="<<lambdas[0]<<" lane="<<w<<" nm="<<lambdas[w];
            std::cout<<std::setprecision(8)<<tag.str()<<" HWSS="<<h.mean<<"+-"<<h.se<<" NM="<<r.mean<<"+-"<<r.se
                <<" ratio="<<h.mean/r.mean<<"\n";
            Check(nm.ok&&h.mean>3*h.se,tag.str()+": estimator A delivers the light through the prism");
            Check(Agree(h,r),tag.str()+": HWSS lane = extended NM at the lane wavelength within 3 combined se");
        }
        // The two TIR lanes (450, 500) carry the total-reflection weight
        // (1); the two below the critical angle (620, 650) a dielectric
        // Fresnel weight of order 0.1.
        double tir=0,plain=0;
        for(unsigned w=0;w<kLanes;++w) (lambdas[w]<550?tir:plain)+=mean[w];
        std::cout<<spec.label<<" hero="<<lambdas[0]<<" TIR/non-TIR lane ratio="<<tir/plain
            <<" acceptedDiscoveries="<<counters.acceptedDiscoveries<<"\n";
        Check(tir>3*plain,spec.label+": lanes past the critical angle carry the TIR weight, the others Fresnel");
    }
}

//////////////////////////////////////////////////////////////////////
// mask: per-lane anchor eligibility (all three switches per lane)
//////////////////////////////////////////////////////////////////////
static void MaskSection()
{
    RenderOptions o;o.N=g_quick?512:2048;o.salts=g_quick?4:8;
    // s(450)=2, s(550)=2, s(600)=1.25, s(640)=0.8: lane 3 is ineligible.
    o.lambdas={{450,550,600,640}};
    LaneExpect m;m.owned[3]=Owned::Forbidden;
    // Delegated emitter hit (mirror, `material none` luminaire), and the
    // HWSS body's own emitter hit after a polished coat (BSDF luminaire).
    LaneCase(CeilingScene("mirror",true,false,"lum",HGChunks()),o,m);
    LaneCase(CeilingScene("polished",true,true,"lumbsdf",HGChunks()),o,m);
    LaneCase(CeilingScene("polished",false,false,"lum",HGChunks()),o,m);
    // Hero ineligible, companions eligible.
    RenderOptions h=o;h.lambdas={{640,450,550,600}};
    LaneExpect hm;hm.owned[0]=Owned::Forbidden;
    LaneCase(CeilingScene("polished",true,false,"lumbsdf",HGChunks()),h,hm);
}

//////////////////////////////////////////////////////////////////////
// delta: estimator A per lane; DL-344's shadow switch per lane
//////////////////////////////////////////////////////////////////////
static void DeltaSection()
{
    RenderOptions o;o.N=g_quick?512:2048;o.salts=g_quick?4:8;
    for(const char* material:{"glass","prism"}) {
        const auto spec=DeltaSlabScene(material);
        RenderOptions a=o;a.saltBase=g_saltBase;g_saltBase+=1000;a.transparentShadows=true;
        RenderOptions b=a;b.transparentShadows=false;
        const auto on=Render(spec,Mode::Full,a), off=Render(spec,Mode::Full,b);
        bool same=on.ok&&off.ok;
        for(unsigned w=0;same&&w<kLanes;++w) for(std::size_t s=0;same&&s<on.lane[w].size();++s) same=on.lane[w][s]==off.lane[w][s];
        Check(same,spec.label+": every lane is an anchor: transparent shadows on/off bit-identical (DL-344)");
        for(unsigned w=0;w<kLanes;++w) {
            RenderOptions n=b;n.saltBase=b.saltBase+100+10*w;
            const auto nm=Render(spec,Mode::Full,n,int(w));
            const Moments h(off.lane[w]),r(nm.lane[w]);
            std::ostringstream tag;tag<<spec.label<<" lane="<<w<<" nm="<<o.lambdas[w];
            std::cout<<std::setprecision(8)<<tag.str()<<" HWSS="<<h.mean<<"+-"<<h.se<<" NM="<<r.mean<<"+-"<<r.se<<"\n";
            Check(nm.ok&&h.mean>3*h.se,tag.str()+": estimator A delivers the light");
            Check(Agree(h,r),tag.str()+": HWSS lane = extended NM at the lane wavelength within 3 combined se");
        }
    }
    // An ineligible lane (HG mask) keeps DL-344 off: transparent shadows
    // then deliver the light in that lane only.
    {
        auto spec=DeltaSlabScene("glass");spec.text+=HGChunks();spec.label+=" + HG mask";
        RenderOptions a=o;a.lambdas={{450,550,600,640}};a.saltBase=g_saltBase;g_saltBase+=1000;a.transparentShadows=true;
        RenderOptions b=a;b.transparentShadows=false;
        const auto on=Render(spec,Mode::Full,a), off=Render(spec,Mode::Full,b);
        for(unsigned w=0;w<kLanes&&on.ok&&off.ok;++w) {
            bool same=true;
            for(std::size_t s=0;same&&s<on.lane[w].size();++s) same=on.lane[w][s]==off.lane[w][s];
            const Moments mon(on.lane[w]),moff(off.lane[w]);
            std::cout<<spec.label<<" lane="<<w<<" transparent on="<<mon.mean<<" off="<<moff.mean<<" identical="<<same<<"\n";
            if(w<3) Check(same,spec.label+": eligible lane unaffected by transparent shadows");
            else Check(mon.mean>1.05*moff.mean,spec.label+": ineligible lane: transparent shadows deliver the light (DL-344 off)");
        }
    }
}

//////////////////////////////////////////////////////////////////////
// modeoff: forced-legacy scope and extended-off HWSS
//////////////////////////////////////////////////////////////////////
static void ModeOffSection()
{
    RenderOptions o;o.N=512;o.salts=2;
    for(const auto& spec:{CeilingScene("polished",true,false),SlabScene(false,"prism",false),CeilingScene("mirror",true,true,"lumbsdf")}) {
        o.saltBase=g_saltBase;g_saltBase+=1000;
        RenderOptions f=o;f.forceLegacyScope=true;
        const auto forced=Render(spec,Mode::Full,f);
        const auto off=Render(spec,Mode::Legacy,o);
        const auto again=Render(spec,Mode::Legacy,o);
        bool same=forced.ok&&off.ok&&again.ok, det=same;
        for(unsigned w=0;w<kLanes;++w) for(std::size_t s=0;s<off.lane[w].size()&&same;++s) {
            same=forced.lane[w][s]==off.lane[w][s];
            det=det&&off.lane[w][s]==again.lane[w][s];
        }
        Check(det,spec.label+": extended-off HWSS is deterministic under a fixed salt");
        Check(same,spec.label+": a forced-legacy scope is bit-identical to extended mode off (HWSS shader-op path)");
    }
}

//////////////////////////////////////////////////////////////////////
// cost (opt-in, `--section cost`): interleaved wall time of one HWSS
// point render, extended vs extended off (legacy SMS), n = 8 pairs.
// Machine contention applies; report the paired mean ratio and its se.
//////////////////////////////////////////////////////////////////////
static void CostSection()
{
    RenderOptions o;o.N=4096;o.salts=1;
    for(const auto& spec:{SlabScene(false,"glass",false),SlabScene(false,"prism",false),
            CeilingScene("polished",true,false,"lumbsdf"),SphereScene("glass")}) {
        std::vector<double> ratios, ext, off;
        for(unsigned pair=0;pair<8;++pair) {
            o.saltBase=g_saltBase+pair;
            const auto time=[&](Mode m) {
                const auto start=std::chrono::steady_clock::now();
                Render(spec,m,o);
                return std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
            };
            const double a=(pair%2)?time(Mode::Legacy):time(Mode::Full);
            const double b=(pair%2)?time(Mode::Full):time(Mode::Legacy);
            const double e=(pair%2)?b:a, l=(pair%2)?a:b;
            ext.push_back(e);off.push_back(l);ratios.push_back(e/l);
        }
        g_saltBase+=1000;
        const Moments r(ratios),e(ext),l(off);
        std::cout<<std::setprecision(6)<<"COST "<<spec.label<<" extended="<<e.mean<<"+-"<<e.se<<"s off="<<l.mean<<"+-"<<l.se
            <<"s ratio="<<r.mean<<"+-"<<r.se<<" (n=8 interleaved pairs, "<<o.N<<" HWSS samples each)\n";
    }
}

//////////////////////////////////////////////////////////////////////
// nmprobe (opt-in): the NM-only partition (Phase 3's own estimator) at a
// lane wavelength, to attribute an HWSS lane disagreement: extended NM
// full vs SMS-off NM, and NM SMS-owned vs PT-owned.  32 salts.
//////////////////////////////////////////////////////////////////////
static void NMProbeSection()
{
    RenderOptions o;o.N=4096;o.salts=32;
    const auto spec=ImmersedBallScene("medium14","prism");
    for(unsigned w:{0u,1u}) {
        o.saltBase=g_saltBase;g_saltBase+=1000;
        const auto ref=Render(spec,Mode::Ref,o,int(w));
        RenderOptions f=o;f.saltBase=o.saltBase+100;
        const auto full=Render(spec,Mode::Full,f,int(w)), kept=Render(spec,Mode::Kept,f,int(w));
        const Moments r(ref.lane[w]),fu(full.lane[w]),k(kept.lane[w]),owned(Diff(full.lane[w],kept.lane[w]));
        std::cout<<std::setprecision(8)<<"NMPROBE "<<spec.label<<" nm="<<o.lambdas[w]<<" ref="<<r.mean<<"+-"<<r.se
            <<" full="<<fu.mean<<"+-"<<fu.se<<" full/ref="<<fu.mean/r.mean<<" z="<<(fu.mean-r.mean)/std::hypot(fu.se,r.se)
            <<" smsOwned="<<owned.mean<<"+-"<<owned.se<<" ptOwned="<<r.mean-k.mean<<"+-"<<std::hypot(r.se,k.se)<<" n=32\n";
    }
}

int main(int argc,char** argv)
{
    Check(ConfigureTestWorker(),"single-worker options configured");
    std::string section;
    for(int i=1;i<argc;++i) {
        const std::string a=argv[i];
        if(a=="--quick") g_quick=true;
        else if(a=="--section" && i+1<argc) section=argv[++i];
    }
    const auto run=[&](const char* name,void(*f)()) {
        if(section.empty()||section==name) {std::cout<<"=== "<<name<<" ===\n";f();}
    };
    run("modeoff",ModeOffSection);
    run("tir",TIRSection);
    run("delta",DeltaSection);
    run("mask",MaskSection);
    run("body",BodySection);
    run("lanes",LanesSection);
    if(section=="cost") {std::cout<<"=== cost ===\n";CostSection();}
    if(section=="nmprobe") {std::cout<<"=== nmprobe ===\n";NMProbeSection();}
    std::cout<<passCount<<" passed, "<<failCount<<" failed"<<std::endl;
    return failCount?1:0;
}
