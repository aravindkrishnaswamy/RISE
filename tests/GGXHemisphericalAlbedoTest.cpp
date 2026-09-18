//////////////////////////////////////////////////////////////////////
//
//  GGXHemisphericalAlbedoTest.cpp - Standalone regression test for
//    GGXBRDF::hemisphericalAlbedo{,NM} (debt ledger DL-123,
//    docs/DEBT_LEDGER.md).
//
//  Coverage:
//    (a) TestBihemispherical -- numerically double-integrates the REAL
//        GGXBRDF::value() over BOTH hemispheres (a brute-force
//        quadrature independent of tools/GGXSpecularBihemisphericalGen.cpp's
//        own bake -- different loop structure, different resolution,
//        and it evaluates value() itself rather than replaying any
//        piece of the fix), at alpha in {0.05,0.2,0.5,1.0} x F0 in
//        {0.04,0.5,1.0} x diffuse in {0,0.5}, Schlick mode.  RED
//        pre-fix (the ledger's own +4.2%..+7.7% at alpha 0.2/0.5);
//        GREEN post-fix to within a stated tolerance.
//    (b) TestConductorMode -- same double-hemisphere brute force at a
//        representative conductor (copper-like n,k) config, alpha in
//        {0.05,0.2,0.5,1.0}, exercising the degree-7 polynomial-fit
//        path (not the exact Schlick affine reduction).
//    (c) TestValueNMConsistency -- same bihemispherical check on the
//        single-wavelength valueNM/hemisphericalAlbedoNM pair, Schlick
//        and conductor.
//    (d) TestMomentZeroCrossCheck -- the baked table's own moment-0
//        row (F=1 identically, i.e. F0=1 Schlick) must reproduce
//        MicrofacetEnergyLUT's E_avg_TABLE_G2 (an independent,
//        previously-shipped bake of the SAME quantity) to within the
//        generator's own measured cross-check tolerance.
//    (e) TestSmoothLimit -- alpha->0.01 (as smooth as the table
//        supports) must approach the pre-fix mirror-limit answer
//        (interfaceFresnel.Mean()) -- the derivation's own stated
//        smooth-limit consistency check.
//    (f) TestAnisotropicFallback -- alphaX != alphaY does not crash
//        and stays a plausible reflectance (residual is documented as
//        an approximation, not gated tightly here -- see DL-139).
//    (g) TestLowAlphaVNDF -- DL-160 red-proof.  `kGGXSpecularQuadWeight`
//        (GGXBRDF.cpp's own baked moment-matched quadrature, consumed
//        by hemisphericalAlbedo{,NM}) resolves its alpha axis with the
//        SAME uniform [0.01,1.0] 32-row mapping and clamp DL-105 fixed
//        on MicrofacetEnergyLUT's E_ss/E_avg tables -- alpha<0.01
//        clamps to row 0 outright.  TestBihemispherical's own uniform
//        angular-grid Bihemispherical() cannot resolve a GGX lobe below
//        alpha~0.03 (same limitation DL-105's own test file documents
//        for its "uniform-hemisphere" estimator), so this uses an
//        independent VNDF-IMPORTANCE-SAMPLING bihemispherical estimator
//        (own re-implementation of D/Lambda/G1 + Heitz-2018 VNDF
//        sampling, own RNG stream, matching the DL-105
//        IndependentGGX precedent in GGXHeightCorrelatedEnergyLUTTest.cpp)
//        that calls the REAL GGXBRDF::value() directly -- not a
//        re-implementation of its Fresnel/multiscatter terms.
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
#include <random>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/math_utils.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/Color/ColorMath.h"
#include "../src/Library/Utilities/MicrofacetEnergyLUT.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/IBSDF.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Materials/GGXBRDF.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	static const double PI = 3.14159265358979323846;

	//! Every painter / BSDF in this codebase is reference-counted with a
	//! PROTECTED destructor -- cannot be stack-allocated.  Same pattern
	//! as tests/OrenNayarHemisphericalAlbedoTest.cpp's Owned<>.
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

	//! True bihemispherical albedo via the REAL BRDF -- independent
	//! loop structure/resolution from GGXSpecularBihemisphericalGen's
	//! own bake, and it evaluates value() directly (not any piece of
	//! the fix).  Same (1/pi)*2*pi=2 reduction as
	//! OrenNayarHemisphericalAlbedoTest.cpp's BihemisphericalAtRhoOne.
	double Bihemispherical( IBSDF& brdf, int Nmu, int Nphi )
	{
		double sum = 0;
		for( int i = 0; i < Nmu; i++ ) {
			const double muI = ( i + 0.5 ) / Nmu;
			const double sinI = sqrt( r_max( 0.0, 1.0 - muI*muI ) );
			const Vector3 wi( sinI, 0, muI ); // phi_i = 0 WLOG (isotropic)
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
		return sum * dmu * dmu * dphi * 2.0;
	}

	//! Resolution picker: a plain uniform-angle midpoint quadrature
	//! needs FAR more nodes to resolve a sharply-peaked GGX lobe at low
	//! alpha than at moderate/high alpha (verified by a convergence
	//! sweep at alpha=0.05, F0=1.0, Schlick: Nmu=96/Nphi=192 reads
	//! 0.9879, Nmu=200/400 reads 0.9982, Nmu=400/800 reads 1.0001,
	//! Nmu=600/1200 reads 1.0003 -- the coarse grid is BIASED LOW by
	//! under-resolving the lobe, not evidence against the fix; alpha
	//! 0.2/0.5/1.0 already agree to <0.02% at the cheap resolution, so
	//! only the low-alpha rows pay the finer cost).
	void PickResolution( double alpha, int& Nmu, int& Nphi )
	{
		if( alpha < 0.1 ) { Nmu = 300; Nphi = 600; }
		else { Nmu = 96; Nphi = 192; }
	}

	double BihemisphericalNM( IBSDF& brdf, double nm, int Nmu, int Nphi )
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

	//////////////////////////////////////////////////////////////////
	//  (a) Schlick mode
	//////////////////////////////////////////////////////////////////

	void TestBihemispherical()
	{
		std::cout << "--- TestBihemispherical (Schlick) ---" << std::endl;

		struct Row { double alpha; double F0; double diffuse; };
		Row rows[] = {
			{ 0.05, 0.04, 0.0 }, { 0.05, 0.04, 0.5 },
			{ 0.05, 0.50, 0.0 }, { 0.05, 1.00, 0.0 },
			{ 0.20, 0.04, 0.0 }, { 0.20, 0.04, 0.5 },
			{ 0.20, 0.50, 0.0 }, { 0.20, 1.00, 0.0 },
			{ 0.50, 0.04, 0.0 }, { 0.50, 0.04, 0.5 },
			{ 0.50, 0.50, 0.0 }, { 0.50, 1.00, 0.0 },
			{ 1.00, 0.04, 0.0 }, { 1.00, 0.04, 0.5 },
			{ 1.00, 0.50, 0.0 }, { 1.00, 1.00, 0.0 },
		};

		// Brute-force quadrature noise + the table's own quadrature-fit
		// residual for Schlick (should be ~exact -- degree-5 curve
		// inside a degree-7 basis) -- 1% covers both with margin,
		// matching DL-07's own bar for this class of fix.
		const double kTol = 0.01;

		for( const Row& row : rows ) {
			Owned<UniformColorPainter> spec( new UniformColorPainter( RISEPel( row.F0, row.F0, row.F0 ) ) );
			Owned<UniformColorPainter> diff( new UniformColorPainter( RISEPel( row.diffuse, row.diffuse, row.diffuse ) ) );
			Owned<UniformScalarPainter> alphaX( new UniformScalarPainter( row.alpha ) );
			Owned<UniformScalarPainter> alphaY( new UniformScalarPainter( row.alpha ) );
			Owned<UniformScalarPainter> iorP( new UniformScalarPainter( 1.5 ) );
			Owned<UniformScalarPainter> extP( new UniformScalarPainter( 0.0 ) );
			Owned<GGXBRDF> brdf( new GGXBRDF( *diff, *spec, *alphaX, *alphaY, *iorP, *extP, eFresnelSchlickF0 ) );

			int Nmu, Nphi; PickResolution( row.alpha, Nmu, Nphi );
			const double trueBi = Bihemispherical( *brdf, Nmu, Nphi );

			RayIntersectionGeometric dummyRi = MakeRIForView( Vector3( 0, 0, 1 ) );
			RISEPel out;
			brdf->hemisphericalAlbedo( dummyRi, out );
			const double reported = ColorMath::MaxValue( out );

			printf( "  alpha=%.2f F0=%.2f diff=%.2f: bihemispherical_true=%.5f  hemisphericalAlbedo()=%.5f  |diff|=%.5f\n",
				row.alpha, row.F0, row.diffuse, trueBi, reported, fabs(reported - trueBi) );

			CheckClose( reported, trueBi, kTol,
				"Schlick hemisphericalAlbedo() matches true bihemispherical at alpha=" + std::to_string(row.alpha)
				+ " F0=" + std::to_string(row.F0) + " diffuse=" + std::to_string(row.diffuse) );
		}
	}

	//////////////////////////////////////////////////////////////////
	//  (b) Conductor mode -- degree-7 polynomial-fit path
	//////////////////////////////////////////////////////////////////

	void TestConductorMode()
	{
		std::cout << "--- TestConductorMode ---" << std::endl;

		// Copper-like n,k at a representative visible wavelength.
		const double ior = 0.47;
		const double ext = 2.63;
		const double kTol = 0.015; // conductor Fresnel is a fit, not exact -- slightly looser

		double alphas[] = { 0.05, 0.2, 0.5, 1.0 };
		for( double alpha : alphas ) {
			Owned<UniformColorPainter> spec( new UniformColorPainter( RISEPel( 1, 1, 1 ) ) );
			Owned<UniformColorPainter> diff( new UniformColorPainter( RISEPel( 0, 0, 0 ) ) );
			Owned<UniformScalarPainter> alphaX( new UniformScalarPainter( alpha ) );
			Owned<UniformScalarPainter> alphaY( new UniformScalarPainter( alpha ) );
			Owned<UniformScalarPainter> iorP( new UniformScalarPainter( ior ) );
			Owned<UniformScalarPainter> extP( new UniformScalarPainter( ext ) );
			Owned<GGXBRDF> brdf( new GGXBRDF( *diff, *spec, *alphaX, *alphaY, *iorP, *extP, eFresnelConductor ) );

			int Nmu, Nphi; PickResolution( alpha, Nmu, Nphi );
			const double trueBi = Bihemispherical( *brdf, Nmu, Nphi );

			RayIntersectionGeometric dummyRi = MakeRIForView( Vector3( 0, 0, 1 ) );
			RISEPel out;
			brdf->hemisphericalAlbedo( dummyRi, out );
			const double reported = ColorMath::MaxValue( out );

			printf( "  conductor alpha=%.2f: bihemispherical_true=%.5f  hemisphericalAlbedo()=%.5f  |diff|=%.5f\n",
				alpha, trueBi, reported, fabs(reported - trueBi) );

			CheckClose( reported, trueBi, kTol,
				"Conductor hemisphericalAlbedo() matches true bihemispherical at alpha=" + std::to_string(alpha) );
		}
	}

	//////////////////////////////////////////////////////////////////
	//  (c) valueNM / hemisphericalAlbedoNM consistency
	//////////////////////////////////////////////////////////////////

	void TestValueNMConsistency()
	{
		std::cout << "--- TestValueNMConsistency ---" << std::endl;
		const double kTol = 0.012;

		struct Row { double alpha; FresnelMode mode; };
		Row rows[] = {
			{ 0.2, eFresnelSchlickF0 }, { 0.5, eFresnelSchlickF0 },
			{ 0.2, eFresnelConductor }, { 0.5, eFresnelConductor },
		};

		for( const Row& row : rows ) {
			Owned<UniformColorPainter> spec( new UniformColorPainter( RISEPel( 0.3, 0.3, 0.3 ) ) );
			Owned<UniformColorPainter> diff( new UniformColorPainter( RISEPel( 0, 0, 0 ) ) );
			Owned<UniformScalarPainter> alphaX( new UniformScalarPainter( row.alpha ) );
			Owned<UniformScalarPainter> alphaY( new UniformScalarPainter( row.alpha ) );
			Owned<UniformScalarPainter> iorP( new UniformScalarPainter( 0.47 ) );
			Owned<UniformScalarPainter> extP( new UniformScalarPainter( 2.63 ) );
			Owned<GGXBRDF> brdf( new GGXBRDF( *diff, *spec, *alphaX, *alphaY, *iorP, *extP, row.mode ) );

			const double trueBi = BihemisphericalNM( *brdf, 550.0, 80, 160 );

			RayIntersectionGeometric dummyRi = MakeRIForView( Vector3( 0, 0, 1 ) );
			Scalar out = 0;
			brdf->hemisphericalAlbedoNM( dummyRi, 550.0, out );

			printf( "  mode=%d alpha=%.2f nm=550: bihemispherical_true=%.5f  hemisphericalAlbedoNM()=%.5f\n",
				(int)row.mode, row.alpha, trueBi, out );

			CheckClose( out, trueBi, kTol,
				"hemisphericalAlbedoNM() matches true bihemispherical, mode=" + std::to_string((int)row.mode)
				+ " alpha=" + std::to_string(row.alpha) );
		}
	}

	//////////////////////////////////////////////////////////////////
	//  (d) Moment-0 cross-check against E_avg_TABLE_G2
	//////////////////////////////////////////////////////////////////

	void TestMomentZeroCrossCheck()
	{
		std::cout << "--- TestMomentZeroCrossCheck ---" << std::endl;

		// F0=1 Schlick reduces exactly to F(muH)==1 always, so
		// hemisphericalAlbedo's R_ss term (no diffuse, alpha isotropic)
		// should reproduce MicrofacetEnergyLUT::LookupEavgG2(alpha) --
		// two independent bakes of the identical quantity (see this
		// test file's own header (d)).
		double alphas[] = { 0.01, 0.1, 0.3, 0.5, 0.7, 1.0 };
		for( double alpha : alphas ) {
			Owned<UniformColorPainter> spec( new UniformColorPainter( RISEPel( 1, 1, 1 ) ) );
			Owned<UniformColorPainter> diff( new UniformColorPainter( RISEPel( 0, 0, 0 ) ) );
			Owned<UniformScalarPainter> alphaX( new UniformScalarPainter( alpha ) );
			Owned<UniformScalarPainter> alphaY( new UniformScalarPainter( alpha ) );
			Owned<UniformScalarPainter> iorP( new UniformScalarPainter( 1.5 ) );
			Owned<UniformScalarPainter> extP( new UniformScalarPainter( 0.0 ) );
			Owned<GGXBRDF> brdf( new GGXBRDF( *diff, *spec, *alphaX, *alphaY, *iorP, *extP, eFresnelSchlickF0 ) );

			RayIntersectionGeometric dummyRi = MakeRIForView( Vector3( 0, 0, 1 ) );
			RISEPel out;
			brdf->hemisphericalAlbedo( dummyRi, out );
			const double reported = ColorMath::MaxValue( out );

			const double eavg = MicrofacetEnergyLUT::LookupEavgG2( alpha );

			printf( "  alpha=%.2f: hemisphericalAlbedo(F0=1)=%.5f  LookupEavgG2=%.5f  |diff|=%.5f\n",
				alpha, reported, eavg, fabs(reported - eavg) );

			// The multiscatter term contributes F_ms*(1-Eavg) on top of
			// R_ss at F0=1 (F_ms->1 as F0->1, a real physical
			// contribution, not zero) -- so this is NOT expected to
			// equal Eavg exactly except in the alpha->0 limit where
			// (1-Eavg)->0.  Only assert equality at the smoothest node.
			if( alpha <= 0.01 + 1e-9 ) {
				CheckClose( reported, eavg, 0.01, "F0=1 smooth-limit hemisphericalAlbedo tracks LookupEavgG2" );
			}
		}
	}

	//////////////////////////////////////////////////////////////////
	//  (e) Smooth limit: alpha->0.01 approaches Mean()
	//////////////////////////////////////////////////////////////////

	void TestSmoothLimit()
	{
		std::cout << "--- TestSmoothLimit ---" << std::endl;

		Owned<UniformColorPainter> spec( new UniformColorPainter( RISEPel( 0.04, 0.04, 0.04 ) ) );
		Owned<UniformColorPainter> diff( new UniformColorPainter( RISEPel( 0, 0, 0 ) ) );
		Owned<UniformScalarPainter> alphaX( new UniformScalarPainter( 0.01 ) );
		Owned<UniformScalarPainter> alphaY( new UniformScalarPainter( 0.01 ) );
		Owned<UniformScalarPainter> iorP( new UniformScalarPainter( 1.5 ) );
		Owned<UniformScalarPainter> extP( new UniformScalarPainter( 0.0 ) );
		Owned<GGXBRDF> brdf( new GGXBRDF( *diff, *spec, *alphaX, *alphaY, *iorP, *extP, eFresnelSchlickF0 ) );

		RayIntersectionGeometric dummyRi = MakeRIForView( Vector3( 0, 0, 1 ) );
		RISEPel out;
		brdf->hemisphericalAlbedo( dummyRi, out );
		const double reported = ColorMath::MaxValue( out );

		// Mean() for Schlick F0=0.04: F0 + (1-F0)/21.
		const double meanClosedForm = 0.04 + (1.0 - 0.04) / 21.0;

		printf( "  alpha=0.01 F0=0.04: hemisphericalAlbedo=%.5f  Mean()=%.5f\n", reported, meanClosedForm );
		CheckClose( reported, meanClosedForm, 0.01, "alpha->0.01 hemisphericalAlbedo approaches Mean() (mirror limit)" );
	}

	//////////////////////////////////////////////////////////////////
	//  (f) Anisotropic fallback -- sanity only
	//////////////////////////////////////////////////////////////////

	void TestAnisotropicFallback()
	{
		std::cout << "--- TestAnisotropicFallback ---" << std::endl;

		Owned<UniformColorPainter> spec( new UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) ) );
		Owned<UniformColorPainter> diff( new UniformColorPainter( RISEPel( 0, 0, 0 ) ) );
		Owned<UniformScalarPainter> alphaX( new UniformScalarPainter( 0.05 ) );
		Owned<UniformScalarPainter> alphaY( new UniformScalarPainter( 0.5 ) );
		Owned<UniformScalarPainter> iorP( new UniformScalarPainter( 1.5 ) );
		Owned<UniformScalarPainter> extP( new UniformScalarPainter( 0.0 ) );
		Owned<GGXBRDF> brdf( new GGXBRDF( *diff, *spec, *alphaX, *alphaY, *iorP, *extP, eFresnelSchlickF0 ) );

		// min(alphaX,alphaY)=0.05 needs the finer resolution -- see
		// PickResolution's convergence note.
		const double trueBi = Bihemispherical( *brdf, 300, 600 );

		RayIntersectionGeometric dummyRi = MakeRIForView( Vector3( 0, 0, 1 ) );
		RISEPel out;
		brdf->hemisphericalAlbedo( dummyRi, out );
		const double reported = ColorMath::MaxValue( out );

		printf( "  alphaX=0.05 alphaY=0.5: bihemispherical_true=%.5f  hemisphericalAlbedo()=%.5f  |diff|=%.5f\n",
			trueBi, reported, fabs(reported - trueBi) );

		Check( reported > 0.0 && reported < 1.5, "anisotropic hemisphericalAlbedo stays a plausible reflectance" );
		// Loose bound only -- DL-139 tracks the isotropic-alphaEff
		// approximation's residual for genuinely anisotropic materials.
		Check( fabs( reported - trueBi ) < 0.15, "anisotropic hemisphericalAlbedo within a loose (DL-139-tracked) bound" );
	}

	//////////////////////////////////////////////////////////////////
	//  (g) DL-160 red-proof: low alpha, VNDF-importance-sampled
	//      independent bihemispherical estimator.
	//////////////////////////////////////////////////////////////////

	// Own from-scratch re-implementation (own RNG stream) of the GGX
	// isotropic D/Lambda/G1 primitives and Heitz-2018 VNDF sampling --
	// independent of tools/GGXSpecularBihemisphericalGen.cpp's and
	// GenerateMicrofacetEnergyLUT.cpp's own copies, matching the DL-105
	// `IndependentGGX` precedent in tests/GGXHeightCorrelatedEnergyLUTTest.cpp.
	// Used ONLY to build a VNDF importance-sampling PROPOSAL for wo
	// given wi -- the numerator of every estimator sample below calls
	// the REAL, production `GGXBRDF::value()` directly, so this is a
	// true black-box check of the fix's effect on the shipped code
	// path, not a replay of any piece of it.
	namespace IndependentGGX
	{
		static double D_Isotropic( double alpha, double cosThetaM )
		{
			if( cosThetaM <= 0 ) return 0.0;
			const double a2 = alpha * alpha;
			const double d = cosThetaM * cosThetaM * ( a2 - 1.0 ) + 1.0;
			return a2 / ( PI * d * d );
		}
		static double Lambda( double alpha, double cosTheta )
		{
			if( cosTheta >= 1.0 - 1e-10 ) return 0.0;
			if( cosTheta < 1e-10 ) return 1e10;
			const double cos2 = cosTheta * cosTheta;
			const double tan2 = ( 1.0 - cos2 ) / cos2;
			return ( -1.0 + sqrt( 1.0 + alpha * alpha * tan2 ) ) * 0.5;
		}
		static double G1( double alpha, double cosTheta ) { return 1.0 / ( 1.0 + Lambda( alpha, cosTheta ) ); }

		// Heitz 2018 "Sampling the GGX Distribution of Visible Normals"
		// -- transcribed independently (own variable names/structure),
		// wi already in local shading space (normal = (0,0,1)).
		static Vector3 VNDFSample( const Vector3& wi, double alpha, double u1, double u2 )
		{
			const Vector3 wiStd = Vector3Ops::Normalize( Vector3( alpha * wi.x, alpha * wi.y, wi.z ) );
			const double lensq = wiStd.x * wiStd.x + wiStd.y * wiStd.y;
			const Vector3 T1 = ( lensq > 0 )
				? Vector3( -wiStd.y, wiStd.x, 0 ) * ( 1.0 / sqrt( lensq ) )
				: Vector3( 1, 0, 0 );
			const Vector3 T2 = Vector3Ops::Cross( wiStd, T1 );

			const double r = sqrt( u1 );
			const double phi = 2.0 * PI * u2;
			double t1 = r * cos( phi );
			double t2 = r * sin( phi );
			const double s = 0.5 * ( 1.0 + wiStd.z );
			t2 = ( 1.0 - s ) * sqrt( r_max( 0.0, 1.0 - t1 * t1 ) ) + s * t2;

			const Vector3 Nh = T1 * t1 + T2 * t2 + wiStd * sqrt( r_max( 0.0, 1.0 - t1 * t1 - t2 * t2 ) );
			return Vector3Ops::Normalize( Vector3( alpha * Nh.x, alpha * Nh.y, r_max( 0.0, Nh.z ) ) );
		}
	}

	//! Independent, VNDF-importance-sampled bihemispherical estimator
	//! of the REAL brdf.value().  Standard Heitz-2018 VNDF pdf:
	//! p(wo|wi) = G1(wi)*D(m)/(4*cosWi), so the per-sample estimator of
	//! INT f(wi,wo)*cosO dwo is f(wi,wo)*cosO*4*cosWi/(G1(wi)*D(m)).
	//! The outer wi integral uses the same fixed-grid + final *2.0
	//! normalization Bihemispherical() above and the generator's own
	//! Moments() use (isotropy lets phi_i integrate out to exactly
	//! 2*pi, which cancels against the 1/pi bihemispherical measure to
	//! a factor of 2).  Needed because a plain angular grid in wo (as
	//! Bihemispherical() uses) cannot resolve a GGX lobe whose angular
	//! width is ~alpha at alpha<0.03 -- same limitation DL-105 hit for
	//! its own "uniform-hemisphere" estimator.
	double BihemisphericalVNDF( IBSDF& brdf, double alpha, int numMuI, int numSamples, unsigned long long seed )
	{
		std::mt19937_64 rng( seed );
		std::uniform_real_distribution<double> uni( 0.0, 1.0 );
		const double dmu = 1.0 / numMuI;
		double outerSum = 0.0;
		for( int bi = 0; bi < numMuI; bi++ )
		{
			const double muI = ( bi + 0.5 ) * dmu;
			const double sinI = sqrt( r_max( 0.0, 1.0 - muI * muI ) );
			const Vector3 wi( sinI, 0.0, muI );
			const double G1wi = IndependentGGX::G1( alpha, muI );

			double innerSum = 0.0;
			for( int s = 0; s < numSamples; s++ )
			{
				const double u1 = uni( rng );
				const double u2 = uni( rng );
				const Vector3 m = IndependentGGX::VNDFSample( wi, alpha, u1, u2 );
				const double wiDotM = Vector3Ops::Dot( wi, m );
				if( wiDotM <= 0 ) continue;
				const Vector3 wo = Vector3Ops::Normalize( m * ( 2.0 * wiDotM ) - wi );
				const double cosWo = wo.z;
				if( cosWo <= 0 ) continue;
				const double Dm = IndependentGGX::D_Isotropic( alpha, m.z );
				if( Dm <= 0 ) continue;

				RayIntersectionGeometric ri = MakeRIForView( wo );
				const double f = ColorMath::MaxValue( brdf.value( wi, ri ) );

				const double weight = f * cosWo * 4.0 * muI / ( G1wi * Dm );
				innerSum += weight;
			}
			outerSum += ( innerSum / numSamples ) * muI * dmu;
		}
		return outerSum * 2.0;
	}

	void TestLowAlphaVNDF()
	{
		std::cout << "--- TestLowAlphaVNDF (DL-160 red-proof) ---" << std::endl;

		struct Row { double alpha; double F0; };
		Row rows[] = {
			{ 0.002, 1.00 }, { 0.002, 0.04 }, { 0.002, 0.00 },
			{ 0.005, 1.00 }, { 0.005, 0.04 }, { 0.005, 0.00 },
			{ 0.008, 1.00 }, { 0.008, 0.00 },
			{ 0.015, 1.00 }, { 0.015, 0.00 },
			{ 0.03,  1.00 }, { 0.03,  0.00 },
		};

		// Tighter than TestBihemispherical's 0.01 -- the DL-160 defect
		// is a nearly F0-INDEPENDENT ABSOLUTE weight-sum error (~5e-4,
		// derived closed-form from the row0-vs-alpha->0-limit weight
		// difference), so at F0=0 (where the true reflectance itself
		// is small, ~0.05) a 1% absolute bar is far too loose to see
		// it -- 4e-4 catches the ~5e-4 defect while staying above this
		// estimator's own VNDF MC noise floor (~1e-4 at these sample
		// counts).
		const double kTol = 0.0004;

		for( const Row& row : rows )
		{
			Owned<UniformColorPainter> spec( new UniformColorPainter( RISEPel( row.F0, row.F0, row.F0 ) ) );
			Owned<UniformColorPainter> diff( new UniformColorPainter( RISEPel( 0, 0, 0 ) ) );
			Owned<UniformScalarPainter> alphaX( new UniformScalarPainter( row.alpha ) );
			Owned<UniformScalarPainter> alphaY( new UniformScalarPainter( row.alpha ) );
			Owned<UniformScalarPainter> iorP( new UniformScalarPainter( 1.5 ) );
			Owned<UniformScalarPainter> extP( new UniformScalarPainter( 0.0 ) );
			Owned<GGXBRDF> brdf( new GGXBRDF( *diff, *spec, *alphaX, *alphaY, *iorP, *extP, eFresnelSchlickF0 ) );

			const unsigned long long seed = 0xD1160ULL + (unsigned long long)( row.alpha * 1.0e7 ) * 1000003ULL + (unsigned long long)( row.F0 * 1.0e6 );
			const double trueBi = BihemisphericalVNDF( *brdf, row.alpha, 48, 100000, seed );

			RayIntersectionGeometric dummyRi = MakeRIForView( Vector3( 0, 0, 1 ) );
			RISEPel out;
			brdf->hemisphericalAlbedo( dummyRi, out );
			const double reported = ColorMath::MaxValue( out );

			printf( "  alpha=%.4f F0=%.2f: bihemispherical_VNDF=%.5f  hemisphericalAlbedo()=%.5f  |diff|=%.5f  relerr=%.2f%%\n",
				row.alpha, row.F0, trueBi, reported, fabs(reported - trueBi), 100.0*fabs(reported-trueBi)/trueBi );

			CheckClose( reported, trueBi, kTol,
				"DL-160 low-alpha hemisphericalAlbedo matches VNDF bihemispherical at alpha=" + std::to_string(row.alpha)
				+ " F0=" + std::to_string(row.F0) );
		}
	}
}

int main()
{
	TestBihemispherical();
	TestConductorMode();
	TestValueNMConsistency();
	TestMomentZeroCrossCheck();
	TestSmoothLimit();
	TestAnisotropicFallback();
	TestLowAlphaVNDF();

	std::cout << std::endl << g_numChecks << " checks, " << g_numFailures << " failures" << std::endl;
	return g_numFailures == 0 ? 0 : 1;
}
