// OIDN-DENOISED-OK: DL-360 intentionally measures Auto quality determinism.
// Replays the same controlled nonzero Sobol salt; this is a clock sensitivity
// check, not a Monte Carlo mean/variance study. Other measurement fixtures
// disable OIDN or pin an explicit quality preset.
#include "SMSRenderTestSupport.h"
#include "../src/Library/Rendering/OIDNDenoiser.h"
#include "../src/Library/Rendering/AOVBuffers.h"
#include "../src/Library/RasterImages/RasterImage.h"
#include "../src/Library/Rendering/PixelBasedRasterizerHelper.h"
#include "../src/Library/Rendering/MLTSpectralRasterizer.h"
// Keep the public const-input red proof buildable with committed master
// OIDN headers, predating the cache inspection accessors. The preservation
// and fresh-output checks do not depend on those inspection methods.
template<class D> static auto LastQuality(const D& d,int)->decltype(d.GetLastResolvedQuality()) {return d.GetLastResolvedQuality();}
template<class D> static OidnQuality LastQuality(const D&,long) {return OidnQuality::Auto;}
template<class D> static auto LastDevice(const D& d,int)->decltype(d.GetLastResolvedDevice()) {return d.GetLastResolvedDevice();}
template<class D> static OidnDevice LastDevice(const D&,long) {return OidnDevice::Auto;}
template<class D> static auto DeviceGeneration(const D& d,int)->decltype(d.GetDeviceGeneration()) {return d.GetDeviceGeneration();}
template<class D> static unsigned DeviceGeneration(const D&,long) {return 0;}
static void FamilyPolicy()
{
#ifdef RISE_ENABLE_OIDN
    struct Row { const char* type; double weight; bool mlt; bool adaptive; };
    const Row rows[]={{"pixelpel_rasterizer",0.1,false,false},
        {"pixelintegratingspectral_rasterizer",0.1,false,false},
        {"pathtracing_pel_rasterizer",0.2,false,true},
        {"pathtracing_spectral_rasterizer",0.8,false,true},
        {"bdpt_pel_rasterizer",0.6,false,true},{"vcm_pel_rasterizer",0.6,false,true},
        {"bdpt_spectral_rasterizer",0.6,false,true},{"vcm_spectral_rasterizer",0.6,false,true},
        {"mlt_rasterizer",0.4,true,false},{"mlt_spectral_rasterizer",1.6,true,false}};
    for(const auto& row : rows) for(bool adaptive : {false,true}) {
        std::string scene=ReadScene("scenes/Tests/Materials/fabric_presets.RISEscene");
        std::ostringstream chunk;
        chunk << row.type << "\n{\n " << (row.mlt ? "mutations_per_pixel" : "samples") << " 25\n oidn_denoise TRUE\n";
        if(adaptive && row.adaptive) chunk << " adaptive_max_samples 40\n";
        chunk << "}\n";
        ReplaceFirstChunk(scene,"pixelpel_rasterizer",chunk.str());
        const std::string path=TestTempPath("cheapbatch_policy_family_"+std::to_string(::getpid())+".RISEscene");
        {std::ofstream f(path);f << scene;}
        IJobPriv* job=nullptr;
        const bool loaded=RISE_CreateJobPriv(&job) && job && job->LoadAsciiSceneViaCst(path.c_str());
        Check(loaded,"policy family scene loads");
        double rate=-1;
        if(loaded) {
            auto* raster=job->GetRasterizer();
            if(auto* spectral=dynamic_cast<MLTSpectralRasterizer*>(raster)) rate=spectral->EstimateDenoiseWorkPerMegapixel();
            else if(auto* mlt=dynamic_cast<MLTRasterizer*>(raster)) rate=mlt->EstimateDenoiseWorkPerMegapixel();
            else if(auto* pixel=dynamic_cast<PixelBasedRasterizerHelper*>(raster)) rate=pixel->EstimateDenoiseWorkPerMegapixel();
        }
        Check(rate==(adaptive && row.adaptive ? 40 : 25)*row.weight,"actual rasterizer configured/adaptive family policy rate");
        safe_release(job);std::remove(path.c_str());
    }
#endif
}
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
            Check(LastQuality(autoDenoiser,0)==expected,"full-frame configured preset equals boundary policy");
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
            Check(LastQuality(autoDenoiser,0)==expected,"crop configured preset equals boundary policy");
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
static void WarmCacheTransitions()
{
#ifdef RISE_ENABLE_OIDN
    const unsigned w=16,h=16;
    std::vector<float> input(w*h*3), output(w*h*3);
    for(size_t i=0;i<input.size();++i) input[i]=float(0.1+double(i%37)/37);
    OIDNDenoiser warmed;
    for(double rate : {2.8,3.0,20.0,2.8}) {
        const OidnQuality q=rate<3 ? OidnQuality::Fast : rate<20 ? OidnQuality::Balanced : OidnQuality::High;
        OIDNDenoiser fresh;
        std::vector<float> expected(output.size());
        warmed.Denoise(input.data(),nullptr,nullptr,w,h,output.data(),OidnQuality::Auto,OidnDevice::CPU,OidnPrefilter::Fast,rate);
        fresh.Denoise(input.data(),nullptr,nullptr,w,h,expected.data(),q,OidnDevice::CPU,OidnPrefilter::Fast,rate);
        Check(LastQuality(warmed,0)==q,"warmed Auto cache crosses quality buckets");
        Check(output==expected,"warmed Auto equals fresh explicit preset");
        Check(DeviceGeneration(warmed,0)==1,"quality changes reuse CPU device");
    }
    unsigned generation=DeviceGeneration(warmed,0);
    for(OidnDevice request : {OidnDevice::GPU,OidnDevice::CPU,OidnDevice::Auto,OidnDevice::CPU}) {
        warmed.Denoise(input.data(),nullptr,nullptr,w,h,output.data(),OidnQuality::Balanced,request,OidnPrefilter::Fast,3);
        Check(DeviceGeneration(warmed,0)==++generation,"changed backend request resolves a new device");
        Check(request!=OidnDevice::CPU || LastDevice(warmed,0)==OidnDevice::CPU,"CPU forces actual CPU after warmed GPU/Auto");
        OIDNDenoiser fresh;
        std::vector<float> expected(output.size());
        fresh.Denoise(input.data(),nullptr,nullptr,w,h,expected.data(),OidnQuality::Balanced,request,OidnPrefilter::Fast,3);
        Check(LastDevice(warmed,0)==LastDevice(fresh,0) && output==expected,"changed backend matches fresh resolver/output including GPU fallback");
    }
#endif
}
// DL-440: public const aux inputs must survive CPU Accurate prefilters.
// Repeated calls exercise cache hits; mode/presence/dimension changes
// exercise ownership transitions. Fresh explicit runs are the output oracle.
static void ConstAuxiliaryInputs()
{
#ifdef RISE_ENABLE_OIDN
    struct Row { unsigned w,h; bool albedo,normal; OidnPrefilter mode; };
    const Row rows[]={{16,16,true,true,OidnPrefilter::Fast},
        {16,16,true,true,OidnPrefilter::Accurate},{16,16,true,true,OidnPrefilter::Accurate},
        {16,16,true,false,OidnPrefilter::Accurate},{16,16,false,false,OidnPrefilter::Accurate},
        {24,19,true,true,OidnPrefilter::Accurate},{24,19,true,true,OidnPrefilter::Fast},
        {16,16,true,true,OidnPrefilter::Accurate}};
    OIDNDenoiser warmed;
    for(const auto& row : rows) {
        std::vector<float> beauty(row.w*row.h*3),albedo(beauty.size()),normal(beauty.size());
        for(size_t i=0;i<beauty.size();++i) {
            beauty[i]=float(.1+double((i*37)%97)/97);
            albedo[i]=float(.1+.8*double((i*13)%31)/31);
            normal[i]=i%3==2 ? .8f : float(.3*double((i*7)%23)/23);
        }
        std::vector<float> output(beauty.size()),expected(beauty.size());
        for(unsigned repeat=0;repeat<2;++repeat) {
        // Same pointers on the second call, with new data, exercise cache
        // hits and require a fresh Accurate input copy each time.
        albedo[0]+=0.001f;normal[0]+=0.001f;
        const auto originalAlbedo=albedo,originalNormal=normal;
        warmed.Denoise(beauty.data(),row.albedo ? albedo.data() : nullptr,
            row.normal ? normal.data() : nullptr,row.w,row.h,output.data(),
            OidnQuality::Balanced,OidnDevice::CPU,row.mode,3);
        Check(albedo==originalAlbedo && normal==originalNormal,"const auxiliary inputs unchanged after warmed CPU filter");
        OIDNDenoiser fresh;
        fresh.Denoise(beauty.data(),row.albedo ? originalAlbedo.data() : nullptr,
            row.normal ? originalNormal.data() : nullptr,row.w,row.h,expected.data(),
            OidnQuality::Balanced,OidnDevice::CPU,row.mode,3);
        Check(output==expected,"aux ownership/cache transitions match fresh denoiser");
        Check(std::any_of(output.begin(),output.end(),[](float x){return x>0;}),"const aux control produces lit output");
        Check(DeviceGeneration(warmed,0)==0 || DeviceGeneration(warmed,0)==1,"aux ownership transitions reuse CPU device when inspection is available");
        }
    }
#endif
}
int main(int argc,char** argv)
{
    ConfigureTestWorker(); // Scene loading can initialize cached global options.
    ConstAuxiliaryInputs();
    if(argc>1 && std::string(argv[1])=="--const-aux-only") {
        std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
        return failCount ? 1 : 0;
    }
    FamilyPolicy();
    PolicyBoundaries();
    WarmCacheTransitions();
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
