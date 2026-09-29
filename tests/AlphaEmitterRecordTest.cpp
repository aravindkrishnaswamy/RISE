// Original emission records stay unchanged; alpha-only copies use physical P.
#include <iostream>
#include <cmath>
#include <vector>
#include "../src/Library/Geometry/ClippedPlaneGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Scene.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Rendering/LuminaryManager.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Materials/LambertianLuminaireMaterial.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/ExpressionPainter.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Lights/LightSampler.h"
using namespace RISE;using namespace RISE::Implementation;
class RecordPainter:public UniformColorPainter {
public:mutable std::vector<Scalar> ys;
RecordPainter():UniformColorPainter(RISEPel(1)){}
RISEPel GetColor(const RayIntersectionGeometric&r)const override{ys.push_back(r.ptIntersection.y);return RISEPel(1+r.ptIntersection.y/4);}
Scalar GetRadianceNM(const RayIntersectionGeometric&r,Scalar)const override{ys.push_back(r.ptIntersection.y);return 1+r.ptIntersection.y/4;}
};
int pass=0,fail=0;void Check(bool b,const char*s){(b?pass:fail)++;std::cout<<(b?"PASS ":"FAIL ")<<s<<'\n';}
void Run(bool expression){
ExpressionProgram prog=ExpressionProgram::Invalid();ExpressionProgram::Builder builder;builder.EnableContextVars(true);
Check(builder.Finalize("vec3(1+P.y/4,1+P.y/4,1+P.y/4)",prog),"actual world-P expression compiles");
std::vector<ParamSpec> params;auto* recorder=new RecordPainter;
IPainter* paint=expression?static_cast<IPainter*>(new ExpressionPainter(prog,params,0)):static_cast<IPainter*>(recorder);if(expression)recorder->release();
auto* white=new UniformColorPainter(RISEPel(1));auto* diffuse=new LambertianMaterial(*white);auto* emitter=new LambertianLuminaireMaterial(*paint,40,*diffuse);
const Point3 pts[4]={Point3(-.25,4,-.25),Point3(.25,4,-.25),Point3(.25,4,.25),Point3(-.25,4,.25)};auto* geometry=new ClippedPlaneGeometry(pts,false);auto* object=new Object(geometry);object->AssignMaterial(*emitter);object->FinalizeTransformations();auto* objects=new ObjectManager(false,false,4,8);objects->AddItem(object,"emitter");auto* scene=new Scene;scene->SetObjectManager(objects);
std::vector<IShaderOp*> ops;IShader* shader=nullptr;RISE_API_CreateStandardShader(&shader,ops);IRayCaster* caster=nullptr;RISE_API_CreateRayCaster(&caster,false,8,*shader,true);caster->AttachScene(scene);const auto* ls=caster->GetLightSampler();auto* lm=const_cast<LuminaryManager*>(dynamic_cast<const LuminaryManager*>(caster->GetLuminaries()));
Check(lm&&lm->getLuminaries().size()==1,"one prepared real emitter");
for(bool masked:{false,true}){
if(masked){ExpressionProgram ap=ExpressionProgram::Invalid();ExpressionProgram::Builder ab;ab.EnableContextVars(true);Check(ab.Finalize("P.y/4",ap),"physical world-P alpha expression compiles");auto* one=new ExpressionScalarPainter(ap,params);emitter->SetAlpha(one,eAlphaMask,.5);one->release();}
RandomNumberGenerator rng(214);IndependentSampler sampler(rng);LightSample sample;
if(!expression)recorder->ys.clear();
bool valid=ls->SampleLight(*scene,lm->getLuminaries(),sampler,sample);
Check(valid&&sample.Le.r>0,"production SampleLight valid");
if(!expression){Check(recorder->ys.size()==1,"single actual root emission query");std::cout<<"root.record.y="<<recorder->ys.back()<<'\n';}
RayIntersectionGeometric oldRecord(Ray(sample.position,sample.direction),nullRasterizerState);oldRecord.vNormal=oldRecord.vGeomNormal=sample.normal;oldRecord.ptCoord=sample.ptCoord;oldRecord.onb.CreateFromW(sample.normal);LightSampler::ApplyEmitterSurface(oldRecord,sample.surface);oldRecord.ptObjIntersec=sample.ptObjIntersec;
const auto oldLe=emitter->GetEmitter()->emittedRadiance(oldRecord,sample.direction,sample.normal);
std::cout<<"expression="<<expression<<" masked="<<masked<<" actualY="<<sample.position.y<<" baseRecordY="<<oldRecord.ptIntersection.y<<" actualLe="<<sample.Le.r<<" baselineRecordLe="<<oldLe.r<<" ratio="<<sample.Le.r/oldLe.r<<'\n';
Check(std::fabs(sample.Le.r-oldLe.r)<1e-10,"unchanged Le-record contract vs exact base record");
RayIntersectionGeometric ri(Ray(Point3(0,1,0),Vector3(0,-1,0)),RasterizerState{17,29});ri.ptIntersection=Point3(0,0,0);ri.vNormal=ri.vGeomNormal=Vector3(0,1,0);ri.onb.CreateFromW(ri.vNormal);
for(bool nm:{false,true}){if(!expression)recorder->ys.clear();RandomNumberGenerator nr(214);IndependentSampler ns(nr);double direct=nm?ls->EvaluateDirectLightingNM(ri,*diffuse->GetBSDF(),diffuse,550,*caster,ns,nullptr,nullptr,false,nullptr):ls->EvaluateDirectLighting(ri,*diffuse->GetBSDF(),diffuse,*caster,ns,nullptr,nullptr,false,nullptr).r;Check(direct>0,"actual NEE remains positive");if(!expression){Check(recorder->ys.size()==1,"single actual NEE emission query");std::cout<<"NEE nm="<<nm<<" record.y="<<recorder->ys.back()<<" contribution="<<direct<<'\n';Check(recorder->ys.back()==oldRecord.ptIntersection.y,"NEE preserves its original P0 record (baseline limitation)");}}
}
caster->release();shader->release();scene->release();objects->release();object->release();geometry->release();emitter->release();diffuse->release();white->release();paint->release();
}
int main(){Run(false);Run(true);std::cout<<pass<<" passed / "<<fail<<" failed\n";return fail?1:0;}
