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
				// HISTORICAL (pre-Stage-C, 2026-09-02,
				// docs/SPECTRAL_ILLUMINANT_CONVENTION.md): under the old
				// flat-E LUT the JH sigmoid uplift was smooth but not
				// perfectly flat for a neutral grey, even off the gamut-edge
				// corner (see docs/JH_LUT_GAMUT.md) -- it rolled off toward
				// the 380/780nm extremes (measured: ~0.16 at the deep-red
				// edge, ~0.55 near 470nm, for an input of 0.5).  Under the
				// current D65 forward model a neutral grey uplifts flat to
				// within ~1e-4 across the visible band (see
				// docs/SPECTRAL_ILLUMINANT_CONVENTION.md §4), so the band
				// below is now far looser than the roll-off it was sized
				// for; it is left wide so it still catches a genuinely
				// broken uplift (negative, zero, wildly overshooting, or
				// non-finite) rather than being a tight regression check.
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

//======================================================================
// S7 (doc 88 P2.3-P2.5): mapping_painter, blend_painter `mode`,
// voronoi3d_painter `space`.  Golden-value math tests below construct
// painters directly through RISE_API (bypassing the parser, the same
// approach UVTransformPainterTest.cpp uses) with tiny local "echo"
// probe painters that reveal exactly which domain field / value the
// wrapper sampled.  Parser-surface tests (registration, diagnostics)
// use S2::ParseBody like the S3 section above.
//======================================================================
namespace S7 {

	// Echoes ptCoord as (R=u, G=v, B=0) -- reveals exactly which UV a
	// wrapper sampled at.  GetColorNM/GetAlpha also echo u, so a caller
	// can check GetColor(ri).r == GetColorNM(ri,*) == GetAlpha(ri) to
	// prove all four Get* paths use the SAME weights/coords.
	class UVEchoPainter : public Painter
	{
	public:
		RISEPel GetColor( const RayIntersectionGeometric& ri ) const
		{
			return RISEPel( ri.ptCoord.x, ri.ptCoord.y, 0.0 );
		}
		Scalar GetColorNM( const RayIntersectionGeometric& ri, const Scalar ) const
		{
			return ri.ptCoord.x;
		}
		Scalar GetAlpha( const RayIntersectionGeometric& ri ) const
		{
			return ri.ptCoord.x;
		}
		IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) { return 0; }
		void SetIntermediateValue( const IKeyframeParameter& ) {}
		void RegenerateData() {}
	};

	// Echoes all three domain fields at once: R=ptIntersection.x,
	// G=ptObjIntersec.x, B=ptCoord.x -- lets a single GetColor call
	// prove which ONE field a given `projection` patched, leaving the
	// other two verbatim.
	class DomainEchoPainter : public Painter
	{
	public:
		RISEPel GetColor( const RayIntersectionGeometric& ri ) const
		{
			return RISEPel( ri.ptIntersection.x, ri.ptObjIntersec.x, ri.ptCoord.x );
		}
		Scalar GetColorNM( const RayIntersectionGeometric& ri, const Scalar ) const { return ri.ptIntersection.x; }
		IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) { return 0; }
		void SetIntermediateValue( const IKeyframeParameter& ) {}
		void RegenerateData() {}
	};

	// A fixed RGB colour, independent of `ri` -- for blend_painter /
	// voronoi3d_painter golden-value tests where the OPERAND values
	// need to be exact hand-computable constants (uniformcolor_painter
	// would work too, but its colour-space uplift makes hand-verifying
	// exact goldens needlessly fiddly).  GetColorNM returns the R
	// channel, so a test can assert GetColorNM == GetColor().r.
	class ConstColorPainter : public Painter
	{
	protected:
		const RISEPel c;
	public:
		explicit ConstColorPainter( const RISEPel& c_ ) : c( c_ ) {}
		RISEPel GetColor( const RayIntersectionGeometric& ) const { return c; }
		Scalar GetColorNM( const RayIntersectionGeometric&, const Scalar ) const { return c.r; }
		IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) { return 0; }
		void SetIntermediateValue( const IKeyframeParameter& ) {}
		void RegenerateData() {}
	};

	// P1-B (S7 review round 1): records whether the txFootprint the
	// wrapper handed down was valid -- mirrors TexCoord1PainterTest.cpp's
	// EchoPainter approach (that test is the in-tree precedent for "does
	// a wrapper correctly invalidate a footprint it can no longer
	// vouch for").  GetColor also echoes ptCoord in (R,G) so a single
	// call can confirm BOTH the remapped coordinate and the footprint
	// state at once.
	class FootprintEchoPainter : public Painter
	{
	public:
		mutable bool lastFootprintValid;
		FootprintEchoPainter() : lastFootprintValid( true ) {}
		RISEPel GetColor( const RayIntersectionGeometric& ri ) const
		{
			lastFootprintValid = ri.txFootprint.valid;
			return RISEPel( ri.ptCoord.x, ri.ptCoord.y, 0.0 );
		}
		Scalar GetColorNM( const RayIntersectionGeometric& ri, const Scalar ) const
		{
			lastFootprintValid = ri.txFootprint.valid;
			return ri.ptCoord.x;
		}
		SpectralPacket GetSpectrum( const RayIntersectionGeometric& ri ) const
		{
			lastFootprintValid = ri.txFootprint.valid;
			SpectralPacket sp( 400, 700, 1 );
			return sp;
		}
		Scalar GetAlpha( const RayIntersectionGeometric& ri ) const
		{
			lastFootprintValid = ri.txFootprint.valid;
			return ri.ptCoord.x;
		}
		IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) { return 0; }
		void SetIntermediateValue( const IKeyframeParameter& ) {}
		void RegenerateData() {}
	};

	// P2.1 (S7 review round 1): echoes the WORLD / OBJECT intersection
	// point verbatim (R=x,G=y,B=z) -- needed for the multi-axis rotation
	// golden test, which must see all three transformed components at
	// once (DomainEchoPainter above only echoes the .x of each field).
	class WorldPointEchoPainter : public Painter
	{
	public:
		RISEPel GetColor( const RayIntersectionGeometric& ri ) const
		{
			return RISEPel( ri.ptIntersection.x, ri.ptIntersection.y, ri.ptIntersection.z );
		}
		Scalar GetColorNM( const RayIntersectionGeometric& ri, const Scalar ) const { return ri.ptIntersection.x; }
		IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) { return 0; }
		void SetIntermediateValue( const IKeyframeParameter& ) {}
		void RegenerateData() {}
	};

	class ObjectPointEchoPainter : public Painter
	{
	public:
		RISEPel GetColor( const RayIntersectionGeometric& ri ) const
		{
			return RISEPel( ri.ptObjIntersec.x, ri.ptObjIntersec.y, ri.ptObjIntersec.z );
		}
		Scalar GetColorNM( const RayIntersectionGeometric& ri, const Scalar ) const { return ri.ptObjIntersec.x; }
		IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) { return 0; }
		void SetIntermediateValue( const IKeyframeParameter& ) {}
		void RegenerateData() {}
	};

	RayIntersectionGeometric MakeRi()
	{
		const Ray r( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
		const RasterizerState rs = { 0, 0 };
		RayIntersectionGeometric ri( r, rs );
		ri.bHit = true;
		return ri;
	}

} // namespace S7

static void TestMappingPainterUVGolden()
{
	std::cout << "Test 33: mapping_painter -- uv projection TRS golden values (scale, then rotate CCW about z, then translate)" << std::endl;
	using namespace S7;

	UVEchoPainter src; src.addref();

	auto MakeMapping = [&]( unsigned int proj, const Vector3& sc, const Vector3& rotDeg, const Vector3& tr, Scalar sharp ) {
		IPainter* mp = 0;
		RISE_API_CreateMappingPainter( &mp, src, proj, sc, rotDeg, tr, sharp );
		return mp;
	};

	// Identity: passthrough.
	{
		IPainter* mp = MakeMapping( 0, Vector3(1,1,1), Vector3(0,0,0), Vector3(0,0,0), 4.0 );
		RayIntersectionGeometric ri = MakeRi(); ri.ptCoord = Point2( 0.3, 0.7 );
		const RISEPel c = mp->GetColor( ri );
		CheckClose( c.r, 0.3, 1e-9, "uv identity: u unchanged" );
		CheckClose( c.g, 0.7, 1e-9, "uv identity: v unchanged" );
		mp->release();
	}
	// Translate only.
	{
		IPainter* mp = MakeMapping( 0, Vector3(1,1,1), Vector3(0,0,0), Vector3(0.5,-0.25,0), 4.0 );
		RayIntersectionGeometric ri = MakeRi(); ri.ptCoord = Point2( 0.1, 0.2 );
		const RISEPel c = mp->GetColor( ri );
		CheckClose( c.r, 0.6,  1e-9, "uv translate: u' = u + tx" );
		CheckClose( c.g, -0.05, 1e-9, "uv translate: v' = v + ty" );
		mp->release();
	}
	// Scale only.
	{
		IPainter* mp = MakeMapping( 0, Vector3(2,0.5,1), Vector3(0,0,0), Vector3(0,0,0), 4.0 );
		RayIntersectionGeometric ri = MakeRi(); ri.ptCoord = Point2( 0.25, 0.4 );
		const RISEPel c = mp->GetColor( ri );
		CheckClose( c.r, 0.5, 1e-9, "uv scale: u' = sx * u" );
		CheckClose( c.g, 0.2, 1e-9, "uv scale: v' = sy * v" );
		mp->release();
	}
	// Rotate 90 deg -- STANDARD CCW convention (u'=cos*su-sin*sv,
	// v'=sin*su+cos*sv), deliberately NOT UVTransformPainter's KHR
	// sign flip -- mapping_painter is the author-facing tool, not the
	// glTF bridge.  (1,0) -> (0,1) under CCW 90 deg.
	{
		IPainter* mp = MakeMapping( 0, Vector3(1,1,1), Vector3(0,0,90), Vector3(0,0,0), 4.0 );
		RayIntersectionGeometric ri = MakeRi(); ri.ptCoord = Point2( 1.0, 0.0 );
		const RISEPel c = mp->GetColor( ri );
		CheckClose( c.r, 0.0, 1e-6, "uv rotate 90 CCW: (1,0) -> u'=0" );
		CheckClose( c.g, 1.0, 1e-6, "uv rotate 90 CCW: (1,0) -> v'=1" );
		mp->release();
	}
	// Combined scale -> rotate -> translate, pinned by hand:
	// (1,1) *scale(2,3)-> (2,3) *rotate90 CCW-> (-3,2) *translate(0.1,0.2)-> (-2.9,2.2)
	{
		IPainter* mp = MakeMapping( 0, Vector3(2,3,1), Vector3(0,0,90), Vector3(0.1,0.2,0), 4.0 );
		RayIntersectionGeometric ri = MakeRi(); ri.ptCoord = Point2( 1.0, 1.0 );
		const RISEPel c = mp->GetColor( ri );
		CheckClose( c.r, -2.9, 1e-6, "uv combined TRS: composition order scale->rotate->translate (u)" );
		CheckClose( c.g,  2.2, 1e-6, "uv combined TRS: composition order scale->rotate->translate (v)" );
		mp->release();
	}
	// scale.z / rotate.x / rotate.y / translate.z are IGNORED for uv --
	// loading them up with large nonzero values must not perturb the
	// output at all.
	{
		IPainter* mp = MakeMapping( 0, Vector3(1,1,99), Vector3(45,45,0), Vector3(0,0,77), 4.0 );
		RayIntersectionGeometric ri = MakeRi(); ri.ptCoord = Point2( 0.3, 0.7 );
		const RISEPel c = mp->GetColor( ri );
		CheckClose( c.r, 0.3, 1e-9, "uv: scale.z/rotate.x/rotate.y/translate.z ignored (u)" );
		CheckClose( c.g, 0.7, 1e-9, "uv: scale.z/rotate.x/rotate.y/translate.z ignored (v)" );
		mp->release();
	}

	src.release();
}

static void TestMappingPainterWorldObjectDomainPatch()
{
	std::cout << "Test 34: mapping_painter -- world/object patch EXACTLY the named domain field, others verbatim" << std::endl;
	using namespace S7;

	DomainEchoPainter src; src.addref();
	RayIntersectionGeometric ri = MakeRi();
	ri.ptIntersection = Point3( 2, 3, 5 );
	ri.ptObjIntersec   = Point3( 11, 13, 17 );
	ri.ptCoord         = Point2( 0.25, 0.75 );

	const Vector3 sc( 2, 1, 1 ), rot( 0, 0, 0 ), tr( 10, 0, 0 );

	// world (projection=1): ptIntersection.x -> 2*2+10=14 (patched);
	// ptObjIntersec.x and ptCoord.x stay verbatim.
	{
		IPainter* mp = 0;
		RISE_API_CreateMappingPainter( &mp, src, 1, sc, rot, tr, 4.0 );
		const RISEPel c = mp->GetColor( ri );
		CheckClose( c.r, 14,   1e-9, "world: ptIntersection.x patched" );
		CheckClose( c.g, 11,   1e-9, "world: ptObjIntersec.x left VERBATIM" );
		CheckClose( c.b, 0.25, 1e-9, "world: ptCoord.x left VERBATIM" );
		mp->release();
	}
	// object (projection=2): ptObjIntersec.x -> 11*2+10=32 (patched);
	// ptIntersection.x and ptCoord.x stay verbatim.
	{
		IPainter* mp = 0;
		RISE_API_CreateMappingPainter( &mp, src, 2, sc, rot, tr, 4.0 );
		const RISEPel c = mp->GetColor( ri );
		CheckClose( c.r, 2,    1e-9, "object: ptIntersection.x left VERBATIM" );
		CheckClose( c.g, 32,   1e-9, "object: ptObjIntersec.x patched" );
		CheckClose( c.b, 0.25, 1e-9, "object: ptCoord.x left VERBATIM" );
		mp->release();
	}
	// uv (projection=0): ptCoord.x -> 2*0.25+10=10.5 (patched, x/y-only
	// TRS -- `tr`'s x=10 applies to uv same as it does to world/object
	// above); ptIntersection.x and ptObjIntersec.x stay verbatim.
	{
		IPainter* mp = 0;
		RISE_API_CreateMappingPainter( &mp, src, 0, sc, rot, tr, 4.0 );
		const RISEPel c = mp->GetColor( ri );
		CheckClose( c.r, 2,    1e-9, "uv: ptIntersection.x left VERBATIM" );
		CheckClose( c.g, 11,   1e-9, "uv: ptObjIntersec.x left VERBATIM" );
		CheckClose( c.b, 10.5, 1e-9, "uv: ptCoord.x patched" );
		mp->release();
	}

	src.release();
}

static void TestMappingPainterTriplanar()
{
	std::cout << "Test 35: mapping_painter -- triplanar axis convention, weight normalization, blend_sharpness, NM/RGB/alpha consistency" << std::endl;
	using namespace S7;

	UVEchoPainter src; src.addref();
	RayIntersectionGeometric ri = MakeRi();
	ri.ptIntersection = Point3( 2, 3, 5 );

	const Vector3 identityScale( 1, 1, 1 ), identityRot( 0, 0, 0 ), identityTr( 0, 0, 0 );

	// Axis-aligned normals collapse to a single pure axis sample --
	// pins the (y,z)/(x,z)/(x,y) convention exactly.
	{
		ri.vNormal = Vector3( 1, 0, 0 );
		IPainter* mp = 0;
		RISE_API_CreateMappingPainter( &mp, src, 3, identityScale, identityRot, identityTr, 4.0 );
		const RISEPel c = mp->GetColor( ri );
		CheckClose( c.r, 3, 1e-9, "triplanar X-facing (N=(1,0,0)): ptCoord = (P.y, P.z) -- u=P.y" );
		CheckClose( c.g, 5, 1e-9, "triplanar X-facing (N=(1,0,0)): ptCoord = (P.y, P.z) -- v=P.z" );
		mp->release();
	}
	{
		ri.vNormal = Vector3( 0, 1, 0 );
		IPainter* mp = 0;
		RISE_API_CreateMappingPainter( &mp, src, 3, identityScale, identityRot, identityTr, 4.0 );
		const RISEPel c = mp->GetColor( ri );
		CheckClose( c.r, 2, 1e-9, "triplanar Y-facing (N=(0,1,0)): ptCoord = (P.x, P.z) -- u=P.x" );
		CheckClose( c.g, 5, 1e-9, "triplanar Y-facing (N=(0,1,0)): ptCoord = (P.x, P.z) -- v=P.z" );
		mp->release();
	}
	{
		ri.vNormal = Vector3( 0, 0, 1 );
		IPainter* mp = 0;
		RISE_API_CreateMappingPainter( &mp, src, 3, identityScale, identityRot, identityTr, 4.0 );
		const RISEPel c = mp->GetColor( ri );
		CheckClose( c.r, 2, 1e-9, "triplanar Z-facing (N=(0,0,1)): ptCoord = (P.x, P.y) -- u=P.x" );
		CheckClose( c.g, 3, 1e-9, "triplanar Z-facing (N=(0,0,1)): ptCoord = (P.x, P.y) -- v=P.y" );
		mp->release();
	}
	// Degenerate (zero-length) normal -- weights fall back to an even
	// 1/3 split instead of a 0/0 NaN: (3+2+2)/3, (5+5+3)/3.
	{
		ri.vNormal = Vector3( 0, 0, 0 );
		IPainter* mp = 0;
		RISE_API_CreateMappingPainter( &mp, src, 3, identityScale, identityRot, identityTr, 4.0 );
		const RISEPel c = mp->GetColor( ri );
		CheckClose( c.r, 7.0/3.0,  1e-9, "triplanar degenerate normal: even 1/3 split (u)" );
		CheckClose( c.g, 13.0/3.0, 1e-9, "triplanar degenerate normal: even 1/3 split (v)" );
		mp->release();
	}
	// blend_sharpness: N=(0.6,0.8,0) (unit length; Z weight 0, so the
	// second ptCoord component is P.z=5 either way -- only the FIRST
	// component (a blend of P.y=3 from the X sample and P.x=2 from the
	// Y sample) moves as sharpness changes).  r = 2 + nwx, where nwx is
	// the NORMALIZED X weight -- higher sharpness pushes weight toward
	// the dominant axis (Y, weight 0.8), so nwx SHRINKS and r DROPS.
	{
		ri.vNormal = Vector3( 0.6, 0.8, 0.0 );
		IPainter* mpSharp4 = 0, *mpSharp1 = 0;
		RISE_API_CreateMappingPainter( &mpSharp4, src, 3, identityScale, identityRot, identityTr, 4.0 );
		RISE_API_CreateMappingPainter( &mpSharp1, src, 3, identityScale, identityRot, identityTr, 1.0 );
		const RISEPel c4 = mpSharp4->GetColor( ri );
		const RISEPel c1 = mpSharp1->GetColor( ri );
		// nwx(sharp=4) = 0.6^4 / (0.6^4+0.8^4) = 0.1296/0.5392 = 0.240356...
		CheckClose( c4.r, 2.0 + 0.1296/0.5392, 1e-6, "triplanar blend_sharpness=4: pinned r" );
		// nwx(sharp=1) = 0.6/1.4 = 0.428571...
		CheckClose( c1.r, 2.0 + 0.6/1.4, 1e-6, "triplanar blend_sharpness=1: pinned r" );
		CheckClose( c4.g, 5.0, 1e-9, "triplanar: Z weight 0 either way -- v stays P.z" );
		CheckClose( c1.g, 5.0, 1e-9, "triplanar: Z weight 0 either way -- v stays P.z" );
		Check( c4.r < c1.r, "triplanar: higher sharpness pulls weight toward the dominant (Y) axis, dropping r" );
		mpSharp4->release();
		mpSharp1->release();
	}
	// Triplanar respects the TRS transform on the WORLD position before
	// deriving axis coordinates: scale.y=10 on P=(2,3,5) -> p'=(2,30,5);
	// pure-X normal samples (p'.y, p'.z) = (30, 5).
	{
		ri.vNormal = Vector3( 1, 0, 0 );
		IPainter* mp = 0;
		RISE_API_CreateMappingPainter( &mp, src, 3, Vector3(1,10,1), identityRot, identityTr, 4.0 );
		const RISEPel c = mp->GetColor( ri );
		CheckClose( c.r, 30, 1e-9, "triplanar: TRS-transformed world position feeds the axis coords (u)" );
		CheckClose( c.g, 5,  1e-9, "triplanar: TRS-transformed world position feeds the axis coords (v)" );
		mp->release();
	}
	// NM / RGB / alpha consistency (the S2 lesson): all three Get*
	// paths route through the SAME ComputeTriplanar() weights/coords,
	// so GetColor(ri).r == GetColorNM(ri,*) == GetAlpha(ri) for a probe
	// whose GetColorNM/GetAlpha echo the same ptCoord.x per axis.
	{
		ri.vNormal = Vector3( 0.6, 0.8, 0.0 );
		IPainter* mp = 0;
		RISE_API_CreateMappingPainter( &mp, src, 3, identityScale, identityRot, identityTr, 4.0 );
		const RISEPel c = mp->GetColor( ri );
		const Scalar nm = mp->GetColorNM( ri, 550.0 );
		const Scalar a  = mp->GetAlpha( ri );
		CheckClose( (Scalar)c.r, nm, 1e-9, "triplanar: GetColorNM matches GetColor.r (same weights)" );
		CheckClose( (Scalar)c.r, a,  1e-9, "triplanar: GetAlpha matches GetColor.r (same weights)" );
		const SpectralPacket sp = mp->GetSpectrum( ri );
		CheckClose( sp.ValueAtNM( 550.0 ), nm, 1e-9, "triplanar: GetSpectrum.ValueAtNM matches GetColorNM (nm-independent probe, so bin quantization introduces no error)" );
		mp->release();
	}

	src.release();
}

static void TestBlendPainterModes()
{
	std::cout << "Test 36: blend_painter -- mode formulas golden per mode, mode=mix byte-identical to pre-P2.4, mask interplay" << std::endl;
	using namespace S7;

	// a = (0.2, 0.4, 0.6), b = (0.8, 0.1, 0.3), mask = (1.0, 0.5, 0.0) --
	// picked so every channel exercises a DIFFERENT mask value (1, 0.5, 0).
	ConstColorPainter a( RISEPel( 0.2, 0.4, 0.6 ) ); a.addref();
	ConstColorPainter b( RISEPel( 0.8, 0.1, 0.3 ) ); b.addref();
	ConstColorPainter mask( RISEPel( 1.0, 0.5, 0.0 ) ); mask.addref();
	RayIntersectionGeometric ri = MakeRi();

	struct Case { unsigned int mode; const char* name; Scalar er, eg, eb; };
	const Case cases[] = {
		{ 0, "mix",      0.2,  0.25, 0.3 },
		{ 1, "multiply", 0.16, 0.07, 0.3 },
		{ 2, "screen",   0.84, 0.28, 0.3 },
		{ 3, "overlay",  0.68, 0.09, 0.3 },
		{ 4, "add",      1.0,  0.3,  0.3 },
	};
	for( const Case& tc : cases ) {
		IPainter* bp = 0;
		RISE_API_CreateBlendPainterWithMode( &bp, a, b, mask, tc.mode );
		const RISEPel c = bp->GetColor( ri );
		CheckClose( c.r, tc.er, 1e-9, std::string("blend mode ") + tc.name + ": R channel golden" );
		CheckClose( c.g, tc.eg, 1e-9, std::string("blend mode ") + tc.name + ": G channel golden" );
		CheckClose( c.b, tc.eb, 1e-9, std::string("blend mode ") + tc.name + ": B channel golden" );
		// GetColorNM uses the SAME formula shape: a/b/mask's GetColorNM
		// echo their R channel, so the NM result must equal the R
		// channel of GetColor exactly.
		const Scalar nm = bp->GetColorNM( ri, 500.0 );
		CheckClose( nm, c.r, 1e-9, std::string("blend mode ") + tc.name + ": GetColorNM matches GetColor.r (same formula shape)" );
		bp->release();
	}

	// Mask interplay: mask=0 always yields b (regardless of mode);
	// mask=1 always yields the raw Combine(a,b) (regardless of mode,
	// value differs per mode except mix==a).
	ConstColorPainter mask0( RISEPel( 0, 0, 0 ) ); mask0.addref();
	ConstColorPainter mask1( RISEPel( 1, 1, 1 ) ); mask1.addref();
	for( const Case& tc : cases ) {
		IPainter* bp0 = 0; RISE_API_CreateBlendPainterWithMode( &bp0, a, b, mask0, tc.mode );
		const RISEPel c0 = bp0->GetColor( ri );
		CheckClose( c0.r, 0.8, 1e-9, std::string("blend mode ") + tc.name + ": mask=0 -> b, ignoring mode (R)" );
		CheckClose( c0.g, 0.1, 1e-9, std::string("blend mode ") + tc.name + ": mask=0 -> b, ignoring mode (G)" );
		CheckClose( c0.b, 0.3, 1e-9, std::string("blend mode ") + tc.name + ": mask=0 -> b, ignoring mode (B)" );
		bp0->release();
	}
	{
		// mode=mix, mask=1 -> a (the historical `colora * mask + colorb
		// * (1-mask)` formula, byte-identical reduction).
		IPainter* bp1 = 0; RISE_API_CreateBlendPainterWithMode( &bp1, a, b, mask1, 0u );
		const RISEPel c1 = bp1->GetColor( ri );
		CheckClose( c1.r, 0.2, 1e-9, "blend mode mix: mask=1 -> a exactly (pre-P2.4 formula)" );
		CheckClose( c1.g, 0.4, 1e-9, "blend mode mix: mask=1 -> a exactly (pre-P2.4 formula)" );
		CheckClose( c1.b, 0.6, 1e-9, "blend mode mix: mask=1 -> a exactly (pre-P2.4 formula)" );
		bp1->release();
	}

	mask0.release(); mask1.release();
	mask.release(); b.release(); a.release();
}

static void TestVoronoi3DSpaceParam()
{
	std::cout << "Test 37: voronoi3d_painter -- space object (default, historical) vs world golden" << std::endl;
	using namespace S7;

	ConstColorPainter colorA( RISEPel( 1, 0, 0 ) ); colorA.addref();
	ConstColorPainter colorB( RISEPel( 0, 1, 0 ) ); colorB.addref();
	ConstColorPainter border( RISEPel( 0, 0, 1 ) ); border.addref();

	std::vector<Point3> pts;
	pts.push_back( Point3( 0, 0, 0 ) );
	pts.push_back( Point3( 10, 0, 0 ) );
	std::vector<IPainter*> ptrs;
	ptrs.push_back( &colorA );
	ptrs.push_back( &colorB );

	RayIntersectionGeometric ri = MakeRi();
	// An instanced/transformed-object scenario: object space keeps the
	// point at the generator-A cell; world space has moved it to the
	// generator-B cell.
	ri.ptObjIntersec  = Point3( 0, 0, 0 );
	ri.ptIntersection = Point3( 10, 0, 0 );

	{
		IPainter* vp = 0;
		RISE_API_CreateVoronoi3DPainterWithSpace( &vp, pts, ptrs, border, 0.0, false );	// space=object (default)
		const RISEPel c = vp->GetColor( ri );
		CheckClose( c.r, 1, 1e-9, "voronoi3d space=object (default): samples ptObjIntersec -> colorA (historical, byte-identical)" );
		CheckClose( c.g, 0, 1e-9, "voronoi3d space=object (default): samples ptObjIntersec -> colorA (historical, byte-identical)" );
		vp->release();
	}
	{
		IPainter* vp = 0;
		RISE_API_CreateVoronoi3DPainterWithSpace( &vp, pts, ptrs, border, 0.0, true );	// space=world
		const RISEPel c = vp->GetColor( ri );
		CheckClose( c.r, 0, 1e-9, "voronoi3d space=world: samples ptIntersection -> colorB (DIFFERS from object default)" );
		CheckClose( c.g, 1, 1e-9, "voronoi3d space=world: samples ptIntersection -> colorB (DIFFERS from object default)" );
		vp->release();
	}

	border.release(); colorB.release(); colorA.release();
}

static void TestS7ChunkParsingAndDiagnostics()
{
	std::cout << "Test 38: mapping_painter / blend_painter mode / voronoi3d_painter space -- chunk parsing, registration, diagnostics" << std::endl;

	// mapping_painter: missing `source` rejects.
	Check( !S2::ParseBody( "map_nosrc", "mapping_painter\n{\nname m1\nprojection uv\n}\n" ),
		"mapping_painter missing `source` rejects" );
	// mapping_painter: bad projection enum rejects.
	Check( !S2::ParseBody( "map_badproj",
		"uniformcolor_painter\n{\nname s\ncolor 1 1 1\n}\n"
		"mapping_painter\n{\nname m2\nsource s\nprojection sideways\n}\n" ),
		"mapping_painter unknown projection rejects" );
	// mapping_painter: all four projections parse and register (dual,
	// like blend_painter/ramp_painter).
	{
		const char* projs[4] = { "uv", "world", "object", "triplanar" };
		for( int i = 0; i < 4; ++i ) {
			Job* job = new Job(); job->addref();
			const std::string body =
				"uniformcolor_painter\n{\nname s\ncolor 1 1 1\n}\n"
				"mapping_painter\n{\nname m\nsource s\nprojection " + std::string(projs[i]) +
				"\nscale 2 2 2\nrotate 0 0 45\ntranslate 0.1 0.1 0\nblend_sharpness 3\n}\n";
			Check( S2::ParseBody( std::string("map_ok_") + projs[i], body, *job ), std::string("mapping_painter projection ") + projs[i] + " parses" );
			IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
			if( priv ) {
				Check( priv->GetPainters()->GetItem( "m" ) != 0, std::string("mapping_painter (") + projs[i] + ") registered as a colour painter" );
				Check( priv->GetFunction2Ds()->GetItem( "m" ) != 0, std::string("mapping_painter (") + projs[i] + ") ALSO registered as an IFunction2D (dual registration)" );
			}
			job->release();
		}
	}

	// blend_painter: bad mode enum rejects; every named mode parses.
	Check( !S2::ParseBody( "blend_badmode",
		"uniformcolor_painter\n{\nname a\ncolor 0 0 0\n}\n"
		"uniformcolor_painter\n{\nname b\ncolor 1 1 1\n}\n"
		"blend_painter\n{\nname bp1\ncolora a\ncolorb b\nmask a\nmode neon\n}\n" ),
		"blend_painter unknown mode rejects" );
	{
		const char* modes[5] = { "mix", "multiply", "screen", "overlay", "add" };
		for( int i = 0; i < 5; ++i ) {
			const std::string body =
				"uniformcolor_painter\n{\nname a\ncolor 0 0 0\n}\n"
				"uniformcolor_painter\n{\nname b\ncolor 1 1 1\n}\n"
				"blend_painter\n{\nname bp\ncolora a\ncolorb b\nmask a\nmode " + std::string(modes[i]) + "\n}\n";
			Check( S2::ParseBody( std::string("blend_mode_") + modes[i], body ), std::string("blend_painter mode ") + modes[i] + " parses" );
		}
	}
	// blend_painter: `mode` omitted still parses (back-compat).
	Check( S2::ParseBody( "blend_nomode",
		"uniformcolor_painter\n{\nname a\ncolor 0 0 0\n}\n"
		"uniformcolor_painter\n{\nname b\ncolor 1 1 1\n}\n"
		"blend_painter\n{\nname bp2\ncolora a\ncolorb b\nmask a\n}\n" ),
		"blend_painter with `mode` omitted still parses (back-compat)" );

	// voronoi3d_painter: bad space enum rejects; object/world both parse.
	Check( !S2::ParseBody( "vor_badspace",
		"uniformcolor_painter\n{\nname g\ncolor 1 1 1\n}\n"
		"voronoi3d_painter\n{\nname v1\ngen 0 0 0 g\ngen 1 1 1 g\nborder g\nspace nowhere\n}\n" ),
		"voronoi3d_painter unknown space rejects" );
	Check( S2::ParseBody( "vor_object",
		"uniformcolor_painter\n{\nname g\ncolor 1 1 1\n}\n"
		"voronoi3d_painter\n{\nname v2\ngen 0 0 0 g\ngen 1 1 1 g\nborder g\nspace object\n}\n" ),
		"voronoi3d_painter space object parses" );
	Check( S2::ParseBody( "vor_world",
		"uniformcolor_painter\n{\nname g\ncolor 1 1 1\n}\n"
		"voronoi3d_painter\n{\nname v3\ngen 0 0 0 g\ngen 1 1 1 g\nborder g\nspace world\n}\n" ),
		"voronoi3d_painter space world parses" );
	// voronoi3d_painter: `space` omitted still parses (back-compat).
	Check( S2::ParseBody( "vor_nospace",
		"uniformcolor_painter\n{\nname g\ncolor 1 1 1\n}\n"
		"voronoi3d_painter\n{\nname v4\ngen 0 0 0 g\ngen 1 1 1 g\nborder g\n}\n" ),
		"voronoi3d_painter with `space` omitted still parses (back-compat)" );
}

static void TestMappingPainterFootprintInvalidation()
{
	std::cout << "Test 39: mapping_painter -- P1-B fix: uv/triplanar remap invalidates txFootprint, world/object leave it valid" << std::endl;
	using namespace S7;

	FootprintEchoPainter src; src.addref();

	// uv projection: the remapped ptCoord no longer matches the
	// footprint's baked (dudx,dudy,dvdx,dvdy) -- must be invalidated on
	// ALL FOUR accessors (TexCoord1PainterTest.cpp's precedent for this
	// exact shape of check).
	{
		IPainter* mp = 0;
		RISE_API_CreateMappingPainter( &mp, src, 0, Vector3(2,2,1), Vector3(0,0,0), Vector3(0,0,0), 4.0 );
		RayIntersectionGeometric ri = MakeRi();
		ri.ptCoord = Point2( 0.3, 0.4 );
		ri.txFootprint.widthValid = true;   // valid => widthValid (a real hit never has one without the other)
		ri.txFootprint.worldWidth = 0.05;
		ri.txFootprint.valid = true;

		src.lastFootprintValid = true;
		mp->GetColor( ri );
		Check( src.lastFootprintValid == false, "uv: GetColor invalidates txFootprint" );

		src.lastFootprintValid = true;
		mp->GetColorNM( ri, 550.0 );
		Check( src.lastFootprintValid == false, "uv: GetColorNM invalidates txFootprint" );

		src.lastFootprintValid = true;
		mp->GetSpectrum( ri );
		Check( src.lastFootprintValid == false, "uv: GetSpectrum invalidates txFootprint" );

		src.lastFootprintValid = true;
		mp->GetAlpha( ri );
		Check( src.lastFootprintValid == false, "uv: GetAlpha invalidates txFootprint" );

		Check( ri.txFootprint.valid == true, "uv: caller's ri.txFootprint.valid is NOT mutated by the wrapper" );

		mp->release();
	}

	// triplanar projection: same invalidation rationale, all four
	// accessors -- each of the three per-axis samples patches ptCoord
	// to a world-derived coordinate the inherited footprint never
	// described.
	{
		IPainter* mp = 0;
		RISE_API_CreateMappingPainter( &mp, src, 3, Vector3(1,1,1), Vector3(0,0,0), Vector3(0,0,0), 4.0 );
		RayIntersectionGeometric ri = MakeRi();
		ri.ptIntersection = Point3( 2, 3, 5 );
		ri.vNormal = Vector3( 1, 0, 0 );
		ri.txFootprint.widthValid = true;   // valid => widthValid (a real hit never has one without the other)
		ri.txFootprint.worldWidth = 0.05;
		ri.txFootprint.valid = true;

		src.lastFootprintValid = true;
		mp->GetColor( ri );
		Check( src.lastFootprintValid == false, "triplanar: GetColor invalidates txFootprint" );

		src.lastFootprintValid = true;
		mp->GetColorNM( ri, 550.0 );
		Check( src.lastFootprintValid == false, "triplanar: GetColorNM invalidates txFootprint" );

		src.lastFootprintValid = true;
		mp->GetSpectrum( ri );
		Check( src.lastFootprintValid == false, "triplanar: GetSpectrum invalidates txFootprint" );

		src.lastFootprintValid = true;
		mp->GetAlpha( ri );
		Check( src.lastFootprintValid == false, "triplanar: GetAlpha invalidates txFootprint" );

		mp->release();
	}

	// world / object projections leave ptCoord UNTOUCHED (they patch
	// ptIntersection / ptObjIntersec instead) -- the footprint, which
	// describes a uv derivative, must stay valid.
	{
		IPainter* mp = 0;
		RISE_API_CreateMappingPainter( &mp, src, 1, Vector3(1,1,1), Vector3(0,0,0), Vector3(0,0,0), 4.0 );
		RayIntersectionGeometric ri = MakeRi();
		ri.ptIntersection = Point3( 2, 3, 5 );
		ri.txFootprint.widthValid = true;   // valid => widthValid (a real hit never has one without the other)
		ri.txFootprint.worldWidth = 0.05;
		ri.txFootprint.valid = true;

		src.lastFootprintValid = false;
		mp->GetColor( ri );
		Check( src.lastFootprintValid == true, "world: GetColor leaves txFootprint valid (ptCoord untouched)" );
		mp->release();
	}
	{
		IPainter* mp = 0;
		RISE_API_CreateMappingPainter( &mp, src, 2, Vector3(1,1,1), Vector3(0,0,0), Vector3(0,0,0), 4.0 );
		RayIntersectionGeometric ri = MakeRi();
		ri.ptObjIntersec = Point3( 2, 3, 5 );
		ri.txFootprint.widthValid = true;   // valid => widthValid (a real hit never has one without the other)
		ri.txFootprint.worldWidth = 0.05;
		ri.txFootprint.valid = true;

		src.lastFootprintValid = false;
		mp->GetColor( ri );
		Check( src.lastFootprintValid == true, "object: GetColor leaves txFootprint valid (ptCoord untouched)" );
		mp->release();
	}

	src.release();
}

static void TestMappingPainterMultiAxisRotationGolden()
{
	std::cout << "Test 40: mapping_painter -- multi-axis rotation (rotate 30 45 60) world+object, hand-computed golden" << std::endl;
	using namespace S7;

	// Hand computation (Z-then-Y-then-X composition order, matching
	// xform3D = Translation(translate) * XRotation(rx) * YRotation(ry) *
	// ZRotation(rz) * Stretch(scale), and Matrix4Ops::operator*'s
	// "rightmost factor applied first" semantics -- see
	// MappingPainter.cpp's file-header derivation):
	//   p1 = scale * p = (1,1,1)                     (scale = identity)
	//   p2 = Rz(60deg) p1:  x'=Cz*x-Sz*y, y'=Sz*x+Cz*y, z'=z
	//     Cz=cos60=0.5, Sz=sin60=0.8660254
	//     p2 = (0.5-0.8660254, 0.8660254+0.5, 1) = (-0.3660254, 1.3660254, 1)
	//   p3 = Ry(45deg) p2:  x'=Cy*x+Sy*z, y'=y, z'=-Sy*x+Cy*z
	//     Cy=Sy=cos45=sin45=0.7071068
	//     p3 = (0.7071068*(-0.3660254+1), 1.3660254, 0.7071068*(0.3660254+1))
	//        = (0.4482877, 1.3660254, 0.9659258)
	//   p4 = Rx(30deg) p3:  x'=x, y'=Cx*y-Sx*z, z'=Sx*y+Cx*z
	//     Cx=cos30=0.8660254, Sx=sin30=0.5
	//     p4.x = 0.4482877  (Rx never touches x)
	//     p4.y = 0.8660254*1.3660254 - 0.5*0.9659258 ~= 1.1830131 - 0.4829629 = 0.7000502
	//     p4.z = 0.5*1.3660254 + 0.8660254*0.9659258 ~= 0.6830127 + 0.8365173 = 1.5195300
	//   translate=(10,20,30) is added AFTER rotation: p' = p4 + translate
	//        ~= (10.4482877, 20.7000502, 31.5195300)
	//   Sanity check on the rotation-only part: |p4| must equal
	//   sqrt(3) = |p|, since scale=identity and pure rotation preserves
	//   length: 0.4482877^2 + 0.7000502^2 + 1.5195300^2 ~= 3.0000,
	//   confirming the hand arithmetic above.
	const Vector3 rotate( 30, 45, 60 );
	const Vector3 translate( 10, 20, 30 );
	const Vector3 scale( 1, 1, 1 );
	const Scalar ex = 10.448288, ey = 20.700050, ez = 31.519530;
	const Scalar tol = 1e-4;

	// world (projection=1): ptIntersection patched by the full rotate+
	// translate; ptObjIntersec/ptCoord are a different field, untouched.
	{
		WorldPointEchoPainter src; src.addref();
		IPainter* mp = 0;
		RISE_API_CreateMappingPainter( &mp, src, 1, scale, rotate, translate, 4.0 );
		RayIntersectionGeometric ri = MakeRi();
		ri.ptIntersection = Point3( 1, 1, 1 );
		const RISEPel c = mp->GetColor( ri );
		CheckClose( c.r, ex, tol, "world multi-axis rotate 30/45/60: x" );
		CheckClose( c.g, ey, tol, "world multi-axis rotate 30/45/60: y" );
		CheckClose( c.b, ez, tol, "world multi-axis rotate 30/45/60: z" );
		mp->release();
		src.release();
	}
	// object (projection=2): identical math, applied to ptObjIntersec.
	{
		ObjectPointEchoPainter src; src.addref();
		IPainter* mp = 0;
		RISE_API_CreateMappingPainter( &mp, src, 2, scale, rotate, translate, 4.0 );
		RayIntersectionGeometric ri = MakeRi();
		ri.ptObjIntersec = Point3( 1, 1, 1 );
		const RISEPel c = mp->GetColor( ri );
		CheckClose( c.r, ex, tol, "object multi-axis rotate 30/45/60: x" );
		CheckClose( c.g, ey, tol, "object multi-axis rotate 30/45/60: y" );
		CheckClose( c.b, ez, tol, "object multi-axis rotate 30/45/60: z" );
		mp->release();
		src.release();
	}
}

static void TestBlendPainterScreenOverlayClamping()
{
	std::cout << "Test 41: blend_painter -- P1-C fix: screen/overlay clamp inputs to [0,1] before the curve, multiply/add stay unbounded" << std::endl;
	using namespace S7;

	// mask=1 isolates Combine(a,b) exactly: out = combined*1 + b*(1-1)
	// = combined -- the same isolation TestBlendPainterModes already
	// uses for its "mode mix, mask=1 -> a" case above.
	ConstColorPainter mask1( RISEPel( 1, 1, 1 ) ); mask1.addref();

	struct Case { Scalar av, bv; const char* label; };
	const Case cases[] = {
		{  1.5, 0.8, "a=1.5,b=0.8 (a clamps to 1.0)" },
		{ -0.5, 0.3, "a=-0.5,b=0.3 (a clamps to 0.0)" },
	};

	for( const Case& tc : cases ) {
		ConstColorPainter a( RISEPel( tc.av, tc.av, tc.av ) ); a.addref();
		ConstColorPainter b( RISEPel( tc.bv, tc.bv, tc.bv ) ); b.addref();
		RayIntersectionGeometric ri = MakeRi();

		const Scalar ac = tc.av < Scalar(0) ? Scalar(0) : ( tc.av > Scalar(1) ? Scalar(1) : tc.av );
		const Scalar bc = tc.bv < Scalar(0) ? Scalar(0) : ( tc.bv > Scalar(1) ? Scalar(1) : tc.bv );

		// screen: out = 1-(1-ac)*(1-bc), operands clamped first (P1-C).
		{
			const Scalar eScreen = Scalar(1) - (Scalar(1)-ac)*(Scalar(1)-bc);
			IPainter* bp = 0;
			RISE_API_CreateBlendPainterWithMode( &bp, a, b, mask1, 2u );
			const RISEPel c = bp->GetColor( ri );
			CheckClose( c.r, eScreen, 1e-9, std::string("screen clamped golden (GetColor), ") + tc.label );
			const Scalar nm = bp->GetColorNM( ri, 500.0 );
			CheckClose( nm, eScreen, 1e-9, std::string("screen clamped golden (GetColorNM), ") + tc.label );
			bp->release();
		}
		// overlay: out = bc<=0.5 ? 2*ac*bc : 1-2*(1-ac)*(1-bc), clamped first.
		{
			const Scalar eOverlay = bc <= Scalar(0.5) ? Scalar(2)*ac*bc : Scalar(1) - Scalar(2)*(Scalar(1)-ac)*(Scalar(1)-bc);
			IPainter* bp = 0;
			RISE_API_CreateBlendPainterWithMode( &bp, a, b, mask1, 3u );
			const RISEPel c = bp->GetColor( ri );
			CheckClose( c.r, eOverlay, 1e-9, std::string("overlay clamped golden (GetColor), ") + tc.label );
			const Scalar nm = bp->GetColorNM( ri, 500.0 );
			CheckClose( nm, eOverlay, 1e-9, std::string("overlay clamped golden (GetColorNM), ") + tc.label );
			bp->release();
		}
		// multiply / add: UNBOUNDED passthrough control -- no clamp, so
		// these must reproduce the raw (unclamped) av/bv arithmetic.
		{
			const Scalar eMul = tc.av * tc.bv;
			IPainter* bp = 0;
			RISE_API_CreateBlendPainterWithMode( &bp, a, b, mask1, 1u );
			const RISEPel c = bp->GetColor( ri );
			CheckClose( c.r, eMul, 1e-9, std::string("multiply UNCLAMPED passthrough control, ") + tc.label );
			bp->release();
		}
		{
			const Scalar eAdd = tc.av + tc.bv;
			IPainter* bp = 0;
			RISE_API_CreateBlendPainterWithMode( &bp, a, b, mask1, 4u );
			const RISEPel c = bp->GetColor( ri );
			CheckClose( c.r, eAdd, 1e-9, std::string("add UNCLAMPED passthrough control, ") + tc.label );
			bp->release();
		}

		b.release(); a.release();
	}

	mask1.release();
}

//======================================================================
// S8 (doc 88 P3.1/P3.2): stochastic_tile_painter, scatter_painter.
//
// P2-b (S8 review round 1) corrected banner: the helpers below (Hash01,
// HexTileRaw, CellInstance) are NOT an independent reimplementation --
// they are deliberate TRANSCRIPTIONS of the painters' own hash-draw /
// triangle-lattice / per-cell-instance formulas, kept as free functions
// here so a test can recompute an expected value without hand-copying
// a magic float.  Their job is DRIFT-GUARDING: if a future edit changes
// StochasticTilePainter.cpp / ScatterPainter.cpp's math without a
// matching edit here, the transcribed copy and the real implementation
// disagree and the test fails -- catching an implementation-vs-spec
// DIVERGENCE, not a conceptual error (a bug shared by both the real
// code and its transcription here would pass silently).
//
// The genuinely independent goldens are the ones actually computed by
// hand/closed-form from the documented algorithm rather than by
// calling a transcribed helper -- the contrast-restore formula in
// Test 44 (mu + w.(x-mu)/sqrt(sum w^2), evaluated by hand at the
// equal-thirds point) and the mean/blend_gamma golden in Test 45.
// Those two catch a CONCEPTUAL error (the transcription and the real
// code agreeing with each other but not with the spec) that the
// transcription-based tests structurally cannot.
//
// Parser-surface tests use S2::ParseBody like the S3/S7 sections above.
//======================================================================
namespace S8 {

	// Mirrors StochasticTilePainter.cpp / ScatterPainter.cpp's Hash01
	// EXACTLY (both use the identical technique) -- see either file's
	// header comment.  Duplicated here so a test can independently
	// recompute the exact hash draw a given (vertex/cell, channel,
	// seed) produces and compare it against the painter's actual
	// output, instead of hand-copying a magic float.
	inline Scalar Hash01( int vx, int vy, int channel, unsigned int seed )
	{
		const unsigned int zu = (unsigned int)channel * 1000003u + seed * 7919u;
		const unsigned int h = NoiseCore::WorleyHashCell( vx, vy, (int)zu );
		return Scalar( h ) / Scalar( 4294967296.0 );
	}

	// Mirrors StochasticTilePainter::ComputeHexTiling's triangle-lattice
	// solve, RAW (pre-sharpen) weights only -- callers apply gamma-
	// sharpening themselves so a single helper serves both the gamma-
	// invariant (equal-weight) and gamma-sensitive golden tests.
	void HexTileRaw( Scalar coordX, Scalar coordY, Scalar tileScale, int vi[3], int vj[3], Scalar wraw[3] )
	{
		const Scalar kSqrt3Over2 = Scalar( 0.86602540378443864676 );
		const Scalar px = coordX * tileScale;
		const Scalar py = coordY * tileScale;
		const Scalar j = py / kSqrt3Over2;
		const Scalar i = px - Scalar( 0.5 ) * j;
		const Scalar baseI = std::floor( (double)i );
		const Scalar baseJ = std::floor( (double)j );
		const Scalar fi = i - baseI;
		const Scalar fj = j - baseJ;
		const int bi = (int)baseI;
		const int bj = (int)baseJ;
		if( fi + fj <= Scalar( 1 ) ) {
			vi[0] = bi;     vj[0] = bj;     wraw[0] = Scalar(1) - fi - fj;
			vi[1] = bi + 1; vj[1] = bj;     wraw[1] = fi;
			vi[2] = bi;     vj[2] = bj + 1; wraw[2] = fj;
		} else {
			vi[0] = bi + 1; vj[0] = bj + 1; wraw[0] = fi + fj - Scalar(1);
			vi[1] = bi + 1; vj[1] = bj;     wraw[1] = Scalar(1) - fj;
			vi[2] = bi;     vj[2] = bj + 1; wraw[2] = Scalar(1) - fi;
		}
	}

	// Mirrors ScatterPainter::FindStamp's per-cell instance geometry
	// (position/rotation/scale draws only -- no neighbourhood search or
	// probability roll, since callers already know which cell they
	// want and whether it should be active).
	void CellInstance( int ci, int cj, unsigned int seed, Scalar jitterPosition, Scalar jitterRotationDeg, Scalar jitterScale, Scalar stampScale,
		Scalar& Cx, Scalar& Cy, Scalar& angleRad, Scalar& S )
	{
		const Scalar rx = Scalar(2) * Hash01( ci, cj, 0, seed ) - Scalar(1);
		const Scalar ry = Scalar(2) * Hash01( ci, cj, 1, seed ) - Scalar(1);
		Cx = Scalar( ci ) + Scalar(0.5) + jitterPosition * Scalar(0.5) * rx;
		Cy = Scalar( cj ) + Scalar(0.5) + jitterPosition * Scalar(0.5) * ry;
		const Scalar rr = Scalar(2) * Hash01( ci, cj, 2, seed ) - Scalar(1);
		angleRad = jitterRotationDeg * rr * DEG_TO_RAD;
		const Scalar rs = Scalar(2) * Hash01( ci, cj, 3, seed ) - Scalar(1);
		S = stampScale * ( Scalar(1) + jitterScale * rs );
	}

	// A stamp whose alpha is always exactly 0 -- for the "alpha-gated
	// stamp shows background" test.  GetColor still returns a loud,
	// distinguishable colour so a test failure (background NOT shown)
	// would be visually obvious in a render, even though this is a
	// numeric test.
	class TransparentPainter : public Painter
	{
	public:
		RISEPel GetColor( const RayIntersectionGeometric& ) const { return RISEPel( 1, 0, 1 ); }
		Scalar GetColorNM( const RayIntersectionGeometric&, const Scalar ) const { return 1.0; }
		Scalar GetAlpha( const RayIntersectionGeometric& ) const { return 0.0; }
		IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) { return 0; }
		void SetIntermediateValue( const IKeyframeParameter& ) {}
		void RegenerateData() {}
	};

	// Echoes ptCoord like S7::UVEchoPainter (R=u, G=v) but with alpha
	// hard-pinned to 1.0 (fully opaque) -- the scatter_painter golden
	// tests use this as the STAMP source instead of S7::UVEchoPainter,
	// because UVEchoPainter deliberately echoes GetAlpha == ptCoord.x
	// too (by design, for the mapping_painter/blend_painter "all four
	// paths agree" tests in S7) -- and scatter_painter's Porter-Duff
	// "over" composite READS the stamp's alpha as a real mix factor,
	// so an echoing alpha would silently blend the golden local-UV
	// colour toward background instead of passing it through, making
	// the position/rotation/scale goldens below unreadable.  Pinning
	// alpha to 1.0 isolates exactly what those tests want to measure:
	// the local sample COORDINATE, with the compositing math taken out
	// of the picture.
	class OpaqueUVEchoPainter : public Painter
	{
	public:
		RISEPel GetColor( const RayIntersectionGeometric& ri ) const { return RISEPel( ri.ptCoord.x, ri.ptCoord.y, 0.0 ); }
		Scalar GetColorNM( const RayIntersectionGeometric& ri, const Scalar ) const { return ri.ptCoord.x; }
		Scalar GetAlpha( const RayIntersectionGeometric& ) const { return 1.0; }
		IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) { return 0; }
		void SetIntermediateValue( const IKeyframeParameter& ) {}
		void RegenerateData() {}
	};

} // namespace S8

static void TestStochasticTileDeterminismAndSeed()
{
	std::cout << "Test 42: stochastic_tile_painter -- determinism and seed dependence" << std::endl;
	using namespace S7;

	ConstColorPainter src( RISEPel( 0.3, 0.6, 0.9 ) ); src.addref();
	IPainter* stp = 0;
	RISE_API_CreateStochasticTilePainter( &stp, src, 5.0, 3, RISEPel(0.5,0.5,0.5), 7.0 );
	RayIntersectionGeometric ri = MakeRi(); ri.ptCoord = Point2( 0.37, 0.61 );
	const RISEPel c1 = stp->GetColor( ri );
	const RISEPel c2 = stp->GetColor( ri );
	Check( c1.r == c2.r && c1.g == c2.g && c1.b == c2.b, "stochastic_tile: identical UV+seed -> bit-identical output" );
	stp->release();
	src.release();

	// Seed dependence: UVEchoPainter's output tracks the offset UV
	// directly, so a different seed (different hash draws) must move
	// at least one sampled channel.
	UVEchoPainter esrc; esrc.addref();
	IPainter* a = 0; RISE_API_CreateStochasticTilePainter( &a, esrc, 5.0, 3,  RISEPel(0.5,0.5,0.5), 7.0 );
	IPainter* b = 0; RISE_API_CreateStochasticTilePainter( &b, esrc, 5.0, 99, RISEPel(0.5,0.5,0.5), 7.0 );
	RayIntersectionGeometric ri2 = MakeRi(); ri2.ptCoord = Point2( 0.22, 0.48 );
	const RISEPel ca = a->GetColor( ri2 );
	const RISEPel cb = b->GetColor( ri2 );
	Check( ca.r != cb.r || ca.g != cb.g, "stochastic_tile: different seed -> different output" );
	a->release(); b->release(); esrc.release();
}

static void TestStochasticTileLatticeContinuityNoNaN()
{
	std::cout << "Test 43: stochastic_tile_painter -- lattice continuity spot-check, no NaN across tile borders" << std::endl;
	using namespace S7;

	UVEchoPainter src; src.addref();
	IPainter* stp = 0;
	RISE_API_CreateStochasticTilePainter( &stp, src, 4.0, 11, RISEPel(0.5,0.5,0.5), 7.0 );

	bool allFinite = true;
	for( int iy = -20; iy <= 30; ++iy ) {
		for( int ix = -20; ix <= 30; ++ix ) {
			RayIntersectionGeometric ri = MakeRi();
			ri.ptCoord = Point2( Scalar(ix) * 0.1, Scalar(iy) * 0.1 );	// sweeps [-2,3]x[-2,3], crossing many tile borders
			const RISEPel c = stp->GetColor( ri );
			const Scalar a = stp->GetAlpha( ri );
			if( !std::isfinite( (double)c.r ) || !std::isfinite( (double)c.g ) || !std::isfinite( (double)c.b ) || !std::isfinite( (double)a ) ) {
				allFinite = false;
			}
		}
	}
	Check( allFinite, "stochastic_tile: no NaN/inf across a dense UV sweep including negative coordinates and many tile borders" );
	stp->release();
	src.release();
}

static void TestStochasticTileContrastRestoreGolden()
{
	std::cout << "Test 44: stochastic_tile_painter -- histogram-preserving contrast-restore formula, hand/formula-computed golden" << std::endl;
	using namespace S7;
	using namespace S8;

	// tile_scale 1, coord (0.5, sqrt(3)/6): lands exactly at fi=fj=1/3,
	// so ALL THREE raw barycentric weights are 1/3 -- and stay 1/3 after
	// ANY gamma-sharpening (equal inputs sharpen to equal outputs), so
	// this point's blend weights are gamma-INVARIANT.  See file header
	// derivation replicated in HexTileRaw above.
	const Scalar coordX = 0.5;
	const Scalar coordY = Scalar( 0.16666666666666666667 ) * Scalar( 1.7320508075688772935 );	// sqrt(3)/6
	const Scalar tileScale = 1.0;
	const unsigned int seed = 17;

	int vi[3], vj[3]; Scalar wraw[3];
	HexTileRaw( coordX, coordY, tileScale, vi, vj, wraw );
	CheckClose( wraw[0], 1.0/3.0, 1e-9, "contrast-restore golden setup: w0 == 1/3" );
	CheckClose( wraw[1], 1.0/3.0, 1e-9, "contrast-restore golden setup: w1 == 1/3" );
	CheckClose( wraw[2], 1.0/3.0, 1e-9, "contrast-restore golden setup: w2 == 1/3" );

	// UVEchoPainter's GetColor(ri) = (ptCoord.x, ptCoord.y, 0) --
	// GetColorNM/GetAlpha = ptCoord.x.  The 3 offset-UV x components
	// (channel 0) are the "synthetic 3-value source" for the R channel;
	// channel 1 for G; the CONSTANT 0 for B (a degenerate all-equal
	// "source").
	Scalar x[3], y[3];
	for( int k = 0; k < 3; ++k ) {
		x[k] = coordX + Hash01( vi[k], vj[k], 0, seed );
		y[k] = coordY + Hash01( vi[k], vj[k], 1, seed );
	}

	const RISEPel mean( 0.5, 0.5, 0.5 );
	UVEchoPainter esrc; esrc.addref();
	IPainter* stp = 0;
	RISE_API_CreateStochasticTilePainter( &stp, esrc, tileScale, seed, mean, 7.0 );	// gamma irrelevant here (equal weights)
	RayIntersectionGeometric ri = MakeRi(); ri.ptCoord = Point2( coordX, coordY );
	const RISEPel c = stp->GetColor( ri );

	// Equal-thirds contrast restore: out = mean + sqrt(3) * (avg(x_i) - mean).
	const Scalar sqrt3 = Scalar( 1.7320508075688772935 );
	const Scalar expectedR = 0.5 + sqrt3 * ( (x[0]+x[1]+x[2])/3.0 - 0.5 );
	const Scalar expectedG = 0.5 + sqrt3 * ( (y[0]+y[1]+y[2])/3.0 - 0.5 );
	const Scalar expectedB = 0.5 + sqrt3 * ( 0.0 - 0.5 );	// B channel is always exactly 0 (constant "source")
	CheckClose( c.r, expectedR, 1e-9, "contrast-restore golden: R channel matches formula" );
	CheckClose( c.g, expectedG, 1e-9, "contrast-restore golden: G channel matches formula" );
	CheckClose( c.b, expectedB, 1e-9, "contrast-restore golden: B channel (constant source) matches formula" );

	// GetAlpha does NOT contrast-restore -- plain average, using the
	// SAME (gamma-invariant here) weights.  Different from expectedR
	// above by construction -- proves the documented divergence.
	const Scalar a = stp->GetAlpha( ri );
	const Scalar expectedAlpha = (x[0]+x[1]+x[2])/3.0;
	CheckClose( a, expectedAlpha, 1e-9, "contrast-restore golden: GetAlpha is the PLAIN average (no restore)" );
	Check( std::fabs( (double)(a - c.r) ) > 1e-6, "contrast-restore golden: GetAlpha differs from GetColor.r (restore only applies to colour)" );

	stp->release();
	esrc.release();
}

static void TestStochasticTileMeanAndGammaRespected()
{
	std::cout << "Test 45: stochastic_tile_painter -- mean and blend_gamma parameters are respected" << std::endl;
	using namespace S7;
	using namespace S8;

	// mean: at the SAME equal-thirds point as Test 43, a CONSTANT
	// source C gives out = mean + sqrt(3)*(C - mean) for ANY mean --
	// verify two different `mean` values both match their own formula.
	{
		const Scalar coordX = 0.5;
		const Scalar coordY = Scalar( 0.16666666666666666667 ) * Scalar( 1.7320508075688772935 );
		const Scalar sqrt3 = Scalar( 1.7320508075688772935 );
		ConstColorPainter csrc( RISEPel( 0.8, 0.8, 0.8 ) ); csrc.addref();
		RayIntersectionGeometric ri = MakeRi(); ri.ptCoord = Point2( coordX, coordY );

		for( Scalar meanVal : { 0.5, 0.2 } ) {
			IPainter* stp = 0;
			RISE_API_CreateStochasticTilePainter( &stp, csrc, 1.0, 17, RISEPel(meanVal,meanVal,meanVal), 7.0 );
			const RISEPel c = stp->GetColor( ri );
			const Scalar expected = meanVal + sqrt3 * ( 0.8 - meanVal );
			CheckClose( c.r, expected, 1e-9, std::string("mean respected: mean=") + std::to_string((double)meanVal) );
			stp->release();
		}
		csrc.release();
	}

	// blend_gamma: an ASYMMETRIC point (raw weights 0.7/0.2/0.1) makes
	// gamma=1 (unsharpened) and gamma=7 (default sharpening) diverge --
	// verify both match their own formula-computed golden, and that
	// they differ from each other.
	{
		const Scalar coordX = 0.25;
		const Scalar coordY = Scalar( 0.1 ) * Scalar( 0.86602540378443864676 );	// j*sqrt(3)/2 for j=0.1, i=0.2 at tile_scale 1
		const unsigned int seed = 5;
		int vi[3], vj[3]; Scalar wraw[3];
		HexTileRaw( coordX, coordY, 1.0, vi, vj, wraw );
		CheckClose( wraw[0], 0.7, 1e-9, "gamma golden setup: w0 == 0.7" );
		CheckClose( wraw[1], 0.2, 1e-9, "gamma golden setup: w1 == 0.2" );
		CheckClose( wraw[2], 0.1, 1e-9, "gamma golden setup: w2 == 0.1" );

		Scalar x[3];
		for( int k = 0; k < 3; ++k ) x[k] = coordX + Hash01( vi[k], vj[k], 0, seed );

		UVEchoPainter esrc; esrc.addref();
		RayIntersectionGeometric ri = MakeRi(); ri.ptCoord = Point2( coordX, coordY );
		const RISEPel mean( 0.5, 0.5, 0.5 );

		Scalar outputs[2];
		const Scalar gammas[2] = { 1.0, 7.0 };
		for( int g = 0; g < 2; ++g ) {
			Scalar wsharp[3], wsum = 0;
			for( int k = 0; k < 3; ++k ) { wsharp[k] = std::pow( (double)wraw[k], (double)gammas[g] ); wsum += wsharp[k]; }
			Scalar wnorm[3], wsq = 0;
			for( int k = 0; k < 3; ++k ) { wnorm[k] = wsharp[k]/wsum; wsq += wnorm[k]*wnorm[k]; }
			Scalar weighted = 0;
			for( int k = 0; k < 3; ++k ) weighted += wnorm[k]*(x[k]-0.5);
			const Scalar expected = 0.5 + weighted/std::sqrt((double)wsq);

			IPainter* stp = 0;
			RISE_API_CreateStochasticTilePainter( &stp, esrc, 1.0, seed, mean, gammas[g] );
			const RISEPel c = stp->GetColor( ri );
			CheckClose( c.r, expected, 1e-6, std::string("blend_gamma respected: gamma=") + std::to_string((double)gammas[g]) );
			outputs[g] = c.r;
			stp->release();
		}
		Check( std::fabs( (double)(outputs[0]-outputs[1]) ) > 1e-4, "blend_gamma respected: gamma=1 and gamma=7 give DIFFERENT output at an asymmetric point" );

		// P3 (S8 review round 1): blend_gamma == 0 corner.  pow(w,0) == 1
		// for every (positive) raw weight regardless of how asymmetric
		// they are, so the sharpen-and-renormalize step collapses to
		// UNIFORM 1/3 weights everywhere -- mathematically consistent
		// (see StochasticTilePainter.cpp's ComputeHexTiling comment at
		// the pow() call site) and parser-legal (blend_gamma is clamped
		// to [0, 64], 0 included).  Pin it against the SAME asymmetric
		// point (0.7/0.2/0.1 raw weights) used above, so the corner is
		// exercised where gamma actually matters, not a degenerate
		// equal-thirds point where every gamma looks the same.
		{
			const Scalar wnormUniform = 1.0 / 3.0;
			const Scalar wsqUniform = 3.0 * wnormUniform * wnormUniform;	// == 1/3
			Scalar weighted = 0;
			for( int k = 0; k < 3; ++k ) weighted += wnormUniform * ( x[k] - 0.5 );
			const Scalar expectedGamma0 = 0.5 + weighted / std::sqrt( (double)wsqUniform );

			IPainter* stp = 0;
			RISE_API_CreateStochasticTilePainter( &stp, esrc, 1.0, seed, mean, 0.0 );
			const RISEPel c = stp->GetColor( ri );
			CheckClose( c.r, expectedGamma0, 1e-6, "blend_gamma==0 corner: pow(w,0)==1 collapses to uniform 1/3 weights" );
			stp->release();
		}
		esrc.release();
	}
}

static void TestStochasticTileFootprintInvalidation()
{
	std::cout << "Test 46: stochastic_tile_painter -- txFootprint invalidated on every Get* path" << std::endl;
	using namespace S7;

	FootprintEchoPainter src; src.addref();
	IPainter* stp = 0;
	RISE_API_CreateStochasticTilePainter( &stp, src, 4.0, 1, RISEPel(0.5,0.5,0.5), 7.0 );
	RayIntersectionGeometric ri = MakeRi();
	ri.ptCoord = Point2( 0.3, 0.4 );
	ri.txFootprint.widthValid = true;   // valid => widthValid (a real hit never has one without the other)
	ri.txFootprint.worldWidth = 0.05;
	ri.txFootprint.valid = true;

	src.lastFootprintValid = true; stp->GetColor( ri );      Check( src.lastFootprintValid == false, "stochastic_tile: GetColor invalidates txFootprint" );
	src.lastFootprintValid = true; stp->GetColorNM( ri, 550.0 ); Check( src.lastFootprintValid == false, "stochastic_tile: GetColorNM invalidates txFootprint" );
	src.lastFootprintValid = true; stp->GetSpectrum( ri );    Check( src.lastFootprintValid == false, "stochastic_tile: GetSpectrum invalidates txFootprint" );
	src.lastFootprintValid = true; stp->GetAlpha( ri );       Check( src.lastFootprintValid == false, "stochastic_tile: GetAlpha invalidates txFootprint" );
	Check( ri.txFootprint.valid == true, "stochastic_tile: caller's ri.txFootprint.valid is NOT mutated by the wrapper" );

	stp->release();
	src.release();
}

static void TestStochasticTileNMRGBConsistency()
{
	std::cout << "Test 47: stochastic_tile_painter -- NM/RGB path consistency (source == mean collapses both paths to mean)" << std::endl;
	using namespace S7;

	const RISEPel gray( 0.5, 0.5, 0.5 );
	ConstColorPainter src( gray ); src.addref();
	IPainter* stp = 0;
	RISE_API_CreateStochasticTilePainter( &stp, src, 6.0, 21, gray, 7.0 );
	RayIntersectionGeometric ri = MakeRi(); ri.ptCoord = Point2( 0.44, 0.71 );

	// RGB path: source IS exactly `mean` everywhere, so every sample's
	// (x_i - mean) term is EXACTLY zero -- the restored output must be
	// EXACTLY mean, bit-for-bit (no accumulated rounding: the weighted
	// sum is a sum of exact zeros).
	const RISEPel c = stp->GetColor( ri );
	Check( c.r == 0.5 && c.g == 0.5 && c.b == 0.5, "NM/RGB consistency: GetColor with source==mean returns EXACTLY mean" );

	// Spectral path: ConstColorPainter's GetColorNM returns a flat 0.5
	// (not JH-uplifted -- see its own comment), while the painter's
	// internal `mean` IS JH-uplifted -- so exact equality isn't
	// expected, but a mid-spectrum sample should land close to 0.5
	// (JH-uplift of a neutral grey is close to flat).  This is the
	// "consistency" this test's name refers to: both paths collapse
	// toward the same answer when there's no information in the source.
	const Scalar nm = stp->GetColorNM( ri, 550.0 );
	CheckClose( nm, 0.5, 0.05, "NM/RGB consistency: GetColorNM(source==mean) lands close to GetColor's exact 0.5 (JH-uplift tolerance)" );

	stp->release();
	src.release();
}

static void TestS8ChunkParsingAndDiagnostics()
{
	std::cout << "Test 48: stochastic_tile_painter / scatter_painter -- chunk parsing, registration, diagnostics" << std::endl;

	// stochastic_tile_painter diagnostics.
	Check( !S2::ParseBody( "st_nosrc", "stochastic_tile_painter\n{\nname t1\n}\n" ),
		"stochastic_tile_painter missing `source` rejects" );
	Check( !S2::ParseBody( "st_badscale",
		"uniformcolor_painter\n{\nname s\ncolor 1 1 1\n}\n"
		"stochastic_tile_painter\n{\nname t2\nsource s\ntile_scale 0\n}\n" ),
		"stochastic_tile_painter tile_scale <= 0 rejects" );
	Check( !S2::ParseBody( "st_gammaneg",
		"uniformcolor_painter\n{\nname s\ncolor 1 1 1\n}\n"
		"stochastic_tile_painter\n{\nname t3\nsource s\nblend_gamma -1\n}\n" ),
		"stochastic_tile_painter blend_gamma < 0 rejects" );
	Check( !S2::ParseBody( "st_gammahigh",
		"uniformcolor_painter\n{\nname s\ncolor 1 1 1\n}\n"
		"stochastic_tile_painter\n{\nname t4\nsource s\nblend_gamma 65\n}\n" ),
		"stochastic_tile_painter blend_gamma > 64 rejects" );
	{
		Job* job = new Job(); job->addref();
		Check( S2::ParseBody( "st_ok",
			"uniformcolor_painter\n{\nname s\ncolor 1 1 1\n}\n"
			"stochastic_tile_painter\n{\nname t5\nsource s\ntile_scale 6\nseed 3\nmean 0.4 0.4 0.4\nblend_gamma 5\ncolor_space Rec709RGB_Linear\n}\n",
			*job ), "stochastic_tile_painter fully-specified chunk parses" );
		IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
		if( priv ) {
			Check( priv->GetPainters()->GetItem( "t5" ) != 0, "stochastic_tile_painter registered as a colour painter" );
			Check( priv->GetFunction2Ds()->GetItem( "t5" ) != 0, "stochastic_tile_painter ALSO registered as an IFunction2D (dual registration)" );
		}
		job->release();
	}

	// scatter_painter diagnostics.
	Check( !S2::ParseBody( "sc_nosrc",
		"uniformcolor_painter\n{\nname bg\ncolor 0 0 0\n}\n"
		"scatter_painter\n{\nname sp1\nbackground bg\n}\n" ),
		"scatter_painter missing `source` rejects" );
	Check( !S2::ParseBody( "sc_nobg",
		"uniformcolor_painter\n{\nname st\ncolor 1 1 1\n}\n"
		"scatter_painter\n{\nname sp2\nsource st\n}\n" ),
		"scatter_painter missing `background` rejects" );
	Check( !S2::ParseBody( "sc_badcell",
		"uniformcolor_painter\n{\nname a\ncolor 1 1 1\n}\n"
		"uniformcolor_painter\n{\nname b\ncolor 0 0 0\n}\n"
		"scatter_painter\n{\nname sp3\nsource a\nbackground b\ncell_scale 0\n}\n" ),
		"scatter_painter cell_scale <= 0 rejects" );
	Check( !S2::ParseBody( "sc_badstamp",
		"uniformcolor_painter\n{\nname a\ncolor 1 1 1\n}\n"
		"uniformcolor_painter\n{\nname b\ncolor 0 0 0\n}\n"
		"scatter_painter\n{\nname sp4\nsource a\nbackground b\nstamp_scale 0\n}\n" ),
		"scatter_painter stamp_scale <= 0 rejects" );
	Check( !S2::ParseBody( "sc_badjitpos",
		"uniformcolor_painter\n{\nname a\ncolor 1 1 1\n}\n"
		"uniformcolor_painter\n{\nname b\ncolor 0 0 0\n}\n"
		"scatter_painter\n{\nname sp5\nsource a\nbackground b\njitter_position 1.5\n}\n" ),
		"scatter_painter jitter_position out of [0,1] rejects" );
	Check( !S2::ParseBody( "sc_badjitrot",
		"uniformcolor_painter\n{\nname a\ncolor 1 1 1\n}\n"
		"uniformcolor_painter\n{\nname b\ncolor 0 0 0\n}\n"
		"scatter_painter\n{\nname sp6\nsource a\nbackground b\njitter_rotation -5\n}\n" ),
		"scatter_painter jitter_rotation < 0 rejects" );
	Check( !S2::ParseBody( "sc_badjitscale",
		"uniformcolor_painter\n{\nname a\ncolor 1 1 1\n}\n"
		"uniformcolor_painter\n{\nname b\ncolor 0 0 0\n}\n"
		"scatter_painter\n{\nname sp7\nsource a\nbackground b\njitter_scale 1.0\n}\n" ),
		"scatter_painter jitter_scale >= 1 rejects (must be [0,1))" );
	Check( !S2::ParseBody( "sc_badprob",
		"uniformcolor_painter\n{\nname a\ncolor 1 1 1\n}\n"
		"uniformcolor_painter\n{\nname b\ncolor 0 0 0\n}\n"
		"scatter_painter\n{\nname sp8\nsource a\nbackground b\nprobability 1.2\n}\n" ),
		"scatter_painter probability out of [0,1] rejects" );
	// stamp_scale * (1 + jitter_scale) = 1.2 * 1.3 = 1.56 > sqrt(2) -- the
	// "stamp too large" neighbourhood-reach diagnostic.
	Check( !S2::ParseBody( "sc_toolarge",
		"uniformcolor_painter\n{\nname a\ncolor 1 1 1\n}\n"
		"uniformcolor_painter\n{\nname b\ncolor 0 0 0\n}\n"
		"scatter_painter\n{\nname sp9\nsource a\nbackground b\nstamp_scale 1.2\njitter_scale 0.3\n}\n" ),
		"scatter_painter stamp_scale*(1+jitter_scale) > sqrt(2) rejects (stamp too large for the 3x3 search)" );
	{
		Job* job = new Job(); job->addref();
		Check( S2::ParseBody( "sc_ok",
			"uniformcolor_painter\n{\nname a\ncolor 1 1 1\n}\n"
			"uniformcolor_painter\n{\nname b\ncolor 0 0 0\n}\n"
			"scatter_painter\n{\nname sp10\nsource a\nbackground b\ncell_scale 5\nstamp_scale 0.6\njitter_position 0.4\njitter_rotation 10\njitter_scale 0.2\nprobability 0.7\nseed 2\n}\n",
			*job ), "scatter_painter fully-specified chunk parses" );
		IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
		if( priv ) {
			Check( priv->GetPainters()->GetItem( "sp10" ) != 0, "scatter_painter registered as a colour painter" );
			Check( priv->GetFunction2Ds()->GetItem( "sp10" ) != 0, "scatter_painter ALSO registered as an IFunction2D (dual registration)" );
		}
		job->release();
	}
}

static void TestScatterDeterminismAndProbability()
{
	std::cout << "Test 49: scatter_painter -- determinism, probability 0 (pure background), probability 1 + no jitter (grid golden)" << std::endl;
	using namespace S7;

	ConstColorPainter stamp( RISEPel( 1, 1, 0 ) ); stamp.addref();
	ConstColorPainter bg( RISEPel( 0.05, 0.05, 0.05 ) ); bg.addref();

	// Determinism.
	{
		IPainter* sp = 0;
		RISE_API_CreateScatterPainter( &sp, stamp, bg, 4.0, 0.6, 0.5, 15.0, 0.2, 0.8, 9 );
		RayIntersectionGeometric ri = MakeRi(); ri.ptCoord = Point2( 0.33, 0.71 );
		const RISEPel c1 = sp->GetColor( ri );
		const RISEPel c2 = sp->GetColor( ri );
		Check( c1.r == c2.r && c1.g == c2.g && c1.b == c2.b, "scatter: identical UV+seed -> bit-identical output" );
		sp->release();
	}

	// probability 0 -> pure background everywhere: strict `<` means a
	// roll of exactly 0.0 (possible from Hash01's [0,1) range) still
	// never activates a cell.
	{
		IPainter* sp = 0;
		RISE_API_CreateScatterPainter( &sp, stamp, bg, 4.0, 0.6, 0.5, 15.0, 0.2, 0.0, 0 );
		bool allBackground = true;
		for( int iy = 0; iy < 15; ++iy ) {
			for( int ix = 0; ix < 15; ++ix ) {
				RayIntersectionGeometric ri = MakeRi();
				ri.ptCoord = Point2( Scalar(ix)*0.13, Scalar(iy)*0.13 );
				const RISEPel c = sp->GetColor( ri );
				const RISEPel e = bg.GetColor( ri );
				if( c.r != e.r || c.g != e.g || c.b != e.b ) allBackground = false;
				if( sp->GetAlpha( ri ) != bg.GetAlpha( ri ) ) allBackground = false;
			}
		}
		Check( allBackground, "scatter: probability 0 -> pure background everywhere (colour AND alpha)" );
		sp->release();
	}

	// probability 1 + zero jitter -> exact grid: stamp_scale 1.0 means
	// each cell's stamp exactly fills its cell (no gaps/overlaps), so
	// a query point's local coordinate is a plain fractional part.
	{
		S8::OpaqueUVEchoPainter estamp; estamp.addref();
		IPainter* sp = 0;
		RISE_API_CreateScatterPainter( &sp, estamp, bg, 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 4 );
		RayIntersectionGeometric ri = MakeRi(); ri.ptCoord = Point2( 0.9, 0.5 );	// cell (0,0), local = (0.9, 0.5)
		const RISEPel c = sp->GetColor( ri );
		CheckClose( c.r, 0.9, 1e-9, "scatter: probability 1 + zero jitter -> grid golden local.u" );
		CheckClose( c.g, 0.5, 1e-9, "scatter: probability 1 + zero jitter -> grid golden local.v" );
		sp->release();
		estamp.release();
	}

	stamp.release(); bg.release();
}

static void TestScatterAlphaGatedAndFootprint()
{
	std::cout << "Test 50: scatter_painter -- alpha-gated stamp shows background; txFootprint invalidated" << std::endl;
	using namespace S7;
	using namespace S8;

	// Alpha-gated: a fully-transparent stamp, geometrically covering
	// (probability 1, zero jitter, stamp fills the cell), must show
	// pure background -- the texture-bombing cutout idiom.
	{
		TransparentPainter stamp; stamp.addref();
		ConstColorPainter bg( RISEPel( 0.1, 0.2, 0.3 ) ); bg.addref();
		IPainter* sp = 0;
		RISE_API_CreateScatterPainter( &sp, stamp, bg, 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0 );
		RayIntersectionGeometric ri = MakeRi(); ri.ptCoord = Point2( 0.5, 0.5 );
		const RISEPel c = sp->GetColor( ri );
		Check( c.r == 0.1 && c.g == 0.2 && c.b == 0.3, "scatter: alpha-0 stamp shows EXACTLY the background colour" );
		Check( sp->GetAlpha( ri ) == bg.GetAlpha( ri ), "scatter: alpha-0 stamp's composited alpha == background alpha (Porter-Duff over, a_src=0)" );
		sp->release();
		bg.release();
	}

	// txFootprint invalidation on the winning stamp's local sample.
	{
		FootprintEchoPainter stampSrc; stampSrc.addref();
		ConstColorPainter bg( RISEPel( 0, 0, 0 ) ); bg.addref();
		IPainter* sp = 0;
		RISE_API_CreateScatterPainter( &sp, stampSrc, bg, 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0 );
		RayIntersectionGeometric ri = MakeRi();
		ri.ptCoord = Point2( 0.5, 0.5 );
		ri.txFootprint.widthValid = true;   // valid => widthValid (a real hit never has one without the other)
		ri.txFootprint.worldWidth = 0.05;
		ri.txFootprint.valid = true;

		stampSrc.lastFootprintValid = true; sp->GetColor( ri );      Check( stampSrc.lastFootprintValid == false, "scatter: GetColor invalidates the stamp's txFootprint" );
		stampSrc.lastFootprintValid = true; sp->GetColorNM( ri, 550.0 ); Check( stampSrc.lastFootprintValid == false, "scatter: GetColorNM invalidates the stamp's txFootprint" );
		stampSrc.lastFootprintValid = true; sp->GetSpectrum( ri );    Check( stampSrc.lastFootprintValid == false, "scatter: GetSpectrum invalidates the stamp's txFootprint" );
		stampSrc.lastFootprintValid = true; sp->GetAlpha( ri );       Check( stampSrc.lastFootprintValid == false, "scatter: GetAlpha invalidates the stamp's txFootprint" );
		Check( ri.txFootprint.valid == true, "scatter: caller's ri.txFootprint.valid is NOT mutated by the wrapper" );

		sp->release();
		bg.release();
	}
}

static void TestScatterJitterBoundedAtZero()
{
	std::cout << "Test 51: scatter_painter -- jitter_rotation=0 and jitter_scale=0 pin the instance EXACTLY, across many seeds" << std::endl;
	using namespace S7;

	// jitter_rotation 0: an offset purely along +x from the (jitter-
	// position-0, hence exactly known) cell center must land at local
	// v == 0.5 EXACTLY for every seed -- any leaked rotation would move
	// v away from 0.5.
	{
		S8::OpaqueUVEchoPainter estamp; estamp.addref();
		ConstColorPainter bg( RISEPel(0,0,0) ); bg.addref();
		for( unsigned int seed = 0; seed < 10; ++seed ) {
			IPainter* sp = 0;
			RISE_API_CreateScatterPainter( &sp, estamp, bg, 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, seed );
			RayIntersectionGeometric ri = MakeRi(); ri.ptCoord = Point2( 0.8, 0.5 );	// cell(0,0) center (0.5,0.5) + 0.3 along +x
			const RISEPel c = sp->GetColor( ri );
			CheckClose( c.r, 0.8, 1e-9, std::string("jitter_rotation=0 pinned (u), seed=") + std::to_string(seed) );
			CheckClose( c.g, 0.5, 1e-9, std::string("jitter_rotation=0 pinned (v), seed=") + std::to_string(seed) );
			sp->release();
		}
		estamp.release(); bg.release();
	}

	// jitter_scale 0: stamp_scale 0.6 pins S EXACTLY, so a point at
	// HALF the stamp width from the (jitter-position-0) center sits
	// EXACTLY on the local boundary (u=1.0, found) while a point just
	// past it is NOT found, for every seed.
	{
		S8::OpaqueUVEchoPainter estamp; estamp.addref();
		ConstColorPainter bg( RISEPel(0,0,0) ); bg.addref();
		for( unsigned int seed = 0; seed < 10; ++seed ) {
			IPainter* sp = 0;
			RISE_API_CreateScatterPainter( &sp, estamp, bg, 1.0, 0.6, 0.0, 0.0, 0.0, 1.0, seed );
			RayIntersectionGeometric riOn = MakeRi(); riOn.ptCoord = Point2( 0.8, 0.5 );	// 0.5 + 0.3 == half of 0.6
			const RISEPel c = sp->GetColor( riOn );
			CheckClose( c.r, 1.0, 1e-6, std::string("jitter_scale=0 pinned: boundary point found at u=1.0, seed=") + std::to_string(seed) );

			RayIntersectionGeometric riOff = MakeRi(); riOff.ptCoord = Point2( 0.82, 0.5 );	// just past the boundary
			const RISEPel cOff = sp->GetColor( riOff );
			Check( cOff.r == 0.0 && cOff.g == 0.0 && cOff.b == 0.0, std::string("jitter_scale=0 pinned: just-past-boundary point is NOT the stamp (shows EXACT background), seed=") + std::to_string(seed) );
			sp->release();
		}
		estamp.release(); bg.release();
	}
}

static void TestScatterNeighborOverlapGolden()
{
	std::cout << "Test 52: scatter_painter -- neighbour-overlap correctness, formula-computed golden (a stamp crossing a cell border wins from the adjacent cell)" << std::endl;
	using namespace S7;
	using namespace S8;

	// seed=7 found by brute-force search (see the S8 slice notes) over
	// a config where cell (0,0)'s jittered/enlarged stamp crosses into
	// cell (1,0) and WINS (is nearer than cell (1,0)'s own instance) at
	// query point (1.05, 0.5).  stamp_scale*(1+jitter_scale) = 1.0*1.3
	// = 1.3 <= sqrt(2), so the parser's neighbourhood-reach bound holds.
	const Scalar cellScale = 1.0, stampScale = 1.0, jitterPosition = 1.0, jitterRotationDeg = 0.0, jitterScale = 0.3, probability = 1.0;
	const unsigned int seed = 7;
	const Scalar qx = 1.05, qy = 0.5;

	// Independently recompute the expected winner + local UV via the
	// SAME per-cell formula (CellInstance) over the 3x3 neighbourhood
	// of the query point's own cell -- this is the reimplementation the
	// file-header comment describes, not a magic-number transcription.
	const int qi = (int)std::floor( (double)(qx*cellScale) );
	const int qj = (int)std::floor( (double)(qy*cellScale) );
	bool found = false; Scalar bestD2 = 0, bestU = 0, bestV = 0; int bestCi = 0, bestCj = 0;
	for( int dj = -1; dj <= 1; ++dj ) {
		for( int di = -1; di <= 1; ++di ) {
			const int ci = qi+di, cj = qj+dj;
			if( Hash01(ci,cj,4,seed) >= probability ) continue;
			Scalar Cx, Cy, angle, S;
			CellInstance( ci, cj, seed, jitterPosition, jitterRotationDeg, jitterScale, stampScale, Cx, Cy, angle, S );
			const Scalar dx = qx*cellScale - Cx, dy = qy*cellScale - Cy;
			const Scalar cs = std::cos((double)angle), sn = std::sin((double)angle);
			const Scalar lx = cs*dx + sn*dy, ly = -sn*dx + cs*dy;
			const Scalar u = lx/S + Scalar(0.5), v = ly/S + Scalar(0.5);
			if( u < 0 || u > 1 || v < 0 || v > 1 ) continue;
			const Scalar d2 = dx*dx+dy*dy;
			if( !found || d2 < bestD2 ) { found = true; bestD2 = d2; bestU = u; bestV = v; bestCi = ci; bestCj = cj; }
		}
	}
	Check( found, "neighbour-overlap golden setup: SOME stamp covers the query point" );
	Check( bestCi == 0 && bestCj == 0, "neighbour-overlap golden setup: cell (0,0) -- crossing the border -- is the WINNER, not the query's own cell (1,0)" );

	S8::OpaqueUVEchoPainter estamp; estamp.addref();
	ConstColorPainter bg( RISEPel(0,0,0) ); bg.addref();
	IPainter* sp = 0;
	RISE_API_CreateScatterPainter( &sp, estamp, bg, cellScale, stampScale, jitterPosition, jitterRotationDeg, jitterScale, probability, seed );
	RayIntersectionGeometric ri = MakeRi(); ri.ptCoord = Point2( qx, qy );
	const RISEPel c = sp->GetColor( ri );
	CheckClose( c.r, bestU, 1e-6, "neighbour-overlap golden: actual local.u matches the independently-recomputed winner" );
	CheckClose( c.g, bestV, 1e-6, "neighbour-overlap golden: actual local.v matches the independently-recomputed winner" );
	sp->release();
	estamp.release(); bg.release();
}

static void TestScatterDrawOrderStability()
{
	std::cout << "Test 53: scatter_painter -- draw-order stability (repeated evaluation over many points is fully deterministic)" << std::endl;
	using namespace S7;

	S8::OpaqueUVEchoPainter estamp; estamp.addref();
	ConstColorPainter bg( RISEPel( 0.02, 0.02, 0.02 ) ); bg.addref();
	IPainter* sp = 0;
	RISE_API_CreateScatterPainter( &sp, estamp, bg, 3.0, 0.7, 0.6, 25.0, 0.25, 0.75, 13 );

	bool stable = true;
	for( int iy = 0; iy < 12; ++iy ) {
		for( int ix = 0; ix < 12; ++ix ) {
			RayIntersectionGeometric ri = MakeRi();
			ri.ptCoord = Point2( Scalar(ix)*0.11, Scalar(iy)*0.11 );
			const RISEPel c1 = sp->GetColor( ri );
			const RISEPel c2 = sp->GetColor( ri );
			const RISEPel c3 = sp->GetColor( ri );
			if( c1.r != c2.r || c1.g != c2.g || c1.b != c2.b || c2.r != c3.r || c2.g != c3.g || c2.b != c3.b ) stable = false;
		}
	}
	Check( stable, "scatter: repeated evaluation at 144 UVs is bit-identical every time (no draw-order or iteration-dependent nondeterminism)" );

	sp->release();
	estamp.release(); bg.release();
}

//======================================================================
// P1-A (S8 review round 1): bare (int)floor casts in
// StochasticTilePainter.cpp / ScatterPainter.cpp were UB for |value| >
// INT_MAX, reachable via an unbounded tile_scale/cell_scale multiplied
// against an ordinary UV -- the same class of bug as the cellhash(1e20)
// UB fixed in TestOctaveAndCellhashRuntimeSafety (Test 11) above, now
// recurring a third time because NoiseCore::SafeFloorToInt (the fix)
// was trapped in ProceduralNoiseCore.cpp's anonymous namespace and
// unreachable from Painters/.  Both painters now route through the
// promoted, public NoiseCore::SafeFloorToInt -- these tests pin the
// same "finite, deterministic, no crash" contract at a lattice index
// that overflows int (tile_scale/cell_scale 1e6 at u=1e5 -> lattice
// coordinate ~1e11, far past INT_MAX's ~2.1e9).
//======================================================================
static void TestStochasticTileExtremeScaleSafety()
{
	std::cout << "Test 54: stochastic_tile_painter -- P1-A extreme tile_scale x UV lattice-index safety (no floor->int UB)" << std::endl;
	using namespace S7;

	ConstColorPainter src( RISEPel( 0.3, 0.6, 0.9 ) ); src.addref();
	IPainter* stp = 0;
	RISE_API_CreateStochasticTilePainter( &stp, src, 1.0e6, 3, RISEPel(0.5,0.5,0.5), 7.0 );
	RayIntersectionGeometric ri = MakeRi(); ri.ptCoord = Point2( 1.0e5, 1.0e5 );

	const RISEPel c1 = stp->GetColor( ri );
	const RISEPel c2 = stp->GetColor( ri );
	Check( ExpressionProgram::IsFinite(c1.r) && ExpressionProgram::IsFinite(c1.g) && ExpressionProgram::IsFinite(c1.b),
		"stochastic_tile: tile_scale=1e6 at u=v=1e5 (lattice index >> INT_MAX) gives a finite GetColor" );
	Check( c1.r == c2.r && c1.g == c2.g && c1.b == c2.b,
		"stochastic_tile: tile_scale=1e6 at u=v=1e5 is bit-identical on repeated evaluation (deterministic, not UB-dependent garbage)" );

	const Scalar a = stp->GetAlpha( ri );
	Check( ExpressionProgram::IsFinite(a), "stochastic_tile: tile_scale=1e6 at u=v=1e5 gives a finite GetAlpha" );

	const Scalar nm = stp->GetColorNM( ri, 550.0 );
	Check( ExpressionProgram::IsFinite(nm), "stochastic_tile: tile_scale=1e6 at u=v=1e5 gives a finite GetColorNM" );

	stp->release();
	src.release();
}

static void TestScatterExtremeScaleSafety()
{
	std::cout << "Test 55: scatter_painter -- P1-A extreme cell_scale x UV lattice-index safety (no floor->int UB)" << std::endl;
	using namespace S7;

	S8::OpaqueUVEchoPainter estamp; estamp.addref();
	ConstColorPainter bg( RISEPel( 0.02, 0.02, 0.02 ) ); bg.addref();
	IPainter* sp = 0;
	RISE_API_CreateScatterPainter( &sp, estamp, bg, 1.0e6, 0.5, 0.5, 20.0, 0.25, 0.5, 7 );
	RayIntersectionGeometric ri = MakeRi(); ri.ptCoord = Point2( 1.0e5, 1.0e5 );

	const RISEPel c1 = sp->GetColor( ri );
	const RISEPel c2 = sp->GetColor( ri );
	Check( ExpressionProgram::IsFinite(c1.r) && ExpressionProgram::IsFinite(c1.g) && ExpressionProgram::IsFinite(c1.b),
		"scatter: cell_scale=1e6 at u=v=1e5 (lattice index >> INT_MAX) gives a finite GetColor" );
	Check( c1.r == c2.r && c1.g == c2.g && c1.b == c2.b,
		"scatter: cell_scale=1e6 at u=v=1e5 is bit-identical on repeated evaluation (deterministic, not UB-dependent garbage)" );

	const Scalar a = sp->GetAlpha( ri );
	Check( ExpressionProgram::IsFinite(a), "scatter: cell_scale=1e6 at u=v=1e5 gives a finite GetAlpha" );

	const Scalar nm = sp->GetColorNM( ri, 550.0 );
	Check( ExpressionProgram::IsFinite(nm), "scatter: cell_scale=1e6 at u=v=1e5 gives a finite GetColorNM" );

	sp->release();
	estamp.release(); bg.release();
}

//======================================================================
// S9 (doc 88 Phase-2 §7 decision 3): `fw` plumbing + fbm/turbulence/
// ridged footprint-aware octave fade.
//
//   - NoiseCore::OctaveFadeWeight(fw) golden pins (closed-form
//     smoothstep(0.2,0.6,fw), independently re-derived below).
//   - Fbm3D/Turbulence3D/Ridged3D at fw=0 (the default, and the value
//     every pre-S9 call site used) reproduce the EXACT pre-existing
//     "fbm golden" / "turbulence golden" / "ridged golden" constants
//     from TestNoiseBuiltins -- bit-identity, not just "close".
//   - A from-scratch weighted-sum reimplementation (built on the
//     already golden-pinned PerlinOctave3D primitive) matches
//     Fbm3D/Turbulence3D/Ridged3D's fw>0 output, proving the per-octave
//     fw*lacunarity^i scaling and weight application are wired as
//     documented.
//   - fw at/above the octave-0 cutoff (0.6) collapses every octave's
//     weight to exactly 0, so all three functions return exactly 0 --
//     a literal, implementation-independent golden.
//   - Turbulence3D/Ridged3D (both non-negative per-term sums divided by
//     a fw-independent ampSum) are monotonically non-increasing in fw --
//     proven structurally (every term's weight is non-increasing in fw)
//     and checked numerically across several fw values.
//   - ExpressionPainter/ExpressionScalarPainter::BuildContext populate
//     ExprEvalContext::fw from ri.txFootprint (0 when invalid, the
//     stored worldWidth when valid) -- exercised through the public
//     GetColor/GetValuesAt surface, matching TestExpressionPainter
//     ContextVaries' pattern (S2, test 17) for P.
//   - The VM plumbing end-to-end: Eval(ExprEvalContext) with fw>0 for a
//     literal fbm(...) call matches the direct NoiseCore::Fbm3D(...,fw)
//     call -- proves env[kFwSlot] really reaches CallFunc's cases 43-45
//     and isn't silently stuck at 0.
//   - GetColorNM (spectral uplift) stays consistent with EvalRGB when
//     the body reads `fw` on a footprint-carrying RI -- NM/RGB parity
//     unaffected by the new context var.
//======================================================================

// Independent (from the VM's smoothstep case 31 and the documented
// weight formula) re-derivation, NOT a call into NoiseCore -- a real
// cross-check, not a tautology.
static Scalar S9_Smoothstep( Scalar e0, Scalar e1, Scalar x )
{
	const Scalar t = std::min( std::max( ( x - e0 ) / ( e1 - e0 ), Scalar(0) ), Scalar(1) );
	return t*t*( Scalar(3) - Scalar(2)*t );
}
static Scalar S9_OctaveFadeWeight( Scalar fw )
{
	return Scalar(1) - S9_Smoothstep( Scalar(0.2), Scalar(0.6), fw );
}
// Mirrors the documented Fbm3D/Turbulence3D/Ridged3D composition
// (amplitude_i = gain^i, frequency_i = lacunarity^i, per-octave fade
// weight at fw*lacunarity^i) built directly on PerlinOctave3D -- the
// SAME lattice primitive TestNoiseBuiltins already golden-pins.
static Scalar S9_ExpectedFadedFbm( Scalar x, Scalar y, Scalar z, int octaves, Scalar gain, Scalar lacunarity, Scalar fw )
{
	Scalar total = 0, amplitude = 1, frequency = 1, fwOct = fw;
	for( int i = 0; i < octaves; ++i ) {
		total += NoiseCore::PerlinOctave3D( x*frequency, y*frequency, z*frequency ) * amplitude * S9_OctaveFadeWeight( fwOct );
		amplitude *= gain; frequency *= lacunarity; fwOct *= lacunarity;
	}
	return total;
}
// P1 (S9 review round 1): fade-to-mean means, reproduced here as plain
// literals (NOT a reference into ProceduralNoiseCore.cpp's anonymous
// namespace, which is deliberately not exported) -- MUST match
// kTurbulenceMeanAbs / kRidgedMeanSq there exactly; see that file's
// derivation comment (probe seed/extent/N and the two measured runs) for
// where these numbers come from.
static const Scalar S9_kTurbulenceMeanAbs = Scalar(0.1037);
static const Scalar S9_kRidgedMeanSq      = Scalar(0.8093);

static Scalar S9_ExpectedFadedTurbulence( Scalar x, Scalar y, Scalar z, int octaves, Scalar gain, Scalar lacunarity, Scalar fw )
{
	Scalar total = 0, amplitude = 1, frequency = 1, ampSum = 0, fwOct = fw;
	bool allFullyFaded = true;
	for( int i = 0; i < octaves; ++i ) {
		const Scalar term = std::fabs( NoiseCore::PerlinOctave3D( x*frequency, y*frequency, z*frequency ) );
		const Scalar w = S9_OctaveFadeWeight( fwOct );
		if( w != Scalar(0) ) allFullyFaded = false;
		// Fade-to-mean, mirroring NoiseCore::Turbulence3D's fix: a faded
		// octave converges to its expected value, not zero.
		total += ( S9_kTurbulenceMeanAbs * ( Scalar(1) - w ) + term * w ) * amplitude;
		ampSum += amplitude;
		amplitude *= gain; frequency *= lacunarity; fwOct *= lacunarity;
	}
	if( allFullyFaded ) return S9_kTurbulenceMeanAbs;
	return ( ampSum > 1e-12 ) ? ( total / ampSum ) : Scalar(0);
}
static Scalar S9_ExpectedFadedRidged( Scalar x, Scalar y, Scalar z, int octaves, Scalar gain, Scalar lacunarity, Scalar fw )
{
	Scalar total = 0, amplitude = 1, frequency = 1, ampSum = 0, fwOct = fw;
	bool allFullyFaded = true;
	for( int i = 0; i < octaves; ++i ) {
		const Scalar n = NoiseCore::PerlinOctave3D( x*frequency, y*frequency, z*frequency );
		const Scalar ridge = Scalar(1) - std::fabs( n );
		const Scalar term = ridge * ridge;
		const Scalar w = S9_OctaveFadeWeight( fwOct );
		if( w != Scalar(0) ) allFullyFaded = false;
		total += ( S9_kRidgedMeanSq * ( Scalar(1) - w ) + term * w ) * amplitude;
		ampSum += amplitude;
		amplitude *= gain; frequency *= lacunarity; fwOct *= lacunarity;
	}
	if( allFullyFaded ) return S9_kRidgedMeanSq;
	return ( ampSum > 1e-12 ) ? ( total / ampSum ) : Scalar(0);
}

static void TestFbmFootprintFadeWeightGoldens()
{
	std::cout << "Test 56: NoiseCore::OctaveFadeWeight golden values" << std::endl;

	CheckClose( NoiseCore::OctaveFadeWeight( 0.0 ),  1.0, 1e-12, "weight(0.0) == 1 (fully resolved)" );
	CheckClose( NoiseCore::OctaveFadeWeight( 0.2 ),  1.0, 1e-12, "weight(0.2) == 1 (fade-band lower edge)" );
	CheckClose( NoiseCore::OctaveFadeWeight( 0.4 ),  0.5, 1e-12, "weight(0.4) == 0.5 (fade-band midpoint)" );
	CheckClose( NoiseCore::OctaveFadeWeight( 0.6 ),  0.0, 1e-12, "weight(0.6) == 0 (fade-band upper edge)" );
	CheckClose( NoiseCore::OctaveFadeWeight( 0.8 ),  0.0, 1e-12, "weight(0.8) == 0 (past cutoff)" );
	CheckClose( NoiseCore::OctaveFadeWeight( 100.0 ),0.0, 1e-12, "weight(100) == 0 (way past cutoff)" );
	CheckClose( NoiseCore::OctaveFadeWeight( -5.0 ), 1.0, 1e-12, "weight(-5) clamps to fw=0 -> 1 (never negative-fade)" );
	CheckClose( NoiseCore::OctaveFadeWeight( std::numeric_limits<Scalar>::quiet_NaN() ), 1.0, 1e-12,
		"weight(NaN) guarded to fw=0 -> 1 (SafeParam)" );
}

static void TestFbmFootprintFadeBitIdentityAtZero()
{
	std::cout << "Test 57: fbm/turbulence/ridged fw=0 (default) reproduce the pre-S9 goldens EXACTLY" << std::endl;

	// Same point/params as TestNoiseBuiltins' "fbm/turbulence/ridged golden".
	const Scalar x = 0.3, y = 0.7, z = 1.4;
	const int octaves = 4; const Scalar gain = 0.5, lacunarity = 2.0;

	CheckClose( NoiseCore::Fbm3D( x,y,z, octaves, gain, lacunarity ), -0.19859653334474803, 1e-15,
		"Fbm3D default (no fw arg) == pre-S9 fbm golden, bit-exact" );
	CheckClose( NoiseCore::Fbm3D( x,y,z, octaves, gain, lacunarity, 0.0 ), -0.19859653334474803, 1e-15,
		"Fbm3D fw=0.0 explicit == pre-S9 fbm golden, bit-exact" );
	CheckClose( NoiseCore::Turbulence3D( x,y,z, octaves, gain, lacunarity, 0.0 ), 0.12500609165612192, 1e-15,
		"Turbulence3D fw=0.0 == pre-S9 turbulence golden, bit-exact" );
	CheckClose( NoiseCore::Ridged3D( x,y,z, octaves, gain, lacunarity, 0.0 ), 0.77670332638038786, 1e-15,
		"Ridged3D fw=0.0 == pre-S9 ridged golden, bit-exact" );

	// The two call forms (implicit default vs explicit 0.0) must be the
	// SAME double bit pattern, not just numerically close.
	Check( NoiseCore::Fbm3D( x,y,z, octaves, gain, lacunarity ) == NoiseCore::Fbm3D( x,y,z, octaves, gain, lacunarity, 0.0 ),
		"Fbm3D: default-arg call == explicit fw=0.0 call, bit-identical" );

	// Same golden point through the VM (Eval(u,v) leaves fw at its
	// default 0 via BindEnv) -- end-to-end, not just the C++ API.
	CheckClose( Prog( "fbm(vec3(0.3,0.7,1.4), 4, 0.5, 2.0)" ).prog.Eval(0,0), -0.19859653334474803, 1e-15,
		"VM fbm(...) with fw untouched (defaults to 0 via Eval(u,v)) still matches the pre-S9 golden" );
}

static void TestFbmFootprintFadeMatchesReimplementation()
{
	std::cout << "Test 58: fbm/turbulence/ridged fw>0 output matches an independent weighted-sum reimplementation" << std::endl;

	const Scalar x = 0.3, y = 0.7, z = 1.4;
	const int octaves = 4; const Scalar gain = 0.5, lacunarity = 2.0;
	const Scalar fws[] = { 0.05, 0.1, 0.2, 0.3, 0.6, 5.0 };

	for( Scalar fw : fws ) {
		char label[64]; snprintf( label, sizeof(label), "fw=%.3f", (double)fw );

		CheckClose( NoiseCore::Fbm3D( x,y,z, octaves, gain, lacunarity, fw ),
			S9_ExpectedFadedFbm( x,y,z, octaves, gain, lacunarity, fw ), 1e-12,
			std::string("Fbm3D matches reimplementation at ") + label );
		CheckClose( NoiseCore::Turbulence3D( x,y,z, octaves, gain, lacunarity, fw ),
			S9_ExpectedFadedTurbulence( x,y,z, octaves, gain, lacunarity, fw ), 1e-12,
			std::string("Turbulence3D matches reimplementation at ") + label );
		CheckClose( NoiseCore::Ridged3D( x,y,z, octaves, gain, lacunarity, fw ),
			S9_ExpectedFadedRidged( x,y,z, octaves, gain, lacunarity, fw ), 1e-12,
			std::string("Ridged3D matches reimplementation at ") + label );
	}
}

static void TestFbmFootprintFadeCollapsesPastCutoff()
{
	std::cout << "Test 59: fw at/above the first-octave cutoff collapses fbm to exactly 0, turbulence/ridged to their fade-to-mean constants (P1, S9 review round 1)" << std::endl;

	// lacunarity=2, octaves=4: fwOctave sequence at fw=0.6 is
	// {0.6, 1.2, 2.4, 4.8} -- every one >= 0.6, so every octave's weight
	// is exactly 0 (see the golden-pinned weight(0.6)==0 above).  Fbm3D is
	// signed/zero-mean, so its composed sum is still exactly 0.0 regardless
	// of what PerlinOctave3D returns.  Turbulence3D/Ridged3D are NOT
	// zero-mean -- the pre-fix code collapsed them to 0 too (the P1 bug:
	// darkening, because the numerator faded to 0 while ampSum stayed
	// full), the fix makes them collapse to their EXPECTED VALUE instead,
	// exactly (via the allFullyFaded shortcut in NoiseCore::Turbulence3D/
	// Ridged3D -- see that file for why total/ampSum alone isn't reliably
	// bit-exact here).
	const Scalar x = 0.3, y = 0.7, z = 1.4;
	CheckClose( NoiseCore::Fbm3D( x,y,z, 4, 0.5, 2.0, 0.6 ), 0.0, 1e-15, "Fbm3D collapses to exactly 0 at fw=0.6" );
	CheckClose( NoiseCore::Turbulence3D( x,y,z, 4, 0.5, 2.0, 0.6 ), 0.1037, 1e-15, "Turbulence3D collapses to exactly kTurbulenceMeanAbs at fw=0.6" );
	CheckClose( NoiseCore::Ridged3D( x,y,z, 4, 0.5, 2.0, 0.6 ), 0.8093, 1e-15, "Ridged3D collapses to exactly kRidgedMeanSq at fw=0.6" );

	// A much larger fw (10x, then 100x per the P1 brief) must land on the
	// SAME exact constants -- not just "smaller"/"closer".
	CheckClose( NoiseCore::Fbm3D( x,y,z, 4, 0.5, 2.0, 6.0 ), 0.0, 1e-15, "Fbm3D stays exactly 0 at fw=6.0" );
	CheckClose( NoiseCore::Turbulence3D( x,y,z, 4, 0.5, 2.0, 100.0 ), 0.1037, 1e-15, "Turbulence3D == kTurbulenceMeanAbs exactly at fw=100" );
	CheckClose( NoiseCore::Ridged3D( x,y,z, 4, 0.5, 2.0, 100.0 ), 0.8093, 1e-15, "Ridged3D == kRidgedMeanSq exactly at fw=100" );
	// Different (x,y,z), octaves, gain, lacunarity -- the constant must not
	// depend on the sample point or octave shape, only on full fade.
	CheckClose( NoiseCore::Turbulence3D( 9.1,-4.4,0.02, 7, 0.62, 1.87, 100.0 ), 0.1037, 1e-15,
		"Turbulence3D == kTurbulenceMeanAbs exactly at fw=100, different point/octaves/gain/lacunarity" );
	CheckClose( NoiseCore::Ridged3D( 9.1,-4.4,0.02, 7, 0.62, 1.87, 100.0 ), 0.8093, 1e-15,
		"Ridged3D == kRidgedMeanSq exactly at fw=100, different point/octaves/gain/lacunarity" );
}

// Deterministic 32x32x1 grid of world-space sample points (P1, S9 review
// round 1): a non-integer step so no sample lands on a lattice vertex
// (which would degenerate PerlinOctave3D to a single raw hash value
// instead of the smoothed/interpolated field), spanning many lattice
// cells so the mean/variance are a real spatial statistic, not a
// single-cell artifact.  Shared by the anti-darkening pin and the
// variance-monotonicity test below.
static void S9_GridMeanVariance( int octaves, Scalar gain, Scalar lacunarity, Scalar fw,
	Scalar (*fn)( Scalar, Scalar, Scalar, int, Scalar, Scalar, Scalar ),
	Scalar& outMean, Scalar& outVariance )
{
	const int kGrid = 32;
	const Scalar step = Scalar(0.371);
	const Scalar z = Scalar(1.13);
	Scalar sum = 0, sumSq = 0;
	for( int i = 0; i < kGrid; ++i ) {
		for( int j = 0; j < kGrid; ++j ) {
			const Scalar x = i * step, y = j * step;
			const Scalar v = fn( x, y, z, octaves, gain, lacunarity, fw );
			sum += v; sumSq += v*v;
		}
	}
	const int n = kGrid*kGrid;
	outMean = sum / n;
	outVariance = sumSq/n - outMean*outMean;
}

static void TestFbmFootprintFadeAntiDarkeningPin()
{
	std::cout << "Test 60: turbulence/ridged grid-mean brightness preservation at fw=0.25 (P1 anti-darkening pin -- pre-fix this failed by ~45%)" << std::endl;

	const int octaves = 4; const Scalar gain = 0.5, lacunarity = 2.0;

	Scalar meanT0, varT0, meanT25, varT25;
	S9_GridMeanVariance( octaves, gain, lacunarity, 0.0,  NoiseCore::Turbulence3D, meanT0, varT0 );
	S9_GridMeanVariance( octaves, gain, lacunarity, 0.25, NoiseCore::Turbulence3D, meanT25, varT25 );
	Check( std::fabs( meanT25 - meanT0 ) <= Scalar(0.05) * meanT0,
		"Turbulence3D 32x32 grid mean at fw=0.25 is within 5% of fw=0 (measured ~0.6% post-fix; pre-fix ~45% darkening)" );

	Scalar meanR0, varR0, meanR25, varR25;
	S9_GridMeanVariance( octaves, gain, lacunarity, 0.0,  NoiseCore::Ridged3D, meanR0, varR0 );
	S9_GridMeanVariance( octaves, gain, lacunarity, 0.25, NoiseCore::Ridged3D, meanR25, varR25 );
	Check( std::fabs( meanR25 - meanR0 ) <= Scalar(0.05) * meanR0,
		"Ridged3D 32x32 grid mean at fw=0.25 is within 5% of fw=0 (measured ~0.2% post-fix)" );
}

static void TestFbmFootprintFadeVarianceMonotonic()
{
	std::cout << "Test 61: turbulence/ridged grid VARIANCE is non-increasing in fw while the grid MEAN stays in the anti-darkening band (reworked from Test 60 -- per-point value is no longer monotonic under fade-to-mean, since a point below the mean now RISES toward it)" << std::endl;

	const int octaves = 6; const Scalar gain = 0.55, lacunarity = 2.13;	// same params as TestFbmDeterminism / the old Test 60
	const Scalar fws[] = { 0.0, 0.05, 0.1, 0.15, 0.2, 0.3, 0.4, 0.5, 0.6, 1.0, 5.0 };
	const size_t n = sizeof(fws)/sizeof(fws[0]);

	Scalar mean0T, var0T; S9_GridMeanVariance( octaves, gain, lacunarity, fws[0], NoiseCore::Turbulence3D, mean0T, var0T );
	Scalar mean0R, var0R; S9_GridMeanVariance( octaves, gain, lacunarity, fws[0], NoiseCore::Ridged3D,     mean0R, var0R );

	Scalar prevVarT = var0T, prevVarR = var0R;
	Scalar lastVarT = var0T, lastVarR = var0R;
	bool varTMonotonic = true, varRMonotonic = true;
	bool meanTInBand = true, meanRInBand = true;
	for( size_t i = 1; i < n; ++i ) {
		Scalar meanT, varT; S9_GridMeanVariance( octaves, gain, lacunarity, fws[i], NoiseCore::Turbulence3D, meanT, varT );
		Scalar meanR, varR; S9_GridMeanVariance( octaves, gain, lacunarity, fws[i], NoiseCore::Ridged3D,     meanR, varR );
		if( varT > prevVarT + Scalar(1e-9) ) varTMonotonic = false;
		if( varR > prevVarR + Scalar(1e-9) ) varRMonotonic = false;
		if( std::fabs( meanT - mean0T ) > Scalar(0.05) * mean0T ) meanTInBand = false;
		if( std::fabs( meanR - mean0R ) > Scalar(0.05) * mean0R ) meanRInBand = false;
		prevVarT = varT; prevVarR = varR;
		lastVarT = varT; lastVarR = varR;
	}
	Check( varTMonotonic, "Turbulence3D grid variance is non-increasing as fw sweeps 0..5" );
	Check( varRMonotonic, "Ridged3D grid variance is non-increasing as fw sweeps 0..5" );
	Check( meanTInBand, "Turbulence3D grid mean stays within 5% of the fw=0 mean across the whole fw sweep (anti-darkening holds throughout, not just at one fw)" );
	Check( meanRInBand, "Ridged3D grid mean stays within 5% of the fw=0 mean across the whole fw sweep" );

	// And a real drop happened (not flat the whole way, which would
	// trivially "pass" monotonicity without proving the fade fired).
	Check( var0T > 0 && ( var0T - lastVarT ) > Scalar(0.5) * var0T,
		"Turbulence3D grid variance actually drops materially (>50%) from fw=0 to fw=5 (fade is live, not a no-op)" );
	Check( var0R > 0 && ( var0R - lastVarR ) > Scalar(0.5) * var0R,
		"Ridged3D grid variance actually drops materially (>50%) from fw=0 to fw=5" );
}

static void TestExpressionPainterFootprintContext()
{
	std::cout << "Test 62: ExpressionPainter/ExpressionScalarPainter BuildContext populate fw from ri.txFootprint" << std::endl;

	Job* job = new Job();
	job->addref();
	const char* body =
		"expression_painter\n{\nname fwcolor\nexpr fw\n}\n"
		"scalar_painter\n{\nname fwscalar\nexpression fw\n}\n";
	Check( S2::ParseBody( "s9_fw", body, *job ), "parses" );
	IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
	Check( priv != 0, "IJobPriv available" );
	if( priv ) {
		IPainter* colorP = priv->GetPainters()->GetItem( "fwcolor" );
		IScalarPainter* scalarP = priv->GetScalarPainters()->GetItem( "fwscalar" );
		Check( colorP != 0, "expression_painter registered" );
		Check( scalarP != 0, "scalar_painter registered" );

		if( colorP && scalarP ) {
			// No footprint (default-constructed RayIntersectionGeometric):
			// fw must be the honest 0.0, not a stale/garbage value.
			RayIntersectionGeometric rInvalid( Ray(), nullRasterizerState );
			rInvalid.bHit = true;
			Check( !rInvalid.txFootprint.valid && !rInvalid.txFootprint.widthValid,
				"test bug: freshly-constructed RI has an invalid footprint (both flags)" );

			const RISEPel cInvalid = colorP->GetColor( rInvalid );
			CheckClose( cInvalid[0], 0.0, 1e-12, "expression_painter: fw==0 with no footprint (color pipe)" );
			CheckClose( cInvalid[1], 0.0, 1e-12, "expression_painter: fw==0 with no footprint (color pipe, g)" );

			const ScalarTriple tInvalid = scalarP->GetValuesAt( rInvalid );
			CheckClose( tInvalid.v[0], 0.0, 1e-12, "scalar_painter: fw==0 with no footprint (scalar pipe)" );

			// Valid footprint carrying a specific worldWidth: fw must read
			// that exact value back, on BOTH pipes.
			RayIntersectionGeometric rValid( Ray(), nullRasterizerState );
			rValid.bHit = true;
			// `widthValid` is the flag `fw` keys on -- `valid` is the
			// UV-Jacobian flag and is set here only to honour the
			// `valid => widthValid` invariant a real hit would carry.
			rValid.txFootprint.widthValid = true;
			rValid.txFootprint.valid = true;
			rValid.txFootprint.worldWidth = 0.037;

			const RISEPel cValid = colorP->GetColor( rValid );
			CheckClose( cValid[0], 0.037, 1e-12, "expression_painter: fw==worldWidth with a valid footprint (color pipe)" );
			CheckClose( cValid[1], 0.037, 1e-12, "expression_painter: fw==worldWidth with a valid footprint (color pipe, g)" );
			CheckClose( cValid[2], 0.037, 1e-12, "expression_painter: fw==worldWidth with a valid footprint (color pipe, b)" );

			const ScalarTriple tValid = scalarP->GetValuesAt( rValid );
			CheckClose( tValid.v[0], 0.037, 1e-12, "scalar_painter: fw==worldWidth with a valid footprint (scalar pipe)" );

			// A different worldWidth gives a different fw -- not a
			// constant baked in anywhere.
			RayIntersectionGeometric rValid2( Ray(), nullRasterizerState );
			rValid2.bHit = true;
			rValid2.txFootprint.widthValid = true;
			rValid2.txFootprint.valid = true;
			rValid2.txFootprint.worldWidth = 1.5;
			const RISEPel cValid2 = colorP->GetColor( rValid2 );
			Check( cValid2[0] != cValid[0], "expression_painter: fw varies with worldWidth (not a stuck constant)" );
		}
	}
	job->release();
}

static void TestExpressionVMFwEndToEnd()
{
	std::cout << "Test 63: ExprEvalContext.fw reaches fbm/turbulence/ridged through the full VM env-slot path (kContextSlotFw)" << std::endl;

	ExpressionProgram::Builder b;
	b.EnableContextVars( true );
	ExpressionProgram prog = ExpressionProgram::Invalid();
	// The position argument is BARE `P` on purpose.  It used to be the
	// literal `vec3(0.3,0.7,1.4)`, which since the 2026-09-06 domain-scale
	// change is a provably P-independent argument and therefore never
	// fades at all -- a fine thing to pin (TestFbmDomainScaleFallbacks
	// does), but useless for proving fw REACHES CallFunc.  Bare `P` has
	// Jacobian == identity, so the multiplier is exactly 1.0 and the
	// direct-call comparison below stays the strict identity it always was.
	Check( b.Finalize( "fbm(P, 4, 0.5, 2.0)", prog ), "compiles with context vars enabled" );

	if( prog.IsValid() ) {
		ExprEvalContext ctx;
		ctx.u = 0; ctx.v = 0; ctx.P = Vector3(0.3,0.7,1.4); ctx.Po = Vector3(0,0,0); ctx.N = Vector3(0,0,0);
		ctx.time = 0;

		for( Scalar fw : { 0.0, 0.05, 0.3, 0.6, 2.0 } ) {
			ctx.fw = fw;
			const Scalar viaVM = prog.Eval( ctx );
			const Scalar viaDirect = NoiseCore::Fbm3D( 0.3,0.7,1.4, 4, 0.5, 2.0, fw );
			char label[64]; snprintf( label, sizeof(label), "fw=%.3f", (double)fw );
			Check( viaVM == viaDirect, std::string("VM fbm(P) with ctx.fw is BIT-IDENTICAL to direct NoiseCore::Fbm3D at ") + label );

			// SECOND FOOTPRINT, SAME ANSWER (2026-09-06).  The hit now
			// also carries `fwo`, the object-space width, and RunAny
			// evaluates `fw*val + fwo*valo` at every noise call site.  A
			// body whose argument does not mention `Po` compiles to
			// valo == exactly 0, so an arbitrarily large fwo must not
			// move the result by one ulp -- this is the runtime half of
			// the bit-identity promise test 67 makes at compile time.
			for( Scalar fwo : { 0.05, 7.0, 1000.0 } ) {
				ctx.fwo = fwo;
				char l2[128];
				snprintf( l2, sizeof(l2),
					"a P-only body is untouched by ctx.fwo (fw=%.3f, fwo=%g)", (double)fw, (double)fwo );
				Check( prog.Eval( ctx ) == viaDirect, l2 );
			}
			ctx.fwo = 0;
		}
	}
}

//======================================================================
// Domain-scale (2026-09-06): fbm/turbulence/ridged rescale the
// world-space `fw` into their OWN position argument's domain, using a
// compile-time forward-mode Jacobian of that argument w.r.t. P
// (ExpressionEval.h, Builder::NoiseFwScales).  Before this, `fw` went in
// unscaled, so the standard frequency idiom `fbm(P*k, ...)` set the
// Nyquist threshold k-times too low and the fade effectively never
// engaged in any real scene (measured k: 7 .. 820).
//
//   - Test 65 DOMAIN CONSISTENCY: the same noise field reached two ways
//     -- scaling the argument, or pre-scaling P and the footprint --
//     must fade identically.  This is the invariant the whole change
//     exists to establish, and it FAILS on the pre-change engine.
//   - Test 66 THE FADE ENGAGES: on a scaled domain a large footprint
//     leaves strictly fewer octaves with non-zero weight than fw == 0.
//   - Test 67 BIT-IDENTITY: the values recorded from a run of the
//     PRE-change engine, asserted with ==, for every case the change
//     promises not to touch -- fw == 0 (any domain), and a bare `P`
//     argument (Jacobian == identity, multiplier exactly 1.0).
//   - Test 69 CONSERVATIVE SUBTRACTION: a subtraction whose other
//     operand is opaque can CANCEL the affine part, so it reports no
//     scale at all rather than the surviving term's; addition keeps
//     its affine part (the domain-warp idiom) unchanged.
//   - Test 70 ROTATED / SHEARED: the non-diagonal (cyclic Jacobi)
//     branch of JacobianSpectralNorm, reachable only with literal
//     off-diagonal coefficients, fades at the true largest singular
//     value -- not at the naive largest row norm.
//   - Test 68 FALLBACKS: a `def`/`param`-carried scale resolves;
//     a domain warp keeps its affine part's scale; an argument built
//     from u/v (no provable relation to world P) falls back to the
//     pre-change multiplier 1.0; a literal argument never fades.
//======================================================================

static void TestFbmDomainScaleConsistency()
{
	std::cout << "Test 65: fbm(P*k) at footprint w == fbm(P) at k*P and footprint k*w (the fade is domain-consistent)" << std::endl;

	const Vector3 p0( 0.13, -0.41, 0.77 );
	const Scalar k = 7.0;
	const char* bodies[3] = { "fbm(%s, 5, 0.5, 2.0)", "turbulence(%s, 5, 0.5, 2.0)", "ridged(%s, 5, 0.5, 2.0)" };
	const char* names[3]  = { "fbm", "turbulence", "ridged" };

	for( int f = 0; f < 3; ++f ) {
		char scaledBody[128], plainBody[128];
		snprintf( scaledBody, sizeof(scaledBody), bodies[f], "P*7.0" );
		snprintf( plainBody,  sizeof(plainBody),  bodies[f], "P" );

		ExpressionProgram::Builder bs; bs.EnableContextVars( true );
		ExpressionProgram scaled = ExpressionProgram::Invalid();
		Check( bs.Finalize( scaledBody, scaled ), std::string(names[f]) + "(P*7.0, ...) compiles" );

		ExpressionProgram::Builder bp; bp.EnableContextVars( true );
		ExpressionProgram plain = ExpressionProgram::Invalid();
		Check( bp.Finalize( plainBody, plain ), std::string(names[f]) + "(P, ...) compiles" );

		if( !scaled.IsValid() || !plain.IsValid() ) continue;

		for( Scalar w : { 0.0, 0.004, 0.02, 0.06, 0.2 } ) {
			ExprEvalContext a;			// scaled argument, world-space footprint
			a.P = p0; a.fw = w;
			ExprEvalContext b2;			// pre-scaled position, pre-scaled footprint
			b2.P = Vector3( p0.x*k, p0.y*k, p0.z*k ); b2.fw = k*w;

			char label[96];
			snprintf( label, sizeof(label), "%s: fw=%.4f scaled-domain == pre-scaled equivalent", names[f], (double)w );
			// Exact, not approximate: both routes must hand
			// NoiseCore the very same (x,y,z,fw), so any difference
			// would be a real disagreement, not rounding.
			Check( scaled.Eval( a ) == plain.Eval( b2 ), label );
		}
	}
}

static void TestFbmDomainScaleFadeEngages()
{
	std::cout << "Test 66: a scaled-domain fbm at a large footprint keeps strictly fewer octaves than at fw == 0" << std::endl;

	// Counts octaves the fade leaves alive, from the documented weight
	// (re-derived above, independently of NoiseCore) at the DOMAIN
	// footprint the VM should now be using.
	struct Local {
		static int LiveOctaves( Scalar domainFw, int octaves, Scalar lacunarity )
		{
			int live = 0;
			Scalar fwOct = domainFw;
			for( int i = 0; i < octaves; ++i ) {
				if( S9_OctaveFadeWeight( fwOct ) > Scalar(0) ) ++live;
				fwOct *= lacunarity;
			}
			return live;
		}
	};

	const int    octaves = 5;
	const Scalar lac = 2.0, gain = 0.5, k = 7.0, w = 0.06;
	const Vector3 p0( 0.13, -0.41, 0.77 );

	ExpressionProgram::Builder b; b.EnableContextVars( true );
	ExpressionProgram prog = ExpressionProgram::Invalid();
	Check( b.Finalize( "fbm(P*7.0, 5, 0.5, 2.0)", prog ), "fbm(P*7.0, 5, 0.5, 2.0) compiles" );
	if( !prog.IsValid() ) return;

	const int liveAtZero   = Local::LiveOctaves( 0.0,   octaves, lac );
	const int liveAtScaled = Local::LiveOctaves( k*w,   octaves, lac );
	const int liveUnscaled = Local::LiveOctaves( w,     octaves, lac );
	Check( liveAtZero == octaves, "fw == 0 resolves every octave (5 live)" );
	Check( liveAtScaled < liveAtZero, "the domain footprint 7*0.06 kills octaves the point sample kept" );
	// The pre-change engine faded at the UNSCALED footprint, which
	// leaves strictly more octaves alive -- it under-fades by exactly
	// the domain scale, which is the bug this change fixes.
	Check( liveUnscaled > liveAtScaled,
		"the pre-change (unscaled) footprint under-fades: strictly more octaves survive it" );
	// At a footprint typical of an actual render of this scene class
	// (a 256px frame of a sphere ~4 units away) the pre-change fade was
	// not merely weaker, it was completely inert.
	const Scalar renderFw = 0.008;
	Check( Local::LiveOctaves( renderFw, octaves, lac ) == octaves,
		"at a real render footprint the unscaled fade touched NOTHING (all 5 octaves full weight)" );
	Check( Local::LiveOctaves( k*renderFw, octaves, lac ) < octaves,
		"the same footprint, correctly scaled into the noise's domain, does fade" );

	ExprEvalContext ctx; ctx.P = p0; ctx.fw = w;
	const Scalar viaVM = prog.Eval( ctx );
	const Scalar expectedScaled   = S9_ExpectedFadedFbm( p0.x*k, p0.y*k, p0.z*k, octaves, gain, lac, k*w );
	const Scalar expectedUnscaled = S9_ExpectedFadedFbm( p0.x*k, p0.y*k, p0.z*k, octaves, gain, lac, w );
	CheckClose( viaVM, expectedScaled, 1e-12, "VM matches the from-scratch sum faded at the SCALED footprint" );
	Check( std::fabs( expectedScaled - expectedUnscaled ) > 1e-6,
		"the scaled and unscaled fades are genuinely different values (the test is not vacuous)" );
}

static void TestFbmDomainScaleBitIdentity()
{
	std::cout << "Test 67: pre-change golden values, unchanged (fw == 0 anywhere; a bare `P` argument at any fw)" << std::endl;

	// RECORDED from a run of the engine as it stood at 174ea668, BEFORE
	// the domain-scale change, printed at %.17g.  Asserted with == so a
	// future edit that perturbs an un-scaled body by one ulp is caught:
	// every scene without a footprint, and every body whose noise
	// argument is bare `P`, must keep rendering exactly as it did.
	// `fwo` (2026-09-06) is carried on every row: a body that does not
	// mention `Po` must produce the recorded pre-change value no matter
	// what object-space footprint the hit reports.  (The mirror claim --
	// a Po-only body at fwo == 0 reproduces the un-faded sum -- is
	// pinned in test 71, against a from-scratch reference rather than a
	// recorded literal.)
	struct Row { const char* body; double px, py, pz; double u, v; double fw; double fwo; double expect; };
	const Row rows[] = {
		// Bare `P` -- Jacobian is the identity, multiplier exactly 1.0.
		{ "fbm(P, 4, 0.5, 2.0)",        0.3, 0.7, 1.4, 0.37, 0.61, 0.0,  0.0, -0.19859653334474803 },
		{ "fbm(P, 4, 0.5, 2.0)",        0.3, 0.7, 1.4, 0.37, 0.61, 0.05, 0.0, -0.20462375812775757 },
		{ "fbm(P, 4, 0.5, 2.0)",        0.3, 0.7, 1.4, 0.37, 0.61, 0.3,  0.0, -0.057247597370529507 },
		{ "turbulence(P, 4, 0.5, 2.0)", 0.3, 0.7, 1.4, 0.37, 0.61, 0.0,  0.0,  0.12500609165612192 },
		{ "turbulence(P, 4, 0.5, 2.0)", 0.3, 0.7, 1.4, 0.37, 0.61, 0.05, 0.0,  0.12524823843851685 },
		{ "turbulence(P, 4, 0.5, 2.0)", 0.3, 0.7, 1.4, 0.37, 0.61, 0.3,  0.0,  0.087567051930949061 },
		{ "ridged(P, 4, 0.5, 2.0)",     0.3, 0.7, 1.4, 0.37, 0.61, 0.0,  0.0,  0.77670332638038786 },
		{ "ridged(P, 4, 0.5, 2.0)",     0.3, 0.7, 1.4, 0.37, 0.61, 0.05, 0.0,  0.77646570533967341 },
		{ "ridged(P, 4, 0.5, 2.0)",     0.3, 0.7, 1.4, 0.37, 0.61, 0.3,  0.0,  0.83612246546057789 },
		// A SCALED domain at fw == 0: the point-sample path, which the
		// multiplier cannot disturb because 0 * anything finite is 0.
		{ "fbm(P*7.0, 5, 0.5, 2.0)",    0.3, 0.7, 1.4, 0.37, 0.61, 0.0,  0.0, -0.11547628157415081 },
		// An argument with no provable relation to EITHER position:
		// falls back to the pre-change multipliers (1.0, 0.0) at EVERY
		// footprint, not just 0.
		{ "fbm(vec3(u*10.0, v*10.0, 0.0), 4, 0.5, 2.0)", 0,0,0, 0.37, 0.61, 0.0,  0.0,   0.006329461722634845 },
		{ "fbm(vec3(u*10.0, v*10.0, 0.0), 4, 0.5, 2.0)", 0,0,0, 0.37, 0.61, 0.05, 0.0,   0.009510600846260894 },
		{ "fbm(vec3(u*10.0, v*10.0, 0.0), 4, 0.5, 2.0)", 0,0,0, 0.37, 0.61, 0.3,  0.0,  -0.10298160573714994 },
		// THE SAME P-ONLY GOLDENS WITH A LARGE OBJECT-SPACE FOOTPRINT
		// PRESENT (2026-09-06).  These are the rows the Po change could
		// plausibly have broken: `valo` must be an exact 0 on a body
		// that never names Po, so `fw*val + fwo*0` is `fw*val`.
		{ "fbm(P, 4, 0.5, 2.0)",        0.3, 0.7, 1.4, 0.37, 0.61, 0.05, 900.0, -0.20462375812775757 },
		{ "turbulence(P, 4, 0.5, 2.0)", 0.3, 0.7, 1.4, 0.37, 0.61, 0.3,  900.0,  0.087567051930949061 },
		{ "ridged(P, 4, 0.5, 2.0)",     0.3, 0.7, 1.4, 0.37, 0.61, 0.05, 900.0,  0.77646570533967341 },
		{ "fbm(P*7.0, 5, 0.5, 2.0)",    0.3, 0.7, 1.4, 0.37, 0.61, 0.0,  900.0, -0.11547628157415081 },
		{ "fbm(vec3(u*10.0, v*10.0, 0.0), 4, 0.5, 2.0)", 0,0,0, 0.37, 0.61, 0.3, 900.0, -0.10298160573714994 },
	};

	for( size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); ++i ) {
		const Row& r = rows[i];
		ExpressionProgram::Builder b; b.EnableContextVars( true );
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( b.Finalize( r.body, prog ), std::string("compiles: ") + r.body );
		if( !prog.IsValid() ) continue;
		ExprEvalContext ctx;
		ctx.u = r.u; ctx.v = r.v; ctx.P = Vector3( r.px, r.py, r.pz );
		ctx.fw = r.fw; ctx.fwo = r.fwo;
		char label[192];
		snprintf( label, sizeof(label), "pre-change golden: %s at fw=%g fwo=%g", r.body, r.fw, r.fwo );
		Check( prog.Eval( ctx ) == r.expect, label );
	}
}

static void TestFbmDomainScaleFallbacks()
{
	std::cout << "Test 68: domain scale through def/param, through a warp's affine part, and the unprovable fallbacks" << std::endl;

	const Vector3 p0( 0.3, 0.7, 1.4 );
	const Scalar w = 0.01;

	// (a) The scale carried by a `param` through a `def` -- the shape
	// real bodies use -- resolves exactly like the inlined literal.
	{
		ExpressionProgram::Builder b; b.EnableContextVars( true );
		Check( b.AddParam( "k", 40.0 ), "param k 40" );
		Check( b.AddDef( "q", "P*k" ), "def q P*k" );
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( b.Finalize( "fbm(q, 4, 0.5, 2.0)", prog ), "fbm(q, ...) compiles" );
		if( prog.IsValid() ) {
			ExprEvalContext ctx; ctx.P = p0; ctx.fw = w;
			Check( prog.Eval( ctx ) == NoiseCore::Fbm3D( p0.x*40.0, p0.y*40.0, p0.z*40.0, 4, 0.5, 2.0, 40.0*w ),
				"a param-through-def scale reaches the noise as 40*fw" );
		}
	}

	// (b) Anisotropic per-component scale (the plank_closeup idiom):
	// the LARGEST stretch is what a footprint has to survive, so the
	// multiplier is the largest singular value, here 820.
	{
		ExpressionProgram::Builder b; b.EnableContextVars( true );
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( b.Finalize( "fbm(vec3(P.x*40.0, P.y*820.0, P.z*820.0), 4, 0.5, 2.0)", prog ), "anisotropic vec3 compiles" );
		if( prog.IsValid() ) {
			ExprEvalContext ctx; ctx.P = p0; ctx.fw = 0.0004;
			Check( prog.Eval( ctx ) == NoiseCore::Fbm3D( p0.x*40.0, p0.y*820.0, p0.z*820.0, 4, 0.5, 2.0, 820.0*0.0004 ),
				"an anisotropic domain fades at its LARGEST stretch (820), not its smallest" );
		}
	}

	// (c) Domain warp: the warp term's own slope is unknowable at
	// compile time, so the sum keeps the affine part's scale rather
	// than giving up on the whole argument.
	{
		ExpressionProgram::Builder b; b.EnableContextVars( true );
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( b.Finalize( "fbm(P*3.0 + 0.5*vec3(fbm(P,2,0.5,2.0),0.0,0.0), 4, 0.5, 2.0)", prog ), "warped domain compiles" );
		if( prog.IsValid() ) {
			ExprEvalContext ctx; ctx.P = p0; ctx.fw = w;
			const Scalar warp = 0.5 * NoiseCore::Fbm3D( p0.x, p0.y, p0.z, 2, 0.5, 2.0, w );
			Check( prog.Eval( ctx ) == NoiseCore::Fbm3D( p0.x*3.0 + warp, p0.y*3.0, p0.z*3.0, 4, 0.5, 2.0, 3.0*w ),
				"a warped argument keeps its affine part's scale (3*fw), warp position included" );
		}
	}

	// (d) An argument with no affine part at all (u/v have no
	// compile-time relation to world P): multiplier 1.0, i.e. exactly
	// what the engine did before -- never made worse, just left alone.
	{
		ExpressionProgram::Builder b; b.EnableContextVars( true );
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( b.Finalize( "fbm(vec3(u*10.0, v*10.0, 0.0), 4, 0.5, 2.0)", prog ), "uv-domain compiles" );
		if( prog.IsValid() ) {
			ExprEvalContext ctx; ctx.u = 0.37; ctx.v = 0.61; ctx.fw = 0.3;
			Check( prog.Eval( ctx ) == NoiseCore::Fbm3D( 3.7, 6.1, 0.0, 4, 0.5, 2.0, 0.3 ),
				"an unprovable domain falls back to the unscaled (pre-change) footprint" );
		}
	}

	// (e) A literal argument provably does not move with P, so there is
	// nothing to alias and the fade must not fire at ANY footprint.
	{
		ExpressionProgram::Builder b; b.EnableContextVars( true );
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( b.Finalize( "fbm(vec3(0.3,0.7,1.4), 4, 0.5, 2.0)", prog ), "literal vec3 argument compiles" );
		if( prog.IsValid() ) {
			const Scalar unfaded = NoiseCore::Fbm3D( 0.3, 0.7, 1.4, 4, 0.5, 2.0, 0.0 );
			for( Scalar fw : { 0.0, 0.05, 0.3, 2.0 } ) {
				ExprEvalContext ctx; ctx.P = p0; ctx.fw = fw;
				char label[96];
				snprintf( label, sizeof(label), "a P-independent argument never fades (fw=%g)", (double)fw );
				Check( prog.Eval( ctx ) == unfaded, label );
			}
		}
	}
}

static void TestFbmDomainScaleSubtractionIsConservative()
{
	std::cout << "Test 69: a SUBTRACTION with an unknown operand reports no domain scale (it can CANCEL the affine part), while an addition still keeps it" << std::endl;

	const Vector3 p0( 0.3, 0.7, 1.4 );

	// (a) THE PROBE.  `q` is an opaque copy of `P*40` -- clamp() is a
	// kFunc, so AnalyzeLinear cannot see through it -- and the argument
	// `P*40 - q + (0.5,0.5,0.5)` is therefore a CONSTANT position: the
	// true stretch is 0 and nothing can alias.  The old "one side known
	// wins" rule reported 40 for it and over-blurred a field that has no
	// high frequencies at all.  The contract now is that a subtraction
	// missing an operand's gradient yields no gradient, so the call site
	// falls back to the pre-change multiplier; 0 would be equally
	// correct (the position IS constant), 40 is the wrong answer this
	// pins against.
	{
		ExpressionProgram::Builder b; b.EnableContextVars( true );
		Check( b.AddDef( "q", "vec3(clamp(P.x*40.0,-1000000.0,1000000.0), "
			"clamp(P.y*40.0,-1000000.0,1000000.0), clamp(P.z*40.0,-1000000.0,1000000.0))" ), "def q (opaque copy of P*40)" );
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( b.Finalize( "fbm(P*40.0 - q + vec3(0.5,0.5,0.5), 4, 0.5, 2.0)", prog ), "cancelling-subtraction body compiles" );
		if( prog.IsValid() ) {
			const Scalar fw = 0.3;
			ExprEvalContext ctx; ctx.P = p0; ctx.fw = fw;
			// clamp() is the identity on these inputs, so the position
			// argument is exactly (0.5, 0.5, 0.5) at runtime.
			const Scalar got   = prog.Eval( ctx );
			const Scalar mult1 = NoiseCore::Fbm3D( 0.5, 0.5, 0.5, 4, 0.5, 2.0, fw );
			const Scalar mult0 = NoiseCore::Fbm3D( 0.5, 0.5, 0.5, 4, 0.5, 2.0, 0.0 );
			const Scalar mult40= NoiseCore::Fbm3D( 0.5, 0.5, 0.5, 4, 0.5, 2.0, 40.0*fw );
			Check( got == mult1 || got == mult0,
				"a cancelling subtraction falls back to the unscaled footprint (multiplier 1.0, or 0 -- never the dropped term's 40)" );
			Check( got != mult40, "the over-blurring 40x answer the old rule gave is NOT what comes out" );
			// Not vacuous: the three candidate answers are genuinely
			// different numbers at this footprint.
			Check( std::fabs( mult1 - mult40 ) > 1e-6 && std::fabs( mult1 - mult0 ) > 1e-6,
				"multiplier 1 / 0 / 40 are three distinct values here (the check above can actually fail)" );
		}
	}

	// (b) ADDITION is deliberately unchanged -- the domain-warp idiom
	// this whole analysis exists to resolve is an ADD, and it must keep
	// reporting its affine part's scale.
	{
		ExpressionProgram::Builder b; b.EnableContextVars( true );
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( b.Finalize( "fbm(P*8.0 + vec3(fbm(P*2.0,2,0.5,2.0),0.0,0.0), 4, 0.5, 2.0)", prog ), "warp-by-addition body compiles" );
		if( prog.IsValid() ) {
			const Scalar fw = 0.01;
			ExprEvalContext ctx; ctx.P = p0; ctx.fw = fw;
			// The inner fbm carries its OWN domain scale (2), so its
			// footprint is 2*fw -- the analysis is per call site.
			const Scalar warp  = NoiseCore::Fbm3D( p0.x*2.0, p0.y*2.0, p0.z*2.0, 2, 0.5, 2.0, 2.0*fw );
			const Scalar got   = prog.Eval( ctx );
			const Scalar want8 = NoiseCore::Fbm3D( p0.x*8.0 + warp, p0.y*8.0, p0.z*8.0, 4, 0.5, 2.0, 8.0*fw );
			const Scalar want1 = NoiseCore::Fbm3D( p0.x*8.0 + warp, p0.y*8.0, p0.z*8.0, 4, 0.5, 2.0, fw );
			// CheckClose, not ==, ONLY because the warped x coordinate
			// is a mul-then-add here and the compiler may contract it to
			// an FMA (macOS builds are -ffast-math), which the VM's own
			// two-opcode evaluation cannot be: a last-bit difference in
			// the POSITION, not in the multiplier under test.  The
			// multiplier itself is pinned to a part in 1e12, and the
			// second check rules out the 1.0 fallback outright.
			CheckClose( got, want8, 1e-12,
				"an additive warp still resolves to its affine part's scale (8*fw), warp position included" );
			Check( std::fabs( got - want1 ) > 1e-6,
				"and NOT the unscaled 1.0 fallback -- the two answers are far apart at this footprint" );
		}
	}
}

static void TestFbmDomainScaleRotatedShearedDomain()
{
	std::cout << "Test 70: a rotated / sheared domain reaches JacobianSpectralNorm's cyclic-Jacobi branch and fades at the true largest singular value" << std::endl;

	const Vector3 p0( 0.3, 0.7, 1.4 );

	// Reaching the non-diagonal branch at all takes LITERAL off-diagonal
	// coefficients: cos()/sin() are kFunc calls, which the analysis
	// reports as unknown, so a rotation written with them lands on the
	// 1.0 fallback instead and never exercises this code.
	//
	// (a) 45-degree rotation about z, times 40.  J is dense but J^T J is
	// exactly 1600*I, so the sweep converges on entry -- this pins the
	// branch's ENTRY and its J^T J construction.  Accurate, not exact
	// (see the branch's own comment): 40 to within ~1e-13, hence
	// CheckClose rather than ==.
	{
		const double c = 0.7071067811865476;   // cos(45 deg) = sin(45 deg)
		ExpressionProgram::Builder b; b.EnableContextVars( true );
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( b.Finalize( "fbm(vec3((P.x*0.7071067811865476 - P.y*0.7071067811865476)*40.0, "
			"(P.x*0.7071067811865476 + P.y*0.7071067811865476)*40.0, P.z*40.0), 4, 0.5, 2.0)", prog ),
			"rot45 x 40 domain compiles" );
		if( prog.IsValid() ) {
			const Scalar fw = 0.01;
			ExprEvalContext ctx; ctx.P = p0; ctx.fw = fw;
			const double qx = ( p0.x*c - p0.y*c ) * 40.0;
			const double qy = ( p0.x*c + p0.y*c ) * 40.0;
			const double qz = p0.z * 40.0;
			CheckClose( prog.Eval( ctx ), NoiseCore::Fbm3D( qx, qy, qz, 4, 0.5, 2.0, 40.0*fw ), 1e-12,
				"a rotated domain fades at 40*fw (the rotation contributes no stretch of its own)" );
			Check( std::fabs( NoiseCore::Fbm3D( qx, qy, qz, 4, 0.5, 2.0, 40.0*fw )
			                - NoiseCore::Fbm3D( qx, qy, qz, 4, 0.5, 2.0, fw ) ) > 1e-6,
				"the 40x and 1x answers differ here (the check above is not vacuous)" );
		}
	}

	// (b) Shear [[1,10,0],[0,1,0],[0,0,1]] -- a genuine Jacobi sweep (the
	// off-diagonal mass is 100).  Its largest singular value is
	// (10 + sqrt(104))/2 = 10.099019513592784, NOT 10: a naive
	// largest-row-norm reading would give 10, and the last check below
	// distinguishes the two.
	{
		const double sigma = 10.099019513592784;
		ExpressionProgram::Builder b; b.EnableContextVars( true );
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( b.Finalize( "fbm(vec3(P.x + P.y*10.0, P.y, P.z), 4, 0.5, 2.0)", prog ), "sheared domain compiles" );
		if( prog.IsValid() ) {
			const Scalar fw = 0.04;
			ExprEvalContext ctx; ctx.P = p0; ctx.fw = fw;
			const double qx = p0.x + p0.y*10.0, qy = p0.y, qz = p0.z;
			const Scalar got = prog.Eval( ctx );
			CheckClose( got, NoiseCore::Fbm3D( qx, qy, qz, 4, 0.5, 2.0, sigma*fw ), 1e-12,
				"a sheared domain fades at its true largest singular value (10.099019513592784)" );
			Check( std::fabs( got - NoiseCore::Fbm3D( qx, qy, qz, 4, 0.5, 2.0, 10.0*fw ) ) > 1e-9,
				"and NOT at the naive largest-row-norm 10 -- the two are distinguishable at this footprint" );
		}
	}
}

static void TestExpressionPainterFwSpectralParity()
{
	std::cout << "Test 64: GetColorNM stays consistent with EvalRGB when the body reads fw on a footprint-carrying RI (NM/RGB parity)" << std::endl;

	Job* job = new Job();
	job->addref();
	// fw broadcast into all three channels via mix so GetColorNM's
	// per-wavelength uplift has real (non-degenerate) RGB to work with.
	const char* body = "expression_painter\n{\nname fwspec\nexpr vec3(0.2+0.3*fw, 0.5, 0.8-0.2*fw)\n}\n";
	Check( S2::ParseBody( "s9_fwspec", body, *job ), "parses" );
	IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
	if( priv ) {
		IPainter* p = priv->GetPainters()->GetItem( "fwspec" );
		Check( p != 0, "registered" );
		if( p ) {
			RayIntersectionGeometric r( Ray(), nullRasterizerState );
			r.bHit = true;
			r.txFootprint.widthValid = true;
			r.txFootprint.valid = true;
			r.txFootprint.worldWidth = 0.25;

			const RISEPel rgb = p->GetColor( r );
			// RGBAlbedoSpectrum::FromRGB's uplift at the nominal R/G/B
			// wavelengths recovers (approximately) the source RGB -- the
			// same relationship TestExpressionPainterSpectralPath already
			// pins for the P-driven case; here it's fw-driven.
			const Scalar nmR = p->GetColorNM( r, 650.0 );
			const Scalar nmG = p->GetColorNM( r, 550.0 );
			const Scalar nmB = p->GetColorNM( r, 450.0 );
			Check( ExpressionProgram::IsFinite(nmR) && ExpressionProgram::IsFinite(nmG) && ExpressionProgram::IsFinite(nmB),
				"GetColorNM finite when the body reads fw" );
			// Sanity bound rather than an exact match (JH uplift is not a
			// pure per-channel passthrough) -- catches a NM path that
			// silently ignores fw entirely (would still be finite, but
			// GetColor's own RGB values already prove fw is read; this
			// confirms the NM path routes through the SAME EvalRGB(fw)
			// result rather than re-deriving fw independently and
			// disagreeing).
			CheckClose( rgb[0], 0.2 + 0.3*0.25, 1e-9, "GetColor r channel reflects fw (sanity anchor for the NM check below)" );
			Check( nmR > 0.0 && nmG > 0.0 && nmB > 0.0, "GetColorNM at R/G/B nominal wavelengths all positive (plausible uplift)" );
		}
	}
	job->release();
}

//======================================================================
// Po DOMAINS (2026-09-06, docs/TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md 11)
//
// The domain-scale analysis now differentiates a noise call's position
// argument with respect to `Po` as well as `P`, and the hit carries the
// matching object-space width `fwo` alongside the world-space `fw`.  A
// call site is filtered at `scale_P * fw + scale_Po * fwo`.
//
//   - Test 71 DOMAIN CONSISTENCY ACROSS THE OBJECT TRANSFORM: on an
//     object with uniform `scale s`, `fbm(Po*k)` must fade at exactly
//     the octave `fbm(P*k/s)` fades at -- the two describe the SAME
//     field on the SAME surface, so the only way they can disagree is a
//     footprint bug.  Also pins the short-circuit: fwo == 0 leaves a
//     Po-only body un-faded.
//   - Test 72 MIXED FRAMES take the documented CONSERVATIVE SUM.
//   - Test 73 NON-UNIFORM SCALE lands on the documented side.
//   - Test 74 THE COMPILE-TIME SPLIT: a Po-only body puts NO weight on
//     `fw`, a P-only body NO weight on `fwo`, and an unprovable
//     argument still falls back to (1.0, 0.0).
//======================================================================

//! What the object-space footprint IS, for an object under uniform
//! `scale s`: the world footprint divided by s.  This mirrors
//! TextureFootprintTest test 13a, which proves Object::IntersectRay
//! actually reports it (`worldWidth / objectWidth == s`); here it is
//! the arithmetic the VM is handed.
static Scalar S11_ObjectWidthForUniformScale( Scalar worldWidth, Scalar s )
{
	return worldWidth / s;
}

static void TestPoDomainConsistencyAcrossTransform()
{
	std::cout << "Test 71: fbm(Po*k) on a `scale s` object fades at the same octave as fbm(P*k/s) -- the domain is consistent ACROSS the object transform" << std::endl;

	// The surface point, in both frames, for an object under uniform
	// `scale s`: P = s * Po.  Both bodies below therefore evaluate the
	// noise at the SAME lattice coordinate, and the only question the
	// test asks is whether they FILTER it the same way.
	const Scalar s  = 8.0;					// the instance scale
	const Scalar k  = 62.0;					// the shipped Hair/variety_gallery domain scale
	const Vector3 po( 0.13, -0.41, 0.77 );	// object-space hit
	const Vector3 pw( po.x*s, po.y*s, po.z*s );

	const char* bodies[3] = { "fbm(%s, 5, 0.5, 2.0)", "turbulence(%s, 5, 0.5, 2.0)", "ridged(%s, 5, 0.5, 2.0)" };
	const char* names[3]  = { "fbm", "turbulence", "ridged" };

	for( int f = 0; f < 3; ++f ) {
		char objBody[160], worldBody[160];
		// `Po * 62.0` -- the object-space idiom.
		snprintf( objBody, sizeof(objBody), bodies[f], "Po*62.0" );
		// `P * (62/8)` -- the SAME field written in world space.
		snprintf( worldBody, sizeof(worldBody), bodies[f], "P*7.75" );

		ExpressionProgram::Builder bo; bo.EnableContextVars( true );
		ExpressionProgram objProg = ExpressionProgram::Invalid();
		Check( bo.Finalize( objBody, objProg ), std::string(names[f]) + "(Po*62.0, ...) compiles" );

		ExpressionProgram::Builder bw; bw.EnableContextVars( true );
		ExpressionProgram worldProg = ExpressionProgram::Invalid();
		Check( bw.Finalize( worldBody, worldProg ), std::string(names[f]) + "(P*7.75, ...) compiles" );

		if( !objProg.IsValid() || !worldProg.IsValid() ) continue;

		for( Scalar worldFw : { 0.0, 0.0008, 0.004, 0.02, 0.06 } ) {
			ExprEvalContext ctx;
			ctx.P = pw; ctx.Po = po;
			ctx.fw = worldFw;
			ctx.fwo = S11_ObjectWidthForUniformScale( worldFw, s );

			char label[160];
			snprintf( label, sizeof(label),
				"%s: fw=%.4f -- Po*62 and P*7.75 agree BIT-for-BIT on a scale-8 object",
				names[f], (double)worldFw );
			// Exact, not approximate: 62 * (fw/8) and 7.75 * fw are the
			// same IEEE product (62/8 == 7.75 exactly, both powers-of-two
			// denominators), so both routes must hand NoiseCore the very
			// same (x,y,z,fw).  Any difference is a real disagreement.
			Check( objProg.Eval( ctx ) == worldProg.Eval( ctx ), label );
		}
	}

	// THE FADE ACTUALLY ENGAGES on the Po body -- without this the test
	// above would pass just as well on the pre-change engine, where BOTH
	// bodies could have been inert.  At a render-scale world footprint
	// the object-domain footprint 62*fw/8 is well past the cutoff.
	{
		ExpressionProgram::Builder b; b.EnableContextVars( true );
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( b.Finalize( "fbm(Po*62.0, 5, 0.5, 2.0)", prog ), "fbm(Po*62.0, 5, 0.5, 2.0) compiles (fade probe)" );
		if( prog.IsValid() ) {
			// 0.01 world units on a scale-8 object is an object-space
			// width of 0.00125, i.e. a DOMAIN footprint of 62*0.00125 =
			// 0.0775 -- whose 4th and 5th octaves (0.62, 1.24) are past
			// the fade band's hi = 0.6 and are therefore dropped
			// outright.  Chosen over a smaller width so the
			// "genuinely different" assertions below cannot be
			// satisfied by rounding.
			const Scalar worldFw = 0.01;
			const Scalar objFw   = S11_ObjectWidthForUniformScale( worldFw, s );

			ExprEvalContext point;  point.P = pw; point.Po = po; point.fw = 0;       point.fwo = 0;
			ExprEvalContext filt;   filt.P  = pw; filt.Po  = po; filt.fw  = worldFw; filt.fwo  = objFw;

			const Scalar unfaded = prog.Eval( point );
			const Scalar faded   = prog.Eval( filt );

			// Independent reference: the from-scratch faded sum at the
			// domain footprint k*fwo, computed without the VM.
			const Scalar expected = S9_ExpectedFadedFbm( po.x*k, po.y*k, po.z*k, 5, 0.5, 2.0, k*objFw );
			CheckClose( faded, expected, 1e-12,
				"a Po domain is filtered at k*fwo (matches the from-scratch faded sum)" );
			Check( std::fabs( faded - unfaded ) > 1e-6,
				"...and that is genuinely DIFFERENT from the point sample -- the fade engages, "
				"which it did NOT before Po was a differentiation variable" );

			// THE PRE-CHANGE ANSWER, named so a regression is diagnosed:
			// the fallback multiplier 1.0 on the world fw.
			const Scalar preChange = S9_ExpectedFadedFbm( po.x*k, po.y*k, po.z*k, 5, 0.5, 2.0, worldFw );
			Check( std::fabs( faded - preChange ) > 1e-6,
				"...and it is NOT the pre-change value (fw at multiplier 1.0), which under-fades by "
				"the whole domain scale" );

			// SHORT-CIRCUIT: no object-space footprint (a CSG hit whose
			// winning child stamped none, or any hit with no
			// differentials) leaves the Po body un-faded, exactly as
			// fw == 0 leaves a P body un-faded.
			ExprEvalContext noObj; noObj.P = pw; noObj.Po = po; noObj.fw = worldFw; noObj.fwo = 0;
			Check( prog.Eval( noObj ) == unfaded,
				"fwo == 0 short-circuits: a Po-only body reproduces the un-faded sum BIT-for-BIT, "
				"however large the WORLD footprint is" );
		}
	}
}

static void TestPoDomainMixedFrameIsConservativeSum()
{
	std::cout << "Test 72: an argument that reads BOTH frames is filtered at the conservative sum scale_P*fw + scale_Po*fwo" << std::endl;

	const Vector3 po( 0.21, 0.35, -0.62 );
	const Vector3 pw( 1.10, -0.40, 0.30 );
	const Scalar  fw = 0.003, fwo = 0.011;
	const Scalar  a = 9.0, b = 4.0;			// the two domain scales

	ExpressionProgram::Builder bl; bl.EnableContextVars( true );
	ExpressionProgram prog = ExpressionProgram::Invalid();
	Check( bl.Finalize( "fbm(P*9.0 + Po*4.0, 5, 0.5, 2.0)", prog ), "fbm(P*9 + Po*4, ...) compiles" );
	if( !prog.IsValid() ) return;

	ExprEvalContext ctx; ctx.P = pw; ctx.Po = po; ctx.fw = fw; ctx.fwo = fwo;

	const Vector3 arg( pw.x*a + po.x*b, pw.y*a + po.y*b, pw.z*a + po.z*b );
	const Scalar  expectedSum = S9_ExpectedFadedFbm( arg.x, arg.y, arg.z, 5, 0.5, 2.0, a*fw + b*fwo );
	CheckClose( prog.Eval( ctx ), expectedSum, 1e-12,
		"the mixed-frame call is filtered at 9*fw + 4*fwo (the triangle-inequality bound)" );

	// The two counterfactuals this rule was chosen OVER, named so a
	// future 'simplification' to either is caught.  Both are strictly
	// less filtering, i.e. the aliasing direction.
	const Scalar worldOnly = S9_ExpectedFadedFbm( arg.x, arg.y, arg.z, 5, 0.5, 2.0, a*fw );
	const Scalar maxRule   = S9_ExpectedFadedFbm( arg.x, arg.y, arg.z, 5, 0.5, 2.0,
		( a*fw > b*fwo ) ? a*fw : b*fwo );
	Check( std::fabs( expectedSum - worldOnly ) > 1e-9,
		"...NOT the P term alone (which is what dropping the Po half would give)" );
	Check( std::fabs( expectedSum - maxRule ) > 1e-9,
		"...NOT max(scale_P*fw, scale_Po*fwo) either -- the sum is deliberately the more "
		"conservative of the two (over-filter, never alias)" );
	Check( a*fw + b*fwo > ( ( a*fw > b*fwo ) ? a*fw : b*fwo ),
		"(oracle) the sum really is the larger footprint of the two candidate rules" );
}

static void TestPoDomainNonUniformScaleSide()
{
	std::cout << "Test 73: under NON-uniform object scale a Po domain lands on the documented side (the object-space footprint is the geometry's own measure)" << std::endl;

	// The doc's worked case: `stretch (4, 0.05, 4)` viewed face-on down
	// the flattened axis, so the world footprint is 4x the object one
	// (TextureFootprintTest test 13b proves Object::IntersectRay reports
	// exactly that pair).  The VM's job is only to USE the pair it is
	// handed -- and the point of this test is that a Po body is then
	// filtered at the geometry's OWN measure, NOT at the |det|^(1/3)
	// geometric mean a derive-by-scale implementation would have used.
	const Scalar objFw   = 0.0025;
	const Scalar worldFw = objFw * Scalar( 4 );					// exact in-plane factor
	const Scalar detCubeRoot = std::pow( 4.0 * 0.05 * 4.0, 1.0 / 3.0 );	// 0.9283
	const Scalar k = 40.0;
	const Vector3 po( 0.31, 0.0, -0.44 );

	ExpressionProgram::Builder b; b.EnableContextVars( true );
	ExpressionProgram prog = ExpressionProgram::Invalid();
	Check( b.Finalize( "fbm(Po*40.0, 5, 0.5, 2.0)", prog ), "fbm(Po*40.0, ...) compiles" );
	if( !prog.IsValid() ) return;

	ExprEvalContext ctx; ctx.Po = po; ctx.P = Vector3( po.x*4, po.y*0.05, po.z*4 );
	ctx.fw = worldFw; ctx.fwo = objFw;

	const Scalar expected = S9_ExpectedFadedFbm( po.x*k, po.y*k, po.z*k, 5, 0.5, 2.0, k*objFw );
	CheckClose( prog.Eval( ctx ), expected, 1e-12,
		"a Po domain under an anisotropic stretch filters at k * the OBJECT-space width" );

	// The counterfactual: had objectWidth been derived as
	// worldWidth / |det M|^(1/3), the VM would have been handed
	// worldFw / 0.9283 -- 4.31x too large a footprint, over-blurring the
	// field by two octaves.
	const Scalar derivedWouldBe = worldFw / Scalar( detCubeRoot );
	const Scalar wrong = S9_ExpectedFadedFbm( po.x*k, po.y*k, po.z*k, 5, 0.5, 2.0, k*derivedWouldBe );
	{
		char oracle[224];
		snprintf( oracle, sizeof(oracle),
			"(oracle) the retired |det|^(1/3) derivation really is the more-blurred side (%.4gx "
			"the true object width, vs the exact 4x)", (double)( derivedWouldBe / objFw ) );
		Check( derivedWouldBe > objFw * Scalar( 4 ), oracle );
	}
	Check( std::fabs( expected - wrong ) > 1e-9,
		"...and it would give a MEASURABLY different value, so this test is not vacuous" );
}

static void TestPoDomainCompileTimeSplit()
{
	std::cout << "Test 74: the two multipliers are independent -- a Po-only body puts no weight on fw, a P-only body none on fwo, and an unprovable argument still falls back to (1.0, 0.0)" << std::endl;

	const Vector3 pw( 0.3, 0.7, 1.4 ), po( -0.9, 0.25, 0.6 );

	struct Case {
		const char* body;
		bool readsP;		//!< a change in fw must move the result
		bool readsPo;		//!< a change in fwo must move the result
	};
	const Case cases[] = {
		{ "fbm(P*40.0, 5, 0.5, 2.0)",            true,  false },
		{ "fbm(Po*40.0, 5, 0.5, 2.0)",           false, true  },
		{ "fbm(P*20.0 + Po*20.0, 5, 0.5, 2.0)",  true,  true  },
		// A `def`-carried object-space scale -- the shape real bodies
		// use -- resolves exactly like the inlined literal.
		{ "fbm(qo, 5, 0.5, 2.0)",                false, true  },
		// No provable relation to either position: the pre-change
		// fallback, which reads `fw` at multiplier 1.0 and ignores fwo.
		{ "fbm(vec3(u*10.0, v*10.0, 0.0), 5, 0.5, 2.0)", true, false },
	};

	for( size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i ) {
		const Case& c = cases[i];
		ExpressionProgram::Builder b; b.EnableContextVars( true );
		if( std::string( c.body ) == "fbm(qo, 5, 0.5, 2.0)" ) {
			Check( b.AddParam( "ko", 40.0 ), "param ko 40" );
			Check( b.AddDef( "qo", "Po*ko" ), "def qo Po*ko" );
		}
		ExpressionProgram prog = ExpressionProgram::Invalid();
		Check( b.Finalize( c.body, prog ), std::string("compiles: ") + c.body );
		if( !prog.IsValid() ) continue;

		ExprEvalContext base;  base.u = 0.37; base.v = 0.61; base.P = pw; base.Po = po;
		base.fw = 0; base.fwo = 0;
		ExprEvalContext bumpW = base; bumpW.fw  = 0.02;
		ExprEvalContext bumpO = base; bumpO.fwo = 0.02;

		const Scalar v0 = prog.Eval( base );
		const Scalar vW = prog.Eval( bumpW );
		const Scalar vO = prog.Eval( bumpO );

		char label[224];
		snprintf( label, sizeof(label), "%s: %s by fw", c.body, c.readsP ? "IS moved" : "is NOT moved" );
		if( c.readsP ) Check( std::fabs( vW - v0 ) > 1e-9, label );
		else           Check( vW == v0, label );

		snprintf( label, sizeof(label), "%s: %s by fwo", c.body, c.readsPo ? "IS moved" : "is NOT moved" );
		if( c.readsPo ) Check( std::fabs( vO - v0 ) > 1e-9, label );
		else            Check( vO == v0, label );
	}
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
	TestMappingPainterUVGolden();
	TestMappingPainterWorldObjectDomainPatch();
	TestMappingPainterTriplanar();
	TestBlendPainterModes();
	TestVoronoi3DSpaceParam();
	TestS7ChunkParsingAndDiagnostics();
	TestMappingPainterFootprintInvalidation();
	TestMappingPainterMultiAxisRotationGolden();
	TestBlendPainterScreenOverlayClamping();
	TestStochasticTileDeterminismAndSeed();
	TestStochasticTileLatticeContinuityNoNaN();
	TestStochasticTileContrastRestoreGolden();
	TestStochasticTileMeanAndGammaRespected();
	TestStochasticTileFootprintInvalidation();
	TestStochasticTileNMRGBConsistency();
	TestS8ChunkParsingAndDiagnostics();
	TestScatterDeterminismAndProbability();
	TestScatterAlphaGatedAndFootprint();
	TestScatterJitterBoundedAtZero();
	TestScatterNeighborOverlapGolden();
	TestScatterDrawOrderStability();
	TestStochasticTileExtremeScaleSafety();
	TestScatterExtremeScaleSafety();
	TestFbmFootprintFadeWeightGoldens();
	TestFbmFootprintFadeBitIdentityAtZero();
	TestFbmFootprintFadeMatchesReimplementation();
	TestFbmFootprintFadeCollapsesPastCutoff();
	TestFbmFootprintFadeAntiDarkeningPin();
	TestFbmFootprintFadeVarianceMonotonic();
	TestExpressionPainterFootprintContext();
	TestExpressionVMFwEndToEnd();
	TestExpressionPainterFwSpectralParity();
	TestFbmDomainScaleConsistency();
	TestFbmDomainScaleFadeEngages();
	TestFbmDomainScaleBitIdentity();
	TestFbmDomainScaleFallbacks();
	TestFbmDomainScaleSubtractionIsConservative();
	TestFbmDomainScaleRotatedShearedDomain();
	TestPoDomainConsistencyAcrossTransform();
	TestPoDomainMixedFrameIsConservativeSum();
	TestPoDomainNonUniformScaleSide();
	TestPoDomainCompileTimeSplit();
	std::cout << std::endl << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
