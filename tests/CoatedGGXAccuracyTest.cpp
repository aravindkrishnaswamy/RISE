// DL-423: K4's explicit composite walk, without the residual pin.
#define main CompositeFixtureMain
#include "CompositeEnergyConservationTest.cpp"
#undef main
static FurnaceStats OffMirror(const ISPF& spf,double theta,unsigned seed) {
    auto ri=MakeIntersection(theta*kPi/180);const Point3 origin=ri.ray.origin;
    const Vector3 mirror(std::sin(theta*kPi/180),0,std::cos(theta*kPi/180));
    std::vector<double> means;
    for(unsigned batch=0;batch<8;++batch) {
        RandomNumberGenerator rng(seed+7919u*batch);IndependentSampler sampler(rng);
        IORStack stack=MakeTestIORStack(g_stub);double sum=0;
        for(unsigned draw=0;draw<20000;++draw) {
            const Point3 p(rng.CanonicalRandom()*10,rng.CanonicalRandom()*10,0);
            ri.ptIntersection=p;ri.ray.origin=Point3(origin.x+p.x,origin.y+p.y,origin.z);
            ScatteredRayContainer rays;spf.Scatter(ri,sampler,rays,stack);
            for(unsigned j=0;j<rays.Count();++j) {
                const Vector3 d=Vector3Ops::Normalize(rays[j].ray.Dir());
                if(d.z>0 && Vector3Ops::Dot(d,mirror)<std::cos(kPi/6))
                    sum+=ColorMath::MaxValue(rays[j].kray);
            }
        }
        means.push_back(sum/20000);
    }
    FurnaceStats result{};for(double x:means)result.mean+=x/8;
    for(double x:means)result.sd+=(x-result.mean)*(x-result.mean)/7;
    result.sd=std::sqrt(result.sd);result.sem=result.sd/std::sqrt(8.);return result;
}
int main(int argc,char** argv)
{
    const bool gate=argc>1 && std::string(argv[1])=="--gate";
    g_stub=new StubObject();g_stub->addref();
    Fixtures f=MakeFixtures();
    auto* delta=new UniformScalarPainter(1000000);delta->addref();
    auto* smooth=new DielectricMaterial(*f.s1,*f.s15,*delta,false);smooth->addref();
    bool failed=false;
    for(double alpha:{.05,.16,.5}) {
        auto* base=MakeSchlickGgx(0,alpha==.05?.9:.5,alpha);
        for(double sigma:{0.,.2}) {
            auto* ext=new UniformScalarPainter(sigma);ext->addref();
            auto* reference=MakeComposite(*smooth,*base,3,3,3,3,3,1,*ext);
            auto* coated=MakeCoated(*base,sigma,*f.white);
            for(double theta:{0.,45.,70.}) {
                const auto a=Furnace(*coated->GetSPF(),theta,false,false,8,20000,7101u+unsigned(theta));
                const auto b=Furnace(*reference->GetSPF(),theta,false,false,8,20000,9101u+unsigned(theta));
                if(!(a.mean>=0 && b.mean>0 && a.sem>=0 && b.sem>=0) ||
                    !std::isfinite(a.mean+b.mean+a.sem+b.sem)) {
                    std::cerr<<"FAIL: DL-423 invalid furnace sample\n";return 1;
                }
                const double band=3*std::hypot(a.sem,b.sem);
                const bool equal=std::fabs(a.mean-b.mean)<=band;
                std::cout<<std::setprecision(9)<<"DL423 alpha="<<alpha<<" sigma="<<sigma<<" theta="<<theta
                    <<" coated="<<a.mean<<" SE="<<a.sem<<" composite="<<b.mean<<" SE="<<b.sem
                    <<" ratio="<<a.mean/b.mean<<" 3sigma="<<band<<" equal="<<equal<<std::endl;
                failed|=!equal;
                if(alpha==.05 && sigma==0 && theta==70) {
                    const auto c=OffMirror(*coated->GetSPF(),theta,23101);
                    const auto r=OffMirror(*reference->GetSPF(),theta,25101);
                    if(!(c.mean>=0 && r.mean>0 && c.sem>=0 && r.sem>=0) ||
                        !std::isfinite(c.mean+r.mean+c.sem+r.sem)) {
                        std::cerr<<"FAIL: DL-423 invalid angular sample\n";return 1;
                    }
                    const double angularBand=3*std::hypot(c.sem,r.sem);
                    const bool angularEqual=std::fabs(c.mean-r.mean)<=angularBand;
                    std::cout<<"DL423 off-mirror>30deg coated="<<c.mean<<" SE="<<c.sem
                        <<" composite="<<r.mean<<" SE="<<r.sem<<" 3sigma="<<angularBand<<" equal="<<angularEqual<<std::endl;
                    failed|=!angularEqual;
                }
            }
            coated->release();reference->release();ext->release();
        }
        base->release();
    }
    smooth->release();delta->release();
    return gate&&failed?1:0;
}
