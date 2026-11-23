// r275: exercises the capstone's incremental manifest on a short resumed tier-6 window. Same as ab_driver_tier.cpp
// (resume a retained checkpoint, 16 Hz frames anchored at RISE_R273_FRAME_ORIGIN_S) except that frames are written to
// <out>/checkpoints like the capstone does and the capstone's partial-manifest observer is attached, so
// <out>/sequence_manifest.partial.rise-fire.cbor appears after the second frame and grows with every frame.
// argv: checkpoint, out dir, window s (tier 6 only). Build: build_tool.py ab_partial_manifest <out dir>.
#define main FireSequenceSuiteMain
#include R271_HARNESS_SOURCE
#undef main
int main(int argc,char** argv){
 if(argc!=4)return 1;
 const std::filesystem::path checkpoint=argv[1],out=argv[2];const double window=std::stod(argv[3]);
 if(!(window>0.&&window<=10.))return 1;
 if(std::filesystem::exists(out))return 91;
 std::filesystem::create_directories(out/"checkpoints");
 std::string error;MethaneRunCheckpoint start;
 if(!LoadMethaneRunCheckpoint(checkpoint,start,error)){std::cerr<<error<<'\n';return 2;}
 setenv("RISE_R271_WINDOW_S",argv[3],1);
 const double targetS=25.156864278900095;
 RunPersistenceOptions p;p.resume=true;p.isolatedEquivalenceProbe=true;p.productionMetal=true;p.portedProductionTransport=true;
 p.checkpointPath=checkpoint;p.checkpointCadenceWallS=std::numeric_limits<double>::max();
 p.isolatedExpectedCheckpointBuildId=start.producerBuildId;p.isolatedExpectedCheckpointDigest=DigestFile(checkpoint);
 p.stopAfterAdditionalAcceptedSteps=8192u;
 p.temporalSnapshotDirectory=out/"checkpoints";p.temporalSnapshotCadenceS=.0625;
 p.temporalFrameObserver=MakePartialTemporalManifestObserver(out,p.temporalSnapshotDirectory,6.0,.0625,targetS,false,false);
 p.maximumProductionSourceStepS=double(float(.0016462659696117043));
 const auto r=RunMethaneFrameProbe(FireRunWorkerCount(8u),1u,0.,targetS,16.,6.,CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,false,p);
 std::ofstream f(out/"result.txt");f<<std::setprecision(17)<<"succeeded "<<r.succeeded<<"\nerror "<<r.structuredError
  <<"\nstart_time_s "<<start.simulationTimeS<<"\nend_time_s "<<r.simulatedTimeS<<"\ncase_record_id "<<r.caseRecordId<<'\n';
 return r.succeeded&&r.simulatedTimeS>=start.simulationTimeS+window?0:5;
}
