//////////////////////////////////////////////////////////////////////
//
//  RandomWalkSSSSpectralColourTest.cpp - Regression guard for DL-374:
//    `randomwalk_sss_material` rendered ACHROMATIC under every spectral
//    integrator, path tracing included.
//
//  THE DEFECT
//
//    `RandomWalkSSSMaterial::GetRandomWalkSSSParamsNM` returned the
//    material's construction-time RGB coefficient snapshot unchanged
//    (only the boundary IOR was evaluated at lambda), and
//    `RandomWalkSSS::SampleExit`'s NM mode collapsed that RGB triple to
//    its Rec. 709 luminance.  Every wavelength therefore walked the SAME
//    grey medium: a strongly chromatic sphere (absorption 3.0/0.5/0.02)
//    read 0.275/0.277/0.277 spectrally against the RGB renderer's
//    0.116/0.437/0.950.  The fix evaluates absorption and scattering at
//    lambda (IScalarPainter::GetValueAtNM -- the query the diffusion
//    profiles' EvaluateProfileNM already makes), broadcast per
//    IMaterial's documented contract, and the walk reads a broadcast
//    triple exactly.
//
//  WHAT EACH PART PROVES
//
//    A1 (deterministic): the material's NM parameters are the painters'
//       values AT lambda, broadcast, with sigma_t = sigma_a + sigma_s, and
//       they vary with lambda for a chromatic painter.
//    A2 (Monte Carlo, the walk itself): through the material's NM
//       parameters, the walk's mean NM weight at 650/550/450 nm --
//       exactly the RGBScalarPainter's R/G/B nodes -- equals the RGB
//       walk's mean per-channel weight (the RGB walk is unbiased for each
//       channel's expectation), and it is ordered by absorption across a
//       wavelength sweep.
//    B  (rendered): a chromatic random-walk sphere in a white furnace;
//       PT spectral, BDPT spectral and VCM spectral must reproduce the
//       PT RGB render's chromaticity (G/R and B/R) and per-channel level.
//       The spectral reconstruction of a 3-node coefficient curve is not
//       the RGB model, so the bands are measured, not "noise".  A hwss
//       TRUE BDPT row is PRINTED, not gated: its companions are DL-357.
//
//    Usage: [--unit-only]
//
//  Author: RISE debt-cleanup, slice `debt-dl374`
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>
#endif

#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/RandomWalkSSS.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Materials/RandomWalkSSSMaterial.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE { bool RISE_CreateJobPriv( IJobPriv** ppi ); }

namespace
{
	int passCount = 0;
	int failCount = 0;

	void Check( const bool condition, const std::string& label )
	{
		if( condition ) {
			++passCount;
		} else {
			++failCount;
			std::cout << "  FAILED: " << label << std::endl;
		}
	}

	class TestSampler : public ISampler
	{
		RandomNumberGenerator rng;
	public:
		explicit TestSampler( const unsigned int seed ) : rng( seed ) {}
		Scalar Get1D() { return rng.CanonicalRandom(); }
		Point2 Get2D() { return Point2( Get1D(), Get1D() ); }
	};

	// The chromatic medium of the DL-374 report.
	const Scalar kAbs[3] = { 3.0, 0.5, 0.02 };
	const Scalar kScat = 4.0;
	const Scalar kIOR = 1.3;

	struct Bundle
	{
		UniformScalarPainter* ior;
		RGBScalarPainter* absorption;
		UniformScalarPainter* scattering;
		RandomWalkSSSMaterial* material;
		Bundle()
		{
			ior = new UniformScalarPainter( kIOR ); ior->addref();
			absorption = new RGBScalarPainter( kAbs[0], kAbs[1], kAbs[2] ); absorption->addref();
			scattering = new UniformScalarPainter( kScat ); scattering->addref();
			material = new RandomWalkSSSMaterial( *ior, *absorption, *scattering, 0.0, 0.0, 256 );
			material->addref();
		}
		~Bundle()
		{
			material->release();
			scattering->release();
			absorption->release();
			ior->release();
		}
	};

	RayIntersectionGeometric ProbeRI()
	{
		RayIntersectionGeometric ri( Ray( Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ) ), nullRasterizerState );
		ri.bHit = true;
		ri.vNormal = Vector3( 0, 1, 0 );
		ri.onb.CreateFromW( ri.vNormal );
		return ri;
	}

	//////////////////////////////////////////////////////////////////
	// A1 -- the material's NM parameters are the coefficients at lambda
	//////////////////////////////////////////////////////////////////
	void TestParamsAtWavelength()
	{
		std::cout << "A1: GetRandomWalkSSSParamsNM evaluates the coefficients at lambda" << std::endl;
		Bundle b;
		const RayIntersectionGeometric ri = ProbeRI();
		Scalar prevSigmaA = -1;
		bool monotone = true;
		for( const Scalar nm : { 450.0, 500.0, 550.0, 600.0, 650.0 } ) {
			RandomWalkSSSParams p;
			const bool ok = b.material->GetRandomWalkSSSParamsNM( nm, p );
			const Scalar sa = b.absorption->GetValueAtNM( ri, nm );
			char buf[256];
			std::snprintf( buf, sizeof( buf ), "A1: nm=%.0f sigma_a (%g %g %g) == painter %g broadcast, sigma_s %g, sigma_t %g",
				nm, p.sigma_a[0], p.sigma_a[1], p.sigma_a[2], sa, p.sigma_s[0], p.sigma_t[0] );
			std::cout << "    " << buf << std::endl;
			Check( ok, std::string( buf ) + " (query answered)" );
			Check( p.sigma_a[0] == sa && p.sigma_a[1] == sa && p.sigma_a[2] == sa, buf );
			Check( p.sigma_s[0] == kScat && p.sigma_s[1] == kScat && p.sigma_s[2] == kScat, std::string( buf ) + " (sigma_s)" );
			Check( p.sigma_t[0] == sa + kScat && p.sigma_t[1] == sa + kScat && p.sigma_t[2] == sa + kScat, std::string( buf ) + " (sigma_t)" );
			Check( p.ior == kIOR, std::string( buf ) + " (ior)" );
			if( prevSigmaA >= 0 && !( p.sigma_a[0] > prevSigmaA ) ) monotone = false;
			prevSigmaA = p.sigma_a[0];
		}
		Check( monotone, "A1: the NM absorption rises from 450 to 650 nm with the chromatic painter" );
		// The RGB parameters are unchanged: the construction-time snapshot.
		const RandomWalkSSSParams* rgb = b.material->GetRandomWalkSSSParams();
		Check( rgb && rgb->sigma_a[0] == kAbs[0] && rgb->sigma_a[1] == kAbs[1] && rgb->sigma_a[2] == kAbs[2],
			"A1: RGB parameters are the per-channel snapshot (unchanged)" );
	}

	Object* MakeUnitSphere()
	{
		SphereGeometry* geometry = new SphereGeometry( 1.0 );
		geometry->addref();
		Object* object = new Object( geometry );
		object->addref();
		geometry->release();
		return object;
	}

	//! A front-face hit at the bottom of the unit sphere, ray travelling +Y.
	RayIntersectionGeometric MakeSurfaceRI()
	{
		const Point3 point( 0, -1, 0 );
		const Vector3 normal( 0, -1, 0 );
		const Vector3 incoming( 0, 1, 0 );
		RayIntersectionGeometric ri(
			Ray( Point3Ops::mkPoint3( point, -incoming * 2.0 ), incoming ),
			nullRasterizerState );
		ri.bHit = true;
		ri.ptIntersection = point;
		ri.vNormal = normal;
		ri.vGeomNormal = normal;
		ri.onb.CreateFromW( normal );
		ri.ambientIOR = 1.0;
		return ri;
	}

	struct Moments { double mean, se; };
	Moments Summ( const std::vector<double>& v )
	{
		double m = 0; for( double x : v ) m += x; m /= double( v.size() );
		double ss = 0; for( double x : v ) ss += ( x - m ) * ( x - m );
		return Moments{ m, std::sqrt( ss / double( v.size() - 1 ) / double( v.size() ) ) };
	}

	//////////////////////////////////////////////////////////////////
	// A2 -- the walk's NM weight at a channel node equals the RGB walk's
	//       channel weight, and orders with absorption across lambda
	//////////////////////////////////////////////////////////////////
	void TestWalkWavelengthSweep()
	{
		std::cout << "A2: random-walk NM weight vs the RGB walk's channel weight" << std::endl;
		Bundle b;
		Object* sphere = MakeUnitSphere();
		const RayIntersectionGeometric ri = MakeSurfaceRI();
		const int kWalks = 40000;
		const RandomWalkSSSParams* rgb = b.material->GetRandomWalkSSSParams();

		std::vector<double> rgbW[3];
		{
			TestSampler s( 37401 );
			for( int i = 0; i < kWalks; ++i ) {
				const BSSRDFSampling::SampleResult r = RandomWalkSSS::SampleExit(
					ri, sphere, rgb->sigma_a, rgb->sigma_s, rgb->sigma_t, rgb->g, rgb->ior, rgb->maxBounces, s, 0.0 );
				for( int c = 0; c < 3; ++c ) rgbW[c].push_back( r.valid ? double( r.weightSpatial[c] ) : 0.0 );
			}
		}
		const Scalar sweep[] = { 450.0, 500.0, 550.0, 600.0, 650.0 };
		double sweepMean[5];
		for( int k = 0; k < 5; ++k ) {
			RandomWalkSSSParams p;
			b.material->GetRandomWalkSSSParamsNM( sweep[k], p );
			TestSampler s( 37402 + k );
			std::vector<double> w;
			for( int i = 0; i < kWalks; ++i ) {
				const BSSRDFSampling::SampleResult r = RandomWalkSSS::SampleExit(
					ri, sphere, p.sigma_a, p.sigma_s, p.sigma_t, p.g, p.ior, p.maxBounces, s, sweep[k] );
				w.push_back( r.valid ? double( r.weightSpatialNM ) : 0.0 );
			}
			const Moments m = Summ( w );
			sweepMean[k] = m.mean;
			// 650/550/450 nm are exactly the RGBScalarPainter's R/G/B nodes.
			const int ch = sweep[k] == 650.0 ? 0 : sweep[k] == 550.0 ? 1 : sweep[k] == 450.0 ? 2 : -1;
			char buf[256];
			if( ch >= 0 ) {
				const Moments mc = Summ( rgbW[ch] );
				const double z = ( m.mean - mc.mean ) / std::sqrt( m.se * m.se + mc.se * mc.se );
				std::snprintf( buf, sizeof( buf ), "A2: nm=%.0f NM walk %.5f +/- %.5f vs RGB channel %d %.5f +/- %.5f (z %+.2f, |z| <= 4)",
					sweep[k], m.mean, m.se, ch, mc.mean, mc.se, z );
				std::cout << "    " << buf << std::endl;
				Check( std::isfinite( z ) && std::fabs( z ) <= 4.0, buf );
			} else {
				std::snprintf( buf, sizeof( buf ), "A2: nm=%.0f NM walk %.5f +/- %.5f", sweep[k], m.mean, m.se );
				std::cout << "    " << buf << std::endl;
			}
		}
		bool ordered = true;
		for( int k = 1; k < 5; ++k ) if( !( sweepMean[k] < sweepMean[k - 1] ) ) ordered = false;
		Check( ordered, "A2: the walk weight falls monotonically 450 -> 650 nm as absorption rises" );
		Check( sweepMean[0] > 2.0 * sweepMean[4], "A2: the 450 nm walk weight is more than twice the 650 nm one" );
		sphere->release();
	}

	//////////////////////////////////////////////////////////////////
	// B -- rendered chromaticity, spectral vs RGB
	//////////////////////////////////////////////////////////////////
	class CapturingRasterizerOutput
		: public virtual IRasterizerOutput
		, public virtual Reference
	{
	public:
		std::vector<RISEColor> pixels;
		CapturingRasterizerOutput() {}
	protected:
		virtual ~CapturingRasterizerOutput() {}
	public:
		virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
		virtual void OutputImage( const IRasterImage& image, const Rect*, const unsigned int ) override
		{
			pixels.resize( size_t( image.GetWidth() ) * image.GetHeight() );
			for( unsigned int y = 0; y < image.GetHeight(); y++ )
				for( unsigned int x = 0; x < image.GetWidth(); x++ )
					pixels[size_t( y ) * image.GetWidth() + x] = image.GetPEL( x, y );
		}
	};

	std::string ChromaticAbsorption()
	{
		std::ostringstream a;
		a << kAbs[0] << " " << kAbs[1] << " " << kAbs[2];
		return a.str();
	}

	std::string BuildScene( const std::string& rasterizer, const std::string& absorption )
	{
		std::ostringstream s;
		s << "RISE ASCII SCENE 7\n";
		s << "film\n{\n\twidth 32\n\theight 32\n}\n\n";
		s << "pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n";
		s << "uniformcolor_painter\n{\n\tname env\n\tcolor 1 1 1\n}\n\n";
		s << "randomwalk_sss_material\n{\n\tname subject\n\tior " << kIOR << "\n\tabsorption "
		  << absorption << "\n\tscattering " << kScat
		  << "\n\tg 0\n\troughness 0\n\tmax_bounces 256\n}\n\n";
		s << "sphere_geometry\n{\n\tname subject_geo\n\tradius 0.8\n}\n\n";
		s << "standard_object\n{\n\tname subject_obj\n\tgeometry subject_geo\n\tmaterial subject\n}\n\n";
		s << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
		s << rasterizer;
		s << "file_rasterizeroutput\n{\n\tpattern rendered/dl374_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";
		return s.str();
	}

	std::string Rasterizer( const char* kind, const int spp, const bool hwss )
	{
		const std::string env = "\tradiance_map env\n\tradiance_background FALSE\n\tpixel_filter box\n\toidn_denoise FALSE\n";
		std::ostringstream s;
		s << kind << "\n{\n\tsamples " << spp << "\n";
		if( std::strstr( kind, "bdpt" ) || std::strstr( kind, "vcm" ) ) {
			s << "\tmax_eye_depth 16\n\tmax_light_depth 16\n";
		} else {
			s << "\trr_min_depth 8\n";
		}
		if( std::strstr( kind, "spectral" ) ) s << "\thwss " << ( hwss ? "TRUE" : "FALSE" ) << "\n";
		s << env << "}\n\n";
		return s.str();
	}

	bool RenderChannelMeans( const std::string& scene, double out[3] )
	{
		char path[512];
		std::snprintf( path, sizeof( path ), "/tmp/dl374_colour_%d.RISEscene", static_cast<int>( ::getpid() ) );
		{
			std::ofstream ofs( path );
			if( !ofs.is_open() ) return false;
			ofs << scene;
		}
		IJobPriv* job = nullptr;
		if( !RISE_CreateJobPriv( &job ) || !job ) return false;
		bool ok = job->LoadAsciiSceneViaCst( path );
		std::remove( path );
		if( !ok ) { safe_release( job ); return false; }
		job->RemoveRasterizerOutputs();
		CapturingRasterizerOutput* cap = new CapturingRasterizerOutput();
		GlobalLog()->PrintNew( cap, __FILE__, __LINE__, "dl374 capture" );
		job->GetRasterizer()->AddRasterizerOutput( cap );
		ok = job->Rasterize() && !cap->pixels.empty();
		out[0] = out[1] = out[2] = 0;
		if( ok ) {
			for( const RISEColor& c : cap->pixels ) {
				out[0] += c.base.r * c.a; out[1] += c.base.g * c.a; out[2] += c.base.b * c.a;
			}
			for( int k = 0; k < 3; ++k ) {
				out[k] /= double( cap->pixels.size() );
				ok = ok && std::isfinite( out[k] ) && out[k] > 0;
			}
		}
		safe_release( cap );
		safe_release( job );
		return ok;
	}

	void PrintRow( const char* label, const double sp[3], const double pel[3], const char* note, char* buf, const size_t n )
	{
		std::snprintf( buf, n, "%-20s R %.5f G %.5f B %.5f  G/R %.3f B/R %.3f  vs RGB: R x%.3f G x%.3f B x%.3f%s",
			label, sp[0], sp[1], sp[2], sp[1] / sp[0], sp[2] / sp[0],
			sp[0] / pel[0], sp[1] / pel[1], sp[2] / pel[2], note );
		std::cout << "    " << buf << std::endl;
	}

	void TestRenderedChromaticity()
	{
		std::cout << "B: random-walk sphere in a white furnace, spectral vs RGB (single renders, 32x32, 1024 spp)" << std::endl;
		const int kSpp = 1024;
		char buf[512];

		// B0 -- grey control: a wavelength-independent medium must render the
		// same under PT spectral as under PT RGB.  Green before AND after the
		// fix (the grey reduction is exact for a grey medium); it pins that the
		// spectral furnace/film path itself reproduces the RGB render, so the
		// chromatic rows below measure the medium alone.
		{
			double pel[3], sp[3];
			const bool ok = RenderChannelMeans( BuildScene( Rasterizer( "pathtracing_pel_rasterizer", kSpp, false ), "0.5" ), pel )
				&& RenderChannelMeans( BuildScene( Rasterizer( "pathtracing_spectral_rasterizer", kSpp, false ), "0.5" ), sp );
			Check( ok, "B0: grey-medium renders produced output" );
			if( ok ) {
				std::printf( "    grey PT RGB          R %.5f G %.5f B %.5f\n", pel[0], pel[1], pel[2] );
				PrintRow( "grey PT spectral", sp, pel, "", buf, sizeof( buf ) );
				for( int k = 0; k < 3; ++k )
					Check( std::fabs( sp[k] / pel[k] - 1.0 ) <= 0.05,
						std::string( "B0: grey medium, spectral == RGB within 5% (channel " ) + char( '0' + k ) + "): " + buf );
			}
		}

		double pel[3];
		const bool okPel = RenderChannelMeans( BuildScene( Rasterizer( "pathtracing_pel_rasterizer", kSpp, false ), ChromaticAbsorption() ), pel );
		Check( okPel, "B: PT RGB render produced output" );
		if( !okPel ) return;
		std::printf( "    PT RGB               R %.5f G %.5f B %.5f  G/R %.3f B/R %.3f\n",
			pel[0], pel[1], pel[2], pel[1] / pel[0], pel[2] / pel[0] );

		struct Row { const char* label; const char* kind; bool hwss; bool gated; };
		const Row rows[] = {
			{ "PT spectral",          "pathtracing_spectral_rasterizer", false, true },
			{ "PT spectral hwss",     "pathtracing_spectral_rasterizer", true,  true },
			{ "BDPT spectral",        "bdpt_spectral_rasterizer",        false, true },
			{ "VCM spectral",         "vcm_spectral_rasterizer",         false, true },
			{ "BDPT spectral hwss",   "bdpt_spectral_rasterizer",        true,  false },
		};
		// Bands, measured on the fixed build (see docs/DEBT_LEDGER.md DL-374).
		// The spectral medium is the RGBScalarPainter's 3-node piecewise-linear
		// absorption curve (nodes 450/550/650 nm), integrated against the CMFs
		// -- not three independent RGB media -- so it does NOT reproduce the RGB
		// render to noise: G and B agree within ~5%, while R reads ~0.60-0.67x
		// (a saturated blue-green spectrum loses red to the Rec.709 red matching
		// function's negative lobe; the red channel is also the noisiest, a
		// small difference of large terms).  The walk itself is exact per
		// wavelength (A2).  Pre-fix every spectral row read grey: R x2.65-2.78,
		// G x0.89, B x0.46-0.47.
		for( const Row& r : rows ) {
			double sp[3];
			const bool ok = RenderChannelMeans( BuildScene( Rasterizer( r.kind, kSpp, r.hwss ), ChromaticAbsorption() ), sp );
			Check( ok || !r.gated, std::string( "B: render produced output: " ) + r.label );
			if( !ok ) continue;
			PrintRow( r.label, sp, pel, r.gated ? "" : "  (printed only: DL-357)", buf, sizeof( buf ) );
			if( !r.gated ) continue;
			Check( std::fabs( sp[1] / pel[1] - 1.0 ) <= 0.08, std::string( "B: green channel within 8% of RGB: " ) + buf );
			Check( std::fabs( sp[2] / pel[2] - 1.0 ) <= 0.08, std::string( "B: blue channel within 8% of RGB: " ) + buf );
			Check( sp[0] / pel[0] >= 0.45 && sp[0] / pel[0] <= 0.85, std::string( "B: red channel in [0.45, 0.85] of RGB: " ) + buf );
			Check( sp[2] / sp[0] >= 5.0, std::string( "B: spectral render is chromatic (B/R >= 5): " ) + buf );
		}
	}
}

int main( int argc, char** argv )
{
	const bool unitOnly = argc >= 2 && std::strcmp( argv[1], "--unit-only" ) == 0;
	TestParamsAtWavelength();
	TestWalkWavelengthSweep();
	if( !unitOnly ) TestRenderedChromaticity();
	std::cout << "Passed: " << passCount << "\nFailed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
