// Actual direct-light entry points must retain boundary events when shadows are disabled.
#include "AlphaTransportFixture.h"
static void Replace(std::string& s,const std::string& a,const std::string& b) {
 const auto at=s.find(a);if(at==std::string::npos){std::cerr<<"Missing fixture token "<<a;std::exit(2);}s.replace(at,a.size(),b);
}
static std::string Raster(unsigned w) {
 std::string r=RastPT(1024);if(w){Replace(r,"pathtracing_pel_rasterizer","pathtracing_spectral_rasterizer");Replace(r,"{\n","{\n hwss "+std::string(w==2?"true":"false")+"\n");}return r;
}
static std::string Scene(LightKind kind,bool shadows,int mediumAlpha=-1,bool blocker=false) {
 std::string s=ReceiverScene(kind,false,0,kTight);Replace(s,"color 0.01 0.01 0.01","color 0 0 0");
 if(!shadows)Replace(s,"name obj_recv\n","name obj_recv\n receives_shadows FALSE\n");
 s+="homogeneous_medium\n{\n name testmed\n absorption 1 1 1\n scattering 0 0 0\n phase hg 0\n}\nsphere_geometry\n{\n name medgeo\n radius 0.5\n}\nlambertian_material\n{\n name boundarymat\n reflectance pnt_recv\n";
 if(mediumAlpha>=0)s+=" alpha_mode mask\n alpha_coverage "+std::to_string(mediumAlpha)+"\n";
 s+="}\nstandard_object\n{\n name medobj\n geometry medgeo\n material boundarymat\n position 0 2 0\n casts_shadows FALSE\n interior_medium testmed\n}\n";
 if(blocker)s+="standard_object\n{\n name blocker\n geometry medgeo\n material mat_recv_base\n position 0 3.2 0\n}\n";
 return s;
}
int main(){
 const std::string unused="lambertian_material\n{\n name unused_alpha\n reflectance pnt_recv\n alpha_mode blend\n alpha_coverage 0.5\n}\n";
 for(unsigned w=0;w<3;++w)for(LightKind kind:{kOmni,kDirectional,kArea})for(bool shadows:{true,false}) {
  const auto r=Raster(w);const auto s=Scene(kind,shadows);const double base=Render(Assemble(r,s),"medium baseline");
  Check(std::isfinite(base)&&base>.001,"opaque medium control finite and nonzero");
  const double local=Render(Assemble(r,s+unused),"unused local alpha");
  IJobPriv* other=nullptr;RISE_CreateJobPriv(&other);double white[3]={1,1,1};other->AddUniformColorPainter("white",white,"Rec709RGB_Linear");other->AddLambertianMaterial("alpha","white");other->SetMaterialAlpha("alpha","0.5","blend",.5);
  const double foreign=Render(Assemble(r,s),"unrelated live alpha job");other->release();
  std::cout<<"w="<<w<<" kind="<<kind<<" shadows="<<shadows<<" base="<<base<<" local="<<local/base<<" foreign="<<foreign/base<<std::endl;
  Check(std::fabs(local/base-1)<.025,"unused material preserves medium attenuation");Check(std::fabs(foreign/base-1)<.025,"foreign job preserves medium attenuation");
  if(!shadows){
   const double rejected=Render(Assemble(r,Scene(kind,false,0,true)),"rejected alpha boundary past ordinary occluder");
   const double accepted=Render(Assemble(r,Scene(kind,false,1,true)),"accepted alpha boundary past ordinary occluder");
   std::cout<<"accepted="<<accepted/base<<" rejected="<<rejected/base<<std::endl;
   Check(std::fabs(accepted/base-1)<.025,"disabled shadows ignore blocker but retain accepted medium");
   if(kind==kArea){auto clear=Scene(kind,false,1,true);Replace(clear,"absorption 1 1 1","absorption 0 0 0");const double noabs=Render(Assemble(r,clear),"area zero-absorption chord control");Check(std::fabs(rejected/noabs-1)<.025,"area rejected boundary matches unabsorbed chords");}
   else Check(rejected/base>2.65&&rejected/base<2.78,"rejected medium boundaries do not absorb (exp(1) ratio)");
  }
 }
 // Successive deterministic lights query distinct medium segments.
 for(unsigned w=0;w<3;++w)for(bool shadows:{true,false}){
  auto first=Scene(kDirectional,shadows,1);auto second=first;Replace(second,"direction 0 1 0","direction 1 1 0");
  const auto r=Raster(w);const double a=Render(Assemble(r,first),"first medium segment");const double b=Render(Assemble(r,second),"second medium segment");
  first+="directional_light\n{\n name second\n direction 1 1 0\n color 1 1 1\n power 1.0\n}\n";
  const double both=Render(Assemble(r,first),"successive distinct light rays");
  Check(std::fabs(both/(a+b)-1)<.025,"each directional light gets its own boundary records");
  auto env=Scene(kOmni,shadows,1);Replace(env,"radius 0.5","radius 1.2");auto at=env.find("omni_light\n");auto end=env.find("\n}",at);env.erase(at,end+2-at);
  // Index-matched delta boundary: changing alpha must not also remove a
  // diffuse scatterer from BSDF continuation under the environment.
  Replace(env,"lambertian_material\n{\n name boundarymat\n reflectance pnt_recv\n","dielectric_material\n{\n name boundarymat\n tau 1 1 1\n ior 1\n scattering 1000000\n");
  const std::string envPainter="uniformcolor_painter\n{\n name env\n color 1 1 1\n colorspace Rec709RGB_Linear\n}\n";
  auto er=r;Replace(er,"{\n","{\n radiance_map env\n");er=envPainter+er;
  const double opaque=Render(Assemble(er,env),"environment medium segment");
  auto cut=env;Replace(cut,"alpha_coverage 1","alpha_coverage 0");const double removed=Render(Assemble(er,cut),"environment rejected boundary");
  auto clear=env;Replace(clear,"absorption 1 1 1","absorption 0 0 0");const double unabsorbed=Render(Assemble(er,clear),"environment accepted unabsorbed control");
  // Match the alpha/path topology in each optical-depth comparison. Even
  // index1 delta vertices participate in legacy visibility/MIS policy.
  auto cutClear=cut;Replace(cutClear,"absorption 1 1 1","absorption 0 0 0");const double cutUnabsorbed=Render(Assemble(er,cutClear),"environment rejected unabsorbed control");
  std::cout<<"env w="<<w<<" shadows="<<shadows<<" absorbed="<<opaque<<" rejected="<<removed<<" clear="<<unabsorbed<<" cutClear="<<cutUnabsorbed<<std::endl;
  Check(std::isfinite(opaque)&&opaque>.01&&opaque<.90*unabsorbed,"environment control includes at least 10% object absorption");
  Check(std::fabs(removed/cutUnabsorbed-1)<.025,"environment rejected boundaries retain full segment light");
  if(!shadows){auto opaqueClear=clear;Replace(opaqueClear,"alpha_mode mask","alpha_mode opaque");const double opaqueValue=Render(Assemble(er,opaqueClear),"opaque index1 zero-absorption attribution");
   std::cout<<"opaque index1 w="<<w<<" value="<<opaqueValue<<" MASK1="<<unabsorbed<<std::endl;
   Check(std::fabs(opaqueValue/unabsorbed-1)<.025,"MASK1 preserves opaque index1 environment baseline");
  }
 }
 std::cout<<passCount<<" passed / "<<failCount<<" failed"<<std::endl;return failCount?1:0;
}
