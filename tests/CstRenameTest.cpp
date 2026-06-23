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

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
