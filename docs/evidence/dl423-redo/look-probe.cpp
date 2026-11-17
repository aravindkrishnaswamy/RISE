#define main CurvedFixtureMain
#include "../../../tests/CurvedOpenSheetSlabTest.cpp"
#undef main
#include "../../../src/Library/Interfaces/IScenePriv.h"
#include "../../../src/Library/Interfaces/ICamera.h"
#include "../../../src/Library/Interfaces/IObjectManager.h"
#include "../../../src/Library/Utilities/RuntimeContext.h"
#include "../../../src/Library/Intersection/RayIntersection.h"
int main(int argc,char**argv){
 if(argc!=5)return 2;unsigned salt=std::stoul(argv[2]);std::srand(salt);SobolSamplerTestHooks::ValueSalt().store(salt);
 IJobPriv*job=nullptr;if(!RISE_CreateJobPriv(&job)||!job)return 3;
 if(!job->LoadAsciiSceneViaCst(argv[1]))return 4;
 auto*cap=new CapturingRasterizerOutput();job->GetRasterizer()->AddRasterizerOutput(cap);
 if(!job->Rasterize())return 5;
 const auto*scene=job->GetScene();const auto*cam=scene->GetCamera();const auto*material=job->GetMaterials()->GetItem(argv[3]);if(!material)return 6;
 const unsigned w=scene->GetFilm()->GetWidth(),h=scene->GetFilm()->GetHeight();if(cap->pixels.size()!=w*h)return 7;
 RandomNumberGenerator rng(123);RuntimeContext rc(rng,RuntimeContext::PASS_NORMAL,false);
 std::ofstream out(argv[4],std::ios::binary);out.write(reinterpret_cast<const char*>(&w),sizeof(w));out.write(reinterpret_cast<const char*>(&h),sizeof(h));
 unsigned maskCount=0;for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
  Ray ray;if(!cam->GenerateRay(rc,ray,RasterConvention::PixelCentreToScreen(x,y,h)))return 8;RasterizerState state={x,y};RayIntersection hit(ray,state);job->GetObjects()->IntersectRay(hit,true,false,false);
  double mask=hit.geometric.bHit&&hit.pMaterial==material?1.:0.;maskCount+=unsigned(mask);const auto&c=cap->pixels[y*w+x];double pair[]={c.a*(.2126*c.base.r+.7152*c.base.g+.0722*c.base.b),mask};out.write(reinterpret_cast<const char*>(pair),sizeof(pair));
 }
 std::cout<<"LOOK salt="<<salt<<" mean="<<std::setprecision(12)<<MeanLuminance(*cap)<<" maskPixels="<<maskCount<<" hash="<<g_hash<<"\n";cap->release();job->release();return out.good()?0:9;
}
