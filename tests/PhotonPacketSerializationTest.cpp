// Exact packet formats and intentional compressed-legacy migration boundary.
#include <cstdio>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <type_traits>
#include <limits>
#include "../src/Library/Job.h"
#include "../src/Library/Scene.h"
#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/PhotonMapping/GlobalPelPhotonMap.h"
#include "../src/Library/PhotonMapping/CausticPelPhotonMap.h"
#include "../src/Library/PhotonMapping/GlobalSpectralPhotonMap.h"
#include "../src/Library/PhotonMapping/CausticSpectralPhotonMap.h"
using namespace RISE;using namespace RISE::Implementation;
namespace {
unsigned checks=0,failures=0;
void Check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL %s\n",label);}}
template<class Base>class Probe:public Base {
public:
 explicit Probe(unsigned maximum):Base(maximum,nullptr){}
 const auto& First()const{return this->vphotons.front();}
};
template<class Base>constexpr bool Spectral(){return std::is_same<Base,GlobalSpectralPhotonMap>::value||std::is_same<Base,CausticSpectralPhotonMap>::value;}
template<class Base>bool Insert(Probe<Base>& map,const Vector3& wi){
 if constexpr(Spectral<Base>())return map.Store(2,550,Point3(.5,0,0),wi);
 else if constexpr(std::is_same<Base,GlobalPelPhotonMap>::value)return map.Store(RISEPel(2,3,4),Point3(.5,0,0),Vector3(0,0,1),wi);
 else return map.Store(RISEPel(2,3,4),Point3(.5,0,0),wi);
}
template<class Base>void Install(Job& job,Base* map){
 if constexpr(std::is_same<Base,GlobalPelPhotonMap>::value)job.GetScene()->SetGlobalPelMap(map);
 else if constexpr(std::is_same<Base,CausticPelPhotonMap>::value)job.GetScene()->SetCausticPelMap(map);
 else if constexpr(std::is_same<Base,GlobalSpectralPhotonMap>::value)job.GetScene()->SetGlobalSpectralMap(map);
 else job.GetScene()->SetCausticSpectralMap(map);
}
template<class Base>IPhotonMap* Installed(Job& job){
 if constexpr(std::is_same<Base,GlobalPelPhotonMap>::value)return job.GetScene()->GetGlobalPelMapMutable();
 else if constexpr(std::is_same<Base,CausticPelPhotonMap>::value)return job.GetScene()->GetCausticPelMapMutable();
 else if constexpr(std::is_same<Base,GlobalSpectralPhotonMap>::value)return job.GetScene()->GetGlobalSpectralMapMutable();
 else return job.GetScene()->GetCausticSpectralMapMutable();
}
template<class Base>bool Load(Job& job,const char* path){
 if constexpr(std::is_same<Base,GlobalPelPhotonMap>::value)return job.LoadGlobalPelPhotonmap(path);
 else if constexpr(std::is_same<Base,CausticPelPhotonMap>::value)return job.LoadCausticPelPhotonmap(path);
 else if constexpr(std::is_same<Base,GlobalSpectralPhotonMap>::value)return job.LoadGlobalSpectralPhotonmap(path);
 else return job.LoadCausticSpectralPhotonmap(path);
}
template<class Base>void Run(const char* name){
 std::printf("FAMILY %s\n",name);Probe<Base> source(3);const Vector3 wi(.3,.4,std::sqrt(.75));
 if constexpr(Spectral<Base>())Check(source.ConfigureWavelengthSampling(400,700,160),"source configured");
 Check(Insert(source,wi),"source packet inserted");source.Balance();
 if constexpr(Spectral<Base>())source.SetGatherParamsNM(.7,.08,0,3,6.75,nullptr);
 else source.SetGatherParams(.7,.08,0,3,nullptr);
 auto* buffer=new MemoryBuffer();source.Serialize(*buffer);const unsigned used=buffer->getCurPos();buffer->seek(IBuffer::START,0);
 Probe<Base> restored(0);const bool loaded=restored.DeserializeChecked(*buffer);Check(loaded,"new exact format loads");
 if(loaded){
  Check(restored.NumStored()==1&&restored.MaxPhotons()==3,"record and capacity retained");
  const auto& p=restored.First();Check(p.incomingDirection.x==wi.x&&p.incomingDirection.y==wi.y&&p.incomingDirection.z==wi.z,"exact incident direction survives roundtrip");
  Check(p.ptPosition.x==.5&&p.ptPosition.y==0&&p.ptPosition.z==0,"position survives roundtrip");
  if constexpr(Spectral<Base>()){
   double a=0,b=0;unsigned n=0;Check(restored.GetWavelengthSampling(a,b,n)&&a==400&&b==700&&n==160,"wavelength law survives roundtrip");double radius=0,ellipse=0,width=0;unsigned low=0,high=0;restored.GetGatherParamsNM(radius,ellipse,low,high,width);Check(width==6.75,"NM bandwidth survives roundtrip");Check(p.power==2&&p.nm==550,"spectral packet content retained");
   Check(!restored.ConfigureWavelengthSampling(380,780,160),"loaded nonempty map cannot change law");Check(!restored.Store(1,401,Point3(0,0,0),wi)&&restored.NumStored()==1,"loaded law rejects incompatible insertion");
  }else Check(p.power.r==2&&p.power.g==3&&p.power.b==4,"Pel packet power retained");
  Check(Insert(restored,wi)&&restored.NumStored()==2,"compatible insertion after load succeeds");
 }
 if constexpr(Spectral<Base>()){
  // The serialized atom population, not a recomputed ideal grid, is the law.
  for(Scalar invalid:{400.0,700.0,std::numeric_limits<Scalar>::infinity(),std::numeric_limits<Scalar>::quiet_NaN()}){
   auto* bad=new MemoryBuffer(used);bad->setBytes(buffer->Pointer(),used);bad->seek(IBuffer::START,48+8);bad->setDouble(invalid);bad->seek(IBuffer::START,0);
   const auto count=restored.NumStored();const Scalar atom=restored.SampleWavelength(1.5/160);
   Check(!restored.DeserializeChecked(*bad)&&restored.NumStored()==count&&restored.SampleWavelength(1.5/160)==atom,"malformed atom grid retains records and sampling law");bad->release();
  }
  auto* exact=new MemoryBuffer(used);exact->setBytes(buffer->Pointer(),used);exact->seek(IBuffer::START,48+8);const Scalar atom=std::nextafter(401.875,402.0);exact->setDouble(atom);exact->seek(IBuffer::START,0);
  Probe<Base> custom(0);Check(custom.DeserializeChecked(*exact)&&custom.SampleWavelength(1.5/160)==atom,"represented atom is authoritative across load");
  auto* again=new MemoryBuffer();custom.Serialize(*again);again->seek(IBuffer::START,0);Probe<Base> twice(0);Check(twice.DeserializeChecked(*again)&&twice.SampleWavelength(1.5/160)==atom,"represented atom survives second roundtrip");again->release();exact->release();
  for(unsigned n:{0u,160u,10000u}){
   Probe<Base> empty(4);if(n)Check(empty.ConfigureWavelengthSampling(400,700,n),"empty source sampling law configured");
   auto* bytes=new MemoryBuffer();empty.Serialize(*bytes);bytes->seek(IBuffer::START,0);Probe<Base> copy(0);
   const bool ok=copy.DeserializeChecked(*bytes);Check(ok&&copy.NumStored()==0&&copy.MaxPhotons()==4,"empty exact spectral map roundtrip");
   if(ok){double a=0,b=0;unsigned count=0;Check(copy.GetWavelengthSampling(a,b,count)==(n!=0)&&(!n||(a==400&&b==700&&count==n)),"empty map preserves configured state");}
   bytes->release();
  }
 }
 auto* truncated=new MemoryBuffer(used-1);truncated->setBytes(buffer->Pointer(),used-1);truncated->seek(IBuffer::START,0);
 const unsigned before=restored.NumStored();Check(!restored.DeserializeChecked(*truncated)&&restored.NumStored()==before,"truncated load retains prior records");truncated->release();
 // Alter the first exact direction, using the documented serialized byte fields.
 const unsigned directionOffset=std::is_same<Base,GlobalPelPhotonMap>::value?147:Spectral<Base>()?(169+4+160*8):157;
 buffer->seek(IBuffer::START,directionOffset);buffer->setDouble(0);buffer->setDouble(0);buffer->setDouble(0);buffer->seek(IBuffer::START,0);
 Check(!restored.DeserializeChecked(*buffer)&&restored.NumStored()==before,"invalid direction load is transactional");
 // Independent old-format writer: a real complete compressed record, not a
 // truncated prefix that could accidentally pass the wrong rejection path.
 auto* legacy=new MemoryBuffer(512);legacy->setUInt(1);legacy->setUInt(0);legacy->setDouble(1);legacy->setDouble(.05);legacy->setUInt(0);legacy->setUInt(1);legacy->setDouble(1);
 if constexpr(std::is_same<Base,GlobalPelPhotonMap>::value)legacy->setUChar(0);
 if constexpr(std::is_same<Base,CausticSpectralPhotonMap>::value)legacy->setDouble(1);
 BoundingBox(Point3(-1,-1,-1),Point3(1,1,1)).Serialize(*legacy);legacy->setUInt(1);Point3Ops::Serialize(Point3(.5,0,0),*legacy);legacy->setUChar(0);
 if constexpr(Spectral<Base>())legacy->setDouble(1);else ColorUtils::SerializeRGBPel(RISEPel(1),*legacy);
 legacy->setUChar(0);legacy->setUChar(0);
 if constexpr(Spectral<Base>())legacy->setDouble(550);
 if constexpr(std::is_same<Base,GlobalPelPhotonMap>::value){ColorUtils::SerializeRGBPel(RISEPel(0.0),*legacy);legacy->setUChar(0);legacy->setUChar(0);}
 const auto path=std::filesystem::temp_directory_path()/(std::string("rise_exact_packet_")+name+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".pmap");Check(legacy->DumpToFileToCursor(path.string().c_str()),"legacy complete fixture written");
 auto* job=new Job();auto* installed=new Base(0,nullptr);Install<Base>(*job,installed);
 Check(!Load<Base>(*job,path.string().c_str())&&Installed<Base>(*job)==installed,"legacy Job load fails and retains installed map");
 auto* valid=new MemoryBuffer();source.Serialize(*valid);Check(valid->DumpToFileToCursor(path.string().c_str()),"exact fixture written");Check(Load<Base>(*job,path.string().c_str())&&Installed<Base>(*job)!=installed&&Installed<Base>(*job)->NumStored()==1,"exact Job load installs valid map");
 std::filesystem::remove(path);valid->release();job->release();installed->release();legacy->release();buffer->release();
}
}
int main(){Run<GlobalPelPhotonMap>("globalPel");Run<CausticPelPhotonMap>("causticPel");Run<GlobalSpectralPhotonMap>("globalNM");Run<CausticSpectralPhotonMap>("causticNM");std::printf("PhotonPacketSerializationTest checks=%u failures=%u\n",checks,failures);return failures?1:0;}
