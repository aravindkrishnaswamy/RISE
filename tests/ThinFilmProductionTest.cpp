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
#include <limits>

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
		// Detection by COMPARISON, not std::isfinite.  (Historically this was
		// REQUIRED: under -ffinite-math-only std::isfinite folded to true and
		// never fired.  macOS pairs -fno-finite-math-only since 2026-07-29, so
		// isfinite works -- the comparison form is kept because it also
		// catches out-of-[0,1] values, which isfinite would not.)
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
		// math -- no longer true since the 2026-07-29 reformulation, which
		// returns R -> 1 directly at cosθ = 0).  The evaluator clamps the
		// cosine to kGrazingCosFloor (1e-6, NOT NEARZERO/1e-12 -- see
		// ThinFilm.h) so it returns
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

	// ------------------------------------------------------------------
	// Degenerate cos == 0: the SHIPPED single-film path must stay finite
	// (2026-07-30).
	//
	// ThinFilm::ReflectanceConductor is what GGX `fresnel_mode thinfilm`
	// calls (GGXBRDF.cpp / GGXSPF.cpp).  At exactly the FILM's own critical
	// angle the Airy closed form it used is 0/0, and the resulting NaN
	// survives the [0,1] clamps (NaN < 0 and NaN > 1 are both false) and
	// reaches the BRDF -- via the RGB-spectral integral one bad wavelength
	// poisons the whole XYZ accumulation.  This was live until 2026-07-30 and
	// was masked before that by -ffast-math folding the finiteness guards.
	//
	// RED against the pre-fix evaluator: nan on the two LOSSLESS stacks, and a
	// silent finite 1.0 on the three ABSORBING ones (five stacks total).  The
	// absorbing rows are the ones that matter -- see the note on cs[] below.
	// ------------------------------------------------------------------
	{
		std::printf( "\n[Degenerate] shipped single-film path at a film's own critical angle\n" );
		// MUST include ABSORBING substrates -- see the note in
		// ThinFilmTMMTest [8/8](b).  With k == 0 below the film the degenerate
		// Airy quotient is a loud 0/0; with ANY absorption the pre-factoring
		// implementation returned a SILENT, finite, wrong 1.0 (a perfect
		// mirror; error up to 0.477 on Ti) on exactly the air/oxide/metal
		// stack this feature exists for.  A lossless-only set misses that.
		struct { const char* tag; double n0, n1, ns, ks; } cs[] = {
			{ "glass/airgap/glass (lossless)", 1.5,  1.00, 1.5,   0.0  },
			{ "oil/MgF2/glass (lossless)",     1.5,  1.38, 1.5,   0.0  },
			{ "glass/airgap/SILVER",           1.5,  1.00, 0.144, 3.28 },
			{ "glass/MgF2/ALUMINIUM",          1.5,  1.38, 1.02,  6.62 },
			{ "enamel/airgap/TITANIUM",        1.52, 1.00, 2.74,  3.81 },
		};
		double worstJump = 0.0;
		for( size_t i = 0; i < sizeof(cs)/sizeof(cs[0]); ++i ) {
			const double ratio = cs[i].n1 / cs[i].n0;
			const double cosC  = std::sqrt( 1.0 - ratio * ratio );

			const RISE::Scalar R = RISE::ThinFilm::ReflectanceConductor(
				cosC, 550.0, cs[i].n0, 0.0, cs[i].n1, 0.0, 200.0, cs[i].ns, cs[i].ks );
			Check( std::isfinite( R ), "shipped path finite at film's own critical angle" );
			Check( R >= 0.0 && R <= 1.0, "shipped path in [0,1] at film's own critical angle" );

			// The single-film path must equal the N-layer path there.  This is
			// the assertion that catches a SILENT wrong value: finiteness and
			// range both passed while the evaluator returned a perfect-mirror
			// 1.0 instead of 0.886.
			RISE::ThinFilm::Complex f1[1] =
				{ RISE::ThinFilm::detail::MakeIndex( cs[i].n1, 0.0 ) };
			RISE::Scalar d1[1] = { 200.0 };
			const RISE::Scalar Rstack = RISE::ThinFilm::ReflectanceConductorStack(
				cosC, 550.0, RISE::ThinFilm::detail::MakeIndex( cs[i].n0, 0.0 ),
				f1, d1, 1, RISE::ThinFilm::detail::MakeIndex( cs[i].ns, cs[i].ks ) );
			Check( std::fabs( R - Rstack ) < 1e-9,
			       "shipped single-film == N-layer at the film's critical angle" );

			// Continuity, not just finiteness -- a clamp would pass the above.
			const RISE::Scalar lo = RISE::ThinFilm::ReflectanceConductor(
				cosC - 1e-9, 550.0, cs[i].n0, 0.0, cs[i].n1, 0.0, 200.0, cs[i].ns, cs[i].ks );
			const RISE::Scalar hi = RISE::ThinFilm::ReflectanceConductor(
				cosC + 1e-9, 550.0, cs[i].n0, 0.0, cs[i].n1, 0.0, 200.0, cs[i].ns, cs[i].ks );
			const double jump = std::max( std::fabs( R - lo ), std::fabs( R - hi ) );
			worstJump = std::max( worstJump, jump );
			Check( jump < 1e-6, "shipped path CONTINUOUS across the film's critical angle" );
		}

		// A thick absorbing film must evaluate finite.  (This block used to
		// justify itself as "the pairing must not cost thick-absorber
		// headroom, because the TMM overflows from d ~ 1.0e4 nm at k = 3
		// where Airy is finite past 1e6 nm".  That asymmetry no longer
		// exists: since 2026-07-30 the N-layer form is exponent-factored and
		// is total too -- see [Thick], which asserts BOTH forms.  Airy is
		// still primary, but for cost.)
		for( double d = 1e3; d <= 1e6; d *= 10.0 ) {
			const RISE::Scalar R = RISE::ThinFilm::ReflectanceConductor(
				0.866, 550.0, 1.0, 0.0, 2.0, 3.0, d, 1.5, 0.0 );
			Check( std::isfinite( R ) && R >= 0.0 && R <= 1.0,
			       "thick absorbing film still finite (Airy remains the primary form)" );
		}
		std::printf( "  worst |R(crit) - R(crit +/- 1e-9)| = %.3e ; thick absorber finite\n",
		             worstJump );
	}


	// ------------------------------------------------------------------
	// [Helpers] The removable-singularity helpers, pinned on the PRODUCTION
	// definitions (2026-07-30).
	//
	// ThinFilmTMMTest [8/8](d) also pins detail::Sinc / detail::ExpM1OverZ --
	// but it runs under `using namespace RISE::ThinFilmReference`, so it pins
	// the ORACLE's copies in tests/thinfilm/TmmReference.h.  That test never
	// includes src/Library/Utilities/ThinFilm.h and therefore binds NOTHING
	// here: stubbing the PRODUCTION Sinc to a constant 1 below its cut was
	// demonstrated to survive all five production-header binaries, which is
	// exactly the mutant [8/8](d) claims to kill.  This block is the
	// production twin.
	//
	// The reference values are mpmath at 50 decimal digits.  They are NOT a
	// long-double series: on arm64 macOS sizeof(long double) == 8, so a
	// long-double reference carries no extra precision over the double under
	// test and can only pin the FORMULA, never the accuracy.
	// ------------------------------------------------------------------
	{
		std::printf( "\n[Helpers] production detail::Sinc / detail::ExpM1OverZ vs 50-dps mpmath\n" );
		using RISE::ThinFilm::Complex;

		static const struct { double re, im, sre, sim, ere, eim; } kPins[] = {
				{ 5.0, 0.0,
				  -0.19178485493262769378, 0.0,
				  29.482631820515320684, 0.0 },
				{ 3.0, 4.0,
				  -3.8602415567303042395, -3.85861567702757251,
				  -4.1275794838663316996, 0.43651115746579074539 },
				{ -2.0, 1.0,
				  0.46343644844055750049, 0.47624635374092558704,
				  0.3935273565736497649, 0.13982332125464083784 },
				{ -40.0, 5.0,
				  1.2088293195181249714, 1.3883273007178918107,
				  0.024615384615384615342, 0.0030769230769230770196 },
				{ -800.0, 100.0,
				  1.3861647565118960593e+40, 9.2615762581359821721e+39,
				  0.0012307692307692307692, 0.00015384615384615384615 },
				{ 1.0005999999999999339, 0.0,
				  0.84129024056285609261, 0.0,
				  1.718881957770060509, 0.0 },
				{ 0.5999999999999999778, 0.80000000000000004441,
				  1.0394919431829427788, -0.1643467627209476296,
				  1.2073758519179362677, 0.56867889711385716473 },
				{ 0.59940000000000004388, 0.79920000000000002149,
				  1.0394276047650050937, -0.1640098319591696742,
				  1.2073135413356335097, 0.56794062950677345912 },
				{ 0.2999999999999999889, -0.4000000000000000222,
				  1.0112251726220997948, 0.040277947387336925326,
				  1.1330184007985691183, -0.24150806292517034778 },
				{ 0.050000000000000002776, 0.050000000000000002776,
				  0.999999791666668389, -0.00083333330853174620255,
				  1.0249893732640456311, 0.025843748238932301839 },
				{ 0.0010000000000000000208, 0.0,
				  0.99999983333334166667, 0.0,
				  1.0005001667083416681, 0.0 },
				{ 0.00029999999999999997372, -0.00029999999999999997372,
				  0.99999999999999973, 2.9999999999999993587e-8,
				  1.00014999999774973, -0.00015003000224999997335 },
				{ 0.00010000000000000000479, 0.0,
				  0.99999999833333333417, 0.0,
				  1.0000500016667083342, 0.0 },
				{ 0.000050000000000000002396, 0.000050000000000000002396,
				  0.99999999999999999979, -8.3333333333333341318e-10,
				  1.0000249999999895831, 0.000025000833343750001196 },
				{ 9.9999999999999995475e-7, 0.0,
				  0.99999999999983333333, 0.0,
				  1.0000005000001666667, 0.0 },
				{ 0.0, 1.0000000000000000623e-9,
				  1.0000000000000000002, 0.0,
				  0.99999999999999999983, 5.000000000000000311e-10 },
				{ -0.000010000000000000000818, 0.000020000000000000001636,
				  1.00000000005, 6.6666666668666677574e-11,
				  0.99999499995000045833, 9.999933333250002818e-6 },
		};

		double worstS = 0.0, worstE = 0.0;
		for( size_t i = 0; i < sizeof(kPins)/sizeof(kPins[0]); ++i ) {
			const Complex z( kPins[i].re, kPins[i].im );
			const Complex refS( kPins[i].sre, kPins[i].sim );
			const Complex refE( kPins[i].ere, kPins[i].eim );
			const Complex gotS = RISE::ThinFilm::detail::Sinc( z );
			const Complex gotE = RISE::ThinFilm::detail::ExpM1OverZ( z );
			// Scale-relative: the pins span |value| from 1e-3 to 1.4e40.
			const double dS = std::abs( gotS - refS ) / std::max( 1.0, std::abs( refS ) );
			const double dE = std::abs( gotE - refE ) / std::max( 1.0, std::abs( refE ) );
			worstS = std::max( worstS, dS );
			worstE = std::max( worstE, dE );
			Check( dS < 1e-14, "production Sinc matches the 50-dps reference" );
			Check( dE < 1e-14, "production ExpM1OverZ matches the 50-dps reference" );
		}
		std::printf( "  worst relative error: Sinc %.3e  ExpM1OverZ %.3e (tol 1e-14)\n",
		             worstS, worstE );

		// Exact analytic limits at z == 0.
		Check( RISE::ThinFilm::detail::Sinc( Complex(0,0) ) == Complex(1,0),
		       "production Sinc(0) == 1 exactly" );
		Check( RISE::ThinFilm::detail::ExpM1OverZ( Complex(0,0) ) == Complex(1,0),
		       "production ExpM1OverZ(0) == 1 exactly" );

		// Continuity across each branch switch.  Sinc switches at |z| = 1e-4;
		// ExpM1OverZ at |z| = 1.  A stub that is constant below its cut passes
		// the z == 0 checks above but fails these and the table above.
		Check( std::abs( RISE::ThinFilm::detail::Sinc( Complex(9.99e-5,0) )
		                - RISE::ThinFilm::detail::Sinc( Complex(1.001e-4,0) ) ) < 1e-8,
		       "production Sinc continuous across the series cut" );
		Check( std::abs( RISE::ThinFilm::detail::ExpM1OverZ( Complex(0.6,0.8) )
		                - RISE::ThinFilm::detail::ExpM1OverZ( Complex(0.6006,0.8008) ) ) < 1e-3,
		       "production ExpM1OverZ continuous across the |z| = 1 split" );

		// Directly kill the constant-1 stub: Sinc must actually curve below
		// the cut region.  |Sinc(1e-2) - 1| = 1.7e-5.
		Check( std::abs( RISE::ThinFilm::detail::Sinc( Complex(1e-2,0) ) - Complex(1,0) ) > 1e-6,
		       "production Sinc is NOT a constant 1 (kills the stub mutant)" );
		// And the series branch itself must curve: |Sinc(5e-5+5e-5i) - 1| ~ 8.3e-10.
		Check( std::abs( RISE::ThinFilm::detail::Sinc( Complex(5e-5,5e-5) ) - Complex(1,0) ) > 1e-11,
		       "production Sinc series branch is NOT a constant 1 below the cut" );
	}

	// ------------------------------------------------------------------
	// [Branch] Forward-cos branch selection (production detail::PickForwardCos,
	// detail::CosThetaInMedium, detail::InterfaceTerms).  2026-07-30.
	// ------------------------------------------------------------------
	{
		std::printf( "\n[Branch] forward-cos root selection and interface sign\n" );
		using RISE::ThinFilm::Complex;
		namespace TFd = RISE::ThinFilm::detail;

		// (a) THE invariant the evaluator rests on: the forward root DECAYS,
		// Im(N cos) >= 0, in every medium of a passive stack.  This was FALSE
		// for evanescent films until 2026-07-30 -- the branch was decided by
		// the sign of a ~6.1e-17 round-off residue and picked the GROWING
		// root, so |e^{+2i delta}| diverged with thickness.
		{
			bool allDecay = true;
			double worstNeg = 0.0;
			int    n = 0;
			const double k1s[] = { 0.0, 0.7, 3.2 };
			for( double n0 = 1.0; n0 <= 2.61; n0 += 0.1 )
			for( double n1 = 0.2; n1 <= 3.01; n1 += 0.1 )
			for( size_t ki = 0; ki < 3; ++ki )
			for( double cosT = 0.0; cosT <= 1.001; cosT += 0.05 ) {
				const Complex N0 = TFd::MakeIndex( n0, 0.0 );
				const Complex N1 = TFd::MakeIndex( n1, k1s[ki] );
				const Complex si = TFd::SnellInvariant( N0, cosT );
				const Complex c1 = TFd::CosThetaInMedium( N1, si );
				const double  im = ( N1 * c1 ).imag();
				if( im < 0.0 ) { allDecay = false; if( im < worstNeg ) worstNeg = im; }
				++n;
			}
			Check( allDecay,
			       "forward root DECAYS: Im(N cos) >= 0 in every medium of a passive stack" );
			std::printf( "  decay invariant over %d media: worst Im(N cos) = %.3e\n", n, worstNeg );
		}

		// (b) WHY the order of the two tests inside PickForwardCos is
		// load-bearing.  Over an evanescent / propagating grid the `Im == 0`
		// tie-break FIRES (lossless propagating media reach it exactly), while
		// `Re == 0` -- the tie-break the pre-2026-07-30 rule relied on -- never
		// fires for an evanescent medium, because std::sqrt of a negative real
		// leaves a ~6.1e-17 real residue.
		//
		// The census below is a PLATFORM-ASSUMPTION guard, not a guard on
		// PickForwardCos: it recomputes the principal root inline and never
		// calls it, so no edit to PickForwardCos can move these counters.  (An
		// earlier version of this comment claimed it caught the order swap;
		// mutation testing showed it does not -- what kills that swap is (a)
		// above plus [Thick] and [Truth].)  It earns its place by going red on
		// a libm whose std::sqrt returns an exact (0, +-r), which would
		// invalidate the rationale written into PickForwardCos even though
		// every result stayed correct.  The direct pin on the function itself
		// is immediately after.
		{
			int imExactZero = 0, reExactZeroEvan = 0, etaExactZero = 0;
			int evanescent = 0, n = 0;
			double minAbsRe = 1e300;
			for( double n0 = 1.0; n0 <= 2.61; n0 += 0.05 )
			for( double n1 = 0.2; n1 <= 3.01; n1 += 0.05 )
			for( double cosT = 0.0; cosT <= 1.001; cosT += 0.05 ) {
				const Complex N0 = TFd::MakeIndex( n0, 0.0 );
				const Complex N1 = TFd::MakeIndex( n1, 0.0 );
				const Complex si = TFd::SnellInvariant( N0, cosT );
				// The PRINCIPAL root, i.e. the input PickForwardCos judges.
				const Complex st   = si / N1;
				const Complex root = std::sqrt( Complex(1,0) - st * st );
				const Complex eta  = N1 * root;
				if( eta.imag() == 0.0 ) ++imExactZero;
				if( eta.real() == 0.0 && eta.imag() == 0.0 ) ++etaExactZero;
				if( eta.imag() != 0.0 ) {
					// A genuinely EVANESCENT medium: this is the case the old
					// `Re == 0` tie-break was written for.
					++evanescent;
					if( eta.real() == 0.0 ) ++reExactZeroEvan;
					if( std::abs( eta.real() ) < minAbsRe ) minAbsRe = std::abs( eta.real() );
				}
				++n;
			}
			Check( imExactZero > 0,
			       "the `Im == 0` tie-break is LIVE (lossless propagating media reach it exactly)" );
			Check( evanescent > 0 && reExactZeroEvan == 0,
			       "the `Re == 0` tie-break is DEAD for evanescent media -- never test Re first" );
			std::printf( "  %d media: Im(eta)==0 exactly %d times; over %d EVANESCENT media"
			             " Re(eta)==0 %d times (min |Re| = %.3e);"
			             " eta==0 exactly (at critical, both roots 0) %d times\n",
			             n, imExactZero, evanescent, reExactZeroEvan, minAbsRe, etaExactZero );
		}

		// (b2) The DIRECT pin on PickForwardCos, stated as its CONTRACT over
		// explicitly constructed candidates.  This is the assertion an order
		// swap has to get past.
		//
		// It deliberately does NOT obtain the candidate from std::sqrt.  Which
		// root sqrt returns for an evanescent medium depends on the sign of a
		// signed zero in Im(1 - sin^2), and that sign is CONSTANT-FOLDING
		// DEPENDENT: with literal inputs the compiler folds it to +0 and the
		// principal root lands in the first quadrant, whereas at run time it
		// is -0 and the root lands in the fourth.  (Verified both ways; it is
		// part of why the pre-2026-07-30 bug hid from a test suite full of
		// literals while misbehaving in every render.)  Feeding the function
		// both candidates directly removes that dependence entirely.
		{
			const Scalar resid = Scalar( 6.123233995736766e-17 );	// the sqrt residue
			struct Row { const char* tag; Complex N, cand; bool expectNegate; };
			const Row rows[] = {
				// Evanescent: the growing candidate must be negated, the
				// decaying one kept.  Re-first gets BOTH of these wrong --
				// it keeps whichever has Re > 0, which is the growing one.
				{ "evanescent, growing candidate",
				  Complex(1.0,0.0), Complex(  resid, -0.83 ), true  },
				{ "evanescent, decaying candidate",
				  Complex(1.0,0.0), Complex( -resid,  0.83 ), false },
				// Lossless propagating: Im is exactly 0, so the Re tie-break
				// must fire and decide.
				{ "propagating, forward candidate",
				  Complex(1.5,0.0), Complex(  0.8, 0.0 ), false },
				{ "propagating, backward candidate",
				  Complex(1.5,0.0), Complex( -0.8, 0.0 ), true  },
				// Absorbing at normal incidence: cos must stay +1, because
				// Im(eta) = k > 0.  (This file used to add "the classic bug a
				// naive Im >= 0 else negate rule creates"; that claim is FALSE
				// and was retracted in ThinFilm.h and twice in TmmReference.h
				// -- for N = 2.5+3i that rule keeps +1 too.  This was the
				// FOURTH copy.)
				{ "absorbing, normal incidence",
				  Complex(2.5,3.0), Complex( 1.0, 0.0 ), false },
			};
			for( size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); ++i ) {
				const Complex got = TFd::PickForwardCos( rows[i].N, rows[i].cand );
				const Complex want = rows[i].expectNegate ? -rows[i].cand : rows[i].cand;
				Check( got == want, rows[i].tag );
				Check( ( rows[i].N * got ).imag() >= Scalar(0),
				       "PickForwardCos result always decays" );
			}
		}

		// (c) p-polarization SIGN of the interface terms, against the textbook
		// amplitude coefficients.  The Airy <-> TMM agreement cannot catch this:
		// R = |r|^2, and negating BOTH r01 and r1s leaves the Airy denominator
		// invariant, so a whole-branch p sign flip passes the agreement grid.
		// The oracle twin is ThinFilmTMMTest [8/8](c); this pins production.
		{
			const double n0 = 1.0, n1 = 1.5;
			double worstS = 0.0, worstP = 0.0;
			const double angs[] = { 0, 15, 30, 45, 60, 75 };
			for( size_t i = 0; i < sizeof(angs)/sizeof(angs[0]); ++i ) {
				const double th = angs[i] * kDeg;
				const double c0 = std::cos( th );
				const double s1 = ( n0 / n1 ) * std::sin( th );
				const double c1 = std::sqrt( 1.0 - s1 * s1 );
				const double rs = ( n0 * c0 - n1 * c1 ) / ( n0 * c0 + n1 * c1 );
				const double rp = ( n0 * c1 - n1 * c0 ) / ( n0 * c1 + n1 * c0 );

				Complex as, bs, ap, bp;
				TFd::InterfaceTerms( TFd::MakeIndex(n0,0.0), Complex(c0,0.0),
				                     TFd::MakeIndex(n1,0.0), Complex(c1,0.0),
				                     RISE::ThinFilm::ePolS, as, bs );
				TFd::InterfaceTerms( TFd::MakeIndex(n0,0.0), Complex(c0,0.0),
				                     TFd::MakeIndex(n1,0.0), Complex(c1,0.0),
				                     RISE::ThinFilm::ePolP, ap, bp );
				const Complex gs = ( as - bs ) / ( as + bs );
				const Complex gp = ( ap - bp ) / ( ap + bp );
				worstS = std::max( worstS, std::fabs( gs.real() - rs ) );
				worstP = std::max( worstP, std::fabs( gp.real() - rp ) );
				Check( std::fabs( gs.imag() ) < 1e-15 && std::fabs( gp.imag() ) < 1e-15,
				       "production p-sign pin: lossless interface gives real amplitudes" );
			}
			Check( worstS < 1e-14, "production p-sign pin: s amplitude matches closed form (SIGNED)" );
			Check( worstP < 1e-14, "production p-sign pin: p amplitude matches closed form (SIGNED)" );
			std::printf( "  signed amplitude match: worst |s| = %.3e, worst |p| = %.3e\n",
			             worstS, worstP );
		}

		// (d) A film whose index EQUALS the ambient's is at its own critical
		// angle exactly at grazing -- the topology that reopened the cos == 0
		// degeneracy.  [3/5]'s grazing block uses an oxide film and misses it.
		{
			const RISE::Scalar R = RISE::ThinFilm::ReflectanceConductor(
				0.0, 550.0, 1.0, 0.0, 1.0, 0.0, 120.0, 1.5, 0.0 );
			Check( R >= 0.0 && R <= 1.0 && std::isfinite( R ),
			       "grazing with film index == ambient index: finite and in [0,1]" );
			Check( R > 0.999, "grazing with film index == ambient index: R -> 1" );
		}
	}

	// ------------------------------------------------------------------
	// [Thick] Totality on thick ABSORBING and EVANESCENT layers (2026-07-30).
	//
	// This is the GATE for two claims made in ThinFilm.h, both of which would
	// otherwise be prose:
	//   * that the exponent-factored N-layer layer matrix is TOTAL (before
	//     2026-07-30 it overflowed from d ~ 1e4 nm at k = 3, which is what made
	//     ReflectanceConductorStack non-total);
	//   * that SingleFilmReflectanceForPol's `isfinite` TMM fallback is DEAD,
	//     i.e. the Airy form alone already produces a finite value here.
	// Both are asserted on the per-polarization internals, not just on the
	// clamped public result, because the [0,1] clamp swallows a NaN.
	// ------------------------------------------------------------------
	{
		std::printf( "\n[Thick] thick absorbing / evanescent layers stay finite in BOTH forms\n" );
		using RISE::ThinFilm::Complex;
		namespace TFd = RISE::ThinFilm::detail;

		int  n = 0;
		bool airyFinite = true, tmmFinite = true, agree = true;
		double worstDiff = 0.0;
		// Absorbing film (k = 3), and a lossless EVANESCENT film (glass
		// ambient over an air gap beyond critical) -- the two ways |delta|
		// grows without bound.
		const struct { double n0, n1, k1, ns, ks, cosT; } cases[] = {
			{ 1.0, 2.0, 3.0, 1.5,   0.0,  0.866 },
			{ 1.0, 1.8, 0.5, 2.5,   3.0,  0.500 },
			{ 1.5, 1.0, 0.0, 1.5,   0.0,  0.500 },	// evanescent gap, lossless below
			{ 1.5, 1.0, 0.0, 0.144, 3.28, 0.500 },	// evanescent gap, silver below
			{ 1.52,1.0, 0.0, 2.74,  3.81, 0.400 },	// enamel / air gap / titanium
		};
		for( size_t ci = 0; ci < sizeof(cases)/sizeof(cases[0]); ++ci ) {
			for( double d = 1e3; d <= 1e12; d *= 10.0 ) {
				const Complex N0 = TFd::MakeIndex( cases[ci].n0, 0.0 );
				const Complex N1 = TFd::MakeIndex( cases[ci].n1, cases[ci].k1 );
				const Complex Ns = TFd::MakeIndex( cases[ci].ns, cases[ci].ks );
				const Complex si = TFd::SnellInvariant( N0, cases[ci].cosT );
				const Complex f[1] = { N1 };
				const RISE::Scalar t[1] = { RISE::Scalar(d) };

				const RISE::Scalar as = TFd::AiryReflectanceForPol( N0, N1, Ns, d, 550.0, si, RISE::ThinFilm::ePolS );
				const RISE::Scalar ap = TFd::AiryReflectanceForPol( N0, N1, Ns, d, 550.0, si, RISE::ThinFilm::ePolP );
				const RISE::Scalar ts = TFd::TmmReflectanceForPol( N0, f, t, 1, Ns, 550.0, si, RISE::ThinFilm::ePolS );
				const RISE::Scalar tp = TFd::TmmReflectanceForPol( N0, f, t, 1, Ns, 550.0, si, RISE::ThinFilm::ePolP );

				if( !std::isfinite(as) || !std::isfinite(ap) ) airyFinite = false;
				if( !std::isfinite(ts) || !std::isfinite(tp) ) tmmFinite  = false;
				// BOTH polarizations must be finite before folding, or a NaN in
				// the p term would be silently dropped: std::max(finite, NaN)
				// returns the finite operand.
				if( std::isfinite(as) && std::isfinite(ts)
				 && std::isfinite(ap) && std::isfinite(tp) ) {
					const double dd = std::max( std::fabs(double(as)-double(ts)),
					                            std::fabs(double(ap)-double(tp)) );
					if( dd > worstDiff ) worstDiff = dd;
					if( dd > 1e-13 ) agree = false;
				}
				++n;
			}
		}
		Check( airyFinite,
		       "Airy form ALONE stays finite on thick absorbing/evanescent films (fallback is dead)" );
		Check( tmmFinite,
		       "exponent-factored N-layer form stays finite on thick absorbing/evanescent films" );
		Check( agree, "Airy == N-layer on the thick sweep (tol 1e-13; measured ~9e-16)" );
		std::printf( "  %d (stack x thickness) points to d = 1e12 nm; worst |Airy - TMM| = %.3e\n",
		             n, worstDiff );

		// Multi-layer stacks containing thick absorbers -- the parser-legal
		// `ar_layer` shape that returned NaN before 2026-07-30.
		{
			const Complex N0( 1.0, 0.0 ), Ns( 1.52, 0.0 );
			const Complex f3[3] = { Complex(1.38,0.0), Complex(2.1,0.6), Complex(1.46,0.0) };
			const RISE::Scalar d3[3] = { 100.0, 100000.0, 90.0 };
			const RISE::Scalar R3 = RISE::ThinFilm::ReflectanceConductorStack(
				0.8, 550.0, N0, f3, d3, 3, Ns );
			Check( std::isfinite( R3 ) && R3 >= 0.0 && R3 <= 1.0,
			       "3-layer ar_layer stack with a thick absorbing layer is finite" );

			const Complex Nag( 0.144, 3.28 );
			const Complex f4[4] = { Complex(1.38,0.0), Complex(2.2,0.9),
			                        Complex(1.46,0.0), Complex(2.6,1.4) };
			const RISE::Scalar d4[4] = { 120.0, 50000.0, 90.0, 200000.0 };
			const RISE::Scalar R4 = RISE::ThinFilm::ReflectanceConductorStack(
				0.6, 633.0, N0, f4, d4, 4, Nag );
			Check( std::isfinite( R4 ) && R4 >= 0.0 && R4 <= 1.0,
			       "4-layer stack with TWO thick absorbing layers is finite" );
			std::printf( "  ar_layer 3-stack R = %.17g ; 4-stack R = %.17g\n",
			             double(R3), double(R4) );
		}
	}

	// ------------------------------------------------------------------
	// [Truth] Absolute regression pins against an INDEPENDENT 120-decimal-digit
	// mpmath evaluation of the textbook Airy quotient and the textbook
	// characteristic matrix (2026-07-30).
	//
	// Finiteness and range are NOT enough here, and that is the whole point of
	// this block: every value below was FINITE and IN RANGE on the pre-fix
	// evaluator while being wrong -- the first row returned 0.49999999999999994
	// (its p-polarization Airy was exactly 0) where the truth is exactly 1.
	// ------------------------------------------------------------------
	{
		std::printf( "\n[Truth] absolute values vs a 120-dps independent reference\n" );
		using RISE::ThinFilm::Complex;

		struct Row { const char* tag; double cosT, lam, n0, k0, n1, k1, d, n2, k2, truth; };
		const Row rows[] = {
			// The P1-A silent-wrong case: an EVANESCENT film, so this is total
			// internal reflection and R is exactly 1.  Pre-fix: 0.49999999999999994.
			{ "evanescent film / absorbing substrate",
			  0.33415816796736975, 510.88126972901341,
			  2.2636308289185867, 0.0, 0.44278123514951256, 0.0,
			  6884.8552408452115, 2.9724385231768848, 5.140684523956045, 1.0 },
			// Evanescent gaps far past the pre-fix NaN onset (~4 um).
			{ "glass / 1e5 nm air gap / silver",
			  0.5, 550.0, 1.5, 0.0, 1.0, 0.0, 1e5, 0.144, 3.28, 1.0 },
			{ "glass / 1e6 nm air gap / glass",
			  0.5, 550.0, 1.5, 0.0, 1.0, 0.0, 1e6, 1.5, 0.0, 1.0 },
			// A film exactly at its own critical angle over silver: the case
			// the unfactored Airy quotient reported as a perfect mirror (1.0).
			{ "glass / air gap / silver, film at own critical angle",
			  0.74535599249992990, 550.0, 1.5, 0.0, 1.0, 0.0, 200.0, 0.144, 3.28,
			  0.88582495937080988 },
			{ "thick strong absorber, 1e6 nm at k = 3",
			  0.866, 550.0, 1.0, 0.0, 2.0, 3.0, 1e6, 1.5, 0.0,
			  0.55473591548798321 },
		};
		double worst = 0.0;
		for( size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); ++i ) {
			const Row& r = rows[i];
			const RISE::Scalar R = RISE::ThinFilm::ReflectanceConductor(
				r.cosT, r.lam, r.n0, r.k0, r.n1, r.k1, r.d, r.n2, r.k2 );
			const double e = std::fabs( double(R) - r.truth );
			worst = std::max( worst, e );
			// 1e-13, not 1e-9.  At 1e-9 this block discriminated almost
			// nothing -- measured worst is 3.3e-16, so a 1e-10-scale real
			// error walked straight through a pin whose stated job is
			// ABSOLUTE accuracy.  1e-13 still leaves ~300x margin over libm
			// variation while actually constraining the value.
			Check( e < 1e-13, r.tag );
			if( e >= 1e-13 ) {
				std::printf( "    got %.17g want %.17g\n", double(R), r.truth );
			}
		}

		// N-layer path against the same reference.  The first is the exact
		// counter-example that disproved the "nFilms == 1 is the Airy result"
		// claim: it returned NaN while the single-film path returned
		// 0.35449755010853984.
		{
			const Complex N0( 1.0, 0.0 );
			const Complex f1[1] = { Complex( 1.0331426416274845, 0.19106534294380173 ) };
			const RISE::Scalar t1[1] = { 53166.370246100108 };
			const Complex Ns1( 2.5119024320597871, 7.2819781982852083 );
			const RISE::Scalar Rs1 = RISE::ThinFilm::ReflectanceConductorStack(
				0.18439276498056584, 386.41219459337918, N0, f1, t1, 1, Ns1 );
			const double e1 = std::fabs( double(Rs1) - 0.35449755010854007 );
			worst = std::max( worst, e1 );
			Check( e1 < 1e-13, "N-layer(1 thick absorbing film) matches the 120-dps reference" );

			const Complex Ns3( 1.52, 0.0 );
			const Complex f3[3] = { Complex(1.38,0.0), Complex(2.1,0.6), Complex(1.46,0.0) };
			const RISE::Scalar d3[3] = { 100.0, 100000.0, 90.0 };
			const RISE::Scalar Rs3 = RISE::ThinFilm::ReflectanceConductorStack(
				0.8, 550.0, N0, f3, d3, 3, Ns3 );
			const double e3 = std::fabs( double(Rs3) - 0.014823504562343579 );
			worst = std::max( worst, e3 );
			Check( e3 < 1e-13, "3-layer ar_layer stack matches the 120-dps reference" );

			const Complex Nag( 0.144, 3.28 );
			const Complex f4[4] = { Complex(1.38,0.0), Complex(2.2,0.9),
			                        Complex(1.46,0.0), Complex(2.6,1.4) };
			const RISE::Scalar d4[4] = { 120.0, 50000.0, 90.0, 200000.0 };
			const RISE::Scalar Rs4 = RISE::ThinFilm::ReflectanceConductorStack(
				0.6, 633.0, N0, f4, d4, 4, Nag );
			const double e4 = std::fabs( double(Rs4) - 0.037386571463810047 );
			worst = std::max( worst, e4 );
			Check( e4 < 1e-13, "4-layer stack matches the 120-dps reference" );
		}
		std::printf( "  worst |production - 120-dps truth| = %.3e (tol 1e-13)\n", worst );
	}


	// ------------------------------------------------------------------
	// [Domain] Inputs outside the physical domain (2026-07-30).
	//
	// NOTHING between a scene file and ThinFilm.h validates a number: GGXBRDF /
	// GGXSPF resolve the film thickness AND all three media's n and k from
	// IScalarPainters, so a black texel, a negative `scale` or a polynomial
	// that dips below zero arrives at the evaluator directly.  Two failure
	// modes, both of which the [0,1] clamp hides:
	//   * kd < 0 defeats the decaying-root guarantee (|e^{+2i delta}| > 1) and
	//     the round trip diverges -- NaN, or a saturated 1.0.
	//   * a non-passive (n < 0 or k < 0) or zero index gives per-polarization
	//     reflectances above 1, or NaN.  A NaN reaches the BRDF two different
	//     ways, neither benign: the RGB path yields a NaN pixel, the spectral
	//     path goes silently BLACK (GGXBRDF's `if( Rfilm > 0 )` is false for
	//     NaN).
	// Measured pre-normalization values are in the ThinFilm.h comments on
	// detail::PhaseCoefficient and detail::kZeroIndexStandIn.
	// ------------------------------------------------------------------
	{
		std::printf( "\n[Domain] out-of-domain inputs normalize instead of poisoning\n" );
		const struct { const char* tag; double c, n0, n1, k1, ns, ks; } cs[] = {
			{ "lossless film / conductor",  0.7, 1.0, 2.4, 0.0, 2.5,   3.0  },
			{ "absorbing film / conductor", 0.7, 1.0, 2.0, 3.0, 2.5,   3.0  },
			{ "evanescent gap / glass",     0.5, 1.5, 1.0, 0.0, 1.5,   0.0  },
			{ "evanescent gap / silver",    0.5, 1.5, 1.0, 0.0, 0.144, 3.28 },
		};
		double worst = 0.0;
		for( size_t i = 0; i < sizeof(cs)/sizeof(cs[0]); ++i ) {
			const double bare = double( RISE::ThinFilm::ReflectanceConductor(
				cs[i].c, 550.0, cs[i].n0, 0.0, cs[i].n1, cs[i].k1, 0.0, cs[i].ns, cs[i].ks ) );
			// Negative thicknesses spanning the range where the old code went
			// from plausible-but-fictitious to NaN.
			const double negs[] = { -1e-3, -120.0, -5e3, -1e5, -1e9 };
			for( size_t j = 0; j < sizeof(negs)/sizeof(negs[0]); ++j ) {
				const double R = double( RISE::ThinFilm::ReflectanceConductor(
					cs[i].c, 550.0, cs[i].n0, 0.0, cs[i].n1, cs[i].k1, negs[j],
					cs[i].ns, cs[i].ks ) );
				Check( std::isfinite( R ) && R >= 0.0 && R <= 1.0,
				       "negative thickness: finite and in [0,1]" );
				worst = std::max( worst, std::fabs( R - bare ) );
				Check( std::fabs( R - bare ) < 1e-12,
				       "negative thickness == the d -> 0+ limit (bare substrate)" );

				const RISE::ThinFilm::Complex N0 = RISE::ThinFilm::detail::MakeIndex( cs[i].n0, 0.0 );
				const RISE::ThinFilm::Complex Ns = RISE::ThinFilm::detail::MakeIndex( cs[i].ns, cs[i].ks );
				const RISE::ThinFilm::Complex f[1] = { RISE::ThinFilm::detail::MakeIndex( cs[i].n1, cs[i].k1 ) };
				const RISE::Scalar t[1] = { RISE::Scalar( negs[j] ) };
				const RISE::Scalar Rst = RISE::ThinFilm::ReflectanceConductorStack(
					cs[i].c, 550.0, N0, f, t, 1, Ns );
				Check( std::isfinite( Rst ) && std::fabs( double(Rst) - bare ) < 1e-12,
				       "negative thickness: N-layer path agrees with the bare substrate too" );
			}
		}
		// A mixed stack: only the negative layer collapses, the others stand.
		{
			const RISE::ThinFilm::Complex N0( 1.0, 0.0 ), Ns( 1.52, 0.0 );
			const RISE::ThinFilm::Complex f[3] = { RISE::ThinFilm::Complex(1.38,0.0),
			                                       RISE::ThinFilm::Complex(2.1,0.6),
			                                       RISE::ThinFilm::Complex(1.46,0.0) };
			const RISE::Scalar dNeg[3] = { 100.0, -100000.0, 90.0 };
			const RISE::Scalar dZero[3] = { 100.0, 0.0, 90.0 };
			const RISE::Scalar Ra = RISE::ThinFilm::ReflectanceConductorStack( 0.8, 550.0, N0, f, dNeg,  3, Ns );
			const RISE::Scalar Rb = RISE::ThinFilm::ReflectanceConductorStack( 0.8, 550.0, N0, f, dZero, 3, Ns );
			Check( std::isfinite( Ra ) && std::fabs( double(Ra) - double(Rb) ) < 1e-12,
			       "one negative layer in a 3-layer stack collapses to zero thickness, others intact" );
		}
		std::printf( "  worst |R(d<0) - R(d=0)| = %.3e\n", worst );

		// A phase coefficient that is not a POSITIVE FINITE number makes the
		// layer absent.  The condition is on kd, not on thickness: lambda is
		// just as unvalidated as d, and lambda < 0 flips the sign of kd the
		// same way.  All six of these were broken before 2026-07-30 (four NaN,
		// one saturated 1.0, one accidentally fine).
		{
			const double bare = double( RISE::ThinFilm::ReflectanceConductor(
				0.5, 550.0, 1.5, 0.0, 1.4, 0.5, 0.0, 0.144, 3.28 ) );
			const struct { const char* tag; double d, lam; } bad[] = {
				{ "negative thickness",  -300.0, 550.0 },
				{ "negative wavelength",  300.0, -550.0 },
				{ "zero wavelength",      300.0, 0.0 },
				{ "infinite thickness",   std::numeric_limits<double>::infinity(), 550.0 },
				{ "NaN thickness",        std::numeric_limits<double>::quiet_NaN(), 550.0 },
				{ "NaN wavelength",       300.0, std::numeric_limits<double>::quiet_NaN() },
			};
			for( size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i ) {
				const double R = double( RISE::ThinFilm::ReflectanceConductor(
					0.5, bad[i].lam, 1.5, 0.0, 1.4, 0.5, bad[i].d, 0.144, 3.28 ) );
				Check( std::isfinite( R ) && std::fabs( R - bare ) < 1e-12,
				       "non-positive/non-finite kd makes the layer absent" );
			}
		}

		// A NON-PASSIVE index (n < 0 or k < 0) must fold to its passive twin
		// |n| + i|k|, not produce R_p up to 18.31 laundered into a saturated 1.
		// (An earlier comment said 48.8.  That figure was never reproducible
		// on this stack at ANY commit -- including the one that introduced
		// it -- and is the fifth stray copy of a cross-contaminated number in
		// this arc.)
		// Checked through BOTH public APIs: the scalar one routes through
		// MakeIndex, the Complex one used to bypass it entirely.
		{
			const struct { double n1, k1; } signs[] = {
				{  1.4,  0.5 }, { -1.4,  0.5 }, {  1.4, -0.5 }, { -1.4, -0.5 },
			};
			const double ref = double( RISE::ThinFilm::ReflectanceConductor(
				0.5, 550.0, 1.5, 0.0, 1.4, 0.5, 300.0, 0.144, 3.28 ) );
			for( size_t i = 0; i < sizeof(signs)/sizeof(signs[0]); ++i ) {
				const double Rs = double( RISE::ThinFilm::ReflectanceConductor(
					0.5, 550.0, 1.5, 0.0, signs[i].n1, signs[i].k1, 300.0, 0.144, 3.28 ) );
				Check( std::fabs( Rs - ref ) < 1e-12,
				       "scalar API: non-passive index folds to its passive twin" );
				const RISE::ThinFilm::Complex N0( 1.5, 0.0 ), Ns( 0.144, 3.28 );
				const RISE::ThinFilm::Complex N1( signs[i].n1, signs[i].k1 );
				const double Rc = double( RISE::ThinFilm::ReflectanceConductor(
					0.5, 550.0, N0, N1, 300.0, Ns ) );
				Check( std::fabs( Rc - ref ) < 1e-12,
				       "Complex API: non-passive index folds too (it bypasses MakeIndex)" );
				const RISE::ThinFilm::Complex f[1] = { N1 };
				const RISE::Scalar t[1] = { 300.0 };
				const double Rk = double( RISE::ThinFilm::ReflectanceConductorStack(
					0.5, 550.0, N0, f, t, 1, Ns ) );
				Check( std::fabs( Rk - ref ) < 1e-12,
				       "N-layer API: non-passive index folds too" );
			}
		}

		// A ZERO or NaN index is not a medium.  It divides by zero in the
		// Snell step; the resulting NaN survives the [0,1] clamp and reaches
		// the BRDF.  All THREE roles were affected, not just the film.
		{
			const double qnan = std::numeric_limits<double>::quiet_NaN();
			const struct { const char* tag; double n0,k0,n1,k1,n2,k2; } z[] = {
				{ "zero film index",      1.0,0.0,  0.0,0.0,  2.5,3.0 },
				{ "zero substrate index", 1.0,0.0,  2.4,0.0,  0.0,0.0 },
				{ "zero ambient index",   0.0,0.0,  2.4,0.0,  2.5,3.0 },
				{ "NaN film index",       1.0,0.0, qnan,qnan, 2.5,3.0 },
			};
			for( size_t i = 0; i < sizeof(z)/sizeof(z[0]); ++i ) {
				const double R = double( RISE::ThinFilm::ReflectanceConductor(
					0.7, 550.0, z[i].n0, z[i].k0, z[i].n1, z[i].k1, 120.0, z[i].n2, z[i].k2 ) );
				Check( std::isfinite( R ) && R >= 0.0 && R <= 1.0,
				       "zero / NaN index in any role stays finite and in range" );
			}
			// And through the RGB preview integral, where ONE bad wavelength
			// used to poison the whole XYZ accumulation.
			const RISEPel rgb = RISE::ThinFilm::ReflectanceConductorRGB(
				0.7, 1.0, 0.0, 0.0, 0.0, 120.0, 2.5, 3.0 );
			Check( std::isfinite( double(rgb.r) ) && std::isfinite( double(rgb.g) )
			       && std::isfinite( double(rgb.b) ),
			       "zero film index: RGB preview stays finite" );
		}

		// A ZERO index in any role must produce the LIMIT, not an invented
		// constant.  This block has been rewritten FOUR times because the rule
		// was wrong four times (vacuum, absent, opaque-1, and now the
		// stand-in); ThinFilm.h records all four and why.
		//
		// Two properties are non-negotiable for the test itself, because each
		// of the first three rules survived a review round only by being
		// tested in a blind spot:
		//   * a DENSE ambient, since in air vacuum-substitution and "absent"
		//     coincide (that hid rule 1);
		//   * an ABSORBING film, since with a lossless film R == 1 happens to
		//     be the right answer for a degenerate substrate (that hid
		//     rule 3).
		// The reference is the limit itself, probed at n = 1e-25 -- deep in
		// the ~66-decade plateau and far from the overflow cliff.
		{
			const struct { const char* tag; double n0, n1, k1, n2, k2, d, c; } stacks[] = {
				// absorbing films -- the case rule 3 got wrong
				{ "glass / absorbing oxide / X",  1.5,  2.40, 0.30, 2.5,   3.00, 120.0, 0.70 },
				{ "enamel / titanium / X",        1.52, 2.74, 3.81, 1.8,   0.15, 200.0, 0.60 },
				{ "glass / weak absorber / X",    1.5,  1.40, 0.50, 1.8,   0.15, 400.0, 0.50 },
				// lossless films -- the case rules 1 and 2 got wrong
				{ "glass / lossless oxide / X",   1.5,  2.40, 0.00, 1.8,   0.15, 400.0, 0.60 },
				{ "sapphire / lossless / silver", 1.77, 2.10, 0.00, 0.144, 3.28, 250.0, 0.40 },
				{ "air / lossless oxide / X",     1.0,  2.40, 0.00, 2.5,   3.00, 120.0, 0.70 },
			};
			const double probe = 1e-25;		// the limit
			double worst = 0.0, spread = 0.0;
			for( size_t i = 0; i < sizeof(stacks)/sizeof(stacks[0]); ++i ) {
				const double n0 = stacks[i].n0, n1 = stacks[i].n1, k1 = stacks[i].k1;
				const double n2 = stacks[i].n2, k2 = stacks[i].k2;
				const double d  = stacks[i].d,  c  = stacks[i].c;
				// zero FILM index
				const double zF = double( RISE::ThinFilm::ReflectanceConductor(
					c, 550.0, n0, 0.0, 0.0, 0.0, d, n2, k2 ) );
				const double lF = double( RISE::ThinFilm::ReflectanceConductor(
					c, 550.0, n0, 0.0, probe, 0.0, d, n2, k2 ) );
				// zero SUBSTRATE index
				const double zS = double( RISE::ThinFilm::ReflectanceConductor(
					c, 550.0, n0, 0.0, n1, k1, d, 0.0, 0.0 ) );
				const double lS = double( RISE::ThinFilm::ReflectanceConductor(
					c, 550.0, n0, 0.0, n1, k1, d, probe, 0.0 ) );
				// zero AMBIENT index
				const double zA = double( RISE::ThinFilm::ReflectanceConductor(
					c, 550.0, 0.0, 0.0, n1, k1, d, n2, k2 ) );
				const double lA = double( RISE::ThinFilm::ReflectanceConductor(
					c, 550.0, probe, 0.0, n1, k1, d, n2, k2 ) );
				worst = std::max( worst, std::fabs( zF - lF ) );
				worst = std::max( worst, std::fabs( zS - lS ) );
				worst = std::max( worst, std::fabs( zA - lA ) );
				Check( std::fabs( zF - lF ) < 1e-12 && std::fabs( zS - lS ) < 1e-12
				       && std::fabs( zA - lA ) < 1e-12,
				       "a zero index in any role gives the LIMIT, not a constant" );
				Check( zF > 0.0 && zS > 0.0 && zA > 0.0,
				       "the result is > 0, so the GGX lobe survives" );
				// How far the superseded opaque rule would have been.
				spread = std::max( spread, std::fabs( lS - 1.0 ) );
			}
			// The test must EXERCISE the regime that distinguishes the rules,
			// or it proves nothing -- this is the assertion the round-4 test
			// was missing, and its absence is why rule 3 shipped.
			Check( spread > 0.5,
			       "the grid really does contain a stack where R == 1 is badly wrong "
			       "(else this block cannot discriminate)" );
			std::printf( "  zero index vs limit: worst %.3e ; the superseded R==1 rule "
			             "would be off by up to %.4f here\n", worst, spread );
		}

		// ⚠ THE THREE BLIND SPOTS THIS BLOCK USED TO HAVE, closed 2026-07-31.
		// Mutation testing (round 6) showed the grid above passes against
		// several WRONG implementations, because it exercised only one of the
		// three public entry points and only one of the two multi-medium
		// shapes:
		//   (i)  it called only the scalar ReflectanceConductor; the Complex
		//        4-arg overload has its own degeneracy handling and had ZERO
		//        coverage -- reverting JUST that overload to the vacuum rule
		//        passed 3493/0;
		//   (ii) the N-layer rows zeroed a LAYER but never n0/ns, so reverting
		//        just the stack path's endpoint handling also passed;
		//   (iii) the AMBIENT role is vacuous on this grid -- the limit is
		//        EXACTLY 1 on every row, so |zA - lA| < tol is satisfied by
		//        the very rules the block exists to kill.
		// Each is now covered explicitly.  Do not delete these without
		// re-running the mutants.
		{
			const RISE::ThinFilm::Complex N0c( 1.5, 0.0 ), Nsc( 1.8, 0.15 );
			const RISE::ThinFilm::Complex zero( 0.0, 0.0 );
			const RISE::ThinFilm::Complex tiny( 1e-25, 0.0 );
			const RISE::ThinFilm::Complex film( 2.4, 0.3 );	// ABSORBING

			// (i) the Complex 4-arg overload, all three roles
			Check( std::fabs(
			         double( RISE::ThinFilm::ReflectanceConductor( 0.7, 550.0, N0c, zero, 120.0, Nsc ) )
			       - double( RISE::ThinFilm::ReflectanceConductor( 0.7, 550.0, N0c, tiny, 120.0, Nsc ) ) ) < 1e-12
			    && std::fabs(
			         double( RISE::ThinFilm::ReflectanceConductor( 0.7, 550.0, N0c, film, 120.0, zero ) )
			       - double( RISE::ThinFilm::ReflectanceConductor( 0.7, 550.0, N0c, film, 120.0, tiny ) ) ) < 1e-12
			    && std::fabs(
			         double( RISE::ThinFilm::ReflectanceConductor( 0.7, 550.0, zero, film, 120.0, Nsc ) )
			       - double( RISE::ThinFilm::ReflectanceConductor( 0.7, 550.0, tiny, film, 120.0, Nsc ) ) ) < 1e-12,
			       "Complex overload: a zero index in any role gives the LIMIT" );

			// (ii) zero ENDPOINTS through the N-layer path
			{
				const RISE::ThinFilm::Complex f[2] = { RISE::ThinFilm::Complex(1.38,0.0), film };
				const RISE::Scalar t[2] = { 100.0, 120.0 };
				Check( std::fabs(
				         double( RISE::ThinFilm::ReflectanceConductorStack( 0.7, 550.0, N0c, f, t, 2, zero ) )
				       - double( RISE::ThinFilm::ReflectanceConductorStack( 0.7, 550.0, N0c, f, t, 2, tiny ) ) ) < 1e-12
				    && std::fabs(
				         double( RISE::ThinFilm::ReflectanceConductorStack( 0.7, 550.0, zero, f, t, 2, Nsc ) )
				       - double( RISE::ThinFilm::ReflectanceConductorStack( 0.7, 550.0, tiny, f, t, 2, Nsc ) ) ) < 1e-12,
				       "N-layer: a zero AMBIENT or SUBSTRATE gives the LIMIT" );
			}

			// (iii) The ambient role CANNOT be made discriminating, and
			// pretending otherwise would be worse than saying so.  As N0 -> 0
			// the ambient admittance eta0 -> 0, so
			//   r = (eta0*B - C)/(eta0*B + C) -> -1
			// and |r| = 1 for ANY stack beneath.  Measured: worst |limit - 1|
			// = 3.3e-15 over 200,000 random stacks.  So the ambient assertions
			// in the grid above are TRUE but vacuous -- any rule that returns
			// 1 satisfies them.  Discrimination comes from the film and
			// substrate rows and from (i) and (ii) above; this row only pins
			// that the ambient case stays finite and lands on that limit.
			{
				const double zA = double( RISE::ThinFilm::ReflectanceConductor(
					0.7, 550.0, 0.0, 0.0, 2.4, 0.3, 120.0, 1.8, 0.15 ) );
				Check( std::isfinite( zA ) && std::fabs( zA - 1.0 ) < 1e-12,
				       "zero AMBIENT lands on its (structurally unit) limit" );
			}

			// TWO degenerate layers separated by an ordinary one.  This is the
			// shape that made ReflectanceConductorStack NON-TOTAL under the
			// shipped flags before the per-layer renormalization: the p-pol
			// entries reach ~1e161 and the naive c^2+d^2 division overflows.
			{
				const RISE::ThinFilm::Complex f3[3] = { zero, RISE::ThinFilm::Complex(1.38,0.0), zero };
				const RISE::ThinFilm::Complex p3[3] = { tiny, RISE::ThinFilm::Complex(1.38,0.0), tiny };
				const RISE::Scalar t3[3] = { 300.0, 100.0, 300.0 };
				const double Rz = double( RISE::ThinFilm::ReflectanceConductorStack(
					0.6, 550.0, N0c, f3, t3, 3, RISE::ThinFilm::Complex(1.46,0.0) ) );
				const double Rp3 = double( RISE::ThinFilm::ReflectanceConductorStack(
					0.6, 550.0, N0c, p3, t3, 3, RISE::ThinFilm::Complex(1.46,0.0) ) );
				Check( std::isfinite( Rz ) && std::fabs( Rz - Rp3 ) < 1e-12,
				       "two zero layers around an ordinary one: finite, and the LIMIT" );
			}
		}

		// A NON-FINITE index is not the endpoint of any limit -- it is corrupt
		// input.  It must stay in range and stay > 0 (a 0 would render
		// silently black through GGXBRDF's `if( Rfilm > 0 )`).
		{
			const double qnan = std::numeric_limits<double>::quiet_NaN();
			const double inf  = std::numeric_limits<double>::infinity();
			const struct { double n, k; } bad[] = {
				{ qnan, 0.0 }, { qnan, qnan }, { qnan, 3.0 }, { 1.5, qnan },
				{ inf, 0.0 }, { 1.5, inf }, { -inf, 2.0 },
			};
			for( size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i ) {
				const double Rf = double( RISE::ThinFilm::ReflectanceConductor(
					0.60, 550.0, 1.5, 0.0, bad[i].n, bad[i].k, 400.0, 1.8, 0.15 ) );
				const double Rs2 = double( RISE::ThinFilm::ReflectanceConductor(
					0.60, 550.0, 1.5, 0.0, 2.4, 0.3, 400.0, bad[i].n, bad[i].k ) );
				const double Ra = double( RISE::ThinFilm::ReflectanceConductor(
					0.60, 550.0, bad[i].n, bad[i].k, 2.4, 0.3, 400.0, 1.8, 0.15 ) );
				Check( std::isfinite(Rf) && Rf > 0.0 && Rf <= 1.0
				       && std::isfinite(Rs2) && Rs2 > 0.0 && Rs2 <= 1.0
				       && std::isfinite(Ra) && Ra > 0.0 && Ra <= 1.0,
				       "a non-finite index gives a finite in-range result > 0" );
			}
		}

		// N-layer path: a zero layer index anywhere gives the limit too, at
		// any position, without disturbing its neighbours.
		{
			const RISE::ThinFilm::Complex N0( 1.5, 0.0 ), Ns( 1.8, 0.15 );
			const RISE::Scalar t[3] = { 100.0, 400.0, 90.0 };
			for( int pos = 0; pos < 3; ++pos ) {
				RISE::ThinFilm::Complex fz[3] = {
					RISE::ThinFilm::Complex( 1.38, 0.0 ),
					RISE::ThinFilm::Complex( 2.10, 0.20 ),
					RISE::ThinFilm::Complex( 1.46, 0.0 ) };
				RISE::ThinFilm::Complex fp[3] = {
					RISE::ThinFilm::Complex( 1.38, 0.0 ),
					RISE::ThinFilm::Complex( 2.10, 0.20 ),
					RISE::ThinFilm::Complex( 1.46, 0.0 ) };
				fz[pos] = RISE::ThinFilm::Complex( 0.0, 0.0 );
				fp[pos] = RISE::ThinFilm::Complex( 1e-25, 0.0 );
				const double Rz = double( RISE::ThinFilm::ReflectanceConductorStack(
					0.60, 550.0, N0, fz, t, 3, Ns ) );
				const double Rp2 = double( RISE::ThinFilm::ReflectanceConductorStack(
					0.60, 550.0, N0, fp, t, 3, Ns ) );
				Check( std::isfinite( Rz ) && std::fabs( Rz - Rp2 ) < 1e-12,
				       "N-layer: a zero layer index gives the limit at any position" );
			}
		}

		// RGB preview: one bad wavelength used to poison the whole XYZ
		// accumulation.
		{
			const RISEPel rgb = RISE::ThinFilm::ReflectanceConductorRGB(
				0.7, 1.0, 0.0, 0.0, 0.0, 120.0, 2.5, 3.0 );
			Check( std::isfinite( double(rgb.r) ) && std::isfinite( double(rgb.g) )
			       && std::isfinite( double(rgb.b) ) && double(rgb.r) > 0.0,
			       "zero film index: RGB preview stays finite and non-black" );
		}

		// A NaN incidence cosine must normalize like every other out-of-range
		// one.  No live call site is known to produce it (GGXBRDF's
		// half-vector is behind a geometric-horizon gate), so this is defence
		// in depth -- but the asymmetry it removes is exactly the one
		// PhaseCoefficient was corrected for.
		{
			const double qnan = std::numeric_limits<double>::quiet_NaN();
			const double inf  = std::numeric_limits<double>::infinity();
			const double floorR = double( RISE::ThinFilm::ReflectanceConductor(
				0.0, 550.0, 1.0, 0.0, 2.4, 0.0, 120.0, 2.5, 3.0 ) );
			const double cs[] = { qnan, -inf, -1.0, -0.5 };
			for( size_t i = 0; i < sizeof(cs)/sizeof(cs[0]); ++i ) {
				const double R = double( RISE::ThinFilm::ReflectanceConductor(
					cs[i], 550.0, 1.0, 0.0, 2.4, 0.0, 120.0, 2.5, 3.0 ) );
				Check( std::isfinite( R ) && std::fabs( R - floorR ) < 1e-12,
				       "out-of-range or NaN cosine normalizes to the grazing floor" );
			}
			const double Rhigh = double( RISE::ThinFilm::ReflectanceConductor(
				inf, 550.0, 1.0, 0.0, 2.4, 0.0, 120.0, 2.5, 3.0 ) );
			const double Rone = double( RISE::ThinFilm::ReflectanceConductor(
				1.0, 550.0, 1.0, 0.0, 2.4, 0.0, 120.0, 2.5, 3.0 ) );
			Check( std::fabs( Rhigh - Rone ) < 1e-12, "cosine above 1 clamps to normal incidence" );
		}

		// ⚠ DISCLOSED RESIDUAL, deliberately NOT closed: an index far outside
		// the range of real optical constants still overflows -- NaN below
		// 1e-77.02 and above 1e154.06 under the shipped flags, and the large
		// end goes SILENTLY WRONG before it goes NaN (1e154 returns 0.516
		// where the truth is 1).  Under strict IEEE the small end stays
		// CORRECT to ~4e-16 down to ~1e-154.
		// No threshold is asserted here, because any threshold would be the
		// magic epsilon this file refuses; the named reformulation is on
		// detail::kZeroIndexStandIn.  What IS pinned is the boundary that is
		// real scene data -- exactly 0, a black texel -- and a value 50
		// decades small, which is already far past anything physical.
		{
			const double R = double( RISE::ThinFilm::ReflectanceConductor(
				0.7, 550.0, 1.0, 0.0, 1e-50, 0.0, 120.0, 2.5, 3.0 ) );
			Check( std::isfinite( R ) && R >= 0.0 && R <= 1.0,
			       "an index 50 decades small is still handled (the cliff is far below)" );
		}
	}

	// ------------------------------------------------------------------
	// [Renormalize] detail::RenormalizeLayerProduct (2026-08-01).
	//
	// The layer-product rescale is what makes the N-layer path total when a
	// stand-in index drives an entry to ~1e161.  Two properties matter and
	// neither is observable from a reflectance alone, which is why the helper
	// was extracted from the loop to be pinned directly:
	//   * it is EXACT -- a power-of-two rescale preserves every mantissa, so
	//     the ratios the reflectance depends on are bit-unchanged;
	//   * it is TOTAL -- including for a DENORMAL magnitude, where the obvious
	//     implementation (multiply by a precomputed 2^-exponent) overflows the
	//     multiplier to inf while the magnitude itself is still finite.
	// ------------------------------------------------------------------
	{
		std::printf( "\n[Renormalize] exactness and totality of the layer-product rescale\n" );
		using RISE::ThinFilm::Complex;
		const double den = 1e-310;					// denormal
		const double tiny = std::numeric_limits<double>::denorm_min();
		const struct { const char* tag; Complex a, b, c, d; } cases[] = {
			{ "O(1)",            Complex(1,2), Complex(3,-4), Complex(-5,6), Complex(7,8) },
			{ "huge (1e161)",    Complex(1e161,2e160), Complex(3,-4), Complex(-5,6), Complex(7,8) },
			{ "near overflow",   Complex(1e300,0), Complex(1e299,0), Complex(0,0), Complex(1,0) },
			{ "DENORMAL",        Complex(den,0), Complex(den/2,0), Complex(0,0), Complex(den/4,0) },
			{ "denorm_min",      Complex(tiny,0), Complex(0,0), Complex(0,0), Complex(0,0) },
			{ "all zero",        Complex(0,0), Complex(0,0), Complex(0,0), Complex(0,0) },
		};
		for( size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i ) {
			Complex m00 = cases[i].a, m01 = cases[i].b, m10 = cases[i].c, m11 = cases[i].d;
			RISE::ThinFilm::detail::RenormalizeLayerProduct( m00, m01, m10, m11 );
			// TOTAL: nothing may become non-finite.
			Check( std::isfinite(m00.real()) && std::isfinite(m00.imag())
			    && std::isfinite(m01.real()) && std::isfinite(m01.imag())
			    && std::isfinite(m10.real()) && std::isfinite(m10.imag())
			    && std::isfinite(m11.real()) && std::isfinite(m11.imag()),
			       "renormalize stays finite (incl. denormal magnitudes)" );
			// EXACT: every RATIO is bit-identical, which is all the
			// reflectance depends on.  Compare against the original in a
			// scale-free way, skipping the all-zero row.
			const double n0 = std::abs( cases[i].a ), r0 = std::abs( m00 );
			const double n1 = std::abs( cases[i].b ), r1 = std::abs( m01 );
			if( n0 > 0.0 && n1 > 0.0 && r0 > 0.0 && r1 > 0.0 ) {
				Check( std::fabs( (r1/r0) - (n1/n0) ) <= 1e-15 * (n1/n0),
				       "renormalize preserves the ratios exactly" );
			}
		}
		// The reason it exists: the same stack, rescaled or not, must agree.
		// (The un-rescaled form is NaN under the shipped flags, so this is a
		// one-way check -- it pins that the rescaled answer is the right one.)
		{
			const RISE::ThinFilm::Complex N0( 1.5, 0.0 ), Ns( 1.46, 0.0 );
			const RISE::ThinFilm::Complex f[3] = {
				RISE::ThinFilm::Complex(0,0), RISE::ThinFilm::Complex(1.38,0),
				RISE::ThinFilm::Complex(0,0) };
			const RISE::Scalar t[3] = { 300.0, 100.0, 300.0 };
			const double R = double( RISE::ThinFilm::ReflectanceConductorStack(
				0.6, 550.0, N0, f, t, 3, Ns ) );
			// 120-dps limit for this stack.
			Check( std::isfinite( R ) && std::fabs( R - 0.99999989915178811 ) < 1e-12,
			       "two stand-in layers around an ordinary one match the 120-dps limit" );
			std::printf( "  two-stand-in-layer stack: R = %.17g (120-dps 0.99999989915178811)\n", R );
		}
	}

	// ------------------------------------------------------------------
	// [Invariant] Branch-rule-FREE physics invariants (2026-07-30).
	//
	// Every other block in this file scores the evaluator against a reference
	// or against its own sibling algebra.  These two do neither: they are
	// consequences of energy conservation and of the Fresnel equations that
	// hold whatever cos-root convention is in force, so they stay valid even
	// if a future change makes the oracle and production agree for the wrong
	// reason.  That matters here because the 2026-07-30 reformulation moved
	// the oracle's TMM to the SAME exponent-factored algebra as production, so
	// oracle-vs-production is a weaker independent check than it used to be.
	// ------------------------------------------------------------------
	{
		std::printf( "\n[Invariant] TIR and Brewster (independent of the cos-root convention)\n" );

		// (a) Total internal reflection.  Past the critical angle of a LOSSLESS
		// dense->rare interface there is no transmitted wave and no
		// absorption, so R == 1 EXACTLY.  Two forms, because they are not the
		// same statement:
		//   * a BARE interface (nothing beyond the rare medium) is exactly 1
		//     at every angle past critical;
		//   * a dense / rare GAP / dense sandwich is FRUSTRATED TIR -- the
		//     evanescent field tunnels across a thin gap, so R < 1 there and
		//     only reaches 1 in the thick-gap limit.  Asserting exactness on a
		//     thin gap would be asserting the wrong physics.
		{
			double worstBare = 0.0;
			int nBare = 0;
			const double n0s[] = { 1.33, 1.5, 1.52, 1.77, 2.4 };
			for( size_t a = 0; a < sizeof(n0s)/sizeof(n0s[0]); ++a ) {
				const double n0 = n0s[a], nRare = 1.0;
				const double cosC = std::sqrt( 1.0 - ( nRare / n0 ) * ( nRare / n0 ) );
				const double cosines[] = { cosC * 0.9, cosC * 0.5, cosC * 0.1, 0.0 };
				for( size_t c = 0; c < sizeof(cosines)/sizeof(cosines[0]); ++c ) {
					// d = 0 -> the film is absent, so this is the bare
					// n0 -> air interface; the film index is irrelevant.
					const double R = double( RISE::ThinFilm::ReflectanceConductor(
						cosines[c], 550.0, n0, 0.0, 2.0, 0.0, 0.0, nRare, 0.0 ) );
					worstBare = std::max( worstBare, std::fabs( R - 1.0 ) );
					++nBare;
				}
			}
			Check( worstBare < 1e-12,
			       "TIR: bare dense->rare interface is R == 1 exactly past critical" );

			// Frustrated TIR: monotone in gap thickness, leaking when thin,
			// and exactly 1 once the gap is many evanescent decay lengths.
			bool monotone = true;
			double prev = -1.0, thin = 0.0, thick = 0.0;
			const double gaps[] = { 5.0, 25.0, 100.0, 400.0, 2e3, 1e4, 1e6 };
			for( size_t g = 0; g < sizeof(gaps)/sizeof(gaps[0]); ++g ) {
				const double R = double( RISE::ThinFilm::ReflectanceConductor(
					0.5, 550.0, 1.5, 0.0, 1.0, 0.0, gaps[g], 1.5, 0.0 ) );
				if( prev >= 0.0 && R < prev - 1e-12 ) monotone = false;
				prev = R;
				if( g == 0 ) thin = R;
				thick = R;
			}
			Check( monotone, "frustrated TIR: R increases monotonically with gap thickness" );
			Check( thin < 0.9, "frustrated TIR: a thin gap really does leak" );
			Check( std::fabs( thick - 1.0 ) < 1e-12,
			       "frustrated TIR: a thick gap is R == 1 exactly" );
			std::printf( "  TIR: %d bare configurations, worst |R - 1| = %.3e ;"
			             " FTIR R(5nm) = %.6f -> R(1e6 nm) = %.17g\n",
			             nBare, worstBare, thin, thick );
		}

		// (b) Brewster: for a BARE lossless interface, R_p vanishes at
		// tan(theta_B) = n2/n0.  Pins the p-polarization branch against a
		// closed-form angle that owes nothing to this file's algebra.
		{
			double worst = 0.0;
			const double pairs[][2] = { {1.0,1.5}, {1.0,2.4}, {1.5,1.0}, {1.33,1.52}, {1.0,1.38} };
			for( size_t i = 0; i < sizeof(pairs)/sizeof(pairs[0]); ++i ) {
				const double n0 = pairs[i][0], n2 = pairs[i][1];
				const double thB = std::atan( n2 / n0 );
				const RISE::ThinFilm::Complex N0( n0, 0.0 ), Ns( n2, 0.0 );
				const RISE::ThinFilm::Complex si =
					RISE::ThinFilm::detail::SnellInvariant( N0, std::cos( thB ) );
				// d = 0 -> bare interface; the film index is then irrelevant.
				const double Rp = double( RISE::ThinFilm::detail::SingleFilmReflectanceForPol(
					N0, RISE::ThinFilm::Complex( 2.0, 0.0 ), Ns, 0.0, 550.0, si,
					RISE::ThinFilm::ePolP ) );
				worst = std::max( worst, Rp );
				// and via the N-layer path with no films at all
				const double RpN = double( RISE::ThinFilm::detail::TmmReflectanceForPol(
					N0, NULL, NULL, 0, Ns, 550.0, si, RISE::ThinFilm::ePolP ) );
				worst = std::max( worst, RpN );
			}
			Check( worst < 1e-20, "Brewster: R_p vanishes at tan(theta_B) = n2/n0" );
			std::printf( "  Brewster: worst R_p at the Brewster angle = %.3e\n", worst );
		}

		// (c) The grazing floor is a real constant with a real job, and until
		// 2026-07-30 nothing bracketed it: mutating kGrazingCosFloor from 1e-6
		// to 1e-12 or to 0 passed every thin-film binary.  The floor exists
		// because the Snell invariant is built as sqrt(1 - cos^2), and for
		// cos <~ 1.5e-8 that rounds to exactly 1, collapsing the near-grazing
		// answer onto the exact-grazing limit R = 1.  Two assertions bracket
		// it from both sides.
		{
			const double R9 = double( RISE::ThinFilm::ReflectanceConductor(
				1e-9, 550.0, 1.0, 0.0, 2.4, 0.0, 120.0, 2.5, 3.0 ) );
			const double R7 = double( RISE::ThinFilm::ReflectanceConductor(
				1e-7, 550.0, 1.0, 0.0, 2.4, 0.0, 120.0, 2.5, 3.0 ) );
			const double R4 = double( RISE::ThinFilm::ReflectanceConductor(
				1e-4, 550.0, 1.0, 0.0, 2.4, 0.0, 120.0, 2.5, 3.0 ) );
			// Below the floor every cosine gives the SAME answer (the floor is
			// active). A floor of 1e-12 or 0 lets 1e-9 collapse while 1e-7 does
			// not, so this fails.
			Check( R9 == R7, "grazing floor is ACTIVE below itself (kills floor -> 1e-12 / 0)" );
			// And that shared answer must NOT be the degenerate R = 1.
			Check( R9 < 1.0, "grazing floor prevents the collapse to R == 1" );
			// Above the floor the result must still vary with the cosine, so
			// the floor is not merely swallowing everything.
			Check( R4 < R7, "above the floor R still varies with the cosine" );
			std::printf( "  grazing floor: R(1e-9) = R(1e-7) = %.17g ; R(1e-4) = %.17g\n",
			             R9, R4 );
		}
	}

	std::cout << "\nResults: " << s_pass << " passed, " << s_fail << " failed.\n";
	return ( s_fail == 0 ) ? 0 : 1;
}
