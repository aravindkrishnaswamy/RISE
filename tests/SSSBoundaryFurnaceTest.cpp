// DL-334: monochromatic slab furnace through the real PT NM/HWSS entry points.
// Independent reference: conservative complete scattering returns environment
// radiance; a camera in a uniform exterior reads n_camera^2 times air radiance.
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>
#include <unistd.h>
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IObject.h"
#include "../src/Library/Shaders/PathTracingIntegrator.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/IORStackSeeding.h"
#include "../src/Library/Utilities/Color/SampledWavelengths.h"
using namespace RISE;
using namespace RISE::Implementation;
namespace RISE { bool RISE_CreateJobPriv(IJobPriv**); }
// Keep the regression source buildable against committed original production.
template<class F> static void Seed(F f, IORStack& stack, const Point3& p, const IScene& scene, double nm) {
 if constexpr(std::is_invocable_v<F,IORStack&,const Point3&,const IScene&,Scalar>) f(stack,p,scene,nm);
 else f(stack,p,scene);
}
static std::string Scratch() { return std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp"; }
static int checks=0,failures=0;
static void Check(bool ok,const char* label) { checks++;if(!ok){failures++;std::printf("FAIL %s\n",label);} }
static std::string Scene(bool rw,int ambient,int control) {
 std::ostringstream s;
 s<<"RISE ASCII SCENE 7\nfilm\n{\nwidth 16\nheight 16\n}\n"
  <<"orthographic_camera\n{\nlocation 0 0 4\nlookat 0 0 0\nup 0 1 0\nviewport_scale 2 2\n}\n"
  <<"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
  <<"scalar_painter\n{\nname curve\nfile "<<Scratch()<<"/dl334_measured_"<<getpid()<<".txt\n}\n";
 if(control==2)s<<"perfectrefractor_material\n{\nname subject\nior curve\nrefractance white\n}\n";
 else if(control)s<<"lambertian_material\n{\nname subject\nreflectance white\n}\n";
 else s<<(rw?"randomwalk_sss_material":"subsurfacescattering_material")<<"\n{\nname subject\nior curve\nabsorption 0\nscattering "<<(rw?2:200)<<"\ng 0\nroughness 0\n"<<(rw?"max_bounces 8192\n":"")<<"}\n";
 s<<"box_geometry\n{\nname slab_geo\nwidth 10\nheight 10\ndepth 1\n}\nstandard_object\n{\nname slab\ngeometry slab_geo\nmaterial subject\n}\n";
 if(ambient) {
   s<<"perfectrefractor_material\n{\nname ambient_mat\nior "<<(ambient==1?"1.2":"curve")<<"\nrefractance white\n}\nsphere_geometry\n{\nname ambient_geo\nradius 60\n}\nstandard_object\n{\nname ambient\ngeometry ambient_geo\nmaterial ambient_mat\n}\n";
 }
 if(ambient==3) {
   s<<"perfectrefractor_material\n{\nname inner_mat\nior 1.1\nrefractance white\n}\nsphere_geometry\n{\nname inner_geo\nradius 20\n}\nstandard_object\n{\nname inner\ngeometry inner_geo\nmaterial inner_mat\n}\n";
 }
 s<<"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\npathtracing_spectral_rasterizer\n{\nsamples 1\nrr_min_depth 20\noidn_denoise FALSE\nradiance_map white\nradiance_scale 1\nradiance_background TRUE\n}\n";
 return s.str();
}
static void BoundaryChain(IJobPriv& job,double nm) {
 const IObject* objects[4]={job.GetObjects()->GetItem("ambient"),job.GetObjects()->GetItem("inner"),job.GetObjects()->GetItem("inner"),job.GetObjects()->GetItem("ambient")};
 const Point3 starts[4]={Point3(0,0,80),Point3(0,0,40),Point3(0,0,10),Point3(0,0,30)};
 const Vector3 dirs[4]={Vector3(0,0,-1),Vector3(0,0,-1),Vector3(0,0,1),Vector3(0,0,1)};
 IORStack stack(1.17);double factor=1;
 RayIntersectionGeometric query(Ray(Point3(0,0,0),Vector3(0,0,-1)),nullRasterizerState);
 const double outer=job.GetScalarPainters()->GetItem("curve")->GetValueAtNM(query,nm);
 const double after[4]={outer,1.1,outer,1.17};
 RandomNumberGenerator rng(17);IndependentSampler sampler(rng);
 for(unsigned k=0;k<4;k++) {
  RayIntersection hit(Ray(starts[k],dirs[k]),nullRasterizerState);
  objects[k]->IntersectRay(hit,1000,true,true,false);Check(hit.geometric.bHit,"public nested boundary hit");
  IORStack replay(1.17);const Point3 mid=hit.geometric.ray.PointAtLength(hit.geometric.range*.5);
  Seed(&IORStackSeeding::SeedFromPoint,replay,mid,*job.GetScene(),nm);
  Check(std::fabs(replay.top()-stack.top())<1e-12,"incoming segment midpoint exterior");
  stack.SetCurrentObject(objects[k]);ScatteredRayContainer rays;
  hit.pMaterial->GetSPF()->ScatterNM(hit.geometric,sampler,nm,rays,stack);
  bool found=false;
  for(unsigned r=0;r<rays.Count();r++)if(rays[r].type==ScatteredRay::eRayRefraction && rays[r].ior_stack) {
   found=true;Check(std::fabs(rays[r].ior_stack->top()-after[k])<1e-12,"public enter exit wavelength chain");
   factor*=RadianceEtaScale(stack,rays[r].ior_stack);stack=*rays[r].ior_stack;break;
  }
  Check(found,"nested transmitted ray");
 }
 Check(std::fabs(stack.top()-1.17)<1e-12&&std::fabs(factor-1)<1e-12,"environment and eta square telescope");
 // A grazing incoming segment intersects the outer sphere alone. Its midpoint
 // stays outside even arbitrarily close to tangency; seeding must retain root.
 RayIntersection grazing(Ray(Point3(59.9,0,80),Vector3(0,0,-1)),nullRasterizerState);
 objects[0]->IntersectRay(grazing,1000,true,true,false);
 Check(grazing.geometric.bHit,"grazing public sphere hit");IORStack outside(1.17);
 Seed(&IORStackSeeding::SeedFromPoint,outside,grazing.geometric.ray.PointAtLength(grazing.geometric.range*.5),*job.GetScene(),nm);
 Check(outside.top()==1.17,"grazing midpoint physical exterior");
}
int main() {
 const std::string curveFile=Scratch()+"/dl334_measured_"+std::to_string(getpid())+".txt";
 {std::ofstream f(curveFile); f<<"380 1.8\n465 1.6\n549 1.35\n611 1.2\n780 1.1\n";}
 const double nm[4]={420,500,611,700};
 for(bool rw:{false,true}) for(int ambient:{0,1,2,3}) for(int control:{0,1,2}) {
  IJobPriv* job=nullptr;Check(RISE_CreateJobPriv(&job),"create job");if(!job)continue;
  const std::string sceneFile=Scratch()+"/dl334_furnace_"+std::to_string(getpid())+".RISEscene";
  {std::ofstream f(sceneFile);f<<Scene(rw,ambient,control);}
  const bool loaded=job->LoadAsciiSceneViaCst(sceneFile.c_str());Check(loaded,"load furnace");std::remove(sceneFile.c_str());if(!loaded){job->release();continue;}
  IRayCaster* caster=nullptr;const IShader* shader=job->GetShaders()->GetItem("global");
  Check(shader && RISE_API_CreateRayCaster(&caster,true,2000,*shader,true),"create caster");
  if(!caster){job->release();continue;}caster->AttachScene(job->GetScene());
  StabilityConfig config;config.maxTranslucentBounce=1000;config.rrMinDepth=20;
  PathTracingIntegrator* pt=new PathTracingIntegrator(ManifoldSolverConfig(),config);pt->SetMaxPathDepth(1000);
  const IRadianceMap* env=job->GetScene()->GetGlobalRadianceMap();Check(env!=nullptr,"live environment");
  if(ambient==3&&control==2)for(double w:nm)BoundaryChain(*job,w);
  RandomNumberGenerator rng(91);IndependentSampler sampler(rng);RuntimeContext rc(rng,RuntimeContext::PASS_NORMAL,false);rc.pSampler=&sampler;
  const Ray ray(Point3(0,0,4),Vector3(0,0,-1));
  // Independent incident-medium reference, including an outer dispersive
  // enclosure hidden by a constant inner volume in the nested row.
  for(unsigned w=0;w<4;w++) {
   IORStack stack(1);Seed(&IORStackSeeding::SeedFromPoint,stack,ray.origin,*job->GetScene(),nm[w]);
   const double nc=ambient==0?1:ambient==1?1.2:ambient==3?1.1:job->GetScalarPainters()->GetItem("curve")->GetValueAtNM(RayIntersectionGeometric(ray,nullRasterizerState),nm[w]);
   Check(std::fabs(stack.top()-nc)<1e-12,"spectral containment top");
   double sum=0,sum2=0;const int n=60000;
   for(int i=0;i<n;i++) {double v=pt->IntegrateRayNM(rc,nullRasterizerState,ray,nm[w],*job->GetScene(),*caster,sampler,env)/(env->GetRadianceNM(ray,nullRasterizerState,nm[w])*nc*nc);sum+=v;sum2+=v*v;}
   const double mean=sum/n,sd=std::sqrt(std::fmax(0.,sum2/n-mean*mean)),se=sd/std::sqrt(double(n));
   std::printf("NM rw=%d ambient=%d control=%d nm=%.0f N=%d mean=%.9g se=%.6g\n",rw,ambient,control,nm[w],n,mean,se);
   Check(std::isfinite(mean)&&se<0.02&&std::fabs(mean-1)<0.015+6*se,"NM conservative slab furnace");
  }
  double sums[4]={},sums2[4]={}; const int n=60000;
  for(int i=0;i<n;i++) {
   SampledWavelengths swl;for(unsigned w=0;w<4;w++){swl.lambda[w]=nm[w];swl.pdf[w]=0.0025;}
   if(i==0)Check(!swl.terminated[0]&&!swl.terminated[1]&&!swl.terminated[2]&&!swl.terminated[3],"all HWSS lanes active");
   double values[4];pt->IntegrateRayHWSS(rc,nullRasterizerState,ray,swl,*job->GetScene(),*caster,sampler,env,values);
   for(unsigned w=0;w<4;w++){sums[w]+=values[w];sums2[w]+=values[w]*values[w];}
  }
  for(unsigned w=0;w<4;w++) {
   const double nc=ambient==0?1:ambient==1?1.2:ambient==3?1.1:job->GetScalarPainters()->GetItem("curve")->GetValueAtNM(RayIntersectionGeometric(ray,nullRasterizerState),nm[w]);
   const double mean=sums[w]/n/(env->GetRadianceNM(ray,nullRasterizerState,nm[w])*nc*nc);
   const double reference=env->GetRadianceNM(ray,nullRasterizerState,nm[w])*nc*nc;
   const double se=std::sqrt(std::fmax(0.,sums2[w]/n-(sums[w]/n)*(sums[w]/n))/n)/reference;
   std::printf("HWSS rw=%d ambient=%d control=%d nm=%.0f N=%d mean=%.9g se=%.6g\n",rw,ambient,control,nm[w],n,mean,se);
   Check(std::isfinite(mean)&&se<0.02&&std::fabs(mean-1)<0.015+6*se,"HWSS companion slab furnace");
  }
  pt->release();caster->release();job->release();
 }
 std::remove(curveFile.c_str());std::printf("Checks: %d Failures: %d\n",checks,failures);return failures?1:0;
}
