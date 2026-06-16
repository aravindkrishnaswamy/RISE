//////////////////////////////////////////////////////////////////////
//
//  ThinFilmProductionTest.cpp - Validates the PRODUCTION thin-film
//    reflectance evaluator (src/Library/Utilities/ThinFilm.h, the lifted
//    Phase-2 evaluator) against the Phase-1 ground-truth oracle
//    (tests/thinfilm/AiryReference.h single-film Airy +
//    tests/thinfilm/TmmReference.h N-layer TMM) and against RISE's own
//    conductor Fresnel.
//
//    The production evaluator is the SAME algorithm as the oracle,
//    re-expressed in RISE Scalar / std::complex<Scalar>, so it must agree
//    to ~machine epsilon -- if it only matched LOSSLESS stacks, the §5
//    sign-convention port (forward-root cosθ branch, -i sinδ matrix,
//    e^{+2iδ} Airy factor) would be wrong.  The grid therefore deliberately
//    INCLUDES an absorbing film (k>0) AND a conductor (absorbing)
//    substrate, the regime that the sign bug blows up on.
//
//    Assertions:
//      1. Production ReflectanceConductor ≡ Airy oracle ≡ TMM oracle
//         across a (λ × θ∈[0,80°] × thickness∈[5,400]nm × stack) grid,
//         including a TiO2-on-Ti (transparent film, conductor substrate),
//         an absorbing-film-on-conductor, and a lossless dielectric AR
//         stack, to tol ~1e-12.
//      2. Bare-substrate limit (d -> 0) ≡
//         Optics::CalculateConductorReflectance for a conductor substrate,
//         across angles.
//      3. Energy / sanity: R ∈ [0,1], finite (no NaN/Inf) for every
//         sampled (λ,θ,d) including absorbing cases.
//      4. Quarter-wave AR exact closed form (independent of the oracle):
//         a lossless n1 on lossless n2 in air at d = λ0/(4 n1), normal
//         incidence, gives R = ((n0 n2 - n1²)/(n0 n2 + n1²))², and R = 0
//         when n1 = sqrt(n0 n2).
//      5. N-layer path (ReflectanceConductorStack with nFilms == 1) ≡ the
//         single-film Airy path, so the documented multi-oxide extension
//         shares the validated single-film result.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#define _USE_MATH_DEFINES
#include <cmath>
#include <cstdio>
#include <iostream>
#include <complex>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "../src/Library/Utilities/ThinFilm.h"

#include "thinfilm/ThinFilmStack.h"
#include "thinfilm/TmmReference.h"
#include "thinfilm/AiryReference.h"

#include "../src/Library/Utilities/Optics.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"

using namespace RISE;

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

	const double kDeg = M_PI / 180.0;

	// A named (n,k) pair, used to build both an oracle Stack and a
	// production call with matching indices.
	struct Medium { double n; double k; };

	struct StackDesc
	{
		const char*	name;
		Medium		ambient;	// N0
		Medium		film;		// N1
		Medium		substrate;	// Ns
	};
}

int main()
{
	std::cout << "ThinFilmProductionTest -- production ThinFilm.h vs Phase-1 oracle\n";

	//------------------------------------------------------------------
	// [1/5] Production ≡ Airy oracle ≡ TMM oracle across a grid that
	//        includes absorbing films and conductor substrates.
	//------------------------------------------------------------------
	std::cout << "\n[1/5] Production ReflectanceConductor == Airy == TMM (incl. absorbing film + conductor substrate)\n";
	{
		// Stacks chosen to exercise every branch of the convention port:
		//  - TiO2 (transparent) on Ti (absorbing conductor): the canonical
		//    heat-tint case; film k=0, substrate k>0.
		//  - Absorbing oxide (Fe3O4-like, k>0) on an Fe-like conductor:
		//    BOTH film and substrate absorbing -- the regime the e^{+2iδ}
		//    sign bug makes diverge.
		//  - Lossless dielectric film on a lossless dielectric substrate
		//    (an AR-coating-like stack): pure-real indices, the easy case
		//    that a wrong port might still pass -- kept so a regression that
		//    only breaks the absorbing path is visibly localized.
		//  - High-index absorbing film on air-like low substrate: stress
		//    the admittance ratios.
		const StackDesc stacks[] = {
			{ "TiO2/Ti (transparent film, conductor sub)", {1.0,0.0}, {2.40,0.0},  {2.50,3.00} },
			{ "Fe3O4/Fe (absorbing film + conductor sub)", {1.0,0.0}, {2.30,0.35}, {2.90,2.95} },
			{ "lossless AR (dielectric/dielectric)",        {1.0,0.0}, {1.38,0.0},  {1.52,0.00} },
			{ "absorbing film / low substrate",             {1.0,0.0}, {2.80,0.80}, {1.45,0.00} },
		};

		const double lambdas[]    = { 380.0, 440.0, 500.0, 550.0, 620.0, 700.0, 780.0 };
		const double thetaDeg[]   = { 0.0, 5.0, 15.0, 30.0, 45.0, 60.0, 70.0, 80.0 };
		const double thicknessNm[]= { 5.0, 20.0, 60.0, 120.0, 200.0, 300.0, 400.0 };

		// Same algorithm, double precision; the only intentional
		// difference is the Snell invariant built as N0*sqrt(1-cos²θ)
		// (production hot path) vs N0*sin(θ) (oracle), which differ by
		// ~1e-16.  1e-12 is the gate; agreement is typically ~1e-15.
		const double tol = 1e-12;
		double worstAiry = 0.0;
		double worstTmm  = 0.0;
		int    nChecked  = 0;

		for( size_t si = 0; si < sizeof(stacks)/sizeof(stacks[0]); ++si ) {
			const StackDesc& sd = stacks[si];
			for( size_t li = 0; li < sizeof(lambdas)/sizeof(lambdas[0]); ++li ) {
				for( size_t ti = 0; ti < sizeof(thetaDeg)/sizeof(thetaDeg[0]); ++ti ) {
					const double theta = thetaDeg[ti] * kDeg;
					const double cosT  = std::cos( theta );
					for( size_t di = 0; di < sizeof(thicknessNm)/sizeof(thicknessNm[0]); ++di ) {
						const double d = thicknessNm[di];

						// Oracle stack (uses the validated reference code).
						const ThinFilmReference::Stack oracleStack =
							ThinFilmReference::MakeSingleFilmStack(
								ThinFilmReference::MakeIndex( sd.ambient.n, sd.ambient.k ),
								ThinFilmReference::MakeIndex( sd.film.n,    sd.film.k ),
								d,
								ThinFilmReference::MakeIndex( sd.substrate.n, sd.substrate.k ) );

						const double Rairy =
							ThinFilmReference::AiryReflectanceUnpolarized( oracleStack, lambdas[li], theta );
						const double Rtmm =
							ThinFilmReference::TmmReflectanceUnpolarized( oracleStack, lambdas[li], theta );

						// Production evaluator (the code under test).
						const Scalar Rprod = ThinFilm::ReflectanceConductor(
							Scalar(cosT), Scalar(lambdas[li]),
							Scalar(sd.ambient.n),   Scalar(sd.ambient.k),
							Scalar(sd.film.n),      Scalar(sd.film.k),
							Scalar(d),
							Scalar(sd.substrate.n), Scalar(sd.substrate.k) );

						const double dAiry = std::fabs( double(Rprod) - Rairy );
						const double dTmm  = std::fabs( double(Rprod) - Rtmm );
						worstAiry = std::max( worstAiry, dAiry );
						worstTmm  = std::max( worstTmm,  dTmm );
						++nChecked;

						Check( dAiry < tol, "production == Airy oracle (unpolarized)" );
						// The bare-Airy and TMM oracles themselves agree to
						// ~1e-15; production must track BOTH.  Use a slightly
						// looser gate for TMM since it is a different algebra
						// (matrix product) accumulating more round-off.
						Check( dTmm < 1e-11, "production == TMM oracle (unpolarized)" );
					}
				}
			}
		}
		std::printf( "  checked %d (stack x lambda x theta x thickness) points\n", nChecked );
		std::printf( "  worst |prod - Airy| = %.3e (tol %.0e)\n", worstAiry, tol );
		std::printf( "  worst |prod - TMM | = %.3e (tol %.0e)\n", worstTmm, 1e-11 );
	}

	//------------------------------------------------------------------
	// [2/5] Bare-substrate limit (d -> 0) == RISE conductor Fresnel.
	//------------------------------------------------------------------
	std::cout << "\n[2/5] Bare-substrate limit (d->0) == Optics::CalculateConductorReflectance (conductor)\n";
	{
		// Synthetic conductor substrate, air ambient, no film (d = 0).
		const double nSub = 2.5, kSub = 3.0;

		// Optics::CalculateConductorReflectance takes vectors: cos = |dot(v,n)|.
		// Build n=+Y, v=(sinθ,-cosθ,0) so |dot(v,n)| = cosθ == our incidence
		// cosine.  Both compute unpolarized R.  Away from grazing they agree
		// to ~1e-12; Optics clamps cos at NEARZERO and uses a slightly
		// different real-ambient algebra, so allow modest slack near grazing.
		const Vector3 nrm( 0, 1, 0 );
		const double tol = 1e-7;
		double worst = 0.0;

		const double angsDeg[] = { 0, 10, 20, 30, 40, 50, 60, 70, 80, 85, 89 };
		const double lambdas[] = { 380, 550, 780 };	// must be wavelength-independent with no film

		for( size_t ai = 0; ai < sizeof(angsDeg)/sizeof(angsDeg[0]); ++ai ) {
			const double theta = angsDeg[ai] * kDeg;
			const double cosT  = std::cos( theta );
			const Vector3 v( std::sin( theta ), -std::cos( theta ), 0 );

			const Scalar Roptics = Optics::CalculateConductorReflectance<Scalar>(
				v, nrm, Scalar(1.0), Scalar(nSub), Scalar(kSub) );

			for( size_t li = 0; li < sizeof(lambdas)/sizeof(lambdas[0]); ++li ) {
				// d = 0: the film index is irrelevant (zero thickness), but
				// pass a representative oxide to confirm thickness, not index,
				// is what collapses the film.
				const Scalar Rprod = ThinFilm::ReflectanceConductor(
					Scalar(cosT), Scalar(lambdas[li]),
					Scalar(1.0), Scalar(0.0),			// air
					Scalar(2.40), Scalar(0.0),			// (ignored at d=0)
					Scalar(0.0),						// d -> 0
					Scalar(nSub), Scalar(kSub) );

				const double diff = std::fabs( double(Roptics) - double(Rprod) );
				worst = std::max( worst, diff );
				Check( diff < tol, "bare (d=0) production == Optics conductor Fresnel" );
			}
		}
		std::printf( "  worst |prod(d=0) - Optics| over angles: %.3e (tol %.0e)\n", worst, tol );
	}

	//------------------------------------------------------------------
	// [3/5] Energy / sanity: R in [0,1] and finite for all samples,
	//        including the absorbing-film + conductor-substrate grid.
	//------------------------------------------------------------------
	std::cout << "\n[3/5] Energy: R in [0,1], finite (incl. absorbing film + conductor substrate)\n";
	{
		// Re-use the most adversarial stack (both film and substrate
		// absorbing) plus the transparent canonical case, swept fine over
		// thickness and angle including near-grazing.
		const StackDesc stacks[] = {
			{ "Fe3O4/Fe (both absorbing)",     {1.0,0.0}, {2.30,0.35}, {2.90,2.95} },
			{ "TiO2/Ti (transparent/conductor)", {1.0,0.0}, {2.40,0.0},  {2.50,3.00} },
			{ "thick strong absorber",         {1.0,0.0}, {1.80,3.50}, {2.50,3.00} },
		};
		int n = 0;
		// Detection by COMPARISON, not std::isfinite: the RISE build
		// mandates -ffast-math, under which std::isfinite is folded to
		// true and never fires (see ThinFilmAnodizeSwatchTest.cpp and
		// src/Library/Utilities/RasterSanityScan.h for the same rationale).
		// The negated in-range test  !(R >= 0 && R <= 1)  rejects NaN and
		// +/-Inf (every comparison with NaN is false) AS WELL AS any value
		// that escaped the evaluator's [0,1] clamp, and stays effective
		// under -ffast-math.
		bool allFiniteInRange = true;
		double rmin = 1e9, rmax = -1e9;

		for( size_t si = 0; si < sizeof(stacks)/sizeof(stacks[0]); ++si ) {
			const StackDesc& sd = stacks[si];
			for( double lam = 380.0; lam <= 780.0; lam += 20.0 ) {
				for( double thetaDeg = 0.0; thetaDeg <= 89.0; thetaDeg += 7.0 ) {
					const double cosT = std::cos( thetaDeg * kDeg );
					for( double d = 0.0; d <= 1000.0; d += 17.0 ) {
						const double R = double( ThinFilm::ReflectanceConductor(
							Scalar(cosT), Scalar(lam),
							Scalar(sd.ambient.n), Scalar(sd.ambient.k),
							Scalar(sd.film.n),    Scalar(sd.film.k),
							Scalar(d),
							Scalar(sd.substrate.n), Scalar(sd.substrate.k) ) );
						if( !( R >= 0.0 && R <= 1.0 ) ) allFiniteInRange = false;
						if( R < rmin ) rmin = R;
						if( R > rmax ) rmax = R;
						++n;
					}
				}
			}
		}
		Check( allFiniteInRange, "all R finite (no NaN/Inf) and in [0,1]" );
		std::printf( "  swept %d points; R range observed [%.6f, %.6f]\n", n, rmin, rmax );

		// Grazing boundary: EXACTLY cosθ = 0 is the documented θ = 90°
		// degeneracy (η_p = N/cosθ -> Inf, r_ab -> Inf/Inf = NaN in the raw
		// math).  The evaluator clamps the cosine to NEARZERO so it returns
		// the well-conditioned physical LIMIT (R -> 1) instead of NaN/0,
		// even if a caller passes cosθ = 0.  Verify: (a) cosθ = 0 yields a
		// finite R in [0,1]; (b) R -> 1 monotonically as cosθ -> 0.  Uses
		// comparison-based detection (effective under -ffast-math).
		{
			const double Rgraze = double( ThinFilm::ReflectanceConductor(
				Scalar(0.0), Scalar(550.0),
				Scalar(1.0), Scalar(0.0),
				Scalar(2.40), Scalar(0.0),
				Scalar(120.0),
				Scalar(2.50), Scalar(3.00) ) );
			Check( Rgraze >= 0.0 && Rgraze <= 1.0,
			       "grazing cosθ=0 gives a finite R in [0,1] (no NaN)" );

			const double R1   = double( ThinFilm::ReflectanceConductor(
				Scalar(0.10),   Scalar(550.0), Scalar(1.0),Scalar(0.0), Scalar(2.40),Scalar(0.0), Scalar(120.0), Scalar(2.50),Scalar(3.00) ) );
			const double R01  = double( ThinFilm::ReflectanceConductor(
				Scalar(0.01),   Scalar(550.0), Scalar(1.0),Scalar(0.0), Scalar(2.40),Scalar(0.0), Scalar(120.0), Scalar(2.50),Scalar(3.00) ) );
			const double R001 = double( ThinFilm::ReflectanceConductor(
				Scalar(0.001),  Scalar(550.0), Scalar(1.0),Scalar(0.0), Scalar(2.40),Scalar(0.0), Scalar(120.0), Scalar(2.50),Scalar(3.00) ) );
			Check( R001 > R01 && R01 > R1,
			       "R increases toward grazing (R -> 1 limit)" );
			Check( R001 > 0.99,
			       "R is near 1 at near-grazing (cosθ=0.001)" );
			std::printf( "  grazing limit: R(cos=0)=%.6f  R(0.001)=%.6f  R(0.01)=%.6f  R(0.1)=%.6f\n",
				Rgraze, R001, R01, R1 );
		}
	}

	//------------------------------------------------------------------
	// [4/5] Quarter-wave AR exact closed form (independent of the oracle).
	//------------------------------------------------------------------
	std::cout << "\n[4/5] Quarter-wave AR exact closed form at normal incidence\n";
	{
		// Lossless n1 film on lossless n2 substrate in air (n0=1), thickness
		// d = lambda0/(4 n1) at normal incidence:
		//   R = ((n0 n2 - n1^2)/(n0 n2 + n1^2))^2
		// and R == 0 exactly when n1 = sqrt(n0 n2).
		const double lambda0 = 550.0;
		const double n0 = 1.0;

		// Case A: a generic quarter-wave stack (n1 != sqrt(n0 n2)).
		{
			const double n1 = 1.45, n2 = 2.30;
			const double d  = lambda0 / (4.0 * n1);
			const double Rexpect = std::pow( (n0*n2 - n1*n1)/(n0*n2 + n1*n1), 2.0 );
			const Scalar Rprod = ThinFilm::ReflectanceConductor(
				Scalar(1.0), Scalar(lambda0),
				Scalar(n0), Scalar(0.0),
				Scalar(n1), Scalar(0.0),
				Scalar(d),
				Scalar(n2), Scalar(0.0) );
			const double diff = std::fabs( double(Rprod) - Rexpect );
			std::printf( "  generic QW: R_prod=%.10f R_expect=%.10f diff=%.3e\n",
				double(Rprod), Rexpect, diff );
			Check( diff < 1e-12, "quarter-wave AR matches closed form" );
		}

		// Case B: perfect AR, n1 = sqrt(n0 n2) -> R == 0.
		{
			const double n2 = 2.25;
			const double n1 = std::sqrt( n0 * n2 );	// 1.5
			const double d  = lambda0 / (4.0 * n1);
			const Scalar Rprod = ThinFilm::ReflectanceConductor(
				Scalar(1.0), Scalar(lambda0),
				Scalar(n0), Scalar(0.0),
				Scalar(n1), Scalar(0.0),
				Scalar(d),
				Scalar(n2), Scalar(0.0) );
			std::printf( "  perfect AR (n1=sqrt(n0 n2)): R_prod=%.3e (expect ~0)\n", double(Rprod) );
			Check( double(Rprod) < 1e-12, "perfect quarter-wave AR gives R == 0" );
		}
	}

	//------------------------------------------------------------------
	// [5/5] N-layer path (nFilms==1) == single-film Airy path.
	//------------------------------------------------------------------
	std::cout << "\n[5/5] ReflectanceConductorStack(nFilms=1) == ReflectanceConductor (single film)\n";
	{
		// The general characteristic-matrix path with one film must reduce
		// to the Airy single-film result -- this is the same TMM<->Airy
		// agreement the oracle relies on, re-checked inside production so the
		// multi-oxide extension point shares the validated result.
		const StackDesc stacks[] = {
			{ "TiO2/Ti",  {1.0,0.0}, {2.40,0.0},  {2.50,3.00} },
			{ "Fe3O4/Fe", {1.0,0.0}, {2.30,0.35}, {2.90,2.95} },
		};
		const double lambdas[]   = { 420.0, 550.0, 680.0 };
		const double thetaDeg[]  = { 0.0, 25.0, 55.0, 78.0 };
		const double thicknessNm[]= { 15.0, 90.0, 230.0, 380.0 };

		double worst = 0.0;
		int n = 0;
		for( size_t si = 0; si < sizeof(stacks)/sizeof(stacks[0]); ++si ) {
			const StackDesc& sd = stacks[si];
			const ThinFilm::Complex N0( Scalar(sd.ambient.n), Scalar(sd.ambient.k) );
			const ThinFilm::Complex Ns( Scalar(sd.substrate.n), Scalar(sd.substrate.k) );
			for( size_t li = 0; li < sizeof(lambdas)/sizeof(lambdas[0]); ++li ) {
				for( size_t ti = 0; ti < sizeof(thetaDeg)/sizeof(thetaDeg[0]); ++ti ) {
					const double cosT = std::cos( thetaDeg[ti] * kDeg );
					for( size_t di = 0; di < sizeof(thicknessNm)/sizeof(thicknessNm[0]); ++di ) {
						const double d = thicknessNm[di];

						const ThinFilm::Complex filmIdx[1] = {
							ThinFilm::Complex( Scalar(sd.film.n), Scalar(sd.film.k) ) };
						const Scalar filmThk[1] = { Scalar(d) };

						const Scalar Rstack = ThinFilm::ReflectanceConductorStack(
							Scalar(cosT), Scalar(lambdas[li]),
							N0, filmIdx, filmThk, 1, Ns );

						const Scalar Rairy = ThinFilm::ReflectanceConductor(
							Scalar(cosT), Scalar(lambdas[li]),
							N0, ThinFilm::Complex( Scalar(sd.film.n), Scalar(sd.film.k) ),
							Scalar(d), Ns );

						const double diff = std::fabs( double(Rstack) - double(Rairy) );
						worst = std::max( worst, diff );
						++n;
						Check( diff < 1e-11, "N-layer(nFilms=1) == single-film Airy" );
					}
				}
			}
		}
		std::printf( "  checked %d points; worst |stack - airy| = %.3e (tol %.0e)\n", n, worst, 1e-11 );
	}

	std::cout << "\nResults: " << s_pass << " passed, " << s_fail << " failed.\n";
	return ( s_fail == 0 ) ? 0 : 1;
}
