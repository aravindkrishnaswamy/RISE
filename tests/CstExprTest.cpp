//////////////////////////////////////////////////////////////////////
//
//  CstExprTest.cpp - Facet 1 / #5 slice 2: the expr(...) value sublanguage (D4).
//  An `expr( <arithmetic> )` param value is EVALUATED at derive time (via the shared
//  ExpressionProgram) to a numeric literal, so `radius expr(2+3)` derives identically
//  to `radius 5`.  Facet 1 owns the lossless representation + this canonical derive-time
//  evaluation; the editor's incremental traced-input invalidation (D4) is deferred
//  Facet-2 work.  There is NO legacy oracle (the v6 parser has no `expr()`), so the
//  derive is checked against the HAND-WRITTEN literal, not DumpLegacy.  Locks in:
//    [round-trip] an expr(...) value round-trips byte-for-byte (the lexer is exact).
//    [derive]     expr(...) evaluates to the SAME Job as the literal it folds to
//                 (arithmetic, a function, pow, a non-integer).
//    [refuse]     a malformed / non-single / non-finite expr applies NOTHING + diagnoses
//                 (refuse-all -- never a silent bad value).
//    [neutral]    a non-expr value is untouched (a plain literal still derives).
//
//////////////////////////////////////////////////////////////////////

#include "CstRenderEquivalence.h"
#include "../src/Library/Cst/Cst.h"

using namespace RISE;
using namespace RISE::Cst;
using namespace risequiv;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

static const std::string HDR = "RISE ASCII SCENE 6\n";

// Derive a scene through the CST path (fresh Job each call); DumpJob + optional diagnostics.
static std::string DumpCst( const std::string& scene, std::vector<std::string>* outDiags = nullptr )
{
	Job* j = new Job();
	Document d = ParseToCst( scene );
	std::vector<std::string> diags;
	DeriveToJob( d, *j, &diags );
	if( outDiags ) *outDiags = diags;
	std::string s = DumpJob( *j );
	j->release();
	return s;
}

static std::string Sphere( const std::string& radius )
{
	return HDR + "sphere_geometry\n{\nname s\nradius " + radius + "\n}\n";
}

static bool DiagHas( const std::vector<std::string>& d, const char* sub )
{
	for( const std::string& s : d ) if( s.find( sub ) != std::string::npos ) return true;
	return false;
}

int main()
{
	std::printf( "CstExprTest -- Facet 1 / #5 slice 2: expr(...) derive-time evaluation (D4)\n" );

	// [round-trip] expr(...) values round-trip byte-for-byte (concatenation-exact lexer), with or
	// without internal whitespace.
	{
		const std::string a = Sphere( "expr( 2 + 3 )" );
		const std::string b = Sphere( "expr(2+3)" );
		Check( SerializeCst( ParseToCst( a ) ) == a, "round-trip: expr( 2 + 3 ) preserved byte-for-byte" );
		Check( SerializeCst( ParseToCst( b ) ) == b, "round-trip: expr(2+3) (no spaces) preserved byte-for-byte" );
	}

	// [derive] expr evaluates to the SAME Job as the hand-written literal it folds to.
	const std::string lit5 = DumpCst( Sphere( "5" ) );
	Check( !lit5.empty() && lit5 != DumpCst( HDR ), "sanity: the literal sphere derives to a non-empty Job" );
	Check( DumpCst( Sphere( "expr(2+3)" ) )       == lit5,                       "derive: expr(2+3) == literal 5" );
	Check( DumpCst( Sphere( "expr( 2 + 3 )" ) )   == lit5,                       "derive: expr with whitespace == literal 5" );
	Check( DumpCst( Sphere( "expr(1 + 0.5*4)" ) ) == DumpCst( Sphere( "3" ) ),   "derive: expr(1 + 0.5*4) == literal 3 (precedence)" );
	Check( DumpCst( Sphere( "expr(sqrt(16))" ) )  == DumpCst( Sphere( "4" ) ),   "derive: expr(sqrt(16)) == literal 4 (function, nested parens)" );
	Check( DumpCst( Sphere( "expr(2^10)" ) )      == DumpCst( Sphere( "1024" ) ),"derive: expr(2^10) == literal 1024 (pow)" );
	Check( DumpCst( Sphere( "expr(1.0/4.0)" ) )   == DumpCst( Sphere( "0.25" ) ),"derive: expr(1.0/4.0) == literal 0.25 (non-integer, exact)" );

	// [refuse] a malformed / non-single / non-finite expr applies NOTHING + diagnoses (refuse-all).
	{
		std::vector<std::string> d;
		const std::string dump = DumpCst( Sphere( "expr(2 +)" ), &d );
		Check( !d.empty() && dump != lit5, "refuse: a malformed expr(2 +) applies NOTHING + diagnoses" );
	}
	{
		std::vector<std::string> d;
		const std::string dump = DumpCst( Sphere( "expr(2)+expr(3)" ), &d );
		Check( !d.empty() && dump != lit5, "refuse: expr(2)+expr(3) is NOT a single expr -> rejected, not mis-evaluated" );
	}
	{
		std::vector<std::string> d;
		DumpCst( Sphere( "expr(nope(1))" ), &d );   // unknown function -> compile error
		Check( !d.empty(), "refuse: an unknown function in expr(...) diagnoses + applies nothing" );
	}

	// [neutral] a non-expr value is untouched: a plain literal still derives (behavior-neutral).
	{
		std::vector<std::string> d;
		DumpCst( Sphere( "2.5" ), &d );
		Check( d.empty(), "neutral: a plain literal value derives with no diagnostics" );
	}
	// A reference value that merely STARTS with 'expr' but has no '(' is NOT intercepted (it needs the
	// paren) -- a material named 'expr' resolves normally.
	{
		const std::string scene = HDR
			+ "uniformcolor_painter\n{\nname expr\ncolor 0.5 0.5 0.5\n}\n"
			+ "lambertian_material\n{\nname m\nreflectance expr\n}\n";
		std::vector<std::string> d;
		DumpCst( scene, &d );
		Check( d.empty(), "neutral: a reference named 'expr' (no paren) is a normal token, not intercepted" );
	}

	// [refuse: non-finite] a DOMAIN-ERROR expr (log(-1) -> nan) is rejected AT THE EVAL BOUNDARY (not
	// only by the numeric descriptor), so it is caught even in a NON-numeric slot -- the escape
	// ExpressionProgram::IsFinite leaves open under -ffast-math.
	{
		std::vector<std::string> d;
		DumpCst( Sphere( "expr(log(-1))" ), &d );
		Check( DiagHas( d, "non-finite value" ), "refuse: a non-finite expr (numeric slot) is caught at the eval boundary" );
	}
	{
		// a STRING slot (name) has NO descriptor numeric check -- without the eval-boundary guard this
		// would SILENTLY bake an entity literally named "nan".
		std::vector<std::string> d;
		DumpCst( HDR + "sphere_geometry\n{\nname expr(log(-1))\nradius 1\n}\n", &d );
		Check( DiagHas( d, "non-finite value" ), "refuse: a non-finite expr in a STRING slot is rejected (no silent nan-named entity)" );
	}
	// [tuple slot] expr yields ONE scalar -- a per-component expr in a Vec3 slot is NOT a single expr
	// value, so it is passed through + rejected (per-component evaluation is out of scope).
	{
		std::vector<std::string> d;
		DumpCst( HDR + "uniformcolor_painter\n{\nname p\ncolor expr(0.1) expr(0.2) expr(0.3)\n}\n", &d );
		Check( !d.empty(), "tuple: per-component expr in a Vec3 (color) slot is rejected (expr is one scalar)" );
	}
	// [whitespace-heavy] interior spacing + nested parens evaluate the same.
	Check( DumpCst( Sphere( "expr(  sqrt( ( 4 ) * 4 )  )" ) ) == DumpCst( Sphere( "4" ) ), "derive: whitespace-heavy nested expr == literal 4" );

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
