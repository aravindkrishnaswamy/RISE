//////////////////////////////////////////////////////////////////////
//
//  CstDocumentCostTest.cpp - transfer-gate item 3: the persistent Document.
//
//  The real Document (src/Library/Cst) is now a persistent balanced sequence of
//  top-level items, each node caching subtree byte-width + newline + count
//  aggregates. This test proves the load-bearing property the item-2 review
//  required (and that slice 3 lacked): FINDING the edit target is O(log N) and
//  COUNTED -- not handed an already-known index, not an O(N) side scan -- and a
//  path-copy edit is O(log N), both INVARIANT to total document size N.
//
//  Measured at N = 8/64/512 top-level chunks: find-by-byte-offset visits ~log N,
//  DocReplaceItem rebuild ~log N, both << N; aggregates stay exact; round-trip
//  and structural sharing hold across edit/insert/erase.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Cst/Cst.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace RISE;

static int g_pass = 0, g_fail = 0;
static void Check( bool cond, const char* what ) { if( cond ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", what ); } }
static int CeilLog2( int n ) { int b = 0; while( (1 << b) < n ) ++b; return b; }

// An N-sphere scene (each chunk has a unique name + radius).
static std::string SceneN( int n )
{
	std::string s = "RISE ASCII SCENE 6\n";
	for( int i = 0; i < n; ++i )
		s += "sphere_geometry\n{\nname s" + std::to_string(i) + "\nradius 0." + std::to_string(100 + i) + "\n}\n";
	return s;
}
// Extract the first chunk item of a freshly-parsed single-chunk source.
static Cst::NodeRef FirstChunk( const std::string& src )
{
	Cst::Document d = Cst::ParseToCst( src );
	Cst::NodeRef item; size_t start = 0; int v = 0;
	Cst::DocItemAtByteOffset( d, 0, &item, &start, &v );
	return item;
}

struct Row { int N, items, findVisits, editVisits, bound; };

int main()
{
	std::printf( "CstDocumentCostTest -- transfer-gate item 3 (persistent Document, counted lookup/edit)\n" );
	const int Ns[] = { 8, 64, 512 };
	std::vector<Row> rows;

	for( int N : Ns ) {
		const std::string scene = SceneN( N );
		Cst::Document doc = Cst::ParseToCst( scene );
		const int items = Cst::DocItemCount( doc );
		const int bound = 3 * ( CeilLog2(items) + 1 );

		// --- aggregates are exact ---
		char m[112];
		std::snprintf( m, sizeof(m), "N=%d: DocByteWidth == serialized size (aggregate exact)", N );
		Check( Cst::DocByteWidth(doc) == scene.size(), m );
		std::snprintf( m, sizeof(m), "N=%d: round-trip byte-identical through the persistent Document", N );
		Check( Cst::SerializeCst(doc) == scene, m );

		// --- find-by-byte-offset is O(log N) and COUNTED ---
		const std::string needle = "name s" + std::to_string( N/2 );
		size_t off = scene.find( needle );
		Cst::NodeRef item; size_t start = 0; int findVisits = 0;
		int idx = Cst::DocItemAtByteOffset( doc, off, &item, &start, &findVisits );
		std::snprintf( m, sizeof(m), "N=%d: byte offset resolves to the middle sphere chunk", N );
		Check( idx >= 0 && item && item->kind == Cst::NodeKind::Chunk && item->role == "sphere_geometry", m );
		std::snprintf( m, sizeof(m), "N=%d: find-by-offset visits %d <= ~3*log2(items)=%d (NOT O(N)=%d)", N, findVisits, bound, items );
		Check( findVisits <= bound, m );

		// --- path-copy edit (DocReplaceItem) is O(log N) and COUNTED ---
		Cst::NodeRef newChunk = FirstChunk( "sphere_geometry\n{\nname s" + std::to_string(N/2) + "\nradius 9.99\n}" );
		int editVisits = 0;
		Cst::Document edited = Cst::DocReplaceItem( doc, idx, newChunk, &editVisits );
		std::snprintf( m, sizeof(m), "N=%d: edit path-copy visits %d <= ~3*log2(items)=%d (NOT O(N))", N, editVisits, bound );
		Check( editVisits <= bound, m );

		// edit correctness: only the target changed; aggregates stay exact
		std::string out = Cst::SerializeCst( edited );
		std::snprintf( m, sizeof(m), "N=%d: edited doc has the new radius and not the old", N );
		Check( out.find("radius 9.99") != std::string::npos
		    && out.find("radius 0." + std::to_string(100 + N/2)) == std::string::npos, m );
		// FULL-SEQUENCE correctness: the edited doc must be byte-identical to the
		// expected sequence. (Pins ORDER + position -- catches a transposition, an
		// off-by-one, or a wrong-chunk replace that the content-only checks above
		// and the order-insensitive DumpJob gate would BOTH miss.)
		{
			std::string oldRad = "radius 0." + std::to_string(100 + N/2);
			std::string expected = scene;
			size_t rp = expected.find( oldRad );
			expected.replace( rp, oldRad.size(), "radius 9.99" );
			std::snprintf( m, sizeof(m), "N=%d: edited doc byte-identical to the expected sequence (order pinned)", N );
			Check( out == expected, m );
		}
		std::snprintf( m, sizeof(m), "N=%d: DocByteWidth exact after edit", N );
		Check( Cst::DocByteWidth(edited) == out.size(), m );
		std::snprintf( m, sizeof(m), "N=%d: original Document unchanged (persistence)", N );
		Check( Cst::SerializeCst(doc) == scene, m );

		// structural sharing: an untouched earlier chunk is the SAME NodeRef
		size_t off0 = scene.find( "name s0" );
		Cst::NodeRef itA, itB; size_t s0a, s0b; int va, vb;
		Cst::DocItemAtByteOffset( doc,    off0, &itA, &s0a, &va );
		Cst::DocItemAtByteOffset( edited, off0, &itB, &s0b, &vb );
		std::snprintf( m, sizeof(m), "N=%d: untouched chunk s0 is pointer-SHARED across the edit", N );
		Check( itA && itA.get() == itB.get(), m );

		rows.push_back( { N, items, findVisits, editVisits, bound } );
	}

	// --- the headline: cost is invariant to N (grows ~log N, not ~N) ---
	std::printf( "  %6s | %7s | %10s %11s | %8s\n", "N", "items", "find(log)", "edit(log)", "O(N)" );
	for( const Row& r : rows )
		std::printf( "  %6d | %7d | %10d %11d | %8d\n", r.N, r.items, r.findVisits, r.editVisits, r.items );
	{
		const Row& big = rows.back();   // N=512
		Check( big.findVisits < big.items / 8 && big.editVisits < big.items / 8,
		       "N=512: find + edit cost are each << N/8 (sub-linear, the unchanged bulk untouched)" );
		Check( rows.front().findVisits > 0 && big.findVisits <= rows.front().findVisits * 3,
		       "find cost grows only ~log N from N=8 to N=512 (not 64x)" );
	}

	// --- DocItemAtByteOffset boundary coverage ---
	std::printf( "[bounds] byte-offset lookup at the edges (0 / one-past-end / last byte / trivia)\n" );
	{
		const std::string scene = SceneN( 8 );
		Cst::Document doc = Cst::ParseToCst( scene );
		size_t W = Cst::DocByteWidth( doc );
		Cst::NodeRef it; size_t st; int v;
		Check( Cst::DocItemAtByteOffset(doc, 0,    &it, &st, &v) == 0,  "offset 0 -> first item (index 0)" );
		Check( Cst::DocItemAtByteOffset(doc, W,    &it, &st, &v) == -1, "offset == byte width -> -1 (clean one-past-end)" );
		Check( Cst::DocItemAtByteOffset(doc, W+99, &it, &st, &v) == -1, "offset past end -> -1" );
		Check( Cst::DocItemAtByteOffset(doc, W-1,  &it, &st, &v) >= 0,  "last byte resolves to a valid item" );
		// an offset in the inter-chunk trivia (the '\n' after the first '}')
		size_t triviaOff = scene.find( "}\n" ) + 1;
		int ti = Cst::DocItemAtByteOffset( doc, triviaOff, &it, &st, &v );
		Check( ti >= 0 && it && it->kind == Cst::NodeKind::Trivia, "an inter-chunk trivia offset resolves to a Trivia item (not a chunk)" );
	}

	// --- insert / erase: functional + aggregate-correct + round-trip ---
	std::printf( "[struct] insert / erase keep the sequence + aggregates correct\n" );
	{
		Cst::Document doc = Cst::ParseToCst( SceneN(8) );
		int n0 = Cst::DocItemCount( doc );
		Cst::Document ins = Cst::DocInsertItem( doc, 1, FirstChunk("sphere_geometry\n{\nname extra\nradius 7\n}") );
		Check( Cst::DocItemCount(ins) == n0 + 1, "insert adds one item" );
		Check( Cst::DocByteWidth(ins) == Cst::SerializeCst(ins).size(), "aggregate exact after insert" );
		Check( Cst::SerializeCst(ins).find("name extra") != std::string::npos, "inserted chunk present in round-trip" );
		Cst::Document era = Cst::DocEraseItem( ins, 1 );
		Check( Cst::DocItemCount(era) == n0, "erase removes one item" );
		Check( Cst::SerializeCst(era) == Cst::SerializeCst(doc), "erase of the inserted item restores the original bytes" );
	}

	// --- WBT: insert/erase are O(log N) and keep the tree BALANCED (D16) ---
	std::printf( "[wbt] insert/erase are O(log N) on a weight-balanced tree, not flatten+rebuild\n" );
	for( int N : Ns ) {
		// insert N items one-by-one into an empty doc (appending); height stays log N
		Cst::Document doc = Cst::ParseToCst( "" );
		int maxIns = 0;
		for( int i = 0; i < N; ++i ) {
			Cst::NodeRef it = FirstChunk( "sphere_geometry\n{\nname x" + std::to_string(i) + "\nradius 1\n}" );
			int v = 0; doc = Cst::DocInsertItem( doc, Cst::DocItemCount(doc), it, &v );
			if( v > maxIns ) maxIns = v;
		}
		int items = Cst::DocItemCount( doc );
		int bound = 3 * ( CeilLog2(items) + 1 );
		char m[128];
		std::snprintf( m, sizeof(m), "N=%d: %d inserts -> items=%d, max insert visits=%d <= ~3*log2=%d (O(log N))", N, N, items, maxIns, bound );
		Check( items == N && maxIns <= bound, m );
		// a find on the result is O(log N) -> proves the tree is BALANCED (a degenerate list would be ~N)
		Cst::NodeRef fi; size_t fs; int fv = 0;
		Cst::DocItemAtByteOffset( doc, Cst::DocByteWidth(doc) / 2, &fi, &fs, &fv );
		std::snprintf( m, sizeof(m), "N=%d: post-insert find visits=%d <= ~3*log2=%d (tree balanced after inserts)", N, fv, bound );
		Check( fv <= bound, m );
		// erase to empty; max erase visits O(log N)
		int maxEra = 0;
		while( Cst::DocItemCount(doc) > 0 ) { int v = 0; doc = Cst::DocEraseItem( doc, 0, &v ); if( v > maxEra ) maxEra = v; }
		std::snprintf( m, sizeof(m), "N=%d: erase-to-empty max visits=%d <= ~3*log2=%d (O(log N))", N, maxEra, bound );
		Check( maxEra <= bound, m );
		std::printf( "      N=%4d: max-insert=%d  post-insert-find=%d  max-erase=%d  (vs N=%d, bound=%d)\n", N, maxIns, fv, maxEra, N, bound );
	}

	// --- null edits are refused (non-null contract; no crash) ---
	std::printf( "[null] null replace/insert are refused -- doc unchanged, no crash\n" );
	{
		Cst::Document doc = Cst::ParseToCst( SceneN(4) );
		std::string before = Cst::SerializeCst( doc );
		int n = Cst::DocItemCount( doc );
		Cst::NodeRef nullRef;   // default-constructed == null
		Cst::Document r = Cst::DocReplaceItem( doc, 1, nullRef );
		Check( Cst::SerializeCst(r) == before && Cst::DocItemCount(r) == n, "null DocReplaceItem -> unchanged" );
		Cst::Document i = Cst::DocInsertItem( doc, 1, nullRef );
		Check( Cst::SerializeCst(i) == before && Cst::DocItemCount(i) == n, "null DocInsertItem -> unchanged" );
	}

	// --- DURABLE work instrumentation: an edit walks EXACTLY ONE item ---
	std::printf( "[walks] an edit re-walks exactly ONE item, invariant to N and to sibling size\n" );
	for( int N : { 8, 512 } ) {
		// every OTHER item is huge; a spine re-scan would show up as extra walks
		const std::string pad( 2000, 'q' );
		std::string scene = "RISE ASCII SCENE 6\n";
		for( int i = 0; i < N; ++i ) scene += "sphere_geometry\n{\nname s" + std::to_string(i) + pad + "\nradius 0.5\n}\n";
		Cst::Document doc = Cst::ParseToCst( scene );
		Cst::NodeRef it; size_t st; int fv;
		int idx = Cst::DocItemAtByteOffset( doc, scene.find("name s" + std::to_string(N/2) + pad), &it, &st, &fv );
		char m[128];
		// REPLACE: exactly 1 walk (build the new item FIRST so its parse-walk isn't counted)
		Cst::NodeRef small = FirstChunk( "sphere_geometry\n{\nname tiny\nradius 9\n}" );
		unsigned long w0 = Cst::DebugItemStatWalks();
		Cst::DocReplaceItem( doc, idx, small );
		std::snprintf( m, sizeof(m), "N=%d: DocReplaceItem walks exactly 1 item (delta=%lu) despite huge siblings", N, Cst::DebugItemStatWalks() - w0 );
		Check( Cst::DebugItemStatWalks() - w0 == 1, m );
		// INSERT: exactly 1 walk
		Cst::NodeRef small2 = FirstChunk( "sphere_geometry\n{\nname tiny2\nradius 9\n}" );
		unsigned long w1 = Cst::DebugItemStatWalks();
		Cst::DocInsertItem( doc, idx, small2 );
		std::snprintf( m, sizeof(m), "N=%d: DocInsertItem walks exactly 1 item (delta=%lu)", N, Cst::DebugItemStatWalks() - w1 );
		Check( Cst::DebugItemStatWalks() - w1 == 1, m );
		// LOOKUP + O(1) accessors: ZERO walks
		unsigned long w2 = Cst::DebugItemStatWalks();
		Cst::DocItemAtByteOffset( doc, st, &it, &st, &fv );
		(void)Cst::DocByteWidth(doc); (void)Cst::DocNewlineCount(doc); (void)Cst::DocItemCount(doc);
		Check( Cst::DebugItemStatWalks() - w2 == 0, "find-by-offset + byte/newline/count accessors walk 0 items" );
	}

	// --- newline aggregate is GATED (exact across parse/replace/insert/erase, LF+CRLF) ---
	std::printf( "[newlines] DocNewlineCount is exact (gated), incl. CRLF, across edits\n" );
	{
		auto realNL = []( const std::string& s ){ int n = 0; for( char c : s ) if( c == '\n' ) ++n; return n; };
		const std::string scenes2[] = {
			SceneN( 6 ),
			std::string( "RISE ASCII SCENE 6\r\nsphere_geometry\r\n{\r\nname s\r\nradius 0.6\r\n}\r\nsphere_geometry\r\n{\r\nname t\r\nradius 0.7\r\n}\r\n" ),
		};
		for( const std::string& scene : scenes2 ) {
			Cst::Document doc = Cst::ParseToCst( scene );
			Check( Cst::DocNewlineCount(doc) == realNL(Cst::SerializeCst(doc)), "DocNewlineCount == actual '\\n' count (parse; LF + CRLF)" );
			Cst::NodeRef it; size_t st; int v;
			int idx = Cst::DocItemAtByteOffset( doc, scene.find("name s"), &it, &st, &v );
			Cst::Document r = Cst::DocReplaceItem( doc, idx, FirstChunk("sphere_geometry\n{\nname z\nradius 1\n}") );
			Check( Cst::DocNewlineCount(r) == realNL(Cst::SerializeCst(r)), "DocNewlineCount exact after replace" );
			Cst::Document i = Cst::DocInsertItem( doc, 1, FirstChunk("sphere_geometry\n{\nname ins\nradius 1\n}") );
			Check( Cst::DocNewlineCount(i) == realNL(Cst::SerializeCst(i)), "DocNewlineCount exact after insert" );
			Cst::Document e = Cst::DocEraseItem( doc, 0 );
			Check( Cst::DocNewlineCount(e) == realNL(Cst::SerializeCst(e)), "DocNewlineCount exact after erase" );
		}
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
