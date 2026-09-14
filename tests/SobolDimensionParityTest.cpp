//////////////////////////////////////////////////////////////////////
//
//  SobolDimensionParityTest.cpp - regression guard for DL-81
//    (docs/DEBT_LEDGER.md): two dimensions drawn from `SobolSequence`
//    must be a genuine two-dimensional low-discrepancy pair, not two
//    Owen-scrambled copies of ONE base sequence.
//
//  Why this test exists
//  --------------------
//    `SobolSequence::Sample` used to reduce the requested dimension to
//    `dimension & 1` -- "padded Sobol", where every dimension reuses
//    Sobol base dimension 0 or 1 and is distinguished only by its Owen
//    scramble seed.  That padding is only legitimate when the SAMPLE
//    INDEX is also permuted per dimension (Burley 2020; PBRT-v4's
//    `PaddedSobolSampler` does exactly that).  Without the index
//    permutation the two draws are `Owen_a(v_i)` and `Owen_b(v_i)` for
//    the SAME base value `v_i`, and Owen scrambling acts digit-by-digit:
//    the leading digit of each output is the leading digit of `v_i`
//    XOR a per-dimension bit.  So the leading digits of the two draws
//    are related by a FIXED bit, the joint distribution collapses onto
//    half of the dyadic 2x2 boxes, and the pair is useless as two
//    independent random decisions.
//
//    `SobolSampler::kStreamStride` is 32 -- EVEN -- and
//    `StartStream(16 + depth)` puts bounce `d`'s stream at dimension
//    `512 + 32d`.  The k-th draw of every bounce therefore has the same
//    parity as the k-th draw of every other bounce, so precisely the
//    draws that must be independent (Fresnel reflect/refract choice,
//    multi-lobe selection, Russian roulette at successive bounces) were
//    the collapsed ones.
//
//  What is asserted
//  ----------------
//    A. (0,2)-net preservation for the image-plane pair (dimensions 0
//       and 1).  Every dyadic elementary interval of the first 2^m
//       points must contain exactly one point.  This must hold both
//       before and after any change to the sampler -- it is the
//       property the padding existed to protect.
//    B. Joint occupancy.  For a same-parity pair (512+k, 544+k), a
//       different-parity pair (512+k, 545+k) and a same-parity pair
//       under two different scramble seeds, the 3x3 joint occupancy
//       must match the product of the marginals, and each dyadic 2x2
//       box must hold roughly a quarter of the points.  Pre-fix the
//       same-parity pair fails both; the two controls pass.
//    C. Sweep: the same joint-occupancy statistic over every pair of
//       the 32 dimensions of one bounce's stream against the next
//       bounce's stream, which is the configuration a render actually
//       draws.
//    D. Marginal uniformity of every dimension, so that a fix cannot
//       "pass" B by degrading the individual sequences into noise.
//
//  Author: Claude (debt-sobol slice, DL-81)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <algorithm>

#include "../src/Library/Sampling/SobolSequence.h"
#include "../src/Library/Utilities/SobolSampler.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const std::string& testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

//////////////////////////////////////////////////////////////////////
// A. (0,2)-net property of the image-plane dimension pair.
//
// For the first N = 2^m points of a (0,2)-sequence, every elementary
// interval [a/2^k, (a+1)/2^k) x [b/2^(m-k), (b+1)/2^(m-k)) contains
// EXACTLY one point, for every k in [0, m].  Owen scrambling preserves
// this.  Checked here for dimensions 0 and 1 -- the pair the film /
// pixel-position Get2D() draws.
//////////////////////////////////////////////////////////////////////
static void TestNetProperty( unsigned int m, uint32_t seed )
{
	const uint32_t N = 1u << m;
	std::vector<double> xs( N ), ys( N );
	for( uint32_t i = 0; i < N; i++ ) {
		xs[i] = SobolSequence::Sample( i, 0, seed );
		ys[i] = SobolSequence::Sample( i, 1, seed );
	}

	bool allOk = true;
	for( unsigned int k = 0; k <= m; k++ ) {
		const uint32_t nx = 1u << k;
		const uint32_t ny = 1u << ( m - k );
		std::vector<uint32_t> counts( size_t(nx) * size_t(ny), 0u );
		for( uint32_t i = 0; i < N; i++ ) {
			uint32_t bx = uint32_t( xs[i] * double(nx) );
			uint32_t by = uint32_t( ys[i] * double(ny) );
			if( bx >= nx ) bx = nx - 1;
			if( by >= ny ) by = ny - 1;
			counts[ size_t(by) * size_t(nx) + size_t(bx) ]++;
		}
		for( size_t b = 0; b < counts.size(); b++ ) {
			if( counts[b] != 1u ) { allOk = false; break; }
		}
		if( !allOk ) {
			std::cout << "    (0,2)-net violated at split k=" << k
			          << " (" << nx << "x" << ny << ")" << std::endl;
			break;
		}
	}

	char msg[192];
	std::snprintf( msg, sizeof(msg),
		"A: dims (0,1) form a (0,2)-net over 2^%u points, seed %u", m, seed );
	Check( allOk, msg );
}

//////////////////////////////////////////////////////////////////////
// B/C. Joint-occupancy statistic.
//////////////////////////////////////////////////////////////////////
struct Joint
{
	double worstRel;		//!< max |joint / (marginal_x * marginal_y) - 1| over a 3x3 partition
	double worstDyadic;		//!< max |count / (N/4) - 1| over the four dyadic 2x2 boxes
	double worstQuad;		//!< max |count / (N/16) - 1| over the sixteen dyadic 4x4 boxes
	double dyadic[4];		//!< the four 2x2 box fractions, as multiples of 1/4
	double marginalDev;		//!< max |marginal - 1/3| * 3 over both dimensions
};

static Joint ProbeJoint(
	uint32_t dimA, uint32_t seedA,
	uint32_t dimB, uint32_t seedB,
	uint32_t N )
{
	uint32_t j3[3][3] = {{0,0,0},{0,0,0},{0,0,0}};
	uint32_t mx[3] = {0,0,0};
	uint32_t my[3] = {0,0,0};
	uint32_t d4[4] = {0,0,0,0};
	uint32_t d16[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

	for( uint32_t i = 0; i < N; i++ ) {
		const double x = SobolSequence::Sample( i, dimA, seedA );
		const double y = SobolSequence::Sample( i, dimB, seedB );
		int bx = int( x * 3.0 ); if( bx > 2 ) bx = 2; if( bx < 0 ) bx = 0;
		int by = int( y * 3.0 ); if( by > 2 ) by = 2; if( by < 0 ) by = 0;
		j3[bx][by]++;
		mx[bx]++;
		my[by]++;
		d4[ ( x < 0.5 ? 0 : 1 ) + ( y < 0.5 ? 0 : 2 ) ]++;
		int qx = int( x * 4.0 ); if( qx > 3 ) qx = 3; if( qx < 0 ) qx = 0;
		int qy = int( y * 4.0 ); if( qy > 3 ) qy = 3; if( qy < 0 ) qy = 0;
		d16[ qy * 4 + qx ]++;
	}

	Joint r;
	r.worstRel = 0.0;
	const double dN = double( N );
	for( int a = 0; a < 3; a++ ) {
		for( int b = 0; b < 3; b++ ) {
			const double expected = ( double( mx[a] ) / dN ) * ( double( my[b] ) / dN );
			if( expected <= 0.0 ) { r.worstRel = 1.0; continue; }
			const double rel = ( double( j3[a][b] ) / dN ) / expected;
			r.worstRel = std::max( r.worstRel, std::fabs( rel - 1.0 ) );
		}
	}
	r.worstDyadic = 0.0;
	for( int b = 0; b < 4; b++ ) {
		r.dyadic[b] = ( double( d4[b] ) / dN ) * 4.0;
		r.worstDyadic = std::max( r.worstDyadic, std::fabs( r.dyadic[b] - 1.0 ) );
	}
	r.worstQuad = 0.0;
	for( int b = 0; b < 16; b++ ) {
		r.worstQuad = std::max( r.worstQuad,
			std::fabs( ( double( d16[b] ) / dN ) * 16.0 - 1.0 ) );
	}
	r.marginalDev = 0.0;
	for( int a = 0; a < 3; a++ ) {
		r.marginalDev = std::max( r.marginalDev,
			std::fabs( double( mx[a] ) / dN * 3.0 - 1.0 ) );
		r.marginalDev = std::max( r.marginalDev,
			std::fabs( double( my[a] ) / dN * 3.0 - 1.0 ) );
	}
	return r;
}

//////////////////////////////////////////////////////////////////////
// Independent helpers for the generator self-check.  Deliberately
// written separately from SobolSequence's own versions: sharing them
// would make E1 a tautology.
//////////////////////////////////////////////////////////////////////
static uint32_t EulerPhi( uint32_t n )
{
	uint32_t result = n;
	for( uint32_t q = 2; q * q <= n; q++ ) {
		if( n % q == 0 ) {
			while( n % q == 0 ) n /= q;
			result -= result / q;
		}
	}
	if( n > 1 ) result -= result / n;
	return result;
}

//! Multiply two GF(2) polynomials modulo `p` of degree `d`, by plain
//! shift-and-add with reduction after every shift.
static uint32_t GF2Mul( uint32_t a, uint32_t b, uint32_t p, unsigned int d )
{
	uint32_t r = 0;
	if( ( a >> d ) & 1u ) a ^= p;		// x itself needs reducing when d == 1
	for( unsigned int i = 0; i < 32; i++ ) {
		if( ( b >> i ) & 1u ) r ^= a;
		a <<= 1;
		if( ( a >> d ) & 1u ) a ^= p;
	}
	return r;
}

//! Does x have multiplicative order exactly 2^d - 1 modulo `p`?
//! Computed by walking the powers of x until it returns to 1, which
//! shares no code path with the generator's version (that one raises x
//! to n and to n/q for each prime factor q of n).
static bool PolyOrderIsFull( uint32_t p, unsigned int d )
{
	const uint32_t n = ( 1u << d ) - 1u;
	uint32_t x = GF2Mul( 1u, 2u, p, d );		// x, reduced mod p
	uint32_t order = 1u;
	while( x != 1u && order <= n ) { x = GF2Mul( x, 2u, p, d ); order++; }
	return x == 1u && order == n;
}

static void ReportAndCheck( const char* label, const Joint& j, double relTol, double dyadicTol )
{
	std::cout << "    " << std::left << std::setw( 44 ) << label << std::right
	          << "  worst 3x3 rel dev " << std::fixed << std::setprecision( 5 ) << j.worstRel
	          << "   dyadic 2x2 x4 = [" << std::setprecision( 4 )
	          << j.dyadic[0] << " " << j.dyadic[1] << " "
	          << j.dyadic[2] << " " << j.dyadic[3] << "]" << std::endl;

	char msg[256];
	std::snprintf( msg, sizeof(msg), "%s: 3x3 joint == product of marginals within %.1f%%",
		label, relTol * 100.0 );
	Check( j.worstRel <= relTol, msg );
	std::snprintf( msg, sizeof(msg), "%s: every dyadic 2x2 box holds N/4 within %.1f%%",
		label, dyadicTol * 100.0 );
	Check( j.worstDyadic <= dyadicTol, msg );
}

int main( int argc, char** argv )
{
	uint32_t seed = 0x5eed1234u;
	if( argc > 1 && argv[1] ) {
		const long v = std::strtol( argv[1], nullptr, 10 );
		if( v > 0 ) seed = uint32_t( v );
	}

	std::cout << "=== SobolDimensionParityTest ===" << std::endl;
	std::cout << "scramble seed = " << seed << std::endl;

	// ----------------------------------------------------------------
	// A. The image-plane pair must stay a (0,2)-net.
	// ----------------------------------------------------------------
	std::cout << std::endl << "A. (0,2)-net property of the image-plane pair (dims 0,1)" << std::endl;
	TestNetProperty( 10, seed );
	TestNetProperty( 12, seed ^ 0x9e3779b9u );

	// ----------------------------------------------------------------
	// B. The three-way comparison the DL-81 row describes.
	//
	// Dimensions 512 and 544 are eye-bounce 0 and eye-bounce 1 of a
	// live render: SobolSampler::StartStream(16 + depth) with
	// kStreamStride 32.  Dimension 545 is the same bounce-1 stream one
	// slot further along, i.e. the different-parity control.
	// ----------------------------------------------------------------
	const uint32_t N = 1u << 20;
	std::cout << std::endl << "B. Joint occupancy over 2^20 samples" << std::endl;

	double worstSame = 0.0;
	for( uint32_t k = 0; k < 4; k++ )
	{
		char label[128];

		const Joint same = ProbeJoint( 512 + k, seed, 544 + k, seed, N );
		std::snprintf( label, sizeof(label), "same parity   (%u,%u) same seed", 512 + k, 544 + k );
		ReportAndCheck( label, same, 0.02, 0.02 );
		worstSame = std::max( worstSame, same.worstRel );

		const Joint diff = ProbeJoint( 512 + k, seed, 545 + k, seed, N );
		std::snprintf( label, sizeof(label), "diff parity   (%u,%u) same seed", 512 + k, 545 + k );
		ReportAndCheck( label, diff, 0.02, 0.02 );

		const Joint indep = ProbeJoint( 512 + k, seed, 544 + k, seed ^ 0xabcd1234u, N );
		std::snprintf( label, sizeof(label), "same parity   (%u,%u) indep seed", 512 + k, 544 + k );
		ReportAndCheck( label, indep, 0.02, 0.02 );

		// Marginals must stay uniform in every configuration -- this is
		// the guard against "fixing" the joint statistic by turning the
		// individual sequences into white noise.
		std::snprintf( label, sizeof(label),
			"D: marginals of dims %u/%u uniform within 1%%", 512 + k, 544 + k );
		Check( same.marginalDev <= 0.01, label );
	}
	std::cout << "    worst same-parity 3x3 relative deviation over k=0..3: "
	          << std::setprecision( 5 ) << worstSame << std::endl;

	// ----------------------------------------------------------------
	// C. Bounce-to-bounce sweep -- the live-render configuration.
	//
	// Every slot of eye-bounce 0's stream against the SAME slot of
	// eye-bounce 1's stream, judged by DYADIC box occupancy, which is
	// the criterion a digital net is actually built to satisfy (the 3x3
	// partition above is deliberately non-dyadic, and a pair with a
	// large t-value concentrates its error on those boundaries).
	//
	// The two sample counts are not interchangeable.  Over the first
	// 2^M samples only index bits 0..M-1 vary, so a pair's
	// equidistribution is governed by its generator rows TRUNCATED to M
	// columns -- and a dimension's leading row has only 2^(M-1)
	// distinct values there.  With 2311 dimensions that is 128 values
	// at 256 samples per pixel, so SOME pairs of the 2311 must share a
	// leading row and collapse their 2x2 occupancy; it is a counting
	// bound, not something better direction numbers could fix (see
	// SobolSequence.h, "What this does NOT fix").  The 32 pairs swept
	// here are clear of it, and this test pins that.
	// ----------------------------------------------------------------
	std::cout << std::endl << "C. Bounce-to-bounce sweep, all 32 slots of streams 16 and 17"
	          << std::endl;
	{
		static const uint32_t kCounts[2] = { 1u << 8, 1u << 12 };
		for( int c = 0; c < 2; c++ )
		{
			const uint32_t n = kCounts[c];
			double worst2 = 0.0, worst4 = 0.0, worst3 = 0.0;
			unsigned int slot2 = 0, slot4 = 0, collapsed = 0;
			for( unsigned int k = 0; k < 32; k++ ) {
				const Joint j = ProbeJoint( 512 + k, seed, 544 + k, seed, n );
				if( j.worstDyadic > worst2 ) { worst2 = j.worstDyadic; slot2 = k; }
				if( j.worstQuad   > worst4 ) { worst4 = j.worstQuad;   slot4 = k; }
				if( j.worstDyadic > 1e-9 ) collapsed++;
				worst3 = std::max( worst3, j.worstRel );
			}
			std::cout << "    N=" << std::setw( 5 ) << n
			          << "  worst 2x2 dyadic dev " << std::fixed << std::setprecision( 5 )
			          << worst2 << " (slot " << slot2 << ")"
			          << "   worst 4x4 dyadic dev " << worst4 << " (slot " << slot4 << ")"
			          << "   worst 3x3 dev " << worst3
			          << "   slots with a 2x2 collapse: " << collapsed << "/32" << std::endl;

			char msg[192];
			if( n >= ( 1u << 12 ) ) {
				// Far enough above the counting bound that every pair
				// here has a distinct leading row: demand exactness.
				std::snprintf( msg, sizeof(msg),
					"C: every 2x2 AND 4x4 dyadic box exact on all 32 slots at N=%u", n );
				Check( worst2 <= 1e-9 && worst4 <= 1e-9, msg );
			} else {
				// At 256 samples per pixel only 128 distinct leading
				// rows exist for 2311 dimensions, so ~0.78% of ALL
				// dimension pairs must collapse (SobolSequence.h, "What
				// this does NOT fix") -- about 0.25 of these 32.  One is
				// consistent with that; 32 is the pre-DL-81 collapse.
				std::snprintf( msg, sizeof(msg),
					"C: at most 2 of 32 slots collapse at N=%u (counting bound predicts ~0.25)", n );
				Check( collapsed <= 2u, msg );
			}
		}
	}

	// ----------------------------------------------------------------
	// E. Generator self-checks.
	//
	// The direction numbers are BUILT at first use rather than
	// tabulated, so the build itself needs pinning.
	//   E1  The number of primitive polynomials the enumeration accepts
	//       at each degree d must be phi(2^d - 1) / d, computed here by
	//       an independent Euler-phi over the trial-division
	//       factorisation -- nothing the generator shares.
	//   E2  Sobol' dimensions 0 and 1 must still be exactly the
	//       closed-form van der Corput and x+1 sequences the padded
	//       implementation used, so the image plane is untouched.
	// ----------------------------------------------------------------
	std::cout << std::endl << "E. Generator self-checks" << std::endl;
	{
		// E1 -- count what the same enumeration rule accepts, using an
		// independently written primitivity test, and compare the per
		// degree totals against phi(2^d - 1) / d.
		unsigned int accepted = 1;		// dimension 0 carries no polynomial
		bool countsOk = true;
		for( unsigned int d = 1; d <= 15 && accepted < 2311; d++ ) {
			unsigned int got = 0;
			for( uint32_t a = 0; a < ( 1u << ( d - 1 ) ) && accepted < 2311; a++ ) {
				const uint32_t poly = ( 1u << d ) | ( a << 1 ) | 1u;
				if( PolyOrderIsFull( poly, d ) ) { got++; accepted++; }
			}
			const uint32_t n = ( 1u << d ) - 1u;
			const unsigned int expect = EulerPhi( n ) / d;
			// The last degree is truncated by the 2311 cap, so only
			// compare the degrees that ran to completion.
			if( accepted < 2311 && got != expect ) {		// NOLINT
				countsOk = false;
				std::cout << "    degree " << d << ": accepted " << got
				          << ", phi(2^d-1)/d = " << expect << std::endl;
			} else {
				std::cout << "    degree " << std::setw( 2 ) << d << ": "
				          << std::setw( 4 ) << got << " primitive"
				          << ( accepted < 2311 ? "" : " (truncated by the 2311 cap)" )
				          << ",  phi(2^d-1)/d = " << expect << std::endl;
			}
		}
		Check( countsOk,
			"E1: primitive-polynomial counts per degree equal phi(2^d-1)/d" );

		bool d0 = true, d1 = true;
		for( uint32_t i = 0; i < ( 1u << 16 ); i++ ) {
			if( SobolSequence::Sobol( i, 0 ) != SobolSequence::SobolDim0( i ) ) d0 = false;
			if( SobolSequence::Sobol( i, 1 ) != SobolSequence::SobolDim1( i ) ) d1 = false;
		}
		Check( d0, "E2: table dimension 0 == SobolDim0 (van der Corput), 2^16 indices" );
		Check( d1, "E2: table dimension 1 == SobolDim1 (x+1), 2^16 indices" );
	}

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
