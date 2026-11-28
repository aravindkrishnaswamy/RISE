//////////////////////////////////////////////////////////////////////
//
//  AgentRpc.h - JSON-RPC 2.0 dispatch layer over an AgentSession.
//
//    Single dispatch entry point (`HandleLine`: one JSON-RPC request line ->
//    one response line) shared by `rise --agent-stdio`, `AgentMcpAdapter`,
//    the in-app chat drivers, and in-process tests.  Single-threaded (not
//    thread-safe, matching AgentSession) and never throws: escaped exceptions
//    become -32603 responses.
//
//    Wire format (line-delimited JSON-RPC 2.0):
//      request : {"jsonrpc":"2.0","id":<id>,"method":<m>,"params":<obj>}
//      success : {"jsonrpc":"2.0","id":<id>,"result":<obj>}
//      error   : {"jsonrpc":"2.0","id":<id>,"error":{"code":<n>,"message":<s>,"data"?:<obj>}}
//
//    Error codes:
//      Standard  : -32700 parse error, -32600 invalid request,
//                  -32601 method not found, -32602 invalid params,
//                  -32603 internal error.
//      App-range : -32011 kAutonomyRefused (policy refusal with data:{verb,autonomy}),
//                  -32012 kProposalQueueFull (pending proposal cap reached),
//                  -32013 kMutatingRateLimitExceeded (HTTP transport only).
//
//    Method groups (mapped to AgentSession):
//      Read / inspection / validation:
//        read_document      -> {document, hasDocument, headVersion:{uuid,revision}}
//        read_schema        {keyword?, keywords?, category?} -> {schema:...};
//                           batch `keywords` preserves order (unmatched slots carry
//                           {keyword, error}), allowing at most 24 chunk keywords
//                           across `keywords` + `keyword`.  Stateless.
//        read_skill         {name?} -> index {skills:[...], note?} or {name, markdown};
//                           bare skill names only (no path separators / "..").  Stateless.
//        validate           {text?} -> {diagnostics:[...], validated:"head"|"text", headVersion?};
//                           validates candidate text (stateless) or retained head.
//        scene_inventory    {} -> visible object footprints, frame/world placement,
//                           parent/instancedFrom tree metadata.
//
//      Build protocol & visual targets (read-safe; document untouched):
//        file_build_plan    {elements:[{element,pieces,construction,outline,view?,note?},...]}
//                           -> enters pieces phase, rasterizes 256x256 silhouettes + composite PNG.
//        finish_element     {} -> closes active element, advances phase, returns isolate render.
//        reopen_element     {element} -> re-enters an element from any phase (incl. compose).
//        imagine_scene      {description} -> generates/stores whole-scene target PNG (capability-
//                           conditional; provider failure disarms requirement anti-stranding).
//
//      Mutations (optimistic concurrency via optional `baseHeadVersion:{uuid,revision}`):
//        propose_patch      / propose_patches: single or sequential-batch param edits (stale base
//                           is batch-fatal on propose_patches).
//        insert_chunk       / insert_chunks: single or sequential best-effort chunk inserts
//                           (rasterizer-allowlist policy refusal aborts batch atomically).
//        remove_chunk       / remove_chunks: single or all-or-nothing atomic batch removal.
//        insert_material_scaffold / insert_geometry_scaffold / replace_geometry_scaffold:
//                           deterministic named template graphs; Commit-only (excluded from
//                           IsProposeSafeVerb in AgentRpc.cpp).
//        Note: GUI chat drivers (macOS/Windows) may attach `"guiDriverRetry":{attempts,note}`
//              to wholly-unapplied retriable mutation refusals after driver retries.
//
//      Clean-room scene passes (Commit-only; single-turn minimal provider completions):
//        light_scene, populate_scene, environment_scene, frame_scene ({notes?}).
//
//      Rendering & visual observation:
//        render             {samples?, width?, height?, camera?, view?, quality?, mode?,
//                           xray?, light?, isolate?, target?, pinned?, imageMaxEdge?, async?}
//                           -> stats, optional legend/isolate/target/inline PNG, or async job handle.
//        render_status      / render_wait / render_cancel: async render lifecycle (live controller).
//        read_image         {maxEdge?, representation?} -> cached render PNG (beauty or perception
//                           atlas; omit maxEdge for native objectmap reads).
//        read_viewport      {maxEdge?} -> live GUI viewport frame (no re-render; structured
//                           available:false + reason: no_controller, no_frame_yet,
//                           editor_transaction_in_progress, render_in_progress,
//                           editor_interaction_finalize_failed, editor_shutting_down,
//                           editor_interaction_unrecoverable, or output_provenance_unavailable).
//        query_object_at    {x, y, camera?, width?, height?} -> single-pixel objectmap probe.
//        compare_to_reference {reference, camera?, visual?, samples?} -> RMSE, channel deltas,
//                           3x3 spatial grid, and optional [render|reference|diff] composite PNG.
//
//      Proposals & simulation:
//        list_proposals     {} -> staged/resolved proposals on attached controller (values/chunkText
//                           clipped to 16 KiB in listing only; empty when headless).
//        resolve_proposal   {proposalId, approve} -> approve/reject staged proposal (Owner + Commit).
//        sim_list / sim_status / sim_wait / sim_cache / sim_param_get -> read-safe sim queries.
//
//    Autonomy vs. Authority:
//      AgentAutonomy is a wire-level posture (never settable via RPC or scene text).
//      AgentRpc.cpp's IsReadSafeVerb / IsProposeSafeVerb are the single source of truth
//      for verb membership.  Read is deny-by-default; Propose allows read-safe + propose-safe
//      mutations through to AgentSession (pairing with AgentAuthority::External so mutations
//      stage); Commit dispatches unrestricted (pairing with AgentAuthority::Owner).
//      Class default is Commit for in-process back-compat; CLI binaries default to Read.
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
		struct AgentChunkResult;

		//! Test seams for clean-chunk-success classification and insert_chunks
		//! per-element serialization (production callers use AgentRpcDispatcher).
		bool        AgentRpcIsCleanChunkSuccess( const AgentChunkResult& cr );
		std::string AgentRpcBatchChunkResultJsonText( const AgentChunkResult& cr, const std::string& element );

		//! Launch-time wire autonomy posture (class default: Commit; CLI default: Read).
		enum class AgentAutonomy
		{
			Read,     //!< Deny-by-default: allows only IsReadSafeVerb -- read_document/read_schema/read_skill/validate/render/render_status/render_wait/render_cancel/sim_list/sim_status/sim_wait/sim_cache/sim_param_get/read_image/read_viewport/list_proposals/query_object_at/scene_inventory/compare_to_reference/file_build_plan/finish_element/reopen_element/imagine_scene/set_render_anchor; all other methods return kAutonomyRefused (-32011).
			Propose,  //!< Allows IsReadSafeVerb plus IsProposeSafeVerb (see AgentRpc.cpp) through to AgentSession; pairs with AgentAuthority::External so mutations stage. Excludes scaffolds and resolve_proposal.
			Commit    //!< Unrestricted dispatch; C++ constructor default for back-compat.
		};

		//! Single-threaded JSON-RPC 2.0 dispatcher owning an AgentSession.
		class AgentRpcDispatcher
		{
		public:
			//! Takes ownership of `session` (null -> session-backed calls return -32603).
			//! Defaults to Commit for in-process callers; CLI passes autonomy explicitly.
			explicit AgentRpcDispatcher( std::unique_ptr<AgentSession> session,
			                             AgentAutonomy autonomy = AgentAutonomy::Commit );
			~AgentRpcDispatcher();

			//! Dispatches one JSON-RPC request line and returns the response line (no
			//! trailing newline). Never throws; notifications still return a response line.
			std::string HandleLine( const std::string& jsonRpcRequest );

			//! Borrowed access to the wrapped session (may be null).
			AgentSession* Session() { return mSession.get(); }

			//! Current wire autonomy posture (used by wrappers such as AgentMcpAdapter).
			AgentAutonomy Autonomy() const { return mAutonomy; }

			//! Trusted in-process embedder hook (e.g. GUI autonomy selector) to change
			//! wire posture on the owning thread. Never RPC-reachable; does NOT change
			//! the wrapped session's AgentAuthority (Owner vs. External).
			void SetAutonomy( AgentAutonomy autonomy ) { mAutonomy = autonomy; }

		private:
			AgentRpcDispatcher( const AgentRpcDispatcher& );             // deleted
			AgentRpcDispatcher& operator=( const AgentRpcDispatcher& );  // deleted

			std::unique_ptr<AgentSession> mSession;
			AgentAutonomy                  mAutonomy;
		};
	}
}

#endif
