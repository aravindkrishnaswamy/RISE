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
//      read_document                     -> {document:string}
//      read_schema  {keyword?}           -> the schema JSON (as a nested object)
//      validate     {text}               -> {diagnostics:[{severity,code,message,offset,length}]}
//      propose_patch{target,kind?,param,value} -> {applied,rawCode,message}
//      render       {samples?}           -> {ok,width,height,meanR,meanG,meanB,message}
//      read_image                        -> {png_base64:string,width,height}
//
//    Standard JSON-RPC error codes are honoured: -32700 parse error,
//    -32600 invalid request, -32601 method not found, -32602 invalid
//    params, -32603 internal error.  HandleLine NEVER throws or lets an
//    exception escape -- any thrown exception becomes a -32603 response.
//
//    Single-threaded + headless (slice 0c): no revision/DocumentId
//    concurrency gate, no auth token, no networking beyond the stdin/
//    stdout pipe the CLI wires -- those are slice 1+.
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
