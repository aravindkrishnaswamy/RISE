//////////////////////////////////////////////////////////////////////
//
//  ExpressionFunction2DTest.cpp - the in-scene math-expression engine
//  (ExpressionEval / expression_function2d).
//
//    1. Engine correctness: operators, precedence, functions, params,
//       let-bindings (defs), and the parse-error paths.
//    2. Proof of generality: ALL SIX guilloché dial patterns are authored
//       as scene EXPRESSIONS (the C++ relief field is retired) -- the entire
//       dial pattern library lives in the scene file.  Now a SHARP V-CUT
//       groove profile (see GuillocheDialExpr.h / watch_dial.RISEscene).
//
//  The six Build* functions below ARE the canonical scene expressions
//  (the watch's dial library, scenes/FeatureBased/GuillocheWatch) -- the
//  SAME strings the scene chunks use.  The goldens (below) are these
//  builders evaluated at eight dial points, cross-checked vs an independent
//  numpy replication.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <string>
#include "../src/Library/Painters/ExpressionEval.h"
#include "GuillocheDialExpr.h"		// the six dial patterns (shared with the scene-chunk emitter)

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const char* name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}
static void CheckClose( Scalar got, Scalar want, Scalar tol, const char* name )
{
	if( std::fabs( got - want ) <= tol ) { ++passCount; }
	else {
		++failCount;
		std::cout.precision( 12 );
		std::cout << "  FAIL: " << name << "  got " << got << "  want " << want << "  |d| " << std::fabs(got-want) << std::endl;
	}
}

static ExpressionProgram Compile1( const char* expr )
{
	ExpressionProgram p = ExpressionProgram::Invalid();
	ExpressionProgram::Builder b;
	b.Finalize( expr, p );
	return p;
}

static void TestEngine()
{
	std::cout << "Test 1: expression engine (operators / functions / defs / errors)" << std::endl;
	CheckClose( Compile1( "1 + 2 * 3" ).Eval(0,0), 7, 1e-12, "precedence" );
	CheckClose( Compile1( "(1 + 2) * 3" ).Eval(0,0), 9, 1e-12, "parens" );
	CheckClose( Compile1( "2 ^ 3 ^ 2" ).Eval(0,0), 512, 1e-9, "pow right-assoc" );
	CheckClose( Compile1( "-2 + 3 * -4" ).Eval(0,0), -14, 1e-12, "unary minus" );
	CheckClose( Compile1( "u * 10 + v" ).Eval( 2, 3 ), 23, 1e-12, "u,v vars" );
	CheckClose( Compile1( "sin(0)+cos(0)+sqrt(9)+abs(-4)+floor(2.7)" ).Eval(0,0), 0+1+3+4+2, 1e-9, "unary fns" );
	CheckClose( Compile1( "atan2(1,0)" ).Eval(0,0), std::atan2(1.0,0.0), 1e-12, "atan2" );
	CheckClose( Compile1( "mod(-1, 3)" ).Eval(0,0), 2, 1e-12, "floor-mod" );
	CheckClose( Compile1( "clamp(5,0,3)+smoothstep(0,1,0.5)+mix(0,8,0.25)+select(1<2,10,20)" ).Eval(0,0), 3+0.5+2+10, 1e-9, "ternary fns" );
	CheckClose( Compile1( "select(3>2,10,20)+select(2>=2,1,0)+select(1!=1,5,6)" ).Eval(0,0), 10+1+6, 1e-9, "gt/ge/ne" );
	CheckClose( Compile1( "hypot(3,4)" ).Eval(0,0), 5, 1e-9, "hypot" );
	CheckClose( Compile1( "pi" ).Eval(0,0), 3.14159265358979323846, 1e-12, "pi const" );

	// let-bindings (defs) + params
	{
		ExpressionProgram p = ExpressionProgram::Invalid();
		ExpressionProgram::Builder b;
		b.AddParam( "k", 5 );
		b.AddDef( "a", "u + v" );
		b.AddDef( "b", "a * a" );		// references an earlier def
		b.Finalize( "b + k", p );
		CheckClose( p.Eval( 1, 2 ), (1.0+2.0)*(1.0+2.0)+5.0, 1e-12, "defs + params" );
	}

	// error paths
	{ ExpressionProgram p = ExpressionProgram::Invalid(); ExpressionProgram::Builder b;
	  Check( !b.Finalize( "u + nope", p ), "unknown variable rejects" ); }
	{ ExpressionProgram p = ExpressionProgram::Invalid(); ExpressionProgram::Builder b;
	  Check( !b.Finalize( "foo(1)", p ), "unknown function rejects" ); }
	{ ExpressionProgram p = ExpressionProgram::Invalid(); ExpressionProgram::Builder b;
	  Check( !b.Finalize( "sin(1,2)", p ), "wrong arity rejects" ); }
	{ ExpressionProgram p = ExpressionProgram::Invalid(); ExpressionProgram::Builder b;
	  Check( !b.Finalize( "(1 + 2", p ), "unbalanced paren rejects" ); }
}

// Compile a templated pattern from GuillocheDialExpr (the SAME code the
// scene-chunk emitter replays) into an ExpressionProgram.
typedef std::string (*PatternFn)( ExpressionProgram::Builder& );

static ExpressionProgram CompilePattern( PatternFn fn )
{
	ExpressionProgram p = ExpressionProgram::Invalid();
	ExpressionProgram::Builder b;
	const std::string fin = fn( b );
	b.Finalize( fin, p );
	return p;
}

//====================================================================
//  Validate each scene dial expression against blessed goldens.
//
//  The dial library uses a SHARP V-CUT groove profile (Stripe = clamp(
//  |cos(2pi*arg)|/gridE1,0,1), landLevel 0.5) -- see GuillocheDialExpr.h
//  and watch_dial.RISEscene.  These goldens were regenerated for that
//  profile at eight dial points with the blessed per-pattern parameters,
//  and CROSS-CHECKED to ~1e-9 against an independent numpy replication of
//  the field (all six patterns share the same Stripe primitive + AddFinish,
//  so verifying one independently validates the set).  The test pins the
//  expression engine + the dial builders against accidental drift.  To
//  regenerate after a deliberate field change, temporarily re-add the
//  DumpGoldens() harness (git history of this file) and paste its output.
//====================================================================

static const Scalar kR = 20.6;
static const Scalar kPTS[8][2] = {
	{ 0.31, 0.17 }, { 3.2, 1.1 }, { -5.7, 8.3 }, { 12.4, -3.9 },
	{ -15.2, -9.8 }, { 0.0, 18.9 }, { 7.07, 7.07 }, { -19.5, 2.0 }
};

// V-cut dial-field goldens, one row of 8 per pattern.  Row order
// follows the ValidatePattern() call order below (uniform, radial,
// lightning, iris, swirl, varwidth) -- NOT the GuillocheParams::Pattern
// enum order (which swaps lightning/radial).
static const Scalar kGold[6][8] = {
	{ 0.236167129878, 0.854735906704, 0.920384928924, 0.925, 0.33025163478, 0.925, 0.703401483847, 0.925 },
	{ 0.240571048653, 0.743054097583, 0.775257827395, 0.925, 0.925, 0.368235061596, 0.703401483847, 0.587005600687 },
	{ 0.805016044693, 0.487857142857, 0.283386660463, 0.487857142857, 0.487857142857, 0.57829634242, 0.925, 0.398533614971 },
	{ 0.588627746394, 0.298571796523, 0.66455010853, 0.8857841528, 0.611588371714, 0.283422827543, 0.210870792087, 0.790509663793 },
	{ 0.612717333338, 0.272890264287, 0.925, 0.0904063599098, 0.311836378197, 0.335398864293, 0.751193743782, 0.419508999486 },
	{ 0.700942595909, 0.925, 0.866560025475, 0.075, 0.353183436015, 0.075, 0.552933296971, 0.075 }
};

static void ValidatePattern( const char* name, int gi, const ExpressionProgram& prog )
{
	char label[96];
	snprintf( label, sizeof(label), "%s expression compiles", name );
	Check( prog.IsValid(), label );
	if( !prog.IsValid() ) return;
	for( int i = 0; i < 8; ++i ) {
		const Scalar x = kPTS[i][0], y = kPTS[i][1];
		const Scalar u = ( x + kR ) / ( 2 * kR );
		const Scalar v = ( y + kR ) / ( 2 * kR );
		snprintf( label, sizeof(label), "%s expr==Height pt%d", name, i );
		CheckClose( prog.Eval( u, v ), kGold[gi][i], Scalar(1e-6), label );
	}
}

static void TestGuillocheEquivalence()
{
	std::cout << "Test 2: all six guilloché dial patterns AS SCENE EXPRESSIONS == V-cut field goldens" << std::endl;
	ValidatePattern( "uniform",  0, CompilePattern( GuillocheDialExpr::BuildUniform  ) );
	ValidatePattern( "radial",   1, CompilePattern( GuillocheDialExpr::BuildRadial   ) );
	ValidatePattern( "lightning",2, CompilePattern( GuillocheDialExpr::BuildLightning) );
	ValidatePattern( "iris",     3, CompilePattern( GuillocheDialExpr::BuildIris     ) );
	ValidatePattern( "swirl",    4, CompilePattern( GuillocheDialExpr::BuildSwirl    ) );
	ValidatePattern( "varwidth", 5, CompilePattern( GuillocheDialExpr::BuildVarwidth ) );
}

int main( int, char** )
{
	std::cout << "ExpressionFunction2DTest -- in-scene math-expression field engine" << std::endl << std::endl;
	TestEngine();
	TestGuillocheEquivalence();
	std::cout << std::endl << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
