//////////////////////////////////////////////////////////////////////
//
//  Polynomial.cpp - Implementation of polynomial solving functions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 21, 2001
//  Tabs: 4
//  Comments: Implemented from Graphics Gems Cubic and Quartic Roots
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Polynomial.h"
#include "../Utilities/FiniteMath.h"
#include <float.h>			// for copysign

using namespace RISE;

static const Scalar	EQN_EPS =    1e-19;

#define	    IsZero(x)	((x) > -EQN_EPS && (x) < EQN_EPS)
#define	    IsReallyZero(x)	(x==0)

#define     cbrt(x)     (((x) > 0.0 ? pow((Scalar)(x), THIRD) : ((x) < 0.0 ? -pow((Scalar)-(x), THIRD) : 0.0)))

int Polynomial::SolveQuadric( const Scalar (&coeff)[ 3 ], Scalar (&sol)[ 2 ] )
{
	//
	// To solve a quadric or quadratic, we just need to
	// apply (-b +- sqrt( b^2 - 4ac )) / 2a
	//

	// Compute d, the stuff inside the square root
	const Scalar& a = coeff[0];
	const Scalar& b = coeff[1];
	const Scalar& c = coeff[2];

	// Degenerate leading coefficient: b*x + c = 0 is LINEAR, not quadratic.
	// `SolveQuadricWithinRange` below has always had this branch; this one
	// used to fall through to the 1/(2a) division and hand back a pair of
	// infinities (or, before the divisor fix below, a pair of exact zeros
	// that satisfy nothing).  The two siblings now agree on this a == 0
	// branch; they still diverge on the d == 0 double-root branch below
	// -- this one guards it with the epsilon-based `IsZero(d)`,
	// `SolveQuadricWithinRange` with an exact `d == 0.0` -- that
	// divergence is untouched here.
	if( a == 0.0 )
	{
		if( b == 0.0 ) {
			return 0;
		}
		sol[0] = -c / b;
		return 1;
	}

	const Scalar	d = b*b - 4*a*c;

	if( IsZero( d ) )
	{
		sol[0] = -b / (2*a);
		return 1;
	}
	else if( d < 0 )
	{
		return 0;
	}
	else
	{
		// (-b +- sqrt(d)) / (2a).  This was written as a multiply by
		// `0.5 * a`, which is the same number as `0.5 / a` only when
		// a == +/-1 -- true by accident for every ray-primitive caller
		// (their leading coefficient is |Dir|^2 on a normalised
		// direction) and false for the general-coefficient routes, which
		// got roots scaled by a^2: `RayBezierPatchIntersection`'s
		// degenerate-v fallback, `QuadraticFunction::Solve`, and
		// `SolveCubic`'s own `IsReallyZero(coeff[0])` branch below --
		// reachable from `SolveQuartic`'s degenerate branch, which is the
		// route `RayBezierPatchIntersection` takes on every call (it
		// always passes `quartCoeff[0] = 0`, a bicubic patch's F1(u,.)
		// being cubic in v, not quartic).
		const Scalar p = 0.5 / a;
		const Scalar sq_d = sqrt( d );

		sol[0] = (-b + sq_d) * p;
		sol[1] = (-b - sq_d) * p;
		return 2;
	}
}

//! Solves a quadratic function within the given range
/// \return Number of solutions
int Polynomial::SolveQuadricWithinRange( 
	const Scalar (&coeff)[ 3 ],				///< [in] Coefficients
	Scalar (&sol)[ 2 ],						///< [out] Solutions
	const Scalar min,						///< [in] Minimum value
	const Scalar max						///< [in] Maximum value
	)
{
	const Scalar& a = coeff[0];
	const Scalar& b = coeff[1];
	const Scalar& c = coeff[2];

	sol[0] = sol[1] = min-min;
	if( a == 0.0 ) {
		if(b != 0.0) {
			sol[0] = -c/b;
			if( sol[0] > min && sol[0] < max ) {
				return 1;
			} else {
				return 0;
			}
		} else {
			return 0;
		}
	}

	const Scalar d = b*b - 4*a*c; //discriminant

	if(d <= 0.0) {
		if(d == 0.0) {
			// The double root of a*x^2 + b*x + c is -b/(2a), not -b/a --
			// the vertex of the parabola, which is where the two roots of
			// (-b +- sqrt(d))/(2a) coincide when d == 0.  `SolveQuadric`
			// above has always had the 2 here.  The wrong root is off by
			// exactly a factor of two, so it satisfied the polynomial only
			// when the root was 0 (b == 0), which is why the bilinear-patch
			// caller (`RayBilinearPatchIntersection`) turned it into a MISS
			// rather than a visibly misplaced hit for most patches.  An
			// earlier draft of this comment called that the only in-tree
			// consumer that can reach a genuine double root; it is not --
			// `GeometricUtilities::BilinearInverse` calls this function too
			// and hits the same branch, and there the `-b/a` bug silently
			// repaired itself almost as often as it broke: of 400000
			// dyadic on-surface inversions, 2311 land exactly on this
			// double-root branch, and 1007 of those previously returned
			// false because the doubled root fell outside the
			// [-1e-4, 1+1e-4] acceptance window `BilinearInverse` passes
			// as (min, max).
			sol[0] = -b/(2*a);
			if(sol[0] > min && sol[0] < max) {
				return 1;
			} else {
			return 0;
			}
		} else {
			return 0;
		}
	}

#ifdef _WIN32
	// Bloody MSVC crt
	const Scalar q = -0.5  * (b + _copysign(sqrt(d),b));
#else
	const Scalar q = -0.5  * (b + copysign(sqrt(d),b));
#endif

	sol[0] = c/q;
	sol[1] = q/a;

	if(
		(sol[0] > min && sol[0] < max ) &&
		(sol[1] > min && sol[1] < max)) {
		return 2;
	} else if(sol[0] > min && sol[0] < max) {
		return 1;
	} else if(sol[1] > min && sol[1] < max) {
		// Swap them around
		const Scalar temp = sol[0];
		sol[0] = sol[1];
		sol[1] = temp;
		return 1;
	}

	return 0;
}

int Polynomial::SolveCubic( const Scalar (&coeff)[ 4 ], Scalar (&sol)[ 3 ] )
{
	int		numSol = 0;

	// If the first coefficient is 0, then get the quadratic solver
	// to solve this root
	if( IsReallyZero( coeff[0] ) )
	{
		Scalar	coeffs[3] = { coeff[1], coeff[2], coeff[3] };
		Scalar	sols[2];
		numSol = SolveQuadric( coeffs, sols );

		for( int z=0; z<numSol; z++ ) {
			sol[ z ] = sols[ z ];
		}
		return numSol;
	}

	//
	// To solve a cubic, first get the normal form.  Then use Cardano's
	// Formula to get the determinant.  
	//

	// Normalization
	Scalar	OVa = 1.0 / coeff[0];
	Scalar	A = coeff[1] * OVa;
	Scalar	B = coeff[2] * OVa;
	Scalar	C = coeff[3] * OVa;

	// Substitute x = y - A / 3 to eliminate the quadratic term
	Scalar	sqA = A * A;
	Scalar	p = THIRD * (- THIRD * sqA + B);
    Scalar	q = HALF * (2.0/27 * A * sqA - THIRD * A * B + C);

	// Use Cardano's formula to get the determinant
	Scalar	cb_p = p*p*p;
	Scalar	D = q*q + cb_p;

	if( IsReallyZero( D ) )		// two real values
	{
		if( IsZero( q ) )	// one triple solution
		{
			sol[0] = 0;
			numSol = 1;
		}
		else				// one single and one Scalar solution
		{
			Scalar	u = cbrt( -q );
		    sol[0] = 2 * u;
			sol[1] = - u;
			numSol = 2;
		}
	}
	else if( D < 0 )		// casus irreducibilis, 3 different real values
	{
		Scalar phi = THIRD * acos( -q / sqrt (-cb_p ) );
		Scalar t = 2 * sqrt( -p );

		sol[0] =   t * cos(phi);
		sol[1] = - t * cos(phi + PI_OV_THREE);
		sol[2] = - t * cos(phi - PI_OV_THREE);
		numSol = 3;
	}
	else					// one real solution
	{
		Scalar sqrt_D = sqrt(D);
		Scalar u = cbrt(sqrt_D - q);
		Scalar v = - cbrt(sqrt_D + q);

		sol[0] = u + v;
		numSol = 1;
	}

	// Resubstitution
    Scalar	sub = THIRD * A;

    for( int i = 0; i < numSol; i++ ) {
		sol[i] -= sub;
	}

    return numSol;
}

//////////////////////////////////////////////////////////////////////
// OQS — Orellana & De Michele 2020 "ACM Algorithm 1010" quartic solver.
//
//   Replaces the classical Ferrari's-method implementation that
//   previously sat here.  Ferrari produces roots accurate only to
//   ~1e-3 when the resolvent cubic's substitution expression is
//   ill-conditioned (e.g. near-tangent rays on an analytic torus),
//   which translates to ~cm-scale off-surface intersection points —
//   catastrophic for Specular-Manifold-Sampling Newton iteration,
//   which needs the vertex to really be ON the surface so its
//   on-surface probe can re-snap.
//
//   OQS factors the quartic as two quadratics
//     (x^2 + alpha1 x + beta1)(x^2 + alpha2 x + beta2)
//   choosing one of three candidate LDL^T-style parameterisations
//   that minimises a forward-error metric, then runs a 4-variable
//   Newton-Raphson refinement on (alpha1, beta1, alpha2, beta2)
//   against the original quartic coefficients (see oqs_NRabcd).
//   Finally each quadratic factor is solved via Vieta's stable form
//   (use the larger-magnitude root, recover the smaller via c/root).
//
//   The result is double-precision accurate for well-conditioned
//   quartics and significantly more robust than Ferrari for
//   ill-conditioned cases.  Reference C code:
//     Orellana & De Michele, "Algorithm 1010: Boosting Efficiency
//     in Solving Quartic Equations with No Compromise in Accuracy",
//     ACM TOMS, Vol. 46, No. 2, 2020, DOI 10.1145/3386241.
//   Reference source: https://github.com/cridemichel/quartic_C.
//   See the upstream copyright/permission notice; no MIT license is implied.
//
//   API: This function keeps RISE's signature — coeff[0] is the
//   LEADING coefficient (x^4), coeff[4] is the constant term.
//   The OQS reference uses the opposite convention; we flip at
//   the boundary.
//////////////////////////////////////////////////////////////////////
namespace
{
	static const double oqs_pi = 3.14159265358979323846;
	static const double oqs_macheps = 2.2204460492503131e-16; // DBL_EPSILON
	static const double oqs_cubic_rescal_fact = 3.488062113727083e102; // pow(DBL_MAX, 1/3) / phi

	// Binary64 significand with a separate exponent for exceptionally wide
	// root scales. This preserves nonzero coefficients and Vieta products
	// that a single double variable transformation cannot represent. Finite
	// nonzero values have .5 <= |m| < 1; zero has e=0. The 64-bit exponent
	// leaves ample range for the finite binary64 inputs and bounded OQS
	// iterations. Only final root conversion returns to binary64 range.
	// This extends range, not significand precision or root multiplicity
	// guarantees. Rounding a tiny addend away does not erase its independent
	// coefficient storage, unlike underflowing normalization into a zero.
	struct oqs_wide
	{
		double m;
		std::int64_t e;
		oqs_wide( double x=0 ) : m(0), e(0) {
			int exponent;
			m=std::frexp( x, &exponent );
			e=exponent;
		}
		static oqs_wide scaled( double x, std::int64_t exponent ) {
			oqs_wide r(x);
			if( r.m != 0 ) r.e += exponent;
			return r;
		}
		double unscale( int exponent=0 ) const {
			const std::int64_t total=e+exponent;
			if(m==0) return m;
			if(total>DBL_MAX_EXP) return std::copysign(HUGE_VAL,m);
			if(total<DBL_MIN_EXP-DBL_MANT_DIG) return std::copysign(0.0,m);
			return std::scalbn(m,static_cast<int>(total));
		}
		oqs_wide operator-() const { return scaled(-m,e); }
		oqs_wide& operator+=(oqs_wide x);
		oqs_wide& operator*=(oqs_wide x);
	};
	oqs_wide operator+(oqs_wide a,oqs_wide b) {
		if( a.m == 0 ) return b;
		if( b.m == 0 ) return a;
		if( a.e < b.e ) { const oqs_wide t=a; a=b; b=t; }
		const std::int64_t gap=b.e-a.e;
		if(gap<DBL_MIN_EXP-DBL_MANT_DIG) return a;
		return oqs_wide::scaled(a.m+std::scalbn(b.m,static_cast<int>(gap)),a.e);
	}
	oqs_wide operator-(oqs_wide a,oqs_wide b){return a+(-b);}
	oqs_wide operator*(oqs_wide a,oqs_wide b){return oqs_wide::scaled(a.m*b.m,a.e+b.e);}
	oqs_wide operator/(oqs_wide a,oqs_wide b){return oqs_wide::scaled(a.m/b.m,a.e-b.e);}
	oqs_wide& oqs_wide::operator+=(oqs_wide x){return *this=*this+x;}
	oqs_wide& oqs_wide::operator*=(oqs_wide x){return *this=*this*x;}
	bool operator==(oqs_wide a,oqs_wide b){return a.m==b.m && (a.m==0 || a.e==b.e);}
	bool operator!=(oqs_wide a,oqs_wide b){return !(a==b);}
	bool operator<(oqs_wide a,oqs_wide b){
		if(a.m<=0 && b.m>=0)return a.m<b.m;
		if(a.m>=0 && b.m<=0)return false;
		if(a.e==b.e)return a.m<b.m;
		return a.m>0 ? a.e<b.e : a.e>b.e;
	}
	bool operator>(oqs_wide a,oqs_wide b){return b<a;}
	bool operator<=(oqs_wide a,oqs_wide b){return !(a>b);}
	bool operator>=(oqs_wide a,oqs_wide b){return !(a<b);}
	double oqs_round(double x){volatile double r=x;return r;}
	oqs_wide oqs_round(oqs_wide x){x.m=oqs_round(x.m);return x;}
	double oqs_fabs(double x){return std::fabs(x);}
	oqs_wide oqs_fabs(oqs_wide x){return oqs_wide::scaled(std::fabs(x.m),x.e);}
	double oqs_sqrt(double x){return std::sqrt(x);}
	oqs_wide oqs_sqrt( oqs_wide x ) {
		const std::int64_t q=x.e/2;
		const int r=static_cast<int>(x.e-2*q);
		return oqs_wide::scaled( std::sqrt(std::scalbn(x.m,r)), q );
	}
	double oqs_cbrt(double x){return cbrt(x);}
	oqs_wide oqs_cbrt( oqs_wide x ) {
		const std::int64_t q=x.e/3;
		const int r=static_cast<int>(x.e-3*q);
		return oqs_wide::scaled( oqs_cbrt(std::scalbn(x.m,r)), q );
	}
	double oqs_acos(double x){return std::acos(x);}
	oqs_wide oqs_acos(oqs_wide x){return oqs_wide(std::acos(x.unscale()));}
	double oqs_cos(double x){return std::cos(x);}
	oqs_wide oqs_cos(oqs_wide x){return oqs_wide(std::cos(x.unscale()));}
	double oqs_copysign(double a,double b){return std::copysign(a,b);}
	oqs_wide oqs_copysign(double a,oqs_wide b){return oqs_wide(std::copysign(a,b.m));}
	bool oqs_finite(double x){return RISE::IsFiniteDouble(x);}
	bool oqs_finite(oqs_wide x){return RISE::IsFiniteDouble(x.m);}
	double oqs_fma(double a,double b,double c){return std::fma(a,b,c);}
	oqs_wide oqs_fma(oqs_wide a,oqs_wide b,oqs_wide c){
		if(a.m==0 || b.m==0) return c;
		const std::int64_t exponent=a.e+b.e;
		const std::int64_t gap=c.e-exponent;
		// Product significands lie in [1/4,1). Align c to that binade and
		// perform one hardware FMA, retaining its single rounding. Extremely
		// remote terms cannot affect rounding; never form an overflowing shift.
		if(c.m!=0 && gap>DBL_MAX_EXP-2) return c;
		const double aligned=(c.m==0 || gap<DBL_MIN_EXP-DBL_MANT_DIG)
			? 0.0 : std::scalbn(c.m,static_cast<int>(gap));
		return oqs_wide::scaled(std::fma(a.m,b.m,aligned),exponent);
	}

	// Dominant real root of depressed cubic x^3 + b x + c = 0, handles
	// b or c near DBL_MAX without overflow.  Reference: eq. 85/86.
	template<class T>
	void oqs_solve_cubic_analytic_depressed_handle_inf( T b, T c, T* sol )
	{
		const T PI2 = oqs_pi / 2.0, TWOPI = 2.0 * oqs_pi;
		T Q = -b / 3.0;
		T R = 0.5 * c;
		if( R == 0 ) {
			*sol = ( b <= 0 ) ? oqs_sqrt( -b ) : 0.0;
			return;
		}
		T KK;
		if( oqs_fabs( Q ) < oqs_fabs( R ) ) {
			T QR = Q / R;
			T QRSQ = QR * QR;
			KK = 1.0 - Q * QRSQ;
		} else {
			T RQ = R / Q;
			KK = oqs_copysign( 1.0, Q ) * ( RQ * RQ / Q - 1.0 );
		}
		if( KK < 0.0 ) {
			T sqrtQ = oqs_sqrt( Q );
			T theta = oqs_acos( ( R / oqs_fabs( Q ) ) / sqrtQ );
			*sol = ( theta < PI2 )
				? -2.0 * sqrtQ * oqs_cos( theta / 3.0 )
				: -2.0 * sqrtQ * oqs_cos( ( theta + TWOPI ) / 3.0 );
		} else {
			T A;
			if( oqs_fabs( Q ) < oqs_fabs( R ) ) {
				A = -oqs_copysign( 1.0, R ) * oqs_cbrt( oqs_fabs( R ) * ( 1.0 + oqs_sqrt( KK ) ) );
			} else {
				A = -oqs_copysign( 1.0, R ) * oqs_cbrt( oqs_fabs( R ) + oqs_sqrt( oqs_fabs( Q ) ) * oqs_fabs( Q ) * oqs_sqrt( KK ) );
			}
			T B = ( A == 0.0 ) ? 0.0 : Q / A;
			*sol = A + B;
		}
	}

	// Dominant real root of depressed cubic x^3 + b x + c = 0.
	template<class T>
	void oqs_solve_cubic_analytic_depressed( T b, T c, T* sol )
	{
		T Q = -b / 3.0;
		T R = 0.5 * c;
		if( oqs_fabs( Q ) > 1e102 || oqs_fabs( R ) > 1e154 ) {
			oqs_solve_cubic_analytic_depressed_handle_inf( b, c, sol );
			return;
		}
		T Q3 = Q * Q * Q;
		T R2 = R * R;
		if( R2 < Q3 ) {
			T theta = oqs_acos( R / oqs_sqrt( Q3 ) );
			T sqrtQ = -2.0 * oqs_sqrt( Q );
			if( theta < oqs_pi / 2.0 ) {
				*sol = sqrtQ * oqs_cos( theta / 3.0 );
			} else {
				*sol = sqrtQ * oqs_cos( ( theta + 2.0 * oqs_pi ) / 3.0 );
			}
		} else {
			T A = -oqs_copysign( 1.0, R ) * oqs_cbrt( oqs_fabs( R ) + oqs_sqrt( R2 - Q3 ) );
			T B = ( A == 0.0 ) ? 0.0 : Q / A;
			*sol = A + B;
		}
	}

	// phi0 = dominant root of the depressed-shifted cubic derived
	// from the quartic (eq. 79 in the paper).  Includes an optional
	// internal-rescale branch when the cubic itself over/underflows.
	template<class T>
	void oqs_calc_phi0( T a, T b, T c, T d, T* phi0, int scaled )
	{
		T diskr = 9.0 * a * a - 24.0 * b;
		T s;
		if( diskr > 0.0 ) {
			diskr = oqs_sqrt( diskr );
			s = ( a > 0.0 ) ? ( -2.0 * b / ( 3.0 * a + diskr ) )
				            : ( -2.0 * b / ( 3.0 * a - diskr ) );
		} else {
			s = -a / 4.0;
		}
		T aq = a + 4.0 * s;
		T bq = b + 3.0 * s * ( a + 2.0 * s );
		T cq = c + s * ( 2.0 * b + s * ( 3.0 * a + 4.0 * s ) );
		T dq = d + s * ( c + s * ( b + s * ( a + s ) ) );
		T gg = bq * bq / 9.0;
		T hh = aq * cq;
		T g = hh - 4.0 * dq - 3.0 * gg;
		T h = ( 8.0 * dq + hh - 2.0 * gg ) * bq / 3.0 - cq * cq - dq * aq * aq;
		T rmax;
		oqs_solve_cubic_analytic_depressed( g, h, &rmax );
		if( !oqs_finite( rmax ) ) {
			oqs_solve_cubic_analytic_depressed_handle_inf( g, h, &rmax );
			if( !oqs_finite( rmax ) && scaled ) {
				T rfact = oqs_cubic_rescal_fact;
				T rfactsq = rfact * rfact;
				T ggss = gg / rfactsq;
				T hhss = hh / rfactsq;
				T dqss = dq / rfactsq;
				T aqs = aq / rfact;
				T bqs = bq / rfact;
				T cqs = cq / rfact;
				ggss = bqs * bqs / 9.0;
				hhss = aqs * cqs;
				T g2 = hhss - 4.0 * dqss - 3.0 * ggss;
				T h2 = ( 8.0 * dqss + hhss - 2.0 * ggss ) * bqs / 3.0 - cqs * ( cqs / rfact ) - ( dq / rfact ) * aqs * aqs;
				oqs_solve_cubic_analytic_depressed( g2, h2, &rmax );
				if( !oqs_finite( rmax ) ) {
					oqs_solve_cubic_analytic_depressed_handle_inf( g2, h2, &rmax );
				}
				rmax *= rfact;
			}
		}
		// Newton-Raphson polish of phi0 on the depressed cubic x^3 + g x + h.
		T x = rmax;
		T xsq = x * x;
		T xxx = x * xsq;
		T gx = g * x;
		T f = x * ( xsq + g ) + h;
		T maxtt = oqs_fabs( xxx ) > oqs_fabs( gx ) ? oqs_fabs( xxx ) : oqs_fabs( gx );
		if( oqs_fabs( h ) > maxtt ) maxtt = oqs_fabs( h );
		if( oqs_fabs( f ) > oqs_macheps * maxtt ) {
			for( int iter = 0; iter < 8; iter++ ) {
				T df = 3.0 * xsq + g;
				if( df == 0 ) break;
				T xold = x;
				x += -f / df;
				T fold = f;
				xsq = x * x;
				f = x * ( xsq + g ) + h;
				if( f == 0 ) break;
				if( oqs_fabs( f ) >= oqs_fabs( fold ) ) { x = xold; break; }
			}
		}
		*phi0 = x;
	}

	// Relative-error metrics (eq. 29, 48-51, 68-69 in the manuscript).
	template<class T>
	T oqs_calc_err_ldlt( T b, T c, T d, T d2, T l1, T l2, T l3 )
	{
		T s = ( b == 0 ) ? oqs_fabs( d2 + l1 * l1 + 2.0 * l3 ) : oqs_fabs( ( ( d2 + l1 * l1 + 2.0 * l3 ) - b ) / b );
		s += ( c == 0 ) ? oqs_fabs( 2.0 * d2 * l2 + 2.0 * l1 * l3 ) : oqs_fabs( ( ( 2.0 * d2 * l2 + 2.0 * l1 * l3 ) - c ) / c );
		s += ( d == 0 ) ? oqs_fabs( d2 * l2 * l2 + l3 * l3 ) : oqs_fabs( ( ( d2 * l2 * l2 + l3 * l3 ) - d ) / d );
		return s;
	}
	template<class T>
	T oqs_calc_err_abcd( T a, T b, T c, T d, T aq, T bq, T cq, T dq )
	{
		T s = ( d == 0 ) ? oqs_fabs( bq * dq ) : oqs_fabs( ( bq * dq - d ) / d );
		s += ( c == 0 ) ? oqs_fabs( bq * cq + aq * dq ) : oqs_fabs( ( ( bq * cq + aq * dq ) - c ) / c );
		s += ( b == 0 ) ? oqs_fabs( bq + aq * cq + dq ) : oqs_fabs( ( ( bq + aq * cq + dq ) - b ) / b );
		s += ( a == 0 ) ? oqs_fabs( aq + cq ) : oqs_fabs( ( ( aq + cq ) - a ) / a );
		return s;
	}
	template<class T>
	T oqs_calc_err_abc( T a, T b, T c, T aq, T bq, T cq, T dq )
	{
		T s = ( c == 0 ) ? oqs_fabs( bq * cq + aq * dq ) : oqs_fabs( ( ( bq * cq + aq * dq ) - c ) / c );
		s += ( b == 0 ) ? oqs_fabs( bq + aq * cq + dq ) : oqs_fabs( ( ( bq + aq * cq + dq ) - b ) / b );
		s += ( a == 0 ) ? oqs_fabs( aq + cq ) : oqs_fabs( ( ( aq + cq ) - a ) / a );
		return s;
	}
	template<class T>
	T oqs_calc_err_d( T errmin, T d, T bq, T dq )
	{
		return ( ( d == 0 ) ? oqs_fabs( bq * dq ) : oqs_fabs( ( bq * dq - d ) / d ) ) + errmin;
	}

	// Error-free summation must retain the individual roundings even under
	// the renderer's fast-math flags. Volatile materializes those operations.
// Clang and MSVC preserve the five roundings without stack barriers in
// this precise scope. Reassociation would algebraically erase the low
// error term. Other compilers retain the explicit round-trip equivalent.
#if defined(__clang__) || defined(_MSC_VER)
#pragma float_control(precise, on, push)
#endif
	template<class T>
	T oqs_sum_error( T a, T b, T& error )
	{
#if defined(__clang__) || defined(_MSC_VER)
		const T sum = a + b;
		const T bv = sum - a;
		const T av = sum - bv;
		const T br = b - bv;
		const T ar = a - av;
#else
		const T sum = oqs_round( a + b );
		const T bv = oqs_round( sum - a );
		const T av = oqs_round( sum - bv );
		const T br = oqs_round( b - bv );
		const T ar = oqs_round( a - av );
#endif
		error = ar + br;
		return sum;
	}
#if defined(__clang__) || defined(_MSC_VER)
#pragma float_control(pop)
#endif

	template<class T>
	T oqs_sum4( T a, T b, T c, T d )
	{
		T e1, e2, e3;
		const T s1 = oqs_sum_error( a, b, e1 );
		const T s2 = oqs_sum_error( s1, c, e2 );
		const T s3 = oqs_sum_error( s2, d, e3 );
		return s3 + ( e1 + e2 + e3 );
	}
	template<class T>
	void oqs_factor_residual( const T x[4], T a, T b, T c, T d, T f[4] )
	{
		f[0] = oqs_fma( x[1], x[3], -d );
		const T p = oqs_round( x[1] * x[2] );
		const T q = oqs_round( x[0] * x[3] );
		f[1] = oqs_sum4( p, q, -c, oqs_fma( x[1], x[2], -p ) + oqs_fma( x[0], x[3], -q ) );
		const T r = oqs_round( x[0] * x[2] );
		f[2] = oqs_sum4( r, x[1], x[3], -b ) + oqs_fma( x[0], x[2], -r );
		f[3] = oqs_sum4( x[0], x[2], -a, T(0.0) );
	}

	// Newton-Raphson refine (alpha1, beta1, alpha2, beta2) against the
	// original quartic coefficients.  Converges in typically 2-4
	// iterations and drives total forward error below macheps.
	template<class T>
	void oqs_NRabcd( T a, T b, T c, T d,
		T* AQ, T* BQ, T* CQ, T* DQ )
	{
		const int NITERMAX = 20;
		T x[4] = { *AQ, *BQ, *CQ, *DQ };
		T vr[4] = { d, c, b, a };
		T fvec[4];
		oqs_factor_residual( x, a, b, c, d, fvec );
		T errf = 0, errfa = 0, errfmin;
		T xmin[4] = { x[0], x[1], x[2], x[3] };
		for( int k1 = 0; k1 < 4; k1++ ) {
			T fveca = oqs_fabs( fvec[k1] );
			errf += ( vr[k1] == 0 ) ? fveca : oqs_fabs( fveca / vr[k1] );
			errfa += fveca;
		}
		errfmin = errfa;
		if( errfa == 0 ) return;

		for( int iter = 0; iter < NITERMAX; iter++ ) {
			T x02 = x[0] - x[2];
			T det = x[1] * x[1] + x[1] * ( -x[2] * x02 - 2.0 * x[3] ) + x[3] * ( x[0] * x02 + x[3] );
			if( det == 0.0 ) break;
			T Jinv[4][4];
			Jinv[0][0] = x02;
			Jinv[0][1] = x[3] - x[1];
			Jinv[0][2] = x[1] * x[2] - x[0] * x[3];
			Jinv[0][3] = -x[1] * Jinv[0][1] - x[0] * Jinv[0][2];
			Jinv[1][0] = x[0] * Jinv[0][0] + Jinv[0][1];
			Jinv[1][1] = -x[1] * Jinv[0][0];
			Jinv[1][2] = -x[1] * Jinv[0][1];
			Jinv[1][3] = -x[1] * Jinv[0][2];
			Jinv[2][0] = -Jinv[0][0];
			Jinv[2][1] = -Jinv[0][1];
			Jinv[2][2] = -Jinv[0][2];
			Jinv[2][3] = Jinv[0][2] * x[2] + Jinv[0][1] * x[3];
			Jinv[3][0] = -x[2] * Jinv[0][0] - Jinv[0][1];
			Jinv[3][1] = Jinv[0][0] * x[3];
			Jinv[3][2] = x[3] * Jinv[0][1];
			Jinv[3][3] = x[3] * Jinv[0][2];
			T dx[4] = { 0, 0, 0, 0 };
			for( int k1 = 0; k1 < 4; k1++ )
				for( int k2 = 0; k2 < 4; k2++ )
					dx[k1] += Jinv[k1][k2] * fvec[k2];
			for( int k1 = 0; k1 < 4; k1++ )
				x[k1] += -dx[k1] / det;
			oqs_factor_residual( x, a, b, c, d, fvec );
			T errfold = errf;
			errf = 0; errfa = 0;
			for( int k1 = 0; k1 < 4; k1++ ) {
				T fveca = oqs_fabs( fvec[k1] );
				errf += ( vr[k1] == 0 ) ? fveca : oqs_fabs( fveca / vr[k1] );
				errfa += fveca;
			}
			if( errfa < errfmin ) { errfmin = errfa; for( int k1 = 0; k1 < 4; k1++ ) xmin[k1] = x[k1]; }
			if( errfa == 0 ) break;
			if( errf >= errfold ) break;
		}
		*AQ = xmin[0]; *BQ = xmin[1]; *CQ = xmin[2]; *DQ = xmin[3];
	}

	// Vieta-stable quadratic: returns number of real roots (0 or 2).
	// For x^2 + a x + b = 0, compute the larger-magnitude root via the
	// stable formula and the smaller via b / larger.
	template<class T>
	int oqs_solve_quadratic_real( T a, T b, T out[2] )
	{
		T diskr = a * a - 4.0 * b;
		if( diskr < 0.0 ) return 0;
		T sq = oqs_sqrt( diskr );
		T div = ( a >= 0.0 ) ? ( -a - sq ) : ( -a + sq );
		T zmax = div / 2.0;
		T zmin = ( zmax == 0.0 ) ? 0.0 : ( b / zmax );
		out[0] = zmax;
		out[1] = zmin;
		return 2;
	}
	template<class T>
	int oqs_factor_quartic( T A, T B, T C, T D, T sol[4] )
	{
		T phi0=0;
		oqs_calc_phi0( A, B, C, D, &phi0, 0 );
		// Build the LDL^T decomposition (eqs. 16-28).
		const T l1 = A / 2.0;
		const T l3 = B / 6.0 + phi0 / 2.0;
		const T del2 = C - A * l3;
		const T bl311 = 2.0 * B / 3.0 - phi0 - l1 * l1;
		const T dml3l3 = D - l3 * l3;

		T l2m[4], d2m[4], res[4];
		int nsol = 0;
		if( bl311 != 0.0 ) {
			d2m[nsol] = bl311;
			l2m[nsol] = del2 / ( 2.0 * d2m[nsol] );
			res[nsol] = oqs_calc_err_ldlt( B, C, D, d2m[nsol], l1, l2m[nsol], l3 );
			nsol++;
		}
		if( del2 != 0 ) {
			l2m[nsol] = 2.0 * dml3l3 / del2;
			if( l2m[nsol] != 0 ) {
				d2m[nsol] = del2 / ( 2.0 * l2m[nsol] );
				res[nsol] = oqs_calc_err_ldlt( B, C, D, d2m[nsol], l1, l2m[nsol], l3 );
				nsol++;
			}
			d2m[nsol] = bl311;
			l2m[nsol] = 2.0 * dml3l3 / del2;
			res[nsol] = oqs_calc_err_ldlt( B, C, D, d2m[nsol], l1, l2m[nsol], l3 );
			nsol++;
		}
		T d2 = 0, l2 = 0;
		if( nsol > 0 ) {
			int kmin = 0;
			T resmin = res[0];
			for( int k = 1; k < nsol; k++ ) {
				if( res[k] < resmin ) { resmin = res[k]; kmin = k; }
			}
			d2 = d2m[kmin];
			l2 = l2m[kmin];
		}

		// Build candidate (alpha1, beta1, alpha2, beta2) factorisation.
		// Real coefficients arise for d2 < 0. The d2 > 0 candidate has
		// conjugate-complex coefficients, but its reconstruction error is still
		// essential when comparing the alternative real factorisation below.
		int realcase0 = ( d2 < 0.0 ) ? 1 : ( d2 > 0.0 ? 0 : -1 );
		T aq = 0, bq = 0, cq = 0, dq = 0;
		T errmin = 0;

		if( realcase0 == 1 ) {
			T gamma = oqs_sqrt( -d2 );
			aq = l1 + gamma;
			bq = l3 + gamma * l2;
			cq = l1 - gamma;
			dq = l3 - gamma * l2;
			if( oqs_fabs( dq ) < oqs_fabs( bq ) ) dq = D / bq;
			else if( oqs_fabs( dq ) > oqs_fabs( bq ) ) bq = D / dq;

			T aqv[3], cqv[3], errv[3];
			int kmin = 0;
			if( oqs_fabs( aq ) < oqs_fabs( cq ) ) {
				int n = 0;
				if( dq != 0 ) { aqv[n] = ( C - bq * cq ) / dq; errv[n] = oqs_calc_err_abc( A, B, C, aqv[n], bq, cq, dq ); n++; }
				if( cq != 0 ) { aqv[n] = ( B - dq - bq ) / cq; errv[n] = oqs_calc_err_abc( A, B, C, aqv[n], bq, cq, dq ); n++; }
				aqv[n] = A - cq; errv[n] = oqs_calc_err_abc( A, B, C, aqv[n], bq, cq, dq ); n++;
				errmin = errv[0];
				for( int k = 1; k < n; k++ ) if( errv[k] < errmin ) { errmin = errv[k]; kmin = k; }
				aq = aqv[kmin];
			} else {
				int n = 0;
				if( bq != 0 ) { cqv[n] = ( C - aq * dq ) / bq; errv[n] = oqs_calc_err_abc( A, B, C, aq, bq, cqv[n], dq ); n++; }
				if( aq != 0 ) { cqv[n] = ( B - bq - dq ) / aq; errv[n] = oqs_calc_err_abc( A, B, C, aq, bq, cqv[n], dq ); n++; }
				cqv[n] = A - aq; errv[n] = oqs_calc_err_abc( A, B, C, aq, bq, cqv[n], dq ); n++;
				errmin = errv[0];
				for( int k = 1; k < n; k++ ) if( errv[k] < errmin ) { errmin = errv[k]; kmin = k; }
				cq = cqv[kmin];
			}
		}

		// Identical-alpha / split-beta alternative (OQS case III).
		// Evaluate it even away from d2=0 to retain the alternate-factor recovery
		// used by ill-conditioned torus rays, but compare against the ACTUAL
		// primary factorisation, including the conjugate-complex case.
		//
		// For d2>0 the primary product is
		//   (x^2+l1*x+l3)^2 + d2*(x+l2)^2.
		// oqs_calc_err_ldlt reconstructs its B/C/D coefficients directly without
		// complex arithmetic; A=2*l1 already matches. Treating this error as
		// infinity would allow an incompatible real alternative to win. For
		// (x^2+1)^2 it replaced +2*x^2 by -2*x^2, then the singular repeated-
		// factor Newton system could not repair it (DL226).
		int whichcase = 0;
		{
			T d3 = D - l3 * l3;
			if( d3 <= 0 ) {
				const T err0 = ( realcase0 == 1 )
					? oqs_calc_err_d( errmin, D, bq, dq )
					: oqs_calc_err_ldlt( B, C, D, d2, l1, l2, l3 );
				T sqrtd3 = oqs_sqrt( -d3 );
				T aq1 = l1, bq1 = l3 + sqrtd3, cq1 = l1, dq1 = l3 - sqrtd3;
				if( oqs_fabs( dq1 ) < oqs_fabs( bq1 ) ) dq1 = D / bq1;
				else if( oqs_fabs( dq1 ) > oqs_fabs( bq1 ) ) bq1 = D / dq1;
				T err1 = oqs_calc_err_abcd( A, B, C, D, aq1, bq1, cq1, dq1 );
				// Equal rounded coefficient errors do not imply equally conditioned
				// roots. When d3==0 this alternative is one quadratic squared:
				// prefer its exact coalescence over a numerically split primary.
				// Example: (x-1)^4 can give d2=-epsilon, alpha=-2+/-oqs_sqrt(epsilon)
				// with both errors rounding to zero. Newton's singular system need
				// not repair that split. This tie rule introduces no error band;
				// an incompatible positive-quartic fallback still has larger error.
				const bool coalescedTie = ( d3 == 0.0 && err1 == err0 );
				if( realcase0 == -1 || err1 < err0 || coalescedTie ) {
					whichcase = 1;
					aq = aq1; bq = bq1; cq = cq1; dq = dq1;
					realcase0 = 1;  // swapped to the identical-alpha real case
				}
			}
			// d3 > 0: identical-alpha factorisation has complex β, no real
			// roots from this branch.  Leave realcase0 as-is.
		}

		// Extract real roots from the selected real-coefficient factors; their
		// quadratic discriminants still decide whether each pair is real.
		int num = 0;
		if( realcase0 == 1 ) {
			// Refine (alpha1, beta1, alpha2, beta2) via Newton-Raphson.
			oqs_NRabcd( A, B, C, D, &aq, &bq, &cq, &dq );

			T qr[2];
			if( oqs_solve_quadratic_real( aq, bq, qr ) == 2 ) {
				sol[num++] = qr[0];
				sol[num++] = qr[1];
			}
			if( oqs_solve_quadratic_real( cq, dq, qr ) == 2 ) {
				sol[num++] = qr[0];
				sol[num++] = qr[1];
			}
		}
		(void)whichcase;

		return num;
	}

}

int Polynomial::SolveQuartic( const Scalar (&coeff)[ 5 ], Scalar (&sol)[ 4 ] )
{
	// Degenerate leading coefficient — fall through to cubic.
	if( IsReallyZero( coeff[0] ) )
	{
		Scalar coeffs[4] = { coeff[1], coeff[2], coeff[3], coeff[4] };
		Scalar sols[3];
		int n = SolveCubic( coeffs, sols );
		for( int i = 0; i < n; i++ ) sol[i] = sols[i];
		return n;
	}

	// Normalize x=2^scaleExponent*y before division by the leading
	// coefficient. Exponent decomposition avoids overflowing monic ratios,
	// and moves both tiny and large root scales into the resolvent's range.
	int leadingExponent;
	const double leadingMantissa = std::frexp( coeff[0], &leadingExponent );
	int scaleExponent = 0;
	bool hasScale = false;
	int exponents[4] = {};
	double mantissas[4] = {};
	for( int i = 1; i <= 4; ++i ) {
		if( coeff[i] == 0 ) continue;
		mantissas[i-1] = std::frexp( coeff[i], &exponents[i-1] );
		const int difference = exponents[i-1] - leadingExponent;
		const int bound = difference >= 0 ? ( difference + i - 1 ) / i : difference / i;
		if( !hasScale || bound > scaleExponent ) scaleExponent = bound;
		hasScale = true;
	}
	// The resolvent's discriminant reaches degree twelve in the variable.
	// Use separate exponents before that product range can become subnormal,
	// not merely after a normalized coefficient has already become zero.
	bool extended = false;
	for( int i = 1; i <= 4; ++i ) {
		const int exponent = exponents[i-1] - leadingExponent - i * scaleExponent;
		if( mantissas[i-1] != 0 && exponent < DBL_MIN_EXP / 12 ) extended = true;
	}
	int num;
	if( extended ) {
		oqs_wide normalized[4], roots[4];
		for( int i = 1; i <= 4; ++i )
			normalized[i-1] = oqs_wide::scaled( mantissas[i-1] / leadingMantissa,
				exponents[i-1] - leadingExponent - i * scaleExponent );
		num = oqs_factor_quartic( normalized[0], normalized[1], normalized[2], normalized[3], roots );
		for( int i = 0; i < num; ++i ) sol[i] = roots[i].unscale( scaleExponent );
	} else {
		double normalized[4], roots[4];
		for( int i = 1; i <= 4; ++i )
			normalized[i-1] = std::scalbn( mantissas[i-1] / leadingMantissa,
				exponents[i-1] - leadingExponent - i * scaleExponent );
		num = oqs_factor_quartic( normalized[0], normalized[1], normalized[2], normalized[3], roots );
		for( int i = 0; i < num; ++i ) sol[i] = std::scalbn( roots[i], scaleExponent );
	}
	return num;
}

Scalar Polynomial::bessi0( Scalar x )
{
	const Scalar ax = fabs(x);
	if (ax < 3.75)
	{
		Scalar y=x/3.75;
		y*=y;
		return (1.0+y*(3.5156229+y*(3.0899424+y*(1.2067492
			+y*(0.2659732+y*(0.360768e-1+y*0.45813e-2))))));
	}
	else
	{
		Scalar y=3.75/ax;
		return (exp(ax)/sqrt(ax))*(0.39894228+y*(0.1328592e-1
			+y*(0.225319e-2+y*(-0.157565e-2+y*(0.916281e-2
			+y*(-0.2057706e-1+y*(0.2635537e-1+y*(-0.1647633e-1
			+y*0.392377e-2))))))));
	}
}
