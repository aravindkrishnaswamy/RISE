// DL281: finite spectral deposits priced by an independently enumerated
// wavelength law. The spatial neighborhood is fixed and complete (160 packets).
#include <cmath>
#include <cstdio>
#include <limits>
#include "../src/Library/PhotonMapping/GlobalSpectralPhotonMap.h"
#include "../src/Library/PhotonMapping/CausticSpectralPhotonMap.h"
#include "../src/Library/Materials/LambertianBRDF.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Utilities/Color/ColorUtils.h"
using namespace RISE;using namespace RISE::Implementation;
namespace {
unsigned checks=0,failures=0;
void Check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL %s\n",label);}}
void Near(double got,double expected,const char* label){Check(std::isfinite(got)&&std::fabs(got-expected)<=1e-11*std::max(std::fabs(expected),1e-12),label);if(!std::isfinite(got)||std::fabs(got-expected)>1e-11*std::max(std::fabs(expected),1e-12))std::printf(" got=%.17g expected=%.17g\n",got,expected);}
template<class Map>void Run(const char* family){
 std::printf("FAMILY %s\n",family);
 for(bool colored:{false,true}){
  Map map(160,nullptr);Check(!map.Store(1,550,Point3(.5,0,0),Vector3(0,0,1))&&map.NumStored()==0,"unconfigured Store rejects without mutation");
  Check(map.ConfigureWavelengthSampling(400,700,160),"configure actual producer grid");
  auto* paint=new UniformColorPainter(colored?RISEPel(.15,.7,.35):RISEPel(1));auto* brdf=new LambertianBRDF(*paint);
  RayIntersectionGeometric q(Ray(Point3(0,0,1),Vector3(0,0,-1)),nullRasterizerState);q.ptIntersection=Point3(0,0,0);q.vNormal=q.vGeomNormal=Vector3(0,0,1);q.onb.CreateFromW(q.vNormal);
  const Vector3 wi(0,0,1);const double area=PI*.25;
  std::vector<double> wavelengths;for(unsigned i=0;i<160;++i)wavelengths.push_back(map.SampleWavelength((i+.5)/160));
  for(unsigned i=0;i<160;++i){const double nm=wavelengths[i],power=colored?1+.005*(nm-400):1;Check(map.Store(power/160,nm,Point3(.5,0,0),wi),"grid packet stored");}
  Check(!map.ConfigureWavelengthSampling(380,780,160)&&map.NumStored()==160,"changing populated map law rejects without mutation");map.Balance();
  XYZPel expectedXYZ(0,0,0);
  for(unsigned i=0;i<160;++i){const double nm=wavelengths[i],power=colored?1+.005*(nm-400):1;XYZPel cmf;ColorUtils::XYZFromNM(cmf,nm);expectedXYZ=expectedXYZ+cmf*(power*brdf->valueNM(wi,q,nm)/160);}
  const double norm=300/ColorUtils::CIE_Y_Integral(400,700);const RISEPel expectedPel(expectedXYZ*(norm/area));
  for(double width:{0.,1.,10.,20.,300.}){
   map.SetGatherParamsNM(1,.05,0,160,width,nullptr);RISEPel rgb;map.RadianceEstimate(rgb,q,*brdf);
   for(unsigned c=0;c<3;++c)Near(rgb[c],expectedPel[c],"Pel CMF integral uses scalar material at each photon wavelength");
   double observedY=0,expectedY=0;
   for(unsigned j=0;j<160;++j){
    const double nm=wavelengths[j];double sum=0;unsigned accepted=0;
    for(unsigned i=0;i<160;++i){const double stored=wavelengths[i];if(std::fabs(stored-nm)<=width){sum+=colored?1+.005*(stored-400):1;++accepted;}}
    const double expected=accepted?sum/accepted*brdf->valueNM(wi,q,nm)/area:0;double got=0;map.RadianceEstimateNM(nm,got,q,*brdf);Near(got,expected,"NM kernel divides exact discrete sampling mass including clipped edges");
    XYZPel cmf;ColorUtils::XYZFromNM(cmf,nm);observedY+=cmf.Y*got/160*norm;expectedY+=cmf.Y*expected/160*norm;
   }
   Near(observedY,expectedY,"camera-normalized spectral integral");std::printf("%s colored=%d width=%.17g observedY=%.17g expectedY=%.17g\n",family,int(colored),width,observedY,expectedY);
  }
  for(double nm:{399.,700.,701.}){double got=42;map.RadianceEstimateNM(nm,got,q,*brdf);Near(got,0,"out-of-support observer wavelength is zero");}
  brdf->release();paint->release();
 }
}
void SamplingBoundaries(){
 SpectralPhotonSamplingLaw law;
 for(unsigned n:{1u,2u,160u,9999u}){
  Check(law.Configure(400,700,n),"valid finite grid");
  std::printf("represented grid N=%u size=%zu capacity=%zu retained_bytes=%zu\n",n,law.representatives.size(),law.representatives.capacity(),law.representatives.capacity()*sizeof(Scalar));
  for(double u:{0.,.5,std::nextafter(1.,0.)}){
   const unsigned index=static_cast<unsigned>(u*n);Near(law.Sample(u),law.representatives[index],"discrete sampling selects its represented atom without another random draw");Check(law.Contains(law.Sample(u)),"sample lies in represented grid");
  }
  for(double query:{400.,std::nextafter(400.,700.),550.,std::nextafter(700.,400.)})for(double width:{0.,std::nextafter(300./n,0.),300./n,std::nextafter(300./n,300.),300.}){
   unsigned k=0;for(unsigned i=0;i<n;++i)if(std::fabs(law.representatives[i]-query)<=width)++k;
   Near(law.WindowMass(query,width),double(k)/n,"binary count agrees with full represented-grid enumeration");
  }
 }
 Check(law.Configure(400,700,10000),"continuous uniform law");Near(law.WindowMass(400,10),10./300,"continuous lower clipping");Near(law.WindowMass(695,10),15./300,"continuous upper clipping");Near(law.WindowMass(550,0),0,"continuous zero-width window has zero mass");
 Check(law.Sample(std::nextafter(1.,0.))<700&&law.Sample(std::nextafter(1.,0.))>=400,"rounded upper endpoint remains within finite-precision support");Near(law.Sample(0),400,"continuous lower endpoint");
 GlobalSpectralPhotonMap map(2,nullptr);Check(map.ConfigureWavelengthSampling(400,700,160),"metadata control configured");
 for(auto bad:{0.,-1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){
  Check(!map.ConfigureWavelengthSampling(bad,700,160),"invalid lower bound rejected");double a=0,b=0;unsigned n=0;Check(map.GetWavelengthSampling(a,b,n)&&a==400&&b==700&&n==160,"invalid law leaves previous metadata unchanged");
 }
 Check(!map.ConfigureWavelengthSampling(700,400,160)&&!map.ConfigureWavelengthSampling(400,700,0),"reversed or empty sampling law rejected");
 Check(!map.Store(1,401,Point3(0,0,0),Vector3(0,0,1))&&map.NumStored()==0,"off-grid insert rejected without mutation");
}
}
int main(){Run<GlobalSpectralPhotonMap>("global");Run<CausticSpectralPhotonMap>("caustic");SamplingBoundaries();std::printf("SpectralPhotonMeasureTest checks=%u failures=%u\n",checks,failures);return failures?1:0;}
