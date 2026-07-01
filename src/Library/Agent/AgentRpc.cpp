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
				// read_document -> {document:string}
				//--------------------------------------------------------------
				if( m == "read_document" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					JsonValue result = JsonValue::MakeObject();
					result.set( "document", JsonValue::MakeString( s->ReadDocument() ) );
					result.set( "hasDocument", JsonValue::MakeBool( s->HasDocument() ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// read_schema {keyword?} -> the schema JSON (nested object)
				//--------------------------------------------------------------
				if( m == "read_schema" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					std::string keyword;
					if( const JsonValue* kw = params.find( "keyword" ) ) {
						if( kw->isString() ) keyword = kw->asString();
						else if( !kw->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'keyword' must be a string" );
					}
					const std::string schemaText = s->ReadSchema( keyword );
					JsonValue result = JsonValue::MakeObject();
					result.set( "schema", SchemaAsJson( schemaText ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// validate {text} -> {diagnostics:[...]}
				//--------------------------------------------------------------
				if( m == "validate" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					const JsonValue* text = params.find( "text" );
					if( !text || !text->isString() ) {
						return MakeError( idValue, kInvalidParams, "Invalid params: 'text' (string) is required" );
					}
					const std::vector<AgentDiagnostic> diags = s->Validate( text->asString() );
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
				// propose_patch {target,kind?,param,value} -> {applied,rawCode,message}
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
					const AgentPatchResult pr = s->ProposePatch( sp );
					JsonValue result = JsonValue::MakeObject();
					result.set( "applied", JsonValue::MakeBool( pr.applied ) );
					result.set( "rawCode", JsonValue::MakeNumber( static_cast<double>( pr.rawCode ) ) );
					result.set( "message", JsonValue::MakeString( pr.message ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// render {samples?} -> {ok,width,height,meanR,meanG,meanB,message}
				//   (NOT the image bytes -- render stays lean; read_image
				//    carries the base64 PNG.)
				//--------------------------------------------------------------
				if( m == "render" ) {
					if( !s ) return MakeError( idValue, kInternalError, "no session loaded" );
					int samples = -1;
					if( const JsonValue* sm = params.find( "samples" ) ) {
						if( sm->isNumber() ) samples = static_cast<int>( sm->asNumber() );
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
					result.set( "message", JsonValue::MakeString( rr.message ) );
					return MakeSuccess( idValue, result );
				}

				//--------------------------------------------------------------
				// read_image -> {png_base64:string,width,height}
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
