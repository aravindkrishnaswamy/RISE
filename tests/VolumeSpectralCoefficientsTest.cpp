//////////////////////////////////////////////////////////////////////
//
//  VolumeSpectralCoefficientsTest.cpp - Unit test for G1 (vitreous
//    enamel): genuine per-wavelength sigma_a(lambda)/sigma_s(lambda)
//    in HomogeneousMedium's spectral (NM) path.
//
//  Before G1, HomogeneousMedium::GetCoefficientsNM() collapsed the RGB
//  triples to a single luminance scalar, so a red enamel rendered
//  ACHROMATIC in any *_spectral_* rasterizer.  G1 lets the medium carry
//  optional IFunction1D coefficient curves; when present, the NM path
//  evaluates sigma(lambda) directly.  This test exercises that path at
//  the medium level (no scene / rasterizer needed):
//
//    A. Curve-driven coefficients: GetCoefficientsNM(nm) returns
//       sigma_a(nm) + sigma_s(nm) and sigma_s(nm) at several wavelengths.
//    B. Chromatic transmittance: a green-absorbing / red-transmitting
//       curve yields Tr(650nm) >> Tr(550nm) — the medium is genuinely
//       red in spectral mode (the whole point of G1).
//    C. Pure absorber: a null scattering curve => sigma_s == 0 in NM,
//       sigma_t == sigma_a(nm) (the enamel colorant case).
//    D. Luminance fallback: a medium built WITHOUT curves returns a
//       wavelength-INDEPENDENT luminance-weighted blend — the fallback
//       is distinct from the spectral path.  (The numeric byte-identity
//       to pre-G1 is proven by the full suite staying green, not by this
//       case, which only checks the fallback's wavelength-flat shape.)
//    E. Sampling consistency: SampleDistanceNM's scatter probability
//       matches 1 - exp(-sigma_t(nm) * maxDist) at the queried
//       wavelength (i.e. it shares sigma_t(nm) with GetCoefficientsNM).
//    F. RGB path untouched: GetCoefficients()/EvalTransmittance() still
//       return the per-channel RGB triples for a spectral medium.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <vector>

#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IMedium.h"
#include "../src/Library/Interfaces/IFunction1D.h"
#include "../src/Library/Interfaces/IPiecewiseFunction.h"
#include "../src/Library/Interfaces/IPhaseFunction.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/Color/RGBSpectra.h"
#include "../src/Library/Materials/HomogeneousMedium.h"
#include "../src/Library/Materials/HeterogeneousMedium.h"
#include "../src/Library/Interfaces/IPainter.h"
#include <algorithm>

using namespace RISE;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const char* msg )
{
	if( cond ) {
		++passCount;
		std::cout << "  [ ok ] " << msg << std::endl;
	} else {
		++failCount;
		std::cout << "  [FAIL] " << msg << std::endl;
	}
}

static bool Close( double a, double b, double eps = 1e-6 )
{
	return std::fabs( a - b ) <= eps * std::fmax( 1.0, std::fmax( std::fabs(a), std::fabs(b) ) );
}

// Build a named-free IPiecewiseFunction1D curve from (x,y) samples.
static IFunction1D* MakeCurve( const std::vector<double>& x, const std::vector<double>& y )
{
	IPiecewiseFunction1D* f = 0;
	RISE_API_CreatePiecewiseLinearFunction1D( &f );
	f->addControlPoints( (int)x.size(), &x[0], &y[0] );
	return f;
}

// Green-absorbing / red-transmitting absorption curve (like a gold-ruby
// red enamel): sigma_a is high in the green (~550nm), low in the red
// (~650nm).  Units are 1/scene-unit.
static IFunction1D* MakeRedAbsorber()
{
	std::vector<double> x = { 400, 500, 550, 600, 650, 700 };
	std::vector<double> y = { 3.0, 4.0, 5.0, 2.0, 0.5, 0.3 };
	return MakeCurve( x, y );
}

// Flat scattering curve (glass turbidity): 0.2 everywhere.
static IFunction1D* MakeFlatScatter()
{
	std::vector<double> x = { 400, 700 };
	std::vector<double> y = { 0.2, 0.2 };
	return MakeCurve( x, y );
}

// Evaluate a curve directly for the expected value.
static double Eval( const IFunction1D* f, double nm ) { return f->Evaluate( nm ); }

//----------------------------------------------------------------------
// A + B + F: full spectral medium (absorption + scattering curves)
//----------------------------------------------------------------------
static void TestSpectralAbsorptionScattering()
{
	std::cout << "A/B/F: spectral medium with absorption + scattering curves" << std::endl;

	IFunction1D* absCurve = MakeRedAbsorber();
	IFunction1D* scaCurve = MakeFlatScatter();

	IPhaseFunction* phase = 0;
	RISE_API_CreateIsotropicPhaseFunction( &phase );

	// RGB triples are a rough preview only; the curves drive NM.
	IMedium* medium = 0;
	RISE_API_CreateHomogeneousMediumSpectral( &medium,
		RISEPel( 1.0, 3.0, 0.4 ),		// preview sigma_a
		RISEPel( 0.2, 0.2, 0.2 ),		// preview sigma_s
		RISEPel( 0.0, 0.0, 0.0 ),		// no emission
		absCurve, scaCurve, *phase );

	const Point3 p( 0, 0, 0 );
	const Ray ray( p, Vector3( 0, 0, 1 ) );

	// A: coefficients at several wavelengths equal the curve sums.
	const double lambdas[] = { 450, 550, 650 };
	for( double nm : lambdas ) {
		MediumCoefficientsNM c = medium->GetCoefficientsNM( p, nm );
		const double ea = Eval( absCurve, nm );
		const double es = Eval( scaCurve, nm );
		Check( Close( c.sigma_s, es ), "  sigma_s(nm) == scattering curve" );
		Check( Close( c.sigma_t, ea + es ), "  sigma_t(nm) == sigma_a + sigma_s curves" );
	}

	// B: chromatic transmittance — red passes, green is absorbed.
	const double d = 1.0;
	const double trGreen = medium->EvalTransmittanceNM( ray, d, 550.0 );
	const double trRed   = medium->EvalTransmittanceNM( ray, d, 650.0 );
	Check( Close( trGreen, std::exp( -(Eval(absCurve,550)+Eval(scaCurve,550)) * d ) ),
		"  Tr(550nm) == exp(-sigma_t(550)*d)" );
	Check( Close( trRed,   std::exp( -(Eval(absCurve,650)+Eval(scaCurve,650)) * d ) ),
		"  Tr(650nm) == exp(-sigma_t(650)*d)" );
	Check( trRed > trGreen * 10.0,
		"  Tr(650nm) >> Tr(550nm): medium is genuinely RED in spectral mode" );

	// F: RGB path untouched — still the per-channel triples.
	MediumCoefficients rgb = medium->GetCoefficients( p );
	Check( Close( rgb.sigma_t[0], 1.2 ) && Close( rgb.sigma_t[1], 3.2 ) && Close( rgb.sigma_t[2], 0.6 ),
		"  RGB GetCoefficients() unchanged (sigma_a+sigma_s per channel)" );
	RISEPel trRGB = medium->EvalTransmittance( ray, d );
	Check( Close( trRGB[0], std::exp(-1.2) ) && Close( trRGB[2], std::exp(-0.6) ),
		"  RGB EvalTransmittance() per-channel Beer-Lambert unchanged" );

	safe_release( medium );
	safe_release( phase );
	safe_release( absCurve );
	safe_release( scaCurve );
}

//----------------------------------------------------------------------
// C: pure absorber (scattering curve null)
//----------------------------------------------------------------------
static void TestPureAbsorber()
{
	std::cout << "C: pure-absorber spectral medium (null scattering curve)" << std::endl;

	IFunction1D* absCurve = MakeRedAbsorber();
	IPhaseFunction* phase = 0;
	RISE_API_CreateIsotropicPhaseFunction( &phase );

	IMedium* medium = 0;
	RISE_API_CreateHomogeneousMediumSpectral( &medium,
		RISEPel( 1.0, 3.0, 0.4 ),
		RISEPel( 0.0, 0.0, 0.0 ),
		RISEPel( 0.0, 0.0, 0.0 ),
		absCurve, /*sigma_s_spectral=*/nullptr, *phase );

	const Point3 p( 0, 0, 0 );
	MediumCoefficientsNM c = medium->GetCoefficientsNM( p, 650.0 );
	Check( Close( c.sigma_s, 0.0 ), "  sigma_s == 0 (no scattering curve)" );
	Check( Close( c.sigma_t, Eval( absCurve, 650.0 ) ), "  sigma_t == sigma_a(nm)" );

	safe_release( medium );
	safe_release( phase );
	safe_release( absCurve );
}

//----------------------------------------------------------------------
// D: byte-identical luminance fallback (no curves)
//----------------------------------------------------------------------
static void TestLuminanceFallback()
{
	std::cout << "D: RGB-only medium falls back to wavelength-independent luminance" << std::endl;

	IPhaseFunction* phase = 0;
	RISE_API_CreateIsotropicPhaseFunction( &phase );

	IMedium* medium = 0;
	RISE_API_CreateHomogeneousMedium( &medium,
		RISEPel( 0.2, 0.6, 1.2 ),		// colored sigma_a
		RISEPel( 0.0, 0.0, 0.0 ),
		*phase );

	const Point3 p( 0, 0, 0 );
	MediumCoefficientsNM c1 = medium->GetCoefficientsNM( p, 450.0 );
	MediumCoefficientsNM c2 = medium->GetCoefficientsNM( p, 650.0 );
	// Fallback is luminance of the RGB triple — the SAME scalar at every
	// wavelength (this is exactly the pre-G1 behaviour we preserve).
	Check( Close( c1.sigma_t, c2.sigma_t ),
		"  fallback sigma_t is wavelength-independent (luminance)" );
	Check( c1.sigma_t > 0.2 && c1.sigma_t < 1.2,
		"  fallback sigma_t is a luminance-weighted blend of the channels" );

	safe_release( medium );
	safe_release( phase );
}

//----------------------------------------------------------------------
// E: SampleDistanceNM scatter probability tracks sigma_t(nm)
//----------------------------------------------------------------------
static void TestSamplingConsistency()
{
	std::cout << "E: SampleDistanceNM scatter probability == 1 - exp(-sigma_t(nm)*maxDist)" << std::endl;

	IFunction1D* absCurve = MakeRedAbsorber();
	IFunction1D* scaCurve = MakeFlatScatter();
	IPhaseFunction* phase = 0;
	RISE_API_CreateIsotropicPhaseFunction( &phase );

	IMedium* medium = 0;
	RISE_API_CreateHomogeneousMediumSpectral( &medium,
		RISEPel( 1.0, 3.0, 0.4 ), RISEPel( 0.2, 0.2, 0.2 ), RISEPel( 0.0, 0.0, 0.0 ),
		absCurve, scaCurve, *phase );

	RandomNumberGenerator rng( 20260701u );
	Implementation::IndependentSampler sampler( rng );

	const Ray ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
	const double maxDist = 2.0;
	const int N = 400000;

	const double lambdas[] = { 550.0, 650.0 };
	for( double nm : lambdas ) {
		const double sigma_t = Eval( absCurve, nm ) + Eval( scaCurve, nm );
		const double pExpected = 1.0 - std::exp( -sigma_t * maxDist );
		int nScatter = 0;
		for( int i = 0; i < N; ++i ) {
			bool scattered = false;
			medium->SampleDistanceNM( ray, maxDist, nm, sampler, scattered );
			if( scattered ) ++nScatter;
		}
		const double pMeasured = double(nScatter) / double(N);
		std::cout << "    nm=" << nm << "  P(scatter) measured " << pMeasured
			<< "  expected " << pExpected << std::endl;
		Check( std::fabs( pMeasured - pExpected ) < 0.01,
			"  scatter fraction matches 1 - exp(-sigma_t(nm)*maxDist)" );
	}

	safe_release( medium );
	safe_release( phase );
	safe_release( absCurve );
	safe_release( scaCurve );
}

//----------------------------------------------------------------------
// G: non-negativity clamp — a curve with a negative control point must
//    not produce sigma_t < 0 (which would give Tr = exp(-sigma_t*d) > 1,
//    unphysical energy gain).  Guards author error.
//----------------------------------------------------------------------
static void TestNegativeClamp()
{
	std::cout << "G: negative curve values clamp to non-negative (no Tr > 1)" << std::endl;

	// Absorption curve dips negative around 600nm (author error).
	std::vector<double> x = { 400, 550, 600, 650, 700 };
	std::vector<double> y = { 1.0, 0.5, -2.0, 0.4, 0.3 };
	IFunction1D* absCurve = MakeCurve( x, y );
	IPhaseFunction* phase = 0;
	RISE_API_CreateIsotropicPhaseFunction( &phase );

	IMedium* medium = 0;
	RISE_API_CreateHomogeneousMediumSpectral( &medium,
		RISEPel( 1.0, 1.0, 1.0 ), RISEPel( 0.0, 0.0, 0.0 ), RISEPel( 0.0, 0.0, 0.0 ),
		absCurve, /*sigma_s_spectral=*/nullptr, *phase );

	const Point3 p( 0, 0, 0 );
	const Ray ray( p, Vector3( 0, 0, 1 ) );
	MediumCoefficientsNM c = medium->GetCoefficientsNM( p, 600.0 );
	Check( c.sigma_t >= 0.0, "  sigma_t(600nm) clamped to >= 0 despite negative curve value" );
	const double tr = medium->EvalTransmittanceNM( ray, 1.0, 600.0 );
	Check( tr <= 1.0 + 1e-9, "  Tr(600nm) <= 1 (no unphysical energy gain)" );

	safe_release( medium );
	safe_release( phase );
	safe_release( absCurve );
}

//----------------------------------------------------------------------
// H: the REAL gold-ruby colorant curve (G1-d).  Locks the physical ruby
// transmission window: a colloidal-gold sigma_a(lambda) with a plasmon
// peak at ~515nm and a blue inter-band shoulder must ABSORB green
// (Tr(515,d=1) ~ 0.011) and TRANSMIT red (Tr(650,d=1) ~ 0.70) -> a
// gold-ruby / oxblood medium.  Control points mirror
// scenes/FeatureBased/EnamelWatch/goldruby_sigma_a.txt (kept in sync).
//----------------------------------------------------------------------
static void TestGoldRubyColorant()
{
	std::cout << "H: gold-ruby colorant sigma_a(lambda) — ruby transmission window (G1-d)" << std::endl;

	std::vector<double> x = { 400, 420, 440, 460, 480, 500, 515, 530, 550,
	                          570, 590, 610, 630, 650, 680, 700, 720 };
	std::vector<double> y = { 2.48, 2.61, 2.79, 3.15, 3.69, 4.23, 4.50, 4.37, 3.83,
	                          2.79, 1.80, 1.08, 0.63, 0.36, 0.23, 0.16, 0.14 };
	IFunction1D* absCurve = MakeCurve( x, y );
	IPhaseFunction* phase = 0;
	RISE_API_CreateIsotropicPhaseFunction( &phase );

	IMedium* medium = 0;
	RISE_API_CreateHomogeneousMediumSpectral( &medium,
		RISEPel( 3.0, 3.5, 0.4 ), RISEPel( 0.0, 0.0, 0.0 ), RISEPel( 0.0, 0.0, 0.0 ),
		absCurve, /*sigma_s_spectral=*/nullptr, *phase );

	const Point3 p( 0, 0, 0 );
	const Ray ray( p, Vector3( 0, 0, 1 ) );
	const double d = 1.0;

	// Peak absorption sits at the plasmon resonance (~515nm), not in the red.
	const double sa515 = medium->GetCoefficientsNM( p, 515.0 ).sigma_t;
	const double sa650 = medium->GetCoefficientsNM( p, 650.0 ).sigma_t;
	Check( sa515 > sa650 * 5.0, "  sigma_a peaks at the plasmon band (515nm >> 650nm)" );

	const double trGreen = medium->EvalTransmittanceNM( ray, d, 515.0 );
	const double trRed   = medium->EvalTransmittanceNM( ray, d, 650.0 );
	std::cout << "    Tr(515nm)=" << trGreen << "  Tr(650nm)=" << trRed << std::endl;
	Check( Close( trGreen, std::exp( -4.50 * d ), 1e-3 ), "  Tr(515nm) == exp(-4.50*d) (green absorbed)" );
	Check( Close( trRed,   std::exp( -0.36 * d ), 1e-3 ), "  Tr(650nm) == exp(-0.36*d) (red transmitted)" );
	Check( trRed > trGreen * 40.0, "  ruby window: red passes >> green (Tr(650) >> Tr(515))" );
	// Blue is partly absorbed too (inter-band) -> not magenta; a red-dominant ruby.
	const double trBlue = medium->EvalTransmittanceNM( ray, d, 440.0 );
	Check( trBlue < trRed, "  blue is absorbed more than red (ruby, not magenta)" );

	safe_release( medium );
	safe_release( phase );
	safe_release( absCurve );
}

//----------------------------------------------------------------------
// G: Volume EMISSION carries the reference illuminant (Stage C slice 2)
//
// Emission is a SOURCE term, not a coefficient.  It used to be projected
// with ColorMath::Luminance -- ONE flat Rec.709 luma scalar reused at
// every wavelength -- while every other source in the engine (lights,
// emitters, radiance maps, radiance-uplifting shader ops) had moved to
// the D65-shaped reference illuminant.  A flat spectrum resolves to
// (1.205, 0.948, 0.909) on this film, and a RED emissive medium came out
// GREY.  GetCoefficientsNM now evaluates an RGBIlluminantSpectrum cached
// on the medium.
//
// Mutation guard: restore the luminance projection and the ratio below
// becomes exactly 1.0, failing every check in this function.
//----------------------------------------------------------------------
static void TestEmissionIlluminantShape()
{
	std::cout << "G: volumetric emission is illuminant-shaped, not luma-flat" << std::endl;

	IPhaseFunction* phase = 0;
	RISE_API_CreateIsotropicPhaseFunction( &phase );

	const Point3 p( 0, 0, 0 );

	// A strongly RED emission.  Under the old luma projection e(450) and
	// e(650) were the same number; under the illuminant uplift the red
	// sigmoid puts far more energy at 650 than at 450.
	IMedium* redMedium = 0;
	RISE_API_CreateHomogeneousMediumWithEmission( &redMedium,
		RISEPel( 0.1, 0.1, 0.1 ),		// sigma_a
		RISEPel( 0.0, 0.0, 0.0 ),		// sigma_s
		RISEPel( 1.0, 0.2, 0.2 ),		// emission -- RED
		*phase );

	const double eRedBlue  = redMedium->GetCoefficientsNM( p, 450.0 ).emission;
	const double eRedRed   = redMedium->GetCoefficientsNM( p, 650.0 ).emission;
	std::cout << "    red emission: e(450) = " << eRedBlue
	          << ", e(650) = " << eRedRed
	          << ", ratio = " << ( eRedBlue > 0 ? eRedRed / eRedBlue : 0.0 ) << std::endl;

	Check( eRedRed > 0.0 && eRedBlue > 0.0, "  emission is non-zero at both wavelengths" );
	Check( eRedRed > eRedBlue * 2.0,
		"  RED emission puts >2x more energy at 650nm than at 450nm (was 1.0x)" );

	// A WHITE emission is the reference-illuminant shape itself: the
	// sigmoid is ~1 everywhere, so e(nm) tracks D65norm(nm).  Assert
	// against the SAME table the runtime uses, so this cannot drift.
	IMedium* whiteMedium = 0;
	RISE_API_CreateHomogeneousMediumWithEmission( &whiteMedium,
		RISEPel( 0.1, 0.1, 0.1 ),
		RISEPel( 0.0, 0.0, 0.0 ),
		RISEPel( 1.0, 1.0, 1.0 ),		// emission -- WHITE
		*phase );

	bool whiteTracksD65 = true;
	const double nms[] = { 420, 450, 500, 550, 600, 650, 700 };
	for( int i = 0; i < 7; ++i ) {
		const double got = whiteMedium->GetCoefficientsNM( p, nms[i] ).emission;
		const double want =
			double( RGBIlluminantSpectrum::ReferenceIlluminant( Scalar( nms[i] ) ) );
		if( std::fabs( got - want ) > 1e-3 * std::max( 1.0, want ) ) {
			whiteTracksD65 = false;
			std::cout << "    white emission at " << nms[i]
			          << ": got " << got << ", D65norm " << want << std::endl;
		}
	}
	Check( whiteTracksD65,
		"  WHITE emission == the Y-normalised reference illuminant at every nm" );

	// And it is NOT flat -- the luma projection would have been.
	const double w450 = whiteMedium->GetCoefficientsNM( p, 450.0 ).emission;
	const double w650 = whiteMedium->GetCoefficientsNM( p, 650.0 ).emission;
	Check( std::fabs( w450 - w650 ) > 1e-3,
		"  WHITE emission is NOT wavelength-flat (the luma projection was)" );

	// SetEmission must rebuild the cached spectrum, else a scene edit
	// keeps radiating the constructed colour on the spectral path.
	{
		HomogeneousMedium* hm = dynamic_cast<HomogeneousMedium*>( redMedium );
		Check( hm != 0, "  medium is a HomogeneousMedium (SetEmission reachable)" );
		if( hm ) {
			hm->SetEmission( RISEPel( 0.2, 0.2, 1.0 ) );	// now BLUE
			const double b450 = hm->GetCoefficientsNM( p, 450.0 ).emission;
			const double b650 = hm->GetCoefficientsNM( p, 650.0 ).emission;
			Check( b450 > b650,
				"  SetEmission rebuilds the cached spectrum (blue now peaks at 450nm)" );
		}
	}

	safe_release( whiteMedium );
	safe_release( redMedium );
	safe_release( phase );
}

//----------------------------------------------------------------------
// G-het: same illuminant-shape contract as case G above, but for
// HeterogeneousMedium::GetCoefficientsNM.  Stage C slice 2 follow-up
// (474b3ef4) cached an RGBIlluminantSpectrum (`m_emissionSpectrum`) on
// this class too, built at construction from the same `emission` RGB
// triple HomogeneousMedium takes.  Unlike HomogeneousMedium, emission
// here is "constant, not density-modulated" (HeterogeneousMedium.h) --
// GetCoefficientsNM's `c.emission = m_emissionSpectrum.Eval(nm)` does
// not consult LookupDensity at all -- so there is no SetEmission
// mutator to re-check (HeterogeneousMedium has none; emission is
// construction-only).
//
// Built via RISE_API_CreatePainterHeterogeneousMediumWithEmission with
// a UniformColorPainter density field, so this needs no volume file on
// disk.
//
// Mutation guard: restore the luminance projection here too and both
// checks below go flat -- the RED case's own e(650)/e(450) ratio drops
// from >2x to 1.0x, and the WHITE case stops tracking the reference
// illuminant table (it collapses to a single luma scalar repeated at
// every wavelength instead).
//----------------------------------------------------------------------
static void TestHeterogeneousEmissionIlluminantShape()
{
	std::cout << "G-het: HeterogeneousMedium emission is illuminant-shaped, not luma-flat" << std::endl;

	IPhaseFunction* phase = 0;
	RISE_API_CreateIsotropicPhaseFunction( &phase );

	const Point3 bboxMin( -1, -1, -1 );
	const Point3 bboxMax(  1,  1,  1 );
	const Point3 p( 0, 0, 0 );

	// A strongly RED emission, mirroring case G exactly.
	IPainter* redDensity = 0;
	RISE_API_CreateUniformColorPainter( &redDensity, RISEPel( 1, 1, 1 ) );
	IMedium* redMedium = 0;
	RISE_API_CreatePainterHeterogeneousMediumWithEmission( &redMedium,
		RISEPel( 0.1, 0.1, 0.1 ),		// max_sigma_a
		RISEPel( 0.0, 0.0, 0.0 ),		// max_sigma_s
		RISEPel( 1.0, 0.2, 0.2 ),		// emission -- RED
		*phase, *redDensity,
		/*virtualResolution*/ 4, /*colorToScalar*/ 'l',
		bboxMin, bboxMax );

	const double eRedBlue = redMedium->GetCoefficientsNM( p, 450.0 ).emission;
	const double eRedRed  = redMedium->GetCoefficientsNM( p, 650.0 ).emission;
	std::cout << "    red emission: e(450) = " << eRedBlue
	          << ", e(650) = " << eRedRed
	          << ", ratio = " << ( eRedBlue > 0 ? eRedRed / eRedBlue : 0.0 ) << std::endl;

	Check( eRedRed > 0.0 && eRedBlue > 0.0, "  het emission is non-zero at both wavelengths" );
	Check( eRedRed > eRedBlue * 2.0,
		"  het RED emission puts >2x more energy at 650nm than at 450nm (was 1.0x)" );

	// A WHITE emission must track the SAME Y-normalised reference
	// illuminant table HomogeneousMedium's case G checks against.
	IPainter* whiteDensity = 0;
	RISE_API_CreateUniformColorPainter( &whiteDensity, RISEPel( 1, 1, 1 ) );
	IMedium* whiteMedium = 0;
	RISE_API_CreatePainterHeterogeneousMediumWithEmission( &whiteMedium,
		RISEPel( 0.1, 0.1, 0.1 ),
		RISEPel( 0.0, 0.0, 0.0 ),
		RISEPel( 1.0, 1.0, 1.0 ),		// emission -- WHITE
		*phase, *whiteDensity,
		/*virtualResolution*/ 4, /*colorToScalar*/ 'l',
		bboxMin, bboxMax );

	bool whiteTracksD65 = true;
	const double nms[] = { 420, 450, 500, 550, 600, 650, 700 };
	for( int i = 0; i < 7; ++i ) {
		const double got = whiteMedium->GetCoefficientsNM( p, nms[i] ).emission;
		const double want =
			double( RGBIlluminantSpectrum::ReferenceIlluminant( Scalar( nms[i] ) ) );
		if( std::fabs( got - want ) > 1e-3 * std::max( 1.0, want ) ) {
			whiteTracksD65 = false;
			std::cout << "    het white emission at " << nms[i]
			          << ": got " << got << ", D65norm " << want << std::endl;
		}
	}
	Check( whiteTracksD65,
		"  het WHITE emission == the Y-normalised reference illuminant at every nm" );

	// And it is NOT flat -- the luma projection would have been.
	const double w450 = whiteMedium->GetCoefficientsNM( p, 450.0 ).emission;
	const double w650 = whiteMedium->GetCoefficientsNM( p, 650.0 ).emission;
	Check( std::fabs( w450 - w650 ) > 1e-3,
		"  het WHITE emission is NOT wavelength-flat (the luma projection was)" );

	// Emission is NOT density-modulated (HeterogeneousMedium.h) -- a
	// point well outside the bbox (LookupDensity == 0) must still read
	// the same emission spectrum as a point at the centre.
	const Point3 pOutside( 100, 100, 100 );
	const double eOutside650 = redMedium->GetCoefficientsNM( pOutside, 650.0 ).emission;
	Check( Close( eOutside650, eRedRed, 1e-12 ),
		"  het emission is constant, not density-modulated (matches outside the bbox)" );

	safe_release( whiteMedium );
	safe_release( whiteDensity );
	safe_release( redMedium );
	safe_release( redDensity );
	safe_release( phase );
}

int main( int /*argc*/, char* /*argv*/[] )
{
	std::cout << "VolumeSpectralCoefficientsTest — G1 per-wavelength "
		"sigma_a(lambda)/sigma_s(lambda) in HomogeneousMedium" << std::endl;

	TestSpectralAbsorptionScattering();
	TestPureAbsorber();
	TestLuminanceFallback();
	TestSamplingConsistency();
	TestNegativeClamp();
	TestGoldRubyColorant();
	TestEmissionIlluminantShape();
	TestHeterogeneousEmissionIlluminantShape();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
