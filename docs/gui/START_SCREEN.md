# Start Screen — three-path launch experience

Status: **IMPLEMENTED both platforms (branch gui-start-screen, 2026-07-15).**  Mac live-verified; Windows review-verified only (first Windows compile pending, per house convention).
Owner surfaces: Mac SwiftUI shell, Windows Qt shell, shared core (starter-scene
load + agent handoff), agent harness (empty-scene construction — parallel
workstream, out of scope here).

## 1. Problem

Today the no-scene state is a dead placeholder ("Open a .RISEscene file to
begin").  Every session starts with a trip through the File menu.  The launch
screen should instead present the three real user intents simultaneously:

1. **Resume** — reopen one of the last 10 scenes with one click.
2. **Locate** — open the native file picker.
3. **Create** — describe a scene in a prompt; the agent builds it.

## 2. Ratified decisions (2026-07-15)

| # | Decision | Choice |
|---|----------|--------|
| 1 | Layout | **Grouped 2-column**: "Open a scene" (recents list + Browse button) beside "Create with the agent" (prompt box + Create button). Not three equal columns. |
| 2 | Starter scene for the agent path | **Truly empty** — the structural minimum only (shader, rasterizer, film, camera; ZERO objects/lights; renders black). A parallel agent-harness workstream owns making the agent effective from an empty scene. Do NOT add scaffold geometry to compensate. |
| 3 | Create behavior | **Auto-send**: the prompt is submitted to the agent immediately on Create; the user watches the agent build. Not just pre-filled. |
| 4 | Recent rows | **Text + icon only.** No thumbnails (complicates on-disk storage). |
| 5 | Scope | **Wire the full freeform-prompt flow now.** Scene-from-prompt *quality* is the parallel harness workstream, with its own eval; the GUI ships the plumbing without waiting for it. |

## 3. Layout

Replaces the placeholder in the center column whenever no scene is loaded
(fresh launch AND after Close Scene).  The top bar, menus, and log drawer stay;
the left/right panels are hidden as today (no bridge exists yet).

```
┌────────────────────────────────────────────────────────────────┐
│                        Start a session                         │
│   Open a scene you've worked on, browse for a file, or         │
│   describe one for the agent to build.                         │
│                                                                │
│  ┌─ Recent scenes ─────────────┐  ┌─ Create with the agent ──┐ │
│  │ pool_caustics_vcm    2m ago │  │ [provider chip]          │ │
│  │ watch_dial         yesterday│  │ ┌──────────────────────┐ │ │
│  │ cornellbox_vcm_simple   Mon │  │ │ multi-line prompt    │ │ │
│  │ …(up to 10)                 │  │ │ box                  │ │ │
│  │ glass_pavilion  (not found) │  │ └──────────────────────┘ │ │
│  ├─────────────────────────────┤  │ [✦ Create scene]         │ │
│  │ [📂 Browse files…]          │  │ Loads a blank stage, then│ │
│  │  or drop a .RISEscene here  │  │ the agent builds it.     │ │
│  └─────────────────────────────┘  │ ── Presets (future) ──   │ │
│                                   └──────────────────────────┘ │
└────────────────────────────────────────────────────────────────┘
```

Styling: Theme tokens (dark/light aware), IBM Plex, spectral accent on the
Create button.  Sentence case, no exclamation marks.

## 4. The three flows

### 4.1 Recent → open
- Row = scene basename (no extension), parent folder (tail-truncated), and a
  relative last-opened time.  Click anywhere on the row → `loadScene(at:)` /
  `MainWindow::openSceneFile` — the exact code path Open Recent uses today.
- **Missing file**: verified per render of the screen (`stat`, cheap at ≤10).
  A missing entry renders dimmed with "file not found" and an ✕ that removes
  it from the list.  Clicking a missing row does nothing (no error dialog).
- Drag-and-drop of a `.RISEscene` anywhere on the start screen = open it
  (both platforms register the drop target on the start view only).

### 4.2 Browse → open
- Button → the existing native picker (`openScene()` / `onOpenScene`).  ⌘O /
  Ctrl+O continue to work.  Nothing new.

### 4.3 Create → agent
1. Validate: non-empty prompt AND agent configured (see §6).  The Create
   button is enabled-with-feedback, not silently disabled: clicking with an
   empty prompt focuses the box; clicking while unconfigured shows the
   connect-agent state inline (§6).
2. Load the **starter scene** (§5) through the normal scene-load path, so
   controller/bridge/CST wiring is identical to a file open.  The app enters
   the standard workspace with a black (empty) viewport.
3. The scene is **untitled** (§5.2): not on disk, not in recents.
4. Switch the left panel to the Agent tab and submit the prompt through the
   existing `ChatViewModel.send()` / Qt chat-send path — literally the same
   entry as a typed message, so gating (`isSceneEditableForAgents`), proposal
   flow, autonomy, and trajectory recording all apply unchanged.
5. The user watches the agent construct the scene (viewport updates live as
   edits apply — existing behavior).

## 5. Core additions

### 5.1 Starter scene (the only new scene content)
A canonical in-repo asset, `scenes/Templates/empty_starter.RISEscene`
(header `RISE ASCII SCENE 7`), containing exactly:
- `standard_shader` (global, DefaultPathTracing)
- `pathtracing_pel_rasterizer` (modest interactive-friendly samples; OIDN on
  per house default for non-diagnostic renders)
- `film` (a sensible default, e.g. 800×600)
- `pinhole_camera` (origin-ish framing so inserted objects near the origin
  are visible once lit)
- **No objects, no lights, no environment.**  It renders black by design
  (decision #2).  The camera exists because the interactive loop and film
  require one; it is the structural minimum for a loadable scene, not
  scaffolding.

The GUI loads it via a new core entry `RISE_API` call (see §5.3) rather than
by path, so the asset can ship inside the app bundle / resources and the
canonical copy in `scenes/Templates/` stays the single source of truth
(build step copies it; test asserts they stay identical).

### 5.2 Untitled-scene state (first-class)
`loadedFilePath == nil` (Mac) / empty path (Windows) already half-exists;
make it a supported state everywhere:
- Top bar shows "Untitled" + dirty dot once the agent's first edit lands.
- **Save → Save-As** when untitled (both the menu item and the top-bar save
  affordance route to the file-picker path; after a successful Save-As the
  scene gains a path, joins recents, and behaves as a normal scene).
- Untitled scenes never enter the recents list (nothing to reopen).
- Close Scene with unsaved edits (scene OR raw editor text, named OR
  untitled) prompts Save / Discard / Cancel.  (Round-2 review: this flow
  did not previously exist for ANY scene — implemented on this branch;
  Save routes untitled scenes to Save-As and a cancelled save aborts the
  close.)
- The CST-mirror editor pane works unchanged (it reads the retained Document,
  not the disk file); the editor's "file changed on disk" states are simply
  unreachable while untitled.
- Audit knowledge: SaveEngine's external-modification refusal keys on the
  loaded file — verify it treats no-file as "Save-As required", never as a
  refusal loop.

### 5.3 C-ABI additions (shared core; Windows gets identical behavior)
- `RISE_API_LoadStarterScene(...)` — loads the embedded/bundled starter
  content through the SAME CST load path as a file (returns the same job
  handle shape the file-open path produces), flags the result untitled.
- No other core surface changes: recents, the picker, and chat-send all
  exist.

### 5.4 Recents schema
Keep the existing shared key `recentSceneFiles` (both platforms already use
it) as an ordered path list, most recent first, cap 10.  Add a **sibling** key
`recentSceneMeta` mapping path → `{lastOpenedUnix}` for the time label;
absence of meta (old installs) renders the row without a time rather than
migrating.  No schema break, no thumbnails (decision #4).

## 6. Agent-readiness states (path 3 must never dead-end)

The Create column reflects the real agent state, reusing the chat panel's
provider config:

| State | Column shows |
|---|---|
| Configured (provider + key present) | Prompt box + Create + provider chip |
| Not configured | Prompt box (disabled look) + "Connect an agent to create scenes from a prompt" + a **Set up…** button that opens the existing chat provider settings |
| Configured but send fails (key revoked, network) | The normal chat error surfaces in the Agent tab after handoff — the start screen does NOT pre-flight the network |

Readiness = the same check the chat panel uses to enable send (provider
selected AND key retrievable).  No new validation machinery.

## 7. Platform split

| Piece | Where |
|---|---|
| StartView (SwiftUI) replacing the placeholder in `RenderImageView`/center column | Mac shell |
| StartWidget (Qt) replacing the "Open a .RISEscene file to begin" central widget | Windows shell |
| Starter-scene load + untitled flagging | shared core + thin bridge methods |
| Recents meta read/write | platform-native storage (UserDefaults/QSettings), same keys/schema |
| Auto-send handoff | platform shells calling their existing chat-send, sequenced after bridge attach (Mac: after `chat.sceneOpened(...)`; Windows: after the bridge/controller wiring completes) |

Sequencing note (the one race to design for): the prompt must be submitted
only after the scene load completes and the chat is bound to the new bridge —
i.e. the same point where `sceneOpened` fires today.  Implement as
"pending-first-prompt" state on the chat VM consumed exactly once by
`sceneOpened`, so a slow scene load can't drop or double-send it.

## 8. Edge cases

- **Recents all missing** (moved repo): list shows the missing states;
  Browse and Create remain fully usable.  Empty recents (fresh install):
  column shows a quiet "Scenes you open appear here."
- **Create while a previous session's scene is loading**: the start screen
  only exists when no scene is loaded; no overlap.
- **Double-click Create**: button disables on first activation until the
  load+handoff completes or fails.
- **Agent turn fails immediately** (bad key): user lands in the normal
  workspace with the error in the chat transcript — the untitled empty scene
  stays open (they can retry by typing; nothing is torn down).
- **Prompt length**: cap at the chat input's existing limit; no new rule.
- **Close Scene returns here**: the start screen re-renders with updated
  recents (the just-closed scene now first).
- **Render Animation while untitled**: the movie output path is derived
  from the scene path, so an untitled scene renders animation frames with
  video output disabled.  Save-As first to get a movie.  (Known v1
  limitation; acceptable — untitled scenes are mid-construction.)
- **Media paths while untitled**: the bundled starter loads from inside the
  app bundle, so the `global.options` project-root walk-up finds nothing —
  relative media paths won't resolve and the core logs a MediaPathLocator
  warning (harmless for an empty scene).  Save-As re-anchors media paths to
  the chosen directory (implemented).  LATENT (core, out of scope here):
  `GlobalOptions()` is a first-call-wins process-lifetime singleton — if the
  first scene of a session is the bundled starter, options resolve to coded
  fallbacks for the whole session even after Save-As.  Today's authored
  global.options values mostly match the fallbacks, so impact is muted; a
  real fix is core GlobalOptions re-resolution.

## 9. Phasing

1. **P1 — Open paths**: StartView/StartWidget layout, recents list with
   missing-file handling + meta times, Browse, drag-drop.  (Pure shell work;
   shippable alone.)
2. **P2 — Create path**: starter asset + `RISE_API_LoadStarterScene` +
   untitled state + Save-As routing + auto-send handoff + readiness states.
3. **P3 — Presets (future, not now)**: data-driven chips
   `{label, promptSeed}` seeding the prompt box; the column reserves the row.
   Depends on the agent-harness workstream's findings about what seeds work.

Definition of done per repo standard: implementation-review-loop to zero P1s,
both platforms (Windows review-verified if uncompilable here), launch smoke
test (this feature IS the launch surface), and a live click-through of all
three paths on Mac.

## 10. Explicit non-goals (v1)

- Scene thumbnails (decision #4).
- Scaffolded/staged starter content (decision #2 — harness workstream owns
  empty-scene competence).
- Presets (P3).
- Any new agent capability; path 3 rides the existing chat pipeline
  end-to-end.
- Template gallery / "new from template" — subsumed by presets later.
