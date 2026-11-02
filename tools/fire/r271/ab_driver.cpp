// r271 A/B: resume a retained r270 checkpoint for a bounded physical window.
// The viscous stencil is selected by RISE_FIRE_COMPACT_VISCOUS_STENCIL at launch.
#define main FireSequenceSuiteMain
#include R271_HARNESS_SOURCE
#undef main
int main(int argc,char** argv){
 if(argc!=4)return 1;
 const std::filesystem::path checkpoint=argv[1],out=argv[2];const double window=std::stod(argv[3]);
 if(!(window>0.&&window<=4.))return 1;
 if(std::filesystem::exists(out))return 91;
 std::filesystem::create_directories(out/"frames");
 std::string error;MethaneRunCheckpoint start;
 if(!LoadMethaneRunCheckpoint(checkpoint,start,error)){std::cerr<<error<<'\n';return 2;}
 setenv("RISE_R271_WINDOW_S",argv[3],1);
 RunPersistenceOptions p;p.resume=true;p.isolatedEquivalenceProbe=true;p.productionMetal=true;p.portedProductionTransport=true;
 p.checkpointPath=checkpoint;p.checkpointCadenceWallS=std::numeric_limits<double>::max();
 p.isolatedExpectedCheckpointBuildId=start.producerBuildId;p.isolatedExpectedCheckpointDigest=DigestFile(checkpoint);
 p.stopAfterAdditionalAcceptedSteps=8192u;
 p.temporalSnapshotDirectory=out/"frames";p.temporalSnapshotCadenceS=.0625;
 p.maximumProductionSourceStepS=double(float(.0016462659696117043));
 const bool compact=FireProductionForceMetalCompactViscousStencil();
 const char* floor=std::getenv("RISE_FIRE_FLOOR_EMISSIVITY");
 std::cerr<<"R271_AB_BEGIN step="<<start.acceptedSteps<<" time="<<std::setprecision(17)<<start.simulationTimeS
  <<" window_s="<<window<<" compact="<<compact<<" floor="<<(floor?floor:"unset")<<'\n';
 const auto wall=std::chrono::steady_clock::now();
 const auto r=RunMethaneFrameProbe(8u,1u,0.,25.091040127156969,16.,8.,CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,false,p);
 std::ofstream f(out/"result.txt");f<<std::setprecision(17)<<"succeeded "<<r.succeeded<<"\nerror "<<r.structuredError
  <<"\ncheckpoint "<<checkpoint.string()<<"\ncheckpoint_sha256 "<<p.isolatedExpectedCheckpointDigest
  <<"\nstart_step "<<start.acceptedSteps<<"\nstart_time_s "<<start.simulationTimeS<<"\nend_time_s "<<r.simulatedTimeS
  <<"\nwindow_s "<<window<<"\nviscous_stencil "<<(compact?"r271-compact-mac-v2":"legacy")
  <<"\nfloor_emissivity "<<(floor?floor:"unset")<<"\ncase_record_id "<<r.caseRecordId
  <<"\nwall_s "<<std::chrono::duration<double>(std::chrono::steady_clock::now()-wall).count()<<'\n';
 return r.succeeded&&r.simulatedTimeS>=start.simulationTimeS+window?0:5;
}
