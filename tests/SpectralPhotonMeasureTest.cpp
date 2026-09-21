// DL281: independently enumerated uniform spectral sampling and window mass.
#include <cstdio>
#include <cmath>
#include "../src/Library/PhotonMapping/GlobalSpectralPhotonMap.h"
#include "../src/Library/PhotonMapping/CausticSpectralPhotonMap.h"
#include "../src/Library/Materials/LambertianBRDF.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Utilities/Color/ColorUtils.h"
using namespace RISE;using namespace RISE::Implementation;
template<class Map> int run(const char* name){
 Map map(160,nullptr);for(unsigned i=0;i<160;++i)map.Store(1./160,400+i*300./160,Point3(.5,0,0),Vector3(0,0,1));map.Balance();
 auto* paint=new UniformColorPainter(RISEPel(1));auto* brdf=new LambertianBRDF(*paint);
 RayIntersectionGeometric q(Ray(Point3(0,0,1),Vector3(0,0,-1)),nullRasterizerState);q.ptIntersection=Point3(0,0,0);q.vNormal=q.vGeomNormal=Vector3(0,0,1);q.onb.CreateFromW(q.vNormal);
 int fails=0;
 for(double halfwidth:{0.,1.,10.,20.,300.}){
  map.SetGatherParamsNM(1,.05,0,160,halfwidth,nullptr);RISEPel rgb;map.RadianceEstimate(rgb,q,*brdf);XYZPel xyz(rgb);
  double y=0,predicted=0,ciesum=0;
  for(unsigned j=0;j<160;++j){double nm=400+j*300./160,got=0;map.RadianceEstimateNM(nm,got,q,*brdf);XYZPel cmf;ColorUtils::XYZFromNM(cmf,nm);y+=cmf.Y*got/160;ciesum+=cmf.Y/160;unsigned k=0;for(unsigned i=0;i<160;++i)if(std::fabs((400+i*300./160)-nm)<=halfwidth)++k;predicted+=cmf.Y*double(k)/160/160/(.25*PI*PI);}
  double norm=300/ColorUtils::CIE_Y_Integral(400,700);y*=norm;predicted*=norm;
  double expected=norm*ciesum/(.25*PI*PI);
  bool valid=std::fabs(y-predicted)<1e-12;bool correct=std::fabs(y-expected)<1e-12;if(!correct)++fails;
  std::printf("%s halfwidth=%.17g PelY=%.17g NMY=%.17g formula=%.17g physical_flatY=%.17g source_formula_match=%d normalized=%d\n",name,halfwidth,xyz.Y,y,predicted,expected,int(valid),int(correct));
  if(!valid)return 99;
 }
 brdf->release();paint->release();return fails;
}
int main(){int failed=run<GlobalSpectralPhotonMap>("global")+run<CausticSpectralPhotonMap>("caustic");std::printf("NORMALIZATION_FAILED_ROWS=%d\n",failed);return failed?1:0;}
