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

		//! A JSON-RPC 2.0 dispatcher holding an AgentSession.  Owns the
		//! session (constructed from one).  NOT thread-safe (slice 0c is
		//! single-threaded, matching AgentSession).
		class AgentRpcDispatcher
		{
		public:
			//! Take ownership of `session` (may be null -- then every
			//! session-backed method returns a -32603 "no session" error, so
			//! a malformed launch still speaks valid JSON-RPC).
			explicit AgentRpcDispatcher( std::unique_ptr<AgentSession> session );
			~AgentRpcDispatcher();

			//! Handle ONE JSON-RPC request line, returning the response line
			//! (no trailing newline -- the caller frames lines).  NEVER
			//! throws: a malformed line -> -32700, an unknown method ->
			//! -32601, a bad params shape -> -32602, an internal failure /
			//! escaped exception -> -32603.  A JSON-RPC NOTIFICATION (a
			//! request with no `id`) still gets a response line in slice 0c
			//! (the stdio transport is strictly request/response; true
			//! fire-and-forget notifications are not part of this set).
			std::string HandleLine( const std::string& jsonRpcRequest );

			//! The wrapped session (for a host that wants direct access; the
			//! CLI does not need it).  May be null.
			AgentSession* Session() { return mSession.get(); }

		private:
			AgentRpcDispatcher( const AgentRpcDispatcher& );             // deleted
			AgentRpcDispatcher& operator=( const AgentRpcDispatcher& );  // deleted

			std::unique_ptr<AgentSession> mSession;
		};
	}
}

#endif
