// Bounded retained-state reproduction; never an empirical continuation.
#define main FireSequenceSuiteMain
#include "../../tests/FireSequenceTest.cpp"
#undef main
int main(int argc,char** argv)
{
 if(argc==4&&std::string(argv[1])=="--spectrum"){
  MethaneRunCheckpoint c;std::string e;if(!LoadMethaneRunCheckpoint(argv[2],c,e))return 4;
  const std::filesystem::path directory(argv[3]);std::filesystem::create_directories(directory);
  UniformHannSpectrum full;
  if(!ComputeUniformHannSpectrum(c.values.probeTimeS,c.values.probeCenterlineHeatReleaseW,512u,full))return 5;
  std::ofstream signal(directory/"centerline_signal.csv"),spectra(directory/"spectrum.csv");
  signal<<std::setprecision(17)<<"time_s,centerline_hrr_W\n";
  for(size_t i=0;i<c.values.probeTimeS.size();++i)signal<<c.values.probeTimeS[i]<<','<<c.values.probeCenterlineHeatReleaseW[i]<<'\n';
  spectra<<std::setprecision(17)<<"signal,bin,frequency_Hz,power,power_fraction\n";
  auto write=[&](const std::string& name,const UniformHannSpectrum& s){
   for(size_t k=0;k<s.power.size();++k)spectra<<name<<','<<k<<','<<s.frequencyHz[k]<<','<<s.power[k]<<','<<s.powerFraction[k]<<'\n';
   std::cout<<name<<"_peak_Hz="<<std::setprecision(17)<<s.frequencyHz[s.dominantBin]<<'\n';
  };
  write("full",full);UniformHannSpectrum mean;
  for(size_t window=0;window<3;++window){const size_t start=128u*window;
   std::vector<double> time(full.uniformTimeS.begin()+start,full.uniformTimeS.begin()+start+256u);
   std::vector<double> values(full.uniformSignal.begin()+start,full.uniformSignal.begin()+start+256u);
   UniformHannSpectrum s;if(!ComputeUniformHannSpectrum(time,values,256u,s))return 6;
   write("window_"+std::to_string(window),s);
   if(window==0u){mean=s;std::fill(mean.power.begin(),mean.power.end(),0.);}
   for(size_t k=0;k<s.power.size();++k)mean.power[k]+=s.power[k]/3.;
  }
  const double sum=std::accumulate(mean.power.begin()+1,mean.power.end(),0.);
  mean.dominantBin=static_cast<size_t>(std::max_element(mean.power.begin()+1,mean.power.end())-mean.power.begin());
  for(size_t k=0;k<mean.power.size();++k)mean.powerFraction[k]=mean.power[k]/sum;
  write("three_window_mean",mean);
  return ClosePublishedStream(signal)&&ClosePublishedStream(spectra)?0:7;
 }
 if(argc<3||argc>4)return 1;
 MethaneRunCheckpoint checkpoint;std::string error;
 if(!LoadMethaneRunCheckpoint(argv[1],checkpoint,error)){std::cerr<<error;return 2;}
 const std::filesystem::path directory(argv[2]);
 std::filesystem::create_directories(directory/"frames");
 std::filesystem::create_directories(directory/"checkpoints");
 RunPersistenceOptions options;options.productionMetal=true;options.portedProductionTransport=true;
 options.resume=true;options.isolatedEquivalenceProbe=true;options.checkpointPath=argv[1];
 options.isolatedExpectedCheckpointBuildId=checkpoint.producerBuildId;
 options.isolatedExpectedCheckpointDigest=DigestFile(argv[1]);options.stopAfterAdditionalAcceptedSteps=64u;
 options.checkpointCadenceWallS=std::numeric_limits<double>::max();
 options.equivalenceSnapshotDirectory=directory/"checkpoints";
 options.temporalSnapshotDirectory=directory/"frames";options.temporalSnapshotCadenceS=.0625;
 options.maximumProductionSourceStepS=static_cast<double>(static_cast<float>(.0016462659696117043));
 if(argc==4)options.maximumProductionSourceStepS*=std::stod(argv[3]);
 setenv("RISE_FIRE_PICARD_TRACE","1",1);
 const auto result=RunMethaneFrameProbe(8u,8u,0.0,25.091040127156969,16.,8.,
  CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,false,options);
 std::cout<<std::setprecision(17)<<"success="<<result.succeeded<<" time="<<result.simulatedTimeS
  <<" steps="<<result.acceptedTimeStepHistoryS.size()<<" error="<<result.structuredError<<'\n';
 return result.succeeded?0:3;
}
