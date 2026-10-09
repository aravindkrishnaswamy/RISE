// DL-339: native closed-ceiling fixtures, independent SMS-off PT reference.
#define SMS_COVERAGE_NO_MAIN
#include "SMSLegacyCoverageTest.cpp"
#include <array>
int main() {
    char executable[]="SMSUniformReflectionCoverageTest", mode[]="uniform", spp[]="256", gate[]="gate", scope[]="first-reflection";
    char* args[]={executable,mode,spp,gate,scope};
    bool failed=RunLegacyCoverageMeasurement(5,args)!=0;
    // Check NM and the HWSS body's own emitter hits, with matched nw160.
    for(bool hwss:{false,true}) for(int kind:{0,2}) {
        std::array<std::vector<double>,3> on,off;
        for(unsigned t=0;t<4;++t) for(bool enabled:{true,false}) {
            std::string raster=RastPTSpectralSMS(256,hwss,enabled);
            const std::string old="num_wavelengths 8";
            raster.replace(raster.find(old),old.size(),"num_wavelengths 160");
            if(enabled) raster.insert(raster.find("\n}\n"),"\n sms_seeding uniform\n");
            const double luminance=RenderSalted(Assemble(raster,CasterCeilingScene(kind,false,false)),
                "339_spectral",SobolSequence::HashCombine(339,t));
            if(!(luminance>=0) || g_lastPixels.empty()) return 1;
            for(unsigned c=0;c<3;++c) {
                double sum=0;for(const auto& pixel:g_lastPixels) sum+=pixel.base[c]*pixel.a;
                (enabled?on:off)[c].push_back(sum/g_lastPixels.size());
            }
        }
        for(unsigned c=0;c<3;++c) {
            auto stats=[](const std::vector<double>& v) {
                double m=0,s=0;for(double x:v)m+=x/v.size();
                for(double x:v)s+=(x-m)*(x-m)/(v.size()*(v.size()-1));
                return std::make_pair(m,std::sqrt(s));
            };
            const auto a=stats(on[c]),b=stats(off[c]);
            const double band=3*std::hypot(a.second,b.second);
            std::cout<<"DL339 spectral hwss="<<hwss<<" kind="<<kind<<" channel="<<c
                <<" SMS="<<a.first<<" PT="<<b.first<<" ratio="<<a.first/b.first<<" 3sigma_abs="<<band<<std::endl;
            if(!(b.first>0)) return 1;
            // DL-339: RW-SSS returns through its BSSRDF, so the ceiling
            // fixture isolates the first surface reflection. Dielectric
            // includes later internal reflections and remains measured.
            if(kind==0 && std::fabs(a.first-b.first)>band) {
                std::cerr<<"FAIL: DL-339 spectral uncovered first reflection\n";failed=true;
            }
        }
    }
    return failed?1:0;
}
