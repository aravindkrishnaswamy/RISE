// DL239: original incident support survives packet storage. In particular,
// quantization must not turn below-Ng packets into positive near-zero cosines.
#include <cmath>
#include <cstdio>
#include "../src/Library/PhotonMapping/CausticPelPhotonMap.h"
#include "../src/Library/PhotonMapping/GlobalPelPhotonMap.h"
#include "../src/Library/PhotonMapping/TranslucentPelPhotonMap.h"
#include "../src/Library/PhotonMapping/CausticSpectralPhotonMap.h"
#include "../src/Library/PhotonMapping/GlobalSpectralPhotonMap.h"
#include "../src/Library/Materials/LambertianBRDF.h"
#include "../src/Library/Painters/UniformColorPainter.h"
using namespace RISE;using namespace RISE::Implementation;
namespace {
unsigned checks=0,failures=0;
void Near(double got,double expected,const char* label,double rotation,double z){
 ++checks;const bool ok=std::isfinite(got)&&std::fabs(got-expected)<=2e-12*std::fabs(expected);
 if(!ok){++failures;std::printf("FAIL %s rotation=%.17g z=%.17g got=%.17g expected=%.17g\n",label,rotation,z,got,expected);}
}
void Run(){
 auto* paint=new UniformColorPainter(RISEPel(.8));auto* brdf=new LambertianBRDF(*paint);
 for(double angle:{0.,.37,1.12}){
  const Vector3 ng(std::sin(angle),0,std::cos(angle)),t(std::cos(angle),0,-std::sin(angle));
  const Vector3 ns=(ng+t)*std::sqrt(.5);const Point3 position(t.x*.5,t.y*.5,t.z*.5);
  RayIntersectionGeometric q(Ray(Point3(ng.x,ng.y,ng.z),-ng),nullRasterizerState);q.ptIntersection=Point3(0,0,0);q.vNormal=ns;q.vGeomNormal=ng;q.onb.CreateFromW(ns);
  for(double z:{-.01,-.001,.001,.01}){
   const Vector3 wi=t*std::sqrt(1-z*z)+ng*z;
   const double ngwi=Vector3Ops::Dot(ng,wi),nswi=Vector3Ops::Dot(ns,wi);
   const double bsdf=ngwi>0&&nswi>0?.8/PI:0;
   const double area=ngwi!=0?std::fabs(nswi/ngwi):0;
   const double alpha=.918,beta=1.953,D=1-std::exp(-beta);
   const double norm=2*alpha*(.5*(1-1/D)+(1-std::exp(-beta*.5))/(beta*D));
   const double weight=alpha*(1-(1-std::exp(-beta*.5))/D);
   const double gaussian=bsdf*area*weight/(PI*.25*norm);
   CausticPelPhotonMap cp(1,nullptr);cp.Store(RISEPel(1),position,wi);cp.Balance();cp.SetGatherParams(1,.05,0,1,nullptr);RISEPel v;cp.RadianceEstimate(v,q,*brdf);Near(v.r,gaussian,"caustic Pel original direction",angle,z);
   GlobalPelPhotonMap gp(1,nullptr);gp.Store(RISEPel(1),position,ng,wi);gp.Balance();gp.SetGatherParams(1,.05,0,1,nullptr);gp.PrecomputeIrradiance(0,nullptr);gp.RadianceEstimate(v,q,*brdf);Near(v.r,gaussian,"global direct original direction",angle,z);
   CausticSpectralPhotonMap cs(1,nullptr);cs.ConfigureWavelengthSampling(550,555,1);cs.Store(1,550,position,wi);cs.Balance();cs.SetGatherParamsNM(1,.05,0,1,1,nullptr);double scalar=0;cs.RadianceEstimateNM(550,scalar,q,*brdf);
   // Painter's scalar spectrum is evaluated independently of map storage.
   const double scalarExpected=ngwi>0&&nswi>0?brdf->valueNM(wi,q,550)*area/(PI*.25):0;
   Near(scalar,scalarExpected,"caustic spectral original direction",angle,z);
   GlobalSpectralPhotonMap gs(1,nullptr);gs.ConfigureWavelengthSampling(550,555,1);gs.Store(1,550,position,wi);gs.Balance();gs.SetGatherParamsNM(1,.05,0,1,1,nullptr);gs.RadianceEstimateNM(550,scalar,q,*brdf);Near(scalar,scalarExpected,"global spectral original direction",angle,z);
   GlobalPelPhotonMap cache(9,nullptr);
   for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x)cache.Store(RISEPel(1),Point3(t.x*x*.1,y*.1,t.z*x*.1),ng,wi);
   cache.Balance();cache.SetGatherParams(1,.05,0,9,nullptr);cache.PrecomputeIrradiance(1,nullptr);cache.RadianceEstimate(v,q,*brdf);Near(v.r,9*bsdf*area/(PI*.02),"anchor original direction",angle,z);
  }
 }
 // Exactly representable tangent input: no tiny positive cos(pi/2) surrogate.
 RayIntersectionGeometric q(Ray(Point3(0,0,1),Vector3(0,0,-1)),nullRasterizerState);q.ptIntersection=Point3(0,0,0);q.vGeomNormal=Vector3(0,0,1);q.vNormal=Vector3(std::sqrt(.5),0,std::sqrt(.5));q.onb.CreateFromW(q.vNormal);
 CausticPelPhotonMap tangent(1,nullptr);tangent.Store(RISEPel(1),Point3(.5,0,0),Vector3(1,0,0));tangent.Balance();tangent.SetGatherParams(1,.05,0,1,nullptr);RISEPel v;tangent.RadianceEstimate(v,q,*brdf);Near(v.r,0,"exact tangent has zero area measure",0,0);

 // Exactly co-located packets define no positive-area density kernel.
 // This is a degenerate neighborhood, not a tiny-radius threshold.
 CausticPelPhotonMap zero(1,nullptr);zero.Store(RISEPel(1),Point3(0,0,0),Vector3(0,0,1));zero.Balance();zero.SetGatherParams(1,.05,0,1,nullptr);zero.RadianceEstimate(v,q,*brdf);Near(v.r,0,"co-located caustic Pel has no finite area",0,0);
 GlobalPelPhotonMap zeroGlobal(1,nullptr);zeroGlobal.Store(RISEPel(1),Point3(0,0,0),Vector3(0,0,1),Vector3(0,0,1));zeroGlobal.Balance();zeroGlobal.SetGatherParams(1,.05,0,1,nullptr);zeroGlobal.RadianceEstimate(v,q,*brdf);Near(v.r,0,"co-located anchored global has no finite area",0,0);zeroGlobal.PrecomputeIrradiance(0,nullptr);zeroGlobal.RadianceEstimate(v,q,*brdf);Near(v.r,0,"co-located direct global has no finite area",0,0);
 CausticSpectralPhotonMap zeroCS(1,nullptr);zeroCS.ConfigureWavelengthSampling(550,555,1);zeroCS.Store(1,550,Point3(0,0,0),Vector3(0,0,1));zeroCS.Balance();zeroCS.SetGatherParamsNM(1,.05,0,1,1,nullptr);zeroCS.RadianceEstimate(v,q,*brdf);Near(v.r,0,"co-located caustic spectral Pel has no finite area",0,0);double n=0;zeroCS.RadianceEstimateNM(550,n,q,*brdf);Near(n,0,"co-located caustic spectral NM has no finite area",0,0);
 GlobalSpectralPhotonMap zeroGS(1,nullptr);zeroGS.ConfigureWavelengthSampling(550,555,1);zeroGS.Store(1,550,Point3(0,0,0),Vector3(0,0,1));zeroGS.Balance();zeroGS.SetGatherParamsNM(1,.05,0,1,1,nullptr);zeroGS.RadianceEstimate(v,q,*brdf);Near(v.r,0,"co-located global spectral Pel has no finite area",0,0);zeroGS.RadianceEstimateNM(550,n,q,*brdf);Near(n,0,"co-located global spectral NM has no finite area",0,0);
 for(bool exit:{false,true}){
  TranslucentPelPhotonMap zeroT(1,nullptr);zeroT.Store(RISEPel(1),Point3(0,0,0),Vector3(0,0,1),exit);zeroT.Balance();zeroT.SetGatherParams(1,.05,0,1,nullptr);zeroT.RadianceEstimate(v,q,*brdf);Near(v.r,0,exit?"co-located translucent exit has no finite area":"co-located translucent incident has no finite area",0,0);
 }
 brdf->release();paint->release();
}
}
int main(){std::printf("record bytes: Photon=%zu IrradPhoton=%zu SpectralPhoton=%zu TranslucentPhoton=%zu samplingLaw=%zu\n",sizeof(Photon),sizeof(IrradPhoton),sizeof(SpectralPhoton),sizeof(TranslucentPhoton),sizeof(SpectralPhotonSamplingLaw));Run();std::printf("PhotonDirectionSupportTest checks=%u failures=%u\n",checks,failures);return failures?1:0;}
