//////////////////////////////////////////////////////////////////////
//
//  AgentRevertRevisionTest.cpp - doc 90 slice R2, THE WAY BACK.
//
//  docs/agentic-redesign/90-iteration-ratchet.md sec 1 records a failure
//  with two halves: the dragon probe had NO MEMORY OF BETTER and NO WAY
//  BACK.  R1 shipped the memory (the anchor composite).  This is the way
//  back -- `revert_to_revision`, which restores the DOCUMENT to its text
//  as of an earlier head revision of the session.
//
//  The property that makes the verb safe to hand a model is that it is
//  NOT a rewind: the restore is one ordinary agent edit whose text
//  happens to be one the document had before, so history stays
//  append-only, the undo stack is untouched, and a single Cmd-Z undoes
//  the revert like any other edit.  Every case below is written to catch
//  a violation of that, not merely to observe a success:
//
//    A  ACROSS SEVERAL REVISIONS.  Four edits, then a revert to the
//       first: the head goes UP (never back), the document is
//       BYTE-IDENTICAL to the recorded text, the scene still derives,
//       and it still renders non-black.  Byte-identity is the assertion
//       a "close enough" restore fails.
//    B  REVERT-THE-REVERT.  Back to the pre-revert head, then back
//       again.  Both land, every head is higher than the last, and each
//       document is byte-exact.  A rewind implementation passes A and
//       fails B.
//    C  THE RING.  Its cap really evicts (oldest-first, the refusal
//       NAMES the oldest still available), and the ANCHOR's revision
//       SURVIVES eviction -- R1's render note now tells the model to
//       pass exactly that number, so an evicted anchor would make the
//       advice a lie at the moment it matters most.  Red-proof: with the
//       pin removed from RecordRevisionSnapshot_, this case fails.
//    D  THE ELEMENT LEDGER.  Build an element, revert to before it
//       existed: the attributions for the vanished chunks are DROPPED
//       (an attribution that outlives its chunk makes
//       CheckElementWindowForEdit_ refuse an edit naming a chunk that
//       does not exist -- unfixable by the reopen_element it
//       recommends), the PHASE and active element are NOT rewound (they
//       record what the model did, not what the document holds), and the
//       protocol is not stranded: the same element builds again and
//       finishes.
//    E  UNDO through a live SceneEditController: ONE Undo() restores the
//       pre-revert document exactly, and Redo() reinstalls the reverted
//       one.  The real Cmd-Z path, not a simulation.
//    F  THE R1 NOTE.  The render that BECOMES the anchor does not name
//       the verb (there is nothing yet to go back to).  The other half
//       -- the composite note naming `revert_to_revision <anchorRev>` --
//       is pinned in AgentRenderAnchorTest, where the assertion that
//       pinned its ABSENCE was deliberately inverted when R2 landed.
//    G  THE REFUSAL MATRIX, each one asserted on a message substring
//       (which RULE fired, not merely that something did) AND on the
//       document being byte-identical afterwards: revision 0, the
//       current head, a future revision, an unrecorded revision, an
//       evicted revision, External authority.
//    J  THE LEDGER'S OTHER HALF.  D pins the DROP; this pins the
//       RESTORE.  Revert away from an element's chunk, then revert BACK:
//       the chunk's text returns AND so does its attribution, proved
//       functionally (element_chunks / finish_element / reopen_element
//       all see it) rather than by inspecting a counter.  Then the
//       finished-element combination is exercised for real: with "tower"
//       CLOSED, an edit on the restored chunk from another element's
//       window is refused NAMING "tower", and reopen_element("tower")
//       makes the same edit land.  Red-proof: with the restore pass
//       disabled, the reappeared chunk is unattributed -- freely
//       editable across element windows, which is the bug.
//    K  A GAP IS NOT AN EVICTION, even when eviction has swept past it.
//       A revision the session's head passed through with NO capture (two
//       raw controller edits back to back, the GUI-hand-edit shape) sits
//       numerically below hundreds of later evictions.  The refusal must
//       still say "this session has no copy", not "it aged out" -- the
//       first sends the model somewhere else, the second sends it hunting
//       for a later revision that does not exist either.  Red-proof: with
//       the old bare "highest revision ever evicted" high-water test,
//       this case fails on exactly that lie.
//    H  WIRE: dispatches through AgentRpcDispatcher with the documented
//       result shape, is declared in the shared chat-codec tool table,
//       and -- the two-list-drift lesson -- is ADVERTISED and ROUTABLE
//       on MCP.  Plus the autonomy postures: refused under Read, refused
//       with a Propose-specific message under Propose.
//
//  Self-contained: an inline native-v7 scene (a lit sphere), OIDN off,
//  no RISE_MEDIA_PATH, headless except case E.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentRpc.h"
#include "../src/Library/Agent/AgentMcpAdapter.h"
#include "../src/Library/Agent/AgentChatCodecs.h"
#include "../src/Library/Agent/Json.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Job.h"
#include "../src/Library/SceneEditor/SceneEditController.h"

#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>
#endif
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace RISE;
using namespace RISE::Agent;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const std::string& w )
{
	if( c ) ++g_pass;
	else { ++g_fail; std::printf( "  FAIL: %s\n", w.c_str() ); }
}

// A small, self-contained native-v7 scene: a lit diffuse sphere with an
// area emitter, path-traced at a low sample count with OIDN off.  Same
// body AgentRenderAnchorTest uses, which is known to render non-black.
static const char* const kScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n\n"
	"standard_object\n{\n\tname obj_sph\n\tgeometry sph\n\tmaterial mat_diffuse\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 30.0\n\tmaterial none\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_emit\n\tpta -0.6 0.6 3.5\n\tptb 0.6 0.6 3.5\n\tptc 0.6 -0.6 3.5\n\tptd -0.6 -0.6 3.5\n}\n\n"
	"standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}\n";

static std::string TempPath( const char* name )
{
	const char* base = std::getenv( "TMPDIR" );
	std::string dir = base ? base : "/tmp";
	if( !dir.empty() && dir[dir.size() - 1] != '/' ) dir += '/';
	// Per-process: nothing stops two copies of this binary running at once,
	// and a fixed name would let them clobber each other's scene file.
	return dir + std::to_string( (long)getpid() ) + "_" + name;
}

static std::string WriteTemp( const char* name, const std::string& text )
{
	const std::string path = TempPath( name );
	std::ofstream f( path.c_str(), std::ios::binary );
	if( !f ) return std::string();
	f.write( text.data(), (std::streamsize)text.size() );
	f.close();
	return path;
}

static Job* LoadJob( const std::string& path )
{
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
		pJob->release();
		return nullptr;
	}
	return pJob;
}

//----------------------------------------------------------------------
// A NO-OP-render SceneEditController, so case E can drive the REAL
// Undo() the GUI's Cmd-Z drives (EditHistory lives on the controller;
// the headless direct-Job path has no undo stack at all).  Copied in
// shape from AgentVaryMaterialTest's, including the no-Start(): the test
// never needs a render thread, and not spawning one keeps it
// deterministic.
//----------------------------------------------------------------------
class QuietController : public SceneEditController
{
public:
	explicit QuietController( IJobPriv& job )
	: SceneEditController( job, /*interactiveRasterizer*/0 ) {}
protected:
	void DoOneRenderPass() override {}
};

//! Bump the head by editing the sphere's radius to a fresh value.  Every
//! call is one commit and therefore exactly one new head revision.
static bool BumpRevision( AgentSession& s, double radius )
{
	AgentSetPatch p;
	p.target = "sph";
	p.param  = "radius";
	char buf[64];
	std::snprintf( buf, sizeof( buf ), "%.6f", radius );
	p.value = buf;
	const AgentPatchResult r = s.ProposePatch( p );
	return r.applied;
}

static AgentRenderParams FullFrameParams( unsigned int imageMaxEdge )
{
	AgentRenderParams p;
	p.imageMaxEdge     = imageMaxEdge;
	p.fromAgentSurface = true;
	return p;
}

//! Mean luma of a rendered PNG is not what we want here (the anchor test
//! owns the pixel probes); what THIS file needs from a render is only
//! "the restored document still renders at all", so a non-empty PNG plus
//! ok is the honest check.
static bool RendersOk( AgentSession& s )
{
	const AgentRenderResult rr = s.Render( FullFrameParams( 32 ) );
	return rr.ok && !rr.png.empty();
}

//======================================================================
// A. Across several revisions.
//======================================================================
static void CaseA( const std::string& scenePath )
{
	std::printf( "[A] four edits, then a revert to the first -- head UP, bytes EXACT\n" );
	std::unique_ptr<AgentSession> s = AgentSession::LoadFromFile( scenePath );
	Check( s != nullptr, "A: the fixture loads" );
	if( !s ) return;

	const std::uint64_t rev0 = s->ReadHeadVersion().revision;
	const std::string   doc0 = s->ReadDocument();
	Check( !doc0.empty(), "A: the loaded document is readable" );

	Check( s->RecordedRevisionCount() == 0,
	       "A: a session that has done nothing yet records NOTHING -- the ring fills from the "
	       "session's own work, it is not seeded at load" );

	std::uint64_t revs[4] = { 0, 0, 0, 0 };
	std::string   docs[4];
	for( int i = 0; i < 4; ++i ) {
		Check( BumpRevision( *s, 0.5 + 0.1 * ( i + 1 ) ), "A: edit " + std::to_string( i + 1 ) + " applies" );
		revs[i] = s->ReadHeadVersion().revision;
		docs[i] = s->ReadDocument();
	}
	Check( revs[0] > rev0 && revs[1] > revs[0] && revs[2] > revs[1] && revs[3] > revs[2],
	       "A: (premise) each edit bumped the head" );
	Check( docs[3] != doc0, "A: (premise) the document really changed" );

	// The ring holds every head the edits STARTED from -- which is every
	// revision except the current one.
	Check( s->HasRecordedRevision( rev0 ), "A: the load revision was recorded (edit 1 started from it)" );
	Check( s->HasRecordedRevision( revs[2] ), "A: and so was the head edit 4 started from" );
	Check( !s->HasRecordedRevision( revs[3] ),
	       "A: the CURRENT head is not in the ring yet -- and it is the one revision a revert "
	       "refuses anyway, so the lag is invisible" );

	const AgentSession::AgentRevertResult rr = s->RevertToRevision( rev0 );
	Check( rr.ok && rr.applied, std::string( "A MONEY: the revert applied -- " ) + rr.message );
	Check( rr.status == "applied", "A: with status \"applied\"" );
	Check( rr.requestedRevision == rev0, "A: the result echoes the revision asked for" );
	Check( rr.previousRevision == revs[3], "A: and the head it left, which is what undoes this restore" );

	const std::uint64_t revNew = s->ReadHeadVersion().revision;
	Check( revNew > revs[3],
	       "A MONEY: the head went UP -- the restore is a NEW commit, not a rewind of the counter" );
	Check( rr.headVersion.revision == revNew, "A: and the result names it" );
	Check( s->ReadDocument() == doc0,
	       "A MONEY: the document is BYTE-IDENTICAL to what it was at that revision -- not "
	       "similar, not re-serialized differently: the same bytes" );
	Check( RendersOk( *s ), "A: and the restored scene still derives and renders" );
}

//======================================================================
// B. Revert the revert.
//======================================================================
static void CaseB( const std::string& scenePath )
{
	std::printf( "[B] revert-the-revert -- append-only in both directions\n" );
	std::unique_ptr<AgentSession> s = AgentSession::LoadFromFile( scenePath );
	Check( s != nullptr, "B: the fixture loads" );
	if( !s ) return;

	const std::uint64_t revA = s->ReadHeadVersion().revision;
	const std::string   docA = s->ReadDocument();
	Check( BumpRevision( *s, 0.31 ), "B: edit 1 applies" );
	Check( BumpRevision( *s, 0.32 ), "B: edit 2 applies" );
	const std::uint64_t revB = s->ReadHeadVersion().revision;
	const std::string   docB = s->ReadDocument();

	const AgentSession::AgentRevertResult back = s->RevertToRevision( revA );
	Check( back.applied, std::string( "B: the revert to the original applies -- " ) + back.message );
	const std::uint64_t revC = s->ReadHeadVersion().revision;
	Check( s->ReadDocument() == docA, "B: and the original bytes are back" );

	// THE POINT OF THE CASE.  `revB` is the head the revert left behind.  A
	// rewind implementation would have lost it; an append-only one still
	// holds it, so the model can change its mind.
	Check( s->HasRecordedRevision( revB ),
	       "B MONEY: the head the revert LEFT is still recorded -- undoing the undo is possible "
	       "because nothing was thrown away" );
	const AgentSession::AgentRevertResult fwd = s->RevertToRevision( revB );
	Check( fwd.applied, std::string( "B MONEY: reverting to the pre-revert head applies too -- " ) + fwd.message );
	Check( s->ReadDocument() == docB, "B: with THOSE bytes restored exactly" );
	const std::uint64_t revD = s->ReadHeadVersion().revision;
	Check( revD > revC && revC > revB && revB > revA,
	       "B MONEY: every head in the sequence is higher than the last -- four commits, no rewind, "
	       "and the last two of them restored older content" );
}

//======================================================================
// C. The ring: eviction, and the anchor's pin against it.
//======================================================================
static void CaseC( const std::string& scenePath )
{
	std::printf( "[C] the ring evicts oldest-first, and the ANCHOR's revision survives it\n" );
	// The protocol switches are OFF for this session (main() sets them so),
	// which arms the ratchet on the FIRST qualifying render -- the anchor
	// definition for a session with no build plan.  That is what lets one
	// render pin an anchor at a known, early revision.
	std::unique_ptr<AgentSession> s = AgentSession::LoadFromFile( scenePath );
	Check( s != nullptr, "C: the fixture loads" );
	if( !s ) return;

	const AgentRenderResult r0 = s->Render( FullFrameParams( 32 ) );
	Check( r0.ok, std::string( "C: the first render succeeded: " ) + r0.message );
	Check( r0.anchorEstablished, "C: (premise) it BECAME the session's anchor" );
	const std::uint64_t anchorRev = s->RenderAnchorRevision();
	Check( anchorRev != 0 && anchorRev == r0.anchorRevision, "C: at a known revision" );
	Check( s->HasRecordedRevision( anchorRev ),
	       "C MONEY: the RENDER recorded the document it was made from -- without this capture "
	       "point the model would be told to revert to a revision the session never held" );
	const std::string anchorDoc = s->ReadDocument();

	// Overflow the entry cap.  kRevisionRingMaxEntries is 256; every edit
	// below is one head bump and one recorded revision.
	const int kEdits = 300;
	for( int i = 0; i < kEdits; ++i ) {
		if( !BumpRevision( *s, 0.20 + 0.001 * i ) ) {
			Check( false, "C: edit " + std::to_string( i ) + " applied" );
			return;
		}
	}
	Check( s->RecordedRevisionCount() <= 257,
	       "C: the ring is BOUNDED after 300 revisions (" +
	           std::to_string( (unsigned long long)s->RecordedRevisionCount() ) +
	           " entries: at most the cap plus the pinned anchor)" );
	Check( s->RecordedRevisionCount() >= 256,
	       "C: and it really is holding the cap, not silently dropping everything" );
	// THE PREMISE THE PIN IS TESTED AGAINST: eviction has swept PAST the
	// anchor's revision -- its immediate successors are gone -- so an
	// unpinned entry that old could not possibly still be here.
	Check( !s->HasRecordedRevision( anchorRev + 1 ),
	       "C: (premise) revisions immediately AFTER the anchor's have been evicted, so an entry "
	       "of that age survives only by being pinned" );

	Check( s->HasRecordedRevision( anchorRev ),
	       "C MONEY: the anchor's revision SURVIVED eviction -- R1's render note tells the model "
	       "to pass exactly this number, so the ring must never age it out while the anchor lives" );
	Check( s->OldestRecordedRevision() == anchorRev,
	       "C: and it is the OLDEST entry left -- the evictor stepped over it and took the "
	       "next-oldest instead, rather than stopping at it" );

	const AgentSession::AgentRevertResult back = s->RevertToRevision( anchorRev );
	Check( back.applied,
	       std::string( "C MONEY: and it is still restorable after 300 later revisions -- " ) + back.message );
	Check( s->ReadDocument() == anchorDoc, "C: byte-exactly" );

	// AN EVICTED REVISION refuses, and the refusal carries the next call.
	const std::uint64_t evicted = anchorRev + 1;
	Check( !s->HasRecordedRevision( evicted ), "C: (premise) revision anchorRev+1 has been evicted" );
	const AgentSession::AgentRevertResult ev = s->RevertToRevision( evicted );
	Check( !ev.ok && !ev.applied, "C: reverting to an evicted revision is refused" );
	Check( ev.message.find( "no longer held" ) != std::string::npos,
	       "C: naming the RULE that fired (aged out), not a generic failure" );
	Check( ev.message.find( std::to_string( (unsigned long long)s->OldestRecordedRevision() ) ) !=
	           std::string::npos,
	       "C MONEY: and the refusal NAMES the oldest revision still available, so the answer "
	       "carries the model's next call instead of only a \"no\"" );
	Check( ev.oldestAvailableRevision == s->OldestRecordedRevision(),
	       "C: the same number rides the result struct, for a caller that does not parse prose" );
}

//======================================================================
// D. The element ledger.
//======================================================================
static std::vector<AgentSession::AgentBuildPlanEntry> OneElementPlan()
{
	std::vector<AgentSession::AgentBuildPlanEntry> p;
	AgentSession::AgentBuildPlanEntry a;
	a.element = "tower";
	a.pieces.push_back( "shaft" );
	a.construction.push_back( "csg" );
	a.outline = "0 0; 2 0; 1.2 4; 0.8 4";
	p.push_back( a );
	AgentSession::AgentBuildPlanEntry b;
	b.element = "base";
	b.pieces.push_back( "slab" );
	b.construction.push_back( "displaced" );
	b.outline = "0 0; 8 0; 8 1; 0 1";
	p.push_back( b );
	return p;
}

static std::string BoxChunk( const char* name )
{
	return std::string( "box_geometry\n{\n\tname " ) + name +
		"\n\twidth 1.0\n\theight 1.0\n\tdepth 1.0\n}\n";
}

static void CaseD( const std::string& scenePath )
{
	std::printf( "[D] the element ledger after a revert: attributions pruned, phases NOT rewound\n" );
	AgentSession::SetBuildProtocolDefaultEnabled( true );
	AgentSession::SetBuildPlanGateDefaultEnabled( true );
	std::unique_ptr<AgentSession> s = AgentSession::LoadFromFile( scenePath );
	AgentSession::SetBuildProtocolDefaultEnabled( false );
	AgentSession::SetBuildPlanGateDefaultEnabled( false );
	Check( s != nullptr, "D: the fixture loads" );
	if( !s ) return;
	Check( s->BuildProtocolActive(), "D: (premise) the staged build protocol is in force" );

	Check( s->FileBuildPlan( OneElementPlan() ).ok, "D: the plan files" );
	Check( s->ActiveElement() == "tower", "D: with the first element active" );

	// The revision BEFORE the element's chunk exists.  Captured by the
	// insert below, which starts from it.
	const std::uint64_t revBefore = s->ReadHeadVersion().revision;
	const std::string   docBefore = s->ReadDocument();

	Check( s->InsertChunk( BoxChunk( "tower_shaft" ) ).applied, "D: the element's geometry inserts" );
	Check( s->ChunkElement( "tower_shaft" ) == "tower",
	       "D: (premise) and is attributed to the active element" );

	const AgentSession::AgentRevertResult rr = s->RevertToRevision( revBefore );
	Check( rr.applied, std::string( "D: the revert to before the element existed applies -- " ) + rr.message );
	Check( s->ReadDocument() == docBefore, "D: bytes exact" );

	Check( s->ChunkElement( "tower_shaft" ).empty(),
	       "D MONEY: the attribution for the vanished chunk was DROPPED -- an attribution that "
	       "outlives its chunk makes the cross-element gate refuse an edit naming a chunk that "
	       "does not exist, which reopen_element cannot fix and which spends one of three shared "
	       "phase-refusal slots" );
	Check( rr.droppedAttributions == 1, "D: and the result says how many it dropped" );
	Check( rr.message.find( "bookkeeping was dropped" ) != std::string::npos,
	       "D: in prose too, so a model reading the result knows its ledger moved" );

	Check( s->BuildPhase() == AgentSession::AgentBuildPhase::Pieces,
	       "D MONEY: the PHASE is NOT rewound -- it records what the model did, not what the "
	       "document holds" );
	Check( s->ActiveElement() == "tower", "D: and the active element is untouched" );

	// NOT STRANDED.  The same element builds again and closes, which is the
	// only claim that matters: a ledger left in an impossible state would
	// refuse one of these two calls.
	Check( s->InsertChunk( BoxChunk( "tower_shaft" ) ).applied,
	       "D MONEY: the element builds AGAIN after the revert -- the protocol is not stranded" );
	Check( s->ChunkElement( "tower_shaft" ) == "tower", "D: re-attributed to the same element" );
	const AgentSession::AgentFinishElementResult f = s->FinishElement();
	Check( f.ok && f.element == "tower", "D: and finish_element closes it" );
	Check( f.chunks.size() == 1 && f.chunks[0] == "tower_shaft",
	       "D: reporting exactly ONE chunk -- the pruned entry did not come back as a ghost, and "
	       "the rebuilt one was not recorded twice" );
	Check( s->ActiveElement() == "base", "D: advancing to the next element as usual" );
}

//======================================================================
// E. Undo through a live controller.
//======================================================================
static void CaseE( const std::string& scenePath )
{
	std::printf( "[E] ONE Undo() reverses the revert (the real Cmd-Z path)\n" );
	Job* pJob = LoadJob( scenePath );
	Check( pJob != nullptr, "E: the fixture loads" );
	if( !pJob ) return;

	{
		QuietController c( *pJob );
		std::unique_ptr<AgentSession> s = AgentSession::WrapJob( pJob );
		s->AttachController( &c );

		const std::uint64_t rev0 = s->ReadHeadVersion().revision;
		const std::string   doc0 = s->ReadDocument();
		Check( BumpRevision( *s, 0.44 ), "E: an edit applies through the controller" );
		const std::string docEdited = s->ReadDocument();
		Check( docEdited != doc0, "E: (premise) it really changed the document" );

		const AgentSession::AgentRevertResult rr = s->RevertToRevision( rev0 );
		Check( rr.applied, std::string( "E: the revert applies through the controller -- " ) + rr.message );
		Check( s->ReadDocument() == doc0, "E: restoring the original bytes" );

		// ONE Undo(), not two: the whole restore is ONE history record, so a
		// single Cmd-Z must put the reverted-away document back.
		c.Undo();
		Check( s->ReadDocument() == docEdited,
		       "E MONEY: ONE Undo() reverses the revert exactly -- the restore is one undoable "
		       "unit on the SAME stack every other edit uses, so the user is never left half "
		       "way between two documents" );

		c.Redo();
		Check( s->ReadDocument() == doc0,
		       "E: and Redo() reinstalls the restored document -- the revert is a real history "
		       "entry, not a discarded one" );

		s->AttachController( nullptr );
	}
	pJob->release();
}

//======================================================================
// F. The establishing render does not name the verb.
//======================================================================
static void CaseF( const std::string& scenePath )
{
	std::printf( "[F] the render that BECOMES the anchor does not name the verb\n" );
	std::unique_ptr<AgentSession> s = AgentSession::LoadFromFile( scenePath );
	Check( s != nullptr, "F: the fixture loads" );
	if( !s ) return;

	const AgentRenderResult r0 = s->Render( FullFrameParams( 32 ) );
	Check( r0.ok && r0.anchorEstablished, "F: (premise) the first render establishes the anchor" );
	Check( r0.message.find( "revert_to_revision" ) == std::string::npos,
	       "F MONEY: the ESTABLISHING render's note does NOT name revert_to_revision -- there is "
	       "nothing yet to go back to, and doc 90 sec 2's rule cuts both ways: an offer with no "
	       "destination burns the same repair budget a stale promise does" );

	// And the composite render DOES name it, with the anchor's revision.
	// (AgentRenderAnchorTest owns the detailed assertion; this is the
	// two-sided pin that keeps THIS case from passing on a note that never
	// names the verb at all.)
	Check( BumpRevision( *s, 0.55 ), "F: an edit between the two renders" );
	const AgentRenderResult r1 = s->Render( FullFrameParams( 32 ) );
	Check( r1.ok && r1.anchorApplied && !r1.anchorEstablished, "F: the second render is anchored" );
	Check( r1.message.find( "revert_to_revision " +
	                        std::to_string( (unsigned long long)r0.anchorRevision ) ) != std::string::npos,
	       "F: and the COMPOSITE note names the verb with the anchor's revision -- the sentence "
	       "R1 left as a marked TODO" );
	// And the promise is honest at the moment it is made.
	Check( s->HasRecordedRevision( r0.anchorRevision ),
	       "F MONEY: the session can really restore the revision the note just named" );
}

//======================================================================
// G. The refusal matrix.
//======================================================================
static void CaseG( const std::string& scenePath )
{
	std::printf( "[G] the refusal matrix -- every one leaves the document byte-identical\n" );
	std::unique_ptr<AgentSession> s = AgentSession::LoadFromFile( scenePath );
	Check( s != nullptr, "G: the fixture loads" );
	if( !s ) return;

	const std::uint64_t rev0 = s->ReadHeadVersion().revision;
	Check( BumpRevision( *s, 0.61 ), "G: one edit, so there is something to go back to" );
	const std::uint64_t head = s->ReadHeadVersion().revision;
	const std::string   doc  = s->ReadDocument();

	struct Case { std::uint64_t revision; const char* needle; const char* what; };
	const Case cases[] = {
		{ 0,           "no-head sentinel",        "revision 0" },
		{ head,        "IS the current head",     "the current head" },
		{ head + 100,  "never had a revision",    "a future revision" },
	};
	for( const Case& c : cases ) {
		const AgentSession::AgentRevertResult r = s->RevertToRevision( c.revision );
		Check( !r.ok && !r.applied,
		       std::string( "G: " ) + c.what + " is refused (ok false, nothing applied)" );
		Check( r.status.empty(),
		       std::string( "G: " ) + c.what + " leaves status EMPTY -- the pre-commit refusal "
		       "convention, so a caller branches on `applied`" );
		Check( r.message.find( c.needle ) != std::string::npos,
		       std::string( "G: " ) + c.what + " refuses for the RIGHT reason (message contains \"" +
		       c.needle + "\"), not merely for some reason" );
		Check( s->ReadDocument() == doc,
		       std::string( "G: " ) + c.what + " left the document BYTE-IDENTICAL" );
		Check( s->ReadHeadVersion().revision == head,
		       std::string( "G: " ) + c.what + " did not move the head" );
	}

	// AN UNRECORDED (never observed) revision, distinguished from an evicted
	// one: nothing has been evicted in this session, so a gap must not claim
	// it aged out.
	{
		// rev0 IS recorded (the edit started from it), so manufacture a gap by
		// asking for a revision between the two that no call ever observed.
		// With one edit there is none, so use rev0-1 when it is a real
		// revision, else assert the "holds nothing older" wording.
		const std::uint64_t gap = ( rev0 > 1 ) ? ( rev0 - 1 ) : 0;
		if( gap != 0 ) {
			const AgentSession::AgentRevertResult r = s->RevertToRevision( gap );
			Check( !r.ok && !r.applied, "G: a revision this session never observed is refused" );
			Check( r.message.find( "no copy of the document at revision" ) != std::string::npos,
			       "G MONEY: and it is refused as a GAP, not as an eviction -- nothing has aged "
			       "out here, and telling a model its revision expired when the session simply "
			       "never looked would send it hunting for a newer number that does not exist" );
			Check( s->ReadDocument() == doc, "G: document byte-identical" );
		}
	}

	// EXTERNAL AUTHORITY: no staged-proposal form for a whole-document swap.
	//
	// Getting an External session to a state where the refusal can even fire
	// takes the loopback topology this posture actually runs in: TWO sessions
	// over ONE Job.  An External session cannot commit, so it cannot move the
	// head itself -- and a revision that is still the head is refused as the
	// head, which would test the wrong rule.  So it RENDERS (read-safe, and
	// the ring's other capture point) to record the document it is looking
	// at, an Owner session moves the head underneath it exactly as a
	// co-editor would, and only then is the earlier revision a real target.
	// The refusal must then fire at the COMMIT stage, after the request has
	// been found perfectly well-formed.
	{
		Job* pJob = LoadJob( scenePath );
		Check( pJob != nullptr, "G: the External fixture loads" );
		if( pJob ) {
			std::unique_ptr<AgentSession> ext =
				AgentSession::WrapJob( pJob, AgentAuthority::External );
			std::unique_ptr<AgentSession> owner = AgentSession::WrapJob( pJob );
			const std::uint64_t r0 = ext->ReadHeadVersion().revision;
			Check( ext->Render( FullFrameParams( 32 ) ).ok, "G: the External session can render" );
			Check( ext->HasRecordedRevision( r0 ),
			       "G MONEY: a RENDER records the revision it was made from even in a session that "
			       "cannot commit -- observation is a capture point, not just mutation" );
			Check( BumpRevision( *owner, 0.71 ), "G: an Owner session moves the head underneath it" );
			const std::string before = ext->ReadDocument();
			const AgentSession::AgentRevertResult r = ext->RevertToRevision( r0 );
			Check( !r.ok && !r.applied, "G: External authority refuses the revert" );
			Check( r.message.find( "External-authority" ) != std::string::npos &&
			       r.message.find( "no staged-proposal form" ) != std::string::npos,
			       std::string( "G: naming the posture and the reason (one composite swap is no "
			       "proposal kind an Owner could approve card-by-card) -- got: " ) + r.message );
			Check( ext->ReadDocument() == before, "G: document byte-identical" );
			pJob->release();
		}
	}
}

//======================================================================
// H. The wire.
//======================================================================
static void CaseH( const std::string& scenePath )
{
	std::printf( "[H] the wire: RPC, chat codec, MCP (advertised AND routable), autonomy\n" );

	// --- RPC dispatch, the real path a hosted client takes.
	{
		std::unique_ptr<AgentSession> s = AgentSession::LoadFromFile( scenePath );
		Check( s != nullptr, "H: the fixture loads" );
		if( !s ) return;
		const std::uint64_t rev0 = s->ReadHeadVersion().revision;
		Check( BumpRevision( *s, 0.66 ), "H: one edit" );
		const std::string doc0Now = s->ReadDocument();

		AgentRpcDispatcher disp( std::move( s ) );
		const std::string req =
			"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"revert_to_revision\",\"params\":{\"revision\":" +
			std::to_string( (unsigned long long)rev0 ) + "}}";
		const std::string resp = disp.HandleLine( req );
		JsonValue env; std::string perr;
		Check( JsonParse( resp, env, perr ), "H: the response parses" );
		Check( !env.has( "error" ), "H: revert_to_revision dispatches (no JSON-RPC error)" );
		const JsonValue& res = env.get( "result" );
		Check( res.get( "ok" ).asBool() && res.get( "applied" ).asBool(),
		       "H: and the wire result reports the restore" );
		Check( res.get( "status" ).asString() == "applied", "H: with status applied" );
		Check( res.has( "requestedRevision" ) && res.has( "previousRevision" ) &&
		       res.has( "oldestAvailableRevision" ) && res.has( "headVersion" ),
		       "H: carrying the documented fields (the numbers a model needs to go back again)" );
		Check( (unsigned long long)res.get( "requestedRevision" ).asNumber() ==
		           (unsigned long long)rev0,
		       "H: requestedRevision echoes the request" );

		// A MALFORMED call is a JSON-RPC error; a REFUSAL is not.
		const std::string bad = disp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"revert_to_revision\",\"params\":{}}" );
		JsonValue badEnv; std::string e2;
		Check( JsonParse( bad, badEnv, e2 ) && badEnv.has( "error" ),
		       "H: a MISSING revision is a JSON-RPC error -- that one really is a malformed call" );
		const std::string refused = disp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"revert_to_revision\",\"params\":{\"revision\":999999}}" );
		JsonValue refEnv; std::string e3;
		Check( JsonParse( refused, refEnv, e3 ) && !refEnv.has( "error" ) &&
		       !refEnv.get( "result" ).get( "ok" ).asBool(),
		       "H MONEY: a REFUSAL is a SUCCESSFUL response with ok:false -- \"that revision does "
		       "not exist yet\" is an answer, and a model must be able to read it as one" );
	}

	// --- The shared chat-codec tool table (all three provider renderings
	//     come from it, so one entry is the whole claim).
	{
		// ONE table, four provider formatters -- so the table IS the claim
		// for every provider (AgentVaryMaterialTest's own convention).
		const std::string defs = ChatToolDefsFingerprint();
		Check( defs.find( "revert_to_revision" ) != std::string::npos,
		       "H: the verb is declared in the shared kToolDefs table, so every provider codec "
		       "carries it" );
		Check( defs.find( "PUT THE WHOLE DOCUMENT BACK" ) != std::string::npos,
		       "H: with the description a model actually reads" );
	}

	// --- MCP: ADVERTISED and ROUTABLE.  The 1ed4e7c3 lesson -- a verb on
	//     tools/list but missing from IsKnownToolName answers -32601 to
	//     every call.
	{
		Job* pJob = LoadJob( scenePath );
		Check( pJob != nullptr, "H: the MCP fixture loads" );
		if( pJob ) {
			AgentMcpAdapter mcp( AgentSession::WrapJob( pJob ), AgentAutonomy::Commit );
			const std::string list = mcp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}" );
			Check( list.find( "\"revert_to_revision\"" ) != std::string::npos,
			       "H: tools/list ADVERTISES it" );
			const std::string call = mcp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
				"{\"name\":\"revert_to_revision\",\"arguments\":{\"revision\":999999}}}" );
			Check( call.find( "-32601" ) == std::string::npos,
			       "H MONEY: and tools/call ROUTES it -- not \"method not found\", which is what a "
			       "verb advertised but missing from IsKnownToolName answers" );
			pJob->release();
		}
	}

	// --- Autonomy: refused under Read and under Propose, with a
	//     Propose-specific message rather than the generic read fallback.
	{
		Job* pJob = LoadJob( scenePath );
		if( pJob ) {
			AgentRpcDispatcher disp( AgentSession::WrapJob( pJob ), AgentAutonomy::Read );
			const std::string resp = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"revert_to_revision\",\"params\":{\"revision\":1}}" );
			Check( resp.find( "error" ) != std::string::npos,
			       "H: refused under Read autonomy -- it MUTATES, so it is not on the read-safe list" );
			pJob->release();
		}
		Job* pJob2 = LoadJob( scenePath );
		if( pJob2 ) {
			AgentRpcDispatcher disp( AgentSession::WrapJob( pJob2 ), AgentAutonomy::Propose );
			const std::string resp = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"revert_to_revision\",\"params\":{\"revision\":1}}" );
			Check( resp.find( "error" ) != std::string::npos, "H: refused under Propose autonomy too" );
			Check( resp.find( "propose" ) != std::string::npos &&
			       resp.find( "revert_to_revision" ) != std::string::npos,
			       "H: with the Propose-SPECIFIC message (it names the posture and the verb), not "
			       "the generic read-posture fallback" );
			pJob2->release();
		}
	}
}

//======================================================================
// I. The head LINEAGE, and why the ring's revision key is sufficient.
//======================================================================
static void CaseI( const std::string& scenePath )
{
	std::printf( "[I] the head lineage cannot change under a session -- and the ring says so anyway\n" );
	Job* pJob = LoadJob( scenePath );
	Check( pJob != nullptr, "I: the fixture loads" );
	if( !pJob ) return;
	{
		std::unique_ptr<AgentSession> s = AgentSession::WrapJob( pJob );
		const std::uint64_t uuid0 = s->ReadHeadVersion().uuid;
		Check( uuid0 != 0, "I: the head carries a lineage uuid" );
		Check( BumpRevision( *s, 0.81 ), "I: one edit, so the ring holds the load revision" );
		Check( s->RecordedRevisionCount() > 0, "I: (premise) the ring is not empty" );

		// THE INVARIANT THE RING RESTS ON.  `revision` restarts at 1 on every
		// fresh load, so a ring keyed on that number alone would be unsafe IF a
		// session's Job could be reloaded underneath it: "restore revision 1"
		// could then hand back the PREVIOUS scene.  It cannot -- a Job refuses
		// re-load outright, so one Job is one lineage for its whole life and a
		// new scene means a new Job and a new session.  Pinned here rather than
		// assumed, because the ring's key is only sufficient while it holds.
		Check( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ),
		       "I MONEY: a Job REFUSES a second load -- so the head lineage a session records "
		       "against can never change underneath it, which is what makes a revision number a "
		       "sufficient ring key" );
		Check( s->ReadHeadVersion().uuid == uuid0,
		       "I: and the lineage really is unchanged after the refused reload" );

		// The ring records the whole (uuid, revision) anyway, and clears itself
		// on a lineage change -- belt and braces against a future load path that
		// does not have today's refusal.  Nothing here can exercise that arm;
		// what IS asserted is that the ordinary path still works after the
		// refused reload, i.e. the guard has not misfired.
		const std::uint64_t oldest = s->OldestRecordedRevision();
		const AgentSession::AgentRevertResult r = s->RevertToRevision( oldest );
		Check( r.applied,
		       std::string( "I: a revert still works after the refused reload -- the lineage guard "
		       "did not misfire on the ONE lineage that exists -- " ) + r.message );
	}
	pJob->release();
}

//======================================================================
// J. The ledger's other half: a revert that brings a chunk BACK brings
//    its attribution back with it -- including into a FINISHED element.
//======================================================================
static void CaseJ( const std::string& scenePath )
{
	std::printf( "[J] revert-the-revert RESTORES attribution, and a finished element copes\n" );
	Job* pJob = LoadJob( scenePath );
	Check( pJob != nullptr, "J: the fixture loads" );
	if( !pJob ) return;
	AgentSession::SetBuildProtocolDefaultEnabled( true );
	AgentSession::SetBuildPlanGateDefaultEnabled( true );
	// PINNED TO BUILDING.  This fixture carries form (a lit sphere), so it
	// classifies REFINING, in which every construction gate is inert by
	// design -- and the last claim below is precisely that the restored
	// attribution GATES an edit.  Without the pin the case would pass for
	// the wrong reason.  (Case D does not need the pin: attribution and the
	// phase transitions run in Refining too; only the REFUSALS are off.)
	std::unique_ptr<AgentSession> s =
		AgentSession::WrapJobWithSessionMode( pJob, AgentSession::AgentSessionMode::Building );
	AgentSession::SetBuildProtocolDefaultEnabled( false );
	AgentSession::SetBuildPlanGateDefaultEnabled( false );
	Check( s != nullptr, "J: the session wraps it" );
	if( !s ) { pJob->release(); return; }
	Check( s->BuildProtocolActive(), "J: (premise) the staged build protocol is in force" );

	Check( s->FileBuildPlan( OneElementPlan() ).ok, "J: the plan files" );
	Check( s->ActiveElement() == "tower", "J: with the first element active" );

	const std::uint64_t revBefore = s->ReadHeadVersion().revision;
	Check( s->InsertChunk( BoxChunk( "tower_shaft" ) ).applied, "J: the element's geometry inserts" );
	// THE REVISION AT WHICH THE CHUNK EXISTS AND IS ATTRIBUTED.  It enters
	// the ring on the next mutating verb -- which is the first revert
	// below, since a revert records the head it starts from.
	const std::uint64_t revWith = s->ReadHeadVersion().revision;
	Check( s->ChunkElement( "tower_shaft" ) == "tower",
	       "J: (premise) and is attributed to the active element" );

	// --- Away.  This is case D's ground, re-walked only far enough to set
	//     up the return trip.
	const AgentSession::AgentRevertResult away = s->RevertToRevision( revBefore );
	Check( away.applied, std::string( "J: the revert to before the chunk existed applies -- " ) + away.message );
	Check( away.droppedAttributions == 1, "J: (premise) it dropped the vanished chunk's attribution" );
	Check( away.restoredAttributions == 0,
	       "J: and restored none -- there was nothing to bring back on the way out" );
	Check( s->ChunkElement( "tower_shaft" ).empty(), "J: (premise) the ledger really is empty of it" );

	// --- Back.  THE CASE.
	Check( s->HasRecordedRevision( revWith ),
	       "J: (premise) the revision where the chunk existed is in the ring -- the revert above "
	       "recorded the head it started from" );
	const AgentSession::AgentRevertResult back = s->RevertToRevision( revWith );
	Check( back.applied, std::string( "J: the revert-the-revert applies -- " ) + back.message );
	Check( s->ReadDocument().find( "tower_shaft" ) != std::string::npos,
	       "J: (premise) the chunk's TEXT is back in the document" );

	Check( s->ChunkElement( "tower_shaft" ) == "tower",
	       "J MONEY: and so is its ATTRIBUTION -- a chunk that is visibly back in the document but "
	       "recorded against no element is freely editable from any other element's window and is "
	       "no longer form-protected, when it is really that element's own content" );
	Check( back.restoredAttributions == 1, "J: and the result says how many it brought back" );
	Check( back.droppedAttributions == 0, "J: with nothing dropped on this leg" );
	Check( back.message.find( "re-recorded against the build element" ) != std::string::npos,
	       "J: in prose too, so a model reading the result knows its ledger moved back" );

	const std::vector<std::string> tower = s->ElementChunks( "tower" );
	Check( tower.size() == 1 && tower[0] == "tower_shaft",
	       "J MONEY: element_chunks lists it exactly ONCE -- the restore filled a gap, it did not "
	       "append a duplicate beside a live entry" );

	// --- The FINISHED-ELEMENT combination, exercised rather than inspected.
	const AgentSession::AgentFinishElementResult f = s->FinishElement();
	Check( f.ok && f.element == "tower", "J: finish_element closes the element" );
	Check( f.chunks.size() == 1 && f.chunks[0] == "tower_shaft",
	       "J MONEY: reporting the RESTORED chunk as its content -- the strongest form of the "
	       "claim, since finish_element reads the same ledger every gate reads" );
	Check( s->ActiveElement() == "base", "J: and moves on to the next element" );

	// "tower" is now FINISHED and the restored attribution names it.  That
	// needs no special case: the gates compare against the ACTIVE element by
	// name and never consult the finished flags, so the edit is refused
	// NAMING "tower" and pointing at reopen_element -- exactly as it would
	// be for an attribution that never went away.
	AgentSetPatch p;
	p.target = "tower_shaft";
	p.param  = "width";
	p.value  = "2.0";
	const AgentPatchResult crossed = s->ProposePatch( p );
	Check( !crossed.applied,
	       "J MONEY: an edit on the restored chunk from ANOTHER element's window is refused -- the "
	       "restored attribution is live protection, not decoration" );
	Check( crossed.message.find( "tower" ) != std::string::npos &&
	       crossed.message.find( "reopen_element" ) != std::string::npos,
	       "J: naming the element it belongs to and the route back in, not a generic refusal" );

	const AgentSession::AgentReopenElementResult re = s->ReopenElement( "tower" );
	Check( re.ok && re.element == "tower",
	       std::string( "J: reopen_element re-enters the FINISHED element -- " ) + re.message );
	Check( re.chunks.size() == 1 && re.chunks[0] == "tower_shaft",
	       "J: reporting the restored chunk as already attributed to it" );
	Check( s->ProposePatch( p ).applied,
	       "J MONEY: and the same edit now LANDS -- the documented route back works on a restored "
	       "attribution exactly as on an original one, so the combination needs no special case" );
	Check( s->ChunkElement( "tower_shaft" ) == "tower",
	       "J: with the attribution still naming the element after the whole finish/reopen round "
	       "trip -- the restore put back a durable record, not a one-call ghost" );

	s.reset();
	pJob->release();
}

//======================================================================
// K. A gap is not an eviction, even under hundreds of later evictions.
//======================================================================
static void CaseK( const std::string& scenePath )
{
	std::printf( "[K] a NEVER-CAPTURED revision is reported as a GAP, not as \"aged out\"\n" );
	Job* pJob = LoadJob( scenePath );
	Check( pJob != nullptr, "K: the fixture loads" );
	if( !pJob ) return;
	{
		// A REAL controller, so the test can move the head WITHOUT going
		// through the session -- which is the only way to manufacture a
		// revision the session never captured.  This is the GUI hand-edit
		// shape: the head advances while no agent call is looking.
		QuietController c( *pJob );
		std::unique_ptr<AgentSession> s = AgentSession::WrapJob( pJob );
		s->AttachController( &c );

		Check( BumpRevision( *s, 0.41 ), "K: one ordinary session edit, to seed the ring" );

		// TWO raw controller edits back to back, with NO session call
		// between them.  The head passes through `gapRev` and out the other
		// side, and nothing captures it -- the second edit is what makes it
		// an interior gap rather than merely the current head.
		const SceneEditController::AgentCommitResult e1 =
			c.ApplyAgentParamEdit( String( "sph" ), String( "" ), String( "radius" ),
			                       String( "0.42" ), nullptr );
		Check( e1.applied, "K: the first raw controller edit applies" );
		const std::uint64_t gapRev = pJob->GetCstHeadVersion().revision;
		const SceneEditController::AgentCommitResult e2 =
			c.ApplyAgentParamEdit( String( "sph" ), String( "" ), String( "radius" ),
			                       String( "0.43" ), nullptr );
		Check( e2.applied, "K: the second raw controller edit applies" );
		// The head AFTER the gap.  The NEXT session call captures it (a
		// mutating verb records the head it starts from), so it is the
		// genuinely-recorded-then-evicted revision the complement below
		// needs -- derived, not assumed to be gapRev+1.
		const std::uint64_t afterGap = pJob->GetCstHeadVersion().revision;
		Check( afterGap > gapRev,
		       "K: (premise) the head moved PAST the gap revision, so it is interior" );
		Check( !s->HasRecordedRevision( gapRev ),
		       "K: (premise) and the session never captured it" );

		// Now overflow the ring so eviction sweeps well past gapRev's
		// NUMERIC position.  THIS is the compound case: under the old bare
		// "highest revision ever evicted" high-water mark, gapRev is now
		// below the mark and gets called "aged out" -- a revision that was
		// never here at all being described as one that was.
		const int kEdits = 300;
		for( int i = 0; i < kEdits; ++i ) {
			if( !BumpRevision( *s, 0.20 + 0.001 * i ) ) {
				Check( false, "K: edit " + std::to_string( i ) + " applied" );
				return;
			}
		}
		Check( s->OldestRecordedRevision() > gapRev,
		       "K: (premise) eviction has swept PAST the gap revision -- a high-water mark now sits "
		       "far above it" );

		const AgentSession::AgentRevertResult gap = s->RevertToRevision( gapRev );
		Check( !gap.ok && !gap.applied, "K: reverting to it is refused, as it must be" );
		Check( gap.message.find( "no copy of the document at revision" ) != std::string::npos,
		       "K MONEY: with the GAP wording -- this session never held that head, and no later "
		       "choice of revision recovers it" );
		Check( gap.message.find( "aged out" ) == std::string::npos &&
		       gap.message.find( "no longer held" ) == std::string::npos,
		       "K MONEY: and NOT the eviction wording -- \"it aged out, pick a later one\" is a LIE "
		       "about a revision that was never recorded, and it sends the model hunting through "
		       "revisions that will not help either" );

		// THE COMPLEMENT, so the case cannot pass by simply never saying
		// "aged out": a revision that genuinely WAS captured and has since
		// been evicted still gets the eviction wording.
		const std::uint64_t evicted = afterGap;
		Check( !s->HasRecordedRevision( evicted ),
		       "K: (premise) the revision right after the gap was captured and has been evicted" );
		const AgentSession::AgentRevertResult ev = s->RevertToRevision( evicted );
		Check( ev.message.find( "no longer held" ) != std::string::npos,
		       "K MONEY: a genuinely EVICTED revision still says so -- the two refusals stayed "
		       "distinguishable, which is the whole reason the gap branch exists" );
	}
	pJob->release();
}

//----------------------------------------------------------------------
int main()
{
	std::printf( "=== AgentRevertRevisionTest (doc 90 slice R2: the way back) ===\n" );

	// The phase machinery is OFF for most of this binary (case D turns it on
	// for itself): with it off the ratchet arms on the first qualifying
	// render, which is what lets case C pin an anchor at a known early
	// revision without staging a whole build.
	AgentSession::SetBuildProtocolDefaultEnabled( false );
	AgentSession::SetBuildPlanGateDefaultEnabled( false );

	const std::string scenePath = WriteTemp( "rise_agent_revert.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the scene to a temp file" );
	if( scenePath.empty() ) return 1;

	CaseA( scenePath );
	CaseB( scenePath );
	CaseC( scenePath );
	CaseD( scenePath );
	CaseE( scenePath );
	CaseF( scenePath );
	CaseG( scenePath );
	CaseH( scenePath );
	CaseI( scenePath );
	CaseJ( scenePath );
	CaseK( scenePath );

	std::remove( scenePath.c_str() );
	std::printf( "=== AgentRevertRevisionTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
