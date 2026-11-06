// r271 section 25 (transplant): the real tier-8 production capstone from t=0 with RISE_FIRE_INERT_PLUME=1, its state
// replaced at step 0 by FDS r271_inert's instantaneous temperature and velocity (build_bt.py kind fds; harness copy only).
// Stops at RISE_R271_WINDOW_S; retained checkpoints (checkpoint_history) are the output.
// argv: out dir, window s. Env: RISE_R271_XP_STATE, RISE_R271_XP_CKPT [, RISE_R271_XP_OWN=1 control].
#define main FireSequenceSuiteMain
#include R271_HARNESS_SOURCE
#undef main
int main(int argc,char** argv){
 if(argc!=3)return 1;const double window=std::stod(argv[2]);if(!(window>0.&&window<=4.))return 1;
 if(std::filesystem::exists(argv[1]))return 91;
 if(!FireCase::InertPlumeSelected()||!std::getenv("RISE_R271_XP_STATE")||!std::getenv("RISE_R271_XP_CKPT"))return 1;
 setenv("RISE_R271_WINDOW_S",argv[2],1);setenv("RISE_R271_BT_KIND","fds",1);setenv("RISE_R271_BT_N","6",0);
 std::cerr<<"R271_XP_BEGIN window_s="<<window<<" state="<<std::getenv("RISE_R271_XP_STATE")<<" ckpt="<<std::getenv("RISE_R271_XP_CKPT")
  <<" compact="<<FireProductionForceMetalCompactViscousStencil()<<'\n';
 return RunProductionTemporalCapstoneChild(8.0,0.0625,argv[1],false,true);
}
