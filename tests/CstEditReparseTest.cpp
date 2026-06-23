//////////////////////////////////////////////////////////////////////
//
//  CstEditReparseTest.cpp - transfer-gate item 7: exercise STRUCTURED edits
//  AND free-form REPARSES, including chunk identity + RENAME, end-to-end to the
//  derived Job.
//
//  Items 3/4 proved the edit/identity PRIMITIVES (DocReplaceItem/InsertItem/
//  EraseItem preserve identity exactly; DocReparse carries NodeIds best-effort).
//  Item 5 made the derive faithful; item 6 built the reference graph. Item 7 ties
//  them together: an edit must (a) preserve identity and (b) produce a Job
//  identical to a fresh parse of the edited text. The marquee is RENAME (D14):
//  changing a chunk's name preserves its NodeId (lineage, D44) and rewrites ALL its
//  referrers from the traced reference graph, so those references still resolve (a
//  tuple referrer's reference TOKEN is rewritten, other tokens intact -- slice 2; a
//  name collision is refused -- see [rename-tuple] / [rename-collision]).
//
//  This suite proves:
//    * [setparam]  a within-chunk value edit (DocSetParamValue) preserves the
//                  chunk's NodeId + the untouched params' ids, and the edited CST
//                  derives the same Job as a fresh parse of its serialization;
//    * [replace]   DocReplaceItem (whole-chunk) preserves the position's NodeId;
//    * [erase]     DocEraseItem drops a chunk; derive has one fewer;
//    * [insert]    DocInsertItem adds a chunk; derive has one more;
//    * [reparse]   DocReparse carries NodeIds across a free-form text change; the
//                  reparsed CST derives consistently;
//    * [rename]    DocRename preserves the chunk's NodeId (now addressed by the
//                  NEW name), rewrites the referrer (material.reflectance) from
//                  the traced graph, and the renamed scene still derives + the
//                  reference re-resolves to the same chunk;
//    * [rename-tuple] a tuple-reference referrer (advanced_shader.shaderop) is
//                  REWRITTEN at the reference token (slice 2 atomic rename), other
//                  tuple tokens intact;
//    * [rename-collision] renaming into an existing same-category name is refused
//                  ATOMICALLY (Document unchanged; the referrer is not re-targeted).
//
//////////////////////////////////////////////////////////////////////

#include "CstRenderEquivalence.h"
#include "../src/Library/Cst/Cst.h"

using namespace RISE;
using namespace RISE::Cst;
using namespace risequiv;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

// A 4-chunk scene: painter -> material -> object, + geometry.
static std::string Scene()
{
	return
		"RISE ASCII SCENE 6\n"
		"uniformcolor_painter\n{\nname red\ncolor 0.2 0.4 0.6\n}\n"
		"lambertian_material\n{\nname redmat\nreflectance red\n}\n"
		"sphere_geometry\n{\nname ball\nradius 0.6\n}\n"
		"standard_object\n{\nname obj\ngeometry ball\nmaterial redmat\n}\n";
}

// Dump the Job a CST derives.
static std::string DeriveDump( const Document& d )
{
	Job* j = new Job(); std::vector<std::string> dg; DeriveToJob( d, *j, &dg );
	std::string s = DumpJob( *j ); j->release(); return s;
}
// The edit is END-TO-END FAITHFUL iff the edited CST derives the SAME Job as a
// fresh legacy parse of the edited document's serialization.
static bool DeriveMatchesReparse( const Document& edited )
{
	Job* lj = new Job(); ParseLegacy( SerializeCst( edited ), *lj );
	std::string viaLegacy = DumpJob( *lj ); lj->release();
	return DeriveDump( edited ) == viaLegacy;
}
// Build a standalone node from text's first top-level item (a chunk, or a
// trivia separator) -- for replace/insert.
static NodeRef ItemFromText( const std::string& text )
{
	Document d = ParseToCst( text );
	return DocResolveNodeId( d, DocNodeIdAt( d, 0 ) );
}

int main()
{
	std::printf( "CstEditReparseTest -- transfer-gate item 7 (structured edits + reparse + rename)\n" );

	//----------------------------------------------------------------------
	std::printf( "[setparam] within-chunk value edit: NodeId preserved, derive faithful\n" );
	{
		Document d = ParseToCst( Scene() );
		const NodeId ball = DocFindByName( d, "sphere_geometry/ball" );
		const NodeId nameBefore = DocParamId( d, ball, "name" );
		Document d2 = DocSetParamValue( d, ball, "radius", 0, "0.9" );
		Check( d2.items.get() != d.items.get(), "edit produced a new Document (immutability)" );
		Check( SerializeCst( d ) == Scene(), "original Document unchanged (persistence)" );
		Check( DocFindByName( d2, "sphere_geometry/ball" ) == ball, "ball's NodeId preserved across the value edit (lineage)" );
		Check( DocParamId( d2, ball, "name" ) == nameBefore, "the untouched 'name' param keeps its NodeId" );
		Check( DocParamId( d2, ball, "radius" ) == DocParamId( d, ball, "radius" ), "the EDITED 'radius' param keeps its NodeId (value edit preserves param lineage)" );
		Check( SerializeCst( d2 ).find( "radius 0.9" ) != std::string::npos, "serialization shows the new value" );
		Check( DeriveDump( d2 ).find( "ball " + std::string("bsphere=") ) != std::string::npos, "derived geometry present" );
		Check( DeriveMatchesReparse( d2 ), "edited CST derives the same Job as a fresh parse of its text" );
		// the new radius actually took (bsphere == 0.9)
		char b[64]; std::snprintf( b, sizeof(b), "bsphere=%.17g", (double)(Scalar)0.9 );
		Check( DeriveDump( d2 ).find( b ) != std::string::npos, "derived bsphere == 0.9 (the edit reached the engine)" );
	}

	//----------------------------------------------------------------------
	std::printf( "[replace] whole-chunk DocReplaceItem preserves the position's NodeId\n" );
	{
		Document d = ParseToCst( Scene() );
		const NodeId ball = DocFindByName( d, "sphere_geometry/ball" );
		NodeRef item; const int idx = DocIndexOfNodeId( d, ball, &item );
		Check( idx >= 0, "ball located by NodeId" );
		Document d2 = DocReplaceItem( d, idx, ItemFromText( "sphere_geometry\n{\nname ball\nradius 3\n}\n" ) );
		Check( DocFindByName( d2, "sphere_geometry/ball" ) == ball, "replaced chunk keeps the position's NodeId" );
		char b[64]; std::snprintf( b, sizeof(b), "bsphere=%.17g", (double)(Scalar)3 );
		Check( DeriveDump( d2 ).find( b ) != std::string::npos && DeriveMatchesReparse( d2 ), "replace derives radius 3, faithfully" );
	}

	//----------------------------------------------------------------------
	std::printf( "[erase]/[insert] item count changes flow to the derived Job\n" );
	{
		Document d = ParseToCst( Scene() );
		const int n0 = DocItemCount( d );
		// erase the object chunk
		const NodeId obj = DocFindByName( d, "standard_object/obj" );
		NodeRef oi; const int oidx = DocIndexOfNodeId( d, obj, &oi );
		Document de = DocEraseItem( d, oidx );
		Check( DocItemCount( de ) == n0 - 1, "erase removes one top-level item" );
		Check( DocFindByName( de, "standard_object/obj" ) == 0, "erased object no longer addressable" );
		Check( DeriveMatchesReparse( de ), "post-erase CST derives faithfully (object gone)" );
		// insert a new geometry before the object's slot: a valid insert is the
		// chunk item PLUS a separator trivia item (inter-chunk newlines are their
		// own rope items), so the serialization stays well-formed.
		Document di = DocInsertItem( d,  oidx,     ItemFromText( "sphere_geometry\n{\nname ball2\nradius 1.5\n}" ) );
		di          = DocInsertItem( di, oidx + 1, ItemFromText( "\n" ) );
		Check( DocItemCount( di ) == n0 + 2, "insert adds the chunk + its separator (two items)" );
		Check( DocFindByName( di, "sphere_geometry/ball2" ) != 0, "inserted geometry addressable by name" );
		Check( DeriveMatchesReparse( di ), "inserted geometry derives faithfully (well-formed serialization)" );
	}

	//----------------------------------------------------------------------
	std::printf( "[reparse] free-form text change carries NodeIds; derive stays consistent\n" );
	{
		Document d = ParseToCst( Scene() );
		const NodeId red = DocFindByName( d, "uniformcolor_painter/red" );
		// change red's colour via a free-form reparse (value edit, name unchanged)
		std::string text = Scene();
		const size_t p = text.find( "color 0.2 0.4 0.6" );
		text.replace( p, std::string("color 0.2 0.4 0.6").size(), "color 0.9 0.1 0.1" );
		Document d2 = DocReparse( d, text );
		Check( DocFindByName( d2, "uniformcolor_painter/red" ) == red, "red's NodeId carried across the reparse (value edit keeps lineage)" );
		Check( SerializeCst( d2 ) == text, "reparsed Document serializes to the new text" );
		Check( DeriveMatchesReparse( d2 ), "reparsed CST derives faithfully" );
	}

	//----------------------------------------------------------------------
	std::printf( "[rename] DocRename: NodeId lineage + referrer rewrite from the traced graph (D14)\n" );
	{
		Document d = ParseToCst( Scene() );
		const NodeId red    = DocFindByName( d, "uniformcolor_painter/red" );
		const NodeId redmat = DocFindByName( d, "lambertian_material/redmat" );
		// before: the painter is referred to by redmat.reflectance
		std::vector<ReferenceUse> before = TraceReferences( d );
		bool refBefore = false;
		for( const auto& u : before ) if( u.targetNodeId == red && u.sourceValueNodeId == DocParamId( d, redmat, "reflectance" ) ) refBefore = true;
		Check( refBefore, "before: redmat.reflectance -> red" );

		std::vector<std::string> diags;
		Document d2 = DocRename( d, red, "crimson", &diags );
		Check( diags.empty(), "rename reports no un-rewritable referrers" );
		// lineage: the SAME NodeId is now addressed by the new name; the old name is gone
		Check( DocFindByName( d2, "uniformcolor_painter/crimson" ) == red, "renamed chunk keeps its NodeId (now named crimson) -- lineage survives rename" );
		Check( DocFindByName( d2, "uniformcolor_painter/red" ) == 0, "old name no longer resolves" );
		// referrer rewritten: the reference still points at the same chunk, via the new name
		std::vector<ReferenceUse> after = TraceReferences( d2 );
		bool refAfter = false;
		for( const auto& u : after ) if( u.targetNodeId == red && u.sourceValueNodeId == DocParamId( d2, redmat, "reflectance" ) ) refAfter = true;
		Check( refAfter, "after: redmat.reflectance -> the SAME painter chunk, via 'crimson'" );
		Check( SerializeCst( d2 ).find( "name crimson" ) != std::string::npos
		    && SerializeCst( d2 ).find( "reflectance crimson" ) != std::string::npos, "both the name and the referrer rewritten in the text" );
		// the painter's old name line and the old reference are gone (NOT a bare
		// substring check -- the material 'redmat' legitimately contains 'red').
		Check( SerializeCst( d2 ).find( "name red\n" ) == std::string::npos
		    && SerializeCst( d2 ).find( "reflectance red\n" ) == std::string::npos, "no stale painter-name 'red' or reference to it remains" );
		// derive: the renamed scene still resolves (the material's painter exists under the new name)
		Check( DeriveMatchesReparse( d2 ), "renamed CST derives faithfully (reference resolves under the new name)" );
		// redmat's NodeId also survived (only a value was rewritten)
		Check( DocFindByName( d2, "lambertian_material/redmat" ) == redmat, "the referrer chunk (redmat) keeps its NodeId" );
	}

	//----------------------------------------------------------------------
	std::printf( "[rename-tuple] a tuple-reference referrer is REWRITTEN (ref token swapped, other tokens intact) -- slice 2 atomic rename\n" );
	{
		// advanced_shader.shaderop carries the reference as ONE token of a multi-token
		// value (`dl 0 1 +`). Slice 2's atomic rename rewrites the reference TOKEN
		// (dl -> dl2) while leaving the other tuple tokens (0 1 +) intact -- a complete
		// rename (no partial / dangling referrer), no diagnostic.
		const std::string s =
			"RISE ASCII SCENE 6\n"
			"directlighting_shaderop\n{\nname dl\n}\n"
			"advanced_shader\n{\nname sh\nshaderop dl 0 1 +\n}\n";
		Document d = ParseToCst( s );
		const NodeId dl = DocFindByName( d, "directlighting_shaderop/dl" );
		std::vector<std::string> diags;
		Document d2 = DocRename( d, dl, "dl2", &diags );
		const std::string out = SerializeCst( d2 );
		Check( DocFindByName( d2, "directlighting_shaderop/dl2" ) == dl, "the shader-op chunk is renamed, NodeId preserved" );
		Check( out.find( "shaderop dl2 0 1 +" ) != std::string::npos, "the tuple reference TOKEN is rewritten (dl -> dl2), other tokens (0 1 +) intact" );
		Check( out.find( "shaderop dl 0 1 +" ) == std::string::npos, "the old tuple reference (dl) is gone -- no dangling referrer" );
		Check( diags.empty(), "no diagnostic -- the tuple referrer was rewritten, not left partial" );
	}

	//----------------------------------------------------------------------
	std::printf( "[rename-collision] renaming to an existing same-category name is refused atomically\n" );
	{
		const std::string s =
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname a\ncolor 1 0 0\n}\n"
			"uniformcolor_painter\n{\nname b\ncolor 0 1 0\n}\n"
			"lambertian_material\n{\nname m\nreflectance b\n}\n";
		Document d = ParseToCst( s );
		const NodeId b = DocFindByName( d, "uniformcolor_painter/b" );
		std::vector<std::string> diags;
		Document d2 = DocRename( d, b, "a", &diags );   // collide with painter 'a' (same category)
		Check( !diags.empty(), "collision is reported in diagnostics" );
		Check( d2.items.get() == d.items.get(), "refused ATOMICALLY: Document unchanged (no silent re-target of the referrer)" );
		Check( DocFindByName( d2, "uniformcolor_painter/b" ) == b, "'b' is still named 'b' (not renamed into the collision)" );
		// a NON-colliding rename of the same chunk still succeeds + rewrites the referrer
		std::vector<std::string> diags2;
		Document d3 = DocRename( d, b, "green", &diags2 );
		Check( diags2.empty() && DocFindByName( d3, "uniformcolor_painter/green" ) == b
		    && SerializeCst( d3 ).find( "reflectance green" ) != std::string::npos,
		       "a non-colliding rename of the same chunk succeeds + rewrites the referrer" );
		// renaming a chunk with NO name param is a DIAGNOSED no-op (not silent)
		Document dn = ParseToCst( "RISE ASCII SCENE 6\nsphere_geometry\n{\nradius 1\n}\n" );
		NodeId unnamed = 0;
		for( int i = 0; i < DocItemCount( dn ); ++i ) {
			NodeRef it = DocResolveNodeId( dn, DocNodeIdAt( dn, i ) );
			if( it && it->kind == NodeKind::Chunk ) { unnamed = DocNodeIdAt( dn, i ); break; }
		}
		std::vector<std::string> dgn;
		Document dn2 = DocRename( dn, unnamed, "x", &dgn );
		Check( unnamed != 0 && !dgn.empty() && dn2.items.get() == dn.items.get(), "renaming a name-less chunk is a diagnosed no-op (not silent)" );
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
