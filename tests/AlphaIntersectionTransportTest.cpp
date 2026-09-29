#include "AlphaTransportFixture.h"

int main()
{
    const std::string rasters[] = { RastPT(1024), RastBDPT(1024), RastVCM(1024),
        "mlt_rasterizer\n{\n mutations_per_pixel 1024\n bootstrap_samples 20000\n chains 8\n pixel_filter box\n oidn_denoise FALSE\n}\n" + std::string(kOutputChunk) };
    const char* labels[] = { "PT", "BDPT", "VCM", "MLT" };
    for (unsigned r=0; r<4; ++r) {
        double base=Render(Assemble(rasters[r], ReceiverScene(kOmni,false,0,kWide)), labels[r]);
        Check(base>0, std::string(labels[r])+" lit-floor control renders");
        std::cout << labels[r] << " clear-floor " << base << std::endl;
        const double glow = 0.01/3.14159265358979323846;
        Check(std::abs(base-glow-kRho/3.14159265358979323846)<0.01, std::string(labels[r])+" control matches rho/pi");
        for(double transmission : {0.5,0.7}) {
            double value=Render(Assemble(rasters[r], ReceiverScene(kOmni,true,transmission,kWide)), labels[r]);
            std::cout << labels[r] << " transmission " << transmission << " measured " << (value-glow)/(base-glow) << std::endl;
            Check(value>=0 && base>0 && std::abs((value-glow)/(base-glow)-transmission)<0.035,
                std::string(labels[r])+" alpha irradiance matches closed form");
        }
    }


    for (unsigned r=0;r<3;++r) {
        std::string scene=ReceiverScene(kArea,false,0,kWide);
        const double base=Render(Assemble(rasters[r],scene),"area emitter control");
        const auto at=scene.find("name mat_emit\n");
        scene.insert(at+std::string("name mat_emit\n").size()," alpha_mode blend\n alpha_coverage 0.3\n");
        const double alpha=Render(Assemble(rasters[r],scene),"alpha area emitter");
        const double glow=.01/3.14159265358979323846;
        const double ratio=(alpha-glow)/(base-glow);
        std::cout<<labels[r]<<" emitter coverage ratio="<<ratio<<std::endl;
        Check(base>glow+.01,std::string(labels[r])+" area emitter control lights receiver");
        Check(std::fabs(ratio-.3)<.035,std::string(labels[r])+" emitter endpoint coverage counted once across MIS strategies");
    }
    for (const char* kind : {"pathtracing_spectral_rasterizer","bdpt_spectral_rasterizer","vcm_spectral_rasterizer"}) {
        for(bool hwss : {false,true}) {
            const std::string raster=std::string(kind)+"\n{\n samples 1024\n hwss "+(hwss?"true":"false")+"\n pixel_filter box\n oidn_denoise FALSE\n}\n"+kOutputChunk;
            const double base=Render(Assemble(raster,ReceiverScene(kOmni,false,0,kWide)),kind);
            const double alpha=Render(Assemble(raster,ReceiverScene(kOmni,true,.7,kWide)),kind);
            const double ratio=alpha/base;
            // Glow is only 2% of the unobstructed floor and travels the same
            // spectral conversion; the expected ratio including it is known.
            const double expected=(.7*kRho+.01)/(kRho+.01);
            std::cout<<kind<<" hwss="<<hwss<<" ratio="<<ratio<<std::endl;
            Check(base>0,std::string(kind)+" finite spectral control");
            Check(std::fabs(ratio-expected)<.035,std::string(kind)+" spectral alpha has wavelength-independent coverage");
        }
    }

    // A delta mirror cannot receive NEE. The emitter is reached only by a
    // BSDF continuation, crossing the sheet after the first camera hit.
    // Absorbing global medium prices the original full segment after skips.
    for(unsigned r=0;r<3;++r)for(bool medium:{false,true}) {
        auto body=[&](bool sheet){
            std::string scene=ReceiverScene(kArea,sheet,.7,kWide);
            auto rep=[&](const std::string& a,const std::string& b){const auto at=scene.find(a);if(at!=std::string::npos)scene.replace(at,a.size(),b);};
            rep("lambertian_material\n{\n\tname mat_recv_base","perfectreflector_material\n{\n\tname mat_recv_base");
            rep("color 0.01 0.01 0.01","color 0 0 0");
            const auto emitter=scene.find("name geo_emit");
            if(emitter!=std::string::npos){
                std::string tail=scene.substr(emitter);size_t pos=0;
                while((pos=tail.find("0.25",pos))!=std::string::npos){tail.replace(pos,4,"8");++pos;}
                scene.replace(emitter,scene.size()-emitter,tail);
            }
            if(medium)scene+="homogeneous_medium\n{\n name absorber\n absorption 0.15 0.15 0.15\n scattering 0 0 0\n phase hg 0\n}\nglobal_medium\n{\n medium absorber\n}\n";
            return scene;
        };
        const double base=Render(Assemble(rasters[r],body(false)),"mirror continuation control");
        const double alpha=Render(Assemble(rasters[r],body(true)),"mirror alpha continuation");
        std::cout<<labels[r]<<" continuation medium="<<medium<<" ratio="<<alpha/base<<std::endl;
        Check(base>.01,std::string(labels[r])+" specular continuation reaches emitter");
        Check(std::fabs(alpha/base-.7)<.035,std::string(labels[r])+" skipped-alpha continuation retains full medium segment");
    }
    // VM-only has no NEE/VC source and zero glow. A positive control proves
    // the photon merge strategy actually contributes. Coverage at both eye
    // and photon endpoints must leave one (not two) factors of 0.5.
    for (unsigned mode=0;mode<3;++mode) {
        std::string rast=RastVCM(1024);
        auto replace=[](std::string& text,const std::string& a,const std::string& b) {
            const auto at=text.find(a); if(at!=std::string::npos)text.replace(at,a.size(),b);
        };
        replace(rast,"merge_radius 0.0","merge_radius 0.20");
        if(mode==0)replace(rast,"vc_enabled true","vc_enabled false");
        if(mode==1)replace(rast,"vm_enabled true","vm_enabled false");
        std::string scene=ReceiverScene(kSpot,false,0,kWide);
        replace(scene,"color 0.01 0.01 0.01","color 0 0 0");
        replace(scene,"inner 1.5","inner 20"); replace(scene,"outer 2.0","outer 25");
        const double opaque=Render(Assemble(rast,scene),"VCM coverage control");
        replace(scene,"name mat_recv\n","name mat_recv\n alpha_mode blend\n alpha_coverage 0.5\n");
        const double alpha=Render(Assemble(rast,scene),"VCM coverage receiver");
        const char* modeName=mode==0?"VM-only":mode==1?"VC-only":"VC+VM";
        std::cout<<modeName<<" opaque="<<opaque<<" alpha="<<alpha<<" ratio="<<alpha/opaque<<std::endl;
        Check(opaque>.05,std::string(modeName)+" nonzero lit control (VM is active)");
        Check(std::fabs(alpha/opaque-.5)<.04,std::string(modeName)+" receiver coverage has one factor");
    }
    std::cout << passCount << " passed / " << failCount << " failed" << std::endl;
    return failCount ? 1 : 0;
}
