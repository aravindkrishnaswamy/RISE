// DL-352: canonical mask, independent PT and VCM, four salted replicas.
#define main ExteriorFixtureMain
#include "ExteriorIndexInvarianceTest.cpp"
#undef main
int main(int argc,char** argv) {
 const unsigned budget=argc>1 ? std::strtoul(argv[1],nullptr,10) : 16384;
 const unsigned refBudget=argc>2 ? std::strtoul(argv[2],nullptr,10) : 512;
 const std::string root=std::getenv("RISE_MEDIA_PATH") ? std::getenv("RISE_MEDIA_PATH") : "";
 const std::string shipped=ReadFileText(root+"scenes/Tests/SMS/sms_k2_glassblock.RISEscene");
 std::string sms=PatchShippedScene(shipped,budget,100,75,false),pt=PatchShippedScene(shipped,budget,100,75,false);
 auto at=pt.find("sms_enabled"); if(at==std::string::npos) return 2; auto end=pt.find("\n",at); pt.replace(at,end-at,"sms_enabled FALSE");
 std::string ref=PatchShippedScene(ReadFileText(root+"scenes/Tests/SMS/sms_k2_glassblock_ref.RISEscene"),refBudget,100,75,false);
 auto maskpath=WriteScene(PatchShippedScene(shipped,256,100,75,true),"352_mask");
 std::vector<double> image; RenderMean(maskpath,1,1,false,Point3(0,0,0),"mask",nullptr,&image,0x5ca1ab1e);
 std::remove(maskpath.c_str());double full=0;for(double x:image)full=std::max(full,x);
 std::vector<char> mask(image.size());unsigned n=0;for(unsigned i=0;i<mask.size();++i)if(image[i]>.99*full){mask[i]=1;++n;}
 if(n<200 || n>1000) return 2;std::printf("DL352 mask=%u\n",n);
 auto a=WriteScene(sms,"352_sms"),b=WriteScene(pt,"352_pt"),c=WriteScene(ref,"352_vcm");
 std::vector<double> x,y,z;
 for(unsigned t=0;t<4;++t) {
  const unsigned seed=352000+t;const uint32_t salt=0x9e3779b9u*seed;
  x.push_back(RenderMean(a,seed,1,false,Point3(0,0,0),"sms",&mask,nullptr,salt,true));
  y.push_back(RenderMean(b,seed,1,false,Point3(0,0,0),"pt",&mask,nullptr,salt^0xab12u,true));
  z.push_back(RenderMean(c,seed,1,false,Point3(0,0,0),"vcm",&mask,nullptr,salt^0x5bd1e995u,false));
 }
 auto sx=Summarize(x),sy=Summarize(y),sz=Summarize(z);
 std::printf("DL352 SMS %.9g SE %.9g PT %.9g SE %.9g VCM %.9g SE %.9g SMS/PT %.7g SMS/VCM %.7g PT/VCM %.7g\n",sx.mean,sx.sd/2,sy.mean,sy.sd/2,sz.mean,sz.sd/2,sx.mean/sy.mean,sx.mean/sz.mean,sy.mean/sz.mean);
 std::printf("DL352 combined3sigma SMS/PT %.9g SMS/VCM %.9g\n",1.5*std::hypot(sx.sd,sy.sd),1.5*std::hypot(sx.sd,sz.sd));
 Check(sx.mean>0&&sy.mean>0&&sz.mean>0,"DL352 independent references finite and lit");
 Check(std::abs(sx.mean-sy.mean)<=1.5*std::hypot(sx.sd,sy.sd),"DL352 SMS/PT within combined three SE");
 Check(std::abs(sx.mean-sz.mean)<=1.5*std::hypot(sx.sd,sz.sd),"DL352 SMS/VCM within combined three SE");
 std::printf("DL352 %d passed, %d failed\n",passCount,failCount);
 std::remove(a.c_str());std::remove(b.c_str());std::remove(c.c_str());return failCount?1:0;
}
