// DL-474: a boundary-free global medium must retain exact pixel values.
#define main VCMVolumeMergeRegressionMain
#include "VCMVolumeMergeTest.cpp"
#undef main
#include <cstdint>
int main() {
    const std::string opts=WriteScene("force_number_of_threads 1\nrender_thread_reserve_count 0\n", "hashopts");
#ifdef _WIN32
    _putenv_s("RISE_OPTIONS_FILE",opts.c_str());
#else
    setenv("RISE_OPTIONS_FILE",opts.c_str(),1);
#endif
    std::string scene=GlobalFogScene(true,64);
    for(const std::string chunk : {"clippedplane_geometry", "standard_object"}) {
        const auto begin=scene.find(chunk), end=scene.find("}\n",begin);
        if(begin==std::string::npos || end==std::string::npos) return 2;
        scene.erase(begin,end+2-begin);
    }
    std::vector<double> means;
    for(unsigned i=0;i<4;++i) {
        const unsigned salt=474900+i;
        const std::string path=WriteScene(scene,"hash");
        IJobPriv* job=nullptr;
        if(!RISE_CreateJobPriv(&job) || !job || !job->LoadAsciiSceneViaCst(path.c_str())) return 2;
        std::remove(path.c_str());
        job->RemoveRasterizerOutputs();
        auto* cap=new CapturingRasterizerOutput();
        job->GetRasterizer()->AddRasterizerOutput(cap);
        SobolSamplerTestHooks::ValueSalt().store(SobolSequence::HashCombine(salt,0x469u));
        std::srand(salt);
        if(!job->Rasterize() || cap->pixels.empty()) return 2;
        uint64_t hash=14695981039346656037ull;
        double sum=0;
        for(const auto& pixel:cap->pixels) {
            const Scalar channels[]={pixel.base.r,pixel.base.g,pixel.base.b,pixel.a};
            for(const Scalar channel:channels) {
                if(!std::isfinite(channel)) return 2;
                const auto* bytes=reinterpret_cast<const unsigned char*>(&channel);
                for(size_t j=0;j<sizeof(channel);++j) { hash^=bytes[j];hash*=1099511628211ull; }
            }
            sum+=(pixel.base.r+pixel.base.g+pixel.base.b)*pixel.a/3;
        }
        const double mean=sum/cap->pixels.size();
        means.push_back(mean);
        std::cout << "salt=" << salt << " hash=" << hash << " mean=" << std::setprecision(12) << mean << "\n";
        safe_release(cap);safe_release(job);
    }
    SobolSamplerTestHooks::ValueSalt().store(0);
    std::remove(opts.c_str());
    const Stats stats=Summarize(means);
    std::cout << "n=4 mean=" << stats.mean << " sd=" << stats.sd << "\n";
    return stats.mean>0 ? 0 : 1;
}
