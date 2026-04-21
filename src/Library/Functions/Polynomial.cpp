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
#include <float.h>			// for copysign

using namespace RISE;

static const Scalar	EQN_EPS =    1e-19;
static const Scalar EQN_CE =	 1e-15;

#define		IsCloseEnoughToZero(x)		((x) > -EQN_CE && (x) < EQN_CE)
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
		const Scalar p = 0.5 * a;
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
			sol[0] = -b/a;
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
//   Source: https://github.com/cridemichel/quartic_C (MIT-like).
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
	static const double oqs_fact_d0 = 1.4901161193847656e-8;  // sqrt(macheps)
	static const double oqs_cubic_rescal_fact = 3.488062113727083e102; // pow(DBL_MAX, 1/3) / phi
	static const double oqs_quart_rescal_fact = 7.156344627944542e76;  // pow(DBL_MAX, 1/4) / phi

	inline double oqs_sqr( double x ) { return x * x; }

	// Dominant real root of depressed cubic x^3 + b x + c = 0, handles
	// b or c near DBL_MAX without overflow.  Reference: eq. 85/86.
	void oqs_solve_cubic_analytic_depressed_handle_inf( double b, double c, double* sol )
	{
		const double PI2 = oqs_pi / 2.0, TWOPI = 2.0 * oqs_pi;
		double Q = -b / 3.0;
		double R = 0.5 * c;
		if( R == 0 ) {
			*sol = ( b <= 0 ) ? sqrt( -b ) : 0.0;
			return;
		}
		double KK;
		if( fabs( Q ) < fabs( R ) ) {
			double QR = Q / R;
			double QRSQ = QR * QR;
			KK = 1.0 - Q * QRSQ;
		} else {
			double RQ = R / Q;
			KK = copysign( 1.0, Q ) * ( RQ * RQ / Q - 1.0 );
		}
		if( KK < 0.0 ) {
			double sqrtQ = sqrt( Q );
			double theta = acos( ( R / fabs( Q ) ) / sqrtQ );
			*sol = ( theta < PI2 )
				? -2.0 * sqrtQ * cos( theta / 3.0 )
				: -2.0 * sqrtQ * cos( ( theta + TWOPI ) / 3.0 );
		} else {
			double A;
			if( fabs( Q ) < fabs( R ) ) {
				A = -copysign( 1.0, R ) * cbrt( fabs( R ) * ( 1.0 + sqrt( KK ) ) );
			} else {
				A = -copysign( 1.0, R ) * cbrt( fabs( R ) + sqrt( fabs( Q ) ) * fabs( Q ) * sqrt( KK ) );
			}
			double B = ( A == 0.0 ) ? 0.0 : Q / A;
			*sol = A + B;
		}
	}

	// Dominant real root of depressed cubic x^3 + b x + c = 0.
	void oqs_solve_cubic_analytic_depressed( double b, double c, double* sol )
	{
		double Q = -b / 3.0;
		double R = 0.5 * c;
		if( fabs( Q ) > 1e102 || fabs( R ) > 1e154 ) {
			oqs_solve_cubic_analytic_depressed_handle_inf( b, c, sol );
			return;
		}
		double Q3 = Q * Q * Q;
		double R2 = R * R;
		if( R2 < Q3 ) {
			double theta = acos( R / sqrt( Q3 ) );
			double sqrtQ = -2.0 * sqrt( Q );
			if( theta < oqs_pi / 2.0 ) {
				*sol = sqrtQ * cos( theta / 3.0 );
			} else {
				*sol = sqrtQ * cos( ( theta + 2.0 * oqs_pi ) / 3.0 );
			}
		} else {
			double A = -copysign( 1.0, R ) * cbrt( fabs( R ) + sqrt( R2 - Q3 ) );
			double B = ( A == 0.0 ) ? 0.0 : Q / A;
			*sol = A + B;
		}
	}

	// phi0 = dominant root of the depressed-shifted cubic derived
	// from the quartic (eq. 79 in the paper).  Includes an optional
	// internal-rescale branch when the cubic itself over/underflows.
	void oqs_calc_phi0( double a, double b, double c, double d, double* phi0, int scaled )
	{
		double diskr = 9.0 * a * a - 24.0 * b;
		double s;
		if( diskr > 0.0 ) {
			diskr = sqrt( diskr );
			s = ( a > 0.0 ) ? ( -2.0 * b / ( 3.0 * a + diskr ) )
				            : ( -2.0 * b / ( 3.0 * a - diskr ) );
		} else {
			s = -a / 4.0;
		}
		double aq = a + 4.0 * s;
		double bq = b + 3.0 * s * ( a + 2.0 * s );
		double cq = c + s * ( 2.0 * b + s * ( 3.0 * a + 4.0 * s ) );
		double dq = d + s * ( c + s * ( b + s * ( a + s ) ) );
		double gg = bq * bq / 9.0;
		double hh = aq * cq;
		double g = hh - 4.0 * dq - 3.0 * gg;
		double h = ( 8.0 * dq + hh - 2.0 * gg ) * bq / 3.0 - cq * cq - dq * aq * aq;
		double rmax;
		oqs_solve_cubic_analytic_depressed( g, h, &rmax );
		if( std::isnan( rmax ) || std::isinf( rmax ) ) {
			oqs_solve_cubic_analytic_depressed_handle_inf( g, h, &rmax );
			if( ( std::isnan( rmax ) || std::isinf( rmax ) ) && scaled ) {
				double rfact = oqs_cubic_rescal_fact;
				double rfactsq = rfact * rfact;
				double ggss = gg / rfactsq;
				double hhss = hh / rfactsq;
				double dqss = dq / rfactsq;
				double aqs = aq / rfact;
				double bqs = bq / rfact;
				double cqs = cq / rfact;
				ggss = bqs * bqs / 9.0;
				hhss = aqs * cqs;
				double g2 = hhss - 4.0 * dqss - 3.0 * ggss;
				double h2 = ( 8.0 * dqss + hhss - 2.0 * ggss ) * bqs / 3.0 - cqs * ( cqs / rfact ) - ( dq / rfact ) * aqs * aqs;
				oqs_solve_cubic_analytic_depressed( g2, h2, &rmax );
				if( std::isnan( rmax ) || std::isinf( rmax ) ) {
					oqs_solve_cubic_analytic_depressed_handle_inf( g2, h2, &rmax );
				}
				rmax *= rfact;
			}
		}
		// Newton-Raphson polish of phi0 on the depressed cubic x^3 + g x + h.
		double x = rmax;
		double xsq = x * x;
		double xxx = x * xsq;
		double gx = g * x;
		double f = x * ( xsq + g ) + h;
		double maxtt = fabs( xxx ) > fabs( gx ) ? fabs( xxx ) : fabs( gx );
		if( fabs( h ) > maxtt ) maxtt = fabs( h );
		if( fabs( f ) > oqs_macheps * maxtt ) {
			for( int iter = 0; iter < 8; iter++ ) {
				double df = 3.0 * xsq + g;
				if( df == 0 ) break;
				double xold = x;
				x += -f / df;
				double fold = f;
				xsq = x * x;
				f = x * ( xsq + g ) + h;
				if( f == 0 ) break;
				if( fabs( f ) >= fabs( fold ) ) { x = xold; break; }
			}
		}
		*phi0 = x;
	}

	// Relative-error metrics (eq. 29, 48-51, 68-69 in the manuscript).
	double oqs_calc_err_ldlt( double b, double c, double d, double d2, double l1, double l2, double l3 )
	{
		double s = ( b == 0 ) ? fabs( d2 + l1 * l1 + 2.0 * l3 ) : fabs( ( ( d2 + l1 * l1 + 2.0 * l3 ) - b ) / b );
		s += ( c == 0 ) ? fabs( 2.0 * d2 * l2 + 2.0 * l1 * l3 ) : fabs( ( ( 2.0 * d2 * l2 + 2.0 * l1 * l3 ) - c ) / c );
		s += ( d == 0 ) ? fabs( d2 * l2 * l2 + l3 * l3 ) : fabs( ( ( d2 * l2 * l2 + l3 * l3 ) - d ) / d );
		return s;
	}
	double oqs_calc_err_abcd( double a, double b, double c, double d, double aq, double bq, double cq, double dq )
	{
		double s = ( d == 0 ) ? fabs( bq * dq ) : fabs( ( bq * dq - d ) / d );
		s += ( c == 0 ) ? fabs( bq * cq + aq * dq ) : fabs( ( ( bq * cq + aq * dq ) - c ) / c );
		s += ( b == 0 ) ? fabs( bq + aq * cq + dq ) : fabs( ( ( bq + aq * cq + dq ) - b ) / b );
		s += ( a == 0 ) ? fabs( aq + cq ) : fabs( ( ( aq + cq ) - a ) / a );
		return s;
	}
	double oqs_calc_err_abc( double a, double b, double c, double aq, double bq, double cq, double dq )
	{
		double s = ( c == 0 ) ? fabs( bq * cq + aq * dq ) : fabs( ( ( bq * cq + aq * dq ) - c ) / c );
		s += ( b == 0 ) ? fabs( bq + aq * cq + dq ) : fabs( ( ( bq + aq * cq + dq ) - b ) / b );
		s += ( a == 0 ) ? fabs( aq + cq ) : fabs( ( ( aq + cq ) - a ) / a );
		return s;
	}
	double oqs_calc_err_d( double errmin, double d, double bq, double dq )
	{
		return ( ( d == 0 ) ? fabs( bq * dq ) : fabs( ( bq * dq - d ) / d ) ) + errmin;
	}

	// Newton-Raphson refine (alpha1, beta1, alpha2, beta2) against the
	// original quartic coefficients.  Converges in typically 2-4
	// iterations and drives total forward error below macheps.
	void oqs_NRabcd( double a, double b, double c, double d,
		double* AQ, double* BQ, double* CQ, double* DQ )
	{
		const int NITERMAX = 20;
		double x[4] = { *AQ, *BQ, *CQ, *DQ };
		double vr[4] = { d, c, b, a };
		double fvec[4];
		fvec[0] = x[1] * x[3] - d;
		fvec[1] = x[1] * x[2] + x[0] * x[3] - c;
		fvec[2] = x[1] + x[0] * x[2] + x[3] - b;
		fvec[3] = x[0] + x[2] - a;
		double errf = 0, errfa = 0, errfmin;
		double xmin[4] = { x[0], x[1], x[2], x[3] };
		for( int k1 = 0; k1 < 4; k1++ ) {
			double fveca = fabs( fvec[k1] );
			errf += ( vr[k1] == 0 ) ? fveca : fabs( fveca / vr[k1] );
			errfa += fveca;
		}
		errfmin = errfa;
		if( errfa == 0 ) return;

		for( int iter = 0; iter < NITERMAX; iter++ ) {
			double x02 = x[0] - x[2];
			double det = x[1] * x[1] + x[1] * ( -x[2] * x02 - 2.0 * x[3] ) + x[3] * ( x[0] * x02 + x[3] );
			if( det == 0.0 ) break;
			double Jinv[4][4];
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
			double dx[4] = { 0, 0, 0, 0 };
			for( int k1 = 0; k1 < 4; k1++ )
				for( int k2 = 0; k2 < 4; k2++ )
					dx[k1] += Jinv[k1][k2] * fvec[k2];
			for( int k1 = 0; k1 < 4; k1++ )
				x[k1] += -dx[k1] / det;
			fvec[0] = x[1] * x[3] - d;
			fvec[1] = x[1] * x[2] + x[0] * x[3] - c;
			fvec[2] = x[1] + x[0] * x[2] + x[3] - b;
			fvec[3] = x[0] + x[2] - a;
			double errfold = errf;
			errf = 0; errfa = 0;
			for( int k1 = 0; k1 < 4; k1++ ) {
				double fveca = fabs( fvec[k1] );
				errf += ( vr[k1] == 0 ) ? fveca : fabs( fveca / vr[k1] );
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
	int oqs_solve_quadratic_real( double a, double b, double out[2] )
	{
		double diskr = a * a - 4.0 * b;
		if( diskr < 0.0 ) return 0;
		double sq = sqrt( diskr );
		double div = ( a >= 0.0 ) ? ( -a - sq ) : ( -a + sq );
		double zmax = div / 2.0;
		double zmin = ( zmax == 0.0 ) ? 0.0 : ( b / zmax );
		out[0] = zmax;
		out[1] = zmin;
		return 2;
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

	// Translate from RISE's [leading ... constant] ordering into the
	// OQS monic form x^4 + a x^3 + b x^2 + c x + d = 0.
	const double a = coeff[1] / coeff[0];
	const double b = coeff[2] / coeff[0];
	const double c = coeff[3] / coeff[0];
	const double d = coeff[4] / coeff[0];

	// Solve for phi0; rescale if we overflow in the dominant-root step.
	double phi0 = 0;
	oqs_calc_phi0( a, b, c, d, &phi0, 0 );
	double rfact = 1.0;
	double A = a, B = b, C = c, D = d;
	if( std::isnan( phi0 ) || std::isinf( phi0 ) ) {
		rfact = oqs_quart_rescal_fact;
		A = a / rfact;
		double rfactsq = rfact * rfact;
		B = b / rfactsq;
		C = c / ( rfactsq * rfact );
		D = d / ( rfactsq * rfactsq );
		oqs_calc_phi0( A, B, C, D, &phi0, 1 );
	}

	// Build the LDL^T decomposition (eqs. 16-28).
	const double l1 = A / 2.0;
	const double l3 = B / 6.0 + phi0 / 2.0;
	const double del2 = C - A * l3;
	const double bl311 = 2.0 * B / 3.0 - phi0 - l1 * l1;
	const double dml3l3 = D - l3 * l3;

	double l2m[4], d2m[4], res[4];
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
	double d2 = 0, l2 = 0;
	if( nsol > 0 ) {
		int kmin = 0;
		double resmin = res[0];
		for( int k = 1; k < nsol; k++ ) {
			if( res[k] < resmin ) { resmin = res[k]; kmin = k; }
		}
		d2 = d2m[kmin];
		l2 = l2m[kmin];
	}

	// Build candidate (alpha1, beta1, alpha2, beta2) factorisation.  We
	// only care about the fully-real case (d2 <= 0) — complex roots
	// mean no ray-surface intersection, which the caller treats as miss.
	int realcase0 = ( d2 < 0.0 ) ? 1 : ( d2 > 0.0 ? 0 : -1 );
	double aq = 0, bq = 0, cq = 0, dq = 0;
	double errmin = 0;

	if( realcase0 == 1 ) {
		double gamma = sqrt( -d2 );
		aq = l1 + gamma;
		bq = l3 + gamma * l2;
		cq = l1 - gamma;
		dq = l3 - gamma * l2;
		if( fabs( dq ) < fabs( bq ) ) dq = D / bq;
		else if( fabs( dq ) > fabs( bq ) ) bq = D / dq;

		double aqv[3], cqv[3], errv[3];
		int kmin = 0;
		if( fabs( aq ) < fabs( cq ) ) {
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

	// Case d2 ≈ 0: check whether a simpler (identical-alpha, split-beta)
	// factorisation has lower forward error and, if so, use it.
	int whichcase = 0;
	if( realcase0 == -1 || ( fabs( d2 ) <= oqs_fact_d0 * ( fabs( 2.0 * B / 3.0 ) + fabs( phi0 ) + l1 * l1 ) ) ) {
		double d3 = D - l3 * l3;
		double err0 = ( realcase0 == 1 ) ? oqs_calc_err_d( errmin, D, bq, dq ) : std::numeric_limits<double>::infinity();
		if( d3 <= 0 ) {
			// Real case II: both quadratic factors have real roots with shared alpha.
			double sqrtd3 = sqrt( -d3 );
			double aq1 = l1, bq1 = l3 + sqrtd3, cq1 = l1, dq1 = l3 - sqrtd3;
			if( fabs( dq1 ) < fabs( bq1 ) ) dq1 = D / bq1;
			else if( fabs( dq1 ) > fabs( bq1 ) ) bq1 = D / dq1;
			double err1 = oqs_calc_err_abcd( A, B, C, D, aq1, bq1, cq1, dq1 );
			if( realcase0 == -1 || err1 < err0 ) {
				whichcase = 1;
				aq = aq1; bq = bq1; cq = cq1; dq = dq1;
				realcase0 = 1;  // swapped to the identical-alpha real case
			}
		}
		// complex d3 branch produces complex roots — no real solutions; skip.
	}

	// Extract real roots.  If realcase0 != 1 at this point the quartic
	// has all-complex roots (from our caller's standpoint, no ray hit).
	int num = 0;
	if( realcase0 == 1 ) {
		// Refine (alpha1, beta1, alpha2, beta2) via Newton-Raphson.
		oqs_NRabcd( A, B, C, D, &aq, &bq, &cq, &dq );

		double qr[2];
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

	// Unscale back to the original variable if we rescaled phi0.
	if( rfact != 1.0 ) {
		for( int i = 0; i < num; i++ ) sol[i] *= rfact;
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

