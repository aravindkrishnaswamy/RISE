// Equal-solid-angle RGB environment proposal and actual probe-source controls.
// Small deterministic fixtures only: no rasterizer/render invocation.
#include "../src/Library/Rendering/EnvironmentSampler.h"
#include "../src/Library/Rendering/RadianceMap.h"
#include "../src/Library/Rendering/HosekWilkieSpectralRadianceMap.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Lights/PointLight.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Scene.h"
#include "../src/Library/Managers/LightManager.h"
#include "../src/Library/RISE_API.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
using namespace RISE;using namespace RISE::Implementation;
namespace {
int failures=0;
void Check(bool ok,const char* label){if(!ok){++failures;std::printf("FAIL: %s\n",label);}else if((std::string(label).find("[env-support]")!=std::string::npos||std::string(label).find("[env-retain]")!=std::string::npos))std::printf("PASS: %s\n",label);}
void Near(double a,double b,const char* label,double tolerance=1e-10){Check(std::isfinite(a)&&std::abs(a-b)<=tolerance*std::max(1.0,std::abs(b)),label);}
Vector3 Direction(double u,double v){const double z=1-2*v,r=2*std::sqrt(v*(1-v)),phi=TWO_PI*u;return Vector3(r*std::cos(phi),r*std::sin(phi),z);}
class ProbePainter final:public UniformColorPainter {
public:
    enum class Kind { Constant,Affine,Skew,Cap,MissedCap,CapAndPatch,Coordinates } kind;
    double magnitude=1;
    explicit ProbePainter(Kind k=Kind::Constant,double m=1):UniformColorPainter(RISEPel(.125,.125,.125)),kind(k),magnitude(m){}
    double Value(const RayIntersectionGeometric& ri) const {
        const double x=ri.ptCoord.x-.5,y=.5-ri.ptCoord.y;
        const double r=std::hypot(x,y),theta=TWO_PI*r;
        const Vector3 d(r>0?x*std::sin(theta)/r:0,r>0?y*std::sin(theta)/r:0,-std::cos(theta));
        if(kind==Kind::MissedCap)return magnitude*(d.z>.99?1:0);
        if(kind==Kind::CapAndPatch)return magnitude*((d.z>.99||d.z<-.25)?1:0);
        if(kind==Kind::Affine)return magnitude*(2+.5*d.z+.25*d.x);
        if(kind==Kind::Skew)return magnitude*((d.x>0&&d.z>.25)?1000:1);
        if(kind==Kind::Cap)return magnitude*((d.x>0&&d.z>.25)?1:0);
        return magnitude;
    }
    RISEPel GetRadianceColor(const RayIntersectionGeometric& ri) const override {
        if(kind==Kind::Coordinates)return RISEPel(ri.ptCoord.x,ri.ptCoord.y,0);
        const double v=Value(ri);return RISEPel(v,.5*v,.25*v);
    }
    Scalar GetRadianceNM(const RayIntersectionGeometric& ri,Scalar) const override {
        return kind==Kind::Coordinates?ri.ptCoord.x+2*ri.ptCoord.y:Value(ri);
    }
};
class PairSequence final:public ISampler {
    unsigned int index=0;double a,b;
public:
    PairSequence(double u,double v):a(u),b(v){}
    Scalar Get1D() override {++index;return index==1?.5:index==2?a:index==3?b:.37;}
    Point2 Get2D() override {return Point2(Get1D(),Get1D());}
};
class RootSequence final:public ISampler {
    bool initial=true;double first;
public:
    explicit RootSequence(double u):first(u){}
    Scalar Get1D() override {if(initial){initial=false;return first;}return .37;}
    Point2 Get2D() override {return Point2(Get1D(),Get1D());}
};
class UnitBSDF final:public IBSDF,public Reference {
public:
    RISEPel value(const Vector3&,const RayIntersectionGeometric&) const override {return RISEPel(.25,.25,.25);}
    Scalar valueNM(const Vector3&,const RayIntersectionGeometric&,Scalar) const override {return .25;}
};
class ClassFixture final:public LightSampler {
public:
    void SetInputs(Scalar radius,Scalar finiteWeight,const ILightPriv* point){
        cachedSceneRadius=radius;lightEntries.clear();
        if(finiteWeight>0){LightEntry e;e.pLight=point;e.exitance=finiteWeight;e.lumIndex=0;e.twoSided=false;e.position=Point3(0,0,0);lightEntries.push_back(e);aliasTable.Build({finiteWeight});}
        else aliasTable.Build({});
    }
};
void ConstantControls(){
    auto* painter=new ProbePainter(ProbePainter::Kind::Constant,4);
    const double h=1.0/4294967296.0;
    const double edges[]={0,h,.0001,.01,.5,.99,1-h,std::nextafter(1.0,0.0)};
    for(const unsigned int resolution:{1u,2u,3u,8u,64u,127u}){
        auto* env=new EnvironmentSampler(*painter,.5,Matrix4Ops::Identity(),resolution);env->Build();
        Near(env->TotalLuminance(),8*PI,"constant angular integral is resolution invariant");
        for(double u:edges)for(double v:edges){Vector3 direction;Scalar q;env->Sample(u,v,direction,q);
            Near(Vector3Ops::SquaredModulus(direction),1,"boundary sample stays on sphere");
            Near(q,1/(4*PI),"constant environment has uniform solid-angle density");Near(q,env->Pdf(direction),"boundary sample/query density agreement [env-equal-area-boundary]");}
        // Independent uniform-solid-angle quadrature of the queried density,
        // plus the estimator integral of positive-z cosine under the samples.
        double pdfIntegral=0,cosineIntegral=0,mz=0,mzz=0;unsigned int cap=0;
        const unsigned int n=128;
        for(unsigned int y=0;y<n;++y)for(unsigned int x=0;x<n;++x){
            const double u=(x+.5)/n,v=(y+.5)/n;pdfIntegral+=env->Pdf(Direction(u,v))*4*PI;
            Vector3 d;double q;env->Sample(u,v,d,q);cosineIntegral+=std::max(0.0,d.z)/q;mz+=d.z;mzz+=d.z*d.z;if(d.z>.5)++cap;
        }
        Near(pdfIntegral/(n*n),1,"queried density integrates to unity independently");
        Near(cosineIntegral/(n*n),PI,"sample estimator matches analytic hemisphere cosine integral",1e-9);
        Near(mz/(n*n),0,"constant sample first moment");Near(mzz/(n*n),(1-1.0/(n*n))/3,"constant sampled z-squared moment");
        Near(double(cap)/(n*n),.25,"constant sampled cap probability");env->release();
    }
    bool refused=false;try{auto* env=new EnvironmentSampler(*painter,1,Matrix4Ops::Identity(),0);env->release();}catch(const std::invalid_argument&){refused=true;}
    Check(refused,"zero resolution refused before painter retain");
    auto* tiny=new ProbePainter(ProbePainter::Kind::Constant,std::numeric_limits<double>::denorm_min());
    auto* tinyEnv=new EnvironmentSampler(*tiny,1,Matrix4Ops::Identity(),8);tinyEnv->Build();Vector3 tinyD;double tinyQ;tinyEnv->Sample(.2,.8,tinyD,tinyQ);Near(tinyQ,1/(4*PI),"subnormal constant weights normalize before division");Check(tinyEnv->TotalLuminance()>0,"subnormal angular integral stays positive");tinyEnv->release();tiny->release();
    auto* excessive=new ProbePainter(ProbePainter::Kind::Constant,1e308);auto* excessiveEnv=new EnvironmentSampler(*excessive,1,Matrix4Ops::Identity(),8);refused=false;try{excessiveEnv->Build();}catch(const std::invalid_argument&){refused=true;}Check(refused&&!excessiveEnv->IsValid(),"unrepresentable angular integral refused without stale valid table");excessiveEnv->release();excessive->release();
    auto* black=new EnvironmentSampler(*painter,0,Matrix4Ops::Identity(),8);black->Build();Check(!black->IsValid(),"legacy black scale remains invalid");Vector3 d;double q;black->Sample(.1,.2,d,q);Near(q,0,"black sample has zero density");black->release();painter->release();
}
void SkewAndRotationControls(){
    const Matrix4 rotation=Matrix4Ops::XRotation(.37)*Matrix4Ops::YRotation(-.82)*Matrix4Ops::ZRotation(.19);
    for(const auto kind:{ProbePainter::Kind::Affine,ProbePainter::Kind::Skew}){
        auto* painter=new ProbePainter(kind);auto* map=new RadianceMap(*painter,1);map->SetTransformation(rotation);
        auto* env=new EnvironmentSampler(*painter,1,rotation,64);env->Build();
        if(kind==ProbePainter::Kind::Affine)Near(env->TotalLuminance(),8*PI,"affine source angular integral",2e-8);
        double integral=0,estimate=0,meanMapZ=0;const unsigned int n=256;
        for(unsigned int y=0;y<n;++y)for(unsigned int x=0;x<n;++x){
            const double u=(x+.5)/n,v=(y+.5)/n;
            const Vector3 test=Vector3Ops::Transform(Matrix4Ops::Transpose(rotation),Direction(u,v));integral+=env->Pdf(test)*4*PI;
            Vector3 d;double q;env->Sample(u,v,d,q);Near(q,env->Pdf(d),"skew/rotated sample queried density");
            estimate+=map->GetRadiance(Ray(Point3(0,0,0),d),nullRasterizerState).r/q;
            meanMapZ+=Vector3Ops::Transform(rotation,d).z;
        }
        Near(integral/(n*n),1,"nonuniform rotated PDF integrates to unity",2e-8);
        const double expected=kind==ProbePainter::Kind::Affine?8*PI:(4*PI+999*(.375*PI*2));
        // The skew cap x>0,z>.25 occupies solid angle 3*pi/4.
        Near(estimate/(n*n),expected,"nonuniform sampled source integral agrees with analytic source",kind==ProbePainter::Kind::Affine?.001:.015);
        if(kind==ProbePainter::Kind::Affine)Near(meanMapZ/(n*n),(.95*(1-1.0/(64*64)))/12,"affine source sampling follows source weights",.001);
        const double edges[]={0,1.0/4294967296.0,.0001,.5,.99,std::nextafter(1.0,0.0)};
        for(double u:edges)for(double v:edges){Vector3 d;double q;env->Sample(u,v,d,q);Check(std::isfinite(q)&&q>0,"skew boundary density stays positive finite");Near(q,env->Pdf(d),"skew boundary/pole/rotation agreement");}
        map->release();env->release();painter->release();
    }
}
void IndependentCellFrequencyControls(){
    const unsigned int width=8,height=8,n=512;auto* painter=new ProbePainter(ProbePainter::Kind::Affine);
    const Matrix4 rotation=Matrix4Ops::XRotation(.37)*Matrix4Ops::ZRotation(-.28);
    auto* env=new EnvironmentSampler(*painter,1,rotation,width);env->Build();
    double cell[height][width]={},row[height]={};unsigned int hits[height][width]={};double total=0;
    for(unsigned int y=0;y<height;++y)for(unsigned int x=0;x<width;++x){
        const Vector3 d=Direction((x+.5)/width,(y+.5)/height);
        cell[y][x]=2+.5*d.z+.25*d.x;row[y]+=cell[y][x];total+=cell[y][x];
        const Vector3 world=Vector3Ops::Transform(Matrix4Ops::Transpose(rotation),d);
        Near(env->Pdf(world),(.95*cell[y][x]/2+.05)/(4*PI),"analytic cell source weight determines queried density");
    }
    for(unsigned int y=0;y<height;++y){row[y]=0;for(unsigned int x=0;x<width;++x){cell[y][x]=.95*cell[y][x]/total+.05/(width*height);row[y]+=cell[y][x];}}
    total=1;
    for(unsigned int y=0;y<n;++y)for(unsigned int x=0;x<n;++x){
        Vector3 world;double q;env->Sample((x+.5)/n,(y+.5)/n,world,q);const Vector3 d=Vector3Ops::Transform(rotation,world);
        double phi=std::atan2(d.y,d.x);if(phi<0)phi+=TWO_PI;
        const unsigned int cx=std::min(static_cast<unsigned int>(phi/TWO_PI*width),width-1);
        const unsigned int cy=std::min(static_cast<unsigned int>((1-d.z)*.5*height),height-1);++hits[cy][cx];
    }
    for(unsigned int y=0;y<height;++y)for(unsigned int x=0;x<width;++x){
        const double expected=cell[y][x]/total,pRow=row[y]/total,pColumn=cell[y][x]/row[y];
        // Each one-dimensional CDF interval differs from its midpoint-grid
        // count by at most one draw. Bound the product error independently.
        const double discrepancy=(pRow+pColumn)/n+1.0/(n*n)+1e-12;
        Check(std::abs(double(hits[y][x])/(n*n)-expected)<=discrepancy,"sampled cell frequencies match independent source masses [env-cell-frequency]");
    }
    env->release();painter->release();
    painter=new ProbePainter(ProbePainter::Kind::Cap);env=new EnvironmentSampler(*painter,1,Matrix4Ops::Identity(),width);env->Build();
    Near(env->TotalLuminance(),.75*PI,"zero rows and columns retain true cap angular integral");
    unsigned int inside=0;for(auto& hitRow:hits)for(auto& hit:hitRow)hit=0;
    for(unsigned int y=0;y<n;++y)for(unsigned int x=0;x<n;++x){Vector3 d;double q;env->Sample((x+.5)/n,(y+.5)/n,d,q);if(d.x>0&&d.z>.25)++inside;double phi=std::atan2(d.y,d.x);if(phi<0)phi+=TWO_PI;++hits[std::min(static_cast<unsigned int>((1-d.z)*.5*height),height-1)][std::min(static_cast<unsigned int>(phi/TWO_PI*width),width-1)];Check(q>0,"support mixture samples positive density outside midpoint cap");}
    Near(double(inside)/(n*n),.95+.05*(3.0/16),"cap plus uniform mixture draw frequencies",.003);
    double integral=0;
    for(unsigned int y=0;y<height;++y)for(unsigned int x=0;x<width;++x){
        const Vector3 d=Direction((x+.5)/width,(y+.5)/height);
        const double expected=.05/(4*PI)+(d.x>0&&d.z>.25?.95*4/(3*PI):0);
        Near(env->Pdf(d),expected,"every cell retains analytic support-mixture PDF [env-support]");
        Check(env->Pdf(d)>0,"every cell has positive support [env-support]");integral+=env->Pdf(d)*4*PI/(width*height);
        const double pCell=.05/(width*height)+(d.x>0&&d.z>.25?.95/12:0);
        const double pRow=.05/height+(y<3?.95/3:0),pColumn=pCell/pRow;
        Check(std::abs(double(hits[y][x])/(n*n)-pCell)<=(pRow+pColumn)/n+1.0/(n*n)+1e-12,"every cell draw frequency follows positive support mixture [env-support]");
    }
    Near(integral,1,"cap support mixture integrates independently to one [env-support]");
    Near(env->Pdf(Vector3(0,0,-1)),.05/(4*PI),"cap complement keeps explicit uniform density [env-support]");env->release();painter->release();
}
void BetweenProbeSupportControls(){
    for(const auto kind:{ProbePainter::Kind::MissedCap,ProbePainter::Kind::CapAndPatch}){
        auto* painter=new ProbePainter(kind);auto* map=new RadianceMap(*painter,1);
        auto* env=new EnvironmentSampler(*painter,1,Matrix4Ops::Identity(),64);env->Build();
        Check(env->IsValid(),"between-probe source stays valid [env-support]");
        if(kind==ProbePainter::Kind::MissedCap)Near(env->TotalLuminance(),0,"missed source keeps honest zero power estimate [env-support]");
        Check(env->Pdf(Vector3(0,0,1))>0,"between-probe cap has positive directional density [env-support]");
        auto* ls=new ClassFixture;auto* point=new PointLight(1,RISEPel(1,1,1),false);
        ls->SetInputs(1,0,point);ls->SetEnvironmentSampler(map,env);Near(ls->EnvSelectProbability(),1,"zero-estimate env-only root stays selectable [env-support]");
        ls->SetInputs(1,1,point);ls->SetEnvironmentSampler(map,env);Check(ls->EnvSelectProbability()>=.025&&ls->EnvSelectProbability()<=.975,"between-probe mixed root retains both classes [env-support]");
        if(kind==ProbePainter::Kind::MissedCap)Near(ls->EnvSelectProbability(),.025,"zero-estimate environment retains mixed root floor [env-support]");
        auto* scene=new Scene;LuminaryManager::LuminariesList empty;LightSample root;
        RootSequence envDraw(0),finiteDraw(.999);
        Check(ls->SampleLight(*scene,empty,envDraw,root)&&root.pEnvLight==map,"zero-estimate mixed root environment sample succeeds [env-support]");Near(root.pdfSelect,ls->EnvSelectProbability(),"between-probe environment root PDF matches query [env-support]");
        Check(ls->SampleLight(*scene,empty,finiteDraw,root)&&root.pLight==point,"between-probe mixed root finite sample succeeds [env-support]");Near(root.pdfSelect,ls->PdfSelectLight(*scene,empty,*point,Point3(0,0,0),Vector3(0,0,1)),"between-probe finite root PDF matches query [env-support]");
        scene->release();ls->release();point->release();env->release();map->release();painter->release();
    }
}
class DirectSequence final:public ISampler {
    unsigned int index=0;double a,b;bool finite;
public:
    DirectSequence(double u,double v,bool hasFinite):a(u),b(v),finite(hasFinite){}
    Scalar Get1D() override {const unsigned int i=index++;return finite?(i==0?.5:i==1?a:i==2?b:.37):(i==0?a:i==1?b:.37);}
    Point2 Get2D() override {return Point2(Get1D(),Get1D());}
};
void BetweenProbeDirectControls(){
    // Real direct-only NEE path: no BSDF continuation can recover missed source
    // support. At 64 rows all probe z <= .984375, below this .99 cap.
    for(const auto kind:{ProbePainter::Kind::MissedCap,ProbePainter::Kind::CapAndPatch})for(const bool finite:{false,true}){
        auto* scene=new Scene;IObjectManager* objects=nullptr;RISE_API_CreateObjectManager(&objects,false,false,4,32);scene->SetObjectManager(objects);
        auto* lights=new LightManager;scene->SetLightManager(lights);
        if(finite){auto* point=new PointLight(3,RISEPel(1,1,1),false);point->SetPosition(Point3(0,0,2));point->FinalizeTransformations();lights->AddItem(point,"point");point->release();}
        auto* painter=new ProbePainter(kind);auto* map=new RadianceMap(*painter,1);scene->SetGlobalRadianceMap(map);
        IShader* shader=nullptr;RISE_API_CreateStandardShader(&shader,{});auto* caster=new RayCaster(false,4,*shader,false);caster->AttachScene(scene);auto* bsdf=new UnitBSDF;
        RayIntersectionGeometric receiver(Ray(Point3(0,0,1),Vector3(0,0,-1)),nullRasterizerState);receiver.ptIntersection=Point3(0,0,0);receiver.vNormal=Vector3(0,0,1);receiver.vGeomNormal=receiver.vNormal;receiver.onb.CreateFromW(receiver.vNormal);
        double sum=0;const unsigned int n=1u<<19;
        // These axisymmetric sources make azimuth immaterial. Equal draws
        // keep this one-dimensional quadrature independent of C++ argument
        // evaluation order at the production sampler call.
        for(unsigned int y=0;y<n;++y){DirectSequence sequence((y+.5)/n,(y+.5)/n,finite);sum+=caster->GetLightSampler()->EvaluateDirectLighting(receiver,*bsdf,nullptr,*caster,sequence,nullptr,nullptr,false,nullptr).r;}
        // UnitBSDF is .25, so the analytic cap integral pi*(1-.99^2)
        // contributes one quarter of that; the lower-hemisphere patch is dark
        // to this receiver, while the point contributes exactly .1875.
        Near(sum/n,.25*PI*(1-.99*.99)+(finite?.1875:0),"direct-only between-probe cap irradiance [env-support]",.00012);
        bsdf->release();caster->release();shader->release();map->release();painter->release();lights->release();objects->release();scene->release();
    }
}

void ActualSourcePoleControls(){
    auto* painter=new ProbePainter(ProbePainter::Kind::Coordinates);auto* map=new RadianceMap(*painter,3);
    const double sub=std::numeric_limits<double>::denorm_min();
    const Vector3 directions[]={Vector3(0,0,1),Vector3(0,0,-1),Vector3(sub,0,1),Vector3(-sub,0,1),Vector3(sub,sub,1),Vector3(sub,sub,-1)};
    const Point2 expected[]={Point2(1,.5),Point2(.5,.5),Point2(1,.5),Point2(0,.5),Point2(.5+.5/std::sqrt(2.0),.5-.5/std::sqrt(2.0)),Point2(.5,.5)};
    for(unsigned int i=0;i<6;++i){const Ray ray(Point3(0,0,0),directions[i]);const auto rgb=map->GetRadiance(ray,nullRasterizerState);const double nm=map->GetRadianceNM(ray,nullRasterizerState,1200);
        Near(rgb.r,3*expected[i].x,"actual source RGB stable pole/near-axis u [probe-pole-formulation]");Near(rgb.g,3*expected[i].y,"actual source RGB stable pole/near-axis v");Near(nm,3*(expected[i].x+2*expected[i].y),"actual source NM shares pole mapping");}
    const double theta=std::atan2(std::sqrt(5.0),-1.0),radius=theta/TWO_PI;
    const Ray mixed(Point3(0,0,0),Vector3(sub,2*sub,sub));
    const Point2 mixedExpected(.5+radius/std::sqrt(5.0),.5-2*radius/std::sqrt(5.0));
    Near(map->GetRadiance(mixed,nullRasterizerState).r,3*mixedExpected.x,"mixed subnormal source RGB uses bounded exponent normalization");
    Near(map->GetRadianceNM(mixed,nullRasterizerState,1200),3*(mixedExpected.x+2*mixedExpected.y),"mixed subnormal source NM uses bounded exponent normalization");
    auto* affine=new ProbePainter(ProbePainter::Kind::Affine);auto* lookup=new EnvironmentSampler(*affine,1,Matrix4Ops::Identity(),8);lookup->Build();
    Near(lookup->Pdf(Vector3(sub,2*sub,sub)),lookup->Pdf(Vector3(1,2,1)),"sampler query normalizes mixed subnormal direction without reciprocal overflow");
    Near(lookup->Pdf(Vector3(sub,sub,1)),lookup->Pdf(Vector3(1e-100,1e-100,1)),"sampler preserves near-axis azimuth across exponent scales");lookup->release();affine->release();
    Matrix4 rotate=Matrix4Ops::Identity();rotate._00=0;rotate._22=0;rotate._20=1;rotate._02=-1;map->SetTransformation(rotate);
    const Vector3 world=Vector3Ops::Transform(Matrix4Ops::Transpose(rotate),Vector3(0,0,1));
    Near(map->GetRadiance(Ray(Point3(0,0,0),world),nullRasterizerState).r,3,"actual rotated RGB source reaches canonical +Z edge");
    Near(map->GetRadianceNM(Ray(Point3(0,0,0),world),nullRasterizerState,1200),6,"actual rotated NM source reaches canonical +Z edge");
    const Vector3 rotatedMixed=Vector3Ops::Transform(Matrix4Ops::Transpose(rotate),Vector3(sub,2*sub,sub));
    Near(map->GetRadiance(Ray(Point3(0,0,0),rotatedMixed),nullRasterizerState).r,3*mixedExpected.x,"rotated mixed-subnormal RGB source mapping");
    Near(map->GetRadianceNM(Ray(Point3(0,0,0),rotatedMixed),nullRasterizerState,1200),3*(mixedExpected.x+2*mixedExpected.y),"rotated mixed-subnormal NM source mapping");
    affine=new ProbePainter(ProbePainter::Kind::Affine);lookup=new EnvironmentSampler(*affine,1,rotate,8);lookup->Build();
    Near(lookup->Pdf(rotatedMixed),lookup->Pdf(Vector3Ops::Transform(Matrix4Ops::Transpose(rotate),Vector3(1,2,1))),"rotated mixed-subnormal sampler lookup");lookup->release();affine->release();
    auto* domainEnv=new EnvironmentSampler(*map,SpectralSourceDomain::Create({{1200,1}}));domainEnv->Build();Vector3 d;double q;domainEnv->Sample(.2,.8,d,q);Near(q,1/(4*PI),"spectral uniform proposal unchanged");Near(q,domainEnv->Pdf(d),"spectral uniform sample/query unchanged");
    domainEnv->release();map->release();painter->release();
}
void RootPowerControls(){
    auto* point=new PointLight(1,RISEPel(1,1,1),false);
    const struct {double radiance,radius,finiteWeight,expected;} cases[]={
        {1e307,1,1e308,(4*PI*PI)/(4*PI*PI+10)},
        {1e307,1e-150,1e8,(4*PI*PI*.1)/(4*PI*PI*.1+1)},
        {1e-307,1e6,1e-294,(4*PI*PI*.1)/(4*PI*PI*.1+1)}
    };
    for(const auto& c:cases){auto* p=new ProbePainter(ProbePainter::Kind::Constant,c.radiance);auto* map=new RadianceMap(*p,1);auto* env=new EnvironmentSampler(*p,1,Matrix4Ops::Identity(),8);env->Build();auto* ls=new ClassFixture;ls->SetInputs(c.radius,c.finiteWeight,point);ls->SetEnvironmentSampler(map,env);Near(ls->EnvSelectProbability(),.025+.95*c.expected,"log-domain legacy root ratio avoids intermediate overflow/underflow",2e-10);Check(ls->EnvSelectProbability()>0&&ls->EnvSelectProbability()<1,"supported extreme root classes keep positive shares");
        auto* scene=new Scene;LuminaryManager::LuminariesList empty;LightSample root;
        RootSequence environmentDraw(0),finiteDraw(1-1.0/4294967296.0);
        Check(ls->SampleLight(*scene,empty,environmentDraw,root)&&root.pEnvLight==map,"supported extreme environment root is sampleable");Near(root.pdfSelect,ls->EnvSelectProbability(),"environment sampled root share matches query");Near(root.pdfDirection,env->Pdf(-root.direction),"environment sampled root direction density matches query");
        Check(ls->SampleLight(*scene,empty,finiteDraw,root)&&root.pLight==point,"supported extreme finite root is sampleable");Near(root.pdfSelect,ls->PdfSelectLight(*scene,empty,*point,Point3(0,0,0),Vector3(0,0,1)),"finite sampled root share matches actual PMF query");scene->release();ls->release();env->release();map->release();p->release();}
    for(const double radiance:{1e307,1e-307}){auto* p=new ProbePainter(ProbePainter::Kind::Constant,radiance);auto* map=new RadianceMap(*p,1);auto* env=new EnvironmentSampler(*p,1,Matrix4Ops::Identity(),8);env->Build();auto* ls=new ClassFixture;ls->SetInputs(1,1,point);ls->SetEnvironmentSampler(map,env);Near(ls->EnvSelectProbability(),radiance>1?.975:.025,"extreme legacy root retains disclosed support share [env-support]");ls->release();env->release();map->release();p->release();}
    {
        auto* p=new ProbePainter;auto* map=new RadianceMap(*p,1);auto* env=new EnvironmentSampler(*p,1,Matrix4Ops::Identity(),8);env->Build();auto* ls=new ClassFixture;
        ls->SetInputs(1,std::numeric_limits<double>::infinity(),point); // HYGIENE-OK: intentional rejected upstream finite-power overflow, not a sentinel.
        bool rejected=false;try{ls->SetEnvironmentSampler(map,env);}catch(const std::invalid_argument&){rejected=true;}Check(rejected,"nonfinite upstream finite-light power explicitly refused");ls->release();env->release();map->release();p->release();
    }
    auto* p=new ProbePainter;auto* map=new RadianceMap(*p,1);auto* env=new EnvironmentSampler(*p,1,Matrix4Ops::Identity(),8);env->Build();auto* ls=new ClassFixture;ls->SetInputs(1,0,point);ls->SetEnvironmentSampler(map,env);Near(ls->EnvSelectProbability(),1,"env-only class keeps exact one");ls->SetInputs(0,1,point);ls->SetEnvironmentSampler(map,env);Near(ls->EnvSelectProbability(),0,"zero-radius class keeps exact zero");ls->release();env->release();map->release();p->release();point->release();
}
class LifetimeEnvironment final:public EnvironmentSampler {
    int& destructions;
    ~LifetimeEnvironment() override {++destructions;}
public:
    LifetimeEnvironment(const IPainter& painter,int& destroyed):EnvironmentSampler(painter,1,Matrix4Ops::Identity(),8),destructions(destroyed){}
    LifetimeEnvironment(const IRadianceMap& map,std::shared_ptr<const SpectralSourceDomain> domain,int& destroyed):EnvironmentSampler(map,std::move(domain)),destructions(destroyed){}
};
class LifetimeMap final:public RadianceMap {
    int& destructions;
    const EnvironmentSampler* child;
    ~LifetimeMap() override {if(child)child->release();++destructions;}
public:
    LifetimeMap(const IPainter& painter,int& destroyed,const EnvironmentSampler* owned=nullptr):RadianceMap(painter,1),destructions(destroyed),child(owned){if(child)child->addref();}
};
void QueryRetainedEnvironment(ClassFixture& lights,const IRadianceMap* map,const EnvironmentSampler* env){
    Check(lights.GetEnvironmentSampler()==env,"retained environment identity survives replacement [env-retain]");
    Near(env->Pdf(Vector3(0,0,1)),1/(4*PI),"retained environment queried PDF stays live [env-retain]");
    auto* scene=new Scene;LuminaryManager::LuminariesList empty;RootSequence sequence(.37);LightSample sample;
    const bool sampled=lights.SampleLight(*scene,empty,sequence,sample);
    Check(sampled&&sample.pEnvLight==map,"retained map supports actual root emission query [env-retain]");
    if(sampled){Check(sample.Le.r>0,"retained map root emission remains positive [env-retain]");Near(sample.pdfSelect,1,"retained environment root PMF stays one [env-retain]");}
    scene->release();
}
void EnvironmentSetterLifetimeControls(int only=-1){
    // Separate last-reference sampler/map arms and both at once. Destruction
    // witnesses permit cleanup checks without dereferencing released objects.
    for(int mode=0;mode<3;++mode){
        if(only>=0&&only!=mode)continue;
        int mapDestroyed=0,envDestroyed=0;auto* painter=new ProbePainter;
        auto* map=new LifetimeMap(*painter,mapDestroyed);auto* env=new LifetimeEnvironment(*painter,envDestroyed);env->Build();
        auto* lights=new ClassFixture;lights->SetInputs(1,0,nullptr);lights->SetEnvironmentSampler(map,env);
        const bool dropEnv=mode!=1,dropMap=mode!=0;
        if(dropEnv)env->release();if(dropMap)map->release();
        lights->SetEnvironmentSampler(map,lights->GetEnvironmentSampler());
        Check(mapDestroyed==0&&envDestroyed==0,"same-pointer setter preserves last-reference inputs [env-retain]");
        if(mapDestroyed==0&&envDestroyed==0)QueryRetainedEnvironment(*lights,map,env);
        lights->SetEnvironmentSampler(nullptr,nullptr);
        Check(mapDestroyed==(dropMap?1:0)&&envDestroyed==(dropEnv?1:0),"clearing environment releases exactly its retained references [env-retain]");
        if(!dropEnv)env->release();if(!dropMap)map->release();
        Check(mapDestroyed==1&&envDestroyed==1,"environment setter has no retained-reference leak [env-retain]");
        lights->release();Near(painter->refcount(),1,"environment cleanup releases source painter owners [env-retain]");painter->release();
    }
    if(only<0||only==3){
        // Natural cross-ownership: an old spectral sampler is the only owner
        // of the incoming map. Releasing that sampler first destroys the map.
        int mapDestroyed=0,oldDestroyed=0,newDestroyed=0;auto* painter=new ProbePainter;
        auto* map=new LifetimeMap(*painter,mapDestroyed);
        auto* oldEnv=new LifetimeEnvironment(*map,SpectralSourceDomain::Create({{550,1}}),oldDestroyed);oldEnv->Build();
        auto* nextEnv=new LifetimeEnvironment(*painter,newDestroyed);nextEnv->Build();
        auto* lights=new ClassFixture;lights->SetInputs(1,0,nullptr);lights->SetEnvironmentSampler(nullptr,oldEnv);oldEnv->release();map->release();
        lights->SetEnvironmentSampler(map,nextEnv);nextEnv->release();
        Check(oldDestroyed==1&&mapDestroyed==0&&newDestroyed==0,"old spectral sampler may solely own incoming map [env-retain]");
        if(mapDestroyed==0&&newDestroyed==0)QueryRetainedEnvironment(*lights,map,nextEnv);
        lights->SetEnvironmentSampler(nullptr,nullptr);lights->release();
        Check(mapDestroyed==1&&oldDestroyed==1&&newDestroyed==1,"spectral cross-owner replacement releases each object once [env-retain]");
        Near(painter->refcount(),1,"spectral cross-owner cleanup releases painter owners [env-retain]");painter->release();
    }
    if(only<0||only==4){
        // The IRadianceMap interface permits ownership in the other direction:
        // releasing the previous map must not destroy an incoming sampler.
        int oldMapDestroyed=0,newMapDestroyed=0,envDestroyed=0;auto* painter=new ProbePainter;
        auto* env=new LifetimeEnvironment(*painter,envDestroyed);env->Build();
        auto* oldMap=new LifetimeMap(*painter,oldMapDestroyed,env);env->release();
        auto* nextMap=new LifetimeMap(*painter,newMapDestroyed);auto* lights=new ClassFixture;lights->SetInputs(1,0,nullptr);lights->SetEnvironmentSampler(oldMap,nullptr);oldMap->release();
        lights->SetEnvironmentSampler(nextMap,env);nextMap->release();
        Check(oldMapDestroyed==1&&envDestroyed==0&&newMapDestroyed==0,"old map may solely own incoming sampler [env-retain]");
        if(envDestroyed==0&&newMapDestroyed==0)QueryRetainedEnvironment(*lights,nextMap,env);
        lights->SetEnvironmentSampler(nullptr,nullptr);lights->release();
        Check(oldMapDestroyed==1&&newMapDestroyed==1&&envDestroyed==1,"reverse cross-owner replacement releases each object once [env-retain]");
        Near(painter->refcount(),1,"reverse cross-owner cleanup releases painter owners [env-retain]");painter->release();
    }
    if(only<0||only==5){
        // Recompute can reject invalid cached finite power after ownership is
        // transferred. The setter still owns both inputs until cleared.
        int mapDestroyed=0,envDestroyed=0;auto* painter=new ProbePainter;auto* map=new LifetimeMap(*painter,mapDestroyed);auto* env=new LifetimeEnvironment(*painter,envDestroyed);env->Build();
        auto* lights=new ClassFixture;lights->SetInputs(1,0,nullptr);lights->SetEnvironmentSampler(map,env);map->release();env->release();
        lights->SetInputs(1,std::numeric_limits<double>::infinity(),nullptr); // HYGIENE-OK: intentional rejected power, not a sentinel.
        bool rejected=false;try{lights->SetEnvironmentSampler(map,env);}catch(const std::invalid_argument&){rejected=true;}
        Check(rejected&&mapDestroyed==0&&envDestroyed==0,"failed probability recompute retains valid owned inputs [env-retain]");
        lights->SetInputs(1,0,nullptr);lights->SetEnvironmentSampler(nullptr,nullptr);lights->release();
        Check(mapDestroyed==1&&envDestroyed==1,"rejected recompute cleanup releases each input once [env-retain]");Near(painter->refcount(),1,"rejected recompute cleanup releases painter owners [env-retain]");painter->release();
    }
}
class LifetimeKernel final:public ISampling2D,public Reference {
    int& destructions;unsigned int count=1;
    ~LifetimeKernel() override {++destructions;}
public:
    explicit LifetimeKernel(int& destroyed):destructions(destroyed){}
    void SetNumSamples(unsigned int n) override {count=n;}
    unsigned int GetNumSamples() override {return count;}
    void GenerateSamplePoints(const RandomNumberGenerator&,SamplesList2D& points) const override {points.assign(count,Point2(.25,.75));}
    ISampling2D* Clone() const override {auto* result=new LifetimeKernel(destructions);result->SetNumSamples(count);return result;}
};
class KernelCaster final:public RayCaster {
public:
    using RayCaster::RayCaster;
    ISampling2D* Kernel() const {return pLumSampling;}
};
void LuminaireSetterLifetimeControl(){
    int destroyed=0;IShader* shader=nullptr;RISE_API_CreateStandardShader(&shader,{});auto* caster=new KernelCaster(false,4,*shader,false);
    auto* kernel=new LifetimeKernel(destroyed);kernel->SetNumSamples(3);caster->SetLuminaireSampling(kernel);kernel->release();
    caster->SetLuminaireSampling(caster->Kernel());
    Check(destroyed==0,"luminaire setter preserves same-pointer last owner [env-retain]");
    if(destroyed==0){Near(caster->Kernel()->GetNumSamples(),3,"retained luminaire kernel supports real count query [env-retain]");ISampling2D::SamplesList2D points;RandomNumberGenerator random;caster->Kernel()->GenerateSamplePoints(random,points);Check(points.size()==3&&points[0].x==.25&&points[0].y==.75,"retained luminaire kernel generates samples [env-retain]");}
    caster->SetLuminaireSampling(nullptr);Check(destroyed==1&&caster->Kernel()==nullptr,"clearing luminaire kernel releases final reference [env-retain]");caster->release();shader->release();Check(destroyed==1,"luminaire kernel is destroyed exactly once [env-retain]");
}

void SkyPainterLifetimeControl(){
    auto* sky=new HosekWilkieSpectralRadianceMap(.7,.2,3,RISEPel(0,0,0),1);
    const IPainter& painter=sky->GetPainter();Check(painter.refcount()==1,"sky owns exactly the new adapter initial reference");painter.addref();
    RayIntersectionGeometric rig(Ray(Point3(0,0,0),Vector3(0,1,0)),nullRasterizerState);rig.ptCoord=Point2(.25,.25);
    const RISEPel before=painter.GetRadianceColor(rig);const double beforeNM=painter.GetRadianceNM(rig,550);
    Check(ColorMath::MaxValue(before)>0&&beforeNM>0,"retained sky painter fixture emits in supported visible band");
    sky->release();Check(painter.refcount()==1,"sky destruction releases its adapter reference without leak");
    const RISEPel after=painter.GetRadianceColor(rig);Near(after.r,before.r,"retained sky painter RGB survives parent destruction");Near(after.g,before.g,"retained sky painter green survives parent destruction");Near(after.b,before.b,"retained sky painter blue survives parent destruction");
    Near(painter.GetRadianceNM(rig,550),beforeNM,"retained sky painter NM survives parent destruction");painter.release();
}
void NonHyliosDirectControl(){
    auto* scene=new Scene;IObjectManager* objects=nullptr;RISE_API_CreateObjectManager(&objects,false,false,4,32);scene->SetObjectManager(objects);
    auto* lights=new LightManager;scene->SetLightManager(lights);auto* point=new PointLight(3,RISEPel(1,1,1),false);point->SetPosition(Point3(0,0,2));point->FinalizeTransformations();lights->AddItem(point,"point");point->release();
    auto* painter=new ProbePainter(ProbePainter::Kind::Constant,4);auto* map=new RadianceMap(*painter,1);scene->SetGlobalRadianceMap(map);
    IShader* shader=nullptr;RISE_API_CreateStandardShader(&shader,{});auto* caster=new RayCaster(false,4,*shader,false);caster->AttachScene(scene);auto* bsdf=new UnitBSDF;
    RayIntersectionGeometric receiver(Ray(Point3(0,0,1),Vector3(0,0,-1)),nullRasterizerState);receiver.ptIntersection=Point3(0,0,0);receiver.vNormal=Vector3(0,0,1);receiver.vGeomNormal=receiver.vNormal;receiver.onb.CreateFromW(receiver.vNormal);
    RISEPel sum(0,0,0);const unsigned int n=128;
    for(unsigned int y=0;y<n;++y)for(unsigned int x=0;x<n;++x){PairSequence sequence((x+.5)/n,(y+.5)/n);sum=sum+caster->GetLightSampler()->EvaluateDirectLighting(receiver,*bsdf,nullptr,*caster,sequence,nullptr,nullptr,false,nullptr);}
    Near(sum.r/(n*n),PI+.1875,"non-Hylios RGB direct point plus environment red oracle",1e-9);Near(sum.g/(n*n),.5*PI+.1875,"non-Hylios RGB direct point plus environment green oracle",1e-9);Near(sum.b/(n*n),.25*PI+.1875,"non-Hylios RGB direct point plus environment blue oracle",1e-9);
    bsdf->release();caster->release();shader->release();map->release();painter->release();lights->release();objects->release();scene->release();
}
}
int main(int argc,char** argv){const std::string filter=argc>1?argv[1]:"all";if(filter=="all"||filter=="cdf"){ConstantControls();SkewAndRotationControls();IndependentCellFrequencyControls();}if(filter=="all"||filter=="retain-env")EnvironmentSetterLifetimeControls();if(filter=="retain-sampler")EnvironmentSetterLifetimeControls(0);if(filter=="retain-map")EnvironmentSetterLifetimeControls(1);if(filter=="retain-both")EnvironmentSetterLifetimeControls(2);if(filter=="retain-cross-map")EnvironmentSetterLifetimeControls(3);if(filter=="retain-cross-sampler")EnvironmentSetterLifetimeControls(4);if(filter=="all"||filter=="retain-kernel")LuminaireSetterLifetimeControl();if(filter=="all"||filter=="support"){BetweenProbeSupportControls();BetweenProbeDirectControls();}if(filter=="all"||filter=="poles")ActualSourcePoleControls();if(filter=="all"||filter=="roots")RootPowerControls();if(filter=="all"||filter=="direct")NonHyliosDirectControl();if(filter=="all"||filter=="sky")SkyPainterLifetimeControl();std::printf("EnvironmentSamplerDomainTest: %d failures\n",failures);return failures?1:0;}
