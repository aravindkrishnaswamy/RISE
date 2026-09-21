// DL239: fixed directional deposits distinguish cache-area geometry from
// query BSDF/shading frame. No rasterizer or stochastic acceptance bands.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include <chrono>
#include <filesystem>
#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/Utilities/Color/ColorUtils.h"
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
 size_t AnchorCount()const{return anchors.size();}
 std::vector<std::pair<Point3,Vector3>> AnchorSnapshot()const{std::vector<std::pair<Point3,Vector3>> out;for(const auto& a:anchors)out.emplace_back(a.position,a.geometricNormal);return out;}
 void RawParams(){PhotonMapCore<IrradPhoton>::SetGatherParams(.2,.05,10,400,nullptr);}
 Vector3 FirstDirection()const{return PhotonDir(vphotons[0].theta,vphotons[0].phi);}
 Vector3 LastDirection()const{return PhotonDir(vphotons.back().theta,vphotons.back().phi);}
 std::vector<double> Distances(const Point3& p)const{PhotonDistListType heap;LocatePhotons(p,dGatherRadius,nMaxPhotonsOnGather,heap,0,static_cast<int>(vphotons.size())-1);std::vector<double> d;for(const auto& h:heap)d.push_back(h.distance);std::sort(d.begin(),d.end());return d;}
 Point3 NearestPosition(const Point3& p,const Vector3& n)const{return FindAnchor(p,n)->position;}
 bool HasAnchor(const Point3& p,const Vector3& n)const{return FindAnchor(p,n)!=nullptr;}
};
// Independent finite-kernel oracle: sort all deposits by Euclidean distance,
// retain400, use the legacy uniform disk with geometric anchor area. Query
// material evaluation remains at query (including its shading/view frame).
struct Deposit {Point3 p;RISEPel power;};
RISEPel Reference(const std::vector<Deposit>& deposits,const Point3& anchor,const Vector3& areaNormal,const Vector3& wi,const RayIntersectionGeometric& query,const IBSDF& brdf,bool angular,bool gaussian=false,unsigned limit=400,double searchRadius2=.04){
 std::vector<std::pair<double,const Deposit*> > sorted;
 for(const auto& p:deposits){const Vector3 d=Vector3Ops::mkVector3(p.p,anchor);const double d2=Vector3Ops::SquaredModulus(d);if(d2<searchRadius2)sorted.emplace_back(d2,&p);}
 std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.first<b.first;});if(sorted.size()>limit)sorted.resize(limit);
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

class PositionPainter:public UniformColorPainter {
public:
 PositionPainter():UniformColorPainter(RISEPel(.5)){}
 RISEPel GetColor(const RayIntersectionGeometric& q)const override{return RISEPel(.6+.2*sin(100*q.ptIntersection.x),.4+.2*sin(100*q.ptIntersection.y),.3+.1*cos(100*q.ptIntersection.z));}
};
void ObliqueAnchorQuery(){
 const double angle=PI/6;const Vector3 normal(sin(angle),0,cos(angle));const Vector3 incident(0,0,1);CacheProbe map(2601);std::vector<Deposit> deposits;
 for(int y=-25;y<=25;++y)for(int x=-25;x<=25;++x){const Point3 p(x*.01*cos(angle),y*.01,-x*.01*sin(angle));map.Store(RISEPel(1.),p,normal,incident);deposits.push_back({p,RISEPel(1.)});}
 const Vector3 decoded=map.FirstDirection();map.Balance();map.RawParams();map.PrecomputeIrradiance(4,nullptr);
 RayIntersectionGeometric query(Ray(Point3(0,0,1),Vector3(0,0,-1)),nullRasterizerState);query.ptIntersection=Point3(.0007,.0002,0);query.vGeomNormal=Vector3(sin(angle+.1),0,cos(angle+.1));query.vNormal=Vector3(sin(angle-.2),0,cos(angle-.2));query.onb.CreateFromW(query.vNormal);
 Point3 expectedAnchor(0,0,0);Vector3 expectedNormal(0,0,1);double best=.04;
 for(const auto& a:map.AnchorSnapshot()){const double d2=Vector3Ops::SquaredModulus(Vector3Ops::mkVector3(a.first,query.ptIntersection));if(d2<best&&Vector3Ops::Dot(a.second,query.vGeomNormal)>.9){best=d2;expectedAnchor=a.first;expectedNormal=a.second;}}
 Check(map.HasAnchor(query.ptIntersection,query.vGeomNormal),"oblique query has compatible geometric anchor",map.HasAnchor(query.ptIntersection,query.vGeomNormal),1);
 if(map.HasAnchor(query.ptIntersection,query.vGeomNormal))Near(Vector3Ops::SquaredModulus(Vector3Ops::mkVector3(map.NearestPosition(query.ptIntersection,query.vGeomNormal),expectedAnchor)),0,"anchor KD lookup equals exhaustive normal-filtered nearest search");
 auto* painter=new PositionPainter();auto* black=new UniformColorPainter(RISEPel(0.));auto* exponent=new UniformScalarPainter(12.);auto* lambert=new LambertianBRDF(*painter);auto* phong=new IsotropicPhongBRDF(*black,*painter,*exponent);
 for(const IBSDF* f:{static_cast<IBSDF*>(lambert),static_cast<IBSDF*>(phong)}){RISEPel got;map.RadianceEstimate(got,query,*f);const RISEPel expected=Reference(deposits,expectedAnchor,expectedNormal,decoded,query,*f,true);for(int c=0;c<3;++c)Near(got[c],expected[c],"anchor geometric area and query position/material/shading frame stay separate");}
 // Reversing only the query geometric side makes every anchor incompatible;
 // it must not choose one using incident direction or shading normal.
 Check(!map.HasAnchor(query.ptIntersection,-query.vGeomNormal),"opposite geometric side rejects cache anchors",map.HasAnchor(query.ptIntersection,-query.vGeomNormal),0);
 phong->release();lambert->release();exponent->release();black->release();painter->release();
}

// A spatially varying angular/color field cannot be represented by one scalar
// irradiance or one representative direction, even at a fixed anchor.
void MixedFieldLifecycle(){
 struct Packet {Point3 p; Vector3 wi; RISEPel power;};
 CacheProbe map(2602);std::vector<Packet> packets;
 const Vector3 ng(0,0,1);
 for(int y=-25;y<=25;++y)for(int x=-25;x<=25;++x){
  const Point3 p(x*.01+std::sin(double(x*y))*.0001,y*.01+std::sin(double(x+2*y))*.0001,0);
  const double angle=((x-y+102)%3-1)*PI/3;
  const Vector3 wi(std::sin(angle),0,std::cos(angle));
  const RISEPel power(.2+.01*(x+25),.3+.01*(y+25),.9-.01*(x+25));
  map.Store(power,p,ng,wi);packets.push_back({p,map.LastDirection(),power});
 }
 map.Balance();map.RawParams();map.PrecomputeIrradiance(1,nullptr);
 auto* painter=new PositionPainter();auto* black=new UniformColorPainter(RISEPel(0.));
 auto* exponent=new UniformScalarPainter(12.);auto* lambert=new LambertianBRDF(*painter);
 auto* phong=new IsotropicPhongBRDF(*black,*painter,*exponent);
 RayIntersectionGeometric q(Ray(Point3(0,0,1),Vector3(0,0,-1)),nullRasterizerState);
 q.ptIntersection=Point3(0,0,0);q.vGeomNormal=ng;
 for(double tilt:{-.3,.4})for(const IBSDF* f:{static_cast<IBSDF*>(lambert),static_cast<IBSDF*>(phong)}){
  q.vNormal=Vector3(std::sin(tilt),0,std::cos(tilt));q.onb.CreateFromW(q.vNormal);
  std::vector<std::pair<double,const Packet*>> sorted;
  for(const auto& p:packets){const double d2=Vector3Ops::SquaredModulus(Vector3Ops::mkVector3(p.p,q.ptIntersection));if(d2<.04)sorted.emplace_back(d2,&p);}
  std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.first<b.first;});sorted.resize(400);
  const double r2=sorted.back().first;RISEPel expected(0.);
  for(const auto& p:sorted){const double area=std::fabs(Vector3Ops::Dot(ng,p.second->wi));const double shading=std::fabs(Vector3Ops::Dot(q.vNormal,p.second->wi));expected=expected+p.second->power*f->value(p.second->wi,q)*(shading/area);}
  expected=expected/(PI*r2);RISEPel got;map.RadianceEstimate(got,q,*f);
  for(int c=0;c<3;++c)Near(got[c],expected[c],"mixed incident directions/colors use each packet at the query material");
 }
 const bool stored=map.Store(RISEPel(1.),Point3(2,2,0),ng,ng);
 Check(stored,"post-cache packet insertion succeeds",stored,1);
 Check(map.AnchorCount()==0,"successful insertion invalidates existing anchors",map.AnchorCount(),0);
 map.Balance();map.PrecomputeIrradiance(1,nullptr);
 Check(map.AnchorCount()==2602,"rebuild includes the newly inserted packet",map.AnchorCount(),2602);
 const bool overflow=map.Store(RISEPel(1.),Point3(3,3,0),ng,ng);
 Check(!overflow,"capacity rejection reports false",overflow,0);
 Check(map.AnchorCount()==2602,"rejected insertion preserves valid anchors",map.AnchorCount(),2602);
 phong->release();lambert->release();exponent->release();black->release();painter->release();
}

void LegacyCacheLoad(){
 // Write the old byte format independently of GlobalPelPhotonMap::Serialize.
 // A scalar-precomputed record cannot restore the discarded angular field.
 auto* job=new Job();auto* installed=new GlobalPelPhotonMap(1,nullptr);
 installed->Store(RISEPel(1.),Point3(0,0,0),Vector3(0,0,1),Vector3(0,0,1));
 job->GetScene()->SetGlobalPelMap(installed);
 auto* buffer=new MemoryBuffer(512);
 buffer->setUInt(1);buffer->setUInt(0);buffer->setDouble(.04);buffer->setDouble(.05);buffer->setUInt(0);buffer->setUInt(1);buffer->setDouble(1);buffer->setUChar(1);
 BoundingBox(Point3(-1,-1,-1),Point3(1,1,1)).Serialize(*buffer);buffer->setUInt(1);
 Point3Ops::Serialize(Point3(0,0,0),*buffer);buffer->setUChar(0);ColorUtils::SerializeRGBPel(RISEPel(1.),*buffer);buffer->setUChar(0);buffer->setUChar(0);ColorUtils::SerializeRGBPel(RISEPel(1.),*buffer);buffer->setUChar(0);buffer->setUChar(0);
 const auto path=std::filesystem::temp_directory_path()/("rise_dl239_cache_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".pmap");
 const bool written=buffer->DumpToFileToCursor(path.string().c_str());Check(written,"legacy scalar fixture written",written,1);
 const bool loaded=written&&job->LoadGlobalPelPhotonmap(path.string().c_str());Check(!loaded,"legacy scalar cache reports unsupported directional reconstruction",loaded,0);Check(job->GetScene()->GetGlobalPelMap()==installed,"failed cache load retains installed valid map",job->GetScene()->GetGlobalPelMap()==installed,1);
 std::filesystem::remove(path);
 const bool missing=job->LoadGlobalPelPhotonmap(path.string().c_str());Check(!missing,"missing cache file reports failure",missing,0);Check(job->GetScene()->GetGlobalPelMap()==installed,"missing cache load retains installed valid map",job->GetScene()->GetGlobalPelMap()==installed,1);
 // The old raw flag0 is recoverable as directional packets, but its
 // compressed normal must never be promoted from Ns provenance to Ng.
 const unsigned used=buffer->getCurPos();buffer->seek(IBuffer::START,40);buffer->setUChar(0);buffer->seek(IBuffer::START,used);
 Check(buffer->DumpToFileToCursor(path.string().c_str()),"legacy raw fixture written",1,1);
 const bool rawLoaded=job->LoadGlobalPelPhotonmap(path.string().c_str());Check(rawLoaded,"legacy full-direction map still loads",rawLoaded,1);
 if(rawLoaded){
  auto* loaded=job->GetScene()->GetGlobalPelMapMutable();loaded->SetGatherParams(.2,.05,0,1,nullptr);
  auto* paint=new UniformColorPainter(RISEPel(.8));auto* bsdf=new LambertianBRDF(*paint);
  RayIntersectionGeometric q(Ray(Point3(0,0,1),Vector3(0,0,-1)),nullRasterizerState);q.ptIntersection=Point3(.1,0,0);q.vGeomNormal=Vector3(0,0,1);q.vNormal=Vector3(.5,0,sqrt(.75));q.onb.CreateFromW(q.vNormal);
  RISEPel got;loaded->RadianceEstimate(got,q,*bsdf);const double alpha=.918,beta=1.953,D=1-exp(-beta),norm=2*alpha*(.5*(1-1/D)+(1-exp(-beta*.5))/(beta*D));const double weight=alpha*(1-(1-exp(-beta*.5))/D);const double expected=.8*sqrt(.75)*weight/(PI*PI*.01*norm);
  for(int c=0;c<3;++c)Near(got[c],expected,"legacy raw load stays on directional direct gather after parameter update");
  bsdf->release();paint->release();
 }
 // Historical Store allowed one packet beyond its nominal maximum. These
 // legacy raw packets still carry all directions and must survive loading.
 buffer->seek(IBuffer::START,0);buffer->setUInt(0);buffer->seek(IBuffer::START,used);
 Check(buffer->DumpToFileToCursor(path.string().c_str()),"legacy one-over-capacity fixture written",1,1);
 const bool overfull=job->LoadGlobalPelPhotonmap(path.string().c_str());
 Check(overfull,"recoverable legacy one-over-capacity map loads",overfull,1);
 if(overfull){
  auto* current=job->GetScene()->GetGlobalPelMapMutable();
  Check(current->NumStored()==1&&current->MaxPhotons()==0,"legacy record/count metadata preserved",current->NumStored(),1);
  auto* roundtrip=new MemoryBuffer();current->Serialize(*roundtrip);roundtrip->seek(IBuffer::START,0);
  CacheProbe restored(0);const bool reload=restored.DeserializeChecked(*roundtrip);
  Check(reload&&restored.NumStored()==1&&restored.MaxPhotons()==0,"converted directional format retains legacy overflow provenance",restored.NumStored(),1);
  roundtrip->release();
 }
 // Two extra packets cannot be produced by the old Store contract. Supply
 // all bytes so rejection cannot be attributed to a truncated record list.
 const std::vector<char> packet(buffer->Pointer()+93,buffer->Pointer()+used);
 buffer->ResizeForMore(static_cast<unsigned>(packet.size()));buffer->setBytes(packet.data(),static_cast<unsigned>(packet.size()));
 const unsigned twoUsed=buffer->getCurPos();buffer->seek(IBuffer::START,89);buffer->setUInt(2);buffer->seek(IBuffer::START,twoUsed);
 Check(buffer->DumpToFileToCursor(path.string().c_str()),"two-over-capacity fixture written",1,1);
 const IPhotonMap* beforeInvalid=job->GetScene()->GetGlobalPelMap();
 const bool invalid=job->LoadGlobalPelPhotonmap(path.string().c_str());
 Check(!invalid&&job->GetScene()->GetGlobalPelMap()==beforeInvalid,"unproducible legacy count rejected without replacement",invalid,0);
 // Current geometric-normal records have no historical capacity exception.
 CacheProbe currentFormat(1);currentFormat.Store(RISEPel(1.),Point3(0,0,0),Vector3(0,0,1),Vector3(0,0,1));
 auto* modern=new MemoryBuffer();currentFormat.Serialize(*modern);modern->seek(IBuffer::START,0);modern->setUInt(0);modern->seek(IBuffer::START,0);
 CacheProbe untouched(2);untouched.Store(RISEPel(1.),Point3(1,0,0),Vector3(0,0,1),Vector3(0,0,1));
 const bool modernLoaded=untouched.DeserializeChecked(*modern);
 Check(!modernLoaded&&untouched.NumStored()==1,"modern over-capacity records rejected transactionally",modernLoaded,0);
 modern->release();
 std::filesystem::remove(path);
 buffer->release();installed->release();job->release();
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
  auto* serialized=new MemoryBuffer();map.Serialize(*serialized);const unsigned bytes=serialized->getCurPos();serialized->seek(IBuffer::START,0);
  CacheProbe restored(0);restored.Deserialize(*serialized);
  Check(restored.NumStored()==2601,"directional roundtrip retains every incident packet",restored.NumStored(),2601);
  Check(restored.AnchorCount()==2601,"directional roundtrip restores explicit anchor spacing",restored.AnchorCount(),2601);
  RISEPel roundtrip;restored.RadianceEstimate(roundtrip,query,*phong);const RISEPel expected=Reference(deposits,anchor,areaNormal,decoded,query,*phong,true);
  for(int c=0;c<3;++c)Near(roundtrip[c],expected[c],"directional serialized response");
  restored.ScalePhotonPower(.25);restored.RadianceEstimate(roundtrip,query,*phong);
  for(int c=0;c<3;++c)Near(roundtrip[c],expected[c]*.25,"anchor gather reads newly scaled live packet powers");
  auto* truncated=new MemoryBuffer(serialized->Pointer(),bytes-1,false);restored.Deserialize(*truncated);restored.RadianceEstimate(roundtrip,query,*phong);
  for(int c=0;c<3;++c)Near(roundtrip[c],expected[c]*.25,"truncated load leaves prior map response intact");
  restored.PrecomputeIrradiance(4,nullptr);Check(restored.NumStored()==2601,"quarter anchor preparation preserves full packet field",restored.NumStored(),2601);Check(restored.AnchorCount()==651,"quarter anchor population",restored.AnchorCount(),651);
  // Rebuild at every packet to make the exact chosen anchor independent of
  // tie ordering, then change the finite spatial gather itself.
  restored.PrecomputeIrradiance(1,nullptr);restored.SetGatherParams(.05,.05,10,20,nullptr);restored.RadianceEstimate(roundtrip,query,*phong);
  const RISEPel changed=Reference(deposits,anchor,areaNormal,decoded,query,*phong,true,false,20,.0025)*.25;
  for(int c=0;c<3;++c)Near(roundtrip[c],changed[c],"changed gather parameters immediately alter anchored kernel");
  truncated->release();serialized->release();
 }
 phong->release();lambert->release();exp->release();black->release();paint->release();
}
}
int main(){Run();DirectGathers();ObliqueAnchorQuery();MixedFieldLifecycle();LegacyCacheLoad();std::printf("PhotonDirectionalCacheTest checks=%d failures=%d\n",checks,failures);return failures?1:0;}
