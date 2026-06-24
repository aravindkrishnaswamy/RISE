//////////////////////////////////////////////////////////////////////
//
//  CstRecordDeriveTest.cpp - D35 slice 1 (docs/agentic-redesign/21-stable-apply-
//  and-resolver.md §8): the RECORD-DURING-DERIVE graph + the recorded-vs-static
//  DRIFT CROSS-CHECK.
//
//  DeriveToJob(doc, job, diags, &recorded) builds a reference graph from the
//  engine's ACTUAL production (manager AddItem) + resolution (manager GetItem),
//  keyed by entity pointer -- no heuristic, so it cannot drift from the engine.
//  This suite asserts the recorded (chunk-level) dependents AGREE with the static
//  BuildReferenceGraph on clean canonical scenes: static's heuristic edges are all
//  real engine resolutions (static dependents subset-of recorded), and on the
//  simplest scene the two are EQUAL. A future manager-choice drift (a static edge
//  the engine does not actually resolve, or vice versa) breaks the subset check.
//
//  Scenes are deliberately CONFLATION-FREE: the static graph's conservative painter
//  ALIAS edges (a same-named colour+scalar pair) are static-only by design (the
//  engine does not alias), so they would break the subset direction -- out of scope
//  for the cross-check, which validates the manager-choice convergence.
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
struct Pair { std::set<std::pair<NodeId,NodeId> > rec, stat; int applied; };
static Pair Run( const std::string& scene )
{
	Document doc = ParseToCst( scene );
	Job* j = new Job(); std::vector<std::string> d; ReferenceGraph recorded;
	int applied = DeriveToJob( doc, *j, &d, &recorded );
	ReferenceGraph stat = BuildReferenceGraph( doc );
	Pair r{ DepSet( recorded ), DepSet( stat ), applied };
	j->release();
	return r;
}

static bool Subset( const std::set<std::pair<NodeId,NodeId> >& a, const std::set<std::pair<NodeId,NodeId> >& b )
{
	for( const std::pair<NodeId,NodeId>& e : a ) if( !b.count( e ) ) return false;
	return true;
}

int main()
{
	std::printf( "CstRecordDeriveTest -- D35 slice 1 (record-during-derive + drift cross-check)\n" );

	// [equal] the simplest clean scene: recorded dependents == static dependents (BOTH
	// directions -- the only scene that also catches a static-MISS, since `subset` below only
	// catches static-OVER). ==-eligible because every Finalize lookup here is a STORED
	// reference (no incidental GetItem-hit, no default-name hit, no name reuse) and there is no
	// conflation painter-alias edge -- so recorded carries no spurious edge over static.
	{
		Pair r = Run(
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname m\nreflectance p\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n" );
		Check( r.applied > 0, "equal: scene derived" );
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
			// (MEDIA scenes are also excluded: interior_medium resolves via the Job's mediaMap,
			//  which BYPASSES the GenericManager chokepoint, so the recorder misses the edge and
			//  a medium scene would fail `subset`. Recording media is a slice-2 prerequisite --
			//  see 21-*.md §8.)
			// two objects sharing one geometry + two materials sharing one painter
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname m1\nreflectance p\n}\n"
			"lambertian_material\n{\nname m2\nreflectance p\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname oa\ngeometry g\nmaterial m1\n}\n"
			"standard_object\n{\nname ob\ngeometry g\nmaterial m2\n}\n",
		};
		for( const char* s : scenes ) {
			Pair r = Run( s );
			Check( r.applied > 0 && !r.stat.empty(), "subset: scene derived with edges" );
			Check( Subset( r.stat, r.rec ), "subset: static dependents subset-of recorded (heuristic edges are all real engine resolutions)" );
		}
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
