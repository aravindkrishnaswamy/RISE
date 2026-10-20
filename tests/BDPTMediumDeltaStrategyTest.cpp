// DL-475: exact missing single-scatter class; strict energy pin stays red.
// Reuse the ledger's fog-box fixture and in-memory linear image capture.
#include "VCMVolumeMergeFixture.h"

namespace {
std::string SingleScatter(const char* rasterizer, unsigned spp) {
    std::string scene=FogBoxScene(false,spp,false);
    const size_t at=scene.find("pathtracing_pel_rasterizer");
    const size_t end=scene.find("}\n",at)+2;
    std::ostringstream chunk;
    chunk << rasterizer << "\n{\n\tsamples " << spp
          << "\n\tmax_volume_bounce 1\n\tpixel_filter box\n\toidn_denoise FALSE\n";
    if(std::string(rasterizer)=="pathtracing_pel_rasterizer") {
        chunk << "\ttransparent_shadows TRUE\n\tpathguiding FALSE\n\tadaptive_max_samples 0\n";
    } else {
        chunk << "\tmax_eye_depth 32\n\tmax_light_depth 32\n";
    }
    chunk << "}\n";
    scene.replace(at,end-at,chunk.str());
    return scene;
}
}
int main(int argc,char** argv) {
    const bool strict=argc==2 && std::string(argv[1])=="--strict-energy-pin";
    const std::string opts=WriteScene("force_number_of_threads 1\nrender_thread_reserve_count 0\n", "475opts");
    if(opts.empty()) return 2;
#ifdef _WIN32
    _putenv_s("RISE_OPTIONS_FILE",opts.c_str());
#else
    setenv("RISE_OPTIONS_FILE",opts.c_str(),1);
#endif
    std::vector<double> pt,bdpt,vcm,difference;
    for(unsigned i=0;i<8;++i) {
        const unsigned salt=47500+i;
        pt.push_back(RenderMean(SingleScatter("pathtracing_pel_rasterizer",256),salt));
        bdpt.push_back(RenderMean(SingleScatter("bdpt_pel_rasterizer",256),salt));
        vcm.push_back(RenderMean(SingleScatter("vcm_pel_rasterizer",1024),salt));
        difference.push_back(vcm.back()-pt.back());
        Check(pt.back()>0 && vcm.back()>0 && bdpt.back()==0,
              "single scatter: PT/VCM positive, BDPT exactly zero");
    }
    std::remove(opts.c_str());
    const Stats p=Summarize(pt),b=Summarize(bdpt),v=Summarize(vcm),d=Summarize(difference);
    std::cout << std::setprecision(9) << "DL475 n8 PT " << p.mean << " sd " << p.sd
              << " BDPT " << b.mean << " sd " << b.sd << " VCM " << v.mean << " sd " << v.sd
              << " VCM/PT " << v.mean/p.mean << " difference " << d.mean << " sd " << d.sd << std::endl;
    Check(std::fabs(d.mean)<=3*d.sd/std::sqrt(8.0),"VCM agrees with PT within paired 3SE");
    if(strict) Check(std::fabs(b.mean-p.mean)<=3*p.sd/std::sqrt(8.0),"strict BDPT/PT energy parity (unimplemented)");
    std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
    return failCount ? 1 : 0;
}
