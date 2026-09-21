// DL239: fixed directional deposits distinguish cache-area geometry from
// query BSDF/shading frame. No rasterizer or stochastic acceptance bands.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include "../src/Library/PhotonMapping/GlobalPelPhotonMap.h"
#include "../src/Library/PhotonMapping/CausticPelPhotonMap.h"
#include "../src/Library/PhotonMapping/GlobalSpectralPhotonMap.h"
#include "../src/Library/PhotonMapping/CausticSpectralPhotonMap.h"
#include "../src/Library/Materials/LambertianBRDF.h"
#include "../src/Library/Materials/IsotropicPhongBRDF.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
using namespace RISE;using namespace RISE::Implementation;
namespace {
int checks=0,failures=0;
void Check(bool b,const char* label,double a,double e){++checks;if(!b)++failures;std::printf("%s %s got=%.17g expected=%.17g\n",b?"PASS":"FAIL",label,a,e);}
void Near(double a,double e,const char* label){Check(std::isfinite(a)&&std::fabs(a-e)<=1e-10*std::max(1.,std::fabs(e)),label,a,e);}
class CacheProbe:public GlobalPelPhotonMap {
public:
 CacheProbe(unsigned n):GlobalPelPhotonMap(n,nullptr){}
 void RawParams(){PhotonMapCore<IrradPhoton>::SetGatherParams(.2,.05,10,400,nullptr);}
 Vector3 FirstDirection()const{return PhotonDir(vphotons[0].theta,vphotons[0].phi);}
 std::vector<double> Distances(const Point3& p)const{PhotonDistListType heap;LocatePhotons(p,dGatherRadius,nMaxPhotonsOnGather,heap,0,static_cast<int>(vphotons.size())-1);std::vector<double> d;for(const auto& h:heap)d.push_back(h.distance);std::sort(d.begin(),d.end());return d;}
 Point3 NearestPosition(const Point3& p,const Vector3& n)const{IrradPhoton dummy;distance_container<IrradPhoton> nearest(dummy,RISE_INFINITY);LocateNearestPhoton(p,n,dGatherRadius,nearest);return nearest.element.ptPosition;}
 bool HasAnchor(const Point3& p,const Vector3& n)const{IrradPhoton dummy;distance_container<IrradPhoton> nearest(dummy,RISE_INFINITY);LocateNearestPhoton(p,n,dGatherRadius,nearest);return nearest.distance<RISE_INFINITY;}
};
// Independent finite-kernel oracle: sort all deposits by Euclidean distance,
// retain400, use the legacy uniform disk with geometric anchor area. Query
// material evaluation remains at query (including its shading/view frame).
struct Deposit {Point3 p;RISEPel power;};
RISEPel Reference(const std::vector<Deposit>& deposits,const Point3& anchor,const Vector3& areaNormal,const Vector3& wi,const RayIntersectionGeometric& query,const IBSDF& brdf,bool angular,bool gaussian=false){
 std::vector<std::pair<double,const Deposit*> > sorted;
 for(const auto& p:deposits){const Vector3 d=Vector3Ops::mkVector3(p.p,anchor);const double d2=Vector3Ops::SquaredModulus(d);if(d2<.04)sorted.emplace_back(d2,&p);}
 std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.first<b.first;});if(sorted.size()>400)sorted.resize(400);
 RISEPel sum(0.0);if(sorted.size()<=10)return sum;const double r2=sorted.back().first;
 const double alpha=.918,beta=1.953,D=1-std::exp(-beta);const double norm=gaussian?2*alpha*(.5*(1-1/D)+(1-std::exp(-beta*.5))/(beta*D)):1;
 for(const auto& p:sorted){const double height=Vector3Ops::Dot(Vector3Ops::mkVector3(p.second->p,anchor),areaNormal);if(std::fabs(height)<r2*.05){const double weight=gaussian?alpha*(1-(1-std::exp(-beta*p.first/(2*r2)))/D):1;sum=sum+p.second->power*weight;}}
 double factor=angular?std::fabs(Vector3Ops::Dot(query.vNormal,wi)/Vector3Ops::Dot(areaNormal,wi)):1;
 return sum*brdf.value(wi,query)*(factor/(PI*r2*norm));
}

void DirectGathers(){
 auto* paint=new UniformColorPainter(RISEPel(.8,.5,.2));auto* bsdf=new LambertianBRDF(*paint);
 for(double incidence:{-80.,0.,45.,180.}){
  CacheProbe global(2601);CausticPelPhotonMap caustic(2601,nullptr);GlobalSpectralPhotonMap globalNM(2601,nullptr);CausticSpectralPhotonMap causticNM(2601,nullptr);
  const double a=incidence*PI/180;const Vector3 incoming(std::sin(a),0,std::cos(a));const Vector3 ng(0,0,1);std::vector<Deposit> deposits;
  for(int y=-25;y<=25;++y)for(int x=-25;x<=25;++x){Point3 p(x*.01,y*.01,0);RISEPel power(1);global.Store(power,p,ng,incoming);caustic.Store(power,p,incoming);globalNM.Store(1,550,p,incoming);causticNM.Store(1,550,p,incoming);deposits.push_back({p,power});}
  const Vector3 wi=global.FirstDirection();global.Balance();global.RawParams();caustic.Balance();caustic.SetGatherParams(.2,.05,10,400,nullptr);globalNM.Balance();globalNM.SetGatherParamsNM(.2,.05,10,400,1,nullptr);causticNM.Balance();causticNM.SetGatherParamsNM(.2,.05,10,400,1,nullptr);
  // The -80 degree light/view with +60 degree Ns puts both directions
  // below Ns but above Ng. Lambertian's view-facing support admits it.
  const double view=incidence==-80?-80*PI/180:0;
  RayIntersectionGeometric q(Ray(Point3(0,0,1),Vector3(-std::sin(view),0,-std::cos(view))),nullRasterizerState);q.ptIntersection=Point3(0,0,0);q.vGeomNormal=ng;
  RISEPel spectralFlat[2];
  for(double tilt:{0.,30.,60.}){
   const double t=tilt*PI/180;q.vNormal=Vector3(std::sin(t),0,std::cos(t));q.onb.CreateFromW(q.vNormal);
   const RISEPel expected=Reference(deposits,q.ptIntersection,ng,wi,q,*bsdf,true,true);RISEPel g,c;global.RadianceEstimate(g,q,*bsdf);caustic.RadianceEstimate(c,q,*bsdf);
   std::printf("DIRECT incidence=%.17g view=%.17g tilt=%.17g encodedWi=(%.17g,%.17g,%.17g)\n",incidence,view*180/PI,tilt,wi.x,wi.y,wi.z);
   for(int channel=0;channel<3;++channel){Near(g[channel],expected[channel],"DL239 raw global Pel area response");Near(c[channel],expected[channel],"DL239 caustic Pel area response");}
   // Spectral maps retain their own uniform spatial kernel. Price NM
   // directly, then independently check their spectral-to-Pel angular ratio.
   const RISEPel uniform=Reference(deposits,q.ptIntersection,ng,wi,q,*bsdf,true);
   const double f=bsdf->value(wi,q).r;const double fNM=bsdf->valueNM(wi,q,550);const double expectedNM=f>0?uniform.r*fNM/f:0;
   double values[2];globalNM.RadianceEstimateNM(550,values[0],q,*bsdf);causticNM.RadianceEstimateNM(550,values[1],q,*bsdf);for(double v:values)Near(v,expectedNM,"DL239 spectral gather NM area response");
   RISEPel spectra[2];globalNM.RadianceEstimate(spectra[0],q,*bsdf);causticNM.RadianceEstimate(spectra[1],q,*bsdf);
   if(tilt==0){spectralFlat[0]=spectra[0];spectralFlat[1]=spectra[1];}
   const double ratio=f>0?std::fabs(Vector3Ops::Dot(q.vNormal,wi)/Vector3Ops::Dot(ng,wi)):0;
   for(int mode=0;mode<2;++mode)for(int channel=0;channel<3;++channel)Near(spectra[mode][channel],spectralFlat[mode][channel]*ratio,"DL239 spectral gather Pel area response");
  }
 }
 bsdf->release();paint->release();
}

void Run(){
 auto* paint=new UniformColorPainter(RISEPel(.8,.5,.2));auto* black=new UniformColorPainter(RISEPel(0.0));auto* exp=new UniformScalarPainter(12);auto* lambert=new LambertianBRDF(*paint);auto* phong=new IsotropicPhongBRDF(*black,*paint,*exp);
 const Point3 anchor(0,0,0);const Vector3 areaNormal(0,0,1);
 RayIntersectionGeometric query(Ray(Point3(0,0,1),Vector3(0,0,-1)),nullRasterizerState);query.ptIntersection=anchor;query.vGeomNormal=areaNormal;
 for(double incident:{-45.,0.,45.}){
  CacheProbe map(2601);std::vector<Deposit> deposits;const double a=incident*PI/180;const Vector3 wi(std::sin(a),0,std::cos(a));
  for(int y=-25;y<=25;++y)for(int x=-25;x<=25;++x){Point3 p(x*.01,y*.01,0);RISEPel power(1);map.Store(power,p,areaNormal,wi);deposits.push_back({p,power});}
  // Price the encoded direction actually stored, so angular quantization is
  // explicit and cannot masquerade as a transport-factor error.
  const Vector3 decoded=map.FirstDirection();map.Balance();map.RawParams();
  const auto actualDistances=map.Distances(anchor);std::vector<double> expectedDistances;for(const auto& d:deposits){double d2=Vector3Ops::SquaredModulus(Vector3Ops::mkVector3(d.p,anchor));if(d2<.04)expectedDistances.push_back(d2);}std::sort(expectedDistances.begin(),expectedDistances.end());expectedDistances.resize(400);
  bool same=actualDistances.size()==expectedDistances.size();double maxError=0;for(unsigned i=0;i<std::min(actualDistances.size(),expectedDistances.size());++i)maxError=std::max(maxError,std::fabs(actualDistances[i]-expectedDistances[i]));same=same&&maxError<1e-15;
  std::printf("KNN n=%zu r2=%.17g expectedN=%zu expectedR2=%.17g maxDistanceError=%.17g\n",actualDistances.size(),actualDistances.back(),expectedDistances.size(),expectedDistances.back(),maxError);Check(same,"actual photon KD distances match independent exhaustive search",maxError,0);
  map.PrecomputeIrradiance(1,nullptr);
  if(map.HasAnchor(anchor,areaNormal)){Point3 found=map.NearestPosition(anchor,areaNormal);std::printf("ANCHOR chosen=(%.17g,%.17g,%.17g) expected=(0,0,0)\n",found.x,found.y,found.z);Near(Vector3Ops::SquaredModulus(Vector3Ops::mkVector3(found,anchor)),0,"nearest cache anchor at exact stored query point");}
  std::printf("CACHE incident=%.17g decoded=(%.17g,%.17g,%.17g)\n",incident,decoded.x,decoded.y,decoded.z);
  Check(map.HasAnchor(anchor,areaNormal),"cache anchor is chosen by surface normal",map.HasAnchor(anchor,areaNormal),1);
  for(double tilt:{0.,-10.})for(const IBSDF* bsdf:{static_cast<IBSDF*>(lambert),static_cast<IBSDF*>(phong)}){
   const double t=tilt*PI/180;query.vNormal=Vector3(std::sin(t),0,std::cos(t));query.onb.CreateFromW(query.vNormal);
   RISEPel got;map.RadianceEstimate(got,query,*bsdf);const RISEPel expected=Reference(deposits,anchor,areaNormal,decoded,query,*bsdf,true);
   std::printf("CACHE tilt=%.17g material=%s\n",tilt,bsdf==lambert?"Lambertian":"Phong");for(int c=0;c<3;++c)Near(got[c],expected[c],"DL239 cached directional query response");
  }
 }
 phong->release();lambert->release();exp->release();black->release();paint->release();
}
}
int main(){Run();DirectGathers();std::printf("PhotonDirectionalCacheTest checks=%d failures=%d\n",checks,failures);return failures?1:0;}
