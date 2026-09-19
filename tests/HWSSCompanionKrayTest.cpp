//////////////////////////////////////////////////////////////////////
//
//  HWSSCompanionKrayTest.cpp - Red-proof and regression gate for
//    DL-125: an SPF that stores a PER-LOBE conditional density on each
//    emitted ray must implement `ISPF::EvaluateKrayNM`, so that the
//    HWSS companion ladder can price each companion wavelength with
//    the SELECTED lobe's own `f_I cos / p_I` at THAT wavelength.
//
//  DL-125 (docs/DL69_BDPT_LOBE_THROUGHPUT.md, "DL-125 outcome").
//  `PathTracingIntegrator.cpp`'s HWSS body and `BDPTIntegrator.cpp`'s
//  two HWSS companion loops all run the same ladder per companion
//  wavelength:
//
//      compWeight = pSPF->EvaluateKrayNM( ri, dir, type, lambda_w, ... );
//      if( compWeight < 0 )                       // base-class default
//          compWeight = pBRDF->valueNM(...) * cos / pS->pdf;
//
//  The fallback pairs the material's AGGREGATE spectral BSDF with the
//  ONE selected lobe's conditional density -- exactly the mismatch
//  DL-69 removed from the hero/RGB paths.  It is EXACT only for an SPF
//  whose emitted ray carries the aggregate mixture density (CoatedSPF,
//  FabricSPF, WeaveSPF, GGXSPF, CookTorranceSPF) and wrong for one that
//  carries a per-lobe conditional density.
//
//  FIVE classes are in that second group and now override the method:
//  `SchlickSPF`, `WardIsotropicGaussianSPF`,
//  `WardAnisotropicEllipticalGaussianSPF`, `IsotropicPhongSPF`,
//  `AshikminShirleyAnisotropicPhongSPF`. TranslucentSPF now also
//  answers (DL-222, integrated 2026-09-19); section D independently
//  checks entry painters, interior Beer/scattering and live-distance
//  replay. CompositeSPF remains unresolved (DL-221): its stochastic
//  layer walk cannot be recovered from (ri, outDir, type, nm).
//
//  SECTIONS
//    A. SAMPLER <-> EVALUATOR, SAME WAVELENGTH.  For every non-delta
//       lobe `ScatterNM(nm)` emits, `EvaluateKrayNM(ri, dir, type, nm)`
//       must reproduce that ray's own `krayNM`.  THE red-proof: pre-fix
//       every one of these five returns -1 ("not implemented") on every
//       query.
//    B. CROSS-WAVELENGTH.  With WAVELENGTH-INDEPENDENT shape painters
//       (roughness / isotropy / alpha / exponent) the sampler's warp
//       does not move with lambda, so two identically-seeded
//       `ScatterNM` runs at lambda_h and lambda_c emit the SAME
//       directions -- asserted, not assumed.  `EvaluateKrayNM` queried
//       at the HERO's direction and the COMPANION's wavelength must
//       then equal the companion run's own `krayNM`.  That is precisely
//       what the HWSS companion ladder asks of it.
//    C. AGAINST THE BRDF.  NOT the aggregate-vs-per-lobe discriminator
//       -- sections A/A2/B are (a contaminated override fails those and
//       would pass this one, because both sides of C's identity are
//       evaluated at the SAME wavelength).  What C adds is an
//       INDEPENDENT implementation: it re-derives the lobe weight from
//       the material's separately-written `IBSDF::valueNM` and the
//       density the SPF stored on the ray, so a formula that is
//       self-consistent between this SPF's sampler and its evaluator
//       but wrong about the BRDF is still caught.
//       `EvaluateKrayNM * p_lobe == f_lobe * cos`,
//       with `f_lobe` read from the material's own (separately
//       implemented) `IBSDF::valueNM` and `p_lobe` the density the SPF
//       stored on the ray.  Checked on the SPECULAR lobe of all five
//       (diffuse reflectance painter BLACK, so `valueNM` reduces to the
//       specular term up to the Jakob-Hanika uplift's black residual --
//       the sigmoid never returns a hard 0, IPainter.h) and on the
//       DIFFUSE lobe of the three whose specular BRDF term is
//       PROPORTIONAL to the specular reflectance, so that blacking that
//       painter really does isolate the diffuse term.  `SchlickSPF` and
//       `AshikminShirleyAnisotropicPhongSPF` are excluded from the
//       diffuse half for a stated reason and not an oversight: both
//       weight their specular lobe by `Rs + (1-Rs)*Fresnel`, which is
//       the FRESNEL TERM, not zero, at `Rs = 0` -- no painter choice
//       reduces their `valueNM` to the diffuse term alone.  Sections A
//       and B cover those two lobes exactly instead.
//       Two residuals in this section are the JH black residual, not a
//       kray defect, and are why the band is 5e-3 rather than machine
//       precision: the three DIFFUSE rows read 1.6e-3 / 3.1e-3 / 2.6e-3
//       (the black SPECULAR painter's own uplifted contribution, which
//       is NOT subtracted because each class's specular term has a
//       different closed form), against <= 2.8e-14 on every SPECULAR
//       row, where the black diffuse term IS subtracted exactly.
//    D. NEGATIVE CONTROLS.  An SPF for which the fallback is exact
//       (`GGXSPF`) or which has a single lobe whose density IS the
//       aggregate (`LambertianSPF`) must still return -1, so the
//       fallback ladder stays reachable; and an unknown / unsupported
//       `rayType` on the five must return -1 rather than a wrong
//       number. CompositeSPF names its unresolved fallback. Translucent
//       entry/exit weights have independent painter/Beer oracles; both
//       eye/light replay must consume recorded non-unit incoming
//       distance and apply the ratio only to downstream throughput.
//       Its unsupported types retain the diagnostic identity.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <iomanip>
#include <cmath>
#include <string>
#include <vector>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/GeometricUtilities.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Interfaces/IBSDF.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Interfaces/ILog.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Materials/SchlickSPF.h"
#include "../src/Library/Materials/SchlickBRDF.h"
#include "../src/Library/Materials/WardIsotropicGaussianSPF.h"
#include "../src/Library/Materials/WardIsotropicGaussianBRDF.h"
#include "../src/Library/Materials/WardAnisotropicEllipticalGaussianSPF.h"
#include "../src/Library/Materials/WardAnisotropicEllipticalGaussianBRDF.h"
#include "../src/Library/Materials/IsotropicPhongSPF.h"
#include "../src/Library/Materials/IsotropicPhongBRDF.h"
#include "../src/Library/Materials/AshikminShirleyAnisotropicPhongSPF.h"
#include "../src/Library/Materials/AshikminShirleyAnisotropicPhongBRDF.h"
#include "../src/Library/Materials/CompositeSPF.h"
#include "../src/Library/Materials/TranslucentSPF.h"
#include "../src/Library/Materials/GGXSPF.h"
#include "../src/Library/Materials/LambertianSPF.h"
#include "../src/Library/Materials/TranslucentMaterial.h"
#include "../src/Library/Shaders/BDPTIntegrator.h"
#include "../src/Library/Shaders/StandardShader.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Scene.h"
#include "TestStubObject.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const std::string& testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

static StubObject* g_stubObject = 0;

//! A scalar painter whose value RAMPS with wavelength, so the
//! wavelength-DEPENDENT shape-parameter case (section A at three
//! wavelengths) is genuinely exercised and not silently constant.
class LambdaRampScalarPainter :
	public virtual IScalarPainter,
	public virtual Reference
{
protected:
	const Scalar base;
	const Scalar slope;			///< per nm
	virtual ~LambdaRampScalarPainter() {}
public:
	LambdaRampScalarPainter( Scalar b, Scalar s ) : base( b ), slope( s ) {}

	ScalarTriple GetValuesAt( const RayIntersectionGeometric& ) const
	{
		// The RGB lane reads the ramp at the green channel wavelength so
		// the two pipes describe one curve.
		return ScalarTriple( base + slope * ( 549.0 - 550.0 ) );
	}

	Scalar GetValueAtNM( const RayIntersectionGeometric&, Scalar nm ) const
	{
		return base + slope * ( nm - 550.0 );
	}
};

//////////////////////////////////////////////////////////////////////
// Synthetic flat-surface intersection, normal +Z, incoming ray in the
// X-Z plane at `incomingTheta` from the normal.  Identical geometry to
// tests/SchlickKrayBRDFConsistencyTest.cpp.
//////////////////////////////////////////////////////////////////////
static RayIntersectionGeometric MakeIntersection( double incomingTheta )
{
	const double sinT = sin( incomingTheta );
	const double cosT = cos( incomingTheta );
	const Vector3 inDir( sinT, 0, -cosT );

	Ray inRay( Point3( sinT, 0, 1.0 ), inDir );
	RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( inRay, rs );

	ri.bHit = true;
	ri.range = 1.0 / cosT;
	ri.ptIntersection = Point3( 0, 0, 0 );
	ri.vNormal = Vector3( 0, 0, 1 );
	ri.vGeomNormal = Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
	ri.ptCoord = Point2( 0.5, 0.5 );

	return ri;
}

static double RelDiff( double a, double b )
{
	const double d = std::fabs( a - b );
	const double m = std::max( std::fabs( a ), std::fabs( b ) );
	return ( m > 1e-300 ) ? ( d / m ) : d;
}

//! One SPF under test, with the BRDF twin section C reads through.
struct Subject
{
	std::string	name;
	ISPF*		spf;
	const IBSDF* brdf;			///< may be 0 when section C is skipped
	const IPainter* diffusePainter;	///< the BLACK painter section C subtracts
	bool		specularBRDFIsolable;	///< see section C's header comment
	bool		diffuseBRDFIsolable;	///< see section C's header comment
	//! Ashikmin-Shirley's `valueNM` is `Rd*(1-Rs)*diffuseFactor +
	//! specularFactor`, NOT `Rd/pi + specular` -- so the BLACK diffuse
	//! painter's residual has a different closed form there and must be
	//! subtracted with the BRDF's own helper.  See section C.
	bool		ashikminDiffuseForm;
	const IPainter* rsPainter;			///< only read when ashikminDiffuseForm
	Scalar		nuConst;				///< only read when ashikminDiffuseForm
	Scalar		nvConst;				///< only read when ashikminDiffuseForm
};

//! Every Subject starts with the non-Ashikmin defaults; the one
//! Ashikmin section-C row overrides them explicitly.
static Subject MakeSubject( const char* name, ISPF* spf, const IBSDF* brdf,
	const IPainter* diffusePainter, bool specIsolable, bool diffIsolable )
{
	Subject s;
	s.name = name;
	s.spf = spf;
	s.brdf = brdf;
	s.diffusePainter = diffusePainter;
	s.specularBRDFIsolable = specIsolable;
	s.diffuseBRDFIsolable = diffIsolable;
	s.ashikminDiffuseForm = false;
	s.rsPainter = 0;
	s.nuConst = 0;
	s.nvConst = 0;
	return s;
}

static const Scalar kLambdas[3] = { 450.0, 550.0, 650.0 };
static const double kDegrees[3] = { 0.0, 35.0, 70.0 };

//////////////////////////////////////////////////////////////////////
// SECTION A -- sampler <-> evaluator, same wavelength.
//////////////////////////////////////////////////////////////////////
static void SectionA( const Subject& s, const IORStack& iorStack, unsigned int nDraws )
{
	double worst = 0;
	unsigned int nChecked = 0;
	unsigned int nDeclined = 0;

	for( int li = 0; li < 3; li++ ) {
	for( int di = 0; di < 3; di++ ) {
		const RayIntersectionGeometric ri = MakeIntersection( kDegrees[di] * PI / 180.0 );
		RandomNumberGenerator rng( 8123u + li * 37u + di );
		IndependentSampler sampler( rng );

		for( unsigned int k = 0; k < nDraws; k++ ) {
			ScatteredRayContainer scattered;
			s.spf->ScatterNM( ri, sampler, kLambdas[li], scattered, iorStack );

			for( unsigned int i = 0; i < scattered.Count(); i++ ) {
				const ScatteredRay& sr = scattered[i];
				if( sr.isDelta ) continue;

				const Scalar got = s.spf->EvaluateKrayNM(
					ri, sr.ray.Dir(), sr.type, kLambdas[li], iorStack );
				if( got < 0 ) { nDeclined++; continue; }

				const double rd = RelDiff( got, sr.krayNM );
				if( rd > worst ) worst = rd;
				nChecked++;
			}
		}
	}
	}

	std::cout << "   " << std::setw( 42 ) << std::left << s.name << std::right
	          << "  checked " << std::setw( 6 ) << nChecked
	          << "  declined " << std::setw( 6 ) << nDeclined
	          << "  worst rel diff " << std::scientific << std::setprecision( 3 )
	          << worst << std::fixed << std::endl;

	Check( nDeclined == 0,
		"DL-125 section A (" + s.name + "): EvaluateKrayNM answers every non-delta lobe ScatterNM emits" );
	Check( nChecked > 500,
		"DL-125 section A (" + s.name + "): enough lobes exercised" );
	Check( worst < 1e-9,
		"DL-125 section A (" + s.name + "): EvaluateKrayNM reproduces ScatterNM's own krayNM" );
}

//////////////////////////////////////////////////////////////////////
// SECTION B -- cross-wavelength.
//////////////////////////////////////////////////////////////////////
static void SectionB( const Subject& s, const IORStack& iorStack, unsigned int nDraws )
{
	double worst = 0;
	double worstDir = 0;
	double biggestHeroGap = 0;		///< how far the hero's own kray is from the companion's
	unsigned int nChecked = 0;
	unsigned int nDeclined = 0;

	const Scalar heroNM = 550.0;

	for( int li = 0; li < 3; li++ ) {
		if( kLambdas[li] == heroNM ) continue;
	for( int di = 0; di < 3; di++ ) {
		const RayIntersectionGeometric ri = MakeIntersection( kDegrees[di] * PI / 180.0 );

		RandomNumberGenerator rngH( 5501u + li * 91u + di );
		RandomNumberGenerator rngC( 5501u + li * 91u + di );
		IndependentSampler samplerH( rngH );
		IndependentSampler samplerC( rngC );

		for( unsigned int k = 0; k < nDraws; k++ ) {
			ScatteredRayContainer scatH, scatC;
			s.spf->ScatterNM( ri, samplerH, heroNM,       scatH, iorStack );
			s.spf->ScatterNM( ri, samplerC, kLambdas[li], scatC, iorStack );

			if( scatH.Count() != scatC.Count() ) {
				Check( false, "DL-125 section B (" + s.name +
					"): wavelength-independent shape painters give the same lobe count" );
				return;
			}

			for( unsigned int i = 0; i < scatH.Count(); i++ ) {
				const ScatteredRay& h = scatH[i];
				const ScatteredRay& c = scatC[i];
				if( h.isDelta ) continue;

				// The two runs must have drawn the SAME direction --
				// asserted, because the whole cross-wavelength claim
				// rests on it.
				const Vector3 dh = h.ray.Dir();
				const Vector3 dc = c.ray.Dir();
				const double dd = std::fabs( dh.x - dc.x )
				                + std::fabs( dh.y - dc.y )
				                + std::fabs( dh.z - dc.z );
				if( dd > worstDir ) worstDir = dd;

				const Scalar got = s.spf->EvaluateKrayNM(
					ri, h.ray.Dir(), h.type, kLambdas[li], iorStack );
				if( got < 0 ) { nDeclined++; continue; }

				const double rd = RelDiff( got, c.krayNM );
				if( rd > worst ) worst = rd;

				const double gap = RelDiff( h.krayNM, c.krayNM );
				if( gap > biggestHeroGap ) biggestHeroGap = gap;
				nChecked++;
			}
		}
	}
	}

	std::cout << "   " << std::setw( 42 ) << std::left << s.name << std::right
	          << "  checked " << std::setw( 6 ) << nChecked
	          << "  declined " << std::setw( 6 ) << nDeclined
	          << "  worst rel diff " << std::scientific << std::setprecision( 3 ) << worst
	          << "  dir drift " << worstDir
	          << "  hero-vs-companion spread " << biggestHeroGap
	          << std::fixed << std::endl;

	Check( worstDir < 1e-12,
		"DL-125 section B (" + s.name + "): the two wavelengths' samplers drew identical directions" );
	Check( nDeclined == 0,
		"DL-125 section B (" + s.name + "): EvaluateKrayNM answers at a companion wavelength" );
	Check( nChecked > 300,
		"DL-125 section B (" + s.name + "): enough companion queries exercised" );
	Check( worst < 1e-9,
		"DL-125 section B (" + s.name + "): companion kray equals what ScatterNM would produce at that wavelength" );
	// If the reflectance painter were wavelength-flat this whole
	// section would be vacuous, so pin that it is not.
	Check( biggestHeroGap > 0.02,
		"DL-125 section B (" + s.name + "): the hero and companion krays genuinely differ" );
}

//////////////////////////////////////////////////////////////////////
// SECTION C -- against the BRDF.
//////////////////////////////////////////////////////////////////////
static void SectionC( const Subject& s, const IORStack& iorStack, unsigned int nDraws )
{
	if( !s.brdf ) return;

	double worstSpec = 0, worstDiff = 0;
	unsigned int nSpec = 0, nDiff = 0;

	for( int li = 0; li < 3; li++ ) {
	for( int di = 0; di < 3; di++ ) {
		const RayIntersectionGeometric ri = MakeIntersection( kDegrees[di] * PI / 180.0 );
		const Vector3 n = ri.onb.w();
		RandomNumberGenerator rng( 3301u + li * 13u + di );
		IndependentSampler sampler( rng );

		// The exact term `valueNM` adds for the BLACK diffuse painter.
		// A UniformColorPainter of exact black does NOT return exact
		// zero through the Jakob-Hanika spectral uplift, so subtract
		// what it contributes rather than loosening the band -- the
		// same treatment tests/SchlickKrayBRDFConsistencyTest.cpp
		// section 4 gives it.
		const Scalar fBlackDiffuse = GuardedGetColorNM( *s.diffusePainter, ri, kLambdas[li] ) * INV_PI;
		OrthonormalBasis3D ashonb = ri.onb;
		if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
			ashonb.FlipW();
		}

		for( unsigned int k = 0; k < nDraws; k++ ) {
			ScatteredRayContainer scattered;
			s.spf->ScatterNM( ri, sampler, kLambdas[li], scattered, iorStack );

			for( unsigned int i = 0; i < scattered.Count(); i++ ) {
				const ScatteredRay& sr = scattered[i];
				if( sr.isDelta || sr.pdf <= 0 ) continue;

				const Vector3 wo = Vector3Ops::Normalize( sr.ray.Dir() );
				const double cosO = Vector3Ops::Dot( wo, n );
				if( cosO <= 0 ) continue;

				const Scalar kray = s.spf->EvaluateKrayNM( ri, sr.ray.Dir(), sr.type, kLambdas[li], iorStack );
				if( kray <= 0 ) continue;

				if( sr.type == ScatteredRay::eRayReflection && s.specularBRDFIsolable ) {
					// Subtract exactly what `valueNM` added for the
					// BLACK diffuse painter.  Ashikmin-Shirley's diffuse
					// term is `Rd*(1-Rs)*diffuseFactor`, not `Rd/pi`, and
					// the difference (~20% of a ~1e-5 residual) is large
					// enough RELATIVE to its own specular factor at a
					// low-reflectance channel to swamp this check --
					// measured 7.65e-3 with the wrong subtraction against
					// <= 2.8e-14 for the other four classes.
					double fSub = fBlackDiffuse;
					if( s.ashikminDiffuseForm ) {
						const Scalar rhoNM = GuardedGetColorNM( *s.rsPainter, ri, kLambdas[li] );
						Scalar dF = 0, sF = 0;
						AshikminShirleyAnisotropicPhongBRDF::ComputeDiffuseSpecularFactors(
							dF, sF, wo, ri, ashonb.w(), ashonb.u(), ashonb.v(),
							s.nuConst, s.nvConst, rhoNM );
						fSub = GuardedGetColorNM( *s.diffusePainter, ri, kLambdas[li] )
						       * ( 1.0 - rhoNM ) * dF;
					}
					const double fI = s.brdf->valueNM( wo, ri, kLambdas[li] ) - fSub;
					if( fI <= 0 ) continue;
					const double r = ( kray * sr.pdf ) / ( fI * cosO );
					if( std::isfinite( r ) ) {
						const double d = std::fabs( r - 1.0 );
						if( d > worstSpec ) worstSpec = d;
						nSpec++;
					}
				} else if( sr.type == ScatteredRay::eRayDiffuse && s.diffuseBRDFIsolable ) {
					const double fI = s.brdf->valueNM( wo, ri, kLambdas[li] );
					if( fI <= 0 ) continue;
					const double r = ( kray * sr.pdf ) / ( fI * cosO );
					if( std::isfinite( r ) ) {
						const double d = std::fabs( r - 1.0 );
						if( d > worstDiff ) worstDiff = d;
						nDiff++;
					}
				}
			}
		}
	}
	}

	std::cout << "   " << std::setw( 42 ) << std::left << s.name << std::right
	          << "  spec n " << std::setw( 6 ) << nSpec
	          << " worst |r-1| " << std::scientific << std::setprecision( 3 ) << worstSpec
	          << "   diff n " << std::setw( 6 ) << nDiff
	          << " worst |r-1| " << worstDiff << std::fixed << std::endl;

	if( s.specularBRDFIsolable ) {
		Check( nSpec > 200,
			"DL-125 section C (" + s.name + "): enough specular draws" );
		Check( worstSpec < 5e-3,
			"DL-125 section C (" + s.name + "): EvaluateKrayNM * p_S == f_S cos through the BRDF" );
	}
	if( s.diffuseBRDFIsolable ) {
		Check( nDiff > 200,
			"DL-125 section C (" + s.name + "): enough diffuse draws" );
		Check( worstDiff < 5e-3,
			"DL-125 section C (" + s.name + "): EvaluateKrayNM * p_D == f_D cos through the BRDF" );
	}
}

int main()
{
	GlobalLog();

	std::cout << "=== HWSSCompanionKrayTest (DL-125) ===" << std::endl;
	std::cout << std::fixed << std::setprecision( 6 );

	g_stubObject = new StubObject();
	g_stubObject->addref();
	IORStack iorStack = MakeTestIORStack( g_stubObject );

	// A deliberately CHROMATIC reflectance: `UniformColorPainter`'s
	// `GetColorNM` routes through the Jakob-Hanika uplift, so an
	// off-grey triple really does vary across 450 / 550 / 650 nm --
	// which is what makes sections A/B/C non-vacuous.
	UniformColorPainter* black = new UniformColorPainter( RISEPel( 0, 0, 0 ) ); black->addref();
	UniformColorPainter* spec  = new UniformColorPainter( RISEPel( 0.75, 0.30, 0.12 ) ); spec->addref();
	UniformColorPainter* diff  = new UniformColorPainter( RISEPel( 0.20, 0.55, 0.80 ) ); diff->addref();

	// WAVELENGTH-INDEPENDENT shape painters -- section B's premise.
	UniformScalarPainter* rough  = new UniformScalarPainter( 0.35 ); rough->addref();
	UniformScalarPainter* iso    = new UniformScalarPainter( 0.7 );  iso->addref();
	UniformScalarPainter* alphaX = new UniformScalarPainter( 0.25 ); alphaX->addref();
	UniformScalarPainter* alphaY = new UniformScalarPainter( 0.12 ); alphaY->addref();
	UniformScalarPainter* expo   = new UniformScalarPainter( 40.0 ); expo->addref();
	UniformScalarPainter* nu     = new UniformScalarPainter( 90.0 ); nu->addref();
	UniformScalarPainter* nv     = new UniformScalarPainter( 25.0 ); nv->addref();

	SchlickSPF*  schSPF  = new SchlickSPF(  *diff, *spec, *rough, *iso ); schSPF->addref();
	SchlickSPF*  schSPFb = new SchlickSPF(  *black, *spec, *rough, *iso ); schSPFb->addref();
	SchlickBRDF* schBRDF = new SchlickBRDF( *black, *spec, *rough, *iso ); schBRDF->addref();

	WardIsotropicGaussianSPF*  wiSPF  = new WardIsotropicGaussianSPF( *diff, *spec, *alphaX ); wiSPF->addref();
	WardIsotropicGaussianSPF*  wiSPFb = new WardIsotropicGaussianSPF( *black, *spec, *alphaX ); wiSPFb->addref();
	WardIsotropicGaussianBRDF* wiBRDF = new WardIsotropicGaussianBRDF( *black, *spec, *alphaX ); wiBRDF->addref();

	WardAnisotropicEllipticalGaussianSPF*  waSPF  = new WardAnisotropicEllipticalGaussianSPF( *diff, *spec, *alphaX, *alphaY ); waSPF->addref();
	WardAnisotropicEllipticalGaussianSPF*  waSPFb = new WardAnisotropicEllipticalGaussianSPF( *black, *spec, *alphaX, *alphaY ); waSPFb->addref();
	WardAnisotropicEllipticalGaussianBRDF* waBRDF = new WardAnisotropicEllipticalGaussianBRDF( *black, *spec, *alphaX, *alphaY ); waBRDF->addref();

	IsotropicPhongSPF*  ipSPF  = new IsotropicPhongSPF( *diff, *spec, *expo ); ipSPF->addref();
	IsotropicPhongSPF*  ipSPFb = new IsotropicPhongSPF( *black, *spec, *expo ); ipSPFb->addref();
	IsotropicPhongBRDF* ipBRDF = new IsotropicPhongBRDF( *black, *spec, *expo ); ipBRDF->addref();

	AshikminShirleyAnisotropicPhongSPF*  asSPF  = new AshikminShirleyAnisotropicPhongSPF( *nu, *nv, *diff, *spec ); asSPF->addref();
	AshikminShirleyAnisotropicPhongSPF*  asSPFb = new AshikminShirleyAnisotropicPhongSPF( *nu, *nv, *black, *spec ); asSPFb->addref();
	AshikminShirleyAnisotropicPhongBRDF* asBRDF = new AshikminShirleyAnisotropicPhongBRDF( *nu, *nv, *black, *spec ); asBRDF->addref();

	std::vector<Subject> subjects;
	{
		subjects.push_back( MakeSubject( "SchlickSPF", schSPF, 0, diff, false, false ) );
		subjects.push_back( MakeSubject( "WardIsotropicGaussianSPF", wiSPF, 0, diff, false, false ) );
		subjects.push_back( MakeSubject( "WardAnisotropicEllipticalGaussianSPF", waSPF, 0, diff, false, false ) );
		subjects.push_back( MakeSubject( "IsotropicPhongSPF", ipSPF, 0, diff, false, false ) );
		subjects.push_back( MakeSubject( "AshikminShirleyAnisotropicPhongSPF", asSPF, 0, diff, false, false ) );
	}

	// Section C reads the BLACK-diffuse twins so `valueNM` isolates the
	// specular term (and, where the flag says so, the diffuse one).
	std::vector<Subject> cSubjects;
	{
		cSubjects.push_back( MakeSubject( "SchlickSPF", schSPFb, schBRDF, black, true, false ) );
		cSubjects.push_back( MakeSubject( "WardIsotropicGaussianSPF", wiSPFb, wiBRDF, black, true, false ) );
		cSubjects.push_back( MakeSubject( "WardAnisotropicEllipticalGaussianSPF", waSPFb, waBRDF, black, true, false ) );
		cSubjects.push_back( MakeSubject( "IsotropicPhongSPF", ipSPFb, ipBRDF, black, true, false ) );
		{
			Subject ash = MakeSubject( "AshikminShirleyAnisotropicPhongSPF", asSPFb, asBRDF, black, true, false );
			ash.ashikminDiffuseForm = true;
			ash.rsPainter = spec;
			ash.nuConst = 90.0;
			ash.nvConst = 25.0;
			cSubjects.push_back( ash );
		}
	}

	// The DIFFUSE half of section C needs the specular painter black
	// instead, and only works where the specular BRDF term is
	// proportional to that painter (Ward x2, Phong).
	UniformScalarPainter* alphaXD = alphaX;
	WardIsotropicGaussianSPF*  wiSPFd = new WardIsotropicGaussianSPF( *diff, *black, *alphaXD ); wiSPFd->addref();
	WardIsotropicGaussianBRDF* wiBRDFd = new WardIsotropicGaussianBRDF( *diff, *black, *alphaXD ); wiBRDFd->addref();
	WardAnisotropicEllipticalGaussianSPF*  waSPFd = new WardAnisotropicEllipticalGaussianSPF( *diff, *black, *alphaX, *alphaY ); waSPFd->addref();
	WardAnisotropicEllipticalGaussianBRDF* waBRDFd = new WardAnisotropicEllipticalGaussianBRDF( *diff, *black, *alphaX, *alphaY ); waBRDFd->addref();
	IsotropicPhongSPF*  ipSPFd  = new IsotropicPhongSPF( *diff, *black, *expo ); ipSPFd->addref();
	IsotropicPhongBRDF* ipBRDFd = new IsotropicPhongBRDF( *diff, *black, *expo ); ipBRDFd->addref();

	std::vector<Subject> dSubjects;
	{
		dSubjects.push_back( MakeSubject( "WardIsotropicGaussianSPF (diffuse)", wiSPFd, wiBRDFd, black, false, true ) );
		dSubjects.push_back( MakeSubject( "WardAnisotropicEllipticalGaussianSPF (diffuse)", waSPFd, waBRDFd, black, false, true ) );
		dSubjects.push_back( MakeSubject( "IsotropicPhongSPF (diffuse)", ipSPFd, ipBRDFd, black, false, true ) );
	}

	//----------------------------------------------------------------
	std::cout << std::endl
	          << "-- Section A: EvaluateKrayNM(dir, type, nm) vs ScatterNM(nm)'s own krayNM"
	          << std::endl;
	for( size_t i = 0; i < subjects.size(); i++ ) SectionA( subjects[i], iorStack, 400 );

	//----------------------------------------------------------------
	// Section A2: the SAME contract with WAVELENGTH-VARYING shape
	// painters (roughness / isotropy / alpha / exponent), which section
	// B deliberately cannot use.  Section A queries only at the
	// wavelength the ray was drawn at, so a ramped shape painter is
	// fully exercised here: it proves `EvaluateKrayNM` reads every
	// shape parameter at the SAME lambda `ScatterNM` did.
	std::cout << std::endl
	          << "-- Section A2: same, with wavelength-VARYING shape painters"
	          << std::endl;
	LambdaRampScalarPainter* rRough = new LambdaRampScalarPainter( 0.35, 0.0008 ); rRough->addref();
	LambdaRampScalarPainter* rIso   = new LambdaRampScalarPainter( 0.70, 0.0010 ); rIso->addref();
	LambdaRampScalarPainter* rAX    = new LambdaRampScalarPainter( 0.25, 0.0006 ); rAX->addref();
	LambdaRampScalarPainter* rAY    = new LambdaRampScalarPainter( 0.12, 0.0003 ); rAY->addref();
	LambdaRampScalarPainter* rExp   = new LambdaRampScalarPainter( 40.0, 0.1500 ); rExp->addref();
	LambdaRampScalarPainter* rNu    = new LambdaRampScalarPainter( 90.0, 0.2000 ); rNu->addref();
	LambdaRampScalarPainter* rNv    = new LambdaRampScalarPainter( 25.0, 0.0800 ); rNv->addref();

	SchlickSPF* schR = new SchlickSPF( *diff, *spec, *rRough, *rIso ); schR->addref();
	WardIsotropicGaussianSPF* wiR = new WardIsotropicGaussianSPF( *diff, *spec, *rAX ); wiR->addref();
	WardAnisotropicEllipticalGaussianSPF* waR = new WardAnisotropicEllipticalGaussianSPF( *diff, *spec, *rAX, *rAY ); waR->addref();
	IsotropicPhongSPF* ipR = new IsotropicPhongSPF( *diff, *spec, *rExp ); ipR->addref();
	AshikminShirleyAnisotropicPhongSPF* asR = new AshikminShirleyAnisotropicPhongSPF( *rNu, *rNv, *diff, *spec ); asR->addref();

	{
		std::vector<Subject> rSubjects;
		rSubjects.push_back( MakeSubject( "SchlickSPF (lambda-varying shape)", schR, 0, diff, false, false ) );
		rSubjects.push_back( MakeSubject( "WardIsotropicGaussianSPF (lambda-varying shape)", wiR, 0, diff, false, false ) );
		rSubjects.push_back( MakeSubject( "WardAnisotropicEllipticalGaussianSPF (lambda-varying shape)", waR, 0, diff, false, false ) );
		rSubjects.push_back( MakeSubject( "IsotropicPhongSPF (lambda-varying shape)", ipR, 0, diff, false, false ) );
		rSubjects.push_back( MakeSubject( "AshikminShirleyAnisotropicPhongSPF (lambda-varying shape)", asR, 0, diff, false, false ) );
		for( size_t i = 0; i < rSubjects.size(); i++ ) SectionA( rSubjects[i], iorStack, 400 );
	}

	//----------------------------------------------------------------
	std::cout << std::endl
	          << "-- Section B: companion wavelength at the HERO's direction"
	          << std::endl;
	for( size_t i = 0; i < subjects.size(); i++ ) SectionB( subjects[i], iorStack, 400 );

	//----------------------------------------------------------------
	std::cout << std::endl
	          << "-- Section C: EvaluateKrayNM * p_lobe == f_lobe cos (through IBSDF::valueNM)"
	          << std::endl;
	for( size_t i = 0; i < cSubjects.size(); i++ ) SectionC( cSubjects[i], iorStack, 300 );
	for( size_t i = 0; i < dSubjects.size(); i++ ) SectionC( dSubjects[i], iorStack, 300 );

	//----------------------------------------------------------------
	// Section E: the PREMISE behind the second half of the DL-125 fix.
	//
	// `BDPTIntegrator::RecomputeSubpathThroughputNM` -- the
	// render-visible companion pricing for BDPT, VCM and MLT -- used to
	// rescale a companion by the AGGREGATE ratio
	// `f_agg(lambda_c)/f_agg(lambda_h)`.  It now asks the SPF for
	// `kray_I(lambda_c)/kray_I(lambda_h)` instead.  Those are the same
	// number only when the SELECTED lobe's spectrum is the aggregate's
	// spectrum; this section MEASURES how far apart they are on a
	// `schlick_material` whose two lobes carry DIVERGENT spectra (a
	// blue diffuse under a red specular), over real `ScatterNM` draws.
	//
	// Gated as a premise, not as a tolerance: if a future change makes
	// the two ratios agree, this check fails and tells the reader the
	// second half of DL-125's fix has become a no-op -- the same way
	// tests/PTGuidingMISPartitionTest.cpp pins `SchlickSPF`'s
	// aggregate-vs-selected-lobe PDF spread as a premise for DL-103.
	std::cout << std::endl
	          << "-- Section E: aggregate-ratio vs per-lobe-ratio premise "
	             "(chromatic two-lobe schlick_material)"
	          << std::endl;
	{
		UniformColorPainter* blueD = new UniformColorPainter( RISEPel( 0.05, 0.10, 0.70 ) ); blueD->addref();
		UniformColorPainter* redS  = new UniformColorPainter( RISEPel( 0.70, 0.10, 0.05 ) ); redS->addref();
		SchlickSPF*  chSPF  = new SchlickSPF(  *blueD, *redS, *rough, *iso ); chSPF->addref();
		SchlickBRDF* chBRDF = new SchlickBRDF( *blueD, *redS, *rough, *iso ); chBRDF->addref();

		const Scalar heroNM = 550.0;
		double worstDisagree = 1.0;		///< max over draws of max(r, 1/r)
		unsigned int n = 0;

		for( int li = 0; li < 3; li++ ) {
			if( kLambdas[li] == heroNM ) continue;
		for( int di = 0; di < 3; di++ ) {
			const RayIntersectionGeometric ri = MakeIntersection( kDegrees[di] * PI / 180.0 );
			const Vector3 nrm = ri.onb.w();
			RandomNumberGenerator rng( 7717u + li * 53u + di );
			IndependentSampler sampler( rng );

			for( unsigned int k = 0; k < 400; k++ ) {
				ScatteredRayContainer scattered;
				chSPF->ScatterNM( ri, sampler, heroNM, scattered, iorStack );

				for( unsigned int i = 0; i < scattered.Count(); i++ ) {
					const ScatteredRay& sr = scattered[i];
					if( sr.isDelta ) continue;
					const Vector3 wo = Vector3Ops::Normalize( sr.ray.Dir() );
					if( Vector3Ops::Dot( wo, nrm ) <= 0 ) continue;

					const Scalar kh = chSPF->EvaluateKrayNM( ri, sr.ray.Dir(), sr.type, heroNM, iorStack );
					const Scalar kc = chSPF->EvaluateKrayNM( ri, sr.ray.Dir(), sr.type, kLambdas[li], iorStack );
					const double fh = chBRDF->valueNM( wo, ri, heroNM );
					const double fc = chBRDF->valueNM( wo, ri, kLambdas[li] );
					if( kh <= 1e-12 || fh <= 1e-12 ) continue;

					const double lobeRatio = kc / kh;
					const double aggRatio  = fc / fh;
					if( !std::isfinite( lobeRatio ) || !std::isfinite( aggRatio ) ||
					    lobeRatio <= 0 || aggRatio <= 0 ) continue;

					const double r = lobeRatio / aggRatio;
					const double d = std::max( r, 1.0 / r );
					if( d > worstDisagree ) worstDisagree = d;
					n++;
				}
			}
		}
		}

		std::cout << "   draws " << n << "   worst |per-lobe ratio / aggregate ratio| "
		          << std::fixed << std::setprecision( 4 ) << worstDisagree << "x" << std::endl;

		// DL-125 review round 2, P2 -- WHERE THE VARIANCE COST COMES
		// FROM.  `RecomputeSubpathThroughputNM`'s per-lobe branch forms
		// `lobeRatio = krayComp / krayHero`, which is UNBOUNDED as
		// `krayHero -> 0`; the aggregate ratio it replaced was a convex
		// blend and so was bounded between the two lobes' own ratios.
		// The post-fix render's run-to-run sd duly stops falling cleanly
		// with sample count.  This block asks WHICH population the heavy
		// ratios come from -- a near-zero hero (a numerical tail the
		// caller could bound) or a legitimately large spectral swing (a
		// real quantity that must not be clamped).  Printed, not gated:
		// it is a characterisation of a recorded cost, not a contract.
		// Synthetic ri at 0/35/70 degrees only: this population is not a
		// render-weighted tail distribution or a bound on grazing heroes.
		{
			unsigned int nBig = 0, nBigFromSmallHero = 0, nTot = 0;
			double worstRatio = 0, heroAtWorst = 0;
			double minHero = 1e300;
			for( int li = 0; li < 3; li++ ) {
				if( kLambdas[li] == heroNM ) continue;
			for( int di = 0; di < 3; di++ ) {
				const RayIntersectionGeometric ri = MakeIntersection( kDegrees[di] * PI / 180.0 );
				RandomNumberGenerator rng( 9911u + li * 17u + di );
				IndependentSampler sampler( rng );
				for( unsigned int k = 0; k < 400; k++ ) {
					ScatteredRayContainer scattered;
					chSPF->ScatterNM( ri, sampler, heroNM, scattered, iorStack );
					for( unsigned int i = 0; i < scattered.Count(); i++ ) {
						const ScatteredRay& sr = scattered[i];
						if( sr.isDelta ) continue;
						const Scalar kh = chSPF->EvaluateKrayNM( ri, sr.ray.Dir(), sr.type, heroNM, iorStack );
						const Scalar kc = chSPF->EvaluateKrayNM( ri, sr.ray.Dir(), sr.type, kLambdas[li], iorStack );
						if( kh <= 0 || kc < 0 ) continue;
						nTot++;
						if( kh < minHero ) minHero = kh;
						const double r = kc / kh;
						if( r > worstRatio ) { worstRatio = r; heroAtWorst = kh; }
						if( r > 4.0 ) {
							nBig++;
							// "small hero" = two orders below the median
							// hero weight this material produces (~0.1).
							if( kh < 1e-3 ) nBigFromSmallHero++;
						}
					}
				}
			}
			}
			std::cout << "   synthetic-ri ratio population (0/35/70 degrees): " << nTot << " ratios, " << nBig << " over 4x ("
			          << ( nTot ? 100.0 * double(nBig) / double(nTot) : 0.0 ) << "%), of which "
			          << nBigFromSmallHero << " have krayHero < 1e-3;  worst ratio "
			          << worstRatio << "x at krayHero " << std::scientific
			          << std::setprecision( 3 ) << heroAtWorst
			          << ", min krayHero seen " << minHero << std::fixed
			          << std::setprecision( 4 ) << std::endl;
		}
		Check( n > 2000, "DL-125 section E: enough chromatic draws" );
		Check( worstDisagree > 1.2,
			"DL-125 section E (PREMISE): the per-lobe and aggregate companion ratios are "
			"measurably DIFFERENT functions of wavelength -- if this ever passes trivially, "
			"RecomputeSubpathThroughputNM's per-lobe branch has become a no-op" );

		chSPF->release(); chBRDF->release(); blueD->release(); redS->release();
	}

	//----------------------------------------------------------------
	std::cout << std::endl << "-- Section D: controls and Translucent lobe/replay oracles" << std::endl;
	{
		const RayIntersectionGeometric ri = MakeIntersection( 30.0 * PI / 180.0 );
		const Vector3 wo = Vector3Ops::Normalize( Vector3( 0.3, 0.2, 0.9 ) );

		// An unsupported lobe type must DECLINE, not invent a number.
		for( size_t i = 0; i < subjects.size(); i++ ) {
			const Scalar v = subjects[i].spf->EvaluateKrayNM(
				ri, wo, ScatteredRay::eRayRefraction, 550.0, iorStack );
			Check( v < 0,
				"DL-125 section D (" + subjects[i].name + "): an unsupported lobe type declines (-1)" );
		}

		// GGXSPF: the fallback is EXACT for it (single emitted lobe whose
		// `.pdf` IS the aggregate mixture density), so it must keep
		// declining and must NOT name itself to the diagnostic.
		UniformScalarPainter* ggxAX  = new UniformScalarPainter( 0.3 ); ggxAX->addref();
		UniformScalarPainter* ggxAY  = new UniformScalarPainter( 0.3 ); ggxAY->addref();
		UniformScalarPainter* ggxIOR = new UniformScalarPainter( 1.5 ); ggxIOR->addref();
		UniformScalarPainter* ggxExt = new UniformScalarPainter( 0.0 ); ggxExt->addref();
		GGXSPF* ggx = new GGXSPF( *diff, *spec, *ggxAX, *ggxAY, *ggxIOR, *ggxExt ); ggx->addref();
		Check( ggx->EvaluateKrayNM( ri, wo, ScatteredRay::eRayReflection, 550.0, iorStack ) < 0,
			"DL-125 section D (GGXSPF): still declines -- its companion fallback is exact" );
		Check( ggx->PerLobeDensityFallbackName() == 0,
			"DL-125 section D (GGXSPF): does not name itself to the fallback diagnostic" );
		ggx->release(); ggxAX->release(); ggxAY->release(); ggxIOR->release(); ggxExt->release();

		LambertianSPF* lam = new LambertianSPF( *diff ); lam->addref();
		Check( lam->EvaluateKrayNM( ri, wo, ScatteredRay::eRayDiffuse, 550.0, iorStack ) < 0,
			"DL-125 section D (LambertianSPF): still declines -- single lobe, aggregate density" );
		Check( lam->PerLobeDensityFallbackName() == 0,
			"DL-125 section D (LambertianSPF): does not name itself to the fallback diagnostic" );

		// CompositeSPF: DL-221.  It cannot answer, and must SAY SO by
		// name so the one-shot diagnostic at the three HWSS call sites
		// can report which class fell through.
		UniformScalarPainter* ext = new UniformScalarPainter( 0.0 ); ext->addref();
		CompositeSPF* comp = new CompositeSPF( *lam, *ipSPF, 8, 4, 4, 4, 4, 0.1, *ext ); comp->addref();
		Check( comp->EvaluateKrayNM( ri, wo, ScatteredRay::eRayDiffuse, 550.0, iorStack ) < 0,
			"DL-125 section D (CompositeSPF): declines -- DL-221, its walk is not reconstructible" );
		Check( comp->PerLobeDensityFallbackName() != 0 &&
		       std::string( comp->PerLobeDensityFallbackName() ) == "CompositeSPF",
			"DL-125 section D (CompositeSPF): names itself to the fallback diagnostic" );
		comp->release();

		// DL-222 integration: derive from painter and Beer/scattering
		// inputs, independently of ScatterNM and BuildLobeSet.
		UniformScalarPainter* tN = new UniformScalarPainter( 8.0 ); tN->addref();
		LambdaRampScalarPainter* tExt = new LambdaRampScalarPainter( 0.2, 0.0005 ); tExt->addref();
		LambdaRampScalarPainter* tS = new LambdaRampScalarPainter( 0.3, 0.0005 ); tS->addref();
		TranslucentMaterial* transMat = new TranslucentMaterial( *diff, *spec, *tExt, *tN, *tS ); transMat->addref();
		const ISPF* trans = transMat->GetSPF();
		IORStack inside = iorStack;
		inside.push( inside.top() );
		Check( !iorStack.containsCurrent() && inside.containsCurrent(),
			"DL-222: entry and exit oracle stacks differ in actual object membership" );
		Scene* scene = new Scene();
		StandardShader* shader = new StandardShader( std::vector<IShaderOp*>() );
		RayCaster* caster = new RayCaster( false, 8, *shader, false );
		BDPTIntegrator* integrator = new BDPTIntegrator( 4, 4, StabilityConfig() );
		const Scalar distances[] = { 0.4, 2.5 };
		const Scalar wavelengths[] = { 450, 550, 650 };
		for( Scalar distance : distances ) {
			RayIntersectionGeometric entry = MakeIntersection( 0 );
			entry.ray.Set( Point3( 0, 0, distance ), Vector3( 0, 0, -1 ) );
			RayIntersectionGeometric exit = MakeIntersection( 0 );
			exit.ray.Set( Point3( 0, 0, -distance ), Vector3( 0, 0, 1 ) );
			for( Scalar nm : wavelengths ) {
				const Scalar sigma = 0.2 + 0.0005 * ( nm - 550 );
				const Scalar split = 0.3 + 0.0005 * ( nm - 550 );
				const Scalar beer = std::exp( -sigma * distance );
				Check( RelDiff( trans->EvaluateKrayNM( entry, Vector3(0,0,1), ScatteredRay::eRayDiffuse, nm, iorStack ), diff->GetColorNM(entry,nm) ) < 1e-12,
					"DL-222 entry: diffuse lobe equals reflection painter" );
				Check( RelDiff( trans->EvaluateKrayNM( entry, Vector3(0,0,-1), ScatteredRay::eRayTranslucent, nm, iorStack ), spec->GetColorNM(entry,nm) ) < 1e-12,
					"DL-222 entry: Phong lobe equals transmission painter" );
				Check( RelDiff( trans->EvaluateKrayNM( exit, Vector3(0,0,1), ScatteredRay::eRayDiffuse, nm, inside ), beer*(1-split) ) < 1e-12,
					"DL-222 exit: diffuse lobe equals Beer times one minus scattering" );
				Check( RelDiff( trans->EvaluateKrayNM( exit, Vector3(0,0,-1), ScatteredRay::eRayTranslucent, nm, inside ), beer*split ) < 1e-12,
					"DL-222 exit: backscatter lobe equals Beer times scattering" );
				Check( trans->EvaluateKrayNM( exit, wo, ScatteredRay::eRayRefraction, nm, inside ) < 0,
					"DL-222: unsupported scatter type still declines" );
				for( bool light : { false, true } ) {
					for( ScatteredRay::ScatRayType lobe : { ScatteredRay::eRayDiffuse, ScatteredRay::eRayTranslucent } ) {
						std::vector<BDPTVertex> vertices( 4 );
						// The geometric predecessor is deliberately 0.125 further
						// away than the actual live origin: using predecessor
						// position instead of recorded distance must fail too.
						// A neutral root isolates scatter replay from light emission.
						// Both isLightPath modes exercise the shared consumer; real
						// light/eye producers are checked in TranslucentIORStackTest.
						vertices[0].type = BDPTVertex::CAMERA;
						vertices[0].position = Point3( 0, 0, -distance-0.125 );
						vertices[1].position = Point3( 0, 0, 0 );
						vertices[1].normal = vertices[1].geomNormal = Vector3( 0, 0, 1 );
						vertices[1].onb.CreateFromW( Vector3( 0, 0, 1 ) );
						vertices[1].pMaterial = transMat;
						vertices[1].pObject = g_stubObject;
						vertices[1].insideObject = true;
						vertices[1].scatterType = lobe;
						vertices[1].scatterIncomingDistance = distance;
						vertices[2].position = Point3( 0, 0, lobe == ScatteredRay::eRayDiffuse ? 1 : -1 );
						vertices[2].isDelta = true;
						vertices[3].position = Point3( 0, 0, 2 );
						for( size_t j=0; j<vertices.size(); ++j ) vertices[j].throughputNM = j+1;
						integrator->RecomputeSubpathThroughputNM( vertices, light, 550, nm, *scene, *caster );
						const Scalar splitRatio = lobe == ScatteredRay::eRayDiffuse ? (1-split)/0.7 : split/0.3;
						const Scalar expectedRatio = std::exp( -(sigma-0.2)*distance ) * splitRatio;
						Check( vertices[0].throughputNM == 1 && vertices[1].throughputNM == 2,
							"DL-222 replay: scatter ratio does not affect its own vertex or predecessor" );
						Check( RelDiff( vertices[2].throughputNM, 3*expectedRatio ) < 1e-12 &&
						       RelDiff( vertices[3].throughputNM, 4*expectedRatio ) < 1e-12,
							"DL-222 replay: eye/light downstream throughput uses recorded non-unit Beer distance" );
					}
				}
			}
		}
		Check( trans->PerLobeDensityFallbackName() != 0 &&
		       std::string( trans->PerLobeDensityFallbackName() ) == "TranslucentSPF",
			"DL-222: unsupported lobe retains its diagnostic identity" );
		std::cout << "   BDPTVertex storage: " << sizeof(BDPTVertex) << " bytes" << std::endl;
		integrator->release(); caster->release(); shader->release(); scene->release();
		transMat->release(); tExt->release(); tN->release(); tS->release();

		ext->release(); lam->release();

		// And the five that now answer must NOT name themselves.
		for( size_t i = 0; i < subjects.size(); i++ ) {
			Check( subjects[i].spf->PerLobeDensityFallbackName() == 0,
				"DL-125 section D (" + subjects[i].name + "): no longer names itself to the diagnostic" );
		}
	}

	schSPF->release();  schSPFb->release(); schBRDF->release();
	wiSPF->release();   wiSPFb->release();  wiBRDF->release();  wiSPFd->release(); wiBRDFd->release();
	waSPF->release();   waSPFb->release();  waBRDF->release();  waSPFd->release(); waBRDFd->release();
	ipSPF->release();   ipSPFb->release();  ipBRDF->release();  ipSPFd->release(); ipBRDFd->release();
	asSPF->release();   asSPFb->release();  asBRDF->release();
	schR->release(); wiR->release(); waR->release(); ipR->release(); asR->release();
	rRough->release(); rIso->release(); rAX->release(); rAY->release();
	rExp->release(); rNu->release(); rNv->release();
	rough->release(); iso->release(); alphaX->release(); alphaY->release();
	expo->release();  nu->release();  nv->release();
	black->release(); spec->release(); diff->release();
	g_stubObject->release();

	std::cout << std::endl << "=== Results: " << passCount << " passed, "
	          << failCount << " failed ===" << std::endl;
	return failCount == 0 ? 0 : 1;
}
