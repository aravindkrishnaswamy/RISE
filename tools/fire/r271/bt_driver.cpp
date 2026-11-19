// r271 small-scale response tests (section 24): the real tier-8 production capstone from t=0 with a small temperature
// pattern superposed in ambient air at z 1.6-3.0 m (harness copy only; see build_bt.py). Stops at RISE_R271_WINDOW_S.
// argv: out dir, window s. RISE_R271_BT_KIND=buoy|adv, RISE_R271_BT_N (cells per wavelength), RISE_R271_BT_A (K),
// RISE_R271_BT_U (m/s, adv only). Per-step R271_BT lines on stderr (see build_bt.py).
#define main FireSequenceSuiteMain
#include R271_HARNESS_SOURCE
#undef main
int main(int argc,char** argv){
 if(argc!=3)return 1;const double window=std::stod(argv[2]);if(!(window>0.&&window<=1.))return 1;
 if(std::filesystem::exists(argv[1]))return 91;
 if(!std::getenv("RISE_R271_BT_KIND")||!std::getenv("RISE_R271_BT_N")||!std::getenv("RISE_R271_BT_A"))return 1;
 setenv("RISE_R271_WINDOW_S",argv[2],1);
 std::cerr<<"R271_BT_BEGIN kind="<<std::getenv("RISE_R271_BT_KIND")<<" n="<<std::getenv("RISE_R271_BT_N")<<" A="<<std::getenv("RISE_R271_BT_A")
  <<" U="<<(std::getenv("RISE_R271_BT_U")?std::getenv("RISE_R271_BT_U"):"0")<<" compact="<<FireProductionForceMetalCompactViscousStencil()<<'\n';
 return RunProductionTemporalCapstoneChild(8.0,0.0625,argv[1],false,true);
}
