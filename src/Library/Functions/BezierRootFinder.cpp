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

static Scalar intPow(Scalar x, int n) {
	Scalar total = 1.0;
	for (int i = 0; i < n; i++) {
		total *= x;
	}
	return total;
}

// Precomputed Pascal's triangle in Scalar precision.  Replaces the
// factorial()-then-divide implementation that silently overflowed int32
// at n=13 (13! = 6.2e9 > 2^31) — for any polynomial of size >= 14 the
// old choose() returned garbage coefficients, which for bicubic-patch
// resultants (size up to 19) corrupted the Bernstein-subdivision root
// search and produced the intermittent "patch holes" we saw in the
// analytic teapot / aphrodite / f16 renders.  Using Scalar throughout
// also avoids a chain of int/Scalar conversions in the subdivide inner
// loop that the compiler could not hoist.
static const int MAX_CHOOSE_N = 64;          // generous bound — resultant size is <= ~24
static Scalar g_pascal[MAX_CHOOSE_N + 1][MAX_CHOOSE_N + 1];
static bool g_pascalReady = false;

static void BuildPascal() {
	if (g_pascalReady) return;
	for (int n = 0; n <= MAX_CHOOSE_N; n++) {
		g_pascal[n][0] = 1.0;
		for (int k = 1; k < n; k++) {
			g_pascal[n][k] = g_pascal[n-1][k-1] + g_pascal[n-1][k];
		}
		g_pascal[n][n] = 1.0;
		for (int k = n + 1; k <= MAX_CHOOSE_N; k++) {
			g_pascal[n][k] = 0.0;
		}
	}
	g_pascalReady = true;
}

// Constant-time choose(n, m) via Pascal table.  Returns 0 for m > n
// (matches the old behaviour) and for n > MAX_CHOOSE_N (was undefined
// overflow before; now returns 0 and assert-fires in debug builds).
static inline Scalar choose(int n, int m) {
	assert(n >= 0 && n <= MAX_CHOOSE_N);
	if (m < 0 || m > n) return 0.0;
	return g_pascal[n][m];
}

// Build the Pascal table at static-init time.
struct PascalInit {
	PascalInit() { BuildPascal(); }
};
static PascalInit g_pascalInit;

// Polynomial size bound — large enough for every in-tree caller.
// The ray/bicubic-bezier-patch resultant is degree 18 (size 19); bumping
// this to 64 leaves headroom for Resultant.cpp's over-sized SmallPolynomial
// (up to MAX_COEF=37) and any future degree increases without touching this
// file again.  Exceeding the bound drops to the legacy std::vector path.
static const int MAX_POLY_SIZE = 64;

// Bezier subdivision at t = 0.5 of a Bernstein-basis polynomial.  Produces
// two size-N polynomials (left half, right half) in the SAME basis.
//
// Uses a precomputed Pascal table so the inner multiply chain is plain FMAs
// (no integer factorials), and takes raw pointers so callers can stack-
// allocate the scratch buffers.  This is the single hottest loop in
// analytic-bezier rendering per the macOS `sample` profile taken on
// aphrodite — every ray-patch intersection triggers one univariate root
// search which triggers O(subdivision depth) calls to this function.
static inline void subdivide_fixed(
	const Scalar* coeff,
	Scalar*       coeff1,
	Scalar*       coeff2,
	const int     size
	)
{
	Scalar scale = 1.0;
	for (int i = 0; i < size; i++) {
		Scalar left = 0.0;
		Scalar right = 0.0;
		const Scalar* pc = &g_pascal[i][0];   // row i of Pascal
		for (int j = 0; j <= i; j++) {
			left  += pc[j] * coeff[j];
			right += pc[j] * coeff[size-1-j];
		}
		coeff1[i]        = left  / scale;
		coeff2[size-1-i] = right / scale;
		scale *= 2.0;
	}
}

static inline bool noRoot_fixed(const Scalar* coeff, const int size) {
	for (int i = 1; i < size; i++) {
		if (coeff[i] >= 0 && coeff[0] <= 0) return false;
		if (coeff[i] <= 0 && coeff[0] >= 0) return false;
	}
	return true;
}

// Stack-allocated recursive Bezier root search — single-root variant.
// Mirrors the pre-2026 std::vector-based function; the only change is that
// the subdivision buffers live on the recursion frame.  Depth is bounded
// by log2((b-a)/epsilon): for (1 - 0) / 1e-7 that's about 24 levels, so
// the worst-case stack footprint is 24 * 2 * MAX_POLY_SIZE * sizeof(Scalar)
// = ~24 KB — well under any thread-stack limit.
static bool SolveBernsteinFixed(
	const Scalar* coeff,
	const int size,
	const Scalar a,
	const Scalar b,
	Scalar& solution,
	const Scalar epsilon )
{
	if (noRoot_fixed(coeff, size)) return false;

	const Scalar mid = (a + b) / 2.0;
	if ((b - a < epsilon) || (mid <= a) || (b <= mid)) {
		solution = mid;
		return true;
	}

	Scalar coeff1[MAX_POLY_SIZE];
	Scalar coeff2[MAX_POLY_SIZE];
	subdivide_fixed(coeff, coeff1, coeff2, size);

	if (SolveBernsteinFixed(coeff1, size, a, mid, solution, epsilon)) return true;
	if (SolveBernsteinFixed(coeff2, size, mid, b, solution, epsilon)) return true;
	return false;
}

// Multi-root variant.
static int SolveBernsteinFixedMulti(
	const Scalar* coeff,
	const int size,
	const Scalar a,
	const Scalar b,
	std::vector<Scalar>& solutions,
	const Scalar epsilon )
{
	if (noRoot_fixed(coeff, size)) return 0;

	const Scalar mid = (a + b) / 2.0;
	if ((b - a < epsilon) || (mid <= a) || (b <= mid)) {
		if (coeff[0] != 0.0 && coeff[size-1] != 0.0) {
			solutions.push_back(mid);
			return 1;
		}
		return 0;
	}

	Scalar coeff1[MAX_POLY_SIZE];
	Scalar coeff2[MAX_POLY_SIZE];
	subdivide_fixed(coeff, coeff1, coeff2, size);

	int roots = 0;
	roots += SolveBernsteinFixedMulti(coeff1, size, a, mid, solutions, epsilon);
	if (coeff2[0] == 0.0) {
		assert(coeff1[size-1] == 0.0);
		solutions.push_back(mid);
		roots++;
	}
	roots += SolveBernsteinFixedMulti(coeff2, size, mid, b, solutions, epsilon);
	return roots;
}

// Convert standard-basis polynomial p(t) of given size to Bernstein basis.
// Writes `size` Bernstein coefficients into `out`.  Uses Pascal table via
// choose(); no vector allocations.
static inline void ConvertToBernsteinBasisFixed(
	const Scalar* stdbasis,
	const int     size,
	Scalar*       out )
{
	const int degree = size - 1;
	for (int i = 0; i < size; i++) {
		Scalar v = 0.0;
		for (int j = i; j >= 0; j--) {
			v += choose(i, j) / choose(degree, j) * stdbasis[j];
		}
		out[i] = v;
	}
}

// First compose p((t-a)/(b-a)) in the standard basis, then switch to
// Bernstein.  Same algebra as the pre-2026 two-step ConvertToBernsteinBasis
// but uses fixed-size scratch.
static inline void ConvertToBernsteinBasisRangeFixed(
	const Scalar* stdbasis,
	const int     size,
	const Scalar  a,
	const Scalar  b,
	Scalar*       out )
{
	Scalar adjusted[MAX_POLY_SIZE];
	for (int i = 0; i < size; i++) adjusted[i] = 0.0;

	const Scalar length = b - a;
	for (int i = 0; i < size; i++) {
		for (int j = 0; j <= i; j++) {
			adjusted[j] += choose(i, j) * intPow(a, i - j) * intPow(length, j) * stdbasis[i];
		}
	}
	ConvertToBernsteinBasisFixed(adjusted, size, out);
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
	const int size = static_cast<int>(coeff.size());
	if (size == 0) return false;
	if (size > MAX_POLY_SIZE) {
		// Fall back to std::vector path if someone hands us a degree > 63
		// polynomial.  Slower (heap allocations + integer overflow in the
		// legacy choose()) but keeps correctness for toy workloads.
		// Paste the old implementation here rather than recursing into it.
		std::vector<Scalar> bcoef(size);
		ConvertToBernsteinBasisRangeFixed(coeff.data(), size, a, b, bcoef.data());
		return SolveBernsteinFixed(bcoef.data(), size, a, b, solution, epsilon);
	}
	Scalar bcoef[MAX_POLY_SIZE];
	ConvertToBernsteinBasisRangeFixed(coeff.data(), size, a, b, bcoef);
	return SolveBernsteinFixed(bcoef, size, a, b, solution, epsilon);
}

//! Finds all roots of a given polynomial within a given range
/// \return Number of roots found
int Polynomial::SolvePolynomialWithinRange(
	const std::vector<Scalar> & coeff,			///< [in] Coefficients
	const Scalar a,
	const Scalar b,							///< [in] Search for roots between a and b.
	std::vector<Scalar> & solutions,		///< [out] Solutions
	const Scalar epsilon
	)
{
	const int size = static_cast<int>(coeff.size());
	if (size == 0) return 0;

	Scalar stackCoef[MAX_POLY_SIZE];
	std::vector<Scalar> heapCoef;
	Scalar* bcoef = stackCoef;
	if (size > MAX_POLY_SIZE) {
		heapCoef.resize(size);
		bcoef = heapCoef.data();
	}
	ConvertToBernsteinBasisRangeFixed(coeff.data(), size, a, b, bcoef);

	int roots = 0;
	if (bcoef[0] == 0.0) {
		solutions.push_back(a);
		roots++;
	}
	roots += SolveBernsteinFixedMulti(bcoef, size, a, b, solutions, epsilon);
	if (bcoef[size-1] == 0.0) {
		solutions.push_back(b);
		roots++;
	}
	return roots;
}

