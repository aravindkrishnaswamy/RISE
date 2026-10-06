// Cross-build float32 ULP comparisons, exact hashes, diagnostics and timing.
// Adopted 2026-10-04; within-build rejection tests retain HashPixels doubles.
// Run the same --trial value after each interleaved master/candidate build.
#include "SMSRenderTestSupport.h"
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
    for(unsigned fixture=0;fixture<7;++fixture) {
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
    std::cout<<passCount<<" passed, "<<failCount<<" failed"<<std::endl;
    return failCount?1:0;
}
