//////////////////////////////////////////////////////////////////////
//
//  CsgOperandTransformTest.cpp - locks in the "87" csg_object operand-rebase
//  semantics reported as an all-black-render bug and empirically re-diagnosed
//  as WORKING AS DESIGNED (docs/SCENE_CONVENTIONS.md "csg_object operand
//  transforms are CSG-local"): an operand's transform is interpreted in its
//  csg_object's LOCAL frame, so when the csg_object ALSO carries its own
//  position/orientation, that transform composes with (not replaces, not
//  double-applies) each operand's transform -- CSGObject::IntersectRay reads
//  world -> csg-local via the csg's inverse matrix, then the operand applies
//  its own matrix, EXACTLY ONCE.
//
//  Part A pins the arithmetic directly against CSGObject::IntersectRay /
//  getBoundingBox -- the same construction technique as CSGObjectIdentityTest
//  (direct SphereGeometry + Object + CSGObject, no parser) -- with four
//  regression tripwires: "csg transform ignored", "operand transform applied
//  twice", and "csg transform applied twice" must all MISS where the real,
//  composed-once answer HITS.
//
//  Part B exercises the PARSE path (Job::AddCSGObject, via ParseToCst +
//  DeriveToJob -- CstSourceInstanceTest's own route, in-memory, no temp
//  file) and locks in the new parse-time diagnostic that names this
//  construction for an author staring at an unexpectedly empty render: it
//  must fire when the csg_object AND at least one positioned operand are both
//  transformed, and must NOT fire on the shape scenes/Tests/Geometry/
//  csg.RISEscene uses (positioned csg_object over UNtransformed operands).
//
//  WHY A TALLY, NOT `assert` (see CSGObjectIdentityTest's header for the full
//  case): run_all_tests.ps1 defaults to `-Config Release`, whose CMake flags
//  carry `/DNDEBUG`, silently compiling every `assert` (and any side effect
//  hiding inside one) to nothing.  Every side-effecting call here is on its
//  own line; only its RESULT is checked.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "../src/Library/Agent/AgentDiagnostic.h"
#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Interfaces/ILogPriv.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Job.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Utilities/BoundingBox.h"

using namespace RISE;
using RISE::Agent::AgentSession;
using RISE::Agent::AgentDiagnostic;
namespace AgentDiagnosticCode = RISE::Agent::AgentDiagnosticCode;
using namespace RISE::Implementation;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

//////////////////////////////////////////////////////////////////////
// Part A -- compose-once semantics, direct construction
//////////////////////////////////////////////////////////////////////

// Fire a +z -> -z ray through world x = `x` and report whether CSGObject
// reports a front-face hit.  All the composed-once / regression-candidate
// centers below sit at y=0 z=0, so a ray straight down -z through the
// candidate x is the tangent-free, front-surface probe: it meets a sphere of
// radius 0.2 centered on that x (if any) at world z = +0.2.
static bool HitsAtX( const CSGObject* csg, double x, double* outZ = nullptr )
{
	RayIntersection ri(
		Ray( Point3( x, 0, 2 ), Vector3( 0, 0, -1 ) ),
		nullRasterizerState );
	csg->IntersectRay( ri, RISE_INFINITY, true, true, true );
	if( outZ ) *outZ = ri.geometric.ptIntersection.z;
	return ri.geometric.bHit;
}

void TestComposeOnceSemantics()
{
	std::printf( "=== CsgOperandTransformTest: compose-once semantics (csg transform . operand transform, exactly once) ===\n" );

	// Two operands, positioned in what CSGObject::IntersectRay treats as the
	// csg_object's OWN local frame: A at local x=-0.25, B at local x=+0.25.
	SphereGeometry* pSphereA = new SphereGeometry( 0.2 );
	SphereGeometry* pSphereB = new SphereGeometry( 0.2 );
	Object* pObjectA = new Object( pSphereA );
	Object* pObjectB = new Object( pSphereB );
	safe_release( pSphereA );
	safe_release( pSphereB );

	pObjectA->SetPosition( Point3( -0.25, 0, 0 ) );
	pObjectB->SetPosition( Point3(  0.25, 0, 0 ) );
	pObjectA->FinalizeTransformations();
	pObjectB->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_UNION );
	const bool assigned = csg->AssignObjects( pObjectA, pObjectB );
	Check( assigned, "the composite takes both operands" );

	// The csg_object's OWN transform: world position (1,0,0).
	csg->SetPosition( Point3( 1, 0, 0 ) );
	csg->FinalizeTransformations();

	// Composed-once world centers: csg-transform . operand-transform.
	//   A: 1 + (-0.25) = 0.75         B: 1 + 0.25 = 1.25
	double zHitA = 0.0, zHitB = 0.0;
	const bool hitA = HitsAtX( csg, 0.75, &zHitA );
	const bool hitB = HitsAtX( csg, 1.25, &zHitB );
	Check( hitA, "composed-once center of operand A (1 + (-0.25) = 0.75) HITS" );
	Check( hitB, "composed-once center of operand B (1 + 0.25 = 1.25) HITS" );
	// Both probe rays pass straight through the sphere's own center, so the
	// front surface is met at local z = +radius = +0.2 exactly (no tangent
	// slop to tolerate).
	if( hitA ) Check( std::fabs( zHitA - 0.2 ) < 1.0e-9, "front hit z near +0.2 for A" );
	if( hitB ) Check( std::fabs( zHitB - 0.2 ) < 1.0e-9, "front hit z near +0.2 for B" );

	// Regression tripwires -- each is where a KNOWN wrong composition would
	// have landed the hit; the real, composed-once geometry must MISS there.
	// Side-effecting calls on their own line (this file's header comment);
	// only the RESULT is passed to Check.
	const bool hitIgnoredCsgTransform = HitsAtX( csg, -0.25 );
	const bool hitDoubledOperandTransform = HitsAtX( csg, 0.5 );
	const bool hitDoubledCsgTransform = HitsAtX( csg, 2.25 );
	Check( !hitIgnoredCsgTransform,
	       "REGRESSION 'csg transform ignored': A's bare authored world position (-0.25) MISSES" );
	Check( !hitDoubledOperandTransform,
	       "REGRESSION 'operand transform applied twice': 1 + 2*(-0.25) = 0.5 MISSES" );
	Check( !hitDoubledCsgTransform,
	       "REGRESSION 'csg transform applied twice': 2*1 + 0.25 = 2.25 MISSES" );

	// ri.pObject identity contract (CSGObjectIdentityTest's contract):
	// an intersection reports the COMPOSITE, never an operand.
	{
		RayIntersection ri(
			Ray( Point3( 0.75, 0, 2 ), Vector3( 0, 0, -1 ) ),
			nullRasterizerState );
		csg->IntersectRay( ri, RISE_INFINITY, true, true, true );
		Check( ri.geometric.bHit, "(control) the identity-probe ray hits at all" );
		Check( ri.pObject == csg, "the intersection reports the COMPOSITE as the object hit" );
		Check( ri.pObject != pObjectA, "... not operand A" );
		Check( ri.pObject != pObjectB, "... and not operand B" );
	}

	// getBoundingBox() guards the TLAS side of the same compose-once
	// invariant: union of two radius-0.2 spheres at composed-once centers
	// 0.75 and 1.25 spans x in [0.55, 1.45].
	{
		const BoundingBox bb = csg->getBoundingBox();
		Check( std::fabs( bb.ll.x - 0.55 ) < 1.0e-6, "bbox low x near 0.55 (composed-once span)" );
		Check( std::fabs( bb.ur.x - 1.45 ) < 1.0e-6, "bbox high x near 1.45 (composed-once span)" );
	}

	safe_release( csg );
	safe_release( pObjectA );
	safe_release( pObjectB );
}

//////////////////////////////////////////////////////////////////////
// Part B -- the parse-time diagnostic (Job::AddCSGObject)
//////////////////////////////////////////////////////////////////////

// A minimal ILogPrinter that records every message containing `needle`
// (case-sensitive substring).  Installed once via GlobalLogPriv()->AddPrinter
// and never removed -- matches CstSourceInstanceTest's CapturingLogPrinter;
// see that file's header comment for why a return-code check alone cannot
// distinguish "refused" from "silently no-op'd".
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

protected:
	~CapturingLogPrinter() override {}

private:
	std::string                mNeedle;
	mutable std::mutex         mMutex;
	std::vector<std::string>   mMatches;
};

static const std::string HDR = "RISE ASCII SCENE 7\n";

// Minimal scene: one sphere_geometry, two spheres A/B, one csg_object union.
// `positionOperandA` / `positionOperandB` toggle whether A / B carry their
// own `position` (the shape that re-bases under the csg's own transform);
// the csg_object always carries `position 1 0 0` (a non-identity transform
// of its own).  The per-operand split exists so the warning's TWO grammar
// branches ("operands ... RE-BASES them" vs "operand ... RE-BASES it") are
// each reachable from a dedicated case below.
// `ackTransformedOperands` appends `allow_transformed_operands TRUE` to the
// csg_object chunk -- the acknowledgment idiom that suppresses the advisory.
static std::string MinimalCsgScene( bool positionOperandA, bool positionOperandB, bool ackTransformedOperands = false )
{
	std::string s = HDR
		+ "sphere_geometry\n{\nname geo\nradius 0.2\n}\n"
		+ "standard_object\n{\nname opA\ngeometry geo\n"
		+ ( positionOperandA ? "position -0.25 0 0\n" : "" )
		+ "}\n"
		+ "standard_object\n{\nname opB\ngeometry geo\n"
		+ ( positionOperandB ? "position 0.25 0 0\n" : "" )
		+ "}\n"
		+ "csg_object\n{\nname csg1\nobja opA\nobjb opB\noperation union\nposition 1 0 0\n"
		+ ( ackTransformedOperands ? "allow_transformed_operands TRUE\n" : "" )
		+ "}\n";
	return s;
}

// Derive `scene` into a throwaway Job and discard it -- only the log
// printer's side effect (installed by the caller, for the whole run) is
// being observed.  Returns the derive's `diags` so callers can assert a
// CLEAN derive rather than trusting the return value alone -- a derive
// FAILURE (which discards the partially-built Job) could otherwise fake a
// "no new warning" pass just by never reaching the warning's call site.
static std::vector<std::string> DeriveDiscard( const std::string& scene )
{
	Job* j = new Job();
	Cst::Document d = Cst::ParseToCst( scene );
	std::vector<std::string> diags;
	Cst::DeriveToJob( d, *j, &diags );
	j->release();
	return diags;
}

void TestParseTimeDiagnostic( CapturingLogPrinter* pRebaseLog,
                              CapturingLogPrinter* pRebaseItLog,
                              CapturingLogPrinter* pRebaseThemLog )
{
	std::printf( "=== CsgOperandTransformTest: the parse-time operand-rebase WARNING fires exactly when both halves are transformed, and `allow_transformed_operands TRUE` silences it ===\n" );

	// (1) BOTH operands positioned + positioned csg_object -> the trap:
	// WARNS, through the plural grammar branch ("operands ... RE-BASES them").
	{
		const int before = pRebaseLog->MatchCount();
		const int beforeThem = pRebaseThemLog->MatchCount();
		const std::vector<std::string> diags = DeriveDiscard( MinimalCsgScene( /*positionOperandA=*/true, /*positionOperandB=*/true ) );
		Check( diags.empty(), "positioned operands under a positioned csg_object: derives with no diagnostics" );
		Check( pRebaseLog->MatchCount() >= before + 1,
		       "positioned operands under a positioned csg_object: the rebase warning fires" );
		Check( pRebaseThemLog->MatchCount() >= beforeThem + 1,
		       "both-operands case took the plural grammar branch (`RE-BASES them`)" );
	}

	// (1b) exactly ONE operand positioned -> still the trap, through the
	// SINGULAR grammar branch ("operand `opA`, which is already transformed
	// ... RE-BASES it") -- the branch case (1) cannot reach.
	{
		const int before = pRebaseLog->MatchCount();
		const int beforeIt = pRebaseItLog->MatchCount();
		const std::vector<std::string> diags = DeriveDiscard( MinimalCsgScene( /*positionOperandA=*/true, /*positionOperandB=*/false ) );
		Check( diags.empty(), "single positioned operand under a positioned csg_object: derives with no diagnostics" );
		Check( pRebaseLog->MatchCount() >= before + 1,
		       "single positioned operand under a positioned csg_object: the rebase warning fires" );
		Check( pRebaseItLog->MatchCount() >= beforeIt + 1,
		       "single-operand case took the singular grammar branch (`RE-BASES it`)" );
	}

	// (2) UNpositioned operands + positioned csg_object -- the shape
	// scenes/Tests/Geometry/csg.RISEscene uses (a positioned csg_object over
	// untransformed operands is a normal, common, WARNING-FREE construction):
	// no new warning.  Checking `diags.empty()` first rules out the
	// alternative explanation that the derive simply FAILED before reaching
	// the warning's call site -- a derive failure can't fake a no-warning
	// pass.
	{
		const int before = pRebaseLog->MatchCount();
		const std::vector<std::string> diags = DeriveDiscard( MinimalCsgScene( /*positionOperandA=*/false, /*positionOperandB=*/false ) );
		Check( diags.empty(), "the unpositioned-operand scene derives with no diagnostics" );
		Check( pRebaseLog->MatchCount() == before,
		       "unpositioned operands under a positioned csg_object (the csg.RISEscene shape): NO new warning" );
	}

	// (3) positioned operands + positioned csg_object + `allow_transformed_operands TRUE`
	// -- the acknowledgment idiom (mirrors `allow_non_sampling_emitter`):
	// derives clean AND the advisory is silenced.
	{
		const int before = pRebaseLog->MatchCount();
		const std::vector<std::string> diags = DeriveDiscard( MinimalCsgScene( /*positionOperandA=*/true, /*positionOperandB=*/true, /*ackTransformedOperands=*/true ) );
		Check( diags.empty(), "positioned operands + `allow_transformed_operands TRUE`: derives with no diagnostics" );
		Check( pRebaseLog->MatchCount() == before,
		       "positioned operands + `allow_transformed_operands TRUE`: the rebase warning is SILENCED" );
	}
}

//////////////////////////////////////////////////////////////////////
// Part C -- the agent-facing diagnostic (AgentSession::ValidateText,
// AgentDiagnosticCode::CSG_OPERAND_REBASE)
//////////////////////////////////////////////////////////////////////

// Reuses MinimalCsgScene (Part B) -- the SAME fixture builder, so the
// agent-facing Warning below and the runtime GlobalLog advisory Part B
// proved are known to fire on byte-identical scene text.  Unlike Part B
// (which captures GlobalLog output via a printer), this drives
// AgentSession::ValidateText's stateless post-derive audit directly.
// CSG_OPERAND_REBASE is Warning-tier ONLY -- no paired creation gate (see
// AgentDiagnosticCode::CSG_OPERAND_REBASE, AgentDiagnostic.h, for why) --
// so ValidateText's (b2) audit is this diagnostic's entire agent-facing
// surface; there is no InsertChunk/ProposePatch refusal to test alongside
// it, unlike LUMINAIRE_NULL_GEOMETRY's E1 gate.
void TestAgentValidateTextSurfacesRebaseWarning()
{
	std::printf( "=== CsgOperandTransformTest: AgentSession::ValidateText fires CSG_OPERAND_REBASE "
	             "(Warning); `allow_transformed_operands TRUE` silences it; unpositioned operands never trip it ===\n" );

	// (a) unacknowledged positioned-operands + positioned csg_object -- the
	// trap -- ValidateText returns a Warning-severity CSG_OPERAND_REBASE
	// naming csg1 and stating the acknowledgment escape.
	{
		const std::vector<AgentDiagnostic> diags =
			AgentSession::ValidateText( MinimalCsgScene( /*positionOperandA=*/true, /*positionOperandB=*/true ) );
		const AgentDiagnostic* found = nullptr;
		for( const AgentDiagnostic& d : diags )
			if( d.code == AgentDiagnosticCode::CSG_OPERAND_REBASE ) { found = &d; break; }
		Check( found != nullptr, "(a) CSG_OPERAND_REBASE fires on the unacknowledged positioned-operands scene" );
		if( found ) {
			Check( found->severity == AgentDiagnostic::Severity::Warning,
			       "(a) ...at Severity::Warning (a valid construction, not a refusal)" );
			Check( found->message.find( "csg1" ) != std::string::npos,
			       "(a) message names the offending csg_object" );
			Check( found->message.find( "allow_transformed_operands" ) != std::string::npos,
			       "(a) message states the acknowledgment escape" );
		}
	}

	// (b) the SAME scene, WITH `allow_transformed_operands TRUE` -- no
	// CSG_OPERAND_REBASE at all: an acknowledged, disclosed rebase must not
	// nag on every subsequent Validate call (the same anti-pattern
	// LUMINAIRE_NULL_GEOMETRY's ack flag guards against).
	{
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText(
			MinimalCsgScene( /*positionOperandA=*/true, /*positionOperandB=*/true, /*ackTransformedOperands=*/true ) );
		bool sawCode = false;
		for( const AgentDiagnostic& d : diags )
			if( d.code == AgentDiagnosticCode::CSG_OPERAND_REBASE ) sawCode = true;
		Check( !sawCode, "(b) `allow_transformed_operands TRUE`: ValidateText is SILENT on CSG_OPERAND_REBASE" );
	}

	// (c) UNpositioned operands under a positioned csg_object -- the common,
	// unremarkable construction (the scenes/Tests/Geometry/csg.RISEscene
	// shape) -- no CSG_OPERAND_REBASE at all.  The false-positive guard: the
	// diagnostic must not fire merely because the csg_object itself is
	// transformed.
	{
		const std::vector<AgentDiagnostic> diags =
			AgentSession::ValidateText( MinimalCsgScene( /*positionOperandA=*/false, /*positionOperandB=*/false ) );
		bool sawCode = false;
		for( const AgentDiagnostic& d : diags )
			if( d.code == AgentDiagnosticCode::CSG_OPERAND_REBASE ) sawCode = true;
		Check( !sawCode,
		       "(c) unpositioned operands under a positioned csg_object: no CSG_OPERAND_REBASE (common, valid case)" );
	}

	// (d) exactly ONE operand transformed -- pins the "at least one operand"
	// semantics on THIS surface too (Part B's case (1b) pins it for the
	// runtime advisory): if CollectRebasedOperandCsgs_ ever regressed to
	// requiring BOTH operands transformed, this case fails while (a)/(b)/(c)
	// would all keep passing (they transform both or neither).
	{
		const std::vector<AgentDiagnostic> diags =
			AgentSession::ValidateText( MinimalCsgScene( /*positionOperandA=*/true, /*positionOperandB=*/false ) );
		const AgentDiagnostic* found = nullptr;
		for( const AgentDiagnostic& d : diags )
			if( d.code == AgentDiagnosticCode::CSG_OPERAND_REBASE ) { found = &d; break; }
		Check( found != nullptr, "(d) CSG_OPERAND_REBASE fires with exactly ONE transformed operand" );
		if( found ) {
			Check( found->message.find( "csg1" ) != std::string::npos,
			       "(d) ...naming the csg_object" );
		}
	}

	// (e) TWO unacknowledged rebased csg_objects in one scene -- the audit's
	// PLURAL message branch (quoted names joined with \"and\", \"carry\"/
	// \"their\", and a fix tail that scales past one chunk) is otherwise
	// never exercised by any case in either test file.  Operand sharing
	// across composites is a supported construction (csg.RISEscene's csgA /
	// csgB share boxB / sphereB), so csg2 simply reuses opA / opB.
	{
		std::string scene = MinimalCsgScene( /*positionOperandA=*/true, /*positionOperandB=*/true );
		scene += "csg_object\n{\nname csg2\nobja opA\nobjb opB\noperation union\nposition 2 0 0\n}\n";
		const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( scene );
		const AgentDiagnostic* found = nullptr;
		for( const AgentDiagnostic& d : diags )
			if( d.code == AgentDiagnosticCode::CSG_OPERAND_REBASE ) { found = &d; break; }
		Check( found != nullptr, "(e) two unacknowledged rebased csg_objects: the diagnostic fires" );
		if( found ) {
			Check( found->message.find( "'csg1' and 'csg2'" ) != std::string::npos,
			       "(e) both offenders are named, joined with 'and'" );
			Check( found->message.find( " carry " ) != std::string::npos,
			       "(e) plural verb agreement (`carry`, not `carries`)" );
			Check( found->message.find( "each named csg_object" ) != std::string::npos,
			       "(e) the fix-instruction tail scales past one chunk" );
		}
	}
}

int main()
{
	std::printf( "CsgOperandTransformTest -- csg_object operand-rebase: compose-once semantics + parse-time diagnostic\n" );

	// Distinctive substring of the new Job::AddCSGObject warning (see its
	// full text at src/Library/Job.cpp, Job::AddCSGObject).  "RE-BASES" --
	// not "RE-BASES it" -- because the warning's grammar branches on operand
	// count ("RE-BASES it" for one transformed operand, "RE-BASES them" for
	// both); the shared stem is the stable needle.
	CapturingLogPrinter* pRebaseLogOwned = new CapturingLogPrinter( "RE-BASES" );
	RISE::GlobalLogPriv()->AddPrinter( pRebaseLogOwned );
	CapturingLogPrinter* pRebaseLog = pRebaseLogOwned;   // AddPrinter addref'd; keep a raw read handle

	// Branch-specific needles, so the two grammar branches are each pinned by
	// the case that reaches them: "RE-BASES it" is the single-transformed-
	// operand wording, "RE-BASES them" the both-operands wording.
	CapturingLogPrinter* pRebaseItLogOwned = new CapturingLogPrinter( "RE-BASES it" );
	RISE::GlobalLogPriv()->AddPrinter( pRebaseItLogOwned );
	CapturingLogPrinter* pRebaseItLog = pRebaseItLogOwned;

	CapturingLogPrinter* pRebaseThemLogOwned = new CapturingLogPrinter( "RE-BASES them" );
	RISE::GlobalLogPriv()->AddPrinter( pRebaseThemLogOwned );
	CapturingLogPrinter* pRebaseThemLog = pRebaseThemLogOwned;

	TestComposeOnceSemantics();
	TestParseTimeDiagnostic( pRebaseLog, pRebaseItLog, pRebaseThemLog );
	TestAgentValidateTextSurfacesRebaseWarning();

	safe_release( pRebaseLogOwned );
	safe_release( pRebaseItLogOwned );
	safe_release( pRebaseThemLogOwned );

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
