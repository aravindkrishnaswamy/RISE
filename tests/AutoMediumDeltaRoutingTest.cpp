// DL-475: automatic routing must avoid the proved missing medium/delta class.
#include "VCMVolumeMergeFixture.h"
#include "../src/Library/Rendering/AutoRasterizer.h"
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
    } else if(std::string(rasterizer)!="auto_rasterizer") {
        chunk << "\tmax_eye_depth 32\n\tmax_light_depth 32\n";
    }
    if(std::string(rasterizer)=="auto_rasterizer") chunk << "\tprobe TRUE\n\tpathguiding FALSE\n";
    chunk << "}\n";
    scene.replace(at,end-at,chunk.str());
    return scene;
}
}

static double AutoMean(const std::string& scene,unsigned salt,bool guarded,AutoIntegratorChoice expected) {
    const std::string path=WriteScene(scene,"475auto");
    IJobPriv* job=nullptr;
    if(path.empty() || !RISE_CreateJobPriv(&job) || !job || !job->LoadAsciiSceneViaCst(path.c_str())) {
        safe_release(job); std::remove(path.c_str()); Check(false,"auto fixture loaded"); return -1;
    }
    std::remove(path.c_str()); job->RemoveRasterizerOutputs();
    auto* cap=new CapturingRasterizerOutput(); job->GetRasterizer()->AddRasterizerOutput(cap);
    auto* ras=dynamic_cast<AutoRasterizer*>(job->GetRasterizer());
    if(!ras) { safe_release(cap);safe_release(job);Check(false,"auto rasterizer constructed");return -1; }
    SobolSamplerTestHooks::ValueSalt().store(SobolSequence::HashCombine(salt,0x469u)); std::srand(salt);
    const bool rendered=job->Rasterize();
    SobolSamplerTestHooks::ValueSalt().store(0);
    Check(rendered && !cap->pixels.empty(),"auto render completed");
    Check(ras->ResolvedIntegrator()==expected,"auto support-safe integrator");
    const bool reason=std::string(ras->ResolveReason()).find("DL-475")!=std::string::npos;
    Check(reason==guarded,"guard reason only when applicable");
    if(guarded) Check(ras->LastProbeRenders()==0,"support guard skips variance probe");
    double mean=0;
    for(const auto& c:cap->pixels) mean+=(c.base.r+c.base.g+c.base.b)*c.a/3;
    mean=cap->pixels.empty() ? -1 : mean/cap->pixels.size();
    Check(std::isfinite(mean) && mean>=0,"auto finite radiance");
    std::cout << "route=" << ras->ResolvedIntegratorName() << " reason=" << ras->ResolveReason() << " mean=" << mean << "\n";
    safe_release(cap);safe_release(job);return mean;
}
int main() {
    const std::string opts=WriteScene("force_number_of_threads 1\nrender_thread_reserve_count 0\nauto_probe_activation_spp 1\nauto_probe_spp 4\nauto_probe_scale 4\nauto_probe_tau_caustic 1000000\n", "475routingopts");
#ifdef _WIN32
    _putenv_s("RISE_OPTIONS_FILE",opts.c_str());
#else
    setenv("RISE_OPTIONS_FILE",opts.c_str(),1);
#endif
    std::vector<double> av,pv,vv,dp,dv;
    for(unsigned i=0;i<8;++i) {
        const unsigned salt=47500+i;
        const double a=AutoMean(SingleScatter("auto_rasterizer",1024),salt,true,AutoIntegratorChoice::VCM);
        const double p=RenderMean(SingleScatter("pathtracing_pel_rasterizer",256),salt);
        const double v=RenderMean(SingleScatter("vcm_pel_rasterizer",1024),salt);
        av.push_back(a);pv.push_back(p);vv.push_back(v);dp.push_back(a-p);dv.push_back(a-v);
        Check(a>0 && p>0 && v>0,"every salted renderer reaches single scatter");
    }
    const Stats a=Summarize(av),p=Summarize(pv),v=Summarize(vv),d=Summarize(dp),e=Summarize(dv);
    Check(std::fabs(d.mean)<=3*d.sd/std::sqrt(8.0),"auto agrees with PT within paired3SE");
    Check(std::fabs(e.mean)<=3*e.sd/std::sqrt(8.0),"auto agrees with VCM within paired3SE");
    std::cout << std::setprecision(12) << "n8 AUTO " << a.mean << " sd " << a.sd << " PT " << p.mean << " sd " << p.sd << " VCM " << v.mean << " sd " << v.sd << "\n";
    std::string pinned=SingleScatter("auto_rasterizer",1);pinned.insert(pinned.find("\tprobe TRUE"),"\tintegrator bdpt\n");
    Check(AutoMean(pinned,47500,false,AutoIntegratorChoice::BDPT)==0,"author BDPT pin still exhibits support gap");
    std::string control=SingleScatter("auto_rasterizer",1);
    control.erase(control.find("\tinterior_medium fog\n"),std::string("\tinterior_medium fog\n").size());
    control.replace(control.find("probe TRUE"),10,"probe FALSE");
    AutoMean(control,47500,false,AutoIntegratorChoice::VCM);
    std::remove(opts.c_str());
    std::cout << passCount << " passed, " << failCount << " failed\n"; return failCount ? 1 : 0;
}
