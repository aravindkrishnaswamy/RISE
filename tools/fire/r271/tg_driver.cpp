// r271 operator damping test: the real tier-8 production capstone from t=0, with a 2D Taylor-Green vortex array
// superposed in ambient air far above the burner (harness copy only; see build_tg.py). Stops at RISE_R271_WINDOW_S.
// argv: out dir, window s. Vortex: RISE_R271_TG_MODES (m), RISE_R271_TG_U (m/s); optional RISE_R271_CV.
#define main FireSequenceSuiteMain
#include R271_HARNESS_SOURCE
#undef main
int main(int argc,char** argv){
 if(argc!=3)return 1;const double window=std::stod(argv[2]);if(!(window>0.&&window<=1.))return 1;
 if(std::filesystem::exists(argv[1]))return 91;
 if(!std::getenv("RISE_R271_TG_MODES")||!std::getenv("RISE_R271_TG_U"))return 1;
 setenv("RISE_R271_WINDOW_S",argv[2],1);
 std::cerr<<"R271_TG_BEGIN modes="<<std::getenv("RISE_R271_TG_MODES")<<" U="<<std::getenv("RISE_R271_TG_U")
  <<" Cv="<<(std::getenv("RISE_R271_CV")?std::getenv("RISE_R271_CV"):"production")<<" compact="<<FireProductionForceMetalCompactViscousStencil()<<'\n';
 return RunProductionTemporalCapstoneChild(8.0,0.0625,argv[1],false,true);
}
