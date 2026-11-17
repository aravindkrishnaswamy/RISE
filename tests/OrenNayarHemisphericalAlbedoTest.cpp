//////////////////////////////////////////////////////////////////////
//
//  OrenNayarHemisphericalAlbedoTest.cpp - Standalone regression test
//    for OrenNayarBRDF::hemisphericalAlbedo{,NM} (debt ledger DL-07,
//    docs/DEBT_LEDGER.md; CLOTH_FABRIC_DESIGN.md section 15 item 17 /
//    WETNESS_COAT_DESIGN.md section 12 item 13).
//
//  Coverage:
//    (a) TestDirectionalHemisphericalReproduction -- numerically
//        integrates the REAL OrenNayarBRDF::value() over the outgoing
//        hemisphere, cosine-weighted, at fixed incidence angles
//        {0,30,60,80} deg and roughness {0,0.2,0.3,0.5,1.0}, exactly
//        the DL-07 recipe's item (1).  This is the DIRECTIONAL-
//        hemispherical reflectance R(theta_i); at rho=1 the pre-fix
//        `hemisphericalAlbedo` (which returns rho verbatim,
//        independent of theta_i) over-estimates it by the margins the
//        ledger names.  Also serves as an independent check on the
//        OrenNayarBRDF.cpp header comment's own quoted table -- see
//        the printed deltas if that comment is ever revised again.
//    (b) TestBihemispherical -- the quantity `hemisphericalAlbedo`
//        actually promises (IBSDF.h: "reflectance under a uniform
//        incident field", `ri.ray` must not be read, so the contract
//        is BIHEMISPHERICAL, not directional-at-a-fixed-incidence).
//        Computes the true bihemispherical albedo by numerically
//        double-integrating the REAL OrenNayarBRDF::value() over BOTH
//        hemispheres (incident and outgoing), at a resolution and
//        loop structure independent of
//        tools/OrenNayarHemisphericalAlbedoGen.cpp's own bake, and
//        compares against `hemisphericalAlbedo()`'s return.  RED
//        pre-fix (current code returns rho unconditionally); GREEN
//        post-fix to within a stated tolerance.
//    (c) TestValueNMConsistency -- same bihemispherical check on the
//        single-wavelength `valueNM`/`hemisphericalAlbedoNM` pair.
//    (d) TestEdgeCases -- sigma=0 exact Lambertian identity,
//        monotonic decrease of the returned albedo with roughness,
//        table-domain clamp beyond kSigmaMax stays finite and bounded.
//    (e) TestPerChannelRoughness -- a roughness painter that returns a
//        DIFFERENT sigma per RGB channel (OrenNayarBRDF::value reads
//        the full ScalarTriple, not just v[0] -- see IScalarPainter.h
//        vs the actual template instantiation) is honoured per
//        channel by hemisphericalAlbedo, not collapsed to one sigma.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#ifdef NDEBUG
#undef NDEBUG	// This standalone test relies on assert in Release builds too.
#endif
#include <cassert>
#include <cmath>
#include <cstdio>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/math_utils.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/Color/ColorMath.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/IBSDF.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Materials/OrenNayarBRDF.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	static const double PI = 3.14159265358979323846;

	//! Every painter / BSDF in this codebase is reference-counted with a
	//! PROTECTED destructor (see e.g. UniformColorPainter.h) -- they
	//! cannot be stack-allocated.  Minimal RAII wrapper matching
	//! tests/SPFBSDFConsistencyTest.cpp's `new T(...); t->addref();`
	//! pattern, released automatically at scope exit via safe_release.
	template< typename T >
	class Owned
	{
	public:
		explicit Owned( T* ptr ) : p( ptr ) { p->addref(); }
		~Owned() { safe_release( p ); }
		Owned( const Owned& ) = delete;
		Owned& operator=( const Owned& ) = delete;
		T& operator*() const { return *p; }
		T* operator->() const { return p; }
		T* get() const { return p; }
	private:
		T* p;
	};

	int g_numChecks = 0;
	int g_numFailures = 0;

	void Check( bool cond, const std::string& msg )
	{
		g_numChecks++;
		if( !cond ) {
			g_numFailures++;
			std::cerr << "FAILED: " << msg << std::endl;
		}
	}

	void CheckClose( double a, double b, double tol, const std::string& msg )
	{
		g_numChecks++;
		if( fabs( a - b ) > tol ) {
			g_numFailures++;
			std::cerr << "FAILED: " << msg << "  (got " << a << ", expected " << b
			          << ", |diff|=" << fabs(a-b) << " > tol=" << tol << ")" << std::endl;
		}
	}

	//! Build a RayIntersectionGeometric whose shading normal is +Z and
	//! whose incoming ray direction is -wo (so the "view" direction
	//! r = -ri.ray.Dir() the BRDF reads is wo).  Mirrors
	//! tests/SPFBSDFConsistencyTest.cpp's MakeIntersection /
	//! MakeIntersectionFromView pattern.
	RayIntersectionGeometric MakeRIForView( const Vector3& wo )
	{
		Vector3 inDir = Vector3Ops::Normalize( wo * -1.0 );
		Ray inRay( Point3( -inDir.x, -inDir.y, -inDir.z ), inDir );
		RasterizerState rs = {0, 0};
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

	Vector3 HemisphereDir( double mu, double phi )
	{
		const double s = sqrt( r_max( 0.0, 1.0 - mu*mu ) );
		return Vector3( s * cos(phi), s * sin(phi), mu );
	}

	//////////////////////////////////////////////////////////////////
	//  (a) Directional-hemispherical reproduction, via the REAL BRDF.
	//////////////////////////////////////////////////////////////////

	//! R(theta_i)/rho at rho=1 -- integrate brdf.value(wi, ri(wo)) *
	//! cos(theta_o) over the outgoing hemisphere, fixed incident wi.
	double DirectionalHemispherical( IBSDF& brdf, double thetaI, int Nmu, int Nphi )
	{
		const Vector3 wi( sin(thetaI), 0, cos(thetaI) );
		double sum = 0;
		for( int i = 0; i < Nmu; i++ ) {
			const double muO = ( i + 0.5 ) / Nmu;
			for( int j = 0; j < Nphi; j++ ) {
				const double phi = 2.0*PI*( j + 0.5 ) / Nphi;
				const Vector3 wo = HemisphereDir( muO, phi );
				RayIntersectionGeometric ri = MakeRIForView( wo );
				const RISEPel f = brdf.value( wi, ri );
				sum += ColorMath::MaxValue( f ) * muO;
			}
		}
		const double dmu = 1.0 / Nmu;
		const double dphi = 2.0*PI / Nphi;
		return sum * dmu * dphi;
	}

	void TestDirectionalHemisphericalReproduction()
	{
		std::cout << "--- TestDirectionalHemisphericalReproduction ---" << std::endl;

		Owned<UniformColorPainter> white( new UniformColorPainter( RISEPel( 1, 1, 1 ) ) );

		struct Row { double sigma; double deg; };
		// Cross-referenced against OrenNayarBRDF.cpp's own header-comment
		// table (measured 2026-09 pre-fix).  Printed, not hard-asserted
		// against that table's exact figures -- see the printed deltas;
		// this test's OWN independent quadrature is the ground truth it
		// gates on below, in TestBihemispherical.
		Row rows[] = {
			{ 0.00,  0 }, { 0.00, 30 }, { 0.00, 60 }, { 0.00, 80 },
			{ 0.20,  0 }, { 0.20, 30 }, { 0.20, 60 }, { 0.20, 80 },
			{ 0.30,  0 }, { 0.30, 30 }, { 0.30, 60 }, { 0.30, 80 },
			{ 0.50,  0 }, { 0.50, 30 }, { 0.50, 60 }, { 0.50, 80 },
			{ 1.00,  0 }, { 1.00, 30 }, { 1.00, 60 }, { 1.00, 80 },
		};

		for( const Row& row : rows ) {
			Owned<UniformScalarPainter> roughness( new UniformScalarPainter( row.sigma ) );
			Owned<OrenNayarBRDF> brdf( new OrenNayarBRDF( *white, *roughness ) );
			const double thetaI = row.deg * PI / 180.0;
			const double R = DirectionalHemispherical( *brdf, thetaI, 60, 120 );

			RayIntersectionGeometric dummyRi = MakeRIForView( Vector3( 0, 0, 1 ) );
			RISEPel out;
			brdf->hemisphericalAlbedo( dummyRi, out );
			const double reported = ColorMath::MaxValue( out );

			printf( "  sigma=%.2f theta_i=%3.0fdeg: R_true=%.4f  hemisphericalAlbedo()=%.4f  overestimate=%.4f\n",
				row.sigma, row.deg, R, reported, reported - R );

			// Sanity bounds: R must be a plausible reflectance for a
			// unit-white substrate (loosely bounded -- this model is
			// not tightly energy-conserving, see file header).
			Check( R > 0.0 && R < 1.5, "directional-hemispherical R in a plausible range" );
		}
	}

	//////////////////////////////////////////////////////////////////
	//  (b) True bihemispherical albedo via the REAL BRDF, independent
	//      loop structure/resolution from the generator's own bake.
	//////////////////////////////////////////////////////////////////

	//! (A1,A2) such that R_bi = rho*A1 + rho^2*A2, computed by directly
	//! double-integrating brdf.value() with rho=1 (so A1+A2 = R_bi at
	//! rho=1).  Structured as: for each incident mu_i (light, phi
	//! fixed at 0 WLOG -- isotropy is exact for this BRDF, built only
	//! from dot products with the shared normal, so fixing phi_i loses
	//! no generality; see the derivation in
	//! tools/OrenNayarHemisphericalAlbedoGen.cpp's header for why this
	//! reduction is exact, not an approximation), loop the FULL
	//  outgoing hemisphere (mu_o, phi_o over the whole circle) through
	//! the real ri/BRDF call.
	double BihemisphericalAtRhoOne( IBSDF& brdf, int Nmu, int Nphi )
	{
		double sum = 0;
		for( int i = 0; i < Nmu; i++ ) {
			const double muI = ( i + 0.5 ) / Nmu;
			const double sinI = sqrt( r_max( 0.0, 1.0 - muI*muI ) );
			const Vector3 wi( sinI, 0, muI ); // phi_i = 0
			for( int k = 0; k < Nmu; k++ ) {
				const double muO = ( k + 0.5 ) / Nmu;
				for( int j = 0; j < Nphi; j++ ) {
					const double phi = 2.0*PI*( j + 0.5 ) / Nphi;
					const Vector3 wo = HemisphereDir( muO, phi );
					RayIntersectionGeometric ri = MakeRIForView( wo );
					const RISEPel f = brdf.value( wi, ri );
					sum += ColorMath::MaxValue( f ) * muI * muO;
				}
			}
		}
		const double dmu = 1.0 / Nmu;
		const double dphi = 2.0*PI / Nphi;
		// R_bi = (1/pi) INT R(wi) (n.wi) dwi = (1/pi) INT INT f cos_i cos_o dwi dwo
		//      = (1/pi) * [sum * dmuI*dmuO*dphi_o] * (2*pi from the
		//        redundant phi_i integral, since f depends only on
		//        deltaPhi and we fixed phi_i=0)
		return sum * dmu * dmu * dphi * 2.0; // (1/pi)*2*pi = 2
	}

	void TestBihemispherical()
	{
		std::cout << "--- TestBihemispherical (rho=1) ---" << std::endl;
		Owned<UniformColorPainter> white( new UniformColorPainter( RISEPel( 1, 1, 1 ) ) );

		// Post-fix tolerance: the bake (Nmu=256,Nphi=512, generator)
		// interpolates its own converged integral to within 2.3e-4
		// absolute (see the generator's header); this test's OWN
		// independent quadrature (Nmu=60,Nphi=120, real BRDF calls,
		// different loop resolution) should agree with the bake to
		// within quadrature noise at that resolution. 0.01 absolute
		// (1% of a rho=1 substrate) covers both with margin.
		const double kTol = 0.01;

		double sigmas[] = { 0.0, 0.1, 0.2, 0.3, 0.5, 0.75, 1.0, 1.5, 2.0 };
		for( double sigma : sigmas ) {
			Owned<UniformScalarPainter> roughness( new UniformScalarPainter( sigma ) );
			Owned<OrenNayarBRDF> brdf( new OrenNayarBRDF( *white, *roughness ) );

			const double trueBi = BihemisphericalAtRhoOne( *brdf, 60, 120 );

			RayIntersectionGeometric dummyRi = MakeRIForView( Vector3( 0, 0, 1 ) );
			RISEPel out;
			brdf->hemisphericalAlbedo( dummyRi, out );
			const double reported = ColorMath::MaxValue( out );

			printf( "  sigma=%.2f: bihemispherical_true=%.5f  hemisphericalAlbedo()=%.5f  |diff|=%.5f\n",
				sigma, trueBi, reported, fabs(reported - trueBi) );

			CheckClose( reported, trueBi, kTol,
				"hemisphericalAlbedo() matches the true bihemispherical albedo at sigma=" + std::to_string(sigma) );
		}
	}

	//////////////////////////////////////////////////////////////////
	//  (c) valueNM / hemisphericalAlbedoNM consistency
	//////////////////////////////////////////////////////////////////

	double BihemisphericalAtRhoOneNM( IBSDF& brdf, double nm, int Nmu, int Nphi )
	{
		double sum = 0;
		for( int i = 0; i < Nmu; i++ ) {
			const double muI = ( i + 0.5 ) / Nmu;
			const double sinI = sqrt( r_max( 0.0, 1.0 - muI*muI ) );
			const Vector3 wi( sinI, 0, muI );
			for( int k = 0; k < Nmu; k++ ) {
				const double muO = ( k + 0.5 ) / Nmu;
				for( int j = 0; j < Nphi; j++ ) {
					const double phi = 2.0*PI*( j + 0.5 ) / Nphi;
					const Vector3 wo = HemisphereDir( muO, phi );
					RayIntersectionGeometric ri = MakeRIForView( wo );
					const double f = brdf.valueNM( wi, ri, nm );
					sum += f * muI * muO;
				}
			}
		}
		const double dmu = 1.0 / Nmu;
		const double dphi = 2.0*PI / Nphi;
		return sum * dmu * dmu * dphi * 2.0;
	}

	void TestValueNMConsistency()
	{
		std::cout << "--- TestValueNMConsistency ---" << std::endl;
		Owned<UniformColorPainter> white( new UniformColorPainter( RISEPel( 1, 1, 1 ) ) );
		const double kTol = 0.01;

		double sigmas[] = { 0.0, 0.3, 0.5, 1.0 };
		for( double sigma : sigmas ) {
			Owned<UniformScalarPainter> roughness( new UniformScalarPainter( sigma ) );
			Owned<OrenNayarBRDF> brdf( new OrenNayarBRDF( *white, *roughness ) );

			const double trueBi = BihemisphericalAtRhoOneNM( *brdf, 550.0, 60, 120 );

			RayIntersectionGeometric dummyRi = MakeRIForView( Vector3( 0, 0, 1 ) );
			Scalar out = 0;
			brdf->hemisphericalAlbedoNM( dummyRi, 550.0, out );

			printf( "  sigma=%.2f nm=550: bihemispherical_true=%.5f  hemisphericalAlbedoNM()=%.5f\n",
				sigma, trueBi, out );

			CheckClose( out, trueBi, kTol,
				"hemisphericalAlbedoNM() matches true bihemispherical at sigma=" + std::to_string(sigma) );
		}
	}

	//////////////////////////////////////////////////////////////////
	//  (d) Edge cases
	//////////////////////////////////////////////////////////////////

	void TestEdgeCases()
	{
		std::cout << "--- TestEdgeCases ---" << std::endl;
		Owned<UniformColorPainter> grey( new UniformColorPainter( RISEPel( 0.6, 0.6, 0.6 ) ) );
		RayIntersectionGeometric dummyRi = MakeRIForView( Vector3( 0, 0, 1 ) );

		// sigma=0 is exactly Lambertian: hemisphericalAlbedo == rho.
		{
			Owned<UniformScalarPainter> roughness( new UniformScalarPainter( 0.0 ) );
			Owned<OrenNayarBRDF> brdf( new OrenNayarBRDF( *grey, *roughness ) );
			RISEPel out;
			brdf->hemisphericalAlbedo( dummyRi, out );
			CheckClose( ColorMath::MaxValue( out ), 0.6, 1e-4, "sigma=0 hemisphericalAlbedo == rho" );
		}

		// Monotonic decrease with roughness (energy moves into
		// non-Lambertian shape, but the model still fits actual
		// measured retroreflective materials -- checked as a shape
		// property, not a physical necessity, since A2 partially
		// offsets A1's decline -- verified numerically decreasing over
		// the table's node range for this specific model).
		{
			double prev = 1.1; // > any legal albedo
			double sigmas[] = { 0.0, 0.2, 0.4, 0.6, 0.8, 1.0, 1.5, 2.0 };
			for( double s : sigmas ) {
				Owned<UniformScalarPainter> roughness( new UniformScalarPainter( s ) );
				Owned<OrenNayarBRDF> brdf( new OrenNayarBRDF( *grey, *roughness ) );
				RISEPel out;
				brdf->hemisphericalAlbedo( dummyRi, out );
				const double v = ColorMath::MaxValue( out );
				Check( v <= prev + 1e-6, "hemisphericalAlbedo is non-increasing in sigma at sigma=" + std::to_string(s) );
				prev = v;
			}
		}

		// Beyond the table's domain (kSigmaMax=3): must stay finite,
		// bounded in [0,1] for rho<1, and equal the sigma=3 value
		// (constant extrapolation -- the table's STATED domain, not a
		// silent clamp of convenience).
		{
			Owned<UniformScalarPainter> roughnessAt3( new UniformScalarPainter( 3.0 ) );
			Owned<UniformScalarPainter> roughnessAt100( new UniformScalarPainter( 100.0 ) );
			Owned<OrenNayarBRDF> brdfAt3( new OrenNayarBRDF( *grey, *roughnessAt3 ) );
			Owned<OrenNayarBRDF> brdfAt100( new OrenNayarBRDF( *grey, *roughnessAt100 ) );
			RISEPel out3, out100;
			brdfAt3->hemisphericalAlbedo( dummyRi, out3 );
			brdfAt100->hemisphericalAlbedo( dummyRi, out100 );
			CheckClose( ColorMath::MaxValue( out3 ), ColorMath::MaxValue( out100 ), 1e-5,
				"sigma>kSigmaMax clamps to the sigma=kSigmaMax value" );
			Check( ColorMath::MaxValue( out100 ) >= 0.0 && ColorMath::MaxValue( out100 ) <= 1.0,
				"clamped extrapolation stays in [0,1] for rho=0.6" );
		}
	}

	//////////////////////////////////////////////////////////////////
	//  (e) Per-channel roughness
	//////////////////////////////////////////////////////////////////

	//! OrenNayarBRDF::value reads the full ScalarTriple, not v[0]
	//! (`const ScalarTriple r = pRoughness->GetValuesAt(ri);` in
	//! OrenNayarBRDF.cpp), so hemisphericalAlbedo must honour a
	//! per-channel roughness too, using the built-in
	//! `RGBScalarPainter` -- or it would silently disagree with
	//! value()'s own per-channel roughness for a spectrally-varying
	//! roughness painter, the same class of bug this whole row is
	//! about, just checked on the OTHER input.
	void TestPerChannelRoughness()
	{
		std::cout << "--- TestPerChannelRoughness ---" << std::endl;
		Owned<UniformColorPainter> white( new UniformColorPainter( RISEPel( 1, 1, 1 ) ) );
		Owned<RGBScalarPainter> roughness( new RGBScalarPainter( 0.0, 0.5, 1.0 ) );
		Owned<OrenNayarBRDF> brdf( new OrenNayarBRDF( *white, *roughness ) );

		RayIntersectionGeometric dummyRi = MakeRIForView( Vector3( 0, 0, 1 ) );
		RISEPel out;
		brdf->hemisphericalAlbedo( dummyRi, out );

		printf( "  per-channel sigma={0,0.5,1.0}: hemisphericalAlbedo=(%.5f,%.5f,%.5f)\n", out[0], out[1], out[2] );

		// Channel 0 (sigma=0) must be exactly 1 (Lambertian); channel 2
		// (sigma=1.0) must be strictly less than channel 0 (the model's
		// bihemispherical albedo decreases with roughness -- see (d));
		// the three channels must NOT all be equal (that would mean
		// the fix silently collapsed to a single-channel roughness,
		// reintroducing the failure mode this test exists to catch).
		CheckClose( out[0], 1.0, 1e-4, "per-channel sigma=0 channel exactly 1" );
		Check( out[2] < out[0] - 0.05, "per-channel sigma=1.0 channel visibly lower than sigma=0 channel" );
		Check( out[0] != out[1] || out[1] != out[2], "per-channel roughness produces distinct channel values" );
	}
}

int main()
{
	TestDirectionalHemisphericalReproduction();
	TestBihemispherical();
	TestValueNMConsistency();
	TestEdgeCases();
	TestPerChannelRoughness();

	std::cout << std::endl << g_numChecks << " checks, " << g_numFailures << " failures" << std::endl;
	return g_numFailures == 0 ? 0 : 1;
}
