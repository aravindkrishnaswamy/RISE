// DL-331: actual emitted photons, independently known nested interfaces.
#define main SMSDomainSuiteMain
#include "SMSDomainReplayTest.cpp"
#undef main
#include "../src/Library/Utilities/SMSPhotonMap.h"
int main()
{
    LoadedScene loaded(Materials(false)+
        "perfectrefractor_material\n{\n name outer_mat\n refractance white\n ior 1.4\n}\n"
        "dielectric_material\n{\n name inner_mat\n tau 1\n ior 1.5\n scattering 1000000\n}\n"
        "lambertian_material\n{\n name floor_mat\n reflectance white\n}\n"
        "box_geometry\n{\n name enclosure\n width 6\n height 6\n depth 6\n}\n"
        "standard_object\n{\n name outer\n geometry enclosure\n material outer_mat\n}\n"
        "sphere_geometry\n{\n name ball\n radius 0.5\n}\n"
        "standard_object\n{\n name inner_obj\n geometry ball\n material inner_mat\n}\n"
        "clippedplane_geometry\n{\n name floor\n pta -2 -2 1\n ptb -2 2 1\n ptc 2 2 1\n ptd 2 -2 1\n doublesided TRUE\n}\n"
        "standard_object\n{\n name receiver\n geometry floor\n material floor_mat\n}\n"
        "omni_light\n{\n name source\n position 0 0 -1.2\n color 1 1 1\n power 1\n}\n");
    SMSPhotonMap map;std::srand(331);
    Check(map.Build(loaded.Scene(),4000)>0,"actual photon seeds deposited");
    std::vector<SMSPhoton> photons;map.QuerySeeds(Point3(0,0,1),100,photons);
    auto* solver=new ManifoldSolver(ManifoldSolverConfig());unsigned checked=0;
    for(const auto& photon:photons) {
        if(photon.chainLen!=2) continue;
        bool eligible=true;
        for(unsigned i=0;i<2;++i) if(photon.chain[i].pObject!=loaded.Object("inner_obj") || (photon.chain[i].flags&2)) eligible=false;
        if(!eligible) continue;
        std::vector<ManifoldVertex> chain;
        Check(solver->ReversePhotonChainForSeed(photon,chain)==2,"photon topology reverses");
        if(chain.size()!=2) continue;
        Check(Near(chain[0].etaI,1.4)&&Near(chain[0].etaT,1.5),"receiver-to-inner entry restores 1.4/1.5");
        Check(Near(chain[1].etaI,1.5)&&Near(chain[1].etaT,1.4),"inner-to-source exit restores 1.5/1.4");
        if(++checked>=16) break;
    }
    Check(checked>=8,"enough physical nested-photon chains checked");
    solver->release();std::cout<<"nested chains="<<checked<<" "<<passCount<<" passed, "<<failCount<<" failed\n";
    return failCount?1:0;
}
