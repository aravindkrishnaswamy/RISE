//////////////////////////////////////////////////////////////////////
//
//  ThinFilmFurnaceTest.cpp - Phase-2 piece P2-D: quantify the
//    Kulla-Conty energy-compensation error for the thin-film GGX mode
//    (docs/THIN_FILM_INTERFERENCE.md §7, "Kulla-Conty").
//
//    The thin-film single-scatter Fresnel is exact (P2-B, validated by
//    ThinFilmProductionTest + ThinFilmBRDFTest).  But the multiscatter
//    (Kulla-Conty) TAIL of GGXBRDF/GGXSPF falls through to the conductor
//    branch and uses the SUBSTRATE's hemispherical Fresnel average
//    F_avg = ComputeFresnelAvg(ior, ext) — NOT a thin-film average.  This
//    test measures how wrong that approximation is.
//
//    Why analytic, not Monte-Carlo:
//      * Energy GAIN is structurally impossible.  The multiscatter factor
//        F_ms = F_avg²·Eavg/(1 − F_avg(1−Eavg)) is monotonic in F_avg on
//        [0,1] and bounded by F_avg ≤ 1; the substrate F_avg is a genuine
//        reflectance average ≤ 1, so the compensation cannot exceed the
//        Kulla-Conty energy budget.  P2-B's additive invariant
//        (conductor ≡ thinfilm(film=air) to 3.1e-16) proves the thin-film
//        multiscatter path is byte-identical to the conductor path that
//        GGXWhiteFurnaceTest already certifies energy-conserving, and
//        ThinFilmProductionTest proves the single-scatter R ∈ [0,1].
//      * What remains is a QUALITY question — does substrate-F_avg over- or
//        under-compensate vs the true thin-film F_avg, and by how much?
//        That is an exact analytic quantity (no MC noise): the difference
//        propagated through F_ms and weighted by the multiscatter lobe's
//        directional albedo bound (1 − E_ss) ≤ (1 − E_avg).
//
//    Decision rule: if the worst-case albedo error across the heat-tint
//    regime stays below kAcceptThreshold, the substrate-F_avg
//    approximation is ACCEPTED and documented; otherwise a thin-film
//    F_avg must be implemented.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <iomanip>
#include <cmath>
#include <algorithm>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/ThinFilm.h"
#include "../src/Library/Utilities/Optics.h"
#include "../src/Library/Utilities/MicrofacetEnergyLUT.h"

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

	// Hemispherical Fresnel average of the THIN-FILM stack at one
	// wavelength:  F_avg = 2 ∫₀¹ R(μ) μ dμ.  256-point midpoint rule on μ
	// (the integrand is smooth in μ for the heat-tint regime; 256 points
	// converges F_avg to < 1e-5).  Mirrors the conductor ComputeFresnelAvg
	// definition so the two averages are directly comparable.
	Scalar ThinFilmFavg(
		Scalar filmN, Scalar filmK, Scalar thickness_nm,
		Scalar subN,  Scalar subK,  Scalar nm )
	{
		const int N = 256;
		Scalar sum = 0.0;
		for( int i = 0; i < N; i++ ) {
			const Scalar mu = ( i + 0.5 ) / N;			// μ ∈ (0,1)
			const Scalar R  = ThinFilm::ReflectanceConductor(
				mu, nm, 1.0, 0.0, filmN, filmK, thickness_nm, subN, subK );
			sum += R * mu;
		}
		return 2.0 * sum / N;
	}

	// Multiscatter lobe directional-albedo bound: the energy the K-C tail
	// adds is F_ms · (1 − E_ss(cosθ)) ≤ F_ms · (1 − E_avg).  Using the
	// looser (1 − E_avg) gives a conservative upper bound on the albedo
	// error from an F_avg mismatch, independent of incidence angle.
	Scalar MultiscatterAlbedoErrorBound(
		Scalar Favg_substrate, Scalar Favg_thinfilm, Scalar alpha )
	{
		const Scalar Eavg = MicrofacetEnergyLUT::LookupEavg( alpha );
		const Scalar Fms_sub  = MicrofacetEnergyLUT::ComputeFms<Scalar>( Favg_substrate, Eavg );
		const Scalar Fms_film = MicrofacetEnergyLUT::ComputeFms<Scalar>( Favg_thinfilm,  Eavg );
		return std::fabs( Fms_sub - Fms_film ) * ( 1.0 - Eavg );
	}

	// Accept the substrate-F_avg approximation if the worst-case albedo
	// error stays under 3% (well below MC render noise at practical spp,
	// and confined to the very-rough tail).
	const Scalar kAcceptThreshold = 0.03;

	struct Stack { const char* name; Scalar filmN, filmK, subN, subK; };
}

int main()
{
	std::cout << "ThinFilmFurnaceTest -- P2-D Kulla-Conty energy-compensation error\n";

	// The four canonical heat-tint stacks (representative visible n,k from
	// colors/thinfilm/; constant here so the sweep isolates thickness/λ).
	const Stack stacks[] = {
		{ "Ti / TiO2  (transparent film)", 2.55, 0.00, 2.74, 3.79 },
		{ "Ta / Ta2O5 (transparent film)", 2.20, 0.00, 1.50, 2.30 },
		{ "Nb / Nb2O5 (transparent film)", 2.40, 0.00, 2.60, 2.90 },
		{ "Steel / Fe3O4 (ABSORBING film)",2.40, 0.10, 2.90, 3.20 },
	};
	const Scalar thick[]  = { 0, 40, 80, 120, 160, 200, 250 };		// nm
	const Scalar nms[]    = { 450, 550, 650 };						// nm
	const Scalar alphas[] = { 0.3, 0.6, 0.9 };						// roughness (rough end matters most)

	Scalar worstErr = 0.0;
	Scalar worstFavgDelta = 0.0;
	Scalar worstQuadErr = 0.0;
	const char* worstWhere = "";

	bool allFavgPhysical = true;

	for( const Stack& st : stacks )
	{
		// Substrate F_avg is wavelength-dependent only through n,k; here the
		// substrate n,k are constant, so it is a single value per stack.
		const Scalar FavgSub = MicrofacetEnergyLUT::ComputeFresnelAvg<Scalar>(
			Vector3( 0, 0, 1 ), 1.0, st.subN, st.subK );

		std::cout << "\n  === " << st.name << " ===\n";
		std::cout << "    substrate F_avg = " << std::fixed << std::setprecision(4) << FavgSub << "\n";
		std::cout << "    thk(nm)  F_avg_film(550)  |dF_avg|   max albedo-err (a=0.9)\n";

		for( Scalar d : thick )
		{
			const Scalar FavgFilm550 = ThinFilmFavg( st.filmN, st.filmK, d, st.subN, st.subK, 550 );

			// Worst over wavelengths + roughness for this thickness.
			Scalar errHere = 0.0, dFavgHere = 0.0;
			for( Scalar nm : nms )
			{
				const Scalar FavgFilm = ThinFilmFavg( st.filmN, st.filmK, d, st.subN, st.subK, nm );
				if( FavgFilm < -1e-9 || FavgFilm > 1.0 + 1e-9 ) allFavgPhysical = false;
				const Scalar FavgShip = ThinFilm::FresnelAvgConductor( nm, 1.0, 0.0, st.filmN, st.filmK, d, st.subN, st.subK );
				worstQuadErr = std::max( worstQuadErr, std::fabs( FavgShip - FavgFilm ) );
				dFavgHere = std::max( dFavgHere, std::fabs( FavgFilm - FavgSub ) );
				for( Scalar a : alphas ) {
					errHere = std::max( errHere,
						MultiscatterAlbedoErrorBound( FavgSub, FavgFilm, a ) );
				}
			}

			std::cout << "    " << std::setw(5) << (int)d
					  << "    " << std::setw(8) << std::setprecision(4) << FavgFilm550
					  << "       " << std::setw(7) << dFavgHere
					  << "      " << std::setw(7) << errHere << "\n";

			if( errHere > worstErr ) { worstErr = errHere; worstFavgDelta = dFavgHere; worstWhere = st.name; }
		}
	}

	std::cout << "\n  --- summary ---\n";
	std::cout << "  worst |dF_avg| (substrate vs thin-film) = " << std::setprecision(4) << worstFavgDelta << "\n";
	std::cout << "  worst multiscatter albedo error bound   = " << worstErr
			  << "   at: " << worstWhere << "\n";
	std::cout << "  accept threshold                        = " << kAcceptThreshold << "\n";
	std::cout << "  shipped 21pt-GL vs 256pt F_avg quad err = " << worstQuadErr << "\n";
	std::cout << "  DECISION: " << ( worstErr < kAcceptThreshold ? "ACCEPT substrate-F_avg (document)" :
									 "IMPLEMENT thin-film F_avg" ) << "\n\n";

	// Gate 1: every thin-film F_avg is a physical reflectance average in
	// [0,1] -> the multiscatter compensation cannot inject energy.
	Check( allFavgPhysical, "all thin-film F_avg in [0,1] (no energy gain possible)" );

	// Gate 2: the SHIPPED 16-pt renderer quadrature
	// (ThinFilm::FresnelAvgConductor) matches this 256-pt reference, so the
	// implemented thin-film F_avg is accurate.
	Check( worstQuadErr < 2e-3, "shipped ThinFilm::FresnelAvgConductor (21pt Gauss-Legendre) == reference (256pt)" );

	// Gate 3: the substrate-F_avg error WAS significant (>3%) -- reusing the
	// bare-substrate average (the old behaviour) would have been wrong, so the
	// thin-film F_avg implementation is justified, not gratuitous.
	Check( worstErr > kAcceptThreshold, "substrate-F_avg error > 3% (thin-film F_avg required + implemented)" );

	// Gate 3: sanity — with the FILM INDEX == air the stack reduces
	// algebraically to bare air->substrate at any thickness, so the
	// thin-film F_avg must collapse onto the substrate F_avg (the same
	// additive limit P2-B used).  NOTE d=0 does NOT do this: a coincident
	// zero-thickness film still has two interfaces.
	{
		const Stack& st = stacks[0];
		const Scalar FavgSub = MicrofacetEnergyLUT::ComputeFresnelAvg<Scalar>( Vector3(0,0,1), 1.0, st.subN, st.subK );
		const Scalar FavgAir = ThinFilmFavg( 1.0, 0.0, 120.0, st.subN, st.subK, 550 );
		Check( std::fabs( FavgAir - FavgSub ) < 2e-3,
			   "film=air: thin-film F_avg collapses onto substrate F_avg (additive limit)" );
	}

	std::cout << "Results: " << s_pass << " passed, " << s_fail << " failed.\n";
	return ( s_fail == 0 ) ? 0 : 1;
}
