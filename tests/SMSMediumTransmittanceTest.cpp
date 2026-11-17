//////////////////////////////////////////////////////////////////////
//
//  SMSMediumTransmittanceTest.cpp - DL-419: SMS must pay the
//    participating-medium transmittance of every specular-chain
//    segment, receiver -> v_1 -> ... -> v_k -> light, exactly as the
//    walk it replaces does.
//
//  Before DL-419 `ManifoldSolver` had no medium term, so a receiver
//  in fog lit through a caster read PT+SMS / PT 1.2 and a caustic
//  through an absorbing interior medium was not darkened at all.
//
//  Rows (salted, n renders each, ratio of means):
//    A  the ledger's fog box: a Lambertian sphere inside an index-1.0
//       refractor box filled with isotropic fog, omni outside.
//       PT+SMS / PT (`transparent_shadows TRUE`, so the walk carries the
//       fog) gated against 1; VCM / PT printed (0.76-0.81 before DL-469,
//       which added VCM's volume merge; ~1.0 after, heavy-tailed --
//       tests/VCMVolumeMergeTest.cpp gates it).
//    B  a slab caustic (ior 1.5, 0.2 thick) on a floor lit only through
//       the slab by an omni directly overhead; the camera sits under the
//       slab and sees a few-degree patch under the light, so every chain
//       is near-normal.  Closed forms against the same scene without the
//       medium:
//         B1 homogeneous absorbing `interior_medium` (sigma_a 2.5):
//            exp(-2.5 * 0.2)                       -- RGB, NM, HWSS
//         B2 constant-density `painter_heterogeneous_medium` (the
//            ratio-tracking estimator), same sigma: same closed form
//         B3 a GLOBAL absorbing medium (sigma_a 0.1), no interior
//            medium: the walk's convention (MediumTracking: inside a
//            caster with no medium the global medium applies) gives
//            exp(-0.1 * (3.0 + |camera - floor|)).
//    C  extended SMS (`sms_extended TRUE`) on B1 and B3: media are
//       outside its contract, so the scene runs legacy SMS (before this
//       routing every anchor was ineligible and the caustic rendered
//       black); gated two-sided on the same closed forms.
//
//  Usage: SMSMediumTransmittanceTest [--trials n] [--only substring]
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
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
#include "../src/Library/Utilities/SobolSampler.h"
#include "../src/Library/Interfaces/ILog.h"
#include "../src/Library/RISE_API.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

namespace
{
	int passCount = 0;
	int failCount = 0;

	void Check( bool ok, const std::string& what )
	{
		if( ok ) { ++passCount; }
		else { ++failCount; std::cout << "  FAIL: " << what << std::endl; }
	}

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

	enum class Integ { PT, PTSMS, PTSMSExtended, NMSMS, HWSSSMS, VCM };

	std::string Rasterizer( Integ integ, unsigned int spp, bool transparentShadows, bool directOnly = false )
	{
		std::ostringstream o;
		o << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
		const bool spectral = integ == Integ::NMSMS || integ == Integ::HWSSSMS;
		if( integ == Integ::VCM ) {
			o << "vcm_pel_rasterizer\n{\n\tsamples " << spp
			  << "\n\tmax_eye_depth 32\n\tmax_light_depth 32\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
			return o.str();
		}
		o << ( spectral ? "pathtracing_spectral_rasterizer" : "pathtracing_pel_rasterizer" )
		  << "\n{\n\tsamples " << spp << "\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n";
		if( !spectral ) o << "\tpathguiding FALSE\n\tadaptive_max_samples 0\n";
		else o << "\tnum_wavelengths 8\n\tspectral_samples 1\n\thwss " << ( integ == Integ::HWSSSMS ? "TRUE" : "FALSE" ) << "\n";
		o << "\ttransparent_shadows " << ( transparentShadows ? "TRUE" : "FALSE" ) << "\n";
		if( integ != Integ::PT ) o << "\tsms_enabled TRUE\n";
		if( integ == Integ::PTSMSExtended ) o << "\tsms_extended TRUE\n";
		// The slab rows' closed form is the floor's DIRECT (SMS) caustic:
		// a diffuse bounce would add floor -> slab -> floor indirect light
		// whose internal-reflection share crosses the medium twice.
		if( directOnly ) o << "\tmax_diffuse_bounce 0\n";
		o << "}\n\n";
		return o.str();
	}

	const char* kOutput =
		"file_rasterizeroutput\n{\n\tpattern rendered/dl419_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";

	//! Row A: the ledger's fog box (pinhole so VCM is a valid reference).
	std::string FogBoxScene( Integ integ, unsigned int spp )
	{
		std::ostringstream o;
		o << "RISE ASCII SCENE 7\n";
		o << "film\n{\n\twidth 24\n\theight 24\n}\n\n";
		o << "pinhole_camera\n{\n\tlocation 0 0 5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 18\n}\n\n";
		o << "uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n";
		o << "lambertian_material\n{\n\tname subject\n\treflectance white\n}\n\n";
		o << "sphere_geometry\n{\n\tname subject_geo\n\tradius 1\n}\n\n";
		o << "standard_object\n{\n\tname subject_obj\n\tgeometry subject_geo\n\tmaterial subject\n}\n\n";
		o << "perfectrefractor_material\n{\n\tname wall_mat\n\tior 1.0\n\trefractance white\n}\n\n"
		  << "homogeneous_medium\n{\n\tname fog\n\tabsorption 0 0 0\n\tscattering 0.5 0.5 0.5\n\tphase isotropic\n}\n\n"
		  << "box_geometry\n{\n\tname wall_geo\n\twidth 3\n\theight 3\n\tdepth 3\n}\n\n"
		  << "standard_object\n{\n\tname wall\n\tgeometry wall_geo\n\tmaterial wall_mat\n\tinterior_medium fog\n}\n\n";
		o << "omni_light\n{\n\tname lgt\n\tposition 0.5 3 0.5\n\tcolor 1 1 1\n\tpower 20\n}\n\n";
		o << Rasterizer( integ, spp, true ) << kOutput;
		return o.str();
	}

	enum class SlabMedium { None, Homogeneous, Heterogeneous, Global };

	//! Row B/C: slab caustic seen from under the slab.
	std::string SlabScene( Integ integ, SlabMedium med, unsigned int spp )
	{
		std::ostringstream o;
		o << "RISE ASCII SCENE 7\n";
		o << "film\n{\n\twidth 12\n\theight 12\n}\n\n";
		o << "pinhole_camera\n{\n\tlocation 0 0.5 0.3\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 3\n}\n\n";
		o << "uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n";
		o << "uniformcolor_painter\n{\n\tname half\n\tcolor 0.5 0.5 0.5\n\tcolorspace Rec709RGB_Linear\n}\n\n";
		o << "lambertian_material\n{\n\tname floor_mat\n\treflectance half\n}\n\n";
		o << "clippedplane_geometry\n{\n\tname floor_geo\n\tpta -4 0 4\n\tptb 4 0 4\n\tptc 4 0 -4\n\tptd -4 0 -4\n\tdoublesided TRUE\n}\n\n";
		o << "standard_object\n{\n\tname floor\n\tgeometry floor_geo\n\tmaterial floor_mat\n}\n\n";
		o << "perfectrefractor_material\n{\n\tname slab_mat\n\tior 1.5\n\trefractance white\n}\n\n";
		o << "box_geometry\n{\n\tname slab_geo\n\twidth 8\n\theight 0.2\n\tdepth 8\n}\n\n";
		if( med == SlabMedium::Homogeneous ) {
			o << "homogeneous_medium\n{\n\tname absorber\n\tabsorption 2.5 2.5 2.5\n\tscattering 0 0 0\n\tphase isotropic\n}\n\n";
		} else if( med == SlabMedium::Heterogeneous ) {
			o << "painter_heterogeneous_medium\n{\n\tname absorber\n\tabsorption 5 5 5\n\tscattering 0 0 0\n\tphase isotropic\n"
			     "\tdensity_painter half\n\tresolution 8\n\tbbox_min -4.1 0.9 -4.1\n\tbbox_max 4.1 1.3 4.1\n}\n\n";
		} else if( med == SlabMedium::Global ) {
			o << "homogeneous_medium\n{\n\tname air\n\tabsorption 0.1 0.1 0.1\n\tscattering 0 0 0\n\tphase isotropic\n}\n\n"
			  << "global_medium\n{\n\tmedium air\n}\n\n";
		}
		o << "standard_object\n{\n\tname slab\n\tgeometry slab_geo\n\tposition 0 1.1 0\n\tmaterial slab_mat\n";
		if( med == SlabMedium::Homogeneous || med == SlabMedium::Heterogeneous ) o << "\tinterior_medium absorber\n";
		o << "}\n\n";
		o << "omni_light\n{\n\tname lgt\n\tposition 0 3 0\n\tcolor 1 1 1\n\tpower 20\n}\n\n";
		o << Rasterizer( integ, spp, false, true ) << kOutput;
		return o.str();
	}

	std::string WriteScene( const std::string& text, const char* tag )
	{
		std::ostringstream p;
		p << "tests/dl419_" << tag << "_" << getpid() << ".RISEscene";
		std::ofstream f( p.str().c_str() );
		if( !f ) return std::string();
		f << text;
		return p.str();
	}

	double RenderMean( const std::string& text, unsigned int seed )
	{
		const std::string path = WriteScene( text, "scene" );
		if( path.empty() ) return -1;
		IJobPriv* job = nullptr;
		if( !RISE_CreateJobPriv( &job ) || !job ) { std::remove( path.c_str() ); return -1; }
		if( !job->LoadAsciiSceneViaCst( path.c_str() ) ) { safe_release( job ); std::remove( path.c_str() ); return -1; }
		std::remove( path.c_str() );
		job->RemoveRasterizerOutputs();
		CapturingRasterizerOutput* cap = new CapturingRasterizerOutput();
		GlobalLog()->PrintNew( cap, __FILE__, __LINE__, "dl419 capture" );
		job->GetRasterizer()->AddRasterizerOutput( cap );
		SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( seed, 0x419u ) );
		std::srand( seed );
		const bool rendered = job->Rasterize();
		SobolSamplerTestHooks::ValueSalt().store( 0u );
		double mean = -1;
		if( rendered && !cap->pixels.empty() ) {
			double sum = 0;
			bool finite = true;
			for( const RISEColor& c : cap->pixels ) {
				const double v = ( c.base.r + c.base.g + c.base.b ) * c.a / 3.0;
				if( !std::isfinite( v ) ) { finite = false; break; }
				sum += v;
			}
			if( finite ) mean = sum / double( cap->pixels.size() );
		}
		safe_release( cap );
		safe_release( job );
		return mean;
	}

	struct Stats { double mean; double sd; };
	Stats Summarize( const std::vector<double>& v )
	{
		double m = 0;
		for( double x : v ) m += x;
		m /= double( v.size() );
		double var = 0;
		for( double x : v ) var += ( x - m ) * ( x - m );
		var /= double( v.size() > 1 ? v.size() - 1 : 1 );
		return { m, std::sqrt( var ) };
	}

	//! Paired salted renders; returns the ratio of means and prints it.
	//! Gates |ratio - expected| < band when band > 0.
	double RatioRow( const std::string& label, const std::string& test, const std::string& ref,
		double expected, double band, unsigned int trials, unsigned int& seed, const std::string& only )
	{
		if( !only.empty() && label.find( only ) == std::string::npos ) return -1;
		std::vector<double> tv, rv, ratios;
		bool valid = true;
		for( unsigned int t = 0; t < trials; ++t ) {
			const unsigned int s = seed++;
			const double a = RenderMean( test, s );
			const double b = RenderMean( ref, s );
			if( !( a > 0 ) || !( b > 0 ) ) valid = false;
			tv.push_back( a ); rv.push_back( b ); ratios.push_back( a / b );
		}
		Check( valid, label + ": every render finite and non-black" );
		if( !valid ) return -1;
		const Stats st = Summarize( tv ), sr = Summarize( rv ), sq = Summarize( ratios );
		const double ratio = st.mean / sr.mean;
		std::cout << std::setprecision( 6 ) << "  " << label << ": test " << st.mean << " +/- " << st.sd
			<< "  ref " << sr.mean << " +/- " << sr.sd << "  ratio " << ratio << " (expected " << expected
			<< ", per-pair sd " << sq.sd << ", band " << band << ")" << std::endl;
		if( band > 0 ) Check( std::fabs( ratio - expected ) < band, label + ": ratio within band" );
		return ratio;
	}
}

int main( int argc, char** argv )
{
	unsigned int trials = 4;
	std::string only;
	for( int i = 1; i < argc; ++i ) {
		const std::string a( argv[i] );
		if( a == "--only" && i + 1 < argc ) only = argv[++i];
		else if( a == "--trials" && i + 1 < argc ) trials = static_cast<unsigned int>( std::atoi( argv[++i] ) );
	}
	if( trials < 2 ) trials = 2;
	std::cout << "=== DL-419 SMS chain medium transmittance, n=" << trials << " ===" << std::endl;
	unsigned int seed = 41900;

	// A: the ledger row.
	RatioRow( "A: fog box PT+SMS / PT (omni, index-1.0 wall)",
		FogBoxScene( Integ::PTSMS, 64 ), FogBoxScene( Integ::PT, 64 ), 1.0, 0.03, trials, seed, only );
	RatioRow( "A: fog box VCM / PT (printed)",
		FogBoxScene( Integ::VCM, 1024 ), FogBoxScene( Integ::PT, 256 ), 1.0, 0.0, trials, seed, only );

	// B: slab caustic closed forms.
	const double trInterior = std::exp( -2.5 * 0.2 );
	const double camDist = std::sqrt( 0.5 * 0.5 + 0.3 * 0.3 );
	const double trGlobal = std::exp( -0.1 * ( 3.0 + camDist ) );
	const unsigned int spp = 16;
	RatioRow( "B1: interior homogeneous absorber / none, PT+SMS RGB",
		SlabScene( Integ::PTSMS, SlabMedium::Homogeneous, spp ), SlabScene( Integ::PTSMS, SlabMedium::None, spp ),
		trInterior, 0.01, trials, seed, only );
	RatioRow( "B1: interior homogeneous absorber / none, PT+SMS NM",
		SlabScene( Integ::NMSMS, SlabMedium::Homogeneous, spp ), SlabScene( Integ::NMSMS, SlabMedium::None, spp ),
		trInterior, 0.01, trials, seed, only );
	RatioRow( "B1: interior homogeneous absorber / none, PT+SMS HWSS",
		SlabScene( Integ::HWSSSMS, SlabMedium::Homogeneous, spp ), SlabScene( Integ::HWSSSMS, SlabMedium::None, spp ),
		trInterior, 0.01, trials, seed, only );
	RatioRow( "B2: interior heterogeneous absorber / none, PT+SMS RGB",
		SlabScene( Integ::PTSMS, SlabMedium::Heterogeneous, 64 ), SlabScene( Integ::PTSMS, SlabMedium::None, 64 ),
		trInterior, 0.03, trials, seed, only );
	RatioRow( "B3: global absorber / none, PT+SMS RGB",
		SlabScene( Integ::PTSMS, SlabMedium::Global, 1024 ), SlabScene( Integ::PTSMS, SlabMedium::None, 16 ),
		trGlobal, 0.01, trials, seed, only );
	RatioRow( "B3: global absorber / none, PT+SMS NM",
		SlabScene( Integ::NMSMS, SlabMedium::Global, 4096 ), SlabScene( Integ::NMSMS, SlabMedium::None, 1024 ),
		trGlobal, 0.01, trials, seed, only );

	// C: extended SMS has no medium segment laws; a scene with media runs
	// legacy SMS (ObjectManager / ExtendedModeActive route it), so the
	// extended-on render must equal the closed form, not go black.
	RatioRow( "C: extended, interior absorber / legacy none",
		SlabScene( Integ::PTSMSExtended, SlabMedium::Homogeneous, spp ), SlabScene( Integ::PTSMS, SlabMedium::None, spp ),
		trInterior, 0.01, trials, seed, only );
	RatioRow( "C: extended, global absorber / legacy none",
		SlabScene( Integ::PTSMSExtended, SlabMedium::Global, 1024 ), SlabScene( Integ::PTSMS, SlabMedium::None, 16 ),
		trGlobal, 0.01, trials, seed, only );

	std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount ? 1 : 0;
}
