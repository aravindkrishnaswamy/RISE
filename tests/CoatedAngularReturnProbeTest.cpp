// DL-417 diagnostic: solve the full discretized angular return operator,
// without replacing the substrate distribution by a cosine reservoir.
#define main CompositeFixtureMain
#include "CompositeEnergyConservationTest.cpp"
#undef main
static double Fresnel(double mu,double ni,double nt) {
    const double st2=(ni/nt)*(ni/nt)*(1-mu*mu);
    if(st2>=1)return 1;
    const double ct=std::sqrt(1-st2);
    const double rs=(ni*mu-nt*ct)/(ni*mu+nt*ct);
    const double rp=(nt*mu-ni*ct)/(nt*mu+ni*ct);
    return (rs*rs+rp*rp)/2;
}
struct Node {Vector3 direction;double mu,weight;};
static bool Probe(const char* name,const IMaterial& material,unsigned polar,unsigned azimuth) {
    const double eta=1.5,critical=std::sqrt(1-1/(eta*eta));
    std::vector<Node> nodes;
    for(unsigned side=0;side<2;++side) {
        const double lo=side?critical:0,hi=side?1:critical;
        for(unsigned u=0;u<polar;++u) for(unsigned a=0;a<azimuth;++a) {
            const double mu=lo+(hi-lo)*(u+.5)/polar,phi=2*kPi*(a+.5)/azimuth;
            const double r=std::sqrt(1-mu*mu);
            nodes.push_back({Vector3(r*std::cos(phi),r*std::sin(phi),mu),mu,mu*(hi-lo)/polar*2*kPi/azimuth});
        }
    }
    const size_t n=nodes.size();std::vector<double> kernel(n*n);
    // Directions point away from the substrate. A coat reflection keeps
    // the travel tangent, so the next incident/view direction has the
    // opposite tangent. This pi rotation matters for anisotropic fibres.
    auto returnIndex=[&](size_t j) {return (j/azimuth)*azimuth+(j%azimuth+azimuth/2)%azimuth;};
    IORStack inner(eta);
    for(size_t i=0;i<n;++i) {
        auto ri=MakeIntersection(0);ri.ambientIOR=eta;ri.ray.SetDir(-nodes[i].direction);
        for(size_t j=0;j<n;++j) {
            const double value=material.GetBSDF()->valueStateful(nodes[j].direction,ri,&inner)[0]*nodes[j].weight;
            if(!(value>=0) || !std::isfinite(value)) {
                std::cerr<<"FAIL: invalid angular kernel for "<<name<<std::endl;return false;
            }
            kernel[i*n+j]=value;
        }
    }
    for(double sigma:{0.,.3}) {
        std::vector<double> escape(n),returned(n),h(n,0),next(n);
        for(size_t j=0;j<n;++j) {
            const double f=Fresnel(nodes[j].mu,eta,1),a=std::exp(-sigma/nodes[j].mu);
            escape[j]=(1-f)*a;returned[j]=f*a*a;
        }
        double residual=0;unsigned iteration=0;
        for(;iteration<1000;++iteration) {
            residual=0;
            for(size_t i=0;i<n;++i) {
                double value=0;for(size_t j=0;j<n;++j)value+=kernel[i*n+j]*(escape[j]+returned[j]*h[returnIndex(j)]);
                next[i]=value;residual=std::max(residual,std::fabs(value-h[i]));
            }
            h.swap(next);if(residual<1e-10)break;
        }
        if(iteration==1000 || !std::isfinite(residual)) {
            std::cerr<<"FAIL: angular iteration did not converge for "<<name<<std::endl;return false;
        }
        for(double theta:{0.,45.,70.}) {
            const double outside=std::cos(theta*kPi/180),mu=std::sqrt(1-(1-outside*outside)/(eta*eta));
            auto ri=MakeIntersection(std::acos(mu));ri.ambientIOR=eta;
            double escaped=0;
            for(size_t j=0;j<n;++j) escaped+=material.GetBSDF()->valueStateful(nodes[j].direction,ri,&inner)[0]*nodes[j].weight*(escape[j]+returned[j]*h[returnIndex(j)]);
            const double f=Fresnel(outside,1,eta);
            const double rho=f+(1-f)*std::exp(-sigma/mu)*escaped;
            if(!(rho>=0) || !std::isfinite(rho)) return false;
            std::cout<<std::setprecision(9)<<"DL417 angular name="<<name<<" grid="<<polar<<"x"<<azimuth<<" sigma="<<sigma<<" theta="<<theta<<" rho="<<rho<<" iterations="<<iteration<<" residual="<<residual<<std::endl;
        }
    }
    return true;
}
// An independent native-SPF walk separates substrate sampler/evaluator
// differences from errors in the discretized return operator. Deposit the
// escape at each encounter and roulette only the continuation after event 8.
static bool WalkProbe(const char* name,const IMaterial& material) {
    constexpr double eta=1.5;
    for(double sigma:{0.,.3}) for(double theta:{0.,45.,70.}) {
        const double angle=theta*kPi/180,outside=std::cos(angle);
        const double mu=std::sqrt(1-std::pow(std::sin(angle)/eta,2));
        const double f=Fresnel(outside,1,eta);
        std::vector<double> means;
        for(unsigned batch=0;batch<8;++batch) {
            RandomNumberGenerator rng(17301u+7919u*batch+unsigned(theta));
            IndependentSampler sampler(rng);double sum=0;
            for(unsigned sample=0;sample<20000;++sample) {
                auto ri=MakeIntersection(std::acos(mu));ri.ambientIOR=eta;
                IORStack inner(eta);inner.SetCurrentObject(g_stub);
                double weight=(1-f)*std::exp(-sigma/mu),value=f;
                unsigned event=0;
                for(;event<10000 && weight>0;++event) {
                    ScatteredRayContainer rays;material.GetSPF()->Scatter(ri,sampler,rays,inner);
                    double total=0;
                    for(unsigned j=0;j<rays.Count();++j) if(rays[j].ray.Dir().z>0)
                        total+=ColorMath::MaxValue(rays[j].kray);
                    if(!(total>=0) || !std::isfinite(total)) return false;
                    if(total==0) break;
                    double pick=sampler.Get1D()*total;unsigned selected=rays.Count();
                    for(unsigned j=0;j<rays.Count();++j) if(rays[j].ray.Dir().z>0) {
                        pick-=ColorMath::MaxValue(rays[j].kray);
                        selected=j;if(pick<=0)break;
                    }
                    if(selected==rays.Count()) return false;
                    const Vector3 direction=Vector3Ops::Normalize(rays[selected].ray.Dir());
                    const double pass=std::exp(-sigma/direction.z);
                    const double returned=Fresnel(direction.z,eta,1);
                    weight*=total;
                    value+=weight*(1-returned)*pass;
                    weight*=returned*pass*pass;
                    if(event>=8 && weight>0) {
                        const double q=std::min(.95,weight);
                        if(sampler.Get1D()>=q)break;
                        weight/=q;
                    }
                    ri.ray.SetDir(Vector3(direction.x,direction.y,-direction.z));
                }
                if(event==10000 || !(value>=0) || !std::isfinite(value)) return false;
                sum+=value;
            }
            means.push_back(sum/20000);
        }
        double mean=0,variance=0;for(double x:means)mean+=x/8;
        for(double x:means)variance+=(x-mean)*(x-mean)/(8*7);
        std::cout<<std::setprecision(9)<<"DL417 native-walk name="<<name<<" sigma="<<sigma
            <<" theta="<<theta<<" rho="<<mean<<" SE="<<std::sqrt(variance)<<std::endl;
    }
    return true;
}
int main(int argc,char** argv) {
    const bool walk=argc>2 && std::string(argv[2])=="--walk";
    const unsigned resolution=argc>1?std::min(48,std::max(4,std::atoi(argv[1]))):12;
    g_stub=new StubObject();g_stub->addref();Fixtures f=MakeFixtures();
    auto* sheen=new UniformColorPainter(RISEPel(1,1,1));sheen->addref();
    auto* rough=new UniformScalarPainter(.7);rough->addref();
    auto* fabric=new FabricMaterial(*f.lamb,*sheen,*rough,*f.s0);fabric->addref();
    auto* grey=new UniformColorPainter(RISEPel(.5,.5,.5));grey->addref();
    auto* lambGrey=new LambertianMaterial(*grey);lambGrey->addref();
    auto* roughGrey=new UniformScalarPainter(.3);roughGrey->addref();
    auto* fabricGrey=new FabricMaterial(*lambGrey,*sheen,*roughGrey,*f.s0);fabricGrey->addref();
    WeaveTest::PresetWeave silk("silk",0,.5,true),denim("denim",0,.5,true);
    bool valid=Probe("fabric-white",*fabric,resolution,2*resolution);
    valid&=Probe("fabric-grey",*fabricGrey,resolution,2*resolution);
    valid&=Probe("silk",*silk.Material(),resolution,2*resolution);
    valid&=Probe("denim",*denim.Material(),resolution,2*resolution);
    if(walk) {
        valid&=WalkProbe("fabric-white",*fabric);
        valid&=WalkProbe("fabric-grey",*fabricGrey);
        valid&=WalkProbe("silk",*silk.Material());
        valid&=WalkProbe("denim",*denim.Material());
    }
    fabricGrey->release();roughGrey->release();lambGrey->release();grey->release();
    fabric->release();rough->release();sheen->release();return valid?0:1;
}
