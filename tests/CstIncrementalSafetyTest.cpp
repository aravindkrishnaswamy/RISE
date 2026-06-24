//////////////////////////////////////////////////////////////////////
//
//  CstIncrementalSafetyTest.cpp - regression coverage for the slice-0 safety
//  guards of DeriveToJobIncremental (docs/agentic-redesign/21-stable-apply-and-
//  resolver.md). The bulk review found nine P1s in the original drop/re-add apply;
//  slice 0 made it SAFE by REFUSING what it cannot reverse + aborting on a failed
//  drop. The cost suite only exercised the happy path, so a regression could
//  silently re-open a hole -- this suite locks the refusals + the abort in.
//
//  Each refusal returns 0 + a diagnostic and mutates NOTHING (caller falls back to
//  a full re-derive; D51: never a silent partial undo).
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Cst/Cst.h"
#include "CstRenderEquivalence.h"      // Job, DumpJob

#include <cstdio>
#include <string>
#include <vector>

using namespace RISE;
using namespace RISE::Cst;
using namespace risequiv;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

// Derive `scene`, then run DeriveToJobIncremental over the closure of the chunk
// named `keyword/name`; return the applied count + diagnostics + the pre/post dump.
struct IncResult { int applied; size_t diagCount; std::string dumpBefore, dumpAfter; size_t closureSize; };
static IncResult RunInc( const std::string& scene, const char* findKey, NodeId* outId = nullptr )
{
	Document doc = ParseToCst( scene );
	NodeId id = DocFindByName( doc, findKey );
	if( outId ) *outId = id;
	Job* j = new Job(); std::vector<std::string> d0; DeriveToJob( doc, *j, &d0 );
	std::string before = DumpJob( *j );
	std::vector<NodeId> closure = DocEditClosure( doc, id );
	std::vector<std::string> di;
	int applied = DeriveToJobIncremental( doc, *j, closure, &di );
	std::string after = DumpJob( *j );
	IncResult r{ applied, di.size(), before, after, closure.size() };
	j->release();
	return r;
}

int main()
{
	std::printf( "CstIncrementalSafetyTest -- slice-0 refusal/abort guards\n" );

	// Positive control: a clean single-manager value edit (geometry radius) is
	// ACCEPTED and applies the whole closure.
	{
		std::string s =
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname m\nreflectance p\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n";
		IncResult r = RunInc( s, "sphere_geometry/g" );
		Check( r.applied == (int)r.closureSize && r.applied > 0, "clean geometry value edit ACCEPTED (applied == closure)" );
	}

	// Painter closure -> REFUSED (func2d dual-registration).
	{
		std::string s =
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname m\nreflectance p\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n";
		IncResult r = RunInc( s, "uniformcolor_painter/p" );
		Check( r.applied == 0 && r.diagCount > 0, "painter closure REFUSED (applied 0 + diagnosed)" );
		Check( r.dumpBefore == r.dumpAfter, "painter refusal mutated NOTHING" );
	}

	// translucent_material closure -> REFUSED (reads ambient painter-colour cache).
	{
		std::string s =
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"translucent_material\n{\nname tm\nref p\ntau p\n}\n";
		Document doc = ParseToCst( s );
		NodeId id = DocFindByName( doc, "translucent_material/tm" );
		// translucent's Finalize may fail without a fully-valid scene; the refusal is
		// pre-apply (by keyword), so test the refusal directly on the closure.
		Job* j = new Job(); std::vector<std::string> d0; DeriveToJob( doc, *j, &d0 );
		std::vector<NodeId> closure; closure.push_back( id );
		std::vector<std::string> di;
		int applied = DeriveToJobIncremental( doc, *j, closure, &di );
		Check( applied == 0 && !di.empty(), "translucent_material closure REFUSED (applied 0 + diagnosed)" );
		j->release();
	}

	// gltf_import (a bulk importer: one chunk spawns many entries) -> REFUSED. It has
	// no `name` param; locate its chunk by ROLE (a hardcoded index would land on a
	// header trivia node) and pass its NodeId as a hand-built closure. Today the
	// name-empty guard catches it first (it is unnamed); the gltf_import keyword guard
	// is the durable refusal that also protects a future NAMED bulk importer -- the
	// point verified here is it never half-drops.
	{
		std::string s =
			"RISE ASCII SCENE 6\n"
			"gltf_import\n{\nfile nonexistent.gltf\n}\n";
		Document doc = ParseToCst( s );
		NodeId id = 0;
		for( int i = 0, n = DocItemCount( doc ); i < n; ++i ) {
			const NodeId cid = DocNodeIdAt( doc, i );
			NodeRef nd = DocResolveNodeId( doc, cid );
			if( nd && nd->kind == NodeKind::Chunk && nd->role == "gltf_import" ) { id = cid; break; }
		}
		Check( id != 0, "gltf_import chunk located by role" );
		Job* j = new Job(); std::vector<std::string> d0; DeriveToJob( doc, *j, &d0 );
		std::vector<NodeId> closure; closure.push_back( id );
		std::vector<std::string> di;
		int applied = DeriveToJobIncremental( doc, *j, closure, &di );
		Check( applied == 0 && !di.empty(), "gltf_import (bulk importer) REFUSED (applied 0 + diagnosed)" );
		j->release();
	}

	// A document with an Animation-category chunk -> ANY incremental REFUSED (the
	// static graph cannot trace timeline String references).
	{
		std::string s =
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname m\nreflectance p\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n"
			"animation\n{\nname anim\n}\n";
		Document doc = ParseToCst( s );
		NodeId animId = DocFindByName( doc, "animation/anim" );
		if( animId != 0 ) {
			NodeId gid = DocFindByName( doc, "sphere_geometry/g" );
			Job* j = new Job(); std::vector<std::string> d0; DeriveToJob( doc, *j, &d0 );
			std::vector<NodeId> closure = DocEditClosure( doc, gid );
			std::vector<std::string> di;
			int applied = DeriveToJobIncremental( doc, *j, closure, &di );
			Check( applied == 0 && !di.empty(), "incremental REFUSED when the document has an animation chunk" );
			j->release();
		} else {
			std::printf( "  (skip animation: chunk did not parse in this build)\n" );
		}
	}

	// Abort-on-failed-drop: rename a geometry in the document, then incrementally
	// apply the renamed closure to a Job that still has the OLD name -> the drop of
	// the new name finds nothing -> ABORT (return 0 + diagnostic), no silent re-add.
	{
		std::string s =
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname m\nreflectance p\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n";
		Document doc = ParseToCst( s );
		Job* j = new Job(); std::vector<std::string> d0; DeriveToJob( doc, *j, &d0 );
		const std::string before = DumpJob( *j );
		NodeId gid = DocFindByName( doc, "sphere_geometry/g" );
		Document docR = DocRename( doc, gid, "grenamed" );
		// closure of the renamed geometry on docR (its name is now grenamed, but the
		// Job still has g); the preflight finds "grenamed" absent -> refuse ATOMICALLY.
		std::vector<NodeId> closure = DocEditClosure( docR, gid );
		std::vector<std::string> di;
		int applied = DeriveToJobIncremental( docR, *j, closure, &di );
		Check( applied == 0 && !di.empty(), "abort-on-stale-closure: renamed closure name absent in Job -> applied 0 + diagnosed" );
		Check( DumpJob( *j ) == before, "abort-on-stale-closure: REFUSED atomically -- nothing mutated (review P1.7)" );
		j->release();
	}

	// override_object present -> ANY incremental REFUSED (its String target reference is
	// invisible to the static graph, so editing the target would erase the override; P1.3).
	{
		std::string s =
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname m\nreflectance p\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial m\nposition 0 0 0\n}\n"
			"override_object\n{\nname o\nposition 5 0 0\n}\n";
		Document doc = ParseToCst( s );
		Job* j = new Job(); std::vector<std::string> d0; DeriveToJob( doc, *j, &d0 );
		Check( j->GetObjectOverrideCount() > 0, "override_object: the derive recorded the override (NoteObjectOverride)" );
		NodeId gid = DocFindByName( doc, "sphere_geometry/g" );
		std::vector<NodeId> closure = DocEditClosure( doc, gid );
		std::vector<std::string> di;
		int applied = DeriveToJobIncremental( doc, *j, closure, &di );
		Check( applied == 0 && !di.empty(), "incremental REFUSED when the doc has an override_object (P1.3)" );
		j->release();
	}

	// A value edit introducing a DANGLING reference -> REFUSED atomically (whole-plan
	// preflight; nothing mutated -- review P1.7).
	{
		std::string s =
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname m\nreflectance p\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n";
		Document doc = ParseToCst( s );
		Job* j = new Job(); std::vector<std::string> d0; DeriveToJob( doc, *j, &d0 );
		const std::string before = DumpJob( *j );
		const NodeId mId = DocFindByName( doc, "lambertian_material/m" );
		Document docD = DocSetParamValue( doc, mId, "reflectance", 0, "nosuchpainter" );
		std::vector<NodeId> closure = DocEditClosure( docD, mId );
		std::vector<std::string> di;
		int applied = DeriveToJobIncremental( docD, *j, closure, &di );
		Check( applied == 0 && !di.empty(), "dangling-reference edit REFUSED (preflight; P1.7)" );
		Check( DumpJob( *j ) == before, "dangling-reference edit refused ATOMICALLY -- nothing mutated (P1.7)" );
		j->release();
	}

	// A dangling reference edit (ior -> a non-existent name) must refuse atomically.  Historical:
	// ior was {Painter,Function} and this guarded the P1.7 "Function-gap" -- the preflight must
	// check EVERY referenceCategory, else a Function-named ior would pass a Painter-only
	// preflight, get dropped, then fail to re-Finalize -> permanently gone.  Workstream #2 made
	// ior {Painter} (the engine resolves it via scalar-then-colour painter, NEVER a Function),
	// so the gap is gone for ior; "nosuchfunc" is now caught by the Painter preflight.  The
	// all-categories preflight loop (Cst.cpp ~1335) still stands for any future multi-cat slot.
	{
		std::string s =
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"perfectrefractor_material\n{\nname glass\nrefractance p\nior 1.5\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial glass\n}\n";
		Document doc = ParseToCst( s );
		Job* j = new Job(); std::vector<std::string> d0; DeriveToJob( doc, *j, &d0 );
		const std::string before = DumpJob( *j );
		const bool hadGlass = j->GetMaterials() && j->GetMaterials()->GetItem( "glass" ) != 0;
		const NodeId mId = DocFindByName( doc, "perfectrefractor_material/glass" );
		Document docD = DocSetParamValue( doc, mId, "ior", 0, "nosuchfunc" );
		std::vector<NodeId> closure = DocEditClosure( docD, mId );
		std::vector<std::string> di;
		int applied = DeriveToJobIncremental( docD, *j, closure, &di );
		const bool stillGlass = j->GetMaterials() && j->GetMaterials()->GetItem( "glass" ) != 0;
		Check( hadGlass, "dangling-ior: precondition -- glass material derived" );
		Check( applied == 0 && !di.empty(), "dangling-ior: dangling {Function} ior ref REFUSED (preflight checks the Function managers)" );
		Check( stillGlass && DumpJob( *j ) == before, "dangling-ior: refused ATOMICALLY -- the material is NOT dropped (review P1.7 Function-gap)" );
		j->release();
	}

	// ROLLBACK (review #1, Part A): a value edit that PASSES the preflight but FAILS the
	// re-Finalize must leave the Job UNMUTATED.  A NUMERIC in a pure-painter slot is the
	// canonical case: `reflectance 0.5` looks like a literal to the preflight (skipped), so
	// the material is dropped -- then AddMaterial cannot resolve "0.5" as a painter and the
	// re-Finalize fails.  Without the rollback the material would be permanently gone; with
	// it, the captured original is restored and DumpJob is byte-identical to pre-edit.
	{
		std::string s =
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname m\nreflectance p\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n";
		Document doc = ParseToCst( s );
		Job* j = new Job(); std::vector<std::string> d0; DeriveToJob( doc, *j, &d0 );
		const std::string before = DumpJob( *j );
		const IMaterial* preMat = j->GetMaterials() ? j->GetMaterials()->GetItem( "m" ) : 0;
		const NodeId mId = DocFindByName( doc, "lambertian_material/m" );
		Document docD = DocSetParamValue( doc, mId, "reflectance", 0, "0.5" );   // numeric in a pure-painter slot
		std::vector<NodeId> closure = DocEditClosure( docD, mId );
		std::vector<std::string> di;
		int applied = DeriveToJobIncremental( docD, *j, closure, &di );
		const IMaterial* postMat = j->GetMaterials() ? j->GetMaterials()->GetItem( "m" ) : 0;
		Check( applied == 0 && !di.empty(), "rollback: numeric-in-painter-slot re-Finalize FAILS (applied 0 + diagnosed)" );
		Check( postMat != 0, "rollback: the material is RESTORED, not left dropped (review #1 Part A)" );
		Check( postMat == preMat, "rollback: the ORIGINAL material instance is restored (capture-and-restore)" );
		Check( DumpJob( *j ) == before, "rollback: Job byte-identical to pre-edit -- atomic on the Finalize-failure path" );
		j->release();
	}

	// INTERLEAVING (review #1, correctness): a closure whose doc-sorted order places an
	// OBJECT before a later non-object ENTITY is REFUSED -- the entity-only rollback could
	// not restore the re-pointed object if the later entity failed.  `base` is referenced by
	// BOTH object `o` and (later) luminaire material `lum`, so closure(base) sorts as
	// [base, o, lum]: `o` (object) precedes `lum` (entity).  Must refuse atomically.
	{
		std::string s =
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname base\nreflectance p\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial base\n}\n"
			"lambertian_luminaire_material\n{\nname lum\nexitance p\nmaterial base\n}\n";
		Document doc = ParseToCst( s );
		Job* j = new Job(); std::vector<std::string> d0; DeriveToJob( doc, *j, &d0 );
		const std::string before = DumpJob( *j );
		const NodeId baseId = DocFindByName( doc, "lambertian_material/base" );
		std::vector<NodeId> closure = DocEditClosure( doc, baseId );
		std::vector<std::string> di;
		int applied = DeriveToJobIncremental( doc, *j, closure, &di );
		Check( closure.size() >= 3, "interleaving: closure(base) includes base + o + lum" );
		Check( applied == 0 && !di.empty(), "interleaving: object-before-later-entity closure REFUSED (applied 0 + diagnosed, review #1)" );
		Check( DumpJob( *j ) == before, "interleaving: refused ATOMICALLY -- nothing mutated" );
		j->release();
	}

	// OBJECT NUMERIC (review #1, 2nd pass): a numeric in an object reference slot bypasses the
	// preflight literal-skip; interior_medium is applied by a SEPARATE SetObjectInteriorMedium
	// AFTER AddObject re-points (and MOVES) the object, so without the refusal the object would
	// be half-mutated + the TLAS left stale. The preflight now REFUSES it atomically.
	{
		std::string s =
			"RISE ASCII SCENE 6\n"
			"homogeneous_medium\n{\nname med\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\ninterior_medium med\nposition 0 0 0\n}\n";
		Document doc = ParseToCst( s );
		Job* j = new Job(); std::vector<std::string> d0; DeriveToJob( doc, *j, &d0 );
		const std::string before = DumpJob( *j );
		const NodeId oId = DocFindByName( doc, "standard_object/o" );
		Document docM = DocSetParamValue( doc, oId, "position", 0, "5 0 0" );       // moves the object
		docM = DocSetParamValue( docM, oId, "interior_medium", 0, "0.5" );          // numeric -> post-AddObject failure
		std::vector<NodeId> closure = DocEditClosure( docM, oId );
		std::vector<std::string> di;
		int applied = DeriveToJobIncremental( docM, *j, closure, &di );
		Check( applied == 0 && !di.empty(), "object-numeric: numeric interior_medium (post-AddObject) REFUSED (applied 0 + diagnosed, review #1)" );
		Check( DumpJob( *j ) == before, "object-numeric: refused ATOMICALLY -- object NOT moved, TLAS untouched (nothing mutated)" );
		j->release();
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
