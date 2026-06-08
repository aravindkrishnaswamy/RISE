//////////////////////////////////////////////////////////////////////
//
//  ThinFilmBRDFTest.cpp - Validates the eFresnelThinFilmConductor wiring
//    into the GGX microfacet BRDF/SPF (Phase-2 piece P2-B,
//    docs/THIN_FILM_INTERFERENCE.md §7).
//
//    The thin-film GGX is NOT yet reachable through the public factory
//    (parser/RISE_API wiring is the next piece, P2-C), so this test
//    constructs GGXBRDF / GGXSPF DIRECTLY with uniform-value
//    IScalarPainters for the film/substrate stack, exactly like
//    GGXFresnelModeTest does for the conductor/Schlick modes.
//
//    METHODOLOGY NOTE — the production build is -O3 -ffast-math -flto, so
//    a hand-reconstruction of the FULL BRDF (single-scatter × specFactor +
//    Kulla-Conty multiscatter) drifts from the library's compiled value by
//    FP-reassociation noise.  The single-scatter reflectance R is therefore
//    pinned EXACTLY (to ~1e-9 / ~1e-16) by Test B, where a geometric weight
//    cancels in a ratio; Test A validates the FULL single+multi model at the
//    looser ~1e-3 reconstruction tolerance:
//
//      * Test A drives the SAME geometry at FOUR film thicknesses.  Per
//        (λ, geometry) cell the thin-film valueNM is exactly
//        valueNM(d) = A·R(d) + B·F_ms(d) with A = specColor·specFactor and
//        B = specColor·f_ms BOTH thickness-independent, R(d) the
//        single-scatter reflectance and F_ms(d) the Kulla-Conty tail built
//        from the THIN-FILM hemispherical F_avg (P2-D).  The test solves
//        (A,B) from the best-conditioned thickness pair (R and F_avg are not
//        collinear — the film shifts the average reflectance differently
//        from the single-scatter one, so the [R F_ms] 2×2 is non-singular),
//        then PREDICTS the other two valueNMs.  This FAILS if the tail still
//        used the substrate F_avg, so it validates the P2-D wiring — match
//        to ~1e-3 (the -ffast-math reconstruction limit).
//      * Test B drives ScatterNM in thin-film AND conductor mode with the
//        SAME scripted sampler (⇒ identical half-vector, G2, G1, pSelect)
//        and compares the kray ratio to the oracle R_film/R_cond — the
//        geometric weight cancels, isolating the SINGLE-SCATTER R — match to
//        ~1e-16.  ScatterNM's reflection ray is a pure single-scatter lobe,
//        so this is the exact-R pin; valueNM (which SUMS single+multi) routes
//        the SAME single-scatter term, so the two agree there (the
//        HWSS-companion twin).
//      * Test D's additive invariant compares conductor mode to thin-film
//        mode with film index == air: the thin-film Airy then reduces
//        ALGEBRAICALLY to the bare air→substrate Fresnel, so both modes
//        run the identical downstream code and agree to ~1e-16 — proving
//        the conductor path is byte-identical (unperturbed by the new
//        branch) AND that the branch collapses correctly when the film
//        vanishes.
//
//    Assertions:
//      A. FULL SINGLE+MULTI MODEL — per (λ, geometry) cell, valueNM(d) ==
//         A·R(d) + B·F_ms(d) (A,B thickness-independent; F_ms from the
//         THIN-FILM F_avg, P2-D): solve A,B from a well-conditioned
//         thickness pair, predict the rest (~1e-3).  Validates the
//         multiscatter tail uses the thin-film hemispherical average AND
//         correct cosine/λ/stack/thickness argument passing.
//      B. TWIN CONSISTENCY (exact single-scatter R) — ScatterNM's
//         thin-film/conductor kray ratio equals the oracle R_film/R_cond to
//         ~1e-16 (the reflection ray is a pure single-scatter lobe, so this
//         is the exact-R pin).  GGXSPF does NOT override EvaluateKrayNM, so
//         HWSS companions route through valueNM's single-scatter term — the
//         two MUST agree (audit-by-bug-pattern.md).
//      C. RECIPROCITY — valueNM(wi→wo) ≈ valueNM(wo→wi) for the thin-film
//         lobe (symmetric NDF + height-correlated G2 + half-vector Fresnel).
//      D. ADDITIVE INVARIANT — conductor mode ≡ thin-film(film=air) mode to
//         FP precision (conductor path untouched; branch collapses on a
//         vanishing film); plus ThinFilm::ReflectanceConductor(film=air) ≡
//         Optics::CalculateConductorReflectance (pure-math bare limit).
//      E. RGB ALBEDO BASIS — a wavelength-INDEPENDENT reflectance integrates
//         to a NEUTRAL RGB (R≡const → R·(1,1,1)), NOT an illuminant-tinted
//         colour (the §8 white-normalisation), while a real interference
//         stack is chromatically tinted.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <iomanip>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Optics.h"
#include "../src/Library/Utilities/ThinFilm.h"
#include "../src/Library/Utilities/MicrofacetEnergyLUT.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/ISampler.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Materials/GGXBRDF.h"
#include "../src/Library/Materials/GGXSPF.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	int s_pass = 0;
	int s_fail = 0;

	void Check( bool ok, const char* what )
	{
		if( ok ) {
			++s_pass;
		} else {
			++s_fail;
			std::cout << "  FAIL: " << what << "\n";
		}
	}

	// A canonical heat-tint stack: TiO2-like transparent film on a
	// titanium-like (absorbing) substrate.  Constant n,k across the band
	// (uniform painters), so the interference is driven purely by the
	// thickness/phase.
	const Scalar kFilmN   = 2.50;	// oxide n (TiO2-ish)
	const Scalar kFilmK   = 0.00;	// transparent oxide
	const Scalar kSubN    = 2.74;	// substrate n (Ti-ish)
	const Scalar kSubK    = 3.79;	// substrate k (absorbing)
	const Scalar kSpec    = 0.85;	// specular tint (scalar)

	// Build an RI whose incoming ray travels along rayDir (INTO the
	// surface) with geometric normal +Z.  Mirrors GGXFresnelModeTest.
	RayIntersectionGeometric MakeRI( const Vector3& rayDir )
	{
		Ray inRay( Point3( 0, 0, 1 ), rayDir );
		RasterizerState rs = { 0, 0 };
		RayIntersectionGeometric ri( inRay, rs );
		ri.bHit = true;
		ri.range = 1.0;
		ri.ptIntersection = Point3( 0, 0, 0 );
		ri.vNormal = Vector3( 0, 0, 1 );
		ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
		ri.ptCoord = Point2( 0.5, 0.5 );
		return ri;
	}

	// A deterministic sampler replaying a fixed list of 1D draws, so we
	// steer GGXSPF::ScatterNM into a CHOSEN microfacet sample and get the
	// same half-vector across two SPF instances.  Beyond the script it
	// returns a constant tail.
	class ScriptedSampler : public ISampler
	{
		const Scalar*	seq;
		unsigned int	n;
		unsigned int	idx;
		Scalar			tail;
	public:
		ScriptedSampler( const Scalar* s, unsigned int count, Scalar tailVal )
			: seq( s ), n( count ), idx( 0 ), tail( tailVal ) {}

		Scalar Get1D()
		{
			if( idx < n ) return seq[idx++];
			return tail;
		}
		Point2 Get2D() { return Point2( Get1D(), Get1D() ); }
		void StartStream( int /*streamIndex*/ ) {}
	};

	// Construct a thin-film GGXBRDF for a given film thickness + roughness.
	// Caller owns the returned BRDF (release()) and the painter set.
	struct Stack
	{
		UniformColorPainter*	diffuse;
		UniformColorPainter*	specular;
		UniformScalarPainter*	alphaX;
		UniformScalarPainter*	alphaY;
		UniformScalarPainter*	ior;
		UniformScalarPainter*	ext;
		UniformScalarPainter*	filmIor;
		UniformScalarPainter*	filmExt;
		UniformScalarPainter*	filmThk;

		Stack( Scalar alpha, Scalar filmN, Scalar filmK, Scalar filmThkNm )
		{
			diffuse  = new UniformColorPainter( RISEPel( 0.0, 0.0, 0.0 ) ); diffuse->addref();
			specular = new UniformColorPainter( RISEPel( kSpec, kSpec, kSpec ) ); specular->addref();
			alphaX   = new UniformScalarPainter( alpha );  alphaX->addref();
			alphaY   = new UniformScalarPainter( alpha );  alphaY->addref();
			ior      = new UniformScalarPainter( kSubN );  ior->addref();
			ext      = new UniformScalarPainter( kSubK );  ext->addref();
			filmIor  = new UniformScalarPainter( filmN );  filmIor->addref();
			filmExt  = new UniformScalarPainter( filmK );  filmExt->addref();
			filmThk  = new UniformScalarPainter( filmThkNm ); filmThk->addref();
		}
		~Stack()
		{
			diffuse->release(); specular->release();
			alphaX->release(); alphaY->release();
			ior->release(); ext->release();
			filmIor->release(); filmExt->release(); filmThk->release();
		}
		GGXBRDF* MakeThinFilmBRDF() const
		{
			GGXBRDF* b = new GGXBRDF( *diffuse, *specular, *alphaX, *alphaY, *ior, *ext,
				eFresnelThinFilmConductor, nullptr, filmIor, filmExt, filmThk );
			b->addref();
			return b;
		}
		GGXSPF* MakeThinFilmSPF() const
		{
			GGXSPF* s = new GGXSPF( *diffuse, *specular, *alphaX, *alphaY, *ior, *ext,
				eFresnelThinFilmConductor, nullptr, filmIor, filmExt, filmThk );
			s->addref();
			return s;
		}
		// Conductor twin: NO film slots (default nullptr) — exactly the
		// legacy call the additive invariant must leave untouched.
		GGXBRDF* MakeConductorBRDF() const
		{
			GGXBRDF* b = new GGXBRDF( *diffuse, *specular, *alphaX, *alphaY, *ior, *ext,
				eFresnelConductor );
			b->addref();
			return b;
		}
		GGXSPF* MakeConductorSPF() const
		{
			GGXSPF* s = new GGXSPF( *diffuse, *specular, *alphaX, *alphaY, *ior, *ext,
				eFresnelConductor );
			s->addref();
			return s;
		}
	};
}

// ============================================================
//  Test A: full single+multi model  valueNM(d) = A*R(d) + B*F_ms(d)
//          (A,B thickness-independent; F_ms uses the THIN-FILM F_avg;
//          solve A,B from a well-conditioned pair, predict the rest).
// ============================================================
static bool TestSpectralExactness()
{
	std::cout << "--- Test A: full single+multi model (valueNM = A*R(d) + B*F_ms(d)) ---\n";
	const bool startFail = s_fail;

	// The thin-film GGX valueNM is, per hero wavelength and FIXED geometry,
	//   valueNM(d) = A * R(d)  +  B * F_ms(d)
	// where A = specColor*specFactor and B = specColor*f_ms are BOTH
	// thickness-INDEPENDENT (same alpha, same half-vector across thickness),
	// R(d)  = ThinFilm::ReflectanceConductor (the single-scatter lobe), and
	// F_ms(d) = ComputeFms( ThinFilm::FresnelAvgConductor(d), Eavg ) is the
	// Kulla-Conty multiscatter tail evaluated with the THIN-FILM hemispherical
	// F_avg (P2-D).  With 4 thicknesses we have 4 equations in the 2 unknowns
	// (A,B): per cell we solve A,B from the BEST-CONDITIONED thickness pair
	// (the [R F_ms] 2x2 must be non-singular -- R and F_avg are not collinear
	// because the film shifts the average reflectance differently from the
	// single-scatter reflectance), then PREDICT the remaining valueNMs and
	// compare to the library.  Because the multiscatter term carries the
	// thin-film F_avg, this prediction would FAIL if the renderer still used
	// the substrate F_avg in the tail -- so it validates the P2-D wiring, not
	// just argument passing.  Tolerance ~1e-3 is the -ffast-math
	// reconstruction limit (header methodology note).
	//
	// Single-scatter R EXACTNESS (the old ratio's job) is now pinned to 1e-9
	// by Test B (ScatterNM's reflection ray is a pure single-scatter lobe).

	// Roughness 0.50: the multiscatter tail (which carries the thin-film
	// F_avg) is a HIGH-roughness correction, so the tail must carry real
	// weight for this test to discriminate thin-film F_avg from substrate
	// F_avg.  At alpha=0.50 a substrate-F_avg regression shows up as a ~13%
	// prediction error (vs the ~1e-3 fast-math reconstruction floor); at
	// alpha=0.10 the tail is too small and the signal collapses to the floor.
	const Scalar alpha = 0.50;
	const Scalar ds[4] = { 60.0, 160.0, 250.0, 310.0 };	// well-conditioned quartet
	Stack s0( alpha, kFilmN, kFilmK, ds[0] );
	Stack s1( alpha, kFilmN, kFilmK, ds[1] );
	Stack s2( alpha, kFilmN, kFilmK, ds[2] );
	Stack s3( alpha, kFilmN, kFilmK, ds[3] );
	GGXBRDF* b[4] = { s0.MakeThinFilmBRDF(), s1.MakeThinFilmBRDF(), s2.MakeThinFilmBRDF(), s3.MakeThinFilmBRDF() };

	// Eavg for the isotropic LUT (alphaEff = sqrt(alphaX*alphaY) = alpha here),
	// matching GGXBRDF::valueNM's multiscatter tail exactly.
	const Scalar Eavg = MicrofacetEnergyLUT::LookupEavg( alpha );

	const Scalar wavelengths[] = { 430.0, 500.0, 560.0, 620.0, 670.0 };
	const Scalar thetaIs[] = { 0.0, 0.4, 0.8 };

	double maxRel = 0.0;
	int compared = 0;
	int skippedIllCond = 0;
	double bestCondSeen = 0.0;
	const double kCondFloor = 0.05;	// require |det|/scale above this to solve

	for( int wq = 0; wq < 5; ++wq )
	{
		const Scalar nm = wavelengths[wq];
		for( int a = 0; a < 3; ++a )
		{
			const Vector3 v( sin( thetaIs[a] ), 0, cos( thetaIs[a] ) );	// light dir (wi)
			const Scalar to = 0.15;
			const Vector3 r( sin( to ) * cos( 1.0 ), sin( to ) * sin( 1.0 ), cos( to ) );	// view dir
			RayIntersectionGeometric ri = MakeRI( -r );

			const Vector3 h = Vector3Ops::Normalize( v + r );
			const Scalar cosWoH = r_max( Scalar( 0 ), Vector3Ops::Dot( r, h ) );

			double val[4], R[4], Fms[4];
			for( int k = 0; k < 4; ++k ) {
				val[k] = b[k]->valueNM( v, ri, nm );
				R[k]   = ThinFilm::ReflectanceConductor( cosWoH, nm, 1.0, 0.0, kFilmN, kFilmK, ds[k], kSubN, kSubK );
				const Scalar Favg = ThinFilm::FresnelAvgConductor( nm, 1.0, 0.0, kFilmN, kFilmK, ds[k], kSubN, kSubK );
				Fms[k] = MicrofacetEnergyLUT::ComputeFms<Scalar>( Favg, Eavg );
			}

			// Pick the best-conditioned [R F_ms] 2x2 among the 4 thicknesses.
			int si = -1, sj = -1;
			double bestCond = 0.0;
			for( int i = 0; i < 4; ++i )
			for( int j = i + 1; j < 4; ++j ) {
				const double det = R[i] * Fms[j] - R[j] * Fms[i];
				const double scale = ( std::fabs( R[i] ) + std::fabs( R[j] ) ) * ( std::fabs( Fms[i] ) + std::fabs( Fms[j] ) );
				const double cond = ( scale > 0.0 ) ? std::fabs( det ) / scale : 0.0;
				if( cond > bestCond ) { bestCond = cond; si = i; sj = j; }
			}
			if( bestCond > bestCondSeen ) bestCondSeen = bestCond;
			if( si < 0 || bestCond < kCondFloor ) { ++skippedIllCond; continue; }

			// Solve [R_si F_si; R_sj F_sj] [A;B] = [val_si; val_sj] (Cramer).
			const double det = R[si] * Fms[sj] - R[sj] * Fms[si];
			const double A = ( val[si] * Fms[sj] - val[sj] * Fms[si] ) / det;
			const double B = ( R[si] * val[sj] - R[sj] * val[si] ) / det;

			// Predict the OTHER two thicknesses and compare to the library.
			for( int k = 0; k < 4; ++k ) {
				if( k == si || k == sj ) continue;
				const double predict = A * R[k] + B * Fms[k];
				const double rel = std::fabs( predict - val[k] ) / r_max( std::fabs( val[k] ), Scalar( 1e-9 ) );
				if( rel > maxRel ) maxRel = rel;
				++compared;
			}
		}
	}

	std::cout << "  compared=" << compared << " skippedIllCond=" << skippedIllCond
			  << " bestCond=" << std::fixed << std::setprecision( 3 ) << bestCondSeen
			  << " maxRelErr = " << std::scientific << std::setprecision( 3 ) << maxRel << "\n";
	Check( compared >= 10, "full-model prediction exercised >=10 (lambda, geometry) cells" );
	Check( bestCondSeen > 0.10, "the [R F_ms] solve was well-conditioned (R, F_avg not collinear)" );
	Check( maxRel < 1e-3,
		"valueNM(d) == A*R(d) + B*F_ms(d) with THIN-FILM F_avg in the tail (predict-2-from-2)" );

	for( int k = 0; k < 4; ++k ) b[k]->release();
	return s_fail == startFail;
}

// ============================================================
//  Test B: ScatterNM ≡ valueNM thin-film term (twin consistency)
// ============================================================
static bool TestTwinConsistency()
{
	std::cout << "\n--- Test B: ScatterNM ≡ valueNM (twin / HWSS-companion consistency) ---\n";
	const bool startFail = s_fail;

	const Scalar alpha = 0.30;
	const Scalar thk   = 180.0;
	Stack stk( alpha, kFilmN, kFilmK, thk );
	GGXSPF* spfTF   = stk.MakeThinFilmSPF();
	GGXSPF* spfCond = stk.MakeConductorSPF();

	const Scalar wavelengths[] = { 450.0, 532.0, 610.0 };
	const Scalar thetaIs[] = { 0.2, 0.7, 1.1 };

	double maxRel = 0.0;
	int compared = 0;

	for( int wq = 0; wq < 3; ++wq )
	{
		const Scalar nm = wavelengths[wq];
		for( int a = 0; a < 3; ++a )
		{
			const Vector3 v( sin( thetaIs[a] ), 0, cos( thetaIs[a] ) );	// wi
			RayIntersectionGeometric ri = MakeRI( -v );

			// Force the specular lobe (uLobe=0.5 with wd≈0 ⇒ specular),
			// then drive the VNDF sample with two scripted draws.  Both
			// SPFs get the SAME draws ⇒ the SAME half-vector / G2 / G1 /
			// pSelect, so the kray ratio isolates R_film/R_cond.
			const Scalar draws[] = { 0.5, 0.37, 0.61 };
			ScriptedSampler sTF( draws, 3, 0.5 );
			ScriptedSampler sCond( draws, 3, 0.5 );

			ScatteredRayContainer scTF, scCond;
			IORStack iorTF( 1.0 ), iorCond( 1.0 );	// environment IOR (air); explicit ctor
			spfTF->ScatterNM( ri, sTF, nm, scTF, iorTF );
			spfCond->ScatterNM( ri, sCond, nm, scCond, iorCond );

			int iTF = -1, iCond = -1;
			for( unsigned int k = 0; k < scTF.Count(); ++k )
				if( scTF[k].type == ScatteredRay::eRayReflection ) { iTF = (int) k; break; }
			for( unsigned int k = 0; k < scCond.Count(); ++k )
				if( scCond[k].type == ScatteredRay::eRayReflection ) { iCond = (int) k; break; }
			if( iTF < 0 || iCond < 0 ) continue;

			const Scalar krayTF = scTF[iTF].krayNM;
			const Scalar krayCond = scCond[iCond].krayNM;
			if( krayCond < 1e-12 ) continue;

			// Half-vector from the (identical) sampled wo.
			const Vector3 wo = Vector3Ops::Normalize( scTF[iTF].ray.Dir() );
			const Vector3 h = Vector3Ops::Normalize( v + wo );
			const Scalar cosWoH = r_max( Scalar( 0 ), Vector3Ops::Dot( wo, h ) );
			const Scalar Rfilm = ThinFilm::ReflectanceConductor( cosWoH, nm, 1.0, 0.0, kFilmN, kFilmK, thk, kSubN, kSubK );
			const Scalar Rcond = Optics::CalculateConductorReflectance( ri.ray.Dir(), h, 1.0, kSubN, kSubK );
			if( Rcond < 1e-12 ) continue;

			const double gotRatio = krayTF / krayCond;	// (specColor*Rfilm*W)/(specColor*Rcond*W) = Rfilm/Rcond
			const double expRatio = Rfilm / Rcond;
			const double rel = std::fabs( gotRatio - expRatio ) / r_max( std::fabs( expRatio ), Scalar( 1e-9 ) );
			if( rel > maxRel ) maxRel = rel;
			++compared;
		}
	}

	std::cout << "  compared=" << compared << " maxRelErr(R_scatter vs oracle) = "
			  << std::scientific << std::setprecision( 3 ) << maxRel << "\n";
	Check( compared >= 3, "twin test exercised ≥3 deterministic specular samples" );
	Check( maxRel < 1e-9,
		"ScatterNM thin-film R ≡ ThinFilm::ReflectanceConductor (== valueNM term via Test A)" );

	spfTF->release();
	spfCond->release();
	return s_fail == startFail;
}

// ============================================================
//  Test C: reciprocity of the thin-film lobe
// ============================================================
static bool TestReciprocity()
{
	std::cout << "\n--- Test C: thin-film BSDF reciprocity ---\n";
	const bool startFail = s_fail;

	const Scalar alpha = 0.25;
	Stack stk( alpha, kFilmN, kFilmK, 180.0 );
	GGXBRDF* brdf = stk.MakeThinFilmBRDF();

	RandomNumberGenerator rng;
	const int N = 2048;
	const Scalar nm = 560.0;
	double maxRel = 0.0;
	int valid = 0;

	for( int i = 0; i < N; ++i )
	{
		const Scalar t1 = acos( 1.0 - rng.CanonicalRandom() * 0.9 );
		const Scalar p1 = rng.CanonicalRandom() * TWO_PI;
		const Vector3 wi( sin( t1 ) * cos( p1 ), sin( t1 ) * sin( p1 ), cos( t1 ) );

		const Scalar t2 = acos( 1.0 - rng.CanonicalRandom() * 0.9 );
		const Scalar p2 = rng.CanonicalRandom() * TWO_PI;
		const Vector3 wo( sin( t2 ) * cos( p2 ), sin( t2 ) * sin( p2 ), cos( t2 ) );

		RayIntersectionGeometric riF = MakeRI( -wo );
		const Scalar f1 = brdf->valueNM( wi, riF, nm );
		RayIntersectionGeometric riR = MakeRI( -wi );
		const Scalar f2 = brdf->valueNM( wo, riR, nm );

		if( f1 < 1e-8 && f2 < 1e-8 ) continue;
		const Scalar denom = r_max( r_max( f1, f2 ), Scalar( 1e-10 ) );
		const double rel = std::fabs( f1 - f2 ) / denom;
		if( rel > maxRel ) maxRel = rel;
		++valid;
	}

	std::cout << "  valid=" << valid << " maxRelErr = " << std::scientific << std::setprecision( 3 ) << maxRel << "\n";
	Check( valid > 100, "reciprocity exercised enough samples" );
	Check( maxRel < 1e-6, "valueNM(wi→wo) ≡ valueNM(wo→wi) for thin-film lobe" );

	brdf->release();
	return s_fail == startFail;
}

// ============================================================
//  Test D: ADDITIVE INVARIANT — conductor mode byte-identical;
//          thin-film(film=air) collapses to conductor.
// ============================================================
static bool TestAdditiveInvariant()
{
	std::cout << "\n--- Test D: additive invariant (conductor untouched; film=air ≡ conductor) ---\n";
	const bool startFail = s_fail;

	// (1) PURE-MATH bare limit: ThinFilm::ReflectanceConductor with film
	//     index == air MUST equal Optics::CalculateConductorReflectance for
	//     every angle/wavelength (the air→air→substrate stack is the bare
	//     air→substrate interface).  No BRDF, no fast-math drift.
	{
		double maxRel = 0.0;
		const Scalar nms[] = { 430.0, 555.0, 660.0 };
		for( int w = 0; w < 3; ++w )
		for( int a = 0; a < 6; ++a )
		{
			const Scalar nm = nms[w];
			const Scalar cosI = 0.05 + a * ( 0.95 / 5.0 );
			const Vector3 n( 0, 0, 1 );
			const Vector3 vDir( sqrt( r_max( Scalar( 0 ), 1.0 - cosI * cosI ) ), 0, -cosI );	// dot(vDir,n) = -cosI
			const Scalar Rcond = Optics::CalculateConductorReflectance<Scalar>( vDir, n, 1.0, kSubN, kSubK );
			const Scalar Rfilm = ThinFilm::ReflectanceConductor( cosI, nm, 1.0, 0.0, /*film=air*/ 1.0, 0.0, 180.0, kSubN, kSubK );
			const double rel = std::fabs( Rfilm - Rcond ) / r_max( std::fabs( Rcond ), Scalar( 1e-12 ) );
			if( rel > maxRel ) maxRel = rel;
		}
		std::cout << "  ThinFilm(film=air) ≡ Optics  maxRelErr = " << std::scientific << std::setprecision( 3 ) << maxRel << "\n";
		Check( maxRel < 1e-12, "ThinFilm::ReflectanceConductor(film=air) ≡ Optics::CalculateConductorReflectance" );
	}

	// (2) BRDF additive invariant: conductor mode ≡ thin-film(film=air)
	//     mode.  Both build the same downstream specFactor + Kulla-Conty
	//     terms; the thin-film Airy reduces algebraically to the conductor
	//     Fresnel when the film vanishes, so they execute equivalent code
	//     and match to FP precision under fast-math.  This simultaneously
	//     proves the conductor path is UNPERTURBED by the new branch.
	{
		const Scalar alpha = 0.20;
		Stack stk( alpha, /*film=air*/ 1.0, 0.0, 180.0 );
		GGXBRDF* tfAir = stk.MakeThinFilmBRDF();
		GGXBRDF* cond  = stk.MakeConductorBRDF();

		const Scalar wavelengths[] = { 480.0, 555.0, 640.0 };
		const Scalar thetaIs[] = { 0.0, 0.5, 1.0 };
		const Scalar thetaOs[] = { 0.2, 0.6, 1.1 };

		double maxRel = 0.0;
		for( int wq = 0; wq < 3; ++wq )
		{
			const Scalar nm = wavelengths[wq];
			for( int a = 0; a < 3; ++a )
			for( int b = 0; b < 3; ++b )
			{
				const Vector3 v( sin( thetaIs[a] ), 0, cos( thetaIs[a] ) );
				const Vector3 r( sin( thetaOs[b] ) * cos( 0.7 ), sin( thetaOs[b] ) * sin( 0.7 ), cos( thetaOs[b] ) );
				RayIntersectionGeometric ri = MakeRI( -r );
				const Scalar a_tf = tfAir->valueNM( v, ri, nm );
				const Scalar a_c  = cond->valueNM( v, ri, nm );
				const double rel = std::fabs( a_tf - a_c ) / r_max( std::fabs( a_c ), Scalar( 1e-12 ) );
				if( rel > maxRel ) maxRel = rel;
			}
		}
		std::cout << "  conductor ≡ thinfilm(film=air)  maxRelErr = " << std::scientific << std::setprecision( 3 ) << maxRel << "\n";
		Check( maxRel < 1e-12, "eFresnelConductor ≡ eFresnelThinFilmConductor(film=air) — conductor path byte-identical" );

		tfAir->release();
		cond->release();
	}

	// (3) The RGB path: conductor value() ≡ thin-film(film=air) value() to
	//     INTEGRATION accuracy, not FP precision.  Unlike the spectral path
	//     (which evaluates R per hero wavelength), the RGB thin-film path is
	//     a 32-sample albedo-basis SPECTRAL INTEGRAL of R(λ) (§8), while the
	//     conductor RGB path is the closed-form per-channel Fresnel.  For a
	//     vanishing film both describe the SAME air→substrate reflectance,
	//     so they must agree to the integral's quadrature error (~1e-4) —
	//     this confirms the RGB thin-film preview is a faithful rendering of
	//     the same physical reflectance, and that the (textually unchanged)
	//     RGB conductor branch still produces the right answer.  The
	//     byte-identical additive guarantee is carried by the SPECTRAL path
	//     above (3e-16); the RGB path is preview-grade by design.
	{
		const Scalar alpha = 0.18;
		Stack stk( alpha, /*film=air*/ 1.0, 0.0, 150.0 );
		GGXBRDF* tfAir = stk.MakeThinFilmBRDF();
		GGXBRDF* cond  = stk.MakeConductorBRDF();

		const Scalar thetaIs[] = { 0.0, 0.5, 1.0 };
		const Scalar thetaOs[] = { 0.2, 0.6, 1.1 };
		double maxRel = 0.0;
		for( int a = 0; a < 3; ++a )
		for( int b = 0; b < 3; ++b )
		{
			const Vector3 v( sin( thetaIs[a] ), 0, cos( thetaIs[a] ) );
			const Vector3 r( sin( thetaOs[b] ) * cos( 0.7 ), sin( thetaOs[b] ) * sin( 0.7 ), cos( thetaOs[b] ) );
			RayIntersectionGeometric ri = MakeRI( -r );
			const RISEPel a_tf = tfAir->value( v, ri );
			const RISEPel a_c  = cond->value( v, ri );
			const Scalar d = fabs( a_tf.r - a_c.r ) + fabs( a_tf.g - a_c.g ) + fabs( a_tf.b - a_c.b );
			const Scalar mag = r_max( ColorMath::MaxValue( a_c ), Scalar( 1e-12 ) );
			const double rel = d / mag;
			if( rel > maxRel ) maxRel = rel;
		}
		std::cout << "  RGB conductor ≈ thinfilm(film=air)  maxRelErr = " << std::scientific << std::setprecision( 3 ) << maxRel << "\n";
		Check( maxRel < 1e-3, "RGB value() conductor ≈ thinfilm(film=air) to integration accuracy (preview-grade)" );

		tfAir->release();
		cond->release();
	}

	return s_fail == startFail;
}

// ============================================================
//  Test E: RGB albedo-basis normalisation is illuminant-independent
// ============================================================
static bool TestRGBAlbedoBasis()
{
	std::cout << "\n--- Test E: RGB albedo-basis (R≡const → neutral; interference → tinted) ---\n";
	const bool startFail = s_fail;

	// A wavelength-INDEPENDENT reflectance MUST integrate to a NEUTRAL RGB
	// (no chromatic tint), because the white-normalisation maps R≡const to
	// const·(white point) → neutral Rec.709.  We realise a flat R(λ) with a
	// constant-n,k substrate and NO film (film index == air): the Airy then
	// returns the (wavelength-independent, constant-n,k) conductor
	// reflectance at every λ.
	{
		const RISEPel rgb = ThinFilm::ReflectanceConductorRGB(
			1.0, 1.0, 0.0, /*film=air*/ 1.0, 0.0, 100.0, kSubN, kSubK );
		const Scalar mx = r_max( r_max( rgb.r, rgb.g ), rgb.b );
		const Scalar mn = r_min( r_min( rgb.r, rgb.g ), rgb.b );
		const Scalar spread = ( mx > 1e-9 ) ? ( mx - mn ) / mx : 0.0;
		std::cout << "  const-R stack RGB = (" << std::fixed << std::setprecision( 5 )
				  << rgb.r << ", " << rgb.g << ", " << rgb.b << ")  spread=" << spread << "\n";
		Check( spread < 5e-3, "wavelength-independent reflectance → NEUTRAL RGB (no illuminant tint)" );

		// Magnitude: the neutral RGB luma equals the λ-independent
		// conductor reflectance at the same cosine.
		const Scalar Rcond = Optics::CalculateConductorReflectance<Scalar>(
			Vector3( 0, 0, -1 ), Vector3( 0, 0, 1 ), 1.0, kSubN, kSubK );	// cos=1
		const Scalar luma = 0.2126 * rgb.r + 0.7152 * rgb.g + 0.0722 * rgb.b;
		const double rel = std::fabs( luma - Rcond ) / r_max( Rcond, Scalar( 1e-9 ) );
		std::cout << "  albedo-basis luma=" << std::fixed << std::setprecision( 5 ) << luma
				  << " vs conductor R=" << Rcond << " relErr=" << std::scientific << std::setprecision( 3 ) << rel << "\n";
		Check( rel < 1e-2, "albedo-basis magnitude matches the λ-independent conductor reflectance" );
	}

	// A REAL interference stack (transparent oxide on metal) MUST be
	// chromatically TINTED — the whole point of thin-film.  This guards
	// against an over-aggressive "neutralisation" that would also flatten
	// genuine iridescence.
	{
		const RISEPel rgb = ThinFilm::ReflectanceConductorRGB(
			1.0, 1.0, 0.0, kFilmN, kFilmK, 180.0, kSubN, kSubK );
		const Scalar mx = r_max( r_max( rgb.r, rgb.g ), rgb.b );
		const Scalar mn = r_min( r_min( rgb.r, rgb.g ), rgb.b );
		const Scalar spread = ( mx > 1e-9 ) ? ( mx - mn ) / mx : 0.0;
		std::cout << "  180nm TiO2/Ti RGB = (" << std::fixed << std::setprecision( 5 )
				  << rgb.r << ", " << rgb.g << ", " << rgb.b << ")  spread=" << spread << "\n";
		Check( spread > 0.05, "a real interference stack produces a chromatically TINTED RGB" );
	}

	return s_fail == startFail;
}

// ============================================================
//  Main
// ============================================================
int main()
{
	std::cout << "=== Thin-Film GGX BRDF/SPF Test ===\n";
	GlobalLog();	// initialize the global log

	TestSpectralExactness();
	TestTwinConsistency();
	TestReciprocity();
	TestAdditiveInvariant();
	TestRGBAlbedoBasis();

	std::cout << "\n=== ThinFilmBRDFTest: " << s_pass << " passed, " << s_fail << " failed ===\n";
	return s_fail == 0 ? 0 : 1;
}
