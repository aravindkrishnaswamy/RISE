// Native component and wavelength queries over real geometry. This suite
// tests domain/replay primitives; production partition gates are separate.
#include "SMSRenderTestSupport.h"
#include "../src/Library/Utilities/ManifoldSolver.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IObjectPriv.h"
#include "../src/Library/Shaders/PathTracingIntegrator.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Utilities/RuntimeContext.h"
#include "../src/Library/RISE_API.h"
#include <sstream>
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/Optics.h"

static bool Near(double a, double b) { return std::fabs(a-b)<1e-11; }
static std::string Materials(bool composite)
{
    std::string s = "RISE ASCII SCENE 7\nuniformcolor_painter\n{\n name white\n color 1 1 1\n}\n"
        "scalar_painter\n{\n name triple\n values 1.3 1.5 1.9\n}\n"
        "perfectrefractor_material\n{\n name glass\n refractance white\n ior triple\n}\n"
        "perfectrefractor_material\n{\n name inner\n refractance white\n ior 1.2\n}\n";
    if(composite) s += "composite_material\n{\n name layers\n top glass\n bottom inner\n thickness 0.02\n extinction 0\n}\n";
    return s;
}
static std::string Mesh(bool closed, bool reverse)
{
    std::ostringstream s;
    s << "indexedmesh_geometry\n{\n name shape\n double_sided TRUE\n face_normals TRUE\n";
    const Point3 vertices[] = {Point3(-1,-1,-1),Point3(1,-1,-1),Point3(1,1,-1),Point3(-1,1,-1),
        Point3(-1,-1,1),Point3(1,-1,1),Point3(1,1,1),Point3(-1,1,1)};
    for(const auto& p : vertices) s << " vertex " << p.x << ' ' << p.y << ' ' << p.z << '\n';
    for(int i=0;i<8;++i) s << " uv " << (i%2) << ' ' << ((i/2)%2) << '\n';
    const int triangles[][3] = {{0,2,1},{0,3,2},{4,5,6},{4,6,7},
        {0,1,5},{0,5,4},{3,7,6},{3,6,2},{0,4,7},{0,7,3},{1,2,6},{1,6,5}};
    for(int i=0;i<(closed?12:2);++i) s << " triangle " << triangles[i][0] << ' '
        << triangles[i][reverse?2:1] << ' ' << triangles[i][reverse?1:2] << '\n';
    return s.str()+"}\n";
}
struct LoadedScene {
    IJobPriv* job=nullptr;
    explicit LoadedScene(const std::string& text) {
        const auto path=TestTempPath("sms_domain_"+std::to_string(::getpid())+".RISEscene");
        { std::ofstream out(path); out << text; }
        if(RISE_CreateJobPriv(&job)) Check(job->LoadAsciiSceneViaCst(path.c_str()),"domain fixture parses");
        std::remove(path.c_str());
    }
    ~LoadedScene() { safe_release(job); }
    const IScene& Scene() const { return *job->GetScene(); }
    const IObject* Object(const char* name) const { return Scene().GetObjects()->GetItem(name); }
};
static RayIntersection Hit(const IObject& object, Point3 origin, Vector3 direction)
{
    RayIntersection hit(Ray(origin,direction),nullRasterizerState);
    object.IntersectRay(hit,RISE_INFINITY,true,true,false);
    Check(hit.geometric.bHit,"actual geometry intersection");
    return hit;
}
#ifdef RISE_SMS_DOMAIN_REPLAY
static void ComponentAndCrossingCases()
{
    for(bool closed : {false,true}) for(bool reverse : {false,true}) for(bool transformed : {false,true}) {
        const double offset=transformed ? 3 : 0;
        std::string scene=Materials(false)+Mesh(closed,reverse)+
            "standard_object\n{\n name caster\n geometry shape\n material glass\n position "+std::to_string(offset)+" 0 0\n scale 1 1 1\n}\n";
        LoadedScene loaded(scene);
        const IObject* object=loaded.Object("caster");
        if(!object) { Check(false,"caster lookup"); continue; }
        for(int side : {-1,1}) {
            auto hit=Hit(*object,Point3(offset,0,side*3),Vector3(0,0,-side));
            for(unsigned c=0;c<3;++c) {
                const double indices[]={1.3,1.5,1.9};
                IORStack stack(1);
                SMSNativeMaterialQuery query;
                Check(SMSDomainReplay::Query(*hit.pMaterial,hit.geometric,stack,SMSQueryDomain::RGB(c),query),"native RGB domain supported");
                Check(Near(query.index,indices[c]),"authored component index retained");
                const auto before=stack;
                double ni=0,nt=0; bool exiting=false;
                Check(SMSDomainReplay::Cross(*hit.pMaterial,object,hit.geometric,SMSQueryDomain::RGB(c),true,stack,ni,nt,exiting),"reflection domain replay");
                Check(stack.SameInterfaces(before),"reflection preserves membership");
                Check(SMSDomainReplay::Cross(*hit.pMaterial,object,hit.geometric,SMSQueryDomain::RGB(c),false,stack,ni,nt,exiting),"transmission domain replay");
                Check(Near(ni,1)&&Near(nt,indices[c]),"first closed or uncertified mesh crossing follows native membership");
            }
        }
    }
}
static void NestedAndCompositeCases()
{
    for(bool composite : {false,true}) for(bool reverse : {false,true}) {
        LoadedScene loaded(Materials(composite)+Mesh(true,reverse)+
            "standard_object\n{\n name outer\n geometry shape\n material "+std::string(composite?"layers":"glass")+"\n scale 2 2 2\n}\n"
            "sphere_geometry\n{\n name sphere\n radius 0.5\n}\n"
            "standard_object\n{\n name inner_obj\n geometry sphere\n material inner\n}\n");
        const IObject* outer=loaded.Object("outer");
        const IObject* inner=loaded.Object("inner_obj");
        if(!outer || !inner) { Check(false,"nested objects present"); continue; }
        IORStack live(1); live.SetCurrentObject(outer); live.push(1.3);
        live.SetCurrentObject(inner); live.push(1.2);
        SMSStartingMedia capture;
        const bool captured=SMSDomainReplay::Capture(loaded.Scene(),Point3(0,0,0),live,capture);
        Check(captured!=composite,"composite starting membership rejected");
        if(composite) {
            IORStack unseeded(1);
            Check(!SMSDomainReplay::Capture(loaded.Scene(),Point3(0,0,0),unseeded,capture),"DL-407 missing composite containment rejected");
            auto hit=Hit(*outer,Point3(0,0,3),Vector3(0,0,-1));
            SMSNativeMaterialQuery q;
            Check(!SMSDomainReplay::Query(*hit.pMaterial,hit.geometric,unseeded,SMSQueryDomain::RGB(1),q),"composite caster rejected");
            continue;
        }
        for(auto domain : {SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),SMSQueryDomain::NM(450),SMSQueryDomain::NM(550),SMSQueryDomain::NM(650)}) {
            IORStack replay(1);
            Check(SMSDomainReplay::BuildStack(capture,domain,replay),"start-inside membership evaluated in selected domain");
            auto hit=Hit(*inner,Point3(0,0,0),Vector3(0,0,1));
            double ni=0,nt=0; bool exiting=false;
            Check(SMSDomainReplay::Cross(*hit.pMaterial,inner,hit.geometric,domain,false,replay,ni,nt,exiting),"nested exit supported");
            const double expected=domain.kind==SMSQueryDomain::RGBComponent
                ? (domain.component==0?1.3:domain.component==1?1.5:1.9)
                : (domain.nm==450?1.9:domain.nm==550?1.5:1.3);
            Check(exiting && Near(ni,1.2) && Near(nt,expected),"nested exit restores queried exterior, never seeded scalar");
        }
    }
}
static void SolvedRootCases()
{
    // Off-seed tint is evaluated at the converged point, independently of
    // its seed value. All three RGB geometries use their authored indices.
    std::string scene=Materials(false)+
        "expression_function2d\n{\n name tint_fn\n expr 0.2 + 0.3 * (u + v)\n}\n"
        "function2d_painter\n{\n name tint\n function2d tint_fn\n}\n"
        "perfectrefractor_material\n{\n name tinted\n refractance tint\n ior triple\n}\n"
        "clippedplane_geometry\n{\n name sheet\n pta -2 -2 0\n ptb -2 2 0\n ptc 2 2 0\n ptd 2 -2 0\n doublesided TRUE\n}\n"
        "standard_object\n{\n name caster\n geometry sheet\n material tinted\n}\n";
    LoadedScene loaded(scene);
    const IObject* object=loaded.Object("caster");
    if(!object) { Check(false,"solved-root object"); return; }
    const Point3 start(0,0,-2), end(1,0,2);
    auto seed=Hit(*object,start,Vector3(0,0,1));
    ManifoldSolverConfig config;
    config.biased=true; config.maxIterations=40; config.solverThreshold=1e-9;
    ManifoldSolver* solver=new ManifoldSolver(config);
    RandomNumberGenerator random(17);
    IndependentSampler sampler(random);
    IORStack stack(1);
    std::vector<double> positions;
    for(unsigned c=0;c<3;++c) {
        SMSDomainVertex record(seed.geometric);
        record.geometry.position=seed.geometric.ptIntersection;
        record.geometry.normal=seed.geometric.vNormal;
        record.geometry.geomNormal=seed.geometric.UnflippedGeomNormal();
        record.geometry.pObject=object; record.geometry.pMaterial=seed.pMaterial;
        record.geometry.uv=seed.geometric.ptCoord;
        record.geometry.isReflection=false;
        std::vector<SMSDomainVertex> vertices{record};
        const auto result=solver->SolveDomain(start,Vector3(0,0,1),end,Vector3(0,0,-1),
            loaded.Scene(),stack,SMSQueryDomain::RGB(c),vertices,sampler,1e-4);
        Check(result.valid,"domain plane solve converges");
        if(!result.valid) continue;
        const auto& v=result.specularChain[0];
        positions.push_back(v.position.x);
        const Vector3 incoming=Vector3Ops::Normalize(Vector3Ops::mkVector3(v.position,start));
        const Vector3 outgoing=Vector3Ops::Normalize(Vector3Ops::mkVector3(end,v.position));
        const double sinI=std::sqrt(incoming.x*incoming.x+incoming.y*incoming.y);
        const double sinT=std::sqrt(outgoing.x*outgoing.x+outgoing.y*outgoing.y);
        Check(std::fabs(v.etaI*sinI-v.etaT*sinT)<1e-8,"native-index Snell oracle at solved geometry");
        const double tint=seed.pMaterial->GetSpecularInfo(vertices[0].context,stack).attenuation[c];
        const double fresnel=Optics::CalculateDielectricReflectanceCosine(std::fabs(incoming.z),v.etaI,v.etaT);
        const double expected=tint*(1-fresnel)*(v.etaI/v.etaT)*(v.etaI/v.etaT);
        Check(Near(result.contribution[c],expected),"solved-root tint and native Fresnel refreshed");
        Check(tint!=seed.pMaterial->GetSpecularInfo(seed.geometric,stack).attenuation[c],"off-seed tint oracle distinguishes stale query");
    }
    Check(positions.size()==3 && positions[0]!=positions[1] && positions[1]!=positions[2],"distinct authored RGB indices produce distinct roots");
    solver->release();
}
static void CoatedEventCases()
{
    // Independent lossless single-film Airy oracle at normal incidence.
    const auto airy=[](double ni,double nt,double nm) {
        const double nf=std::sqrt(1.5), thickness=550/(4*nf);
        const double a=(ni-nf)/(ni+nf), b=(nf-nt)/(nf+nt);
        const double phase=4*3.14159265358979323846*nf*thickness/nm;
        return (a*a+b*b+2*a*b*std::cos(phase))/(1+a*a*b*b+2*a*b*std::cos(phase));
    };
    for(bool closed:{false,true}) for(bool reverse:{false,true}) for(int side:{-1,1}) {
        LoadedScene loaded(Materials(false)+
            "dielectric_material\n{\n name coated\n tau 0.7\n ior triple\n scattering 1000000\n ar_layer 1.224744871391589 112.26827987812466 0\n}\n"+
            Mesh(closed,reverse)+"standard_object\n{\n name caster\n geometry shape\n material coated\n position 3 0 0\n}\n");
        const IObject* object=loaded.Object("caster");
        if(!object) { Check(false,"coated caster lookup"); continue; }
        for(bool inside:{false,true}) {
            auto hit=Hit(*object,Point3(3,0,inside?0:side*3),Vector3(0,0,inside?(closed?side:-1):-side));
            // An open sheet may have historical walk membership; this is
            // distinct from geometric containment and must be replayed.
            if(!hit.geometric.bHit) continue;
            for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),
                SMSQueryDomain::NM(450),SMSQueryDomain::NM(550),SMSQueryDomain::NM(650)}) {
                IORStack before(1);
                SMSNativeMaterialQuery query;
                Check(SMSDomainReplay::Query(*hit.pMaterial,hit.geometric,before,domain,query),"native coated query supported");
                if(inside) { before.SetCurrentObject(object); before.push(query.index); }
                for(bool reflection:{false,true}) {
                    IORStack replay(before); double ni=0,nt=0,weight=0; bool exiting=false;
                    Check(SMSDomainReplay::Cross(*hit.pMaterial,object,hit.geometric,domain,reflection,replay,ni,nt,exiting),"coated event crossing supported");
                    Check(SMSDomainReplay::EventWeight(*hit.pMaterial,hit.geometric,before,domain,reflection,exiting,ni,nt,2,weight),"coated event priced");
                    const double nm=domain.kind==SMSQueryDomain::Wavelength?domain.nm:ScalarPainterRGB::kChannelNM[domain.component];
                    const double f=airy(ni,nt,nm);
                    const double expected=reflection?f:(1-f)*(ni/nt)*(ni/nt)*(exiting?0.49:1);
                    Check(Near(weight,expected),"selected index, film wavelength and exit absorption match Airy oracle");
                    if(reflection) Check(replay.SameInterfaces(before),"coated reflection leaves medium unchanged");
                }
            }
        }
    }
}
static void ClosedChainCases()
{
    for(bool sphere:{false,true}) for(bool reverse:{false,true}) for(int side:{-1,1}) {
        LoadedScene loaded(Materials(false)+(sphere?
            "sphere_geometry\n{\n name shape\n radius 1\n}\n":Mesh(true,reverse))+
            "standard_object\n{\n name caster\n geometry shape\n material glass\n position 3 0 0\n}\n");
        const IObject* object=loaded.Object("caster");
        if(!object) { Check(false,"closed-chain object"); continue; }
        const Point3 start(3,0,side*3), end(3,0,-side*3);
        const Vector3 direction(0,0,-side);
        const auto entry=Hit(*object,start,direction), exit=Hit(*object,Point3(3,0,0),direction);
        for(bool inside:{false,true}) for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),
            SMSQueryDomain::RGB(2),SMSQueryDomain::NM(450),SMSQueryDomain::NM(550),SMSQueryDomain::NM(650)}) {
            std::vector<SMSDomainVertex> records;
            for(const auto* hit:{&entry,&exit}) {
                if(inside && hit==&entry) continue;
                records.emplace_back(hit->geometric);
                auto& vertex=records.back().geometry;
                vertex.position=hit->geometric.ptIntersection; vertex.normal=hit->geometric.vNormal;
                vertex.geomNormal=hit->geometric.UnflippedGeomNormal();
                vertex.pObject=object; vertex.pMaterial=hit->pMaterial; vertex.isReflection=false;
            }
            IORStack stack(1);
            if(inside) { stack.SetCurrentObject(object); stack.push(1.3); }
            ManifoldSolverConfig config; config.biased=true; config.solverThreshold=1e-9;
            ManifoldSolver* solver=new ManifoldSolver(config);
            RandomNumberGenerator random(45); IndependentSampler sampler(random);
            const auto result=solver->SolveDomain(inside?Point3(3,0,0):start,direction,end,-direction,
                loaded.Scene(),stack,domain,records,sampler,1e-4);
            Check(result.valid,"closed sphere/mesh multi-interface and start-inside solve");
            if(result.valid) {
                const auto& final=result.specularChain.back();
                Check(final.isExiting && Near(final.etaT,1),"final-root replay exits to environment");
                const double expected=domain.kind==SMSQueryDomain::RGBComponent?
                    (domain.component==0?1.3:domain.component==1?1.5:1.9):
                    (domain.nm==450?1.9:domain.nm==550?1.5:1.3);
                Check(Near(final.etaI,expected),"closed-chain exit uses selected domain");
            }
            solver->release();
        }
    }
}
static std::vector<RISEColor> TraceCompositeGrid(LoadedScene& loaded, bool extended,
    bool startInside, unsigned salt)
{
    std::vector<IShaderOp*> ops;
    IShader* shader=nullptr;
    if(!RISE_API_CreateStandardShader(&shader,ops)) return {};
    RayCaster* caster=new RayCaster(false,16,*shader,true);
    loaded.Scene().GetObjects()->PrepareForRendering();
    caster->AttachScene(&loaded.Scene());
    ManifoldSolverConfig config; config.enabled=true; config.extendedMode=extended;
    StabilityConfig stability; stability.rrMinDepth=20;
    PathTracingIntegrator* integrator=new PathTracingIntegrator(config,stability);
    integrator->SetMaxPathDepth(16);
    std::vector<const IObject*> casters;
    ManifoldSolver::EnumerateSpecularCasters(loaded.Scene(),casters);
    integrator->GetSolver()->SetSpecularCasters(casters);
    if(extended) {
        IORStack missing(1);
        if(startInside) Check(!integrator->GetSolver()->ExtendedAnchorEligible(
            loaded.Scene(),*caster,Point3(0,0,-0.5),missing),"production eligibility rejects unseeded composite anchor");
    }
    SobolSamplerTestHooks::ValueSalt().store(salt);
    std::vector<RISEColor> pixels;
    for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x) {
        RISEPel sum(0,0,0);
        for(unsigned sample=0;sample<32;++sample) {
            RandomNumberGenerator random(salt+sample+64*y+x);
            SobolSampler sampler(sample,64*y+x);
            RuntimeContext context(random,RuntimeContext::PASS_NORMAL,false);
            context.pSampler=&sampler;
            RasterizerState raster{}; raster.x=x; raster.y=y;
            const Point3 origin((double(x)-3.5)*0.025,(double(y)-3.5)*0.025,startInside?0:-3);
            const Vector3 direction(0,0,startInside?-1:1);
            sum=sum+integrator->IntegrateRay(context,raster,Ray(origin,direction),
                loaded.Scene(),*caster,sampler,nullptr,nullptr);
        }
        pixels.emplace_back(sum*(1.0/32),1);
    }
    SobolSamplerTestHooks::ValueSalt().store(0);
    integrator->release(); caster->release(); shader->release();
    return pixels;
}
static void CompositePTCases()
{
    for(bool reverse : {false,true}) for(bool startInside : {false,true}) {
        std::string scene=Materials(true)+Mesh(startInside,reverse)+
            "standard_object\n{\n name composite\n geometry shape\n material layers\n scale 2 2 2\n}\n"
            "lambertian_luminaire_material\n{\n name emitter_mat\n material none\n exitance white\n scale 2\n}\n"
            "clippedplane_geometry\n{\n name emitter\n pta -20 -20 4\n ptb -20 20 4\n ptc 20 20 4\n ptd 20 -20 4\n doublesided FALSE\n}\n"
            "standard_object\n{\n name light\n geometry emitter\n material emitter_mat\n}\n";
        if(startInside) scene +=
            "lambertian_material\n{\n name receiver_mat\n reflectance white\n}\n"
            "clippedplane_geometry\n{\n name receiver\n pta -1 -1 -0.5\n ptb 1 -1 -0.5\n ptc 1 1 -0.5\n ptd -1 1 -0.5\n doublesided FALSE\n}\n"
            "standard_object\n{\n name floor\n geometry receiver\n material receiver_mat\n}\n";
        LoadedScene loaded(scene);
        std::vector<double> differences, offMeans, onMeans;
        for(unsigned trial=0;trial<4;++trial) {
            const unsigned salt=SobolSequence::HashCombine(9000+trial,0x534d5344);
            const auto off=TraceCompositeGrid(loaded,false,startInside,salt);
            const auto on=TraceCompositeGrid(loaded,true,startInside,salt);
            double a=0,b=0;
            for(const auto& pixel:off) a+=(pixel.base.r+pixel.base.g+pixel.base.b)/3;
            for(const auto& pixel:on) b+=(pixel.base.r+pixel.base.g+pixel.base.b)/3;
            a/=64; b/=64;
            Check(off.size()==64 && on.size()==64 && std::isfinite(a) && std::isfinite(b) && a>0 && b>0,
                "PT keeps composite-crossing emitter paths with extended mode on/off");
            if(!startInside) Check(HashPixels(off)==HashPixels(on),"deterministic composite camera path stays bit-identical");
            differences.push_back(b-a); offMeans.push_back(a); onMeans.push_back(b);
        }
        const auto d=Summarize(differences), a=Summarize(offMeans), b=Summarize(onMeans);
        std::cout << "composite PT reverse=" << reverse << " startInside=" << startInside
            << " off=" << a.mean << " sd=" << a.sd << " on=" << b.mean << " sd=" << b.sd
            << " paired delta=" << d.mean << " sd=" << d.sd << " n=4\n";
        Check(d.mean==0 || std::fabs(d.mean)<=3*d.sd/2,"composite mode parity within three measured mean SDs");
    }
}
int main()
{
    ComponentAndCrossingCases();
    NestedAndCompositeCases();
    SolvedRootCases();
    CoatedEventCases();
    ClosedChainCases();
    CompositePTCases();
    std::cout << passCount << " passed, " << failCount << " failed\n";
    return failCount?1:0;
}
#else
// Red proof: exercise real master entry points, rather than a mock of
// their bugs. New-domain primitives above cannot compile against master.
int main()
{
    LoadedScene loaded(Materials(false)+
        "expression_function2d\n{\n name tint_fn\n expr 0.2 + 0.3 * (u + v)\n}\n"
        "function2d_painter\n{\n name tint\n function2d tint_fn\n}\n"
        "perfectrefractor_material\n{\n name tinted\n refractance tint\n ior triple\n}\n"
        "clippedplane_geometry\n{\n name shape\n pta -2 -2 0\n ptb -2 2 0\n ptc 2 2 0\n ptd 2 -2 0\n doublesided TRUE\n}\n"
        "standard_object\n{\n name caster\n geometry shape\n material tinted\n}\n");
    std::vector<IShaderOp*> ops; IShader* shader=nullptr;
    if(!RISE_API_CreateStandardShader(&shader,ops)) return 2;
    RayCaster* caster=new RayCaster(false,16,*shader,true);
    loaded.Scene().GetObjects()->PrepareForRendering(); caster->AttachScene(&loaded.Scene());
    ManifoldSolverConfig config; config.biased=true; config.maxIterations=40; config.solverThreshold=1e-9;
    ManifoldSolver* solver=new ManifoldSolver(config);
    RandomNumberGenerator random(17); IndependentSampler sampler(random); IORStack stack(1);
    const Point3 start(0,0,-2), end(1,0,2);
    for(unsigned c=0;c<3;++c) {
        std::vector<ManifoldVertex> chain;
        solver->BuildSeedChain(start,end,loaded.Scene(),*caster,chain,true,&stack,&sampler);
        const auto result=solver->Solve(start,Vector3(0,0,1),end,Vector3(0,0,-1),chain,sampler);
        Check(result.valid,"master plane solve converges");
        if(!result.valid) continue;
        const auto& v=result.specularChain[0];
        const Vector3 wi=Vector3Ops::Normalize(Vector3Ops::mkVector3(v.position,start));
        const Vector3 wo=Vector3Ops::Normalize(Vector3Ops::mkVector3(end,v.position));
        const double n[]={1.3,1.5,1.9};
        Check(std::fabs(std::fabs(wi.x)-n[c]*std::fabs(wo.x))<1e-8,"authored component Snell oracle");
        auto actual=Hit(*loaded.Object("caster"),start,wi);
        const double tint=actual.pMaterial->GetSpecularInfo(actual.geometric,stack).attenuation[c];
        const double f=Optics::CalculateDielectricReflectanceCosine(std::fabs(wi.z),1,n[c]);
        Check(Near(result.contribution[c],tint*(1-f)/(n[c]*n[c])),"final-root selected-domain tint/Fresnel oracle");
    }
    LoadedScene nested(Materials(false)+Mesh(true,false)+
        "standard_object\n{\n name outer\n geometry shape\n material glass\n scale 2 2 2\n}\n"
        "sphere_geometry\n{\n name sphere\n radius 0.5\n}\n"
        "standard_object\n{\n name inner_obj\n geometry sphere\n material inner\n}\n");
    nested.Scene().GetObjects()->PrepareForRendering(); caster->AttachScene(&nested.Scene());
    IORStack live(1); live.SetCurrentObject(nested.Object("outer")); live.push(1.3);
    live.SetCurrentObject(nested.Object("inner_obj")); live.push(1.2);
    std::vector<ManifoldVertex> chain;
    solver->BuildSnellBaseSeed(Point3(0,0,0),Vector3(0,0,1),Point3(0,0,3),
        nested.Scene(),*caster,chain,&live,&sampler,450);
    Check(!chain.empty(),"master nested NM seed exists");
    if(!chain.empty()) Check(Near(chain[0].etaT,1.9),"nested NM exit restores wavelength-specific exterior");
    solver->release(); caster->release(); shader->release();
    std::cout << "committed master entry-point red proof: " << passCount << " passed, " << failCount << " failed\n";
    return failCount?1:0;
}
#endif
