//////////////////////////////////////////////////////////////////////
//
//  OslVoronoiTest.cpp - The `osl_voronoi_` builtins against Cycles.
//
//  1. Golden values.  tests/data/osl_voronoi_golden.inc was dumped from
//     Cycles' own svm_node_tex_voronoi (tools/osl_golden/
//     dump_voronoi_golden.sh): every dimension, feature and metric, with
//     the node's clamps, fractal parameters and both normalize settings;
//     ties (randomness 0 at half-integer coordinates, where Cycles' loop
//     order picks the cell); edge cases (the non-finite results Cycles
//     produces from finite inputs, and a zero Scale); and coordinates
//     that are -0 after the Scale, where 1D hashes the sign.  The PCG
//     cell hashes are integer arithmetic and must match exactly.  Each
//     node output comes from three builds of Cycles' sources (SIMD and
//     scalar as Blender builds them, scalar as strict IEEE); the port is
//     gated EXACTLY against the strict build (bit for bit, the sign of a
//     zero included; the rows that call powf only on Apple arm64, whose
//     libm the table was dumped with -- 1e-6 elsewhere), and its
//     distance to the other two is printed, split by cause (OslVoronoi.h,
//     PRECISION).
//     THIS IS THE GATE: the later tests run at randomness 0 or compare
//     the kernel with itself, so they do not exercise the hash.  Two
//     details the table does not pin, because no row reaches them: the
//     0.0001 threshold below which Distance to Edge skips a neighbour (no
//     row has two feature points that close), and the use of
//     voronoi_distance_bound rather than voronoi_distance inside F1's
//     search (they order distances alike except at ties made by rounding
//     the final square root or Minkowski root).
//  2. Independent checks that do not use Cycles at all: at randomness 0
//     every feature point sits on its cell's integer corner, so F1, F2,
//     Distance to Edge and N-Sphere Radius have closed forms; one octave
//     plus a second at twice the frequency equals the fractal at detail 1;
//     Smooth F1 never exceeds F1.
//  3. The VM.  Each builtin compiled from expression text returns exactly
//     the kernel's output, from every coordinate layout.
//  4. Compile-time checks: literal arguments, ranges, arity, types.
//  5. Robustness: non-finite coordinates, the octave guard, int-range
//     saturation, NaN parameters, out-of-range enums.
//  6. The memo gate sees the builtins.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/OslVoronoi.h"
#include "../src/Library/Painters/ExpressionEval.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_pass = 0;
static int g_fail = 0;

static void Check( const bool ok, const std::string& what )
{
	if( ok ) { ++g_pass; return; }
	++g_fail;
	std::printf( "  FAIL: %s\n", what.c_str() );
}

//////////////////////////////////////////////////////////////////////
// The golden table's row types (the .inc only holds initialisers).
// Outputs are {distance, color.rgb, position.xyz, w, radius}, given as
// { SIMD as built, scalar as built, scalar as strict IEEE }.
//////////////////////////////////////////////////////////////////////

struct OslPcgRow { int dims; int32_t k[4]; double h[4]; };
struct OslVoronoiRow
{
	int dims; double c[4]; int feature, metric;
	double scale, detail, roughness, lacunarity, smoothness, exponent, randomness;
	int normalize;
	double simd[9], scalar[9], ieee[9];
};

#include "data/osl_voronoi_golden.inc"

#define COUNT( a ) ( (int)( sizeof( a ) / sizeof( ( a )[0] ) ) )

static const char* const kOutputName[9] = { "distance", "color.r", "color.g", "color.b",
	"position.x", "position.y", "position.z", "w", "radius" };

static OslVoronoi::Result Run( const OslVoronoiRow& r )
{
	OslVoronoi::Result out;
	OslVoronoi::VoronoiTexture( r.dims, r.c, r.feature, r.metric, r.scale, r.detail, r.roughness,
		r.lacunarity, r.smoothness, r.exponent, r.randomness, r.normalize != 0, out );
	return out;
}

static bool Equal( const OslVoronoi::Result& a, const OslVoronoi::Result& b );	// every output, Same() (Test 5)

static void Flatten( const OslVoronoi::Result& o, double v[9] )
{
	v[0] = o.distance;
	v[1] = o.color[0]; v[2] = o.color[1]; v[3] = o.color[2];
	v[4] = o.position[0]; v[5] = o.position[1]; v[6] = o.position[2];
	v[7] = o.w; v[8] = o.radius;
}

// From here to the octave walk's end, the comparisons and the walk
// mirror float arithmetic down to the sign of a zero, so they must not be
// compiled with fast-math's no-signed-zeros or reassociation (the test
// build uses -ffast-math).
#if defined(__clang__) || defined(_MSC_VER)
#pragma float_control(precise, on, push)
#endif

//! Bit-for-bit equal, with every NaN equal to every NaN (the golden table
//! writes NaN by name, so its sign and payload are not recorded).  The
//! sign of a zero counts: 1D Voronoi hashes it, so an output fed on to
//! another node's W could pick a different cell.
static bool Same( const double a, const double b )
{
	if( std::isnan( a ) || std::isnan( b ) ) return std::isnan( a ) && std::isnan( b );
	return a == b && std::signbit( a ) == std::signbit( b );
}

//////////////////////////////////////////////////////////////////////
// 1. Golden values
//////////////////////////////////////////////////////////////////////

static void TestPcgHashes()
{
	std::printf( "Test 1a: the PCG cell hashes match Cycles exactly\n" );
	for( int i = 0; i < COUNT( kPcgRows ); ++i ) {
		const OslPcgRow& r = kPcgRows[i];
		float h[4] = { 0, 0, 0, 0 };
		if( r.dims == 2 ) OslVoronoi::HashInt2ToFloat2( r.k[0], r.k[1], h );
		else if( r.dims == 3 ) OslVoronoi::HashInt3ToFloat3( r.k[0], r.k[1], r.k[2], h );
		else OslVoronoi::HashInt4ToFloat4( r.k[0], r.k[1], r.k[2], r.k[3], h );
		bool ok = true;
		for( int c = 0; c < 4; ++c ) ok = ok && double( h[c] ) == r.h[c];
		Check( ok, "PCG hash row " + std::to_string( i ) + " (dims " + std::to_string( r.dims ) + ")" );
	}
}

namespace
{
	double RelErr( const double a, const double b )
	{
		if( std::isnan( a ) || std::isnan( b ) || std::isinf( a ) || std::isinf( b ) ) return Same( a, b ) ? 0.0 : 1.0;
		return std::fabs( a - b ) / std::fmax( 1.0, std::fabs( b ) );
	}

	//! Does the row depart from a build by more than float rounding?  A
	//! different nearest point moves Color and Position by a hash value
	//! and N-Sphere Radius by the gap to another pair, far above the
	//! ~1e-7 rounding gives (the distances stay continuous); a different
	//! -0 hash or int32 conversion moves anything.  Any output counts, so
	//! a departure is seen for every feature, not only those with a
	//! colour.
	const double kBeyondRounding = 1e-5;
	bool Departs( const double* a, const double* b )
	{
		for( int k = 0; k < 9; ++k ) {
			if( !( RelErr( a[k], b[k] ) <= kBeyondRounding ) ) return true;
		}
		return false;
	}

	//! Why a row can depart from a Blender-flags build other than through
	//! rounding.  Both are structural, so they are counted, not folded
	//! into the rounding figure.
	enum Cause { kRounding = 0, kIntRange = 1, kSignedZero = 2, kCauseCount = 3 };
	const char* const kCauseName[kCauseCount] = { "rounding", "a 3D/4D cell id above int32", "a 1D coordinate of -0" };

	struct BuildStats
	{
		int rows[kCauseCount];		// rows of each cause
		int departs[kCauseCount];	// of those, rows that depart beyond rounding
		double rounding;			// max error over rounding rows that do not depart
		std::string worst;			// where `rounding` was reached
		BuildStats() : rounding( 0 ) { for( int c = 0; c < kCauseCount; ++c ) rows[c] = departs[c] = 0; }
	};

	struct Stats
	{
		int rows, intRange, signedZero;
		int powRows, powInexact;	// rows that call powf; of those, not bit-identical (non-Apple only)
		BuildStats simd, scalar;
		Stats() : rows( 0 ), intRange( 0 ), signedZero( 0 ), powRows( 0 ), powInexact( 0 ) {}
	};
}

//! The node's clamps on detail and roughness, in float, and whether it
//! takes Cycles' single-octave shortcut (N-Sphere Radius always does).
static void OctavePlan( const OslVoronoiRow& r, float& detail, bool& oneOctave )
{
	detail = float( r.detail );
	detail = ( detail > 0.0f ) ? detail : 0.0f;
	detail = ( detail < 15.0f ) ? detail : 15.0f;
	float rough = float( r.roughness );
	rough = ( rough > 0.0f ) ? rough : 0.0f;
	rough = ( rough < 1.0f ) ? rough : 1.0f;
	oneOctave = r.feature == OslVoronoi::eNSphereRadius || detail == 0.0f || rough == 0.0f;
}

//! Walks the octaves the kernel runs, in float as it does: the scaled
//! coordinate octave i reads is the node's coordinate times
//! lacunarity^i, rounded at each step, until one is not finite (the
//! port's guard).  `visit` sees each one and returns true to stop.
template<class Visit>
static bool AnyOctave( const OslVoronoiRow& r, const Visit& visit )
{
	float detail;
	bool oneOctave;
	OctavePlan( r, detail, oneOctave );
	const float scale = float( r.scale );
	const float lac = float( r.lacunarity );
	float c[4];
	for( int k = 0; k < r.dims; ++k ) c[k] = float( r.c[k] ) * scale;
	float octaveScale = 1.0f;
	for( int i = 0; (float)i <= std::ceil( detail ); ++i ) {
		float v[4];
		bool finite = true;
		for( int k = 0; k < r.dims; ++k ) { v[k] = c[k] * octaveScale; finite = finite && std::isfinite( v[k] ); }
		if( !finite ) return false;
		if( visit( v ) ) return true;
		if( oneOctave ) return false;
		if( (float)i <= detail ) octaveScale *= lac;
	}
	return false;
}

//! Does an octave that runs put a cell id outside int32 -- above it, when
//! `upwardOnly`?  Cycles' own conversion is undefined there; the port
//! saturates, as Cycles' arm64 scalar path (and its 2D SIMD path, an (int)
//! cast) does.  Its 3D and 4D SIMD path gives INT32_MIN for both
//! directions, so it parts from the port only above the range.
static bool LeavesIntRange( const OslVoronoiRow& r, const bool upwardOnly = false )
{
	if( r.dims == 1 ) return false;
	return AnyOctave( r, [&r, upwardOnly]( const float* v ) {
		for( int k = 0; k < r.dims; ++k ) {
			const float f = std::floor( v[k] );
			if( f >= 2147483648.0f || ( !upwardOnly && f < -2147483648.0f ) ) return true;
		}
		return false;
	} );
}

//! Does an octave that runs read a 1D coordinate of -0?  Blender's
//! -fno-signed-zeros build hashes -0 there where strict IEEE hashes +0.
static bool SignedZero1D( const OslVoronoiRow& r )
{
	if( r.dims != 1 ) return false;
	return AnyOctave( r, []( const float* v ) { return v[0] == 0.0f && std::signbit( v[0] ); } );
}

#if defined(__clang__) || defined(_MSC_VER)
#pragma float_control(pop)
#endif

static void Record( const OslVoronoiRow& r, const double* got, const double* built, const Cause cause,
	const char* label, const int i, BuildStats& b )
{
	++b.rows[cause];
	const bool departs = Departs( got, built );
	if( departs ) ++b.departs[cause];
	if( cause != kRounding || departs ) return;
	for( int k = 0; k < 9; ++k ) {
		const double e = RelErr( got[k], built[k] );
		if( e > b.rounding ) {
			b.rounding = e;
			char buf[200];
			std::snprintf( buf, sizeof buf, "%s row %d, %s (dims %d, feature %d, metric %d, detail %g, roughness %g, lacunarity %g)",
				label, i, kOutputName[k], r.dims, r.feature, r.metric, r.detail, r.roughness, r.lacunarity );
			b.worst = buf;
		}
	}
}

//! Does the row call powf?  Only the Minkowski metric does, and only
//! where the metric is read (F1, F2, Smooth F1 in 2D to 4D).  The strict
//! column was dumped with macOS's libm, and C libraries need not round
//! powf alike, so the gate is bit-for-bit on these rows only on Apple
//! arm64 (the table's platform; an Intel Mac's libm may differ);
//! elsewhere it is 1e-6 relative to max(1, |v|), with the sign of a zero
//! still held, and the test prints how many rows were not bit-identical.  (A correctly rounded
//! pow already parts from macOS's in the last bit on one row.)
static bool UsesPow( const OslVoronoiRow& r )
{
	return r.dims >= 2 && r.metric == OslVoronoi::eMinkowski && r.feature <= OslVoronoi::eSmoothF1;
}

#if defined(__APPLE__) && defined(__aarch64__)
static const bool kPowIsGoldenLibm = true;
#else
static const bool kPowIsGoldenLibm = false;
#endif

static void CheckTable( const char* label, const OslVoronoiRow* rows, const int n, Stats& st )
{
	for( int i = 0; i < n; ++i ) {
		const OslVoronoiRow& r = rows[i];
		double got[9];
		Flatten( Run( r ), got );
		const bool exact = kPowIsGoldenLibm || !UsesPow( r );
		bool identical = true;
		std::string bad;
		for( int k = 0; k < 9; ++k ) {
			identical = identical && Same( got[k], r.ieee[k] );
			// The tolerant path still holds the sign of a zero: where either
			// value is zero, both signs must agree.
			const bool zeroSide = got[k] == 0 || r.ieee[k] == 0;
			const bool ok = exact ? Same( got[k], r.ieee[k] )
				: ( RelErr( got[k], r.ieee[k] ) <= 1e-6 && ( !zeroSide || std::signbit( got[k] ) == std::signbit( r.ieee[k] ) ) );
			if( !ok ) {
				char buf[200];
				std::snprintf( buf, sizeof buf, " %s got %a want %a;", kOutputName[k], got[k], r.ieee[k] );
				bad += buf;
			}
		}
		Check( bad.empty(), std::string( label ) + " row " + std::to_string( i ) + " (dims " + std::to_string( r.dims )
			+ ", feature " + std::to_string( r.feature ) + ", metric " + std::to_string( r.metric ) + "):" + bad );
		if( UsesPow( r ) ) {
			++st.powRows;
			if( !identical ) ++st.powInexact;
		}

		++st.rows;
		const bool intRange = LeavesIntRange( r ), signedZero = SignedZero1D( r );
		if( intRange ) ++st.intRange;
		if( signedZero ) ++st.signedZero;
		// The SIMD path converts 3D and 4D cell ids above int32 differently
		// from the port (below it, both give INT32_MIN); the scalar path and
		// 2D convert alike, so there an out-of-range row is an ordinary
		// rounding row.
		const bool simdIntRange = r.dims >= 3 && LeavesIntRange( r, true );
		const Cause simdCause = signedZero ? kSignedZero : simdIntRange ? kIntRange : kRounding;
		const Cause scalarCause = signedZero ? kSignedZero : kRounding;
		Record( r, got, r.simd, simdCause, label, i, st.simd );
		Record( r, got, r.scalar, scalarCause, label, i, st.scalar );
	}
}

static void PrintBuild( const char* name, const BuildStats& b )
{
	std::printf( "    %s: departs beyond rounding on %d of %d rounding rows, %d of %d rows with %s, %d of %d with %s;\n",
		name, b.departs[kRounding], b.rows[kRounding], b.departs[kIntRange], b.rows[kIntRange], kCauseName[kIntRange],
		b.departs[kSignedZero], b.rows[kSignedZero], kCauseName[kSignedZero] );
	std::printf( "      on every other rounding row, max error %.3g (at %s)\n", b.rounding, b.worst.c_str() );
}

static void TestVoronoiGolden()
{
	std::printf( "Test 1b: the Voronoi node matches strict-IEEE Cycles exactly (powf rows: see below)\n" );
	Stats st;
	CheckTable( "voronoi", kVoronoiRows, COUNT( kVoronoiRows ), st );
	CheckTable( "tie", kTieRows, COUNT( kTieRows ), st );
	CheckTable( "edge", kEdgeRows, COUNT( kEdgeRows ), st );
	CheckTable( "signed-zero", kSignedZeroRows, COUNT( kSignedZeroRows ), st );
	// The edge table exists to pin non-finite results; make sure it does.
	int nonFinite = 0;
	for( int i = 0; i < COUNT( kEdgeRows ); ++i ) {
		for( int k = 0; k < 9; ++k ) if( !std::isfinite( kEdgeRows[i].ieee[k] ) ) { ++nonFinite; break; }
	}
	Check( nonFinite > 0, "the edge table contains non-finite Cycles results" );
	// And the signed-zero table exists to pin the -0 hash: make sure the
	// strict and as-built columns really part there, so the gated column
	// is the one that sees the sign.
	int partsAtZero = 0;
	for( int i = 0; i < COUNT( kSignedZeroRows ); ++i ) {
		const OslVoronoiRow& r = kSignedZeroRows[i];
		if( !SignedZero1D( r ) ) continue;
		if( Departs( r.ieee, r.scalar ) ) ++partsAtZero;
	}
	Check( partsAtZero > 0, "at a 1D coordinate of -0 the strict and as-built Cycles columns differ" );
	std::printf( "  %d rows; %d run an octave whose cell id leaves int32 (Cycles: undefined; port: saturates, as the arm64 scalar build does); %d run an octave at a 1D coordinate of -0\n",
		st.rows, st.intRange, st.signedZero );
	std::printf( "  %d rows call powf (Minkowski); gated %s, and %d of them are not bit-identical\n",
		st.powRows, kPowIsGoldenLibm ? "bit for bit (this libm is the golden table's)" : "to 1e-6 (this libm is not the golden table's)",
		st.powInexact );
	std::printf( "  against Cycles' builds with Blender's flags (printed, not gated); errors relative to max(1, |v|), \"beyond rounding\" = above %g:\n",
		kBeyondRounding );
	PrintBuild( "SIMD", st.simd );
	PrintBuild( "scalar", st.scalar );
}

//////////////////////////////////////////////////////////////////////
// 2. Independent checks
//////////////////////////////////////////////////////////////////////

static OslVoronoi::Result Node( const int dims, const Scalar* p, const int feature, const int metric,
	const Scalar scale, const Scalar detail, const Scalar roughness, const Scalar lacunarity,
	const Scalar smoothness, const Scalar exponent, const Scalar randomness, const bool normalize )
{
	OslVoronoi::Result out;
	OslVoronoi::VoronoiTexture( dims, p, feature, metric, scale, detail, roughness, lacunarity,
		smoothness, exponent, randomness, normalize, out );
	return out;
}

//! A deterministic coordinate in [-5, 5), away from cell edges by at
//! least `margin` so a float rounding cannot change the nearest corner.
static double Coord( const int i, const int c, const double margin )
{
	const uint32_t h = ( uint32_t( i ) * 2654435761u ) ^ ( uint32_t( c + 1 ) * 2246822519u );
	const double f = double( ( h * 747796405u ) >> 8 ) / 16777216.0;	// [0, 1)
	const double cell = double( int( h % 10u ) ) - 5.0;
	double frac = margin + f * ( 1.0 - 2.0 * margin );
	if( std::fabs( frac - 0.5 ) < margin ) frac = 0.5 + margin;
	return cell + frac;
}

static void TestClosedForms()
{
	std::printf( "Test 2: closed forms at randomness 0, octave composition, Smooth F1 <= F1\n" );
	const double tol = 2e-6;
	for( int dims = 1; dims <= 4; ++dims ) {
		for( int i = 0; i < 40; ++i ) {
			Scalar p[4];
			for( int c = 0; c < 4; ++c ) p[c] = Coord( i, c, 0.02 );
			const std::string tag = " (dims " + std::to_string( dims ) + ", point " + std::to_string( i ) + ")";
			// Per axis, the distance to the nearest and the farther of the
			// two bracketing integers, and the nearest integer itself.
			double dn[4], df[4], nearest[4];
			for( int c = 0; c < dims; ++c ) {
				const double l = p[c] - std::floor( p[c] );
				dn[c] = std::fmin( l, 1.0 - l );
				df[c] = std::fmax( l, 1.0 - l );
				nearest[c] = ( l < 0.5 ) ? std::floor( p[c] ) : std::floor( p[c] ) + 1.0;
			}
			// F1 under each metric: every feature point on an integer corner.
			for( int metric = 0; metric < OslVoronoi::kMetricCount; ++metric ) {
				const double e = 1.7;
				double want = 0;
				if( dims == 1 ) want = dn[0];
				else if( metric == OslVoronoi::eEuclidean ) { for( int c = 0; c < dims; ++c ) want += dn[c] * dn[c]; want = std::sqrt( want ); }
				else if( metric == OslVoronoi::eManhattan ) { for( int c = 0; c < dims; ++c ) want += dn[c]; }
				else if( metric == OslVoronoi::eChebychev ) { for( int c = 0; c < dims; ++c ) want = std::fmax( want, dn[c] ); }
				else { for( int c = 0; c < dims; ++c ) want += std::pow( dn[c], e ); want = std::pow( want, 1.0 / e ); }
				const OslVoronoi::Result r = Node( dims, p, OslVoronoi::eF1, metric, 1, 0, 0.5, 2, 1, e, 0, false );
				Check( std::fabs( r.distance - want ) <= tol, "F1 distance is the distance to the nearest lattice point, metric "
					+ std::to_string( metric ) + tag );
				// Position: the nearest lattice point (W carries it in 1D and 4D).
				bool posOk = true;
				if( dims == 1 ) posOk = std::fabs( r.w - nearest[0] ) <= tol;
				else for( int c = 0; c < dims && c < 3; ++c ) posOk = posOk && std::fabs( r.position[c] - nearest[c] ) <= tol;
				if( dims == 4 ) posOk = posOk && std::fabs( r.w - nearest[3] ) <= tol;
				Check( posOk, "F1 position is the nearest lattice point, metric " + std::to_string( metric ) + tag );
			}
			// Scale: the same pattern at p * s, with Position divided back.
			{
				const double s = 2.5;
				Scalar ps[4];
				for( int c = 0; c < 4; ++c ) ps[c] = p[c] / s;
				const OslVoronoi::Result a = Node( dims, ps, OslVoronoi::eF1, 0, s, 0, 0.5, 2, 1, 1, 0, false );
				bool ok = true;
				if( dims == 1 ) ok = std::fabs( a.w - nearest[0] / s ) <= tol;
				else for( int c = 0; c < dims && c < 3; ++c ) ok = ok && std::fabs( a.position[c] - nearest[c] / s ) <= tol;
				Check( ok, "Scale multiplies the coordinate and divides Position back" + tag );
			}
			if( dims == 1 ) {
				// F2 in 1D: the farther of the two bracketing integers.
				const OslVoronoi::Result r = Node( 1, p, OslVoronoi::eF2, 0, 1, 0, 0.5, 2, 1, 1, 0, false );
				Check( std::fabs( r.distance - df[0] ) <= tol, "1D F2 distance is the farther bracketing integer" + tag );
			}
			// Distance to Edge: the cells are unit cubes about integer
			// points, so the distance to the nearest face.
			{
				double want = 1e9;
				for( int c = 0; c < dims; ++c ) want = std::fmin( want, 0.5 - std::fabs( p[c] - nearest[c] ) );
				const OslVoronoi::Result r = Node( dims, p, OslVoronoi::eDistanceToEdge, 0, 1, 0, 0.5, 2, 1, 1, 0, false );
				Check( std::fabs( r.distance - want ) <= tol, "Distance to Edge is the distance to the cell face" + tag );
				const OslVoronoi::Result rn = Node( dims, p, OslVoronoi::eDistanceToEdge, 0, 1, 0, 0.5, 2, 1, 1, 0, true );
				Check( std::fabs( rn.distance - want / 0.5 ) <= tol, "normalized Distance to Edge divides by 0.5 at randomness 0" + tag );
			}
			// N-Sphere Radius: half the unit lattice spacing.
			{
				const OslVoronoi::Result r = Node( dims, p, OslVoronoi::eNSphereRadius, 0, 1, 0, 0.5, 2, 1, 1, 0, false );
				Check( r.radius == 0.5 && r.distance == 0, "N-Sphere Radius is 0.5, Distance 0" + tag );
			}
			// One octave plus a second at twice the frequency, weighted by
			// roughness, is the fractal at detail 1 (random feature points).
			for( int feature = 0; feature <= OslVoronoi::eDistanceToEdge; feature += 3 ) {
				Scalar p2[4];
				for( int c = 0; c < 4; ++c ) p2[c] = p[c] * 2.0;
				const OslVoronoi::Result o0 = Node( dims, p, feature, 0, 1, 0, 0.5, 2, 1, 1, 1, false );
				const OslVoronoi::Result o1 = Node( dims, p2, feature, 0, 1, 0, 0.5, 2, 1, 1, 1, false );
				const OslVoronoi::Result f = Node( dims, p, feature, 0, 1, 1, 0.5, 2, 1, 1, 1, false );
				if( feature == OslVoronoi::eF1 ) {
					// Sums: exact in float (the 0.5 weight is exact).
					const double want = double( float( o0.distance ) + float( o1.distance ) * 0.5f );
					Check( f.distance == want, "F1 at detail 1 = octave 0 + roughness * octave 1" + tag );
				} else {
					// fractal_voronoi_distance_to_edge mixes toward the finer
					// octave's (scaled) edge distance with weight roughness,
					// starting from 8, so its first octave rounds through
					// 8 + (d0 - 8): equal to float rounding, not bit for bit.
					const double d0 = o0.distance, d1 = o1.distance / 2.0;
					const double want = d0 + 0.5 * ( std::fmin( d0, d1 ) - d0 );
					Check( std::fabs( f.distance - want ) <= 4e-6,
						"Distance to Edge at detail 1 = mix(octave 0, min(octave 0, octave 1 / 2), roughness)" + tag );
				}
			}
			// Smooth F1 is a smooth minimum: never above F1.
			for( int metric = 0; metric < OslVoronoi::kMetricCount; ++metric ) {
				const OslVoronoi::Result s = Node( dims, p, OslVoronoi::eSmoothF1, metric, 1, 0, 0.5, 2, 0.8, 1.5, 1, false );
				const OslVoronoi::Result f = Node( dims, p, OslVoronoi::eF1, metric, 1, 0, 0.5, 2, 0.8, 1.5, 1, false );
				Check( s.distance <= f.distance + 1e-6, "Smooth F1 <= F1, metric " + std::to_string( metric ) + tag );
			}
		}
	}
	// Smooth F1 at smoothness 0 is F1, as in Cycles' dispatch.
	const Scalar q[4] = { 0.3, -1.7, 2.2, 0.9 };
	for( int dims = 1; dims <= 4; ++dims ) {
		const OslVoronoi::Result s = Node( dims, q, OslVoronoi::eSmoothF1, 0, 1.5, 2.5, 0.5, 2, 0, 1, 1, true );
		const OslVoronoi::Result f = Node( dims, q, OslVoronoi::eF1, 0, 1.5, 2.5, 0.5, 2, 0, 1, 1, true );
		Check( Equal( s, f ), "Smooth F1 at smoothness 0 equals F1 on every output (dims " + std::to_string( dims ) + ")" );
	}
}

//////////////////////////////////////////////////////////////////////
// 3. The VM
//////////////////////////////////////////////////////////////////////

struct Prog
{
	ExpressionProgram prog;
	ExpressionProgram::Builder builder;
	bool ok;
	explicit Prog( const std::string& expr ) : prog( ExpressionProgram::Invalid() )
	{
		builder.EnableContextVars( true );
		ok = builder.Finalize( expr, prog );
	}
};

static std::string Num( const double x )
{
	char buf[64];
	std::snprintf( buf, sizeof buf, "%.17g", x );
	return buf;
}

static void TestVMMatchesKernel()
{
	std::printf( "Test 3: the osl_voronoi_ builtins through the VM equal the kernel exactly\n" );
	const Vector3 pts[] = { Vector3( 0.3, 1.7, -2.2 ), Vector3( -5.25, 0.125, 9.5 ), Vector3( 41.0, -13.5, 0.75 ) };
	const double wv[] = { 0.4, -2.75, 17.0 };
	const char* const names[5] = { "osl_voronoi_distance", "osl_voronoi_color", "osl_voronoi_position",
		"osl_voronoi_w", "osl_voronoi_radius" };
	for( int ip = 0; ip < 3; ++ip ) {
		ExprEvalContext ctx;
		ctx.P = pts[ip];
		const double w = wv[ip];
		for( int dims = 1; dims <= 4; ++dims ) {
			Scalar c[4] = { 0, 0, 0, 0 };
			if( dims == 1 ) c[0] = w;
			else { c[0] = ctx.P.x; c[1] = ctx.P.y; if( dims >= 3 ) c[2] = ctx.P.z; if( dims == 4 ) c[3] = w; }
			for( int feature = 0; feature < OslVoronoi::kFeatureCount; ++feature ) {
				const int metric = ( feature + dims ) % OslVoronoi::kMetricCount;
				const std::string tag = " (point " + std::to_string( ip ) + ", dims " + std::to_string( dims )
					+ ", feature " + std::to_string( feature ) + ")";
				const std::string args = "P, " + Num( w ) + ", " + std::to_string( dims ) + ", " + std::to_string( feature )
					+ ", " + std::to_string( metric ) + ", 1.75, 2.5, 0.6, 2.1, 0.7, 1.3, 0.9, 1";
				OslVoronoi::Result r;
				OslVoronoi::VoronoiTexture( dims, c, feature, metric, 1.75, 2.5, 0.6, 2.1, 0.7, 1.3, 0.9, true, r );
				const double want[5][3] = {
					{ r.distance, 0, 0 },
					{ r.color[0], r.color[1], r.color[2] },
					{ r.position[0], r.position[1], r.position[2] },
					{ r.w, 0, 0 },
					{ r.radius, 0, 0 } };
				for( int o = 0; o < 5; ++o ) {
					Prog pr( std::string( names[o] ) + "(" + args + ")" );
					Check( pr.ok, std::string( names[o] ) + " compiles" + tag + ": " + pr.builder.Error() );
					if( !pr.ok ) continue;
					const bool vec = ( o == 1 || o == 2 );
					Check( pr.prog.ResultType() == ( vec ? ExpressionProgram::kVec3 : ExpressionProgram::kScalar ),
						std::string( names[o] ) + " has the right result type" + tag );
					if( vec ) {
						const Vector3 v = pr.prog.EvalVec3( ctx );
						Check( Same( v.x, want[o][0] ) && Same( v.y, want[o][1] ) && Same( v.z, want[o][2] ), std::string( names[o] ) + " == kernel" + tag );
					} else {
						Check( Same( pr.prog.Eval( ctx ), want[o][0] ), std::string( names[o] ) + " == kernel" + tag );
					}
				}
			}
		}
	}
	// The sign of a zero survives the VM.  At a negative Scale a 2D
	// Position's z (0 times a negative reciprocal) is -0; and 1D Distance
	// to Edge hashes the floored coordinate's bits, so a W of -0 and +0
	// give different distances.
	{
		ExprEvalContext ctx;
		ctx.P = Vector3( 0.3, -1.2, 0.0 );
		const Scalar c[4] = { 0.3, -1.2, 0, 0 };
		OslVoronoi::Result r;
		OslVoronoi::VoronoiTexture( 2, c, 0, 0, -1.5, 2, 0.5, 2, 1, 1, 1, false, r );
		Check( r.position[2] == 0 && std::signbit( r.position[2] ), "the kernel's 2D Position.z at a negative Scale is -0 (so the VM check is meaningful)" );
		Prog pos( "osl_voronoi_position(P, 0, 2, 0, 0, -1.5, 2, 0.5, 2, 1, 1, 1, 0)" );
		const Vector3 v = pos.ok ? pos.prog.EvalVec3( ctx ) : Vector3( 1, 1, 1 );
		Check( pos.ok && Same( v.x, r.position[0] ) && Same( v.y, r.position[1] ) && Same( v.z, r.position[2] ),
			"osl_voronoi_position keeps the sign of a zero component" );

		const Scalar wNeg[4] = { -0.0, 0, 0, 0 }, wPos[4] = { 0.0, 0, 0, 0 };
		OslVoronoi::Result rn, rp;
		OslVoronoi::VoronoiTexture( 1, wNeg, OslVoronoi::eDistanceToEdge, 0, 1, 0, 0.5, 2, 1, 1, 1, false, rn );
		OslVoronoi::VoronoiTexture( 1, wPos, OslVoronoi::eDistanceToEdge, 0, 1, 0, 0.5, 2, 1, 1, 1, false, rp );
		Check( rn.distance != rp.distance, "the kernel's 1D Distance to Edge differs at W = -0 and +0 (so the VM check is meaningful)" );
		Prog dn( "osl_voronoi_distance(P, -0.0, 1, 3, 0, 1, 0, 0.5, 2, 1, 1, 1, 0)" );
		Prog dp( "osl_voronoi_distance(P, 0.0, 1, 3, 0, 1, 0, 0.5, 2, 1, 1, 1, 0)" );
		Check( dn.ok && Same( dn.prog.Eval( ctx ), rn.distance ), "a W of -0 reaches 1D Distance to Edge through the VM" );
		Check( dp.ok && Same( dp.prog.Eval( ctx ), rp.distance ), "a W of +0 reaches 1D Distance to Edge through the VM" );
	}
	// A non-finite output reads 0 through the VM: a zero lacunarity sends
	// Position through 1/0 in Cycles' arithmetic.
	{
		const Scalar c[4] = { 0.3, 0.4, 0.5, 0 };
		OslVoronoi::Result r;
		OslVoronoi::VoronoiTexture( 3, c, 0, 0, 2, 3, 0.5, 0, 0.5, 1, 1, true, r );
		Check( !std::isfinite( r.position[0] ), "the kernel's Position at lacunarity 0 is not finite (so the VM check is meaningful)" );
		Prog pos( "osl_voronoi_position(P, 0, 3, 0, 0, 2, 3, 0.5, 0, 0.5, 1, 1, 1)" );
		ExprEvalContext ctx;
		ctx.P = Vector3( 0.3, 0.4, 0.5 );
		const Vector3 v = pos.ok ? pos.prog.EvalVec3( ctx ) : Vector3( 1, 1, 1 );
		Check( pos.ok && v.x == 0 && v.y == 0 && v.z == 0, "a non-finite Position reads 0 through the VM" );
		// And a non-finite Distance: a Minkowski exponent of 0 makes F1's
		// distance a power of 1/0, infinite.  (F2's comparisons against an
		// infinite distance never fire, so it keeps its finite start value.
		// Normalized F1 would work as well -- inf / inf is NaN, which also
		// reads 0 -- but un-normalized F1 keeps it a plain infinity.)
		OslVoronoi::Result rd;
		OslVoronoi::VoronoiTexture( 3, c, OslVoronoi::eF1, OslVoronoi::eMinkowski, 2, 0, 0.5, 2, 0.5, 0, 1, false, rd );
		Check( !std::isfinite( rd.distance ), "the kernel's Minkowski distance at exponent 0 is not finite (so the VM check is meaningful)" );
		Prog dist( "osl_voronoi_distance(P, 0, 3, 0, 3, 2, 0, 0.5, 2, 0.5, 0, 1, 0)" );
		Check( dist.ok && dist.prog.Eval( ctx ) == 0, "a non-finite Distance reads 0 through the VM" );
	}
	// Composition: a def of the colour and arithmetic over it.
	{
		ExprEvalContext ctx;
		ctx.P = Vector3( 0.7, -1.1, 2.4 );
		ExpressionProgram prog = ExpressionProgram::Invalid();
		ExpressionProgram::Builder b;
		b.EnableContextVars( true );
		Check( b.AddDef( "cell", "osl_voronoi_color(P, 0, 3, 0, 0, 4, 0, 0.5, 2, 1, 1, 1, 0)" ), "def with an osl_voronoi_ vec3 call" );
		Check( b.Finalize( "cell.y * 2 + osl_voronoi_distance(P, 0, 3, 0, 0, 4, 0, 0.5, 2, 1, 1, 1, 0)", prog ),
			"final expression over the def: " + b.Error() );
		const Scalar c[4] = { 0.7, -1.1, 2.4, 0 };
		OslVoronoi::Result r;
		OslVoronoi::VoronoiTexture( 3, c, 0, 0, 4, 0, 0.5, 2, 1, 1, 1, false, r );
		Check( prog.Eval( ctx ) == r.color[1] * 2 + r.distance, "composed program matches the kernel" );
	}
}

//////////////////////////////////////////////////////////////////////
// 4. Compile-time checks
//////////////////////////////////////////////////////////////////////

static void ExpectError( const std::string& expr, const std::string& fragment )
{
	Prog p( expr );
	const std::string err = p.builder.Error();
	Check( !p.ok && err.find( fragment ) != std::string::npos,
		"`" + expr + "` is refused with \"" + fragment + "\" (got: " + ( p.ok ? std::string( "compiled" ) : err ) + ")" );
}

static void TestCompileChecks()
{
	std::printf( "Test 4: literal, range, arity and type checks\n" );
	const std::string tail = "1, 2, 0.5, 2, 1, 1, 1, 1)";
	ExpectError( "osl_voronoi_distance(P, 0, 0, 0, 0, " + tail, "argument 3 must be an integer literal from 1 to 4" );
	ExpectError( "osl_voronoi_distance(P, 0, 3, 5, 0, " + tail, "argument 4 must be an integer literal from 0 to 4" );
	ExpectError( "osl_voronoi_color(P, 0, 3, 0, 4, " + tail, "argument 5 must be an integer literal from 0 to 3" );
	ExpectError( "osl_voronoi_color(P, 0, 3, u, 0, " + tail, "argument 4 must be an integer literal from 0 to 4" );
	ExpectError( "osl_voronoi_w(P, 0, 4, 0, 0, 1, 2, 0.5, 2, 1, 1, 1, 2)", "argument 13 must be an integer literal from 0 to 1" );
	ExpectError( "osl_voronoi_radius(P, 0, 3, 4, 0, 1, 2, 0.5, 2, 1, 1, 1)", "expects 13 argument(s)" );
	ExpectError( "osl_voronoi_radius(P, 0, 3, 4, 0, 1, 2, 0.5, 2, 1, 1, 1, 1, 0)", "expects 13 argument(s)" );
	ExpectError( "osl_voronoi_position(u, 0, 3, 0, 0, " + tail, "argument 1 must be vec3" );
	ExpectError( "osl_voronoi_position(P, 0, 3, 0, 0, P, 2, 0.5, 2, 1, 1, 1, 1)", "argument 6 must be scalar" );
	// The scalar parameters may be computed; only the node properties are literal.
	Check( Prog( "osl_voronoi_distance(P*2, u, 3, 2, 3, v+1, u*15, 0.5, 2, v, 1+u, u, 0)" ).ok,
		"computed scalar parameters are accepted" );
	// The builtins are pure: they compile where context vars are off.
	ExpressionProgram p = ExpressionProgram::Invalid();
	ExpressionProgram::Builder b;
	Check( b.Finalize( "osl_voronoi_distance(vec3(u, v, 0), 0, 2, 0, 0, 5, 0, 0.5, 2, 1, 1, 1, 0)", p ),
		"osl_voronoi_ builtins need no surface context: " + b.Error() );
}

//////////////////////////////////////////////////////////////////////
// 5. Robustness
//////////////////////////////////////////////////////////////////////

static bool AllZero( const OslVoronoi::Result& r )
{
	return r.distance == 0 && r.color[0] == 0 && r.color[1] == 0 && r.color[2] == 0 &&
		r.position[0] == 0 && r.position[1] == 0 && r.position[2] == 0 && r.w == 0 && r.radius == 0;
}

static bool Equal( const OslVoronoi::Result& a, const OslVoronoi::Result& b )
{
	double x[9], y[9];
	Flatten( a, x );
	Flatten( b, y );
	for( int k = 0; k < 9; ++k ) if( !Same( x[k], y[k] ) ) return false;
	return true;
}

static bool AllFinite( const OslVoronoi::Result& r )
{
	double x[9];
	Flatten( r, x );
	for( int k = 0; k < 9; ++k ) if( !std::isfinite( x[k] ) ) return false;
	return true;
}

static void TestRobustness()
{
	std::printf( "Test 5: non-finite input, the octave guard, int-range saturation, NaN parameters\n" );
	const double inf = std::numeric_limits<double>::infinity();
	const double nan = std::numeric_limits<double>::quiet_NaN();
	const Scalar bad[4][4] = { { inf, 0, 0, 0 }, { 0, nan, 0, 0 }, { 0, 0, -inf, 1 }, { 1e300, 0.2, 0.3, 0.4 } };
	for( int i = 0; i < 4; ++i ) {
		for( int dims = 1; dims <= 4; ++dims ) {
			const bool readsBad = ( i == 0 || i == 3 ) || ( i == 1 && dims >= 2 ) || ( i == 2 && dims >= 3 );
			if( !readsBad ) continue;
			for( int feature = 0; feature < OslVoronoi::kFeatureCount; ++feature ) {
				const OslVoronoi::Result r = Node( dims, bad[i], feature, 0, 1, 3, 0.5, 2, 0.5, 1, 1, true );
				Check( AllZero( r ), "a coordinate that is not finite (or beyond float range) gives 0, case "
					+ std::to_string( i ) + ", dims " + std::to_string( dims ) + ", feature " + std::to_string( feature ) );
			}
		}
	}
	// A finite coordinate that overflows only after the Scale multiply.
	{
		const Scalar p[4] = { 1e30, 0.2, 0.3, 0.4 };
		Check( AllZero( Node( 3, p, 0, 0, 1e10, 0, 0.5, 2, 0.5, 1, 1, false ) ), "a coordinate that overflows under Scale gives 0" );
	}
	// The octave guard: once an octave's scaled coordinate overflows, the
	// loop ends as if Detail had.  With lacunarity 1e30 octave 1 is finite
	// (1e30 < FLT_MAX) and octave 2 is not, so detail 15 equals detail 1.
	const Scalar p[4] = { 0.31, 0.42, 0.53, 0.64 };
	for( int dims = 1; dims <= 4; ++dims ) {
		for( int feature = 0; feature <= OslVoronoi::eDistanceToEdge; ++feature ) {
			const OslVoronoi::Result a = Node( dims, p, feature, 0, 1, 15, 0.5, 1e30, 0.5, 1, 1, true );
			const OslVoronoi::Result b = Node( dims, p, feature, 0, 1, 1, 0.5, 1e30, 0.5, 1, 1, true );
			const std::string tag = " (dims " + std::to_string( dims ) + ", feature " + std::to_string( feature ) + ")";
			Check( Equal( a, b ), "an overflowing octave ends the fractal: detail 15 == detail 1 at lacunarity 1e30" + tag );
			const OslVoronoi::Result c = Node( dims, p, feature, 0, 1, 15, 0.5, inf, 0.5, 1, 1, true );
			const OslVoronoi::Result d = Node( dims, p, feature, 0, 1, 0.999, 0.5, inf, 0.5, 1, 1, true );
			Check( Equal( c, d ) && std::isfinite( c.distance ), "an infinite lacunarity keeps only octave 0's full weight" + tag );
		}
	}
	// Cell ids beyond int32 saturate (defined here; Cycles' conversion is
	// undefined): finite, repeatable outputs, and every cell id past the
	// top is the same cell.
	for( int dims = 2; dims <= 4; ++dims ) {
		const Scalar big1[4] = { 3.0e9, 4.0e9, 5.0e9, 6.0e9 };
		const Scalar big2[4] = { 7.0e9, 8.0e9, 9.0e9, 1.0e10 };
		const OslVoronoi::Result a = Node( dims, big1, 0, 0, 1, 0, 0.5, 2, 0.5, 1, 1, false );
		const OslVoronoi::Result b = Node( dims, big2, 0, 0, 1, 0, 0.5, 2, 0.5, 1, 1, false );
		Check( AllFinite( a ) && a.color[0] == b.color[0] && a.color[1] == b.color[1] && a.color[2] == b.color[2],
			"cell ids above int32 saturate to one cell (dims " + std::to_string( dims ) + ")" );
		const OslVoronoi::Result e = Node( dims, big1, OslVoronoi::eDistanceToEdge, 0, 1, 0, 0.5, 2, 0.5, 1, 1, false );
		const OslVoronoi::Result n = Node( dims, big1, OslVoronoi::eNSphereRadius, 0, 1, 0, 0.5, 2, 0.5, 1, 1, false );
		Check( std::isfinite( e.distance ) && std::isfinite( n.radius ), "Distance to Edge and N-Sphere Radius stay finite there" );
	}
	// NaN and infinite parameters through the node's clamps: NaN reads as
	// the lower bound, +infinity as the upper.
	for( int dims = 1; dims <= 4; ++dims ) {
		const std::string tag = " (dims " + std::to_string( dims ) + ")";
		Check( Equal( Node( dims, p, 0, 0, 1, nan, 0.5, 2, 0.5, 1, 1, true ), Node( dims, p, 0, 0, 1, 0, 0.5, 2, 0.5, 1, 1, true ) ),
			"a NaN detail reads 0" + tag );
		Check( Equal( Node( dims, p, 0, 0, 1, inf, 0.5, 2, 0.5, 1, 1, true ), Node( dims, p, 0, 0, 1, 15, 0.5, 2, 0.5, 1, 1, true ) ),
			"a +infinity detail reads 15" + tag );
		Check( Equal( Node( dims, p, 0, 0, 1, 3, nan, 2, 0.5, 1, 1, true ), Node( dims, p, 0, 0, 1, 3, 0, 2, 0.5, 1, 1, true ) ),
			"a NaN roughness reads 0" + tag );
		Check( Equal( Node( dims, p, 0, 0, 1, 3, 0.5, 2, 0.5, 1, nan, true ), Node( dims, p, 0, 0, 1, 3, 0.5, 2, 0.5, 1, 0, true ) ),
			"a NaN randomness reads 0" + tag );
		Check( Equal( Node( dims, p, 2, 0, 1, 3, 0.5, 2, nan, 1, 1, true ), Node( dims, p, 0, 0, 1, 3, 0.5, 2, 0, 1, 1, true ) ),
			"a NaN smoothness reads 0, so Smooth F1 runs F1" + tag );
	}
	// Out-of-range enums (unreachable from the VM, which checks literals).
	Check( AllZero( Node( 0, p, 0, 0, 1, 2, 0.5, 2, 0.5, 1, 1, true ) ) && AllZero( Node( 5, p, 0, 0, 1, 2, 0.5, 2, 0.5, 1, 1, true ) ),
		"dims outside 1..4 give 0" );
	Check( AllZero( Node( 3, p, 5, 0, 1, 2, 0.5, 2, 0.5, 1, 1, true ) ) && AllZero( Node( 3, p, -1, 0, 1, 2, 0.5, 2, 0.5, 1, 1, true ) ),
		"a feature outside 0..4 gives 0" );
	Check( AllZero( Node( 3, p, 0, 4, 1, 2, 0.5, 2, 0.5, 1, 1, true ) ), "a metric outside 0..3 gives 0" );
}

//////////////////////////////////////////////////////////////////////
// 6. The memo gate
//////////////////////////////////////////////////////////////////////

static void TestMemoWorthiness()
{
	std::printf( "Test 6: an osl_voronoi_ call makes a short body memo-worthy\n" );
	Check( Prog( "osl_voronoi_distance(P, 0, 3, 0, 0, 5, 0, 0.5, 2, 1, 1, 1, 0)" ).prog.MemoWorthy(), "osl_voronoi_distance" );
	Check( Prog( "osl_voronoi_color(P, 0, 3, 0, 0, 5, 0, 0.5, 2, 1, 1, 1, 0).x" ).prog.MemoWorthy(), "osl_voronoi_color" );
	Check( Prog( "osl_voronoi_radius(P, 0, 2, 4, 0, 5, 0, 0.5, 2, 1, 1, 1, 0)" ).prog.MemoWorthy(), "osl_voronoi_radius" );
}

int main()
{
	TestPcgHashes();
	TestVoronoiGolden();
	TestClosedForms();
	TestVMMatchesKernel();
	TestCompileChecks();
	TestRobustness();
	TestMemoWorthiness();
	std::printf( "\nOslVoronoiTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
