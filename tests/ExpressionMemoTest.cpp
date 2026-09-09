//////////////////////////////////////////////////////////////////////
//
//  ExpressionMemoTest.cpp - the correctness gate for the two-level
//  per-hit expression memo (src/Library/Utilities/ExpressionMemo.h,
//  and its own header comment).
//
//  A memo is only ever allowed to be invisible.  Every check here is
//  therefore either a DIFFERENTIAL (memo on == memo off) or a
//  RED-PROOF (with the safeguard removed the WRONG answer really does
//  come back, so the check is not tautological).
//
//    (a) DIFFERENTIAL EQUALITY over 10 000+ randomized contexts,
//        including live `signals` from a real SDF geometry, on BOTH
//        painter pipes and through the spectral GetColorNM path.  Bit
//        equality, not a tolerance: a memo hit returns a stored double,
//        so anything but bit equality is a bug.
//    (b) GENERATION RED-PROOF.  Fill the memo, change what the provider
//        answers, and show BOTH halves: WITHOUT the bump the stale value
//        really is returned (so the memo is genuinely caching and the
//        test is not vacuous), and WITH the bump the fresh one is.
//    (c) PROGRAM-ID RED-PROOF.  Destroy a compiled program, compile a
//        different one, and show the second gets its own answer at a
//        byte-identical context.  Includes the address-reuse case the id
//        exists for: a `this`-keyed memo would serve the dead program's
//        value.
//    (d) THREAD ISOLATION.  Eight threads, eight distinct evaluation
//        sequences, every answer its own.
//    (e) KILL SWITCH.  With `expression_memo` off the memo is never
//        consulted -- red-proved the same way (b) is: the stale-provider
//        trick returns STALE with it on and FRESH with it off.
//    (g) THE COMPILE-TIME GATE.  A cheap body is excluded, a noise or
//        signal body is not, and an excluded one still evaluates
//        correctly.
//    (h) `time` IN THE KEY.  Two painters over ONE compiled program,
//        differing only in their keyframable `time`, must not share an
//        entry -- the one key field neither painter pipe lets the (a)
//        sweep vary.
//    (f) L1 SINGLE-SOURCING.  A refusal and a null provider still read
//        their documented neutral through the memo, and a memoised
//        neutral never leaks across signal kinds (whose neutrals differ:
//        occlusion 1, thickness 1, convexity 0).
//    (i) THE MESH FAMILY'S KEY FIELDS.  A live TriangleMeshGeometryIndexed
//        bake -- the one provider whose answer is a function of
//        (primId, baryA, baryB) and of nothing else -- read through the
//        memo, and two records differing ONLY in those three fields kept
//        apart.  Every OTHER check in this file carries primId == -1, so
//        without this one the three fields could leave
//        SurfaceSignalInfo::MemoHitKey() unnoticed.
//    (j) THE SHIPPED DEFAULT.  Everything above runs under an explicit
//        MemoSwitch, which never consults the option at all; this one
//        reads the real thing -- ON by default, OFF from an options file
//        -- across two processes, because both the option read and
//        EnabledSlow()'s cache are once-per-process.
//        THE ONLY CHECK HERE THAT TOUCHES THE MACHINE: it writes two
//        files into a private mkdtemp/GetTempPath directory (never the
//        CWD), sets and then RESTORES RISE_OPTIONS_FILE, and SPAWNS A
//        CHILD PROCESS via std::system re-running this same binary.  It
//        reports SKIP -- not FAIL -- when the environment cannot support
//        that (no temp dir, unwritable file, no shell).  Its `_WIN32`
//        branch is UNCOMPILED; see the note at the function and in
//        tests/README.md.
//    (k) L2: EVERY KEY FIELD SEPARATES, one at a time.  (a)'s randomized
//        sweep looks like it covers this and does not.  Warms at a base
//        context, perturbs exactly one field, and requires the perturbed
//        context's own memo-off answer.  All fifteen record-reachable
//        fields (u, v, P.xyz, Po.xyz, N.xyz, fw, fwo, curv, curvR) plus
//        the signal channel; `progId` is (c)'s, `pipe` is (m)'s, and
//        `time` is (h)'s.  RED-PROVED: dropping `fwo` from the L2 key
//        reddens only this test's `fwo` row and leaves (a) green.
//    (l) L1: EVERY KEY FIELD SEPARATES, one at a time -- the same
//        argument, over a synthetic provider whose answer depends on all
//        fourteen SignalKey fields including `pProvider` identity, which
//        no shipping provider does.  Its eleven hit-channel fields are
//        the same SignalHitKey::Equals ProgramKey uses, so this covers
//        both levels' use of it.  RED-PROVED: dropping `baryB` reddens
//        only this test's `baryB` row -- (i), which splices all three
//        mesh fields at once, does not notice.
//    (m) THE TWO PIPES DO NOT SHARE AN L2 ENTRY.  One scalar-typed
//        program handed to BOTH RISE_API_CreateExpressionPainter and
//        RISE_API_CreateExpressionScalarPainter -- reachable, because a
//        copied program keeps its id -- where the colour pipe calls
//        `EvalVec3` and the scalar pipe calls `Eval`.  Each must read its
//        OWN memo-off answer, in both orders.  RED-PROOF ATTEMPTED AND
//        NEGATIVE: removing the `pipe` key field leaves this GREEN,
//        because the two entry points produce identical bits on this
//        toolchain.  The check pins the contract, not an observed
//        divergence -- see its own header, and EvalPipe's.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

#include "../src/Library/Interfaces/ISurfaceSignalProvider.h"
#include "../src/Library/Interfaces/SurfaceSignalProximity.h"
#include "../src/Library/Geometry/SDFGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Painters/ExpressionEval.h"
#include "../src/Library/Painters/ExpressionPainter.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/ExpressionMemo.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Animation/KeyframableHelper.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const std::string& name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

static void CheckExact( const Scalar got, const Scalar want, const std::string& name )
{
	if( got == want ) { ++passCount; }
	else {
		++failCount;
		std::cout.precision( 17 );
		std::cout << "  FAIL: " << name << "  got " << got << "  want " << want << std::endl;
	}
}

//======================================================================
// Helpers
//======================================================================

//! RAII flip of the memo kill switch, so a failing check cannot leave the
//! process with the switch stuck for the tests that follow.
//!
//! RESTORES THE ENCLOSING VALUE, not the -1 "no override" sentinel.  Two
//! of these do nest (a per-field sweep inside a test that has already
//! forced the memo on), and a destructor that unconditionally wrote -1
//! would drop the OUTER switch on the inner one's way out -- silently
//! handing the rest of that test whatever the options file says instead
//! of what it asked for.
class MemoSwitch
{
public:
	explicit MemoSwitch( const int tri )
		: m_prev( ExpressionMemo::EnableOverrideRef().load( std::memory_order_relaxed ) )
	{
		ExpressionMemo::SetEnabledOverride( tri );
	}
	~MemoSwitch() { ExpressionMemo::SetEnabledOverride( m_prev ); }
private:
	const int m_prev;
};

static RayIntersection MkRI( const Point3& origin, const Vector3& dir )
{
	return RayIntersection( Ray( origin, dir ), nullRasterizerState );
}

static bool HitObject( const Object* obj, RayIntersection& ri )
{
	obj->IntersectRay( ri, RISE_INFINITY, true, true, false );
	return ri.geometric.bHit;
}

//! Two overlapping spheres: a genuine concave crease plus purely convex
//! tips, so one geometry gives the estimators something to vary over.
static SDFGeometry* BuildSdfCreasedPair( const Scalar off )
{
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart(
		SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3( -off, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), 1.0, 0, 0, 0 ) );
	parts.push_back( SDFGeometry::MakePart(
		SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3( off, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), 1.0, 0, 0, 0 ) );
	return new SDFGeometry( parts, 512, Scalar( 1e-5 ) );
}

static bool CompileWithContext( const std::string& body, ExpressionProgram& out )
{
	ExpressionProgram::Builder b;
	b.EnableContextVars( true );
	return b.Finalize( body, out );
}

//! A provider whose answer is a MEMBER this test can move at will --
//! which is the only way to red-prove the generation counter, since every
//! shipping provider is a pure function of immutable state and so cannot
//! change its answer for an unchanged key by any other means.
class MutableTestProvider : public ISurfaceSignalProvider
{
public:
	Scalar value;
	bool   refuse;

	MutableTestProvider() : value( Scalar( 0.25 ) ), refuse( false ) {}

	bool ComputeOcclusion( const SurfaceSignalInfo&, const Scalar, const bool, Scalar& out ) const override
	{
		if( refuse ) return false;
		out = value;
		return true;
	}
	bool ComputeThickness( const SurfaceSignalInfo&, const Scalar, const bool, Scalar& out ) const override
	{
		if( refuse ) return false;
		out = value;
		return true;
	}
	bool ComputeConvexity( const SurfaceSignalInfo&, const Scalar, const bool, Scalar& out ) const override
	{
		if( refuse ) return false;
		out = value;
		return true;
	}
};

//! A deterministic, self-contained PRNG so the randomized sweep is the
//! same on every machine and every run.  (xorshift64*, seeded fixed.)
class Rng
{
public:
	explicit Rng( unsigned long long s ) : m_s( s ? s : 88172645463325252ULL ) {}
	Scalar Next() { return Scalar( (double)( Step() >> 11 ) / (double)( 1ull << 53 ) ); }
	Scalar NextIn( const Scalar a, const Scalar b ) { return a + ( b - a ) * Next(); }
	int    NextInt( const int n ) { return (int)( Step() % (unsigned long long)n ); }
private:
	unsigned long long Step()
	{
		m_s ^= m_s >> 12; m_s ^= m_s << 25; m_s ^= m_s >> 27;
		return m_s * 2685821657736338717ULL;
	}
	unsigned long long m_s;
};

//======================================================================
// (a) differential equality, memo on vs memo off
//======================================================================

//! ONE DRAW: the context an expression sees, plus the hit-record scale
//! factor it was derived from.  The two travel together because
//! ExpressionPainter::BuildContext does not COPY `curv` off the record --
//! it computes it as `curvature * scaleHint`, so a test that wants a hit
//! record and a context to agree BIT FOR BIT has to know both halves and
//! multiply them the same way.
struct Draw
{
	ExprEvalContext	ctx;
	Scalar			curvature;		//!< -> ri.derivatives.curvature  (== ctx.curvR)
	Scalar			scaleHint;		//!< -> ri.derivatives.scaleHint
};

//! Builds a draw that is DELIBERATELY taken from a SMALL set of distinct
//! values per field, so the sweep actually exercises memo HITS (a fully
//! random context would miss every time and prove nothing about the hit
//! path) while still varying every field of the key.
static Draw RandomDraw( Rng& rng, const SurfaceSignalInfo& live )
{
	static const Scalar grid[4] = { Scalar(0.0), Scalar(0.25), Scalar(0.5), Scalar(0.75) };
	Draw d;
	ExprEvalContext& ctx = d.ctx;
	ctx.u = grid[ rng.NextInt(4) ];
	ctx.v = grid[ rng.NextInt(4) ];
	ctx.P  = Vector3( grid[ rng.NextInt(4) ], grid[ rng.NextInt(4) ], grid[ rng.NextInt(4) ] );
	ctx.Po = Vector3( grid[ rng.NextInt(4) ], grid[ rng.NextInt(4) ], grid[ rng.NextInt(4) ] );
	ctx.N  = Vector3( grid[ rng.NextInt(4) ], grid[ rng.NextInt(4) ], Scalar(1) );
	ctx.fw    = grid[ rng.NextInt(4) ] * Scalar( 0.01 );
	ctx.fwo   = grid[ rng.NextInt(4) ] * Scalar( 0.01 );
	// `time` is pinned at 0, not drawn: BOTH painter pipes SET it (the
	// colour pipe from its keyframable `m_time`, the scalar pipe to a
	// constant 0), so a drawn value could never reach a painter and the
	// painter-vs-VM comparison below would fail on a difference this
	// test invented.  `time`'s place in the key is covered directly by
	// TestKeyedOnPainterTime instead.
	ctx.time  = Scalar( 0 );

	d.curvature = grid[ rng.NextInt(4) ];
	d.scaleHint = grid[ rng.NextInt(4) ] + Scalar( 1 );
	ctx.curvR = d.curvature;
	ctx.curv  = d.curvature * d.scaleHint;		// exactly PopulateCurvature's arithmetic

	// Half the draws carry the LIVE provider channel (so occlusion() /
	// convexity() really run against an SDF), half carry none (so the
	// neutral-fallback path is memoised and compared too).
	if( rng.NextInt( 2 ) ) ctx.signals = live;
	return d;
}

//! Turn a draw back into the hit record a painter would be handed, so the
//! SAME draw can be pushed through the painter pipes (where the memo
//! lives) and through the VM directly (where it does not).  `base` is a
//! real hit, so whatever a genuine intersection put in the fields this
//! test does not vary survives.
//!
//! fw/fwo and curv/curvR are written THROUGH the record's own gates --
//! `txFootprint.widthValid` and `derivatives.curvatureValid` -- because
//! that is how BuildContext reads them; assigning them to the context and
//! not to the record would silently pin them and stop covering them as
//! key fields.
static RayIntersectionGeometric MakeHit( const RayIntersectionGeometric& base, const Draw& d )
{
	RayIntersectionGeometric ri = base;
	ri.ptCoord        = Point2( d.ctx.u, d.ctx.v );
	ri.ptIntersection = Point3( d.ctx.P.x, d.ctx.P.y, d.ctx.P.z );
	ri.ptObjIntersec  = Point3( d.ctx.Po.x, d.ctx.Po.y, d.ctx.Po.z );
	ri.vNormal        = d.ctx.N;
	ri.signals        = d.ctx.signals;
	ri.txFootprint.widthValid   = true;
	ri.txFootprint.worldWidth   = d.ctx.fw;
	ri.txFootprint.objectWidth  = d.ctx.fwo;
	ri.derivatives.curvatureValid = true;
	ri.derivatives.curvature      = d.curvature;
	ri.derivatives.scaleHint      = d.scaleHint;
	return ri;
}

static void TestDifferentialEquality()
{
	std::cout << "(a) differential equality, memo on vs memo off, 10 000+ contexts" << std::endl;

	SDFGeometry* g = BuildSdfCreasedPair( 0.7 );
	Object* o = new Object( g );
	g->release();
	o->FinalizeTransformations();

	RayIntersection crease = MkRI( Point3( 0, 30, 0 ), Vector3( 0, -1, 0 ) );
	Check( HitObject( o, crease ), "(a) crease hit" );
	const SurfaceSignalInfo live = crease.geometric.signals;
	Check( live.pProvider != 0, "(a) the SDF hit publishes a live signal provider" );

	// A body that exercises BOTH memo levels: two DISTINCT signal queries
	// (so L1 must hold more than one key), noise (so the program is
	// memo-worthy on its own account), and every context variable.
	ExpressionProgram prog = ExpressionProgram::Invalid();
	Check( CompileWithContext(
		"vec3( occlusion(0.2)*0.5 + convexity(0.05)*0.25 + fbm(P*3.0+Po, 3, 0.5, 2.0)*0.1,"
		"      u*0.3 + v*0.2 + curv*0.1 + curvR*0.05 + time*0.02 + fw*7.0 + fwo*3.0,"
		"      N.x*0.2 + N.y*0.1 + N.z*0.05 )", prog ), "(a) body compiles" );
	Check( prog.MemoWorthy(), "(a) the body is classified memo-worthy" );

	std::vector<ParamSpec> specs;
	ExpressionPainter*       colour = new ExpressionPainter( prog, specs, Scalar( 0 ) );
	ExpressionScalarPainter* scalar = new ExpressionScalarPainter( prog, specs );

	const int kN = 10500;

	// Pass 1: memo OFF, record every answer through both painter pipes.
	std::vector<Scalar>  refNM( kN );
	std::vector<Scalar>  refScalar( kN );
	std::vector<RISEPel> refColour( kN );
	{
		MemoSwitch off( 0 );
		Rng rng( 12345 );
		for( int i = 0; i < kN; ++i ) {
			const Draw d = RandomDraw( rng, live );
			const RayIntersectionGeometric ri = MakeHit( crease.geometric, d );
			refColour[i]  = colour->GetColor( ri );
			refNM[i]      = colour->GetColorNM( ri, Scalar( 380 ) + Scalar( i % 41 ) * Scalar( 10 ) );
			refScalar[i]  = scalar->GetValuesAt( ri ).v[0];
		}
	}

	// Pass 2: memo ON, the same draws.  Bit equality on both pipes.
	int colourBad = 0, nmBad = 0, scalarBad = 0, progBad = 0;
	{
		MemoSwitch on( 1 );
		Rng rng( 12345 );
		for( int i = 0; i < kN; ++i ) {
			const Draw d = RandomDraw( rng, live );
			const RayIntersectionGeometric ri = MakeHit( crease.geometric, d );
			const RISEPel c = colour->GetColor( ri );
			if( c[0] != refColour[i][0] || c[1] != refColour[i][1] || c[2] != refColour[i][2] ) ++colourBad;
			if( colour->GetColorNM( ri, Scalar( 380 ) + Scalar( i % 41 ) * Scalar( 10 ) ) != refNM[i] ) ++nmBad;
			if( scalar->GetValuesAt( ri ).v[0] != refScalar[i] ) ++scalarBad;

			// THE STRONGER FORM of the same claim: the memoised painter
			// must equal the VM evaluated DIRECTLY, which bypasses L2 --
			// and ONLY L2.  L2 lives in the painters (see
			// ExpressionProgram::MakeMemoKey's comment for why), so this
			// call cannot be served a stored program result; it is the
			// check that would catch L2 returning a plausible but wrong
			// value, which comparing two memo-on runs could not.
			//
			// It does NOT bypass L1: `occlusion()` / `convexity()` inside
			// this program reach SurfaceSignalInfo::SignalQuery through
			// the VM's CallFunc, and that is exactly where L1 sits.  So
			// this oracle would agree with the painter even if L1 served a
			// wrong signal to both.  What pins L1 is pass 1 above, taken
			// under `MemoSwitch off(0)` with no memo of either level in
			// play, and the red-proofs in (b)/(e)/(f)/(i).
			const Vector3 direct = prog.EvalVec3( d.ctx );
			if( c[0] != direct.x || c[1] != direct.y || c[2] != direct.z ) ++progBad;
		}
	}

	Check( colourBad == 0, "(a) ExpressionPainter::GetColor bit-identical with the memo on" );
	Check( nmBad     == 0, "(a) ExpressionPainter::GetColorNM bit-identical with the memo on" );
	Check( scalarBad == 0, "(a) ExpressionScalarPainter::GetValuesAt bit-identical with the memo on" );
	Check( progBad   == 0, "(a) a memoised painter equals the un-memoised VM, evaluation for evaluation" );

	scalar->release();
	colour->release();
	o->release();
}

//======================================================================
// (b) generation red-proof   +   (e) kill switch red-proof
//======================================================================

//! Both live here because they are the SAME red-proof run three ways:
//! change what the provider answers behind an unchanged key, and see
//! which of stale / fresh comes back.
static void TestGenerationAndKillSwitchRedProof()
{
	std::cout << "(b)/(e) generation and kill-switch red-proofs" << std::endl;

	MutableTestProvider provider;

	SurfaceSignalInfo hit;
	hit.pProvider = &provider;
	hit.ptObject  = Point3( 0.125, 0.25, 0.5 );
	hit.nObject   = Vector3( 0, 0, 1 );

	// --- L1, RED HALF: memo ON, no bump.  The STALE value must come back.
	//     If it does not, the memo is not caching and every "the bump
	//     fixed it" check below would be vacuous.
	{
		MemoSwitch on( 1 );
		ExpressionMemo::Invalidate();			// start from a cold table
		provider.value = Scalar( 0.25 );
		CheckExact( hit.Occlusion( 0.1, true ), Scalar( 0.25 ), "(b) first query returns the live value" );
		provider.value = Scalar( 0.75 );		// the provider now answers differently...
		CheckExact( hit.Occlusion( 0.1, true ), Scalar( 0.25 ),
			"(b) RED-PROOF: without a bump the memo really does return the STALE value" );

		// --- GREEN HALF: bump, and the fresh value must come back.
		ExpressionMemo::Invalidate();
		CheckExact( hit.Occlusion( 0.1, true ), Scalar( 0.75 ),
			"(b) after Invalidate() the fresh value is returned" );
	}

	// --- (e) KILL SWITCH: the identical sequence with the memo off must
	//     never return a stale value, which is what "never consulted"
	//     means observably.
	{
		MemoSwitch off( 0 );
		provider.value = Scalar( 0.25 );
		CheckExact( hit.Occlusion( 0.1, true ), Scalar( 0.25 ), "(e) switch off: first query live" );
		provider.value = Scalar( 0.75 );
		CheckExact( hit.Occlusion( 0.1, true ), Scalar( 0.75 ),
			"(e) switch off: the memo is never consulted (fresh value, no bump)" );
		provider.value = Scalar( 0.5 );
		CheckExact( hit.Occlusion( 0.1, true ), Scalar( 0.5 ),
			"(e) switch off: still fresh on a third change" );
	}

	// The switch must also take effect on a table that is already WARM --
	// SetEnabledOverride bumps the generation for exactly this reason.
	{
		MemoSwitch on( 1 );
		provider.value = Scalar( 0.125 );
		CheckExact( hit.Occlusion( 0.1, true ), Scalar( 0.125 ), "(e) warm again" );
		provider.value = Scalar( 0.875 );
		CheckExact( hit.Occlusion( 0.1, true ), Scalar( 0.125 ), "(e) warm and stale, as expected" );
		{
			MemoSwitch off( 0 );
			CheckExact( hit.Occlusion( 0.1, true ), Scalar( 0.875 ),
				"(e) turning the switch off drops an already-warm table" );
		}
	}

	// --- The L2 half of the same red-proof, through a PAINTER (which is
	//     where L2 lives).  `fbm(P,...)*0` keeps the program memo-worthy
	//     while leaving the value entirely the provider's.
	{
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( CompileWithContext( "occlusion(0.1) + fbm(P,2,0.5,2.0)*0.0", prog ), "(b) L2 body compiles" );
		Check( prog.MemoWorthy(), "(b) L2 body is memo-worthy" );
		std::vector<ParamSpec> specs;
		ExpressionScalarPainter* painter = new ExpressionScalarPainter( prog, specs );

		// No default ctor on the hit record -- it is always built from a
		// ray.  The ray itself is irrelevant here; only `signals` is read.
		RayIntersection carrier = MkRI( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
		RayIntersectionGeometric& ri = carrier.geometric;
		ri.signals = hit;

		MemoSwitch on( 1 );
		ExpressionMemo::Invalidate();
		provider.value = Scalar( 0.3 );
		CheckExact( painter->GetValuesAt( ri ).v[0], Scalar( 0.3 ), "(b) L2 first evaluation is live" );
		provider.value = Scalar( 0.6 );
		CheckExact( painter->GetValuesAt( ri ).v[0], Scalar( 0.3 ),
			"(b) RED-PROOF: L2 returns the stale program result without a bump" );
		ExpressionMemo::Invalidate();
		CheckExact( painter->GetValuesAt( ri ).v[0], Scalar( 0.6 ), "(b) L2 fresh after the bump" );

		{
			MemoSwitch off( 0 );
			provider.value = Scalar( 0.9 );
			CheckExact( painter->GetValuesAt( ri ).v[0], Scalar( 0.9 ),
				"(e) L2 is never consulted with the switch off" );
		}
		painter->release();
	}
}

//======================================================================
// (c) program-id red-proof
//======================================================================

static void TestProgramIdRedProof()
{
	std::cout << "(c) program-id red-proof (address reuse must not alias)" << std::endl;

	MemoSwitch on( 1 );
	ExpressionMemo::Invalidate();

	RayIntersection carrier = MkRI( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
	RayIntersectionGeometric& ri = carrier.geometric;
	ri.ptCoord        = Point2( Scalar( 0.375 ), Scalar( 0.625 ) );
	ri.ptIntersection = Point3( 1, 2, 3 );
	ri.ptObjIntersec  = Point3( 1, 2, 3 );

	std::vector<ParamSpec> specs;

	// Heap-allocate the first painter, evaluate it (filling the memo),
	// then DESTROY it and allocate the second -- which the allocator will
	// very often place at the freed address.  A `this`-keyed memo would
	// then serve program A's value for program B.
	unsigned long long idA = 0;
	Scalar valA = 0;
	const void* addrA = 0;
	{
		ExpressionProgram a = ExpressionProgram::Invalid();
		Check( CompileWithContext( "u*100.0 + v*10.0 + P.x + fbm(P, 3, 0.5, 2.0)*0.0", a ), "(c) program A compiles" );
		idA = a.ProgramId();
		ExpressionScalarPainter* pa = new ExpressionScalarPainter( a, specs );
		addrA = (const void*)pa;
		valA  = pa->GetValuesAt( ri ).v[0];
		pa->release();
	}

	ExpressionProgram b = ExpressionProgram::Invalid();
	Check( CompileWithContext( "u*1.0 + v*2.0 + P.x*3.0 + fbm(P, 3, 0.5, 2.0)*0.0", b ), "(c) program B compiles" );
	const unsigned long long idB = b.ProgramId();
	ExpressionScalarPainter* pb = new ExpressionScalarPainter( b, specs );
	const void* addrB = (const void*)pb;

	Check( idA != 0 && idB != 0, "(c) both programs got a non-zero id" );
	Check( idA != idB, "(c) the two ids differ" );

	// What B must return, computed with the memo off so no cache can be
	// involved in establishing the reference.
	Scalar wantB = 0;
	{
		MemoSwitch off( 0 );
		wantB = pb->GetValuesAt( ri ).v[0];
	}
	CheckExact( pb->GetValuesAt( ri ).v[0], wantB, "(c) program B gets its OWN answer at A's context" );
	Check( wantB != valA, "(c) the two programs really do disagree here (the check has teeth)" );
	// Reported, not asserted: whether the allocator actually reused the
	// address is outside this test's control, but when it does, the check
	// above is the exact scenario the id exists for.
	if( addrA == addrB ) {
		std::cout << "    (address WAS reused -- a `this`-keyed memo would have aliased here)" << std::endl;
	}
	pb->release();

	// A COPY of a compiled program must keep its id: ExpressionPainter
	// holds the program BY VALUE, so if a copy were re-issued an id then
	// the painter and the program it was built from would never share an
	// entry and the memo would be dead on the painter path.
	ExpressionProgram bCopy = b;
	Check( bCopy.ProgramId() == idB, "(c) a copied program keeps its id" );
}

//======================================================================
// (d) eight threads, eight distinct sequences
//======================================================================

static void TestThreadIsolation()
{
	std::cout << "(d) eight-thread isolation" << std::endl;

	MemoSwitch on( 1 );
	ExpressionMemo::Invalidate();

	SDFGeometry* g = BuildSdfCreasedPair( 0.7 );
	Object* o = new Object( g );
	g->release();
	o->FinalizeTransformations();
	RayIntersection crease = MkRI( Point3( 0, 30, 0 ), Vector3( 0, -1, 0 ) );
	Check( HitObject( o, crease ), "(d) crease hit" );
	const SurfaceSignalInfo live = crease.geometric.signals;

	ExpressionProgram prog = ExpressionProgram::Invalid();
	Check( CompileWithContext(
		"occlusion(0.2)*1000.0 + convexity(0.05)*100.0 + u*10.0 + v + fbm(P,2,0.5,2.0)*0.0", prog ),
		"(d) body compiles" );
	std::vector<ParamSpec> specs;
	ExpressionScalarPainter* painter = new ExpressionScalarPainter( prog, specs );

	const int kThreads = 8;
	const int kPer     = 4000;

	// Reference: the whole cross-product, computed serially with the memo
	// OFF.  Thread t will walk its own slice of it.
	std::vector<Scalar> want( (size_t)kThreads * kPer );
	{
		MemoSwitch off( 0 );
		for( int t = 0; t < kThreads; ++t ) {
			Rng rng( 900 + (unsigned long long)t );
			for( int i = 0; i < kPer; ++i ) {
				const Draw d = RandomDraw( rng, live );
				want[ (size_t)t * kPer + i ] = painter->GetValuesAt( MakeHit( crease.geometric, d ) ).v[0];
			}
		}
	}

	std::atomic<int> bad( 0 );
	std::vector<std::thread> threads;
	for( int t = 0; t < kThreads; ++t ) {
		threads.push_back( std::thread( [&, t]() {
			Rng rng( 900 + (unsigned long long)t );
			for( int i = 0; i < kPer; ++i ) {
				const Draw d = RandomDraw( rng, live );
				if( painter->GetValuesAt( MakeHit( crease.geometric, d ) ).v[0] != want[ (size_t)t * kPer + i ] ) {
					bad.fetch_add( 1, std::memory_order_relaxed );
				}
			}
		} ) );
	}
	for( size_t i = 0; i < threads.size(); ++i ) threads[i].join();

	Check( bad.load() == 0, "(d) every thread got its own correct sequence with the memo on" );
	painter->release();
	o->release();
}

// (f) L1 single-sourcing: neutrals, refusals, and no cross-kind leak
//======================================================================

static void TestNeutralsAndKindSeparation()
{
	std::cout << "(f) neutrals, refusals, and kind separation through the memo" << std::endl;

	MemoSwitch on( 1 );
	ExpressionMemo::Invalidate();

	// No provider at all -- every kind must read its own neutral, and the
	// memo must not serve one kind's neutral for another (they differ:
	// occlusion 1, thickness 1, convexity 0).
	SurfaceSignalInfo none;
	CheckExact( none.Occlusion( 0.1, true ), Scalar( 1 ), "(f) null provider -> neutral occlusion 1" );
	CheckExact( none.Thickness( 0.1, true ), Scalar( 1 ), "(f) null provider -> neutral thickness 1" );
	CheckExact( none.Convexity( 0.1, true ), Scalar( 0 ), "(f) null provider -> neutral convexity 0" );
	// ...and again, now that all three are memoised, in the reverse order.
	CheckExact( none.Convexity( 0.1, true ), Scalar( 0 ), "(f) memoised convexity neutral stays 0" );
	CheckExact( none.Thickness( 0.1, true ), Scalar( 1 ), "(f) memoised thickness neutral stays 1" );
	CheckExact( none.Occlusion( 0.1, true ), Scalar( 1 ), "(f) memoised occlusion neutral stays 1" );

	MutableTestProvider provider;
	SurfaceSignalInfo hit;
	hit.pProvider = &provider;
	hit.ptObject  = Point3( 1, 2, 3 );

	// A REFUSAL must memoise as the neutral, not as the provider's stale
	// out-parameter, and must stay per-kind.
	provider.refuse = true;
	CheckExact( hit.Occlusion( 0.1, true ), Scalar( 1 ), "(f) refusal -> neutral occlusion" );
	CheckExact( hit.Convexity( 0.1, true ), Scalar( 0 ), "(f) refusal -> neutral convexity" );
	CheckExact( hit.Occlusion( 0.1, true ), Scalar( 1 ), "(f) refusal memoised per kind (occlusion)" );

	// An unusable radius is refused BEFORE the provider is consulted, and
	// two different radii are two different keys.
	//
	// The Invalidate() is load-bearing and not boilerplate: the refusal
	// checks above memoised the NEUTRAL under (occlusion, r=0.1), so
	// un-refusing without a bump correctly keeps returning that neutral --
	// which is (b)'s red-proof, not a bug, but it would make this check
	// measure the wrong thing.
	provider.refuse = false;
	provider.value  = Scalar( 0.4 );
	ExpressionMemo::Invalidate();
	CheckExact( hit.Occlusion( -1.0, true ), Scalar( 1 ), "(f) non-positive radius -> neutral" );
	CheckExact( hit.Occlusion(  0.1, true ), Scalar( 0.4 ), "(f) a different radius is a different key" );
	CheckExact( hit.Occlusion( -1.0, true ), Scalar( 1 ), "(f) ...and the neutral entry survives beside it" );

	// The constant-radius PROOF is part of the key too: a baked provider
	// answers one and refuses the other, so they must not share an entry.
	ExpressionMemo::Invalidate();
	provider.refuse = false;
	CheckExact( hit.Occlusion( 0.1, true ), Scalar( 0.4 ), "(f) constant-radius query" );
	provider.value = Scalar( 0.9 );
	ExpressionMemo::Invalidate();
	CheckExact( hit.Occlusion( 0.1, false ), Scalar( 0.9 ), "(f) dynamic-radius query is its own key" );
	CheckExact( hit.Occlusion( 0.1, true ),  Scalar( 0.9 ), "(f) both keys coexist" );

	// Two hits that differ ONLY in bComplementedField (the CSG sense flip)
	// must not share an entry -- the field is in the key for this reason.
	ExpressionMemo::Invalidate();
	SurfaceSignalInfo flipped = hit;
	flipped.bComplementedField = true;
	provider.value = Scalar( 0.2 );
	CheckExact( hit.Occlusion( 0.1, true ), Scalar( 0.2 ), "(f) unflipped hit" );
	provider.value = Scalar( 0.8 );
	ExpressionMemo::Invalidate();
	CheckExact( flipped.Occlusion( 0.1, true ), Scalar( 0.8 ), "(f) flipped hit is a distinct key" );
	CheckExact( hit.Occlusion( 0.1, true ),     Scalar( 0.8 ), "(f) both senses coexist" );
}

//======================================================================
// (g) the per-program memo gate
//======================================================================

static void TestMemoWorthinessGate()
{
	std::cout << "(g) the compile-time memo-worthiness gate" << std::endl;

	ExpressionProgram cheap = ExpressionProgram::Invalid();
	Check( CompileWithContext( "u*v", cheap ), "(g) cheap body compiles" );
	Check( !cheap.MemoWorthy(), "(g) a two-instruction arithmetic body is NOT memo-worthy" );

	ExpressionProgram noisy = ExpressionProgram::Invalid();
	Check( CompileWithContext( "fbm(P, 3, 0.5, 2.0)", noisy ), "(g) noise body compiles" );
	Check( noisy.MemoWorthy(), "(g) a noise body IS memo-worthy" );

	// THE BAND'S ENDS, not just something in the middle of it.  The gate
	// tests `fn` as a RANGE [kFnNoiseFirst, kFnNoiseLast], so an off-by-one
	// at either end would exclude a real noise builtin from the memo and
	// `fbm` alone (43, comfortably interior) would never notice.  `perlin`
	// is the bottom end (kFnPerlin) and `worley_id` the top (kFnWorleyId);
	// both bodies are far shorter than kFields, so they qualify on the
	// noise scan or not at all.
	ExpressionProgram bandLo = ExpressionProgram::Invalid();
	Check( CompileWithContext( "perlin(P)", bandLo ), "(g) perlin body compiles" );
	Check( bandLo.MemoWorthy(), "(g) the noise band's BOTTOM end (perlin) is memo-worthy" );

	ExpressionProgram bandHi = ExpressionProgram::Invalid();
	Check( CompileWithContext( "worley_id(P, 0.5)", bandHi ), "(g) worley_id body compiles" );
	Check( bandHi.MemoWorthy(), "(g) the noise band's TOP end (worley_id) is memo-worthy" );

	// And the id immediately PAST the top end is excluded, which is what
	// makes the band an interval and not "everything from 42 up".
	// `cellhash` is a single integer hash; a short body around it must not
	// qualify.
	ExpressionProgram pastBand = ExpressionProgram::Invalid();
	Check( CompileWithContext( "cellhash(u)", pastBand ), "(g) cellhash body compiles" );
	Check( !pastBand.MemoWorthy(),
		"(g) the id just past the band (cellhash) does NOT qualify a short body" );

	ExpressionProgram signal = ExpressionProgram::Invalid();
	Check( CompileWithContext( "occlusion(0.1)", signal ), "(g) signal body compiles" );
	Check( signal.MemoWorthy(), "(g) a signal body IS memo-worthy" );

	// A gated-out program must still be correct -- the gate changes cost,
	// never answers.
	MemoSwitch on( 1 );
	ExprEvalContext ctx;
	ctx.u = Scalar( 0.25 ); ctx.v = Scalar( 0.5 );
	CheckExact( cheap.EvalVec3( ctx ).x, Scalar( 0.125 ), "(g) a gated-out program still evaluates correctly" );

	// THE MEMORY CLAIM, ASSERTED rather than printed.  The commit that
	// introduced the memo priced it at 1408 bytes per worker (25.3 kB
	// decimal across 18); adding the L2 key's `pipe` field on 2026-09-07
	// moved it to 1440 (25.9 kB decimal across 18).  The figure is quoted
	// in docs/OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md, which records both
	// on the same decimal-kB basis.
	//
	// A CEILING rather than an equality, and that drift is exactly why:
	// adding a key field or a way is allowed to move the number, but it
	// must stay a rounding error against a render worker's stack, and a
	// change that blows past this has almost certainly widened a table or
	// put something non-trivial in `Tables` by accident.
	const std::size_t bytes = ExpressionMemo::BytesPerThread();
	std::cout << "    bytes of thread-local memo per render worker: " << bytes
		<< "  (1408 when the memo shipped, 1440 since the `pipe` key field, 1824 since the"
		   " four cross-object SignalHitKey fields -- six 8-byte slots x eight SignalHitKey"
		   " instances across the two 4-way tables; ceiling 2048)" << std::endl;
	Check( bytes <= 2048, "(g) the per-thread memo stays under the 2048-byte ceiling" );
}

//======================================================================
// (i) real-mesh hits: (primId, baryA, baryB) are L1 key fields
//======================================================================

//! Emits an axis-aligned quad, tessellated `cells` x `cells`, with a
//! constant normal -- the same builder MeshSignalBakeTest uses, and for
//! the same reason: every face carries its own vertices, so a corner
//! normal is not an average of several faces.
static void AddGrid( TriangleMeshGeometryIndexed* mesh, unsigned int& nextIndex,
	const Point3& o, const Vector3& du, const Vector3& dv, const Vector3& n, const int cells )
{
	const int side = cells + 1;
	const unsigned int base = nextIndex;
	for( int j = 0; j < side; ++j ) {
		for( int i = 0; i < side; ++i ) {
			const Scalar fu = Scalar(i) / Scalar(cells);
			const Scalar fv = Scalar(j) / Scalar(cells);
			mesh->AddVertex( Point3( o.x + du.x*fu + dv.x*fv,
			                         o.y + du.y*fu + dv.y*fv,
			                         o.z + du.z*fu + dv.z*fv ) );
			mesh->AddNormal( n );
			mesh->AddTexCoord( Point2( fu, fv ) );
			++nextIndex;
		}
	}
	for( int j = 0; j < cells; ++j ) {
		for( int i = 0; i < cells; ++i ) {
			const unsigned int a = base + (unsigned int)( j*side + i );
			const unsigned int b = a + 1;
			const unsigned int c = a + (unsigned int)side;
			const unsigned int d = c + 1;
			IndexedTriangle t1, t2;
			t1.iVertices[0] = a; t1.iVertices[1] = b; t1.iVertices[2] = d;
			t2.iVertices[0] = a; t2.iVertices[1] = d; t2.iVertices[2] = c;
			for( int k = 0; k < 3; ++k ) {
				t1.iNormals[k] = t1.iVertices[k]; t1.iCoords[k] = t1.iVertices[k];
				t2.iNormals[k] = t2.iVertices[k]; t2.iCoords[k] = t2.iVertices[k];
			}
			mesh->AddIndexedTriangle( t1 );
			mesh->AddIndexedTriangle( t2 );
		}
	}
}

//! MeshSignalBakeTest's trench: a floor in z = 0 spanning y in [-0.9, 1]
//! plus a wall in y = -1 rising to z = 2.  One mesh carrying both an
//! occluded region (the floor rows near the wall) and a clear one.
static TriangleMeshGeometryIndexed* BuildTrench()
{
	TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( false, false );
	mesh->BeginIndexedTriangles();
	unsigned int next = 0;
	AddGrid( mesh, next, Point3( -1, -0.9, 0 ), Vector3( 2, 0, 0 ), Vector3( 0, 1.9, 0 ), Vector3( 0, 0, 1 ), 8 );
	AddGrid( mesh, next, Point3( -1, -1, 0 ),   Vector3( 2, 0, 0 ), Vector3( 0, 0, 2 ),   Vector3( 0, 1, 0 ), 8 );
	mesh->DoneIndexedTriangles();
	return mesh;
}

//! THE MESH FAMILY'S KEY FIELDS, which no other check in this file
//! touches: every SurfaceSignalInfo the rest of the suite builds carries
//! `primId == -1` (the SDF family answers from position and normal and
//! stamps no triangle), so `primId` / `baryA` / `baryB` could fall out of
//! SurfaceSignalInfo::MemoHitKey() and the suite would stay green.
//!
//! HOW MUCH THIS CAN HONESTLY CLAIM.  A mesh provider
//! (TriangleMeshGeometryIndexed::LookupBakedSignal) reads ONLY (primId,
//! baryA, baryB) -- position and normal never enter its answer -- so the
//! three fields are load-bearing by construction.  But on a hit that a
//! real intersection produced, `ptObject` is DERIVED from them, so two
//! natural hits differing ONLY in (primId, bary) essentially do not occur
//! (it would take coincident, separately-indexed geometry with matching
//! normals).  The separation check below therefore SPLICES: it takes hit
//! A's record and writes hit B's (primId, bary) into it, so the two
//! records differ in exactly the fields under test.  What is real is the
//! PROVIDER -- a live bake on a real mesh, not a stub -- and the value it
//! returns for the spliced record, which the check first proves is B's
//! answer and not A's.
static void TestRealMeshKeyFields()
{
	std::cout << "(i) real-mesh key fields: primId / baryA / baryB" << std::endl;

	TriangleMeshGeometryIndexed* mesh = BuildTrench();
	Object* o = new Object( mesh );
	mesh->release();
	o->FinalizeTransformations();

	// Fire straight down onto the floor at a spread of y, from jammed
	// against the wall to well clear of it.
	const int kHits = 12;
	std::vector<RayIntersection> hits;
	int distinctPrims = 0;
	{
		std::vector<int> seenPrims;
		for( int i = 0; i < kHits; ++i ) {
			const Scalar y = Scalar( -0.85 ) + Scalar( i ) * Scalar( 0.15 );
			RayIntersection ri = MkRI( Point3( Scalar( 0.13 ), y, 5 ), Vector3( 0, 0, -1 ) );
			if( !HitObject( o, ri ) ) continue;
			if( ri.geometric.signals.pProvider == 0 || ri.geometric.signals.primId < 0 ) continue;
			bool seen = false;
			for( size_t s = 0; s < seenPrims.size(); ++s ) if( seenPrims[s] == ri.geometric.signals.primId ) seen = true;
			if( !seen ) { seenPrims.push_back( ri.geometric.signals.primId ); ++distinctPrims; }
			hits.push_back( ri );
		}
	}
	Check( hits.size() >= 8, "(i) the sweep produced real mesh hits" );
	Check( distinctPrims >= 4, "(i) ...spread over several triangles (so primId really varies)" );

	// ---- Differential over the REAL provider, memo off vs memo on.
	ExpressionProgram prog = ExpressionProgram::Invalid();
	Check( CompileWithContext( "occlusion(0.1) + fbm(P,2,0.5,2.0)*0.0", prog ), "(i) body compiles" );
	std::vector<ParamSpec> specs;
	ExpressionScalarPainter* painter = new ExpressionScalarPainter( prog, specs );

	std::vector<Scalar> ref( hits.size() );
	{
		MemoSwitch off( 0 );
		for( size_t i = 0; i < hits.size(); ++i ) ref[i] = painter->GetValuesAt( hits[i].geometric ).v[0];
	}
	int bad = 0, distinctVals = 0;
	{
		MemoSwitch on( 1 );
		ExpressionMemo::Invalidate();
		for( size_t i = 0; i < hits.size(); ++i ) {
			if( painter->GetValuesAt( hits[i].geometric ).v[0] != ref[i] ) ++bad;
		}
	}
	for( size_t i = 0; i < ref.size(); ++i ) {
		bool seen = false;
		for( size_t j = 0; j < i; ++j ) if( ref[j] == ref[i] ) seen = true;
		if( !seen ) ++distinctVals;
	}
	Check( bad == 0, "(i) a live mesh bake reads bit-identically through the memo" );
	Check( distinctVals >= 3, "(i) ...and the sweep really varies (the differential has teeth)" );

	// ---- Separation: two records differing ONLY in (primId, baryA, baryB).
	// Pick the two hits whose baked occlusion is furthest apart, so the
	// check cannot pass on two values that happen to agree.
	size_t iA = 0, iB = 0;
	Scalar spread = Scalar( 0 );
	for( size_t i = 0; i < ref.size(); ++i ) {
		for( size_t j = 0; j < ref.size(); ++j ) {
			const Scalar d = std::fabs( ref[i] - ref[j] );
			if( d > spread ) { spread = d; iA = i; iB = j; }
		}
	}
	Check( spread > Scalar( 0.02 ), "(i) two hits with genuinely different baked occlusion exist" );

	const SurfaceSignalInfo& sa = hits[iA].geometric.signals;
	const SurfaceSignalInfo& sb = hits[iB].geometric.signals;

	// A's hit record with B's triangle and barycentrics spliced in.
	SurfaceSignalInfo spliced = sa;
	spliced.primId = sb.primId;
	spliced.baryA  = sb.baryA;
	spliced.baryB  = sb.baryB;
	Check( spliced.primId != sa.primId, "(i) the spliced record really names a different triangle" );

	Scalar wantA = 0, wantB = 0, wantSpliced = 0;
	{
		MemoSwitch off( 0 );
		wantA       = sa.Occlusion( 0.1, true );
		wantB       = sb.Occlusion( 0.1, true );
		wantSpliced = spliced.Occlusion( 0.1, true );
	}
	Check( wantA != wantB, "(i) the two hits' baked occlusion differs" );
	// The provider reads ONLY (primId, bary), so A's position with B's
	// triangle answers exactly B -- which is precisely why the memo may
	// not key on position alone.
	CheckExact( wantSpliced, wantB, "(i) the provider's answer is a function of (primId, bary) alone" );

	{
		MemoSwitch on( 1 );
		ExpressionMemo::Invalidate();
		CheckExact( sa.Occlusion( 0.1, true ), wantA, "(i) A memoises its own answer" );
		CheckExact( spliced.Occlusion( 0.1, true ), wantSpliced,
			"(i) a record differing ONLY in (primId, bary) is NOT served A's entry" );
		// ...and A is still A afterwards: both keys coexist rather than
		// one having overwritten the other.
		CheckExact( sa.Occlusion( 0.1, true ), wantA, "(i) both keys coexist" );
	}

	painter->release();
	o->release();
}

//======================================================================
// (j) the SHIPPED DEFAULT path: no override, `expression_memo` in the
//     options file
//======================================================================

//! Every other check in this file runs under an explicit MemoSwitch,
//! which short-circuits EnabledSlow() before it ever looks at the
//! options.  So a typo in the option NAME, or a flipped default, would
//! leave the whole suite green while shipping a memo that is off (or a
//! kill switch that cannot be thrown).  This is the check for that, and
//! it needs TWO processes: EnabledSlow() caches the options read in a
//! function-local static, and GlobalOptions() itself is a read-once
//! singleton, so one process can only ever observe one answer.
//!
//! The PARENT observes the default: an options file that does not mention
//! the key at all must leave the memo ON.  The CHILD -- this same binary,
//! re-executed with RISE_OPTIONS_FILE pointing at a file that says
//! `expression_memo false` -- must read it OFF, and reports that as its
//! exit status.
static const char* kOptionChildFlag = "--expression-memo-option-child";

//! Reported instead of a FAIL when the environment cannot support this
//! check at all -- see TestOptionDefaultAndKey's own comment for the
//! three ways that happens.  Counted separately from pass/fail so a
//! sandbox without a shell does not turn the suite red for a reason that
//! has nothing to do with the memo.
static int skipCount = 0;

static void Skip( const std::string& name, const std::string& why )
{
	++skipCount;
	std::cout << "  SKIPPED: " << name << "  (" << why << ")" << std::endl;
}

static bool WriteOptionsFile( const std::string& path, const char* body )
{
	std::ofstream ofs( path.c_str() );
	if( !ofs ) return false;
	ofs << body;
	ofs.close();
	return !ofs.fail();
}

static void SetOptionsEnv( const char* path )
{
#ifdef _WIN32
	_putenv_s( "RISE_OPTIONS_FILE", path );
#else
	setenv( "RISE_OPTIONS_FILE", path, 1 );
#endif
}

static void UnsetOptionsEnv()
{
#ifdef _WIN32
	_putenv_s( "RISE_OPTIONS_FILE", "" );
#else
	unsetenv( "RISE_OPTIONS_FILE" );
#endif
}

//! SAVE AND RESTORE the caller's RISE_OPTIONS_FILE.  (j) has to point it
//! at a file of its own, and GlobalOptions() is a read-once singleton --
//! so without this the variable it set would outlive the check and be
//! inherited by anything the process does afterwards, including a
//! debugger session or a future test appended below.  "Unset" is restored
//! as unset, not as the empty string.
class OptionsEnvRestore
{
public:
	OptionsEnvRestore()
	{
		const char* v = std::getenv( "RISE_OPTIONS_FILE" );
		m_had = ( v != 0 );
		if( m_had ) m_prev = v;
	}
	~OptionsEnvRestore()
	{
		if( m_had ) SetOptionsEnv( m_prev.c_str() );
		else        UnsetOptionsEnv();
	}
private:
	bool        m_had;
	std::string m_prev;
};

//! A PRIVATE TEMP DIRECTORY for (j)'s two options files, removed on every
//! exit path.  They used to be written to the CWD under fixed names,
//! which collides with a concurrently-running copy of this test and
//! leaves litter behind if a check throws or a `Check` aborts the run.
//!
//! `Ok()` false means "no directory" -- the caller reports SKIP, not
//! FAIL.  `Path(name)` is only ever called when Ok().
class TempDir
{
public:
	TempDir()
	{
#ifdef _WIN32
		char base[ MAX_PATH ] = {0};
		char dir[ MAX_PATH ]  = {0};
		if( GetTempPathA( MAX_PATH, base ) == 0 ) return;
		// GetTempFileNameA makes a unique FILE; replace it with a
		// directory of the same name, which is the documented idiom.
		if( GetTempFileNameA( base, "rmemo", 0, dir ) == 0 ) return;
		DeleteFileA( dir );
		if( !CreateDirectoryA( dir, NULL ) ) return;
		m_dir = dir;
#else
		std::string tmpl = "/tmp/rise_expr_memo_XXXXXX";
		std::vector<char> buf( tmpl.begin(), tmpl.end() );
		buf.push_back( '\0' );
		if( !mkdtemp( &buf[0] ) ) return;
		m_dir = &buf[0];
#endif
	}

	~TempDir()
	{
		if( m_dir.empty() ) return;
		for( size_t i = 0; i < m_files.size(); ++i ) std::remove( m_files[i].c_str() );
#ifdef _WIN32
		RemoveDirectoryA( m_dir.c_str() );
#else
		rmdir( m_dir.c_str() );
#endif
	}

	bool Ok() const { return !m_dir.empty(); }

	//! Reserve a path inside the directory; it is removed by the
	//! destructor whether or not it was ever created.
	std::string Path( const char* leaf )
	{
#ifdef _WIN32
		const std::string p = m_dir + "\\" + leaf;
#else
		const std::string p = m_dir + "/" + leaf;
#endif
		m_files.push_back( p );
		return p;
	}

private:
	std::string              m_dir;
	std::vector<std::string> m_files;
};

//! The child half.  Runs before anything else touches the memo, so the
//! options read it forces is the one under test.
static int RunOptionChild()
{
	const bool enabled = ExpressionMemo::EnabledSlow();
	if( enabled ) {
		std::cout << "  FAIL: child: `expression_memo false` in the options file "
			"did not turn the memo off" << std::endl;
		return 1;
	}
	// And the memo really is inert, not merely reported off: the stale
	// provider trick from (b) must return FRESH values.
	MutableTestProvider provider;
	SurfaceSignalInfo hit;
	hit.pProvider = &provider;
	hit.ptObject  = Point3( 0.5, 0.25, 0.125 );
	provider.value = Scalar( 0.25 );
	if( hit.Occlusion( 0.1, true ) != Scalar( 0.25 ) ) return 1;
	provider.value = Scalar( 0.75 );
	if( hit.Occlusion( 0.1, true ) != Scalar( 0.75 ) ) {
		std::cout << "  FAIL: child: the memo answered from a table with the option off" << std::endl;
		return 1;
	}
	return 0;
}

//! WHAT THIS CHECK DOES TO THE MACHINE, disclosed because it is the only
//! one in this file that leaves the process:
//!
//!   * it WRITES TWO FILES, into a private directory made by mkdtemp
//!     (POSIX) / GetTempPath + CreateDirectory (Windows).  Never the CWD:
//!     fixed names there collide with a concurrent copy of this test and
//!     survive an aborted run.  Both files and the directory are removed
//!     by TempDir's destructor on every exit path;
//!   * it SETS RISE_OPTIONS_FILE in this process, and restores the
//!     caller's value -- or unsets it again if there was none -- through
//!     OptionsEnvRestore;
//!   * it SPAWNS A CHILD PROCESS with std::system(), re-executing this
//!     same binary with kOptionChildFlag.  Two processes are unavoidable:
//!     GlobalOptions() is a read-once singleton and EnabledSlow() caches
//!     its answer in a function-local static, so one process can only
//!     ever observe one answer.
//!
//! AND IT REPORTS SKIP, NOT FAIL, when the environment cannot support
//! that: no writable temp directory, an options file that will not open,
//! no shell for std::system (which then returns 0 from the availability
//! probe, or -1 from the run), a temp path or argv0 this test cannot
//! safely single-quote, a child killed by a signal, or -- on POSIX -- a
//! shell exit of 126/127 (command not found / not executable), which is
//! the SHELL failing to run the child at all, not the child reporting a
//! real verdict.  A sandbox without a shell is not a memo bug.
//!
//! WINDOWS DEBT: the `_WIN32` branches here -- _putenv_s, GetTempPathA /
//! GetTempFileNameA / CreateDirectoryA / RemoveDirectoryA, and the `set
//! X=Y && ...` command form -- have NOT been compiled.  Noted here and in
//! tests/README.md; first MSVC build of this file should expect to fix
//! them.
static void TestOptionDefaultAndKey( const char* argv0 )
{
	std::cout << "(j) the shipped default, and the `expression_memo` options key" << std::endl;

	const OptionsEnvRestore envRestore;
	TempDir tmp;
	if( !tmp.Ok() ) {
		Skip( "(j)", "no writable temp directory" );
		return;
	}

	const std::string noKeyPath = tmp.Path( "expr_memo_opts_default.txt" );
	const std::string offPath   = tmp.Path( "expr_memo_opts_off.txt" );

	// --- The DEFAULT.  A real options file with a real key in it, but no
	//     `expression_memo` line: EnabledSlow() must fall through to its
	//     `true` default.  This must be the FIRST thing in the run that
	//     asks -- both GlobalOptions() and EnabledSlow()'s own static read
	//     once per process.
	if( !WriteOptionsFile( noKeyPath, "render_thread_reserve_count 1\n" ) ) {
		Skip( "(j)", "could not write " + noKeyPath );
		return;
	}
	SetOptionsEnv( noKeyPath.c_str() );
	Check( ExpressionMemo::EnabledSlow(),
		"(j) with no override and no `expression_memo` key, the memo ships ON" );

	// --- The KEY, in a child process.
	if( !WriteOptionsFile( offPath, "expression_memo false\n" ) ) {
		Skip( "(j) child", "could not write " + offPath );
		return;
	}

	// std::system(0) is the standard's own "is there a command processor"
	// probe: zero means there is none, and the child half simply cannot
	// run.
	if( std::system( 0 ) == 0 ) {
		Skip( "(j) child", "no command processor available to std::system" );
		return;
	}

#ifndef _WIN32
	// A single-quoted shell argument cannot contain a literal single quote
	// without the `'"'"'` escape dance this test does not carry -- SKIP
	// rather than build a command line that silently means something else
	// (or breaks) on a temp path we do not fully control the naming of.
	if( offPath.find( '\'' ) != std::string::npos ||
	    std::string( argv0 ).find( '\'' ) != std::string::npos ) {
		Skip( "(j) child", "temp path or argv0 contains a single quote; cannot safely quote for the shell" );
		return;
	}
#endif

	std::string cmd;
#ifdef _WIN32
	cmd = std::string( "set \"RISE_OPTIONS_FILE=" ) + offPath + "\" && \"" + argv0 + "\" " + kOptionChildFlag;
#else
	cmd = std::string( "RISE_OPTIONS_FILE='" ) + offPath + "' '" + argv0 + "' " + kOptionChildFlag;
#endif
	const int rc = std::system( cmd.c_str() );
	if( rc == -1 ) {
		Skip( "(j) child", "std::system could not create the child process" );
		return;
	}

#ifdef _WIN32
	const int childExit = rc;
#else
	// On POSIX, std::system() returns the raw wait status, not the exit
	// code -- WEXITSTATUS() decodes it, and only when WIFEXITED() is true
	// (the child was not killed by a signal).  A shell that could not run
	// the command at all reports its OWN exit code this way: 127 is
	// "command not found" (e.g. argv0 did not resolve the way this test
	// assumed) and 126 is "found but not executable" -- both are
	// environmental failures of the harness, not a verdict on the memo,
	// so they SKIP rather than FAIL.  The existing SKIP paths above (no
	// command processor, std::system() == -1) are unaffected.
	if( !WIFEXITED( rc ) ) {
		Skip( "(j) child", "child process did not exit normally (terminated by a signal)" );
		return;
	}
	const int childExit = WEXITSTATUS( rc );
	if( childExit == 126 || childExit == 127 ) {
		Skip( "(j) child", "shell exit " + std::to_string( childExit ) +
			" (command not found / not executable) -- environmental, not the memo's fault" );
		return;
	}
#endif
	Check( childExit == 0, "(j) a child process reading `expression_memo false` has the memo OFF" );
	if( childExit != 0 ) {
		std::cout << "    (child command was: " << cmd << ", raw status " << rc
			<< ", exit " << childExit << ")" << std::endl;
	}
}

//======================================================================
// (h) `time` is in the key
//======================================================================

//! Two ExpressionPainters built from ONE compiled program -- so one
//! program id -- differing only in their keyframable `time`.  They must
//! not share a memo entry.
//!
//! This is the check that the sweep in (a) structurally cannot make: both
//! painter pipes SET `time` rather than reading it off the hit, so the
//! only way it varies at a fixed hit is across painters, which is exactly
//! what an animated `expression_painter` does between frames.
static void TestKeyedOnPainterTime()
{
	std::cout << "(h) two painters over one program, different `time`" << std::endl;

	MemoSwitch on( 1 );
	ExpressionMemo::Invalidate();

	ExpressionProgram prog = ExpressionProgram::Invalid();
	Check( CompileWithContext( "time*10.0 + u + fbm(P,2,0.5,2.0)*0.0", prog ), "(h) body compiles" );
	Check( prog.MemoWorthy(), "(h) body is memo-worthy" );

	std::vector<ParamSpec> specs;
	ExpressionPainter* early = new ExpressionPainter( prog, specs, Scalar( 0.25 ) );
	ExpressionPainter* late  = new ExpressionPainter( prog, specs, Scalar( 0.75 ) );
	Check( early->GetProgram().ProgramId() == late->GetProgram().ProgramId(),
		"(h) the two painters really do share one program id (the check has teeth)" );

	RayIntersection carrier = MkRI( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
	RayIntersectionGeometric& ri = carrier.geometric;
	ri.ptCoord = Point2( Scalar( 0.5 ), Scalar( 0 ) );

	// Warm the memo with `early`, then ask `late` at the same hit.
	const Scalar gotEarly = early->GetColor( ri )[0];
	const Scalar gotLate  = late->GetColor( ri )[0];
	CheckExact( gotEarly, Scalar( 0.25 ) * Scalar( 10 ) + Scalar( 0.5 ), "(h) the early painter reads its own time" );
	CheckExact( gotLate,  Scalar( 0.75 ) * Scalar( 10 ) + Scalar( 0.5 ), "(h) the late painter is NOT served the early one's entry" );

	// And the keyframe path itself: moving `time` on a live painter must
	// be visible immediately, because the new value is part of the key.
	Parameter<Scalar> moved( Scalar( 0.5 ), 300 );
	early->SetIntermediateValue( moved );
	CheckExact( early->GetColor( ri )[0], Scalar( 0.5 ) * Scalar( 10 ) + Scalar( 0.5 ),
		"(h) a keyframed `time` change is visible through the memo with no bump" );

	late->release();
	early->release();
}

//======================================================================
// (k) L2: EVERY key field separates, one at a time
//======================================================================

//! WHY THE (a) SWEEP IS NOT THIS CHECK.  (a) draws each field from a
//! four-value grid and compares memo-on against memo-off over 10 500
//! draws -- which looks like it would catch a dropped key field, and does
//! not.  Two draws collide only if they agree in EVERY OTHER field, and
//! that essentially never happens: a review measured zero aliasing draws
//! across the sweep for every L2 field it varies, and the RED-PROOF below
//! confirms the consequence directly.  (`ctx.signals` is constant within
//! (a), and (f)'s stub provider ignores position, so neither covers this
//! either.)
//!
//! RED-PROVED 2026-09-07: dropping `fwo` from ProgramKey::Equals turns
//! exactly ONE assertion red -- this test's `fwo` row -- and leaves (a)'s
//! 10 500-draw differential, and every other check in the file, GREEN.
//! So the field really could have left the key unnoticed, and this is the
//! check that notices.
//!
//! SO THIS ONE SPLICES, the way (i) does for the mesh channel: warm the
//! memo at ONE base context, perturb EXACTLY ONE key field, and require
//! the answer to equal that perturbed context's MEMO-OFF answer.  A
//! dropped field returns the base's value instead, which the check first
//! proves is a different number.
//!
//! ALL FIFTEEN record-reachable fields are covered here (u, v, P.xyz,
//! Po.xyz, N.xyz, fw, fwo, curv, curvR).  The other three of ProgramKey's
//! eighteen have checks of their own: `progId` is (c), `pipe` is (m),
//! and `time` is (h) -- `time` is NOT perturbed by this test's mechanism
//! because it does not come from the compiled body's own inputs the way
//! the fifteen above do; it is the painter's own `m_time`, which is why
//! (h) drives it from two painters sharing one program instead.  `curv`
//! and `curvR` need the base's `scaleHint` at ZERO, which is what lets
//! them move independently -- BuildContext computes
//! `curv = curvature * scaleHint` and `curvR = curvature`, so at
//! scaleHint 0 moving `scaleHint` moves only `curv`, and moving
//! `curvature` moves only `curvR`.
//!
//! The sixteenth check, on the SIGNAL CHANNEL, is weaker on purpose and
//! says so: perturbing `signals.ptObject` has to be separated by BOTH
//! levels (the channel is in the L1 key AND, whole, in the L2 key), so a
//! failure there does not say which level dropped it.  What it does prove
//! is that ProgramKey::Equals compares `signals` at all.  Field-by-field
//! separation of the channel's own eleven is (l)'s job, on the same
//! SignalHitKey::Equals that ProgramKey uses.
static void TestL2KeyFieldSeparation()
{
	std::cout << "(k) L2: every key field separates, one field at a time" << std::endl;

	SDFGeometry* g = BuildSdfCreasedPair( 0.7 );
	Object* o = new Object( g );
	g->release();
	o->FinalizeTransformations();

	RayIntersection crease = MkRI( Point3( 0, 30, 0 ), Vector3( 0, -1, 0 ) );
	Check( HitObject( o, crease ), "(k) crease hit" );
	const SurfaceSignalInfo live = crease.geometric.signals;
	Check( live.pProvider != 0, "(k) the SDF hit publishes a live signal provider" );

	// A body that reads EVERY context variable and calls a signal, so
	// every key field is load-bearing for the answer.  The weights are
	// mutually incommensurate so no two perturbations can cancel.
	ExpressionProgram prog = ExpressionProgram::Invalid();
	Check( CompileWithContext(
		"vec3( occlusion(0.2)*0.37 + fbm(P*3.0 + Po*5.0, 3, 0.5, 2.0)*0.29 + fw*11.0 + fwo*13.0,"
		"      u*0.41 + v*0.43 + curv*0.47 + curvR*0.53 + time*0.59,"
		"      N.x*0.61 + N.y*0.67 + N.z*0.71 )", prog ), "(k) body compiles" );
	Check( prog.MemoWorthy(), "(k) the body is memo-worthy" );

	std::vector<ParamSpec> specs;
	ExpressionPainter* painter = new ExpressionPainter( prog, specs, Scalar( 0 ) );

	// THE BASE.  `scaleHint` is 0 for the curv/curvR reason above.
	Draw base;
	base.ctx.u  = Scalar( 0.11 );  base.ctx.v = Scalar( 0.23 );
	base.ctx.P  = Vector3( Scalar( 0.31 ), Scalar( 0.37 ), Scalar( 0.41 ) );
	base.ctx.Po = Vector3( Scalar( 0.43 ), Scalar( 0.47 ), Scalar( 0.53 ) );
	base.ctx.N  = Vector3( Scalar( 0.59 ), Scalar( 0.61 ), Scalar( 0.67 ) );
	base.ctx.fw   = Scalar( 0.0071 );
	base.ctx.fwo  = Scalar( 0.0079 );
	base.ctx.time = Scalar( 0 );
	base.curvature = Scalar( 0.83 );
	base.scaleHint = Scalar( 0 );
	base.ctx.curvR = base.curvature;
	base.ctx.curv  = base.curvature * base.scaleHint;
	base.ctx.signals = live;

	// The sixteen perturbations, each touching exactly one key field.
	struct Case { const char* name; Draw d; };
	std::vector<Case> cases;
	{
		#define PERTURB( label, mutation ) \
			do { Case c; c.name = label; c.d = base; { Draw& d = c.d; (void)d; mutation; } cases.push_back( c ); } while( 0 )

		PERTURB( "u",            d.ctx.u  = Scalar( 0.19 ) );
		PERTURB( "v",            d.ctx.v  = Scalar( 0.29 ) );
		PERTURB( "P.x",          d.ctx.P.x  = Scalar( 0.73 ) );
		PERTURB( "P.y",          d.ctx.P.y  = Scalar( 0.79 ) );
		PERTURB( "P.z",          d.ctx.P.z  = Scalar( 0.89 ) );
		PERTURB( "Po.x",         d.ctx.Po.x = Scalar( 0.97 ) );
		PERTURB( "Po.y",         d.ctx.Po.y = Scalar( 1.03 ) );
		PERTURB( "Po.z",         d.ctx.Po.z = Scalar( 1.09 ) );
		PERTURB( "N.x",          d.ctx.N.x  = Scalar( 1.13 ) );
		PERTURB( "N.y",          d.ctx.N.y  = Scalar( 1.19 ) );
		PERTURB( "N.z",          d.ctx.N.z  = Scalar( 1.27 ) );
		PERTURB( "fw",           d.ctx.fw   = Scalar( 0.0131 ) );
		PERTURB( "fwo",          d.ctx.fwo  = Scalar( 0.0139 ) );
		// scaleHint moves `curv` alone (curvR = curvature is untouched).
		PERTURB( "curv",         d.scaleHint = Scalar( 1.9 );
		                         d.ctx.curv  = d.curvature * d.scaleHint );
		// curvature moves `curvR` alone (curv = curvature * 0 stays 0).
		PERTURB( "curvR",        d.curvature = Scalar( 1.29 );
		                         d.ctx.curvR = d.curvature;
		                         d.ctx.curv  = d.curvature * d.scaleHint );
		// The signal channel, whole -- see this test's header for why this
		// one check cannot localise a failure to L2.
		PERTURB( "signals",      d.ctx.signals.ptObject =
		                             Point3( live.ptObject.x + Scalar( 0.31 ),
		                                     live.ptObject.y,
		                                     live.ptObject.z ) );
		#undef PERTURB
	}

	// Memo-OFF references: the base's answer, and each perturbation's.
	RISEPel refBase;
	std::vector<RISEPel> refCase( cases.size() );
	{
		MemoSwitch off( 0 );
		refBase = painter->GetColor( MakeHit( crease.geometric, base ) );
		for( size_t i = 0; i < cases.size(); ++i ) {
			refCase[i] = painter->GetColor( MakeHit( crease.geometric, cases[i].d ) );
		}
	}

	for( size_t i = 0; i < cases.size(); ++i ) {
		const std::string tag = std::string( "(k) " ) + cases[i].name;

		// TEETH: the perturbation must actually move the answer, or a
		// dropped field would return the right number by accident.
		const bool moved = refCase[i][0] != refBase[0]
		                || refCase[i][1] != refBase[1]
		                || refCase[i][2] != refBase[2];
		Check( moved, tag + ": the perturbation really moves the answer (the check has teeth)" );

		MemoSwitch on( 1 );
		ExpressionMemo::Invalidate();
		painter->GetColor( MakeHit( crease.geometric, base ) );		// warm, base only
		const RISEPel got = painter->GetColor( MakeHit( crease.geometric, cases[i].d ) );

		const bool ok = got[0] == refCase[i][0] && got[1] == refCase[i][1] && got[2] == refCase[i][2];
		Check( ok, tag + ": a context differing ONLY here is not served the base's entry" );
	}

	painter->release();
	o->release();
}

//======================================================================
// (l) L1: EVERY key field separates, one at a time
//======================================================================

//! A provider whose answer depends on EVERY field of SurfaceSignalInfo
//! and on both query fields, which is what makes a one-field-at-a-time
//! separation check possible at all.  The shipping providers cannot do
//! this job: the SDF family reads position and normal but not primId or
//! the barycentrics, and the mesh family reads only (primId, bary) -- so
//! neither would notice most of the key leaving SignalHitKey::Equals.
//!
//! A plain weighted sum with mutually incommensurate weights, folded to
//! [0,1) by subtracting its floor.  In range on purpose: SignalQuery
//! clamps to [0,1], and a clamped answer would flatten two different
//! inputs into one and hide exactly the difference under test.
//!
//! `m_salt` is what makes PROVIDER IDENTITY observable -- two instances
//! with different salts answer differently for the same record, which is
//! how the `pProvider` field gets a check.
class FieldSensitiveProvider : public ISurfaceSignalProvider
{
public:
	explicit FieldSensitiveProvider( const double salt ) : m_salt( salt ) {}

	bool ComputeOcclusion( const SurfaceSignalInfo& s, const Scalar r, const bool c, Scalar& out ) const override
	{ out = Mix( s, r, c, 0.1013 ); return true; }
	bool ComputeThickness( const SurfaceSignalInfo& s, const Scalar r, const bool c, Scalar& out ) const override
	{ out = Mix( s, r, c, 0.2111 ); return true; }
	bool ComputeConvexity( const SurfaceSignalInfo& s, const Scalar r, const bool c, Scalar& out ) const override
	{ out = Mix( s, r, c, 0.3177 ); return true; }

private:
	Scalar Mix( const SurfaceSignalInfo& s, const Scalar r, const bool constR, const double kindW ) const
	{
		const double v = m_salt * 0.7717
			+ (double)s.ptObject.x * 1.13 + (double)s.ptObject.y * 1.27 + (double)s.ptObject.z * 1.39
			+ (double)s.nObject.x  * 1.51 + (double)s.nObject.y  * 1.63 + (double)s.nObject.z  * 1.77
			+ (double)s.primId * 0.01931
			+ (double)s.baryA * 1.91 + (double)s.baryB * 2.03
			+ ( s.bComplementedField ? 0.2819 : 0.0 )
			// The CROSS-OBJECT four (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md
			// §5.1).  No shipping provider reads them -- SignalQuery hands
			// the whole record to occlusion/thickness/convexity, none of
			// which cares where in the scene the hit is -- so without this
			// there is no way to give a one-field-at-a-time L1 separation
			// check TEETH for them.  The two POINTERS enter through a
			// low-address hash, not their raw value: what the key must
			// distinguish is "a different scene / a different self object",
			// and two live objects in one process differ in these bits.
			+ PtrTerm( s.pScene ) * 2.1713
			+ PtrTerm( s.pSelf )  * 2.3719
			+ (double)s.ptWorld.x * 2.5711 + (double)s.ptWorld.y * 2.7717 + (double)s.ptWorld.z * 2.9723
			+ (double)s.time * 3.1729
			+ (double)r * 2.1101
			+ ( constR ? 0.3739 : 0.0 )
			+ kindW;
		return (Scalar)( v - std::floor( v ) );		// -> [0,1), so SignalQuery's clamp is a no-op
	}

	//! A pointer, folded to a small deterministic number.  Deterministic
	//! WITHIN one run, which is all the checks need -- every reference
	//! value they compare against is computed in the same run, memo off.
	static double PtrTerm( const void* p )
	{
		const std::uintptr_t bits = ( reinterpret_cast<std::uintptr_t>( p ) >> 3 ) & 0x3FFFFFu;
		return (double)bits * 1.7e-5;
	}

	const double m_salt;
};

//! One L1 lookup, in the form the three wrappers present it.
struct L1Query
{
	SurfaceSignalInfo	s;
	Scalar				radius;
	int					kind;		//!< 0 occlusion, 1 thickness, 2 convexity
	bool				constR;
};

static Scalar RunL1( const L1Query& q )
{
	return q.kind == 0 ? q.s.Occlusion( q.radius, q.constR )
	     : q.kind == 1 ? q.s.Thickness( q.radius, q.constR )
	                   : q.s.Convexity( q.radius, q.constR );
}

//! The L1 twin of (k), and the same argument for it: (a)'s sweep never
//! aliases on a single dropped field, (f)'s provider ignores position
//! entirely, and (i) covers only the mesh family's three -- so eleven of
//! SignalKey's fourteen had no separation check that would go red if they
//! left Equals.  This one warms the memo at a base query, perturbs
//! EXACTLY one field, and requires the perturbed query's own memo-off
//! answer back.
//!
//! RED-PROVED 2026-09-07: dropping `baryB` from SignalHitKey::Equals
//! turns exactly ONE assertion red -- this test's `baryB` row -- and
//! leaves everything else green, (i) INCLUDED.  (i) splices primId, baryA
//! and baryB together, so with two of the three still compared it
//! separates the records anyway and never notices the third is gone.
//! That is precisely the gap one-field-at-a-time closes.
//!
//! TWENTY fields: the hit channel's SEVENTEEN plus the query's three
//! (radius, kind, constant-radius proof).  The hit channel's seventeen
//! are eleven OWN-SURFACE ones (provider identity, ptObject xyz, nObject
//! xyz, primId, baryA, baryB, bComplementedField) and, since 2026-09-08,
//! six CROSS-OBJECT ones (pScene, pSelf, ptWorld xyz, time) that
//! `proximity(r)` reads -- docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §5.1.
//! All seventeen are the SAME SignalHitKey::Equals that ProgramKey uses
//! for its `signals` member, so this covers both levels' use of it.
//!
//! THE SIX CROSS-OBJECT ROWS NEED A PROVIDER THAT READS THEM, and no
//! shipping one does: occlusion / thickness / convexity are self-signals
//! and could not care less where in the scene the hit is.  So
//! FieldSensitiveProvider above reads all six, which is what gives these
//! rows teeth at the L1 level -- exactly the same trick that gave the
//! eleven own-surface rows theirs.  RED-PROVED 2026-09-08: dropping
//! `pSelf` from SignalHitKey::Equals turns exactly ONE assertion red --
//! this test's `pSelf` row -- and leaves every other check in the file
//! green, (a)'s 10 500-draw differential and (k) INCLUDED.  Dropping
//! `time` likewise reddens only the `signals.time` row.
static void TestL1KeyFieldSeparation()
{
	std::cout << "(l) L1: every key field separates, one field at a time" << std::endl;

	FieldSensitiveProvider providerA( 1.0 );
	FieldSensitiveProvider providerB( 5.0 );

	// Two real managers and two real objects for the cross-object
	// back-pointers.  Neither the memo (which stores them as opaque
	// addresses) nor FieldSensitiveProvider (which hashes them) ever
	// dereferences them, so a cast-from-`int` stand-in would work -- real
	// instances are used anyway because a key-layout test should not be the
	// one place in the tree that puts a non-IObjectManager address in an
	// `IObjectManager*`.  Empty managers and geometry-less objects: they
	// are identities here, nothing more.
	IObjectManager* sceneA = 0;
	IObjectManager* sceneB = 0;
	Check( RISE_API_CreateObjectManager( &sceneA, true, false, 4, 32 ), "(l) scene A" );
	Check( RISE_API_CreateObjectManager( &sceneB, true, false, 4, 32 ), "(l) scene B" );
	// Real (if trivial) geometry: `new Object( 0 )` logs "Geometry ptr was
	// passed in but is invalid" on every construction, and a test should
	// not print an error line to say nothing is wrong.
	SphereGeometry* dummyGeom = new SphereGeometry( Scalar( 1 ) );
	Object* selfA = new Object( dummyGeom );
	Object* selfB = new Object( dummyGeom );
	dummyGeom->release();

	L1Query base;
	base.s.pProvider          = &providerA;
	base.s.ptObject           = Point3( Scalar( 0.31 ), Scalar( 0.37 ), Scalar( 0.41 ) );
	base.s.nObject            = Vector3( Scalar( 0.59 ), Scalar( 0.61 ), Scalar( 0.67 ) );
	base.s.primId             = 7;
	base.s.baryA              = Scalar( 0.13 );
	base.s.baryB              = Scalar( 0.29 );
	base.s.bComplementedField = false;
	base.s.pScene             = sceneA;
	base.s.pSelf              = selfA;
	base.s.ptWorld            = Point3( Scalar( 1.31 ), Scalar( 1.37 ), Scalar( 1.41 ) );
	base.s.time               = Scalar( 2.17 );
	base.radius               = Scalar( 0.17 );
	base.kind                 = 0;
	base.constR               = true;

	struct Case { const char* name; L1Query q; };
	std::vector<Case> cases;
	{
		#define PERTURB1( label, mutation ) \
			do { Case c; c.name = label; c.q = base; { L1Query& q = c.q; (void)q; mutation; } cases.push_back( c ); } while( 0 )

		PERTURB1( "pProvider",          q.s.pProvider = &providerB );
		PERTURB1( "ptObject.x",         q.s.ptObject.x = Scalar( 0.73 ) );
		PERTURB1( "ptObject.y",         q.s.ptObject.y = Scalar( 0.79 ) );
		PERTURB1( "ptObject.z",         q.s.ptObject.z = Scalar( 0.89 ) );
		PERTURB1( "nObject.x",          q.s.nObject.x = Scalar( 1.13 ) );
		PERTURB1( "nObject.y",          q.s.nObject.y = Scalar( 1.19 ) );
		PERTURB1( "nObject.z",          q.s.nObject.z = Scalar( 1.27 ) );
		PERTURB1( "primId",             q.s.primId = 23 );
		PERTURB1( "baryA",              q.s.baryA = Scalar( 0.41 ) );
		PERTURB1( "baryB",              q.s.baryB = Scalar( 0.47 ) );
		PERTURB1( "bComplementedField", q.s.bComplementedField = true );
		// The four CROSS-OBJECT fields
		// (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §5.1).  `proximity(r)`
		// is a function of the SCENE and of WHERE IN IT the hit is, and
		// none of the eleven own-surface fields above can separate two of
		// its answers: two instances of one geometry agree in every one
		// of them at the same object-space point, and a hit whose
		// NEIGHBOUR moved between keyframes agrees in all of them.
		PERTURB1( "pScene",             q.s.pScene = sceneB );
		PERTURB1( "pSelf",              q.s.pSelf = selfB );
		PERTURB1( "ptWorld.x",          q.s.ptWorld.x = Scalar( 3.11 ) );
		PERTURB1( "ptWorld.y",          q.s.ptWorld.y = Scalar( 3.19 ) );
		PERTURB1( "ptWorld.z",          q.s.ptWorld.z = Scalar( 3.23 ) );
		PERTURB1( "signals.time",       q.s.time = Scalar( 7.13 ) );
		PERTURB1( "radius",             q.radius = Scalar( 0.53 ) );
		PERTURB1( "fn (signal kind)",   q.kind = 2 );
		PERTURB1( "bRadiusIsConstant",  q.constR = false );

		#undef PERTURB1
	}

	Scalar refBase = Scalar( 0 );
	std::vector<Scalar> refCase( cases.size() );
	{
		MemoSwitch off( 0 );
		refBase = RunL1( base );
		for( size_t i = 0; i < cases.size(); ++i ) refCase[i] = RunL1( cases[i].q );
	}

	for( size_t i = 0; i < cases.size(); ++i ) {
		const std::string tag = std::string( "(l) " ) + cases[i].name;
		Check( refCase[i] != refBase,
			tag + ": the perturbation really moves the answer (the check has teeth)" );

		MemoSwitch on( 1 );
		ExpressionMemo::Invalidate();
		RunL1( base );							// warm, base only
		CheckExact( RunL1( cases[i].q ), refCase[i],
			tag + ": a query differing ONLY here is not served the base's entry" );
	}

	selfB->release();
	selfA->release();
	safe_release( sceneB );
	safe_release( sceneA );
}

//======================================================================
// (n) L2: the CROSS-OBJECT channel separates too
//======================================================================

//! (k)'s twin for the four fields `proximity(r)` added
//! (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §5.1), and it needs its own
//! test rather than four more (k) rows for one reason: (k)'s base carries
//! a LIVE SDF provider, and the SDF family does not read the
//! cross-object fields at all -- so perturbing `signals.pScene` there
//! would leave the answer unmoved and the check would have no teeth.
//! This one stamps a FieldSensitiveProvider (which reads all seventeen)
//! into the channel instead, and is otherwise (k)'s mechanism exactly:
//! warm at one context, perturb EXACTLY one field, require the perturbed
//! context's own memo-off answer back.
//!
//! FIVE PERTURBATIONS, not six.  `signals.time` cannot be driven this
//! way, and that is a property of the design rather than a gap in the
//! test: `ExpressionPainter::BuildContext` STAMPS `ctx.signals.time` from
//! its own `m_time`, so whatever a caller puts on the record is
//! overwritten before the key is made.  The check for it is the sixth
//! assertion below -- two painters over one program with different
//! `m_time` land on different entries, which is (h)'s mechanism applied
//! to the channel's copy of the same number.
static void TestL2CrossObjectChannelSeparation()
{
	std::cout << "(n) L2: the cross-object channel (pScene / pSelf / ptWorld / time) separates" << std::endl;

	FieldSensitiveProvider provider( 3.0 );

	IObjectManager* sceneA = 0;
	IObjectManager* sceneB = 0;
	Check( RISE_API_CreateObjectManager( &sceneA, true, false, 4, 32 ), "(n) scene A" );
	Check( RISE_API_CreateObjectManager( &sceneB, true, false, 4, 32 ), "(n) scene B" );
	// Real (if trivial) geometry: `new Object( 0 )` logs "Geometry ptr was
	// passed in but is invalid" on every construction, and a test should
	// not print an error line to say nothing is wrong.
	SphereGeometry* dummyGeom = new SphereGeometry( Scalar( 1 ) );
	Object* selfA = new Object( dummyGeom );
	Object* selfB = new Object( dummyGeom );
	dummyGeom->release();

	ExpressionProgram prog = ExpressionProgram::Invalid();
	Check( CompileWithContext(
		"vec3( occlusion(0.2), thickness(0.3), convexity(0.4) )", prog ), "(n) body compiles" );
	Check( prog.MemoWorthy(), "(n) the body is memo-worthy (it calls signals)" );

	std::vector<ParamSpec> specs;
	ExpressionPainter* painter = new ExpressionPainter( prog, specs, Scalar( 0 ) );

	RayIntersection carrier = MkRI( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );

	SurfaceSignalInfo live;
	live.pProvider = &provider;
	live.ptObject  = Point3( Scalar( 0.31 ), Scalar( 0.37 ), Scalar( 0.41 ) );
	live.nObject   = Vector3( Scalar( 0.59 ), Scalar( 0.61 ), Scalar( 0.67 ) );
	live.primId    = 5;
	live.baryA     = Scalar( 0.13 );
	live.baryB     = Scalar( 0.29 );
	live.pScene    = sceneA;
	live.pSelf     = selfA;
	live.ptWorld   = Point3( Scalar( 1.31 ), Scalar( 1.37 ), Scalar( 1.41 ) );

	Draw base;
	base.ctx.u = Scalar( 0.11 ); base.ctx.v = Scalar( 0.23 );
	base.ctx.P  = Vector3( Scalar( 0.31 ), Scalar( 0.37 ), Scalar( 0.41 ) );
	base.ctx.Po = Vector3( Scalar( 0.43 ), Scalar( 0.47 ), Scalar( 0.53 ) );
	base.ctx.N  = Vector3( Scalar( 0.59 ), Scalar( 0.61 ), Scalar( 0.67 ) );
	base.ctx.fw = Scalar( 0.0071 ); base.ctx.fwo = Scalar( 0.0079 );
	base.ctx.time = Scalar( 0 );
	base.curvature = Scalar( 0.83 ); base.scaleHint = Scalar( 0 );
	base.ctx.curvR = base.curvature; base.ctx.curv = Scalar( 0 );
	base.ctx.signals = live;

	struct Case { const char* name; Draw d; };
	std::vector<Case> cases;
	{
		#define PERTURB2( label, mutation ) \
			do { Case c; c.name = label; c.d = base; { Draw& d = c.d; (void)d; mutation; } cases.push_back( c ); } while( 0 )

		PERTURB2( "signals.pScene",    d.ctx.signals.pScene = sceneB );
		PERTURB2( "signals.pSelf",     d.ctx.signals.pSelf  = selfB );
		PERTURB2( "signals.ptWorld.x", d.ctx.signals.ptWorld.x = Scalar( 4.11 ) );
		PERTURB2( "signals.ptWorld.y", d.ctx.signals.ptWorld.y = Scalar( 4.19 ) );
		PERTURB2( "signals.ptWorld.z", d.ctx.signals.ptWorld.z = Scalar( 4.23 ) );

		#undef PERTURB2
	}

	RISEPel refBase;
	std::vector<RISEPel> refCase( cases.size() );
	{
		MemoSwitch off( 0 );
		refBase = painter->GetColor( MakeHit( carrier.geometric, base ) );
		for( size_t i = 0; i < cases.size(); ++i ) {
			refCase[i] = painter->GetColor( MakeHit( carrier.geometric, cases[i].d ) );
		}
	}

	for( size_t i = 0; i < cases.size(); ++i ) {
		const std::string tag = std::string( "(n) " ) + cases[i].name;
		const bool moved = refCase[i][0] != refBase[0]
		                || refCase[i][1] != refBase[1]
		                || refCase[i][2] != refBase[2];
		Check( moved, tag + ": the perturbation really moves the answer (the check has teeth)" );

		MemoSwitch on( 1 );
		ExpressionMemo::Invalidate();
		painter->GetColor( MakeHit( carrier.geometric, base ) );		// warm, base only
		const RISEPel got = painter->GetColor( MakeHit( carrier.geometric, cases[i].d ) );
		const bool ok = got[0] == refCase[i][0] && got[1] == refCase[i][1] && got[2] == refCase[i][2];
		Check( ok, tag + ": a context differing ONLY here is not served the base's entry" );
	}

	// `signals.time`, driven the only way it can be: two painters over ONE
	// compiled program, differing only in `m_time`.  BuildContext stamps
	// BOTH `ctx.time` and `ctx.signals.time` from it, so this asserts that
	// the pair lands on different entries -- and, with a provider that
	// reads `signals.time` and a body that does NOT read `time`, that the
	// CHANNEL's copy is what carries the difference through L1.
	{
		ExpressionProgram sigOnly = ExpressionProgram::Invalid();
		Check( CompileWithContext( "vec3( occlusion(0.2), 0, 0 )", sigOnly ),
			"(n) signal-only body compiles" );
		ExpressionPainter* pT0 = new ExpressionPainter( sigOnly, specs, Scalar( 0 ) );
		ExpressionPainter* pT1 = new ExpressionPainter( sigOnly, specs, Scalar( 11.5 ) );

		RISEPel r0, r1;
		{
			MemoSwitch off( 0 );
			r0 = pT0->GetColor( MakeHit( carrier.geometric, base ) );
			r1 = pT1->GetColor( MakeHit( carrier.geometric, base ) );
		}
		Check( r0[0] != r1[0], "(n) signals.time: the two painters' times really move the answer" );

		MemoSwitch on( 1 );
		ExpressionMemo::Invalidate();
		pT0->GetColor( MakeHit( carrier.geometric, base ) );		// warm at time 0
		const RISEPel got = pT1->GetColor( MakeHit( carrier.geometric, base ) );
		Check( got[0] == r1[0],
			"(n) signals.time: a painter at a different time is not served the other's entry" );

		pT1->release();
		pT0->release();
	}

	painter->release();
	selfB->release();
	selfA->release();
	safe_release( sceneB );
	safe_release( sceneA );
}

//======================================================================
// (o) the PROXIMITY entry clears on a generation bump
//======================================================================

//! (b) red-proves the generation counter for the SELF-signals, using a
//! provider whose answer is a member the test can move.  `proximity` needs
//! its own version of that check for a structural reason: it has NO
//! provider.  Its answer is a function of the SCENE -- of where other
//! objects are -- and a scene is exactly the kind of state that CAN change
//! behind an unchanged key, because a NEIGHBOUR can move while the
//! receiver's own hit point, and therefore every other field of the key,
//! stays bit-identical.  That is the one way this signal's staleness
//! differs from the other three (design 5.4), so it is the one that has to
//! be checked directly rather than inherited from (b).
//!
//! THE FIXTURE MOVES THE NEIGHBOUR FARTHER, not nearer, and that is
//! deliberate: `PrepareForRendering` itself bumps the generation (twice),
//! so a check that had to call it could never observe a stale entry at
//! all.  Moving the box from y = 3 to y = 5 changes the answer (2 -> 4)
//! without needing the world-AABB snapshot rebuilt -- the stale box,
//! expanded by the query radius, still admits the probe point, and
//! `DistanceToSurface` reads the LIVE transform.  So the ONLY thing
//! standing between the two answers is the memo bump.
//!
//! Both halves are asserted, which makes this its own red-proof: WITHOUT
//! the bump the stale answer comes back (proving an entry really was
//! held), and WITH it the fresh one does.
static void TestProximityEntryClearsOnBump()
{
	std::cout << "(o) a proximity entry (fn = 3) is cleared by a generation bump" << std::endl;

	IObjectManager* mgr = 0;
	Check( RISE_API_CreateObjectManager( &mgr, true, false, 4, 32 ), "(o) a manager" );
	if( !mgr ) return;

	SphereGeometry* gFloor = new SphereGeometry( Scalar( 1 ) );
	Object* receiver = new Object( gFloor );
	gFloor->release();
	receiver->FinalizeTransformations();
	mgr->AddItem( receiver, "receiver" );

	SphereGeometry* gBox = new SphereGeometry( Scalar( 1 ) );
	Object* mover = new Object( gBox );
	gBox->release();
	mover->SetPosition( Point3( 0, 3, 0 ) );
	mover->FinalizeTransformations();
	mgr->AddItem( mover, "mover" );

	mgr->PrepareForRendering();

	SurfaceSignalInfo s;
	s.pScene  = mgr;
	s.pSelf   = receiver;
	s.ptWorld = Point3( 0, 0, 0 );

	MemoSwitch on( 1 );
	ExpressionMemo::Invalidate();
	const Scalar warm = s.Proximity( Scalar( 8 ) );		// 1 - 2/8
	CheckExact( warm, Scalar( 0.75 ), "(o) the neighbour 2 away reads 1 - 2/8" );

	// MOVE IT, and do NOT bump.
	mover->SetPosition( Point3( 0, 5, 0 ) );
	mover->FinalizeTransformations();

	const Scalar stale = s.Proximity( Scalar( 8 ) );
	CheckExact( stale, warm,
		"(o) RED-PROOF -- without a bump the STALE entry is served, so there really was one" );

	ExpressionMemo::Invalidate();
	const Scalar fresh = s.Proximity( Scalar( 8 ) );		// 1 - 4/8
	CheckExact( fresh, Scalar( 0.5 ),
		"(o) MONEY -- the bump clears the proximity entry and the moved neighbour is seen" );

	mover->release();
	receiver->release();
	safe_release( mgr );
}

//======================================================================
// (m) the two painter pipes never share an L2 entry
//======================================================================

//! THE HAZARD, and why it is not hypothetical.  On a SCALAR-typed program
//! the colour pipe calls `EvalVec3` and broadcasts, while the scalar pipe
//! calls `Eval` -- each keeping the entry point it had before the memo,
//! deliberately, because under `-ffast-math` the two are free to differ
//! in the last ulp (ExpressionPainter.cpp's own comments).  And ONE
//! compiled program really can reach both: RISE_API_CreateExpressionPainter
//! and RISE_API_CreateExpressionScalarPainter each take an
//! `ExpressionProgram` by const reference, each painter holds a copy, and
//! a copy KEEPS its source's id.  Without a pipe tag in the L2 key the
//! second pipe to ask would be served the first's entry -- the scalar
//! pipe returning `EvalVec3(...).x`.
//!
//! Built through the API on purpose: that IS the reachability claim, and
//! constructing the two painters directly would prove something weaker.
//! Run in both orders, so neither pipe is only ever the one that fills.
//!
//! WHAT THIS CHECK CANNOT DO, said plainly so nobody reads more into it
//! than it carries.  RED-PROOF ATTEMPTED 2026-09-07: removing `pipe` from
//! ProgramKey::Equals leaves every assertion below GREEN.  That is not a
//! defect in the checks -- it is the honest state of the hazard.  Without
//! the field the two pipes really do share one entry (structural, and
//! readable straight off the key), but on this toolchain the values they
//! would then swap are BIT-IDENTICAL: `Eval` and `EvalVec3` reach the same
//! `RunAny` over the same `env[]`, and only an inlining / FMA-contraction
//! difference could separate them, which is not something a test can
//! summon on demand.
//!
//! So what is pinned here is the CONTRACT -- each pipe returns its own
//! answer, in both fill orders, with both entries coexisting -- and it
//! will go red the day those two entry points do diverge, which is
//! precisely when it matters.  ExpressionMemo::EvalPipe's comment carries
//! the same disclosure and the argument for keeping the field anyway.
static void TestCrossPipeL2Separation()
{
	std::cout << "(m) a scalar-typed program shared across both pipes keys apart" << std::endl;

	ExpressionProgram prog = ExpressionProgram::Invalid();
	Check( CompileWithContext(
		"occlusion(0.2)*0.37 + fbm(P*3.0, 3, 0.5, 2.0)*0.29 + u*0.41 + v*0.43", prog ),
		"(m) body compiles" );
	Check( prog.ResultType() == ExpressionProgram::kScalar,
		"(m) the body is SCALAR-typed (the case where the two pipes call different entry points)" );
	Check( prog.MemoWorthy(), "(m) the body is memo-worthy" );

	std::vector<ParamSpec> specs;
	IPainter*       colour = 0;
	IScalarPainter* scalar = 0;
	Check( RISE_API_CreateExpressionPainter( &colour, prog, specs, Scalar( 0 ) ),
		"(m) the API builds a colour painter over it" );
	Check( RISE_API_CreateExpressionScalarPainter( &scalar, prog, specs ),
		"(m) the API builds a scalar painter over the SAME program" );
	if( !colour || !scalar ) { if( colour ) colour->release(); if( scalar ) scalar->release(); return; }

	SDFGeometry* g = BuildSdfCreasedPair( 0.7 );
	Object* o = new Object( g );
	g->release();
	o->FinalizeTransformations();
	RayIntersection crease = MkRI( Point3( 0, 30, 0 ), Vector3( 0, -1, 0 ) );
	Check( HitObject( o, crease ), "(m) crease hit" );
	RayIntersectionGeometric& ri = crease.geometric;
	ri.ptCoord = Point2( Scalar( 0.11 ), Scalar( 0.23 ) );

	Scalar refColour = 0, refScalar = 0;
	{
		MemoSwitch off( 0 );
		refColour = colour->GetColor( ri )[0];
		refScalar = scalar->GetValuesAt( ri ).v[0];
	}

	// Colour first, then scalar.
	{
		MemoSwitch on( 1 );
		ExpressionMemo::Invalidate();
		CheckExact( colour->GetColor( ri )[0], refColour, "(m) colour-then-scalar: the colour pipe reads its own answer" );
		CheckExact( scalar->GetValuesAt( ri ).v[0], refScalar,
			"(m) colour-then-scalar: the scalar pipe is NOT served the colour pipe's entry" );
	}

	// Scalar first, then colour.
	{
		MemoSwitch on( 1 );
		ExpressionMemo::Invalidate();
		CheckExact( scalar->GetValuesAt( ri ).v[0], refScalar, "(m) scalar-then-colour: the scalar pipe reads its own answer" );
		CheckExact( colour->GetColor( ri )[0], refColour,
			"(m) scalar-then-colour: the colour pipe is NOT served the scalar pipe's entry" );
	}

	// And both survive being asked again, so one did not simply evict the
	// other's entry rather than keying apart from it.
	{
		MemoSwitch on( 1 );
		ExpressionMemo::Invalidate();
		colour->GetColor( ri );
		scalar->GetValuesAt( ri );
		CheckExact( colour->GetColor( ri )[0], refColour, "(m) both entries coexist (colour)" );
		CheckExact( scalar->GetValuesAt( ri ).v[0], refScalar, "(m) both entries coexist (scalar)" );
	}

	o->release();
	scalar->release();
	colour->release();
}

int main( int argc, char** argv )
{
	// The child half of (j), before anything else can warm a table or
	// pin EnabledSlow()'s options read.  It reports through its exit
	// status; the parent turns that into one check.
	if( argc > 1 && std::string( argv[1] ) == kOptionChildFlag ) {
		return RunOptionChild();
	}

	std::cout << "=== ExpressionMemoTest (two-level per-hit expression memo) ===" << std::endl;

	// FIRST, for the reason its own comment gives: it is the only check
	// that runs with no MemoSwitch in force, so it must ask before any
	// other check has pinned the options read.
	TestOptionDefaultAndKey( argv[0] );

	TestDifferentialEquality();
	TestGenerationAndKillSwitchRedProof();
	TestProgramIdRedProof();
	TestThreadIsolation();
	TestNeutralsAndKindSeparation();
	TestMemoWorthinessGate();
	TestKeyedOnPainterTime();
	TestRealMeshKeyFields();
	TestL2KeyFieldSeparation();
	TestL1KeyFieldSeparation();
	TestL2CrossObjectChannelSeparation();
	TestProximityEntryClearsOnBump();
	TestCrossPipeL2Separation();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << "   Failed: " << failCount;
	if( skipCount ) std::cout << "   Skipped: " << skipCount;
	std::cout << std::endl;
	return failCount == 0 ? 0 : 1;
}
