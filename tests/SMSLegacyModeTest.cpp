// Build-independent hashes and timing for the extended-mode-off contract.
// Run the same --trial value after each interleaved master/candidate build.
#include "SMSRenderTestSupport.h"
#include <iomanip>
#include <sstream>

int main(int argc,char** argv)
{
    Check(ConfigureTestWorker(),"single-worker options configured");
    unsigned first=0, count=4;
    if(argc==3 && std::string(argv[1])=="--trial") {
        first=static_cast<unsigned>(std::stoul(argv[2])); count=1;
    }
    const char* paths[]={"scenes/Tests/SMS/sms_k1_refract.RISEscene",
        "scenes/Tests/SMS/sms_k2_glasssphere.RISEscene",
        "scenes/Tests/Spectral/spectral_dispersive_caustic_pt_sms_uniform.RISEscene"};
    for(unsigned fixture=0;fixture<4;++fixture) {
        std::string scene=ReadScene(paths[std::min(fixture,2u)]);
        ReplaceFirstChunk(scene,"film","film\n{\n width 64\n height 64\n}\n");
        // Keep the shipped SMS configuration and material domain; bound
        // only the image/sample budget and disable denoising for raw hashes.
        const std::string type=fixture<2?"pathtracing_pel_rasterizer":"pathtracing_spectral_rasterizer";
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
        scene.replace(begin,end-begin+1,raster);
        std::vector<double> seconds;
        for(unsigned trial=first;trial<first+count;++trial) {
            g_renderIndex=trial;
            const auto start=std::chrono::steady_clock::now();
            const auto result=Render(scene,"sms_legacy_mode");
            const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
            Check(result.ok && result.pixels.size()==4096 && result.mean>0,"shipped legacy SMS finite and lit");
            seconds.push_back(elapsed);
            std::cout<<std::setprecision(17)<<"LEGACY fixture="<<fixture<<" trial="<<trial
                <<" hash="<<result.hash<<" mean="<<result.mean<<" seconds="<<elapsed<<std::endl;
        }
        const auto stats=Summarize(seconds);
        std::cout<<"LEGACY timing fixture="<<fixture<<" mean="<<stats.mean<<" sd="<<stats.sd<<" n="<<count<<std::endl;
    }
    std::cout<<passCount<<" passed, "<<failCount<<" failed"<<std::endl;
    return failCount?1:0;
}
