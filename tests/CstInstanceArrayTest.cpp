//////////////////////////////////////////////////////////////////////
//
//  CstInstanceArrayTest.cpp - Facet 1 / #5 slice 4: the `instance_array` generator (§2.6.1, the
//  nested-FOR replacement).  `instance_array { name g  template geo  material m  count_u U  count_v V
//  position expr(...) ... }` is EXPANDED at derive time into U*V standard_objects named `g[i,j]`, each
//  with geometry=template + the pass-through params (material/position/orientation/...) evaluated
//  PER INSTANCE (instance vars: i,j indices; u=i/(U-1), v=j/(V-1) in [0,1]).  The CST stores only the
//  generator (INV-3/INV-4); the expansion is canonical-derive (Facet-1).  Locks in:
//    [round-trip] an instance_array scene round-trips byte-for-byte.
//    [expand]     U*V objects named g[i,j] == the hand-written standard_objects (single / grid / u,v).
//    [count_v]    count_v defaults to 1 (a 1D linear array).
//    [refuse]     missing template / count_u, a bad count.
//    [incremental] an instance_array edit refuses -> full-derive fallback.
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

// A scene with the geometry/material/painter the array references, + a body.
static std::string Scene( const std::string& body )
{
	return HDR
		+ "sphere_geometry\n{\nname geo\nradius 1\n}\n"
		+ "uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
		+ "lambertian_material\n{\nname m\nreflectance p\n}\n"
		+ body;
}
static std::string Obj( const std::string& name, const std::string& position )
{
	return "standard_object\n{\nname " + name + "\ngeometry geo\nmaterial m\nposition " + position + "\n}\n";
}
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

int main()
{
	std::printf( "CstInstanceArrayTest -- Facet 1 / #5 slice 4: instance_array generator (§2.6.1)\n" );

	// [round-trip] an instance_array scene round-trips byte-for-byte (the generic lexer; nothing applied).
	{
		const std::string scene = Scene( "instance_array\n{\nname g\ntemplate geo\nmaterial m\ncount_u 2\nposition expr(i) 0 0\n}\n" );
		Check( SerializeCst( ParseToCst( scene ) ) == scene, "round-trip: an instance_array scene round-trips byte-for-byte" );
	}

	// [expand: single] count_u 1, count_v 1 -> ONE object g[0,0].
	{
		std::vector<std::string> d;
		const std::string ia = DumpCst( Scene( "instance_array\n{\nname g\ntemplate geo\nmaterial m\ncount_u 1\ncount_v 1\nposition expr(i) expr(j) 0\n}\n" ), &d );
		Check( ia == DumpCst( Scene( Obj( "g[0,0]", "0 0 0" ) ) ) && d.empty(),
		       "expand: 1x1 -> one object g[0,0] == hand-written standard_object" );
	}

	// [expand: grid + i/j] count_u 2 count_v 2, position expr(i) expr(j) 0 -> 4 objects (i fastest).
	{
		const std::string ia = DumpCst( Scene( "instance_array\n{\nname g\ntemplate geo\nmaterial m\ncount_u 2\ncount_v 2\nposition expr(i) expr(j) 0\n}\n" ) );
		const std::string hand = DumpCst( Scene(
			Obj( "g[0,0]", "0 0 0" ) + Obj( "g[1,0]", "1 0 0" ) + Obj( "g[0,1]", "0 1 0" ) + Obj( "g[1,1]", "1 1 0" ) ) );
		Check( ia == hand, "expand: 2x2 grid with expr(i)/expr(j) == 4 hand-written objects g[i,j]" );
	}

	// [expand: u/v] count_u 3, position expr(u) 0 0 -> u = 0, 0.5, 1 across the row.
	{
		const std::string ia = DumpCst( Scene( "instance_array\n{\nname g\ntemplate geo\nmaterial m\ncount_u 3\nposition expr(u) 0 0\n}\n" ) );
		const std::string hand = DumpCst( Scene(
			Obj( "g[0,0]", "0 0 0" ) + Obj( "g[1,0]", "0.5 0 0" ) + Obj( "g[2,0]", "1 0 0" ) ) );
		Check( ia == hand, "expand: count_u 3 with expr(u) -> u = 0, 0.5, 1 (normalized index)" );
	}

	// [count_v default] count_u 2, no count_v -> 1D linear (count_v == 1).
	{
		const std::string ia = DumpCst( Scene( "instance_array\n{\nname g\ntemplate geo\nmaterial m\ncount_u 2\nposition expr(i*0.02-0.1) 0 0\n}\n" ) );
		const std::string hand = DumpCst( Scene( Obj( "g[0,0]", "-0.1 0 0" ) + Obj( "g[1,0]", "-0.08 0 0" ) ) );
		Check( ia == hand, "count_v default: count_u 2 with no count_v -> 2 objects (1D linear)" );
	}

	// [let in an instance_array expr] a document-level let resolves inside a per-instance expr.
	{
		const std::string ia = DumpCst( HDR
			+ "let\n{\nGAP 10\n}\n"
			+ "sphere_geometry\n{\nname geo\nradius 1\n}\n"
			+ "uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			+ "lambertian_material\n{\nname m\nreflectance p\n}\n"
			+ "instance_array\n{\nname g\ntemplate geo\nmaterial m\ncount_u 2\nposition expr(i*GAP) 0 0\n}\n" );
		Check( ia == DumpCst( Scene( Obj( "g[0,0]", "0 0 0" ) + Obj( "g[1,0]", "10 0 0" ) ) ),
		       "let: a document-level let (GAP) resolves inside a per-instance expr" );
	}

	// [refuse] missing template / count_u, and a bad count.
	{
		std::vector<std::string> d;
		DumpCst( Scene( "instance_array\n{\nname g\nmaterial m\ncount_u 2\n}\n" ), &d );
		Check( !d.empty(), "refuse: a missing template diagnoses" );
	}
	{
		std::vector<std::string> d;
		DumpCst( Scene( "instance_array\n{\nname g\ntemplate geo\nmaterial m\n}\n" ), &d );
		Check( !d.empty(), "refuse: a missing count_u diagnoses" );
	}
	{
		std::vector<std::string> d;
		DumpCst( Scene( "instance_array\n{\nname g\ntemplate geo\nmaterial m\ncount_u foo\n}\n" ), &d );
		Check( !d.empty(), "refuse: a non-numeric count diagnoses" );
	}

	// [count cap] a typo-huge count refuses BEFORE allocating (DoS guard).
	{
		std::vector<std::string> d;
		DumpCst( Scene( "instance_array\n{\nname g\ntemplate geo\nmaterial m\ncount_u 1000001\n}\n" ), &d );
		Check( !d.empty(), "cap: count_u > 1e6 refuses (no allocation)" );
	}
	{
		std::vector<std::string> d;
		DumpCst( Scene( "instance_array\n{\nname g\ntemplate geo\nmaterial m\ncount_u 4000\ncount_v 4000\n}\n" ), &d );
		Check( !d.empty(), "cap: count_u*count_v > 1e7 refuses (no allocation)" );
	}
	// [count_u==1 + expr(u)] u must be 0, not a 0/0 NaN.
	{
		const std::string ia = DumpCst( Scene( "instance_array\n{\nname g\ntemplate geo\nmaterial m\ncount_u 1\nposition expr(u) expr(v) 0\n}\n" ) );
		Check( ia == DumpCst( Scene( Obj( "g[0,0]", "0 0 0" ) ) ), "count_u==1: expr(u)/expr(v) -> 0 (no 0/0 NaN)" );
	}
	// [passthrough] orientation/scale evaluate per-component like position (generic pass-through).
	{
		const std::string ia = DumpCst( Scene( "instance_array\n{\nname g\ntemplate geo\nmaterial m\ncount_u 2\norientation expr(i*90) 0 0\nscale expr(u+1) 1 1\n}\n" ) );
		const std::string hand = DumpCst( Scene(
			"standard_object\n{\nname g[0,0]\ngeometry geo\nmaterial m\norientation 0 0 0\nscale 1 1 1\n}\n"
			"standard_object\n{\nname g[1,0]\ngeometry geo\nmaterial m\norientation 90 0 0\nscale 2 1 1\n}\n" ) );
		Check( ia == hand, "passthrough: orientation/scale per-component eval == hand-written" );
	}
	// [collision] a generated name clashing an existing object diagnoses (the pre-check).
	{
		std::vector<std::string> d;
		DumpCst( Scene(
			"standard_object\n{\nname g[0,0]\ngeometry geo\nmaterial m\n}\n"
			"instance_array\n{\nname g\ntemplate geo\nmaterial m\ncount_u 1\n}\n" ), &d );
		Check( !d.empty(), "collision: a generated name clashing an existing object diagnoses (no silent first-win)" );
	}

	// [incremental] an instance_array edit refuses on the O(closure) path -> full-derive fallback.
	{
		Document d = ParseToCst( Scene( "instance_array\n{\nname g\ntemplate geo\nmaterial m\ncount_u 2\nposition expr(i) 0 0\n}\n" ) );
		Job* j = new Job(); std::vector<std::string> dd;
		DeriveToJob( d, *j, &dd );
		Check( dd.empty(), "incremental: the full derive of the instance_array scene succeeds" );
		const NodeId iaId = DocFindByName( d, "instance_array/g" );   // named chunks are in byName even if not a registry chunk
		std::vector<std::string> di;
		const int applied = DeriveToJobIncremental( d, *j, std::vector<NodeId>( 1, iaId ), &di );
		Check( iaId != 0 && applied == 0 && !di.empty(), "incremental: an instance_array in the closure refuses (applied=0) -> full-derive fallback" );
		j->release();
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
