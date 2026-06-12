//////////////////////////////////////////////////////////////////////
//
//  ExpressionFunction2DTest.cpp - the in-scene math-expression engine
//  (ExpressionEval / expression_function2d).
//
//    1. Engine correctness: operators, precedence, functions, params,
//       let-bindings (defs), and the parse-error paths.
//    2. Proof of generality: the UNIFORM guilloché field authored as a
//       scene EXPRESSION reproduces GuillocheField::Height (the retired
//       C++ field) to golden precision -- i.e. the patterning really can
//       move out of C++ into the scene file.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include "../src/Library/Painters/ExpressionEval.h"
#include "../src/Library/Painters/GuillochePainter.h"		// GuillocheField (the reference)

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

// The uniform guilloché field as an in-scene expression (the same param/def
// chain authored in scenes), built here to validate it == GuillocheField.
static ExpressionProgram BuildUniformGuilloche()
{
	ExpressionProgram p = ExpressionProgram::Invalid();
	ExpressionProgram::Builder b;
	b.AddParam( "R", 20.6 );          b.AddParam( "arms", 12 );      b.AddParam( "swirl", 0 );
	b.AddParam( "seamJag", 0.16 );    b.AddParam( "seamJagFreq", 3.0 );
	b.AddParam( "cell", 0.9 );        b.AddParam( "gridAmp", 0.85 ); b.AddParam( "petalAmp", 0.30 );
	b.AddParam( "gridE0", 0.12 );     b.AddParam( "gridE1", 0.5 );
	b.AddParam( "petalE0", 0.0 );     b.AddParam( "petalE1", 0.82 );
	b.AddParam( "base", 0.15 );       b.AddParam( "landLevel", 0.45 );
	b.AddParam( "reliefDepth", 0.85 );b.AddParam( "centerRadius", 0.03 );
	b.AddDef( "x", "(2*u-1)*R" );     b.AddDef( "y", "(2*v-1)*R" );
	b.AddDef( "r", "hypot(x,y)" );    b.AddDef( "rho", "clamp(r/R,0,1)" );
	b.AddDef( "theta", "atan2(y,x)" );
	b.AddDef( "jagf", "frac(seamJagFreq*rho)" );
	b.AddDef( "jag", "seamJag*(2*abs(2*jagf-1)-1)" );
	b.AddDef( "psi", "theta+swirl*rho+jag" );
	b.AddDef( "petalc", "abs(cos(arms*psi))" );
	b.AddDef( "petal", "smoothstep(petalE0,petalE1,petalc)" );
	b.AddDef( "wsec", "tau/arms" );   b.AddDef( "q", "psi/wsec" );
	b.AddDef( "sector", "sign(q)*floor(abs(q)+0.5)" );
	b.AddDef( "thetaC", "sector*wsec-swirl*rho-jag" );
	b.AddDef( "cc", "cos(thetaC)" );  b.AddDef( "ss", "sin(thetaC)" );
	b.AddDef( "xr", "cc*x+ss*y" );    b.AddDef( "yr", "-ss*x+cc*y" );
	b.AddDef( "freq", "0.5/cell" );
	b.AddDef( "ax", "freq*xr" );      b.AddDef( "ay0", "freq*yr" );
	b.AddDef( "rowpar", "floor(2*ax)" );
	b.AddDef( "ay", "ay0+0.25*mod(rowpar,2)" );
	b.AddDef( "stripex", "smoothstep(gridE0,gridE1,abs(cos(tau*ax)))" );
	b.AddDef( "stripey", "smoothstep(gridE0,gridE1,abs(cos(tau*ay)))" );
	b.AddDef( "grid", "stripex*stripey" );
	b.AddDef( "raw", "base+petalAmp*petal+gridAmp*grid" );
	b.AddDef( "h0", "clamp((raw-base)/(petalAmp+gridAmp),0,1)" );
	b.AddDef( "h1", "pow(h0,log(landLevel)/log(0.5))" );
	b.AddDef( "h2", "0.5+(h1-0.5)*reliefDepth" );
	b.AddDef( "hub", "0.5*(1-reliefDepth)" );
	b.AddDef( "rin", "max(centerRadius*R,0.000001)" );
	b.AddDef( "whub", "clamp(r/rin,0,1)" );
	b.AddDef( "hfin", "(1-whub*whub)*hub+whub*whub*h2" );
	b.Finalize( "clamp(hfin,0,1)", p );
	return p;
}

static void TestGuillocheEquivalence()
{
	std::cout << "Test 2: uniform guilloché AS A SCENE EXPRESSION == GuillocheField::Height (goldens)" << std::endl;
	const ExpressionProgram prog = BuildUniformGuilloche();
	Check( prog.IsValid(), "uniform guilloché expression compiles" );
	const GuillocheField field( GuillocheParamsFromDescriptor( GuillocheDiskDescriptor() ) );
	const Scalar R = 20.6;
	const Scalar PTS[8][2] = {
		{ 0.31, 0.17 }, { 3.2, 1.1 }, { -5.7, 8.3 }, { 12.4, -3.9 },
		{ -15.2, -9.8 }, { 0.0, 18.9 }, { 7.07, 7.07 }, { -19.5, 2.0 }
	};
	char label[64];
	for( int i = 0; i < 8; ++i ) {
		const Scalar x = PTS[i][0], y = PTS[i][1];
		const Scalar u = ( x + R ) / ( 2 * R );
		const Scalar v = ( y + R ) / ( 2 * R );
		snprintf( label, sizeof(label), "expr == Height pt%d", i );
		// the scene expression must reproduce the C++ field to tight tolerance
		CheckClose( prog.Eval( u, v ), field.Height( x, y ), Scalar(1e-6), label );
	}
}

int main( int, char** )
{
	std::cout << "ExpressionFunction2DTest -- in-scene math-expression field engine" << std::endl << std::endl;
	TestEngine();
	TestGuillocheEquivalence();
	std::cout << std::endl << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
