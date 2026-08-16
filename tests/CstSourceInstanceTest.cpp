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
//    [refuse]      geometry+source (BOTH spellings, `geometry none` included) / undeclared /
//                  forward-reference / self / CSG operand (in BOTH declaration orders) /
//                  source-with-children (3b) / `source S parent S` / duplicate document-level
//                  name -- each separately, each with its own TRUE reason.
//    [provenance]  the manager records (entry -> instancing chunk, source node); the source
//                  itself has none; an unknown name leaves the out-pointers untouched.
//    [visible]     the source still renders after being instanced.
//    [area]        an instanced emitter's GetArea() tracks the INSTANCE's transform, not the
//                  source's -- the one property with a documented bug history (2026-08-13).
//    [incremental] an edit to a `source` chunk refuses -> full-derive fallback; SETTING `source
//                  none` does too, so the stale provenance row is retired.
//
//  A NOTE ON WHAT `DumpJob` CAN SEE.  It prints geometry / material / modifier / shader /
//  radiance_map / interior_medium / visible / bbox and nothing else -- so `casts_shadows`,
//  `receives_shadows` and `GetArea()` are INVISIBLE to a dump-vs-dump compare (two scenes
//  differing only in a shadow flag produce byte-identical dumps).  Those are asserted on the
//  IObject directly, via DeriveJob() below.
//
//////////////////////////////////////////////////////////////////////

#include "CstRenderEquivalence.h"
#include "../src/Library/Cst/Cst.h"

#include <cmath>
#include <cstdlib>
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>			// getpid() -- per-process temp scene filenames
#endif

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

// Derive a scene into a Job the CALLER owns (and must release).  The route to everything
// DumpJob is blind to -- the shadow flags and GetArea() (see the header note).
static Job* DeriveJob( const std::string& scene, std::vector<std::string>* outDiags = nullptr )
{
	Job* j = new Job();
	Document d = ParseToCst( scene );
	std::vector<std::string> diags;
	DeriveToJob( d, *j, &diags );
	if( outDiags ) *outDiags = diags;
	return j;
}

static IObject* Obj( Job* j, const char* name )
{
	return ( j && j->GetObjects() ) ? j->GetObjects()->GetItem( name ) : 0;
}

// Job::ApplyCstParamEdit / ...Checked read the RETAINED CST head, which only
// LoadAsciiSceneViaCst installs -- and that takes a FILENAME.  Per-process name so two
// copies of this binary (a hand run alongside the suite, a repeat-run flake hunt) cannot
// clobber each other's fixture mid-load.
static std::string WriteTempScene( const char* name, const std::string& text )
{
	const char* base = std::getenv( "TMPDIR" );
	std::string dir = base ? base : "/tmp";
	if( !dir.empty() && dir.back() != '/' ) dir += '/';
	const std::string path = dir + std::to_string( (long)getpid() ) + "_" + name;
	std::ofstream f( path.c_str(), std::ios::binary | std::ios::trunc );
	f << text;
	f.close();
	return path;
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

	// [inherit] `modifier` comes across -- that one IS in the dump compare.
	{
		const std::string src = "standard_object\n{\nname S\ngeometry geo\nmaterial m\nmodifier bump\n}\n";
		const std::string got  = DumpCst( Scene( src + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ) );
		const std::string want = DumpCst( Scene( src + "standard_object\n{\nname I\ngeometry geo\nmaterial m\nmodifier bump\nposition 5 0 0\n}\n" ) );
		const std::string bare = DumpCst( Scene( src + "standard_object\n{\nname I\ngeometry geo\nmaterial m\nposition 5 0 0\n}\n" ) );
		Check( got == want, "inherit: `modifier` comes across with the rest" );
		Check( got != bare, "inherit: ... and the modifier compare is not vacuous (an instance without it dumps differently)" );
	}

	// [inherit] the SHADOW FLAGS, asserted on the IObject.  A dump-vs-dump compare cannot pin
	// these: DumpJob never prints them, so two scenes differing ONLY in `casts_shadows` produce
	// byte-identical dumps and the compare would pass with the flags dropped entirely.
	{
		const std::string src = "standard_object\n{\nname S\ngeometry geo\nmaterial m\ncasts_shadows FALSE\nreceives_shadows FALSE\n}\n";
		Job* j = DeriveJob( Scene( src + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ) );
		IObject* S = Obj( j, "S" ); IObject* I = Obj( j, "I" );
		Check( S && !S->DoesCastShadows() && !S->DoesReceiveShadows(),
		       "inherit: (control) the SOURCE really carries both shadow flags OFF" );
		Check( I && !I->DoesCastShadows(),    "inherit: `casts_shadows FALSE` is inherited by the instance" );
		Check( I && !I->DoesReceiveShadows(), "inherit: `receives_shadows FALSE` is inherited by the instance" );
		j->release();

		// The control that makes those two non-vacuous: both flags DEFAULT to TRUE, so an
		// expansion that dropped them would read `true` here, not `false`.
		Job* j2 = DeriveJob( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ) );
		IObject* I2 = Obj( j2, "I" );
		Check( I2 && I2->DoesCastShadows() && I2->DoesReceiveShadows(),
		       "inherit: ... and an instance of a DEFAULT-flagged source reads TRUE/TRUE (the pins above are not vacuous)" );
		j2->release();

		// [override] and an explicit flag on the instancing chunk beats the inherited one,
		// per-parameter -- the other flag stays inherited.
		Job* j3 = DeriveJob( Scene( src + "standard_object\n{\nname I\nsource S\ncasts_shadows TRUE\nposition 5 0 0\n}\n" ) );
		IObject* I3 = Obj( j3, "I" );
		Check( I3 && I3->DoesCastShadows() && !I3->DoesReceiveShadows(),
		       "override: `casts_shadows TRUE` on the instance beats the inherited FALSE, and `receives_shadows` stays inherited" );
		j3->release();
	}

	// [inherit] the remaining reference slots -- `shader`, `radiance_map` (+ its scale) and
	// `interior_medium`.  These ARE printed by DumpJob, so the pin is one scene that binds all
	// three on the source and nothing on the instance, against a bare instance that proves the
	// compare would notice their absence.
	{
		const std::string prelude = "pathtracing_shaderop\n{\nname pop\n}\n"
		                            "standard_shader\n{\nname sh\nshaderop pop\n}\n"
		                            "homogeneous_medium\n{\nname med\nabsorption 0.1 0.1 0.1\nscattering 0.2 0.2 0.2\n}\n";
		const std::string src = prelude + "standard_object\n{\nname S\ngeometry geo\nmaterial m\nshader sh\n"
		                                  "radiance_map p2\nradiance_scale 2.5\ninterior_medium med\n}\n";
		std::vector<std::string> diags;
		const std::string got  = DumpCst( Scene( src + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ), &diags );
		const std::string want = DumpCst( Scene( src + "standard_object\n{\nname I\ngeometry geo\nmaterial m\nshader sh\n"
		                                               "radiance_map p2\nradiance_scale 2.5\ninterior_medium med\nposition 5 0 0\n}\n" ) );
		const std::string bare = DumpCst( Scene( src + "standard_object\n{\nname I\ngeometry geo\nmaterial m\nposition 5 0 0\n}\n" ) );
		Check( diags.empty() && got == want, "inherit: `shader`, `radiance_map` (+ `radiance_scale`) and `interior_medium` come across" );
		Check( got != bare, "inherit: ... and the compare is not vacuous (a bare instance dumps differently)" );
	}

	// [area] an instanced EMITTER's GetArea().  This is what LuminaryManager samples, it is
	// derived from the object's COMPOSED transform (Object::GetArea folds in |det|^(2/3) -- the
	// 2026-08-13 fix), and DumpJob does not print it, so nothing else here would catch an
	// instance whose area came from the SOURCE's transform.  Unit sphere: 4*pi = 12.566.
	{
		const double kUnitSphereArea = 4.0 * 3.14159265358979323846;
		const std::string emis = "lambertian_luminaire_material\n{\nname lum\nexitance p\n}\n";
		const std::string src  = emis + "standard_object\n{\nname S\ngeometry geo\nmaterial lum\nposition 2 0 0\n}\n";
		std::vector<std::string> diags;
		Job* j = DeriveJob( Scene( src + "standard_object\n{\nname I\nsource S\nscale 2 2 2\n}\n" ), &diags );
		IObject* S = Obj( j, "S" ); IObject* I = Obj( j, "I" );
		Check( diags.empty() && S && std::fabs( (double)S->GetArea() - kUnitSphereArea ) < 1e-6,
		       "area: the source unit-sphere emitter has area 4*pi (12.566)" );
		Check( I && std::fabs( (double)I->GetArea() - 4.0 * kUnitSphereArea ) < 1e-6,
		       "area: the INSTANCE's own `scale 2 2 2` scales its area by |det|^(2/3) = 4 -> 50.265" );
		j->release();

		// The mirror: the SOURCE's scale is not inherited, so a bare instance is back at 4*pi
		// while the source itself really is scaled -- the control that makes 12.566 a result
		// rather than a coincidence.
		const std::string src3 = emis + "standard_object\n{\nname S\ngeometry geo\nmaterial lum\nscale 3 3 3\n}\n";
		Job* j2 = DeriveJob( Scene( src3 + "standard_object\n{\nname I\nsource S\n}\n" ) );
		IObject* S3 = Obj( j2, "S" ); IObject* I3 = Obj( j2, "I" );
		Check( S3 && std::fabs( (double)S3->GetArea() - 9.0 * kUnitSphereArea ) < 1e-5,
		       "area: (control) the source's OWN `scale 3 3 3` really does scale its area by 9" );
		Check( I3 && std::fabs( (double)I3->GetArea() - kUnitSphereArea ) < 1e-6,
		       "area: a bare instance of a scaled source is UNSCALED (4*pi) -- transforms are the instance's own" );
		j2->release();
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

	// [refuse] `geometry none` + `source` -- the SAME rule, the other spelling.  `none` is the
	// container spelling and is zero forms on its own, but on an instancing chunk it is NOT
	// inert: the merge drops only `source` from the instancing chunk's own params, so a
	// `geometry none` written there OVERRIDES the geometry inherited from the source and the
	// "copy" derives as an empty, invisible container.  A form count that reads `none` as zero
	// forms lets that through with no diagnostic at all -- while a REAL geometry on the same
	// chunk is refused.  Both spellings must reach the same answer.
	{
		const std::string body = SRC_LEAF + "standard_object\n{\nname I\nsource S\ngeometry none\n}\n";
		std::string all;
		Check( RefusedWith( Scene( body ), "mutually exclusive", &all ),
		       "refuse: `geometry none` + `source` is refused exactly as a REAL geometry is" );
		Check( all.find( "not inert" ) != std::string::npos,
		       "refuse: ... and the message says why `none` is not a way to spell 'leave the geometry alone' here" );
		std::vector<std::string> diags;
		const std::string dump = DumpCst( Scene( body ), &diags );
		Check( dump.find( "  I " ) == std::string::npos,
		       "refuse: ... and no `I` is created -- the state this exists to prevent is a silent geometry-less container" );
	}

	// [refuse] ... and the same refusal is reachable through the LIVE EDIT path, which is where
	// it matters.  Writing `geometry none` onto an instancing chunk through the agent-gated edit
	// used to COMMIT and leave a chunk carrying BOTH forms -- precisely the state exclusivity
	// exists to make unreachable.
	{
		const std::string scene = Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" );
		const std::string path  = WriteTempScene( "cst_source_instance_geomnone.RISEscene", scene );
		Job* j = new Job();
		const bool loaded = j->LoadAsciiSceneViaCst( path.c_str() );
		Check( loaded, "live-edit: the `geometry none` fixture loads with a retained CST head" );
		if( loaded ) {
			const int rc = j->ApplyCstParamEditChecked( "I", "object", "geometry", 0, "none" );
			Check( rc == 0, "live-edit: `geometry none` onto an instancing chunk is REFUSED (rc=0), head and live scene untouched" );
			IObject* I = Obj( j, "I" );
			Check( I && I->GetGeometry() != 0, "live-edit: ... and `I` still has the geometry it inherited" );
			Check( j->GetCstDocument() && SerializeCst( *j->GetCstDocument() ).find( "geometry none" ) == std::string::npos,
			       "live-edit: ... and the retained head never grew a second form" );
		}
		j->release();
		std::remove( path.c_str() );
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

	// [refuse] a CSG OPERAND -- in BOTH declaration orders.  An operand is CONSUMED by its
	// composite (CSGObject::AssignObjects takes ownership and hides it), so it is a term in a
	// boolean expression, not a shape that stands on its own -- the same rule
	// ObjectManager::SetObjectParent applies.  That is a DOCUMENT property, so the answer must
	// not depend on where the `csg_object` chunk sits: a live `IsWorldVisible()` test only sees
	// the operand as hidden once the composite's Finalize has run, which would refuse the
	// composite-first spelling and wave the composite-last one through.
	{
		const std::string ab   = "standard_object\n{\nname a\ngeometry geo\nmaterial m\n}\n"
		                         "standard_object\n{\nname b\ngeometry boxg\nmaterial m\n}\n";
		const std::string csg  = "csg_object\n{\nname C\nobja a\nobjb b\noperation union\n}\n";
		const std::string inst = "standard_object\n{\nname I\nsource a\n}\n";
		std::string before, after;
		Check( RefusedWith( Scene( ab + csg + inst ), "is a CSG OPERAND", &before ),
		       "refuse: `source` naming a CSG operand -- composite declared BEFORE the instancing chunk" );
		Check( RefusedWith( Scene( ab + inst + csg ), "is a CSG OPERAND", &after ),
		       "refuse: `source` naming a CSG operand -- composite declared AFTER it too (the rule is order-INDEPENDENT)" );
		Check( before.find( "CONSUMES" ) != std::string::npos && before.find( "Instance the `csg_object` itself" ) != std::string::npos,
		       "refuse: ... and the reason given is the TRUE one (the composite consumes the operand) with the way out" );
		Check( before.find( "would not land where the number says" ) == std::string::npos,
		       "refuse: ... and NOT the false CSG-local-matrix reason -- the collapse semantics drop the source's matrix entirely" );
		// `objb` is scanned as well as `obja`, so the rule covers both slots.
		Check( RefusedWith( Scene( ab + csg + "standard_object\n{\nname I\nsource b\n}\n" ), "is a CSG OPERAND" ),
		       "refuse: ... and the `objb` slot is scanned too, not just `obja`" );
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

	// [refuse] `source S` + `parent S` on ONE chunk.  This makes S appear to HAVE CHILDREN --
	// the instance is its own source's only child -- so the 3b rule fires on a scene with no
	// subtree in it at all, and an author sent to look for S's children finds none.  The real
	// cause is the author's own `parent` line, and the message must say that.
	{
		std::string all;
		Check( RefusedWith( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\nparent S\n}\n" ),
		                    "recursive definition", &all ),
		       "refuse: `source S parent S` is refused for the REAL reason -- a copy of S parented under S" );
		Check( all.find( "step 3b" ) == std::string::npos || all.find( "`parent S`" ) != std::string::npos,
		       "refuse: ... naming the `parent` line as the cause, not sending the author after a subtree that does not exist" );
		Check( all.find( "has CHILDREN -- instancing a multi-node subtree" ) == std::string::npos,
		       "refuse: ... and NOT with the has-children message" );
		// A source with a REAL other child still gets the 3b message, even when the instance
		// also parents to it -- the self-child detection must be exactly "the ONLY child is me".
		Check( RefusedWith( Scene( SRC_LEAF
		                         + "standard_object\n{\nname kid\ngeometry boxg\nmaterial m\nparent S\n}\n"
		                         + "standard_object\n{\nname I\nsource S\nparent S\n}\n" ),
		                    "has CHILDREN -- instancing a multi-node subtree" ),
		       "refuse: ... while a source with a genuine OTHER child still gets the 3b message" );
	}

	// [refuse] DOCUMENT-level name collision: two object chunks declaring the entry name.  The
	// manager pre-check alone cannot see this when the other chunk is declared AFTER -- it does
	// not exist yet -- so the scan is over the document, and it names both chunks.
	{
		// COMMENTS ON PURPOSE.  The message exists to name BOTH chunks so the author can
		// reconcile them, which means naming them in terms an author can COUNT TO.  A raw CST
		// item index is not one: trivia (comments, blank lines) are items too, so in this scene
		// -- whose colliding chunks are the 9th and 10th the author wrote -- the raw indices are
		// nowhere near 9 and 10.  Without the comments the two numberings would coincide and
		// this test would pass on the broken message.
		const std::string body = SRC_LEAF
		                       + "# a comment, so the raw item index and the chunk ordinal diverge\n"
		                       + "\n"
		                       + "# and another\n"
		                       + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n"
		                       + "\n# a third, between the two colliding chunks\n\n"
		                       + "standard_object\n{\nname I\ngeometry boxg\nmaterial m\n}\n";
		std::string all;
		Check( RefusedWith( Scene( body ), "declared by MORE THAN ONE object chunk", &all ),
		       "refuse: the entry name is also declared by a LATER authored chunk (document-level mis-targeting)" );
		Check( all.find( "chunk #9" ) != std::string::npos && all.find( "chunk #10" ) != std::string::npos,
		       "refuse: ... naming both by their position among the file's CHUNKS (#9 and #10 here, comments not counted)" );
		Check( all.find( "a `standard_object`" ) != std::string::npos,
		       "refuse: ... and by role, so the author knows what to look for" );
		Check( all.find( "item " ) == std::string::npos,
		       "refuse: ... and never by raw CST item index (which counts comments and blank lines)" );
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
		// WHAT THIS DOES AND DOES NOT CLAIM.  The parser's SPECIFIC text reaches the LOG, not
		// `diags`: ExpandInstanceArray runs outside PASS-2's armed g_cstFinalizeDiagSink window,
		// so all that comes back here is its own generic apply-failed line.  So this pins the
		// BEHAVIOUR -- a `source` that reaches the parser is refused and builds nothing -- and
		// deliberately does NOT claim which of the parser's two `source` gates (the
		// geometry+source exclusivity one, or the lone-unexpanded-`source` backstop) fired.
		// Distinguishing them from here is not possible without reading the log file.
		Check( !diags.empty(), "refuse: a `source` reaching the PARSER (via instance_array pass-through) is refused" );
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
		// An unknown name: FALSE, and -- the part of the contract worth a test -- BOTH
		// out-pointers left UNTOUCHED, so a caller that reads them before checking the bool
		// gets its own initializer back rather than a stale interior pointer.
		static const char kSentinel[] = "untouched";
		const char* i3 = kSentinel; const char* s3 = kSentinel;
		Check( objs && !objs->GetObjectProvenance( "nosuch", &i3, &s3 ) && i3 == kSentinel && s3 == kSentinel,
		       "provenance: an unknown name answers FALSE and leaves BOTH out-pointers untouched" );
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

	// [incremental] CLEARING the slot -- `source none` -- refuses too.  It is tempting to exempt
	// it: the chunk then derives as a plain container, which the in-place re-point handles
	// perfectly well.  But the manager entry still carries the PROVENANCE row the earlier
	// expansion wrote, and provenance is retired only by RemoveItem / Shutdown -- neither of
	// which an in-place re-point calls.  An incremental commit would leave GetObjectProvenance
	// answering "(I, S)" for an object that is no longer an instance of anything.
	{
		Job* j = new Job();
		Document d = ParseToCst( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ) );
		std::vector<std::string> diags;
		DeriveToJob( d, *j, &diags );
		const NodeId id = DocFindByName( d, "standard_object/I" );
		Document d2 = DocSetParamValue( d, id, "source", 0, "none" );
		std::vector<std::string> di;
		const int applied = DeriveToJobIncremental( d2, *j, std::vector<NodeId>( 1, id ), &di );
		std::string all;
		for( std::size_t i = 0; i < di.size(); ++i ) { all += di[i]; all += "\n"; }
		Check( applied == 0 && all.find( "carries `source none`" ) != std::string::npos,
		       "incremental: `source none` refuses too -- ANY `source` edit takes the full re-derive" );
		j->release();
	}

	// [incremental] ... and the consequence, through the LIVE edit path: the full re-derive is
	// what actually retires the stale row.
	{
		const std::string scene = Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" );
		const std::string path  = WriteTempScene( "cst_source_instance_clear.RISEscene", scene );
		Job* j = new Job();
		const bool loaded = j->LoadAsciiSceneViaCst( path.c_str() );
		Check( loaded, "incremental-clear: the fixture loads with a retained CST head" );
		if( loaded ) {
			const char* c0 = 0;
			Check( j->GetObjects() && j->GetObjects()->GetObjectProvenance( "I", &c0, 0 ),
			       "incremental-clear: (precondition) `I` starts with a provenance row" );
			const int rc = j->ApplyCstParamEdit( "I", "object", "source", 0, "none" );
			Check( rc == 2, "incremental-clear: `source none` takes the FULL re-derive (rc=2), not the incremental fast path (rc=1)" );
			const char* c1 = 0;
			Check( j->GetObjects() && !j->GetObjects()->GetObjectProvenance( "I", &c1, 0 ),
			       "incremental-clear: ... so `I`'s provenance row is GONE -- it is not an instance of anything any more" );
			IObject* I = Obj( j, "I" );
			Check( I && I->GetGeometry() == 0, "incremental-clear: ... and `I` really did become a bare container" );
		}
		j->release();
		std::remove( path.c_str() );
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
