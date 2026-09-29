// Manually built light/SMS coverage contexts must carry physical endpoint data.
#include "AlphaTransportFixture.h"
#include "../src/Library/Geometry/ClippedPlaneGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Scene.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Materials/PerfectReflectorMaterial.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/ExpressionPainter.h"
#include "../src/Library/Utilities/ManifoldSolver.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Geometry/BoxGeometry.h"
#include "../src/Library/Objects/CSGObject.h"
static void Replace(std::string& s,const std::string& a,const std::string& b){const auto at=s.find(a);if(at==std::string::npos)std::exit(2);s.replace(at,a.size(),b);}
static void SMSContext(){
 auto* white=new UniformColorPainter(RISEPel(1));auto* mirror=new PerfectReflectorMaterial(*white);
 const Point3 pts[4]={Point3(-2,1,-2),Point3(2,1,-2),Point3(2,1,2),Point3(-2,1,2)};
 auto* geometry=new ClippedPlaneGeometry(pts,true);auto* object=new Object(geometry);object->AssignMaterial(*mirror);object->FinalizeTransformations();
 auto* enclosingGeo=new SphereGeometry(5);auto* enclosing=new Object(enclosingGeo);enclosing->FinalizeTransformations();
 auto* objects=new ObjectManager(false,false,4,8);objects->AddItem(object,"mirror");objects->AddItem(enclosing,"enclosing");auto* scene=new Scene;scene->SetObjectManager(objects);
 ExpressionProgram prog=ExpressionProgram::Invalid();ExpressionProgram::Builder builder;builder.EnableContextVars(true);Check(builder.Finalize("1-interior(2)",prog),"SMS scalar expression compiles");std::vector<ParamSpec> params;auto* coverage=new ExpressionScalarPainter(prog,params);
 std::vector<IShaderOp*> ops;IShader* shader=nullptr;RISE_API_CreateStandardShader(&shader,ops);IRayCaster* caster=nullptr;RISE_API_CreateRayCaster(&caster,false,8,*shader,true);caster->AttachScene(scene);
 RayIntersection control(Ray(Point3(.5,0,0),Vector3(0,1,0)),nullRasterizerState);objects->IntersectRay(control,true,true,false);
 Check(control.geometric.bHit&&control.pObject==object&&coverage->GetValuesAt(control.geometric).v[0]==0,"SMS real manager-hit context gives zero coverage");
 ManifoldVertex vertex;vertex.position=Point3(.5,1,0);vertex.normal=Vector3(0,-1,0);vertex.geomNormal=vertex.normal;vertex.pObject=object;vertex.pMaterial=mirror;
 std::vector<ManifoldVertex> chain{vertex};auto* solver=new ManifoldSolver(ManifoldSolverConfig());RandomNumberGenerator random(214);IndependentSampler sampler(random);
 const bool opaque=solver->CheckChainVisibility(Point3(0,0,0),Point3(1,0,0),chain,*caster,&sampler);
 mirror->SetAlpha(coverage,eAlphaMask,.5);
 const bool alpha=solver->CheckChainVisibility(Point3(0,0,0),Point3(1,0,0),chain,*caster,&sampler);
 Check(opaque,"SMS opaque solved-chain visibility control");Check(!alpha,"SMS final physical coverage sees scene context");
 solver->release();caster->release();shader->release();scene->release();objects->release();enclosing->release();enclosingGeo->release();object->release();geometry->release();mirror->release();coverage->release();white->release();
}
class CountingSampler:public IndependentSampler {
public:
 unsigned ordinary=0,alpha=0;
 explicit CountingSampler(const RandomNumberGenerator& r):IndependentSampler(r){}
 Scalar Get1D()override{++ordinary;return IndependentSampler::Get1D();}
 Point2 Get2D()override{ordinary+=2;return IndependentSampler::Get2D();}
 Scalar GetAlpha1D()override{++alpha;return .5;}
};
class EndpointCoverage:public IScalarPainter,public Reference {
public:
 const IObjectManager* scene;const IObject* object;Scalar sampledY;bool rasterExpected=false;
 EndpointCoverage(const IObjectManager* s,const IObject* o,Scalar y):scene(s),object(o),sampledY(y){}
 ScalarTriple GetValuesAt(const RayIntersectionGeometric& r)const override {
  return ScalarTriple(r.signals.pScene==scene && r.signals.pSelf==object &&
   std::fabs(r.ptIntersection.y-sampledY)<1e-6 && std::fabs(r.signals.ptWorld.y-sampledY)<1e-6 &&
   (!rasterExpected || (r.rast.x==17 && r.rast.y==29)) ? 1:0);
 }
};
static void DirectContexts(){
 auto* white=new UniformColorPainter(RISEPel(1));auto* diffuse=new LambertianMaterial(*white);auto* emitter=new LambertianLuminaireMaterial(*white,40,*diffuse);
 const Point3 pts[4]={Point3(-.25,4,-.25),Point3(.25,4,-.25),Point3(.25,4,.25),Point3(-.25,4,.25)};auto* geometry=new ClippedPlaneGeometry(pts,false);auto* object=new Object(geometry);object->AssignMaterial(*emitter);object->FinalizeTransformations();
 auto* objects=new ObjectManager(false,false,4,8);objects->AddItem(object,"emitter");auto* scene=new Scene;scene->SetObjectManager(objects);
 // Single-sided clipped planes publish a normal-offset sample. Derive the
 // oracle from that actual proposal rather than guessing an epsilon slab.
 Point3 samplePoint;Vector3 sampleNormal;Point2 sampleUV;
 object->UniformRandomPoint(&samplePoint,&sampleNormal,&sampleUV,Point3(.5,.5,.5));
 auto* coverage=new EndpointCoverage(objects,object,samplePoint.y);
 std::vector<IShaderOp*> ops;IShader* shader=nullptr;RISE_API_CreateStandardShader(&shader,ops);IRayCaster* caster=nullptr;RISE_API_CreateRayCaster(&caster,false,8,*shader,true);caster->AttachScene(scene);
 const auto* ls=caster->GetLightSampler();auto* lm=const_cast<LuminaryManager*>(dynamic_cast<const LuminaryManager*>(caster->GetLuminaries()));Check(lm&&lm->getLuminaries().size()==1,"one actual mesh emitter prepared");
 RandomNumberGenerator ar(214),br(214);CountingSampler a(ar),b(br);LightSample base,alpha;
 const bool baseValid=ls->SampleLight(*scene,lm->getLuminaries(),a,base);emitter->SetAlpha(coverage,eAlphaMask,.5);const bool alphaValid=ls->SampleLight(*scene,lm->getLuminaries(),b,alpha);
 std::cout<<"emission baseValid="<<baseValid<<" alphaValid="<<alphaValid<<" baseLe="<<base.Le.r<<" alphaLe="<<alpha.Le.r<<std::endl;
 Check(baseValid&&base.Le.r>0,"opaque SampleLight control");Check(alphaValid&&alpha.Le.r==base.Le.r,"emission alpha gets known scene self and world context without signal demand");
 Check(a.ordinary==b.ordinary&&a.alpha==0&&b.alpha==0,"MASK1 emission context leaves proposal draws unchanged");
 coverage->rasterExpected=true;RayIntersectionGeometric ri(Ray(Point3(0,1,0),Vector3(0,-1,0)),RasterizerState{17,29});ri.ptIntersection=Point3(0,0,0);ri.vNormal=ri.vGeomNormal=Vector3(0,1,0);ri.onb.CreateFromW(ri.vNormal);
 for(bool nm:{false,true}){
  RandomNumberGenerator cr(214),dr(214);CountingSampler c(cr),d(dr);emitter->SetAlpha(nullptr,eAlphaOpaque,.5);
  const double original=nm?ls->EvaluateDirectLightingNM(ri,*diffuse->GetBSDF(),diffuse,550,*caster,c,nullptr,nullptr,false,nullptr):ls->EvaluateDirectLighting(ri,*diffuse->GetBSDF(),diffuse,*caster,c,nullptr,nullptr,false,nullptr).r;
  emitter->SetAlpha(coverage,eAlphaMask,.5);
  const double covered=nm?ls->EvaluateDirectLightingNM(ri,*diffuse->GetBSDF(),diffuse,550,*caster,d,nullptr,nullptr,false,nullptr):ls->EvaluateDirectLighting(ri,*diffuse->GetBSDF(),diffuse,*caster,d,nullptr,nullptr,false,nullptr).r;
  std::cout<<"direct nm="<<nm<<" original="<<original<<" covered="<<covered<<std::endl;
  Check(original>0,"opaque NEE entry control");Check(covered==original,"NEE alpha receives physical scene self world and receiver raster");Check(c.ordinary==d.ordinary&&c.alpha==0&&d.alpha==0,"MASK1 NEE context leaves proposal draws unchanged");
 }
 caster->release();shader->release();scene->release();objects->release();object->release();geometry->release();emitter->release();diffuse->release();coverage->release();white->release();
}
static void SMSMaterialCrossing(){
 auto* white=new UniformColorPainter(RISEPel(1));auto* leftMat=new PerfectReflectorMaterial(*white);auto* rightMat=new PerfectReflectorMaterial(*white);auto* zero=new UniformScalarPainter(0);rightMat->SetAlpha(zero,eAlphaMask,.5);
 auto* box=new BoxGeometry(2,.2,4);auto* left=new Object(box);left->AssignMaterial(*leftMat);left->SetPosition(Point3(-1,1.1,0));left->FinalizeTransformations();auto* right=new Object(box);right->AssignMaterial(*rightMat);right->SetPosition(Point3(1,1.1,0));right->FinalizeTransformations();auto* csg=new CSGObject(CSG_UNION);csg->AssignObjects(left,right);csg->FinalizeTransformations();
 auto* objects=new ObjectManager(false,false,4,8);objects->AddItem(csg,"composite");auto* scene=new Scene;scene->SetObjectManager(objects);std::vector<IShaderOp*> ops;IShader* shader=nullptr;RISE_API_CreateStandardShader(&shader,ops);IRayCaster* caster=nullptr;RISE_API_CreateRayCaster(&caster,false,8,*shader,true);caster->AttachScene(scene);
 ManifoldSolverConfig config;config.targetBounces=1;config.maxChainDepth=1;auto* solver=new ManifoldSolver(config);RandomNumberGenerator random(214);IndependentSampler sampler(random);std::vector<ManifoldVertex> chain;
 const Point3 start(.5,0,0),light(1.5,0,0);const auto seeds=solver->BuildSeedChain(start,Point3(-1,1,0),*scene,*caster,chain,false,nullptr,&sampler);
 Check(seeds==1&&chain[0].pMaterial==leftMat,"production seed trace selects opaque left CSG material");
 const auto result=solver->Solve(start,Vector3(0,1,0),light,Vector3(0,1,0),chain,sampler);
 Check(result.valid&&result.specularChain.size()==1,"unmodified Newton converges to physical mirror chain");
 if(result.valid){const auto& v=result.specularChain[0];RayIntersection hit(Ray(Point3Ops::mkPoint3(v.position,v.geomNormal*1e-4),-v.geomNormal),nullRasterizerState);csg->IntersectRay(hit,.001,true,true,false);
 std::cout<<"SMS seed-to-solve x="<<v.position.x<<" storedLeft="<<(v.pMaterial==leftMat)<<" actualRight="<<(hit.pMaterial==rightMat)<<std::endl;
 Check(hit.geometric.bHit&&hit.pMaterial==rightMat,"solved endpoint independently reaches right MASK0 material");
 Check(!solver->CheckChainVisibility(start,light,result.specularChain,*caster,&sampler),"final coverage belongs to actual solved material");}
 solver->release();caster->release();shader->release();scene->release();objects->release();csg->release();left->release();right->release();box->release();leftMat->release();rightMat->release();zero->release();white->release();
}
int main(){
 SMSContext();DirectContexts();SMSMaterialCrossing();
 for(unsigned w=0;w<3;++w){
  auto r=RastPT(1024);if(w){Replace(r,"pathtracing_pel_rasterizer","pathtracing_spectral_rasterizer");Replace(r,"{\n","{\n hwss "+std::string(w==2?"true":"false")+"\n");}
  auto s=ReceiverScene(kArea,false,0,kTight);Replace(s,"color 0.01 0.01 0.01","color 0 0 0");
  const double base=Render(Assemble(r,s),"endpoint opaque control");
  const std::string expression="scalar_painter\n{\n name endpoint_alpha\n expression P.y/4\n}\n";
  Replace(s,"lambertian_luminaire_material\n{\n\tname mat_emit",expression+"lambertian_luminaire_material\n{\n alpha_mode mask\n alpha_coverage endpoint_alpha\n\tname mat_emit");
  const double alpha=Render(Assemble(r,s),"world position endpoint alpha");
  std::cout<<"NEE w="<<w<<" opaque="<<base<<" alpha="<<alpha<<" ratio="<<alpha/base<<std::endl;
  Check(std::isfinite(base)&&base>.01,"valid opaque area-light control");Check(alpha>=0&&std::fabs(alpha/base-1)<.025,"area-light alpha uses actual y4 endpoint rather than origin");
 }
 std::cout<<passCount<<" passed / "<<failCount<<" failed"<<std::endl;return failCount?1:0;
}
