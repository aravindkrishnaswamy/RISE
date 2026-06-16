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
//        B = f_ms BOTH thickness-independent, R(d) the single-scatter
//        reflectance and F_ms(d) = ComputeFms(specColor·F_avg(d), Eavg) the
//        Kulla-Conty tail built from the TINTED THIN-FILM hemispherical
//        average specColor·F_avg (P2-D + the 2026-06 energy-compensation
//        fix: specColor is INSIDE the nonlinear Fms because the tinted
//        per-bounce reflectance specColor·F_avg compounds across bounces,
//        matching the single-scatter lobe specColor·R; ComputeFms is
//        nonlinear so specColor·ComputeFms(F_avg) ≠ ComputeFms(specColor·
//        F_avg)).  The test solves (A,B) from the best-conditioned thickness
//        pair (R and the tinted F_avg are not collinear — the film shifts the
//        average reflectance differently from the single-scatter one, so the
//        [R F_ms] 2×2 is non-singular), then PREDICTS the other two valueNMs.
//        This FAILS if the tail still used the substrate F_avg OR pulled
//        specColor outside the nonlinear Fms, so it validates the P2-D wiring
//        AND the tinted-average correction — match to ~1e-3 (the -ffast-math
//        reconstruction limit).  A dedicated specColor-varied subtest below
//        pins the tinted-average direction adversarially (new < old).
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
//      F. NULL FILM_EXTINCTION — the documented transparent default (k=0)
//         must not crash and must equal an explicit k=0 painter (own banner
//         above TestNullFilmExtinction).
//      G. TINTED MULTISCATTER ENERGY FIX (2026-06) — the Kulla-Conty tail
//         uses ComputeFms(specColor·F_avg), NOT specColor·ComputeFms(F_avg);
//         since ComputeFms is nonlinear the two differ for specColor<1 and
//         the correct (smaller) form prevents over-bright tinted rough
//         metals.  Adversarial: matches the tinted form, rejects the old
//         form by >5%, and is byte-identical at specColor==1 (own banner
//         above TestSpecColorInsideMultiscatter).
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
#include "../src/Library/Utilities/MicrofacetUtils.h"
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
	// where A = specColor*specFactor and B = f_ms are BOTH thickness-
	// INDEPENDENT (same alpha, same half-vector across thickness),
	// R(d)  = ThinFilm::ReflectanceConductor (the single-scatter lobe), and
	// F_ms(d) = ComputeFms( specColor * ThinFilm::FresnelAvgConductor(d), Eavg )
	// is the Kulla-Conty multiscatter tail evaluated with the TINTED THIN-FILM
	// hemispherical average specColor*F_avg (P2-D + the 2026-06 energy fix:
	// specColor lives INSIDE the nonlinear Fms; see the header note).  With 4
	// thicknesses we have 4 equations in the 2 unknowns (A,B): per cell we
	// solve A,B from the BEST-CONDITIONED thickness pair (the [R F_ms] 2x2
	// must be non-singular -- R and the tinted F_avg are not collinear because
	// the film shifts the average reflectance differently from the single-
	// scatter reflectance), then PREDICT the remaining valueNMs and compare to
	// the library.  Because the multiscatter term carries the tinted thin-film
	// F_avg, this prediction would FAIL if the renderer still used the
	// substrate F_avg in the tail OR pulled specColor outside the nonlinear
	// Fms -- so it validates the P2-D wiring AND the tinted-average correction.
	// Tolerance ~1e-3 is the -ffast-math reconstruction limit (header note).
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

			// The NM path multiplies the lobe by specColor = GetColorNM (the
			// JH-uplifted spectral tint at nm), NOT kSpec — and the multiscatter
			// Fms is NONLINEAR in it, so the tail basis must use this exact value.
			const Scalar specColor = s0.specular->GetColorNM( ri, nm );

			double val[4], R[4], Fms[4];
			for( int k = 0; k < 4; ++k ) {
				val[k] = b[k]->valueNM( v, ri, nm );
				R[k]   = ThinFilm::ReflectanceConductor( cosWoH, nm, 1.0, 0.0, kFilmN, kFilmK, ds[k], kSubN, kSubK );
				const Scalar Favg = ThinFilm::FresnelAvgConductor( nm, 1.0, 0.0, kFilmN, kFilmK, ds[k], kSubN, kSubK );
				// Tinted average: specColor INSIDE the nonlinear Fms (2026-06 fix).
				// The solved B then comes out as f_ms (specColor no longer in B).
				Fms[k] = MicrofacetEnergyLUT::ComputeFms<Scalar>( specColor * Favg, Eavg );
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
//////////////////////////////////////////////////////////////////////
//  Test F: null film_extinction is the documented transparent default
//  (k = 0) and MUST NOT crash.  Regression for the round-1 adversarial P1:
//  the BSDF dereferenced pFilmExtinction unconditionally at 9 sites, so
//  omitting film_extinction (parser-/API-supported, IJob.h doc) segfaulted
//  on the first shade.  A null-film_extinction thin-film BRDF/SPF must (a)
//  evaluate FINITE on value()/valueNM()/albedo()/Scatter()/ScatterNM()
//  without crashing, and (b) equal an explicit k=0 painter on value/valueNM.
//////////////////////////////////////////////////////////////////////
static bool TestNullFilmExtinction()
{
	std::cout << "--- Test F: null film_extinction == explicit k=0, no crash ---\n";
	const int startFail = s_fail;
	
	Stack s( 0.20, kFilmN, /*filmK=*/Scalar( 0 ), Scalar( 150 ) );   // explicit-0 reference
	GGXBRDF* bRef = s.MakeThinFilmBRDF();   // film_extinction = UniformScalarPainter(0)
	// Null-film_extinction twins (the omitted-slot case):
	GGXBRDF* bNull = new GGXBRDF( *s.diffuse, *s.specular, *s.alphaX, *s.alphaY, *s.ior, *s.ext,
		eFresnelThinFilmConductor, nullptr, s.filmIor, /*film_extinction=*/nullptr, s.filmThk );
	bNull->addref();
	GGXSPF* spfNull = new GGXSPF( *s.diffuse, *s.specular, *s.alphaX, *s.alphaY, *s.ior, *s.ext,
		eFresnelThinFilmConductor, nullptr, s.filmIor, /*film_extinction=*/nullptr, s.filmThk );
	spfNull->addref();
	
	const Vector3 v( std::sin( Scalar( 0.5 ) ), Scalar( 0 ), std::cos( Scalar( 0.5 ) ) );
	const Scalar to = Scalar( 0.2 );
	const Vector3 r( std::sin( to ) * std::cos( Scalar( 1 ) ), std::sin( to ) * std::sin( Scalar( 1 ) ), std::cos( to ) );
	RayIntersectionGeometric ri = MakeRI( -r );
	
	// (a) NM path (GGXBRDF sites 352/393): finite + null == explicit-0.
	double maxRelNM = 0.0;
	const Scalar nms[] = { Scalar( 450 ), Scalar( 550 ), Scalar( 650 ) };
	for( int i = 0; i < 3; ++i ) {
		const Scalar a = bRef->valueNM( v, ri, nms[i] );
		const Scalar b = bNull->valueNM( v, ri, nms[i] );   // MUST NOT crash
		Check( std::isfinite( b ), "null-ext valueNM finite" );
		maxRelNM = std::max( maxRelNM, std::fabs( a - b ) / r_max( std::fabs( a ), Scalar( 1e-12 ) ) );
	}
	Check( maxRelNM < 1e-12, "null film_extinction valueNM == explicit k=0 (NM single+multi)" );
	
	// (b) RGB value() (GGXBRDF sites 200/249): finite + null == explicit-0.
	const RISEPel ca = bRef->value( v, ri );
	const RISEPel cb = bNull->value( v, ri );   // MUST NOT crash
	Check( std::isfinite( cb.r ) && std::isfinite( cb.g ) && std::isfinite( cb.b ), "null-ext value() finite" );
	double maxRelRGB = std::fabs( ca.r - cb.r ) / r_max( std::fabs( ca.r ), Scalar( 1e-12 ) );
	maxRelRGB = std::max( maxRelRGB, std::fabs( ca.g - cb.g ) / r_max( std::fabs( ca.g ), Scalar( 1e-12 ) ) );
	maxRelRGB = std::max( maxRelRGB, std::fabs( ca.b - cb.b ) / r_max( std::fabs( ca.b ), Scalar( 1e-12 ) ) );
	Check( maxRelRGB < 1e-9, "null film_extinction value() == explicit k=0 (RGB single+multi)" );
	
	// (c) albedo() AOV (GGXBRDF site 460): finite, no crash.
	const RISEPel alb = bNull->albedo( ri );
	Check( std::isfinite( alb.r ) && std::isfinite( alb.g ) && std::isfinite( alb.b ), "null-ext albedo() finite" );
	
	// (d) SPF Scatter()/ScatterNM() (GGXSPF sites 240/332/484/565): no crash.
	const Scalar script[] = { Scalar( 0.4 ), Scalar( 0.6 ), Scalar( 0.4 ), Scalar( 0.6 ), Scalar( 0.4 ), Scalar( 0.6 ) };
	{
		ScriptedSampler smp( script, 6, Scalar( 0.5 ) );
		ScatteredRayContainer sc;
		IORStack ist( Scalar( 1 ) );
		spfNull->ScatterNM( ri, smp, Scalar( 550 ), sc, ist );   // MUST NOT crash
	}
	{
		ScriptedSampler smp( script, 6, Scalar( 0.5 ) );
		ScatteredRayContainer sc;
		IORStack ist( Scalar( 1 ) );
		spfNull->Scatter( ri, smp, sc, ist );   // MUST NOT crash
	}
	Check( true, "null-ext Scatter/ScatterNM did not crash" );
	
	bRef->release(); bNull->release(); spfNull->release();
	return s_fail == startFail;
}

// ============================================================
//  Test G: specColor-INSIDE the Kulla-Conty multiscatter Fms
//          (the 2026-06 tinted energy-compensation fix).
// ============================================================
//  The multiscatter tail models a geometric series of bounces, each
//  attenuated by the per-bounce reflectance.  For a TINTED lobe that
//  reflectance is specColor*F_avg (the tint compounds across bounces),
//  and ComputeFms is NONLINEAR in its argument, so the tail must be
//      ComputeFms(specColor*F_avg, Eavg) * f_ms          (NEW, correct)
//  NOT
//      specColor * ComputeFms(F_avg, Eavg) * f_ms        (OLD, over-bright)
//  These are EQUAL only at specColor==1 (ComputeFms(1*F)==1*ComputeFms(F)),
//  and the OLD form is strictly LARGER for specColor<1 (tinted) because
//  Fms(s*F) = (s*F)^2*Eavg/(1-s*F*(1-Eavg)) is sub-linear in s.
//
//  Isolation: at a FIXED geometry, valueNM = singleScatter + multiScatter,
//  where singleScatter = specColor*R*specFactor (R the EXACT thin-film
//  single-scatter reflectance, pinned by Test B; specFactor = D*G2/(4 nv nr)
//  recomputed through the SAME onb the library uses).  Subtract the
//  reconstructed single-scatter from the library valueNM to recover the
//  multiscatter term, then compare to NEW vs OLD.  Run base tint rs in
//  {0.25,0.5,1.0} at one rough alpha + one thickness.  IMPORTANT: the NM
//  shading path multiplies by specColor = pSpecular->GetColorNM (the
//  JH-uplifted spectral value of the RGB tint, NONLINEAR in rs), so the
//  test queries that exact specColor and uses it in BOTH the single-scatter
//  reconstruction and the NEW/OLD formulas.  Assert (a) multi ≈ NEW tightly
//  for every rs, (b) multi ≠ OLD for rs<1 (the test has TEETH — it would
//  catch the pre-fix code), (c) NEW ≈ OLD ≈ measured for rs≈1 (untinted is
//  unchanged), and (d) the physics direction NEW < OLD for rs<1 (tinted
//  multiscatter DECREASES).
static bool TestSpecColorInsideMultiscatter()
{
	std::cout << "\n--- Test G: specColor INSIDE Kulla-Conty Fms (tinted energy fix) ---\n";
	const bool startFail = s_fail;

	const Scalar alpha = 0.50;	// rough — the multiscatter tail carries real weight
	const Scalar thk   = 180.0;
	const Scalar nm    = 560.0;
	const Scalar Eavg  = MicrofacetEnergyLUT::LookupEavg( alpha );

	// Fixed geometry (same family as Test A: a small-angle view, moderate wi).
	const Vector3 v( sin( Scalar( 0.4 ) ), 0, cos( Scalar( 0.4 ) ) );	// wi (light)
	const Scalar to = 0.15;
	const Vector3 r( sin( to ) * cos( 1.0 ), sin( to ) * sin( 1.0 ), cos( to ) );	// view dir
	RayIntersectionGeometric ri = MakeRI( -r );

	// Recompute specFactor EXACTLY as valueNM does, projecting through the
	// SAME onb (ri.onb was CreateFromW(+Z); no anisotropy rotation here so
	// effOnb == ri.onb).  alpha is isotropic so alphaX==alphaY==alpha.
	const OrthonormalBasis3D& onb = ri.onb;
	const Vector3 n = onb.w();
	const Scalar nr = Vector3Ops::Dot( n, r );
	const Scalar nv = Vector3Ops::Dot( n, v );
	const Vector3 h = Vector3Ops::Normalize( v + r );
	const Vector3 h_local( Vector3Ops::Dot( h, onb.u() ), Vector3Ops::Dot( h, onb.v() ), Vector3Ops::Dot( h, onb.w() ) );
	const Vector3 wi_local( Vector3Ops::Dot( v, onb.u() ), Vector3Ops::Dot( v, onb.v() ), Vector3Ops::Dot( v, onb.w() ) );
	const Vector3 wo_local( Vector3Ops::Dot( r, onb.u() ), Vector3Ops::Dot( r, onb.v() ), Vector3Ops::Dot( r, onb.w() ) );
	const Scalar D  = MicrofacetUtils::GGX_D_Aniso<Scalar>( alpha, alpha, h_local );
	const Scalar G2 = MicrofacetUtils::GGX_G2_Aniso( alpha, alpha, wi_local, wo_local );
	const Scalar specFactor = D * G2 / ( 4.0 * nv * nr );

	// Single-scatter reflectance (exact thin-film Airy; cosWoH = dot(r,h)).
	const Scalar cosWoH = r_max( Scalar( 0 ), Vector3Ops::Dot( r, h ) );
	const Scalar Rfilm  = ThinFilm::ReflectanceConductor( cosWoH, nm, 1.0, 0.0, kFilmN, kFilmK, thk, kSubN, kSubK );

	// Thin-film hemispherical F_avg feeding the multiscatter tail.
	const Scalar Favg = ThinFilm::FresnelAvgConductor( nm, 1.0, 0.0, kFilmN, kFilmK, thk, kSubN, kSubK );

	// f_ms = (1-Ess_o)(1-Ess_i)/(PI(1-Eavg)); valueNM ADDS F_ms * f_ms.
	const Scalar Ess_o = MicrofacetEnergyLUT::LookupEss( nr, alpha );
	const Scalar Ess_i = MicrofacetEnergyLUT::LookupEss( nv, alpha );
	const Scalar f_ms  = ( 1.0 - Ess_o ) * ( 1.0 - Ess_i ) / ( PI * ( 1.0 - Eavg ) );

	const Scalar rsList[] = { 0.25, 0.5, 1.0 };
	double maxRelNew = 0.0;	// worst FULL-model rel err (full vs single+NEW multi)
	int    teethCount = 0;	// rs<1 cells whose measured multi is FAR from OLD
	int    rsLt1Count = 0;	// total rs<1 cells
	bool   identityRs1 = false;	// at rs≈1, NEW ≈ OLD ≈ measured
	bool   directionOk = true;	// NEW < OLD for rs<1

	for( int q = 0; q < 3; ++q )
	{
		const Scalar rs = rsList[q];

		// Build a thin-film GGXBRDF with this base specular tint (rs) — uniform
		// stack.  NOTE: the NM shading path reads pSpecular->GetColorNM, which
		// is the JH-uplifted spectral value of the RGB tint at nm, NOT rs
		// itself (and it is NONLINEAR in rs — uplift(0.25·white) ≠ 0.25·uplift
		// (white)).  We must therefore drive BOTH the single-scatter
		// reconstruction AND the NEW/OLD formulas with the SAME specColor the
		// library sees (queried below), or the reconstruction is inconsistent.
		UniformColorPainter*  diffuse  = new UniformColorPainter( RISEPel( 0, 0, 0 ) ); diffuse->addref();
		UniformColorPainter*  specular = new UniformColorPainter( RISEPel( rs, rs, rs ) ); specular->addref();
		UniformScalarPainter* aX = new UniformScalarPainter( alpha ); aX->addref();
		UniformScalarPainter* aY = new UniformScalarPainter( alpha ); aY->addref();
		UniformScalarPainter* ior = new UniformScalarPainter( kSubN ); ior->addref();
		UniformScalarPainter* ext = new UniformScalarPainter( kSubK ); ext->addref();
		UniformScalarPainter* fIor = new UniformScalarPainter( kFilmN ); fIor->addref();
		UniformScalarPainter* fExt = new UniformScalarPainter( kFilmK ); fExt->addref();
		UniformScalarPainter* fThk = new UniformScalarPainter( thk ); fThk->addref();
		GGXBRDF* brdf = new GGXBRDF( *diffuse, *specular, *aX, *aY, *ior, *ext,
			eFresnelThinFilmConductor, nullptr, fIor, fExt, fThk );
		brdf->addref();

		// The EXACT per-λ specColor the library multiplies into the lobe.
		const Scalar specColor = specular->GetColorNM( ri, nm );

		const Scalar full = brdf->valueNM( v, ri, nm );

		// Isolate the multiscatter term: subtract the reconstructed
		// single-scatter lobe specColor*Rfilm*specFactor (specColor == the
		// uplifted value, matching the library's single-scatter exactly).
		const Scalar singleScatter = specColor * Rfilm * specFactor;
		const Scalar multi = full - singleScatter;

		// NEW (correct): specColor INSIDE the nonlinear Fms.
		const Scalar fmsNew = MicrofacetEnergyLUT::ComputeFms<Scalar>( specColor * Favg, Eavg );
		const Scalar multiNew = fmsNew * f_ms;
		// OLD (buggy): specColor pulled OUTSIDE the nonlinear Fms.
		const Scalar fmsOld = MicrofacetEnergyLUT::ComputeFms<Scalar>( Favg, Eavg );
		const Scalar multiOld = specColor * fmsOld * f_ms;

		// (1) STRONG correctness pin, measured on the FULL valueNM (so the
		//     single-scatter subtraction does NOT amplify fast-math noise):
		//     full == singleScatter + multiNew to the fast-math floor.
		const Scalar predFullNew = singleScatter + multiNew;
		const double relFullNew  = std::fabs( full - predFullNew ) / r_max( std::fabs( full ), Scalar( 1e-12 ) );
		if( relFullNew > maxRelNew ) maxRelNew = relFullNew;

		// (2) TEETH, measured on the ISOLATED multiscatter term (single-scatter
		//     subtracted): how far the measured tail sits from the OLD formula.
		const double relMultiNew = std::fabs( multi - multiNew ) / r_max( std::fabs( multiNew ), Scalar( 1e-12 ) );
		const double relMultiOld = std::fabs( multi - multiOld ) / r_max( std::fabs( multiOld ), Scalar( 1e-12 ) );

		std::cout << "  rs=" << std::fixed << std::setprecision( 2 ) << rs
				  << " specColor(560nm)=" << std::setprecision( 4 ) << specColor
				  << "  full=" << std::scientific << std::setprecision( 4 ) << full
				  << " (relFullNew " << relFullNew << ")"
				  << "  multi=" << multi << "  NEW=" << multiNew << " (rel " << relMultiNew << ")"
				  << "  OLD=" << multiOld << " (rel " << relMultiOld << ")\n";

		if( rs < 0.999 ) {
			++rsLt1Count;
			// Teeth: the measured (isolated) multiscatter must be FAR from the
			// OLD formula (the test would have FAILED against the pre-fix
			// renderer).  The OLD/NEW gap grows as specColor shrinks — require
			// the measured tail to sit >5% away from OLD AND much closer to NEW.
			if( relMultiOld > 0.05 && relMultiNew < relMultiOld * 0.25 ) ++teethCount;
			// Physics direction: tinted multiscatter DECREASES (NEW < OLD).
			if( !( multiNew < multiOld ) ) directionOk = false;
		} else {
			// (Near-)untinted: ComputeFms(s*F) -> s*ComputeFms(F) as s->1, so
			// NEW and OLD converge; assert they agree (specColor≈1) — this is
			// the "rs=1 unchanged" guarantee that the fix is byte-identical on
			// white materials.
			const double relNewOld = std::fabs( multiNew - multiOld ) / r_max( std::fabs( multiNew ), Scalar( 1e-12 ) );
			identityRs1 = ( relNewOld < 1e-2 ) && ( relMultiNew < 1.5e-2 );
		}

		brdf->release();
		diffuse->release(); specular->release(); aX->release(); aY->release();
		ior->release(); ext->release(); fIor->release(); fExt->release(); fThk->release();
	}

	// The STRONG correctness pin is measured on the FULL valueNM (single +
	// NEW multi), where the single-scatter is NOT subtracted, so it holds to
	// the ~1e-3 -ffast-math reconstruction floor (header methodology note).
	Check( maxRelNew < 3e-3,
		"valueNM == singleScatter + ComputeFms(specColor*F_avg)*f_ms for all rs (full-model fast-math floor)" );
	Check( teethCount == rsLt1Count && rsLt1Count >= 2,
		"the subtest has TEETH: isolated multiscatter is >5% from OLD specColor*ComputeFms(F_avg) and >=4x closer to NEW, for every rs<1" );
	Check( directionOk,
		"physics direction: tinted multiscatter DECREASES (NEW < OLD) for rs<1" );
	Check( identityRs1,
		"(near-)untinted (rs≈1) multiscatter unchanged: NEW ≈ OLD ≈ measured" );

	return s_fail == startFail;
}

int main()
{
	std::cout << "=== Thin-Film GGX BRDF/SPF Test ===\n";
	GlobalLog();	// initialize the global log

	TestSpectralExactness();
	TestTwinConsistency();
	TestReciprocity();
	TestAdditiveInvariant();
	TestRGBAlbedoBasis();
	TestNullFilmExtinction();
	TestSpecColorInsideMultiscatter();

	std::cout << "\n=== ThinFilmBRDFTest: " << s_pass << " passed, " << s_fail << " failed ===\n";
	return s_fail == 0 ? 0 : 1;
}
