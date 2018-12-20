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

int Polynomial::SolveQuartic( const Scalar (&coeff)[ 5 ], Scalar (&sol)[ 4 ] )
{
    int     num=0;

	// If the first coefficient is 0, then get the quadratic solver
	// to solve this root
	if( IsReallyZero( coeff[0] ) )
	{
		Scalar	coeffs[4] = { coeff[1], coeff[2], coeff[3], coeff[4] };
		Scalar	sols[3];
		num = SolveCubic( coeffs, sols );

		for( int cnt=0; cnt<num; cnt++ ) {
			sol[ cnt ] = sols[ cnt ];
		}
		return num;
	}

    /* normal form: x^4 + Ax^3 + Bx^2 + Cx + D = 0 */

    Scalar A = coeff[ 1 ] / coeff[ 0 ];
    Scalar B = coeff[ 2 ] / coeff[ 0 ];
    Scalar C = coeff[ 3 ] / coeff[ 0 ];
    Scalar D = coeff[ 4 ] / coeff[ 0 ];

    /*  substitute x = y - A/4 to eliminate cubic term:
	x^4 + px^2 + qx + r = 0 */

    Scalar sq_A = A * A;
    Scalar p = - 3.0/8 * sq_A + B;
    Scalar q = 1.0/8 * sq_A * A - 1.0/2 * A * B + C;
    Scalar r = - 3.0/256*sq_A*sq_A + 1.0/16*sq_A*B - 1.0/4*A*C + D;

    if (IsReallyZero(r))
    {
		/* no absolute term: y(y^3 + py + q) = 0 */
		Scalar  coeffs[ 4 ];

		coeffs[ 0 ] = 1;
		coeffs[ 1 ] = 0;
		coeffs[ 2 ] = p;
		coeffs[ 3 ] = q;

		Scalar		sols[3];

		num = SolveCubic(coeffs, sols);

		for( int cnt=0; cnt<num; cnt++ ) {
			sol[ cnt ] = sols[ cnt ];
		}

		sol[ num++ ] = 0;
    }
    else
    {
		/* solve the resolvent cubic ... */
		Scalar  coeffs[ 4 ];

		coeffs[ 0 ] = 1;
		coeffs[ 1 ] = - 1.0/2 * p;
		coeffs[ 2 ] = - r;
		coeffs[ 3 ] = 1.0/2 * r * p - 1.0/8 * q * q;

		Scalar		sols[3];

		SolveCubic(coeffs, sols);

		/* ... and take the one real solution ... */

		Scalar z = sols[ 0 ];

		/* ... to build two quadric equations */

		Scalar u = z * z - r;
		Scalar v = 2 * z - p;

		if (IsCloseEnoughToZero(u)) {
			u = 0;
		} else if (u > 0) {
			u = sqrt(u);
		} else {
			return 0;
		}

		if (IsCloseEnoughToZero(v)) {
			v = 0;
		} else if (v > 0) {
			v = sqrt(v);
		} else {
			return 0;
		}

		Scalar	qcoeff[3];
		
		qcoeff[ 0 ] = 1;
		qcoeff[ 1 ] = q < 0 ? -v : v;
		qcoeff[ 2 ] = z - u;

		Scalar		solsA[2];
		Scalar		solsB[2];

		num = SolveQuadric(qcoeff, solsA);

		int cnt = 0;
		for( ; cnt<num; cnt++ ) {
			sol[ cnt ] = solsA[ cnt ];
		}

		qcoeff[ 0 ] = 1;
		qcoeff[ 1 ] = q < 0 ? v : -v;
		qcoeff[ 2 ] = z + u;

		num += SolveQuadric(qcoeff, solsB);

		for( int y=0; cnt<num; cnt++, y++ ) {
			sol[ cnt ] = solsB[ y ];
		}
    }

    /* resubstitute */

    Scalar sub = 0.25 * A;

    for (int i = 0; i < num; ++i) {
		sol[ i ] -= sub;
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

