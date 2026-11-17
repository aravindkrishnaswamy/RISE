// DL-466: broad slab versus a receiver patch separated from corner
// coalescence and root-existence boundaries. Every render has a ValueSalt.
// Default: 128 salts on the off-critical patch with a test-only retry budget
// of 4096. Args: salts, narrow/broad/both, budget (0 uses production 100),
// optional starting salt index (for disjoint repeat blocks).
// `128 narrow 100` exposes the unresolved tail; do not loosen its band.
#define main SupersededRowsMain
#include "SMSSupersededRowsTest.cpp"
#undef main
#include "SMSPlanarSlabReference.h"
#include "../src/Library/Rendering/PathTracingPelRasterizer.h"
#include "../src/Library/Shaders/PathTracingIntegrator.h"
// A pointer-to-member formed in a derived class may be applied to the actual
// base object; this reads the protected integrator without a downcast to a
// fictional subclass and without changing the library's interface.
struct RasterizerDiagnosticsAccess : PathTracingPelRasterizer {
    static auto IntegratorMember() { return &RasterizerDiagnosticsAccess::pIntegrator; }
};
struct SolverDiagnosticsAccess : ManifoldSolver {
    static auto ConfigMember() { return &SolverDiagnosticsAccess::config; }
};
static bool diagnosticsAttached=false;
static SMSReferenceCounters* counters=nullptr;
static unsigned probeBudget=0;
static void AttachCounters(IJobPriv* job) {
    if(auto* raster=dynamic_cast<PathTracingPelRasterizer*>(job->GetRasterizer())) {
        auto* integrator=raster->*RasterizerDiagnosticsAccess::IntegratorMember();
        if(integrator && integrator->GetSolver()) {
            auto& config=integrator->GetSolver()->*SolverDiagnosticsAccess::ConfigMember();
            config.referenceCounters=counters;
            if(probeBudget) config.maxBernoulliTrials=probeBudget;
            diagnosticsAttached=true;
        }
    }
}
int main(int argc,char** argv) {
    const unsigned n=argc>1?std::max(4,std::atoi(argv[1])):128;
    const std::string scope=argc>2?argv[2]:"narrow";
    if(scope!="narrow" && scope!="broad" && scope!="both") return 1;
    probeBudget=argc>3?std::strtoul(argv[3],nullptr,10):4096;
    const unsigned saltOffset=argc>4?std::strtoul(argv[4],nullptr,10):0;
    for(bool narrow:{false,true}) {
        if((scope=="narrow" && !narrow) || (scope=="broad" && narrow)) continue;
        const double cx=narrow?.5:0,cy=narrow?.15:0,half=narrow?.04:2;
        const auto truth=SMSPlanarSlabReference::Closed(400,.5,0,cx,cy,half);
        const auto refined=SMSPlanarSlabReference::Closed(800,.5,0,cx,cy,half);
        Check(std::fabs(truth[0]-.6300666)<2e-6 || narrow,"DL-466 independent k2 reference calibration");
        Check(std::fabs(truth[1]-.0779853)<2e-6 || narrow,"DL-466 independent k3 reference calibration");
        Check(std::fabs(truth[2]-refined[2])<2e-6,"DL-466 reference refinement");
        SMSReferenceCounters diagnostics; counters=&diagnostics; g_beforeSMSRender=&AttachCounters;
        const auto margins=SMSPlanarSlabReference::CornerMargins(200,.5,0,cx,cy,half);
        std::cout<<"DL466 sampled corner margins narrow="<<narrow<<" order="<<margins[0]<<" existence="<<margins[1]<<std::endl;
        if(narrow) Check(margins[0]>1e-3 && margins[1]>1e-3,"DL-466 probe excludes sampled order/existence boundaries");
        Series values;
        for(unsigned salt=0;salt<n;++salt) {
            g_renderIndex=salt+saltOffset;
            std::string scene=SlabScene(kOmni,SlabRaster("extd:4",64));
            if(narrow) {
                const std::string old="location 0 0 -1.9\n lookat 0 0 -2\n up 0 1 0\n fov 174.275189547777";
                if(scene.find(old)==std::string::npos) return 1;
                scene.replace(scene.find(old),old.size(),"location 0.5 0.15 -1.9\n lookat 0.5 0.15 -2\n up 0 1 0\n fov 43.60281897270362");
            }
            diagnosticsAttached=false;
            const auto image=Render(scene,"466_fourvertex");
            Check(diagnosticsAttached,"DL-466 diagnostics installed before render");
            Check(image.ok&&image.mean>=0,"DL-466 finite render");
            values.v.push_back(image.mean);
            std::cout<<std::setprecision(10)<<"DL466 sample narrow="<<narrow<<" salt="<<salt+saltOffset<<" mean="<<image.mean<<std::endl;
            if((salt+1)%32==0) std::cout<<"DL466 prefix narrow="<<narrow<<" n="<<salt+1<<" mean="<<values.mean()<<" SE="<<values.se()<<" truth="<<refined[2]<<std::endl;
        }
        g_beforeSMSRender=nullptr;counters=nullptr;
        std::cout<<"DL466 retry histogram narrow="<<narrow<<" trials="<<diagnostics.retryTrials<<" tail="<<diagnostics.tailTrials<<" stops="<<diagnostics.rouletteStops;
        for(unsigned b=0;b<32;++b) if(diagnostics.retryHistogram[b]) std::cout<<" [2^"<<b<<"]="<<diagnostics.retryHistogram[b];
        std::cout<<std::endl;
        auto sorted=values.v;std::sort(sorted.begin(),sorted.end());
        double m3=0,total=0;for(double x:sorted){m3+=std::pow(x-values.mean(),3)/n;total+=x;}
        const double sd=values.se()*std::sqrt(double(n));
        std::cout<<std::setprecision(10)<<"DL466 narrow="<<narrow<<" budget="<<(probeBudget?probeBudget:100)<<" n="<<n<<" mean="<<values.mean()<<" SE="<<values.se()<<" truth="<<refined[2]
            <<" sd="<<sd<<" saltOffset="<<saltOffset
            <<" ratio="<<values.mean()/refined[2]
            <<" nominalRatio3SE=["<<(values.mean()-3*values.se())/refined[2]<<","<<(values.mean()+3*values.se())/refined[2]<<"]"<<" skew="<<(sd>0?m3/std::pow(sd,3):0)<<" max/mean="<<sorted.back()/values.mean()<<" largest_fraction="<<sorted.back()/total<<std::endl;
        // Broad-domain variance is the debt under investigation; do not gate
        // its finite-sample mean or widen a band to make it pass.
        if(narrow) Check(std::fabs(values.mean()-refined[2])<=3*values.se(),"DL-466 off-critical patch equals independent closed form at 3 sigma");
    }
    std::cout<<passCount<<" passed, "<<failCount<<" failed\n";
    return failCount?1:0;
}
