// External from-zero onset diagnostic; unchanged production harness/physical owner.
#define main FireSequenceSuiteMain
#include "../../tests/FireSequenceTest.cpp"
#undef main
int main(int argc,char** argv){
 if(argc!=3)return 1;
 const double tier=std::stod(argv[1]);if(tier!=8.&&tier!=10.)return 1;
 const double end=tier==8.?3.5:3.;
 const std::filesystem::path out=argv[2];std::filesystem::create_directories(out);
 RunPersistenceOptions p;p.productionMetal=true;p.portedProductionTransport=true;
 p.productionOnsetDiagnosticDirectory=out/"budgets";
 p.productionOnsetStopVelocityMPerS=tier==8.?60.:15.;
 p.checkpointPath=out/"latest.checkpoint";p.finalCheckpointPath=out/"final.checkpoint";
 p.retainedCheckpointDirectory=out/"checkpoint_history";p.checkpointCadenceWallS=300.;
 p.maximumProductionSourceStepS=double(float(.0016462659696117043));
 const auto r=RunMethaneFrameProbe(8u,1u,end,tier==8.?25.091040127156969:25.05154563611109,16.,tier,.3,33.,false,p);
 std::ofstream f(out/"result.txt");f<<std::setprecision(17)<<"succeeded "<<r.succeeded<<"\nerror "<<r.structuredError<<"\nend_time_s "<<r.simulatedTimeS<<"\ntarget_time_s "<<end<<"\nfrom_zero true\n";
 return r.succeeded&&r.simulatedTimeS>=end?0:5;
}
