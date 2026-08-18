//////////////////////////////////////////////////////////////////////
//
//  CstSourceInstanceTest.cpp - 87 step 3a/3b: `source` on a `standard_object`.
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
//                  `source S parent S` / duplicate document-level name -- each separately,
//                  each with its own TRUE reason.
//    [provenance]  the manager records (entry -> instancing chunk, source node); the source
//                  itself has none; an unknown name leaves the out-pointers untouched.
//    [visible]     the source still renders after being instanced.
//    [area]        an instanced emitter's GetArea() tracks the INSTANCE's transform, not the
//                  source's -- the one property with a documented bug history (2026-08-13).
//    [incremental] an edit to a `source` chunk refuses -> full-derive fallback; so does an edit
//                  to an entry that still carries a PROVENANCE row (`source none`, or the line
//                  DELETED), which is how the stale row gets retired -- while a `source none`
//                  chunk that was never an instance stays on the incremental path.
//    [gizmo]       CstObjectTransformKind answers for the SOURCE's role, so the transform gate
//                  and the transform commit cannot disagree (the live/CST divergence class); a
//                  same-named `override_object` OWNS the pose, so the components commit walks to
//                  it (else the drag reports SUCCESS and the re-derive puts the object back);
//                  `matrix` / `quaternion` / `scale` are stripped SYMMETRICALLY from whichever
//                  chunk the commit lands on -- and from EVERY same-named layer, not merely the one
//                  written, since an untouched layer's `scale` is applied on top of the flip the
//                  written `orientation` already carries; and each refusal names its own true cause
//                  -- the author's `source` line rather than a chunk type their scene does not
//                  contain, a blocking non-unit SCALE rather than a rotation they never made, EVERY
//                  defect when a matrix has more than one, and the `scale` param behind a REFLECTION.
//
//  87 STEP 3b adds SUBTREE instancing -- `source S` where S has CHILDREN -- and its blocks
//  are grouped at the END of main(), under a banner.  What they lock in:
//    [subtree]     a 2-level expansion == the two hand-written objects it stands for (names,
//                  bindings, LINKS and world bboxes, via the DumpJob oracle); a 3-level one
//                  composes through the middle clone; each clone is parented to the CLONE of
//                  its parent, not to the original's parent nor flat to the instance root.
//    [naming]      ONE level of qualification: `I.D`, never the path `I.C.D`.
//    [light]       a `rect_light` in the subtree -- all FOUR of its entities are renamed by
//                  remapping the one `name`, which is why clones are built by re-Finalizing
//                  the member's own CHUNK rather than by cloning the live object.
//    [csg]         a `csg_object` in the subtree -- the operands are SHARED BY POINTER (an
//                  operand's matrix is CSG-LOCAL, so it reads correctly relative to whichever
//                  composite asks) and the result equals a hand-written csg over the same two.
//    [nested]      an instance INSIDE an instanced subtree: `I2.I1` and `I2.I1.A2`, and the
//                  provenance chain lands on a live entry at every hop.
//    [collision]   a synthesized name colliding with an authored chunk, in BOTH declaration
//                  orders (the manager pre-check cannot see the AFTER case) and for a
//                  `rect_light` collider, which the source-resolvable role scan cannot see.
//    [refuse]      an `override_object` on a subtree member; a member declared BELOW the
//                  instance; a recursion reached through a TRANSITIVE descendant.
//    [gizmo]       a synthesized entry is not transform-routable, and the refusal names the
//                  INSTANCING chunk -- the message 3a shipped unreachable.
//
//    [end-to-end]  the same poses driven through SceneEditController -- the whole chain
//                  (Apply -> the DecomposeRigid post-mutate gate -> CommitPendingCstObject
//                  Transforms -> ApplyCstObjectComponentsEdit) -- asserting where the object
//                  ENDS UP after the commit re-derives, which a direct Job::ApplyCst*Edit call
//                  cannot: the direct route accepts inputs production can never produce.  That
//                  includes the shape whose commit takes the INCREMENTAL re-apply (an authored
//                  csg_object with neither `source` nor `override_object`): its re-point must
//                  discard the transform stack the live gesture pushed onto, or the pose is
//                  applied TWICE -- once from the stack, once from the committed `position`.
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
#include "../src/Library/Interfaces/ILogPriv.h"   // round-3: pin a REFUSAL on its own reason, not on an rc every failure shares
#include "../src/Library/SceneEditor/SceneEditController.h"   // round-3: the gizmo-facing scale refusal and the message it prints
#include "../src/Library/Objects/CSGObject.h"   // 3b: a csg_object in the subtree -- the operands must be SHARED, which is a pointer identity

#include <algorithm>			// std::sort -- EntryNames()'s whole-set oracle
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
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

// x of an object's world bounding-box centre.  The transform probe the gizmo tests use: IObject
// exposes IBasicTransform, not the composed matrix, so a TRANSLATION is read as a bbox shift.
// A missing object answers a value no delta assertion can accidentally satisfy.
static double CenterX( IObject* o )
{
	if( !o ) return -1.0e30;
	const BoundingBox bb = o->getBoundingBox();
	return ( bb.ll.x + bb.ur.x ) * 0.5;
}

// The world bbox CENTRE of an object, as three numbers -- the transform probe every 87
// step 3b subtree assertion uses.  `IObject` exposes IBasicTransform, not the composed
// matrix, so a COMPOSED position is read as a bbox shift; the geometry in these scenes is
// a unit sphere / unit box centred on the node, so the centre IS the composed position.
// A missing object answers a value no assertion can accidentally satisfy.
static bool CenterIs( IObject* o, double x, double y, double z, std::string* outGot = nullptr )
{
	if( !o ) { if( outGot ) *outGot = "(no such object)"; return false; }
	const BoundingBox bb = o->getBoundingBox();
	const double cx = ( bb.ll.x + bb.ur.x ) * 0.5;
	const double cy = ( bb.ll.y + bb.ur.y ) * 0.5;
	const double cz = ( bb.ll.z + bb.ur.z ) * 0.5;
	if( outGot ) {
		char buf[96];
		std::snprintf( buf, sizeof( buf ), "%.6g %.6g %.6g", cx, cy, cz );
		*outGot = buf;
	}
	// 1e-9 -- these are exact translations through a composed matrix, so the only slack
	// needed is FP noise from the multiply, not tolerance for a different answer.
	return std::fabs( cx - x ) < 1.0e-9 && std::fabs( cy - y ) < 1.0e-9 && std::fabs( cz - z ) < 1.0e-9;
}

// An object's recorded parent NAME (the authored graph), or "(no manager)".  The link
// structure -- not just the world pose -- is what a subtree expansion has to get right:
// a flattened expansion that parented every clone to the instance root would still put a
// one-level subtree in the right place.
static std::string ParentOf( Job* j, const char* name )
{
	if( !j || !j->GetObjects() ) return "(no manager)";
	const char* p = j->GetObjects()->GetObjectParent( name );
	return p ? std::string( p ) : std::string();
}

// EVERY object name the manager holds, sorted and joined with '|'.  The WHOLE-SET oracle the
// revisit-guard fixtures below need: a false "recursive definition" refusal drops the entire
// instance subtree, so what distinguishes it from a correct derive is the SET of entries, not
// any one of them -- and a per-name `!= 0` sweep cannot notice an entry that should NOT exist.
// Sorted rather than in registration order on purpose: sibling ORDER has its own fixtures
// (GetItemSerial), and pinning it here too would make this oracle red for two unrelated reasons.
static std::string EntryNames( Job* j )
{
	if( !j || !j->GetObjects() ) return "(no manager)";
	struct Collect : public IEnumCallback<const char*>
	{
		std::vector<std::string> names;
		bool operator()( const char* const& n ) override { if( n ) names.push_back( n ); return true; }
	} c;
	j->GetObjects()->EnumerateItemNames( c );
	std::sort( c.names.begin(), c.names.end() );
	std::string out;
	for( std::size_t i = 0; i < c.names.size(); ++i ) { if( i ) out += '|'; out += c.names[i]; }
	return out;
}

// " (got X)", for appending to a POSE assertion's message.  Costs nothing on a pass (Check prints
// only failures) and turns "the object is not where it should be" into the number it IS at, which
// is what says WHICH defect fired -- a double-apply reads 10.25, a commit that never landed 0.25.
static std::string Got( double x )
{
	char buf[64];
	std::snprintf( buf, sizeof( buf ), " (got %.6g)", x );
	return buf;
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

// A minimal ILogPrinter that records every message containing `needle` (case-sensitive
// substring).  Installed once via GlobalLogPriv()->AddPrinter and never removed -- no test in
// this binary depends on log output being ABSENT, and RemoveAllPrinters would also kill the
// default stdout/file printers for everything that runs afterwards.
//
// WHY A TEST NEEDS THE LOG AT ALL.  Several Job::ApplyCst*Edit refusals are indistinguishable
// by RETURN CODE: a guard that refuses returns 0, and so does a write that sails past the guard
// and then fails the dry-run derive (RederiveCstDocumentFull_ returns 0 on any diagnostic).  An
// `== 0` assertion therefore pins the pair, not the guard -- it stays GREEN with the guard
// deleted.  The guard's own REASON is the only thing that separates them.
class CapturingLogPrinter : public virtual RISE::ILogPrinter, public virtual RISE::Implementation::Reference
{
public:
	explicit CapturingLogPrinter( std::string needle ) : mNeedle( std::move( needle ) ) {}

	void Print( const RISE::LogEvent& event ) override
	{
		const std::string msg( event.szMessage );
		if( msg.find( mNeedle ) != std::string::npos ) {
			std::lock_guard<std::mutex> lk( mMutex );
			mMatches.push_back( msg );
		}
	}
	void Flush() override {}

	int MatchCount() const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		return static_cast<int>( mMatches.size() );
	}
	std::string LastMatch() const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		return mMatches.empty() ? std::string() : mMatches.back();
	}

protected:
	~CapturingLogPrinter() override {}

private:
	std::string                mNeedle;
	mutable std::mutex         mMutex;
	std::vector<std::string>   mMatches;
};

// Every diagnostic `DeriveToJob` raised for ONE chunk, joined.  A scene can hold two REFUSABLE
// chunks whose reasons differ (a self-parenting instance beside an unparented sibling that merely
// names the same source), and a whole-bag substring search could not tell which chunk got which
// message.  Today the derive stops at the first refusal, so the bag holds one -- this keeps the
// assertions meaningful if that ever changes.  `who` is "standard_object `NAME`: ...".
static std::string DiagsForChunk( const std::string& scene, const char* chunkName )
{
	std::vector<std::string> diags;
	DumpCst( scene, &diags );
	const std::string prefix = std::string( "standard_object `" ) + chunkName + "`:";
	std::string out;
	for( std::size_t i = 0; i < diags.size(); ++i ) {
		if( diags[i].compare( 0, prefix.size(), prefix ) == 0 ) { out += diags[i]; out += "\n"; }
	}
	return out;
}

int main()
{
	std::printf( "CstSourceInstanceTest -- 87 step 3a/3b: `source` instancing, collapse + subtree\n" );

	// Installed for the whole run; only the gizmo blocks below read them.
	CapturingLogPrinter* pMatrixLogOwned = new CapturingLogPrinter( "ApplyCstObjectMatrixEdit" );
	RISE::GlobalLogPriv()->AddPrinter( pMatrixLogOwned );
	CapturingLogPrinter* pMatrixLog = pMatrixLogOwned;   // AddPrinter addref'd; keep a raw read handle
	safe_release( pMatrixLogOwned );                     // drop OUR construction ref (safe_release nulls its arg)

	CapturingLogPrinter* pRefusalLogOwned = new CapturingLogPrinter( "transform cannot be saved on a CST-loaded scene" );
	RISE::GlobalLogPriv()->AddPrinter( pRefusalLogOwned );
	CapturingLogPrinter* pRefusalLog = pRefusalLogOwned;
	safe_release( pRefusalLogOwned );

	// The POST-MUTATE gate's refusal (a different one -- the op-level gate above admitted the op,
	// and the resulting MATRIX turned out not to decompose).  Read by the end-to-end block.
	CapturingLogPrinter* pDecomposeLogOwned = new CapturingLogPrinter( "transform is not committable to a csg_object" );
	RISE::GlobalLogPriv()->AddPrinter( pDecomposeLogOwned );
	CapturingLogPrinter* pDecomposeLog = pDecomposeLogOwned;
	safe_release( pDecomposeLogOwned );

	// 87 step 3c: the DESCRIPTOR's own numeric rejection.  It goes to the LOG, and the
	// diagnostic that reaches `diags` is the shared, uninformative "invalid parameter(s)
	// (see log)" -- so an assertion on the diagnostic alone cannot tell a numeric rejection
	// from an undeclared parameter, a value-less line, or any other PASS-1 failure.  Read
	// the log line, by DELTA, since the whole run shares one printer.
	CapturingLogPrinter* pNumericLogOwned = new CapturingLogPrinter( "expects finite numeric value" );
	RISE::GlobalLogPriv()->AddPrinter( pNumericLogOwned );
	CapturingLogPrinter* pNumericLog = pNumericLogOwned;
	safe_release( pNumericLogOwned );

	// 87 step 3c: the descriptor's UNDECLARED-PARAMETER rejection, for the same reason as
	// the one above -- it goes to the LOG, and what reaches `diags` is the same generic
	// "invalid parameter(s) (see log)".  An assertion that the DIAGNOSTICS lack this text
	// is therefore vacuous: they never contain it, whatever the descriptor declares.  Read
	// by DELTA, since other blocks in this binary legitimately provoke it.
	CapturingLogPrinter* pUndeclaredLogOwned = new CapturingLogPrinter( "not declared in `standard_object` descriptor" );
	RISE::GlobalLogPriv()->AddPrinter( pUndeclaredLogOwned );
	CapturingLogPrinter* pUndeclaredLog = pUndeclaredLogOwned;
	safe_release( pUndeclaredLogOwned );

	// 87 step 3c review: `override_object`'s target-missing message, which is the one a
	// bare reference to a counted chunk USED to land on.  It is a LOG line -- the parser
	// returns false and PASS-2 turns that into the generic "apply failed (e.g. unresolved
	// reference); see log" -- so a diagnostics-only assertion could not see whether the
	// misleading three-cause message was still being printed.  Read by DELTA.
	CapturingLogPrinter* pOverrideMissingLogOwned = new CapturingLogPrinter( "not found in scene" );
	RISE::GlobalLogPriv()->AddPrinter( pOverrideMissingLogOwned );
	CapturingLogPrinter* pOverrideMissingLog = pOverrideMissingLogOwned;
	safe_release( pOverrideMissingLogOwned );

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

	// [subtree] 87 step 3b: a source WITH CHILDREN is EXPANDED.  3a refused this and named 3b;
	// the refusal is gone and the whole subtree is copied.  Kept here, at the position 3a's
	// refusal test held, so the replacement is visible in the diff.
	{
		const std::string body = SRC_LEAF
		                       + "standard_object\n{\nname kid\ngeometry boxg\nmaterial m\nparent S\n}\n"
		                       + "standard_object\n{\nname I\nsource S\n}\n";
		std::vector<std::string> diags;
		DumpCst( Scene( body ), &diags );
		std::string all;
		for( std::size_t i = 0; i < diags.size(); ++i ) all += diags[i];
		Check( diags.empty(), ( std::string( "subtree: a source WITH CHILDREN now expands rather than refusing (" ) + all + ")" ).c_str() );
		// (The companion assertion here used to search the diagnostics for `step 3b` /
		// `has CHILDREN` -- strings 3b DELETED from production, so nothing could emit them
		// and no mutation could turn the line red.  `diags.empty()` above already says
		// everything it said.  What the 3a refusal is GONE means positively is that the
		// child is really copied, which is what the assertion below now checks.)
		Job* jk = DeriveJob( Scene( body ) );
		Check( Obj( jk, "I.kid" ) != 0 && ParentOf( jk, "I.kid" ) == "I",
		       "subtree: ... and the child is really COPIED, not merely un-refused" );
		jk->release();
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
		// UNCONDITIONAL.  This was written as a disjunction ("no `step 3b` OR names `parent S`"),
		// which a message mentioning NEITHER term satisfies -- so deleting both strings from the
		// diagnostic would have left it green while violating the property it states.  It was also
		// strictly weaker than the "recursive definition" pin above it.  The message must NAME the
		// author's `parent` line, because that line is the cause.
		Check( all.find( "`parent S`" ) != std::string::npos,
		       "refuse: ... naming the `parent` line as the cause, not sending the author after a subtree that does not exist" );
		// THE SPECIFIC MESSAGE, not the walk's generic one.  ClonePlanBuilder's revisit guard
		// would ALSO catch this scene (the instancing chunk is on the clone path from the start,
		// so cloning it as its own source's child trips the guard) -- and its message says only
		// "expanding this instance would copy `S` into its own subtree", which is true but never
		// mentions the one `parent` line that caused it.  The pre-walk check exists to say that,
		// so the assertion pins WHICH of the two fired.
		Check( all.find( "on the SAME chunk" ) != std::string::npos,
		       "refuse: ... via the pre-walk check that knows the cause, not the walk's generic revisit guard" );
		Check( all.find( "would copy" ) == std::string::npos,
		       "refuse: ... so the author is not left with a message that omits their `parent` line" );
		// A source with a REAL other child is EXPANDED under 3b -- but a self-parenting instance
		// of it is still a recursive definition, and that check fires first.  (Under 3a this
		// scene got the has-children/3b message, which no longer exists.)
		//
		// THE DISCRIMINATING NEEDLE, not the shared one.  THREE sites emit "recursive
		// definition" -- the pre-walk self-parent check and both of the walk's revisit
		// guards -- so a bare "recursive definition" assertion stays GREEN with the
		// pre-walk block deleted entirely, which is precisely the claim ("that check fires
		// first") this line exists to make.  "on the SAME chunk" is emitted only by the
		// pre-walk check.
		Check( RefusedWith( Scene( SRC_LEAF
		                         + "standard_object\n{\nname kid\ngeometry boxg\nmaterial m\nparent S\n}\n"
		                         + "standard_object\n{\nname I\nsource S\nparent S\n}\n" ),
		                    "on the SAME chunk" ),
		       "refuse: ... and a genuine OTHER child does not stop the self-parent refusal (the subtree is real, the recursion still is)" );
		// TWO self-parenting instances.  A "S has exactly ONE child" test is defeated here: the
		// count is 2, so it falls through to the 3b message -- which is precisely the misdirection
		// this branch exists to remove, since there is still no subtree and both children are the
		// author's own `parent` lines.  Every child instances S, so both get the recursive-
		// definition message.
		{
			const std::string two = SRC_LEAF
			                      + "standard_object\n{\nname I1\nsource S\nparent S\n}\n"
			                      + "standard_object\n{\nname I2\nsource S\nparent S\n}\n";
			std::string twoAll;
			Check( RefusedWith( Scene( two ), "on the SAME chunk", &twoAll ),
			       "refuse: ... and a SECOND self-parenting instance does not defeat the rule (count is 2, still no subtree)" );
			// (The line that stood here searched for `has CHILDREN -- instancing a
			// multi-node subtree`, a string 3b deleted from production; nothing could emit
			// it and no mutation could redden it.  The needle above now carries the whole
			// claim: it is the PRE-WALK self-parent check that fires, by its own message.)
		}
		// THE REFUSAL IS PER-CHUNK, NOT PER-SOURCE.  `I source S` with NO `parent` line, beside a
		// SEPARATE `K source S parent S`.  S therefore has a child, so under 3b `I` has a real
		// subtree to copy -- and `K` is declared AFTER `I`, so what `I` gets is the
		// declared-after refusal.  What it must NOT get is the self-parent one: `I` has no
		// `parent` line at all, and being told its own `parent S` created a recursive definition
		// is exactly the misdirection this arc has repeatedly had to remove.
		//
		// `I` is declared BEFORE `K`, so `I` is the chunk this scene diagnoses.  (DeriveToJob stops
		// at the FIRST refusal, so `K` raises nothing here -- measured, not assumed; the message
		// `K` gets on its own is pinned by the single-chunk case at the top of this block.)  The
		// assertions read `I`'s diagnostic SPECIFICALLY rather than the whole bag, so they keep
		// their meaning if the derive ever continues past a refusal.
		{
			const std::string mixed = SRC_LEAF
			                        + "standard_object\n{\nname I\nsource S\n}\n"
			                        + "standard_object\n{\nname K\nsource S\nparent S\n}\n";
			const std::string forI = DiagsForChunk( Scene( mixed ), "I" );
			Check( forI.find( "is declared AFTER this instancing chunk" ) != std::string::npos,
			       "refuse: an instance whose source acquires a child BELOW it is refused for that reason" );
			Check( forI.find( "recursive definition" ) == std::string::npos,
			       "refuse: ... and is NOT told its own `parent` line is the cause -- it has none" );
		}
	}

	// [refuse] DOCUMENT-level name collision: two object chunks declaring the entry name.  The
	// manager pre-check alone cannot see this when the other chunk is declared AFTER -- it does
	// not exist yet -- so the scan is over the document, and it names both chunks.
	{
		// COMMENTS ON PURPOSE.  The message exists to name BOTH chunks so the author can
		// reconcile them, which means naming them in terms an author can COUNT TO.  A raw CST
		// item index is not one: trivia (comments, blank lines) are items too, so in this scene
		// -- whose colliding chunks are the 11th and 12th the author wrote -- the raw indices are
		// nowhere near 11 and 12.  Without the comments the two numberings would coincide and
		// this test would pass on the broken message.
		//
		// TWO DIFFERENT ROLES ON PURPOSE, too.  The message claims to print each chunk's role;
		// with both colliding chunks a `standard_object` that claim is untestable -- a message
		// that printed one hard-coded literal, or printed items[a]->role twice, reads identically.
		// The second chunk is therefore a `csg_object`, and BOTH spellings are asserted.
		const std::string body = SRC_LEAF
		                       + "standard_object\n{\nname opa\ngeometry geo\nmaterial m\n}\n"
		                       + "standard_object\n{\nname opb\ngeometry boxg\nmaterial m\nposition 1 0 0\n}\n"
		                       + "# a comment, so the raw item index and the chunk ordinal diverge\n"
		                       + "\n"
		                       + "# and another\n"
		                       + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n"
		                       + "\n# a third, between the two colliding chunks\n\n"
		                       + "csg_object\n{\nname I\nobja opa\nobjb opb\noperation union\n}\n";
		std::string all;
		Check( RefusedWith( Scene( body ), "declared by MORE THAN ONE object chunk", &all ),
		       "refuse: the entry name is also declared by a LATER authored chunk (document-level mis-targeting)" );
		Check( all.find( "chunk #11" ) != std::string::npos && all.find( "chunk #12" ) != std::string::npos,
		       "refuse: ... naming both by their position among the file's CHUNKS (#11 and #12 here, comments not counted)" );
		Check( all.find( "a `standard_object`" ) != std::string::npos && all.find( "a `csg_object`" ) != std::string::npos,
		       "refuse: ... and by role -- BOTH roles, so the author knows what to look for" );
		Check( all.find( "item " ) == std::string::npos,
		       "refuse: ... and never by raw CST item index (which counts comments and blank lines)" );
	}

	// [refuse] THE PARSER-SIDE `source` GATES ARE NOW UNREACHABLE FROM A SCENE FILE, and
	// that is RECORDED here rather than tested, because 87 step 3d removed the only route
	// to them.  `standard_object`'s Finalize carries two -- the geometry+source exclusivity
	// refusal and the lone-unexpanded-`source` backstop -- and PASS-2 hands every chunk
	// carrying a real `source` to ExpandSourceInstance instead of to Finalize, so neither
	// can fire from a document.  The `instance_array` generator used to reach them by
	// passing a `source` straight through into the standard_object it synthesized,
	// alongside a `geometry` from its `template`; that generator is gone.  The EXPANSION's
	// own copy of the exclusivity rule -- the reachable one -- is pinned by the
	// `[refuse] geometry + source` block far above.  The parser's two are backstops
	// against a future third apply path, exactly as their own comments say.

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

	// [closure] `source` is a descriptor-declared Reference, so editing the SOURCE puts the
	// instancing chunk in the edit closure.  Without this the incremental apply would re-point
	// S while I kept its stale copy of S's bindings.
	//
	// THIS IS THE HOP THAT WAS NEVER IN DOUBT, and 87 step 3d rewrote this header to read as
	// the warrant for deleting the document-wide incremental refusal.  It is not: the hop that
	// needed proving runs the other way, from a subtree MEMBER to the instancing chunk, and it
	// does NOT close -- see the `[subtree][closure][incremental]` block in the 3b section below,
	// which asserts the closure fact as it really is and then pins the BEHAVIOUR the restored
	// document-wide gate delivers.
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
	// [incremental] A DOCUMENT HOLDING ANY INSTANCING CHUNK REFUSES WHOLESALE -> the caller
	// full-derives, which re-expands from the document.  Restored after 87 step 3d deleted the
	// document-wide form on a claim that turned out to be false (see the SUBTREE-MEMBER block at
	// the end of this file for the divergence it lets through, and Cst.cpp's `source` guard for
	// why nothing narrower can see it).
	//
	// The refusal is document-wide, so it also catches EVERY OTHER chunk in such a document --
	// the ordinary sibling `S` included.  That over-refusal is the ADVERTISED COST, asserted here
	// rather than left implicit: an author who writes one `source` line pays a full DeriveToJob
	// (ClearAll + manager rebind, rc 2/3) on every subsequent discrete edit anywhere in the scene.
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
		Check( applied == 0 && all.find( "contains a `source` instancing chunk" ) != std::string::npos,
		       "incremental: a document containing a `source` instancing chunk refuses -> full-derive fallback" );
		// The sibling: SAME refusal, SAME reason -- not the per-chunk one, which never gets a look in.
		const NodeId sid = DocFindByName( d, "standard_object/S" );
		Document d3 = DocSetParamValue( d, sid, "position", 0, "3 0 0" );
		std::vector<std::string> di2;
		const int applied2 = DeriveToJobIncremental( d3, *j, std::vector<NodeId>( 1, sid ), &di2 );
		std::string all2;
		for( std::size_t i = 0; i < di2.size(); ++i ) { all2 += di2[i]; all2 += "\n"; }
		Check( applied2 == 0 && all2.find( "contains a `source` instancing chunk" ) != std::string::npos,
		       "incremental: ... and so does an ORDINARY chunk in the same document -- the cost of a document-wide gate, stated" );
		j->release();
	}

	// [incremental] AND THE GATE IS NOT OVER-BROAD ACROSS DOCUMENTS.  The byte-identical ordinary
	// chunk in a document with NO instancing chunk still takes the incremental path -- so the
	// refusal above is keyed on the DOCUMENT holding a `source`, not on "objects are risky".
	{
		Job* j = new Job();
		Document d = ParseToCst( Scene( SRC_LEAF ) );
		std::vector<std::string> diags;
		DeriveToJob( d, *j, &diags );
		const NodeId sid = DocFindByName( d, "standard_object/S" );
		Document d2 = DocSetParamValue( d, sid, "position", 0, "3 0 0" );
		std::vector<std::string> di;
		const int applied = DeriveToJobIncremental( d2, *j, std::vector<NodeId>( 1, sid ), &di );
		Check( applied >= 1 && di.empty(), "incremental: the same ordinary standard_object in a `source`-free document still applies incrementally" );
		j->release();
	}

	// [incremental] THE DOCUMENT-WIDE SIGNAL IS MAINTAINED ACROSS INSERT AND ERASE, not only
	// across parse and param-edit.  `Job::ApplyCstInsertChunk` builds a `source` chunk through
	// `DocInsertItem` and `ApplyCstRemoveChunk` drops one through `DocEraseChunkTidy` ->
	// `DocRemoveItem` -> `DocEraseItem`, so a counter maintained only at parse time would let an
	// AGENT-CREATED instance take the incremental path forever after -- and would keep charging
	// full derives forever after the instance was deleted.  Driven through the Doc primitives so
	// the two directions are separable; the param-edit direction is covered by the `source none`
	// block below (clearing the slot decrements, which is what lets the PROVENANCE gate be the
	// one that answers there).
	{
		Job* j = new Job();
		Document d = ParseToCst( Scene( SRC_LEAF ) );
		std::vector<std::string> diags;
		DeriveToJob( d, *j, &diags );
		const NodeId sid = DocFindByName( d, "standard_object/S" );

		// INSERT an instancing chunk at the end of the document, exactly the way
		// Job::ApplyCstInsertChunk builds one: parse the chunk text into its own one-item
		// Document and splice that item in.
		Document instDoc = ParseToCst( std::string( "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ) );
		NodeRef inst = DocResolveNodeId( instDoc, DocNodeIdAt( instDoc, 0 ) );
		Check( inst != nullptr && inst->kind == NodeKind::Chunk, "incremental-insert: the instancing chunk parses to a single top-level chunk item" );
		Document dIns = DocInsertItem( d, DocItemCount( d ), inst );
		Document e1 = DocSetParamValue( dIns, sid, "position", 0, "3 0 0" );
		std::vector<std::string> di1;
		const int a1 = DeriveToJobIncremental( e1, *j, std::vector<NodeId>( 1, sid ), &di1 );
		std::string all1;
		for( std::size_t i = 0; i < di1.size(); ++i ) { all1 += di1[i]; all1 += "\n"; }
		Check( a1 == 0 && all1.find( "contains a `source` instancing chunk" ) != std::string::npos,
		       "incremental-insert: INSERTING an instancing chunk arms the document-wide refusal" );

		// ERASE it again -- the document is `source`-free once more and the gate must stand down.
		Document dEra = DocEraseItem( dIns, DocItemCount( dIns ) - 1 );
		Document e2 = DocSetParamValue( dEra, sid, "position", 0, "4 0 0" );
		std::vector<std::string> di2;
		const int a2 = DeriveToJobIncremental( e2, *j, std::vector<NodeId>( 1, sid ), &di2 );
		Check( a2 >= 1 && di2.empty(), "incremental-insert: ERASING it again disarms it -- the count is a delta, not a latch" );
		j->release();
	}

	// [incremental] THE PER-CHUNK `carries source` REFUSAL IS NOW UNREACHABLE FROM A DOCUMENT, and
	// that is RECORDED here rather than tested.  It is the 3a gate that says WHY such a chunk
	// cannot be re-Finalized (PASS-2 expands it; its own Finalize would refuse or build a bare
	// container), and it is KEPT -- the document-wide gate above is the thing that would be retired
	// first, once the closure consumer-switch and a transitive source -> descendant edge land, and
	// retiring it must not silently re-open the per-chunk case.  The document-wide gate returns
	// before any closure member is inspected, so no document can reach it today.  The OTHER
	// per-chunk gate -- the live PROVENANCE row -- stays fully reachable and is pinned by the three
	// blocks below, because the document it fires in has had its `source` cleared or deleted and so
	// no longer trips the document-wide count.

	// [incremental] CLEARING the slot -- `source none` -- refuses too, for a DIFFERENT reason.
	// The chunk then derives as a plain container, which the in-place re-point handles perfectly
	// well.  But the manager entry still carries the PROVENANCE row the earlier expansion wrote,
	// and provenance is retired only by RemoveItem / Shutdown -- neither of which an in-place
	// re-point calls.  An incremental commit would leave GetObjectProvenance answering "(I, S)"
	// for an object that is no longer an instance of anything.
	//
	// The gate keys on THE LIVE ROW, not on the `source` text -- see the two tests below for the
	// pair of spellings that proves why.
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
		Check( applied == 0 && all.find( "PROVENANCE row" ) != std::string::npos,
		       "incremental: `source none` refuses too -- the live provenance row is what only a full re-derive retires" );
		j->release();
	}

	// [incremental] DELETING the `source` LINE -- the spelling a text-presence gate misses.  The
	// chunk carries no `source` at all afterwards, so "does the text say `source`?" answers NO and
	// the in-place re-point commits, leaving `I` a bare container with a live `(I, S)` provenance
	// row.  Reachable in production: SceneEditor's agent-Undo of an agent-INSERTED `source` takes
	// the `prevValueWasAbsent` arm, which routes RouteCstParamRemove_ -> ApplyCstParamRemoveChecked.
	{
		const std::string scene = Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" );
		const std::string path  = WriteTempScene( "cst_source_instance_remove.RISEscene", scene );
		Job* j = new Job();
		const bool loaded = j->LoadAsciiSceneViaCst( path.c_str() );
		Check( loaded, "incremental-remove: the fixture loads with a retained CST head" );
		if( loaded ) {
			const char* c0 = 0;
			Check( j->GetObjects() && j->GetObjects()->GetObjectProvenance( "I", &c0, 0 ),
			       "incremental-remove: (precondition) `I` starts with a provenance row" );
			const int rc = j->ApplyCstParamRemoveChecked( "I", "standard_object", "source", 0 );
			Check( rc == 2, "incremental-remove: REMOVING the `source` line takes the FULL re-derive (rc=2), not the in-place re-point (rc=1)" );
			const char* c1 = 0;
			Check( j->GetObjects() && !j->GetObjects()->GetObjectProvenance( "I", &c1, 0 ),
			       "incremental-remove: ... so the stale provenance row is GONE -- `I` is a bare container now" );
		}
		j->release();
		std::remove( path.c_str() );
	}

	// [incremental] ... and the OTHER direction: a `source none` line on a chunk that was NEVER an
	// instance must NOT cost a full re-derive.  A text-presence gate charges this container a
	// ClearAll + full derive + manager rebind on EVERY later edit, forever, for a retirement that
	// has nothing to retire -- while the byte-identical chunk without the `source none` line goes
	// incremental.  Pinned against the ordinary container as the control.
	{
		const std::string container = "standard_object\n{\nname C\nsource none\nposition 5 0 0\n}\n";
		const std::string plain     = "standard_object\n{\nname C\nposition 5 0 0\n}\n";
		Job* j = new Job();
		Document d = ParseToCst( Scene( SRC_LEAF + container ) );
		std::vector<std::string> diags;
		DeriveToJob( d, *j, &diags );
		const NodeId id = DocFindByName( d, "standard_object/C" );
		Document d2 = DocSetParamValue( d, id, "position", 0, "9 0 0" );
		std::vector<std::string> di;
		const int applied = DeriveToJobIncremental( d2, *j, std::vector<NodeId>( 1, id ), &di );
		Check( diags.empty() && applied >= 1 && di.empty(),
		       "incremental: a `source none` container that was never an instance still applies INCREMENTALLY (no forever-full-derive tax)" );
		j->release();
		// The control: the same chunk without the `source none` line behaves identically.
		Job* j2 = new Job();
		Document e = ParseToCst( Scene( SRC_LEAF + plain ) );
		std::vector<std::string> ediags;
		DeriveToJob( e, *j2, &ediags );
		const NodeId eid = DocFindByName( e, "standard_object/C" );
		Document e2 = DocSetParamValue( e, eid, "position", 0, "9 0 0" );
		std::vector<std::string> ei;
		const int applied2 = DeriveToJobIncremental( e2, *j2, std::vector<NodeId>( 1, eid ), &ei );
		Check( applied2 >= 1 && ei.empty() && applied2 == applied,
		       "incremental: ... exactly as the identical chunk WITHOUT the `source none` line does" );
		j2->release();
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

	// ---------------------------------------------------------------- gizmo transform routing
	// [gizmo] THE ONE WITH LIVE/CST DIVERGENCE.  An instancing chunk's own role is always
	// `standard_object`, but the entry is built through the SOURCE's parser -- so `source <a
	// csg_object>` yields a node that can express only translate+rotate, exactly like an authored
	// csg_object.  CstObjectTransformKind is the gate SceneEditor consults BEFORE it mutates the
	// live object, precisely so a transform it cannot record is never applied.  Answering on the
	// CHUNK's role made it say 1 ("commit the full `matrix`"), SceneEditor declared the op
	// committable and moved the object, and then ApplyCstObjectMatrixEdit refused at commit --
	// leaving a live transform the Document never recorded, which the next full re-derive silently
	// reverts (and the editor's own log says so: "a later full re-derive will REVERT the live
	// transform").  The kind must answer for the TARGET role.
	{
		const std::string csgSrc =
			  "standard_object\n{\nname opa\ngeometry geo\nmaterial m\n}\n"
			  "standard_object\n{\nname opb\ngeometry boxg\nmaterial m\nposition 1 0 0\n}\n"
			  "csg_object\n{\nname C\nobja opa\nobjb opb\noperation union\n}\n";
		const std::string scene = Scene( SRC_LEAF + csgSrc
			+ "standard_object\n{\nname I\nsource C\n}\n"
			+ "standard_object\n{\nname L\nsource S\n}\n" );
		const std::string path = WriteTempScene( "cst_source_instance_gizmo.RISEscene", scene );
		Job* j = new Job();
		const bool loaded = j->LoadAsciiSceneViaCst( path.c_str() );
		Check( loaded, "gizmo: the csg-source fixture loads with a retained CST head" );
		if( loaded ) {
			// THE BUG.  Pre-fix this was 1, and the gate and the commit disagreed.
			Check( j->CstObjectTransformKind( "I" ) == 2,
			       "gizmo: an instance of a `csg_object` classifies as COMPONENTS (2) -- the TARGET role decides, not the chunk's" );
			// ... and the gate now agrees with the commit in BOTH directions.  A `matrix` write is
			// refused (the csg target has no such param) -- which is what kind 1 would have
			// promised -- while the components route the kind DOES promise succeeds.
			//
			// PINNED ON THE REASON, NOT ON rc.  `rc == 0` alone pins NOTHING here: delete the
			// target-role guard and this call still answers 0, because it would then write
			// `matrix` onto the instancing chunk, the dry-run derive would diagnose the param the
			// csg_object target cannot express, and RederiveCstDocumentFull_ returns 0 on any
			// dry-run diagnostic.  The rc assertion was green at 469dc2c2 and 77969fd6, before
			// the guard existed at all.  Only the guard emits its own reason.
			const int matrixLogBefore = pMatrixLog->MatchCount();
			Check( j->ApplyCstObjectMatrixEdit( "I", "1 0 0 0 0 1 0 0 0 0 1 0 5 0 0 1" ) == 0,
			       "gizmo: ... so a `matrix` commit on it is refused, exactly as the kind now says" );
			Check( pMatrixLog->MatchCount() == matrixLogBefore + 1,
			       "gizmo: ... refused by ApplyCstObjectMatrixEdit ITSELF (it logged), not silently by the dry-run derive" );
			Check( pMatrixLog->LastMatch().find( "instances a `csg_object` (via `source`)" ) != std::string::npos,
			       "gizmo: ... and for the GUARD's own reason -- the `source` resolves to a chunk type with no `matrix` param" );
			const double instBefore = CenterX( Obj( j, "I" ) );
			const int rcInst = j->ApplyCstObjectComponentsEdit( "I", "5 0 0", "0 0 0" );
			Check( rcInst >= 1, "gizmo: ... and the COMPONENTS commit the kind routes to SUCCEEDS (no live/CST divergence)" );
			const double instAfter = CenterX( Obj( j, "I" ) );
			Check( std::fabs( ( instAfter - instBefore ) - 5.0 ) < 1e-6,
			       "gizmo: ... and the entry really MOVED by +5 in x (the commit was applied, not merely accepted)" );
			// CONTROL 1: a LEAF-sourced instance is unaffected -- still kind 1, still commits its
			// full matrix (rc=2, the full re-derive a `source` chunk's closure always takes).
			Check( j->CstObjectTransformKind( "L" ) == 1,
			       "gizmo: a LEAF-sourced instance still classifies as MATRIX (1)" );
			const double leafBefore = CenterX( Obj( j, "L" ) );
			Check( j->ApplyCstObjectMatrixEdit( "L", "1 0 0 0 0 1 0 0 0 0 1 0 7 0 0 1" ) == 2,
			       "gizmo: ... and still commits its full matrix (rc=2)" );
			Check( std::fabs( ( CenterX( Obj( j, "L" ) ) - leafBefore ) - 7.0 ) < 1e-6,
			       "gizmo: ... moving it by +7 in x" );
			// CONTROL 2: the properties-panel route (a typed param write) still refuses CLEANLY --
			// `scale` is a `standard_object` param the csg target cannot express, the dry-run
			// derive diagnoses, and nothing is committed or half-applied.
			const double beforeRefusal = CenterX( Obj( j, "I" ) );
			Check( j->ApplyCstParamEdit( "I", "object", "scale", 0, "2 2 2" ) == 0,
			       "gizmo: the properties-panel route still refuses a `scale` on a csg-sourced instance (rc=0)" );
			Check( std::fabs( CenterX( Obj( j, "I" ) ) - beforeRefusal ) < 1e-6,
			       "gizmo: ... cleanly -- the refused edit left the live entry exactly where the committed one put it" );
		}
		j->release();
		std::remove( path.c_str() );
	}

	// [gizmo] CONTROL 3: an AUTHORED csg_object takes the same kind-2 route it always did, and its
	// components commit still goes through the INCREMENTAL apply (rc=1) -- the instance-aware
	// resolution added to ApplyCstObjectComponentsEdit must not perturb the ordinary case.
	{
		const std::string scene = Scene(
			  "standard_object\n{\nname opa\ngeometry geo\nmaterial m\n}\n"
			  "standard_object\n{\nname opb\ngeometry boxg\nmaterial m\nposition 1 0 0\n}\n"
			  "csg_object\n{\nname C\nobja opa\nobjb opb\noperation union\n}\n" );
		const std::string path = WriteTempScene( "cst_source_instance_csgctl.RISEscene", scene );
		Job* j = new Job();
		const bool loaded = j->LoadAsciiSceneViaCst( path.c_str() );
		Check( loaded, "gizmo-control: the authored-csg fixture loads" );
		if( loaded ) {
			Check( j->CstObjectTransformKind( "C" ) == 2, "gizmo-control: an authored csg_object is still COMPONENTS (2)" );
			const double beforeX = CenterX( Obj( j, "C" ) );
			Check( j->ApplyCstObjectComponentsEdit( "C", "5 0 0", "0 0 0" ) == 1,
			       "gizmo-control: ... and its components commit still takes the INCREMENTAL apply (rc=1)" );
			// rc alone is an EMPTY assertion on this route -- the whole class of defect this file
			// keeps finding is "reports success, object did not move" (see the override block
			// below, where rc>=1 stayed green through a silent revert).  Assert the POSE.
			Check( std::fabs( ( CenterX( Obj( j, "C" ) ) - beforeX ) - 5.0 ) < 1e-6,
			       "gizmo-control: ... and the object really LANDED at the committed position, not merely rc=1" );
			Check( j->ApplyCstObjectMatrixEdit( "C", "1 0 0 0 0 1 0 0 0 0 1 0 5 0 0 1" ) == 0,
			       "gizmo-control: ... while a `matrix` commit on it is still refused" );
		}
		j->release();
		std::remove( path.c_str() );
	}

	// ---------------------------------------------------------------- override_object owner walk
	// [gizmo] THE DRAG THAT UN-HAPPENS, ROUND 3.  A same-named `override_object` is applied AFTER
	// the base chunk and REPLACES the transform fields it names, so a commit written to the base
	// chunk is overwritten by the re-derive: the gizmo moves the object, the commit reports
	// SUCCESS, nothing is logged, and the object is back where it started.  ApplyCstObjectMatrix
	// Edit has walked to the override for exactly this reason since long before 87.
	//
	// FOR THE CSG-SOURCED INSTANCE THIS IS A REGRESSION, not a pre-existing gap: at 469dc2c2 and
	// 77969fd6 the instance classified as kind 1 and its drag went through the MATRIX route, which
	// HAS the walk -- and it worked.  Round 2 (c18e54b6) re-answered the kind as 2 and rerouted
	// the commit to ApplyCstObjectComponentsEdit, which had no walk, turning a working drag into
	// a silent revert.  (c18e54b6's own message disclosed the asymmetry as pre-existing.  That is
	// true for an AUTHORED csg_object -- pinned below -- and false for the instance.)
	{
		const std::string csgSrc =
			  "standard_object\n{\nname opa\ngeometry geo\nmaterial m\n}\n"
			  "standard_object\n{\nname opb\ngeometry boxg\nmaterial m\nposition 1 0 0\n}\n"
			  "csg_object\n{\nname C\nobja opa\nobjb opb\noperation union\n}\n";
		const std::string scene = Scene( csgSrc
			+ "standard_object\n{\nname I\nsource C\n}\n"
			+ "override_object\n{\nname I\nposition 0 2 0\n}\n" );
		const std::string path = WriteTempScene( "cst_source_instance_override.RISEscene", scene );
		Job* j = new Job();
		const bool loaded = j->LoadAsciiSceneViaCst( path.c_str() );
		Check( loaded, "override: the csg-sourced-instance-plus-override fixture loads with a retained CST head" );
		if( loaded ) {
			Check( j->CstObjectTransformKind( "I" ) == 2,
			       "override: (precondition) the instance still routes to COMPONENTS (2)" );
			const double before = CenterX( Obj( j, "I" ) );
			const int rc = j->ApplyCstObjectComponentsEdit( "I", "5 0 0", "0 0 0" );
			Check( rc >= 1, "override: the components commit reports success" );
			// THE ASSERTION THAT WAS RED.  rc alone said SUCCESS while the object had not moved:
			// `position 5 0 0` landed on the base instancing chunk and the full re-derive then
			// re-applied the override's `position 0 2 0` straight over it.
			Check( std::fabs( ( CenterX( Obj( j, "I" ) ) - before ) - 5.0 ) < 1e-6,
			       "override: ... and the object really MOVED by +5 in x -- the commit reached the override_object, not the chunk under it" );
		}
		j->release();
		std::remove( path.c_str() );
	}

	// [gizmo] the same walk on an AUTHORED csg_object -- the shape c18e54b6 correctly described as
	// a pre-existing gap.  It is closed by the same code, so pin it here too.
	//
	// AND the `scale` question the walk raises, ON THE ONLY FIXTURE THAT CAN REACH IT.  Round 3
	// kept `scale` on an override_object (stripping it only from a base chunk), reasoning that the
	// override's per-field branch applies position, orientation and scale independently so a scale
	// there is live on the object being dragged.  It pinned that with `scale 2 2 2` -- a value that
	// CANNOT REACH the code under test through production at all: the commit's caller derives its
	// position/orientation from DecomposeRigid, which refuses any non-unit column magnitude, and
	// the editor's post-mutate gate restores-and-refuses the gesture before the commit runs.
	// (Measured: the panel gesture on that document is refused and the object does not move.)  So
	// the old fixture reached the branch only by calling the commit DIRECTLY, and its "WITHOUT
	// un-scaling it" assertion pinned the defect rather than the behaviour.
	//
	// The scales that DO reach it are unit SIGN FLIPS -- DecomposeRigid admits `scale -1 -1 1`
	// (all magnitudes 1, det > 0) and FOLDS its 180-degree rotation into the `orientation` the
	// commit writes.  Keeping the `scale` then applies that rotation a SECOND time.  Fixture
	// re-cut to that, with the exact orientation DecomposeRigid produces for `position 5 0 0` on
	// this document -- the object must land where the live drag put it (bbox centre 4.75), not
	// half a unit past it (5.25, which is what the kept `scale` produced).
	{
		const std::string scene = Scene(
			  "standard_object\n{\nname opa\ngeometry geo\nmaterial m\n}\n"
			  "standard_object\n{\nname opb\ngeometry boxg\nmaterial m\nposition 1 0 0\n}\n"
			  "csg_object\n{\nname C\nobja opa\nobjb opb\noperation union\n}\n"
			  "override_object\n{\nname C\nscale -1 -1 1\n}\n" );
		const std::string path = WriteTempScene( "cst_source_instance_override_csg.RISEscene", scene );
		Job* j = new Job();
		const bool loaded = j->LoadAsciiSceneViaCst( path.c_str() );
		Check( loaded, "override-csg: the authored-csg-plus-override fixture loads" );
		if( loaded ) {
			// Pre-edit: opa is a unit sphere at the origin, opb a unit box at x=1, so the union
			// spans [-1, 1.5] -> centre 0.25; the sign flip mirrors it to [-1.5, 1] -> -0.25.
			const double before = CenterX( Obj( j, "C" ) );
			Check( std::fabs( before + 0.25 ) < 1e-6,
			       "override-csg: (precondition) the override's sign flip is live on the pre-edit object" );
			// `0 0 180` is what DecomposeRigid returns for T(5,0,0) * diag(-1,-1,1) -- the local
			// matrix the live drag leaves behind.  Passing anything else would test a pose the
			// production caller never produces.
			Check( j->ApplyCstObjectComponentsEdit( "C", "5 0 0", "0 0 180" ) >= 1,
			       "override-csg: the components commit on an overridden authored csg_object reports success" );
			Check( std::fabs( ( CenterX( Obj( j, "C" ) ) - before ) - 5.0 ) < 1e-6,
			       "override-csg: ... and the object really MOVED by +5 in x -- to 4.75, where the drag put it.  "
			       "Keeping the override's `scale` re-applies the flip the written `orientation` already carries "
			       "and lands it at 5.25 instead" );
		}
		j->release();
		std::remove( path.c_str() );
	}

	// ---------------------------------------------------------------- the refusal an AUTHOR reads
	// [gizmo] A SCALE on a csg-sourced instance is correctly refused -- but the reason the author
	// is handed has to be about the scene they WROTE.  "csg_object has no scale param" names a
	// chunk type that appears nowhere in `standard_object { name I  source C }` and never mentions
	// the `source` line that is the entire reason for the restriction.  The provenance-based
	// rewrite that sits next to it CANNOT fire here: it is gated on `instancingChunk != objectName`
	// and the collapse row is `I -> (I, C)`, so that test is false by construction.  Key on the
	// row's SOURCE field instead, which IS populated in the collapse case.
	{
		using Cat = SceneEditController::Category;
		const std::string csgSrc =
			  "standard_object\n{\nname opa\ngeometry geo\nmaterial m\n}\n"
			  "standard_object\n{\nname opb\ngeometry boxg\nmaterial m\nposition 1 0 0\n}\n"
			  "csg_object\n{\nname C\nobja opa\nobjb opb\noperation union\n}\n";
		const std::string scene = Scene( csgSrc + "standard_object\n{\nname I\nsource C\n}\n" );
		const std::string path = WriteTempScene( "cst_source_instance_refusalmsg.RISEscene", scene );
		Job* j = new Job();
		const bool loaded = j->LoadAsciiSceneViaCst( path.c_str() );
		Check( loaded, "refusal-msg: the csg-sourced-instance fixture loads" );
		if( loaded ) {
			SceneEditController c( *j, 0 );
			c.SetSelection( Cat::Object, String( "I" ) );
			const int before = pRefusalLog->MatchCount();
			Check( !c.SetPropertyForCategory( Cat::Object, String( "scale" ), String( "2 2 2" ) ),
			       "refusal-msg: a SCALE on a csg-sourced instance is refused (translate/rotate only)" );
			Check( pRefusalLog->MatchCount() == before + 1, "refusal-msg: ... and the refusal is reported to the author" );
			const std::string msg = pRefusalLog->LastMatch();
			Check( msg.find( "`source`" ) != std::string::npos && msg.find( "`C`" ) != std::string::npos,
			       "refusal-msg: ... naming the `source` line and the object it resolves to, which is what the author wrote" );
			Check( msg.find( "csg_object has no scale param -- only translate/rotate are committable" ) == std::string::npos,
			       "refusal-msg: ... NOT the bare csg_object reason, which names a chunk type this scene does not contain" );
		}
		j->release();
		std::remove( path.c_str() );
	}

	// [gizmo] CONTROL: an AUTHORED csg_object has no provenance row, so it still gets the plain
	// reason -- and there the chunk type it names IS the one the author wrote.  Pins that the
	// rewrite above is keyed on provenance rather than applied to every kind-2 refusal.
	{
		using Cat = SceneEditController::Category;
		const std::string scene = Scene(
			  "standard_object\n{\nname opa\ngeometry geo\nmaterial m\n}\n"
			  "standard_object\n{\nname opb\ngeometry boxg\nmaterial m\nposition 1 0 0\n}\n"
			  "csg_object\n{\nname C\nobja opa\nobjb opb\noperation union\n}\n" );
		const std::string path = WriteTempScene( "cst_source_instance_refusalmsg_ctl.RISEscene", scene );
		Job* j = new Job();
		const bool loaded = j->LoadAsciiSceneViaCst( path.c_str() );
		Check( loaded, "refusal-msg-control: the authored-csg fixture loads" );
		if( loaded ) {
			SceneEditController c( *j, 0 );
			c.SetSelection( Cat::Object, String( "C" ) );
			const int before = pRefusalLog->MatchCount();
			Check( !c.SetPropertyForCategory( Cat::Object, String( "scale" ), String( "2 2 2" ) ),
			       "refusal-msg-control: a SCALE on an authored csg_object is still refused" );
			Check( pRefusalLog->MatchCount() == before + 1, "refusal-msg-control: ... and reported" );
			Check( pRefusalLog->LastMatch().find( "csg_object has no scale param -- only translate/rotate are committable" ) != std::string::npos,
			       "refusal-msg-control: ... with the plain csg_object reason, which is the true one here" );
		}
		j->release();
		std::remove( path.c_str() );
	}

	// ------------------------------------------------------------------ END TO END, through the
	// ------------------------------------------------------------------ chain a user actually drives
	// EVERY transform assertion above calls Job::ApplyCstObject*Edit DIRECTLY.  That is the middle
	// of the chain, and it is exactly where the last three rounds of defects hid: the direct call
	// happily accepts inputs the real caller can never produce (`scale 2 2 2` -- see the
	// override-csg block), so a green direct-route assertion says nothing about whether a user's
	// gesture lands.  Nothing in this binary exercised
	//
	//     SceneEditController -> SceneEditor::Apply -> the DecomposeRigid post-mutate gate
	//                         -> CommitPendingCstObjectTransforms -> ApplyCstObjectComponentsEdit
	//
	// end to end and then asked WHERE THE OBJECT IS.  These do.  The assertion is on the FINAL
	// COMMITTED pose -- read back after the commit has re-derived -- because the whole defect
	// class here is "live and committed disagree, silently".
	//
	// The panel route is used to drive it: SetPropertyForCategory( Object, "position", ... ) runs
	// the live mutate, the gate, and then flushes the deferred CST commit under the same park,
	// which is the complete chain.
	//
	// THE NO-OVERRIDE AUTHORED CSG IS NOW PINNED HERE (it was deliberately absent until the
	// double-apply below it was fixed).  It is the ONLY shape in this table that reaches the
	// INCREMENTAL apply -- every other case carries a `source` or an `override_object`, each of
	// which forces a FULL re-derive that rebuilds the object from scratch and so cannot double
	// anything.  Its re-apply (Job::AddCSGObject) used to call SetPosition WITHOUT clearing the
	// transform stack the live gesture had pushed onto (SceneEdit::SetObjectPosition ->
	// Transformable::TranslateObject -> PushBottomTransStack), so FinalizeTransformations folded
	// T(5)*T(5) and `position 5 0 0` committed to 10.25.  Job::AddObject had cleared for exactly
	// this reason since review P1.1; AddCSGObject was the sibling that had not.
	{
		using Cat = SceneEditController::Category;
		const std::string csgSrc =
			  "standard_object\n{\nname opa\ngeometry geo\nmaterial m\n}\n"
			  "standard_object\n{\nname opb\ngeometry boxg\nmaterial m\nposition 1 0 0\n}\n"
			  "csg_object\n{\nname C\nobja opa\nobjb opb\noperation union\n}\n";
		struct Case {
			const char* label;      // assertion prefix
			std::string body;       // scene body
			const char* target;     // object the user selects
			double      expectX;    // bbox-centre x after the COMMIT
		};
		// Pre-edit centres: the union spans [-1, 1.5] -> 0.25; the sign-flip override mirrors it
		// to [-1.5, 1] -> -0.25.  `position 5 0 0` therefore lands at 5.25 / 4.75 respectively.
		const Case cases[] = {
			// The INCREMENTAL-apply shape -- no `source`, no `override_object`, so nothing forces a
			// full re-derive and the commit re-points the live object in place.  10.25 is the
			// double-apply; 0.25 would be a commit that never landed.
			{ "e2e-authored-csg-no-override",
			  csgSrc,                                                                                 "C", 5.25 },
			{ "e2e-authored-csg",
			  csgSrc + "override_object\n{\nname C\nposition 0 2 0\n}\n",                              "C", 5.25 },
			{ "e2e-authored-csg-signflip",
			  csgSrc + "override_object\n{\nname C\nscale -1 -1 1\n}\n",                               "C", 4.75 },
			{ "e2e-instance",
			  csgSrc + "standard_object\n{\nname I\nsource C\n}\n",                                    "I", 5.25 },
			{ "e2e-instance-override",
			  csgSrc + "standard_object\n{\nname I\nsource C\n}\n"
			           "override_object\n{\nname I\nposition 0 2 0\n}\n",                              "I", 5.25 },
			// ---- STACKED OVERRIDE LAYERS, round 5.  Both commit routes implement a LAST-WINS walk
			// over same-named `override_object` chunks -- a walk that only means anything when there
			// is more than one -- and nothing in this binary (nor, per a sweep, anywhere in tests/)
			// drove a commit through such a document.  The strip was written to match the walk and
			// went to the WRITE TARGET ONLY, so a `scale` on any layer the commit did not land on
			// stayed live, while the `orientation` the commit writes already carried the flip that
			// scale produced.  The flip was applied TWICE: live 4.75, committed 5.25, rc>=1, nothing
			// logged.  One override was correct; two were not.
			{ "e2e-stacked-signflip-first",
			  csgSrc + "override_object\n{\nname C\nscale -1 -1 1\n}\n"
			           "override_object\n{\nname C\nposition 0 0 0\n}\n",                              "C", 4.75 },
			// ... and in the MIDDLE of three.  NOTE WHAT THIS DOES AND DOES NOT PIN: the flip is on
			// the layer IMMEDIATELY BEFORE the owner here, exactly as in the two-layer case above, so
			// like that case it separates "strip the owner" from "strip more than the owner" and
			// NOTHING FURTHER.  An earlier comment claimed the `orientation 0 0 0` on layer 0 stopped
			// a "strip the one before the owner too" fix from passing.  That was false: the components
			// strip removes matrix/quaternion/scale and never `orientation`, so layer 0 is INERT and a
			// last-two-layers strip passes this case unchanged (demonstrated -- that mutation gives
			// 171 passed / 0 failed against the pre-existing suite).  The next case is the one that
			// pins the distance.
			{ "e2e-stacked-signflip-middle",
			  csgSrc + "override_object\n{\nname C\norientation 0 0 0\n}\n"
			           "override_object\n{\nname C\nscale -1 -1 1\n}\n"
			           "override_object\n{\nname C\nposition 0 0 0\n}\n",                              "C", 4.75 },
			// THE FLIP TWO LAYERS BEFORE THE OWNER -- the case that separates "strip EVERY layer" from
			// "strip the last two".  Both fixtures above sit one layer from the owner, so a strip with
			// any finite reach >= 1 satisfies them; this one is satisfied only by a strip with no reach
			// limit at all.  Under the last-two-layers mutation the surviving `scale -1 -1 1` re-applies
			// the flip that the committed `orientation` already carries and the object lands at 5.25 --
			// silently, with rc >= 1 and nothing logged, which is the exact shape of the defect this
			// whole family keeps re-presenting one layer further out.
			{ "e2e-stacked-signflip-two-before",
			  csgSrc + "override_object\n{\nname C\nscale -1 -1 1\n}\n"
			           "override_object\n{\nname C\norientation 0 0 0\n}\n"
			           "override_object\n{\nname C\nposition 0 0 0\n}\n",                              "C", 4.75 },
			// NO BASE-CHUNK CASE, and that is a finding rather than an omission.  The same pattern one
			// hop down would be a flip on the BASE under an override -- but the components route's base
			// is either an authored csg_object (no `scale` param) or a csg-SOURCED instance, and an
			// instance accepts exactly the SOURCE chunk type's params, so `scale` on it does not derive
			// ("`source C` resolves to a `csg_object` node ... which do not include `scale`").  The
			// fixture was written, and refused to load.  The commit strips the base layer anyway, for
			// uniformity with the override layers rather than for a reachable defect.
		};
		for( std::size_t k = 0; k < sizeof( cases ) / sizeof( cases[0] ); ++k ) {
			const std::string fname = std::string( "cst_source_instance_" ) + cases[k].label + ".RISEscene";
			const std::string path  = WriteTempScene( fname.c_str(), Scene( cases[k].body ) );
			Job* j = new Job();
			const bool loaded = j->LoadAsciiSceneViaCst( path.c_str() );
			Check( loaded, ( std::string( cases[k].label ) + ": the fixture loads with a retained CST head" ).c_str() );
			if( loaded ) {
				const int refusalsBefore   = pRefusalLog->MatchCount();
				const int decomposesBefore = pDecomposeLog->MatchCount();
				SceneEditController c( *j, 0 );
				c.SetSelection( Cat::Object, String( cases[k].target ) );
				Check( c.SetPropertyForCategory( Cat::Object, String( "position" ), String( "5 0 0" ) ),
				       ( std::string( cases[k].label ) + ": a translate through the CONTROLLER is accepted" ).c_str() );
				// The pose the user is left with, after the commit's re-derive rebuilt the scene.
				const double landed = CenterX( Obj( j, cases[k].target ) );
				Check( std::fabs( landed - cases[k].expectX ) < 1e-6,
				       ( std::string( cases[k].label ) + ": ... and the COMMITTED pose is where the user put it, "
				         "not where a re-derive re-decided" + Got( landed ) ).c_str() );
				// A silent refusal is the other half of the failure shape -- an accepted-looking
				// edit that logged a refusal somewhere in the chain is not a success.
				Check( pRefusalLog->MatchCount() == refusalsBefore
				    && pDecomposeLog->MatchCount() == decomposesBefore,
				       ( std::string( cases[k].label ) + ": ... with nothing refused anywhere along the chain" ).c_str() );
			}
			j->release();
			std::remove( path.c_str() );
		}
	}

	// [end-to-end] THE SECOND EDIT, on the incremental route.  One commit landing correctly is not
	// enough: the defect was LIVE STATE (the gesture's transform stack) SURVIVING the re-apply, so
	// it compounds across gestures and the drift is not even a constant factor.  Under the bug a
	// first `position 5 0 0` committed to 10.25, and a SECOND `position 3 0 0` -- whose live gesture
	// pushes T(-7) to correct for the doubled pose it is looking at -- committed to 1.25 (both
	// measured against the reverted fix), i.e. the error changed SIGN.  A fix that merely subtracted
	// one surviving stack entry out would pass the single-edit case and fail this one -- the second
	// gesture leaves TWO.  Both edits go through the controller and each assertion
	// reads the pose after that commit's re-derive.  Route coverage -- that this fixture is the
	// INCREMENTAL apply (rc=1) rather than a full re-derive -- is pinned by `gizmo-control` above.
	{
		using Cat = SceneEditController::Category;
		const std::string scene = Scene(
			  "standard_object\n{\nname opa\ngeometry geo\nmaterial m\n}\n"
			  "standard_object\n{\nname opb\ngeometry boxg\nmaterial m\nposition 1 0 0\n}\n"
			  "csg_object\n{\nname C\nobja opa\nobjb opb\noperation union\n}\n" );
		const std::string path = WriteTempScene( "cst_source_instance_e2e_repeat.RISEscene", scene );
		Job* j = new Job();
		const bool loaded = j->LoadAsciiSceneViaCst( path.c_str() );
		Check( loaded, "e2e-repeat: the authored-csg fixture loads with a retained CST head" );
		if( loaded ) {
			const int refusalsBefore   = pRefusalLog->MatchCount();
			const int decomposesBefore = pDecomposeLog->MatchCount();
			SceneEditController c( *j, 0 );
			c.SetSelection( Cat::Object, String( "C" ) );
			Check( c.SetPropertyForCategory( Cat::Object, String( "position" ), String( "5 0 0" ) ),
			       "e2e-repeat: a translate through the CONTROLLER is accepted" );
			const double first = CenterX( Obj( j, "C" ) );
			Check( std::fabs( first - 5.25 ) < 1e-6,
			       ( std::string( "e2e-repeat: ... and the COMMITTED pose is 5.25" ) + Got( first ) ).c_str() );
			c.SetSelection( Cat::Object, String( "C" ) );   // a commit may have re-derived; re-assert the selection
			Check( c.SetPropertyForCategory( Cat::Object, String( "position" ), String( "3 0 0" ) ),
			       "e2e-repeat: a SECOND translate is accepted" );
			const double second = CenterX( Obj( j, "C" ) );
			Check( std::fabs( second - 3.25 ) < 1e-6,
			       ( std::string( "e2e-repeat: ... and lands at 3.25 -- `position` is ABSOLUTE, so the second "
			         "commit replaces the first rather than composing with what the first left on the object" )
			         + Got( second ) ).c_str() );
			Check( pRefusalLog->MatchCount() == refusalsBefore
			    && pDecomposeLog->MatchCount() == decomposesBefore,
			       "e2e-repeat: ... with nothing refused anywhere along the chain" );
		}
		j->release();
		std::remove( path.c_str() );
	}

	// [end-to-end] THE MATRIX ROUTE ACROSS STACKED LAYERS -- the other half of the coverage gap.
	// ApplyCstObjectMatrixEdit shares the last-wins OWNER WALK with the components route, and this
	// pins that walk: the `matrix` must reach the LAST layer, not the base under a later absolute
	// `position 0 0 0`.
	//
	// WHAT IT DELIBERATELY DOES NOT PIN is an every-layer strip, because this route does not perform
	// one and must not.  The surviving `scale -1 -1 1` on layer 1 is harmless here BY CONSTRUCTION,
	// not by luck: a `matrix` on the last layer takes SetFinalTransformMatrix -> ReplaceFinalStack_,
	// which clears the transform stack and the stretch outright, so every earlier layer is subsumed.
	// The fixture keeps that `scale` precisely so the fixture ITSELF is the standing demonstration --
	// the object lands at 5.0, flip and all, with the earlier layer untouched in the document.
	//
	// RED-PROOF: removing the owner walk (committing to the base chunk) reddens this -- the trailing
	// `position 0 0 0` masks the `matrix` on re-derive and the object stays at 0 while the commit
	// reports success.  It was briefly NOT red against that mutation, during the round-5 window when
	// this route also stripped every layer and so removed that masking `position` as a side effect;
	// the strip was reverted to owner-only and the walk is the single mechanism again.
	{
		using Cat = SceneEditController::Category;
		const std::string scene = Scene(
			  "standard_object\n{\nname S\ngeometry geo\nmaterial m\nposition 9 9 9\n}\n"
			  "override_object\n{\nname S\nscale -1 -1 1\n}\n"
			  "override_object\n{\nname S\nposition 0 0 0\n}\n" );
		const std::string path = WriteTempScene( "cst_source_instance_e2e_matrix_stacked.RISEscene", scene );
		Job* j = new Job();
		const bool loaded = j->LoadAsciiSceneViaCst( path.c_str() );
		Check( loaded, "e2e-matrix-stacked: the standard_object-plus-two-overrides fixture loads" );
		if( loaded ) {
			Check( j->CstObjectTransformKind( "S" ) == 1,
			       "e2e-matrix-stacked: (precondition) a plain standard_object still routes to MATRIX (1)" );
			const int matrixRefusalsBefore = pMatrixLog->MatchCount();
			const int refusalsBefore       = pRefusalLog->MatchCount();
			const int decomposesBefore     = pDecomposeLog->MatchCount();
			SceneEditController c( *j, 0 );
			c.SetSelection( Cat::Object, String( "S" ) );
			Check( c.SetPropertyForCategory( Cat::Object, String( "position" ), String( "5 0 0" ) ),
			       "e2e-matrix-stacked: a translate through the CONTROLLER is accepted" );
			// The unit sphere is flip-invariant, so this reads the TRANSLATION alone -- which is the
			// thing the owner walk decides.  Committing to the base leaves it at 0.
			Check( std::fabs( CenterX( Obj( j, "S" ) ) - 5.0 ) < 1e-6,
			       "e2e-matrix-stacked: ... and the COMMITTED pose is where the user put it -- the `matrix` "
			       "reached the LAST override layer, not the chunk under a later `position 0 0 0`" );
			Check( pMatrixLog->MatchCount() == matrixRefusalsBefore
			    && pRefusalLog->MatchCount() == refusalsBefore
			    && pDecomposeLog->MatchCount() == decomposesBefore,
			       "e2e-matrix-stacked: ... with nothing refused anywhere along the chain" );
			// THE COST OF THE EVERY-LAYER STRIP ON *THIS* ROUTE, pinned so it cannot come back as a
			// consistency tidy-up.  Round 5 widened this strip to match the components route; the pose
			// above is BYTE-IDENTICAL either way (ReplaceFinalStack_ subsumes the earlier layers), but
			// the wide strip took the base's `position 9 9 9` and the whole of the `scale` layer with
			// it -- and an override_object declares only name + the five transform params, so that
			// layer is left as a name-only block that logs "no override parameters present" on every
			// later derive.  Not a corner case: it is EVERY non-owner override layer, always.  The two
			// params below are author-written and NOT live; surviving is exactly the point.
			const RISE::Cst::Document* doc = j->GetCstDocument();
			const std::string out = doc ? SerializeCst( *doc ) : std::string();
			Check( out.find( "position 9 9 9" ) != std::string::npos,
			       "e2e-matrix-stacked: ... and the BASE chunk's own `position` survives a commit that "
			       "lands on a later layer -- the strip is OWNER-ONLY on the matrix route" );
			Check( out.find( "scale -1 -1 1" ) != std::string::npos,
			       "e2e-matrix-stacked: ... and the non-owner override layer keeps its `scale`, so it is "
			       "not reduced to a name-only block the parser then complains about forever" );
		}
		j->release();
		std::remove( path.c_str() );
	}

	// [end-to-end] and the refusal the OTHER gate emits, on the object it actually applies to.  A
	// non-unit `scale` on the override blocks the commit -- but the blocker is the SCALE, and the
	// author ran a pure TRANSLATE.  Round 3 answered them all with one line about ROTATION and
	// GIMBAL-LOCK, for an author who neither rotated anything nor went near a singularity.
	{
		using Cat = SceneEditController::Category;
		const std::string scene = Scene(
			  "standard_object\n{\nname opa\ngeometry geo\nmaterial m\n}\n"
			  "standard_object\n{\nname opb\ngeometry boxg\nmaterial m\nposition 1 0 0\n}\n"
			  "csg_object\n{\nname C\nobja opa\nobjb opb\noperation union\n}\n"
			  "override_object\n{\nname C\nscale 2 2 2\n}\n" );
		const std::string path = WriteTempScene( "cst_source_instance_e2e_scaleblock.RISEscene", scene );
		Job* j = new Job();
		const bool loaded = j->LoadAsciiSceneViaCst( path.c_str() );
		Check( loaded, "e2e-scale-blocked: the non-unit-scale-override fixture loads" );
		if( loaded ) {
			const double before = CenterX( Obj( j, "C" ) );
			SceneEditController c( *j, 0 );
			c.SetSelection( Cat::Object, String( "C" ) );
			const int decomposesBefore = pDecomposeLog->MatchCount();
			Check( !c.SetPropertyForCategory( Cat::Object, String( "position" ), String( "5 0 0" ) ),
			       "e2e-scale-blocked: a pure TRANSLATE is refused (the override's non-unit scale is in the local matrix)" );
			Check( std::fabs( CenterX( Obj( j, "C" ) ) - before ) < 1e-6,
			       "e2e-scale-blocked: ... and the object did not move -- the refusal RESTORED it" );
			Check( pDecomposeLog->MatchCount() == decomposesBefore + 1,
			       "e2e-scale-blocked: ... and the author is told" );
			const std::string msg = pDecomposeLog->LastMatch();
			Check( msg.find( "SCALE" ) != std::string::npos,
			       "e2e-scale-blocked: ... naming the SCALE, which is the actual blocker" );
			Check( msg.find( "GIMBAL-LOCK" ) == std::string::npos && msg.find( "gimbal-lock" ) == std::string::npos,
			       "e2e-scale-blocked: ... and NOT gimbal-lock, which this author neither caused nor can act on" );
		}
		j->release();
		std::remove( path.c_str() );
	}

	// [end-to-end] CONTROL for the pair above: a genuine GIMBAL-LOCK rotation still says so.  Without
	// this, collapsing every reason to the scale wording would pass the block above.
	{
		using Cat = SceneEditController::Category;
		const std::string scene = Scene(
			  "standard_object\n{\nname opa\ngeometry geo\nmaterial m\n}\n"
			  "standard_object\n{\nname opb\ngeometry boxg\nmaterial m\nposition 1 0 0\n}\n"
			  "csg_object\n{\nname C\nobja opa\nobjb opb\noperation union\n}\n" );
		const std::string path = WriteTempScene( "cst_source_instance_e2e_gimbal.RISEscene", scene );
		Job* j = new Job();
		const bool loaded = j->LoadAsciiSceneViaCst( path.c_str() );
		Check( loaded, "e2e-gimbal-control: the plain authored-csg fixture loads" );
		if( loaded ) {
			SceneEditController c( *j, 0 );
			c.SetSelection( Cat::Object, String( "C" ) );
			const int decomposesBefore = pDecomposeLog->MatchCount();
			Check( !c.SetPropertyForCategory( Cat::Object, String( "orientation" ), String( "0 90 0" ) ),
			       "e2e-gimbal-control: a Y=90deg rotation on a csg_object is refused" );
			Check( pDecomposeLog->MatchCount() == decomposesBefore + 1, "e2e-gimbal-control: ... and reported" );
			const std::string msg = pDecomposeLog->LastMatch();
			Check( msg.find( "GIMBAL-LOCK" ) != std::string::npos,
			       "e2e-gimbal-control: ... naming GIMBAL-LOCK, which here IS the cause" );
			Check( msg.find( "SCALE" ) == std::string::npos,
			       "e2e-gimbal-control: ... and not the scale reason, which is false here" );
		}
		j->release();
		std::remove( path.c_str() );
	}

	// [end-to-end] TWO CAUSES AT ONCE, round 5.  The rejections are not mutually exclusive, and the
	// reason was first-one-wins with a trailing claim about the ones that had not been reached:
	// "a non-unit SCALE ... -- the translate/rotate itself is fine".  On `2 * Ry(90)` that claim is
	// FALSE -- remove the scale and the gesture is still refused, on gimbal-lock.  Both are named now,
	// and nothing affirms the rest of the matrix.
	{
		using Cat = SceneEditController::Category;
		const std::string scene = Scene(
			  "standard_object\n{\nname opa\ngeometry geo\nmaterial m\n}\n"
			  "standard_object\n{\nname opb\ngeometry boxg\nmaterial m\nposition 1 0 0\n}\n"
			  "csg_object\n{\nname C\nobja opa\nobjb opb\noperation union\n}\n"
			  "override_object\n{\nname C\nscale 2 2 2\n}\n" );
		const std::string path = WriteTempScene( "cst_source_instance_e2e_twocause.RISEscene", scene );
		Job* j = new Job();
		const bool loaded = j->LoadAsciiSceneViaCst( path.c_str() );
		Check( loaded, "e2e-two-cause: the scaled-override fixture loads" );
		if( loaded ) {
			SceneEditController c( *j, 0 );
			c.SetSelection( Cat::Object, String( "C" ) );
			const int decomposesBefore = pDecomposeLog->MatchCount();
			Check( !c.SetPropertyForCategory( Cat::Object, String( "orientation" ), String( "0 90 0" ) ),
			       "e2e-two-cause: a Y=90deg rotation on an object carrying a non-unit scale is refused" );
			Check( pDecomposeLog->MatchCount() == decomposesBefore + 1, "e2e-two-cause: ... and reported" );
			const std::string msg = pDecomposeLog->LastMatch();
			Check( msg.find( "GIMBAL-LOCK" ) != std::string::npos && msg.find( "SCALE" ) != std::string::npos,
			       "e2e-two-cause: ... naming BOTH defects -- this matrix has both, and un-scaling it would "
			       "still be refused" );
			Check( msg.find( "the translate/rotate itself is fine" ) == std::string::npos,
			       "e2e-two-cause: ... and affirming NOTHING about the parts of the matrix it did not clear" );
		}
		j->release();
		std::remove( path.c_str() );
	}

	// [end-to-end] and the REFLECTION reason, which the author reaches by writing a `scale` -- a
	// negative one passes the magnitude test outright, so the old wording ("negative determinant")
	// never mentioned the param they actually wrote.
	{
		using Cat = SceneEditController::Category;
		const std::string scene = Scene(
			  "standard_object\n{\nname opa\ngeometry geo\nmaterial m\n}\n"
			  "standard_object\n{\nname opb\ngeometry boxg\nmaterial m\nposition 1 0 0\n}\n"
			  "csg_object\n{\nname C\nobja opa\nobjb opb\noperation union\n}\n"
			  "override_object\n{\nname C\nscale -1 1 1\n}\n" );
		const std::string path = WriteTempScene( "cst_source_instance_e2e_reflection.RISEscene", scene );
		Job* j = new Job();
		const bool loaded = j->LoadAsciiSceneViaCst( path.c_str() );
		Check( loaded, "e2e-reflection: the mirrored-override fixture loads" );
		if( loaded ) {
			const double before = CenterX( Obj( j, "C" ) );
			SceneEditController c( *j, 0 );
			c.SetSelection( Cat::Object, String( "C" ) );
			const int decomposesBefore = pDecomposeLog->MatchCount();
			Check( !c.SetPropertyForCategory( Cat::Object, String( "position" ), String( "5 0 0" ) ),
			       "e2e-reflection: a pure TRANSLATE is refused (the override's mirror is in the local matrix)" );
			Check( std::fabs( CenterX( Obj( j, "C" ) ) - before ) < 1e-6,
			       "e2e-reflection: ... and the object did not move -- the refusal RESTORED it" );
			Check( pDecomposeLog->MatchCount() == decomposesBefore + 1, "e2e-reflection: ... and the author is told" );
			const std::string msg = pDecomposeLog->LastMatch();
			Check( msg.find( "REFLECTION" ) != std::string::npos,
			       "e2e-reflection: ... naming the REFLECTION, which is the actual blocker" );
			Check( msg.find( "`scale`" ) != std::string::npos,
			       "e2e-reflection: ... AND the `scale` param that produced it, which is what the author wrote" );
			Check( msg.find( "GIMBAL-LOCK" ) == std::string::npos,
			       "e2e-reflection: ... and not gimbal-lock, which is false here" );
		}
		j->release();
		std::remove( path.c_str() );
	}

	// [end-to-end] the SINGULAR reason, and the fact that it is reported ALONE.  Round 5's comment on
	// the early-out claimed the opposite -- "it also trips the magnitude test, so this is never the
	// ONLY thing reported" -- while the block RETURNS before the scale reason is appended.  A comment
	// asserting behaviour nobody had run is this arc's recurring defect, so the behaviour is pinned
	// here instead of described.
	//
	// The early-out is a MESSAGE-QUALITY choice, not a correctness one: delete it and `nonUnitScale`
	// (unconditionally true when a column has zero magnitude) still refuses the gesture, only with the
	// wrong words.  The second assertion is what makes that choice load-bearing -- it fails if the
	// early-out is removed and the author is told a degenerate matrix has "a non-unit SCALE".
	{
		using Cat = SceneEditController::Category;
		const std::string scene = Scene(
			  "standard_object\n{\nname opa\ngeometry geo\nmaterial m\n}\n"
			  "standard_object\n{\nname opb\ngeometry boxg\nmaterial m\nposition 1 0 0\n}\n"
			  "csg_object\n{\nname C\nobja opa\nobjb opb\noperation union\n}\n"
			  "override_object\n{\nname C\nscale 0 1 1\n}\n" );
		const std::string path = WriteTempScene( "cst_source_instance_e2e_singular.RISEscene", scene );
		Job* j = new Job();
		const bool loaded = j->LoadAsciiSceneViaCst( path.c_str() );
		Check( loaded, "e2e-singular: the zero-axis-scale-override fixture loads" );
		if( loaded ) {
			SceneEditController c( *j, 0 );
			c.SetSelection( Cat::Object, String( "C" ) );
			const int decomposesBefore = pDecomposeLog->MatchCount();
			Check( !c.SetPropertyForCategory( Cat::Object, String( "position" ), String( "5 0 0" ) ),
			       "e2e-singular: a pure TRANSLATE is refused (the override collapsed an axis)" );
			Check( pDecomposeLog->MatchCount() == decomposesBefore + 1, "e2e-singular: ... and the author is told" );
			const std::string msg = pDecomposeLog->LastMatch();
			Check( msg.find( "SINGULAR" ) != std::string::npos,
			       "e2e-singular: ... naming it SINGULAR, which describes a collapsed axis as what it is" );
			Check( msg.find( "non-unit SCALE" ) == std::string::npos,
			       "e2e-singular: ... and SINGULAR is the WHOLE message -- the early-out returns before the "
			       "scale reason, which would describe a degenerate matrix as a merely resizeable one" );
		}
		j->release();
		std::remove( path.c_str() );
	}

	// ================================================================ 87 step 3b
	// SUBTREE INSTANCING.  `source S` where S HAS CHILDREN expands into the instancing
	// chunk's own entry plus one clone per node in S's subtree, named `I.X` and parented
	// to the clone of X's parent.  Everything below is new in 3b.

	// The two fixtures every subtree assertion below builds on.  DIFFERENT geometry and
	// material on the child, so a clone that inherited the ROOT's bindings instead of the
	// child's own would be caught by the dump compare rather than passing on a coincidence.
	const std::string SUB2 = "standard_object\n{\nname S\ngeometry geo\nmaterial m\nposition 2 0 0\n}\n"
	                         "standard_object\n{\nname C\nparent S\ngeometry boxg\nmaterial m2\nposition 0 1 0\n}\n";
	const std::string SUB3 = SUB2 + "standard_object\n{\nname D\nparent C\ngeometry geo\nmaterial m\nposition 0 0 1\n}\n";

	// [subtree] A 2-LEVEL subtree expansion is INDISTINGUISHABLE from the pair of objects
	// it stands for -- same names, same bindings, same link structure, same world bboxes.
	// This is the strongest oracle in the file: DumpJob prints every object's geometry,
	// material, modifier, shader, radiance map, interior medium, visibility and WORLD
	// bounding box, so an expansion that got any of those wrong on any entry diverges.
	{
		std::vector<std::string> diags;
		const std::string got  = DumpCst( Scene( SUB2 + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ), &diags );
		const std::string want = DumpCst( Scene( SUB2
			+ "standard_object\n{\nname I\ngeometry geo\nmaterial m\nposition 5 0 0\n}\n"
			  "standard_object\n{\nname I.C\nparent I\ngeometry boxg\nmaterial m2\nposition 0 1 0\n}\n" ) );
		std::string all;
		for( std::size_t i = 0; i < diags.size(); ++i ) all += diags[i];
		Check( diags.empty(), ( std::string( "subtree: a 2-level subtree instance derives cleanly (" ) + all + ")" ).c_str() );
		Check( got == want, "subtree: the expansion == the two hand-written objects it stands for (names, bindings, links, world bboxes)" );
		// NON-VACUITY, and it is the specific wrong answer worth excluding: parenting the
		// clone to the SOURCE's parent instead of to the clone of it.  The child's LOCAL
		// transform is identical either way, so only the composed world position separates
		// them -- the clone would sit on top of the original's child at (2,1,0).
		const std::string flat = DumpCst( Scene( SUB2
			+ "standard_object\n{\nname I\ngeometry geo\nmaterial m\nposition 5 0 0\n}\n"
			  "standard_object\n{\nname I.C\nparent S\ngeometry boxg\nmaterial m2\nposition 0 1 0\n}\n" ) );
		Check( got != flat, "subtree: ... and the clone is parented to the CLONE of its parent, not to the original's parent" );
	}

	// [subtree] A 3-LEVEL subtree: the LINK STRUCTURE and the composed world transform of
	// the DEEPEST node.  A one-level test cannot separate "parented to the clone of my
	// parent" from "parented to the instance root" -- at depth 1 those are the same node.
	{
		Job* j = DeriveJob( Scene( SUB3 + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ) );
		std::string got;
		Check( CenterIs( Obj( j, "I" ), 5, 0, 0, &got ), ( "subtree3: the instance root lands at its own `position` (got " + got + ")" ).c_str() );
		Check( CenterIs( Obj( j, "I.C" ), 5, 1, 0, &got ), ( "subtree3: `I.C` composes C's local (0,1,0) onto it (got " + got + ")" ).c_str() );
		Check( CenterIs( Obj( j, "I.D" ), 5, 1, 1, &got ), ( "subtree3: `I.D` composes THROUGH `I.C` (got " + got + ")" ).c_str() );
		Check( ParentOf( j, "I" ).empty(),          "subtree3: the instance root is a root (it took no `parent` of its own)" );
		Check( ParentOf( j, "I.C" ) == "I",         "subtree3: `I.C` is parented to the instance root" );
		Check( ParentOf( j, "I.D" ) == "I.C",       "subtree3: `I.D` is parented to `I.C` -- the CLONE of D's parent, not the root" );
		// ONE LEVEL OF QUALIFICATION.  D's entry name in the source tree is `D`, so its
		// clone is `I.D`; a path-style name would be `I.C.D`, which the design rejects
		// because descendant names are already globally unique in the manager.
		Check( Obj( j, "I.C.D" ) == 0,              "subtree3: the name is `I.D`, NOT a path `I.C.D`" );
		// `source` COPIES.  The whole original subtree still renders where it was.
		Check( CenterIs( Obj( j, "S" ), 2, 0, 0 ) && CenterIs( Obj( j, "C" ), 2, 1, 0 ) && CenterIs( Obj( j, "D" ), 2, 1, 1 ),
		       "subtree3: the SOURCE subtree is untouched -- `source` copies, it does not move or hide" );
		j->release();

		// AND THE INSTANCE'S OWN `parent` STILL APPLIES.  The instancing node is a node in
		// its own right, placeable anywhere in the tree, and the clone loop must not
		// clobber the root's own link on its way to setting the clones'.  A whole
		// instanced assembly therefore composes through its host: move the host, the
		// copy and everything in it moves.
		Job* j2 = DeriveJob( Scene( "standard_object\n{\nname host\nposition 0 0 20\n}\n" + SUB3
			+ "standard_object\n{\nname I\nsource S\nparent host\nposition 5 0 0\n}\n" ) );
		std::string g2;
		Check( ParentOf( j2, "I" ) == "host", "subtree3: the instance keeps its OWN `parent`" );
		Check( CenterIs( Obj( j2, "I" ), 5, 0, 20, &g2 ), ( "subtree3: ... so the instance root composes through it (got " + g2 + ")" ).c_str() );
		Check( CenterIs( Obj( j2, "I.D" ), 5, 1, 21, &g2 ), ( "subtree3: ... and so does the deepest clone (got " + g2 + ")" ).c_str() );
		j2->release();
	}

	// [subtree][closure][incremental] EDITING A SUBTREE MEMBER, THROUGH THE PRODUCTION ENTRY POINT.
	//
	// THIS IS THE HOP THE FILE'S ONE `closure` FIXTURE DOES NOT PROVE.  That fixture asserts the
	// SOURCE hop -- `DocEditClosure(d, S)` contains `I` -- which holds because `source` is a
	// descriptor-declared Reference; it was never in doubt, and 87 step 3d cited it as the warrant
	// for deleting the document-wide incremental refusal.  The hop that needed proving is the
	// MEMBER hop, and it goes the OTHER way: the `parent` Reference runs child -> parent, so
	// NOTHING references `C`, and no amount of reference-graph work reaches `I` from it.
	//
	// STATED HONESTLY: the closure STILL cannot reach `I`, and the fix does not change that -- it
	// is a document-wide derive gate, not a closure change.  So the assertion that matters is
	// BEHAVIOURAL, driven through `Job::ApplyCstParamEdit` (the GUI panel/gizmo route) and
	// `ApplyCstParamEditChecked` (the agent route): the edit must take the FULL re-derive (rc 2,
	// not the in-place re-point's rc 1) and the CLONE must move with the member.  Before the
	// document-wide refusal was restored this returned rc=1 with zero diagnostics and left
	// `I.C` / `I.D` at their pre-edit poses -- silently, and live-only, since the saved bytes were
	// correct and a reload healed it.
	{
		const std::string scene = Scene( SUB3 + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" );

		// (a) the closure fact, recorded as it really is rather than as one would wish it.
		{
			Document d = ParseToCst( scene );
			const NodeId cid = DocFindByName( d, "standard_object/C" );
			const NodeId iid = DocFindByName( d, "standard_object/I" );
			const std::vector<NodeId> closure = DocEditClosure( d, cid );
			bool hasI = false;
			for( std::size_t k = 0; k < closure.size(); ++k ) if( closure[k] == iid ) hasI = true;
			Check( cid != 0 && iid != 0 && !hasI,
			       "member-closure: editing a subtree MEMBER does NOT put the instancing chunk in the edit closure (the `parent` edge runs child -> parent)" );
		}

		// (b) the behaviour, on the GUI route.
		{
			const std::string path = WriteTempScene( "cst_source_member_edit.RISEscene", scene );
			Job* j = new Job();
			const bool loaded = j->LoadAsciiSceneViaCst( path.c_str() );
			Check( loaded, "member-edit: the fixture loads with a retained CST head" );
			if( loaded ) {
				std::string got;
				Check( CenterIs( Obj( j, "I.C" ), 5, 1, 0, &got ), ( "member-edit: (precondition) the clone starts at C's local (got " + got + ")" ).c_str() );
				const int rc = j->ApplyCstParamEdit( "C", "standard_object", "position", 0, "0 6 0" );
				Check( rc == 2, "member-edit: the edit takes the FULL re-derive (rc=2), NOT the in-place re-point (rc=1)" );
				Check( CenterIs( Obj( j, "C" ), 2, 6, 0, &got ), ( "member-edit: the authored member moved (got " + got + ")" ).c_str() );
				// THE ASSERTION THAT WAS RED ON THE PRE-FIX TREE: the clone tracks it.
				Check( CenterIs( Obj( j, "I.C" ), 5, 6, 0, &got ), ( "member-edit: ... and the CLONE moved with it -- not stale at the pre-edit pose (got " + got + ")" ).c_str() );
				Check( CenterIs( Obj( j, "I.D" ), 5, 6, 1, &got ), ( "member-edit: ... and so did the clone BELOW it, which composes through it (got " + got + ")" ).c_str() );
			}
			j->release();
			std::remove( path.c_str() );
		}

		// (c) and the agent route, whose dry-run goes into a THROWAWAY Job -- so it diverges
		// exactly the same way and needs the same gate.  Kept separate rather than assumed.
		{
			const std::string path = WriteTempScene( "cst_source_member_edit_checked.RISEscene", scene );
			Job* j = new Job();
			const bool loaded = j->LoadAsciiSceneViaCst( path.c_str() );
			if( loaded ) {
				std::string got;
				const int rc = j->ApplyCstParamEditChecked( "C", "standard_object", "position", 0, "0 6 0" );
				Check( rc == 2, "member-edit(agent): the checked route ALSO takes the full re-derive (rc=2)" );
				Check( CenterIs( Obj( j, "I.C" ), 5, 6, 0, &got ), ( "member-edit(agent): ... and the clone is not stale (got " + got + ")" ).c_str() );
			}
			j->release();
			std::remove( path.c_str() );
		}
	}

	// [subtree][order][compose] THE TWO STRUCTURAL CLAIMS OF THE WALK, each against the
	// mutation that would violate it.  Both were unpinned: every other subtree fixture
	// has ONE child per parent (so no sibling order exists to get wrong) and every other
	// transform assertion uses pure TRANSLATIONS (which COMMUTE, so a composition that
	// transposed parent and child would land on the same number).
	//
	//   (a) SIBLING ORDER IS DOCUMENT ORDER.  The clones are registered in the order the
	//       plan lists them, and the manager's registration SERIAL is what step 2's
	//       per-parent sort reads as child display order -- so document order is a real,
	//       observable property of the expansion and not just a comment.  Reversing
	//       either child loop in `ClonePlanBuilder` reverses the serials.
	//   (b) A CHILD COMPOSES THROUGH ITS PARENT'S FULL LOCAL TRANSFORM.  `C` carries a
	//       ROTATION, so `I.D1`'s world position is `I` + `C`'s translation + R(C) applied
	//       to `D1`'s translation.  Under pure translations that is indistinguishable from
	//       composing them the other way round; with the rotation in the chain the two
	//       answers are (5,2,0) and (6,1,0), and only one of them is the tree the author
	//       wrote.
	{
		const std::string MULTI =
			"standard_object\n{\nname S\ngeometry geo\nmaterial m\nposition 2 0 0\n}\n"
			"standard_object\n{\nname C\nparent S\ngeometry boxg\nmaterial m2\nposition 0 1 0\norientation 0 0 90\n}\n"
			"standard_object\n{\nname D1\nparent C\ngeometry geo\nmaterial m\nposition 1 0 0\n}\n"
			"standard_object\n{\nname D2\nparent C\ngeometry geo\nmaterial m\nposition 0 0 1\n}\n"
			"standard_object\n{\nname E\nparent S\ngeometry geo\nmaterial m\nposition 0 -1 0\n}\n";
		std::vector<std::string> diags;
		Job* j = DeriveJob( Scene( MULTI + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ), &diags );
		std::string all;
		for( std::size_t i = 0; i < diags.size(); ++i ) all += diags[i];
		Check( diags.empty(), ( std::string( "order: the multi-sibling fixture derives cleanly (" ) + all + ")" ).c_str() );
		IObjectManager* objs = j->GetObjects();

		// (a) TWO SIBLINGS UNDER THE INSTANCE ROOT, and two under a MEMBER -- the two
		// different child loops in the walk, each with a sibling pair to order.
		Check( ParentOf( j, "I.C" ) == "I" && ParentOf( j, "I.E" ) == "I",
		       "order: (control) `I.C` and `I.E` really are siblings under the instance root" );
		Check( ParentOf( j, "I.D1" ) == "I.C" && ParentOf( j, "I.D2" ) == "I.C",
		       "order: (control) and `I.D1` / `I.D2` are siblings under `I.C`" );
		const unsigned long long sI  = objs ? objs->GetItemSerial( "I" )    : 0;
		const unsigned long long sC  = objs ? objs->GetItemSerial( "I.C" )  : 0;
		const unsigned long long sD1 = objs ? objs->GetItemSerial( "I.D1" ) : 0;
		const unsigned long long sD2 = objs ? objs->GetItemSerial( "I.D2" ) : 0;
		const unsigned long long sE  = objs ? objs->GetItemSerial( "I.E" )  : 0;
		Check( sI && sC && sD1 && sD2 && sE, "order: (control) every clone got a registration serial" );
		Check( sC < sE,   "order: siblings of the INSTANCE ROOT keep document order (`C` before `E`)" );
		Check( sD1 < sD2, "order: siblings of a MEMBER keep document order (`D1` before `D2`)" );
		// PRE-ORDER: a parent is registered before its children, which is what
		// ObjectManager::SetObjectParent's declare-before-use guard requires.
		Check( sI < sC && sC < sD1 && sD2 < sE,
		       "order: ... and the walk is PRE-ORDER, so a parent always precedes its own children" );

		// (b) NON-COMMUTING COMPOSITION.
		std::string got;
		Check( CenterIs( Obj( j, "D1" ), 2, 2, 0, &got ),
		       ( "compose: (control) the SOURCE child composes through its parent's rotation (got " + got + ")" ).c_str() );
		Check( CenterIs( Obj( j, "I.C" ), 5, 1, 0, &got ),
		       ( "compose: the cloned parent lands at its own local translation (got " + got + ")" ).c_str() );
		Check( CenterIs( Obj( j, "I.D1" ), 5, 2, 0, &got ),
		       ( "compose: and the clone's child composes THROUGH the parent's ROTATION -- (5,2,0), not the "
		         "transposed (6,1,0) that a pure-translation chain could not tell apart (got " + got + ")" ).c_str() );
		Check( CenterIs( Obj( j, "I.D2" ), 5, 1, 1, &got ),
		       ( "compose: ... and its sibling, whose local axis the same rotation leaves alone (got " + got + ")" ).c_str() );
		// The rotation reached the clone at all -- if `orientation` had been dropped from
		// the cloned member, `I.D1` would sit at (6,1,0) and `I.D2` would be unmoved, so
		// this pair separates "no rotation" from "wrong composition order" too.
		Check( !CenterIs( Obj( j, "I.D1" ), 6, 1, 0 ),
		       "compose: ... which is NOT where an unrotated or transposed chain would put it" );
		j->release();
	}

	// [provenance] every synthesized entry records the node it is a COPY OF, which is what
	// makes it traceable to something an author can edit.  `I.C -> (I, C)`: the instancing
	// chunk plus the source-side node.
	{
		Job* j = DeriveJob( Scene( SUB3 + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ) );
		IObjectManager* objs = j->GetObjects();
		const char* inst = 0; const char* src = 0;
		const bool got = objs && objs->GetObjectProvenance( "I.D", &inst, &src );
		Check( got && inst && std::string( inst ) == "I", "provenance: a synthesized entry names the INSTANCING chunk" );
		Check( got && src && std::string( src ) == "D",   "provenance: ... and the SOURCE NODE it is a copy of" );
		// The source node it names must be a LIVE entry, or "traceable" is a claim about
		// a string rather than about the scene.  DEREFERENCE THE RETURNED POINTER: with a
		// hardcoded `"D"` this asserted only that the fixture's own authored node exists,
		// which is true whatever provenance recorded.
		Check( got && src && objs && objs->GetItem( src ) != 0,
		       "provenance: ... and the name it recorded really is an entry in the manager" );
		// The source subtree's own members have NO provenance -- they were declared, not synthesized.
		const char* i2 = 0;
		Check( objs && !objs->GetObjectProvenance( "D", &i2, 0 ), "provenance: an authored subtree member has none" );
		j->release();
	}

	// [subtree][light] a `rect_light` INSIDE the subtree.  Its Finalize synthesizes FOUR
	// entities -- `<name>__pnt`, `<name>__mat`, `<name>__geo` and the object `<name>` --
	// so remapping the one `name` renames all four for free.  That is the reason clones are
	// built by re-`Finalize`ing the member's own CHUNK: an object-level clone walk would
	// duplicate the object and collide on all three helpers.
	//
	// It is also the case 3a got WRONG: its has-children scan covered only
	// `standard_object` / `csg_object`, so a source whose only child was a `rect_light`
	// passed the refusal and instanced ROOT-ONLY, silently dropping the lamp.
	{
		const std::string body = "standard_object\n{\nname S\ngeometry geo\nmaterial m\nposition 2 0 0\n}\n"
		                         "rect_light\n{\nname L\nparent S\ncenter 0 3 0\nsize 2 1\nfacing 0 -1 0\ncolor 1 1 1\nexitance 60\n}\n"
		                         "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n";
		std::vector<std::string> diags;
		Job* j = DeriveJob( Scene( body ), &diags );
		std::string all;
		for( std::size_t i = 0; i < diags.size(); ++i ) all += diags[i];
		Check( diags.empty(), ( std::string( "subtree-light: a subtree containing a rect_light expands cleanly (" ) + all + ")" ).c_str() );
		std::string got;
		Check( CenterIs( Obj( j, "I.L" ), 5, 3, 0, &got ),
		       ( "subtree-light: the lamp's object is cloned as `I.L` and rides the instance (got " + got + ")" ).c_str() );
		Check( ParentOf( j, "I.L" ) == "I", "subtree-light: ... parented to the instance root" );
		Check( j->GetGeometries() && j->GetGeometries()->GetItem( "I.L__geo" ) != 0,
		       "subtree-light: the panel GEOMETRY is renamed with it (`I.L__geo`)" );
		Check( j->GetMaterials() && j->GetMaterials()->GetItem( "I.L__mat" ) != 0,
		       "subtree-light: the luminaire MATERIAL too (`I.L__mat`)" );
		Check( j->GetPainters() && j->GetPainters()->GetItem( "I.L__pnt" ) != 0,
		       "subtree-light: and the colour PAINTER (`I.L__pnt`)" );
		// The original four are all still there -- four names, not four names moved.
		Check( Obj( j, "L" ) != 0 && j->GetGeometries()->GetItem( "L__geo" ) != 0
		    && j->GetMaterials()->GetItem( "L__mat" ) != 0 && j->GetPainters()->GetItem( "L__pnt" ) != 0,
		       "subtree-light: ... and the source lamp keeps all four of its own entities" );
		// The clone is a REAL emitter, not just an object with a name: its material carries
		// an emitter, which is what LuminaryManager collects.
		IMaterial* mat = j->GetMaterials() ? j->GetMaterials()->GetItem( "I.L__mat" ) : 0;
		Check( mat && mat->GetEmitter() != 0, "subtree-light: the cloned material is a real LUMINAIRE (it has an emitter)" );
		j->release();
	}

	// [subtree][csg] a `csg_object` INSIDE the subtree.  Re-Finalized with `obja` / `objb`
	// UNCHANGED, so the two composites SHARE their operands.
	//
	// SHARING IS CORRECT AND IT IS NOT OBVIOUS.  `CSGObject::IntersectRay` transforms the
	// world ray into the COMPOSITE's own frame BEFORE handing it to either operand, so an
	// operand's matrix is CSG-LOCAL and reads correctly relative to whichever composite is
	// asking -- two composites at different world poses get the right solid out of one
	// shared operand.  Nothing here should ever be "fixed" into a deep clone.
	//
	// An operand can never itself BE a subtree member: Job::AddCSGObject refuses an operand
	// that has a parent or children, and ObjectManager::SetObjectParent refuses it from the
	// other side -- so the walk cannot reach one.
	{
		const std::string body = "standard_object\n{\nname S\ngeometry geo\nmaterial m\nposition 2 0 0\n}\n"
		                         "standard_object\n{\nname opa\ngeometry geo\nmaterial m\n}\n"
		                         "standard_object\n{\nname opb\ngeometry boxg\nmaterial m\nposition 2 0 0\n}\n"
		                         "csg_object\n{\nname X\nparent S\nobja opa\nobjb opb\noperation union\nposition 0 2 0\n}\n";
		std::vector<std::string> diags;
		const std::string got  = DumpCst( Scene( body + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ), &diags );
		// The reference semantics, hand-written: a second csg_object over the SAME two
		// operands, parented to the instance.  DumpJob equality over the whole scene is
		// this file's render-equivalence oracle (see its header), so this is the
		// "renders equivalently" assertion.
		const std::string want = DumpCst( Scene( body
			+ "standard_object\n{\nname I\ngeometry geo\nmaterial m\nposition 5 0 0\n}\n"
			  "csg_object\n{\nname I.X\nparent I\nobja opa\nobjb opb\noperation union\nposition 0 2 0\n}\n" ) );
		std::string all;
		for( std::size_t i = 0; i < diags.size(); ++i ) all += diags[i];
		Check( diags.empty(), ( std::string( "subtree-csg: a subtree containing a csg_object expands cleanly (" ) + all + ")" ).c_str() );
		Check( got == want, "subtree-csg: the cloned composite == a hand-written csg_object over the same operands at the same pose" );

		Job* j = DeriveJob( Scene( body + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ) );
		Implementation::CSGObject* origin = dynamic_cast<Implementation::CSGObject*>( Obj( j, "X" ) );
		Implementation::CSGObject* clone  = dynamic_cast<Implementation::CSGObject*>( Obj( j, "I.X" ) );
		Check( origin && clone, "subtree-csg: the clone is itself a CSGObject (built through the SOURCE member's own chunk type)" );
		if( origin && clone ) {
			Check( clone->GetOperandA() == origin->GetOperandA() && clone->GetOperandB() == origin->GetOperandB(),
			       "subtree-csg: the operands are SHARED BY POINTER, not deep-copied" );
		}
		// NO OPERAND COPIES.  Asserting the ABSENCE of `I.opa` / `I.opb` proved nothing --
		// no code path can mint those names, so the line was green by construction.  The
		// property that IS at stake is that the clone's operands are the LIVE named
		// operands themselves, so the manager gained no operand entry to be a copy.
		if( clone ) {
			Check( clone->GetOperandA() == Obj( j, "opa" ) && clone->GetOperandB() == Obj( j, "opb" ),
			       "subtree-csg: ... and they are the LIVE `opa` / `opb` entries, so no operand copy was synthesized" );
		}
		std::string ctr;
		// (-1..1) from the sphere at the origin unioned with (1.5..2.5) from the box at
		// x=2 gives a bbox centre of x=0.75 in the composite's own frame -- deliberately
		// ASYMMETRIC, so this number cannot be reached with either operand missing (the
		// sphere alone centres at 0, the box alone at 2).
		Check( CenterIs( Obj( j, "I.X" ), 5.75, 2, 0, &ctr ),
		       ( "subtree-csg: BOTH shared operands still produce the composite at the CLONE's pose (got " + ctr + ")" ).c_str() );
		// SHARING IS N-WAY, AND BEING AN OPERAND IS RECORDED ON THE OPERAND.  Both
		// composites CONSUME `opa` / `opb`, which is what keeps them out of every
		// world-visible walk -- the TLAS / enumeration admission filter.  Removing ONE
		// composite (reachable live: the console's `remove object`) must not resurrect
		// operands the other is still consuming; with consumption held as a plain bool
		// that CSGObject's destructor unconditionally set back to true, it did, and the
		// operands then rendered as standalone shapes beside the surviving composite.
		{
			IObject* opa = Obj( j, "opa" );
			IObject* opb = Obj( j, "opb" );
			Check( opa && opb && !opa->IsWorldVisible() && !opb->IsWorldVisible(),
			       "subtree-csg: (control) both shared operands are hidden while two composites consume them" );
			Check( j->RemoveObject( "I.X" ), "subtree-csg: (control) the cloned composite can be removed" );
			Check( opa && opb && !opa->IsWorldVisible() && !opb->IsWorldVisible(),
			       "subtree-csg: ... and removing ONE composite leaves the operands hidden, `X` still consuming them" );
			Check( j->RemoveObject( "X" ), "subtree-csg: (control) and so can the original" );
			Check( opa && opb && opa->IsWorldVisible() && opb->IsWorldVisible(),
			       "subtree-csg: ... while removing the LAST one hands them back -- the count is balanced, not monotone" );
		}
		j->release();
	}

	// [subtree][nested] AN INSTANCE INSIDE AN INSTANCED SUBTREE.  This is where the naming
	// rule earns the words "one level": `I1`'s own expansion already produced `I1.A2`, so
	// instancing the container that holds `I1` adds ONE more level to a name that had one
	// -- `I2.I1` and `I2.I1.A2` -- rather than re-deriving a path from the tree shape.
	{
		const std::string body = "standard_object\n{\nname A\ngeometry geo\nmaterial m\n}\n"
		                         "standard_object\n{\nname A2\nparent A\ngeometry boxg\nmaterial m2\nposition 0 1 0\n}\n"
		                         "standard_object\n{\nname Sroot\n}\n"
		                         "standard_object\n{\nname I1\nsource A\nparent Sroot\nposition 1 0 0\n}\n"
		                         "standard_object\n{\nname I2\nsource Sroot\nposition 10 0 0\n}\n";
		std::vector<std::string> diags;
		Job* j = DeriveJob( Scene( body ), &diags );
		std::string all;
		for( std::size_t i = 0; i < diags.size(); ++i ) all += diags[i];
		Check( diags.empty(), ( std::string( "nested: an instance nested inside an instanced subtree derives cleanly (" ) + all + ")" ).c_str() );
		std::string got;
		Check( CenterIs( Obj( j, "I1" ),      1,  0, 0, &got ), ( "nested: (control) the inner instance itself (got " + got + ")" ).c_str() );
		Check( CenterIs( Obj( j, "I1.A2" ),   1,  1, 0, &got ), ( "nested: (control) and its own synthesized child (got " + got + ")" ).c_str() );
		Check( CenterIs( Obj( j, "I2.I1" ),   11, 0, 0, &got ), ( "nested: `I2.I1` -- the clone of an entry that is itself an instance (got " + got + ")" ).c_str() );
		Check( CenterIs( Obj( j, "I2.I1.A2" ),11, 1, 0, &got ), ( "nested: `I2.I1.A2` -- the clone of an already-synthesized entry (got " + got + ")" ).c_str() );
		Check( ParentOf( j, "I2.I1" ) == "I2" && ParentOf( j, "I2.I1.A2" ) == "I2.I1",
		       "nested: the cloned links follow the cloned tree" );
		// The qualification is added to the ENTRY name, so it is NOT `I2.A2`: that name
		// would claim `A2` is a direct member of what `I2` copied, which it is not.
		Check( Obj( j, "I2.A2" ) == 0, "nested: ... and NOT `I2.A2` -- the copied entry's own name already carried a level" );
		// PROVENANCE THROUGH THE NEST: each hop names a LIVE entry, so a consumer can walk
		// from a rendered pixel back to an editable chunk one step at a time.
		IObjectManager* objs = j->GetObjects();
		const char* inst = 0; const char* src = 0;
		const bool prov = objs && objs->GetObjectProvenance( "I2.I1.A2", &inst, &src );
		Check( prov && inst && std::string( inst ) == "I2", "nested: provenance names the OUTER instancing chunk" );
		Check( prov && src && std::string( src ) == "I1.A2",
		       "nested: ... and the source ENTRY it copied, which is itself a synthesized name" );
		// DEREFERENCED, not hardcoded -- the point is that whatever provenance RECORDED
		// resolves, so the trace continues from the name the consumer would actually follow.
		Check( prov && src && objs && objs->GetItem( src ) != 0,
		       "nested: ... and that name really is a live entry, so the trace continues" );
		j->release();
	}

	// [subtree][nested][order] A NESTED INSTANCE'S SIBLINGS COME OUT IN THE ORIGINAL'S
	// ORDER, which is NOT flat document order.  `I1 source A` mints `I1.A2` at `I1`'s own
	// document position; `Q parent I1` must be declared BELOW `I1` (declare-before-use),
	// so in the tree being copied the SYNTHESIZED sibling precedes the AUTHORED one.  A
	// walk that emitted document children before descending the `source` chain handed the
	// copy the reverse of the order the thing it copies has.
	{
		const std::string body = "standard_object\n{\nname A\ngeometry geo\nmaterial m\n}\n"
		                         "standard_object\n{\nname A2\nparent A\ngeometry boxg\nmaterial m2\nposition 0 1 0\n}\n"
		                         "standard_object\n{\nname Sroot\n}\n"
		                         "standard_object\n{\nname I1\nsource A\nparent Sroot\nposition 1 0 0\n}\n"
		                         "standard_object\n{\nname Q\nparent I1\ngeometry geo\nmaterial m\nposition 0 0 1\n}\n"
		                         "standard_object\n{\nname I2\nsource Sroot\nposition 10 0 0\n}\n";
		std::vector<std::string> diags;
		Job* j = DeriveJob( Scene( body ), &diags );
		std::string all;
		for( std::size_t i = 0; i < diags.size(); ++i ) all += diags[i];
		Check( diags.empty(), ( std::string( "nested-order: the fixture derives cleanly (" ) + all + ")" ).c_str() );
		IObjectManager* objs = j->GetObjects();
		Check( ParentOf( j, "I1.A2" ) == "I1" && ParentOf( j, "Q" ) == "I1",
		       "nested-order: (control) `I1.A2` and `Q` really are siblings under `I1`" );
		// THE ORDER TO MATCH, read off the ORIGINAL rather than assumed.
		const unsigned long long oA2 = objs ? objs->GetItemSerial( "I1.A2" ) : 0;
		const unsigned long long oQ  = objs ? objs->GetItemSerial( "Q" )     : 0;
		Check( oA2 && oQ && oA2 < oQ,
		       "nested-order: (control) in the ORIGINAL the synthesized sibling precedes the authored one" );
		Check( ParentOf( j, "I2.I1.A2" ) == "I2.I1" && ParentOf( j, "I2.Q" ) == "I2.I1",
		       "nested-order: the copy reproduces both siblings under the cloned instance" );
		const unsigned long long cA2 = objs ? objs->GetItemSerial( "I2.I1.A2" ) : 0;
		const unsigned long long cQ  = objs ? objs->GetItemSerial( "I2.Q" )     : 0;
		Check( cA2 && cQ && cA2 < cQ,
		       "nested-order: ... in the SAME order, so the copy's child list looks like the original's" );
		j->release();
	}

	// [collision] A SYNTHESIZED name colliding with an AUTHORED chunk, BOTH directions.
	// The two failure modes are genuinely different and neither guard covers the other.
	{
		const std::string inst = "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n";
		// (a) The authored chunk is declared AFTER the instancing chunk.  The manager
		// pre-check CANNOT see this -- the colliding object does not exist yet -- and the
		// consequence is the silent one: DocFindByNameAnyRole would resolve a pick on the
		// synthesized `I.C` to the authored chunk, so an edit lands somewhere unrelated.
		{
			std::string all;
			Check( RefusedWith( Scene( SUB2 + inst + "standard_object\n{\nname I.C\ngeometry geo\nmaterial m\n}\n" ),
			                    "the document ALREADY declares an object of that name", &all ),
			       "collision: an authored chunk named like a synthesized entry, declared AFTER, is refused by the DOCUMENT scan" );
			Check( all.find( "`I.C`" ) != std::string::npos && all.find( "chunk #" ) != std::string::npos,
			       "collision: ... naming the entry and the chunk the author has to go find" );
		}
		// (b) Declared BEFORE.  Now the object exists when the expansion runs, so the
		// manager pre-check fires first -- and its message says which SUBTREE MEMBER the
		// clashing entry was being synthesized for, which the raw AddItem duplicate-name
		// error does not.
		{
			std::string all;
			Check( RefusedWith( Scene( SUB2 + "standard_object\n{\nname I.C\ngeometry geo\nmaterial m\n}\n" + inst ),
			                    "already exists as an object", &all ),
			       "collision: ... and declared BEFORE, by the MANAGER pre-check" );
			// WHICH GUARD.  Both messages name the subtree member, so a bare
			// "for the subtree member `C`" search is satisfied by the DOCUMENT scan's
			// message too -- it stays green with the manager pre-check deleted (measured).
			// The manager one is the only one that says the object EXISTS.
			Check( all.find( "for the subtree member `C`" ) != std::string::npos,
			       "collision: ... naming the subtree member whose clone collided" );
			Check( all.find( "the document ALREADY declares" ) == std::string::npos,
			       "collision: ... and it is the MANAGER pre-check that answers, not the document scan standing in for it" );
		}
		// (c) The collider is a `rect_light`, declared AFTER.  Its object entry claims the
		// name `I.C` exactly as a `standard_object` would, but it is NOT a role a `source`
		// can name -- so a scan restricted to the source-resolvable roles misses it and the
		// clone silently mis-targets.  This is the case the separate ENTRY-name keyspace exists for.
		{
			Check( RefusedWith( Scene( SUB2 + inst
			                         + "rect_light\n{\nname I.C\ncenter 0 3 0\nsize 2 1\nfacing 0 -1 0\ncolor 1 1 1\nexitance 60\n}\n" ),
			                    "the document ALREADY declares an object of that name" ),
			       "collision: ... and a `rect_light` claiming a synthesized entry name is caught too" );
		}
	}

	// [refuse] an `override_object` layer on a subtree member.  An override is applied to
	// the LIVE object by name AFTER its base chunk, so it never reaches a clone built from
	// that base chunk -- the copy would silently carry the un-overridden pose.  Overlaying
	// the override's params onto the merge is not equivalent either (a base `matrix` plus an
	// override `position` COMPOSES on the live object but would be swallowed by
	// standard_object's matrix-wins precedence in one merged param list), so this refuses
	// rather than approximating.
	{
		std::string all;
		Check( RefusedWith( Scene( SUB2 + "override_object\n{\nname C\nposition 0 9 0\n}\n"
		                                + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ),
		                    "has an `override_object` layer", &all ),
		       "refuse: a subtree member with an `override_object` layer is refused, not silently un-overridden" );
		Check( all.find( "UN-overridden pose" ) != std::string::npos && all.find( "chunk #" ) != std::string::npos,
		       "refuse: ... saying what the copy would have been, and where the override is" );
		// AND ON A SYNTHESIZED ENTRY.  `I1 source A` mints `I1.B`; an `override_object`
		// naming `I1.B` decided that entry's pose for the ORIGINAL and would not reach a
		// copy re-Finalized from `B`'s chunk.  The member being cloned here has a
		// QUALIFIED entry name, so this is the lookup that has to key on the entry name
		// and not on the member's bare chunk name.
		{
			std::string q;
			Check( RefusedWith( Scene( "standard_object\n{\nname A\ngeometry geo\nmaterial m\n}\n"
			                         + std::string( "standard_object\n{\nname B\nparent A\ngeometry geo\nmaterial m\nposition 0 2 0\n}\n" )
			                         + "standard_object\n{\nname I1\nsource A\nposition 5 0 0\n}\n"
			                         + "override_object\n{\nname I1.B\nposition 0 4 0\n}\n"
			                         + "standard_object\n{\nname I2\nsource I1\nposition -5 0 0\n}\n" ),
			                    "has an `override_object` layer", &q ),
			       "refuse: ... and one on a SYNTHESIZED entry is refused too, keyed on the ENTRY name" );
			Check( q.find( "`I1.B`" ) != std::string::npos,
			       "refuse: ... naming the qualified entry, which is what the author wrote the override against" );
		}
		// The SOURCE ROOT's own override is irrelevant and must NOT refuse: `override_object`
		// declares only transform params, and the collapse semantics drop the source's
		// transform entirely, so nothing an override on S could say survives into the copy.
		//
		// The claim is implemented by the ABSENCE of a check, so there is no production
		// line to mutate -- which is why the assertion has to state what the exemption
		// BUYS, positively: the scene derives, and the copy carries the INSTANCE's own
		// pose rather than the override's.  Adding a root-side override refusal reddens
		// the first; letting the source's (overridden) transform survive into the copy
		// reddens the second.
		{
			std::vector<std::string> diags;
			Job* j = DeriveJob( Scene( SUB2 + "override_object\n{\nname S\nposition 9 0 0\n}\n"
			                              + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ), &diags );
			std::string d2;
			for( std::size_t i = 0; i < diags.size(); ++i ) d2 += diags[i];
			Check( diags.empty(),
			       ( std::string( "refuse: ... while an override on the source ROOT is not refused (its transform is dropped anyway) (" ) + d2 + ")" ).c_str() );
			std::string og;
			Check( CenterIs( Obj( j, "S" ), 9, 0, 0, &og ),
			       ( "refuse: ... (control) the override really did move the SOURCE (got " + og + ")" ).c_str() );
			Check( CenterIs( Obj( j, "I" ), 5, 0, 0, &og ),
			       ( "refuse: ... and the copy took its OWN position, carrying nothing the override said (got " + og + ")" ).c_str() );
			Check( CenterIs( Obj( j, "I.C" ), 5, 1, 0, &og ),
			       ( "refuse: ... and so did the subtree under it (got " + og + ")" ).c_str() );
			j->release();
		}
	}

	// [refuse] a subtree member declared AFTER the instancing chunk.  The expansion runs at
	// the instancing chunk's own document position, so a later member does not exist yet --
	// and copying "the part of the subtree that happens to precede me" is the silent partial
	// copy 3a refused to make.
	{
		std::string all;
		Check( RefusedWith( Scene( SUB2.substr( 0, SUB2.find( "standard_object\n{\nname C" ) )
		                         + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n"
		                         + "standard_object\n{\nname C\nparent S\ngeometry boxg\nmaterial m2\nposition 0 1 0\n}\n" ),
		                    "is declared AFTER this instancing chunk", &all ),
		       "refuse: a subtree member declared below the instance is refused, not silently omitted from the copy" );
		Check( all.find( "`C`" ) != std::string::npos, "refuse: ... naming the member" );
	}

	// [refuse] GONE WITH `instance_array` (87 step 3d).  The two fixtures that stood here
	// pinned the subtree walk's refusal of a generator parented into the copied subtree --
	// at depth 1, and onto a SYNTHESIZED entry.  The generator, the `generatorChildrenOf`
	// index and both refusal sites were deleted with it.  What those fixtures ALSO
	// exercised -- that the walk reads a member's fully-qualified entry name as well as its
	// bare chunk name -- is pinned independently, and more strongly, by the depth-3 CHILD
	// fixture below, which asserts which of the three key kinds carried each branch.

	// [subtree][nested] A DOCUMENT NODE PARENTED ONTO A SYNTHESIZED ENTRY is a live
	// transitive descendant and must be copied.  `I1 source A` mints `I1.B`; `X parent
	// I1.B` is then a child of `I1.B` in exactly the sense every other subtree member is
	// a child of its parent, so `I2 source I1` has to carry it.
	//
	// THE WALK IS OVER THE DOCUMENT AND THE LIVE TREE IS NOT THE SAME SHAPE.  The design
	// doc used to claim the two "describe the same tree at derive time"; they do not --
	// the live tree has an `I1.B -> X` link and the document index has NO KEY for it,
	// because `X`'s `parent` line names a name no chunk declares.  A walk that looks a
	// member's children up by its bare chunk name (`B`) finds nothing here and drops the
	// whole branch, with no diagnostic: the derive came back count=8, diags=0, and `X`
	// simply had no copy.
	{
		const std::string body = "standard_object\n{\nname A\ngeometry geo\nmaterial m\n}\n"
		                         "standard_object\n{\nname B\nparent A\ngeometry geo\nmaterial m\nposition 0 2 0\n}\n"
		                         "standard_object\n{\nname Y\nparent B\ngeometry boxg\nmaterial m2\nposition 0 0 1\n}\n"
		                         "standard_object\n{\nname I1\nsource A\nposition 5 0 0\n}\n"
		                         "standard_object\n{\nname X\nparent I1.B\ngeometry boxg\nmaterial m2\nposition 0 0 3\n}\n"
		                         "standard_object\n{\nname I2\nsource I1\nposition -5 0 0\n}\n";
		std::vector<std::string> diags;
		Job* j = DeriveJob( Scene( body ), &diags );
		std::string all;
		for( std::size_t i = 0; i < diags.size(); ++i ) all += diags[i];
		Check( diags.empty(), ( std::string( "synth-child: the fixture derives cleanly (" ) + all + ")" ).c_str() );
		// CONTROLS -- the source side, so a failure below is about the COPY and not about
		// the tree it copies.
		std::string got;
		Check( CenterIs( Obj( j, "I1.B" ), 5, 2, 0, &got ), ( "synth-child: (control) `I1.B` (got " + got + ")" ).c_str() );
		Check( CenterIs( Obj( j, "X" ),    5, 2, 3, &got ), ( "synth-child: (control) `X` hangs off it (got " + got + ")" ).c_str() );
		Check( ParentOf( j, "X" ) == "I1.B", "synth-child: (control) ... by a real link in the live tree" );
		// THE COPY.  Both branches, and the pair is the assertion: `I1.Y` is reached
		// through the key `B` (a document child of the ORIGINAL `B`, already qualified by
		// `I1.`), `X` through the key `I1.B` (a document child of the SYNTHESIZED entry,
		// carrying no qualification of its own).  A walk that tried one key and FELL BACK
		// to the other -- rather than taking both -- passes whichever assertion matches
		// the key it happened to try first and drops the other branch.
		Check( CenterIs( Obj( j, "I2.I1.Y" ), -5, 2, 1, &got ),
		       ( "synth-child: the copy carries the branch reached by the member's own name (got " + got + ")" ).c_str() );
		Check( CenterIs( Obj( j, "I2.X" ),    -5, 2, 3, &got ),
		       ( "synth-child: ... AND the branch reached by the SYNTHESIZED entry name (got " + got + ")" ).c_str() );
		Check( ParentOf( j, "I2.I1.Y" ) == "I2.I1.B" && ParentOf( j, "I2.X" ) == "I2.I1.B",
		       "synth-child: ... both parented to the clone of `I1.B`, which is the node they hung off" );
		// AND IN THE RIGHT ORDER, which is the half of the union that nothing was
		// asserting.  The union is assembled key by key -- the SYNTHESIZED key `I1.B`
		// first, the bare `B` second -- so its natural order is by KEY, not by document
		// position, and the two disagree here: `Y` is written above `X`, but `Y` is
		// found under the SECOND key.  The `std::sort` over (chunk index, prefix length)
		// is the only thing that puts them back; delete that one line and the suite is
		// otherwise entirely green while this copy's child list comes out REVERSED
		// against the original's.  Sibling order is registration-serial order, which is
		// what step 2's per-parent sort shows as child display order in the outliner --
		// so this is user-visible, not bookkeeping.
		IObjectManager* sObjs = j->GetObjects();
		const unsigned long long oY = sObjs ? sObjs->GetItemSerial( "I1.Y" )    : 0;
		const unsigned long long oX = sObjs ? sObjs->GetItemSerial( "X" )       : 0;
		const unsigned long long cY = sObjs ? sObjs->GetItemSerial( "I2.I1.Y" ) : 0;
		const unsigned long long cX = sObjs ? sObjs->GetItemSerial( "I2.X" )    : 0;
		// (control) the ORDER TO MATCH, read off the ORIGINAL rather than assumed --
		// `I1.Y` and `X` are the two children of the live entry `I1.B`.
		Check( ParentOf( j, "I1.Y" ) == "I1.B",
		       "synth-child: (control) `I1.Y` is the ORIGINAL's other child of `I1.B`" );
		Check( oY && oX && oY < oX,
		       "synth-child: (control) in the ORIGINAL the `B`-keyed sibling precedes the `I1.B`-keyed one" );
		Check( cY && cX && cY < cX,
		       "synth-child: ... and the COPY reproduces that order ACROSS THE TWO KEYS, so the union is "
		       "sorted by document position and not by which key each branch was found under" );
		// ONE LEVEL OF QUALIFICATION, applied to each branch's OWN entry name: `Y`'s entry
		// name in the source tree is already `I1.Y`, so its copy is `I2.I1.Y`; `X`'s is
		// just `X`, so its copy is `I2.X`.  Getting this from a single prefix would give
		// one of them the other's name.
		Check( Obj( j, "I2.Y" ) == 0 && Obj( j, "I2.I1.X" ) == 0,
		       "synth-child: ... each named from its OWN entry name, not from one shared prefix" );
		j->release();
	}

	// [subtree][depth-3] THE KEY UNION AT ARBITRARY DEPTH -- the claim 2917fbe2 made and
	// did not pin.  Every fixture above is at most ONE qualification level deep, where an
	// entry's key set is exactly {fully-qualified, bare}: `keys[0]` and `keys.back()` ARE
	// the whole set, so nothing in the suite could tell the union apart from a two-key
	// special case.  Add a third level and the INTERMEDIATE keys become load-bearing:
	//
	//   A / B parent A / Y parent B          -- the authored tree
	//   I1 source A                          -- mints `I1.B`, `I1.Y`
	//   X parent I1.B                        -- authored onto a 1-level synthesized entry
	//   I2 source I1                         -- mints `I2.I1.B`, `I2.I1.Y`, `I2.X`
	//   Z parent I2.I1.B                     -- authored onto a 2-level synthesized entry
	//   I3 source I2                         -- must carry ALL THREE branches
	//
	// Cloning `I2.I1.B` (ownName `B`, ctxParts {`I2`,`I1`}) the entry answers to THREE
	// document keys at once, each with its own prefix inheritance:
	//   `I2.I1.B` -> `Z` keeps nothing        -> `I3.Z`
	//   `I1.B`    -> `X` keeps `I2.`          -> `I3.I2.X`        <-- THE INTERMEDIATE
	//   `B`       -> `Y` keeps `I2.I1.`       -> `I3.I2.I1.Y`
	// Keeping only the outer two re-creates round 1's P1 exactly one level deeper, and
	// just as silently: the derive comes back clean with `I3.I2.X` and everything under
	// it simply absent.
	{
		const std::string body = "standard_object\n{\nname A\ngeometry geo\nmaterial m\n}\n"
		                         "standard_object\n{\nname B\nparent A\ngeometry geo\nmaterial m\nposition 0 2 0\n}\n"
		                         "standard_object\n{\nname Y\nparent B\ngeometry boxg\nmaterial m2\nposition 0 0 1\n}\n"
		                         "standard_object\n{\nname I1\nsource A\nposition 5 0 0\n}\n"
		                         "standard_object\n{\nname X\nparent I1.B\ngeometry boxg\nmaterial m2\nposition 0 0 3\n}\n"
		                         "standard_object\n{\nname I2\nsource I1\nposition -5 0 0\n}\n"
		                         "standard_object\n{\nname Z\nparent I2.I1.B\ngeometry boxg\nmaterial m2\nposition 0 0 -4\n}\n"
		                         "standard_object\n{\nname I3\nsource I2\nposition 0 7 0\n}\n";
		std::vector<std::string> diags;
		Job* j = DeriveJob( Scene( body ), &diags );
		std::string all;
		for( std::size_t i = 0; i < diags.size(); ++i ) all += diags[i];
		Check( diags.empty(), ( std::string( "depth3: the three-level fixture derives cleanly (" ) + all + ")" ).c_str() );
		std::string got;
		// CONTROLS -- the two-level tree `I3` copies, so a failure below is about the
		// THIRD level and not about the thing it copies.
		Check( CenterIs( Obj( j, "I2.I1.B" ), -5, 2,  0, &got ), ( "depth3: (control) `I2.I1.B` (got " + got + ")" ).c_str() );
		Check( CenterIs( Obj( j, "I2.I1.Y" ), -5, 2,  1, &got ), ( "depth3: (control) `I2.I1.Y` (got " + got + ")" ).c_str() );
		Check( CenterIs( Obj( j, "I2.X" ),    -5, 2,  3, &got ), ( "depth3: (control) `I2.X` (got " + got + ")" ).c_str() );
		Check( CenterIs( Obj( j, "Z" ),       -5, 2, -4, &got ), ( "depth3: (control) `Z` hangs off `I2.I1.B` (got " + got + ")" ).c_str() );
		Check( ParentOf( j, "Z" ) == "I2.I1.B", "depth3: (control) ... by a real link in the live tree" );
		// THE COPY: all three branches, each named from its OWN entry name.
		Check( CenterIs( Obj( j, "I3.I2.I1.B" ), 0, 9,  0, &got ), ( "depth3: the copied 3-level member (got " + got + ")" ).c_str() );
		Check( CenterIs( Obj( j, "I3.I2.I1.Y" ), 0, 9,  1, &got ),
		       ( "depth3: the branch reached by the BARE key `B` (got " + got + ")" ).c_str() );
		Check( CenterIs( Obj( j, "I3.I2.X" ),    0, 9,  3, &got ),
		       ( "depth3: MONEY ASSERTION -- the branch reached by the INTERMEDIATE key `I1.B`, which "
		         "is neither the fully-qualified name nor the bare one (got " + got + ")" ).c_str() );
		Check( CenterIs( Obj( j, "I3.Z" ),       0, 9, -4, &got ),
		       ( "depth3: the branch reached by the FULLY-QUALIFIED key `I2.I1.B` (got " + got + ")" ).c_str() );
		Check( ParentOf( j, "I3.I2.I1.Y" ) == "I3.I2.I1.B"
		    && ParentOf( j, "I3.I2.X" )    == "I3.I2.I1.B"
		    && ParentOf( j, "I3.Z" )       == "I3.I2.I1.B",
		       "depth3: ... all three parented to the clone of `I2.I1.B`, which is the node they hung off" );
		// Each branch's copy is named from ITS OWN entry name, so the three names carry
		// three DIFFERENT amounts of qualification -- a single shared prefix would give
		// at least two of them a name the other should have had.
		Check( Obj( j, "I3.I2.I1.X" ) == 0 && Obj( j, "I3.X" ) == 0
		    && Obj( j, "I3.I2.Z" ) == 0 && Obj( j, "I3.I2.Y" ) == 0,
		       "depth3: ... and from no shared prefix -- none of the mis-qualified spellings exists" );
		// SIBLING ORDER ACROSS THREE KEYS.  The union is assembled key-first (fully-
		// qualified, then intermediate, then bare), which is the exact REVERSE of the
		// document order here, so the sort is doing real work at every position.
		IObjectManager* objs = j->GetObjects();
		const unsigned long long dY = objs ? objs->GetItemSerial( "I3.I2.I1.Y" ) : 0;
		const unsigned long long dX = objs ? objs->GetItemSerial( "I3.I2.X" )    : 0;
		const unsigned long long dZ = objs ? objs->GetItemSerial( "I3.Z" )       : 0;
		const unsigned long long oY = objs ? objs->GetItemSerial( "I2.I1.Y" )    : 0;
		const unsigned long long oX = objs ? objs->GetItemSerial( "I2.X" )       : 0;
		const unsigned long long oZ = objs ? objs->GetItemSerial( "Z" )          : 0;
		// NOT AN INDEPENDENT CONTROL, and saying so is the point: two of these three
		// (`I2.I1.Y`, `I2.X`) are themselves produced by the union at depth 2, so this
		// line and the one below go red together under a broken sort.  It is asserted
		// because the depth-2 copy has to be right too, not because it isolates the
		// depth-3 claim -- the branch-EXISTENCE assertions above are what do that.
		Check( oY && oX && oZ && oY < oX && oX < oZ,
		       "depth3: the ORIGINAL's three children of `I2.I1.B` are in document order" );
		Check( dY && dX && dZ && dY < dX && dX < dZ,
		       "depth3: ... and the copy reproduces it, so the union is sorted by document position and "
		       "not by which of the THREE keys each branch was found under" );
		j->release();
	}

	// [refuse][depth-3] GONE WITH `instance_array` (87 step 3d).  This fixture parented a
	// generator onto a TWO-level synthesized entry (`I2.I1.B`) -- the one position only the
	// third-level expansion's fully-qualified key can see.  The refusal it pinned no longer
	// exists.  The KEY SET it incidentally exercised is pinned by the depth-3 CHILD fixture
	// above, which is the stronger instrument anyway: it names which branch each of the
	// three key kinds carried, rather than only asserting that some refusal fired.

	// [refuse] a RECURSIVE definition the pre-walk self-parent check CANNOT see, because
	// the instancing chunk is not a DIRECT child of its own source.  `C source M parent M2`
	// where `M2 parent M`: expanding `C` clones `M2` (fine), then walks M2's children and
	// finds `C` itself -- which would have to be copied into its own output, forever.  The
	// walk's revisit guard is the only thing that terminates this, so this scene is what
	// proves that guard is not dead code.
	{
		const std::string body = "standard_object\n{\nname M\ngeometry geo\nmaterial m\n}\n"
		                         "standard_object\n{\nname M2\nparent M\ngeometry boxg\nmaterial m2\nposition 0 1 0\n}\n"
		                         "standard_object\n{\nname C\nsource M\nparent M2\nposition 0 0 1\n}\n";
		std::string all;
		Check( RefusedWith( Scene( body ), "recursive definition", &all ),
		       "refuse: a recursion through a TRANSITIVE descendant is caught by the walk's revisit guard" );
		Check( all.find( "into its own subtree" ) != std::string::npos,
		       "refuse: ... with the walk's own message, since no single `parent`+`source` pair explains it" );
		Check( all.find( "on the SAME chunk" ) == std::string::npos,
		       "refuse: ... and NOT the pre-walk message, which describes a `parent` line this scene does not have" );
	}

	// [allow][revisit] THE REVISIT GUARD'S *POSITIVE* CLAIM -- A LEGAL REPEAT VISIT IS
	// ALLOWED.  Everything above proves the guard REFUSES a real cycle; nothing proved it
	// PERMITS the ordinary thing, because every nested fixture in this file has exactly ONE
	// instance per source on a LINEAR chain, so no chunk is ever reached twice in a single
	// expansion.  `path` is therefore a PATH (push on entry, pop on exit) and not a
	// visited-set, and the two `path.erase` lines at the tails of `ClonePlanBuilder::
	// ClonedEntry` / `SourceSubtree` are the whole of that difference.  Delete either and a
	// MAINSTREAM scene -- an assembly holding two copies of one part, then instanced itself,
	// which is the canonical kit-bash and the shape `source` exists for -- is refused with a
	// false "recursive definition" AND has its entire instance subtree silently dropped,
	// while every other assertion in this file stays green.
	//
	// Two fixtures, one per erase, each red under ITS OWN deletion:
	//
	//   (a) THE DIAMOND -- pins `ClonedEntry`'s erase.  `B parent A`, `D parent B`,
	//       `C parent A source B`.  Expanding `I source A` walks A's children in document
	//       order: `B` (pushing B, then D, popping both) and then `C`, whose own `source B`
	//       RE-ENTERS chunks B and D by a disjoint path to produce `I.C` / `I.C.D`.  The
	//       second visit is legal -- neither chunk is on the path from `I` to it.
	//   (b) TWO SIBLING INSTANCES OF ONE SOURCE -- pins `SourceSubtree`'s erase.  `P source
	//       L` and `Q source L`, both `parent A`, then `I source A`.  Expanding `I` hops
	//       across the `source` boundary onto `L` once for `P` and once for `Q`.  `L` is
	//       deliberately a LEAF so the only chunk visited twice is the one `SourceSubtree`
	//       pushes -- which is what keeps this fixture red under (b)'s deletion ALONE and
	//       green under (a)'s, so the two proofs do not collapse into one.
	//
	// The oracle is the WHOLE ENTRY SET plus the deepest composed world position: a false
	// refusal drops the instance subtree wholesale, so a per-name spot check is the weaker
	// instrument, and the deepest position is what says the re-entered branch was copied
	// through the right parent rather than merely existing.
	{
		// (a) THE DIAMOND.
		const std::string diamond =
			"standard_object\n{\nname A\ngeometry geo\nmaterial m\n}\n"
			"standard_object\n{\nname B\nparent A\ngeometry boxg\nmaterial m2\nposition 1 0 0\n}\n"
			"standard_object\n{\nname D\nparent B\ngeometry geo\nmaterial m\nposition 0 0 1\n}\n"
			"standard_object\n{\nname C\nparent A\nsource B\nposition -4 0 0\n}\n"
			"standard_object\n{\nname I\nsource A\nposition 0 5 0\n}\n";
		std::vector<std::string> diags;
		Job* j = DeriveJob( Scene( diamond ), &diags );
		std::string all;
		for( std::size_t i = 0; i < diags.size(); ++i ) all += diags[i];
		Check( diags.empty(),
		       ( std::string( "revisit-diamond: a diamond of `parent` + `source` derives CLEANLY -- the second visit "
		         "to a chunk by a disjoint path is not a cycle (" ) + all + ")" ).c_str() );
		// Five AUTHORED entries -- `A`, `B`, `D`, `C`, and `C.D` (which `C source B`
		// produces on its own, with or without `I`) -- and five SYNTHESIZED ones.
		const std::string wantSet =
			"A|B|C|C.D|D|I|I.B|I.C|I.C.D|I.D";
		const std::string gotSet = EntryNames( j );
		Check( gotSet == wantSet,
		       ( std::string( "MONEY ASSERTION (revisit-diamond): the WHOLE entry set is the ten objects the diamond "
		         "describes -- five authored, five synthesized (got " ) + gotSet + ")" ).c_str() );
		std::string got;
		Check( CenterIs( Obj( j, "I.C.D" ), -4, 5, 1, &got ),
		       ( std::string( "MONEY ASSERTION (revisit-diamond): the DEEPEST re-entered entry `I.C.D` composes "
		         "through `I.C` through `I` (got " ) + got + ")" ).c_str() );
		Check( ParentOf( j, "I.C.D" ) == "I.C" && ParentOf( j, "I.C" ) == "I" && ParentOf( j, "I.D" ) == "I.B",
		       "revisit-diamond: ... and both re-entered branches are parented to the CLONE of their parent" );
		// NON-VACUITY: the ORIGINAL diamond is untouched, so the set above is not ten copies
		// of a mistake -- `C.D` is what `C source B` produced before `I` existed at all.
		Check( CenterIs( Obj( j, "C.D" ), -4, 0, 1 ) && CenterIs( Obj( j, "D" ), 1, 0, 1 ),
		       "revisit-diamond: (control) the authored diamond still stands where it was" );
		j->release();
	}
	{
		// (b) TWO SIBLING INSTANCES OF ONE SOURCE, inside one instanced subtree.
		const std::string siblings =
			"standard_object\n{\nname A\ngeometry geo\nmaterial m\n}\n"
			"standard_object\n{\nname L\ngeometry boxg\nmaterial m2\nposition 9 9 9\n}\n"
			"standard_object\n{\nname P\nparent A\nsource L\nposition 1 0 0\n}\n"
			"standard_object\n{\nname Q\nparent A\nsource L\nposition 0 1 0\n}\n"
			"standard_object\n{\nname I\nsource A\nposition 0 0 6\n}\n";
		std::vector<std::string> diags;
		Job* j = DeriveJob( Scene( siblings ), &diags );
		std::string all;
		for( std::size_t i = 0; i < diags.size(); ++i ) all += diags[i];
		Check( diags.empty(),
		       ( std::string( "revisit-sibling: two sibling instances of ONE source, inside an instanced subtree, "
		         "derive CLEANLY (" ) + all + ")" ).c_str() );
		const std::string wantSet = "A|I|I.P|I.Q|L|P|Q";
		const std::string gotSet  = EntryNames( j );
		Check( gotSet == wantSet,
		       ( std::string( "MONEY ASSERTION (revisit-sibling): the WHOLE entry set is the seven objects the scene "
		         "describes -- the `source L` hop is taken TWICE in one expansion (got " ) + gotSet + ")" ).c_str() );
		std::string got;
		Check( CenterIs( Obj( j, "I.P" ), 1, 0, 6, &got ),
		       ( std::string( "MONEY ASSERTION (revisit-sibling): the deepest entry of the FIRST hop composes "
		         "through `I` (got " ) + got + ")" ).c_str() );
		Check( CenterIs( Obj( j, "I.Q" ), 0, 1, 6, &got ),
		       ( std::string( "MONEY ASSERTION (revisit-sibling): ... and so does the SECOND hop's, which is the one "
		         "a visited-set would have refused (got " ) + got + ")" ).c_str() );
		Check( ParentOf( j, "I.P" ) == "I" && ParentOf( j, "I.Q" ) == "I",
		       "revisit-sibling: ... both under the instance root" );
		// NON-VACUITY: `L`'s own transform is DROPPED by each instancing node (3a's rule), so
		// (9,9,9) appearing anywhere would mean the copies took the SOURCE's pose.
		Check( CenterIs( Obj( j, "P" ), 1, 0, 0 ) && CenterIs( Obj( j, "Q" ), 0, 1, 0 )
		       && CenterIs( Obj( j, "L" ), 9, 9, 9 ),
		       "revisit-sibling: (control) the two authored instances and the untouched source" );
		j->release();
	}

	// [cap] NOT TESTED HERE, and the reason is worth recording rather than leaving as a
	// gap.  The document-wide synthesized-entry budget (kMaxSynthesizedEntries, 10,000,000
	// -- the same magnitude as the per-generator `instance_array` cap it subsumed) can only
	// be crossed by actually MATERIALIZING ten million objects: the shared count validator
	// clamps each of `count_u` / `count_v` to 1e6, so the only way to spend the budget is
	// with expansions that each pass their own cap and build, and a `source` expansion's
	// plan would need a ten-million-node subtree.  A test that crossed it would take minutes and
	// gigabytes.  The arithmetic and the threading were instead verified by temporarily
	// lowering the constant and confirming a three-entry subtree refuses with the
	// source-side message -- a red/green proof in the opposite direction, run by hand.

	// [round-trip] a 3b scene round-trips byte-for-byte.  The expansion is a DERIVE-time
	// act, so the file the author wrote is the file they get back -- no synthesized chunk
	// is ever written into the document.
	{
		const std::string scene = Scene( SUB3 + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" );
		Check( SerializeCst( ParseToCst( scene ) ) == scene, "round-trip: a subtree-instancing scene round-trips byte-for-byte" );
		// AND AFTER A DERIVE.  The assertion that mattered was never tested: the pair of
		// lines here parsed and re-serialized WITHOUT deriving, so no expansion had run
		// and "the synthesized entries appear nowhere in it" was true of a document
		// nothing had had the chance to write into.  Derive the SAME document, then
		// serialize it: an expansion that appended synthesized chunks to the CST (rather
		// than only to the Job) is what this excludes.
		Document d = ParseToCst( scene );
		{
			Job* j = new Job();
			std::vector<std::string> diags;
			DeriveToJob( d, *j, &diags );
			Check( diags.empty(), "round-trip: ... (control) that document really did derive, so the expansion ran" );
			Check( Obj( j, "I.D" ) != 0, "round-trip: ... (control) and really did synthesize entries" );
			j->release();
		}
		const std::string out = SerializeCst( d );
		Check( out == scene, "round-trip: ... and DERIVING it leaves the document byte-identical" );
		Check( out.find( "I.C" ) == std::string::npos && out.find( "I.D" ) == std::string::npos,
		       "round-trip: ... so the synthesized entries appear nowhere in it" );
	}

	// [gizmo] A SYNTHESIZED entry has no chunk of its own name, so a transform edit on it
	// is not committable -- and the refusal must name the INSTANCING chunk, which is the
	// thing the author can actually move.  3a shipped this message unreachable (its collapse
	// entry always has its own chunk, so CstObjectTransformKind answered 1); 3b's clones are
	// the first entries that reach it.
	{
		using Cat = SceneEditController::Category;
		const std::string scene = Scene( SUB3 + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" );
		const std::string path = WriteTempScene( "cst_source_instance_3b_gizmo.RISEscene", scene );
		Job* j = new Job();
		const bool loaded = j->LoadAsciiSceneViaCst( path.c_str() );
		Check( loaded, "gizmo-3b: the subtree fixture loads through the retained-CST path" );
		if( loaded ) {
			Check( j->CstObjectTransformKind( "I.C" ) == 0,
			       "gizmo-3b: a synthesized entry is NOT transform-routable (it has no chunk of its own name)" );
			Check( j->CstObjectTransformKind( "I" ) == 1,
			       "gizmo-3b: ... while the instancing chunk itself is (the control that makes the line above mean something)" );
			SceneEditController c( *j, 0 );
			c.SetSelection( Cat::Object, String( "I.C" ) );
			const int before = pRefusalLog->MatchCount();
			Check( !c.SetPropertyForCategory( Cat::Object, String( "position" ), String( "0 5 0" ) ),
			       "gizmo-3b: a transform edit on a synthesized entry is refused" );
			Check( pRefusalLog->MatchCount() == before + 1, "gizmo-3b: ... and the author is told" );
			const std::string msg = pRefusalLog->LastMatch();
			Check( msg.find( "INSTANCE synthesized by `I`" ) != std::string::npos,
			       "gizmo-3b: ... naming the INSTANCING chunk, resolved through provenance" );
			Check( msg.find( "move `I` instead" ) != std::string::npos,
			       "gizmo-3b: ... and saying what to move instead" );
			Check( msg.find( "no CST `matrix` param" ) == std::string::npos,
			       "gizmo-3b: ... not the generic reason, which is true but useless for a chunk the author never wrote" );
		}
		j->release();
		std::remove( path.c_str() );
	}

	// ================================================================ 87 step 3c
	// COUNTS.  `count_u U [count_v V]` on a chunk carrying `source` repeats the WHOLE
	// instance -- root plus subtree -- U x V times, naming each repetition `I[i,j]` and
	// each of its clones `I[i,j].X`, with the instancing chunk's OWN parameters evaluated
	// per repetition over `i`/`j` (indices) and `u`/`v` (the same, normalized into [0,1]).
	//
	// THE DECISION THIS SLICE HAD TO MAKE, recorded here and pinned below: PRESENCE of a
	// count selects the repeated naming, not its VALUE.  `count_u 1` derives `I[0,0]`, NOT
	// `I`.  Two reasons, and the second is the load-bearing one: it keeps the entry names
	// byte-compatible with the `instance_array` generator 3d retired (which named its
	// one-instance case `g[0,0]`), and a count may be an `expr(...)` over a `let`, so a
	// value-keyed rule would silently re-name every entry -- dangling every `parent
	// I[0,0]` in the file -- when a constant went from 2 to 1.

	// [count][1-D] the simplest form: a leaf source repeated along one axis, with a
	// per-instance `position`.  The oracle is a dump-vs-dump compare against the two
	// hand-written objects, so bindings, visibility and world bboxes all have to match.
	{
		std::vector<std::string> diags;
		const std::string got = DumpCst( Scene( SRC_LEAF
			+ "standard_object\n{\nname I\nsource S\ncount_u 2\nposition expr(i*10) 0 0\n}\n" ), &diags );
		const std::string want = DumpCst( Scene( SRC_LEAF
			+ "standard_object\n{\nname I[0,0]\ngeometry geo\nmaterial m\nposition 0 0 0\n}\n"
			  "standard_object\n{\nname I[1,0]\ngeometry geo\nmaterial m\nposition 10 0 0\n}\n" ) );
		std::string all;
		for( std::size_t i = 0; i < diags.size(); ++i ) all += diags[i];
		Check( diags.empty(), ( std::string( "count-1d: a counted leaf instance derives cleanly (" ) + all + ")" ).c_str() );
		Check( got == want, "count-1d: `count_u 2` == the two hand-written `I[i,j]` objects it stands for" );
	}

	// [count][2-D] + [count_v] a grid, with `i` FASTEST -- the traversal order
	// `instance_array` had, so a legend or a `parent` line written against one of its
	// grids means the same thing now that 87 step 3d has replaced it.
	{
		std::vector<std::string> diags;
		Job* j = DeriveJob( Scene( SRC_LEAF
			+ "standard_object\n{\nname I\nsource S\ncount_u 2\ncount_v 3\nposition expr(i) expr(j) 0\n}\n" ), &diags );
		Check( diags.empty(), "count-2d: a 2x3 grid derives cleanly" );
		std::string got;
		Check( CenterIs( Obj( j, "I[0,0]" ), 0, 0, 0, &got ) && CenterIs( Obj( j, "I[1,2]" ), 1, 2, 0, &got ),
		       ( "count-2d: `position expr(i) expr(j) 0` places every cell of the grid (got " + got + ")" ).c_str() );
		// THE WHOLE SET, so an expansion that produced six objects with the wrong INDICES
		// (a transposed i/j, or a `[i,j]` built from the wrong pair) cannot pass on two
		// spot checks -- and an expansion that produced the right SET at the wrong POSES
		// is caught by the pair above.
		Check( EntryNames( j ) == "I[0,0]|I[0,1]|I[0,2]|I[1,0]|I[1,1]|I[1,2]|S",
		       "count-2d: the entry set is exactly the 2x3 grid plus the untouched source" );
		j->release();
	}

	// [count_v defaulting] `count_u` alone means ONE row: `count_v` defaults to 1, so the
	// j index is 0 everywhere and there is no `[i,1]`.
	{
		Job* j = DeriveJob( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\ncount_u 3\n}\n" ) );
		Check( EntryNames( j ) == "I[0,0]|I[1,0]|I[2,0]|S",
		       "count_v-default: `count_u 3` alone is a single row -- count_v defaults to 1" );
		j->release();
	}

	// [count==1] THE DECISION, PINNED.  `count_u 1` is the REPEATED form with one
	// repetition: the entry is `I[0,0]` and there is no `I`.  Keying the naming on the
	// VALUE instead would make this scene derive `I` -- and every `parent I[0,0]` written
	// against it would dangle the day the count changed.
	{
		Job* j = DeriveJob( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\ncount_u 1\nposition 5 0 0\n}\n" ) );
		Check( Obj( j, "I[0,0]" ) != 0, "count==1: `count_u 1` names its one entry `I[0,0]`" );
		Check( Obj( j, "I" ) == 0,      "count==1: ... and NOT `I` -- presence of the count selects the naming, not its value" );
		std::string got;
		Check( CenterIs( Obj( j, "I[0,0]" ), 5, 0, 0, &got ), ( "count==1: ... and it is placed by its own `position` (got " + got + ")" ).c_str() );
		j->release();
		// The CONTROL that makes the pair above mean something: the same chunk with NO
		// count is still plain `I`, exactly as 3a/3b derived it.
		Job* j2 = DeriveJob( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n" ) );
		Check( Obj( j2, "I" ) != 0 && Obj( j2, "I[0,0]" ) == 0,
		       "count==1: (control) the count-less form is untouched -- plain `I`, no `[0,0]`" );
		j2->release();
	}

	// [count==0] a zero count produces NO entries.  The shared count validator has always
	// admitted a zero count (non-negative, integral), and 3c inherits it verbatim rather
	// than inventing a different rule for the same word.
	{
		std::vector<std::string> diags;
		Job* j = DeriveJob( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\ncount_u 0\n}\n" ), &diags );
		Check( diags.empty() && EntryNames( j ) == "S",
		       "count==0: a zero count derives cleanly and synthesizes nothing" );
		j->release();
	}

	// [count][long name] THE REPETITION-ROOT NAME IS BUILT WITH std::string, NOT A FIXED
	// BUFFER -- and the collision scan is why this is a correctness assertion rather than a
	// tidiness one.  That scan does NOT put every repetition's base into one `planned` set;
	// it argues instead that "distinct (i,j) give distinct bases by construction".  A
	// truncating `snprintf` into `char[256]` retires that argument: at 253 characters the
	// `[0,0]` and `[0,1]` suffixes are both cut to `[0`, the scan sees no collision because
	// it never compares them, and the SECOND repetition dies at AddItem -- reported as an
	// apply failure, with the first repetition already in the Job.
	//
	// 253 rather than 300 on purpose: at 300 the suffix vanishes ENTIRELY and every
	// repetition collapses onto one name, which is a louder failure.  253 is the quiet one,
	// where the name is JUST long enough for the truncation to eat the index and nothing
	// else.
	{
		const std::string LONG( 253, 'N' );
		std::vector<std::string> diags;
		Job* j = DeriveJob( Scene( SRC_LEAF
			+ "standard_object\n{\nname " + LONG + "\nsource S\ncount_u 1\ncount_v 2\n}\n" ), &diags );
		std::string all;
		for( std::size_t k = 0; k < diags.size(); ++k ) all += diags[k];
		Check( diags.empty(), ( "long-name: a 253-character instance name derives cleanly (" + all + ")" ).c_str() );
		Check( Obj( j, ( LONG + "[0,0]" ).c_str() ) != 0 && Obj( j, ( LONG + "[0,1]" ).c_str() ) != 0,
		       "long-name: ... and BOTH repetitions exist -- the [i,j] suffix is not truncated away" );
		j->release();
	}

	// [count][u/v] the NORMALIZED instance variables, and the divide-by-zero they must not
	// do.  `u = i/(count_u-1)` across a row of three is 0, 0.5, 1; with a count of ONE it
	// is 0, not a 0/0 NaN.  Both came across from `instance_array` unchanged.
	{
		Job* j = DeriveJob( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\ncount_u 3\nposition expr(u) 0 0\n}\n" ) );
		std::string got;
		Check( CenterIs( Obj( j, "I[0,0]" ), 0,   0, 0, &got )
		    && CenterIs( Obj( j, "I[1,0]" ), 0.5, 0, 0, &got )
		    && CenterIs( Obj( j, "I[2,0]" ), 1,   0, 0, &got ),
		       ( "u/v: `expr(u)` runs 0, 0.5, 1 across a row of three (got " + got + ")" ).c_str() );
		j->release();
		Job* j2 = DeriveJob( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\ncount_u 1\nposition expr(u) expr(v) 0\n}\n" ) );
		Check( CenterIs( Obj( j2, "I[0,0]" ), 0, 0, 0, &got ),
		       ( "u/v: a count of ONE gives u=v=0, not a 0/0 NaN (got " + got + ")" ).c_str() );
		j2->release();
		// `v` ON ITS OWN AXIS, AND THIS IS THE ASSERTION THE `v` LINE HAS.  The two above
		// do not have it: BOTH read `v` only where every wrong implementation also answers
		// 0 (the `count_u 1` scene has no `count_v` at all).  A `2 x 3` grid separates
		// every near-miss the two adjacent normalization lines invite -- `v = 0`, an
		// off-by-one `j/count_v` (which would put `I[0,1]` at 1/3), and the copy-paste
		// `v = i/(count_u-1)` (which would put it at 0 and `I[1,2]` at 1 for the wrong
		// reason).  `I[0,1]` pins j WITH i held at 0; `I[1,2]` pins the far corner, so a
		// transposed pair cannot satisfy both.
		//
		// IT HAS TO LIVE HERE, on the `source` path.  Before 3c review round 1 added it the
		// only `v`-discriminating fixtures in the tree were on the `instance_array` copy of
		// this arithmetic -- which 87 step 3d has since deleted, along with that copy.
		Job* j3 = DeriveJob( Scene( SRC_LEAF
			+ "standard_object\n{\nname I\nsource S\ncount_u 2\ncount_v 3\nposition expr(u) expr(v) 0\n}\n" ) );
		Check( CenterIs( Obj( j3, "I[0,1]" ), 0, 0.5, 0, &got ),
		       ( "u/v: `v` is normalized over its OWN axis -- j=1 of count_v 3 is 0.5 (got " + got + ")" ).c_str() );
		Check( CenterIs( Obj( j3, "I[1,2]" ), 1, 1, 0, &got ),
		       ( "u/v: ... and the far corner is u=v=1, so u and v are not transposed (got " + got + ")" ).c_str() );
		j3->release();
	}

	// [count][expr on orientation / scale] THE OTHER TWO TRANSFORM PARAMS, per component.
	//
	// A COVERAGE GAP RE-FILLED, and named as such: `tests/CstInstanceArrayTest.cpp` carried
	// "passthrough: orientation/scale per-component eval == hand-written", and 87 step 3d
	// deleted that file with its subject without writing the `source` twin -- leaving EVERY
	// per-instance-expression fixture in this file on `position`, `count_u` or `material`.
	// `EvalInstanceValue` is generic over the param name, so this is a pass-through claim, and
	// a pass-through claim is exactly the kind that survives on one param and quietly stops
	// being true on another (`orientation` and `scale` reach `standard_object` through
	// different bag slots than `position`, and `matrix` precedence sits between them).
	// A BOX SOURCE, AND A 45-DEGREE ANGLE, BOTH ON PURPOSE.  DumpJob's only transform-sensitive
	// field is the world bbox, and the deleted `instance_array` fixture rotated a SPHERE by 90
	// degrees -- a bbox-invariant mutation on a bbox-invariant shape, so its `orientation` half
	// asserted nothing at all.  A unit box at 45 degrees widens its bbox to sqrt(2), which the
	// dump does see.
	{
		const std::string SRC_BOX = "standard_object\n{\nname SB\ngeometry boxg\nmaterial m\nposition 0 0 0\n}\n";
		std::vector<std::string> diags;
		const std::string got = DumpCst( Scene( SRC_BOX
			+ "standard_object\n{\nname I\nsource SB\ncount_u 2\norientation expr(i*45) 0 0\nscale expr(u+1) 1 1\n}\n" ), &diags );
		const std::string want = DumpCst( Scene( SRC_BOX
			+ "standard_object\n{\nname I[0,0]\ngeometry boxg\nmaterial m\norientation 0 0 0\nscale 1 1 1\n}\n"
			  "standard_object\n{\nname I[1,0]\ngeometry boxg\nmaterial m\norientation 45 0 0\nscale 2 1 1\n}\n" ) );
		std::string all;
		for( std::size_t i = 0; i < diags.size(); ++i ) all += diags[i];
		Check( diags.empty(), ( std::string( "expr-passthrough: the fixture derives cleanly (" ) + all + ")" ).c_str() );
		Check( got == want, "expr-passthrough: `orientation` / `scale` evaluate PER COMPONENT, exactly like `position`" );
		// NON-VACUITY, SEPARATELY PER PARAM -- one compare against one wrong variant cannot say
		// WHICH of the two carried the claim.  `scale expr(u+1)` doubles the second cell's x
		// extent; `orientation expr(i*45)` widens its y/z.  Each is visible in the dumped bbox.
		const std::string noScale = DumpCst( Scene( SRC_BOX
			+ "standard_object\n{\nname I[0,0]\ngeometry boxg\nmaterial m\norientation 0 0 0\nscale 1 1 1\n}\n"
			  "standard_object\n{\nname I[1,0]\ngeometry boxg\nmaterial m\norientation 45 0 0\nscale 1 1 1\n}\n" ) );
		Check( got != noScale, "expr-passthrough: ... and the `scale` expr alone really moves the dump" );
		const std::string noRot = DumpCst( Scene( SRC_BOX
			+ "standard_object\n{\nname I[0,0]\ngeometry boxg\nmaterial m\norientation 0 0 0\nscale 1 1 1\n}\n"
			  "standard_object\n{\nname I[1,0]\ngeometry boxg\nmaterial m\norientation 0 0 0\nscale 2 1 1\n}\n" ) );
		Check( got != noRot, "expr-passthrough: ... and so does the `orientation` expr alone -- the half the deleted sphere fixture could not see" );
	}

	// [count][expr on a NON-transform param] the per-instance expression applies to the
	// instancing chunk's parameters, NOT only to its transform: `material expr(i)` selects
	// a different material per repetition.  (Numerically-named materials because an expr
	// evaluates to a NUMBER -- which is exactly what makes this a real test of the
	// reference slot rather than of the transform path.)
	{
		const std::string MATS = "lambertian_material\n{\nname 0\nreflectance p\n}\n"
		                         "lambertian_material\n{\nname 1\nreflectance p2\n}\n";
		std::vector<std::string> diags;
		const std::string got = DumpCst( Scene( MATS + SRC_LEAF
			+ "standard_object\n{\nname I\nsource S\ncount_u 2\nmaterial expr(i)\nposition expr(i*4) 0 0\n}\n" ), &diags );
		const std::string want = DumpCst( Scene( MATS + SRC_LEAF
			+ "standard_object\n{\nname I[0,0]\ngeometry geo\nmaterial 0\nposition 0 0 0\n}\n"
			  "standard_object\n{\nname I[1,0]\ngeometry geo\nmaterial 1\nposition 4 0 0\n}\n" ) );
		std::string all;
		for( std::size_t i = 0; i < diags.size(); ++i ) all += diags[i];
		Check( diags.empty(), ( std::string( "expr-material: the fixture derives cleanly (" ) + all + ")" ).c_str() );
		Check( got == want, "expr-material: `material expr(i)` binds a DIFFERENT material per repetition" );
		// NON-VACUITY: the two materials really are distinguishable in the dump (they carry
		// different painters), so the compare above would have caught both cells taking one.
		const std::string same = DumpCst( Scene( MATS + SRC_LEAF
			+ "standard_object\n{\nname I[0,0]\ngeometry geo\nmaterial 0\nposition 0 0 0\n}\n"
			  "standard_object\n{\nname I[1,0]\ngeometry geo\nmaterial 0\nposition 4 0 0\n}\n" ) );
		Check( got != same, "expr-material: ... and the dump really does separate the two materials" );
	}

	// [count][subtree] A COUNTED SUBTREE -- the entry NAMES and the LINK STRUCTURE.  Every
	// repetition carries its own copy of the whole subtree, each clone parented to the
	// CLONE OF ITS OWN PARENT within that repetition: `I[1,0].D` hangs off `I[1,0].C`, not
	// off the repetition root and not off some other repetition's `C`.
	{
		std::vector<std::string> diags;
		Job* j = DeriveJob( Scene( SUB3
			+ "standard_object\n{\nname I\nsource S\ncount_u 2\nposition expr(i*10) 0 0\n}\n" ), &diags );
		std::string all;
		for( std::size_t i = 0; i < diags.size(); ++i ) all += diags[i];
		Check( diags.empty(), ( std::string( "count-subtree: a counted 3-level subtree derives cleanly (" ) + all + ")" ).c_str() );
		Check( EntryNames( j ) == "C|D|I[0,0]|I[0,0].C|I[0,0].D|I[1,0]|I[1,0].C|I[1,0].D|S",
		       "count-subtree: the entry set is TWO whole copies of the subtree, named `I[i,j].X`" );
		Check( ParentOf( j, "I[0,0].C" ) == "I[0,0]" && ParentOf( j, "I[1,0].C" ) == "I[1,0]",
		       "count-subtree: each repetition's direct child hangs off THAT repetition's root" );
		Check( ParentOf( j, "I[0,0].D" ) == "I[0,0].C" && ParentOf( j, "I[1,0].D" ) == "I[1,0].C",
		       "count-subtree: and the grandchild hangs off the CLONE of its own parent, per repetition" );
		Check( ParentOf( j, "I[1,0]" ).empty(), "count-subtree: (control) a repetition root took no parent of its own" );
		// THE COMPOSED POSE, which is what a wrong link would move.  A grandchild parented
		// to the repetition ROOT instead of to `I[i,j].C` would sit at (10,0,1) rather than
		// (10,1,1); one parented to the FIRST repetition's clone would sit at (0,1,1).
		std::string got;
		Check( CenterIs( Obj( j, "I[1,0]" ),   10, 0, 0, &got ), ( "count-subtree: repetition 1's root (got " + got + ")" ).c_str() );
		Check( CenterIs( Obj( j, "I[1,0].C" ), 10, 1, 0, &got ), ( "count-subtree: ... its child composes through it (got " + got + ")" ).c_str() );
		Check( CenterIs( Obj( j, "I[1,0].D" ), 10, 1, 1, &got ), ( "count-subtree: ... and its grandchild through BOTH (got " + got + ")" ).c_str() );
		Check( CenterIs( Obj( j, "I[0,0].D" ), 0,  1, 1, &got ), ( "count-subtree: repetition 0's grandchild is independent of it (got " + got + ")" ).c_str() );
		// PRE-ORDER, per repetition: a parent is registered before its children (what
		// SetObjectParent's declare-before-use guard needs), and repetition 0 wholly
		// precedes repetition 1.
		IObjectManager* objs = j->GetObjects();
		const unsigned long long s00  = objs ? objs->GetItemSerial( "I[0,0]" )   : 0;
		const unsigned long long s00C = objs ? objs->GetItemSerial( "I[0,0].C" ) : 0;
		const unsigned long long s00D = objs ? objs->GetItemSerial( "I[0,0].D" ) : 0;
		const unsigned long long s10  = objs ? objs->GetItemSerial( "I[1,0]" )   : 0;
		Check( s00 && s00C && s00D && s10, "count-subtree: (control) every entry got a registration serial" );
		Check( s00 < s00C && s00C < s00D && s00D < s10,
		       "count-subtree: the walk is PRE-ORDER within a repetition, and repetitions are emitted in order" );
		j->release();
	}

	// [count][parent] EVERY repetition keeps the instancing chunk's OWN `parent`, so a
	// whole counted grid composes through its host: move the host, the grid moves.
	{
		Job* j = DeriveJob( Scene( "standard_object\n{\nname host\nposition 0 0 20\n}\n" + SUB2
			+ "standard_object\n{\nname I\nsource S\nparent host\ncount_u 2\nposition expr(i*10) 0 0\n}\n" ) );
		Check( ParentOf( j, "I[0,0]" ) == "host" && ParentOf( j, "I[1,0]" ) == "host",
		       "count-parent: every repetition root keeps the instancing chunk's own `parent`" );
		std::string got;
		Check( CenterIs( Obj( j, "I[1,0]" ),   10, 0, 20, &got ), ( "count-parent: ... so it composes through the host (got " + got + ")" ).c_str() );
		Check( CenterIs( Obj( j, "I[1,0].C" ), 10, 1, 20, &got ), ( "count-parent: ... and so does its subtree (got " + got + ")" ).c_str() );
		j->release();
	}

	// [count][collision] A COUNTED entry name colliding with an AUTHORED chunk, in BOTH
	// declaration orders.  A counted root's name is SYNTHESIZED like a clone's, so it takes
	// the clone's "declared at all" test rather than the count-less root's "declared more
	// than once" one -- and the DOCUMENT half is the one the manager pre-check structurally
	// cannot see, because the colliding chunk may not exist yet.
	{
		const std::string collider = "standard_object\n{\nname I[1,0]\ngeometry boxg\nmaterial m2\n}\n";
		const std::string inst     = "standard_object\n{\nname I\nsource S\ncount_u 2\n}\n";
		std::string all;
		Check( RefusedWith( Scene( SRC_LEAF + collider + inst ), "already exists as an object", &all ),
		       ( "count-collision: an authored chunk declared BEFORE the instance is caught by the manager pre-check (got: " + all + ")" ).c_str() );
		Check( RefusedWith( Scene( SRC_LEAF + inst + collider ), "the document ALREADY declares an object of that name", &all ),
		       ( "count-collision: ... and one declared AFTER it by the DOCUMENT scan, which the pre-check cannot see (got: " + all + ")" ).c_str() );
		Check( all.find( "`I[1,0]`" ) != std::string::npos, "count-collision: ... naming the exact synthesized entry that clashes" );
	}

	// [count][collision] the chunk NAME declared twice.  Provenance maps every entry back to
	// that one name, so two chunks holding it send a picked instance to whichever the lookup
	// finds first.  Under counts the name is not an entry, so the refusal says "the name"
	// rather than "the entry name" -- the advice is the same, the noun is not.
	{
		std::string all;
		Check( RefusedWith( Scene( SRC_LEAF
			+ "standard_object\n{\nname I\ngeometry boxg\nmaterial m2\n}\n"
			  "standard_object\n{\nname I\nsource S\ncount_u 2\n}\n" ), "declared by MORE THAN ONE object chunk", &all ),
		       "count-collision: a twice-declared chunk NAME is refused under counts too" );
		Check( all.find( "the name `I` is declared" ) != std::string::npos
		    && all.find( "the entry name `I` is declared" ) == std::string::npos,
		       "count-collision: ... and says `the name`, since under counts `I` is not itself an entry" );
	}

	// [count][provenance] BOTH shapes, which is what makes a counted entry traceable back
	// to editable text.  `I[i,j] -> (I, S)`: the chunk to edit and the node it copies.
	// `I[i,j].X -> (I, X)`: the same chunk, and the SOURCE-SIDE node this clone is a copy
	// of -- a live entry, so a consumer that follows the hop lands on something real.
	{
		Job* j = DeriveJob( Scene( SUB3 + "standard_object\n{\nname I\nsource S\ncount_u 2\n}\n" ) );
		IObjectManager* objs = j->GetObjects();
		const char* inst = 0; const char* src = 0;
		const bool gotRoot = objs && objs->GetObjectProvenance( "I[1,0]", &inst, &src );
		Check( gotRoot && inst && std::string( inst ) == "I", "count-provenance: `I[1,0]` names the INSTANCING chunk `I`" );
		Check( gotRoot && src && std::string( src ) == "S",   "count-provenance: ... and the source node `S`" );
		const char* inst2 = 0; const char* src2 = 0;
		const bool gotKid = objs && objs->GetObjectProvenance( "I[1,0].D", &inst2, &src2 );
		Check( gotKid && inst2 && std::string( inst2 ) == "I", "count-provenance: `I[1,0].D` names the same instancing chunk" );
		Check( gotKid && src2 && std::string( src2 ) == "D",   "count-provenance: ... and the subtree node `D` it is a copy of" );
		Check( gotKid && src2 && objs && objs->GetItem( src2 ) != 0,
		       "count-provenance: ... which really is a live entry in the manager" );
		// EVERY repetition's row points at the ONE chunk -- the property
		// AgentSession::ResolveIsolateObject reads to answer "isolate `I`" with the list of
		// entries `I` produced.  A per-repetition provenance chunk name would break it.
		int rows = 0;
		const char* c = 0;
		if( objs ) {
			const char* every[] = { "I[0,0]", "I[0,0].C", "I[0,0].D", "I[1,0]", "I[1,0].C", "I[1,0].D" };
			for( std::size_t k = 0; k < sizeof(every)/sizeof(every[0]); ++k )
				if( objs->GetObjectProvenance( every[k], &c, 0 ) && c && std::string( c ) == "I" ) ++rows;
		}
		Check( rows == 6, "count-provenance: all six synthesized entries trace to the single chunk `I`" );
		j->release();
	}

	// [count][provenance consumers] the two provenance readers a test in this binary can
	// drive, both on a COUNTED entry.  (The third, AgentSession::ResolveIsolateObject, reads
	// the same rows the block above asserts and is unchanged by 3c.)
	{
		using Cat = SceneEditController::Category;
		const std::string scene = Scene( SUB2 + "standard_object\n{\nname I\nsource S\ncount_u 2\n}\n" );
		const std::string path = WriteTempScene( "cst_source_instance_3c_prov.RISEscene", scene );
		Job* j = new Job();
		const bool loaded = j->LoadAsciiSceneViaCst( path.c_str() );
		Check( loaded, "count-consumers: the counted fixture loads through the retained-CST path" );
		if( loaded ) {
			SceneEditController c( *j, 0 );
			// (1) ResolveSourceChunkId, reached through ResolveSourceSpan: a counted entry
			// has no chunk of its own name, so it must trace through provenance to `I`'s
			// chunk.  Compared against the span `I` itself resolves to -- an absolute byte
			// offset would pin the fixture's layout rather than the resolution.
			SceneEditController::SourceSpan viaEntry, viaChunk, viaClone;
			const bool okEntry = c.ResolveSourceSpan( Cat::Object, String( "I[1,0]" ), String( "source" ), 0, viaEntry );
			const bool okChunk = c.ResolveSourceSpan( Cat::Object, String( "I" ),      String( "source" ), 0, viaChunk );
			const bool okClone = c.ResolveSourceSpan( Cat::Object, String( "I[1,0].C" ), String( "source" ), 0, viaClone );
			Check( okChunk, "count-consumers: (control) the instancing chunk's own `source` line resolves to a span" );
			Check( okEntry && viaEntry.byteOffset == viaChunk.byteOffset && viaEntry.byteLength == viaChunk.byteLength,
			       "count-consumers: a counted ENTRY `I[1,0]` traces to the instancing chunk's span" );
			Check( okClone && viaClone.byteOffset == viaChunk.byteOffset,
			       "count-consumers: ... and so does a counted subtree CLONE `I[1,0].C`" );
			// (2) SceneEditor's transform refusal: a counted entry is not transform-routable,
			// and the message names the chunk the author can actually move.
			Check( j->CstObjectTransformKind( "I[1,0]" ) == 0,
			       "count-consumers: a counted entry is NOT transform-routable (no chunk of its own name)" );
			c.SetSelection( Cat::Object, String( "I[1,0]" ) );
			const int before = pRefusalLog->MatchCount();
			Check( !c.SetPropertyForCategory( Cat::Object, String( "position" ), String( "0 5 0" ) ),
			       "count-consumers: a transform edit on a counted entry is refused" );
			Check( pRefusalLog->MatchCount() == before + 1, "count-consumers: ... and the author is told" );
			Check( pRefusalLog->LastMatch().find( "INSTANCE synthesized by `I`" ) != std::string::npos,
			       "count-consumers: ... naming the INSTANCING chunk, resolved through provenance" );
		}
		j->release();
		std::remove( path.c_str() );
	}

	// [count][round-trip] a counted scene round-trips byte-for-byte, and DERIVING it leaves
	// the document untouched -- the expansion writes into the Job, never into the CST.
	{
		const std::string scene = Scene( SUB2
			+ "standard_object\n{\nname I\nsource S\ncount_u 2\ncount_v 2\nposition expr(i*3) expr(j*3) 0\n}\n" );
		Check( SerializeCst( ParseToCst( scene ) ) == scene, "count-round-trip: a counted `source` scene round-trips byte-for-byte" );
		Document d = ParseToCst( scene );
		{
			Job* j = new Job();
			std::vector<std::string> diags;
			DeriveToJob( d, *j, &diags );
			Check( diags.empty(), "count-round-trip: (control) that document really did derive" );
			Check( Obj( j, "I[1,1].C" ) != 0, "count-round-trip: (control) and really did synthesize the grid" );
			j->release();
		}
		const std::string out = SerializeCst( d );
		Check( out == scene, "count-round-trip: ... and deriving it leaves the document byte-identical" );
		Check( out.find( "I[0,0]" ) == std::string::npos,
		       "count-round-trip: ... so no synthesized name appears anywhere in it (the `expr` survives verbatim)" );
	}

	// [count][cap] THE CAP ARITHMETIC, and this is the first slice in which it can be
	// tested at all: 3b could only cross the document budget by MATERIALIZING ten million
	// objects, but a count is refused from ARITHMETIC alone, before a single entry is built.
	//
	// The number in the refusal is the discriminator.  A 3-node subtree repeated 1e6 x 10
	// times is 1e7 INSTANCES -- exactly the budget, so an instance-counting cap would let
	// it through -- and 3e7 ENTRIES, which is what actually reaches the TLAS, the luminary
	// list and every per-frame walk.  The message must say 30000000.
	{
		std::string all;
		const bool refused = RefusedWith( Scene( SUB3
			+ "standard_object\n{\nname I\nsource S\ncount_u 1000000\ncount_v 10\n}\n" ), "would synthesize 30000000 objects", &all );
		Check( refused, ( "count-cap: count x SUBTREE SIZE is what the document budget counts (got: " + all + ")" ).c_str() );
		Check( all.find( "would synthesize 10000000 objects" ) == std::string::npos,
		       "count-cap: ... NOT the instance count, which at 1e7 is exactly the budget and would have passed" );
		Check( all.find( "room for only 10000000 more" ) != std::string::npos,
		       "count-cap: ... and the refusal states the document-wide allowance it ran out of" );
		// And nothing was built: the refusal is arithmetic, not a failure part-way through.
		Job* j = DeriveJob( Scene( SUB3 + "standard_object\n{\nname I\nsource S\ncount_u 1000000\ncount_v 10\n}\n" ) );
		Check( EntryNames( j ) == "C|D|S", "count-cap: ... and NOTHING was synthesized -- the source subtree is all that derived" );
		j->release();
		// A LEAF source (subtree size 1) crosses the same budget on the product alone.
		Check( RefusedWith( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\ncount_u 1000000\ncount_v 100\n}\n" ),
		                    "would synthesize 100000000 objects" ),
		       "count-cap: a leaf source's total IS the product, and the same budget bounds it" );
	}

	// [count][cap] THE ACCOUNTING, which the block above does NOT test.  Its fixture is the
	// document's ONLY expansion, so `entryBudget` is still the full 1e7 when the refusal is
	// composed -- and the refusal reads "room for only 10000000 more" whether the budget is
	// decremented per entry, per repetition, or NOT AT ALL.  Delete either `--entryBudget`
	// and that block stays green.
	//
	// The discriminator is free, because the refusal PRINTS the remaining budget: put a
	// SPENDING expansion in the same document first, and the number in the second one's
	// refusal is the ledger.  `I source S count_u 2` over the 3-node subtree SUB3 spends
	// 2 x 3 = SIX -- two repetition roots and four clones -- so the next generator must be
	// told 9999994.  With the repetition-root decrement deleted it reads 9999996; with the
	// clone decrement deleted, 9999998.
	//
	// THE LEDGER IS DOCUMENT-ORDER, and 87 step 3d is what made that true.  Until it landed,
	// `instance_array` chunks were SKIPPED by the PASS-2 walk and expanded in a trailing
	// loop afterwards, so a generator declared ABOVE a `source` had spent nothing by the
	// time that `source`'s refusal was composed, and the number quoted there was the full
	// allowance.  With the generator gone, `ExpandSourceInstance` is the ONLY writer to
	// `entryBudget` and it runs at each instancing chunk's own PASS-2 position -- so every
	// entry an earlier chunk in the file spent is already on the ledger when a later
	// refusal quotes it, which is exactly what these two fixtures assert.
	{
		std::string all;
		const bool refused = RefusedWith( Scene( SUB3
			+ "standard_object\n{\nname I\nsource S\ncount_u 2\n}\n"
			  "standard_object\n{\nname K\nsource S\ncount_u 1000000\ncount_v 10\n}\n" ),
			"room for only 9999994 more", &all );
		Check( refused, ( "count-cap-ledger: an earlier counted expansion SPENDS from the shared budget, "
		                  "roots and clones alike -- 2 repetitions x 3 entries = 6 (got: " + all + ")" ).c_str() );
		// The SAME ledger through the UNCOUNTED form, which spends 3 (one root + two clones)
		// -- so the decrements are on the shared path, not on a counted-only branch.
		std::string all2;
		Check( RefusedWith( Scene( SUB3
			+ "standard_object\n{\nname I\nsource S\n}\n"
			  "standard_object\n{\nname K\nsource S\ncount_u 1000000\ncount_v 10\n}\n" ),
			"room for only 9999997 more", &all2 ),
		       ( "count-cap-ledger: ... and an UNCOUNTED instance spends its 3 entries too (got: " + all2 + ")" ).c_str() );
		// NON-VACUITY: the two scenes really do differ only in what came before, and the
		// full-budget spelling the block above asserts is NOT what either of them says.
		Check( all.find( "room for only 10000000 more" ) == std::string::npos
		    && all2.find( "room for only 10000000 more" ) == std::string::npos,
		       "count-cap-ledger: ... and neither refusal still claims the full document allowance" );
	}

	// [count][clamp] the PER-COUNT clamp inherited with the shared validator (it was
	// `instance_array`'s before 87 step 3d retired it): 1e6 on EACH axis.  Without it a
	// `count_u 1e12` would be refused only by the budget line above -- one arithmetic slip
	// away from a 1e12-iteration loop.
	{
		// The counts here would ALSO cross the document budget (2e8 entries), so the
		// assertion separates the two refusals: with the clamp removed this scene is still
		// refused, by the budget line, with a different message.  A count that only the
		// clamp catches would have to be materialisable to prove anything.
		Check( RefusedWith( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\ncount_u 2000000\ncount_v 100\n}\n" ),
		                    "count_u must be a non-negative integer <= 1e6 (got '2000000')" ),
		       "count-clamp: a count above 1e6 is refused PER-AXIS, before any document-budget arithmetic" );
	}

	// [count][fractional] A FRACTIONAL COUNT IS REFUSED, NEVER ROUNDED.  The `(long long)`
	// round-trip in the shared validator is what does it; rounding `1.5` to 2 would change
	// how many objects the scene has, silently.
	{
		Check( RefusedWith( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\ncount_u 1.5\n}\n" ),
		                    "count_u must be a non-negative integer <= 1e6 (got '1.5')" ),
		       "count-fractional: `count_u 1.5` is refused with the value the author wrote" );
		Check( RefusedWith( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\ncount_u expr(1.5)\n}\n" ),
		                    "count_u must be a non-negative integer" ),
		       "count-fractional: ... and so is an expr that evaluates to one" );
		Check( RefusedWith( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\ncount_u 2\ncount_v 0.5\n}\n" ),
		                    "count_v must be a non-negative integer" ),
		       "count-fractional: ... on the second axis too" );
		// The CONTROL: an INTEGRAL expr is fine, so the refusal above is about the fraction
		// and not about exprs in a count.
		Job* j = DeriveJob( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\ncount_u expr(1.0+1.0)\n}\n" ) );
		Check( EntryNames( j ) == "I[0,0]|I[1,0]|S", "count-fractional: (control) an integral `expr` count is accepted" );
		j->release();
	}

	// [count][ERANGE] THE UNDERFLOW TRAP, which is the one thing in the shared validator
	// NOTHING ELSE catches.  `strtod("1e-999")` sets ERANGE and returns a finite ZERO --
	// which passes the range test AND the integrality test.  Drop the `errno == ERANGE`
	// term and this scene silently becomes `count 0`: no objects, no diagnostic, no clue.
	//
	// (Its OVERFLOW sibling, `1e999`, never reaches here at all any more: the descriptor
	// declares the counts numeric, so PASS-1's string-layer overflow check refuses first,
	// and 87 step 3d deleted `instance_array` -- the one caller PASS-1 skipped entirely and
	// so the one route by which overflow could still reach ERANGE.  The term is therefore
	// load-bearing for UNDERFLOW only now, which is the half this fixture pins; its
	// overflow half is dead code kept because strtod reports both through one errno.)
	{
		std::string all;
		Check( RefusedWith( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\ncount_u 1e-999\n}\n" ),
		                    "count_u must be a non-negative integer", &all ),
		       ( "count-erange: an UNDERFLOWING count is refused, not silently taken as zero (got: " + all + ")" ).c_str() );
		// NON-VACUITY: the refusal is the validator's, and the scene really would otherwise
		// have derived -- `count_u 0` is legal and derives clean, so "refused" here cannot
		// be an artifact of zero being rejected.
		std::vector<std::string> zdiags;
		Job* jz = DeriveJob( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\ncount_u 0\n}\n" ), &zdiags );
		Check( zdiags.empty(), "count-erange: (control) an explicit `count_u 0` derives CLEANLY, so the refusal above is about ERANGE" );
		jz->release();
	}

	// [count][refuse] `count_v` with no `count_u`.  There is no grid to describe, and
	// silently treating it as `count_u 1` would repeat along an axis the author never named.
	{
		Check( RefusedWith( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\ncount_v 3\n}\n" ),
		                    "`count_v` without `count_u`" ),
		       "count-refuse: `count_v` alone is refused -- `count_u` is the first axis" );
	}

	// [count][refuse] COUNTS WITHOUT A `source`.  The descriptor accepts the parameter names
	// on every `standard_object` (a descriptor IS the accepted-parameter set, and it cannot
	// be conditional), so the meaning check belongs in the parser -- and unlike the lone
	// `source` backstop beside it, THIS one is genuinely reachable: a chunk with counts and
	// no `source` is not an instancing chunk, so PASS-2 hands it to Finalize like any other.
	{
		Check( RefusedWith( Scene( "standard_object\n{\nname X\ngeometry geo\nmaterial m\ncount_u 3\n}\n" ),
		                    "repeat an INSTANCE, so they need a `source`" ),
		       "count-refuse: `count_u` on a LEAF chunk is refused (there is nothing to repeat)" );
		Check( RefusedWith( Scene( "standard_object\n{\nname X\ncount_v 3\n}\n" ),
		                    "repeat an INSTANCE, so they need a `source`" ),
		       "count-refuse: ... and on a CONTAINER chunk too" );
		// The parameter is DECLARED, which is what makes the refusal above the parser's own
		// message rather than the descriptor's "not declared in `standard_object`".
		//
		// ASSERTED ON THE LOG, not on the diagnostics.  The descriptor's undeclared-name
		// rejection is PRINTED (ChunkParserRegistry's DispatchChunkParameters) and what
		// reaches `diags` is the generic "invalid parameter(s) (see log)" -- so the
		// obvious `diags.find(...) == npos` form cannot fail whatever the descriptor
		// says, and stayed green with `count_u` removed from it.
		const int undeclBefore = pUndeclaredLog->MatchCount();
		std::string all;
		RefusedWith( Scene( "standard_object\n{\nname X\ngeometry geo\ncount_u 3\n}\n" ), "zzz-never", &all );
		Check( pUndeclaredLog->MatchCount() == undeclBefore,
		       "count-refuse: ... and it is the PARSER's reason -- the descriptor DECLARES `count_u`, so nothing "
		       "logged an undeclared-parameter rejection" );
	}

	// [count][refuse] A COUNTED CHUNK CANNOT BE A `source`, from both sides.  It produces
	// one entry per (i,j) rather than a single node, so a copy of it would silently be a
	// copy of exactly one -- the partial-copy shape every other refusal in the walk exists
	// to prevent.
	{
		Check( RefusedWith( Scene( SRC_LEAF
			+ "standard_object\n{\nname A\nsource S\ncount_u 3\n}\n"
			  "standard_object\n{\nname B\nsource A\n}\n" ), "names a chunk carrying `count_u` / `count_v`" ),
		       "count-refuse: instancing a COUNTED chunk is refused (there is no single node to copy)" );
		// NAMED FOR ITS REAL CAUSE.  The manager probe one line further on would have
		// refused this scene anyway -- a counted chunk produces `A[0,0]`, never `A` -- with
		// "declared earlier but did not produce an object (its own chunk failed)", a cause
		// that did not happen and a chunk that is not broken.
		{
			std::string all;
			RefusedWith( Scene( SRC_LEAF
				+ "standard_object\n{\nname A\nsource S\ncount_u 3\n}\n"
				  "standard_object\n{\nname B\nsource A\n}\n" ), "zzz-never", &all );
			Check( all.find( "did not produce an object" ) == std::string::npos,
			       "count-refuse: ... and NOT with the misleading `its own chunk failed` message the probe would give" );
		}
		Check( RefusedWith( Scene( SRC_LEAF
			+ "standard_object\n{\nname M\nsource S\nparent S\ncount_u 3\n}\n" ), "recursive definition" )
		    || RefusedWith( Scene( SRC_LEAF
			+ "standard_object\n{\nname M\nsource S\nparent S\ncount_u 3\n}\n" ), "SAME chunk" ),
		       "count-refuse: (control) the pre-existing `source S parent S` refusal still fires under counts" );
		// The MEMBER side: a counted node INSIDE a subtree being copied.
		Check( RefusedWith( Scene(
			  "standard_object\n{\nname L\ngeometry geo\nmaterial m\n}\n"
			  "standard_object\n{\nname S\ngeometry geo\nmaterial m\nposition 2 0 0\n}\n"
			  "standard_object\n{\nname M\nparent S\nsource L\ncount_u 3\n}\n"
			  "standard_object\n{\nname I\nsource S\n}\n" ), "carries `count_u` / `count_v`" ),
		       "count-refuse: a counted MEMBER of a copied subtree is refused too" );
	}

	// [count][refuse] THE OTHER TWO WAYS TO NAME A COUNTED CHUNK BY ITS BARE NAME, and both
	// of them landed on a pre-existing diagnostic that enumerates causes NONE of which is
	// the real one.  `source A` got its own refusal above; `parent A` and
	// `override_object { name A }` did not.
	//
	// The negative half of each pair carries the claim.  Refusing at all is easy -- both
	// scenes were ALREADY refused, which is exactly why the gap was invisible: `parent I`
	// with "A `parent` must be a DECLARED-EARLIER object; must not be this object; must not
	// already be one of its descendants; and must not be a CSG operand", four causes and `I`
	// satisfies every one of them; `override_object` with "target `I` not found in scene.
	// Possible causes: (a) ... BEFORE the chunk that creates the target (b) ... deleted
	// (c) ... typo", none of them either.  So the assertions that the OLD text is GONE are
	// what distinguishes the fix from the status quo ante.
	//
	// And the positive control is the point: `parent I[0,0]` is not an error at all, it is
	// the intended idiom -- so the refusal has to name it rather than merely refuse.
	{
		const std::string COUNTED = SRC_LEAF + "standard_object\n{\nname I\nsource S\ncount_u 2\n}\n";

		std::string all;
		Check( RefusedWith( Scene( COUNTED + "standard_object\n{\nname Z\nparent I\ngeometry boxg\nmaterial m2\n}\n" ),
		                    "names a chunk carrying `count_u` / `count_v`", &all ),
		       ( "count-refuse: `parent <counted chunk>` is refused for its REAL cause -- `I` is a repetition, "
		         "so there is no entry called `I` to attach to (got: " + all + ")" ).c_str() );
		Check( all.find( "`parent I[0,0]`" ) != std::string::npos,
		       ( "count-refuse: ... and the refusal spells the WORKING form, which is the intended idiom rather "
		         "than an error (got: " + all + ")" ).c_str() );
		Check( all.find( "must be a DECLARED-EARLIER object" ) == std::string::npos,
		       "count-refuse: ... and NOT with the four-cause `parent` message, whose every clause `I` satisfies" );

		// POSITIVE CONTROL, and it is the sentence the refusal points at: the repetition's
		// own entry name parents perfectly well.  Without this the refusal above could be
		// recommending something that does not work.
		{
			std::vector<std::string> diags;
			Job* j = DeriveJob( Scene( COUNTED + "standard_object\n{\nname Z\nparent I[0,0]\ngeometry boxg\nmaterial m2\n}\n" ), &diags );
			Check( diags.empty(), "count-refuse: (control) `parent I[0,0]` -- the form the refusal recommends -- DERIVES" );
			Check( ParentOf( j, "Z" ) == "I[0,0]", "count-refuse: (control) ... and really does attach to that repetition" );
			j->release();
		}

		// `override_object { name I }`, the same shape through the other reference.
		const int overrideMissingBefore = pOverrideMissingLog->MatchCount();
		std::string all2;
		Check( RefusedWith( Scene( COUNTED + "override_object\n{\nname I\nposition 9 0 0\n}\n" ),
		                    "names a chunk carrying `count_u` / `count_v`", &all2 ),
		       ( "count-refuse: `override_object` on a counted chunk is refused for its REAL cause (got: " + all2 + ")" ).c_str() );
		Check( all2.find( "`name I[0,0]`" ) != std::string::npos,
		       ( "count-refuse: ... naming the layer that WOULD work (got: " + all2 + ")" ).c_str() );
		Check( all2.find( "apply failed" ) == std::string::npos,
		       "count-refuse: ... and not as a generic PASS-2 apply failure" );
		// The misleading three-cause message is LOG-only, so `all2` could never have held it:
		// the delta on the log is the only thing that can say it is no longer printed.
		Check( pOverrideMissingLog->MatchCount() == overrideMissingBefore,
		       ( "count-refuse: ... and the three-cause `not found in scene` LOG line is not printed either (last: "
		         + pOverrideMissingLog->LastMatch() + ")" ).c_str() );

		// NON-VACUITY of that log delta: the SAME override against a genuinely absent name
		// still prints it, so the assertion above is reading a real signal.
		{
			const int before = pOverrideMissingLog->MatchCount();
			RefusedWith( Scene( SRC_LEAF + "override_object\n{\nname NoSuchThing\nposition 9 0 0\n}\n" ), "zzz-never" );
			Check( pOverrideMissingLog->MatchCount() == before + 1,
			       "count-refuse: (control) an override of a name that really is absent DOES still print it" );
		}

		// POSITIVE CONTROL for the override side too.
		{
			std::vector<std::string> diags;
			Job* j = DeriveJob( Scene( COUNTED + "override_object\n{\nname I[0,0]\nposition 9 0 0\n}\n" ), &diags );
			Check( diags.empty(), "count-refuse: (control) `override_object { name I[0,0] }` -- the recommended form -- DERIVES" );
			Check( CenterIs( Obj( j, "I[0,0]" ), 9, 0, 0 ), "count-refuse: (control) ... and really does move that repetition" );
			j->release();
		}

		// The scan is keyed on the chunk CARRYING counts, not on the spelling of the name:
		// an UNCOUNTED source instance is still an ordinary parent.
		{
			std::vector<std::string> diags;
			Job* j = DeriveJob( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\n}\n"
			                                      "standard_object\n{\nname Z\nparent I\ngeometry boxg\nmaterial m2\n}\n" ), &diags );
			Check( diags.empty() && ParentOf( j, "Z" ) == "I", "count-refuse: (control) `parent <uncounted instance>` is untouched" );
			j->release();
		}
	}

	// [count][validation] AN INSTANCING CHUNK IS STILL DESCRIPTOR-VALIDATED.  PASS-1 cannot
	// check `position expr(i*2) 0 0` as written -- it is not a finite numeric triple -- so it
	// evaluates the chunk at instance ZERO and checks THAT.  The point of the pair below is
	// that this buys the per-instance form without giving up the validation: a component that
	// is neither an expr nor a number is still refused.
	{
		std::vector<std::string> diags;
		Job* j = DeriveJob( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\ncount_u 2\nposition expr(i) 0 0\n}\n" ), &diags );
		Check( diags.empty() && Obj( j, "I[1,0]" ) != 0, "validation: a per-component `expr` on an instancing chunk is ACCEPTED" );
		j->release();
		const int numericBefore = pNumericLog->MatchCount();
		Check( RefusedWith( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\ncount_u 2\nposition expr(i) bogus 0\n}\n" ),
		                    "invalid parameter(s)" ),
		       "validation: ... while a non-numeric component beside it is still REFUSED" );
		Check( pNumericLog->MatchCount() == numericBefore + 1
		    && pNumericLog->LastMatch().find( "`position`" ) != std::string::npos,
		       "validation: ... by the DESCRIPTOR's numeric check, on `position` -- not by some other PASS-1 failure" );
		// And a bad expr BODY is refused at the eval boundary, naming the parameter.
		std::string all;
		RefusedWith( Scene( SRC_LEAF + "standard_object\n{\nname I\nsource S\ncount_u 2\nposition expr(nosuchvar) 0 0\n}\n" ),
		             "zzz-never", &all );
		Check( all.find( "standard_object.position" ) != std::string::npos && all.find( "failed to compile" ) != std::string::npos,
		       ( "validation: ... and an expr over an unknown identifier is refused, naming the parameter (got: " + all + ")" ).c_str() );
	}

	// [count][scope] PER-INSTANCE VARIATION IS THE INSTANCING CHUNK'S OWN PARAMS ONLY.  A
	// subtree MEMBER is an ordinary `standard_object`, so PASS-1 refuses a per-component
	// expr on it -- the `instance_array` generator 3d deleted never offered per-descendant
	// variation either (it had no descendants at all), so this is a documented limit and
	// not a regression.
	{
		const int numericBefore = pNumericLog->MatchCount();
		Check( RefusedWith( Scene(
			  "standard_object\n{\nname S\ngeometry geo\nmaterial m\n}\n"
			  "standard_object\n{\nname C\nparent S\ngeometry boxg\nmaterial m2\nposition expr(i) 0 0\n}\n"
			  "standard_object\n{\nname I\nsource S\ncount_u 2\n}\n" ), "invalid parameter(s)" ),
		       "scope: a per-component expr on a subtree MEMBER is refused -- counts vary the instancing chunk only" );
		Check( pNumericLog->MatchCount() == numericBefore + 1,
		       "scope: ... by the descriptor's numeric check on the MEMBER's own chunk" );
	}

	// ================================================================
	// NAMELESS -- the document keyspace vs the LIVE MANAGER keyspace.
	//
	// `standard_object` / `csg_object` DEFAULT the registered entry name to
	// `noname` (ChunkParserRegistry.cpp), so a chunk that spells no `name`
	// still produces a real object.  `BuildObjectChunkIndex` used to index
	// only chunks that SPELL a name, so the collision scan was blind to
	// those -- and an instancing chunk explicitly named `noname` coexisted
	// with one, undiagnosed, two authored things claiming one name.
	// ================================================================
	{
		// The PREMISE, asserted first so the case cannot pass for the wrong
		// reason: a nameless chunk really does register `noname`.  This is the
		// behavioural pin that keeps the CST's literal and the parser's default
		// from drifting apart -- there is no shared header to hold one copy.
		{
			std::vector<std::string> diags;
			Job* j = DeriveJob( Scene(
				"standard_object\n{\ngeometry geo\nmaterial m\n}\n" ), &diags );
			Check( EntryNames( j ) == "noname",
			       "nameless: the premise -- a chunk spelling no `name` registers `noname`" );
			if( j ) j->release();
		}
		// A nameless csg_object defaults the same way.  Pinned because
		// RoleDefaultedEntryName lists exactly these two roles, and the other
		// two object roles REFUSE an empty name instead of defaulting.
		{
			Job* j = DeriveJob( Scene(
				"standard_object\n{\nname A\ngeometry geo\nmaterial m\n}\n"
				"standard_object\n{\nname B\ngeometry boxg\nmaterial m\n}\n"
				"csg_object\n{\nobja A\nobjb B\noperation union\n}\n" ) );
			const std::string names = EntryNames( j );
			Check( names.find( "noname" ) != std::string::npos,
			       "nameless: a csg_object spelling no `name` also registers `noname`" );
			if( j ) j->release();
		}
		// THE DIVERGENCE, which used to load with ZERO diagnostics.
		std::string all;
		const bool refused = RefusedWith( Scene(
			"standard_object\n{\nname S\ngeometry geo\nmaterial m\n}\n"
			"standard_object\n{\ngeometry geo\nmaterial m\n}\n"
			"standard_object\n{\nname noname\nsource S\ncount_u 2\n}\n" ),
			"is declared by MORE THAN ONE object chunk", &all );
		Check( refused,
		       "nameless: an instancing chunk named `noname` beside a NAMELESS chunk is REFUSED" );
		// The instruction has to fit the chunk.  "Rename one" is wrong advice for
		// a chunk that spells no name at all, so the message must say the other
		// thing -- and this assertion is what stops it silently reverting.
		Check( all.find( "spells no `name` at all" ) != std::string::npos,
		       "nameless: ... and the message says to NAME the nameless one, not rename it" );
		Check( all.find( "Rename one." ) == std::string::npos,
		       "nameless: ... and does NOT say `Rename one.` for that case" );
		// The ordinary duplicate keeps the ordinary wording.  Without this, the
		// branch above could return the nameless text for every collision and
		// nothing would notice.
		// EXACTLY TWO chunks declare the name, both SPELLED -- the same shape as
		// the divergence case above with the nameless chunk replaced by a named
		// one, so the only difference between them is the thing under test.  (A
		// third same-named chunk makes the derive fail earlier with "apply
		// failed" and the scan never runs, which is why this is a pair.)
		std::string all2;
		Check( RefusedWith( Scene(
			"standard_object\n{\nname S\ngeometry geo\nmaterial m\n}\n"
			"standard_object\n{\nname D\ngeometry geo\nmaterial m\n}\n"
			"standard_object\n{\nname D\nsource S\ncount_u 2\n}\n" ),
			"Rename one.", &all2 ),
		       "nameless: the control -- two SPELLED duplicates still say `Rename one.`" );
		Check( all2.find( "spells no `name` at all" ) == std::string::npos,
		       "nameless: ... and do NOT get the nameless instruction" );
		// THE ROLE SCOPING'S PREMISE.  RoleDefaultedEntryName deliberately lists
		// only `standard_object` and `csg_object`; `rect_light` and `shape_light`
		// read `GetString( "name", std::string() )` and REFUSE an empty one.  This
		// pins that refusal, which is the whole justification for excluding them.
		{
			std::string all3;
			RefusedWith( Scene(
				"standard_object\n{\nname S\ngeometry geo\nmaterial m\n}\n"
				"rect_light\n{\ncenter 2 3 1\nsize 1 1\nfacing 0 -1 0\ncolor 1 1 1\nexitance 20\n}\n" ),
				"", &all3 );
			Check( all3.find( "`name` is required" ) != std::string::npos,
			       "nameless: a NAMELESS rect_light REFUSES rather than defaulting -- the premise "
			       "that lets RoleDefaultedEntryName exclude the two light roles" );
		}
		// AND THE SCOPING ITSELF, which POSITION makes observable.
		//
		// Widening RoleDefaultedEntryName to the two light roles would make a
		// nameless `rect_light` claim `noname` in the collision keyspace -- so
		// this document would refuse with a COLLISION naming the light, instead of
		// with the light's own "`name` is required".
		//
		// The light is placed AFTER the instancing chunk on purpose, and that is
		// the entire trick.  PASS-2 applies chunks in DOCUMENT ORDER and breaks AT
		// the failure, so a nameless light placed FIRST aborts the derive before
		// any expansion runs and the two helper widths look identical.  But
		// `BuildObjectChunkIndex` is a whole-document PRE-PASS: put the light last
		// and the expansion -- with the light already in the index -- runs first.
		// Measured both ways: narrow gives `name` is required, wide gives the
		// collision naming chunk #9 (`standard_object`) and #10 (`rect_light`).
		{
			std::string all4;
			RefusedWith( Scene(
				"standard_object\n{\nname S\ngeometry geo\nmaterial m\n}\n"
				"standard_object\n{\nname noname\nsource S\ncount_u 2\n}\n"
				"rect_light\n{\ncenter 2 3 1\nsize 1 1\nfacing 0 -1 0\ncolor 1 1 1\nexitance 20\n}\n" ),
				"", &all4 );
			Check( all4.find( "declared by MORE THAN ONE object chunk" ) == std::string::npos,
			       "nameless: RoleDefaultedEntryName is SCOPED to the two defaulting roles -- a "
			       "nameless `rect_light` after an instancing chunk named `noname` must NOT be "
			       "reported as a collision with it" );
			Check( all4.find( "`name` is required" ) != std::string::npos,
			       "nameless: ... the light still refuses on its own terms instead" );
		}
		// THE MESSAGE NAMES THE CHUNKS ITS ADVICE IS ABOUT.
		//
		// The collision text quotes exactly TWO declarers, `[0]` and `[1]`, so the
		// nameless-vs-rename branch must be decided over those two and no others.
		// Deciding it over EVERY declarer produced a message that named two chunks
		// which both SPELL `name noname` and then told the author "one of them
		// spells no `name` at all" -- true of neither, while the nameless chunk it
		// was about (#11) went unnamed.  Three declarers is the smallest document
		// that can tell the two implementations apart.
		{
			std::string all5;
			Check( RefusedWith( Scene(
				"standard_object\n{\nname S\ngeometry geo\nmaterial m\n}\n"
				"standard_object\n{\nname noname\ngeometry boxg\nmaterial m\n}\n"
				"standard_object\n{\nname noname\nsource S\ncount_u 2\n}\n"
				"standard_object\n{\ngeometry boxg\nmaterial m\n}\n" ),
				"declared by MORE THAN ONE object chunk", &all5 ),
			       "nameless: three declarers of `noname` -- two SPELLED, one nameless -- collide" );
			Check( all5.find( "spells no `name` at all" ) == std::string::npos,
			       "nameless: ... and the message does NOT claim one of the two chunks it NAMES is "
			       "nameless when BOTH of them spell `name noname`" );
			Check( all5.find( "Rename one." ) != std::string::npos,
			       "nameless: ... it gives the advice that fits the pair it quoted" );
		}
		// THE DEFAULTED NAME IS NOT ADDRESSABLE, and the two keyspaces the index
		// keeps are what separate that from the collision above.
		//
		// `entryByName` answers "what live entry name does this chunk CLAIM?", and
		// the defaulted `noname` belongs there -- that is the whole fix.  `byName`
		// answers "which chunk does this name RESOLVE to?", and it is read by
		// `source` resolution, by SourceChainOf and by both ClonePlanBuilder nested
		// lookups.  Putting the defaulted name in THAT one made `source noname`
		// legal (measured), binding an instance to a chunk the editor cannot
		// address at all: `DocFindByNameAnyRole` skips a chunk whose
		// `ChunkNamePath` is empty, so nothing could ever resolve back to it -- and
		// the link would break with "no object of that name" the moment the author
		// finally spelled a name on it.  So the feed is SPELLED-only, and this is
		// the assertion that keeps it that way.
		{
			Check( RefusedWith( Scene(
				"standard_object\n{\ngeometry geo\nmaterial m\n}\n"
				"standard_object\n{\nname I\nsource noname\n}\n" ),
				"no `standard_object` / `csg_object` of that name exists" ),
			       "nameless: `source noname` is REFUSED -- a chunk's DEFAULTED entry name is not a "
			       "`source` target, because the editor cannot address a nameless chunk" );
		}
		// A nameless chunk with NO instancing chunk in the document must still
		// derive cleanly: indexing the defaulted name must not turn a lone
		// nameless object into a refusal.  0 of 404 tracked scenes have one,
		// but the corpus is not the contract.
		{
			std::vector<std::string> diags;
			Job* j = DeriveJob( Scene(
				"standard_object\n{\nname A\ngeometry geo\nmaterial m\n}\n"
				"standard_object\n{\ngeometry boxg\nmaterial m\n}\n" ), &diags );
			Check( diags.empty(), "nameless: a lone nameless chunk still derives with NO diagnostic" );
			Check( EntryNames( j ) == "A|noname",
			       "nameless: ... and both objects exist" );
			if( j ) j->release();
		}
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
