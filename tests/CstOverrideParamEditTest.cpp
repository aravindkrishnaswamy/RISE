//////////////////////////////////////////////////////////////////////
//
//  CstOverrideParamEditTest.cpp - Job::ApplyCstParamEdit vs `override_object`
//
//  THE BUG THIS FILE EXISTS FOR.  A `override_object` chunk is applied AFTER the chunk that
//  created the object and REPLACES the transform fields it names.  Job::ApplyCstParamEditImpl_
//  -- the route a TYPED param edit takes (properties panel, agent `set_param`) -- resolved the
//  BASE chunk and wrote there, while the two whole-pose routes beside it
//  (ApplyCstObjectMatrixEdit, ApplyCstObjectComponentsEdit) walk to the override first.  So a
//  typed `position` on an object carrying a same-named override landed on a chunk that no
//  longer decides anything: the full re-derive re-applied the override straight over it, the
//  call returned 2 (SUCCESS), nothing was logged, and the object did not move.
//
//  WHY EVERY ASSERTION HERE READS THE OBJECT, NOT THE RETURN CODE.  The return code is 2 with
//  the bug and 2 without it -- that is precisely what kept the defect invisible.  An `rc >= 1`
//  assertion is green in both worlds and pins nothing.  Each case below therefore reads the
//  object's world bounding box after the edit and asserts where it actually IS.  (The rc is
//  still asserted, explicitly labelled as the assertion that CANNOT catch this, so a future
//  reader does not mistake it for the guard.)
//
//  SCOPE PINS.  Beyond the walk itself, the cases fix the two boundaries a later change is most
//  likely to blur:
//    * a `scale` on the override is LIVE (its per-field branch applies position, orientation and
//      scale INDEPENDENTLY), so a typed `position` must not silently un-scale the object.  This is
//      deliberately the OPPOSITE of what ApplyCstObjectComponentsEdit does: that route strips
//      `scale` for reasons that belong to ITS caller (inputs come from DecomposeRigid, which admits
//      only unit columns and has already folded any sign flip into the orientation it writes), and
//      a typed single-param edit has neither property;
//    * the rule is scoped to TRANSFORM params on OBJECT-category chunks.  A `position` on an
//      omni_light, and a non-transform param on an overridden object, must both be untouched --
//      an override_object has no `material` param, so a walk that fired there would write a line
//      the derive rejects.
//
//  `override_object` is a legacy chunk: nothing in the editor or the agent surface emits one,
//  and a single scene in the tree (scenes/Tests/ChunkCoverage/cc_override_object.RISEscene)
//  contains one.  This is correctness debt on a rarely-walked path, which is exactly the kind
//  that stays broken -- hence a test that reads the geometry.
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

// Geometry + material the bodies below reference.  `m2` exists only so the non-transform
// control has somewhere to re-point.
static std::string Scene( const std::string& body )
{
	return HDR
		+ "sphere_geometry\n{\nname geo\nradius 1\n}\n"
		// Deliberately NOT cube-shaped: a rotation has to be visible in the world bounding box,
		// and a symmetric shape hides one.  Long on z, short on x.
		+ "box_geometry\n{\nname boxg\nwidth 1\nheight 1\ndepth 4\n}\n"
		+ "uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
		+ "uniformcolor_painter\n{\nname p2\ncolor 0.25 0.25 0.25\n}\n"
		+ "lambertian_material\n{\nname m\nreflectance p\n}\n"
		+ "lambertian_material\n{\nname m2\nreflectance p2\n}\n"
		+ body;
}

// Job::ApplyCstParamEdit reads the RETAINED CST head, which only LoadAsciiSceneViaCst installs
// -- and that takes a FILENAME.  Per-process name so two copies of this binary cannot clobber
// each other's fixture mid-load.
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

static IObject* Obj( Job* j, const char* name )
{
	return ( j && j->GetObjects() ) ? j->GetObjects()->GetItem( name ) : 0;
}

// x of an object's world bounding-box centre.  IObject exposes IBasicTransform, not the composed
// matrix, so a TRANSLATION is read as a bbox shift.  A missing object answers a value no
// assertion below can accidentally satisfy.
static double CenterX( IObject* o )
{
	if( !o ) return -1.0e30;
	const BoundingBox bb = o->getBoundingBox();
	return ( bb.ll.x + bb.ur.x ) * 0.5;
}

// Width of an object's world bounding box -- the SCALE probe (a unit sphere is 2 wide, and 4
// once a `scale 2 2 2` is live).  Same missing-object sentinel discipline as CenterX.
static double Width( IObject* o )
{
	if( !o ) return -1.0e30;
	const BoundingBox bb = o->getBoundingBox();
	return bb.ur.x - bb.ll.x;
}

static bool Near( double a, double b ) { return std::fabs( a - b ) < 1e-6; }

// Load a fixture through the CST so the Job carries a retained head.  Caller releases.
static Job* LoadFixture( const char* name, const std::string& scene, bool* outLoaded )
{
	const std::string path = WriteTempScene( name, scene );
	Job* j = new Job();
	const bool loaded = j->LoadAsciiSceneViaCst( path.c_str() );
	if( outLoaded ) *outLoaded = loaded;
	std::remove( path.c_str() );
	return j;
}

int main()
{
	std::printf( "CstOverrideParamEditTest -- a typed transform edit vs a same-named override_object\n" );

	// ------------------------------------------------------------------ the regression itself
	// Base says x=0, the override says x=3, so the object sits at x=3 and the override is what
	// decides.  A typed `position 5 0 0` must put it at x=5.  With the write landing on the base
	// chunk it stays at x=3 -- while the call still answers 2.
	{
		bool loaded = false;
		Job* j = LoadFixture( "cst_override_param_position.RISEscene", Scene(
			  "standard_object\n{\nname X\ngeometry geo\nmaterial m\nposition 0 0 0\n}\n"
			  "override_object\n{\nname X\nposition 3 2 1\n}\n" ), &loaded );
		Check( loaded, "position: the base-plus-override fixture loads with a retained CST head" );
		if( loaded ) {
			Check( Near( CenterX( Obj( j, "X" ) ), 3.0 ),
			       "position: (precondition) the OVERRIDE decides the pose -- the object is at x=3, not the base's 0" );
			const int rc = j->ApplyCstParamEdit( "X", "standard_object", "position", 0, "5 0 0" );
			// This one is GREEN WITH THE BUG.  Kept, and labelled, because "the panel reported
			// success" is half the defect -- but it is not the guard.
			Check( rc >= 1, "position: the edit reports success (this assertion CANNOT catch the bug -- rc is 2 either way)" );
			// THE GUARD.  Red without the owner walk: the base chunk gets `position 5 0 0`, the
			// re-derive re-applies `position 3 2 1` on top, and the object never left x=3.
			Check( Near( CenterX( Obj( j, "X" ) ), 5.0 ),
			       "position: ... and the object really MOVED to x=5 -- the write reached the override_object, not the chunk under it" );
		}
		j->release();
	}

	// The agent-facing twin shares the impl, so it must take the same walk.  Pinned separately
	// because it reaches ApplyCstParamEditImpl_ through a different entry point (and with the
	// full-derivability gate on, which re-derives the WHOLE edited document first).
	{
		bool loaded = false;
		Job* j = LoadFixture( "cst_override_param_checked.RISEscene", Scene(
			  "standard_object\n{\nname X\ngeometry geo\nmaterial m\nposition 0 0 0\n}\n"
			  "override_object\n{\nname X\nposition 3 2 1\n}\n" ), &loaded );
		Check( loaded, "checked: the fixture loads" );
		if( loaded ) {
			Check( j->ApplyCstParamEditChecked( "X", "standard_object", "position", 0, "5 0 0" ) >= 1,
			       "checked: the gated agent edit reports success" );
			Check( Near( CenterX( Obj( j, "X" ) ), 5.0 ),
			       "checked: ... and the object really MOVED to x=5 -- the gate did not cost the walk" );
		}
		j->release();
	}

	// ------------------------------------------------------------------ the scale asymmetry
	// The override carries a `scale` ALONGSIDE its position; its per-field branch applies the two
	// INDEPENDENTLY, so the scale is live on the object being edited.  A typed `position` must
	// move it without un-scaling it.  Red if `scale` is ever added to the per-field strip list.
	{
		bool loaded = false;
		Job* j = LoadFixture( "cst_override_param_scale.RISEscene", Scene(
			  "standard_object\n{\nname X\ngeometry geo\nmaterial m\nposition 0 0 0\n}\n"
			  "override_object\n{\nname X\nposition 3 2 1\nscale 2 2 2\n}\n" ), &loaded );
		Check( loaded, "scale: the override-with-scale fixture loads" );
		if( loaded ) {
			Check( Near( Width( Obj( j, "X" ) ), 4.0 ),
			       "scale: (precondition) the override's `scale 2 2 2` is live -- the unit sphere is 4 wide" );
			Check( j->ApplyCstParamEdit( "X", "standard_object", "position", 0, "5 0 0" ) >= 1,
			       "scale: the position edit reports success" );
			Check( Near( CenterX( Obj( j, "X" ) ), 5.0 ),
			       "scale: ... the object moved to x=5" );
			Check( Near( Width( Obj( j, "X" ) ), 4.0 ),
			       "scale: ... and is STILL 4 wide -- a translate must not silently un-scale the object" );
		}
		j->release();
	}

	// ------------------------------------------------------------------ the masking strip
	// An override that carries a `matrix` takes the whole-matrix branch of its Finalize, which
	// ignores the per-field values outright -- so routing the write there is not enough on its
	// own: the masking param has to go, or the edit is a silent no-op ON the override instead of
	// a silent revert UNDER it.  Matrix below is a pure translate to x=3 (column-major).
	{
		bool loaded = false;
		Job* j = LoadFixture( "cst_override_param_matrix.RISEscene", Scene(
			  "standard_object\n{\nname X\ngeometry geo\nmaterial m\nposition 0 0 0\n}\n"
			  "override_object\n{\nname X\nmatrix 1 0 0 0 0 1 0 0 0 0 1 0 3 0 0 1\n}\n" ), &loaded );
		Check( loaded, "matrix-mask: the override-with-matrix fixture loads" );
		if( loaded ) {
			Check( Near( CenterX( Obj( j, "X" ) ), 3.0 ),
			       "matrix-mask: (precondition) the override's `matrix` decides the pose -- x=3" );
			Check( j->ApplyCstParamEdit( "X", "standard_object", "position", 0, "5 0 0" ) >= 1,
			       "matrix-mask: the position edit reports success" );
			Check( Near( CenterX( Obj( j, "X" ) ), 5.0 ),
			       "matrix-mask: ... and the object really MOVED to x=5 -- the outranking `matrix` was stripped from the owner" );
		}
		j->release();
	}

	// A `quaternion` outranks Euler but NOT position/scale, so the strip is per-role rather than one
	// list: a typed `orientation` must clear it (the typed Euler IS the replacement rotation), while
	// the position case above must not.  The override rotates the 1x1x4 box 90 degrees about Y, which
	// swings its long axis into x; clearing the quaternion swings it back.
	{
		bool loaded = false;
		Job* j = LoadFixture( "cst_override_param_orientation.RISEscene", Scene(
			  "standard_object\n{\nname X\ngeometry boxg\nmaterial m\n}\n"
			  "override_object\n{\nname X\nquaternion 0 0.70710678118654752 0 0.70710678118654752\n}\n" ), &loaded );
		Check( loaded, "orientation: the override-with-quaternion fixture loads" );
		if( loaded ) {
			Check( Near( Width( Obj( j, "X" ) ), 4.0 ),
			       "orientation: (precondition) the override's quaternion is live -- the long axis is on x, width 4" );
			Check( j->ApplyCstParamEdit( "X", "standard_object", "orientation", 0, "0 0 0" ) >= 1,
			       "orientation: the orientation edit reports success" );
			Check( Near( Width( Obj( j, "X" ) ), 1.0 ),
			       "orientation: ... and the box really UN-ROTATED to width 1 -- the outranking `quaternion` was stripped" );
		}
		j->release();
	}

	// ------------------------------------------------------------------ LAST override wins
	// Two same-named overrides: the SECOND is applied last and decides.  The walk must land on
	// that one, not on the first it happens to find.
	{
		bool loaded = false;
		Job* j = LoadFixture( "cst_override_param_last.RISEscene", Scene(
			  "standard_object\n{\nname X\ngeometry geo\nmaterial m\nposition 0 0 0\n}\n"
			  "override_object\n{\nname X\nposition 3 0 0\n}\n"
			  "override_object\n{\nname X\nposition 7 0 0\n}\n" ), &loaded );
		Check( loaded, "last-wins: the two-override fixture loads" );
		if( loaded ) {
			Check( Near( CenterX( Obj( j, "X" ) ), 7.0 ),
			       "last-wins: (precondition) the LAST override decides -- x=7" );
			Check( j->ApplyCstParamEdit( "X", "standard_object", "position", 0, "5 0 0" ) >= 1,
			       "last-wins: the position edit reports success" );
			Check( Near( CenterX( Obj( j, "X" ) ), 5.0 ),
			       "last-wins: ... and the object is at x=5 -- the write reached the LAST override, not the first" );
		}
		j->release();
	}

	// ------------------------------------------------------------------ controls: what must NOT change
	// No override in the document: the write stays on the base chunk and still moves the object.
	// The plain path the walk must not disturb.
	{
		bool loaded = false;
		Job* j = LoadFixture( "cst_override_param_plain.RISEscene", Scene(
			  "standard_object\n{\nname X\ngeometry geo\nmaterial m\nposition 0 0 0\n}\n" ), &loaded );
		Check( loaded, "control-plain: the no-override fixture loads" );
		if( loaded ) {
			Check( j->ApplyCstParamEdit( "X", "standard_object", "position", 0, "5 0 0" ) >= 1,
			       "control-plain: the position edit on an un-overridden object reports success" );
			Check( Near( CenterX( Obj( j, "X" ) ), 5.0 ),
			       "control-plain: ... and the object moved to x=5 (the base chunk still owns its own pose)" );
		}
		j->release();
	}

	// A NON-transform param on an overridden object must still land on the BASE chunk: an
	// override_object declares only a transform, so a walk that fired for `material` would write
	// a param the derive rejects.  Read through DumpJob, which prints each object's material.
	{
		bool loaded = false;
		Job* j = LoadFixture( "cst_override_param_binding.RISEscene", Scene(
			  "standard_object\n{\nname X\ngeometry geo\nmaterial m\nposition 0 0 0\n}\n"
			  "override_object\n{\nname X\nposition 3 2 1\n}\n" ), &loaded );
		Check( loaded, "control-binding: the fixture loads" );
		if( loaded ) {
			Check( j->ApplyCstParamEdit( "X", "standard_object", "material", 0, "m2" ) >= 1,
			       "control-binding: a `material` edit on an overridden object reports success" );
			const std::string dump = DumpJob( *j );
			Check( dump.find( "material=m2" ) != std::string::npos,
			       "control-binding: ... and it really re-bound -- a non-transform role is not walked to the override" );
			Check( Near( CenterX( Obj( j, "X" ) ), 3.0 ),
			       "control-binding: ... and the override still owns the pose (x=3, undisturbed)" );
		}
		j->release();
	}

	// `position` is an omni_light param too, and nothing in a light chunk outranks it -- so the rule
	// is scoped by the chunk's DESCRIPTOR CATEGORY, not by the role name alone.  This fixture is
	// built so that dropping the category gate MISBEHAVES VISIBLY rather than merely doing nothing:
	// the light shares its name with an overridden object, so a walk that fired on the light chunk
	// would write the LIGHT's position onto the OBJECT's override_object -- moving the object the
	// user never touched, and leaving the light where it was.  The object's own pose is therefore
	// the probe.  (Resolution stays unambiguous: the kind `light` narrows three same-named chunks
	// to the one omni_light.)
	{
		bool loaded = false;
		Job* j = LoadFixture( "cst_override_param_light.RISEscene", Scene(
			  "standard_object\n{\nname X\ngeometry geo\nmaterial m\nposition 0 0 0\n}\n"
			  "override_object\n{\nname X\nposition 3 2 1\n}\n"
			  "omni_light\n{\nname X\nposition 0 0 0\npower 100\ncolor 1 1 1\n}\n" ), &loaded );
		Check( loaded, "control-light: the same-named light-plus-overridden-object fixture loads" );
		if( loaded ) {
			Check( Near( CenterX( Obj( j, "X" ) ), 3.0 ),
			       "control-light: (precondition) the object sits at its override's x=3" );
			Check( j->ApplyCstParamEdit( "X", "light", "position", 0, "4 0 0" ) >= 1,
			       "control-light: a `position` edit on the omni_light still applies" );
			Check( Near( CenterX( Obj( j, "X" ) ), 3.0 ),
			       "control-light: ... and the OBJECT did not move -- a light's position is never walked to an override_object" );
		}
		j->release();
	}

	// ------------------------------------------------------------------ the lockstep partners
	// THE WALK IS A RULE, NOT A LINE OF CODE.  Three sites resolve the same edit -- the write, the
	// agent's prior-value capture, and the Undo of an inserted param -- and they must agree.  A write
	// that walks while the other two do not does not fix the silent failure, it moves it into the
	// history: Undo would remove a param from a chunk that never got one and report success, leaving
	// the transform it was meant to revert live on the object.  Hence Cst::DocTransformOwnerId, and
	// hence these two cases.
	//
	// The Undo twin, driven through Job::ApplyCstParamRemoveChecked (what SceneEditController's
	// SetAgentCstParam Undo arm calls).  Removing the override's `position` must drop the object back
	// to the BASE chunk's x=0 -- with the remove landing on the base instead, the override survives
	// untouched and the object never moves off x=3.
	{
		bool loaded = false;
		Job* j = LoadFixture( "cst_override_param_remove.RISEscene", Scene(
			  "standard_object\n{\nname X\ngeometry geo\nmaterial m\nposition 0 0 0\n}\n"
			  "override_object\n{\nname X\nposition 3 2 1\n}\n" ), &loaded );
		Check( loaded, "undo-twin: the fixture loads" );
		if( loaded ) {
			Check( Near( CenterX( Obj( j, "X" ) ), 3.0 ), "undo-twin: (precondition) the override holds the object at x=3" );
			Check( j->ApplyCstParamRemoveChecked( "X", "standard_object", "position", 0 ) >= 1,
			       "undo-twin: the param remove reports success" );
			Check( Near( CenterX( Obj( j, "X" ) ), 0.0 ),
			       "undo-twin: ... and the object really FELL BACK to the base chunk's x=0 -- the remove came off the override" );
		}
		j->release();
	}

	// The rule itself, read directly.  These pin the branches the whole-Job cases above cannot reach
	// cheaply -- in particular the capture side, which lives behind a private SceneEditController
	// method but resolves through exactly this function.
	{
		const std::string scene = Scene(
			  "standard_object\n{\nname X\ngeometry geo\nmaterial m\nposition 0 0 0\n}\n"
			  "override_object\n{\nname X\nposition 3 0 0\n}\n"
			  "override_object\n{\nname X\nposition 7 0 0\n}\n"
			  "omni_light\n{\nname L\nposition 0 0 0\npower 1\ncolor 1 1 1\n}\n" );
		const Document d = ParseToCst( scene );
		const NodeId base  = DocFindByNameAnyRole( d, "X", nullptr, "standard_object", false );
		const NodeId light = DocFindByNameAnyRole( d, "L", nullptr, "light", false );
		Check( base != 0 && light != 0, "rule: (precondition) the base object and the light both resolve" );

		bool isTransform = false;
		const NodeId owner = DocTransformOwnerId( d, base, "position", &isTransform );
		Check( isTransform, "rule: `position` on a standard_object IS an object-transform edit" );
		Check( owner != base, "rule: ... and it resolves to the override, not the base chunk" );
		const NodeRef ownerRef = DocResolveNodeId( d, owner );
		Check( ownerRef && ownerRef->role == "override_object", "rule: ... the owner is an override_object" );
		Check( ParamValueAsParsed( ownerRef, "position" ).find( "7" ) != std::string::npos,
		       "rule: ... and it is the LAST one (x=7), the one whose value the derive keeps" );

		// Addressed directly: answer itself.  Redirecting a caller that named a specific override to a
		// DIFFERENT override would be a fresh misdirection, not a fix.
		Check( DocTransformOwnerId( d, owner, "position" ) == owner,
		       "rule: an override_object addressed directly is never redirected" );

		// Non-transform role, and non-object chunk: unchanged, and NOT flagged as an object transform.
		bool bindingIsTransform = true;
		Check( DocTransformOwnerId( d, base, "material", &bindingIsTransform ) == base,
		       "rule: a non-transform role stays on the chunk the caller addressed" );
		Check( !bindingIsTransform, "rule: ... and is not reported as an object-transform edit" );

		bool lightIsTransform = true;
		Check( DocTransformOwnerId( d, light, "position", &lightIsTransform ) == light,
		       "rule: a light's `position` stays on the light -- nothing in a light chunk outranks it" );
		Check( !lightIsTransform, "rule: ... and is not reported as an object-transform edit" );
	}

	// An object with NO override: the answer is the chunk itself, but it IS an object transform -- the
	// caller still needs the precedence rules to apply to its write.  Split out because conflating the
	// two would silently disable the masking strip on every un-overridden object.
	{
		const Document d = ParseToCst( Scene(
			  "standard_object\n{\nname X\ngeometry geo\nmaterial m\nposition 0 0 0\n}\n" ) );
		const NodeId base = DocFindByNameAnyRole( d, "X", nullptr, "standard_object", false );
		bool isTransform = false;
		Check( base != 0 && DocTransformOwnerId( d, base, "position", &isTransform ) == base,
		       "rule: an un-overridden object keeps its own chunk as the owner" );
		Check( isTransform,
		       "rule: ... but it is STILL an object-transform edit (the precedence rules apply to the base too)" );
	}

	std::printf( "\n%d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
