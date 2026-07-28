//////////////////////////////////////////////////////////////////////
//
//  AgentChatLoop.h - the provider-agnostic, sans-IO chat-loop core for
//    the LLM scene-editing chat panel (Facet 5, the agentic surface --
//    slice B1).
//
//    OWNS conversation state and translates between LLM provider wire
//    formats (via AgentChatCodecs) and the JSON-RPC verbs the
//    AgentRpcDispatcher speaks -- but performs NO I/O.  (No count here on
//    purpose: the verb set has grown five times and every restated count of
//    it in this family went stale.  AgentRpc.cpp's dispatcher is the list.)
//    The caller (the
//    Swift GUI in slice B2; C++ tests today) drives the loop:
//
//      loop.AddUserMessage(text);
//      for (;;) {
//          ChatHttpRequest req = loop.BuildRequest(apiKey);   // caller does HTTP
//          ChatStepResult  st  = loop.HandleResponse(status, body);
//          if (st.kind != ToolCalls) break;                   // FinalText / error
//          for (call : st.toolCalls) {
//              line = dispatcher.HandleLine(loop.ToolCallToJsonRpcLine(call, id++));
//              loop.AddToolResult(call, line);                // packed per-provider
//          }
//      }
//
//    CONTRACT / KEY RULES:
//      * SWITCHING PROVIDER RESETS THE TRANSCRIPT.  Assistant turns are
//        stored as RAW provider-native JSON (echoed verbatim on later
//        requests -- thinking blocks with signatures must round-trip
//        unmodified), so they CANNOT cross providers.
//      * The API key is passed INTO BuildRequest per call, held nowhere,
//        and appears ONLY in the request's auth header.  Nothing in this
//        module logs request/response bodies or the key.
//      * All tool results for one assistant turn are flushed into ONE
//        user message (AddToolResult buffers; the flush happens when
//        every call of the turn has a result, or at the next
//        BuildRequest / AddUserMessage).  A flush SYNTHESIZES an error
//        tool result ("tool call was not executed...") for every
//        pending call that has no buffered result.  Together with the
//        codecs' record-or-refuse rule (ParseResponse REFUSES -- and
//        the loop records nothing of -- any response whose tool calls
//        could not become the pending set: calls under a non-tool-call
//        stop_reason/finishReason, malformed call blocks, DUPLICATE
//        call ids on either provider, and id-less Anthropic tool_use
//        blocks -- Gemini instead SYNTHESIZES ids for id-less calls;
//        see AgentChatCodecs.h), the wire invariant "every RECORDED
//        tool call is answered in the immediately-following user
//        message" holds for every entry this loop records, on both
//        providers (Anthropic hard-400s unanswered tool_use ids;
//        Gemini rejects mismatched functionResponses).
//      * THE SYSTEM PROMPT IS FIXED FOR A SESSION.  ComposeSystemPrompt()
//        is a pure function of (base prompt, skill index, override), all
//        of which a host sets before the first turn -- so every request
//        of a session carries a BYTE-IDENTICAL system block.  This is
//        load-bearing, not incidental: tools render before system in
//        every provider's cached prefix, so one changed system byte
//        invalidates tools + system + the whole history.  Anything
//        transient the model needs to see rides the CONVERSATION
//        instead, appended after the tool-results entry so no tool
//        result is orphaned -- see AppendPendingBuildNudge, the one
//        such message the loop synthesizes today.  It goes on the wire
//        as ordinary user content but is tagged Role::DriverNote in the
//        transcript so a GUI does not attribute it to the human.
//      * HONEST POISON SCOPING: a user interrupt, tool crash, or
//        hostile response body cannot create RECORDED-BUT-
//        UNANSWERABLE tool calls -- the parse gates plus the flush
//        synthesis guarantee that much.  What byte-preservation CANNOT
//        guarantee is that every recorded echo replays cleanly: an
//        exotic-but-well-formed content the codec does not model
//        (e.g. a server_tool_use block under end_turn) is echoed
//        verbatim and may be invalid on replay.  Reset() /
//        SetProvider() recovers; the GUI driver should offer that on
//        repeated HTTP 400s.
//      * IMAGE RETENTION: only the MOST RECENT tool-result PNG stays
//        live in the transcript -- from read_image, from
//        compare_to_reference, or from a render called with
//        imageMaxEdge (the one-call observe form), all of which
//        ChatToolResultCarriesImage recognizes alike.  A render's
//        image is elided by this rule while its statistics stay
//        live, since the elision replaces only the image block.
//        When a new tool-results entry packs
//        an image, every OLDER ToolResults entry's image block/part is
//        rewritten to a short "[image elided -- superseded by a newer
//        render]" text note (Anthropic: the {type:"image"} element;
//        Gemini: the functionResponse.parts inlineData) via the
//        codec's RewriteElidedImages -- which also rewrites the loop-
//        written "the PNG is attached ..." note so the model never
//        sees contradictory context.  The rule is entry-internal too:
//        when ONE flush packs several image results, PackToolResults
//        keeps only the LAST live (earlier ones are packed pre-
//        elided), so exactly one image is live globally.  Without all
//        this, every request re-sends (and re-bills) every historical
//        ~1.3MB base64 PNG and long sessions eventually exceed
//        provider request-size limits.  Rewriting is legal ONLY for
//        ToolResults entries -- they are loop-generated; assistant
//        entries keep the verbatim byte-preservation contract and are
//        never touched.
//      * SUPERSEDED-READ RETENTION: the TEXT-side sibling of the
//        IMAGE RETENTION rule above.  For a read verb whose result is
//        the WHOLE view of one piece of MUTABLE state, only the MOST
//        RECENT result stays live; every older one is rewritten to a
//        short "[<verb> result elided -- superseded by a later <verb>
//        call ...]" note via the codec's RewriteElidedToolResults.
//        The allowlist is ChatToolResultSupersessionKey's (today:
//        read_document ONLY -- that function's doc states the four
//        admission properties and gives, per excluded verb, either the
//        property it fails or the other reason it is off the list).
//        MEASURED MOTIVATION (trajectory 20260727T063526Z-a7ee472c):
//        one GUI session ("make the middle object red") called
//        read_document SIX times, each returning a 19.8 KB document
//        (a ~21.5 KB JSON-RPC response), every copy live forever --
//        the request grew from 10 K to 68 K PROVIDER-REPORTED input
//        tokens, and the edit it was checking was a THREE-parameter
//        patch (three propose_patch calls of ~183 bytes each).
//        It is a CORRECTNESS improvement as much as a cost one, though
//        state that precisely: FIVE of the six documents were BYTE-
//        IDENTICAL (19,824 B) -- pure redundancy -- and the sixth
//        differed (19,828 B), i.e. the head DID move under the agent
//        mid-task, which is what makes an older copy potentially STALE
//        rather than merely wasteful.  Like IMAGE RETENTION the rule is
//        entry-internal too (one flush packing two read_documents
//        keeps only the last live), it rewrites ONLY loop-generated
//        ToolResults entries, and it is a deterministic pure function
//        of the transcript -- so a replayed trajectory elides
//        identically.  UNLIKE span compaction it is UNCONDITIONAL: there
//        is no budget to enable, so it applies to EVERY host including
//        the eval runner.  That is deliberate (it is a correctness rule
//        about stale reads, not a cost knob), but it does mean the
//        pre-existing eval baseline in docs/agentic-redesign/
//        70-agent-eval-harness.md predates it and is no longer a
//        like-for-like comparison.  It does NOT touch the display-layer
//        ChatToolDisplaySummary::resultJson: unlike an image blob that
//        one is capped at 8 KB, never rides the wire, and is the GUI's
//        record of what the agent actually saw at that step.
//      * USER IMAGE RETENTION (Model-B F5 chat image attachments): a
//        SEPARATE, INDEPENDENT policy from the tool-result IMAGE
//        RETENTION rule above -- user reference images are the
//        modeling TARGET (the agent keeps comparing against them
//        across turns), not a disposable render preview, so "keep only
//        the newest" is the wrong rule here.  Instead: up to
//        kMaxLiveUserImages (4) user-attached images stay live
//        UN-ELIDED across the WHOLE conversation.  AddUserMessage's
//        attachments overload elides the OLDEST already-live user
//        image(s) -- across every earlier User entry, oldest entry
//        first, oldest attachment within an entry first -- just enough
//        to make room so the running live-image count never exceeds
//        the cap once the new message's own images are added (an
//        attachment that itself would still exceed the cap alone is
//        simply added anyway -- the cap bounds STEADY-STATE growth, it
//        never refuses an attach).  An elided attachment is replaced
//        with the text placeholder "[reference image elided --
//        re-attach if needed]" via the codec's RewriteElidedUserImages.
//        Rewriting is legal ONLY for User entries this loop generated
//        via MakeUserEntry; assistant entries are never touched.  The
//        cap bounds per-turn token cost (a 1024px-edge JPEG runs
//        ~100-400KB base64, re-sent on EVERY later request since the
//        whole history replays) while keeping enough recent references
//        live for a multi-turn modeling session to compare against.
//      * CALLER-CONTRACT GUARDS (each refuses or ignores; none throw):
//          - HandleResponse while the previous turn's tool calls are
//            still pending returns ProviderError and records NOTHING
//            (a legitimate response can only follow a BuildRequest,
//            which flushes the pending set first).
//          - AddToolResult for a call id that is NOT pending, or a
//            SECOND result for an already-answered id, is IGNORED
//            (first result wins; results are accepted only for the
//            current assistant turn's pending calls).
//          - BuildRequest on an empty transcript returns an EMPTY
//            request (url == "") that the caller must not send.
//      * ITERATION CAP: at most kMaxToolRoundsPerTurn (20) tool rounds
//        per conversation-turn.  The 21st tool-call response within one
//        user turn returns ProviderError("iteration cap...") WITHOUT
//        recording the turn, so a GUI loop cannot spin.  The counter
//        resets on AddUserMessage.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_AGENT_AGENTCHATLOOP_
#define RISE_AGENT_AGENTCHATLOOP_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "AgentChatCodecs.h"
#include "ChatTrajectory.h"

namespace RISE
{
	namespace Agent
	{
		//! The selectable providers (each backed by a codec).  XAI and
		//! Local both reuse the OpenAIChatCodec (OpenAI-Chat-Completions-
		//! compatible wire shape) with a different Config -- see MakeCodec.
		enum class ChatProvider
		{
			Anthropic,
			Gemini,
			OpenAI,
			XAI,
			Local
		};

		//! One tool call's display summary, held on a ToolResults
		//! ChatTranscriptEntry (see ChatTranscriptEntry::toolSummaries).
		//! DISPLAY-ONLY -- see the CRITICAL INVARIANT block on
		//! ChatTranscriptEntry::reasoningText below; none of these fields
		//! are read by BuildRequest or EstimateContextTokens.
		struct ChatToolDisplaySummary
		{
			std::string name;         //!< the tool name (a JSON-RPC verb), e.g. "insert_chunks"
			std::string outcomeLine;  //!< the ToolOutcomeLine one-liner for this call (see AgentChatLoop.cpp)
			std::string argsJson;     //!< the call's arguments, as sent (ChatToolCall::argsJson)

			//! The raw JSON-RPC response ENVELOPE line AddToolResult
			//! recorded for this call, CAPPED at 8 KB with a
			//! "...[truncated]" suffix when longer -- a full read_image
			//! result can carry a large base64 PNG, and this field exists
			//! for a GUI's "show me the raw result" affordance, not to
			//! duplicate the wire payload.  ELISION-TRACKING: when
			//! `carriesImage` is true and a NEWER image supersedes this
			//! summary's call (the same IMAGE RETENTION rule that rewrites
			//! the owning entry's rawJson -- see the file header), this
			//! field is overwritten with the fixed placeholder
			//! "[image elided -- superseded by a newer render]" and
			//! `carriesImage` is cleared, at BOTH elision call sites
			//! (FlushPendingToolResults' older-entry rewrite pass and
			//! ElideAllLiveImages).  Without this, a summary would keep
			//! serving a STALE base64 blob after its owning entry's rawJson
			//! was already elided -- exactly the retained memory / stale
			//! data the elision exists to avoid.
			std::string resultJson;

			//! True iff `resultJson` currently holds a LIVE base64 image
			//! result (i.e. ChatToolResultCarriesImage was true for
			//! this call at flush time) -- mirrors
			//! ChatTranscriptEntry::carriesLiveImage but scoped to this ONE
			//! summary rather than the whole entry (an entry can pack
			//! several calls; only the image-bearing one(s) need tracking).
			//! Cleared alongside the resultJson rewrite described above.
			bool        carriesImage = false;
		};

		//! SUPERSEDED-READ RETENTION bookkeeping for ONE packed tool
		//! result of a ToolResults entry (see the file header).
		//!
		//! WIRE-BEARING, deliberately SEPARATE from the display-only
		//! ChatToolDisplaySummary next to it: this vector's INDICES are
		//! what the codec's RewriteElidedToolResults addresses, so it
		//! must not be conflated with a field the CRITICAL INVARIANT
		//! block below declares nothing on the wire path may read.
		struct ChatToolResultSlot
		{
			//! ChatToolResultSupersessionKey's verdict for this result at
			//! pack time -- "" when the result is not supersedable (the
			//! overwhelmingly common case: every verb off the allowlist,
			//! and every error envelope).
			std::string supersessionKey;

			//! False once this result's payload has been rewritten to the
			//! superseded-result placeholder, so the pass is idempotent
			//! and a second sweep re-elides nothing.
			bool        live = true;
		};

		//! One transcript entry as the GUI sees it.  `rawJson` is the
		//! provider-native message this entry contributes to BuildRequest;
		//! `displayText` is the human-readable extraction.
		struct ChatTranscriptEntry
		{
			enum class Role
			{
				User,         //!< a user text message
				Assistant,    //!< a model turn (text and/or tool calls)
				ToolResults,  //!< the packed tool results of one model turn

				//! A note the LOOP ITSELF injected into the conversation --
				//! today only the blind-edit nudge (AppendPendingBuildNudge);
				//! the human never typed it.
				//!
				//! ON THE WIRE IT IS INDISTINGUISHABLE FROM Role::User, AND
				//! THAT IS THE POINT.  rawJson is built by the SAME
				//! mCodec->MakeUserEntry() a real user message uses, so every
				//! codec emits an ordinary user message and no provider sees a
				//! role it does not model.  BuildRequest never reads `role` at
				//! all -- it assembles `rawEntries` from rawJson alone -- so
				//! adding this enumerator cannot change one byte of any
				//! request (T44 asserts exactly that, per provider).
				//!
				//! WHAT IT IS FOR: the two GUIs.  A driver that renders
				//! straight out of this transcript (the Windows ChatPanel)
				//! would otherwise paint a loop-synthesized reminder as the
				//! USER's own chat bubble, which is a lie about who said it.
				//! Both drivers render this role as a system-notice row (dim,
				//! centered) -- the affordance they already use for the
				//! compaction notice.  Do NOT silently drop it: the model DID
				//! receive it, and a reminder that visibly steered the agent
				//! must be visible to the person watching.
				//!
				//! COMPACTION: deliberately NOT a span boundary (spans start
				//! at Role::User).  A driver note is not a user turn -- the
				//! trajectory already records it as a `history_edit` rather
				//! than a `user` record for the same reason -- so it stays
				//! attached to the span it was injected into instead of
				//! splitting it.
				DriverNote
			};

			Role        role = Role::User;
			std::string displayText;
			std::string rawJson;   //!< provider-native message JSON (verbatim for assistant turns)

			//! True while this ToolResults entry still carries a live
			//! image block/part.  Cleared when a NEWER image supersedes
			//! it and rawJson is rewritten with the image elided (see the
			//! IMAGE RETENTION rule in the file header).  Always false
			//! for User/Assistant entries.
			bool        carriesLiveImage = false;

			//! Number of still-LIVE user-attached reference images this
			//! User entry carries (see USER IMAGE RETENTION in the file
			//! header) -- a SEPARATE counter from carriesLiveImage, which
			//! only ever applies to ToolResults entries.  Decremented (via
			//! the codec's RewriteElidedUserImages) as the running cap
			//! elides the oldest live attachments first.  Always 0 for
			//! Assistant/ToolResults entries and for a User entry with no
			//! attachments.
			int         liveUserImageCount = 0;

			//! Total base64 image-payload bytes of the CURRENTLY-LIVE
			//! images in this entry, EXCLUDED from the text-proxy estimate
			//! (images bill by area, not byte length -- see
			//! EstimateContextTokens).  Maintained wherever carriesLiveImage
			//! / liveUserImageCount change.  0 for entries with no live image.
			std::size_t imageContentBytes = 0;

			//! ======================================================
			//! DISPLAY-LAYER ENRICHMENT (regression fix, see below) --
			//! reasoningText and toolSummaries/ChatToolDisplaySummary.
			//!
			//! CRITICAL INVARIANT, stated loudly because it is the entire
			//! point of these fields existing as SEPARATE members rather
			//! than being folded into displayText or rawJson: they are
			//! DISPLAY-ONLY.
			//!   * rawJson (the wire echo) is NEVER touched by anything
			//!     that populates these fields -- BuildRequest reads ONLY
			//!     mTranscript[i].rawJson when assembling `rawEntries`
			//!     (AgentChatLoop.cpp, the `for` loop right after the
			//!     empty-transcript / CompactTranscript guards in
			//!     BuildRequest), so BuildRequest's output is BYTE-
			//!     IDENTICAL to before this change -- reasoningText and
			//!     toolSummaries are simply never read on that path.
			//!   * Nothing in this struct is EVER serialized back to a
			//!     provider.  A provider's own reasoning representation
			//!     (an Anthropic thinking block with its signature, an
			//!     OpenAI-family message.reasoning / reasoning_content
			//!     field) already rides in rawJson verbatim, byte-
			//!     preserved, independently of reasoningText -- these
			//!     fields are a PARALLEL extraction for the GUI to render,
			//!     not a replacement or a second source of truth for the
			//!     wire.
			//!   * EstimateContextTokens (the context-compaction budget
			//!     estimator) reads ONLY ComposeSystemPrompt(),
			//!     mCodec->ToolsWireBytes(), and per-entry rawJson /
			//!     carriesLiveImage / liveUserImageCount / imageContentBytes
			//!     -- it never reads displayText, reasoningText, or
			//!     toolSummaries, so populating these fields does not
			//!     perturb the compaction estimate or trigger point by even
			//!     one byte.
			//!
			//! SCOPE: this invariant covers reasoningText and toolSummaries
			//! ONLY.  `toolResultSlots`, declared immediately after
			//! toolSummaries below, is deliberately NOT part of the family
			//! -- it is WIRE-BEARING (its indices drive the codec's
			//! RewriteElidedToolResults rewrite of rawJson).  Do not fold
			//! the two together, and do not "simplify" the elision to read
			//! toolSummaries instead: that would make a display field
			//! load-bearing for the wire, which is exactly what this block
			//! forbids.
			//! ======================================================

			//! Assistant entries only: the model's reasoning/thinking text
			//! for this turn, extracted by the codec from whichever
			//! provider-specific field carries it (Anthropic `thinking`
			//! content blocks; OpenAI-family `message.reasoning` (Ollama)
			//! or `message.reasoning_content` (xAI)) -- see
			//! ChatStepResult::reasoningText's doc for the full per-
			//! provider rule.  "" when the provider exposes no reasoning
			//! for this turn (every Gemini turn; a plain gpt-family turn;
			//! any ToolResults/User entry).
			std::string reasoningText;

			//! ToolResults entries only: a per-call display summary, one
			//! per element of `ordered` in FlushPendingToolResults
			//! (pending-call order, synthesized-error results included) --
			//! see ChatToolDisplaySummary's doc.  Empty for User/Assistant
			//! entries.
			std::vector<ChatToolDisplaySummary> toolSummaries;

			//! ToolResults entries only: SUPERSEDED-READ RETENTION state,
			//! one element per packed tool result, in the SAME order
			//! PackToolResults wrote them (so index i here IS index i for
			//! the codec's RewriteElidedToolResults).  Parallel to
			//! toolSummaries in length and order, but NOT display-only:
			//! see ChatToolResultSlot's doc.  Empty for User/Assistant
			//! entries.
			std::vector<ChatToolResultSlot> toolResultSlots;
		};

		//! The sans-IO chat loop (see the file header for the contract).
		//! Single-threaded, like the rest of the Agent module.
		class AgentChatLoop
		{
		public:
			//! Tool rounds allowed per conversation-turn before the loop
			//! refuses with ProviderError("iteration cap...").  Round N
			//! (N <= cap) succeeds; round cap+1 trips.  This is the
			//! HOSTED default; a host with its OWN honest budget
			//! enforcement (the eval runner's per-scenario
			//! maxToolCalls/maxLlmCalls) may RAISE the instance cap via
			//! SetMaxToolRoundsPerTurn so a legitimately iterative
			//! single-turn task is stopped by its budgets, not preempted
			//! by this anti-spin backstop.
			//!
			//! Raised 20 -> 100 (2026-07-20).  20 was preempting real work,
			//! not runaway loops: a GUI session asked qwen3.6:27b to build a
			//! furnished room around a hero object and it died at round 21
			//! still inserting chunks, having never once rendered.  A
			//! build-a-whole-scene turn legitimately needs dozens of rounds
			//! (one insert_chunk per chunk, plus render-inspect cycles), so
			//! a cap that low turns a capability question into a harness
			//! artefact.  This is an ANTI-SPIN backstop, not a budget --
			//! cost control belongs in the caller's budget, and for a local
			//! model there is no cost at all.
			static const int kMaxToolRoundsPerTurn = 100;

			//! The same backstop for a LOCAL (self-hosted) provider, where
			//! a round costs nothing but wall time.  Set far higher so the
			//! only thing it stops is a genuine runaway: an iterative
			//! scene build on a local model is exactly the workload we WANT
			//! to run long, and re-running it is free.  Applied
			//! automatically by SetProvider (see mToolRoundsCapExplicit)
			//! unless the host has already pinned a cap of its own.
			static const int kMaxToolRoundsPerTurnLocal = 1000;

			//! Override the per-turn tool-round cap for THIS loop instance
			//! (values < 1 are ignored; the default is
			//! kMaxToolRoundsPerTurn).  Intended for hosts that enforce
			//! their own per-run budgets -- the cap should sit AT or ABOVE
			//! the host's budget so the budget (an honest, accounted stop)
			//! fires first and this cap remains a pure runaway backstop.
			//! Pinning a cap here also marks it EXPLICIT, so a later
			//! SetProvider (a GUI model switch, say) will not silently
			//! replace a host's deliberate cap with the provider default.
			void SetMaxToolRoundsPerTurn( int cap )
			{
				if( cap >= 1 ) {
					mMaxToolRoundsPerTurn = cap;
					mToolRoundsCapExplicit = true;
				}
			}

			//! The cap currently in force -- the provider default that
			//! SetProvider installed, or a host's explicit pin.  Exposed so
			//! a host (or a test) can report/verify which posture is active
			//! rather than re-deriving it from the provider.
			int MaxToolRoundsPerTurn() const { return mMaxToolRoundsPerTurn; }

			//! Default "blind-edit" nudge threshold: after this many
			//! consecutive calls to one of the 5 DOCUMENT-MUTATING tools
			//! (insert_chunk / insert_chunks / propose_patch /
			//! propose_patches / remove_chunk) with NO intervening VISUAL
			//! observation (render / read_image / read_viewport /
			//! query_object_at), the loop appends a reminder MESSAGE to the
			//! conversation telling the model to render and look at its work.
			//! Delivered as conversation content, NOT in the system prompt --
			//! the system prompt must stay byte-identical across a session or
			//! every provider's prompt cache is invalidated (see
			//! AppendPendingBuildNudge).  Chosen from a measured failure: a local
			//! model asked to build a furnished scene inserted 70-100+ chunks
			//! and NEVER rendered once, building entirely blind until it
			//! exhausted its budget.  This is a general drive-loop nudge (like
			//! an editor reminding you to save), NOT an eval-specific hack and
			//! NOT a budget -- it never blocks, it only reminds.
			static const int kDefaultBlindEditNudgeThreshold = 10;

			//! Configure the blind-edit nudge (see kDefaultBlindEditNudgeThreshold).
			//! `threshold <= 0` disables it entirely.  Default is enabled at
			//! the default threshold.  A host that wants a different cadence
			//! (or none -- e.g. a purely non-visual editing session) sets it here.
			void SetBlindEditNudgeThreshold( int threshold ) { mBlindEditNudgeThreshold = threshold; }

			//! The active blind-edit nudge threshold (0 = disabled).  Exposed
			//! for hosts/tests to report or assert the posture.
			int BlindEditNudgeThreshold() const { return mBlindEditNudgeThreshold; }

			//! Maximum user-attached reference images kept LIVE (un-
			//! elided) across the WHOLE conversation -- see USER IMAGE
			//! RETENTION in the file header.  Attaching beyond this cap
			//! elides the oldest live one(s) first.
			static const int kMaxLiveUserImages = 4;

			//! Context-compaction slice S2: the FLOOR on how many trailing
			//! spans CompactTranscript will ever leave in place.  A "span"
			//! is a maximal run beginning at a Role::User entry, up to (not
			//! including) the next Role::User entry (a User entry plus its
			//! zero-or-more Assistant+ToolResults rounds).  Compaction drops
			//! whole spans from the FRONT and never shrinks the transcript
			//! below this many trailing spans -- the previous + current
			//! conversation turn stay verbatim so a single over-budget turn
			//! is accepted rather than mangled.
			static const int kMinRetainedSpans = 2;

			//! The compaction budget BOTH GUI drivers install at startup
			//! (Mac: RISEAgentChatBridge's init; Windows: ChatPanel's
			//! constructor).  Kept here rather than in each driver so the
			//! two cannot drift.  Compaction is still OFF by default for
			//! any other host -- SetContextBudget is what turns it on, and
			//! the eval runner deliberately does not call it (a scenario's
			//! honest per-run budgets, not a silent span drop, are what
			//! should stop an eval).
			//!
			//! WHAT THIS KNOB CAN AND CANNOT REACH -- read this before
			//! re-tuning the numbers, because it bounds their value.
			//! CompactTranscript drops whole SPANS, a span starts at a User
			//! entry, and it stops while `spanCount <= kMinRetainedSpans` (2)
			//! -- so it can only ever fire in a conversation of THREE OR MORE
			//! user turns.  MEASURED over the whole recorded GUI corpus (27
			//! trajectories): 20 sessions have ONE user turn, 6 have two, and
			//! exactly ONE has three.  The dominant shape is "one instruction,
			//! then 13-48 tool rounds", which span compaction is structurally
			//! blind to at ANY budget -- that shape is what SUPERSEDED-READ
			//! RETENTION addresses instead.  So this is an honest backstop for
			//! long MULTI-TURN sessions and nothing more; do not describe it
			//! as catching the single-turn runaways in that corpus, because it
			//! cannot.  (An earlier revision of this comment claimed exactly
			//! that about the 141 K session below.  That session has ONE user
			//! turn: spanCount == 1, so the loop breaks before erasing
			//! anything.  Recorded because it is the same single-span error
			//! this file already documents for a different trajectory.)
			//!
			//! THE NUMBERS.  150 K high / 75 K low, in the units
			//! EstimateContextTokens produces (a chars/4 text proxy plus a
			//! flat per-image charge -- NOT a provider's own count; see the
			//! calibration caveat below).
			//!   * HIGH must sit above a healthy session of the shape this
			//!     mechanism can act on.  Max provider-reported input, by
			//!     session, over the 27-trajectory corpus:
			//!       141 K  20260710T191019Z-95c9935e   1 user turn  (unreachable)
			//!        98 K  20260728T042751Z-6c02328e   3 user turns (REACHABLE)
			//!        77 K  20260728T044129Z-781468ac   1 user turn  (unreachable)
			//!        74 K  20260728T043424Z-d02506e9   1 user turn  (unreachable)
			//!        68 K  20260727T063526Z-a7ee472c   1 user turn  (unreachable)
			//!     The only COMPACTABLE session on record peaks at ~98 K, so
			//!     150 K is ~1.5x over the largest healthy session this
			//!     mechanism could actually mangle.  It is NOT chosen to sit
			//!     under any provider's window (see the caveat), and it is
			//!     NOT a claim that no session exceeds it.
			//!   * LOW at half the high means one compaction event does
			//!     real work instead of re-triggering on the very next
			//!     turn; kMinRetainedSpans still floors it at the previous
			//!     + current turn, so a single over-budget turn is
			//!     accepted whole rather than mangled.
			//!   * THIN-MARGIN DISCLOSURE: 1.5x is not generous, and the
			//!     chars/4 proxy is not the provider's tokenizer, so a
			//!     healthy long multi-turn session COULD cross this.  When it
			//!     does the user is told (CompactedEntryCount, surfaced by
			//!     both drivers) rather than silently losing history -- that
			//!     notice, not the margin, is what makes the posture safe.
			//! CALIBRATION CAVEAT: the two quantities above are NOT the same
			//! unit.  EstimateContextTokens is a chars/4 proxy over the
			//! request bytes; a provider's tokenizer is not.  The proxy is
			//! deliberately used only as a monotone growth signal, and the
			//! headroom factor is what absorbs the mismatch -- so this pair
			//! is a BACKSTOP against unbounded growth, not a figure tuned to
			//! any provider's context window.  A host that knows its model's
			//! real window (and wants compaction to track it) should call
			//! SetContextBudget with its own numbers rather than these.
			//! DISCLOSED LIMITATION: for that reason this does nothing for a
			//! small-window LOCAL model (an Ollama server may serve 8 K-32 K,
			//! which this budget never reaches) -- the loop cannot know the
			//! served model's window, and such servers truncate server-side.
			//! Sizing for the smallest local window instead would compact
			//! aggressively on every hosted session, which is the worse
			//! trade.  Worth stating plainly because Local IS the DEFAULT
			//! provider in both GUI drivers: on the out-of-the-box
			//! configuration, against a server with a small num_ctx, this
			//! budget is effectively inert.
			static const std::size_t kDefaultContextBudgetHighTokens = 150000;
			static const std::size_t kDefaultContextBudgetLowTokens  = 75000;

			//! Constructs with the ChatGPT/OpenAI provider + its default model.
			AgentChatLoop();
			~AgentChatLoop();

			//! Clear the transcript, pending tool state, and the round
			//! counter.  The provider/model selection is kept.
			void Reset();

			//! Select the provider (and optionally a model id; empty =
			//! the provider's default).  RESETS THE TRANSCRIPT (assistant
			//! turns are provider-native and cannot cross providers) --
			//! even when re-selecting the current provider, for a uniform
			//! rule the GUI can state plainly.
			void SetProvider( ChatProvider provider, const std::string& modelId = std::string() );

			ChatProvider       Provider() const { return mProvider; }
			const std::string& ModelId() const  { return mModelId; }

			//! Append a user text message and reset the per-turn tool-round
			//! counter.  Any pending tool calls are flushed first (they
			//! belong to the previous assistant turn; unanswered ones get
			//! synthesized error results so the wire stays valid).
			//! EMPTY or whitespace-only text is a documented NO-OP
			//! (Anthropic hard-400s an empty text block): nothing is
			//! appended, flushed, or reset.
			void AddUserMessage( const std::string& text );

			//! Append a user message carrying reference-image attachments
			//! (Model-B F5 chat image attachments) alongside the text --
			//! see USER IMAGE RETENTION in the file header for the
			//! persistence + cap policy.  `text` may be empty when
			//! `attachments` is non-empty (an attachment-only message);
			//! the documented NO-OP rule above still holds when BOTH are
			//! empty.  After appending, elides the oldest live user
			//! image(s) across the transcript so the running live count
			//! never exceeds kMaxLiveUserImages.
			void AddUserMessage( const std::string& text,
			                     const std::vector<ChatAttachment>& attachments );

			//! Build the next HTTP request for the caller to perform.
			//! `apiKey` is forwarded to the codec for the auth header only
			//! -- it is not stored.  Pending tool calls are flushed into
			//! the transcript first (unanswered ones get synthesized error
			//! results).  If the transcript is empty there is nothing to
			//! send: the returned request is EMPTY (url == "") and must
			//! not be performed.
			ChatHttpRequest BuildRequest( const std::string& apiKey );

			//! Feed back the raw HTTP response (status + body).  On
			//! ToolCalls the assistant turn is recorded and the calls
			//! become the pending set AddToolResult answers; on FinalText
			//! the turn is recorded and the loop is idle; on ProviderError
			//! nothing is recorded.  Enforces the iteration cap.  REFUSED
			//! (ProviderError, nothing recorded) while the previous turn's
			//! tool calls are still pending -- resolve them via
			//! AddToolResult, or let BuildRequest / AddUserMessage flush
			//! them with synthesized error results.
			ChatStepResult HandleResponse( long httpStatus, const std::string& rawBody );

			//! Translate one tool call into the JSON-RPC request line the
			//! AgentRpcDispatcher consumes:
			//!   {"jsonrpc":"2.0","id":<rpcId>,"method":<name>,"params":<args>}
			//! Malformed argsJson degrades to empty params (for verbs with
			//! REQUIRED params the dispatcher then answers -32602, which
			//! flows back to the model as an error tool result; verbs whose
			//! params are all optional simply execute with their defaults --
			//! self-correcting either way, never throwing).
			//! NOTE (Eval-harness E1): NON-const.  When a trajectory sink is
			//! attached this STAMPS the tool call's dispatch start time and
			//! the JSON-RPC line, keyed by call id, so AddToolResult can
			//! complete the `tool` trajectory record with a measured latency.
			//! With no sink attached it is a pure translation (the stamp is
			//! skipped) -- behaviour is byte-identical to the pre-E1 const
			//! version.
			std::string ToolCallToJsonRpcLine( const ChatToolCall& call, int rpcId );

			//! Record the raw JSON-RPC response ENVELOPE line for one of
			//! the pending calls.  Results are buffered and packed into
			//! ONE provider-native user message when every pending call
			//! has been answered (or at the next BuildRequest /
			//! AddUserMessage, which synthesize error results for any
			//! still-unanswered calls).  A result whose call id is not in
			//! the pending set, or a second result for an already-answered
			//! id, is IGNORED (first result wins).
			void AddToolResult( const ChatToolCall& call, const std::string& rawJsonRpcResponseLine );

			//! Transcript access for the GUI (role + display text; the raw
			//! provider-native JSON rides along).  An out-of-range index
			//! returns a reference to a static EMPTY entry (never throws)
			//! -- the same bounds-safe convention as JsonValue::at.
			std::size_t                TranscriptSize() const { return mTranscript.size(); }
			const ChatTranscriptEntry& TranscriptAt( std::size_t i ) const;

			//! Tool rounds consumed in the current conversation-turn.
			int ToolRoundCount() const { return mToolRounds; }

			//! Pending tool calls awaiting AddToolResult (empty when idle).
			const std::vector<ChatToolCall>& PendingToolCalls() const { return mPendingCalls; }

			//! The static co-editing system prompt sent on every request.
			//! (The BASE prompt only -- when a skill index is set via
			//! SetSkillIndex, BuildRequest appends the skills section to
			//! this base; SystemPrompt() itself never changes.)
			static const char* SystemPrompt();

			//! GUI STAGE 2: the public, declared form of the file-local
			//! ToolOutcomeLine helper in AgentChatLoop.cpp -- lets a caller
			//! OUTSIDE this translation unit (the Mac GUI's ObjC++ bridge)
			//! compute the SAME deterministic one-line outcome summary
			//! (e.g. "160x120, luma 0.19", "3/4 applied") that
			//! FlushPendingToolResults uses for
			//! ChatToolDisplaySummary::outcomeLine -- needed for the async
			//! render-tool path, where the GUI has a call + its raw
			//! JSON-RPC response line before that summary exists in the
			//! transcript.  Pure, stateless, DISPLAY-ONLY (see the
			//! CRITICAL INVARIANT block above): never touches the
			//! transcript or reads/writes any loop state.
			static std::string ToolOutcomeLineForDisplay( const ChatToolCall& call,
			                                               const std::string& rawJsonRpcResponseLine );

			//! Facet 5 slice S1: set the "Available skills" section appended
			//! to the system prompt of every subsequent BuildRequest.
			//! `indexText` is a stable, human-readable rendering of the
			//! read_skill INDEX (one "name -- hook" line per skill); the
			//! DRIVER fetches it ONCE via the read_skill verb at panel init
			//! and passes the rendered text here -- this loop stays sans-IO
			//! (it never reads the skills itself).  An EMPTY string OMITS
			//! the section entirely (the base prompt is sent unchanged).
			//! The setting is provider-neutral config, like the provider /
			//! model selection: it survives Reset() and SetProvider().
			void SetSkillIndex( const std::string& indexText );

			//! GUI STAGE 3 (prompt triage): override the ENTIRE system
			//! prompt for this loop instance.  When `prompt` is non-empty,
			//! ComposeSystemPrompt() returns it VERBATIM -- no base
			//! kSystemPrompt, no "Available skills" section, regardless of
			//! SetSkillIndex.  Passing an EMPTY string clears the override
			//! (ComposeSystemPrompt reverts to the normal base+skills
			//! composition) -- so re-calling with "" is the documented way
			//! back, not Reset()/SetProvider() (see below).
			//!
			//! INTENDED USE: auxiliary, single-purpose loops -- e.g. a
			//! prompt-triage pass that asks a cheap model one narrow
			//! question before the real scene-editing turn -- constructed,
			//! configured ONCE, driven for exactly one exchange, and
			//! discarded.  It is NOT for the scene-editing loop itself:
			//! that loop's identity IS the co-editing base prompt (plus
			//! whatever skills are indexed), and the trajectory session
			//! record stamps whatever ComposeSystemPrompt() returns as
			//! `systemPrompt`/`systemPromptHash` -- an override is
			//! therefore HONESTLY recorded (a session driven under an
			//! override never claims to have run the base prompt), but a
			//! long-lived loop that flips this on and off would make that
			//! record misleading turn-to-turn.
			//!
			//! DELIBERATELY NOT CLEARED by Reset() or SetProvider(): both
			//! are "same loop, fresh conversation" operations (the
			//! provider/model selection and mSkillIndexText survive them
			//! too, for the same reason), and an auxiliary loop is
			//! constructed and configured exactly once before its single
			//! use -- there is no scenario where this loop needs the
			//! override to survive a provider switch and then silently
			//! stop applying.  A caller that truly wants it gone calls this
			//! again with an empty string.
			void SetSystemPromptOverride( const std::string& prompt );

			//==========================================================
			// Context-compaction slice S1: token estimator + budget
			// config (observability only -- see EstimateContextTokens
			// for the formula; NOTHING here changes request bytes or
			// transcript behaviour.  S2 will make BuildRequest act on
			// this).
			//==========================================================

			//! Set the compaction budget, in estimated tokens.
			//! `highWaterTokens` == 0 DISABLES compaction (the default) --
			//! WouldCompactNow() is then always false regardless of
			//! transcript size.  `lowWaterTokens` is the S2 compaction
			//! TARGET (how far a future compaction pass would shrink the
			//! estimate back down); S1 just stores both values verbatim,
			//! with no clamping or validation -- the caller is expected to
			//! pass lowWaterTokens < highWaterTokens.  Provider-neutral
			//! config, like mSkillIndexText: survives Reset() and
			//! SetProvider().
			void SetContextBudget( std::size_t highWaterTokens, std::size_t lowWaterTokens );

			//! Estimate the current request's size in tokens: the system
			//! prompt + tool declarations (fixed prefix) plus the
			//! transcript body, IMAGE-DISCOUNTED (see the .cpp for the
			//! formula and rationale).  Deterministic and pure -- reads
			//! only mCodec/mSkillIndexText/mTranscript.  Cheap enough to
			//! call on every BuildRequest (S2).
			std::size_t EstimateContextTokens() const;

			//! True iff a high-water budget is set AND the current
			//! estimate has reached or exceeded it.  Always false while
			//! the budget is disabled (ContextBudgetHigh() == 0) OR while
			//! the window is otherwise invalid (see ContextBudgetActive) --
			//! this MUST agree with CompactTranscript's own guard, or a
			//! caller could see WouldCompactNow() true while a BuildRequest
			//! silently no-ops the compaction.  S1: observability only --
			//! nothing acts on this yet.
			bool WouldCompactNow() const
			{
				return ContextBudgetActive() &&
				       EstimateContextTokens() >= mContextBudgetHighTokens;
			}

			//! The configured high-water mark, in tokens (0 = disabled).
			std::size_t ContextBudgetHigh() const { return mContextBudgetHighTokens; }

			//! The configured low-water (S2 compaction target), in tokens.
			std::size_t ContextBudgetLow() const { return mContextBudgetLowTokens; }

			//! How many transcript entries CompactTranscript has dropped in
			//! the current conversation (0 until a compaction fires; cleared
			//! by Reset/SetProvider along with the transcript itself).
			//!
			//! LOAD-BEARING FOR THE UI, not mere observability.  A driver that
			//! renders the chat DIRECTLY out of this transcript -- the Windows
			//! ChatPanel does; the Mac ChatViewModel keeps its own display
			//! mirror and does not -- would otherwise have the user's earlier
			//! messages silently VANISH from the panel the first time a long
			//! session crosses the high-water mark.  Such a driver must show a
			//! notice when this is non-zero.  Dropping wire spans is the
			//! intended behaviour; dropping them without telling the user is
			//! not.
			std::size_t CompactedEntryCount() const { return mCompactedEntryCount; }

			//! How many Role::DriverNote entries the loop has injected into
			//! the CURRENT conversation (cleared by Reset/SetProvider along
			//! with the transcript).  Monotone within a conversation, and --
			//! unlike a transcript index -- unperturbed by compaction, which
			//! is why a driver watermarks against this rather than against a
			//! position in mTranscript.
			//!
			//! For the SAME reason CompactedEntryCount() exists: a driver
			//! that keeps its own append-only display list (the Mac
			//! ChatViewModel) never sees the transcript, so without this it
			//! could not tell the user that the loop just told the agent to
			//! go render.  Poll it where the compaction count is polled and
			//! show the delta.  See LastDriverNoteText().
			std::size_t DriverNoteCount() const { return mDriverNoteCount; }

			//! The text of the most recent Role::DriverNote entry ("" until
			//! one fires).  Companion to DriverNoteCount() for the
			//! display-mirror drivers; a driver that renders the transcript
			//! directly reads the entry's displayText instead.
			const std::string& LastDriverNoteText() const { return mLastDriverNoteText; }

			//==========================================================
			// Eval-harness E1: trajectory recording.
			//==========================================================

			//! Attach (or replace) a trajectory sink: every subsequent user
			//! message, LLM round, tool call, and image-elision rewrite is
			//! recorded as a JSONL line handed to `sink`.  `config` supplies
			//! the trace id (empty => generated), an optional epoch-ms clock
			//! (empty => a real clock at the IO edge), and the scene
			//! path/head version captured in the session record.  The
			//! session record is emitted lazily on the first recorded action
			//! (so it always leads, and captures the skill index set by
			//! then).  Passing an EMPTY sink detaches recording.  With no
			//! sink attached EVERY hook below is a cheap no-op -- the loop's
			//! non-recording behaviour is byte-identical to the pre-E1 core.
			//! Replacing an active session emits its summary ("replaced")
			//! first.
			void SetTrajectorySink( std::function<void(const std::string&)> sink,
			                        const ChatTrajectoryConfig& config = ChatTrajectoryConfig() );

			//! Record one LLM HTTP round into the `llm` trajectory record.
			//! Called by the DRIVER just before HandleResponse (the driver
			//! owns the HTTP round-trip, so it alone knows the status/body/
			//! latency).  The response body is stored VERBATIM (the replay
			//! payload); usage/finish-reasons/error are derived from it via
			//! the codec.  AUTH HEADERS ARE STRIPPED from `request` by name
			//! before the record is built (belt); the writer's regex pass is
			//! the suspenders.  `attempt`/`retryOf` capture driver-initiated
			//! retries (retryOf < 0 for a first attempt).  No-op when no sink
			//! is attached.
			void RecordHttpRound( const ChatHttpRequest& request, long httpStatus,
			                      const std::string& rawBody, int64_t elapsedMs,
			                      int attempt = 1, int retryOf = -1 );

			//! Convenience overload using the request most recently returned
			//! by BuildRequest (what the driver actually sent) -- so a GUI
			//! driver need not marshal the request back into C++.  No-op when
			//! no sink is attached or no request has been built yet.
			void RecordHttpRound( long httpStatus, const std::string& rawBody,
			                      int64_t elapsedMs, int attempt = 1, int retryOf = -1 );

			//! Emit the terminal `summary` record with `status` and detach the
			//! current session (a fresh trace id / new session begins on the
			//! next recorded action).  No-op when no session is active.  Use
			//! this at chat end / app close for a clean summary line; a Reset
			//! or SetProvider also closes the session automatically.
			void FinishTrajectory( const std::string& status );

			//! True while a trajectory sink is attached.
			bool TrajectoryActive() const { return mRecorder != nullptr; }

			//! Eval-harness (scenario interventions): emit a `history_edit`
			//! trajectory record for a NON-LLM, NON-tool, history-visible
			//! event that mutated the shared head BETWEEN the model's turns
			//! WITHOUT consuming a turn -- e.g. the eval runner's simulated
			//! co-editor applying a param edit while the agent is mid-task.
			//! `reason` is a short machine tag (e.g. "scenario_intervention");
			//! `beforeBytes`/`afterBytes` record the affected document's byte
			//! size before/after the edit; `entryIndex` is advisory (-1 = not
			//! tied to a transcript entry, the intervention case).  No-op when
			//! no trajectory sink is attached.
			void RecordHistoryEdit( const std::string& reason, long long beforeBytes = 0,
			                        long long afterBytes = 0, int entryIndex = -1 );

		private:
			AgentChatLoop( const AgentChatLoop& );             // deleted
			AgentChatLoop& operator=( const AgentChatLoop& );  // deleted

			//! Pack the pending turn's tool results into one transcript
			//! entry, in pending-call order, SYNTHESIZING an error result
			//! for every pending call that has no buffered result (so the
			//! wire never carries an unanswered tool call).  No-op when
			//! there is no pending turn.
			void FlushPendingToolResults();

			//! BLIND-EDIT NUDGE delivery: when AddToolResult armed one,
			//! append it to the transcript as a User entry and clear the
			//! stash.  No-op when nothing is armed.  Called from
			//! FlushPendingToolResults (both paths) so the nudge lands
			//! immediately AFTER the tool-results entry -- keeping every
			//! tool_use answered by the message that directly follows it --
			//! and so it can never be stranded.  Deliberately NOT folded into
			//! the system prompt: see the rationale block on the definition.
			void AppendPendingBuildNudge();

			//! Compose the full system prompt (base + skills section when a
			//! skill index is set) -- shared by BuildRequest and the session
			//! record so the recorded prompt matches the sent prompt.
			std::string ComposeSystemPrompt() const;

			//! True iff the configured (high, low) budget window is valid
			//! and enabled: high > 0, low > 0, and low < high.  The SINGLE
			//! guard shared by WouldCompactNow() and CompactTranscript() --
			//! before this helper the two had drifted (WouldCompactNow only
			//! checked high > 0), so a caller could see WouldCompactNow()
			//! return true for a budget CompactTranscript treats as invalid
			//! and silently no-ops on.
			bool ContextBudgetActive() const
			{
				return mContextBudgetHighTokens > 0 && mContextBudgetLowTokens > 0 &&
				       mContextBudgetLowTokens < mContextBudgetHighTokens;
			}

			//! TEXT-ONLY-MODEL IMAGE-REJECTION RECOVERY: strip EVERY live
			//! image from the WHOLE transcript in one sweep -- both packed
			//! tool-result images (via the codec's
			//! RewriteElidedImages) and live user reference-image
			//! attachments (via RewriteElidedUserImages), recording a
			//! history_edit for each rewritten entry.  Idempotent (a second
			//! sweep finds nothing live and is a no-op).  Invoked when a
			//! text-only model 400-rejects multimodal content, and again at
			//! the top of every later BuildRequest while mElideAllImages is
			//! set, so once a model proves text-only the conversation never
			//! re-sends an image.
			void ElideAllLiveImages();

			//! SUPERSEDED-READ RETENTION (see the file header): sweep the
			//! WHOLE transcript and, for every supersession key present in
			//! more than one still-live tool-result slot, rewrite all but
			//! the LAST occurrence's payload to
			//! ChatSupersededResultNote(verb) via the codec's
			//! RewriteElidedToolResults.  Handles the cross-entry case (six
			//! read_documents across six turns) and the entry-internal one
			//! (two read_documents packed by ONE flush) with the same pass,
			//! because it scans slots in wire order across all entries.
			//!
			//! Pure function of (mTranscript, the codec) -> deterministic
			//! and replay-safe, and IDEMPOTENT (a second sweep finds every
			//! superseded slot already dead and rewrites nothing).  Records
			//! one "tool_result_supersession" history_edit per rewritten
			//! entry (no-op with no trajectory sink).  Called at the end of
			//! FlushPendingToolResults -- the only place ToolResults entries
			//! are created.
			void ElideSupersededToolResults();

			//! Context-compaction slice S2: the structural span-dropper
			//! (Option C core; design doc 71 §7).  When a valid budget
			//! window is set AND the current EstimateContextTokens() has
			//! reached the high-water mark, erase the OLDEST WHOLE spans one
			//! at a time until the estimate falls to (or below) the low-water
			//! target -- or until only kMinRetainedSpans trailing spans
			//! remain, whichever comes first.  A "span" is a maximal run
			//! beginning at a Role::User entry up to (not including) the next
			//! Role::User entry (Role::DriverNote does NOT start one -- see
			//! its doc); dropping whole spans from the front keeps
			//! mTranscript[0] a User entry (Anthropic requires the first wire
			//! message be user) and never orphans a tool_result from its
			//! tool_use (the wire invariants in the file header).  Pure
			//! function of (mTranscript, the budget, the S1 estimator) ->
			//! deterministic, replay-safe.  Records a "context_compaction"
			//! history_edit (no-op with no trajectory sink) when it drops
			//! anything.  A no-op when the budget is disabled/invalid, when
			//! we are under the high-water, or when <= kMinRetainedSpans
			//! spans remain.  Invoked by BuildRequest after the flush +
			//! image-elision passes and before rawEntries assembly.
			void CompactTranscript();

			//! Emit the `session` record exactly once, on the first recorded
			//! action, so it always leads the trajectory.  No-op with no sink.
			void EnsureSessionRecordEmitted();

			//! Emit the terminal `summary` (when a session is active) and roll
			//! the recorder to a fresh trace id on the same sink, so the next
			//! action starts a new session.  Shared by Reset / SetProvider /
			//! FinishTrajectory.
			void CloseTrajectorySession( const std::string& status );

			//! One in-flight tool dispatch: the JSON-RPC line + start time
			//! stamped by ToolCallToJsonRpcLine, completed at AddToolResult.
			struct ToolLinePending
			{
				std::string id;
				std::string line;
				int64_t     startMs;
			};

			std::unique_ptr<IChatProviderCodec> mCodec;
			ChatProvider                        mProvider;
			std::string                         mModelId;

			//! Facet 5 slice S1: the rendered skills-index text ("" = no
			//! skills section).  Provider-neutral config -- survives Reset()
			//! and SetProvider(), like mProvider/mModelId.
			std::string                         mSkillIndexText;

			//! GUI STAGE 3 (prompt triage): the verbatim system-prompt
			//! override ("" = no override -- see SetSystemPromptOverride).
			//! Provider-neutral config, like mSkillIndexText: NOT cleared
			//! by Reset()/SetProvider().
			std::string                         mSystemPromptOverride;

			//! Context-compaction slice S1: the compaction budget, in
			//! estimated tokens (0 = disabled).  Provider-neutral config,
			//! like mSkillIndexText -- survives Reset() and SetProvider().
			//! See SetContextBudget's doc.
			std::size_t                         mContextBudgetHighTokens;
			std::size_t                         mContextBudgetLowTokens;

			//! Entries CompactTranscript has dropped this conversation -- see
			//! CompactedEntryCount().  NOT provider-neutral config: it
			//! describes the CURRENT transcript, so Reset() clears it.
			std::size_t                         mCompactedEntryCount = 0;

			std::vector<ChatTranscriptEntry>    mTranscript;

			//! The tool calls of the last assistant turn + the results
			//! received so far (flushed into ONE user message).
			std::vector<ChatToolCall>                          mPendingCalls;
			std::vector<std::pair<ChatToolCall, std::string>>  mPendingResults;

			int mToolRounds;   //!< tool rounds in the current conversation-turn
			int mMaxToolRoundsPerTurn = kMaxToolRoundsPerTurn;   //!< instance cap (SetMaxToolRoundsPerTurn); default = the static anti-spin cap
			//! true once a host pinned the cap via SetMaxToolRoundsPerTurn.
			//! Guards SetProvider's provider-default assignment so a
			//! deliberate host cap survives a provider switch.  Deliberately
			//! NOT reset by Reset()/SetProvider -- the host set it for the
			//! lifetime of this loop, same posture as the model selection.
			bool mToolRoundsCapExplicit = false;
			//! Blind-edit nudge state (see kDefaultBlindEditNudgeThreshold).
			//! `mBlindEditStreak` counts consecutive document-mutating tool
			//! calls with no intervening visual observe; it resets to 0 on any
			//! observe verb and on a new user turn.  When it reaches a positive
			//! multiple of the threshold, `mPendingBuildNudge` is armed with a
			//! reminder that the flush at the end of the round appends to the
			//! transcript as a Role::DriverNote entry and clears
			//! (AppendPendingBuildNudge).
			//! The stash exists because arming happens per-call while delivery
			//! must wait for the whole parallel round to be answered.
			int         mBlindEditStreak = 0;
			int         mBlindEditNudgeThreshold = kDefaultBlindEditNudgeThreshold;
			std::string mPendingBuildNudge;

			//! Driver-note delivery bookkeeping -- see DriverNoteCount() /
			//! LastDriverNoteText().  Describes the CURRENT transcript, so
			//! Reset() clears both alongside mCompactedEntryCount.
			std::size_t mDriverNoteCount = 0;
			std::string mLastDriverNoteText;

			//! TEXT-ONLY-MODEL IMAGE-REJECTION RECOVERY: sticky once a
			//! text-only model 400-rejects multimodal content.  While set,
			//! every BuildRequest strips images from the transcript, so the
			//! session never re-sends an image to a model that proved it has
			//! no vision.  Doubles as the once-per-round retry guard (a
			//! second multimodal 400 finds it already true and does NOT
			//! retry again).  Cleared by Reset() (a new session may target a
			//! different, image-capable model).

			//! Eval-harness E1 trajectory state (all inert when mRecorder is
			//! null -- the non-recording path is byte-identical to pre-E1).
			std::unique_ptr<ChatTrajectoryRecorder> mRecorder;
			bool                                     mSessionEmitted;
			std::function<void(const std::string&)>  mTrajectorySink;
			ChatTrajectoryConfig                     mTrajectoryConfig;
			std::function<int64_t()>                 mTrajectoryClock;
			ChatHttpRequest                          mLastRequest;
			std::vector<ToolLinePending>             mToolLineStash;

			//! See the "TEXT-ONLY-MODEL IMAGE-REJECTION RECOVERY" note above
			//! mToolRounds.
			bool                                     mElideAllImages;

			//! REASONING-MODEL TOOLS-VS-EFFORT 400 RECOVERY: sticky once an
			//! OpenAI-family reasoning model (observed: gpt-5.6-terra) 400-
			//! rejects a function-tools request over /v1/chat/completions
			//! because its server-side default reasoning_effort is
			//! incompatible with tool calling on that endpoint ("Function
			//! tools with reasoning_effort are not supported for <model> in
			//! /v1/chat/completions. To use function tools, use
			//! /v1/responses or set reasoning_effort to 'none'.").  This
			//! codebase never SENDS a reasoning_effort field itself (the
			//! model's default applies server-side), so the fix is NOT an
			//! omission -- there is nothing to strip.  Instead, while set,
			//! every BuildRequest asks the codec to EXPLICITLY add
			//! `"reasoning_effort":"none"` to the request body, which is the
			//! documented remedy for staying on /v1/chat/completions.
			//! Doubles as the once-per-round retry guard, mirroring
			//! mElideAllImages exactly.  Cleared by Reset().
			bool                                     mForceReasoningEffortNone;
		};
	}
}

#endif
