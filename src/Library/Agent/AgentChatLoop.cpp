//////////////////////////////////////////////////////////////////////
//
//  AgentChatLoop.cpp - the provider-agnostic, sans-IO chat-loop core
//    (see AgentChatLoop.h for the contract).
//
//  NO LOGGING anywhere in this file: request/response bodies may embed
//  scene content, and the API key must never reach a log.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "AgentChatLoop.h"

#include "Json.h"

#include <algorithm> // std::min -- clamping imageContentBytes decrements (context-compaction P1-1 fix)
#include <cstdlib>   // getenv -- ONLY for RISE_LOCAL_LLM_BASE_URL (config, not a credential; see MakeCodec)
#include <cstring>   // strlen -- ASCII case-insensitive substring match (multimodal-400 detection)
#include <mutex>     // process-wide reasoning-effort capability cache (see ReasoningEffortNoneAlreadyKnown)
#include <set>       // ditto

namespace RISE
{
	namespace Agent
	{
		namespace
		{
			//! The static co-editing system prompt (rule 7): the user and
			//! the agent co-edit ONE live scene through the tools
			//! AgentChatCodecs.cpp's kToolDefs declares.  No count here --
			//! this text is MODEL-FACING, and a stale count in it is a lie
			//! told to the model on every single turn.
			const char* const kSystemPrompt =
				"You are a scene-editing agent embedded in the RISE renderer. You and "
				"the user CO-EDIT one live scene: the user sees the same viewport and "
				"may edit concurrently, so never assume the document is unchanged -- "
				"always work from a fresh read_document.\n"
				"\n"
				"Workflow for every edit:\n"
				"1. read_document to see the scene and its headVersion (read_schema "
				"when unsure about a chunk or parameter; consult read_skill before "
				"scene-authoring tasks -- the skills carry the conventions that make "
				"scenes render correctly on the first try). When you need MORE THAN "
				"ONE chunk kind, ask for them ALL IN ONE read_schema call: "
				"{keywords:[\"sdf_geometry\",\"pbr_metallic_roughness_material\","
				"\"uniformcolor_painter\",\"omni_light\",\"standard_object\"]} returns "
				"every one of those schemas in that single result, positionally "
				"aligned with what you asked for, at most 24 per call. Batch the "
				"kinds you have DECIDED to use -- one call per chunk kind is "
				"wasteful, but a padded batch is just a bigger bill. Use "
				"{category:\"material\"|"
				"\"geometry\"|\"painter\"|...} only when you do not yet know which chunk kinds "
				"exist: it is a cheap keyword list. Do ALL the category listings you "
				"need FIRST, then ONE keywords batch for everything you picked across "
				"all of them -- a batch per category is most of the waste back. "
				"A bare read_schema (whole grammar) is expensive and rarely "
				"needed.\n"
				"2. propose_patch (or insert_chunk / remove_chunk) with the "
				"headVersion you just read as baseHeadVersion. On status=conflict, "
				"re-read and re-propose; on "
				"retriable=true, retry the same patch after a moment. "
				"status=diagnosed means the edit WAS applied but the re-derive "
				"produced diagnostics -- read them and fix the reported problem; "
				"do not blindly re-propose the same patch.\n"
				"3. Verify by CHANGE KIND. PARAM/STRUCTURAL edits (value changes, "
				"bindings, insert/remove of non-visual chunks) are confirmed by "
				"the apply response's status and bumped headVersion alone -- "
				"if the edit was structural (insert_chunk/remove_chunk), also "
				"call validate WITH NO ARGUMENTS, which checks the CURRENT "
				"scene; NEVER re-send the document you just edited (validate's "
				"`text` argument is only for a candidate you have NOT applied, "
				"and echoing a whole scene back costs thousands of output "
				"tokens for nothing) -- then declare done from a clean "
				"apply + clean validate; do NOT render just to confirm a "
				"parameter took. When the user SPECIFIES the exact target (a "
				"specific colour value, a named binding, a given chunk to "
				"add/remove), that IS a param/structural edit even if it changes "
				"appearance -- apply + validate suffices; a look is for judging "
				"appearance you must EVALUATE (does it read as shiny/right), "
				"never for confirming a specified value landed. "
				"The dividing line is WHAT you are checking, not whether an "
				"apply succeeded. A VALUE landing is a param confirmation: "
				"the apply response already told you, so a render adds "
				"nothing. But WHERE something sits, WHAT SHAPE it reads as, "
				"and HOW the frame composes are NOT param confirmations -- "
				"they are visual judgements the apply response cannot answer "
				"at all, because a patch reports clean success for "
				"coordinates that leave two objects a foot apart. So the two "
				"rules never collide: never render to re-read a number you "
				"set; always look to judge a placement, a silhouette, or a "
				"composition. "
				"VISUALLY-CONSEQUENTIAL judgement calls (material "
				"appearance, lighting, geometry placement/shape, camera "
				"framing), or whenever the user asks how it looks, need ONE "
				"cheap look first: render width/height 128-192 (or "
				"quality:\"draft\" for a geometry/placement-only check -- it "
				"uses a fixed studio-preview shader, capped at 4 samples, that "
				"IGNORES materials and lighting, so NEVER judge those from it; "
				"check `renderMode`, not `integrator`, to confirm which "
				"pipeline ran) with imageMaxEdge ~192, which returns the PNG "
				"in that same call -- do not follow it with read_image. Use render's "
				"`camera` override to check 2-3 angles WITHOUT touching the "
				"actual camera -- ephemeral, restored automatically, far "
				"cheaper than a full render. Reserve a full-size, full-sample, "
				"quality:\"production\" render (the default) for the FINAL "
				"check on a genuinely visual task and any judgement of "
				"materials/lighting/exposure/colour. To find WHICH object is "
				"at one spot (e.g. before moving or recoloring \"the mug\"), "
				"use query_object_at {x,y} instead of render mode:\"objectmap\" "
				"-- cheaper to parse, returns the name directly (hit:false, "
				"not an error, when empty); combine with `camera` to aim "
				"first.\n"
				"4. BUILD CADENCE: when you are building a scene from "
				"scratch, look after EACH OBJECT GROUP you place -- not once "
				"at the end. Four renders across a six-object scene is too "
				"few. One small look (width/height ~192, or quality:\"draft\") "
				"per object or pair of objects is cheap, and it is the only "
				"thing that keeps placement honest; the expensive failure is "
				"discovering at chunk sixty that the hero has been floating "
				"since chunk twelve. Blind construction, not over-rendering, "
				"is the failure mode that loses builds -- batching chunks "
				"into one insert_chunks call is where you save round-trips, "
				"NOT skipping the looks between groups.\n"
				"5. RELATIONAL CONSTRAINTS MUST BE SEEN. When the request "
				"contains a relational or compositional constraint -- one "
				"thing resting against / behind / in front of / overlapping "
				"another, something breaking a horizon or a leading line, "
				"\"reads as X from this angle\" -- verify it against an "
				"actual rendered IMAGE before calling the task done. "
				"Reasoning about coordinates is NOT verification: two "
				"objects with entirely plausible numbers routinely fail to "
				"touch, interpenetrate, or occlude the wrong way. Render "
				"with imageMaxEdge and LOOK; use query_object_at {x,y} to "
				"settle \"is object A actually at this screen position\" for "
				"the price of one cheap call. If the check fails, patch the "
				"position and look again -- do not re-derive the answer from "
				"the numbers.\n"
				"\n"
				// CAPABILITY SCOPE (Model-B F5 slice S2: insert_chunk /
				// remove_chunk shipped -- entity add/remove is real now;
				// round 2: declaration positioning + the full-derivability
				// gate make the rename recipe safe, and the unnamed one-way
				// door is disclosed; round 3: the one-way door is film +
				// rasterizers ONLY -- the SOLE camera IS removable via the
				// kind="camera" positional fallback, so the camera-SWAP
				// recipe (remove FIRST, then insert) is taught, plus the
				// retarget-refused remove+reinsert escape).
				"You can change PARAMETERS of existing entities via propose_patch. "
				"To patch a chunk that has NO name -- the sole camera, the film, "
				"the rasterizer -- leave `target` EMPTY and pass `kind`: "
				"{target:\"\", kind:\"camera\", param:\"location\", value:\"0 1 4\"} "
				"moves an unnamed camera. Do NOT pass the chunk keyword as the "
				"target (target:\"pinhole_camera\" does not resolve), and do NOT "
				"remove-and-reinsert a camera just to move it. "
				"ADD a new entity with insert_chunk (exactly ONE complete "
				"`keyword { ... }` chunk per call, braces on their own lines; "
				"declaration chunks -- painters, materials, geometry, shaders, "
				"media -- are positioned before the objects that consume them "
				"automatically), and DELETE an entity with remove_chunk (refused "
				"while another chunk still references the target -- retarget or "
				"remove the consumers first). Any edit that would leave the "
				"document unable to derive in order is refused cleanly with the "
				"head unchanged. Honest limits: whole-chunk granularity only -- "
				"no rename verb (the safe recipe: insert_chunk the renamed "
				"entity, retarget its consumers via propose_patch, then "
				"remove_chunk the old one), no reordering, and unnamed film and "
				"rasterizer chunks cannot be removed once inserted -- insert "
				"them deliberately. The SOLE camera (even unnamed) IS removable: "
				"remove_chunk with kind=\"camera\" resolves it by position. To "
				"SWAP cameras, REMOVE the old camera FIRST, THEN insert the "
				"replacement -- insert-first creates two cameras and strands "
				"the old unnamed one (the fallback requires exactly one; a "
				"NAMED second camera stays removable by name). If a retarget is "
				"refused because the new entity sits later in the document, "
				"remove_chunk the consumer and re-insert it (it will be "
				"appended after the entity it references). For a BIG addition, "
				"compose the full candidate document and validate it FIRST, then "
				"insert chunk by chunk. ONCE, on the WHOLE candidate -- not once "
				"per chunk as you build it up. Every validate{text} re-sends the "
				"entire document, so validating an eight-chunk scene in eight "
				"steps costs eight full copies to learn what one call would have "
				"told you. And a validate that returns an EMPTY diagnostics array "
				"has told you everything it can: go insert. Re-validating a "
				"superset of text you just validated clean almost never reports "
				"anything new -- if you find yourself sending a third clean "
				"candidate in a row, stop and start inserting. That validate-first "
				"recipe is for BIG MULTI-CHUNK additions only -- for one or two "
				"chunks, insert DIRECTLY: the apply response is itself the "
				"validation, a rejection is informative and cheap, and "
				"pre-validating variants of a small insert wastes rounds.\n"
				"\n"
				"Keep responses concise. After acting, report plainly what changed "
				"(entity, parameter, old vs new value when known) and what you "
				"observed in the render.";

			std::unique_ptr<IChatProviderCodec> MakeCodec( ChatProvider provider )
			{
				if( provider == ChatProvider::Gemini )
					return std::unique_ptr<IChatProviderCodec>( new GeminiChatCodec() );
				if( provider == ChatProvider::OpenAI )
					return std::unique_ptr<IChatProviderCodec>( new OpenAIChatCodec() );
				if( provider == ChatProvider::XAI ) {
					// xAI (Grok): OpenAI-Chat-Completions-compatible endpoint,
					// same Bearer auth (env convention XAI_API_KEY).  Default
					// model id is the literal "grok-4.5"; xAI's usage block
					// carries the OpenAI fields plus extras (e.g.
					// cost_in_usd_ticks) that ParseUsage harmlessly ignores.
					OpenAIChatCodec::Config cfg;
					cfg.providerName   = "xai";
					cfg.baseUrl        = "https://api.x.ai/v1/chat/completions";
					cfg.defaultModelId = "grok-4.5";
					cfg.requiresAuth   = true;
					return std::unique_ptr<IChatProviderCodec>( new OpenAIChatCodec( cfg ) );
				}
				if( provider == ChatProvider::Local ) {
					// A local OpenAI-compatible server (Ollama et al.).  The
					// base URL is read ONCE here, at codec construction, from
					// RISE_LOCAL_LLM_BASE_URL (falling back to Ollama's default
					// loopback endpoint).  CREDENTIAL INVARIANT NOTE: the
					// Library layer deliberately reads NO api key from the
					// environment (keys arrive as parameters) -- but a BASE URL
					// is CONFIG, not a secret, so this one getenv is acceptable.
					// It is deliberately NOT in AgentEvalRunner (which stays
					// wholly env-read-free).  Changing the env var takes effect
					// only on the NEXT provider selection (MakeCodec re-runs on
					// SetProvider), not mid-conversation.  The IP LITERAL
					// 127.0.0.1 (not "localhost") is intentional: macOS ATS
					// exempts IP literals from its cleartext-HTTP block.
					// DESIGN SEAM: this getenv bypasses RunEvalMatrix's
					// envLookup injection (deliberate -- envLookup exists to
					// inject CREDENTIALS; this is config), so a matrix run
					// resolves the local base URL from the REAL process
					// environment.  If a future test needs to mock/override
					// it, thread the URL through AgentEvalRunOptions instead
					// of teaching envLookup about config reads.
					OpenAIChatCodec::Config cfg;
					const char* envUrl = std::getenv( "RISE_LOCAL_LLM_BASE_URL" );
					cfg.providerName   = "local";
					cfg.baseUrl        = ( envUrl && envUrl[0] != '\0' )
						? std::string( envUrl )
						: std::string( "http://127.0.0.1:11434/v1/chat/completions" );
					// Default model "opencoder" (user decision 2026-07-16:
					// a local open-weights coder as the out-of-box default;
					// earlier default was "qwen3:32b").  requiresAuth=false ->
					// keyless local servers emit NO Authorization header,
					// while a server started with --api-key still gets a
					// Bearer header when a key is supplied.
					cfg.defaultModelId = "opencoder";
					cfg.requiresAuth   = false;
					// Local inference legitimately exceeds the hosted-provider
					// 300s budget: a cold model swap loads 17-43GB before the
					// first token, and a long 70B generation can run minutes
					// past that.  900s was observed sufficient where 300s
					// produced NSURLErrorDomain -1001 ("The request timed
					// out.") transport failures in the local-model shootout.
					cfg.requestTimeoutSeconds = 900;
					// EVAL SIGNAL, deliberately NOT worked around (local-model
					// shootout, 2026-07-12): llama3.3:70b-instruct-q4_K_M via
					// Ollama's OpenAI endpoint frequently answers a tool result
					// (most often right after propose_patch's status=conflict)
					// with finish_reason "stop" and NO tool_calls entry at all
					// -- instead it writes the intended call as pseudo-JSON
					// text in `content`, e.g. `{"name": "read_document",
					// "parameters": {}}`.  That is syntactically valid JSON, so
					// OpenAIChatCodec::ParseResponse's blank-content check does
					// NOT fire; it is accepted as an honest FinalText turn (see
					// ParseResponse's "stop" branch) and the tool is never
					// actually invoked.  10 of 12 llama3.3 shootout runs across
					// conflict_retry/material_add_and_bind/remove_object/
					// reserved_name_recovery ended this way (evals/runs/
					// local_shootout/*llama3.3*).  A SEPARATE, rarer failure in
					// the same runs: llama3.3 sent insert_chunk's body under
					// the key 'chunk' instead of the schema-required
					// 'chunkText' (kToolDefs in AgentChatCodecs.cpp already
					// spells out "chunkText must be EXACTLY ONE `keyword {
					// ... }` block" in the tool description AND declares it
					// `required` in the JSON schema -- the model ignored both).
					//
					// CORRECTION (2026-07-25): this comment used to add "no
					// OTHER provider in the shootout (gemini-3.5-flash,
					// qwen3:32b, qwen3.6:27b, qwen3-coder:30b) exhibits either
					// pattern".  That was true of the 12-run-per-model
					// local_shootout it was written from, and is FALSIFIED by
					// the larger full_baseline sweep (15 scenarios x 3 repeats
					// = 45 runs per model, evals/runs/full_baseline/).  There,
					// qwen3-coder:30b leaks tool-call intent into `content`
					// too -- in a DIFFERENT, Hermes-style syntax rather than
					// llama3.3's pseudo-JSON:
					//     <function=read_document>
					//     </function>
					//     </tool_call>
					// again with tool_calls:null and finish_reason "stop".
					// Measured incidence of a prose tool call in a
					// tool_calls-free message, per model, over full_baseline's
					// 45 runs each (the same detector reproduces the 10-of-12
					// llama3.3 figure above on local_shootout, so the two run
					// sets are directly comparable):
					//     llama3.3:70b-instruct-q4_K_M  14/45  (pseudo-JSON)
					//     qwen3-coder:30b                6/45  (<function=...>)
					//     qwen3-coder-next               0/45
					//     qwen3:32b                      0/45
					//     qwen3.6:27b                    0/45
					//     glm-4.7-flash                  0/45
					// and 0/45 for every hosted model in that sweep
					// (claude-opus-4-8, gpt-5.6-terra, gemini-3.5-flash,
					// grok-4.5).
					// Five of qwen3-coder:30b's six are FATAL -- the prose call
					// is the model's FIRST reply, so the run ends at
					// llmCalls=1, toolCalls=0, terminalStatus="final_text"
					// (conflict_retry r2 + r3, param_edit r2,
					// reserved_name_clarify r3, lighting_luma_band r1).  The
					// sixth (multi_turn_edit r1) leaked one prose call and then
					// carried on to make 11 real tool calls in the same run.
					//
					// So the behaviour is INTERMITTENT, not a stable per-model
					// property: the same model, same scenario, same harness
					// build produces a real tool_calls array on one repeat and
					// prose on the next (conflict_retry r1 succeeded where r2
					// and r3 died).  Treat any "model X does/does not do this"
					// claim as a sample-size statement with a run count
					// attached, never as a capability verdict -- llama3.3 shows
					// the same instability from the other side, leaking in 10
					// of 12 local_shootout runs (83 %) but only 14 of 45
					// full_baseline runs (31 %).
					//
					// Neither pattern is a codec/protocol bug.  This is a
					// MODEL-capability signal the eval is measuring -- do NOT
					// add tolerant handling here (sniffing `content` for an
					// inline pseudo-tool-call in EITHER syntax, or aliasing
					// 'chunk' -> 'chunkText' in AgentRpc.cpp's insert_chunk
					// handler) to paper over it; that would silently raise the
					// affected models' scores by doing the self-correction FOR
					// them.  Detection/reporting of these turns belongs in the
					// eval-analysis layer, not in the codec.  The one
					// legitimate hardening move already landed instead:
					// AgentRpc.cpp's insert_chunk missing-'chunkText' error
					// now also NAMES whatever key WAS present (e.g. "got
					// 'chunk' instead"), so a model that reads its own tool
					// error has enough to self-correct in one round -- see
					// DescribeOtherParamKeys's doc in AgentRpc.cpp.
					return std::unique_ptr<IChatProviderCodec>( new OpenAIChatCodec( cfg ) );
				}
				return std::unique_ptr<IChatProviderCodec>( new AnthropicChatCodec() );
			}

			//! REASONING-EFFORT CAPABILITY MEMOIZATION (process-wide, cross-
			//! SESSION): the retry in HandleResponse (mForceReasoningEffortNone)
			//! learns the tools/reasoning_effort conflict the hard way -- one
			//! wasted 400 round-trip -- and that knowledge is otherwise scoped
			//! to a single AgentChatLoop instance (cleared by Reset()/
			//! SetProvider, never shared).  A host that drives MANY sessions
			//! against the SAME (provider, model) in one process (the eval
			//! harness's RunEvalMatrix: repeats x scenarios all reusing one
			//! gpt-5.6-terra-class model) pays that wasted round-trip on EVERY
			//! session, even though the very first one already proved the
			//! answer.  For an attachment-bearing session (reference-image
			//! prompts) that wasted round-trip re-sends the FULL image payload
			//! a second time -- real token cost against the provider's rate
			//! limit, not just latency.  This tiny process-wide cache lets
			//! every session AFTER the first one that hits the 400 start
			//! pre-armed, cutting the wasted request instead of re-discovering
			//! it per session.  Deliberately NOT a hardcoded model-name list --
			//! entries are learned ONLY from a live 400, exactly like the
			//! per-session retry it complements; a model that never trips the
			//! 400 never appears here, and one that does is only ever added
			//! AFTER the SAME live detection AgentChatLoop::HandleResponse
			//! already performs.  Mutex-guarded: AgentChatLoop itself is
			//! single-threaded, but this cache is shared by every instance
			//! that has ever existed in the process, including ones on other
			//! threads (e.g. a GUI chat panel and a headless eval run in the
			//! same process).
			std::mutex& ReasoningEffortCacheMutex()
			{
				static std::mutex m;
				return m;
			}
			std::set<std::string>& ReasoningEffortCacheSet()
			{
				static std::set<std::string> s;
				return s;
			}
			std::string ReasoningEffortCacheKey( const char* providerName, const std::string& modelId )
			{
				// NUL-separated: neither field can legally contain a NUL, and
				// this avoids any ambiguity a plain concatenation could have
				// between e.g. provider "a" model "bc" and provider "ab"
				// model "c".
				std::string key( providerName ? providerName : "" );
				key.push_back( '\0' );
				key += modelId;
				return key;
			}
			bool ReasoningEffortNoneAlreadyKnown( const char* providerName, const std::string& modelId )
			{
				std::lock_guard<std::mutex> lock( ReasoningEffortCacheMutex() );
				return ReasoningEffortCacheSet().count( ReasoningEffortCacheKey( providerName, modelId ) ) > 0;
			}
			void MarkReasoningEffortNoneKnown( const char* providerName, const std::string& modelId )
			{
				std::lock_guard<std::mutex> lock( ReasoningEffortCacheMutex() );
				ReasoningEffortCacheSet().insert( ReasoningEffortCacheKey( providerName, modelId ) );
			}

			//! SEQUENCING GATE (see the file header): the DOCUMENT-MUTATING
			//! verb allowlist -- shared by AddToolResult's streak accounting
			//! and GateRefusalResponse's gate check, so the two can never
			//! disagree about which verbs count.  The BATCH forms
			//! (insert_chunks / propose_patches / insert_material_scaffold /
			//! insert_geometry_scaffold) count too -- they still edit the
			//! document with no visual observation in between, same
			//! blind-edit risk, and if anything a LARGER one (N edits land
			//! per call, so a model that batches would otherwise never
			//! accrue the streak at all and the gate would silently never
			//! fire for exactly the models making the biggest unobserved
			//! edits).
			bool IsMutatingToolName( const std::string& v )
			{
				return v == "insert_chunk" || v == "insert_chunks" ||
				       v == "insert_material_scaffold" ||
				       v == "insert_geometry_scaffold" ||
				       v == "propose_patch" || v == "propose_patches" ||
				       v == "remove_chunk";
			}

			//! The VISUAL-OBSERVE verb allowlist -- resets the blind-edit
			//! streak (see IsMutatingToolName's doc).  compare_to_reference
			//! belongs here for the same reason render/read_image/
			//! read_viewport do: AgentRpc.cpp documents it as a PURE READ
			//! that RENDERS (a visual side-by-side comparison), and the file
			//! header's IMAGE RETENTION rule already treats it as an
			//! image-bearing result alongside those three -- it was simply
			//! missing from this specific allowlist.  query_object_at is
			//! also a look even though it is not image-bearing itself: it
			//! answers from a RENDERED frame (see AgentSession.cpp's
			//! EphemeralRenderCacheGuard), so querying it requires the scene
			//! to have actually been rendered first.  ask_user is
			//! DELIBERATELY neither this nor a mutation: it neither mutates
			//! the document nor observes the rendered result, so (like
			//! read_document/read_schema/read_skill/validate) it leaves the
			//! streak UNCHANGED -- pausing to ask a clarifying question is
			//! not progress toward the blind-edit failure mode this gate
			//! guards against, so it should neither reset nor grow it.
			bool IsVisualObserveToolName( const std::string& v )
			{
				return v == "render" || v == "read_image" || v == "read_viewport" ||
				       v == "query_object_at" || v == "compare_to_reference";
			}
		}

		const char* AgentChatLoop::SystemPrompt()
		{
			return kSystemPrompt;
		}

		void AgentChatLoop::SetSkillIndex( const std::string& indexText )
		{
			// Provider-neutral config (like the provider/model selection):
			// stored verbatim; BuildRequest composes the skills section onto
			// the base prompt when non-empty.  Survives Reset()/SetProvider().
			mSkillIndexText = indexText;
		}

		void AgentChatLoop::SetSystemPromptOverride( const std::string& prompt )
		{
			// See the header doc: verbatim replacement for auxiliary
			// single-purpose loops, NOT cleared by Reset()/SetProvider().
			mSystemPromptOverride = prompt;
		}

		void AgentChatLoop::SetContextBudget( std::size_t highWaterTokens, std::size_t lowWaterTokens )
		{
			// Provider-neutral config (like SetSkillIndex above): stored
			// verbatim, no clamping.  Survives Reset()/SetProvider().
			mContextBudgetHighTokens = highWaterTokens;
			mContextBudgetLowTokens = lowWaterTokens;
		}

		std::size_t AgentChatLoop::EstimateContextTokens() const
		{
			// Context-compaction slice S1: the estimator slice S2's
			// CompactTranscript triggers on (BuildRequest calls it every
			// round; both GUI drivers install a budget via
			// SetContextBudget / kDefaultContextBudget*, so this is a
			// PRODUCTION trigger, not observability).  Deterministic, provider-
			// aware, IMAGE-DISCOUNTED text-proxy estimator:
			//
			//   text tokens  ~= (system prompt + tool defs + PER-ENTRY
			//                    (rawJson bytes - tracked live image
			//                    payload bytes)) / kCharsPerToken
			//   image tokens ~= (live image count) * kTokensPerImage
			//
			// IMAGES ARE DISCOUNTED DELIBERATELY: a base64 image blob bills
			// by pixel area on every real provider, NOT by encoded byte
			// length -- counting its (huge) base64 bytes at the same
			// chars-per-token rate as text would wildly overcount and
			// trigger compaction on image-heavy turns that are actually
			// cheap in text terms.  Live images are already bounded (at
			// most one live render + kMaxLiveUserImages user images -- see
			// the IMAGE RETENTION / USER IMAGE RETENTION rules in the file
			// header), so each is charged a flat per-image token cost
			// instead.  What's subtracted from the text proxy is NOT a flat
			// per-entry charge but the entry's OWN tracked
			// imageContentBytes -- an image-bearing ToolResults entry can
			// co-pack a large non-image tool result (e.g. a read_document
			// alongside a read_image) in the SAME PackToolResults call, and
			// a user message can co-pack a long caption alongside an
			// attachment; a flat per-entry text charge silently dropped
			// that co-packed text from the estimate, under-counting in the
			// dangerous direction (compaction failing to trigger).
			// imageContentBytes is maintained wherever carriesLiveImage /
			// liveUserImageCount change (see ChatTranscriptEntry's doc).
			const std::size_t kCharsPerToken = 4;          // standard rough text proxy
			const std::size_t kTokensPerImage = 1600;      // image billed by area, not base64 len

			std::size_t chars = ComposeSystemPrompt().size();
			if( mCodec ) chars += mCodec->ToolsWireBytes();

			std::size_t imageTokens = 0;
			for( std::size_t i = 0; i < mTranscript.size(); ++i ) {
				const ChatTranscriptEntry& e = mTranscript[i];
				const std::size_t imgs = ( e.carriesLiveImage ? 1u : 0u ) +
				                          static_cast<std::size_t>( e.liveUserImageCount );
				imageTokens += imgs * kTokensPerImage;   // 0 for non-image entries
				chars += ( e.rawJson.size() > e.imageContentBytes )
				         ? ( e.rawJson.size() - e.imageContentBytes ) : 0;
			}
			return chars / kCharsPerToken + imageTokens;
		}

		AgentChatLoop::AgentChatLoop() :
			mCodec( MakeCodec( ChatProvider::OpenAI ) ),
			mProvider( ChatProvider::OpenAI ),
			mModelId( mCodec->DefaultModelId() ),
			mContextBudgetHighTokens( 0 ),
			mContextBudgetLowTokens( 0 ),
			mToolRounds( 0 ),
			mSessionEmitted( false ),
			mElideAllImages( false ),
			// REASONING-EFFORT CAPABILITY MEMOIZATION: pre-arm from the
			// process-wide cache in case an EARLIER AgentChatLoop instance
			// in this process already proved this (provider, model) pair
			// needs the override -- see ReasoningEffortNoneAlreadyKnown's
			// doc comment.  A model this process has never seen 400 for
			// (the overwhelmingly common case) finds the cache empty and
			// this is false, byte-identical to the pre-memoization default.
			mForceReasoningEffortNone(
				ReasoningEffortNoneAlreadyKnown( mCodec->ProviderName(), mModelId ) )
		{
		}

		AgentChatLoop::~AgentChatLoop()
		{
		}

		void AgentChatLoop::Reset()
		{
			// Eval-harness E1: a direct Reset closes the trajectory session
			// with a "reset" summary (no-op when SetProvider already closed
			// it as "provider_switch", or when no session is active).
			CloseTrajectorySession( "reset" );
			mTranscript.clear();
			mPendingCalls.clear();
			mPendingResults.clear();
			mToolLineStash.clear();
			mToolRounds = 0;
			// Describes the transcript we just cleared, so it goes with it.
			mCompactedEntryCount = 0;
			mBlindEditStreak = 0;
			mGateRefusedCallIds.clear();
			// Describe the transcript we just cleared, like mCompactedEntryCount.
			mDriverNoteCount = 0;
			mLastDriverNoteText.clear();
			// A fresh session may target a different, image-capable model --
			// the text-only proof does not carry across a Reset/SetProvider.
			mElideAllImages = false;
			// Likewise, a fresh session may target a different model that
			// has no reasoning-vs-tools conflict at all -- but re-arm from
			// the process-wide cache rather than blindly clearing: if THIS
			// process already proved (mProvider, mModelId) needs the
			// override (learned by an earlier session, possibly this same
			// loop instance before this very Reset), a fresh session
			// against the SAME pair should not have to pay the wasted
			// round-trip again.  mProvider/mModelId are already the
			// TARGET pair by the time Reset() runs (SetProvider sets them
			// before calling Reset(); a direct Reset() keeps the current
			// pair) -- see ReasoningEffortNoneAlreadyKnown's doc comment.
			mForceReasoningEffortNone =
				ReasoningEffortNoneAlreadyKnown( mCodec->ProviderName(), mModelId );
		}

		void AgentChatLoop::SetProvider( ChatProvider provider, const std::string& modelId )
		{
			// Eval-harness E1: close the current trajectory session BEFORE
			// the provider swap so the summary belongs to the old provider and
			// the next session record reflects the new one.  Reset() below then
			// finds the session already closed (a no-op "reset").
			CloseTrajectorySession( "provider_switch" );
			// SWITCHING PROVIDER RESETS THE TRANSCRIPT: assistant entries
			// are provider-native raw JSON and cannot cross providers.
			mProvider = provider;
			mCodec = MakeCodec( provider );
			mModelId = modelId.empty() ? std::string( mCodec->DefaultModelId() ) : modelId;

			// Anti-spin backstop follows the provider's COST posture, unless
			// a host pinned its own cap (the eval runner does, right after
			// this call -- and its explicit value must win).  A local round
			// costs only wall time, so the backstop sits far out of the way
			// of long iterative scene builds; a hosted round costs money, so
			// it stays bounded.  Neither is a budget: a host that needs a
			// real spend limit enforces it in its own budget accounting.
			if( !mToolRoundsCapExplicit ) {
				mMaxToolRoundsPerTurn = ( provider == ChatProvider::Local )
					? kMaxToolRoundsPerTurnLocal
					: kMaxToolRoundsPerTurn;
			}

			Reset();
		}

		namespace
		{
			bool IsBlank( const std::string& text )
			{
				for( std::size_t i = 0; i < text.size(); ++i ) {
					const char c = text[i];
					if( c != ' ' && c != '\t' && c != '\n' && c != '\r' ) return false;
				}
				return true;
			}

			//! Case-insensitive substring test (ASCII).
			bool ContainsCI( const std::string& hay, const char* needle )
			{
				const std::size_t nlen = std::strlen( needle );
				if( nlen == 0 ) return true;
				if( hay.size() < nlen ) return false;
				for( std::size_t i = 0; i + nlen <= hay.size(); ++i ) {
					std::size_t j = 0;
					for( ; j < nlen; ++j ) {
						char a = hay[i + j];
						char b = needle[j];
						if( a >= 'A' && a <= 'Z' ) a = static_cast<char>( a - 'A' + 'a' );
						if( b >= 'A' && b <= 'Z' ) b = static_cast<char>( b - 'A' + 'a' );
						if( a != b ) break;
					}
					if( j == nlen ) return true;
				}
				return false;
			}

			//! TEXT-ONLY-MODEL IMAGE-REJECTION detection.  A text-only
			//! backend (observed: Ollama's qwen3:32b via the OpenAI-
			//! compatible Local provider) answers the NEXT request after an
			//! image was packed into the conversation with HTTP 400 whose
			//! body reads e.g. "Multimodal data provided, but model does not
			//! support multimodal requests".  The match is deliberately
			//! NARROW -- status must be exactly 400 AND the raw body must
			//! contain BOTH "multimodal" and "not support" (case-
			//! insensitive) -- so an ordinary bad-request 400 (malformed
			//! JSON, unknown model, rate-limit-shaped 400, ...) does NOT
			//! trigger the image-elide retry.  Both tokens are provider-
			//! stable substrings of the observed message; requiring the
			//! pair avoids matching a 400 that merely mentions one word.
			bool IsMultimodalUnsupported400( long httpStatus, const std::string& rawBody )
			{
				if( httpStatus != 400 ) return false;
				return ContainsCI( rawBody, "multimodal" ) &&
				       ContainsCI( rawBody, "not support" );
			}

			//! REASONING-MODEL TOOLS-VS-EFFORT 400 detection.  An OpenAI-
			//! family reasoning model (observed: gpt-5.6-terra, over
			//! /v1/chat/completions) answers a function-tools request with
			//! HTTP 400: "Function tools with reasoning_effort are not
			//! supported for gpt-5.6-terra in /v1/chat/completions. To use
			//! function tools, use /v1/responses or set reasoning_effort to
			//! 'none'." -- the model's server-side default reasoning_effort
			//! is incompatible with tool calling on this endpoint, even
			//! though this codebase never sends a reasoning_effort field
			//! itself.  The match is deliberately NARROW, mirroring
			//! IsMultimodalUnsupported400 -- status must be exactly 400 AND
			//! the raw body must contain BOTH "reasoning_effort" and
			//! "not support" (case-insensitive) -- so an ordinary bad-
			//! request 400 does not trigger the retry.
			bool IsReasoningEffortToolsUnsupported400( long httpStatus, const std::string& rawBody )
			{
				if( httpStatus != 400 ) return false;
				return ContainsCI( rawBody, "reasoning_effort" ) &&
				       ContainsCI( rawBody, "not support" );
			}
		}

		namespace
		{
			// Eval-harness E1: the auth header names stripped from every
			// recorded request (case-insensitive) -- the key must never reach
			// a trajectory line via the header path.
			bool IsAuthHeaderName( const std::string& name )
			{
				std::string lower;
				lower.reserve( name.size() );
				for( std::size_t i = 0; i < name.size(); ++i ) {
					char c = name[i];
					if( c >= 'A' && c <= 'Z' ) c = static_cast<char>( c - 'A' + 'a' );
					lower += c;
				}
				return lower == "authorization" || lower == "x-api-key" ||
				       lower == "x-goog-api-key";
			}

			//! A copy of `req` with every auth header removed by name.
			ChatHttpRequest StripAuthHeaders( const ChatHttpRequest& req )
			{
				ChatHttpRequest out;
				out.url = req.url;
				out.body = req.body;
				for( std::size_t i = 0; i < req.headers.size(); ++i ) {
					if( IsAuthHeaderName( req.headers[i].first ) ) continue;
					out.headers.push_back( req.headers[i] );
				}
				return out;
			}
		}

		void AgentChatLoop::AddUserMessage( const std::string& text )
		{
			AddUserMessage( text, std::vector<ChatAttachment>() );
		}

		void AgentChatLoop::AddUserMessage( const std::string& text,
		                                    const std::vector<ChatAttachment>& attachments )
		{
			// EMPTY / whitespace-only text with NO attachments is a
			// documented NO-OP (see the header): Anthropic hard-400s an
			// empty text block, so recording one would poison every later
			// request.  The caller sent nothing, so nothing is flushed or
			// reset either -- the turn state stays exactly as it was. An
			// attachment-only message (blank text, non-empty attachments)
			// is legitimate -- MakeUserEntry omits the empty text block
			// for that case (see AgentChatCodecs.cpp) -- so the no-op only
			// fires when BOTH are empty.
			if( IsBlank( text ) && attachments.empty() ) return;

			// Eval-harness E1: ensure the session record leads the trajectory.
			EnsureSessionRecordEmitted();

			// Pending tool calls belong to the PREVIOUS assistant turn --
			// flush them ahead of the new user message so the wire order
			// stays assistant(tool_use) -> user(tool_results) -> user(text).
			// Unanswered calls get synthesized error results in the flush.
			FlushPendingToolResults();

			ChatTranscriptEntry entry;
			entry.role = ChatTranscriptEntry::Role::User;
			entry.displayText = text;
			entry.rawJson = mCodec->MakeUserEntry( text, attachments );
			entry.liveUserImageCount = static_cast<int>( attachments.size() );
			// EstimateContextTokens's per-entry text proxy excludes tracked
			// live image payload bytes (images bill by area, not encoded
			// length) -- track the raw base64 payload size here so a long
			// co-packed caption is NOT silently dropped from the estimate.
			for( std::size_t i = 0; i < attachments.size(); ++i )
				entry.imageContentBytes += attachments[i].base64Data.size();
			mTranscript.push_back( entry );

			// Eval-harness E1: record the user turn (text + attachment count).
			if( mRecorder ) {
				TrajectoryUserRecord u;
				u.text = text;
				u.attachments = static_cast<int>( attachments.size() );
				mRecorder->EmitUser( u );
			}

			// USER IMAGE RETENTION (see the file header): elide the oldest
			// live user images across the WHOLE transcript, just enough
			// that the running total never exceeds kMaxLiveUserImages.
			// Walking oldest-first and only ever touching User entries
			// (Assistant/ToolResults entries always carry
			// liveUserImageCount == 0, so this loop never looks at them).
			if( !attachments.empty() ) {
				int totalLive = 0;
				for( std::size_t i = 0; i < mTranscript.size(); ++i )
					totalLive += mTranscript[i].liveUserImageCount;

				int toElide = totalLive - kMaxLiveUserImages;
				for( std::size_t i = 0; toElide > 0 && i < mTranscript.size(); ++i ) {
					ChatTranscriptEntry& e = mTranscript[i];
					if( e.role != ChatTranscriptEntry::Role::User || e.liveUserImageCount <= 0 )
						continue;
					const int elideHere = ( toElide < e.liveUserImageCount ) ? toElide : e.liveUserImageCount;
					const std::size_t beforeBytes = e.rawJson.size();
					e.rawJson = mCodec->RewriteElidedUserImages( e.rawJson, elideHere );
					e.liveUserImageCount -= elideHere;
					toElide -= elideHere;
					// Shrink the tracked live-image payload by the byte
					// delta this rewrite just removed (see
					// EstimateContextTokens); force to 0 once no live
					// images remain so rounding never leaves a residue.
					e.imageContentBytes -= std::min( e.imageContentBytes, beforeBytes - e.rawJson.size() );
					if( e.liveUserImageCount == 0 ) e.imageContentBytes = 0;
					// Eval-harness E1: record the transcript rewrite.
					if( mRecorder ) {
						TrajectoryHistoryEditRecord h;
						h.entryIndex = static_cast<int>( i );
						h.beforeBytes = static_cast<long long>( beforeBytes );
						h.afterBytes = static_cast<long long>( e.rawJson.size() );
						h.reason = "user_image_elision";
						mRecorder->EmitHistoryEdit( h );
					}
				}
			}

			// A new conversation-turn: the tool-round cap starts over, and so
			// does the SEQUENCING GATE's blind-edit streak.  DECISION (E4):
			// a new user message ALWAYS resets the streak, even mid-gate --
			// i.e. a redirect ("actually, make it blue instead") immediately
			// un-gates the next mutating call, without requiring a look
			// first.  Justification: the gate exists to stop a model from
			// compounding errors it never checked, but a human who just
			// spoke has, by definition, looked at SOMETHING (the running
			// conversation/scene state) and made a deliberate judgment call
			// to redirect -- inheriting a stale gate from an unrelated prior
			// streak would refuse the model's very first attempt to act on
			// fresh, human-supervised instructions, for no safety benefit.
			// This mirrors the tool-round cap immediately above it (also
			// reset unconditionally by a new user turn) and was already
			// true of the pre-gate blind-edit-NUDGE streak this replaces --
			// carried forward unchanged, now serving the gate instead.
			mToolRounds = 0;
			mBlindEditStreak = 0;
		}

		ChatHttpRequest AgentChatLoop::BuildRequest( const std::string& apiKey )
		{
			FlushPendingToolResults();

			// TEXT-ONLY-MODEL IMAGE-REJECTION RECOVERY: once a model has
			// proven text-only, every request stays image-free -- strip any
			// image the flush above may have just packed (a new read_image
			// result) before it reaches the wire.  Idempotent no-op in the
			// common case (nothing live), so the pre-fix byte shape is
			// preserved for every image-capable session.
			if( mElideAllImages ) ElideAllLiveImages();

			// Nothing to send: refuse with an EMPTY request (url == "").
			// Documented in the header; the caller must not perform it.
			if( mTranscript.empty() )
				return ChatHttpRequest();

			// Context-compaction slice S2: drop the oldest whole spans when
			// over the token budget.  Runs AFTER the flush + image-elision
			// passes above (so there are no unanswered tool_use blocks to
			// split) and BEFORE rawEntries assembly (so it sees the final
			// transcript before it is serialized).  A no-op when the budget
			// is disabled/invalid or we are under the high-water mark.
			CompactTranscript();

			std::vector<std::string> rawEntries;
			rawEntries.reserve( mTranscript.size() );
			for( std::size_t i = 0; i < mTranscript.size(); ++i )
				rawEntries.push_back( mTranscript[i].rawJson );

			// Facet 5 slice S1: append the skills section (when set) to the
			// base prompt.  An empty index text sends the base prompt
			// UNCHANGED -- byte-identical to the pre-S1 behaviour.
			//
			// PROMPT-CACHE INVARIANT: this is the ONLY input to the system
			// block, and it is a pure function of (base prompt, skill index,
			// override) -- none of which change per request.  The system
			// prompt is therefore BYTE-IDENTICAL on every request of a
			// session.  Nothing transient may be folded in here: tools render
			// before system in every provider's cache prefix, so one changed
			// system byte invalidates tools + system + the whole history for
			// that request AND for the next one (which differs again by
			// reverting).  An early blind-edit NUDGE design used to append
			// advisory text here (and, after that, to the conversation
			// instead) -- both were retired for the SEQUENCING GATE (see the
			// file header), which needs no system-prompt or conversation
			// content at all: it answers a tool call, through the ordinary
			// ToolResults path.
			const std::string systemPrompt = ComposeSystemPrompt();

			// The key goes straight through to the codec's auth header and
			// is retained NOWHERE in this object.  REASONING-MODEL TOOLS-
			// VS-EFFORT 400 RECOVERY: once a reasoning model has proven the
			// tools/effort conflict, every request explicitly asks for
			// reasoning_effort "none" (see mForceReasoningEffortNone).
			ChatHttpRequest req = mCodec->BuildRequest(
				mModelId, apiKey, systemPrompt, rawEntries, mForceReasoningEffortNone );

			// Eval-harness E1: cache the request for RecordHttpRound's
			// convenience overload -- but with EVERY auth header STRIPPED so
			// the key is still held NOWHERE in this object (the same rule the
			// class contract states).
			if( mRecorder ) mLastRequest = StripAuthHeaders( req );

			return req;
		}

		ChatStepResult AgentChatLoop::HandleResponse( long httpStatus, const std::string& rawBody )
		{
			// Caller-contract guard: a legitimate response can only follow
			// a BuildRequest, and BuildRequest flushes the pending set --
			// so a HandleResponse while tool calls are still pending is a
			// double-consume for one round.  Refuse it, recording nothing.
			if( !mPendingCalls.empty() ) {
				ChatStepResult refused;
				refused.kind = ChatStepResult::Kind::ProviderError;
				refused.errorKind = ChatErrorKind::Misuse;
				refused.errorMessage =
					"chat-loop misuse: HandleResponse called while " +
					std::to_string( mPendingCalls.size() ) +
					" tool call(s) of the previous turn are still pending -- resolve them "
					"with AddToolResult (or BuildRequest/AddUserMessage, which flush them "
					"with synthesized error results) before handling another response";
				return refused;
			}

			ChatParsedResponse pr = mCodec->ParseResponse( httpStatus, rawBody );

			// TEXT-ONLY-MODEL IMAGE-REJECTION RECOVERY: a text-only backend
			// 400-rejects the request because a prior read_image (or a user
			// attachment) put an image on the wire.  Detect it NARROWLY
			// (status 400 + "multimodal"/"not support" in the body), and
			// only the FIRST time (mElideAllImages gates once-per-session,
			// which is also once-per-round: the retry request carries no
			// image, so a well-behaved server cannot 400-multimodal it
			// again; a misbehaving one that does finds mElideAllImages
			// already set and falls through to the normal provider_error).
			// On detection: elide EVERY live image from the transcript, set
			// the sticky bool so later rounds stay image-free, and flag the
			// step so the driver re-issues the SAME round once.  The step
			// stays a ProviderError (errorKind Http) so that if the retry
			// ALSO fails the existing error UX is unchanged; nothing is
			// recorded either way (the ProviderError contract).
			if( pr.step.kind == ChatStepResult::Kind::ProviderError &&
			    pr.step.errorKind == ChatErrorKind::Http &&
			    !mElideAllImages &&
			    IsMultimodalUnsupported400( httpStatus, rawBody ) ) {
				mElideAllImages = true;
				ElideAllLiveImages();
				pr.step.retryWithoutImages = true;
				return pr.step;
			}

			// REASONING-MODEL TOOLS-VS-EFFORT 400 RECOVERY: an OpenAI-family
			// reasoning model 400-rejects a function-tools request because
			// its server-side default reasoning_effort conflicts with tool
			// calling over /v1/chat/completions.  Detect it NARROWLY (status
			// 400 + "reasoning_effort"/"not support" in the body), and only
			// the FIRST time (mForceReasoningEffortNone gates once-per-
			// session/once-per-round, mirroring the image-rejection
			// recovery above).  On detection: set the sticky bool so this
			// and every later BuildRequest explicitly sends
			// "reasoning_effort":"none" (the documented remedy for staying
			// on /v1/chat/completions -- see mForceReasoningEffortNone's
			// header comment for why this is an ADD, not an omission), and
			// flag the step so the driver re-issues the SAME round once.
			// The step stays a ProviderError (errorKind Http) so that if the
			// retry ALSO fails the existing error UX is unchanged; nothing
			// is recorded either way (the ProviderError contract).  ALSO
			// records the fact into the process-wide capability cache
			// (ReasoningEffortNoneAlreadyKnown/MarkReasoningEffortNoneKnown)
			// so every LATER AgentChatLoop instance in this process against
			// the SAME (provider, model) starts pre-armed and never pays
			// this wasted round-trip again -- see that cache's doc comment.
			if( pr.step.kind == ChatStepResult::Kind::ProviderError &&
			    pr.step.errorKind == ChatErrorKind::Http &&
			    !mForceReasoningEffortNone &&
			    IsReasoningEffortToolsUnsupported400( httpStatus, rawBody ) ) {
				mForceReasoningEffortNone = true;
				MarkReasoningEffortNoneKnown( mCodec->ProviderName(), mModelId );
				pr.step.retryReasoningEffortNone = true;
				return pr.step;
			}

			if( pr.step.kind == ChatStepResult::Kind::ToolCalls ) {
				// Iteration cap: the (kMaxToolRoundsPerTurn+1)-th tool round
				// within one conversation-turn is refused WITHOUT recording
				// the assistant turn -- the transcript stays at its previous
				// consistent state (no unanswered tool_use on the wire) and
				// a new AddUserMessage resets the counter.
				if( mToolRounds >= mMaxToolRoundsPerTurn ) {
					ChatStepResult capped;
					capped.kind = ChatStepResult::Kind::ProviderError;
					capped.errorKind = ChatErrorKind::IterationCap;
					capped.errorMessage =
						"iteration cap: the model requested more than " +
						std::to_string( mMaxToolRoundsPerTurn ) +
						" tool rounds in one turn; stopping so the loop cannot spin";
					return capped;
				}
				++mToolRounds;

				ChatTranscriptEntry entry;
				entry.role = ChatTranscriptEntry::Role::Assistant;
				entry.displayText = pr.assistantDisplayText;
				// DISPLAY-LAYER ENRICHMENT: mirrors displayText <- pr.assistantDisplayText
				// immediately above -- DISPLAY-ONLY, see the CRITICAL INVARIANT
				// block on ChatTranscriptEntry::reasoningText (AgentChatLoop.h).
				entry.reasoningText = pr.reasoningText;
				entry.rawJson = pr.assistantEntryJson;
				mTranscript.push_back( entry );

				mPendingCalls = pr.step.toolCalls;
				mPendingResults.clear();
			}
			else if( pr.step.kind == ChatStepResult::Kind::FinalText ) {
				ChatTranscriptEntry entry;
				entry.role = ChatTranscriptEntry::Role::Assistant;
				entry.displayText = pr.assistantDisplayText;
				// DISPLAY-LAYER ENRICHMENT: see the tool-call branch above.
				entry.reasoningText = pr.reasoningText;
				entry.rawJson = pr.assistantEntryJson;
				mTranscript.push_back( entry );

				mPendingCalls.clear();
				mPendingResults.clear();
			}
			// ProviderError: record nothing -- the transcript stays at its
			// previous consistent state and the caller may retry.

			return pr.step;
		}

		std::string AgentChatLoop::ToolCallToJsonRpcLine( const ChatToolCall& call, int rpcId )
		{
			JsonValue params;
			std::string perr;
			if( !JsonParse( call.argsJson, params, perr ) || !params.isObject() )
				params = JsonValue::MakeObject();

			JsonValue req = JsonValue::MakeObject();
			req.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
			req.set( "id", JsonValue::MakeNumber( static_cast<double>( rpcId ) ) );
			req.set( "method", JsonValue::MakeString( call.name ) );
			req.set( "params", params );
			const std::string line = JsonSerialize( req );

			// Eval-harness E1: stamp the dispatch start + line, keyed by call
			// id, so AddToolResult can complete the `tool` record's latency.
			if( mRecorder ) {
				ToolLinePending pend;
				pend.id = call.id;
				pend.line = line;
				pend.startMs = mTrajectoryClock ? mTrajectoryClock() : TrajectoryNowMs();
				mToolLineStash.push_back( pend );
			}
			return line;
		}

		std::string AgentChatLoop::GateRefusalResponse( const ChatToolCall& call, int rpcId )
		{
			if( !IsMutatingToolName( call.name ) ) return std::string();
			if( mBlindEditGateThreshold <= 0 ) return std::string();
			if( mBlindEditStreak < mBlindEditGateThreshold ) return std::string();

			// Gated: this call would be the (threshold+1)-th (or later)
			// consecutive mutation with no look between.  Mark the call id
			// so the AddToolResult the caller makes next (with THIS
			// envelope, per the contract) knows to skip the streak
			// accounting -- a refused call never touched the document, so
			// it is not itself a blind edit and must not advance the streak
			// further (every refusal reports the SAME streak count until a
			// look resets it).
			mGateRefusedCallIds.push_back( call.id );

			char msg[400];
			std::snprintf( msg, sizeof( msg ),
				"%d document edits in a row without looking. Rendering catches compounding "
				"errors -- call render (a small preview is enough) or read_viewport, then "
				"edits continue.",
				mBlindEditStreak );

			JsonValue result = JsonValue::MakeObject();
			result.set( "applied",   JsonValue::MakeBool( false ) );
			result.set( "status",    JsonValue::MakeString( "rejected" ) );
			result.set( "retriable", JsonValue::MakeBool( true ) );
			result.set( "message",   JsonValue::MakeString( msg ) );

			JsonValue env = JsonValue::MakeObject();
			env.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
			env.set( "id", JsonValue::MakeNumber( static_cast<double>( rpcId ) ) );
			env.set( "result", result );
			return JsonSerialize( env );
		}

		void AgentChatLoop::AddToolResult( const ChatToolCall& call, const std::string& rawJsonRpcResponseLine )
		{
			// Accept a result ONLY for a pending, not-yet-answered call of
			// the current assistant turn (documented in the header): an id
			// that is not pending, or a second result for an id that
			// already has one, is IGNORED -- first result wins.
			bool pending = false;
			for( std::size_t i = 0; i < mPendingCalls.size(); ++i ) {
				if( mPendingCalls[i].id == call.id ) { pending = true; break; }
			}
			if( !pending ) return;
			for( std::size_t i = 0; i < mPendingResults.size(); ++i ) {
				if( mPendingResults[i].first.id == call.id ) return;
			}

			mPendingResults.push_back( std::make_pair( call, rawJsonRpcResponseLine ) );

			// SEQUENCING GATE (see kDefaultBlindEditGateThreshold and the
			// file header): track a run of document mutations with no
			// VISUAL observation between them.  A model building a whole
			// scene can slip into "insert forever, never look" -- measured:
			// a local model emitted 70-100+ chunks and rendered zero times.
			// We reset the run on any visual observe (IsVisualObserveToolName),
			// leave it UNCHANGED on a non-visual read (read_document /
			// read_schema / read_skill / validate -- inspecting text or the
			// registry is not looking at the RENDERED result) and on
			// ask_user (see IsVisualObserveToolName's doc), and grow it on a
			// mutation (IsMutatingToolName) -- UNLESS this particular result
			// is itself a GateRefusalResponse the caller just built for this
			// exact call id, in which case nothing was actually mutated and
			// the streak must not move at all (see GateRefusalResponse's
			// doc).  GateRefusalResponse is what actually REFUSES a call
			// once the streak reaches the threshold; this block only ever
			// counts.
			{
				bool wasGateRefused = false;
				for( std::size_t i = 0; i < mGateRefusedCallIds.size(); ++i ) {
					if( mGateRefusedCallIds[i] == call.id ) {
						wasGateRefused = true;
						mGateRefusedCallIds.erase( mGateRefusedCallIds.begin() + i );
						break;
					}
				}
				if( !wasGateRefused ) {
					if( IsVisualObserveToolName( call.name ) ) {
						mBlindEditStreak = 0;
					}
					else if( IsMutatingToolName( call.name ) ) {
						++mBlindEditStreak;
					}
				}
			}

			// Eval-harness E1: complete the `tool` record for this call --
			// pair the stamped JSON-RPC line/latency with the response envelope.
			if( mRecorder ) {
				EnsureSessionRecordEmitted();
				TrajectoryToolRecord t;
				t.name = call.name;
				t.callId = call.id;
				t.argsJson = call.argsJson.empty() ? std::string( "{}" ) : call.argsJson;
				t.jsonRpcResponse = rawJsonRpcResponseLine;
				const int64_t now = mTrajectoryClock ? mTrajectoryClock() : TrajectoryNowMs();
				for( std::size_t k = 0; k < mToolLineStash.size(); ++k ) {
					if( mToolLineStash[k].id == call.id ) {
						t.jsonRpcRequest = mToolLineStash[k].line;
						t.latencyMs = now - mToolLineStash[k].startMs;
						mToolLineStash.erase( mToolLineStash.begin() + k );
						break;
					}
				}
				JsonValue env;
				std::string perr;
				if( JsonParse( rawJsonRpcResponseLine, env, perr ) && env.isObject() ) {
					const JsonValue* errp = env.find( "error" );
					if( errp && !errp->isNull() ) t.error = true;
					const JsonValue& result = env.get( "result" );
					if( result.isObject() ) {
						const JsonValue& hv = result.get( "headVersion" );
						if( hv.isObject() ) {
							if( hv.get( "revision" ).isNumber() )
								t.headVersionAfter = static_cast<long long>( hv.get( "revision" ).asNumber() );
						}
						else if( hv.isNumber() ) {
							t.headVersionAfter = static_cast<long long>( hv.asNumber() );
						}
					}
				}
				mRecorder->EmitTool( t );
			}

			// Once every pending call of the assistant turn has a result,
			// pack them ALL into one user message (Anthropic requires the
			// parallel tool_use blocks to be answered together).
			if( mPendingResults.size() >= mPendingCalls.size() )
				FlushPendingToolResults();
		}

		namespace
		{
			//! Back off from byte offset `n` (into `s`) while the byte there
			//! is a UTF-8 CONTINUATION byte ((b & 0xC0) == 0x80), returning
			//! the largest cut position <= `n` that does NOT land inside a
			//! multi-byte codepoint.  SHARED by every byte-count truncation
			//! in this file -- the 8KB resultJson cap in
			//! FlushPendingToolResults and ToolOutcomeLine's ~80-char
			//! message cap below (via TruncateForOutcome) -- so cutting a
			//! provider- or RPC-supplied string at a fixed byte count can
			//! never split a multi-byte sequence and emit invalid UTF-8.
			//! `n >= s.size()` is returned unchanged (nothing to cut); the
			//! back-off floor is 0 (an empty prefix is always valid UTF-8,
			//! so a pathological string of nothing but continuation bytes
			//! degrades safely rather than looping).
			std::size_t Utf8SafeCutPoint( const std::string& s, std::size_t n )
			{
				if( n >= s.size() ) return n;
				while( n > 0 && ( static_cast<unsigned char>( s[n] ) & 0xC0 ) == 0x80 ) --n;
				return n;
			}

			//! Truncate `s` to at most `maxChars` bytes (UTF-8-safe -- see
			//! Utf8SafeCutPoint), appending "..." when it was longer --
			//! shared by ToolOutcomeLine's error/rejection arms so a
			//! runaway provider- or RPC-supplied message string can never
			//! blow up the one-line outcome summary.
			std::string TruncateForOutcome( const std::string& s, std::size_t maxChars )
			{
				if( s.size() <= maxChars ) return s;
				return s.substr( 0, Utf8SafeCutPoint( s, maxChars ) ) + "...";
			}

			//! DISPLAY-LAYER ENRICHMENT: the "read schema tells you nothing"
			//! fix.  Turns one tool call's raw JSON-RPC response ENVELOPE
			//! line into a short, deterministic, human-readable one-liner
			//! for the GUI's transcript row -- e.g. "160x120, luma 0.19" for
			//! a render, "3/4 applied" for a batch insert.  DISPLAY-ONLY:
			//! the caller (FlushPendingToolResults) never feeds this string
			//! back onto the wire.
			//!
			//! Deterministic rules, in priority order (mirrors the class's
			//! design brief exactly -- a parse failure or any shape this
			//! function does not recognize degrades to a safe generic
			//! string rather than guessing or crashing):
			//!   1. A JSON-RPC `error` envelope           -> "error: <msg, <=80 chars>"
			//!   2. result.status == "rejected"            -> "rejected: <issues[0].reason `param`, or <=80 chars of message>"
			//!   3. result.status == "conflict"             -> "conflict (stale base)"
			//!   4. name in {insert_chunks,propose_patches,insert_material_scaffold,insert_geometry_scaffold}  -> "<applied>/<total> applied"
			//!   5. name in {insert_chunk,propose_patch,remove_chunk}
			//!      AND result.applied == true               -> "applied: <kind> `<name>`" (propose_patch has no kind/name echo -> "applied")
			//!   6. name == "render"                         -> "<w>x<h>, luma <2dp>" (+ " [<renderMode>]" when renderMode isn't "" or "beauty")
			//!   7. name in {read_image,read_viewport}       -> "image <w>x<h>" when width/height are present, else "ok"
			//!   8. name == "validate" AND result.diagnostics is an array
			//!                                               -> "clean" | "<n> warning(s)" | "<n> error(s): <firstCode>",
			//!                                                  each with " (candidate)" appended when validated == "text".
			//!                                                  "clean" means zero error AND zero warning entries --
			//!                                                  Info-severity entries (creative-richness P2.b's
			//!                                                  DESIGN_SCALAR_PIPE_UNUSED / DESIGN_NO_ADVANCED_GEOMETRY
			//!                                                  advisories) count toward NEITHER bucket, so a scene
			//!                                                  carrying only advisories still reads "clean" here --
			//!                                                  same convention AgentEvalRunner.cpp's
			//!                                                  CheckDiagnosticsKind uses for expect:"clean".
			//!   9. anything else                            -> "ok"
			//! A line that does not parse as a JSON object returns "?".
			std::string ToolOutcomeLine( const ChatToolCall& call, const std::string& rawJsonRpcResponseLine )
			{
				JsonValue env;
				std::string perr;
				if( !JsonParse( rawJsonRpcResponseLine, env, perr ) || !env.isObject() ) return "?";

				// 1. JSON-RPC error envelope (the dispatcher's -32602/-32001/...
				// shape, or the loop's own synthesized "not executed" envelope).
				if( const JsonValue* err = env.find( "error" ) ) {
					if( err->isObject() ) {
						return "error: " + TruncateForOutcome( err->get( "message" ).asString(), 80 );
					}
				}

				const JsonValue& result = env.get( "result" );
				const std::string status = result.get( "status" ).asString();

				// 2. Rejected: prefer the first structured issue (matches
				// insert_chunk/insert_chunks/propose_patch/remove_chunk's
				// shared {param,value,reason,suggestions} `issues` shape --
				// see AgentRpc.cpp's IssuesJson); fall back to the free-text
				// `message` when no issues were attached.
				if( status == "rejected" ) {
					const JsonValue& issues = result.get( "issues" );
					if( issues.isArray() && issues.size() > 0 ) {
						const JsonValue& first = issues.at( 0 );
						const std::string reason = first.get( "reason" ).asString();
						if( !reason.empty() ) {
							return "rejected: " + reason + " `" + first.get( "param" ).asString() + "`";
						}
					}
					return "rejected: " + TruncateForOutcome( result.get( "message" ).asString(), 80 );
				}

				// 3. Conflict: a stale baseHeadVersion precondition -- the
				// SAME fixed string regardless of verb (insert_chunk/
				// insert_chunks/propose_patch/remove_chunk all gate this way).
				if( status == "conflict" ) return "conflict (stale base)";

				// 4. The BATCH verbs: the batch summary (result.applied is a
				// COUNT here, not a bool -- distinct from insert_chunk's /
				// propose_patch's per-call boolean of the same name; see
				// AgentRpc.cpp).  Both batch verbs return the IDENTICAL
				// {applied,total,results} envelope, so they share this rule --
				// without it propose_patches would fall through to the generic
				// "ok" of rule 9 and report the SAME string whether 17/17 or
				// 0/17 elements applied, which is precisely the outcome a
				// best-effort batch verb most needs to surface.
				if( call.name == "insert_chunks" || call.name == "propose_patches" ||
				    // Arc-75 S2.1: insert_material_scaffold returns the
				    // EXACT same {applied,total,results} batch envelope
				    // (it submits through InsertChunks) -- same rule.
				    call.name == "insert_material_scaffold" ||
				    // Arc-75 S3b: insert_geometry_scaffold is the geometry
				    // sibling, SAME {applied,total,results} batch envelope.
				    call.name == "insert_geometry_scaffold" ) {
					const long long applied = static_cast<long long>( result.get( "applied" ).asNumber() );
					const long long total   = static_cast<long long>( result.get( "total" ).asNumber() );
					return std::to_string( applied ) + "/" + std::to_string( total ) + " applied";
				}

				// 5. A single-chunk mutation that applied cleanly.
				// propose_patch's result carries no name/kind echo (only
				// insert_chunk/remove_chunk do, via ChunkResultJson) -- so it
				// gets the bare "applied" the design brief calls for.
				if( ( call.name == "insert_chunk" || call.name == "propose_patch" ||
				      call.name == "remove_chunk" ) && result.get( "applied" ).asBool() ) {
					if( call.name == "propose_patch" ) return "applied";
					return "applied: " + result.get( "kind" ).asString() +
					       " `" + result.get( "name" ).asString() + "`";
				}

				// 6. render: dimensions + mean luma (unweighted RGB mean, 2dp),
				// with the render mode annotated when it is anything other
				// than the default "beauty" (an empty renderMode -- e.g. a
				// hand-built envelope in a test -- is treated the same as
				// "beauty": no annotation).
				if( call.name == "render" ) {
					const long long w = static_cast<long long>( result.get( "width" ).asNumber() );
					const long long h = static_cast<long long>( result.get( "height" ).asNumber() );
					const double luma = ( result.get( "meanR" ).asNumber() +
					                      result.get( "meanG" ).asNumber() +
					                      result.get( "meanB" ).asNumber() ) / 3.0;
					char lumaBuf[32];
					std::snprintf( lumaBuf, sizeof( lumaBuf ), "%.2f", luma );
					std::string line = std::to_string( w ) + "x" + std::to_string( h ) + ", luma " + lumaBuf;
					const std::string mode = result.get( "renderMode" ).asString();
					if( !mode.empty() && mode != "beauty" ) line += " [" + mode + "]";
					return line;
				}

				// 7. read_image / read_viewport: both always carry
				// width/height (0 when read_viewport has no live frame) --
				// the "if present" guard is for a hostile/hand-built
				// envelope (a test fixture, a misbehaving dispatcher) that
				// omits them entirely rather than the documented RPC shape.
				if( call.name == "read_image" || call.name == "read_viewport" ) {
					if( result.has( "width" ) && result.has( "height" ) ) {
						const long long w = static_cast<long long>( result.get( "width" ).asNumber() );
						const long long h = static_cast<long long>( result.get( "height" ).asNumber() );
						return "image " + std::to_string( w ) + "x" + std::to_string( h );
					}
					return "ok";
				}

				// 8. validate: the one-liner must say whether the scene is
				// CLEAN, for the same reason rule 4 summarises a batch as
				// "3/4 applied" rather than "ok" -- reporting the identical
				// string for a clean head and for one carrying errors is
				// exactly the display defect that rule exists to avoid.  The
				// exposure grew when the no-argument form made validate THE
				// routine post-edit check (see the system prompt and both tool
				// descriptions); the model always saw the diagnostics (they
				// ride the raw JSON-RPC line), but the human watching the
				// transcript saw "validate -> ok" over a diagnosed document.
				if( call.name == "validate" && result.get( "diagnostics" ).isArray() ) {
					const JsonValue& diags = result.get( "diagnostics" );
					std::size_t errors = 0, warnings = 0;
					std::string firstCode;
					// Creative-richness P2.b: Info-severity entries (the
					// DESIGN_* advisories) count toward NEITHER bucket -- an
					// advisory is not a warning, and folding it into the
					// warning count here would be exactly the mislabeling sec 9
					// warns against (an Info entry described as a problem).
					for( std::size_t i = 0; i < diags.size(); ++i ) {
						const std::string sev = diags.at( i ).get( "severity" ).asString();
						if( sev == "error" ) {
							++errors;
							if( firstCode.empty() ) firstCode = diags.at( i ).get( "code" ).asString();
						} else if( sev == "warning" ) {
							++warnings;
						}
					}
					const std::string scope =
						result.get( "validated" ).asString() == "text" ? " (candidate)" : "";
					if( errors == 0 && warnings == 0 ) return "clean" + scope;
					if( errors == 0 )
						return std::to_string( warnings ) + " warning(s)" + scope;
					return std::to_string( errors ) + " error(s)" +
					       ( firstCode.empty() ? std::string() : ": " + firstCode ) + scope;
				}

				// 9. Every other verb (read_document, read_schema, read_skill,
				// query_object_at, list_proposals, ...): a plain success with
				// nothing terser to say than "ok".
				return "ok";
			}

			//! The text that replaces an elided (superseded) summary result --
			//! DELIBERATELY the same wording AgentChatCodecs.cpp's
			//! kImageElidedNote uses for the owning entry's rawJson rewrite
			//! (that constant is file-local to AgentChatCodecs.cpp, so this
			//! is a separate literal, not a shared symbol -- keep the two in
			//! sync by hand if the wording ever changes).
			const char* const kSummaryImageElidedNote =
				"[image elided -- superseded by a newer image]";

			//! ELISION CONSISTENCY (see ChatToolDisplaySummary::carriesImage's
			//! doc): rewrite every image-bearing summary in `summaries` to
			//! the elided placeholder and clear its flag -- the
			//! toolSummaries sibling of RewriteElidedImages' rawJson
			//! rewrite.  Shared by BOTH elision call sites (the
			//! FlushPendingToolResults older-entry retention pass and
			//! ElideAllLiveImages) so a summary can never keep serving a
			//! STALE base64 blob after its owning entry's rawJson has
			//! already been elided.  A no-op (no-cost early return for the
			//! common case) when nothing in `summaries` carries an image.
			void ElideSummaryImages( std::vector<ChatToolDisplaySummary>& summaries )
			{
				for( std::size_t i = 0; i < summaries.size(); ++i ) {
					if( !summaries[i].carriesImage ) continue;
					summaries[i].resultJson = kSummaryImageElidedNote;
					summaries[i].carriesImage = false;
				}
			}
		}

		void AgentChatLoop::FlushPendingToolResults()
		{
			// No pending turn -> nothing to flush.  (mPendingResults cannot
			// be non-empty here: AddToolResult only accepts answers to
			// pending calls.  Clear defensively all the same.)
			if( mPendingCalls.empty() ) {
				mPendingResults.clear();
				return;
			}

			// WIRE INVARIANT (by construction): every tool call of the
			// recorded assistant turn is answered in this ONE user message.
			// Any pending call without a buffered result -- user interrupt,
			// tool crash, partial round -- gets a SYNTHESIZED error result;
			// otherwise the transcript would replay an unanswered tool call
			// on every later request (Anthropic hard-400s it, Gemini
			// rejects the mismatched functionResponse count) and only
			// Reset() could recover.
			static const char* const kUnexecutedEnvelope =
				"{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32001,"
				"\"message\":\"tool call was not executed (cancelled or interrupted)\"}}";

			std::vector<std::pair<ChatToolCall, std::string>> ordered;
			ordered.reserve( mPendingCalls.size() );
			std::vector<bool> used( mPendingResults.size(), false );
			for( std::size_t i = 0; i < mPendingCalls.size(); ++i ) {
				std::size_t found = mPendingResults.size();
				for( std::size_t j = 0; j < mPendingResults.size(); ++j ) {
					if( !used[j] && mPendingResults[j].first.id == mPendingCalls[i].id ) {
						found = j;
						break;
					}
				}
				if( found < mPendingResults.size() ) {
					used[found] = true;
					ordered.push_back( mPendingResults[found] );
				}
				else {
					// ToolOutcomeLine reads this envelope's "error" object and
					// reports it verbatim ("error: tool call was not executed
					// ...") -- so the not-executed case needs no separate
					// name-suffix bookkeeping the way the old "[tool results:
					// ...]" display string did.
					ordered.push_back( std::make_pair( mPendingCalls[i],
					                                   std::string( kUnexecutedEnvelope ) ) );
				}
			}
			// Defensive: mPendingResults can only hold answers to pending
			// calls (AddToolResult filters), so `ordered` covers everything.

			// DISPLAY-LAYER ENRICHMENT (regression fix): a per-call outcome
			// one-liner (ToolOutcomeLine) plus a per-call display summary,
			// built from `ordered` -- i.e. covering exactly the calls that
			// were packed onto the wire, synthesized "not executed" results
			// included.  This is what REPLACES the old opaque
			// "[tool results: name, name]" display string (which told the
			// user nothing about what actually happened) -- entry.displayText
			// below joins each "<name> → <outcome>" item (U+2192
			// RIGHTWARDS ARROW) with a " · " (U+00B7 MIDDLE DOT)
			// separator, e.g. "insert_chunks → 4/4 applied · render
			// → 160x120, luma 0.19" -- both emitted as explicit UTF-8
			// byte-escape literals below (NOT raw source characters) so the
			// bytes are correct regardless of the compiler's assumed source
			// encoding (MSVC narrow-literal handling is code-page-dependent
			// without an explicit /utf-8 flag; escapes sidestep that
			// entirely on every one of the five build projects).
			// DISPLAY-ONLY throughout: see the CRITICAL INVARIANT block on
			// ChatTranscriptEntry::reasoningText in AgentChatLoop.h --
			// nothing here touches entry.rawJson.
			std::string joinedOutcome;
			std::vector<ChatToolDisplaySummary> summaries;
			summaries.reserve( ordered.size() );
			static const std::size_t kMaxResultJsonBytes = 8192;
			for( std::size_t i = 0; i < ordered.size(); ++i ) {
				const ChatToolCall& c = ordered[i].first;
				const std::string& raw = ordered[i].second;
				const std::string outcome = ToolOutcomeLine( c, raw );

				if( i ) joinedOutcome += " \xC2\xB7 ";   // U+00B7 MIDDLE DOT, UTF-8
				joinedOutcome += c.name + " \xE2\x86\x92 " + outcome;   // U+2192 RIGHTWARDS ARROW, UTF-8

				ChatToolDisplaySummary s;
				s.name = c.name;
				s.outcomeLine = outcome;
				s.argsJson = c.argsJson;
				s.resultJson = ( raw.size() > kMaxResultJsonBytes )
					? raw.substr( 0, Utf8SafeCutPoint( raw, kMaxResultJsonBytes ) ) + "...[truncated]"
					: raw;
				// ELISION CONSISTENCY (see ChatToolDisplaySummary::carriesImage's
				// doc): stamp whether THIS summary's resultJson is a live
				// read_image base64 result, using the SAME predicate the
				// IMAGE RETENTION pass below uses to decide whether the
				// owning entry carries an image -- so the two never disagree
				// about what counts as "image-bearing".
				s.carriesImage = ChatToolResultCarriesImage( c, raw );
				summaries.push_back( s );
			}

			// IMAGE RETENTION (see the header): when this entry packs a NEW
			// image, elide the image from every older ToolResults entry so
			// only the most recent PNG rides (and is billed) per request.
			// ToolResults entries are loop-generated, so rewriting them is
			// legal; assistant entries are never touched.
			bool carriesImage = false;
			for( std::size_t i = 0; i < ordered.size(); ++i ) {
				if( ChatToolResultCarriesImage( ordered[i].first, ordered[i].second ) ) {
					carriesImage = true;
					break;
				}
			}
			if( carriesImage ) {
				for( std::size_t i = 0; i < mTranscript.size(); ++i ) {
					if( mTranscript[i].role != ChatTranscriptEntry::Role::ToolResults ||
					    !mTranscript[i].carriesLiveImage ) continue;
					const std::size_t beforeBytes = mTranscript[i].rawJson.size();
					mTranscript[i].rawJson = mCodec->RewriteElidedImages( mTranscript[i].rawJson );
					mTranscript[i].carriesLiveImage = false;
					// The entry's live image is gone -- its whole tracked
					// payload goes with it (see EstimateContextTokens).
					mTranscript[i].imageContentBytes = 0;
					// ELISION CONSISTENCY: the display-layer toolSummaries
					// sibling of the rawJson rewrite just above -- without
					// this, a summary would keep serving the STALE base64
					// blob (and the memory it retains) after the owning
					// entry's rawJson was already elided.
					ElideSummaryImages( mTranscript[i].toolSummaries );
					// Eval-harness E1: record the transcript rewrite.
					if( mRecorder ) {
						EnsureSessionRecordEmitted();
						TrajectoryHistoryEditRecord h;
						h.entryIndex = static_cast<int>( i );
						h.beforeBytes = static_cast<long long>( beforeBytes );
						h.afterBytes = static_cast<long long>( mTranscript[i].rawJson.size() );
						h.reason = "tool_image_elision";
						mRecorder->EmitHistoryEdit( h );
					}
				}
			}

			ChatTranscriptEntry entry;
			entry.role = ChatTranscriptEntry::Role::ToolResults;
			entry.displayText = joinedOutcome;
			entry.toolSummaries = summaries;
			// SUPERSEDED-READ RETENTION bookkeeping: one slot per packed
			// result, in `ordered` order -- which IS the order
			// PackToolResults writes them, and therefore the index space
			// RewriteElidedToolResults addresses.  Built here (not lazily)
			// so the key reflects the envelope AS PACKED.
			entry.toolResultSlots.resize( ordered.size() );
			for( std::size_t i = 0; i < ordered.size(); ++i )
				entry.toolResultSlots[i].supersessionKey =
					ChatToolResultSupersessionKey( ordered[i].first, ordered[i].second );
			entry.rawJson = mCodec->PackToolResults( ordered );
			entry.carriesLiveImage = carriesImage;
			if( carriesImage ) {
				// EstimateContextTokens's per-entry text proxy excludes
				// tracked live image payload bytes -- approximate that
				// payload as the byte delta between the packed entry and
				// its already-elided form (RewriteElidedImages replaces the
				// image block with a short note), so a co-packed
				// read_document's text is NOT silently dropped from the
				// estimate.  This slightly OVER-counts text (the elided
				// note itself has nonzero length), which is the SAFE
				// direction -- it only makes compaction trigger sooner.
				entry.imageContentBytes = entry.rawJson.size() -
					mCodec->RewriteElidedImages( entry.rawJson ).size();
			}
			mTranscript.push_back( entry );

			// SUPERSEDED-READ RETENTION (see the header): now that the new
			// entry is in the transcript, keep only the LAST live result per
			// supersession key across the whole conversation.  Runs AFTER the
			// image pass and after entry.imageContentBytes is computed: the
			// two rewrites are disjoint (an image result is never
			// supersedable -- see ChatToolResultSupersessionKey's property
			// (4)), and a text-only shrink of rawJson leaves the tracked
			// image payload correct either way.
			ElideSupersededToolResults();

			mPendingResults.clear();
			mPendingCalls.clear();
			// Eval-harness E1: the turn's tool calls are done -- drop any
			// remaining stamped lines (unanswered/synthesized calls).
			mToolLineStash.clear();
		}

		void AgentChatLoop::ElideSupersededToolResults()
		{
			// PASS 1: find, for every supersession key, the LAST still-live
			// slot in wire order.  Scanning forward and overwriting means the
			// map ends holding exactly the survivor of each key.
			//
			// A dead slot is skipped entirely rather than treated as a
			// survivor -- so a key whose only remaining live slot is the
			// newest one converges, and re-running the sweep is a no-op
			// (idempotence).
			std::vector<std::pair<std::string, std::pair<std::size_t, std::size_t>>> lastLive;
			for( std::size_t e = 0; e < mTranscript.size(); ++e ) {
				const ChatTranscriptEntry& entry = mTranscript[e];
				if( entry.role != ChatTranscriptEntry::Role::ToolResults ) continue;
				for( std::size_t s = 0; s < entry.toolResultSlots.size(); ++s ) {
					const ChatToolResultSlot& slot = entry.toolResultSlots[s];
					if( !slot.live || slot.supersessionKey.empty() ) continue;
					bool replaced = false;
					for( std::size_t k = 0; k < lastLive.size(); ++k ) {
						if( lastLive[k].first == slot.supersessionKey ) {
							lastLive[k].second = std::make_pair( e, s );
							replaced = true;
							break;
						}
					}
					if( !replaced )
						lastLive.push_back( std::make_pair( slot.supersessionKey,
						                                    std::make_pair( e, s ) ) );
				}
			}
			if( lastLive.empty() ) return;

			// PASS 2: every OTHER live slot sharing a surviving key is
			// superseded.  Collect per entry so each entry's rawJson is
			// rewritten (and recorded) exactly once, even when it packs
			// several superseded results.
			for( std::size_t e = 0; e < mTranscript.size(); ++e ) {
				ChatTranscriptEntry& entry = mTranscript[e];
				if( entry.role != ChatTranscriptEntry::Role::ToolResults ) continue;

				// Grouped BY KEY, not one bucket per entry: the placeholder
				// names its own verb, so an entry that ever packs superseded
				// results of TWO different allowlisted verbs must not be told
				// one verb's note for both.  (Today's single-verb allowlist
				// makes that unreachable -- grouping keeps it correct by
				// construction rather than by that coincidence.)
				std::vector<std::pair<std::string, std::vector<std::size_t>>> byKey;
				for( std::size_t s = 0; s < entry.toolResultSlots.size(); ++s ) {
					ChatToolResultSlot& slot = entry.toolResultSlots[s];
					if( !slot.live || slot.supersessionKey.empty() ) continue;
					bool survives = false;
					for( std::size_t k = 0; k < lastLive.size(); ++k ) {
						if( lastLive[k].first != slot.supersessionKey ) continue;
						survives = ( lastLive[k].second.first == e &&
						             lastLive[k].second.second == s );
						break;
					}
					if( survives ) continue;
					bool bucketed = false;
					for( std::size_t k = 0; k < byKey.size(); ++k ) {
						if( byKey[k].first == slot.supersessionKey ) {
							byKey[k].second.push_back( s );
							bucketed = true;
							break;
						}
					}
					if( !bucketed ) {
						std::vector<std::size_t> one;
						one.push_back( s );
						byKey.push_back( std::make_pair( slot.supersessionKey, one ) );
					}
				}
				if( byKey.empty() ) continue;

				// REWRITE FIRST, THEN mark the bucket dead -- and only when
				// the rewrite actually LANDED.  Every codec returns its input
				// UNCHANGED when the entry does not parse or its shape guards
				// miss; clearing `live` before knowing that would leave the
				// payload riding forever while the bookkeeping believed it
				// elided, and pass 1 (which skips dead slots) would never
				// retry.  A no-op rewrite instead leaves the slots live, so
				// the next flush tries again -- bounded work, and no
				// history_edit is emitted for a non-edit.
				//
				// GRANULARITY, stated exactly: the landing signal is
				// PER BUCKET (did the entry's bytes move?), not per index.
				// If a codec ever rewrote index i but skipped index j of the
				// same bucket, both would be marked dead and j would ride on
				// un-elided.  That is unreachable today -- RewriteElidedToolResults
				// is TOTAL over a bucket's indices, because
				// toolResultSlots.size() == ordered.size() and every codec
				// emits exactly one addressable result per element of
				// `ordered` at indices 0..N-1 (the OpenAI trailing image
				// message is appended AFTER them).  A codec that ever
				// violated that totality would need a per-index landing
				// signal here.
				const std::size_t beforeBytes = entry.rawJson.size();
				bool anyLanded = false;
				for( std::size_t k = 0; k < byKey.size(); ++k ) {
					const std::string before = entry.rawJson;
					entry.rawJson = mCodec->RewriteElidedToolResults(
						entry.rawJson, byKey[k].second,
						ChatSupersededResultNote( byKey[k].first ) );
					if( entry.rawJson == before ) continue;
					anyLanded = true;
					for( std::size_t j = 0; j < byKey[k].second.size(); ++j )
						entry.toolResultSlots[ byKey[k].second[j] ].live = false;
				}
				if( !anyLanded ) continue;
				// Eval-harness E1: record the transcript rewrite, exactly as
				// the image-elision pass does.
				if( mRecorder ) {
					EnsureSessionRecordEmitted();
					TrajectoryHistoryEditRecord h;
					h.entryIndex = static_cast<int>( e );
					h.beforeBytes = static_cast<long long>( beforeBytes );
					h.afterBytes = static_cast<long long>( entry.rawJson.size() );
					h.reason = "tool_result_supersession";
					mRecorder->EmitHistoryEdit( h );
				}
			}
		}

		//! GUI STAGE 2: a public, declared entry point onto the file-local
		//! ToolOutcomeLine helper above -- so a caller OUTSIDE this
		//! translation unit (the Mac GUI's ObjC++ bridge) can compute the
		//! same one-line outcome summary FlushPendingToolResults uses for
		//! ChatToolDisplaySummary::outcomeLine, for a tool result it is
		//! handling asynchronously (the render tool's async path) before
		//! that summary has been packed into the transcript.  Pure
		//! delegation -- the anonymous-namespace implementation is
		//! deliberately left in place (single source of truth for the
		//! parsing rules; see its doc above) rather than moved or
		//! duplicated.
		std::string AgentChatLoop::ToolOutcomeLineForDisplay( const ChatToolCall& call,
		                                                       const std::string& rawJsonRpcResponseLine )
		{
			return ToolOutcomeLine( call, rawJsonRpcResponseLine );
		}

		void AgentChatLoop::ElideAllLiveImages()
		{
			// TEXT-ONLY-MODEL IMAGE-REJECTION RECOVERY: rewrite every entry
			// that still carries a live image to its already-elided form.
			// Two independent kinds ride here, each with its own codec
			// rewrite + its own liveness flag (mirroring the two retention
			// policies already in this file):
			//   * ToolResults entries (packed read_image PNGs) via
			//     RewriteElidedImages -- clears carriesLiveImage.
			//   * User entries (reference-image attachments) via
			//     RewriteElidedUserImages(count) -- clears liveUserImageCount.
			// Rewriting is legal ONLY because both entry kinds are
			// loop-generated; assistant entries carry the byte-preservation
			// contract and are never touched (their flags are always 0).
			for( std::size_t i = 0; i < mTranscript.size(); ++i ) {
				ChatTranscriptEntry& e = mTranscript[i];
				if( e.role == ChatTranscriptEntry::Role::ToolResults && e.carriesLiveImage ) {
					const std::size_t beforeBytes = e.rawJson.size();
					e.rawJson = mCodec->RewriteElidedImages( e.rawJson );
					e.carriesLiveImage = false;
					// The entry's live image is gone -- its whole tracked
					// payload goes with it (see EstimateContextTokens).
					e.imageContentBytes = 0;
					// ELISION CONSISTENCY: see the identical call in
					// FlushPendingToolResults' older-entry retention pass.
					ElideSummaryImages( e.toolSummaries );
					if( mRecorder ) {
						EnsureSessionRecordEmitted();
						TrajectoryHistoryEditRecord h;
						h.entryIndex = static_cast<int>( i );
						h.beforeBytes = static_cast<long long>( beforeBytes );
						h.afterBytes = static_cast<long long>( e.rawJson.size() );
						h.reason = "multimodal_tool_image_elision";
						mRecorder->EmitHistoryEdit( h );
					}
				}
				else if( e.role == ChatTranscriptEntry::Role::User && e.liveUserImageCount > 0 ) {
					const std::size_t beforeBytes = e.rawJson.size();
					e.rawJson = mCodec->RewriteElidedUserImages( e.rawJson, e.liveUserImageCount );
					e.liveUserImageCount = 0;
					// All live user images in this entry are gone -- the
					// tracked payload goes to 0 too (see
					// EstimateContextTokens); ElideAllLiveImages sweeps
					// every image in the entry at once, unlike the partial
					// cap-enforcement elision in AddUserMessage.
					e.imageContentBytes = 0;
					if( mRecorder ) {
						EnsureSessionRecordEmitted();
						TrajectoryHistoryEditRecord h;
						h.entryIndex = static_cast<int>( i );
						h.beforeBytes = static_cast<long long>( beforeBytes );
						h.afterBytes = static_cast<long long>( e.rawJson.size() );
						h.reason = "multimodal_user_image_elision";
						mRecorder->EmitHistoryEdit( h );
					}
				}
			}
		}

		void AgentChatLoop::CompactTranscript()
		{
			// Context-compaction slice S2 (design doc 71 §7): the structural
			// span-dropper.  Deterministic pure function of (mTranscript, the
			// budget, the S1 estimator) -- no LLM call, replay-safe.

			// No-op unless a valid budget window is set and we're over the
			// high-water mark.  A disabled (0) or inverted (low >= high)
			// window never compacts -- ContextBudgetActive() is the SAME
			// guard WouldCompactNow() uses, so the two never disagree.
			if( !ContextBudgetActive() ) return;
			if( EstimateContextTokens() < mContextBudgetHighTokens ) return;

			// A "span" is a maximal run beginning at a Role::User entry, up
			// to (not including) the next Role::User entry.  The transcript
			// always starts with a User entry (wire invariant 1), so the
			// scan below always finds spanStarts[0] == 0.  Dropping WHOLE
			// spans from the FRONT keeps mTranscript[0] a User entry and
			// never orphans a tool_result from its preceding tool_use.
			//
			// Role::DriverNote is NOT a span boundary -- it is a message the
			// loop injected mid-round, not a turn the user took, so it stays
			// with the span it belongs to.  That is also what keeps the
			// "mTranscript[0] is a real user message" guarantee above literal:
			// the erase boundary can only ever land on a Role::User entry.
			const std::size_t beforeEstimate = EstimateContextTokens();
			bool dropped = false;

			// Local helper: index of the next span start after index 0, or
			// the transcript size when only one span remains.  (Recomputed
			// each iteration -- transcripts are small; clarity over cleverness.)
			for( ;; ) {
				// Count spans + find the erase boundary (start of span #1).
				std::size_t spanCount = 0;
				std::size_t secondSpanStart = mTranscript.size();
				for( std::size_t i = 0; i < mTranscript.size(); ++i ) {
					if( mTranscript[i].role == ChatTranscriptEntry::Role::User ) {
						++spanCount;
						if( spanCount == 2 ) secondSpanStart = i;
					}
				}

				// Stop when only the floor of trailing spans remains -- a
				// single turn larger than the budget cannot be span-compacted
				// further, so accept it.
				if( spanCount <= static_cast<std::size_t>( kMinRetainedSpans ) ) break;

				// Stop once we're back at (or below) the low-water target.
				if( EstimateContextTokens() <= mContextBudgetLowTokens ) break;

				// Erase the OLDEST whole span: entries [0 .. secondSpanStart).
				mTranscript.erase( mTranscript.begin(),
				                   mTranscript.begin() + static_cast<std::ptrdiff_t>( secondSpanStart ) );
				// A driver that renders the chat straight out of this
				// transcript must be able to TELL the user these turns are
				// gone -- see CompactedEntryCount()'s doc.
				mCompactedEntryCount += secondSpanStart;
				dropped = true;
			}

			// Observability: one history_edit per compaction event (no-op
			// with no trajectory sink attached).  entryIndex -1: not tied to
			// a single transcript entry (a whole-span structural edit).
			if( dropped )
				RecordHistoryEdit( "context_compaction",
				                   static_cast<long long>( beforeEstimate ),
				                   static_cast<long long>( EstimateContextTokens() ), -1 );
		}

		const ChatTranscriptEntry& AgentChatLoop::TranscriptAt( std::size_t i ) const
		{
			// Bounds-safe like JsonValue::at: a static empty entry when out
			// of range (never throws).
			static const ChatTranscriptEntry kEmpty;
			if( i >= mTranscript.size() ) return kEmpty;
			return mTranscript[i];
		}

		//==============================================================
		// Eval-harness E1: trajectory helpers + hooks.
		//==============================================================
		namespace
		{
			//! ChatErrorKind -> the OTel-ish error.type token.
			const char* ErrorKindToString( ChatErrorKind kind )
			{
				switch( kind ) {
					case ChatErrorKind::Http:         return "http";
					case ChatErrorKind::Parse:        return "parse";
					case ChatErrorKind::Provider:     return "provider";
					case ChatErrorKind::Refusal:      return "refusal";
					case ChatErrorKind::MaxTokens:    return "max_tokens";
					case ChatErrorKind::IterationCap: return "iteration_cap";
					case ChatErrorKind::Misuse:       return "misuse";
					case ChatErrorKind::None:
					default:                          return "";
				}
			}

			//! Normalized finish reasons derived from a parsed disposition.
			std::vector<std::string> FinishReasonsForStep( const ChatStepResult& step )
			{
				std::vector<std::string> out;
				if( step.kind == ChatStepResult::Kind::ToolCalls ) {
					out.push_back( "tool_calls" );
				}
				else if( step.kind == ChatStepResult::Kind::FinalText ) {
					out.push_back( "stop" );
				}
				else if( step.errorKind == ChatErrorKind::MaxTokens ) {
					out.push_back( "length" );
				}
				else if( step.errorKind == ChatErrorKind::Refusal ) {
					out.push_back( "content_filter" );
				}
				return out;
			}

			//! Best-effort provider-neutral response-model extraction.
			std::string ExtractResponseModel( const std::string& rawBody )
			{
				JsonValue root;
				std::string perr;
				if( !JsonParse( rawBody, root, perr ) || !root.isObject() ) return std::string();
				if( root.get( "model" ).isString() ) return root.get( "model" ).asString();
				if( root.get( "modelVersion" ).isString() ) return root.get( "modelVersion" ).asString();
				return std::string();
			}

			//! Extract just the sampling PARAMS from a request body: every
			//! top-level member except the large conversation/tool arrays.
			//! Returns a JSON object literal string.
			std::string ExtractRequestParams( const std::string& body )
			{
				JsonValue root;
				std::string perr;
				if( !JsonParse( body, root, perr ) || !root.isObject() )
					return "{}";
				JsonValue params = JsonValue::MakeObject();
				const std::vector<std::pair<std::string, JsonValue> >& members = root.members();
				for( std::size_t i = 0; i < members.size(); ++i ) {
					const std::string& key = members[i].first;
					if( key == "messages" || key == "input" || key == "contents" ||
					    key == "tools" || key == "instructions" || key == "system" ||
					    key == "systemInstruction" || key == "toolConfig" )
						continue;
					params.set( key, members[i].second );
				}
				return JsonSerialize( params );
			}

			//! Serialize a header list (already auth-stripped) as a JSON
			//! object literal string.
			std::string HeadersToJson(
				const std::vector<std::pair<std::string, std::string> >& headers )
			{
				JsonValue o = JsonValue::MakeObject();
				for( std::size_t i = 0; i < headers.size(); ++i )
					o.set( headers[i].first, JsonValue::MakeString( headers[i].second ) );
				return JsonSerialize( o );
			}
		}

		std::string AgentChatLoop::ComposeSystemPrompt() const
		{
			// GUI STAGE 3 (prompt triage): a non-empty override REPLACES
			// the whole composition -- no base prompt, no skills section.
			// See SetSystemPromptOverride's doc.
			if( !mSystemPromptOverride.empty() ) return mSystemPromptOverride;

			std::string systemPrompt = kSystemPrompt;
			if( !mSkillIndexText.empty() ) {
				systemPrompt += "\n\nAvailable skills:\n";
				systemPrompt += mSkillIndexText;
				// The block above IS the index, so say so.  Measured 2026-07-28:
				// the chat tool description used to open with "Call with NO name
				// first to list the available skills", and models obeyed it --
				// gemini-3.5-flash burned a bare read_skill{} round-trip in 9 of
				// 18 sessions, qwen3.6 in 2 of 2 -- fetching a list they were
				// already holding.  (gpt-5.6-terra ignored it: 0 of 1.)  This
				// line is inside the have-an-index branch, which is what makes
				// the instruction safe to give: when no index was supplied the
				// sentence is absent and the listing form remains the right move.
				systemPrompt += "\nThat list IS the skill index -- call read_skill"
				                " with a NAME directly before scene-authoring tasks."
				                "  Do NOT call it with no arguments to list them"
				                " first; you already have the list.";
			}
			return systemPrompt;
		}

		void AgentChatLoop::EnsureSessionRecordEmitted()
		{
			if( !mRecorder || mSessionEmitted ) return;
			mSessionEmitted = true;   // set first (the emit path never re-enters)

			TrajectorySessionRecord sess;
			sess.provider = mCodec->ProviderName();
			sess.requestModel = mModelId;
			sess.systemPrompt = ComposeSystemPrompt();
			sess.systemPromptHash = TrajectoryHashHex( sess.systemPrompt );
			sess.toolDefsHash = TrajectoryHashHex( ChatToolDefsFingerprint() );
			sess.scenePath = mTrajectoryConfig.scenePath;
			sess.sceneHeadVersion = mTrajectoryConfig.sceneHeadVersion;
			mRecorder->EmitSession( sess );
		}

		void AgentChatLoop::CloseTrajectorySession( const std::string& status )
		{
			if( !mRecorder || !mSessionEmitted ) return;
			mRecorder->EmitSummary( status );
			mSessionEmitted = false;
			// Roll to a fresh trace id on the SAME sink so the next recorded
			// action starts a distinct session (the GUI additionally rotates
			// to a new FILE on a new chat -- see the driver wiring).
			if( mTrajectorySink ) {
				ChatTrajectoryConfig cfg = mTrajectoryConfig;
				cfg.traceId.clear();
				mRecorder.reset( new ChatTrajectoryRecorder( mTrajectorySink, cfg ) );
			}
		}

		void AgentChatLoop::SetTrajectorySink(
			std::function<void(const std::string&)> sink,
			const ChatTrajectoryConfig& config )
		{
			// Replacing an active session closes it with a "replaced" summary
			// so no rollup is lost.
			if( mRecorder && mSessionEmitted )
				mRecorder->EmitSummary( "replaced" );

			mSessionEmitted = false;
			mTrajectorySink = sink;
			mTrajectoryConfig = config;
			mTrajectoryClock = config.clock;   // "" -> the recorder/loop use a real clock

			if( sink ) {
				mRecorder.reset( new ChatTrajectoryRecorder( sink, config ) );
			}
			else {
				// An empty sink DETACHES recording (every hook becomes a no-op).
				mRecorder.reset();
				mTrajectorySink = std::function<void(const std::string&)>();
			}
		}

		void AgentChatLoop::RecordHttpRound(
			const ChatHttpRequest& request, long httpStatus,
			const std::string& rawBody, int64_t elapsedMs, int attempt, int retryOf )
		{
			if( !mRecorder ) return;
			EnsureSessionRecordEmitted();

			// Parse the disposition for finish reasons + error type (a second,
			// independent parse from HandleResponse's -- keeps the two hooks
			// cleanly separated; response bodies are small).
			ChatParsedResponse pr = mCodec->ParseResponse( httpStatus, rawBody );
			ChatUsage usage = mCodec->ParseUsage( rawBody );

			TrajectoryLlmRecord rec;
			rec.requestModel = mModelId;
			rec.responseModel = ExtractResponseModel( rawBody );
			rec.requestParamsJson = ExtractRequestParams( request.body );
			// BELT: strip auth headers by name before the record is built.
			rec.requestHeadersJson = HeadersToJson( StripAuthHeaders( request ).headers );
			rec.httpStatus = httpStatus;
			rec.latencyMs = elapsedMs;
			rec.finishReasons = FinishReasonsForStep( pr.step );
			rec.inputTokens = usage.inputTokens;
			rec.outputTokens = usage.outputTokens;
			rec.cacheReadInputTokens = usage.cacheReadInputTokens;
			// The reasoning/visible SPLIT, not just the total: outputTokens is
			// the total billed generation on every provider, so without this
			// field the split is unrecoverable downstream -- and on the
			// providers whose output counter already includes reasoning
			// (Anthropic, OpenAI) parsing it would otherwise be write-only.
			rec.reasoningOutputTokens = usage.reasoningOutputTokens;
			rec.reasoningClamped = usage.reasoningClamped;
			// The provider's PRE-CLAMP count, so a clamped record keeps the
			// evidence of what it clamped away (the clamp is a normalization;
			// the discarded claim is the diagnostic).  Serialized only when the
			// clamp flag is set -- it is identical to the field above otherwise.
			rec.reasoningOutputTokensReported = usage.reasoningOutputTokensReported;
			if( pr.step.kind == ChatStepResult::Kind::ProviderError )
				rec.errorType = ErrorKindToString( pr.step.errorKind );
			rec.attempt = attempt;
			rec.retryOf = retryOf;
			rec.responseBody = rawBody;
			mRecorder->EmitLlm( rec );
		}

		void AgentChatLoop::RecordHttpRound(
			long httpStatus, const std::string& rawBody, int64_t elapsedMs,
			int attempt, int retryOf )
		{
			if( !mRecorder ) return;
			RecordHttpRound( mLastRequest, httpStatus, rawBody, elapsedMs, attempt, retryOf );
		}

		void AgentChatLoop::RecordAuxiliaryHttpRound(
			const std::string& purpose, const std::string& url,
			const std::string& requestBodySansAuth,
			long httpStatus, const std::string& responseBody, int64_t elapsedMs )
		{
			if( !mRecorder ) return;
			EnsureSessionRecordEmitted();

			TrajectoryLlmRecord rec;
			rec.purpose = purpose;
			// Provider-agnostic best-effort model sniff (a raw JSON parse
			// checking "model"/"modelVersion" -- no mCodec involved, since
			// the auxiliary round's provider need not be this loop's).
			rec.requestModel = ExtractResponseModel( requestBodySansAuth );
			rec.responseModel = ExtractResponseModel( responseBody );
			// Reuse the same "strip the big arrays" helper the main path
			// uses (also provider-agnostic), then fold in the URL -- the
			// ONE piece of forensic context this call carries that the main
			// `llm` record has no field for (e.g. it is what shows which
			// model a Gemini-shaped path-embedded-model URL actually hit).
			JsonValue params;
			std::string perr;
			std::string paramsJson = ExtractRequestParams( requestBodySansAuth );
			if( !JsonParse( paramsJson, params, perr ) || !params.isObject() )
				params = JsonValue::MakeObject();
			params.set( "url", JsonValue::MakeString( url ) );
			rec.requestParamsJson = JsonSerialize( params );
			// No headers parameter exists on this call BY DESIGN -- the
			// driver never hands this loop any header list to strip, so
			// there is nothing to carry (and nothing that could leak).
			rec.requestHeadersJson = "{}";
			rec.httpStatus = httpStatus;
			rec.latencyMs = elapsedMs;
			// Usage/finish-reasons stay at their documented "absent"
			// defaults (-1 / empty) -- this loop's mCodec parses ONE
			// provider's wire shape, and an auxiliary round's provider need
			// not match it; a wrong-codec guess would look authoritative
			// and be silently misleading.  The verbatim response body below
			// is the honest replacement: full forensic detail, no guess.
			rec.attempt = 1;
			rec.retryOf = -1;
			rec.responseBody = responseBody;
			mRecorder->EmitLlm( rec );
		}

		void AgentChatLoop::FinishTrajectory( const std::string& status )
		{
			CloseTrajectorySession( status );
		}

		void AgentChatLoop::RecordHistoryEdit( const std::string& reason, long long beforeBytes,
		                                       long long afterBytes, int entryIndex )
		{
			if( !mRecorder ) return;
			EnsureSessionRecordEmitted();   // keep the `session` record leading, like RecordHttpRound
			TrajectoryHistoryEditRecord rec;
			rec.entryIndex  = entryIndex;
			rec.beforeBytes = beforeBytes;
			rec.afterBytes  = afterBytes;
			rec.reason      = reason;
			mRecorder->EmitHistoryEdit( rec );
		}
	}
}
