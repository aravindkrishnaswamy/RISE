// OIDN-DENOISED-OK: DL-360 intentionally measures Auto quality determinism.
// Replays the same controlled nonzero Sobol salt; this is a clock sensitivity
// check, not a Monte Carlo mean/variance study. Other measurement fixtures
// disable OIDN or pin an explicit quality preset.
#include "SMSRenderTestSupport.h"
int main()
{
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
