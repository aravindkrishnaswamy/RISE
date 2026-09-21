// DL239: fixed directional deposits distinguish cache-area geometry from
// query BSDF/shading frame. No rasterizer or stochastic acceptance bands.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include "../src/Library/PhotonMapping/GlobalPelPhotonMap.h"
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
 bool HasAnchor(const Point3& p,const Vector3& n)const{IrradPhoton dummy;distance_container<IrradPhoton> nearest(dummy,RISE_INFINITY);LocateNearestPhoton(p,n,dGatherRadius,nearest);return nearest.distance<RISE_INFINITY;}
};
// Independent finite-kernel oracle: sort all deposits by Euclidean distance,
// retain400, use the legacy uniform disk with geometric anchor area. Query
// material evaluation remains at query (including its shading/view frame).
struct Deposit {Point3 p;RISEPel power;};
RISEPel Reference(const std::vector<Deposit>& deposits,const Point3& anchor,const Vector3& areaNormal,const Vector3& wi,const RayIntersectionGeometric& query,const IBSDF& brdf,bool angular){
 std::vector<std::pair<double,const Deposit*> > sorted;
 for(const auto& p:deposits){const Vector3 d=Vector3Ops::mkVector3(p.p,anchor);const double d2=Vector3Ops::SquaredModulus(d);if(d2<.04)sorted.emplace_back(d2,&p);}
 std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.first<b.first;});if(sorted.size()>400)sorted.resize(400);
 RISEPel sum(0.0);if(sorted.size()<=10)return sum;const double r2=sorted.back().first;
 for(const auto& p:sorted){const double height=Vector3Ops::Dot(Vector3Ops::mkVector3(p.second->p,anchor),areaNormal);if(std::fabs(height)<r2*.05&&Vector3Ops::Dot(wi,areaNormal)>0)sum=sum+p.second->power;}
 double factor=angular?std::fabs(Vector3Ops::Dot(query.vNormal,wi)/Vector3Ops::Dot(areaNormal,wi)):1;
 return sum*brdf.value(wi,query)*(factor/(PI*r2));
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
  const Vector3 decoded=map.FirstDirection();map.Balance();map.RawParams();map.PrecomputeIrradiance(1,nullptr);
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
int main(){Run();std::printf("PhotonDirectionalCacheTest checks=%d failures=%d\n",checks,failures);return failures?1:0;}
