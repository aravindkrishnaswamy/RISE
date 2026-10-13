// DL-382: baseline and fix-forward must produce identical frozen composite pixels.
#include "SMSRenderTestSupport.h"
int main(int argc,char** argv) {
    const auto scene=ReadScene("tests/fixtures/dl382/holedcomposite.RISEscene");
    Check(!scene.empty(),"frozen composite fixture exists");
    g_seedBase=382616; g_renderIndex=0;
    // Cover forwarded glass SPFs as well as a two-scattering-layer composite.
    std::vector<std::string> scenes{scene};
    for(const std::string& pair : {std::string("top glass\n bottom none"),std::string("top none\n bottom glass")}) {
        std::string single=scene;
        single.replace(single.find("top glass\n bottom glass"),std::string("top glass\n bottom glass").size(),pair);
        scenes.push_back(single);
    }
    std::string wrapped=scenes[1];
    const auto object=wrapped.find("standard_object",wrapped.find("composite_material"));
    const std::string wrapper="lambertian_luminaire_material\n{\n name wrapped_comp\n material frozen_comp\n exitance gw\n scale 0\n}\n";
    wrapped.insert(object,wrapper);
    wrapped.replace(wrapped.find("material frozen_comp",object+wrapper.size()),std::string("material frozen_comp").size(),"material wrapped_comp");
    scenes.push_back(wrapped);
    unsigned long long combined=1469598103934665603ULL;
    for(size_t i=0;i<scenes.size();++i) {
        const std::string tag="dl382_frozen_"+std::to_string(i);
        const auto image=Render(scenes[i],tag.c_str());
        Check(image.ok && image.mean>0,"frozen composite variant renders finite positive pixels");
        std::cout<<"frozen variant="<<i<<" mean="<<image.mean<<" pixels="<<image.hash<<"\n";
        combined=(combined^image.hash)*1099511628211ULL;
    }
    std::cout<<"frozen composite hash="<<combined<<"\n";
    // Pass the paired baseline hash to gate exact pixels on this platform.
    if(argc>1) Check(combined==std::strtoull(argv[1],nullptr,10),"frozen composite pixels exactly match pre-merge baseline");
    std::cout<<passCount<<" passed, "<<failCount<<" failed\n";
    return failCount?1:0;
}
