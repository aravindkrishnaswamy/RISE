//////////////////////////////////////////////////////////////////////
//
//  CstRenameTest.cpp - slice 2 of docs/agentic-redesign/21-stable-apply-and-
//  resolver.md: ATOMIC rename on the shared reference graph. Locks in:
//    [atomic]      every referrer is rewritten (rewrite-all, no partial rename),
//                  including a tuple-reference token (CstEditReparseTest covers the
//                  tuple case); the target's NodeId is preserved.
//    [collision]   a rename that would collide -- with another same-category chunk
//                  OR with a runtime default (`none` / `Default*`) -- is REFUSED
//                  atomically (document returned unchanged) (P1.7).
//    [validation]  an empty new name is refused; a no-op rename is a no-op.
//    [animation]   a rename is refused when the document has any animation/timeline
//                  (its String element/animation references are invisible to the
//                  static graph and could be left dangling) (P1.8 / timeline).
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Cst/Cst.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace RISE;
using namespace RISE::Cst;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }
static bool Has( const std::string& s, const char* n ) { return s.find( n ) != std::string::npos; }

int main()
{
	std::printf( "CstRenameTest -- slice 2 (atomic rename: rewrite-all / collision / validation / animation)\n" );

	const std::string twoRef =
		"RISE ASCII SCENE 6\n"
		"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
		"lambertian_material\n{\nname m1\nreflectance p\n}\n"
		"lambertian_material\n{\nname m2\nreflectance p\n}\n";

	// [atomic] rename a painter referenced by TWO materials -> BOTH referrers rewritten.
	{
		Document d = ParseToCst( twoRef );
		const NodeId p = DocFindByName( d, "uniformcolor_painter/p" );
		std::vector<std::string> diags;
		Document d2 = DocRename( d, p, "p2", &diags );
		const std::string out = SerializeCst( d2 );
		Check( DocFindByName( d2, "uniformcolor_painter/p2" ) == p, "target painter renamed p->p2, NodeId preserved" );
		Check( !Has( out, "reflectance p\n" ), "no referrer left pointing at the old name (no partial rename)" );
		Check( Has( out, "reflectance p2\n" ), "referrers rewritten to the new name" );
		Check( diags.empty(), "clean rename: no diagnostics" );
		// every other byte is untouched (only the name + the two referrers changed).
		Check( Has( out, "name m1" ) && Has( out, "name m2" ) && Has( out, "color 0.5 0.5 0.5" ), "unrelated content untouched" );
	}

	// [collision vs runtime default] rename a material to `none` -> REFUSED, unchanged.
	{
		Document d = ParseToCst( "RISE ASCII SCENE 6\nlambertian_material\n{\nname m\nreflectance none\n}\n" );
		const std::string before = SerializeCst( d );
		const NodeId m = DocFindByName( d, "lambertian_material/m" );
		std::vector<std::string> diags;
		Document d2 = DocRename( d, m, "none", &diags );
		Check( !diags.empty() && SerializeCst( d2 ) == before, "rename material -> 'none' (runtime default) REFUSED, document unchanged" );
	}

	// [collision vs CST chunk] rename m1 -> m2 (existing same-category name) -> REFUSED.
	{
		Document d = ParseToCst( twoRef );
		const std::string before = SerializeCst( d );
		const NodeId m1 = DocFindByName( d, "lambertian_material/m1" );
		std::vector<std::string> diags;
		Document d2 = DocRename( d, m1, "m2", &diags );
		Check( !diags.empty() && SerializeCst( d2 ) == before, "rename into an existing same-category name REFUSED, unchanged" );
	}

	// [validation] empty new name -> refused; no-op rename -> unchanged, no diag.
	{
		Document d = ParseToCst( "RISE ASCII SCENE 6\nlambertian_material\n{\nname m\nreflectance none\n}\n" );
		const std::string before = SerializeCst( d );
		const NodeId m = DocFindByName( d, "lambertian_material/m" );
		std::vector<std::string> de;
		Document d2 = DocRename( d, m, "", &de );
		Check( !de.empty() && SerializeCst( d2 ) == before, "empty new name REFUSED, unchanged" );
		std::vector<std::string> dn;
		Document d3 = DocRename( d, m, "m", &dn );
		Check( dn.empty() && SerializeCst( d3 ) == before, "no-op rename (same name) -> unchanged, no diagnostic" );
	}

	// [animation guard] rename refused when the document has an animation chunk.
	{
		Document d = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"lambertian_material\n{\nname m\nreflectance none\n}\n"
			"animation\n{\nname anim\n}\n" );
		const std::string before = SerializeCst( d );
		const NodeId m = DocFindByName( d, "lambertian_material/m" );
		std::vector<std::string> diags;
		Document d2 = DocRename( d, m, "m9", &diags );
		Check( !diags.empty() && SerializeCst( d2 ) == before, "rename REFUSED when an animation/timeline is present (untraceable String refs)" );
	}

	// [override guard] (review P1.3) rename refused when the doc has an override_object
	// (its String target reference cannot be rewritten -> would dangle).
	{
		Document d = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nposition 0 0 0\n}\n"
			"override_object\n{\nname o\nposition 5 0 0\n}\n" );
		const std::string before = SerializeCst( d );
		const NodeId g = DocFindByName( d, "sphere_geometry/g" );
		std::vector<std::string> diags;
		Document d2 = DocRename( d, g, "g2", &diags );
		Check( !diags.empty() && SerializeCst( d2 ) == before, "rename REFUSED when an override_object is present (untraceable String target ref, P1.3)" );
	}

	// [painter conflation] (review P1.4) renaming a painter whose name is in BOTH the
	// colour and scalar managers is refused (the (category,name) graph cannot disambiguate).
	{
		Document d = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"scalar_painter\n{\nname p\nfile noise.dat\n}\n" );
		const std::string before = SerializeCst( d );
		const NodeId pc = DocFindByName( d, "uniformcolor_painter/p" );
		std::vector<std::string> diags;
		Document d2 = DocRename( d, pc, "p2", &diags );
		Check( !diags.empty() && SerializeCst( d2 ) == before, "rename REFUSED for a painter conflated across colour+scalar managers (P1.4)" );
	}

	// [piecewise_linear_function2d guard] (review #2) rename refused when the doc has a
	// piecewise_linear_function2d: its `cp` entries embed Function1D references that ARE traced
	// for closure (review #2, 2nd pass), but they are opaque String tokens, not rewritable
	// Reference params, so the rename cannot substitute the new name -> refuse.
	{
		Document d = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"piecewise_linear_function2d\n{\nname f2\n}\n" );
		const std::string before = SerializeCst( d );
		const NodeId g = DocFindByName( d, "sphere_geometry/g" );
		std::vector<std::string> diags;
		Document d2 = DocRename( d, g, "g2", &diags );
		Check( !diags.empty() && SerializeCst( d2 ) == before, "rename REFUSED when a piecewise_linear_function2d is present (cp Function1D refs traced for closure but String tokens the rename can't rewrite, #2)" );
	}

	// [function 1D/2D conflation] (review #3) rename refused for a Function chunk whose name
	// has another Function-namespace producer (a 1D + 2D pair).
	{
		Document d = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"piecewise_linear_function\n{\nname fx\n}\n"
			"piecewise_linear_function2d\n{\nname fx\n}\n" );
		const std::string before = SerializeCst( d );
		const NodeId f1 = DocFindByName( d, "piecewise_linear_function/fx" );
		std::vector<std::string> diags;
		Document d2 = DocRename( d, f1, "fy", &diags );
		Check( !diags.empty() && SerializeCst( d2 ) == before, "rename REFUSED for a Function name with both a 1D and 2D producer (#3)" );
	}

	// [plf1d colour rename] (review #3a) a piecewise_linear_function referenced from a colour
	// slot (reflectance) -- via its dual-register into the colour painter manager -- is now a
	// traced edge, so renaming it REWRITES the referrer (no dangling, no false refusal).
	{
		Document d = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"piecewise_linear_function\n{\nname fx\ncp 0 0\ncp 1 1\n}\n"
			"lambertian_material\n{\nname m\nreflectance fx\n}\n" );
		const NodeId fx = DocFindByName( d, "piecewise_linear_function/fx" );
		std::vector<std::string> diags;
		Document d2 = DocRename( d, fx, "fx2", &diags );
		const std::string out = SerializeCst( d2 );
		Check( diags.empty() && Has( out, "reflectance fx2\n" ) && !Has( out, "reflectance fx\n" ), "plf1d colour rename: reflectance referrer REWRITTEN fx->fx2, none dangling (review #3a)" );
	}

	// [plf1d+colour-painter conflation] (review #3a) when a plf1d and a colour painter share a
	// name, (Painter,name) is ambiguous -> renaming EITHER is refused (the #3 funcProducers
	// guard counts both, since a colour painter is itself a Function-namespace producer).
	{
		Document d = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"piecewise_linear_function\n{\nname x\ncp 0 0\ncp 1 1\n}\n"
			"uniformcolor_painter\n{\nname x\ncolor 0.5 0.5 0.5\n}\n" );
		const std::string before = SerializeCst( d );
		const NodeId plf = DocFindByName( d, "piecewise_linear_function/x" );
		const NodeId pnt = DocFindByName( d, "uniformcolor_painter/x" );
		std::vector<std::string> dp, dc;
		Document dpr = DocRename( d, plf, "y", &dp );
		Document dcr = DocRename( d, pnt, "y", &dc );
		Check( !dp.empty() && SerializeCst( dpr ) == before, "plf1d+painter conflation: renaming the plf1d REFUSED (review #3a)" );
		Check( !dc.empty() && SerializeCst( dcr ) == before, "plf1d+painter conflation: renaming the colour painter REFUSED (review #3a)" );
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
