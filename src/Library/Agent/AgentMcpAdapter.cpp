//////////////////////////////////////////////////////////////////////
//
//  AgentMcpAdapter.cpp - the MCP envelope adapter (see AgentMcpAdapter.h).
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "AgentMcpAdapter.h"

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

			//! The `baseHeadVersion` object shared by propose_patch /
			//! insert_chunk / remove_chunk -- optional optimistic-concurrency
			//! precondition, {uuid,revision} both numeric.
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

			//! GUI render modes P1 (docs/gui/RENDER_MODES.md §8): build the
			//! `mode` = "normals"|"depth"|"facets"|"wireframe" portion of the
			//! render tool's `mode` description FROM
			//! Implementation::GetViewportRenderModes -- filtered to
			//! `casterFactory` entries, each rendered as `"name" (question)` --
			//! so the wording can never drift from the registry the C-ABI, both
			//! GUI dropdowns, and AgentRpc.cpp's mode parser all share (single
			//! source of truth, docs/gui/RENDER_MODES.md §4).
			std::string DescribeViewModes()
			{
				unsigned int modeCount = 0;
				const Implementation::ViewportRenderModeInfo* modes =
					Implementation::GetViewportRenderModes( modeCount );
				std::string out;
				bool first = true;
				for( unsigned int i = 0; i < modeCount; ++i ) {
					if( !modes[i].casterFactory ) continue;
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

			//! Secure-MCP slice 2: the mutating-verb refusal note prepended
			//! to propose_patch/insert_chunk/remove_chunk's descriptions
			//! under AgentAutonomy::Read.  DECIDED: annotate, don't hide --
			//! the tool stays fully visible (real inputSchema, callable
			//! shape) so a client can still explain to its user what the
			//! tool would do and why it is currently refused, rather than
			//! the tool silently disappearing from tools/list.
			const std::string kAutonomyReadNote =
				"[REFUSED under --agent-autonomy=read: this session is read-only; "
				"calling this tool returns a policy-refusal error (relaunch with "
				"--agent-autonomy=commit to enable it)] ";

			//! Secure-MCP slice 5b fix round (P2-1): the sibling annotation for
			//! propose_patch/insert_chunk/remove_chunk under
			//! AgentAutonomy::Propose specifically.  Under Propose these three
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

			//! Build the `tools/list` result: the 16 existing AgentRpc verbs,
			//! each carrying an inputSchema faithful to AgentRpc.cpp's ACTUAL
			//! parsing, and a description mined from AgentRpc.h's verb-doc
			//! comments for the gotchas an external MCP client needs (paired
			//! width/height, camera vector shapes, the async-refused-headless
			//! note, pinned semantics, samples clamp, the ODD/EVEN id-space
			//! split, the baseHeadVersion conflict protocol, retriable).
			//! Secure-MCP slice 2: under AgentAutonomy::Read, the three
			//! mutating tools' descriptions are ANNOTATED (prefixed with
			//! kAutonomyReadNote) rather than hidden -- see the note's doc.
			//! Secure-MCP slice 5b: resolve_proposal is annotated (with the
			//! DISTINCT kResolveProposalOwnerOnlyNote) under BOTH Read and
			//! Propose -- it is refused at the dispatcher under either posture
			//! (see AgentRpc.h); the 3 mutating tools' kAutonomyReadNote
			//! annotation, by contrast, applies ONLY under Read (Propose lets
			//! them reach the session, which stages rather than refuses).
			//! Secure-MCP slice 5b fix round (P2-1): under AgentAutonomy::
			//! Propose, the SAME 3 mutating tools instead get the DISTINCT
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
					"baseHeadVersion on propose_patch/insert_chunk/remove_chunk to guard against "
					"editing a stale head.",
					ObjectProp( "", JsonValue::MakeObject(), std::vector<std::string>() ) ) );

				// read_schema
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "keyword", StringProp( "OPTIONAL chunk keyword (e.g. \"sphere_geometry\"). Omit for the schema of the WHOLE scene-file grammar." ) );
					tools.push_back( MakeTool( "read_schema",
						"Read the JSON Schema for one scene-file chunk keyword, or the whole "
						"grammar when 'keyword' is omitted. STATELESS -- works with no scene loaded. "
						"The descriptor registry IS the accepted-parameter set, so this schema can "
						"never drift from what the parser actually accepts.",
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
						"is rejected.",
						ObjectProp( "", props, std::vector<std::string>() ) ) );
				}

				// validate
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "text", StringProp( "The CANDIDATE scene text to validate (a full .RISEscene document)." ) );
					std::vector<std::string> required; required.push_back( "text" );
					tools.push_back( MakeTool( "validate",
						"Validate a CANDIDATE scene text with NO side effects: parses it to a CST "
						"and derives it into a throwaway scene, returning structured diagnostics "
						"{severity,code,message,offset,length}. STATELESS -- works with no scene "
						"loaded, so an agent can validate a from-scratch scene before any head "
						"exists. An empty diagnostics array means no errors were found.",
						ObjectProp( "", props, required ) ) );
				}

				// propose_patch
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "target", StringProp( "The entity NAME to edit (a chunk's `name` param); an unnamed camera resolves positionally if it is the sole one." ) );
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
						"verbatim can never succeed. headVersion is always the head AFTER this call." );
					tools.push_back( MakeTool( "propose_patch", desc, ObjectProp( "", props, required ) ) );
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
						"with a clean message. A SUCCESSFUL insert (applied=true) may still "
						"return `unresolvedReferences`: [{param,value,suggestions:[...]}] -- this "
						"is a WARNING, NOT a rejection. It means the just-inserted chunk names a "
						"painter/material/geometry/etc. that is not defined ANYWHERE in the "
						"document (yet). If you are about to insert that missing chunk next "
						"(a legitimate forward reference), this is fine and needs no action. "
						"Otherwise, insert the missing chunk or correct the misspelled name -- "
						"check `suggestions` first, it lists near-miss names already defined in "
						"the document." );
					tools.push_back( MakeTool( "insert_chunk", desc, ObjectProp( "", props, required ) ) );
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
						"dry-run and is rejected with the diagnostic (document left byte-identical)." );
					tools.push_back( MakeTool( "remove_chunk", desc, ObjectProp( "", props, required ) ) );
				}

				// render
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "samples", NumberProp( "OPTIONAL sample-count override, CLAMPED to [1,65536] (out-of-range values are clamped, not rejected). Omit for the scene-authored sample count. Only honoured by rasterizers that support a sample-count override (the pixel-based family: PT, spectral PT, BDPT, VCM) -- see the result's `samplesOverridden`/`effectiveSamples` fields; on an unsupported rasterizer (MLT, photon-map-only, Auto's outer wrapper) the override is honestly reported as NOT applied, never silently ignored." ) );
					props.set( "width",  NumberProp( "OPTIONAL transient film-width override in pixels, CLAMPED to [16,512]. Must be paired with `height` -- supplying only one is ignored (ambiguous aspect ratio), not applied. NEVER mutates the scene document; restored after the render." ) );
					props.set( "height", NumberProp( "OPTIONAL transient film-height override in pixels, CLAMPED to [16,512]. Must be paired with `width`." ) );
					props.set( "camera", CameraOverrideSchema() );
					props.set( "pinned", BoolProp( "OPTIONAL, default false. When true, this render cannot be silently superseded by a later render submission while it is in flight (it still responds to an explicit cancel or teardown) -- meaningful only against a live in-app GUI session's controller; has no effect in headless `rise --agent-stdio`." ) );
					props.set( "quality", StringProp( "OPTIONAL, \"draft\" or \"production\" (default \"production\" -- today's exact behaviour). \"draft\" renders through a wholly SEPARATE, cheap studio-preview pipeline (the SAME fixed preview shader the GUI's live interactive editor uses) that IGNORES the scene's authored materials and lighting entirely -- geometry, composition, and camera framing are representative; materials, lighting, exposure, and colour are NOT. NEVER judge materials/lighting/exposure/colour from a draft image -- use quality:\"production\" (or read_viewport) for that. A draft render CAPS samples at 4 regardless of the requested `samples` value. Check the result's `renderMode` field (\"production\"/\"draft\") to see which pipeline actually ran -- `integrator` always names the head's active PRODUCTION rasterizer regardless of `quality`, so it is NOT the field to check for this." ) );
					props.set( "mode", StringProp( std::string(
						"OPTIONAL, \"beauty\" (default), \"objectmap\", or one of the view-mode diagnostic names below. "
						"\"objectmap\" renders a flat per-object IDENTITY segmentation -- each scene object painted a distinct high-contrast colour, no lighting/materials -- and adds a `legend` array of {name,colorHex,pixelCount} to the result. Use it to reason about WHICH object is at WHICH pixel and how much of the frame each covers (occlusion, placement, framing). IMPORTANT: read the objectmap image at NATIVE size -- do NOT pass read_image's maxEdge, since box-downscaling blends the identity colours and corrupts colorHex matching. "
						"The view-mode names, each a single-pass false-colour diagnostic render (no `legend`, unlike objectmap): " ) + DescribeViewModes() + std::string(
						". `quality` and `samples` are IGNORED under objectmap or any view mode (each has exactly one fidelity: 1 sample/pixel). Check the result's `renderMode` field to confirm which one actually ran -- it echoes back the exact mode name (e.g. \"normals\"), distinguishing it from \"objectmap\"/\"draft\"/\"production\". Orthogonal to `quality`: these modes are about geometry/segmentation, draft is about cheap studio shading. Known limitation (wireframe): triangle-mesh edges only -- analytic primitives (sphere/box/SDF) and unmeshed geometry render as dim facet shading with no lines, which is correct behaviour, not a bug. Known limitation (depth): brightness is normalized PER RENDER to the VISIBLE hit-distance range in that frame (auto-calibrated -- a wide shot and a close-up of the same scene use DIFFERENT brightness scales, so never compare depth renders across two different camera framings as if they shared a scale), self-calibrating within a single render call, falling back to the scene's bounding-box diagonal only for a degenerate/empty scene. INSTANCE NAMES (objectmap only): a generator-synthesized legend name (e.g. \"grid[0,1]\" from an instance_array) identifies the instance in the map but is NOT a CST chunk -- to EDIT it, target the GENERATOR chunk (strip the \"[i,j]\" suffix, e.g. \"grid\"), since propose_patch/remove_chunk on the instance name will fail." ) ) );
					props.set( "xray", BoolProp(
						"OPTIONAL, default TRUE. ONLY meaningful when `mode` is one of the view-mode diagnostics (normals/depth/facets/wireframe) -- resolves the ray THROUGH transmissive (glass-like) surfaces to the first OPAQUE hit, following a STRAIGHT LINE with NO refraction bending (deliberately an x-ray, not an optics simulation), up to 16 surfaces skipped. On by default -- a view-mode render sees through glass and other transparent geometry to what's underneath/inside it (e.g. mode:\"depth\" or mode:\"facets\" showing the mechanism under a crystal/cover with no extra argument needed). Pass xray:false to inspect the transmissive surface itself instead. If the ray never reaches an opaque surface after 16 skips, the LAST transmissive surface is shown instead of a black hole -- an honest partial answer. Silently IGNORED under mode:\"beauty\"/\"objectmap\" (noted in the result message)." ) );
					tools.push_back( MakeTool( "render",
						"Render the current scene head SYNCHRONOUSLY and return {ok,width,height,"
						"meanR,meanG,meanB,integrator,previewWidth,previewHeight,cameraOverridden,"
						"message,renderJobId,samplesOverridden,effectiveSamples,renderMode} (plus a "
						"per-object `legend` when mode:\"objectmap\"). Does NOT "
						"return image bytes -- call read_image afterward for the rendered PNG. "
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
						"NOTE: this adapter's headless `rise --agent-stdio --mcp` process has no "
						"live in-app controller, so the async submission mode that the underlying "
						"RPC surface supports (`{\"async\":true}`) is NOT exposed as an option here "
						"-- every render through this tool is fully synchronous and blocks until "
						"complete; there is no async/pinned-supersession semantics reachable from "
						"this headless transport beyond the advisory `pinned` flag above (which is "
						"a no-op without a controller).",
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
					props.set( "maxEdge", NumberProp( "OPTIONAL long-edge bound in pixels, CLAMPED to [16,1024]. Downscales the cached image (box filter, aspect-preserving, NEVER upscales) before encoding -- no re-render. Omit for the native render resolution." ) );
					tools.push_back( MakeTool( "read_image",
						"Read the last successful render's image. Returns an MCP image content "
						"block (inline PNG, so an MCP vision-capable client sees the rendered frame "
						"directly) PLUS a text block with the metadata "
						"{png_base64,byteLength,width,height} (the same fields the underlying RPC "
						"verb returns, for a client that wants the raw base64/dims rather than the "
						"image block). Call `render` at least once first -- before any render this "
						"returns an image for whatever is cached (empty/default if nothing has "
						"rendered yet). IF THE LAST RENDER WAS mode:\"objectmap\": read at NATIVE "
						"size -- OMIT maxEdge. Downscaling box-blends the flat identity colours, "
						"corrupting the exact-byte legend match (maxEdge is for beauty/draft only).",
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
						"width,height}. When `available` is false there is no image: `reason` is "
						"\"no_controller\" (this session has no live GUI viewport -- e.g. a headless "
						"run) or \"no_frame_yet\" (the viewport exists but has not rendered a frame "
						"yet). available:false is a normal result, not an error -- do not retry "
						"blindly; a headless session will never have a viewport.",
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

				// compare_to_reference (the reconstruction feedback instrument)
				{
					JsonValue props = JsonValue::MakeObject();
					props.set( "reference", StringProp( "REQUIRED. The name of a HOST-registered reference image (e.g. the eval harness's \"view1\", \"view2\", ... prompt-attachment naming contract, in prompt-then-attachment order). An unknown name is an error listing every registered reference name -- there is no way to compare against an arbitrary path; only images the host explicitly registered are reachable." ) );
					props.set( "camera", CameraOverrideSchema() );
					props.set( "visual", BoolProp( "OPTIONAL, default true. When true, ALSO returns a composite [render | reference | abs-diff heatmap] side-by-side PNG (3x the reference's width) as a real image content block, using the SAME mechanism read_image uses. Set false once you only need the numeric feedback (rmse/channelDelta/grid) -- saves the encode cost and the response's token footprint." ) );
					props.set( "samples", NumberProp( "OPTIONAL sample-count override, CLAMPED to [1,65536]. IMPORTANT QUALITY TRADEOFF: omit this (the default) and the comparison renders at quality:\"draft\" -- cheap, but the draft pipeline IGNORES the scene's authored materials and lighting entirely, so a low draft-mode RMSE only confirms geometry/composition/camera alignment, NOT colour or material match. Supplying `samples` switches the comparison to quality:\"production\" at that sample count -- the real, grader-equivalent RMSE reading, and materially more expensive. Recommended workflow: iterate cheaply under the draft default while getting composition/placement right, then pass `samples` (e.g. 16-64) for the real measurement once composition looks plausible." ) );
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
						"\"remove_chunk\"; `status` is \"pending\"/\"applied\"/\"rejected\"/\"conflict\". "
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
						"proposals but never approve or reject one. Returns {resolved,status,"
						"headVersion,message}: resolved is true only when this session's authority "
						"permitted the resolve to run at all; status is \"applied\"/\"rejected\"/"
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

			//! The list of the 17 tool names this adapter recognizes --
			//! shared between tools/list and tools/call's unknown-name check.
			bool IsKnownToolName( const std::string& name )
			{
				static const char* const kNames[] = {
					"read_document", "read_schema", "read_skill", "validate",
					"propose_patch", "insert_chunk", "remove_chunk",
					"render", "render_status", "render_wait", "render_cancel",
					"read_image", "read_viewport", "query_object_at",
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
				// tools/list -> the 17 verbs as MCP tools.
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

					// read_image / read_viewport get special treatment:
					// surface the PNG as a real MCP image content block (so a
					// vision-capable client sees the frame inline) ALONGSIDE a
					// text block with the metadata fields -- a real win over a
					// bare base64 string the client would otherwise have to
					// know to decode and reinterpret itself.  read_viewport's
					// result carries the SAME png_base64 field when
					// available:true; when available:false the field is "" and
					// the image block is simply skipped (the text block still
					// carries {available,reason,...} so the client learns why).
					// compare_to_reference joins this set too: when visual
					// (the default) was requested, its result carries the
					// SAME "png_base64" field name read_image does (the
					// composite [render|reference|heatmap] diff PNG) --
					// deliberately, so this one image-surfacing branch
					// covers it without a separate case. When visual:false
					// was requested there is no png_base64 field at all, so
					// the `!b64.empty()` guard below simply skips the image
					// block and only the text block (rmse/grid/summary/...)
					// is returned -- no special-casing needed here.
					if( toolName == "read_image" || toolName == "read_viewport" ||
					    toolName == "compare_to_reference" )
					{
						JsonValue content = JsonValue::MakeArray();
						const std::string b64 = innerResult.get( "png_base64" ).asString();
						if( !b64.empty() ) {
							content.push_back( ImageBlock( b64, "image/png" ) );
						}
						// Metadata as text (byteLength/width/height), and the
						// base64 too (for a client that wants the raw field
						// rather than to decode the image block).
						content.push_back( TextBlock( JsonSerialize( innerResult ) ) );
						return MakeSuccess( idValue, MakeCallToolResult( content, /*isError=*/false ) );
					}

					// Every other verb: the result JSON, serialized, as a
					// single text block.
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
