// Host-side observation of an unchanged production API. No request is modified.
#include "../../src/Library/Utilities/FireProductionForce.h"
namespace RISE { bool R256ObservedStep(const FireProductionProjectedHeunMetalOwnerRequest&,
 FireProductionResidentStepResult&,FireProductionProjectedHeunMetalOwnerResult* =nullptr,std::string* =nullptr); }
#define AttemptFireProductionProjectedHeunResidentStepMetal R256ObservedStep
#define main FireSequenceSuiteMain
#include R256_HARNESS_SOURCE
#undef main
#undef AttemptFireProductionProjectedHeunResidentStepMetal
namespace {
 std::ofstream activity,mass;size_t step=0;bool observe=false;
 double previousTime=0.,nativeWidth=0.;std::array<size_t,3> stations{};
 std::array<double,3> previousMass{},massIntegral{};
 void Fatal(const std::string& message){std::cerr<<"R256_STOP "<<message<<'\n';std::exit(120);}
}
bool RISE::R256ObservedStep(const FireProductionProjectedHeunMetalOwnerRequest& r,
 FireProductionResidentStepResult& result,FireProductionProjectedHeunMetalOwnerResult* diagnostics,std::string* error){
 const bool ok=RISE::AttemptFireProductionProjectedHeunResidentStepMetal(r,result,diagnostics,error);
 if(!observe||!ok||!result.HasAcceptedManifoldToken())return ok;
 if(!diagnostics)Fatal("accepted owner diagnostics absent");
 const auto& d=*diagnostics;const auto& t=r.lineage.eos.physicalFlux.transport;const auto& s=t.shape;
 const double beginning=r.lineage.frozenSource.BeginningTimeS(),dt=r.lineage.frozenSource.TimeStepS(),end=beginning+dt;
 ++step;
 const size_t plane=s.nx*s.ny;
 for(size_t station=0;station<3;++station){
  const size_t z=stations[station];double net=0.,up=0.;
  for(size_t y=0;y<s.ny;++y)for(size_t x=0;x<s.nx;++x){
   const size_t i=x+s.nx*(y+s.ny*z);double rho=0.;
   for(size_t k=0;k<MethaneCarbon;++k)rho+=std::max(0.,double(d.conservativeValues[(1+k)*s.CellCount()+i]));
   const float w=.5f*(d.velocityMPerS[2][i]+d.velocityMPerS[2][i+plane]);
   const double flux=rho*w*nativeWidth*nativeWidth;
   const double xx=(double(x)+.5-.5*s.nx)*s.cellWidthM,yy=(double(y)+.5-.5*s.ny)*s.cellWidthM;
   up+=std::max(0.,flux);if(xx*xx+yy*yy<.64)net+=flux;
  }
  massIntegral[station]+=.5*(previousMass[station]+net)*(end-previousTime);
  mass<<step<<','<<end<<','<<dt<<','<<z<<','<<(z+.5)*nativeWidth<<','<<net<<','<<up<<','<<massIntegral[station]<<'\n';previousMass[station]=net;
 }
 previousTime=end;mass.flush();if(!mass)Fatal("mass output failed");
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
 if(argc!=4)return 1;std::string error;MethaneRunCheckpoint start,history;
 if(!LoadMethaneRunCheckpoint(argv[1],start,error)||!LoadMethaneRunCheckpoint(argv[2],history,error)){std::cerr<<error;return 2;}
 const std::filesystem::path out=argv[3];std::filesystem::create_directories(out/"frames");
 activity.open(out/"limiter_instant.csv");mass.open(out/"mass_timeseries.csv");activity<<std::setprecision(17)<<"step,beginning_s,dt_s,axis,z,height_m,faces,limited,low_order_sum,alpha_sum\n";
 mass<<std::setprecision(17)<<"step,time_s,dt_s,z,height_m,mass_r08_kg_s,plane_up_kg_s,integrated_mass_kg\n";
 step=start.acceptedSteps;previousTime=start.simulationTimeS;nativeWidth=start.cellWidthM;
 for(size_t k=0;k<3;++k){
  stations[k]=size_t(std::round((k+1)/start.cellWidthM-.5));const size_t z=stations[k],nx=start.dimensions[0],ny=start.dimensions[1],plane=nx*ny;
  for(size_t y=0;y<ny;++y)for(size_t x=0;x<nx;++x){const size_t i=x+nx*(y+ny*z);
   const double xx=(double(x)+.5-.5*nx)*start.cellWidthM,yy=(double(y)+.5-.5*ny)*start.cellWidthM;if(xx*xx+yy*yy>=.64)continue;
   double rho=0.;for(size_t a=0;a<MethaneCarbon;++a)rho+=std::max(0.,start.states[i].constituent[a]);
   const float w=.5f*(float(start.velocity.component[2][i])+float(start.velocity.component[2][i+plane]));
   previousMass[k]+=rho*w*start.cellWidthM*start.cellWidthM;
  }
  mass<<step<<','<<previousTime<<",0,"<<z<<','<<(z+.5)*start.cellWidthM<<','<<previousMass[k]<<",nan,0\n";
 }
 double time=start.simulationTimeS;size_t stop=step;
 // The original history estimates a four-second step count only. It is never
 // imposed on this continuation; dt and physical fields are unrestricted.
 while(time<start.simulationTimeS+4.)time+=history.values.acceptedTimeStepHistoryS.at(stop++);
 RunPersistenceOptions p;p.resume=true;p.isolatedEquivalenceProbe=true;p.productionMetal=true;p.portedProductionTransport=true;
 p.checkpointPath=argv[1];p.checkpointCadenceWallS=std::numeric_limits<double>::max();
 p.isolatedExpectedCheckpointBuildId=start.producerBuildId;p.isolatedExpectedCheckpointDigest=DigestFile(argv[1]);p.stopAfterAdditionalAcceptedSteps=stop-step;
 p.temporalSnapshotDirectory=out/"frames";p.temporalSnapshotCadenceS=.0625;p.maximumProductionSourceStepS=double(float(.0016462659696117043));
 observe=true;std::cerr<<"R256_BEGIN step="<<step<<" time="<<start.simulationTimeS<<" additional="<<p.stopAfterAdditionalAcceptedSteps<<'\n';
 const auto result=RunMethaneFrameProbe(8,1,0.,25.091040127156969,16.,8.,.3,33.,false,p);
 std::ofstream receipt(out/"result.txt");receipt<<std::setprecision(17)<<"succeeded "<<result.succeeded<<"\nerror "<<result.structuredError<<"\nstart_time_s "<<start.simulationTimeS<<"\nend_time_s "<<result.simulatedTimeS<<"\nsteps "<<step<<'\n';
 for(size_t k=0;k<3;++k)receipt<<"mean_mass_kg_s "<<stations[k]<<' '<<massIntegral[k]/(previousTime-start.simulationTimeS)<<'\n';
 return result.succeeded?0:5;
}
