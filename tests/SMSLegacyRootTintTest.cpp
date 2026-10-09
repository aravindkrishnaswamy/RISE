// DL-398: final-root tint against a native material query at that root.
#define main SMSDomainSuiteMain
#include "SMSDomainReplayTest.cpp"
#undef main
int main()
{
    LoadedScene loaded(Materials(false)+
        "expression_function2d\n{\n name tint_fn\n expr 0.2 + 0.3 * (u + v)\n}\n"
        "function2d_painter\n{\n name tint\n function2d tint_fn\n}\n"
        "perfectrefractor_material\n{\n name tinted\n refractance tint\n ior 1.5\n}\n"
        "clippedplane_geometry\n{\n name shape\n pta -2 -2 0\n ptb -2 2 0\n ptc 2 2 0\n ptd 2 -2 0\n doublesided TRUE\n}\n"
        "standard_object\n{\n name caster\n geometry shape\n material tinted\n}\n");
    std::vector<IShaderOp*> ops; IShader* shader=nullptr;
    if(!RISE_API_CreateStandardShader(&shader,ops)) return 2;
    auto* caster=new RayCaster(false,16,*shader,true); caster->AttachScene(&loaded.Scene());
    ManifoldSolverConfig config;config.biased=true;config.maxIterations=40;config.solverThreshold=1e-10;
    auto* solver=new ManifoldSolver(config);
    RandomNumberGenerator rng(398);IndependentSampler sampler(rng);IORStack air(1);
    const Point3 start(0,0,-2),end(1,0,2);
    std::vector<ManifoldVertex> chain;
    solver->BuildSnellBaseSeed(start,Vector3(0,0,1),end,loaded.Scene(),*caster,chain,&air,&sampler,550);
    const auto result=solver->Solve(start,Vector3(0,0,1),end,Vector3(0,0,-1),chain,sampler);
    Check(result.valid,"textured plane root converges");
    if(result.valid) {
        const auto& v=result.specularChain.front();
        const Vector3 incoming=Vector3Ops::Normalize(Vector3Ops::mkVector3(v.position,start));
        const auto hit=Hit(*loaded.Object("caster"),start,incoming);
        const auto native=hit.pMaterial->GetSpecularInfo(hit.geometric,air);
        const auto spectral=hit.pMaterial->GetSpecularInfoNM(hit.geometric,air,550);
        const double factor=(1-Optics::CalculateDielectricReflectanceCosine(std::fabs(incoming.z),1,1.5))/2.25;
        for(unsigned c=0;c<3;++c) Check(Near(result.contribution[c],native.attenuation[c]*factor),"RGB final root uses native spatial tint");
        const double nm=solver->EvaluateChainThroughputNM(start,end,result.specularChain,550);
        Check(Near(nm,spectral.attenuationNM*factor),"NM final root uses native spatial tint");
        std::cout<<"seedUV="<<chain.front().uv.x<<" rootUV="<<v.uv.x<<" RGB="<<result.contribution[0]<<" expected="<<native.attenuation[0]*factor
            <<" NM="<<nm<<" expectedNM="<<spectral.attenuationNM*factor<<std::endl;
    }
    solver->release();caster->release();shader->release();
    std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
}
