//////////////////////////////////////////////////////////////////////
//
//  SheenDirectionalAlbedo.h - Baked directional- and bihemispherical-
//    albedo tables for the Charlie sheen lobe (D + Lambda-visibility
//    in CharlieSheen.h), for docs/CLOTH_FABRIC_DESIGN.md section 9.4
//    "The E table" and section 9.2's `fabric_material::hemisphericalAlbedo`
//    route 1 / route 1b / route 2 discussion
//    (`sed -n '/^### 9.2/,/^### 9.5/p' docs/CLOTH_FABRIC_DESIGN.md`).
//
//  ------------------------------------------------------------------
//  WHAT THE TWO TABLES HOLD
//  ------------------------------------------------------------------
//
//  Both are for the BARE, achromatic Charlie lobe (sheenColor ==
//  1); colour is a linear post-multiply the caller applies, so none of
//  this depends on it.
//
//   E(alpha, cosThetaV) -- the directional-hemispherical albedo of the
//     single-scatter Charlie lobe:
//       E(alpha, cosThetaV) = integral_H D(alpha, n.h) * V(alpha, n.l, n.v)
//                              * (n.l) dl
//     the same quantity MicrofacetEnergyLUT.h calls E_ss, for the
//     Charlie lobe instead of GGX.
//
//   EHatMean(alpha) -- the hemispherical mean of the CLAMPED lobe
//     albedo Ehat = min(E, 1):
//       EHatMean(alpha) = 2 * integral_0^1 min(E(alpha, mu), 1) * mu dmu
//     MicrofacetEnergyLUT.h's E_avg, for Ehat rather than E.  Baked
//     directly from the STORED 32-point E row by a trapezoid IN mu
//     (the nodes are not uniformly spaced -- see the axis note below),
//     not from an independent higher-resolution integral, so it is
//     exactly reconstructable from the shipped E table alone -- see
//     tools/SheenDirectionalAlbedoGen.cpp's ComputeEHatMeanFromRow.
//
//     THE CLAMP IN THE NAME IS LOAD-BEARING.  What `fabric_material`
//     multiplies the substrate by is `Ehat`, never the raw E (the
//     Charlie+Lambda fit exceeds 1 in a near-grazing sliver, where an
//     unclamped `1 - m*E` would go NEGATIVE).  So this table holds the
//     mean of the quantity the code actually uses; baking the mean of
//     the raw E would make `hemisphericalAlbedo`'s closed form
//     disagree with `value()`'s own denominator.
//
//  RETIRED 2026-09-02: S(alpha, m).  A third table used to live here --
//  the bihemispherical average of the round-4 sheen/base mixing kernel
//  `min(1 - m*E(n.v), 1 - m*E(n.l))` -- because a `min` DOES NOT FACTOR
//  into (a function of l) x (a function of v), so
//  `fabric_material`'s `hemisphericalAlbedo` could only get that double
//  integral out of a bake.  Round 5 of docs/CLOTH_FABRIC_DESIGN.md
//  replaced the kernel with the Kulla-Conty PRODUCT form
//
//      (1 - m*E(n.v)) * (1 - m*E(n.l)) / (1 - m*EMean)
//
//  -- reciprocal AND energy-exact, where the `min` form was only the
//  former -- and a product factors, collapsing the whole table to a
//  closed form in EMean alone:
//
//      INT INT f_base * kernel * (n.l)(n.v) dl dv
//        = rho_base * (1 - m*EMean)^2 / (1 - m*EMean)
//        = rho_base * (1 - m*EMean)
//
//  So route 1 is now `substrate.hemisphericalAlbedo() * (1 - m*EMean)
//  + sheenColor * EMean`, with no interpolated table between it and
//  the answer -- and its "exact for Lambertian" claim is now exact in
//  closed form rather than up to a bake error.  Full derivation at
//  FabricBRDF.cpp's hemisphericalAlbedo.
//
//  WHY E CAN EXCEED 1, AND WHAT PROTECTS THE CALLER.  Estevez &
//  Kulla's Charlie+Lambda fit is production-friendly, not tightly
//  energy-conserving: the measured E range (see
//  SheenDirectionalAlbedo_LUTData.cpp's banner) EXCEEDS 1 in the
//  extreme low-alpha, near-grazing-view corner, where `1 - m*E` would
//  go NEGATIVE for a high sheenColor and the base term would SUBTRACT
//  energy.  Nothing in THIS file prevents that -- `E()` deliberately
//  reports the lobe honestly, and that is why it is not clamped on
//  return.  The protection lives at the consumer:
//  `FabricBRDF::kMinSheenAlpha` is 0.04, and its criterion is
//
//      the smallest alpha for which  max over mu >= 0.03 of E  <=  1
//
//  -- NOT "max over the whole row", which NO alpha satisfies.  Row 16
//  (alpha = 0.035350) has a whole-row max of 1.251216; what is <= 1
//  there is its max restricted to mu >= 0.03 (0.928138).  (An earlier
//  revision quoted 1.152228 as row 16's whole-row max; that is row
//  17's, alpha = 0.044173.  The restricted figure beside it was and is
//  correct.)  For the runtime the relevant statement is the
//  INTERPOLATED one: max E() over alpha >= 0.04 and mu >= 0.03 is
//  0.961508 -- the criterion holds -- while max E() over alpha >= 0.04
//  at ANY mu is ~1.196, at alpha = 0.04, mu ~ 0.0041, which is exactly
//  the sliver the normaliser exists for.  An earlier
//  revision of this comment stated the unrestricted form and quoted a
//  pre-warp number; it was false against the shipped table and would
//  have authorised the wrong floor at the next re-bake.  The generator
//  prints the scan under "kMinSheenAlpha scan" on every run --
//  consult that, not this sentence.  A defensive min() in
//  `FabricBRDF::SheenTransmit` and the symmetric normaliser in
//  `FabricBRDF::SheenNormaliser` bound whatever remains above 1 inside
//  the near-grazing sliver.
//
//  ------------------------------------------------------------------
//  THE DOMAIN IS [mu1, 1], WITH CONSTANT EXTRAPOLATION BELOW
//  ------------------------------------------------------------------
//
//  `E()` FLOORS its cosTheta argument at node 1, mu1 = 1/961, so
//  everything below that reads E(alpha, mu1).  This is the table's
//  stated domain, not a clamp of convenience, and it is load-bearing.
//
//  Node 0 holds an analytically exact E = 0 at mu = 0.  Interpolating
//  between node 0 and node 1 therefore ramps E up from ZERO across a
//  cell in which the true lobe is already at its PEAK -- E_true is 0.53
//  at mu = 5e-6 and 1.15 by node 1 (alpha = 0.04), where the
//  un-floored interpolant read 0.08.  Because `fabric_material` EMITS
//  the true lobe while both suppressing the base by, and normalising
//  the lobe against, the TABLED E, that under-read let a white
//  Lambertian fabric return rho = 1.714.  The normaliser does NOT bound
//  it: `max(1, E(v), E(l))` collapses to 1 exactly where E_tab < 1 <
//  E_true.
//
//  Constant extrapolation is SOUND, but the argument is not a proof and
//  must not be written as one.  E_true is indeed monotone increasing in
//  mu on (0, mu1), so E_true(mu) <= E_true(mu1) throughout the cell.
//  What does NOT follow is `E_tab(mu1) >= E_true(mu)`, because
//  `E_tab(mu1)` is a LINEAR INTERPOLATION IN LOG-ALPHA and E at node 1
//  is CONCAVE in alpha with a peak near alpha ~ 0.9:
//
//      row 30  alpha = 0.800250   kETable[30][1] = 0.872837
//      row 31  alpha = 1.000000   kETable[31][1] = 0.866264
//      chord at alpha = 0.8983                   = 0.869430
//      E_true at alpha = 0.8983, mu = mu1        = 0.876104
//
//  -- so in the last, widest log-alpha cell the chord UNDER-reads the
//  true lobe by up to 0.0067, and the domination property fails there.
//  Measured worst shortfall (E_true - E_tab(mu1)) over
//  alpha in [0.04, 1] x mu in (0, mu1]: +0.00675 at alpha = 0.898.
//
//  CONSEQUENCE, and it is a bound rather than a blow-up: where
//  E_tab < 1 the normaliser collapses to 1 and the base is only ~87 %
//  suppressed, so rho creeps above 1 -- measured max 1.0067 (+0.67 %)
//  as mu -> mu1 from below, decaying monotonically to 0.18 at
//  mu = 1e-6.  Not "rho <= 1", which an earlier revision of this
//  comment claimed.
//
//  Both the shortfall and the residual would be closed by an alpha node
//  near 0.9 (or by baking node 1 as the cell's supremum rather than its
//  endpoint); neither is Phase-1 work, and the exactness class in
//  FabricBRDF.h carries the measured numbers meanwhile.
//
//  At and above mu1 the floor is a bit-identical no-op, so nothing
//  outside the cell moves.
//
//  ONE MORE DOCUMENTED BEHAVIOUR AT THE EXTREME.  For alpha <~ 0.05 the
//  fabric goes exactly BLACK below mu ~ 1e-6: `CharlieSheen::V` hard-
//  returns 0 once n.l * n.v < 1e-6, so the sheen lobe vanishes for
//  every l, while the floored E(mu1) >= 1 at those roughnesses fully
//  suppresses the base.  Measured rho(alpha = 0.04, mu = 1e-6) = 0.000
//  against 0.648 at mu = 1e-5.  Energy LOSS, not gain, and a 1e-6
//  cosine is a sub-pixel sliver -- but it is a step to zero, not the
//  graceful tail the decay figures above might suggest.
//
//  `EHatMean` integrates the SAME floored function -- see the
//  generator's ComputeEHatMeanFromRow, which adds an analytic
//  constant-extrapolation term for [0, mu1] instead of trapezoiding
//  through the zero node.
//
//  ------------------------------------------------------------------
//  WHERE THE NUMBERS COME FROM
//  ------------------------------------------------------------------
//
//  tools/SheenDirectionalAlbedoGen.cpp bakes SheenDirectionalAlbedo_LUTData.cpp
//  directly by #including CharlieSheen.h and calling its D/V verbatim --
//  no duplicated coefficients, unlike HairMedullaProfileGen.cpp's
//  necessarily-separate PRNG.  E is baked with PER-CELL ADAPTIVE
//  quadrature (a narrow low-alpha/near-grazing band needs far more
//  nodes than the rest of the table to converge); (A third table, S, was baked
//  here until 2026-09-02; see the RETIRED note above.)  See the
//  generator's file header and SheenDirectionalAlbedo_LUTData.cpp's
//  banner for the exact quadrature settings and the measured
//  convergence deltas.  No Monte Carlo anywhere -- byte-identical
//  across re-runs.
//
//  ------------------------------------------------------------------
//  AXES (all endpoint-inclusive uniform-in-their-own-coordinate grids)
//  ------------------------------------------------------------------
//
//   alpha:     kNumAlphaBins nodes, LOG-spaced on [kAlphaMin, kAlphaMax].
//              kAlphaMin == 1e-3 matches CharlieSheen::D's own internal
//              floor (`r_max(alpha, 1e-3)`) -- there is nothing smaller
//              the lobe ever evaluates at.  Log spacing (not linear) is
//              required because D's exponent is 1/alpha: the lobe
//              sharpens monotonically as alpha -> 0, and only a log
//              grid puts enough nodes where E(alpha, .) actually
//              curves.
//   cosTheta:  kNumCosThetaBins nodes, GRAZING-WARPED:
//              mu_j = (j/(N-1))^2, so node 1 sits at 0.00104 and about
//              SIX of the 32 nodes fall below mu = 0.03.  cosTheta==0
//              and cosTheta==1 are both still exact nodes (E is
//              analytically 0 at mu==0 -- CharlieSheen::V's geometric
//              cutoff fires for every incident direction when n.v == 0).
//
//              WHY WARPED, measured (M1 review, 2026-09-02).  This axis
//              was UNIFORM until then, which put the first interior
//              node at mu = 0.0323 -- so the lookup ramped linearly
//              from the exact 0 at mu = 0 across the entire band the
//              Charlie lobe occupies, when the lobe is already near its
//              PEAK by mu ~ 0.005.  Table 0.146 vs truth 1.19 at
//              alpha = 0.04, mu = 0.005.  Because `fabric_material`
//              EMITS the true lobe while suppressing the base by the
//              TABLED E, that under-read broke the white-furnace energy
//              identity by up to +1.05 ABSOLUTE (rho ~ 2.05) under
//              grazing illumination, at every roughness -- and the
//              furnace could not see it, because its incident angles
//              stopped at 80 deg (mu = 0.17, five times the first
//              node).  `SheenDirectionalAlbedo.cpp`'s CosThetaPos
//              inverts the warp with sqrt(); the two are a matched pair.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SHEEN_DIRECTIONAL_ALBEDO_
#define SHEEN_DIRECTIONAL_ALBEDO_

#include "../Utilities/Math3D/Math3D.h"

namespace RISE
{
	namespace SheenDirectionalAlbedo
	{
		//! Table extents.  These are ARCHITECTURAL constants this header
		//! and the generator both hard-code (unlike HairMedullaProfile's
		//! extents, which are `extern` runtime values) -- doing so lets
		//! the static_asserts below catch a mismatched regenerated data
		//! file AT COMPILE TIME rather than only in a unit test.
		static const unsigned int kNumAlphaBins    = 32;
		static const unsigned int kNumCosThetaBins = 32;

		//! Alpha axis endpoints.  Defined in SheenDirectionalAlbedo_LUTData.cpp
		//! alongside the tables themselves.
		extern const float kAlphaMin;	//!< 1e-3f -- matches CharlieSheen::D's internal floor
		extern const float kAlphaMax;	//!< 1.0f

		//! E(alpha, cosTheta), row-major [alphaIdx][cosThetaIdx].
		extern const float kETable[ kNumAlphaBins ][ kNumCosThetaBins ];

		//! EHatMean(alpha) -- the mean of min(E, 1), not of E.
		extern const float kEHatMeanTable[ kNumAlphaBins ];

		//! Compile-time capacity checks: an extern array's TYPE (bounds
		//! included) is known at the declaration above regardless of
		//! which translation unit defines it, so a regenerated data file
		//! whose bounds disagree with kNumAlphaBins/kNumCosThetaBins
		//! fails to compile here -- not just in a unit test.
		static_assert( sizeof( kETable ) / sizeof( kETable[0] ) == kNumAlphaBins,
		               "kETable alpha extent must match kNumAlphaBins" );
		static_assert( sizeof( kETable[0] ) / sizeof( float ) == kNumCosThetaBins,
		               "kETable cosTheta extent must match kNumCosThetaBins" );
		static_assert( sizeof( kEHatMeanTable ) / sizeof( float ) == kNumAlphaBins,
		               "kEHatMeanTable extent must match kNumAlphaBins" );

		//! Directional-hemispherical albedo of the bare Charlie lobe at
		//! (alpha, cosTheta).  Bilinear interpolation (log-alpha x
		//! WARPED-cosTheta, inverted with sqrt), clamped at the table
		//! edges.  Not clamped to [0, 1] on return -- see the file
		//! header: the lobe itself is not tightly energy-conserving near
		//! grazing, and this function reports that honestly.  The
		//! CONSUMER is what bounds it (FabricBRDF's symmetric normaliser
		//! and Ehat), so that a caller wanting the true lobe can have it.
		Scalar E( Scalar alpha, Scalar cosTheta );

		//! Hemispherical mean of Ehat = min(E, 1) at `alpha`.  Linear
		//! interpolation (log-alpha), clamped at the table edges.  See
		//! the file header for why the mean is of the CLAMPED lobe.
		Scalar EHatMean( Scalar alpha );
	}
}

#endif
