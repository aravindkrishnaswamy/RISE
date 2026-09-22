//////////////////////////////////////////////////////////////////////
//
//  TranslucentSPF.cpp - Implementation of the translucent
//  SPF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 21, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TranslucentSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/Optics.h"
#include "../Utilities/RandomNumbers.h"

using namespace RISE;
using namespace RISE::Implementation;
// Hoisted above the anonymous namespace (DL-112 review P1-2): the three
// small helpers `OrientedLobeAxis` / `ExitValidFraction` /
// `kExitVanishThreshold` now live in TranslucentSPF.h so
// `TranslucentBSDF` can renormalize by the SAME valid fraction.  The
// unqualified uses throughout this file are unchanged.
using namespace RISE::Implementation::TranslucentSPFDetail;

namespace
{
	// DL-45 (P1 exact-remap follow-up, 2026-09-13): the diffuse exit re-emission must actually leave the
	// object GEOMETRICALLY, not merely according to the (possibly bump/
	// glint-tilted) shading normal `n` used to build the cosine sample.
	// Sampling unconditionally around `n` and unconditionally popping the
	// IOR stack lets a fraction of "exit" rays point back INTO the solid
	// while the stack now says outside (DL-03's tilted fixture measured
	// 1021/4096 such inward exits per 4096 trials at a 60-degree shading-
	// normal tilt).  A later hit on the same object then gets
	// misclassified.
	//
	// Malley's-method disk projection gives the geometrically-valid
	// fraction of the cosine-weighted hemisphere a closed form:
	//
	//   cosine-around-n sampling <=> uniform sampling on the unit disk via
	//   (x,y) -> (x,y,sqrt(1-x^2-y^2)) in the (u,v,n) frame.  Writing
	//   geomN = cos(phi)*n + sin(phi)*u for the angle phi between n and
	//   geomN, the constraint dot(wo,geomN)>0 reduces on that disk to
	//   x > -cos(phi)*sqrt(1-y^2) -- the region to the right of one branch
	//   of an ellipse with semi-axes cos(phi) (x) and 1 (y) inscribed in
	//   the disk.  The excluded crescent between that arc and the disk
	//   boundary has area (1-cos(phi))*pi/2, so the valid fraction of the
	//   full cosine-weighted hemisphere is exactly
	//
	//     P(valid) = (1 + cos(phi)) / 2
	//
	// DL-45's original fix rejection-sampled against the unclipped cosine
	// distribution until a valid direction turned up (up to 32 attempts),
	// drawing 2 sampler dimensions per attempt.  PT's `ISampler` may
	// be a `SobolSampler`, which partitions dimensions into fixed-size
	// phases (`ISampler::HasFixedDimensionBudget()`, `ISampler.h`) so that
	// every bounce starts its RR / light-selection draws at the same
	// dimension offset regardless of what earlier draws in the bounce
	// consumed.  A variable-length rejection loop breaks that invariant:
	// at 60-degree tilt ~25% of exit scatters drew >=4 dimensions instead
	// of 2, shifting every later Get1D() in the bounce's phase in a
	// tilt-correlated way, and >=16 rejections in a row crossed into the
	// NEXT bounce's phase entirely.
	//
	// Fix: an EXACT, unconditional two-draw remap -- no rejection, no
	// second sampler.  Build a local orthonormal frame (u, v, n) with u
	// the (normalized) component of geomN perpendicular to n, so that
	// geomN = cos(phi)*n + sin(phi)*u by construction (v = n x u is then
	// the frame's third axis).  Malley's-method draws a disk point
	// (x,y) with r=sqrt(1-u1) [NOT sqrt(u1) -- see the comment on `r`'s
	// computation below for why: this is the convention that matches
	// GeometricUtilities::Perturb's existing cos(theta)=sqrt(u) sampler,
	// which TranslucentSpectralParityTest's deterministic inverse-CDF
	// check pins for the untilted (identity-remap) case], psi=2*pi*u2,
	// x=r*cos(psi), y=r*sin(psi).  For FIXED y, the disk's x-chord is
	// x in [-R,R] with R=sqrt(1-y^2)
	// (length 2R); the geometrically-valid sub-chord derived above is
	// x in [-cos(phi)*R, R] (length R*(1+cos(phi))), independent of y.
	// Remapping the already-drawn x into that sub-chord via the constant-
	// Jacobian affine map
	//
	//   x' = (x+R)*(1+cos(phi))/2 - cos(phi)*R
	//
	// turns the unconditional cosine draw into an EXACT, unconditional
	// draw from the valid region using the SAME 2 canonical numbers --
	// same normalized density `cos(theta)/pi / P(valid)` DL-45 derived
	// (z' = sqrt(1-x'^2-y^2) is exactly cos(theta) for the emitted
	// direction), just without the rejection loop or its dimension-count
	// variability.
	//
	// `kExitVanishThreshold` is a defensive guard that is UNREACHABLE from
	// production calls since round 3: every caller first orients the exit
	// frame with OrientedExitNormal(), so cosPhi = |Dot(n, geomN)| >= 0 and
	// P(valid) = (1 + cosPhi)/2 >= 0.5 (pinned by
	// TranslucentSamplerDimensionCountTest at 179/180 deg tilt).  It is
	// kept so that an externally supplied, un-oriented (n, geomN) pair --
	// e.g. a future Pdf() caller passing a raw inward shading normal --
	// makes both the sampler and Pdf()/PdfNM() report "no lobe" / 0 density
	// TOGETHER rather than one side dividing by near-zero while the other
	// still claims support.
	// `kExitVanishThreshold` and `ExitValidFraction` are DEFINED in
	// TranslucentSPF.h (namespace TranslucentSPFDetail) since the DL-112
	// review -- `TranslucentBSDF::value`/`valueNM` need the identical
	// valid fraction, and a second copy would be exactly the kind of
	// sampler/evaluator drift DL-112 exists to close.  The derivation
	// above is their documentation; the header carries only a summary.

	// P1 (review round 3, 2026-09-13): the exit lobe's SAMPLING FRAME has
	// to be oriented outward before any of the above applies.
	//
	// `ri.onb` is built by Object::IntersectRay from `ri.vNormal`, and on
	// a DOUBLE-SIDED triangle mesh the geometry flips BOTH `vNormal` and
	// `vGeomNormal` to face the incoming ray
	// (TriangleMeshGeometry{,Indexed}::IntersectRay).  At a genuine EXIT
	// hit the ray is already travelling outward, so the flipped
	// `ri.onb.w()` points INTO the solid -- while `geomNRaw` (the
	// recovered, un-flipped geometric normal) correctly points out.  With
	// the raw `n`, cos(phi) = dot(n, geomNRaw) is then ~ -1: exactly -1 on
	// a flat-shaded face, so P(valid) = 0 and NO exit lobe is emitted at
	// all (silent total energy loss; the interior ray just ping-pongs on
	// backscatter to the depth cap), and ~ -0.998 on a smooth-shaded one,
	// collapsing the whole lobe into a degrees-wide wedge at the horizon
	// with the density inflated by 1/P(valid).
	//
	// The exit lobe is re-emission into the OUTSIDE, so its axis is the
	// outward-facing shading normal.  Re-orient `n` against the object's
	// own outward direction first; the horizon clip above is then the
	// small correction DL-45 intended (a shading tilt of phi degrees),
	// not a near-total suppression.  On every geometry that does not flip
	// (single-sided meshes, analytical primitives) `dot(n, geomNRaw)` is
	// already positive and this is the identity.
	inline Vector3 OrientedExitNormal( const Vector3& n, const Vector3& geomNRaw )
	{
		return ( Vector3Ops::Dot( n, geomNRaw ) < Scalar(0) ) ? -n : n;
	}

	// Draws a cosine-around-`n` direction that is EXACTLY conditioned on
	// the true geometric horizon `dot(wo,geomN)>0`, using precisely 2
	// canonical sampler draws every call (see the derivation above).
	//
	// DL-112 (2026-09-17): nothing in the construction is specific to the
	// EXIT lobe it was written for -- it samples a plain cosine lobe about
	// any axis, clipped to any half-space through the origin -- so the
	// ENTRY branch's front (reflection) lobe now uses it too, with
	// `geomN` (the ray-anchored geometric normal, the side the incoming
	// ray arrived from) as the half-space instead of `geomNRaw`.  Read
	// the name as "sample a VALID (horizon-conditioned) DIFFUSE
	// direction"; the "exit" is the original caller, not a restriction.
	//
	// The function is otherwise unchanged --
	// never more, regardless of geometry.  Returns false only when the
	// valid region has vanished (see kExitVanishThreshold); the caller
	// must then not emit this lobe, matching Pdf()/PdfNM()'s own return
	// of 0 for the same configuration so sampler and evaluator stay
	// honest about their shared support.
	bool SampleValidDiffuseExit( const Vector3& n, const Vector3& geomN,
		ISampler& sampler, Vector3& outDir, Scalar& outPdf )
	{
		const Scalar cosPhi = r_max( Scalar(-1), r_min( Scalar(1), Vector3Ops::Dot(n,geomN) ) );
		const Scalar pValid = (Scalar(1)+cosPhi) * Scalar(0.5);

		// Exactly 2 draws, unconditionally -- drawn even when the region
		// below turns out to have vanished, so the sampler's dimension
		// counter advances identically regardless of geometry (no branch-
		// dependent consumption; see the fixed-dimension-budget rationale
		// above).
		const Scalar u1 = sampler.Get1D();
		const Scalar u2 = sampler.Get1D();

		if( pValid < kExitVanishThreshold ) {
			return false;
		}

		// Local frame (u, v, n) with geomN in the n-u plane.
		Vector3 u = geomN - cosPhi * n;
		const Scalar uLen2 = Vector3Ops::SquaredModulus( u );
		if( uLen2 > Scalar(1e-12) ) {
			u = u * ( Scalar(1) / sqrt(uLen2) );
		} else {
			// geomN parallel to n (phi ~ 0 or ~180 deg): no preferred tilt
			// direction -- the remap below is either the identity
			// (phi~0, pValid~1) or already rejected above (phi~180,
			// pValid~0).  Any tangent axis works.
			OrthonormalBasis3D arbitrary;
			arbitrary.CreateFromW( n );
			u = arbitrary.u();
		}
		const Vector3 v = Vector3Ops::Cross( n, u );

		// Malley's-method disk sample, then the constant-Jacobian affine
		// remap of the x-chord into the geometrically-valid sub-chord.
		// r = sqrt(1-u1), NOT sqrt(u1): matches the existing plain-cosine
		// convention (GeometricUtilities::Perturb's `cos(theta)=sqrt(u)`,
		// i.e. z=sqrt(u1) before any clipping) that TranslucentSpectralParityTest's
		// deterministic inverse-CDF check pins -- the classic Malley's-
		// method disk radius r=sqrt(u1) gives the complementary (though
		// equally valid, since u1 and 1-u1 are identically distributed
		// for a truly random draw) z=sqrt(1-u1) convention instead, which
		// would silently swap that pinned mapping for a FIXED canonical
		// input without changing the sampled distribution's statistics.
		const Scalar r = sqrt( r_max( Scalar(0), Scalar(1) - u1 ) );
		const Scalar psi = TWO_PI * u2;
		const Scalar x = r * cos(psi);
		const Scalar y = r * sin(psi);
		const Scalar R = sqrt( r_max( Scalar(0), Scalar(1) - y*y ) );

		const Scalar xPrime = (x + R) * (Scalar(1)+cosPhi) * Scalar(0.5) - cosPhi * R;
		const Scalar zPrime = sqrt( r_max( Scalar(0), Scalar(1) - xPrime*xPrime - y*y ) );

		outDir = u*xPrime + v*y + n*zPrime;
		outPdf = ( zPrime * INV_PI ) / pValid;
		return true;
	}

	// DL-68 (2026-09-14): the ENTRY-PUSH mirror of DL-45's exit-pop fix.
	//
	// Two of this SPF's Phong `cos^N` lobes make a SIDE-MEMBERSHIP claim
	// that the sampler never checked:
	//
	//   * the ENTERING transmission lobe, which pushes the IOR stack --
	//     it claims the continuation crossed to the far side of the
	//     surface;
	//   * the exit branch's interior BACKSCATTER lobe, which
	//     deliberately leaves the stack alone -- it claims the
	//     continuation stayed on the side the interior ray arrived from.
	//
	// Both were sampled about the (possibly bump/normal-map/glint-tilted,
	// or merely smooth-shading-interpolated) shading normal with no
	// geometric gate at all, so under tilt a real fraction of each lobe
	// travels the WRONG way while the stack records the other: an entry
	// ray that leaves the way it came in with the object pushed, or a
	// "backscatter" ray that exits without the matching pop.  A later hit
	// on the same object is then misclassified (the DL-03/DL-45 failure
	// mode).  Measured pre-fix on a closed analytic fixture, isotropic
	// N=1, 8192 trials: 548/8192 wrong-side entries at 30 deg tilt,
	// 2039/8192 at 60 deg, 4049/8192 at 89 deg -- matching (1-cos(phi))/2.
	//
	// THE GEOMETRIC REFERENCE.  A transmission lobe's defining property
	// is that it continues THROUGH the surface, i.e. it must leave on the
	// opposite side from the one the incoming ray arrived on.  That is
	// the RAY-ANCHORED `geomN` this file already computes for the entry
	// front (reflection) lobe -- the transmission half-space is its exact
	// complement, `-geomN`, and the interior backscatter's is `+geomN`
	// (the interior ray arrived from the inside).  This is deliberately
	// NOT DL-45's unflipped `geomNRaw`: on a closed object the two agree
	// (a genuine entry travels inward, so `geomN == geomNRaw`), but on an
	// OPEN double-sided sheet struck from its back face -- and translucent
	// is the material authors put on open sheets, cf. DL-46 review round
	// 3(c) / DL-76 -- the object has no "inside" while "through the sheet"
	// is still perfectly well defined, and `-geomNRaw` would aim the
	// transmission straight back at whatever the ray came from.  DL-45's
	// exit lobe needs the opposite treatment for the symmetric reason:
	// its ray is already travelling outward, so the ray-anchored flip
	// lands on the inward direction there.
	//
	// THE SAMPLER.  Rejection is not available: `ISampler` may have a
	// fixed per-bounce dimension budget (`HasFixedDimensionBudget()`,
	// ISampler.h; TranslucentSamplerDimensionCountTest pins this), so the
	// draw count must not depend on geometry.  DL-45's Malley's-method
	// disk remap is not available either -- its disk-projection
	// equivalence is specific to the plain cosine (N=1) case, and these
	// lobes carry an author-controlled `N`.
	//
	// Instead, note that the clipped Phong density is CONSTANT IN AZIMUTH
	// at fixed theta (the lobe is azimuthally symmetric and the clip is a
	// plane through the origin).  So: draw theta from the UNCLIPPED
	// marginal exactly as before, cos(theta) = u1^(1/(N+1)), and then draw
	// the azimuth UNIFORMLY on the valid ARC at that theta.  In the frame
	// (u, v, axis) whose +u carries clipN's tangential part -- so that
	// clipN = cos(phi)*axis + sin(phi)*u -- the constraint
	// dot(w,clipN) > 0 reads
	//
	//     sin(theta)cos(psi)sin(phi) + cos(theta)cos(phi) > 0
	//   <=>  cos(psi) > -cot(theta)cot(phi),
	//
	// an arc of half-width
	//
	//     halfArc(theta) = PI                          if cot(theta)cot(phi) >= 1
	//                    = acos(-cot(theta)cot(phi))   otherwise,
	//
	// centred on +u.  The resulting solid-angle density is closed form:
	//
	//     q(w) = (N+1) * cos^N(theta) / (2 * halfArc(theta)),
	//
	// which collapses to the pre-existing (N+1)cos^N(theta)/(2*PI)
	// whenever the clip is inactive.  Exactly 2 canonical draws, every
	// call, for every N -- and every emitted direction is valid by
	// construction, so the clipped-away energy is RENORMALIZED into the
	// valid region (DL-45's choice) rather than dropped.
	//
	// DL-130 CLOSED 2026-09-21: this renormalizes the DIRECTION density,
	// not the lobe's total transport weight.  Under the shared DL-157
	// contract, `f = kray*q/|cos|` and integral(q)=1, hence
	// integral(f*|cos|)=kray at every tilt.  Scaling `kray` by
	// halfArc/PI here would apply the clip twice and delete energy.
	//
	// DL-111 (2026-09-17) needed this same arc construction for a lobe
	// whose POLAR marginal is not cos^N (DielectricSPF's `scattering` warp
	// draws its angle from either a Phong or a Henyey-Greenstein inverse
	// CDF), and it lives there as `GeometricUtilities::PerturbClipped`.
	// That is a SECOND implementation, deliberately: this one has
	// cos(theta) as a VALUE and uses it directly, while PerturbClipped
	// takes an ANGLE, so delegating would insert an acos/cos round trip
	// into the untilted branch below that TranslucentSpectralParityTest
	// pins bit-for-bit.  **A change to the arc math belongs in BOTH.**
	//
	// NOTE this renormalizes PER THETA RING rather than globally, so it
	// is NOT DL-45's `cos/pi / P(valid)` shape; the theta marginal is
	// deliberately left at the unclipped one, which is what makes the
	// azimuth conditional exactly uniform and therefore exactly
	// invertible.  Both are exact samplers of their own reported density;
	// this one is the one that extends to N != 1.
	//
	// Callers must orient the lobe axis into the half-space first
	// (OrientedLobeAxis), so cos(phi) = |dot(n, clipN)| >= 0 and the arc
	// is never empty: halfArc >= PI/2 always.  (P3-f, review round 2: at
	// the arc's own extreme -- theta -> 0, i.e. u1 -> 1 -- `uAxis` is
	// evaluated but its direction is immaterial, since sin(theta) -> 0
	// multiplies it away in `outDir`; this is the ordinary pole of any
	// spherical parameterization, not a distinct failure mode, and no
	// caller needs to special-case it.  Separately, `psi`'s own
	// endpoints `u2 -> 0` and `u2 -> 1` both land exactly on the arc
	// boundary `Dot(w,clipN) = 0` -- a measure-zero tangent-plane
	// direction that is a valid sample of the closed interval
	// `[-half,half]` and carries the ordinary density `q` computed
	// below, not a degenerate case.)
	// `OrientedLobeAxis` is DEFINED in TranslucentSPF.h (namespace
	// TranslucentSPFDetail) since the DL-112 review, for the same reason
	// as `ExitValidFraction` above: `TranslucentBSDF` must orient the
	// front lobe's axis exactly as the sampler does before measuring the
	// valid fraction against it.
}

namespace RISE { namespace Implementation { namespace TranslucentSPFDetail
{
	// P3-b (review round 2): this function used to live in the anonymous
	// namespace above and masked a violated precondition
	// (`Dot(axis,clipN) < 0`, i.e. a caller that skipped
	// `OrientedLobeAxis`) by clamping `cosPhi` to 0 with `r_max(0,...)`.
	// That clamp did not just under-report -- it corrupted the
	// (axis,uAxis,vAxis) frame: `uAxis = clipN - cosPhi*axis` was built
	// from the WRONG cosPhi, so `uAxis` was no longer orthogonal to
	// `axis`, and the resulting `outDir` was not even a unit vector
	// (reviewer-measured |outDir|=0.722, reported pdf=0.4502, at
	// Dot(axis,clipN)=-0.5) -- a silently WRONG sample a caller could go
	// on to trace, not merely an unnormalized density.  Every production
	// call site in this file orients `axis` via `OrientedLobeAxis`
	// first, so this path is unreachable today; failing loudly (return
	// false, emit nothing) is the `SampleValidDiffuseExit` precedent
	// above -- fail the call, don't manufacture a bad direction -- and
	// is exercised directly by `TranslucentClippedPhongContractTest` /
	// `TranslucentEntryHorizonTest` sub-test 9 (this function is declared
	// in TranslucentSPF.h precisely so those tests can drive it without
	// going through a caller that would never violate the precondition).
	bool SampleClippedPhong(
		const Vector3& axis, const Vector3& clipN, const Scalar N,
		const Scalar u1, const Scalar u2,
		Vector3& outDir, Scalar& outPdf )
	{
		const Scalar cosPhiRaw = Vector3Ops::Dot(axis,clipN);
		if( cosPhiRaw < Scalar(0) ) {
			GlobalLog()->PrintEasyError(
				"TranslucentSPF::SampleClippedPhong:: precondition violated -- "
				"axis is not oriented into the clip half-space (Dot(axis,clipN) < 0); "
				"refusing to sample rather than emit a non-unit direction." );
			outDir = axis;
			outPdf = 0;
			return false;
		}
		const Scalar cosPhi = r_min( Scalar(1), cosPhiRaw );
		const Scalar cosTheta = pow( u1, Scalar(1)/(N+Scalar(1)) );

		// Tangential component of the clip normal in the lobe's frame.
		Vector3 uAxis = clipN - cosPhi*axis;
		const Scalar uLen2 = Vector3Ops::SquaredModulus( uAxis );
		if( uLen2 <= Scalar(1e-12) ) {
			// clipN parallel to the lobe axis: the clip is inactive and
			// the whole lobe is already valid.  Reproduce the pre-DL-68
			// draw EXACTLY -- same Perturb call, same azimuth convention,
			// same pdf expression -- so every untilted surface (analytic
			// primitives, flat-shaded faces: the overwhelming majority of
			// production hits) is bit-for-bit unchanged.
			outDir = GeometricUtilities::Perturb( axis, acos(cosTheta), TWO_PI * u2 );
			outPdf = (N + Scalar(1)) * Scalar(0.5) * INV_PI
				* pow( fabs( Vector3Ops::Dot( outDir, axis ) ), N );
			return true;
		}
		uAxis = uAxis * ( Scalar(1) / sqrt(uLen2) );
		const Vector3 vAxis = Vector3Ops::Cross( axis, uAxis );

		const Scalar sinPhi = sqrt( r_max( Scalar(0), Scalar(1) - cosPhi*cosPhi ) );
		const Scalar sinTheta = sqrt( r_max( Scalar(0), Scalar(1) - cosTheta*cosTheta ) );

		// Half-width of the valid azimuth arc at this theta, written as a
		// ratio comparison rather than cot(theta)*cot(phi) so that
		// theta -> 0 (cot -> infinity) needs no special case: the
		// "whole circle" branch is exactly `num >= denom`.
		Scalar half = PI;
		const Scalar denom = sinTheta * sinPhi;
		const Scalar num = cosTheta * cosPhi;
		if( num < denom ) {
			half = acos( -num/denom );
		}

		const Scalar psi = ( Scalar(2)*u2 - Scalar(1) ) * half;
		outDir = uAxis*(sinTheta*cos(psi)) + vAxis*(sinTheta*sin(psi)) + axis*cosTheta;
		outPdf = (N + Scalar(1)) * pow( cosTheta, N ) / ( Scalar(2) * half );
		return true;
	}

	//////////////////////////////////////////////////////////////////////
	//  DL-157 / DL-41 / DL-38 -- the shared per-hit lobe set.
	//
	//  `Scatter`/`ScatterNM` DRAW from these lobes; `Pdf`/`PdfNM` report
	//  the density of what `RandomlySelect` returns from them; and
	//  `TranslucentBSDF::value`/`valueNM` return each one's own
	//  `kray * pdf / |cos|`.  All three read the set built here, so the
	//  sampler, its density and its evaluator describe one function per
	//  side by construction rather than by three parallel transcriptions
	//  (which is precisely how DL-41 and DL-157 arose).
	//////////////////////////////////////////////////////////////////////

	bool EvalLobe( const Lobe& lobe, const Vector3& w,
		Scalar& outPdf, Scalar& outFOverKray )
	{
		const Scalar cosTheta = Vector3Ops::Dot( w, lobe.axis );
		if( cosTheta <= 0 ) return false;
		if( Vector3Ops::Dot( w, lobe.clipN ) <= 0 ) return false;

		if( !lobe.isPhong ) {
			// DL-45 / DL-112's clipped cosine: `cos/pi` renormalized by the
			// exact valid fraction `(1+cos phi)/2` its 2-draw Malley remap
			// covers.  `f = kray * pdf / cos` cancels the cosine outright.
			const Scalar pValid = ExitValidFraction( lobe.axis, lobe.clipN );
			if( pValid < kExitVanishThreshold ) return false;
			outFOverKray = INV_PI / pValid;
			outPdf       = cosTheta * outFOverKray;
			return true;
		}

		// DL-68's clipped Phong.  `half` is transcribed from
		// SampleClippedPhong above -- deliberately the same arithmetic, so
		// the density this reports is the one that sampler realizes.
		// WHICH GATE CHECKS WHAT: gate 1 in
		// TranslucentLobeConsistencyTest pairs this density with the
		// lobe's own `kray` and `value`, so it catches a NORMALISATION
		// error (a wrong constant in front); only gate 3, a total
		// variation against a histogram of directions the real sampler
		// produced, can catch a wrong SHAPE.  The first draft of this
		// comment claimed gate 1 does both.
		const Scalar cosPhi = r_max( Scalar(0), r_min( Scalar(1),
			Vector3Ops::Dot( lobe.axis, lobe.clipN ) ) );
		Scalar half = PI;
		{
			const Vector3 uAxis = lobe.clipN - cosPhi*lobe.axis;
			if( Vector3Ops::SquaredModulus( uAxis ) > Scalar(1e-12) ) {
				const Scalar sinPhi   = sqrt( r_max( Scalar(0), Scalar(1) - cosPhi*cosPhi ) );
				const Scalar sinTheta = sqrt( r_max( Scalar(0), Scalar(1) - cosTheta*cosTheta ) );
				const Scalar denom = sinTheta * sinPhi;
				const Scalar num   = cosTheta * cosPhi;
				if( num < denom ) {
					half = acos( -num/denom );
				}
			}
		}
		// pow(cosTheta, N-1) rather than pow(cosTheta,N)/cosTheta: the
		// cosine the integrators multiply back in is cancelled in closed
		// form, so a grazing direction never divides by a vanishing number.
		outFOverKray = (lobe.N + Scalar(1)) * pow( cosTheta, lobe.N - Scalar(1) ) / ( Scalar(2)*half );
		outPdf       = cosTheta * outFOverKray;
		return true;
	}

	namespace
	{
		inline void AddLobe( LobeSet& set, bool isPhong, const Vector3& axis,
			const Vector3& clipN, Scalar N, const RISEPel& kray, Scalar krayNM, bool bNM )
		{
			if( set.count >= 4 ) return;
			Lobe& L = set.lobes[set.count++];
			L.isPhong = isPhong;
			L.axis    = axis;
			L.clipN   = clipN;
			L.N       = N;
			L.kray    = kray;
			L.krayNM  = krayNM;
			// EXACTLY what ScatteredRayContainer::RandomlySelect's CDF uses
			// (ColorMath::MaxValue of the RISEPel the sampler built, or the
			// raw krayNM on the spectral pipe).  DL-98/DL-99 review lesson
			// (3): reduce in the SAME order the sampler does.
			L.selectWeight = bNM ? krayNM : ColorMath::MaxValue( kray );
			set.totalSelectWeight += L.selectWeight;
		}
	}

	void BuildLobeSet(
		const IPainter& refFront,
		const IPainter& trans,
		const IScalarPainter& extinction,
		const IScalarPainter& phongN,
		const IScalarPainter& scattering,
		const RayIntersectionGeometric& ri,
		const IORStack* pIorStack,
		const bool bNM,
		const Scalar nm,
		LobeSet& out )
	{
		out.count = 0;
		out.totalSelectWeight = 0;

		// The two geometric references, recovered exactly as Scatter() does
		// (DL-70's un-flip first; hair and a degenerate normal fall back to
		// the shading normal, which makes every gate a no-op there).
		const Vector3 n = ri.onb.w();
		out.n = n;
		const Vector3 trueGeomNormal = ri.HasTrueGeomSide() ? ri.UnflippedGeomNormal() : n;
		const Vector3 geomNRaw = ( Vector3Ops::SquaredModulus( trueGeomNormal ) > Scalar(1e-12) )
			? trueGeomNormal : n;

		// The side.  With a live stack this is bit-for-bit Scatter()'s own
		// `!ior_stack.containsCurrent()`.  Without one it is the geometric
		// reading of the same question -- which face the incoming ray
		// struck -- exact for a closed object and for a double-sided mesh
		// (the un-flip above is what makes the latter true).
		// Without a stack the side has to be inferred, and the ray anchor
		// is EXACT for a closed object: a ray entering one travels inward
		// at its boundary hit.  It is NOT exact on an OPEN sheet, where a
		// back-face-first hit is a genuine ENTRY with a leaving ray.
		//
		// ⚠ `ri.bOpenSheet` IS NOT THE FLAG TO ASK, and review round 2
		// measured what asking it costs.  For the two mesh classes that
		// flag means UNCERTIFIED, not open: `TriangleMeshGeometryIndexed`
		// sets it whenever DL-143's build-time weld could not certify
		// watertightness -- all four glTF assets DL-143 audited, every one
		// a closed solid -- and the non-indexed twin sets it on every
		// double-sided hit.  Round 2 briefly used it here, and at a
		// genuine interior EXIT on such a mesh the stackless path then
		// priced the ENTRY lobes: measured `E[kray]/E[value*cos/pdf]`
		// 0.866690 / 0.865710 / 0.865039 on the diffuse exit and
		// 0.321477 / 0.310505 / 0.313584 on the backscatter at tilt
		// 0/30/60, against 1.000000 everywhere with the clause gone.
		//
		// `ri.bProvablyNoInterior` is the flag that means what is needed,
		// and exactly one class sets it: `ClippedPlaneGeometry`, whose
		// four corners span ONE bounded bilinear sheet.  A mesh can never
		// set it, because "not certified closed" is not "certified open";
		// neither can any geometry holding a COLLECTION of primitives,
		// since N interior-free sheets can bound a volume no single sheet
		// can -- `BezierPatchGeometry` stamped it in round 2 and was
		// removed in round 3 for exactly that reason (its `patches` is a
		// vector, and a `.bezier` file's 28 patches load into one
		// geometry).  Where the flag is absent the ray anchor is what is
		// left, and it is right for the interior exit and wrong for the
		// sheet's back face (the residual on DL-223).
		//
		// MOST but not all callers are stacked.  The stackless ones are
		// the translucent photon-map gather
		// (`TranslucentPelPhotonMap::RadianceEstimate`, whose shader op
		// HAS a stack but whose `IPhotonMap` interface does not carry
		// one), the three global/caustic photon maps and `PhotonMap.h`'s
		// own gather,
		// `PointSetOctree`'s SSS irradiance cache, the interactive
		// preview and `ManifoldSolver`'s four SMS sites -- sixteen calls
		// across eight files, enumerated on DL-223(4). Both FinalGather
		// arms now carry the stack. The modern paths -- PT NEE, BDPT/VCM
		// connections, the zero-exitance sweep, PT guiding -- are all
		// STACKED.
		const bool bEntering = pIorStack
			? !pIorStack->containsCurrent()
			: ( ri.bProvablyNoInterior || Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 );

		// `geomN` -- the side the INCOMING ray arrived from -- is what
		// `Scatter`'s ENTRY lobes are clipped against, and it is NOT
		// derivable from the side alone.
		//
		// WHAT `Scatter` ACTUALLY DOES, per branch (read it above, ~:760):
		//
		//   ENTRY  front reflection  clip = geomN   (RAY-anchored)
		//          transmission      clip = -geomN  (RAY-anchored)
		//   EXIT   backscatter       clip = geomN   (RAY-anchored)
		//          diffuse exit      clip = geomNRaw, axis = OrientedExitNormal(n, geomNRaw)
		//                                           (already ray-INDEPENDENT -- DL-45)
		//
		// THE ENTRY SIDE NEEDS THE RAY, and a stack-derived
		// `bEntering ? geomNRaw : -geomNRaw` is WRONG there on a
		// reachable record: a camera ray striking the BACK FACE FIRST of
		// an open double-sided sheet has a not-inside stack (true -- it
		// never entered) and a leaving ray (true -- it hit the back
		// face), so the two disagree.  `clippedplane_geometry`'s
		// `doublesided` DEFAULTS TO TRUE, and translucent is what authors
		// put on open sheets (DL-46 review round 3(c)), so this is a
		// first-class authoring case, not a corner.  Measured on that
		// record with a stack-derived frame (round 3's re-measurement,
		// superseding round 1's 0.840 / 1.402 -- which came from a gate
		// that drove only the stackless entry point): gate 1's
		// `E[kray]/E[value*cos/pdf]` read 1.260222 on the front-reflection
		// lobe and 2.336741 on the transmission lobe at zero tilt, on
		// BOTH entry points, where all four must read 1.000.  So the entry side uses LITERALLY
		// `Scatter`'s own expression.
		//
		// THE EXIT SIDE MUST NOT, and cannot be made to.
		// `PathVertexEval::EvalBSDFAtVertex` rebuilds a record as
		// `Ray(vertex.position, -wo)`, and the two BDPT generators pass
		// their `(wi, wo)` in OPPOSITE roles -- the eye walk as
		// `(scatDir, -currentRay.Dir())`, so the rebuilt ray IS the
		// incoming segment, and the light walk as
		// `(-currentRay.Dir(), scatDir)`, so it is the REVERSE of the
		// outgoing one.  On the light side that puts BOTH exit-branch
		// lobes in the same half-space and leaves the interior direction
		// the walk is actually asking about with no lobe at all, so
		// `f == 0` and `GenerateLightSubpathImpl`'s
		// `PositiveMagnitude(f)` gate kills the walk --
		// `TranslucentIORStackTest`'s BDPT light rows measured
		// `reached=512 -> 0` on every mode.
		//
		// It does not have to: on the EXIT side `Scatter`'s own `geomN`
		// IS `-geomNRaw` at every record its exit branch can be reached
		// with.  That branch requires `ior_stack.containsCurrent()` --
		// the walk is inside the object -- and a ray inside an object
		// travels outward at its boundary hit, which is exactly
		// `Dot(geomNRaw, rayDir) > 0`.  So `-geomNRaw` is not an
		// approximation of Scatter there; it is the same value, computed
		// without the ray.  (The one record where it is not is a stack
		// that claims "inside" for a ray that is entering -- the open-
		// sheet parity failure DL-76 tracks, where the exit branch has no
		// meaning in the first place.)
		//
		// WHAT IS LEFT, stated precisely.  A LIGHT-subpath ENTRY vertex
		// evaluated through the rebuilt record gets the ray-anchored
		// frame computed from `-scatDir`, which is inverted relative to
		// the walk's own incoming segment.  That is not a new defect and
		// not closable here: it is the caller-convention half of DL-223,
		// the same role-swap that makes a NON-RECIPROCAL BSDF give two
		// answers for one direction pair.  The rule below is the one that
		// is right for PT / NEE and for the eye subpath (both of which
		// hold a record whose ray IS the incoming ray) and that keeps the
		// light subpath alive; the residual is measured on DL-223.
		const Vector3 geomN = bEntering
			? ( ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw )
			: -geomNRaw;

		if( bEntering )
		{
			// --- entry front reflection (clipped cosine, kray = ref) ---
			const RISEPel refC = bNM ? RISEPel(0,0,0) : refFront.GetColor(ri);
			const Scalar  refN = bNM ? GuardedGetColorNM( refFront, ri, nm ) : Scalar(0);
			if( bNM ? (refN > 0) : (ColorMath::MaxValue(refC) > 0) ) {
				const Vector3 nFront = OrientedLobeAxis( n, geomN );
				if( ExitValidFraction( nFront, geomN ) >= kExitVanishThreshold ) {
					AddLobe( out, false, nFront, geomN, Scalar(1), refC, refN, bNM );
				}
			}

			// --- entry transmission (clipped Phong, kray = tau) ---
			const RISEPel tauC = bNM ? RISEPel(0,0,0) : trans.GetColor(ri);
			const Scalar  tauN = bNM ? GuardedGetColorNM( trans, ri, nm ) : Scalar(0);
			if( bNM ? (tauN > 0) : (ColorMath::MaxValue(tauC) > 0) ) {
				const Vector3 intoSolid = -geomN;
				const Vector3 nEnter = OrientedLobeAxis( n, intoSolid );
				if( bNM ) {
					AddLobe( out, true, nEnter, intoSolid, phongN.GetValueAtNM(ri,nm),
						RISEPel(0,0,0), tauN, bNM );
				} else {
					const ScalarTriple Nt = phongN.GetValuesAt(ri);
					if( (Nt.v[0] == Nt.v[1]) && (Nt.v[1] == Nt.v[2]) ) {
						AddLobe( out, true, nEnter, intoSolid, Nt.v[0], tauC, 0, bNM );
					} else {
						for( int i = 0; i < 3; i++ ) {
							RISEPel chan( 0, 0, 0 );
							chan[i] = tauC[i];
							AddLobe( out, true, nEnter, intoSolid, Nt.v[i], chan, 0, bNM );
						}
					}
				}
			}
			return;
		}

		// --- exit side ---
		const Scalar distance = Vector3Ops::Magnitude(
			Vector3Ops::mkVector3( ri.ray.origin, ri.ptIntersection ) );

		// DL-38: the interior segment's Beer extinction is part of what this
		// side of the BSDF is, and it is a function of `ri` -- the incoming
		// ray's own origin -- so a reverse / NEE evaluation at a REAL
		// intersection record sees exactly the attenuation the sampler
		// charged.  (A BDPT / VCM connection evaluates through a record
		// rebuilt by `PathVertexEval::PopulateRIGFromVertex`, whose ray
		// origin IS the vertex, so `distance == 0` and `B == 1` there:
		// DL-223, recorded not closed.)
		RISEPel B( 1, 1, 1 );
		Scalar  Bnm = 1;
		if( bNM ) {
			Bnm = exp( -( extinction.GetValueAtNM(ri,nm) * distance ) );
		} else {
			const ScalarTriple abt = extinction.GetValuesAt(ri);
			const RISEPel ab( abt.v[0], abt.v[1], abt.v[2] );
			B = ColorMath::exponential( -distance*ab );
		}

		RISEPel exitKray  = B;
		Scalar  exitKrayNM = Bnm;

		if( bNM ? (Bnm > 0) : (ColorMath::MaxValue(B) > 0) )
		{
			const Vector3 stayInside = geomN;
			const Vector3 nBack = OrientedLobeAxis( n, stayInside );

			if( bNM ) {
				const Scalar scat = scattering.GetValueAtNM(ri,nm);
				if( scat > 0 ) {
					AddLobe( out, true, nBack, stayInside, phongN.GetValueAtNM(ri,nm),
						RISEPel(0,0,0), Bnm*scat, bNM );
					exitKrayNM = Bnm * (Scalar(1)-scat);
				}
			} else {
				const ScalarTriple scat_t = scattering.GetValuesAt(ri);
				const RISEPel scat( scat_t.v[0], scat_t.v[1], scat_t.v[2] );
				if( ColorMath::MaxValue(scat) > 0 ) {
					const ScalarTriple Nt = phongN.GetValuesAt(ri);
					if( (Nt.v[0] == Nt.v[1]) && (Nt.v[1] == Nt.v[2]) ) {
						AddLobe( out, true, nBack, stayInside, Nt.v[0], B*scat, 0, bNM );
						exitKray = B * (RISEPel(1.0,1.0,1.0)-scat);
					} else {
						for( int i = 0; i < 3; i++ ) {
							RISEPel chan( 0, 0, 0 );
							chan[i] = B[i]*scat[i];
							AddLobe( out, true, nBack, stayInside, Nt.v[i], chan, 0, bNM );
							exitKray[i] = B[i] * (Scalar(1)-scat[i]);
						}
					}
				}
			}
		}

		// --- diffuse exit re-emission (clipped cosine) ---
		// Emitted whatever its weight, matching Scatter(): the exit ray is
		// added after the scattering block with no `kray > 0` gate of its
		// own, so a fully extinguished segment still produces a (zero-weight)
		// ray, which `RandomlySelect` can still return when it is the only one.
		{
			const Vector3 nExit = OrientedExitNormal( n, geomNRaw );
			if( ExitValidFraction( nExit, geomNRaw ) >= kExitVanishThreshold ) {
				AddLobe( out, false, nExit, geomNRaw, Scalar(1), exitKray, exitKrayNM, bNM );
			}
		}
	}
} } }

using namespace RISE::Implementation::TranslucentSPFDetail;

TranslucentSPF::TranslucentSPF(
	const IPainter& rF,
	const IPainter& T,
	const IScalarPainter& ext,
	const IScalarPainter& N_,
	const IScalarPainter& scat
	) :
  pRefFront( &rF ),
  pTrans( &T ),
  pExtinction( &ext ),
  pN( &N_ ),
  pScat( &scat )
{
	pRefFront->addref();
	pTrans->addref();
	pExtinction->addref();
	pN->addref();
	pScat->addref();
}

TranslucentSPF::~TranslucentSPF( )
{
	safe_release( pRefFront );
	safe_release( pTrans );
	safe_release( pExtinction );
	safe_release( pN );
	safe_release( pScat );
}

void TranslucentSPF::SetRefFront( const IPainter& v )         { v.addref(); safe_release( pRefFront );   pRefFront   = &v; }
void TranslucentSPF::SetTrans( const IPainter& v )            { v.addref(); safe_release( pTrans );      pTrans      = &v; }
void TranslucentSPF::SetExtinction( const IScalarPainter& v ) { v.addref(); safe_release( pExtinction ); pExtinction = &v; }
void TranslucentSPF::SetN( const IScalarPainter& v )          { v.addref(); safe_release( pN );          pN          = &v; }
void TranslucentSPF::SetScat( const IScalarPainter& v )       { v.addref(); safe_release( pScat );       pScat       = &v; }

void TranslucentSPF::Scatter( 
			const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
			ISampler& sampler,				///< [in] Sampler
			ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
			const IORStack& ior_stack								///< [in/out] Index of refraction stack
			) const
{
	ScatteredRay	front;
	ScatteredRay	trans;

	const Vector3& n = ri.onb.w();

	// Geometric-horizon gate: GlintModifier can tilt the shading normal up
	// to 60 deg off the true surface, so a front-lobe direction that
	// validates against the (tilted) shading normal can still point below
	// the geometric surface -- the continuation ray then tunnels into the
	// solid.  The entering-branch front (reflection) lobe is gated against
	// `geomN` below (ray-anchor sweep: `geomN`'s orientation is anchored
	// to ri.ray.Dir(), not to the shading normal, so a glint tilt cannot
	// flip the gate to the wrong side -- this is the correct reference for
	// a REFLECTION, which must stay on the same side the incoming ray
	// arrived from).  DL-68 (2026-09-14) gave the two `trans` Phong lobes
	// -- the entering transmission and the exit branch's interior
	// backscatter -- the SAME ray-anchored reference, with opposite signs
	// (`-geomN` "through the surface" for the entry push; `+geomN` "stays
	// inside" for the backscatter), clipped EXACTLY rather than gated, so
	// neither drops a sample: see SampleClippedPhong above.
	//
	// The exit-face re-emission (DL-45) is gated separately, against
	// `geomNRaw` WITHOUT the ray-anchor flip: unlike the entry reflection,
	// an exit ray must travel to the OPPOSITE side from where the incoming
	// (interior) ray arrived, i.e. the object's actual outward direction.
	// `ri.vGeomNormal` is NOT unconditionally that direction: a double-
	// sided triangle mesh (TriangleMeshGeometry{,Indexed}::IntersectRay),
	// and BezierPatchGeometry / ClippedPlaneGeometry on a back-face hit,
	// flip it to face whichever side the ray struck, recording that in
	// `ri.bGeomNormalOrientedToRay`, so on such a surface the raw field
	// always faces the ray -- using it as-is would make this gate a
	// no-op on exactly the meshes it exists to catch (P2-1 sibling
	// audit).  Recover the TRUE, author-authored geometric normal with
	// the documented un-flip (RayIntersectionGeometric.h) before using it
	// as the unflipped reference; the geometries that never flip
	// (single-sided triangle meshes and the analytical primitives --
	// sphere, box, ellipsoid, torus, cylinder, plane, disk, bilinear
	// patch) leave the flag false, so this recovery is a no-op for them,
	// and `RayIntersectionGeometric.h` carries the authoritative list.
	// Reusing the ray-anchored `geomN` for the exit gate
	// would validate the WRONG hemisphere -- for a ray already travelling
	// outward (dot(geomNRaw, ri.ray.Dir()) > 0, the characteristic exit
	// signature), the flip below produces `geomN` pointing back INTO the
	// solid.  Degenerate vGeomNormal (SquaredModulus guard, matches
	// GlintModifier.cpp) falls back to the shading normal for both gates,
	// making each a no-op when no independent geometric truth is
	// available.
	// P2-4 (review round 3): the un-flip only recovers a real surface
	// facing when the reported normal IS a surface property.  A hair
	// strand's is derived from the ray itself (HairGeometry sets
	// `bGeomNormalRayDerived`), so un-flipping yields "always away from
	// the ray" -- no geometric truth to gate against.  Fall back to the
	// shading normal there, which makes both gates no-ops, exactly like
	// the degenerate-normal fallback below.
	const Vector3 trueGeomNormal = ri.HasTrueGeomSide() ? ri.UnflippedGeomNormal() : n;
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( trueGeomNormal ) > Scalar(1e-12) )
		? trueGeomNormal : n;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;

	const Vector3	r = ri.ray.Dir();
	Vector3		rv;

	// Use the IOR stack as the authoritative source for inside/outside
	// determination when available, matching DielectricSPF.  The normal-
	// based dot-product test is unreliable for nested translucent objects
	// because a back-scattered ray hitting an enclosing surface from
	// inside the cavity is misclassified as "exiting."
	const bool bEntering = !ior_stack.containsCurrent();

	if( bEntering )
	{
		// Going in
		// Front face
		front.kray = pRefFront->GetColor(ri);
		front.type = ScatteredRay::eRayDiffuse;

		// MaxValue, not channel 0 alone (C2, review round 3): a reflectance
		// painter like (0, 0.5, 0.5) has zero red but non-zero green/blue,
		// and gating on channel 0 alone dropped the WHOLE lobe -- 100% of
		// the green/blue energy along with it.  The exit branch's
		// equivalent gate (below) already uses
		// `ColorMath::MaxValue(front.kray) > 0`; this makes the entry gate
		// consistent with it.
		if( ColorMath::MaxValue(front.kray) > 0 ) {
			// DL-112 (2026-09-17): this lobe always HAD its geometric gate
			// and makes no IOR-stack claim, so nothing was ever
			// misclassified here -- but it DROPPED a below-horizon sample
			// instead of resampling, so the lobe integrated to
			// P(valid) = (1+cos(phi))/2 < 1 under shading-normal tilt
			// rather than to 1.  Measured emitted energy against a 0.3
			// reflectance painter: 0.30000 / 0.22493 / 0.15428 at 0 / 60 /
			// 89 degrees of tilt, matching that closed form exactly
			// (tests/TransmissionPushGateTest.cpp sub-test 8).  And the
			// lobe's AXIS was the raw `n`, never oriented into `geomN`'s
			// hemisphere, so on a double-sided mesh (where `ri.onb.w()`
			// can sit in the opposite hemisphere entirely -- DL-45 review
			// round 3(a), the same trap the exit lobe hit) P(valid) was
			// not bounded below by 0.5 and could collapse the lobe to
			// nothing.
			//
			// Both are closed by the tool DL-45 already built for the exit
			// lobe: orient the axis first, then draw the EXACT 2-draw
			// remap onto the geometrically valid region, which
			// renormalizes the clipped-away energy into it rather than
			// dropping it.  The draw count is unchanged at 2, so the
			// fixed-dimension budget (`ISampler::HasFixedDimensionBudget()`)
			// is untouched.  `Pdf()`'s front branch reports the matching
			// NORMALIZED density.  Identity at zero tilt (P(valid) = 1),
			// which is why no untilted fixture can see this change --
			// `TranslucentSpectralParityTest`'s `Pdf == INV_PI` pin at
			// zero tilt stays exact.
			const Vector3 nFront = OrientedLobeAxis( n, geomN );
			Scalar frontPdf = 0;
			if( SampleValidDiffuseExit( nFront, geomN, sampler, rv, frontPdf ) ) {
				front.ray.Set( ri.ptIntersection, rv );
				front.pdf = frontPdf;
				front.isDelta = false;
				scattered.AddScatteredRay( front );
			}
		}

		trans.kray = pTrans->GetColor(ri);
		trans.type = ScatteredRay::eRayTranslucent;

		// MaxValue, not channel 0 alone -- see the front-lobe gate above
		// (C2, review round 3) for the rationale; same trap, same fix.
		if( ColorMath::MaxValue(trans.kray) > 0 ) {
			// DL-68: the transmission half-space is "through the surface",
			// the exact complement of the front lobe's gate; the lobe axis
			// is the shading normal oriented into it.  See
			// SampleClippedPhong above for the exact two-draw construction
			// and why the ray-anchored `geomN` (not `geomNRaw`) is the
			// reference here.
			const Vector3 intoSolid = -geomN;
			const Vector3 nEnter = OrientedLobeAxis( n, intoSolid );

			const ScalarTriple Nfactor_t = pN->GetValuesAt(ri); const RISEPel Nfactor( Nfactor_t.v[0], Nfactor_t.v[1], Nfactor_t.v[2] );
			if( (Nfactor[0] == Nfactor[1]) && (Nfactor[1] == Nfactor[2]) ) {
				const Scalar u1 = sampler.Get1D();
				const Scalar u2 = sampler.Get1D();
				Scalar transPdf = 0;
				// P3-b: SampleClippedPhong now fails loudly (returns false)
				// on a violated precondition instead of masking it -- never
				// true here since `nEnter` is already oriented, but skip the
				// emit rather than trust an unpopulated rv/transPdf.
				if( SampleClippedPhong( nEnter, intoSolid, Nfactor[0], u1, u2, rv, transPdf ) ) {
					trans.ray.Set( ri.ptIntersection, rv );
					trans.pdf = transPdf;
					trans.isDelta = false;
					trans.ior_stack = new IORStack( ior_stack );
					// translucent_material has no ior parameter -- there is no
					// second medium to enter, so re-push the enclosing medium's
					// own IOR (RadianceEtaScale then sees before==after and
					// returns exactly 1) rather than fabricating a jump to
					// air's 1.0, which would misprice a translucent object
					// nested inside water or glass.
					trans.ior_stack->push( ior_stack.top() );
					GlobalLog()->PrintNew( trans.ior_stack, __FILE__, __LINE__, "ior stack" );
					scattered.AddScatteredRay( trans );
				}
			} else {
				// Add a new ray for each color component
				RISEPel p = trans.kray;
				trans.kray = 0;
				Point2 ptrand( sampler.Get1D(), sampler.Get1D() );
				for( int i=0; i<3; i++ ) {
					// DL-68: same exact two-draw clipped construction per
					// channel; the two canonical numbers stay SHARED across
					// the three channels, so the draw count is unchanged.
					Scalar transPdf = 0;
					// P3-b: fail-loud precondition guard, see above; skip
					// this channel's emit rather than trust a bad sample.
					if( !SampleClippedPhong( nEnter, intoSolid, Nfactor[i], ptrand.x, ptrand.y, rv, transPdf ) ) {
						continue;
					}

					trans.kray = 0;
					trans.kray[i] = p[i];
					trans.ray.Set( ri.ptIntersection, rv );
					trans.pdf = transPdf;
					trans.isDelta = false;
					// `trans` is ONE local reused across all three loop
					// iterations.  `AddScatteredRay` only clears
					// `delete_stack` to false on the CALLER's local after a
					// successful memcpy (ScatteredRayContainer.cpp) -- it does
					// NOT touch the value that gets memcpy'd INTO the stored
					// copy, which is whatever `delete_stack` reads at the
					// moment of the call.  Iteration 0 starts from the
					// freshly-constructed `trans` (delete_stack==true from
					// ScatteredRay's ctor), so its stored copy correctly owns
					// the stack.  Without this re-arm, iterations 1 and 2
					// would memcpy a stored copy with `delete_stack==false`
					// (left over from iteration 0's post-add reset on the
					// local) alongside a BRAND NEW `IORStack` pointer that
					// copy is the only reference to -- an unconditional,
					// two-per-call leak on the success path.  Re-arming right
					// before every new allocation makes each iteration's
					// stored copy independently own its own stack, matching
					// the exit loop below (which never assigns `ior_stack` at
					// all, so it never needed this).
					//
					// This loop never checks `AddScatteredRay`'s return value
					// (debt 31(b), RENDERING_INTEGRATORS.md), so a container
					// near `kCapacity` can still make an iteration's add FAIL
					// (overflow).  On failure `AddScatteredRay` leaves
					// `delete_stack` exactly as it found it -- true, since we
					// just armed it -- so `trans` correctly RETAINS ownership
					// of that iteration's stack.  If nothing intervened, the
					// pointer would then be silently overwritten by the NEXT
					// iteration's `new IORStack` below, orphaning the still-
					// owned allocation (a real leak distinct from the one
					// above).  Free any such carried-over stack before
					// overwriting the pointer; a no-op on the ordinary path
					// (iteration 0's `ior_stack` starts null, and a
					// successful add already cleared `delete_stack` to false,
					// so there is nothing to free).
					if( trans.delete_stack ) {
						safe_delete( trans.ior_stack );
					}
					trans.delete_stack = true;
					trans.ior_stack = new IORStack( ior_stack );
					// See the comment on the single-color-component branch
					// above: translucent_material has no ior, so re-push
					// the enclosing medium's own IOR rather than 1.0.
					trans.ior_stack->push( ior_stack.top() );
					GlobalLog()->PrintNew( trans.ior_stack, __FILE__, __LINE__, "ior stack" );
					scattered.AddScatteredRay( trans );
				}
			}
		}
	}
	else
	{
		// Coming out the other side.  Orient the exit frame outward
		// BEFORE sampling either child (P1, review round 3) -- see
		// OrientedExitNormal above.
		const Vector3 nExit = OrientedExitNormal( n, geomNRaw );

		const Scalar distance = Vector3Ops::Magnitude( Vector3Ops::mkVector3(ri.ray.origin, ri.ptIntersection) );
		const ScalarTriple abt = pExtinction->GetValuesAt(ri);
		const RISEPel ab( abt.v[0], abt.v[1], abt.v[2] );
		front.kray = ColorMath::exponential( -distance*ab );

		front.type = ScatteredRay::eRayDiffuse;

		// Don't bother checking scattering if the ray is totally extinguished
		if( ColorMath::MaxValue(front.kray) > 0 ) {
			// Check the scattering parameter
			const ScalarTriple scat_t = pScat->GetValuesAt(ri);
			const RISEPel scat( scat_t.v[0], scat_t.v[1], scat_t.v[2] );

			if( ColorMath::MaxValue(scat) > 0 ) {
				// Multiple scatter back.  The backscattered ray must stay
				// INSIDE the object (this branch's own "no stack change"
				// contract).  P1 (review round 3) got the lobe's AXIS
				// right by flipping the shading frame conditionally; DL-68
				// closes the remaining gap -- the axis alone does not stop
				// a tilted lobe from emitting past the geometric horizon
				// and out of the solid with the stack untouched (measured:
				// 605/4096 at 45 deg tilt).  "Inside" is the side the
				// interior ray arrived from, i.e. the ray-anchored `geomN`;
				// clip the lobe to it exactly (SampleClippedPhong).
				const Vector3 stayInside = geomN;
				const Vector3 nBack = OrientedLobeAxis( n, stayInside );

				trans.type = ScatteredRay::eRayTranslucent;
				trans.kray = front.kray * scat;

				const ScalarTriple Nfactor_t = pN->GetValuesAt(ri); const RISEPel Nfactor( Nfactor_t.v[0], Nfactor_t.v[1], Nfactor_t.v[2] );
				if( (Nfactor[0] == Nfactor[1]) && (Nfactor[1] == Nfactor[2]) ) {
					const Scalar u1 = sampler.Get1D();
					const Scalar u2 = sampler.Get1D();
					Scalar backPdf = 0;
					// P3-b: fail-loud precondition guard, see above; never
					// true here since `nBack` is already oriented.
					if( SampleClippedPhong( nBack, stayInside, Nfactor[0], u1, u2, rv, backPdf ) ) {
						trans.ray.Set( ri.ptIntersection, rv );
						trans.pdf = backPdf;
						trans.isDelta = false;
						front.kray = front.kray * (RISEPel(1.0,1.0,1.0)-scat);
						// Back-scattered ray stays inside this object, no stack change
						scattered.AddScatteredRay( trans );
					}
				} else {
					// Add a new ray for each color component
					RISEPel p = trans.kray;
					RISEPel f = front.kray;
					trans.kray = 0;
					// `front` is added to `scattered` ONCE, after this whole
					// if/else block (the exit-ray site below) -- unlike
					// `trans`, which is (correctly) added once per channel
					// inside this loop.  So `front.kray` must ACCUMULATE all
					// three channels here, not get reset to 0 each iteration:
					// zero it once, before the loop, and only ever assign
					// (never re-zero) the i'th component inside it.  The
					// previous per-iteration `front.kray = 0;` left every
					// channel but the last (i=2) at zero on the exit ray --
					// the sibling of the entry-loop bug fixed in 74cd56f4.
					front.kray = 0;
					Point2 ptrand( sampler.Get1D(), sampler.Get1D() );
					for( int i=0; i<3; i++ ) {
						// DL-68: same exact two-draw clipped construction
						// per channel, on the shared canonical pair.
						Scalar backPdf = 0;
						// P3-b: fail-loud precondition guard, see above;
						// never true here since `nBack` is already oriented.
						// On the (unreachable) failure path, keep this
						// channel's full pre-scatter flux on the exit ray
						// rather than silently discarding it.
						if( SampleClippedPhong( nBack, stayInside, Nfactor[i], ptrand.x, ptrand.y, rv, backPdf ) ) {
							trans.kray = 0;
							trans.kray[i] = p[i];
							trans.ray.Set( ri.ptIntersection, rv );
							trans.pdf = backPdf;
							trans.isDelta = false;
							front.kray[i] = f[i] * (1.0-scat[i]);
							// Back-scattered ray stays inside this object, no stack change
							scattered.AddScatteredRay( trans );
						} else {
							front.kray[i] = f[i];
						}
					}
				}
			}
		}

		// Exit diffuse ray leaves the object — pop from IOR stack.
		// DL-45: the direction must actually be geometrically outward
		// (dot(rv,geomN)>0), not just above the (possibly tilted) shading
		// horizon -- otherwise we'd pop the stack while the ray still
		// travels into the solid.  See SampleValidDiffuseExit above (P1:
		// an exact 2-draw remap, not a rejection loop); it returns false
		// only when the valid region has vanished (n and geomN nearly
		// opposed), in which case this lobe is simply not emitted this
		// trial, exactly like the entry front-lobe's existing geometric
		// gate above.  The lobe's axis is the OUTWARD-oriented shading
		// normal `nExit`, not the raw (possibly double-sided-flipped)
		// `n` -- P1, review round 3.
		Scalar exitPdf = 0;
		if( SampleValidDiffuseExit( nExit, geomNRaw, sampler, rv, exitPdf ) ) {
			front.ray.Set( ri.ptIntersection, rv );
			front.pdf = exitPdf;
			front.isDelta = false;
			front.ior_stack = new IORStack( ior_stack );
			front.ior_stack->pop();
			GlobalLog()->PrintNew( front.ior_stack, __FILE__, __LINE__, "ior stack" );
			scattered.AddScatteredRay( front );
		}
	}
}

void TranslucentSPF::ScatterNM(
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	ISampler& sampler,				///< [in] Sampler
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack& ior_stack								///< [in/out] Index of refraction stack
	) const
{
	ScatteredRay	front;
	ScatteredRay	trans;

	const Vector3& n = ri.onb.w();

	// Geometric-horizon gates: `geomN` (ray-anchored, mirrors Scatter()'s
	// entry front-lobe gate) and `geomNRaw` (the recovered TRUE, unflipped
	// geometric normal -- DL-45's exit gate; P2-1: see the long
	// comment in Scatter() above for why the exit case needs the
	// UNFLIPPED reference, and for why `ri.vGeomNormal` must first be
	// un-flipped via `ri.bGeomNormalOrientedToRay` on a double-sided
	// mesh).
	// P2-4 (review round 3): the un-flip only recovers a real surface
	// facing when the reported normal IS a surface property.  A hair
	// strand's is derived from the ray itself (HairGeometry sets
	// `bGeomNormalRayDerived`), so un-flipping yields "always away from
	// the ray" -- no geometric truth to gate against.  Fall back to the
	// shading normal there, which makes both gates no-ops, exactly like
	// the degenerate-normal fallback below.
	const Vector3 trueGeomNormal = ri.HasTrueGeomSide() ? ri.UnflippedGeomNormal() : n;
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( trueGeomNormal ) > Scalar(1e-12) )
		? trueGeomNormal : n;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;

	const Vector3	r = ri.ray.Dir();
	Vector3		rv;

	const bool bEnteringNM = !ior_stack.containsCurrent();

	if( bEnteringNM )
	{
		// Extinction check
		front.krayNM = GuardedGetColorNM( *pRefFront, ri, nm );
		front.type = ScatteredRay::eRayDiffuse;

		if( front.krayNM > 0 ) {
			// DL-112, NM twin of the RGB entry front lobe -- see the long
			// note in Scatter() above for the derivation and the measured
			// pre-fix energy.
			const Vector3 nFront = OrientedLobeAxis( n, geomN );
			Scalar frontPdf = 0;
			if( SampleValidDiffuseExit( nFront, geomN, sampler, rv, frontPdf ) ) {
				front.ray.Set( ri.ptIntersection, rv );
				front.pdf = frontPdf;
				front.isDelta = false;
				scattered.AddScatteredRay( front );
			}
		}

		trans.krayNM = GuardedGetColorNM( *pTrans, ri, nm );
		trans.type = ScatteredRay::eRayTranslucent;

		if( trans.krayNM > 0 ) {
			// DL-68, RGB twin of Scatter()'s entering transmission lobe:
			// clip the Phong lobe to the "through the surface" half-space
			// exactly, using the same two canonical draws.
			const Vector3 intoSolid = -geomN;
			const Vector3 nEnter = OrientedLobeAxis( n, intoSolid );

			const Scalar Nval = pN->GetValueAtNM(ri,nm);
			const Scalar u1 = sampler.Get1D();
			const Scalar u2 = sampler.Get1D();
			Scalar transPdf = 0;
			// P3-b: fail-loud precondition guard (RGB twin above); never
			// true here since `nEnter` is already oriented.
			if( SampleClippedPhong( nEnter, intoSolid, Nval, u1, u2, rv, transPdf ) ) {
				trans.ray.Set( ri.ptIntersection, rv );
				trans.pdf = transPdf;
				trans.isDelta = false;
				trans.ior_stack = new IORStack( ior_stack );
				// NM twin of the RGB entry lobe above: translucent_material has
				// no ior parameter, so re-push the enclosing medium's own IOR
				// rather than fabricating a jump to air's 1.0.
				trans.ior_stack->push( ior_stack.top() );
				GlobalLog()->PrintNew( trans.ior_stack, __FILE__, __LINE__, "ior stack" );
				scattered.AddScatteredRay( trans );
			}
		}
	}
	else
	{
		// Coming out the other side.  Orient the exit frame outward
		// BEFORE sampling either child -- RGB twin's P1 (review round 3);
		// see OrientedExitNormal above.
		const Vector3 nExit = OrientedExitNormal( n, geomNRaw );

		const Scalar distance = Vector3Ops::Magnitude( Vector3Ops::mkVector3(ri.ray.origin, ri.ptIntersection) );
		// The primary-layer transmittance was paid on entry, just as in
		// Scatter(). Each interior segment pays only Beer extinction before
		// splitting into exit and backscatter; multiplying pTrans here again
		// attenuates both children a second time (DL-01).
		front.krayNM = exp(-(pExtinction->GetValueAtNM(ri,nm)*distance));

		front.type = ScatteredRay::eRayDiffuse;

		// Don't bother checking scattering if the ray is totally extinguished
		if( front.krayNM > 0 ) {
			// Check the scattering parameter
			const Scalar scat = pScat->GetValueAtNM(ri,nm);

			if( scat > 0 ) {
				// Multiple scatter back.  RGB twin's P1 (review round 3)
				// fixed the lobe AXIS; DL-68 fixes the missing horizon clip --
				// the backscattered ray must stay INSIDE, i.e. on the side the
				// interior ray arrived from (the ray-anchored `geomN`).
				const Vector3 stayInside = geomN;
				const Vector3 nBack = OrientedLobeAxis( n, stayInside );

				const Scalar Nval_scat = pN->GetValueAtNM(ri,nm);
				const Scalar u1 = sampler.Get1D();
				const Scalar u2 = sampler.Get1D();
				Scalar backPdf = 0;
				// P3-b: fail-loud precondition guard (RGB twin above); never
				// true here since `nBack` is already oriented.  On the
				// (unreachable) failure path, leave `front.krayNM` at its
				// pre-scatter value rather than silently discarding it.
				if( SampleClippedPhong( nBack, stayInside, Nval_scat, u1, u2, rv, backPdf ) ) {
					trans.type = ScatteredRay::eRayTranslucent;
					// RGB twin (Scatter(), ~line 182) assigns
					// `trans.kray = front.kray * scat;` -- trans.krayNM was never
					// assigned before this point (ScatteredRay's ctor defaults
					// krayNM to 0), so `*= scat` left this lobe at 0 (dead) in
					// every spectral render.  Mirror the RGB twin: derive it from
					// front.krayNM (the extinction-attenuated pass-through, set
					// just above).
					trans.krayNM = front.krayNM * scat;
					trans.ray.Set( ri.ptIntersection, rv );
					trans.pdf = backPdf;
					trans.isDelta = false;
					// Back-scattered ray stays inside this object, no stack change
					scattered.AddScatteredRay( trans );

					front.krayNM *= (1.0-scat);
				}
			}
		}

		// Exit re-emission is diffuse in both RGB and NM. N controls
		// entry transmission and internal backscatter, not this exit lobe.
		// DL-45: same geometric-horizon requirement as the RGB twin above,
		// and the same outward-oriented lobe axis (P1, review round 3).
		Scalar exitPdf = 0;
		if( SampleValidDiffuseExit( nExit, geomNRaw, sampler, rv, exitPdf ) ) {
			front.ray.Set( ri.ptIntersection, rv );
			front.pdf = exitPdf;
			front.isDelta = false;

			front.ior_stack = new IORStack( ior_stack );
			front.ior_stack->pop();
			GlobalLog()->PrintNew( front.ior_stack, __FILE__, __LINE__, "ior stack" );
			scattered.AddScatteredRay( front );
		}
	}
}

Scalar TranslucentSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ior_stack
	) const
{
	return AggregatePdf( ri, wo, false, 0, ior_stack );
}

//! DL-222: the spectral companion weight, read off the SAME lobe set
//! `ScatterNM` draws from rather than reconstructed as `value*cos/pdf`.
//!
//! `ScatterNM` emits at most two rays and they have DISTINCT types --
//! `eRayDiffuse` (the entry front reflection on the entry side, the
//! diffuse exit on the interior side) and `eRayTranslucent` (the entry
//! transmission, the interior backscatter) -- so `rayType` alone
//! identifies which lobe a companion is asking about, on either side.
//! `outDir` is not needed to make that identification and is unused, but
//! is kept in the signature because it is the interface's and because a
//! future lobe split would need it.
//!
//! The weights themselves are wavelength-dependent through the painters
//! (`GuardedGetColorNM` on `ref`/`tau`; `GetValueAtNM` on
//! `ext`/`scattering`), which is exactly what the companion lane wants
//! and what the `value*cos/pdf` fallback could only approximate.
//////////////////////////////////////////////////////////////////////
// EvaluateLobeFNM -- DL-216.
//
// Evaluates the SELECTED lobe's own spectral BSDF value f_I(wo; nm)
// in [1/sr], without multiplying by cosine and without dividing by
// any sampling density.
//////////////////////////////////////////////////////////////////////
Scalar TranslucentSPF::EvaluateLobeFNM(
	const RayIntersectionGeometric& ri,
	const Vector3& outDir,
	ScatteredRay::ScatRayType rayType,
	Scalar nm,
	const IORStack& ior_stack
	) const
{
	if( rayType != ScatteredRay::eRayDiffuse && rayType != ScatteredRay::eRayTranslucent ) {
		return -1;
	}

	LobeSet set;
	BuildLobeSet( *pRefFront, *pTrans, *pExtinction, *pN, *pScat,
		ri, &ior_stack, true, nm, set );

	const bool bWantPhong = ( rayType == ScatteredRay::eRayTranslucent );
	const Vector3 wo = Vector3Ops::Normalize( outDir );
	for( int i = 0; i < set.count; i++ ) {
		if( set.lobes[i].isPhong == bWantPhong ) {
			Scalar pdf = 0, fOverKray = 0;
			if( EvalLobe( set.lobes[i], wo, pdf, fOverKray ) ) {
				return set.lobes[i].krayNM * fOverKray;
			}
			return 0;
		}
	}
	return 0;
}

Scalar TranslucentSPF::EvaluateKrayNM(
	const RayIntersectionGeometric& ri,
	const Vector3& /*outDir*/,
	ScatteredRay::ScatRayType rayType,
	Scalar nm,
	const IORStack& ior_stack
	) const
{
	if( rayType != ScatteredRay::eRayDiffuse && rayType != ScatteredRay::eRayTranslucent ) {
		return -1;
	}

	LobeSet set;
	BuildLobeSet( *pRefFront, *pTrans, *pExtinction, *pN, *pScat,
		ri, &ior_stack, true, nm, set );

	// The cosine lobe is the `eRayDiffuse` one on BOTH sides (entry front
	// reflection / interior diffuse exit); the Phong lobe is the
	// `eRayTranslucent` one (entry transmission / interior backscatter).
	const bool bWantPhong = ( rayType == ScatteredRay::eRayTranslucent );
	for( int i = 0; i < set.count; i++ ) {
		if( set.lobes[i].isPhong == bWantPhong ) {
			return set.lobes[i].krayNM;
		}
	}
	// The lobe this companion is asking about is not emitted at this
	// wavelength (a zero painter, a fully extinguished segment, a zero
	// scattering split).  That is a real answer, not a decline.
	return 0;
}

Scalar TranslucentSPF::EvaluateKrayNM(
	const RayIntersectionGeometric& ri,
	const Vector3& outDir,
	ScatteredRay::ScatRayType rayType,
	Scalar nm,
	const IORStack& ior_stack,
	Scalar pdfHero
	) const
{
	if( pdfHero <= 0 ) {
		return EvaluateKrayNM( ri, outDir, rayType, nm, ior_stack );
	}

	if( rayType != ScatteredRay::eRayDiffuse && rayType != ScatteredRay::eRayTranslucent ) {
		return -1;
	}

	LobeSet set;
	BuildLobeSet( *pRefFront, *pTrans, *pExtinction, *pN, *pScat,
		ri, &ior_stack, true, nm, set );

	const bool bWantPhong = ( rayType == ScatteredRay::eRayTranslucent );
	const Vector3 wo = Vector3Ops::Normalize( outDir );
	for( int i = 0; i < set.count; i++ ) {
		if( set.lobes[i].isPhong == bWantPhong ) {
			Scalar pdf = 0, fOverKray = 0;
			if( EvalLobe( set.lobes[i], wo, pdf, fOverKray ) ) {
				return ( set.lobes[i].krayNM * pdf ) / pdfHero;
			}
			return 0;
		}
	}
	return 0;
}

Scalar TranslucentSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	return AggregatePdf( ri, wo, true, nm, ior_stack );
}

//! DL-41 (2026-09-18): the density of what `Scatter`/`ScatterNM` plus
//! `ScatteredRayContainer::RandomlySelect` actually generate.
//!
//! Before this, `Pdf` returned ONE lobe -- the entry front reflection or
//! the diffuse exit, whichever side the stack said -- at its full,
//! unweighted density, and reported ZERO over the entire half-space the
//! two Phong lobes (entry transmission, interior backscatter) occupy.
//! That is why DL-69's BDPT `pdfFwd` and DL-103's PT escape-side MIS
//! partner both carry a `misFwdPdf <= NEARZERO` fallback naming this
//! class: the aggregate really did report nothing for a direction the
//! sampler had just generated.  It also integrated to 1 over the sphere,
//! so a mass check could not see it -- only a shape (total-variation)
//! check against the real sampler can, which is
//! `TranslucentLobeConsistencyTest`'s gate 3.
//!
//! The mixture weights are EXACT rather than quadrature-estimated
//! (contrast DL-67/DL-98/DL-99): every lobe's `kray` here is
//! direction-independent, so `RandomlySelect`'s realized probability IS
//! the raw weight ratio.  See the `BuildLobeSet` contract in
//! TranslucentSPF.h.
Scalar TranslucentSPF::AggregatePdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const bool bNM,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	LobeSet set;
	BuildLobeSet( *pRefFront, *pTrans, *pExtinction, *pN, *pScat,
		ri, &ior_stack, bNM, nm, set );

	if( set.count == 0 ) return 0;

	// Mirror RandomlySelect exactly: a single emitted ray is returned with
	// probability 1 whatever its weight, and a degenerate all-zero weight
	// total with two or more rays selects NOTHING at all.
	if( set.count == 1 ) {
		Scalar pdf = 0, fOverKray = 0;
		return EvalLobe( set.lobes[0], wo, pdf, fOverKray ) ? pdf : Scalar(0);
	}
	if( set.totalSelectWeight <= NEARZERO ) return 0;

	Scalar total = 0;
	for( int i = 0; i < set.count; i++ ) {
		Scalar pdf = 0, fOverKray = 0;
		if( !EvalLobe( set.lobes[i], wo, pdf, fOverKray ) ) continue;
		total += ( set.lobes[i].selectWeight / set.totalSelectWeight ) * pdf;
	}
	return total;
}
