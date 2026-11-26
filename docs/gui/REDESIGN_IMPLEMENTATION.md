# UI Redesign — Implementation Record (2026-07-11)

**Status:** shipped on branch `ui-redesign` for both desktop clients.
**Design source:** the approved "RISE Prototype" comp (claude.ai/design
project "RISE UI Full Screen Layout"), commissioned against
[DESIGN_BRIEF.md](DESIGN_BRIEF.md).  This document records what was
actually built, the honesty calls made where the comp and the engine
disagreed, and what remains deferred.  It is a record, not a plan —
when code and this document disagree, trust the code and fix this file.

## What shipped

### Design system (both platforms)
- Token files: `build/XCode/rise/RISE-GUI/App/Theme.swift` and
  `build/VS2022/RISE-GUI/Theme.{h,cpp}` — neutral dark surfaces, text
  ramp, accent/status colors, category tag colors, radii, the
  380–780 nm spectral identity gradient.  The two files mirror each
  other **by convention**; change both together.
- IBM Plex Sans (400/500/600/700) + IBM Plex Mono (400/500/600),
  OFL-licensed, bundled on both platforms (CoreText registration on
  macOS; a new rcc resource step + QFontDatabase on Windows).  System
  fallback when registration fails.
- Dark-only chrome: `NSApp.appearance = .darkAqua` on macOS;
  Fusion style + `Theme::applyDarkPalette` on Windows (the "windows11"
  style ignores QPalette in places and was dropped).

### Core (shared C++, `SceneEditController` + C API + both bridges)
- **Refinement pause/resume** (`PauseRefinement`/`ResumeRefinement`):
  composed from the proven `StopInteractive()`/`Start()` pair under a
  new `mLifecycleMutex` (serializes spawn/join; a concurrent
  pause/resume could previously `std::terminate`).  Any `Start()`
  un-pauses; the production-render restart respects a raced pause.
- **Honest refinement status** (`GetRefinementStatus`): the interactive
  loop refines by a 6-level resolution ladder + a denoised polish pass
  — there is **no sample-accumulation "pass N of M"**, so the comp's
  "Pass 12 / 64" readout was replaced by `RefinementPhase`
  {Idle, Rendering, Refining, Polishing, Paused} + the ladder divisor.
  The ladder-halve became a CAS so a gesture-end reset can never be
  silently lost (stuck-coarse preview + stuck-"Refining" status).
- **Interactive region-of-interest** (`SetInteractiveRegion` /
  `ClearInteractiveRegion` / `GetInteractiveRegion`): inclusive
  full-res film-pixel box, packed into one atomic; applied in
  `DoOneRenderPass` **only at full-resolution passes** (coarse
  navigation passes stay full-frame so nothing outside the box goes
  stale at mismatched scales); preserved while production owns the
  scene. Ordinary production callbacks never consult the viewport
  region, while the separately named Render Active Region action
  captures the bounds and calls `Job::RasterizeRegion` explicitly —
  the Blender region-leak footgun remains structurally designed against.
  Regional final post-processing is confined as well: filtered-film
  resolve accepts the rect, and OIDN denoises a cropped beauty/AOV set
  before copying only the selected pixels back.
- **`IRasterizer::HonorsRegion()`** honesty query (defaulted, declared
  last per the header's ABI convention): MLT returns false (global
  splat film); `AutoRasterizer` forwards to its resolved delegate.
- **Undo/redo labels**: `EditHistory`'s labels surfaced through
  `UndoLabel()/RedoLabel()` + C exports, feeding both shells' Edit
  menus ("Undo Translate", "Undo Agent Edit", …).

### Shells (macOS SwiftUI + Windows Qt6, near-identical)
- **Top bar**: spectral logo mark, filename + amber dirty dot,
  pause/restart + shared status readout (one formatter per platform —
  `RefinementStatusFormatter.swift`, ported verbatim to `TopBar.cpp` —
  so the top bar and the viewport pill can never disagree), spectral
  progress strip, elapsed/remaining during production renders,
  explicit **Error** state (never "Settled" on failure), integrator
  chip `AUTO → PT/BDPT/VCM` with the resolve reason as tooltip —
  omitted entirely when no resolved integrator exists (no fake
  "AUTO" placeholder), Save.
- **Left panel** — Agent / Scene-file tabs:
  - Agent tab: restyled chat (user bubbles / plain assistant text /
    tool-trace chips) and **proposal diff cards** (Apply / Reject /
    applied / rejected states) wired to the existing
    `AgentProposal` queue via `list_proposals`/`resolve_proposal`.
    Windows gained the proposals UI for the first time (it had the
    transport but no surface).  Apply/Reject/send are gated on the
    same scene-editable predicate as every other agent-mutation path,
    **including outstanding chat-driven renders**.
  - Scene tab: the text editor, permanently available, recolored to
    the comp palette in IBM Plex Mono, with a truthful save-state
    status bar (no fabricated "parsed · 0 errors" — there is no live
    parse-diagnostics surface yet).
- **Right panel**: new **outliner** (category tag chips + counts +
  expansion + selection through the same bridge selection the
  accordion used) above the descriptor-driven **inspector** restyle:
  typed value cells (wells, tinted X/Y/Z vec3, pill toggles,
  enum/preset chips, filename browse), entity header, and a
  Basic/Advanced two-level split (>8 rows → presets-first membership,
  descriptor display order; everything stays reachable — presentation
  only, per the brief's two-level cap).
- **Center column**: viewport toolbar row (tool group with the
  existing category flyouts, active-camera chip, 3-state REGION chip,
  EV chip with the exposure popover + EDR interlock, EDR chip),
  region drag-to-refine with eight edge/corner handles, move + redraw,
  measured badge, semantic accessibility actions, Esc/clear paths, and
  pointer suppression so a cancelled drag never leaks an orphaned
  pointer-up into tool state, selection chip + refinement pill overlays, log
  drawer (severity pills, WARN/ERR counts, filter, follow, collapse
  strip, ⌥L / Alt+L), restyled timeline (transport + thin track +
  "Render movie…"), preserving the exact scrub begin/move/end bridge
  contract.
- **Menus**: Render (pause/resume, restart, production render,
  animation, cancel), Edit (undo/redo with live labels), View (panel
  toggles, EDR, tone curve), File (open/recent/close/save scene/save
  rendered image).  macOS uses the native menu bar (ratified deviation
  from the comp's in-window strip); Windows keeps its QMenuBar.

## Honesty calls (comp vs. engine)
Fabrication was rejected wherever the comp showed data the engine
doesn't have; each omission is commented at the site:
pass counts (→ ladder phases), scene-file line numbers on entities
("⌗ L34" — no reverse node→offset API in the CST yet), camera lens
summaries, "grammar v2.3 · CST n nodes" badges, the L0–L3 autonomy
selector (no autonomy state on the chat VM yet), the light-mix
suggestion card, AOV pills (Beauty/False color/Heatmap), quad/floating
viewport layouts, the views rail, timeline camera-binding lanes, and
diff-card "− old value" lines for param edits (the proposal wire
doesn't carry the old value).

## Zero-regression parity
Every pre-redesign control was rehoused, none dropped — verified by a
dedicated capability-parity review with an old-UI control inventory on
each platform (the two review-caught regressions, elapsed/remaining
time and error-state visibility, were fixed before merge).
`ControlsWidget` (Windows) was fully rehoused and deleted.

## Known deltas / deferred
- **Windows is code-review-verified only** — this branch was built on
  macOS, which cannot compile the Qt client.  Two review rounds
  (compile-surrogate + behavior-parity, then a verification round)
  substituted for the compiler.  **First Windows build should expect
  minor breakage risk** despite the discipline; treat any compile
  error as a bug in this branch, not the design.
- Mac chat has provider-error poison-scoping (`consecutiveHttp400s` +
  reset offer) that Windows lacks (comment at the Windows site states
  the delta).
- Basic/Advanced tiering is a UI heuristic (presets-first, cap 8);
  a descriptor-level `uiTier` tag is future work (DESIGN_BRIEF A6).
- D-series spectral hero widgets (Kelvin/λ pickers, thin-film slider,
  scopes) and entity creation / asset library remain roadmap items
  (Directions C/D and the ENTITY_CREATION spec) — this redesign built
  the workspace they will live in.
