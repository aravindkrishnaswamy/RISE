//////////////////////////////////////////////////////////////////////
//
//  NonUniformLuminaryAreaTest.cpp - DL-448: a CURVED analytic luminary
//    under a NON-UNIFORM object transform is sampled uniformly in WORLD
//    area and reports its true world area, so pdfPosition = 1/GetArea()
//    holds for every consumer (NEE, light subpaths, photons, SMS, the
//    emitter-hit MIS partner).
//
//    Before: Object::GetArea() scaled the object area by |det|^(2/3)
//    (exact only for similarities) and UniformRandomPoint() pushed an
//    OBJECT-uniform sample through the map, which is not world-uniform
//    when the area stretch J = |det L| |L^-T n| varies over the surface.
//    Meshes and boxes were fixed by a5328c59a (triangle CDF); this suite
//    covers sphere, ellipsoid, cylinder, torus and disk.  It also covers
//    EllipsoidGeometry itself, whose sampler (marginal theta CDF, uniform
//    phi) was area-uniform only for a == b and whose GetArea() was a ~1 %
//    Thomsen approximation.
//
//    A  GetArea() against closed forms / an independent parametric
//       quadrature (relative 2e-5).
//    B  world-uniformity: the fraction of samples in a world region
//       against that region's independent area fraction (5 sigma).
//    C  renders (8 salted repeats x 256 spp per cell, box filter, OIDN
//       off; the gate is 4 combined standard errors because each se is
//       estimated from only 8 repeats, ~t(14)): a
//       scaled primitive against the same world surface authored
//       directly where one exists (scaled sphere vs ellipsoid_geometry,
//       scaled cylinder vs authored cylinder, scaled disk vs authored
//       disk), and PT vs BDPT vs VCM.  Pre-fix these differ by 6-230 %.
//
//  Author: Claude (debt-dl448)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <string>
#include <unistd.h>

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/Utilities/SobolSampler.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static unsigned int g_seedBase = 4480u;
static unsigned int g_renderIndex = 0;
static int passCount = 0;
static int failCount = 0;
static const double kPi = 3.14159265358979323846;

static void Check( bool condition, const std::string& testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

static std::string Fmt( const char* f, ... )
{
	char buf[8192];
	va_list ap;
	va_start( ap, f );
	std::vsnprintf( buf, sizeof(buf), f, ap );
	va_end( ap );
	return std::string( buf );
}

static std::string TempPath( const char* tag )
{
	return Fmt( "/tmp/dl448_%s_%d.RISEscene", tag, static_cast<int>( ::getpid() ) );
}

//////////////////////////////////////////////////////////////////////
// Shape definitions: geometry chunk + object transform
//////////////////////////////////////////////////////////////////////
struct Shape
{
	const char* name;
	std::string geometry;	// chunk body, named geo_<name>
	std::string transform;	// standard_object extra lines
};

static std::string ObjectText( const Shape& s, const char* material, const char* position )
{
	return s.geometry + Fmt( "standard_object\n{\n\tname %s\n\tgeometry geo_%s\n\tmaterial %s\n\tposition %s\n%s}\n\n",
		s.name, s.name, material, position, s.transform.c_str() );
}

//////////////////////////////////////////////////////////////////////
// A: areas
//////////////////////////////////////////////////////////////////////
struct Fixture
{
	IJobPriv* job = nullptr;
	explicit Fixture( const std::string& text )
	{
		const std::string path = TempPath( "fixture" );
		{ std::ofstream out( path ); out << text; }
		if( RISE_CreateJobPriv( &job ) ) {
			Check( job->LoadAsciiSceneViaCst( path.c_str() ), "fixture scene parses" );
		}
		std::remove( path.c_str() );
		if( job ) job->GetScene()->GetObjects()->PrepareForRendering();
	}
	~Fixture() { safe_release( job ); }
	const IObject* Object( const char* name ) const { return job ? job->GetScene()->GetObjects()->GetItem( name ) : nullptr; }
};

//! Area of the parametric surface p(u,v) on [0,1]^2, midpoint rule.
template<class F>
static double ParamArea( F p, int nu, int nv )
{
	double sum = 0;
	const double h = 1e-6;
	for( int j = 0; j < nv; j++ ) {
		for( int i = 0; i < nu; i++ ) {
			const double u = ( i + 0.5 ) / nu, v = ( j + 0.5 ) / nv;
			double a[3], b[3], c[3], d[3];
			p( u + h, v, a ); p( u - h, v, b ); p( u, v + h, c ); p( u, v - h, d );
			const double du[3] = { (a[0]-b[0])/(2*h), (a[1]-b[1])/(2*h), (a[2]-b[2])/(2*h) };
			const double dv[3] = { (c[0]-d[0])/(2*h), (c[1]-d[1])/(2*h), (c[2]-d[2])/(2*h) };
			const double x = du[1]*dv[2]-du[2]*dv[1], y = du[2]*dv[0]-du[0]*dv[2], z = du[0]*dv[1]-du[1]*dv[0];
			sum += std::sqrt( x*x + y*y + z*z );
		}
	}
	return sum / ( double( nu ) * nv );
}

//! Ellipsoid (a,b,c) parameterized by (u -> theta, v -> phi); also the
//! area of its region with |n_z| > t, n the unit normal.
static double EllipsoidArea( double a, double b, double c, double zThresh = -1, double* region = nullptr )
{
	const int N = 1200;
	double total = 0, reg = 0;
	for( int j = 0; j < N; j++ ) {
		for( int i = 0; i < N; i++ ) {
			const double th = kPi * ( i + 0.5 ) / N, ph = 2 * kPi * ( j + 0.5 ) / N;
			const double st = std::sin( th ), ct = std::cos( th ), sp = std::sin( ph ), cp = std::cos( ph );
			const double dA = st * std::sqrt( b*b*c*c*st*st*cp*cp + a*a*c*c*st*st*sp*sp + a*a*b*b*ct*ct ) * ( kPi / N ) * ( 2 * kPi / N );
			total += dA;
			const double nx = st*cp/a, ny = st*sp/b, nz = ct/c;
			if( std::fabs( nz ) / std::sqrt( nx*nx + ny*ny + nz*nz ) > zThresh ) reg += dA;
		}
	}
	if( region ) *region = reg;
	return total;
}

static void CheckArea( const IObject* o, double expected, const char* label )
{
	const double got = o ? double( o->GetArea() ) : -1;
	std::cout << "  A " << label << ": GetArea " << got << " expected " << expected
		<< " rel " << ( got / expected - 1 ) << "\n";
	Check( o && std::fabs( got / expected - 1 ) < 2e-5, std::string( "DL-448 area: " ) + label );
}

static const char* kFixtureHeader =
	"RISE ASCII SCENE 7\n\n"
	"uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname lum\n\texitance white\n\tscale 1\n\tmaterial none\n}\n\n";

static Shape SphereScaled( const char* n, double sx, double sy, double sz )
{
	return Shape{ n, Fmt( "sphere_geometry\n{\n\tname geo_%s\n\tradius 1\n}\n\n", n ), Fmt( "\tscale %g %g %g\n", sx, sy, sz ) };
}
static Shape EllipsoidScaled( const char* n, double a, double b, double c, double sx, double sy, double sz )
{
	return Shape{ n, Fmt( "ellipsoid_geometry\n{\n\tname geo_%s\n\tradii %g %g %g\n}\n\n", n, a, b, c ), Fmt( "\tscale %g %g %g\n", sx, sy, sz ) };
}
static Shape CylinderScaled( const char* n, double r, double h, double sx, double sy, double sz )
{
	return Shape{ n, Fmt( "cylinder_geometry\n{\n\tname geo_%s\n\taxis z\n\tradius %g\n\theight %g\n\tcapped TRUE\n}\n\n", n, r, h ),
		Fmt( "\tscale %g %g %g\n", sx, sy, sz ) };
}
static Shape TorusScaled( const char* n, double R, double ratio, double sx, double sy, double sz )
{
	return Shape{ n, Fmt( "torus_geometry\n{\n\tname geo_%s\n\tmajorradius %g\n\tminorratio %g\n}\n\n", n, R, ratio ),
		Fmt( "\tscale %g %g %g\n", sx, sy, sz ) };
}
static Shape DiskScaled( const char* n, double r, double sx, double sy, double sz )
{
	return Shape{ n, Fmt( "circulardisk_geometry\n{\n\tname geo_%s\n\tradius %g\n\taxis z\n}\n\n", n, r ),
		Fmt( "\tscale %g %g %g\n", sx, sy, sz ) };
}

static void TestAreas()
{
	std::cout << "A: world areas\n";
	std::string text = kFixtureHeader;
	const Shape shapes[] = {
		SphereScaled( "sph", 2, 1, 0.5 ),
		EllipsoidScaled( "ell", 2, 1, 0.5, 1, 1, 1 ),
		EllipsoidScaled( "ellflat", 1, 1, 1, 0.5, 0.5, 0.00025 ),
		EllipsoidScaled( "ellsq", 1.5, 1, 0.5, 0.5, 2, 1 ),
		CylinderScaled( "cyl", 1, 2, 0.5, 0.5, 3 ),
		CylinderScaled( "cylell", 1, 2, 2, 1, 1 ),
		TorusScaled( "tor", 1, 0.3, 1, 0.2, 1 ),
		DiskScaled( "dsk", 1, 2, 0.5, 7 ),
	};
	for( const Shape& s : shapes ) text += ObjectText( s, "lum", "0 0 0" );
	Fixture f( text );

	CheckArea( f.Object( "sph" ), EllipsoidArea( 2, 1, 0.5 ), "sphere r1 scale (2,1,0.5) = ellipsoid quadrature" );
	CheckArea( f.Object( "ell" ), EllipsoidArea( 2, 1, 0.5 ), "ellipsoid_geometry (2,1,0.5) unscaled" );
	CheckArea( f.Object( "ellflat" ), EllipsoidArea( 0.5, 0.5, 0.00025 ), "ellipsoid (1,1,1) scale (0.5,0.5,0.00025)" );
	CheckArea( f.Object( "ellsq" ), EllipsoidArea( 0.75, 2, 0.5 ), "ellipsoid (1.5,1,0.5) scale (0.5,2,1)" );
	CheckArea( f.Object( "cyl" ), 2*kPi*0.5*6 + 2*kPi*0.25, "capped cylinder r1 h2 scale (0.5,0.5,3)" );
	{
		// Elliptical cylinder a=2,b=1, height 2 plus two elliptical caps.
		double perim = 0; const int N = 200000;
		for( int i = 0; i < N; i++ ) { const double t = 2*kPi*(i+0.5)/N; perim += std::sqrt( 4*std::sin(t)*std::sin(t) + std::cos(t)*std::cos(t) ); }
		perim *= 2*kPi/N;
		CheckArea( f.Object( "cylell" ), 2*perim + 2*kPi*2*1, "capped cylinder r1 h2 scale (2,1,1)" );
	}
	CheckArea( f.Object( "tor" ), ParamArea( []( double u, double v, double* p ) {
		const double U = 2*kPi*u, V = 2*kPi*v, R = 1, r = 0.3;
		p[0] = ( R + r*std::cos(V) )*std::cos(U); p[1] = 0.2*r*std::sin(V); p[2] = ( R + r*std::cos(V) )*std::sin(U);
	}, 800, 800 ), "torus R1 r0.3 scale (1,0.2,1)" );
	CheckArea( f.Object( "dsk" ), kPi*2*0.5, "disk r1 scale (2,0.5,7)" );
}

//////////////////////////////////////////////////////////////////////
// B: world-uniformity of the samples
//////////////////////////////////////////////////////////////////////
static double RadInv( unsigned b, unsigned i ) { double f = 1.0/b, r = 0; while( i ) { r += f*(i%b); i /= b; f /= b; } return r; }

template<class Pred>
static double SampleFraction( const IObject* o, Pred inRegion, unsigned N )
{
	unsigned hits = 0;
	for( unsigned i = 0; o && i < N; i++ ) {
		Point3 p; Vector3 n;
		o->UniformRandomPoint( &p, &n, nullptr, Point3( ( i + 0.5 ) / N, RadInv( 3, i ), RadInv( 5, i ) ) );
		hits += inRegion( p, n ) ? 1u : 0u;
	}
	return double( hits ) / N;
}

static void CheckFraction( double got, double expected, unsigned N, const char* label )
{
	const double sigma = std::sqrt( expected * ( 1 - expected ) / N );
	std::cout << "  B " << label << ": sample fraction " << got << " area fraction " << expected
		<< " z " << ( got - expected ) / sigma << "\n";
	Check( std::fabs( got - expected ) < 5 * sigma, std::string( "DL-448 world-uniform: " ) + label );
}

static void TestUniformity()
{
	std::cout << "B: world-uniform sampling\n";
	std::string text = kFixtureHeader;
	text += ObjectText( SphereScaled( "sph", 2, 1, 0.5 ), "lum", "0 0 0" );
	text += ObjectText( EllipsoidScaled( "ell", 2, 1, 0.5, 1, 1, 1 ), "lum", "0 0 0" );
	text += ObjectText( EllipsoidScaled( "ellsq", 1.5, 1, 0.5, 0.5, 2, 1 ), "lum", "0 0 0" );
	text += ObjectText( CylinderScaled( "cyl", 1, 2, 0.5, 0.5, 3 ), "lum", "0 0 0" );
	text += ObjectText( TorusScaled( "tor", 1, 0.3, 1, 0.2, 1 ), "lum", "0 0 0" );
	Fixture f( text );
	const unsigned N = 40000;

	double reg = 0;
	double tot = EllipsoidArea( 2, 1, 0.5, 0.9, &reg );
	auto zCap = []( const Point3&, const Vector3& n ) { return std::fabs( n.z ) > 0.9; };
	CheckFraction( SampleFraction( f.Object( "sph" ), zCap, N ), reg / tot, N, "sphere scale (2,1,0.5), |n.z|>0.9" );
	CheckFraction( SampleFraction( f.Object( "ell" ), zCap, N ), reg / tot, N, "ellipsoid_geometry (2,1,0.5), |n.z|>0.9" );
	tot = EllipsoidArea( 0.75, 2, 0.5, 0.9, &reg );
	CheckFraction( SampleFraction( f.Object( "ellsq" ), zCap, N ), reg / tot, N, "ellipsoid (1.5,1,0.5) scale (0.5,2,1), |n.z|>0.9" );
	// Cylinder (r 0.5, h 6 in world): caps hold 0.5 pi / 6.5 pi.
	CheckFraction( SampleFraction( f.Object( "cyl" ), zCap, N ), 0.5 / 6.5, N, "cylinder scale (0.5,0.5,3), cap fraction" );
	// Torus squashed in y: region = outer half (x^2+z^2 > R^2).
	{
		const int M = 1500; double all = 0, outer = 0;
		for( int j = 0; j < M; j++ ) for( int i = 0; i < M; i++ ) {
			const double V = 2*kPi*(j+0.5)/M, r = 0.3;
			// |dp/dU x dp/dV| for p = ((1+r cosV)cosU, 0.2 r sinV, (1+r cosV) sinU)
			const double rho = 1 + r*std::cos(V);
			const double nx = 0.2*r*std::cos(V), ny = r*std::sin(V);	// in the (radial, y) plane, times rho
			const double dA = rho * std::sqrt( nx*nx + ny*ny );
			all += dA; if( std::cos(V) > 0 ) outer += dA;
		}
		CheckFraction( SampleFraction( f.Object( "tor" ), []( const Point3& p, const Vector3& ) { return p.x*p.x + p.z*p.z > 1.0; }, N ),
			outer / all, N, "torus scale (1,0.2,1), outer half" );
	}
}

//////////////////////////////////////////////////////////////////////
// C: renders
//////////////////////////////////////////////////////////////////////
class CapturingRasterizerOutput : public virtual IRasterizerOutput, public virtual Reference
{
public:
	std::vector<RISEColor> pixels;
protected:
	virtual ~CapturingRasterizerOutput() {}
public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	virtual void OutputImage( const IRasterImage& img, const Rect*, const unsigned int ) override
	{
		pixels.resize( img.GetWidth() * img.GetHeight() );
		for( unsigned int y = 0; y < img.GetHeight(); y++ )
			for( unsigned int x = 0; x < img.GetWidth(); x++ )
				pixels[y * img.GetWidth() + x] = img.GetPEL( x, y );
	}
};

static bool RenderOnce( const std::string& sceneText, double& mean )
{
	const std::string path = TempPath( "render" );
	{ std::ofstream ofs( path ); ofs << sceneText; }
	bool ok = false;
	IJobPriv* pJob = nullptr;
	if( RISE_CreateJobPriv( &pJob ) && pJob ) {
		if( pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
			pJob->RemoveRasterizerOutputs();
			CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
			GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "capture" );
			pJob->GetRasterizer()->AddRasterizerOutput( pCap );
			SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( g_seedBase + g_renderIndex, 0x448u ) );
			std::srand( g_seedBase + g_renderIndex++ );
			const bool bRendered = pJob->Rasterize();
			SobolSamplerTestHooks::ValueSalt().store( 0u );
			if( bRendered && !pCap->pixels.empty() ) {
				double sum = 0; ok = true;
				for( const RISEColor& c : pCap->pixels ) {
					const double v = ( c.base.r + c.base.g + c.base.b ) * c.a / 3.0;
					if( !std::isfinite( v ) ) { ok = false; break; }
					sum += v;
				}
				mean = sum / double( pCap->pixels.size() );

			}
			safe_release( pCap );
		}
		safe_release( pJob );
	}
	std::remove( path.c_str() );
	return ok;
}

struct Stat { double mean, se; bool ok; };

static Stat RenderN( const std::string& text, int n )
{
	Stat s{ 0, 0, true };
	std::vector<double> v;
	for( int i = 0; i < n; i++ ) {
		double m = 0;
		if( !RenderOnce( text, m ) ) { s.ok = false; return s; }
		v.push_back( m );
	}
	for( double x : v ) s.mean += x;
	s.mean /= n;
	double ss = 0;
	for( double x : v ) ss += ( x - s.mean ) * ( x - s.mean );
	s.se = std::sqrt( ss / ( n - 1 ) / n );
	return s;
}

//! Floor (rho 0.5) at z = 0 seen by an orthographic camera at z = 1
//! looking down; the emitter hangs above the camera, so only
//! emitter -> floor -> camera transport is imaged.
static std::string RenderScene( const Shape& emitter, const char* position, const std::string& rasterizer )
{
	return std::string( "RISE ASCII SCENE 7\n\n" ) +
		"film\n{\n\twidth 24\n\theight 24\n}\n\n"
		"orthographic_camera\n{\n\tlocation 0 0 1\n\tlookat 0 0 0\n\tup 0 1 0\n\tviewport_scale 4 4\n}\n\n"
		"uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n"
		"uniformcolor_painter\n{\n\tname grey\n\tcolor 0.5 0.5 0.5\n}\n\n"
		"lambertian_material\n{\n\tname floor_mat\n\treflectance grey\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname lum\n\texitance white\n\tscale 10\n\tmaterial none\n}\n\n"
		"clippedplane_geometry\n{\n\tname geo_floor\n\tpta -200 -200 0\n\tptb 200 -200 0\n\tptc 200 200 0\n\tptd -200 200 0\n\tdoublesided FALSE\n}\n\n"
		"standard_object\n{\n\tname floor\n\tgeometry geo_floor\n\tmaterial floor_mat\n}\n\n"
		+ ObjectText( emitter, "lum", position ) +
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n" + rasterizer;
}

static int g_spp = 256;
static int g_repeats = 8;
static std::string RasPT() { return "pathtracing_pel_rasterizer\n{\n\tsamples " + std::to_string( g_spp ) + "\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n"; }
static std::string RasBDPT() { return "bdpt_pel_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples " + std::to_string( g_spp ) + "\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n"; }
static std::string RasVCM() { return "vcm_pel_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples " + std::to_string( g_spp ) + "\n\toidn_denoise FALSE\n\tpixel_filter box\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled false\n}\n\n"; }

static bool Agree( const Stat& a, const Stat& b )
{
	const double se = std::sqrt( a.se * a.se + b.se * b.se );
	return std::fabs( a.mean - b.mean ) <= 4 * se + 1e-12;
}

static void Report( const char* label, const Stat& s ) { std::cout << "    " << label << " " << s.mean << " +/- " << s.se << "\n"; }

static void RenderCase( const char* label, const Shape& scaled, const Shape* authored, const char* position )
{
	std::cout << "  C " << label << "\n";
	const int n = g_repeats;
	const Stat pt  = RenderN( RenderScene( scaled, position, RasPT() ), n );
	const Stat bd  = RenderN( RenderScene( scaled, position, RasBDPT() ), n );
	const Stat vc  = RenderN( RenderScene( scaled, position, RasVCM() ), n );
	Report( "scaled PT  ", pt ); Report( "scaled BDPT", bd ); Report( "scaled VCM ", vc );
	Check( pt.ok && bd.ok && vc.ok, std::string( "DL-448 renders: " ) + label );
	Check( Agree( pt, bd ), std::string( "DL-448 PT vs BDPT: " ) + label );
	Check( Agree( pt, vc ), std::string( "DL-448 PT vs VCM: " ) + label );
	if( authored ) {
		const Stat ref = RenderN( RenderScene( *authored, position, RasPT() ), n );
		Report( "authored PT", ref );
		Check( ref.ok && Agree( pt, ref ), std::string( "DL-448 scaled vs authored (PT): " ) + label );
		Check( ref.ok && Agree( bd, ref ), std::string( "DL-448 scaled (BDPT) vs authored (PT): " ) + label );
	}
}

static void TestRenders()
{
	std::cout << "C: renders\n";
	{
		const Shape s = SphereScaled( "e", 1.2, 0.6, 0.15 );
		const Shape a = EllipsoidScaled( "e", 1.2, 0.6, 0.15, 1, 1, 1 );
		RenderCase( "sphere scale (1.2,0.6,0.15) vs ellipsoid_geometry", s, &a, "0.3 0 2.6" );
	}
	{
		const Shape s = CylinderScaled( "e", 1, 2, 0.6, 0.6, 0.15 );
		const Shape a = CylinderScaled( "e", 0.6, 0.3, 1, 1, 1 );
		RenderCase( "capped cylinder scale (0.6,0.6,0.15) vs authored", s, &a, "0 0.2 2.6" );
	}
	{
		const Shape s = DiskScaled( "e", 1, 0.8, 0.8, 5 );
		const Shape a = DiskScaled( "e", 0.8, 1, 1, 1 );
		// Disk axis z faces +z; flip it to face the floor with a negative z scale on both.
		Shape sf = s; sf.transform = "\tscale 0.8 0.8 -5\n";
		Shape af = a; af.transform = "\tscale 1 1 -1\n";
		RenderCase( "disk scale (0.8,0.8,-5) vs authored r0.8", sf, &af, "0 0 2.6" );
	}
	{
		const Shape s = TorusScaled( "e", 1, 0.3, 1, 0.25, 0.7 );
		RenderCase( "torus scale (1,0.25,0.7)", s, nullptr, "0 0 2.6" );
	}
	if( getenv( "DL448_TORUS_CONTROL" ) ) {
		const Shape s = TorusScaled( "e", 1, 0.3, 0.7, 0.7, 0.7 );
		RenderCase( "CONTROL torus uniform scale 0.7", s, nullptr, "0 0 2.6" );
	}
}

//! H: under a SIMILARITY transform the new sampling path is not taken, so
//! GetArea() and every UniformRandomPoint() output must be bit-identical to
//! the pre-DL-448 build (EllipsoidGeometry excepted: its own sampler and
//! area were deliberately fixed).  Prints one FNV hash per shape over the
//! area and 4096 samples, for an A/B between builds.
static void PrintSimilarityHashes()
{
	std::cout << "H: similarity-transform sampler hashes (compare across builds)\n";
	std::string text = kFixtureHeader;
	const Shape shapes[] = {
		SphereScaled( "sph", 0.7, 0.7, 0.7 ),
		CylinderScaled( "cyl", 1, 2, 0.5, 0.5, 0.5 ),
		TorusScaled( "tor", 1, 0.3, 0.6, 0.6, 0.6 ),
		DiskScaled( "dsk", 1, 0.8, 0.8, -0.8 ),
		Shape{ "box", "box_geometry\n{\n\tname geo_box\n\twidth 1\n\theight 2\n\tdepth 3\n}\n\n", "\tscale 1.5 1.5 1.5\n\torientation 10 20 30\n" },
		Shape{ "pln", "clippedplane_geometry\n{\n\tname geo_pln\n\tpta -1 -1 0\n\tptb 1 -1 0\n\tptc 1 1 0\n\tptd -1 1 0\n}\n\n", "\tscale 2 2 2\n\torientation 0 45 0\n" },
	};
	for( const Shape& s : shapes ) text += ObjectText( s, "lum", "0.1 0.2 0.3" );
	Fixture f( text );
	for( const Shape& s : shapes ) {
		const IObject* o = f.Object( s.name );
		unsigned long long h = 1469598103934665603ULL;
		auto mix = [&h]( double v ) { const unsigned char* b = reinterpret_cast<const unsigned char*>( &v ); for( std::size_t k = 0; k < sizeof( v ); k++ ) { h ^= b[k]; h *= 1099511628211ULL; } };
		if( o ) {
			mix( o->GetArea() );
			for( unsigned i = 0; i < 4096; i++ ) {
				Point3 p; Vector3 n; Point2 c;
				o->UniformRandomPoint( &p, &n, &c, Point3( ( i + 0.5 ) / 4096, RadInv( 3, i ), RadInv( 5, i ) ) );
				mix( p.x ); mix( p.y ); mix( p.z ); mix( n.x ); mix( n.y ); mix( n.z ); mix( c.x ); mix( c.y );
			}
		}
		std::printf( "  H %s area=%.17g hash=%016llx\n", s.name, o ? double( o->GetArea() ) : -1.0, h );
	}
}

int main( int argc, char** argv )
{
	if( argc > 1 ) g_seedBase = unsigned( std::strtoul( argv[1], nullptr, 10 ) );
	const std::string only = argc > 2 ? argv[2] : "all";
	if( argc > 3 ) g_spp = std::atoi( argv[3] );
	if( argc > 4 ) g_repeats = std::atoi( argv[4] );
	std::cout << "NonUniformLuminaryAreaTest (DL-448)\n";
	if( only == "all" || only == "A" ) TestAreas();
	if( only == "all" || only == "B" ) TestUniformity();
	if( only == "all" || only == "C" ) TestRenders();
	if( only == "H" ) PrintSimilarityHashes();
	std::cout << "\n" << passCount << " passed, " << failCount << " failed\n";
	return failCount == 0 ? 0 : 1;
}
