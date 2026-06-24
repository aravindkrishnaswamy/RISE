//////////////////////////////////////////////////////////////////////
//
//  CstResolverTest.cpp - slice 1 of docs/agentic-redesign/21-stable-apply-and-
//  resolver.md: the shared resolver (BuildReferenceGraph). Locks in the four
//  properties that make it the AUTHORITATIVE graph rename + closure consume:
//    [consistency] its edges agree with what the derivation actually binds
//                  (no drift between the resolver and the derive -- P1.8).
//    [stamp]       a conservative fingerprint: every graph-changing edit moves it
//                  (a reference re-point / rename) and is STABLE otherwise (a
//                  non-reference value edit) -- so a cached graph's staleness is
//                  detectable in O(1) (P1.8 unstamped).
//    [namespace]   the runtime defaults (the `none` material/painter) are in the
//                  derivation namespace (P1.8 coarser-namespace) -- and stay in
//                  present in the engine (a listed default renamed/removed in
//                  Job.cpp fails this test; a Job-side addition is not auto-caught).
//    [ref-or-literal] a reference-kind value that is a pure NUMBER is a literal
//                  (e.g. ggx `roughness 0.5`), not a dangling reference -- not an
//                  edge, not a false diagnostic (P1.8 ref-or-literal).
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Cst/Cst.h"
#include "CstRenderEquivalence.h"      // Job, IObject/manager interfaces

#include <cstdio>
#include <string>
#include <vector>

using namespace RISE;
using namespace RISE::Cst;
using namespace risequiv;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

static bool HasEdge( const ReferenceGraph& g, NodeId src, NodeId tgt )
{
	for( const ReferenceUse& e : g.edges ) if( e.sourceValueNodeId == src && e.targetNodeId == tgt ) return true;
	return false;
}

static bool DiagsMention( const std::vector<std::string>& d, const char* needle )
{
	for( const std::string& s : d ) if( s.find( needle ) != std::string::npos ) return true;
	return false;
}

int main()
{
	std::printf( "CstResolverTest -- slice 1 (shared resolver: consistency / stamp / namespace / ref-or-literal)\n" );

	const std::string scene =
		"RISE ASCII SCENE 6\n"
		"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
		"lambertian_material\n{\nname m\nreflectance p\n}\n"
		"lambertian_material\n{\nname m2\nreflectance p\n}\n"
		"sphere_geometry\n{\nname g\nradius 1\n}\n"
		"standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n";

	//----------------------------------------------------------------------
	// [consistency] the graph's edges agree with the derivation's bindings.
	//----------------------------------------------------------------------
	{
		Document doc = ParseToCst( scene );
		const NodeId pid = DocFindByName( doc, "uniformcolor_painter/p" );
		const NodeId mid = DocFindByName( doc, "lambertian_material/m" );
		const NodeId gid = DocFindByName( doc, "sphere_geometry/g" );
		const NodeId oid = DocFindByName( doc, "standard_object/o" );
		const NodeId oMat  = DocParamId( doc, oid, "material", 0 );
		const NodeId oGeom = DocParamId( doc, oid, "geometry", 0 );
		const NodeId mRefl = DocParamId( doc, mid, "reflectance", 0 );

		std::vector<std::string> diags;
		ReferenceGraph gph = BuildReferenceGraph( doc, &diags );
		Check( HasEdge( gph, oMat,  mid ), "graph: object.material -> material m" );
		Check( HasEdge( gph, oGeom, gid ), "graph: object.geometry -> geometry g" );
		Check( HasEdge( gph, mRefl, pid ), "graph: material.reflectance -> painter p" );
		Check( diags.empty(), "graph: no dangling-reference diagnostics on a clean scene" );

		// derive, and confirm the ENGINE bound the object exactly where the graph says.
		Job* j = new Job(); std::vector<std::string> dd; DeriveToJob( doc, *j, &dd );
		IObject* o = j->GetObjects() ? j->GetObjects()->GetItem( "o" ) : 0;
		const IMaterial* mPtr = j->GetMaterials()  ? j->GetMaterials()->GetItem( "m" )  : 0;
		const IGeometry* gPtr = j->GetGeometries() ? j->GetGeometries()->GetItem( "g" ) : 0;
		Check( o && mPtr && o->GetMaterial() == mPtr, "derive: object o bound to material m (matches graph edge)" );
		Check( o && gPtr && o->GetGeometry() == gPtr, "derive: object o bound to geometry g (matches graph edge)" );
		j->release();
	}

	//----------------------------------------------------------------------
	// [drift] EVERY object->material/geometry edge is cross-verified against the
	// derive's ACTUAL binding (the drift guard, slice 5): if BuildReferenceGraph
	// resolved a name differently than the derive does, this fails. Painter / other
	// edges are existence-checked only (a material does not expose its painters for a
	// pointer cross-check) -- so this is a drift DETECTOR on the tested scenes, not an
	// exhaustive structural every-edge proof.
	//----------------------------------------------------------------------
	{
		const std::string s2 =
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname m1\nreflectance p\n}\n"
			"lambertian_material\n{\nname m2\nreflectance p\n}\n"
			"sphere_geometry\n{\nname g1\nradius 1\n}\n"
			"sphere_geometry\n{\nname g2\nradius 2\n}\n"
			"standard_object\n{\nname oa\ngeometry g1\nmaterial m1\n}\n"
			"standard_object\n{\nname ob\ngeometry g2\nmaterial m2\n}\n";
		Document doc = ParseToCst( s2 );
		ReferenceGraph g = BuildReferenceGraph( doc );
		Job* j = new Job(); std::vector<std::string> dd; DeriveToJob( doc, *j, &dd );
		struct Ob { const char* o; const char* m; const char* gm; };
		const Ob obs[] = { { "oa", "m1", "g1" }, { "ob", "m2", "g2" } };
		bool allMatch = true;
		for( const Ob& ob : obs ) {
			const NodeId oid = DocFindByName( doc, ( std::string( "standard_object/" ) + ob.o ).c_str() );
			const NodeId mid = DocFindByName( doc, ( std::string( "lambertian_material/" ) + ob.m ).c_str() );
			const NodeId gid = DocFindByName( doc, ( std::string( "sphere_geometry/" ) + ob.gm ).c_str() );
			if( !HasEdge( g, DocParamId( doc, oid, "material", 0 ), mid ) ) allMatch = false;
			if( !HasEdge( g, DocParamId( doc, oid, "geometry", 0 ), gid ) ) allMatch = false;
			IObject* o = j->GetObjects() ? j->GetObjects()->GetItem( ob.o ) : 0;
			const IMaterial* mp = j->GetMaterials()  ? j->GetMaterials()->GetItem( ob.m )  : 0;
			const IGeometry* gp = j->GetGeometries() ? j->GetGeometries()->GetItem( ob.gm ) : 0;
			if( !( o && mp && o->GetMaterial() == mp ) ) allMatch = false;
			if( !( o && gp && o->GetGeometry() == gp ) ) allMatch = false;
		}
		Check( allMatch, "drift: EVERY object->material/geometry graph edge matches the derive's actual binding (by pointer), across multiple objects" );
		j->release();
	}

	//----------------------------------------------------------------------
	// [stamp] conservative fingerprint: every graph-changing edit moves it (the
	// load-bearing direction); it may also move on a graph-neutral edit (safe).
	//----------------------------------------------------------------------
	{
		Document doc = ParseToCst( scene );
		const unsigned long long s0 = BuildReferenceGraph( doc ).stamp;

		// a NON-reference value edit (sphere radius) cannot change the graph -> stable.
		Document docR = DocSetParamValue( doc, DocFindByName( doc, "sphere_geometry/g" ), "radius", 0, "2" );
		Check( BuildReferenceGraph( docR ).stamp == s0, "stamp STABLE across a non-reference value edit (radius)" );

		// a reference RE-POINT (object.material m -> m2) changes the graph -> stamp moves.
		Document docP = DocSetParamValue( doc, DocFindByName( doc, "standard_object/o" ), "material", 0, "m2" );
		Check( BuildReferenceGraph( docP ).stamp != s0, "stamp CHANGES on a reference re-point (object.material m->m2)" );

		// a RENAME (material m -> mx, referrers rewritten) changes the graph -> stamp moves.
		Document docN = DocRename( doc, DocFindByName( doc, "lambertian_material/m" ), "mx" );
		Check( BuildReferenceGraph( docN ).stamp != s0, "stamp CHANGES on a rename (material m->mx)" );

		// re-stamping the SAME document is deterministic.
		Check( BuildReferenceGraph( doc ).stamp == s0, "stamp deterministic (same doc -> same stamp)" );
	}

	//----------------------------------------------------------------------
	// [namespace] the runtime defaults are in the engine's namespace + in sync.
	//----------------------------------------------------------------------
	{
		Document doc = ParseToCst( "RISE ASCII SCENE 6\n" );   // empty scene: only the engine defaults
		Job* j = new Job(); std::vector<std::string> dd; DeriveToJob( doc, *j, &dd );
		Check( j->GetMaterials() && j->GetMaterials()->GetItem( "none" ) != 0, "runtime default present: material 'none' (resolver namespace in sync)" );
		Check( j->GetPainters()  && j->GetPainters()->GetItem( "none" )  != 0, "runtime default present: painter 'none' (resolver namespace in sync)" );
		// Guard RuntimeDefaultDefs's shader ops: each listed op must be PRESENT in a
		// freshly-derived Job, so a Job.cpp default RENAMED or REMOVED (relative to the
		// list below) fails here. (A Job-side ADDITION, or a drop from RuntimeDefaultDefs
		// alone, is not auto-caught -- the latter surfaces as a dangling diagnostic only
		// when a scene references the now-unseeded default.)
		const char* shops[] = { "DefaultReflection", "DefaultRefraction", "DefaultEmission",
			"DefaultDirectLighting", "DefaultCausticPelPhotonMap", "DefaultCausticSpectralPhotonMap",
			"DefaultGlobalPelPhotonMap", "DefaultGlobalSpectralPhotonMap", "DefaultTranslucentPelPhotonMap",
			"DefaultShadowPhotonMap", "DefaultPathTracing" };
		bool allShops = j->GetShaderOps() != 0;
		for( const char* s : shops ) if( !( j->GetShaderOps() && j->GetShaderOps()->GetItem( s ) ) ) allShops = false;
		Check( allShops, "runtime defaults present: all 11 Default* shader ops (RuntimeDefaultDefs in sync with Job.cpp)" );
		j->release();
	}

	//----------------------------------------------------------------------
	// [ref-or-literal] a numeric value in a reference-kind slot is a LITERAL.
	//----------------------------------------------------------------------
	{
		// pbr_metallic_roughness_material.roughness is ValueKind::Reference but "can be
		// a painter ref OR a scalar string"; `roughness 0.5` is a literal, not a
		// dangling reference.
		Document doc = ParseToCst( "RISE ASCII SCENE 6\npbr_metallic_roughness_material\n{\nname gx\nroughness 0.5\n}\n" );
		std::vector<std::string> diags;
		ReferenceGraph gph = BuildReferenceGraph( doc, &diags );
		Check( !DiagsMention( diags, "roughness" ) && !DiagsMention( diags, "0.5" ), "ref-or-literal: numeric `roughness 0.5` is NOT a false dangling reference" );
		const NodeId gxRough = DocParamId( doc, DocFindByName( doc, "pbr_metallic_roughness_material/gx" ), "roughness", 0 );
		bool edgeFromRough = false;
		for( const ReferenceUse& e : gph.edges ) if( e.sourceValueNodeId == gxRough ) edgeFromRough = true;
		Check( !edgeFromRough, "ref-or-literal: numeric `roughness 0.5` produces NO edge" );

		// control: a NON-numeric unresolved reference IS a dangling diagnostic (a name
		// pointing at nothing) -- in a ref-or-literal slot too.
		Document doc2 = ParseToCst( "RISE ASCII SCENE 6\npbr_metallic_roughness_material\n{\nname gx\nroughness nosuchpainter\n}\n" );
		std::vector<std::string> diags2;
		BuildReferenceGraph( doc2, &diags2 );
		Check( DiagsMention( diags2, "nosuchpainter" ), "control: a non-numeric unresolved reference (a name) IS diagnosed dangling" );

		// An inline multi-number literal (`scattering 1 2 3`, the `r g b` form some
		// scalar slots accept) is also a literal -- entirely numeric tokens -> NOT a
		// dangling reference. (This is why LooksNumeric checks every token, not just
		// the first -- a per-slot ref-or-literal flag could not have covered it.)
		Document doc4 = ParseToCst( "RISE ASCII SCENE 6\ntranslucent_material\n{\nname tm\nscattering 1 2 3\n}\n" );
		std::vector<std::string> diags4;
		BuildReferenceGraph( doc4, &diags4 );
		Check( !DiagsMention( diags4, "scattering" ), "ref-or-literal: inline `scattering 1 2 3` is NOT a false dangling reference" );

		// A pure number in a PURE-reference slot (`reflectance 0.5`) is NOT a dangling
		// reference -- a number is never a name. It is a TYPE MISMATCH, which the full
		// DeriveToJob refuses at apply time (verified below); the static reference-graph
		// pass deliberately does not double-report it. (This replaced the fragile
		// per-slot ref-or-literal flag, which under-flagged ~30 real scalar slots.)
		Document doc3 = ParseToCst( "RISE ASCII SCENE 6\nlambertian_material\n{\nname m\nreflectance 0.5\n}\n" );
		std::vector<std::string> diags3;
		BuildReferenceGraph( doc3, &diags3 );
		Check( !DiagsMention( diags3, "reflectance" ), "numeric `reflectance 0.5` is NOT a dangling reference (a number is not a name)" );
		Job* jt = new Job(); std::vector<std::string> dt; int applied = DeriveToJob( doc3, *jt, &dt ); jt->release();
		Check( applied == 0 && !dt.empty(), "the type mismatch `reflectance 0.5` IS caught by the full DeriveToJob (refused at apply time)" );
	}

	//----------------------------------------------------------------------
	// [conflation] (review P1.4) scalar + colour painters share ChunkCategory::Painter
	// but live in SEPARATE managers; the (category,name) defs key cannot tell them apart.
	// A same-named pair must be FLAGGED (the edge to that name is imprecise) rather than
	// silently resolved to one.
	//----------------------------------------------------------------------
	{
		Document doc = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"scalar_painter\n{\nname p\nfile noise.dat\n}\n" );
		std::vector<std::string> diags;
		BuildReferenceGraph( doc, &diags );
		Check( DiagsMention( diags, "colour and scalar" ), "conflation: a same-named colour+scalar painter is FLAGGED by the resolver (review P1.4)" );

		// control: distinct painter names -> NO conflation diagnostic.
		Document docOk = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname pc\ncolor 0.5 0.5 0.5\n}\n"
			"scalar_painter\n{\nname ps\nfile noise.dat\n}\n" );
		std::vector<std::string> diagsOk;
		BuildReferenceGraph( docOk, &diagsOk );
		Check( !DiagsMention( diagsOk, "colour and scalar" ), "conflation control: distinct painter names -> no false flag" );
	}

	//----------------------------------------------------------------------
	// [function2d-ref] (review P1.4 sibling) a colour painter (e.g. expression_function2d,
	// ChunkCategory::Painter) ALSO registers in the Function-2D manager, so a {Function}
	// reference to it (scalar_painter.function2d) must RESOLVE -- not be a false dangling
	// with a missed closure/rename edge.  The resolver seeds (Function, name) for such
	// painters to match the derive (pFunc2DManager->GetItem).
	//----------------------------------------------------------------------
	{
		Document doc = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"expression_function2d\n{\nname noise\nexpr u*v\n}\n"
			"scalar_painter\n{\nname disp\nfunction2d noise\n}\n" );
		std::vector<std::string> diags;
		ReferenceGraph g = BuildReferenceGraph( doc, &diags );
		Check( !DiagsMention( diags, "noise" ), "function2d-ref: a {Function} reference to a dual-registered painter is NOT a false dangling (P1.4 sibling)" );
		const NodeId noiseId = DocFindByName( doc, "expression_function2d/noise" );
		Check( noiseId != 0 && DocEditClosure( noiseId, g ).size() == 2, "function2d-ref: closure(noise) = {noise, disp} = 2 (the dual-register edge is traced)" );
		Check( !DiagsMention( diags, "dimension-precisely" ), "function2d-ref control: a single Function producer is NOT flagged as a 1D/2D conflation" );
	}

	//----------------------------------------------------------------------
	// [func-conflation] (review #3) Function1D and Function2D producers share
	// ChunkCategory::Function but use TYPED lookups in the derive; the (Function,name)
	// graph cannot disambiguate them, so a same-named 1D + 2D pair must be FLAGGED.
	//----------------------------------------------------------------------
	{
		Document doc = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"piecewise_linear_function\n{\nname fx\n}\n"
			"piecewise_linear_function2d\n{\nname fx\n}\n" );
		std::vector<std::string> diags;
		BuildReferenceGraph( doc, &diags );
		Check( DiagsMention( diags, "dimension-precisely" ), "func-conflation: same-named 1D + 2D function FLAGGED by the resolver (review #3)" );
	}

	//----------------------------------------------------------------------
	// [plf1d-colour] (review #3a) a piecewise_linear_function dual-registers into the COLOUR
	// painter manager (Job::AddPiecewiseLinearFunction), so reflectance <plf1d> is a valid
	// engine binding -- the graph must trace it (else closure/rename miss the referrer).
	//----------------------------------------------------------------------
	{
		Document doc = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"piecewise_linear_function\n{\nname fx\ncp 0 0\ncp 1 1\n}\n"
			"lambertian_material\n{\nname m\nreflectance fx\n}\n" );
		std::vector<std::string> diags;
		ReferenceGraph g = BuildReferenceGraph( doc, &diags );
		const NodeId fxId = DocFindByName( doc, "piecewise_linear_function/fx" );
		Check( fxId != 0 && DocEditClosure( fxId, g ).size() == 2, "plf1d-colour: closure(fx) = {fx, m} = 2 (the reverse dual-register edge is traced)" );
		Check( !DiagsMention( diags, "reflectance -> 'fx'" ) && !DiagsMention( diags, "unresolved" ), "plf1d-colour: reflectance->plf1d is NOT a false dangling" );
	}

	//----------------------------------------------------------------------
	// [plf1d-painter-conflation] (review #3a) a plf1d and a colour painter sharing a name
	// both seed (Painter, name); the (category,name) graph cannot disambiguate -> FLAG it.
	//----------------------------------------------------------------------
	{
		Document doc = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"piecewise_linear_function\n{\nname x\ncp 0 0\ncp 1 1\n}\n"
			"uniformcolor_painter\n{\nname x\ncolor 0.5 0.5 0.5\n}\n" );
		std::vector<std::string> diags;
		BuildReferenceGraph( doc, &diags );
		Check( DiagsMention( diags, "share this name" ), "plf1d-painter-conflation: same-named plf1d + colour painter FLAGGED (review #3a)" );
	}

	//----------------------------------------------------------------------
	// [func-precise] (review #3, 2nd pass) a function1d consumer binds the same-named
	// Function1D, NOT the Function2D -- closure follows the dimension-precise edge.
	//----------------------------------------------------------------------
	{
		Document doc = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"piecewise_linear_function\n{\nname x\ncp 0 0\ncp 1 1\n}\n"
			"piecewise_linear_function2d\n{\nname x\n}\n"
			"scalar_painter\n{\nname sp\nfunction1d x\n}\n" );
		ReferenceGraph g = BuildReferenceGraph( doc, 0 );
		const NodeId f1 = DocFindByName( doc, "piecewise_linear_function/x" );
		const NodeId f2 = DocFindByName( doc, "piecewise_linear_function2d/x" );
		const NodeId sp = DocFindByName( doc, "scalar_painter/sp" );
		std::vector<NodeId> c1 = DocEditClosure( f1, g );
		std::vector<NodeId> c2 = DocEditClosure( f2, g );
		bool f1HasSp = false; for( NodeId n : c1 ) if( n == sp ) f1HasSp = true;
		bool f2HasSp = false; for( NodeId n : c2 ) if( n == sp ) f2HasSp = true;
		Check( f1 && f2 && sp && f1HasSp, "func-precise: closure(Function1D x) INCLUDES the scalar_painter.function1d consumer" );
		Check( !f2HasSp, "func-precise: closure(Function2D x) does NOT include the function1d consumer (no misbind)" );
	}

	//----------------------------------------------------------------------
	// [cp-closure] (review #2, 2nd pass) a piecewise_linear_function2d cp row embeds a
	// Function1D name; editing that Function1D must include the Function2D in the closure.
	//----------------------------------------------------------------------
	{
		Document doc = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"piecewise_linear_function\n{\nname f1\ncp 0 0\ncp 1 1\n}\n"
			"piecewise_linear_function2d\n{\nname f2\ncp 0.0 f1\ncp 1.0 f1\n}\n" );
		ReferenceGraph g = BuildReferenceGraph( doc, 0 );
		const NodeId f1 = DocFindByName( doc, "piecewise_linear_function/f1" );
		const NodeId f2 = DocFindByName( doc, "piecewise_linear_function2d/f2" );
		std::vector<NodeId> c1 = DocEditClosure( f1, g );
		bool hasF2 = false; for( NodeId n : c1 ) if( n == f2 ) hasF2 = true;
		Check( f1 && f2 && hasF2, "cp-closure: closure(Function1D f1) INCLUDES the piecewise_linear_function2d that references it via cp (review #2)" );
	}

	//----------------------------------------------------------------------
	// [painter-alias] (review P1.4, 2nd pass) a colour painter and a scalar painter sharing a
	// name: the (Painter,name) edge first-wins to ONE, so a consumer the engine binds to the
	// OTHER (here scalar_painter.base, a scalar slot) would be MISSED from that painter's
	// closure.  The conservative alias makes editing EITHER include the consumer (superset).
	//----------------------------------------------------------------------
	{
		Document doc = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname x\ncolor 0.5 0.5 0.5\n}\n"
			"scalar_painter\n{\nname x\nvalue 0.3\n}\n"
			"scalar_painter\n{\nname sp\nbase x\n}\n" );
		ReferenceGraph g = BuildReferenceGraph( doc, 0 );
		const NodeId colourX = DocFindByName( doc, "uniformcolor_painter/x" );
		const NodeId scalarX = DocFindByName( doc, "scalar_painter/x" );
		const NodeId sp = DocFindByName( doc, "scalar_painter/sp" );
		bool scalarReachesSp = false; for( NodeId n : DocEditClosure( scalarX, g ) ) if( n == sp ) scalarReachesSp = true;
		bool colourReachesSp = false; for( NodeId n : DocEditClosure( colourX, g ) ) if( n == sp ) colourReachesSp = true;
		Check( colourX && scalarX && sp && scalarReachesSp, "painter-alias: closure(scalar x) reaches the scalar consumer (alias rescues the first-wins miss, P1.4)" );
		Check( colourReachesSp, "painter-alias: closure(colour x) also reaches it (superset, never misses)" );
	}

	//----------------------------------------------------------------------
	// [transfer-precise] (review #3, 2nd-pass sibling) directvolumerendering transfer_red is
	// Function1D-only in the engine; it must bind the same-named Function1D, not a colour painter.
	//----------------------------------------------------------------------
	{
		Document doc = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname t\ncolor 1 0 0\n}\n"
			"piecewise_linear_function\n{\nname t\ncp 0 0\ncp 1 1\n}\n"
			"directvolumerendering_shader\n{\nname dvr\ntransfer_red t\n}\n" );
		ReferenceGraph g = BuildReferenceGraph( doc, 0 );
		const NodeId plf = DocFindByName( doc, "piecewise_linear_function/t" );
		const NodeId colour = DocFindByName( doc, "uniformcolor_painter/t" );
		const NodeId dvr = DocFindByName( doc, "directvolumerendering_shader/dvr" );
		bool plfHasDvr = false; for( NodeId n : DocEditClosure( plf, g ) ) if( n == dvr ) plfHasDvr = true;
		bool colourHasDvr = false; for( NodeId n : DocEditClosure( colour, g ) ) if( n == dvr ) colourHasDvr = true;
		Check( plf && colour && dvr && plfHasDvr, "transfer-precise: closure(Function1D t) INCLUDES the directvolumerendering consumer" );
		Check( !colourHasDvr, "transfer-precise: closure(colour t) does NOT (transfer_red binds Function1D, not the painter)" );
	}

	//----------------------------------------------------------------------
	// [transfer-spectral] (review #3, 2nd-pass sibling) the spectral-DVR transfer_spectral is
	// Function2D-only in the engine; with a same-named 1D+2D pair it must bind the Function2D.
	//----------------------------------------------------------------------
	{
		Document doc = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"piecewise_linear_function\n{\nname s\ncp 0 0\ncp 1 1\n}\n"
			"piecewise_linear_function2d\n{\nname s\n}\n"
			"spectraldirectvolumerendering_shader\n{\nname sdvr\ntransfer_spectral s\n}\n" );
		ReferenceGraph g = BuildReferenceGraph( doc, 0 );
		const NodeId f1 = DocFindByName( doc, "piecewise_linear_function/s" );
		const NodeId f2 = DocFindByName( doc, "piecewise_linear_function2d/s" );
		const NodeId sdvr = DocFindByName( doc, "spectraldirectvolumerendering_shader/sdvr" );
		bool f2HasSdvr = false; for( NodeId n : DocEditClosure( f2, g ) ) if( n == sdvr ) f2HasSdvr = true;
		bool f1HasSdvr = false; for( NodeId n : DocEditClosure( f1, g ) ) if( n == sdvr ) f1HasSdvr = true;
		Check( f1 && f2 && sdvr && f2HasSdvr, "transfer-spectral: closure(Function2D s) INCLUDES the spectral-DVR consumer" );
		Check( !f1HasSdvr, "transfer-spectral: closure(Function1D s) does NOT (transfer_spectral binds Function2D)" );
	}

	//----------------------------------------------------------------------
	// [painter-decl-func2d] (review #3, 3rd-pass exhaustive table) displaced_geometry.displacement
	// is declared {Painter} but the engine binds it via pFunc2DManager (Function2D, which holds
	// plf2d). It must reach a piecewise_linear_function2d target in closure (the (Painter,name)
	// key would have missed it -- plf2d is not in the painter managers).
	//----------------------------------------------------------------------
	{
		Document doc = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"piecewise_linear_function2d\n{\nname d2\n}\n"
			"sphere_geometry\n{\nname base\nradius 1\n}\n"
			"displaced_geometry\n{\nname disp\nbase_geometry base\ndisplacement d2\n}\n"
			"bumpmap_modifier\n{\nname bm\nfunction d2\n}\n"
			"composite_function2d_painter\n{\nname comp\nchild_a d2\nchild_b d2\n}\n" );
		ReferenceGraph g = BuildReferenceGraph( doc, 0 );
		const NodeId d2 = DocFindByName( doc, "piecewise_linear_function2d/d2" );
		const NodeId disp = DocFindByName( doc, "displaced_geometry/disp" );
		const NodeId bm   = DocFindByName( doc, "bumpmap_modifier/bm" );
		const NodeId comp = DocFindByName( doc, "composite_function2d_painter/comp" );
		bool hasDisp = false, hasBm = false, hasComp = false;
		for( NodeId n : DocEditClosure( d2, g ) ) { if( n == disp ) hasDisp = true; if( n == bm ) hasBm = true; if( n == comp ) hasComp = true; }
		Check( d2 && disp && hasDisp, "painter-decl-func2d: closure(Function2D d2) INCLUDES displaced_geometry.displacement (review #3 table)" );
		Check( bm && hasBm, "painter-decl-func2d: ...and bumpmap_modifier.function" );
		Check( comp && hasComp, "painter-decl-func2d: ...and composite_function2d_painter.child_a/child_b" );
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
