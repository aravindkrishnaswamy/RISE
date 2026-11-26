# Consolidation arc — manual Mac GUI click-through checklist

**Why this exists:** SwiftUI panels cannot be driven headlessly; every C++
mechanism below is unit-tested, but the Swift flow around each button is
human-verify-only.  Precedent: the S4 round-4 bug (production render refused
after viewport pause) was found by the user's **first real click** after five
green review rounds.  These are the owed checklists from the F2, image-attach,
Secure-MCP S5c, and chat-provider arcs, consolidated.

Setup: build the `RISE-GUI` scheme, open `scenes/Tests/Geometry/shapes.RISEscene`.
Handy probe edit (visible + undoable): *"make the orange things red"* in chat, or
`propose_patch color_orange.color = "1 0 0"` in the raw panel.

## A. Chat providers (incl. the new ChatGPT provider, `e89bf937`)

- [ ] Settings popover: pick each of Anthropic / Gemini / ChatGPT; save a key
      for each — caption reflects key source (Keychain vs env) instantly.
- [ ] Pick a provider ≠ the applied one → orange "chat is using X — click
      Apply" banner shows; Apply switches provider AND resets the transcript.
- [ ] ChatGPT end-to-end turn: a real edit request → tool calls execute, the
      viewport updates live, Save enables, **Cmd-Z undoes the agent edit**
      (and Cmd-Shift-Z redoes).
- [ ] Fresh-default check: `defaults delete <bundle-id> agentChat.provider`,
      relaunch → app starts on ChatGPT (the new default); with no OpenAI key
      the error message names the fix ("Add one in the chat settings … or
      launch with OPENAI_API_KEY").
- [ ] Mid-conversation key save for the active provider → Retry recovers the
      turn (in-process key cache reseeds).

## A2. Chat providers — xAI + local/Ollama (Providers S1-S3, `2fbf0f86` + this slice)

- [ ] Settings popover: pick Grok (xAI) → the normal key-entry UI appears
      (SecureField + Save/Clear Key); paste an `XAI_API_KEY`-valid key, Save,
      Apply → a real edit request round-trips (tool calls execute, viewport
      updates live).
- [ ] Settings popover: pick Local (Ollama) → the key-entry UI is REPLACED by
      an endpoint block: no SecureField/Save/Clear Key, just the resolved
      endpoint (`http://127.0.0.1:11434/v1/chat/completions` by default) and
      a hint that a local server must be running (e.g. `ollama serve`).
- [ ] Local, with `ollama` NOT running: send a message → the turn ends in a
      clean transport/network error in the transcript (Retry offered), NOT a
      hang or a crash.
- [ ] Local, with `ollama serve` running and `qwen3:32b` pulled: send a
      message → a real edit request round-trips end-to-end.
- [ ] Local with `RISE_LOCAL_LLM_BASE_URL` set before launch (e.g. to an
      LM Studio / llama-server endpoint) → the settings popover's endpoint
      line reflects the override, and requests actually go there (verify via
      the target server's own request log, or by pointing it at a bogus port
      and confirming the transport error above).
- [ ] Windows leg (compile-blind; verify by inspection during the next MSVC
      pass): provider combo lists "Grok (xAI)" and "Local (Ollama)" after
      "Gemini"; picking Local swaps the API-key field's placeholder to the
      resolved-endpoint hint and sending with an empty key field no longer
      blocks on "Enter an API key before sending."

## B. F2/S2b — non-blocking chat renders (the five owed scenarios)

- [ ] Ask the chat to render: UI stays responsive for the whole render (type,
      scroll, open popovers).
- [ ] Click **Render** (production) while a chat render is in flight → visible
      "Turn cancelled — a production render started." notice; production runs.
- [ ] Close the scene mid-chat-render → no crash, no hang, no zombie worker.
- [ ] Raw JSON-RPC dev panel (Settings → Developer → "Show raw JSON-RPC
      panel"): `render {"async":true}` → poll `render_status` / `render_wait`
      to completion.
- [ ] **Stop** mid-chat-render → turn ends promptly, render cancelled.

## C. F2/S4 — production renders through the coordinator

- [ ] Cancel during a production render works and the progress bar is live
      throughout.
- [ ] Start a production render while an agent (chat) render runs → production
      **waits then runs** (single slot), never crashes, never double-renders.
- [ ] Pause the viewport, then production render → **accepted** (the
      `StopInteractive()` split — the round-4/5 user-found bug; re-verify on
      every branch that touches teardown).
- [ ] A second production render later in the same session works (worker not
      retired).

## D. User image attachments (`86d7ab2a` + `acca5c6b`)

- [ ] Attach via picker AND drag-drop (drop-target highlight appears);
      thumbnails render; UI does not hitch during downscale of a large photo.
- [ ] The model demonstrably sees the image (ask "what's in the photo?").
- [ ] Attach a 5th reference image across turns → oldest is elided to a
      placeholder; the newest 4 stay live.

## E. Secure-MCP S5c — external proposes, owner approves

- [ ] Settings → "Allow external agent (MCP) connections" ON → copyable URL +
      per-launch token shown; note the honest "can propose, not commit" text.
- [ ] From a terminal MCP client (e.g. Claude Code with the URL+token):
      `tools/list` succeeds **with** the token; **without** it → 401/403;
      forged `Origin: http://evil.example` → 403.
- [ ] External `propose_patch` → proposal appears in the panel's disclosure
      group within ~1s; **Approve** → scene changes live + Save enables +
      Cmd-Z works; **Reject** → scene untouched.
- [ ] External `render` while the interactive viewport refines → completes,
      no crash (coordinator-serialized).
- [ ] Close/reload the scene with the server running → server stops (client
      connection refused), pending proposals invalidated (stale approve →
      conflict, not a wrong-scene commit).
- [ ] Toggle OFF → connection refused immediately.

## F. Cross-cutting regression spot-checks

- [ ] 1c-1 raw JSON-RPC panel: `read_document` → text + headVersion;
      stale-`baseHeadVersion` `propose_patch` → `status:"conflict"`.
- [ ] Close-without-save after ANY agent edit → the unsaved-changes prompt
      appears (dirty/Save-safety).
- [ ] After Undo of an agent chunk insert, Save + reload the scene → loads
      clean (glue-safe splice).

Record failures with the exact click sequence and screenshots; fixes go through
the normal implementation-review-loop.
