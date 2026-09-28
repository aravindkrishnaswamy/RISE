//////////////////////////////////////////////////////////////////////
//
//  SchlickMaskingBoundTest.cpp - DL-225: the bounded Schlick masking
//    helper (src/Library/Materials/SchlickMasking.h) against an
//    INDEPENDENT numerical Smith masking of Schlick's own distribution.
//
//  The energy bound of the DL-225 model rests on one inequality:
//
//      m(v) <= nv / I(v),   I(v) = int max(0, v.m) Z(m) A(m) / pi dm,
//
//  i.e. the production masking never exceeds the exact Smith G1 of
//  Schlick's Z*A distribution.  The production helper evaluates a
//  closed-form UPPER bound of I (elliptic constants by AGM, a Jensen
//  bound and a rearrangement/Cauchy-Schwarz bound).  This test computes
//  I itself by quadrature that shares nothing with those closed forms:
//
//    1. c(p) and C2(p) (the AGM constants) against graded
//       Gauss-Legendre quadrature of A.
//    2. The semi-analytic reduction I/nv = c(p) + (k/2pi) E(phi_v) that
//       the whole bound family starts from, against a brute-force 2-D
//       quadrature of the projected area.
//    3. THE GATE: m_prod <= G1_exact (1 + 1e-7) over a
//       (roughness, isotropy incl. > 1, cos, view azimuth) grid.
//    4. Isotropy 1: m_prod == min(Eq.31, closed-form GGX Smith G1).
//    5. Reduction: m_prod == Eq.31 wherever Eq.31 <= the bound.
//    6. Isotropy p > 1 == isotropy 1/p rotated a quarter turn.
//    7. Continuity at isotropy -> 1.
//    8. Tightness (reported and bounded): G1_exact / m_prod where the
//       bound binds.
//    9. The cFast shortcut never skips a state the full evaluation
//       would bound.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>
#include "../src/Library/Materials/SchlickMasking.h"

using namespace RISE::Implementation;

static int checks = 0, failures = 0;
static void Check( bool ok, const char* label, double actual, double expected )
{
	++checks;
	if( !ok ) {
		++failures;
		printf( "FAIL %s: %.15g expected %.15g\n", label, actual, expected );
	}
}

static const double kPi = 3.14159265358979323846;

static double ARaw( double phi, double p )
{
	const double c = cos( phi ), s = sin( phi );
	return sqrt( p / ( p*p*c*c + s*s ) );
}

//! 16-point Gauss-Legendre on [a,b].
template< class F >
static double GL16( F f, double a, double b )
{
	static const double x[8] = { 0.0950125098376374, 0.2816035507792589, 0.4580167776572274, 0.6178762444026438,
	                             0.7554044083550030, 0.8656312023878318, 0.9445750230732326, 0.9894009349916499 };
	static const double w[8] = { 0.1894506104550685, 0.1826034150449236, 0.1691565193950025, 0.1495959888165767,
	                             0.1246289712555339, 0.0951585116824928, 0.0622535239386479, 0.0271524594117541 };
	const double m = 0.5 * ( a + b ), h = 0.5 * ( b - a );
	double s = 0;
	for( int i = 0; i < 8; i++ ) {
		s += w[i] * ( f( m - h * x[i] ) + f( m + h * x[i] ) );
	}
	return s * h;
}

//! Integrate on [a,b] with panels geometrically graded toward every
//! point in `peaks` (A's peaks sit at multiples of pi, width ~ p).
template< class F >
static double GradedIntegral( F f, double a, double b, const std::vector<double>& peaks, double width )
{
	std::vector<double> cuts;
	cuts.push_back( a ); cuts.push_back( b );
	for( double pk : peaks ) {
		if( pk > a && pk < b ) cuts.push_back( pk );
		for( double d = width * 1e-3; d < ( b - a ); d *= 1.6 ) {
			if( pk - d > a && pk - d < b ) cuts.push_back( pk - d );
			if( pk + d > a && pk + d < b ) cuts.push_back( pk + d );
		}
	}
	std::sort( cuts.begin(), cuts.end() );
	double sum = 0;
	for( size_t i = 0; i + 1 < cuts.size(); i++ ) {
		const double lo = cuts[i], hi = cuts[i+1];
		if( hi <= lo ) continue;
		// Subdivide long panels uniformly.
		const int n = std::max( 1, int( ( hi - lo ) / 0.05 ) );
		for( int j = 0; j < n; j++ ) {
			sum += GL16( f, lo + ( hi - lo ) * j / n, lo + ( hi - lo ) * ( j + 1 ) / n );
		}
	}
	return sum;
}

//! A's peaks, for an isotropy that may exceed 1 (then A peaks at pi/2 + j pi).
static std::vector<double> Peaks( double p )
{
	std::vector<double> pk;
	const double off = ( p > 1 ) ? 0.5 * kPi : 0.0;
	for( int j = -3; j <= 4; j++ ) pk.push_back( off + j * kPi );
	return pk;
}

static double Width( double p ) { return ( p > 1 ) ? 1.0 / p : p; }

static double CpNumeric( double p )
{
	return GradedIntegral( [p]( double ph ) { return ARaw( ph, p ); }, 0, 2 * kPi, Peaks( p ), Width( p ) ) / ( 2 * kPi );
}

static double C2Numeric( double p )
{
	return GradedIntegral( [p]( double ph ) { return ARaw( ph, p ) * cos( 2 * ph ); }, 0, kPi, Peaks( p ), Width( p ) );
}

//! E(phi_v) = int_{-pi/2}^{pi/2} A(phi_v + x) cos x atan(k cos x) dx.
static double ENumeric( double phiv, double k, double p )
{
	std::vector<double> pk;
	for( double q : Peaks( p ) ) pk.push_back( q - phiv );
	return GradedIntegral( [&]( double x ) { return ARaw( phiv + x, p ) * cos( x ) * atan( k * cos( x ) ); },
		-0.5 * kPi, 0.5 * kPi, pk, Width( p ) );
}

//! Exact Smith G1 of Schlick's Z*A/pi at (c, phi_v).
static double G1Exact( double c, double phiv, double r, double p )
{
	const double s = sqrt( 1 - c*c );
	const double k = sqrt( r ) * s / c;
	const double onePlusLambda = CpNumeric( p ) + k / ( 2 * kPi ) * ENumeric( phiv, k, p );
	return 1.0 / onePlusLambda;
}

static double Eq31( double c, double r ) { return c / ( r + ( 1 - r ) * c ); }

static double MProd( double c, double phiv, double r, double p )
{
	SchlickMasking::Lane L;
	SchlickMasking::Prepare( L, r, p );
	return c * SchlickMasking::MaskOverCos( L, c, cos( phiv ), sin( phiv ) );
}

int main()
{
	// 1. Elliptic constants.
	double worstC = 0;
	for( double p : { 1e-3, 0.01, 0.1, 0.3, 0.7, 0.99, 1.0 } ) {
		SchlickMasking::Lane L;
		SchlickMasking::Prepare( L, 0.1, p );
		const double cpN = CpNumeric( p ), c2N = C2Numeric( p );
		worstC = std::max( worstC, std::max( fabs( L.cp - cpN ), fabs( L.C2 - c2N ) ) );
		Check( fabs( L.cp - cpN ) < 1e-9, "c(p) = (1/2pi) int A dphi (AGM vs quadrature)", L.cp, cpN );
		Check( fabs( L.C2 - c2N ) < 1e-9, "C2(p) = int_0^pi A cos 2phi (AGM series vs quadrature)", L.C2, c2N );
	}
	printf( "1. elliptic constants: worst |AGM - quadrature| = %.3e\n", worstC );

	// 2. The semi-analytic reduction against brute-force 2-D projected area.
	{
		struct V { double r, p, thDeg, phDeg; };
		const V cases[] = { { 0.05, 0.3, 80, 40 }, { 0.2, 1.0, 70, 0 }, { 0.02, 3.0, 85, 20 }, { 0.5, 0.1, 60, 90 } };
		double worst = 0;
		for( const V& q : cases ) {
			const double th = q.thDeg * kPi / 180, ph = q.phDeg * kPi / 180;
			const double vx = sin( th ) * cos( ph ), vy = sin( th ) * sin( ph ), vz = cos( th );
			// int max(0, v.m) Z A / pi dm with the sampler-free warp
			// tan(theta_m) = sqrt(r) tan(pi u / 2), graded in azimuth.
			const int NU = 1500;
			double I = 0;
			const double sr = sqrt( q.r );
			for( int i = 0; i < NU; i++ ) {
				const double a = 0.5 * kPi * ( i + 0.5 ) / NU;
				const double tt = sr * tan( a );
				const double tm = atan( tt );
				const double dth = 0.5 * kPi * sr / ( cos( a ) * cos( a ) ) / ( 1 + tt * tt ) / NU;
				const double ct = cos( tm ), st = sin( tm );
				const double zd = 1 - ( 1 - q.r ) * ct * ct;
				const double Z = q.r / ( zd * zd );
				I += GradedIntegral( [&]( double phm ) {
					const double vm = st * cos( phm ) * vx + st * sin( phm ) * vy + ct * vz;
					return vm > 0 ? vm * ARaw( phm, q.p ) : 0.0; },
					0, 2 * kPi, Peaks( q.p ), Width( q.p ) ) * Z / kPi * st * dth;
			}
			const double semi = 1.0 / G1Exact( vz, ph, q.r, q.p );
			const double rel = fabs( I / vz - semi ) / semi;
			worst = std::max( worst, rel );
			Check( rel < 2e-4, "I(v)/nv brute force == c(p) + (k/2pi) E(phi_v)", I / vz, semi );
		}
		printf( "2. semi-analytic projected area vs brute force: worst relative %.3e\n", worst );
	}

	// 3. THE GATE, and 8. tightness where it binds.
	{
		const double rs[] = { 0.001, 0.005, 0.02, 0.1, 0.3, 0.6, 1.0 };
		const double ps[] = { 0.001, 0.01, 0.1, 0.3, 0.7, 1.0, 1.5, 10.0, 100.0 };
		const double cs[] = { 1e-6, 1e-3, 0.01, 0.05, 0.1, 0.2, 0.4, 0.7, 0.95 };
		const double phs[] = { 0, 15, 45, 75, 90, 135, 200 };
		double worstExcess = -1, worstSlack = 1;
		int n = 0, binding = 0;
		for( double p : ps ) {
			// E does not depend on r; cache it per (c-independent) k below.
			for( double r : rs ) {
				for( double c : cs ) {
					for( double phd : phs ) {
						const double ph = phd * kPi / 180;
						const double mp = MProd( c, ph, r, p );
						const double ge = G1Exact( c, ph, r, p );
						const double excess = mp / ge - 1;
						worstExcess = std::max( worstExcess, excess );
						Check( mp <= ge * ( 1 + 1e-7 ), "m_prod <= exact Smith G1 of Z*A (the energy bound)", mp, ge );
						n++;
						const double e31 = Eq31( c, r );
						if( mp < e31 * ( 1 - 1e-12 ) ) {
							binding++;
							worstSlack = std::min( worstSlack, mp / ge );
						}
					}
				}
			}
		}
		printf( "3. %d states: worst m_prod/G1_exact - 1 = %.3e (must be <= 1e-7)\n", n, worstExcess );
		printf( "8. %d binding states: tightest-to-loosest m_prod/G1_exact min = %.4f\n", binding, worstSlack );
		Check( binding > 100, "the grid exercises the bounded branch", binding, 100 );
		// Tightness regression: the closed-form bound stays within 5% of
		// exact Smith in every binding state of this grid (measured
		// minimum 0.9641; it is exact at isotropy 1, and Jensen's gap is
		// largest across the grain at low isotropy).
		Check( worstSlack > 0.95, "closed-form bound stays within 5% of exact Smith where it binds", worstSlack, 0.95 );
	}

	// 4. Isotropy 1: exactly min(Eq.31, closed-form GGX Smith G1).
	{
		double worst = 0;
		for( double r : { 0.001, 0.01, 0.1, 0.24, 0.3, 1.0 } ) {
			for( double c : { 1e-7, 1e-4, 0.01, 0.1, 0.3, 0.6, 0.99 } ) {
				const double smith = 2 * c / ( c + sqrt( r + ( 1 - r ) * c * c ) );
				const double ref = std::min( Eq31( c, r ), smith );
				const double mp = MProd( c, 0.3, r, 1.0 );
				const double rel = fabs( mp - ref ) / ref;
				worst = std::max( worst, rel );
				Check( rel < 1e-12, "isotropy 1: m == min(Eq.31, exact GGX Smith G1)", mp, ref );
			}
		}
		printf( "4. isotropy 1 vs closed-form min(Eq.31, GGX Smith): worst relative %.3e\n", worst );
	}

	// 5. Reduction to Eq.31 where Eq.31 is inside the bound: every r >= 1/4
	// at isotropy 1, and cos >= (1-4r)/(3-4r) below that.
	{
		double worst = 0;
		for( double r : { 0.25, 0.4, 0.8 } ) for( double c : { 1e-6, 0.01, 0.3, 0.9 } ) {
			const double d = fabs( MProd( c, 1.0, r, 1.0 ) - Eq31( c, r ) ) / Eq31( c, r );
			worst = std::max( worst, d );
			Check( d < 1e-14, "r >= 1/4, isotropy 1: m == Eq.31 at every cosine", d, 0 );
		}
		for( double r : { 0.001, 0.05, 0.2 } ) {
			const double cstar = ( 1 - 4 * r ) / ( 3 - 4 * r );
			for( double f : { 1.001, 1.3, 2.0 } ) {
				const double c = std::min( 0.999, cstar * f );
				const double d = fabs( MProd( c, 0.7, r, 1.0 ) - Eq31( c, r ) ) / Eq31( c, r );
				worst = std::max( worst, d );
				Check( d < 1e-14, "isotropy 1, cos above (1-4r)/(3-4r): m == Eq.31", d, 0 );
			}
		}
		printf( "5. reduction to Eq.31 outside the binding region: worst relative %.3e\n", worst );
	}

	// 6. Isotropy p > 1 == isotropy 1/p rotated by pi/2.
	{
		double worst = 0;
		for( double p : { 0.01, 0.3, 0.8 } ) for( double c : { 1e-4, 0.05, 0.3 } ) for( double ph : { 0.0, 0.4, 1.2 } ) {
			const double a = MProd( c, ph, 0.02, 1.0 / p );
			const double b = MProd( c, ph + 0.5 * kPi, 0.02, p );
			const double d = fabs( a - b ) / b;
			worst = std::max( worst, d );
			Check( d < 1e-12, "isotropy p > 1 is isotropy 1/p rotated a quarter turn", a, b );
		}
		printf( "6. isotropy > 1 symmetry: worst relative %.3e\n", worst );
	}

	// 7. Continuity as isotropy -> 1.
	{
		double worst = 0;
		for( double c : { 1e-5, 0.02, 0.2 } ) for( double ph : { 0.0, 0.9 } ) {
			const double a = MProd( c, ph, 0.01, 1.0 );
			for( double e : { 1e-12, 1e-9, 1e-6 } ) {
				const double b = MProd( c, ph, 0.01, 1.0 - e );
				const double d = fabs( a - b ) / a;
				worst = std::max( worst, d );
				Check( d < 10 * e + 1e-13, "masking is continuous as isotropy -> 1", b, a );
			}
		}
		printf( "7. continuity at isotropy 1: worst relative %.3e\n", worst );
	}

	// 9. The cFast threshold: for c >= cFast the full slow path must
	// itself return Eq.31 (so the shortcut never skips a binding state).
	{
		int n = 0;
		for( double r : { 0.001, 0.02, 0.1, 0.3 } ) for( double p : { 0.01, 0.3, 1.0, 5.0 } ) {
			SchlickMasking::Lane L;
			SchlickMasking::Prepare( L, r, p );
			for( int i = 0; i <= 200; i++ ) {
				const double c = L.cFast + ( 1.0 - L.cFast ) * i / 200.0;
				if( c <= 0 || c >= 1 ) continue;
				for( double ph : { 0.0, 0.7, 1.5 } ) {
					const double den31 = r + ( 1 - r ) * c;
					const double slow = SchlickMasking::MaskOverCosSlow( L, c, cos( ph ), sin( ph ), den31 );
					Check( slow == 1.0 / den31, "c >= cFast: the full evaluation also returns Eq.31", slow, 1.0 / den31 );
					n++;
				}
			}
		}
		printf( "9. cFast shortcut agrees with the full evaluation in %d states\n", n );
	}

	printf( "Checks: %d Failures: %d\n", checks, failures );
	return failures ? 1 : 0;
}
