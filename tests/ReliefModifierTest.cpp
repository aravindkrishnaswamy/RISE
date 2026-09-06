//////////////////////////////////////////////////////////////////////
//
//  ReliefModifierTest.cpp - Validates the painter-driven micro-relief
//    modifier (src/Library/Modifiers/ReliefModifier.{h,cpp}).  Covers
//    tests 1-8 and 10 of docs/RELIEF_MODIFIER_DESIGN.md 8 (test 9 is
//    `modifier_stack`, a Phase-2 deliverable).
//
//    1. Analytic gradient.  Height `P.x`, N = +Z, T = +X: N' is exactly
//       normalize( (0,0,1) - scale*(1,0,0) ).  The SIGN is the whole
//       point -- it proves the Blinn convention (positive height RISES
//       along +N), which is the OPPOSITE of bumpmap_modifier's.
//    2. Legacy equivalence.  For a real Perlin2DPainter and both
//       `normalize_gradient` values, BumpMap(F, S, W, G) and
//       Relief(Function2DScalarPainter(F), S', uv, W) agree on vNormal
//       and on all three ONB axes at 1000 random (u,v), with
//       S' = -S (normalized) / -S*2W (not) -- the migrator's algebra of
//       design 7.2, which is what makes migration lossless.
//    3. No texcoords.  With derivatives.valid false and ptCoord
//       untouched, a 3D field still perturbs; and ptCoord is unchanged
//       after Modify.
//    4. Footprint fade.  With txFootprint.valid, |N'-N| is
//       non-increasing in worldWidth over a decade sweep on an `fbm`
//       height (the octave fade plus the max(., fw) step rule).
//    5. Object-space exactness.  A height field reading `Po` on a
//       ROTATED instance gives the same N' as the algebraically
//       equivalent world-space field -- which needs pmxWorldToObject.
//    6. UV chain rule.  A UV-parameterised painter in `surface` mode
//       with valid dpdu/dpdv matches `domain uv` on a plane whose UV is
//       the identity map.
//    7. Handedness.  A mirrored (left-handed) incoming frame is still
//       left-handed after the rebuild -- mirrors NormalMap's guarantee.
//    8. Non-finite guard.  A height that returns NaN (and one that
//       returns +Inf) leaves the hit bit-for-bit untouched.
//   10. Parse.  `relief_modifier` round-trips through the real CST
//       loader; `height` bound to an IPainter yields
//       kScalarBoundToIPainterFmt; an unknown name yields
//       kScalarUnknownFmt; an unknown `domain` is refused by name.
//
//  RED-PROOFS PERFORMED (each mutation was applied, the test was run and
//  observed to FAIL, and the mutation was then reverted -- none of them
//  is left in the tree):
//
//    (a) Test 1 / test 2, SIGN.  Flipping ReliefModifier.cpp's
//        perturbation from `N - (T*hT + B*hB)*dScale` to `N + ...`
//        fails test 1's exact-vector check and every one of test 2's
//        4000 legacy-equivalence comparisons.  This is the red-proof
//        design 8 asks for by name on test 2.
//    (b) Test 5, OBJECT SPACE.  Nulling ri.pmxWorldToObject on the
//        object-space hit (so the object-space point moves by the
//        un-rotated world step) makes test 5's world-vs-object
//        agreement fail.  This one is asserted IN the test as a live
//        negative control (NullPointerRedProof below), not just
//        performed by hand, since the test can construct it honestly.
//    (c) Test 8, NON-FINITE GUARD.  ReliefModifier has TWO finiteness
//        gates -- the explicit `isfinite(dT) || isfinite(dB)` early
//        return the design calls for, and the `mag2 > 1e-12 &&
//        isfinite(mag2)` degenerate-normal gate below it -- and the
//        red-proof MEASURED them to be mutually redundant for a
//        non-finite height: removing EITHER one alone leaves test 8
//        green (59/59), and only removing BOTH fails it (6 failures,
//        NaN/Inf normals reaching the frame rebuild).  So test 8 pins
//        the BEHAVIOUR, not either specific line; do not read a green
//        test 8 as proof that the explicit guard is still present.  The
//        explicit guard is kept anyway: it is what the design specifies,
//        it states the intent at the point where the value is produced,
//        and it short-circuits before the perturbation arithmetic.
//    (d) Test 6, CHAIN RULE.  Dropping the ptCoord offset in the
//        surface branch (leaving ri2.ptCoord at the centre) makes the
//        UV-painter-in-surface-mode gradient identically zero and fails
//        test 6.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
	#include <io.h>
	#include <process.h>
	#define getpid _getpid
	#define RISE_TEST_DUP    _dup
	#define RISE_TEST_DUP2   _dup2
	#define RISE_TEST_CLOSE  _close
	#define RISE_TEST_FILENO _fileno
#else
	#include <unistd.h>
	#define RISE_TEST_DUP    dup
	#define RISE_TEST_DUP2   dup2
	#define RISE_TEST_CLOSE  close
	#define RISE_TEST_FILENO fileno
#endif

#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IModifierManager.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Modifiers/BumpMap.h"
#include "../src/Library/Modifiers/ReliefModifier.h"
#include "../src/Library/Painters/ExpressionEval.h"
#include "../src/Library/Painters/ExpressionParamSpec.h"
#include "../src/Library/Painters/Function2DScalarPainter.h"
#include "../src/Library/Painters/Perlin2DPainter.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_failures = 0;
static int g_passes = 0;

#define CHECK( cond, msg )													\
	do {																	\
		if( !(cond) ) {														\
			std::cout << "  FAIL: " << msg << std::endl;					\
			g_failures++;													\
		} else {															\
			g_passes++;														\
		}																	\
	} while( 0 )

static bool VecClose( const Vector3& a, const Vector3& b, Scalar tol )
{
	return std::fabs( a.x - b.x ) <= tol
	    && std::fabs( a.y - b.y ) <= tol
	    && std::fabs( a.z - b.z ) <= tol;
}

// ============================================================
//  Fixtures
// ============================================================

namespace {

//! A hand-written IScalarPainter whose value is a caller-supplied
//! function of the whole hit.  Deliberately NOT one of the shipped
//! painters for the tests that need an exactly-known analytic field: the
//! point of test 1 is to compare against a closed form, and a stub keeps
//! the field's definition in one line next to the assertion.
class FnScalarPainter :
	public virtual IScalarPainter,
	public virtual Reference
{
public:
	typedef Scalar (*Fn)( const RayIntersectionGeometric& );

	explicit FnScalarPainter( Fn f ) : m_f( f ) {}

	ScalarTriple GetValuesAt( const RayIntersectionGeometric& ri ) const override
	{
		return ScalarTriple( m_f( ri ) );
	}

protected:
	virtual ~FnScalarPainter() {}

private:
	Fn m_f;
};

//! Heap-allocated Reference-pattern objects the tests keep alive to the
//! end (both IScalarPainter and IRayIntersectionModifier have protected
//! destructors, so they are released, never deleted).
std::vector<IReference*> g_owned;
template< typename T > T* Own( T* p ) { p->addref(); g_owned.push_back( p ); return p; }
void ReleaseOwned()
{
	for( size_t i = 0; i < g_owned.size(); i++ ) g_owned[i]->release();
	g_owned.clear();
}

//! A flat surface hit: normal +Z, tangent +X, bitangent +Y, at the given
//! world point.  Object-space point defaults to the world point (identity
//! transform) and no object transform is stamped -- the tests that care
//! set both explicitly.
RayIntersectionGeometric MakeRI( const Point3& p )
{
	const Ray inRay( Point3( p.x, p.y, p.z + 1 ), Vector3( 0, 0, -1 ) );
	RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( inRay, rs );

	ri.bHit = true;
	ri.range = 1.0;
	ri.ptIntersection = p;
	ri.ptObjIntersec  = p;
	ri.vNormal        = Vector3( 0, 0, 1 );
	ri.vGeomNormal    = Vector3( 0, 0, 1 );
	ri.onb.CreateFromUV( Vector3( 1, 0, 0 ), Vector3( 0, 1, 0 ) );
	ri.ptCoord = Point2( 0, 0 );
	return ri;
}

ReliefModifier* MakeRelief( const IScalarPainter& h, Scalar scale, ReliefDomain dom, Scalar step )
{
	return Own( new ReliefModifier( h, scale, dom, step ) );
}

//! Compile an expression program with the full 3D context (P, Po, N, fw)
//! enabled -- the `scalar_painter { expression ... }` surface's own
//! configuration, so the tests exercise the real pipe.
ExpressionProgram CompileCtx( const char* expr )
{
	ExpressionProgram p = ExpressionProgram::Invalid();
	ExpressionProgram::Builder b;
	b.EnableContextVars( true );
	b.Finalize( expr, p );
	return p;
}

IScalarPainter* MakeExprScalar( const char* expr )
{
	const ExpressionProgram prog = CompileCtx( expr );
	IScalarPainter* p = 0;
	const std::vector<ParamSpec> noSpecs;
	if( !RISE_API_CreateExpressionScalarPainter( &p, prog, noSpecs ) || !p ) {
		std::cout << "  FATAL: could not compile `" << expr << "`" << std::endl;
		g_failures++;
		return 0;
	}
	g_owned.push_back( p );		// factory hands back refcount 1; do not addref again
	return p;
}

} // anonymous namespace

// ============================================================
//  Test 1: analytic gradient + the Blinn sign
// ============================================================

static Scalar HeightPx( const RayIntersectionGeometric& ri ) { return ri.ptIntersection.x; }

static void Test1_AnalyticGradient()
{
	std::cout << "Test 1: analytic gradient (height = P.x) and the Blinn sign" << std::endl;

	FnScalarPainter* h = Own( new FnScalarPainter( &HeightPx ) );

	const Scalar scales[] = { Scalar(0.25), Scalar(1.0), Scalar(-0.5), Scalar(4.0) };
	for( int i = 0; i < 4; i++ ) {
		const Scalar s = scales[i];
		ReliefModifier* m = MakeRelief( *h, s, ReliefDomain::Surface, Scalar(1e-3) );

		RayIntersectionGeometric ri = MakeRI( Point3( 0.3, -0.7, 0.0 ) );
		m->Modify( ri );

		// dH/dT = 1 exactly (H is linear in x, T = +X), dH/dB = 0.
		// N' = normalize( (0,0,1) - s*(1,0,0) ).
		const Vector3 want = Vector3Ops::Normalize( Vector3( -s, 0, 1 ) );
		CHECK( VecClose( ri.vNormal, want, 1e-12 ),
			"1: N' == normalize(N - scale*T) exactly, for scale " << s );

		// The sign, stated independently of the formula: a field that
		// RISES toward +x must tilt the normal AWAY from +x.  A
		// `bumpmap_modifier`-style `+` would put N'.x on the other side.
		CHECK( ( s > 0 ) ? ( ri.vNormal.x < 0 ) : ( ri.vNormal.x > 0 ),
			"1: positive height rises along +N -- N' leans away from the up-slope (scale " << s << ")" );

		CHECK( std::fabs( Vector3Ops::Magnitude( ri.vNormal ) - 1.0 ) < 1e-12,
			"1: N' is unit length" );
		CHECK( std::fabs( Vector3Ops::Dot( ri.onb.w(), ri.vNormal ) - 1.0 ) < 1e-12,
			"1: the rebuilt ONB's w IS the perturbed normal" );
	}

	// A zero amplitude is inert -- not "perturbs by zero", inert: the
	// frame must come back bit-identical, including the ONB axes.
	{
		ReliefModifier* m = MakeRelief( *h, Scalar(0), ReliefDomain::Surface, Scalar(1e-3) );
		RayIntersectionGeometric ri = MakeRI( Point3( 0.3, -0.7, 0.0 ) );
		const Vector3 n0 = ri.vNormal, u0 = ri.onb.u(), v0 = ri.onb.v();
		m->Modify( ri );
		CHECK( VecClose( ri.vNormal, n0, 0 ) && VecClose( ri.onb.u(), u0, 0 ) && VecClose( ri.onb.v(), v0, 0 ),
			"1: scale 0 leaves the hit bit-identical (inert, not a zero perturbation)" );
	}
}

// ============================================================
//  Test 2: legacy equivalence with bumpmap_modifier
// ============================================================

static void Test2_LegacyEquivalence()
{
	std::cout << "Test 2: legacy bumpmap_modifier equivalence in the uv domain (design 7.2 algebra)" << std::endl;

	// A REAL Perlin2DPainter, dual-registered as the IFunction2D that
	// bumpmap_modifier samples and wrapped by Function2DScalarPainter for
	// the relief side.  Function2DScalarPainter evaluates
	// F.Evaluate(ptCoord.x, ptCoord.y) -- the SAME call BumpMap makes --
	// so the sampled values are identical and the only difference is FP
	// reassociation of the `scale` multiply.
	UniformColorPainter* cA = Own( new UniformColorPainter( RISEPel( 0, 0, 0 ) ) );
	UniformColorPainter* cB = Own( new UniformColorPainter( RISEPel( 1, 1, 1 ) ) );
	Perlin2DPainter* perlin = Own( new Perlin2DPainter(
		Scalar(0.5), 4, *cA, *cB, Vector2( 3.0, 3.0 ), Vector2( 0.0, 0.0 ) ) );

	const Scalar W = Scalar( 0.01 );
	const Scalar S = Scalar( 0.7 );

	for( int gi = 0; gi < 2; gi++ ) {
		const bool normalized = ( gi == 1 );
		// The migrator's fold: legacy tilt is +T*S*(f+ - f-) (or /2W when
		// normalized); the new modifier tilts -T*S'*(f+ - f-)/(2W).
		// Equate: S' = -S when normalized, S' = -S*2W when not.
		const Scalar Sprime = normalized ? ( -S ) : ( -S * Scalar(2) * W );

		BumpMap* bump = Own( new BumpMap( *perlin, S, W, normalized ) );
		Function2DScalarPainter* h = Own( new Function2DScalarPainter( perlin ) );
		ReliefModifier* relief = MakeRelief( *h, Sprime, ReliefDomain::UV, W );

		RandomNumberGenerator rng;
		int normalMismatch = 0, frameMismatch = 0;
		Scalar worstN = 0, worstFrame = 0;
		for( int i = 0; i < 1000; i++ ) {
			const Scalar u = rng.CanonicalRandom() * 4 - 2;
			const Scalar v = rng.CanonicalRandom() * 4 - 2;

			RayIntersectionGeometric riB = MakeRI( Point3( 0, 0, 0 ) );
			riB.ptCoord = Point2( u, v );
			RayIntersectionGeometric riR = riB;

			bump->Modify( riB );
			relief->Modify( riR );

			Scalar dN = 0;
			dN = std::max( dN, std::fabs( riB.vNormal.x - riR.vNormal.x ) );
			dN = std::max( dN, std::fabs( riB.vNormal.y - riR.vNormal.y ) );
			dN = std::max( dN, std::fabs( riB.vNormal.z - riR.vNormal.z ) );
			if( dN > 1e-12 ) normalMismatch++;
			worstN = std::max( worstN, dN );

			Scalar dF = 0;
			const Vector3 axB[3] = { riB.onb.u(), riB.onb.v(), riB.onb.w() };
			const Vector3 axR[3] = { riR.onb.u(), riR.onb.v(), riR.onb.w() };
			for( int k = 0; k < 3; k++ ) {
				dF = std::max( dF, std::fabs( axB[k].x - axR[k].x ) );
				dF = std::max( dF, std::fabs( axB[k].y - axR[k].y ) );
				dF = std::max( dF, std::fabs( axB[k].z - axR[k].z ) );
			}
			if( dF > 1e-12 ) frameMismatch++;
			worstFrame = std::max( worstFrame, dF );
		}

		std::cout << "    normalize_gradient " << ( normalized ? "TRUE " : "FALSE" )
		          << "  worst |dN| " << std::scientific << std::setprecision(3) << worstN
		          << "  worst |donb| " << worstFrame << std::defaultfloat << std::endl;

		CHECK( normalMismatch == 0,
			"2: vNormal matches BumpMap at all 1000 (u,v) within 1e-12 (normalize_gradient "
			<< ( normalized ? "TRUE" : "FALSE" ) << "); mismatches: " << normalMismatch );
		CHECK( frameMismatch == 0,
			"2: the whole rebuilt ONB matches BumpMap within 1e-12 (normalize_gradient "
			<< ( normalized ? "TRUE" : "FALSE" ) << "); mismatches: " << frameMismatch );
	}

	// Oracle: the comparison above is not vacuous -- the WRONG sign is
	// detectably different.  (The by-hand red-proof (a) mutates the
	// modifier itself; this is the in-test version of the same idea,
	// proving the 1e-12 agreement is a real constraint and not two
	// no-ops agreeing.)
	{
		BumpMap* bump = Own( new BumpMap( *perlin, S, W, true ) );
		Function2DScalarPainter* h = Own( new Function2DScalarPainter( perlin ) );
		ReliefModifier* wrongSign = MakeRelief( *h, +S, ReliefDomain::UV, W );	// should be -S

		int differed = 0;
		for( int i = 0; i < 64; i++ ) {
			RayIntersectionGeometric riB = MakeRI( Point3( 0, 0, 0 ) );
			riB.ptCoord = Point2( Scalar(i) * Scalar(0.037), Scalar(i) * Scalar(0.061) );
			RayIntersectionGeometric riR = riB;
			bump->Modify( riB );
			wrongSign->Modify( riR );
			if( !VecClose( riB.vNormal, riR.vNormal, 1e-9 ) ) differed++;
		}
		CHECK( differed >= 60,
			"2: (oracle) the FLIPPED sign disagrees with BumpMap on essentially every probe, "
			"so the match above is a real constraint; differed on " << differed << "/64" );
	}
}

// ============================================================
//  Test 3: no texcoords required
// ============================================================

static Scalar HeightRadial( const RayIntersectionGeometric& ri )
{
	// A genuine 3D field with a non-zero tangential gradient at the probe.
	return std::sin( ri.ptIntersection.x * 3.0 ) * std::cos( ri.ptIntersection.y * 2.0 );
}

static void Test3_NoTexcoords()
{
	std::cout << "Test 3: a 3D field perturbs with no texcoords and no derivatives" << std::endl;

	FnScalarPainter* h = Own( new FnScalarPainter( &HeightRadial ) );
	ReliefModifier* m = MakeRelief( *h, Scalar(0.5), ReliefDomain::Surface, Scalar(1e-3) );

	RayIntersectionGeometric ri = MakeRI( Point3( 0.37, 0.21, 0.0 ) );
	ri.derivatives.valid = false;
	ri.ptCoord = Point2( -12345.0, 54321.0 );		// nonsense UV: must be neither read nor written
	const Point2 uv0 = ri.ptCoord;
	const Vector3 n0 = ri.vNormal;

	m->Modify( ri );

	CHECK( !VecClose( ri.vNormal, n0, 1e-9 ),
		"3: the normal IS perturbed by a 3D field with derivatives.valid == false" );
	CHECK( ri.ptCoord.x == uv0.x && ri.ptCoord.y == uv0.y,
		"3: ptCoord is left exactly as it arrived" );
	CHECK( std::fabs( Vector3Ops::Magnitude( ri.vNormal ) - 1.0 ) < 1e-12,
		"3: N' is unit length" );

	// Closed form: dH/dx = 3*cos(3x)*cos(2y), dH/dy = -2*sin(3x)*sin(2y).
	const Scalar x = 0.37, y = 0.21;
	const Scalar gx =  3.0 * std::cos( 3.0 * x ) * std::cos( 2.0 * y );
	const Scalar gy = -2.0 * std::sin( 3.0 * x ) * std::sin( 2.0 * y );
	const Vector3 want = Vector3Ops::Normalize( Vector3( -0.5 * gx, -0.5 * gy, 1 ) );
	// Central difference at h = 1e-3 has O(h^2) truncation error; the
	// third derivative here is O(30), so ~1e-5 is the honest bound.
	CHECK( VecClose( ri.vNormal, want, 1e-5 ),
		"3: N' matches the closed-form gradient to central-difference truncation order" );
}

// ============================================================
//  Test 4: footprint fade
// ============================================================

static void Test4_FootprintFade()
{
	std::cout << "Test 4: |N'-N| is non-increasing in the pixel footprint (fbm height)" << std::endl;

	IScalarPainter* h = MakeExprScalar( "fbm(P*8, 5, 0.5, 2.0)" );
	if( !h ) return;
	ReliefModifier* m = MakeRelief( *h, Scalar(1.0), ReliefDomain::Surface, Scalar(0) );	// step 0 = auto

	// Averaged over a patch of probe points: the claim is about the field's
	// BEHAVIOUR under filtering, and a single point can move either way by
	// chance as the stencil widens across a noise field.
	const Scalar widths[] = { Scalar(1e-3), Scalar(1e-2), Scalar(1e-1), Scalar(1.0) };
	Scalar mean[4] = { 0, 0, 0, 0 };
	const int N = 400;
	for( int wi = 0; wi < 4; wi++ ) {
		RandomNumberGenerator rng( 12345 );
		Scalar acc = 0;
		for( int i = 0; i < N; i++ ) {
			const Point3 p( rng.CanonicalRandom() * 2 - 1, rng.CanonicalRandom() * 2 - 1, 0 );
			RayIntersectionGeometric ri = MakeRI( p );
			ri.txFootprint.valid = true;
			ri.txFootprint.worldWidth = widths[wi];
			const Vector3 n0 = ri.vNormal;
			m->Modify( ri );
			acc += Vector3Ops::Magnitude( ri.vNormal - n0 );
		}
		mean[wi] = acc / N;
		std::cout << "    worldWidth " << std::scientific << std::setprecision(1) << widths[wi]
		          << "  mean |N'-N| " << std::setprecision(4) << mean[wi] << std::defaultfloat << std::endl;
	}

	for( int wi = 1; wi < 4; wi++ ) {
		CHECK( mean[wi] <= mean[wi-1] * Scalar(1.0 + 1e-9),
			"4: mean |N'-N| is non-increasing from worldWidth " << widths[wi-1]
			<< " to " << widths[wi] << " (" << mean[wi-1] << " -> " << mean[wi] << ")" );
	}
	CHECK( mean[0] > mean[3] * Scalar(2),
		"4: (oracle) the fade is real, not a flat line -- the narrowest footprint "
		"perturbs at least 2x more than the widest (" << mean[0] << " vs " << mean[3] << ")" );
}

// ============================================================
//  Test 5: object-space exactness under a rotated instance
// ============================================================

static void Test5_ObjectSpaceExactness()
{
	std::cout << "Test 5: an object-space height field on a rotated instance matches the world-space equivalent" << std::endl;

	// A rotation about +Z by 37 degrees, as the object's WORLD->OBJECT map.
	const Scalar ang = Scalar( 37.0 * 3.14159265358979323846 / 180.0 );
	const Scalar ca = std::cos( ang ), sa = std::sin( ang );
	// Po = R * P, with R's rows (ca, sa, 0), (-sa, ca, 0), (0, 0, 1).
	// Matrix4's Transform reads columns as m._i0/_i1/_i2 * v.x etc (see
	// VectorsOps.h), so lay the matrix out to realise exactly that map.
	Matrix4 W2O;
	W2O = Matrix4Ops::Identity();
	W2O._00 =  ca;  W2O._10 =  sa;  W2O._20 = 0;
	W2O._01 = -sa;  W2O._11 =  ca;  W2O._21 = 0;
	W2O._02 =   0;  W2O._12 =   0;  W2O._22 = 1;

	// Sanity: the matrix really implements the intended linear map.
	{
		const Vector3 e( 1, 0, 0 );
		const Vector3 got = Vector3Ops::Transform( W2O, e );
		CHECK( VecClose( got, Vector3( ca, -sa, 0 ), 1e-12 ),
			"5: (setup) the test matrix maps world +X to object (cos, -sin, 0)" );
	}

	// Object-space field H_o(Po) = 3*Po.x + 2*Po.y, and its world-space
	// equivalent H_w(P) = 3*(ca*P.x + sa*P.y) + 2*(-sa*P.x + ca*P.y).
	IScalarPainter* hObj = MakeExprScalar( "3*Po.x + 2*Po.y" );
	char worldExpr[256];
	std::snprintf( worldExpr, sizeof(worldExpr), "%.17g*P.x + %.17g*P.y",
		(double)( 3 * ca - 2 * sa ), (double)( 3 * sa + 2 * ca ) );
	IScalarPainter* hWorld = MakeExprScalar( worldExpr );
	if( !hObj || !hWorld ) return;

	ReliefModifier* mObj   = MakeRelief( *hObj,   Scalar(0.3), ReliefDomain::Surface, Scalar(1e-3) );
	ReliefModifier* mWorld = MakeRelief( *hWorld, Scalar(0.3), ReliefDomain::Surface, Scalar(1e-3) );

	const Point3 P( 0.41, -0.23, 0.15 );
	const Point3 Po = Point3Ops::Transform( W2O, P );

	RayIntersectionGeometric riObj = MakeRI( P );
	riObj.ptObjIntersec = Po;
	riObj.pmxWorldToObject = &W2O;
	mObj->Modify( riObj );

	RayIntersectionGeometric riWorld = MakeRI( P );
	mWorld->Modify( riWorld );

	CHECK( VecClose( riObj.vNormal, riWorld.vNormal, 1e-9 ),
		"5: the object-space field's N' equals the world-space equivalent's" );
	CHECK( !VecClose( riObj.vNormal, Vector3( 0, 0, 1 ), 1e-6 ),
		"5: (oracle) the field actually perturbs, so the agreement is not two no-ops" );

	// RED-PROOF (b), asserted live: with pmxWorldToObject absent the
	// object-space point is moved by the UN-rotated world step, so the
	// object-space field sees the wrong gradient and the two disagree.
	// If this ever starts passing, the pointer has stopped being read.
	RayIntersectionGeometric riNull = MakeRI( P );
	riNull.ptObjIntersec = Po;
	riNull.pmxWorldToObject = 0;
	mObj->Modify( riNull );
	CHECK( !VecClose( riNull.vNormal, riWorld.vNormal, 1e-6 ),
		"5: RED-PROOF -- nulling pmxWorldToObject DOES change the answer "
		"(the object-space step is really being transformed)" );
}

// ============================================================
//  Test 6: UV chain rule in the surface domain
// ============================================================

static void Test6_UVChainRule()
{
	std::cout << "Test 6: a UV-parameterised height in `surface` mode matches `uv` mode on an identity-UV plane" << std::endl;

	// The plane z = 0 with u = x, v = y: dpdu = +X, dpdv = +Y, so a world
	// step of s along T = +X is exactly a UV step of s along u.  Under
	// that identity map the surface-domain chain rule and the uv domain
	// must produce the same gradient -- which is the whole claim.
	UniformColorPainter* cA = Own( new UniformColorPainter( RISEPel( 0, 0, 0 ) ) );
	UniformColorPainter* cB = Own( new UniformColorPainter( RISEPel( 1, 1, 1 ) ) );
	Perlin2DPainter* perlin = Own( new Perlin2DPainter(
		Scalar(0.5), 3, *cA, *cB, Vector2( 2.0, 2.0 ), Vector2( 0.0, 0.0 ) ) );
	Function2DScalarPainter* h = Own( new Function2DScalarPainter( perlin ) );

	const Scalar s = Scalar( 0.01 );
	ReliefModifier* mSurface = MakeRelief( *h, Scalar(0.4), ReliefDomain::Surface, s );
	ReliefModifier* mUV      = MakeRelief( *h, Scalar(0.4), ReliefDomain::UV,      s );

	RandomNumberGenerator rng( 777 );
	int mismatch = 0, perturbed = 0;
	Scalar worst = 0;
	for( int i = 0; i < 200; i++ ) {
		const Scalar x = rng.CanonicalRandom() * 2 - 1;
		const Scalar y = rng.CanonicalRandom() * 2 - 1;

		RayIntersectionGeometric riS = MakeRI( Point3( x, y, 0 ) );
		riS.ptCoord = Point2( x, y );
		riS.derivatives.valid = true;
		riS.derivatives.dpdu = Vector3( 1, 0, 0 );
		riS.derivatives.dpdv = Vector3( 0, 1, 0 );

		RayIntersectionGeometric riU = riS;

		mSurface->Modify( riS );
		mUV->Modify( riU );

		Scalar d = 0;
		d = std::max( d, std::fabs( riS.vNormal.x - riU.vNormal.x ) );
		d = std::max( d, std::fabs( riS.vNormal.y - riU.vNormal.y ) );
		d = std::max( d, std::fabs( riS.vNormal.z - riU.vNormal.z ) );
		if( d > 1e-12 ) mismatch++;
		worst = std::max( worst, d );
		if( std::fabs( riU.vNormal.z - 1.0 ) > 1e-9 ) perturbed++;
	}

	std::cout << "    worst |dN| " << std::scientific << std::setprecision(3) << worst
	          << std::defaultfloat << "   perturbed probes " << perturbed << "/200" << std::endl;
	CHECK( mismatch == 0,
		"6: surface-domain chain rule == uv domain on an identity-UV plane (mismatches " << mismatch << ")" );
	CHECK( perturbed >= 190,
		"6: (oracle) the field actually perturbs on essentially every probe (" << perturbed << "/200)" );

	// And the negative half: WITHOUT derivatives the surface-domain
	// evaluation cannot move ptCoord, so a UV-only painter reads flat.
	// That is the documented behaviour (design 3.2), and the descriptor
	// names `domain uv` as the route for it -- pin it so it stays a
	// documented answer and not an accident.
	{
		RayIntersectionGeometric ri = MakeRI( Point3( 0.3, 0.4, 0 ) );
		ri.ptCoord = Point2( 0.3, 0.4 );
		ri.derivatives.valid = false;
		const Vector3 n0 = ri.vNormal;
		mSurface->Modify( ri );
		CHECK( VecClose( ri.vNormal, n0, 0 ),
			"6: a UV-only painter in surface mode with NO derivatives reads flat (documented)" );
	}
}

// ============================================================
//  Test 7: handedness preservation
// ============================================================

static void Test7_Handedness()
{
	std::cout << "Test 7: a mirrored (left-handed) incoming frame stays left-handed" << std::endl;

	FnScalarPainter* h = Own( new FnScalarPainter( &HeightRadial ) );
	ReliefModifier* m = MakeRelief( *h, Scalar(0.6), ReliefDomain::Surface, Scalar(1e-3) );

	int kept = 0, probes = 0, actuallyPerturbed = 0;
	RandomNumberGenerator rng( 4242 );
	for( int i = 0; i < 500; i++ ) {
		RayIntersectionGeometric ri = MakeRI( Point3(
			rng.CanonicalRandom() * 2 - 1, rng.CanonicalRandom() * 2 - 1, 0 ) );

		// A geometry-supplied tangent (so the projection branch runs) plus
		// the deliberate FlipV that Object::IntersectRay applies to a
		// negative-determinant instance.
		ri.bHasShadingTangent = true;
		ri.vShadingTangent = ri.onb.u();
		ri.onb.FlipV();

		const Scalar before = Vector3Ops::Dot( ri.onb.u(),
			Vector3Ops::Cross( ri.onb.v(), ri.onb.w() ) );
		if( !( before < 0 ) ) continue;		// setup sanity
		const Vector3 n0 = ri.vNormal;

		m->Modify( ri );
		probes++;
		if( !VecClose( ri.vNormal, n0, 1e-9 ) ) actuallyPerturbed++;

		const Scalar after = Vector3Ops::Dot( ri.onb.u(),
			Vector3Ops::Cross( ri.onb.v(), ri.onb.w() ) );
		if( after < 0 ) kept++;
	}

	CHECK( probes >= 490, "7: (setup) the mirrored frames were actually left-handed (" << probes << "/500)" );
	CHECK( actuallyPerturbed >= 490, "7: (oracle) the probes were actually perturbed (" << actuallyPerturbed << ")" );
	CHECK( kept == probes,
		"7: every perturbed hit kept its left-handed frame (" << kept << "/" << probes << ")" );

	// And the ONB stays orthonormal through the flip.
	{
		RayIntersectionGeometric ri = MakeRI( Point3( 0.11, 0.22, 0 ) );
		ri.bHasShadingTangent = true;
		ri.vShadingTangent = ri.onb.u();
		ri.onb.FlipV();
		m->Modify( ri );
		const Vector3 u = ri.onb.u(), v = ri.onb.v(), w = ri.onb.w();
		CHECK( std::fabs( Vector3Ops::Dot( u, v ) ) < 1e-12
		    && std::fabs( Vector3Ops::Dot( u, w ) ) < 1e-12
		    && std::fabs( Vector3Ops::Dot( v, w ) ) < 1e-12
		    && std::fabs( Vector3Ops::Magnitude( u ) - 1 ) < 1e-12
		    && std::fabs( Vector3Ops::Magnitude( v ) - 1 ) < 1e-12
		    && std::fabs( Vector3Ops::Magnitude( w ) - 1 ) < 1e-12,
			"7: the rebuilt frame is still orthonormal after the handedness restore" );
	}
}

// ============================================================
//  Test 8: the non-finite guard
// ============================================================

static Scalar HeightNaN( const RayIntersectionGeometric& ri )
{
	// NaN only OFF-centre, so the guard has to catch it in the stencil
	// rather than at the shading point.
	return ( ri.ptIntersection.x > 0.5 ) ? std::nan( "" ) : Scalar( 1.0 );
}
static Scalar HeightInf( const RayIntersectionGeometric& )
{
	return std::numeric_limits<Scalar>::infinity();
}

static void Test8_NonFiniteGuard()
{
	std::cout << "Test 8: a non-finite height leaves the hit untouched" << std::endl;

	FnScalarPainter::Fn fns[2] = { &HeightNaN, &HeightInf };
	const char* names[2] = { "NaN", "+Inf" };

	for( int i = 0; i < 2; i++ ) {
		FnScalarPainter* h = Own( new FnScalarPainter( fns[i] ) );
		ReliefModifier* m = MakeRelief( *h, Scalar(1.0), ReliefDomain::Surface, Scalar(1e-3) );

		RayIntersectionGeometric ri = MakeRI( Point3( 0.5, 0.0, 0.0 ) );
		const Vector3 n0 = ri.vNormal, u0 = ri.onb.u(), v0 = ri.onb.v(), w0 = ri.onb.w();

		m->Modify( ri );

		CHECK( VecClose( ri.vNormal, n0, 0 ),
			"8: " << names[i] << " height leaves vNormal bit-identical" );
		CHECK( VecClose( ri.onb.u(), u0, 0 ) && VecClose( ri.onb.v(), v0, 0 ) && VecClose( ri.onb.w(), w0, 0 ),
			"8: " << names[i] << " height leaves the whole ONB bit-identical" );
		CHECK( std::isfinite( ri.vNormal.x ) && std::isfinite( ri.vNormal.y ) && std::isfinite( ri.vNormal.z ),
			"8: " << names[i] << " height never produces a non-finite normal" );
	}
}

// ============================================================
//  Test 10: the scene chunk
// ============================================================

namespace {

std::string WriteTempScene( const std::string& tag, const std::string& body )
{
	const char* tmp = getenv( "TMPDIR" );
	std::string dir = tmp ? tmp : "/tmp/";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	char pid[32];
	std::snprintf( pid, sizeof(pid), "%d", static_cast<int>( ::getpid() ) );
	const std::string path = dir + "rise_relief_" + tag + "_" + pid + ".RISEscene";
	std::ofstream f( path.c_str(), std::ios::binary | std::ios::trunc );
	f << body;
	f.close();
	return path;
}

//! Load a scene body with stdout captured -- GlobalLog's eLog_Console
//! sink includes eLog_Error, so the Job::AddReliefModifier diagnostics
//! land there.  Same fd-dup technique as HairMaterialChunkTest.
bool ParseCapturing( const std::string& tag, const std::string& body,
                     IJobPriv& job, std::string& captured )
{
	const char* tmpEnv = getenv( "TMPDIR" );
	std::string dir = tmpEnv ? tmpEnv : "/tmp/";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	char pidbuf[32];
	std::snprintf( pidbuf, sizeof(pidbuf), "%d", static_cast<int>( ::getpid() ) );
	const std::string capPath = dir + "rise_relief_stdout_" + tag + "_" + pidbuf + ".txt";

	const std::string path = WriteTempScene( tag, "RISE ASCII SCENE 7\n" + body );

	std::fflush( stdout );
	const int savedFd = RISE_TEST_DUP( RISE_TEST_FILENO( stdout ) );
	FILE* capFile = std::fopen( capPath.c_str(), "w" );
	if( capFile ) RISE_TEST_DUP2( RISE_TEST_FILENO( capFile ), RISE_TEST_FILENO( stdout ) );

	const bool ok = job.LoadAsciiSceneViaCst( path.c_str() );

	std::fflush( stdout );
	if( savedFd >= 0 ) { RISE_TEST_DUP2( savedFd, RISE_TEST_FILENO( stdout ) ); RISE_TEST_CLOSE( savedFd ); }
	if( capFile ) std::fclose( capFile );

	std::ifstream ifs( capPath.c_str() );
	if( ifs.is_open() ) { std::ostringstream oss; oss << ifs.rdbuf(); captured = oss.str(); }
	remove( capPath.c_str() );
	remove( path.c_str() );
	return ok;
}

bool Contains( const std::string& hay, const char* needle )
{
	return hay.find( needle ) != std::string::npos;
}

} // anonymous namespace

namespace RISE { bool RISE_CreateJobPriv( IJobPriv** ppi ); }

static void Test10_Parse()
{
	std::cout << "Test 10: the relief_modifier chunk parses, and the scalar-pipe diagnostics fire" << std::endl;

	// (a) The happy path: a scalar_painter height, both domains, an
	//     explicit step, and a negative amplitude.
	{
		IJobPriv* job = 0;
		RISE_CreateJobPriv( &job );
		CHECK( job != 0, "10a: job created" );
		if( job ) {
			std::string log;
			const bool ok = ParseCapturing( "ok",
				"scalar_painter\n{\n\tname h3\n\texpression 0.25*sin(P.x*8)\n}\n"
				"relief_modifier\n{\n\tname r_surface\n\theight h3\n\tscale 0.02\n}\n"
				"relief_modifier\n{\n\tname r_uv\n\theight h3\n\tscale -0.01\n\tdomain uv\n\tstep 0.005\n}\n",
				*job, log );
			CHECK( ok, "10a: the scene loads" );
			CHECK( job->GetModifiers()->GetItem( "r_surface" ) != 0,
				"10a: `r_surface` is registered in the modifier manager" );
			CHECK( job->GetModifiers()->GetItem( "r_uv" ) != 0,
				"10a: `r_uv` (domain uv, negative scale, explicit step) is registered" );
			safe_release( job );
		}
	}

	// (b) `height` bound to a COLOUR painter: the standing scalar-pipe
	//     diagnostic, whose text lives in ChunkDescriptor.h
	//     (kScalarBoundToIPainterFmt) and is matched here only as
	//     substrings so a re-word does not silently un-test it.
	{
		IJobPriv* job = 0;
		RISE_CreateJobPriv( &job );
		if( job ) {
			std::string log;
			ParseCapturing( "ipainter",
				"uniformcolor_painter\n{\n\tname cp\n\tcolor 0.5 0.5 0.5\n}\n"
				"relief_modifier\n{\n\tname r_bad\n\theight cp\n}\n",
				*job, log );
			CHECK( Contains( log, "relief_modifier `r_bad`" )
			    && Contains( log, "is bound to `IPainter` chunk `cp`" ),
				"10b: an IPainter bound to `height` yields the kScalarBoundToIPainterFmt diagnostic" );
			CHECK( Contains( log, "`scalar_painter`" ) && Contains( log, "no JH spectral uplift" ),
				"10b: the diagnostic names the `scalar_painter` fix and why the pipe matters" );
			CHECK( job->GetModifiers()->GetItem( "r_bad" ) == 0,
				"10b: the modifier is NOT registered after the diagnostic" );
			safe_release( job );
		}
	}

	// (c) An unknown name: kScalarUnknownFmt.
	{
		IJobPriv* job = 0;
		RISE_CreateJobPriv( &job );
		if( job ) {
			std::string log;
			ParseCapturing( "unknown",
				"relief_modifier\n{\n\tname r_unk\n\theight nosuchpainter\n}\n",
				*job, log );
			CHECK( Contains( log, "relief_modifier `r_unk`" )
			    && Contains( log, "`nosuchpainter`" )
			    && Contains( log, "neither a registered scalar_painter nor an inline" ),
				"10c: an unknown `height` name yields the kScalarUnknownFmt diagnostic" );
			CHECK( job->GetModifiers()->GetItem( "r_unk" ) == 0,
				"10c: the modifier is NOT registered after the diagnostic" );
			safe_release( job );
		}
	}

	// (d) An unrecognized `domain` is refused BY NAME, not defaulted --
	//     the silent-wrong-answer this validation exists to prevent is a
	//     3D field sampled in UV, which reads flat on anything without
	//     texcoords.
	{
		IJobPriv* job = 0;
		RISE_CreateJobPriv( &job );
		if( job ) {
			std::string log;
			ParseCapturing( "domain",
				"relief_modifier\n{\n\tname r_dom\n\theight 0.5\n\tdomain sideways\n}\n",
				*job, log );
			CHECK( Contains( log, "relief_modifier `r_dom`" )
			    && Contains( log, "`domain` value `sideways` is not recognized" )
			    && Contains( log, "`surface`" ) && Contains( log, "`uv`" ),
				"10d: an unknown `domain` is refused with a diagnostic naming both legal values" );
			CHECK( job->GetModifiers()->GetItem( "r_dom" ) == 0,
				"10d: the modifier is NOT registered after the domain diagnostic" );
			safe_release( job );
		}
	}

	// (e) An undeclared parameter is refused by the descriptor layer --
	//     proving the chunk really is descriptor-driven and its parameter
	//     set is the descriptor.
	{
		IJobPriv* job = 0;
		RISE_CreateJobPriv( &job );
		if( job ) {
			std::string log;
			ParseCapturing( "undeclared",
				"relief_modifier\n{\n\tname r_x\n\theight 0.5\n\twindowsize 0.01\n}\n",
				*job, log );
			CHECK( Contains( log, "windowsize" ) && Contains( log, "relief_modifier" ),
				"10e: an undeclared parameter (`windowsize`) is refused by the descriptor" );
			safe_release( job );
		}
	}
}

// ============================================================

int main()
{
	std::cout << "=== ReliefModifierTest (docs/RELIEF_MODIFIER_DESIGN.md 8, tests 1-8 + 10) ===" << std::endl;

	Test1_AnalyticGradient();
	Test2_LegacyEquivalence();
	Test3_NoTexcoords();
	Test4_FootprintFade();
	Test5_ObjectSpaceExactness();
	Test6_UVChainRule();
	Test7_Handedness();
	Test8_NonFiniteGuard();
	Test10_Parse();

	ReleaseOwned();

	std::cout << std::endl;
	std::cout << g_passes << " passed, " << g_failures << " failed." << std::endl;
	if( g_failures == 0 ) std::cout << "=== ALL TESTS PASSED ===" << std::endl;
	return g_failures == 0 ? 0 : 1;
}
