//////////////////////////////////////////////////////////////////////
//
//  CstSaveFidelityTest.cpp - P5 (save-as-CST) Slice 0 down-payment.
//
//  Pins the MINIMAL-DIFF-ON-EDIT property that Model-B P5 relies on: when a
//  value is edited (DocSetParamValue) and the document is re-serialized
//  (SerializeCst), the output is byte-identical to the original EXCEPT at the
//  edited value -- comments, indentation (tabs/spaces), blank lines, trailing
//  comments on the edited line, and every other parameter are preserved
//  verbatim (structural sharing).  This is the guarantee that lets "save =
//  SerializeCst(head)" replace the legacy SaveEngine's verbatim-except-the-
//  value byte-splice without regressing round-trip fidelity (G5 in
//  docs/agentic-redesign/62-model-b-p5-save-as-cst-plan.md).
//
//  The ONE intentional normalization (F-P5.3): the edited value itself is
//  re-tokenized SINGLE-SPACED (WithParamValue, Cst.cpp), so a multi-token value
//  with custom internal alignment collapses to single spaces on edit.  Nothing
//  ELSE changes.  Case F below pins exactly that boundary.
//
//  Each case asserts SerializeCst(edited) == the original with ONLY the edited
//  value's bytes replaced -- the strongest (byte-exact) form of the property.
//
//  (The complementary load half -- text -> Migrate -> ParseToCst -> DeriveToJob
//  == legacy -- is already covered by CstDeriveDifferentialTest + the
//  CstCorpusEquivalenceTest corpus gate; Slice 0 adds only this save-fidelity
//  pin, which was the one missing piece.)
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <string>

#include "../src/Library/Cst/Cst.h"

using namespace RISE;
using namespace RISE::Cst;

namespace
{
	int s_pass = 0, s_fail = 0;
	void Check( bool ok, const char* what ) { if( ok ) ++s_pass; else { ++s_fail; std::printf( "  FAIL: %s\n", what ); } }

	// Edit the occ-th `role` value of the chunk named `catName` and return the re-serialized text.
	std::string EditOne( const std::string& text, const char* catName, const char* role, int occ, const std::string& nv )
	{
		Document d = ParseToCst( text );
		const NodeId id = DocFindByName( d, catName );
		if( id == 0 ) return std::string( "<name-not-found>" );
		return SerializeCst( DocSetParamValue( d, id, role, occ, nv ) );
	}

	// Original with the FIRST occurrence of `a` replaced by `b` -- the expected minimal-diff output.
	std::string Replace1( std::string s, const std::string& a, const std::string& b )
	{
		const size_t i = s.find( a );
		if( i != std::string::npos ) s.replace( i, a.size(), b );
		return s;
	}

	// Build a standalone item NodeRef from `src` (its first top-level item) -- the chunk
	// to splice in for the insert-then-remove round-trip cases.
	Cst::NodeRef FirstChunk( const std::string& src )
	{
		Document d = ParseToCst( src );
		NodeRef item; size_t start = 0; int v = 0;
		DocItemAtByteOffset( d, 0, &item, &start, &v );
		return item;
	}
}

int main()
{
	std::printf( "=== CstSaveFidelityTest (P5 Slice 0: minimal-diff-on-edit) ===\n" );

	// A. Unedited round-trip is byte-exact (the SerializeCst(ParseToCst(t)) == t invariant, reaffirmed
	//    here on a comment+blank-line+tab scene so the edit cases below build on a proven baseline).
	{
		const std::string t =
			"RISE ASCII SCENE 6\n# header comment\nsphere_geometry\n{\n\tradius 1\n\n\tname s   # trailing\n}\n";
		Check( SerializeCst( ParseToCst( t ) ) == t, "A: unedited round-trip is byte-exact (comments/tabs/blank lines preserved)" );
	}

	// B. A scalar edit changes ONLY the edited value -- a leading comment, other params, and the
	//    surrounding structure are byte-identical.
	{
		const std::string t   = "RISE ASCII SCENE 6\nsphere_geometry\n{\n# a sphere\nradius 1\nname s\n}\n";
		const std::string exp = Replace1( t, "radius 1", "radius 2" );
		const std::string got = EditOne( t, "sphere_geometry/s", "radius", 0, "2" );
		Check( got == exp, "B: scalar edit is minimal-diff (only the edited value's bytes change)" );
		Check( got != t,   "B: the edit actually took effect (output differs from the original)" );
	}

	// C. A TRAILING COMMENT on the edited line survives the edit (the comment is sibling trivia, not
	//    part of the replaced value).
	{
		const std::string t   = "RISE ASCII SCENE 6\nsphere_geometry\n{\nradius 1   # the radius\nname s\n}\n";
		const std::string exp = Replace1( t, "radius 1", "radius 2" );   // -> "radius 2   # the radius"
		const std::string got = EditOne( t, "sphere_geometry/s", "radius", 0, "2" );
		Check( got == exp, "C: trailing comment on the edited line is preserved verbatim" );
		Check( got.find( "# the radius" ) != std::string::npos, "C: the comment text survives" );
	}

	// D. Leading indentation (a TAB) and an adjacent BLANK LINE are preserved across the edit.
	{
		const std::string t   = "RISE ASCII SCENE 6\nsphere_geometry\n{\n\tradius 1\n\n\tname s\n}\n";
		const std::string exp = Replace1( t, "radius 1", "radius 2" );
		const std::string got = EditOne( t, "sphere_geometry/s", "radius", 0, "2" );
		Check( got == exp, "D: tab indent + adjacent blank line preserved across the edit" );
	}

	// E. A MULTI-TOKEN value edit changes only that value's line; everything else is verbatim.
	{
		const std::string t   = "RISE ASCII SCENE 6\nuniformcolor_painter\n{\nname p\ncolor 1 0 0\n}\n";
		const std::string exp = Replace1( t, "color 1 0 0", "color 0.9 0.1 0.1" );
		const std::string got = EditOne( t, "uniformcolor_painter/p", "color", 0, "0.9 0.1 0.1" );
		Check( got == exp, "E: multi-token value edit is minimal-diff" );
	}

	// F. THE ONE NORMALIZATION (F-P5.3): editing a multi-token value re-tokenizes it SINGLE-SPACED,
	//    so custom internal alignment in the EDITED value collapses -- and nothing else moves.  Here
	//    the value is re-set to the same logical "1 0 0"; the 3-space gaps normalize to single spaces.
	{
		const std::string t   = "RISE ASCII SCENE 6\nuniformcolor_painter\n{\nname p\ncolor 1   0   0\n}\n";
		const std::string exp = Replace1( t, "color 1   0   0", "color 1 0 0" );
		const std::string got = EditOne( t, "uniformcolor_painter/p", "color", 0, "1 0 0" );
		Check( got == exp, "F: editing a multi-token value normalizes ITS internal spacing (single-spaced), nothing else" );
		Check( got != t,   "F: the normalization is observable (the edited value's gaps collapsed)" );
	}

	// ---- DocRemoveItem (inverse of DocInsertItem) ----

	// G. INSERT-THEN-REMOVE ROUND-TRIP: parse a doc, insert a new chunk at index i, then
	//    remove the SAME index -> SerializeCst is byte-identical to the original.  The
	//    insert/remove pair is an exact no-op on the serialized bytes.
	{
		const std::string t =
			"RISE ASCII SCENE 6\n"
			"sphere_geometry\n{\nname a\nradius 1\n}\n"
			"sphere_geometry\n{\nname b\nradius 2\n}\n";
		Document d0  = ParseToCst( t );
		Document ins = DocInsertItem( d0, 1, FirstChunk( "sphere_geometry\n{\nname mid\nradius 9\n}\n" ) );
		Check( SerializeCst( ins ).find( "name mid" ) != std::string::npos, "G: the insert actually took effect (chunk present after insert)" );
		int v = 0;
		Document rem = DocRemoveItem( ins, 1, &v );
		Check( SerializeCst( rem ) == t, "G: insert-then-remove round-trips byte-identically to the original" );
		Check( DocItemCount( rem ) == DocItemCount( d0 ), "G: item count restored after the round-trip" );
	}

	// H. SIBLING IDENTITY PRESERVED: removing a MIDDLE item leaves every OTHER item's
	//    NodeId UNCHANGED (only the removed item's id disappears).  Chunks are addressed
	//    by name (DocFindByName -> DocIndexOfNodeId), since the magic header "RISE ASCII
	//    SCENE 6" tokenizes into leading stray leaf items ahead of the chunks.
	{
		const std::string t =
			"RISE ASCII SCENE 6\n"
			"sphere_geometry\n{\nname a\nradius 1\n}\n"
			"sphere_geometry\n{\nname b\nradius 2\n}\n"
			"sphere_geometry\n{\nname c\nradius 3\n}\n";
		Document d   = ParseToCst( t );
		const int n  = DocItemCount( d );
		const NodeId idFirst = DocFindByName( d, "sphere_geometry/a" );
		const NodeId idMid   = DocFindByName( d, "sphere_geometry/b" );   // to be removed
		const NodeId idLast  = DocFindByName( d, "sphere_geometry/c" );
		Check( idFirst && idMid && idLast, "H: all three chunks resolve by name before the remove" );

		const NodeId idMidPosBefore = DocNodeIdAt( d, DocIndexOfNodeId( d, idMid, nullptr ) - 1 );   // the item just BEFORE "b"
		Document rem = DocRemoveItem( d, DocIndexOfNodeId( d, idMid, nullptr ) );                     // drop the middle chunk "b"
		Check( DocItemCount( rem ) == n - 1, "H: removing a middle item drops exactly one item" );
		Check( DocResolveNodeId( rem, idMid ) == nullptr, "H: the removed item's NodeId no longer resolves" );
		Check( DocFindByName( rem, "sphere_geometry/b" ) == 0, "H: the removed item's name no longer resolves" );
		Check( DocFindByName( rem, "sphere_geometry/a" ) == idFirst, "H: the sibling BEFORE the removed item keeps its NodeId" );
		Check( DocFindByName( rem, "sphere_geometry/c" ) == idLast,  "H: the sibling AFTER the removed item keeps its NodeId" );
		Check( DocResolveNodeId( rem, idFirst ) != nullptr && DocResolveNodeId( rem, idLast ) != nullptr, "H: both survivors still resolve" );
		Check( DocResolveNodeId( rem, idMidPosBefore ) != nullptr, "H: the item immediately preceding the removed one is untouched" );
	}

	// I. REMOVE A STANDALONE-AUTHORED CHUNK from a multi-chunk doc -> the remaining text is
	//    exactly the original with that chunk's own bytes excised (resolve by name, then
	//    remove by index).  The chunk item spans `keyword { ... }`; its trailing newline is
	//    a SEPARATE trivia leaf (the inter-chunk separator), so a single-item removal of the
	//    chunk leaves that separator behind -- the expected text below reflects exactly that
	//    (the AddCamera cutover removes the orphan separator as a second DocRemoveItem).
	{
		const std::string chunkBytes = "lambertian_material\n{\nname m\nreflectance p\n}";
		const std::string t =
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			+ chunkBytes + "\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n";
		const std::string exp = Replace1( t, chunkBytes, "" );   // original minus exactly the chunk item's own bytes
		Document d   = ParseToCst( t );
		NodeRef  it  = NodeRef();
		const NodeId mid = DocFindByName( d, "lambertian_material/m" );
		const int    idx = DocIndexOfNodeId( d, mid, &it );
		Check( mid != 0 && idx >= 0, "I: the named chunk resolves to a top-level index" );
		Document rem = DocRemoveItem( d, idx );
		Check( SerializeCst( rem ) == exp, "I: removing a named chunk yields the original minus exactly that chunk item's bytes" );
		Check( SerializeCst( rem ).find( "reflectance p" ) == std::string::npos, "I: the removed chunk's content is gone" );
		Check( SerializeCst( rem ).find( "name p" ) != std::string::npos && SerializeCst( rem ).find( "name g" ) != std::string::npos, "I: the surviving chunks' content remains" );
	}

	// J. Out-of-range index is a no-op (mirrors DocInsertItem's bounds clamp).
	{
		const std::string t = "RISE ASCII SCENE 6\nsphere_geometry\n{\nname a\nradius 1\n}\n";
		Document d = ParseToCst( t );
		Check( SerializeCst( DocRemoveItem( d, -1 ) ) == t, "J: DocRemoveItem at index<0 is a no-op" );
		Check( SerializeCst( DocRemoveItem( d, DocItemCount(d) ) ) == t, "J: DocRemoveItem past the end is a no-op" );
	}

	std::printf( "%d passed, %d failed.\n", s_pass, s_fail );
	return s_fail == 0 ? 0 : 1;
}
