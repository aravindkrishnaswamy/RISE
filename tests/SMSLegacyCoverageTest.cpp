// DL-339 measurement: retain the row's independent SMS-off PT fixture.
// Legacy stochastic ownership is deliberately measured, not pinned as truth.
#define main WeaveGapSuiteMain
#include "WeaveGapShadowTransmittanceTest.cpp"
#undef main
int main(int argc, char** argv)
{
    const std::string mode=argc>1?argv[1]:"uniform";
    const unsigned spp=argc>2?std::strtoul(argv[2],nullptr,10):512;
    for(int kind: {0,1,2}) {
        std::vector<double> on,off;
        for(unsigned t=0;t<4;++t) {
            std::string rast=RastPTSMS(spp,true);
            const auto end=rast.find("\n}\n");
            std::string extra="\n sms_seeding "+mode+"\n";
            if(mode=="unbiased") extra="\n sms_biased FALSE\n";
            if(mode=="photon") extra="\n sms_photon_count 100000\n";
            rast.insert(end,extra);
            const unsigned salt=SobolSequence::HashCombine(339,t);
            on.push_back(RenderSalted(Assemble(rast,CasterCeilingScene(kind,false,false)),"339_on",salt));
            off.push_back(RenderSalted(Assemble(RastPTSMS(spp,false),CasterCeilingScene(kind,false,false)),"339_off",salt));
        }
        double a=0,b=0,va=0,vb=0;
        for(unsigned t=0;t<4;++t) {a+=on[t]/4;b+=off[t]/4;}
        for(unsigned t=0;t<4;++t) {va+=(on[t]-a)*(on[t]-a)/12;vb+=(off[t]-b)*(off[t]-b)/12;}
        std::cout<<"DL339 mode="<<mode<<" kind="<<kind<<" SMS="<<a<<" PT="<<b<<" ratio="<<a/b
            <<" 3sigma_abs="<<3*std::sqrt(va+vb)<<std::endl;
        if(!(b>0&&a>=0)) return 1;
    }
    return 0;
}
