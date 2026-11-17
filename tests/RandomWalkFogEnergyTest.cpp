// DL-416: a subsurface NEE shadow leg must traverse the live exterior medium.
// Reuse the DL-375 spot fixture and capture/mask helpers. The absorption-only
// control has a BLACK wall. Every contribution to the sphere must travel at
// least (3 - 1.15) in the fog on the camera leg and |light-center| - radius on
// the light leg. Thus Beer-Lambert bounds its ratio to vacuum independently
// of the SSS solution, MIS, and all three integrators.
#define main ExistingBDPTBalanceMain
#include "BDPTStrategyBalanceTest.cpp"
#undef main

static std::string FogBody(const std::string& model, const std::string& medium, bool black)
{
    std::string body = kSceneDeltaLitRandomWalkDL375;
    if(black) { const auto c=body.find("color 0.5 0.5 0.5"); body.replace(c,17,"color 0 0 0"); }
    if(model=="diffusion") {
        const auto b=body.find("randomwalk_sss_material"), e=body.find("}\n\n",b)+3;
        body.replace(b,e-b,"subsurfacescattering_material\n{\n name mat_rw\n ior 1.3\n absorption 0.1\n scattering 10\n g 0\n roughness 0.3\n}\n\n");
    }
    body += kLightSpotDL375;
    body += "homogeneous_medium\n{\n name fog\n absorption ";
    body += medium=="vacuum" ? "0 0 0\n scattering 0 0 0" :
        medium=="absorb" ? "0.3 0.3 0.3\n scattering 0 0 0" : "0.05 0.05 0.05\n scattering 0.3 0.3 0.3";
    body += "\n phase isotropic\n}\ndielectric_material\n{\n name shell\n tau 1 1 1\n ior 1\n scattering 1000000\n}\nbox_geometry\n{\n name fogbox\n width 6\n height 6\n depth 6\n}\nstandard_object\n{\n name enclosure\n geometry fogbox\n material shell\n interior_medium fog\n}\n";
    return body;
}

struct FogStats { double mean=0, sd=0; };
static bool FogMeans(const std::string& model, const std::string& medium, bool black,
    int mode, int spp, FogStats (&stats)[2])
{
    std::string raster=SSSRasterizer(mode ? "bdpt" : "pt",16,spp);
    if(mode==2) {
        raster.replace(raster.find("bdpt_pel_rasterizer"),19,"vcm_pel_rasterizer");
        raster.insert(raster.find("\tmax_eye_depth"),"\tmerge_radius 0\n\tvm_enabled FALSE\n");
    }
    raster.insert(raster.find("\tpixel_filter box"),"\tmax_volume_bounce 64\n");
    double sum[2]={}, sq[2]={};
    for(unsigned i=0;i<4;++i) {
        double s=0,wall=0;
        if(!RenderRegionMeansBodyDL375(raster,FogBody(model,medium,black),41600+i,s,wall)) return false;
        const double v[2]={s,wall};
        for(int r=0;r<2;++r) { sum[r]+=v[r]; sq[r]+=v[r]*v[r]; }
    }
    for(int r=0;r<2;++r) {
        stats[r].mean=sum[r]/4;
        stats[r].sd=std::sqrt(std::fmax(0.0,(sq[r]-4*stats[r].mean*stats[r].mean)/3));
        std::cout << model << " " << medium << " mode=" << mode << " region=" << r
                  << " mean=" << stats[r].mean << " sd=" << stats[r].sd << std::endl;
    }
    return true;
}
static void BeerBound()
{
    const double minimumDistance=3.0-1.15+std::sqrt(1.8*1.8+0.05*0.05+0.9*0.9)-0.55;
    const double upper=std::exp(-0.3*minimumDistance);
    for(int mode=0;mode<3;++mode) {
        FogStats vacuum[2], absorb[2];
        const bool ok=FogMeans("rw","vacuum",true,mode,512,vacuum)
            && FogMeans("rw","absorb",true,mode,512,absorb);
        Check(ok,"DL-416 absorption-only renders");
        if(!ok) continue;
        const double band=3*std::hypot(absorb[0].sd,upper*vacuum[0].sd);
        std::cout << "Beer ratio mode=" << mode << " " << absorb[0].mean/vacuum[0].mean
                  << " closed-form upper=" << upper << " band=" << band << std::endl;
        Check(absorb[0].mean<=upper*vacuum[0].mean+band,"DL-416 exact Beer-Lambert upper bound");
    }
}
static void FogParity(const std::string& model)
{
    FogStats reference[2];
    const bool ok=FogMeans(model,"fog",false,1,512,reference);
    Check(ok,"DL-416 independent BDPT fog reference");
    if(!ok) return;
    for(int mode : {0,2}) {
        FogStats measured[2];
        const bool rendered=FogMeans(model,"fog",false,mode,512,measured);
        Check(rendered,"DL-416 fog renders");
        if(!rendered) continue;
        for(int r=0;r<2;++r) {
            const double band=3*std::hypot(reference[r].sd,measured[r].sd);
            std::cout << "Fog ratio " << model << " mode=" << mode << " region=" << r
                      << " " << measured[r].mean/reference[r].mean << " band=" << band << std::endl;
            Check(std::fabs(measured[r].mean-reference[r].mean)<=band,
                "DL-416 fog agrees within three pooled render deviations");
        }
    }
}

int main(int argc,char** argv)
{
 const std::string only=argc>1 ? argv[1] : "";
 if(only.empty() || only=="--beer-only") BeerBound();
 if(only.empty() || only=="--fog-only") FogParity("rw");
 if(only.empty() || only=="--diffusion-only") FogParity("diffusion");
 std::cout << "Passed: " << passCount << "\nFailed: " << failCount << std::endl;
 return failCount ? 1 : 0;
}
