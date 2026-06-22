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
//     distinct chunks keeps ids regardless of position, a RENAME invalidates the
//     old id and assigns a fresh one (D9/D15: invalidate-don't-remap).
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

		// rename via reparse: old id INVALIDATED, new name gets a FRESH id
		const std::string renamed = "RISE ASCII SCENE 6\nsphere_geometry\n{\nname s2\nradius 0.6\n}\n";
		std::vector<Cst::NodeId> inv2;
		Cst::Document neu2 = Cst::DocReparse( old, renamed, &inv2 );
		Check( Cst::DocFindByName( neu2, "sphere_geometry/s" ) == 0, "[reparse] rename: the old name no longer resolves" );
		Cst::NodeId newId = Cst::DocFindByName( neu2, "sphere_geometry/s2" );
		Check( newId != 0 && newId != idS, "[reparse] rename: the new name gets a FRESH id (never the old, never a position remap)" );
		Check( std::find( inv2.begin(), inv2.end(), idS ) != inv2.end(), "[reparse] rename: the old id is reported invalidated" );

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

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail ? 1 : 0;
}
