#define main CompositeFixtureMain
#include "../../../tests/CompositeEnergyConservationTest.cpp"
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


static unsigned long long Hash(unsigned long long h,const double*v,unsigned n){for(unsigned j=0;j<n;++j){auto*b=reinterpret_cast<const unsigned char*>(&v[j]);for(unsigned i=0;i<sizeof(double);++i){h^=b[i];h*=1099511628211ULL;}}return h;}
int main(){g_stub=new StubObject();g_stub->addref();Fixtures f=MakeFixtures();auto*sig=new UniformScalarPainter(.8);sig->addref();auto*oren=new OrenNayarMaterial(*f.white,*sig);oren->addref();
 for(auto*base:{static_cast<IMaterial*>(f.lamb),static_cast<IMaterial*>(oren)}){unsigned long long h=1469598103934665603ULL;
  for(double eta:{1.3,1.5,1.7})for(double tau:{0.,.2}){auto*c=MakeCoated(*base,tau,*f.white,eta);IORStack stack=MakeTestIORStack(g_stub);
   for(double theta:{0.,45.,70.,85.}){auto ri=MakeRI(Dir(theta,0));auto a=c->GetBSDF()->albedo(ri);double av[]={a[0],a[1],a[2]};h=Hash(h,av,3);
    for(double th:{0.,20.,60.,88.})for(double phi:{0.,30.,90.,180.}){auto d=Dir(th,phi);auto v=c->GetBSDF()->value(d,ri);double vals[]={v[0],v[1],v[2],c->GetSPF()->Pdf(ri,d,stack)};h=Hash(h,vals,4);}
    RandomNumberGenerator rng(4271);IndependentSampler sampler(rng);for(unsigned i=0;i<512;++i){ScatteredRayContainer rays;c->GetSPF()->Scatter(ri,sampler,rays,stack);for(unsigned j=0;j<rays.Count();++j){auto&r=rays[j];auto d=r.ray.Dir();double vals[]={d.x,d.y,d.z,r.kray[0],r.kray[1],r.kray[2],r.pdf};h=Hash(h,vals,7);}}
   }c->release();
  }std::cout<<(base==f.lamb?"Lambertian":"Oren-Nayar")<<" value/AOV/Pdf/512-draw hashes "<<h<<"\n";
 }oren->release();sig->release();return 0;}
