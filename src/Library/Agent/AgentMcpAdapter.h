//////////////////////////////////////////////////////////////////////
//
//  AgentMcpAdapter.h - a PURE MCP (Model Context Protocol) envelope
//    adapter over AgentRpcDispatcher (Secure-MCP slice 1).
//
//    RISE already speaks its OWN JSON-RPC 2.0 dispatch for the 14 agent
//    verbs (AgentRpc.h's AgentRpcDispatcher::HandleLine).  This adapter
//    wraps that dispatcher and re-frames the SAME verb surface as MCP tools,
//    so an MCP client (Claude Code, or any other MCP host) can drive
//    RISE over `rise --agent-stdio --mcp <scene>`.  ZERO changes to
//    AgentRpcDispatcher / AgentSession / verb semantics -- this is
//    strictly an envelope translator:
//
//      MCP `initialize`             -> capability handshake (no dispatch)
//      MCP `notifications/initialized` -> NO response line (a true MCP
//                                         notification -- see HandleLine's
//                                         doc for why this differs from
//                                         AgentRpcDispatcher's respond-to-
//                                         everything convention)
//      MCP `ping`                    -> liveness check; MCP spec mandates
//                                        the server respond with an empty
//                                        object result ({}), handled here
//                                        directly (no dispatch).
//      MCP `tools/list`              -> every dispatcher verb, each as an MCP Tool
//                                        {name, description, inputSchema}
//      MCP `tools/call {name,args}`  -> builds an internal JSON-RPC
//                                        request line for the named verb,
//                                        runs it through the WRAPPED
//                                        AgentRpcDispatcher, and re-wraps
//                                        the result as an MCP
//                                        CallToolResult ({content:[...],
//                                        isError}).  A PROTOCOL failure
//                                        (unknown tool name, malformed
//                                        arguments shape) is a JSON-RPC
//                                        error response; a TOOL-EXECUTION
//                                        failure (the wrapped verb itself
//                                        returned a JSON-RPC error, e.g.
//                                        propose_patch on an unknown
//                                        entity) is a SUCCESS envelope
//                                        whose result carries
//                                        isError:true -- MCP's documented
//                                        split between the two failure
//                                        classes.
//
//    `read_image`'s PNG is additionally surfaced as a proper MCP `image`
//    content block ({type:"image", data:<base64>, mimeType:"image/png"})
//    alongside a `text` block carrying the metadata fields (byteLength/
//    width/height) -- so an MCP vision-capable client sees the rendered
//    frame inline, not just a base64 string it has to know to decode.
//
//    Protocol version: this adapter implements the 2025-03-26 MCP
//    revision (the last version before batch JSON-RPC requests were
//    removed from the spec -- and this adapter also does not support
//    batching, so this is the natural baseline).  `initialize` echoes
//    the client's requested protocolVersion when RECOGNIZED (currently
//    only "2025-03-26"); otherwise it reports this adapter's own
//    baseline version so the client can decide whether to proceed.
//
//    Ownership: constructed FROM a unique_ptr<AgentSession> (mirroring
//    AgentRpcDispatcher's own ownership style) and OWNS an internal
//    AgentRpcDispatcher built over that session -- so this adapter is a
//    drop-in alternative entry point next to AgentRpcDispatcher, not a
//    wrapper needing one constructed externally.
//
//    Secure-MCP slice 2 (headless autonomy policy): the OPTIONAL
//    `autonomy` constructor argument flows straight through to the
//    wrapped AgentRpcDispatcher -- this adapter performs NO independent
//    policy check.  A `tools/call` on one of the 5 mutating verbs
//    (propose_patch/propose_patches/insert_chunk/insert_chunks/
//    remove_chunk) under AgentAutonomy::Read surfaces the
//    wrapped dispatcher's kAutonomyRefused JSON-RPC error through the
//    SAME "tool-execution error" path every other wrapped-verb error
//    already takes: a SUCCESS envelope whose CallToolResult carries
//    isError:true and a text block with the refusal's structured error
//    JSON -- NOT a top-level JSON-RPC protocol error.  This matches MCP's
//    documented split (see the class doc above): the tool NAME and
//    argument SHAPE were valid, so this is not a protocol failure, but
//    invoking it was refused, so it is not a clean result either.
//    `tools/list` additionally ANNOTATES (never hides) the 5 mutating
//    tools' descriptions under Read with a leading
//    "[REFUSED under --agent-autonomy=read: ...]" note -- the tool stays
//    fully visible (with its real inputSchema) so a client can still
//    explain to its user what the tool WOULD do and why it is currently
//    unavailable, rather than silently disappearing from the list.
//
//    Single-threaded, matching AgentRpcDispatcher / AgentSession: one
//    call to HandleLine at a time, no concurrent use.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_AGENT_AGENTMCPADAPTER_
#define RISE_AGENT_AGENTMCPADAPTER_

#include "AgentRpc.h"   // AgentAutonomy (small header; the enum + dispatcher decl only)

#include <memory>
#include <string>

namespace RISE
{
	namespace Agent
	{
		class AgentSession;

		//! An MCP-over-stdio envelope adapter wrapping an AgentRpcDispatcher.
		//! See the file header above for the full method mapping.
		class AgentMcpAdapter
		{
		public:
			//! Take ownership of `session` (may be null -- exactly like
			//! AgentRpcDispatcher, a malformed launch still speaks valid
			//! MCP/JSON-RPC; every session-backed tool call reports a
			//! tool-execution error until a head exists).  `autonomy`
			//! defaults to Commit -- SAME back-compat rationale as
			//! AgentRpcDispatcher's own constructor (see AgentRpc.h): every
			//! existing call site keeps today's unrestricted behaviour
			//! without an edit.
			explicit AgentMcpAdapter( std::unique_ptr<AgentSession> session,
			                          AgentAutonomy autonomy = AgentAutonomy::Commit );
			~AgentMcpAdapter();

			//! Handle ONE line of MCP JSON-RPC input.  Returns the response
			//! line to write to stdout (no trailing newline), OR an EMPTY
			//! string when the input was a JSON-RPC NOTIFICATION (no `id`
			//! field at all -- e.g. `notifications/initialized`) per the
			//! MCP/JSON-RPC 2.0 notification contract: notifications get NO
			//! response.  The caller (the `--mcp` stdio loop) must skip
			//! writing a line when this returns empty.
			//!
			//! NEVER throws: malformed JSON -> -32700; a non-object /
			//! batch-array envelope -> a clean rejection (MCP 2025-03-26
			//! dropped batching); an unknown method -> -32601; a bad
			//! `tools/call` shape -> -32602; an internal failure / escaped
			//! exception -> -32603.
			std::string HandleLine( const std::string& mcpRequestLine );

		private:
			AgentMcpAdapter( const AgentMcpAdapter& );             // deleted
			AgentMcpAdapter& operator=( const AgentMcpAdapter& );  // deleted

			std::unique_ptr<AgentRpcDispatcher> mDispatcher;
			// Secure-MCP slice 2 hardening: const -- same rationale as
			// AgentRpcDispatcher::mAutonomy (see AgentRpc.h): copy-assign
			// is already deleted below, so this adds no new restriction,
			// only forecloses a future setter reintroducing mutability.
			const AgentAutonomy                 mAutonomy;
		};
	}
}

#endif
