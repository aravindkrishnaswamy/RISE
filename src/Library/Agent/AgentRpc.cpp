//////////////////////////////////////////////////////////////////////
//
//  AgentRpc.cpp - the JSON-RPC 2.0 dispatch layer (see AgentRpc.h).
//
//  HandleLine is the single realization of the read-eval-print loop.  It
//  is written to be TOTAL: it parses the request, validates the JSON-RPC
//  envelope, dispatches to the mapped AgentSession call, and serializes a
//  response -- and it wraps the whole body in a try/catch so ANY escaped
//  exception (bad_alloc, a std::exception from a deep call) becomes a
//  -32603 internal-error response rather than crashing the loop.
//
//////////////////////////////////////////////////////////////////////

#include "AgentRpc.h"

#include "AgentSession.h"
#include "AgentDiagnostic.h"
#include "Base64.h"
#include "Json.h"
#include "SchemaGen.h"

#include <cmath>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace RISE
{
	namespace Agent
	{
		namespace
		{
			// Standard JSON-RPC 2.0 error codes.
			const int kParseError     = -32700;
			const int kInvalidRequest = -32600;
			const int kMethodNotFound = -32601;
			const int kInvalidParams  = -32602;
			const int kInternalError  = -32603;

			//! The severity token for a diagnostic.
			const char* SeverityName( AgentDiagnostic::Severity s )
			{
				switch( s ) {
					case AgentDiagnostic::Severity::Error:   return "error";
					case AgentDiagnostic::Severity::Warning: return "warning";
					case AgentDiagnostic::Severity::Info:    return "info";
				}
				return "error";
			}

			//! Build a JSON-RPC success envelope string for `result` under
			//! request id `id` (which is a raw JSON value: number, string, or
			//! null for a request that had no/invalid id).
			std::string MakeSuccess( const JsonValue& id, const JsonValue& result )
			{
				JsonValue env = JsonValue::MakeObject();
				env.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
				env.set( "id", id );
				env.set( "result", result );
				return JsonSerialize( env );
			}

			//! Build a JSON-RPC error envelope string.
			std::string MakeError( const JsonValue& id, int code, const std::string& message )
			{
				JsonValue err = JsonValue::MakeObject();
				err.set( "code", JsonValue::MakeNumber( static_cast<double>( code ) ) );
				err.set( "message", JsonValue::MakeString( message ) );

				JsonValue env = JsonValue::MakeObject();
				env.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
				env.set( "id", id );
				env.set( "error", err );
				return JsonSerialize( env );
			}

			//! Facet 5 slice 1a: a head-version as a nested JSON object
			//! {uuid:number, revision:number}.  JSON numbers are doubles, but a
			//! monotonic counter starting at 1 stays well under 2^53, so both
			//! fields are exactly representable -- emitting them as numbers is
			//! lossless (documented in the AgentRpc.h method index).
			JsonValue HeadVersionJson( const RISE::Cst::CstHeadVersion& hv )
			{
				JsonValue o = JsonValue::MakeObject();
				o.set( "uuid",     JsonValue::MakeNumber( static_cast<double>( hv.uuid ) ) );
				o.set( "revision", JsonValue::MakeNumber( static_cast<double>( hv.revision ) ) );
				return o;
			}

			//! Model-B F5 slice S2: parse the OPTIONAL `baseHeadVersion` param
			//! shared by propose_patch / insert_chunk / remove_chunk.  Returns
			//! 1 = present and valid (outBase filled), 0 = absent (or null --
			//! unconditional edit), -1 = malformed (outErr carries the -32602
			//! message).  The validation is the slice-1a contract verbatim:
			//! numeric uuid/revision, finite (explicit range test, NOT
			//! std::isfinite -- -ffast-math folds that to true), non-negative,
			//! integral, <= 2^53 (the largest exactly-representable integer
			//! double; the monotonic-from-1 counters never approach it).
			int ParseBaseHeadVersionParam( const JsonValue& params,
			                               RISE::Cst::CstHeadVersion& outBase,
			                               std::string& outErr )
			{
				const JsonValue* bhv = params.find( "baseHeadVersion" );
				if( !bhv || bhv->isNull() ) return 0;
				if( !bhv->isObject() ) {
					outErr = "Invalid params: 'baseHeadVersion' must be an object {uuid,revision}";
					return -1;
				}
				const JsonValue* u = bhv->find( "uuid" );
				const JsonValue* rv = bhv->find( "revision" );
				if( !u || !u->isNumber() || !rv || !rv->isNumber() ) {
					outErr = "Invalid params: 'baseHeadVersion' needs numeric 'uuid' and 'revision'";
					return -1;
				}
				const double ud = u->asNumber();
				const double rd = rv->asNumber();
				if( !( ud >= 0.0 && ud <= 9007199254740992.0 && ud == std::floor( ud ) &&
				       rd >= 0.0 && rd <= 9007199254740992.0 && rd == std::floor( rd ) ) ) {
					outErr = "Invalid params: 'baseHeadVersion' uuid/revision must be finite non-negative integers";
					return -1;
				}
				outBase.uuid     = static_cast<std::uint64_t>( ud );
				outBase.revision = static_cast<std::uint64_t>( rd );
				return 1;
			}

			//! Model-B F5 slice S2: serialize an AgentChunkResult (insert_chunk /
			//! remove_chunk share the shape: the propose_patch result fields plus
			//! the affected chunk's name/kind echo).
			JsonValue ChunkResultJson( const AgentChunkResult& cr )
			{
				JsonValue result = JsonValue::MakeObject();
				result.set( "applied",   JsonValue::MakeBool( cr.applied ) );
				result.set( "rawCode",   JsonValue::MakeNumber( static_cast<double>( cr.rawCode ) ) );
				result.set( "status",    JsonValue::MakeString( cr.status ) );
				result.set( "retriable", JsonValue::MakeBool( cr.retriable ) );
				result.set( "headVersion", HeadVersionJson( cr.headVersion ) );
				result.set( "message",   JsonValue::MakeString( cr.message ) );
				result.set( "name",      JsonValue::MakeString( cr.name ) );
				result.set( "kind",      JsonValue::MakeString( cr.kind ) );
				return result;
			}

			//! Parse a schema JSON STRING (from SchemaGen) into a JsonValue so
			//! it embeds as a nested object in the response, not a stringified
			//! blob.  On the (defensive) chance SchemaGen ever emits something
			//! that does not re-parse, fall back to a string value so the
			//! caller still gets the schema.
			JsonValue SchemaAsJson( const std::string& schemaText )
			{
				JsonValue parsed;
				std::string err;
				if( JsonParse( schemaText, parsed, err ) ) return parsed;
				return JsonValue::MakeString( schemaText );
			}
		}

		AgentRpcDispatcher::AgentRpcDispatcher( std::unique_ptr<AgentSession> session )
			: mSession( std::move( session ) )
		{
		}

		AgentRpcDispatcher::~AgentRpcDispatcher()
		{
		}

		std::string AgentRpcDispatcher::HandleLine( const std::string& jsonRpcRequest )
		{
			// The id defaults to null: a request that fails to parse (or whose
			// envelope is invalid before we can read an id) responds with
			// id=null, which is the JSON-RPC contract for un-attributable
			// errors.  Once we have read a valid id we echo it back.
			JsonValue idValue = JsonValue::MakeNull();

			try {
				// (1) Parse the line -> -32700 on malformation.
				JsonValue req;
				std::string parseErr;
				if( !JsonParse( jsonRpcRequest, req, parseErr ) ) {
					return MakeError( idValue, kParseError, "Parse error: " + parseErr );
				}

				// (2) Envelope must be an object -> else -32600.
				if( !req.isObject() ) {
					return MakeError( idValue, kInvalidRequest, "Invalid Request: not a JSON object" );
				}

				// (3) Echo the id if present (number / string / null are the
				// valid id types; anything else we treat as absent -> null).
				if( const JsonValue* id = req.find( "id" ) ) {
					if( id->isNumber() || id->isString() || id->isNull() ) idValue = *id;
				}

				// (4) `method` must be a string -> else -32600.
				const JsonValue* method = req.find( "method" );
				if( !method || !method->isString() ) {
					return MakeError( idValue, kInvalidRequest, "Invalid Request: missing or non-string 'method'" );
				}
				const std::string& m = method->asString();

				// (5) `params` is optional; when present it must be an object
				// (this set uses named params only).  Absent -> empty object.
				JsonValue params = JsonValue::MakeObject();
				if( const JsonValue* p = req.find( "params" ) ) {
					if( p->isObject() ) params = *p;
					else if( !p->isNull() )
						return MakeError( idValue, kInvalidParams, "Invalid params: 'params' must be an object" );
				}

				AgentSession* s = mSession.get();

				//--------------------------------------------------------------
				// read_document -> {document:string, hasDocument:bool, headVersion:{uuid,revision}}
				//   No session (no-head bootstrap): an agent starting FRESH
				//   calls read_document first -- an error there is hostile.
				//   Return the honest, useful signal: an empty document with
				//   hasDocument=false + headVersion {0,0} (there is genuinely no
				//   head yet).  Facet 5 slice 1a: an agent reads headVersion here
				//   and passes it back as a patch's baseHeadVersion.
				//--------------------------------------------------------------
				if( m == "read_document" ) {
					JsonValue result = JsonValue::MakeObject();
					if( !s ) {
						result.set( "document", JsonValue::MakeString( "" ) );
						result.set( "hasDocument", JsonValue::MakeBool( false ) );
						result.set( "headVersion", HeadVersionJson( RISE::Cst::CstHeadVersion{} ) );
						return MakeSuccess( idValue, result );
					}
					result.set( "document", JsonValue::MakeString( s->ReadDocument() ) );
					result.set( "hasDocument", JsonValue::MakeBool( s->HasDocument() ) );
					result.set( "headVersion", HeadVersionJson( s->HeadVersion() ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// read_schema {keyword?} -> the schema JSON (nested object)
				//   STATELESS: the schema is a pure descriptor-registry walk
				//   (SchemaGenAll / SchemaGenForChunk touch NO Job), so it needs
				//   NO loaded head -- an agent CONSTRUCTING a scene from scratch
				//   reads the grammar first.  We call SchemaGen directly rather
				//   than through the session so the no-head path works.
				//--------------------------------------------------------------
				if( m == "read_schema" ) {
					std::string keyword;
					if( const JsonValue* kw = params.find( "keyword" ) ) {
						if( kw->isString() ) keyword = kw->asString();
						else if( !kw->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'keyword' must be a string" );
					}
					const std::string schemaText =
						keyword.empty() ? RISE::Agent::SchemaGenAll()
						                 : RISE::Agent::SchemaGenForChunk( keyword );
					JsonValue result = JsonValue::MakeObject();
					result.set( "schema", SchemaAsJson( schemaText ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// read_skill {name?} -> the skills index / one skill's markdown
				//   STATELESS (like read_schema): skills are on-disk markdown,
				//   no Job involved -- so it needs NO loaded head and works in
				//   the no-head bootstrap.  Progressive disclosure: no `name`
				//   -> {skills:[{name,title,hook},...]} (the INDEX); a `name`
				//   -> {name, markdown}.  Path safety lives in
				//   AgentSession::ReadSkill (bare-name-only; '/', '\\', ".."
				//   rejected; only .md files inside the skills root are
				//   served); a rejected or unknown name maps to -32602 with
				//   the session's message.
				//--------------------------------------------------------------
				if( m == "read_skill" ) {
					std::string name;
					if( const JsonValue* nv = params.find( "name" ) ) {
						if( nv->isString() ) name = nv->asString();
						else if( !nv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'name' must be a string" );
					}
					const AgentSkillResult sr = AgentSession::ReadSkill( name );
					if( !sr.ok ) {
						return MakeError( idValue, kInvalidParams, "Invalid params: " + sr.error );
					}
					JsonValue result = JsonValue::MakeObject();
					if( name.empty() ) {
						JsonValue arr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < sr.index.size(); ++i ) {
							JsonValue e = JsonValue::MakeObject();
							e.set( "name",  JsonValue::MakeString( sr.index[i].name ) );
							e.set( "title", JsonValue::MakeString( sr.index[i].title ) );
							e.set( "hook",  JsonValue::MakeString( sr.index[i].hook ) );
							arr.push_back( e );
						}
						result.set( "skills", arr );
						// Index-only advisory: a missing skills ROOT (vs a
						// present-but-empty one) -- surfaced so an agent can tell
						// a miswired install from "no skills shipped".
						if( !sr.note.empty() )
							result.set( "note", JsonValue::MakeString( sr.note ) );
					}
					else {
						result.set( "name",     JsonValue::MakeString( sr.name ) );
						result.set( "markdown", JsonValue::MakeString( sr.markdown ) );
					}
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// validate {text} -> {diagnostics:[...]}
				//   STATELESS: validation parses `text` to a CST and derives it
				//   into a THROWAWAY Job (never a session's head), so it needs
				//   NO loaded head -- an agent REPAIRING a scene from scratch
				//   validates a candidate BEFORE any head exists.  We call the
				//   static ValidateText directly so the no-head path works.
				//--------------------------------------------------------------
				if( m == "validate" ) {
					const JsonValue* text = params.find( "text" );
					if( !text || !text->isString() ) {
						return MakeError( idValue, kInvalidParams, "Invalid params: 'text' (string) is required" );
					}
					const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( text->asString() );
					JsonValue arr = JsonValue::MakeArray();
					for( const AgentDiagnostic& d : diags ) {
						JsonValue dj = JsonValue::MakeObject();
						dj.set( "severity", JsonValue::MakeString( SeverityName( d.severity ) ) );
						dj.set( "code",     JsonValue::MakeString( d.code ) );
						dj.set( "message",  JsonValue::MakeString( d.message ) );
						dj.set( "offset",   JsonValue::MakeNumber( static_cast<double>( d.offset ) ) );
						dj.set( "length",   JsonValue::MakeNumber( static_cast<double>( d.length ) ) );
						arr.push_back( dj );
					}
					JsonValue result = JsonValue::MakeObject();
					result.set( "diagnostics", arr );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// propose_patch {target,kind?,param,value,baseHeadVersion?}
				//   -> {applied,rawCode,status,retriable,headVersion,message}
				//   `applied` is CLEAN success only; `status` is the four-state
				//   gate {"applied","rejected","diagnosed","conflict"} (a rawCode-3
				//   re-derive is applied=false/status="diagnosed": mutated but
				//   the re-derive diagnosed -- NOT a clean apply; a stale
				//   baseHeadVersion is applied=false/status="conflict", head
				//   untouched).  REQUIRES a head (it edits the retained Document):
				//   no session -> error.  Facet 5 slice 1a: the OPTIONAL
				//   `baseHeadVersion` object {uuid:number,revision:number} is the
				//   optimistic-concurrency precondition -- when present, a mismatch
				//   with the current head returns status="conflict" WITHOUT
				//   mutating; absent -> unconditional (back-compat).  The result's
				//   `headVersion` is the head AFTER the call (post-commit on a
				//   clean apply; current head otherwise).  `retriable` splits the
				//   "rejected" bucket: true = TRANSIENT refusal (today: an open
				//   editor transaction in LIVE mode) -- resubmit the SAME patch
				//   later; false = permanent (retrying verbatim can never
				//   succeed).  A "conflict" is retriable-by-protocol via re-read
				//   + re-propose, so it does NOT set the flag.
				//--------------------------------------------------------------
				if( m == "propose_patch" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const JsonValue* target = params.find( "target" );
					const JsonValue* param  = params.find( "param" );
					const JsonValue* value  = params.find( "value" );
					if( !target || !target->isString() ||
					    !param  || !param->isString()  ||
					    !value  || !value->isString() ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'target', 'param', and 'value' (strings) are required" );
					}
					AgentSetPatch sp;
					sp.target = target->asString();
					sp.param  = param->asString();
					sp.value  = value->asString();
					if( const JsonValue* kind = params.find( "kind" ) ) {
						if( kind->isString() ) sp.kind = kind->asString();
						else if( !kind->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'kind' must be a string" );
					}
					// OPTIONAL baseHeadVersion: when present it MUST be an object
					// with numeric, finite, non-negative INTEGRAL uuid/revision
					// (else -32602); null is treated as absent (unconditional
					// edit).  Validation factored into ParseBaseHeadVersionParam
					// (Model-B F5 S2: insert_chunk / remove_chunk share it) --
					// the UB-guard rationale (explicit range test surviving
					// -ffast-math, NOT std::isfinite) lives on the helper.
					{
						std::string bErr;
						const int b = ParseBaseHeadVersionParam( params, sp.baseVersion, bErr );
						if( b < 0 ) return MakeError( idValue, kInvalidParams, bErr );
						sp.hasBaseVersion = ( b == 1 );
					}
					const AgentPatchResult pr = s->ProposePatch( sp );
					JsonValue result = JsonValue::MakeObject();
					result.set( "applied", JsonValue::MakeBool( pr.applied ) );
					result.set( "rawCode", JsonValue::MakeNumber( static_cast<double>( pr.rawCode ) ) );
					result.set( "status",  JsonValue::MakeString( pr.status ) );
					result.set( "retriable", JsonValue::MakeBool( pr.retriable ) );
					result.set( "headVersion", HeadVersionJson( pr.headVersion ) );
					result.set( "message", JsonValue::MakeString( pr.message ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// insert_chunk {chunkText, baseHeadVersion?}
				//   -> {applied,rawCode,status,retriable,headVersion,message,name,kind}
				//   Model-B F5 slice S2: ADD one complete chunk to the head and
				//   REALIZE it via a dry-run-guarded full re-derive.  The result
				//   gates exactly like propose_patch ({"applied","rejected",
				//   "diagnosed","conflict"} + retriable) and echoes the parsed
				//   chunk's kind/name.  REQUIRES a head: no session -> error.
				//--------------------------------------------------------------
				if( m == "insert_chunk" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const JsonValue* chunkText = params.find( "chunkText" );
					if( !chunkText || !chunkText->isString() ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'chunkText' (string) is required" );
					}
					RISE::Cst::CstHeadVersion base;
					std::string bErr;
					const int b = ParseBaseHeadVersionParam( params, base, bErr );
					if( b < 0 ) return MakeError( idValue, kInvalidParams, bErr );
					const AgentChunkResult cr =
						s->InsertChunk( chunkText->asString(), ( b == 1 ) ? &base : nullptr );
					return MakeSuccess( idValue, ChunkResultJson( cr ) );
				}

				//--------------------------------------------------------------
				// remove_chunk {target, kind?, baseHeadVersion?}
				//   -> {applied,rawCode,status,retriable,headVersion,message,name,kind}
				//   Model-B F5 slice S2: REMOVE the chunk resolved by bare name
				//   (+ optional kind narrowing, same resolution as propose_patch)
				//   via the trivia-preserving erase; a still-referenced target is
				//   rejected with the dry-run diagnostic, head byte-identical.
				//--------------------------------------------------------------
				if( m == "remove_chunk" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const JsonValue* target = params.find( "target" );
					if( !target || !target->isString() ) {
						return MakeError( idValue, kInvalidParams,
							"Invalid params: 'target' (string) is required" );
					}
					std::string kind;
					if( const JsonValue* kv = params.find( "kind" ) ) {
						if( kv->isString() ) kind = kv->asString();
						else if( !kv->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'kind' must be a string" );
					}
					RISE::Cst::CstHeadVersion base;
					std::string bErr;
					const int b = ParseBaseHeadVersionParam( params, base, bErr );
					if( b < 0 ) return MakeError( idValue, kInvalidParams, bErr );
					const AgentChunkResult cr =
						s->RemoveChunk( target->asString(), kind, ( b == 1 ) ? &base : nullptr );
					return MakeSuccess( idValue, ChunkResultJson( cr ) );
				}

				//--------------------------------------------------------------
				// render {samples?} -> {ok,width,height,meanR,meanG,meanB,
				//                       integrator,message}
				//   (NOT the image bytes -- render stays lean; read_image
				//    carries the base64 PNG.  `integrator` = the ACTIVE
				//    rasterizer's chunk keyword, empty when none is active.)
				//--------------------------------------------------------------
				if( m == "render" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					int samples = -1;
					if( const JsonValue* sm = params.find( "samples" ) ) {
						if( sm->isNumber() ) {
							// Guard the cast: static_cast<int>(inf/nan) is UB.  A
							// hostile {"samples":1e999} parses to +inf.  We do NOT
							// use std::isfinite here: the production build compiles
							// with -ffast-math (-> -ffinite-math-only), under which
							// clang constant-folds std::isfinite(x) to true and the
							// guard becomes dead code stripped by the optimizer.  An
							// explicit range comparison against the int32 bounds
							// survives -ffinite-math-only (a plain >=/<= on the
							// double is not folded away) and rejects NaN and +/-inf
							// alike before the narrowing cast: NaN fails both
							// comparisons, +/-inf fails the finite bound.  The
							// bounds are exactly representable as double
							// (2^31-1 and -2^31).
							const double sv = sm->asNumber();
							if( !( sv >= -2147483648.0 && sv <= 2147483647.0 ) )
								return MakeError( idValue, kInvalidParams, "Invalid params: 'samples' must be a finite, in-range number" );
							samples = static_cast<int>( sv );
						}
						else if( !sm->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'samples' must be a number" );
					}
					const AgentRenderResult rr = s->Render( samples );
					JsonValue result = JsonValue::MakeObject();
					result.set( "ok",     JsonValue::MakeBool( rr.ok ) );
					result.set( "width",  JsonValue::MakeNumber( static_cast<double>( rr.width ) ) );
					result.set( "height", JsonValue::MakeNumber( static_cast<double>( rr.height ) ) );
					result.set( "meanR",  JsonValue::MakeNumber( rr.meanR ) );
					result.set( "meanG",  JsonValue::MakeNumber( rr.meanG ) );
					result.set( "meanB",  JsonValue::MakeNumber( rr.meanB ) );
					result.set( "integrator", JsonValue::MakeString( rr.integrator ) );
					result.set( "message", JsonValue::MakeString( rr.message ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// read_image -> {png_base64:string, byteLength:number}
				//   Reads the LAST successful render's cached PNG bytes and
				//   base64-encodes them so the binary travels in JSON.
				//--------------------------------------------------------------
				if( m == "read_image" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const std::vector<unsigned char> png = s->ReadImage();
					JsonValue result = JsonValue::MakeObject();
					result.set( "png_base64", JsonValue::MakeString( Base64Encode( png ) ) );
					result.set( "byteLength", JsonValue::MakeNumber( static_cast<double>( png.size() ) ) );
					return MakeSuccess( idValue, result );
				}

				// (6) Anything else -> method not found.
				return MakeError( idValue, kMethodNotFound, "Method not found: " + m );
			}
			catch( const std::exception& e ) {
				return MakeError( idValue, kInternalError, std::string( "internal error: " ) + e.what() );
			}
			catch( ... ) {
				return MakeError( idValue, kInternalError, "internal error: unknown exception" );
			}
		}
	}
}
