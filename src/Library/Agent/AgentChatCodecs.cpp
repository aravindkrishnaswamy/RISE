//////////////////////////////////////////////////////////////////////
//
//  AgentChatCodecs.cpp - provider wire-format codecs for the sans-IO
//    LLM chat loop (see AgentChatCodecs.h).
//
//  Layout:
//    (1) the THIRTEEN provider-neutral tool definitions -- twelve are
//        1:1 with the AgentRpc verbs (parameter names/shapes mirror
//        AgentRpc.cpp); `ask_user` is the one CHAT-LOOP-ONLY exception
//        -- it has no AgentRpc verb and no AgentMcpAdapter tool, it is
//        intercepted by the HOST (GUI drive loop / eval runner) before
//        reaching HandleLine (see AgentChatLoop.cpp / AgentEvalRunner.cpp),
//    (2) a small raw-span JSON scanner (byte-exact extraction of the
//        assistant content from a response body, so provider-opaque
//        fields -- thinking-block signatures -- echo back VERBATIM),
//    (3) the Anthropic Messages API codec,
//    (4) the Gemini v1beta generateContent codec,
//    (5) the OpenAI Chat Completions codec.
//
//  NO LOGGING anywhere in this file: request/response bodies may embed
//  scene content, and the API key must never reach a log.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "AgentChatCodecs.h"

#include "Json.h"

#include <cstring>
#include <map>
#include <string>

namespace RISE
{
	namespace Agent
	{
		namespace
		{
			//------------------------------------------------------------------
			// (1) The provider-neutral tool definitions.
			//
			// One entry per JSON-RPC verb.  `schemaJson` is a JSON-Schema
			// object literal (the subset all providers accept); nullptr
			// means "no parameters" (Anthropic then gets an empty object
			// schema -- input_schema is mandatory there -- while Gemini
			// omits `parameters` entirely).  Descriptions are PRESCRIPTIVE:
			// they say WHEN to call, not just what the verb does.
			//
			// KEEP IN SYNC BY HAND: `render`'s "mode" enum below is a raw
			// string literal (these are `const char*`, not built at runtime),
			// so unlike AgentMcpAdapter.cpp's DescribeViewModes() helper it
			// CANNOT be generated from the registry.  The accepted set is
			// RISE::Implementation::GetViewportRenderModes' casterFactory
			// entries UNION its BeautyVariant entries (IsBeautyVariantMode)
			// + "beauty"/"objectmap" (docs/gui/RENDER_MODES.md §8) -- when a
			// mode is added there, update this literal's enum array and
			// description by hand.
			//------------------------------------------------------------------
			struct NeutralToolDef
			{
				const char* name;
				const char* description;
				const char* schemaJson;   // nullptr = no parameters
			};

			const NeutralToolDef kToolDefs[] =
			{
				{
					"read_document",
					"Read the current scene document. Call this FIRST, before any edit, "
					"to see the live .RISEscene text and the current headVersion "
					"{uuid,revision}. Always pass the headVersion you last read here as "
					"propose_patch's baseHeadVersion. Re-call after a conflict to rebase.",
					nullptr
				},
				{
					"read_schema",
					"Read the scene-language schema (chunk and parameter reference). Call "
					"with a chunk keyword to learn one chunk's parameters. To DISCOVER "
					"which chunk kinds exist under a category (e.g. which material or "
					"geometry types are available), pass category (\"material\", "
					"\"geometry\", \"painter\", \"light\", \"rasterizer\", ...) with NO "
					"keyword -- that returns just the cheap keyword list + one-line "
					"descriptions, so you pick a kind then fetch its full schema by "
					"keyword. Omitting BOTH returns the whole grammar (large; rarely "
					"needed). Use this before proposing a patch whose parameter name or "
					"value format you are not sure about.",
					"{\"type\":\"object\",\"properties\":{"
						"\"keyword\":{\"type\":\"string\",\"description\":"
						"\"A single chunk keyword (e.g. sphere_geometry) to fetch just that chunk's schema.\"},"
						"\"category\":{\"type\":\"string\",\"description\":"
						"\"A chunk category (e.g. material, geometry, painter, light, rasterizer) to CHEAPLY list its chunk keywords + one-line descriptions; omit keyword to use this. Omit both for the whole grammar.\"}"
					"}}"
				},
				{
					"read_skill",
					"Read a scene-authoring skill (curated RISE how-to notes with "
					"verified scene snippets). Call with NO name first to list the "
					"available skills (name + one-line hook each); call with a name to "
					"read one BEFORE authoring or explaining scenes -- the skills carry "
					"the conventions (light directions, power semantics, painter "
					"wiring) that make first-try scenes render correctly.",
					"{\"type\":\"object\",\"properties\":{"
						"\"name\":{\"type\":\"string\",\"description\":"
						"\"A skill name from the index (e.g. scene-skeleton-and-conventions); omit to list all available skills.\"}"
					"}}"
				},
				{
					"validate",
					"Validate a CANDIDATE scene document without touching the live scene. "
					"Call this to check a document you are considering BEFORE proposing "
					"changes; no error-severity diagnostics means the candidate will load "
					"(warnings/info alone are not failures).",
					"{\"type\":\"object\",\"properties\":{"
						"\"text\":{\"type\":\"string\",\"description\":"
						"\"The complete candidate .RISEscene document text to check.\"}"
					"},\"required\":[\"text\"]}"
				},
				{
					"propose_patch",
					"Set one parameter of one named scene entity (the ONLY way to change "
					"a PARAMETER; insert_chunk/remove_chunk add and delete whole "
					"entities). Always pass the headVersion you last read as "
					"baseHeadVersion. If the result has status=conflict, re-call "
					"read_document and re-propose against the new headVersion. If "
					"retriable=true the refusal is transient -- retry the SAME patch "
					"after a moment. Only status=applied means the edit landed cleanly. "
					"status=diagnosed means the edit WAS applied but the re-derive "
					"produced diagnostics -- read them and fix the reported problem; do "
					"not blindly re-propose the same patch. A REJECTED patch may return "
					"\"issues\": [{param,value,reason,suggestions:[...]}] (the same shape "
					"insert_chunk/remove_chunk use). reason is one of \"unknown_target\" "
					"(target does not resolve -- check suggestions for a near-miss name), "
					"\"unknown_param\" (param is not declared on that entity's chunk type "
					"-- message also lists every valid parameter), "
					"\"numeric_in_reference_slot\" (this slot needs the NAME of another "
					"chunk, not a literal number), \"unresolved_reference\" (value names a "
					"chunk not defined anywhere -- check suggestions for a near-miss "
					"already-defined name), or \"invalid_value\" (value is ill-typed for "
					"the parameter -- for an Enum, message lists the allowed values). An "
					"EMPTY or missing issues list on a rejection does not mean the patch "
					"was fine, only that this could not be statically pinned; message "
					"still carries the engine's own diagnostic.",
					"{\"type\":\"object\",\"properties\":{"
						"\"target\":{\"type\":\"string\",\"description\":"
						"\"The entity NAME to edit (a chunk name from the document).\"},"
						"\"kind\":{\"type\":\"string\",\"description\":"
						"\"Optional entity KIND keyword (e.g. lambertian_material) to disambiguate a name clash.\"},"
						"\"param\":{\"type\":\"string\",\"description\":"
						"\"The parameter to set (e.g. radius, color, location).\"},"
						"\"value\":{\"type\":\"string\",\"description\":"
						"\"The new value as scene-language text (e.g. 0.9 0.1 0.1).\"},"
						"\"baseHeadVersion\":{\"type\":\"object\",\"description\":"
						"\"The headVersion from your last read_document -- pass it EVERY time so a stale edit is rejected as a conflict instead of clobbering.\","
						"\"properties\":{\"uuid\":{\"type\":\"number\"},\"revision\":{\"type\":\"number\"}},"
						"\"required\":[\"uuid\",\"revision\"]}"
					"},\"required\":[\"target\",\"param\",\"value\"]}"
				},
				{
					"propose_patches",
					"BATCH form of propose_patch: set MULTIPLE parameters across one or "
					"several named scene entities in ONE call instead of one call per parameter. "
					"USE THIS whenever you are modifying multiple parameters or entities -- "
					"e.g. position, orientation, power, or materials across objects -- "
					"it is one round-trip instead of N. Elements are applied IN ARRAY ORDER. "
					"SEQUENTIAL and BEST-EFFORT: a rejected patch element does NOT stop "
					"the batch -- every remaining element is still attempted in order. Always "
					"pass the headVersion you last read as baseHeadVersion -- it is checked "
					"against the FIRST element only, and if it is STALE the whole batch stops "
					"with every element status=\"conflict\" (nothing was applied): re-read the "
					"document and resubmit. Returns {applied,total,results:[...]}: "
					"total is patches.length, applied is how many results have applied=true, and "
					"each results[i] is the EXACT same shape propose_patch returns.",
					"{\"type\":\"object\",\"properties\":{"
						"\"patches\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{"
							"\"target\":{\"type\":\"string\",\"description\":\"The entity NAME to edit.\"},"
							"\"kind\":{\"type\":\"string\",\"description\":\"Optional entity KIND keyword to disambiguate a name clash.\"},"
							"\"param\":{\"type\":\"string\",\"description\":\"The parameter to set.\"},"
							"\"value\":{\"type\":\"string\",\"description\":\"The new value as scene-language text.\"}"
						"},\"required\":[\"target\",\"param\",\"value\"]},\"description\":"
						"\"An array of patch objects ({target,param,value,kind?}), applied in order.\"},"
						"\"baseHeadVersion\":{\"type\":\"object\",\"description\":"
						"\"The headVersion from your last read_document -- checked against the FIRST element only.\","
						"\"properties\":{\"uuid\":{\"type\":\"number\"},\"revision\":{\"type\":\"number\"}},"
						"\"required\":[\"uuid\",\"revision\"]}"
					"},\"required\":[\"patches\"]}"
				},
				{
					"insert_chunk",
					"ADD one new entity to the live scene by inserting a complete chunk. "
					"chunkText must be EXACTLY ONE `keyword { ... }` block with the braces "
					"on their own lines -- no scene header, no directives, no comments "
					"outside the chunk, one chunk per call. Order matters: an entity must "
					"appear EARLIER in the document than its consumer -- insert_chunk "
					"positions declaration chunks (painters, materials, geometry, "
					"shaders, media) before the objects that consume them automatically "
					"and appends everything else at the end; a rasterizer inserted this "
					"way becomes the ACTIVE integrator (matching save+reload). An insert "
					"that would leave the document unable to derive is refused cleanly. "
					"Unnamed film and rasterizer chunks can NEVER be removed through "
					"this surface once inserted -- insert them deliberately. The SOLE "
					"camera (even unnamed) IS removable via remove_chunk kind=\"camera\", "
					"so to SWAP cameras REMOVE the old one FIRST, THEN insert the "
					"replacement -- insert-first creates two cameras and strands the old "
					"unnamed one (a NAMED second camera stays removable by name) "
					"(the positional fallback requires exactly one). "
					"Unnamed chunks are singletons per keyword EXCEPT append-class kinds "
					"(timeline, keyframe -- see read_schema's unnamedRepeatable), which may "
					"legally repeat unnamed. "
					"Use read_schema for the chunk's parameters; for a BIG "
					"addition, compose the full candidate document and validate it "
					"FIRST, then insert chunk by chunk. Always pass the headVersion you "
					"last read as baseHeadVersion. A duplicate (kind,name) is rejected "
					"-- pick a fresh name. status=applied means the entity is live (a "
					"full re-derive ran); render + read_image to verify. "
					"EITHER a successful OR a rejected insert may return "
					"\"issues\": [{param,value,reason,suggestions:[...]}]. reason is one of "
					"\"unresolved_reference\" (the value names a chunk not defined ANYWHERE "
					"in the document -- fine if you are about to insert that missing chunk "
					"next, a legitimate forward reference; otherwise insert the missing "
					"chunk or correct the misspelled name), \"unknown_param\" (the param "
					"name is not declared on this chunk type -- the message also lists "
					"every valid parameter name), \"numeric_in_reference_slot\" (this slot "
					"needs the NAME of another chunk, not a literal number), or "
					"\"unknown_chunk_type\" (the keyword itself is not registered). On a "
					"SUCCESSFUL insert (applied=true) issues are always "
					"unresolved_reference WARNINGS and do NOT change applied/status; check "
					"`suggestions` first, it lists near-miss names already defined in the "
					"document. On a REJECTED insert, issues explain the cause -- but an "
					"EMPTY issues list on a rejection does not mean the chunk was fine, "
					"only that this could not be statically pinned; the message still "
					"carries the engine's own diagnostic.",
					"{\"type\":\"object\",\"properties\":{"
						"\"chunkText\":{\"type\":\"string\",\"description\":"
						"\"One complete chunk as scene-language text, e.g. omni_light\\n{\\nname key\\nposition 0 5 0\\ncolor 1 1 1\\npower 3.0\\n}\"},"
						"\"baseHeadVersion\":{\"type\":\"object\",\"description\":"
						"\"The headVersion from your last read_document -- pass it EVERY time so a stale edit is rejected as a conflict instead of clobbering.\","
						"\"properties\":{\"uuid\":{\"type\":\"number\"},\"revision\":{\"type\":\"number\"}},"
						"\"required\":[\"uuid\",\"revision\"]}"
					"},\"required\":[\"chunkText\"]}"
				},
				{
					"insert_chunks",
					"BATCH form of insert_chunk: add SEVERAL complete chunks to the live "
					"scene in ONE call instead of one call per chunk. USE THIS instead of "
					"many separate insert_chunk calls whenever you're adding a coherent "
					"group of entities together -- e.g. a painter plus the material that "
					"references it, a geometry, and the object that binds them, or a whole "
					"lighting rig -- it is one round-trip instead of N. Elements are "
					"applied IN ARRAY ORDER; the SAME ordering rule as insert_chunk applies "
					"WITHIN the array: a chunk referenced by a later chunk must appear "
					"BEFORE it (e.g. a painter at index 0 then a material at index 1 that "
					"names it resolves cleanly, because index 0 has already landed by the "
					"time index 1 is applied). SEQUENTIAL and BEST-EFFORT: a rejected "
					"element does NOT stop the batch -- every remaining element is still "
					"attempted (a later chunk that depended on a rejected earlier one will "
					"simply also fail, with its own actionable issues, rather than being "
					"silently skipped). Always pass the headVersion you last read as "
					"baseHeadVersion -- it is checked against the FIRST element only, since "
					"the whole batch is one logical operation. Returns "
					"{applied,total,results:[...]}: total is chunks.length, applied is how "
					"many results have applied=true, and each results[i] is the EXACT same "
					"shape insert_chunk returns for chunks[i] (applied, rawCode, status, "
					"retriable, headVersion, message, name, kind, and optional issues with "
					"the same reasons insert_chunk documents). Check every element's own "
					"status -- do not assume the whole batch succeeded just because the "
					"call returned. Each array element must be EXACTLY ONE `keyword "
					"{ ... }` block, same grammar as insert_chunk's chunkText.",
					"{\"type\":\"object\",\"properties\":{"
						"\"chunks\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":"
						"\"An array of complete chunks, one keyword { ... } block per element (same grammar as insert_chunk's chunkText), applied in order. A chunk referenced by a later element must appear before it in this array -- e.g. a uniformcolor_painter chunk at index 0 followed by a lambertian_material chunk at index 1 that names it in its reflectance parameter.\"},"
						"\"baseHeadVersion\":{\"type\":\"object\",\"description\":"
						"\"The headVersion from your last read_document -- pass it EVERY time so a stale batch is rejected as a conflict instead of clobbering. Checked against the FIRST element only.\","
						"\"properties\":{\"uuid\":{\"type\":\"number\"},\"revision\":{\"type\":\"number\"}},"
						"\"required\":[\"uuid\",\"revision\"]}"
					"},\"required\":[\"chunks\"]}"
				},
				{
					"remove_chunk",
					"DELETE one entity (a whole chunk) from the live scene by name. "
					"Removal is whole-chunk only; the target is the chunk's bare name "
					"(the same addressing as propose_patch), with optional kind (the "
					"chunk keyword or a suffix like material) to narrow a name clash. "
					"A remove is rejected when the remaining document would not derive: "
					"usually the target is still REFERENCED by another chunk (retarget "
					"or remove the consumers first), or the document no longer derives "
					"in order -- read_document and validate to inspect. If a retarget "
					"is refused because the new entity sits later in the document, "
					"remove_chunk the consumer and re-insert it (it will be appended "
					"after the entity it references). Unnamed film and rasterizer "
					"chunks cannot be removed at all -- they have no addressable name; "
					"insert them deliberately. The SOLE camera CAN be removed even "
					"unnamed: pass kind=\"camera\" (the target resolves by position "
					"when exactly one camera exists). To SWAP cameras, remove the old "
					"one FIRST, then insert the replacement -- insert-first creates two "
					"cameras and strands the old unnamed one (a NAMED second camera "
					"stays removable by name). Always "
					"pass the headVersion you last read as baseHeadVersion. There is no "
					"rename verb -- the safe recipe: insert_chunk the renamed entity "
					"(declarations are positioned before their consumers), retarget its "
					"consumers via propose_patch, then remove_chunk the old one. "
					"Removing a repeatable-unnamed kind (timeline, keyframe) by kind alone "
					"while 2+ unnamed instances exist is refused as ambiguous -- address a "
					"named chunk, or use kind only when exactly one unnamed instance remains. "
					"A REJECTED remove may return \"issues\": [{param,value,reason,"
					"suggestions:[...]}] (the same shape propose_patch/insert_chunk use). "
					"The one reason remove_chunk produces is \"still_referenced\": value is "
					"the target's own name and suggestions NAMES every chunk still "
					"referencing it -- edit or remove those first, then retry. An EMPTY or "
					"missing issues list on a rejection does NOT mean the target was "
					"unreferenced -- it may instead be the order-derive cause, or a DYNAMIC "
					"reference (e.g. a timeline naming this entity) this pass cannot see; "
					"message still carries the engine's own hedged diagnostic in that case.",
					"{\"type\":\"object\",\"properties\":{"
						"\"target\":{\"type\":\"string\",\"description\":"
						"\"The bare NAME of the chunk to remove (a chunk name from the document).\"},"
						"\"kind\":{\"type\":\"string\",\"description\":"
						"\"Optional entity KIND keyword (e.g. omni_light) to disambiguate a name clash.\"},"
						"\"baseHeadVersion\":{\"type\":\"object\",\"description\":"
						"\"The headVersion from your last read_document -- pass it EVERY time so a stale edit is rejected as a conflict instead of clobbering.\","
						"\"properties\":{\"uuid\":{\"type\":\"number\"},\"revision\":{\"type\":\"number\"}},"
						"\"required\":[\"uuid\",\"revision\"]}"
					"},\"required\":[\"target\"]}"
				},
				{
					"render",
					"Re-render the live scene headlessly and return lean statistics "
					"(dimensions + linear per-channel means -- a stable image signature "
					"-- plus `integrator`, the ACTIVE rasterizer's chunk keyword, e.g. "
					"pathtracing_pel_rasterizer). Call after a successful propose_patch; "
					"compare the channel means against the previous render to confirm "
					"the edit changed the image. After inserting a rasterizer, check "
					"`integrator` to confirm which one is live. "
					"TOKEN ECONOMY: for modeling/placement checks use width/height 128-192 "
					"(NOT the full authored resolution) and read_image maxEdge ~192 -- a "
					"tiny preview is enough to confirm placement/shape/color and costs a "
					"fraction of the tokens and render time. Use `camera` to check the "
					"scene from 2-3 DIFFERENT ANGLES without editing the actual camera "
					"chunk -- the override is EPHEMERAL (restored after this one render) "
					"and never touches the document, so it's the cheap way to look at a "
					"scene from the side/above/behind before committing to a placement. "
					"For the CHEAPEST possible orientation check (is the geometry/camera "
					"roughly right, nothing else), set quality:\"draft\" -- it renders "
					"through a fixed studio-preview shader that IGNORES the scene's "
					"authored materials and lighting entirely, capped at 4 samples. "
					"Geometry/composition/camera framing ARE representative in a draft "
					"image; materials, lighting, exposure, and colour are NOT -- never "
					"judge those from a draft, and check the result's `renderMode` field "
					"(not `integrator`, which never changes with quality) to confirm which "
					"pipeline actually ran. "
					"Reserve full-size, full-sample, quality:\"production\" renders (the "
					"default; no width/height/camera override) for the FINAL verification "
					"once you're confident the edit is right, and for ANY judgement of "
					"materials/lighting/exposure/colour.",
					"{\"type\":\"object\",\"properties\":{"
						"\"samples\":{\"type\":\"number\",\"description\":"
						"\"Optional sample-count override. Honoured by the pixel-based rasterizer family (PT, spectral PT, BDPT, VCM) via a transient, non-mutating setter -- check the result's samplesOverridden/effectiveSamples fields. On an unsupported rasterizer (MLT, photon-map-only, Auto's outer wrapper) the override is honestly reported as NOT applied, never silently ignored. Under quality:draft this is instead a firm request CAPPED at 4.\"},"
						"\"width\":{\"type\":\"number\",\"description\":"
						"\"Optional TRANSIENT preview width in pixels, clamped to [16,512]. Must be paired with height -- if only one of width/height is given, NEITHER is applied and the render silently proceeds at the scene's authored dimensions (not rejected); check previewWidth/previewHeight in the result to confirm an override took. Does not touch the document -- use 128-192 for cheap placement checks.\"},"
						"\"height\":{\"type\":\"number\",\"description\":"
						"\"Optional TRANSIENT preview height in pixels, clamped to [16,512]. Must be paired with width -- see width's description for the unpaired behavior.\"},"
						"\"camera\":{\"type\":\"object\",\"description\":"
						"\"Optional EPHEMERAL camera-pose override for this ONE render only -- captured and restored automatically, never touches the document. Use to check the scene from a different angle.\","
						"\"properties\":{"
							"\"location\":{\"type\":\"string\",\"description\":\"Eye position, a string of EXACTLY 3 finite numbers \\\"x y z\\\" (space-separated, e.g. \\\"0 5 10\\\") -- required if camera is given. Malformed shapes (wrong token count, non-numeric component) are rejected with a clean error.\"},"
							"\"lookat\":{\"type\":\"string\",\"description\":\"Target point, a string of EXACTLY 3 finite numbers \\\"x y z\\\" -- required if camera is given. Same shape rule as location.\"},"
							"\"up\":{\"type\":\"string\",\"description\":\"Optional up vector, a string of EXACTLY 3 finite numbers \\\"x y z\\\"; defaults to the camera's current up. Same shape rule as location.\"},"
							"\"fov\":{\"type\":\"number\",\"description\":\"Optional field of view in degrees, EXCLUSIVE range (0, 180); defaults to the camera's current fov. 0, 180, negative, or non-finite values are rejected.\"}"
						"},\"required\":[\"location\",\"lookat\"]},"
						"\"quality\":{\"type\":\"string\",\"enum\":[\"draft\",\"production\"],\"description\":"
						"\"Optional, default \\\"production\\\" (today's exact behaviour). \\\"draft\\\" renders through a wholly SEPARATE, cheap studio-preview pipeline (same fixed shader the GUI's live interactive editor uses) that IGNORES the scene's authored materials and lighting -- geometry/composition/camera framing are representative, materials/lighting/exposure/colour are NOT. Samples are capped at 4 under draft. Check the result's `renderMode` field to see which pipeline ran; `integrator` does not change with `quality`.\"},"
						"\"mode\":{\"type\":\"string\",\"enum\":[\"beauty\",\"objectmap\",\"normals\",\"depth\",\"facets\",\"wireframe\",\"deep_reflect\",\"direct\",\"indirect\",\"clay_lights\"],\"description\":"
						"\"Optional, default \\\"beauty\\\". \\\"objectmap\\\" renders a flat per-object IDENTITY segmentation -- each scene object a distinct high-contrast colour, no lighting/materials -- and adds a `legend` array of {name,colorHex,pixelCount} to the result. Use it to reason about which object is where and how much of the frame each covers. Read the objectmap image at NATIVE size (do NOT pass read_image maxEdge -- downscaling box-blends the identity colours and corrupts colorHex matching). quality/samples are ignored under objectmap (one fidelity). Orthogonal to quality (objectmap = geometry identity, draft = cheap shading); check renderMode==\\\"objectmap\\\" in the result. A generator-synthesized legend name (e.g. grid[0,1] from an instance_array) identifies the instance but is NOT a CST chunk -- to EDIT it, target the generator chunk (strip the [i,j] suffix, e.g. grid), not the instance name. \\\"normals\\\"/\\\"depth\\\"/\\\"facets\\\"/\\\"wireframe\\\" are single-pass false-colour DIAGNOSTIC modes, no legend: \\\"normals\\\" (which way do surfaces face -- world-space shading normal as RGB), \\\"depth\\\" (how far away is everything -- grayscale, near=bright, brightness normalized PER RENDER to the VISIBLE hit-distance range in that frame, auto-calibrated -- a wide shot and a close-up of the same scene use DIFFERENT brightness scales, self-calibrating within a single render call, falling back to the scene's bounding-box diagonal only for a degenerate/empty scene), \\\"facets\\\" (what does the actual tessellation look like -- headlamp-shaded GEOMETRIC normal, no smoothing), \\\"wireframe\\\" (where are the polygon edges -- triangle-mesh edges only; analytic primitives like sphere/box/SDF render as dim facet shading with no lines, which is correct, not a bug). quality/samples are ignored under all four too; check renderMode for the exact name that ran (e.g. \\\"normals\\\"). \\\"deep_reflect\\\"/\\\"direct\\\"/\\\"indirect\\\"/\\\"clay_lights\\\" are DIFFERENT: REAL production-class path-traced renders at a FIXED reduced resolution and FIXED higher sample count, not single-pass diagnostics -- \\\"deep_reflect\\\" (what do reflections and refractions resolve to -- quarter-res, 16 spp, 24 bounces; use it to check specular/glass/metal content), \\\"direct\\\" (what does direct lighting alone contribute -- half-res, 8 spp, 1 bounce; use it to check lighting placement independent of indirect bounce), \\\"indirect\\\" (beauty minus the direct/emission contribution at the camera-visible vertex, all indirect bounces intact -- half-res, 12 spp, 16 bounces; a directly-lit surface or directly-visible emitter/env background reads black, energy arriving after >=1 bounce reads normally; use it to see where bounced light actually lands), and \\\"clay_lights\\\" (full transport with every surface's reflectance replaced by a neutral mid-grey clay Lambertian, real lights/GI untouched -- half-res, 12 spp, 12 bounces; use it to check whether the LIGHTING is right independent of any surface's material). quality/samples are STILL ignored (the mode's config is fixed by the registry) -- check the result's `effectiveSamples` field for the actual spp used.\"},"
						"\"xray\":{\"type\":\"boolean\",\"description\":"
						"\"Optional, default TRUE. Only meaningful when mode is one of the false-colour diagnostics (normals/depth/facets/wireframe) -- resolves the ray THROUGH transmissive (glass-like) surfaces to the first OPAQUE hit, following a STRAIGHT LINE with no refraction bending (deliberately an x-ray, not an optics simulation), up to 16 surfaces skipped. On by default so depth/normals/facets/wireframe see through glass and other transparent geometry to what's underneath -- pass xray:false to inspect the transmissive surface itself instead. If no opaque surface is ever reached, the LAST transmissive surface is shown instead of a black hole. Silently ignored under mode beauty/objectmap or a production-transport mode (deep_reflect/direct/indirect/clay_lights -- skipping glass would defeat their purpose).\"},"
						"\"view\":{\"type\":\"string\",\"description\":"
						"\"Optional name of a saved viewport bookmark (a live in-app GUI session's Named Views) or, headless, a scene CAMERA name -- renders from that vantage for THIS call only, composing with every mode above. If both view and camera are supplied, view wins. PINHOLE-ONLY: the override carries pose+FOV and cannot re-type the active camera, so a view naming a thin-lens/fisheye/orthographic camera FAILS the render (ok:false) naming the unsupported type rather than silently using the active camera's optics. An unresolvable name likewise FAILS with the available-name list in message.\"},"
						"\"light\":{\"type\":\"string\",\"description\":"
						"\"Optional name of a light (or an emissive object) to render with as the ONLY active light -- every other light contributes exactly zero, an unbiased partition of the full lighting, not a dim/approximate preview of it. Valid with mode:beauty (default) and the four production-transport modes (deep_reflect/direct/indirect/clay_lights); silently ignored (honestly noted in message) under objectmap, the false-colour diagnostics (normals/depth/facets/wireframe), or quality:draft -- none of those evaluate scene lighting. An unresolvable name FAILS the render (ok:false) with the available-name list in message, same contract as an unresolvable view. Use it to check one light's contribution in isolation.\"},"
						"\"perception\":{\"type\":\"boolean\",\"description\":"
						"\"Optional, default TRUE for agent transports. On a production beauty render, capture albedo, world-space normal, and primary-camera-hit depth alongside beauty without changing beauty pixels. Set false to save perception-specific memory when you only need beauty; OIDN may still allocate its own denoising auxiliaries. Ignored for draft/objectmap/view modes, which are already diagnostics.\"}"
					"}}"
				},
				{
					"read_image",
					"Fetch the LAST successful render as a PNG image so you can SEE the "
					"scene. Call after propose_patch + render to visually verify your "
					"edit did what you intended. If nothing has been rendered yet this "
					"returns an empty png_base64 (byteLength 0) -- call render first. "
					"TOKEN ECONOMY: pass maxEdge ~192 for a modeling/placement check -- the "
					"image is downscaled (no re-render) before being sent to you, so a "
					"quick look costs far fewer tokens than the full-resolution image. "
					"Omit maxEdge only for the final, full-detail look once you're done. "
					"EXCEPTION -- objectmap: if the last render was mode:\"objectmap\", read at "
					"NATIVE size (omit maxEdge). Downscaling box-blends the flat identity "
					"colours and corrupts the exact-byte legend match; the ~192 economy "
					"pattern applies to beauty/draft renders only. Set representation:\"perception\" "
					"after a production beauty render to receive one 2x2 atlas ordered "
					"[beauty, albedo; world normal, log depth] plus guide-prefilter, depth, exact persistent-memory metadata, and a conservative session peak bound. "
					"If maxEdge is omitted, beauty remains native but perception is bounded to 1024.",
					"{\"type\":\"object\",\"properties\":{"
						"\"maxEdge\":{\"type\":\"number\",\"description\":"
						"\"Optional long-edge bound in pixels, clamped to [16,1024]. Downscales (box filter, aspect-preserving, never upscales) the cached image before sending -- no re-render. Use ~192 for cheap modeling checks. Omission keeps beauty native and bounds perception to 1024.\"},"
						"\"representation\":{\"type\":\"string\",\"enum\":[\"beauty\",\"perception\"],\"description\":"
						"\"Optional, default beauty. perception returns a conventional 2x2 beauty/albedo/world-normal/log-depth atlas from the same render, with guidePrefilter and no re-render.\"}"
					"}}"
				},
				{
					"query_object_at",
					"Identify WHICH single object is at one pixel (x,y) -- the cheap way to "
					"answer \"what is that\" / locate ONE object before acting on it (e.g. "
					"before a propose_patch that moves or recolors it), without paying for a "
					"full render mode:\"objectmap\" segmentation. Returns hit:false (a NORMAL "
					"result, not an error) when the pixel is empty background -- name is empty "
					"in that case. width/height/camera compose EXACTLY like render's own "
					"overrides (ephemeral, restored automatically, never touch the document) -- "
					"use camera to aim at a spot, then query a known pixel (e.g. the image "
					"center) to name what's there. An out-of-range x/y for the effective film "
					"dims is refused as an error, never a silent hit:false. Works even with no "
					"active production rasterizer -- it runs its own cheap identity render "
					"internally.",
					"{\"type\":\"object\",\"properties\":{"
						"\"x\":{\"type\":\"number\",\"description\":"
						"\"Required integer pixel X coordinate, in the EFFECTIVE film dims (the width/height override below when both are given, else the scene's authored dims).\"},"
						"\"y\":{\"type\":\"number\",\"description\":"
						"\"Required integer pixel Y coordinate, same effective-dims rule as x.\"},"
						"\"width\":{\"type\":\"number\",\"description\":"
						"\"Optional TRANSIENT film-width override in pixels, clamped to [16,512]. Must be paired with height.\"},"
						"\"height\":{\"type\":\"number\",\"description\":"
						"\"Optional TRANSIENT film-height override in pixels, clamped to [16,512]. Must be paired with width.\"},"
						"\"camera\":{\"type\":\"object\",\"description\":"
						"\"Optional EPHEMERAL camera-pose override for this ONE query -- captured and restored automatically, never touches the document.\","
						"\"properties\":{"
							"\"location\":{\"type\":\"string\",\"description\":\"Eye position, a string of EXACTLY 3 finite numbers \\\"x y z\\\" -- required if camera is given.\"},"
							"\"lookat\":{\"type\":\"string\",\"description\":\"Target point, a string of EXACTLY 3 finite numbers \\\"x y z\\\" -- required if camera is given.\"},"
							"\"up\":{\"type\":\"string\",\"description\":\"Optional up vector, a string of EXACTLY 3 finite numbers \\\"x y z\\\".\"},"
							"\"fov\":{\"type\":\"number\",\"description\":\"Optional field of view in degrees, EXCLUSIVE range (0, 180).\"}"
						"},\"required\":[\"location\",\"lookat\"]}"
					"},\"required\":[\"x\",\"y\"]}"
				},
				{
					"compare_to_reference",
					"Measure how closely the current scene's render matches a reference photo -- "
					"the SAME RMSE objective function an image-reconstruction grader uses, handed "
					"to you directly instead of leaving you to render-then-eyeball. Renders the "
					"live scene at the NAMED reference's exact pixel dimensions (no width/height "
					"override -- the comparison needs pixel-for-pixel alignment) and returns "
					"{rmse, channelDelta:{r,g,b}, grid, worstCell, width, height, reference, "
					"summary}. rmse = sqrt(mean((render-reference)/255)^2) over all pixels -- "
					"lower is better, 0 is a perfect match; treat this as the primary objective to "
					"minimize. channelDelta is the mean SIGNED per-channel difference (render minus "
					"reference, [-1,1]) -- positive means your render runs brighter than the "
					"reference on that channel (e.g. channelDelta.b > 0 means too blue). grid is a "
					"3x3 ROW-MAJOR array (index 0 = top-left ... index 8 = bottom-right) of "
					"{rmse,dr,dg,db} -- the SAME two measures broken down spatially, so you can "
					"find WHICH region is worst (background/environment staging is the most common "
					"weak spot) instead of guessing from one number; worstCell names that region "
					"(e.g. \"top-right\"). QUALITY TRADEOFF: omit `samples` (the default) and the "
					"comparison renders quality:\"draft\" -- cheap, but IGNORES materials/lighting "
					"entirely, so a low draft-mode RMSE only confirms geometry/composition/camera "
					"alignment, NOT colour/material match. Pass `samples` (e.g. 16-64) to switch to "
					"quality:\"production\" for the real, grader-equivalent reading -- recommended "
					"workflow: iterate cheaply under the draft default while getting composition "
					"right, then request `samples` once composition looks plausible. `visual` "
					"(default true) also returns a [render | reference | abs-diff heatmap] "
					"composite image; set it false once you only need the numbers. `split` "
					"(default false) additionally returns split:{objectRmse,backgroundRmse,"
					"objectPixelFraction,ok,note} -- an object-vs-background RMSE breakdown from "
					"an EXTRA ephemeral mode:\"objectmap\" render of your own candidate. Reach for "
					"it once your overall rmse plateaus: a high backgroundRmse means staging "
					"(ground/environment/lighting) is still the biggest lever; a low backgroundRmse "
					"with a high objectRmse means staging is DONE -- stop tuning it and spend "
					"remaining iterations on the object's silhouette/proportions instead. Honesty "
					"caveat: the object mask comes from YOUR candidate only (the reference PNG has "
					"no objectmap of its own), so it answers \"on the pixels where my object is, how "
					"wrong am I\" and \"on my background pixels, how wrong am I\" -- a badly "
					"misplaced object still shows up (high objectRmse on the candidate's object "
					"pixels, AND the reference's real object pixels raise backgroundRmse since your "
					"candidate has no object there). Both figures sentinel to -1 when their bucket is "
					"EMPTY: objectRmse is -1 when no object pixels are visible (camera pointed "
					"away, object off-frame), backgroundRmse is -1 when registered objects cover "
					"the ENTIRE frame. Check for >= 0 before trusting either -- -1 means \"not "
					"measured\", NOT \"perfect match\". WARNING: without `splitObjects`, EVERY "
					"registered object counts as OBJECT -- including a ground plane, backdrop, or "
					"any other staging geometry you built as a real scene object -- so an unscoped "
					"split measures \"geometry vs. environment\", not \"hero object vs. staging\" "
					"(observed averaging 86% of the frame in the OBJECT bucket on scenes with a "
					"modeled ground plane). Pass `splitObjects` (an array of object names) to scope "
					"the OBJECT bucket to just your hero object -- every other pixel, including "
					"other registered geometry, then falls into BACKGROUND instead, giving a true "
					"hero-object-vs-staging reading. A requested name absent from the candidate's "
					"objectmap legend is dropped from the mask (not a hard failure) and surfaced in "
					"split.note along with the names that ARE available, so a typo can't silently "
					"shrink your mask unnoticed; if NONE of the requested names match, objectRmse "
					"comes back -1 with a note saying so explicitly (distinct from the ordinary "
					"\"object off-frame\" -1 case).",
					"{\"type\":\"object\",\"properties\":{"
						"\"reference\":{\"type\":\"string\",\"description\":"
						"\"Required. The name of a host-registered reference image (e.g. view1, view2, ... in prompt-attachment order). An unknown name is an error listing every registered reference.\"},"
						"\"camera\":{\"type\":\"object\",\"description\":"
						"\"Optional EPHEMERAL camera-pose override for this ONE comparison render -- captured and restored automatically, never touches the document.\","
						"\"properties\":{"
							"\"location\":{\"type\":\"string\",\"description\":\"Eye position, a string of EXACTLY 3 finite numbers \\\"x y z\\\" -- required if camera is given.\"},"
							"\"lookat\":{\"type\":\"string\",\"description\":\"Target point, a string of EXACTLY 3 finite numbers \\\"x y z\\\" -- required if camera is given.\"},"
							"\"up\":{\"type\":\"string\",\"description\":\"Optional up vector, a string of EXACTLY 3 finite numbers \\\"x y z\\\".\"},"
							"\"fov\":{\"type\":\"number\",\"description\":\"Optional field of view in degrees, EXCLUSIVE range (0, 180).\"}"
						"},\"required\":[\"location\",\"lookat\"]},"
						"\"visual\":{\"type\":\"boolean\",\"description\":"
						"\"Optional, default true. When true, also returns a composite [render|reference|heatmap] diff image. Set false to save tokens once you only need the numeric feedback.\"},"
						"\"samples\":{\"type\":\"number\",\"description\":"
						"\"Optional sample-count override, clamped to [1,65536]. Omit for a cheap quality:draft comparison (materials/lighting ignored); supply for a real quality:production RMSE reading -- see the tool description's quality tradeoff.\"},"
						"\"split\":{\"type\":\"boolean\",\"description\":"
						"\"Optional, default false. Returns an object-vs-background RMSE breakdown (one extra objectmap render) -- see the tool description's split paragraph.\"},"
						"\"splitObjects\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":"
						"\"Optional array of object names, only meaningful alongside split:true. Scopes the OBJECT bucket to ONLY the named registered object(s) -- every other pixel, including other registered geometry like a ground plane or backdrop, falls into BACKGROUND instead. Without this, a modeled ground plane/backdrop counts as OBJECT too, inflating the OBJECT bucket -- see the tool description's WARNING. A name not found in the candidate's objectmap legend is dropped from the mask and surfaced in split.note (never a hard failure).\"}"
					"},\"required\":[\"reference\"]}"
				},
				{
					"ask_user",
					"Pause and ask the user a single, SPECIFIC clarifying question when a "
					"real ambiguity would MATERIALLY change the scene you build -- subject "
					"identity, style/mood, or a key composition choice. Do NOT ask about "
					"details you can decide yourself with reasonable taste (exact colours, "
					"minor placement, secondary props) -- pick something sensible and note "
					"the choice in your final summary instead. If the brief is already "
					"clear, do NOT call this tool at all; asking when the answer is obvious "
					"wastes the user's time. Budget: at most 2-3 calls per task, and prefer "
					"ONE call carrying your single most important question over several "
					"small ones. Prefer `options` (2-5 short choices the user can click) "
					"over a fully open-ended question -- options are faster for the user to "
					"answer and easier for you to act on. Returns either {\"answer\": "
					"\"...\"} -- the text of the option the user picked (verbatim) or their "
					"typed freeform reply -- or, when no interactive user is available to "
					"answer (e.g. a headless eval run), {\"available\": false, \"note\": "
					"\"...\"}. On available:false, do NOT re-ask -- make your best judgment "
					"from the brief as given and proceed.",
					"{\"type\":\"object\",\"properties\":{"
						"\"question\":{\"type\":\"string\",\"description\":"
						"\"Required. ONE specific question, phrased so a short answer resolves it (e.g. \\\"Should the mood be warm sunset or cool overcast?\\\").\"},"
						"\"options\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"minItems\":2,\"maxItems\":5,\"description\":"
						"\"Optional, 2-5 short strings the user can pick from with one click. Preferred over leaving the question fully open-ended -- offer options whenever the plausible answers are enumerable.\"},"
						"\"allowFreeform\":{\"type\":\"boolean\",\"description\":"
						"\"Optional, default true. Whether a typed answer (instead of clicking one of options) is acceptable. Set false only when options is exhaustive and a freeform reply would not make sense.\"}"
					"},\"required\":[\"question\"]}"
				},
			};

			const std::size_t kToolDefCount = sizeof( kToolDefs ) / sizeof( kToolDefs[0] );

			//! Anthropic-native tools array: [{name,description,input_schema},...].
			const std::string& AnthropicToolsJson()
			{
				static const std::string json = []() {
					std::string out = "[";
					for( std::size_t i = 0; i < kToolDefCount; ++i ) {
						if( i ) out += ",";
						out += "{\"name\":";
						JsonAppendEscapedString( out, kToolDefs[i].name );
						out += ",\"description\":";
						JsonAppendEscapedString( out, kToolDefs[i].description );
						out += ",\"input_schema\":";
						out += kToolDefs[i].schemaJson ? kToolDefs[i].schemaJson
						                               : "{\"type\":\"object\",\"properties\":{}}";
						out += "}";
					}
					out += "]";
					return out;
				}();
				return json;
			}

			//! Gemini-native function declarations: [{name,description[,parameters]},...].
			const std::string& GeminiFunctionDeclarationsJson()
			{
				static const std::string json = []() {
					std::string out = "[";
					for( std::size_t i = 0; i < kToolDefCount; ++i ) {
						if( i ) out += ",";
						out += "{\"name\":";
						JsonAppendEscapedString( out, kToolDefs[i].name );
						out += ",\"description\":";
						JsonAppendEscapedString( out, kToolDefs[i].description );
						if( kToolDefs[i].schemaJson ) {
							out += ",\"parameters\":";
							out += kToolDefs[i].schemaJson;
						}
						out += "}";
					}
					out += "]";
					return out;
				}();
				return json;
			}

			//! OpenAI-native tools array: [{type:"function",function:{...}},...].
			const std::string& OpenAIToolsJson()
			{
				static const std::string json = []() {
					std::string out = "[";
					for( std::size_t i = 0; i < kToolDefCount; ++i ) {
						if( i ) out += ",";
						out += "{\"type\":\"function\",\"function\":{\"name\":";
						JsonAppendEscapedString( out, kToolDefs[i].name );
						out += ",\"description\":";
						JsonAppendEscapedString( out, kToolDefs[i].description );
						out += ",\"parameters\":";
						out += kToolDefs[i].schemaJson ? kToolDefs[i].schemaJson
						                               : "{\"type\":\"object\",\"properties\":{}}";
						out += "}}";
					}
					out += "]";
					return out;
				}();
				return json;
			}

			//! Responses-native tools put name/description/parameters directly
			//! on the tool object (there is no Chat-Completions `function`
			//! wrapper).
			const std::string& OpenAIResponsesToolsJson()
			{
				static const std::string json = []() {
					std::string out = "[";
					for( std::size_t i = 0; i < kToolDefCount; ++i ) {
						if( i ) out += ",";
						out += "{\"type\":\"function\",\"name\":";
						JsonAppendEscapedString( out, kToolDefs[i].name );
						out += ",\"description\":";
						JsonAppendEscapedString( out, kToolDefs[i].description );
						out += ",\"parameters\":";
						out += kToolDefs[i].schemaJson ? kToolDefs[i].schemaJson
						                               : "{\"type\":\"object\",\"properties\":{}}";
						out += "}";
					}
					out += "]";
					return out;
				}();
				return json;
			}

			//------------------------------------------------------------------
			// (2) Raw-span JSON scanner.
			//
			// The assistant content must be echoed back BYTE-PRESERVED on
			// later requests (thinking-block signatures are opaque and
			// must round-trip unmodified), so we extract it as a raw byte
			// span of the response body rather than parse + re-serialize.
			// The keys we navigate by ("content", "candidates") never
			// contain escapes, so a raw key comparison is exact.
			//------------------------------------------------------------------

			const std::size_t kNpos = std::string::npos;

			std::size_t SkipWs( const std::string& s, std::size_t i )
			{
				while( i < s.size() &&
				       ( s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' ) ) ++i;
				return i;
			}

			//! `i` at the opening quote; returns one past the closing quote.
			std::size_t ScanRawString( const std::string& s, std::size_t i )
			{
				++i;
				while( i < s.size() ) {
					const char c = s[i];
					if( c == '\\' ) { i += 2; continue; }
					if( c == '"' ) return i + 1;
					++i;
				}
				return kNpos;
			}

			//! `i` at (or before, in whitespace) the first char of a JSON
			//! value; returns one past its end.  Balanced-bracket walk that
			//! skips strings; kNpos on malformation.
			std::size_t ScanRawValue( const std::string& s, std::size_t i )
			{
				i = SkipWs( s, i );
				if( i >= s.size() ) return kNpos;
				const char c = s[i];
				if( c == '"' ) return ScanRawString( s, i );
				if( c == '{' || c == '[' ) {
					int depth = 0;
					while( i < s.size() ) {
						const char d = s[i];
						if( d == '"' ) {
							i = ScanRawString( s, i );
							if( i == kNpos ) return kNpos;
							continue;
						}
						if( d == '{' || d == '[' ) ++depth;
						else if( d == '}' || d == ']' ) {
							--depth;
							if( depth == 0 ) return i + 1;
						}
						++i;
					}
					return kNpos;
				}
				// number / true / false / null: scan to a structural delimiter
				std::size_t j = i;
				while( j < s.size() && s[j] != ',' && s[j] != '}' && s[j] != ']' &&
				       s[j] != ' ' && s[j] != '\t' && s[j] != '\n' && s[j] != '\r' ) ++j;
				return ( j > i ) ? j : kNpos;
			}

			//! Find member `key` of the object starting at `objBegin`
			//! (whitespace allowed before the '{') and return the raw span
			//! of its value.  LAST match wins, deliberately matching
			//! JsonValue::find's last-set-wins rule -- so on a hostile
			//! body that repeats a key, the parsed view we act on and the
			//! raw span we echo back are the SAME value.
			bool RawObjectMember( const std::string& s, std::size_t objBegin, const char* key,
			                      std::size_t& valBegin, std::size_t& valEnd )
			{
				std::size_t i = SkipWs( s, objBegin );
				if( i >= s.size() || s[i] != '{' ) return false;
				++i;
				const std::size_t keyLen = std::strlen( key );
				bool found = false;
				while( true ) {
					i = SkipWs( s, i );
					if( i >= s.size() ) return false;
					if( s[i] == '}' ) return found;
					if( s[i] != '"' ) return false;
					const std::size_t keyBegin = i + 1;
					const std::size_t afterKey = ScanRawString( s, i );
					if( afterKey == kNpos ) return false;
					const std::size_t rawKeyLen = ( afterKey - 1 ) - keyBegin;
					const bool match = ( rawKeyLen == keyLen ) &&
					                   ( s.compare( keyBegin, keyLen, key ) == 0 );
					i = SkipWs( s, afterKey );
					if( i >= s.size() || s[i] != ':' ) return false;
					++i;
					i = SkipWs( s, i );
					const std::size_t vEnd = ScanRawValue( s, i );
					if( vEnd == kNpos ) return false;
					if( match ) { valBegin = i; valEnd = vEnd; found = true; }
					i = SkipWs( s, vEnd );
					if( i < s.size() && s[i] == ',' ) { ++i; continue; }
					if( i < s.size() && s[i] == '}' ) return found;
					return false;
				}
			}

			//! Raw span of element `index` of the array starting at `arrBegin`.
			bool RawArrayElement( const std::string& s, std::size_t arrBegin, std::size_t index,
			                      std::size_t& valBegin, std::size_t& valEnd )
			{
				std::size_t i = SkipWs( s, arrBegin );
				if( i >= s.size() || s[i] != '[' ) return false;
				++i;
				std::size_t n = 0;
				while( true ) {
					i = SkipWs( s, i );
					if( i >= s.size() || s[i] == ']' ) return false;
					const std::size_t vEnd = ScanRawValue( s, i );
					if( vEnd == kNpos ) return false;
					if( n == index ) { valBegin = i; valEnd = vEnd; return true; }
					++n;
					i = SkipWs( s, vEnd );
					if( i < s.size() && s[i] == ',' ) { ++i; continue; }
					return false;
				}
			}

			//------------------------------------------------------------------
			// Shared response/result helpers.
			//------------------------------------------------------------------

			//! Output-token budget for Anthropic requests.  Adaptive
			//! thinking on claude-sonnet-5 (the default model) counts
			//! against max_tokens, and tool results routinely carry whole
			//! scene documents into context -- 8192 was realistically
			//! exhaustible mid-reply.  16000 keeps the cap a safety net
			//! rather than a limit the loop actually hits.
			const int kAnthropicMaxTokens = 16000;

			//! Output-token budget for OpenAI Chat Completions.  Mirrors
			//! Anthropic's headroom: tool results can carry whole scene docs,
			//! and gpt-5.5 reasoning/output may otherwise hit too small a cap.
			const int kOpenAIMaxCompletionTokens = 16000;

			//! Strip every control character (< 0x20 -- CR, LF, TAB, ...)
			//! from a caller-supplied string before splicing it into an
			//! HTTP header VALUE, so a pasted API key carrying a stray
			//! newline cannot smuggle extra headers into the request
			//! (header-injection defense; a stripped key simply fails
			//! auth, which is the honest outcome).
			std::string SanitizeHeaderValue( const std::string& v )
			{
				std::string out;
				out.reserve( v.size() );
				for( std::size_t i = 0; i < v.size(); ++i ) {
					const unsigned char c = static_cast<unsigned char>( v[i] );
					if( c >= 0x20 ) out += v[i];
				}
				return out;
			}

			//! Percent-escape any modelId character outside [A-Za-z0-9._-]
			//! before splicing it into a URL path segment, so a hostile or
			//! mistyped model id cannot alter the request path or smuggle
			//! query parameters.
			std::string SanitizeModelIdForUrl( const std::string& modelId )
			{
				static const char* const kHex = "0123456789ABCDEF";
				std::string out;
				out.reserve( modelId.size() );
				for( std::size_t i = 0; i < modelId.size(); ++i ) {
					const char c = modelId[i];
					const bool ok = ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) ||
					                ( c >= '0' && c <= '9' ) ||
					                c == '.' || c == '_' || c == '-';
					if( ok ) {
						out += c;
					}
					else {
						const unsigned char u = static_cast<unsigned char>( c );
						out += '%';
						out += kHex[( u >> 4 ) & 0xF];
						out += kHex[u & 0xF];
					}
				}
				return out;
			}

			ChatStepResult MakeProviderError( ChatErrorKind kind, const std::string& message )
			{
				ChatStepResult r;
				r.kind = ChatStepResult::Kind::ProviderError;
				r.errorKind = kind;
				r.errorMessage = message;
				return r;
			}

			//! A ProviderError for a non-200 HTTP status, carrying the
			//! provider's error.message when the body parses (providers
			//! use the {"error":{"message":...}} shape).  The raw body is
			//! deliberately NOT included (it may be large / arbitrary).
			ChatStepResult MakeHttpError( const char* provider, long status, const std::string& body )
			{
				std::string msg;
				JsonValue root;
				std::string perr;
				if( JsonParse( body, root, perr ) && root.isObject() ) {
					const JsonValue& m = root.get( "error" ).get( "message" );
					if( m.isString() ) msg = m.asString();
				}
				std::string full = std::string( provider ) + " HTTP " + std::to_string( status );
				if( !msg.empty() ) full += ": " + msg;
				else full += " (no parseable error message)";
				return MakeProviderError( ChatErrorKind::Http, full );
			}

			//! The text that replaces an elided (superseded) image block or
			//! part -- see AgentChatLoop.h "IMAGE RETENTION".
			const char* const kImageElidedNote =
				"[image elided -- superseded by a newer render]";

			//! The text that replaces an elided user reference-image
			//! attachment once the live-image cap is exceeded -- see
			//! AgentChatLoop.h "USER IMAGE RETENTION".  Deliberately
			//! distinct wording from kImageElidedNote (a different policy:
			//! capped-oldest-first, not most-recent-only) and actionable
			//! ("re-attach if needed") since, unlike a read_image result
			//! the loop can always regenerate by rendering again, a user
			//! photo is gone for good unless the user attaches it again.
			const char* const kUserImageElidedNote =
				"[reference image elided -- re-attach if needed]";

			JsonValue MakeTextBlock( const std::string& text )
			{
				JsonValue b = JsonValue::MakeObject();
				b.set( "type", JsonValue::MakeString( "text" ) );
				b.set( "text", JsonValue::MakeString( text ) );
				return b;
			}

			//! For a read_image result: `result` minus the png_base64 field,
			//! plus a note that the image travels as a real image block/part.
			//! Stripping the base64 from the textual half keeps it from being
			//! double-sent.
			JsonValue StripPngBase64( const JsonValue& result, const char* note )
			{
				JsonValue summary = JsonValue::MakeObject();
				const std::vector<std::pair<std::string, JsonValue>>& members = result.members();
				for( std::size_t i = 0; i < members.size(); ++i ) {
					if( members[i].first == "png_base64" ) continue;
					summary.set( members[i].first, members[i].second );
				}
				// Check-then-set: JsonValue::set APPENDS members, so blindly
				// setting "note" would serialize a DUPLICATE key if the RPC
				// result ever grows a note field of its own.
				summary.set( summary.has( "note" ) ? "image_note" : "note",
				             JsonValue::MakeString( note ) );
				return summary;
			}

			//! True + the base64 payload iff this call is a read_image (or a
			//! compare_to_reference called with visual:true -- see
			//! AgentRpc.cpp's compare_to_reference dispatch doc: it
			//! deliberately reuses the SAME "png_base64" field name so this
			//! one predicate, and every retention/elision policy built on
			//! it, covers both without a second code path) whose JSON-RPC
			//! result carries a non-empty png_base64 string.
			bool IsImageResult( const ChatToolCall& call, const JsonValue& result, std::string& outB64 )
			{
				if( ( call.name != "read_image" && call.name != "compare_to_reference" ) || !result.isObject() ) return false;
				const JsonValue* b64 = result.find( "png_base64" );
				if( !b64 || !b64->isString() || b64->asString().empty() ) return false;
				outB64 = b64->asString();
				return true;
			}

			//! Rewrite the attach note inside a StripPngBase64-produced
			//! textual summary so an elided image is not described as
			//! "attached" (contradictory context for the model).  The
			//! summary is LOOP-OWNED JSON (StripPngBase64 wrote it), so
			//! parse + rewrite is legal.  Prefers "image_note" -- the key
			//! StripPngBase64 falls back to when the RPC result already
			//! owned a "note" of its own -- so an RPC-owned note field is
			//! never clobbered.  Text that does not parse as an object, or
			//! carries neither key, is returned unchanged.
			std::string RewriteElidedSummaryText( const std::string& text )
			{
				JsonValue obj;
				std::string perr;
				if( !JsonParse( text, obj, perr ) || !obj.isObject() ) return text;
				// INVARIANT: this prefer-image_note-else-note order matches
				// StripPngBase64's collision fallback (which writes the attach
				// note under "image_note" ONLY when the RPC result already
				// owns a "note").  If an RPC result ever grows an
				// "image_note" field of its own WITHOUT a "note", the two
				// rules diverge (this rewrite would clobber the RPC's field)
				// -- keep the key-choice rules in lockstep.
				const char* key = obj.has( "image_note" ) ? "image_note"
				                : obj.has( "note" )       ? "note"
				                : nullptr;
				if( !key ) return text;
				JsonValue out = JsonValue::MakeObject();
				const std::vector<std::pair<std::string, JsonValue>>& mem = obj.members();
				for( std::size_t i = 0; i < mem.size(); ++i ) {
					out.set( mem[i].first, mem[i].first == key
					         ? JsonValue::MakeString( kImageElidedNote )
					         : mem[i].second );
				}
				return JsonSerialize( out );
			}

			JsonValue MakeOpenAIImageUrlBlock( const std::string& mimeType,
			                                   const std::string& b64 )
			{
				JsonValue imageUrl = JsonValue::MakeObject();
				imageUrl.set( "url", JsonValue::MakeString(
					"data:" + mimeType + ";base64," + b64 ) );
				JsonValue block = JsonValue::MakeObject();
				block.set( "type", JsonValue::MakeString( "image_url" ) );
				block.set( "image_url", imageUrl );
				return block;
			}

			JsonValue MakeOpenAITextContentBlock( const std::string& text )
			{
				JsonValue block = JsonValue::MakeObject();
				block.set( "type", JsonValue::MakeString( "text" ) );
				block.set( "text", JsonValue::MakeString( text ) );
				return block;
			}

			JsonValue MakeOpenAIResponsesImageBlock( const std::string& mimeType,
			                                         const std::string& b64 )
			{
				JsonValue block = JsonValue::MakeObject();
				block.set( "type", JsonValue::MakeString( "input_image" ) );
				block.set( "image_url", JsonValue::MakeString(
					"data:" + mimeType + ";base64," + b64 ) );
				return block;
			}

			JsonValue MakeOpenAIResponsesTextBlock( const std::string& text )
			{
				JsonValue block = JsonValue::MakeObject();
				block.set( "type", JsonValue::MakeString( "input_text" ) );
				block.set( "text", JsonValue::MakeString( text ) );
				return block;
			}

			//! True when `text` is empty or contains only ASCII whitespace --
			//! nothing a user could read.  Mirrors AgentChatLoop's user-side
			//! IsBlank (kept local: the codec is a separate TU).
			bool ChatContentIsBlank( const std::string& text )
			{
				for( std::size_t i = 0; i < text.size(); ++i ) {
					const char c = text[i];
					if( c != ' ' && c != '\t' && c != '\n' && c != '\r' ) return false;
				}
				return true;
			}

			std::string JsonObjectContentToText( const JsonValue& content )
			{
				if( content.isString() ) return content.asString();
				if( !content.isArray() ) return std::string();
				std::string out;
				for( std::size_t i = 0; i < content.size(); ++i ) {
					const JsonValue& b = content.at( i );
					if( b.get( "type" ).asString() == "text" )
						out += b.get( "text" ).asString();
				}
				return out;
			}
		}

		bool ChatToolResultCarriesImage( const ChatToolCall& call,
		                                 const std::string& rawJsonRpcResponseLine )
		{
			// Same predicate the pack paths apply: a parseable JSON-RPC
			// SUCCESS envelope (an error envelope never packs an image)
			// whose result is a read_image payload with non-empty
			// png_base64.
			JsonValue env;
			std::string perr;
			if( !JsonParse( rawJsonRpcResponseLine, env, perr ) || !env.isObject() ) return false;
			if( env.find( "error" ) ) return false;
			std::string b64;
			return IsImageResult( call, env.get( "result" ), b64 );
		}

		int ChatUserEntryLiveImageCount( const std::string& userEntryJson )
		{
			JsonValue root;
			std::string perr;
			if( !JsonParse( userEntryJson, root, perr ) || !root.isObject() ) return 0;

			int count = 0;
			// Anthropic shape: {"role":"user","content":[{"type":"image",...},...]}
			const JsonValue& content = root.get( "content" );
			if( content.isArray() ) {
				for( std::size_t i = 0; i < content.size(); ++i )
					if( content.at( i ).get( "type" ).asString() == "image" ) ++count;
			}
			// Gemini shape: {"role":"user","parts":[{"inlineData":{...}},...]}
			const JsonValue& parts = root.get( "parts" );
			if( parts.isArray() ) {
				for( std::size_t i = 0; i < parts.size(); ++i )
					if( parts.at( i ).get( "inlineData" ).isObject() ) ++count;
			}
			// OpenAI Chat/Responses shapes use image_url/input_image respectively.
			if( root.get( "role" ).asString() == "user" && content.isArray() ) {
				for( std::size_t i = 0; i < content.size(); ++i ) {
					const std::string type = content.at( i ).get( "type" ).asString();
					if( type == "image_url" || type == "input_image" ) ++count;
				}
			}
			return count;
		}

		//======================================================================
		// Shared token-usage normalization.
		//
		// ChatUsage's contract (AgentChatCodecs.h) is ESTABLISHED here and
		// nowhere else: every provider's ParseUsage reads its counts through
		// ReadTokenCount, folds (if that provider's counter is a separate
		// summand) through FoldReasoningIntoOutput, and ends in
		// EnforceUsageInvariant.  Keeping the three steps in one place is what
		// makes "0 <= reasoningOutputTokens <= outputTokens" a property of the
		// TYPE rather than a claim each branch has to re-honour by hand.
		//======================================================================
		namespace
		{
			//! Ceiling on any single parsed token count.  1e12 is ~5 orders of
			//! magnitude above the largest count any real model can emit, is
			//! exactly representable as a double, and leaves >7 orders of
			//! headroom under LLONG_MAX so a fold can never overflow.
			//!
			//! The cap is not cosmetic.  `static_cast<long long>(d)` for a `d`
			//! outside long long's range is UNDEFINED BEHAVIOUR (and a UBSan
			//! trap), and a JSON number is an arbitrary-magnitude double: a
			//! body carrying `"reasoning_tokens":1e19` reaches this code from
			//! any provider.  So the RANGE CHECK happens in double arithmetic,
			//! BEFORE the cast.
			const long long kMaxTokenCount = 1000000000000LL;   // 1e12

			//! Reads `container[key]` as a token count, normalizing every
			//! shape a provider (or a gateway in front of one) can emit:
			//!   absent / null / non-number  -> -1  (the ABSENT sentinel)
			//!   negative                    ->  0  (garbage, never a count,
			//!                                       and must never SUBTRACT)
			//!   NaN                         ->  0  (fails every comparison)
			//!   >= kMaxTokenCount           ->  kMaxTokenCount (saturate)
			//!   otherwise                   ->  the truncated integer
			//! `container` need not be an object: JsonValue::get on a non-object
			//! yields null, which reads as absent.
			long long ReadTokenCount( const JsonValue& container, const char* key )
			{
				const JsonValue& v = container.get( key );
				if( !v.isNumber() ) return -1;
				const double d = v.asNumber();
				if( d >= static_cast<double>( kMaxTokenCount ) ) return kMaxTokenCount;
				if( d > 0.0 ) return static_cast<long long>( d );
				return 0;   // <= 0 and NaN both land here
			}

			//! Folds a SEPARATE-SUMMAND reasoning count into the billed total.
			//! Both operands are <= kMaxTokenCount by construction, so the sum
			//! cannot overflow.  A ZERO visible-output count still folds (a
			//! turn can bill thinking and emit nothing visible).
			//!
			//! The ABSENT case (outputTokens < 0) is deliberately NOT handled
			//! here: it is not fold-specific -- an INCLUSIVE provider reporting
			//! reasoning with no output counter needs the identical answer --
			//! so it lives once, in EnforceUsageInvariant, which every
			//! ParseUsage runs.  Handling it in both places would leave one of
			//! the two unreachable by any observable behaviour.
			void FoldReasoningIntoOutput( ChatUsage& u )
			{
				if( u.reasoningOutputTokens <= 0 || u.outputTokens < 0 ) return;
				u.outputTokens += u.reasoningOutputTokens;
			}

			//! The LAST statement of every ParseUsage, and the only place the
			//! invariant is established.  It also PRESERVES the provider's own
			//! reasoning number in `reasoningOutputTokensReported` before any
			//! clamp can overwrite it -- the clamp is a normalization, not a
			//! correction, and the evidence that it fired (what the body
			//! actually claimed) is exactly what a later audit needs.
			//!
			//! Two cases:
			//!   * output ABSENT (-1) while a reasoning count was REPORTED (>=
			//!     0 -- i.e. anything but the -1 sentinel).  Reasoning is a
			//!     SUBSET of the billed generation, so the reported count is
			//!     itself a lower bound on that generation -- publish it
			//!     rather than leaving an "absent" total next to a known
			//!     subset.  The bound is taken at >= 0 rather than > 0 so the
			//!     header's contract ("reasoningOutputTokens >= 0 implies
			//!     outputTokens >= 0") holds LITERALLY, with no zero-shaped
			//!     hole: `{"output_tokens_details":{"thinking_tokens":0}}` with
			//!     no output counter used to publish output -1 beside
			//!     reasoning 0, so `output - reasoning` read -1 -- a negative
			//!     "visible output" from a body that contained no
			//!     contradiction at all.  A published 0 is the same claim the
			//!     positive case makes (the subset is a lower bound), and the
			//!     recorder's cross-turn sums are untouched either way (they
			//!     add only counts > 0).  This makes every provider behave
			//!     identically in this shape (a separate-summand provider's
			//!     fold already lands exactly here) and keeps the invariant
			//!     unconditional: a run's reasoning total can never exceed its
			//!     output total.
			//!   * reasoning > output.  The body contradicts itself.  The
			//!     BILLED TOTAL is the number a cost model must not lose, so
			//!     the subset is clamped down to it (never the total raised to
			//!     meet a suspect subset) and the anomaly is flagged for the
			//!     trajectory record.
			void EnforceUsageInvariant( ChatUsage& u )
			{
				// Captured BEFORE either branch can move a number: post-fold,
				// post-normalization, pre-clamp.  On a healthy body this equals
				// reasoningOutputTokens; on a clamped one it is the only
				// surviving copy of what the provider actually said.
				u.reasoningOutputTokensReported = u.reasoningOutputTokens;
				if( u.outputTokens < 0 ) {
					if( u.reasoningOutputTokens >= 0 ) u.outputTokens = u.reasoningOutputTokens;
					return;
				}
				if( u.reasoningOutputTokens > u.outputTokens ) {
					u.reasoningOutputTokens = u.outputTokens;
					u.reasoningClamped = true;
				}
			}
		}

		//======================================================================
		// (3) AnthropicChatCodec
		//======================================================================

		const char* AnthropicChatCodec::ProviderName() const { return "anthropic"; }

		const char* AnthropicChatCodec::DefaultModelId() const { return "claude-sonnet-5"; }

		std::string AnthropicChatCodec::MakeUserEntry(
			const std::string& text, const std::vector<ChatAttachment>& attachments ) const
		{
			JsonValue msg = JsonValue::MakeObject();
			msg.set( "role", JsonValue::MakeString( "user" ) );
			JsonValue content = JsonValue::MakeArray();
			// Images FIRST, then the caption -- see the header note on
			// MakeUserEntry (an empty attachments vector reproduces the
			// pre-attachment byte shape exactly: one text block, nothing
			// else, since the loop below then simply doesn't run).
			for( std::size_t i = 0; i < attachments.size(); ++i ) {
				JsonValue source = JsonValue::MakeObject();
				source.set( "type", JsonValue::MakeString( "base64" ) );
				source.set( "media_type", JsonValue::MakeString( attachments[i].mimeType ) );
				source.set( "data", JsonValue::MakeString( attachments[i].base64Data ) );
				JsonValue img = JsonValue::MakeObject();
				img.set( "type", JsonValue::MakeString( "image" ) );
				img.set( "source", source );
				content.push_back( img );
			}
			// Anthropic hard-400s an EMPTY text block -- an attachment-
			// only message (no caption) must therefore omit the text
			// block entirely rather than send {"type":"text","text":""}.
			// A text-only call (the common case) is unaffected: text is
			// non-empty by the loop's blank-message no-op contract.
			if( !text.empty() ) content.push_back( MakeTextBlock( text ) );
			msg.set( "content", content );
			return JsonSerialize( msg );
		}

		std::string AnthropicChatCodec::RewriteElidedUserImages(
			const std::string& userEntryJson, int countToElide ) const
		{
			// Parse + regenerate is LEGAL here: this entry was produced by
			// MakeUserEntry above (loop-generated), never by a provider --
			// only assistant entries carry the byte-preservation contract.
			if( countToElide <= 0 ) return userEntryJson;
			JsonValue root;
			std::string perr;
			if( !JsonParse( userEntryJson, root, perr ) || !root.isObject() ) return userEntryJson;
			const JsonValue& content = root.get( "content" );
			if( !content.isArray() ) return userEntryJson;

			bool changed = false;
			int remaining = countToElide;
			JsonValue newContent = JsonValue::MakeArray();
			for( std::size_t i = 0; i < content.size(); ++i ) {
				const JsonValue& b = content.at( i );
				if( b.get( "type" ).asString() == "image" && remaining > 0 ) {
					newContent.push_back( MakeTextBlock( kUserImageElidedNote ) );
					--remaining;
					changed = true;
				}
				else {
					newContent.push_back( b );
				}
			}
			if( !changed ) return userEntryJson;

			JsonValue newRoot = JsonValue::MakeObject();
			const std::vector<std::pair<std::string, JsonValue>>& mem = root.members();
			for( std::size_t i = 0; i < mem.size(); ++i )
				newRoot.set( mem[i].first, mem[i].first == "content" ? newContent : mem[i].second );
			return JsonSerialize( newRoot );
		}

		std::string AnthropicChatCodec::PackToolResults(
			const std::vector<std::pair<ChatToolCall, std::string>>& results ) const
		{
			// ONE user message carrying one tool_result block per tool_use --
			// Anthropic requires every tool_use of the assistant turn to be
			// answered in the SAME following user message.
			//
			// IMAGE RETENTION, entry-internal half: within one flush only
			// the LAST image-bearing result keeps a live image block --
			// earlier ones are packed PRE-ELIDED (summary note + elision
			// text, exactly what RewriteElidedImages would later produce)
			// so the "one live image globally" rule holds even when one
			// assistant turn requested read_image twice.
			std::size_t lastImage = results.size();
			for( std::size_t i = 0; i < results.size(); ++i ) {
				if( ChatToolResultCarriesImage( results[i].first, results[i].second ) )
					lastImage = i;
			}

			JsonValue contentArr = JsonValue::MakeArray();
			for( std::size_t i = 0; i < results.size(); ++i ) {
				const ChatToolCall& call = results[i].first;
				JsonValue env;
				std::string perr;
				const bool parsed = JsonParse( results[i].second, env, perr ) && env.isObject();

				bool isError = false;
				JsonValue blocks = JsonValue::MakeArray();
				if( !parsed ) {
					isError = true;
					blocks.push_back( MakeTextBlock( "tool transport error: the JSON-RPC response line did not parse as JSON" ) );
				}
				else if( const JsonValue* e = env.find( "error" ) ) {
					// JSON-RPC error envelope -> error tool result.
					isError = true;
					blocks.push_back( MakeTextBlock( JsonSerialize( *e ) ) );
				}
				else {
					const JsonValue& result = env.get( "result" );
					std::string b64;
					if( IsImageResult( call, result, b64 ) ) {
						if( i == lastImage ) {
							blocks.push_back( MakeTextBlock( JsonSerialize(
								StripPngBase64( result, "the PNG is attached as an image block" ) ) ) );
							JsonValue source = JsonValue::MakeObject();
							source.set( "type", JsonValue::MakeString( "base64" ) );
							source.set( "media_type", JsonValue::MakeString( "image/png" ) );
							source.set( "data", JsonValue::MakeString( b64 ) );
							JsonValue img = JsonValue::MakeObject();
							img.set( "type", JsonValue::MakeString( "image" ) );
							img.set( "source", source );
							blocks.push_back( img );
						}
						else {
							// Superseded within this very flush: pack it
							// already elided (see the header comment above).
							blocks.push_back( MakeTextBlock( JsonSerialize(
								StripPngBase64( result, kImageElidedNote ) ) ) );
							blocks.push_back( MakeTextBlock( kImageElidedNote ) );
						}
					}
					else {
						blocks.push_back( MakeTextBlock( JsonSerialize( result ) ) );
					}
				}

				JsonValue tr = JsonValue::MakeObject();
				tr.set( "type", JsonValue::MakeString( "tool_result" ) );
				tr.set( "tool_use_id", JsonValue::MakeString( call.id ) );
				tr.set( "content", blocks );
				if( isError ) tr.set( "is_error", JsonValue::MakeBool( true ) );
				contentArr.push_back( tr );
			}

			JsonValue msg = JsonValue::MakeObject();
			msg.set( "role", JsonValue::MakeString( "user" ) );
			msg.set( "content", contentArr );
			return JsonSerialize( msg );
		}

		std::string AnthropicChatCodec::RewriteElidedImages( const std::string& packedEntryJson ) const
		{
			// Parse + regenerate is LEGAL here: this entry was produced by
			// PackToolResults above (loop-generated), not by a provider --
			// only assistant entries carry the byte-preservation contract.
			JsonValue root;
			std::string perr;
			if( !JsonParse( packedEntryJson, root, perr ) || !root.isObject() ) return packedEntryJson;
			const JsonValue& content = root.get( "content" );
			if( !content.isArray() ) return packedEntryJson;

			bool changed = false;
			JsonValue newContent = JsonValue::MakeArray();
			for( std::size_t i = 0; i < content.size(); ++i ) {
				const JsonValue& tr = content.at( i );
				const JsonValue& blocks = tr.get( "content" );
				if( tr.get( "type" ).asString() != "tool_result" || !blocks.isArray() ) {
					newContent.push_back( tr );
					continue;
				}
				// Swap each {type:"image",...} element for the elision text
				// block; every other member of the tool_result (tool_use_id
				// included) is copied through, so the rewritten entry stays
				// wire-valid.  In an image-bearing tool_result the textual
				// summary's attach note ("the PNG is attached as an image
				// block") is rewritten too -- it is loop-written content
				// (StripPngBase64 produced it) and leaving it would show
				// the model a note contradicting the elided block.
				bool hasImage = false;
				for( std::size_t j = 0; j < blocks.size(); ++j )
					if( blocks.at( j ).get( "type" ).asString() == "image" ) hasImage = true;
				if( !hasImage ) {
					newContent.push_back( tr );
					continue;
				}
				changed = true;
				JsonValue newBlocks = JsonValue::MakeArray();
				for( std::size_t j = 0; j < blocks.size(); ++j ) {
					const JsonValue& b = blocks.at( j );
					const std::string btype = b.get( "type" ).asString();
					if( btype == "image" ) {
						newBlocks.push_back( MakeTextBlock( kImageElidedNote ) );
					}
					else if( btype == "text" ) {
						newBlocks.push_back( MakeTextBlock(
							RewriteElidedSummaryText( b.get( "text" ).asString() ) ) );
					}
					else {
						newBlocks.push_back( b );
					}
				}
				JsonValue newTr = JsonValue::MakeObject();
				const std::vector<std::pair<std::string, JsonValue>>& mem = tr.members();
				for( std::size_t j = 0; j < mem.size(); ++j )
					newTr.set( mem[j].first, mem[j].first == "content" ? newBlocks : mem[j].second );
				newContent.push_back( newTr );
			}
			if( !changed ) return packedEntryJson;

			JsonValue newRoot = JsonValue::MakeObject();
			const std::vector<std::pair<std::string, JsonValue>>& mem = root.members();
			for( std::size_t i = 0; i < mem.size(); ++i )
				newRoot.set( mem[i].first, mem[i].first == "content" ? newContent : mem[i].second );
			return JsonSerialize( newRoot );
		}

		ChatHttpRequest AnthropicChatCodec::BuildRequest(
			const std::string& modelId,
			const std::string& apiKey,
			const std::string& systemPrompt,
			const std::vector<std::string>& rawEntries,
			bool /*forceReasoningEffortNone*/ ) const
		{
			// Anthropic has no reasoning_effort field / tools-vs-effort 400
			// -- the parameter exists only to satisfy the shared
			// IChatProviderCodec::BuildRequest signature (see its header
			// doc); this codec ignores it.
			ChatHttpRequest r;
			r.url = "https://api.anthropic.com/v1/messages";
			// The key appears ONLY here, in the auth header -- control
			// characters stripped so a pasted key cannot inject headers.
			r.headers.push_back( std::make_pair( "content-type", "application/json" ) );
			r.headers.push_back( std::make_pair( "x-api-key", SanitizeHeaderValue( apiKey ) ) );
			r.headers.push_back( std::make_pair( "anthropic-version", "2023-06-01" ) );

			// The body is assembled as a string so assistant entries (raw
			// provider-native JSON) splice in VERBATIM.  No thinking config
			// is set (omitted = adaptive on models that support it).
			std::string body = "{\"model\":";
			JsonAppendEscapedString( body, modelId );
			body += ",\"max_tokens\":" + std::to_string( kAnthropicMaxTokens );
			// Prompt caching: the system prompt + the tool definitions are a
			// large, byte-identical prefix on EVERY request in a session (the
			// system string is fixed and AnthropicToolsJson() is a static
			// built once from a fixed-order table).  Mark the single system
			// block with cache_control:ephemeral so Anthropic caches that
			// whole prefix -- tools render before system in the cache prefix,
			// so one breakpoint on the last (only) system block covers tools
			// AND system together (max 4 breakpoints/request; we use 1).
			// Cache reads bill at ~0.1x input and the 5-minute TTL is
			// refreshed by any request in the window, which a live chat turn
			// trivially satisfies.  Only this codec needs an explicit marker:
			// OpenAI/xAI (auto prefix caching >=1024 tokens) and Gemini
			// (implicit caching) reuse the identical leading prefix with no
			// client action.  An empty system prompt would be an illegal
			// empty cacheable text block, so fall back to the plain-string
			// form in that (never-in-practice) case.
			if( systemPrompt.empty() ) {
				body += ",\"system\":\"\"";
			}
			else {
				body += ",\"system\":[{\"type\":\"text\",\"text\":";
				JsonAppendEscapedString( body, systemPrompt );
				body += ",\"cache_control\":{\"type\":\"ephemeral\"}}]";
			}
			body += ",\"tools\":";
			body += AnthropicToolsJson();
			body += ",\"messages\":[";
			for( std::size_t i = 0; i < rawEntries.size(); ++i ) {
				if( i ) body += ",";
				body += rawEntries[i];
			}
			body += "]}";
			r.body = body;
			return r;
		}

		ChatParsedResponse AnthropicChatCodec::ParseResponse(
			long httpStatus, const std::string& rawBody ) const
		{
			ChatParsedResponse out;
			if( httpStatus != 200 ) {
				out.step = MakeHttpError( ProviderName(), httpStatus, rawBody );
				return out;
			}

			JsonValue root;
			std::string perr;
			if( !JsonParse( rawBody, root, perr ) || !root.isObject() ) {
				out.step = MakeProviderError( ChatErrorKind::Parse,
					"anthropic response did not parse as JSON: " + perr );
				return out;
			}
			const JsonValue& content = root.get( "content" );
			if( !content.isArray() ) {
				out.step = MakeProviderError( ChatErrorKind::Provider,
					"anthropic response carries no content array" );
				return out;
			}

			std::string text;
			// DISPLAY-LAYER ENRICHMENT (regression fix): reasoning text is a
			// PARALLEL extraction alongside `text` -- collected here from
			// every "thinking" content block's "thinking" field, joined by
			// "\n\n" when the turn carries more than one.  This NEVER
			// touches the raw echo below (assistantEntryJson splices the
			// content array VERBATIM, signature and all); it only feeds
			// ChatParsedResponse::reasoningText / ChatStepResult::reasoningText
			// for display.
			std::string reasoningText;
			std::vector<ChatToolCall> calls;
			for( std::size_t i = 0; i < content.size(); ++i ) {
				const JsonValue& block = content.at( i );
				const std::string type = block.get( "type" ).asString();
				if( type == "text" ) {
					text += block.get( "text" ).asString();
				}
				else if( type == "thinking" ) {
					const std::string t = block.get( "thinking" ).asString();
					if( !t.empty() ) {
						if( !reasoningText.empty() ) reasoningText += "\n\n";
						reasoningText += t;
					}
				}
				else if( type == "tool_use" ) {
					ChatToolCall c;
					c.id = block.get( "id" ).asString();
					c.name = block.get( "name" ).asString();
					if( c.id.empty() ) {
						// A tool_use with no id could never be answered (the
						// tool_result must echo tool_use_id) -- executing the
						// rest of the turn would ship an unanswerable block,
						// so refuse the WHOLE response instead of emitting
						// tool_use_id:"" downstream.
						out.step = MakeProviderError( ChatErrorKind::Provider,
							"anthropic tool_use block carries no id; refusing the turn (its result could never be matched)" );
						return out;
					}
					// Duplicate ids within one turn make result matching
					// ambiguous -- ids are the result-matching key and must
					// be unique.  Without this gate, AddToolResult would
					// ignore the second call as already-answered, the flush
					// would synthesize a SECOND result for the SAME id, and
					// the wire would carry duplicate tool_use ids answered by
					// duplicate tool_use_id results (a permanent 400).
					// Refuse the whole turn instead (mirrors the Gemini
					// duplicate-functionCall-id gate).
					for( std::size_t k = 0; k < calls.size(); ++k ) {
						if( calls[k].id == c.id ) {
							out.step = MakeProviderError( ChatErrorKind::Provider,
								"anthropic response repeats tool_use id \"" + c.id +
								"\" -- refusing the turn (results could not be matched unambiguously)" );
							return out;
						}
					}
					// RECORD-OR-REFUSE: an ABSENT "input" key is Anthropic's
					// legal no-args tool_use shape (maps to "{}"); a PRESENT
					// "input" that is not an object (including an explicit
					// "input":null) must not be recorded and then executed
					// with fabricated empty args -- refuse the WHOLE
					// response instead (mirrors the OpenAI malformed-
					// arguments gate below).
					if( block.has( "input" ) ) {
						const JsonValue& input = block.get( "input" );
						if( !input.isObject() ) {
							out.step = MakeProviderError( ChatErrorKind::Provider,
								"anthropic tool_use block \"" + c.name + "\" carries a non-object "
								"\"input\" -- refusing the turn (executing it would fabricate empty args)" );
							return out;
						}
						c.argsJson = JsonSerialize( input );
					}
					else {
						c.argsJson = "{}";
					}
					calls.push_back( c );
				}
				// other block kinds (e.g. server_tool_use): not displayed;
				// the raw echo below preserves them for the provider.
			}

			// EVERY exit from here on goes through AttachReasoning: the
			// disposition below REPLACES out.step wholesale on a refusal
			// (MakeProviderError builds a fresh ChatStepResult), so thinking
			// blocks collected above have to be attached AFTER that assignment
			// or they are dropped.  SIBLING of the Gemini and Responses sites
			// (same bug pattern, same fix).  The stop_reason=="max_tokens" exit
			// is the sharp case: extended thinking that runs into the output
			// cap produces thinking blocks and NOTHING else, so dropping the
			// reasoning discards the entire turn's output.
			//
			// The STRUCTURAL refusals ABOVE (an id-less / duplicate-id /
			// non-object-input tool_use) deliberately stay OUTSIDE this, and
			// that exclusion is a decision rather than an oversight: they
			// reject a MALFORMED body mid-scan, with the content array not read
			// to the end, so the reasoning collected so far is a partial read
			// of a body being declined as untrustworthy.  A disposition refusal
			// below is the opposite -- a WELL-FORMED turn the provider itself
			// ended early -- and there the reasoning is complete and is often
			// all the turn produced.
			const auto AttachReasoning = [&]() -> ChatParsedResponse& {
				out.reasoningText = reasoningText;
				out.step.reasoningText = reasoningText;
				return out;
			};

			const std::string stopReason = root.get( "stop_reason" ).asString();
			if( stopReason == "tool_use" ) {
				if( calls.empty() ) {
					out.step = MakeProviderError( ChatErrorKind::Provider,
						"anthropic stopped with stop_reason \"tool_use\" but no tool_use blocks were present" );
					return AttachReasoning();
				}
				out.step.kind = ChatStepResult::Kind::ToolCalls;
				out.step.toolCalls = calls;
			}
			else if( stopReason == "max_tokens" ) {
				// Distinct, actionable dead-end message (the reply is
				// discarded -- transcript consistency: record nothing).
				out.step = MakeProviderError( ChatErrorKind::MaxTokens,
					"anthropic: the response hit the output-token cap (stop_reason max_tokens) -- "
					"the truncated reply was discarded; try a narrower request" );
				return AttachReasoning();
			}
			else if( stopReason == "refusal" ) {
				out.step = MakeProviderError( ChatErrorKind::Refusal,
					"anthropic: the provider declined this request (stop_reason refusal)" );
				return AttachReasoning();
			}
			else if( !calls.empty() ) {
				// WIRE-INVARIANT GATE: tool_use blocks under a non-tool_use
				// stop_reason (a hostile or corrupted body).  Recording this
				// turn would echo tool_use blocks the loop never pends --
				// so no tool_result could EVER answer them and every later
				// request would replay an unanswered tool_use (a permanent
				// 400).  Refuse the WHOLE response; record nothing.
				out.step = MakeProviderError( ChatErrorKind::Provider,
					"anthropic response carries tool_use blocks under stop_reason \"" + stopReason +
					"\" -- refusing the turn (its calls would be recorded but never answerable)" );
				return AttachReasoning();
			}
			else if( stopReason == "end_turn" ) {
				if( ChatContentIsBlank( text ) ) {
					// A degenerate final turn with no readable text.  The
					// empty-array case (content:[]) also poisons the echo (the
					// API rejects an assistant message with no content blocks);
					// but a NON-empty array whose only text is blank/absent (a
					// whitespace-only text block, or nothing but non-text
					// blocks) is equally a silent blank bubble to the user --
					// testing the extracted `text` for blankness catches both.
					out.step = MakeProviderError( ChatErrorKind::Provider,
						"anthropic ended the turn with no readable text -- refusing the degenerate turn" );
					return AttachReasoning();
				}
				out.step.kind = ChatStepResult::Kind::FinalText;
				out.step.finalText = text;
			}
			else {
				// pause_turn / anything unexpected -> error with the
				// stop_reason in the message.
				out.step = MakeProviderError( ChatErrorKind::Provider,
					"anthropic stopped with stop_reason \"" + stopReason + "\"" );
				return AttachReasoning();
			}
			out.assistantDisplayText = text;
			out.step.assistantDisplayText = text;
			AttachReasoning();

			// The assistant transcript entry: the content array as a RAW
			// byte span of the body (verbatim echo -- signatures intact).
			std::size_t b = 0, e = 0;
			if( RawObjectMember( rawBody, 0, "content", b, e ) ) {
				out.assistantEntryJson = "{\"role\":\"assistant\",\"content\":" +
				                         rawBody.substr( b, e - b ) + "}";
			}
			else {
				// Defensive fallback (should not happen for a body that
				// parsed above): re-serialize; loses byte-fidelity only.
				out.assistantEntryJson = "{\"role\":\"assistant\",\"content\":" +
				                         JsonSerialize( content ) + "}";
			}
			return out;
		}

		ChatUsage AnthropicChatCodec::ParseUsage( const std::string& rawBody ) const
		{
			ChatUsage u;
			JsonValue root;
			std::string perr;
			if( !JsonParse( rawBody, root, perr ) || !root.isObject() ) return u;
			const JsonValue& usage = root.get( "usage" );
			if( !usage.isObject() ) return u;
			u.inputTokens          = ReadTokenCount( usage, "input_tokens" );
			u.outputTokens         = ReadTokenCount( usage, "output_tokens" );
			u.cacheReadInputTokens = ReadTokenCount( usage, "cache_read_input_tokens" );

			// THINKING TOKENS.  PROVIDER DISPOSITION: INCLUSIVE -- Anthropic
			// reports them as a BREAKDOWN of output_tokens
			// (`usage.output_tokens_details.thinking_tokens`), not as a
			// separate summand, so this is published as the reasoning subset
			// and NOT folded in (folding would double count).
			// Evidence from the recorded runs (819 Anthropic usage blocks under
			// evals/runs/**): output_tokens_details is present on all 819, and
			// thinking_tokens is 0 on all 819 because the harness never enables
			// extended thinking.  The field is parsed anyway so that enabling
			// thinking later reports the split without another codec change --
			// this is the provider's own number, never a synthesized one.
			u.reasoningOutputTokens =
				ReadTokenCount( usage.get( "output_tokens_details" ), "thinking_tokens" );
			// A body claiming thinking > output contradicts itself; the shared
			// enforcement clamps it rather than publishing a negative
			// "visible output" downstream.
			EnforceUsageInvariant( u );
			return u;
		}

		std::size_t AnthropicChatCodec::ToolsWireBytes() const
		{
			return AnthropicToolsJson().size();
		}

		//======================================================================
		// (4) GeminiChatCodec
		//======================================================================

		namespace
		{
			//! Gemini's functionResponse.response rides the wire as a protobuf
			//! Struct, which HARD-REJECTS duplicate map keys ("Repeated map
			//! key: '...'" -> HTTP 400, killing the whole turn).  Our JsonValue
			//! happily round-trips duplicate object keys (Json.h: "duplicate
			//! object keys: last wins ... we store an ordered vector of
			//! pairs" -- JsonValue::find() scans from the end so LOOKUPS
			//! already resolve last-wins; only *serialization* blindly
			//! re-emits every stored pair).  A tool result can legitimately
			//! arrive with duplicate keys -- e.g. read_schema's SchemaGenAll()
			//! used to emit the same chunk keyword twice when a legacy alias
			//! (mis_pathtracing_shaderop) shared a descriptor with its
			//! canonical keyword (pathtracing_shaderop); see SchemaGen.cpp for
			//! the source-side fix.  This is the wire-side backstop: whatever
			//! a tool emits, no duplicate key ever reaches Gemini.  Recurses
			//! into array/object children so a nested duplicate (inside an
			//! array element, say) is caught too.  Keeps the LAST value per
			//! key, matching JsonValue::find()'s documented last-wins lookup
			//! semantics so behavior here is consistent with what the rest of
			//! the loop already sees when it reads the (un-deduplicated)
			//! JsonValue via find()/get().  Deliberately scoped to this file
			//! (not JsonParse/JsonSerialize) -- other JsonValue consumers
			//! (e.g. the eval-harness E3 checker) depend on today's
			//! preserve-duplicates serialize behavior.
			JsonValue DedupeJsonKeysLastWins( const JsonValue& v )
			{
				if( v.isArray() ) {
					JsonValue out = JsonValue::MakeArray();
					for( std::size_t i = 0; i < v.size(); ++i )
						out.push_back( DedupeJsonKeysLastWins( v.at( i ) ) );
					return out;
				}
				if( !v.isObject() ) return v;

				const std::vector<std::pair<std::string, JsonValue>>& mem = v.members();
				std::vector<std::string> order;             // first-seen key order
				std::map<std::string, JsonValue> latest;    // key -> last raw value
				for( std::size_t i = 0; i < mem.size(); ++i ) {
					if( latest.find( mem[i].first ) == latest.end() )
						order.push_back( mem[i].first );
					latest[ mem[i].first ] = mem[i].second;  // later duplicate overwrites -> last wins
				}
				JsonValue out = JsonValue::MakeObject();
				for( std::size_t i = 0; i < order.size(); ++i )
					out.set( order[i], DedupeJsonKeysLastWins( latest[ order[i] ] ) );
				return out;
			}
		}

		const char* GeminiChatCodec::ProviderName() const { return "gemini"; }

		const char* GeminiChatCodec::DefaultModelId() const { return "gemini-3.5-flash"; }

		std::string GeminiChatCodec::MakeUserEntry(
			const std::string& text, const std::vector<ChatAttachment>& attachments ) const
		{
			JsonValue parts = JsonValue::MakeArray();
			// Images FIRST, then the caption -- mirrors the Anthropic
			// codec (see the interface doc on MakeUserEntry); an empty
			// attachments vector reproduces the pre-attachment byte shape
			// exactly (one text part, nothing else).
			for( std::size_t i = 0; i < attachments.size(); ++i ) {
				JsonValue blob = JsonValue::MakeObject();
				blob.set( "mimeType", JsonValue::MakeString( attachments[i].mimeType ) );
				blob.set( "data", JsonValue::MakeString( attachments[i].base64Data ) );
				JsonValue part = JsonValue::MakeObject();
				part.set( "inlineData", blob );
				parts.push_back( part );
			}
			// An attachment-only message (no caption) omits the text part
			// entirely rather than sending an empty-string text part --
			// the inlineData part(s) already satisfy Gemini's non-empty-
			// parts requirement.  A text-only call (the common case) is
			// unaffected: text is non-empty by the loop's blank-message
			// no-op contract.
			if( !text.empty() ) {
				JsonValue part = JsonValue::MakeObject();
				part.set( "text", JsonValue::MakeString( text ) );
				parts.push_back( part );
			}
			JsonValue msg = JsonValue::MakeObject();
			msg.set( "role", JsonValue::MakeString( "user" ) );
			msg.set( "parts", parts );
			return JsonSerialize( msg );
		}

		std::string GeminiChatCodec::RewriteElidedUserImages(
			const std::string& userEntryJson, int countToElide ) const
		{
			// Parse + regenerate is LEGAL here: this entry was produced by
			// MakeUserEntry above (loop-generated), never by a provider --
			// only model entries carry the byte-preservation contract.
			// A FunctionResponsePart-style inlineData part carries only
			// media (no text form), so elision replaces the WHOLE part
			// with a plain text part (unlike the tool-result elision,
			// which rewrites the response object in place -- there is no
			// surrounding response object here to carry a note).
			if( countToElide <= 0 ) return userEntryJson;
			JsonValue root;
			std::string perr;
			if( !JsonParse( userEntryJson, root, perr ) || !root.isObject() ) return userEntryJson;
			const JsonValue& parts = root.get( "parts" );
			if( !parts.isArray() ) return userEntryJson;

			bool changed = false;
			int remaining = countToElide;
			JsonValue newParts = JsonValue::MakeArray();
			for( std::size_t i = 0; i < parts.size(); ++i ) {
				const JsonValue& p = parts.at( i );
				if( p.get( "inlineData" ).isObject() && remaining > 0 ) {
					JsonValue textPart = JsonValue::MakeObject();
					textPart.set( "text", JsonValue::MakeString( kUserImageElidedNote ) );
					newParts.push_back( textPart );
					--remaining;
					changed = true;
				}
				else {
					newParts.push_back( p );
				}
			}
			if( !changed ) return userEntryJson;

			JsonValue newRoot = JsonValue::MakeObject();
			const std::vector<std::pair<std::string, JsonValue>>& mem = root.members();
			for( std::size_t i = 0; i < mem.size(); ++i )
				newRoot.set( mem[i].first, mem[i].first == "parts" ? newParts : mem[i].second );
			return JsonSerialize( newRoot );
		}

		std::string GeminiChatCodec::PackToolResults(
			const std::vector<std::pair<ChatToolCall, std::string>>& results ) const
		{
			// ONE user turn: one functionResponse part per call.  When the
			// model's functionCall carried an id it is echoed back as
			// functionResponse.id (the documented matching rule); only
			// synthesized ids are withheld (those calls match by name +
			// order).  read_image results deliver the PNG through
			// functionResponse.parts[].inlineData -- the documented
			// FunctionResponsePart mechanism for multimodal function
			// output -- with the base64 stripped from the JSON half.
			//
			// IMAGE RETENTION, entry-internal half (mirrors the Anthropic
			// codec): within one flush only the LAST image-bearing result
			// keeps a live inlineData part; earlier ones are packed
			// PRE-ELIDED (no parts array; the note already reads elided).
			std::size_t lastImage = results.size();
			for( std::size_t i = 0; i < results.size(); ++i ) {
				if( ChatToolResultCarriesImage( results[i].first, results[i].second ) )
					lastImage = i;
			}

			JsonValue parts = JsonValue::MakeArray();
			for( std::size_t i = 0; i < results.size(); ++i ) {
				const ChatToolCall& call = results[i].first;
				JsonValue env;
				std::string perr;
				const bool parsed = JsonParse( results[i].second, env, perr ) && env.isObject();

				JsonValue respObj = JsonValue::MakeObject();
				std::string b64;
				if( !parsed ) {
					respObj.set( "error", JsonValue::MakeString(
						"tool transport error: the JSON-RPC response line did not parse as JSON" ) );
				}
				else if( const JsonValue* e = env.find( "error" ) ) {
					respObj.set( "error", *e );
				}
				else {
					const JsonValue& result = env.get( "result" );
					if( IsImageResult( call, result, b64 ) ) {
						if( i == lastImage ) {
							respObj = StripPngBase64( result, "the PNG is attached as a functionResponse media part" );
						}
						else {
							// Superseded within this very flush: pack it
							// already elided (note reads elided; b64 cleared
							// so no inlineData parts array is emitted below).
							respObj = StripPngBase64( result, kImageElidedNote );
							b64.clear();
						}
					}
					else if( result.isObject() ) {
						respObj = result;
					}
					else {
						// functionResponse.response must be an object.
						respObj.set( "result", result );
					}
				}

				JsonValue fr = JsonValue::MakeObject();
				if( !call.idSynthesized && !call.id.empty() )
					fr.set( "id", JsonValue::MakeString( call.id ) );
				fr.set( "name", JsonValue::MakeString( call.name ) );
				// Wire-side backstop (see DedupeJsonKeysLastWins above): every
				// branch above (`error` echo, image-strip, `respObj = result`,
				// the scalar/array `result` wrap) can embed a JsonValue parsed
				// straight from the tool's JSON-RPC line, which may carry
				// duplicate object keys our JsonValue tolerates but Gemini's
				// protobuf Struct backend rejects outright.  Applying the
				// dedupe here, once, downstream of every branch, covers all
				// of them without needing a call site in each.
				fr.set( "response", DedupeJsonKeysLastWins( respObj ) );
				if( !b64.empty() ) {
					// FunctionResponse.parts: [{inlineData:{mimeType,data}}]
					// (FunctionResponsePart / FunctionResponseBlob, per the
					// ai.google.dev/api Content reference, 2026-07-02).
					JsonValue blob = JsonValue::MakeObject();
					blob.set( "mimeType", JsonValue::MakeString( "image/png" ) );
					blob.set( "data", JsonValue::MakeString( b64 ) );
					JsonValue frp = JsonValue::MakeObject();
					frp.set( "inlineData", blob );
					JsonValue frParts = JsonValue::MakeArray();
					frParts.push_back( frp );
					fr.set( "parts", frParts );
				}
				JsonValue frPart = JsonValue::MakeObject();
				frPart.set( "functionResponse", fr );
				parts.push_back( frPart );
			}

			JsonValue msg = JsonValue::MakeObject();
			msg.set( "role", JsonValue::MakeString( "user" ) );
			msg.set( "parts", parts );
			return JsonSerialize( msg );
		}

		std::string GeminiChatCodec::RewriteElidedImages( const std::string& packedEntryJson ) const
		{
			// Parse + regenerate is LEGAL here: this entry was produced by
			// PackToolResults above (loop-generated), not by a provider --
			// only assistant (model) entries carry the byte-preservation
			// contract.  The image travels as functionResponse.parts
			// [{inlineData:...}]; a FunctionResponsePart carries only
			// media (no text form exists), so elision DROPS the parts
			// array and rewrites the response object's note to say the
			// image is gone.
			JsonValue root;
			std::string perr;
			if( !JsonParse( packedEntryJson, root, perr ) || !root.isObject() ) return packedEntryJson;
			const JsonValue& parts = root.get( "parts" );
			if( !parts.isArray() ) return packedEntryJson;

			bool changed = false;
			JsonValue newParts = JsonValue::MakeArray();
			for( std::size_t i = 0; i < parts.size(); ++i ) {
				const JsonValue& p = parts.at( i );
				const JsonValue* fr = p.find( "functionResponse" );
				const bool hasImage = fr && fr->isObject() && fr->get( "parts" ).isArray();
				if( !hasImage ) {
					newParts.push_back( p );
					continue;
				}
				changed = true;
				// Rebuild the functionResponse WITHOUT its parts array and
				// with the response note rewritten to the elision text
				// (PackToolResults always sets a note on an image result;
				// append one defensively if it is ever absent).
				JsonValue newFr = JsonValue::MakeObject();
				const std::vector<std::pair<std::string, JsonValue>>& mem = fr->members();
				for( std::size_t j = 0; j < mem.size(); ++j ) {
					if( mem[j].first == "parts" ) continue;
					if( mem[j].first == "response" && mem[j].second.isObject() ) {
						// Rewrite the ATTACH note StripPngBase64 wrote --
						// preferring "image_note" (the key it falls back to
						// when the RPC result already owned a "note"), so an
						// RPC-owned note field is never clobbered.
						JsonValue newResp = JsonValue::MakeObject();
						bool noted = false;
						const std::vector<std::pair<std::string, JsonValue>>& rm = mem[j].second.members();
						// INVARIANT (mirrors RewriteElidedSummaryText's rule):
						// prefer-image_note-else-note matches StripPngBase64's
						// collision fallback; if an RPC result ever grows
						// image_note WITHOUT note, the rules diverge -- keep
						// the key-choice rules in lockstep.
						bool hasImageNote = false;
						for( std::size_t k = 0; k < rm.size(); ++k )
							if( rm[k].first == "image_note" ) hasImageNote = true;
						const char* noteKey = hasImageNote ? "image_note" : "note";
						for( std::size_t k = 0; k < rm.size(); ++k ) {
							if( rm[k].first == noteKey ) {
								newResp.set( rm[k].first, JsonValue::MakeString( kImageElidedNote ) );
								noted = true;
							}
							else {
								newResp.set( rm[k].first, rm[k].second );
							}
						}
						if( !noted )
							newResp.set( "note", JsonValue::MakeString( kImageElidedNote ) );
						newFr.set( "response", newResp );
					}
					else {
						newFr.set( mem[j].first, mem[j].second );
					}
				}
				JsonValue newPart = JsonValue::MakeObject();
				const std::vector<std::pair<std::string, JsonValue>>& pm = p.members();
				for( std::size_t j = 0; j < pm.size(); ++j )
					newPart.set( pm[j].first, pm[j].first == "functionResponse" ? newFr : pm[j].second );
				newParts.push_back( newPart );
			}
			if( !changed ) return packedEntryJson;

			JsonValue newRoot = JsonValue::MakeObject();
			const std::vector<std::pair<std::string, JsonValue>>& mem = root.members();
			for( std::size_t i = 0; i < mem.size(); ++i )
				newRoot.set( mem[i].first, mem[i].first == "parts" ? newParts : mem[i].second );
			return JsonSerialize( newRoot );
		}

		ChatHttpRequest GeminiChatCodec::BuildRequest(
			const std::string& modelId,
			const std::string& apiKey,
			const std::string& systemPrompt,
			const std::vector<std::string>& rawEntries,
			bool /*forceReasoningEffortNone*/ ) const
		{
			// Gemini has no reasoning_effort field / tools-vs-effort 400 --
			// the parameter exists only to satisfy the shared
			// IChatProviderCodec::BuildRequest signature (see its header
			// doc); this codec ignores it.
			ChatHttpRequest r;
			// The model id is percent-escaped so it cannot alter the URL
			// path or smuggle query parameters.
			r.url = "https://generativelanguage.googleapis.com/v1beta/models/" +
			        SanitizeModelIdForUrl( modelId ) + ":generateContent";
			// The key appears ONLY here, in the auth header (NOT as the
			// ?key= query parameter the docs also allow -- a URL leaks into
			// logs/history far more easily than a header).  Control
			// characters are stripped so a pasted key cannot inject headers.
			r.headers.push_back( std::make_pair( "content-type", "application/json" ) );
			r.headers.push_back( std::make_pair( "x-goog-api-key", SanitizeHeaderValue( apiKey ) ) );

			// Gemini requires ALTERNATING roles on the wire, but the
			// transcript can legally hold ADJACENT user entries (an
			// interrupt-flushed tool-results entry followed by the user's
			// next text message).  Merge every run of adjacent role:"user"
			// entries into ONE content whose parts are the run's parts
			// concatenated with the functionResponse parts FIRST (the tool
			// answers must lead the content that answers a functionCall
			// turn).  User entries are loop-generated (MakeUserEntry /
			// PackToolResults), so parse + re-serialize is legal for them;
			// model entries splice VERBATIM (byte-preservation contract)
			// and are never merged -- ParseResponse guarantees a recorded
			// model entry's role really is "model" (a candidate spoofing
			// any other role is refused outright; role-less content is
			// wrapped under an explicit "model"), so a provider body can
			// never smuggle an assistant turn into this user merge.
			std::vector<std::string> wireEntries;
			std::vector<JsonValue>   userRun;
			std::vector<std::size_t> userRunSrc;   // rawEntries index per userRun element
			wireEntries.reserve( rawEntries.size() );
			const auto flushUserRun = [&]() {
				if( userRun.empty() ) return;
				if( userRun.size() == 1 ) {
					// A lone user entry needs no merge: splice the original
					// string (cheaper, and trivially byte-stable).
					wireEntries.push_back( rawEntries[userRunSrc[0]] );
				}
				else {
					JsonValue parts = JsonValue::MakeArray();
					for( int wantFr = 1; wantFr >= 0; --wantFr ) {
						for( std::size_t i = 0; i < userRun.size(); ++i ) {
							const JsonValue& runParts = userRun[i].get( "parts" );
							for( std::size_t j = 0; j < runParts.size(); ++j ) {
								const bool isFr = runParts.at( j ).has( "functionResponse" );
								if( isFr == ( wantFr == 1 ) ) parts.push_back( runParts.at( j ) );
							}
						}
					}
					JsonValue merged = JsonValue::MakeObject();
					merged.set( "role", JsonValue::MakeString( "user" ) );
					merged.set( "parts", parts );
					wireEntries.push_back( JsonSerialize( merged ) );
				}
				userRun.clear();
				userRunSrc.clear();
			};
			for( std::size_t i = 0; i < rawEntries.size(); ++i ) {
				JsonValue e;
				std::string perr;
				if( JsonParse( rawEntries[i], e, perr ) && e.isObject() &&
				    e.get( "role" ).asString() == "user" && e.get( "parts" ).isArray() ) {
					userRun.push_back( e );
					userRunSrc.push_back( i );
				}
				else {
					flushUserRun();
					wireEntries.push_back( rawEntries[i] );
				}
			}
			flushUserRun();

			std::string body = "{\"systemInstruction\":{\"parts\":[{\"text\":";
			JsonAppendEscapedString( body, systemPrompt );
			body += "}]},\"tools\":[{\"functionDeclarations\":";
			body += GeminiFunctionDeclarationsJson();
			body += "}],\"contents\":[";
			for( std::size_t i = 0; i < wireEntries.size(); ++i ) {
				if( i ) body += ",";
				body += wireEntries[i];
			}
			body += "]}";
			r.body = body;
			return r;
		}

		ChatParsedResponse GeminiChatCodec::ParseResponse(
			long httpStatus, const std::string& rawBody ) const
		{
			ChatParsedResponse out;
			if( httpStatus != 200 ) {
				out.step = MakeHttpError( ProviderName(), httpStatus, rawBody );
				return out;
			}

			JsonValue root;
			std::string perr;
			if( !JsonParse( rawBody, root, perr ) || !root.isObject() ) {
				out.step = MakeProviderError( ChatErrorKind::Parse,
					"gemini response did not parse as JSON: " + perr );
				return out;
			}
			const JsonValue& candidates = root.get( "candidates" );
			if( !candidates.isArray() || candidates.size() == 0 ) {
				std::string msg = "gemini response carries no candidates";
				ChatErrorKind kind = ChatErrorKind::Provider;
				const JsonValue& em = root.get( "error" ).get( "message" );
				if( em.isString() ) msg += ": " + em.asString();
				const JsonValue& block = root.get( "promptFeedback" ).get( "blockReason" );
				if( block.isString() ) {
					// A blocked prompt is the provider declining the request.
					msg += " (blockReason " + block.asString() + ")";
					kind = ChatErrorKind::Refusal;
				}
				out.step = MakeProviderError( kind, msg );
				return out;
			}

			const JsonValue& cand = candidates.at( 0 );
			const JsonValue& content = cand.get( "content" );

			// RECORD-OR-REFUSE role gate: the raw-span echo below records
			// candidates[0].content VERBATIM whenever it carries a "role"
			// member, and BuildRequest's adjacent-user merge classifies
			// transcript entries by parsing that role.  A hostile candidate
			// spoofing content.role:"user" would therefore be recorded as
			// the assistant turn and then MERGED into the surrounding user
			// run on the next request -- the conversation collapses into
			// ONE user content with a functionCall part inside it (wire-
			// invalid, permanent poison).  Refuse any candidate whose
			// content.role is present and not "model"; an ABSENT role keeps
			// the wrap-as-model behavior below.
			if( content.isObject() ) {
				const JsonValue* role = content.find( "role" );
				if( role && !( role->isString() && role->asString() == "model" ) ) {
					out.step = MakeProviderError( ChatErrorKind::Provider,
						"gemini candidate content carries role \"" +
						( role->isString() ? role->asString() : std::string( "(non-string)" ) ) +
						"\" instead of \"model\" -- refusing the turn (a spoofed role would corrupt the wire-role alternation on replay)" );
					return out;
				}
			}

			const JsonValue& parts = content.get( "parts" );

			std::string text;
			// DISPLAY-LAYER ENRICHMENT: Gemini marks a thought-summary part by
			// setting `thought: true` ALONGSIDE its "text" key --
			//     {"text":"...","thought":true}
			// -- rather than by a distinct part type.  Such a part is the
			// model's REASONING, not its answer, and must not be concatenated
			// into `text`: `text` is what becomes finalText / the assistant's
			// visible answer, AND what the blank-turn gate below tests, so a
			// thought-only turn would otherwise be mistaken for a real answer.
			// This mirrors the Anthropic codec's `type=="thinking"` branch and
			// the Responses codec's `type=="reasoning"` branch -- reasoning
			// goes to `reasoningText`, never to `text`.
			//
			// The harness does not currently request thought summaries (no
			// generationConfig.thinkingConfig.includeThoughts on the wire --
			// see BuildRequest), so no `thought`-marked part appears in any
			// recorded run today; this branch exists so that enabling them --
			// or Gemini returning one unbidden -- cannot silently corrupt the
			// answer text.  Note that NOT requesting summaries does not mean no
			// thinking happens: ParseUsage below shows 6,086 of the 6,087
			// recorded Gemini responses billed a non-zero thoughtsTokenCount.
			std::string reasoningText;
			std::vector<ChatToolCall> calls;
			std::vector<std::string> usedIds;   // every id captured this turn (provider + synthesized)
			int synthCounter = 0;
			const auto idUsed = [&usedIds]( const std::string& id ) {
				for( std::size_t k = 0; k < usedIds.size(); ++k )
					if( usedIds[k] == id ) return true;
				return false;
			};
			for( std::size_t i = 0; i < parts.size(); ++i ) {
				const JsonValue& part = parts.at( i );
				// Only the literal boolean true marks a thought part; any other
				// shape (absent, false, "true", 1) leaves the part visible, so
				// a malformed marker can never silently swallow real answer
				// text.
				const JsonValue* thoughtFlag = part.find( "thought" );
				const bool isThought = thoughtFlag && thoughtFlag->isBool() && thoughtFlag->asBool();
				if( const JsonValue* t = part.find( "text" ) ) {
					if( t->isString() ) {
						if( isThought ) {
							const std::string& s = t->asString();
							if( !s.empty() ) {
								if( !reasoningText.empty() ) reasoningText += "\n\n";
								reasoningText += s;
							}
						}
						else {
							text += t->asString();
						}
					}
				}
				if( const JsonValue* fc = part.find( "functionCall" ) ) {
					if( !fc->isObject() ) {
						// WIRE-INVARIANT GATE: a functionCall-keyed part whose
						// value is not an object cannot become a pending call,
						// but it WOULD ride in the raw echo -- an un-answered
						// call on every later request.  Refuse the whole turn.
						out.step = MakeProviderError( ChatErrorKind::Provider,
							"gemini response carries a malformed functionCall part (not an object) -- refusing the turn" );
						return out;
					}
					// Gemini 3.x populates functionCall.id and the docs
					// require the response to echo the matching id --
					// capture it.  Synthesize "call_0", "call_1", ...
					// ONLY when absent (those match by name + order and
					// no id field is emitted in the functionResponse).
					ChatToolCall c;
					const JsonValue* idv = fc->find( "id" );
					if( idv && idv->isString() && !idv->asString().empty() ) {
						c.id = idv->asString();
						// Duplicate ids (a repeated provider id, or a provider
						// id colliding with one synthesized earlier this turn)
						// make result matching ambiguous -- refuse the whole
						// turn rather than guess (consistent with the other
						// record-or-refuse gates).
						if( idUsed( c.id ) ) {
							out.step = MakeProviderError( ChatErrorKind::Provider,
								"gemini response repeats functionCall id \"" + c.id +
								"\" -- refusing the turn (results could not be matched unambiguously)" );
							return out;
						}
					}
					else {
						// Skip any candidate that collides with an id already
						// captured this turn (e.g. a provider id literally
						// named "call_0").
						do {
							c.id = "call_" + std::to_string( synthCounter++ );
						} while( idUsed( c.id ) );
						c.idSynthesized = true;
					}
					usedIds.push_back( c.id );
					c.name = fc->get( "name" ).asString();
					// RECORD-OR-REFUSE: an ABSENT "args" key is Gemini's
					// legal no-args functionCall shape (maps to "{}"); a
					// PRESENT "args" that is not an object (including an
					// explicit "args":null) must not be recorded and then
					// executed with fabricated empty args -- refuse the
					// WHOLE response instead (mirrors the Anthropic
					// malformed-input gate above / the OpenAI malformed-
					// arguments gate below).
					if( fc->has( "args" ) ) {
						const JsonValue& args = fc->get( "args" );
						if( !args.isObject() ) {
							out.step = MakeProviderError( ChatErrorKind::Provider,
								"gemini functionCall \"" + c.name + "\" carries non-object "
								"\"args\" -- refusing the turn (executing it would fabricate empty args)" );
							return out;
						}
						c.argsJson = JsonSerialize( args );
					}
					else {
						c.argsJson = "{}";
					}
					calls.push_back( c );
				}
			}

			// DISPLAY-LAYER ENRICHMENT (corrected 2026-07-25; the previous
			// claim here -- "Gemini exposes no reasoning/thinking field on the
			// wire" -- was wrong).  Gemini DOES have a reasoning surface, in
			// two independent forms:
			//   * thought SUMMARIES, returned as parts marked
			//     `{"text":"...","thought":true}`.  They only appear when the
			//     request asks for them via
			//     generationConfig.thinkingConfig.includeThoughts, which this
			//     harness deliberately does NOT send (instrumentation only --
			//     changing how runs are DRIVEN would break comparability with
			//     the recorded baselines).  So reasoningText is "" on every
			//     Gemini turn RECORDED SO FAR -- because of our request shape,
			//     not because the field does not exist.  The parts loop above
			//     routes such parts to reasoningText the moment one arrives.
			//   * thought TOKEN COUNTS (usageMetadata.thoughtsTokenCount),
			//     which arrive unconditionally and were non-zero on 461 of the
			//     462 recorded responses in gemini_only_e12/build_ambiguous
			//     (6,086 of 6,087 across every recorded Gemini run) -- see
			//     ParseUsage.  Thinking happens on essentially every turn; only
			//     its TEXT is withheld.
			// Thought SIGNATURES (`thoughtSignature`, an opaque per-part blob
			// riding on functionCall and text parts alike) are neither
			// summaries nor counts: they are provider-opaque state that must
			// round-trip untouched, which the verbatim raw-span echo below
			// already guarantees.  They are PER PART, not per response: 6,086
			// of the 6,398 recorded content parts carry one (in the 462-block
			// slice, 461 of 493 parts).
			//
			// EVERY exit from here on goes through AttachReasoning.  The
			// disposition below REPLACES out.step wholesale on a refusal
			// (MakeProviderError builds a fresh ChatStepResult), so reasoning
			// collected above has to be attached AFTER that assignment or it is
			// dropped -- and a refused turn is exactly when it matters most:
			// for a thought-ONLY turn the reasoning is the only text the model
			// produced, and returning a ProviderError with an empty
			// reasoningText discards it.  The STRUCTURAL refusals above (a
			// malformed / id-colliding functionCall) deliberately stay OUTSIDE
			// this -- see the Anthropic site for the full rationale: they
			// abandon a mid-scan read of a body being declined as malformed,
			// where a disposition refusal declines a well-formed turn whose
			// reasoning is complete.
			const auto AttachReasoning = [&]() -> ChatParsedResponse& {
				out.reasoningText = reasoningText;
				out.step.reasoningText = reasoningText;
				return out;
			};

			const std::string finishReason = cand.get( "finishReason" ).asString();
			// Classify a non-STOP finish for the error kind: token cap and
			// the safety-ish family get their own kinds so a driver can
			// react without string-matching.
			const ChatErrorKind finishKind =
				( finishReason == "MAX_TOKENS" ) ? ChatErrorKind::MaxTokens :
				( finishReason == "SAFETY" || finishReason == "RECITATION" ||
				  finishReason == "BLOCKLIST" || finishReason == "PROHIBITED_CONTENT" ||
				  finishReason == "SPII" || finishReason == "IMAGE_SAFETY" ) ? ChatErrorKind::Refusal :
				ChatErrorKind::Provider;
			if( !calls.empty() ) {
				// Mirror the Anthropic stop_reason gate: a call turn that
				// was cut short (MAX_TOKENS, SAFETY, ...) may be missing
				// calls or carry mangled arguments -- it must NOT execute.
				// POLICY on an ABSENT/empty finishReason: finishReason is
				// nominally optional in the proto, so TEXT-ONLY turns stay
				// lenient (below) -- but a calls-bearing turn without an
				// explicit STOP cannot be distinguished from one cut short
				// mid-call-list, and executing a truncated call set is
				// worse than refusing.  Require STOP before calls execute.
				if( finishReason != "STOP" ) {
					out.step = MakeProviderError( finishKind,
						finishReason.empty()
							? std::string( "gemini returned function calls without a finishReason -- "
							               "refusing (an explicit STOP is required before a call turn executes)" )
							: "gemini returned function calls but stopped with finishReason \"" +
							  finishReason + "\" -- a truncated call turn is not executed" );
					return AttachReasoning();
				}
				out.step.kind = ChatStepResult::Kind::ToolCalls;
				out.step.toolCalls = calls;
			}
			else if( finishReason == "STOP" || finishReason.empty() ) {
				if( ChatContentIsBlank( text ) ) {
					// A degenerate final turn with no readable text.  A
					// missing/empty parts array (parts:null or []) poisons the
					// echo (a partless model turn is invalid on replay); a
					// NON-empty parts array whose only text is blank/absent (a
					// whitespace-only text part, or nothing but non-text parts
					// such as inlineData) is equally a silent blank bubble --
					// testing the extracted `text` for blankness catches both
					// (text is "" when parts is not an array).  A turn
					// whose ONLY text lives in `thought:true` parts lands
					// here too, and correctly so: a thought summary is the
					// model's reasoning, not its answer -- but the reasoning
					// itself still rides out on the error (AttachReasoning),
					// since it is then the only text the model produced.
					out.step = MakeProviderError( ChatErrorKind::Provider,
						"gemini candidate carries no readable text -- refusing the degenerate turn" );
					return AttachReasoning();
				}
				out.step.kind = ChatStepResult::Kind::FinalText;
				out.step.finalText = text;
			}
			else {
				// SAFETY / MAX_TOKENS / RECITATION / ... -> error with the
				// finishReason in the message.
				out.step = MakeProviderError( finishKind,
					"gemini stopped with finishReason \"" + finishReason + "\"" );
				return AttachReasoning();
			}
			out.assistantDisplayText = text;
			out.step.assistantDisplayText = text;
			AttachReasoning();

			// Raw-span echo of candidates[0].content (verbatim -- preserves
			// provider-opaque fields such as thought signatures).
			std::size_t cb = 0, ce = 0, eb = 0, ee = 0, vb = 0, ve = 0;
			if( RawObjectMember( rawBody, 0, "candidates", cb, ce ) &&
			    RawArrayElement( rawBody, cb, 0, eb, ee ) &&
			    RawObjectMember( rawBody, eb, "content", vb, ve ) ) {
				if( content.has( "role" ) ) {
					out.assistantEntryJson = rawBody.substr( vb, ve - vb );
				}
				else {
					// Defensive: a content object with no role -- wrap the
					// raw parts span under an explicit model role.
					std::size_t pb = 0, pe = 0;
					if( RawObjectMember( rawBody, vb, "parts", pb, pe ) ) {
						out.assistantEntryJson = "{\"role\":\"model\",\"parts\":" +
						                         rawBody.substr( pb, pe - pb ) + "}";
					}
				}
			}
			if( out.assistantEntryJson.empty() ) {
				// Defensive fallback: re-serialize (loses byte-fidelity only).
				JsonValue msg = JsonValue::MakeObject();
				msg.set( "role", JsonValue::MakeString( "model" ) );
				msg.set( "parts", parts );
				out.assistantEntryJson = JsonSerialize( msg );
			}
			return out;
		}

		ChatUsage GeminiChatCodec::ParseUsage( const std::string& rawBody ) const
		{
			ChatUsage u;
			JsonValue root;
			std::string perr;
			if( !JsonParse( rawBody, root, perr ) || !root.isObject() ) return u;
			const JsonValue& m = root.get( "usageMetadata" );
			if( !m.isObject() ) return u;
			u.inputTokens          = ReadTokenCount( m, "promptTokenCount" );
			u.outputTokens         = ReadTokenCount( m, "candidatesTokenCount" );
			u.cacheReadInputTokens = ReadTokenCount( m, "cachedContentTokenCount" );

			// THINKING TOKENS (2026-07-25).  PROVIDER DISPOSITION: SEPARATE
			// SUMMAND -- Gemini bills hidden reasoning as output but reports it
			// in its OWN counter, `thoughtsTokenCount`, which
			// `candidatesTokenCount` does NOT include.  Dropping it made every
			// Gemini cost figure downstream wrong: across the 20 recorded
			// Gemini eval cells (954 LLM calls -- evals/runs/gemini_only_e12
			// and evals/runs/ask_user_board_e12, 10 cells each) the harness
			// recorded 168,582 output tokens against 425,194 thinking tokens
			// actually generated.  Real generation was 593,776, so the recorded
			// figure was 28 % of it -- a ~72 % undercount.
			//
			// EMPIRICAL BASIS for folding it into outputTokens rather than
			// leaving it purely informational (measured, not assumed; the
			// identity is tested only on blocks carrying all four numbers):
			//                                       build_ambiguous  ALL gemini
			//                                       (gemini_only_e12) runs
			//     total == prompt+candidates+thoughts   461/461      6086/6086
			//     total == prompt+candidates              0/461         0/6086
			// i.e. candidates and thoughts are DISJOINT summands of the total,
			// so candidatesTokenCount alone is the VISIBLE output only.  Per
			// ChatUsage's contract, outputTokens must be the total billed
			// generation, so thoughts are added in and also published verbatim
			// as the reasoning subset.  thoughtsTokenCount was present AND
			// non-zero on 461 of those 462 blocks (6,086 of 6,087 across all
			// recorded Gemini runs) even though the harness never requests
			// thought SUMMARIES -- the tokens are generated (and charged)
			// regardless of whether their text is returned.  The one odd block
			// out carries NEITHER candidatesTokenCount NOR thoughtsTokenCount
			// (an empty generation), which is why the identity above is 461 of
			// 461 testable blocks rather than 462 of 462.
			//
			// The fold is a PER-PROVIDER decision from that evidence, never
			// re-derived per response from the body's own arithmetic -- see the
			// PROVIDER DISPOSITION note in OpenAIChatCodec::ParseUsage.
			u.reasoningOutputTokens = ReadTokenCount( m, "thoughtsTokenCount" );
			FoldReasoningIntoOutput( u );
			EnforceUsageInvariant( u );
			return u;
		}

		std::size_t GeminiChatCodec::ToolsWireBytes() const
		{
			return GeminiFunctionDeclarationsJson().size();
		}

		//======================================================================
		// (5) OpenAIChatCodec
		//======================================================================

		OpenAIChatCodec::OpenAIChatCodec()
		{
			mConfig.providerName   = "openai";
			mConfig.baseUrl        = "https://api.openai.com/v1/responses";
			mConfig.defaultModelId = "gpt-5.6-terra";
			mConfig.requiresAuth   = true;
			mConfig.useResponsesApi = true;
		}

		OpenAIChatCodec::OpenAIChatCodec( const Config& config ) : mConfig( config )
		{
		}

		const char* OpenAIChatCodec::ProviderName() const { return mConfig.providerName.c_str(); }

		const char* OpenAIChatCodec::DefaultModelId() const { return mConfig.defaultModelId.c_str(); }

		std::string OpenAIChatCodec::MakeUserEntry(
			const std::string& text, const std::vector<ChatAttachment>& attachments ) const
		{
			JsonValue msg = JsonValue::MakeObject();
			msg.set( "role", JsonValue::MakeString( "user" ) );
			if( mConfig.useResponsesApi ) {
				if( attachments.empty() ) {
					msg.set( "content", JsonValue::MakeString( text ) );
					return JsonSerialize( msg );
				}
				JsonValue content = JsonValue::MakeArray();
				for( std::size_t i = 0; i < attachments.size(); ++i )
					content.push_back( MakeOpenAIResponsesImageBlock(
						attachments[i].mimeType, attachments[i].base64Data ) );
				if( !text.empty() ) content.push_back( MakeOpenAIResponsesTextBlock( text ) );
				msg.set( "content", content );
				return JsonSerialize( msg );
			}
			if( attachments.empty() ) {
				msg.set( "content", JsonValue::MakeString( text ) );
				return JsonSerialize( msg );
			}

			JsonValue content = JsonValue::MakeArray();
			for( std::size_t i = 0; i < attachments.size(); ++i )
				content.push_back( MakeOpenAIImageUrlBlock( attachments[i].mimeType,
				                                            attachments[i].base64Data ) );
			if( !text.empty() ) content.push_back( MakeOpenAITextContentBlock( text ) );
			msg.set( "content", content );
			return JsonSerialize( msg );
		}

		std::string OpenAIChatCodec::RewriteElidedUserImages(
			const std::string& userEntryJson, int countToElide ) const
		{
			// Parse + regenerate is LEGAL here: this entry was produced by
			// MakeUserEntry above (loop-generated), never by the provider.
			if( countToElide <= 0 ) return userEntryJson;
			JsonValue root;
			std::string perr;
			if( !JsonParse( userEntryJson, root, perr ) || !root.isObject() ) return userEntryJson;
			const JsonValue& content = root.get( "content" );
			if( !content.isArray() ) return userEntryJson;

			bool changed = false;
			int remaining = countToElide;
			JsonValue newContent = JsonValue::MakeArray();
			for( std::size_t i = 0; i < content.size(); ++i ) {
				const JsonValue& b = content.at( i );
				const std::string blockType = b.get( "type" ).asString();
				if( ( blockType == "image_url" || blockType == "input_image" ) && remaining > 0 ) {
					newContent.push_back( mConfig.useResponsesApi
						? MakeOpenAIResponsesTextBlock( kUserImageElidedNote )
						: MakeOpenAITextContentBlock( kUserImageElidedNote ) );
					--remaining;
					changed = true;
				}
				else {
					newContent.push_back( b );
				}
			}
			if( !changed ) return userEntryJson;

			JsonValue newRoot = JsonValue::MakeObject();
			const std::vector<std::pair<std::string, JsonValue>>& mem = root.members();
			for( std::size_t i = 0; i < mem.size(); ++i )
				newRoot.set( mem[i].first, mem[i].first == "content" ? newContent : mem[i].second );
			return JsonSerialize( newRoot );
		}

		std::string OpenAIChatCodec::PackToolResults(
			const std::vector<std::pair<ChatToolCall, std::string>>& results ) const
		{
			// Chat Completions requires one role:"tool" message per
			// tool_call_id.  It does not carry image bytes inside that tool
			// message, so the LAST read_image PNG is attached as a following
			// user message; earlier image results in the same flush are
			// packed pre-elided to preserve the "one live render image"
			// invariant.
			std::size_t lastImage = results.size();
			for( std::size_t i = 0; i < results.size(); ++i ) {
				if( ChatToolResultCarriesImage( results[i].first, results[i].second ) )
					lastImage = i;
			}

			JsonValue messages = JsonValue::MakeArray();
			std::string liveB64;
			for( std::size_t i = 0; i < results.size(); ++i ) {
				const ChatToolCall& call = results[i].first;
				JsonValue env;
				std::string perr;
				const bool parsed = JsonParse( results[i].second, env, perr ) && env.isObject();

				JsonValue payload = JsonValue::MakeObject();
				std::string b64;
				if( !parsed ) {
					payload.set( "error", JsonValue::MakeString(
						"tool transport error: the JSON-RPC response line did not parse as JSON" ) );
				}
				else if( const JsonValue* e = env.find( "error" ) ) {
					payload.set( "error", *e );
				}
				else {
					const JsonValue& result = env.get( "result" );
					if( IsImageResult( call, result, b64 ) ) {
						if( i == lastImage ) {
							payload = StripPngBase64( result,
								"the PNG is attached as a following user image_url message" );
							liveB64 = b64;
						}
						else {
							payload = StripPngBase64( result, kImageElidedNote );
						}
					}
					else if( result.isObject() ) {
						payload = result;
					}
					else {
						payload.set( "result", result );
					}
				}

				JsonValue msg = JsonValue::MakeObject();
				if( mConfig.useResponsesApi ) {
					msg.set( "type", JsonValue::MakeString( "function_call_output" ) );
					msg.set( "call_id", JsonValue::MakeString( call.id ) );
					msg.set( "output", JsonValue::MakeString( JsonSerialize( payload ) ) );
				}
				else {
					msg.set( "role", JsonValue::MakeString( "tool" ) );
					msg.set( "tool_call_id", JsonValue::MakeString( call.id ) );
					msg.set( "content", JsonValue::MakeString( JsonSerialize( payload ) ) );
				}
				messages.push_back( msg );
			}

			if( !liveB64.empty() ) {
				JsonValue content = JsonValue::MakeArray();
				content.push_back( mConfig.useResponsesApi
					? MakeOpenAIResponsesTextBlock( "The PNG returned by read_image is attached below." )
					: MakeOpenAITextContentBlock( "The PNG returned by read_image is attached below." ) );
				content.push_back( mConfig.useResponsesApi
					? MakeOpenAIResponsesImageBlock( "image/png", liveB64 )
					: MakeOpenAIImageUrlBlock( "image/png", liveB64 ) );
				JsonValue imageMsg = JsonValue::MakeObject();
				imageMsg.set( "role", JsonValue::MakeString( "user" ) );
				imageMsg.set( "content", content );
				messages.push_back( imageMsg );
			}
			return JsonSerialize( messages );
		}

		std::string OpenAIChatCodec::RewriteElidedImages( const std::string& packedEntryJson ) const
		{
			// PackToolResults emits a loop-owned ARRAY of messages.  Elide
			// the user image_url message and rewrite the tool summary note
			// that pointed at it.
			JsonValue root;
			std::string perr;
			if( !JsonParse( packedEntryJson, root, perr ) || !root.isArray() ) return packedEntryJson;

			bool changed = false;
			JsonValue out = JsonValue::MakeArray();
			for( std::size_t i = 0; i < root.size(); ++i ) {
				const JsonValue& msg = root.at( i );
				const bool chatTool = msg.get( "role" ).asString() == "tool" &&
				                      msg.get( "content" ).isString();
				const bool responsesTool = msg.get( "type" ).asString() == "function_call_output" &&
				                           msg.get( "output" ).isString();
				if( chatTool || responsesTool ) {
					const std::string rewritten =
						RewriteElidedSummaryText( msg.get( chatTool ? "content" : "output" ).asString() );
					if( rewritten != msg.get( chatTool ? "content" : "output" ).asString() ) changed = true;
					JsonValue newMsg = JsonValue::MakeObject();
					const std::vector<std::pair<std::string, JsonValue>>& mem = msg.members();
					for( std::size_t j = 0; j < mem.size(); ++j )
						newMsg.set( mem[j].first, mem[j].first == ( chatTool ? "content" : "output" )
							? JsonValue::MakeString( rewritten ) : mem[j].second );
					out.push_back( newMsg );
					continue;
				}

				const JsonValue& content = msg.get( "content" );
				if( msg.get( "role" ).asString() == "user" && content.isArray() ) {
					JsonValue newContent = JsonValue::MakeArray();
					bool msgChanged = false;
					for( std::size_t j = 0; j < content.size(); ++j ) {
						const JsonValue& b = content.at( j );
						const std::string blockType = b.get( "type" ).asString();
						if( blockType == "image_url" || blockType == "input_image" ) {
							newContent.push_back( mConfig.useResponsesApi
								? MakeOpenAIResponsesTextBlock( kImageElidedNote )
								: MakeOpenAITextContentBlock( kImageElidedNote ) );
							msgChanged = true;
						}
						else if( blockType == "text" || blockType == "input_text" ) {
							std::string t = b.get( "text" ).asString();
							if( t.find( "attached" ) != std::string::npos ) {
								t = kImageElidedNote;
								msgChanged = true;
							}
							newContent.push_back( mConfig.useResponsesApi
								? MakeOpenAIResponsesTextBlock( t ) : MakeOpenAITextContentBlock( t ) );
						}
						else {
							newContent.push_back( b );
						}
					}
					if( msgChanged ) {
						changed = true;
						JsonValue newMsg = JsonValue::MakeObject();
						const std::vector<std::pair<std::string, JsonValue>>& mem = msg.members();
						for( std::size_t j = 0; j < mem.size(); ++j )
							newMsg.set( mem[j].first,
								mem[j].first == "content" ? newContent : mem[j].second );
						out.push_back( newMsg );
						continue;
					}
				}
				out.push_back( msg );
			}
			return changed ? JsonSerialize( out ) : packedEntryJson;
		}

		ChatHttpRequest OpenAIChatCodec::BuildRequest(
			const std::string& modelId,
			const std::string& apiKey,
			const std::string& systemPrompt,
			const std::vector<std::string>& rawEntries,
			bool forceReasoningEffortNone ) const
		{
			ChatHttpRequest r;
			r.url = mConfig.baseUrl;
			r.timeoutSeconds = mConfig.requestTimeoutSeconds;
			r.headers.push_back( std::make_pair( "content-type", "application/json" ) );
			// Emit the Bearer header when the provider requires auth (OpenAI,
			// xAI) OR when a key was supplied anyway (a local server started
			// with --api-key).  A KEYLESS local provider (requiresAuth=false,
			// empty key) emits NO Authorization header at all -- Ollama and
			// friends expect none, and an empty "Bearer " is worse than
			// absent.  This is the key-hygiene inverse of the OpenAI path:
			// no key in, no auth header out.
			if( mConfig.requiresAuth || !apiKey.empty() ) {
				r.headers.push_back( std::make_pair( "authorization",
					"Bearer " + SanitizeHeaderValue( apiKey ) ) );
			}

			std::string body = "{\"model\":";
			JsonAppendEscapedString( body, modelId );
			body += mConfig.useResponsesApi
				? ",\"max_output_tokens\":" + std::to_string( kOpenAIMaxCompletionTokens )
				: ",\"max_completion_tokens\":" + std::to_string( kOpenAIMaxCompletionTokens );
			if( mConfig.useResponsesApi ) {
				// OpenAI recommends medium as the balanced default for agentic,
				// multi-step tool workflows.  Naming it explicitly makes the
				// eval configuration reproducible across server-default changes.
				body += ",\"reasoning\":{\"effort\":\"medium\"},\"instructions\":";
				JsonAppendEscapedString( body, systemPrompt );
				body += ",\"input\":[";
				bool firstInput = true;
				for( std::size_t i = 0; i < rawEntries.size(); ++i ) {
					JsonValue e;
					std::string perr;
					if( JsonParse( rawEntries[i], e, perr ) && e.isArray() ) {
						for( std::size_t j = 0; j < e.size(); ++j ) {
							if( !firstInput ) body += ",";
							body += JsonSerialize( e.at( j ) );
							firstInput = false;
						}
					}
					else {
						if( !firstInput ) body += ",";
						body += rawEntries[i];
						firstInput = false;
					}
				}
				body += "],\"tools\":";
				body += OpenAIResponsesToolsJson();
				body += ",\"store\":false}";
				r.body = body;
				return r;
			}
			// REASONING-MODEL TOOLS-VS-EFFORT 400 RECOVERY (see
			// ChatStepResult::retryReasoningEffortNone): a reasoning-family
			// model's server-side default reasoning_effort is incompatible
			// with function tools over this endpoint.  This codec never
			// sends reasoning_effort otherwise -- there is no default value
			// to omit -- so once the loop has proven the conflict, EXPLICITLY
			// override it to "none" (the alternative the provider's own
			// error message documents, short of switching off
			// /v1/chat/completions entirely).
			if( forceReasoningEffortNone ) body += ",\"reasoning_effort\":\"none\"";
			body += ",\"messages\":[{\"role\":\"system\",\"content\":";
			JsonAppendEscapedString( body, systemPrompt );
			body += "}";
			for( std::size_t i = 0; i < rawEntries.size(); ++i ) {
				JsonValue e;
				std::string perr;
				if( JsonParse( rawEntries[i], e, perr ) && e.isArray() ) {
					for( std::size_t j = 0; j < e.size(); ++j ) {
						body += ",";
						body += JsonSerialize( e.at( j ) );
					}
				}
				else {
					body += ",";
					body += rawEntries[i];
				}
			}
			body += "],\"tools\":";
			body += OpenAIToolsJson();
			body += "}";
			r.body = body;
			return r;
		}

		ChatParsedResponse OpenAIChatCodec::ParseResponse(
			long httpStatus, const std::string& rawBody ) const
		{
			ChatParsedResponse out;
			if( httpStatus != 200 ) {
				out.step = MakeHttpError( ProviderName(), httpStatus, rawBody );
				return out;
			}

			JsonValue root;
			std::string perr;
			if( !JsonParse( rawBody, root, perr ) || !root.isObject() ) {
				out.step = MakeProviderError( ChatErrorKind::Parse,
					"openai response did not parse as JSON: " + perr );
				return out;
			}
			// Parse by response shape rather than configured request mode.  This
			// keeps the codec useful for archived Chat-Completions trajectories
			// and OpenAI-compatible fixture tests during the migration.
			if( mConfig.useResponsesApi && ( root.find( "output" ) || root.find( "status" ) ) ) {
				// Reasoning-item summaries, harvested from `output` WITHOUT the
				// strict per-item validation the completed path applies (a
				// non-completed turn is under no obligation to carry a
				// well-formed message item, and refusing to read its reasoning
				// because some OTHER item is malformed would defeat the point).
				// This exists because the status dispositions below return
				// BEFORE the completed path's output loop ever runs, so the
				// AttachReasoning lambda further down cannot reach them.
				const auto ResponsesReasoningText = []( const JsonValue& output ) -> std::string {
					std::string s;
					if( !output.isArray() ) return s;
					for( std::size_t i = 0; i < output.size(); ++i ) {
						const JsonValue& item = output.at( i );
						if( item.get( "type" ).asString() != "reasoning" ) continue;
						const JsonValue& summary = item.get( "summary" );
						if( !summary.isArray() ) continue;
						for( std::size_t j = 0; j < summary.size(); ++j ) {
							const std::string t = summary.at( j ).get( "text" ).asString();
							if( t.empty() ) continue;
							if( !s.empty() ) s += "\n\n";
							s += t;
						}
					}
					return s;
				};

				const std::string status = root.get( "status" ).asString();
				if( status != "completed" ) {
					// SIBLING of Anthropic's stop_reason=="max_tokens" exit, and
					// the same sharp case: `incomplete` with
					// incomplete_details.reason "max_output_tokens" is the
					// NORMAL output-cap outcome on the Responses wire (not a
					// protocol violation), and OpenAI populates `output` with
					// the reasoning item on exactly that turn -- a reasoning-
					// only response whose summary is then the ONLY text the
					// model produced.  Returning before reading it discarded the
					// entire turn's output.
					if( status == "incomplete" ) {
						const std::string reason =
							root.get( "incomplete_details" ).get( "reason" ).asString();
						out.step = MakeProviderError(
							reason == "max_output_tokens" ? ChatErrorKind::MaxTokens : ChatErrorKind::Provider,
							"openai response incomplete" + ( reason.empty() ? std::string() : ": " + reason ) );
					}
					else {
						out.step = MakeProviderError( ChatErrorKind::Provider,
							"openai response status is \"" + status + "\" instead of \"completed\"" );
					}
					// AFTER the MakeProviderError assignment (it builds a FRESH
					// ChatStepResult, so an earlier write would be overwritten).
					out.reasoningText = ResponsesReasoningText( root.get( "output" ) );
					out.step.reasoningText = out.reasoningText;
					return out;
				}
				const JsonValue& output = root.get( "output" );
				if( !output.isArray() ) {
					out.step = MakeProviderError( ChatErrorKind::Provider,
						"openai response carries no output array" );
					return out;
				}

				std::vector<ChatToolCall> calls;
				std::string text;
				std::string refusal;
				std::string reasoningText;
				for( std::size_t i = 0; i < output.size(); ++i ) {
					const JsonValue& item = output.at( i );
					if( !item.isObject() || !item.get( "type" ).isString() ) {
						out.step = MakeProviderError( ChatErrorKind::Provider,
							"openai Responses output item lacks an object type" );
						return out;
					}
					const std::string type = item.get( "type" ).asString();
					if( type == "function_call" ) {
						ChatToolCall c;
						c.id = item.get( "call_id" ).asString();
						c.name = item.get( "name" ).asString();
						if( c.id.empty() || c.name.empty() ) {
							out.step = MakeProviderError( ChatErrorKind::Provider,
								"openai Responses function_call lacks call_id or name" );
							return out;
						}
						for( std::size_t k = 0; k < calls.size(); ++k ) {
							if( calls[k].id == c.id ) {
								out.step = MakeProviderError( ChatErrorKind::Provider,
									"openai response repeats function call_id \"" + c.id + "\"" );
								return out;
							}
						}
						JsonValue args;
						std::string aerr;
						if( !JsonParse( item.get( "arguments" ).asString(), args, aerr ) ||
						    !args.isObject() ) {
							out.step = MakeProviderError( ChatErrorKind::Provider,
								"openai function_call \"" + c.name +
								"\" carries malformed arguments JSON" );
							return out;
						}
						c.argsJson = JsonSerialize( args );
						calls.push_back( c );
					}
					else if( type == "message" ) {
						if( item.get( "role" ).asString() != "assistant" ) {
							out.step = MakeProviderError( ChatErrorKind::Provider,
								"openai Responses message role is not assistant" );
							return out;
						}
						const JsonValue& content = item.get( "content" );
						if( !content.isArray() ) {
							out.step = MakeProviderError( ChatErrorKind::Provider,
								"openai Responses message content is not an array" );
							return out;
						}
						for( std::size_t j = 0; j < content.size(); ++j ) {
							const JsonValue& part = content.at( j );
							const std::string partType = part.get( "type" ).asString();
							if( partType == "output_text" ) {
								if( !part.get( "text" ).isString() ) {
									out.step = MakeProviderError( ChatErrorKind::Provider,
										"openai Responses output_text has no text string" );
									return out;
								}
								if( !text.empty() ) text += "\n";
								text += part.get( "text" ).asString();
							}
							else if( partType == "refusal" ) {
								if( !part.get( "refusal" ).isString() ) {
									out.step = MakeProviderError( ChatErrorKind::Provider,
										"openai Responses refusal has no refusal string" );
									return out;
								}
								if( !refusal.empty() ) refusal += "\n";
								refusal += part.get( "refusal" ).asString();
							}
							else {
								out.step = MakeProviderError( ChatErrorKind::Provider,
									"openai Responses message has unsupported content type \"" +
									partType + "\"" );
								return out;
							}
						}
					}
					else if( type == "reasoning" ) {
						const JsonValue& summary = item.get( "summary" );
						if( !summary.isArray() ) continue;
						for( std::size_t j = 0; j < summary.size(); ++j ) {
							const std::string s = summary.at( j ).get( "text" ).asString();
							if( s.empty() ) continue;
							if( !reasoningText.empty() ) reasoningText += "\n\n";
							reasoningText += s;
						}
					}
					else {
						out.step = MakeProviderError( ChatErrorKind::Provider,
							"openai Responses output has unsupported item type \"" +
							type + "\"" );
						return out;
					}
				}

				// EVERY exit from here on goes through AttachReasoning: the
				// disposition below REPLACES out.step wholesale on a refusal
				// (MakeProviderError builds a fresh ChatStepResult), so the
				// reasoning-item summaries collected above have to be attached
				// AFTER that assignment or they are dropped.  SIBLING of the
				// Gemini and Anthropic sites (same bug pattern, same fix): a
				// Responses turn that emits a reasoning item and no message is
				// exactly the "blank" case below, and its summary text is then
				// the only text the model produced.  The STRUCTURAL refusals
				// above (a typeless / unsupported output item, a malformed
				// function_call) deliberately stay OUTSIDE this -- see the
				// Anthropic site for the full rationale.  The non-"completed"
				// STATUS exits earlier are a third case again: they never reach
				// the output loop at all, so they harvest their reasoning
				// through ResponsesReasoningText instead.
				const auto AttachReasoning = [&]() -> ChatParsedResponse& {
					out.reasoningText = reasoningText;
					out.step.reasoningText = reasoningText;
					return out;
				};

				if( !calls.empty() ) {
					out.step.kind = ChatStepResult::Kind::ToolCalls;
					out.step.toolCalls = calls;
				}
				else if( !refusal.empty() ) {
					out.step = MakeProviderError( ChatErrorKind::Refusal,
						"openai declined this request: " + refusal );
					return AttachReasoning();
				}
				else if( ChatContentIsBlank( text ) ) {
					out.step = MakeProviderError( ChatErrorKind::Provider,
						"openai ended the response with no text or function calls" );
					return AttachReasoning();
				}
				else {
					out.step.kind = ChatStepResult::Kind::FinalText;
					out.step.finalText = text;
				}
				out.assistantDisplayText = text;
				out.step.assistantDisplayText = text;
				AttachReasoning();

				std::size_t ob = 0, oe = 0;
				if( RawObjectMember( rawBody, 0, "output", ob, oe ) )
					out.assistantEntryJson = rawBody.substr( ob, oe - ob );
				if( out.assistantEntryJson.empty() )
					out.assistantEntryJson = JsonSerialize( output );
				return out;
			}
			const JsonValue& choices = root.get( "choices" );
			if( !choices.isArray() || choices.size() == 0 ) {
				out.step = MakeProviderError( ChatErrorKind::Provider,
					"openai response carries no choices" );
				return out;
			}

			const JsonValue& choice = choices.at( 0 );
			const JsonValue& msg = choice.get( "message" );
			if( !msg.isObject() ) {
				out.step = MakeProviderError( ChatErrorKind::Provider,
					"openai choice carries no assistant message object" );
				return out;
			}
			const JsonValue* role = msg.find( "role" );
			if( role && !( role->isString() && role->asString() == "assistant" ) ) {
				out.step = MakeProviderError( ChatErrorKind::Provider,
					"openai message carries role \"" +
					( role->isString() ? role->asString() : std::string( "(non-string)" ) ) +
					"\" instead of \"assistant\" -- refusing the turn" );
				return out;
			}

			const std::string text = JsonObjectContentToText( msg.get( "content" ) );

			// DISPLAY-LAYER ENRICHMENT (regression fix): this ONE codec
			// serves OpenAI, xAI, and a local/Ollama-style server (see the
			// class doc), and each names its reasoning field differently --
			// Ollama's /api/chat-compatible local server emits
			// `message.reasoning`, xAI emits `message.reasoning_content`.
			// Prefer `reasoning` when it is a non-empty string, else fall
			// back to `reasoning_content`; a plain gpt response carries
			// neither, so reasoningText stays "" (its default) exactly as
			// documented on ChatStepResult::reasoningText.
			std::string reasoningText;
			if( const JsonValue* r = msg.find( "reasoning" ) ) {
				if( r->isString() && !r->asString().empty() ) reasoningText = r->asString();
			}
			if( reasoningText.empty() ) {
				if( const JsonValue* rc = msg.find( "reasoning_content" ) ) {
					if( rc->isString() && !rc->asString().empty() ) reasoningText = rc->asString();
				}
			}

			std::vector<ChatToolCall> calls;
			const JsonValue& toolCalls = msg.get( "tool_calls" );
			if( toolCalls.isArray() ) {
				for( std::size_t i = 0; i < toolCalls.size(); ++i ) {
					const JsonValue& tc = toolCalls.at( i );
					if( !tc.isObject() || tc.get( "type" ).asString() != "function" ) {
						out.step = MakeProviderError( ChatErrorKind::Provider,
							"openai response carries a malformed/non-function tool_call -- refusing the turn" );
						return out;
					}
					ChatToolCall c;
					c.id = tc.get( "id" ).asString();
					if( c.id.empty() ) {
						out.step = MakeProviderError( ChatErrorKind::Provider,
							"openai tool_call carries no id; refusing the turn (its result could never be matched)" );
						return out;
					}
					for( std::size_t k = 0; k < calls.size(); ++k ) {
						if( calls[k].id == c.id ) {
							out.step = MakeProviderError( ChatErrorKind::Provider,
								"openai response repeats tool_call id \"" + c.id +
								"\" -- refusing the turn (results could not be matched unambiguously)" );
							return out;
						}
					}
					const JsonValue& fn = tc.get( "function" );
					c.name = fn.get( "name" ).asString();
					if( c.name.empty() ) {
						out.step = MakeProviderError( ChatErrorKind::Provider,
							"openai tool_call carries no function name -- refusing the turn" );
						return out;
					}
					JsonValue args;
					std::string aerr;
					const std::string argText = fn.get( "arguments" ).asString();
					if( !JsonParse( argText, args, aerr ) || !args.isObject() ) {
						// RECORD-OR-REFUSE: a tool_call whose arguments did not
						// parse as a JSON object must not be recorded and then
						// executed with fabricated empty args -- refuse the
						// WHOLE response instead of silently degrading to "{}".
						out.step = MakeProviderError( ChatErrorKind::Provider,
							"openai tool_call \"" + c.name + "\" carries malformed arguments JSON -- "
							"refusing the turn (executing it would fabricate empty args)" );
						return out;
					}
					c.argsJson = JsonSerialize( args );
					calls.push_back( c );
				}
			}

			// EVERY exit from here on goes through AttachReasoning: the
			// disposition below REPLACES out.step wholesale on a refusal
			// (MakeProviderError builds a fresh ChatStepResult), so the
			// `reasoning` / `reasoning_content` text captured above has to be
			// attached AFTER that assignment or it is dropped.  SIBLING of the
			// Gemini, Anthropic and Responses sites (same bug pattern, same
			// fix), and the likeliest of the four to fire in practice: a local
			// reasoning model that emits `reasoning` with EMPTY content lands
			// in the blank-turn refusal below, where its reasoning is the only
			// text the turn produced.  The STRUCTURAL refusals above (a
			// non-function / id-less / duplicate-id / malformed-arguments
			// tool_call, a spoofed role) deliberately stay OUTSIDE this -- see
			// the Anthropic site for the full rationale.
			const auto AttachReasoning = [&]() -> ChatParsedResponse& {
				out.reasoningText = reasoningText;
				out.step.reasoningText = reasoningText;
				return out;
			};

			const std::string finishReason = choice.get( "finish_reason" ).asString();
			if( finishReason == "tool_calls" ) {
				if( calls.empty() ) {
					out.step = MakeProviderError( ChatErrorKind::Provider,
						"openai stopped with finish_reason \"tool_calls\" but no tool_calls were present" );
					return AttachReasoning();
				}
				out.step.kind = ChatStepResult::Kind::ToolCalls;
				out.step.toolCalls = calls;
			}
			else if( !calls.empty() ) {
				out.step = MakeProviderError( ChatErrorKind::Provider,
					"openai response carries tool_calls under finish_reason \"" + finishReason +
					"\" -- refusing the turn (its calls would be recorded but never answerable)" );
				return AttachReasoning();
			}
			else if( finishReason == "stop" ) {
				// A "stop" turn with no tool_calls is degenerate when it
				// carries no readable text -- it would otherwise emit a silent
				// blank (or whitespace-only) assistant bubble.  `text` is the
				// fully extracted content (JsonObjectContentToText already
				// handles the string, null, and content-ARRAY shapes), so a
				// single blank test on it catches every degenerate shape
				// uniformly: absent/null/empty-string content (documented
				// OpenAI), a whitespace-only string, AND an array that is empty
				// or has no "text"-typed parts (a non-conformant
				// OpenAI-compatible proxy -- content is string|null in the real
				// schema, but a proxy can send anything).  Whitespace-only is
				// refused deliberately, matching the user-side IsBlank policy:
				// a blank bubble is never a useful answer and a clean refusal
				// lets the caller retry.  When OpenAI's structured-refusal
				// field (message.refusal, a string) is present, surface ITS
				// text so the user sees why.
				if( ChatContentIsBlank( text ) ) {
					const JsonValue* refusal = msg.find( "refusal" );
					if( refusal && refusal->isString() && !refusal->asString().empty() ) {
						out.step = MakeProviderError( ChatErrorKind::Refusal,
							"openai declined this request: " + refusal->asString() );
					}
					else {
						out.step = MakeProviderError( ChatErrorKind::Provider,
							"openai ended the turn with no content -- refusing the degenerate turn" );
					}
					return AttachReasoning();
				}
				out.step.kind = ChatStepResult::Kind::FinalText;
				out.step.finalText = text;
			}
			else if( finishReason == "length" ) {
				out.step = MakeProviderError( ChatErrorKind::MaxTokens,
					"openai: the response hit the output-token cap (finish_reason length) -- "
					"the truncated reply was discarded; try a narrower request" );
				return AttachReasoning();
			}
			else if( finishReason == "content_filter" ) {
				out.step = MakeProviderError( ChatErrorKind::Refusal,
					"openai: the provider declined this request (finish_reason content_filter)" );
				return AttachReasoning();
			}
			else {
				out.step = MakeProviderError( ChatErrorKind::Provider,
					"openai stopped with finish_reason \"" + finishReason + "\"" );
				return AttachReasoning();
			}
			out.assistantDisplayText = text;
			out.step.assistantDisplayText = text;
			AttachReasoning();

			std::size_t cb = 0, ce = 0, eb = 0, ee = 0, mb = 0, me = 0;
			if( RawObjectMember( rawBody, 0, "choices", cb, ce ) &&
			    RawArrayElement( rawBody, cb, 0, eb, ee ) &&
			    RawObjectMember( rawBody, eb, "message", mb, me ) ) {
				out.assistantEntryJson = rawBody.substr( mb, me - mb );
			}
			if( out.assistantEntryJson.empty() ) {
				JsonValue fallback = JsonValue::MakeObject();
				fallback.set( "role", JsonValue::MakeString( "assistant" ) );
				fallback.set( "content", msg.get( "content" ) );
				if( toolCalls.isArray() ) fallback.set( "tool_calls", toolCalls );
				out.assistantEntryJson = JsonSerialize( fallback );
			}
			return out;
		}

		ChatUsage OpenAIChatCodec::ParseUsage( const std::string& rawBody ) const
		{
			ChatUsage u;
			JsonValue root;
			std::string perr;
			if( !JsonParse( rawBody, root, perr ) || !root.isObject() ) return u;
			const JsonValue& usage = root.get( "usage" );
			if( !usage.isObject() ) return u;

			// PROVIDER DISPOSITION (the ONE decision this function makes about
			// reasoning tokens).  Several providers share this codec and they
			// DISAGREE about whether reasoning_tokens is inside their output
			// counter, so the question is answered PER PROVIDER -- from that
			// provider's recorded evidence -- and not per response from the
			// body's arithmetic:
			//   * "openai"  INCLUSIVE.  Documented as a SUBSET of
			//     completion_tokens / output_tokens, and confirmed on 273
			//     recorded Responses blocks (204 with non-zero reasoning):
			//         total_tokens == input_tokens + output_tokens   273/273
			//         output_tokens >  reasoning_tokens              204/204
			//     A separate summand would have broken the first identity on
			//     every one of the 204 reasoning turns.
			//   * "xai"     SEPARATE SUMMAND.  Measured over the 733 recorded
			//     grok-4.5 blocks with non-zero reasoning (evals/runs/**):
			//         total == prompt + completion + reasoning       733/733
			//         total == prompt + completion                     0/733
			//         completion_tokens < reasoning_tokens           356/733
			//     the last line is decisive on its own: a subset can never
			//     exceed the set that contains it, so grok's completion_tokens
			//     is the VISIBLE output only (e.g. completion 51 vs reasoning
			//     362 -- the same ~72 %-class undercount as Gemini's).
			//   * "local"   INCLUSIVE by default.  No recorded local block
			//     reports the field at all (0 of 3,524 carry
			//     completion_tokens_details), so there is no evidence to decide
			//     on; the OpenAI-compatible contract such a server claims to
			//     implement documents inclusion, and assuming inclusion FAILS
			//     SAFE -- it under-reports rather than double counting, and a
			//     self-contradictory body is caught by EnforceUsageInvariant.
			//
			// WHY NOT decide per response from the body's arithmetic (the shape
			// this replaced): the evidence above is per-provider, and the codec
			// already knows the provider, so deriving the same answer from
			// arithmetic buys nothing the data supports while introducing three
			// failure modes of its own.  (1) It FLIPS between turns of ONE
			// conversation: with identical (prompt, completion, reasoning) =
			// (7183, 43, 32) the old code folded when total_tokens was present
			// and did not when it was absent, stringified by a gateway, or
			// unaccompanied by prompt_tokens -- and the trajectory recorder
			// SUMS outputTokens across turns, so a single run silently mixed
			// folded and unfolded turns.  Streaming without
			// stream_options.include_usage produces exactly that.  (2) The
			// three-way identity is NOT a proof of exclusion: it only says the
			// total exceeds prompt+completion by exactly `reasoning`, which any
			// other summand of equal size also satisfies -- e.g.
			// {prompt 100, completion 50, total 180, reasoning 30, audio 30}
			// double counted to 80 where the billed generation was 50.  (3) The
			// `completion < reasoning` size test was unreachable in practice
			// and its "proof" was untested.  A per-provider switch dissolves
			// all three.
			const bool reasoningIsSeparateSummand = ( mConfig.providerName == "xai" );

			if( usage.find( "input_tokens" ) || usage.find( "output_tokens" ) ) {
				// ---- Responses-API shape -------------------------------------
				u.inputTokens  = ReadTokenCount( usage, "input_tokens" );
				u.outputTokens = ReadTokenCount( usage, "output_tokens" );
				u.cacheReadInputTokens =
					ReadTokenCount( usage.get( "input_tokens_details" ), "cached_tokens" );
				u.reasoningOutputTokens =
					ReadTokenCount( usage.get( "output_tokens_details" ), "reasoning_tokens" );
				// The disposition is the PROVIDER's, not the shape's: a
				// provider whose counter is a separate summand stays that way
				// whichever wire shape it answers in, and -- more to the point
				// -- a "local" gateway that happens to answer in Responses
				// shape must NOT inherit OpenAI's disposition by accident.
				if( reasoningIsSeparateSummand ) FoldReasoningIntoOutput( u );
				EnforceUsageInvariant( u );
				return u;
			}

			// ---- Chat-Completions shape (OpenAI, xAI, Ollama) ----------------
			u.inputTokens  = ReadTokenCount( usage, "prompt_tokens" );
			u.outputTokens = ReadTokenCount( usage, "completion_tokens" );
			u.cacheReadInputTokens =
				ReadTokenCount( usage.get( "prompt_tokens_details" ), "cached_tokens" );
			u.reasoningOutputTokens =
				ReadTokenCount( usage.get( "completion_tokens_details" ), "reasoning_tokens" );
			if( reasoningIsSeparateSummand ) FoldReasoningIntoOutput( u );
			EnforceUsageInvariant( u );
			return u;
		}

		std::size_t OpenAIChatCodec::ToolsWireBytes() const
		{
			return ( mConfig.useResponsesApi ? OpenAIResponsesToolsJson() : OpenAIToolsJson() ).size();
		}

		//======================================================================
		// Eval-harness E1: provider-neutral tool-definition fingerprint.
		//======================================================================
		std::string ChatToolDefsFingerprint()
		{
			std::string out;
			for( std::size_t i = 0; i < kToolDefCount; ++i ) {
				out += kToolDefs[i].name;
				out += '\n';
				out += kToolDefs[i].description;
				out += '\n';
				if( kToolDefs[i].schemaJson ) out += kToolDefs[i].schemaJson;
				out += '\n';
			}
			return out;
		}
	}
}
