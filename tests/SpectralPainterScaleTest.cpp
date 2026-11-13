//////////////////////////////////////////////////////////////////////
//
//  SpectralPainterScaleTest.cpp - DL-464 regression: `spectral_painter`
//    (SpectralColorPainter) and `blackbody_painter` (BlackBodyPainter)
//    must give an RGB render and a spectral render of the same scene the
//    same answer.
//
//    THE BUG: `spectral_painter`'s `GetColor` was `scale * mean_bins(cmf
//    * F)` -- an unnormalised bin mean -- while `GetColorNM` /
//    `GetRadianceNM` returned the UNSCALED F; `blackbody_painter`'s
//    `GetColor` was the bin mean of the spectrum, max-channel normalised
//    to 1 when `normalize` (ignoring `scale`), and `normalize` divided the
//    authored `scale` in place, so every keyframed RegenerateData divided
//    it by the Planck peak again.
//
//    THE RULING (2026-10-09): `scale` multiplies the spectrum on BOTH
//    paths; the RGB views are the DL-396 projections of the SCALED
//    spectrum (`GetColor` = reflectance under D65, `GetRadianceColor` =
//    the source view the spectral film resolves).
//
//    ROWS
//      A. Painter-level: NM samples carry `scale`; RGB views equal
//         `ProjectPhysicalSpectrumToRGB` of the scaled spectrum; a flat
//         reflectance projects to grey `scale`; blackbody `normalize`
//         peaks at `scale` and keyframe regeneration is idempotent.
//      B. Emitter: a `spectral_painter` (scale 4) and a `blackbody_painter`
//         (scale 2, normalize) luminaire light a white wall -- PT pel vs
//         PT spectral (num_wavelengths 160), 4 salted renders each, per
//         channel.
//      C. Reflectance slot: a `spectral_painter` (scale 0.8) wall under an
//         RGB-white emitter -- PT pel vs PT spectral, per channel.
//
//  Tabs: 4
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "SMSRenderTestSupport.h"
#include <sstream>
#include <string_view>
#include "../src/Library/RISE_API.h"
#include "../src/Library/Painters/Painter.h"
#include "../src/Library/Painters/BlackBodyPainter.h"
#include "../src/Library/Utilities/Color/SpectralPacket.h"
#include "../src/Library/Utilities/PiecewiseLinearFunction.h"

using namespace RISE::Implementation;

//////////////////////////////////////////////////////////////////////
// Row A: painter level
//////////////////////////////////////////////////////////////////////

static double Ramp( double nm ) { return 0.2 + 0.6 * ( nm - 400.0 ) / 300.0; }

static Scalar EvalScaledRamp( const void* ctx, const Scalar nm )
{
	const double s = *static_cast<const double*>( ctx );
	// The painter's packet is piecewise CONSTANT over 31 bins of 300/31 nm,
	// bin i holding F at its left edge (Ramp is linear, so the control
	// points' interpolation reproduces it exactly).
	const double delta = 300.0 / 31.0;
	if( nm < 400.0 || nm > 700.0 ) return 0;
	int idx = int( ( nm - 400.0 ) / delta );
	if( idx > 30 ) idx = 30;
	return s * Ramp( 400.0 + delta * idx );
}

static bool Near( const RISEPel& a, const RISEPel& b, double tol )
{
	for( int c = 0; c < 3; c++ ) {
		if( std::fabs( a[c] - b[c] ) > tol * std::max( 1e-12, std::fabs( b[c] ) ) ) return false;
	}
	return true;
}

static IPainter* MakeSpectral( double (*F)( double ), double scale )
{
	PiecewiseLinearFunction1D* pFunc = new PiecewiseLinearFunction1D();
	GlobalLog()->PrintNew( pFunc, __FILE__, __LINE__, "func" );
	for( int nm = 400; nm <= 700; nm += 10 ) {
		pFunc->addControlPoint( std::make_pair( Scalar( nm ), Scalar( F( nm ) ) ) );
	}
	// Job::AddSpectralColorPainter's construction: num_freq = number of
	// control points, bins over [nmbegin, nmend].
	const SpectralPacket sp( 400, 700, 31, pFunc );
	IPainter* p = 0;
	RISE_API_CreateSpectralColorPainter( &p, sp, scale );
	safe_release( pFunc );
	return p;
}

static void TestPacketAssignment()
{
	SpectralPacket source( 400, 800, 4 );
	for( unsigned int bin = 0; bin < source.NumBins(); ++bin ) {
		source.SetAtIndex( bin, bin + 1 );
	}

	SpectralPacket sameCount( 400, 600, 4 );
	sameCount = source;
	Check( sameCount.minLambda() == 400 && sameCount.maxLambda() == 800 &&
		sameCount.deltaLambda() == 100 && sameCount.ValueAtNM( 525 ) == 2,
		"packet assignment updates wavelength mapping for an equal bin count" );

	SpectralPacket differentCount;
	differentCount = source;
	Check( differentCount.NumBins() == 4 && differentCount.deltaLambda() == 100,
		"packet assignment copies metadata when the count changes" );
	Check( differentCount.ValueAtNM( 450 ) == 1 && differentCount.ValueAtNM( 525 ) == 2 &&
		differentCount.ValueAtNM( 650 ) == 3 && differentCount.ValueAtNM( 750 ) == 4,
		"packet assignment copies every bin when the count changes" );
	source.SetAtIndex( 1, 9 );
	Check( differentCount.ValueAtNM( 525 ) == 2,
		"packet assignment owns an independent copy of the bins" );
}

static void TestPacketEndpoints()
{
	SpectralPacket empty( 400, 700, 0 );
	const auto xyz = empty.GetXYZ();
	Check( empty.ValueAtNM( 500 ) == 0 && xyz.X == 0 && xyz.Y == 0 && xyz.Z == 0,
		"empty spectrum looks up and projects to zero" );

	SpectralPacket collapsed( 500, 500, 2 );
	collapsed.SetAtIndex( 0, 1 );
	collapsed.SetAtIndex( 1, 2 );
	Check( collapsed.ValueAtNM( 500 ) == 2 && collapsed.ValueAtNM( 501 ) == 0,
		"coincident endpoints select the last bin at the endpoint only" );

	VisibleSpectralPacket visible;
	visible.SetIndex( 39, 3 );
	Check( visible.ValueAtNM( 770 ) == 3 && visible.ValueAtNM( 780 ) == 3 &&
		visible.ValueAtNM( 781 ) == 0,
		"fixed visible spectrum includes 780 nm in the final bin" );
	SpectralPacket_Template<Scalar, 380, 780, 1> singleBin( 2 );
	Check( singleBin.ValueAtNM( 500 ) == 2 && singleBin.ValueAtNM( 780 ) == 2 &&
		singleBin.ValueAtNM( 781 ) == 0, "one fixed bin covers its entire wavelength range" );

	SpectralPacket outsidePDFBand( 730, 780, 5 );
	IProbabilityDensityFunction* pdf = nullptr;
	if( RISE_API_CreatePDF_SpectralPacket( &pdf, outsidePDFBand ) && pdf ) {
		Scalar lookupWeight = 1;
		Scalar directWeight = 1;
		Check( pdf->warp( 0.5, lookupWeight, true ) == 0 && lookupWeight == 0 &&
			pdf->warp( 0.5, directWeight, false ) == 0 && directWeight == 0,
			"empty spectral density produces zero weight in both lookup modes" );
		safe_release( pdf );
	} else {
		Check( false, "empty spectral density is representable through the PDF factory" );
	}

	SpectralPacket lastBinOnly( 400, 700, 11 );
	lastBinOnly.SetAtIndex( 10, 1 );
	if( RISE_API_CreatePDF_SpectralPacket( &pdf, lastBinOnly ) && pdf ) {
		Scalar lookupWeight = 0;
		Scalar directWeight = 0;
		const Scalar lookup = pdf->warp( 0.5, lookupWeight, true );
		const Scalar direct = pdf->warp( 0.5, directWeight, false );
		Check( lookup == direct && direct >= 672 && direct <= 673 &&
			lookupWeight == directWeight && directWeight > 0,
			"spectral density includes the final bin on a fractional wavelength grid" );
		safe_release( pdf );
	} else {
		Check( false, "fractional-grid spectral density is constructible" );
	}

	SpectralPacket visibleBoundary( 380, 754, 33 );
	visibleBoundary.SetAtIndex( 30, 1 );
	SpectralPacket positiveSingleBin( 400, 700, 1 );
	positiveSingleBin.SetAtIndex( 0, 1 );
	const auto checkDensity = [&]( const SpectralPacket& packet, Scalar wavelength, const char* label ) {
		const bool created = RISE_API_CreatePDF_SpectralPacket( &pdf, packet );
		Scalar lookupWeight = 0;
		Scalar directWeight = 0;
		Check( created && pdf && pdf->warp( 0.5, lookupWeight, true ) == wavelength &&
			pdf->warp( 0.5, directWeight, false ) == wavelength && lookupWeight > 0 &&
			lookupWeight == directWeight, label );
		safe_release( pdf );
	};
	checkDensity( visibleBoundary, 720, "spectral density includes the 720 nm boundary bin" );
	checkDensity( positiveSingleBin, 400, "positive one-bin spectral density samples its authored wavelength" );
}

static void TestSpectralFileCounts()
{
	const std::string stem = "rise_spectral_counts_" + std::to_string( ::getpid() );
	const std::string scene = TestTempPath( stem + ".RISEscene" );
	const std::string wavelengths = TestTempPath( stem + "_nm.txt" );
	const std::string amplitudes = TestTempPath( stem + "_amp.txt" );
	if( scene.empty() || wavelengths.empty() || amplitudes.empty() ) {
		Check( false, "spectral file count fixture has a temporary directory" );
		return;
	}

	std::ofstream( scene ) << "RISE ASCII SCENE 7\n\nspectral_painter\n{\n"
		<< "  name file_spectrum\n  nmfile " << wavelengths
		<< "\n  ampfile " << amplitudes << "\n}\n";
	std::ofstream( amplitudes ) << "0.2\n0.4\n";

	const auto loadWithWavelengths = [&]( const char* samples ) {
		std::ofstream( wavelengths ) << samples;
		IJobPriv* job = nullptr;
		if( !RISE_CreateJobPriv( &job ) || !job ) {
			Check( false, "spectral file count fixture creates a Job" );
			return false;
		}
		const bool loaded = job->LoadAsciiSceneViaCst( scene.c_str() );
		safe_release( job );
		return loaded;
	};
	const bool accepted = loadWithWavelengths( "400\n500\n" );
	Check( accepted, "spectral painter loads matching amplitude and wavelength files" );
	if( accepted ) {
		Check( !loadWithWavelengths( "400\n" ), "spectral painter rejects unequal file sample counts" );
		Check( !loadWithWavelengths( "400\nnan\n" ), "spectral painter rejects a nonfinite wavelength" );
		Check( !loadWithWavelengths( "400\n500 junk\n" ), "spectral painter rejects an incomplete file record" );
		const std::string splitRecord = "400" + std::string( 8188, ' ' ) + "500\n";
		Check( !loadWithWavelengths( splitRecord.c_str() ), "spectral painter treats a long physical row as one record" );
	}
	std::remove( scene.c_str() );
	std::remove( wavelengths.c_str() );
	std::remove( amplitudes.c_str() );
}

static void TestPainterLevel()
{
	std::cout << "Row A: painter level" << std::endl;
	RayIntersectionGeometric ri( Ray(), nullRasterizerState );

	// spectral_painter: NM samples carry scale.
	{
		const double s = 4.0;
		IPainter* p = MakeSpectral( Ramp, s );
		bool nmOK = true;
		for( int nm = 400; nm < 700; nm += 7 ) {
			const double expect = EvalScaledRamp( &s, nm );
			if( std::fabs( p->GetColorNM( ri, nm ) - expect ) > 1e-9 * expect ) nmOK = false;
			if( std::fabs( p->GetRadianceNM( ri, nm ) - expect ) > 1e-9 * expect ) nmOK = false;
		}
		Check( nmOK, "spectral_painter: GetColorNM / GetRadianceNM == scale * F" );
		Check( std::fabs( p->GetColorNM( ri, 700.0 ) - s * Ramp( 400.0 + 30.0 * 300.0 / 31.0 ) ) < 1e-9,
			"spectral_painter: nm == nmend reads the last bin (no read past the packet)" );

		const RISEPel refl = ProjectPhysicalSpectrumToRGB( &EvalScaledRamp, &s, false );
		const RISEPel rad = ProjectPhysicalSpectrumToRGB( &EvalScaledRamp, &s, true );
		Check( Near( p->GetColor( ri ), refl, 1e-9 ), "spectral_painter: GetColor == D65 reflectance projection of scale*F" );
		Check( Near( p->GetRadianceColor( ri ), rad, 1e-9 ), "spectral_painter: GetRadianceColor == source projection of scale*F" );
		std::cout << "  ramp scale 4: GetColor (" << refl[0] << ", " << refl[1] << ", " << refl[2]
		          << ")  GetRadianceColor (" << rad[0] << ", " << rad[1] << ", " << rad[2] << ")" << std::endl;
		safe_release( p );
	}

	// A flat reflectance F = 0.5 at scale 1.6 is grey 0.8 under D65
	// (the projection only covers [400,700] of the CIE table, so allow
	// the band-limit loss).
	{
		IPainter* p = MakeSpectral( []( double ) { return 0.5; }, 1.6 );
		const RISEPel c = p->GetColor( ri );
		std::cout << "  flat 0.5 x 1.6: GetColor (" << c[0] << ", " << c[1] << ", " << c[2] << ")" << std::endl;
		Check( std::fabs( c[1] - 0.8 ) < 0.01 && std::fabs( c[0] - c[1] ) < 0.02 && std::fabs( c[2] - c[1] ) < 0.02,
			"spectral_painter: flat 0.5 x scale 1.6 projects to grey ~0.8 as a reflectance" );
		Check( !IsUntintedWhitePainter( *p, RISEPel( 1, 1, 1 ) ),
			"spectral_painter is never white-guarded (IsUntintedWhitePainter)" );
		safe_release( p );
	}

	// blackbody_painter: normalize peaks at scale; RGB views project
	// GetColorNM; keyframed regeneration is idempotent.
	{
		const double T = 3000.0, s = 2.5;
		IPainter* p = 0;
		RISE_API_CreateBlackBodyPainter( &p, T, 400, 700, 30, true, s );
		const double peakM = 0.0029 / T;
		const double atPeak = p->GetColorNM( ri, peakM * 1e9 );
		Check( std::fabs( atPeak - s ) < 1e-9 * s, "blackbody normalize: GetColorNM at the Wien peak == scale" );

		struct Ctx { const IPainter* p; const RayIntersectionGeometric* ri; } ctx = { p, &ri };
		auto eval = []( const void* c, const Scalar nm ) -> Scalar {
			const Ctx* x = static_cast<const Ctx*>( c );
			return x->p->GetColorNM( *x->ri, nm );
		};
		const RISEPel refl = ProjectPhysicalSpectrumToRGB( eval, &ctx, false );
		const RISEPel rad = ProjectPhysicalSpectrumToRGB( eval, &ctx, true );
		Check( Near( p->GetColor( ri ), refl, 1e-9 ), "blackbody: GetColor == D65 reflectance projection of GetColorNM" );
		Check( Near( p->GetRadianceColor( ri ), rad, 1e-9 ), "blackbody: GetRadianceColor == source projection of GetColorNM" );
		std::cout << "  blackbody 3000K norm x2.5: GetRadianceColor (" << rad[0] << ", " << rad[1] << ", " << rad[2] << ")" << std::endl;

		const double before = p->GetColorNM( ri, 550 );
		IKeyframable* k = dynamic_cast<IKeyframable*>( p );
		IKeyframeParameter* kp = k->KeyframeFromParameters( "temperature", "3000" );
		for( int i = 0; i < 3; i++ ) {
			k->SetIntermediateValue( *kp );
			k->RegenerateData();
		}
		safe_release( kp );
		Check( std::fabs( p->GetColorNM( ri, 550 ) - before ) < 1e-12 * before,
			"blackbody normalize: repeated keyframe RegenerateData leaves the scale alone" );
		safe_release( p );
	}

	// Projection helper for scene-tuning: a normalised 5800 K blackbody
	// source view (planetary_survey's star).
	{
		IPainter* p = 0;
		RISE_API_CreateBlackBodyPainter( &p, 5800, 400, 700, 30, true, 1.0 );
		const RISEPel rad = p->GetRadianceColor( ri );
		std::cout << "  blackbody 5800K norm x1: GetRadianceColor (" << rad[0] << ", " << rad[1] << ", " << rad[2] << ")" << std::endl;
		safe_release( p );
	}
}

//////////////////////////////////////////////////////////////////////
// Rows B/C: renders
//////////////////////////////////////////////////////////////////////

struct ChanStat { double mean[3]; double sdMean[3]; };

static ChanStat SaltedChannels( const std::string& scene, const char* tag, int n )
{
	std::vector<double> v[3];
	for( int i = 0; i < n; i++ ) {
		const RenderResult r = Render( scene, tag );
		if( !r.ok ) { Check( false, std::string( tag ) + ": render succeeded" ); return ChanStat{ { -1, -1, -1 }, { 0, 0, 0 } }; }
		double s[3] = { 0, 0, 0 };
		for( const RISEColor& c : r.pixels ) { s[0] += c.base.r; s[1] += c.base.g; s[2] += c.base.b; }
		for( int k = 0; k < 3; k++ ) v[k].push_back( s[k] / double( r.pixels.size() ) );
	}
	ChanStat out;
	for( int k = 0; k < 3; k++ ) {
		const Stats st = Summarize( v[k] );
		out.mean[k] = st.mean;
		out.sdMean[k] = st.sd / std::sqrt( double( n ) );
	}
	return out;
}

static const char* kBase =
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad\n\tpta -1 -1 0\n\tptb 1 -1 0\n\tptc 1 1 0\n\tptd -1 1 0\n}\n\n"
	"clippedplane_geometry\n{\n\tname geo_emit\n\tpta -1 1 6\n\tptb 1 1 6\n\tptc 1 -1 6\n\tptd -1 -1 6\n}\n\n";

static std::string Rasterizer( bool spectral )
{
	std::ostringstream s;
	s << "standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n";
	if( spectral ) {
		s << "pathtracing_spectral_rasterizer\n{\n\tsamples 256\n\toidn_denoise FALSE\n\tpixel_filter box\n"
		     "\tnmbegin 380\n\tnmend 780\n\tnum_wavelengths 160\n\tspectral_samples 1\n\thwss FALSE\n\tmax_diffuse_bounce 1\n}\n";
	} else {
		s << "pathtracing_pel_rasterizer\n{\n\tsamples 256\n\toidn_denoise FALSE\n\tpixel_filter box\n\tmax_diffuse_bounce 1\n}\n";
	}
	return s.str();
}

static std::string Scene( const std::string& painters, const char* wallPainter, const char* emitPainter, bool spectral )
{
	std::ostringstream s;
	s << "RISE ASCII SCENE 7\n" << kBase << painters
	  << "lambertian_material\n{\n\tname mat_wall\n\treflectance " << wallPainter << "\n}\n\n"
	  << "lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance " << emitPainter << "\n\tscale 10.0\n\tmaterial none\n}\n\n"
	  << "standard_object\n{\n\tname obj_wall\n\tgeometry quad\n\tmaterial mat_wall\n}\n\n"
	  << "standard_object\n{\n\tname obj_emit\n\tgeometry geo_emit\n\tmaterial mat_emit\n}\n\n"
	  << Rasterizer( spectral );
	return s.str();
}

static void CompareRGBSpectral( const char* label, const std::string& painters, const char* wall, const char* emit, double tol )
{
	const ChanStat rgb = SaltedChannels( Scene( painters, wall, emit, false ), "dl464_rgb", 4 );
	const ChanStat spe = SaltedChannels( Scene( painters, wall, emit, true ), "dl464_spec", 4 );
	const char* ch = "RGB";
	for( int k = 0; k < 3; k++ ) {
		const double ratio = spe.mean[k] / rgb.mean[k];
		std::cout << "  " << label << " " << ch[k] << ": RGB " << rgb.mean[k] << " +/- " << rgb.sdMean[k]
		          << "  spectral " << spe.mean[k] << " +/- " << spe.sdMean[k] << "  ratio " << ratio << std::endl;
		Check( rgb.mean[k] > 0 && std::fabs( ratio - 1.0 ) < tol,
			std::string( label ) + " channel " + ch[k] + ": spectral / RGB within band" );
	}
}

static void TestRenders()
{
	std::cout << "Row B: emitters (PT pel vs PT spectral, 160 wavelengths)" << std::endl;
	const std::string white = "uniformcolor_painter\n{\n\tname pnt_wall\n\tcolor 0.8 0.8 0.8\n}\n\n";
	CompareRGBSpectral( "spectral_painter emitter scale 4",
		white + "spectral_painter\n{\n\tname pnt_emit\n\tnmbegin 380\n\tnmend 780\n\tscale 4\n"
		"\tcp 380 0.2\n\tcp 480 0.5\n\tcp 580 1.0\n\tcp 680 0.6\n\tcp 780 0.3\n}\n\n",
		"pnt_wall", "pnt_emit", 0.03 );
	CompareRGBSpectral( "blackbody_painter emitter 4000K scale 2",
		white + "blackbody_painter\n{\n\tname pnt_emit\n\ttemperature 4000\n\tscale 2\n}\n\n",
		"pnt_wall", "pnt_emit", 0.03 );

	std::cout << "Row C: reflectance slot (spectral_painter wall under an RGB white emitter)" << std::endl;
	CompareRGBSpectral( "spectral_painter reflectance scale 0.8",
		"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1 1 1\n}\n\n"
		"spectral_painter\n{\n\tname pnt_wall\n\tnmbegin 380\n\tnmend 780\n\tscale 0.8\n"
		"\tcp 380 0.3\n\tcp 480 0.9\n\tcp 580 0.6\n\tcp 680 0.4\n\tcp 780 0.4\n}\n\n",
		"pnt_wall", "pnt_emit", 0.03 );
	// Control: the same rig with an RGB-authored wall -- the spectral
	// estimator's own offset against RGB, independent of DL-464.
	CompareRGBSpectral( "RGB-authored reflectance control",
		"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1 1 1\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_wall\n\tcolor 0.3 0.45 0.3\n}\n\n",
		"pnt_wall", "pnt_emit", 0.03 );
}

int main( int argc, char** argv )
{
	const bool packetOnly = argc == 2 && std::string_view( argv[1] ) == "--packet-only";
	std::cout << "SpectralPainterScaleTest (DL-464)" << std::endl;
	TestPacketAssignment();
	TestPacketEndpoints();
	TestSpectralFileCounts();
	if( !packetOnly ) {
		TestPainterLevel();
		TestRenders();
	}
	std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
