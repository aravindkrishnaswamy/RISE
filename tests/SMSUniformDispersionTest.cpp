#include "SMSRenderTestSupport.h"
#include <sstream>
// N-SF11 coefficients: SCHOTT optical glass datasheet (B1..3,C1..3).
// https://media.schott.com/api/public/content/78e83df5ca2c4da4ad4490a52c80a146?v=1a468147
static std::string Fixture( double nm, bool hwss, bool dispersive, bool mesh = false )
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
    std::string scene=s.str();
    if(mesh) ReplaceNamedChunk(scene,"clippedplane_geometry","name caster",
        "indexedmesh_geometry\n{\n name caster\n"
        " vertex -0.5 1 -0.5\n vertex 0.5 1 -0.5\n vertex 0.5 1 0.5\n vertex -0.5 1 0.5\n"
        " uv 0 0\n uv 1 0\n uv 1 1\n uv 0 1\n"
        " triangle 0 1 2\n triangle 0 2 3\n double_sided TRUE\n face_normals TRUE\n}");
    return scene;
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
static void UVIndexControl( bool objectSpace = false )
{
    // The origin has a different IOR; every interior sample is 1.78.
    // Paired paths should be pixel-identical to constant 1.78. No noisy
    // mean band is needed for this wavelength-independent context check.
    for(bool hwss : {false,true}) for(bool uniform : {false,true}) {
        for(int t=0;t<4;++t) {
            std::string control=Fixture(550,hwss,false);
            const auto at=control.find("samples 128");
            control.replace(at,std::string("samples 128").size(),"samples 8");
            if(!uniform) {
                const auto seed=control.find("sms_seeding uniform");
                control.replace(seed,std::string("sms_seeding uniform").size(),"sms_seeding snell");
            }
            std::string uv=control;
            const auto mat=uv.find("perfectrefractor_material");
            uv.insert(mat,"expression_function2d\n{\n name uv_index_fn\n expr 1.78 - 0.68 * ( 1 - step( 0.000001, u*u + v*v ) )\n}\nscalar_painter\n{\n name uv_index\n function2d uv_index_fn\n}\n");
            if(objectSpace) {
                const auto begin=uv.find("expression_function2d");
                const auto end=uv.find("perfectrefractor_material",begin);
                uv.replace(begin,end-begin,"scalar_painter\n{\n name uv_index\n expression 1.78 - 0.68 * (1 - step(0.000001, dot(Po,Po)))\n}\n");
            }
            const auto ior=uv.find("ior 1.78");
            uv.replace(ior,std::string("ior 1.78").size(),"ior uv_index");
            const unsigned index=g_renderIndex;
            const auto a=Render(control,"uv_control");
            g_renderIndex=index;
            const auto b=Render(uv,"uv_index");
            Check(a.ok && b.ok && a.mean>0 && b.mean>0,"off-origin UV IOR fixtures finite and lit");
            std::cout << "context object=" << objectSpace << " hwss=" << hwss << " uniform=" << uniform << " a=" << a.mean << " b=" << b.mean << " hashes=" << a.hash << "," << b.hash << std::endl;
            Check(a.hash==b.hash,"NM material query preserves sampled context, matching constant interior IOR");
        }
    }
}

static void AttenuationControlDL435()
{
    // Cover the entire camera footprint with the caster: all source paths
    // cross it, so changing only refractance scales the expected image by
    // .25 in expectation. Throughput-dependent continuation changes paired
    // sample decisions. Both windings exercise double-sided sheet entry/exit.
    for(bool mesh : {false,true}) for(bool flipped : {false,true})
    for(bool hwss : {false,true}) for(bool uniform : {false,true}) {
        std::vector<double> ratios;
        for(int t=0;t<4;++t) {
            std::string white=Fixture(550,hwss,false,mesh);
            ReplaceFirstChunk(white,"film","film\n{\n width 16\n height 8\n}");
            white.replace(white.find("samples 128"),11,"samples 8");
            if(!uniform) white.replace(white.find("sms_seeding uniform"),19,"sms_seeding snell");
            if(mesh) ReplaceNamedChunk(white,"indexedmesh_geometry","name caster",
                std::string("indexedmesh_geometry\n{\n name caster\n")+
                " vertex -5 1 -5\n vertex 5 1 -5\n vertex 5 1 5\n vertex -5 1 5\n"
                " uv 0 0\n uv 1 0\n uv 1 1\n uv 0 1\n"+
                (flipped ? " triangle 0 2 1\n triangle 0 3 2\n" : " triangle 0 1 2\n triangle 0 2 3\n")+
                " double_sided TRUE\n face_normals TRUE\n}");
            else ReplaceNamedChunk(white,"clippedplane_geometry","name caster",
                flipped ? "clippedplane_geometry\n{\n name caster\n pta -5 1 -5\n ptb -5 1 5\n ptc 5 1 5\n ptd 5 1 -5\n doublesided TRUE\n}"
                        : "clippedplane_geometry\n{\n name caster\n pta -5 1 -5\n ptb 5 1 -5\n ptc 5 1 5\n ptd -5 1 5\n doublesided TRUE\n}");
            std::string grey=white;
            const auto mat=grey.find("perfectrefractor_material");
            grey.insert(mat,"uniformcolor_painter\n{\n name quarter\n color 0.25 0.25 0.25\n colorspace Rec709RGB_Linear\n}\n");
            grey.replace(grey.find("refractance white"),17,"refractance quarter");
            const unsigned index=g_renderIndex;
            const auto a=Render(white,"attenuation_pair");
            g_renderIndex=index;
            const auto b=Render(grey,"attenuation_pair");
            Check(a.ok && b.ok && a.mean>0 && b.mean>0,"DL-435 salted sheet fixture finite and lit");
            if(a.mean>0) ratios.push_back(b.mean/a.mean);
        }
        const auto stats=Summarize(ratios);
        std::cout << "DL-435 mesh="<<mesh<<" flipped="<<flipped<<" hwss="<<hwss<<" uniform="<<uniform
                  <<" ratio="<<stats.mean<<" render sd="<<stats.sd<<" n="<<ratios.size()<<std::endl;
        // n4 pilot: worst ratio render SD .00529804; mean SD .00264902.
        // .01 is 3.775 mean SDs; each run checks the measured precision.
        const double band=.01;
        Check(ratios.size()==4 && 3*stats.sd/2<band,"DL-435 paired attenuation band resolves three mean SDs");
        Check(std::fabs(stats.mean-.25)<band,"DL-435 wavelength attenuation scales transmitted caustic by .25");
    }
}


static void InterfaceControlDL435()
{
    // Reflection-only illumination: upward spot cannot directly light the
    // floor. A refractor's reflected lobe must ignore its transmission tint.
    // Entry-facing refractor reflection seeds are a separate DL-437
    // design question. This reachable reversed-winding control exercises
    // reflected roots; both sides/windings remain deterministic unit cases.
    const bool flipped=true;
    for(int mode=0;mode<3;++mode) {
        std::vector<double> ratios;
        for(int t=0;t<4;++t) {
            std::string white=Fixture(550,mode==2,false,true);
            ReplaceFirstChunk(white,"film","film\n{\n width 16\n height 8\n}");
            ReplaceNamedChunk(white,"indexedmesh_geometry","name caster",
                std::string("indexedmesh_geometry\n{\n name caster\n")+
                " vertex -2 1 -2\n vertex 2 1 -2\n vertex 2 1 2\n vertex -2 1 2\n"
                " uv 0 0\n uv 1 0\n uv 1 1\n uv 0 1\n"+
                (flipped ? " triangle 0 2 1\n triangle 0 3 2\n" : " triangle 0 1 2\n triangle 0 2 3\n")+
                " double_sided TRUE\n face_normals TRUE\n}\n");
            ReplaceFirstChunk(white,"omni_light","spot_light\n{\n name source\n position 0 0.5 0\n target 0 2 0\n color 1 1 1\n power 40\n inner 10\n outer 30\n}\n");
            std::ostringstream raster;
            raster<<(mode==0 ? "pathtracing_pel_rasterizer" : "pathtracing_spectral_rasterizer")
                  <<"\n{\n samples 32\n oidn_denoise FALSE\n pixel_filter box\n sms_enabled TRUE\n sms_seeding uniform\n sms_target_bounces 1\n sms_biased TRUE\n sms_multi_trials 4\n";
            if(mode!=0) raster<<" num_wavelengths 1\n spectral_samples 1\n hwss "<<(mode==2 ? "TRUE" : "FALSE")<<"\n nmbegin 550\n nmend 550.01\n";
            raster<<"}\n";
            ReplaceFirstChunk(white,"pathtracing_spectral_rasterizer",raster.str());
            std::string grey=white;
            const auto at=grey.find("perfectrefractor_material");
            grey.insert(at,"uniformcolor_painter\n{\n name quarter\n color 0.25 0.25 0.25\n colorspace Rec709RGB_Linear\n}\n");
            grey.replace(grey.find("refractance white"),17,"refractance quarter");
            const unsigned index=g_renderIndex;
            const auto a=Render(white,"reflection_pair");g_renderIndex=index;
            const auto b=Render(grey,"reflection_pair");
            Check(a.ok && b.ok && a.mean>0 && b.mean>0,"DL-435 reflection-only fixtures finite and lit");
            if(a.mean>0) ratios.push_back(b.mean/a.mean);
        }
        const auto stats=Summarize(ratios);
        std::cout<<"DL-435 native reflection mode="<<mode<<" flipped="<<flipped<<" ratio="<<stats.mean<<" render sd="<<stats.sd<<" n="<<ratios.size()<<std::endl;
        const double band=.02;
        Check(ratios.size()==4 && 3*stats.sd/2<band,"native reflection band resolves three salted mean SDs");
        Check(std::fabs(stats.mean-1)<band,"native reflected caustic ignores transmission-only tint");
    }
}


static void CoatingControlDL436()
{
    // A distant point source approaches normal incidence on the camera
    // footprint, giving a lossless single-interface
    // transmission ratio. Independent Airy normal-incidence formula; no
    // production thin-film helper is used by the oracle. A 0.0001
    // receiver albedo bounds floor/sheet feedback below 0.000101 relative
    // even for a perfectly reflecting sheet; it cannot explain a 0.1% band.
    const auto reflectance=[](double nm) {
        const double nf=std::sqrt(1.5), d=550/(4*nf);
        const double r01=(1-nf)/(1+nf), r12=(nf-1.5)/(nf+1.5);
        const double phase=4*3.14159265358979323846*nf*d/nm;
        return (r01*r01+r12*r12+2*r01*r12*std::cos(phase)) /
               (1+r01*r01*r12*r12+2*r01*r12*std::cos(phase));
    };
    for(bool flipped : {false,true}) for(int mode=0;mode<3;++mode)
    for(bool uniform : {false,true}) {
        std::vector<double> ratios;
        for(int t=0;t<4;++t) {
            std::string bare=Fixture(550,mode==2,false,true);
            ReplaceNamedChunk(bare,"uniformcolor_painter","name grey",
                "uniformcolor_painter\n{\n name grey\n color 0.0001 0.0001 0.0001\n colorspace Rec709RGB_Linear\n}\n");
            ReplaceFirstChunk(bare,"film","film\n{\n width 16\n height 8\n}\n");
            ReplaceNamedChunk(bare,"indexedmesh_geometry","name caster",
                std::string("indexedmesh_geometry\n{\n name caster\n")+
                " vertex -5 1 -5\n vertex 5 1 -5\n vertex 5 1 5\n vertex -5 1 5\n"
                " uv 0 0\n uv 1 0\n uv 1 1\n uv 0 1\n"+
                (flipped ? " triangle 0 2 1\n triangle 0 3 2\n" : " triangle 0 1 2\n triangle 0 2 3\n")+
                " double_sided TRUE\n face_normals TRUE\n}\n");
            ReplaceFirstChunk(bare,"perfectrefractor_material",
                "dielectric_material\n{\n name glass\n tau 1\n ior 1.5\n scattering 1000000\n}\n");
            ReplaceFirstChunk(bare,"omni_light",
                "omni_light\n{\n name source\n position 0 100 0\n color 1 1 1\n power 40\n}\n");
            std::ostringstream raster;
            raster<<(mode==0 ? "pathtracing_pel_rasterizer" : "pathtracing_spectral_rasterizer")
                  <<"\n{\n samples 32\n oidn_denoise FALSE\n pixel_filter box\n sms_enabled TRUE\n sms_seeding "<<(uniform ? "uniform" : "snell")
                  <<"\n sms_target_bounces 1\n sms_biased TRUE\n sms_multi_trials 4\n";
            if(mode!=0) raster<<" num_wavelengths 1\n spectral_samples 1\n hwss "<<(mode==2 ? "TRUE" : "FALSE")<<"\n nmbegin 550\n nmend 550.01\n";
            raster<<"}\n";
            ReplaceFirstChunk(bare,"pathtracing_spectral_rasterizer",raster.str());
            std::string coated=bare;
            ReplaceFirstChunk(coated,"dielectric_material",
                "dielectric_material\n{\n name glass\n tau 1\n ior 1.5\n scattering 1000000\n ar_layer 1.224744871391589 112.26827987812466 0\n}\n");
            const unsigned index=g_renderIndex;
            const auto a=Render(bare,"coating_pair");g_renderIndex=index;
            const auto b=Render(coated,"coating_pair");
            Check(a.ok && b.ok && a.mean>0 && b.mean>0,"DL-436 coated and bare controls finite and lit");
            if(a.mean>0) ratios.push_back(b.mean/a.mean);
        }
        const auto stats=Summarize(ratios);
        const double expected=mode==0 ?
            (3-reflectance(611)-reflectance(549)-reflectance(465))/(3*.96) : 1/.96;
        std::cout<<"DL-436 coating mode="<<mode<<" flipped="<<flipped<<" uniform="<<uniform<<" ratio="<<stats.mean<<" expected="<<expected<<" render sd="<<stats.sd<<" n="<<ratios.size()<<std::endl;
        // n4 pilot maximum render SD 1.26e-6 (mean SD 6.3e-7).
        // .001 also leaves room for the bounded feedback and finite angle.
        const double band=.001;
        Check(ratios.size()==4 && 3*stats.sd/2<band,"coating band resolves three salted mean SDs");
        Check(std::fabs(stats.mean-expected)<band,"native coating transmission matches independent Airy ratio");
    }
}

int main( int argc, char** argv )
{
    ConfigureTestWorker();
    if(argc>1 && std::string(argv[1])=="--coating-only") {
        CoatingControlDL436();
        std::cout<<passCount<<" passed, "<<failCount<<" failed"<<std::endl;
        return failCount ? 1 : 0;
    }
    if(argc>1 && std::string(argv[1])=="--interface-only") {
        InterfaceControlDL435();
        std::cout<<passCount<<" passed, "<<failCount<<" failed"<<std::endl;
        return failCount ? 1 : 0;
    }
    if(argc>1 && std::string(argv[1])=="--attenuation-only") {
        AttenuationControlDL435();
        std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
        return failCount ? 1 : 0;
    }
    if(argc>1 && std::string(argv[1])=="--uv-only") {
        UVIndexControl();
        UVIndexControl(true);
        std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
        return failCount ? 1 : 0;
    }
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
    AttenuationControlDL435();
    InterfaceControlDL435();
    CoatingControlDL436();
    UVIndexControl();
    UVIndexControl(true);
    g_renderIndex=0; // Retain the independently calibrated dispersion salts.
    for(bool mesh : {false,true}) {
    g_renderIndex=0; // Use the same four independently salted pairs for each geometry.
    for(bool hwss : {false,true}) for(bool dispersion : {false,true}) {
        std::vector<double> shifts;
        for(int t=0;t<4;++t) {
            const unsigned index=g_renderIndex;
            const auto blue=Render(Fixture(450,hwss,dispersion,mesh),"uniform_blue");
            g_renderIndex=index;
            const auto red=Render(Fixture(650,hwss,dispersion,mesh),"uniform_red");
            Check(blue.ok && red.ok && blue.mean>0 && red.mean>0,"uniform spectral fixture finite and lit");
            shifts.push_back(Centroid(blue)-Centroid(red));
        }
        const auto stats=Summarize(shifts);
        std::cout << "DL-353 indexedmesh=" << mesh << " hwss=" << hwss << " dispersion=" << dispersion << " centroid shift=" << stats.mean << " sd=" << stats.sd << " n=4" << std::endl;
        const double meanSD=stats.sd/2.0;
        Check(dispersion ? std::fabs(stats.mean)-0.02>3*meanSD : 0.005>3*meanSD,
            "wavelength displacement decision resolves three salted mean SDs");
        if(dispersion) Check(std::fabs(stats.mean)>0.02,"N-SF11 caustic moves with wavelength");
        else Check(std::fabs(stats.mean)<0.005,"constant-index control has no wavelength displacement");
    }
    }
    std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
    return failCount ? 1 : 0;
}
