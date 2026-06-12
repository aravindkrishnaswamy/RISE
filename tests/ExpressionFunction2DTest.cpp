//////////////////////////////////////////////////////////////////////
//
//  ExpressionFunction2DTest.cpp - the in-scene math-expression engine
//  (ExpressionEval / expression_function2d).
//
//    1. Engine correctness: operators, precedence, functions, params,
//       let-bindings (defs), and the parse-error paths.
//    2. Proof of generality: ALL SIX guilloché dial patterns authored as
//       scene EXPRESSIONS reproduce GuillocheField::Height (the retired
//       C++ relief field) to golden precision -- i.e. the entire dial
//       pattern library moves out of C++ into the scene file.
//
//  The six Build* functions below ARE the canonical scene expressions
//  (the watch's dial library, scenes/FeatureBased/GuillocheWatch).  The
//  goldens are GuillocheField::Height at eight dial points with the exact
//  blessed per-pattern parameters from watch_dial.RISEscene.
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
//  Validate each scene expression == the retired GuillocheField::Height.
//
//  These goldens were captured from GuillocheField::Height (the C++ relief
//  field, now removed) at eight dial points with the exact blessed
//  per-pattern parameters from watch_dial.RISEscene.  The scene
//  expressions above reproduce them to 1e-6 -- the proof that the entire
//  dial pattern library lives in the scene file, not in C++.  To
//  regenerate after a deliberate field change, see the dump harness in
//  the commit that introduced this test.
//====================================================================

static const Scalar kR = 20.6;
static const Scalar kPTS[8][2] = {
	{ 0.31, 0.17 }, { 3.2, 1.1 }, { -5.7, 8.3 }, { 12.4, -3.9 },
	{ -15.2, -9.8 }, { 0.0, 18.9 }, { 7.07, 7.07 }, { -19.5, 2.0 }
};

// GuillocheField::Height goldens, one row of 8 per pattern.  Row order
// follows the ValidatePattern() call order below (uniform, radial,
// lightning, iris, swirl, varwidth) -- NOT the GuillocheParams::Pattern
// enum order (which swaps lightning/radial).
static const Scalar kGold[6][8] = {
	{ 0.20847977124, 0.84457644488, 0.919685621104, 0.925,
	  0.255774385448, 0.925, 0.675201634858, 0.925 },
	{ 0.214067284782, 0.746485235929, 0.761970231601, 0.925,
	  0.925, 0.255774385448, 0.675201634858, 0.382105955876 },
	{ 0.792073389645, 0.444937813954, 0.221172453769, 0.444937813954,
	  0.444937813954, 0.53975985465, 0.925, 0.39180309277 },
	{ 0.386690741972, 0.257496664918, 0.632657702808, 0.911528472042,
	  0.427947593856, 0.239217848864, 0.152535667977, 0.80483823114 },
	{ 0.636197449139, 0.0752886813544, 0.925, 0.075,
	  0.0780249280251, 0.0852142707664, 0.75298819258, 0.149854333332 },
	{ 0.700942595909, 0.925, 0.90244896724, 0.075,
	  0.0937768284557, 0.075, 0.359700879375, 0.075 }
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
	std::cout << "Test 2: all six guilloché dial patterns AS SCENE EXPRESSIONS == GuillocheField::Height goldens" << std::endl;
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
