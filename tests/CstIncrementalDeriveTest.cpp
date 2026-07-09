//////////////////////////////////////////////////////////////////////
//
//  CstIncrementalDeriveTest.cpp - Slice 1.5 of the agentic redesign.
//
//  Slice 1 proved the front of the pipe (lossless round-trip, apply-layer
//  reuse) but DEFERRED the two mechanisms the design's headline rests on.
//  This slice proves them, still on the simple sphere_geometry chunk:
//
//   (A) MEMOIZED, NODE-GRANULAR DERIVATION -- the EDGE-LESS case of incremental
//       derivation (NOT yet the full O(closure) traced-graph headline; see the
//       scope note):
//       - a derivation KEY per chunk = trivia-INSENSITIVE values
//       - a derivation CACHE; re-derive is node-granular: a comment/whitespace-
//         only edit is a cache HIT and re-derives NOTHING; editing one chunk in
//         a multi-chunk scene re-derives ONLY that chunk (the others are
//         untouched engine instances, witnessed by GetItemSerial). Proven by
//         counting REAL apply-layer calls.
//
//   (B) REPARSE-STABLE IDENTITY (the hard half of D15/D9/D26):
//       - slice 1 preserved NodeId only across a STRUCTURED edit. Here a
//         free-form TEXT edit is RE-PARSED into fresh green nodes, and
//         MatchIdentities re-associates prior NodeIds by CONTENT (unique value)
//         then position -- so a durable reference (agent/UI binding) survives a
//         text edit and a REORDER never silently remaps. The load-bearing
//         safety rule (D15): genuinely AMBIGUOUS rows are INVALIDATED (flagged),
//         never silently remapped to a survivor.
//
//  HONEST SCOPE (after the slice-1.5 review):
//    * This proves the EDGE-LESS case of incremental derivation. The full
//      O(closure) HEADLINE -- a dependency graph TRACED during derivation
//      (D4), a reverse `dependents` index, FORWARD-CONE invalidation (§2.4),
//      the edge lifecycle (D20) -- requires references and is the REFERENCE
//      slice. On a reference-free scene every node's forward cone is {itself},
//      so "re-derive only the edited chunk" holds by construction; this slice
//      does NOT yet prove cross-node invalidation.
//    * The cache is keyed by chunk NAME (a shortcut). The design keys identity
//      by NodeId (D9/D44). Consequence: a RENAME or DELETE is not handled here
//      and would leak a stale derived object -- the reference slice (which adds
//      deletion + the NodeId-keyed cache + edges) owns that.
//    * The APPLY in-place-mutate fast-path (§2.5) needs engine setters sphere
//      lacks, so a value change is a REBUILD. The win proven is "don't re-derive
//      UNCHANGED chunks", which is what makes a big scene cheap.
//    * Params bind ONE value token; multi-token DoubleVec3 values (e.g. a
//      transform triple) parse as separate params -- a later slice.
//
//////////////////////////////////////////////////////////////////////

#include "CstSlicePrototype.h"

using namespace RISE;

//======================================================================
// (A) Incremental derivation: trivia-insensitive key + memoized cache.
//======================================================================

// Derivation key (D15): the chunk's typed values in order, IGNORING all
// trivia (whitespace + comments). Two CSTs that differ only in formatting
// produce the SAME key -> a cache hit -> zero re-derivation.
static std::string DerivationKey( const cst::Green* chunk )
{
	std::string k = chunk->role + "{";
	for( const auto& p : chunk->kids )
		if( p->kind == cst::NK::Param ) {
			std::string val;
			for( const auto& v : p->kids )
				if( v->kind == cst::NK::Tok && v->role == "pvalue" ) val = v->text;
			k += p->role + "=" + val + ";";
		}
	return k + "}";
}

struct DeriveStats { int hits = 0; int misses = 0; int engineCalls = 0; };

// Incremental re-derive: only chunks whose derivation key changed touch the
// engine. (A vanished chunk would also drop its derived object; the scenes
// here keep the same chunk set, so that branch is exercised by the reference
// slice, not here.)
static void IncrementalDerive( const cst::GP& doc, const ChunkDescriptor& desc, Job* job,
                               std::map<std::string,std::string>& cache, DeriveStats& st )
{
	for( const auto& c : doc->kids ) {
		if( c->kind != cst::NK::Chunk || c->role != desc.keyword ) continue;
		std::string name; cst::ParamValueTextIn( c.get(), "name", name );
		std::string key = DerivationKey( c.get() );
		auto it = cache.find( name );
		if( it != cache.end() && it->second == key ) { ++st.hits; continue; }   // memo HIT: skip entirely
		double r; DeriveSphereChunk( c.get(), desc, job, r );                    // MISS: rebuild via real apply layer
		++st.engineCalls; ++st.misses;
		cache[name] = key;
	}
}

//======================================================================
// (B) Reparse-stable identity matching (D15).
//======================================================================

static std::vector<const cst::Green*> ParamsOf( const cst::Green* chunk )
{
	std::vector<const cst::Green*> v;
	for( const auto& p : chunk->kids ) if( p->kind == cst::NK::Param ) v.push_back( p.get() );
	return v;
}
static const cst::Green* ValueTokOf( const cst::Green* param )
{
	for( const auto& v : param->kids ) if( v->kind == cst::NK::Tok && v->role == "pvalue" ) return v.get();
	return nullptr;
}
static std::string ValOf( const cst::Green* param )
{
	const cst::Green* v = ValueTokOf( param );
	return v ? v->text : std::string();
}

struct MatchResult {
	cst::IdMap       newIds;        // new Green* -> NodeId (carried where matched, fresh otherwise)
	std::vector<int> invalidated;   // old NodeIds whose node could NOT be unambiguously matched
};

// Re-associate prior NodeIds onto a freshly-reparsed document. Chunks match by
// (keyword, name), each old chunk used at most once (so duplicate names cannot
// collide ids). Params match per role with a TWO-PHASE rule that never does a
// silent position-only remap (the slice-1.5 review found that bug):
//   (1) carry identity for new rows that match an old row by UNIQUE value --
//       this handles REORDERED distinct rows and unchanged distinct rows
//       without remapping;
//   (2) for the leftover rows, positionally pair them IFF the leftover counts
//       are EQUAL (an in-place value edit, or unchanged identical repeats);
//       otherwise the rows are genuinely AMBIGUOUS (D15) -> invalidate the
//       leftover old ids, give the leftover new rows FRESH ids.
static MatchResult MatchIdentities( const cst::GP& oldDoc, const cst::IdMap& oldIds,
                                    const cst::GP& newDoc, int& freshId )
{
	MatchResult R;
	auto carry = [&]( const cst::Green* oldN, const cst::Green* newN ) {
		auto it = oldIds.find( oldN );
		R.newIds[newN] = (it != oldIds.end()) ? it->second : freshId++;
	};
	auto fresh = [&]( const cst::Green* newN ) { R.newIds[newN] = freshId++; };
	auto invalidate = [&]( const cst::Green* oldParam ) {
		auto it = oldIds.find( oldParam );
		if( it != oldIds.end() ) R.invalidated.push_back( it->second );
		if( const cst::Green* vt = ValueTokOf(oldParam) ) {
			auto it2 = oldIds.find( vt );
			if( it2 != oldIds.end() ) R.invalidated.push_back( it2->second );
		}
	};
	auto carryParam = [&]( const cst::Green* op, const cst::Green* np ) {
		carry( op, np );
		const cst::Green* nvt = ValueTokOf(np);
		const cst::Green* ovt = ValueTokOf(op);
		if( nvt ) { if( ovt ) carry( ovt, nvt ); else fresh( nvt ); }
	};
	auto freshParam = [&]( const cst::Green* np ) {
		fresh( np );
		if( const cst::Green* nvt = ValueTokOf(np) ) fresh( nvt );
	};
	auto matchRole = [&]( std::vector<const cst::Green*>& ol, std::vector<const cst::Green*>& nl ) {
		std::vector<bool> oldUsed( ol.size(), false ), newUsed( nl.size(), false );
		// phase 1: unique-value match (reordered/unchanged distinct rows -- by CONTENT, not position)
		for( size_t ni = 0; ni < nl.size(); ++ni ) {
			std::string nv = ValOf( nl[ni] );
			int idx = -1, cnt = 0;
			for( size_t oi = 0; oi < ol.size(); ++oi ) if( !oldUsed[oi] && ValOf(ol[oi]) == nv ) { idx = (int)oi; ++cnt; }
			if( cnt == 1 ) { oldUsed[idx] = true; newUsed[ni] = true; carryParam( ol[idx], nl[ni] ); }
		}
		// phase 2: positional pairing of the remainder iff equal count, else ambiguous
		std::vector<size_t> remOld, remNew;
		for( size_t oi = 0; oi < ol.size(); ++oi ) if( !oldUsed[oi] ) remOld.push_back( oi );
		for( size_t ni = 0; ni < nl.size(); ++ni ) if( !newUsed[ni] ) remNew.push_back( ni );
		if( remOld.size() == remNew.size() ) {
			for( size_t k = 0; k < remOld.size(); ++k ) carryParam( ol[remOld[k]], nl[remNew[k]] );
		} else {
			for( size_t k = 0; k < remNew.size(); ++k ) freshParam( nl[remNew[k]] );
			for( size_t k = 0; k < remOld.size(); ++k ) invalidate( ol[remOld[k]] );
		}
	};

	std::vector<const cst::Green*> usedOld;
	auto findUnusedOldChunk = [&]( const std::string& kw, const std::string& nm ) -> const cst::Green* {
		for( const auto& c : oldDoc->kids ) {
			if( c->kind != cst::NK::Chunk || c->role != kw ) continue;
			std::string cn; cst::ParamValueTextIn( c.get(), "name", cn );
			if( cn != nm ) continue;
			if( std::find(usedOld.begin(), usedOld.end(), c.get()) != usedOld.end() ) continue;
			return c.get();
		}
		return nullptr;
	};

	for( const auto& ncp : newDoc->kids ) {
		if( ncp->kind != cst::NK::Chunk ) continue;
		const cst::Green* nc = ncp.get();
		std::string nname; cst::ParamValueTextIn( nc, "name", nname );
		const cst::Green* oc = findUnusedOldChunk( nc->role, nname );
		if( !oc ) { for( auto* np : ParamsOf(nc) ) freshParam( np ); continue; }   // brand-new (or renamed) chunk
		usedOld.push_back( oc );
		std::map<std::string,std::vector<const cst::Green*>> oldByRole, newByRole;
		for( auto* p : ParamsOf(oc) ) oldByRole[p->role].push_back( p );
		for( auto* p : ParamsOf(nc) ) newByRole[p->role].push_back( p );
		for( auto& kv : newByRole ) matchRole( oldByRole[kv.first], kv.second );
		for( auto& kv : oldByRole ) if( !newByRole.count(kv.first) ) for( auto* op : kv.second ) invalidate( op );
	}
	// old chunks never matched (removed, or renamed and so unmatchable here) -> invalidate
	for( const auto& ocp : oldDoc->kids ) {
		if( ocp->kind != cst::NK::Chunk ) continue;
		if( std::find(usedOld.begin(), usedOld.end(), ocp.get()) != usedOld.end() ) continue;
		for( auto* op : ParamsOf( ocp.get() ) ) invalidate( op );
	}
	return R;
}

// Helpers to fetch a param node / its id by (chunk-name, role) for assertions.
static const cst::Green* ParamNode( const cst::GP& doc, const std::string& keyword, const std::string& name, const std::string& role, int which = 0 )
{
	const cst::Green* c = cst::FindChunk( doc, keyword, name );
	if( !c ) return nullptr;
	int seen = 0;
	for( const auto& p : c->kids )
		if( p->kind == cst::NK::Param && p->role == role ) { if( seen++ == which ) return p.get(); }
	return nullptr;
}

int main()
{
	using namespace cst;
	std::printf( "CstIncrementalDeriveTest -- slice 1.5 (incremental derive + reparse identity)\n" );

	const ChunkDescriptor desc = SphereDescriptor();

	//==================================================================
	// (A) INCREMENTAL DERIVATION
	//==================================================================
	std::printf( "[A] trivia-insensitive derivation key\n" );
	{
		IdMap a, b; int na = 1, nb = 1;
		GP d1 = ParseStr( "sphere_geometry {\n\tname s\n\tradius 0.6\n}\n", a, na );
		GP d2 = ParseStr( "sphere_geometry {\n\t\tname   s\n\tradius 0.6   # tweaked spacing + comment\n\n}\n", b, nb );
		Check( DerivationKey( FindChunk(d1,"sphere_geometry","s") )
		    == DerivationKey( FindChunk(d2,"sphere_geometry","s") ),
		    "key ignores whitespace/comments (formatting-only edit => same key)" );
		GP d3 = ParseStr( "sphere_geometry {\n\tname s\n\tradius 0.8\n}\n", a, na );
		Check( DerivationKey( FindChunk(d1,"sphere_geometry","s") )
		    != DerivationKey( FindChunk(d3,"sphere_geometry","s") ),
		    "key changes when a typed value changes (0.6 -> 0.8)" );
	}

	std::printf( "[A] memoized re-derive: comment-only edit re-derives NOTHING; one-chunk edit re-derives ONE\n" );
	{
		const std::string srcV1 =
			"sphere_geometry\n{\n\tname\t\t\tA\n\tradius\t\t\t0.165\n}\n\n"
			"# second ball\nsphere_geometry\n{\n\tname\t\t\tB\n\tradius\t\t\t0.25\n}\n";
		// V2 differs ONLY in a comment (formatting), no typed value changes.
		const std::string srcV2 =
			"sphere_geometry\n{\n\tname\t\t\tA\n\tradius\t\t\t0.165\n}\n\n"
			"# 2nd ball (renamed comment)\nsphere_geometry\n{\n\tname\t\t\tB\n\tradius\t\t\t0.25\n}\n";

		Job* job = new Job();
		std::map<std::string,std::string> cache;

		IdMap i1; int n1 = 1; GP v1 = ParseStr( srcV1, i1, n1 );
		DeriveStats s1; IncrementalDerive( v1, desc, job, cache, s1 );
		Check( s1.engineCalls == 2 && s1.misses == 2 && s1.hits == 0, "initial derive: 2 misses, 2 engine calls" );
		// GetItemSerial bumps on every AddItem -> a robust "was this re-created?"
		// witness (immune to the freed-then-realloc-same-address ABA a raw
		// pointer compare would suffer).
		unsigned long long serA0 = job->GetGeometries()->GetItemSerial("A");
		unsigned long long serB0 = job->GetGeometries()->GetItemSerial("B");

		IdMap i2; int n2 = 1; GP v2 = ParseStr( srcV2, i2, n2 );
		DeriveStats s2; IncrementalDerive( v2, desc, job, cache, s2 );
		Check( s2.engineCalls == 0 && s2.hits == 2, "comment-only edit: 0 engine calls (memoized)" );
		Check( job->GetGeometries()->GetItemSerial("A") == serA0
		    && job->GetGeometries()->GetItemSerial("B") == serB0, "comment-only edit: both derived instances UNTOUCHED (serial witness)" );

		// Now a real value edit on chunk A only (path-copy on the CST).
		GP v3 = SetParamValue( v2, "sphere_geometry", "A", "radius", "0.9", i2 );
		DeriveStats s3; IncrementalDerive( v3, desc, job, cache, s3 );
		Check( s3.engineCalls == 1 && s3.misses == 1 && s3.hits == 1, "one-chunk value edit: exactly 1 engine call (node-granular)" );
		Check( job->GetGeometries()->GetItemSerial("B") == serB0, "B's derived instance UNTOUCHED while A re-derived (serial witness)" );
		Check( job->GetGeometries()->GetItemSerial("A") != serA0, "A's derived instance was re-created (serial changed)" );
		double rA = 0; { IGeometry* g = job->GetGeometries()->GetItem("A"); if( g ){ Point3 c; Scalar r=0; g->GenerateBoundingSphere(c,r); rA=r; } }
		Check( rA == 0.9, "A's derived radius updated to 0.9" );
		Check( job->GetGeometries()->getItemCount() == 2, "still exactly two geometries" );

		job->release();
	}

	//==================================================================
	// (B) REPARSE-STABLE IDENTITY
	//==================================================================
	std::printf( "[B] reparse preserves NodeId across a free-form text edit\n" );
	{
		IdMap oldIds; int oldN = 1;
		GP oldDoc = ParseStr( "sphere_geometry {\n\tname s\n\tradius 0.6\n}\n", oldIds, oldN );
		int radIdBefore  = oldIds[ ParamNode(oldDoc,"sphere_geometry","s","radius") ];
		int nameIdBefore = oldIds[ ParamNode(oldDoc,"sphere_geometry","s","name") ];

		// A free-form TEXT edit (not a structured path-copy): change the radius
		// digits, then RE-PARSE the whole file into fresh green nodes.
		GP newDoc = ParseStr( "sphere_geometry {\n\tname s\n\tradius 0.66\n}\n", oldIds /*unused*/, oldN );
		int freshId = 1000;
		MatchResult m = MatchIdentities( oldDoc, oldIds, newDoc, freshId );

		const Green* newRad  = ParamNode( newDoc, "sphere_geometry", "s", "radius" );
		const Green* newName = ParamNode( newDoc, "sphere_geometry", "s", "name" );
		Check( m.newIds.count(newRad)  && m.newIds[newRad]  == radIdBefore,  "radius NodeId preserved across reparse (value edit)" );
		Check( m.newIds.count(newName) && m.newIds[newName] == nameIdBefore, "name NodeId preserved across reparse" );
		Check( m.invalidated.empty(), "value edit invalidates no durable refs" );
	}

	std::printf( "[B] reparse after a comment-only edit preserves all ids\n" );
	{
		IdMap oldIds; int oldN = 1;
		GP oldDoc = ParseStr( "sphere_geometry {\n\tname s\n\tradius 0.6\n}\n", oldIds, oldN );
		int radId = oldIds[ ParamNode(oldDoc,"sphere_geometry","s","radius") ];
		GP newDoc = ParseStr( "sphere_geometry {\n\tname s\n\tradius 0.6   # added comment\n}\n", oldIds, oldN );
		int freshId = 1000;
		MatchResult m = MatchIdentities( oldDoc, oldIds, newDoc, freshId );
		const Green* newRad = ParamNode( newDoc, "sphere_geometry", "s", "radius" );
		Check( m.newIds[newRad] == radId && m.invalidated.empty(), "comment-only reparse preserves ids, invalidates nothing" );
	}

	std::printf( "[B] reparse with a REMOVED distinct row: survivor keeps id, removed is invalidated\n" );
	{
		// Two DISTINCT repeated rows; remove one. The survivor matches by unique
		// value (keeps its id); the removed row's id is invalidated.
		IdMap oldIds; int oldN = 1;
		GP oldDoc = ParseStr( "thing {\n\tname t\n\tval 1\n\tval 2\n}\n", oldIds, oldN );
		int idVal1 = oldIds[ ParamNode(oldDoc,"thing","t","val",0) ];   // "val 1"
		int idVal2 = oldIds[ ParamNode(oldDoc,"thing","t","val",1) ];   // "val 2"
		GP newDoc = ParseStr( "thing {\n\tname t\n\tval 2\n}\n", oldIds, oldN );   // removed "val 1"
		int freshId = 1000;
		MatchResult m = MatchIdentities( oldDoc, oldIds, newDoc, freshId );
		const Green* newVal2 = ParamNode( newDoc, "thing", "t", "val", 0 );
		bool survivorKeptId = m.newIds.count(newVal2) && m.newIds[newVal2] == idVal2;
		bool removedInvalidated = std::find(m.invalidated.begin(), m.invalidated.end(), idVal1) != m.invalidated.end();
		bool survivorNotInvalidated = std::find(m.invalidated.begin(), m.invalidated.end(), idVal2) == m.invalidated.end();
		Check( survivorKeptId,          "distinct survivor 'val 2' keeps its NodeId (unique-value match)" );
		Check( removedInvalidated,      "removed 'val 1' NodeId is invalidated" );
		Check( survivorNotInvalidated,  "survivor's NodeId is NOT invalidated" );
	}

	std::printf( "[B] reparse with AMBIGUOUS identical rows: invalidate, do NOT silently remap (the D15 safety rule)\n" );
	{
		// Two IDENTICAL rows; remove one. A durable ref points at the FIRST.
		// Matching is genuinely ambiguous -> the design INVALIDATES the durable
		// ref rather than binding it to the survivor.
		IdMap oldIds; int oldN = 1;
		GP oldDoc = ParseStr( "thing {\n\tname t\n\tval 7\n\tval 7\n}\n", oldIds, oldN );
		int idFirst  = oldIds[ ParamNode(oldDoc,"thing","t","val",0) ];
		int idSecond = oldIds[ ParamNode(oldDoc,"thing","t","val",1) ];
		GP newDoc = ParseStr( "thing {\n\tname t\n\tval 7\n}\n", oldIds, oldN );
		int freshId = 1000;
		MatchResult m = MatchIdentities( oldDoc, oldIds, newDoc, freshId );
		const Green* survivor = ParamNode( newDoc, "thing", "t", "val", 0 );
		bool durableRefInvalidated =
			std::find(m.invalidated.begin(), m.invalidated.end(), idFirst)  != m.invalidated.end() &&
			std::find(m.invalidated.begin(), m.invalidated.end(), idSecond) != m.invalidated.end();
		bool survivorGotFreshId = m.newIds.count(survivor) && m.newIds[survivor] != idFirst && m.newIds[survivor] != idSecond;
		Check( durableRefInvalidated, "both ambiguous identical-row ids are invalidated (flagged)" );
		Check( survivorGotFreshId,    "survivor gets a FRESH id -- the durable ref is NOT silently remapped to it" );
	}

	std::printf( "[B] reparse with REORDERED distinct rows: ids follow CONTENT, never silently remapped by position\n" );
	{
		// The slice-1.5 review's P0: a position-only matcher would carry the id
		// of "the row that said 1" onto the row now showing "2". The fixed
		// matcher follows content, so ids track values across a reorder.
		IdMap oldIds; int oldN = 1;
		GP oldDoc = ParseStr( "thing {\n\tname t\n\tval 1\n\tval 2\n}\n", oldIds, oldN );
		int idVal1 = oldIds[ ParamNode(oldDoc,"thing","t","val",0) ];
		int idVal2 = oldIds[ ParamNode(oldDoc,"thing","t","val",1) ];
		GP newDoc = ParseStr( "thing {\n\tname t\n\tval 2\n\tval 1\n}\n", oldIds, oldN );   // reordered
		int freshId = 1000;
		MatchResult m = MatchIdentities( oldDoc, oldIds, newDoc, freshId );
		const Green* newFirst  = ParamNode( newDoc, "thing", "t", "val", 0 );  // now shows "2"
		const Green* newSecond = ParamNode( newDoc, "thing", "t", "val", 1 );  // now shows "1"
		Check( m.newIds[newFirst]  == idVal2, "reordered row showing '2' carries the OLD '2' id (by content, not position)" );
		Check( m.newIds[newSecond] == idVal1, "reordered row showing '1' carries the OLD '1' id (by content, not position)" );
		Check( m.invalidated.empty(), "pure reorder invalidates nothing -- and crucially does NOT silently remap" );
	}

	std::printf( "[B] reparse of UNCHANGED identical repeats preserves ids by position\n" );
	{
		IdMap oldIds; int oldN = 1;
		GP oldDoc = ParseStr( "thing {\n\tname t\n\tval 7\n\tval 7\n}\n", oldIds, oldN );
		int idFirst  = oldIds[ ParamNode(oldDoc,"thing","t","val",0) ];
		int idSecond = oldIds[ ParamNode(oldDoc,"thing","t","val",1) ];
		GP newDoc = ParseStr( "thing {\n\tname t\n\tval 7\n\tval 7\n}\n", oldIds, oldN );   // identical
		int freshId = 1000;
		MatchResult m = MatchIdentities( oldDoc, oldIds, newDoc, freshId );
		const Green* n0 = ParamNode( newDoc, "thing", "t", "val", 0 );
		const Green* n1 = ParamNode( newDoc, "thing", "t", "val", 1 );
		Check( m.newIds[n0] == idFirst && m.newIds[n1] == idSecond, "unchanged identical repeats keep ids positionally (no false invalidation)" );
		Check( m.invalidated.empty(), "unchanged identical repeats invalidate nothing" );
	}

	return CheckSummary();
}
