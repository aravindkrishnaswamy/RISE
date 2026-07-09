//////////////////////////////////////////////////////////////////////
//
//  CstFirstSliceTest.cpp - First slice of the agentic redesign.
//
//  Proves the `sphere_geometry` vertical end-to-end (docs/agentic-redesign,
//  §D10/§D18, and 10-scene-language-and-cst.md §6.2): the smallest non-trivial
//  chunk carried through
//
//      bytes  --ParseToCst-->  CST  --SerializeCst-->  bytes        (Gate G1)
//      CST    --derive-------> Job::AddSphereGeometry (REAL apply layer)
//      edit   --path-copy----> new CST (structural sharing)
//      change --re-derive----> drop+add the changed chunk (apply-layer reuse)
//
//  The CST + codec + derive helpers live in CstSlicePrototype.h (shared with
//  the incremental-derive and reference slices). This file is the gate suite.
//
//  HONEST SCOPE (after adversarial review):
//    * G1 lossless round-trip: PROVEN on MULTI-CHUNK documents in the real
//      corpus shape (incl. the live csg.RISEscene sphere chunks) and the
//      tar-pit cases. NOT proven on full arbitrary scenes (header line,
//      FOR/DEFINE/expr/`>` commands) -- those need the general grammar.
//    * Apply-layer reuse: PROVEN (real Job::AddSphereGeometry; engine read-back).
//    * Re-derive here is the full drop+add REBUILD path. TRUE incremental
//      derivation (node-granular diff, memoization, traced invalidation, the
//      APPLY fast-path) is the NEXT slice (CstIncrementalDeriveTest).
//    * Structural sharing + NodeId lineage proven across a STRUCTURED edit;
//      identity across a free-form RE-PARSE is the next slice.
//
//////////////////////////////////////////////////////////////////////

#include "CstSlicePrototype.h"

using namespace RISE;

int main()
{
	using namespace cst;
	std::printf( "CstFirstSliceTest -- sphere_geometry vertical (D10/D18 first slice)\n" );

	const ChunkDescriptor desc = SphereDescriptor();

	//----------------------------------------------------------------------
	// GATE G1 -- lossless round-trip (the non-negotiable correctness gate).
	// Fixtures cover the F1 §4 tar-pit cases AND the real corpus shape:
	// MULTI-CHUNK, keyword and `{` on their own lines, tab-aligned columns,
	// comments between chunks, blank lines, CRLF, no-trailing-newline.
	//----------------------------------------------------------------------
	std::printf( "[G1] lossless parse->serialize identity\n" );
	const std::vector<std::string> fixtures = {
		"sphere_geometry {\n\tname s\n\tradius 0.6  # main ball\n\n}\n",
		"# a scene\nsphere_geometry {\n\tname ball\n\tradius 2\n}",
		"sphere_geometry {\r\n\tname   s\r\n\tradius    0.6\r\n}\r\n",
		"sphere_geometry {\n\tname s \n\tradius 1.0\t\n}\n",
		// MULTI-CHUNK in the real corpus shape (mirrors csg.RISEscene byte shape)
		"sphere_geometry\n{\n\tname\t\t\tspheregeomA\n\tradius\t\t\t0.165\n}\n\n"
		"# second ball\nsphere_geometry\n{\n\tname\t\t\tspheregeomB\n\tradius\t\t\t0.25\n}\n",
		// nested braces inside a (non-sphere) chunk must NOT truncate
		"some_chunk {\n\touter {\n\t\tinner 1\n\t}\n\tname z\n}\nsphere_geometry {\n\tname s\n\tradius 3\n}\n",
	};
	for( size_t fi = 0; fi < fixtures.size(); ++fi ) {
		IdMap fids; int fn = 1;
		GP fdoc = ParseStr( fixtures[fi], fids, fn );
		std::string out = SerializeCst( fdoc );
		bool identical = (out == fixtures[fi]);
		char msg[80]; std::snprintf( msg, sizeof(msg), "fixture %zu round-trips byte-identical", fi );
		Check( identical, msg );
		if( !identical ) std::printf( "    in (%zuB) =[%s]\n    out(%zuB)=[%s]\n",
		                              fixtures[fi].size(), fixtures[fi].c_str(), out.size(), out.c_str() );
	}

	// G1 on a real on-disk scene file's sphere chunks, if present (golden corpus).
	{
		const char* path = "scenes/Tests/Geometry/csg.RISEscene";
		FILE* f = std::fopen( path, "rb" );
		if( f ) {
			std::string bytes; char buf[4096]; size_t r;
			while( (r = std::fread(buf, 1, sizeof(buf), f)) > 0 ) bytes.append( buf, r );
			std::fclose( f );
			std::string spheres;
			size_t p = 0;
			while( (p = bytes.find("sphere_geometry", p)) != std::string::npos ) {
				size_t open = bytes.find('{', p);
				size_t close = (open == std::string::npos) ? std::string::npos : bytes.find('}', open);
				if( close == std::string::npos ) break;
				spheres += bytes.substr( p, close - p + 1 );
				spheres += "\n";
				p = close + 1;
			}
			if( !spheres.empty() ) {
				IdMap gids; int gn = 1;
				GP gdoc = ParseStr( spheres, gids, gn );
				Check( SerializeCst(gdoc) == spheres, "real csg.RISEscene sphere chunks round-trip byte-identical" );
			} else {
				std::printf( "  (note: no sphere_geometry chunks found in %s; skipping golden round-trip)\n", path );
			}
		} else {
			std::printf( "  (note: %s not found from cwd; skipping golden round-trip -- run from repo root)\n", path );
		}
	}

	//----------------------------------------------------------------------
	// Canonical fixture for the remaining checks.
	//----------------------------------------------------------------------
	const std::string src = fixtures[0];
	IdMap ids; int nid = 1;
	GP doc = ParseStr( src, ids, nid );

	//----------------------------------------------------------------------
	// Descriptor-driven binding + name-path addressing.
	//----------------------------------------------------------------------
	std::printf( "[bind] descriptor-driven structure + name-path addressing\n" );
	const Green* chunk = FindChunk( doc, "sphere_geometry", "s" );
	Check( chunk != nullptr, "Chunk node addressed by geometry/s (keyword + name)" );
	std::string nameVal, radVal;
	Check( ParamValueTextIn(chunk, "name",   nameVal) && nameVal == "s",   "geometry/s.name == s via name-path" );
	Check( ParamValueTextIn(chunk, "radius", radVal)  && radVal  == "0.6", "geometry/s.radius == 0.6 via name-path" );

	//----------------------------------------------------------------------
	// Red cursor (D16): the computed offset must LAND on the value's bytes.
	//----------------------------------------------------------------------
	std::printf( "[cursor] relative-width red cursor computes absolute offsets\n" );
	const Green* radTok = FindValueTok( chunk, "radius" );
	size_t computed = 0; bool found = AbsOffsetOf( doc, radTok, computed );
	Check( found && radTok && computed + radTok->text.size() <= src.size()
	             && src.compare( computed, radTok->text.size(), radTok->text ) == 0,
	       "red cursor offset lands exactly on the radius value bytes" );

	//----------------------------------------------------------------------
	// Edit = path-copy with structural sharing (D11) + NodeId lineage (D26).
	//----------------------------------------------------------------------
	std::printf( "[edit] path-copy: structural sharing + preserved NodeId\n" );
	int radNodeIdBefore = ids.count(radTok) ? ids[radTok] : -1;
	const Green* nameParamBefore = nullptr;
	const Green* commentTriviaBefore = nullptr;
	for( const auto& k : chunk->kids ) {
		if( k->kind == NK::Param && k->role == "name" ) nameParamBefore = k.get();
		if( k->kind == NK::Trivia && k->text.find("# main ball") != std::string::npos ) commentTriviaBefore = k.get();
	}
	GP doc2 = SetParamValue( doc, "sphere_geometry", "s", "radius", "0.8", ids );
	Check( doc2.get() != doc.get(),  "edit produced a NEW document root (immutability)" );
	Check( SerializeCst(doc) == src, "original document still serializes to the original bytes (persistence)" );

	const Green* chunk2 = FindChunk( doc2, "sphere_geometry", "s" );
	const Green* nameParamAfter = nullptr;
	const Green* commentTriviaAfter = nullptr;
	for( const auto& k : chunk2->kids ) {
		if( k->kind == NK::Param && k->role == "name" ) nameParamAfter = k.get();
		if( k->kind == NK::Trivia && k->text.find("# main ball") != std::string::npos ) commentTriviaAfter = k.get();
	}
	Check( nameParamBefore && nameParamBefore == nameParamAfter,             "untouched 'name' Param is pointer-SHARED (structural sharing)" );
	Check( commentTriviaBefore && commentTriviaBefore == commentTriviaAfter, "untouched comment trivia is pointer-SHARED" );

	const Green* radTok2 = FindValueTok( chunk2, "radius" );
	Check( radTok2 && ids.count(radTok2) && ids[radTok2] == radNodeIdBefore, "radius value keeps its NodeId across the edit (lineage)" );

	//----------------------------------------------------------------------
	// Cross-chunk structural sharing on a MULTI-CHUNK document.
	//----------------------------------------------------------------------
	std::printf( "[edit] multi-chunk: editing one chunk shares the other chunk's subtree\n" );
	{
		IdMap mids; int mn = 1;
		GP mdoc = ParseStr( fixtures[4], mids, mn );
		const Green* chunkB_before = FindChunk( mdoc, "sphere_geometry", "spheregeomB" );
		GP mdoc2 = SetParamValue( mdoc, "sphere_geometry", "spheregeomA", "radius", "0.999", mids );
		const Green* chunkB_after = FindChunk( mdoc2, "sphere_geometry", "spheregeomB" );
		Check( chunkB_before && chunkB_before == chunkB_after, "untouched sibling chunk is pointer-SHARED after editing its neighbor" );
		Check( SerializeCst(mdoc2).find("0.999") != std::string::npos
		    && SerializeCst(mdoc2).find("0.25")  != std::string::npos, "edit changed chunk A's radius, preserved chunk B's" );
	}

	//----------------------------------------------------------------------
	// Round-trip after edit: only the value changed; comment + tabs preserved.
	//----------------------------------------------------------------------
	std::printf( "[edit] round-trip preserves comment + formatting, changes only the value\n" );
	const std::string expectAfter = "sphere_geometry {\n\tname s\n\tradius 0.8  # main ball\n\n}\n";
	Check( SerializeCst(doc2) == expectAfter, "edited document serializes with only 0.6->0.8 changed" );

	//----------------------------------------------------------------------
	// Derive via the REAL apply layer + re-derive of the changed chunk.
	//----------------------------------------------------------------------
	std::printf( "[derive] apply-layer reuse (Job::AddSphereGeometry); re-derive = drop+add (full, not yet incremental)\n" );
	Job* job = new Job();   // ctor calls InitializeContainers() -> geometry manager ready
	if( !job ) { Check( false, "Job created" ); return CheckSummary(); }

	double derived = 0;
	Check( DeriveSphere(doc,  "s", desc, job, derived) && derived == 0.6, "derive v1 -> engine sphere radius 0.6 (read back)" );
	Check( DeriveSphere(doc2, "s", desc, job, derived) && derived == 0.8, "re-derive changed chunk -> engine sphere radius 0.8 (read back)" );
	Check( job->GetGeometries()->getItemCount() == 1, "exactly one geometry after re-derive (replaced, not duplicated)" );

	{
		IdMap mids; int mn = 1;
		GP mdoc = ParseStr( fixtures[4], mids, mn );
		Job* job2 = new Job();
		Check( DeriveAllSpheres(mdoc, desc, job2) == 2,     "multi-chunk: both sphere chunks derive" );
		Check( job2->GetGeometries()->getItemCount() == 2,  "multi-chunk: two distinct geometries in the scene" );
		double rA = 0, rB = 0;
		IGeometry* gA = job2->GetGeometries()->GetItem("spheregeomA");
		IGeometry* gB = job2->GetGeometries()->GetItem("spheregeomB");
		if( gA ) { Point3 c; Scalar r=0; gA->GenerateBoundingSphere(c,r); rA = r; }
		if( gB ) { Point3 c; Scalar r=0; gB->GenerateBoundingSphere(c,r); rB = r; }
		Check( rA == 0.165 && rB == 0.25, "multi-chunk: each derived radius matches its chunk" );
		job2->release();
	}

	//----------------------------------------------------------------------
	// Latency mechanism (toy scale). The G2 gate proper (<50ms incremental
	// DERIVE on a 155-mesh Sponza-scale scene) needs a large scene AND true
	// incremental derivation -- both later slices.
	//----------------------------------------------------------------------
	std::printf( "[perf] CST op latency (toy scale; the <50ms-Sponza G2 gate + true incremental derive are later slices)\n" );
	double tParse = TimeMicros( [&]{ IdMap z; int n=1; volatile auto d = ParseStr(src, z, n); (void)d; } );
	double tSer   = TimeMicros( [&]{ volatile auto s = SerializeCst(doc); (void)s; } );
	double tEdit  = TimeMicros( [&]{ IdMap z=ids; volatile auto d = SetParamValue(doc,"sphere_geometry","s","radius","0.7",z); (void)d; } );
	std::printf( "      parse=%.1fus  serialize=%.1fus  path-copy-edit=%.2fus\n", tParse, tSer, tEdit );
	Check( tEdit < tParse, "incremental path-copy edit cheaper than a full re-parse" );

	job->release();
	return CheckSummary();
}
