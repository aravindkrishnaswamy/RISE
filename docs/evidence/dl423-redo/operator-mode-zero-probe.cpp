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

// angular histogram: energy (MaxValue kray) in wedges
struct Ang { double inPlaneFar, offPlane, nearMirror, total; };
static Ang Angular( const ISPF& spf, double theta, unsigned seed, int batches, int per )
{
	auto ri = MakeIntersection( theta * kPi / 180 ); const Point3 origin = ri.ray.origin;
	Ang a = { 0, 0, 0, 0 };
	const double mth = theta;
	for( int b = 0; b < batches; ++b ) {
		RandomNumberGenerator rng( seed + 7919u * b ); IndependentSampler sampler( rng );
		IORStack stack = MakeTestIORStack( g_stub );
		for( int i = 0; i < per; ++i ) {
			const Point3 p( rng.CanonicalRandom()*10, rng.CanonicalRandom()*10, 0 );
			ri.ptIntersection = p; ri.ray.origin = Point3( origin.x+p.x, origin.y+p.y, origin.z );
			ScatteredRayContainer rays; spf.Scatter( ri, sampler, rays, stack );
			for( unsigned j = 0; j < rays.Count(); ++j ) {
				const Vector3 d = Vector3Ops::Normalize( rays[j].ray.Dir() );
				if( d.z <= 0 ) continue;
				const double w = ColorMath::MaxValue( rays[j].kray ) / ( double( batches ) * per );
				const double th = std::acos( std::min( 1.0, d.z ) ) * 180 / kPi;
				double ph = std::atan2( d.y, d.x ) * 180 / kPi; // mirror at phi=0 (view from +x... ray dir +x)
				// mirror of incoming (s,0,-c) is (s,0,c): phi = 0
				const bool inPlane = std::fabs( ph ) < 5.0;
				a.total += w;
				if( inPlane && std::fabs( th - mth ) > 20 ) a.inPlaneFar += w;
				if( !inPlane && std::fabs( ph ) < 30 && std::fabs( th - mth ) > 20 ) a.offPlane += w; // per 25deg az band
				if( std::fabs( th - mth ) < 5 && std::fabs( ph ) < 5 ) a.nearMirror += w;
			}
		}
	}
	return a;
}

static double MaxKray( const ISPF& spf, double theta, unsigned seed, int n, double& meanOut )
{
	auto ri = MakeIntersection( theta * kPi / 180 ); const Point3 origin = ri.ray.origin;
	RandomNumberGenerator rng( seed ); IndependentSampler sampler( rng );
	IORStack stack = MakeTestIORStack( g_stub );
	double mx = 0, s = 0;
	for( int i = 0; i < n; ++i ) {
		const Point3 p( rng.CanonicalRandom()*10, rng.CanonicalRandom()*10, 0 );
		ri.ptIntersection = p; ri.ray.origin = Point3( origin.x+p.x, origin.y+p.y, origin.z );
		ScatteredRayContainer rays; spf.Scatter( ri, sampler, rays, stack );
		for( unsigned j = 0; j < rays.Count(); ++j ) {
			const double k = ColorMath::MaxValue( rays[j].kray );
			if( rays[j].ray.Dir().z > 0 ) { mx = std::max( mx, k ); s += k; }
		}
	}
	meanOut = s / n;
	return mx;
}

#include <fstream>
int main(int argc,char**argv) {
 if(argc<2) return 2;
 g_stub=new StubObject();g_stub->addref(); Fixtures f=MakeFixtures();
 if(std::string(argv[1])=="continuity") {
  for(double alpha:{.02,.05,.16,1.}) {
   auto*g1=MakeSchlickGgx(0,.9,alpha-1e-4);auto*g2=MakeSchlickGgx(0,.9,alpha+1e-4);
   auto*c1=MakeCoated(*g1,0,*f.white);auto*c2=MakeCoated(*g2,0,*f.white);
   auto ri=MakeRI(Dir(70,0));auto wi=Dir(68,180);
   double v1=c1->GetBSDF()->value(wi,ri)[0],v2=c2->GetBSDF()->value(wi,ri)[0];
   std::cout<<std::setprecision(12)<<"CONT alpha="<<alpha<<" low="<<v1<<" high="<<v2<<" relative="<<std::abs(v1-v2)/std::max(v1,v2)<<"\n";
   c1->release();c2->release();g1->release();g2->release();
  } return 0;
 }
 if(argc!=7) return 2;
 std::ifstream in(argv[1]);unsigned n;in>>n;std::vector<double>mu(n),weight(n);
 for(unsigned i=0;i<n;++i)in>>mu[i]>>weight[i];
 double alpha=std::stod(argv[3]),f0=std::stod(argv[4]),eta=std::stod(argv[5]);unsigned phases=std::stoul(argv[6]);
 auto*g=MakeSchlickGgx(0,f0,alpha);std::ofstream out(argv[2],std::ios::binary);
 auto row=[&](double mi){auto ri=MakeRI(Vector3(std::sqrt(1-mi*mi),0,mi));ri.ambientIOR=eta;
  for(unsigned j=0;j<n;++j){double total=0;for(unsigned k=0;k<phases;++k){double p=2*kPi*(k+.5)/phases;Vector3 u(std::sqrt(1-mu[j]*mu[j])*std::cos(p),std::sqrt(1-mu[j]*mu[j])*std::sin(p),mu[j]);total+=g->GetBSDF()->value(u,ri)[0];}double value=total/phases;out.write(reinterpret_cast<const char*>(&value),sizeof(value));}
 };
 for(double m:mu)row(m);for(double theta:{0.,45.,70.,80.,85.,89.})row(std::sqrt(1-std::pow(std::sin(theta*kPi/180)/eta,2)));
 g->release();return out.good()?0:1;
}
