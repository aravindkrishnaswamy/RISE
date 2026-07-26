//////////////////////////////////////////////////////////////////////
//
//  AgentSession.h - the headless read/validate surface over a Job
//    (Facet 5, the agentic surface -- slice 0a).
//
//    The FIRST code of the agentic surface: a single-threaded, HEADLESS
//    session object wrapping a Job whose scene was loaded via the
//    canonical CST path (Model-B: Scene = derive(CST)).  It exposes the
//    three READ/VALIDATE verbs of the design
//    (docs/agentic-redesign/50-agentic-surface.md §2.2.1 read tools,
//    §2.2.4 `validate`) as plain C++ methods -- no JSON, no networking
//    (the JSON-RPC transport is slice 0c):
//
//      * ReadDocument() -> the canonical `.RISEscene` text of the head
//                          (SerializeCst of the retained CST Document).
//      * ReadSchema(kw) -> the descriptor-generated JSON schema (L6).
//      * Validate(text) -> a structured, side-effect-free check of a
//                          CANDIDATE scene text (THE keystone).
//
//    This slice deliberately ships NO mutating verb (propose_patch),
//    NO render, and NO transport -- those are 0b / 0c.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_AGENT_AGENTSESSION_
#define RISE_AGENT_AGENTSESSION_

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "AgentDiagnostic.h"

#include "../Cst/Cst.h"   // Facet 5 slice 1a: RISE::Cst::CstHeadVersion (the (uuid,revision) optimistic-concurrency identity)
#include "../Rendering/InteractivePelRasterizer.h"   // GUI render modes P1: RISE::Implementation::ViewportRenderMode (AgentRenderTarget::ViewMode's payload -- needs the complete enum for AgentRenderParams::viewMode's default member initializer)

namespace RISE
{
	class IJobPriv;
	class SceneEditController;   // Facet 5 slice 1b: LIVE mode routes ProposePatch through the controller's render-safe edit path (fwd-decl only -- no header dep)

	namespace Agent { class InMemoryRasterizerOutput; }   // preview-render: cached sink for read_image's maxEdge downscale (fwd-decl only -- no header dep)

	namespace Agent
	{
		//! compare_to_reference (the reconstruction feedback instrument --
		//! docs/agentic-redesign vision baseline: models reconstructing a
		//! scene from a reference photo used to score RMSE 0.24-0.40 with
		//! NO feedback signal at all -- they render, then guess): one
		//! HOST-registered reference image the compare_to_reference verb
		//! may grade a render against.  `name` is the caller-chosen handle
		//! a compare_to_reference call passes back (e.g. the eval harness's
		//! "view1".."viewN" prompt-attachment naming contract -- see
		//! AgentEvalRunner.cpp's RunScenarioDriven, which registers every
		//! prompt's image attachments in prompt-then-attachment order);
		//! `pngBytes` is the RAW PNG FILE BYTES (NOT base64) -- decoded at
		//! compare time through the SAME in-tree PNGReader path
		//! AgentEvalRunner.cpp's DecodePngToRgb8 uses (see AgentSession.cpp's
		//! own copy of that decode helper for the exact byte-round-trip
		//! contract; duplicated rather than exported because the eval
		//! runner's copy is a file-local helper in an anonymous namespace).
		//!
		//! READ-SIDE HYGIENE: compare_to_reference can ONLY see images the
		//! host explicitly registered via AgentSession::SetReferenceImages
		//! -- there is no wire parameter that names a filesystem path or
		//! arbitrary bytes.  This mirrors read_skill's bare-name rule (a
		//! caller picks a NAME off a host-curated index; it never supplies
		//! a path the host then opens) -- a remote/agent caller cannot use
		//! this verb to read an arbitrary file off the host's disk.
		struct AgentReferenceImage
		{
			std::string name;
			std::string pngBytes;
		};

		//! Secure-MCP slice 5a: a construction-time, WIRE-IMMUTABLE flag
		//! naming who this session speaks for -- the local human operating
		//! the GUI (Owner) or a remote/external agent proposing edits for a
		//! human to review (External).  Distinct from AgentAutonomy
		//! (AgentRpc.h), which is a TRANSPORT-layer verb-dispatch gate (all-
		//! or-nothing: which JSON-RPC methods this dispatcher will even
		//! attempt); AgentAuthority is a SESSION-layer identity consulted by
		//! ProposePatch/InsertChunk/RemoveChunk themselves to decide whether
		//! a mutating call COMMITS directly or only STAGES a proposal for an
		//! Owner to resolve.  The wire-level plumbing that lets a remote
		//! transport construct an External session and expose
		//! list_proposals/resolve_proposal verbs is slice 5b -- this slice is
		//! the headless C++ core only.
		enum class AgentAuthority
		{
			Owner,      //!< the local human / the GUI itself: mutating verbs COMMIT directly (today's behaviour, byte-for-byte). The default -- every EXISTING construction site keeps committing without a code change.
			External    //!< a remote/other agent: mutating verbs STAGE a proposal (inert) instead of committing; may NOT resolve (approve/reject) ANY proposal, including its own.
		};

		//! Secure-MCP slice 5a: the structured result of a STAGED
		//! propose_patch / insert_chunk / remove_chunk call (External
		//! authority + Propose autonomy, with a live controller attached).
		//! Deliberately narrow -- a stage is INERT, so there is no
		//! rawCode/status tri-state to fold; `staged` is true iff the
		//! controller accepted the stage (always true when this path is
		//! actually taken -- StageProposal cannot itself fail), `proposalId`
		//! is the id an Owner session later passes to ResolveProposal, and
		//! `message` is a human-readable confirmation.
		struct AgentProposeResult
		{
			bool          staged = false;
			std::uint64_t proposalId = 0;
			std::string   message;
		};
		//! A STRUCTURED set-param patch (slice 0b: set only -- no text-patch,
		//! no add/remove chunk; those are later slices).  Locates a named
		//! entity in the retained CST Document and sets one of its params to a
		//! new value string, routed through Job::ApplyCstParamEditChecked --
		//! the SAME edit pathway the GUI property panel uses (L2: the agent is
		//! just another client of the edit surface) plus the round-2 P1-A
		//! full-derivability gate (an agent retarget must not commit a head
		//! that no longer derives in document order).
		struct AgentSetPatch
		{
			std::string target;   //!< the entity NAME to edit (a chunk `name`; unnamed cameras resolve by kind)
			std::string kind;     //!< the entity KIND keyword (e.g. "material", "sphere_geometry", "camera") -- disambiguates a cross-category name clash; "" = any
			std::string param;    //!< the parameter role to set (e.g. "radius", "reflectance", "location")
			std::string value;    //!< the new value string (parsed by the derive layer per the param's declared kind)

			//! Facet 5 slice 1a: OPTIONAL optimistic-concurrency precondition.  When `hasBaseVersion` is
			//! true, ProposePatch REJECTS the patch with a CONFLICT (WITHOUT mutating) unless `baseVersion`
			//! equals the Job's CURRENT retained head-version -- so a patch built against a stale head (the
			//! head moved since the agent last read it) is rejected rather than silently clobbering the newer
			//! head (docs/agentic-redesign/50-agentic-surface.md §2.2.2 baseHeadVersion precondition).  When
			//! false (the default), the edit is UNCONDITIONAL (slice-0 back-compat: no gating).
			bool                     hasBaseVersion = false;
			RISE::Cst::CstHeadVersion baseVersion;   //!< the head-version the patch was built against (checked iff hasBaseVersion)
		};

		//! The structured result of ProposePatch.  `applied` means CLEAN
		//! SUCCESS ONLY, and `status` is the tri-state gate a client keys on;
		//! together they fold ApplyCstParamEdit's 0/1/2/3 return HONESTLY:
		//!   * 1/2 -> applied=true,  status="applied": the Document was mutated
		//!            + the live Job re-derived CLEANLY (1 incremental /
		//!            2 full re-derive).  The ONLY clean-success codes.
		//!   * 3   -> applied=FALSE, status="diagnosed": the Document WAS
		//!            mutated and the live managers WERE replaced, BUT the full
		//!            re-derive EMITTED DIAGNOSTICS.  The source contract
		//!            (Job.cpp DeriveEditedCstDocument_) treats 3 as FAILURE
		//!            ("the edit FAILED -- treat as failure"), so slice 0 does
		//!            too: `applied` is false and the caller MUST NOT proceed as
		//!            if the patch cleanly succeeded.  This is DISTINCT from a
		//!            reject: the head is NOT byte-identical -- the Document was
		//!            mutated and the managers replaced -- so `message` states
		//!            that nuance plainly (a caller must not assume nothing
		//!            changed either).
		//!   * 0   -> applied=false, status="rejected": edit refused; the head
		//!            is byte-identical (nothing changed).  The pre-flight
		//!            guards (no Document / empty target/param/value, and --
		//!            LIVE mode only -- the controller's open-editor-
		//!            transaction refusal) map here too -- they are refusals
		//!            with an unchanged head.
		//! Facet 5 slice 1a adds a FOURTH status, the optimistic-concurrency
		//! CONFLICT: when the patch carries `hasBaseVersion` and its `baseVersion`
		//! does NOT equal the Job's CURRENT head-version, ProposePatch returns
		//!   * CONFLICT -> applied=FALSE, status="conflict", rawCode=0: the base
		//!            precondition failed, so the patch was REJECTED WITHOUT
		//!            touching the Document (the head is byte-identical -- a stale
		//!            patch must never mutate).  Distinct from "rejected": the
		//!            entity/param may be perfectly valid; the patch is just stale.
		//!            The caller should re-read the head and re-propose against the
		//!            new `headVersion` this result carries.
		//! `rawCode` preserves the underlying contract value (0/1/2/3) for a
		//! caller that wants the raw code (0 for a conflict, since nothing was
		//! applied); `status` is the recommended gate:
		//!   applied  <=> a CLEAN apply (rawCode 1 or 2);
		//!   status   in {"applied","rejected","diagnosed","conflict"}.
		//! `headVersion` is the Job's head-version AFTER the call: the POST-COMMIT
		//! head on a clean apply (its revision bumped), and the CURRENT (unchanged)
		//! head on a reject / diagnosed / conflict.  A caller keying on optimistic
		//! concurrency reads it to learn where the head now is.
		//! `message` is a human explanation (rebind codes 2/3 note the full
		//! re-derive; the code-3 message spells out mutated-but-diagnosed; the
		//! conflict message reports the revision the head moved to).
		//! `retriable` disambiguates the "rejected" bucket for a machine
		//! client: true means the refusal is TRANSIENT -- resubmitting the
		//! IDENTICAL patch later can succeed.  Today the only transient
		//! reject is the LIVE-mode controller's open-editor-transaction
		//! refusal (retry after the gesture completes); permanent rejects
		//! (no Document / empty field / unknown entity / bad value) stay
		//! false -- retrying them verbatim can never succeed.  A version
		//! CONFLICT does NOT set it: conflicts carry their own
		//! status="conflict" and are retriable-by-protocol via re-read
		//! (re-read the head, rebase, re-propose), not by verbatim
		//! resubmission.  Carried 1:1 from the controller's
		//! AgentCommitResult in LIVE mode; the direct (headless) path has
		//! no transaction surface, so its rejects are all permanent.
		//! Forward-declared here (full definition + the seven-reason `reason`
		//! contract below, after AgentChunkResult) so AgentPatchResult::issues
		//! can be declared below without reordering this file's established
		//! feature-chronology layout (AgentPatchResult predates the actionable-
		//! diagnostics system by several slices).
		struct AgentChunkIssue;
		struct AgentPatchResult
		{
			bool        applied = false;
			bool        retriable = false;   //!< always emitted on the wire; meaningful for status="rejected" only: true = transient refusal (open editor transaction) -- retry the SAME patch later; false = permanent
			int         rawCode = 0;         //!< 0 reject/conflict / 1 incremental / 2 D2 full re-derive / 3 replaced-but-diagnosed
			std::string status;              //!< "applied" (clean) / "rejected" (head intact) / "diagnosed" (mutated but re-derive diagnosed) / "conflict" (stale baseVersion, head intact)
			RISE::Cst::CstHeadVersion headVersion;   //!< the head-version AFTER the call (post-commit on success; current head on reject/diagnosed/conflict)
			std::string message;
			//! Secure-MCP slice 6: set true ONLY for an External-authority
			//! stage attempt that SceneEditController::StageProposal refused
			//! because the attached controller's PENDING proposal queue is
			//! already at SceneEditController::kMaxPendingProposals (see
			//! that constant's doc). `status` is "rejected" and `applied` is
			//! false in this case too, exactly like any other permanent
			//! reject -- but the wire layer (AgentRpc.cpp) checks THIS flag
			//! specifically to surface a distinct, structured top-level
			//! JSON-RPC error (kProposalQueueFull) instead of the normal
			//! success-envelope result shape, so a programmatic caller can
			//! branch on "the queue is full, resolve some proposals first"
			//! without string-matching `message`. Always false on every
			//! other path (LIVE-mode commit, headless direct-Job, a
			//! successful stage).
			bool        queueFull = false;
			//! Actionable-rejection extension (the propose_patch sibling of
			//! AgentChunkResult::issues -- see AgentChunkIssue's doc for the
			//! full seven-reason set and AgentSession.cpp's
			//! AnalyzeRejectedParamEdit for how this is derived): populated
			//! ONLY for a REJECTED param edit (`applied` false, `status`
			//! "rejected", the generic "would not derive" cause -- never for
			//! a conflict, a queue-full stage refusal, or a transient
			//! open-editor-transaction refusal, all of which already carry
			//! their own precise message and must not be second-guessed).
			//! `applied`/`status` are UNCHANGED by this field -- it is EXTRA
			//! detail on a verdict already reached, never a different one.
			//! Empty on a clean apply and on a rejection the descriptor-based
			//! analyser could not statically explain (HONESTY: empty is NOT
			//! exoneration -- see AnalyzeRejectedParamEdit's doc).  Serialized
			//! by AgentRpc.cpp as `issues`, OMITTED entirely when empty (same
			//! back-compat posture as AgentChunkResult::issues).
			std::vector<AgentChunkIssue> issues;
		};

		//! Model-B F5 slice S3 (actionable insert_chunk diagnostics), extended by
		//! a later slice to propose_patch and remove_chunk: ONE shape for EVERY
		//! per-parameter / per-chunk diagnostic signal the three mutating verbs
		//! can return -- a NON-BLOCKING WARNING attached to a SUCCESSFUL
		//! insert_chunk (the chunk landed but names something not yet defined --
		//! a forward reference is legitimate incremental authoring, so
		//! `applied`/`status` are UNCHANGED by it) and the descriptor- or
		//! reference-graph-derivable CAUSE of a REJECTED insert_chunk /
		//! propose_patch / remove_chunk (see AgentSession.cpp's
		//! AnalyzeRejectedInsert, AnalyzeRejectedParamEdit, AnalyzeRejectedRemove).
		//! Shipping ONE shape instead of several overlapping ones means a caller
		//! learns ONE contract, not three nearly-identical ones -- and the
		//! REJECTED case is the more urgent one: the engine's own dry-run
		//! diagnostic for a rejected edit is a coarse, hedged, log-only message
		//! ("<kw>: apply failed (e.g. unresolved reference); see log" for a
		//! chunk, "edit rejected (entity/param not found or the edit would not
		//! derive)" for a patch, "...it is likely still REFERENCED by another
		//! chunk, or the remaining document no longer derives in order..." for a
		//! remove) that names no param, no value, no referrer, and -- for
		//! insert/patch -- points at a log an agent cannot read; this struct is
		//! what turns that into something actionable.
		//!   * `param` -- the offending parameter name; "" for a chunk-level
		//!     issue (unknown_chunk_type, unknown_target, still_referenced --
		//!     none of these name a PARAMETER).
		//!   * `value` -- the offending value token: the dangling reference name
		//!     (unresolved_reference), the numeric literal found in a reference
		//!     slot (numeric_in_reference_slot), the undeclared param's own given
		//!     value (unknown_param, informational), the unregistered keyword
		//!     itself (unknown_chunk_type), the ill-typed value verbatim
		//!     (invalid_value), the target name that did not resolve
		//!     (unknown_target), or the remove target's own name
		//!     (still_referenced -- the BLOCKING referrers ride in `suggestions`
		//!     instead, since `value` is already spoken for by the target).
		//!   * `reason` -- a short STABLE slug from a CLOSED set a machine
		//!     caller can switch on without string-matching `message`, grouped
		//!     by which verb(s) can produce it:
		//!
		//!     insert_chunk, propose_patch, AND remove_chunk (target/param
		//!     resolution and reference-typing are shared concerns):
		//!       "unresolved_reference"      a Reference-kind param's value is
		//!                                   a NAME that does not resolve
		//!                                   against the document in scope
		//!                                   (the current head on a rejected
		//!                                   insert/patch; the just-landed head
		//!                                   on a successful insert).
		//!       "unknown_param"             the param name is not declared on
		//!                                   the chunk's ChunkDescriptor at all
		//!                                   (a typo, e.g. `constant` for the
		//!                                   real param `value`).
		//!       "numeric_in_reference_slot" a Reference-kind param's value is
		//!                                   ENTIRELY numeric tokens -- a TYPE
		//!                                   MISMATCH (a literal where a chunk
		//!                                   NAME belongs), not a dangling
		//!                                   reference.
		//!
		//!     insert_chunk ONLY:
		//!       "unknown_chunk_type"        the chunk keyword itself is not a
		//!                                   registered chunk type.
		//!
		//!     propose_patch ONLY:
		//!       "unknown_target"            the `target` entity name does not
		//!                                   resolve to any chunk in the head
		//!                                   (restricted to `kind` when given).
		//!       "invalid_value"             `param` is declared, but `value` is
		//!                                   ill-typed for its ValueKind -- a
		//!                                   non-finite/non-numeric token in a
		//!                                   Double/UInt/vector slot, or a string
		//!                                   outside an Enum's declared set (the
		//!                                   message then lists the allowed
		//!                                   values).
		//!
		//!     remove_chunk ONLY:
		//!       "still_referenced"          the remove target resolves, AND the
		//!                                   reference graph's reverse adjacency
		//!                                   names at least one chunk that still
		//!                                   references it (those referrer names
		//!                                   ride in `suggestions` -- edit or
		//!                                   remove them first).  A remove reject
		//!                                   whose target has NO dependents (the
		//!                                   OTHER derive-order cause) emits NO
		//!                                   issue -- see the HONESTY note below.
		//!
		//!   * `suggestions` -- near-miss candidates, best match first (up to 3
		//!     via the shared substring-then-edit-distance ranking; the
		//!     `unknown_param` case falls back to the chunk's FULL declared
		//!     parameter list when nothing lexically close exists -- a typo
		//!     that reads as a wholly different word, e.g. `constant` for
		//!     `value`, has no near-miss to rank, but the author still needs a
		//!     concrete candidate set). ALWAYS empty for
		//!     numeric_in_reference_slot -- a literal has no name to suggest a
		//!     near-miss of. For "still_referenced" this is NOT a near-miss
		//!     ranking -- it is the EXACT list of blocking referrer chunk names
		//!     (every one of them, not a top-3 heuristic guess).
		//!
		//!   HONESTY (every analyser above shares this rule): an issue-producing
		//!   pass is a STATIC, descriptor-/reference-graph-only check -- it does
		//!   not see every semantic constraint the full derive validates (e.g. a
		//!   cross-param requirement descriptors don't encode), and remove's
		//!   pass does not see DYNAMIC references (e.g. a timeline `element`
		//!   naming an entity outside any declared Reference param). Finding
		//!   NOTHING is therefore never treated as exoneration -- an empty
		//!   `issues` list on a rejection leaves `message` as the engine's own
		//!   (already-hedged) diagnostic, never a clause implying the target/
		//!   value checked out.
		struct AgentChunkIssue
		{
			std::string              param;
			std::string              value;
			std::string              reason;
			std::vector<std::string> suggestions;
		};

		//! Model-B F5 slice S2: the structured result of InsertChunk /
		//! RemoveChunk.  SAME shape and gating semantics as AgentPatchResult
		//! (applied = CLEAN success only; status in {"applied","rejected",
		//! "diagnosed","conflict"}; retriable marks the ONE transient reject
		//! -- the LIVE-mode open-editor-transaction refusal; headVersion is
		//! the head AFTER the call) PLUS the affected chunk's identity echo:
		//! `kind` = the chunk KEYWORD, `name` = its `name` param (the remove
		//! target's bare name on a remove; "" for an unnamed chunk).  Filled
		//! as soon as the chunk parses / the target resolves, so even a
		//! refusal identifies what was attempted.
		//! rawCode stays in the wire contract {0,1,2,3}: a chunk CRUD that
		//! lands is ALWAYS a D2 full re-derive, so a success is rawCode 2
		//! (never 1); Job's internal negative pre-derive refusal codes are
		//! normalized to 0 with a specific message.
		struct AgentChunkResult
		{
			bool        applied = false;
			bool        retriable = false;
			int         rawCode = 0;
			std::string status;
			RISE::Cst::CstHeadVersion headVersion;
			std::string message;
			std::string name;   //!< the chunk's `name` param / the remove target
			std::string kind;   //!< the chunk keyword (e.g. "omni_light")
			//! Secure-MCP slice 6: SAME meaning as AgentPatchResult::queueFull
			//! (see its doc) -- set true only for an External-authority
			//! insert_chunk/remove_chunk stage refused by StageProposal's
			//! kMaxPendingProposals gate.
			bool        queueFull = false;
			//! Populated on any of THREE paths -- see AgentChunkIssue's doc for
			//! the full contract:
			//!   * a SUCCESSFUL insert_chunk whose newly-landed chunk itself
			//!     references a name the document has no definition for (a
			//!     WARNING; `applied`/`status` UNCHANGED -- see
			//!     AgentSession::InsertChunk's AttachChunkIssueWarnings call);
			//!   * a REJECTED insert_chunk the descriptor-based pre-flight
			//!     analyser could explain (see AgentSession.cpp's
			//!     AnalyzeRejectedInsert) -- `applied` stays false / `status`
			//!     stays "rejected"; this is EXTRA detail, not a different
			//!     verdict;
			//!   * a REJECTED remove_chunk whose target the reference graph
			//!     shows is still referenced (see AgentSession.cpp's
			//!     AnalyzeRejectedRemove) -- reason "still_referenced", the
			//!     blocking referrer(s) named in `suggestions`.
			//! Empty on every other path (a clean remove, a rejection either
			//! analyser could not explain -- see AgentChunkIssue's HONESTY note
			//! -- a clean insert with no dangling reference). AgentRpc.cpp
			//! serializes this as `issues` and OMITS the key entirely when
			//! empty (back-compat with every existing insert_chunk/remove_chunk
			//! caller).
			std::vector<AgentChunkIssue> issues;
		};

		//! Preview-render (F5 the cheap multi-angle observe loop): an OPTIONAL
		//! EPHEMERAL override of the active camera's pose for one Render call
		//! only.  Each `has*` flag gates its field independently, so a caller
		//! can override just `location`+`lookat` and leave `up`/`fov` at the
		//! camera's current values.  Values are the SAME string forms
		//! CameraIntrospection::SetProperty accepts ("x y z" for the Vec3
		//! fields, a plain number in DEGREES for fov).  Render captures the
		//! camera's CURRENT value of every overridden field via
		//! CameraIntrospection::GetPropertyValue BEFORE applying any override,
		//! and restores every captured field after the render -- the active
		//! camera's properties are IDENTICAL before and after a Render call,
		//! whether or not an override was requested (see AgentSession.cpp for
		//! the capture-set-render-restore sequencing and the LIVE-mode safety
		//! note).
		struct AgentCameraOverride
		{
			bool        hasLocation = false;
			std::string location;    //!< "x y z"
			bool        hasLookAt = false;
			std::string lookAt;      //!< "x y z"
			bool        hasUp = false;
			std::string up;          //!< "x y z"
			bool        hasOrientation = false;
			std::string orientation; //!< "heading pitch bank" in degrees
			bool        hasTargetOrientation = false;
			std::string targetOrientation; //!< "heading pitch 0" in degrees
			bool        hasFov = false;
			std::string fov;         //!< degrees, plain number
		};

		//! Toolkit slice 2: `render`'s optional quality selector.  `Production`
		//! (the default) is today's EXACT behaviour -- the head's active
		//! (production) rasterizer, byte-for-byte unchanged.  `Draft`
		//! renders through a wholly SEPARATE, EPHEMERAL preview pipeline
		//! (CreateInteractiveMaterialPreviewPipeline -- the SAME studio-
		//! preview shading the GUI's live interactive editor uses) that
		//! NEVER references the production rasterizer, its FrameStore, or
		//! its outputs -- see AgentRenderParams::quality's doc and
		//! AgentRenderResult::renderMode's doc for the full honesty
		//! contract (a draft render is geometry/composition/camera-
		//! accurate but IGNORES the scene's authored materials and
		//! lighting; never judge those from a draft).
		enum class AgentRenderQuality
		{
			Production,   //!< today's exact behaviour -- the head's active rasterizer (default)
			Draft         //!< a cheap, ephemeral studio-preview render -- see the class doc above
		};

		//! Toolkit slice 3a: `render`'s optional SEGMENTATION selector,
		//! orthogonal to `quality`.  `Beauty` (the default) is today's EXACT
		//! behaviour -- radiance (production) or studio-preview (draft)
		//! shading.  `ObjectMap` renders through a wholly SEPARATE,
		//! EPHEMERAL identity pipeline (CreateInteractiveObjectMapPipeline)
		//! that paints each hit object a FLAT, high-contrast identity colour
		//! (no lighting, no materials) and returns a per-object colour
		//! `legend` (see AgentRenderResult::legend).  It answers "which
		//! object is at which pixel" for spatial reasoning; it says NOTHING
		//! about appearance.  An objectmap render has exactly ONE fidelity:
		//! `quality` is IGNORED under ObjectMap (and any `samples` override
		//! is ignored too -- the exact per-pixel identity requires the
		//! single-ray path; see AgentRenderResult::renderMode's doc for the
		//! honesty contract, and read the returned PNG at NATIVE size --
		//! read_image's maxEdge box-downscale BLENDS identity colours and
		//! corrupts legend matching).
		enum class AgentRenderTarget
		{
			Beauty,     //!< radiance / studio-preview shading (default)
			ObjectMap,  //!< flat per-object identity segmentation + legend
			//! GUI render modes P1+P2a (docs/gui/RENDER_MODES.md §8): one of
			//! the registry's OTHER agent-visible modes -- either a
			//! ShaderPipeline data/diagnostic mode (Normals/Depth/Facets/
			//! Wireframe, `casterFactory`) or a P2a BeautyVariant mode
			//! (DeepReflect/Direct, `IsBeautyVariantMode`).  WHICH one is
			//! carried in AgentRenderParams::viewMode below;
			//! RenderCore_ branches internally
			//! (`Implementation::IsBeautyVariantMode(viewModeInfo->mode)`)
			//! between the two: a ShaderPipeline mode routes through
			//! CreateInteractiveViewModePipeline (single-ray-per-pixel
			//! exactness, `quality`/`samples` IGNORED exactly as under
			//! ObjectMap); a BeautyVariant mode routes through
			//! CreateBeautyVariantPipeline (a REAL production-class PT
			//! render at a FIXED reduced resolution + FIXED higher spp --
			//! `quality`/`samples`/`xray` are ALL ignored, but
			//! `res.effectiveSamples` reports the mode's real fixed spp, not
			//! the ShaderPipeline exactness invariant's 1).  Neither ever
			//! touches the production rasterizer.  `res.renderMode` is the
			//! registry's wire name either way (e.g. "normals" or
			//! "deep_reflect") so a caller can distinguish it from
			//! "objectmap"/"draft"/"production".
			ViewMode
		};

		//! Preview-render params (all optional; every field at its default
		//! reproduces EXACTLY today's Render(-1) behaviour -- wire-additive).
		//! `width`/`height` are a TRANSIENT film-dims override (both must be
		//! set together; clamped to [16,512] by the caller -- AgentRpc.cpp --
		//! before reaching here); 0 means "no override, use the Document's
		//! authored dims".  `camera` is the optional ephemeral camera-pose
		//! override above.
		//!
		//! Model-B F2 slice S3 (EffectiveRenderConfig): `samples` is now
		//! HONORED for rasterizers that opt in to IRasterizer::
		//! SetSampleCountOverride (the pixel-based family: PT, spectral PT,
		//! BDPT, VCM -- see that virtual's doc) via a capture/apply/restore
		//! window around the render, WITHOUT mutating the retained CST
		//! Document -- ReadDocument() stays byte-identical across a Render
		//! call regardless of `samples`, exactly like the film-dims/camera
		//! overrides above.  -1 means "no override, use the scene-authored
		//! sample count" (default).  A value < 1 other than -1 is NOT
		//! valid (the RPC layer clamps to >= 1; a direct C++ caller passing
		//! 0 or a negative value other than -1 is treated as "no override",
		//! matching the pre-existing advisory-only contract for out-of-band
		//! values).  On a rasterizer that has NOT opted in (MLT, photon-map
		//! families, AutoRasterizer's outer wrapper), the override is
		//! honestly NOT APPLIED -- see AgentRenderResult::samplesOverridden.
		//!
		//! Model-B F2 slice S3 (pinned-vs-preview): `pinned` (default false
		//! = today's PREVIEW semantics, unchanged) marks this Render /
		//! RenderAsync call as PINNED.  With the controller's single agent-
		//! render slot, a PINNED job in flight causes any NEW submission
		//! (async or sync, pinned or not) to be REJECTED rather than
		//! silently superseded -- see SceneEditController::
		//! SubmitAgentRenderAsync's `pinned` doc for the full policy
		//! (including that render_cancel / Stop() still cancel a pinned
		//! render; pinned protects against supersession, not against an
		//! explicit cancel).  Has no effect on a HEADLESS session (no
		//! controller, no slot to protect) or on the OVERRIDE-park path
		//! (RunPreviewRenderParked has no single-slot concept -- see
		//! Render(AgentRenderParams)'s doc); only the two coordinator-
		//! tracked SubmitAgentRenderAsync/Sync paths consult it.
		struct AgentRenderParams
		{
			unsigned int         width = 0;    //!< 0 = no override
			unsigned int         height = 0;   //!< 0 = no override
			//! Internal resource ceiling.  Zero leaves the session unrestricted;
			//! evaluators set a finite cap so an authored Film cannot bypass their
			//! explicit width/height preflight.
			std::uint64_t        maxPixelCount = 0;
			int                  samples = -1;  //!< -1 = no override; else the requested SPP (see EffectiveRenderConfig doc above)
			AgentCameraOverride  camera;
			bool                 pinned = false;  //!< false = preview (today's semantics); true = pinned (never silently superseded -- see doc above)
			//! Toolkit slice 2: Production (default) = today's exact
			//! behaviour, strictly additive.  Draft routes this ONE render
			//! through the ephemeral studio-preview pipeline instead of the
			//! production rasterizer -- see AgentRenderQuality's doc.  A
			//! draft render's requested `samples` (above) is CAPPED at 4
			//! regardless of the value requested (see AgentRenderResult::
			//! renderMode's doc for the honesty contract this enforces);
			//! absent a request, the preview pipeline's own 1-SPP default
			//! is used.  Composes with `width`/`height`/`camera`/`pinned`
			//! exactly as the production path does (all four are Job/Scene-
			//! level state, not rasterizer-specific).
			AgentRenderQuality   quality = AgentRenderQuality::Production;
			//! Toolkit slice 3a: Beauty (default) = today's exact
			//! behaviour, strictly additive.  ObjectMap routes this ONE
			//! render through the ephemeral identity pipeline (see
			//! AgentRenderTarget's doc) and populates
			//! AgentRenderResult::legend.  Composes with
			//! width/height/camera exactly as Beauty does (all Job/Scene-
			//! level state); `quality` and `samples` are IGNORED under
			//! ObjectMap (honestly noted in the result message).
			AgentRenderTarget    renderTarget = AgentRenderTarget::Beauty;
			//! GUI render modes P1 (docs/gui/RENDER_MODES.md §8): which
			//! ShaderPipeline data mode to render when renderTarget ==
			//! ViewMode.  Meaningless (and ignored) otherwise -- the default
			//! (Normals) is harmless precisely because it is never consulted
			//! unless a caller also sets renderTarget = ViewMode.
			RISE::Implementation::ViewportRenderMode viewMode = RISE::Implementation::ViewportRenderMode::Normals;
			//! X-ray axis (docs/gui/RENDER_MODES.md "X-ray axis"): DEFAULT
			//! TRUE (2026-07-17 user decision) -- an agent view-mode render
			//! is see-through by default, matching the viewport's own
			//! default.  Meaningful ONLY when renderTarget == ViewMode --
			//! stamps RayCaster::SetXrayViewResolve(true) on the ephemeral
			//! view-mode caster (resolve through transmissive surfaces to
			//! the first opaque hit, straight-line, no refraction bending)
			//! before shading.  Pass xray:false to inspect the transmissive
			//! surface itself instead of what's inside/behind it.  Silently
			//! IGNORED under Beauty/ObjectMap (honestly noted in the result
			//! message), matching the quality/samples-ignored precedent.
			bool                 xray = true;
			//! GUI render modes P2a (docs/gui/RENDER_MODES.md §8, deferred
			//! from P1): OPTIONAL named-view vantage override for THIS
			//! render only -- "" (default) = no override, use the active
			//! camera / an explicit `camera` override exactly as today.
			//! Resolves to the SAME ephemeral `camera` override fields above
			//! (composes for free with every existing camera-override
			//! consumer) rather than a parallel mechanism.  Valid with EVERY
			//! render target (Beauty/ObjectMap/ViewMode, draft or
			//! production).  Resolution order: (1) a live controller's
			//! in-memory named-view store (SceneEditController::
			//! FindNamedViewPose); (2) a scene CAMERA of that exact name
			//! (the honest headless fallback -- a WrapJob session has no
			//! named-view store at all).  An unresolved name is a FAILED
			//! render (res.ok=false) with the available-name list in
			//! res.message, not a silent fall-through to the active camera.
			//! If BOTH `view` and an explicit `camera` override are
			//! supplied, `view` wins (camera fields it resolves take
			//! precedence) -- see AgentSession::RenderCore_'s resolution
			//! block for the exact precedence.
			//! External review P2 fix: ONLY a PINHOLE view can be applied --
			//! AgentCameraOverride (above) has fields for the full shared
			//! position/orbit pose plus fov, and
			//! applyCameraOverride only ever SetProperty's those onto the
			//! ACTIVE camera, so a ThinLens/Fisheye/Orthographic named view's
			//! real optics (sensor/focal-length/fstop/focus-distance/
			//! aperture/tilt-shift, fisheye scale, ortho viewport scale)
			//! cannot be transferred -- rendering it would silently borrow
			//! the active camera's own optics under the requested pose. A
			//! non-pinhole resolved view is therefore ALSO a FAILED render
			//! (res.ok=false), naming the unsupported camera type in
			//! res.message, exactly like an unresolved name -- see
			//! RenderCore_'s resolution block.
			//! Re-review P2 fix: the check above is about the NAMED VIEW's
			//! own type -- it says nothing about the ACTIVE camera, which is
			//! what actually receives the override.  A resolved PINHOLE view
			//! is therefore NO LONGER rejected just because the ACTIVE
			//! camera happens to be non-pinhole: CameraIntrospection::
			//! SetProperty rejects "fov" on anything but a PinholeCamera, so
			//! doRenderWork preflights the active camera and, when it can't
			//! store a fov, applies the view's pose (location/lookat/up)
			//! ONLY and drops the fov, noting the drop honestly in
			//! res.message (" (view \"...\": FOV not applied -- ...)")
			//! instead of failing the render.  Net contract: a resolved
			//! PINHOLE `view` succeeds when the active camera can snapshot and
			//! restore that shared pose (fov is applied IFF the active camera is
			//! also pinhole); a resolved
			//! NON-pinhole `view` always fails loudly, regardless of the
			//! active camera's type -- see RenderCore_'s doRenderWork for
			//! both checks.
			std::string          view;
			//! GUI render modes P2b (docs/gui/RENDER_MODES.md §3 "light
			//! solo", §9): OPTIONAL light-selector for a single-light
			//! render -- "" (default) = no solo, every light in the scene
			//! contributes normally, exactly as today.  A non-empty name is
			//! resolved against the scene's light manager (any light, by
			//! name) then its object manager (any named object whose
			//! material is emissive, by name) and designates the match as
			//! the SOLE active light for this render: every OTHER light
			//! (explicit or mesh-emitter) contributes exactly zero direct
			//! lighting and its BSDF-sampled emission is suppressed too
			//! (see LightSampler::SetSoloLight/SetSoloLuminary's doc for the
			//! exact NEE/MIS mechanism that keeps this unbiased, not merely
			//! "mostly dark").  Valid with `beauty` (the default renderTarget)
			//! and all four BeautyVariant modes (`deep_reflect`/`direct`/
			//! `indirect`/`clay_lights`) -- an unresolved name FAILS the
			//! render (res.ok=false) with the available-name list in
			//! res.message, same contract as an unresolved `view`.  Silently
			//! IGNORED (honestly noted in the result message) under
			//! `objectmap`, the ShaderPipeline data modes (`normals`/`depth`/
			//! `facets`/`wireframe`), and `quality:"draft"` -- none of those
			//! evaluate scene lighting at all, matching the quality/samples/
			//! xray-ignored precedent used throughout this struct.
			std::string          light;
		};

		//! Toolkit slice 3b: the OPTIONAL ephemeral camera/dims overrides
		//! for query_object_at -- the SAME composition rule render's
		//! width/height/camera use (see AgentRenderParams's doc above): 0/0
		//! (the default) means "no dims override, use the Document's
		//! authored Film dims"; a default-constructed `camera` means "no
		//! camera override, use the active camera's current pose".
		//! Deliberately a narrower struct than AgentRenderParams --
		//! `samples`/`quality`/`pinned`/`renderTarget` have no meaning for a
		//! point query (it always runs the objectmap identity pipeline
		//! internally; see AgentSession::QueryObjectAt's doc for why).
		//! NAMESPACE-SCOPE (not nested in AgentSession, unlike
		//! AgentQueryObjectResult below) so it can default-construct as
		//! QueryObjectAt's own default argument value from within
		//! AgentSession's class body (a nested type's default member
		//! initializers are not yet complete at that point).
		struct AgentQueryObjectParams
		{
			unsigned int        width = 0;    //!< 0 = no override
			unsigned int        height = 0;   //!< 0 = no override
			AgentCameraOverride camera;
		};

		//! Toolkit slice 3a: one entry of an objectmap render's colour
		//! LEGEND -- the mapping a caller uses to decode the segmentation
		//! PNG.  `name` is the object's manager name (its scene-file chunk
		//! `name`, or the `<gen>[i,j]` synthesized name for an
		//! instance_array element); `colorHex` is the EXACT "#RRGGBB" 8-bit
		//! sRGB byte triple that object's pixels carry in the PNG (byte-for-
		//! byte -- match on these bytes, at NATIVE image size); `pixelCount`
		//! is how many pixels that object covers in this render.
		struct LegendEntry
		{
			std::string   name;
			std::string   colorHex;     //!< "#RRGGBB", the exact PNG bytes
			std::uint32_t pixelCount = 0;
		};

		//! The structured result of Render: the rendered head as PNG bytes
		//! plus the film dims.  `ok` is false (and `png` empty) when no head
		//! is loaded or the render failed.
		//!
		//! `meanR/meanG/meanB` are the LINEAR (pre-sRGB, pre-quantization)
		//! per-channel means over all pixels -- a stable, order-independent
		//! image signature.  RISE's PT sampler draws from a per-worker RNG
		//! whose state depends on thread scheduling, so two renders of the
		//! same head are NOT byte-identical (the PNG stream diverges
		//! wholesale on a sub-LSB pixel change); the linear channel means, by
		//! contrast, differ only by the tiny MC noise floor between runs and
		//! shift measurably under a visible edit.  Callers wanting to compare
		//! images robustly (edit-changed-the-render, not-all-black) should use
		//! these, not raw PNG bytes.
		struct AgentRenderResult
		{
			bool                       ok = false;
			unsigned int               width = 0;
			unsigned int               height = 0;
			std::vector<unsigned char> png;   //!< 8-bit sRGB PNG bytes of the final image
			double                     meanR = 0.0;   //!< linear per-channel mean (order-independent image signature)
			double                     meanG = 0.0;
			double                     meanB = 0.0;
			//! Round-3 ADDITIVE wire field: the ACTIVE rasterizer's registered
			//! type name -- IJob::GetActiveRasterizerName(), which registers
			//! each rasterizer under its scene-file CHUNK KEYWORD (e.g.
			//! "pathtracing_pel_rasterizer", "bdpt_pel_rasterizer") -- so an
			//! agent can OBSERVE which integrator is live after a rasterizer
			//! insert_chunk.  Empty when no rasterizer is active (the no-head /
			//! no-rasterizer failure paths).
			//!
			//! P2 fix (2026-07-19 mutation review): also EMPTY on any render
			//! that was refused, or threw, BEFORE doRenderWork ever entered the
			//! park -- concretely, a RunPreviewRenderParked refusal/throw or a
			//! SubmitAgentRenderSync refusal/throw (AgentSession.cpp's
			//! RenderCore_).  Those paths deliberately do NOT read
			//! mJob->GetActiveRasterizerName() on the calling thread, because
			//! by that point the park has already released (or never started)
			//! and the interactive render thread may be concurrently mutating
			//! that Job state -- an unsynchronized std::string read racing a
			//! concurrent mutation is exactly the class of bug the park fix
			//! exists to prevent.  An empty `integrator` on a failed render
			//! (`ok == false`) therefore means one of two things: no rasterizer
			//! was active, OR the render never resolved far enough to observe
			//! one (never entered the park) -- callers that need to
			//! disambiguate should inspect `message`.  Every path where
			//! doRenderWork DID run (including its own internal failure/
			//! cancellation returns) still gets a fresh, correctly-synchronized
			//! read -- doRenderWork sets this field FIRST, unconditionally, at
			//! its own top, under the park.
			std::string                integrator;
			//! Preview-render ADDITIVE wire fields: the dims ACTUALLY used for
			//! this render (== width/height above; kept as an explicit echo so
			//! a caller reading only the new fields doesn't have to cross-
			//! reference) and whether a camera override was applied (and
			//! restored) for this call.
			unsigned int               previewWidth = 0;
			unsigned int               previewHeight = 0;
			bool                       cameraOverridden = false;
			std::string                message;
			//! Model-B F2 slice S1 ADDITIVE wire field: the RenderJobId this
			//! render was assigned (see SceneEditController::RenderJobId).
			//! COORDINATOR-TRACKED (a controller is attached AND this call
			//! actually routes through RunPreviewRenderParked -- i.e. a
			//! width/height or camera override was requested): the id
			//! SceneEditController::RunPreviewRenderParked assigned --
			//! distinct and monotonically increasing across successive
			//! coordinator-tracked AND interactive renders on the SAME
			//! controller (one shared counter). EVERY OTHER CASE -- no
			//! controller attached at all, OR a controller IS attached but
			//! this particular call has no override to park for (so it never
			//! reaches RunPreviewRenderParked and calls mJob->Rasterize()
			//! directly): a SESSION-LOCAL counter on THIS AgentSession, also
			//! starting at 1 and incrementing per render, so these callers
			//! still observe distinct monotonically increasing ids -- there
			//! is no multi-session or multi-controller coordinator behind
			//! it, so don't read a session-local id as comparable across
			//! sessions OR against a coordinator-tracked id from the SAME
			//! session's OTHER render calls (the two counters are
			//! independent, BUT disjoint BY CONSTRUCTION: coordinator ids
			//! are always EVEN, session-local ids are always ODD -- see
			//! mNextSessionLocalRenderJobId's doc -- so a future
			//! Status(jobId)/Wait(jobId) can tell which counter minted a
			//! given id and reject/route accordingly). 0 is never assigned
			//! to a real render (reserved "none").  A FAILED render (ok ==
			//! false, e.g. a fail-loud camera-override rejection or an
			//! exception from mJob->Rasterize()) still carries a real,
			//! nonzero renderJobId when the render actually reached that
			//! stage -- this field names "a call that ran", not "a call
			//! that succeeded"; check `ok`, not `renderJobId != 0`, to test
			//! success.
			std::uint64_t              renderJobId = 0;
			//! Model-B F2 slice S3 (EffectiveRenderConfig) ADDITIVE wire
			//! fields.  `samplesOverridden` is true iff params.samples was
			//! present (>= 1) AND the active rasterizer accepted
			//! IRasterizer::SetSampleCountOverride for this render (capture-
			//! apply-restore around Rasterize(), no CST mutation -- see
			//! AgentRenderParams::samples's doc).  False whenever no
			//! override was requested, OR one was requested but the active
			//! rasterizer honestly does not support it (SetSampleCountOverride
			//! returned false -- e.g. MLT, a photon-map-only rasterizer, or
			//! AutoRasterizer's outer wrapper) -- `message` notes the
			//! unsupported case so a caller isn't left guessing why the
			//! override had no effect.  `effectiveSamples` is the sample
			//! count the render actually ran at: the override value when
			//! `samplesOverridden` is true, else the rasterizer's own
			//! GetSampleCountOverride() reading (its scene-authored count)
			//! when that query is cheaply available, else 0 (unknown --
			//! never guessed).
			bool                       samplesOverridden = false;
			int                        effectiveSamples = 0;
			//! Toolkit slice 2 ADDITIVE wire field: "production" (default)
			//! or "draft" -- which pipeline THIS render actually ran
			//! through (see AgentRenderParams::quality's doc).  Set
			//! unconditionally on the calling thread before any park/dispatch
			//! decision (it's a pure function of `params`, never live Job
			//! state) and persists across every later return path.
			//! `integrator` does NOT share that unconditional timing -- see
			//! its own field doc above: it is left EMPTY on any render
			//! refused/thrown before doRenderWork ever entered the park.
			//! DELIBERATELY DISTINCT from `integrator`:
			//! `integrator` always names the HEAD's active (production)
			//! rasterizer, independent of what this call rendered with --
			//! a draft render still reports the production integrator's
			//! name here, NOT "draft" or the preview pipeline's identity.
			//! Use `renderMode` to tell which shading actually produced
			//! THIS image: "draft" means the pixels came from a fixed
			//! studio-preview shader that IGNORES the scene's authored
			//! materials and lighting entirely (geometry, composition, and
			//! camera framing are representative; materials, lighting,
			//! exposure, and colour are NOT) -- never judge those from a
			//! draft image; render at quality:"production" (the default)
			//! or use ReadViewport for what the user actually sees.
			//! Toolkit slice 3a adds a THIRD value "objectmap" (set when
			//! params.renderTarget == ObjectMap) -- a flat per-object
			//! identity segmentation, distinct from both beauty modes; the
			//! `legend` below is populated only for this mode.  GUI render
			//! modes P1 (docs/gui/RENDER_MODES.md §8) adds a FOURTH family
			//! (set when params.renderTarget == ViewMode): the registry's wire
			//! name for params.viewMode -- "normals" / "depth" / "facets" /
			//! "wireframe" -- so a caller can tell exactly which data mode
			//! produced this image without cross-referencing the request.
			//! `legend` stays empty for these (view modes have no per-object
			//! identity registry -- that is ObjectMap's own thing).
			std::string                renderMode;
			//! Toolkit slice 3a: the object-colour legend of an OBJECTMAP
			//! render -- one LegendEntry per registered scene object,
			//! plus a trailing "<unmapped>" entry IFF any hit pixel
			//! resolved to no registered object.  EMPTY for every beauty
			//! (production/draft) render.  Entries are in deterministic
			//! (sorted-object-name) order.  See LegendEntry's doc.
			std::vector<LegendEntry>   legend;
		};

		//! compare_to_reference params.  `reference` is REQUIRED -- the
		//! name of a HOST-registered AgentReferenceImage (see
		//! AgentSession::SetReferenceImages).  `camera` composes EXACTLY
		//! like AgentRenderParams::camera (an ephemeral, per-field-optional
		//! override, captured and restored, never touches the document).
		//! There is deliberately NO width/height override: the comparison
		//! ALWAYS renders at the NAMED reference's own pixel dimensions (a
		//! caller cannot compare at a different resolution than the
		//! reference was authored at -- the RMSE/grid math requires
		//! pixel-for-pixel alignment).
		//!
		//! `quality`/`samples` tradeoff (deliberate, see CompareToReference's
		//! doc for the full rationale): absent `samples` (the default, -1),
		//! the comparison renders at AgentRenderQuality::Draft -- cheap
		//! (capped at 4 samples, fixed studio-preview shading), good enough
		//! to iterate on GEOMETRY/COMPOSITION/CAMERA alignment against the
		//! reference, but IGNORES the scene's authored materials and
		//! lighting entirely -- a low draft-mode RMSE says nothing about
		//! material/colour/lighting match.  Supplying `samples` (>=1)
		//! switches to AgentRenderQuality::Production at that sample count
		//! -- the real grader-equivalent measurement, and materially more
		//! expensive.  The intended loop: iterate cheaply under the Draft
		//! default while getting composition right, then pass `samples`
		//! for the real RMSE reading once composition looks plausible.
		//!
		//! `visual` (default true) additionally builds and returns the
		//! side-by-side composite diff PNG (see AgentCompareToReferenceResult::
		//! compositePng) -- set false once only the numeric feedback is
		//! needed, to save the image-encode cost and the response's token
		//! footprint.
		//!
		//! `split` (default false, back-compat -- see AgentCompareSplitResult's
		//! doc for the full mechanism): when true, ALSO renders the candidate
		//! scene a SECOND time as an ephemeral mode:"objectmap" identity
		//! segmentation (same dims/camera as the comparison render) and uses
		//! it to partition the SAME candidate-vs-reference pixel pairs into
		//! an OBJECT bucket (pixels the candidate's own objectmap marks as a
		//! real registered object) and a BACKGROUND bucket (every other
		//! pixel), each with its own RMSE.  This is the diagnostic that
		//! separates "your staging/background is done" from "the residual
		//! is the object's shape" -- once `backgroundRmse` is low, stop
		//! tuning ground/environment/lighting and spend remaining iterations
		//! on the object's silhouette and proportions instead.  Costs one
		//! EXTRA render (the objectmap pass) on top of the usual comparison
		//! render.
		//!
		//! `splitObjects` (default empty, back-compat) SCOPES the OBJECT
		//! bucket to a caller-named subset of the objectmap legend -- only
		//! meaningful when `split` is true; ignored otherwise.  Empty (the
		//! default) keeps the ORIGINAL, unscoped rule: every registered
		//! object -- whatever the objectmap legend lists -- counts as
		//! OBJECT.  That default has a sharp failure mode found in a live
		//! eval run: models routinely build a ground plane and a backdrop
		//! as ordinary registered objects to stage the hero object, so the
		//! STAGE lands in the OBJECT bucket right alongside the hero and
		//! BACKGROUND shrinks to whatever the sky/environment leaves
		//! uncovered (observed objectPixelFraction averaging 0.86 across a
		//! run).  The split then measures "geometry vs. environment", not
		//! "hero object vs. staging", which is what it is documented and
		//! used for.  Passing a non-empty `splitObjects` fixes this: ONLY
		//! legend entries whose `name` (LegendEntry::name -- the scene's own
		//! object name) appears in this list count as OBJECT; every other
		//! pixel -- INCLUDING other registered geometry such as a ground
		//! plane or backdrop -- falls into BACKGROUND.  `"<unmapped>"` is
		//! never a selectable name here (it is not a real object and is
		//! always excluded from the OBJECT bucket, scoped or not).  See
		//! AgentSession::CompareToReference's split block for how an
		//! unknown/typo'd name is surfaced in AgentCompareSplitResult::note
		//! rather than silently shrinking the mask.
		struct AgentCompareToReferenceParams
		{
			std::string              reference;
			AgentCameraOverride      camera;
			bool                     visual  = true;
			int                      samples = -1;   //!< -1 = no override (quality:Draft); >=1 = quality:Production at this SPP
			bool                     split   = false;   //!< see the doc block above this struct
			std::vector<std::string> splitObjects;      //!< see the doc block above this struct; empty = unscoped (every registered object)
		};

		//! One cell of a compare_to_reference 3x3 grid -- see
		//! AgentCompareToReferenceResult::grid's doc.
		struct AgentCompareGridCell
		{
			double rmse = 0.0;   //!< this cell's RMSE, same formula as the overall AgentCompareToReferenceResult::rmse, restricted to this cell's pixels
			double dr   = 0.0;   //!< this cell's mean signed R delta (render - reference), in [-1,1]
			double dg   = 0.0;
			double db   = 0.0;
		};

		//! compare_to_reference split:true result -- an OBJECT-vs-BACKGROUND
		//! RMSE breakdown built from the CANDIDATE's own mode:"objectmap"
		//! render (see AgentCompareToReferenceParams::split's doc for the
		//! render mechanics).  A pixel is OBJECT iff the candidate's
		//! objectmap identity render resolves it to a REAL registered
		//! object's legend colour (the reserved background/no-hit colour,
		//! and a hit on an unregistered/"<unmapped>" object, both fall into
		//! BACKGROUND) -- see AgentSession::CompareToReference's doc for the
		//! exact mask rule.
		//!
		//! HONESTY CAVEAT (read before trusting a number from this struct):
		//! the mask comes from the CANDIDATE ONLY -- the reference is a
		//! committed PNG with no objectmap of its own.  This answers "on
		//! the pixels where MY object currently is, how wrong am I" and
		//! "on my background pixels, how wrong am I" -- NOT "how wrong is
		//! the reference's object region".  A badly MISPLACED object is
		//! still informative under this rule: its pixels show high
		//! `objectRmse` (the candidate's object doesn't match what the
		//! reference has there), AND the reference's actual object pixels
		//! -- now sitting in the candidate's BACKGROUND mask, since the
		//! candidate's object isn't there -- pull `backgroundRmse` up too,
		//! rather than silently vanishing into an "object" bucket that
		//! doesn't cover them.
		struct AgentCompareSplitResult
		{
			//! true iff the candidate's objectmap render succeeded, decoded,
			//! and matched the comparison's own width/height.  false (with
			//! `note` explaining why) on ANY failure along that path --
			//! never crashes, and never fails the OVERALL compare_to_reference
			//! call (the split is a pure ADD-ON measurement).
			bool        ok = false;
			//! RMSE (identical formula to AgentCompareToReferenceResult::rmse)
			//! restricted to OBJECT-mask pixels.  Left at -1.0 when the mask
			//! is empty (no object visible in the candidate's objectmap --
			//! see `note`) so a caller can distinguish "no object pixels" from
			//! a genuine 0.0 (a pixel-perfect object match).
			double      objectRmse = -1.0;
			//! RMSE restricted to every pixel NOT in the object mask
			//! (background / no-hit / an unregistered-object hit).  Left at
			//! -1.0 when THAT mask is empty -- registered objects cover the
			//! entire frame -- for the same reason objectRmse does: a 0.0
			//! here would read as "the background matches perfectly" when in
			//! truth no background pixel was ever measured.  Both buckets
			//! sentinel symmetrically; check `>= 0.0` before trusting either.
			double      backgroundRmse = -1.0;
			//! Object-mask pixel count divided by total pixel count, in
			//! [0,1] -- how much of the frame the candidate's object(s)
			//! actually cover.
			double      objectPixelFraction = 0.0;
			//! Empty on a clean split.  Explains a `!ok` split (the
			//! objectmap render or its PNG decode failed, or its dims came
			//! back mismatched) or an ok-but-degenerate split (the object
			//! mask was empty -- camera pointed away, object off-frame or
			//! fully occluded -- `objectRmse` stays -1 and `backgroundRmse`
			//! covers the whole frame).
			std::string note;
		};

		//! The structured result of compare_to_reference.  `ok` is false
		//! (and `error` explains why) for an unregistered `reference` name,
		//! a reference PNG that fails to decode, a comparison render that
		//! fails, or a candidate/reference dimension mismatch (should not
		//! happen in practice -- the render is forced to the reference's
		//! own dims -- but checked defensively rather than indexing past
		//! either buffer's end).  `badReference` disambiguates the FIRST
		//! of those (an unregistered name -- a caller usage error naming
		//! the wrong reference) from every other failure (a render/decode
		//! problem) so AgentRpc.cpp can pick -32602 vs -32603 without
		//! string-matching `error`.
		struct AgentCompareToReferenceResult
		{
			bool        ok = false;
			std::string error;
			bool        badReference = false;
			//! Overall RMSE = sqrt(mean over all pixels*RGB of
			//! ((render-reference)/255)^2) -- THE SAME FORMULA the eval
			//! checker's "render" checkpoint compareToImage assertion uses
			//! (AgentEvalRunner.cpp's CheckRenderKind) -- i.e. this is the
			//! grader's own objective function, computed BEFORE the grader
			//! ever runs.
			double      rmse = 0.0;
			//! Mean SIGNED per-channel delta (render minus reference, each
			//! byte scaled to [0,1] before averaging) over ALL pixels -- a
			//! positive value means the render is, on average, brighter
			//! than the reference on that channel; negative means dimmer.
			//! Range [-1,1].
			double      channelDeltaR = 0.0;
			double      channelDeltaG = 0.0;
			double      channelDeltaB = 0.0;
			//! A 3x3 spatial breakdown of the SAME RMSE/delta measures
			//! above, ROW-MAJOR (index 0 = top-left ... index 8 =
			//! bottom-right; row = index/3, col = index%3).  The image is
			//! split into 3 columns and 3 rows (the last column/row
			//! absorbs any remainder when width/height isn't a multiple of
			//! 3) -- lets a caller localize WHERE the reconstruction is
			//! worst (the vision baseline's dominant failure mode was
			//! background/env staging, ~60% of pixels, that this grid is
			//! meant to surface directly rather than leave the caller to
			//! infer from one scalar).
			std::vector<AgentCompareGridCell> grid;
			//! The highest-RMSE cell's human label: "top-left",
			//! "top-center", "top-right", "middle-left", "center",
			//! "middle-right", "bottom-left", "bottom-center",
			//! "bottom-right".  Empty iff `grid` is empty (a failed
			//! comparison).
			std::string worstCell;
			//! The reference's (== the render's, forced) pixel dims.
			unsigned int width  = 0;
			unsigned int height = 0;
			//! Echoes the requested reference name (even on a failure, so
			//! a caller need not thread the request back through itself).
			std::string reference;
			//! A human-readable one-line synthesis of the measures above
			//! (RMSE + per-channel brightness delta + worst-region call-
			//! out) -- meant to be read directly, not just machine-parsed.
			std::string summary;
			//! The [render | reference | abs-diff heatmap] side-by-side
			//! composite, 8-bit sRGB PNG bytes, 3x the reference's width by
			//! its height (compositeWidth/compositeHeight echo the exact
			//! encoded dims).  EMPTY unless ok && the request's `visual`
			//! was true.  See AgentSession::CompareToReference's doc for
			//! the heatmap ramp (black -> red -> yellow -> white on mean
			//! per-pixel |delta|).
			std::vector<unsigned char> compositePng;
			unsigned int compositeWidth  = 0;
			unsigned int compositeHeight = 0;
			//! true iff the REQUEST's `split` was true (regardless of
			//! whether the split itself then succeeded) -- test THIS, not
			//! `split.ok`, to distinguish "not requested" from "requested
			//! but failed" (both leave `split.ok == false`).  When false,
			//! `split` is default-constructed and callers should not read
			//! it (mirrors the wire contract: the "split" JSON key is
			//! omitted entirely from the response unless this is true).
			bool                    hasSplit = false;
			//! The object-vs-background RMSE breakdown -- see
			//! AgentCompareSplitResult's doc.  Only meaningfully populated
			//! when `hasSplit` is true.
			AgentCompareSplitResult split;
		};

		//! Facet 5 slice S1: one entry of the skills INDEX -- `name` is the
		//! skill's filename minus ".md" (the handle a client passes back to
		//! fetch the full markdown), `title` the "# ..." first line, `hook`
		//! the "> hook: ..." second line (the one-line when-to-read cue).
		struct AgentSkillEntry
		{
			std::string name;
			std::string title;
			std::string hook;
		};

		//! Facet 5 slice S1: the result of ReadSkill.  Progressive
		//! disclosure: an EMPTY requested name returns the INDEX (`index`
		//! filled, `markdown` empty); a named request returns the full
		//! `markdown`.  `ok` is false (and `error` states why) for an
		//! unknown skill name or a rejected (unsafe / non-bare) name.
		struct AgentSkillResult
		{
			bool                         ok = false;
			std::string                  error;      //!< filled iff !ok
			std::string                  name;       //!< the requested name (named fetch)
			std::string                  markdown;   //!< the full skill markdown (named fetch)
			std::vector<AgentSkillEntry> index;      //!< the index (empty-name fetch)
			std::string                  note;       //!< index-only advisory: set when the skills ROOT DIRECTORY itself was not found (distinguishing a miswired root from a present-but-empty one; both give an empty index)
		};

		//! A headless read/validate session over a Job.  Owns the Job iff it
		//! created it (LoadFromFile); a wrapped Job (WrapJob) is non-owning.
		//!
		//! Fix-round-1 P3-a: the slice-0a doc above this line used to claim
		//! "deliberately single-threaded -- no mutex" for the WHOLE class.
		//! That has not been true since Model-B F2 slice S2a added
		//! RenderAsync: this class now has a REAL, NARROW cross-thread
		//! surface, and the actual contract is:
		//!
		//!   * mAsyncCacheMutex-guarded state (mLastPng, mLastSink,
		//!     mAsyncOutstandingJobId) is the ONLY state touched from more
		//!     than one thread -- written by the async worker thread
		//!     (RenderCore_'s cache-population tail; RenderAsync's
		//!     OutstandingGuard) and read/written by whatever thread calls
		//!     ReadImage() / ReadImage(maxEdge) / RenderAsync /
		//!     DrainAsyncRender_.  Every access to these three fields goes
		//!     through mAsyncCacheMutex; there is no unguarded access
		//!     anywhere in this class.
		//!   * EVERYTHING ELSE (mJob, mController, mNextSessionLocalRenderJobId,
		//!     ProposePatch / InsertChunk / RemoveChunk / Validate / the
		//!     synchronous Render() family, AttachController) remains
		//!     single-threaded-caller: call these from ONE thread at a time
		//!     (typically the RPC dispatcher's serving thread), same as the
		//!     original slice-0a contract -- no mutex protects them, and
		//!     none is added by this fix.
		//!   * LIFETIME across the async surface: RenderAsync's submitted
		//!     closure captures a raw `this` and runs on the ATTACHED
		//!     controller's dedicated worker thread, independent of this
		//!     session's own call stack.  ~AgentSession and AttachController
		//!     both call DrainAsyncRender_ BEFORE this object's identity
		//!     changes (destruction, or a different/null controller) --
		//!     see that method's doc.  A caller that constructs an
		//!     AgentSession, attaches a controller, and may destroy the
		//!     session (or detach) while an async render could still be in
		//!     flight relies on this drain for correctness; it is automatic
		//!     (no caller action required) as of this fix.
		class AgentSession
		{
		public:
			//! Load `path` into a fresh Job via the canonical CST path and
			//! return a session that OWNS that Job.  Returns null when the
			//! scene fails to load (not native-v7, or a derive error) so a
			//! caller can distinguish "no session" from an empty document.
			//! Secure-MCP slice 5a: `authority` defaults to Owner -- EVERY
			//! EXISTING call site (the CLI's headless loader, every
			//! pre-slice-5a test) keeps committing directly without a code
			//! change.  Pass External to construct a test/CLI seam for the
			//! staging path this slice adds (the real wire-level External
			//! construction path -- a remote transport -- is slice 5b).
			static std::unique_ptr<AgentSession> LoadFromFile( const std::string& path,
			                                                    AgentAuthority authority = AgentAuthority::Owner );

			//! Wrap an EXISTING Job (non-owning: the caller keeps ownership
			//! and must outlive the session).  Used when the GUI / a host
			//! already holds a CST-loaded Job (L2: the GUI is just another
			//! agent).  Null `job` returns null.  Secure-MCP slice 5a:
			//! `authority` defaults to Owner (see LoadFromFile's doc for the
			//! back-compat rationale) -- the GUI's own construction sites
			//! are unaffected; an External-authority session over a live
			//! controller is the two-session pattern this slice's tests use.
			static std::unique_ptr<AgentSession> WrapJob( IJobPriv* job,
			                                               AgentAuthority authority = AgentAuthority::Owner );

			//! Secure-MCP slice 5a: this session's construction-time
			//! authority (Owner / External).  Wire-immutable -- never
			//! changed after construction.
			AgentAuthority Authority() const { return mAuthority; }

			//! Secure-MCP slice 5c: a human-readable, caller-supplied label
			//! naming WHO this session speaks for -- stamped into every
			//! AgentProposal this session stages (ProposePatch/InsertChunk/
			//! RemoveChunk's External-authority branches) so ListProposals /
			//! the GUI's proposals panel can show which session proposed a
			//! given edit.  Unlike mAuthority this is NOT wire-immutable: it
			//! is set POST-construction (the GUI-hosted loopback transport
			//! constructs the session first, then labels it once it knows
			//! which connection/client it is serving -- see
			//! RISEViewportBridge.mm's StartAgentHostedServer).  Purely
			//! diagnostic -- never consulted by any gating decision (the
			//! Owner-only-may-resolve gate is authority-based, not label-
			//! based).  Defaults to "" (every existing construction site --
			//! LoadFromFile/WrapJob, the headless CLI transports -- leaves
			//! this unset and StageProposal keeps stamping an empty label,
			//! byte-for-byte the pre-5c behaviour).
			const std::string& SessionLabel() const { return mSessionLabel; }
			void SetSessionLabel( const std::string& label ) { mSessionLabel = label; }

			//! Fix-round-1 P1-A: drains any OUTSTANDING async render before
			//! this session is destroyed.  RenderAsync's submitted closure
			//! captures a raw `this` and runs on the controller's dedicated
			//! worker thread; without a drain, ~AgentSession could return
			//! (and the caller could then destroy the controller / Job too)
			//! while that closure is still running RenderCore_ on THIS,
			//! now-being-destroyed, object -- a real, reachable
			//! use-after-free (the shipped Mac GUI's live JSON-RPC panel
			//! sequence: AgentRpcDispatcher owns a unique_ptr<AgentSession>;
			//! ~AgentRpcDispatcher's implicit member-destruction runs before
			//! RISEViewportBridge's -shutdown gets to
			//! RISE_API_DestroySceneEditController).  See DrainAsyncRender_
			//! for the mechanism (cancel-then-wait, bounded).
			~AgentSession();

			//! Facet 5 slice 1b: attach a LIVE SceneEditController so
			//! ProposePatch routes its set-param commit through the
			//! controller's render-thread-SAFE edit path (cancel-and-park +
			//! rebind-after-D2) instead of calling Job::ApplyCstParamEdit
			//! DIRECTLY.  Attach this when the session shares a Job with a
			//! running interactive editor (a live GUI), so an agent commit
			//! cannot race the render thread or dangle the editor's cached
			//! pointers on a D2 re-derive.  Pass `nullptr` to DETACH (revert
			//! to the direct-Job path).  The controller is BORROWED -- the
			//! caller keeps ownership and must outlive the session (or detach
			//! first).  The controller's Job MUST be the SAME Job this session
			//! wraps (the caller's responsibility; a live GUI builds both over
			//! one Job).  When NOT attached (the default / headless slice-0
			//! mode), ProposePatch is byte-for-byte its prior behaviour.
			//!
			//! Fix-round-1 P1-A: drains any outstanding async render against
			//! the CURRENTLY attached controller (if any) BEFORE switching --
			//! detaching (or re-attaching to a DIFFERENT controller) while a
			//! RenderAsync submission against the OLD controller is still in
			//! flight would otherwise leave that closure racing a controller
			//! this session no longer considers live.  See DrainAsyncRender_.
			void AttachController( SceneEditController* controller );

			//! True iff a live controller is attached (ProposePatch routes
			//! through it).
			bool HasController() const { return mController != nullptr; }

			//! The canonical `.RISEscene` text of the head -- SerializeCst of
			//! the retained CST Document.  Returns "" when the Job retains no
			//! Document (a Job not loaded via the CST path); `HasDocument()`
			//! distinguishes that empty-because-absent case from a genuinely
			//! empty scene.
			std::string ReadDocument() const;

			//! True iff the wrapped Job retains a CST Document (so
			//! ReadDocument / the head is meaningful).
			bool HasDocument() const;

			//! Facet 5 slice 1a: the retained CST head's (uuid,revision)
			//! optimistic-concurrency identity (see RISE::Cst::CstHeadVersion).
			//! {0,0} when there is no wrapped Job (or the Job retains no head).
			//! An agent reads this alongside ReadDocument, then passes it back as
			//! a patch's baseVersion so a stale edit is rejected with a CONFLICT.
			RISE::Cst::CstHeadVersion HeadVersion() const;

			//! The descriptor-generated JSON schema (charter L6): one chunk
			//! when `keyword` is non-empty, else the whole grammar.
			std::string ReadSchema( const std::string& keyword = std::string() ) const;

			//! THE KEYSTONE.  Validate a CANDIDATE scene text with NO side
			//! effects on this session's Job: parse it to a CST, derive it
			//! into a THROWAWAY Job, and map the derive diagnostics into
			//! structured AgentDiagnostics with best-effort byte-offset
			//! localization (see AgentSession.cpp for the localization
			//! strategy and its honest slice-0 limits).  An empty result
			//! means "no errors" (the candidate is valid at the semantic
			//! phase this slice covers).
			std::vector<AgentDiagnostic> Validate( const std::string& candidateText ) const;

			//! The STATELESS validation core.  `Validate()` above is a thin
			//! forwarder to this; the logic references NO member state (it
			//! parses `candidateText` to a CST and derives it into a THROWAWAY
			//! Job -- never this session's mJob), so it is exposed as a static
			//! for the transport's no-head bootstrap: an agent CONSTRUCTING or
			//! REPAIRING a scene from scratch (the CLI's `--agent-stdio` with
			//! no scene loaded) must be able to `validate` a candidate BEFORE
			//! any head exists.  Identical result to `Validate()`.
			static std::vector<AgentDiagnostic> ValidateText( const std::string& candidateText );

			//! Facet 5 slice S1: read_skill -- STATELESS, like ReadSchema /
			//! ValidateText (references NO member state; exposed static so the
			//! transport's no-head bootstrap can read skills before any head
			//! exists).  Progressive disclosure: an EMPTY `name` returns the
			//! INDEX (name + title + one-line hook per skill, scanned from the
			//! *.md files in the skills root); a non-empty `name` returns that
			//! skill's full markdown.
			//!
			//! SKILLS ROOT RESOLUTION (first hit wins):
			//!   1. $RISE_SKILLS_PATH               (when set and non-empty)
			//!   2. $RISE_MEDIA_PATH + "skills/agent/"  (when set and non-empty)
			//!   3. "./skills/agent/"               (cwd fallback -- the repo
			//!      root when run from a checkout, matching run_all_tests.sh)
			//! Reads are READ-ONLY; nothing is ever written under the root.
			//!
			//! PATH SAFETY: `name` must be a BARE filename component -- any
			//! '/', '\\', or ".." is REJECTED (no traversal), and only files
			//! ending ".md" inside the root are served (the ".md" suffix is
			//! appended by this call; the caller passes the bare skill name).
			//! An unknown name -> ok=false with a clean error message.
			//!
			//! MEMBERSHIP: a named fetch is served ONLY for a name present in
			//! the index (the same regular-*.md-files listing the no-name call
			//! returns) -- the fetchable set IS the listed set.  So dotfiles,
			//! directories named "<x>.md", FIFOs, and Windows device names
			//! (CON / NUL) are never opened.  SYMLINKS ARE FOLLOWED: a symlink
			//! under the root that resolves to a regular file is listed and
			//! served.  That is deliberate and honest -- this is a
			//! trusted-OPERATOR surface (the root comes from the operator's
			//! environment above, never from agent input; the agent only picks
			//! names off the operator's own index).
			static AgentSkillResult ReadSkill( const std::string& name = std::string() );

			//! propose_patch (slice 0b: STRUCTURED set only).  Apply one
			//! param-value edit to the retained CST Document via
			//! Job::ApplyCstParamEdit -- the SAME call the GUI property panel
			//! makes -- then let that call re-derive the live Job (incremental
			//! or D2 full re-derive) so the head's derived Scene stays
			//! consistent with the mutated Document, EXACTLY as the GUI does.
			//! The result's `applied` is TRUE only for a CLEAN apply (rawCode
			//! 1 or 2); `status` is the tri-state gate {"applied","rejected",
			//! "diagnosed"} (see AgentPatchResult).  A rawCode-3 re-derive
			//! (mutated + managers replaced BUT diagnostics emitted) maps to
			//! applied=FALSE / status="diagnosed" -- the source contract treats
			//! 3 as a failure, so this surface does NOT report it as a success.
			//! No retained Document (or an empty target/param/value) ->
			//! applied=false, status="rejected", head byte-identical.
			//! Facet 5 slice 1a: optimistic concurrency.  When `patch.hasBaseVersion`
			//! is set, the base precondition is checked FIRST, BEFORE any mutation:
			//! if `patch.baseVersion` != the Job's current head-version the patch is
			//! REJECTED with status="conflict" (applied=false, head byte-identical) --
			//! a stale patch never touches the Document.  On every path the result's
			//! `headVersion` is populated (post-commit on a clean apply; the current
			//! head on reject/diagnosed/conflict).  Absent baseVersion -> the edit is
			//! UNCONDITIONAL (slice-0 back-compat).
			//!
			//! Secure-MCP slice 5a: the authority x autonomy matrix.  This
			//! session's construction-time Authority() gates what happens
			//! BEFORE any of the above:
			//!   * Owner              -> unchanged: falls straight through to
			//!            the direct-commit / LIVE-controller-commit behaviour
			//!            documented above (byte-for-byte, zero regression).
			//!   * External + a live controller attached -> STAGES the edit
			//!            instead of committing it: the controller's queue
			//!            gets a new AgentProposal (INERT -- no Document
			//!            mutation, no EditHistory record, no render kick),
			//!            and this call returns applied=false,
			//!            status="staged", `message` naming the proposalId
			//!            (also embedded so a caller need not additionally
			//!            call the C-API's own StageProposal accessor) --
			//!            an Owner-authority session resolves it later via
			//!            ResolveProposal.  `headVersion` is the CURRENT
			//!            (unchanged) head, same convention as a reject.
			//!   * External + NO controller attached (headless) -> REFUSED:
			//!            applied=false, status="rejected", `message`
			//!            explains staging needs a live Owner to resolve
			//!            against.  A headless External session has nowhere
			//!            to stage TO (no queue exists without a
			//!            controller), so this is a hard refusal, not a
			//!            silent commit -- the "external commits with no
			//!            human gate" hole this slice closes must not have a
			//!            headless back door.
			AgentPatchResult ProposePatch( const AgentSetPatch& patch );

			//! BATCH form of ProposePatch (propose_patches): apply MULTIPLE
			//! parameter edits across one or several named entities in ONE call
			//! instead of one round-trip per patch.
			//!
			//! Semantics are SEQUENTIAL and BEST-EFFORT, delegating to ProposePatch
			//! for each element in `patches`.  `baseOrNull` is checked against the
			//! FIRST element only; subsequent elements pass nullptr to apply
			//! against the evolving head.  A REJECTED element does NOT stop the
			//! batch -- later elements are still attempted so their own results
			//! are informative even when they depended on the rejected one.
			//!
			//! ONE EXCEPTION -- a STALE-BASE CONFLICT is batch-fatal.  When the
			//! caller supplied `baseOrNull` and the first element comes back
			//! status=="conflict", the document has moved since the caller last
			//! read it, so the WHOLE batch's precondition has failed and no
			//! further element is attempted.  This differs DELIBERATELY from
			//! InsertChunks, which continues: an insert is ADDITIVE (racing a
			//! concurrent editor merely interleaves new entities), whereas a
			//! patch OVERWRITES an existing value -- continuing past a stale
			//! base would blind-clobber a co-editor's concurrent edits for
			//! elements 1..N-1, the classic lost update.  The returned vector
			//! still has ONE entry per input element (so results[i] always
			//! corresponds to patches[i]); the unattempted tail carries
			//! applied=false, status="conflict" and a message saying so.
			//! Re-read the head and resubmit the batch.
			std::vector<AgentPatchResult> ProposePatches( const std::vector<AgentSetPatch>& patches,
			                                              const RISE::Cst::CstHeadVersion* baseOrNull = nullptr );


			//! Model-B F5 slice S2 (insert_chunk): ADD one complete chunk (a
			//! `keyword { ... }` block, braces on their own lines) to the head
			//! and REALIZE it in the live scene via a dry-run-guarded FULL
			//! re-derive -- a failed dry-run leaves the Document AND the live
			//! scene byte-identical (no half-applied state).  Validation
			//! (all authoritative in Job, under the controller's lock in LIVE
			//! mode): exactly ONE chunk with nothing but whitespace around it
			//! (headers / directives / multi-chunk text refused); a duplicate
			//! (kind,name) against an existing chunk refused early with a
			//! clean message (variant overlays exempt); an in-context derive
			//! failure refused with the first dry-run diagnostic.  LIVE mode
			//! (controller attached) routes through the render-safe
			//! ApplyAgentInsertChunk (park + conflict gate + rebind + dirty +
			//! kick); headless calls Job::ApplyCstInsertChunk directly with
			//! the same conflict gate here.  `baseOrNull` is the OPTIONAL
			//! optimistic-concurrency precondition (see ProposePatch).
			//! Secure-MCP slice 5a: subject to the SAME authority x autonomy
			//! matrix as ProposePatch (see that method's doc) -- an External-
			//! authority session with a live controller attached STAGES this
			//! insert instead of committing it (applied=false,
			//! status="staged"); with no controller attached it is refused.
			AgentChunkResult InsertChunk( const std::string& chunkText,
			                              const RISE::Cst::CstHeadVersion* baseOrNull = nullptr );

			//! BATCH form of InsertChunk (insert_chunks): apply MULTIPLE
			//! complete chunks in ONE call instead of one round-trip per
			//! chunk.  Motivation: a scene assembled one insert_chunk at a
			//! time costs N tool calls / N LLM round-trips for an N-chunk
			//! scene (measured: 70-140 inserts per agent run, dominating
			//! tool use and re-sent context) -- batching collapses that to
			//! ONE call.
			//!
			//! Semantics are SEQUENTIAL and BEST-EFFORT, and this method
			//! does NOT duplicate InsertChunk's logic -- it simply calls
			//! InsertChunk once per element of `chunkTexts`, IN ORDER, and
			//! collects one AgentChunkResult per input (same size and order
			//! as `chunkTexts`).  Because each call lands (or is rejected)
			//! before the next one runs, a chunk EARLIER in the batch that
			//! is referenced by a LATER chunk resolves cleanly -- e.g. a
			//! painter at index 0 followed by a material at index 1 that
			//! references it -- exactly as if the two had been separate
			//! insert_chunk calls in the same order, without the caller
			//! having to round-trip in between.
			//!
			//! A REJECTED element does NOT stop the batch: the remaining
			//! elements are still attempted in order.  If a later element
			//! depended on the rejected one, it will simply also fail --
			//! with its own actionable `issues` diagnostic (see InsertChunk
			//! / AttachRejectionIssues) -- which is informative rather than
			//! silently swallowed.  Callers should check every element's
			//! `status`/`applied`, not just the overall return.
			//!
			//! `baseOrNull` is the OPTIONAL optimistic-concurrency
			//! precondition for the BATCH AS A WHOLE (see ProposePatch /
			//! InsertChunk): it is passed to the FIRST element's InsertChunk
			//! call only.  Every subsequent element passes nullptr, so it
			//! applies against the head as it stands AFTER the prior
			//! elements in this same batch -- the batch is one logical
			//! operation and only its start needs to be pinned to the
			//! caller's observed head; re-checking it after every
			//! self-inflicted mutation would be meaningless.
			//!
			//! Authority / LIVE-vs-headless routing, conflict detection, and
			//! per-chunk `issues` diagnostics are ALL inherited unchanged
			//! from InsertChunk (including the Secure-MCP External-authority
			//! staging behaviour) -- there is nothing batch-specific to
			//! those concerns.
			//!
			//! `chunkTexts` empty -> returns an empty vector (no-op); no
			//! InsertChunk call is made.
			std::vector<AgentChunkResult> InsertChunks( const std::vector<std::string>& chunkTexts,
			                                            const RISE::Cst::CstHeadVersion* baseOrNull = nullptr );

			//! Model-B F5 slice S2 (remove_chunk): REMOVE the chunk resolved
			//! by bare name `target` (+ optional `kind` keyword-suffix
			//! narrowing -- the SAME resolution rules as ProposePatch,
			//! including the sole-unnamed-camera positional fallback) and drop
			//! the entity from the live scene via the same dry-run-guarded
			//! full re-derive.  The Document erase is the TRIVIA-PRESERVING
			//! Cst::DocEraseChunkTidy -- safe for FILE-AUTHORED chunks (never
			//! the clone-undo-only idx-1 drop).  Unknown target -> rejected;
			//! ambiguous -> rejected with a disambiguation hint; a target
			//! still REFERENCED by another chunk fails the dry-run and is
			//! rejected with the diagnostic, head byte-identical.
			//! Secure-MCP slice 5a: subject to the SAME authority x autonomy
			//! matrix as ProposePatch (see that method's doc) -- an External-
			//! authority session with a live controller attached STAGES this
			//! remove instead of committing it (applied=false,
			//! status="staged"); with no controller attached it is refused.
			AgentChunkResult RemoveChunk( const std::string& target,
			                              const std::string& kind = std::string(),
			                              const RISE::Cst::CstHeadVersion* baseOrNull = nullptr );

			//! Secure-MCP slice 5a: one entry of ListProposals -- a wire-
			//! friendly flattening of SceneEditController::AgentProposal (see
			//! that struct's doc for field meaning; `kind` here is the
			//! string form "param_edit"/"insert_chunk"/"remove_chunk" rather
			//! than the controller's internal enum, so this header does not
			//! need to expose SceneEditController's type to a caller that
			//! only wants the read-only listing).
			struct AgentProposalEntry
			{
				std::uint64_t             id = 0;
				std::string               kind;          //!< "param_edit" / "insert_chunk" / "remove_chunk"
				std::string               target;
				std::string               entityKind;
				std::string               param;
				std::string               value;
				std::string               chunkText;
				RISE::Cst::CstHeadVersion baseVersion;
				std::string               sessionLabel;  //!< diagnostic: which session staged it. Plumbed end-to-end since 5a (this struct, StageProposal, SceneEditController::AgentProposal); "" through 5a/5b (no session-identifying name/label existed yet to stamp in). Secure-MCP slice 5c: populated whenever the staging session had SetSessionLabel() called on it -- the GUI-hosted loopback transport labels its External session (e.g. "external-http") at server-start time, so a proposal staged by a real remote MCP client now carries a human-readable "who proposed this" string; a session that never calls SetSessionLabel (every pre-5c construction site) still stages "" (unchanged).
				std::string               status;        //!< "pending" / "applied" / "rejected" / "conflict"
			};

			//! Secure-MCP slice 5a: list every proposal staged on the
			//! ATTACHED controller's queue (pending and resolved -- resolved
			//! proposals stay visible for audit).  CONTROLLER-ATTACHED ONLY:
			//! a headless session has no shared queue to list against and
			//! returns an empty vector.  Callable by ANY authority (listing
			//! is a read, not a mutation or a resolve) -- the Owner-only
			//! gate applies to ResolveProposal, not to this.
			std::vector<AgentProposalEntry> ListProposals() const;

			//! Secure-MCP slice 5a: the structured result of ResolveProposal.
			struct AgentResolveResult
			{
				bool        ok = false;        //!< true iff the resolve actually ran (id found, this session's authority permits it); false leaves `message` explaining why
				std::string status;            //!< "applied" / "rejected" / "conflict" -- meaningful only when ok
				AgentPatchResult   paramResult;    //!< filled iff ok && the resolved proposal was a ParamEdit
				AgentChunkResult   chunkResult;     //!< filled iff ok && the resolved proposal was Insert/RemoveChunk
				std::string message;
			};

			//! Secure-MCP slice 5a: approve or reject the proposal named by
			//! `proposalId` on the ATTACHED controller's queue.
			//!
			//! OWNER-ONLY GATE (enforced HERE, not trusted to the caller):
			//! an External-authority session may NOT resolve ANY proposal --
			//! not even one it staged itself.  This is the "external cannot
			//! approve its own proposal" invariant: refused with `ok=false`
			//! before the controller is even consulted.
			//!
			//! CONTROLLER-ATTACHED ONLY: a headless session has no shared
			//! queue to resolve against; refused with `ok=false`.
			//!
			//! On a real resolve, delegates to
			//! SceneEditController::ResolveProposal -- see that method's doc
			//! for the hard base-version re-check-then-apply invariant.
			//! `approve=false` (reject) never re-checks anything and never
			//! mutates.  `approve=true` (approve) re-checks the STAGED
			//! baseVersion against the controller's CURRENT head atomically
			//! with the apply; a stale proposal (the head moved since it was
			//! staged, including via a reload that minted a fresh uuid)
			//! resolves to status="conflict" and is NOT applied.
			AgentResolveResult ResolveProposal( std::uint64_t proposalId, bool approve );

			//! compare_to_reference: register (or wholesale REPLACE) the set
			//! of reference images THIS session's compare_to_reference verb
			//! may grade against.  HOST-PROVIDED ONLY -- there is no wire
			//! verb that lets a remote/agent caller add, remove, or rename
			//! an entry; only the embedding host calls this (the eval
			//! runner pre-flight-loading a scenario's prompt-attachment
			//! images -- see AgentEvalRunner.cpp's RunScenarioDriven; a
			//! future GUI could pass chat attachments the same way),
			//! typically once, before the session starts serving requests.
			//! Empty by default -- every EXISTING construction site
			//! (LoadFromFile/WrapJob) leaves this unset, so
			//! compare_to_reference cleanly refuses every name (see
			//! AgentCompareToReferenceResult::badReference) until a host
			//! registers at least one image; back-compat by construction.
			//! Replaces the ENTIRE set, not additive -- a second call fully
			//! supersedes the first (no accumulation across calls).
			void SetReferenceImages( std::vector<AgentReferenceImage> images )
			{
				mReferenceImages = std::move( images );
			}

			//! compare_to_reference: render the live head at the NAMED
			//! reference's exact pixel dims (see AgentCompareToReferenceParams'
			//! doc for the quality/samples tradeoff and the camera-override
			//! composition), decode both PNGs through the SAME in-tree
			//! PNGReader path read_image's underlying bytes / the eval
			//! checker's compareToImage assertion use, and report the
			//! grader's own RMSE objective function PLUS a 3x3 spatial
			//! breakdown and (optionally) a visual [render|reference|diff]
			//! composite -- see AgentCompareToReferenceResult's doc for the
			//! full field-by-field contract.
			//!
			//! NEVER mutates the retained Document (like every Render call,
			//! the comparison render's camera/dims overrides are captured
			//! and restored) and NEVER touches ReadImage()'s cache
			//! differently than an ordinary Render call would -- a
			//! compare_to_reference call's render is cached for ReadImage()
			//! exactly like any other successful Render, so a caller CAN
			//! read_image afterward to see the same frame the comparison
			//! graded.  This holds for `split:true` too: that path runs a
			//! SECOND, ephemeral objectmap render to build its mask, and
			//! since Render() caches every success, the implementation
			//! stashes and restores the beauty cache (under an RAII guard)
			//! around it, so the objectmap never PERSISTS past the call.
			//! Precisely: the cache transiently holds the objectmap for the
			//! width of that internal render, so a ReadImage() issued from
			//! ANOTHER thread mid-compare can still observe it -- bounded and
			//! self-healing, unlike the unguarded behaviour it replaced,
			//! which was permanent.  Single-threaded callers never see it.
			//! Locked by the "cache-survival" case in AgentViewportReadTest.
			AgentCompareToReferenceResult CompareToReference( const AgentCompareToReferenceParams& params );

			//! render + read_image (slice 0b): render the current head into an
			//! in-memory sRGB PNG and return the bytes + film dims.  Headless
			//! (no window).  A render NEVER mutates the retained Document --
			//! ReadDocument() is byte-identical across a Render call.
			//! `samplesOverride` is currently IGNORED (retained for API
			//! stability): a render-scoped sample override cannot be applied
			//! without either mutating the CST or a transient IRasterizer
			//! sample-count setter that does not exist in slice 0b, so it is
			//! deferred to the EffectiveRenderConfig layer
			//! (docs/agentic-redesign/50-agentic-surface.md §2.2.5); the render
			//! uses the AUTHORED sample count.  The bytes of a SUCCESSFUL
			//! render are cached for ReadImage().
			//!
			//! LIVE-MODE SAFETY (investigated for the preview-render work;
			//! updated for Model-B F2 slice S2a, which CLOSED the race this
			//! comment used to document as pre-existing/unchanged -- see
			//! RenderCore_'s "S2a CLOSES the pre-existing race" comment in
			//! AgentSession.cpp for the fix itself): when a controller IS
			//! attached, EVERY controller-attached render this method makes
			//! -- override or not -- now runs on the controller's dedicated
			//! agent-render worker under the SAME cancel-and-park critical
			//! section the interactive render loop respects
			//! (SceneEditController::SubmitAgentRenderSync for the no-
			//! override case, RunPreviewRenderParked for the override case),
			//! never unserialized against DoOneRenderPass.  Empirically
			//! max-concurrency-1: the controller's single agent-render slot
			//! means a render already in flight causes a NEW submission
			//! (sync or async, pinned or not) to be REFUSED rather than
			//! racing it -- see SubmitAgentRenderAsync's `pinned` doc.  Only
			//! a TRULY HEADLESS session (no controller attached at all --
			//! e.g. `rise --agent-stdio` with no live GUI) still calls
			//! mJob->Rasterize() directly, which is correct there: there is
			//! no interactive render thread in that topology to race in the
			//! first place.  A plain Render(-1) (or an all-absent-params
			//! Render(AgentRenderParams)) is routed through this SAME
			//! controller-attached path when a controller exists; it only
			//! skips the FILM-DIMS/CAMERA override-mutation window (the
			//! preview-render-specific park), not the render-serialization
			//! path itself.
			AgentRenderResult Render( int samplesOverride = -1 );

			//! Preview-render (F5 the cheap multi-angle observe loop): the SAME
			//! Render as above, plus the OPTIONAL transient overrides in
			//! `params` (film dims / camera pose).  Wire-additive: a
			//! default-constructed AgentRenderParams (every field absent)
			//! reproduces Render(-1) EXACTLY, byte-for-byte, including the
			//! Document byte-identity guarantee.
			//!
			//! Film-dims override: capture the Scene's CURRENT Film dims,
			//! Job::SetFilm to the requested preview dims (a LIVE-only
			//! mutation of the derived Scene -- it does NOT touch the CST
			//! Document; see Job::SetFilm / ScaleFilmToFit), Rasterize, then
			//! SetFilm back to the captured dims.  Camera override: capture
			//! every requested field's CURRENT value via
			//! CameraIntrospection::GetPropertyValue, SetProperty the
			//! overrides onto the ACTIVE camera, Rasterize, then SetProperty
			//! every captured field back.  Restoration is RAII-guarded (an
			//! internal RenderOverrideRestoreGuard, see AgentSession.cpp) so
			//! it runs on EVERY exit from the render window -- including an
			//! exception unwinding out of Rasterize() itself (OIDN denoise is
			//! a documented real throw site): the film dims / camera pose are
			//! never left permanently overridden just because the render
			//! failed mid-flight.
			//!
			//! FAIL-LOUD camera-override validation: every requested camera
			//! field's `CameraIntrospection::SetProperty` return is checked.
			//! If ANY field fails to apply (a malformed value that bypassed
			//! the RPC-layer shape validation in AgentRpc.cpp -- e.g. a
			//! direct C++ caller), the call returns `ok=false` with a
			//! `message` naming the failed field, `cameraOverridden=false`,
			//! and does NOT render -- every field that WAS applied before the
			//! failure is restored first.  This never reports
			//! `cameraOverridden=true` on a partial or no-op override.
			//!
			//! Toolkit slice 2: `params.quality == AgentRenderQuality::Draft`
			//! routes the render through a wholly SEPARATE, EPHEMERAL
			//! studio-preview pipeline instead of the production
			//! rasterizer -- see AgentRenderQuality's doc for the honesty
			//! contract and AgentSession.cpp's RenderCore_ for the
			//! isolation mechanism (never touches the production
			//! rasterizer, its FrameStore, or its outputs).  The film-dims
			//! and camera-pose overrides above still apply identically in
			//! either mode (both are Job/Scene-level state, not
			//! rasterizer-specific); the single-agent-render-slot /
			//! cancel-and-park machinery below also applies identically --
			//! a draft render is just as genuinely cancellable as a
			//! production one.
			//!
			//! LIVE mode (a controller is attached): the mutate-render-restore
			//! window for BOTH overrides runs under
			//! SceneEditController::RunPreviewRenderParked -- the render
			//! thread's OWN per-pass Film-dims / camera-frame swap
			//! (DoOneRenderPass) touches the SAME shared Film/cameras with NO
			//! lock of its own, so an unparked override would race it.  When
			//! parking is refused (an editor transaction is open), the
			//! override is likewise refused -- see the honest failure mode in
			//! the result's `message` (dims/camera stay at the Document's
			//! authored values and `cameraOverridden` is false; the render
			//! still runs, just without the requested override).  Headless
			//! (no controller attached) mode has no interactive thread to race
			//! and applies the override directly.
			AgentRenderResult Render( const AgentRenderParams& params );

			//! Model-B F2 slice S2a: the result of RenderAsync -- either the
			//! render was ACCEPTED (submitted to the controller's dedicated
			//! agent-render worker; `renderJobId` names it) or REFUSED
			//! (no controller attached, an editor transaction is open, or
			//! the worker's single slot is already occupied).
			struct AgentRenderAsyncResult
			{
				bool          accepted = false;
				std::uint64_t renderJobId = 0;   //!< 0 when !accepted
				std::string   message;
				//! Model-B F2 slice S3 ADDITIVE wire field: echoes the
				//! `pinned` flag this submission was made with (regardless
				//! of `accepted` -- a caller can see what it ASKED for even
				//! on a refusal).
				bool          pinned = false;
			};

			//! Model-B F2 slice S2a: submit a render to run ASYNCHRONOUSLY on
			//! the ATTACHED controller's dedicated agent-render worker, and
			//! return IMMEDIATELY (does not block for the render's
			//! duration).  CONTROLLER-ATTACHED ONLY: `HasController()` must
			//! be true, else this refuses outright (`accepted=false`,
			//! `message` explains why) -- a headless session has no
			//! coordinator/worker to submit to, and this is deliberately
			//! NOT silently downgraded to a synchronous direct call (that
			//! would defeat the caller's async intent without telling
			//! them). Same override semantics as Render(params): film-dims
			//! / camera-pose overrides are captured, applied, and restored
			//! around the render exactly as the synchronous path does --
			//! the restore happens on the WORKER thread before it reports
			//! the job complete, so by the time GetRenderJobStatus/
			//! WaitForRenderJob observes {active:false}, the Document and
			//! camera/film LIVE state are back to their pre-call values.
			//! `outImage`/`outMeans`-style results are NOT returned here
			//! (there is nothing to return yet -- the render hasn't run);
			//! poll RenderStatus(renderJobId) and, once complete, ReadImage()
			//! for the cached PNG (populated on a successful async render
			//! exactly like a successful synchronous one).
			AgentRenderAsyncResult RenderAsync( const AgentRenderParams& params );

			//! Model-B F2 slice S2b: cancel the OUTSTANDING async render (if
			//! any), WITHOUT blocking for it to finish -- a public, callable-
			//! any-time sibling of the private DrainAsyncRender_ (which is
			//! cancel-THEN-WAIT, unbounded, and only reachable from
			//! ~AgentSession / AttachController).  A caller (the `render_cancel`
			//! RPC verb) that just wants to trip the cancel flag and let the
			//! worker's normal completion path (RenderStatus / RenderWait /
			//! ReadImage) observe the result promptly, without tying up the
			//! calling thread, uses this instead.
			//!
			//! Mechanism: reuses SceneEditController::CancelAgentRender_ --
			//! the SAME CancellableProgressCallback (mCancelProgress) an
			//! in-flight agent render's doRenderWork installed on the Job
			//! before calling Rasterize() (see AgentRenderProgress()'s doc).
			//! Idempotent and safe to call whether or not a render is
			//! actually in flight (a no-op cancel on an idle controller, or
			//! when no controller is attached at all).
			//!
			//! CONTROLLER-ATTACHED ONLY in the sense that matters: with no
			//! controller attached there is no async render this session
			//! could have submitted (RenderAsync itself refuses outright in
			//! that case), so this degrades to a harmless no-op rather than
			//! an error -- unlike RenderAsync, there is no "wrong intent"
			//! reading of calling cancel when nothing is outstanding.
			//!
			//! `renderJobId`, if nonzero, is advisory only: it is NOT
			//! currently used to target a SPECIFIC job (SceneEditController's
			//! agent-render worker is single-slot, so there is at most one
			//! outstanding async render to cancel at a time regardless of
			//! which id the caller names); it exists in the signature so a
			//! future multi-slot worker can route the cancellation to the
			//! right job without an ABI change to this method, and so the
			//! RPC verb can echo the caller's id in its response for a
			//! sanity mismatch check.  This method does NOT wait for the
			//! render to actually stop -- the worker's own completion path
			//! (observed via RenderStatus/RenderWait) is what reports
			//! "actually cancelled" (typically within tens of ms, per
			//! DrainAsyncRender_'s doc -- live-measured 7-20ms in
			//! AgentRenderAsyncTest.cpp's Stop()-during-a-render red-prove).
			//! Does NOT clear mAsyncOutstandingJobId (unlike
			//! DrainAsyncRender_) -- that id is retired by the worker's own
			//! OutstandingGuard when the render actually completes, exactly
			//! as it is for an uncancelled render; this method only trips
			//! the cancel flag, it does not pretend the job is done.
			void CancelAsyncRender( std::uint64_t renderJobId = 0 );

			//! Model-B F2 slice S2b: the full stats of the LAST async render
			//! to complete (whatever RenderAsync's submitted closure got back
			//! from RenderCore_, which the closure itself discards -- see
			//! that closure's `(void)r;`).  A caller that drove
			//! render{"async":true} -> render_status/render_wait and
			//! observed completion uses this to retrieve the SAME
			//! {ok,width,height,meanR,meanG,meanB,integrator,previewWidth,
			//! previewHeight,cameraOverridden,message} shape a synchronous
			//! Render() call returns directly -- the whole point being that
			//! an async-driven caller (the Mac GUI chat driver) can present
			//! an IDENTICAL result contract to its consumer (the LLM)
			//! regardless of which path actually ran the render.
			//!
			//! `renderJobId` MUST match the id of the render this cache
			//! holds, else `found` is false (the caller asked about a
			//! DIFFERENT job than the one most recently cached -- e.g. a
			//! stale id from an earlier submission, or a job that hasn't
			//! completed yet).  This is a strict identity check, not "any
			//! completed render": a caller polling job A must never be
			//! handed job B's stats just because B happened to finish more
			//! recently.
			//!
			//! Guarded by mAsyncCacheMutex (the SAME lock already used for
			//! mLastPng/mLastSink/mAsyncOutstandingJobId) -- populated by
			//! RenderAsync's submitted closure right where it currently
			//! discards `r`, read here by whatever thread polls after
			//! render_wait/render_status observes completion.
			struct AgentLastAsyncRenderResult
			{
				bool             found = false;   //!< true iff renderJobId matches the cached result's job
				AgentRenderResult result;         //!< meaningful only when found
			};
			AgentLastAsyncRenderResult LastAsyncRenderResult( std::uint64_t renderJobId ) const;

			//! Model-B F2 slice S2a: the wire-friendly status of an
			//! outstanding (or just-completed) RenderAsync call.  Mirrors
			//! SceneEditController::RenderJobLookup but does not leak the
			//! controller type into this header's public surface.
			struct AgentRenderJobStatus
			{
				bool   found  = false;   //!< false: unrecognized id (unknown, or a session-local/ODD id -- see GetRenderJobStatus's doc)
				bool   active = false;   //!< meaningful only when found
				//! Model-B F2 slice S3 ADDITIVE wire field: whether the job
				//! `found`/`active` describe was submitted PINNED.  Only
				//! meaningful when `found` is true (mirrors `active`'s own
				//! caveat); false for an Interactive-class job (no pinned
				//! concept) and for any not-found lookup.
				bool   pinned = false;
			};

			//! Model-B F2 slice S2a: poll the status of a render job id
			//! previously returned by RenderAsync (or by a synchronous
			//! Render() call that happened to route through the
			//! controller's coordinator -- see AgentRenderResult::
			//! renderJobId).  Controller-attached only; headless always
			//! reports `found=false` (there is no coordinator to ask).
			AgentRenderJobStatus RenderStatus( std::uint64_t renderJobId ) const;

			//! Model-B F2 slice S2a: block up to `timeoutMs` for the render
			//! job named by `renderJobId` to complete.  Returns true iff it
			//! was observed complete (or was already complete) within the
			//! timeout; false on timeout OR an unrecognized id (headless,
			//! or a session-local/ODD id -- see SceneEditController::
			//! WaitForRenderJob's doc).  `timeoutMs=0` polls once.
			bool RenderWait( std::uint64_t renderJobId, unsigned int timeoutMs ) const;

			//! The PNG bytes of the LAST successful Render (empty before the
			//! first render).  A convenience read of the cached result.
			std::vector<unsigned char> ReadImage() const;

			//! read_image maxEdge (F5 the cheap multi-angle observe loop):
			//! the LAST successful Render's image, DOWNSCALED (box filter, in
			//! linear space, aspect-preserving, never upscales) so its long
			//! edge is <= `maxEdge` -- clamped to [16,1024] by the caller
			//! (AgentRpc.cpp) before reaching here.  `maxEdge==0` is treated
			//! as "no bound" and returns the SAME bytes as ReadImage() (so
			//! read_image with no maxEdge stays byte-compatible).  Fills
			//! outWidth/outHeight with the dims of the returned image (0/0
			//! when nothing has been rendered yet).  Re-encodes from the
			//! cached full-resolution linear pixel buffer of the last
			//! render -- it does NOT re-render.
			std::vector<unsigned char> ReadImage( unsigned int maxEdge,
			                                      unsigned int& outWidth,
			                                      unsigned int& outHeight ) const;

			//! P3c (RENDER_MODES.md §7.8 ratified decision 3): READ-ONLY
			//! introspection of the N-up pane set, so an agent can reason
			//! about what the user is looking at.  `sourcePane` is the pane
			//! whose content ReadViewport currently returns (the r2-B
			//! honesty gap, closed structurally).  NO agent control of the
			//! pane set exists by decision.
			struct ViewportPaneInfo
			{
				bool        visible = false;
				std::string mode;         //!< registry wire name
				int         vantageKind = 0;   //!< 0 SceneCamera / 1 FreeFly / 2 NamedView
				std::string namedView;    //!< set when vantageKind==2
			};
			struct ViewportPanesInfo
			{
				int              layout = 0;      //!< 0 Single/1 TwoH/2 OnePlusTwo/3 Quad
				unsigned int     primary = 0;
				unsigned int     sourcePane = 0;  //!< whose pixels ReadViewport returns
				ViewportPaneInfo panes[4];
			};

			//! Toolkit slice 1 (read_viewport): fetch the CURRENT live
			//! interactive GUI viewport's pixels as PNG bytes -- the exact
			//! frame the user is looking at right now.  This is DISTINCT from
			//! ReadImage(): ReadImage returns the AGENT's own last headless
			//! render; ReadViewport returns the USER's live viewport (the
			//! attached SceneEditController's `mInteractiveFrameStore`).  It
			//! NEVER triggers a render -- it copies whatever the interactive
			//! render loop has most recently produced (the cheapest possible
			//! "observe").
			//!
			//! `outAvailable` is the structured outcome; `outReason` is one of
			//!   ""              (available == true)
			//!   "no_controller" (no live SceneEditController attached -- a
			//!                    headless session has no viewport at all)
			//!   "no_frame_yet"  (a controller is attached but the interactive
			//!                    render loop has not produced a frame yet).
			//! An unavailable result is a STRUCTURED, NON-error outcome (the
			//! returned byte vector is empty, outW/outH are 0).
			//!
			//! `maxEdge` (0 = native size, else clamped [16,1024] by the
			//! caller) downscales the copied frame exactly as ReadImage's
			//! maxEdge does (box filter, linear space, aspect-preserving,
			//! never upscales) -- reusing InMemoryRasterizerOutput's encode
			//! path on an already-coherent snapshot, no re-render.
			//! outW/outH report the dims of the returned PNG.
			//!
			//! Single-threaded-caller like the rest of this class's non-Render
			//! surface: the coherent, cross-thread-safe copy of the live store
			//! is done INSIDE SceneEditController::CopyInteractiveFrame (which
			//! locks against the render thread) -- no AgentSession-side lock is
			//! taken here (mController is read on the session's own call
			//! stack).
			std::vector<unsigned char> ReadViewport( unsigned int maxEdge,
			                                         unsigned int& outWidth,
			                                         unsigned int& outHeight,
			                                         bool& outAvailable,
			                                         std::string& outReason,
			                                         unsigned int& outSourcePane,
			                                         // user-review P1-3: the WHOLE pane set,
			                                         // snapshotted ATOMICALLY with the frame
			                                         // inside the parked window (not read back
			                                         // through the locking getters after the
			                                         // render resumed).  outHavePaneSet is false
			                                         // only when the parked read never ran.
			                                         ViewportPanesInfo& outPaneSet,
			                                         bool& outHavePaneSet ) const;

			// (user-review P1-3: the standalone DescribeViewportPanes getter was
			//  removed -- read_viewport now snapshots the pane set atomically with
			//  the frame; see ReadViewport's outPaneSet + SnapshotPaneSetForParkedRead.)

			//! Toolkit slice 3b: the structured result of query_object_at.
			//! `hit`/`name`/`kind`/`pixelX`/`pixelY`/`width`/`height`/`message`
			//! are the wire-visible fields (AgentRpc.cpp's query_object_at
			//! dispatch mirrors them 1:1); `outOfRange` is an INTERNAL-ONLY
			//! signal (never serialized) the RPC layer checks to decide
			//! between a structured success and a -32602 kInvalidParams error
			//! -- see QueryObjectAt's doc for the full out-of-range contract.
			struct AgentQueryObjectResult
			{
				//! True iff (x,y) fell OUTSIDE the EFFECTIVE film dims (the
				//! override dims when both width+height were supplied, else
				//! the Document's authored Film dims). AgentRpc.cpp maps this
				//! to a JSON-RPC -32602 error rather than a structured result
				//! -- every other field is left at its default in this case
				//! except `width`/`height` (the effective dims that WOULD have
				//! applied) and `message` (why).
				bool         outOfRange = false;
				//! True iff the probe pixel resolved to a registered, world-
				//! visible object (the SAME identity registry a mode:
				//! "objectmap" render's legend is built from). False is a
				//! STRUCTURED result, NOT a failure -- it means the pixel
				//! decoded to the reserved background colour (the ray missed
				//! every object), exactly like an objectmap render's
				//! background pixels.
				bool         hit = false;
				//! The LEGEND name of the hit object (see LegendEntry::name's
				//! doc -- same instance-array `grid[i,j]`-is-not-a-CST-chunk
				//! caveat applies identically here) -- "" when !hit.
				std::string  name;
				//! OPTIONALLY the hit object's manager "kind" -- ALWAYS "" as
				//! of this slice: neither IObject nor IObjectPriv exposes a
				//! cheap kind/type-name accessor (the scene-file chunk
				//! keyword lives on the CST Document, and resolving a
				//! possibly generator-synthesized legend name -- e.g.
				//! "grid[0,1]" -- back to a real chunk would need a
				//! Document scan per query, defeating the point of a CHEAP
				//! point-query). Wired for a future cheap accessor; honestly
				//! empty until one exists rather than paying that cost or
				//! guessing.
				std::string  kind;
				unsigned int pixelX = 0;
				unsigned int pixelY = 0;
				//! The EFFECTIVE film dims this query actually ran against
				//! (the override dims when supplied, else the Document's
				//! authored Film dims) -- always populated, even on
				//! `outOfRange` (reporting what the caller SHOULD target).
				unsigned int width = 0;
				unsigned int height = 0;
				std::string  message;
			};

			//! Toolkit slice 3b: query_object_at -- the cheap single-pixel
			//! companion to render's mode:"objectmap" (see
			//! AgentRenderTarget::ObjectMap's doc): "which WORLD-VISIBLE
			//! object is under pixel (x,y)?"
			//!
			//! IMPLEMENTATION CHOICE: this reuses the objectmap ephemeral
			//! pipeline WHOLESALE rather than a bespoke single-ray probe --
			//! it runs one full mode:"objectmap" Render() at the effective
			//! dims (composing width/height/camera EXACTLY as render's own
			//! overrides do, via the SAME AgentRenderParams path) and then
			//! reads ONE pixel back from the cached sink, matched against
			//! that render's legend by EXACT colorHex byte (the same byte-
			//! unique-by-construction contract the palette guarantees).  This
			//! costs one identity render (measured ~20ms at 256x256) rather
			//! than a second, bespoke GetCamera()->GenerateRay + caster code
			//! path -- shared machinery, shared invariants (exactness,
			//! byte-uniqueness, emissive visibility, isolation from the
			//! production FrameStore), zero new rendering code.  See
			//! AgentSession.cpp for the pixel-decode + legend-match mechanics.
			//!
			//! `x`/`y` are pixel coordinates in the EFFECTIVE film dims (the
			//! override dims when `params.width`/`params.height` are both
			//! set, else the Document's authored Film dims) -- resolved and
			//! range-checked BEFORE the render runs (a cheap, read-only Film
			//! query), so an out-of-range request never pays for the
			//! ephemeral render; `result.outOfRange` is set and NO render
			//! happens (AgentRpc.cpp maps this to -32602 kInvalidParams).
			//! `result.hit=false` (a STRUCTURED result, never a failure) means
			//! the probe ray missed every object (the reserved background
			//! colour) -- `result.outOfRange` stays false in that case.
			//!
			//! Document byte-identity: like every Render call, this NEVER
			//! mutates the retained CST Document (ReadDocument() is byte-
			//! identical before and after, camera/film overrides are
			//! captured-applied-restored exactly as render's own do).
			//!
			//! Works on a head with NO active production rasterizer (mirrors
			//! the quality:"draft" / mode:"objectmap" gate -- the underlying
			//! render this reuses never dereferences the production
			//! rasterizer at all).
			AgentQueryObjectResult QueryObjectAt( int x, int y,
			                                       const AgentQueryObjectParams& params = AgentQueryObjectParams() );

			//! Round-2 P1-1 test hook: override DrainAsyncRender_'s per-chunk
			//! wait duration for THIS session instance (default 0 = "use
			//! whatever chunkMs the caller/default passes").  Exists so a
			//! test can red-prove the UNBOUNDED cancel-then-wait LOOP (not
			//! just the single-cancel common case) with a wall-clock time
			//! the test suite can actually afford: a cancel-ignoring render
			//! genuinely longer than one small chunk (e.g. chunk=200ms,
			//! render=1s) proves the destructor blocks for the FULL
			//! duration and the escalating warning fires, without the test
			//! needing to wait out multiple 5000ms production-default
			//! chunks.  Production code never calls this.
			void ForTest_SetDrainChunkMs( unsigned int chunkMs ) { mDrainChunkMsForTest = chunkMs; }

			//! Invoked once after ReadViewport releases its parked controller
			//! snapshot but before it serializes the response.  Test-only: lets a
			//! regression test prove response metadata comes from that snapshot,
			//! rather than getters read after rendering resumes.  Set and consume
			//! on one thread before the ReadViewport/RPC call.
			void ForTest_SetReadViewportAfterParkHook( std::function<void()> hook )
			{
				mReadViewportAfterParkHookForTest = std::move( hook );
			}

			//! Round-2 P1-2 test hook: read mAsyncOutstandingJobId directly
			//! (under mAsyncCacheMutex, like every other access to this
			//! field).  Exists so a test can red-prove the publish-before-
			//! clear ordering fix: WITHOUT the fix, a trivially-fast async
			//! render's completion could clear this field to 0 BEFORE the
			//! submitting call ever published the real id, then have the
			//! (too-late) publish leave it PERMANENTLY STUCK at a nonzero,
			//! already-completed id -- observable here well after
			//! RenderAsync/RenderWait have both returned.  Production code
			//! never calls this.
			std::uint64_t ForTest_GetAsyncOutstandingJobId() const
			{
				std::lock_guard<std::mutex> cacheLk( mAsyncCacheMutex );
				return mAsyncOutstandingJobId;
			}

			//! Offscreen-isolation fix-round P1-A test hook: force
			//! RenderCore_'s doRenderWork to throw a std::runtime_error
			//! immediately before it calls mJob->Rasterize() -- AFTER any
			//! requested film-dims override and private-FrameStore install
			//! have already run, so the throw lands at exactly the point a
			//! real OIDN-class throw would.  Exists so a test can red-prove
			//! the FrameStoreIsolationGuard / RenderOverrideRestoreGuard
			//! construction-order fix (fsGuard must be constructed BEFORE
			//! restoreGuard so it destructs AFTER -- see AgentSession.cpp's
			//! doRenderWork comment) without depending on OIDN or any other
			//! real throw site actually firing. Default false (disabled);
			//! production code never calls this. Single-threaded like the
			//! rest of this class's non-Render test-hook surface -- set
			//! before the Render() call that will observe it, never
			//! concurrently.
			void ForTest_SetThrowBeforeRasterize( bool on ) { mThrowBeforeRasterizeForTest = on; }

			//! Toolkit slice 3a fix-round P2-1 test hook: exercise the objectmap
			//! identity-palette generator standalone (it is otherwise a file-
			//! static in AgentSession.cpp).  Fills `outBytes` with `count`
			//! byte triples and `outMinDistanceUsed` with the smallest L1
			//! separation the generator had to relax to (== 24 when it never
			//! degraded).  Static (references no member state) so a unit test
			//! can request a large count and assert byte-uniqueness + roundtrip
			//! + degrade-flag WITHOUT rendering a 2000-object scene.
			//! `forTestGoldenTries` (default = the production 4096) starves
			//! the golden walk when 0, so every id past the base list takes
			//! the EXHAUSTIVE last-resort scan -- the branch the closing-
			//! review P1 lived in (the scan starts at rgb=0, which IS the
			//! reserved background byte; the reserved set must be interned
			//! in takenKeys up front or that id paints as background).
			static void ForTest_BuildObjectMapPaletteBytes(
				std::size_t count,
				std::vector<std::array<unsigned char, 3> >& outBytes,
				unsigned int& outMinDistanceUsed,
				unsigned int forTestGoldenTries = 4096 );

			//! P2 test hook: exercise ResolveBeautyDisplayTransform_ directly
			//! against the currently-loaded head, without going through a full
			//! Render()/PNG-encode round trip.  Lets a unit test assert on the
			//! RESOLVED (exposureEV, displayTransform) pair itself -- e.g. that
			//! an HDR `file_rasterizeroutput` declared FIRST no longer shadows
			//! a LATER LDR output's declared `display_transform`/`exposure` --
			//! rather than only observing it indirectly through pixel bytes.
			//! External review P2 fix: also exposes the resolved output
			//! COLOUR SPACE (`outColorSpace`, `COLOR_SPACE` enum values, int to
			//! keep this header off Color.h) -- previously the in-memory PNG
			//! sink hardcoded eColorSpace_sRGB regardless of the scene's
			//! declared `file_rasterizeroutput` color_space.
			void ForTest_ResolveBeautyDisplayTransform( double& outExposureEV,
			                                            int& outDisplayTransform,
			                                            int& outColorSpace ) const
			{
				ResolveBeautyDisplayTransform_( outExposureEV, outDisplayTransform, outColorSpace );
			}

		private:
			AgentSession( IJobPriv* job, bool owns, AgentAuthority authority );
			AgentSession( const AgentSession& );             // deleted
			AgentSession& operator=( const AgentSession& );  // deleted

			//! The shared core of both Render overloads: legacy Render(int) is a
			//! thin forwarder that builds an all-absent AgentRenderParams (so
			//! it is BYTE-COMPATIBLE with the pre-preview-render behaviour).
			//!
			//! Model-B F2 slice S2a: `assumeParked` (default false, back-
			//! compat) is set to true ONLY by RenderAsync's submitted
			//! closure, which already runs INSIDE the controller's dedicated
			//! agent-render worker -- i.e. the cancel-and-park critical
			//! section is ALREADY held by the caller (the worker loop), so
			//! this call must skip its own routing (calling
			//! RunPreviewRenderParked / SubmitAgentRenderSync again from
			//! there would self-deadlock on the controller's non-recursive
			//! mMutex).  When true, this runs the render body directly
			//! (equivalent to the headless branch) and reports the
			//! `forcedJobId` the caller already minted, rather than minting
			//! or routing its own.
			AgentRenderResult RenderCore_( const AgentRenderParams& params,
			                                bool assumeParked = false,
			                                std::uint64_t forcedJobId = 0 );

			//! Resolve the effective BEAUTY display transform (exposure EV +
			//! tone-curve enum) the agent's in-memory PNG encode must apply so
			//! read_image / read_viewport / a compareToImage grading render
			//! reproduce what the CLI file-output pipeline (and the viewport a
			//! human watches) produce for the SAME head -- rather than a raw
			//! linear->sRGB image.  Mirrors the CLI defaults: an LDR
			//! `file_rasterizeroutput` chunk's declared `display_transform` +
			//! `exposureEV` when the head declares one, otherwise the LDR
			//! default (ACES filmic, 0 EV) -- an agent render is always an
			//! 8-bit PNG preview, so even an HDR-only or output-less head gets
			//! a viewable tone curve.  P2 fix: scans ALL `file_rasterizeroutput`
			//! chunks in document order and adopts the FIRST **LDR** one's
			//! declared curve+exposure -- an HDR output earlier in document
			//! order is SKIPPED (not stopped on), so a LATER LDR output's
			//! transform is still found; only when NO LDR output exists
			//! anywhere does the ACES/0EV default stand.  (Pre-fix, the first
			//! `file_rasterizeroutput` of ANY kind won unconditionally, so an
			//! HDR-output-first, LDR-output-second head wrongly fell back to
			//! the ACES default instead of the LDR output's own declared
			//! transform.)  The active camera's GetExposureCompensationEV() is
			//! stacked additively onto the exposure, matching
			//! FileRasterizerOutput's camera-EV stacking.
			//! `outDisplayTransform` uses the DISPLAY_TRANSFORM enum values
			//! (int to keep this header off the DisplayTransform.h dependency).
			//! Never applied to the OBJECTMAP identity sink (which must emit
			//! un-tonemapped per-pixel identity bytes).
			//! External review P2 fix: also resolves `outColorSpace` (`COLOR_SPACE`
			//! enum values, int to keep this header off Color.h) from the SAME
			//! LDR `file_rasterizeroutput`'s declared `color_space` -- default
			//! `eColorSpace_sRGB` (0) when absent/no LDR output, matching the
			//! descriptor's own default hint.  Exposure is now resolved via the
			//! CST v7 `expr(...)`-aware path (see AgentSession.cpp's
			//! ResolveParamNumeric) instead of a raw std::strtod, so an
			//! expr(...)-authored `exposure` no longer silently resolves to 0.
			void ResolveBeautyDisplayTransform_( double& outExposureEV,
			                                     int& outDisplayTransform,
			                                     int& outColorSpace ) const;

			//! Fix-round-1 P1-A / round-2 P1-1: cancel + wait, UNBOUNDED, for
			//! any OUTSTANDING async render submitted against the
			//! controller CURRENTLY attached (mController, read/captured
			//! before any detach).  Called from ~AgentSession (BEFORE any
			//! member is torn down) and from AttachController (BEFORE
			//! mController is reassigned or cleared) -- both are the two
			//! places this session's lifetime/identity can change out from
			//! under a still-running RenderAsync closure.
			//!
			//! Round-2 P1-1 fix: this used to wait a SINGLE bounded
			//! WaitForRenderJob(id, timeoutMs) and proceed regardless of
			//! whether that wait actually observed completion -- on a
			//! timeout (a render whose deep loop -- an MLT/VCM checkpoint
			//! gap, a wedged OIDN call -- does not poll the cancel flag
			//! often enough to abort within timeoutMs), ~AgentSession
			//! proceeded to free `this` while the worker could still be
			//! INSIDE the closure that holds that raw pointer: a genuine
			//! use-after-free gated only by a liveness timeout, not by
			//! actual completion.  Correctness must not be bounded by a
			//! timeout picked for the common case.
			//!
			//! Fixed shape: loop calling
			//! controller->CancelAgentRender_() (trips the SAME
			//! CancellableProgressCallback the render's doRenderWork
			//! installed -- see RenderCore_) then waiting in `chunkMs`-sized
			//! slices via controller->WaitForRenderJob(id, chunkMs) --
			//! returning the instant a slice observes completion, and
			//! otherwise RE-ISSUING the cancel and logging an ESCALATING
			//! eLog_Warning ("agent render ignoring cancellation for Ns;
			//! session teardown blocked") before waiting another slice.
			//! UNBOUNDED BY DESIGN: freeing the session while a worker
			//! closure is still touching it is strictly worse than a
			//! blocked teardown (a hung destructor is debuggable and
			//! visible in a stack trace; a UAF is neither).  In practice
			//! this loop runs at most once -- cancelling makes the render
			//! abort at its next progress/block-boundary check, which
			//! live-measured (AgentRenderAsyncTest.cpp's Stop()-during-a-
			//! render red-prove) takes 7-20ms, not tens of seconds -- the
			//! loop only iterates more than once against a render that is
			//! itself ignoring cancellation, which is exactly the case
			//! this fix exists for.  A no-op (returns immediately) when
			//! there is no outstanding job, or the controller was already
			//! null.  `chunkMs` is the size of EACH wait slice (not a
			//! total bound) -- the prior `timeoutMs` parameter is
			//! repurposed here rather than removed so the signature stays
			//! honest about what it now controls; default 5000ms mirrors
			//! the old default's order of magnitude for a caller that
			//! doesn't care, while keeping any individual log-escalation
			//! gap short.
			//!
			//! Round-2 P3-d scope note: this only drains a render submitted
			//! through RenderAsync -- i.e. one that went through
			//! SceneEditController::SubmitAgentRenderAsync and is tracked
			//! by mAsyncOutstandingJobId.  A render that instead went
			//! through Render(AgentRenderParams)'s OVERRIDE path
			//! (RunPreviewRenderParked, used when a film-dims or
			//! camera-pose override is requested) is OUTSIDE the
			//! mAgentRenderSlotMutex ticket queue by design -- it runs
			//! SYNCHRONOUSLY on the calling thread (the caller blocks for
			//! the render's duration inside that same call), so there is no
			//! separate worker-thread closure holding a raw `this` for this
			//! method to drain: by the time Render(AgentRenderParams)
			//! returns, that render is already fully complete.  Nothing to
			//! do here for that path; it is mentioned only so a future
			//! reader does not go looking for it in this method.
			void DrainAsyncRender_( unsigned int chunkMs = 5000 );

			IJobPriv* mJob;    //!< the wrapped Job (owned iff mOwnsJob)
			bool      mOwnsJob;

			//! Secure-MCP slice 5a: this session's construction-time
			//! authority.  const -- compiler-enforced immutability of the
			//! launch-time identity, mirroring AgentRpcDispatcher::mAutonomy's
			//! own const-member pattern (AgentRpc.h).  Defaults to Owner via
			//! every existing factory call site (LoadFromFile/WrapJob's
			//! default parameter) -- see those methods' docs.
			const AgentAuthority mAuthority;

			//! Secure-MCP slice 5c: see SessionLabel()/SetSessionLabel()'s
			//! doc.  "" until a caller (the GUI-hosted transport) sets one;
			//! single-threaded like the rest of this class's non-Render
			//! surface (set once at server-start time, before any request
			//! is served, and never mutated concurrently with a stage).
			std::string mSessionLabel;

			//! compare_to_reference: the HOST-registered reference set (see
			//! SetReferenceImages's doc).  Empty until a host registers at
			//! least one image; single-threaded like the rest of this
			//! class's non-Render surface (set once by the host before the
			//! session serves requests, never mutated concurrently with a
			//! CompareToReference call).
			std::vector<AgentReferenceImage> mReferenceImages;

			//! Facet 5 slice 1b: the attached LIVE controller (null = headless
			//! direct-Job mode).  BORROWED (never released); the caller owns it.
			//! When non-null, ProposePatch delegates to
			//! SceneEditController::ApplyAgentParamEdit.
			SceneEditController* mController = nullptr;

			std::vector<unsigned char> mLastPng;   //!< cached PNG bytes of the last Render (for ReadImage)

			//! read_image maxEdge: the LAST successful Render's in-memory sink,
			//! kept alive (addref'd) so ReadImage(maxEdge) can re-encode a
			//! downscaled PNG from the cached full-resolution linear pixels
			//! WITHOUT re-rendering.  Null before the first successful render.
			//! Owned (released in the destructor and whenever replaced).
			InMemoryRasterizerOutput* mLastSink = nullptr;

			//! Model-B F2 slice S2b: the full AgentRenderResult of the LAST
			//! async render to complete, plus the renderJobId it belongs to
			//! -- see LastAsyncRenderResult()'s doc.  {0, default-constructed
			//! AgentRenderResult} before the first async render completes.
			//! Guarded by mAsyncCacheMutex, same as mLastPng/mLastSink.
			std::uint64_t     mLastAsyncRenderResultJobId = 0;
			AgentRenderResult mLastAsyncRenderResult;

			//! Model-B F2 slice S1: SESSION-LOCAL render-id counter, used
			//! whenever this call does NOT route through a
			//! SceneEditController's coordinator-tracked RunPreviewRenderParked
			//! (no controller attached at all, OR a controller is attached
			//! but this particular call has no film/camera override to park
			//! for) -- see AgentRenderResult::renderJobId's doc for the full
			//! coordinator-tracked-vs-session-local honesty contract.
			//!
			//! Pre-S2 hardening: this counter and a SceneEditController's
			//! coordinator counter (SceneEditController::mNextRenderJobId)
			//! are two INDEPENDENT counters that both start small -- without
			//! a disjointness rule the same numeric id could name two
			//! different renders in one process (a session-local render and
			//! a coordinator-tracked render on an attached controller both
			//! minting "5", say), which a future Status(jobId)/Wait(jobId)
			//! (S2) would alias onto the wrong job.  Fix: the two spaces are
			//! disjoint by PARITY -- this counter mints ODD ids only,
			//! starting at 1 and incrementing by
			//! kSessionLocalRenderJobIdStride; SceneEditController's
			//! coordinator counter mints EVEN ids (starts at 2, same
			//! stride).  See SceneEditController::kControllerRenderJobIdStride's
			//! doc for why a tagged-high-bit scheme (id | (1ULL<<63)) was
			//! considered and rejected (it corrupts on the JSON wire path --
			//! Json.cpp SerializeNumber's exact-integer fast path only
			//! covers fabs(d) < 9.0e15, and 2^63 cannot even round-trip
			//! through a double exactly).  Single-threaded like the rest of
			//! this class -- no atomic needed.
			static constexpr std::uint64_t kSessionLocalRenderJobIdStride = 2;
			std::uint64_t mNextSessionLocalRenderJobId = 1;

			//! Model-B F2 slice S2a: guards mLastPng / mLastSink against a
			//! torn read/write race between the agent-render WORKER thread
			//! (which runs RenderCore_'s cache-population tail at the end of
			//! an async render, on the controller's dedicated worker
			//! thread) and whatever thread calls ReadImage() /
			//! ReadImage(maxEdge) while that async render is still in
			//! flight or just completing.  The synchronous Render() path
			//! (headless direct call, or SubmitAgentRenderSync/
			//! RunPreviewRenderParked which BLOCK the calling thread until
			//! the render is done) never races its OWN caller this way --
			//! this mutex exists specifically for the async case, where the
			//! submitting thread has already moved on by the time the
			//! worker populates the cache.  A plain std::mutex, not
			//! reentrant -- mirrors the controller's own mMutex discipline.
			mutable std::mutex mAsyncCacheMutex;

			//! Fix-round-1 P1-A: the renderJobId of the MOST RECENT
			//! RenderAsync submission that has not yet been observed
			//! complete -- 0 when there is none outstanding (initial state,
			//! or after DrainAsyncRender_ / a normal completion clears it).
			//! Guarded by mAsyncCacheMutex (the SAME lock already used for
			//! the async cache-population race -- this is touched from the
			//! SAME two thread contexts: the submitting thread, which sets
			//! it in RenderAsync, and the WORKER thread, which clears it at
			//! the end of the submitted closure).  Read by DrainAsyncRender_
			//! (~AgentSession / AttachController) to know whether there is
			//! anything to cancel-and-wait for.
			std::uint64_t mAsyncOutstandingJobId = 0;

			//! Round-2 P1-1 test hook -- see ForTest_SetDrainChunkMs's doc.
			//! 0 = disabled (DrainAsyncRender_ uses its own chunkMs argument
			//! unmodified); single-threaded like the rest of this class's
			//! non-Render surface -- set before the teardown/detach call
			//! that will read it, never concurrently.
			unsigned int mDrainChunkMsForTest = 0;

			//! See ForTest_SetReadViewportAfterParkHook.  Mutable because
			//! ReadViewport is a logically-const observe operation.
			mutable std::function<void()> mReadViewportAfterParkHookForTest;

			//! Offscreen-isolation fix-round P1-A test hook -- see
			//! ForTest_SetThrowBeforeRasterize's doc. false = disabled
			//! (doRenderWork calls mJob->Rasterize() normally); read/set
			//! only from doRenderWork and the test-hook setter above,
			//! single-threaded like mDrainChunkMsForTest.
			bool mThrowBeforeRasterizeForTest = false;
		};
	}
}

#endif
