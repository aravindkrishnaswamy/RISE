//////////////////////////////////////////////////////////////////////
//
//  SchlickKrayBRDFConsistencyTest.cpp - Red-proof and regression gate
//    for DL-127: `SchlickSPF`'s per-lobe `kray` must be that lobe's
//    own `f_I * cos / p_I`, not the Schlick-1994 sampling weight.
//
//  DL-127 (docs/DL127_SCHLICK_KRAY_VS_BRDF.md).  `SchlickSPF::Scatter`
//  used to set the specular lobe's `kray = rho + (1-rho)*fresnel` --
//  Schlick's own sampling weight -- while `SchlickBRDF::value` returns
//  `(rho + (1-rho)*fresnel) * Z*A/(4*PI*nl*nv)` and the lobe's stored
//  `pdf` is `ComputeSchlickSpecularPdf`.  Those three are one triple:
//  a sampled continuation carries `kray`, NEE evaluates `value`, and
//  MIS divides by `pdf`, so unless `kray * p_I == f_I * cos` the two
//  MIS partners at a `schlick_material` vertex are pricing different
//  BRDFs and the sampled estimator does not integrate the BRDF.
//
//  Historical DL-127 discrepancy (before DL-178, derived in section 2):
//
//      f_S cos / p_S = S * A(phi) * (h.v)
//                        / ( 2 pi * (n.v) * (n.h) * p_phi(phi) )
//
//  with `p_phi` the azimuthal density `GenerateSpecularRay` actually
//  draws from (DL-67 corrected it to
//  `p^2 / (2 pi (p^2 + t_q^2 (1-p^2))^{3/2})`).  The bracket is 1 only
//  when `h == n`; it departs from 1 with roughness and incidence.
//
//  DL-178 additionally multiplies the BRDF and this weight by
//  G(nv)G(nl), G(c)=c/[r+(1-r)c]. Z still cancels; roughness
//  remains through G. The identities below use the live BRDF.
//
//  SECTIONS
//    1. Per-draw identity `kray_I * p_I == f_I * cos` on real
//       `Scatter()` draws over a (rho, roughness, isotropy, theta)
//       grid.  THE red-proof: pre-fix this reads the bracket above.
//    2. Per-channel branch (`RGBScalarPainter` roughness): the same
//       identity per lane, per channel.
//    3. Aggregate energy: `E[sum_I max(kray_I)]` -- what the sampled
//       continuation delivers -- against the BRDF's own quadrature
//       integral `Q = int f_agg cos dw`.
//    4. Spectral (`ScatterNM`) lane: same identity against `valueNM`.
//    5. RULING evidence.  `SchlickBRDF::value` is reciprocal; the BRDF
//       the old `kray` convention implied,
//       `f_impl = kray * p_S / cos = S t Z p_phi / (2 nl (h.v))`,
//       is NOT -- swapping the two directions leaves `h`, `t`, `Z`,
//       `p_phi`, `S` and `(h.v)` untouched and turns `nl` into `nv`,
//       so `f_impl(l->v) / f_impl(v->l) = nl / nv`.  That is why
//       `value()` is the truth side and `kray` is what had to move.
//       This section pins the test's own closed form of `f_impl`
//       against the LIVE SPF's emitted `kray * pdf / cos` first, so
//       the non-reciprocity is measured on the real code's quantity
//       and not on an independent re-derivation of it.
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
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Materials/SchlickSPF.h"
#include "../src/Library/Materials/SchlickBRDF.h"
#include "../src/Library/Materials/CookTorranceSPF.h"
#include "../src/Library/Materials/CookTorranceBRDF.h"
#include "../src/Library/Materials/GGXSPF.h"
#include "../src/Library/Materials/GGXBRDF.h"
#include "../src/Library/Materials/WardIsotropicGaussianSPF.h"
#include "../src/Library/Materials/WardIsotropicGaussianBRDF.h"
#include "../src/Library/Materials/WardAnisotropicEllipticalGaussianSPF.h"
#include "../src/Library/Materials/WardAnisotropicEllipticalGaussianBRDF.h"
#include "../src/Library/Materials/IsotropicPhongSPF.h"
#include "../src/Library/Materials/IsotropicPhongBRDF.h"
#include "../src/Library/Materials/AshikminShirleyAnisotropicPhongSPF.h"
#include "../src/Library/Materials/AshikminShirleyAnisotropicPhongBRDF.h"
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

//////////////////////////////////////////////////////////////////////
// Synthetic flat-surface intersection, normal +Z, incoming ray in the
// X-Z plane at `incomingTheta` from the normal.  Identical geometry to
// tests/SchlickLobePairingTest.cpp and tests/SPFBSDFConsistencyTest.cpp.
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

//! Quadrature reference Q = int over the upper hemisphere of
//! max(f_agg(w)) * cos(theta) dw, from `SchlickBRDF::value()`.
static double QuadratureAggregate(
	const IBSDF& brdf,
	const RayIntersectionGeometric& ri,
	int nTheta = 400,
	int nPhi = 800 )
{
	double sum = 0;
	const double dTheta = (PI * 0.5) / nTheta;
	const double dPhi   = TWO_PI / nPhi;

	for( int it = 0; it < nTheta; it++ ) {
		const double theta = (it + 0.5) * dTheta;
		const double st = sin( theta );
		const double ct = cos( theta );
		for( int ip = 0; ip < nPhi; ip++ ) {
			const double phi = (ip + 0.5) * dPhi;
			const Vector3 wo( st * cos( phi ), st * sin( phi ), ct );
			const double fv = ColorMath::MaxValue( brdf.value( wo, ri ) );
			if( fv > 0 && std::isfinite( fv ) ) {
				sum += fv * ct * st * dTheta * dPhi;
			}
		}
	}
	return sum;
}

//////////////////////////////////////////////////////////////////////
// Section 1/2/4: the per-draw identity.
//
// For every non-delta lobe `Scatter()` emits, `kray_I * p_I` must equal
// `f_I(w) * cos`.  With the diffuse reflectance painter set to BLACK,
// `SchlickBRDF::value` reduces to the specular term alone, so `f_I` for
// the reflection lobe IS `value()` and the identity is directly
// measurable through the public API.  (The diffuse lobe needs no test:
// `kray_D = Rd`, `p_D = cos/pi` and `f_D = Rd/pi` give `f_D cos/p_D =
// Rd` identically -- see the doc, section 2.)
//////////////////////////////////////////////////////////////////////
struct RatioStat
{
	double mean;
	double minR;
	double maxR;
	unsigned int n;
};

static RatioStat MeasureSpecularRatio(
	ISPF& spf,
	const IBSDF& brdf,
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	const IORStack& iorStack,
	unsigned int nDraws,
	int channel )			///< [in] -1 = MaxValue (grey), 0..2 = that channel only
{
	RatioStat st{};
	st.minR = 1e300;
	st.maxR = -1e300;

	const Vector3 n = ri.onb.w();
	double sum = 0;

	for( unsigned int s = 0; s < nDraws; s++ )
	{
		ScatteredRayContainer scattered;
		spf.Scatter( ri, sampler, scattered, iorStack );

		for( unsigned int i = 0; i < scattered.Count(); i++ )
		{
			const ScatteredRay& sr = scattered[i];
			if( sr.type != ScatteredRay::eRayReflection ) {
				continue;
			}
			if( sr.pdf <= 0 ) {
				continue;
			}

			const Vector3 wo = Vector3Ops::Normalize( sr.ray.Dir() );
			const double cosO = Vector3Ops::Dot( wo, n );
			if( cosO <= 0 ) {
				continue;
			}

			const RISEPel fPel = brdf.value( wo, ri );
			const double krayI = ( channel < 0 ) ? ColorMath::MaxValue( sr.kray ) : sr.kray[channel];
			const double fI    = ( channel < 0 ) ? ColorMath::MaxValue( fPel )    : fPel[channel];

			if( fI <= 0 || krayI <= 0 ) {
				continue;
			}

			const double ratio = ( krayI * sr.pdf ) / ( fI * cosO );
			if( !std::isfinite( ratio ) ) {
				continue;
			}

			sum += ratio;
			if( ratio < st.minR ) st.minR = ratio;
			if( ratio > st.maxR ) st.maxR = ratio;
			st.n++;
		}
	}

	if( st.n == 0 ) {
		st.minR = st.maxR = 0;
		return st;
	}
	st.mean = sum / double( st.n );
	return st;
}

//! Spectral twin of the above, against `ScatterNM` / `valueNM`.
static RatioStat MeasureSpecularRatioNM(
	ISPF& spf,
	const IBSDF& brdf,
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	const IORStack& iorStack,
	unsigned int nDraws,
	const Scalar nm,
	const IPainter& diffuse )		///< [in] the BRDF's own diffuse painter -- see below
{
	// `valueNM` adds `GuardedGetColorNM(diffuse) * INV_PI`, and a
	// UniformColorPainter of exact black does NOT return exact zero
	// through the spectral uplift (the Jakob-Hanika sigmoid gives
	// 1-eps / eps, never a hard 0 or 1 -- which is why
	// `GuardedGetColorNM` exists at all, IPainter.h).  That residual is
	// ~1e-5 of radiance, invisible in a render but 3.5e-4 RELATIVE at
	// this test's smallest specular reflectance, so subtract exactly
	// the term `valueNM` added rather than loosening the band.
	const Scalar fDiffuseNM = GuardedGetColorNM( diffuse, ri, nm ) * INV_PI;
	RatioStat st{};
	st.minR = 1e300;
	st.maxR = -1e300;

	const Vector3 n = ri.onb.w();
	double sum = 0;

	for( unsigned int s = 0; s < nDraws; s++ )
	{
		ScatteredRayContainer scattered;
		spf.ScatterNM( ri, sampler, nm, scattered, iorStack );

		for( unsigned int i = 0; i < scattered.Count(); i++ )
		{
			const ScatteredRay& sr = scattered[i];
			if( sr.type != ScatteredRay::eRayReflection || sr.pdf <= 0 ) {
				continue;
			}

			const Vector3 wo = Vector3Ops::Normalize( sr.ray.Dir() );
			const double cosO = Vector3Ops::Dot( wo, n );
			if( cosO <= 0 ) {
				continue;
			}

			const double fI = brdf.valueNM( wo, ri, nm ) - fDiffuseNM;
			if( fI <= 0 || sr.krayNM <= 0 ) {
				continue;
			}

			const double ratio = ( sr.krayNM * sr.pdf ) / ( fI * cosO );
			if( !std::isfinite( ratio ) ) {
				continue;
			}

			sum += ratio;
			if( ratio < st.minR ) st.minR = ratio;
			if( ratio > st.maxR ) st.maxR = ratio;
			st.n++;
		}
	}

	if( st.n == 0 ) {
		st.minR = st.maxR = 0;
		return st;
	}
	st.mean = sum / double( st.n );
	return st;
}

//////////////////////////////////////////////////////////////////////
// Section 3: what the sampled continuation actually delivers.
//
// `E[ sum_I max(kray_I) ]` over `Scatter()` draws is exactly
// `sum_I int p_I(w) max(kray_I(w)) dw` -- the energy the sampled path
// carries away from this vertex, summed over the lobes the container
// offers.  With the DL-127 convention that equals `int f_agg cos dw`,
// which is `Q`.  With the old sampling-weight convention it does not.
//////////////////////////////////////////////////////////////////////
static double MeasureEmittedEnergy(
	ISPF& spf,
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	const IORStack& iorStack,
	unsigned int nDraws )
{
	double sum = 0;
	for( unsigned int s = 0; s < nDraws; s++ ) {
		ScatteredRayContainer scattered;
		spf.Scatter( ri, sampler, scattered, iorStack );
		for( unsigned int i = 0; i < scattered.Count(); i++ ) {
			if( scattered[i].isDelta ) continue;
			sum += ColorMath::MaxValue( scattered[i].kray );
		}
	}
	return sum / double( nDraws );
}

//////////////////////////////////////////////////////////////////////
// Section 5: the ruling.
//
// `SchlickImpliedBRDF` is this test's OWN closed form of the BRDF the
// pre-DL-127 `kray` convention implied, `kray * p_S / cos`, written out
// from the model rather than read back from the SPF:
//
//     f_impl(v->l) = S(h.v) * t * Z(t) * p_phi(phi) / ( 2 * nl * (h.v) )
//
// It is pinned against the LIVE SPF below (`Check` on `maxPin`), so the
// non-reciprocity it then exhibits is a property of the shipped
// quantity, not of this re-derivation.
//////////////////////////////////////////////////////////////////////
static double SchlickAzimuthalDensityRef( const double phi, const double p )
{
	// The density `GenerateSpecularRay` actually draws phi from, in the
	// quadrant-local parametrisation t_q = |phi from this quadrant's
	// axis| / (pi/2) -- see SchlickSPF.cpp's ComputeSchlickSpecularPdf.
	double t;
	if( phi <= PI_OV_TWO )            { t = phi / PI_OV_TWO; }
	else if( phi <= PI )              { t = (PI - phi) / PI_OV_TWO; }
	else if( phi <= PI + PI_OV_TWO )  { t = (phi - PI) / PI_OV_TWO; }
	else                              { t = (TWO_PI - phi) / PI_OV_TWO; }

	const double sqr_p = p * p;
	const double den = sqr_p + t * t * ( 1.0 - sqr_p );
	if( den <= 0 ) {
		return 0;
	}
	return sqr_p / ( TWO_PI * den * sqrt( den ) );
}

static double SchlickImpliedBRDF(
	const Vector3& wv,				///< [in] direction toward the viewer
	const Vector3& wl,				///< [in] direction toward the light
	const double rho,
	const double r,
	const double p )
{
	const Vector3 n( 0, 0, 1 );
	const double nl = Vector3Ops::Dot( wl, n );
	if( nl <= 0 ) {
		return 0;
	}

	const Vector3 h = Vector3Ops::Normalize( Vector3( wv.x + wl.x, wv.y + wl.y, wv.z + wl.z ) );
	const double t = Vector3Ops::Dot( h, n );
	if( t <= 0 ) {
		return 0;
	}
	const double hv = Vector3Ops::Dot( h, wv );
	if( hv <= 0 ) {
		return 0;
	}

	const double zd = ( r * t * t + 1.0 ) - t * t;
	const double Z = r / ( zd * zd );

	double phi = atan2( h.y, h.x );
	if( phi < 0 ) {
		phi += TWO_PI;
	}
	const double pphi = SchlickAzimuthalDensityRef( phi, p );

	const double S = rho + ( 1.0 - rho ) * ::pow( 1.0 - hv, 5 );

	// p_S(l|v) = t * Z * p_phi / (2 * (h.v)),  f_impl = S * p_S / nl.
	return S * t * Z * pphi / ( 2.0 * nl * hv );
}

//! Section 6's generic probe.  `specularOnly` selects the multi-emit
//! contract (one lobe's own `.pdf`, diffuse painter black so `value()`
//! IS that lobe) from the single-emit one (aggregate `.pdf`, full
//! `value()`).
static RatioStat MeasureAnyPairRatio(
	ISPF& spf,
	const IBSDF& brdf,
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	const IORStack& iorStack,
	unsigned int nDraws,
	bool specularOnly )
{
	RatioStat st{};
	st.minR = 1e300;
	st.maxR = -1e300;

	const Vector3 n = ri.onb.w();
	double sum = 0;

	for( unsigned int s = 0; s < nDraws; s++ )
	{
		ScatteredRayContainer scattered;
		spf.Scatter( ri, sampler, scattered, iorStack );

		for( unsigned int i = 0; i < scattered.Count(); i++ )
		{
			const ScatteredRay& sr = scattered[i];
			if( sr.isDelta || sr.pdf <= 0 ) {
				continue;
			}
			if( specularOnly && sr.type != ScatteredRay::eRayReflection ) {
				continue;
			}

			const Vector3 wo = Vector3Ops::Normalize( sr.ray.Dir() );
			const double cosO = Vector3Ops::Dot( wo, n );
			if( cosO <= 0 ) {
				continue;
			}

			const double fI = ColorMath::MaxValue( brdf.value( wo, ri ) );
			const double krayI = ColorMath::MaxValue( sr.kray );
			if( fI <= 0 || krayI <= 0 ) {
				continue;
			}

			const double ratio = ( krayI * sr.pdf ) / ( fI * cosO );
			if( !std::isfinite( ratio ) ) {
				continue;
			}

			sum += ratio;
			if( ratio < st.minR ) st.minR = ratio;
			if( ratio > st.maxR ) st.maxR = ratio;
			st.n++;
		}
	}

	if( st.n == 0 ) {
		st.minR = st.maxR = 0;
		return st;
	}
	st.mean = sum / double( st.n );
	return st;
}

int main()
{
	GlobalLog();

	std::cout << "=== SchlickKrayBRDFConsistencyTest (DL-127) ===" << std::endl;
	std::cout << std::fixed;

	g_stubObject = new StubObject();
	g_stubObject->addref();

	RandomNumberGenerator rng;
	IndependentSampler sampler( rng );
	IORStack iorStack = MakeTestIORStack( g_stubObject );

	UniformColorPainter* black = new UniformColorPainter( RISEPel( 0, 0, 0 ) ); black->addref();

	const double rhos[]   = { 0.1, 0.4, 0.9 };
	const double roughs[] = { 0.1, 0.3, 0.5, 0.8 };
	const double isos[]   = { 1.0, 0.5 };
	const double degs[]   = { 0.0, 30.0, 60.0, 80.0 };

	//----------------------------------------------------------------
	// SECTION 1 -- per-draw identity, RGB lane, grey reflectances.
	//----------------------------------------------------------------
	std::cout << std::endl
	          << "-- Section 1: kray_S * p_S / (f_S cos) over Scatter() draws "
	             "(must be 1; diffuse painter BLACK so value() is the specular term)"
	          << std::endl;
	std::cout << "   rho  rough  iso   theta        mean          min          max" << std::endl;

	double worstMean1 = 0, worstMax1 = 0;
	for( int a = 0; a < 3; a++ ) {
	for( int b = 0; b < 4; b++ ) {
	for( int c = 0; c < 2; c++ ) {
		UniformColorPainter*  rs    = new UniformColorPainter( RISEPel( rhos[a], rhos[a], rhos[a] ) ); rs->addref();
		UniformScalarPainter* rough = new UniformScalarPainter( roughs[b] ); rough->addref();
		UniformScalarPainter* iso   = new UniformScalarPainter( isos[c] );   iso->addref();

		SchlickSPF*  spf  = new SchlickSPF(  *black, *rs, *rough, *iso ); spf->addref();
		SchlickBRDF* brdf = new SchlickBRDF( *black, *rs, *rough, *iso ); brdf->addref();

		for( int d = 0; d < 4; d++ ) {
			const RayIntersectionGeometric ri = MakeIntersection( degs[d] * PI / 180.0 );
			const RatioStat st = MeasureSpecularRatio( *spf, *brdf, ri, sampler, iorStack, 20000, -1 );

			std::cout << "   " << std::setprecision(1) << rhos[a]
			          << "    " << roughs[b]
			          << "    " << isos[c]
			          << "   " << std::setw(4) << degs[d]
			          << "   " << std::setprecision(8) << std::setw(12) << st.mean
			          << " " << std::setw(12) << st.minR
			          << " " << std::setw(12) << st.maxR
			          << "   (n=" << st.n << ")" << std::endl;

			Check( st.n > 1000,
				"Section 1: enough specular draws for the measurement" );
			if( st.n == 0 ) continue;

			const double devMean = fabs( st.mean - 1.0 );
			const double devMax  = std::max( fabs( st.maxR - 1.0 ), fabs( st.minR - 1.0 ) );
			if( devMean > worstMean1 ) worstMean1 = devMean;
			if( devMax  > worstMax1 )  worstMax1  = devMax;

			// The identity is EXACT arithmetic, so the only slack is
			// floating-point rounding through two independently-spelled
			// evaluations of the same product.
			Check( devMax < 1e-6,
				std::string( "DL-127: kray_S * p_S == f_S cos on every draw "
				             "(rho/rough/iso/theta grid cell)" ) );
		}

		spf->release();
		brdf->release();
		rs->release();
		rough->release();
		iso->release();
	}}}
	std::cout << "   worst |mean-1| = " << std::setprecision(9) << worstMean1
	          << "   worst |per-draw - 1| = " << worstMax1 << std::endl;

	//----------------------------------------------------------------
	// SECTION 2 -- per-channel specular branch.
	//----------------------------------------------------------------
	std::cout << std::endl
	          << "-- Section 2: same identity per CHANNEL on Scatter()'s per-channel "
	             "specular branch (RGBScalarPainter roughness)" << std::endl;
	{
		RGBScalarPainter*     rough = new RGBScalarPainter( 0.08, 0.35, 0.9 ); rough->addref();
		UniformScalarPainter* iso   = new UniformScalarPainter( 0.6 );         iso->addref();
		UniformColorPainter*  rs    = new UniformColorPainter( RISEPel( 0.2, 0.5, 0.85 ) ); rs->addref();

		SchlickSPF*  spf  = new SchlickSPF(  *black, *rs, *rough, *iso ); spf->addref();
		SchlickBRDF* brdf = new SchlickBRDF( *black, *rs, *rough, *iso ); brdf->addref();

		for( int d = 0; d < 4; d++ ) {
			const RayIntersectionGeometric ri = MakeIntersection( degs[d] * PI / 180.0 );
			for( int ch = 0; ch < 3; ch++ ) {
				const RatioStat st = MeasureSpecularRatio( *spf, *brdf, ri, sampler, iorStack, 20000, ch );
				std::cout << "   theta=" << std::setw(4) << std::setprecision(1) << degs[d]
				          << " ch=" << ch
				          << "   mean " << std::setprecision(8) << std::setw(12) << st.mean
				          << "   [min " << st.minR << ", max " << st.maxR << "]"
				          << "   (n=" << st.n << ")" << std::endl;
				Check( st.n > 1000, "Section 2: enough per-channel specular draws" );
				if( st.n == 0 ) continue;
				const double devMax = std::max( fabs( st.maxR - 1.0 ), fabs( st.minR - 1.0 ) );
				Check( devMax < 1e-6,
					"DL-127: per-channel lane kray[i] * p_i == f_i cos on every draw" );
			}
		}

		spf->release();
		brdf->release();
		rs->release();
		rough->release();
		iso->release();
	}

	//----------------------------------------------------------------
	// SECTION 3 -- aggregate energy against the BRDF's own integral.
	//----------------------------------------------------------------
	std::cout << std::endl
	          << "-- Section 3: E[sum_I max(kray_I)] vs Q = int max(f_agg) cos dw "
	             "(diffuse 0.2, both lobes live)" << std::endl;
	std::cout << "   rho  rough  iso   theta            Q      E[sum kray]     ratio" << std::endl;
	{
		UniformColorPainter* rd = new UniformColorPainter( RISEPel( 0.2, 0.2, 0.2 ) ); rd->addref();

		double worstRatioDev = 0;
		for( int a = 0; a < 3; a++ ) {
		for( int b = 0; b < 4; b++ ) {
		for( int c = 0; c < 2; c++ ) {
			UniformColorPainter*  rs    = new UniformColorPainter( RISEPel( rhos[a], rhos[a], rhos[a] ) ); rs->addref();
			UniformScalarPainter* rough = new UniformScalarPainter( roughs[b] ); rough->addref();
			UniformScalarPainter* iso   = new UniformScalarPainter( isos[c] );   iso->addref();

			SchlickSPF*  spf  = new SchlickSPF(  *rd, *rs, *rough, *iso ); spf->addref();
			SchlickBRDF* brdf = new SchlickBRDF( *rd, *rs, *rough, *iso ); brdf->addref();

			for( int d = 0; d < 4; d++ ) {
				const RayIntersectionGeometric ri = MakeIntersection( degs[d] * PI / 180.0 );
				const double Q = QuadratureAggregate( *brdf, ri );
				const double E = MeasureEmittedEnergy( *spf, ri, sampler, iorStack, 240000 );
				const double ratio = ( Q > 0 ) ? E / Q : 0;

				std::cout << "   " << std::setprecision(1) << rhos[a]
				          << "    " << roughs[b]
				          << "    " << isos[c]
				          << "   " << std::setw(4) << degs[d]
				          << "   " << std::setprecision(6) << std::setw(10) << Q
				          << "   " << std::setw(12) << E
				          << "   " << std::setw(9) << ratio << std::endl;

				Check( Q > 0, "Section 3: quadrature reference positive" );
				if( Q <= 0 ) continue;
				const double dev = fabs( ratio - 1.0 );
				if( dev > worstRatioDev ) worstRatioDev = dev;

				// 2% covers the 400x800 quadrature's own discretisation of
				// a low-roughness peak plus the 240000-draw MC error.  The
				// worst cell measures 0.0059 (rho=0.9, roughness=0.3,
				// isotropy=1.0, 80 deg) -- the grazing tail is heavy
				// because `kray` now carries the model's own unbounded
				// 1/(n.v) growth (see DL-178), so this row is the noisiest
				// in the suite; the sign of the residual is not systematic
				// across the grid, which is what distinguishes it from a
				// real convention error.
				Check( dev < 0.02,
					"DL-127: sampled continuation delivers the BRDF's own hemispherical integral" );
			}

			spf->release();
			brdf->release();
			rs->release();
			rough->release();
			iso->release();
		}}}
		std::cout << "   worst |E/Q - 1| = " << std::setprecision(6) << worstRatioDev << std::endl;
		rd->release();
	}

	//----------------------------------------------------------------
	// SECTION 4 -- spectral lane.
	//----------------------------------------------------------------
	std::cout << std::endl
	          << "-- Section 4: ScatterNM krayNM * p_S / (valueNM cos) (must be 1)" << std::endl;
	{
		double worstDev = 0;
		for( int a = 0; a < 3; a++ ) {
		for( int b = 0; b < 4; b++ ) {
			UniformColorPainter*  rs    = new UniformColorPainter( RISEPel( rhos[a], rhos[a], rhos[a] ) ); rs->addref();
			UniformScalarPainter* rough = new UniformScalarPainter( roughs[b] ); rough->addref();
			UniformScalarPainter* iso   = new UniformScalarPainter( 0.7 );       iso->addref();

			SchlickSPF*  spf  = new SchlickSPF(  *black, *rs, *rough, *iso ); spf->addref();
			SchlickBRDF* brdf = new SchlickBRDF( *black, *rs, *rough, *iso ); brdf->addref();

			for( int d = 0; d < 4; d++ ) {
				const RayIntersectionGeometric ri = MakeIntersection( degs[d] * PI / 180.0 );
				const RatioStat st = MeasureSpecularRatioNM( *spf, *brdf, ri, sampler, iorStack, 20000, 550.0, *black );
				std::cout << "   rho=" << std::setprecision(1) << rhos[a]
				          << " rough=" << roughs[b]
				          << " theta=" << std::setw(4) << degs[d]
				          << "   mean " << std::setprecision(8) << std::setw(12) << st.mean
				          << "   [min " << st.minR << ", max " << st.maxR << "]" << std::endl;
				Check( st.n > 1000, "Section 4: enough spectral specular draws" );
				if( st.n == 0 ) continue;
				const double devMax = std::max( fabs( st.maxR - 1.0 ), fabs( st.minR - 1.0 ) );
				if( devMax > worstDev ) worstDev = devMax;
				Check( devMax < 1e-6,
					"DL-127: krayNM * p_S == valueNM cos on every ScatterNM draw" );
			}

			spf->release();
			brdf->release();
			rs->release();
			rough->release();
			iso->release();
		}}
		std::cout << "   worst |per-draw - 1| = " << std::setprecision(9) << worstDev << std::endl;
	}

	//----------------------------------------------------------------
	// SECTION 5 -- the ruling: value() is reciprocal, the old kray
	// convention's implied BRDF is not.
	//----------------------------------------------------------------
	std::cout << std::endl
	          << "-- Section 5: RULING.  value() reciprocity, and the "
	             "non-reciprocity of the BRDF the sampling-weight convention implied"
	          << std::endl;
	{
		const double rho = 0.4, r = 0.5, p = 1.0;
		UniformColorPainter*  rs    = new UniformColorPainter( RISEPel( rho, rho, rho ) ); rs->addref();
		UniformScalarPainter* rough = new UniformScalarPainter( r ); rough->addref();
		UniformScalarPainter* iso   = new UniformScalarPainter( p ); iso->addref();
		SchlickSPF*  spf  = new SchlickSPF(  *black, *rs, *rough, *iso ); spf->addref();
		SchlickBRDF* brdf = new SchlickBRDF( *black, *rs, *rough, *iso ); brdf->addref();

		// (a) Pin the test's closed form of f_impl against the LIVE SPF:
		//     on real draws, kray * pdf / cos must equal
		//     SchlickImpliedBRDF at the same direction pair PRE-fix, and
		//     post-fix the SPF no longer uses that convention -- so the
		//     pin is made on the QUANTITY THE TEST CONTROLS: the SPF's
		//     own `pdf` must equal the closed form's p_S = f_impl*nl/S.
		double maxPin = 0;
		unsigned int nPin = 0;
		for( int d = 1; d < 4; d++ ) {
			const RayIntersectionGeometric ri = MakeIntersection( degs[d] * PI / 180.0 );
			const Vector3 wv = Vector3Ops::Normalize( -ri.ray.Dir() );
			for( int s = 0; s < 4000; s++ ) {
				ScatteredRayContainer scattered;
				spf->Scatter( ri, sampler, scattered, iorStack );
				for( unsigned int i = 0; i < scattered.Count(); i++ ) {
					const ScatteredRay& sr = scattered[i];
					if( sr.type != ScatteredRay::eRayReflection || sr.pdf <= 0 ) continue;
					const Vector3 wl = Vector3Ops::Normalize( sr.ray.Dir() );
					const double nl = wl.z;
					if( nl <= 0 ) continue;
					const double fImpl = SchlickImpliedBRDF( wv, wl, rho, r, p );
					if( fImpl <= 0 ) continue;
					// f_impl = S * p_S / nl  =>  p_S = f_impl * nl / S.
					const Vector3 h = Vector3Ops::Normalize(
						Vector3( wv.x + wl.x, wv.y + wl.y, wv.z + wl.z ) );
					const double hv = Vector3Ops::Dot( h, wv );
					const double S = rho + ( 1.0 - rho ) * ::pow( 1.0 - hv, 5 );
					const double pRef = fImpl * nl / S;
					const double rel = fabs( pRef - sr.pdf ) / sr.pdf;
					if( rel > maxPin ) maxPin = rel;
					nPin++;
				}
			}
		}
		std::cout << "   (a) closed-form p_S vs the SPF's own emitted pdf: max relative "
		          << std::setprecision(3) << std::scientific << maxPin
		          << std::fixed << "  (n=" << nPin << ")" << std::endl;
		Check( nPin > 1000, "Section 5: enough draws to pin the closed form" );
		Check( maxPin < 1e-9,
			"Section 5: the test's closed form of the implied BRDF reproduces the "
			"SPF's own specular density exactly" );

		// (b) SchlickBRDF::value is RECIPROCAL; f_impl is NOT, by nv/nl.
		const double pairDeg[][2] = { {20,50}, {30,70}, {10,80}, {45,60}, {5,75} };
		double maxValueAsym = 0, minImplRatioErr = 1e300, maxImplRatioErr = 0, maxImplAsym = 0;
		for( int k = 0; k < 5; k++ ) {
			const double tv = pairDeg[k][0] * PI / 180.0;
			const double tl = pairDeg[k][1] * PI / 180.0;
			const Vector3 wv( sin( tv ), 0, cos( tv ) );
			const Vector3 wl( sin( tl ), 0, cos( tl ) );

			const RayIntersectionGeometric riV = MakeIntersection( tv );
			const RayIntersectionGeometric riL = MakeIntersection( tl );

			const double fVL = ColorMath::MaxValue( brdf->value( wl, riV ) );
			const double fLV = ColorMath::MaxValue( brdf->value( wv, riL ) );
			const double asym = ( fVL > 0 ) ? fabs( fLV - fVL ) / fVL : 0;
			if( asym > maxValueAsym ) maxValueAsym = asym;

			const double iVL = SchlickImpliedBRDF( wv, wl, rho, r, p );
			const double iLV = SchlickImpliedBRDF( wl, wv, rho, r, p );
			const double implAsym = ( iVL > 0 ) ? fabs( iLV - iVL ) / iVL : 0;
			if( implAsym > maxImplAsym ) maxImplAsym = implAsym;

			// Predicted exactly: f_impl(v->l) = S p_S / nl and
			// f_impl(l->v) = S p_S / nv with the SAME p_S (it depends on
			// h and (h.v) only, both symmetric), so the ratio is nl/nv.
			const double predicted = cos( tl ) / cos( tv );
			const double measured  = ( iVL > 0 ) ? iLV / iVL : 0;
			const double err = fabs( measured - predicted ) / predicted;
			if( err < minImplRatioErr ) minImplRatioErr = err;
			if( err > maxImplRatioErr ) maxImplRatioErr = err;

			std::cout << "   (b) theta_v=" << std::setprecision(0) << pairDeg[k][0]
			          << " theta_l=" << pairDeg[k][1]
			          << "   value() asym " << std::setprecision(3) << std::scientific << asym
			          << "   f_impl(l->v)/f_impl(v->l) = " << std::fixed << std::setprecision(6) << measured
			          << "  (predicted nl/nv = " << predicted << ")" << std::endl;
		}
		Check( maxValueAsym < 1e-12,
			"Section 5: SchlickBRDF::value is reciprocal (it is the truth side)" );
		Check( maxImplAsym > 0.2,
			"Section 5: the sampling-weight convention's implied BRDF is materially NON-reciprocal" );
		Check( maxImplRatioErr < 1e-9,
			"Section 5: that non-reciprocity is exactly the factor nl/nv the derivation predicts" );

		spf->release();
		brdf->release();
		rs->release();
		rough->release();
		iso->release();
	}

	//----------------------------------------------------------------
	// SECTION 6 -- SIBLING AUDIT (DL-177, REPORTING ONLY, NOT GATED).
	//
	// "Is this SPF's `kray` the transport weight its own paired BSDF
	// implies?" asked of every other SPF/BRDF pair in the tree, with the
	// same probe section 1 uses.  The contract differs by sampler shape:
	//
	//   * MULTI-EMIT SPFs (one `ScatteredRay` per lobe, each with its
	//     OWN `.pdf`) owe `kray_I * p_I == f_I cos` per lobe -- the
	//     diffuse painter is BLACK so `value()` reduces to the specular
	//     term and `f_I` is directly readable, exactly as in section 1.
	//   * SINGLE-EMIT SPFs (one `ScatteredRay` whose `.pdf` is the
	//     AGGREGATE `mixPdf`; DL-69 lists `GGXSPF` and `CookTorranceSPF`)
	//     owe `kray * p_agg == f_agg cos` -- so the diffuse painter stays
	//     lit and the full `value()` is the reference.
	//
	// Measured verdicts (2026-09-17, printed below so they stay live;
	// the two Ward rows re-measured 2026-09-18 after DL-177 closed):
	//
	//   IsotropicPhongSPF                       1.000000 exactly, all 4 angles -- IMMUNE
	//   AshikminShirleyAnisotropicPhongSPF      1.000000 exactly, all 4 angles -- IMMUNE
	//   CookTorranceSPF (single-emit)           mean 0.999-1.006  -- IMMUNE
	//   GGXSPF (single-emit)                    mean 0.990-1.002  -- IMMUNE
	//   WardIsotropicGaussianSPF                1.10 / 1.39 / 2.95 / 5.88  -> 1.000000 (DL-177)
	//   WardAnisotropicEllipticalGaussianSPF    1.06 / 1.32 / 2.97 / 6.07  -> 1.000000 (DL-177)
	//
	// The two single-emit rows are not pointwise 1 and are not supposed
	// to be: their `kray` is the INTERNAL-selection estimator
	// `f_I cos / (p_agg * pSelect_I)`, whose expectation over that
	// internal choice is `f_agg cos / p_agg` -- which is why their MEAN
	// lands on 1 while individual draws span [0.006, 37].  Those two
	// stay PRINTED-only.
	//
	// DL-177 CLOSED 2026-09-18 (docs/DL177_WARD_DENSITY_AND_KRAY.md),
	// so every MULTI-EMIT row -- the Schlick control, both Wards, both
	// Phongs -- is now GATED on the per-lobe identity holding to 1e-9
	// per draw, not merely on the probe having run.  The Ward rows were
	// the last per-lobe SPFs in the tree that violated it.
	//----------------------------------------------------------------
	std::cout << std::endl
	          << "-- Section 6: SIBLING AUDIT (DL-177, reporting only): "
	             "kray * pdf / (f cos) for every other SPF/BRDF pair" << std::endl;
	{
		UniformColorPainter*  lit    = new UniformColorPainter( RISEPel( 0.2, 0.2, 0.2 ) ); lit->addref();
		UniformColorPainter*  spec   = new UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) ); spec->addref();
		UniformScalarPainter* alpha  = new UniformScalarPainter( 0.3 );  alpha->addref();
		UniformScalarPainter* alphaY = new UniformScalarPainter( 0.12 ); alphaY->addref();
		UniformScalarPainter* iorSc  = new UniformScalarPainter( 1.5 );  iorSc->addref();
		UniformScalarPainter* extSc  = new UniformScalarPainter( 0.0 );  extSc->addref();
		UniformScalarPainter* expSc  = new UniformScalarPainter( 40.0 ); expSc->addref();
		UniformScalarPainter* nuSc   = new UniformScalarPainter( 100.0 ); nuSc->addref();
		UniformScalarPainter* nvSc   = new UniformScalarPainter( 20.0 );  nvSc->addref();

		// MULTI-EMIT (per-lobe `.pdf`): probe the specular lobe with a
		// black diffuse painter.
		WardIsotropicGaussianSPF*  wiS = new WardIsotropicGaussianSPF( *black, *spec, *alpha ); wiS->addref();
		WardIsotropicGaussianBRDF* wiB = new WardIsotropicGaussianBRDF( *black, *spec, *alpha ); wiB->addref();
		WardAnisotropicEllipticalGaussianSPF*  waS = new WardAnisotropicEllipticalGaussianSPF( *black, *spec, *alpha, *alphaY ); waS->addref();
		WardAnisotropicEllipticalGaussianBRDF* waB = new WardAnisotropicEllipticalGaussianBRDF( *black, *spec, *alpha, *alphaY ); waB->addref();
		IsotropicPhongSPF*  ipS = new IsotropicPhongSPF( *black, *spec, *expSc ); ipS->addref();
		IsotropicPhongBRDF* ipB = new IsotropicPhongBRDF( *black, *spec, *expSc ); ipB->addref();
		AshikminShirleyAnisotropicPhongSPF*  asS = new AshikminShirleyAnisotropicPhongSPF( *nuSc, *nvSc, *black, *spec ); asS->addref();
		AshikminShirleyAnisotropicPhongBRDF* asB = new AshikminShirleyAnisotropicPhongBRDF( *nuSc, *nvSc, *black, *spec ); asB->addref();

		// SINGLE-EMIT (aggregate `.pdf`): probe every non-delta ray
		// against the FULL `value()`, diffuse lit.
		CookTorranceSPF*  ctS = new CookTorranceSPF( *lit, *spec, *alpha, *iorSc, *extSc ); ctS->addref();
		CookTorranceBRDF* ctB = new CookTorranceBRDF( *lit, *spec, *alpha, *iorSc, *extSc ); ctB->addref();
		GGXSPF*  ggS = new GGXSPF( *lit, *spec, *alpha, *alpha, *iorSc, *extSc ); ggS->addref();
		GGXBRDF* ggB = new GGXBRDF( *lit, *spec, *alpha, *alpha, *iorSc, *extSc ); ggB->addref();

		struct AuditEntry { const char* name; ISPF* spf; IBSDF* brdf; bool specularOnly; };
		const AuditEntry audit[] = {
			{ "SchlickSPF (this row's subject, FIXED)", 0, 0, true },	// filled below
			{ "WardIsotropicGaussianSPF",               wiS, wiB, true },
			{ "WardAnisotropicEllipticalGaussianSPF",   waS, waB, true },
			{ "IsotropicPhongSPF",                      ipS, ipB, true },
			{ "AshikminShirleyAnisotropicPhongSPF",     asS, asB, true },
			{ "CookTorranceSPF (single-emit)",          ctS, ctB, false },
			{ "GGXSPF (single-emit)",                   ggS, ggB, false },
		};

		// The control row: the fixed SchlickSPF at the same settings.
		SchlickSPF*  scS = new SchlickSPF(  *black, *spec, *alpha, *nuSc ); scS->addref();
		SchlickBRDF* scB = new SchlickBRDF( *black, *spec, *alpha, *nuSc ); scB->addref();

		for( int e = 0; e < 7; e++ ) {
			ISPF*  spfP  = ( e == 0 ) ? (ISPF*)scS  : audit[e].spf;
			IBSDF* brdfP = ( e == 0 ) ? (IBSDF*)scB : audit[e].brdf;
			std::cout << "   " << audit[e].name << std::endl;
			for( int d = 0; d < 4; d++ ) {
				const RayIntersectionGeometric ri = MakeIntersection( degs[d] * PI / 180.0 );
				const RatioStat st = MeasureAnyPairRatio( *spfP, *brdfP, ri, sampler, iorStack,
				                                          20000, audit[e].specularOnly );
				std::cout << "      theta=" << std::setw(4) << std::setprecision(1) << degs[d]
				          << "   mean " << std::setprecision(6) << std::setw(10) << st.mean
				          << "   [min " << st.minR << ", max " << st.maxR << "]"
				          << "   (n=" << st.n << ")" << std::endl;
				Check( st.n > 1000,
					std::string( "Section 6: sibling probe produced draws: " ) + audit[e].name );
				if( audit[e].specularOnly ) {
					// Multi-emit contract: EVERY draw must satisfy
					// `kray_I p_I == f_I cos` exactly (DL-127 for
					// Schlick, DL-177 for the two Wards; the two Phongs
					// always did).
					Check( fabs( st.minR - 1.0 ) < 1e-9 && fabs( st.maxR - 1.0 ) < 1e-9,
						std::string( "Section 6: per-lobe kray identity holds per draw: " ) + audit[e].name );
				}
			}
		}

		scS->release(); scB->release();
		wiS->release(); wiB->release(); waS->release(); waB->release();
		ipS->release(); ipB->release(); asS->release(); asB->release();
		ctS->release(); ctB->release(); ggS->release(); ggB->release();
		lit->release(); spec->release(); alpha->release(); alphaY->release();
		iorSc->release(); extSc->release(); expSc->release(); nuSc->release(); nvSc->release();
	}

	black->release();
	g_stubObject->release();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
