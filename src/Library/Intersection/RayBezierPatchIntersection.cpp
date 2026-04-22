//////////////////////////////////////////////////////////////////////
//
//  RayBezierPatchIntersection.cpp - Analytic ray/bicubic-Bezier-patch
//    intersection via Kajiya's resultant method with robust Bernstein-
//    basis root finding and OQS-powered polynomial solves.
//
//  Algorithm:
//
//    1. Construct two orthogonal planes P1, P2 whose line of intersection
//       is the ray (so a point lies on the ray iff it lies on both planes).
//
//    2. Substitute the bicubic patch P(u,v) into each plane equation:
//           F_k(u,v) = N_k . (P(u,v) - O_k) = 0
//       This gives two bicubic (3,3) polynomials in (u,v).  We store each
//       in power basis as a BiCubicPolynomial — a vector of SmallPolynomials
//       [f_0(v), f_1(v), f_2(v), f_3(v)] with F_k(u,v) = sum_i u^i f_i(v).
//
//    3. Compute the Bezout resultant R(u) = Res_v(F1, F2).  Any (u*,v*)
//       satisfying F1=F2=0 has u* as a root of R.  R has degree up to 18
//       (the classical Kajiya bound for bicubic patches).
//
//    4. Find roots of R(u) in [0,1] robustly via Bernstein-basis subdivision
//       (SolvePolynomialWithinRange).  The convex-hull property prunes empty
//       intervals; subdivision isolates roots.
//
//    5. For each candidate u, the bicubic F1(u, .) reduces to a cubic in v.
//       We upgrade this to a (degenerate) quartic and hand it to the new
//       OQS quartic solver — which auto-dispatches to the OQS-stabilised
//       cubic path when the leading coefficient vanishes.  This keeps the
//       same numerics and Newton-Raphson polishing that the torus uses.
//
//    6. For each v candidate, we run a 2D Newton-Raphson polish on the
//       full system (F1,F2) = 0 starting from (u, v_seed) and then gate
//       on the polished residual.  Newton-polish + residual gate is more
//       discriminating than the raw cubic-solve residual because the
//       resultant u roots from step 4 may be slightly displaced by
//       roundoff, and the residual at the polished pair is what matters
//       for surface accuracy.
//
//    7. Resultant roots alone are not always sufficient: if the ray
//       direction aligns with a patch tangent (common for off-axis views
//       of the Utah teapot), the resultant's univariate roots cluster
//       too tightly for the Bernstein subdivision solver to separate, or
//       the induced cubic-in-v roots fall outside [0,1].  To cover those
//       cases we augment the u roots with a fixed 5-point u grid and the
//       cubic v roots with a 9-point v grid, de-dup polished roots by
//       proximity, and accept the closest valid (u*,v*).  Cost: at most
//       N_u_seeds * N_v_seeds Newton polishes per patch-bbox hit — still
//       a small multiple of bicubic evaluations, not a new asymptotic.
//
//  References:
//
//    [Kajiya 1982]  J.T. Kajiya, "Ray tracing parametric patches",
//                   Proc. SIGGRAPH 1982, pp. 245-254.  The foundational
//                   algebraic-resultant approach this implementation follows.
//
//    [Manocha 1992] D. Manocha and J. Demmel, "Algorithms for intersecting
//                   parametric and algebraic curves I: simple intersections",
//                   ACM TOG 1992.  Extends Kajiya with more robust univariate
//                   root finding (companion-matrix eigenvalues).  We instead
//                   use Bernstein-basis subdivision (Nishita 1990) for the
//                   univariate step — no QR decomposition, no companion
//                   matrix, and trivially range-limited to [0,1].
//
//    [Nishita 1990] T. Nishita, T.W. Sederberg, M. Kakimoto, "Ray tracing
//                   trimmed rational surface patches", SIGGRAPH 1990,
//                   pp. 337-345.  Source of the Bezier-clipping convex-hull
//                   pruning used by the univariate Bernstein solver.
//
//    [Efremov 2005] A. Efremov, V. Havran, H-P Seidel, "Robust and
//                   numerically stable Bezier clipping method for ray
//                   tracing NURBS surfaces", SCCG 2005.  Modern robustness
//                   analysis; informs the 2D Newton polish here.
//
//    [Orellana 2020] A.G. Orellana, C. De Michele, "Algorithm 1010:
//                   Boosting efficiency in solving quartic equations with
//                   no compromise in accuracy", ACM TOMS 46(2), 2020.
//                   The RISE quartic solver (used in step 5) — high-precision
//                   even for near-tangent coefficients.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RayPrimitiveIntersections.h"
#include "../Functions/Polynomial.h"
#include "../Functions/Resultant.h"
#include "../Utilities/Plane.h"
#include "../Utilities/GeometricUtilities.h"

#include <cmath>
#include <vector>

using namespace RISE;

namespace {

// Bernstein-to-power-basis conversion matrix for cubic Bezier.
// Row k gives the coefficients c_i such that
//   (power basis) t^k = sum_i c_i * B_i(t)    — NO, other direction:
// We use it as: if f(t) = sum_i d_i B_i(t), then f(t) = sum_k t^k (sum_i M[k][i] * d_i).
//
// Derivation (expanding Bernstein polynomials in power basis):
//   B_0(t) = 1 - 3t + 3t^2 - t^3  -> row: [ 1, -3,  3, -1] indexed by power
//   B_1(t) = 3t - 6t^2 + 3t^3     -> row: [ 0,  3, -6,  3]
//   B_2(t) = 3t^2 - 3t^3          -> row: [ 0,  0,  3, -3]
//   B_3(t) = t^3                  -> row: [ 0,  0,  0,  1]
// Transposing so M[k][i] is the t^k coefficient when substituting B_i(t):
static const Scalar BERN_TO_POWER[4][4] = {
	{  1.0,  0.0,  0.0,  0.0 },   // t^0: B_0
	{ -3.0,  3.0,  0.0,  0.0 },   // t^1: -3 B_0 + 3 B_1
	{  3.0, -6.0,  3.0,  0.0 },   // t^2: 3 B_0 - 6 B_1 + 3 B_2
	{ -1.0,  3.0, -3.0,  1.0 }    // t^3: -B_0 + 3 B_1 - 3 B_2 + B_3
};

// Construct two orthonormal planes whose intersection is the given ray.
// Any point p lies on the ray iff n1.(p-o) = 0 AND n2.(p-o) = 0.
//
// To pick n1, n2: find an axis-aligned helper vector that is not
// parallel to the ray direction, cross with ray.Dir to get n1, then
// cross ray.Dir with n1 to get a mutually orthogonal n2.  Choosing
// the axis with smallest |component along d| maximises |d x axis|
// and avoids numerical degeneracy for axis-aligned rays.
void MakePlanes( Plane& plane1, Plane& plane2, const Ray& ray )
{
	const Vector3& d = ray.Dir();

	const Scalar ax = fabs( d.x );
	const Scalar ay = fabs( d.y );
	const Scalar az = fabs( d.z );

	Vector3 helper;
	if( ax <= ay && ax <= az ) {
		helper = Vector3( 1.0, 0.0, 0.0 );
	} else if( ay <= az ) {
		helper = Vector3( 0.0, 1.0, 0.0 );
	} else {
		helper = Vector3( 0.0, 0.0, 1.0 );
	}

	Vector3 n1 = Vector3Ops::Normalize( Vector3Ops::Cross( d, helper ) );
	Vector3 n2 = Vector3Ops::Normalize( Vector3Ops::Cross( d, n1 ) );

	plane1.Set( ray.origin, n1 );
	plane2.Set( ray.origin, n2 );
}

// Substitute the patch P(u,v) into the scalar plane equation
//    f(u,v) = N . (P(u,v) - O)
// and store as a BiCubicPolynomial in power basis:
//    f(u,v) = sum_{k=0..3} u^k * poly[k](v)
// where each poly[k] is a cubic in v (4 coefficients, power basis).
//
// With P(u,v) = sum_{i,j} B_i(u) B_j(v) P_{ij}, define
//    d_{ij} = N . (P_{ij} - O)
// and convert both bases to power basis via the BERN_TO_POWER matrix.
// Project the patch's 16 control points onto a plane, track min/max signed
// distances.  Returns true if the patch's convex hull plausibly crosses the
// plane (after a relative tolerance), false when all 16 points fall
// strictly on one side — in which case the ray (which lies IN the plane)
// cannot hit any point of the patch (Bezier convex-hull property).
//
// The tolerance matters: an exact "dmin <= 0 <= dmax" test rejects
// grazing-incidence hits whose true min-distance is 0 but whose computed
// min-distance is a small positive number from dot-product roundoff.  On
// aphrodite, pixel-diff vs the no-prefilter baseline showed ~0.5% of pixels
// darkening by >32 units when we used the exact test — visible artifacts
// along body edges and on the face.  Relaxing to "dmin <= eps && dmax >= -eps"
// with eps scaled by the local d magnitude closes the false-reject hole
// without meaningfully increasing the number of patches that pass through
// to the expensive resultant solve.  (The downstream Newton polish + F1/F2
// residual gate catches any false positives.)
inline bool ProjectPatchOntoPlane(
	const Plane& plane,
	const BezierPatch& patch,
	Scalar (&d)[4][4],
	Scalar& outMin,
	Scalar& outMax )
{
	const Scalar nx = plane.normal.x, ny = plane.normal.y, nz = plane.normal.z;
	const Scalar ox = plane.origin.x, oy = plane.origin.y, oz = plane.origin.z;
	Scalar dmin =  RISE_INFINITY;
	Scalar dmax = -RISE_INFINITY;
	Scalar dabsMax = 0.0;
	for( int i = 0; i < 4; i++ ) {
		for( int j = 0; j < 4; j++ ) {
			const Point3& p = patch.c[i].pts[j];
			const Scalar dij = nx*(p.x - ox) + ny*(p.y - oy) + nz*(p.z - oz);
			d[i][j] = dij;
			if( dij < dmin ) dmin = dij;
			if( dij > dmax ) dmax = dij;
			const Scalar a = (dij < 0.0) ? -dij : dij;
			if( a > dabsMax ) dabsMax = a;
		}
	}
	outMin = dmin;
	outMax = dmax;

	// Relative tolerance: 1e-6 × (largest |d|) plus an absolute floor of
	// 1e-10 for plane-aligned patches where every d is near zero.  Gates
	// only the "confidently all one side" case.
	const Scalar eps = 1e-6 * dabsMax + 1e-10;
	return ( dmin <= eps && dmax >= -eps );
}

// Build the BiCubicPolynomial in power basis from pre-computed d_ij values.
// Kept separate so callers that already have d_ij (ProjectPatchOntoPlane)
// don't redo the 16 dot products.
inline void MakeBiCubicPolyFromD( const Scalar (&d)[4][4], BiCubicPolynomial& bp )
{
	for( int k = 0; k < 4; k++ ) {
		bp.poly[k].numCoef = 4;
		for( int m = 0; m < 4; m++ ) {
			Scalar sum = 0.0;
			for( int i = 0; i < 4; i++ ) {
				const Scalar mi = BERN_TO_POWER[k][i];
				if( mi == 0.0 ) continue;
				for( int j = 0; j < 4; j++ ) {
					sum += mi * BERN_TO_POWER[m][j] * d[i][j];
				}
			}
			bp.poly[k].coef[m] = sum;
		}
	}
}

// Evaluate a bicubic polynomial at (u,v) by Horner's method in each variable.
inline Scalar EvaluateBiCubic( const BiCubicPolynomial& f, const Scalar u, const Scalar v )
{
	Scalar fu[4];
	for( int k = 0; k < 4; k++ ) {
		// Horner in v: fu[k] = ((c3*v + c2)*v + c1)*v + c0
		const SmallPolynomial& p = f.poly[k];
		fu[k] = ((p.coef[3]*v + p.coef[2])*v + p.coef[1])*v + p.coef[0];
	}
	return ((fu[3]*u + fu[2])*u + fu[1])*u + fu[0];
}

// Partial derivative df/du as a scalar at (u,v).
inline Scalar EvaluateBiCubic_dU( const BiCubicPolynomial& f, const Scalar u, const Scalar v )
{
	Scalar fu[4];
	for( int k = 0; k < 4; k++ ) {
		const SmallPolynomial& p = f.poly[k];
		fu[k] = ((p.coef[3]*v + p.coef[2])*v + p.coef[1])*v + p.coef[0];
	}
	// d/du of fu[0] + fu[1]*u + fu[2]*u^2 + fu[3]*u^3
	return fu[1] + 2.0*fu[2]*u + 3.0*fu[3]*u*u;
}

inline Scalar EvaluateBiCubic_dV( const BiCubicPolynomial& f, const Scalar u, const Scalar v )
{
	// df/dv = sum_k u^k * poly[k]'(v), where poly[k]'(v) = c1 + 2*c2*v + 3*c3*v^2.
	Scalar fpv[4];
	for( int k = 0; k < 4; k++ ) {
		const SmallPolynomial& p = f.poly[k];
		fpv[k] = p.coef[1] + 2.0*p.coef[2]*v + 3.0*p.coef[3]*v*v;
	}
	return ((fpv[3]*u + fpv[2])*u + fpv[1])*u + fpv[0];
}

// Combined eval: returns f(u,v), df/du, df/dv in a single Horner pass.
// Shares the 4 Horner-in-v evaluations (for the value) and 4 derivative-
// in-v evaluations across all three outputs.  Roughly halves the FP work
// vs calling EvaluateBiCubic + _dU + _dV separately — each of those did
// its own 4-term Horner-in-v from scratch.  Called from NewtonPolish
// twice per iter (once for F1, once for F2).
inline void EvaluateBiCubicFull(
	const BiCubicPolynomial& F,
	const Scalar u, const Scalar v,
	Scalar& f, Scalar& fu, Scalar& fv )
{
	Scalar a[4], ap[4];                          // a[k] = poly[k](v),  ap[k] = poly[k]'(v)
	for( int k = 0; k < 4; k++ ) {
		const SmallPolynomial& p = F.poly[k];
		const Scalar c0 = p.coef[0], c1 = p.coef[1], c2 = p.coef[2], c3 = p.coef[3];
		a [k] = ((c3*v + c2)*v + c1)*v + c0;
		ap[k] = c1 + v*(2.0*c2 + v*3.0*c3);
	}
	f  = ((a [3]*u + a [2])*u + a [1])*u + a [0];
	fu = a[1] + u*(2.0*a[2] + u*3.0*a[3]);
	fv = ((ap[3]*u + ap[2])*u + ap[1])*u + ap[0];
}

// 2D Newton-Raphson polish on (F1,F2) = 0.
// Quadratic convergence near simple roots.  Capped iterations and a
// bail-out if the step blows up: we drop back to the unrefined (u,v).
//
// Returns true if converged to within tol in both F1 and F2.
bool NewtonPolish(
	const BiCubicPolynomial& F1,
	const BiCubicPolynomial& F2,
	Scalar& u, Scalar& v,
	const Scalar tol,
	const int maxIter )
{
	for( int it = 0; it < maxIter; it++ ) {
		// Use the combined evaluator — one call gives value + both partials
		// for each F, sharing the Horner-in-v chain.  Saves ~40% of the
		// multiplications compared to calling EvaluateBiCubic + _dU + _dV
		// separately (which each did their own 4-term Horner-in-v).
		Scalar f1, f1u, f1v, f2, f2u, f2v;
		EvaluateBiCubicFull( F1, u, v, f1, f1u, f1v );
		EvaluateBiCubicFull( F2, u, v, f2, f2u, f2v );
		if( fabs(f1) < tol && fabs(f2) < tol ) return true;

		const Scalar det = f1u*f2v - f1v*f2u;
		if( fabs(det) < 1e-30 ) return false;          // singular Jacobian
		const Scalar inv = 1.0 / det;

		// Newton step (u,v) -= J^{-1} * (f1,f2)
		const Scalar du = ( f2v*f1 - f1v*f2 ) * inv;
		const Scalar dv = ( f1u*f2 - f2u*f1 ) * inv;

		u -= du;
		v -= dv;

		// Bail if step exploded (divergent or far outside [0,1])
		if( !(u == u) || !(v == v) ) return false;     // NaN check
		if( u < -0.25 || u > 1.25 || v < -0.25 || v > 1.25 ) return false;
	}
	// Final residual test — may return true even past maxIter if already tight.
	const Scalar f1 = EvaluateBiCubic( F1, u, v );
	const Scalar f2 = EvaluateBiCubic( F2, u, v );
	return fabs(f1) < tol && fabs(f2) < tol;
}

// Distance from ray origin to a world-space point, using the largest
// direction component for numerical stability (avoids cancellation when
// two components of Dir are near zero).  Identical idea to the helper
// in RayBilinearPatchIntersection.cpp.
inline Scalar RayDistanceToPoint( const Ray& ray, const Point3& p )
{
	const Vector3& d = ray.Dir();
	const Scalar ax = fabs(d.x), ay = fabs(d.y), az = fabs(d.z);
	if( ax >= ay && ax >= az ) {
		return ( p.x - ray.origin.x ) / d.x;
	} else if( ay >= az ) {
		return ( p.y - ray.origin.y ) / d.y;
	} else {
		return ( p.z - ray.origin.z ) / d.z;
	}
}

// Self-hit guard for analytic Bezier intersection.  The rest of RISE uses
// SURFACE_INTERSEC_ERROR = 1e-12 as its self-hit gate — that value is fine
// for closed-form intersections (sphere, torus) whose hit point is computed
// to machine-epsilon from a polynomial root, but it is well below the
// residual floor of the (F1,F2) 2D Newton polish used here (~1e-10 in t for
// unit-length ray directions).  Any shadow/reflection ray originating from
// a Bezier-patch hit therefore finds a spurious near-zero-t root from
// Newton converging back to (u*, v*) of the primary hit.
//
// 1e-6 sits ~10000x above the Newton floor and ~30 ppm of a unit-scale
// patch — well below any feature we render — so it cleanly separates the
// "same surface point" case from legitimate adjacent-patch hits without
// letting grazing self-shadows through.
static const Scalar BEZIER_SELF_HIT_EPSILON = 1e-6;

// If this (u,v) gives a valid, closer hit than the current one, update hit.
void AccumulateRoot(
	BEZIER_HIT& hit,
	const Ray& ray,
	const Scalar u,
	const Scalar v,
	const BezierPatch& patch )
{
	// Evaluate surface point at (u,v) and project onto the ray to get t.
	const Point3 P  = GeometricUtilities::EvaluateBezierPatchAt( patch, u, v );
	const Scalar t  = RayDistanceToPoint( ray, P );
	if( t < BEZIER_SELF_HIT_EPSILON || t > hit.dRange ) return;

	// Reject spurious ray hits: the resultant-derived (u,v) must actually
	// correspond to a point on the ray — the two-plane parameterisation
	// guarantees this up to Newton convergence, but a wide distance gate
	// costs almost nothing and guards against ill-conditioned cases.
	const Point3 onRay = ray.PointAtLength( t );
	const Scalar dx = P.x - onRay.x;
	const Scalar dy = P.y - onRay.y;
	const Scalar dz = P.z - onRay.z;
	if( dx*dx + dy*dy + dz*dz > 1e-6 ) return;

	hit.bHit   = true;
	hit.dRange = t;
	hit.u      = u;
	hit.v      = v;
}

} // anonymous namespace

namespace RISE {

void RayBezierPatchIntersection(
	const Ray& ray,
	BEZIER_HIT& hit,
	const BezierPatch& patch
	)
{
	hit.bHit   = false;
	// Caller sets hit.dRange to the current closest-hit distance BEFORE
	// calling us — this lets AccumulateRoot's "t > hit.dRange" gate reject
	// far-away (u*,v*) pairs immediately, closing the loop with the BSP
	// tree's own front-to-back closest-hit pruning.  If the caller hasn't
	// set it (e.g., primary hit with no prior closer hit), the BEZIER_HIT
	// default constructor leaves it at RISE_INFINITY, which is the old
	// behaviour.
	hit.u      = 0.0;
	hit.v      = 0.0;

	// --- Step 1: two orthogonal planes whose intersection is the ray.
	Plane plane1, plane2;
	MakePlanes( plane1, plane2, ray );

	// --- Step 1.5: Bezier convex-hull pre-filter.  Profile showed that ~70%
	// of rays which passed the patch AABB test (the only pre-filter the
	// BSP tree applies) still missed the patch surface — the AABB is far
	// looser than the true patch.  Project all 16 control points onto each
	// plane; if they all fall on the same side of either plane, the convex
	// hull of the patch doesn't cross the plane, so the ray (which lies in
	// both planes) cannot hit the surface.  The d_ij values we compute here
	// are the exact input MakeBiCubicPolyFromD needs, so when the check
	// passes we hand them through with no duplicate work — the optimisation
	// pays its cost only when it rejects, and costs zero when it passes.
	Scalar d1[4][4], d2[4][4];
	Scalar d1min, d1max, d2min, d2max;
	if( !ProjectPatchOntoPlane( plane1, patch, d1, d1min, d1max ) ) return;
	if( !ProjectPatchOntoPlane( plane2, patch, d2, d2min, d2max ) ) return;

	// --- Step 2: build the two bicubics from the already-computed d_ij.
	BiCubicPolynomial F1, F2;
	MakeBiCubicPolyFromD( d1, F1 );
	MakeBiCubicPolyFromD( d2, F2 );

	// Newton convergence tolerance.  Absolute rather than relative to a
	// "patch scale" derived from F1/F2 coefficients — that derived scale
	// varied with ray orientation (because the plane-projected coefficients
	// rotate with the helper axis selection in MakePlanes), which caused
	// asymmetric hit/miss behaviour between identical geometry under
	// different object orientations.  A fixed 1e-10 residual is well under
	// the noise floor of any patch with control points in the [-10, 10]
	// range, which covers every scene we ship.
	const Scalar residualTol = 1e-10;

	// --- Step 3: Bezout resultant eliminates v, yielding R(u) of degree <= 18.
	// Pass a tiny epsilon so Resultant() uses the full Bezout3 form whenever
	// the leading-column coefficients are numerically nonzero — the
	// lower-order Bezout fallbacks give an INCORRECT resultant for a truly
	// bicubic patch (they silently drop the highest-order terms rather than
	// preserving them).  They are only legitimate when the patch really is
	// biquadratic / bilinear, which is vanishingly rare in practice.
	const Scalar resultantEps = 0.0;
	const SmallPolynomial R = Resultant( F1, F2, resultantEps );

	// Check for trivial resultant (identically zero within tolerance):
	// indicates a degenerate configuration — the plane pair contains the
	// entire patch, or the patch degenerates to a lower-dimensional curve.
	// Fall through to miss; mesh path handles these rare scenes.
	{
		Scalar maxAbs = 0.0;
		for( unsigned int i = 0; i < R.numCoef; i++ ) {
			const Scalar a = fabs( R.coef[i] );
			if( a > maxAbs ) maxAbs = a;
		}
		if( maxAbs < 1e-14 ) return;
	}

	// --- Step 4: find u roots in [0,1] via Bernstein-basis subdivision.
	// Trim trailing zero coefficients so the solver works on the true degree.
	// The Bezout formulation over-allocates numCoef; leading zeros don't hurt
	// correctness but shrink the effective degree, which matters when
	// leading-coef magnitudes are near the noise floor.
	std::vector<Scalar> resCoef( R.coef, R.coef + R.numCoef );
	while( resCoef.size() > 1 && fabs( resCoef.back() ) < 1e-14 ) {
		resCoef.pop_back();
	}
	std::vector<Scalar> uRoots;
	uRoots.reserve( 18 );
	// Slightly relaxed bracketing epsilon: 1e-8 was empirically too tight for
	// shallow-incidence rays on a teapot (roots cluster near u = 0 or 1).
	const Scalar uRootEps = 1e-7;
	Polynomial::SolvePolynomialWithinRange( resCoef, -1e-5, 1.0 + 1e-5, uRoots, uRootEps );

	// --- Steps 5-6: for each u seed, Newton-polish the (F1,F2) system.
	//
	// Primary seeds are the resultant u roots.  The pre-2026 code also added
	// a uniform 5-point u fallback grid unconditionally, which cost ~50 extra
	// Newton polishes per patch-bbox hit (5 u's * ~10 v seeds each).  That
	// fallback existed to paper over the integer-factorial overflow in the
	// pre-Pascal-table choose() — the resultant coefficients were wrong for
	// degree >= 13 polynomials, so the solver silently missed legitimate
	// roots and the fallback grid re-found them via Newton.
	//
	// With the Pascal-table fix the resultant is numerically correct, so we
	// keep the fallback only as a last resort when the solver finds NOTHING
	// (the rare truly-grazing case where |F1|, |F2| stay near zero along a
	// curve rather than at a point).  For 99%+ of rays that hit a patch,
	// uSeeds == uRoots and the extra work is gone.
	std::vector<Scalar> uSeeds;
	uSeeds.reserve( uRoots.size() + 5 );
	for( std::vector<Scalar>::const_iterator it = uRoots.begin(); it != uRoots.end(); ++it ) {
		Scalar u = *it;
		if( u < 0.0 ) u = 0.0;
		if( u > 1.0 ) u = 1.0;
		uSeeds.push_back( u );
	}
	// 3-point u fallback (boundaries + centre).  An experiment with 5-point
	// (adding 0.25 and 0.75) was perf-negative: restoring them increased
	// wall time ~15% but moved the aphrodite-silhouette pixel-diff vs
	// V2-final statistically in the same place (85 darker >128 pixels vs
	// 74; within noise of 3×5-fallback).  The extra u seeds don't buy new
	// hits for this scene — Newton from 0 / 0.5 / 1 plus the resultant
	// u-roots already covers every realistic basin — so we keep the
	// cheaper grid.
	{
		const Scalar uFallback[3] = { 0.0, 0.5, 1.0 };
		for( int k = 0; k < 3; k++ ) uSeeds.push_back( uFallback[k] );
	}

	for( std::vector<Scalar>::const_iterator it = uSeeds.begin(); it != uSeeds.end(); ++it ) {
		const Scalar u = *it;

		// F1(u,v) as a cubic in v.
		const SmallPolynomial fV = F1( u );

		// Pack as a monic-ready quartic (RISE coefficient convention:
		// coeff[0] leading, coeff[4] constant).  With coeff[0]=0 the OQS
		// SolveQuartic dispatches to the OQS-stabilised cubic path,
		// which shares the numerics (Vieta-stable quadratic, Newton polish)
		// that made the torus rewrite worthwhile.
		const Scalar quartCoeff[5] = {
			0.0,
			fV.coef[3],
			fV.coef[2],
			fV.coef[1],
			fV.coef[0]
		};

		Scalar vRoots[4];
		int nv = Polynomial::SolveQuartic( quartCoeff, vRoots );

		// Fallback: if the cubic coefficient also vanished (patch degenerate
		// in v for this u) and SolveQuartic returned no roots, try the
		// quadratic directly.  Rare, but keeps us correct.
		if( nv == 0 && fabs( fV.coef[3] ) < 1e-14 ) {
			const Scalar quadCoeff[3] = { fV.coef[2], fV.coef[1], fV.coef[0] };
			Scalar vq[2];
			nv = Polynomial::SolveQuadric( quadCoeff, vq );
			for( int k = 0; k < nv; k++ ) vRoots[k] = vq[k];
		}

		// Natural v seeds are the v roots of F1(u,.) in [0,1].  A fallback
		// 9-point v grid is added only when NONE of the cubic roots survived
		// the range clamp — same reasoning as the u-fallback: with the
		// Pascal-table fix the cubic solve is numerically correct for the
		// common case, so the fallback is a last-resort safety net for
		// genuinely-degenerate patches, not routine work.
		Scalar seeds[16];
		int nseeds = 0;
		for( int j = 0; j < nv; j++ ) {
			Scalar v = vRoots[j];
			if( v < -1e-3 || v > 1.0 + 1e-3 ) continue;
			if( v < 0.0 ) v = 0.0;
			if( v > 1.0 ) v = 1.0;
			seeds[nseeds++] = v;
		}
		// 5-point v fallback (quarters + boundaries).  An attempt to use 9
		// points (eighths) was likewise perf-negative with no quality gain
		// on aphrodite — the denser grid added ~5 Newton polishes per
		// patch-ray but hit the same basins as the 5-point grid on every
		// scene we measured.  Keep 5.
		{
			const Scalar fallback[5] = { 0.0, 0.25, 0.5, 0.75, 1.0 };
			for( int k = 0; k < 5 && nseeds < 16; k++ ) {
				seeds[nseeds++] = fallback[k];
			}
		}

		// Track polished roots we've already accepted so we don't multi-count.
		Scalar acceptedV[16];
		int nAccepted = 0;

		for( int s = 0; s < nseeds; s++ ) {
			Scalar uPol = u;
			Scalar vPol = seeds[s];
			NewtonPolish( F1, F2, uPol, vPol, residualTol, 20 );

			// Re-clamp after polish.
			if( uPol < 0.0 || uPol > 1.0 ) continue;
			if( vPol < 0.0 || vPol > 1.0 ) continue;

			const Scalar f1Here = EvaluateBiCubic( F1, uPol, vPol );
			const Scalar f2Here = EvaluateBiCubic( F2, uPol, vPol );
			if( fabs( f1Here ) > 1e-5 || fabs( f2Here ) > 1e-5 ) continue;

			// De-dup: if we've already accepted a root close to this one,
			// skip.  Newton attracts multiple seeds to the same basin.
			bool dup = false;
			for( int a = 0; a < nAccepted; a++ ) {
				if( fabs( acceptedV[a] - vPol ) < 1e-5 ) { dup = true; break; }
			}
			if( dup ) continue;
			acceptedV[nAccepted++] = vPol;

			AccumulateRoot( hit, ray, uPol, vPol, patch );
		}
	}
}

} // namespace RISE
