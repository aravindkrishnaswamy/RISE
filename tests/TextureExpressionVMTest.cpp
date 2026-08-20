//////////////////////////////////////////////////////////////////////
//
//  TextureExpressionVMTest.cpp - Slice S1 of the texture-expressions
//  arc (docs/agentic-redesign/88-procedural-texture-expressiveness-
//  candidates.md): the ExpressionEval VM's new vec3 type system,
//  context variables (P, Po, N, fw, time), noise builtins (factored
//  into ProceduralNoiseCore.h, shared with Perlin3DPainter/
//  Worley3DPainter), the variadic `ramp` builtin, offset-carrying
//  compile errors, and the param-metadata line grammar
//  (ExpressionParamSpec.h).  No scene chunks are exercised here (S1 is
//  headless VM work; S2 wires chunk surfaces).
//
//  Golden values for the noise builtins were computed by an
//  INDEPENDENT Python re-implementation of ProceduralNoiseCore's
//  documented formulas (LatticeHash3D's hash chain, the 26-neighbor
//  smoothing weights, trilinear blend, the Worley 3x3x3 jittered-grid
//  search) and cross-checked against the actual C++ output before
//  being pasted in below -- see the arc's implementation notes.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <limits>
#include <cstdio>
#include <string>

#include "../src/Library/Painters/ExpressionEval.h"
#include "../src/Library/Painters/ExpressionParamSpec.h"
#include "../src/Library/Utilities/ProceduralNoiseCore.h"
#include "../src/Library/Noise/PerlinNoise.h"
#include "../src/Library/Noise/WorleyNoise.h"
#include "../src/Library/Utilities/SimpleInterpolators.h"

// S2 (chunk surfaces) additions below TestContextVarGating() -- drives the
// two new chunks (expression_painter, scalar_painter{expression}) through a
// real Job via the CST loader, exactly like GuillocheChunkParseTest does for
// expression_function2d.
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Painters/ExpressionPainter.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"

// S3 (P2.1 + P2.2: scalar_painter{painter} bridge, ramp_painter) additions
// below TestExpressionPainterScalarBroadcastOnColorPipe().
#include "../src/Library/Painters/PainterChannelScalarPainter.h"
#include "../src/Library/Painters/RampPainter.h"
#include "../src/Library/Utilities/Color/RGBSpectra.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/IRasterImageWriter.h"
#include "../src/Library/Interfaces/IWriteBuffer.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const std::string& name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}
static void CheckClose( Scalar got, Scalar want, Scalar tol, const std::string& name )
{
	if( std::fabs( got - want ) <= tol ) { ++passCount; }
	else {
		++failCount;
		std::cout.precision( 15 );
		std::cout << "  FAIL: " << name << "  got " << got << "  want " << want << "  |d| " << std::fabs(got-want) << std::endl;
	}
}

//======================================================================
// Small harness around ExpressionProgram::Builder for one-shot compiles.
// Context vars (P, Po, N, fw, time) are opt-in (EnableContextVars, P2-A,
// review round 1) -- this harness exercises the full language, so it
// turns them on unconditionally.  (The two document-level `expr(...)`
// surfaces -- Cst.cpp's EvalExprBody, AgentSession.cpp's
// LocalEvalExprBody -- deliberately leave them OFF; see those files.)
//======================================================================
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

static size_t Find( const std::string& hay, const std::string& needle )
{
	const size_t p = hay.find( needle );
	Check( p != std::string::npos, "test bug: `" + needle + "` not found in `" + hay + "`" );
	return p;
}

//======================================================================
// 1. vec3 construction, swizzles, arithmetic (incl. scalar broadcast),
//    dot/cross/length/normalize, vec3-mix.
//======================================================================
static void TestVec3Arithmetic()
{
	std::cout << "Test 1: vec3 construction / swizzle / arithmetic / dot,cross,length,normalize" << std::endl;

	{
		Prog p( "vec3(1,2,3).x + vec3(1,2,3).y*10 + vec3(1,2,3).z*100" );
		Check( p.ok, "vec3 ctor + swizzle compiles" );
		if( p.ok ) CheckClose( p.prog.Eval(0,0), 1+20+300, 1e-12, "vec3 ctor + swizzle values" );
	}
	{
		Prog p( "vec3(1,2,3) + vec3(4,5,6)" );
		Check( p.ok && p.prog.ResultType()==ExpressionProgram::kVec3, "vec3+vec3 result type" );
		const Vector3 v = p.prog.EvalVec3( ExprEvalContext() );
		CheckClose( v.x, 5, 1e-12, "vec3+vec3 .x" ); CheckClose( v.y, 7, 1e-12, "vec3+vec3 .y" ); CheckClose( v.z, 9, 1e-12, "vec3+vec3 .z" );
	}
	{
		Prog p( "vec3(1,2,3) - vec3(0.5,0.5,0.5)" );
		Check( p.ok, "vec3-vec3 compiles" );
		const Vector3 v = p.prog.EvalVec3( ExprEvalContext() );
		CheckClose( v.x, 0.5, 1e-12, "vec3-vec3 .x" ); CheckClose( v.y, 1.5, 1e-12, "vec3-vec3 .y" ); CheckClose( v.z, 2.5, 1e-12, "vec3-vec3 .z" );
	}
	{
		// scalar broadcast both directions: vec3*scalar and scalar*vec3
		Prog p1( "(vec3(1,2,3) * 2).y" );
		Check( p1.ok, "vec3*scalar compiles" );
		if( p1.ok ) CheckClose( p1.prog.Eval(0,0), 4, 1e-12, "vec3*scalar broadcast" );
		Prog p2( "(2 * vec3(1,2,3)).y" );
		Check( p2.ok, "scalar*vec3 compiles" );
		if( p2.ok ) CheckClose( p2.prog.Eval(0,0), 4, 1e-12, "scalar*vec3 broadcast" );
		Prog p3( "(10 / vec3(2,5,10)).z" );
		Check( p3.ok, "scalar/vec3 compiles" );
		if( p3.ok ) CheckClose( p3.prog.Eval(0,0), 1, 1e-12, "scalar/vec3 broadcast" );
		Prog p4( "(vec3(1,2,3) / vec3(2,2,2)).y" );
		Check( p4.ok, "vec3/vec3 compiles" );
		if( p4.ok ) CheckClose( p4.prog.Eval(0,0), 1, 1e-12, "vec3/vec3 componentwise" );
	}
	{
		Prog p( "(-vec3(1,-2,3)).y" );
		Check( p.ok, "unary minus on vec3 compiles" );
		if( p.ok ) CheckClose( p.prog.Eval(0,0), 2, 1e-12, "unary minus on vec3" );
	}
	{
		Prog p( "dot(vec3(1,2,3), vec3(4,5,6))" );
		Check( p.ok, "dot compiles" );
		if( p.ok ) CheckClose( p.prog.Eval(0,0), 32, 1e-12, "dot value" );
	}
	{
		Prog p( "cross(vec3(1,0,0), vec3(0,1,0))" );
		Check( p.ok && p.prog.ResultType()==ExpressionProgram::kVec3, "cross compiles, vec3 result" );
		const Vector3 v = p.prog.EvalVec3( ExprEvalContext() );
		CheckClose( v.x, 0, 1e-12, "cross .x" ); CheckClose( v.y, 0, 1e-12, "cross .y" ); CheckClose( v.z, 1, 1e-12, "cross .z" );
	}
	{
		Prog p( "length(vec3(3,4,0))" );
		Check( p.ok, "length compiles" );
		if( p.ok ) CheckClose( p.prog.Eval(0,0), 5, 1e-12, "length value" );
	}
	{
		Prog p( "normalize(vec3(3,4,0))" );
		Check( p.ok, "normalize compiles" );
		const Vector3 v = p.prog.EvalVec3( ExprEvalContext() );
		CheckClose( v.x, 0.6, 1e-12, "normalize .x" ); CheckClose( v.y, 0.8, 1e-12, "normalize .y" ); CheckClose( v.z, 0, 1e-12, "normalize .z" );
	}
	{
		Prog p( "normalize(vec3(0,0,0))" );
		Check( p.ok, "normalize(0) compiles" );
		const Vector3 v = p.prog.EvalVec3( ExprEvalContext() );
		CheckClose( v.x, 0, 1e-12, "normalize(0) .x safe" ); CheckClose( v.y, 0, 1e-12, "normalize(0) .y safe" ); CheckClose( v.z, 0, 1e-12, "normalize(0) .z safe" );
	}
	{
		Prog p( "mix(vec3(0,0,0), vec3(10,20,30), 0.25)" );
		Check( p.ok && p.prog.ResultType()==ExpressionProgram::kVec3, "vec3 mix compiles, vec3 result" );
		const Vector3 v = p.prog.EvalVec3( ExprEvalContext() );
		CheckClose( v.x, 2.5, 1e-12, "vec3 mix .x" ); CheckClose( v.y, 5, 1e-12, "vec3 mix .y" ); CheckClose( v.z, 7.5, 1e-12, "vec3 mix .z" );
	}
	{
		Prog p( "mix(1,5,0.5)" );
		Check( p.ok && p.prog.ResultType()==ExpressionProgram::kScalar, "scalar mix still scalar" );
		if( p.ok ) CheckClose( p.prog.Eval(0,0), 3, 1e-12, "scalar mix value" );
	}
	{
		// context vec3 vars: P, Po, N
		Prog p( "P.x + P.y + P.z" );
		Check( p.ok, "P.x+P.y+P.z compiles" );
		if( p.ok ) {
			ExprEvalContext ctx; ctx.P = Vector3(1,2,3);
			CheckClose( p.prog.Eval( ctx ), 6, 1e-12, "P components sum" );
		}
	}
	{
		Prog p( "length(N)" );
		Check( p.ok, "length(N) compiles" );
		if( p.ok ) {
			ExprEvalContext ctx; ctx.N = Vector3(3,4,0);
			CheckClose( p.prog.Eval( ctx ), 5, 1e-12, "length(N) value" );
		}
	}
}

//======================================================================
// 2. Noise builtins: golden values cross-checked against an independent
//    Python reimplementation of the documented formulas.
//======================================================================
static void TestNoiseBuiltins()
{
	std::cout << "Test 2: noise builtins (perlin/fbm/turbulence/ridged/worley_*/cellhash) golden values" << std::endl;

	auto evalExpr = []( const std::string& e ) -> Scalar {
		Prog p( e );
		if( !p.ok ) { Check( false, "compile: " + e + " :: " + p.builder.Error() ); return 0; }
		return p.prog.Eval(0,0);
	};

	CheckClose( evalExpr( "perlin(vec3(0.3,0.7,1.4))" ), -0.067849004290997939, 1e-9, "perlin golden A" );
	CheckClose( evalExpr( "perlin(vec3(-2.25,5.5,-0.1))" ), -0.25459063579910429, 1e-9, "perlin golden B" );
	CheckClose( evalExpr( "fbm(vec3(0.3,0.7,1.4), 4, 0.5, 2.0)" ), -0.19859653334474803, 1e-9, "fbm golden" );
	CheckClose( evalExpr( "turbulence(vec3(0.3,0.7,1.4), 4, 0.5, 2.0)" ), 0.12500609165612192, 1e-9, "turbulence golden" );
	CheckClose( evalExpr( "ridged(vec3(0.3,0.7,1.4), 4, 0.5, 2.0)" ), 0.77670332638038786, 1e-9, "ridged golden" );
	CheckClose( evalExpr( "worley_f1(vec3(0.3,0.7,1.4), 1.0)" ), 0.58925369798313076, 1e-9, "worley_f1 golden" );
	CheckClose( evalExpr( "worley_f2(vec3(0.3,0.7,1.4), 1.0)" ), 0.57872695943945174, 1e-9, "worley_f2 golden" );
	CheckClose( evalExpr( "worley_f2f1(vec3(0.3,0.7,1.4), 1.0)" ), 0.37178232156806246, 1e-9, "worley_f2f1 golden" );
	CheckClose( evalExpr( "worley_id(vec3(0.3,0.7,1.4), 1.0)" ), 685014.0, 1e-6, "worley_id golden" );
	CheckClose( evalExpr( "cellhash(worley_id(vec3(0.3,0.7,1.4), 1.0))" ), 0.939578743185848, 1e-9, "cellhash(worley_id) golden" );
	CheckClose( evalExpr( "cellhash(3)" ), 0.62859259406104684, 1e-9, "cellhash(3) golden" );
	CheckClose( evalExpr( "cellhash(-1)" ), 0.049936855677515268, 1e-9, "cellhash(-1) golden" );

	// perlin() is a convex combination of hash samples in [-1,1] -- exact bound, not just "roughly".
	{
		bool inRange = true;
		for( int i = -25; i <= 25 && inRange; ++i ) {
			for( int j = -25; j <= 25 && inRange; ++j ) {
				char buf[128];
				snprintf( buf, sizeof(buf), "perlin(vec3(%f,%f,%f))", i*0.173, j*0.211, (i-j)*0.091 );
				const Scalar v = evalExpr( buf );
				if( v < Scalar(-1.0000001) || v > Scalar(1.0000001) ) inRange = false;
			}
		}
		Check( inRange, "perlin() stays within [-1,1]" );
	}
}

//======================================================================
// 3. Compile-time type errors: failure + correct byte offset.
//======================================================================
static void TestTypeErrorOffsets()
{
	std::cout << "Test 3: compile errors carry the right byte offset" << std::endl;

	// unknown identifier
	{
		const std::string e = "nope + 1";
		Prog p( e );
		Check( !p.ok, "unknown identifier rejected" );
		Check( p.prog.ErrorOffset() == (ptrdiff_t)Find(e,"nope"), "unknown identifier offset" );
	}
	// type mismatch: scalar-only function given a vec3
	{
		const std::string e = "sin(vec3(1,2,3))";
		Prog p( e );
		Check( !p.ok, "sin(vec3) rejected" );
		Check( p.prog.ErrorOffset() == (ptrdiff_t)Find(e,"vec3(1,2,3)"), "sin(vec3) offset points at the argument" );
	}
	// type mismatch: dot() given scalars instead of vec3
	{
		const std::string e = "dot(1,2)";
		Prog p( e );
		Check( !p.ok, "dot(scalar,scalar) rejected" );
		Check( p.prog.ErrorOffset() == (ptrdiff_t)Find(e,"1"), "dot() arg-type offset" );
	}
	// type mismatch: comparison operands must be scalar
	{
		const std::string e = "1 < vec3(1,2,3)";
		Prog p( e );
		Check( !p.ok, "scalar<vec3 rejected" );
		Check( p.prog.ErrorOffset() == (ptrdiff_t)Find(e,"<"), "comparison type-error offset" );
	}
	// type mismatch: `^` requires scalar
	{
		const std::string e = "vec3(1,2,3) ^ 2";
		Prog p( e );
		Check( !p.ok, "vec3 ^ scalar rejected" );
		Check( p.prog.ErrorOffset() == (ptrdiff_t)Find(e,"^"), "`^` type-error offset" );
	}
	// type mismatch: swizzle on a scalar
	{
		const std::string e = "u.x";
		Prog p( e );
		Check( !p.ok, "scalar.x rejected" );
		Check( p.prog.ErrorOffset() == (ptrdiff_t)Find(e,"."), "swizzle-on-scalar offset" );
	}
	// malformed: not a valid swizzle component
	{
		const std::string e = "P.q";
		Prog p( e );
		Check( !p.ok, "P.q (invalid component) rejected" );
		Check( p.prog.ErrorOffset() == (ptrdiff_t)Find(e,"."), "invalid swizzle component offset" );
	}
	// malformed call arity: too many args
	{
		const std::string e = "sin(1,2)";
		Prog p( e );
		Check( !p.ok, "sin(1,2) rejected" );
		Check( p.prog.ErrorOffset() == (ptrdiff_t)Find(e,"sin"), "too-many-args offset" );
	}
	// malformed call arity: too few args
	{
		const std::string e = "dot(vec3(1,2,3))";
		Prog p( e );
		Check( !p.ok, "dot() with 1 arg rejected" );
		Check( p.prog.ErrorOffset() == (ptrdiff_t)Find(e,"dot"), "too-few-args offset" );
	}
	// unexpected token: nothing follows a binary operator
	{
		const std::string e = "1 +";
		Prog p( e );
		Check( !p.ok, "trailing operator rejected" );
		Check( p.prog.ErrorOffset() == (ptrdiff_t)e.size(), "unexpected-token (EOF) offset" );
	}
	// unexpected token: ')' where an expression was expected
	{
		const std::string e = "vec3(1,2)";
		Prog p( e );
		Check( !p.ok, "vec3(1,2) (missing 3rd arg) rejected" );
		Check( p.prog.ErrorOffset() == (ptrdiff_t)Find(e,")"), "unexpected-token ')' offset" );
	}
	// unknown function
	{
		const std::string e = "foo(1)";
		Prog p( e );
		Check( !p.ok, "unknown function rejected" );
		Check( p.prog.ErrorOffset() == (ptrdiff_t)Find(e,"foo"), "unknown function offset" );
	}
}

//======================================================================
// 4. Back-compat: Eval(u,v) vs Eval(context) agree when context extras
//    are zero; a same-named param SHADOWS a context var (real-scene
//    `param N <value>` regression guard, see ExpressionEval.h Builder()).
//======================================================================
static void TestBackCompat()
{
	std::cout << "Test 4: Eval(u,v) back-compat + param/context-var shadowing" << std::endl;

	{
		Prog p( "sin(u*6.2831853)+cos(v*6.2831853)*2.0" );
		Check( p.ok, "legacy-style expression compiles" );
		if( p.ok ) {
			for( int i = 0; i < 5; ++i ) {
				const Scalar u = i*0.173, v = i*0.311;
				const Scalar viaOld = p.prog.Eval( u, v );
				ExprEvalContext ctx; ctx.u = u; ctx.v = v;	// P/Po/N/fw/time default to 0
				const Scalar viaNew = p.prog.Eval( ctx );
				CheckClose( viaOld, viaNew, 0, "Eval(u,v) == Eval(context) with zeroed extras" );
			}
		}
	}
	{
		// an expression that reads every context var: the 2-arg Eval must
		// behave as if they're all exactly zero.
		Prog p( "u + v + P.x + Po.y + N.z + fw + time" );
		Check( p.ok, "all-context-vars expression compiles" );
		if( p.ok ) {
			CheckClose( p.prog.Eval( 2, 3 ), 5, 1e-12, "Eval(u,v) zeros every extra context var" );
			ExprEvalContext ctx; ctx.u=2; ctx.v=3; ctx.P=Vector3(10,0,0); ctx.Po=Vector3(0,20,0); ctx.N=Vector3(0,0,30); ctx.fw=1; ctx.time=1;
			CheckClose( p.prog.Eval( ctx ), 2+3+10+20+30+1+1, 1e-12, "Eval(context) actually threads the extras" );
		}
	}
	{
		// Real-scene regression: `param N 85` (enamel_watch.RISEscene,
		// enamel_dial_dimpled.RISEscene) must keep meaning "the constant
		// 85", not get silently clobbered by (or bound to) the new vec3
		// shading-normal context variable of the same name.
		ExpressionProgram prog = ExpressionProgram::Invalid();
		ExpressionProgram::Builder b;
		b.AddParam( "N", 85 );
		Check( b.Finalize( "N + u", prog ), "param `N` shadows context N: compiles" );
		if( prog.IsValid() ) {
			CheckClose( prog.Eval( 1, 0 ), 86, 1e-12, "param N=85 used, Eval(u,v)" );
			ExprEvalContext ctx; ctx.u = 1; ctx.N = Vector3(7,7,7);	// must NOT leak into `N`
			CheckClose( prog.Eval( ctx ), 86, 1e-12, "param N=85 still used even with a nonzero context N" );
		}
	}
}

//======================================================================
// 5. ramp(): clamped ends, interior lerp, multi-stop, vec3 stops, and
//    its own compile-error cases.
//======================================================================
static void TestRamp()
{
	std::cout << "Test 5: ramp() clamping / interpolation / vec3 stops / errors" << std::endl;

	{
		ExpressionProgram::Builder b0; ExpressionProgram pr0 = ExpressionProgram::Invalid();
		b0.AddParam( "t", 0 );
		Check( b0.Finalize( "ramp(t, 0,0, 1,10)", pr0 ), "ramp(2 stops) compiles" );

		// re-evaluate by rebuilding per-t (AddParam is compile-time only)
		auto evalAt = []( Scalar tval ) -> Scalar {
			ExpressionProgram::Builder b; ExpressionProgram pr = ExpressionProgram::Invalid();
			b.AddParam( "t", tval );
			b.Finalize( "ramp(t, 0,0, 1,10)", pr );
			return pr.Eval(0,0);
		};
		CheckClose( evalAt( -0.5 ), 0, 1e-12, "ramp clamps below first stop" );
		CheckClose( evalAt( 1.5 ), 10, 1e-12, "ramp clamps above last stop" );
		CheckClose( evalAt( 0.5 ), 5, 1e-12, "ramp interior lerp" );
		CheckClose( evalAt( 0 ), 0, 1e-12, "ramp exactly at first stop" );
		CheckClose( evalAt( 1 ), 10, 1e-12, "ramp exactly at last stop" );
	}
	{
		auto evalAt = []( Scalar tval ) -> Scalar {
			ExpressionProgram::Builder b; ExpressionProgram pr = ExpressionProgram::Invalid();
			b.AddParam( "t", tval );
			b.Finalize( "ramp(t, 0,0, 1,10, 2,100)", pr );
			return pr.Eval(0,0);
		};
		CheckClose( evalAt( 1.5 ), 55, 1e-12, "3-stop ramp, second segment" );
		CheckClose( evalAt( -1 ), 0, 1e-12, "3-stop ramp clamps below" );
		CheckClose( evalAt( 5 ), 100, 1e-12, "3-stop ramp clamps above" );
	}
	{
		ExpressionProgram::Builder b; ExpressionProgram pr = ExpressionProgram::Invalid();
		b.AddParam( "t", 0.5 );
		Check( b.Finalize( "ramp(t, 0,vec3(0,0,0), 1,vec3(10,20,30))", pr ), "vec3-stop ramp compiles" );
		if( pr.IsValid() ) {
			Check( pr.ResultType()==ExpressionProgram::kVec3, "vec3-stop ramp result type" );
			const Vector3 v = pr.EvalVec3( ExprEvalContext() );
			CheckClose( v.x, 5, 1e-12, "vec3 ramp .x" ); CheckClose( v.y, 10, 1e-12, "vec3 ramp .y" ); CheckClose( v.z, 15, 1e-12, "vec3 ramp .z" );
		}
	}
	// errors
	{
		const std::string e = "ramp(0.5, 1,10, 0,20)";	// descending literal positions
		Prog p( e );
		Check( !p.ok, "ramp() with descending literal positions rejected" );
		Check( p.prog.ErrorOffset() == (ptrdiff_t)Find(e,"0,20"), "ramp() descending-position offset" );
	}
	{
		const std::string e = "ramp(0.5, 1,10)";	// only 1 stop
		Prog p( e );
		Check( !p.ok, "ramp() with <2 stops rejected" );
		Check( p.prog.ErrorOffset() == (ptrdiff_t)Find(e,"ramp"), "ramp() too-few-stops offset" );
	}
	{
		const std::string e = "ramp(0.5, 0,1, 1,vec3(1,2,3))";	// stop value type mismatch
		Prog p( e );
		Check( !p.ok, "ramp() mixed scalar/vec3 stop values rejected" );
		Check( p.prog.ErrorOffset() == (ptrdiff_t)Find(e,"vec3(1,2,3)"), "ramp() value-type-mismatch offset" );
	}
}

//======================================================================
// 6. fbm/turbulence/ridged octave: compile-time literal validation,
//    runtime clamp otherwise.
//======================================================================
static void TestOctaveValidation()
{
	std::cout << "Test 6: fbm/turbulence/ridged octave compile-time literal check + runtime clamp" << std::endl;

	{
		const std::string e = "fbm(vec3(0,0,0), 15, 0.5, 2.0)";	// literal, out of [1,10]
		Prog p( e );
		Check( !p.ok, "fbm() literal octaves=15 rejected at compile time" );
		Check( p.prog.ErrorOffset() == (ptrdiff_t)Find(e,"15"), "fbm() octave-literal offset" );
	}
	{
		const std::string e = "fbm(vec3(0,0,0), 0, 0.5, 2.0)";	// literal, below range
		Prog p( e );
		Check( !p.ok, "fbm() literal octaves=0 rejected at compile time" );
	}
	{
		// non-literal octave count: compiles fine, clamps at runtime.
		ExpressionProgram::Builder b1; ExpressionProgram p1 = ExpressionProgram::Invalid();
		b1.AddParam( "octParam", 20 );	// way above the cap, but not a literal token
		Check( b1.Finalize( "fbm(vec3(0.3,0.7,1.4), octParam, 0.5, 2.0)", p1 ), "fbm() with a non-literal octave param compiles" );

		ExpressionProgram::Builder b2; ExpressionProgram p2 = ExpressionProgram::Invalid();
		b2.Finalize( "fbm(vec3(0.3,0.7,1.4), 10, 0.5, 2.0)", p2 );

		if( p1.IsValid() && p2.IsValid() ) {
			CheckClose( p1.Eval(0,0), p2.Eval(0,0), 1e-12, "runtime octave clamp matches the literal cap" );
		}
	}
}

//======================================================================
// 7. Param-metadata line grammar (ExpressionParamSpec.h).
//======================================================================
static void TestParamSpec()
{
	std::cout << "Test 7: param-metadata grammar (min/max/step/label)" << std::endl;

	{
		ParamSpec spec; std::string err; ptrdiff_t off;
		Check( ParseParamSpecLine( "ring_scale 4.0 min 0.5 max 20 label \"Ring density\"", spec, err, off ), "all fields parse" );
		Check( spec.name=="ring_scale", "name" );
		CheckClose( spec.value, 4.0, 1e-12, "value" );
		Check( spec.hasMin && std::fabs(spec.min-0.5)<1e-12, "min" );
		Check( spec.hasMax && std::fabs(spec.max-20)<1e-12, "max" );
		Check( !spec.hasStep, "step absent" );
		Check( spec.hasLabel && spec.label=="Ring density", "label" );
	}
	{
		ParamSpec spec; std::string err; ptrdiff_t off;
		Check( ParseParamSpecLine( "k 5", spec, err, off ), "minimal name+value parses" );
		Check( spec.name=="k" && !spec.hasMin && !spec.hasMax && !spec.hasStep && !spec.hasLabel, "minimal has no metadata" );
	}
	{
		ParamSpec spec; std::string err; ptrdiff_t off;
		Check( ParseParamSpecLine( "step_test 1 step 0.25", spec, err, off ), "step field parses" );
		Check( spec.hasStep && std::fabs(spec.step-0.25)<1e-12, "step value" );
	}
	{
		ParamSpec spec; std::string err; ptrdiff_t off;
		Check( !ParseParamSpecLine( "k", spec, err, off ), "missing value rejected" );
	}
	{
		ParamSpec spec; std::string err; ptrdiff_t off;
		Check( !ParseParamSpecLine( "k abc", spec, err, off ), "malformed number rejected" );
	}
	{
		ParamSpec spec; std::string err; ptrdiff_t off;
		Check( !ParseParamSpecLine( "k 5 bogus 1", spec, err, off ), "unknown keyword rejected" );
	}
	{
		ParamSpec spec; std::string err; ptrdiff_t off;
		Check( !ParseParamSpecLine( "k 5 min 1 min 2", spec, err, off ), "duplicate min rejected" );
	}
	{
		ParamSpec spec; std::string err; ptrdiff_t off;
		Check( !ParseParamSpecLine( "k 5 label \"unterminated", spec, err, off ), "unterminated label rejected" );
	}
	{
		ParamSpec spec; std::string err; ptrdiff_t off;
		Check( !ParseParamSpecLine( "k 5 min 10 max 1", spec, err, off ), "min>=max rejected" );
	}
	{
		ParamSpec spec; std::string err; ptrdiff_t off;
		Check( !ParseParamSpecLine( "", spec, err, off ), "empty line rejected" );
	}
	// S5: a label with an interior DOUBLE space parses to a SINGLE space.
	// This is by design, not a shortfall of ReadQuoted -- see its own doc in
	// ExpressionParamSpec.h. The CST layer has no atomic quoted-token type,
	// so Cst.cpp's WithParamValue rewrites ANY param line by whitespace-
	// splitting the value and rejoining tokens with exactly one space,
	// quote characters included -- a hand-authored double space in a label
	// does not survive the FIRST CST write to that line regardless of
	// which field on the line changed.  Normalizing on read keeps what
	// this parser reports in sync with what a write would already do to
	// the same text, rather than reading back one thing and writing back
	// another.
	{
		ParamSpec spec; std::string err; ptrdiff_t off;
		Check( ParseParamSpecLine( "vf 1.0 label \"Vein  frequency\"", spec, err, off ),
		       "S5: a double-space label still parses" );
		Check( spec.hasLabel && spec.label == "Vein frequency",
		       "S5 MONEY: the interior double space COLLAPSES to a single space -- "
		       "\"Vein  frequency\" (two spaces) reads as \"Vein frequency\" (one), matching "
		       "what a CST write of this line would already produce" );
	}
	// A run of three-plus spaces, and multiple runs in the same label, both
	// collapse the same way -- pinning "collapses to ONE space", not merely
	// "stops being exactly two".
	{
		ParamSpec spec; std::string err; ptrdiff_t off;
		Check( ParseParamSpecLine( "vf 1.0 label \"a   b    c\"", spec, err, off ),
		       "S5: multiple multi-space runs still parse" );
		Check( spec.hasLabel && spec.label == "a b c",
		       "S5: every interior run -- three spaces, then four -- collapses to exactly one" );
	}
	// A label that ALREADY has single spaces is unaffected (the collapse is
	// idempotent on already-normalized text, so this fix changes nothing
	// for the common case every other label-bearing case in this file
	// exercises).
	{
		ParamSpec spec; std::string err; ptrdiff_t off;
		Check( ParseParamSpecLine( "ring_scale 4.0 label \"Ring density\"", spec, err, off ),
		       "S5: an already-single-spaced label still parses" );
		Check( spec.hasLabel && spec.label == "Ring density",
		       "S5: ...unchanged -- the collapse is a no-op on text with no run to collapse" );
	}
}

//======================================================================
// 8. fbm determinism: two independent Builder/Program instances agree.
//======================================================================
static void TestFbmDeterminism()
{
	std::cout << "Test 8: fbm determinism across independent VM instances" << std::endl;

	ExpressionProgram::Builder b1; ExpressionProgram p1 = ExpressionProgram::Invalid();
	Check( b1.Finalize( "fbm(vec3(u,v,0.37), 6, 0.55, 2.13)", p1 ), "instance 1 compiles" );

	ExpressionProgram::Builder b2; ExpressionProgram p2 = ExpressionProgram::Invalid();
	Check( b2.Finalize( "fbm(vec3(u,v,0.37), 6, 0.55, 2.13)", p2 ), "instance 2 compiles" );

	if( p1.IsValid() && p2.IsValid() ) {
		bool allEqual = true;
		for( int i = -10; i <= 10; ++i ) {
			const Scalar u = i*0.091, v = -i*0.133;
			const Scalar a = p1.Eval( u, v );
			const Scalar b = p2.Eval( u, v );
			if( a != b ) { allEqual = false; break; }
		}
		Check( allEqual, "two independently compiled/evaluated fbm programs agree exactly" );
	}
}

//======================================================================
// 9. Noise-factoring parity: the shared ProceduralNoiseCore primitives,
//    composed the same way Perlin3DPainter::EvaluateField /
//    Worley3DPainter::EvaluateField now do, reproduce the historical
//    PerlinNoise3D / WorleyNoise3D engines (still in Noise/, untouched,
//    and still used by SDFPrimitives/DomainWarp/Curl/PerlinWorley)
//    bit-for-bit.  This is the pre/post-factoring proof for the painter
//    rewrite in this slice.
//======================================================================
static Scalar PerlinPainterStyleField( Scalar x, Scalar y, Scalar z, Scalar persistence, unsigned int nOctaves )
{
	const unsigned int cappedOctaves = ( nOctaves < 32 ) ? nOctaves : 32;
	const int n = (int)cappedOctaves - 1;
	Scalar total = 0;
	for( int i = 0; i < n; ++i ) {
		const Scalar frequency = std::pow( 2.0, Scalar(i) );
		const Scalar amplitude = std::pow( persistence, Scalar(i) );
		total += NoiseCore::PerlinOctave3D( x*frequency, y*frequency, z*frequency ) * amplitude;
	}
	return total;
}

static void TestNoiseFactoringParity()
{
	std::cout << "Test 9: ProceduralNoiseCore parity vs the historical PerlinNoise3D/WorleyNoise3D engines" << std::endl;

	RealLinearInterpolator* interp = new RealLinearInterpolator();

	const Scalar persistences[] = { 0.35, 0.5, 0.75 };
	const unsigned int octaveCounts[] = { 1, 3, 6, 9 };
	bool allMatch = true;
	for( size_t pi = 0; pi < sizeof(persistences)/sizeof(persistences[0]) && allMatch; ++pi ) {
		for( size_t oi = 0; oi < sizeof(octaveCounts)/sizeof(octaveCounts[0]) && allMatch; ++oi ) {
			PerlinNoise3D* legacy = new PerlinNoise3D( *interp, persistences[pi], (int)octaveCounts[oi] );
			for( int i = -8; i <= 8 && allMatch; ++i ) {
				const Scalar x = i*0.219, y = -i*0.147+0.5, z = i*0.083-1.1;
				const Scalar want = legacy->Evaluate( x, y, z );
				const Scalar got = PerlinPainterStyleField( x, y, z, persistences[pi], octaveCounts[oi] );
				// 1 ULP tolerance, not exact ==: pAmplitudesLUT[i]=pow(persistence,i)
				// is computed in PerlinNoise3D.cpp's translation unit (the legacy
				// path) vs this file's (the factored path) -- under
				// -ffast-math, libm pow() call sites in different TUs are not
				// guaranteed bit-identical even for byte-identical source
				// (confirmed empirically: diff was exactly 1.11e-16 at one
				// sample, i.e. exactly 1 ULP of a ~0.14 value). The formula is
				// identical; this is TU-codegen noise, not a logic difference.
				if( std::fabs( want - got ) > Scalar(1e-12) ) {
					allMatch = false;
					std::cout.precision(20);
					std::cout << "  FAIL: perlin parity persistence=" << persistences[pi] << " octaves=" << octaveCounts[oi]
							  << " at (" << x << "," << y << "," << z << ")  legacy=" << want << " core=" << got << " diff=" << (want-got) << std::endl;
				}
			}
			legacy->release();
		}
	}
	Check( allMatch, "Perlin3DPainter-style octave sum is bit-identical to legacy PerlinNoise3D" );

	interp->release();

	struct WCase { Scalar jitter; WorleyDistanceMetric metric; WorleyOutputMode mode; NoiseCore::WorleyMetric coreMetric; NoiseCore::WorleyMode coreMode; };
	const WCase wcases[] = {
		{ 1.0, eWorley_Euclidean, eWorley_F1,        NoiseCore::eMetricEuclidean, NoiseCore::eModeF1 },
		{ 1.0, eWorley_Euclidean, eWorley_F2,        NoiseCore::eMetricEuclidean, NoiseCore::eModeF2 },
		{ 0.5, eWorley_Manhattan, eWorley_F2minusF1, NoiseCore::eMetricManhattan, NoiseCore::eModeF2MinusF1 },
		{ 1.0, eWorley_Chebyshev, eWorley_F1,        NoiseCore::eMetricChebyshev, NoiseCore::eModeF1 },
	};
	bool worleyMatch = true;
	for( size_t ci = 0; ci < sizeof(wcases)/sizeof(wcases[0]) && worleyMatch; ++ci ) {
		WorleyNoise3D* legacy = new WorleyNoise3D( wcases[ci].jitter, wcases[ci].metric, wcases[ci].mode );
		for( int i = -8; i <= 8 && worleyMatch; ++i ) {
			const Scalar x = i*0.271-2.0, y = -i*0.183+1.0, z = i*0.097;
			const Scalar want = legacy->Evaluate( x, y, z );

			Scalar f1, f2; int cx,cy,cz;
			NoiseCore::WorleySample3D( x, y, z, wcases[ci].jitter, wcases[ci].coreMetric, f1, f2, cx, cy, cz );
			Scalar raw;
			switch( wcases[ci].coreMode ) {
			case NoiseCore::eModeF2: raw = f2; break;
			case NoiseCore::eModeF2MinusF1: raw = f2-f1; break;
			default: raw = f1; break;
			}
			const Scalar got = NoiseCore::WorleyNormalize( raw, wcases[ci].coreMetric, wcases[ci].coreMode );

			if( want != got ) {
				worleyMatch = false;
				std::cout << "  FAIL: worley parity case " << ci << " at (" << x << "," << y << "," << z << ")  legacy=" << want << " core=" << got << std::endl;
			}
		}
		legacy->release();
	}
	Check( worleyMatch, "Worley3DPainter-style evaluation is bit-identical to legacy WorleyNoise3D" );
}

//======================================================================
// 10. Review round 1, P1-A: a duplicate param/def name whose TYPE
//     doesn't match the earlier registration is a compile error (both
//     orders), instead of Slot() silently reusing the earlier slot
//     type-blind (env clobber / type confusion / an ASan-confirmed
//     stack-buffer overflow at BindEnv).  A SAME-type duplicate (e.g.
//     two `param a` lines) stays ALLOWED as "last wins" -- Cst.cpp's
//     `let` chunk semantics (CollectLetBindings feeds every binding,
//     duplicates included, through Builder::AddParam) depend on it, and
//     CstLetTest locks that in independently.
//======================================================================
static void TestDuplicateNames()
{
	std::cout << "Test 10: cross-type duplicate param/def names are a compile error (P1-A); same-type is last-wins" << std::endl;

	// order: param (scalar) then def (vec3) -- TYPE MISMATCH, rejected
	{
		ExpressionProgram::Builder b; ExpressionProgram p = ExpressionProgram::Invalid();
		Check( b.AddParam( "a", 1 ), "param `a` compiles" );
		Check( b.AddParam( "b", 7 ), "param `b` compiles" );
		Check( !b.AddDef( "a", "vec3(4,5,6)" ), "def `a` (vec3) after param `a` (scalar): rejected by AddDef" );
		Check( !b.Finalize( "b", p ), "cross-type duplicate (param then def): Finalize also fails (sticky)" );
	}
	// order: def (vec3) then param (scalar) -- TYPE MISMATCH, rejected
	{
		ExpressionProgram::Builder b; ExpressionProgram p = ExpressionProgram::Invalid();
		Check( b.AddDef( "a", "vec3(4,5,6)" ), "def `a` compiles" );
		Check( !b.AddParam( "a", 1 ), "param `a` (scalar) after def `a` (vec3): rejected by AddParam" );
		Check( !b.Finalize( "a", p ), "cross-type duplicate (def then param): Finalize also fails (sticky)" );
	}
	// the exact reviewer-reported repro (param a / param b / def a(vec3) / expr b,
	// which used to silently read b=5 due to the env clobber) is now caught before
	// Finalize can ever succeed.
	{
		ExpressionProgram::Builder b; ExpressionProgram p = ExpressionProgram::Invalid();
		b.AddParam( "a", 1 );
		b.AddParam( "b", 7 );
		b.AddDef( "a", "vec3(4,5,6)" );	// rejected; sets the sticky error
		Check( !b.Finalize( "b", p ), "P1-A repro (param a / param b / def a(vec3) / expr b) rejected, not silently misread" );
	}

	// SAME-type duplicate: allowed, last value wins.  This is not a gap in
	// the P1-A fix -- Slot() returns the already-correctly-sized existing
	// slot either way when the type matches, so there is no env clobber to
	// guard against; rejecting it would break Cst.cpp's `let` "duplicate
	// name is last-wins" semantics (CstLetTest).
	{
		ExpressionProgram::Builder b; ExpressionProgram p = ExpressionProgram::Invalid();
		Check( b.AddParam( "a", 1 ), "param `a`=1 compiles" );
		Check( b.AddParam( "a", 2 ), "param `a`=2 (same type, same name) is ALLOWED -- last wins" );
		Check( b.Finalize( "a", p ), "Finalize succeeds after a same-type duplicate param" );
		if( p.IsValid() ) CheckClose( p.Eval(0,0), 2, 1e-12, "the LATER param value (2) wins, matching CollectLetBindings' `let` semantics" );
	}
	{
		ExpressionProgram::Builder b; ExpressionProgram p = ExpressionProgram::Invalid();
		Check( b.AddDef( "a", "vec3(1,2,3)" ), "def `a`=vec3(1,2,3) compiles" );
		Check( b.AddDef( "a", "vec3(7,8,9)" ), "def `a`=vec3(7,8,9) (same type, same name) is ALLOWED -- last wins" );
		Check( b.Finalize( "a", p ), "Finalize succeeds after a same-type duplicate def" );
		if( p.IsValid() ) {
			const Vector3 v = p.EvalVec3( ExprEvalContext() );
			CheckClose( v.x, 7, 1e-12, "the LATER def's value wins .x" ); CheckClose( v.y, 8, 1e-12, "..y" ); CheckClose( v.z, 9, 1e-12, "..z" );
		}
	}
}

//======================================================================
// 11. Review round 1: P1-B (compound-octave-argument UB) is now a
//     runtime clamp, not a compile-time-literal-only check; P1-C
//     (cellhash float->int UB) stays finite on extreme inputs.
//======================================================================
static void TestOctaveAndCellhashRuntimeSafety()
{
	std::cout << "Test 11: P1-B compound-octave runtime clamp + P1-C cellhash extreme-input safety" << std::endl;

	// P1-B: a COMPOUND octave argument (not a single Num token) compiles --
	// it is no longer literal-checkable at compile time, so it is clamped
	// at RUNTIME instead (OctavesFromScalar), matching the literal-10 result.
	{
		Prog p( "fbm(vec3(0.3,0.7,1.4), 2*1e20, 0.5, 2.0)" );
		Check( p.ok, "fbm() with a compound octave expression (2*1e20) compiles" );
		Prog p10( "fbm(vec3(0.3,0.7,1.4), 10, 0.5, 2.0)" );
		Check( p10.ok, "fbm() octaves=10 literal reference compiles" );
		if( p.ok && p10.ok ) {
			CheckClose( p.prog.Eval(0,0), p10.prog.Eval(0,0), 1e-12, "compound octave 2e20 clamps to the same result as literal 10 (no UB)" );
		}
	}

	// P1-C: cellhash on an extreme-magnitude argument stays finite and in [0,1).
	{
		Prog p1( "cellhash(1e20)" );
		Check( p1.ok, "cellhash(1e20) compiles" );
		if( p1.ok ) {
			const Scalar v = p1.prog.Eval(0,0);
			Check( ExpressionProgram::IsFinite(v) && v >= Scalar(0) && v < Scalar(1), "cellhash(1e20) finite in [0,1)" );
		}
		Prog p2( "cellhash(0-1e20)" );
		Check( p2.ok, "cellhash(0-1e20) compiles" );
		if( p2.ok ) {
			const Scalar v = p2.prog.Eval(0,0);
			Check( ExpressionProgram::IsFinite(v) && v >= Scalar(0) && v < Scalar(1), "cellhash(0-1e20) finite in [0,1)" );
		}
	}
}

//======================================================================
// 12. Review round 1, P2-B: a deep chain of leading unary signs is a
//     compile error, not a C++ call-stack crash.  Plus: compile-time
//     stack-cap rejection for a deeply-nested value-stack peak.
//======================================================================
static void TestParseDepthAndStackCap()
{
	std::cout << "Test 12: deep unary chain + stack-cap rejection (compile-time bounds)" << std::endl;

	// P2-B: a long run of leading unary signs used to recurse straight into
	// itself, bypassing ParseCmp's depth guard, and blew the C++ call stack
	// (ASan-confirmed).  Now bounded exactly like every other nesting form.
	{
		std::string e( 100000, '-' );
		e += "1";
		Prog p( e );
		Check( !p.ok, "100k-deep unary chain rejected at compile time (not a crash)" );
	}

	// Stack-cap rejection: a maximal-width vec3-stop ramp() (64 stops * (1
	// pos + 3 val) + t == 257 scalars pushed and NOT reduced until its own
	// kRamp instruction fires) nested as the LAST stop's value of another
	// such ramp, several levels deep -- each level's own peak stacks on top
	// of the outer levels' already-pushed-and-not-yet-reduced stops, so the
	// compile-time stack simulation's peak grows past kStackCap (512) by the
	// 3rd level.  Built programmatically (the exact nesting needed depends
	// on ramp's per-stop cost), not hand-authored.
	{
		auto wideRamp = []( const std::string& lastVal ) -> std::string {
			std::string s = "ramp(0.5";
			for( int i = 0; i < 63; ++i ) { s += "," + std::to_string(i) + ",vec3(1,1,1)"; }
			s += "," + std::to_string(63) + "," + lastVal + ")";
			return s;
		};
		std::string expr = "vec3(1,1,1)";
		for( int level = 0; level < 4; ++level ) expr = wideRamp( expr );
		Prog p( expr );
		Check( !p.ok, "deeply-nested max-width ramp() rejected: compile-time stack peak exceeds the cap" );
		if( !p.ok ) Check( p.builder.Error().find( "too large" ) != std::string::npos, "stack-cap rejection carries the expected diagnostic" );
	}
}

//======================================================================
// 13. Review round 1: left-compound broadcast splicing (P1-D regression
//     coverage -- BroadcastAt's fixed comment now correctly describes
//     which operand it targets) + lexer edge cases + EvalVec3/Eval
//     broadcast on the "other" result type.
//======================================================================
static void TestSpliceAndLexerEdges()
{
	std::cout << "Test 13: left-compound broadcast splicing + lexer edge cases + EvalVec3/Eval broadcast" << std::endl;

	{
		ExprEvalContext ctx; ctx.P = Vector3(10,20,30);
		Prog p1( "((1+P)*2).y" );
		Check( p1.ok, "((1+P)*2).y compiles" );
		if( p1.ok ) CheckClose( p1.prog.Eval( ctx ), 42, 1e-12, "((1+P)*2).y == 2*(1+P.y) == 42" );

		Prog p2( "1+2+P" );
		Check( p2.ok && p2.prog.ResultType()==ExpressionProgram::kVec3, "1+2+P compiles, vec3 result" );
		if( p2.ok ) {
			const Vector3 v = p2.prog.EvalVec3( ctx );
			CheckClose( v.x, 13, 1e-12, "1+2+P .x" ); CheckClose( v.y, 23, 1e-12, "1+2+P .y" ); CheckClose( v.z, 33, 1e-12, "1+2+P .z" );
		}

		Prog p3( "P+1+2" );
		Check( p3.ok && p3.prog.ResultType()==ExpressionProgram::kVec3, "P+1+2 compiles, vec3 result" );
		if( p3.ok ) {
			const Vector3 v = p3.prog.EvalVec3( ctx );
			CheckClose( v.x, 13, 1e-12, "P+1+2 .x (matches 1+2+P: broadcast splicing is associative)" );
			CheckClose( v.y, 23, 1e-12, "P+1+2 .y" ); CheckClose( v.z, 33, 1e-12, "P+1+2 .z" );
		}
	}

	// lexer edges
	{
		Prog p( "3.x" );
		Check( !p.ok, "3.x rejected (strtod consumes `3.`; trailing `x` is an unexpected token, not a swizzle)" );
	}
	{
		Prog p( ".x" );
		Check( !p.ok, ".x alone rejected (nothing to swizzle)" );
	}
	{
		// whitespace between an identifier and its swizzle: tokenizes
		// identically to `v.y` (Tokenize skips whitespace generically before
		// each token, including right before the '.'), so this COMPILES.
		ExpressionProgram::Builder b; ExpressionProgram p = ExpressionProgram::Invalid();
		Check( b.AddDef( "v", "vec3(7,8,9)" ), "def v = vec3(7,8,9) compiles" );
		Check( b.Finalize( "v .y", p ), "`v .y` (space before the dot) compiles identically to `v.y`" );
		if( p.IsValid() ) CheckClose( p.Eval(0,0), 8, 1e-12, "`v .y` evaluates to 8" );
	}
	{
		Prog p( "vec3(1,2,3).x.y" );
		Check( !p.ok, "vec3(1,2,3).x.y rejected (.x already reduced to scalar; .y on a scalar is an error)" );
	}

	// EvalVec3 on a scalar-typed program broadcasts to (s,s,s); Eval(u,v) on
	// a vec3-typed program returns .x (the first component) -- both per the
	// documented header contract, exercised here on the "other" overload
	// each wasn't yet directly tested against.
	{
		Prog p( "2+3" );
		Check( p.ok && p.prog.ResultType()==ExpressionProgram::kScalar, "2+3 compiles, scalar result" );
		if( p.ok ) {
			const Vector3 v = p.prog.EvalVec3( ExprEvalContext() );
			CheckClose( v.x, 5, 1e-12, "EvalVec3 on scalar-typed program: .x" );
			CheckClose( v.y, 5, 1e-12, "EvalVec3 on scalar-typed program: .y broadcasts" );
			CheckClose( v.z, 5, 1e-12, "EvalVec3 on scalar-typed program: .z broadcasts" );
		}
	}
	{
		Prog p( "vec3(7,8,9)" );
		Check( p.ok && p.prog.ResultType()==ExpressionProgram::kVec3, "vec3(7,8,9) compiles, vec3 result" );
		if( p.ok ) CheckClose( p.prog.Eval(0,0), 7, 1e-12, "Eval(u,v) on vec3-typed program returns .x" );
	}
}

//======================================================================
// 14. ramp() >64 stops / vec3 t rejected; mix(vec3,scalar,scalar)
//     rejected (only (s,s,s) and (v,v,s) are valid mix() forms).
//======================================================================
static void TestRampMoreEdgesAndMixRejection()
{
	std::cout << "Test 14: ramp() >64 stops / vec3 t rejected; mix(vec3,scalar,scalar) rejected" << std::endl;

	{
		std::string e = "ramp(0.5";
		for( int i = 0; i < 65; ++i ) { e += "," + std::to_string(i) + "," + std::to_string(i*10); }
		e += ")";
		Prog p( e );
		Check( !p.ok, "ramp() with 65 stops rejected (limit 64)" );
	}
	{
		const std::string e = "ramp(vec3(0,0,0), 0,1, 1,2)";
		Prog p( e );
		Check( !p.ok, "ramp() with a vec3 t argument rejected" );
		// ParseRampCall reports this specific error at the `ramp` identifier's
		// own offset (nameOff), not the argument's -- unlike most other
		// type-mismatch diagnostics in this file, which point at the argument.
		Check( p.prog.ErrorOffset() == (ptrdiff_t)Find(e,"ramp"), "ramp() vec3-t offset points at `ramp` itself" );
	}
	{
		const std::string e = "mix(vec3(1,2,3), 1, 0.5)";
		Prog p( e );
		Check( !p.ok, "mix(vec3,scalar,scalar) rejected" );
	}
}

//======================================================================
// 15. Review round 1, P2-A: context vars are opt-in (EnableContextVars);
//     u,v are never gated.
//======================================================================
static void TestContextVarGating()
{
	std::cout << "Test 15: context vars are opt-in (P2-A, EnableContextVars)" << std::endl;

	{
		ExpressionProgram::Builder b; ExpressionProgram p = ExpressionProgram::Invalid();
		Check( !b.Finalize( "time*2+1", p ), "context vars OFF by default: `time` is an unknown identifier" );
	}
	{
		ExpressionProgram::Builder b; ExpressionProgram p = ExpressionProgram::Invalid();
		b.EnableContextVars( true );
		Check( b.Finalize( "time*2+1", p ), "EnableContextVars(true): `time` resolves to the context var" );
	}
	{
		// u, v are never gated -- available with or without EnableContextVars.
		ExpressionProgram::Builder b; ExpressionProgram p = ExpressionProgram::Invalid();
		Check( b.Finalize( "u+v", p ), "u, v are never gated by EnableContextVars" );
	}
}

//======================================================================
// S2 -- chunk surfaces (expression_painter, scalar_painter{expression}).
// Small scene-parse harness, modeled on GuillocheChunkParseTest.cpp.
//======================================================================
namespace S2 {

	std::string WriteTempScene( const std::string& tag, const std::string& body )
	{
		const char* tmp = getenv( "TMPDIR" );
		std::string dir = tmp ? tmp : "/tmp/";
		if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
		std::string path = dir + "rise_texexprvm_s2_" + tag + ".RISEscene";
		std::ofstream f( path.c_str(), std::ios::binary | std::ios::trunc );
		f << body;
		f.close();
		return path;
	}

	// Parse an inline chunk body (no leading header) through the canonical
	// CST load path into `job`.  Returns the load verdict.
	bool ParseBody( const std::string& tag, const std::string& body, Job& job )
	{
		const std::string path = WriteTempScene( tag, "RISE ASCII SCENE 7\n" + body );
		const bool ok = job.LoadAsciiSceneViaCst( path.c_str() );
		remove( path.c_str() );
		return ok;
	}

	bool ParseBody( const std::string& tag, const std::string& body )
	{
		Job* job = new Job();
		job->addref();
		const bool ok = ParseBody( tag, body, *job );
		job->release();
		return ok;
	}

} // namespace S2

static void TestExpressionPainterChunkRegistration()
{
	std::cout << "Test 16: expression_painter parses and registers ONLY as a colour painter (never IFunction2D)" << std::endl;
	Job* job = new Job();
	job->addref();
	const char* body =
		"expression_painter\n{\nname marble\nparam k 4.0\ndef n perlin(P*k)\nexpr vec3(0.5+0.5*n, 0.4, 0.3)\n}\n";
	Check( S2::ParseBody( "epaint1", body, *job ), "expression_painter parses" );
	IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
	Check( priv != 0, "IJobPriv available" );
	if( priv ) {
		Check( priv->GetPainters()->GetItem( "marble" ) != 0, "registered as a colour painter" );
		Check( priv->GetFunction2Ds()->GetItem( "marble" ) == 0,
			"NOT registered as an IFunction2D (avoids the silently-zero P/Po/N trap)" );
	}
	job->release();
}

static void TestExpressionPainterContextVaries()
{
	std::cout << "Test 17: expression_painter body sees P -- two hits at different P give different GetColor" << std::endl;
	Job* job = new Job();
	job->addref();
	const char* body = "expression_painter\n{\nname field\nexpr vec3(P.x, P.y, P.z)\n}\n";
	Check( S2::ParseBody( "epaint2", body, *job ), "parses" );
	IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
	if( priv ) {
		IPainter* p = priv->GetPainters()->GetItem( "field" );
		Check( p != 0, "registered" );
		if( p ) {
			RayIntersectionGeometric r1( Ray(), nullRasterizerState );
			r1.bHit = true; r1.ptIntersection = Point3( 1, 2, 3 );
			RayIntersectionGeometric r2( Ray(), nullRasterizerState );
			r2.bHit = true; r2.ptIntersection = Point3( 9, 8, 7 );
			const RISEPel c1 = p->GetColor( r1 );
			const RISEPel c2 = p->GetColor( r2 );
			CheckClose( c1[0], 1.0, 1e-9, "GetColor(r1).r == P.x" );
			CheckClose( c1[1], 2.0, 1e-9, "GetColor(r1).g == P.y" );
			CheckClose( c1[2], 3.0, 1e-9, "GetColor(r1).b == P.z" );
			Check( c1[0]!=c2[0] || c1[1]!=c2[1] || c1[2]!=c2[2], "GetColor varies with P (r1 != r2)" );
		}
	}
	job->release();
}

static void TestScalarExpressionPerChannelAndUniform()
{
	std::cout << "Test 18: scalar_painter{expression} -- vec3 body is a per-channel triple, scalar body is uniform" << std::endl;
	{
		Job* job = new Job(); job->addref();
		const char* body = "scalar_painter\n{\nname disp\nexpression vec3(1.1, 2.2, 3.3)\n}\n";
		Check( S2::ParseBody( "sexpr1", body, *job ), "vec3-body parses" );
		IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
		if( priv ) {
			IScalarPainter* sp = priv->GetScalarPainters()->GetItem( "disp" );
			Check( sp != 0, "registered" );
			if( sp ) {
				Check( sp->HasPerChannelVariation(), "vec3-typed body reports HasPerChannelVariation" );
				RayIntersectionGeometric r( Ray(), nullRasterizerState ); r.bHit = true;
				const ScalarTriple t = sp->GetValuesAt( r );
				CheckClose( t.v[0], 1.1, 1e-9, "triple.x -> R" );
				CheckClose( t.v[1], 2.2, 1e-9, "triple.y -> G" );
				CheckClose( t.v[2], 3.3, 1e-9, "triple.z -> B" );
				// Mirrors RGBScalarPainter's NM mapping exactly (450/550/650nm).
				CheckClose( sp->GetValueAtNM( r, 650 ), 1.1, 1e-9, "GetValueAtNM(650) == R" );
				CheckClose( sp->GetValueAtNM( r, 550 ), 2.2, 1e-9, "GetValueAtNM(550) == G" );
				CheckClose( sp->GetValueAtNM( r, 450 ), 3.3, 1e-9, "GetValueAtNM(450) == B" );
			}
		}
		job->release();
	}
	{
		Job* job = new Job(); job->addref();
		const char* body = "scalar_painter\n{\nname s1\nexpression 0.42\n}\n";
		Check( S2::ParseBody( "sexpr2", body, *job ), "scalar-body parses" );
		IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
		if( priv ) {
			IScalarPainter* sp = priv->GetScalarPainters()->GetItem( "s1" );
			Check( sp != 0, "registered" );
			if( sp ) {
				Check( !sp->HasPerChannelVariation(), "scalar-typed body reports NO per-channel variation" );
				RayIntersectionGeometric r( Ray(), nullRasterizerState ); r.bHit = true;
				const ScalarTriple t = sp->GetValuesAt( r );
				Check( t.IsUniform(), "triple is uniform" );
				CheckClose( t.v[0], 0.42, 1e-9, "value" );
				CheckClose( sp->GetValueAtNM( r, 700 ), 0.42, 1e-9, "GetValueAtNM is wavelength-independent" );
			}
		}
		job->release();
	}
}

static void TestExpressionPainterParamMetadataRoundTrip()
{
	std::cout << "Test 19: expression_painter param metadata round-trips to GetParamSpecs()" << std::endl;
	Job* job = new Job();
	job->addref();
	const char* body =
		"expression_painter\n{\nname pspec\n"
		"param ring_scale 4.0 min 0.5 max 20 step 0.5 label \"Ring density\"\n"
		"expr vec3(ring_scale, ring_scale, ring_scale)\n}\n";
	Check( S2::ParseBody( "epspec", body, *job ), "parses" );
	IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
	if( priv ) {
		IPainter* p = priv->GetPainters()->GetItem( "pspec" );
		Implementation::ExpressionPainter* ep = dynamic_cast<Implementation::ExpressionPainter*>( p );
		Check( ep != 0, "registered painter is an ExpressionPainter" );
		if( ep ) {
			const std::vector<ParamSpec>& specs = ep->GetParamSpecs();
			Check( specs.size() == 1, "one param spec captured" );
			if( specs.size() == 1 ) {
				const ParamSpec& s = specs[0];
				Check( s.name == "ring_scale", "name" );
				CheckClose( s.value, 4.0, 1e-9, "value" );
				Check( s.hasMin && std::fabs(s.min-0.5) < 1e-9, "min" );
				Check( s.hasMax && std::fabs(s.max-20.0) < 1e-9, "max" );
				Check( s.hasStep && std::fabs(s.step-0.5) < 1e-9, "step" );
				Check( s.hasLabel && s.label == "Ring density", "label" );
			}
		}
	}
	job->release();
}

static void TestExpressionChunkDiagnostics()
{
	std::cout << "Test 20: expression_painter / scalar_painter{expression} diagnostics" << std::endl;
	Check( !S2::ParseBody( "ebad1", "expression_painter\n{\nname noexpr\n}\n" ),
		"expression_painter missing `expr` rejects" );
	Check( !S2::ParseBody( "ebad2", "expression_painter\n{\nname bad\nexpr unknown_var_xyz\n}\n" ),
		"expression_painter unknown-identifier expr rejects" );
	Check( !S2::ParseBody( "sexdup1", "scalar_painter\n{\nname dup\nvalue 0.5\nexpression 0.5\n}\n" ),
		"scalar_painter `expression`+`value` (mutually exclusive) rejects" );
	Check( !S2::ParseBody( "sexdup2", "scalar_painter\n{\nname dup2\ntexture foo\nexpression 0.5\n}\n" ),
		"scalar_painter `expression`+`texture` (mutually exclusive) rejects" );
	Check( S2::ParseBody( "sexok", "scalar_painter\n{\nname ok\nexpression 0.5\n}\n" ),
		"scalar_painter `expression` alone parses" );
}

static void TestExpressionPainterTimeKeyframe()
{
	std::cout << "Test 21: expression_painter `time` keyframe changes GetColor output" << std::endl;
	Job* job = new Job();
	job->addref();
	const char* body = "expression_painter\n{\nname tpaint\nexpr vec3(time, time, time)\ntime 0.0\n}\n";
	Check( S2::ParseBody( "etime", body, *job ), "parses" );
	IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
	if( priv ) {
		IPainter* p = priv->GetPainters()->GetItem( "tpaint" );
		Check( p != 0, "registered" );
		if( p ) {
			RayIntersectionGeometric r( Ray(), nullRasterizerState ); r.bHit = true;
			const RISEPel before = p->GetColor( r );
			CheckClose( before[0], 0.0, 1e-9, "time starts at the chunk's initial value (0.0)" );
			IKeyframeParameter* kp = p->KeyframeFromParameters( "time", "5.0" );
			Check( kp != 0, "KeyframeFromParameters(\"time\", \"5.0\") returns a parameter" );
			if( kp ) {
				p->SetIntermediateValue( *kp );
				const RISEPel after = p->GetColor( r );
				CheckClose( after[0], 5.0, 1e-9, "GetColor reflects the new time after SetIntermediateValue" );
				kp->release();
			}
		}
	}
	job->release();
}

static void TestScalarExpressionRealMaterialBind()
{
	std::cout << "Test 22: scalar_painter{expression} binds to a real material slot (ggx_material alphax/alphay)" << std::endl;
	Job* job = new Job();
	job->addref();
	const char* body =
		"scalar_painter\n{\nname rough\nexpression 0.05 + 0.1*worley_f1(P*2.0, 1.0)\n}\n"
		"ggx_material\n{\nname mat\nalphax rough\nalphay rough\n}\n";
	Check( S2::ParseBody( "matbind", body, *job ), "scalar_painter{expression} + ggx_material parse" );
	IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
	if( priv ) {
		Check( priv->GetScalarPainters()->GetItem( "rough" ) != 0, "scalar_painter registered" );
		Check( priv->GetMaterials()->GetItem( "mat" ) != 0, "ggx_material bound to it and registered" );
	}
	job->release();
}

static void TestExpressionPainterSpectralPath()
{
	std::cout << "Test 23: expression_painter spectral path -- GetColorNM varies with P, GetSpectrum agrees with GetColorNM, a neutral body uplifts near-flat" << std::endl;

	// (a) + (b): a spatially-varying vec3 body.
	{
		Job* job = new Job();
		job->addref();
		const char* body =
			"expression_painter\n{\nname varyfield\nexpr vec3(0.2+0.1*sin(P.x), 0.3+0.1*cos(P.y), 0.5+0.05*P.z)\n}\n";
		Check( S2::ParseBody( "espectral1", body, *job ), "parses" );
		IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
		if( priv ) {
			IPainter* p = priv->GetPainters()->GetItem( "varyfield" );
			Check( p != 0, "registered" );
			if( p ) {
				RayIntersectionGeometric r1( Ray(), nullRasterizerState );
				r1.bHit = true; r1.ptIntersection = Point3( 1, 2, 3 );
				RayIntersectionGeometric r2( Ray(), nullRasterizerState );
				r2.bHit = true; r2.ptIntersection = Point3( 9, 8, 7 );

				// (a) finite at several nm, and differs between r1/r2.
				const Scalar testNMs[3] = { 450, 550, 650 };
				bool anyDiffer = false;
				for( int i = 0; i < 3; ++i ) {
					const Scalar nm = testNMs[i];
					const Scalar c1 = p->GetColorNM( r1, nm );
					const Scalar c2 = p->GetColorNM( r2, nm );
					Check( std::isfinite( (double)c1 ), "GetColorNM(r1) finite at nm" );
					Check( std::isfinite( (double)c2 ), "GetColorNM(r2) finite at nm" );
					if( c1 != c2 ) anyDiffer = true;
				}
				Check( anyDiffer, "GetColorNM differs between two hits with different P" );

				// (b) GetSpectrum's bins agree with GetColorNM at the same
				// wavelength.  GetSpectrum stores s.Eval(lambda_begin + i*delta)
				// at index i (see ExpressionPainter::GetSpectrum); reconstructing
				// the wavelength for SpectralPacket::ValueAtNM's bin lookup via
				// plain subtraction risks a one-ULP-below-integer truncation
				// (int() rounds toward zero), so nudge into the bin interior by
				// a fraction of a bin width -- negligible for a smooth uplift
				// curve, but safely clears the int() truncation edge.
				const Scalar lambda_begin = Scalar(380);
				const Scalar lambda_end   = Scalar(780);
				const unsigned int nbins  = 81;
				const Scalar delta = ( lambda_end - lambda_begin ) / Scalar(nbins);
				const SpectralPacket sp1 = p->GetSpectrum( r1 );
				Check( sp1.NumBins() == nbins, "GetSpectrum bin count" );
				const unsigned int sampleIdx[5] = { 0, 20, 40, 60, 80 };
				for( int k = 0; k < 5; ++k ) {
					const unsigned int i = sampleIdx[k];
					const Scalar nmEdge   = lambda_begin + Scalar(i) * delta;
					const Scalar nmNudged = nmEdge + delta * Scalar(1e-6);
					const Scalar viaSpectrum = sp1.ValueAtNM( nmNudged );
					const Scalar viaColorNM  = p->GetColorNM( r1, nmNudged );
					Check( std::isfinite( (double)viaSpectrum ), "GetSpectrum bin finite" );
					CheckClose( viaSpectrum, viaColorNM, 1e-6, "GetSpectrum bin agrees with GetColorNM at the same nm" );
				}
			}
		}
		job->release();
	}

	// (c) A neutral constant body uplifts to a near-flat spectrum.
	{
		Job* job = new Job();
		job->addref();
		const char* body = "expression_painter\n{\nname neutral\nexpr vec3(0.5, 0.5, 0.5)\n}\n";
		Check( S2::ParseBody( "espectral2", body, *job ), "parses" );
		IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
		if( priv ) {
			IPainter* p = priv->GetPainters()->GetItem( "neutral" );
			Check( p != 0, "registered" );
			if( p ) {
				RayIntersectionGeometric r( Ray(), nullRasterizerState ); r.bHit = true;
				const SpectralPacket sp = p->GetSpectrum( r );
				const Scalar lambda_begin = Scalar(380);
				const Scalar lambda_end   = Scalar(780);
				const unsigned int nbins  = 81;
				const Scalar delta = ( lambda_end - lambda_begin ) / Scalar(nbins);
				// The JH sigmoid uplift is smooth but not perfectly flat for a
				// neutral grey, even off the gamut-edge corner (see
				// docs/JH_LUT_GAMUT.md) -- it rolls off toward the 380/780nm
				// extremes (measured: ~0.16 at the deep-red edge, ~0.55 near
				// 470nm, for an input of 0.5).  The band below is loose enough
				// to tolerate that roll-off while still catching a genuinely
				// broken uplift (negative, zero, wildly overshooting, or
				// non-finite).
				bool allInBand = true;
				for( unsigned int i = 0; i < nbins; ++i ) {
					const Scalar nm = lambda_begin + Scalar(i) * delta + delta * Scalar(1e-6);
					const Scalar v = sp.ValueAtNM( nm );
					if( !std::isfinite( (double)v ) || v < Scalar(0.1) || v > Scalar(0.65) ) allInBand = false;
				}
				Check( allInBand, "neutral vec3(0.5,0.5,0.5) uplifts to a near-flat spectrum in a loose [0.1,0.65] band across all bins" );
			}
		}
		job->release();
	}
}

static void TestScalarExpressionPerChannelInSingleSlotDiagnostic()
{
	std::cout << "Test 24: scalar_painter{expression} vec3 (per-channel) bound to a requireSingle material slot rejects with the targeted diagnostic path" << std::endl;

	// sheen_material's `sheen_roughness` is resolved via
	// ResolveOrDiagnoseScalar( ..., requireSingle = true ) (Job.cpp) --
	// it reads .v[0] only, so a per-channel scalar_painter bound there
	// would silently lose G/B.  Drive it through the real chunk-parser
	// path (ChunkParserRegistry.cpp -> Job::AddSheenMaterial).
	{
		const char* body =
			"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
			"scalar_painter\n{\nname pcvar\nexpression vec3(0.1, 0.5, 0.9)\n}\n"
			"sheen_material\n{\nname mat\nsheen_color white\nsheen_roughness pcvar\n}\n";
		Check( !S2::ParseBody( "reqsingle_bad", body ),
			"sheen_material `sheen_roughness` bound to a per-channel scalar_painter{expression} rejects (a) per-channel-in-single-slot diagnostic path" );
	}

	// Differential control: the SAME slot accepts a scalar-typed (uniform)
	// expression body -- proves the rejection above is specifically about
	// per-channel variation, not scalar_painter{expression} bindings in
	// general.
	{
		const char* body =
			"uniformcolor_painter\n{\nname white2\ncolor 1 1 1\n}\n"
			"scalar_painter\n{\nname uniformvar\nexpression 0.2\n}\n"
			"sheen_material\n{\nname mat2\nsheen_color white2\nsheen_roughness uniformvar\n}\n";
		Check( S2::ParseBody( "reqsingle_ok", body ),
			"sheen_material `sheen_roughness` bound to a scalar-typed (uniform) scalar_painter{expression} parses fine" );
	}
}

static void TestExpressionPainterScalarBroadcastOnColorPipe()
{
	std::cout << "Test 25: expression_painter with a scalar-typed final expr broadcasts to grey on the COLOUR pipe" << std::endl;
	Job* job = new Job();
	job->addref();
	const char* body = "expression_painter\n{\nname grey\nexpr 0.5\n}\n";
	Check( S2::ParseBody( "escalarbcast", body, *job ), "parses (scalar-typed final expr accepted on the colour pipe)" );
	IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
	if( priv ) {
		IPainter* p = priv->GetPainters()->GetItem( "grey" );
		Check( p != 0, "registered" );
		if( p ) {
			RayIntersectionGeometric r( Ray(), nullRasterizerState ); r.bHit = true;
			const RISEPel c = p->GetColor( r );
			CheckClose( c[0], 0.5, 1e-9, "GetColor.r broadcasts scalar" );
			CheckClose( c[1], 0.5, 1e-9, "GetColor.g broadcasts scalar" );
			CheckClose( c[2], 0.5, 1e-9, "GetColor.b broadcasts scalar" );
			const Scalar nm550 = p->GetColorNM( r, 550 );
			Check( std::isfinite( (double)nm550 ), "GetColorNM finite for a scalar-broadcast body" );
			Check( nm550 > Scalar(0), "GetColorNM positive for a scalar-broadcast body" );
		}
	}
	job->release();
}

//======================================================================
// S3 -- P2.1 (scalar_painter{painter} bridge) + P2.2 (ramp_painter).
//======================================================================
namespace S3 {

	// Small mock IPainter with a settable constant RGB + alpha, for
	// exercising channel selection (incl. A, which no in-tree procedural
	// painter carries non-trivially without a real RGBA image file).
	class MockRGBAPainter : public Painter
	{
	protected:
		RISEPel c;
		Scalar  a;
		virtual ~MockRGBAPainter() {}
	public:
		MockRGBAPainter( const RISEPel& c_, Scalar a_ ) : c( c_ ), a( a_ ) {}
		RISEPel GetColor( const RayIntersectionGeometric& ) const override { return c; }
		Scalar  GetAlpha( const RayIntersectionGeometric& ) const override { return a; }
		IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) override { return 0; }
		void SetIntermediateValue( const IKeyframeParameter& ) override {}
		void RegenerateData() override {}
	};

	RayIntersectionGeometric MakeHit( Scalar x = 0, Scalar y = 0, Scalar z = 0 )
	{
		RayIntersectionGeometric r( Ray(), nullRasterizerState );
		r.bHit = true;
		r.ptIntersection = Point3( x, y, z );
		return r;
	}

	// Write a tiny solid-colour PNG to TMPDIR using RISE's own raster
	// image + PNG writer (matches ScalarTexturePainterTest.cpp's helper),
	// so the `texture` form's channel-A rejection test has a REAL image
	// painter that actually constructs -- a nonexistent file would fail
	// at the png_painter step, never reaching the channel-A check.
	std::string WriteTempPNG( const char* tag, double r, double g, double b )
	{
		const char* t = getenv( "TMPDIR" );
		std::string dir = ( t && t[0] ) ? t : "/tmp/";
		if( dir[dir.size()-1] != '/' ) dir += '/';
		const std::string path = dir + "rise_texexprvm_s3_" + tag + ".png";

		IRasterImage* img = 0;
		RISE_API_CreateRISEColorRasterImage( &img, 4, 4, RISEColor( RISEPel( r, g, b ), 1.0 ) );

		IWriteBuffer* buf = 0;
		RISE_API_CreateDiskFileWriteBuffer( &buf, path.c_str() );

		IRasterImageWriter* writer = 0;
		RISE_API_CreatePNGWriter( &writer, *buf, 8, eColorSpace_Rec709RGB_Linear );

		img->DumpImage( writer );

		if( writer ) writer->release();
		if( buf )    buf->release();
		if( img )    img->release();
		return path;
	}

	// Builds a RampPainter over a constant-R MockRGBAPainter(t,t,t,1) and
	// returns GetColor -- the golden-value harness for interpolation mode
	// checks (t is a free parameter per call, unlike a real scene painter).
	RISEPel RampColorForT( RampPainter::Interpolation interp, const std::vector<RampPainter::Stop>& stops, Scalar t )
	{
		MockRGBAPainter* src = new MockRGBAPainter( RISEPel( t, t, t ), Scalar(1) ); src->addref();
		RampPainter* rp = new RampPainter( *src, RampPainter::Channel_R, interp, stops ); rp->addref();
		const RISEPel out = rp->GetColor( MakeHit() );
		rp->release();
		src->release();
		return out;
	}

} // namespace S3

static void TestRampPainterGoldenInterpolation()
{
	std::cout << "Test 26: RampPainter -- linear/constant/smooth golden values at/between/outside stops" << std::endl;
	using S3::RampColorForT;
	const std::vector<RampPainter::Stop> stops = {
		RampPainter::Stop( Scalar(0), RISEPel(0,0,0) ),
		RampPainter::Stop( Scalar(1), RISEPel(10,10,10) ),
		RampPainter::Stop( Scalar(2), RISEPel(100,100,100) ),
	};

	// linear
	{
		CheckClose( RampColorForT( RampPainter::Interp_Linear, stops, Scalar(-1) )[0], 0,   1e-9, "linear: below range clamps to first stop" );
		CheckClose( RampColorForT( RampPainter::Interp_Linear, stops, Scalar(0)  )[0], 0,   1e-9, "linear: exactly at stop 0" );
		CheckClose( RampColorForT( RampPainter::Interp_Linear, stops, Scalar(0.5))[0], 5,   1e-9, "linear: interior of segment 0-1" );
		CheckClose( RampColorForT( RampPainter::Interp_Linear, stops, Scalar(1)  )[0], 10,  1e-9, "linear: exactly at stop 1" );
		CheckClose( RampColorForT( RampPainter::Interp_Linear, stops, Scalar(1.5))[0], 55,  1e-9, "linear: interior of segment 1-2" );
		CheckClose( RampColorForT( RampPainter::Interp_Linear, stops, Scalar(2)  )[0], 100, 1e-9, "linear: exactly at stop 2 (last)" );
		CheckClose( RampColorForT( RampPainter::Interp_Linear, stops, Scalar(5)  )[0], 100, 1e-9, "linear: above range clamps to last stop" );
	}
	// constant (hold-left / step)
	{
		CheckClose( RampColorForT( RampPainter::Interp_Constant, stops, Scalar(-1) )[0], 0,   1e-9, "constant: below range clamps to first stop" );
		CheckClose( RampColorForT( RampPainter::Interp_Constant, stops, Scalar(0)  )[0], 0,   1e-9, "constant: exactly at stop 0" );
		CheckClose( RampColorForT( RampPainter::Interp_Constant, stops, Scalar(0.5))[0], 0,   1e-9, "constant: interior holds the LOWER stop (0)" );
		CheckClose( RampColorForT( RampPainter::Interp_Constant, stops, Scalar(1)  )[0], 10,  1e-9, "constant: exactly at stop 1 jumps to stop 1 (not one interval later)" );
		CheckClose( RampColorForT( RampPainter::Interp_Constant, stops, Scalar(1.5))[0], 10,  1e-9, "constant: interior holds the LOWER stop (1)" );
		CheckClose( RampColorForT( RampPainter::Interp_Constant, stops, Scalar(2)  )[0], 100, 1e-9, "constant: exactly at stop 2 (last)" );
		CheckClose( RampColorForT( RampPainter::Interp_Constant, stops, Scalar(5)  )[0], 100, 1e-9, "constant: above range clamps to last stop" );
	}
	// smooth (smoothstep-eased lerp) -- exact stop values match linear/constant;
	// interior values differ from a plain lerp except at the segment midpoint
	// (smoothstep(0.5) == 0.5 by symmetry, so pick an off-center t to distinguish).
	{
		CheckClose( RampColorForT( RampPainter::Interp_Smooth, stops, Scalar(0)  )[0], 0,   1e-9, "smooth: exactly at stop 0" );
		CheckClose( RampColorForT( RampPainter::Interp_Smooth, stops, Scalar(1)  )[0], 10,  1e-9, "smooth: exactly at stop 1" );
		CheckClose( RampColorForT( RampPainter::Interp_Smooth, stops, Scalar(2)  )[0], 100, 1e-9, "smooth: exactly at stop 2" );
		// u=0.25 -> smoothstep(0.25) = 3*0.0625 - 2*0.015625 = 0.15625
		CheckClose( RampColorForT( RampPainter::Interp_Smooth, stops, Scalar(0.25) )[0], 1.5625, 1e-9, "smooth: off-center interior matches the smoothstep formula" );
		const Scalar linearAt025 = RampColorForT( RampPainter::Interp_Linear, stops, Scalar(0.25) )[0];
		const Scalar smoothAt025 = RampColorForT( RampPainter::Interp_Smooth, stops, Scalar(0.25) )[0];
		Check( std::fabs( linearAt025 - smoothAt025 ) > 1e-6, "smooth differs from linear off-center" );
	}
}

static void TestRampPainterSpectralConsistency()
{
	std::cout << "Test 27: RampPainter -- GetColorNM at exact stops equals that stop's uplifted spectrum; between stops is the linear blend" << std::endl;
	const RISEPel colorLo( 0.9, 0.05, 0.05 );
	const RISEPel colorHi( 0.05, 0.05, 0.9 );
	const RGBAlbedoSpectrum specLo = RGBAlbedoSpectrum::FromRGB( colorLo );
	const RGBAlbedoSpectrum specHi = RGBAlbedoSpectrum::FromRGB( colorHi );
	const Scalar testNMs[3] = { 450, 550, 650 };

	auto colorNMForT = [&]( Scalar t, Scalar nm ) -> Scalar {
		const std::vector<RampPainter::Stop> stops = {
			RampPainter::Stop( Scalar(0), colorLo ),
			RampPainter::Stop( Scalar(1), colorHi ),
		};
		S3::MockRGBAPainter* src = new S3::MockRGBAPainter( RISEPel(t,t,t), Scalar(1) ); src->addref();
		RampPainter* rp = new RampPainter( *src, RampPainter::Channel_R, RampPainter::Interp_Linear, stops ); rp->addref();
		const Scalar v = rp->GetColorNM( S3::MakeHit(), nm );
		rp->release();
		src->release();
		return v;
	};

	for( int i = 0; i < 3; ++i ) {
		const Scalar nm = testNMs[i];
		CheckClose( colorNMForT( 0, nm ), specLo.Eval( nm ), 1e-9, "GetColorNM at exact stop 0 equals stop 0's own uplifted spectrum" );
		CheckClose( colorNMForT( 1, nm ), specHi.Eval( nm ), 1e-9, "GetColorNM at exact stop 1 equals stop 1's own uplifted spectrum" );
		const Scalar expectedMid = Scalar(0.5) * specLo.Eval( nm ) + Scalar(0.5) * specHi.Eval( nm );
		CheckClose( colorNMForT( 0.5, nm ), expectedMid, 1e-9, "GetColorNM at t=0.5 is the linear blend of the two stops' spectra" );
	}
}

static void TestRampPainterChannelSelection()
{
	std::cout << "Test 28: RampPainter -- channel selection (R vs A) routes through the correct source field" << std::endl;
	const std::vector<RampPainter::Stop> stops = {
		RampPainter::Stop( Scalar(0), RISEPel(0,0,0) ),
		RampPainter::Stop( Scalar(1), RISEPel(1,1,1) ),
	};
	// r = 0.0 (drives Channel_R to the first stop), alpha = 1.0 (drives
	// Channel_A to the last stop) -- same source, two different channel
	// selections must land on different stops.
	S3::MockRGBAPainter* src = new S3::MockRGBAPainter( RISEPel(0,0,0), Scalar(1) ); src->addref();
	RampPainter* rpR = new RampPainter( *src, RampPainter::Channel_R, RampPainter::Interp_Linear, stops ); rpR->addref();
	RampPainter* rpA = new RampPainter( *src, RampPainter::Channel_A, RampPainter::Interp_Linear, stops ); rpA->addref();
	const RISEPel outR = rpR->GetColor( S3::MakeHit() );
	const RISEPel outA = rpA->GetColor( S3::MakeHit() );
	CheckClose( outR[0], 0, 1e-9, "Channel_R reads the source's R (0.0) -> first stop" );
	CheckClose( outA[0], 1, 1e-9, "Channel_A reads the source's alpha (1.0) -> last stop" );
	Check( outR[0] != outA[0], "R and A channel selections give different results on the same source" );
	rpR->release(); rpA->release(); src->release();
}

static void TestPainterChannelScalarPainter()
{
	std::cout << "Test 29: PainterChannelScalarPainter -- channel selection (R/G/B/A), scale/bias, uniform triple" << std::endl;
	using Implementation::PainterChannelScalarPainter;
	S3::MockRGBAPainter* src = new S3::MockRGBAPainter( RISEPel(0.2,0.5,0.8), Scalar(0.35) ); src->addref();

	{
		PainterChannelScalarPainter* p = new PainterChannelScalarPainter( *src, PainterChannelScalarPainter::Channel_R, Scalar(1), Scalar(0) ); p->addref();
		const ScalarTriple t = p->GetValuesAt( S3::MakeHit() );
		CheckClose( t.v[0], 0.2, 1e-9, "channel R reads source.GetColor().r" );
		Check( t.IsUniform(), "GetValuesAt is a uniform triple (single channel replicated)" );
		Check( !p->HasPerChannelVariation(), "HasPerChannelVariation is false" );
		CheckClose( p->GetValueAtNM( S3::MakeHit(), 500 ), 0.2, 1e-9, "GetValueAtNM matches GetValuesAt.v[0] (wavelength-independent)" );
		p->release();
	}
	{
		PainterChannelScalarPainter* p = new PainterChannelScalarPainter( *src, PainterChannelScalarPainter::Channel_G, Scalar(1), Scalar(0) ); p->addref();
		CheckClose( p->GetValuesAt( S3::MakeHit() ).v[0], 0.5, 1e-9, "channel G reads source.GetColor().g" );
		p->release();
	}
	{
		PainterChannelScalarPainter* p = new PainterChannelScalarPainter( *src, PainterChannelScalarPainter::Channel_B, Scalar(1), Scalar(0) ); p->addref();
		CheckClose( p->GetValuesAt( S3::MakeHit() ).v[0], 0.8, 1e-9, "channel B reads source.GetColor().b" );
		p->release();
	}
	Scalar chanA = 0, chanR = 0;
	{
		PainterChannelScalarPainter* p = new PainterChannelScalarPainter( *src, PainterChannelScalarPainter::Channel_A, Scalar(1), Scalar(0) ); p->addref();
		chanA = p->GetValuesAt( S3::MakeHit() ).v[0];
		CheckClose( chanA, 0.35, 1e-9, "channel A reads source.GetAlpha()" );
		p->release();
	}
	{
		PainterChannelScalarPainter* p = new PainterChannelScalarPainter( *src, PainterChannelScalarPainter::Channel_R, Scalar(1), Scalar(0) ); p->addref();
		chanR = p->GetValuesAt( S3::MakeHit() ).v[0];
		p->release();
	}
	Check( chanA != chanR, "channel A vs channel R differ on an RGBA source" );
	{
		// scale/bias arithmetic: out = bias + scale*raw
		PainterChannelScalarPainter* p = new PainterChannelScalarPainter( *src, PainterChannelScalarPainter::Channel_R, Scalar(2), Scalar(0.1) ); p->addref();
		CheckClose( p->GetValuesAt( S3::MakeHit() ).v[0], Scalar(0.1) + Scalar(2)*Scalar(0.2), 1e-9, "scale/bias: out = bias + scale*raw" );
		p->release();
	}
	src->release();
}

static void TestRampAndPainterChannelParserDiagnostics()
{
	std::cout << "Test 30: ramp_painter / scalar_painter{painter} -- diagnostics (missing input, <2 stops, non-ascending stops, mutual exclusion)" << std::endl;

	Check( !S2::ParseBody( "ramp_noinput", "ramp_painter\n{\nname r1\nstop 0 0 0 0\nstop 1 1 1 1\n}\n" ),
		"ramp_painter missing `input` rejects" );
	Check( !S2::ParseBody( "ramp_1stop", "ramp_painter\n{\nname r2\ninput bogus\nstop 0 0 0 0\n}\n" ),
		"ramp_painter with < 2 `stop` lines rejects" );
	Check( !S2::ParseBody( "ramp_descend", "ramp_painter\n{\nname r3\ninput bogus\nstop 0 0 0 0\nstop 0.5 1 1 1\nstop 0.2 0 0 0\n}\n" ),
		"ramp_painter with non-ascending stop positions rejects" );
	Check( S2::ParseBody( "ramp_ok", "perlin3d_painter\n{\nname pn_ramp_ok\n}\nramp_painter\n{\nname r4\ninput pn_ramp_ok\nstop 0 0 0 0\nstop 1 1 1 1\n}\n" ),
		"ramp_painter with a valid input + 2 ascending stops parses" );

	Check( !S2::ParseBody( "scalpaint_dup1", "scalar_painter\n{\nname dup3\npainter something\ntexture something\n}\n" ),
		"scalar_painter `painter`+`texture` (mutually exclusive) rejects" );
	Check( !S2::ParseBody( "scalpaint_dup2", "scalar_painter\n{\nname dup4\npainter something\nvalue 0.5\n}\n" ),
		"scalar_painter `painter`+`value` (mutually exclusive) rejects" );
	{
		const std::string png = S3::WriteTempPNG( "texA", 0.4, 0.6, 0.2 );
		const std::string body =
			std::string( "png_painter\n{\nname img\nfile " ) + png + "\ncolor_space Rec709RGB_Linear\n}\n"
			"scalar_painter\n{\nname badchan\ntexture img\nchannel A\n}\n";
		Check( !S2::ParseBody( "scalpaint_texA", body ),
			"scalar_painter `texture` form rejects channel A even with a REAL image painter (TextureScalarPainter has no alpha read)" );
	}
}

static void TestRampPainterChunkRegistrationAndBridgeForm()
{
	std::cout << "Test 31: ramp_painter registers dual (painter + function2d); scalar_painter{painter} bridges perlin3d_painter into ggx_material.alphax" << std::endl;

	// ramp_painter dual registration (unlike expression_painter's
	// deliberate single registration -- ramp_painter is a plain
	// combinator like blend_painter, not a 3D-context source itself).
	{
		Job* job = new Job(); job->addref();
		const char* body =
			"uniformcolor_painter\n{\nname a\ncolor 0 0 0\n}\n"
			"uniformcolor_painter\n{\nname b\ncolor 1 1 1\n}\n"
			"blend_painter\n{\nname driver\ncolora a\ncolorb b\nmask a\n}\n"
			"ramp_painter\n{\nname terrain_ramp\ninput driver\nstop 0 0 0 0\nstop 1 1 1 1\n}\n";
		Check( S2::ParseBody( "rampreg", body, *job ), "parses" );
		IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
		if( priv ) {
			Check( priv->GetPainters()->GetItem( "terrain_ramp" ) != 0, "ramp_painter registered as a colour painter" );
			Check( priv->GetFunction2Ds()->GetItem( "terrain_ramp" ) != 0, "ramp_painter ALSO registered as an IFunction2D (dual registration, like blend_painter)" );
		}
		job->release();
	}

	// The P2.1 any-painter bridge: bind a perlin3d_painter (a 3D-WORLD-SPACE
	// noise field, not a raster texture) into a physical-scalar slot
	// (ggx_material.alphax/alphay) through the REAL parser path.
	{
		Job* job = new Job(); job->addref();
		const char* body =
			"uniformcolor_painter\n{\nname pnt_lo\ncolor 0 0 0\n}\n"
			"uniformcolor_painter\n{\nname pnt_hi\ncolor 1 1 1\n}\n"
			"perlin3d_painter\n{\nname pnoise\ncolora pnt_lo\ncolorb pnt_hi\n}\n"
			"scalar_painter\n{\nname roughvar\npainter pnoise\nchannel R\n}\n"
			"ggx_material\n{\nname mat\nalphax roughvar\nalphay roughvar\n}\n";
		Check( S2::ParseBody( "bridge", body, *job ), "perlin3d_painter -> scalar_painter{painter} -> ggx_material.alphax parses (derive succeeds)" );
		IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
		if( priv ) {
			IScalarPainter* sp = priv->GetScalarPainters()->GetItem( "roughvar" );
			Check( sp != 0, "scalar_painter{painter} registered" );
			Check( priv->GetMaterials()->GetItem( "mat" ) != 0, "ggx_material bound to it and registered" );
			if( sp ) {
				RayIntersectionGeometric r1( Ray(), nullRasterizerState );
				r1.bHit = true; r1.ptIntersection = Point3( 1, 2, 3 );
				RayIntersectionGeometric r2( Ray(), nullRasterizerState );
				r2.bHit = true; r2.ptIntersection = Point3( 97, 41, 23 );
				const ScalarTriple v1 = sp->GetValuesAt( r1 );
				const ScalarTriple v2 = sp->GetValuesAt( r2 );
				Check( std::isfinite( (double)v1.v[0] ) && std::isfinite( (double)v2.v[0] ), "GetValuesAt finite at both hits" );
				Check( v1.v[0] != v2.v[0], "GetValuesAt VARIES across two RIs with different ptIntersection (proves the perlin3d field, not a constant, is driving t)" );
			}
		}
		job->release();
	}
}

// 32. Review round: RampPainter edge inputs -- duplicate stop positions
// (parser permits non-decreasing) and a NaN field sample (mapped to the
// first stop by SampleT rather than poisoning the interpolation weight).
static void TestRampPainterEdgeInputs()
{
	std::cout << "Test 32: RampPainter -- duplicate stop positions + NaN field sample" << std::endl;
	using S3::RampColorForT;

	// Duplicate position: stops at pos 1 hold two different colours.  The
	// locate step's "largest j with pos[j] <= t" rule means the LAST
	// duplicate wins exactly at the shared position, and the zero-width
	// segment can never be selected as an interpolation bracket.
	const std::vector<RampPainter::Stop> dup = {
		RampPainter::Stop( Scalar(0), RISEPel(0,0,0) ),
		RampPainter::Stop( Scalar(1), RISEPel(10,10,10) ),
		RampPainter::Stop( Scalar(1), RISEPel(20,20,20) ),
		RampPainter::Stop( Scalar(2), RISEPel(100,100,100) ),
	};
	CheckClose( RampColorForT( RampPainter::Interp_Linear, dup, Scalar(1) )[0], 20, 1e-9,
	            "duplicate pos: last duplicate wins exactly at the shared position" );
	CheckClose( RampColorForT( RampPainter::Interp_Linear, dup, Scalar(0.5) )[0], 5, 1e-9,
	            "duplicate pos: segment BEFORE the duplicate lerps toward the first duplicate" );
	CheckClose( RampColorForT( RampPainter::Interp_Linear, dup, Scalar(1.5) )[0], 60, 1e-9,
	            "duplicate pos: segment AFTER the duplicate lerps from the last duplicate" );
	const RISEPel dupConst = RampColorForT( RampPainter::Interp_Constant, dup, Scalar(1.5) );
	CheckClose( dupConst[0], 20, 1e-9, "duplicate pos: constant mode holds the last duplicate" );

	// NaN field sample: deterministic first-stop colour, never a NaN pixel.
	const std::vector<RampPainter::Stop> stops = {
		RampPainter::Stop( Scalar(0), RISEPel(3,3,3) ),
		RampPainter::Stop( Scalar(1), RISEPel(9,9,9) ),
	};
	const Scalar nan = std::numeric_limits<Scalar>::quiet_NaN();
	for( int mode = 0; mode < 3; ++mode ) {
		const RISEPel out = RampColorForT( (RampPainter::Interpolation)mode, stops, nan );
		Check( std::isfinite( (double)out[0] ), "NaN t: output is finite" );
		CheckClose( out[0], 3, 1e-9, "NaN t: maps to the FIRST stop in every mode" );
	}
	// +/-inf clamp through the ordered comparisons (no special-casing).
	CheckClose( RampColorForT( RampPainter::Interp_Linear, stops,  std::numeric_limits<Scalar>::infinity() )[0], 9, 1e-9, "+inf t clamps to last stop" );
	CheckClose( RampColorForT( RampPainter::Interp_Linear, stops, -std::numeric_limits<Scalar>::infinity() )[0], 3, 1e-9, "-inf t clamps to first stop" );
}

int main( int, char** )
{
	std::cout << "TextureExpressionVMTest -- ExpressionEval VM S1 (vec3, context vars, noise builtins, ramp, offsets, param-spec)" << std::endl << std::endl;
	TestVec3Arithmetic();
	TestNoiseBuiltins();
	TestTypeErrorOffsets();
	TestBackCompat();
	TestRamp();
	TestOctaveValidation();
	TestParamSpec();
	TestFbmDeterminism();
	TestNoiseFactoringParity();
	TestDuplicateNames();
	TestOctaveAndCellhashRuntimeSafety();
	TestParseDepthAndStackCap();
	TestSpliceAndLexerEdges();
	TestRampMoreEdgesAndMixRejection();
	TestContextVarGating();
	TestExpressionPainterChunkRegistration();
	TestExpressionPainterContextVaries();
	TestScalarExpressionPerChannelAndUniform();
	TestExpressionPainterParamMetadataRoundTrip();
	TestExpressionChunkDiagnostics();
	TestExpressionPainterTimeKeyframe();
	TestScalarExpressionRealMaterialBind();
	TestExpressionPainterSpectralPath();
	TestScalarExpressionPerChannelInSingleSlotDiagnostic();
	TestExpressionPainterScalarBroadcastOnColorPipe();
	TestRampPainterGoldenInterpolation();
	TestRampPainterSpectralConsistency();
	TestRampPainterChannelSelection();
	TestPainterChannelScalarPainter();
	TestRampAndPainterChannelParserDiagnostics();
	TestRampPainterChunkRegistrationAndBridgeForm();
	TestRampPainterEdgeInputs();
	std::cout << std::endl << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
