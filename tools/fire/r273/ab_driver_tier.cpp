// r273 low-tier A/B: resume a retained checkpoint for a bounded window at resolution tier 6 or 8.
// Soot settings come from the environment (RISE_FIRE_METHANE_SOOT_YIELD, RISE_FIRE_SOOT_RADIATIVE_FRACTION_CAP,
// RISE_FIRE_SOOT_SHEET_OXIDATION, RISE_FIRE_SOOT_SELF_ABSORPTION).
// argv: checkpoint, out dir, window s, tier 6|8.
// Environment: RISE_R273_FRAME_ORIGIN_S (required): 16 Hz frames anchored at this absolute time instead of the
// statistics start; RISE_R273_SNAPSHOT_EVERY=<n> with RISE_R273_SNAPSHOT_DIR: an immutable step_<offset>.checkpoint
// every n accepted steps after the resume.
// M2 (2026-11-23): runs the library driver (src/Library/Simulation/Fire) directly. Before M2 this file compiled a
// patched copy of tests/FireSequenceTest.cpp; the patches (window bound, frame origin, snapshots, the 8192-step
// probe limit) are now FireRunPersistenceOptions fields with the same arithmetic. Build: build_tool.py ab_driver_tier.
#include "../../../src/Library/Simulation/Fire/FireRunDriver.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
using namespace RISE::FireRun;
int main(int argc,char** argv){
 if(argc!=5)return 1;
 const std::filesystem::path checkpoint=argv[1],out=argv[2];const double window=std::stod(argv[3]),tier=std::stod(argv[4]);
 if(!(window>0.&&window<=10.)||(tier!=6.&&tier!=8.))return 1;
 const char* origin=std::getenv("RISE_R273_FRAME_ORIGIN_S");
 if(!origin){std::cerr<<"RISE_R273_FRAME_ORIGIN_S is required\n";return 1;}
 if(std::filesystem::exists(out))return 91;
 std::filesystem::create_directories(out/"frames");
 std::string error;MethaneRunCheckpoint start;
 if(!LoadMethaneRunCheckpoint(checkpoint,start,error)){std::cerr<<error<<'\n';return 2;}
 FireRunPersistenceOptions p;p.resume=true;p.isolatedEquivalenceProbe=true;p.productionMetal=true;p.portedProductionTransport=true;
 p.checkpointPath=checkpoint;p.checkpointCadenceWallS=std::numeric_limits<double>::max();
 p.isolatedExpectedCheckpointBuildId=start.producerBuildId;p.isolatedExpectedCheckpointDigest=DigestFile(checkpoint);
 p.stopAfterAdditionalAcceptedSteps=8192u;p.maximumRetainedProbeSteps=8192u;
 p.windowS=std::stod(argv[3]);
 p.hasTemporalFrameOrigin=true;p.temporalFrameOriginS=std::stod(origin);
 const char* snapshotEvery=std::getenv("RISE_R273_SNAPSHOT_EVERY");const char* snapshotDir=std::getenv("RISE_R273_SNAPSHOT_DIR");
 if(snapshotEvery&&snapshotDir&&std::atoi(snapshotEvery)>0){
  p.retainedSnapshotEvery=static_cast<unsigned>(std::atoi(snapshotEvery));p.retainedSnapshotDirectory=snapshotDir;}
 p.temporalSnapshotDirectory=out/"frames";p.temporalSnapshotCadenceS=.0625;
 p.maximumProductionSourceStepS=double(float(.0016462659696117043));
 const char* yield=std::getenv("RISE_FIRE_METHANE_SOOT_YIELD");const char* cap=std::getenv("RISE_FIRE_SOOT_RADIATIVE_FRACTION_CAP");
 const char* sheet=std::getenv("RISE_FIRE_SOOT_SHEET_OXIDATION");const char* self=std::getenv("RISE_FIRE_SOOT_SELF_ABSORPTION");
 std::cerr<<"R273_AB_BEGIN step="<<start.acceptedSteps<<" time="<<std::setprecision(17)<<start.simulationTimeS
  <<" window_s="<<window<<" tier="<<tier<<" yield="<<(yield?yield:"unset")<<" cap="<<(cap?cap:"unset")<<" sheet="<<(sheet?sheet:"unset")<<" self_absorption="<<(self?self:"unset")<<'\n';
 const auto wall=std::chrono::steady_clock::now();
 std::cerr<<"R275_AB_OPTIONS workers="<<FireRunWorkerCount(8u)<<" source_step_cap_s="<<FireProductionSourceStepCapS(p.maximumProductionSourceStepS)<<'\n';
 const auto r=RunFireMethaneDriver(FireRunWorkerCount(8u),1u,0.,tier==6.?25.156864278900095:25.091040127156969,16.,tier,CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,p);
 std::ofstream f(out/"result.txt");f<<std::setprecision(17)<<"succeeded "<<r.succeeded<<"\nerror "<<r.structuredError
  <<"\ncheckpoint "<<checkpoint.string()<<"\nstart_step "<<start.acceptedSteps<<"\nstart_time_s "<<start.simulationTimeS<<"\nend_time_s "<<r.simulatedTimeS
  <<"\nwindow_s "<<window<<"\ntier "<<tier<<"\nyield "<<(yield?yield:"unset")<<"\ncap "<<(cap?cap:"unset")<<"\nsheet "<<(sheet?sheet:"unset")<<"\nself_absorption "<<(self?self:"unset")
  <<"\ncase_record_id "<<r.caseRecordId<<"\nwall_s "<<std::chrono::duration<double>(std::chrono::steady_clock::now()-wall).count()<<'\n';
 return r.succeeded&&r.simulatedTimeS>=start.simulationTimeS+window?0:5;
}
