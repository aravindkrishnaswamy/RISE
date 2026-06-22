//////////////////////////////////////////////////////////////////////
//
//  CstIdentityTest.cpp - transfer-gate item 4: NodeId identity + name-path
//  addressing (the name-path half of the counted lookup; the byte-offset half
//  is item 3).
//
//  Proves the item-3 review's acceptance bar for item 4:
//   * a SEPARATE persistent identity side-map (occurrence -> NodeId), NOT a
//     field on the green/seq node -- so a NodeId is NOT derivable from content:
//     a value edit changes the green node yet the id at that slot persists;
//   * name-path resolution is COUNTED and O(log N) (a balanced name index), not
//     an O(N) scan;
//   * identity survives a value edit (the unchanged chunk keeps its id) AND an
//     insert/erase index shift (position moves; the NodeId moves WITH its item);
//   * a within-chunk descent resolves a byte offset to the Param-in-chunk (the
//     "edit geometry/s.radius" path);
//   * reparse re-matches by content-key: a value edit keeps its id, a reorder of
//     distinct chunks keeps ids regardless of position, a RENAME of a unique-of-type
//     chunk KEEPS its id (lineage survives rename + reparse, D9/D44); only a
//     genuinely-ambiguous row is invalidated rather than position-remapped (D9/D15).
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Cst/Cst.h"

#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

using namespace RISE;

static int g_pass = 0, g_fail = 0;
static void Check( bool cond, const char* what ) { if( cond ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", what ); } }
static int CeilLog2( int n ) { int b = 0; while( (1 << b) < n ) ++b; return b; }

// An N-sphere scene (header strays + N uniquely-named chunks), as in the item-3
// cost test: items are [RISE, ASCII, SCENE, 6, chunk s0, ... chunk s(N-1)].
static std::string SceneN( int n )
{
	std::string s = "RISE ASCII SCENE 6\n";
	for( int i = 0; i < n; ++i )
		s += "sphere_geometry\n{\nname s" + std::to_string(i) + "\nradius 0." + std::to_string(100 + i) + "\n}\n";
	return s;
}
static Cst::NodeRef FirstChunk( const std::string& src )
{
	Cst::Document d = Cst::ParseToCst( src );
	Cst::NodeRef item; size_t start = 0; int v = 0;
	Cst::DocItemAtByteOffset( d, 0, &item, &start, &v );
	return item;
}
static Cst::NodeRef MakeSphere( const std::string& name, const std::string& radius )
{
	return FirstChunk( "sphere_geometry\n{\nname " + name + "\nradius " + radius + "\n}\n" );
}
static int ChunkIndexAt( const Cst::Document& d, const std::string& scene, const std::string& needle )
{
	size_t off = scene.find( needle );
	Cst::NodeRef it; size_t st = 0; int v = 0;
	return Cst::DocItemAtByteOffset( d, off, &it, &st, &v );
}

int main()
{
	std::printf( "CstIdentityTest -- transfer-gate item 4 (NodeId identity + name-path addressing)\n" );

	// ============================================================
	// [name] name-path resolution is COUNTED and O(log N), not O(N).
	// ============================================================
	const int Ns[] = { 8, 64, 512 };
	for( int N : Ns ) {
		const std::string scene = SceneN( N );
		Cst::Document doc = Cst::ParseToCst( scene );
		char m[128];

		int v = 0;
		Cst::NodeId id = Cst::DocFindByName( doc, "sphere_geometry/s" + std::to_string(N/2), &v );
		std::snprintf( m, sizeof(m), "N=%d: name-path resolves to a NodeId", N );
		Check( id != 0, m );

		const int bound = 3 * ( CeilLog2(N) + 1 );   // byName has N entries
		std::snprintf( m, sizeof(m), "N=%d: name lookup visits %d <= ~3*log2(N)=%d (NOT O(N)=%d)", N, v, bound, N );
		Check( v > 0 && v <= bound, m );

		int vm = 0;
		std::snprintf( m, sizeof(m), "N=%d: unknown name-path resolves to 0", N );
		Check( Cst::DocFindByName( doc, "sphere_geometry/nope", &vm ) == 0, m );

		std::printf( "  N=%4d  byName entries=%d  findVisits=%d  bound=%d\n", N, N, v, bound );

		// worst-case over ALL keys, not just the midpoint (a degenerate tree could
		// hide behind a shallow midpoint), and prove sub-linearity at scale.
		int maxV = 0;
		for( int k = 0; k < N; ++k ) {
			int vk = 0; Cst::DocFindByName( doc, "sphere_geometry/s" + std::to_string(k), &vk );
			if( vk > maxV ) maxV = vk;
		}
		std::snprintf( m, sizeof(m), "N=%d: worst-case name lookup over all keys = %d <= bound %d", N, maxV, bound );
		Check( maxV > 0 && maxV <= bound, m );
		std::snprintf( m, sizeof(m), "N=%d: name index sub-linear at scale (not a degenerate list)", N );
		Check( N < 64 || maxV * 2 < N, m );
	}

	// ============================================================
	// [side-map] identity is a SEPARATE side-map: a value edit changes the green
	// node at a slot, yet the NodeId at that slot persists (id is NOT content).
	// ============================================================
	{
		const std::string scene = SceneN( 8 );
		Cst::Document doc = Cst::ParseToCst( scene );
		const int i4 = ChunkIndexAt( doc, scene, "name s4" );
		Cst::NodeId id4 = Cst::DocNodeIdAt( doc, i4 );
		Check( id4 != 0, "[side-map] the chunk at the slot has a NodeId" );

		Cst::Document doc2 = Cst::DocReplaceItem( doc, i4, MakeSphere( "s4", "9.9" ) );   // same name, NEW green node
		Check( Cst::SerializeCst(doc2) != scene, "[side-map] the value edit changed the document bytes (green node differs)" );
		Check( Cst::DocNodeIdAt( doc2, i4 ) == id4, "[side-map] yet the NodeId at the slot persists -> identity is a side-map, not green-derived" );
		Check( Cst::DocFindByName( doc2, "sphere_geometry/s4" ) == id4, "[side-map] the name index still maps s4 to the same id" );

		// siblings keep their ids too (only the edited slot's green node changed)
		const int i3 = ChunkIndexAt( doc, scene, "name s3" );
		Cst::NodeId id3 = Cst::DocNodeIdAt( doc, i3 );
		Check( Cst::DocNodeIdAt( doc2, i3 ) == id3, "[side-map] a sibling's NodeId is unchanged by the edit" );
	}

	// ============================================================
	// [insert] insert shifts positions; the NodeId moves WITH its item. A durable
	// NodeId ref still resolves; the inserted item gets a fresh id.
	// ============================================================
	{
		const std::string scene = SceneN( 8 );
		Cst::Document doc = Cst::ParseToCst( scene );
		const int i4 = ChunkIndexAt( doc, scene, "name s4" );
		Cst::NodeId id4 = Cst::DocNodeIdAt( doc, i4 );
		const Cst::NodeId freshExpected = doc.nextId;

		Cst::Document doc2 = Cst::DocInsertItem( doc, 0, MakeSphere( "ins", "0.5" ) );   // insert at the very front
		Check( Cst::DocItemCount(doc2) == Cst::DocItemCount(doc) + 1, "[insert] item count grows by one" );
		Check( Cst::DocNodeIdAt( doc2, i4 + 1 ) == id4, "[insert] s4 shifted to index+1, carrying its NodeId (position moved, identity did not)" );

		Cst::NodeRef it; int pos = Cst::DocIndexOfNodeId( doc2, id4, &it );
		Check( pos == i4 + 1 && it, "[insert] durable NodeId ref resolves to the shifted position" );
		Check( Cst::DocFindByName( doc2, "sphere_geometry/s4" ) == id4, "[insert] name index survived the insert (s4 -> same id)" );
		Check( Cst::DocFindByName( doc2, "sphere_geometry/ins" ) != 0, "[insert] the inserted chunk is findable by name" );

		Cst::NodeId insId = Cst::DocNodeIdAt( doc2, 0 );
		Check( insId == freshExpected, "[insert] the inserted item got a fresh id (== old nextId)" );
		Check( Cst::DocIndexOfNodeId( doc, insId, nullptr ) == -1, "[insert] that fresh id did not exist in the old document" );
	}

	// ============================================================
	// [erase] erase shifts positions down; the NodeId moves with its item; the
	// name index drops the erased name.
	// ============================================================
	{
		const std::string scene = SceneN( 8 );
		Cst::Document doc = Cst::ParseToCst( scene );
		const int i5 = ChunkIndexAt( doc, scene, "name s5" );
		Cst::NodeId id5 = Cst::DocNodeIdAt( doc, i5 );
		const int i0 = ChunkIndexAt( doc, scene, "name s0" );
		Cst::NodeId id0 = Cst::DocNodeIdAt( doc, i0 );

		Cst::Document doc2 = Cst::DocEraseItem( doc, i0 );   // erase chunk s0
		Check( Cst::DocItemCount(doc2) == Cst::DocItemCount(doc) - 1, "[erase] item count shrinks by one" );
		Check( Cst::DocNodeIdAt( doc2, i5 - 1 ) == id5, "[erase] s5 shifted down by one, carrying its NodeId" );
		Check( Cst::DocFindByName( doc2, "sphere_geometry/s5" ) == id5, "[erase] name index survived the erase (s5 -> same id)" );
		Check( Cst::DocFindByName( doc2, "sphere_geometry/s0" ) == 0, "[erase] the erased chunk's name no longer resolves" );
		Check( Cst::DocIndexOfNodeId( doc2, id0, nullptr ) == -1, "[erase] the erased item's id is gone from the side-map" );
	}

	// ============================================================
	// [rename] a STRUCTURED rename (DocReplaceItem with a new name) PRESERVES the
	// NodeId and re-keys the name index (identity != name).
	// ============================================================
	{
		const std::string scene = SceneN( 4 );
		Cst::Document doc = Cst::ParseToCst( scene );
		const int i2 = ChunkIndexAt( doc, scene, "name s2" );
		Cst::NodeId id2 = Cst::DocNodeIdAt( doc, i2 );

		Cst::Document doc2 = Cst::DocReplaceItem( doc, i2, MakeSphere( "renamed", "0.102" ) );
		Check( Cst::DocNodeIdAt( doc2, i2 ) == id2, "[rename] structured rename keeps the NodeId at the slot" );
		Check( Cst::DocFindByName( doc2, "sphere_geometry/s2" ) == 0, "[rename] the old name no longer resolves" );
		Check( Cst::DocFindByName( doc2, "sphere_geometry/renamed" ) == id2, "[rename] the new name resolves to the SAME id" );
	}

	// ============================================================
	// [within-chunk] resolve a byte offset to the innermost Param-in-chunk.
	// ============================================================
	{
		const std::string scene = "RISE ASCII SCENE 6\nsphere_geometry\n{\nname s\nradius 0.6\n}\n";
		Cst::Document doc = Cst::ParseToCst( scene );
		Cst::NodeRef chunk; int v = 0;

		Cst::NodeRef pr = Cst::DocParamAtByteOffset( doc, scene.find("0.6"), &chunk, &v );
		Check( pr && pr->kind == Cst::NodeKind::Param && pr->role == "radius", "[within] offset in the radius value -> radius Param" );
		Check( chunk && chunk->kind == Cst::NodeKind::Chunk && chunk->role == "sphere_geometry", "[within] the enclosing chunk is returned" );

		Cst::NodeRef pn = Cst::DocParamAtByteOffset( doc, scene.find("name s") + 1, &chunk, &v );
		Check( pn && pn->kind == Cst::NodeKind::Param && pn->role == "name", "[within] offset in the name parameter -> name Param" );

		Cst::NodeRef pk = Cst::DocParamAtByteOffset( doc, scene.find("sphere_geometry"), &chunk, &v );
		Check( !pk && chunk && chunk->role == "sphere_geometry", "[within] offset on the keyword -> null Param but chunk set" );

		Cst::NodeRef ph = Cst::DocParamAtByteOffset( doc, 0, &chunk, &v );
		Check( !ph && !chunk, "[within] offset in the header stray -> null (not a chunk param)" );
	}

	// ============================================================
	// [reparse] free-form reparse re-matches identity by content-key.
	// ============================================================
	{
		const std::string base = "RISE ASCII SCENE 6\nsphere_geometry\n{\nname s\nradius 0.6\n}\n";
		Cst::Document old = Cst::ParseToCst( base );
		Cst::NodeId idS = Cst::DocFindByName( old, "sphere_geometry/s" );
		Check( idS != 0, "[reparse] baseline name resolves" );

		// value edit via reparse: id persists, nothing invalidated, bytes updated
		const std::string edited = "RISE ASCII SCENE 6\nsphere_geometry\n{\nname s\nradius 0.66\n}\n";
		std::vector<Cst::NodeId> inv;
		Cst::Document neu = Cst::DocReparse( old, edited, &inv );
		Check( Cst::DocFindByName( neu, "sphere_geometry/s" ) == idS, "[reparse] value edit: the chunk keeps its NodeId" );
		Check( inv.empty(), "[reparse] value edit: nothing invalidated" );
		Check( Cst::SerializeCst( neu ) == edited, "[reparse] value edit: the new text round-trips" );

		// rename via reparse: lineage SURVIVES (D9/D44 best-effort) -- the unique
		// chunk keeps its id; only the name-path key moves.
		const std::string renamed = "RISE ASCII SCENE 6\nsphere_geometry\n{\nname s2\nradius 0.6\n}\n";
		std::vector<Cst::NodeId> inv2;
		Cst::Document neu2 = Cst::DocReparse( old, renamed, &inv2 );
		Check( Cst::DocFindByName( neu2, "sphere_geometry/s" ) == 0, "[reparse] rename: the old name no longer resolves" );
		Check( Cst::DocFindByName( neu2, "sphere_geometry/s2" ) == idS, "[reparse] rename: the unique chunk KEEPS its id (lineage survives rename, D9/D44)" );
		Check( inv2.empty(), "[reparse] rename: nothing invalidated (lineage carried, not delete+add)" );
		Check( Cst::DocResolveNodeId( neu2, idS ) != nullptr, "[reparse] rename: the carried id resolves to the renamed node" );

		// reorder of distinct chunks via reparse: ids carried by content-key, not position
		const std::string two     = "RISE ASCII SCENE 6\nsphere_geometry\n{\nname a\nradius 0.1\n}\nsphere_geometry\n{\nname b\nradius 0.2\n}\n";
		const std::string swapped = "RISE ASCII SCENE 6\nsphere_geometry\n{\nname b\nradius 0.2\n}\nsphere_geometry\n{\nname a\nradius 0.1\n}\n";
		Cst::Document od = Cst::ParseToCst( two );
		Cst::NodeId idA = Cst::DocFindByName( od, "sphere_geometry/a" );
		Cst::NodeId idB = Cst::DocFindByName( od, "sphere_geometry/b" );
		std::vector<Cst::NodeId> inv3;
		Cst::Document rd = Cst::DocReparse( od, swapped, &inv3 );
		Check( Cst::DocFindByName( rd, "sphere_geometry/a" ) == idA && Cst::DocFindByName( rd, "sphere_geometry/b" ) == idB,
		       "[reparse] reorder: ids matched by content-key, not by position" );
		Check( inv3.empty(), "[reparse] reorder: nothing invalidated" );
	}

	// ============================================================
	// [dup] duplicate name-paths must not corrupt the name index under edits
	// (a degenerate scene the derive layer rejects, but the CST holds losslessly).
	// ============================================================
	{
		const std::string scene = "RISE ASCII SCENE 6\nsphere_geometry\n{\nname s\nradius 0.1\n}\nsphere_geometry\n{\nname s\nradius 0.2\n}\n";
		Cst::Document doc = Cst::ParseToCst( scene );
		const int iA = ChunkIndexAt( doc, scene, "0.1" );
		const int iB = ChunkIndexAt( doc, scene, "0.2" );
		Cst::NodeId idA = Cst::DocNodeIdAt( doc, iA );
		Cst::NodeId idB = Cst::DocNodeIdAt( doc, iB );
		Check( idA != 0 && idB != 0 && idA != idB, "[dup] two same-named chunks get distinct NodeIds" );
		int occ = 0;
		Check( Cst::DocFindByName( doc, "sphere_geometry/s", nullptr, &occ ) == 0 && occ == 2, "[dup] find REFUSES an ambiguous duplicate name (0, occurrences=2)" );
		Check( Cst::DocResolveNodeId( doc, idA ) && Cst::DocResolveNodeId( doc, idB ), "[dup] both duplicate chunks remain resolvable by durable NodeId" );

		Cst::Document e1 = Cst::DocEraseItem( doc, iA );
		Check( Cst::DocFindByName( e1, "sphere_geometry/s" ) == idB, "[dup] erase first twin: survivor still resolves (no silent corruption)" );
		Cst::Document e2 = Cst::DocEraseItem( doc, iB );
		Check( Cst::DocFindByName( e2, "sphere_geometry/s" ) == idA, "[dup] erase second twin: first survives" );

		Cst::Document r1 = Cst::DocReplaceItem( doc, iA, MakeSphere( "q", "0.1" ) );
		Check( Cst::DocFindByName( r1, "sphere_geometry/s" ) == idB, "[dup] rename first twin away: the other s still resolves" );
		Check( Cst::DocFindByName( r1, "sphere_geometry/q" ) == idA, "[dup] rename first twin away: new name resolves to its id" );
	}
	{
		// rename INTO an existing name must not hijack the original's binding
		const std::string scene = "RISE ASCII SCENE 6\nsphere_geometry\n{\nname a\nradius 0.1\n}\nsphere_geometry\n{\nname b\nradius 0.2\n}\n";
		Cst::Document doc = Cst::ParseToCst( scene );
		const int iA = ChunkIndexAt( doc, scene, "0.1" );
		Cst::NodeId idB = Cst::DocFindByName( doc, "sphere_geometry/b" );
		Cst::NodeId idA = Cst::DocNodeIdAt( doc, iA );
		Cst::Document r = Cst::DocReplaceItem( doc, iA, MakeSphere( "b", "0.1" ) );   // a -> b (now two b's)
		int occB = 0; Cst::DocFindByName( r, "sphere_geometry/b", nullptr, &occB );
		Check( occB == 2 && Cst::DocFindByName( r, "sphere_geometry/b" ) == 0, "[dup] rename-into-existing: b is now ambiguous -> refused (no silent hijack)" );
		Check( Cst::DocFindByName( r, "sphere_geometry/a" ) == 0, "[dup] rename-into-existing: the old name a is gone" );
		Check( Cst::DocResolveNodeId( r, idB ) && Cst::DocResolveNodeId( r, idA ), "[dup] rename-into-existing: both b chunks resolvable by NodeId" );
	}

	// ============================================================
	// [reparse-ambig] D15: reorder follows CONTENT; genuinely-ambiguous rows are
	// INVALIDATED, never position-remapped onto an unrelated row.
	// ============================================================
	{
		const std::string two     = "RISE ASCII SCENE 6\nsphere_geometry\n{\nradius 0.1\n}\nsphere_geometry\n{\nradius 0.2\n}\n";
		const std::string swapped = "RISE ASCII SCENE 6\nsphere_geometry\n{\nradius 0.2\n}\nsphere_geometry\n{\nradius 0.1\n}\n";
		Cst::Document od = Cst::ParseToCst( two );
		Cst::NodeId id01 = Cst::DocNodeIdAt( od, ChunkIndexAt( od, two, "0.1" ) );
		Cst::NodeId id02 = Cst::DocNodeIdAt( od, ChunkIndexAt( od, two, "0.2" ) );
		std::vector<Cst::NodeId> inv;
		Cst::Document rd = Cst::DocReparse( od, swapped, &inv );
		Check( Cst::DocNodeIdAt( rd, ChunkIndexAt( rd, swapped, "0.1" ) ) == id01, "[reparse-ambig] unnamed reorder: id follows the 0.1 content, not the slot" );
		Check( Cst::DocNodeIdAt( rd, ChunkIndexAt( rd, swapped, "0.2" ) ) == id02, "[reparse-ambig] unnamed reorder: id follows the 0.2 content" );
		Check( inv.empty(), "[reparse-ambig] unnamed distinct-content reorder: nothing invalidated" );

		const std::string bothEdited = "RISE ASCII SCENE 6\nsphere_geometry\n{\nradius 0.11\n}\nsphere_geometry\n{\nradius 0.22\n}\n";
		std::vector<Cst::NodeId> inv2;
		Cst::Document rd2 = Cst::DocReparse( od, bothEdited, &inv2 );
		bool inv01 = std::find( inv2.begin(), inv2.end(), id01 ) != inv2.end();
		bool inv02 = std::find( inv2.begin(), inv2.end(), id02 ) != inv2.end();
		Check( inv01 && inv02, "[reparse-ambig] both unnamed edited: both old ids INVALIDATED (D15), not position-remapped" );
		Cst::NodeId nid = Cst::DocNodeIdAt( rd2, ChunkIndexAt( rd2, bothEdited, "0.11" ) );
		Check( nid != 0 && nid != id01 && nid != id02, "[reparse-ambig] ambiguous rows get FRESH ids" );
	}
	{
		// mixed named reparse: one value-edited (unique-key pass), one unchanged (full pass)
		const std::string two    = "RISE ASCII SCENE 6\nsphere_geometry\n{\nname a\nradius 0.1\n}\nsphere_geometry\n{\nname b\nradius 0.2\n}\n";
		const std::string edited = "RISE ASCII SCENE 6\nsphere_geometry\n{\nname a\nradius 0.99\n}\nsphere_geometry\n{\nname b\nradius 0.2\n}\n";
		Cst::Document od = Cst::ParseToCst( two );
		Cst::NodeId idA = Cst::DocFindByName( od, "sphere_geometry/a" );
		Cst::NodeId idB = Cst::DocFindByName( od, "sphere_geometry/b" );
		std::vector<Cst::NodeId> inv;
		Cst::Document rd = Cst::DocReparse( od, edited, &inv );
		Check( Cst::DocFindByName( rd, "sphere_geometry/a" ) == idA, "[reparse-ambig] mixed: value-edited named chunk keeps id (unique-key pass)" );
		Check( Cst::DocFindByName( rd, "sphere_geometry/b" ) == idB, "[reparse-ambig] mixed: unchanged sibling keeps id (full-content pass)" );
		Check( inv.empty(), "[reparse-ambig] mixed: nothing invalidated" );
	}

	// ============================================================
	// [reverse] durable NodeId -> current node in counted O(log N) (the reverse
	// index), surviving value edits / erase (P1-2).
	// ============================================================
	{
		const std::string scene = SceneN( 64 );
		Cst::Document doc = Cst::ParseToCst( scene );
		const int i30 = ChunkIndexAt( doc, scene, "name s30" );
		Cst::NodeId id30 = Cst::DocNodeIdAt( doc, i30 );
		int rv = 0;
		Cst::NodeRef n = Cst::DocResolveNodeId( doc, id30, &rv );
		Check( n && n->kind == Cst::NodeKind::Chunk && n->role == "sphere_geometry", "[reverse] DocResolveNodeId returns the chunk for a live id" );
		Check( rv > 0 && rv <= 3 * ( CeilLog2(64) + 1 ), "[reverse] reverse lookup is counted O(log N), not an O(N) scan" );
		Check( Cst::DocResolveNodeId( doc, 999999 ) == nullptr, "[reverse] an unknown id resolves to null" );

		Cst::Document e = Cst::DocReplaceItem( doc, i30, MakeSphere( "s30", "9.9" ) );
		Cst::NodeRef n2 = Cst::DocResolveNodeId( e, id30 );
		Check( n2 && n2 != n, "[reverse] after a value edit, the durable id resolves to the NEW node" );
		Cst::Document del = Cst::DocEraseItem( doc, i30 );
		Check( Cst::DocResolveNodeId( del, id30 ) == nullptr, "[reverse] after erasing its item, the id resolves to null" );
	}

	// ============================================================
	// [reparse-swap] a PARTIAL edit of byte-identical duplicates must invalidate
	// BOTH (no silent id swap among indistinguishable rows) -- P1-3.
	// ============================================================
	{
		const std::string two    = "RISE ASCII SCENE 6\nsphere_geometry\n{\nname s\nradius 0.1\n}\nsphere_geometry\n{\nname s\nradius 0.1\n}\n";
		Cst::Document od = Cst::ParseToCst( two );
		Cst::NodeRef it; size_t st = 0; int vv = 0;
		const int iFirst  = Cst::DocItemAtByteOffset( od, two.find ( "sphere_geometry" ), &it, &st, &vv );
		const int iSecond = Cst::DocItemAtByteOffset( od, two.rfind( "sphere_geometry" ), &it, &st, &vv );
		Cst::NodeId idF = Cst::DocNodeIdAt( od, iFirst );
		Cst::NodeId idS2 = Cst::DocNodeIdAt( od, iSecond );
		Check( idF != 0 && idS2 != 0 && idF != idS2, "[reparse-swap] the two identical chunks have distinct ids" );
		// edit ONLY the first chunk (0.1 -> 0.2); the second is unchanged
		const std::string edited = "RISE ASCII SCENE 6\nsphere_geometry\n{\nname s\nradius 0.2\n}\nsphere_geometry\n{\nname s\nradius 0.1\n}\n";
		std::vector<Cst::NodeId> inv;
		Cst::Document rd = Cst::DocReparse( od, edited, &inv );
		bool invF = std::find( inv.begin(), inv.end(), idF ) != inv.end();
		bool invS = std::find( inv.begin(), inv.end(), idS2 ) != inv.end();
		Check( invF && invS, "[reparse-swap] partial edit of identical duplicates INVALIDATES both (no id swap)" );
		Check( !Cst::DocResolveNodeId( rd, idF ) && !Cst::DocResolveNodeId( rd, idS2 ), "[reparse-swap] neither old id survives in the reverse index" );
	}

	// ============================================================
	// [reparse-cost] DocReparse matching is O(M+N) (linear old-item touches), not
	// O(M*N) -- the committed anti-quadratic gate (P1-5).
	// ============================================================
	{
		for( int N : { 64, 512 } ) {
			const std::string sc = SceneN( N );
			Cst::Document doc = Cst::ParseToCst( sc );
			const std::string needle = "radius 0." + std::to_string( 100 + N/2 );   // unique to the middle chunk
			std::string s2 = sc; size_t pos = s2.find( needle ); s2.replace( pos, needle.size(), "radius 9.999" );
			const unsigned long before = Cst::DebugReparseOldVisits();
			std::vector<Cst::NodeId> inv;
			Cst::Document rd = Cst::DocReparse( doc, s2, &inv );
			const unsigned long touches = Cst::DebugReparseOldVisits() - before;
			const int O = Cst::DocItemCount( doc ), Mm = Cst::DocItemCount( rd );
			char mm[128];
			std::snprintf( mm, sizeof(mm), "[reparse-cost] N=%d: old touches %lu <= 5*(O+M)=%d (linear, not O(M*N)=%d)", N, touches, 5*(O+Mm), O*Mm );
			Check( touches <= (unsigned long)( 5 * (O + Mm) ), mm );
			Check( inv.empty(), "[reparse-cost] the value edit kept every id (lineage)" );
			std::printf( "  reparse N=%4d  oldVisits=%lu  5*(O+M)=%d  O*M=%d\n", N, touches, 5*(O+Mm), O*Mm );
		}
	}

	// ============================================================
	// [param] per-occurrence PARAM identity (P1-1): a parameter has its own
	// durable NodeId -- returned by the within-chunk descent, resolvable via the
	// reverse index, stable across a value edit, dropped on erase.
	// ============================================================
	{
		const std::string scene = "RISE ASCII SCENE 6\nsphere_geometry\n{\nname s\nradius 0.6\n}\n";
		Cst::Document doc = Cst::ParseToCst( scene );
		Cst::NodeId chunkId = 0, radiusId = 0; Cst::NodeRef chunk; int v = 0;
		Cst::NodeRef pr = Cst::DocParamAtByteOffset( doc, scene.find("0.6"), &chunk, &v, &radiusId, &chunkId );
		Check( pr && pr->role == "radius", "[param] within-chunk descent finds the radius Param" );
		Check( chunkId != 0 && radiusId != 0 && radiusId != chunkId, "[param] the param has its own NodeId, distinct from the chunk's" );
		Check( Cst::DocResolveNodeId( doc, radiusId ) == pr, "[param] the param NodeId resolves to the param node (reverse index covers params)" );
		Check( Cst::DocParamId( doc, chunkId, "radius" ) == radiusId, "[param] DocParamId(chunkId, role) resolves to the same id" );
		Cst::NodeId nameId = 0; Cst::DocParamAtByteOffset( doc, scene.find("name s") + 1, &chunk, &v, &nameId, nullptr );
		Check( nameId != 0 && nameId != radiusId, "[param] a different param (name) has a different NodeId" );

		const int ci = ChunkIndexAt( doc, scene, "name s" );
		Cst::Document e = Cst::DocReplaceItem( doc, ci, MakeSphere( "s", "0.66" ) );
		const std::string es = Cst::SerializeCst( e );
		Cst::NodeId radiusId2 = 0; Cst::NodeRef pr2 = Cst::DocParamAtByteOffset( e, es.find("0.66"), &chunk, &v, &radiusId2, nullptr );
		Check( radiusId2 == radiusId, "[param] value edit keeps the radius param NodeId (stable identity)" );
		Check( pr2 && pr2 != pr && Cst::DocResolveNodeId( e, radiusId ) == pr2, "[param] the param id now resolves to the NEW (edited) param node" );

		Cst::Document del = Cst::DocEraseItem( doc, ci );
		Check( Cst::DocResolveNodeId( del, radiusId ) == nullptr && Cst::DocResolveNodeId( del, nameId ) == nullptr, "[param] erasing the chunk drops its param ids" );
	}
	{
		// param ids survive a reparse value edit (carried with their chunk)
		const std::string base   = "RISE ASCII SCENE 6\nsphere_geometry\n{\nname s\nradius 0.6\n}\n";
		const std::string edited = "RISE ASCII SCENE 6\nsphere_geometry\n{\nname s\nradius 0.66\n}\n";
		Cst::Document doc = Cst::ParseToCst( base );
		Cst::NodeId chunkId = 0, radiusId = 0; Cst::NodeRef chunk; int v = 0;
		Cst::DocParamAtByteOffset( doc, base.find("0.6"), &chunk, &v, &radiusId, &chunkId );
		std::vector<Cst::NodeId> inv;
		Cst::Document rd = Cst::DocReparse( doc, edited, &inv );
		Check( Cst::DocParamId( rd, chunkId, "radius" ) == radiusId, "[param-reparse] value edit via reparse keeps the param NodeId" );
		Check( Cst::DocResolveNodeId( rd, radiusId ) != nullptr, "[param-reparse] the carried param id resolves in the new doc" );
	}

	// ============================================================
	// [id-pos] durable NodeId -> document position is O(log N) and COUNTED (P1-A):
	// the order-maintenance label makes DocIndexOfNodeId a label lookup + rank,
	// so the end-to-end edit-by-NodeId path is O(log N), not an O(N) scan.
	// ============================================================
	{
		for( int N : { 8, 64, 512 } ) {
			const std::string scene = SceneN( N );
			Cst::Document doc = Cst::ParseToCst( scene );
			int maxV = 0; bool allOk = true;
			for( int k = 0; k < N; ++k ) {
				const int ci = ChunkIndexAt( doc, scene, "name s" + std::to_string(k) );
				Cst::NodeId id = Cst::DocNodeIdAt( doc, ci );
				int vv = 0; int idx = Cst::DocIndexOfNodeId( doc, id, nullptr, &vv );
				if( idx != ci ) allOk = false;
				if( vv > maxV ) maxV = vv;
			}
			char m[128]; const int bound = 5 * ( CeilLog2(N) + 2 );
			std::snprintf( m, sizeof(m), "[id-pos] N=%d: every durable id resolves to its current index", N );
			Check( allOk, m );
			std::snprintf( m, sizeof(m), "[id-pos] N=%d: id->position worst-case visits %d <= bound %d (O(log N))", N, maxV, bound );
			Check( maxV > 0 && maxV <= bound, m );
			std::snprintf( m, sizeof(m), "[id-pos] N=%d: id->position is sub-linear (not an O(N) scan)", N );
			Check( N < 64 || maxV * 2 < N, m );
			std::printf( "  id-pos N=%4d  maxVisits=%d  bound=%d\n", N, maxV, bound );
		}
	}
	{
		// after an insert shifts positions, the durable id resolves O(log N) to its
		// NEW index, and editing there is end-to-end edit-by-NodeId.
		const std::string scene = SceneN( 16 );
		Cst::Document doc = Cst::ParseToCst( scene );
		const int ci = ChunkIndexAt( doc, scene, "name s9" );
		Cst::NodeId id9 = Cst::DocNodeIdAt( doc, ci );
		Cst::Document ins = Cst::DocInsertItem( doc, 0, MakeSphere( "front", "0.1" ) );
		int vv = 0; int idx = Cst::DocIndexOfNodeId( ins, id9, nullptr, &vv );
		Check( idx == ci + 1, "[id-pos] after an insert, the durable id resolves to the shifted index" );
		Check( vv > 0 && vv <= 5 * ( CeilLog2(20) + 2 ), "[id-pos] the post-shift resolution is still O(log N)" );
		Cst::Document e = Cst::DocReplaceItem( ins, idx, MakeSphere( "s9", "9.9" ) );
		Check( Cst::DocNodeIdAt( e, idx ) == id9, "[id-pos] edit at the resolved index keeps the durable id (end-to-end edit-by-id)" );
	}
	{
		// many same-spot inserts exhaust a label gap -> reflow; id<->position stays correct.
		Cst::Document doc = Cst::ParseToCst( "RISE ASCII SCENE 6\nsphere_geometry\n{\nname a\nradius 0.1\n}\n" );
		std::vector<Cst::NodeId> ids;
		for( int k = 0; k < 80; ++k ) { doc = Cst::DocInsertItem( doc, 1, MakeSphere( "x" + std::to_string(k), "0.5" ) ); ids.push_back( Cst::DocNodeIdAt( doc, 1 ) ); }
		bool ok = true;
		for( Cst::NodeId id : ids ) { int idx = Cst::DocIndexOfNodeId( doc, id, nullptr ); if( idx < 0 || idx >= Cst::DocItemCount(doc) || Cst::DocNodeIdAt( doc, idx ) != id ) ok = false; }
		Check( ok, "[id-pos] after label reflow (80 same-spot inserts), every durable id round-trips id<->position" );
	}

	// ============================================================
	// [param-repeat] repeated same-role params get DISTINCT per-occurrence ids,
	// and erase cleans them all -- no overwrite/orphan (P1-B).
	// ============================================================
	{
		const std::string scene = "RISE ASCII SCENE 6\nsdf_geometry\n{\nname d\npart aaa\npart bbb\n}\n";
		Cst::Document doc = Cst::ParseToCst( scene );
		Cst::NodeId chunkId = 0, p0 = 0, p1 = 0; Cst::NodeRef chunk; int v = 0;
		Cst::DocParamAtByteOffset( doc, scene.find("aaa"), &chunk, &v, &p0, &chunkId );
		Cst::DocParamAtByteOffset( doc, scene.find("bbb"), &chunk, &v, &p1, nullptr );
		Check( p0 != 0 && p1 != 0 && p0 != p1, "[param-repeat] two same-role params resolve to DISTINCT ids" );
		Check( Cst::DocParamId( doc, chunkId, "part", 0 ) == p0 && Cst::DocParamId( doc, chunkId, "part", 1 ) == p1, "[param-repeat] occurrence index distinguishes them" );
		Check( Cst::DocResolveNodeId( doc, p0 ) && Cst::DocResolveNodeId( doc, p1 ) && Cst::DocResolveNodeId( doc, p0 ) != Cst::DocResolveNodeId( doc, p1 ), "[param-repeat] each resolves to its own node" );
		const int ci = ChunkIndexAt( doc, scene, "name d" );
		Cst::Document del = Cst::DocEraseItem( doc, ci );
		Check( !Cst::DocResolveNodeId( del, p0 ) && !Cst::DocResolveNodeId( del, p1 ), "[param-repeat] erase cleans BOTH repeated-param ids (no orphan)" );
	}

	// ============================================================
	// [param-invalidate] reparse reports PARAM ids that died, not just chunk ids (P1-C).
	// ============================================================
	{
		const std::string two = "RISE ASCII SCENE 6\nsphere_geometry\n{\nname a\nradius 0.1\n}\nsphere_geometry\n{\nname b\nradius 0.2\n}\n";
		Cst::Document doc = Cst::ParseToCst( two );
		Cst::NodeId aChunk = 0, aRadius = 0; Cst::NodeRef chunk; int v = 0;
		Cst::DocParamAtByteOffset( doc, two.find("0.1"), &chunk, &v, &aRadius, &aChunk );
		Cst::NodeId aName = Cst::DocParamId( doc, aChunk, "name" );
		Check( aRadius != 0 && aName != 0, "[param-invalidate] chunk a params have ids" );
		const std::string justB = "RISE ASCII SCENE 6\nsphere_geometry\n{\nname b\nradius 0.2\n}\n";
		std::vector<Cst::NodeId> inv;
		Cst::Document rd = Cst::DocReparse( doc, justB, &inv );
		bool invR = std::find( inv.begin(), inv.end(), aRadius ) != inv.end();
		bool invN = std::find( inv.begin(), inv.end(), aName ) != inv.end();
		Check( invR && invN, "[param-invalidate] removing chunk a reports its PARAM ids invalidated (not just the chunk id)" );
		Check( !Cst::DocResolveNodeId( rd, aRadius ), "[param-invalidate] the dead param id no longer resolves" );
	}
	{
		// removing ONE param from a CARRIED chunk invalidates just that param id
		const std::string base = "RISE ASCII SCENE 6\nsphere_geometry\n{\nname s\nradius 0.6\n}\n";
		Cst::Document doc = Cst::ParseToCst( base );
		Cst::NodeId sChunk = 0, sRadius = 0; Cst::NodeRef chunk; int v = 0;
		Cst::DocParamAtByteOffset( doc, base.find("0.6"), &chunk, &v, &sRadius, &sChunk );
		const std::string noRadius = "RISE ASCII SCENE 6\nsphere_geometry\n{\nname s\n}\n";
		std::vector<Cst::NodeId> inv;
		Cst::Document rd = Cst::DocReparse( doc, noRadius, &inv );
		Check( Cst::DocFindByName( rd, "sphere_geometry/s" ) != 0, "[param-invalidate] the chunk itself is carried" );
		Check( std::find( inv.begin(), inv.end(), sRadius ) != inv.end(), "[param-invalidate] the removed param id is invalidated though its chunk survived" );
	}

	// ============================================================
	// [reflow] windowed label reflow: TINY in the common/sparse case, but the
	// fixed-density scheme can grow to Theta(N) under an adversarial DENSE pattern.
	// The gate asserts (a) the sparse case is small, (b) CORRECTNESS holds even
	// when the dense adversary blows the window up -- it does NOT claim window<<N
	// in the dense case (that claim was false; see ReflowWindow's cost note).
	// ============================================================
	{
		char m[160];
		// (a) common/sparse: a shallow same-spot pile -> tiny window
		{
			Cst::Document doc = Cst::ParseToCst( SceneN( 512 ) );
			const int mid = Cst::DocItemCount( doc ) / 2;
			unsigned long maxReflow = 0;
			for( int k = 0; k < 50; ++k ) {
				unsigned long b = Cst::DebugReflowLabelWrites();
				doc = Cst::DocInsertItem( doc, mid, MakeSphere( "z" + std::to_string(k), "0.5" ) );
				maxReflow = std::max( maxReflow, Cst::DebugReflowLabelWrites() - b );
			}
			std::snprintf( m, sizeof(m), "[reflow] sparse case: worst window %lu is small (<< N=%d)", maxReflow, Cst::DocItemCount(doc) );
			Check( maxReflow * 4 < (unsigned long)Cst::DocItemCount(doc), m );
			std::printf( "  reflow sparse: worst window=%lu  N=%d\n", maxReflow, Cst::DocItemCount(doc) );
		}
		// (b) adversarial dense front-pile (repeated inserts at index 1): the window
		// genuinely reaches Theta(N) -- we PROVE the blowup (window > N/4), require it
		// stays <= N, and require id<->position stays correct despite the large window
		// (the honest contract: Theta(N) worst-case is real; the asymptotic fix is
		// Bender level-scaled order-maintenance, deferred).
		{
			Cst::Document doc = Cst::ParseToCst( SceneN( 256 ) );
			unsigned long maxReflow = 0;
			for( int k = 0; k < 2000; ++k ) {
				unsigned long b = Cst::DebugReflowLabelWrites();
				doc = Cst::DocInsertItem( doc, 1, MakeSphere( "f" + std::to_string(k), "0.5" ) );
				maxReflow = std::max( maxReflow, Cst::DebugReflowLabelWrites() - b );
			}
			const int N = Cst::DocItemCount( doc );
			Check( maxReflow <= (unsigned long)N, "[reflow] dense adversary: window stays bounded by N (no over-write)" );
			Check( maxReflow * 4 > (unsigned long)N, "[reflow] dense adversary: window reaches Theta(N) (> N/4) -- the disclosed worst case, witnessed" );
			bool ok = true;   // CORRECTNESS holds even when the window is large
			for( int idx = 0; idx < N; ++idx ) { Cst::NodeId id = Cst::DocNodeIdAt( doc, idx ); if( id && Cst::DocIndexOfNodeId( doc, id, nullptr ) != idx ) ok = false; }
			Check( ok, "[reflow] dense adversary: id<->position round-trips for every item (correct despite large window)" );
			std::printf( "  reflow dense:  worst window=%lu  N=%d  (Theta(N) worst-case, witnessed)\n", maxReflow, N );
		}
	}

	// ============================================================
	// [param-shift] inserting/removing a REPEATED param keeps the others' lineage
	// (P1-C: content-matched, never position-remapped onto an unrelated value).
	// ============================================================
	{
		const std::string scene = "RISE ASCII SCENE 6\nsdf_geometry\n{\nname d\npart aaa\npart bbb\n}\n";
		Cst::Document doc = Cst::ParseToCst( scene );
		Cst::NodeId chunkId = 0, idA = 0, idB = 0; Cst::NodeRef chunk; int v = 0;
		Cst::DocParamAtByteOffset( doc, scene.find("aaa"), &chunk, &v, &idA, &chunkId );
		Cst::DocParamAtByteOffset( doc, scene.find("bbb"), &chunk, &v, &idB, nullptr );
		const int ci = ChunkIndexAt( doc, scene, "name d" );

		// structured whole-chunk replace inserting `part ccc` BEFORE aaa/bbb
		std::vector<Cst::NodeId> inv;
		Cst::Document e = Cst::DocReplaceItem( doc, ci, FirstChunk("sdf_geometry\n{\nname d\npart ccc\npart aaa\npart bbb\n}\n"), nullptr, &inv );
		Check( Cst::DocParamId( e, chunkId, "part", 1 ) == idA && Cst::DocParamId( e, chunkId, "part", 2 ) == idB, "[param-shift] aaa/bbb keep their ids when a part is inserted before them" );
		Cst::NodeId nC = Cst::DocParamId( e, chunkId, "part", 0 );
		Check( nC != 0 && nC != idA && nC != idB, "[param-shift] the inserted part gets a FRESH id (no remap onto a sibling)" );
		Check( inv.empty(), "[param-shift] a pure insert invalidates nothing" );

		// remove aaa -> bbb keeps its id, aaa is invalidated (not shifted onto bbb)
		std::vector<Cst::NodeId> inv2;
		Cst::Document r = Cst::DocReplaceItem( doc, ci, FirstChunk("sdf_geometry\n{\nname d\npart bbb\n}\n"), nullptr, &inv2 );
		Check( Cst::DocParamId( r, chunkId, "part", 0 ) == idB, "[param-shift] removing aaa keeps bbb id (no shift onto bbb)" );
		Check( std::find( inv2.begin(), inv2.end(), idA ) != inv2.end(), "[param-shift] the removed part id is reported invalidated" );

		// same insert-before via REPARSE: lineage preserved by content match
		std::vector<Cst::NodeId> inv3;
		Cst::Document rp = Cst::DocReparse( doc, "RISE ASCII SCENE 6\nsdf_geometry\n{\nname d\npart ccc\npart aaa\npart bbb\n}\n", &inv3 );
		Check( Cst::DocParamId( rp, chunkId, "part", 1 ) == idA && Cst::DocParamId( rp, chunkId, "part", 2 ) == idB, "[param-shift] reparse insert-before keeps aaa/bbb ids by content" );
		Check( inv3.empty(), "[param-shift] reparse insert-before invalidates nothing" );
	}

	// ============================================================
	// [param-ambig] BYTE-IDENTICAL repeated params are genuinely ambiguous: a
	// count-changing edit invalidates them rather than guessing which survived (P1-B).
	// ============================================================
	{
		const std::string scene = "RISE ASCII SCENE 6\nsdf_geometry\n{\nname d\npart aaa\npart aaa\n}\n";
		Cst::Document doc = Cst::ParseToCst( scene );
		const int ci = ChunkIndexAt( doc, scene, "name d" );
		Cst::NodeId chunkId = Cst::DocNodeIdAt( doc, ci );
		Cst::NodeId A = Cst::DocParamId( doc, chunkId, "part", 0 );
		Cst::NodeId B = Cst::DocParamId( doc, chunkId, "part", 1 );
		Check( A != 0 && B != 0 && A != B, "[param-ambig] two identical parts get distinct ids at parse" );
		std::vector<Cst::NodeId> inv;
		Cst::Document e = Cst::DocReplaceItem( doc, ci, FirstChunk("sdf_geometry\n{\nname d\npart bbb\npart aaa\n}\n"), nullptr, &inv );
		Cst::NodeId nBbb = Cst::DocParamId( e, chunkId, "part", 0 );
		Check( nBbb != A && nBbb != B, "[param-ambig] the new bbb does NOT inherit an identical-aaa id (no per-occurrence guess)" );
		Check( std::find(inv.begin(),inv.end(),A) != inv.end() && std::find(inv.begin(),inv.end(),B) != inv.end(), "[param-ambig] both ambiguous identical-aaa ids are invalidated" );
		Check( !Cst::DocResolveNodeId(e,A) && !Cst::DocResolveNodeId(e,B), "[param-ambig] neither old identical id resolves" );
	}

	// ============================================================
	// [param-scale] param matching is O(P), not O(P^2), for large repeated groups (P1-C).
	// ============================================================
	{
		for( int P : { 32, 256 } ) {
			std::string chunkSrc = "sdf_geometry\n{\nname d\n";
			for( int k = 0; k < P; ++k ) chunkSrc += "cp v" + std::to_string(k) + "\n";
			chunkSrc += "}\n";
			const std::string scene = "RISE ASCII SCENE 6\n" + chunkSrc;
			Cst::Document doc = Cst::ParseToCst( scene );
			const int ci = ChunkIndexAt( doc, scene, "name d" );
			std::string editedSrc = "sdf_geometry\n{\nname d\n";
			for( int k = 0; k < P; ++k ) editedSrc += ( k == P/2 ? std::string("cp ZZZ\n") : "cp v" + std::to_string(k) + "\n" );
			editedSrc += "}\n";
			const unsigned long before = Cst::DebugParamMatchVisits();
			std::vector<Cst::NodeId> inv;
			Cst::Document e = Cst::DocReplaceItem( doc, ci, FirstChunk( editedSrc ), nullptr, &inv );
			const unsigned long touches = Cst::DebugParamMatchVisits() - before;
			char m[128];
			std::snprintf( m, sizeof(m), "[param-scale] P=%d: match touches %lu <= 4*P=%d (O(P), not O(P^2)=%d)", P, touches, 4*P, P*P );
			Check( touches <= (unsigned long)(4*P), m );
			Check( inv.empty(), "[param-scale] editing one distinct cp among many keeps its id (unambiguous remainder, no invalidation)" );
			std::printf( "  param-scale P=%4d  touches=%lu  4P=%d  P^2=%d\n", P, touches, 4*P, P*P );
		}
	}

	// ============================================================
	// [trivia] a pure append must NOT spuriously invalidate existing trivia ids:
	// trivia carries greedily by content, not all-or-nothing on a count change.
	// ============================================================
	{
		const std::string old3 = SceneN( 3 );
		Cst::Document doc = Cst::ParseToCst( old3 );
		Cst::NodeId idS1 = Cst::DocFindByName( doc, "sphere_geometry/s1" );
		Check( idS1 != 0, "[trivia] baseline chunk resolves" );
		const std::string new4 = SceneN( 4 );           // old3 + one appended chunk (one more "\n" trivia)
		std::vector<Cst::NodeId> inv;
		Cst::Document rd = Cst::DocReparse( doc, new4, &inv );
		Check( Cst::DocFindByName( rd, "sphere_geometry/s1" ) == idS1, "[trivia] append keeps existing chunk ids" );
		Check( inv.empty(), "[trivia] a pure append invalidates NOTHING (no spurious trivia-id invalidation)" );
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail ? 1 : 0;
}
