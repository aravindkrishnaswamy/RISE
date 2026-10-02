#include "SMSRenderTestSupport.h"
#include <sstream>
// DL-347: the canonical k=1 refractor with the one-sided emitter turned
// away must equal SMS disabled. The camera can still see the lit top.
static std::string Fixture( bool sms, bool uniform, bool spectral, bool phong, bool turned )
{
    std::string scene=ReadScene("scenes/Tests/SMS/sms_k1_refract.RISEscene");
    Check(!scene.empty(),"canonical scene loaded");
    ReplaceFirstChunk(scene,"film","film\n{\n width 32\n height 24\n}");
    const auto lightName=scene.find("name\t\t\t\tlight_geom");
    Check(lightName!=std::string::npos,"light geometry found");
    if(turned) {
        ReplaceNamedChunk(scene,"clippedplane_geometry","name\t\t\t\tlight_geom",
            "clippedplane_geometry\n{\n name light_geom\n pta -0.1 0 0.1\n ptb 0.1 0 0.1\n ptc 0.1 0 -0.1\n ptd -0.1 0 -0.1\n doublesided FALSE\n}");
    }
    if(phong) {
        ReplaceNamedChunk(scene,"lambertian_luminaire_material","name\t\t\t\tlight_mat",
            "phong_luminaire_material\n{\n name light_mat\n exitance pnt_light\n scale 2000\n material none\n N 2\n}");
    }
    std::ostringstream rast;
    rast << (spectral ? "pathtracing_spectral_rasterizer" : "pathtracing_pel_rasterizer")
         << "\n{\n samples 512\n oidn_denoise FALSE\n pixel_filter box\n sms_enabled " << (sms ? "TRUE" : "FALSE")
         << "\n sms_seeding " << (uniform ? "uniform" : "snell") << "\n sms_max_iterations 30\n sms_threshold 1e-4\n sms_max_chain_depth 5\n sms_biased TRUE\n";
    if(spectral) rast << " num_wavelengths 16\n hwss FALSE\n";
    rast << "}\n";
    ReplaceFirstChunk(scene,"pathtracing_pel_rasterizer",rast.str());
    return scene;
}
int main( int argc, char** argv )
{
    if(argc>1 && std::string(argv[1])=="--canonical") {
        if(argc>2) g_seedBase=std::strtoul(argv[2],nullptr,10);
        const unsigned int trials=argc>3 ? std::max(1u,unsigned(std::strtoul(argv[3],nullptr,10))) : 3u;
        for(const char* name : {"sms_k1_refract","sms_k2_glassblock","sms_k2_glasssphere","sms_k1_botonly"}) {
            std::vector<double> means;
            for(unsigned int i=0;i<trials;++i) {
                const std::string path=std::string("scenes/Tests/SMS/")+name+".RISEscene";
                const auto r=Render(ReadScene(path.c_str()),name);
                Check(r.ok && r.mean>0,"canonical SMS render finite and lit");
                means.push_back(r.mean);
                std::cout << "canonical " << name << " trial=" << i << " mean=" << r.mean << " hash=" << r.hash << std::endl;
            }
            const auto stats=Summarize(means);
            std::cout << "canonical " << name << " mean=" << stats.mean << " sd=" << stats.sd << " n=" << trials << std::endl;
        }
        std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
        return failCount ? 1 : 0;
    }
    if(argc>1) g_seedBase=std::strtoul(argv[1],nullptr,10);
    for(bool spectral : {false,true}) for(bool uniform : {false,true}) for(bool phong : {false,true}) {
        std::vector<double> q;
        for(int t=0;t<4;++t) {
            const unsigned int pairIndex=g_renderIndex;
            const auto on=Render(Fixture(true,uniform,spectral,phong,true),"sided_on");
            g_renderIndex=pairIndex;
            const auto off=Render(Fixture(false,uniform,spectral,phong,true),"sided_off");
            Check(on.ok && off.ok && off.mean>0,"turned emitter fixture valid");
            q.push_back(off.mean>0 ? on.mean/off.mean : -1);
        }
        const auto stats=Summarize(q);
        std::cout << "DL-347 spectral=" << spectral << " uniform=" << uniform << " phong=" << phong << " SMS/PT=" << stats.mean << " sd=" << stats.sd << " n=4" << std::endl;
        Check(std::fabs(stats.mean-1)<0.05,"back-facing emitter cannot create an SMS caustic");
    }
    std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
    return failCount ? 1 : 0;
}
