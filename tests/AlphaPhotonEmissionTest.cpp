// Actual legacy emission loops, with an ideal deposit sink to isolate accounting.
#include <cstdio>
#include <cmath>
#include <type_traits>
#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/PhotonMapping/GlobalPelPhotonTracer.h"
#include "../src/Library/PhotonMapping/GlobalSpectralPhotonTracer.h"
#include "../src/Library/PhotonMapping/CausticPelPhotonMap.h"
#include "../src/Library/PhotonMapping/TranslucentPelPhotonMap.h"
#include "../src/Library/PhotonMapping/ShadowPhotonMap.h"
#include "../src/Library/PhotonMapping/CausticSpectralPhotonMap.h"
#include "../src/Library/Materials/LambertianLuminaireMaterial.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Managers/LightManager.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Lights/PointLight.h"
#include "../src/Library/Cameras/PinholeCamera.h"
#include "../src/Library/Managers/CameraManager.h"
#include "../src/Library/Scene.h"
using namespace RISE;using namespace RISE::Implementation;
static int passed=0,failed=0;
static void Check(bool ok,const char* label,double value=0,double expected=0){ok?++passed:++failed;std::printf("%s %s value=%g expected=%g\n",ok?"PASS":"FAIL",label,value,expected);}
static double Power(const RISEPel& p){return p.r;}static double Power(double p){return p;}
template<class Base>class ReadMap:public Base {
public:ReadMap(unsigned n,const IPhotonTracer* t):Base(n,t){}
 bool ReservoirActive()const{return this->reservoirEnabled;}
 bool HasLate()const{for(const auto& p:this->vphotons)if(p.ptPosition.x>=400)return true;return false;}
 bool LabelsCorrect()const{for(const auto& p:this->vphotons)if(p.shadow!=(static_cast<unsigned>(p.ptPosition.x)%2!=0))return false;return true;}
 double MaximumPower()const{return this->maxPower;}
 double PositionSum()const{double sum=0;for(const auto& p:this->vphotons)sum+=p.ptPosition.x;return sum;}
 double Side(bool right)const{double sum=0;for(const auto& p:this->vphotons)if((p.ptPosition.x>0)==right)sum+=Power(p.power);return sum;}
};
static void Deposit(GlobalPelPhotonMap& m,const RISEPel& p,const Point3& x){m.Store(p,x,Vector3(0,0,1),Vector3(0,0,1));}
static void Deposit(CausticPelPhotonMap& m,const RISEPel& p,const Point3& x){m.Store(p,x,Vector3(0,0,1));}
static void Deposit(TranslucentPelPhotonMap& m,const RISEPel& p,const Point3& x){m.Store(p,x,Vector3(0,0,1),false);}
struct Result{double left=0,right=0;unsigned stored=0;unsigned calls=0;bool published=false;bool reservoirClosed=false;};
template<class Base>class RGBTracer:public PhotonTracer<ReadMap<Base>> {
 unsigned copies;bool efficiency;
 void TraceSinglePhoton(const Ray& r,const RISEPel& p,ReadMap<Base>& m,const IORStack&)const override{
  ++result.calls;if(efficiency&&r.origin.x<0&&r.origin.y<0)return;
  for(unsigned i=0;i<copies;++i)Deposit(m,p,r.origin);
 }
 void SetSpecificPhotonMapForScene(ReadMap<Base>* m)const override{result.left=m->Side(false);result.right=m->Side(true);result.stored=m->NumStored();result.published=true;result.reservoirClosed=!m->ReservoirActive();}
public:mutable Result result;
 RGBTracer(unsigned c=1,bool e=false,unsigned t=1,bool nonmesh=false):PhotonTracer<ReadMap<Base>>(nonmesh,1,t,true),copies(c),efficiency(e){}
};
template<class Base>class NMTracer:public SpectralPhotonTracer<ReadMap<Base>> {
 unsigned copies;bool efficiency;
 void TraceSinglePhoton(const Ray& r,Scalar p,Scalar nm,ReadMap<Base>& m,const IORStack&)const override{
  ++result.calls;if(efficiency&&r.origin.x<0&&r.origin.y<0)return;
  for(unsigned i=0;i<copies;++i)m.Store(p,nm,r.origin,Vector3(0,0,1));
 }
 void SetSpecificPhotonMapForScene(ReadMap<Base>* m)const override{result.left=m->Side(false);result.right=m->Side(true);result.stored=m->NumStored();result.published=true;result.reservoirClosed=!m->ReservoirActive();}
public:mutable Result result;
 NMTracer(unsigned c=1,bool e=false,unsigned t=1):SpectralPhotonTracer<ReadMap<Base>>(550,552,1,1,t,true),copies(c),efficiency(e){}
};
class SpatialAlpha:public IScalarPainter,public Reference {
public:ScalarTriple GetValuesAt(const RayIntersectionGeometric& r)const override{return ScalarTriple(r.ptObjIntersec.z>0?.75:.25);}
};
struct Fixture {
 Scene* scene=new Scene;ObjectManager* objects=new ObjectManager(false,false,4,8);LightManager* lights=new LightManager;
 UniformColorPainter* white=new UniformColorPainter(RISEPel(1));LambertianMaterial* base=new LambertianMaterial(*white);
 LambertianLuminaireMaterial* left=new LambertianLuminaireMaterial(*white,1,*base);LambertianLuminaireMaterial* right=new LambertianLuminaireMaterial(*white,3,*base);
 Fixture(bool single=false,bool point=false){scene->SetObjectManager(objects);scene->SetLightManager(lights);
  auto* cameras=new CameraManager;scene->SetCameraManager(cameras);cameras->release();
  auto* camera=new PinholeCamera(Point3(0,0,10),Point3(0,0,0),Vector3(0,1,0),.5,1,1,1,1,0,0,Vector3(0,0,0),Vector2(0,0));scene->AddCamera("camera",camera);scene->SetActiveCamera("camera");camera->release();
  auto* geo=new SphereGeometry(1);
  for(int i=0;i<(single?1:2);++i){if(point&&i==1)continue;auto* o=new Object(geo);o->AssignMaterial(i?*right:*left);o->SetPosition(Point3(i?10:-10,0,0));o->FinalizeTransformations();objects->AddItem(o,i?"right":"left");o->release();}geo->release();
  if(point){auto* l=new PointLight(3,RISEPel(1),true);l->SetPosition(Point3(10,0,0));l->FinalizeTransformations();l->SetCanGeneratePhotons(true);lights->AddItem(l,"point");l->release();}
 }
 ~Fixture(){scene->release();objects->release();lights->release();left->release();right->release();base->release();white->release();}
};
class ProgressCounter:public IProgressCallback {public:double last=0;unsigned calls=0;bool cancel=false;bool Progress(double n,double)override{last=n;++calls;return !cancel||calls<4;}void SetTitle(const char*)override{}};
// RGB and narrow-band NM use the same source-mixture estimator. The latter's
// expected spectral exitance is queried at its sole wavelength (550nm).
template<class Tracer>void Cases(bool nm){
 for(int mode=0;mode<4;++mode){Fixture f;auto* alpha=new UniformScalarPainter(.3);auto* spatial=new SpatialAlpha;
  if(mode==1)f.left->SetAlpha(alpha,eAlphaBlend,.5);if(mode==2)f.left->SetAlpha(spatial,eAlphaBlend,.5);
  const unsigned copies=mode==3?3:1;auto* t=new Tracer(copies,mode==2);t->AttachScene(f.scene);
  Check(t->TracePhotons(100003,0,false,nullptr),"production fixed-budget shoot succeeds");
  Check(t->result.reservoirClosed,"published map restores manual append semantics");
  const double unit=nm?f.left->GetEmitter()->averageRadiantExitanceNM(550):1;
  const double expectedLeft=4*PI*unit*(mode==1?.3:mode==2?.25:copies),expectedRight=12*PI*unit*copies;
  // Six analytic standard errors of the source/alpha/deposit Bernoulli
  // event. Reservoir overflow adds at most an independent K=N sample's
  // conditional variance (law of total variance); repeated path deposits
  // are already included in the attempt-level variance.
  const double event=.25*(mode==1?.3:mode==2?.25:1);
  const double leftBand=6*std::sqrt((1-event)/(100003*event)+(copies>1?3.0/100003:0));
  const double rightBand=6*std::sqrt((1-.75)/(100003*.75)*(copies>1?2:1));
  Check(std::fabs(t->result.left/expectedLeft-1)<leftBand,"left absolute emitted/deposited power",t->result.left,expectedLeft);
  Check(std::fabs(t->result.right/expectedRight-1)<rightBand,"unchanged right source absolute power",t->result.right,expectedRight);
  if(mode==3)Check(t->result.calls==100003&&t->result.stored==100003,"overflow never stops attempts or grows capacity",t->result.calls,100003);
  t->release();alpha->release();spatial->release();
 }
 for(int edge=0;edge<4;++edge){Fixture f;auto* zero=new UniformScalarPainter(0);
  if(edge==0){f.objects->RemoveItem("left");f.objects->RemoveItem("right");}
  if(edge==1){f.left->SetAlpha(zero,eAlphaBlend,.5);f.right->SetAlpha(zero,eAlphaBlend,.5);}
  auto* t=new Tracer(edge==3?0:1); t->AttachScene(f.scene);ProgressCounter progress;const unsigned n=edge==2?0:113;
  Check(t->TracePhotons(n,0,false,&progress)&&t->result.published,"empty emission budget publishes valid empty map");
  Check(t->result.stored==0&&t->result.calls==(edge==3?n:0)&&progress.last==n,"empty/alpha-zero/N-zero terminates without lost attempts");
  t->release();zero->release();
 }
 for(bool temporal:{false,true}){Fixture f(true);auto* t=new Tracer(1,false,7);t->AttachScene(f.scene);ProgressCounter progress;
  Check(t->TracePhotons(1003,0,temporal,&progress),"progress/temporal shoot succeeds");
  const double expected=4*PI*(nm?f.left->GetEmitter()->averageRadiantExitanceNM(550):1);
  Check(t->result.calls==1003&&progress.last==1003,"integer remainders retain all attempts",t->result.calls,1003);
  Check(std::fabs(t->result.left/expected-1)<1e-10,"equal time-stratum normalized power",t->result.left,expected);t->release();
  t=new Tracer(1,false,7);t->AttachScene(f.scene);progress=ProgressCounter();progress.cancel=true;
  Check(!t->TracePhotons(1003,0,temporal,&progress)&&!t->result.published,"cancellation never publishes partial map");t->release();
 }
}
static bool Fill(GlobalPelPhotonMap& m,unsigned i){return m.Store(RISEPel(1),Point3(i,0,0),Vector3(0,0,1),Vector3(0,0,1));}
static bool Fill(CausticPelPhotonMap& m,unsigned i){return m.Store(RISEPel(1),Point3(i,0,0),Vector3(0,0,1));}
static bool Fill(TranslucentPelPhotonMap& m,unsigned i){return m.Store(RISEPel(1),Point3(i,0,0),Vector3(0,0,1),false);}
static bool Fill(GlobalSpectralPhotonMap& m,unsigned i){return m.Store(1,550,Point3(i,0,0),Vector3(0,0,1));}
static bool Fill(CausticSpectralPhotonMap& m,unsigned i){return m.Store(1,550,Point3(i,0,0),Vector3(0,0,1));}
static bool Fill(ShadowPhotonMap& m,unsigned i){return m.Store(Point3(i,0,0),i%2!=0);}
template<class Base>static void ReservoirChecks(){
 ReadMap<Base> map(100,nullptr),same(100,nullptr);
 if constexpr(std::is_base_of<ISpectralPhotonMap,Base>::value){map.ConfigureWavelengthSampling(550,552,1);same.ConfigureWavelengthSampling(550,552,1);}
 Check(map.EnableReservoir()&&same.EnableReservoir(),"fresh map enables reservoir");
 for(unsigned i=0;i<500;++i){Fill(map,i);Fill(same,i);}
 Check(map.NumStored()==100&&map.DepositsSeen()==500&&map.HasLate(),"every deposit remains eligible after capacity");
 Check(map.PositionSum()==same.PositionSum(),"reservoir RNG reproducible and independent of transport");
 const double correction=map.StorageNormalization();map.EndReservoir();map.ScalePhotonPower(correction/100);
 if constexpr(std::is_same<Base,ShadowPhotonMap>::value){Check(correction==1&&map.MaximumPower()==100&&map.LabelsCorrect(),"shadow categorical radius keeps only 1/N scaling");}
 else {Check(correction==5&&std::fabs(map.Side(false)+map.Side(true)-5)<1e-12,"flux reservoir M/K applied exactly once");}
 Check(!Fill(map,999)&&map.DepositsSeen()==0,"finalized manual store does not reenter reservoir");
 auto* bytes=new MemoryBuffer;map.Serialize(*bytes);bytes->seek(IBuffer::START,0);
 ReadMap<Base> loaded(100,nullptr);loaded.EnableReservoir();loaded.Deserialize(*bytes);bytes->release();
 Check(!loaded.ReservoirActive()&&!Fill(loaded,999)&&loaded.NumStored()==100,"deserialization retires shooting state");
}
int main(){ReservoirChecks<GlobalPelPhotonMap>();ReservoirChecks<CausticPelPhotonMap>();ReservoirChecks<TranslucentPelPhotonMap>();ReservoirChecks<GlobalSpectralPhotonMap>();ReservoirChecks<CausticSpectralPhotonMap>();ReservoirChecks<ShadowPhotonMap>();Cases<RGBTracer<GlobalPelPhotonMap>>(false);Cases<RGBTracer<CausticPelPhotonMap>>(false);Cases<RGBTracer<TranslucentPelPhotonMap>>(false);Cases<NMTracer<GlobalSpectralPhotonMap>>(true);Cases<NMTracer<CausticSpectralPhotonMap>>(true);
 Fixture f(false,true);auto* t=new RGBTracer<GlobalPelPhotonMap>(1,false,1,true);t->AttachScene(f.scene);Check(t->TracePhotons(100003,0,false,nullptr),"mixed mesh/nonmesh source shoot");Check(std::fabs(t->result.left/(4*PI)-1)<.035,"mesh power with point sibling",t->result.left,4*PI);Check(std::fabs(t->result.right/(12*PI)-1)<.035,"point-light directional-PDF and source-PDF power",t->result.right,12*PI);t->release();
 std::printf("%d passed / %d failed\n",passed,failed);return failed?1:0;
}
