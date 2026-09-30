// DL213 missing Blender mesh tangent transport: actual image-space highlight axis.
#include <cmath>
#include <cstring>
#include <fstream>
#include <cstdio>
#include <unistd.h>
#include "../src/Library/Interfaces/IGeometryManager.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IObjectPriv.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include <iostream>
#include <vector>
#include "../src/Blender/native/rise_blender_bridge.cpp"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/ITriangleMeshGeometry.h"
#include "../src/Library/Utilities/Reference.h"
class CapturingRasterizerOutput
	: public virtual RISE::IRasterizerOutput
	, public virtual RISE::Implementation::Reference
{
public:
	std::vector<RISE::RISEColor> pixels;
	unsigned int width;
	unsigned int height;

	CapturingRasterizerOutput() : width(0), height(0) {}

protected:
	virtual ~CapturingRasterizerOutput() {}

public:
	virtual void OutputIntermediateImage( const RISE::IRasterImage&, const RISE::Rect* ) {}

	virtual void OutputImage( const RISE::IRasterImage& image, const RISE::Rect*, const unsigned int )
	{
		width = image.GetWidth();
		height = image.GetHeight();
		pixels.resize( width * height );
		for( unsigned int y = 0; y < height; y++ ) {
			for( unsigned int x = 0; x < width; x++ ) {
				pixels[y * width + x] = image.GetPEL( x, y );
			}
		}
	}
};


bool Render(int producer, std::vector<RISE::RISEColor>& pixels) {
 RISE::IJobPriv* j=0; RISE::RISE_CreateJobPriv(&j); if(!j) return false;
 char err[512]={0};
 float verts[]={-3,-3,0, 3,-3,0, 3,3,0, -3,3,0};
 float uv[]={0,0,0, 1,0,0, 1,1,0, 0,1,0};
 float rotatedUV[]={0,1,0, 0,0,0, 1,0,0, 1,1,0};
 unsigned int idx[]={0,1,2,0,2,3}; float norms[]={0,0,1}; unsigned int ni[]={0,0,0,0,0,0};
 bool ok=true;
 if(producer==1) {
  RISE::ITriangleMeshGeometryIndexed* mesh=0; RISE::RISE_API_CreateTriangleMeshGeometryIndexed(&mesh,true,false);
  auto* m=dynamic_cast<RISE::ITriangleMeshGeometryIndexed3*>(mesh); ok=m!=0;
  if(m) { m->BeginIndexedTriangles();
   for(int i=0;i<4;++i) { m->AddVertex(RISE::Vertex(verts[i*3],verts[i*3+1],0)); m->AddNormal(RISE::Normal(0,0,1)); m->AddTexCoord(RISE::TexCoord(uv[i*3],uv[i*3+1]));
    RISE::Tangent4 t; t.dir=RISE::Vector3(0,1,0); t.bitangentSign=1; m->AddTangent(t); }
   for(int i=0;i<2;++i) { RISE::IndexedTriangle t; for(int k=0;k<3;++k) t.iVertices[k]=t.iNormals[k]=t.iCoords[k]=idx[i*3+k]; m->AddIndexedTriangle(t); }
   m->DoneIndexedTriangles(); ok=j->AddPrebuiltTriangleMeshGeometry("quad",m); }
  RISE::safe_release(mesh);
 } else if(producer==3 || producer==4) {
  const std::string stem=std::string("/tmp/rise-dl213-gltf-")+std::to_string(getpid())+"-"+std::to_string(producer);
  float ns[]={0,0,1,0,0,1,0,0,1,0,0,1},ts[]={0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1};
  const float* selectedUV=producer==3?uv:rotatedUV;float uv2[8];for(int k=0;k<4;++k){uv2[k*2]=selectedUV[k*3];uv2[k*2+1]=selectedUV[k*3+1];}
  {std::ofstream file(stem+".bin",std::ios::binary);file.write(reinterpret_cast<const char*>(verts),48);file.write(reinterpret_cast<const char*>(ns),48);file.write(reinterpret_cast<const char*>(uv2),32);file.write(reinterpret_cast<const char*>(ts),64);file.write(reinterpret_cast<const char*>(idx),24);}
  {std::ofstream file(stem+".gltf");file<<"{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\""<<stem.substr(stem.find_last_of('/')+1)<<".bin\",\"byteLength\":216}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48},{\"buffer\":0,\"byteOffset\":48,\"byteLength\":48},{\"buffer\":0,\"byteOffset\":96,\"byteLength\":32},{\"buffer\":0,\"byteOffset\":128,\"byteLength\":64},{\"buffer\":0,\"byteOffset\":192,\"byteLength\":24}],\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\",\"min\":[-3,-3,0],\"max\":[3,3,0]},{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},{\"bufferView\":2,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},{\"bufferView\":3,\"componentType\":5126,\"count\":4,\"type\":\"VEC4\"},{\"bufferView\":4,\"componentType\":5125,\"count\":6,\"type\":\"SCALAR\"}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2"<<(producer==3?",\"TANGENT\":3":"")<<"},\"indices\":4}]}]}";}
  ok=j->AddGLTFTriangleMeshGeometry("quad",(stem+".gltf").c_str(),0,0,true,false,false);
  std::remove((stem+".gltf").c_str());std::remove((stem+".bin").c_str());
 } else {
  rise_blender_mesh m={}; m.name="quad"; m.vertices=verts; m.normals=norms; m.uvs=producer==2?rotatedUV:uv; m.vertex_indices=idx; m.normal_indices=ni; m.uv_indices=idx; m.num_vertices=4; m.num_normals=1; m.num_uvs=4; m.num_triangles=2; m.double_sided=1;
#if RISE_BLENDER_API_VERSION >= 15
  float tangent[]={0,1,0,1, 0,1,0,1, 0,1,0,1, 0,1,0,1, 0,1,0,1, 0,1,0,1};
  if(producer==0) {m.tangent_attribute=tangent; m.num_tangents=6;}
#endif
  ok=add_mesh(*j,m,err,sizeof(err));
 }
 double white[]={1,1,1}; j->AddUniformColorPainter("white",white,"Rec709RGB_Linear");
 double black[]={0,0,0},ax[]={.04,.04,.04},ay[]={.35,.35,.35}; j->AddUniformColorPainter("black",black,"Rec709RGB_Linear");j->AddUniformColorPainter("ax",ax,"Rec709RGB_Linear");j->AddUniformColorPainter("ay",ay,"Rec709RGB_Linear");
 ok=ok&&j->AddGGXMaterial("brushed","black","white",".04",".35","1.5","0","schlick_f0","none");
 rise_blender_object o={}; o.name="surface";o.geometry_name="quad";o.material_name="brushed";o.visible=o.casts_shadows=o.receives_shadows=1;
 for(int k=0;k<4;++k)o.transform[k*4+k]=1;
 ok=ok&&add_object(*j,o,err,sizeof(err));
 double dir[]={0,0,1};ok=ok&&j->AddDirectionalLight("light",1,white,"Rec709RGB_Linear",dir);
 rise_blender_camera c={};c.projection_type=RISE_BLENDER_CAMERA_PERSPECTIVE;c.location[2]=5;c.forward[2]=-1;c.up[1]=1;c.fov_y_radians=.7f;c.width=c.height=65;c.pixel_aspect=1;
 ok=ok&&configure_camera(*j,c,err,sizeof(err));
 rise_blender_render_settings settings={};settings.width=settings.height=65;settings.pixel_samples=1;settings.max_recursion=1;settings.light_samples=1;settings.rasterizer_kind=RISE_BLENDER_RASTERIZER_PIXELPEL;
 rise_blender_scene sc={};ok=ok&&configure_shader(*j,settings,err,sizeof(err))&&configure_rasterizer(*j,settings,sc,err,sizeof(err));
 if(!ok||!j->GetRasterizer()){std::cout<<"setup error "<<err<<"\n";RISE::safe_release(j);return false;}
 auto* cap=new CapturingRasterizerOutput();j->GetRasterizer()->AddRasterizerOutput(cap);
 std::srand(12345);ok=ok&&j->Rasterize();pixels=cap->pixels;
 if(!ok)std::cout<<"setup/render error: "<<err<<"\n";
 RISE::safe_release(cap);RISE::safe_release(j);return ok&&!pixels.empty();
}
double Axis(const std::vector<RISE::RISEColor>& p) {double x=0,y=0;for(int j=0;j<65;++j)for(int i=0;i<65;++i){double w=p[j*65+i].base.r;x+=w*(i-32)*(i-32);y+=w*(j-32)*(j-32);}return x/y;}

#if RISE_BLENDER_API_VERSION >= 15
bool WiringChecks() {
 int checks=0, failed=0;
 auto check=[&](bool ok,const char* label){++checks;if(!ok){++failed;std::cout<<"FAIL "<<label<<"\n";}};
 RISE::IJobPriv* job=0;RISE::RISE_CreateJobPriv(&job);
 float vertices[]={0,0,0, 1,0,0, 0,1,0}; float normals[]={0,0,1};
 float uv[]={0,0,0, 1,0,0, 0,1,0};unsigned int index[]={0,1,2},ni[]={0,0,0};
 float tangents[]={1,0,0,-1, 0,1,0,-1, 1,1,0,-1};
 check(job->AddIndexedTriangleMeshGeometryWithTangents("interpolation",vertices,normals,uv,index,index,ni,3,1,3,1,true,false,tangents,3),"IJob tangent producer registers");
 auto* geometry=job->GetGeometries()->GetItem("interpolation"); check(geometry!=nullptr,"geometry exists");
 for(int back=0;back<2&&geometry;++back){RISE::RasterizerState rs={0,0};RISE::Ray ray(RISE::Point3(.25,.25,back?-1:1),RISE::Vector3(0,0,back?1:-1));RISE::RayIntersectionGeometric hit(ray,rs);geometry->IntersectRay(hit,true,true,false);
  check(hit.bHit&&hit.bHasTangent,"front/back authored tangent hit");
  check(std::abs(hit.vTangent.x-.75)<1e-9&&std::abs(hit.vTangent.y-.5)<1e-9,"front/back barycentric tangent interpolation");
  check(hit.bitangentSign==-1,"front/back authored handedness preserved");
 }
 check(!job->AddIndexedTriangleMeshGeometryWithTangents("badcount",vertices,normals,uv,index,index,ni,3,1,3,1,true,false,tangents,2),"partial tangent payload rejected");
 float bad[]={0,0,0,1,0,1,0,1,0,1,0,1};
 check(!job->AddIndexedTriangleMeshGeometryWithTangents("zero",vertices,normals,uv,index,index,ni,3,1,3,1,true,false,bad,3),"zero authored direction rejected");
 const std::string path=std::string("/tmp/rise-dl213-native-")+std::to_string(getpid())+".RISEscene";
 {std::ofstream file(path);file<<"RISE ASCII SCENE 7\nindexedmesh_geometry\n{\nname native_tangent\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\nnormal 0 0 1\nnormal 0 0 1\nnormal 0 0 1\nuv 0 0\nuv 1 0\nuv 0 1\ntangent 0 1 0 -1\ntangent 0 1 0 -1\ntangent 0 1 0 -1\ntriangle 0 1 2\n}\n";}
 check(job->LoadAsciiSceneViaCst(path.c_str()),"native CST chunk tangent producer derives");std::remove(path.c_str());
 auto* native=job->GetGeometries()->GetItem("native_tangent");check(native!=nullptr,"native geometry registered");
 if(native){RISE::RasterizerState rs={0,0};RISE::RayIntersectionGeometric hit(RISE::Ray(RISE::Point3(.25,.25,1),RISE::Vector3(0,0,-1)),rs);native->IntersectRay(hit,true,true,false);check(hit.bHit&&hit.bHasTangent&&hit.vTangent.y==1&&hit.bitangentSign==-1,"native tangent/sign reaches intersection");}
 // World transform: independent forward tangent and inverse-transpose normal.
 for(int mirror=0;mirror<2;++mirror){
  rise_blender_object o={};o.name=mirror?"mirror":"stretch";o.geometry_name="interpolation";o.material_name="none";o.visible=1;
  o.transform[0]=mirror?-2:2;o.transform[5]=3;o.transform[10]=1;o.transform[15]=1;char err[512]={0};check(add_object(*job,o,err,sizeof(err)),"transformed object producer registers");
  auto* object=job->GetObjects()->GetItem(o.name);
  for(int back=0;back<2&&object;++back){RISE::RasterizerState rs={0,0};RISE::RayIntersection hit(RISE::Ray(RISE::Point3(mirror?-.5:.5,.75,back?-1:1),RISE::Vector3(0,0,back?1:-1)),rs);object->IntersectRay(hit,RISE::RISE_INFINITY,true,true,false);
   check(hit.geometric.bHit,"front/back transformed hit");
   const double x=mirror?-1.5:1.5,y=1.5,len=std::hypot(x,y);const auto& onb=hit.geometric.onb;
   check(std::abs(onb.u().x-x/len)<1e-9&&std::abs(onb.u().y-y/len)<1e-9,"normal-projected forward transformed tangent");
   const double parity=mirror?1:-1;const auto& n=onb.w();
   check(std::abs(onb.v().x-parity*(-n.z*y/len))<1e-9&&std::abs(onb.v().y-parity*(n.z*x/len))<1e-9,"front/back/mirror independent bitangent sign");
  }
 }
 RISE::safe_release(job);std::cout<<"wiring "<<checks<<" checks, "<<failed<<" failures\n";return failed==0;
}
#else
bool WiringChecks(){return true;}
#endif
int main(){bool wiring=WiringChecks();std::vector<RISE::RISEColor> bridge,core,uv,gltf,gltfUV; if(!Render(0,bridge)||!Render(1,core)||!Render(2,uv)||!Render(3,gltf)||!Render(4,gltfUV))return 2;
 if(const char* path=std::getenv("RISE_TANGENT_FALLBACK_IMAGE")){std::ofstream out(path,std::ios::binary);for(const auto& p:uv){double rgb[]={p.base.r,p.base.g,p.base.b};out.write(reinterpret_cast<const char*>(rgb),sizeof(rgb));}}
 double a=Axis(bridge),b=Axis(core),c=Axis(uv),err=0;for(size_t i=0;i<core.size();++i)err=std::max(err,std::abs(core[i].base.r-uv[i].base.r));
 std::cout<<"axis bridge="<<a<<" authored="<<b<<" rotatedUV="<<c<<" core/UV maxdiff="<<err<<"\n";
 double gltfDiff=0;for(size_t i=0;i<gltf.size();++i)gltfDiff=std::max(gltfDiff,std::abs(gltf[i].base.r-gltfUV[i].base.r));
 std::cout<<"glTF TANGENT/rotatedUV maxdiff="<<gltfDiff<<" axis="<<Axis(gltf)<<"\n";
 bool positive=gltfDiff<1e-9&&Axis(gltf)>1.5&&b>1.5&&c>1.5&&err<1e-9; bool transported=a>1.5&&std::abs(a-b)<1e-9;
 std::cout<<"authored positive control "<<(positive?"PASS":"FAIL")<<"; missing bridge transport "<<(transported?"PASS":"FAIL")<<"\n";
 return positive&&transported&&wiring?0:1;
}
