// DL-408: paired contact/gap means and the absolute lossless Lambertian
// control. Measurement by default; --gate exposes the unfixed regression.
#define main SSSExteriorSuiteMain
#include "SSSExteriorIndexInvarianceTest.cpp"
#undef main
int main(int argc,char** argv) {
    const bool gate=argc>1 && std::string(argv[1])=="--gate";
    bool failed=false;
    for(int kind:{0,1,2}) for(bool reverse:{false,true}) {
        std::vector<double> contact,gap;
        for(unsigned salt=0;salt<4;++salt) for(bool separated:{false,true}) {
            auto scene=BuildTouchingPairScene(Model::RandomWalk,2,separated?2e-6:0,256);
            if(kind!=0) {
                const auto begin=scene.find("randomwalk_sss_material\n{\n\tname block_right");
                if(begin==std::string::npos) return 1;
                const auto closing=scene.find("}\n",begin);
                if(closing==std::string::npos) return 1;
                const auto end=closing+2;
                const std::string neighbor=kind==1
                    ? "lambertian_material\n{\n name block_right\n reflectance white\n}\n"
                    : "subsurfacescattering_material\n{\n name block_right\n ior 1.5\n absorption 0\n scattering 20\n g 0\n roughness 0\n}\n";
                scene.replace(begin,end-begin,neighbor);
            }
            if(reverse) {
                const auto left=scene.find("\tmaterial block\n");
                if(left==std::string::npos) return 1;
                scene.replace(left,16,"\tmaterial swap\n");
                const auto right=scene.find("\tmaterial block_right\n");
                if(right==std::string::npos) return 1;
                scene.replace(right,22,"\tmaterial block\n");
                const auto swapped=scene.find("\tmaterial swap\n");
                if(swapped==std::string::npos) return 1;
                scene.replace(swapped,15,"\tmaterial block_right\n");
            }
            const auto path=WriteScene(scene,"408_contact");
            const double mean=RenderFurnaceMean(path,49000+salt);
            std::remove(path.c_str());
            if(!(mean>0) || !std::isfinite(mean)) return 1;
            (separated?gap:contact).push_back(mean);
        }
        const auto a=Summarize(contact),b=Summarize(gap);
        const double seA=a.sd/2,seB=b.sd/2,band=3*std::hypot(seA,seB);
        const bool ratioOK=std::fabs(a.mean-b.mean)<=band;
        const bool absoluteOK=kind!=1 || (std::fabs(a.mean-1)<=3*seA && std::fabs(b.mean-1)<=3*seB);
        std::cout<<std::setprecision(9)<<"DL408 kind="<<kind<<" reverse="<<reverse<<" contact="<<a.mean<<" SE="<<seA<<" gap="<<b.mean<<" SE="<<seB<<" ratio="<<a.mean/b.mean
            <<" combined3sigma="<<band<<" reciprocal="<<ratioOK<<" Lambertian_applicable="<<(kind==1)<<" Lambertian_furnace="<<absoluteOK<<std::endl;
        if(gate && !(ratioOK&&absoluteOK)) failed=true;
    }
    return failed?1:0;
}
