//////////////////////////////////////////////////////////////////////
//
//  SpectralOnlyEmitterTest.cpp - DL-396 regression: an area emitter whose
//    exitance is a PHYSICAL spectrum with no RGB authoring -- a
//    `piecewise_linear_function` bound as a painter
//    (`Function1DSpectralPainter`) -- must be a light like any other.
//
//    THE BUG: `Function1DSpectralPainter::GetColor` returned BLACK, so the
//    emitter's RGB `averageRadiantExitance` was zero and
//    `LightSampler::Prepare` left it out of the light list.  Its
//    per-wavelength radiance (`GetRadianceNM`) was right, so a spectral
//    render still saw it when a camera/BSDF path HIT it, but no
//    light-rooted strategy could select it: no NEE, no BDPT/VCM/MLT light
//    subpath, no photons, no SMS.  An RGB render showed it black outright.
//
//    THE FIX (one function, every side): the painter now carries the
//    spectrum's RGB projections -- as a reflectance under D65 (`GetColor`)
//    and as a source (`IPainter::GetRadianceColor`, what the spectral film
//    resolves the radiance to) -- and emitters read the source view.  The
//    light-selection weight / photon power stays the existing
//    wavelength-independent RGB proxy, now positive whenever the spectrum
//    has visible luminance; `PdfSelectLuminary` reads the same table.
//
//    ROWS
//      A. A 2x2 emitter behind the camera lights a 0.8 grey wall
//         (EmitterAverageExitanceTest's rig) with a chromatic ramp SPD:
//         Lambertian and Phong(N=2) luminaires, PT RGB, PT / BDPT / VCM /
//         MLT spectral, hwss FALSE / TRUE, against the closed-form wall
//         luminance (4 salted renders each).
//      B. Mixed lights: one RGB emitter + one spectral-only emitter side by
//         side -- the selection pmf must partition correctly between them
//         (closed form = sum of both).
//      C. SMS (sms_k1_refract): the light swapped for a spectral-only SPD
//         equal to the Y-normalised D65 the white RGB control radiates, so
//         both emit the SAME spectral radiance; snell / uniform / extended,
//         spectral hwss FALSE / TRUE and RGB: the caustic must be lit and
//         agree with the RGB-authored control.
//
//  Tabs: 4
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "SMSRenderTestSupport.h"
#include <sstream>
#include "../src/Library/Utilities/Color/ColorUtils.h"
#include "../src/Library/Utilities/Color/RGBSpectra.h"

static const double kPi = 3.14159265358979323846;

//////////////////////////////////////////////////////////////////////
// Spectra
//////////////////////////////////////////////////////////////////////

// Chromatic ramp (reddish), in the Rec.709 gamut.
static double RampSPD( double nm )
{
	if( nm <= 550.0 ) return 0.4 + 0.6 * ( nm - 380.0 ) / 170.0;
	return 1.0 + 0.6 * ( nm - 550.0 ) / 230.0;
}
static const char* kRampChunk =
	"piecewise_linear_function\n{\n\tname pnt_emit\n\tcp 380 0.4\n\tcp 550 1.0\n\tcp 780 1.6\n}\n\n";

//! Int F ybar / Int ybar over [380, 780] -- the film luminance of a unit
//! source with spectrum F.
static double LuminanceOf( double (*F)( double ) )
{
	double num = 0, den = 0;
	for( int i = 0; i <= 4000; i++ ) {
		const double nm = 380.0 + 0.1 * i;
		XYZPel cmf;
		if( !ColorUtils::XYZFromNM( cmf, nm ) ) continue;
		num += F( nm ) * cmf.Y;
		den += cmf.Y;
	}
	return num / den;
}

static double Luminance( const std::vector<RISEColor>& px )
{
	double sum = 0;
	for( const RISEColor& c : px ) sum += 0.2126 * c.base.r + 0.7152 * c.base.g + 0.0722 * c.base.b;
	return px.empty() ? -1.0 : sum / double( px.size() );
}

struct Stat { double mean, sdMean; };

static Stat SaltedLuminance( const std::string& scene, const char* tag, int n )
{
	std::vector<double> v;
	for( int i = 0; i < n; i++ ) {
		const RenderResult r = Render( scene, tag );
		if( !r.ok ) { Check( false, std::string( tag ) + ": render succeeded" ); return { -1.0, 0.0 }; }
		v.push_back( Luminance( r.pixels ) );
	}
	const Stats s = Summarize( v );
	return { s.mean, s.sd / std::sqrt( double( n ) ) };
}

//////////////////////////////////////////////////////////////////////
// Rows A/B: closed-form wall
//////////////////////////////////////////////////////////////////////
static const char* kGeometry =
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.8 0.8 0.8\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad\n\tpta -1 -1 0\n\tptb 1 -1 0\n\tptc 1 1 0\n\tptd -1 1 0\n}\n\n"
	"standard_object\n{\n\tname obj_quad\n\tgeometry quad\n\tmaterial mat_diffuse\n}\n\n";

static std::string EmitterQuad( const char* name, const char* mat, double x0, double x1 )
{
	std::ostringstream s;
	s << "clippedplane_geometry\n{\n\tname geo_" << name << "\n\tpta " << x0 << " 1.0 6.0\n\tptb " << x1
	  << " 1.0 6.0\n\tptc " << x1 << " -1.0 6.0\n\tptd " << x0 << " -1.0 6.0\n}\n\n"
	  << "standard_object\n{\n\tname obj_" << name << "\n\tgeometry geo_" << name << "\n\tmaterial " << mat << "\n}\n\n";
	return s.str();
}

static std::string LuminaireMaterial( const char* name, const char* painter, int phongN )
{
	std::ostringstream s;
	if( phongN > 0 ) {
		s << "scalar_painter\n{\n\tname pnt_N_" << name << "\n\tvalue " << phongN << "\n}\n\n"
		  << "phong_luminaire_material\n{\n\tname " << name << "\n\texitance " << painter
		  << "\n\tN pnt_N_" << name << "\n\tscale 10.0\n\tmaterial none\n}\n\n";
	} else {
		s << "lambertian_luminaire_material\n{\n\tname " << name << "\n\texitance " << painter
		  << "\n\tscale 10.0\n\tmaterial none\n}\n\n";
	}
	return s.str();
}

static const char* kShaderPT = "standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n";
static const char* kShaderBD = "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";

static std::string Rasterizer( const std::string& kind, bool hwss )
{
	const char* hw = hwss ? "TRUE" : "FALSE";
	char b[1024];
	if( kind == "PT RGB" ) {
		std::snprintf( b, sizeof b, "%spathtracing_pel_rasterizer\n{\n\tsamples 256\n\toidn_denoise FALSE\n\tpixel_filter box\n\tmax_diffuse_bounce 1\n}\n", kShaderPT );
	} else if( kind == "BDPT RGB" ) {
		std::snprintf( b, sizeof b, "%sbdpt_pel_rasterizer\n{\n\tmax_eye_depth 2\n\tmax_light_depth 2\n\tsamples 256\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n", kShaderBD );
	} else if( kind == "PT spectral" ) {
		std::snprintf( b, sizeof b, "%spathtracing_spectral_rasterizer\n{\n\tsamples 256\n\toidn_denoise FALSE\n\tpixel_filter box\n\tnmbegin 380\n\tnmend 780\n"
			"\tnum_wavelengths 16\n\tspectral_samples 1\n\thwss %s\n\tmax_diffuse_bounce 1\n}\n", kShaderPT, hw );
	} else if( kind == "BDPT spectral" ) {
		std::snprintf( b, sizeof b, "%sbdpt_spectral_rasterizer\n{\n\tmax_eye_depth 2\n\tmax_light_depth 2\n\tsamples 256\n\tnmbegin 380\n\tnmend 780\n"
			"\tnum_wavelengths 16\n\tspectral_samples 1\n\thwss %s\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n", kShaderBD, hw );
	} else if( kind == "VCM spectral" ) {
		std::snprintf( b, sizeof b, "%svcm_spectral_rasterizer\n{\n\tmax_eye_depth 2\n\tmax_light_depth 2\n\tsamples 256\n\tnmbegin 380\n\tnmend 780\n"
			"\tnum_wavelengths 16\n\tspectral_samples 1\n\thwss %s\n\toidn_denoise FALSE\n\tpixel_filter box\n\tmerge_radius 0.0\n}\n", kShaderBD, hw );
	} else {	// MLT spectral
		std::snprintf( b, sizeof b, "%smlt_spectral_rasterizer\n{\n\tmax_eye_depth 2\n\tmax_light_depth 2\n\tbootstrap_samples 20000\n\tchains 64\n"
			"\tmutations_per_pixel 256\n\tlarge_step_prob 0.3\n\tnmbegin 380\n\tnmend 780\n\thwss %s\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n", kShaderBD, hw );
	}
	return b;
}

//! Mean over the pixel centres of the wall LUMINANCE from an emitter quad
//! x in [x0, x1], y in [-1, 1], z = 6 facing the wall, source luminance Y
//! (exitance luminance x scale), Lambertian (n = 0) or Phong cos^n.
static double ClosedForm( double x0, double x1, double Y, int n )
{
	const int W = 24, G = 120;
	const double kScale = 10.0, rho = 0.8;
	const double half = 3.5 * std::tan( 15.0 * kPi / 180.0 );
	const double ang = n > 0 ? ( n + 1 ) / ( 2.0 * kPi ) : 1.0 / kPi;
	const double dA = ( x1 - x0 ) * 2.0 / ( double( G ) * G );
	double acc = 0;
	for( int j = 0; j < W; j++ ) for( int i = 0; i < W; i++ ) {
		const double px = half * ( 2.0 * ( i + 0.5 ) / W - 1.0 );
		const double py = half * ( 2.0 * ( j + 0.5 ) / W - 1.0 );
		double E = 0;
		for( int b = 0; b < G; b++ ) for( int a = 0; a < G; a++ ) {
			const double ex = x0 + ( x1 - x0 ) * ( a + 0.5 ) / G;
			const double ey = -1.0 + 2.0 * ( b + 0.5 ) / G;
			const double dx = px - ex, dy = py - ey, dz = 6.0;
			const double d2 = dx * dx + dy * dy + dz * dz;
			const double cs = dz / std::sqrt( d2 );
			const double Le = kScale * Y * ang * ( n > 0 ? std::pow( cs, n ) : 1.0 );
			E += Le * cs * cs / d2 * dA;
		}
		acc += rho / kPi * E;
	}
	return acc / ( W * W );
}

static void Gate( const std::string& label, const Stat& s, double truth, double slack )
{
	const double dev = s.mean - truth;
	std::printf( "  %-44s %.6f (sd_mean %.6f)  truth %.6f  ratio %.4f  z %.2f\n",
		label.c_str(), s.mean, s.sdMean, truth, s.mean / truth, s.sdMean > 0 ? dev / s.sdMean : 0.0 );
	Check( s.mean > 0.05 * truth, label + ": emitter lights the wall" );
	Check( std::fabs( dev ) <= 3.0 * s.sdMean + slack * truth, label + ": matches the closed form within 3 sigma" );
}

static void RunClosedForm()
{
	std::cout << "Row A: spectral-only emitter vs closed form\n";
	const double Y = LuminanceOf( RampSPD );
	std::printf( "  ramp SPD luminance %.6f\n", Y );
	struct R { const char* kind; bool hwss; double slack; };
	static const R rows[] = {
		{ "PT RGB", false, 0.01 },
		{ "BDPT RGB", false, 0.01 },
		{ "PT spectral", false, 0.01 },
		{ "PT spectral", true, 0.01 },
		{ "BDPT spectral", false, 0.01 },
		{ "BDPT spectral", true, 0.01 },
		{ "VCM spectral", false, 0.01 },
		{ "VCM spectral", true, 0.01 },
		{ "MLT spectral", false, 0.03 },
	};
	for( int phongN : { 0, 2 } ) {
		const double truth = ClosedForm( -1.0, 1.0, Y, phongN );
		for( const R& r : rows ) {
			std::string scene = "RISE ASCII SCENE 7\n";
			scene += kGeometry;
			scene += kRampChunk;
			scene += LuminaireMaterial( "mat_emit", "pnt_emit", phongN );
			scene += EmitterQuad( "emit", "mat_emit", -1.0, 1.0 );
			scene += Rasterizer( r.kind, r.hwss );
			const std::string label = std::string( phongN ? "phong " : "lambert " ) + r.kind + ( r.hwss ? " hwss" : "" );
			Gate( label, SaltedLuminance( scene, "rowA", 4 ), truth, r.slack );
		}
	}
}

static void RunMixed()
{
	std::cout << "Row B: one RGB emitter + one spectral-only emitter\n";
	const double Yspec = LuminanceOf( RampSPD );
	const double Yrgb = 0.6;
	const double truth = ClosedForm( -1.0, 0.0, Yrgb, 0 ) + ClosedForm( 0.0, 1.0, Yspec, 0 );
	// `control`: the spectral-only emitter replaced by an RGB grey of the
	// same luminance -- separates estimator offsets from the selection pmf.
	for( const bool control : { false, true } )
	for( const char* kind : { "PT RGB", "PT spectral", "BDPT spectral", "VCM spectral" } ) {
		std::string scene = "RISE ASCII SCENE 7\n";
		scene += kGeometry;
		if( control ) {
			char c[160];
			std::snprintf( c, sizeof c, "uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor %.6f %.6f %.6f\n}\n\n", Yspec, Yspec, Yspec );
			scene += c;
		} else {
			scene += kRampChunk;
		}
		scene += "uniformcolor_painter\n{\n\tname pnt_rgb\n\tcolor 0.6 0.6 0.6\n}\n\n";
		scene += LuminaireMaterial( "mat_spec", "pnt_emit", 0 );
		scene += LuminaireMaterial( "mat_rgb", "pnt_rgb", 0 );
		scene += EmitterQuad( "rgb", "mat_rgb", -1.0, 0.0 );
		scene += EmitterQuad( "spec", "mat_spec", 0.0, 1.0 );
		scene += Rasterizer( kind, false );
		Gate( std::string( control ? "mixed RGB-only control " : "mixed " ) + kind, SaltedLuminance( scene, "rowB", 4 ), truth, 0.01 );
	}
}

//////////////////////////////////////////////////////////////////////
// Row C: SMS caustic
//////////////////////////////////////////////////////////////////////
static std::string D65Chunk()
{
	std::ostringstream s;
	s << "piecewise_linear_function\n{\n\tname pnt_light\n";
	for( int nm = 380; nm <= 780; nm += 5 ) {
		s << "\tcp " << nm << " " << RGBIlluminantSpectrum::ReferenceIlluminant( Scalar( nm ) ) << "\n";
	}
	s << "}";
	return s.str();
}

static std::string SMSFixture( bool spectralOnly, const char* mode, bool spectral, bool hwss )
{
	std::string scene = ReadScene( "scenes/Tests/SMS/sms_k1_refract.RISEscene" );
	Check( !scene.empty(), "canonical SMS scene loaded" );
	ReplaceFirstChunk( scene, "film", "film\n{\n width 32\n height 24\n}" );
	ReplaceNamedChunk( scene, "uniformcolor_painter", "name\t\t\t\tpnt_light",
		spectralOnly ? D65Chunk() : std::string( "uniformcolor_painter\n{\n name pnt_light\n color 1 1 1\n}" ) );
	const std::string m( mode );
	std::ostringstream rast;
	rast << ( spectral ? "pathtracing_spectral_rasterizer" : "pathtracing_pel_rasterizer" )
	     << "\n{\n samples 256\n oidn_denoise FALSE\n pixel_filter box\n sms_enabled TRUE\n"
	     << " sms_seeding " << ( m == "uniform" ? "uniform" : "snell" )
	     << "\n sms_max_iterations 30\n sms_threshold 1e-4\n sms_max_chain_depth 5\n sms_biased TRUE\n";
	if( m == "extended" ) rast << " sms_extended TRUE\n";
	if( spectral ) rast << " num_wavelengths 16\n hwss " << ( hwss ? "TRUE" : "FALSE" ) << "\n";
	rast << "}\n";
	ReplaceFirstChunk( scene, "pathtracing_pel_rasterizer", rast.str() );
	return scene;
}

static void RunSMS()
{
	std::cout << "Row C: SMS caustic, spectral-only D65 SPD vs RGB white control (same spectral radiance)\n";
	struct R { const char* mode; bool spectral; bool hwss; };
	static const R rows[] = {
		{ "snell", true, false }, { "snell", true, true },
		{ "uniform", true, false },
		{ "extended", true, false }, { "extended", true, true },
		{ "snell", false, false }, { "extended", false, false },
	};
	for( const R& r : rows ) {
		const Stat spec = SaltedLuminance( SMSFixture( true, r.mode, r.spectral, r.hwss ), "smsSpec", 4 );
		const Stat ctrl = SaltedLuminance( SMSFixture( false, r.mode, r.spectral, r.hwss ), "smsCtrl", 4 );
		const std::string label = std::string( "SMS " ) + r.mode + ( r.spectral ? " spectral" : " RGB" ) + ( r.hwss ? " hwss" : "" );
		const double sd = std::sqrt( spec.sdMean * spec.sdMean + ctrl.sdMean * ctrl.sdMean );
		std::printf( "  %-36s spectral-only %.6f (sd %.6f)  RGB control %.6f (sd %.6f)  ratio %.4f  z %.2f\n",
			label.c_str(), spec.mean, spec.sdMean, ctrl.mean, ctrl.sdMean,
			ctrl.mean > 0 ? spec.mean / ctrl.mean : -1.0, sd > 0 ? ( spec.mean - ctrl.mean ) / sd : 0.0 );
		Check( ctrl.mean > 0, label + ": control lit" );
		Check( spec.mean > 0.5 * ctrl.mean, label + ": spectral-only emitter's caustic is lit" );
		Check( std::fabs( spec.mean - ctrl.mean ) <= 3.0 * sd + 0.01 * ctrl.mean, label + ": agrees with the RGB control within 3 sigma" );
	}
}

int main( int argc, char** argv )
{
	g_seedBase = 396000u;
	const std::string only = argc > 1 ? argv[1] : "";
	if( only.empty() || only == "A" ) RunClosedForm();
	if( only.empty() || only == "B" ) RunMixed();
	if( only.empty() || only == "C" ) RunSMS();
	std::cout << "SpectralOnlyEmitterTest: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
