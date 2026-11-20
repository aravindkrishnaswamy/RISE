//////////////////////////////////////////////////////////////////////
//
//  BlenderLobeAdditionsTest.cpp - the two lobe additions the Blender
//    shader-graph translation needs (docs/BLENDER_PROCEDURAL_TRANSLATION_DESIGN.md
//    section 5.6): `orennayar_material model energy_preserving` (EON, Cycles'
//    Diffuse BSDF) and `ggx_material fresnel_mode constant` (Cycles' Glossy
//    BSDF, colour times microfacet with no Fresnel term).
//
//  EON
//    A. White furnace: at rho = 1 the directional albedo is 1 (quadrature
//       of the real BRDF at 6 roughnesses x 5 angles up to 88 degrees).  The
//       classic model at sigma 0.5 radians, 60 degrees, is the control that
//       loses energy.
//    B. The closed forms the BRDF is built on: E(mu) (Fujii's G) and Eavg
//       against independent quadratures of the single-scatter term.
//    C. Cycles parity: f cos equals a transcription of Blender 5.3's
//       bsdf_oren_nayar.h times the closure weight (its roughness-0 early
//       return omitted; roughness 0.1 and up), colours 0.2 .. 1.3.
//    D. Reciprocity; RGB/NM agreement at one direction pair; roughness 0 is
//       exactly Lambertian; per-channel colour and roughness are honoured.
//    E. Sampler: every sampled ray's weight is f cos / pdf (RGB and NM),
//       front- and back-face records.
//    F. hemisphericalAlbedo against a double quadrature.
//
//  GGX constant
//    G. At colour 1 the mode is the same function as schlick_f0 with F0 = 1
//       (BRDF value and bihemispherical albedo).
//    H. The single-scatter lobe is colour times the colour-1 lobe at every
//       angle; schlick_f0 at the same colour is the control that rises at
//       grazing.
//    I. White furnace through the sampler, sampler/evaluator agreement for
//       a tinted lobe over a diffuse base, and the tinted multiscatter
//       albedo rs Ess + F_ms(rs) (1 - Ess).
//    I2. The diffuse lobe's flat (1 - rs)^2; the sampler drawing exactly what
//       schlick_f0 F0 = 1 draws; a colour authored far above 1 in one
//       channel never making that channel negative (ComputeFms per channel).
//    J. RGB/NM agreement.
//    L. coated_material over a constant-mode base: through a coat of index
//       1 its white-sky albedo is the base's own; through a coat of index
//       1.5 a constant base with rs 0 matches a coated Lambertian (the
//       recycling model's flat constant-mode diffuse shape).
//    M. Live CST edits of `model` / `fresnel_mode` / roughness and the
//       snapshot clone keep the new options.
//
//  Scene language
//    K. `fresnel_mode constant` and `model energy_preserving` parse to the
//       new modes; the defaults are unchanged; an unknown model is refused.
//
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/math_utils.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/Color/ColorMath.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/IBSDF.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Utilities/MicrofacetEnergyLUT.h"
#include "../src/Library/Materials/OrenNayarBRDF.h"
#include "../src/Library/Materials/OrenNayarSPF.h"
#include "../src/Library/Materials/OrenNayarMaterial.h"
#include "../src/Library/Materials/GGXBRDF.h"
#include "../src/Library/Materials/GGXSPF.h"
#include "../src/Library/Materials/GGXMaterial.h"
#include "../src/Library/Objects/SnapshotLeafClone.h"
#include "../src/Library/Job.h"
#include "TestStubObject.h"
#include "TestTempDir.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	const double kPi = 3.14159265358979323846;

	int g_pass = 0;
	int g_fail = 0;

	//! The IOR stack's current object (none of these lobes refracts).
	StubObject* g_stubObject = 0;

	void Check( bool ok, const std::string& what )
	{
		if( ok ) { ++g_pass; }
		else     { ++g_fail; std::printf( "  FAIL: %s\n", what.c_str() ); }
	}

	void CheckClose( double got, double want, double tol, const std::string& what )
	{
		const bool ok = std::fabs( got - want ) <= tol;
		if( ok ) { ++g_pass; }
		else {
			++g_fail;
			std::printf( "  FAIL: %s  (got %.9g, want %.9g, |diff| %.3g > %.3g)\n",
				what.c_str(), got, want, std::fabs( got - want ), tol );
		}
	}

	template< typename T >
	class Owned
	{
	public:
		//! Adopts the reference a fresh `new` already holds.
		explicit Owned( T* ptr ) : p( ptr ) {}
		~Owned() { safe_release( p ); }
		Owned( const Owned& ) = delete;
		Owned& operator=( const Owned& ) = delete;
		T& operator*() const { return *p; }
		T* operator->() const { return p; }
	private:
		T* p;
	};

	//! Shading normal +Z; the view direction (r = -ray dir) is `wo`.
	RayIntersectionGeometric MakeRIForView( const Vector3& woIn )
	{
		const Vector3 wo = Vector3Ops::Normalize( woIn );
		const Vector3 inDir = wo * -1.0;
		Ray inRay( Point3( wo.x, wo.y, wo.z ), inDir );
		RasterizerState rs = { 0, 0 };
		RayIntersectionGeometric ri( inRay, rs );
		ri.bHit = true;
		ri.range = 1.0;
		ri.ptIntersection = Point3( 0, 0, 0 );
		ri.vNormal = Vector3( 0, 0, 1 );
		ri.vGeomNormal = Vector3( 0, 0, 1 );
		ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
		ri.ptCoord = Point2( 0.5, 0.5 );
		return ri;
	}

	Vector3 Dir( double thetaDeg, double phiDeg )
	{
		const double t = thetaDeg * kPi / 180.0, p = phiDeg * kPi / 180.0;
		return Vector3( std::sin(t) * std::cos(p), std::sin(t) * std::sin(p), std::cos(t) );
	}

	//! integral of f(wi; view wo) cos(wi) over the hemisphere of wi: the
	//! directional albedo seen from wo (equal to the one lit from wo by
	//! reciprocity).  Midpoint in mu and phi.
	double DirectionalAlbedo( const IBSDF& brdf, const Vector3& wo, int channel, int nMu, int nPhi )
	{
		const RayIntersectionGeometric ri = MakeRIForView( wo );
		double sum = 0;
		for( int i = 0; i < nMu; i++ ) {
			const double mu = ( i + 0.5 ) / nMu;
			const double s = std::sqrt( 1.0 - mu * mu );
			for( int j = 0; j < nPhi; j++ ) {
				const double phi = 2.0 * kPi * ( j + 0.5 ) / nPhi;
				const Vector3 wi( s * std::cos(phi), s * std::sin(phi), mu );
				sum += brdf.value( wi, ri )[channel] * mu;
			}
		}
		return sum * ( 1.0 / nMu ) * ( 2.0 * kPi / nPhi );
	}

	//////////////////////////////////////////////////////////////////
	// EON
	//////////////////////////////////////////////////////////////////

	//! Fujii's G(mu), Cycles' own form (with its grazing Taylor fallback).
	double CyclesG( double cosTheta )
	{
		if( cosTheta < 1e-6 ) {
			return ( kPi / 2 - 2.0 / 3.0 ) - cosTheta;
		}
		const double sinTheta = std::sqrt( std::max( 0.0, 1.0 - cosTheta * cosTheta ) );
		const double theta = std::acos( std::min( 1.0, cosTheta ) );
		return sinTheta * ( theta - 2.0 / 3.0 - sinTheta * cosTheta ) +
			2.0 / 3.0 * ( sinTheta / cosTheta ) * ( 1.0 - sinTheta * sinTheta * sinTheta );
	}

	//! Blender 5.3's bsdf_oren_nayar_eval(), transcribed: the closure weight
	//! (the colour, for the Diffuse BSDF node) times get_intensity, which
	//! already carries the n.l factor.  `wi` is Cycles' view direction,
	//! `wo` the light.
	double CyclesOrenNayarEval( double color, double roughness, const Vector3& view, const Vector3& light )
	{
		const Vector3 n( 0, 0, 1 );
		const double sigma = std::min( 1.0, std::max( 0.0, roughness ) );
		const double a = 1.0 / ( kPi + sigma * ( kPi / 2 - 2.0 / 3.0 ) );
		const double b = sigma * a;
		const double albedo = std::min( 1.0, std::max( 0.0, color ) );	// saturate(color)
		const double Eavg = a * kPi + ( ( 2.0 * kPi - 5.6 ) / 3.0 ) * b;
		const double Ems = ( 1.0 / kPi ) * albedo * ( Eavg / ( 1.0 - Eavg ) ) / ( 1.0 - albedo * ( 1.0 - Eavg ) );
		const double nv = std::max( Vector3Ops::Dot( n, view ), 0.0 );
		const double Ev = a * kPi + b * CyclesG( nv );
		const double multiscatterTerm = Ems * ( 1.0 - Ev );

		const double nl = std::max( Vector3Ops::Dot( n, light ), 0.0 );
		double t = Vector3Ops::Dot( light, view ) - nl * nv;
		if( t > 0 ) {
			t /= std::max( nl, nv ) + FLT_MIN;
		}
		const double single = a + b * t;
		const double El = a * kPi + b * CyclesG( nl );
		const double intensity = nl * ( single + multiscatterTerm * ( 1.0 - El ) );
		return color * intensity;
	}

	//! Single-scatter FON term alone, written independently of the
	//! library: (A + B s/t)/pi per unit albedo.
	double FONSingle( double sigma, const Vector3& wi, const Vector3& wo )
	{
		const double A = 1.0 / ( 1.0 + ( 0.5 - 2.0 / ( 3.0 * kPi ) ) * sigma );
		const double B = sigma * A;
		const double s = Vector3Ops::Dot( wi, wo ) - wi.z * wo.z;
		const double st = s > 0 ? s / std::max( wi.z, wo.z ) : s;
		return ( A + B * st ) / kPi;
	}

	void TestEONFurnace()
	{
		std::printf( "-- A. EON white furnace --\n" );
		Owned<UniformColorPainter> white( new UniformColorPainter( RISEPel( 1, 1, 1 ) ) );
		double worst = 0;
		for( double sigma : { 0.0, 0.1, 0.3, 0.5, 0.8, 1.0 } ) {
			Owned<UniformScalarPainter> r( new UniformScalarPainter( sigma ) );
			Owned<OrenNayarBRDF> eon( new OrenNayarBRDF( *white, *r, true ) );
			for( double deg : { 0.0, 30.0, 60.0, 80.0, 88.0 } ) {
				const double E = DirectionalAlbedo( *eon, Dir( deg, 0 ), 0, 512, 256 );
				worst = std::max( worst, std::fabs( E - 1.0 ) );
				char buf[128];
				std::snprintf( buf, sizeof buf, "EON rho=1 sigma=%.1f theta=%.0f: albedo 1", sigma, deg );
				// The closed form is exactly 1; 2e-5 is the quadrature's own
				// error (measured worst 5.8e-6).
				CheckClose( E, 1.0, 2e-5, buf );
			}
		}
		std::printf( "  worst |albedo - 1| = %.2e\n", worst );

		// Control: the classic model at radians-roughness 0.5 loses energy,
		// so the furnace above can tell the two models apart.
		Owned<UniformScalarPainter> r( new UniformScalarPainter( 0.5 ) );
		Owned<OrenNayarBRDF> classic( new OrenNayarBRDF( *white, *r ) );
		const double Ec = DirectionalAlbedo( *classic, Dir( 60, 0 ), 0, 512, 256 );
		std::printf( "  control: classic sigma=0.5 theta=60 albedo %.4f\n", Ec );
		Check( Ec < 0.95, "control: classic Oren-Nayar at sigma 0.5 loses energy" );
	}

	void TestEONClosedForms()
	{
		std::printf( "-- B. EON closed forms (E(mu), Eavg) --\n" );
		for( double sigma : { 0.25, 0.6, 1.0 } ) {
			// E(mu): quadrature of the independent FON term over wi.
			double eavgQuad = 0;
			const int nMuOuter = 200;
			for( int k = 0; k < nMuOuter; k++ ) {
				const double muO = ( k + 0.5 ) / nMuOuter;
				const Vector3 wo( std::sqrt( 1 - muO * muO ), 0, muO );
				double e = 0;
				const int nMu = 400, nPhi = 256;
				for( int i = 0; i < nMu; i++ ) {
					const double mu = ( i + 0.5 ) / nMu, s = std::sqrt( 1 - mu * mu );
					for( int j = 0; j < nPhi; j++ ) {
						const double phi = 2 * kPi * ( j + 0.5 ) / nPhi;
						e += FONSingle( sigma, Vector3( s * std::cos(phi), s * std::sin(phi), mu ), wo ) * mu;
					}
				}
				e *= ( 1.0 / nMu ) * ( 2 * kPi / nPhi );
				eavgQuad += 2.0 * e * muO / nMuOuter;
				if( k % 50 == 25 ) {
					char buf[128];
					std::snprintf( buf, sizeof buf, "E(mu=%.3f) sigma=%.2f matches quadrature", muO, sigma );
					CheckClose( OrenNayarBRDF::EONSingleScatterAlbedo( sigma, muO ), e, 5e-4, buf );
				}
			}
			// Eavg from the bihemispherical albedo at rho -> 0, where the
			// multiple-scattering term (rho^2) is negligible: R/rho = Eavg.
			const double rho = 1e-6;
			const double eavg = OrenNayarBRDF::EONBihemisphericalAlbedo( rho, sigma ) / rho;
			char buf[128];
			std::snprintf( buf, sizeof buf, "Eavg sigma=%.2f matches 2 int E(mu) mu dmu", sigma );
			CheckClose( eavg, eavgQuad, 5e-4, buf );
			std::printf( "  sigma=%.2f  Eavg closed form %.6f  quadrature %.6f\n", sigma, eavg, eavgQuad );
		}
		CheckClose( OrenNayarBRDF::EONSingleScatterAlbedo( 0.7, 0.0 ), 1.0, 1e-12, "E(mu=0) is 1 (grazing)" );
		{
			// 1 - E(mu) -> (B/pi) mu as mu -> 0.  The unrewritten Fujii form
			// (sin/cos)(1 - sin^3) reads this ratio as about 2 at mu = 1e-9.
			const double s = 0.7, A = 1.0 / ( 1.0 + ( 0.5 - 2.0 / ( 3.0 * kPi ) ) * s ), B = s * A;
			const double mu = 1e-9;
			CheckClose( ( 1.0 - OrenNayarBRDF::EONSingleScatterAlbedo( s, mu ) ) / ( B / kPi * mu ), 1.0, 1e-3,
				"1 - E(mu) has the right slope at grazing (no cancellation)" );
		}
	}

	void TestEONCyclesParity()
	{
		std::printf( "-- C. EON matches Blender 5.3's bsdf_oren_nayar.h --\n" );
		double worst = 0;
		for( double color : { 0.2, 0.5, 0.8, 1.0, 1.3 } ) {
			for( double sigma : { 0.1, 0.5, 1.0 } ) {
				Owned<UniformColorPainter> c( new UniformColorPainter( RISEPel( color, color, color ) ) );
				Owned<UniformScalarPainter> r( new UniformScalarPainter( sigma ) );
				Owned<OrenNayarBRDF> eon( new OrenNayarBRDF( *c, *r, true ) );
				for( double tv : { 0.0, 35.0, 70.0, 85.0 } ) {
					for( double tl : { 10.0, 45.0, 80.0 } ) {
						for( double dphi : { 0.0, 90.0, 180.0 } ) {
							const Vector3 view = Dir( tv, 0 ), light = Dir( tl, dphi );
							const double ours = eon->value( light, MakeRIForView( view ) )[0] * light.z;
							const double cycles = CyclesOrenNayarEval( color, sigma, view, light );
							const double rel = std::fabs( ours - cycles ) / std::max( 1e-12, cycles );
							worst = std::max( worst, rel );
						}
					}
				}
			}
		}
		std::printf( "  worst relative difference %.2e\n", worst );
		Check( worst < 1e-9, "EON f cos equals Cycles 5.3 (weight times intensity)" );
	}

	void TestEONSymmetryAndTwins()
	{
		std::printf( "-- D. EON reciprocity, RGB/NM, roughness 0 --\n" );
		Owned<UniformColorPainter> c( new UniformColorPainter( RISEPel( 0.7, 0.4, 0.9 ) ) );
		Owned<UniformScalarPainter> r( new UniformScalarPainter( 0.65 ) );
		Owned<OrenNayarBRDF> eon( new OrenNayarBRDF( *c, *r, true ) );
		double worstRecip = 0;
		for( double ta : { 5.0, 40.0, 75.0 } ) for( double tb : { 20.0, 60.0, 85.0 } ) for( double dp : { 0.0, 70.0, 180.0 } ) {
			const Vector3 a = Dir( ta, 0 ), b = Dir( tb, dp );
			const RISEPel ab = eon->value( a, MakeRIForView( b ) );
			const RISEPel ba = eon->value( b, MakeRIForView( a ) );
			for( int k = 0; k < 3; k++ ) worstRecip = std::max( worstRecip, std::fabs( ab[k] - ba[k] ) / std::max( 1e-12, ab[k] ) );
		}
		Check( worstRecip < 1e-12, "EON is reciprocal" );

		// NM: a white painter uplifts to exactly 1 at every wavelength.
		Owned<UniformColorPainter> white( new UniformColorPainter( RISEPel( 1, 1, 1 ) ) );
		Owned<OrenNayarBRDF> eonW( new OrenNayarBRDF( *white, *r, true ) );
		const Vector3 v = Dir( 50, 0 ), l = Dir( 30, 120 );
		const RayIntersectionGeometric ri = MakeRIForView( v );
		CheckClose( eonW->valueNM( l, ri, 550.0 ), eonW->value( l, ri )[0], 1e-12, "EON valueNM equals value" );

		Owned<UniformScalarPainter> zero( new UniformScalarPainter( 0.0 ) );
		Owned<OrenNayarBRDF> flat( new OrenNayarBRDF( *c, *zero, true ) );
		const RISEPel f = flat->value( l, ri );
		CheckClose( f[1], 0.4 / kPi, 1e-15, "EON at roughness 0 is Lambertian" );

		// Per-channel colour and roughness: channel k of an RGB material
		// equals a grey material built from channel k's own values.
		const double rho[3] = { 0.7, 0.4, 0.9 }, sig[3] = { 0.1, 0.55, 1.0 };
		Owned<RGBScalarPainter> rr( new RGBScalarPainter( sig[0], sig[1], sig[2] ) );
		Owned<OrenNayarBRDF> mixed( new OrenNayarBRDF( *c, *rr, true ) );
		const RISEPel fm = mixed->value( l, ri );
		RISEPel hm;
		mixed->hemisphericalAlbedo( ri, hm );
		for( int k = 0; k < 3; k++ ) {
			Owned<UniformColorPainter> ck( new UniformColorPainter( RISEPel( rho[k], rho[k], rho[k] ) ) );
			Owned<UniformScalarPainter> sk( new UniformScalarPainter( sig[k] ) );
			Owned<OrenNayarBRDF> grey( new OrenNayarBRDF( *ck, *sk, true ) );
			RISEPel hg;
			grey->hemisphericalAlbedo( ri, hg );
			char buf[96];
			std::snprintf( buf, sizeof buf, "EON channel %d uses its own colour and roughness", k );
			CheckClose( fm[k], grey->value( l, ri )[0], 1e-15, buf );
			std::snprintf( buf, sizeof buf, "EON hemisphericalAlbedo channel %d", k );
			CheckClose( hm[k], hg[0], 1e-15, buf );
		}
	}

	void TestEONSampler()
	{
		std::printf( "-- E. EON sampler weight is f cos / pdf --\n" );
		Owned<UniformColorPainter> c( new UniformColorPainter( RISEPel( 0.9, 0.5, 0.2 ) ) );
		Owned<UniformScalarPainter> r( new UniformScalarPainter( 0.8 ) );
		Owned<OrenNayarBRDF> brdf( new OrenNayarBRDF( *c, *r, true ) );
		Owned<OrenNayarSPF> spf( new OrenNayarSPF( *c, *r, true ) );
		RandomNumberGenerator rng;
		IndependentSampler sampler( rng );
		const IORStack stack = MakeTestIORStack( g_stubObject );
		double worst = 0, worstNM = 0;
		int n = 0, backRays = 0, weightless = 0;
		for( double deg : { 0.0, 45.0, 80.0, 180.0 - 30.0, 180.0 - 75.0 } ) {
			// Angles past 90 view the surface from behind: the BRDF, the
			// sampler and the density must all flip to the same side.
			const RayIntersectionGeometric ri = MakeRIForView( Dir( deg, 30 ) );
			for( int i = 0; i < 2000; i++ ) {
				ScatteredRayContainer sc;
				spf->Scatter( ri, sampler, sc, stack );
				for( unsigned int k = 0; k < sc.Count(); k++ ) {
					const ScatteredRay& s = sc[k];
					const Vector3 d = s.ray.Dir();
					const RISEPel want = brdf->value( d, ri ) * ( std::fabs( d.z ) / s.pdf );
					for( int ch = 0; ch < 3; ch++ ) worst = std::max( worst, std::fabs( s.kray[ch] - want[ch] ) );
					CheckClose( spf->Pdf( ri, d, stack ), s.pdf, 1e-12, "stored pdf equals Pdf()" );
					if( !( std::fabs( s.kray[0] ) > 0 ) ) ++weightless;
					++n;
					if( deg > 90 ) ++backRays;
				}
				ScatteredRayContainer scNM;
				spf->ScatterNM( ri, sampler, 600.0, scNM, stack );
				for( unsigned int k = 0; k < scNM.Count(); k++ ) {
					const ScatteredRay& s = scNM[k];
					const Vector3 d = s.ray.Dir();
					worstNM = std::max( worstNM, std::fabs( s.krayNM - brdf->valueNM( d, ri, 600.0 ) * std::fabs( d.z ) / s.pdf ) );
				}
			}
		}
		std::printf( "  %d rays, worst |kray - f cos/pdf| RGB %.2e NM %.2e\n", n, worst, worstNM );
		Check( n > 1000, "sampler emitted rays" );
		Check( backRays > 500, "sampler emitted rays from back-face records" );
		Check( weightless == 0, "every sampled ray carries weight" );
		Check( worst < 1e-12, "EON RGB weight is f cos / pdf" );
		Check( worstNM < 1e-12, "EON NM weight is f cos / pdf" );
	}

	void TestEONHemispherical()
	{
		std::printf( "-- F. EON hemisphericalAlbedo --\n" );
		for( double rho : { 0.3, 0.8, 1.0 } ) for( double sigma : { 0.2, 0.7, 1.0 } ) {
			Owned<UniformColorPainter> c( new UniformColorPainter( RISEPel( rho, rho, rho ) ) );
			Owned<UniformScalarPainter> r( new UniformScalarPainter( sigma ) );
			Owned<OrenNayarBRDF> eon( new OrenNayarBRDF( *c, *r, true ) );
			double quad = 0;
			const int nMu = 64;
			for( int k = 0; k < nMu; k++ ) {
				const double muO = ( k + 0.5 ) / nMu;
				quad += 2.0 * DirectionalAlbedo( *eon, Vector3( std::sqrt( 1 - muO * muO ), 0, muO ), 0, 200, 128 ) * muO / nMu;
			}
			RISEPel h;
			eon->hemisphericalAlbedo( MakeRIForView( Vector3( 0, 0, 1 ) ), h );
			char buf[128];
			std::snprintf( buf, sizeof buf, "hemisphericalAlbedo rho=%.1f sigma=%.1f", rho, sigma );
			CheckClose( h[0], quad, 1.5e-3, buf );
		}
	}

	//////////////////////////////////////////////////////////////////
	// GGX constant
	//////////////////////////////////////////////////////////////////

	struct GGXSet
	{
		Owned<UniformColorPainter> rd, rs;
		Owned<UniformScalarPainter> alpha, ior, ext;
		Owned<GGXBRDF> brdf;
		Owned<GGXSPF> spf;
		GGXSet( double diffuse, double spec, double a, FresnelMode mode ) :
			rd( new UniformColorPainter( RISEPel( diffuse, diffuse, diffuse ) ) ),
			rs( new UniformColorPainter( RISEPel( spec, spec, spec ) ) ),
			alpha( new UniformScalarPainter( a ) ),
			ior( new UniformScalarPainter( 2.5 ) ),
			ext( new UniformScalarPainter( 3.0 ) ),
			brdf( new GGXBRDF( *rd, *rs, *alpha, *alpha, *ior, *ext, mode ) ),
			spf( new GGXSPF( *rd, *rs, *alpha, *alpha, *ior, *ext, mode ) ) {}
	};

	void TestGGXConstantEqualsSchlickOne()
	{
		std::printf( "-- G. GGX constant at colour 1 equals schlick_f0 F0 = 1 --\n" );
		double worst = 0, worstH = 0;
		for( double a : { 0.05, 0.3, 0.8 } ) {
			GGXSet c( 0.3, 1.0, a, eFresnelConstant ), s( 0.3, 1.0, a, eFresnelSchlickF0 );
			for( double tv : { 0.0, 40.0, 75.0, 88.0 } ) for( double tl : { 10.0, 50.0, 85.0 } ) for( double dp : { 0.0, 90.0, 180.0 } ) {
				const RayIntersectionGeometric ri = MakeRIForView( Dir( tv, 0 ) );
				const Vector3 l = Dir( tl, dp );
				const RISEPel fc = c.brdf->value( l, ri ), fs = s.brdf->value( l, ri );
				worst = std::max( worst, std::fabs( fc[0] - fs[0] ) / std::max( 1e-12, fs[0] ) );
			}
			RISEPel hc, hs;
			c.brdf->hemisphericalAlbedo( MakeRIForView( Vector3( 0, 0, 1 ) ), hc );
			s.brdf->hemisphericalAlbedo( MakeRIForView( Vector3( 0, 0, 1 ) ), hs );
			worstH = std::max( worstH, std::fabs( hc[0] - hs[0] ) );
		}
		Check( worst < 1e-12, "value: constant(1) == schlick_f0(1)" );
		Check( worstH < 1e-12, "hemisphericalAlbedo: constant(1) == schlick_f0(1)" );
	}

	void TestGGXConstantAngleIndependence()
	{
		std::printf( "-- H. GGX constant single scatter is colour times the white lobe --\n" );
		const double tint = 0.4;
		GGXSet c( 0.0, tint, 0.3, eFresnelConstant ), w( 0.0, 1.0, 0.3, eFresnelConstant ), s( 0.0, tint, 0.3, eFresnelSchlickF0 );
		const IORStack stack = MakeTestIORStack( g_stubObject );
		double worst = 0, schlickGrazing = 0;
		for( double tv : { 0.0, 45.0, 80.0, 87.0 } ) for( double tl : { 0.0, 45.0, 80.0, 87.0 } ) {
			const RayIntersectionGeometric ri = MakeRIForView( Dir( tv, 0 ) );
			const Vector3 l = Dir( tl, 180 );
			RISEPel oc[5], ow[5], os[5];
			c.brdf->valueByScatterType( l, ri, &stack, oc );
			w.brdf->valueByScatterType( l, ri, &stack, ow );
			s.brdf->valueByScatterType( l, ri, &stack, os );
			const double ref = ow[ScatteredRay::eRayReflection][0];
			if( ref <= 1e-12 ) continue;
			worst = std::max( worst, std::fabs( oc[ScatteredRay::eRayReflection][0] / ref - tint ) );
			if( tv >= 80 && tl >= 80 ) schlickGrazing = std::max( schlickGrazing, os[ScatteredRay::eRayReflection][0] / ref );
		}
		std::printf( "  constant ratio worst |r - %.2f| = %.2e;  schlick_f0 grazing ratio %.3f\n", tint, worst, schlickGrazing );
		Check( worst < 1e-12, "constant: reflection lobe is colour x white lobe at every angle" );
		Check( schlickGrazing > tint * 1.2, "control: schlick_f0 at the same colour rises at grazing" );
	}

	void TestGGXConstantFurnaceAndSampler()
	{
		std::printf( "-- I. GGX constant furnace and sampler/evaluator agreement --\n" );
		const IORStack stack = MakeTestIORStack( g_stubObject );
		const int N = 200000;
		for( double a : { 0.1, 0.3, 0.6, 1.0 } ) for( double deg : { 0.0, 45.0, 75.0 } ) {
			for( int variant = 0; variant < 3; variant++ ) {
				// variant 0: white spec, black diffuse (furnace = 1).
				// variant 1: tint 0.5 over a 0.3 diffuse (sampler == evaluator).
				// variant 2: tint 0.5, black diffuse: rs Ess + F_ms(rs) (1 - Ess).
				const double rd = ( variant == 1 ) ? 0.3 : 0.0;
				const double rs = ( variant == 0 ) ? 1.0 : 0.5;
				GGXSet g( rd, rs, a, eFresnelConstant );
				const RayIntersectionGeometric ri = MakeRIForView( Dir( deg, 0 ) );
				RandomNumberGenerator rng;
				IndependentSampler sampler( rng );
				double sumK = 0, sumK2 = 0, sumF = 0;
				for( int i = 0; i < N; i++ ) {
					ScatteredRayContainer sc;
					g.spf->Scatter( ri, sampler, sc, stack );
					double k = 0, f = 0;
					for( unsigned int j = 0; j < sc.Count(); j++ ) {
						const ScatteredRay& s = sc[j];
						const Vector3 d = s.ray.Dir();
						k += s.kray[0];
						const double p = g.spf->Pdf( ri, d, stack );
						if( p > 0 ) f += g.brdf->value( d, ri )[0] * d.z / p;
					}
					sumK += k; sumK2 += k * k; sumF += f;
				}
				const double mean = sumK / N;
				const double se = std::sqrt( std::max( 0.0, sumK2 / N - mean * mean ) / N );
				const double meanF = sumF / N;
				char buf[160];
				if( variant == 0 ) {
					std::printf( "  alpha=%.1f theta=%2.0f  furnace %.4f (se %.1e)\n", a, deg, mean, se );
					std::snprintf( buf, sizeof buf, "furnace alpha=%.1f theta=%.0f: albedo %.4f", a, deg, mean );
					// Monte Carlo noise plus the multiscatter LUT's own residual
					// (the schlick_f0 F0 = 1 lobe is the same function, G above).
					// Measured 0.9944 .. 1.0018.
					CheckClose( mean, 1.0, std::max( 6.0 * se, 4e-3 ), buf );
				}
				if( variant == 2 ) {
					const double mu = std::cos( deg * kPi / 180.0 );
					const double ess = MicrofacetEnergyLUT::LookupEssG2( mu, a );
					const double eavg = MicrofacetEnergyLUT::LookupEavgG2( a );
					const double want = rs * ess + MicrofacetEnergyLUT::ComputeFms<Scalar>( rs, eavg ) * ( 1.0 - ess );
					std::snprintf( buf, sizeof buf, "tinted albedo alpha=%.1f theta=%.0f: %.4f vs rs Ess + Fms (1-Ess) %.4f",
						a, deg, mean, want );
					CheckClose( mean, want, std::max( 6.0 * se, 4e-3 ), buf );
				}
				std::snprintf( buf, sizeof buf, "sampler == evaluator alpha=%.1f theta=%.0f variant %d (%.4f vs %.4f, se %.1e)",
					a, deg, variant, mean, meanF, se );
				CheckClose( mean, meanF, std::max( 6.0 * se, 2e-3 ), buf );
			}
		}
	}

	void TestGGXConstantDiffuseAndSampler()
	{
		std::printf( "-- I2. GGX constant diffuse (1 - rs)^2 and sampler identity --\n" );
		// The diffuse lobe's interface transmission is flat: value with a
		// white diffuse minus value without it is (1 - rs)^2 / pi everywhere.
		const double rs = 0.5;
		GGXSet withD( 1.0, rs, 0.3, eFresnelConstant ), noD( 0.0, rs, 0.3, eFresnelConstant );
		double worst = 0;
		for( double tv : { 0.0, 45.0, 80.0, 88.0 } ) for( double tl : { 5.0, 50.0, 85.0 } ) {
			const RayIntersectionGeometric ri = MakeRIForView( Dir( tv, 0 ) );
			const Vector3 l = Dir( tl, 120 );
			const double d = withD.brdf->value( l, ri )[0] - noD.brdf->value( l, ri )[0];
			worst = std::max( worst, std::fabs( d - ( 1 - rs ) * ( 1 - rs ) / kPi ) );
		}
		Check( worst < 1e-12, "constant: the diffuse lobe is rd (1 - rs)^2 / pi at every angle" );

		// At colour 1 the sampler draws exactly what schlick_f0 F0 = 1 draws.
		GGXSet c( 0.3, 1.0, 0.4, eFresnelConstant ), s( 0.3, 1.0, 0.4, eFresnelSchlickF0 );
		const IORStack stack = MakeTestIORStack( g_stubObject );
		const RayIntersectionGeometric ri = MakeRIForView( Dir( 55, 0 ) );
		RandomNumberGenerator rngC( 12345u ), rngS( 12345u );
		IndependentSampler sc( rngC ), ss( rngS );
		int mismatch = 0;
		for( int i = 0; i < 20000; i++ ) {
			ScatteredRayContainer a, b;
			c.spf->Scatter( ri, sc, a, stack );
			s.spf->Scatter( ri, ss, b, stack );
			if( a.Count() != b.Count() ) { ++mismatch; continue; }
			for( unsigned int k = 0; k < a.Count(); k++ ) {
				if( !( a[k].kray[0] == b[k].kray[0] ) || !( a[k].pdf == b[k].pdf ) ||
					!( a[k].ray.Dir().x == b[k].ray.Dir().x ) ) ++mismatch;
			}
		}
		Check( mismatch == 0, "constant(1) samples bit-for-bit what schlick_f0(1) samples" );

		// A colour authored far above 1 in one channel: its multiscatter
		// denominator 1 - rs (1 - Eavg) is negative, and before ComputeFms
		// guarded each channel the other channels' positive denominators let
		// that channel's F_ms through negative.
		Owned<UniformColorPainter> rd0( new UniformColorPainter( RISEPel( 0, 0, 0 ) ) );
		Owned<UniformColorPainter> hot( new UniformColorPainter( RISEPel( 6.0, 0.2, 0.2 ) ) );
		Owned<UniformScalarPainter> a1( new UniformScalarPainter( 1.0 ) );
		Owned<UniformScalarPainter> ior( new UniformScalarPainter( 1.5 ) ), ext( new UniformScalarPainter( 0.0 ) );
		Owned<GGXBRDF> hotBrdf( new GGXBRDF( *rd0, *hot, *a1, *a1, *ior, *ext, eFresnelConstant ) );
		double minChannel = 1e30;
		for( double tv : { 0.0, 60.0, 85.0 } ) for( double tl : { 10.0, 60.0, 85.0 } ) {
			const RISEPel f = hotBrdf->value( Dir( tl, 150 ), MakeRIForView( Dir( tv, 0 ) ) );
			for( int k = 0; k < 3; k++ ) minChannel = std::min( minChannel, (double)f[k] );
		}
		std::printf( "  over-bright colour: smallest channel value %.4g\n", minChannel );
		Check( minChannel >= 0, "an over-bright colour never makes a channel negative" );
	}

	void TestGGXConstantNM()
	{
		std::printf( "-- J. GGX constant RGB/NM --\n" );
		GGXSet g( 0.0, 1.0, 0.35, eFresnelConstant );
		double worst = 0;
		for( double tv : { 0.0, 50.0, 80.0 } ) for( double tl : { 20.0, 60.0 } ) {
			const RayIntersectionGeometric ri = MakeRIForView( Dir( tv, 0 ) );
			const Vector3 l = Dir( tl, 160 );
			worst = std::max( worst, std::fabs( g.brdf->valueNM( l, ri, 500.0 ) - g.brdf->value( l, ri )[0] ) );
		}
		Check( worst < 1e-9, "GGX constant valueNM equals value for a white colour" );
	}

	//////////////////////////////////////////////////////////////////
	// Scene language
	//////////////////////////////////////////////////////////////////

	Job* LoadScene( const std::string& body, const char* tag )
	{
		std::string dir = RISE::TestSupport::ScratchBaseDir();
		if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += '/';
		const std::string path = dir + "rise_bladd_" + tag + ".RISEscene";
		{ std::ofstream o( path.c_str(), std::ios::binary | std::ios::trunc ); o << body; }
		Job* job = new Job();
		const bool ok = job->LoadAsciiSceneViaCst( path.c_str() );
		std::remove( path.c_str() );
		if( !ok ) { job->release(); return nullptr; }
		return job;
	}

	void TestCoatedOverConstant()
	{
		std::printf( "-- L. coated_material over a constant-mode GGX base --\n" );
		const std::string s =
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\n\tname\twhite\n\tcolor\t1 1 1\n}\n"
			"uniformcolor_painter\n{\n\tname\thalf\n\tcolor\t0.5 0.5 0.5\n}\n"
			"ggx_material\n{\n\tname\tbase\n\trd\twhite\n\trs\thalf\n\talphax\t0.3\n\talphay\t0.3\n\tfresnel_mode\tconstant\n}\n"
			"coated_material\n{\n\tname\tcoated\n\tbase\tbase\n\tcoat_ior\t1.0\n\tcoat_roughness\t0.02\n}\n";
		Job* job = LoadScene( s, "coated" );
		Check( job != nullptr, "coated scene loads" );
		if( !job ) return;
		const IMaterial* base = job->GetMaterials()->GetItem( "base" );
		const IMaterial* coated = job->GetMaterials()->GetItem( "coated" );
		Check( base && coated && base->GetBSDF() && coated->GetBSDF(), "materials have BSDFs" );
		if( base && coated && base->GetBSDF() && coated->GetBSDF() ) {
			const RayIntersectionGeometric ri = MakeRIForView( Vector3( 0, 0, 1 ) );
			RISEPel hb, hc;
			base->GetBSDF()->hemisphericalAlbedo( ri, hb );
			coated->GetBSDF()->hemisphericalAlbedo( ri, hc );
			std::printf( "  base white-sky albedo %.5f, through an index-1 coat %.5f\n", hb[0], hc[0] );
			// The lobe reservoir (0.70853) and the base's bihemispherical table
			// (0.70856) agree to 3e-5 here.  Without the flat constant-mode
			// shape (Schlick rise plus Schlick-shaped diffuse) the reservoir
			// reads 0.69312, a measured 0.0154 gap, so 1e-3 separates the two.
			CheckClose( hc[0], hb[0], 1e-3, "coated (coat index 1) white-sky albedo equals the constant base's" );
		}
		job->release();
	}

	//! Coat index 1.5: a constant-mode base with rs = 0 is a Lambertian
	//! (rd/pi, no specular, (1 - 0)^2 interface), so its coated white-sky
	//! albedo through the GGX lobe reservoir (the flat J1 integrals) must
	//! match the coated Lambertian, which coated_material evaluates by its
	//! own interior-diffuse model.  The Schlick-shaped diffuse (sA) does not:
	//! without the flat shape the reservoir reads 0.50044 against 0.53229.
	void TestCoatedOverConstantRefracting()
	{
		std::printf( "-- L2. coated constant base vs coated Lambertian, coat index 1.5 --\n" );
		const std::string s =
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\n\tname\tgrey\n\tcolor\t0.7 0.7 0.7\n}\n"
			"uniformcolor_painter\n{\n\tname\tblack\n\tcolor\t0 0 0\n}\n"
			"ggx_material\n{\n\tname\tbase\n\trd\tgrey\n\trs\tblack\n\talphax\t0.3\n\talphay\t0.3\n\tfresnel_mode\tconstant\n}\n"
			"lambertian_material\n{\n\tname\tlamb\n\treflectance\tgrey\n}\n"
			"coated_material\n{\n\tname\tcg\n\tbase\tbase\n\tcoat_ior\t1.5\n\tcoat_roughness\t0.02\n}\n"
			"coated_material\n{\n\tname\tcl\n\tbase\tlamb\n\tcoat_ior\t1.5\n\tcoat_roughness\t0.02\n}\n"
			"coated_material\n{\n\tname\tcga\n\tbase\tbase\n\tcoat_ior\t1.5\n\tcoat_roughness\t0.02\n\tcoat_thickness\t0.5\n\tcoat_absorption\t0.3\n}\n"
			"coated_material\n{\n\tname\tcla\n\tbase\tlamb\n\tcoat_ior\t1.5\n\tcoat_roughness\t0.02\n\tcoat_thickness\t0.5\n\tcoat_absorption\t0.3\n}\n";
		Job* job = LoadScene( s, "coated15" );
		Check( job != nullptr, "coated index-1.5 scene loads" );
		if( !job ) return;
		const IMaterial* cg = job->GetMaterials()->GetItem( "cg" );
		const IMaterial* cl = job->GetMaterials()->GetItem( "cl" );
		if( cg && cl && cg->GetBSDF() && cl->GetBSDF() ) {
			const RayIntersectionGeometric ri = MakeRIForView( Vector3( 0, 0, 1 ) );
			RISEPel hg, hl;
			cg->GetBSDF()->hemisphericalAlbedo( ri, hg );
			cl->GetBSDF()->hemisphericalAlbedo( ri, hl );
			std::printf( "  coated constant(rs 0) %.5f vs coated Lambertian %.5f\n", hg[0], hl[0] );
			CheckClose( hg[0], hl[0], 2e-3, "coated constant base with rs 0 matches the coated Lambertian" );
		} else {
			Check( false, "coated index-1.5 materials have BSDFs" );
		}
		// Directional: value, valueNM and the albedo guide through an
		// absorbing coat (LobeReturn / LobeEscape's flat branches, the NM
		// reader and the absorbing-coat basis blend that carries J1).
		const IMaterial* cga = job->GetMaterials()->GetItem( "cga" );
		const IMaterial* cla = job->GetMaterials()->GetItem( "cla" );
		if( cga && cla && cga->GetBSDF() && cla->GetBSDF() ) {
			double worst = 0;
			for( double tv : { 0.0, 30.0, 60.0, 80.0 } ) {
				const RayIntersectionGeometric ri = MakeRIForView( Dir( tv, 0 ) );
				for( double tl : { 5.0, 25.0, 45.0, 65.0 } ) for( double dp : { 0.0, 180.0 } ) {
					const Vector3 l = Dir( tl, dp );
					const double a = cga->GetBSDF()->value( l, ri )[0], b = cla->GetBSDF()->value( l, ri )[0];
					const double an = cga->GetBSDF()->valueNM( l, ri, 550.0 ), bn = cla->GetBSDF()->valueNM( l, ri, 550.0 );
					worst = std::max( worst, std::fabs( a - b ) / std::max( 1e-12, b ) );
					worst = std::max( worst, std::fabs( an - bn ) / std::max( 1e-12, bn ) );
				}
				const double ag = cga->GetBSDF()->albedo( ri )[0], al = cla->GetBSDF()->albedo( ri )[0];
				worst = std::max( worst, std::fabs( ag - al ) / std::max( 1e-12, al ) );
			}
			std::printf( "  absorbing coat: worst relative |constant(rs 0) - Lambertian| %.2e\n", worst );
			// Measured 4.3e-5: the lobe reservoir's quadratures against the
			// Lambertian model's closed-form interior diffuse.
			Check( worst < 2e-4, "coated constant(rs 0) equals coated Lambertian in value, valueNM and albedo (absorbing coat)" );
		} else {
			Check( false, "absorbing-coat materials have BSDFs" );
		}
		job->release();
	}

	//! The editor's live paths: a CST edit of `model` / `fresnel_mode`
	//! re-creates the material with the new option, a roughness edit keeps
	//! EON, and the snapshot clone carries both.
	void TestLiveEditsAndClone()
	{
		std::printf( "-- M. live edits and snapshot clones --\n" );
		const std::string s =
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\n\tname\tgrey\n\tcolor\t0.6 0.6 0.6\n}\n"
			"orennayar_material\n{\n\tname\ton\n\treflectance\tgrey\n\troughness\t0.5\n}\n"
			"ggx_material\n{\n\tname\tgg\n\trd\tnone\n\trs\tgrey\n}\n";
		Job* job = LoadScene( s, "edit" );
		Check( job != nullptr, "edit scene loads" );
		if( !job ) return;
		auto eon = [&]() {
			const OrenNayarMaterial* o = dynamic_cast<const OrenNayarMaterial*>( job->GetMaterials()->GetItem( "on" ) );
			return o && o->IsEnergyPreserving();
		};
		auto mode = [&]() {
			const GGXMaterial* g = dynamic_cast<const GGXMaterial*>( job->GetMaterials()->GetItem( "gg" ) );
			return g ? (int)g->GetFresnelMode() : -1;
		};
		Check( !eon(), "edit scene starts classic" );
		job->ApplyCstParamEditChecked( "on", "material", "model", 0, "energy_preserving" );
		Check( eon(), "editing model to energy_preserving re-creates the material as EON" );
		job->ApplyCstParamEditChecked( "on", "material", "roughness", 0, "0.8" );
		Check( eon(), "editing roughness keeps EON" );
		job->ApplyCstParamEditChecked( "gg", "material", "fresnel_mode", 0, "constant" );
		Check( mode() == (int)eFresnelConstant, "editing fresnel_mode to constant takes effect" );

		const IMaterial* oc = CloneMaterialForSnapshot( job->GetMaterials()->GetItem( "on" ) );
		const OrenNayarMaterial* ocn = dynamic_cast<const OrenNayarMaterial*>( oc );
		Check( ocn && ocn->IsEnergyPreserving(), "snapshot clone keeps EON" );
		const IMaterial* gc = CloneMaterialForSnapshot( job->GetMaterials()->GetItem( "gg" ) );
		const GGXMaterial* gcn = dynamic_cast<const GGXMaterial*>( gc );
		Check( gcn && gcn->GetFresnelMode() == eFresnelConstant, "snapshot clone keeps constant mode" );
		safe_release( oc );
		safe_release( gc );
		job->release();
	}

	void TestSceneLanguage()
	{
		std::printf( "-- K. scene language --\n" );
		const std::string pre =
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\n\tname\tgrey\n\tcolor\t0.6 0.6 0.6\n}\n";
		{
			const std::string s = pre +
				"ggx_material\n{\n\tname\tglossy\n\trd\tnone\n\trs\tgrey\n\talphax\t0.2\n\talphay\t0.2\n\tfresnel_mode\tconstant\n}\n"
				"ggx_material\n{\n\tname\tdefault_ggx\n\trd\tnone\n\trs\tgrey\n}\n"
				"orennayar_material\n{\n\tname\teon\n\treflectance\tgrey\n\troughness\t0.5\n\tmodel\tenergy_preserving\n}\n"
				"orennayar_material\n{\n\tname\tclassic\n\treflectance\tgrey\n\troughness\t0.5\n}\n";
			Job* job = LoadScene( s, "ok" );
			Check( job != nullptr, "scene with the new options loads" );
			if( job ) {
				const GGXMaterial* g = dynamic_cast<const GGXMaterial*>( job->GetMaterials()->GetItem( "glossy" ) );
				Check( g && g->GetFresnelMode() == eFresnelConstant, "fresnel_mode constant parses to eFresnelConstant" );
				const GGXMaterial* d = dynamic_cast<const GGXMaterial*>( job->GetMaterials()->GetItem( "default_ggx" ) );
				Check( d && d->GetFresnelMode() == eFresnelConductor, "ggx default stays conductor" );
				const OrenNayarMaterial* e = dynamic_cast<const OrenNayarMaterial*>( job->GetMaterials()->GetItem( "eon" ) );
				Check( e && e->IsEnergyPreserving(), "model energy_preserving parses to EON" );
				const OrenNayarMaterial* c = dynamic_cast<const OrenNayarMaterial*>( job->GetMaterials()->GetItem( "classic" ) );
				Check( c && !c->IsEnergyPreserving(), "orennayar default stays classic" );
				job->release();
			}
		}
		{
			const std::string s = pre +
				"orennayar_material\n{\n\tname\tbad\n\treflectance\tgrey\n\troughness\t0.5\n\tmodel\tfujii\n}\n";
			Job* job = LoadScene( s, "bad" );
			Check( job == nullptr, "an unknown Oren-Nayar model is refused" );
			if( job ) job->release();
		}
	}
}

int main()
{
	g_stubObject = new StubObject();

	TestEONFurnace();
	TestEONClosedForms();
	TestEONCyclesParity();
	TestEONSymmetryAndTwins();
	TestEONSampler();
	TestEONHemispherical();
	TestGGXConstantEqualsSchlickOne();
	TestGGXConstantAngleIndependence();
	TestGGXConstantFurnaceAndSampler();
	TestGGXConstantDiffuseAndSampler();
	TestGGXConstantNM();
	TestCoatedOverConstant();
	TestCoatedOverConstantRefracting();
	TestLiveEditsAndClone();
	TestSceneLanguage();

	safe_release( g_stubObject );

	std::printf( "\nBlenderLobeAdditionsTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
