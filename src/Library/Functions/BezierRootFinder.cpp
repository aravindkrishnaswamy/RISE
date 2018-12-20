//////////////////////////////////////////////////////////////////////
//
//  BezierRootFinder.cpp - Implementation of bezier root finding function
//
//  Author: Russell O'Connor
//  Date of Birth: March 15, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Polynomial.h"
#include <assert.h>

using namespace RISE;

// From Numerical Recipes in C++ 2nd Edition, p. 219
/*
// Gamma function, returns the value of ln(gamma(xx))
static Scalar gammln( const Scalar xx )
{
	Scalar x, y, tmp, ser;
	static const Scalar cof[6] = {76.18009172947146, -86.50532032941677, 24.01409824083091, -1.231739572450155, 0.1208650973866179e-2, -0.5395239384953e-5};

	y = x = xx;
	tmp = x+5.5;
	tmp -= (x+0.5) * log(tmp);
	ser = 1.000000000190015;

	for( int j=0; j<6; j++ ) {
		ser += cof[j]/++y;
	}

	return (-tmp + log(2.5066282746310005*ser/x));
}

// Computes the factorial of n with Scalar precision
static Scalar factorial( const int n )
{
	static int ntop = 4;
	static Scalar a[33] = {1.0,1.0,2.0,6.0,24.0};		// Fill in the table only as required

	assert( n >= 0 );

	if( n > 32 ) {
		return exp( gammln(n+1.0) );
	}

	// Fill the table up to the desired value
	while( ntop < n ) {
		const int j = ntop++;
		a[ntop] = a[j]*ntop;
	}

	return a[n];
}

// Computes ln(n!)
static Scalar factln( const int n )
{
	static Scalar a[101] = {0};

	assert( n >= 0 );

	if( n <= 1 ) {
		return 0.0;
	}

	if( n <= 100 ) {
		// In range of table
		return (a[n] != 0.0 ? a[n] : (a[n] = gammln(n+1.0)));
	}

	return gammln(n+1.0); // out of range of table
}

// Computes the binomial coefficient of n choose k
static Scalar choose( const int n, const int k )
{
	return floor( 0.5 + exp(factln(n)-factln(k)-factln(n-k)));
	// The floor cleans up roundoff error for smaller values of n and k
}

// Computes the beta function of (z,w)
static Scalar beta( const Scalar z, const Scalar w )
{
	return exp( gammln(z)+gammln(w)-gammln(z+w) );
}
*/

static int factorial(int n) {
	int total = 1;
	for (int i = 0; i < n; i++) {
		total *= i+1;
	}
	return total;
}
/*
static int factorial(int n)
{
	static int ntop = 4;
	static int a[33] = {1,1,2,6,24};		// Fill in the table only as required

	assert( n >= 0 );

	if( n > 32 ) {
		return do_factorial(n);
	}

	// Fill the table up to the desired value
	while( ntop < n ) {
		const int j = ntop++;
		a[ntop] = a[j]*ntop;
	}

	return a[n];
}
*/
static Scalar intPow(Scalar x, int n) {
	Scalar total = 1.0;
	for (int i = 0; i < n; i++) {
		total *= x;
	}
	return total;
}

// This can be faster.
static int choose(int n, int m) {
	if (m > n) return 0;
	return factorial(n)/factorial(m)/factorial(n-m);
}

// This can totally be done with dynamic programming or something.
// Look up proper bezier subdivision.
static void subdivide(
	const std::vector<Scalar> & coeff,
	std::vector<Scalar> & coeff1,
	std::vector<Scalar> & coeff2
	)
{
	int size = coeff.size();
	int scale = 1;
	for (int i = 0; i < size; i++) {
		coeff1[i] = 0;
		coeff2[size-1-i] = 0;
		for (int j=0; j < i+1; j++) {
			coeff1[i] += choose(i, j)*coeff[j];
			coeff2[size-1-i] += choose(i, j)*coeff[size-1-j];
		}
		coeff1[i] /= scale;
		coeff2[size-1-i] /= scale;
		scale *= 2;
	}
}

static bool noRoot(const std::vector<Scalar> & coeff) {
	const int size = coeff.size();
	for (int i = 1; (i < size); i++) {
		if (coeff[i] >= 0 && coeff[0] <= 0) return false;
		if (coeff[i] <= 0 && coeff[0] >= 0) return false;
	}
	return true;
}

static bool SlowSolveBernsteinBasisPolynomialWithinRange( 
	const std::vector<Scalar> & coeff,					///< [in] Coefficients with bernstein Basis
	const Scalar a,
	const Scalar b,							///< [in] Search for a root between a and b.
	Scalar& solution,						///< [out] Solution
	const Scalar epsilon
	)
{
	if (noRoot(coeff)) return false;

    const Scalar mid = (a+b)/2.0;

	if ((b-a < epsilon) ||
	    (mid <= a) ||
	    (b <= mid)) 
	{
		solution = mid;
		return true;
	}

	{
		const int size = coeff.size();				/// degree+1;
		std::vector<Scalar> coeff1(size);
		std::vector<Scalar> coeff2(size);

		subdivide(coeff, coeff1, coeff2);
		if (SlowSolveBernsteinBasisPolynomialWithinRange(coeff1, a, mid, solution, epsilon)) return true;
		if (SlowSolveBernsteinBasisPolynomialWithinRange(coeff2, mid, b, solution, epsilon)) return true;

	}
	return false;
}

static int SlowSolveBernsteinBasisPolynomialWithinRange( 
	const std::vector<Scalar> & coeff,					///< [in] Coefficients with bernstein Basis
	const Scalar a,
	const Scalar b,							///< [in] Search for a root between a and b.
	std::vector<Scalar> & solutions,		///< [out] Solutions
	const Scalar epsilon
	)
{
	if (noRoot(coeff)) return 0;

	const int size = coeff.size();				/// degree+1;
    const Scalar mid = (a+b)/2.0;

	if ((b-a < epsilon) ||
	    (mid <= a) ||
	    (b <= mid)) 
	{
		if (0.0!=coeff[0] && 0.0!=coeff[size-1]) {
			solutions.push_back(mid);
			return 1;
		} else {
			return 0;
		}
	}

	{
		std::vector<Scalar> coeff1(size);
		std::vector<Scalar> coeff2(size);
		int roots = 0;

		subdivide(coeff, coeff1, coeff2);
		roots += SlowSolveBernsteinBasisPolynomialWithinRange(coeff1, a, mid, solutions, epsilon);
		{
			if (coeff2[0]==0.0) {
				assert(coeff1[coeff1.size()-1]==0.0);
				roots++;
				solutions.push_back(mid);
			}
		}
		roots += SlowSolveBernsteinBasisPolynomialWithinRange(coeff2, mid, b, solutions, epsilon);
		return roots;
	}
}

static void ConvertToBernsteinBasis(
	const std::vector<Scalar> & stdbasis,
	std::vector<Scalar> & bernsteinbasis
	)
{
	int size = stdbasis.size();
	int degree = size-1;
	for (int i = 0; i < size; i++) {
		bernsteinbasis[i] = 0;
		for (int j=i; j >= 0; j--) {
			bernsteinbasis[i] += (Scalar)(choose(i,j))/(Scalar)(choose(degree,j))*stdbasis[j];
		}
	}
	
}

static void ConvertToBernsteinBasis(
	const std::vector<Scalar> & stdbasis,
	const Scalar a,
	const Scalar b,
	std::vector<Scalar> & bernsteinbasis
	)
{
	// Find the polynomial p((t-a)/(b-a)) in the standard basis
	std::vector<Scalar> adjustedbasis = stdbasis;
	int size = stdbasis.size();
	const Scalar length = b-a;

	for (int i = 0; i < size; i++) {
		adjustedbasis[i] = 0;
		for (int j=0; j < i+1; j++) {
			adjustedbasis[j] += choose(i, j)*intPow(a,i-j)*intPow(length,j)*stdbasis[i];
		}
	}
	ConvertToBernsteinBasis(adjustedbasis, bernsteinbasis);
}

//! Finds the smallest root of a given polynomial within a given range
/// \return TRUE if there is a solution, FALSE otherwise
bool Polynomial::SolvePolynomialWithinRange( 
	const std::vector<Scalar> & coeff,			///< [in] Coefficients
	const Scalar a,
	const Scalar b,							///< [in] Search for a root between a and b.
	Scalar& solution,						///< [out] Solution
	const Scalar epsilon
	)
{
	std::vector<Scalar> newcoeff(coeff.size());

	ConvertToBernsteinBasis(coeff, a, b, newcoeff);

	return SlowSolveBernsteinBasisPolynomialWithinRange(newcoeff, a, b, solution, epsilon);
}

//! Finds the smallest root of a given polynomial within a given range
/// \return TRUE if there is a solution, FALSE otherwise
int Polynomial::SolvePolynomialWithinRange( 
	const std::vector<Scalar> & coeff,			///< [in] Coefficients
	const Scalar a,
	const Scalar b,							///< [in] Search for a root between a and b.
	std::vector<Scalar> & solutions,		///< [out] Solution
	const Scalar epsilon
	)
{
	int size = coeff.size();
	//int oldsolutions = solutions.size();
	std::vector<Scalar> newcoeff(size);

	ConvertToBernsteinBasis(coeff, a, b, newcoeff);

	int roots = 0;
	if (0.0==newcoeff[0]) {
		roots++;
		solutions.push_back(a);
	}
	roots += SlowSolveBernsteinBasisPolynomialWithinRange(newcoeff, a, b, solutions, epsilon);
	if (0.0==newcoeff[size-1]) {
		roots++;
		solutions.push_back(b);
	}
//	assert(solutions.size() == oldsolutions+roots);
	return roots;
}

