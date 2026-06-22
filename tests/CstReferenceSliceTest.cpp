//////////////////////////////////////////////////////////////////////
//
//  CstReferenceSliceTest.cpp - Reference slice (slice 2) of the agentic
//  redesign (docs/agentic-redesign, D9/D14/D18, §2.4/§2.6).
//
//  Slices 1 + 1.5 proved the EDGE-LESS pipeline. References introduce the
//  thing the O(closure) headline actually rests on -- a dependency GRAPH --
//  so this slice proves, on a real reference chain reusing the real apply
//  layer:
//
//    uniformcolor_painter "p"   <--reflectance--   lambertian_material "mat"
//        (producer)                                    (consumer)
//
//   1. TRACED EDGE: deriving the material reads its `reflectance` reference and
//      records mat -> p (a reverse `dependents` index, D4/§2.4).
//   2. FORWARD-CONE INVALIDATION: editing the producer re-derives it AND its
//      transitive consumers; editing an UNRELATED chunk re-derives only it
//      (the consumer is untouched). Proven by counting real apply-layer calls.
//   3. FRESHNESS: the consumer re-derives when its producer changes EVEN THOUGH
//      the consumer's own CST is byte-identical (it's pulled in via the cone).
//   4. NodeId-KEYED CACHE: the memo cache keys on the chunk's NodeId, not its
//      name. This is what lets a RENAME be handled without a leak: same NodeId
//      + a changed name => remove the old engine object, add the new. A
//      name-keyed cache would orphan the old object (the slice-1.5 rename leak).
//   5. RENAME INTEGRITY (D14): renaming the producer rewrites every referrer's
//      reference (from the traced set), so the reference survives.
//   6. DANGLING REFERENCE (D9/§2.9): a reference to a missing target is
//      RESOLVED and FLAGGED, never silently derived -- including BECOME-dangling
//      (delete a producer without rewriting the referrer): the producer's engine
//      object is removed and the orphaned referrer is flagged (D20 edge lifecycle).
//
//  HONEST SCOPE (after the slice-2 review):
//    * The forward-cone is a SINGLE HOP and "topological order" is two hardcoded
//      loops (painters then materials). That is correct for this 2-level scene
//      but is NOT the general mechanism: a TRANSITIVE worklist BFS + a real
//      topological sort (depth >= 3, e.g. painter -> checker_painter -> material)
//      is the next slice.
//    * The O(closure) COST headline is NOT measured here. This engine rebuilds
//      the dependency graph and re-keys every chunk each pass (O(N)/pass). Proving
//      re-derive work is invariant to total scene size (a persistent graph +
//      incremental dirty, D20/D23) is the next slice's gate.
//    * Re-derive is REBUILD (remove+add); the APPLY in-place fast-path + COW
//      closure copy (§2.5/D11) are not exercised. The engine's RemoveItem does
//      NOT refuse an in-use item, so the remove-order is defensive, not required.
//    * The painter's `color` is a multi-token DoubleVec3 (a later slice) so it is
//      derived with a fixed color; the single-token `colorspace` is the editable
//      trigger. Rename here is the STRUCTURED edit (D14/D35); a free-form
//      name-changing REPARSE is the slice-1.5 MatchIdentities gap.
//
//////////////////////////////////////////////////////////////////////

#include "CstSlicePrototype.h"
#include <set>
#include <algorithm>

using namespace RISE;

//----------------------------------------------------------------------
// Descriptors (mirror the real uniformcolor_painter / lambertian_material).
//----------------------------------------------------------------------
static ChunkDescriptor PainterDesc()
{
	ChunkDescriptor cd;
	cd.keyword = "uniformcolor_painter"; cd.category = ChunkCategory::Painter;
	{ ParameterDescriptor p; p.name = "name";       p.kind = ValueKind::String;     cd.parameters.push_back(p); }
	{ ParameterDescriptor p; p.name = "color";      p.kind = ValueKind::DoubleVec3;  cd.parameters.push_back(p); }
	{ ParameterDescriptor p; p.name = "colorspace"; p.kind = ValueKind::String;      cd.parameters.push_back(p); }
	return cd;
}
static ChunkDescriptor MaterialDesc()
{
	ChunkDescriptor cd;
	cd.keyword = "lambertian_material"; cd.category = ChunkCategory::Material;
	{ ParameterDescriptor p; p.name = "name";        p.kind = ValueKind::String;    cd.parameters.push_back(p); }
	{ ParameterDescriptor p; p.name = "reflectance"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; cd.parameters.push_back(p); }
	return cd;
}

// Trivia-insensitive derivation key (as in slice 1.5).
static std::string DerivationKey( const cst::Green* chunk )
{
	std::string k = chunk->role + "{";
	for( const auto& p : chunk->kids )
		if( p->kind == cst::NK::Param ) {
			std::string val;
			for( const auto& v : p->kids ) if( v->kind == cst::NK::Tok && v->role == "pvalue" ) val = v->text;
			k += p->role + "=" + val + ";";
		}
	return k + "}";
}

static bool IsPainter ( const cst::Green* c ) { return c->kind == cst::NK::Chunk && c->role == "uniformcolor_painter"; }
static bool IsMaterial( const cst::Green* c ) { return c->kind == cst::NK::Chunk && c->role == "lambertian_material"; }
static std::string NameOf( const cst::Green* c ) { std::string n; cst::ParamValueTextIn(c, "name", n); return n; }
static std::string ReflOf( const cst::Green* c ) { std::string r = "none"; cst::ParamValueTextIn(c, "reflectance", r); return r; }

//----------------------------------------------------------------------
// Apply-layer derive (real engine), with reference RESOLUTION + dangling.
//----------------------------------------------------------------------
enum class DStatus { Ok, Dangling };

static void DerivePainter( const cst::Green* c, Job* job )
{
	std::string name = NameOf(c), cspace = "Rec709RGB_Linear";
	cst::ParamValueTextIn( c, "colorspace", cspace );
	double color[3] = { 0.5, 0.5, 0.5 };   // multi-token DoubleVec3 -> fixed here (later slice)
	job->AddUniformColorPainter( name.c_str(), color, cspace.c_str() );   // add-only; removal is a separate phase
}
static DStatus DeriveMaterial( const cst::Green* c, Job* job )
{
	std::string name = NameOf(c), refl = ReflOf(c);
	// Resolve the reference (D9/§2.9): the target painter must exist, else FLAG.
	if( refl != "none" && !job->GetPainters()->GetItem( refl.c_str() ) ) return DStatus::Dangling;
	job->AddLambertianMaterial( name.c_str(), refl.c_str() );            // add-only; removal is a separate phase
	return DStatus::Ok;
}

//----------------------------------------------------------------------
// Forward-cone incremental derive over a TRACED dependency graph, with a
// NodeId-keyed cache (so a rename is detected and cleaned up, not leaked).
//----------------------------------------------------------------------
struct CacheEntry { std::string key; std::string name; std::string role; };   // last-derived key + name + role, per chunk NodeId
struct RefStats {
	int painterCalls = 0, materialCalls = 0;
	std::vector<std::string> rederived;
	std::vector<std::string> dangling;
};

static void IncrementalDeriveRef( const cst::GP& doc, const cst::IdMap& ids, Job* job,
                                  std::map<int,CacheEntry>& cache, RefStats& st )
{
	auto cid   = [&]( const cst::Green* c ) -> int { auto it = ids.find(c); return it != ids.end() ? it->second : -1; };

	std::vector<const cst::Green*> painters, materials;
	for( const auto& c : doc->kids ) {
		if( IsPainter (c.get()) ) painters.push_back( c.get() );
		if( IsMaterial(c.get()) ) materials.push_back( c.get() );
	}

	// TRACE edges: dependents[producerName] = [consumer chunks] (the reverse index, D4).
	std::map<std::string,std::vector<const cst::Green*>> dependents;
	for( auto* m : materials ) { std::string r = ReflOf(m); if( r != "none" ) dependents[r].push_back( m ); }

	// Dirty set by derivation-key change, keyed by chunk NodeId.
	std::set<const cst::Green*> dirty;
	std::vector<const cst::Green*> all = painters; all.insert( all.end(), materials.begin(), materials.end() );
	for( auto* c : all ) {
		auto it = cache.find( cid(c) );
		if( it == cache.end() || it->second.key != DerivationKey(c) ) dirty.insert( c );
	}

	// FORWARD-CONE (one hop): a dirty producer drags its direct consumers into the
	// dirty set, so a consumer re-derives even when its OWN CST is unchanged. NB:
	// one hop suffices for this 2-level scene; a TRANSITIVE worklist BFS + a real
	// topological sort for depth >= 3 (painter -> checker -> material) is the next slice.
	for( auto* p : painters ) if( dirty.count(p) ) for( auto* m : dependents[NameOf(p)] ) dirty.insert( m );

	// The cached (old) name per chunk NodeId -- so a rename drops the OLD engine
	// object. Only possible because the cache is NodeId-keyed (a name-keyed cache
	// could not tell "renamed" from "old vanished + new appeared").
	auto oldName = [&]( const cst::Green* c ) -> std::string {
		auto it = cache.find( cid(c) );
		return it != cache.end() ? it->second.name : NameOf(c);
	};

	// DELETION (D20 edge lifecycle): a cache entry whose chunk is gone from the
	// document. Drop its engine object (by cached role -> the right manager) and
	// FORCE-DIRTY its referrers so they re-resolve and surface as dangling
	// (become-dangling), instead of silently pointing at a removed target.
	std::set<int> liveIds;
	for( auto* c : all ) liveIds.insert( cid(c) );
	std::vector<int> vanished;
	for( const auto& kv : cache ) if( !liveIds.count(kv.first) ) vanished.push_back( kv.first );
	for( int vid : vanished ) {
		CacheEntry e = cache[vid];
		if( e.role == "lambertian_material" ) {
			if( job->GetMaterials()->GetItem(e.name.c_str()) ) job->GetMaterials()->RemoveItem(e.name.c_str());
		} else {
			if( job->GetPainters()->GetItem(e.name.c_str())    ) job->GetPainters()->RemoveItem(e.name.c_str());
			if( job->GetFunction2Ds()->GetItem(e.name.c_str()) ) job->GetFunction2Ds()->RemoveItem(e.name.c_str());
		}
		for( auto* m : dependents[e.name] ) dirty.insert( m );   // referrers re-resolve -> dangling
		cache.erase( vid );
	}

	// REMOVE phase, then ADD phase. The ADD order (producers before consumers) is
	// load-bearing: a consumer resolves its reference against the freshly re-added
	// producer; all dirty objects are removed before any re-add so a re-add never
	// collides with a stale entry. (NB: the engine's RemoveItem does NOT refuse an
	// in-use item -- a material holds its painter via the BRDF's own refcount, not
	// the manager's use-list -- so removing consumers before producers is defensive
	// tidiness, not a hard requirement.)
	for( auto* m : materials ) if( dirty.count(m) ) {
		std::string n = oldName(m);
		if( job->GetMaterials()->GetItem(n.c_str()) ) job->GetMaterials()->RemoveItem(n.c_str());
	}
	for( auto* p : painters ) if( dirty.count(p) ) {
		std::string n = oldName(p);
		// a uniform color painter dual-registers in the painter + function-2D
		// managers (Job::AddUniformColorPainter); drop both so the re-add is clean.
		if( job->GetPainters()->GetItem(n.c_str())    ) job->GetPainters()->RemoveItem(n.c_str());
		if( job->GetFunction2Ds()->GetItem(n.c_str()) ) job->GetFunction2Ds()->RemoveItem(n.c_str());
	}
	// ADD phase, topological order (producers before consumers): a material's
	// reference resolves against the freshly-(re)added painter.
	for( auto* p : painters ) if( dirty.count(p) ) {
		DerivePainter( p, job ); st.painterCalls++; st.rederived.push_back( NameOf(p) );
		cache[cid(p)] = { DerivationKey(p), NameOf(p), p->role };
	}
	for( auto* m : materials ) if( dirty.count(m) ) {
		DStatus s = DeriveMaterial( m, job ); st.materialCalls++; st.rederived.push_back( NameOf(m) );
		if( s == DStatus::Dangling ) st.dangling.push_back( NameOf(m) );
		cache[cid(m)] = { DerivationKey(m), NameOf(m), m->role };
	}
}

// Rename a chunk + rewrite every referrer's reference (D14: from the traced set).
static cst::GP RenameChunk( const cst::GP& doc, const std::string& keyword,
                            const std::string& oldName, const std::string& newName, cst::IdMap& ids )
{
	cst::GP d = cst::SetParamValue( doc, keyword, oldName, "name", newName, ids );
	// rewrite referrers (here: any material whose reflectance was oldName)
	std::vector<std::string> toFix;
	for( const auto& c : d->kids )
		if( IsMaterial(c.get()) && ReflOf(c.get()) == oldName ) toFix.push_back( NameOf(c.get()) );
	for( const auto& mname : toFix )
		d = cst::SetParamValue( d, "lambertian_material", mname, "reflectance", newName, ids );
	return d;
}

// Delete a chunk (by keyword + name) from the document.
static cst::GP DeleteChunk( const cst::GP& doc, const std::string& keyword, const std::string& name )
{
	std::vector<cst::GP> kids;
	for( const auto& c : doc->kids ) {
		if( c->kind == cst::NK::Chunk && c->role == keyword ) {
			std::string nm; cst::ParamValueTextIn( c.get(), "name", nm );
			if( nm == name ) continue;   // drop this chunk
		}
		kids.push_back( c );
	}
	return cst::node( cst::NK::Document, kids, "document" );
}

static bool Has( const std::vector<std::string>& v, const std::string& s ) { return std::find(v.begin(), v.end(), s) != v.end(); }

int main()
{
	using namespace cst;
	std::printf( "CstReferenceSliceTest -- slice 2 (references: traced edge, forward-cone, rename, dangling)\n" );
	(void)PainterDesc(); (void)MaterialDesc();   // descriptors define the schema the binder uses

	const std::string src =
		"uniformcolor_painter {\n\tname p\n\tcolor 1 0 0\n\tcolorspace sRGB\n}\n\n"
		"lambertian_material {\n\tname mat\n\treflectance p\n}\n";

	//------------------------------------------------------------------
	// 1. Initial derive + traced edge + reference resolution.
	//------------------------------------------------------------------
	std::printf( "[1] initial derive: producer + consumer, reference resolves\n" );
	IdMap ids; int nid = 1;
	GP doc = ParseStr( src, ids, nid );
	Job* job = new Job();
	std::map<int,CacheEntry> cache;

	RefStats s1; IncrementalDeriveRef( doc, ids, job, cache, s1 );
	Check( s1.painterCalls == 1 && s1.materialCalls == 1, "initial: painter + material both derived" );
	Check( s1.dangling.empty(), "reflectance 'p' resolves (no dangling)" );
	Check( job->GetPainters()->GetItem("p") && job->GetMaterials()->GetItem("mat"), "engine has painter p + material mat" );

	//------------------------------------------------------------------
	// 2. Forward-cone: edit the PRODUCER -> consumer re-derives too (freshness:
	//    mat's own CST is unchanged, but it is pulled in via the cone).
	//------------------------------------------------------------------
	std::printf( "[2] forward-cone: edit producer -> consumer re-derives (freshness)\n" );
	unsigned long long matSer1 = job->GetMaterials()->GetItemSerial("mat");
	GP doc2 = SetParamValue( doc, "uniformcolor_painter", "p", "colorspace", "Rec709RGB_Linear", ids );
	RefStats s2; IncrementalDeriveRef( doc2, ids, job, cache, s2 );
	Check( s2.painterCalls == 1 && s2.materialCalls == 1, "producer edit re-derives producer AND consumer" );
	Check( Has(s2.rederived, "p") && Has(s2.rederived, "mat"), "both p and mat in the re-derived set (forward cone)" );
	Check( job->GetMaterials()->GetItemSerial("mat") != matSer1, "consumer 'mat' re-derived though its own CST was unchanged (freshness)" );

	//------------------------------------------------------------------
	// 3. Unrelated edit: a chunk outside the cone does NOT re-derive the consumer.
	//------------------------------------------------------------------
	std::printf( "[3] unrelated edit: consumer is NOT re-derived (node-granular over the graph)\n" );
	const std::string src3 = src + "\nuniformcolor_painter {\n\tname q\n\tcolor 0 1 0\n\tcolorspace sRGB\n}\n";
	IdMap ids3; int nid3 = 1;
	GP d3 = ParseStr( src3, ids3, nid3 );
	Job* job3 = new Job();
	std::map<int,CacheEntry> cache3;
	RefStats t1; IncrementalDeriveRef( d3, ids3, job3, cache3, t1 );      // initial: p, mat, q
	unsigned long long matSer3 = job3->GetMaterials()->GetItemSerial("mat");
	unsigned long long pSer3   = job3->GetPainters()->GetItemSerial("p");
	GP d3b = SetParamValue( d3, "uniformcolor_painter", "q", "colorspace", "Rec709RGB_Linear", ids3 );
	RefStats t2; IncrementalDeriveRef( d3b, ids3, job3, cache3, t2 );
	Check( t2.painterCalls == 1 && t2.materialCalls == 0, "editing unrelated 'q' re-derives only q" );
	Check( Has(t2.rederived, "q") && !Has(t2.rederived, "mat") && !Has(t2.rederived, "p"), "mat and p untouched by an unrelated edit" );
	Check( job3->GetMaterials()->GetItemSerial("mat") == matSer3
	    && job3->GetPainters()->GetItemSerial("p")   == pSer3, "mat + p engine instances unchanged (serial witness)" );
	job3->release();

	//------------------------------------------------------------------
	// 4. Rename the producer: referrer rewritten, reference survives, NO leak
	//    (the NodeId-keyed cache detects the rename and drops the old object).
	//------------------------------------------------------------------
	std::printf( "[4] rename producer p->p2: referrer rewritten, reference survives, old object not leaked\n" );
	unsigned int painterCountBefore = job->GetPainters()->getItemCount();      // includes the default "none" painter
	unsigned int func2DCountBefore  = job->GetFunction2Ds()->getItemCount();   // uniform painters dual-register here too
	GP doc4 = RenameChunk( doc2, "uniformcolor_painter", "p", "p2", ids );
	Check( ReflOf( FindChunk(doc4, "lambertian_material", "mat") ) == "p2", "material's reflectance rewritten p -> p2" );
	RefStats s4; IncrementalDeriveRef( doc4, ids, job, cache, s4 );
	Check( s4.dangling.empty(), "after rename, reference still resolves (to p2)" );
	Check( job->GetPainters()->GetItem("p2") && !job->GetPainters()->GetItem("p"), "engine now has p2, NOT the orphaned p" );
	Check( job->GetPainters()->getItemCount() == painterCountBefore, "no NET painter leak after rename (NodeId-keyed cache drops the old name)" );
	Check( job->GetFunction2Ds()->getItemCount() == func2DCountBefore, "no NET function-2D leak after rename (dual-registration also cleaned)" );

	//------------------------------------------------------------------
	// 5. Dangling reference: a material referencing a missing painter is FLAGGED.
	//------------------------------------------------------------------
	std::printf( "[5] dangling reference is flagged, not silently derived\n" );
	const std::string srcBad =
		"uniformcolor_painter {\n\tname p\n\tcolor 1 0 0\n\tcolorspace sRGB\n}\n\n"
		"lambertian_material {\n\tname mbad\n\treflectance ghost\n}\n";
	IdMap idsB; int nidB = 1;
	GP dB = ParseStr( srcBad, idsB, nidB );
	Job* jobB = new Job();
	std::map<int,CacheEntry> cacheB;
	RefStats sB; IncrementalDeriveRef( dB, idsB, jobB, cacheB, sB );
	Check( Has(sB.dangling, "mbad"), "material referencing a missing painter is flagged dangling" );
	Check( jobB->GetMaterials()->GetItem("mbad") == nullptr, "the dangling material is NOT silently created in the engine" );
	jobB->release();

	//------------------------------------------------------------------
	// 6. Diamond: two consumers share one producer -> editing the producer
	//    re-derives BOTH (the reverse index fans out).
	//------------------------------------------------------------------
	std::printf( "[6] diamond: two materials share a painter -> editing it re-derives BOTH\n" );
	{
		const std::string srcD =
			"uniformcolor_painter {\n\tname pd\n\tcolor 1 0 0\n\tcolorspace sRGB\n}\n\n"
			"lambertian_material {\n\tname m1\n\treflectance pd\n}\n\n"
			"lambertian_material {\n\tname m2\n\treflectance pd\n}\n";
		IdMap di; int dn = 1; GP dd = ParseStr( srcD, di, dn );
		Job* jd = new Job(); std::map<int,CacheEntry> cD;
		RefStats r0; IncrementalDeriveRef( dd, di, jd, cD, r0 );
		Check( r0.painterCalls == 1 && r0.materialCalls == 2, "diamond initial: 1 painter + 2 materials" );
		unsigned long long sm1 = jd->GetMaterials()->GetItemSerial("m1");
		unsigned long long sm2 = jd->GetMaterials()->GetItemSerial("m2");
		GP dd2 = SetParamValue( dd, "uniformcolor_painter", "pd", "colorspace", "Rec709RGB_Linear", di );
		RefStats r1; IncrementalDeriveRef( dd2, di, jd, cD, r1 );
		Check( r1.painterCalls == 1 && r1.materialCalls == 2, "editing the shared painter re-derives BOTH dependents (fan-out)" );
		Check( jd->GetMaterials()->GetItemSerial("m1") != sm1
		    && jd->GetMaterials()->GetItemSerial("m2") != sm2, "both m1 and m2 were re-derived" );
		jd->release();
	}

	//------------------------------------------------------------------
	// 7. Reference retarget: mr.reflectance pa->pb moves the edge. After, the
	//    OLD target no longer invalidates mr; the NEW target does.
	//------------------------------------------------------------------
	std::printf( "[7] reference retarget pa->pb moves the dependency edge\n" );
	{
		const std::string srcR =
			"uniformcolor_painter {\n\tname pa\n\tcolor 1 0 0\n\tcolorspace sRGB\n}\n\n"
			"uniformcolor_painter {\n\tname pb\n\tcolor 0 1 0\n\tcolorspace sRGB\n}\n\n"
			"lambertian_material {\n\tname mr\n\treflectance pa\n}\n";
		IdMap ri; int rn = 1; GP rd = ParseStr( srcR, ri, rn );
		Job* jr = new Job(); std::map<int,CacheEntry> cR;
		RefStats q0; IncrementalDeriveRef( rd, ri, jr, cR, q0 );
		GP rd2 = SetParamValue( rd, "lambertian_material", "mr", "reflectance", "pb", ri );
		RefStats q1; IncrementalDeriveRef( rd2, ri, jr, cR, q1 );
		Check( Has(q1.rederived,"mr") && q1.dangling.empty(), "retargeted material re-derives + resolves to pb" );
		unsigned long long smr = jr->GetMaterials()->GetItemSerial("mr");
		GP rd3 = SetParamValue( rd2, "uniformcolor_painter", "pa", "colorspace", "Rec709RGB_Linear", ri );
		RefStats q2; IncrementalDeriveRef( rd3, ri, jr, cR, q2 );
		Check( !Has(q2.rederived,"mr") && jr->GetMaterials()->GetItemSerial("mr") == smr, "editing the OLD target pa no longer re-derives mr (edge moved)" );
		GP rd4 = SetParamValue( rd3, "uniformcolor_painter", "pb", "colorspace", "Rec709RGB_Linear", ri );  // real change (was sRGB)
		RefStats q3; IncrementalDeriveRef( rd4, ri, jr, cR, q3 );
		Check( Has(q3.rederived,"mr"), "editing the NEW target pb re-derives mr (edge moved here)" );
		jr->release();
	}

	//------------------------------------------------------------------
	// 8. Delete the producer (referrer NOT rewritten): its engine object is
	//    removed AND the orphaned referrer surfaces as dangling (become-
	//    dangling), not silently bound to a removed target (D20 edge lifecycle).
	//------------------------------------------------------------------
	std::printf( "[8] delete producer: engine object removed + referrer becomes dangling (flagged)\n" );
	{
		const std::string srcDel =
			"uniformcolor_painter {\n\tname pp\n\tcolor 1 0 0\n\tcolorspace sRGB\n}\n\n"
			"lambertian_material {\n\tname mm\n\treflectance pp\n}\n";
		IdMap di; int dn = 1; GP dd = ParseStr( srcDel, di, dn );
		Job* jd = new Job(); std::map<int,CacheEntry> cD;
		RefStats d0; IncrementalDeriveRef( dd, di, jd, cD, d0 );
		Check( d0.dangling.empty() && jd->GetPainters()->GetItem("pp") && jd->GetMaterials()->GetItem("mm"),
		       "initial: pp + mm derived, reference resolves" );
		GP dd2 = DeleteChunk( dd, "uniformcolor_painter", "pp" );   // delete painter, leave mm referencing it
		RefStats d1; IncrementalDeriveRef( dd2, di, jd, cD, d1 );
		Check( !jd->GetPainters()->GetItem("pp"), "deleted painter's engine object is removed" );
		Check( Has(d1.dangling, "mm"), "orphaned material 'mm' becomes dangling (flagged)" );
		Check( !jd->GetMaterials()->GetItem("mm"), "the dangling material is not silently bound to a removed target" );
		jd->release();
	}

	job->release();
	return CheckSummary();
}
