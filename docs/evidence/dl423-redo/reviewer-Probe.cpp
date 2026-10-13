#define main CompositeFixtureMain
#include "/Users/aravind/Working/GitHub/RISE/.claude/worktrees/review-jetski/tests/CompositeEnergyConservationTest.cpp"
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

int main( int argc, char** argv )
{
	const std::string mode = argc > 1 ? argv[1] : "all";
	g_stub = new StubObject(); g_stub->addref();
	Fixtures f = MakeFixtures();
	auto* delta = new UniformScalarPainter( 1000000 ); delta->addref();
	std::cout << std::setprecision(5) << std::fixed;

	if( mode == "recip" || mode == "all" ) {
		// reciprocity of value() for several substrates / coats
		RandomNumberGenerator rng( 12345 );
		struct S { double d, f0, a, sig; };
		S subs[] = { {0,.9,.05,0}, {0,.5,.16,0}, {0,.9,.05,.3}, {.3,.5,.05,0}, {0,.3,.02,.2} };
		for( const S& s : subs ) {
			GGXMaterial* g = MakeSchlickGgx( s.d, s.f0, s.a );
			CoatedMaterial* c = MakeCoated( *g, s.sig, *f.white );
			double worst = 0;
			for( int i = 0; i < 4000; ++i ) {
				const double t1 = std::acos( rng.CanonicalRandom() ) * 180 / kPi, t2 = std::acos( rng.CanonicalRandom() ) * 180 / kPi;
				const double p1 = rng.CanonicalRandom()*360, p2 = rng.CanonicalRandom()*360;
				const Vector3 a = Dir( t1, p1 ), b = Dir( t2, p2 );
				const RISEPel fab = c->GetBSDF()->value( a, MakeRI( b ) );
				const RISEPel fba = c->GetBSDF()->value( b, MakeRI( a ) );
				for( int k = 0; k < 3; ++k ) {
					const double m = std::max( fab[k], fba[k] );
					if( m > 1e-6 ) worst = std::max( worst, std::fabs( fab[k] - fba[k] ) / m );
				}
			}
			std::cout << "RECIP d" << s.d << " F0 " << s.f0 << " a " << s.a << " sig " << s.sig << " worst rel " << worst << "\n";
			c->release(); g->release();
		}
	}

	if( mode == "step" || mode == "all" ) {
		for( double f0 : { 0.19, 0.1999, 0.2, 0.21 } ) {
			GGXMaterial* g = MakeSchlickGgx( 0, f0, .05 );
			CoatedMaterial* c = MakeCoated( *g, 0, *f.white );
			std::cout << "STEP F0 " << f0 << ":";
			for( double th : { 45.0, 70.0, 85.0 } ) {
				const auto s = Furnace( *c->GetSPF(), th, false, false, 8, 20000, 31u + (unsigned)th );
				std::cout << "  th" << th << " albedo " << s.mean << "+-" << s.sem;
			}
			// direct BSDF value near mirror at 70 deg
			const RISEPel v = c->GetBSDF()->value( Dir( 60, 180 ), MakeRI( Dir( 60, 0 ) ) );
			const RISEPel v2 = c->GetBSDF()->value( Dir( 75, 170 ), MakeRI( Dir( 70, 0 ) ) );
			std::cout << "  f(60mirror) " << v[0] << "  f(70/75,dphi170) " << v2[0] << "\n";
			c->release(); g->release();
		}
	}

	if( mode == "furnace" || mode == "all" ) {
		// lossless metal F0 = 1 under clear coat: albedo must be <= 1
		for( double eta : { 1.3, 1.5, 1.7 } )
		for( double al : { 0.02, 0.05, 0.1, 0.3, 1.0 } ) {
			GGXMaterial* g = MakeSchlickGgx( 0, 1.0, al );
			CoatedMaterial* c = MakeCoated( *g, 0, *f.white, eta );
			std::cout << "FURN eta " << eta << " a " << al << ":";
			for( double th : { 0.0, 45.0, 70.0, 80.0, 85.0, 89.0 } ) {
				const auto s = Furnace( *c->GetSPF(), th, false, false, 8, 10000, 51u + (unsigned)th );
				std::cout << " " << (int)th << ":" << s.mean << "+-" << s.sem;
			}
			std::cout << "\n";
			c->release(); g->release();
		}
	}

	if( mode == "acc" || mode == "all" ) {
		// coated vs composite across the grid incl. 85 deg
		struct C { double f0, a, eta, sig; };
		C cells[] = {
			{ .9, .05, 1.5, 0 }, { .9, .02, 1.5, 0 }, { .9, .1, 1.5, 0 }, { .5, .16, 1.5, 0 },
			{ .95, .05, 1.5, 0 }, { .5, .05, 1.5, 0 }, { .3, .05, 1.5, 0 }, { .19, .05, 1.5, 0 }, { .2, .05, 1.5, 0 },
			{ .04, .05, 1.5, 0 }, { .9, .05, 1.3, 0 }, { .9, .05, 1.7, 0 }, { .9, .3, 1.5, 0 }, { .9, .5, 1.5, 0 },
			{ .9, .05, 1.5, .2 }, { .9, .05, 1.5, 1.0 }, { .7, .02, 1.7, 0 },
		};
		for( const C& c : cells ) {
			GGXMaterial* g = MakeSchlickGgx( 0, c.f0, c.a );
			UniformScalarPainter* n = new UniformScalarPainter( c.eta ); n->addref();
			DielectricMaterial* sm = new DielectricMaterial( *f.s1, *n, *delta, false ); sm->addref();
			UniformScalarPainter* ext = new UniformScalarPainter( c.sig ); ext->addref();
			CompositeMaterial* comp = MakeComposite( *sm, *g, 3, 3, 3, 3, 3, 1.0, *ext );
			CoatedMaterial* co = MakeCoated( *g, c.sig, *f.white, c.eta );
			std::cout << "ACC F0 " << c.f0 << " a " << c.a << " eta " << c.eta << " sig " << c.sig << ":";
			for( double th : { 0.0, 45.0, 70.0, 80.0, 85.0 } ) {
				const auto a = Furnace( *co->GetSPF(), th, false, false, 8, 20000, 7101u + (unsigned)th );
				const auto b = Furnace( *comp->GetSPF(), th, false, false, 8, 20000, 9101u + (unsigned)th );
				const double r = a.mean / b.mean;
				const double sr = r * std::sqrt( std::pow( a.sem/a.mean, 2 ) + std::pow( b.sem/b.mean, 2 ) );
				std::cout << " " << (int)th << ":" << r << "+-" << sr;
			}
			std::cout << "\n";
			co->release(); comp->release(); ext->release(); sm->release(); n->release(); g->release();
		}
	}

	if( mode == "ang" || mode == "all" ) {
		struct C { double f0, a; };
		for( C c : { C{ .9, .05 }, C{ .9, .02 }, C{ .5, .16 } } ) {
			GGXMaterial* g = MakeSchlickGgx( 0, c.f0, c.a );
			CompositeMaterial* comp = MakeComposite( *new DielectricMaterial( *f.s1, *f.s15, *delta, false ), *g, 3, 3, 3, 3, 3, 1.0, *f.s0 );
			CoatedMaterial* co = MakeCoated( *g, 0, *f.white );
			for( double th : { 45.0, 70.0, 85.0 } ) {
				const Ang a = Angular( *co->GetSPF(), th, 777, 8, 20000 );
				const Ang b = Angular( *comp->GetSPF(), th, 999, 8, 20000 );
				std::cout << "ANG F0 " << c.f0 << " a " << c.a << " th " << th
				          << " coated[tot " << a.total << " nearMirror " << a.nearMirror << " inPlaneFar " << a.inPlaneFar << " offPlane " << a.offPlane << "]"
				          << " composite[tot " << b.total << " nearMirror " << b.nearMirror << " inPlaneFar " << b.inPlaneFar << " offPlane " << b.offPlane << "]\n";
			}
			double m1, m2;
			for( double th : { 45.0, 70.0, 85.0 } ) {
				const double mx1 = MaxKray( *co->GetSPF(), th, 4242, 200000, m1 );
				std::cout << "MAXK F0 " << c.f0 << " a " << c.a << " th " << th << " coated mean " << m1 << " max kray " << mx1 << "\n";
			}
			(void)m2;
			co->release(); comp->release(); g->release();
		}
	}
	if( mode == "aov" ) {
		for( double al : { 0.02, 0.05, 0.16 } ) {
			GGXMaterial* g = MakeSchlickGgx( 0, 0.9, al );
			CoatedMaterial* c = MakeCoated( *g, 0, *f.white );
			for( double th : { 45.0, 70.0, 80.0, 85.0 } ) {
				const auto s = Furnace( *c->GetSPF(), th, false, false, 8, 40000, 61u + (unsigned)th );
				const RISEPel aov = c->GetBSDF()->albedo( MakeIntersection( th * kPi / 180.0 ) );
				std::cout << "AOV a " << al << " th " << th << " furnace " << s.mean << "+-" << s.sem << " aov " << aov[0] << " ratio " << aov[0]/s.mean << "\n";
			}
			c->release(); g->release();
		}
	}
	return 0;
}
