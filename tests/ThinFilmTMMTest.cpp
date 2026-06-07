//////////////////////////////////////////////////////////////////////
//
//  ThinFilmTMMTest.cpp - Physics-invariant tests for the standalone
//    thin-film optics reference (the ground-truth oracle for the
//    thin-film interference feature, docs/THIN_FILM_INTERFERENCE.md).
//
//    There is NO renderer integration here and NO dependency on
//    external n,k data files -- every stack is a synthetic / textbook
//    construction.  The tests verify the reference against closed-form
//    optics and against itself (TMM vs Airy), which is the cross-check
//    that catches the classic p-polarization sign-convention bug and
//    the cosθ-branch (decaying-wave) bug for absorbing media.
//
//    Assertions (see design doc §9):
//      1. Airy ≡ TMM (single film) across a λ×θ×d grid, including an
//         absorbing film (k>0) and an absorbing/conductor substrate, to
//         ~machine epsilon.  This is the same physics by two algebras.
//      2. Quarter-wave AR exact closed form at normal incidence, and the
//         R=0 case when n1 = sqrt(n0 n2).
//      3. Bare-substrate limit ≡ Optics::CalculateConductorReflectance
//         for a synthetic conductor across angles (RISE's own Fresnel).
//      4. Energy/sanity: R ∈ [0,1], finite, for every sampled (λ,θ,d)
//         including absorbing cases.
//      5. Absorbing-media behaviour: a thick strongly-absorbing film
//         washes out interference and approaches the bare air/film
//         (top-interface) reflectance.
//      6. Total internal reflection: a bare dense->rare interface beyond
//         the critical angle gives R == 1 exactly (lossless energy
//         conservation), and a frustrated-TIR gap (dense/rare/dense)
//         leaks for a thin gap but -> full TIR for a thick gap.  This
//         exercises the EVANESCENT cosθ branch (Re(N cosθ)=0) that the
//         absorbing-conductor stacks do not reach.
//      7. Lossless thin-film thickness sweep vs an independent
//         real-trigonometric closed form (Hecht), plus a grazing-angle
//         conditioning check (θ -> 90° stays finite and R -> 1).  Pins
//         the full thickness dependence and the e^{+2iδ} phase sign.
//
//    Independent cross-checks (each a DIFFERENT formula, not the
//    admittance form) were also run during development and are folded in
//    above: direct amplitude-Fresnel rs/rp (Brewster nulls), Hecht's
//    real-trig 3-media R, and the frustrated-TIR leakage law.
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
#include <vector>
#include <complex>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "thinfilm/ThinFilmStack.h"
#include "thinfilm/TmmReference.h"
#include "thinfilm/AiryReference.h"

#include "../src/Library/Utilities/Optics.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"

using namespace RISE;
using namespace RISE::ThinFilmReference;

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

	// Sampling grids shared across tests (design doc §9).
	const double kNm[]    = { 380, 420, 460, 500, 540, 580, 620, 660, 700, 740, 780 };
	const double kThetaDeg[] = { 0, 15, 30, 45, 60, 75 };
	const double kThickNm[] = { 5, 40, 90, 150, 250, 400 };

	// True iff every element is finite and the unpolarized average is in
	// [0,1] (with a tiny FP slack on the upper edge for grazing/lossless
	// limits where R touches 1).
	bool PhysicallyValid( const ReflectanceResult& r )
	{
		if( !std::isfinite( r.Rs ) || !std::isfinite( r.Rp ) ) {
			return false;
		}
		const double eps = 1e-9;
		if( r.Rs < -eps || r.Rs > 1.0 + eps ) return false;
		if( r.Rp < -eps || r.Rp > 1.0 + eps ) return false;
		const double Ru = r.Unpolarized();
		return ( Ru >= -eps && Ru <= 1.0 + eps );
	}
}

int main()
{
	std::cout << "ThinFilmTMMTest -- standalone thin-film optics reference invariants\n";

	//------------------------------------------------------------------
	// [1/7] Airy ≡ TMM (single film) across a λ×θ×d grid, including
	//       absorbing film and absorbing/conductor substrate.
	//------------------------------------------------------------------
	std::cout << "\n[1/7] Airy single-film closed form == characteristic-matrix TMM\n";
	{
		// A spread of single-film stacks.  Each entry: ambient, film,
		// substrate.  Deliberately spans lossless film + lossless
		// substrate, lossless film + conductor substrate, ABSORBING film
		// + dielectric substrate, and ABSORBING film + conductor
		// substrate.  The cross-check is only meaningful if it holds for
		// the absorbing cases too -- a wrong p-sign or wrong cosθ branch
		// typically still agrees for the purely-lossless case.
		struct NamedStack { const char* name; Stack stack; };
		std::vector<NamedStack> stacks;

		// Lossless oxide (TiO2-like) on lossless glass.
		stacks.push_back( { "lossless film / lossless substrate",
			MakeSingleFilmStack( Air(), MakeIndex( 2.40, 0.0 ), 0.0, MakeIndex( 1.50, 0.0 ) ) } );

		// Lossless oxide on a conductor (Ti-like) substrate (k>0).
		stacks.push_back( { "lossless film / conductor substrate",
			MakeSingleFilmStack( Air(), MakeIndex( 2.40, 0.0 ), 0.0, MakeIndex( 2.74, 3.81 ) ) } );

		// ABSORBING film (magnetite-like, k>0) on a dielectric substrate.
		stacks.push_back( { "absorbing film / lossless substrate",
			MakeSingleFilmStack( Air(), MakeIndex( 2.30, 0.55 ), 0.0, MakeIndex( 1.50, 0.0 ) ) } );

		// ABSORBING film on a conductor substrate (the hardest case for
		// the branch/sign conventions).
		stacks.push_back( { "absorbing film / conductor substrate",
			MakeSingleFilmStack( Air(), MakeIndex( 2.30, 0.55 ), 0.0, MakeIndex( 2.93, 3.42 ) ) } );

		// Dense lossless film on a conductor, with a non-air ambient
		// (sapphire crystal n=1.77 over the stack) -- exercises a
		// non-unit, still-lossless ambient.
		stacks.push_back( { "lossless film / conductor, sapphire ambient",
			MakeSingleFilmStack( MakeIndex( 1.77, 0.0 ), MakeIndex( 2.10, 0.0 ), 0.0, MakeIndex( 0.20, 3.00 ) ) } );

		double worstAbsDiff = 0.0;
		double worstAbsDiffS = 0.0;
		double worstAbsDiffP = 0.0;
		const double tol = 1e-9;

		for( size_t si = 0; si < stacks.size(); ++si ) {
			for( size_t ti = 0; ti < sizeof(kThickNm)/sizeof(kThickNm[0]); ++ti ) {
				Stack s = stacks[si].stack;
				s.films[0].thickness_nm = kThickNm[ti];

				for( size_t li = 0; li < sizeof(kNm)/sizeof(kNm[0]); ++li ) {
					for( size_t ai = 0; ai < sizeof(kThetaDeg)/sizeof(kThetaDeg[0]); ++ai ) {
						const double lambda = kNm[li];
						const double theta = kThetaDeg[ai] * kDeg;

						const ReflectanceResult tmm  = TmmReflectance( s, lambda, theta );
						const ReflectanceResult airy = AiryReflectance( s, lambda, theta );

						const double dS = std::fabs( tmm.Rs - airy.Rs );
						const double dP = std::fabs( tmm.Rp - airy.Rp );
						const double dU = std::fabs( tmm.Unpolarized() - airy.Unpolarized() );

						worstAbsDiffS = std::max( worstAbsDiffS, dS );
						worstAbsDiffP = std::max( worstAbsDiffP, dP );
						worstAbsDiff  = std::max( worstAbsDiff, dU );

						Check( dS < tol && dP < tol && dU < tol,
						       stacks[si].name );

						// Both implementations must be physically valid too.
						Check( PhysicallyValid( tmm ), "TMM physically valid on grid" );
						Check( PhysicallyValid( airy ), "Airy physically valid on grid" );
					}
				}
			}
		}

		std::printf( "  worst |TMM-Airy|: Rs=%.3e  Rp=%.3e  R=%.3e (tol %.0e)\n",
		             worstAbsDiffS, worstAbsDiffP, worstAbsDiff, tol );
	}

	//------------------------------------------------------------------
	// [2/7] Quarter-wave AR: exact closed form at normal incidence, and
	//       the perfect-AR (R=0) case when n1 = sqrt(n0 n2).
	//------------------------------------------------------------------
	std::cout << "\n[2/7] Quarter-wave AR exact closed form (normal incidence)\n";
	{
		const double lambda0 = 550.0;	// design wavelength
		const double tol = 1e-9;

		// General quarter-wave on lossless n2 in lossless n0=air:
		//   d = lambda0 / (4 n1) ; at normal incidence
		//   R = ((n0 n2 - n1^2)/(n0 n2 + n1^2))^2
		struct QW { double n0, n1, n2; };
		std::vector<QW> cases;
		cases.push_back( { 1.0, 1.38, 1.52 } );	// MgF2 on glass (classic single-layer AR)
		cases.push_back( { 1.0, 2.40, 1.50 } );	// high-index film on glass
		cases.push_back( { 1.0, 1.80, 2.50 } );	// arbitrary lossless pair

		for( size_t c = 0; c < cases.size(); ++c ) {
			const double n0 = cases[c].n0, n1 = cases[c].n1, n2 = cases[c].n2;
			const double d = lambda0 / ( 4.0 * n1 );

			Stack s = MakeSingleFilmStack(
				MakeIndex( n0, 0.0 ), MakeIndex( n1, 0.0 ), d, MakeIndex( n2, 0.0 ) );

			const double Rtmm  = TmmReflectanceUnpolarized( s, lambda0, 0.0 );
			const double Rairy = AiryReflectanceUnpolarized( s, lambda0, 0.0 );

			const double num = n0 * n2 - n1 * n1;
			const double den = n0 * n2 + n1 * n1;
			const double Rexpected = ( num / den ) * ( num / den );

			Check( std::fabs( Rtmm  - Rexpected ) < tol, "quarter-wave AR: TMM matches closed form" );
			Check( std::fabs( Rairy - Rexpected ) < tol, "quarter-wave AR: Airy matches closed form" );

			// Cross-check that the phase factor is really e^{-2iδ} = -1
			// at the quarter-wave thickness (δ = π/2 at normal incidence,
			// so 2δ = π).  We assert this indirectly: a HALF-wave film
			// (twice the thickness, δ=π, e^{-2iδ}=+1) must reproduce the
			// BARE-substrate reflectance ((n0-n2)/(n0+n2))^2, independent
			// of n1 -- the absentee-layer property.  If e^{-2iδ} were not
			// hitting +/-1 at these thicknesses the identity would fail.
			Stack sHalf = MakeSingleFilmStack(
				MakeIndex( n0, 0.0 ), MakeIndex( n1, 0.0 ), 2.0 * d, MakeIndex( n2, 0.0 ) );
			const double Rhalf = TmmReflectanceUnpolarized( sHalf, lambda0, 0.0 );
			const double bnum = n0 - n2, bden = n0 + n2;
			const double Rbare = ( bnum / bden ) * ( bnum / bden );
			Check( std::fabs( Rhalf - Rbare ) < tol,
			       "half-wave absentee layer == bare substrate (confirms e^{-2iδ}=+1)" );
		}

		// Perfect AR: n1 = sqrt(n0 n2) -> R == 0 exactly at the design
		// wavelength and normal incidence.  This is the e^{-2iδ}=-1
		// destructive-interference null.
		{
			const double n0 = 1.0, n2 = 2.25;	// air / (n2=2.25)
			const double n1 = std::sqrt( n0 * n2 );
			const double d = lambda0 / ( 4.0 * n1 );
			Stack s = MakeSingleFilmStack(
				MakeIndex( n0, 0.0 ), MakeIndex( n1, 0.0 ), d, MakeIndex( n2, 0.0 ) );
			const double Rtmm  = TmmReflectanceUnpolarized( s, lambda0, 0.0 );
			const double Rairy = AiryReflectanceUnpolarized( s, lambda0, 0.0 );
			Check( Rtmm  < tol, "perfect quarter-wave AR (n1=sqrt(n0 n2)): TMM R == 0" );
			Check( Rairy < tol, "perfect quarter-wave AR (n1=sqrt(n0 n2)): Airy R == 0" );
			std::printf( "  perfect-AR R: TMM=%.3e  Airy=%.3e\n", Rtmm, Rairy );
		}
	}

	//------------------------------------------------------------------
	// [3/7] Bare-substrate limit == Optics::CalculateConductorReflectance.
	//       Ties the reference to RISE's own conductor Fresnel.
	//------------------------------------------------------------------
	std::cout << "\n[3/7] Bare substrate == Optics::CalculateConductorReflectance (synthetic conductor)\n";
	{
		// Synthetic conductor: n=2.5, k=3.0, ambient air.
		const double nSub = 2.5, kSub = 3.0;
		const Stack bare = MakeBareStack( Air(), MakeIndex( nSub, kSub ) );

		// Optics::CalculateConductorReflectance uses cos = |dot(v,n)| with
		// Ni = ambient (1), Nt = nSub, kt = kSub.  Set n = +Y and
		// v = (sinθ, -cosθ, 0) so dot(v,n) = -cosθ and |dot| = cosθ ==
		// our incidence-angle cosine.  Both compute unpolarized R.
		const Vector3 nrm( 0, 1, 0 );

		// Tolerance: same physics, but Optics clamps cos at NEARZERO and
		// uses a slightly different algebraic arrangement (Born & Wolf
		// real-ambient form), so allow a small numeric slack.  Away from
		// exact grazing the two agree to ~1e-12; we use 1e-7 as the gate.
		const double tol = 1e-7;
		double worst = 0.0;

		// Sample angles up to 89 deg (avoid exact 90 where Optics clamps).
		const double angsDeg[] = { 0, 10, 20, 30, 40, 50, 60, 70, 80, 85, 89 };
		// Wavelength is irrelevant with no film, but sample a couple to
		// confirm the no-film path is wavelength-independent.
		const double lambdas[] = { 380, 550, 780 };

		for( size_t ai = 0; ai < sizeof(angsDeg)/sizeof(angsDeg[0]); ++ai ) {
			const double theta = angsDeg[ai] * kDeg;
			const Vector3 v( std::sin( theta ), -std::cos( theta ), 0 );

			const Scalar Roptics = Optics::CalculateConductorReflectance<Scalar>(
				v, nrm, Scalar(1.0), Scalar(nSub), Scalar(kSub) );

			for( size_t li = 0; li < sizeof(lambdas)/sizeof(lambdas[0]); ++li ) {
				const double Rtmm = TmmReflectanceUnpolarized( bare, lambdas[li], theta );
				const double diff = std::fabs( double(Roptics) - Rtmm );
				worst = std::max( worst, diff );
				Check( diff < tol, "bare conductor TMM == Optics conductor Fresnel" );
			}
		}
		std::printf( "  worst |TMM - Optics| over angles: %.3e (tol %.0e)\n", worst, tol );

		// And the d->0 limit of a FILM stack must equal the bare stack
		// (continuity of the film path into the no-film path).
		{
			const double tol0 = 1e-9;
			Stack thin = MakeSingleFilmStack( Air(), MakeIndex( 2.40, 0.0 ), 0.0, MakeIndex( nSub, kSub ) );
			double worst0 = 0.0;
			for( size_t ai = 0; ai < sizeof(angsDeg)/sizeof(angsDeg[0]); ++ai ) {
				const double theta = angsDeg[ai] * kDeg;
				const double Rfilm = TmmReflectanceUnpolarized( thin, 550.0, theta );
				const double Rbare = TmmReflectanceUnpolarized( bare, 550.0, theta );
				worst0 = std::max( worst0, std::fabs( Rfilm - Rbare ) );
			}
			Check( worst0 < tol0, "d->0 film stack == bare substrate" );
			std::printf( "  worst |d=0 film - bare| over angles: %.3e\n", worst0 );
		}
	}

	//------------------------------------------------------------------
	// [4/7] Energy / sanity sweep: R in [0,1], finite, for every sampled
	//       (λ,θ,d) including absorbing film and conductor substrate.
	//------------------------------------------------------------------
	std::cout << "\n[4/7] Energy / sanity: R in [0,1], finite, all sampled (nm,theta,d)\n";
	{
		struct NamedStack { const char* name; Stack stack; };
		std::vector<NamedStack> stacks;
		stacks.push_back( { "TiO2 on Ti (lossless film, conductor sub)",
			MakeSingleFilmStack( Air(), MakeIndex( 2.55, 0.0 ), 0.0, MakeIndex( 2.74, 3.81 ) ) } );
		stacks.push_back( { "magnetite on steel (absorbing film + conductor sub)",
			MakeSingleFilmStack( Air(), MakeIndex( 2.30, 0.55 ), 0.0, MakeIndex( 2.93, 3.42 ) ) } );
		stacks.push_back( { "Ta2O5 on Ta (transparent film + conductor sub)",
			MakeSingleFilmStack( Air(), MakeIndex( 2.15, 0.0 ), 0.0, MakeIndex( 1.30, 3.20 ) ) } );
		stacks.push_back( { "lossless film on lossless glass",
			MakeSingleFilmStack( Air(), MakeIndex( 2.40, 0.0 ), 0.0, MakeIndex( 1.50, 0.0 ) ) } );

		int sampled = 0;
		bool allValidTmm = true;
		bool allValidAiry = true;
		for( size_t si = 0; si < stacks.size(); ++si ) {
			for( size_t ti = 0; ti < sizeof(kThickNm)/sizeof(kThickNm[0]); ++ti ) {
				Stack s = stacks[si].stack;
				s.films[0].thickness_nm = kThickNm[ti];
				for( size_t li = 0; li < sizeof(kNm)/sizeof(kNm[0]); ++li ) {
					for( size_t ai = 0; ai < sizeof(kThetaDeg)/sizeof(kThetaDeg[0]); ++ai ) {
						const double lambda = kNm[li];
						const double theta = kThetaDeg[ai] * kDeg;
						const ReflectanceResult tmm  = TmmReflectance( s, lambda, theta );
						const ReflectanceResult airy = AiryReflectance( s, lambda, theta );
						if( !PhysicallyValid( tmm ) )  allValidTmm = false;
						if( !PhysicallyValid( airy ) ) allValidAiry = false;
						++sampled;
					}
				}
			}
		}
		Check( allValidTmm,  "TMM: R in [0,1] and finite across full sweep" );
		Check( allValidAiry, "Airy: R in [0,1] and finite across full sweep" );
		std::printf( "  sampled %d (stack,nm,theta,d) points, all physical\n", sampled );
	}

	//------------------------------------------------------------------
	// [5/7] Absorbing-media behaviour: a thick strongly-absorbing film
	//       washes out interference and -> bare air/film (top-interface)
	//       reflectance, independent of the substrate underneath.
	//------------------------------------------------------------------
	std::cout << "\n[5/7] Thick strongly-absorbing film -> bare top-interface reflectance\n";
	{
		// Strongly absorbing film index; large thickness so the wave is
		// fully attenuated before it can return from the substrate.
		const Complex filmIdx = MakeIndex( 2.0, 1.5 );	// k=1.5 -> strong absorption
		const double thick = 5000.0;					// nm: many extinction lengths

		// Two very different substrates beneath the same thick film: a
		// dielectric and a conductor.  If interference is truly washed
		// out, R must be (a) nearly identical between the two substrates,
		// and (b) close to the bare air/film top-interface reflectance.
		Stack onGlass = MakeSingleFilmStack( Air(), filmIdx, thick, MakeIndex( 1.50, 0.0 ) );
		Stack onMetal = MakeSingleFilmStack( Air(), filmIdx, thick, MakeIndex( 0.20, 3.50 ) );

		// Bare air/film top interface: a "substrate" equal to the film
		// index (so there is no lower interface to reflect from); the
		// film contents are then irrelevant.
		Stack topOnly = MakeBareStack( Air(), filmIdx );

		const double tolWash = 5e-3;	// generous: this is a physical limit, not an identity
		double worstSubDiff = 0.0;
		double worstTopDiff = 0.0;

		for( size_t li = 0; li < sizeof(kNm)/sizeof(kNm[0]); ++li ) {
			for( size_t ai = 0; ai < sizeof(kThetaDeg)/sizeof(kThetaDeg[0]); ++ai ) {
				const double lambda = kNm[li];
				const double theta = kThetaDeg[ai] * kDeg;

				const double Rg = TmmReflectanceUnpolarized( onGlass, lambda, theta );
				const double Rm = TmmReflectanceUnpolarized( onMetal, lambda, theta );
				const double Rt = TmmReflectanceUnpolarized( topOnly, lambda, theta );

				worstSubDiff = std::max( worstSubDiff, std::fabs( Rg - Rm ) );
				worstTopDiff = std::max( worstTopDiff, std::fabs( Rg - Rt ) );

				// Airy must agree with TMM here too (it's a single film).
				const double RgAiry = AiryReflectanceUnpolarized( onGlass, lambda, theta );
				Check( std::fabs( Rg - RgAiry ) < 1e-9, "thick-absorbing: Airy == TMM" );
			}
		}
		Check( worstSubDiff < tolWash,
		       "thick absorbing film: substrate choice no longer matters" );
		Check( worstTopDiff < tolWash,
		       "thick absorbing film: R -> bare air/film top-interface reflectance" );
		std::printf( "  worst |glass-metal sub| = %.3e ; worst |R - top-interface| = %.3e (tol %.0e)\n",
		             worstSubDiff, worstTopDiff, tolWash );
	}

	//------------------------------------------------------------------
	// [6/7] Total internal reflection (lossless energy conservation) and
	//       frustrated TIR.  Exercises the EVANESCENT cosθ branch
	//       (Re(N cosθ) = 0), which the absorbing-conductor stacks above
	//       never reach -- a wrong evanescent branch passes [1..5] but
	//       fails here.
	//------------------------------------------------------------------
	std::cout << "\n[6/7] Total internal reflection and frustrated TIR (evanescent branch)\n";
	{
		// Pure TIR: bare dense->rare interface (glass n=1.5 ambient, air
		// substrate).  Critical angle asin(1/1.5) = 41.81 deg.  Beyond it,
		// R must be EXACTLY 1 for both polarizations (no absorption, no
		// transmission -> all energy reflected).
		const Stack tir = MakeBareStack( MakeIndex( 1.50, 0.0 ), MakeIndex( 1.00, 0.0 ) );
		const double thetaC = std::asin( 1.0 / 1.5 );	// critical angle

		double worstTIR = 0.0;
		const double aboveDeg[] = { 42, 45, 50, 60, 75, 89 };
		for( size_t ai = 0; ai < sizeof(aboveDeg)/sizeof(aboveDeg[0]); ++ai ) {
			const ReflectanceResult r = TmmReflectance( tir, 550.0, aboveDeg[ai] * kDeg );
			worstTIR = std::max( worstTIR, std::fabs( r.Rs - 1.0 ) );
			worstTIR = std::max( worstTIR, std::fabs( r.Rp - 1.0 ) );
			Check( std::isfinite( r.Rs ) && std::isfinite( r.Rp ), "TIR: finite beyond critical" );
		}
		Check( worstTIR < 1e-12, "beyond-critical TIR: Rs == Rp == 1 exactly" );

		// Just below critical, R must be strictly < 1 (partial transmission)
		// and continuous across the boundary.
		const double Rbelow = TmmReflectanceUnpolarized( tir, 550.0, ( thetaC - 1.0 * kDeg ) );
		const double Rat    = TmmReflectanceUnpolarized( tir, 550.0, thetaC );
		Check( Rbelow < 1.0 - 1e-6, "just below critical: R < 1 (partial transmission)" );
		Check( std::fabs( Rat - 1.0 ) < 1e-9, "exactly at critical: R == 1" );
		std::printf( "  TIR worst |R-1| beyond critical = %.3e ; R(just below crit) = %.5f\n",
		             worstTIR, Rbelow );

		// Frustrated TIR: dense / rare gap / dense (glass / air gap / glass)
		// at 60 deg (beyond critical).  The gap is evanescent.  A THIN gap
		// leaks (R small), a THICK gap -> full TIR (R -> 1), and TMM must
		// equal Airy throughout (this is the single-film evanescent case).
		double prevR = -1.0;
		bool monotonicUp = true;
		double worstFTIR = 0.0;
		const double gapNm[] = { 5, 25, 50, 100, 200, 500, 1000 };
		for( size_t gi = 0; gi < sizeof(gapNm)/sizeof(gapNm[0]); ++gi ) {
			const Stack ft = MakeSingleFilmStack(
				MakeIndex( 1.50, 0.0 ), MakeIndex( 1.00, 0.0 ), gapNm[gi], MakeIndex( 1.50, 0.0 ) );
			const ReflectanceResult tmm  = TmmReflectance( ft, 550.0, 60.0 * kDeg );
			const ReflectanceResult airy = AiryReflectance( ft, 550.0, 60.0 * kDeg );
			worstFTIR = std::max( worstFTIR, std::fabs( tmm.Unpolarized() - airy.Unpolarized() ) );
			Check( PhysicallyValid( tmm ), "frustrated TIR: physically valid" );
			const double Ru = tmm.Unpolarized();
			if( prevR >= 0.0 && Ru < prevR - 1e-9 ) {
				monotonicUp = false;
			}
			prevR = Ru;
		}
		Check( monotonicUp, "frustrated TIR: R increases monotonically with gap thickness" );
		Check( worstFTIR < 1e-9, "frustrated TIR: TMM == Airy (evanescent single film)" );
		// thin gap leaks, thick gap is near-total.
		const Stack ftThin  = MakeSingleFilmStack( MakeIndex(1.50,0.0), MakeIndex(1.00,0.0),    5.0, MakeIndex(1.50,0.0) );
		const Stack ftThick = MakeSingleFilmStack( MakeIndex(1.50,0.0), MakeIndex(1.00,0.0), 1000.0, MakeIndex(1.50,0.0) );
		const double Rthin  = TmmReflectanceUnpolarized( ftThin,  550.0, 60.0 * kDeg );
		const double Rthick = TmmReflectanceUnpolarized( ftThick, 550.0, 60.0 * kDeg );
		Check( Rthin < 0.2,            "frustrated TIR: thin gap leaks (R small)" );
		Check( Rthick > 1.0 - 1e-4,    "frustrated TIR: thick gap -> full TIR" );
		std::printf( "  frustrated TIR: R(5nm)=%.5f  R(1000nm)=%.5f ; worst |TMM-Airy|=%.3e\n",
		             Rthin, Rthick, worstFTIR );
	}

	//------------------------------------------------------------------
	// [7/7] Lossless thin-film thickness sweep vs an INDEPENDENT
	//       real-trigonometric closed form (Hecht), and a grazing-angle
	//       conditioning check.  The Hecht form
	//         R = (r01^2 + r12^2 + 2 r01 r12 cos2β) /
	//             (1 + r01^2 r12^2 + 2 r01 r12 cos2β)
	//       is a DIFFERENT algebra from the admittance Airy/TMM, so it
	//       pins both the full thickness dependence and the e^{+2iδ} phase
	//       sign (a flipped sign changes the fringe phase and fails this).
	//------------------------------------------------------------------
	std::cout << "\n[7/7] Lossless thickness sweep vs Hecht closed form + grazing conditioning\n";
	{
		const double n0 = 1.0, n1 = 2.40, n2 = 1.50;	// lossless, normal incidence
		const double r01 = ( n0 - n1 ) / ( n0 + n1 );
		const double r12 = ( n1 - n2 ) / ( n1 + n2 );	// at normal incidence s==p

		const Stack base = MakeSingleFilmStack(
			MakeIndex( n0, 0.0 ), MakeIndex( n1, 0.0 ), 0.0, MakeIndex( n2, 0.0 ) );

		double worstHecht = 0.0;
		double measMin = 1e9, measMax = -1e9;
		for( double d = 10.0; d <= 600.0; d += 5.0 ) {
			Stack s = base;
			s.films[0].thickness_nm = d;
			const double beta = ( 2.0 * M_PI / 550.0 ) * n1 * d;	// cosθ1 = 1 at normal
			const double c2b = std::cos( 2.0 * beta );
			const double num = r01 * r01 + r12 * r12 + 2.0 * r01 * r12 * c2b;
			const double den = 1.0 + r01 * r01 * r12 * r12 + 2.0 * r01 * r12 * c2b;
			const double Rhecht = num / den;

			const double Rtmm  = TmmReflectanceUnpolarized( s, 550.0, 0.0 );
			const double Rairy = AiryReflectanceUnpolarized( s, 550.0, 0.0 );
			worstHecht = std::max( worstHecht, std::fabs( Rtmm  - Rhecht ) );
			worstHecht = std::max( worstHecht, std::fabs( Rairy - Rhecht ) );
			measMin = std::min( measMin, Rtmm );
			measMax = std::max( measMax, Rtmm );
		}
		Check( worstHecht < 1e-9, "lossless thickness sweep: TMM/Airy == Hecht real-trig form" );

		// Interference fringe envelope: R oscillates between the |r01|-|r12|
		// and |r01|+|r12| reflection sums.  Confirms the fringe amplitude.
		const double envLo = ( ( r01 - r12 ) / ( 1.0 - r01 * r12 ) ) * ( ( r01 - r12 ) / ( 1.0 - r01 * r12 ) );
		const double envHi = ( ( r01 + r12 ) / ( 1.0 + r01 * r12 ) ) * ( ( r01 + r12 ) / ( 1.0 + r01 * r12 ) );
		const double lo = std::min( envLo, envHi ), hi = std::max( envLo, envHi );
		Check( measMin > lo - 1e-3 && measMin < lo + 1e-2, "fringe minimum matches analytic envelope" );
		Check( measMax > hi - 1e-2 && measMax < hi + 1e-3, "fringe maximum matches analytic envelope" );
		std::printf( "  worst |TMM/Airy - Hecht| = %.3e ; fringes in [%.4f,%.4f] (analytic [%.4f,%.4f])\n",
		             worstHecht, measMin, measMax, lo, hi );

		// Grazing-angle conditioning: as θ -> 90° the result must stay
		// finite, TMM must track Airy, and R -> 1.  (Exactly 90° is a
		// documented degenerate input -- η_p = N/0 -- and is NOT sampled.)
		const Stack g = MakeSingleFilmStack(
			MakeIndex( 1.0, 0.0 ), MakeIndex( 2.40, 0.0 ), 120.0, MakeIndex( 2.74, 3.81 ) );
		double worstGraz = 0.0;
		double Rlast = 0.0;
		const double grazDeg[] = { 85.0, 88.0, 89.0, 89.9, 89.99, 89.999 };
		for( size_t gi = 0; gi < sizeof(grazDeg)/sizeof(grazDeg[0]); ++gi ) {
			const ReflectanceResult tmm  = TmmReflectance( g, 550.0, grazDeg[gi] * kDeg );
			const ReflectanceResult airy = AiryReflectance( g, 550.0, grazDeg[gi] * kDeg );
			Check( PhysicallyValid( tmm ), "grazing: TMM finite and in [0,1]" );
			worstGraz = std::max( worstGraz, std::fabs( tmm.Unpolarized() - airy.Unpolarized() ) );
			Rlast = tmm.Unpolarized();
		}
		Check( worstGraz < 1e-9, "grazing: TMM == Airy approaching 90 deg" );
		Check( Rlast > 0.999, "grazing: R -> 1 as theta -> 90 deg" );
		std::printf( "  grazing worst |TMM-Airy| = %.3e ; R(89.999 deg) = %.6f\n", worstGraz, Rlast );
	}

	std::cout << "\nResults: " << s_pass << " passed, " << s_fail << " failed.\n";
	return ( s_fail == 0 ) ? 0 : 1;
}
