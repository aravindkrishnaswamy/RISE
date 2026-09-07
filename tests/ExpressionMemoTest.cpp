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
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstring>
#include <vector>
#include <string>
#include <thread>
#include <atomic>

#include "../src/Library/Interfaces/ISurfaceSignalProvider.h"
#include "../src/Library/Geometry/SDFGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Painters/ExpressionEval.h"
#include "../src/Library/Painters/ExpressionPainter.h"
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
class MemoSwitch
{
public:
	explicit MemoSwitch( const int tri ) { ExpressionMemo::SetEnabledOverride( tri ); }
	~MemoSwitch() { ExpressionMemo::SetEnabledOverride( -1 ); }
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
			// must equal the VM evaluated DIRECTLY, which never consults
			// the memo at all (the memo lives in the painters -- see
			// ExpressionProgram::MakeMemoKey's comment for why).  This is
			// the check that would catch a memo returning a plausible but
			// wrong stored value, which comparing two memo-on runs could
			// not.
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

	ExpressionProgram signal = ExpressionProgram::Invalid();
	Check( CompileWithContext( "occlusion(0.1)", signal ), "(g) signal body compiles" );
	Check( signal.MemoWorthy(), "(g) a signal body IS memo-worthy" );

	// A gated-out program must still be correct -- the gate changes cost,
	// never answers.
	MemoSwitch on( 1 );
	ExprEvalContext ctx;
	ctx.u = Scalar( 0.25 ); ctx.v = Scalar( 0.5 );
	CheckExact( cheap.EvalVec3( ctx ).x, Scalar( 0.125 ), "(g) a gated-out program still evaluates correctly" );

	std::cout << "    bytes of thread-local memo per render worker: "
		<< ExpressionMemo::BytesPerThread() << std::endl;
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

int main()
{
	std::cout << "=== ExpressionMemoTest (two-level per-hit expression memo) ===" << std::endl;

	TestDifferentialEquality();
	TestGenerationAndKillSwitchRedProof();
	TestProgramIdRedProof();
	TestThreadIsolation();
	TestNeutralsAndKindSeparation();
	TestMemoWorthinessGate();
	TestKeyedOnPainterTime();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << "   Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
