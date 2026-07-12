//////////////////////////////////////////////////////////////////////
//
//  AgentRpc.h - the JSON-RPC 2.0 dispatch layer over an AgentSession
//    (Facet 5, the agentic surface -- slice 0c: the transport).
//
//    ONE place the read-eval-print loop is realized: `HandleLine` takes a
//    single JSON-RPC request line and returns the JSON-RPC response line.
//    Both the `rise --agent-stdio` CLI and the end-to-end test drive this
//    same dispatcher (the CLI just wraps it in a stdin/stdout loop) -- so
//    the loop is tested in-process with NO subprocess and NO LLM.
//
//    JSON-RPC 2.0 (line-delimited, one request -> one response):
//      request : {"jsonrpc":"2.0","id":<id>,"method":<m>,"params":<obj>}
//      success : {"jsonrpc":"2.0","id":<id>,"result":<obj>}
//      error   : {"jsonrpc":"2.0","id":<id>,"error":{"code":<n>,"message":<s>}}
//
//    Methods (mapped to AgentSession):
//      read_document                     -> {document:string, hasDocument:bool, headVersion:{uuid,revision}}
//      read_schema  {keyword?}           -> the schema JSON (as a nested object)
//      read_skill   {name?}              -> no name: {skills:[{name,title,hook},...], note?:string}
//                                           (the INDEX; `note` appears only when the
//                                            skills ROOT directory is missing, telling
//                                            a miswired root from a present-but-empty
//                                            one); a name: {name, markdown}
//                                           (Facet 5 slice S1: progressive-disclosure
//                                            scene-authoring skills; STATELESS like
//                                            read_schema.  Name must be BARE -- '/',
//                                            '\\', ".." -> -32602; unknown or not in
//                                            the index (the fetchable set IS the
//                                            listed set) -> -32602.)
//      validate     {text}               -> {diagnostics:[{severity,code,message,offset,length}]}
//      propose_patch{target,kind?,param,value,baseHeadVersion?:{uuid,revision}}
//                                        -> {applied,rawCode,status,retriable,headVersion:{uuid,revision},message}
//                                           (retriable=true marks the ONE transient
//                                            reject -- an open editor transaction in
//                                            LIVE mode; resubmit the same patch later.
//                                            Permanent rejects are false; a "conflict"
//                                            is retriable-by-protocol via re-read.)
//      insert_chunk {chunkText,baseHeadVersion?:{uuid,revision}}
//                                        -> {applied,rawCode,status,retriable,headVersion,message,name,kind}
//                                           (Model-B F5 slice S2: ADD one complete chunk
//                                            -- a `keyword { ... }` block -- to the head
//                                            and REALIZE it via a dry-run-guarded full
//                                            re-derive.  Same result gating as
//                                            propose_patch, plus the parsed chunk's
//                                            `kind` (keyword) + `name` echo.  Exactly
//                                            ONE chunk per call; headers/directives/
//                                            multi-chunk text and duplicate (kind,name)
//                                            are rejected with a specific message.)
//      remove_chunk {target,kind?,baseHeadVersion?}
//                                        -> {applied,rawCode,status,retriable,headVersion,message,name,kind}
//                                           (Model-B F5 slice S2: REMOVE the chunk
//                                            resolved by bare name (+ optional kind
//                                            narrowing, same rules as propose_patch)
//                                            via the trivia-preserving erase.  Unknown
//                                            -> rejected; ambiguous -> rejected with a
//                                            disambiguation hint; a still-referenced
//                                            target fails the dry-run -> rejected with
//                                            the diagnostic, head byte-identical.)
//      render       {samples?,width?,height?,camera?,pinned?}
//                                        -> {ok,width,height,meanR,meanG,meanB,integrator,
//                                            previewWidth,previewHeight,cameraOverridden,message,
//                                            renderJobId,samplesOverridden,effectiveSamples}
//                                           (`integrator` is the ACTIVE rasterizer's
//                                            registered type name = its scene-file
//                                            chunk keyword, e.g.
//                                            "pathtracing_pel_rasterizer" -- empty
//                                            when no rasterizer is active.  Lets an
//                                            agent OBSERVE which integrator a
//                                            rasterizer insert_chunk activated.
//                                            Facet 5 preview-render (the cheap
//                                            multi-angle observe loop): `width`/
//                                            `height` (clamped [16,512], must be
//                                            paired) are a TRANSIENT film-dims
//                                            override -- never touches the
//                                            Document (Job::SetFilm is a LIVE-only
//                                            Scene mutation, captured + restored
//                                            around the render).  `camera`
//                                            {location,lookat,up?,fov?} is an
//                                            EPHEMERAL override of the ACTIVE
//                                            camera's pose for this ONE render --
//                                            captured via CameraIntrospection
//                                            before the override and restored
//                                            after, so the camera's properties are
//                                            byte-for-byte the same before and
//                                            after every render call.  LIVE mode:
//                                            the override window is run under
//                                            SceneEditController::RunPreviewRenderParked
//                                            so it cannot race the interactive
//                                            render thread's own Film/camera
//                                            swap; when an editor transaction is
//                                            open the override is refused and the
//                                            render falls back to un-overridden
//                                            (reported in `message`).
//                                            Model-B F2 slice S3
//                                            (EffectiveRenderConfig): `samples`
//                                            (clamped [1,65536]; -1 or absent =
//                                            no override) is now HONORED for
//                                            rasterizers that opt in to
//                                            IRasterizer::SetSampleCountOverride
//                                            (the pixel-based family: PT,
//                                            spectral PT, BDPT, VCM) via a
//                                            capture/apply/restore window --
//                                            NEVER mutates the retained CST
//                                            Document.  `samplesOverridden`
//                                            (additive) reports whether the
//                                            override actually took;
//                                            `effectiveSamples` (additive) is
//                                            the SPP this render actually ran
//                                            at.  An unsupported rasterizer
//                                            (MLT, photon-map-only, Auto's
//                                            outer wrapper) reports
//                                            samplesOverridden:false with a
//                                            note appended to `message` --
//                                            never silently ignored.
//                                            Model-B F2 slice S3
//                                            (pinned-vs-preview): `pinned`
//                                            (default false = today's PREVIEW
//                                            semantics) marks this render as
//                                            PINNED -- see
//                                            SceneEditController::
//                                            SubmitAgentRenderAsync's `pinned`
//                                            doc for the single-slot
//                                            supersession-refusal policy this
//                                            enables (a pinned render in
//                                            flight refuses ANY new
//                                            submission, async or sync,
//                                            pinned or not, until it
//                                            completes; render_cancel / a
//                                            controller Stop() still cancel
//                                            it -- pinned guards against
//                                            supersession, not against an
//                                            explicit cancel/teardown).)
//      read_image   {maxEdge?}           -> {png_base64:string, byteLength:number,
//                                            width:number, height:number}
//                                           (Facet 5 preview-render: `maxEdge`
//                                            (clamped [16,1024]) downscales the
//                                            cached image -- box filter, aspect-
//                                            preserving, never upscales -- before
//                                            base64-encoding; no re-render.
//                                            `width`/`height` report the dims of
//                                            the returned image.)
//      list_proposals {}                 -> {proposals:[{id,kind,target,entityKind,
//                                            param,value,chunkText,truncated,
//                                            baseVersion:{uuid,revision},
//                                            sessionLabel,status},...]}
//                                           (Secure-MCP slice 5b: the queue of every
//                                            AgentProposal staged on the ATTACHED
//                                            controller -- pending AND resolved
//                                            (resolved entries stay for audit, up
//                                            to the slice 6 storage bound below).
//                                            READ-SAFE: available under ANY autonomy
//                                            posture, including Read -- listing is a
//                                            read of the same scene the caller is
//                                            already editing, not a mutation.
//                                            CONTROLLER-ATTACHED ONLY: a headless
//                                            session (no live controller) reports an
//                                            empty array, not an error -- "no queue
//                                            to list" and "empty queue" look the
//                                            same to a caller that just wants a
//                                            list.  No session at all -> the usual
//                                            "no session loaded" internal error.
//                                            Secure-MCP slice 6 (additive): `value`
//                                            and `chunkText` are each clipped to 16
//                                            KiB in THIS LISTING ONLY (the stored
//                                            proposal is untouched -- approving it
//                                            still applies the FULL text); `truncated`
//                                            is true iff this entry's listing was
//                                            actually clipped.)
//      resolve_proposal {proposalId,approve:bool}
//                                        -> {resolved:bool,status:string,headVersion,
//                                            message}
//                                           (Secure-MCP slice 5b: approve or reject
//                                            a staged proposal.  OWNER-ONLY: routes
//                                            to AgentSession::ResolveProposal, which
//                                            refuses (resolved=false) when THIS
//                                            session's authority is not Owner --
//                                            an External session may not resolve
//                                            ANY proposal, including one it staged
//                                            itself (see AgentSession::
//                                            ResolveProposal's doc).  `status` is
//                                            "applied"/"rejected"/"conflict" on a
//                                            real resolve (approve re-checks the
//                                            proposal's staged baseVersion against
//                                            the controller's CURRENT head under
//                                            the same lock -- a proposal staged
//                                            against a head that has since moved
//                                            resolves to "conflict", not applied);
//                                            empty when `resolved` is false.
//                                            `headVersion` is the head AFTER the
//                                            call on a real resolve (whatever the
//                                            underlying ApplyAgent* replay reports
//                                            on approve; the CURRENT head,
//                                            unmoved, on reject) and {0,0} when
//                                            `resolved` is false (an Owner-only
//                                            refusal or an unknown id has no head
//                                            to report -- there was nothing to
//                                            apply). NOT read-safe (refused under
//                                            Read even for an Owner session --
//                                            resolving is a mutating-adjacent
//                                            verb; see the autonomy-posture note
//                                            below for why this is still coherent
//                                            with "Owner isn't autonomy-gated the
//                                            same way": an Owner's IN-PROCESS
//                                            dispatcher is constructed at Commit,
//                                            so this gate is a non-issue for the
//                                            real GUI topology.)
//
//    Model-B F2 slice S2a/S2b (async render): render{"async":true} submits to
//    the ATTACHED controller's dedicated agent-render worker and returns
//    IMMEDIATELY with {renderJobId,status:"submitted"|"refused",message,pinned}
//    instead of blocking for the render's duration -- LIVE (in-app GUI)
//    controllers only; `rise --agent-stdio` is headless and refuses async
//    cleanly.  render_status {renderJobId} -> {found,active,pinned} polls a job
//    id (from either an async or a synchronous render); render_wait
//    {renderJobId,timeoutMs?} -> {completed,found,active,pinned,result?} blocks up
//    to timeoutMs (default 5000, clamped [0,60000]) for it to finish.
//    `result` (slice S2b, additive) carries the SAME shape a synchronous
//    render returns ({ok,width,height,meanR,...}) when this session cached
//    that renderJobId's stats -- i.e. it was submitted via
//    render{"async":true} on THIS session and has now completed; absent
//    otherwise (a different session's job, a synchronous job, or not yet
//    complete).
//    render_cancel {renderJobId?} -> {cancelled,found,active} trips the
//    cancel signal for the outstanding async render WITHOUT blocking for it
//    to actually stop (poll render_status/render_wait afterward to observe
//    completion) -- headless sessions refuse it (no controller, nothing to
//    cancel) exactly like render{"async":true} refuses submission.  Cancels
//    a PINNED render exactly like a preview one (slice S3: pinned protects
//    against SILENT SUPERSESSION by a later submission, not against an
//    explicit cancel or a controller Stop()/teardown drain -- both of
//    those remain unconditional).
//
//    Facet 5 slice 1a (optimistic concurrency): read_document now carries the
//    retained CST head's (uuid,revision) identity; propose_patch accepts an
//    OPTIONAL baseHeadVersion precondition and, if it is stale (!= the current
//    head), returns status="conflict" WITHOUT mutating -- so a stale agent edit
//    is rejected instead of clobbering a newer head.  Its result always carries
//    the post-call headVersion.  uuid/revision are monotonic counters starting
//    at 1, well under 2^53, so they are emitted as exactly-representable JSON
//    numbers.
//
//    Standard JSON-RPC error codes are honoured: -32700 parse error,
//    -32600 invalid request, -32601 method not found, -32602 invalid
//    params, -32603 internal error.  HandleLine NEVER throws or lets an
//    exception escape -- any thrown exception becomes a -32603 response.
//
//    Single-threaded + headless: the (uuid,revision) baseHeadVersion
//    optimistic-concurrency gate landed in slice 1a (above); an auth token
//    and networking beyond the stdin/stdout pipe the CLI wires remain slice 1b+.
//
//    Secure-MCP slice 2 (headless autonomy policy): AgentAutonomy is a
//    LAUNCH-TIME-ONLY posture -- never settable by the model, a scene
//    file, or any request parameter.  Post-hardening, the choke point is
//    DENY-BY-DEFAULT: `Read` allows ONLY the 9-verb read-safe allowlist
//    (read_document, read_schema, read_skill, validate, render,
//    render_status, render_wait, render_cancel, read_image -- IsReadSafeVerb
//    in AgentRpc.cpp) and refuses EVERYTHING else, including the 3 known-
//    mutating verbs (propose_patch, insert_chunk, remove_chunk), any
//    unrecognized/typo'd method name, and any FUTURE verb added to the
//    dispatch below without also being added to the read-safe list.  This
//    is a deliberate polarity flip from the pre-hardening design (an
//    allow-list of the 3 mutating verb NAMES, checked first) which was
//    FAIL-OPEN: a new mutating verb #13 would have been silently permitted
//    under Read until someone remembered to blacklist it.  One
//    consequence worth calling out: an unrecognized method under Read now
//    surfaces as kAutonomyRefused rather than kMethodNotFound (a Commit
//    dispatcher, or a recognized-but-unsafe verb under Read, still
//    distinguishes the two) -- consistent with "deny unless proven safe"
//    rather than leaking method-existence to a read-only caller.  The
//    refusal itself carries a distinct JSON-RPC error code
//    (kAutonomyRefused == -32011, an unused app-range code alongside the
//    standard set above) + a structured `data` field {verb,
//    autonomy:"read"} -- deliberately NOT the "rejected"/"conflict" status
//    shape propose_patch's own result uses for a scene-state outcome,
//    because a policy refusal must never be mistaken for a retriable scene
//    conflict.  render (and every other verb on the read-safe allowlist)
//    is DELIBERATELY allowed under `Read` -- it does not mutate the
//    retained Document, and rendering is the core value of a read-only
//
//    Secure-MCP slice 6 (limits hardening): two SIBLING app-range codes in
//    the same -320xx family as kAutonomyRefused, each a distinct
//    RESOURCE/BACKPRESSURE refusal rather than a policy or scene-state
//    outcome: kProposalQueueFull (-32012, AgentRpc.cpp) when
//    propose_patch/insert_chunk/remove_chunk would stage past
//    SceneEditController::kMaxPendingProposals; kMutatingRateLimitExceeded
//    (-32013, AgentLoopbackHttpServer.cpp -- enforced at the HTTP
//    TRANSPORT layer, before a request reaches this dispatcher, so it is
//    deliberately NOT reachable via `rise --agent-stdio`) when a mutating
//    verb exceeds the fixed-window call-rate cap.
//    observer session.  `Commit` is today's behaviour: no refusal, every
//    verb dispatches as before.
//
//    Secure-MCP slice 5b (`Propose` autonomy): a THIRD posture, between
//    Read and Commit.  Under `Propose`, the read-safe 9-verb allowlist
//    still passes (same as Read) PLUS list_proposals (also read-safe under
//    every posture, see below) PLUS the 3 mutating verbs (propose_patch,
//    insert_chunk, remove_chunk) are let THROUGH to the wrapped
//    AgentSession rather than refused at this dispatcher's choke point --
//    see IsProposeSafeVerb in AgentRpc.cpp.  Crucially, this dispatcher-
//    layer gate does NOT itself decide whether a mutating verb commits or
//    stages: that is AgentSession::mAuthority's job (AgentSession.h's
//    Owner/External split), one layer down.  `Propose` autonomy is the
//    WIRE POSTURE that pairs with an External-authority session (see
//    commandconsole.cpp's RunAgentHttp/RunAgentMcpStdio, which construct
//    AgentAuthority::External exactly when --agent-autonomy=propose is
//    passed): the mutating verb reaches AgentSession::ProposePatch/
//    InsertChunk/RemoveChunk, and THAT method's own authority check stages
//    it (a live controller attached) or refuses it (headless, no
//    controller -- see AgentSession.h's doc).  So the three-way mapping is:
//    `Read` autonomy pairs with Owner OR External authority (nothing
//    mutating-shaped reaches the session either way, so authority is moot);
//    `Propose` autonomy pairs with External authority (mutating verbs reach
//    the session, which STAGES rather than commits); `Commit` autonomy
//    pairs with Owner authority (mutating verbs reach the session, which
//    COMMITS directly).  These two knobs are chosen TOGETHER at the SAME
//    construction site (never independently) -- Owner authority + Propose-
//    shaped wire gating, or External authority + Commit-shaped wire gating,
//    are combinations no shipped construction site produces, since either
//    would be incoherent (an Owner staging its own edits for itself to
//    approve, or an External session's edits reaching the dispatcher with
//    no staging gate at the wire layer at all).
//
//    resolve_proposal is DELIBERATELY left OFF the Propose-safe set --
//    reachable ONLY under `Commit` (the posture the in-process Owner
//    dispatcher runs at).  This is intentional defense in depth, not a
//    scope gap: AgentSession::ResolveProposal ALSO refuses outright for any
//    non-Owner-authority session (see that method's doc), so an External
//    session calling resolve_proposal is refused twice over regardless of
//    which autonomy posture its transport happens to run at -- the
//    dispatcher-layer omission just means the refusal for an External/
//    Propose caller is the CHEAPER kAutonomyRefused (-32011) policy
//    refusal rather than a round-trip into AgentSession only to be told the
//    same "Owner-only" no.
//
//    Class default vs. binary default (READ THIS BEFORE CHANGING EITHER):
//    the C++ DEFAULT for a dispatcher constructed WITHOUT the autonomy
//    argument is `Commit`, for back-compat with every existing in-process
//    construction -- the GUI's live agent dispatcher
//    (RISEViewportBridge.mm's `new AgentRpcDispatcher(std::move(session))`)
//    and every pre-slice-2 test construct this way and MUST keep today's
//    unrestricted behaviour without an edit.  The `rise --agent-stdio` CLI
//    binary's OWN default is the INVERSE -- `read` -- unless the caller
//    explicitly passes `--agent-autonomy=commit`; the CLI passes the enum
//    value explicitly either way (see commandconsole.cpp), it never relies
//    on the class default.  So: "the BINARY defaults to read; the CLASS
//    defaults to commit-for-back-compat."
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_AGENT_AGENTRPC_
#define RISE_AGENT_AGENTRPC_

#include <memory>
#include <string>

namespace RISE
{
	namespace Agent
	{
		class AgentSession;

		//! Secure-MCP slice 2: the launch-time autonomy posture a headless
		//! (or embedding) session runs with.  See the file header above for
		//! the full class-default-vs-binary-default rationale.
		enum class AgentAutonomy
		{
			Read,     //!< DENY-BY-DEFAULT: only the read-safe ALLOWLIST (IsReadSafeVerb -- read_document/read_schema/read_skill/validate/render/render_status/render_wait/render_cancel/read_image/list_proposals) dispatches; every other method, including the 3 known-mutating verbs (propose_patch/insert_chunk/remove_chunk), resolve_proposal, and any future unclassified verb, is refused.
			//! Secure-MCP slice 5b: the read-safe allowlist PLUS the 3 mutating
			//! verbs (propose_patch/insert_chunk/remove_chunk) dispatch -- but
			//! dispatching only reaches AgentSession, whose OWN Owner/External
			//! authority decides staging-vs-commit (see the file header's
			//! Propose-autonomy doc).  resolve_proposal is deliberately NOT
			//! included -- it stays Commit-only (see the file header).  Pairs
			//! with an External-authority session at the construction site
			//! (commandconsole.cpp); never used with an Owner-authority session.
			Propose,
			Commit    //!< today's behaviour: every verb dispatches unrestricted. The C++ constructor DEFAULT (back-compat).
		};

		//! A JSON-RPC 2.0 dispatcher holding an AgentSession.  Owns the
		//! session (constructed from one).  NOT thread-safe (slice 0c is
		//! single-threaded, matching AgentSession).
		class AgentRpcDispatcher
		{
		public:
			//! Take ownership of `session` (may be null -- then every
			//! session-backed method returns a -32603 "no session" error, so
			//! a malformed launch still speaks valid JSON-RPC).
			//! `autonomy` defaults to Commit -- see the file header's
			//! class-default-vs-binary-default note: every EXISTING call
			//! site (the GUI's live dispatcher, every pre-slice-2 test)
			//! constructs without this argument and must keep today's
			//! unrestricted behaviour verbatim.  `rise --agent-stdio` passes
			//! the value explicitly (defaulting to Read at the CLI layer).
			explicit AgentRpcDispatcher( std::unique_ptr<AgentSession> session,
			                             AgentAutonomy autonomy = AgentAutonomy::Commit );
			~AgentRpcDispatcher();

			//! Handle ONE JSON-RPC request line, returning the response line
			//! (no trailing newline -- the caller frames lines).  NEVER
			//! throws: a malformed line -> -32700, an unknown method under
			//! Commit (or any recognized-but-unsafe method under Read that
			//! still doesn't exist) -> -32601, a bad params shape -> -32602,
			//! an internal failure / escaped exception -> -32603.  A
			//! JSON-RPC NOTIFICATION (a request with no `id`) still gets a
			//! response line in slice 0c (the stdio transport is strictly
			//! request/response; true fire-and-forget notifications are not
			//! part of this set).  Secure-MCP slice 2 hardening: under
			//! AgentAutonomy::Read, any method NOT on the 9-verb read-safe
			//! allowlist (IsReadSafeVerb) -- the 3 mutating verbs, an
			//! unrecognized method, or any future verb not yet classified --
			//! returns kAutonomyRefused (-32011) instead of dispatching; see
			//! the file header's policy-refusal doc.
			std::string HandleLine( const std::string& jsonRpcRequest );

			//! The wrapped session (for a host that wants direct access; the
			//! CLI does not need it).  May be null.
			AgentSession* Session() { return mSession.get(); }

			//! The autonomy posture this dispatcher was constructed with
			//! (Secure-MCP slice 2). Exposed so a wrapping adapter (e.g.
			//! AgentMcpAdapter) can annotate tools/list without duplicating
			//! the policy.
			AgentAutonomy Autonomy() const { return mAutonomy; }

			//! GUI in-app autonomy selector (2026-07): re-point this
			//! dispatcher's posture after construction.  The file header's
			//! "never settable by the model, a scene file, or any request
			//! parameter" invariant is about the WIRE surface -- no RPC verb
			//! reaches this method, so a remote/embedded caller can never
			//! trigger it.  This is a SEPARATE, narrower door: a trusted
			//! FIRST-PARTY embedder (the Mac GUI's own Swift composer, in
			//! reaction to an explicit user click on the Read/Propose/Apply
			//! chips -- see RISEViewportBridge's -setAgentAutonomyLevel:)
			//! choosing which posture ITS OWN local dispatcher instance runs
			//! with next.  Same single-threaded-caller contract as the rest
			//! of this class (AgentSession's, no lock): call only from the
			//! thread that owns this dispatcher (the GUI's case: the main /
			//! UI thread, same thread HandleLine itself must be called from).
			//! Does NOT touch the wrapped AgentSession's own AgentAuthority
			//! (Owner vs External) -- that stays wire-immutable at the
			//! session layer (AgentSession.h); an Owner-authority session's
			//! mutating verbs keep committing directly no matter what this
			//! dispatcher's autonomy is set to (see AgentSession::ProposePatch's
			//! authority gate) -- so flipping THIS dispatcher alone to
			//! Propose over an Owner-authority session does not make it
			//! stage.  RISEViewportBridge accordingly keeps a SEPARATE
			//! External-authority dispatcher instance for its Propose level
			//! rather than calling SetAutonomy(Propose) on an Owner one; see
			//! that file's doc for the full routing.
			void SetAutonomy( AgentAutonomy autonomy ) { mAutonomy = autonomy; }

		private:
			AgentRpcDispatcher( const AgentRpcDispatcher& );             // deleted
			AgentRpcDispatcher& operator=( const AgentRpcDispatcher& );  // deleted

			std::unique_ptr<AgentSession> mSession;
			// Secure-MCP slice 2 hardening (2026-07 update): NOT const --
			// see SetAutonomy above.  The original const was compiler-
			// enforced immutability against a WIRE-reachable re-posture;
			// that property is preserved (SetAutonomy is not, and can never
			// become, an RPC-dispatched verb -- HandleLine's method switch
			// has no path to it). What changed is a narrow, deliberate door
			// for a trusted embedder to re-point its OWN local instance.
			AgentAutonomy                  mAutonomy;
		};
	}
}

#endif
