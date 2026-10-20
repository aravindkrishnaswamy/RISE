// Host-side observation of an unchanged production API. No request is modified.
#include "../../src/Library/Utilities/FireProductionForce.h"
namespace RISE { bool R255ObservedStep(const FireProductionProjectedHeunMetalOwnerRequest&,
 FireProductionResidentStepResult&,FireProductionProjectedHeunMetalOwnerResult* =nullptr,std::string* =nullptr); }
#define AttemptFireProductionProjectedHeunResidentStepMetal R255ObservedStep
#define main FireSequenceSuiteMain
#include R255_HARNESS_SOURCE
#undef main
#undef AttemptFireProductionProjectedHeunResidentStepMetal
namespace {
 std::ofstream activity,equivalence;std::vector<MethaneRunCheckpoint> expected;
 MethaneRunCheckpoint history;size_t step=0,verified=0;bool observe=false;
 bool FloatBitsMatch(float a,double b){const float bf=float(b);return std::memcmp(&a,&bf,sizeof(float))==0;}
 void Fatal(const std::string& message){std::cerr<<"R255_STOP "<<message<<'\n';std::exit(120);}
}
bool RISE::R255ObservedStep(const FireProductionProjectedHeunMetalOwnerRequest& r,
 FireProductionResidentStepResult& result,FireProductionProjectedHeunMetalOwnerResult* diagnostics,std::string* error){
 const bool ok=RISE::AttemptFireProductionProjectedHeunResidentStepMetal(r,result,diagnostics,error);
 if(!observe||!ok||!result.HasAcceptedManifoldToken())return ok;
 if(!diagnostics)Fatal("accepted owner diagnostics absent");
 const auto& d=*diagnostics;const auto& t=r.lineage.eos.physicalFlux.transport;const auto& s=t.shape;
 const double beginning=r.lineage.frozenSource.BeginningTimeS(),dt=r.lineage.frozenSource.TimeStepS(),end=beginning+dt;
 ++step;if(step>history.values.acceptedTimeStepHistoryS.size()||
  DoubleBits(dt)!=DoubleBits(history.values.acceptedTimeStepHistoryS[step-1]))Fatal("dt differs at step "+std::to_string(step));
 for(const auto& e:expected)if(step==e.acceptedSteps){
  bool same=DoubleBits(end)==DoubleBits(e.simulationTimeS);size_t mismatches=0;
  if(e.states.size()!=d.temperatureK.size()||d.conservativeValues.size()!=9*e.states.size())Fatal("reference dimensions differ");
  for(size_t i=0;i<e.states.size();++i){const auto q=ToConservativeVector(e.states[i]);
   for(size_t k=0;k<9;++k)mismatches+=!FloatBitsMatch(d.conservativeValues[k*e.states.size()+i],q[k]);
   mismatches+=!FloatBitsMatch(d.temperatureK[i],e.states[i].temperatureK);
  }
  for(size_t a=0;a<3;++a)for(size_t i=0;i<e.velocity.component[a].size();++i){
   mismatches+=!FloatBitsMatch(d.velocityMPerS[a][i],e.velocity.component[a][i]);
   mismatches+=!FloatBitsMatch(d.momentumKGPerM2S[a][i],e.momentum.component[a][i]);
  }
  same=same&&mismatches==0;equivalence<<step<<','<<end<<','<<same<<','<<mismatches<<'\n';equivalence.flush();
  if(!same)Fatal("physical fields differ at retained step "+std::to_string(step));
  ++verified;std::cerr<<"R255_BIT_IDENTICAL step="<<step<<" time="<<std::setprecision(17)<<end<<" fields=Q,T,M,U\n";
 }
 if(step>expected.back().acceptedSteps&&verified!=expected.size())Fatal("early identity gate incomplete");
 std::vector<std::array<double,4>> bins(3*s.nz);size_t offset=0;
 auto cell=[&](size_t x,size_t y,size_t z){return x+s.nx*(y+s.ny*z);};
 for(size_t a=0;a<3;++a){const size_t nx=s.nx+(a==0),ny=s.ny+(a==1),nz=s.nz+(a==2);
  for(size_t z=0;z<nz;++z)for(size_t y=0;y<ny;++y)for(size_t x=0;x<nx;++x){
   const size_t face=x+nx*(y+ny*z);const float alpha=d.acceptedFaceAlpha[offset+face];
   if(alpha<0.f||alpha>1.f||!std::isfinite(alpha))Fatal("invalid accepted alpha");
   // Primary MAC face locations; this is the inherited scalar-carrier limiter,
   // not a nonexistent independent momentum velocity blend.
   const double xx=(double(x)+(a==0?0.:.5)-.5*s.nx)*s.cellWidthM;
   const double yy=(double(y)+(a==1?0.:.5)-.5*s.ny)*s.cellWidthM;
   if(xx*xx+yy*yy>=.64)continue;
   size_t lx=x,ly=y,lz=z,hx=x,hy=y,hz=z;
   if(a==0){lx=x?x-1:0;hx=std::min(x,s.nx-1);}if(a==1){ly=y?y-1:0;hy=std::min(y,s.ny-1);}if(a==2){lz=z?z-1:0;hz=std::min(z,s.nz-1);}
   if(.5*(d.temperatureK[cell(lx,ly,lz)]+d.temperatureK[cell(hx,hy,hz)])<=301.)continue;
   auto& b=bins[a*s.nz+std::min(z,s.nz-1)];b[0]+=1;b[1]+=alpha<1.f;b[2]+=1.-alpha;b[3]+=alpha;
  }
  offset+=nx*ny*nz;
 }
 for(size_t a=0;a<3;++a)for(size_t z=0;z<s.nz;++z){const auto& b=bins[a*s.nz+z];activity<<step<<','<<beginning<<','<<dt<<','<<a<<','<<z<<','<<(z+.5)*s.cellWidthM;for(double v:b)activity<<','<<v;activity<<'\n';}
 activity.flush();if(!activity)Fatal("activity write failed");return ok;
}
int main(int argc,char** argv){
 if(argc!=5)return 1;std::string error;MethaneRunCheckpoint start;
 if(!LoadMethaneRunCheckpoint(argv[1],start,error)||!LoadMethaneRunCheckpoint(argv[2],history,error)){std::cerr<<error;return 2;}
 std::ifstream refs(argv[3]);std::string path;while(std::getline(refs,path)){MethaneRunCheckpoint e;if(!LoadMethaneRunCheckpoint(path,e,error))return 3;expected.push_back(std::move(e));}
 if(expected.size()!=2||expected[0].acceptedSteps<=start.acceptedSteps)return 4;
 const std::filesystem::path out=argv[4];std::filesystem::create_directories(out/"frames");
 activity.open(out/"limiter_instant.csv");equivalence.open(out/"equivalence.csv");activity<<std::setprecision(17)<<"step,beginning_s,dt_s,axis,z,height_m,faces,limited,low_order_sum,alpha_sum\n";equivalence<<std::setprecision(17)<<"step,time_s,bit_identical,mismatches\n";
 step=start.acceptedSteps;double time=start.simulationTimeS;size_t stop=step;
 while(time<start.simulationTimeS+4.){time+=history.values.acceptedTimeStepHistoryS.at(stop++);}
 RunPersistenceOptions p;p.resume=true;p.isolatedEquivalenceProbe=true;p.productionMetal=true;p.portedProductionTransport=true;
 p.checkpointPath=argv[1];p.checkpointCadenceWallS=std::numeric_limits<double>::max();
 p.isolatedExpectedCheckpointBuildId=start.producerBuildId;p.isolatedExpectedCheckpointDigest=DigestFile(argv[1]);p.stopAfterAdditionalAcceptedSteps=stop-step;
 p.temporalSnapshotDirectory=out/"frames";p.temporalSnapshotCadenceS=.0625;
 p.maximumProductionSourceStepS=double(float(.0016462659696117043));
 observe=true;std::cerr<<"R255_BEGIN step="<<step<<" time="<<start.simulationTimeS<<" additional="<<p.stopAfterAdditionalAcceptedSteps<<'\n';
 const auto result=RunMethaneFrameProbe(8,1,0.,25.091040127156969,16.,8.,.3,33.,false,p);
 std::ofstream receipt(out/"result.txt");receipt<<std::setprecision(17)<<"succeeded "<<result.succeeded<<"\nerror "<<result.structuredError<<"\nverified_states "<<verified<<"\nend_time_s "<<result.simulatedTimeS<<"\nsteps "<<step<<'\n';
 return result.succeeded&&verified==2?0:5;
}
