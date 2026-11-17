// Finite light visibility must retain medium events in the endpoint exclusion tail.
#include "AlphaTransportFixture.h"
static void Replace(std::string& s,const std::string& a,const std::string& b) {
 const auto at=s.find(a);if(at==std::string::npos)std::exit(2);s.replace(at,a.size(),b);
}
int main() {
 for(unsigned mode=0;mode<3;++mode) for(unsigned w=0;w<3;++w) {
  auto r=mode==0?RastPT(1024):mode==1?RastBDPT(1024):RastVCM(1024);const std::string family=mode==0?"pathtracing":mode==1?"bdpt":"vcm";if(mode){Replace(r,"max_light_depth 8","max_light_depth 1");Replace(r,"max_eye_depth 8","max_eye_depth 2");}if(w){Replace(r,family+"_pel_rasterizer",family+"_spectral_rasterizer");Replace(r,"{\n","{\n hwss "+std::string(w==2?"true":"false")+"\n");}
  auto s=ReceiverScene(kOmni,false,0,kTight);Replace(s,"color 0.01 0.01 0.01","color 0 0 0");
  s+="homogeneous_medium\n{\n name tailmed\n absorption 2000 2000 2000\n scattering 0 0 0\n phase hg 0\n}\nsphere_geometry\n{\n name tailgeo\n radius 0.0005\n}\nlambertian_material\n{\n name tailmat\n reflectance pnt_recv\n alpha_mode opaque\n alpha_coverage 1\n}\nstandard_object\n{\n name tailobj\n geometry tailgeo\n material tailmat\n position 0 4 0\n casts_shadows FALSE\n interior_medium tailmed\n}\n";
  if(mode){Replace(s,"radius 0.0005","radius 0.0000005");Replace(s,"absorption 2000 2000 2000","absorption 2000000 2000000 2000000");}
  auto clear=s;Replace(clear,mode?"absorption 2000000 2000000 2000000":"absorption 2000 2000 2000","absorption 0 0 0");
  const double baseline=Render(Assemble(r,clear),"unabsorbed endpoint control");
  const double opaque=Render(Assemble(r,s),"opaque endpoint absorber");
  IJobPriv* foreign=nullptr;RISE_CreateJobPriv(&foreign);double white[3]={1,1,1};
  foreign->AddUniformColorPainter("white",white,"Rec709RGB_Linear");foreign->AddLambertianMaterial("alpha","white");foreign->SetMaterialAlpha("alpha","0.5","blend",.5);
  const double unrelated=Render(Assemble(r,s),"opaque tail with unrelated live alpha job");foreign->release();
  Check(std::fabs(unrelated/opaque-1)<.025,"foreign alpha job cannot change this scene medium estimator");
  Replace(s,"alpha_mode opaque","alpha_mode mask");
  const double mask=Render(Assemble(r,s),"MASK1 endpoint absorber");
  std::cout<<"tail mode="<<mode<<" w="<<w<<" clear="<<baseline<<" opaque="<<opaque<<" mask="<<mask<<" opaqueTr="<<opaque/baseline<<" maskTr="<<mask/baseline<<std::endl;
  Check(std::isfinite(baseline)&&baseline>.01,"finite unobstructed light control");
  if(mode==0)Check(std::fabs(opaque/baseline-std::exp(-1.0))<.01,"opaque tail matches exp(-1)");
  Check(std::fabs(mask/baseline-std::exp(-1.0))<.01,"MASK1 tail matches exp(-1)");
 }
 std::cout<<passCount<<" passed / "<<failCount<<" failed"<<std::endl;return failCount?1:0;
}
