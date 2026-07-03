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
//      render       {samples?}           -> {ok,width,height,meanR,meanG,meanB,message}
//      read_image                        -> {png_base64:string, byteLength:number}
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
