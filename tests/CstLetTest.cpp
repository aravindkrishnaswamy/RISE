//////////////////////////////////////////////////////////////////////
//
//  CstLetTest.cpp - Facet 1 / #5 slice 3: document-level `let` constants (§2.6, the DEFINE
//  replacement).  `let { NAME value ... }` declares scene-global named constants used in
//  expr(...): `let { POWER 2.3 }` + `power expr( POWER )`.  A `let` chunk is NOT an engine
//  entity -- DeriveToJob collects its bindings + does NOT apply the chunk.  A binding is one
//  numeric literal or an expr(...) (which may reference EARLIER lets -- lexical document-order
//  scope).  `PI`/`E` are reserved built-in identifiers.  The editor's traced-input invalidation
//  is deferred (Facet-2), so the O(closure) incremental path does NOT carry lets -- a closure
//  expr referencing a let refuses + falls back to a full derive.  Locks in:
//    [round-trip] a let scene round-trips byte-for-byte.
//    [resolve]    expr(NAME) resolves to the let value; multi-let; let-referencing-let; PI/E.
//    [not-entity] a let chunk applies NOTHING to the Job.
//    [refuse]     a malformed literal binding / a forward let reference refuses.
//    [incremental] a closure expr referencing a let refuses -> full-derive fallback.
//
//////////////////////////////////////////////////////////////////////

#include "CstRenderEquivalence.h"
#include "../src/Library/Cst/Cst.h"

#include <cstdio>

using namespace RISE;
using namespace RISE::Cst;
using namespace risequiv;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

static const std::string HDR = "RISE ASCII SCENE 6\n";

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
static bool DiagHas( const std::vector<std::string>& d, const char* sub )
{
	for( const std::string& s : d ) if( s.find( sub ) != std::string::npos ) return true;
	return false;
}
static std::string Radius( const std::string& r ) { return HDR + "sphere_geometry\n{\nname s\nradius " + r + "\n}\n"; }

int main()
{
	std::printf( "CstLetTest -- Facet 1 / #5 slice 3: let constants (§2.6)\n" );

	// [round-trip] a let chunk + consumer round-trips byte-for-byte (the CST parses let{} generically).
	{
		const std::string scene = HDR + "let\n{\nPOWER 2.3\n}\n" + "sphere_geometry\n{\nname s\nradius expr( POWER )\n}\n";
		Check( SerializeCst( ParseToCst( scene ) ) == scene, "round-trip: a let scene round-trips byte-for-byte" );
	}

	// [resolve] expr(NAME) resolves to the let binding.
	{
		const std::string letScene = HDR + "let\n{\nPOWER 2.3\n}\n" + "sphere_geometry\n{\nname s\nradius expr(POWER)\n}\n";
		std::vector<std::string> d;
		Check( DumpCst( letScene, &d ) == DumpCst( Radius( "2.3" ) ) && d.empty(), "resolve: radius expr(POWER) == literal 2.3" );
	}
	// [multi] two lets in one block, used in one expr.
	{
		const std::string s = HDR + "let\n{\nA 2\nB 3\n}\n" + "sphere_geometry\n{\nname s\nradius expr(A*B)\n}\n";
		Check( DumpCst( s ) == DumpCst( Radius( "6" ) ), "resolve: let{A 2 B 3} + expr(A*B) == 6" );
	}
	// [let-references-let] an expr-valued let may reference an EARLIER let (document-order scope).
	{
		const std::string s = HDR + "let\n{\nA 1\nB expr(A+1)\n}\n" + "sphere_geometry\n{\nname s\nradius expr(B*10)\n}\n";
		Check( DumpCst( s ) == DumpCst( Radius( "20" ) ), "resolve: let{A 1 B expr(A+1)} + expr(B*10) == 20 (let references earlier let)" );
	}
	// [PI/E built-ins] reserved identifiers resolve to M_PI / M_E (compute the literal the same way).
	{
		char piLit[64], eLit[64];
		std::snprintf( piLit, sizeof(piLit), "%.17g", 3.14159265358979323846 );
		std::snprintf( eLit,  sizeof(eLit),  "%.17g", 2.71828182845904523536 );
		Check( DumpCst( Radius( "expr(PI)" ) ) == DumpCst( Radius( piLit ) ), "resolve: expr(PI) == M_PI" );
		Check( DumpCst( Radius( "expr(E)"  ) ) == DumpCst( Radius( eLit  ) ), "resolve: expr(E) == M_E" );
		Check( DumpCst( Radius( "expr(PI*2)" ) ) == DumpCst( Radius( "expr(2*PI)" ) ), "resolve: PI is consistent (PI*2 == 2*PI)" );
	}

	// [not-entity] a let chunk applies NOTHING to the Job (a let-only scene == an empty scene).
	{
		std::vector<std::string> d;
		Check( DumpCst( HDR + "let\n{\nLZ 0.1\nPOWER 2.3\n}\n", &d ) == DumpCst( HDR ) && d.empty(),
		       "not-entity: a let chunk applies nothing (let-only scene == empty)" );
	}

	// [refuse] a malformed literal binding (non-numeric, non-expr) refuses (refuse-all).
	{
		std::vector<std::string> d;
		DumpCst( HDR + "let\n{\nX foo\n}\n" + Radius( "1" ).substr( HDR.size() ), &d );
		Check( DiagHas( d, "let.X" ), "refuse: a malformed literal let binding (X foo) diagnoses + refuses" );
	}
	// [refuse] a FORWARD let reference (B uses A defined later) -> document-order scope -> refuse.
	{
		std::vector<std::string> d;
		DumpCst( HDR + "let\n{\nB expr(A)\nA 1\n}\n" + "sphere_geometry\n{\nname s\nradius expr(B)\n}\n", &d );
		Check( DiagHas( d, "let.B" ), "refuse: a forward let reference (B before A) is out of scope -> refuses (let.B diagnosed)" );
	}

	// [reserved] a let may not bind a reserved expr identifier (u/v/i/j coordinate or pi/e/tau/PI/E
	// constant) -- u/v are pre-registered Builder slots a let would be SILENTLY clobbered into (0).
	{
		std::vector<std::string> d; DumpCst( HDR + "let\n{\nu 5\n}\n", &d );
		Check( DiagHas( d, "let.u" ), "reserved: a let named u (a query coordinate) is rejected (no silent clobber)" );
	}
	{
		std::vector<std::string> d; DumpCst( HDR + "let\n{\nPI 9\n}\n", &d );
		Check( DiagHas( d, "let.PI" ), "reserved: a let named PI is rejected (the built-in is un-shadowable)" );
	}
	{
		std::vector<std::string> d; DumpCst( HDR + "let\n{\npi 9\n}\n", &d );
		Check( DiagHas( d, "let.pi" ), "reserved: a let named pi is rejected (lowercase math constant un-shadowable too)" );
	}
	// [duplicate] a duplicate let name is last-wins (the later binding overwrites the slot).
	{
		const std::string s = HDR + "let\n{\nA 1\nA 2\n}\n" + "sphere_geometry\n{\nname s\nradius expr(A)\n}\n";
		Check( DumpCst( s ) == DumpCst( Radius( "2" ) ), "duplicate: a duplicate let name is last-wins (A 1 then A 2 -> 2)" );
	}
	// [value-less] a let binding with no value diagnoses.
	{
		std::vector<std::string> d; DumpCst( HDR + "let\n{\nX\n}\n", &d );
		Check( DiagHas( d, "let.X" ), "value-less: a let binding with no value diagnoses" );
	}

	// [incremental] a closure expr referencing a let refuses on the O(closure) path (no lets) ->
	// the caller full-derives.  A full derive of the SAME scene resolves it (verified above).
	{
		Document d = ParseToCst( HDR + "let\n{\nPOWER 2.3\n}\n" + "sphere_geometry\n{\nname s\nradius expr(POWER)\n}\n" );
		Job* j = new Job();
		std::vector<std::string> dd;
		DeriveToJob( d, *j, &dd );                                 // full derive: resolves POWER
		Check( dd.empty(), "incremental: the full derive of the let scene succeeds" );
		const NodeId sid = DocFindByName( d, "sphere_geometry/s" );
		std::vector<std::string> di;
		const int applied = DeriveToJobIncremental( d, *j, std::vector<NodeId>( 1, sid ), &di );
		Check( applied == 0 && !di.empty(), "incremental: a closure expr referencing a let refuses (applied=0) -> full-derive fallback" );
		j->release();
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
