// Production VCM rasterizer store must count rejected emission attempts.
#include "AlphaTransportFixture.h"
static void Replace(std::string& s,const std::string& a,const std::string& b) {
 const auto at=s.find(a);if(at==std::string::npos){std::cerr<<"Missing fixture token "<<a;std::exit(2);}s.replace(at,a.size(),b);
}
int main() {
 for(unsigned wavelength=0;wavelength<3;++wavelength)for(unsigned mode=0;mode<3;++mode) {
  std::string rast=RastVCM(2048);
  Replace(rast,"merge_radius 0.0","merge_radius 0.20");
  if(mode==0)Replace(rast,"vc_enabled true","vc_enabled false");
  if(mode==1)Replace(rast,"vm_enabled true","vm_enabled false");
  if(wavelength) {
   Replace(rast,"vcm_pel_rasterizer","vcm_spectral_rasterizer");
   Replace(rast,"{\n","{\n hwss "+std::string(wavelength==2?"true":"false")+"\n");
  }
  std::string scene=ReceiverScene(kArea,false,0,kWide);
  Replace(scene,"color 0.01 0.01 0.01","color 0 0 0");
  const double base=Render(Assemble(rast,scene),"nonzero-radius opaque emitter");
  Replace(scene,"name mat_emit\n","name mat_emit\n alpha_mode blend\n alpha_coverage 0.3\n");
  const double alpha=Render(Assemble(rast,scene),"nonzero-radius alpha emitter");
  std::cout<<"wavelength="<<wavelength<<" mode="<<mode<<" opaque="<<base<<" alpha="<<alpha<<" ratio="<<alpha/base<<std::endl;
  Check(base>.01,"opaque emitter lights receiver with requested strategies");
  Check(alpha>=0 && std::fabs(alpha/base-.3)<.035,"emission attempts retain exactly one coverage factor");
 }
 std::cout<<passCount<<" passed / "<<failCount<<" failed"<<std::endl;return failCount?1:0;
}
