// DL-417: K8's explicit layer walk is the independent reference. Unlike
// K8's historical residual pins, --gate requires equality at three SEM.
#define main CompositeFixtureMain
#include "CompositeEnergyConservationTest.cpp"
#undef main
#include "CoatedAmbientReference.h"
int main(int argc,char** argv)
{
    const bool gate=argc>1 && std::string(argv[1])=="--gate";
    const bool greyProbe=argc>2 && std::string(argv[2])=="--grey45";
    g_stub=new StubObject();g_stub->addref();
    Fixtures f=MakeFixtures();
    auto* delta=new UniformScalarPainter(1000000);delta->addref();
    auto* smooth=new DielectricMaterial(*f.s1,*f.s15,*delta,false);smooth->addref();
    auto* sheen=new UniformColorPainter(RISEPel(1,1,1));sheen->addref();
    auto* grey=new UniformColorPainter(RISEPel(.5,.5,.5));grey->addref();
    auto* lambGrey=new LambertianMaterial(*grey);lambGrey->addref();
    auto* r3=new UniformScalarPainter(.3);r3->addref();
    auto* r7=new UniformScalarPainter(.7);r7->addref();
    auto* fabricGrey=new FabricMaterial(*lambGrey,*sheen,*r3,*f.s0);fabricGrey->addref();
    auto* fabricWhite=new FabricMaterial(*f.lamb,*sheen,*r7,*f.s0);fabricWhite->addref();
    WeaveTest::PresetWeave silk("silk",0,.5,true),denim("denim",0,.5,true);
    struct Sub {const char* name;IMaterial* material;bool lossless;};
    const Sub substrates[]={{"fabric-grey",fabricGrey,false},{"fabric-white",fabricWhite,true},
        {"silk",silk.Material(),false},{"denim",denim.Material(),false}};
    bool failed=false;
    for(const auto& sub:substrates) for(double sigma:{0.,.3}) {
        if(greyProbe && (std::string(sub.name)!="fabric-grey" || sigma!=0)) continue;
        auto* ext=new UniformScalarPainter(sigma);ext->addref();
        auto* interior=new CoatedReference::AmbientMaterial(*sub.material);interior->addref();
        auto* reference=MakeComposite(*smooth,*interior,3,3,3,3,3,1,*ext);
        auto* coated=MakeCoated(*sub.material,sigma,*f.white);
        for(double theta:{0.,45.,70.}) {
            if(greyProbe && theta!=45) continue;
            const auto a=Furnace(*coated->GetSPF(),theta,false,false,greyProbe?32:8,greyProbe?100000:20000,7201u+unsigned(theta));
            const auto b=Furnace(*reference->GetSPF(),theta,false,false,greyProbe?32:8,greyProbe?100000:20000,9201u+unsigned(theta));
            if(!(a.mean>=0 && b.mean>0 && a.sem>=0 && b.sem>=0) ||
                    !std::isfinite(a.mean+b.mean+a.sem+b.sem)) {
                    std::cerr<<"FAIL: DL-417 invalid furnace sample\n";return 1;
                }
                const double band=3*std::hypot(a.sem,b.sem);
            const bool equal=std::fabs(a.mean-b.mean)<=band;
            const bool conservative=!sub.lossless || a.mean<=1+3*a.sem;
            std::cout<<std::setprecision(9)<<"DL417 "<<sub.name<<" sigma="<<sigma<<" theta="<<theta
                <<" coated="<<a.mean<<" SE="<<a.sem<<" composite_fixed_ambient="<<b.mean<<" SE="<<b.sem
                <<" ratio="<<a.mean/b.mean<<" 3sigma="<<band<<" equal="<<equal<<" conservative="<<conservative<<std::endl;
            failed|=!equal||!conservative;
        }
        coated->release();reference->release();interior->release();ext->release();
    }
    fabricGrey->release();fabricWhite->release();r3->release();r7->release();
    lambGrey->release();grey->release();sheen->release();smooth->release();delta->release();
    return gate&&failed?1:0;
}
