#define main ContinuityFixtureMain
#include "../tests/CompositeEnergyConservationTest.cpp"
#undef main

static RayIntersectionGeometric MakeRI( const Vector3& view /*toward viewer, z>0*/ )
{
	const Vector3 inDir = -view;
	const Ray inRay( Point3( view.x, view.y, view.z ), inDir );
	const RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( inRay, rs );
	ri.bHit = true; ri.range = 1.0;
	ri.ptIntersection = Point3( 0, 0, 0 );
	ri.vNormal = Vector3( 0, 0, 1 ); ri.vGeomNormal = Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
	ri.ptCoord = Point2( 0.5, 0.5 );
	return ri;
}
static Vector3 Dir( double thDeg, double phDeg )
{
	const double t = thDeg * kPi / 180, p = phDeg * kPi / 180;
	return Vector3( std::sin(t)*std::cos(p), std::sin(t)*std::sin(p), std::cos(t) );
}


// DL-423 redo diagnostic. Tests the restored parent without changing its model.
// Integrated albedo uses a common fixed importance proposal for every parameter:
// its quadrature estimate is for continuity only, not an accuracy proof.
#include <array>
using Metrics=std::array<double,4>;
struct Pose { double view, light, phi; };
struct Cell { double alpha,f0,eta; };
struct WeightedDirection { Vector3 d; double weight; };
static int checks=0, failures=0;
static std::vector<WeightedDirection> Proposal(const Pose&pose,const Fixtures&f) {
    auto*g=MakeSchlickGgx(0,.5,.05);auto*c=MakeCoated(*g,0,*f.white,1.5);
    auto ri=MakeRI(Dir(pose.view,0));IORStack stack=MakeTestIORStack(g_stub);
    RandomNumberGenerator rng(128971);IndependentSampler sampler(rng);
    std::vector<WeightedDirection> points;
    for(unsigned i=0;i<256;++i){ScatteredRayContainer rays;c->GetSPF()->Scatter(ri,sampler,rays,stack);
        for(unsigned j=0;j<rays.Count();++j){const auto d=rays[j].ray.Dir();
            const double p=c->GetSPF()->Pdf(ri,d,stack);
            if(d.z>0&&p>0)points.push_back({d,d.z/(p*256)});
        }
    }
    c->release();g->release();return points;
}
static Metrics Eval(const Cell&cell,const Pose&pose,const Fixtures&f,const std::vector<WeightedDirection>&points){
    auto*g=MakeSchlickGgx(0,cell.f0,cell.alpha);auto*c=MakeCoated(*g,0,*f.white,cell.eta);
    auto ri=MakeRI(Dir(pose.view,0));const auto wo=Dir(pose.light,pose.phi);
    IORStack stack=MakeTestIORStack(g_stub);
    Metrics a{{c->GetBSDF()->value(wo,ri)[0],c->GetSPF()->Pdf(ri,wo,stack),c->GetBSDF()->albedo(ri)[0],0}};
    for(const auto&p:points)a[3]+=c->GetBSDF()->value(p.d,ri)[0]*p.weight;
    c->release();g->release();return a;
}
static Cell Shift(Cell c,unsigned p,double h){
    if(p==0)c.alpha=std::min(1.,std::max(1e-4,c.alpha+h));
    if(p==1)c.f0=std::min(1.,std::max(0.,c.f0+h));
    if(p==2)c.eta=std::max(1.,c.eta+h);
    return c;
}
static const char* names[]={"value","Pdf","AOV","integrated-albedo"};
static void Shrink(const Cell&c,const Pose&pose,const Fixtures&f,const std::vector<WeightedDirection>&pts){
    const auto center=Eval(c,pose,f,pts);
    for(unsigned p=0;p<3;++p){std::array<Metrics,3>d;
        for(unsigned j=0;j<3;++j){const double h=std::pow(10.,-3.-j);const auto lo=Eval(Shift(c,p,-h),pose,f,pts),hi=Eval(Shift(c,p,h),pose,f,pts);
            for(unsigned k=0;k<4;++k){if(!(std::isfinite(lo[k]+hi[k]+center[k])&&lo[k]>=0&&hi[k]>=0&&center[k]>=0)){++failures;std::cout<<"FAIL invalid metric\n";}
                d[j][k]=std::max({lo[k],hi[k],center[k]})<=1e-12?0:std::abs(hi[k]-lo[k])/std::max(center[k],1e-12);
            }
        }
        for(unsigned k=0;k<4;++k)for(unsigned j=0;j<2;++j){++checks;
            if(d[j+1][k]>1e-12&&d[j+1][k]>.2*std::max(d[j][k],1e-12)){++failures;
                std::cout<<"FAIL shrink metric="<<names[k]<<" p="<<p<<" alpha="<<c.alpha<<" F0="<<c.f0<<" eta="<<c.eta<<" view="<<pose.view<<" light="<<pose.light<<" phi="<<pose.phi<<" h-index="<<j<<" d="<<d[j][k]<<","<<d[j+1][k]<<" fine-d="<<d[2][k]<<" fine-ratio="<<d[2][k]/std::max(d[1][k],1e-12)<<"\n";
            }
        }
    }
}
static void Sweep(unsigned p,const Pose&pose,const Fixtures&f,const std::vector<WeightedDirection>&pts){
    std::vector<Metrics> vals;const double start=p==0?.002:.02,end=p==0?1.:.98;
    const unsigned n=unsigned(std::round((end-start)/.001))+1;
    for(unsigned i=0;i<n;++i){Cell c{.05,.9,1.5};if(p==0)c.alpha=start+.001*i;else c.f0=start+.001*i;vals.push_back(Eval(c,pose,f,pts));}
    for(unsigned k=0;k<4;++k){std::vector<double>steps(n-1);for(unsigned i=0;i<n-1;++i){double scale=std::max(std::abs(vals[i][k]),std::abs(vals[i+1][k]));steps[i]=scale<=1e-12?0:std::abs(vals[i+1][k]-vals[i][k])/scale;}
        for(unsigned i=0;i<n-1;++i){std::vector<double> local;const unsigned lo=i>2?i-2:0,hi=std::min(n-2,i+2);
            for(unsigned j=lo;j<=hi;++j)if(j!=i)local.push_back(steps[j]);std::sort(local.begin(),local.end());
            const double median=local.size()%2?local[local.size()/2]:(local[local.size()/2-1]+local[local.size()/2])*.5;
            ++checks;if(steps[i]>1e-12&&steps[i]>10*std::max(median,1e-12)){++failures;std::cout<<"FAIL sweep metric="<<names[k]<<" p="<<p<<" parameter="<<start+.001*i<<" view="<<pose.view<<" light="<<pose.light<<" phi="<<pose.phi<<" step="<<steps[i]<<" median="<<median<<"\n";}
        }
    }
}
int main(){
    std::cout<<std::setprecision(12);g_stub=new StubObject();g_stub->addref();Fixtures f=MakeFixtures();
    const Pose poses[]={{0,0,180},{45,43,180},{70,68,180},{80,78,180},{85,83,180},{89,87,180},{70,45,150},{85,20,90}};
    for(const auto&pose:poses){const auto pts=Proposal(pose,f);
        for(double alpha:{.002,.02,.05,.16,.3,1.})for(double f0:{.02,.04,.199,.2,.201,.5,.9,.98})for(double eta:{1.3,1.5,1.7})Shrink({alpha,f0,eta},pose,f,pts);
        Sweep(0,pose,f,pts);Sweep(1,pose,f,pts);
    }
    std::cout<<"CoatedParameterContinuityProbe: "<<checks-failures<<" passed, "<<failures<<" failed\n";
    return failures?1:0;
}
