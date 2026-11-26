//////////////////////////////////////////////////////////////////////
//
//  HairMedullaProfile.h - The precomputed medulla scattering profile
//    behind the Yan et al. 2017 TTs / TRTs fur lobes (Phase 3 of
//    docs/HAIR_FUR_DESIGN.md; the model itself lives in
//    HairBSDF.{h,cpp}).
//
//  ------------------------------------------------------------------
//  WHAT THE TABLE HOLDS
//  ------------------------------------------------------------------
//
//  Yan 2017 models a fur fibre as a DOUBLE CYLINDER: an absorbing
//  cortex annulus around a SCATTERING medulla of radius ratio
//  kappa = r_medulla / r_fibre.  Light on a TT or TRT path that
//  crosses the medulla either passes ballistically (staying in the
//  ordinary Chiang lobe) or scatters inside it -- and the scattered
//  part leaves along a broad, diffused distribution, which is what
//  makes fur read soft and saturated instead of "thin shiny hair".
//
//  The medulla walk has no closed form, so it is PRECOMPUTED.  This
//  table is the result of a Monte-Carlo simulation of one crossing of
//  a unit-radius, infinitely long, non-absorbing scattering cylinder
//  (tools/HairMedullaProfileGen.cpp), tabulated over three axes:
//
//    b     the impact parameter -- the perpendicular distance from
//          the medulla axis to the entering ray, divided by the
//          medulla radius.  b in [0, 1]; the sign is folded out by
//          the mirror symmetry of the cylinder (a ray at -b produces
//          the mirror-image azimuthal profile), and the CALLER
//          restores it -- see `HairMedullaLookup`'s `bSigned`.
//    tau   the DIAMETRAL optical depth of the medulla along the ray,
//          sigma_m * 2 * kappa / cos(theta_t).
//          ! ON UNITS.  `sigma_m` is per FIBRE RADIUS, and so is
//          `sigma_a` -- HairBSDF's `absorbLen = 2 cos(gamma_t) /
//          cos(theta_t)` is 2 at normal incidence through the axis
//          because h in [-1, 1] makes the radius 1, i.e. 2 radii = one
//          diameter.  Some older comments (and the `sigma_a` chunk
//          description) say "per unit fibre diameter"; that wording
//          predates this file and is off by the factor of 2 in the
//          NAME only -- the two coefficients are in the same units as
//          each other, which is what actually matters, and the 2 above
//          is why.  The optical depth of
//          the actual chord at impact parameter b is
//          tau * sqrt(1 - b^2), so the ballistic (unscattered)
//          fraction is exp( -tau * sqrt(1 - b^2) ) -- the caller
//          computes that itself, and this table describes ONLY what
//          happens to the rest.
//    g     the Henyey-Greenstein anisotropy of the medulla phase
//          function.
//
//  and holding, per cell:
//
//    * `kHairMedullaPhiBins` azimuthal density values, the
//      distribution of the exit azimuthal DEFLECTION d_phi in
//      [-pi, pi) CONDITIONAL ON AT LEAST ONE SCATTERING EVENT (the
//      ballistic case is excluded -- it is accounted for separately,
//      by the caller, as the unscattered lobe).  Values are a density
//      per radian: sum_i value_i * (2 pi / bins) == 1.
//    * one extra float: the VARIANCE (radians^2) of the exit
//      longitudinal angle, used to broaden the scattered lobe's
//      longitudinal term M_p.
//
//  ------------------------------------------------------------------
//  HOW IT IS USED  (and the exact-sampling contract)
//  ------------------------------------------------------------------
//
//  `HairMedullaLookup` trilinearly interpolates the three axes into a
//  single `HairMedullaProfile`, which is then treated as a PERIODIC
//  PIECEWISE-LINEAR density over d_phi with nodes at the bin centres.
//  A convex combination of normalised profiles is itself normalised,
//  and the piecewise-linear reading of a normalised histogram has the
//  same integral as the piecewise-constant one, so the interpolated
//  profile integrates to 1 by construction -- no renormalisation at
//  lookup time, and no drift between `Eval` and `Sample`.
//
//  `HairMedullaProfile::Eval` and `HairMedullaProfile::Sample` are
//  exact inverses in the importance-sampling sense: `Sample(u)` draws
//  from precisely the density `Eval` reports.  That is what lets the
//  scattered lobes join the existing dot(w, ap) / dot(w, apPDF)
//  mixture without breaking Scatter / Pdf / value consistency.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef HAIR_MEDULLA_PROFILE_
#define HAIR_MEDULLA_PROFILE_

#include "../Utilities/Math3D/Math3D.h"

namespace RISE
{
	//! Table dimensions.  Defined in HairMedullaProfile_LUTData.cpp
	//! alongside the data itself so a regenerated table cannot
	//! disagree with the extents the runtime assumes.
	extern const unsigned int kHairMedullaNumB;
	extern const unsigned int kHairMedullaNumTau;
	extern const unsigned int kHairMedullaNumG;
	extern const unsigned int kHairMedullaPhiBins;

	//! Axis endpoints.  `b` is uniform on [0, 1]; `tau` is GEOMETRIC
	//! (log-uniform) on [kHairMedullaTauMin, kHairMedullaTauMax]; `g`
	//! is uniform on [-kHairMedullaGMax, +kHairMedullaGMax].
	extern const float kHairMedullaTauMin;
	extern const float kHairMedullaTauMax;
	extern const float kHairMedullaGMax;

	//! ( kHairMedullaPhiBins + 1 ) floats per cell: the azimuthal
	//! density followed by the longitudinal variance.  Cell index is
	//! ( ( ib * kHairMedullaNumTau + it ) * kHairMedullaNumG + ig ).
	extern const float kHairMedullaProfileData[];

	//! Total float count -- baked alongside the data so a test can
	//! assert the array is the size the extents imply.
	extern const unsigned int kHairMedullaNumFloats;

	namespace Implementation
	{
		//! One interpolated cell: a periodic piecewise-linear azimuthal
		//! density plus the longitudinal broadening variance.
		//!
		//! Fixed-capacity by design -- this is built on the stack in
		//! the BCSDF inner loop and must not allocate.  The capacity is
		//! asserted against `kHairMedullaPhiBins` in the .cpp.
		struct HairMedullaProfile
		{
			enum { kMaxBins = 64 };

			unsigned int	nBins;
			Scalar			bins[kMaxBins];	//!< density per radian at each bin CENTRE
			Scalar			longVariance;	//!< radians^2, to ADD to the lobe's M_p variance
			bool			mirrored;		//!< true when the caller's b was negative and
											//!< d_phi must be negated on the way in / out

			//! Azimuthal density at deflection `dphi` (any real; wrapped
			//! into [-pi, pi)).  Integrates to 1 over the circle.
			Scalar Eval( Scalar dphi ) const;

			//! Draw a deflection from exactly the density `Eval`
			//! reports.  `u` in [0, 1).
			Scalar Sample( Scalar u ) const;
		};

		//! Interpolate the baked table at (bSigned, tau, g) into a
		//! full sampleable profile.  The ONLY table entry point: both
		//! the evaluation side (`LobeWeights`) and the sampling side
		//! (`HairSPF::DoScatter`) go through it, so they cannot read
		//! different cells.  A single-deflection variant that skipped
		//! materialising the whole profile was written and deleted --
		//! it saved ~240 multiply-adds on the eval path but was a
		//! second, separately-testable interpolation of the same table,
		//! which is exactly the kind of duplicate this file exists to
		//! avoid.
		//!
		//! `bSigned` in [-1, 1] -- the SIGNED impact parameter
		//! sin(gamma_t) / kappa; its sign selects the mirror flag.
		//! `tau` and `g` are clamped to the table's extents (the
		//! profile is essentially converged at both tau ends, and the
		//! phase function is only ever authored inside the g range).
		void HairMedullaLookup( Scalar bSigned, Scalar tau, Scalar g,
		                        HairMedullaProfile& out );

	}
}

#endif
