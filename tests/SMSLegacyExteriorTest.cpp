// DL-391: use actual geometry and the native wavelength query as oracle.
#define main SMSDomainSuiteMain
#include "SMSDomainReplayTest.cpp"
#undef main
int main()
{
    LoadedScene nested(Materials(false)+Mesh(true,false)+
        "standard_object\n{\n name outer\n geometry shape\n material glass\n scale 2 2 2\n}\n"
        "sphere_geometry\n{\n name sphere\n radius 0.5\n}\n"
        "standard_object\n{\n name inner_obj\n geometry sphere\n material inner\n}\n");
    std::vector<IShaderOp*> ops; IShader* shader=nullptr;
    if(!RISE_API_CreateStandardShader(&shader,ops)) return 2;
    auto* caster=new RayCaster(false,16,*shader,true); caster->AttachScene(&nested.Scene());
    ManifoldSolverConfig config; config.biased=true;
    auto* solver=new ManifoldSolver(config);
    RandomNumberGenerator random(391); IndependentSampler sampler(random);
    for(double nm:{450.,550.,650.}) for(int location:{0,1,2}) {
        IORStack live(1);
        if(location!=0) {live.SetCurrentObject(nested.Object("outer")); live.push(1.3);}
        Point3 start(0,0,location==0?-3:location==1?-1:0);
        if(location==2) {live.SetCurrentObject(nested.Object("inner_obj"));live.push(1.2);}
        std::vector<ManifoldVertex> chain;
        solver->BuildSnellBaseSeed(start,Vector3(0,0,1),Point3(0,0,3),nested.Scene(),*caster,chain,&live,&sampler,nm);
        Check(!chain.empty(),"nested seed exists");
        if(chain.empty()) continue;
        if(location==0 && chain.size()<2) {Check(false,"nested entry topology");continue;}
        const auto outer=Hit(*nested.Object("outer"),Point3(0,0,0),Vector3(0,0,1));
        const double expected=outer.pMaterial->GetSpecularInfoNM(outer.geometric,live,nm).ior;
        const double observed=location==2?chain.front().etaT:location==1?chain.front().etaI:chain.at(1).etaI;
        std::cout<<"nm="<<nm<<" inside="<<location<<" exterior="<<observed<<" native="<<expected<<std::endl;
        Check(Near(observed,expected),"enclosing medium absent from first interface resolves at requested wavelength");
    }
    solver->release();caster->release();shader->release();
    std::cout<<passCount<<" passed, "<<failCount<<" failed\n";
    return failCount?1:0;
}
