// Independent Blender shader direction vs UV normal-map basis and world rotation.
#define main dl213_tangent_fixture_main
#include "BlenderBridgeTangentTest.cpp"
#undef main
#include "../src/Library/Materials/CoatedBRDF.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/IModifierManager.h"
#include "../src/Library/Modifiers/ModifierFrame.h"
#include "../src/Library/Utilities/MicrofacetUtils.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Objects/CSGObject.h"
namespace {
int checks=0,failures=0;
void Verify(bool ok,const char* message){++checks;if(!ok){++failures;std::cerr<<"FAIL "<<message<<'\n';}}
bool Near(double a,double b){return std::abs(a-b)<=1e-5*std::max(1.0,std::abs(b));}
bool NearV(const RISE::Vector3& a,const RISE::Vector3& b){return Near(a.x,b.x)&&Near(a.y,b.y)&&Near(a.z,b.z);}
class FixedSampler: public RISE::ISampler {
 unsigned i=0; double u,v;
public:
 FixedSampler(double x,double y):u(x),v(y) {}
 RISE::Scalar Get1D() override {const double draws[]={0,u,v};return draws[(i++)%3];}
 RISE::Point2 Get2D() override {const double x=Get1D(),y=Get1D();return RISE::Point2(x,y);}
 void StartStream(int) override {}
};
void Consumers(const RISE::IMaterial& ggx,const RISE::RayIntersectionGeometric& actual,const RISE::Vector3& tangent){
 using namespace RISE;
 auto oracle=actual; const Vector3 n=actual.vNormal;
 oracle.onb=OrthonormalBasis3D(tangent,Vector3Ops::Cross(n,tangent),n);
 const Vector3 light(.6,0,n.z*.8); IORStack stack(1.0);
 Verify(Near(ggx.GetBSDF()->value(light,actual).r,ggx.GetBSDF()->value(light,oracle).r),"GGX RGB value independent canonical oracle");
 Verify(Near(ggx.GetBSDF()->valueNM(light,actual,550),ggx.GetBSDF()->valueNM(light,oracle,550)),"GGX NM value independent canonical oracle");
 Verify(Near(ggx.GetSPF()->Pdf(actual,light,stack),ggx.GetSPF()->Pdf(oracle,light,stack)),"GGX RGB Pdf independent canonical oracle");
 Verify(Near(ggx.GetSPF()->PdfNM(actual,light,550,stack),ggx.GetSPF()->PdfNM(oracle,light,550,stack)),"GGX NM Pdf independent canonical oracle");
 for(bool spectral:{false,true}) {
  unsigned emitted=0;
  for(unsigned sample=0;sample<16;++sample) {
  FixedSampler a((sample+.5)/16,.13),b((sample+.5)/16,.13);ScatteredRayContainer ca,cb;
  if(spectral){ggx.GetSPF()->ScatterNM(actual,a,550,ca,stack);ggx.GetSPF()->ScatterNM(oracle,b,550,cb,stack);}
  else{ggx.GetSPF()->Scatter(actual,a,ca,stack);ggx.GetSPF()->Scatter(oracle,b,cb,stack);}
  Verify(ca.Count()==cb.Count(),"GGX Scatter count canonical oracle");emitted+=ca.Count();
  for(unsigned k=0;k<ca.Count()&&k<cb.Count();++k){Verify(NearV(ca[k].ray.Dir(),cb[k].ray.Dir()),"GGX Scatter direction canonical oracle");Verify(Near(ca[k].pdf,cb[k].pdf),"GGX Scatter PDF canonical oracle");Verify(Near(spectral?ca[k].krayNM:ca[k].kray.r,spectral?cb[k].krayNM:cb[k].kray.r),"GGX Scatter weight canonical oracle");}
  }
  Verify(emitted>0,"GGX Scatter nonvacuous fixed sample grid");
 }
}
void Case(const float* directions,const float* matrix,const RISE::Vector3& expectedT,double rotation,bool nested,const RISE::Vector3* rawDirection=nullptr){
 using namespace RISE;
 IJobPriv* job=0;RISE_CreateJobPriv(&job);char err[512]={0};
 float vertices[]={0,0,0,1,0,0,0,1,0},normals[]={0,0,1},uv[]={0,0,0,1,0,0,0,1,0};unsigned indices[]={0,1,2},normalIndices[]={0,0,0};
 double white[]={1,1,1},black[]={0,0,0},normal[]={.8,.5,.9};job->AddUniformColorPainter("white",white,"Rec709RGB_Linear");job->AddUniformColorPainter("black",black,"Rec709RGB_Linear");job->AddUniformColorPainter("normal",normal,"Rec709RGB_Linear");
 const std::string angle=std::to_string(rotation);
 Verify(job->AddGGXMaterial("ggx","black","white",".04",".35","1.5","0","schlick_f0",angle.c_str()),"GGX setup");
 rise_blender_material material={};material.name="coat";material.model=RISE_BLENDER_MATERIAL_PBR_METALLIC_ROUGHNESS;material.base_color_painter_name="white";material.metallic_painter_name="0";material.roughness_painter_name=".3";material.coat_weight=1;material.coat_roughness=.1;material.coat_ior=1.5;material.coat_tint_painter_name="white";material.coat_normal_painter_name="normal";material.coat_normal_scale=1;
 Verify(add_material(*job,material,err,sizeof(err)),"coat setup");
 const auto* coat=dynamic_cast<const Implementation::CoatedBRDF*>(job->GetMaterials()->GetItem("coat")->GetBSDF());Verify(coat!=0,"coat shipping consumer");
 auto* ggx=job->GetMaterials()->GetItem("ggx");
 Verify(job->AddNormalMapModifier("nm","normal",1),"normal modifier setup");
 for(unsigned mode=0;mode<2;++mode){
  rise_blender_mesh mesh={};mesh.name=mode?"shader":"control";mesh.vertices=vertices;mesh.normals=normals;mesh.uvs=uv;mesh.vertex_indices=indices;mesh.normal_indices=normalIndices;mesh.uv_indices=indices;mesh.num_vertices=3;mesh.num_normals=1;mesh.num_uvs=3;mesh.num_triangles=1;mesh.double_sided=1;
  if(mode){mesh.tangent_attribute=directions;mesh.num_tangents=3;mesh.tangent_is_shader_direction=1;}
  Verify(add_mesh(*job,mesh,err,sizeof(err)),"bridge semantic payload setup");
  rise_blender_object object={};object.name=mesh.name;object.geometry_name=mesh.name;object.material_name="coat";object.visible=1;for(unsigned k=0;k<16;++k)object.transform[k]=nested?(k%5==0?(k==0?2:1):0):matrix[k];
  Verify(add_object(*job,object,err,sizeof(err)),"object setup");
 }
 const Point3 point(matrix[0]*.25+matrix[1]*.25+matrix[3],matrix[4]*.25+matrix[5]*.25+matrix[7],matrix[11]);
 IObjectPriv* shader=job->GetObjects()->GetItem("shader");IObjectPriv* control=job->GetObjects()->GetItem("control");IObjectPriv* csgShader=0;IObjectPriv* csgControl=0;
 if(nested){
  Implementation::SphereGeometry* sphere=new Implementation::SphereGeometry(1);Implementation::Object* far=new Implementation::Object(sphere);safe_release(sphere);far->SetPosition(Point3(1000,1000,1000));far->FinalizeTransformations();
  Verify(RISE_API_CreateCSGObject(&csgShader,shader,far,0),"CSG shader setup");Verify(RISE_API_CreateCSGObject(&csgControl,control,far,0),"CSG control setup");
  csgShader->SetStretch(Vector3(.5,1,1));csgShader->FinalizeTransformations();csgControl->SetStretch(Vector3(.5,1,1));csgControl->FinalizeTransformations();
  IObjectPriv* outerShader=0;IObjectPriv* outerControl=0;
  Verify(RISE_API_CreateCSGObject(&outerShader,csgShader,far,0),"nested CSG shader setup");Verify(RISE_API_CreateCSGObject(&outerControl,csgControl,far,0),"nested CSG control setup");safe_release(far);safe_release(csgShader);safe_release(csgControl);csgShader=outerShader;csgControl=outerControl;
  csgShader->SetStretch(Vector3(matrix[0],matrix[5],matrix[10]));csgShader->FinalizeTransformations();csgControl->SetStretch(Vector3(matrix[0],matrix[5],matrix[10]));csgControl->FinalizeTransformations();shader=csgShader;control=csgControl;
 }
 for(double side:{1.0,-1.0}){
  RasterizerState rs={0,0};RayIntersection hit(Ray(Point3(point.x,point.y,point.z+side),Vector3(0,0,-side)),rs),baseline(hit);
  shader->IntersectRay(hit,RISE_INFINITY,true,true,false);control->IntersectRay(baseline,RISE_INFINITY,true,true,false);
  Verify(hit.geometric.bHit&&baseline.geometric.bHit,"front/back real bridge hit");if(!hit.geometric.bHit||!baseline.geometric.bHit)continue;
  Verify(hit.geometric.bHasShaderDirection&&hit.geometric.bHasNormalMapFrame,"separate hit payload flags");Verify(!hit.geometric.bHasTangent,"shader payload never becomes global normal-map tangent");
  Verify(NearV(hit.geometric.onb.u(),expectedT),"independent world shader tangent");
  Verify(NearV(hit.geometric.onb.v(),Vector3Ops::Cross(hit.geometric.vNormal,expectedT)),"canonical world V independent of mirror parity");
  Verify(NearV(hit.geometric.normalMapOnb.u(),baseline.geometric.onb.u())&&NearV(hit.geometric.normalMapOnb.v(),baseline.geometric.onb.v()),"UV normal frame unaffected");
  if(coat){const auto observed=coat->ResolveCoatFrame(hit.geometric,hit.geometric.onb).w(),truth=coat->ResolveCoatFrame(baseline.geometric,baseline.geometric.onb).w();Verify(NearV(observed,truth),"coat normal independent of anisotropy");if(!nested&&matrix[0]==1&&matrix[5]==1)Verify(NearV(observed,Vector3(.6,0,.8*side)),"coat analytic active UV normal oracle");}
  Consumers(*ggx,hit.geometric,expectedT);
  const auto copied=hit.geometric;RayIntersectionGeometric assigned(hit.geometric.ray,rs);assigned=hit.geometric;
  Verify(copied.bHasShaderDirection&&assigned.bHasNormalMapFrame&&NearV(assigned.normalMapOnb.u(),hit.geometric.normalMapOnb.u()),"hit copy and assignment payload");
  auto modified=hit.geometric;job->GetModifiers()->GetItem("nm")->Modify(modified);
  const auto oldModified=baseline.geometric;auto controlModified=oldModified;job->GetModifiers()->GetItem("nm")->Modify(controlModified);
  Verify(NearV(modified.vNormal,controlModified.vNormal),"base normal decode independent");
  if(coat&&!nested&&matrix[0]==1&&matrix[5]==1) {
   Verify(NearV(coat->ResolveCoatFrame(modified,modified.onb).w(),Vector3(.6,0,.8*side)),"independently authored coat normal unaffected by base normal modifier");
   Verify(NearV(coat->ResolveCoatFrame(controlModified,controlModified.onb).w(),Vector3(.96,0,.28*side)),"legacy noattribute layering explicitly preserved");
  }
  const Vector3 raw=rawDirection?*rawDirection:expectedT; const Vector3 projected=Vector3Ops::Normalize(raw-modified.vNormal*Vector3Ops::Dot(raw,modified.vNormal));Verify(NearV(modified.onb.u(),projected),"normal perturbation preserves shader direction");
  Consumers(*ggx,modified,projected);
  auto twice=modified;job->GetModifiers()->GetItem("nm")->Modify(twice);Verify(NearV(twice.vNormal,modified.vNormal),"separately evaluated normal map uses original UV normal after earlier modifier");
  hit.geometric.ray=Ray(Point3(999,999,999),Vector3(0,0,1));shader->IntersectRay(hit,RISE_INFINITY,true,true,false);Verify(!hit.geometric.bHasShaderDirection&&!hit.geometric.bHasNormalMapFrame,"failed hit clears direction frame flags");
 }
 safe_release(csgShader);safe_release(csgControl);safe_release(job);
}
// Closed analytic fixture supplies the new direction independently of sphere UVs;
// actual Object/CSG intersection and exit-face probing own all transport/adoption.
class ShaderSphere : public RISE::Implementation::SphereGeometry {
public:
 ShaderSphere(double radius):SphereGeometry(radius) {}
 void IntersectRay(RISE::RayIntersectionGeometric& ri,bool front,bool back,bool exit) const override {
  SphereGeometry::IntersectRay(ri,front,back,exit);
  if(ri.bHit){ri.vShaderDirection=RISE::Vector3(0,1,1);ri.bHasShaderDirection=true;ri.bHasNormalMapFrame=false;}
 }
};
void ComplementAndFrameTransport(){
 using namespace RISE;using namespace RISE::Implementation;
 SphereGeometry* aGeometry=new SphereGeometry(2);ShaderSphere* bGeometry=new ShaderSphere(.5);
 Object* a=new Object(aGeometry);Object* b=new Object(bGeometry);safe_release(aGeometry);safe_release(bGeometry);a->FinalizeTransformations();b->FinalizeTransformations();
 CSGObject* difference=new CSGObject(CSG_SUBTRACTION);Verify(difference->AssignObjects(a,b),"difference operands assigned");safe_release(a);safe_release(b);
 difference->SetStretch(Vector3(-2,1,3));difference->FinalizeTransformations();RasterizerState rs={0,0};
 for(double side:{-1.0,1.0}){
  RayIntersection hit(Ray(Point3(0,0,0),Vector3(side,0,0)),rs);difference->IntersectRay(hit,RISE_INFINITY,true,true,true);
  Verify(hit.geometric.bHit&&hit.geometric.bHasShaderDirection&&hit.geometric.bHasNormalMapFrame,"CSG cavity adopts new payload");
  Verify(NearV(hit.geometric.vNormal,Vector3(-side,0,0)),"difference cavity complement normal");
  Verify(NearV(hit.geometric.normalMapOnb.w(),hit.geometric.vNormal),"complement UV normal independently promoted");
  Verify(NearV(hit.geometric.vShaderDirection,Vector3Ops::Normalize(Vector3(0,1,3))),"difference raw direction transport");
  Verify(NearV(hit.geometric.onb.v(),Vector3Ops::Cross(hit.geometric.vNormal,hit.geometric.onb.u())),"complement canonical world handedness");
 }
 safe_release(difference);
 RayIntersectionGeometric synthetic(Ray(Point3(0,0,1),Vector3(0,0,-1)),rs);
 synthetic.vNormal=Vector3(0,0,1);synthetic.onb=OrthonormalBasis3D(Vector3(1,0,0),Vector3(0,1,0),Vector3(0,0,1));synthetic.vShaderDirection=Vector3(1,1,1);synthetic.bHasShaderDirection=true;
 const Matrix4 identity=Matrix4Ops::Identity();ModifierFrame::PromoteShaderDirection(synthetic,identity,identity,1);
 const auto originalUV=synthetic.normalMapOnb;ModifierFrame::RebuildPreservingTangent(synthetic,Vector3(.6,0,.8));
 Verify(NearV(synthetic.normalMapOnb.w(),originalUV.w()),"normal modifier freezes original UV normal");
 Matrix4 stretch=identity;stretch._00=-2;stretch._11=1;stretch._22=3;Matrix4 inverse=identity;inverse._00=-.5;inverse._22=1.0/3;
 synthetic.vNormal=Vector3Ops::Normalize(Vector3(-.3,0,.8/3));ModifierFrame::PromoteShaderDirection(synthetic,stretch,inverse,-1);
 Verify(NearV(synthetic.normalMapOnb.w(),Vector3(0,0,1))&&NearV(synthetic.normalMapOnb.u(),Vector3(-1,0,0))&&NearV(synthetic.normalMapOnb.v(),Vector3(0,1,0)),"nested transport promotes UV own N/U/chart handedness independently");
 synthetic.vGeomNormal=Vector3(0,0,-1);synthetic.vNormal=Vector3(0,1,0);ModifierFrame::PromoteShaderDirection(synthetic,identity,identity,1);
 Verify(NearV(synthetic.normalMapOnb.w(),Vector3(0,0,-1)),"complement orientation follows geometric normal even when perturbed normal is perpendicular");
}
void Validation(){
 using namespace RISE;
 IJobPriv* job=0;RISE_CreateJobPriv(&job);char error[256]={0};
 float vertices[]={0,0,0,1,0,0,0,1,0},normals[]={0,0,1},uv[]={0,0,0,1,0,0,0,1,0},directions[]={0,1,0,1,0,1,0,1,0,1,0,1};unsigned indices[]={0,1,2},ni[]={0,0,0};
 rise_blender_mesh mesh={};mesh.name="validation";mesh.vertices=vertices;mesh.normals=normals;mesh.uvs=uv;mesh.vertex_indices=indices;mesh.normal_indices=ni;mesh.uv_indices=indices;mesh.num_vertices=3;mesh.num_normals=1;mesh.num_uvs=3;mesh.num_triangles=1;mesh.tangent_attribute=directions;mesh.num_tangents=3;mesh.tangent_is_shader_direction=2;
 Verify(!add_mesh(*job,mesh,error,sizeof(error)),"invalid semantic rejected");mesh.tangent_is_shader_direction=1;
 mesh.num_tangents=2;Verify(!add_mesh(*job,mesh,error,sizeof(error)),"corner count rejected");mesh.num_tangents=3;
 indices[2]=3;Verify(!add_mesh(*job,mesh,error,sizeof(error)),"position index rejected");indices[2]=2;
 ni[1]=1;Verify(!add_mesh(*job,mesh,error,sizeof(error)),"normal index rejected");ni[1]=0;
 mesh.num_uvs=2;Verify(!add_mesh(*job,mesh,error,sizeof(error)),"UV index rejected");mesh.num_uvs=3;
 directions[1]=0;Verify(!add_mesh(*job,mesh,error,sizeof(error)),"zero shader direction rejected");directions[1]=1;
 directions[3]=0;Verify(!add_mesh(*job,mesh,error,sizeof(error)),"invalid source sign rejected");directions[3]=-1;
 Verify(add_mesh(*job,mesh,error,sizeof(error)),"Mikk metadata sign does not invalidate independent direction");safe_release(job);
}

}
int main(){
 Validation();ComplementAndFrameTransport();
 float identity[]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},mirror[]={-1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
 float y[]={0,1,0,1,0,1,0,1,0,1,0,1};Case(y,identity,RISE::Vector3(0,1,0),.7853981633974483,false);
 const float q=std::sqrt(.5f);float diagonal[]={-q,q,0,1,-q,q,0,1,-q,q,0,1};Case(diagonal,mirror,RISE::Vector3(std::sqrt(.5),std::sqrt(.5),0),.7853981633974483,false);Case(diagonal,mirror,RISE::Vector3(std::sqrt(.5),std::sqrt(.5),0),.7853981633974483,true);
 float rawPayload[]={1,1,1,1,1,1,1,1,1,1,1,1};const RISE::Vector3 raw(1,1,1);Case(rawPayload,identity,RISE::Vector3(std::sqrt(.5),std::sqrt(.5),0),.7853981633974483,false,&raw);
 float parallelPayload[]={0,0,1,1,0,0,1,1,0,0,1,1};const RISE::Vector3 parallel(0,0,1);Case(parallelPayload,identity,RISE::Vector3(1,0,0),.7853981633974483,false,&parallel);
 float varying[]={1,0,1,-1,0,2,0,-1,2,2,0,-1};const RISE::Vector3 interpolated(1,1,.5);Case(varying,identity,RISE::Vector3(std::sqrt(.5),std::sqrt(.5),0),.7853981633974483,false,&interpolated);
 if(const char* path=std::getenv("RISE_TANGENT_SHADER_CASES")){
  std::ifstream in(path);unsigned count=0;in>>count;Verify(bool(in)&&count>0,"actual exporter cases provided");
  for(unsigned k=0;k<count;++k){float matrix[16],payload[12];double t[3],raw[3],rotation;for(float& v:matrix)in>>v;for(float& v:payload)in>>v;for(double& v:t)in>>v;in>>rotation;for(double& v:raw)in>>v;Verify(bool(in),"actual exporter case parse");if(!in)break;const RISE::Vector3 rawVector(raw[0],raw[1],raw[2]);Case(payload,matrix,RISE::Vector3(t[0],t[1],t[2]),rotation,false,&rawVector);}
 }
 std::cout<<"BlenderShaderDirectionTest "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
