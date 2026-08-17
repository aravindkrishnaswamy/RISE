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
//    [incremental] an edit to a `source` chunk refuses -> full-derive fallback; so does an edit
//                  to an entry that still carries a PROVENANCE row (`source none`, or the line
//                  DELETED), which is how the stale row gets retired -- while a `source none`
//                  chunk that was never an instance stays on the incremental path.
//    [gizmo]       CstObjectTransformKind answers for the SOURCE's role, so the transform gate
//                  and the transform commit cannot disagree (the live/CST divergence class); a
//                  same-named `override_object` OWNS the pose, so the components commit walks to
//                  it (else the drag reports SUCCESS and the re-derive puts the object back);
//                  `matrix` / `quaternion` / `scale` are stripped SYMMETRICALLY from whichever
//                  chunk the commit lands on; and each refusal names its own true cause -- the
//                  author's `source` line rather than a chunk type their scene does not contain,
//                  and a blocking non-unit SCALE rather than a rotation they never made.
//    [end-to-end]  the same poses driven through SceneEditController -- the whole chain
//                  (Apply -> the DecomposeRigid post-mutate gate -> CommitPendingCstObject
//                  Transforms -> ApplyCstObjectComponentsEdit) -- asserting where the object
//                  ENDS UP after the commit re-derives, which a direct Job::ApplyCst*Edit call
//                  cannot: the direct route accepts inputs production can never produce.
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

#include <cmath>
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
	std::printf( "CstSourceInstanceTest -- 87 step 3a: `source` instancing, the collapse case\n" );

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
		// UNCONDITIONAL.  This was written as a disjunction ("no `step 3b` OR names `parent S`"),
		// which a message mentioning NEITHER term satisfies -- so deleting both strings from the
		// diagnostic would have left it green while violating the property it states.  It was also
		// strictly weaker than the "recursive definition" pin above it.  The message must NAME the
		// author's `parent` line, because that line is the cause.
		Check( all.find( "`parent S`" ) != std::string::npos,
		       "refuse: ... naming the `parent` line as the cause, not sending the author after a subtree that does not exist" );
		Check( all.find( "has CHILDREN -- instancing a multi-node subtree" ) == std::string::npos,
		       "refuse: ... and NOT with the has-children message" );
		// A source with a REAL other child still gets the 3b message, even when the instance
		// also parents to it -- the detection is "every child instances S", not "S has one child".
		Check( RefusedWith( Scene( SRC_LEAF
		                         + "standard_object\n{\nname kid\ngeometry boxg\nmaterial m\nparent S\n}\n"
		                         + "standard_object\n{\nname I\nsource S\nparent S\n}\n" ),
		                    "has CHILDREN -- instancing a multi-node subtree" ),
		       "refuse: ... while a source with a genuine OTHER child still gets the 3b message" );
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
			Check( RefusedWith( Scene( two ), "recursive definition", &twoAll ),
			       "refuse: ... and a SECOND self-parenting instance does not defeat the rule (count is 2, still no subtree)" );
			Check( twoAll.find( "has CHILDREN -- instancing a multi-node subtree" ) == std::string::npos,
			       "refuse: ... neither of the two gets the 3b subtree message" );
		}
		// THE OTHER HALF OF THE CONDITION.  `selfChildrenOnly` is `meAmongThem && allSelfSourced`,
		// and only `allSelfSourced` was pinned above -- so `meAmongThem` could be dropped and the
		// suite would stay green.  This is the scene that separates them: `I source S` with NO
		// `parent` line, and a SEPARATE `K source S parent S`.  Every child of S does instance S
		// (allSelfSourced is true), but `I` is not among them -- for `I`, S genuinely HAS a child,
		// and `I` must get the 3b message.  Without `meAmongThem`, `I` would be told its own
		// `parent S` line created a recursive definition when it has no `parent` line at all,
		// which is a worse misdirection than the one the branch was added to remove.
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
			Check( forI.find( "has CHILDREN -- instancing a multi-node subtree" ) != std::string::npos,
			       "refuse: an UNPARENTED instance of a source that has a self-parenting sibling gets the 3b message" );
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
	// DELIBERATELY ABSENT: an authored csg_object with NO override.  That shape takes the
	// INCREMENTAL apply, whose re-apply calls SetPosition without clearing the transform stack the
	// live gesture pushed onto, so `position 5 0 0` commits to 10.25 -- a PRE-EXISTING double-apply
	// on a different subsystem (it predates 87 step 3a; the csg-sourced shapes escape it only
	// because a `source` chunk forces the full re-derive).  It is tracked separately and must not
	// be pinned here in either direction.
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
			{ "e2e-authored-csg",
			  csgSrc + "override_object\n{\nname C\nposition 0 2 0\n}\n",                              "C", 5.25 },
			{ "e2e-authored-csg-signflip",
			  csgSrc + "override_object\n{\nname C\nscale -1 -1 1\n}\n",                               "C", 4.75 },
			{ "e2e-instance",
			  csgSrc + "standard_object\n{\nname I\nsource C\n}\n",                                    "I", 5.25 },
			{ "e2e-instance-override",
			  csgSrc + "standard_object\n{\nname I\nsource C\n}\n"
			           "override_object\n{\nname I\nposition 0 2 0\n}\n",                              "I", 5.25 },
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
				Check( std::fabs( CenterX( Obj( j, cases[k].target ) ) - cases[k].expectX ) < 1e-6,
				       ( std::string( cases[k].label ) + ": ... and the COMMITTED pose is where the user put it, "
				         "not where a re-derive re-decided" ).c_str() );
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

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
