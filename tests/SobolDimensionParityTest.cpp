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
//    G. Get2D's OWN index-scramble collapse (round-2 review, P2-1): two
//       Get2D GROUPS whose `ScrambleIndex` calls happen to share the
//       same Boolean function of the sample index's upper bits have
//       their leading digit locked together below 2^M samples, in
//       every pixel.  A second counting floor, independent of C's;
//       pinned near its measured value, not fixed (see
//       SobolSequence.h).
//    H. The wrap-region stream families (DL-283): PT's volume-walk
//       streams and BDPT/VCM/MLT's medium distance-sampling blocks live
//       past the 8192-dimension table by design, so each of their Get1D
//       draws reads a table row some UNWRAPPED main-loop dimension also
//       reads, at an Owen-permuted index.  Measured with the same
//       dyadic 2x2 leading-digit collapse statistic as G, against (1)
//       that aliased main-loop dimension, (2) the walk iteration's own
//       vertex stream, (3) the other walk's block at the same iteration
//       (same row, different wrap), beside the floor of ordinary
//       adjacent production streams.
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
#include "../src/Library/Utilities/BDPTUtilities.h"
#include "../src/Library/Utilities/PathTransportUtilities.h"

// Declared by SobolSequence.h once the library consumes them; repeated
// here so this test also builds against a library that does not yet.
namespace RISE
{
	const uint32_t* SobolJoeKuoInitialNumbers();
	unsigned int SobolJoeKuoDimensionCount();
	unsigned int SobolJoeKuoRecordCount();
}

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

//! Index of the highest set bit of a non-zero value.
static inline unsigned int HighBit( uint32_t v )
{
	unsigned int r = 0;
	while( ( v >>= 1 ) != 0 ) r++;
	return r;
}

//////////////////////////////////////////////////////////////////////
// C. t-values of the PRODUCTION dimension set, read off the
//    generator matrices.
//
// The dyadic-occupancy probes above sample the sequence; these read
// what the sequence is BUILT from, which is both exact and cheap
// enough to sweep every pair.
//
// For a 2-dimensional digital net over the first 2^m points, the
// generator matrices are the dimensions' direction numbers truncated
// to m rows and m columns.  Its t-value is the smallest t such that
// for EVERY split k1 + k2 = m - t, the first k1 rows of one matrix
// together with the first k2 rows of the other are linearly
// independent over GF(2).  t = 0 is a perfect net; t = m - 1 means
// even the (1,1) split fails, i.e. the two leading rows are equal --
// which is exactly the "fully collapsed dyadic 2x2" DL-81 was about,
// half the boxes empty and half at double density.
//////////////////////////////////////////////////////////////////////

//! Generator-matrix rows of `dim`, truncated to m columns.  Row r is
//! bit (31 - r) of each of the first m direction numbers.  Reads the
//! TABLE, via SobolSequence::DirectionNumber -- not `Sobol`, which
//! short-circuits dimensions 0 and 1 to closed forms.
static void GeneratorRows( uint32_t dim, unsigned int m, uint32_t* rows )
{
	for( unsigned int r = 0; r < m; r++ ) {
		uint32_t mask = 0;
		for( unsigned int c = 0; c < m; c++ ) {
			if( ( SobolSequence::DirectionNumber( dim, c ) >> ( 31u - r ) ) & 1u ) {
				mask |= ( 1u << c );
			}
		}
		rows[r] = mask;
	}
}

//! Are the first k1 rows of A and the first k2 rows of B jointly
//! linearly independent over GF(2)?  Plain Gaussian elimination.
static bool RowsIndependent(
	const uint32_t* A, unsigned int k1,
	const uint32_t* B, unsigned int k2 )
{
	uint32_t basis[64];
	unsigned int n = 0;
	for( unsigned int i = 0; i < k1 + k2; i++ ) {
		uint32_t v = ( i < k1 ) ? A[i] : B[i - k1];
		for( unsigned int j = 0; j < n; j++ ) {
			if( v != 0 && HighBit( v ) == HighBit( basis[j] ) ) v ^= basis[j];
		}
		if( v == 0 ) return false;
		basis[n++] = v;
		for( unsigned int j = n - 1; j > 0; j-- ) {
			if( HighBit( basis[j] ) > HighBit( basis[j - 1] ) ) {
				const uint32_t tmp = basis[j]; basis[j] = basis[j - 1]; basis[j - 1] = tmp;
			} else {
				break;
			}
		}
	}
	return true;
}

//! t-value of the 2D net (dimA, dimB) over the first 2^m points.
static unsigned int TValue( uint32_t dimA, uint32_t dimB, unsigned int m )
{
	uint32_t A[32], B[32];
	GeneratorRows( dimA, m, A );
	GeneratorRows( dimB, m, B );
	for( unsigned int t = 0; t <= m; t++ ) {
		const unsigned int r = m - t;
		bool ok = true;
		for( unsigned int k1 = 0; k1 <= r && ok; k1++ ) {
			if( !RowsIndependent( A, k1, B, r - k1 ) ) ok = false;
		}
		if( ok ) return t;
	}
	return m;
}

struct PairSweep
{
	unsigned int pairs;
	unsigned int collapsed;		//!< t == m-1: leading rows equal
	unsigned int maxT;
};

static PairSweep SweepPairs(
	const std::vector<uint32_t>& dimsA,
	const std::vector<uint32_t>& dimsB,
	bool allCross,
	unsigned int m )
{
	PairSweep r; r.pairs = 0; r.collapsed = 0; r.maxT = 0;
	for( size_t i = 0; i < dimsA.size(); i++ ) {
		const size_t jStart = allCross ? 0 : i + 1;
		const std::vector<uint32_t>& B = allCross ? dimsB : dimsA;
		for( size_t j = jStart; j < B.size(); j++ ) {
			if( dimsA[i] == B[j] ) continue;
			const unsigned int t = TValue( dimsA[i], B[j], m );
			r.pairs++;
			if( t > r.maxT ) r.maxT = t;
			if( t >= m - 1u ) r.collapsed++;
		}
	}
	return r;
}

//! Pigeonhole floor: how many of C(n,2) pairs MUST share a leading
//! generator row when n dimensions are spread over 2^(m-1) possible
//! rows, under the most balanced assignment there is.
static unsigned int CollapseFloor( unsigned int n, unsigned int m )
{
	const unsigned int buckets = 1u << ( m - 1u );
	if( n <= buckets ) return 0;
	const unsigned int q = n / buckets;
	const unsigned int rem = n % buckets;
	// `rem` buckets hold q+1, the rest hold q.
	return rem * ( ( q + 1u ) * q / 2u ) + ( buckets - rem ) * ( q * ( q - 1u ) / 2u );
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
	// C. The PRODUCTION dimension set, swept exhaustively by t-value.
	//
	// The old section C swept only stream 16 slot k against stream 17
	// slot k -- 32 same-slot pairs.  A render draws every slot of every
	// live stream against every slot of the next, so that sweep could
	// (and did) miss collapsed pairs sitting one slot off the diagonal.
	// This one sweeps:
	//
	//   C1  every ADJACENT-bounce cross-slot pair: streams (s, s+1) for
	//       s in 0..23, all 8 x 8 slot combinations -- 1536 pairs.
	//   C2  every pair inside the whole production set, streams 0..24 x
	//       slots 0..7 -- 200 dimensions, 19900 pairs.
	//   C3  the image-plane dimensions 0 and 1 against every production
	//       dimension.
	//   C4  consecutive dimensions (d, d+1) across the WHOLE table,
	//       against Joe & Kuo's own published quality claim.
	//
	// at 64, 256 and 1024 samples per pixel.
	//
	// The bounds are not "zero collapses": that is impossible, and the
	// test prints the proof beside each number.  Over the first 2^m
	// samples a dimension's leading generator row takes one of only
	// 2^(m-1) values (m_1 = 1 pins the top bit), so 200 production
	// dimensions into 32 rows at m = 6, or 128 rows at m = 8, MUST
	// collide -- `CollapseFloor` computes how often under the most
	// balanced assignment that exists.  What the bounds pin is that
	// the measured count stays near that floor, which is what the
	// searched direction numbers buy, and nowhere near the pre-DL-81
	// padding's 100%.
	// ----------------------------------------------------------------
	std::cout << std::endl << "C. Production-set t-value sweep" << std::endl;
	{
		std::vector<uint32_t> production;
		for( uint32_t st = 0; st <= 24; st++ ) {
			for( uint32_t slot = 0; slot < 8; slot++ ) production.push_back( st * 32u + slot );
		}
		std::vector<uint32_t> filmPair;
		filmPair.push_back( 0 );
		filmPair.push_back( 1 );

		// The measured post-fix counts, pinned.  Each is the Joe-Kuo
		// table's own measurement with a small regression margin; the
		// floor printed next to it is the counting bound below which
		// no direction numbers can go.
		struct Bound { unsigned int m; unsigned int adjacent; unsigned int all; unsigned int film; };
		static const Bound kBounds[3] = {
			{  6,  70,  700,  20 },		// measured 53 / 616 / 12, floor 528 on C2
			{  8,  12,  150,   4 },		// measured  6 / 131 /  2, floor  72 on C2
			{ 10,   0,   30,   0 }		// measured  0 /  23 /  0, floor   0 on C2
		};

		for( int b = 0; b < 3; b++ )
		{
			const unsigned int m = kBounds[b].m;
			char msg[256];

			// C1 -- adjacent bounces, every cross-slot combination.
			PairSweep adj; adj.pairs = 0; adj.collapsed = 0; adj.maxT = 0;
			for( uint32_t st = 0; st <= 23; st++ ) {
				std::vector<uint32_t> a, c;
				for( uint32_t slot = 0; slot < 8; slot++ ) {
					a.push_back( st * 32u + slot );
					c.push_back( ( st + 1u ) * 32u + slot );
				}
				const PairSweep one = SweepPairs( a, c, true, m );
				adj.pairs += one.pairs;
				adj.collapsed += one.collapsed;
				if( one.maxT > adj.maxT ) adj.maxT = one.maxT;
			}

			// C2 -- the whole production set.
			const PairSweep all = SweepPairs( production, production, false, m );
			// C3 -- dimensions 0 and 1 against it.
			const PairSweep film = SweepPairs( filmPair, production, true, m );

			std::cout << "    2^" << std::setw( 2 ) << m << " = " << std::setw( 5 ) << ( 1u << m )
			          << " spp:  adjacent " << std::setw( 4 ) << adj.collapsed << "/" << adj.pairs
			          << " collapsed (max t " << adj.maxT << ")"
			          << "   production " << std::setw( 4 ) << all.collapsed << "/" << all.pairs
			          << " (max t " << all.maxT << ", counting floor "
			          << CollapseFloor( unsigned( production.size() ), m ) << ")"
			          << "   dims 0/1 " << film.collapsed << "/" << film.pairs
			          << " (max t " << film.maxT << ")" << std::endl;

			std::snprintf( msg, sizeof(msg),
				"C1: adjacent-bounce cross-slot collapses <= %u at 2^%u spp",
				kBounds[b].adjacent, m );
			Check( adj.collapsed <= kBounds[b].adjacent, msg );

			std::snprintf( msg, sizeof(msg),
				"C2: production-set collapses <= %u at 2^%u spp (counting floor %u)",
				kBounds[b].all, m, CollapseFloor( unsigned( production.size() ), m ) );
			Check( all.collapsed <= kBounds[b].all, msg );

			std::snprintf( msg, sizeof(msg),
				"C3: dims 0/1 vs production collapses <= %u at 2^%u spp",
				kBounds[b].film, m );
			Check( film.collapsed <= kBounds[b].film, msg );

			// No sweep may EVER report a count above the number of
			// pairs it claims to have examined -- a cheap guard that
			// the sweep is really running.
			std::snprintf( msg, sizeof(msg),
				"C: sweeps examined the expected pair counts at 2^%u spp", m );
			Check( adj.pairs == 1536u && all.pairs == 19900u && film.pairs == 398u, msg );
		}

		// ------------------------------------------------------------
		// C4.  Joe & Kuo publish, for their `new-joe-kuo-6.21201` set,
		// the dimension at which each t-value of a two-dimensional
		// projection first occurs.  For m = 10 they report t = 10 first
		// occurring beyond dimension 21201, i.e. t <= 9 throughout; for
		// m = 12, t = 12 beyond 21201, i.e. t <= 11
		// (https://web.maths.unsw.edu.au/~fkuo/sobol/, "Comparison of
		// [2] and [1]").  Re-derive that from the table this build
		// actually expanded: it checks the embedded initial numbers,
		// the recurrence, AND the left-alignment convention all at
		// once, against a number nobody here chose.
		// ------------------------------------------------------------
		static const unsigned int kConsecutiveM[2]     = { 10, 12 };
		static const unsigned int kJoeKuoMaxT[2]       = {  9, 11 };
		for( int c = 0; c < 2; c++ )
		{
			const unsigned int m = kConsecutiveM[c];
			unsigned int maxT = 0, worstAt = 0;
			for( uint32_t d = 0; d + 1 < SobolSequence::kNumDimensions; d++ ) {
				const unsigned int t = TValue( d, d + 1u, m );
				if( t > maxT ) { maxT = t; worstAt = d; }
			}
			std::cout << "    consecutive pairs (d, d+1), d < " << SobolSequence::kNumDimensions
			          << ", m = " << m << ": max t = " << maxT
			          << " (at d = " << worstAt << "), Joe-Kuo publish <= "
			          << kJoeKuoMaxT[c] << std::endl;
			char msg[192];
			std::snprintf( msg, sizeof(msg),
				"C4: consecutive-pair t <= %u at m = %u, as Joe-Kuo's D(6) table claims",
				kJoeKuoMaxT[c], m );
			Check( maxT <= kJoeKuoMaxT[c], msg );
		}
	}

	// ----------------------------------------------------------------
	// E. Generator self-checks.
	//
	// The direction numbers are EXPANDED at first use from the embedded
	// Joe-Kuo initial numbers, so both halves need pinning.
	//   E1a The embedded records are admissible (m_i odd, m_i < 2^i),
	//       genuinely primitive, and in the canonical Sobol' order.
	//       Primitivity is decided here by walking the powers of x
	//       until they return to 1 -- no code shared with anything the
	//       library or the generator does.
	//   E1b Every row of the LIBRARY's table equals the recurrence
	//       expanded here, from those records.  This is what makes the
	//       "RISE uses Joe & Kuo's searched initial numbers" claim
	//       checkable rather than a comment.
	//   E1c Per-degree counts of the embedded polynomials equal
	//       phi(2^d - 1)/d, from an independent Euler-phi.
	//   E2  Table rows 0 and 1 must still BE the closed-form van der
	//       Corput and x+1 sequences, so the image plane is untouched.
	//       Read through `DirectionNumber`, not `Sobol`: `Sobol`
	//       short-circuits dimensions 0 and 1 to those same closed
	//       forms, so checking it against them is a tautology that
	//       passes whatever the table holds.
	// ----------------------------------------------------------------
	std::cout << std::endl << "E. Generator self-checks" << std::endl;
	{
		const uint32_t* rec = SobolJoeKuoInitialNumbers();
		const unsigned int avail = SobolJoeKuoDimensionCount();
		const unsigned int total = SobolJoeKuoRecordCount();

		Check( avail >= SobolSequence::kNumDimensions,
			"E1a: the embedded records cover every dimension the table claims" );

		bool admissible = true, primitive = true, canonical = true, expansion = true;
		unsigned int perDegree[33] = {0};
		uint32_t expectDeg = 1, expectA = 0;
		unsigned int consumed = 0;
		unsigned int firstBadDim = 0;

		const unsigned int sweep =
			( avail < SobolSequence::kNumDimensions ) ? avail : SobolSequence::kNumDimensions;
		for( unsigned int dim = 1; dim < sweep; dim++ )
		{
			const unsigned int s = rec[consumed];
			const uint32_t a     = rec[consumed + 1];
			const uint32_t* m    = rec + consumed + 2;
			consumed += 2 + s;

			if( s == 0 || s > 32 ) { admissible = false; break; }
			for( unsigned int i = 1; i <= s; i++ ) {
				if( ( m[i - 1] & 1u ) == 0u || m[i - 1] >= ( 1u << i ) ) admissible = false;
			}

			const uint32_t p = ( 1u << s ) | ( a << 1 ) | 1u;
			if( !PolyOrderIsFull( p, s ) ) primitive = false;
			if( s <= 32 ) perDegree[s]++;

			// Canonical order: advance our own (degree, a) cursor past
			// every non-primitive a and require this record to name
			// exactly the next primitive polynomial.
			for( ;; ) {
				if( expectA >= ( 1u << ( expectDeg - 1u ) ) ) { expectDeg++; expectA = 0; continue; }
				const uint32_t q = ( 1u << expectDeg ) | ( expectA << 1 ) | 1u;
				if( PolyOrderIsFull( q, expectDeg ) ) break;
				expectA++;
			}
			if( s != expectDeg || a != expectA ) canonical = false;
			expectA++;

			// E1b -- expand the recurrence here and compare against the
			// library's table.
			uint32_t w[32];
			for( unsigned int i = 1; i <= s; i++ ) w[i - 1] = m[i - 1] << ( 32u - i );
			for( unsigned int i = s; i < 32; i++ ) {
				uint32_t t = w[i - s] ^ ( w[i - s] >> s );
				for( unsigned int k = 1; k < s; k++ ) {
					if( ( p >> ( s - k ) ) & 1u ) t ^= w[i - k];
				}
				w[i] = t;
			}
			for( unsigned int i = 0; i < 32; i++ ) {
				if( SobolSequence::DirectionNumber( dim, i ) != w[i] ) {
					if( expansion ) firstBadDim = dim;
					expansion = false;
					break;
				}
			}
		}

		Check( admissible, "E1a: every embedded m_i is odd and below 2^i" );
		Check( primitive,  "E1a: every embedded (s, a) is a primitive polynomial over GF(2)" );
		Check( canonical,  "E1a: the embedded polynomials are in canonical Sobol' order" );
		if( !expansion ) {
			std::cout << "    first dimension whose table row is NOT the Joe-Kuo expansion: "
			          << firstBadDim << std::endl;
		}
		Check( expansion,
			"E1b: every table row is the recurrence expanded from the embedded Joe-Kuo record" );
		Check( consumed <= total,
			"E1a: the record walk stayed inside the embedded array" );

		bool countsOk = true;
		for( unsigned int d = 1; d <= 32; d++ ) {
			if( perDegree[d] == 0 ) continue;
			const uint32_t n = ( 1u << d ) - 1u;
			const unsigned int expect = EulerPhi( n ) / d;
			const bool complete = ( perDegree[d] == expect );
			std::cout << "    degree " << std::setw( 2 ) << d << ": "
			          << std::setw( 5 ) << perDegree[d] << " embedded,  phi(2^d-1)/d = "
			          << expect << ( complete ? "" : "  (truncated by kNumDimensions)" )
			          << std::endl;
			// Only the degrees that ran to completion can be compared;
			// the last one is cut off by the dimension cap.
			if( perDegree[d] > expect ) countsOk = false;
		}
		Check( countsOk, "E1c: per-degree counts never exceed phi(2^d-1)/d" );

		bool d0 = true, d1 = true;
		uint32_t dir1 = 1u << 31;
		for( uint32_t i = 0; i < 32; i++ ) {
			if( SobolSequence::DirectionNumber( 0, i ) != ( 1u << ( 31u - i ) ) ) d0 = false;
			if( SobolSequence::DirectionNumber( 1, i ) != dir1 ) d1 = false;
			dir1 ^= ( dir1 >> 1 );
		}
		Check( d0, "E2: TABLE row 0 is the van der Corput direction numbers 2^31, 2^30, ..." );
		Check( d1, "E2: TABLE row 1 is the x+1 recurrence SobolDim1 implements" );

		bool s0 = true, s1 = true;
		for( uint32_t i = 0; i < ( 1u << 16 ); i++ ) {
			if( SobolSequence::Sobol( i, 0 ) != SobolSequence::SobolDim0( i ) ) s0 = false;
			if( SobolSequence::Sobol( i, 1 ) != SobolSequence::SobolDim1( i ) ) s1 = false;
		}
		Check( s0 && s1,
			"E2: Sobol()'s dimension 0/1 short-circuits agree with their closed forms" );

		// E3.  Round-2-review note (P3-1): at dimension 0 ONLY,
		// SamplePair(i,0,seed).outU coincides bit-for-bit with
		// Sample(i,0,seed), and .outV with Sample(i,1,seed) -- group 0
		// draws at the unpermuted index with the same per-dimension
		// Owen seed Sample would use.  This is deliberately left AS IS
		// (see SamplePair's header comment): fixing it would retag
		// group 0's own scramble seed and break the "bit-identical to
		// every version of this class" guarantee that group's own
		// comment documents.  It is dormant -- no production code
		// draws both Sample(*,0,*) and SamplePair(*,0,*) against the
		// same seed -- but pinned here so a future change to either
		// function makes a conscious choice about it.
		bool e3 = true;
		for( uint32_t i = 0; i < ( 1u << 12 ); i++ ) {
			double u, v;
			SobolSequence::SamplePair( i, 0u, seed, u, v );
			if( SobolSequence::Sample( i, 0u, seed ) != u ) e3 = false;
			if( SobolSequence::Sample( i, 1u, seed ) != v ) e3 = false;
		}
		Check( e3,
			"E3: SamplePair(*,0,seed) coincides with Sample(*,{0,1},seed) -- documented, not fixed" );
	}

	// ----------------------------------------------------------------
	// F. Get2D must be a (0,2)-net -- at EVERY dimension group.
	//
	// Under the original padding, every Get2D drew Sobol' dimensions 0
	// and 1, which is a perfect (0,2)-net; the DL-81 fix quietly traded
	// that for two consecutive rows of the per-dimension table, whose
	// joint projection is a net only by accident.  Get2D is now padded
	// again -- dimensions 0 and 1 at an index permuted per group -- so
	// every 2D draw a renderer makes (film position, lens point,
	// hemisphere direction, light-surface point) is a (0,m,2)-net for
	// every m at once, and different groups are decorrelated.
	//
	// Drawn through the real SobolSampler, so this tests what an
	// integrator gets, not a private helper.
	// ----------------------------------------------------------------
	std::cout << std::endl << "F. Get2D (0,2)-net property, by dimension group" << std::endl;
	{
		// (stream, slots consumed before the Get2D).  Stream 0 slot 0 is
		// the film pair; 3322 is the thin-lens aperture stream, whose
		// pair the DL-81 fix had landed on table dimensions 2309/2310.
		struct Group { int stream; int skip; const char* what; };
		static const Group kGroups[8] = {
			{    0, 0, "film / primary        " },
			{    1, 0, "light bounce 0 slot 0 " },
			{   16, 0, "eye bounce 0 slot 0   " },
			{   16, 1, "eye bounce 0 slot 1   " },
			{   17, 3, "eye bounce 1 slot 3   " },
			{   24, 6, "eye bounce 8 slot 6   " },
			{   47, 0, "BDPT strategy select  " },
			{ 3322, 0, "thin-lens aperture    " }
		};

		for( int g = 0; g < 8; g++ )
		{
			unsigned int worstM = 0;
			bool allOk = true;
			for( unsigned int m = 2; m <= 12 && allOk; m++ )
			{
				const uint32_t n = 1u << m;
				std::vector<double> xs( n ), ys( n );
				for( uint32_t i = 0; i < n; i++ ) {
					SobolSampler sampler( i, seed );
					sampler.StartStream( kGroups[g].stream );
					for( int d = 0; d < kGroups[g].skip; d++ ) sampler.Get1D();
					const Point2 pt = sampler.Get2D();
					xs[i] = pt.x;
					ys[i] = pt.y;
				}
				for( unsigned int k = 0; k <= m && allOk; k++ ) {
					const uint32_t nx = 1u << k;
					const uint32_t ny = 1u << ( m - k );
					std::vector<uint32_t> counts( size_t(nx) * size_t(ny), 0u );
					for( uint32_t i = 0; i < n; i++ ) {
						uint32_t bx = uint32_t( xs[i] * double(nx) ); if( bx >= nx ) bx = nx - 1;
						uint32_t by = uint32_t( ys[i] * double(ny) ); if( by >= ny ) by = ny - 1;
						counts[ size_t(by) * size_t(nx) + size_t(bx) ]++;
					}
					for( size_t b = 0; b < counts.size(); b++ ) {
						if( counts[b] != 1u ) { allOk = false; break; }
					}
				}
				if( allOk ) worstM = m;
			}
			std::cout << "    " << kGroups[g].what
			          << "  (0,m,2)-net up to m = " << worstM
			          << ( worstM >= 12 ? "" : "   <-- FAILS" ) << std::endl;
			char msg[192];
			std::snprintf( msg, sizeof(msg),
				"F: Get2D on stream %d slot %d is a (0,m,2)-net for every m <= 12",
				kGroups[g].stream, kGroups[g].skip );
			Check( worstM >= 12, msg );
		}

		// Cross-group decorrelation: one coordinate of one group against
		// the same coordinate of another must fill the dyadic 2x2 boxes.
		// A shared index permutation -- or none at all -- would leave
		// them locked exactly as the pre-DL-81 padding did.
		const uint32_t nCross = 1u << 16;
		double worstCross = 0.0;
		for( int g = 1; g < 8; g++ )
		{
			uint32_t d4[4] = {0,0,0,0};
			for( uint32_t i = 0; i < nCross; i++ ) {
				SobolSampler sa( i, seed );
				sa.StartStream( kGroups[g - 1].stream );
				for( int d = 0; d < kGroups[g - 1].skip; d++ ) sa.Get1D();
				const Point2 pa = sa.Get2D();

				SobolSampler sb( i, seed );
				sb.StartStream( kGroups[g].stream );
				for( int d = 0; d < kGroups[g].skip; d++ ) sb.Get1D();
				const Point2 pb = sb.Get2D();

				d4[ ( pa.x < 0.5 ? 0 : 1 ) + ( pb.x < 0.5 ? 0 : 2 ) ]++;
			}
			double worst = 0.0;
			for( int b = 0; b < 4; b++ ) {
				worst = std::max( worst,
					std::fabs( ( double( d4[b] ) / double( nCross ) ) * 4.0 - 1.0 ) );
			}
			worstCross = std::max( worstCross, worst );
		}
		std::cout << "    worst cross-group dyadic 2x2 deviation over 2^16 samples: "
		          << std::fixed << std::setprecision( 5 ) << worstCross << std::endl;
		Check( worstCross <= 0.02,
			"F: consecutive Get2D groups fill all four dyadic 2x2 boxes within 2%" );
	}

	// ----------------------------------------------------------------
	// G. Get2D's OWN index-scramble collapse, at low spp, across the
	//    PRODUCTION Get2D group set (round-2 review, P2-1).
	//
	// This is a DIFFERENT collapse from section C's: C is about two
	// Get1D DIMENSIONS sharing a leading generator-matrix row; this is
	// about two Get2D GROUPS' `ScrambleIndex` calls sharing the same
	// Boolean function of the index's upper bits, so their leading
	// (coarsest) digit is locked together for every sample below 2^M,
	// in every pixel.  See SobolSequence.h's "Get2D's index-scramble
	// collapse at low spp" for the derivation: at 2^M samples there are
	// only 2^(2^(M-1)) possible such functions, so this is ALSO a
	// counting floor, not a fixable defect -- the test pins the
	// measured count near that floor rather than asserting "zero".
	//
	// Production Get2D group set: streams 0..24 x slots 0..7 (200
	// groups a real render's Get2D calls can land on) plus the BDPT
	// strategy-select group (stream 47 slot 0) = 201 groups, matching
	// section C's production DIMENSION set in spirit.  For each pair,
	// all four (u,u)/(u,v)/(v,u)/(v,v) coordinate combinations are
	// checked -- 201*200/2*4 = 80400 combinations per M.
	// ----------------------------------------------------------------
	std::cout << std::endl << "G. Get2D index-scramble collapse, production group set" << std::endl;
	{
		std::vector<uint32_t> groups;
		for( uint32_t st = 0; st <= 24; st++ )
			for( uint32_t slot = 0; slot < 8; slot++ ) groups.push_back( st * 32u + slot );
		groups.push_back( 47u * 32u + 0u );

		// Lower bound on the "leading digit forced equal" floor: the
		// simple pigeonhole count for an exact-match family of size
		// 2^(2^(m-1)), same construction as `CollapseFloor` above but
		// with astronomically larger buckets.  This under-counts the
		// true floor (it ignores the exact-COMPLEMENT pairing, which
		// also locks the digit, just with the opposite sign), so the
		// measured/asserted numbers below sit ABOVE it, same as
		// section C's floor column.
		struct GFloor {
			static unsigned long long Of( unsigned int n, unsigned int m ) {
				const unsigned int exp = 1u << ( m - 1u );
				if( exp >= 26u ) return 0ull;		// family >> any n we test
				const unsigned long long F = 1ull << exp;
				if( (unsigned long long)n <= F ) return 0ull;
				const unsigned long long q = n / F, rem = n % F;
				return rem * ( ( q + 1ull ) * q / 2ull ) + ( F - rem ) * ( q * ( q - 1ull ) / 2ull );
			}
		};

		// Measured on this table (seed-independent -- ScrambleIndex
		// never reads the pixel seed, only the group id -- so the
		// counts below are exact, not sampled noise).  Small headroom
		// kept over the measured value, same convention as section C.
		struct GBound { unsigned int m; unsigned long long maxCollapsed; };
		static const GBound kGBounds[3] = {
			{ 2, 40300 },		// measured 40200/80400 = 50.000% (match-floor 2 -> essentially all)
			{ 4, 10100 },		// measured  9970/80400 = 12.400%
			{ 6,   700 }		// measured   624/80400 =  0.776%
		};

		for( int b = 0; b < 3; b++ )
		{
			const unsigned int m = kGBounds[b].m;
			const uint32_t n = 1u << m;
			unsigned long long pairs = 0, collapsed = 0;

			for( size_t i = 0; i < groups.size(); i++ ) {
				for( size_t j = i + 1; j < groups.size(); j++ ) {
					unsigned int box[2][2][4];
					for( int a = 0; a < 2; a++ ) for( int c = 0; c < 2; c++ )
						for( int k = 0; k < 4; k++ ) box[a][c][k] = 0;
					for( uint32_t s = 0; s < n; s++ ) {
						double ua, va, ub, vb;
						SobolSequence::SamplePair( s, groups[i], seed, ua, va );
						SobolSequence::SamplePair( s, groups[j], seed, ub, vb );
						const double A[2] = { ua, va };
						const double B[2] = { ub, vb };
						int combo = 0;
						for( int ca = 0; ca < 2; ca++ ) for( int cb = 0; cb < 2; cb++ ) {
							box[ A[ca] < 0.5 ? 0 : 1 ][ B[cb] < 0.5 ? 0 : 1 ][combo]++;
							combo++;
						}
					}
					for( int c = 0; c < 4; c++ ) {
						unsigned int empty = 0;
						for( int a = 0; a < 2; a++ ) for( int bb = 0; bb < 2; bb++ )
							if( box[a][bb][c] == 0u ) empty++;
						pairs++;
						if( empty >= 2u ) collapsed++;
					}
				}
			}

			std::cout << "    2^" << std::setw( 2 ) << m << " = " << std::setw( 4 ) << n
			          << " spp:  " << std::setw( 5 ) << collapsed << "/" << pairs
			          << " (" << std::fixed << std::setprecision( 3 )
			          << ( 100.0 * double( collapsed ) / double( pairs ) )
			          << "%), pigeonhole match-floor "
			          << GFloor::Of( unsigned( groups.size() ), m ) << std::endl;

			char msg[192];
			std::snprintf( msg, sizeof(msg),
				"G: Get2D production-group collapse <= %llu at 2^%u spp",
				kGBounds[b].maxCollapsed, m );
			Check( collapsed <= kGBounds[b].maxCollapsed, msg );

			std::snprintf( msg, sizeof(msg),
				"G: sweep at 2^%u spp examined all 80400 combinations", m );
			Check( pairs == 80400ull, msg );
		}
	}


	// ----------------------------------------------------------------
	// H. The wrap-region stream families (DL-283).
	//
	// Collapse statistic exactly as section G: over the first 2^M
	// samples, the dyadic 2x2 occupancy of (draw A >= 1/2, draw B >= 1/2);
	// "collapsed" = two or more empty boxes, i.e. the two leading digits
	// locked together.  Four value seeds; Get1D path (SobolSequence::
	// Sample on the RAW dimension, which is what SobolSampler::Get1D
	// calls).  Families of pairs:
	//   PTa   PT walk (lane 0..3, event 0..255, slot 0..7) vs the
	//         unwrapped dimension on the same table row;
	//   MDa   BDPT medium block (side 0..1, iteration 0..63, draw
	//         0..63) vs the unwrapped dimension on the same table row;
	//   MDv   the same block draws vs the iteration's own vertex stream
	//         (BDPTUtilities::EyeWalkStream / LightWalkStream -- 16 + d
	//         eye / 1 + d light for shallow iterations, the DL-286 deep
	//         block past that; slots 0..7);
	//   DWa   DL-286's deep walk streams (light iterations 15..78, eye
	//         31..94, slots 0..7) vs the unwrapped dimension on the same
	//         table row -- the shallow stream the same sample also opens;
	//   MDx   eye block d vs light block d, draw k -- same table row,
	//         different wrap counts;
	//   floor ordinary adjacent production streams (s, s+1), s 16..40,
	//         8 x 8 slots -- what a render already lives with.
	// ----------------------------------------------------------------
	std::cout << std::endl << "H. Wrap-region stream families: leading-digit collapse (DL-283)" << std::endl;
	{
		const uint32_t table = SobolSequence::kNumDimensions;
		const uint32_t stride = SobolSampler::kStreamStride;
		const uint32_t seeds[4] = { 0x5eed1234u, 0x0badf00du, 0x283283u, 0x9e3779b9u };
		typedef std::vector< std::pair<uint32_t, uint32_t> > PairList;
		PairList pta, mda, mdv, mdx, dwa, flo;
		for( uint32_t lane = 0; lane < 4; lane++ )
			for( uint32_t ev = 0; ev < 256; ev++ )
				for( uint32_t slot = 0; slot < 8; slot++ ) {
					const uint32_t d = uint32_t( PathTransportUtilities::PTVolumeWalkStream( lane, ev ) ) * stride + slot;
					pta.push_back( std::make_pair( d, d % table ) );
				}
		for( uint32_t side = 0; side < 2; side++ )
			for( uint32_t it = 0; it < 64; it++ ) {
				const uint32_t b = uint32_t( BDPTUtilities::MediumDistanceStream(
					side ? BDPTUtilities::eLightWalk : BDPTUtilities::eEyeWalk, it ) ) * stride;
				const uint32_t v = uint32_t( side ? BDPTUtilities::LightWalkStream( it, true )
				                                  : BDPTUtilities::EyeWalkStream( it, true ) ) * stride;
				for( uint32_t k = 0; k < 64; k++ ) {
					mda.push_back( std::make_pair( b + k, ( b + k ) % table ) );
					mdv.push_back( std::make_pair( b + k, v + ( k % 8u ) ) );
					if( side == 0 ) {
						const uint32_t bl = uint32_t( BDPTUtilities::MediumDistanceStream(
							BDPTUtilities::eLightWalk, it ) ) * stride;
						mdx.push_back( std::make_pair( b + k, bl + k ) );
					}
				}
			}
		for( uint32_t side = 0; side < 2; side++ )
			for( uint32_t it = 0; it < 64; it++ ) {
				const uint32_t d = side ? BDPTUtilities::kLightWalkShallowIterations + it
				                        : BDPTUtilities::kEyeWalkShallowIterations + it;
				const uint32_t w = uint32_t( side ? BDPTUtilities::LightWalkStream( d, true )
				                                  : BDPTUtilities::EyeWalkStream( d, true ) ) * stride;
				for( uint32_t slot = 0; slot < 8; slot++ )
					dwa.push_back( std::make_pair( w + slot, ( w + slot ) % table ) );
			}
		for( uint32_t st = 16; st <= 40; st++ )
			for( uint32_t a = 0; a < 8; a++ )
				for( uint32_t c = 0; c < 8; c++ )
					flo.push_back( std::make_pair( st * stride + a, ( st + 1u ) * stride + c ) );

		// Sanity: every MDa / PTa partner really is a different raw
		// dimension on the same row (the premise of the family).
		bool rowPremise = true;
		for( const auto& pr : pta ) if( pr.first == pr.second || pr.first % table != pr.second ) rowPremise = false;
		for( const auto& pr : mda ) if( pr.first == pr.second || pr.first % table != pr.second ) rowPremise = false;
		for( const auto& pr : dwa ) if( pr.first == pr.second || pr.first % table != pr.second ) rowPremise = false;
		for( const auto& pr : mdx ) if( pr.first % table != pr.second % table || pr.first == pr.second ) rowPremise = false;
		Check( rowPremise, "H: every aliased pair shares a table row at distinct raw dimensions" );

		auto rate = [&]( const PairList& L, unsigned int m ) {
			const uint32_t n = 1u << m;
			unsigned long long pairs = 0, collapsed = 0;
			std::vector<unsigned char> a( n ), b( n );
			for( uint32_t sd = 0; sd < 4; sd++ ) {
				for( const auto& pr : L ) {
					unsigned int box[2][2] = { { 0, 0 }, { 0, 0 } };
					for( uint32_t i = 0; i < n; i++ ) {
						const int x = SobolSequence::Sample( i, pr.first,  seeds[sd] ) >= 0.5 ? 1 : 0;
						const int y = SobolSequence::Sample( i, pr.second, seeds[sd] ) >= 0.5 ? 1 : 0;
						box[x][y]++;
					}
					unsigned int empty = 0;
					for( int x = 0; x < 2; x++ ) for( int y = 0; y < 2; y++ ) if( box[x][y] == 0u ) empty++;
					pairs++;
					if( empty >= 2u ) collapsed++;
				}
			}
			return 100.0 * double( collapsed ) / double( pairs );
		};

		std::cout << "    pairs x 4 seeds: PTa " << pta.size() << ", MDa " << mda.size()
		          << ", MDv " << mdv.size() << ", MDx " << mdx.size() << ", DWa " << dwa.size()
		          << ", floor " << flo.size() << std::endl;
		for( unsigned int m = 2; m <= 8; m++ ) {
			const double rPTa = rate( pta, m ), rMDa = rate( mda, m ), rMDv = rate( mdv, m ),
				rMDx = rate( mdx, m ), rDWa = rate( dwa, m ), rFlo = rate( flo, m );
			std::cout << "    " << std::setw( 3 ) << ( 1u << m ) << " spp: PTa " << std::fixed
			          << std::setprecision( 2 ) << std::setw( 6 ) << rPTa << "%  MDa " << std::setw( 6 ) << rMDa
			          << "%  MDv " << std::setw( 6 ) << rMDv << "%  MDx " << std::setw( 6 ) << rMDx
			          << "%  DWa " << std::setw( 6 ) << rDWa
			          << "%   floor " << std::setw( 6 ) << rFlo << "%" << std::endl;
			// Measured (this table): at 8 spp PTa 0.00, MDa 6.25, MDv 13.84,
			// MDx 25.00 against a floor of 24.69 -- MDx sits AT the
			// counting floor (two index permutations of one row; the same
			// pigeonhole as section G), not above it by more than noise.
			// From 16 spp every family is exactly 0 while the floor is
			// still 12.6 %.  Gate: at 8 spp within 1 point of the floor;
			// from 16 spp exactly zero -- except MDv, since DL-286 moved
			// the deep iterations' VERTEX streams into the wrap region
			// too: a block draw and a deep vertex slot are then two
			// index-permuted (wrap-keyed) rows, and at 2^M samples each
			// one's leading digit is one of a finite family of affine
			// functions of M index bits (section G's pigeonhole), so a
			// few specific wrap pairs lock by chance.  Measured: 23 of
			// 8192 MDv pairs (0.28 %, identically on all four seeds --
			// Owen scrambling flips a locked pair's digit together) at
			// 16 spp, all light iterations 52..55 (block wrap 301 vs
			// vertex wrap 544); 0 from 32 spp.  MDv's gate from 16 spp is
			// therefore a tenth of the production floor, not zero.
			char msg[200];
			if( m == 3 ) {
				std::snprintf( msg, sizeof(msg),
					"H: at 8 spp every wrap-region family is within 1 point of the production floor "
					"(PTa %.2f MDa %.2f MDv %.2f MDx %.2f DWa %.2f vs %.2f)", rPTa, rMDa, rMDv, rMDx, rDWa, rFlo );
				Check( rPTa <= rFlo + 1.0 && rMDa <= rFlo + 1.0 && rMDv <= rFlo + 1.0 && rMDx <= rFlo + 1.0 &&
					rDWa <= rFlo + 1.0, msg );
			} else if( m >= 4 ) {
				std::snprintf( msg, sizeof(msg),
					"H: at %u spp no wrap-region pair collapses (PTa %.2f MDa %.2f MDx %.2f DWa %.2f) "
					"and MDv %.2f <= a tenth of the floor %.2f",
					1u << m, rPTa, rMDa, rMDx, rDWa, rMDv, rFlo );
				Check( rPTa == 0.0 && rMDa == 0.0 && rMDv <= 0.1 * rFlo && rMDx == 0.0 && rDWa == 0.0, msg );
			}
		}
	}

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
