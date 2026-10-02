#include "SMSRenderTestSupport.h"
#include <sstream>
// N-SF11 coefficients: SCHOTT optical glass datasheet (B1..3,C1..3).
// https://media.schott.com/api/public/content/78e83df5ca2c4da4ad4490a52c80a146?v=1a468147
static std::string Fixture( double nm, bool hwss, bool dispersive )
{
    std::ostringstream s;
    s << "RISE ASCII SCENE 7\nfilm\n{\n width 64\n height 16\n}\n"
         "orthographic_camera\n{\n location 0 0.5 0\n lookat 0 0 0\n up 0 0 1\n viewport_scale 3 1.5\n}\n"
         "uniformcolor_painter\n{\n name grey\n color 0.5 0.5 0.5\n colorspace Rec709RGB_Linear\n}\n"
         "uniformcolor_painter\n{\n name white\n color 1 1 1\n colorspace Rec709RGB_Linear\n}\n"
         "lambertian_material\n{\n name floor_mat\n reflectance grey\n}\n";
    if(dispersive) s << "scalar_painter\n{\n name sf11\n sellmeier 1.73759695 0.313747346 1.898781010 0.013188707 0.0623068142 155.23629\n}\n";
    s << "perfectrefractor_material\n{\n name glass\n refractance white\n ior " << (dispersive ? "sf11" : "1.78") << "\n}\n"
         "clippedplane_geometry\n{\n name floor\n pta -5 0 5\n ptb 5 0 5\n ptc 5 0 -5\n ptd -5 0 -5\n doublesided FALSE\n}\n"
         "standard_object\n{\n name receiver\n geometry floor\n material floor_mat\n}\n"
         "clippedplane_geometry\n{\n name caster\n pta -0.5 1 -0.5\n ptb 0.5 1 -0.5\n ptc 0.5 1 0.5\n ptd -0.5 1 0.5\n doublesided TRUE\n}\n"
         "standard_object\n{\n name pane\n geometry caster\n material glass\n}\n"
         "omni_light\n{\n name source\n position 0.5 2 0\n color 1 1 1\n power 40\n}\n"
         "standard_shader\n{\n name global\n shaderop DefaultPathTracing\n}\n"
         "pathtracing_spectral_rasterizer\n{\n samples 128\n oidn_denoise FALSE\n pixel_filter box\n sms_enabled TRUE\n sms_seeding uniform\n sms_target_bounces 1\n sms_biased TRUE\n sms_multi_trials 4\n num_wavelengths 1\n spectral_samples 1\n hwss " << (hwss ? "TRUE" : "FALSE")
      << "\n nmbegin " << nm << "\n nmend " << nm+0.01 << "\n}\n";
    return s.str();
}
static double Centroid( const RenderResult& r )
{
    double weighted=0, sum=0;
    for(size_t i=0;i<r.pixels.size();++i) {
        const auto& c=r.pixels[i];
        const double v=(c.base.r+c.base.g+c.base.b)*c.a;
        weighted += v * (double(i%64)+0.5);
        sum += v;
    }
    return sum>0 ? weighted/sum : -1;
}
int main( int argc, char** argv )
{
    ConfigureTestWorker();
    for(const char* path : {"scenes/Tests/Spectral/spectral_dispersive_caustic_pt_sms.RISEscene",
                           "scenes/Tests/Spectral/spectral_dispersive_caustic_pt_sms_uniform.RISEscene"}) {
        IJobPriv* job=nullptr;
        const bool created=RISE_CreateJobPriv(&job) && job;
        Check(created && job->LoadAsciiSceneViaCst(path),"shipped spectral SMS scene loads");
        safe_release(job);
    }
    if(argc>1 && std::string(argv[1])=="--shipped") {
        for(bool uniform : {false,true}) {
            const char* path=uniform ? "scenes/Tests/Spectral/spectral_dispersive_caustic_pt_sms_uniform.RISEscene"
                                     : "scenes/Tests/Spectral/spectral_dispersive_caustic_pt_sms.RISEscene";
            std::string scene=ReadScene(path);
            ReplaceFirstChunk(scene,"film","film\n{\n width 16\n height 16\n}");
            if(uniform) ReplaceFirstChunk(scene,"pathtracing_spectral_rasterizer",
                "pathtracing_spectral_rasterizer\n{\n samples 8\n spectral_samples 1\n num_wavelengths 4\n nmbegin 405\n nmend 705\n hwss TRUE\n oidn_denoise FALSE\n sms_enabled TRUE\n sms_seeding uniform\n sms_target_bounces 2\n sms_max_iterations 20\n sms_threshold 1e-5\n sms_max_chain_depth 10\n sms_biased TRUE\n}");
            else ReplaceFirstChunk(scene,"pixelintegratingspectral_rasterizer",
                "pixelintegratingspectral_rasterizer\n{\n samples 8\n lum_samples 1\n nmbegin 405\n nmend 705\n num_wavelengths 4\n max_recursion 3\n oidn_denoise FALSE\n}");
            std::vector<double> means;
            for(int i=0;i<4;++i) {
                const auto r=Render(scene,"spectral_shipped");
                Check(r.ok && r.mean>0,"shipped spectral SMS smoke finite and lit");
                means.push_back(r.mean);
            }
            const auto stats=Summarize(means);
            std::cout << "shipped spectral uniform=" << uniform << " mean=" << stats.mean << " sd=" << stats.sd << " n=4 (16x16,8spp smoke)" << std::endl;
        }
        std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
        return failCount ? 1 : 0;
    }
    for(bool hwss : {false,true}) for(bool dispersion : {false,true}) {
        std::vector<double> shifts;
        for(int t=0;t<4;++t) {
            const unsigned index=g_renderIndex;
            const auto blue=Render(Fixture(450,hwss,dispersion),"uniform_blue");
            g_renderIndex=index;
            const auto red=Render(Fixture(650,hwss,dispersion),"uniform_red");
            Check(blue.ok && red.ok && blue.mean>0 && red.mean>0,"uniform spectral fixture finite and lit");
            shifts.push_back(Centroid(blue)-Centroid(red));
        }
        const auto stats=Summarize(shifts);
        std::cout << "DL-353 hwss=" << hwss << " dispersion=" << dispersion << " centroid shift=" << stats.mean << " sd=" << stats.sd << " n=4" << std::endl;
        if(dispersion) Check(std::fabs(stats.mean)>0.02,"N-SF11 caustic moves with wavelength");
        else Check(std::fabs(stats.mean)<0.005,"constant-index control has no wavelength displacement");
    }
    std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
    return failCount ? 1 : 0;
}
