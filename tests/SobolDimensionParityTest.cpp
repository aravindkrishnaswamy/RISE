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

	for( uint32_t i = 0; i < N; i++ ) {
		const double x = SobolSequence::Sample( i, dimA, seedA );
		const double y = SobolSequence::Sample( i, dimB, seedB );
		int bx = int( x * 3.0 ); if( bx > 2 ) bx = 2; if( bx < 0 ) bx = 0;
		int by = int( y * 3.0 ); if( by > 2 ) by = 2; if( by < 0 ) by = 0;
		j3[bx][by]++;
		mx[bx]++;
		my[by]++;
		d4[ ( x < 0.5 ? 0 : 1 ) + ( y < 0.5 ? 0 : 2 ) ]++;
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
	r.marginalDev = 0.0;
	for( int a = 0; a < 3; a++ ) {
		r.marginalDev = std::max( r.marginalDev,
			std::fabs( double( mx[a] ) / dN * 3.0 - 1.0 ) );
		r.marginalDev = std::max( r.marginalDev,
			std::fabs( double( my[a] ) / dN * 3.0 - 1.0 ) );
	}
	return r;
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
	// C. Sweep: every slot of one bounce's stream against the same slot
	// of the next bounce's stream -- the live-render configuration.
	// ----------------------------------------------------------------
	std::cout << std::endl << "C. Bounce-to-bounce sweep, all "
	          << 32 << " slots of streams 16 and 17, 2^16 samples" << std::endl;
	{
		double worst = 0.0;
		unsigned int worstSlot = 0;
		unsigned int nBad = 0;
		for( unsigned int k = 0; k < 32; k++ ) {
			const Joint j = ProbeJoint( 512 + k, seed, 544 + k, seed, 1u << 16 );
			if( j.worstRel > worst ) { worst = j.worstRel; worstSlot = k; }
			if( j.worstRel > 0.05 ) nBad++;
		}
		std::cout << "    worst 3x3 relative deviation " << std::setprecision( 5 ) << worst
		          << " at slot " << worstSlot
		          << ";  slots exceeding 5%: " << nBad << " of 32" << std::endl;
		Check( nBad == 0u, "C: no bounce-to-bounce slot pair exceeds 5% joint deviation" );
		Check( worst <= 0.05, "C: worst bounce-to-bounce slot pair within 5%" );
	}

	// ----------------------------------------------------------------
	// The property the fix must establish, stated on the sampler rather
	// than on SobolSequence: kStreamStride is even, so two bounces'
	// streams always differ by an even dimension offset.  That is fine
	// once distinct dimensions really are distinct sequences; it was
	// fatal while the dimension was reduced modulo 2.
	// ----------------------------------------------------------------
	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
