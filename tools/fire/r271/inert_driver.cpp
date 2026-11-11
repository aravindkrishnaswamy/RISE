// r271 non-reacting plume: the real tier-8 production capstone from t=0 with RISE_FIRE_INERT_PLUME=1 (reaction, pilot and
// gas radiation off; frozen 26.4 kW heat source over the burner, FireCase::InertPlumeFraction). Harness copy only (see
// build_inert.py): stops at RISE_R271_WINDOW_S and publishes 16 Hz frames from RISE_INERT_FRAME_ORIGIN_S.
// argv: out dir, window s, frame origin s, optional resolution tier 6|8|10 (default 8; section 33) (> 0: the frame schedule's catch-up branch mislabels frames from an origin at
// the start time). The child exits non-zero at the end (frame-count and capstone-ignition checks assume a reacting
// full window); frames and retained checkpoints are written before that.
#define main FireSequenceSuiteMain
#include R271_HARNESS_SOURCE
#undef main
int main(int argc,char** argv){
 if(argc!=4&&argc!=5)return 1;const double window=std::stod(argv[2]),origin=std::stod(argv[3]);
 const double tier=argc==5?std::stod(argv[4]):8.0;if(tier!=6.&&tier!=8.&&tier!=10.)return 1;
 if(!(window>0.&&window<=30.)||!(origin>0.&&origin<window))return 1;
 if(std::filesystem::exists(argv[1]))return 91;
 if(!FireCase::InertPlumeSelected())return 1;
 setenv("RISE_R271_WINDOW_S",argv[2],1);setenv("RISE_INERT_FRAME_ORIGIN_S",argv[3],1);
 std::cerr<<"R271_INERT_BEGIN window_s="<<window<<" frame_origin_s="<<origin<<" tier="<<tier<<" heat_W="<<FireCase::InertPlumeHeatW
  <<" compact="<<FireProductionForceMetalCompactViscousStencil()<<" fds_mixing="<<FireFDSMixingTimeSelected()<<'\n';
 return RunProductionTemporalCapstoneChild(tier,0.0625,argv[1],false,tier==8.0);
}
