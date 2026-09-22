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
			"RISE ASCII SCENE 7\n"
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
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname m\nreflectance p\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n";
		IncResult r = RunInc( s, "uniformcolor_painter/p" );
		Check( r.applied == 0 && r.diagCount > 0, "painter closure REFUSED (applied 0 + diagnosed)" );
		Check( r.dumpBefore == r.dumpAfter, "painter refusal mutated NOTHING" );
	}

	// DL-164 structural guard (docs/DEBT_LEDGER.md residual, added by the DL-25 review):
	// an expression_painter/scalar_painter's `sample(name)`/`sample_scalar(name)` resolves
	// its bound IPainter*/IScalarPainter* ONCE at attach time and holds it under a RAW
	// addref (ExpressionPainter.h's BoundPainterRefs) -- no re-resolution hook exists. That
	// is safe TODAY only because EVERY Painter-category chunk (this switch's `default` arm,
	// just above) is refused from this incremental path and always falls back to a full
	// DeriveToJob, which rebuilds BoundPainterRefs from scratch. If a future change ever
	// widens this switch to admit ChunkCategory::Painter, an in-place re-Finalize of a
	// SAMPLED painter chunk would leave every OTHER chunk's already-resolved sample()/
	// sample_scalar() pointer referencing the OLD (possibly freed) painter object --
	// silently, since a raw addref does not detect its target's replacement. This case
	// pins the refusal specifically for a chunk that USES sample() -- not just an ordinary
	// painter -- so a future incremental-Painter change trips this test and must address
	// BindPainterRefs' doc comment before it can pass.
	{
		std::string s =
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\nname rock\ncolor 0.5 0.5 0.5\n}\n"
			"expression_painter\n{\nname wet\nexpr sample(rock)\n}\n"
			"lambertian_material\n{\nname m\nreflectance wet\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n";
		IncResult r = RunInc( s, "expression_painter/wet" );
		Check( r.applied == 0 && r.diagCount > 0,
		       "DL-164: a sample()-using expression_painter's closure is REFUSED by the incremental path (applied 0 + diagnosed)" );
		Check( r.dumpBefore == r.dumpAfter, "DL-164: the refusal mutated NOTHING" );
	}
	{
		std::string s =
			"RISE ASCII SCENE 7\n"
			"scalar_painter\n{\nname rough\nvalue 0.3\n}\n"
			"scalar_painter\n{\nname wetrough\nexpression sample_scalar(rough)\n}\n"
			"ggx_material\n{\nname m\nrd rough\nalphax wetrough\nalphay wetrough\n}\n";
		IncResult r = RunInc( s, "scalar_painter/wetrough" );
		Check( r.applied == 0 && r.diagCount > 0,
		       "DL-164: a sample_scalar()-using scalar_painter's closure is REFUSED by the incremental path (applied 0 + diagnosed)" );
		Check( r.dumpBefore == r.dumpAfter, "DL-164: the refusal mutated NOTHING (scalar pipe)" );
	}

	// translucent_material closure -> REFUSED (reads ambient painter-colour cache).
	{
		std::string s =
			"RISE ASCII SCENE 7\n"
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
			"RISE ASCII SCENE 7\n"
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
			"RISE ASCII SCENE 7\n"
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
			"RISE ASCII SCENE 7\n"
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
			"RISE ASCII SCENE 7\n"
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
			"RISE ASCII SCENE 7\n"
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
			"RISE ASCII SCENE 7\n"
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
		Check( applied == 0 && !di.empty(), "dangling-ior: dangling ior ref REFUSED (preflight checks the Painter managers -- ior is now {Painter})" );
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
			"RISE ASCII SCENE 7\n"
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

	// INTERLEAVING (review #1 -> workstream #3): a closure where an OBJECT and a later non-object
	// ENTITY both consume the edited chunk used to be REFUSED (the entity-only rollback could not
	// restore a re-pointed object if the later entity failed).  Workstream #3 sorts the apply
	// ENTITIES-FIRST so objects are re-pointed LAST -- a failure is always at an entity before any
	// object is touched -- so the closure now APPLIES.  `base` is referenced by BOTH object `o`
	// and (later) luminaire material `lum`; closure(base) = [base, o, lum], applied entities-first.
	{
		std::string s =
			"RISE ASCII SCENE 7\n"
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
		Check( applied >= 3, "interleaving: object-before-later-entity closure now APPLIES entities-first (was refused; workstream #3)" );
		Check( DumpJob( *j ) == before, "interleaving: re-applying the closure with unchanged values is idempotent (consistent re-derive)" );
		j->release();
	}

	// INTERLEAVING -- a REAL value edit through the interleaved closure matches a FULL derive
	// (proves entities-first is CORRECT, not merely non-crashing -- the idempotent re-apply above
	// would pass for any successful apply; this pins the actual result to the ground truth).
	{
		std::string s =
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"uniformcolor_painter\n{\nname p2\ncolor 0.1 0.2 0.3\n}\n"
			"lambertian_material\n{\nname base\nreflectance p\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial base\n}\n"
			"lambertian_luminaire_material\n{\nname lum\nexitance p\nmaterial base\n}\n";
		Document doc = ParseToCst( s );
		Job* j = new Job(); std::vector<std::string> d0; DeriveToJob( doc, *j, &d0 );
		const NodeId baseId = DocFindByName( doc, "lambertian_material/base" );
		Document docE = DocSetParamValue( doc, baseId, "reflectance", 0, "p2" );   // re-point base.reflectance p -> p2
		std::vector<NodeId> closure = DocEditClosure( docE, baseId );
		std::vector<std::string> di; int applied = DeriveToJobIncremental( docE, *j, closure, &di );
		Job* jFull = new Job(); std::vector<std::string> dF; DeriveToJob( docE, *jFull, &dF );   // ground truth
		Check( applied >= 3, "interleaving-edit: a real value edit through the interleaved closure APPLIES" );
		Check( DumpJob( *j ) == DumpJob( *jFull ), "interleaving-edit: incremental result == full derive of the edited doc (entities-first is correct)" );
		j->release(); jFull->release();
	}

	// INTERLEAVING -- a re-Finalize FAILURE through the interleaved closure rolls back ATOMICALLY:
	// `o` is never stranded, because entities-first re-points `o` only after every entity succeeds,
	// so a base/lum failure leaves `o` untouched (the exact reason the refusal could be removed).
	{
		std::string s =
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname base\nreflectance p\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial base\n}\n"
			"lambertian_luminaire_material\n{\nname lum\nexitance p\nmaterial base\n}\n";
		Document doc = ParseToCst( s );
		Job* j = new Job(); std::vector<std::string> d0; DeriveToJob( doc, *j, &d0 );
		const std::string before = DumpJob( *j );
		const NodeId baseId = DocFindByName( doc, "lambertian_material/base" );
		Document docD = DocSetParamValue( doc, baseId, "reflectance", 0, "0.5" );   // numeric in a pure-painter slot -> re-Finalize fails
		std::vector<NodeId> closure = DocEditClosure( docD, baseId );
		std::vector<std::string> di; int applied = DeriveToJobIncremental( docD, *j, closure, &di );
		Check( applied == 0 && !di.empty(), "interleaving-fail: a re-Finalize failure in the interleaved closure is refused" );
		Check( DumpJob( *j ) == before, "interleaving-fail: rolled back ATOMICALLY -- base+lum restored, object o never stranded" );
		j->release();
	}

	// OBJECT NUMERIC (review #1, 2nd pass): a numeric in an object reference slot bypasses the
	// preflight literal-skip; interior_medium is applied by a SEPARATE SetObjectInteriorMedium
	// AFTER AddObject re-points (and MOVES) the object, so without the refusal the object would
	// be half-mutated + the TLAS left stale. The preflight now REFUSES it atomically.
	{
		std::string s =
			"RISE ASCII SCENE 7\n"
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

	// OBJECT VECTOR ARITY UNDER A PARENT CHAIN (DL-32 round-2 review, P2-2).
	// Cst.cpp's rollback design (the "PART A" comment block above the entity
	// capture loop, and the "Objects are NOT captured" comment on it) argues
	// atomicity from "a failure can only occur at an entity, BEFORE any
	// object is touched" -- entities are captured/restored on a re-Finalize
	// failure; objects are not, because the slot-precise preflight validates
	// every object's REFERENCE param before any mutation, and the
	// entities-first sort means no object is reached until every entity has
	// already succeeded.
	//
	// DL-32 round 2 added a SECOND way an object's OWN Finalize can fail: a
	// wrong-arity position/orientation/quaternion/scale/matrix.  The
	// preflight does not see it (it only validates Reference-kind params),
	// and unlike a Finalize() `return false`, `ParseStateBag::GetVec3` does
	// NOT abort the caller -- it zero-fills the output and only LATCHES
	// `HadHardError()`, so the object's own Finalize keeps running and calls
	// `pJob.AddObject(...)` with the zero-filled value BEFORE the apply loop
	// notices the latch and gives up.  That mutation is never undone: only
	// non-object entities are captured/restored.
	//
	// Grounded in what the closure actually contains (checked below, not
	// assumed): editing `parentObj` closes over {parentObj, childObj} --
	// both objects, no entities (an object's dependents, via `parent`, are
	// only ever OTHER objects) -- sorted entities-first-then-by-document-
	// index, so parentObj (declared first) is always processed before
	// childObj.  The loop breaks at parentObj, so childObj's own Finalize is
	// NEVER called -- its own `position` param is never touched.
	//
	// Measured anyway: childObj's WORLD bbox is corrupted too.
	// `DeriveToJobIncremental`'s failure branch still calls
	// `pJob.ComposeObjectHierarchy()` (kept for a different reason -- the 87
	// step 2 "detached child" self-heal) before returning 0, and that call
	// recomposes EVERY child's world transform from its parent's CURRENT
	// (now zero-filled) local transform.  So the "leaves every sibling
	// object untouched" reading of the old comment is FALSE for a parent
	// chain: the API reports the edit as refused (applied 0 + a diagnostic,
	// matching the documented contract for every OTHER refusal in this
	// suite), but the live Job is left with BOTH the edited parent and every
	// descendant showing a wrong world transform, and nothing here triggers
	// a corrective full re-derive.
	//
	// Filed as DL-156 (docs/DEBT_LEDGER.md) -- fix recipe: extend `ObjState`
	// to also snapshot each closure object's pre-edit LOCAL transform, add
	// an object-transform rollback that restores it before
	// `ComposeObjectHierarchy()` runs on the failure path, mirroring
	// `rollbackEntities()`.  This test PINS the CONFIRMED-CURRENT (not yet
	// fixed) behaviour, both so a future change cannot make it worse
	// unnoticed and so a future fix is caught (these two assertions will
	// need to flip when DL-156 closes).
	{
		std::string s =
			"RISE ASCII SCENE 7\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname parentObj\ngeometry g\nposition 5 0 0\n}\n"
			"standard_object\n{\nname childObj\ngeometry g\nparent parentObj\nposition 1 0 0\n}\n";
		Document doc = ParseToCst( s );
		Job* j = new Job(); std::vector<std::string> d0; DeriveToJob( doc, *j, &d0 );
		Check( d0.empty(), "arity-parent-chain: baseline scene derives cleanly" );
		const std::string before = DumpJob( *j );
		Check( before.find( "parentObj geometry=g material=(none) modifier=(none) shader=(none) radiance_map=(none) interior_medium=(none) visible=1 bbox=[4 -1 -1 .. 6 1 1]" ) != std::string::npos,
		       "arity-parent-chain: baseline parentObj bbox is centred at its authored position (5,0,0)" );
		Check( before.find( "childObj geometry=g material=(none) modifier=(none) shader=(none) radiance_map=(none) interior_medium=(none) visible=1 bbox=[5 -1 -1 .. 7 1 1]" ) != std::string::npos,
		       "arity-parent-chain: baseline childObj bbox is centred at parent+local (5,0,0)+(1,0,0)=(6,0,0)" );
		const NodeId pId = DocFindByName( doc, "standard_object/parentObj" );
		Document docM = DocSetParamValue( doc, pId, "position", 0, "-4 4" );   // wrong arity: 2 tokens, not 3
		std::vector<NodeId> closure = DocEditClosure( docM, pId );
		Check( closure.size() == 2, "arity-parent-chain: closure is exactly {parentObj, childObj} -- no entities" );
		std::vector<std::string> di;
		int applied = DeriveToJobIncremental( docM, *j, closure, &di );
		Check( applied == 0 && !di.empty(), "arity-parent-chain: malformed parent position REFUSED (applied 0 + diagnosed)" );
		const std::string after = DumpJob( *j );
		Check( after == before,
		       "arity-parent-chain: DL-156 -- atomic refusal: dump after == dump before (neither parent nor child moved)" );
		Check( after.find( "parentObj geometry=g material=(none) modifier=(none) shader=(none) radiance_map=(none) interior_medium=(none) visible=1 bbox=[4 -1 -1 .. 6 1 1]" ) != std::string::npos,
		       "arity-parent-chain: DL-156 -- parentObj bbox is unchanged at authored position (5,0,0)" );
		Check( after.find( "childObj geometry=g material=(none) modifier=(none) shader=(none) radiance_map=(none) interior_medium=(none) visible=1 bbox=[5 -1 -1 .. 7 1 1]" ) != std::string::npos,
		       "arity-parent-chain: DL-156 -- childObj bbox is unchanged at parent+local (6,0,0)" );

		// DL-156: matrix-arity case. A malformed 12-token matrix (needs 16) on parentObj is refused
		// before mutation, leaving parent and child bboxes unchanged.
		{
			Document docMat = DocSetOrAddParamValue( doc, pId, "matrix", 0, "1 0 0 0  0 1 0 0  0 0 1 0" ); // 12 tokens, not 16
			std::vector<NodeId> closureMat = DocEditClosure( docMat, pId );
			std::vector<std::string> diMat;
			int appliedMat = DeriveToJobIncremental( docMat, *j, closureMat, &diMat );
			Check( appliedMat == 0 && !diMat.empty(), "arity-parent-chain: malformed parent matrix REFUSED (applied 0 + diagnosed)" );
			const std::string afterMat = DumpJob( *j );
			Check( afterMat == before, "arity-parent-chain: malformed matrix refusal mutated NOTHING" );
			Check( afterMat.find( "parentObj geometry=g material=(none) modifier=(none) shader=(none) radiance_map=(none) interior_medium=(none) visible=1 bbox=[4 -1 -1 .. 6 1 1]" ) != std::string::npos,
			       "arity-parent-chain: parentObj bbox unchanged after malformed matrix refusal" );
			Check( afterMat.find( "childObj geometry=g material=(none) modifier=(none) shader=(none) radiance_map=(none) interior_medium=(none) visible=1 bbox=[5 -1 -1 .. 7 1 1]" ) != std::string::npos,
			       "arity-parent-chain: childObj bbox unchanged after malformed matrix refusal" );
		}

		// DL-156: quaternion-arity case. A malformed 5-token quaternion (needs 4) on parentObj is refused
		// before mutation, leaving parent and child bboxes unchanged.
		{
			Document docQuat = DocSetOrAddParamValue( doc, pId, "quaternion", 0, "0 0 0 1 0" ); // 5 tokens, not 4
			std::vector<NodeId> closureQuat = DocEditClosure( docQuat, pId );
			std::vector<std::string> diQuat;
			int appliedQuat = DeriveToJobIncremental( docQuat, *j, closureQuat, &diQuat );
			Check( appliedQuat == 0 && !diQuat.empty(), "arity-parent-chain: malformed parent quaternion REFUSED (applied 0 + diagnosed)" );
			const std::string afterQuat = DumpJob( *j );
			Check( afterQuat == before, "arity-parent-chain: malformed quaternion refusal mutated NOTHING" );
			Check( afterQuat.find( "parentObj geometry=g material=(none) modifier=(none) shader=(none) radiance_map=(none) interior_medium=(none) visible=1 bbox=[4 -1 -1 .. 6 1 1]" ) != std::string::npos,
			       "arity-parent-chain: parentObj bbox unchanged after malformed quaternion refusal" );
			Check( afterQuat.find( "childObj geometry=g material=(none) modifier=(none) shader=(none) radiance_map=(none) interior_medium=(none) visible=1 bbox=[5 -1 -1 .. 7 1 1]" ) != std::string::npos,
			       "arity-parent-chain: childObj bbox unchanged after malformed quaternion refusal" );
		}
		j->release();
	}

	// OPTIONAL-SLOT REMOVAL (workstream #3): removing radiance_map from a stable object CLEARS it in
	// place and matches a FULL derive of the edited doc (a fresh object has the slot unset).
	{
		Document doc = ParseToCst(
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname m\nreflectance p\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial m\nradiance_map p\n}\n" );
		Job* j = new Job(); std::vector<std::string> d0; DeriveToJob( doc, *j, &d0 );
		const NodeId oId = DocFindByName( doc, "standard_object/o" );
		Document docE = DocSetParamValue( doc, oId, "radiance_map", 0, "none" );
		std::vector<NodeId> closure = DocEditClosure( docE, oId );
		std::vector<std::string> di; int applied = DeriveToJobIncremental( docE, *j, closure, &di );
		Job* jFull = new Job(); std::vector<std::string> dF; DeriveToJob( docE, *jFull, &dF );
		IObjectPriv* o = j->GetObjects() ? j->GetObjects()->GetItem( "o" ) : 0;
		Check( applied >= 1, "removal radiance_map: edit applies in place (not refused)" );
		Check( o && o->GetRadianceMap() == 0, "removal radiance_map: slot CLEARED" );
		Check( DumpJob( *j ) == DumpJob( *jFull ), "removal radiance_map: incremental == full derive of the edited doc" );
		j->release(); jFull->release();
	}

	// OPTIONAL-SLOT REMOVAL (workstream #3): removing a modifier from a stable object CLEARS
	// it in place and matches a FULL derive of the edited doc (a fresh object has the slot
	// unset).  This block was added in the relief-modifier arc as the twin of an identical
	// one over `bumpmap_modifier`; that chunk was REMOVED 2026-09-06
	// (docs/RELIEF_MODIFIER_DESIGN.md 7.5) and its block went with it, leaving this one as
	// the modifier-slot removal check.  `height` is a SCALAR-painter reference (a colour
	// painter is refused there), hence the dedicated scalar_painter.
	{
		Document doc = ParseToCst(
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname m\nreflectance p\n}\n"
			"scalar_painter\n{\nname h\nvalue 0.1\n}\n"
			"relief_modifier\n{\nname r\nheight h\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial m\nmodifier r\n}\n" );
		Job* j = new Job(); std::vector<std::string> d0; DeriveToJob( doc, *j, &d0 );
		const NodeId oId = DocFindByName( doc, "standard_object/o" );
		Document docE = DocSetParamValue( doc, oId, "modifier", 0, "none" );
		std::vector<NodeId> closure = DocEditClosure( docE, oId );
		std::vector<std::string> di; int applied = DeriveToJobIncremental( docE, *j, closure, &di );
		Job* jFull = new Job(); std::vector<std::string> dF; DeriveToJob( docE, *jFull, &dF );
		IObjectPriv* o = j->GetObjects() ? j->GetObjects()->GetItem( "o" ) : 0;
		Check( applied >= 1, "removal relief_modifier: edit applies in place (not refused)" );
		Check( o && o->GetModifier() == 0, "removal relief_modifier: slot CLEARED" );
		Check( DumpJob( *j ) == DumpJob( *jFull ), "removal relief_modifier: incremental == full derive of the edited doc" );
		j->release(); jFull->release();
	}

	// OPTIONAL-SLOT REMOVAL (workstream #3): removing interior_medium from a stable object CLEARS it in
	// place and matches a FULL derive of the edited doc (a fresh object has the slot unset).
	{
		Document doc = ParseToCst(
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname m\nreflectance p\n}\n"
			"homogeneous_medium\n{\nname med\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial m\ninterior_medium med\n}\n" );
		Job* j = new Job(); std::vector<std::string> d0; DeriveToJob( doc, *j, &d0 );
		const NodeId oId = DocFindByName( doc, "standard_object/o" );
		Document docE = DocSetParamValue( doc, oId, "interior_medium", 0, "none" );
		std::vector<NodeId> closure = DocEditClosure( docE, oId );
		std::vector<std::string> di; int applied = DeriveToJobIncremental( docE, *j, closure, &di );
		Job* jFull = new Job(); std::vector<std::string> dF; DeriveToJob( docE, *jFull, &dF );
		IObjectPriv* o = j->GetObjects() ? j->GetObjects()->GetItem( "o" ) : 0;
		Check( applied >= 1, "removal interior_medium: edit applies in place (not refused)" );
		Check( o && o->GetInteriorMedium() == 0, "removal interior_medium: slot CLEARED" );
		Check( DumpJob( *j ) == DumpJob( *jFull ), "removal interior_medium: incremental == full derive of the edited doc" );
		j->release(); jFull->release();
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
