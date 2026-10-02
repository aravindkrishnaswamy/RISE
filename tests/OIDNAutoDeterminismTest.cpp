// OIDN-DENOISED-OK: DL-360 intentionally measures Auto quality determinism.
// Replays the same controlled nonzero Sobol salt; this is a clock sensitivity
// check, not a Monte Carlo mean/variance study. Other measurement fixtures
// disable OIDN or pin an explicit quality preset.
#include "SMSRenderTestSupport.h"
#include "../src/Library/Rendering/OIDNDenoiser.h"
#include "../src/Library/Rendering/AOVBuffers.h"
#include "../src/Library/RasterImages/RasterImage.h"
static void PolicyBoundaries()
{
#ifdef RISE_ENABLE_OIDN
    // Policy rate is area independent. The former area multiply/divide
    // crossed 3/20 at these full-frame/crop sizes through roundoff.
    for(const double rate : {2.8,3.0,3.2,19.2,20.0,20.8}) {
        const OidnQuality expected=rate<3 ? OidnQuality::Fast : rate<20 ? OidnQuality::Balanced : OidnQuality::High;
        for(const auto& dims : {std::pair<unsigned,unsigned>(19,24),{16,32}}) {
            const unsigned w=dims.first,h=dims.second;
            IRasterImage* a=new RISERasterImage(w,h,RISEColor(RISEPel(0,0,0),1));
            IRasterImage* b=new RISERasterImage(w,h,RISEColor(RISEPel(0,0,0),1));
            for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) {
                const double v=0.1+double((x*37+y*61)%97)/97;
                const RISEColor c(RISEPel(v,0.7*v,0.3*v),1);
                a->SetPEL(x,y,c);b->SetPEL(x,y,c);
            }
            AOVBuffers guides(w,h,AOVBuffers::Plan(false,false,false));
            OIDNDenoiser autoDenoiser,explicitDenoiser;
            autoDenoiser.ApplyDenoise(*a,guides,w,h,OidnQuality::Auto,OidnDevice::CPU,OidnPrefilter::Fast,rate);
            explicitDenoiser.ApplyDenoise(*b,guides,w,h,expected,OidnDevice::CPU,OidnPrefilter::Fast,rate);
            bool equal=true;
            for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) {
                const auto ac=a->GetPEL(x,y),bc=b->GetPEL(x,y);
                equal &= ac.base.r==bc.base.r && ac.base.g==bc.base.g && ac.base.b==bc.base.b;
                const double v=0.1+double((x*37+y*61)%97)/97;
                const RISEColor c(RISEPel(v,0.7*v,0.3*v),1);
                a->SetPEL(x,y,c);b->SetPEL(x,y,c);
            }
            Check(equal,"full-frame Auto equals explicit boundary preset");
            autoDenoiser.ApplyDenoiseRegion(*a,guides,w,h,0,0,w-2,h-2,OidnQuality::Auto,OidnDevice::CPU,OidnPrefilter::Fast,rate);
            explicitDenoiser.ApplyDenoiseRegion(*b,guides,w,h,0,0,w-2,h-2,expected,OidnDevice::CPU,OidnPrefilter::Fast,rate);
            equal=true;
            for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) {
                const auto ac=a->GetPEL(x,y),bc=b->GetPEL(x,y);
                equal &= ac.base.r==bc.base.r && ac.base.g==bc.base.g && ac.base.b==bc.base.b;
            }
            Check(equal,"cropped Auto equals same explicit boundary preset");
            safe_release(a);safe_release(b);
        }
    }
#endif
}
int main(int argc,char** argv)
{
    PolicyBoundaries();
    if(argc>1 && std::string(argv[1])=="--policy-only") {
        std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
        return failCount ? 1 : 0;
    }
    const std::string scene=ReadScene("scenes/Tests/Materials/fabric_presets.RISEscene");
    Check(!scene.empty(),"shipped fabric_presets scene loaded");
    // Legacy pixel family: 64 spp * policy weight 0.1 = 6.4 s/MP,
    // so Auto must equal an explicitly pinned Balanced render. This
    // also catches an old timer that happens to choose High on every run.
    std::string balanced=scene;
    const auto raster=balanced.find("pixelpel_rasterizer");
    const auto open=balanced.find('{',raster);
    Check(open!=std::string::npos,"fabric rasterizer found");
    if(open==std::string::npos) return 1;
    balanced.insert(open+1,"\n oidn_quality balanced\n");
    g_renderIndex=0;
    const auto reference=Render(balanced,"oidn_balanced");
    Check(reference.ok && reference.mean>0,"pinned Balanced reference finite and lit");
    unsigned long long firstHash=0, firstRaw=0;
    for(unsigned int i=0;i<8;++i) {
        g_renderIndex=0;
        const auto r=Render(scene,"oidn_auto",(i%2) ? 8000 : 0);
        Check(r.ok && r.mean>0,"fabric render finite and lit");
        Check(i%2==0 || r.appliedDelayMs==8000,"slow run exercised the wall-clock perturbation");
        if(i==0) { firstHash=r.hash; firstRaw=r.rawHash; }
        Check(r.rawHash!=1469598103934665603ULL,"pre-denoise raw capture present");
        Check(r.rawHash==firstRaw,"same salted raw input despite wall-clock delay");
        Check(r.hash==reference.hash,"Auto matches its scene-static Balanced policy");
        Check(r.hash==firstHash,"Auto denoised output independent of wall-clock delay");
        std::cout << "DL-360 replicate=" << i << " mean=" << r.mean << " raw=" << r.rawHash << " denoised=" << r.hash << std::endl;
    }
    std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
    return failCount ? 1 : 0;
}
