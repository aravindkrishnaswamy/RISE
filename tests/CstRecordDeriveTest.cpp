//////////////////////////////////////////////////////////////////////
//
//  CstRecordDeriveTest.cpp - D35 slice 1 (docs/agentic-redesign/21-stable-apply-
//  and-resolver.md §8): the RECORD-DURING-DERIVE graph + the recorded-vs-static
//  DRIFT CROSS-CHECK.
//
//  DeriveToJob(doc, job, diags, &recorded) builds a reference graph from the
//  engine's ACTUAL production (manager AddItem) + resolution (manager GetItem),
//  keyed by entity pointer -- no heuristic, so it cannot drift from the engine.
//  This suite asserts the recorded (chunk-level) dependents EQUAL the static
//  BuildReferenceGraph dependents on clean canonical scenes (both directions: a
//  static-OVER edge -- a heuristic edge the engine never resolves -- AND a static-MISS
//  -- an engine edge the heuristic lacks -- both break the `==`). It also confirms
//  closure over the recorded graph equals closure over the static graph. A future
//  manager-choice drift in either direction fails the cross-check. Media are included
//  (slice 2 records them via the mediaMap hook, despite the GenericManager bypass).
//
//  Scenes are deliberately CONFLATION-FREE: the static graph's conservative painter
//  ALIAS edges (a same-named colour+scalar pair) are static-only by design (the
//  engine does not alias), so they would break the `==` -- out of scope for the
//  cross-check, which validates the manager-choice convergence. Duplicate-name MEDIA are
//  likewise excluded: Job::mediaMap is last-wins (it silently REPLACES) while the static
//  graph is first-wins, so a dup-name medium edge would point at different producers in
//  recorded vs static -- the same out-of-scope class as conflations.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Cst/Cst.h"
#include "CstRenderEquivalence.h"      // Job

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace RISE;
using namespace RISE::Cst;
using namespace risequiv;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

// Flatten dependents into a set of (producer, consumer) pairs (dedup + order-independent).
static std::set<std::pair<NodeId,NodeId> > DepSet( const ReferenceGraph& g )
{
	std::set<std::pair<NodeId,NodeId> > s;
	for( std::map<NodeId, std::vector<NodeId> >::const_iterator it = g.dependents.begin(); it != g.dependents.end(); ++it )
		for( NodeId c : it->second ) s.insert( std::make_pair( it->first, c ) );
	return s;
}

// Derive `scene` WITH recording + build the static graph; return both dep-sets.
// `clean` = the derive applied every chunk with no diagnostic -- the recorded graph only
// equals the static one for a FULLY-applied scene (a chunk that fails Finalize records the
// resolutions it fired before aborting, a partial set), so the `==` assertions below require
// it to rule out a partial-apply false (mis)match.
struct Pair { std::set<std::pair<NodeId,NodeId> > rec, stat; int applied; bool clean; };
static Pair Run( const std::string& scene )
{
	Document doc = ParseToCst( scene );
	Job* j = new Job(); std::vector<std::string> d; ReferenceGraph recorded;
	int applied = DeriveToJob( doc, *j, &d, &recorded );
	ReferenceGraph stat = BuildReferenceGraph( doc );
	Pair r{ DepSet( recorded ), DepSet( stat ), applied, d.empty() };
	j->release();
	return r;
}

int main()
{
	std::printf( "CstRecordDeriveTest -- D35 slice 1 (record-during-derive + drift cross-check)\n" );

	// [equal] the simplest clean scene: recorded dependents == static dependents (BOTH
	// directions). ==-eligible because every Finalize lookup here is a STORED reference (no
	// incidental GetItem-hit, no default-name hit, no name reuse) and there is no conflation
	// painter-alias edge -- so recorded carries no spurious edge over static.
	{
		Pair r = Run(
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname m\nreflectance p\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n" );
		Check( r.applied > 0 && r.clean, "equal: scene fully + cleanly derived" );
		Check( !r.stat.empty() && !r.rec.empty(), "equal: both graphs non-empty" );
		Check( r.rec == r.stat, "equal: recorded dependents == static dependents (engine == heuristic, simplest scene)" );
	}

	// [subset] static dependents subset-of recorded on each clean scene: every heuristic
	// edge is a REAL engine resolution (a manager-choice drift would put a static edge the
	// engine never resolves -> not in recorded -> fails).
	{
		const char* scenes[] = {
			// displacement: {Painter}-declared, engine binds Function2D -> the recently-fixed case
			"RISE ASCII SCENE 6\n"
			"piecewise_linear_function2d\n{\nname d2\n}\n"
			"sphere_geometry\n{\nname base\nradius 1\n}\n"
			"displaced_geometry\n{\nname disp\nbase_geometry base\ndisplacement d2\n}\n",
			// glass: refractance is a colour painter, ior 1.5 is a numeric literal (no edge)
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 1 1 1\n}\n"
			"perfectrefractor_material\n{\nname glass\nrefractance p\nior 1.5\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial glass\n}\n",
			// (transfer_* {Painter,Function}->Function1D is cross-checked in CstResolverTest;
			//  its only host -- directvolumerendering_shader -- needs a volume grid file to
			//  derive, which is out of scope for this kernel cross-check.)
			// media: interior_medium resolves via the Job's mediaMap (which bypasses the
			// GenericManager chokepoint) -- slice 2 hooks Add*Medium/SetObjectInteriorMedium so
			// the (medium -> object) edge IS recorded; this scene would have failed `subset`
			// before that hook (see 21-*.md §8).
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname m\nreflectance p\n}\n"
			"homogeneous_medium\n{\nname med\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial m\ninterior_medium med\n}\n",
			// two objects sharing one geometry + two materials sharing one painter
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname m1\nreflectance p\n}\n"
			"lambertian_material\n{\nname m2\nreflectance p\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname oa\ngeometry g\nmaterial m1\n}\n"
			"standard_object\n{\nname ob\ngeometry g\nmaterial m2\n}\n",
			// scalar-painter chain: exercises the SCALAR painter manager (a distinct
			// GenericManager from the colour one) -- scalar_painter.base resolves via
			// GetScalarPainters, so a non-conflated `s` records the same edge static computes.
			"RISE ASCII SCENE 6\n"
			"scalar_painter\n{\nname base_s\nvalue 0.3\n}\n"
			"scalar_painter\n{\nname sp\nbase base_s\n}\n",
			// function1d: scalar_painter.function1d resolves via the Function-1D manager
			// (dimension-precise, distinct from Function-2D).
			"RISE ASCII SCENE 6\n"
			"piecewise_linear_function\n{\nname f1\ncp 0 0\ncp 1 1\n}\n"
			"scalar_painter\n{\nname sp1\nfunction1d f1\n}\n",
			// modifier: exercises the MODIFIER manager + the {Painter}-declared-but-Function2D
			// `bumpmap_modifier.function` slot (the modifier twin of displaced_geometry).
			"RISE ASCII SCENE 6\n"
			"piecewise_linear_function2d\n{\nname d2\n}\n"
			"bumpmap_modifier\n{\nname bm\nfunction d2\n}\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname m\nreflectance p\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial m\nmodifier bm\n}\n",
		};
		for( const char* s : scenes ) {
			Pair r = Run( s );
			Check( r.applied > 0 && r.clean && !r.stat.empty(), "subset: scene FULLY + cleanly derived with edges" );
			// Strengthened to full EQUALITY: catches static-OVER (a heuristic edge the engine
			// never resolves) AND static-MISS (an engine edge the heuristic lacks). Holds on
			// these clean scenes because the recorder produces no spurious edge over the static
			// reference set (every Finalize lookup is a stored reference).
			Check( r.rec == r.stat, "equal: recorded dependents == static dependents (engine == heuristic, both directions)" );
		}
	}

	// [closure] closure over the RECORDED graph == closure over the static graph -- the
	// recorded graph is a drop-in, sound closure source (slice-2 readiness: a live consumer
	// holding (Job, recorded) can drive DocEditClosure off the engine truth; rename stays
	// on the static/param-level path by design).
	{
		Document doc = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname m1\nreflectance p\n}\n"
			"lambertian_material\n{\nname m2\nreflectance p\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial m1\n}\n" );
		Job* j = new Job(); std::vector<std::string> d; ReferenceGraph recorded;
		DeriveToJob( doc, *j, &d, &recorded );
		ReferenceGraph stat = BuildReferenceGraph( doc );
		const NodeId p = DocFindByName( doc, "uniformcolor_painter/p" );
		std::vector<NodeId> cRec = DocEditClosure( p, recorded );
		std::vector<NodeId> cStat = DocEditClosure( p, stat );
		std::set<NodeId> sRec( cRec.begin(), cRec.end() ), sStat( cStat.begin(), cStat.end() );
		Check( p && !sRec.empty() && sRec == sStat, "closure: DocEditClosure(p, recorded) == DocEditClosure(p, static) -- recorded graph is a sound closure source" );
		j->release();
	}

	// [reuse] (review P1) DeriveToJob RESETS outRecorded at entry, so deriving doc B into a
	// graph that already holds doc A's edges yields ONLY B's edges -- no stale A leakage. (The
	// recorder only APPENDS; without the entry reset a reused/replaced graph would mix A + B.)
	{
		Document docA = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname m\nreflectance p\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n" );
		Document docB = ParseToCst(   // unrelated
			"RISE ASCII SCENE 6\n"
			"scalar_painter\n{\nname base_s\nvalue 0.3\n}\n"
			"scalar_painter\n{\nname sp\nbase base_s\n}\n" );
		ReferenceGraph g;
		Job* jA = new Job(); std::vector<std::string> dA; DeriveToJob( docA, *jA, &dA, &g ); jA->release();
		const bool aRecorded = !DepSet( g ).empty();
		Job* jB = new Job(); std::vector<std::string> dB; DeriveToJob( docB, *jB, &dB, &g ); jB->release();   // REUSE g
		const std::set<std::pair<NodeId,NodeId> > afterB = DepSet( g );
		const std::set<std::pair<NodeId,NodeId> > staticB = DepSet( BuildReferenceGraph( docB ) );
		Check( aRecorded, "reuse: precondition -- doc A recorded edges into g" );
		Check( !afterB.empty() && afterB == staticB, "reuse: reused graph holds ONLY doc B's edges (A reset at entry, no leak)" );
	}

	// [reset-on-failure] (review P1) a derive that FAILS validation resets outRecorded to EMPTY
	// rather than leaving the caller's prior (stale) graph untouched.
	{
		Document docA = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname m\nreflectance p\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n" );
		ReferenceGraph g;
		Job* jA = new Job(); std::vector<std::string> dA; DeriveToJob( docA, *jA, &dA, &g ); jA->release();
		Check( !DepSet( g ).empty(), "reset-on-failure: precondition -- A recorded edges" );
		Document docBad = ParseToCst( "RISE ASCII SCENE 6\nsphere_geometry\n{\nname g\nradius xyz\n}\n" );   // non-numeric radius -> PASS-1 validation failure
		Job* jBad = new Job(); std::vector<std::string> dBad;
		int applied = DeriveToJob( docBad, *jBad, &dBad, &g ); jBad->release();   // REUSE g; fails validation
		Check( applied == 0 && !dBad.empty(), "reset-on-failure: malformed doc refused" );
		Check( DepSet( g ).empty(), "reset-on-failure: outRecorded reset to EMPTY after a failed derive (no stale A)" );
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
