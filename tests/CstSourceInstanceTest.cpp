//////////////////////////////////////////////////////////////////////
//
//  CstSourceInstanceTest.cpp - 87 step 3a: `source` on a `standard_object`, the COLLAPSE case.
//
//  `standard_object { name I  source S  position ... }` INSTANCES a single-node source (a leaf,
//  a container, or a csg_object), producing exactly ONE object under the instancing chunk's own
//  name.  `I` takes S's bindings -- geometry / material / modifier / shader / radiance map /
//  interior medium / shadow flags -- while its LOCAL TRANSFORM IS ITS OWN: S's position is
//  DROPPED, not composed, which is what makes `position` mean "where the copy goes".  Anything
//  written explicitly on the instancing chunk overrides the inherited value.  The source keeps
//  rendering; `source` copies, it does not hide (the one hide-on-reference mechanism in tree,
//  CSGObject::AssignObjects, is OWNERSHIP -- the operand is consumed; `source` consumes nothing).
//
//  Locks in:
//    [round-trip]  an authored `source` scene round-trips byte-for-byte.
//    [derive]      the expansion == the hand-written standard_object it stands for.
//    [transform]   S's local transform is DROPPED, not composed (the load-bearing choice).
//    [inherit]     bindings come across; [override] an explicit binding on the instance wins.
//    [container]   a container source; [csg] a csg_object source; [chain] `I source S source T`.
//    [parent]      `source` + `parent` is ALLOWED (an instance must be placeable in the tree).
//    [refuse]      geometry+source / undeclared / forward-reference / self / CSG operand /
//                  source-with-children (3b) / duplicate document-level name -- each separately.
//    [provenance]  the manager records (entry -> instancing chunk, source node); the source
//                  itself has none.
//    [visible]     the source still renders after being instanced.
//    [incremental] an edit to a `source` chunk refuses -> full-derive fallback.
//
//////////////////////////////////////////////////////////////////////

#include "CstRenderEquivalence.h"
#include "../src/Library/Cst/Cst.h"

using namespace RISE;
using namespace RISE::Cst;
using namespace risequiv;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

static const std::string HDR = "RISE ASCII SCENE 7\n";

// Geometry + two materials + a modifier the bodies below reference.
static std::string Scene( const std::string& body )
{
	return HDR
		+ "sphere_geometry\n{\nname geo\nradius 1\n}\n"
		+ "box_geometry\n{\nname boxg\nwidth 1\nheight 1\ndepth 1\n}\n"
		+ "uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
		+ "uniformcolor_painter\n{\nname p2\ncolor 0.25 0.25 0.25\n}\n"
		+ "lambertian_material\n{\nname m\nreflectance p\n}\n"
		+ "lambertian_material\n{\nname m2\nreflectance p2\n}\n"
		+ "bumpmap_modifier\n{\nname bump\nfunction p\nscale 0.1\n}\n"
		+ body;
}

// Derive a scene and return (dump, diagnostics).
static std::string DumpCst( const std::string& scene, std::vector<std::string>* outDiags = nullptr )
{
	Job* j = new Job();
	Document d = ParseToCst( scene );
	std::vector<std::string> diags;
	DeriveToJob( d, *j, &diags );
	if( outDiags ) *outDiags = diags;
	std::string s = DumpJob( *j );
	j->release();
	return s;
}

// True when deriving `scene` produced at least one diagnostic containing `needle`.
static bool RefusedWith( const std::string& scene, const char* needle, std::string* outAll = nullptr )
{
	std::vector<std::string> diags;
	DumpCst( scene, &diags );
	std::string all;
	for( std::size_t i = 0; i < diags.size(); ++i ) { all += diags[i]; all += "\n"; }
	if( outAll ) *outAll = all;
	return all.find( needle ) != std::string::npos;
}

int main()
{
	std::printf( "CstSourceInstanceTest -- 87 step 3a: `source` instancing, the collapse case\n" );

	const std::string SRC_LEAF = "standard_object\n{\nname S\ngeometry geo\nmaterial m\nposition 2 0 0\n}\n";

	// ---------------------------------------------------------------- round-trip
	// [round-trip] the authored form is stored verbatim in the CST -- the expansion is a
	// DERIVE-time act (INV-3/INV-4), so the file the author wrote is the file they get back.
	{
		const std::string scene = Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" );
		Check( SerializeCst( ParseToCst( scene ) ) == scene, "round-trip: a `source` scene round-trips byte-for-byte" );
	}

	// ---------------------------------------------------------------- derive
	// [derive] + [inherit] the expansion is INDISTINGUISHABLE from the hand-written object it
	// stands for: same geometry, same material, same bbox.
	{
		std::vector<std::string> diags;
		const std::string got = DumpCst( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ), &diags );
		const std::string want = DumpCst( Scene( SRC_LEAF + "standard_object\n{\nname I\ngeometry geo\nmaterial m\nposition 5 0 0\n}\n" ) );
		Check( diags.empty(), "derive: a well-formed `source` chunk derives with no diagnostics" );
		Check( got == want, "derive: the instance == the hand-written standard_object it stands for (geometry + material inherited)" );
	}

	// [transform] THE LOAD-BEARING CHOICE.  S sits at x=2; I says `position 5 0 0`.  I lands at
	// x=5 -- S's local transform is DROPPED, not composed.  Were it composed, I would be at x=7.
	// Pinned against BOTH the right answer and the wrong one, so the test cannot pass vacuously.
	{
		const std::string got  = DumpCst( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ) );
		const std::string at5  = DumpCst( Scene( SRC_LEAF + "standard_object\n{\nname I\ngeometry geo\nmaterial m\nposition 5 0 0\n}\n" ) );
		const std::string at7  = DumpCst( Scene( SRC_LEAF + "standard_object\n{\nname I\ngeometry geo\nmaterial m\nposition 7 0 0\n}\n" ) );
		Check( got == at5, "transform: the instance's local transform is ITS OWN (lands at x=5, S's x=2 dropped)" );
		Check( got != at7, "transform: the instance is NOT composed with the source's transform (would be x=7)" );
	}

	// [transform] an instance with NO transform of its own sits at the ORIGIN, not at S's pose --
	// "dropped" means dropped, not "defaulted to the source's".
	{
		const std::string got  = DumpCst( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\n}\n" ) );
		const std::string want = DumpCst( Scene( SRC_LEAF + "standard_object\n{\nname I\ngeometry geo\nmaterial m\n}\n" ) );
		Check( got == want, "transform: an instance with no transform of its own is at the ORIGIN, not at the source's pose" );
	}

	// [transform] the sharp form of the same rule: S carries a `scale`, I gives only a
	// `position`.  A rule that INHERITED transforms would hand I a silent 3x scale it never
	// asked for -- a wrong SIZE, which no amount of looking at `position` would explain.
	{
		const std::string src = "standard_object\n{\nname S\ngeometry geo\nmaterial m\nposition 2 0 0\nscale 3 3 3\n}\n";
		const std::string got  = DumpCst( Scene( src + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ) );
		const std::string want = DumpCst( Scene( src + "standard_object\n{\nname I\ngeometry geo\nmaterial m\nposition 5 0 0\n}\n" ) );
		Check( got == want, "transform: the instance does NOT inherit the source's `scale` (every transform param is its own)" );
	}

	// [reference] `source` is a descriptor-declared Reference into ChunkCategory::Object, so the
	// shared reference graph traces it -- which is what makes a RENAME of the source rewrite the
	// instancing chunk instead of silently dangling it.  Pinned because the descriptor IS the
	// accepted-parameter set: a `source` declared as a plain String would parse identically and
	// break only here.
	{
		std::vector<std::string> diags;
		Document d = ParseToCst( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ) );
		Document d2 = DocRename( d, DocFindByName( d, "standard_object/S" ), "S2", &diags );
		const std::string out = SerializeCst( d2 );
		Check( diags.empty() && out.find( "source S2" ) != std::string::npos && out.find( "source S\n" ) == std::string::npos,
		       "reference: renaming the source rewrites the instancing chunk's `source` (the reference graph traces it)" );
		std::vector<std::string> dd;
		DumpCst( out, &dd );
		Check( dd.empty(), "reference: ... and the renamed scene still derives cleanly" );
	}

	// [override] an explicit binding on the instancing chunk WINS over the inherited one.
	{
		const std::string got  = DumpCst( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\nmaterial m2\nposition 5 0 0\n}\n" ) );
		const std::string want = DumpCst( Scene( SRC_LEAF + "standard_object\n{\nname I\ngeometry geo\nmaterial m2\nposition 5 0 0\n}\n" ) );
		const std::string inh  = DumpCst( Scene( SRC_LEAF + "standard_object\n{\nname I\ngeometry geo\nmaterial m\nposition 5 0 0\n}\n" ) );
		Check( got == want, "override: `material m2` on the instancing chunk overrides the inherited `m`" );
		Check( got != inh,  "override: ... and the result really differs from the inherited binding" );
	}

	// [inherit] the non-obvious slots come across too: modifier + shadow flags.
	{
		const std::string src = "standard_object\n{\nname S\ngeometry geo\nmaterial m\nmodifier bump\ncasts_shadows FALSE\n}\n";
		const std::string got  = DumpCst( Scene( src + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ) );
		const std::string want = DumpCst( Scene( src + "standard_object\n{\nname I\ngeometry geo\nmaterial m\nmodifier bump\ncasts_shadows FALSE\nposition 5 0 0\n}\n" ) );
		Check( got == want, "inherit: `modifier` and the shadow flags come across with the rest" );
	}

	// [visible] `source` COPIES.  The source subtree keeps rendering -- unlike a CSG operand,
	// which its composite CONSUMES and hides.  Both objects are present and world-visible.
	{
		const std::string dump = DumpCst( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ) );
		Check( dump.find( "  S geometry=geo" ) != std::string::npos && dump.find( "  I geometry=geo" ) != std::string::npos,
		       "visible: both the source and the instance exist as objects" );
		Check( dump.find( "  S geometry=geo material=m modifier=(none) shader=(none) radiance_map=(none) interior_medium=(none) visible=1" ) != std::string::npos,
		       "visible: the SOURCE is still world-visible -- `source` copies, it does not hide (that is CSG operand OWNERSHIP, a different mechanism)" );
	}

	// [container] a CONTAINER source (no geometry) instances to another container.
	{
		const std::string src = "standard_object\n{\nname S\nposition 2 0 0\n}\n";
		std::vector<std::string> diags;
		const std::string got  = DumpCst( Scene( src + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ), &diags );
		const std::string want = DumpCst( Scene( src + "standard_object\n{\nname I\nposition 5 0 0\n}\n" ) );
		Check( diags.empty() && got == want, "container: a container source instances to a container at the instance's own pose" );
	}

	// [parent] `source` + `parent` is ALLOWED -- an instance is a node, and a node must be
	// placeable in the tree.  Its world transform composes with the parent's, as any node's does.
	{
		const std::string body = "standard_object\n{\nname P\nposition 10 0 0\n}\n"
		                       + SRC_LEAF
		                       + "standard_object\n{\nname I\nsource S\nparent P\nposition 5 0 0\n}\n";
		const std::string want = "standard_object\n{\nname P\nposition 10 0 0\n}\n"
		                       + SRC_LEAF
		                       + "standard_object\n{\nname I\ngeometry geo\nmaterial m\nparent P\nposition 5 0 0\n}\n";
		std::vector<std::string> diags;
		const std::string got = DumpCst( Scene( body ), &diags );
		Check( diags.empty(), "parent: `source` + `parent` is allowed" );
		Check( got == DumpCst( Scene( want ) ), "parent: the instance composes under its parent like any other node" );
	}

	// [chain] `I source S`, `S source T` -- nearer overrides farther, instance overrides both.
	{
		const std::string body = "standard_object\n{\nname T\ngeometry geo\nmaterial m\nposition 1 0 0\n}\n"
		                         "standard_object\n{\nname S\nsource T\nmaterial m2\nposition 2 0 0\n}\n"
		                         "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n";
		const std::string want = "standard_object\n{\nname T\ngeometry geo\nmaterial m\nposition 1 0 0\n}\n"
		                         "standard_object\n{\nname S\ngeometry geo\nmaterial m2\nposition 2 0 0\n}\n"
		                         "standard_object\n{\nname I\ngeometry geo\nmaterial m2\nposition 5 0 0\n}\n";
		std::vector<std::string> diags;
		const std::string got = DumpCst( Scene( body ), &diags );
		Check( diags.empty() && got == DumpCst( Scene( want ) ),
		       "chain: `I source S source T` -- S's override of T carries into I, transforms stay each node's own" );
	}

	// [csg] a csg_object source.  The instance is built through the SOURCE's chunk type, so it
	// is a csg_object too, sharing the operands (each composite transforms rays into its OWN
	// frame first, so the copy really is placed by its own `position`).
	{
		const std::string body = "standard_object\n{\nname a\ngeometry geo\nmaterial m\n}\n"
		                         "standard_object\n{\nname b\ngeometry boxg\nmaterial m\n}\n"
		                         "csg_object\n{\nname S\nobja a\nobjb b\noperation subtraction\nmaterial m\nposition 2 0 0\n}\n"
		                         "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n";
		const std::string want = "standard_object\n{\nname a\ngeometry geo\nmaterial m\n}\n"
		                         "standard_object\n{\nname b\ngeometry boxg\nmaterial m\n}\n"
		                         "csg_object\n{\nname S\nobja a\nobjb b\noperation subtraction\nmaterial m\nposition 2 0 0\n}\n"
		                         "csg_object\n{\nname I\nobja a\nobjb b\noperation subtraction\nmaterial m\nposition 5 0 0\n}\n";
		std::vector<std::string> diags;
		const std::string got = DumpCst( Scene( body ), &diags );
		Check( diags.empty(), "csg: a csg_object source expands with no diagnostics" );
		Check( got == DumpCst( Scene( want ) ), "csg: the instance is a csg_object with the source's operands, at its OWN position" );
	}

	// [csg] a parameter the SOURCE's chunk type cannot express is named, not silently dropped.
	{
		const std::string body = "standard_object\n{\nname a\ngeometry geo\nmaterial m\n}\n"
		                         "standard_object\n{\nname b\ngeometry boxg\nmaterial m\n}\n"
		                         "csg_object\n{\nname S\nobja a\nobjb b\noperation union\n}\n"
		                         "standard_object\n{\nname I\nsource S\nscale 2 2 2\n}\n";
		Check( RefusedWith( Scene( body ), "`scale`" ),
		       "refuse: a `scale` on an instance of a csg_object (which has no such param) names the offending param" );
	}

	// ---------------------------------------------------------------- refusals
	// [refuse] geometry + source: mutually exclusive, counted and refused BEFORE any mutation
	// (the sweep_geometry precedent).  Zero forms stays legal -- that is the container.
	{
		Check( RefusedWith( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\ngeometry boxg\n}\n" ),
		                    "mutually exclusive" ),
		       "refuse: `geometry` and `source` on one chunk" );
	}

	// [refuse] a source that does not exist at all.
	{
		Check( RefusedWith( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource nosuch\n}\n" ),
		                    "no `standard_object` / `csg_object` of that name exists" ),
		       "refuse: `source` naming an object that does not exist" );
	}

	// [refuse] a FORWARD reference.  This is also the recursion guard: a mutual `source` pair
	// needs one of its two links to point forward, so declare-before-use makes cycles impossible.
	{
		std::string all;
		Check( RefusedWith( Scene( "standard_object\n{\nname I\nsource S\n}\n" + SRC_LEAF ),
		                    "declared LATER", &all ),
		       "refuse: `source` naming an object declared LATER (the forward reference / cycle guard)" );
		// The mutual pair the rule exists to make impossible.
		Check( RefusedWith( Scene( "standard_object\n{\nname A\nsource B\n}\nstandard_object\n{\nname B\nsource A\n}\n" ),
		                    "DECLARED EARLIER" ),
		       "refuse: a mutual `source` pair (A source B, B source A) cannot be authored" );
	}

	// [refuse] a chunk naming ITSELF.
	{
		Check( RefusedWith( Scene( "standard_object\n{\nname I\nsource I\n}\n" ), "names the chunk ITSELF" ),
		       "refuse: `source` naming the chunk itself" );
	}

	// [refuse] a CSG OPERAND: its matrix is interpreted in the composite's local frame, not the
	// world, so a copy placed by a world `position` would not land where the number says.
	{
		const std::string body = "standard_object\n{\nname a\ngeometry geo\nmaterial m\n}\n"
		                         "standard_object\n{\nname b\ngeometry boxg\nmaterial m\n}\n"
		                         "csg_object\n{\nname C\nobja a\nobjb b\noperation union\n}\n"
		                         "standard_object\n{\nname I\nsource a\n}\n";
		Check( RefusedWith( Scene( body ), "is a CSG OPERAND" ), "refuse: `source` naming a CSG operand" );
	}

	// [refuse] a source WITH CHILDREN is a multi-node subtree -- refused, not silently
	// root-only-instanced, which would drop most of what the author pointed at.
	{
		const std::string body = SRC_LEAF
		                       + "standard_object\n{\nname kid\ngeometry boxg\nmaterial m\nparent S\n}\n"
		                       + "standard_object\n{\nname I\nsource S\n}\n";
		std::string all;
		Check( RefusedWith( Scene( body ), "step 3b", &all ), "refuse: `source` naming a node that HAS CHILDREN (subtree instancing is 3b)" );
		Check( all.find( "has CHILDREN" ) != std::string::npos, "refuse: ... and the message says so plainly" );
	}

	// [refuse] DOCUMENT-level name collision: two object chunks declaring the entry name.  The
	// manager pre-check alone cannot see this when the other chunk is declared AFTER -- it does
	// not exist yet -- so the scan is over the document, and it names both chunks.
	{
		const std::string body = SRC_LEAF
		                       + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n"
		                       + "standard_object\n{\nname I\ngeometry boxg\nmaterial m\n}\n";
		std::string all;
		Check( RefusedWith( Scene( body ), "declared by MORE THAN ONE object chunk", &all ),
		       "refuse: the entry name is also declared by a LATER authored chunk (document-level mis-targeting)" );
		Check( all.find( "item " ) != std::string::npos, "refuse: ... and the message names the colliding items" );
	}

	// [refuse] the SECOND implementation of the exclusivity rule -- the one in the parser's
	// own Finalize, which the expansion path never reaches because it consumes `source` first.
	// `instance_array` passes every non-generator param through to the standard_object it
	// synthesizes, alongside a `geometry` from its `template`, so a `source` there arrives at
	// Finalize together with a geometry.  That is the reachable path to the parser-side gate,
	// and it must refuse rather than pick one silently.
	{
		std::vector<std::string> diags;
		const std::string dump = DumpCst( Scene( SRC_LEAF + "instance_array\n{\nname g\ntemplate geo\nmaterial m\nsource S\ncount_u 1\n}\n" ), &diags );
		std::string all;
		for( std::size_t i = 0; i < diags.size(); ++i ) { all += diags[i]; all += "\n"; }
		// The parser's SPECIFIC text reaches the log, not `diags`: ExpandInstanceArray runs
		// outside PASS-2's armed g_cstFinalizeDiagSink window, so all that comes back here is
		// its own generic apply-failed line.  What this pins is the behaviour that matters --
		// the chunk is refused and produces nothing.  (Red-proven by disabling BOTH of the
		// parser's `source` gates: with them gone the object is built and this goes green->red.)
		Check( !diags.empty(), "refuse: `geometry` + `source` reaching the PARSER (via instance_array pass-through) is refused" );
		Check( dump.find( "  g[0,0] " ) == std::string::npos, "refuse: ... and no object is created by the refused chunk" );
	}

	// ---------------------------------------------------------------- provenance
	// [provenance] the manager records where a synthesized entry came from.  A MAP LOOKUP:
	// nothing anywhere may reconstruct this by splitting the name on `.` or probing for a `[`.
	{
		Job* j = new Job();
		Document d = ParseToCst( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ) );
		std::vector<std::string> diags;
		DeriveToJob( d, *j, &diags );
		const IObjectManager* objs = j->GetObjects();
		const char* inst = 0; const char* src = 0;
		const bool got = objs && objs->GetObjectProvenance( "I", &inst, &src );
		Check( got && inst && src && std::string( inst ) == "I" && std::string( src ) == "S",
		       "provenance: the instance records (instancing chunk `I`, source node `S`)" );
		const char* i2 = 0;
		Check( objs && !objs->GetObjectProvenance( "S", &i2, 0 ),
		       "provenance: the SOURCE is an ordinary authored object -- no provenance record" );
		const char* i3 = 0;
		Check( objs && !objs->GetObjectProvenance( "nosuch", &i3, 0 ),
		       "provenance: an unknown name has no record" );
		j->release();
	}

	// [provenance] retired with the object.  RemoveItem drops the row, exactly as it drops the
	// parent link -- so a re-add under the same name cannot inherit a stale origin.
	{
		Job* j = new Job();
		Document d = ParseToCst( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ) );
		std::vector<std::string> diags;
		DeriveToJob( d, *j, &diags );
		IObjectManager* objs = j->GetObjects();
		const char* before = 0;
		const bool had = objs && objs->GetObjectProvenance( "I", &before, 0 );
		const bool removed = j->RemoveObject( "I" );
		const char* after = 0;
		Check( had && removed && !objs->GetObjectProvenance( "I", &after, 0 ),
		       "provenance: removing the object retires its provenance row" );
		j->release();
	}

	// [closure] the property that lets 3a get away with a PER-CHUNK incremental refusal where
	// `instance_array` needed a document-wide one: `source` is a descriptor-declared Reference,
	// so editing the SOURCE puts the instancing chunk in the edit closure.  Without this the
	// incremental apply would re-point S while I kept its stale copy of S's bindings.
	{
		Document d = ParseToCst( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ) );
		const NodeId sid = DocFindByName( d, "standard_object/S" );
		const NodeId iid = DocFindByName( d, "standard_object/I" );
		const std::vector<NodeId> closure = DocEditClosure( d, sid );
		bool hasI = false;
		for( std::size_t k = 0; k < closure.size(); ++k ) if( closure[k] == iid ) hasI = true;
		Check( sid != 0 && iid != 0 && hasI, "closure: editing the SOURCE puts the instancing chunk in the edit closure" );
	}

	// ---------------------------------------------------------------- incremental
	// [incremental] editing a `source` chunk refuses -> the caller full-derives, which
	// re-expands from the document.  (Its own Finalize cannot apply it: the expansion is
	// DeriveToJob PASS-2's job.)
	{
		Job* j = new Job();
		Document d = ParseToCst( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ) );
		std::vector<std::string> diags;
		DeriveToJob( d, *j, &diags );
		const NodeId id = DocFindByName( d, "standard_object/I" );
		Document d2 = DocSetParamValue( d, id, "position", 0, "9 0 0" );
		std::vector<std::string> di;
		const int applied = DeriveToJobIncremental( d2, *j, std::vector<NodeId>( 1, id ), &di );
		std::string all;
		for( std::size_t i = 0; i < di.size(); ++i ) { all += di[i]; all += "\n"; }
		Check( applied == 0 && all.find( "carries `source" ) != std::string::npos,
		       "incremental: a `standard_object` carrying `source` refuses -> full-derive fallback" );
		// The sibling case must NOT regress: an ordinary object chunk still applies incrementally.
		const NodeId sid = DocFindByName( d, "standard_object/S" );
		Document d3 = DocSetParamValue( d, sid, "position", 0, "3 0 0" );
		std::vector<std::string> di2;
		const int applied2 = DeriveToJobIncremental( d3, *j, std::vector<NodeId>( 1, sid ), &di2 );
		Check( applied2 >= 1 && di2.empty(), "incremental: an ordinary standard_object still applies incrementally (no over-broad refusal)" );
		j->release();
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
