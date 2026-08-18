//////////////////////////////////////////////////////////////////////
//
//  AgentMcpAdapter.cpp - the MCP envelope adapter (see AgentMcpAdapter.h).
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "AgentMcpAdapter.h"

#include "AgentChatCodecs.h"
#include "AgentRpc.h"
#include "AgentSession.h"
#include "Json.h"

#include "../RISE_API.h"

#include <cstdio>
#include <string>
#include <vector>

namespace RISE
{
	namespace Agent
	{
		namespace
		{
			// Standard JSON-RPC 2.0 error codes (the same set AgentRpc.cpp
			// honours -- this adapter's OWN envelope-handling layer, on top
			// of / around the wrapped dispatcher, uses the identical codes
			// for the identical reasons).
			const int kParseError     = -32700;
			const int kInvalidRequest = -32600;
			const int kMethodNotFound = -32601;
			const int kInvalidParams  = -32602;
			const int kInternalError  = -32603;

			// The MCP protocol revision this adapter implements.  2025-03-26
			// is the last revision before batch JSON-RPC requests were
			// dropped from the spec -- and this adapter does not support
			// batching either, so it is the natural, honest baseline (see
			// the file header's doc for the full rationale).
			const char* const kProtocolVersion = "2025-03-26";

			//! Build a JSON-RPC success envelope {jsonrpc,id,result}.
			std::string MakeSuccess( const JsonValue& id, const JsonValue& result )
			{
				JsonValue env = JsonValue::MakeObject();
				env.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
				env.set( "id", id );
				env.set( "result", result );
				return JsonSerialize( env );
			}

			//! Build a JSON-RPC error envelope {jsonrpc,id,error:{code,message}}.
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

			//! A JSON Schema `{type:"string"}` leaf, optionally with a
			//! `description`.  Small helpers below build the per-tool
			//! inputSchema objects hand-authored to match AgentRpc.cpp's
			//! ACTUAL param parsing (types, ranges, required-ness) -- not a
			//! re-derivation via SchemaGen, which describes the SCENE-FILE
			//! chunk grammar, a different schema entirely from the RPC
			//! verbs' parameter shapes.
			JsonValue StringProp( const std::string& description )
			{
				JsonValue o = JsonValue::MakeObject();
				o.set( "type", JsonValue::MakeString( "string" ) );
				if( !description.empty() ) o.set( "description", JsonValue::MakeString( description ) );
				return o;
			}

			JsonValue NumberProp( const std::string& description )
			{
				JsonValue o = JsonValue::MakeObject();
				o.set( "type", JsonValue::MakeString( "number" ) );
				if( !description.empty() ) o.set( "description", JsonValue::MakeString( description ) );
				return o;
			}

			JsonValue BoolProp( const std::string& description )
			{
				JsonValue o = JsonValue::MakeObject();
				o.set( "type", JsonValue::MakeString( "boolean" ) );
				if( !description.empty() ) o.set( "description", JsonValue::MakeString( description ) );
				return o;
			}

			//! A JSON Schema `{type:"array", items:{type:"string"}}` leaf,
			//! optionally with a `description` -- for params AgentRpc.cpp
			//! parses as an array-of-strings (element-type-checked, non-array
			//! or non-string elements rejected with -32602).
			JsonValue StringArrayProp( const std::string& description )
			{
				JsonValue o = JsonValue::MakeObject();
				o.set( "type", JsonValue::MakeString( "array" ) );
				o.set( "items", StringProp( "" ) );
				if( !description.empty() ) o.set( "description", JsonValue::MakeString( description ) );
				return o;
			}

			JsonValue ObjectProp( const std::string& description, const JsonValue& properties,
			                      const std::vector<std::string>& required )
			{
				JsonValue o = JsonValue::MakeObject();
				o.set( "type", JsonValue::MakeString( "object" ) );
				if( !description.empty() ) o.set( "description", JsonValue::MakeString( description ) );
				o.set( "properties", properties );
				if( !required.empty() ) {
					JsonValue req = JsonValue::MakeArray();
					for( const std::string& r : required ) req.push_back( JsonValue::MakeString( r ) );
					o.set( "required", req );
				}
				return o;
			}

			//! The `baseHeadVersion` object shared by all 6 mutating tools
			//! (propose_patch/propose_patches/insert_chunk/insert_chunks/
			//! remove_chunk/remove_chunks) -- optional optimistic-concurrency precondition,
			//! {uuid,revision} both numeric.  The BATCH forms take it too:
			//! the precondition is on the HEAD the batch starts from, not on
			//! any one element.
			JsonValue BaseHeadVersionSchema()
			{
				JsonValue props = JsonValue::MakeObject();
				props.set( "uuid",     NumberProp( "The head's uuid, as read from a prior read_document/*_chunk/propose_patch call's headVersion." ) );
				props.set( "revision", NumberProp( "The head's revision, as read from a prior call's headVersion." ) );
				return ObjectProp(
					"OPTIONAL optimistic-concurrency precondition {uuid,revision}. When present, "
					"the edit is REJECTED with status=\"conflict\" (head left untouched) if the "
					"current head does not match -- re-read read_document and retry with the new "
					"headVersion. Omit for an unconditional edit (back-compat).",
					props, std::vector<std::string>() );
			}

			//! GUI render modes P1+P2a (docs/gui/RENDER_MODES.md §8): build
			//! the `mode` = "normals"|"depth"|"facets"|"wireframe"|
			//! "deep_reflect"|"direct" portion of the render tool's `mode`
			//! description FROM Implementation::GetViewportRenderModes --
			//! filtered to `casterFactory` OR BeautyVariant entries, each
			//! rendered as `"name" (question)` -- so the wording can never
			//! drift from the registry the C-ABI, both GUI dropdowns, and
			//! AgentRpc.cpp's mode parser all share (single source of truth,
			//! docs/gui/RENDER_MODES.md §4).
			std::string DescribeViewModes()
			{
				unsigned int modeCount = 0;
				const Implementation::ViewportRenderModeInfo* modes =
					Implementation::GetViewportRenderModes( modeCount );
				std::string out;
				bool first = true;
				for( unsigned int i = 0; i < modeCount; ++i ) {
					const bool agentVisible = modes[i].casterFactory
						|| Implementation::IsBeautyVariantMode( modes[i].mode );
					if( !agentVisible ) continue;
					if( !first ) out += ", ";
					first = false;
					out += "\"";
					out += modes[i].name;
					out += "\" (";
					out += modes[i].question;
					out += ")";
				}
				return out;
			}

			//! The `camera` override object shared by `render` -- an
			//! EPHEMERAL one-render-only pose override, captured and
			//! restored around the render so the active camera's properties
			//! are byte-for-byte identical before and after every call.
			JsonValue CameraOverrideSchema()
			{
				JsonValue props = JsonValue::MakeObject();
				props.set( "location", StringProp( "Camera position \"x y z\" -- EXACTLY 3 whitespace-separated finite numbers (e.g. \"0 5 10\"). Required together with lookat." ) );
				props.set( "lookat",   StringProp( "Look-at target \"x y z\" -- EXACTLY 3 whitespace-separated finite numbers. Required together with location." ) );
				props.set( "up",       StringProp( "OPTIONAL up vector \"x y z\" -- EXACTLY 3 whitespace-separated finite numbers." ) );
				props.set( "fov",      NumberProp( "OPTIONAL field of view in DEGREES, strictly inside the open interval (0, 180)." ) );
				std::vector<std::string> required;
				required.push_back( "location" );
				required.push_back( "lookat" );
				return ObjectProp(
					"OPTIONAL ephemeral override of the ACTIVE camera's pose for THIS render only. "
					"The camera's properties are captured before the override and restored after, "
					"so they are byte-for-byte identical before and after the call whether or not "
					"an override was requested. location/lookat must be supplied together; each "
					"vector field must parse as EXACTLY 3 finite numbers (wrong token count or "
					"non-numeric components are rejected, not silently ignored).",
					props, required );
			}

			//! Build one MCP Tool descriptor {name,description,inputSchema}.
			JsonValue MakeTool( const std::string& name, const std::string& description,
			                    const JsonValue& inputSchema )
			{
				JsonValue tool = JsonValue::MakeObject();
				tool.set( "name", JsonValue::MakeString( name ) );
				tool.set( "description", JsonValue::MakeString( description ) );
				tool.set( "inputSchema", inputSchema );
				return tool;
			}

			//! Secure-MCP slice 2: the refusal note prepended to the
			//! mutating tools' (propose_patch/propose_patches/insert_chunk/
			//! insert_chunks/insert_material_scaffold/insert_geometry_scaffold/
			//! remove_chunk)
			//! descriptions under AgentAutonomy::Read (arc-75 S2.1 added
			//! insert_material_scaffold to this note's set, and arc-75 S3b
			//! added its geometry sibling insert_geometry_scaffold, but
			//! NEITHER is on IsProposeSafeVerb -- see
			//! kScaffoldProposeRefusedNote's doc for why each needs its OWN,
			//! distinct note under Propose instead of kAutonomyProposeNote).
			//! DECIDED: annotate, don't hide --
			//! the tool stays fully visible (real inputSchema, callable
			//! shape) so a client can still explain to its user what the
			//! tool would do and why it is currently refused, rather than
			//! the tool silently disappearing from tools/list.
			const std::string kAutonomyReadNote =
				"[REFUSED under --agent-autonomy=read: this session is read-only; "
				"calling this tool returns a policy-refusal error (relaunch with "
				"--agent-autonomy=commit to enable it)] ";

			//! Secure-MCP slice 5b fix round (P2-1): the sibling annotation for
			//! the same 6 mutating tools (propose_patch/propose_patches/
			//! insert_chunk/insert_chunks/remove_chunk/remove_chunks) under
			//! AgentAutonomy::Propose specifically.  Under Propose those
			//! tools REACH the session (unlike Read, where kAutonomyReadNote's
			//! tool is refused before dispatch) -- but for an External-
			//! authority session with a live controller attached, the call
			//! STAGES a proposal rather than committing it outright.  Without
			//! this note, an external MCP agent calling propose_patch under
			//! Propose sees a description byte-identical to Commit's and has
			//! no textual signal that its edit needs a human's approval before
			//! it takes effect -- the exact "teaching-text gap" this note
			//! closes.  Distinct wording from BOTH kAutonomyReadNote (that one
			//! says "refused"; this one says "staged, not committed") and
			//! kResolveProposalOwnerOnlyNote (that one is about who may
			//! resolve; this one is about what THIS call itself does).
			const std::string kAutonomyProposeNote =
				"[NOTE under --agent-autonomy=propose: this call STAGES a proposal for the "
				"document owner to approve/reject rather than committing directly -- the "
				"response's status will be \"staged\" (pending), not \"applied\"; poll "
				"list_proposals for the staged entry's status (pending -> applied/rejected/"
				"conflict), and expect the owner to approve it in their own GUI/session "
				"via resolve_proposal, which THIS session may never call] ";

			//! Secure-MCP slice 5b: the sibling annotation for resolve_proposal
			//! specifically, prepended under EITHER Read or Propose (it is
			//! refused under both -- see AgentRpc.h's file header for why it is
			//! deliberately excluded from Propose's extended allowlist).
			//! Distinct wording from kAutonomyReadNote: "relaunch with commit"
			//! is not this tool's real escape hatch for an external/proposing
			//! session (only the document owner can ever resolve a proposal,
			//! regardless of this transport's own posture).
			const std::string kResolveProposalOwnerOnlyNote =
				"[OWNER-ONLY: refused for any non-owner session, at every autonomy posture except "
				"Commit (the posture the document owner's own session runs at) -- an external/"
				"proposing session can list and poll proposals but never approve or reject one] ";

			//! Arc-75 slice S2.1: insert_material_scaffold's OWN annotation
			//! under AgentAutonomy::Propose SPECIFICALLY.  It is a mutating
			//! tool, but -- unlike the 6 tools kAutonomyProposeNote covers
			//! (R2 fix-round, 2026-08-10: recounted at the 6 actual use sites
			//! -- the count had drifted to "5" since remove_chunks landed)
			//! -- it is DELIBERATELY excluded from AgentRpc.cpp's
			//! IsProposeSafeVerb (the ripple across dozens of "N mutating
			//! verbs" prose restatements plus the two GUI client-side
			//! retry-verb sets SourceHygieneTest.cpp mechanically pins to
			//! IsProposeSafeVerb was judged out of scope for this slice;
			//! see the arc log).  So under Propose it is refused with the
			//! SAME kAutonomyRefused error Read gives it (it never reaches
			//! AgentSession, so it never gets a chance to STAGE a proposal
			//! the way propose_patch/insert_chunk do) -- kAutonomyReadNote
			//! is reused VERBATIM for the Read posture (accurate: "this
			//! session is read-only" IS true there), but that text would be
			//! misleading under Propose (the session is NOT read-only,
			//! only this one tool is unreachable), so Propose gets this
			//! dedicated note instead.  Deliberately contains NEITHER
			//! magic substring AgentAutonomyPolicyTest's per-note counters
			//! key on ("[REFUSED under --agent-autonomy=read" /
			//! "[NOTE under --agent-autonomy=propose") -- this tool is
			//! neither read-refused-with-that-wording nor propose-staged,
			//! so counting it in either bucket would corrupt those RED-
			//! PROVE counts.
			const std::string kScaffoldProposeRefusedNote =
				"[UNAVAILABLE at --agent-autonomy=propose: insert_material_scaffold is not on the "
				"Propose-autonomy allowlist and is refused here exactly as under Read (relaunch with "
				"--agent-autonomy=commit to use it)] ";

			//! Arc-75 slice S3b: insert_geometry_scaffold's OWN annotation
			//! under AgentAutonomy::Propose SPECIFICALLY -- the geometry
			//! sibling of kScaffoldProposeRefusedNote above, same
			//! rationale (deliberately excluded from IsProposeSafeVerb;
			//! refused under Propose exactly like Read; deliberately
			//! contains neither magic substring the per-note counters key
			//! on).
			const std::string kGeometryScaffoldProposeRefusedNote =
				"[UNAVAILABLE at --agent-autonomy=propose: insert_geometry_scaffold is not on the "
				"Propose-autonomy allowlist and is refused here exactly as under Read (relaunch with "
				"--agent-autonomy=commit to use it)] ";

			//! R2 (2026-08-10): replace_geometry_scaffold's OWN annotation
			//! under AgentAutonomy::Propose SPECIFICALLY -- the third sibling
			//! of the two notes above, same rationale (deliberately excluded
			//! from IsProposeSafeVerb; refused under Propose exactly like
			//! Read; deliberately contains neither magic substring the
			//! per-note counters key on).
			const std::string kReplaceGeometryScaffoldProposeRefusedNote =
				"[UNAVAILABLE at --agent-autonomy=propose: replace_geometry_scaffold is not on the "
				"Propose-autonomy allowlist and is refused here exactly as under Read (relaunch with "
				"--agent-autonomy=commit to use it)] ";

			//! S2 (2026-08-11): the two CLEAN-ROOM verbs' own annotations under
			//! AgentAutonomy::Propose SPECIFICALLY -- the fourth and fifth
			//! siblings of the three scaffold notes above, same rationale
			//! (both mutate; both deliberately excluded from
			//! AgentRpc.cpp's IsProposeSafeVerb rather than pay the "N mutating
			//! verbs" prose ripple SourceHygieneTest's verb-parity scan pins;
			//! refused under Propose exactly like Read; deliberately contains
			//! neither magic substring the per-note counters key on).
			const std::string kBuildElementProposeRefusedNote =
				"[UNAVAILABLE at --agent-autonomy=propose: build_element is not on the "
				"Propose-autonomy allowlist and is refused here exactly as under Read (relaunch with "
				"--agent-autonomy=commit to use it)] ";
			const std::string kPlaceElementProposeRefusedNote =
				"[UNAVAILABLE at --agent-autonomy=propose: place_element is not on the "
				"Propose-autonomy allowlist and is refused here exactly as under Read (relaunch with "
				"--agent-autonomy=commit to use it)] ";

			//! Arc 81 (2026-08-12): the clean-room LIGHTING verb's own
			//! annotation under AgentAutonomy::Propose SPECIFICALLY -- the
			//! sixth sibling of the notes above, same rationale (it mutates;
			//! deliberately excluded from AgentRpc.cpp's IsProposeSafeVerb
			//! rather than pay the "N mutating verbs" prose ripple
			//! SourceHygieneTest's verb-parity scan pins; refused under
			//! Propose exactly like Read; deliberately contains neither magic
			//! substring the per-note counters key on).
			const std::string kLightSceneProposeRefusedNote =
				"[UNAVAILABLE at --agent-autonomy=propose: light_scene is not on the "
				"Propose-autonomy allowlist and is refused here exactly as under Read (relaunch with "
				"--agent-autonomy=commit to use it)] ";

			//! Arc 82 (2026-08-12): the SEVENTH sibling, same rationale and
			//! same shape -- populate_scene mutates (it inserts the
			//! standard_object chunks its pass returned) and is deliberately
			//! excluded from AgentRpc.cpp's IsProposeSafeVerb.
			const std::string kPopulateSceneProposeRefusedNote =
				"[UNAVAILABLE at --agent-autonomy=propose: populate_scene is not on the "
				"Propose-autonomy allowlist and is refused here exactly as under Read (relaunch with "
				"--agent-autonomy=commit to use it)] ";

			//! Arc 83 slices 5 and 6 (2026-08-13): the EIGHTH and NINTH
			//! siblings, same rationale and same shape.  environment_scene
			//! mutates (it inserts painter / function / medium chunks and
			//! appends the rasterizer chunk that carries its dome binding);
			//! frame_scene mutates (it patches or replaces the camera chunk).
			//! Both are deliberately excluded from AgentRpc.cpp's
			//! IsProposeSafeVerb.
			const std::string kEnvironmentSceneProposeRefusedNote =
				"[UNAVAILABLE at --agent-autonomy=propose: environment_scene is not on the "
				"Propose-autonomy allowlist and is refused here exactly as under Read (relaunch with "
				"--agent-autonomy=commit to use it)] ";
			const std::string kFrameSceneProposeRefusedNote =
				"[UNAVAILABLE at --agent-autonomy=propose: frame_scene is not on the "
				"Propose-autonomy allowlist and is refused here exactly as under Read (relaunch with "
				"--agent-autonomy=commit to use it)] ";

			//! Build the `tools/list` result: the 34 existing AgentRpc verbs,
			//! each carrying an inputSchema faithful to AgentRpc.cpp's ACTUAL
			//! parsing, and a description mined from AgentRpc.h's verb-doc
			//! comments for the gotchas an external MCP client needs (paired
			//! width/height, camera vector shapes, the async-refused-headless
			//! note, pinned semantics, samples clamp, the ODD/EVEN id-space
			//! split, the baseHeadVersion conflict protocol, retriable).
			//! Secure-MCP slice 2: under AgentAutonomy::Read, the 6
			//! mutating tools' descriptions are ANNOTATED (prefixed with
			//! kAutonomyReadNote) rather than hidden -- see the note's doc.
			//! Secure-MCP slice 5b: resolve_proposal is annotated (with the
			//! DISTINCT kResolveProposalOwnerOnlyNote) under BOTH Read and
			//! Propose -- it is refused at the dispatcher under either posture
			//! (see AgentRpc.h); the 6 mutating tools' kAutonomyReadNote
			//! annotation, by contrast, applies ONLY under Read (Propose lets
			//! them reach the session, which stages rather than refuses).
			//! Secure-MCP slice 5b fix round (P2-1): under AgentAutonomy::
			//! Propose, the SAME 6 mutating tools instead get the DISTINCT
			//! kAutonomyProposeNote -- they are not refused (readOnly is
			//! false), but they no longer commit directly either, so leaving
			//! their description bare (Commit-identical) would hide that from
			//! an external caller.  Read/Propose/Commit are mutually
			//! exclusive, so exactly one of {kAutonomyReadNote,
			//! kAutonomyProposeNote, no note} applies per tool per posture.
			JsonValue BuildToolsList( AgentAutonomy autonomy )
			{
				JsonValue tools = JsonValue::MakeArray();
				const bool readOnly = ( autonomy == AgentAutonomy::Read );
				const bool proposeOnly = ( autonomy == AgentAutonomy::Propose );
				// Secure-MCP slice 5b: resolve_proposal is refused (at the
				// dispatcher) under Read AND Propose -- only Commit lets it
				// through to AgentSession (whose OWN Owner-only gate is the
				// second, session-layer refusal for a non-Owner session that
				// somehow reaches it -- see AgentRpc.h's file header).
				const bool resolveProposalRefused =
					( autonomy == AgentAutonomy::Read || autonomy == AgentAutonomy::Propose );

				// read_document
				tools.push_back( MakeTool( "read_document",
					"Read the current scene head as canonical .RISEscene text, plus its "
					"optimistic-concurrency headVersion {uuid,revision}. Works with NO scene "
					"loaded (hasDocument:false, headVersion {0,0}) -- an agent starting from "
					"scratch calls this first. Pass the returned headVersion back as "
					"baseHeadVersion on any of the 6 mutating tools "
					"(propose_patch/propose_patches/insert_chunk/insert_chunks/remove_chunk/remove_chunks) "
					"to guard against editing a stale head.",
					ObjectProp( "", JsonValue::MakeObject(), std::vector<std::string>() ) ) );

				// read_schema
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "keywords", StringArrayProp( "PREFERRED when you need more than one: an array of chunk keywords (at most 24, counting 'keyword' if you also send it) fetched in ONE call. 'schema' is then an ARRAY POSITIONALLY ALIGNED with this one -- schema[i] is keywords[i], never reordered, never collapsed -- and each entry also names its own 'keyword'. An unrecognized keyword occupies its own slot as {keyword, error}. Batch the chunk kinds you have DECIDED to use (typically 4-8); a full 24-entry batch runs roughly 30-45 KB depending on which kinds." ) );
					props.set( "keyword", StringProp( "OPTIONAL single chunk keyword (e.g. \"sphere_geometry\"). Use 'keywords' instead when you want two or more." ) );
					props.set( "category", StringProp( "OPTIONAL chunk category (e.g. \"material\", \"geometry\", \"painter\" -- the texture system -- \"light\", \"rasterizer\") to CHEAPLY list that category's keywords + one-line descriptions -- discovery only, no parameters. Follow it with ONE 'keywords' batch. Ignored when 'keyword' or 'keywords' is supplied." ) );
					tools.push_back( MakeTool( "read_schema",
						"Read the JSON Schema for scene-file chunks. Pass 'keywords' (an array) to "
						"fetch SEVERAL chunk schemas in one call -- the default way to prepare for "
						"authoring, instead of a round-trip per chunk kind; 'keyword' for exactly "
						"one; 'category' to cheaply discover which chunk kinds exist; all three "
						"omitted for the WHOLE grammar (large). STATELESS -- works with no scene "
						"loaded. The descriptor registry IS the accepted-parameter set, so this "
						"schema can never drift from what the parser actually accepts.",
						ObjectProp( "", props, std::vector<std::string>() ) ) );
				}

				// read_skill
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "name", StringProp( "OPTIONAL bare skill name (no '/', '\\\\', or \"..\"). Omit for the INDEX of all available skills ({name,title,hook} each); a name fetches that skill's full markdown." ) );
					tools.push_back( MakeTool( "read_skill",
						"Progressive-disclosure scene-authoring skills. STATELESS, like read_schema "
						"-- works with no scene loaded. Omit 'name' to list the index; pass a listed "
						"name to fetch its markdown. An unrecognized or unsafe (path-traversal) name "
						"is rejected. (The listing form is genuinely useful HERE: an external MCP "
						"client has no RISE system prompt and so has no index until it asks. The "
						"in-app chat transport is the opposite case -- it injects the index into its "
						"prompt, and its own tool description says to skip the listing call.)",
						ObjectProp( "", props, std::vector<std::string>() ) ) );
				}

				// validate
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "text", StringProp( "OPTIONAL candidate scene text (a full .RISEscene document) to validate INSTEAD of the current scene. Omit it to validate the current scene -- do NOT re-send the document you just edited." ) );
					tools.push_back( MakeTool( "validate",
						"Validate a scene with NO side effects, returning structured diagnostics "
						"{severity,code,message,offset,length}. Call it with NO ARGUMENTS to check "
						"the CURRENT scene -- that is the normal way to verify your own edit, and "
						"it also returns the headVersion it validated. Do NOT re-send the document "
						"you just edited: the engine already has it, and echoing a whole scene back "
						"costs thousands of output tokens for nothing. Pass 'text' only to check a "
						"CANDIDATE document you have not applied (e.g. a from-scratch scene you are "
						"composing); that form is STATELESS and works with no scene loaded, while "
						"the no-argument form needs a loaded scene. An empty diagnostics array "
						"means no SEMANTIC errors were found -- it does not check for the "
						"'RISE ASCII SCENE 7' header a candidate still needs in order to load. "
						"The result's 'validated' field ('head' or 'text') says which document "
						"was actually checked. NOTE under a propose-only posture: an edit that "
						"came back status='staged' is NOT in the head yet, so the no-argument "
						"form will not see it.",
						ObjectProp( "", props, std::vector<std::string>() ) ) );
				}

				// propose_patch
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "target", StringProp( "The entity NAME to edit (a chunk's `name` param). For a chunk with NO name -- the sole camera, the film, the rasterizer -- leave this EMPTY and pass `kind` instead (the kind-addressed singleton form). Passing the chunk KEYWORD here as if it were a name (e.g. \"pinhole_camera\") does not resolve and is rejected." ) );
					props.set( "kind",   StringProp( "OPTIONAL entity KIND keyword (e.g. \"material\", \"sphere_geometry\", \"camera\") to disambiguate a cross-category name clash." ) );
					props.set( "param",  StringProp( "The parameter role to set (e.g. \"radius\", \"reflectance\", \"location\")." ) );
					props.set( "value",  StringProp( "The new value, as a string (parsed per the parameter's declared kind by the derive layer)." ) );
					props.set( "baseHeadVersion", BaseHeadVersionSchema() );
					std::vector<std::string> required;
					required.push_back( "target" ); required.push_back( "param" ); required.push_back( "value" );
					const std::string desc = ( readOnly ? kAutonomyReadNote : proposeOnly ? kAutonomyProposeNote : std::string() ) + std::string(
						"Set one parameter on one named entity in the retained scene document. "
						"REQUIRES a scene to be loaded. Returns {applied,rawCode,status,retriable,"
						"headVersion,message}: applied is true ONLY for a clean apply; status is "
						"the authoritative gate, one of \"applied\" (clean success), \"rejected\" "
						"(refused, head byte-identical), \"diagnosed\" (the document WAS mutated but "
						"the full re-derive emitted diagnostics -- treat as FAILURE, not success), "
						"\"conflict\" (a stale baseHeadVersion precondition -- head untouched; "
						"re-read read_document and retry against the new headVersion), or \"staged\" "
						"(this session's authority does not commit directly -- the edit was queued "
						"for a human owner to approve/reject via resolve_proposal; poll "
						"list_proposals to see it move from pending to applied/rejected/conflict). "
						"retriable is meaningful only for status=\"rejected\": true means the refusal "
						"is TRANSIENT (e.g. an open editor transaction in a live GUI session) and "
						"resubmitting the identical patch later can succeed; false means retrying "
						"verbatim can never succeed. headVersion is always the head AFTER this call. "
						"A REJECTED patch may carry `issues`: [{param,value,reason,suggestions:[...]}] "
						"-- the SAME shape insert_chunk/remove_chunk return. `reason` is one of "
						"\"unknown_target\" (the `target` name does not resolve to any chunk -- "
						"`suggestions` lists near-miss candidate names, restricted to `kind` when "
						"given), \"unknown_param\" (`param` is not declared on the target's chunk "
						"type -- `message` also lists every valid parameter name), "
						"\"numeric_in_reference_slot\" (this slot needs the NAME of another chunk, "
						"not a literal number), \"unresolved_reference\" (`value` names a chunk not "
						"defined anywhere in the document -- `suggestions` lists near-miss candidate "
						"names already defined), or \"invalid_value\" (`value` is ill-typed for the "
						"parameter's kind -- a non-numeric token in a Double/vector slot, or a string "
						"outside an Enum's declared set; `message` then lists the allowed values). An "
						"EMPTY or ABSENT `issues` on a rejection does not mean the patch was fine; it "
						"means this pass could not statically pin the cause, and `message` still "
						"carries the engine's own diagnostic. "
						"A rasterizer chunk's own parameters are freely editable through this verb -- "
						"including on a bdpt/mlt/auto rasterizer the USER authored -- with exactly one "
						"exception: pinning an `auto_rasterizer`'s or `auto_spectral_rasterizer`'s "
						"`integrator` to `bdpt` is refused, because that IS selecting BDPT (`pt`, "
						"`vcm` and `auto` are all fine). See insert_chunk for the rasterizer "
						"allowlist this belongs to." );
					tools.push_back( MakeTool( "propose_patch", desc, ObjectProp( "", props, required ) ) );
				}

				// propose_patches -- BATCH form of propose_patch
				{
					JsonValue itemProps = JsonValue::MakeObject();
					itemProps.set( "target", StringProp( "The entity NAME to edit." ) );
					itemProps.set( "kind",   StringProp( "OPTIONAL entity KIND keyword to disambiguate a name clash." ) );
					itemProps.set( "param",  StringProp( "The parameter role to set." ) );
					itemProps.set( "value",  StringProp( "The new value string." ) );
					std::vector<std::string> itemRequired;
					itemRequired.push_back( "target" ); itemRequired.push_back( "param" ); itemRequired.push_back( "value" );

					JsonValue props = JsonValue::MakeObject();
					JsonValue itemSchema = ObjectProp( "A single patch edit ({target,param,value,kind?}).", itemProps, itemRequired );
					JsonValue patchesArrSchema = JsonValue::MakeObject();
					patchesArrSchema.set( "type", JsonValue::MakeString( "array" ) );
					patchesArrSchema.set( "items", itemSchema );
					patchesArrSchema.set( "description", JsonValue::MakeString( "Array of patch objects ({target,param,value,kind?}), applied in order." ) );
					props.set( "patches", patchesArrSchema );
					props.set( "baseHeadVersion", BaseHeadVersionSchema() );

					std::vector<std::string> required;
					required.push_back( "patches" );
					const std::string desc = ( readOnly ? kAutonomyReadNote : proposeOnly ? kAutonomyProposeNote : std::string() ) + std::string(
						"Set MULTIPLE parameters across one or several named entities in the scene document IN ONE CALL, applied in "
						"array order. USE THIS instead of many separate propose_patch calls when adjusting multiple parameters or "
						"entities together -- it is ONE round-trip instead of N. SEQUENTIAL and BEST-EFFORT: a rejected patch element "
						"does NOT stop the batch -- every remaining element is still attempted in order. `baseHeadVersion`, when given, "
						"is checked against the FIRST element only; if it is STALE the whole batch stops with every element "
						"status=\"conflict\" and nothing applied (re-read the document and resubmit). Returns {applied,total,results:[...]}: `total` is patches.size(), "
						"`applied` is how many results have applied=true, and each `results[i]` is the EXACT same shape propose_patch returns." );
					tools.push_back( MakeTool( "propose_patches", desc, ObjectProp( "", props, required ) ) );
				}

				// insert_chunk
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "chunkText", StringProp( "Exactly ONE complete chunk -- a `keyword { ... }` block with braces on their own lines -- to add to the scene. Headers, directives, and multi-chunk text are rejected." ) );
					props.set( "baseHeadVersion", BaseHeadVersionSchema() );
					std::vector<std::string> required; required.push_back( "chunkText" );
					const std::string desc = ( readOnly ? kAutonomyReadNote : proposeOnly ? kAutonomyProposeNote : std::string() ) + std::string(
						"Add one complete chunk to the scene document and realize it via a "
						"dry-run-guarded full re-derive (a failed dry-run leaves the document AND "
						"the live scene byte-identical -- no half-applied state). REQUIRES a scene "
						"to be loaded. Same result gating as propose_patch ({applied,rawCode,"
						"status,retriable,headVersion,message}, including the \"staged\" status -- "
						"see propose_patch's description) plus the parsed chunk's `name`/`kind` "
						"echo. A duplicate (kind,name) against an existing chunk is rejected "
						"with a clean message. EITHER a successful OR a rejected insert may "
						"carry `issues`: [{param,value,reason,suggestions:[...]}] -- `reason` is "
						"one of \"unresolved_reference\" (the value names a chunk not defined "
						"ANYWHERE in the document -- fine if you are about to insert that missing "
						"chunk next, a legitimate forward reference; otherwise insert the missing "
						"chunk or correct the misspelled name), \"unknown_param\" (the param name "
						"is not declared on this chunk type -- `message` also lists every valid "
						"parameter name), \"numeric_in_reference_slot\" (this slot needs the NAME "
						"of another chunk, not a literal number -- define the referenced chunk and "
						"pass its name instead), or \"unknown_chunk_type\" (the keyword itself is "
						"not a registered chunk type). On a SUCCESSFUL insert (applied=true) "
						"`issues` is always \"unresolved_reference\" warnings and does NOT change "
						"applied/status. On a REJECTED insert `issues` explains the cause "
						"in detail (`suggestions` lists near-miss candidate names, best match "
						"first) -- but an EMPTY `issues` on a rejection does not mean the chunk was "
						"fine; it means this pass could not statically pin the cause, and `message` "
						"still carries the engine's own diagnostic. "
						"A rasterizer chunk inserted this way becomes the ACTIVE integrator, and "
						"RASTERIZER SELECTION IS ALLOWLISTED on this surface: you may insert "
						"`pathtracing_pel_rasterizer`, `pathtracing_spectral_rasterizer`, "
						"`vcm_pel_rasterizer` or `vcm_spectral_rasterizer` -- PT for general scenes, "
						"VCM for caustic/refractive/dispersive transport. `bdpt_pel_rasterizer`, "
						"`bdpt_spectral_rasterizer`, `mlt_rasterizer`, `mlt_spectral_rasterizer`, "
						"`auto_rasterizer` and `auto_spectral_rasterizer` are SPECIALIZED and are "
						"refused, with no override parameter -- only the USER selects those (in the "
						"GUI, or by authoring the chunk into the scene file). The utility rasterizers "
						"`pixelpel_rasterizer` and `pixelintegratingspectral_rasterizer` are NOT "
						"gated. A scene that ALREADY contains a blocked rasterizer stays fully "
						"editable, including that chunk's own parameters." );
					tools.push_back( MakeTool( "insert_chunk", desc, ObjectProp( "", props, required ) ) );
				}

				// insert_chunks -- BATCH form of insert_chunk
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "chunks", StringArrayProp( "An array of complete chunks, each a `keyword { ... }` block with braces on their own lines -- SAME grammar as insert_chunk's `chunkText`, one chunk per array element (not multiple chunks concatenated into one element). Order matters: a chunk that is REFERENCED by a later chunk must appear BEFORE it in this array, exactly like the ordering rule across separate insert_chunk calls." ) );
					props.set( "baseHeadVersion", BaseHeadVersionSchema() );
					std::vector<std::string> required; required.push_back( "chunks" );
					const std::string desc = ( readOnly ? kAutonomyReadNote : proposeOnly ? kAutonomyProposeNote : std::string() ) + std::string(
						"Add SEVERAL complete chunks to the scene document IN ONE CALL, applied in "
						"the given array order. USE THIS instead of many separate insert_chunk calls "
						"when adding a coherent group of chunks -- e.g. a painter plus the material "
						"that references it, a geometry, and the object that binds them together, or "
						"a whole lighting rig -- it is ONE round-trip instead of N. Each chunk is "
						"realized via the SAME dry-run-guarded full re-derive insert_chunk uses (a "
						"failed dry-run for one element leaves the document AND the live scene byte-"
						"identical for THAT element -- no half-applied state for it). Order matters: "
						"a chunk that is referenced by a later one in this array must appear BEFORE "
						"it, same rule as across separate insert_chunk calls -- e.g. a painter chunk "
						"at index 0 followed by a material chunk at index 1 that names it resolves "
						"cleanly, because index 0 has already landed by the time index 1 is applied. "
						"SEQUENTIAL and BEST-EFFORT: a REJECTED chunk does NOT stop the batch -- every "
						"remaining element is still attempted in order (a later chunk that depended on "
						"a rejected earlier one will simply also fail, with its own actionable "
						"`issues`, rather than being silently skipped). `baseHeadVersion`, when given, "
						"is the optimistic-concurrency precondition for the BATCH AS A WHOLE, checked "
						"against the FIRST element only -- later elements apply against the head as "
						"this batch has evolved it so far. Returns {applied,total,results:[...]}: "
						"`total` is chunks.size(), `applied` is how many of `results` have "
						"applied=true, and each `results[i]` is the EXACT same shape insert_chunk "
						"returns for `chunks[i]` -- {applied,rawCode,status,retriable,headVersion,"
						"message,name,kind,issues?} -- including insert_chunk's `issues` reasons "
						"(\"unresolved_reference\", \"unknown_param\", \"numeric_in_reference_slot\", "
						"\"unknown_chunk_type\") and its forward-reference-warning-on-success "
						"behaviour. Check every element's own status; do not assume the whole batch "
						"succeeded just because the call itself returned. REQUIRES a scene to be "
						"loaded; `chunks` must be a non-empty array of strings. "
						"The rasterizer allowlist insert_chunk documents applies here too, and it is "
						"the ONE refusal that is NOT best-effort: a blocked rasterizer anywhere in "
						"the array refuses the WHOLE batch atomically -- nothing is inserted and the "
						"head version does not move." );
					tools.push_back( MakeTool( "insert_chunks", desc, ObjectProp( "", props, required ) ) );
				}

				// insert_material_scaffold (Arc-75 slice S2.1)
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "family", StringProp(
						"One of: weathered_wood, rough_stone, brushed_metal, aged_bronze, glazed_ceramic." ) );
					props.set( "name", StringProp(
						"A fresh, unique prefix for this expansion (letters/digits/underscore/hyphen). Every generated "
						"chunk is named tmpl_<name>_<role>, e.g. tmpl_desk1_mat, tmpl_desk1_grain." ) );
					props.set( "tone", StringProp(
						"Base colour as \"r g b\", each 0..1, e.g. \"0.55 0.42 0.30\"." ) );
					props.set( "wear", NumberProp( "Variation intensity, 0..1." ) );
					props.set( "scale", NumberProp( "Spatial frequency of the variation, > 0 (larger = tighter/finer features)." ) );
					props.set( "baseHeadVersion", BaseHeadVersionSchema() );
					std::vector<std::string> required;
					required.push_back( "family" ); required.push_back( "name" ); required.push_back( "tone" );
					required.push_back( "wear" );   required.push_back( "scale" );
					// insert_material_scaffold is Commit-only (see
					// kScaffoldProposeRefusedNote's doc) -- readOnly and
					// proposeOnly BOTH refuse it, but with different text
					// (kAutonomyReadNote is accurate under Read; the
					// dedicated propose note is accurate under Propose).
					const std::string desc = ( readOnly ? kAutonomyReadNote
					                          : proposeOnly ? kScaffoldProposeRefusedNote
					                          : std::string() ) + std::string(
						"Expand ONE of five material-family templates into a small wired painter graph "
						"(2-4 painters + 1 material) added to the scene in one call. Families: "
						"\"weathered_wood\" (pbr_metallic_roughness, base_color AND roughness both bound to a "
						"domainwarp3d wood-grain painter), \"rough_stone\" (cooktorrance, rd bound to a worley3d "
						"pebble field, facets bound to a spatially-varying scalar field), \"brushed_metal\" "
						"(ward_anisotropic, alphax/alphay both bound to a spatially-varying scalar field at "
						"different scale for the anisotropic groove), \"aged_bronze\" (cooktorrance, rd bound to "
						"a reactiondiffusion3d oxidation-patina field, facets bound to a spatially-varying "
						"scalar field), \"glazed_ceramic\" (ggx with fresnel_mode schlick_f0, alphax/alphay both "
						"bound to a LOW-amplitude spatially-varying scalar field). Every family binds AT LEAST "
						"ONE microsurface parameter to a real painter chunk. ALL FIVE params are REQUIRED, no "
						"defaults -- a missing one is a blocking error naming it. Internal graph constants are "
						"jittered deterministically from `name`: the SAME name reproduces byte-identical chunks, "
						"a DIFFERENT name visibly differs. Every generated chunk is an ORDINARY, EDITABLE "
						"document chunk named tmpl_<name>_<role> -- read_document shows them, and propose_patch/"
						"remove_chunk work on them exactly like any hand-authored chunk. Applied IN ORDER "
						"through the SAME batch machinery insert_chunks uses (SEQUENTIAL, BEST-EFFORT). Returns "
						"{applied,total,results:[...]} -- the EXACT insert_chunk per-element shape -- plus "
						"`material` ({name,kind} of the one material chunk) and `boundSlots` "
						"([{param,painter},...], purely factual). Check every element's own status. A missing/"
						"invalid param, an unrecognized family, or a NAME COLLISION refuses the WHOLE call "
						"before any chunk is generated (document unchanged) -- reported as a tool error, not a "
						"partial result. Always pass the headVersion you last read as baseHeadVersion." );
					tools.push_back( MakeTool( "insert_material_scaffold", desc, ObjectProp( "", props, required ) ) );
				}

				// file_build_plan (G2, 2026-08-10; S1 schema v3, 2026-08-11) --
				// READ-SAFE (no autonomy note of any kind: it is on
				// IsReadSafeVerb, so it dispatches under Read and Propose
				// exactly as under Commit).
				{
					JsonValue entryProps = JsonValue::MakeObject();
					entryProps.set( "element", StringProp(
						"Required. The name of this element, in your own words (e.g. \"wizard\", \"terrain\")." ) );
					// S1 (2026-08-11): the REQUIRED piece list.  Declarative --
					// nothing validates, gates or scores the names; finish_element
					// reports which of them appear in a created chunk's NAME.
					{
						JsonValue piecesProp = JsonValue::MakeObject();
						piecesProp.set( "type", JsonValue::MakeString( "array" ) );
						piecesProp.set( "minItems", JsonValue::MakeNumber( 1.0 ) );
						piecesProp.set( "maxItems", JsonValue::MakeNumber(
							static_cast<double>( AgentSession::kBuildPlanMaxPiecesPerElement ) ) );
						piecesProp.set( "items", StringProp( "" ) );
						piecesProp.set( "description", JsonValue::MakeString(
							"Required, at least one -- the pieces this element breaks down into, in your own "
							"words (e.g. [\"robe\",\"hat\",\"beard\",\"staff\"]). Any names are accepted and "
							"nothing checks what they say. They are a declaration, not a commitment: nothing "
							"requires a chunk per piece and no piece is separately gated." ) );
						entryProps.set( "pieces", piecesProp );
					}
					JsonValue construction = StringProp(
						"Required. Exactly one of: primitive, csg, sweep, chain, displaced, mesh." );
					{
						JsonValue enumArr = JsonValue::MakeArray();
						for( std::size_t i = 0; i < AgentSession::kBuildPlanConstructionCount; ++i )
							enumArr.push_back( JsonValue::MakeString( AgentSession::kBuildPlanConstructionValues[i] ) );
						construction.set( "enum", enumArr );
					}
					entryProps.set( "construction", construction );
					// G3a (2026-08-10): the REQUIRED outline and the OPTIONAL
					// view.  Kept semantically identical to the chat codec's
					// hand-authored copy of the same contract
					// (AgentChatCodecs.cpp kToolDefs) -- SourceHygieneTest's
					// G2/G3a parity scan is what pins the two together.
					entryProps.set( "outline", StringProp(
						"Required. A closed 2D outline of this element as semicolon-separated \"x y\" points, "
						"at least 3 of them -- e.g. \"0 0; 3 0.4; 4 1.6; 1.2 2.1; -0.3 1.1\". The last point "
						"joins back to the first automatically. Units and origin are yours; the shape is "
						"scaled to fit its own bounding box. Self-intersecting outlines are allowed (filled "
						"by the even-odd rule). Rejected only if it has fewer than 3 points, a point that is "
						"not two finite numbers, or all points on one horizontal or vertical line." ) );
					JsonValue view = StringProp(
						"Optional, default \"front\". Which axis-aligned direction this outline is drawn from." );
					{
						JsonValue viewEnum = JsonValue::MakeArray();
						for( std::size_t i = 0; i < AgentSession::kBuildPlanViewCount; ++i )
							viewEnum.push_back( JsonValue::MakeString( AgentSession::kBuildPlanViewValues[i] ) );
						view.set( "enum", viewEnum );
					}
					entryProps.set( "view", view );
					entryProps.set( "note", StringProp( "Optional free text about this element." ) );
					std::vector<std::string> entryRequired;
					entryRequired.push_back( "element" ); entryRequired.push_back( "pieces" );
					entryRequired.push_back( "construction" ); entryRequired.push_back( "outline" );

					JsonValue elementsProp = JsonValue::MakeObject();
					elementsProp.set( "type", JsonValue::MakeString( "array" ) );
					elementsProp.set( "minItems", JsonValue::MakeNumber( 1.0 ) );
					// G3a: the resource bound, machine-readable rather than
					// only enforced at the dispatcher.
					elementsProp.set( "maxItems", JsonValue::MakeNumber(
						static_cast<double>( AgentSession::kBuildPlanMaxElements ) ) );
					elementsProp.set( "items", ObjectProp( "", entryProps, entryRequired ) );
					elementsProp.set( "description", JsonValue::MakeString(
						"Required, at least one entry -- the elements of the scene you are about to build." ) );

					JsonValue props = JsonValue::MakeObject();
					props.set( "elements", elementsProp );
					std::vector<std::string> required;
					required.push_back( "elements" );

					const std::string desc =
						"File the build plan for the scene you are building: one entry per ELEMENT, each broken "
						"into named PIECES, each naming how it will be constructed AND carrying a 2D outline "
						"sketch of it. On a session "
						"that has not filed one, EVERY call that "
						"creates geometry (insert_chunk/insert_chunks carrying a geometry chunk, "
						"insert_geometry_scaffold, replace_geometry_scaffold) is refused and names this "
						"tool -- up to 3 refusals; the 4th such call is let through and the gate stops "
						"intercepting for the rest of the session. `pieces` is REQUIRED per element: at least "
						"one name, in your own words; any names are accepted and nothing checks them. "
						"`construction` is REQUIRED per element and "
						"must be one of exactly: \"primitive\" (a single built-in shape chunk -- balls, "
						"crates, poles, rings; the blockout default), \"csg\" (a csg_object or an "
						"sdf_geometry combining shapes with boolean ops -- union/subtract/intersect: drilled "
						"bores, clipped tapers, lenses), \"sweep\" (a sweep_geometry profile swept along a "
						"path -- tentacles, stems, handles; taper and closed loops), \"chain\" "
						"(a skeleton_geometry joint graph, or sdf_geometry parts blended with smin, into "
						"one continuous form -- creature bodies, limbs, spines, necks, vessel bodies; a "
						"chunk mixing smin AND booleans is \"chain\" -- the blend characterises the form, "
						"booleans are trim), \"displaced\" (a "
						"displaced_geometry driven by a painter -- terrain, bark, rock faces), \"mesh\" (a "
						"triangle-mesh chunk -- imported "
						"or authored assets). `outline` is REQUIRED per element: a closed 2D polygon of at "
						"least 3 \"x y\" "
						"points separated by semicolons, in any units you like (it is scaled to fit its own "
						"bounding box). Any shape is accepted, a rough blob included. Each outline is drawn "
						"into a 256x256 silhouette and returned to you as one tiled image alongside its point "
						"count, filled-area fraction and aspect ratio, so you can see what you sketched. Any "
						"construction answer is accepted, including \"primitive\" for every element. The plan "
						"is NOT binding: declaring one construction and then authoring a different chunk "
						"kind is allowed and is never refused, and no piece has to become a chunk. "
						"FILING STARTS THE BUILD: the session enters the PIECES phase with the FIRST element "
						"active, every chunk created while an element is active is recorded against it, "
						"finish_element closes it and moves to the next, and after the last one the session "
						"enters the COMPOSE phase. An edit aimed at a chunk recorded against a DIFFERENT "
						"element is refused (up to 3 times, then the phase rules stop intercepting); chunks "
						"with no element recorded against them, and light, camera, film, rasterizer, "
						"rasterizer-output, material and painter chunks, are editable in every phase. "
						"Filing does not change the document -- no chunk, no head "
						"version, no undo step -- so there is no baseHeadVersion and no conflict outcome. "
						"Returns {filed,replacedPreviousPlan,elementCount,elements:[{element,pieces,"
						"construction,note,outline,view,pointCount,areaFraction,aspect}],phase,activeElement,"
						"png_base64,compositeWidth,compositeHeight,message} "
						"-- the composite sketch image also rides back as an MCP image content block. "
						"Calling it again replaces the previous plan, every sketch filed with it, and every "
						"chunk-to-element record, and makes the first element of the new plan active.";
					tools.push_back( MakeTool( "file_build_plan", desc, ObjectProp( "", props, required ) ) );
				}

				// finish_element / reopen_element (S1, 2026-08-11) -- READ-SAFE
				// (both on IsReadSafeVerb; no autonomy note).  THE CODEC TEXT IS
				// CANONICAL AND THIS MIRRORS IT, for the drift-class reason
				// recorded on file_build_plan above.
				{
					JsonValue props = JsonValue::MakeObject();
					std::vector<std::string> required;
					const std::string desc =
						"Close the element you are working on and move to the next one. Takes no arguments -- "
						"it closes whichever element is currently active. Returns the facts of that element: "
						"the chunks recorded against it, which of its declared pieces have a chunk whose NAME "
						"contains that piece name (a case-insensitive substring check on names only -- it says "
						"nothing about what was built or how well), the phase the session is now in, and the "
						"next active element. It also returns an ISOLATE RENDER of the element -- the object "
						"recorded against it, alone in the frame and auto-framed; when several objects were "
						"recorded, the one with the largest bounding box, and the message says so. After the "
						"LAST element it enters the COMPOSE phase, where arrangement, lighting, camera, "
						"materials and edits to any element's chunks are allowed and creating new geometry is "
						"refused. Nothing is required of an element before you finish it, and nothing is "
						"scored. It changes nothing in the document -- no chunk, no head version, no undo "
						"step. ok:false means the call did nothing (no build plan filed, or every element is "
						"already finished) and message says which. Returns {ok,element,phase,nextElement,"
						"chunks,piecesNamed,piecesNotNamed,isolate,isolateCandidates,rendered,png_base64,"
						"message} -- the isolate render also rides back as an MCP image content block.";
					tools.push_back( MakeTool( "finish_element", desc, ObjectProp( "", props, required ) ) );
				}
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "element", StringProp(
						"Required. The name of an element in the filed build plan, exactly as you filed it." ) );
					std::vector<std::string> required;
					required.push_back( "element" );
					const std::string desc =
						"Go back to an element you already finished, or switch to a different one. The named "
						"element becomes active, the session returns to the PIECES phase, and chunks you "
						"create from then on are recorded against it. Legal from any phase, including "
						"compose; it is never refused and never counts against any refusal cap. It is the "
						"way out of the compose phase's refusal to create new geometry. It changes nothing in the "
						"document -- no chunk, no head version, no undo step. ok:false means the call did "
						"nothing (no build plan filed, or that name is not in it, in which case the message "
						"lists the names that are). Returns {ok,element,phase,previousPhase,chunks,"
						"unfinished,message}.";
					tools.push_back( MakeTool( "reopen_element", desc, ObjectProp( "", props, required ) ) );
				}

				// build_element / place_element (S2, 2026-08-11) -- the two
				// CLEAN-ROOM verbs.  Both MUTATE (build_element inserts what the
				// builder returned; place_element submits one patch batch), so
				// neither is read-safe; both are additionally excluded from
				// AgentRpc's Propose-autonomy allowlist, exactly like the three
				// scaffold verbs, and carry the same kScaffoldProposeRefusedNote
				// caveat on this surface.  THE CODEC TEXT IS CANONICAL AND THIS
				// MIRRORS IT, for the drift-class reason recorded on
				// file_build_plan above.
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "element", StringProp(
						"Required. The name of the ACTIVE element, exactly as filed in file_build_plan. "
						"A name that is not the active element does nothing and says which element is "
						"active." ) );
					JsonValue heightProp = JsonValue::MakeObject();
					heightProp.set( "type", JsonValue::MakeString( "number" ) );
					heightProp.set( "description", JsonValue::MakeString(
						"Required. How tall the element should be in world units (its Y extent). Must "
						"be greater than 0. A request, not a limit -- the realised bounding box comes "
						"back in the result and nothing is refused for missing it." ) );
					props.set( "height", heightProp );
					props.set( "notes", StringProp(
						"Optional free text passed to the builder alongside the pieces and the outline "
						"-- anything about this element the plan does not already say. Nothing checks "
						"what it says." ) );
					std::vector<std::string> required;
					required.push_back( "element" );
					required.push_back( "height" );

					// readOnly and proposeOnly are BOTH refusals for this verb,
					// with different truthful wording (see
					// kBuildElementProposeRefusedNote's doc).
					const std::string desc =
						( readOnly ? kAutonomyReadNote
						            : proposeOnly ? kBuildElementProposeRefusedNote
						                          : std::string() ) +
						std::string(
						"Build the ACTIVE element in one go: this call asks the session's provider, in "
						"a FRESH context that contains nothing but the scene-language grammar for the "
						"chunk kinds involved, the pieces declared for this element, the outline "
						"sketched for it, the requested height and a fixed local-frame contract, to "
						"construct the whole element on its own. What comes back is split into chunks, "
						"checked, and inserted here. It is the way the FIRST geometry for an element "
						"gets made: while the active element has no chunk recorded against it, "
						"insert_chunk and insert_chunks carrying a geometry chunk are refused and name "
						"this tool -- up to 3 refusals shared with the other build-phase rules, after "
						"which they stop intercepting. Once the element has any chunk recorded against "
						"it, authoring geometry for it by hand is allowed and is never refused again. "
						"Only geometry chunks are ever refused this way; materials, painters and "
						"standard_objects never are. THE ELEMENT IS BUILT AT THE ORIGIN, NOT IN PLACE: "
						"the contract sent to the builder puts the element's base-centre at (0,0,0) "
						"with +Y up, facing +Z -- place_element is what moves it into the scene "
						"afterwards. `height` is a REQUEST, not a limit: the realised bounding box is "
						"measured and reported back, and nothing is refused for missing it. EVERY "
						"CHUNK NAME MUST BEGIN with the element's prefix (the element name lowercased, "
						"non-alphanumerics collapsed to underscores, plus a trailing underscore -- "
						"\"wizard\" gives \"wizard_\"); a chunk whose name does not begin with the "
						"required prefix is rejected and is NOT renamed, because renaming would break "
						"the references between the builder's own chunks. If anything is rejected, ONE "
						"repair retry runs automatically with the exact rejection text -- one, then it "
						"stops, and whatever landed stays landed. Nothing is ever dropped silently: a "
						"chunk that came back unclosed, unnamed, wrongly prefixed or rejected by "
						"insertion is reported with its reason. Everything that lands is recorded "
						"against the active element exactly as if it had been inserted directly. On a "
						"provider that cannot run a separate completion this returns ok:false with a "
						"plain statement, nothing else changes, and authoring by hand is not blocked. "
						"Returns {ok,element,provider,model,chunksExtracted,landed,"
						"rejected:[{name,kind,reason}],chunkResults,retryRan,retrySucceeded,"
						"sdfPartCount,bbox:{min,max,height},message}." );
					tools.push_back( MakeTool( "build_element", desc, ObjectProp( "", props, required ) ) );
				}
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "element", StringProp(
						"Required. The name of an element in the filed build plan, exactly as you "
						"filed it." ) );
					// S2 fix-round (2026-08-11, P2): this SHORT line used to say only
					// "where the base-centre goes", which reads as an absolute world
					// coordinate; the OFFSET/compose semantics were stated only in the
					// fuller tool description below.  Now unmistakable here too --
					// mirrors AgentChatCodecs.cpp's identical fix verbatim.
					props.set( "position", StringProp(
						"Required. An OFFSET added to each object's own CURRENT (already scaled/rotated) "
						"position, as \"x y z\" -- equals the base-centre's world position on a first "
						"call from the origin, but a later call composes on top of wherever the element "
						"already is rather than resetting it there." ) );
					JsonValue scaleProp = JsonValue::MakeObject();
					scaleProp.set( "type", JsonValue::MakeString( "number" ) );
					scaleProp.set( "description", JsonValue::MakeString(
						"Optional, default 1. ONE uniform factor -- not three. Must be greater than 0. "
						"It multiplies each object's scale and its offset from the element's origin." ) );
					props.set( "scale", scaleProp );
					props.set( "orientation", StringProp(
						"Optional, default \"0 0 0\". Euler degrees about the element's own origin, as "
						"\"ex ey ez\"." ) );
					std::vector<std::string> required;
					required.push_back( "element" );
					required.push_back( "position" );

					const std::string desc =
						( readOnly ? kAutonomyReadNote
						            : proposeOnly ? kPlaceElementProposeRefusedNote
						                          : std::string() ) +
						std::string(
						"Move an element into the scene: one rigid transform applied to every "
						"standard_object recorded against it, so an element built at the origin ends "
						"up where you want it without patching each object. `position` is where the "
						"element's base-centre goes, and it is an OFFSET -- each object's own position "
						"inside the element is scaled, rotated and then added to it, so the objects "
						"keep their arrangement relative to one another. `scale` is one uniform factor "
						"multiplying each object's scale and its offset from the element's origin, so "
						"the element scales about its own base-centre. `orientation` is Euler degrees "
						"about that same origin; an object that carries no rotation of its own is set "
						"to it exactly, and one that already carries a rotation has the degrees added "
						"per axis, which the result reports because adding Euler angles is only exact "
						"when both rotations are about the same axis. An object authored with `matrix` "
						"is skipped and named, because a matrix overrides position, orientation and "
						"scale and the patch would do nothing; an object authored with `quaternion` is "
						"moved and scaled but not rotated, and is named too. Legal in the pieces phase "
						"and in the compose phase. The whole placement is ONE batch, so ONE head "
						"version bump and ONE undo step. ok:false means nothing was submitted (no "
						"build plan, an element that is not in it, or an element with no "
						"standard_object recorded against it) and the document is unchanged. Returns "
						"{ok,element,objects,skipped:[{object,reason}],patchResults,patchesApplied,"
						"patchesRejected,bbox:{min,max},message}." );
					tools.push_back( MakeTool( "place_element", desc, ObjectProp( "", props, required ) ) );
				}

				// light_scene (Arc 81, 2026-08-12) -- the clean-room LIGHTING
				// verb.  It MUTATES (it inserts what its pass returned), so it
				// is not read-safe, and it is additionally excluded from
				// AgentRpc's Propose-autonomy allowlist exactly like the two
				// clean-room verbs above, carrying the matching note.  THE
				// CODEC TEXT IS CANONICAL AND THIS MIRRORS IT, for the
				// drift-class reason recorded on file_build_plan above.
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "notes", StringProp(
						"Optional free text passed to the lighting pass alongside the inventory and "
						"the camera -- anything about the mood or the look this scene's own state does "
						"not already say. Nothing checks what it says." ) );
					const std::vector<std::string> required;   // no required params

					const std::string desc =
						( readOnly ? kAutonomyReadNote
						            : proposeOnly ? kLightSceneProposeRefusedNote
						                          : std::string() ) +
						std::string(
						"Design the whole scene's lighting: this call first answers WHAT IN THIS WORLD "
						"PHYSICALLY EMITS LIGHT for this scene in one request -- the sun or sky, an "
						"opening light arrives through, a fixture or lamp, or something that itself "
						"glows; things in the world, never a renderer light kind -- then authors the "
						"chunks for ALL of those sources together in one further request. Both requests "
						"run in their own separate FRESH context that contains nothing but this scene's "
						"object inventory (every object, its screen footprint and its world position), "
						"the camera, the scene's world bounds, the imagined description if the session has "
						"one, and the lights that already exist; the second request additionally gets "
						"the light palette with its grammar. At most 16 sources; a longer enumeration is "
						"cut to its first 16 and the result says so. For the model this is still ONE call "
						"and ONE result: both requests run here, so its turn count does not grow with "
						"the lighting. What comes "
						"back is split into chunks, checked, and inserted here. THE PALETTE LEADS "
						"WITH AREA LIGHTING and is the only entry carrying a complete worked "
						"example. A rectangular area light is ONE chunk, rect_light, taking center, "
						"size, facing, color and exitance. A glowing SOLID is also ONE chunk, "
						"shape_light, taking shape (sphere, ellipsoid, box or cylinder), center, "
						"size, orientation, color and exitance -- a closed solid emits outward in "
						"every direction, so it needs no facing. The parser expands either into the "
						"general form, which is what any other shape of emitter needs -- an "
						"ordinary object wearing an emissive lambertian_luminaire_material, which is "
						"a painter plus that material plus a geometry plus a standard_object -- and "
						"being an ordinary object, it is rendered like one: the camera sees its "
						"surface, so it is both a light and a thing the picture shows. Then "
						"hosek_wilkie_skylight (a physically based analytic sun-and-sky). Then "
						"omni_light, spot_light and directional_light with their parameters and no "
						"example -- they are zero-area idealizations whose shadows have no penumbra, "
						"for special cases. Each request to insert one is refused once with the facts "
						"and the alternatives, and the identical request after that refusal is applied. "
						"shape_light, rect_light, an emissive object and hosek_wilkie_skylight are "
						"never subject to this at all. ambient_light is NOT offered and is REFUSED on every "
						"path that could create one, in every phase and with the build protocol off: "
						"it contributes the same color * power at every shading point, has no "
						"position or direction, and casts no shadow ray, so it can produce neither a "
						"shadow nor any falloff. It is the way the FIRST lighting for a "
						"composed scene gets made: in the COMPOSE phase, while light_scene has not "
						"run, insert_chunk and insert_chunks carrying a light chunk are refused and "
						"name this tool -- up to 3 refusals shared with the other build-phase rules, "
						"after which they stop intercepting. Once light_scene has run, authoring "
						"lights by hand is allowed and is never refused again; editing a light that "
						"already exists is never refused at all, and lights created inside an element "
						"window in the pieces phase are not affected by any of this. THERE IS NO NAME "
						"PREFIX -- lights are scene-global and belong to no element -- but a name "
						"already used in the scene is rejected and NOT renamed, because renaming would "
						"break the references between the pass's own chunks. This call only ADDS: it "
						"never removes a light, and it will not accept a camera, a film, a rasterizer "
						"or a shader op, nor geometry unless the same answer defines an emissive "
						"material to put on it. If the build's answer rejects anything, ONE repair "
						"retry runs automatically with the exact rejection text over the WHOLE build, "
						"then it stops, and "
						"whatever landed stays landed. Nothing is ever dropped silently. The result "
						"then reports WHAT EACH LIGHT ACTUALLY DOES: every light, emissive object and "
						"environment in the scene is rendered ALONE at a small size and its frame's "
						"mean luma reported, against the same frame with all of them lit. That "
						"measurement is capped and the message says so when the cap applies; the "
						"figures do not sum to the all-lights frame, because light transport through "
						"this renderer is not additive. On a provider that cannot run a separate "
						"completion this returns ok:false with a plain statement, nothing else "
						"changes, and authoring lights by hand is not blocked. Returns "
						"{ok,provider,model,chunksExtracted,landed,rejected:[{name,kind,reason}],"
						"chunkResults,retryRan,retrySucceeded,soloableLights,soloed,"
						"allLightsMeanLuma,contributions:[{name,kind,soloed,meanLuma,share,reason}],"
						"sourcesPlanned,sourcesReturned,sourcesTruncated,completions,"
						"areaLights,zeroAreaLights,skyLights,otherLights,"
						"sources:[string],"
						"message}." );
					tools.push_back( MakeTool( "light_scene", desc, ObjectProp( "", props, required ) ) );
				}

				// populate_scene (Arc 82, 2026-08-12) -- the clean-room
				// POPULATION verb.  It MUTATES (it inserts the standard_object
				// chunks its pass returned), so it is not read-safe, and it is
				// additionally excluded from AgentRpc's Propose-autonomy
				// allowlist exactly like light_scene, carrying the matching
				// note.  THE CODEC TEXT IS CANONICAL AND THIS MIRRORS IT, for
				// the drift-class reason recorded on file_build_plan above.
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "notes", StringProp(
						"Optional free text passed to the population pass alongside the inventory and "
						"the camera -- anything about what this scene is that its own state does not "
						"already say. Nothing checks what it says." ) );
					const std::vector<std::string> required;   // no required params

					const std::string desc =
						( readOnly ? kAutonomyReadNote
						            : proposeOnly ? kPopulateSceneProposeRefusedNote
						                          : std::string() ) +
						std::string(
						"Populate the scene: this call asks the session's provider, in a FRESH context "
						"that contains nothing but this scene's object inventory (every object, its "
						"screen footprint and its world position), the camera, the scene's world bounds, "
						"the imagined description if the session has one, and A LIST OF THE GEOMETRIES "
						"AND MATERIALS THIS SCENE ALREADY HAS with the objects currently using each, to "
						"place more objects. What comes back is split into chunks, checked, and inserted "
						"here. IT CREATES standard_object CHUNKS AND NOTHING ELSE: no geometry, no "
						"material, no painter, no light, no camera. Each one must name a geometry and a "
						"material THAT ALREADY EXIST -- a chunk naming an unknown geometry or material "
						"is rejected with that reason, and a chunk of any other kind is rejected too. "
						"Making new form is build_element's job, not this one's. The prompt carries ONE "
						"worked example, built from this scene's own geometry and material names so it "
						"is literally insertable here. It is the way the FIRST compose-phase render gets "
						"earned: in the COMPOSE phase, while populate_scene has not run, the first "
						"full-scene render is refused ONCE and names this tool -- one refusal, shared "
						"with the other build-phase rules and capped with them, after which every render "
						"proceeds whether or not this ran; renders in the pieces phase are never "
						"refused, and neither is a render with `isolate`. A name already used in the "
						"scene is rejected and NOT renamed. If anything is rejected, ONE repair retry "
						"runs automatically with the exact rejection text -- one, then it stops, and "
						"whatever landed stays landed. Nothing is ever dropped silently. The result "
						"reports which objects were created and what geometry and material each one "
						"repeats, what was rejected and why, and the scene's object count before and "
						"after. On a provider that cannot run a separate completion this returns "
						"ok:false with a plain statement, nothing else changes, and authoring "
						"standard_object chunks by hand is not blocked. A scene with no standard_object "
						"naming both a geometry and a material has nothing to repeat, and this returns "
						"ok:false saying so without calling the provider. Returns {ok,provider,model,"
						"chunksExtracted,created:[{name,geometry,material}],"
						"rejected:[{name,kind,reason}],chunkResults,retryRan,retrySucceeded,"
						"objectsBefore,objectsAfter,message}." );
					tools.push_back( MakeTool( "populate_scene", desc, ObjectProp( "", props, required ) ) );
				}

				// environment_scene (Arc 83 slice 5, 2026-08-13) -- the
				// clean-room ENVIRONMENT verb.  It MUTATES (it inserts painter /
				// function / medium chunks and appends the rasterizer chunk
				// carrying its dome binding), so it is not read-safe, and it is
				// additionally excluded from AgentRpc's Propose-autonomy
				// allowlist exactly like light_scene, carrying the matching
				// note.  THE CODEC TEXT IS CANONICAL AND THIS MIRRORS IT, for
				// the drift-class reason recorded on file_build_plan above.
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "notes", StringProp(
						"Optional free text passed to the environment pass alongside the inventory and "
						"the camera -- anything about the place or the weather this scene's own state "
						"does not already say. Nothing checks what it says." ) );
					const std::vector<std::string> required;   // no required params

					const std::string desc =
						( readOnly ? kAutonomyReadNote
						            : proposeOnly ? kEnvironmentSceneProposeRefusedNote
						                          : std::string() ) +
						std::string(
						"Author the scene's SURROUND: what fills the frame where no object is, and the "
						"medium light travels through. This call asks the session's provider, in a "
						"FRESH context that contains nothing but this scene's object inventory (every "
						"object, its screen footprint and its world position), the camera, the scene's "
						"world bounds, the lights that already exist, the imagined description if the "
						"session has one, and WHAT ENVIRONMENT THIS SCENE ALREADY HAS, to write the "
						"environment. It is ONE request, because there is one environment. What comes "
						"back is split into chunks, checked, and inserted here. THE PALETTE IS TWO "
						"FAMILIES. THE DOME is a PAINTER -- there is no environment chunk in this "
						"language -- evaluated at a u,v derived from the ray direction, and a graded "
						"one is two colours plus an expression_function2d ramp plus a blend_painter "
						"mixing them, while a noise painter gives cloud or foam structure and an "
						"hdr_painter or exr_painter names an image file. THE MEDIUM is a "
						"homogeneous_medium (absorption, scattering, and phase on one line as either "
						"`isotropic` or `hg <g>`) or a painter_heterogeneous_medium, plus a "
						"global_medium chunk naming it, which is how this renderer spells fog, haze, "
						"underwater depth falloff and shafts of light. THE BINDING IS MADE FOR THE "
						"MODEL: the LAST painter the answer lands is bound as the scene's radiance map "
						"with the camera background on, by a rasterizer chunk this call appends "
						"carrying every parameter the previous one had -- so any painter written "
						"before it is an input that one is built from, and the document ends with two "
						"rasterizer chunks of which the last is live. IT ACCEPTS painter, function and "
						"medium chunks and NOTHING ELSE: a geometry or a standard_object is rejected "
						"by name, because a backdrop plane or a sky dome built as a shape is FORM and "
						"making new form is build_element's job, not this one's; a camera, a film, a "
						"rasterizer and a shader op are rejected too; and hosek_wilkie_skylight is "
						"rejected because it is a light chunk that installs a dome itself and belongs "
						"to light_scene -- on a scene that already carries one, this pass authors no "
						"dome at all and writes only the medium. It is HALF of the way the FIRST "
						"compose-phase render gets earned: in the COMPOSE phase the first full-scene "
						"render is refused ONCE and names whichever of populate_scene and "
						"environment_scene have not yet run -- one refusal for the whole checklist, "
						"shared with the other build-phase rules and capped with them, after which "
						"every render proceeds whether or not either ran; renders in the pieces phase "
						"are never refused, and neither is a render with `isolate`. A name already "
						"used in the scene is rejected and NOT renamed. If anything is rejected, ONE "
						"repair retry runs automatically with the exact rejection text -- one, then it "
						"stops, and whatever landed stays landed. Nothing is ever dropped silently. "
						"The result reports what landed, what was bound, whether the scene has a dome "
						"and a global medium before and after, and the frame's TONAL DISTRIBUTION "
						"measured before and after -- a mean, a spread and two percentiles of the same "
						"small internal render, compared to nothing. On a provider that cannot run a "
						"separate completion this returns ok:false with a plain statement, nothing "
						"else changes, and authoring painter and medium chunks by hand is not blocked. "
						"Returns {ok,provider,model,chunksExtracted,landed,"
						"rejected:[{name,kind,reason}],chunkResults,patchResults,retryRan,"
						"retrySucceeded,"
						"completions,painters,media,boundPainter,bindingApplied,bindingReason,"
						"rasterizer,radianceMapBefore,radianceMapAfter,globalMediumBefore,"
						"globalMediumAfter,toneBefore:{lumaMean,lumaStdDev,lumaP1,lumaP99},"
						"toneAfter:{...},message}." );
					tools.push_back( MakeTool( "environment_scene", desc, ObjectProp( "", props, required ) ) );
				}

				// frame_scene (Arc 83 slice 6, 2026-08-13) -- the clean-room
				// FRAMING verb.  It MUTATES (it patches or replaces the camera
				// chunk), so it is not read-safe, and it is additionally
				// excluded from AgentRpc's Propose-autonomy allowlist exactly
				// like light_scene, carrying the matching note.  THE CODEC TEXT
				// IS CANONICAL AND THIS MIRRORS IT.
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "notes", StringProp(
						"Optional free text passed to the framing pass alongside the inventory and the "
						"current camera -- anything about the shot that this scene's own state does "
						"not already say. Nothing checks what it says." ) );
					const std::vector<std::string> required;   // no required params

					const std::string desc =
						( readOnly ? kAutonomyReadNote
						            : proposeOnly ? kFrameSceneProposeRefusedNote
						                          : std::string() ) +
						std::string(
						"Frame the scene: decide where the camera goes and what it looks at. This call "
						"asks the session's provider, in a FRESH context that contains nothing but THE "
						"FULL INVENTORY OF THIS SCENE -- every object, its screen footprint, where it "
						"sits in the frame and, for the ones covering no pixels, WHY (its bounding box "
						"is behind the camera, or projects entirely off-frame in a named direction, or "
						"crosses the camera plane) -- plus the current camera chunk VERBATIM and its "
						"resolved pose, the scene's world bounds, the frame's measured tone, the "
						"imagined description if the session has one, and the camera palette with each "
						"kind's own parameters. It is ONE request, because there is one camera. IT "
						"RETURNS CAMERA PARAMETERS ONLY: exactly ONE camera chunk is accepted, a "
						"second is rejected with that reason, and a geometry, a light, a material or a "
						"`film` chunk is rejected by name -- film carries width, height and pixelAR, "
						"which is raster-size policy rather than framing, and this call never changes "
						"them. If the camera it writes is the SAME KIND the scene already has, each "
						"parameter it names is applied to that chunk as one patch batch -- one head "
						"bump, one undo step -- and any parameter it leaves out keeps the value it "
						"has; a DIFFERENT kind is inserted and the previous camera chunk then removed, "
						"in that order so a failed insert can never leave the scene with no camera. "
						"THE RESULT IS MEASURED, NOT ASSERTED: the same object-coverage pass runs "
						"through the camera before the edit and again through the camera after it, and "
						"the result states how many objects covered at least one pixel on each side, "
						"naming the ones brought into the picture and the ones no longer in it. If the "
						"new camera covers FEWER objects that is stated plainly and NOTHING is "
						"reverted -- a closer view of fewer things is a framing decision, not an "
						"error. It is the way the FIRST framing of a composed scene gets made: in the "
						"COMPOSE phase, while frame_scene has not run, the first camera-authoring edit "
						"-- an insert or a patch of a camera chunk -- is refused ONCE and names this "
						"tool; one refusal, shared with the other build-phase rules and capped with "
						"them, after which every camera edit proceeds whether or not this ran. Camera "
						"edits in the pieces phase are never refused. THIS CALL ITSELF IS ORDERED, too: "
						"frame_scene frames what the scene CONTAINS, and in the COMPOSE phase, while "
						"populate_scene has not yet reached the provider, frame_scene is refused ONCE "
						"and names populate_scene -- the same shared refusal, capped with the others, "
						"after which every frame_scene call proceeds whether or not populate_scene ran. "
						"If anything is rejected, ONE repair retry runs automatically with the exact "
						"rejection text -- one, then it "
						"stops. Nothing is ever dropped silently. On a provider that cannot run a "
						"separate completion this returns ok:false with a plain statement, nothing "
						"else changes, and editing the camera by hand is not blocked. Returns {ok,"
						"provider,model,chunksExtracted,action,cameraKindBefore,cameraNameBefore,"
						"cameraKindAfter,cameraNameAfter,paramsApplied,rejected:[{name,kind,reason}],"
						"patchResults,chunkResults,retryRan,retrySucceeded,completions,objectsBefore,"
						"coveredBefore,objectsAfter,coveredAfter,broughtIntoFrame,pushedOutOfFrame,"
						"message}." );
					tools.push_back( MakeTool( "frame_scene", desc, ObjectProp( "", props, required ) ) );
				}

				// imagine_scene (Arc 77 Phase 2, 2026-08-11) -- READ-SAFE (on
				// IsReadSafeVerb, so it dispatches under Read and Propose
				// exactly as under Commit; no autonomy note).
				//
				// THE CODEC TEXT IS CANONICAL AND THIS MIRRORS IT.  Every
				// contract sentence below is kept semantically identical to
				// AgentChatCodecs.cpp's kToolDefs entry for the same tool, for
				// the drift-class reason recorded on file_build_plan above: a
				// caveat that lives on only one of the two hand-authored
				// surfaces silently changes the mechanism for whichever
				// transport reads the other one.  SourceHygieneTest's
				// build-plan/imagination parity scan pins the sentences.
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "description", StringProp(
						"Required. Your own words for what the finished scene should look like -- what is in "
						"it, how it is arranged, how it is lit, what it feels like. Write it as you would "
						"describe a picture to someone who will draw it." ) );
					std::vector<std::string> required;
					required.push_back( "description" );

					const std::string desc =
						"Imagine the finished scene before you build it: write your own visual description of "
						"what it should look like -- subject, composition, lighting, mood, colour -- and this "
						"call asks your provider to generate one image from exactly that text, returns it to "
						"you, and holds it as this session's SCENE TARGET. `description` is REQUIRED and is "
						"free text; nothing checks what it says, and no wording is preferred. Writing the "
						"description IS the imagining -- the image is what lets you check yourself against it "
						"afterwards. Your words are the SUBJECT; the image is requested in a simple, "
						"flat-shaded 3D-render style so it is something this renderer can actually approach, "
						"rather than concept art it cannot. Once a target exists, every full-frame production "
						"render (not draft, not a mode: render, not an isolate render) also carries a "
						"`sceneTarget` block -- {imagined, targetWidth, targetHeight, composite, "
						"compositeWidth, compositeHeight} -- and any such render you ask an image of shows "
						"that target ABOVE your render in one picture, separated by a grey rule, in place of "
						"the rendered frame on its own. The render half is exactly the size the frame alone "
						"would have been, so looking at the target costs you no resolution. THERE IS NO SCORE "
						"and there is deliberately no similarity number: comparing the two pictures is your "
						"job, not a metric's, and nothing is gated on the target, no reproduction is expected, "
						"and the target is a reminder of what you set out to make rather than a "
						"specification. Calling it again replaces "
						"this session's scene target. On a provider that does not generate images this returns "
						"ok:false with a plain statement and nothing else changes -- no call is blocked by the "
						"absence of a target. It changes nothing in the document -- no chunk, no head version, "
						"no undo step -- so there is no baseHeadVersion and no conflict outcome. Returns "
						"{ok,imagined,replacedPreviousTarget,provider,model,width,height,png_base64,message} "
						"-- the generated image also rides back as an MCP image content block.";
					tools.push_back( MakeTool( "imagine_scene", desc, ObjectProp( "", props, required ) ) );
				}

				// insert_geometry_scaffold (Arc-75 slice S3b; extended by slice E3
				// with blended_chain, volume_bank)
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "family", StringProp(
						"One of: displaced_slab, sweep_rail, blended_vessel, sdf_column, blended_chain, volume_bank." ) );
					props.set( "name", StringProp(
						"A fresh, unique prefix for this expansion (letters/digits/underscore/hyphen). Every generated "
						"chunk is named tmpl_<name>_<role>, e.g. tmpl_rail1_rail." ) );
					props.set( "size", NumberProp( "Overall scale, > 0 (for blended_chain: base radius at the FIRST point)." ) );
					props.set( "detail", NumberProp(
						"0..1: displacement amplitude / profile complexity / smin blend tightness / tessellation / "
						"node density / density-field swirl, per family." ) );
					props.set( "aspect", NumberProp(
						"Elongation, > 0 (1.0 is roughly proportionate; larger stretches the form). Required for "
						"every family EXCEPT blended_chain." ) );
					props.set( "points", StringProp(
						"blended_chain ONLY, REQUIRED: 2-6 semicolon-separated \"x y z\" triplets, e.g. "
						"\"0 0 0; 0.5 1.2 -0.3; 1 3 0\" -- the spine path; the first/last node lands exactly on "
						"the first/last triplet." ) );
					props.set( "taper", NumberProp(
						"blended_chain ONLY, REQUIRED: 0..1, end-to-end radius falloff from the first point to the last." ) );
					props.set( "tone", StringProp(
						"volume_bank ONLY, REQUIRED: \"r g b\", each 0..1 -- the medium's scatter tint." ) );
					props.set( "baseHeadVersion", BaseHeadVersionSchema() );
					std::vector<std::string> required;
					required.push_back( "family" ); required.push_back( "name" ); required.push_back( "size" );
					required.push_back( "detail" );
					// insert_geometry_scaffold is Commit-only, the SAME
					// posture as insert_material_scaffold (see
					// kGeometryScaffoldProposeRefusedNote's doc).
					const std::string desc = ( readOnly ? kAutonomyReadNote
					                          : proposeOnly ? kGeometryScaffoldProposeRefusedNote
					                          : std::string() ) + std::string(
						"Expand ONE of six geometry-family templates into a small chunk graph added to the scene "
						"in one call. Families: \"displaced_slab\" (a box tessellated + bumped by a perlin2d "
						"noise source via displaced_geometry), \"sweep_rail\" (a compact closed polygon profile "
						"swept along a short bowed path with a taper, via sweep_geometry), \"blended_vessel\" "
						"(a base/belly/rim roundcone chain smoothly blended into a vessel or bowl silhouette, via "
						"sdf_geometry), \"sdf_column\" (a base/shaft/capital roundcone chain into a turned-column "
						"silhouette, via sdf_geometry), \"blended_chain\" (a smin-blended chain of spheres swept "
						"along a path YOU author with `points` -- a continuous, tapered limb/branch/tendril), "
						"\"volume_bank\" (an elongated atmospheric volume -- container + near-invisible "
						"dielectric shell + a swirling painter-driven heterogeneous medium, fully wired). The "
						"FIRST FIVE families emit GEOMETRY ONLY -- YOU wire the standard_object (and any "
						"material) yourself, referencing the returned `geometry.name`. volume_bank is the SOLE "
						"exception -- it ALSO emits a dielectric material, a medium, and a standard_object (a "
						"bare volume graph does nothing until an object binds it, and the medium's density-field "
						"bbox must match that object's placement -- the returned `message` on success carries "
						"the one wiring caveat this implies if you reposition it). Families cover common forms; "
						"anything else is hand-authored alongside using the ordinary geometry chunks -- this "
						"tool composes with hand authoring, it does not replace it. REQUIRED params DIFFER BY "
						"FAMILY, no defaults -- a missing one for the resolved family is a blocking error naming "
						"it: `family`/`name` always; displaced_slab/sweep_rail/blended_vessel/sdf_column ALSO "
						"need `size`/`detail`/`aspect`; blended_chain ALSO needs `points`/`size`/`taper`/`detail` "
						"(NO `aspect`); volume_bank ALSO needs `size`/`aspect`/`detail`/`tone` (NO `points`/"
						"`taper`). Internal graph constants are jittered deterministically from `name`: the SAME "
						"name (and other params) reproduces byte-identical chunks, a DIFFERENT name visibly "
						"differs. Every generated chunk is an ORDINARY, EDITABLE document chunk named "
						"tmpl_<name>_<role> -- read_document shows them, and propose_patch/remove_chunk work on "
						"them exactly like any hand-authored chunk. Applied IN ORDER through the SAME batch "
						"machinery insert_chunks uses (SEQUENTIAL, BEST-EFFORT). Returns "
						"{applied,total,results:[...]} -- the EXACT insert_chunk per-element shape -- plus "
						"`geometry` ({name,kind} of the one geometry chunk to bind into a "
						"standard_object.geometry slot) and, for volume_bank only, `material`/`medium`/`object` "
						"({name,kind} each) plus a factual `message`. Check every element's own status. A "
						"missing/invalid param, an unrecognized family, or a NAME COLLISION refuses the WHOLE "
						"call before any chunk is generated (document unchanged) -- reported as a tool error, "
						"not a partial result. Always pass the headVersion you last read as baseHeadVersion." );
					tools.push_back( MakeTool( "insert_geometry_scaffold", desc, ObjectProp( "", props, required ) ) );
				}

				// replace_geometry_scaffold (R2, 2026-08-10)
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "target", StringProp(
						"The NAME of the standard_object whose geometry to replace -- the OBJECT, not the "
						"geometry chunk." ) );
					props.set( "family", StringProp(
						"One of: displaced_slab, sweep_rail, blended_vessel, sdf_column, blended_chain. "
						"(volume_bank is NOT available here -- it emits its own standard_object.)" ) );
					props.set( "name", StringProp(
						"A fresh, unique prefix for this expansion (letters/digits/underscore/hyphen). Every generated "
						"chunk is named tmpl_<name>_<role>, e.g. tmpl_rail1_rail." ) );
					props.set( "size", NumberProp( "Overall scale, > 0 (for blended_chain: base radius at the FIRST point)." ) );
					props.set( "detail", NumberProp(
						"0..1: displacement amplitude / profile complexity / smin blend tightness / tessellation / "
						"node density, per family." ) );
					props.set( "aspect", NumberProp(
						"Elongation, > 0 (1.0 is roughly proportionate; larger stretches the form). Required for "
						"every family EXCEPT blended_chain." ) );
					props.set( "points", StringProp(
						"blended_chain ONLY, REQUIRED: 2-6 semicolon-separated \"x y z\" triplets, e.g. "
						"\"0 0 0; 0.5 1.2 -0.3; 1 3 0\" -- the spine path; the first/last node lands exactly on "
						"the first/last triplet." ) );
					props.set( "taper", NumberProp(
						"blended_chain ONLY, REQUIRED: 0..1, end-to-end radius falloff from the first point to the last." ) );
					props.set( "baseHeadVersion", BaseHeadVersionSchema() );
					std::vector<std::string> required;
					required.push_back( "target" ); required.push_back( "family" ); required.push_back( "name" );
					required.push_back( "size" ); required.push_back( "detail" );
					// replace_geometry_scaffold is Commit-only, the SAME posture
					// as its two scaffold siblings (see
					// kReplaceGeometryScaffoldProposeRefusedNote's doc) -- and,
					// unlike them, an External-authority session cannot stage it
					// either (it is one composite document swap, not a chunk edit
					// an Owner approves card-by-card).
					const std::string desc = ( readOnly ? kAutonomyReadNote
					                          : proposeOnly ? kReplaceGeometryScaffoldProposeRefusedNote
					                          : std::string() ) + std::string(
						"REPLACE THE FORM of a part already in the scene: expand a geometry family template exactly "
						"as insert_geometry_scaffold does, and rebind an EXISTING object's geometry to it, in ONE "
						"call. Reach for this whenever a rendered shape reads as too plain or the wrong form -- it "
						"costs the same single call a colour tweak costs. `target` names the standard_object (NOT "
						"the geometry chunk -- passing a geometry name is refused with the names of the objects "
						"that use it). The object's position, orientation, scale, material and every other "
						"parameter are PRESERVED byte-for-byte; only `geometry` changes, so placement stays put "
						"across a form change. Families and their per-family required params are IDENTICAL to "
						"insert_geometry_scaffold's, with ONE exception: \"volume_bank\" is not available here "
						"(it emits its own standard_object -- use insert_geometry_scaffold for it). The old "
						"geometry chunk is REMOVED when nothing else references it; when something does, it is "
						"RETAINED and the referrers are named in the result. Chunks left unreferenced one hop "
						"deeper (e.g. the noise source that only fed the old displaced geometry) are REPORTED in "
						"`orphans` for removal with remove_chunks -- never deleted silently. ATOMIC: one head "
						"version bump, one undo step, and on ANY refusal (unknown or ambiguous target, name "
						"collision, a stale baseHeadVersion conflict, a candidate that would not derive, or a "
						"transient busy/in-progress reject) NOTHING changes -- check `status`: \"applied\" is "
						"the only outcome where anything landed. The ONE exception is \"diagnosed\": the "
						"Document WAS mutated (the new geometry landed and the object's slot was rebound) but "
						"the re-derive still emitted diagnostics, so `previousGeometry`/`orphans` describe "
						"something real, not a discarded plan -- look at the log before touching this part "
						"again. Returns {applied,total,results:[...],status,retriable,headVersion,target,"
						"geometry:{name,kind},previousGeometry:{name,kind,removed,referrers},orphans,message} "
						"-- every `results` element carries the SAME verdict, because the call is ONE mutation, "
						"not a batch. Always pass the headVersion you last read as baseHeadVersion." );
					tools.push_back( MakeTool( "replace_geometry_scaffold", desc, ObjectProp( "", props, required ) ) );
				}

				// remove_chunk
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "target", StringProp( "The bare NAME of the chunk to remove." ) );
					props.set( "kind",   StringProp( "OPTIONAL chunk KIND keyword to disambiguate a cross-category name clash (same resolution rules as propose_patch's `kind`)." ) );
					props.set( "baseHeadVersion", BaseHeadVersionSchema() );
					std::vector<std::string> required; required.push_back( "target" );
					const std::string desc = ( readOnly ? kAutonomyReadNote : proposeOnly ? kAutonomyProposeNote : std::string() ) + std::string(
						"Remove the chunk resolved by bare name (+ optional kind) from the scene "
						"document via a trivia-preserving erase. REQUIRES a scene to be loaded. "
						"Same result gating as propose_patch (including the \"staged\" status -- see "
						"propose_patch's description), plus the removed chunk's `name`/`kind` echo. "
						"An unknown target is rejected; an ambiguous name is rejected with a "
						"disambiguation hint; a target still REFERENCED by another chunk fails the "
						"dry-run and is rejected with the diagnostic (document left byte-identical). "
						"A REJECTED remove may carry `issues`: [{param,value,reason,suggestions:[...]}] "
						"-- the SAME shape propose_patch/insert_chunk return. The one reason remove_chunk "
						"produces is \"still_referenced\": `value` is the target's own name and "
						"`suggestions` NAMES every chunk the reference graph found still referencing it "
						"(edit or remove those first, then retry). An EMPTY or ABSENT `issues` on a "
						"rejection does NOT mean the target was unreferenced -- it means either the "
						"remaining document fails to derive for the OTHER reason (order, not "
						"reference), or a DYNAMIC reference (e.g. a timeline naming this entity) this "
						"static pass cannot see; `message` still carries the engine's own hedged "
						"diagnostic in that case." );
					tools.push_back( MakeTool( "remove_chunk", desc, ObjectProp( "", props, required ) ) );
				}

				// remove_chunks (R1a, 2026-08-09) -- the ATOMIC batch form.  This description is
				// hand-authored HERE and must stay semantically identical to the chat-codec tool
				// definition in AgentChatCodecs.cpp's kToolDefs (the two texts are separate by design --
				// this one is MCP-facing -- but they describe ONE verb, so a semantic change to either
				// must land in both).
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "targets", StringArrayProp( "The bare NAMES of the chunks to remove, 1 or more. Order does not matter -- chunks that reference each other WITHIN the batch are removed together regardless of listed order." ) );
					props.set( "baseHeadVersion", BaseHeadVersionSchema() );
					std::vector<std::string> required; required.push_back( "targets" );
					const std::string desc = ( readOnly ? kAutonomyReadNote : proposeOnly ? kAutonomyProposeNote : std::string() ) + std::string(
						"Remove SEVERAL chunks in ONE call, ATOMICALLY. Prefer ONE remove_chunks call over "
						"repeated remove_chunk calls whenever you delete more than one chunk: each separate "
						"remove_chunk costs a full round-trip and a separate undo step, while one "
						"remove_chunks call is one round-trip, ONE headVersion bump, and ONE undo step. "
						"REQUIRES a scene to be loaded. ALL-OR-NOTHING -- unlike insert_chunks, which applies "
						"what it can: if ANY target fails (unknown name, ambiguous name, or still referenced "
						"from outside the batch) then NOTHING is removed and the head is byte-identical, so "
						"you fix the one offender and resend. A chunk referenced ONLY by other chunks IN THE "
						"SAME BATCH is removable, in ANY order -- list a painter and the material that uses it "
						"together and both go; there is no need to order the list by dependency. "
						"\"still_referenced\" is therefore reported only for referrers OUTSIDE the batch, and "
						"`suggestions` NAMES them. Duplicate names are deduped, not refused: the chunk is "
						"removed once and `note` says which names were listed more than once. There is NO "
						"per-target `kind` -- targets are bare names only; for an ambiguous name, or the "
						"sole-unnamed-camera case, use the singular remove_chunk with `kind` for that one "
						"chunk. Same result gating as propose_patch (including the \"staged\" status -- an "
						"External-authority session stages the WHOLE batch as ONE proposal, never N). The "
						"result carries `applied` (a BOOL -- all or nothing), `removed`/`total`, an optional "
						"`note`, and `results`: one entry per UNIQUE target in first-occurrence order (match "
						"by `name`, not index), each with the same {param,value,reason,suggestions} `issues` "
						"shape remove_chunk returns, localizing exactly which target blocked the batch." );
					tools.push_back( MakeTool( "remove_chunks", desc, ObjectProp( "", props, required ) ) );
				}

				// render
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "samples", NumberProp( "OPTIONAL sample-count override, CLAMPED to [1,16] -- an agent-surface cap (out-of-range values are clamped, not rejected; see this tool's own description). Omit for the scene-authored sample count, UNLESS that count exceeds 16, in which case it is likewise capped to 16 (check the result's `agentRenderCap.samplesCapped` / `effectiveSamples`). Only honoured by rasterizers that support a sample-count override (the pixel-based family: PT, spectral PT, BDPT, VCM) -- see the result's `samplesOverridden`/`effectiveSamples` fields; on an unsupported rasterizer (MLT, photon-map-only, Auto's outer wrapper) the override -- or the implicit cap when none was requested -- is honestly reported as NOT applied, never silently ignored." ) );
					props.set( "width",  NumberProp( "OPTIONAL transient film-width override in pixels, CLAMPED to [16,256] -- an agent-surface cap (see this tool's own description). Must be paired with `height` -- supplying only one is ignored (ambiguous aspect ratio), not applied. Omitting BOTH renders at the scene's own aspect ratio scaled to fit the 256px cap, never the full authored resolution -- check `previewWidth`/`previewHeight` or `agentRenderCap.filmWidth`/`filmHeight` for the actual/uncapped dims. NEVER mutates the scene document; restored after the render." ) );
					props.set( "height", NumberProp( "OPTIONAL transient film-height override in pixels, CLAMPED to [16,256] -- an agent-surface cap (see this tool's own description). Must be paired with `width`." ) );
					props.set( "camera", CameraOverrideSchema() );
					props.set( "pinned", BoolProp( "OPTIONAL, default false. When true, this render cannot be silently superseded by a later render submission while it is in flight (it still responds to an explicit cancel or teardown) -- meaningful only against a live in-app GUI session's controller; has no effect in headless `rise --agent-stdio`." ) );
					props.set( "quality", StringProp( "OPTIONAL, \"draft\" or \"production\" (default \"production\" -- today's exact behaviour). \"draft\" renders through a wholly SEPARATE, cheap studio-preview pipeline (the SAME fixed preview shader the GUI's live interactive editor uses) that IGNORES the scene's authored materials and lighting entirely -- geometry, composition, and camera framing are representative; materials, lighting, exposure, and colour are NOT. NEVER judge materials/lighting/exposure/colour from a draft image -- use quality:\"production\" (or read_viewport) for that. A draft render CAPS samples at 4 regardless of the requested `samples` value. Check the result's `renderMode` field (\"production\"/\"draft\") to see which pipeline actually ran -- `integrator` always names the head's active PRODUCTION rasterizer regardless of `quality`, so it is NOT the field to check for this." ) );
					props.set( "mode", StringProp( std::string(
						"OPTIONAL, \"beauty\" (default), \"objectmap\", or one of the view-mode names below. "
						"\"objectmap\" renders a flat per-object IDENTITY segmentation -- each scene object painted a distinct high-contrast colour, no lighting/materials -- and adds a `legend` array of {name,colorHex,pixelCount} to the result. Use it to reason about WHICH object is at WHICH pixel and how much of the frame each covers (occlusion, placement, framing). IMPORTANT: read the objectmap image at NATIVE size -- do NOT pass read_image's maxEdge, since box-downscaling blends the identity colours and corrupts colorHex matching. "
						"The view-mode names (no `legend`, unlike objectmap): " ) + DescribeViewModes() + std::string(
						". Two KINDS of view mode: \"normals\"/\"depth\"/\"facets\"/\"wireframe\" are single-pass FALSE-COLOUR diagnostics (exactly 1 sample/pixel, no scene lighting); \"deep_reflect\"/\"direct\"/\"indirect\"/\"clay_lights\" are REAL production-class path-traced renders (BeautyVariant modes) at a FIXED reduced resolution and a FIXED higher sample count (deep_reflect: quarter-res, 16 spp, 24 bounces -- use it to see what reflections/refractions in glass/metal/water actually resolve to; direct: half-res, 8 spp, direct lighting only, no indirect bounces -- use it to check whether the LIGHTING alone looks right independent of indirect bounce; indirect: half-res, 12 spp, 16 bounces, INDIRECT light only -- the direct/emission contribution at the camera-visible vertex is suppressed, so use it to see what bounce light alone contributes; clay_lights: half-res, 12 spp, 12 bounces, every surface's reflectance substituted for a shared neutral clay material while real lights/GI stay untouched -- use it to check whether the LIGHTING is right independent of materials/texture). `quality` and `samples` are IGNORED under objectmap or ANY view mode (each has a FIXED fidelity, diagnostic or production) -- check the result's `effectiveSamples` field for the actual spp used. Check the result's `renderMode` field to confirm which one actually ran -- it echoes back the exact mode name (e.g. \"normals\", \"deep_reflect\"), distinguishing it from \"objectmap\"/\"draft\"/\"production\". Orthogonal to `quality`: these modes are about geometry/segmentation/transport, draft is about cheap studio shading. Known limitation (wireframe): triangle-mesh edges only -- analytic primitives (sphere/box/SDF) and unmeshed geometry render as dim facet shading with no lines, which is correct behaviour, not a bug. Known limitation (depth): brightness is normalized PER RENDER to the VISIBLE hit-distance range in that frame (auto-calibrated -- a wide shot and a close-up of the same scene use DIFFERENT brightness scales, so never compare depth renders across two different camera framings as if they shared a scale), self-calibrating within a single render call, falling back to the scene's bounding-box diagonal only for a degenerate/empty scene. INSTANCE NAMES (objectmap only): a synthesized legend name (e.g. \"grid[0,1]\" from a `source` chunk carrying count_u/count_v) identifies one repetition in the map but is NOT a CST chunk -- to EDIT it, target the INSTANCING chunk (strip the \"[i,j]\" suffix, e.g. \"grid\"), since propose_patch/remove_chunk on the instance name will fail." ) ) );
					props.set( "xray", BoolProp(
						"OPTIONAL, default FALSE. ONLY meaningful when `mode` is one of the FALSE-COLOUR diagnostics (normals/depth/facets/wireframe) -- when true, resolves the ray THROUGH transmissive (glass-like) surfaces to the first OPAQUE hit, following a STRAIGHT LINE with NO refraction bending (deliberately an x-ray, not an optics simulation), up to 16 surfaces skipped. By default the transmissive surface itself is shown. Pass xray:true to inspect opaque geometry underneath/inside it. If the ray never reaches an opaque surface after 16 skips, the LAST transmissive surface is shown instead of a black hole -- an honest partial answer. Silently IGNORED (honestly noted) under mode:\"beauty\"/\"objectmap\" or any production-transport BeautyVariant view mode (deep_reflect/direct/indirect/clay_lights -- skipping through glass would defeat their whole purpose)." ) );
					props.set( "view", StringProp(
						"OPTIONAL name of a saved viewport bookmark (a live in-app GUI session's Named Views) or, headless, a scene CAMERA name -- renders from that vantage for THIS call only, composing with EVERY `mode` above. Equivalent to transferring the full shared pose (location, lookat, up, Euler orientation, target orientation) plus a pinhole FOV, by name instead of raw numbers -- if both are supplied, `view` wins. PINHOLE-ONLY: the override cannot re-type the active camera, so a view naming a thin-lens/fisheye/orthographic camera FAILS the render (`ok:false`) naming the unsupported type rather than silently rendering with the active camera's optics. An ONB-style active camera that cannot round-trip this pose also fails loudly. An unresolvable name likewise FAILS with the available-name list in `message`. Use this to compare the SAME render mode from several saved angles without re-deriving camera math each time." ) );
					props.set( "isolate", StringProp(
						"OPTIONAL name of ONE standard_object to render BY ITSELF, with the camera AUTO-FRAMED on that object's bounding box (a fixed three-quarter vantage at the distance that fills ~85% of the frame). Every OTHER object is transiently hidden for this one render -- hit by no camera, secondary, or shadow ray -- and nothing is written to the document, the viewport, or the user's camera. Use it to actually LOOK AT a part you just built when it is small, dark, or occluded in the full frame: a 20-pixel wing in a wide night shot tells you nothing about its shape, and a form you never evaluated is a form you cannot fix. mode:\"normals\" and mode:\"facets\" read FORM best here (surface direction / raw tessellation, with no material or lighting confounding the read); mode:\"beauty\" composes too, but a hidden emissive OBJECT also stops acting as a light, so a scene lit ONLY by other objects' emissive materials isolates DARK -- explicit light chunks and the environment are unaffected. Composes with EVERY `mode` and with quality:\"draft\" (object visibility is Scene state, not rasterizer state), so unlike `light` it is never silently ignored. If `camera` or `view` is ALSO supplied, THAT camera wins and no auto-framing happens -- the result's `isolate.autoFramed` says which. A successful isolate render (ok:true) adds an `isolate` object to the result: {object, bboxMin[3]?, bboxMax[3]?, longestEdge?, autoFramed, bboxCoverage?} -- bboxMin/bboxMax/longestEdge are OMITTED if the object's bounding box turned out degenerate/unbounded (only reachable when you also supplied `camera`/`view`, since that case otherwise fails the render outright). `bboxCoverage` is the fraction of the frame covered by the object's PROJECTED BOUNDING BOX (an upper bound on its silhouette, exact and identical across modes -- it can OVERSTATE a thin or diagonal silhouette by a large factor, e.g. a wing seen edge-on; for an EXACT per-pixel count use mode:\"objectmap\" and read the legend's pixelCount). `bboxCoverage` is OMITTED (not an approximation) whenever the ACTIVE camera is not a pinhole -- the projected-bbox formula assumes a pinhole's tan(fov/2) projection, which does not apply to thin-lens/fisheye/orthographic; `message` says so when it happens. An unknown name, a GENERATOR name covering several instances (isolate one instance's full name instead), a CSG operand (isolate the composite the message names), or an object whose bounding box is degenerate/unbounded with NO caller-supplied camera FAILS the render (ok:false) with the available object names in `message`." ) );
					// G3b fix-round (2026-08-10) FIX 2: this text MIRRORS AgentChatCodecs.cpp's
					// `target` description, which is the CANONICAL one -- change that first, then
					// mirror here, keeping the sentences literally identical.  SourceHygieneTest
					// pins the load-bearing sentences on BOTH files, so a caveat added to only one
					// of them fails the build.
					props.set( "target", StringProp(
						"OPTIONAL name of an ELEMENT in the build plan filed with file_build_plan -- compares that element's SKETCH against the silhouette the isolated object actually renders as. REQUIRES `isolate` (the plan-element-to-object join is made HERE, by you, at comparison time); `target` without `isolate` is a clean -32602 stating the requirement. VANTAGE: with no `camera`/`view` of your own, the render uses the AXIS-ALIGNED vantage the sketch declared -- front = looking along -Z (world +X right, world +Y up), side = looking along -X (world -Z right, world +Y up), top = looking straight down (world +X right, world -Z up in frame) -- instead of the three-quarter one `isolate` uses alone. A caller-supplied `camera`/`view` still WINS, and `target.vantage` then reads \"caller-camera\" rather than a view name. The comparison costs ONE extra internal identity render at the same pose and dims, so the measured silhouette is exact and independent of `mode`, `quality`, lighting and materials. On success (ok:true) the result gains a `target` object: {element, view, vantage, iou, mirroredIou, sketchAreaFraction, silhouetteAreaFraction, sketchAspect, silhouetteAspect?, thinnestAxisRatio?, compositeWidth, compositeHeight}. `iou` is intersection-over-union in [0,1] of the two masks after BOTH are cropped to their own bounding box and fitted onto the same 256x256 canvas by the same transform -- so it measures SHAPE ONLY, not position and not size (it is invariant to where the element sits and how big it is); `mirroredIou` is the same measurement with the rendered silhouette flipped in X. `sketchAreaFraction`/`silhouetteAreaFraction` are each mask's filled fraction OF THAT SHARED CANVAS (not of the frame -- `isolate.bboxCoverage` is the frame measurement). `silhouetteAspect` is OMITTED when no pixel of the object landed in the frame; `thinnestAxisRatio` (the object's smallest 3D bounding-box extent over its largest -- 1.0 for a cube, near 0 for a flat plane) is OMITTED when the bounding box is unusable. These are MEASUREMENTS, not scores: nothing is gated on them and no particular value is required. IMAGE: the result carries a [sketch | silhouette | overlay] composite -- three 256x256 tiles: the filed sketch, the rendered silhouette, and both together with sketch-only RED, silhouette-only CYAN, overlap WHITE, neither BLACK (each tile carries its own black background, so the strip reads the same on a light or a dark page). That composite rides back as this call's image WITHOUT `imageMaxEdge`, and REPLACES the rendered frame when `imageMaxEdge` is also supplied -- re-render without `target`, or call read_image, to get the frame itself. An unknown element name, or no filed build plan at all, FAILS the render (ok:false) with the filed element names in `message`." ) );
					props.set( "light", StringProp(
						"OPTIONAL name of a light (or an emissive object) to render with as the ONLY active light -- every other light contributes exactly zero, an unbiased partition of the full lighting (not a dim/approximate preview of it). Valid with mode:\"beauty\" (the default) and the four production-transport BeautyVariant view modes (deep_reflect/direct/indirect/clay_lights); silently IGNORED (honestly noted in `message`) under objectmap, the false-colour diagnostics (normals/depth/facets/wireframe), or quality:\"draft\" -- none of those evaluate scene lighting at all. An unresolvable name FAILS the render (`ok:false`) with the available-name list in `message`, same contract as an unresolvable `view`. Use this to check one light's contribution in isolation (shadow shape, colour, falloff) without the others visually competing for attention." ) );
					props.set( "perception", BoolProp(
						"OPTIONAL, default true. For a production beauty render, captures albedo, world-space normal, and primary-camera-hit depth in the SAME render without changing beauty pixels. Then call read_image with representation:\"perception\" for one 2x2 atlas plus structured depth/memory metadata. Set false to avoid perception-specific allocation when beauty alone is sufficient; an OIDN-enabled render can still allocate its own denoising auxiliaries. Ignored for draft/objectmap/view modes." ) );
					props.set( "imageMaxEdge", NumberProp(
						"OPTIONAL long-edge bound in pixels, CLAMPED to [16,1024]. Supply it to get the rendered PNG back INLINE in this result (png_base64/byteLength/imageWidth/imageHeight), downscaled to that bound -- one call instead of render followed by read_image. The bytes are produced by the SAME downscale+encode read_image uses, so they are exactly what read_image with that maxEdge would have returned. ~192 is enough for a modeling/placement check; omit the whole parameter to get today's lean statistics-only result and no image. REFUSED with mode:\"objectmap\" (an objectmap must be read at NATIVE size -- render without this parameter, then read_image with no maxEdge). Use read_image separately when you want a SECOND bound on a render you already have, or representation:\"perception\"." ) );
					tools.push_back( MakeTool( "render",
						"Render the current scene head SYNCHRONOUSLY and return {ok,width,height,"
						"meanR,meanG,meanB,integrator,previewWidth,previewHeight,cameraOverridden,"
						"message,renderJobId,samplesOverridden,effectiveSamples,renderMode} (plus a "
						"per-object `legend` when mode:\"objectmap\", an `isolate` object when "
						"`isolate` was applied, a `target` object with the measured sketch "
						"comparison when `target` was applied, an `inventory` object on every "
						"full-scene beauty render -- see below -- and an `agentRenderCap` object "
						"when the agent-surface cap described next actually reduced this render). "
						"AGENT RENDERS ARE CAPPED at 256px on the long edge and 16 samples/pixel -- "
						"omitted width/height/samples still render, just at or under those caps "
						"(never the scene's full authored resolution/sample count); an explicit value "
						"above the cap is silently clamped, never rejected. This is a fixed property "
						"of the agent surface -- the user's own full-frame, full-sample renders happen "
						"from the GUI, a separate path this cap does not touch. Returns NO image content "
						"block by default; pass `imageMaxEdge` (e.g. 192) to get the rendered PNG back "
						"as a real MCP image content block in this same result, which is the one-call "
						"form to prefer for an ordinary look. A separate read_image is still the way to "
						"re-read a render you already "
						"have at a different bound, to read an objectmap at native size, and to fetch "
						"representation:\"perception\". "
						"`integrator` is the active rasterizer's scene-file chunk keyword (e.g. "
						"\"pathtracing_pel_rasterizer\"), empty when none is active -- useful to "
						"confirm which integrator an insert_chunk activated; it does NOT change with "
						"`quality` (see `quality`'s own description for the field to use instead). "
						"`meanR/meanG/meanB` are "
						"linear per-channel means: a stable, order-independent signature for "
						"comparing two renders (RISE's sampler is not deterministic across runs, so "
						"raw pixels differ run-to-run by MC noise even on an unchanged scene). "
						"`renderJobId` is a monotonically increasing id from one of two DISJOINT-BY-"
						"PARITY id spaces (coordinator-tracked ids are always EVEN, session-local "
						"ids are always ODD) usable with render_status/render_wait/render_cancel. "
						"TOKEN/TIME ECONOMY: for a quick orientation check (is the geometry/camera "
						"roughly right?) prefer quality:\"draft\" over a small width/height -- it is "
						"the cheapest possible render (capped at 4 samples, fixed studio shading, no "
						"scene lighting to evaluate) but its pixels tell you NOTHING about materials, "
						"lighting, exposure, or colour; reserve quality:\"production\" (the default) "
						"for any check of those. "
						"NOTE: the async submission mode the underlying RPC surface supports "
						"(`{\"async\":true}`) is NOT exposed as an option here -- every render "
						"through this tool is fully synchronous and blocks until complete. "
						"Whether `pinned` and the single-slot/30s-fairness semantics "
						"are LIVE depends on the transport, not on this tool. Over the headless "
						"`rise --agent-stdio --mcp` process there is no in-app controller, so "
						"`pinned` is a no-op and there is no slot to queue on; over the "
						"GUI-HOSTED loopback endpoint the same `tools/list` payload is served by "
						"an adapter whose session IS controller-attached, so `pinned` is honoured "
						"and a render really can queue behind (or be refused by) another render "
						"-- see read_viewport's `render_in_progress` note below for what that "
						"means in practice. "
						"EVERY FULL-SCENE BEAUTY RENDER (production or draft, no `isolate`, no "
						"`mode`) also carries an `inventory` object: {objects, covered, passWidth, "
						"passHeight, notAreaSampled, text}. `text` is one line per object saying how "
						"much of the frame it covers and where it is -- in the frame for the ones that "
						"appeared, and in the world plus which way it lies (behind the camera, "
						"off-frame left, overlapping the frame but covering nothing, ...) for the "
						"ones that did not. It is measured in a small one-ray-per-pixel identity "
						"pass through this render's camera, at the passWidth x passHeight it "
						"reports, so a footprint under one pass pixel reads as 0. Read it before "
						"concluding anything about an empty-looking frame: an object that covered "
						"no pixels is still there, and the block says where. `notAreaSampled` is an "
						"array of {name,reason} for emissive objects this render measured as not "
						"selected by light sampling (their emission still shows on direct view and "
						"BSDF hits); omitted when empty. The `scene_inventory` "
						"tool returns the same measurement on demand.",
						ObjectProp( "", props, std::vector<std::string>() ) ) );
				}

				// render_status
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "renderJobId", NumberProp( "A renderJobId previously returned by `render`." ) );
					std::vector<std::string> required; required.push_back( "renderJobId" );
					tools.push_back( MakeTool( "render_status",
						"Poll the status of a renderJobId. Returns {found,active,pinned}: found is "
						"false for an unrecognized id -- including any id from a DIFFERENT session, "
						"or a session-local (ODD) id when this call has no coordinator to resolve it "
						"against (the common case in headless `rise --agent-stdio`, which has no "
						"live in-app controller). active/pinned are meaningful only when found is "
						"true. A false `found` is NOT an error -- it is an honest \"nothing to "
						"report\" signal.",
						ObjectProp( "", props, required ) ) );
				}

				// render_wait
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "renderJobId", NumberProp( "A renderJobId previously returned by `render`." ) );
					props.set( "timeoutMs", NumberProp( "OPTIONAL wait bound in milliseconds, CLAMPED to [0,60000]. Default 5000. 0 polls once without blocking." ) );
					std::vector<std::string> required; required.push_back( "renderJobId" );
					tools.push_back( MakeTool( "render_wait",
						"Block up to timeoutMs for a renderJobId to complete. Returns {completed,"
						"found,active,pinned,result?}: completed is true iff it was observed to "
						"finish (or was already finished) within the timeout. `result`, when "
						"present, carries the SAME shape the synchronous `render` tool returns -- "
						"but is only populated when this exact session cached that job's stats (a "
						"job submitted asynchronously on THIS session); in headless `rise "
						"--agent-stdio --mcp` every render through this adapter is already "
						"synchronous, so `render` itself already returned the result and this tool "
						"exists mainly for parity with the underlying RPC surface.",
						ObjectProp( "", props, required ) ) );
				}

				// render_cancel
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "renderJobId", NumberProp( "OPTIONAL, advisory only. The render worker this refers to is single-slot, so there is at most one outstanding async render to cancel regardless of which id is named." ) );
					tools.push_back( MakeTool( "render_cancel",
						"Trip the cancel signal for the outstanding async render, WITHOUT blocking "
						"for it to actually stop (poll render_status/render_wait afterward to "
						"observe completion). Returns {cancelled,found,active}: cancelled is true "
						"iff a live controller was attached to route the cancel through -- in "
						"headless `rise --agent-stdio --mcp` there is no live in-app controller, so "
						"this always reports cancelled:false as a harmless no-op rather than an "
						"error (calling it with nothing outstanding, or from a headless session, is "
						"NOT an error condition).",
						ObjectProp( "", props, std::vector<std::string>() ) ) );
				}

				// read_image
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "maxEdge", NumberProp( "OPTIONAL long-edge bound in pixels, CLAMPED to [16,1024]. Downscales the cached image (box filter, aspect-preserving, NEVER upscales) before encoding -- no re-render. Omit for native beauty; an omitted perception atlas is safely bounded to 1024." ) );
					props.set( "representation", StringProp( "OPTIONAL, \"beauty\" (default) or \"perception\". perception returns a conventional 2x2 atlas ordered [beauty, albedo; world-space normal, log depth] from the SAME production beauty render, plus source dimensions, guidePrefilter (fast/accurate), depth range, valid-depth count, and managed auxiliary-memory accounting (exact persistent payload and a conservative session peak bound). It is unavailable after draft/objectmap/view renders, render{perception:false}, or in builds compiled without PNG support." ) );
					tools.push_back( MakeTool( "read_image",
						"Read the last successful render's image. Returns an MCP image content "
						"block (inline PNG, so an MCP vision-capable client sees the rendered frame "
						"directly) PLUS a text block with the metadata "
						"{png_base64,byteLength,width,height,representation} (the same fields the underlying RPC "
						"verb returns, for a client that wants the raw base64/dims rather than the "
						"image block). Call `render` at least once first -- before any render this "
						"returns an image for whatever is cached (empty/default if nothing has "
						"rendered yet). IF THE LAST RENDER WAS mode:\"objectmap\": read at NATIVE "
						"size -- OMIT maxEdge. Downscaling box-blends the flat identity colours, "
						"corrupting the exact-byte legend match (maxEdge is for beauty/draft only). "
						"For richer scene understanding after production beauty, use representation:\"perception\": "
						"the returned single image carries beauty, reflectance, orientation, and log-distance together. "
						"Omitting maxEdge keeps native beauty behavior but bounds this larger atlas to 1024.",
						ObjectProp( "", props, std::vector<std::string>() ) ) );
				}

				// read_viewport (Toolkit slice 1)
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "maxEdge", NumberProp( "OPTIONAL long-edge bound in pixels, CLAMPED to [16,1024]. Downscales the copied viewport frame (box filter, aspect-preserving, NEVER upscales) before encoding -- no re-render. Omit for the native viewport resolution." ) );
					tools.push_back( MakeTool( "read_viewport",
						"Read the user's LIVE interactive viewport -- the exact frame they are "
						"looking at RIGHT NOW in the GUI. This is DIFFERENT from read_image: "
						"read_image returns YOUR last headless render; read_viewport returns the "
						"USER's live viewport as it currently stands. It NEVER triggers a render "
						"(it just copies the most recent interactive frame), so it is the cheapest "
						"way to observe what the user sees. Returns an MCP image content block "
						"(inline PNG) PLUS a text block with {available,reason,png_base64,byteLength,"
						"width,height} plus, when a live viewport exists, `paneSet` -- the N-up "
						"multi-viewport introspection {layout(0 Single/1 TwoH/2 OnePlusTwo/3 Quad),"
						"primary,sourcePane,panes[4]{visible,contentSource(0 Interactive/1 LastRender),mode,vantageKind(0 SceneCamera/1 FreeFly/"
						"2 NamedView/3 SceneCameraNamed),namedView}}. `namedView` holds the referenced "
						"name for kinds 2 and 3. `sourcePane` is WHICH pane's pixels the PNG holds "
						"(the last-rendered pane -- in a multi-pane layout this is NOT necessarily the "
						"primary or pane 0), so always check it before reasoning about a specific "
						"pane's content. The pane set is READ-ONLY by design -- there is no agent "
						"control of layout or panes. When `available` is false there is no image: `reason` is "
						"one of SEVEN values. RETRIABLE (they clear on their own -- one short retry is "
						"reasonable): \"editor_transaction_in_progress\" (the user is mid-gesture or "
						"saving), \"render_in_progress\" (another coordinated render holds the gate), "
						"\"editor_interaction_finalize_failed\" (an open editor interaction could not be "
						"finalized this time). RESOLVES ON ITS OWN, BUT NOT BECAUSE YOU RETRIED: "
						"\"no_frame_yet\" (the viewport exists but has not rendered a frame yet -- it WILL "
						"once the user's viewport draws, so a later read_viewport can succeed, but nothing "
						"YOU do makes that happen sooner; use `render` in the meantime). PERMANENT "
						"(retrying can never succeed): \"no_controller\" (this session has no live GUI "
						"viewport -- e.g. a headless run -- and never will), \"editor_shutting_down\" (the "
						"editor is tearing down and never comes back), \"editor_interaction_unrecoverable\" "
						"(an editor interaction failed to persist and the editor LATCHED that failure -- it "
						"does NOT clear on its own, so read_viewport AND render both stay refused until the "
						"scene is reopened; tell the user rather than retrying). available:false is a "
						"normal, structured result, not an error. FALLING BACK TO `render`: it is the right "
						"move for \"no_controller\" and \"no_frame_yet\" -- there is no live viewport to "
						"read, but rendering works. For \"editor_transaction_in_progress\", "
						"\"editor_interaction_finalize_failed\", \"editor_shutting_down\" and "
						"\"editor_interaction_unrecoverable\", `render` passes through the SAME "
						"editor/admission gate that just refused read_viewport and is refused too. "
						"\"render_in_progress\" is the ONE exception to the same-gate rule -- but `render` "
						"is still a POOR fallback there. A PLAIN `render {}` -- no `width`+`height` pair, "
						"no `camera`, no `view` -- does NOT take the parked path; it queues on the "
						"agent-render slot and WAITS up to 30 s. It SUCCEEDS if the occupant finishes "
						"inside that window, and is REFUSED if the occupant outlives it -- and the "
						"commonest occupant is the USER'S OWN production render, which shares that single "
						"slot and routinely runs longer than 30 s. It is also refused with NO wait at all "
						"when a direct parked render holds the gate (that render owns the admission gate "
						"without occupying the render slot, so the wait is satisfied instantly and the "
						"admission check refuses on the spot). A render carrying a film override "
						"(`width` AND `height`) or a camera/`view` override always takes the parked path "
						"and is refused immediately. (`quality:\"draft\"` and `mode:` alone are NOT "
						"overrides for this purpose -- only width+height or a camera/view change the "
						"routing.) WHAT TO DO on \"render_in_progress\": read_viewport is FREE and becomes "
						"available the instant the gate clears, so ONE short retry of read_viewport is the "
						"cheap poll, not `render`. If a second read still reports it, a long render (most "
						"likely the user's own) owns the gate -- tell the user and wait for it rather than "
						"blocking 30 s on a render that will probably be refused anyway. Call a plain "
						"`render {}` only when you genuinely need a NEW image rather than the viewport's, "
						"and budget for that block.",
						ObjectProp( "", props, std::vector<std::string>() ) ) );
				}

				// query_object_at (Toolkit slice 3b)
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "x", NumberProp( "REQUIRED integer pixel X coordinate, in the EFFECTIVE film dims (the width/height override below when both are given, else the scene's authored dims)." ) );
					props.set( "y", NumberProp( "REQUIRED integer pixel Y coordinate, same effective-dims rule as `x`." ) );
					props.set( "width",  NumberProp( "OPTIONAL transient film-width override in pixels, CLAMPED to [16,512]. Must be paired with `height`. Composes exactly like render's own width/height override -- NEVER mutates the scene document." ) );
					props.set( "height", NumberProp( "OPTIONAL transient film-height override in pixels, CLAMPED to [16,512]. Must be paired with `width`." ) );
					props.set( "camera", CameraOverrideSchema() );
					std::vector<std::string> required; required.push_back( "x" ); required.push_back( "y" );
					tools.push_back( MakeTool( "query_object_at",
						"Identify WHICH single object is at one pixel (x,y) -- the cheap, "
						"single-answer alternative to render mode:\"objectmap\" when you just "
						"need to name or locate ONE object (e.g. before moving/editing it) rather "
						"than see the whole segmentation. Returns "
						"{hit,name,kind,pixelX,pixelY,width,height,message}: hit is false (a "
						"NORMAL result, not an error) when the pixel is empty background -- name "
						"is \"\" in that case. `width`/`height`/`camera` compose EXACTLY like "
						"render's own overrides (ephemeral, captured and restored, never touch "
						"the document) -- use `camera` to aim at an object, then query its known "
						"screen position. An out-of-range x/y for the EFFECTIVE dims is a clean "
						"error, not a silent hit:false. `name` is the SAME legend name a "
						"mode:\"objectmap\" render would report for that pixel (the same "
						"instance-array `grid[i,j]`-is-not-a-CST-chunk caveat applies -- target "
						"the GENERATOR chunk to edit it, not the instance name). Works even on a "
						"scene with no active production rasterizer (it runs its own cheap "
						"identity render internally, costing about one small render, NOT a full "
						"beauty render).",
						ObjectProp( "", props, required ) ) );
				}

				// scene_inventory (Arc 80, 2026-08-12) -- the FORWARD
				// counterpart to query_object_at.  Its text MIRRORS
				// AgentChatCodecs.cpp's entry sentence for sentence, per this
				// file's canonical-codec convention.
				{
					JsonValue props = JsonValue::MakeObject();
					tools.push_back( MakeTool( "scene_inventory",
						"Ask WHERE EVERYTHING IS: one line per object in the scene, saying how much "
						"of the frame it covers and where it sits. Takes no arguments -- it "
						"inventories the ACTIVE camera's view. This is the FORWARD question; "
						"query_object_at is the inverse one (\"what is at this pixel\"), which can "
						"only answer about a spot you already guessed, and whose \"no object at this "
						"pixel\" tells you nothing about where anything actually is. An object that "
						"covered pixels is reported with its pixel count, its share of the frame, and "
						"its position in the frame as fractions from the left and from the top. An "
						"object that covered none is reported with its world bounding-box centre and "
						"where it lies relative to the view: entirely behind the camera, entirely "
						"off-frame (with the direction), overlapping the frame but covering no pixels "
						"(occluded, or smaller than one pass pixel -- this does not distinguish "
						"those), or crossing the camera plane. Positions are measured in a small "
						"one-ray-per-pixel identity pass, so a footprint under one pass pixel reads "
						"as 0, and the pass's dimensions are reported. Where an object lies relative "
						"to the view is NOT computed -- and says so -- when the active camera is not "
						"a pinhole, or the render was taken from a named view or an orientation-only "
						"camera override. YOU USUALLY DO NOT NEED TO CALL THIS: the same inventory "
						"comes back automatically, in the same words, on every full-scene "
						"beauty render's result under `inventory`. Returns "
						"{ok,objects,covered,passWidth,passHeight,framePositionComputed,"
						"framePositionNote,entries,notAreaSampled,text,message}. `notAreaSampled` is "
						"an array of {name,reason} for emissive objects this render measured as not "
						"selected by light sampling (their emission still shows on direct view and "
						"BSDF hits); omitted when empty. It renders internally (one cheap "
						"identity pass), changes nothing in the document, and does not replace the "
						"image read_image returns.",
						ObjectProp( "", props, std::vector<std::string>() ) ) );
				}

				// compare_to_reference (the reconstruction feedback instrument)
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "reference", StringProp( "REQUIRED. The name of a HOST-registered reference image (e.g. the eval harness's \"view1\", \"view2\", ... prompt-attachment naming contract, in prompt-then-attachment order). An unknown name is an error listing every registered reference name -- there is no way to compare against an arbitrary path; only images the host explicitly registered are reachable." ) );
					props.set( "camera", CameraOverrideSchema() );
					props.set( "visual", BoolProp( "OPTIONAL, default true. When true, ALSO returns a composite [render | reference | abs-diff heatmap] side-by-side PNG (3x the reference's width) as a real image content block, using the SAME mechanism read_image uses. Set false once you only need the numeric feedback (rmse/channelDelta/grid) -- saves the encode cost and the response's token footprint." ) );
					props.set( "samples", NumberProp( "OPTIONAL sample-count override, CLAMPED to [1,16] -- an agent-surface cap, same as the render tool's own samples cap. IMPORTANT QUALITY TRADEOFF: omit this (the default) and the comparison renders at quality:\"draft\" -- cheap, but the draft pipeline IGNORES the scene's authored materials and lighting entirely, so a low draft-mode RMSE only confirms geometry/composition/camera alignment, NOT colour or material match. Supplying `samples` switches the comparison to quality:\"production\" at that sample count -- the real, grader-equivalent RMSE reading, and materially more expensive. Recommended workflow: iterate cheaply under the draft default while getting composition/placement right, then pass `samples` (e.g. 8-16) for the real measurement once composition looks plausible. A requested value above 16 is silently clamped -- the result's `agentRenderCap` object names the cap when it fires." ) );
					props.set( "split", BoolProp( "OPTIONAL, default false. When true, ALSO returns split:{objectRmse,backgroundRmse,objectPixelFraction,ok,note} -- an object-vs-background RMSE breakdown built from a SECOND, ephemeral mode:\"objectmap\" render of your OWN candidate (an extra render, so it costs more). Use it once your overall `rmse` plateaus across iterations: a high `backgroundRmse` means your staging (ground/environment/lighting) is still the biggest lever; a low `backgroundRmse` with a high `objectRmse` means staging is DONE -- stop tuning it and spend remaining iterations on the object's silhouette/proportions instead. HONESTY CAVEAT: the object mask comes from YOUR candidate only (the reference is a plain PNG with no objectmap of its own) -- it answers \"on the pixels where my object is, how wrong am I\" and \"on my background pixels, how wrong am I\", not \"how wrong is the reference's object region\". A badly misplaced object still shows up: high objectRmse on the candidate's (wrong) object pixels, AND the reference's actual object pixels raise backgroundRmse too, since your candidate has no object there. Both figures sentinel to -1 when their bucket is EMPTY -- objectRmse is -1 when no object pixels are visible (camera pointed away, object off-frame), backgroundRmse is -1 when registered objects cover the ENTIRE frame -- so ALWAYS check for >= 0 before trusting either; -1 means \"not measured\", NOT \"perfect match\". IMPORTANT CAVEAT ABOUT WHAT COUNTS AS \"OBJECT\": without `splitObjects` (below), EVERY registered object counts as OBJECT -- including a ground plane, backdrop, or any other staging geometry you built as a real scene object. That means an unscoped split measures \"geometry vs. environment\", not \"hero object vs. staging\": a scene with a modeled ground plane can show a huge OBJECT bucket (observed averaging 86% of the frame in practice) that is mostly stage, not your hero object. Pass `splitObjects` naming just your hero object to get a true hero-vs-staging reading." ) );
					props.set( "splitObjects", StringArrayProp( "OPTIONAL array of object names, only meaningful alongside `split:true`. When non-empty, SCOPES the OBJECT bucket to ONLY the named registered object(s) -- every other pixel, INCLUDING other registered geometry like a ground plane or backdrop, falls into BACKGROUND instead. Use this to get a true hero-object-vs-staging reading: without it, a ground plane/backdrop you modeled as a scene object counts as OBJECT too (see the `split` parameter's own caveat), which inflates the OBJECT bucket and starves BACKGROUND down to just the sky/environment. Names are matched against the candidate's own objectmap legend; a name not found there is dropped from the mask (never a hard failure) and is instead surfaced in split.note, along with every name that IS available, so a typo doesn't silently shrink your mask unnoticed. If NONE of the requested names match, objectRmse comes back -1 with a note explicitly saying the named object(s) don't exist in this scene -- distinct from the ordinary \"object off-frame\" -1 case." ) );
					std::vector<std::string> required; required.push_back( "reference" );
					tools.push_back( MakeTool( "compare_to_reference",
						"Measure how closely the current scene's render matches a reference photo -- "
						"the SAME RMSE objective function an image-reconstruction grader uses, handed "
						"to you directly instead of leaving you to render-then-eyeball. Renders the "
						"live scene at the NAMED reference's exact pixel dimensions (no width/height "
						"override -- the comparison needs pixel-for-pixel alignment) and returns "
						"{rmse, channelDelta:{r,g,b}, grid, worstCell, width, height, reference, "
						"summary}. `rmse` = sqrt(mean((render-reference)/255)^2) over all pixels -- "
						"lower is better, 0 is a perfect match; treat this as the primary objective "
						"to minimize, not a vague color the render \"looks close\". `channelDelta` is "
						"the mean SIGNED per-channel difference (render minus reference, [-1,1]) -- "
						"positive means your render runs brighter than the reference on that channel "
						"(e.g. channelDelta.b > 0 means your render is too blue). `grid` is a 3x3 "
						"ROW-MAJOR array (index 0 = top-left ... index 8 = bottom-right; row = "
						"index/3, col = index%3) of {rmse,dr,dg,db} giving the SAME two measures "
						"broken down spatially -- use this to find WHICH region of the frame is "
						"worst (background/environment staging is the most common weak spot) rather "
						"than guessing from the single overall number; `worstCell` names that "
						"region directly (e.g. \"top-right\"). `summary` is a one-line human-readable "
						"synthesis of all of the above. See the `samples` parameter's own description "
						"for the draft-vs-production quality tradeoff -- read it before relying on a "
						"reading for anything beyond composition/geometry. See the `split` and "
						"`splitObjects` parameters' own descriptions for the object-vs-background "
						"RMSE breakdown -- reach for it once your RMSE plateaus, to tell whether the "
						"residual is staging or the object, and scope it to your hero object's name "
						"for an accurate reading when your scene has a modeled ground plane/backdrop.",
						ObjectProp( "", props, required ) ) );
				}

				// list_proposals (Secure-MCP slice 5b)
				{
					tools.push_back( MakeTool( "list_proposals",
						"List every proposal staged on the live scene's proposal queue (pending AND "
						"resolved -- resolved entries stay for audit). Returns "
						"{proposals:[{id,kind,target,entityKind,param,value,chunkText,baseVersion,"
						"sessionLabel,status},...]}: `kind` is one of \"param_edit\"/\"insert_chunk\"/"
						"\"remove_chunk\"/\"remove_chunks\" (the batch remove stages as ONE entry); "
						"`status` is \"pending\"/\"applied\"/\"rejected\"/\"conflict\". "
						"READ-ONLY and available regardless of this session's autonomy posture -- "
						"listing the queue is not a mutation. Requires a scene to be loaded with a live "
						"controller attached (a headless CLI session with no in-app GUI owner returns "
						"an empty list, not an error -- there is no queue to list against).",
						ObjectProp( "", JsonValue::MakeObject(), std::vector<std::string>() ) ) );
				}

				// resolve_proposal (Secure-MCP slice 5b)
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "proposalId", NumberProp( "The id of a proposal returned by list_proposals." ) );
					props.set( "approve", BoolProp( "true to approve (apply the staged edit now, re-checking its baseVersion against the current head); false to reject (no mutation)." ) );
					std::vector<std::string> required;
					required.push_back( "proposalId" ); required.push_back( "approve" );
					const std::string desc = ( resolveProposalRefused ? kResolveProposalOwnerOnlyNote : std::string() ) + std::string(
						"Approve or reject a staged proposal. OWNER-ONLY: this call is refused for any "
						"session that is not the document owner, including a session resolving a "
						"proposal it staged itself -- an external/proposing agent can list and poll "
						"proposals but never approve or reject one. Returns {resolved,retriable,status,"
						"headVersion,message}: resolved is true only when the resolve actually ran, "
						"i.e. the proposal left the pending state; retriable (meaningful only when "
						"resolved is false) is true when a TRANSIENT gate refused -- a render in "
						"progress, or an open editor transaction/gesture -- in which case nothing was "
						"applied and the proposal is STILL PENDING, so the same call works once that "
						"clears; status is \"applied\"/\"rejected\"/"
						"\"conflict\" on a real resolve (approve RE-CHECKS the proposal's staged "
						"baseVersion against the current head -- a proposal staged against a head that "
						"has since moved resolves to \"conflict\", not applied) and is empty when "
						"resolved is false." );
					tools.push_back( MakeTool( "resolve_proposal", desc, ObjectProp( "", props, required ) ) );
				}

				return tools;
			}

			//! Build the internal AgentRpc JSON-RPC request line for a
			//! `tools/call {name,arguments}` -- reusing the wrapped
			//! dispatcher's OWN method names and param shapes verbatim (this
			//! adapter performs NO param translation: MCP tool `arguments`
			//! ARE the AgentRpc `params` object, unchanged).
			std::string BuildInternalRequest( const std::string& toolName, const JsonValue& arguments )
			{
				JsonValue req = JsonValue::MakeObject();
				req.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
				req.set( "id", JsonValue::MakeNumber( 1.0 ) );   // internal id -- discarded, never surfaced to the MCP caller
				req.set( "method", JsonValue::MakeString( toolName ) );
				req.set( "params", arguments );
				return JsonSerialize( req );
			}

			//! MCP CallToolResult: {content:[...], isError}.
			JsonValue MakeCallToolResult( const JsonValue& content, bool isError )
			{
				JsonValue result = JsonValue::MakeObject();
				result.set( "content", content );
				result.set( "isError", JsonValue::MakeBool( isError ) );
				return result;
			}

			//! A {type:"text", text:...} MCP content block.
			JsonValue TextBlock( const std::string& text )
			{
				JsonValue b = JsonValue::MakeObject();
				b.set( "type", JsonValue::MakeString( "text" ) );
				b.set( "text", JsonValue::MakeString( text ) );
				return b;
			}

			//! A {type:"image", data:<base64>, mimeType:...} MCP content block.
			JsonValue ImageBlock( const std::string& base64Data, const std::string& mimeType )
			{
				JsonValue b = JsonValue::MakeObject();
				b.set( "type", JsonValue::MakeString( "image" ) );
				b.set( "data", JsonValue::MakeString( base64Data ) );
				b.set( "mimeType", JsonValue::MakeString( mimeType ) );
				return b;
			}

			//! The list of the 34 tool names this adapter recognizes --
			//! shared between tools/list and tools/call's unknown-name check.
			bool IsKnownToolName( const std::string& name )
			{
				static const char* const kNames[] = {
					"read_document", "read_schema", "read_skill", "validate",
					"file_build_plan",   // G2 (2026-08-10): read-safe, the build-plan gate's unblock
					"finish_element",   // S1 (2026-08-11): read-safe, closes the active element (renders)
					"reopen_element",   // S1 (2026-08-11): read-safe, re-enters an element's window
					"imagine_scene",    // Arc 77 Phase 2 (2026-08-11): read-safe, the gate's OTHER unblock
					"propose_patch", "propose_patches", "insert_chunk", "insert_chunks",
					"insert_material_scaffold", "insert_geometry_scaffold",
					"replace_geometry_scaffold",   // R2 (2026-08-10): one-call form revision
					"remove_chunk",
					"remove_chunks",
					"render", "render_status", "render_wait", "render_cancel",
					"read_image", "read_viewport", "query_object_at",
					"scene_inventory",   // Arc 80 (2026-08-12): read-safe, the FORWARD "where is everything" inventory
					"light_scene",       // Arc 81 (2026-08-12): MUTATING, the clean-room lighting pass
					"populate_scene",    // Arc 82 (2026-08-12): MUTATING, the clean-room population pass
					"environment_scene", // Arc 83 slice 5 (2026-08-13): MUTATING, the clean-room environment pass
					"frame_scene",       // Arc 83 slice 6 (2026-08-13): MUTATING, the clean-room framing pass
					"compare_to_reference",
					"list_proposals", "resolve_proposal"
				};
				for( const char* n : kNames ) if( name == n ) return true;
				return false;
			}
		}

		AgentMcpAdapter::AgentMcpAdapter( std::unique_ptr<AgentSession> session, AgentAutonomy autonomy )
			: mDispatcher( new AgentRpcDispatcher( std::move( session ), autonomy ) )
			, mAutonomy( autonomy )
		{
		}

		AgentMcpAdapter::~AgentMcpAdapter()
		{
		}

		std::string AgentMcpAdapter::HandleLine( const std::string& mcpRequestLine )
		{
			// The id defaults to null: a request that fails to parse (or
			// whose envelope is invalid before we can read an id) responds
			// with id=null, matching AgentRpc's own convention.
			JsonValue idValue = JsonValue::MakeNull();

			try {
				// (1) Parse the line -> -32700 on malformation.
				JsonValue req;
				std::string parseErr;
				if( !JsonParse( mcpRequestLine, req, parseErr ) ) {
					return MakeError( idValue, kParseError, "Parse error: " + parseErr );
				}

				// (2) MCP 2025-03-26 dropped JSON-RPC batching: a top-level
				// ARRAY envelope is rejected cleanly rather than silently
				// processing (or crashing on) only the first element.
				if( req.isArray() ) {
					return MakeError( idValue, kInvalidRequest,
						"Invalid Request: batch (array) requests are not supported (MCP 2025-03-26 dropped JSON-RPC batching)" );
				}

				// (3) Envelope must be an object -> else -32600.
				if( !req.isObject() ) {
					return MakeError( idValue, kInvalidRequest, "Invalid Request: not a JSON object" );
				}

				// (4) Echo the id if present (number / string / null are the
				// valid id types; anything else we treat as absent -> null).
				bool hasId = false;
				if( const JsonValue* id = req.find( "id" ) ) {
					if( id->isNumber() || id->isString() || id->isNull() ) { idValue = *id; hasId = true; }
				}

				// (5) `method` must be a string -> else -32600.
				const JsonValue* method = req.find( "method" );
				if( !method || !method->isString() ) {
					return MakeError( idValue, kInvalidRequest, "Invalid Request: missing or non-string 'method'" );
				}
				const std::string& m = method->asString();

				// (6) `params` is optional; when present it must be an object.
				JsonValue params = JsonValue::MakeObject();
				if( const JsonValue* p = req.find( "params" ) ) {
					if( p->isObject() ) params = *p;
					else if( !p->isNull() )
						return MakeError( idValue, kInvalidParams, "Invalid params: 'params' must be an object" );
				}

				//----------------------------------------------------------
				// notifications/initialized -- a TRUE MCP notification: no
				// `id` field at all means no response line, full stop.  This
				// is the one place this adapter's contract diverges from
				// AgentRpcDispatcher's "always respond" convention (that
				// dispatcher's own doc says a request with no id STILL gets
				// a response; MCP notifications never do).  We key strictly
				// on the ABSENCE of an `id` field (not merely id==null,
				// which IS a valid, response-expecting request id in
				// JSON-RPC) so this only fires for genuine notifications.
				//----------------------------------------------------------
				// Any notification (no `id` field at all -- including, but
				// not limited to, "notifications/initialized") is not
				// responded to: MCP callers may send other notifications
				// this adapter does not act on (e.g. future $/cancelled-
				// style additions); silently dropping rather than erroring
				// keeps a forward-compatible posture without inventing
				// behaviour for methods this slice does not implement. A
				// request that merely REUSES the "notifications/..." method
				// NAME but supplies a real `id` is NOT a notification by the
				// JSON-RPC/MCP contract -- it falls through to the ordinary
				// method dispatch below (and, since this adapter does not
				// register a handler for that name, ends up at the
				// method-not-found fallback -- which is the correct,
				// honest outcome for "you gave this an id, so it wasn't a
				// notification, and I don't have a request-shaped handler
				// for it").
				if( !hasId ) {
					return std::string();
				}

				//----------------------------------------------------------
				// initialize -> capability handshake (handled ENTIRELY here;
				// never reaches the wrapped dispatcher, which has no concept
				// of MCP capabilities).
				//----------------------------------------------------------
				if( m == "initialize" ) {
					std::string echoedVersion = kProtocolVersion;
					if( const JsonValue* pv = params.find( "protocolVersion" ) ) {
						if( pv->isString() && pv->asString() == kProtocolVersion ) {
							echoedVersion = pv->asString();
						}
						// An unrecognized client protocolVersion: honestly
						// report OUR baseline (not the client's unrecognized
						// one) so the client can decide whether to proceed --
						// matches the file header's documented contract.
					}

					JsonValue capabilities = JsonValue::MakeObject();
					capabilities.set( "tools", JsonValue::MakeObject() );   // {} = tools capability present, no sub-options

					int major = 0, minor = 0, revision = 0, build = 0;
					bool isDebug = false;
					RISE_API_GetVersion( &major, &minor, &revision, &build, &isDebug );
					char versionBuf[64];
					std::snprintf( versionBuf, sizeof( versionBuf ), "%d.%d.%d", major, minor, revision );

					JsonValue serverInfo = JsonValue::MakeObject();
					serverInfo.set( "name", JsonValue::MakeString( "rise" ) );
					serverInfo.set( "version", JsonValue::MakeString( versionBuf ) );

					JsonValue result = JsonValue::MakeObject();
					result.set( "protocolVersion", JsonValue::MakeString( echoedVersion ) );
					result.set( "capabilities", capabilities );
					result.set( "serverInfo", serverInfo );
					return MakeSuccess( idValue, result );
				}

				//----------------------------------------------------------
				// ping -> MCP spec: the server MUST respond with an empty
				// object result (no capability/session semantics attached;
				// a bare liveness check).
				//----------------------------------------------------------
				if( m == "ping" ) {
					return MakeSuccess( idValue, JsonValue::MakeObject() );
				}

				//----------------------------------------------------------
				// tools/list -> the 29 verbs as MCP tools.
				//----------------------------------------------------------
				if( m == "tools/list" ) {
					JsonValue result = JsonValue::MakeObject();
					result.set( "tools", BuildToolsList( mAutonomy ) );
					return MakeSuccess( idValue, result );
				}

				//----------------------------------------------------------
				// tools/call {name, arguments} -> dispatch through the
				// wrapped AgentRpcDispatcher and re-wrap as a CallToolResult.
				//----------------------------------------------------------
				if( m == "tools/call" ) {
					const JsonValue* nameVal = params.find( "name" );
					if( !nameVal || !nameVal->isString() ) {
						return MakeError( idValue, kInvalidParams, "Invalid params: 'name' (string) is required" );
					}
					const std::string toolName = nameVal->asString();

					// An unknown tool name is a PROTOCOL error (the caller
					// asked for a tool that does not exist) -- NOT a tool-
					// execution error, so this is a JSON-RPC error response,
					// not an isError:true CallToolResult.
					if( !IsKnownToolName( toolName ) ) {
						return MakeError( idValue, kMethodNotFound, "Unknown tool: " + toolName );
					}

					JsonValue arguments = JsonValue::MakeObject();
					if( const JsonValue* a = params.find( "arguments" ) ) {
						if( a->isObject() ) arguments = *a;
						else if( !a->isNull() )
							return MakeError( idValue, kInvalidParams, "Invalid params: 'arguments' must be an object" );
					}

					// Dispatch through the WRAPPED AgentRpcDispatcher --
					// zero param translation, zero verb-semantic changes.
					const std::string internalReq = BuildInternalRequest( toolName, arguments );
					const std::string internalResp = mDispatcher->HandleLine( internalReq );

					JsonValue innerEnv;
					std::string innerErr;
					if( !JsonParse( internalResp, innerEnv, innerErr ) ) {
						// The wrapped dispatcher is documented to NEVER emit
						// malformed JSON; this is a defensive internal-error
						// fallback, not a reachable path in practice.
						return MakeError( idValue, kInternalError,
							"internal error: wrapped dispatcher response failed to parse: " + innerErr );
					}

					// MCP's documented split: a TOOL-EXECUTION error (the
					// wrapped verb itself returned a JSON-RPC error, e.g.
					// propose_patch on an unknown entity, or "no session
					// loaded") becomes a SUCCESS envelope whose result
					// carries isError:true -- NOT a JSON-RPC protocol error.
					// A PROTOCOL error (unknown tool, bad envelope) was
					// already handled above / returns a real JSON-RPC error.
					if( innerEnv.has( "error" ) ) {
						const JsonValue& innerError = innerEnv.get( "error" );
						const std::string errText = JsonSerialize( innerError );
						JsonValue content = JsonValue::MakeArray();
						content.push_back( TextBlock( errText ) );
						return MakeSuccess( idValue, MakeCallToolResult( content, /*isError=*/true ) );
					}

					const JsonValue& innerResult = innerEnv.get( "result" );

					// Whether this result carries an image is DELEGATED to
					// ChatToolResultCarriesImage (AgentChatCodecs.h) -- the
					// ONE predicate every transport shares, so the verb set
					// (read_image, read_viewport, compare_to_reference,
					// file_build_plan, render) and the "png_base64 must be
					// non-empty" field test live in exactly one place and
					// cannot drift back into a private list here.  Before
					// this unification the adapter kept its own hardcoded
					// verb list, and it HAD drifted: it omitted a plain
					// `render{imageMaxEdge}` (see the 2026-08-11 note below).
					//
					// A vision-capable MCP client seeing a real image content
					// block, instead of a base64 string it would have to know
					// to find inside the serialized JSON and decode itself,
					// is the whole reason this branch exists.  read_viewport's
					// available:false and compare_to_reference's visual:false
					// results simply have no (or an empty) png_base64 field,
					// so the predicate's field test naturally excludes them --
					// no special-casing needed.  G3a's file_build_plan result
					// carries the composite SKETCH PNG under the same field
					// name (the point of the echo is that the model SEES what
					// it sketched); G3b's render{target} result carries the
					// [sketch | silhouette | overlay] comparison composite the
					// same way.
					//
					// 2026-08-11: the follow-up spun off during the G3a review
					// landed -- a plain `render{imageMaxEdge}` PNG now rides
					// back as a real image content block too, closing the gap
					// where it reached MCP clients only as base64 text buried
					// inside the serialized JSON.
					//
					// `probe.name` is the only field ChatToolResultCarriesImage
					// reads off the call; the id/argsJson/idSynthesized fields
					// are chat-transport bookkeeping this adapter has no
					// analogue for, so they stay default.  `internalResp` is
					// the SAME raw JSON-RPC response line already parsed into
					// innerEnv above -- the predicate re-parses it, which is
					// fine here (this is not a hot loop).
					ChatToolCall probe;
					probe.name = toolName;
					if( ChatToolResultCarriesImage( probe, internalResp ) )
					{
						JsonValue content = JsonValue::MakeArray();
						// The predicate already proved png_base64 is
						// non-empty, so no `!b64.empty()` guard is needed
						// here (unlike the old hand-rolled condition).
						content.push_back( ImageBlock(
							innerResult.get( "png_base64" ).asString(), "image/png" ) );
						// Metadata as text (byteLength/width/height), and the
						// base64 too (for a client that wants the raw field
						// rather than to decode the image block).
						content.push_back( TextBlock( JsonSerialize( innerResult ) ) );
						return MakeSuccess( idValue, MakeCallToolResult( content, /*isError=*/false ) );
					}

					// Every other verb -- and every image-capable verb's
					// non-image result (a false from the predicate above) --
					// takes this same path: the result JSON, serialized, as a
					// single text block.  That's what makes the unification
					// behavior-preserving for a non-image result: this branch
					// and the one above build the IDENTICAL single TextBlock,
					// so a call the predicate excludes (or never covered) is
					// byte-for-byte what it always returned.
					JsonValue content = JsonValue::MakeArray();
					content.push_back( TextBlock( JsonSerialize( innerResult ) ) );
					return MakeSuccess( idValue, MakeCallToolResult( content, /*isError=*/false ) );
				}

				// (7) Anything else -> method not found.
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
