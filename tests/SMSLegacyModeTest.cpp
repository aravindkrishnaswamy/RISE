// Cross-build float32 ULP comparisons, exact hashes, diagnostics and timing.
// Adopted 2026-10-04; within-build rejection tests retain HashPixels doubles.
// Run the same --trial value after each interleaved master/candidate build.
//
// Fixtures 7-9 (sms-ext Phase 4 review) are legacy HWSS + SMS scenes through
// the HWSS paths Phase 4 edited.  Like 0-6 they are verified ONLY by the
// two-build dump comparison (RISE_SMS_LEGACY_DUMP_DIR on the base build,
// RISE_SMS_LEGACY_REFERENCE_DIR on the candidate); a single run asserts
// only that they render lit.  The negative control below shows those
// fixtures would see the change: the SAME fixture rendered by a directly
// constructed HWSS rasterizer with extended mode ON differs from it with
// extended mode OFF (and the OFF render repeats bit-identically).
#include "SMSRenderTestSupport.h"
#include "../src/Library/Rendering/PathTracingSpectralRasterizer.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Utilities/ManifoldSolver.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/ISampling2D.h"
#include "../src/Library/Interfaces/IPixelFilter.h"
#include "../src/Library/RISE_API.h"
#include <iomanip>
#include <sstream>

static unsigned long long Float32PixelHash(const std::vector<RISEColor>& pixels)
{
    static_assert(sizeof(float)==4, "cross-build contract requires float32");
    unsigned long long h=1469598103934665603ULL;
    for(const RISEColor& c:pixels) {
        const float v[4]={static_cast<float>(c.base.r),static_cast<float>(c.base.g),
            static_cast<float>(c.base.b),static_cast<float>(c.a)};
        const auto* bytes=reinterpret_cast<const unsigned char*>(v);
        for(std::size_t k=0;k<sizeof(v);++k) { h^=bytes[k]; h*=1099511628211ULL; }
    }
    return h;
}

static unsigned long long Float32ULPDistance(float a, float b)
{
    unsigned int x=0,y=0;std::memcpy(&x,&a,4);std::memcpy(&y,&b,4);
    // Reject NaN/Inf by representation even under fast-math.
    if((x&0x7f800000u)==0x7f800000u || (y&0x7f800000u)==0x7f800000u)
        return std::numeric_limits<unsigned long long>::max();
    const auto ordered=[](unsigned int bits)->unsigned long long {
        return (bits&0x80000000u)?0x80000000ULL-(bits&0x7fffffffu):0x80000000ULL+bits;
    };
    const auto u=ordered(x),v=ordered(y);return u>v?u-v:v-u;
}
static void CheckCrossBuildPixels(const std::vector<RISEColor>& pixels,unsigned fixture,unsigned trial)
{
    const std::string name="fixture-"+std::to_string(fixture)+"-trial-"+std::to_string(trial)+".f32";
    std::vector<float> values;values.reserve(pixels.size()*4);
    for(const auto& c:pixels) {values.push_back(static_cast<float>(c.base.r));values.push_back(static_cast<float>(c.base.g));values.push_back(static_cast<float>(c.base.b));values.push_back(static_cast<float>(c.a));}
    if(const char* directory=std::getenv("RISE_SMS_LEGACY_DUMP_DIR")) {
        std::ofstream out(std::string(directory)+"/"+name,std::ios::binary);
        out.write(reinterpret_cast<const char*>(values.data()),values.size()*sizeof(float));
        Check(bool(out),"cross-build float32 pixel dump is complete");
    }
    if(const char* directory=std::getenv("RISE_SMS_LEGACY_REFERENCE_DIR")) {
        std::ifstream input(std::string(directory)+"/"+name,std::ios::binary);
        std::vector<float> reference(values.size());input.read(reinterpret_cast<char*>(reference.data()),reference.size()*sizeof(float));
        const bool complete=bool(input)&&input.peek()==std::char_traits<char>::eof();
        Check(complete,"cross-build reference has exactly the expected RGBA pixels");
        unsigned long long maximum=0;std::size_t changed=0;
        if(complete) for(std::size_t i=0;i<values.size();++i) {
            const auto distance=Float32ULPDistance(reference[i],values[i]);maximum=std::max(maximum,distance);if(distance)++changed;
        }
        std::cout<<"LEGACY precision fixture="<<fixture<<" trial="<<trial<<" max_float32_ulps="<<maximum<<" changed_components="<<changed<<std::endl;
        Check(complete&&maximum<=1,"cross-build mode-off RGBA differs by at most one float32 ULP");
    }
}

// Phase 4 review: mode-off scenes through the HWSS paths Phase 4 edited
// (legacy HWSS + SMS, extended mode OFF as shipped): a polished delta-coat
// caster whose emitter hit is handed to the NM body (`material none`
// luminaire, fixture 7) or shaded in the HWSS body (luminaire with a BSDF,
// fixture 8), and an SSS receiver under a glass slab (fixture 9: the
// camera-entry SSS fallback).
static std::string Phase4HWSSScene(unsigned int fixture)
{
    std::string s="RISE ASCII SCENE 7\n"
        "standard_shader\n{\n name global\n shaderop DefaultPathTracing\n}\n"
        "film\n{\n width 64\n height 64\n}\n"
        "pinhole_camera\n{\n location 0.6 0 3.2\n lookat 0.6 0 0\n up 0 1 0\n fov 40\n}\n"
        "pathtracing_spectral_rasterizer\n{\n samples 64\n pixel_filter box\n oidn_denoise FALSE\n"
        " nmbegin 380\n nmend 720\n num_wavelengths 8\n spectral_samples 1\n hwss TRUE\n sms_enabled TRUE\n}\n"
        "uniformcolor_painter\n{\n name white\n color 1 1 1\n}\n"
        "uniformcolor_painter\n{\n name black\n color 0 0 0\n}\n"
        "lambertian_material\n{\n name diffuse\n reflectance white\n}\n"
        "lambertian_luminaire_material\n{\n name lum\n exitance white\n scale 10\n material none\n}\n"
        "lambertian_luminaire_material\n{\n name lumbsdf\n exitance white\n scale 10\n material diffuse\n}\n"
        "clippedplane_geometry\n{\n name floor_geo\n pta -3 -3 0\n ptb -3 3 0\n ptc 3 3 0\n ptd 3 -3 0\n doublesided TRUE\n}\n";
    if(fixture<9) {
        s+="polished_material\n{\n name polished\n reflectance black\n tau 1.0\n ior 1.5\n scattering 1000000\n}\n"
            "standard_object\n{\n name floor\n geometry floor_geo\n material diffuse\n}\n"
            "clippedplane_geometry\n{\n name caster_geo\n pta 0.4 -2 2\n ptb 0.4 2 2\n ptc 4 2 2\n ptd 4 -2 2\n doublesided TRUE\n}\n"
            "standard_object\n{\n name caster\n geometry caster_geo\n material polished\n}\n"
            "clippedplane_geometry\n{\n name emitter_geo\n pta 2 0.5 1\n ptb 2 -0.5 1\n ptc 1 -0.5 1\n ptd 1 0.5 1\n doublesided FALSE\n}\n"
            "standard_object\n{\n name emitter\n geometry emitter_geo\n material "+std::string(fixture==7?"lum":"lumbsdf")+"\n}\n";
    } else {
        s+="randomwalk_sss_material\n{\n name rw\n ior 1.3\n absorption 0.8 0.4 0.04\n scattering 3 3.5 4\n g 0\n roughness 0.8\n max_bounces 64\n}\n"
            "perfectrefractor_material\n{\n name glass\n refractance white\n ior 1.5\n}\n"
            "standard_object\n{\n name floor\n geometry floor_geo\n material rw\n}\n"
            "box_geometry\n{\n name slab_geo\n width 3\n height 3\n depth 0.5\n}\n"
            "standard_object\n{\n name caster\n geometry slab_geo\n material glass\n position 0 0 2\n}\n"
            "clippedplane_geometry\n{\n name emitter_geo\n pta -0.6 -0.6 3.5\n ptb -0.6 0.6 3.5\n ptc 0.6 0.6 3.5\n ptd 0.6 -0.6 3.5\n doublesided TRUE\n}\n"
            "standard_object\n{\n name emitter\n geometry emitter_geo\n material lum\n}\n";
    }
    return s;
}

// Directly constructed PT spectral HWSS rasterizer (extended mode is an
// internal ManifoldSolverConfig field, not parser-exposed), 16 spp,
// one salt.  Returns the float32 RGBA pixels, empty on failure.
static std::vector<float> RenderDirectHWSS(const std::string& text,bool extended)
{
    std::vector<float> out;
    if(!ConfigureTestWorker()) return out;
    const std::string path=TestTempPath("sms_legacy_neg_"+std::to_string(::getpid())+".RISEscene");
    {std::ofstream f(path);f<<text;}
    IJobPriv* job=nullptr;
    if(!RISE_CreateJobPriv(&job)||!job||!job->LoadAsciiSceneViaCst(path.c_str())) {std::remove(path.c_str());safe_release(job);return out;}
    std::remove(path.c_str());
    const IScene& scene=*job->GetScene();
    scene.GetObjects()->PrepareForRendering();
    std::vector<IShaderOp*> ops;IShader* shader=nullptr;
    if(!RISE_API_CreateStandardShader(&shader,ops)) {safe_release(job);return out;}
    auto* caster=new RayCaster(false,16,*shader,true);caster->AttachScene(&scene);
    ManifoldSolverConfig cfg;cfg.enabled=true;cfg.extendedMode=extended;
    StabilityConfig stability;
    auto* rasterizer=new PathTracingSpectralRasterizer(caster,380,720,8,1,cfg,AdaptiveSamplingConfig(),stability,false,true);
    ISampling2D* samples=nullptr;IPixelFilter* filter=nullptr;
    RISE_API_CreateMultiJitteredSampling2D(&samples,1,1);RISE_API_CreateBoxPixelFilter(&filter,1,1);
    if(samples&&filter) {
        samples->SetNumSamples(16);rasterizer->SubSampleRays(samples,filter);
        auto* capture=new CapturingRasterizerOutput();rasterizer->AddRasterizerOutput(capture);
        rasterizer->AttachToScene(&scene);
        std::srand(4242);GlobalRNG()=RandomNumberGenerator(4242);
        SobolSamplerTestHooks::ValueSalt().store(0x4E454731u);
        rasterizer->RasterizeScene(scene,nullptr,nullptr);
        SobolSamplerTestHooks::ValueSalt().store(0);
        for(const auto& c:capture->pixels) {out.push_back(float(c.base.r));out.push_back(float(c.base.g));out.push_back(float(c.base.b));out.push_back(float(c.a));}
        rasterizer->DetachFromScene(&scene);capture->release();
    }
    safe_release(samples);safe_release(filter);rasterizer->release();caster->release();shader->release();safe_release(job);
    return out;
}

int main(int argc,char** argv)
{
    Check(ConfigureTestWorker(),"single-worker options configured");
    const float one=1, next=std::nextafter(one,2.f), second=std::nextafter(next,2.f);
    Check(Float32ULPDistance(one,one)==0,"ULP comparator preserves equality");
    Check(Float32ULPDistance(one,next)==1 && Float32ULPDistance(one,second)==2,"ULP comparator accepts one step and detects two steps");
    Check(Float32ULPDistance(-one,-next)==1,"ULP comparator orders negative values");
    Check(Float32ULPDistance(-0.f,0.f)==0,"signed zero has zero numeric ULP distance");
    Check(Float32ULPDistance(one,std::numeric_limits<float>::infinity())>1
        && Float32ULPDistance(one,std::numeric_limits<float>::quiet_NaN())>1,"ULP comparator rejects nonfinite pixels");
    unsigned first=0, count=4;
    if(argc==3 && std::string(argv[1])=="--trial") {
        first=static_cast<unsigned>(std::stoul(argv[2])); count=1;
    }
    const char* paths[]={"scenes/Tests/SMS/sms_k1_refract.RISEscene",
        "scenes/Tests/SMS/sms_k2_glasssphere.RISEscene",
        "scenes/Tests/Spectral/spectral_dispersive_caustic_pt_sms_uniform.RISEscene"};
    for(unsigned fixture=0;fixture<10;++fixture) {
        if(fixture>=7) {
            const std::string scene=Phase4HWSSScene(fixture);
            std::vector<double> seconds;
            for(unsigned trial=first;trial<first+count;++trial) {
                g_renderIndex=trial;
                const auto start=std::chrono::steady_clock::now();
                const auto result=Render(scene,"sms_legacy_mode_p4");
                const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
                Check(result.ok && result.pixels.size()==4096 && result.mean>0,"Phase 4 legacy HWSS fixture finite and lit");
                seconds.push_back(elapsed);
                std::cout<<std::setprecision(17)<<"LEGACY fixture="<<fixture<<" trial="<<trial
                    <<" hash="<<result.hash<<" hash_float32="<<Float32PixelHash(result.pixels)<<" mean="<<result.mean<<" seconds="<<elapsed<<std::endl;
                CheckCrossBuildPixels(result.pixels,fixture,trial);
            }
            const auto stats=Summarize(seconds);
            std::cout<<"LEGACY timing fixture="<<fixture<<" mean="<<stats.mean<<" sd="<<stats.sd<<" n="<<count<<std::endl;
            continue;
        }
        std::string scene=ReadScene(paths[fixture>=4?0:std::min(fixture,2u)]);
        ReplaceFirstChunk(scene,"film","film\n{\n width 64\n height 64\n}\n");
        // Keep the shipped SMS configuration and material domain; bound
        // only the image/sample budget and disable denoising for raw hashes.
        const std::string type=(fixture<2||fixture>=4)?"pathtracing_pel_rasterizer":"pathtracing_spectral_rasterizer";
        const auto begin=scene.find(type), end=scene.find('}',begin);
        Check(begin!=std::string::npos && end!=std::string::npos,"shipped rasterizer found");
        if(begin==std::string::npos || end==std::string::npos) continue;
        std::string raster=scene.substr(begin,end-begin+1);
        // CST rejects duplicate descriptors. Replace existing values by
        // their complete line, or insert an absent descriptor before '}'.
        const auto replace=[&raster](const std::string& key,const std::string& value) {
            std::istringstream input(raster); std::string line, output; bool found=false;
            while(std::getline(input,line)) {
                std::istringstream tokens(line); std::string token; tokens>>token;
                if(token==key) { line=" "+key+" "+value; found=true; }
                output+=line+'\n';
            }
            raster=output;
            if(!found) raster.insert(raster.find('}')," "+key+" "+value+"\n");
        };
        replace("samples","64"); replace("oidn_denoise","FALSE"); replace("pixel_filter","box");
        if(fixture>=2) replace("hwss",fixture==3?"TRUE":"FALSE");
        if(fixture>=4) {
            // Exercise the changed default shader-dispatch path while SMS is
            // off. Keep its actual native advanced-shader composition, camera,
            // materials and lights identical in the interleaved builds.
            raster="pixelpel_rasterizer\n{\n max_recursion 8\n samples 64\n lum_samples 1\n pixel_filter box\n oidn_denoise FALSE\n}\n";
        }
        scene.replace(begin,end-begin+1,raster);
        if(fixture==4) ReplaceFirstChunk(scene,"standard_shader","advanced_shader\n{\n name global\n shaderop DefaultPathTracing 0 100 =\n shaderop DefaultDirectLighting 0 100 +\n}\n");
        if(fixture>=5) {
            // Both native cached SSS implementations participate in the
            // default-mode cost and arithmetic comparison, including build
            // and reuse. Capture ordinary direct irradiance to avoid a
            // recursive dependency on the cache being constructed.
            const std::string op=fixture==5?"simple_sss_shaderop":"donner_jensen_skin_sss_shaderop";
            ReplaceFirstChunk(scene,"standard_shader",
                "standard_shader\n{\n name cached_capture\n shaderop DefaultDirectLighting\n}\n"
                +op+"\n{\n name cached_sss\n numpoints 16\n maxpointspernode 2\n maxdepth 8\n shader cached_capture\n}\n"
                "advanced_shader\n{\n name global\n shaderop cached_sss 0 100 =\n}\n");
        }
        std::vector<double> seconds;
        for(unsigned trial=first;trial<first+count;++trial) {
            g_renderIndex=trial;
            const auto start=std::chrono::steady_clock::now();
            const auto result=Render(scene,"sms_legacy_mode");
            const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
            Check(result.ok && result.pixels.size()==4096 && result.mean>0,"shipped legacy SMS finite and lit");
            seconds.push_back(elapsed);
            std::cout<<std::setprecision(17)<<"LEGACY fixture="<<fixture<<" trial="<<trial
                <<" hash="<<result.hash<<" hash_float32="<<Float32PixelHash(result.pixels)<<" mean="<<result.mean<<" seconds="<<elapsed<<std::endl;
            CheckCrossBuildPixels(result.pixels,fixture,trial);
        }
        const auto stats=Summarize(seconds);
        std::cout<<"LEGACY timing fixture="<<fixture<<" mean="<<stats.mean<<" sd="<<stats.sd<<" n="<<count<<std::endl;
    }
    // Negative control for fixtures 7-9 (see the header).
    for(unsigned fixture:{7u,9u}) {
        const std::string scene=Phase4HWSSScene(fixture);
        const auto off=RenderDirectHWSS(scene,false), again=RenderDirectHWSS(scene,false), on=RenderDirectHWSS(scene,true);
        bool complete=!off.empty()&&off.size()==again.size()&&off.size()==on.size();
        unsigned long long repeat=0,changed=0,maxULP=0;
        if(complete) for(std::size_t i=0;i<off.size();++i) {
            if(Float32ULPDistance(off[i],again[i])) ++repeat;
            const auto d=Float32ULPDistance(off[i],on[i]);
            if(d>1) ++changed;
            maxULP=std::max(maxULP,d);
        }
        std::cout<<"LEGACY negative control fixture="<<fixture<<" extended-off repeat changed="<<repeat
            <<" extended-on vs off components > 1 ULP="<<changed<<" of "<<off.size()<<" max_ulps="<<maxULP<<std::endl;
        Check(complete&&repeat==0,"negative control: extended-off direct HWSS render repeats bit-identically");
        Check(complete&&changed>0,"negative control: extended-on output differs from extended-off on the Phase 4 fixture");
    }
    std::cout<<passCount<<" passed, "<<failCount<<" failed"<<std::endl;
    return failCount?1:0;
}
