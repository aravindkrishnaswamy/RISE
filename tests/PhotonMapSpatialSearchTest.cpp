// DL279: inclusive KD partition bounds and tangent-plane traversal.
// Exact finite record sets are checked against an independent linear search.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include "../src/Library/PhotonMapping/PhotonMap.h"
using namespace RISE;using namespace RISE::Implementation;
namespace {
int checks=0,failures=0;
void Check(bool ok,const char* text){++checks;if(!ok){++failures;std::printf("FAIL %s\n",text);}}
template<class P> class Tree:public PhotonMapCore<P>{
public:
 using PhotonMapCore<P>::CountPhotonsAt;
 Tree():PhotonMapCore<P>(0,nullptr){}
 void Serialize(IWriteBuffer&)const override{} void Deserialize(IReadBuffer&)override{}
 void RadianceEstimate(RISEPel&,const RayIntersectionGeometric&,const IBSDF&)const override{}
 void Load(const std::vector<Point3>& points){this->vphotons.clear();this->bbox=BoundingBox(Point3(RISE_INFINITY,RISE_INFINITY,RISE_INFINITY),Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY));for(const auto& p:points){P v;v.ptPosition=p;this->vphotons.push_back(v);this->bbox.Include(p);}this->Balance();}
 std::vector<Point3> All(const Point3& q,double r2)const{typename PhotonMapCore<P>::PhotonListType p;this->LocateAllPhotons(q,r2,p,0,static_cast<int>(this->vphotons.size())-1);std::vector<Point3> out;for(const auto& a:p)out.push_back(a.ptPosition);return out;}
 std::vector<double> Nearest(const Point3& q,double r2,unsigned k)const{typename PhotonMapCore<P>::PhotonDistListType p;this->LocatePhotons(q,r2,k,p,0,static_cast<int>(this->vphotons.size())-1);std::vector<double> out;for(const auto& a:p)out.push_back(a.distance);std::sort(out.begin(),out.end());return out;}
 void Tangent(int axis){std::vector<Point3> p;for(int x=-1;x<=1;++x){Point3 q(0,0,0);q[axis]=x;p.push_back(q);}Load(p);for(auto& x:this->vphotons)x.plane=axis;}
};
bool Less(const Point3&a,const Point3&b){if(a.x!=b.x)return a.x<b.x;if(a.y!=b.y)return a.y<b.y;return a.z<b.z;}
bool Equal(std::vector<Point3> a,std::vector<Point3> b){std::sort(a.begin(),a.end(),Less);std::sort(b.begin(),b.end(),Less);if(a.size()!=b.size())return false;for(unsigned i=0;i<a.size();++i)if(a[i].x!=b[i].x||a[i].y!=b[i].y||a[i].z!=b[i].z)return false;return true;}
template<class P> void Run(const char* family){
 std::printf("FAMILY %s\n",family);Tree<P> tree;
 for(unsigned n:{0u,1u,2u,3u,17u,127u}){
  std::vector<Point3> points;for(unsigned i=0;i<n;++i){unsigned j=(i*53)%std::max(n,1u);points.emplace_back(double(j)/16,double((j*7)%23)/16,double((j*11)%31)/16);}tree.Load(points);
  for(unsigned t=0;t<13;++t){Point3 q(double(t)/7,double(t%4)/3,double(t%5)/4);double r2=(t+1)*.125;std::vector<Point3> expected;std::vector<double> distances;for(const auto& p:points){double d=Vector3Ops::SquaredModulus(Vector3Ops::mkVector3(p,q));if(d<r2){expected.push_back(p);distances.push_back(d);}}
   Check(Equal(tree.All(q,r2),expected),"all-in-radius record identities");unsigned count=0;tree.CountPhotonsAt(q,r2,1000,count);Check(count==expected.size(),"in-radius count");count=0;tree.CountPhotonsAt(q,r2,3,count);Check(count==std::min<size_t>(3,expected.size()),"capped count");std::sort(distances.begin(),distances.end());
   for(unsigned k:{1u,2u,7u,150u}){auto limited=distances;if(limited.size()>k)limited.resize(k);Check(tree.Nearest(q,r2,k)==limited,"nearest-k squared distances");}
  }
 }
 for(int axis=0;axis<3;++axis){tree.Tangent(axis);for(double side:{-1.,1.}){Point3 q(0,0,0);q[axis]=side;for(double r2:{std::nextafter(1.,0.),1.,std::nextafter(1.,2.)}){const unsigned expected=r2>1?2:1;unsigned count=0;tree.CountPhotonsAt(q,r2,3,count);Check(count==expected,"tangent split count with boundary controls");Check(tree.All(q,r2).size()==expected,"tangent split radius record set");const auto d=tree.Nearest(q,r2,1);Check(d.size()==1&&d[0]==0,"tangent split nearest child");}}}
}
}
int main(){Run<Photon>("Pel");Run<IrradPhoton>("cached Pel");Run<SpectralPhoton>("spectral");Run<TranslucentPhoton>("translucent");Run<ShadowPhoton>("shadow");std::printf("PhotonMapSpatialSearchTest checks=%d failures=%d\n",checks,failures);return failures?1:0;}
