// DL-214: final SMS alpha uses the geometry hit that produced its endpoint.
#include <iostream>
#include <vector>
#include <cmath>
#include "../src/Library/Geometry/EllipsoidGeometry.h"
#include "../src/Library/Geometry/BoxGeometry.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Geometry/SDFGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Scene.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Materials/PerfectReflectorMaterial.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Utilities/ManifoldSolver.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/RISE_API.h"
using namespace RISE; using namespace RISE::Implementation;
static int passed=0,failed=0;
static void Check(bool b,const char* label){(b?passed:failed)++;std::cout<<(b?"PASS ":"FAIL ")<<label<<std::endl;}
class DrawSampler:public IndependentSampler {
public: unsigned draws=0; explicit DrawSampler(const RandomNumberGenerator& r):IndependentSampler(r){}
 Scalar GetAlpha1D()override{++draws;return .25;}
};
class CountingEllipsoid:public EllipsoidGeometry {
public: mutable unsigned smoothCalls=0,physicalCalls=0;
 CountingEllipsoid():EllipsoidGeometry(Vector3(1,1,1)){}
 bool ComputeAnalyticalDerivatives(const Point2& uv,Scalar sm,Point3& p,Vector3& n,Vector3& u,Vector3& v,Vector3& nu,Vector3& nv)const override{
  if(sm>0)++smoothCalls;else ++physicalCalls;
  return EllipsoidGeometry::ComputeAnalyticalDerivatives(uv,sm,p,n,u,v,nu,nv);
 }
};
class ShiftUV:public IRayIntersectionModifier,public Reference {
public:void Modify(RayIntersectionGeometric& r)const override{r.ptCoord.x+=10;r.derivatives.valid=false;}
};
class PreModifierAlpha:public UniformScalarPainter {
public:PreModifierAlpha():UniformScalarPainter(.5){}
 ScalarTriple GetValuesAt(const RayIntersectionGeometric& r)const override{return ScalarTriple(r.ptCoord.x<2?.5:0);}
};
static void Run(bool sdf,double scale,bool stretch=false,bool twoStage=false,bool modifier=false){
 std::cout<<"CASE sdf="<<sdf<<" scale="<<scale<<std::endl;
 auto* white=new UniformColorPainter(RISEPel(1)); auto* mirror=new PerfectReflectorMaterial(*white);
 std::vector<SDFGeometry::Part> parts;
 Check(SDFGeometry::ParsePartLines("sphere union 0  0 0 0  0 0 0  1 1 1  1 0 0  0","review-sms",parts),"valid source geometry");
 IGeometry* geo=twoStage?static_cast<IGeometry*>(new CountingEllipsoid):sdf?static_cast<IGeometry*>(new SDFGeometry(parts,256,.002)):static_cast<IGeometry*>(new SphereGeometry(1));
 auto* obj=new Object(geo);obj->AssignMaterial(*mirror);obj->SetScale(scale);if(stretch)obj->SetStretch(Vector3(2,1,.5));obj->SetPosition(Point3(0,2*scale,0));obj->FinalizeTransformations();if(modifier){auto* m=new ShiftUV;obj->AssignModifier(*m);m->release();}
 auto* remoteGeo=new SphereGeometry(1);auto* remoteMat=new LambertianMaterial(*white);auto* remote=new Object(remoteGeo);remote->AssignMaterial(*remoteMat);remote->SetPosition(Point3(100,100,100));remote->FinalizeTransformations();auto* one=new UniformScalarPainter(1);auto* zero=new UniformScalarPainter(0);
 auto* objects=new ObjectManager(false,false,4,8);objects->AddItem(obj,"mirror");objects->AddItem(remote,"remote");auto* scene=new Scene;scene->SetObjectManager(objects);
 std::vector<IShaderOp*> ops;IShader* shader=nullptr;RISE_API_CreateStandardShader(&shader,ops);IRayCaster* caster=nullptr;RISE_API_CreateRayCaster(&caster,false,8,*shader,true);caster->AttachScene(scene);
 ManifoldSolverConfig cfg;cfg.twoStage=twoStage;cfg.targetBounces=1;cfg.maxChainDepth=1;auto* solver=new ManifoldSolver(cfg);RandomNumberGenerator rng(214);IndependentSampler sampler(rng);
 const Point3 start(-.5*scale,0,0),light(.5*scale,0,0);
 const auto solve=[&](){std::vector<ManifoldVertex> chain;const unsigned n=solver->BuildSeedChain(start,Point3(0,scale,0),*scene,*caster,chain,false,nullptr,&sampler);Check(n==1,"production seed trace reaches one mirror");return solver->Solve(start,Vector3(0,1,0),light,Vector3(0,1,0),chain,sampler);};
 auto original=solve();Check(original.valid&&original.specularChain.size()==1,"production Newton solve succeeds");
 if(!original.valid||original.specularChain.size()!=1)return;
 const auto& v=original.specularChain[0];
 std::cout<<"position="<<v.position.x<<","<<v.position.y<<","<<v.position.z<<std::endl;
 for(double eps:{.0001,.05*scale}){RayIntersection ri(Ray(Point3Ops::mkPoint3(v.position,v.geomNormal*eps),-v.geomNormal),nullRasterizerState);obj->IntersectRay(ri,eps*2,true,true,false);std::cout<<"probe eps="<<eps<<" hit="<<ri.geometric.bHit<<" sameMaterial="<<(ri.pMaterial==mirror)<<" distance="<<Point3Ops::Distance(ri.geometric.ptIntersection,v.position)<<std::endl;if(eps!=.0001)Check(ri.geometric.bHit&&ri.pMaterial==mirror&&Point3Ops::Distance(ri.geometric.ptIntersection,v.position)<1e-5*scale,"independent endpoint identity matches actual mirror");}
 Check(!v.alphaEndpoint,"fully opaque solve retains no alpha payload");
 Check(!caster->GetLightSampler()->SceneHasAlphaCoverage(),"opaque capability control");Check(solver->CheckChainVisibility(start,light,original.specularChain,*caster,&sampler),"opaque solved path visible");
 remoteMat->SetAlpha(one,eAlphaMask,.5);scene->BumpLightTopologyGeneration();caster->AttachScene(scene);
 auto remoteResult=solve();Check(remoteResult.valid&&remoteResult.specularChain.size()==1,"remote MASK1 keeps actual Newton solve valid");
 Check(caster->GetLightSampler()->SceneHasAlphaCoverage(),"remote MASK1 capability active");Check(solver->CheckChainVisibility(start,light,remoteResult.specularChain,*caster,&sampler),"remote MASK1 preserves physical path visibility");
 remoteMat->SetAlpha(nullptr,eAlphaOpaque,.5);mirror->SetAlpha(one,eAlphaMask,.5);scene->BumpLightTopologyGeneration();caster->AttachScene(scene);
 auto maskResult=solve();Check(maskResult.valid&&maskResult.specularChain.size()==1,"mirror MASK1 keeps actual Newton solve valid");Check(solver->CheckChainVisibility(start,light,maskResult.specularChain,*caster,&sampler),"mirror MASK1 preserves physical path visibility");
 Check(maskResult.specularChain[0].HasAlphaEndpoint(),"solved alpha endpoint owns valid position-producing record");
 auto copied=maskResult.specularChain;
 Check(copied[0].alphaEndpoint==maskResult.specularChain[0].alphaEndpoint,"Newton chain copy shares immutable endpoint record");
 copied[0].position.x=std::nextafter(copied[0].position.x,RISE_INFINITY);
 Check(!copied[0].HasAlphaEndpoint()&&!solver->CheckChainVisibility(start,light,copied,*caster,&sampler),"stale position record refuses without a distance tolerance");
 copied=maskResult.specularChain;copied[0].pObject=remote;
 Check(!copied[0].HasAlphaEndpoint()&&!solver->CheckChainVisibility(start,light,copied,*caster,&sampler),"wrong-object record refuses");
 copied=maskResult.specularChain;copied[0].alphaEndpoint.reset();
 Check(!solver->CheckChainVisibility(start,light,copied,*caster,&sampler),"uncertified hand-built endpoint refuses");
 copied=maskResult.specularChain;
 Check(solver->CheckChainVisibility(start,light,copied,*caster,&sampler),"rollback copy restores physical endpoint");
 if(twoStage){auto* e=dynamic_cast<CountingEllipsoid*>(geo);Check(e->smoothCalls>0&&e->physicalCalls>0,"two-stage actually traverses analytic Stage1 and geometric transition");}
 if(modifier)Check(copied[0].alphaEndpoint->geometric.ptCoord.x<2&&copied[0].uv.x>=10,"payload retains pre-modifier UV while solver sees modified UV");
 auto* half=modifier?static_cast<UniformScalarPainter*>(new PreModifierAlpha):new UniformScalarPainter(.5);mirror->SetAlpha(half,eAlphaBlend,.5);half->release();DrawSampler draws(rng);
 const bool blendVisible=solver->CheckChainVisibility(start,light,copied,*caster,&draws);std::cout<<"BLEND visible="<<blendVisible<<" draws="<<draws.draws<<std::endl;Check(blendVisible&&draws.draws==1,"BLEND final endpoint makes exactly one coverage draw");
 mirror->SetAlpha(zero,eAlphaMask,.5);scene->BumpLightTopologyGeneration();caster->AttachScene(scene);
 Check(!solver->CheckChainVisibility(start,light,maskResult.specularChain,*caster,&sampler),"mirror MASK0 rejects final physical endpoint");std::vector<ManifoldVertex> zeroChain;Check(solver->BuildSeedChain(start,Point3(0,scale,0),*scene,*caster,zeroChain,false,nullptr,&sampler)==0,"mirror MASK0 rejects actual seed");
 solver->release();caster->release();shader->release();scene->release();objects->release();remote->release();remoteGeo->release();remoteMat->release();one->release();zero->release();obj->release();geo->release();mirror->release();white->release();
}

static void CloseFaces(){
 auto* white=new UniformColorPainter(RISEPel(1));auto* mainMat=new PerfectReflectorMaterial(*white);auto* otherMat=new PerfectReflectorMaterial(*white);auto* one=new UniformScalarPainter(1);auto* zero=new UniformScalarPainter(0);mainMat->SetAlpha(one,eAlphaMask,.5);otherMat->SetAlpha(zero,eAlphaMask,.5);
 auto* mainGeo=new BoxGeometry(2,.2,2);auto* otherGeo=new BoxGeometry(.01,.02,2);
 auto* mainObj=new Object(mainGeo);mainObj->AssignMaterial(*mainMat);mainObj->SetPosition(Point3(0,1.1,0));mainObj->FinalizeTransformations();
 auto* otherObj=new Object(otherGeo);otherObj->AssignMaterial(*otherMat);otherObj->SetPosition(Point3(0,.97,0));otherObj->FinalizeTransformations();
 auto* csg=new CSGObject(CSG_UNION);csg->AssignObjects(mainObj,otherObj);csg->FinalizeTransformations();auto* objects=new ObjectManager(false,false,4,8);objects->AddItem(csg,"close-faces");auto* scene=new Scene;scene->SetObjectManager(objects);
 std::vector<IShaderOp*> ops;IShader* shader=nullptr;RISE_API_CreateStandardShader(&shader,ops);IRayCaster* caster=nullptr;RISE_API_CreateRayCaster(&caster,false,8,*shader,true);caster->AttachScene(scene);
 ManifoldSolverConfig cfg;cfg.targetBounces=1;cfg.maxChainDepth=1;auto* solver=new ManifoldSolver(cfg);RandomNumberGenerator rng(214);IndependentSampler sampler(rng);const Point3 start(-.5,0,0),light(.5,0,0);std::vector<ManifoldVertex> seed;
 Check(solver->BuildSeedChain(start,Point3(0,1,0),*scene,*caster,seed,false,nullptr,&sampler)==1,"close faces production seed reaches intended mirror");
 auto result=solver->Solve(start,Vector3(0,1,0),light,Vector3(0,1,0),seed,sampler);
 Check(result.valid&&result.specularChain.size()==1,"close faces production Newton solve succeeds");
 if(result.valid&&result.specularChain.size()==1){const auto& v=result.specularChain[0];
 RayIntersection probe(Ray(Point3Ops::mkPoint3(v.position,v.normal*.05),-v.normal),nullRasterizerState);csg->IntersectRay(probe,.1,true,true,false);
 Check(probe.geometric.bHit&&probe.pMaterial==otherMat,"existing FD verification encounters distinct nearby MASK0 face");
 Check(v.HasAlphaEndpoint()&&v.alphaEndpoint->pMaterial==mainMat,"FD verification cannot replace actual endpoint material");
 Check(solver->CheckChainVisibility(start,light,result.specularChain,*caster,&sampler),"nearby MASK0 does not erase actual MASK1 endpoint");
 mainMat->SetAlpha(zero,eAlphaMask,.5);otherMat->SetAlpha(one,eAlphaMask,.5);
 Check(!solver->CheckChainVisibility(start,light,result.specularChain,*caster,&sampler),"nearby MASK1 does not replace actual MASK0 endpoint");}
 solver->release();caster->release();shader->release();scene->release();objects->release();csg->release();mainObj->release();otherObj->release();mainGeo->release();otherGeo->release();mainMat->release();otherMat->release();one->release();zero->release();white->release();
}
int main(){for(bool sdf:{false,true})for(double scale:{.1,1.,10.})Run(sdf,scale);Run(true,1,true);Run(false,1,true,true);Run(false,1,false,true,true);CloseFaces();std::cout<<passed<<" passed / "<<failed<<" failed"<<std::endl;return failed?1:0;}
