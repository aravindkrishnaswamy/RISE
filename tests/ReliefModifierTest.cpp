//////////////////////////////////////////////////////////////////////
//
//  ReliefModifierTest.cpp - Validates the painter-driven micro-relief
//    modifier (src/Library/Modifiers/ReliefModifier.{h,cpp}) AND, since
//    Phase 2, the modifier_stack composition class
//    (src/Library/Modifiers/ModifierStack.{h,cpp}).  Covers tests 1-10
//    of docs/RELIEF_MODIFIER_DESIGN.md 8, plus 7b from fix round 1 and
//    4c from fix round 2.
//    Two of its cases reach into BumpMap and NormalMap: test 2 (legacy
//    equivalence) and test 7b(i) (the shared frame-rebuild gate).  4c
//    reaches into Object::IntersectRay: the txFootprint.worldWidth
//    object-to-world fold it added (fix round 2, P2-A) is not part of
//    ReliefModifier itself, but ReliefModifier is worldWidth's first
//    geometric consumer and the bug (an uncorrected object-space length
//    read as world) is otherwise invisible at scale 1.  Test 9 reaches
//    into NormalMap and GlintModifier too, to exercise the order-
//    semantics table on real modifiers rather than stubs.
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
//       height (the octave fade plus the max(., fw) step rule), AND
//       strictly DECREASING over the same sweep on an fw-BLIND step
//       height -- which is the half that actually discriminates the
//       max(., fw) rule (see red-proof (e)).
//   4c. Footprint world-scale fold (fix round 2, P2-A).  A real cast
//       through Object::IntersectRay -- single-triangle mesh, ray
//       differentials, primary hit -- at world scale 1 and at world
//       scale 10 (uniform), same local hit point, same local
//       differentials: txFootprint.worldWidth's ratio between the two
//       must be 10, not 1.  Catches the object-to-world fold this field
//       was missing (derivatives.scaleHint's sibling fold already
//       existed; worldWidth's did not).
//    5. Object-space exactness.  A height field reading `Po` on a
//       ROTATED instance -- and, separately, on a NON-UNIFORMLY SCALED
//       one -- gives the same N' as the algebraically equivalent
//       world-space field, which needs pmxWorldToObject.  The scale case
//       is what discriminates the field's documented claim that the
//       object-space step is the world step through the LINEAR map: a
//       rotation preserves lengths and cannot tell that apart from
//       moving the object point by the world step.
//    6. UV chain rule.  A UV-parameterised painter in `surface` mode
//       with valid dpdu/dpdv matches `domain uv` on a plane whose UV is
//       the identity map.
//    7. Handedness.  A mirrored (left-handed) incoming frame is still
//       left-handed after the rebuild -- mirrors NormalMap's guarantee.
//   7b. SDF-heightfield frame (fix round 1, P1-A).  A hit carrying
//       `bShadingTangentFromGeometry` WITHOUT `bHasShadingTangent` --
//       SDFGeometry's heightfield mode, built here exactly as
//       Object::IntersectRay builds it (world-X projected into the
//       shading-normal plane, CreateFromWU, optional mirrored FlipV) --
//       is a COHERENT-tangent hit and must take the tangent-preserving
//       rebuild.  (i) a constant height leaves vNormal and the whole ONB
//       bit-identical; (ii) a gradient height leaves onb.u() equal to the
//       incoming u projected into the new tangent plane; (iii) the
//       mirrored variant stays left-handed.  (i) is asserted for BumpMap
//       and NormalMap too: they are the same family and carried the
//       identical gate bug (audit-by-bug-pattern).
//    8. Non-finite guard.  A height that returns NaN (and one that
//       returns +Inf) leaves the hit bit-for-bit untouched.
//    9. Stack order (design 8, item 9; ModifierStack).  (a) `normal_map`
//       then `relief` and `relief` then `normal_map` give DIFFERENT
//       vNormal, and each equals applying the two members by hand in
//       that order, bit-identical.  (b) a stack ending in
//       `glint_modifier` matches applying the prefix then Glint by hand.
//       (c) nesting: `stack{A, stack{B,C}}` is bit-identical to
//       `stack{A,B,C}`.  (d) a stack of one member is bit-identical to
//       the member alone.  (e) parse: a two-member `modifier_stack`
//       parses and registers; an empty stack is a parse-time error
//       naming the stack; an unknown member name diagnoses both the
//       stack and the missing member; a stack referencing another stack
//       parses.
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
//        negative control (the `riNull` block at the end of
//        Test5_ObjectSpaceExactness), not just performed by hand, since
//        the test can construct it honestly, TWICE -- once under the
//        rotation and once under the non-uniform scale, where the
//        un-transformed step is wrong per-axis.
//    (c) Test 8, NON-FINITE GUARD.  ReliefModifier has TWO finiteness
//        gates -- the explicit `isfinite(dT) || isfinite(dB)` early
//        return the design calls for, and the `mag2 > 1e-12 &&
//        isfinite(mag2)` degenerate-normal gate below it -- and the
//        red-proof MEASURED them to be mutually redundant for a
//        non-finite height: removing EITHER one alone leaves test 8
//        green (measured at the 59/59 suite total of the time; the
//        suite is 80/80 as of fix round 1), and only removing BOTH
//        fails it (6 failures, NaN/Inf normals reaching the frame
//        rebuild).  So test 8 pins
//        the BEHAVIOUR, not either specific line; do not read a green
//        test 8 as proof that the explicit guard is still present.  The
//        explicit guard is kept anyway: it is what the design specifies,
//        it states the intent at the point where the value is produced,
//        and it short-circuits before the perturbation arithmetic.
//    (d) Test 6, CHAIN RULE.  Dropping the ptCoord offset in the
//        surface branch (leaving ri2.ptCoord at the centre) makes the
//        UV-painter-in-surface-mode gradient identically zero and fails
//        test 6.
//    (e) Test 4, THE max(., fw) STEP RULE.  Deleting the
//        `if( txFootprint.valid && worldWidth > s ) s = worldWidth`
//        block in ReliefModifier.cpp's surface branch leaves the fbm
//        sweep GREEN -- `fbm` fades its own octaves against the
//        footprint, so its sequence is monotone with or without the max,
//        and that sweep alone would pass a broken implementation.  The
//        fw-blind step-height sweep added in fix round 1 fails all three
//        strict-decrease assertions (|N'-N| pinned at 1.4000 for every
//        footprint instead of 1.400 -> 1.268 -> 0.460 -> 0.050).
//    (f) Test 7b, THE FRAME-REBUILD GATE (fix round 1, P1-A).  Reverting
//        all three of ReliefModifier / BumpMap / NormalMap from
//        `ModifierFrame::HasCoherentTangent( ri )` back to
//        `ri.bHasShadingTangent` fails 5 assertions: 7b(i)'s ONB check
//        for ALL THREE modifiers, 7b(ii), and 7b(iii).  (7b(i)'s vNormal
//        checks stay green by construction -- the gate decides only how
//        the ONB is rebuilt, never the normal -- which is exactly why the
//        bug was invisible to a normal-only assertion.)
//    (g) Test 4c, THE worldWidth OBJECT-TO-WORLD FOLD (fix round 2,
//        P2-A).  Removing the `ri.geometric.txFootprint.worldWidth *=
//        m_worldLinearScale` fold added to Object::IntersectRay makes
//        test 4c's ratio read 1.0 (object units, unfolded) instead of
//        10.0 -- performed by hand (commented out the fold, rebuilt,
//        ran the suite, observed the failure), then reverted.
//    (h) Test 9, THE STACK APPLICATION ORDER (Phase 2).  Reversing the
//        iteration in ModifierStack::Modify (last member first, swapping
//        `members.begin()/end()` for `rbegin()/rend()`) fails 9a's two
//        hand-chain bit-identical comparisons (the stack now matches the
//        OPPOSITE hand-chained order) AND 9b's glint-last comparison, on
//        a probe point hunted (via GlintModifier::FindFacet) to actually
//        land on a facet -- a probe that misses is a no-op for Glint and
//        cannot discriminate order at all, which is exactly what the
//        FIRST attempt at this red-proof measured (9b stayed green under
//        the reversed build, for that reason, before the probe hunt was
//        added).  9c (nesting) is MEASURED NOT TO FAIL under this
//        mutation, and that is correct, not a test gap: reversing
//        applies to every level of nesting alike, so `stack{A,
//        stack{B,C}}` and `stack{A,B,C}` both reverse to the same
//        C-then-B-then-A application order and stay equal to each
//        other. A globally-consistent order reversal is a genuine
//        structural symmetry of flat composition; 9a and 9b are what
//        actually pins the AUTHORED order (against NormalMap and
//        GlintModifier, which are not order-symmetric the way a plain
//        reversal of a flat list is). Reverted after observing 9a/9b
//        fail (3 failures total) and the rest of the suite green.
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
#include <limits>
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

#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Interfaces/IFunction2D.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IModifierManager.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Modifiers/BumpMap.h"
#include "../src/Library/Modifiers/GlintModifier.h"
#include "../src/Library/Modifiers/ModifierStack.h"
#include "../src/Library/Modifiers/NormalMap.h"
#include "../src/Library/Modifiers/ReliefModifier.h"
#include "../src/Library/Objects/Object.h"
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

//! An fw-BLIND height: a step in world x, with no octave structure of its
//! own to fade.  Probed at x = 0 the central difference straddles the step
//! for ANY half-step s, so the measured slope is exactly 0.1 / (2s) -- a
//! pure readout of the differencing span, which is what test 4's second
//! sweep needs in order to discriminate the max(., fw) rule.
static Scalar HeightStepAtX( const RayIntersectionGeometric& ri )
{
	return ( ri.ptIntersection.x > 0 ) ? Scalar( 0.1 ) : Scalar( 0 );
}

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

	// ---- SECOND SWEEP: an fw-BLIND height, which is what actually
	// discriminates the `max(., fw)` step rule.
	//
	// The fbm sweep above does NOT: `fbm` fades its own octaves against
	// ri.txFootprint, so by fw >= 0.6 the field is already flat and the
	// sequence stays monotone even with the max deleted -- it would pass a
	// broken implementation.  A step height is blind to the footprint: it
	// has no octaves to fade, so the ONLY thing that can make its measured
	// slope shrink is the differencing span itself.
	//
	// H(p) = 0.1 for p.x > 0, else 0, probed at x = 0 exactly.  T = +X, so
	// the stencil straddles the step: dT = H(+s) - H(-s) = 0.1 for every
	// s > 0, and hT = 0.1 / (2s).  |N'-N| must therefore DECREASE strictly
	// as fw (hence s) grows.  Delete the max and s is pinned at the 1e-3
	// auto floor for all four widths, the sequence goes flat, and the
	// strict-decrease assertions below fail (red-proof (e)).
	{
		FnScalarPainter* hs = Own( new FnScalarPainter( &HeightStepAtX ) );
		ReliefModifier* ms = MakeRelief( *hs, Scalar(1.0), ReliefDomain::Surface, Scalar(0) );	// step 0 = auto

		Scalar dev[4] = { 0, 0, 0, 0 };
		for( int wi = 0; wi < 4; wi++ ) {
			RayIntersectionGeometric ri = MakeRI( Point3( 0, 0, 0 ) );
			ri.txFootprint.valid = true;
			ri.txFootprint.worldWidth = widths[wi];
			const Vector3 n0 = ri.vNormal;
			ms->Modify( ri );
			dev[wi] = Vector3Ops::Magnitude( ri.vNormal - n0 );
			std::cout << "    (step height) worldWidth " << std::scientific << std::setprecision(1) << widths[wi]
			          << "  |N'-N| " << std::setprecision(4) << dev[wi] << std::defaultfloat << std::endl;
		}

		CHECK( dev[0] > Scalar(1e-3),
			"4: (oracle) the fw-blind step height actually perturbs at the narrowest footprint (" << dev[0] << ")" );
		for( int wi = 1; wi < 4; wi++ ) {
			CHECK( dev[wi] < dev[wi-1],
				"4: (fw-blind height) |N'-N| STRICTLY decreases from worldWidth " << widths[wi-1]
				<< " to " << widths[wi] << " (" << dev[wi-1] << " -> " << dev[wi] << ")" );
		}
	}
}

// ============================================================
//  Test 4c: txFootprint.worldWidth's object-to-world fold
// ============================================================

//! Builds a single, flat, front-facing triangle (v0=(0,0,0), v1=(1,0,0),
//! v2=(0,1,0), constant normal (0,0,1), identity UV == position.xy) --
//! the simplest mesh that gives TriangleMeshGeometryIndexedSpecializations
//! a non-degenerate UV Jacobian, so ri.derivatives.valid and (with ray
//! differentials) ri.txFootprint.valid both come back true.
static Implementation::TriangleMeshGeometryIndexed* BuildUnitUVTriangle()
{
	VerticesListType verts;
	verts.push_back( Point3( 0, 0, 0 ) );
	verts.push_back( Point3( 1, 0, 0 ) );
	verts.push_back( Point3( 0, 1, 0 ) );

	NormalsListType norms;
	norms.push_back( Vector3( 0, 0, 1 ) );
	norms.push_back( Vector3( 0, 0, 1 ) );
	norms.push_back( Vector3( 0, 0, 1 ) );

	TexCoordsListType coords;
	coords.push_back( Point2( 0, 0 ) );
	coords.push_back( Point2( 1, 0 ) );
	coords.push_back( Point2( 0, 1 ) );

	IndexTriangleListType tris;
	IndexedTriangle tri;
	tri.iVertices[0] = 0; tri.iVertices[1] = 1; tri.iVertices[2] = 2;
	tri.iNormals[0]  = 0; tri.iNormals[1]  = 1; tri.iNormals[2]  = 2;
	tri.iCoords[0]   = 0; tri.iCoords[1]   = 1; tri.iCoords[2]   = 2;
	tris.push_back( tri );

	Implementation::TriangleMeshGeometryIndexed* mesh =
		new Implementation::TriangleMeshGeometryIndexed( true, true );
	mesh->BeginIndexedTriangles();
	mesh->AddVertices( verts );
	mesh->AddNormals( norms );
	mesh->AddTexCoords( coords );
	mesh->AddIndexedTriangles( tris );
	mesh->DoneIndexedTriangles();
	return mesh;
}

static void Test4c_FootprintWorldScaleFold()
{
	std::cout << "Test 4c: txFootprint.worldWidth folds by the object's world scale" << std::endl;

	// A well-conditioned interior hit (not on an edge or vertex): local
	// origin (0.2, 0.2, 10), direction straight down -Z, lands at
	// (0.2, 0.2, 0), which is inside the triangle (0.2+0.2 = 0.4 < 1).
	const Point3 localOrigin( 0.2, 0.2, 10.0 );
	const Vector3 localDir( 0, 0, -1 );
	// Small, arbitrary pixel-footprint direction offsets -- their exact
	// magnitude doesn't matter, only that they are IDENTICAL between the
	// scale-1 and scale-10 casts below (see the comment on why that makes
	// this an exact, not approximate, ratio check).
	const Vector3 rxDirOffset( 0.02, 0, 0 );
	const Vector3 ryDirOffset( 0, 0.02, 0 );

	Scalar worldWidthScale1 = 0, worldWidthScale10 = 0;
	bool validScale1 = false, validScale10 = false;

	// ---- scale 1 (identity) ----
	{
		Implementation::TriangleMeshGeometryIndexed* mesh = BuildUnitUVTriangle();
		Implementation::Object* o = new Implementation::Object( mesh );
		mesh->release();
		o->FinalizeTransformations();

		Ray ray( localOrigin, localDir );
		ray.diffs.rxDir = rxDirOffset;
		ray.diffs.ryDir = ryDirOffset;
		ray.hasDifferentials = true;

		RayIntersection ri( ray, nullRasterizerState );
		o->IntersectRay( ri, RISE_INFINITY, true, true, false );
		CHECK( ri.geometric.bHit, "4c: scale-1 ray hits the triangle" );
		validScale1 = ri.geometric.txFootprint.valid;
		worldWidthScale1 = ri.geometric.txFootprint.worldWidth;
		o->release();
	}

	// ---- scale 10 (uniform) ----
	// SAME local hit point: Object::IntersectRay inverse-transforms the
	// world ray by (1/10)*I, so a world ray of (10*localOrigin, localDir)
	// recovers EXACTLY localOrigin/localDir in object space, landing on
	// the identical triangle point.  The direction-diff reconstruction
	// (aux = normalize(d + rxDir), then re-normalize through the inverse
	// transform) is invariant to an overall positive scalar on a UNIFORM
	// scale with no rotation, so passing the SAME rxDir/ryDir offsets in
	// world space here reproduces the SAME object-space diffs as the
	// scale-1 cast above -- i.e. the RAW (pre-fold) worldWidth this
	// geometry/UV pair produces is expected to be identical at both
	// scales.  The only thing that should differ is the object-to-world
	// fold this test exists to check.
	{
		const Scalar s = 10.0;
		Implementation::TriangleMeshGeometryIndexed* mesh = BuildUnitUVTriangle();
		Implementation::Object* o = new Implementation::Object( mesh );
		mesh->release();
		o->SetScale( s );
		o->FinalizeTransformations();

		Ray ray( Point3( localOrigin.x * s, localOrigin.y * s, localOrigin.z * s ), localDir );
		ray.diffs.rxDir = rxDirOffset;
		ray.diffs.ryDir = ryDirOffset;
		ray.hasDifferentials = true;

		RayIntersection ri( ray, nullRasterizerState );
		o->IntersectRay( ri, RISE_INFINITY, true, true, false );
		CHECK( ri.geometric.bHit, "4c: scale-10 ray hits the triangle" );
		validScale10 = ri.geometric.txFootprint.valid;
		worldWidthScale10 = ri.geometric.txFootprint.worldWidth;
		o->release();
	}

	CHECK( validScale1 && validScale10, "4c: both casts populated a valid txFootprint" );
	CHECK( worldWidthScale1 > Scalar( 1e-6 ), "4c: (oracle) scale-1 worldWidth is non-degenerate (" << worldWidthScale1 << ")" );

	const Scalar ratio = (worldWidthScale1 > 0) ? (worldWidthScale10 / worldWidthScale1) : Scalar(0);
	CHECK( std::fabs( ratio - Scalar( 10 ) ) < Scalar( 1e-9 ),
		"4c: worldWidth(scale 10) / worldWidth(scale 1) == 10, the object-to-world fold ("
		<< std::setprecision(12) << ratio << ")" );
}

// ============================================================
//  Test 5: object-space exactness under a rotated instance
// ============================================================

static void Test5_ObjectSpaceExactness()
{
	std::cout << "Test 5: object-space height fields (rotated, then non-uniformly scaled) match their world-space equivalents" << std::endl;

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

	// NON-UNIFORM SCALE.  The field's doc comment claims the object-space
	// step is the world step through the LINEAR map, and that under a
	// non-uniform scale the object-space step is deliberately not the
	// world step's length -- because the field lives in object space and
	// the difference must span the object-space distance the world step
	// actually covers.  A rotation cannot discriminate that claim (it
	// preserves lengths), so pin it with an anisotropic scale, where a
	// naive "move by the world step" would be wrong per-axis.
	{
		// Po = (3*Px, 0.5*Py, 2*Pz).
		Matrix4 S2O = Matrix4Ops::Identity();
		S2O._00 = 3.0;  S2O._11 = 0.5;  S2O._22 = 2.0;

		IScalarPainter* hObjS = MakeExprScalar( "1.5*Po.x - 0.8*Po.y" );
		// Substituting Po: H_w(P) = 1.5*3*Px - 0.8*0.5*Py.
		IScalarPainter* hWorldS = MakeExprScalar( "4.5*P.x - 0.4*P.y" );
		if( hObjS && hWorldS ) {
			ReliefModifier* mo = MakeRelief( *hObjS,   Scalar(0.2), ReliefDomain::Surface, Scalar(1e-3) );
			ReliefModifier* mw = MakeRelief( *hWorldS, Scalar(0.2), ReliefDomain::Surface, Scalar(1e-3) );

			const Point3 Q( 0.2, 0.5, -0.3 );
			RayIntersectionGeometric riO = MakeRI( Q );
			riO.ptObjIntersec = Point3Ops::Transform( S2O, Q );
			riO.pmxWorldToObject = &S2O;
			mo->Modify( riO );

			RayIntersectionGeometric riW = MakeRI( Q );
			mw->Modify( riW );

			CHECK( VecClose( riO.vNormal, riW.vNormal, 1e-9 ),
				"5: an object-space field under NON-UNIFORM scale matches its world-space equivalent" );
			CHECK( !VecClose( riO.vNormal, Vector3( 0, 0, 1 ), 1e-6 ),
				"5: (oracle) the non-uniform-scale field actually perturbs" );

			// And the negative: moving the object point by the un-scaled
			// world step gives a materially different answer, so the
			// per-axis scaling is load-bearing, not cosmetic.
			RayIntersectionGeometric riNoS = MakeRI( Q );
			riNoS.ptObjIntersec = Point3Ops::Transform( S2O, Q );
			riNoS.pmxWorldToObject = 0;
			mo->Modify( riNoS );
			CHECK( !VecClose( riNoS.vNormal, riW.vNormal, 1e-6 ),
				"5: RED-PROOF -- under non-uniform scale the un-transformed step is "
				"measurably wrong (the per-axis map is load-bearing)" );
		}
	}
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
		// negative-determinant instance.  Both flags set together (fix
		// round 2, P2-C): no in-tree geometry sets bHasShadingTangent
		// without also setting bShadingTangentFromGeometry (HairGeometry,
		// the analytic primitives, and the mesh UV-tangent producers all
		// set both -- grep `bShadingTangentFromGeometry\s*=\|
		// bHasShadingTangent\s*=` across src/Library/Geometry), so
		// bHasShadingTangent alone modelled an impossible combination; this
		// matches the real hair/mesh case the test means to probe.
		ri.bShadingTangentFromGeometry = true;
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
		// Both flags set together -- fix round 2, P2-C, same reasoning as
		// the loop above.
		ri.bShadingTangentFromGeometry = true;
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
//  Test 7b: the SDF-heightfield frame (bShadingTangentFromGeometry
//           WITHOUT bHasShadingTangent)
// ============================================================

//! Build the hit an SDFGeometry heightfield produces, exactly as
//! Object::IntersectRay does it (src/Library/Objects/Object.cpp:699-816):
//! the coherent-frame branch keys on `bShadingTangentFromGeometry`, and
//! inside it the "no real supplied tangent" sub-case projects WORLD-X
//! into the shading-normal plane and calls CreateFromWU.  SDFGeometry's
//! heightfield mode sets `bShadingTangentFromGeometry` and NOT
//! `bHasShadingTangent` (SDFGeometry.cpp, the `m_isHeightfield` branch of
//! IntersectRay; the pairing is spelled out in the `vShadingTangent` /
//! `bHasShadingTangent` field comment in RayIntersectionGeometric.h),
//! so this is the flag combination a modifier must not mistake for a
//! tangent-less hit.
static RayIntersectionGeometric MakeSDFHeightfieldRI( const Point3& p, bool mirrored )
{
	RayIntersectionGeometric ri = MakeRI( p );

	const Vector3& n = ri.vNormal;
	Vector3 t( 1.0 - n.x*n.x, -n.x*n.y, -n.x*n.z );			// (1,0,0) - n*dot(n,(1,0,0))
	if( Vector3Ops::SquaredModulus( t ) < NEARZERO ) {
		t = Vector3( -n.y*n.x, 1.0 - n.y*n.y, -n.y*n.z );	// (0,1,0) - n*dot(n,(0,1,0))
	}
	ri.onb.CreateFromWU( n, t );

	// The mirrored-instance correction Object::IntersectRay applies when
	// m_tangentFrameSign < 0 -- it lands on THIS branch too, so a
	// modifier that drops to CreateFromW here loses it.
	if( mirrored ) {
		ri.onb.FlipV();
	}

	ri.bShadingTangentFromGeometry = true;
	ri.bHasShadingTangent          = false;		// the discriminating combination
	return ri;
}

//! A constant height: gradient identically zero in every direction, so a
//! correct modifier is a pure no-op on the frame.
static Scalar HeightConstant( const RayIntersectionGeometric& ) { return Scalar( 0.375 ); }

//! A trivial constant IFunction2D, BumpMap's zero-gradient input.
namespace {
class ConstFunction2D :
	public virtual IFunction2D,
	public virtual Reference
{
public:
	explicit ConstFunction2D( Scalar v ) : m_v( v ) {}
	Scalar Evaluate( const Scalar, const Scalar ) const override { return m_v; }
protected:
	virtual ~ConstFunction2D() {}
private:
	Scalar m_v;
};
} // anonymous namespace

static void Test7b_SDFHeightfieldFrame()
{
	std::cout << "Test 7b: an SDF-heightfield hit (bShadingTangentFromGeometry, no bHasShadingTangent) keeps its coherent frame" << std::endl;

	const Point3 probe( 0.0, 0.0, 0.0 );

	// ---- Precondition: the fixture really is the coherent-frame case,
	// and its axes really are the ones CreateFromW would ROTATE BY 180.
	{
		RayIntersectionGeometric ri = MakeSDFHeightfieldRI( probe, false );
		CHECK( ri.bShadingTangentFromGeometry && !ri.bHasShadingTangent,
			"7b: (setup) the fixture carries bShadingTangentFromGeometry WITHOUT bHasShadingTangent" );
		CHECK( VecClose( ri.onb.u(), Vector3( 1, 0, 0 ), 1e-15 )
		    && VecClose( ri.onb.v(), Vector3( 0, 1, 0 ), 1e-15 ),
			"7b: (setup) Object::IntersectRay's world-X projection gives u = +X, v = +Y" );

		OrthonormalBasis3D legacy;
		legacy.CreateFromW( ri.vNormal );
		CHECK( !VecClose( legacy.u(), ri.onb.u(), 1e-9 ),
			"7b: (oracle) CreateFromW does NOT reproduce this frame -- it is the 180-degree rotation "
			"the gate exists to avoid (u " << legacy.u().x << "," << legacy.u().y << "," << legacy.u().z << ")" );
	}

	// ---- (i) CONSTANT height: zero gradient, so vNormal AND the whole
	// ONB must come back bit-identical.  This is the assertion the old
	// `bHasShadingTangent` gate fails: it would re-derive the frame with
	// CreateFromW and hand back u = -X, v = -Y.
	{
		FnScalarPainter* h = Own( new FnScalarPainter( &HeightConstant ) );
		ReliefModifier* m = MakeRelief( *h, Scalar(1.0), ReliefDomain::Surface, Scalar(1e-3) );

		RayIntersectionGeometric ri = MakeSDFHeightfieldRI( probe, false );
		const Vector3 n0 = ri.vNormal, u0 = ri.onb.u(), v0 = ri.onb.v(), w0 = ri.onb.w();
		m->Modify( ri );

		CHECK( VecClose( ri.vNormal, n0, 0 ),
			"7b(i): relief -- a constant height leaves vNormal bit-identical" );
		CHECK( VecClose( ri.onb.u(), u0, 0 ) && VecClose( ri.onb.v(), v0, 0 ) && VecClose( ri.onb.w(), w0, 0 ),
			"7b(i): relief -- a constant height leaves the whole ONB bit-identical" );
	}

	// ---- (i) SIBLINGS.  BumpMap and NormalMap carried the identical
	// bug (audit-by-bug-pattern: same gate, same family), so the same
	// zero-perturbation invariant is pinned for both.
	{
		ConstFunction2D* f = Own( new ConstFunction2D( Scalar(0.375) ) );
		BumpMap* m = Own( new BumpMap( *f, 1.0, 0.05, false ) );

		RayIntersectionGeometric ri = MakeSDFHeightfieldRI( probe, false );
		const Vector3 n0 = ri.vNormal, u0 = ri.onb.u(), v0 = ri.onb.v(), w0 = ri.onb.w();
		m->Modify( ri );

		CHECK( VecClose( ri.vNormal, n0, 0 ),
			"7b(i): bump_map -- a constant height leaves vNormal bit-identical" );
		CHECK( VecClose( ri.onb.u(), u0, 0 ) && VecClose( ri.onb.v(), v0, 0 ) && VecClose( ri.onb.w(), w0, 0 ),
			"7b(i): bump_map -- a constant height leaves the whole ONB bit-identical" );
	}
	{
		// (0.5, 0.5, 1.0) decodes to the identity tangent-space normal
		// (0, 0, 1): nx = ny = 0, nz = 1, so the perturbed normal is N.
		UniformColorPainter* p = Own( new UniformColorPainter( RISEPel( 0.5, 0.5, 1.0 ) ) );
		NormalMap* m = Own( new NormalMap( *p, 1.0 ) );

		RayIntersectionGeometric ri = MakeSDFHeightfieldRI( probe, false );
		const Vector3 n0 = ri.vNormal, u0 = ri.onb.u(), v0 = ri.onb.v(), w0 = ri.onb.w();
		m->Modify( ri );

		CHECK( VecClose( ri.vNormal, n0, 0 ),
			"7b(i): normal_map -- an identity normal-map texel leaves vNormal bit-identical" );
		CHECK( VecClose( ri.onb.u(), u0, 0 ) && VecClose( ri.onb.v(), v0, 0 ) && VecClose( ri.onb.w(), w0, 0 ),
			"7b(i): normal_map -- an identity normal-map texel leaves the whole ONB bit-identical" );
	}

	// ---- (ii) GRADIENT height: the rebuilt u must be the incoming u
	// PROJECTED into the new tangent plane and renormalized -- the
	// defining property of the tangent-preserving rebuild.
	{
		FnScalarPainter* h = Own( new FnScalarPainter( &HeightPx ) );
		ReliefModifier* m = MakeRelief( *h, Scalar(1.0), ReliefDomain::Surface, Scalar(1e-3) );

		RayIntersectionGeometric ri = MakeSDFHeightfieldRI( probe, false );
		const Vector3 u0 = ri.onb.u();
		m->Modify( ri );

		const Vector3 nNew = ri.vNormal;
		const Vector3 want = Vector3Ops::Normalize( u0 - nNew * Vector3Ops::Dot( u0, nNew ) );

		CHECK( !VecClose( nNew, Vector3( 0, 0, 1 ), 1e-6 ),
			"7b(ii): (oracle) the gradient height actually perturbed the normal" );
		CHECK( VecClose( ri.onb.u(), want, 1e-12 ),
			"7b(ii): onb.u() is the incoming u projected into the new tangent plane" );
		CHECK( std::fabs( Vector3Ops::Dot( ri.onb.w(), nNew ) - 1.0 ) < 1e-12,
			"7b(ii): onb.w() is the perturbed normal" );
	}

	// ---- (iii) MIRRORED: the FlipV Object::IntersectRay applied on this
	// same branch must survive the rebuild, i.e. the frame stays
	// LEFT-handed.  CreateFromWU always emits a right-handed triple, so a
	// gate that drops to CreateFromW (or that rebuilds without restoring
	// handedness) flips it back.
	{
		FnScalarPainter* h = Own( new FnScalarPainter( &HeightPx ) );
		ReliefModifier* m = MakeRelief( *h, Scalar(0.6), ReliefDomain::Surface, Scalar(1e-3) );

		RayIntersectionGeometric ri = MakeSDFHeightfieldRI( probe, true );
		const Scalar before = Vector3Ops::Dot( ri.onb.u(),
			Vector3Ops::Cross( ri.onb.v(), ri.onb.w() ) );
		CHECK( before < 0, "7b(iii): (setup) the mirrored fixture is left-handed" );

		m->Modify( ri );

		const Scalar after = Vector3Ops::Dot( ri.onb.u(),
			Vector3Ops::Cross( ri.onb.v(), ri.onb.w() ) );
		CHECK( after < 0,
			"7b(iii): the mirrored SDF-heightfield frame is still left-handed after the rebuild" );
	}
}

// ============================================================
//  Test 8: the non-finite guard
// ============================================================

// The NaN and Inf below are the test's INPUT -- the hostile height field
// whose handling is the thing under test -- not a not-found sentinel
// returned in place of a real value, which is the disease
// SourceHygieneTest's scanner exists to catch.  A "finite poison" cannot
// substitute here: the modifier's contract is specifically about
// NON-FINITE differences, and a finite spike would exercise the
// degenerate-normal gate instead.  That these values really are
// non-finite at runtime on this build is not assumed either -- red-proof
// (c) removed both guards and observed actual NaN/Inf normals reach the
// frame rebuild (6 failures), which they could not have done if the
// literals had been folded away.
static Scalar HeightNaN( const RayIntersectionGeometric& ri )
{
	// NaN only OFF-centre, so the guard has to catch it in the stencil
	// rather than at the shading point.
	return ( ri.ptIntersection.x > 0.5 ) ? std::nan( "" ) : Scalar( 1.0 );  // HYGIENE-OK: hostile INPUT to the modifier's non-finite guard, not a not-found sentinel
}
static Scalar HeightInf( const RayIntersectionGeometric& )
{
	return std::numeric_limits<Scalar>::infinity();  // HYGIENE-OK: hostile INPUT to the modifier's non-finite guard, not a not-found sentinel
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
//  Test 9 + Test 10: modifier_stack, and the scene chunk
//  (shared parse-capture helpers -- Test 9's (e) parse cases reuse them,
//  per design 8's instruction to reuse ReliefModifierTest's
//  ParseCapturing helper rather than duplicate it in a new file)
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

// ============================================================
//  Test 9: modifier_stack -- order, glint-last, nesting, singleton, parse
// ============================================================

static void Test9_StackOrder()
{
	std::cout << "Test 9: modifier_stack applies members in authored order (docs/RELIEF_MODIFIER_DESIGN.md section 4)" << std::endl;

	// ---- (a) ORDER.  `normal_map` then `relief` and `relief` then
	// `normal_map` give DIFFERENT vNormal, and each is bit-identical to
	// applying the two members by hand in that order -- ModifierStack
	// adds no behaviour of its own beyond "call each Modify in order".
	{
		// (0.6, 0.5, 0.9) decodes to a non-trivial tangent-space tilt
		// (2*0.6-1, 2*0.5-1, 2*0.9-1) = (0.2, 0.0, 0.8) -- a real
		// NormalMap fixture, not a stub, per design 8's instruction.
		UniformColorPainter* pTilt = Own( new UniformColorPainter( RISEPel( 0.6, 0.5, 0.9 ) ) );
		NormalMap* nm = Own( new NormalMap( *pTilt, Scalar(1.0) ) );
		FnScalarPainter* h = Own( new FnScalarPainter( &HeightPx ) );
		ReliefModifier* rel = MakeRelief( *h, Scalar(0.3), ReliefDomain::Surface, Scalar(1e-3) );

		const IRayIntersectionModifier* nmThenRel[2] = { nm, rel };
		const IRayIntersectionModifier* relThenNm[2] = { rel, nm };
		ModifierStack* stackNmRel = Own( new ModifierStack( nmThenRel, 2 ) );
		ModifierStack* stackRelNm = Own( new ModifierStack( relThenNm, 2 ) );

		RayIntersectionGeometric riNmRel = MakeRI( Point3( 0.4, 0, 0 ) );
		RayIntersectionGeometric riRelNm = MakeRI( Point3( 0.4, 0, 0 ) );
		stackNmRel->Modify( riNmRel );
		stackRelNm->Modify( riRelNm );

		CHECK( !VecClose( riNmRel.vNormal, riRelNm.vNormal, 1e-6 ),
			"9a: normal_map->relief and relief->normal_map give DIFFERENT vNormal" );

		RayIntersectionGeometric refNmRel = MakeRI( Point3( 0.4, 0, 0 ) );
		nm->Modify( refNmRel );
		rel->Modify( refNmRel );
		CHECK( VecClose( riNmRel.vNormal, refNmRel.vNormal, 0 )
		    && VecClose( riNmRel.onb.u(), refNmRel.onb.u(), 0 )
		    && VecClose( riNmRel.onb.v(), refNmRel.onb.v(), 0 )
		    && VecClose( riNmRel.onb.w(), refNmRel.onb.w(), 0 ),
			"9a: stack{normal_map, relief} is bit-identical to normal_map.Modify() then relief.Modify() by hand" );

		RayIntersectionGeometric refRelNm = MakeRI( Point3( 0.4, 0, 0 ) );
		rel->Modify( refRelNm );
		nm->Modify( refRelNm );
		CHECK( VecClose( riRelNm.vNormal, refRelNm.vNormal, 0 )
		    && VecClose( riRelNm.onb.u(), refRelNm.onb.u(), 0 )
		    && VecClose( riRelNm.onb.v(), refRelNm.onb.v(), 0 )
		    && VecClose( riRelNm.onb.w(), refRelNm.onb.w(), 0 ),
			"9a: stack{relief, normal_map} is bit-identical to relief.Modify() then normal_map.Modify() by hand" );
	}

	// ---- (b) GLINT-LAST.  A stack ending in glint_modifier matches
	// applying the prefix then Glint by hand.  GlintModifier::Modify is a
	// deterministic pure function of the object-space hit + its
	// parameters (design 5.1's "object-space-stability contract"), so
	// this holds whether or not the probe point happens to land on a
	// facet.  It is still hunted for a real hit below: a probe that
	// MISSES every facet leaves Glint a no-op, and a no-op modifier
	// cannot discriminate ORDER at all (measured in red-proof (h) --
	// the very first probe tried there happened to miss, and the
	// "glint-last" comparison stayed green under a REVERSED apply order
	// for that reason, not because the order was actually honoured).
	{
		FnScalarPainter* h = Own( new FnScalarPainter( &HeightPx ) );
		ReliefModifier* rel = MakeRelief( *h, Scalar(0.25), ReliefDomain::Surface, Scalar(1e-3) );
		GlintModifier* gl = Own( new GlintModifier(
			Scalar(5.0), Scalar(0.5), Scalar(0.6), Scalar(5.0),
			Vector3( 1, 1, 1 ), Vector3( 0, 0, 0 ), 42u ) );

		Point3 probe( 0.7, 0.2, 0 );
		bool foundFacet = false;
		for( int ix = 0; ix < 30 && !foundFacet; ix++ ) {
			for( int iy = 0; iy < 30 && !foundFacet; iy++ ) {
				const Point3 cand( ix * 0.1, iy * 0.1, 0 );
				if( gl->FindFacet( cand ).found ) { probe = cand; foundFacet = true; }
			}
		}
		CHECK( foundFacet, "9b: (setup) a probe point exists where glint_modifier actually finds a facet" );

		const IRayIntersectionModifier* relThenGlint[2] = { rel, gl };
		ModifierStack* stack = Own( new ModifierStack( relThenGlint, 2 ) );

		RayIntersectionGeometric riStack = MakeRI( probe );
		stack->Modify( riStack );

		RayIntersectionGeometric riRef = MakeRI( probe );
		rel->Modify( riRef );
		gl->Modify( riRef );

		CHECK( VecClose( riStack.vNormal, riRef.vNormal, 0 )
		    && VecClose( riStack.onb.u(), riRef.onb.u(), 0 )
		    && VecClose( riStack.onb.v(), riRef.onb.v(), 0 )
		    && VecClose( riStack.onb.w(), riRef.onb.w(), 0 ),
			"9b: stack{relief, glint} is bit-identical to relief.Modify() then glint.Modify() by hand" );
	}

	// ---- (c) NESTING.  stack{A, stack{B,C}} is bit-identical to
	// stack{A,B,C} -- algebraically flat composition, design section 4.
	{
		FnScalarPainter* hA = Own( new FnScalarPainter( &HeightPx ) );
		ReliefModifier* A = MakeRelief( *hA, Scalar(0.2), ReliefDomain::Surface, Scalar(1e-3) );
		UniformColorPainter* pTiltB = Own( new UniformColorPainter( RISEPel( 0.7, 0.4, 0.85 ) ) );
		NormalMap* B = Own( new NormalMap( *pTiltB, Scalar(1.0) ) );
		GlintModifier* C = Own( new GlintModifier(
			Scalar(5.0), Scalar(0.5), Scalar(0.6), Scalar(5.0),
			Vector3( 1, 1, 1 ), Vector3( 0, 0, 0 ), 7u ) );

		const IRayIntersectionModifier* membersBC[2] = { B, C };
		ModifierStack* stackBC = Own( new ModifierStack( membersBC, 2 ) );
		const IRayIntersectionModifier* membersA_BC[2] = { A, stackBC };
		ModifierStack* nested = Own( new ModifierStack( membersA_BC, 2 ) );

		const IRayIntersectionModifier* membersABC[3] = { A, B, C };
		ModifierStack* flat = Own( new ModifierStack( membersABC, 3 ) );

		RayIntersectionGeometric riNested = MakeRI( Point3( 0.3, -0.5, 0 ) );
		RayIntersectionGeometric riFlat   = MakeRI( Point3( 0.3, -0.5, 0 ) );
		nested->Modify( riNested );
		flat->Modify( riFlat );

		CHECK( VecClose( riNested.vNormal, riFlat.vNormal, 0 )
		    && VecClose( riNested.onb.u(), riFlat.onb.u(), 0 )
		    && VecClose( riNested.onb.v(), riFlat.onb.v(), 0 )
		    && VecClose( riNested.onb.w(), riFlat.onb.w(), 0 ),
			"9c: stack{A, stack{B,C}} is bit-identical to stack{A,B,C}" );
	}

	// ---- (d) SINGLETON.  A stack of one member is bit-identical to the
	// member alone -- the wrapper adds no side effects.
	{
		FnScalarPainter* h = Own( new FnScalarPainter( &HeightPx ) );
		ReliefModifier* rel = MakeRelief( *h, Scalar(0.4), ReliefDomain::Surface, Scalar(1e-3) );
		const IRayIntersectionModifier* one[1] = { rel };
		ModifierStack* singleton = Own( new ModifierStack( one, 1 ) );

		RayIntersectionGeometric riStack = MakeRI( Point3( -0.2, 0.6, 0 ) );
		RayIntersectionGeometric riAlone = MakeRI( Point3( -0.2, 0.6, 0 ) );
		singleton->Modify( riStack );
		rel->Modify( riAlone );

		CHECK( VecClose( riStack.vNormal, riAlone.vNormal, 0 )
		    && VecClose( riStack.onb.u(), riAlone.onb.u(), 0 )
		    && VecClose( riStack.onb.v(), riAlone.onb.v(), 0 )
		    && VecClose( riStack.onb.w(), riAlone.onb.w(), 0 ),
			"9d: a one-member stack is bit-identical to that member alone" );
	}

	// ---- (e) PARSE.  A two-member modifier_stack parses and registers;
	// an empty stack is a parse-time error naming the stack; an unknown
	// member name diagnoses both the stack and the missing member; a
	// stack referencing another stack parses.
	{
		IJobPriv* job = 0;
		RISE_CreateJobPriv( &job );
		CHECK( job != 0, "9e: job created" );
		if( job ) {
			std::string log;
			const bool ok = ParseCapturing( "stack_ok",
				"scalar_painter\n{\n\tname h9\n\texpression 0.1*P.x\n}\n"
				"relief_modifier\n{\n\tname r9a\n\theight h9\n\tscale 0.2\n}\n"
				"glint_modifier\n{\n\tname g9b\n}\n"
				"modifier_stack\n{\n\tname finish9\n\tmodifier r9a\n\tmodifier g9b\n}\n",
				*job, log );
			CHECK( ok, "9e: the scene loads" );
			CHECK( job->GetModifiers()->GetItem( "finish9" ) != 0,
				"9e: `finish9` (two members) is registered in the modifier manager" );
		}
		safe_release( job );
	}
	{
		IJobPriv* job = 0;
		RISE_CreateJobPriv( &job );
		if( job ) {
			std::string log;
			ParseCapturing( "stack_empty",
				"modifier_stack\n{\n\tname empty9\n}\n",
				*job, log );
			CHECK( Contains( log, "modifier_stack `empty9`" ) && Contains( log, "empty stack is refused" ),
				"9e: an empty modifier_stack is a parse-time error naming the stack" );
			CHECK( job->GetModifiers()->GetItem( "empty9" ) == 0,
				"9e: the empty stack is NOT registered" );
		}
		safe_release( job );
	}
	{
		IJobPriv* job = 0;
		RISE_CreateJobPriv( &job );
		if( job ) {
			std::string log;
			ParseCapturing( "stack_unknown",
				"modifier_stack\n{\n\tname bad9\n\tmodifier nosuchmodifier\n}\n",
				*job, log );
			CHECK( Contains( log, "modifier_stack `bad9`" ) && Contains( log, "`nosuchmodifier`" ) && Contains( log, "not found" ),
				"9e: an unknown member name diagnoses BOTH the stack `bad9` and the missing member `nosuchmodifier`" );
			CHECK( job->GetModifiers()->GetItem( "bad9" ) == 0,
				"9e: the stack with an unknown member is NOT registered" );
		}
		safe_release( job );
	}
	{
		IJobPriv* job = 0;
		RISE_CreateJobPriv( &job );
		if( job ) {
			std::string log;
			const bool ok = ParseCapturing( "stack_of_stack",
				"glint_modifier\n{\n\tname g9inner\n}\n"
				"modifier_stack\n{\n\tname inner9\n\tmodifier g9inner\n}\n"
				"modifier_stack\n{\n\tname outer9\n\tmodifier inner9\n}\n",
				*job, log );
			CHECK( ok, "9e: a stack referencing another stack parses" );
			CHECK( job->GetModifiers()->GetItem( "outer9" ) != 0,
				"9e: the nested stack `outer9` is registered" );
		}
		safe_release( job );
	}
}

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
	std::cout << "=== ReliefModifierTest (docs/RELIEF_MODIFIER_DESIGN.md 8, tests 1-10 + 4c + 7b) ===" << std::endl;

	Test1_AnalyticGradient();
	Test2_LegacyEquivalence();
	Test3_NoTexcoords();
	Test4_FootprintFade();
	Test4c_FootprintWorldScaleFold();
	Test5_ObjectSpaceExactness();
	Test6_UVChainRule();
	Test7_Handedness();
	Test7b_SDFHeightfieldFrame();
	Test8_NonFiniteGuard();
	Test9_StackOrder();
	Test10_Parse();

	ReleaseOwned();

	std::cout << std::endl;
	std::cout << g_passes << " passed, " << g_failures << " failed." << std::endl;
	if( g_failures == 0 ) std::cout << "=== ALL TESTS PASSED ===" << std::endl;
	return g_failures == 0 ? 0 : 1;
}
