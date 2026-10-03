// Native component and wavelength queries over real geometry. This suite
// tests domain/replay primitives; production partition gates are separate.
#include "SMSRenderTestSupport.h"
#include "../src/Library/Utilities/ManifoldSolver.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IObjectPriv.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Shaders/PathTracingIntegrator.h"
#include "../src/Library/Shaders/PathTracingShaderOp.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Utilities/RuntimeContext.h"
#include "../src/Library/RISE_API.h"
#include <sstream>
#include <cstring>
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/Optics.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"

static void ReportRecordSizes()
{
    std::cout<<"ABI bytes SpecularInfo="<<sizeof(SpecularInfo)<<" ManifoldVertex="<<sizeof(ManifoldVertex)
        <<" ManifoldSolverConfig="<<sizeof(ManifoldSolverConfig)<<" SMSChainRecord="<<sizeof(SMSChainRecord)<<"\n";
#ifdef RISE_SMS_DOMAIN_REPLAY
    std::cout<<"internal bytes domain vertex="<<sizeof(SMSDomainVertex)<<" medium capture="<<sizeof(SMSMediumCapture)
        <<" starting media="<<sizeof(SMSStartingMedia)<<" counters="<<sizeof(SMSDomainCounters)<<"\n";
#endif
}
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
        if(job) job->GetScene()->GetObjects()->PrepareForRendering();
    }
    ~LoadedScene() { safe_release(job); }
    const IScene& Scene() const { return *job->GetScene(); }
    const IObject* Object(const char* name) const { return Scene().GetObjects()->GetItem(name); }
};
static RayIntersection Hit(const IObject& object, Point3 origin, Vector3 direction,
    const RasterizerState& raster=nullRasterizerState)
{
    RayIntersection hit(Ray(origin,direction),raster);
    object.IntersectRay(hit,RISE_INFINITY,true,true,false);
    Check(hit.geometric.bHit,"actual geometry intersection");
    return hit;
}
class RasterTintPainter final : public UniformColorPainter {
public:
    RasterTintPainter() : UniformColorPainter(RISEPel(0.5)) {}
    RISEPel GetColor(const RayIntersectionGeometric& hit) const override {
        return RISEPel(0.2+0.01*hit.rast.x);
    }
    Scalar GetColorNM(const RayIntersectionGeometric& hit, Scalar) const override {
        return 0.2+0.01*hit.rast.y;
    }
};
#ifdef RISE_SMS_DOMAIN_REPLAY
static SMSDomainCounters domainCounters;
static void RasterContextCases()
{
    LoadedScene loaded(Materials(false)+
        "clippedplane_geometry\n{\n name shape\n pta -2 -2 0\n ptb -2 2 0\n ptc 2 2 0\n ptd 2 -2 0\n doublesided TRUE\n}\n"
        "standard_object\n{\n name caster\n geometry shape\n material glass\n}\n");
    auto* object=loaded.job->GetObjects()->GetItem("caster");
    if(!object) { Check(false,"raster-context caster"); return; }
    auto* tint=new RasterTintPainter(); auto* index=new UniformScalarPainter(1.5);
    IMaterial* material=nullptr;
    Check(RISE_API_CreatePerfectRefractorMaterial(&material,*tint,*index),"raster-dependent native material created");
    if(material) object->AssignMaterial(*material);
    Check(material&&object->GetMaterial()==material,"raster-dependent material assigned");
    safe_release(material); tint->release(); index->release();
    const Point3 start(0,0,-2), end(1,0,2);
    RasterizerState raster{}; raster.x=17; raster.y=23;
    auto hit=Hit(*object,start,Vector3(0,0,1),raster);
    for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),SMSQueryDomain::NM(550)}) {
        SMSDomainVertex record(hit.geometric); auto& v=record.geometry;
        v.position=hit.geometric.ptIntersection; v.normal=hit.geometric.vNormal;
        v.geomNormal=hit.geometric.UnflippedGeomNormal(); v.pObject=object; v.pMaterial=hit.pMaterial;
        v.isReflection=false; std::vector<SMSDomainVertex> records{record};
        ManifoldSolverConfig config; config.biased=true; config.solverThreshold=1e-9;
        config.maxIterations=40; config.domainCounters=&domainCounters;
        ManifoldSolver* solver=new ManifoldSolver(config);
        RandomNumberGenerator random(29); IndependentSampler sampler(random); IORStack stack(1);
        const auto result=solver->SolveDomain(start,Vector3(0,0,1),end,Vector3(0,0,-1),
            loaded.Scene(),stack,domain,records,sampler,1e-4);
        Check(result.valid,"raster-context native solve converges");
        if(result.valid) {
            const auto& root=result.specularChain[0];
            const auto incoming=Vector3Ops::Normalize(Vector3Ops::mkVector3(root.position,start));
            const double f=Optics::CalculateDielectricReflectanceCosine(std::fabs(incoming.z),1,1.5);
            const double expected=(domain.kind==SMSQueryDomain::RGBComponent?0.37:0.43)*(1-f)/2.25;
            Check(Near(domain.kind==SMSQueryDomain::RGBComponent?result.contribution[domain.component]:result.contributionNM,
                expected),"fresh native attenuation retains the caller's pixel context");
            Check(records[0].context.rast.x==17&&records[0].context.rast.y==23,"full refreshed context retains raster coordinates");
        }
        solver->release();
    }
}
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
            Check(!SMSDomainReplay::Query(*hit.pMaterial,hit.geometric,unseeded,SMSQueryDomain::NM(550),q),"NM composite caster rejected");
            SMSDomainVertex record(hit.geometric); record.geometry.position=hit.geometric.ptIntersection;
            record.geometry.normal=hit.geometric.vNormal; record.geometry.geomNormal=hit.geometric.UnflippedGeomNormal();
            record.geometry.pObject=outer; record.geometry.pMaterial=hit.pMaterial;
            record.geometry.isReflection=false;
            std::vector<SMSDomainVertex> records{record};
            ManifoldSolverConfig config; config.biased=true; config.domainCounters=&domainCounters;
            ManifoldSolver* solver=new ManifoldSolver(config);
            RandomNumberGenerator random(19); IndependentSampler sampler(random);
            const auto rejected=solver->SolveDomain(Point3(0,0,3),Vector3(0,0,-1),Point3(0,0,-3),Vector3(0,0,1),
                loaded.Scene(),unseeded,SMSQueryDomain::RGB(1),records,sampler,1e-4);
            Check(!rejected.valid&&rejected.specularChain.empty()&&rejected.contributionNM==0,
                "unsupported composite record is an ordinary zero solve trial");
            solver->release();
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
static double FilmReflectance(double ni,double nt,double cosine,double nm)
{
    const double nf=std::sqrt(1.5), thickness=550/(4*nf), sin2=1-cosine*cosine;
    const double cf=std::sqrt(1-ni*ni*sin2/(nf*nf)), ct=std::sqrt(1-ni*ni*sin2/(nt*nt));
    const double phase=4*3.14159265358979323846*nf*thickness*cf/nm;
    const auto airy=[phase](double a,double b) {
        return (a*a+b*b+2*a*b*std::cos(phase))/(1+a*a*b*b+2*a*b*std::cos(phase));
    };
    const double rs=airy((ni*cosine-nf*cf)/(ni*cosine+nf*cf),(nf*cf-nt*ct)/(nf*cf+nt*ct));
    const double rp=airy((nf*cosine-ni*cf)/(nf*cosine+ni*cf),(nt*cf-nf*ct)/(nt*cf+nf*ct));
    return (rs+rp)/2;
}
static void SolvedRootCases(bool coated=false)
{
    // Off-seed tint is evaluated at the converged point, independently of
    // its seed value. All three RGB geometries use their authored indices.
    std::string scene=Materials(false)+
        "expression_function2d\n{\n name tint_fn\n expr 0.2 + 0.3 * (u + v)\n}\n"
        "function2d_painter\n{\n name tint\n function2d tint_fn\n}\n"
        +std::string(coated?
        "dielectric_material\n{\n name tinted\n tau 1\n ior triple\n scattering 1000000\n ar_layer 1.224744871391589 112.26827987812466 0\n}\n":
        "perfectrefractor_material\n{\n name tinted\n refractance tint\n ior triple\n}\n")+
        "clippedplane_geometry\n{\n name sheet\n pta -2 -2 0\n ptb -2 2 0\n ptc 2 2 0\n ptd 2 -2 0\n doublesided TRUE\n}\n"
        "standard_object\n{\n name caster\n geometry sheet\n material tinted\n}\n";
    LoadedScene loaded(scene);
    const IObject* object=loaded.Object("caster");
    if(!object) { Check(false,"solved-root object"); return; }
    const Point3 start(0,0,-2), end(1,0,2);
    auto seed=Hit(*object,start,Vector3(0,0,1));
    ManifoldSolverConfig config;
    config.biased=true; config.maxIterations=40; config.solverThreshold=1e-9;
    config.domainCounters=&domainCounters;
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
        const double fresnel=coated?FilmReflectance(v.etaI,v.etaT,std::fabs(incoming.z),ScalarPainterRGB::kChannelNM[c]):
            Optics::CalculateDielectricReflectanceCosine(std::fabs(incoming.z),v.etaI,v.etaT);
        const double expected=tint*(1-fresnel)*(v.etaI/v.etaT)*(v.etaI/v.etaT);
        Check(Near(result.contribution[c],expected),"solved-root tint and native Fresnel refreshed");
        if(coated) Check(!Near(fresnel,FilmReflectance(v.etaI,v.etaT,1,ScalarPainterRGB::kChannelNM[c])),
            "solved-incidence coating oracle distinguishes seed-normal pricing");
        else Check(tint!=seed.pMaterial->GetSpecularInfo(seed.geometric,stack).attenuation[c],"off-seed tint oracle distinguishes stale query");
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
            config.domainCounters=&domainCounters;
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
static void SF11AbsentExteriorCases()
{
    const auto sf11=[](double nm) {
        const double x=nm*nm/1000000;
        return std::sqrt(1+1.73759695*x/(x-0.013188707)+0.313747346*x/(x-0.0623068142)
            +1.898781010*x/(x-155.23629));
    };
    for(bool reverse:{false,true}) for(int side:{-1,1}) {
        LoadedScene loaded(Materials(false)+
            "scalar_painter\n{\n name sf11\n sellmeier 1.73759695 0.313747346 1.898781010 0.013188707 0.0623068142 155.23629\n}\n"
            "perfectrefractor_material\n{\n name dispersive_outer\n refractance white\n ior sf11\n}\n"+
            Mesh(true,reverse)+
            "standard_object\n{\n name outer\n geometry shape\n material dispersive_outer\n position 3 0 0\n scale 2 2 2\n}\n"
            "sphere_geometry\n{\n name sphere\n radius 0.5\n}\n"
            "standard_object\n{\n name inner_obj\n geometry sphere\n material inner\n position 3 0 0\n}\n");
        const auto* outer=loaded.Object("outer"); const auto* inner=loaded.Object("inner_obj");
        if(!outer || !inner) { Check(false,"SF11 controls present"); continue; }
        const Point3 start(3.2,0,0), end(3.7,0,side*1.5);
        auto seed=Hit(*inner,start,Vector3Ops::Normalize(Vector3Ops::mkVector3(end,start)));
        IORStack live(1); live.SetCurrentObject(outer); live.push(sf11(611));
        live.SetCurrentObject(inner); live.push(1.2);
        std::vector<double> roots;
        for(double nm:{450,550,650}) {
            SMSDomainVertex record(seed.geometric); auto& v=record.geometry;
            v.position=seed.geometric.ptIntersection; v.normal=seed.geometric.vNormal;
            v.geomNormal=seed.geometric.UnflippedGeomNormal(); v.pObject=inner;
            v.pMaterial=seed.pMaterial; v.isReflection=false;
            std::vector<SMSDomainVertex> chain{record};
            ManifoldSolverConfig config; config.biased=true; config.maxIterations=40;
            config.solverThreshold=1e-9; config.domainCounters=&domainCounters;
            ManifoldSolver* solver=new ManifoldSolver(config);
            RandomNumberGenerator random(48); IndependentSampler sampler(random);
            const auto result=solver->SolveDomain(start,Vector3(0,0,side),end,Vector3(0,0,-side),
                loaded.Scene(),live,SMSQueryDomain::NM(nm),chain,sampler,1e-4);
            Check(result.valid,"nested SF11 exterior absent from chain converges");
            if(result.valid) {
                const auto& root=result.specularChain[0]; roots.push_back(root.position.x);
                Check(Near(root.etaI,1.2)&&Near(root.etaT,sf11(nm)),"independent Sellmeier index restored from starting membership");
                const auto wi=Vector3Ops::Normalize(Vector3Ops::mkVector3(root.position,start));
                const auto wo=Vector3Ops::Normalize(Vector3Ops::mkVector3(end,root.position));
                Check(std::fabs(1.2*Vector3Ops::Magnitude(Vector3Ops::Cross(wi,root.geomNormal))-
                    sf11(nm)*Vector3Ops::Magnitude(Vector3Ops::Cross(wo,root.geomNormal)))<1e-8,
                    "nested SF11 final-root native Snell oracle");
                std::cout<<"SF11 absent exterior reverse="<<reverse<<" side="<<side<<" nm="<<nm
                    <<" etaT="<<root.etaT<<" x="<<root.position.x<<'\n';
            }
            solver->release();
        }
        Check(roots.size()==3&&!Near(roots[0],roots[1])&&!Near(roots[1],roots[2]),"absent SF11 exterior changes NM root geometry");
    }
}
static void NativeTIRAndSheetCases()
{
    for(unsigned shape=0;shape<4;++shape) for(bool reverse:{false,true}) for(int side:{-1,1}) {
        const std::string geometry=shape<2?Mesh(shape==1,reverse):shape==2?
            "sphere_geometry\n{\n name shape\n radius 1\n}\n":
            "clippedplane_geometry\n{\n name shape\n pta -1 -1 0\n ptb -1 1 0\n ptc 1 1 0\n ptd 1 -1 0\n doublesided TRUE\n}\n";
        LoadedScene loaded(Materials(false)+geometry+
            "standard_object\n{\n name caster\n geometry shape\n material glass\n position 3 0 0\n}\n");
        const auto* object=loaded.Object("caster");
        if(!object) { Check(false,"TIR/sheet caster"); continue; }
        const Point3 origin=shape==2?Point3(3.6,0,0):shape==3?Point3(2.1,0,-side):Point3(2.1,0,0);
        const Vector3 direction=shape==2?Vector3(0,0,side):Vector3(0.6,0,shape==0?-0.8:side*0.8);
        auto hit=Hit(*object,origin,direction);
        for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),
            SMSQueryDomain::NM(450),SMSQueryDomain::NM(550),SMSQueryDomain::NM(650)}) {
            IORStack stack(1); SMSNativeMaterialQuery query;
            Check(SMSDomainReplay::Query(*hit.pMaterial,hit.geometric,stack,domain,query),"native TIR query");
            stack.SetCurrentObject(object); stack.push(query.index);
            const IORStack before(stack); double ni=0,nt=0,weight=0; bool exiting=false;
            Check(SMSDomainReplay::Cross(*hit.pMaterial,object,hit.geometric,domain,true,stack,ni,nt,exiting),"R remains supported at TIR");
            const bool expectedExit=!hit.geometric.bProvablyNoInterior||hit.geometric.TrueGeomFacing(direction)>0;
            Check(exiting==expectedExit&&Near(ni,expectedExit?query.index:1)&&Near(nt,expectedExit?1:query.index),
                "certified open plane uses native face rule; closed/uncertified geometry uses membership");
            Check(stack.SameInterfaces(before),"TIR reflection does not change membership");
            const double cosine=std::fabs(Vector3Ops::Dot(direction,hit.geometric.vNormal));
            const bool possible=ni*std::sqrt(1-cosine*cosine)<=nt;
            Check(SMSDomainReplay::EventWeight(*hit.pMaterial,hit.geometric,before,domain,true,exiting,ni,nt,1,weight),"native reflection priced at critical-angle control");
            Check(Near(weight,Optics::CalculateDielectricReflectanceCosine(cosine,ni,nt)),"native reflection includes selected-domain TIR law");
            Check(SMSDomainReplay::Cross(*hit.pMaterial,object,hit.geometric,domain,false,stack,ni,nt,exiting)==possible,
                "impossible T is rejected in its selected domain without relabeling");
            if(!possible) Check(stack.SameInterfaces(before),"rejected T preserves original stack");
        }
    }
}
static void FiniteDielectricCases()
{
    for(double exponent:{10000,100000,1000000}) for(bool closed:{false,true})
    for(bool reverse:{false,true}) for(int side:{-1,1}) {
        LoadedScene loaded(Materials(false)+
            "dielectric_material\n{\n name finite\n tau 1\n ior 1.5\n scattering "+std::to_string(exponent)+"\n}\n"+
            Mesh(closed,reverse)+"standard_object\n{\n name caster\n geometry shape\n material finite\n position 3 0 0\n}\n");
        const auto* object=loaded.Object("caster");
        if(!object) { Check(false,"finite dielectric caster"); continue; }
        auto hit=Hit(*object,Point3(3,0,side*3),Vector3(0,0,-side));
        IORStack stack(1);
        Check(hit.pMaterial->GetSpecularInfo(hit.geometric,stack).isSpecular,
            "native finite dielectric advertises its delta-tagged interaction");
        for(auto domain:{SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),
            SMSQueryDomain::NM(450),SMSQueryDomain::NM(550),SMSQueryDomain::NM(650)}) {
            SMSNativeMaterialQuery query;
            const bool supported=SMSDomainReplay::Query(*hit.pMaterial,hit.geometric,stack,domain,query);
            Check(supported,"adopted finite-dielectric delta-limit domain remains eligible");
            if(supported) Check(query.reflection&&query.transmission&&Near(query.index,1.5),
                "finite dielectric retains native index and both interface events");
            if(supported) Check(query.deltaLimitProxy==
                (hit.pMaterial->GetSPF()->DeltaTransmissionWarpExponent(hit.geometric,550)>-1),
                "query explicitly records the native finite-Phong approximation");
        }
    }
}
static std::vector<RISEColor> TraceCompositeGrid(LoadedScene& loaded, bool extended,
    bool startInside, unsigned salt, bool nm=false, std::vector<Scalar>* hwssLanes=nullptr)
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
        #ifdef RISE_SMS_SCENE_POLICY
        Check(!integrator->GetSolver()->ExtendedModeActive(loaded.Scene()),"prepared composite scene disables extended mode");
        Check(integrator->GetSolver()->ExtendedAnchorEligible(loaded.Scene(),*caster,
            Point3(0,0,-0.5),missing),"inert composite scene retains legacy coupled switches");
#else
        if(startInside) Check(!integrator->GetSolver()->ExtendedAnchorEligible(
            loaded.Scene(),*caster,Point3(0,0,-0.5),missing),"production eligibility rejects unseeded composite anchor");
#endif
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
            if(hwssLanes) {
                SampledWavelengths wavelengths=SampledWavelengths::SampleEquidistant(sampler.Get1D(),380,780);
                Scalar result[SampledWavelengths::N]{};
                integrator->IntegrateRayHWSS(context,raster,Ray(origin,direction),wavelengths,
                    loaded.Scene(),*caster,sampler,nullptr,result);
                for(unsigned w=0;w<SampledWavelengths::N;++w) hwssLanes->push_back(result[w]);
                sum=sum+RISEPel(result[0],result[1],result[2]);
            }
            else if(nm) sum=sum+RISEPel(integrator->IntegrateRayNM(context,raster,Ray(origin,direction),550,
                loaded.Scene(),*caster,sampler,nullptr,nullptr));
            else sum=sum+integrator->IntegrateRay(context,raster,Ray(origin,direction),
                loaded.Scene(),*caster,sampler,nullptr,nullptr);
        }
        pixels.emplace_back(sum*(1.0/32),1);
    }
    SobolSamplerTestHooks::ValueSalt().store(0);
    integrator->release(); caster->release(); shader->release();
    return pixels;
}
class DispatchSMSOp final : public PathTracingShaderOp {
public:
    mutable unsigned nmCalls=0, nestedCalls=0, lostMode=0;
    DispatchSMSOp(const ManifoldSolverConfig& config,const StabilityConfig& stability)
        : PathTracingShaderOp(config,stability) { SetMaxPathDepth(12); }
    void Prepare(const IScene& scene) {
        std::vector<const IObject*> casters;
        ManifoldSolver::EnumerateSpecularCasters(scene,casters);
        pIntegrator->GetSolver()->SetSpecularCasters(casters);
    }
    Scalar PerformOperationNM(const RuntimeContext& rc,const RayIntersection& hit,
        const IRayCaster& caster,const IRayCaster::RAY_STATE& state,Scalar accum,Scalar nm,
        const IORStack& stack,const ScatteredRayContainer* scat) const override {
        ++nmCalls;
        if(state.depth>1) ++nestedCalls;
#ifdef RISE_SMS_SCENE_POLICY
        if(!rc.smsForceLegacy) ++lostMode;
#endif
        return PathTracingShaderOp::PerformOperationNM(rc,hit,caster,state,accum,nm,stack,scat);
    }
};
static std::string DispatchScene(int kind,const std::string& compositeMaterial="")
{
    std::string text=Materials(true)+Mesh(false,false)+
        "standard_object\n{\n name pane\n geometry shape\n material glass\n}\n"
        "lambertian_material\n{\n name floor_mat\n reflectance white\n}\n"
        "clippedplane_geometry\n{\n name floor_geo\n pta -5 -5 0\n ptb -5 5 0\n ptc 5 5 0\n ptd 5 -5 0\n doublesided TRUE\n}\n"
        "standard_object\n{\n name receiver\n geometry floor_geo\n material floor_mat\n}\n"
        "omni_light\n{\n name source\n position 0.5 0 -2\n color 1 1 1\n power 40\n}\n";
    if(kind==0) text+=
        "homogeneous_medium\n{\n name zero_fog\n absorption 0 0 0\n scattering 0 0 0\n phase isotropic\n}\n"
        "global_medium\n{\n medium zero_fog\n}\n";
    if(kind>0) text+=std::string(kind==1?"subsurfacescattering_material":"randomwalk_sss_material")+
        "\n{\n name subject_mat\n ior 1.3\n absorption 0.1 0.1 0.1\n scattering 4\n g 0\n roughness 0\n}\n"
        "sphere_geometry\n{\n name subject_geo\n radius 0.25\n}\n"
        "standard_object\n{\n name subject\n geometry subject_geo\n material subject_mat\n position 0 0 -0.5\n}\n";
    if(!compositeMaterial.empty()) text+=compositeMaterial+
        "standard_object\n{\n name remote_composite\n geometry shape\n material wrapped\n position 1000 0 0\n}\n";
    return text;
}
static std::vector<Scalar> DispatchHWSS(LoadedScene& loaded,bool extended,unsigned salt,
    unsigned& nmCalls,unsigned& nestedCalls,unsigned& lostMode)
{
    ManifoldSolverConfig config; config.enabled=true; config.extendedMode=extended;
    config.seedingMode=ManifoldSolverConfig::eSeedingUniform; config.targetBounces=1;
    config.biased=true; config.multiTrials=4; config.photonCount=1;
    StabilityConfig stability; stability.rrMinDepth=20;
    auto* op=new DispatchSMSOp(config,stability); op->Prepare(loaded.Scene());
    std::vector<IShaderOp*> ops{op}; IShader* shader=nullptr;
    Check(RISE_API_CreateStandardShader(&shader,ops),"dispatch HWSS shader created");
    if(!shader) { op->release(); return {}; }
    auto* caster=new RayCaster(false,16,*shader,true); caster->AttachScene(&loaded.Scene());
    SobolSamplerTestHooks::ValueSalt().store(salt);
    std::vector<Scalar> values;
    for(unsigned sample=0;sample<128;++sample) {
        RandomNumberGenerator random(salt+sample); SobolSampler sampler(sample,17);
        RuntimeContext context(random,RuntimeContext::PASS_NORMAL,false); context.pSampler=&sampler;
        SampledWavelengths swl=SampledWavelengths::SampleEquidistant(sampler.Get1D(),380,780);
        Scalar result[SampledWavelengths::N]{}; IRayCaster::RAY_STATE state;
        IORStack stack(1);
        caster->CastRayHWSS(context,nullRasterizerState,Ray(Point3(0,0,-0.9),Vector3(0,0,1)),
            result,state,swl,nullptr,nullptr,stack);
        for(Scalar lane:result) values.push_back(lane);
#ifdef RISE_SMS_SCENE_POLICY
        Check(!context.smsForceLegacy,"HWSS cast restores caller mode after nested shader dispatch");
#endif
    }
    nmCalls=op->nmCalls; nestedCalls=op->nestedCalls; lostMode=op->lostMode;
    SobolSamplerTestHooks::ValueSalt().store(0);
    caster->release(); shader->release(); op->release();
    return values;
}
static void HWSSDispatchCases()
{
    for(int kind=0;kind<3;++kind) {
        LoadedScene loaded(DispatchScene(kind));
        for(unsigned trial=0;trial<4;++trial) {
            unsigned offNM=0,offNested=0,offLost=0,onNM=0,onNested=0,onLost=0;
            const unsigned salt=SobolSequence::HashCombine(1700+trial,0x48575353);
            const auto off=DispatchHWSS(loaded,false,salt,offNM,offNested,offLost);
            const auto on=DispatchHWSS(loaded,true,salt,onNM,onNested,onLost);
            bool finite=true; double sum=0;
            for(Scalar value:off) { finite=finite&&std::isfinite(value); sum+=value; }
            Check(finite&&sum>0,"dispatch HWSS legacy control is finite and lit");
            Check(!off.empty()&&off.size()==on.size()&&
                std::memcmp(off.data(),on.data(),off.size()*sizeof(Scalar))==0,
                "HWSS medium and SSS shader-dispatch fallback keeps legacy SMS bit-identically");
            if(kind==0) Check(onNM>0,"global medium exercises direct HWSS to NM shader fallback");
            else Check(onNested>0,"SSS exercises nested NM shader re-entry");
#ifdef RISE_SMS_SCENE_POLICY
            Check(onLost==0,"every dispatched NM continuation inherits HWSS anchor mode");
#endif
            std::cout<<"HWSS dispatch kind="<<kind<<" salt="<<salt<<" NM="<<onNM
                <<" nested="<<onNested<<" lost="<<onLost<<" sum="<<sum<<"\n";
        }
    }
}
static void PreparedCompositePolicyCases()
{
    const std::string wrappers[]={
        "lambertian_luminaire_material\n{\n name wrapped\n material layers\n exitance white\n scale 0\n}\n",
        "phong_luminaire_material\n{\n name wrapped\n material layers\n exitance white\n scale 0\n N 10\n}\n",
        "composite_material\n{\n name nested\n top layers\n bottom inner\n thickness 0.02\n extinction 0\n}\n"
        "lambertian_luminaire_material\n{\n name wrapped\n material nested\n exitance white\n scale 0\n}\n"};
    for(const auto& wrapper:wrappers) {
        LoadedScene loaded(DispatchScene(-1,wrapper));
        ManifoldSolverConfig config; config.extendedMode=true;
        auto* solver=new ManifoldSolver(config);
#ifdef RISE_SMS_SCENE_POLICY
        Check(!solver->ExtendedModeActive(loaded.Scene()),"prepared wrapped composite disables extended mode scene-wide");
        const auto* objects=dynamic_cast<const ObjectManager*>(loaded.Scene().GetObjects());
        Check(objects&&objects->FirstCompositeObject()=="remote_composite","prepared policy names first composite object");
#else
        Check(!config.extendedMode,"prepared scene must make requested extended mode inert");
#endif
        SMSStartingMedia uncertain; IORStack missing(1);
        Check(!SMSDomainReplay::Capture(loaded.Scene(),Point3(0,0,0),missing,uncertain),
            "prepared composite scene declines missing membership even outside composite bounds");
        solver->release();
        for(bool nm : {false,true}) for(unsigned trial=0;trial<4;++trial) {
            const unsigned salt=SobolSequence::HashCombine(2100+trial,0x434f4d50);
            const auto off=TraceCompositeGrid(loaded,false,false,salt,nm);
            const auto on=TraceCompositeGrid(loaded,true,false,salt,nm);
            double sum=0; bool finite=true;
            for(const auto& pixel:off) {
                sum+=pixel.base.r+pixel.base.g+pixel.base.b;
                finite=finite&&std::isfinite(pixel.base.r)&&std::isfinite(pixel.base.g)&&std::isfinite(pixel.base.b);
            }
            Check(finite&&sum>0,"remote wrapped composite legacy control is finite and lit");
            Check(HashPixels(off)==HashPixels(on),"remote wrapped composite keeps RGB/NM legacy rendering bit-identically");
        }
    }
    LoadedScene control(DispatchScene(-1));
    ManifoldSolverConfig config; config.extendedMode=true; auto* solver=new ManifoldSolver(config);
#ifdef RISE_SMS_SCENE_POLICY
    Check(solver->ExtendedModeActive(control.Scene()),"prepared composite-free scene still runs extended mode");
#endif
    solver->release();
    config.photonCount=1; solver=new ManifoldSolver(config);
    std::vector<IShaderOp*> ops; IShader* shader=nullptr;
    Check(RISE_API_CreateStandardShader(&shader,ops),"composite-free eligibility control shader");
    if(shader) {
        auto* caster=new RayCaster(false,16,*shader,true); caster->AttachScene(&control.Scene());
        IORStack empty(1);
        Check(!solver->ExtendedAnchorEligible(control.Scene(),*caster,Point3(0,0,-0.5),empty),
            "composite-free extended control executes eligibility rejection rather than legacy bypass");
        caster->release(); shader->release();
    }
    solver->release();
}

static void ParticipatingMediumCases()
{
    for(bool global : {false,true}) for(bool closed : {false,true})
        for(bool reverse : {false,true}) for(bool transformed : {false,true}) {
        const double offset=transformed?3:0;
        LoadedScene loaded(Materials(false)+
            "homogeneous_medium\n{\n name fog\n absorption 0.05 0.05 0.05\n scattering 0.6 0.6 0.6\n phase isotropic\n}\n"+
            (global?"global_medium\n{\n medium fog\n}\n":"")+Mesh(closed,reverse)+
            "standard_object\n{\n name caster\n geometry shape\n material glass\n position "+std::to_string(offset)+" 0 0\n"+
            (global?"":" interior_medium fog\n")+"}\n");
        const IObject* object=loaded.Object("caster");
        if(!object) { Check(false,"participating-medium caster exists"); continue; }
        Check(global?loaded.Scene().GetGlobalMedium()!=nullptr:object->GetInteriorMedium()!=nullptr,
            "real global or object participating medium configured");
        IORStack live(1); live.SetCurrentObject(object); live.push(1.3);
        SMSStartingMedia capture;
        Check(!SMSDomainReplay::Capture(loaded.Scene(),Point3(offset,0,0),live,capture),
            "participating starting medium is outside extended domain");
        std::vector<IShaderOp*> ops; IShader* shader=nullptr;
        Check(RISE_API_CreateStandardShader(&shader,ops),"medium eligibility shader exists");
        if(!shader) continue;
        RayCaster* caster=new RayCaster(false,16,*shader,true); caster->AttachScene(&loaded.Scene());
        ManifoldSolverConfig config; config.extendedMode=true; ManifoldSolver* solver=new ManifoldSolver(config);
        Check(!solver->ExtendedAnchorEligible(loaded.Scene(),*caster,Point3(offset,0,0),live),
            "participating starting medium disables coupled extended switches");
        solver->release(); caster->release(); shader->release();
        if(global) continue; // Global exclusion is scene-level; Cross has no scene argument.
        for(int side : {-1,1}) {
            const auto hit=Hit(*object,Point3(offset,0,side*3),Vector3(0,0,-side));
            for(auto domain : {SMSQueryDomain::RGB(0),SMSQueryDomain::RGB(1),SMSQueryDomain::RGB(2),
                SMSQueryDomain::NM(450),SMSQueryDomain::NM(550),SMSQueryDomain::NM(650)}) {
                for(bool reflection : {false,true}) {
                    IORStack stack(1); Scalar ni=0,nt=0; bool exiting=false;
                    const IORStack before(stack);
                    Check(!SMSDomainReplay::Cross(*hit.pMaterial,object,hit.geometric,domain,reflection,
                        stack,ni,nt,exiting),"participating-medium caster event is unsupported");
                    Check(stack.SameInterfaces(before),"unsupported medium event preserves membership");
                }
            }
        }
    }
}
static void PolishedEventCases()
{
    for(bool closed : {false,true}) for(bool reverse : {false,true}) for(bool transformed : {false,true}) {
        const double offset=transformed?3:0;
        LoadedScene loaded(Materials(false)+
            "uniformcolor_painter\n{\n name black\n color 0 0 0\n}\n"
            "scalar_painter\n{\n name coat_tint\n values 0.2 0.4 0.8\n}\n"
            "polished_material\n{\n name polished\n reflectance black\n tau coat_tint\n ior triple\n scattering 1000000\n}\n"+Mesh(closed,reverse)+
            "standard_object\n{\n name caster\n geometry shape\n material polished\n position "+std::to_string(offset)+" 0 0\n}\n");
        const IObject* object=loaded.Object("caster");
        if(!object) { Check(false,"polished caster exists"); continue; }
        for(int side : {-1,1}) {
            const auto hit=Hit(*object,Point3(offset,0,side*3),Vector3(0,0,-side));
            IORStack stack(1); stack.SetCurrentObject(object);
            RandomNumberGenerator random(421); IndependentSampler sampler(random);
            ScatteredRayContainer native;
            hit.pMaterial->GetSPF()->Scatter(hit.geometric,sampler,native,stack);
            Check(native.Count()>0 && native[0].isDelta,"polished native SPF emits delta coat first");
            for(unsigned c=0;c<3;++c) {
                Scalar ni=0,nt=0,weight=0; bool exiting=false; IORStack replay(stack);
                Check(SMSDomainReplay::Cross(*hit.pMaterial,object,hit.geometric,SMSQueryDomain::RGB(c),true,
                    replay,ni,nt,exiting),"polished reflection domain crossing");
                Check(SMSDomainReplay::EventWeight(*hit.pMaterial,hit.geometric,stack,SMSQueryDomain::RGB(c),
                    true,exiting,ni,nt,1,weight),"polished native RGB event query");
                Check(native.Count()>0 && native[0].isDelta && Near(weight,native[0].kray[c]),"polished RGB coat tint matches native SPF delta weight");
            }
            for(double nm : {450.,550.,650.}) {
                ScatteredRayContainer nativeNM;
                hit.pMaterial->GetSPF()->ScatterNM(hit.geometric,sampler,nm,nativeNM,stack);
                Scalar ni=0,nt=0,weight=0; bool exiting=false; IORStack replay(stack);
                Check(SMSDomainReplay::Cross(*hit.pMaterial,object,hit.geometric,SMSQueryDomain::NM(nm),true,
                    replay,ni,nt,exiting),"polished NM reflection domain crossing");
                Check(SMSDomainReplay::EventWeight(*hit.pMaterial,hit.geometric,stack,SMSQueryDomain::NM(nm),
                    true,exiting,ni,nt,1,weight),"polished native NM event query");
                Check(nativeNM.Count()>0 && nativeNM[0].isDelta && Near(weight,nativeNM[0].krayNM),"polished NM coat tint matches native SPF delta weight");
            }
        }
    }
}
static void CompositeCSGCases()
{
    for(bool reverse : {false,true}) {
        LoadedScene loaded(Materials(true)+Mesh(true,reverse)+
            "standard_object\n{\n name composite_child\n geometry shape\n material layers\n}\n"
            "sphere_geometry\n{\n name tiny\n radius 0.1\n}\n"
            "standard_object\n{\n name other\n geometry tiny\n material inner\n position 4 0 0\n}\n"
            "csg_object\n{\n name enclosing\n obja composite_child\n objb other\n operation union\n}\n");
        const IObject* enclosing=loaded.Object("enclosing");
        if(!enclosing) { Check(false,"composite CSG exists"); continue; }
        const auto hit=Hit(*enclosing,Point3(0,0,0),Vector3(0,0,1));
        Check(hit.pMaterial==loaded.Object("composite_child")->GetMaterial(),"CSG boundary uses real composite operand material");
        IORStack missing(1); SMSStartingMedia capture;
        Check(!SMSDomainReplay::Capture(loaded.Scene(),Point3(0,0,0),missing,capture),
            "DL-407 composite CSG membership uncertainty rejects anchor");
    }
}
static void CompositePTCases()
{
    for(bool nm:{false,true}) for(bool reverse : {false,true}) for(bool startInside : {false,true}) {
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
        std::vector<double> channelDelta[3], channelOff[3], channelOn[3];
        for(unsigned trial=0;trial<4;++trial) {
            const unsigned salt=SobolSequence::HashCombine(9000+trial,0x534d5344);
            const auto off=TraceCompositeGrid(loaded,false,startInside,salt,nm);
            const auto on=TraceCompositeGrid(loaded,true,startInside,salt,nm);
            if(!nm) {
                std::vector<Scalar> lanesOff,lanesOn;
                const auto hwssOff=TraceCompositeGrid(loaded,false,startInside,salt,false,&lanesOff);
                const auto hwssOn=TraceCompositeGrid(loaded,true,startInside,salt,false,&lanesOn);
                double laneSum=0; bool lanesFinite=true;
                for(Scalar value : lanesOff) { laneSum+=value; lanesFinite=lanesFinite && std::isfinite(value) && value>=0; }
                Check(lanesFinite && laneSum>0,"HWSS legacy composite control is finite and lit");
                Check(lanesOff.size()==64*32*SampledWavelengths::N && lanesOff.size()==lanesOn.size()
                    && std::memcmp(lanesOff.data(),lanesOn.data(),lanesOff.size()*sizeof(Scalar))==0,
                    "HWSS ignores extended mode bit-identically in every lane and NM delegation");
                Check(HashPixels(hwssOff)==HashPixels(hwssOn),"HWSS composite image keeps legacy output with extended flag on");
            }
            double a=0,b=0;
            for(const auto& pixel:off) a+=(pixel.base.r+pixel.base.g+pixel.base.b)/3;
            for(const auto& pixel:on) b+=(pixel.base.r+pixel.base.g+pixel.base.b)/3;
            a/=64; b/=64;
            for(unsigned c=0;c<3;++c) {
                double ca=0,cb=0;
                for(const auto& pixel:off) ca+=pixel.base[c];
                for(const auto& pixel:on) cb+=pixel.base[c];
                ca/=64; cb/=64;
                channelOff[c].push_back(ca); channelOn[c].push_back(cb); channelDelta[c].push_back(cb-ca);
            }
            Check(off.size()==64 && on.size()==64 && std::isfinite(a) && std::isfinite(b) && a>0 && b>0,
                "PT keeps composite-crossing emitter paths with extended mode on/off");
            if(!startInside) Check(HashPixels(off)==HashPixels(on),"deterministic composite camera path stays bit-identical");
            differences.push_back(b-a); offMeans.push_back(a); onMeans.push_back(b);
        }
        const auto d=Summarize(differences), a=Summarize(offMeans), b=Summarize(onMeans);
        std::cout << "composite PT nm=" << nm << " reverse=" << reverse << " startInside=" << startInside
            << " off=" << a.mean << " sd=" << a.sd << " on=" << b.mean << " sd=" << b.sd
            << " paired delta=" << d.mean << " sd=" << d.sd << " n=4\n";
        Check(d.mean==0 || std::fabs(d.mean)<=3*d.sd/2,"composite mode parity within three measured mean SDs");
        for(unsigned c=0;c<3;++c) {
            const auto dc=Summarize(channelDelta[c]), ac=Summarize(channelOff[c]), bc=Summarize(channelOn[c]);
            std::cout<<"composite channel="<<c<<" nm="<<nm<<" reverse="<<reverse<<" startInside="<<startInside
                <<" off="<<ac.mean<<" sd="<<ac.sd<<" on="<<bc.mean<<" sd="<<bc.sd
                <<" paired delta="<<dc.mean<<" sd="<<dc.sd<<" n=4\n";
            Check(dc.mean==0||std::fabs(dc.mean)<=3*dc.sd/2,"each composite channel matches within three measured mean SDs");
        }
    }
}
int main(int argc,char** argv)
{
    ReportRecordSizes();
    if(argc>1 && std::string(argv[1])=="--finite-only") {
        FiniteDielectricCases();
        std::cout<<passCount<<" passed, "<<failCount<<" failed\n";
        return failCount?1:0;
    }
    if(argc>1 && std::string(argv[1])=="--hwss-dispatch-only") {
        HWSSDispatchCases();
        std::cout<<passCount<<" passed, "<<failCount<<" failed\n";
        return failCount?1:0;
    }
    if(argc>1 && std::string(argv[1])=="--scene-policy-only") {
        PreparedCompositePolicyCases();
        std::cout<<passCount<<" passed, "<<failCount<<" failed\n";
        return failCount?1:0;
    }
    if(argc>1 && std::string(argv[1])=="--medium-only") {
        ParticipatingMediumCases();
        std::cout<<passCount<<" passed, "<<failCount<<" failed\n";
        return failCount?1:0;
    }
    if(argc>1 && std::string(argv[1])=="--polished-only") {
        PolishedEventCases();
        std::cout<<passCount<<" passed, "<<failCount<<" failed\n";
        return failCount?1:0;
    }
    if(argc>1 && std::string(argv[1])=="--composite-only") {
        CompositeCSGCases();
        std::cout<<passCount<<" passed, "<<failCount<<" failed\n";
        return failCount?1:0;
    }
    if(argc>1 && std::string(argv[1])=="--raster-only") {
        RasterContextCases();
        std::cout<<passCount<<" passed, "<<failCount<<" failed\n";
        return failCount?1:0;
    }
    ParticipatingMediumCases();
    PolishedEventCases();
    CompositeCSGCases();
    RasterContextCases();
    ComponentAndCrossingCases();
    NestedAndCompositeCases();
    SolvedRootCases();
    SolvedRootCases(true);
    CoatedEventCases();
    ClosedChainCases();
    SF11AbsentExteriorCases();
    NativeTIRAndSheetCases();
    FiniteDielectricCases();
    CompositePTCases();
    PreparedCompositePolicyCases();
    HWSSDispatchCases();
    Check(domainCounters.attempts.load()==domainCounters.acceptedRoots.load()+domainCounters.rejectedRoots.load(),
        "every domain trial is accounted as accepted or rejected");
    std::cout << "DOMAIN counters attempts=" << domainCounters.attempts.load()
        << " Newton iterations=" << domainCounters.newtonIterations.load()
        << " accepted=" << domainCounters.acceptedRoots.load()
        << " rejected=" << domainCounters.rejectedRoots.load()
        << " retries=0 tail events=0 owned=0 (isolated biased solves, no proposal/ownership estimator)\n";
    std::cout << passCount << " passed, " << failCount << " failed\n";
    return failCount?1:0;
}
#else
// Red proof: exercise real master entry points, rather than a mock of
// their bugs. New-domain primitives above cannot compile against master.
int main()
{
    ReportRecordSizes();
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
    auto* pixelObject=loaded.job->GetObjects()->GetItem("caster");
    auto* pixelTint=new RasterTintPainter(); auto* pixelIndex=new UniformScalarPainter(1.5);
    IMaterial* pixelMaterial=nullptr;
    Check(RISE_API_CreatePerfectRefractorMaterial(&pixelMaterial,*pixelTint,*pixelIndex),"master pixel material created");
    if(pixelMaterial) pixelObject->AssignMaterial(*pixelMaterial);
    Check(pixelMaterial&&pixelObject->GetMaterial()==pixelMaterial,"master pixel material assigned");
    safe_release(pixelMaterial); pixelTint->release(); pixelIndex->release();
    std::vector<ManifoldVertex> pixelChain;
    solver->BuildSnellBaseSeed(start,Vector3(0,0,1),end,loaded.Scene(),*caster,pixelChain,&stack,&sampler,550);
    const auto pixelResult=solver->Solve(start,Vector3(0,0,1),end,Vector3(0,0,-1),pixelChain,sampler);
    Check(pixelResult.valid,"master pixel-context plane solve converges");
    if(pixelResult.valid) {
        const auto& root=pixelResult.specularChain[0];
        const auto incoming=Vector3Ops::Normalize(Vector3Ops::mkVector3(root.position,start));
        const double f=Optics::CalculateDielectricReflectanceCosine(std::fabs(incoming.z),1,1.5);
        RasterizerState actualPixel{}; actualPixel.x=17; actualPixel.y=23;
        const auto actual=Hit(*pixelObject,start,incoming,actualPixel);
        const auto nativeRGB=actual.pMaterial->GetSpecularInfo(actual.geometric,stack).attenuation;
        const double nativeNM=actual.pMaterial->GetSpecularInfoNM(actual.geometric,stack,550).attenuationNM;
        for(unsigned c=0;c<3;++c) Check(Near(pixelResult.contribution[c],nativeRGB[c]*(1-f)/2.25),
            "master seed/solve preserves native caller pixel tint in RGB");
        Check(Near(solver->EvaluateChainThroughputNM(start,end,pixelResult.specularChain,550),nativeNM*(1-f)/2.25),
            "master seed/solve preserves native caller pixel tint in NM");
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
