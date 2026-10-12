// DL-382: baseline and fix-forward must produce identical frozen composite pixels.
#include "SMSRenderTestSupport.h"
int main(int argc,char** argv) {
    const auto scene=ReadScene("tests/fixtures/dl382/holedcomposite.RISEscene");
    Check(!scene.empty(),"frozen composite fixture exists");
    g_seedBase=382616; g_renderIndex=0;
    const auto result=Render(scene,"dl382_frozen");
    Check(result.ok && result.mean>0,"frozen composite renders finite positive pixels");
    std::cout<<"frozen composite mean="<<result.mean<<" hash="<<result.hash<<"\n";
    // Pass the paired baseline hash to gate exact pixels on this platform.
    if(argc>1) Check(result.hash==std::strtoull(argv[1],nullptr,10),"frozen composite pixels exactly match pre-merge baseline");
    std::cout<<passCount<<" passed, "<<failCount<<" failed\n";
    return failCount?1:0;
}
