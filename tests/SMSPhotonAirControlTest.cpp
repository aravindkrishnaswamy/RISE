// DL-331: four salted comparisons of deterministic and photon seeds in air.
#define main ExteriorFixtureMain
#include "ExteriorIndexInvarianceTest.cpp"
#undef main
int main() {
 const std::string root=std::getenv("RISE_MEDIA_PATH") ? std::getenv("RISE_MEDIA_PATH") : "";
 auto deterministic=PatchShippedScene(ReadFileText(root+"scenes/Tests/SMS/sms_k1_refract.RISEscene"),256,32,32,false);
 auto photons=deterministic;auto at=photons.find("sms_enabled");if(at==std::string::npos)return 2;
 photons.insert(at,"sms_photon_count 100000\n\t");
 auto a=WriteScene(deterministic,"331_air_deterministic"),b=WriteScene(photons,"331_air_photons");std::vector<double> x,y;
 for(unsigned i=0;i<4;++i){unsigned seed=331100+i;uint32_t salt=0x9e3779b9u*seed;
  x.push_back(RenderMean(a,seed,1,false,Point3(0,0,0),"air deterministic",nullptr,nullptr,salt));
  y.push_back(RenderMean(b,seed,1,false,Point3(0,0,0),"air photons",nullptr,nullptr,salt^0x5bd1e995u));}
 auto sx=Summarize(x),sy=Summarize(y);double band=1.5*std::hypot(sx.sd,sy.sd);
 std::printf("DL331 air k1 photon/deterministic %.9g deterministic %.9g SE %.9g photon %.9g SE %.9g combined3sigma %.9g within=%d\n",sy.mean/sx.mean,sx.mean,sx.sd/2,sy.mean,sy.sd/2,band,std::abs(sy.mean-sx.mean)<=band);
 std::remove(a.c_str());std::remove(b.c_str());return sx.mean>0&&sy.mean>0?0:1;
}
