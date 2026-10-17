# 91 — Context lifetimes and visible detail

Status: L1–L3 (context) SHIPPED 2026-10-17 (branch `context-lifetimes`; see
§2 "As shipped"); V1–V2 (visible detail) SHIPPED (branch `visible-detail`; see
§3.1 "As built")
Evidence base: trajectories `20261017T011539Z-71dd70e7` (apothecary) and
`20261017T133602Z-dbe83160` (dragons/wizards), both gemini-4-argon, the
default model since `0aa14cf8`.  Measured with `tools/trajectory_census.py`.

## 1. What the two runs show

The geometry toolkit (docs 85, 87, 89) and the iteration ratchet (doc 90)
carried over to the new default model unprompted: the dragons run used 36
`point_morph`, 13 `mirror`, 4 `skin_geometry` membranes and 18
superellipsoid masses, and both runs pinned the render anchor forward.
Two costs remain, and they are general — neither is about a particular
scene.

**Context volume.**  The apothecary session re-sent 6.47M input tokens in
40 turns; per-turn input grew from 36K to 252K.  Images are already elided
(`tool_image_elision`) and repeated `read_document` results are superseded,
so the growth is TEXT that nothing ever retires:

| tool result | live text | why it persists |
|---|---|---|
| `read_skill` x3 | 165 KB | whole skill files, read once during planning, live to the end |
| `build_element` x6 | 122 KB | ~85% is per-chunk success boilerplate (`headVersion`, `rawCode`, "chunk inserted via a full re-derive") and the landed names repeated in `landed` and `message` |
| `read_document` | ~82 KB live | already superseded correctly |
| `render` x4 | 66 KB | inventory + design note per render (simulated saving of superseding it: ~1%, not pursued) |

S2 span compaction (doc 71) cannot help: the session has two user
messages, so the whole build is one span — the blind spot doc 71 names.

**Detail nobody looks at.**  The apothecary cat was authored with six
`hair_geometry` coats (guard hair + undercoat) — fur, previously ruled an
engine boundary, now ships — yet it was never seen at a size where fur
reads.  The finish-element close-up framed the cat at ~25% of the panel
width, and in the scene render the cat is ~20 px wide.  The final scene
inventory reported objects only from a 96 px identity pass.

## 2. Context lifetimes (slices L1–L3)

Principle: a tool result has a LIFETIME determined by what it describes,
not by age.  When the thing it describes is settled, the result is
replaced by a one-line stub that says how to get it back.  Every rule
converges (a slot is rewritten at most once), so each costs one bounded
provider-cache miss — the doc 71 / T1c policy, unchanged.

- **L1 — trim the `build_element` report at the source.**  Successful
  chunks are reported as names once; per-chunk detail (`ChunkResultJson`)
  is kept only for chunks that failed, were rejected, or carry a
  diagnostic.  `message` stops repeating the landed list.  Apply the same
  rule to any other agent verb whose report carries success boilerplate
  per chunk (audit `insert_chunks`, `place_element`, `populate_scene`).
  No history rewrite at all — the cheapest and most cache-friendly lever.
  Consumers of the full per-chunk form (MCP adapter, eval runner, tests)
  must be audited; if a consumer needs it, it gets it via an explicit
  opt-in, not by the chat default.
- **L2 — element-scoped retirement.**  When a successful `finish_element`
  for element E flows through the chat loop, earlier live
  `build_element` and `insert_chunks` results that belong to E are
  replaced with a stub naming E and the recovery path (`read_document`
  shows its chunks).  The stub states ONLY IMMUTABLE facts (review rounds
  5-6): that the result was retired when E was finished, a POINTER to the
  current state ("read_document shows the current document;
  finish_element / reopen_element results list the element's chunks" --
  not a claim that any exist: nothing may have landed, or a later edit or
  undo removed them; read_document itself has no element attribution, and
  both of those verbs' results carry the attributed `chunks` list), the realised bounding box "when built", the head version
  "after that call" (exact integers) and the per-chunk outcomes "that call
  reported".  A retired error envelope keeps Anthropic's `is_error:true`
  around its stub: the call did fail, and the flag is wire-valid.  The
  compose-retired skill stub is past tense too ("retired when every
  element of the build plan was finished").  It
  states NO edit or reopen rule: the phase, the session mode and the plan
  all change after the finish (the plan reaches compose, a final answer
  switches to refining, the give-up trips, a new plan is filed) and the
  stub is never rewritten -- rounds 1-4 each found a new stale state for
  every more precise rule text.  The live rule is stated where it applies:
  `CheckElementWindowForEdit_`'s refusal and the compose-phase creation
  refusal both name `reopen_element` when an edit is actually refused.
  Cost: the finish result carries the isolate PNG, so its flush already
  rewrites the previous image's entry and pays one provider-cache miss
  from there on.  Retiring build results OLDER than that image moves the
  miss earlier in the prompt (more of the prefix is re-sent uncached); a
  finish whose builds are newer than the previous image adds nothing.
  Either way it is one miss per finish flush.  A build result from the
  SAME turn as the finish is retired one flush later, which is a separate
  (small, recent) miss.  Keyed on data the loop already sees (`element` in the call args
  / result), not on session state.
- **L3 — skills retire at Compose.**  `read_skill` results are retired
  once the session reaches the Compose phase (as shipped: the successful
  `finish_element` reporting `phase:"compose"` -- not `place_element`,
  which also runs in the pieces phase), with a stub inviting a
  re-read if needed.  A later re-read of the same skill supersedes the
  earlier copy (the read_document precedent, keyed by skill name).
  The skill index stays in the system prompt, so the model still knows
  every skill exists.

Simulated on the two runs (live tool-result text re-sent per turn):
L1+L2+L3 = -41% (apothecary), -31% (dragons).  L3 alone is -18% on the
apothecary and 0 on the dragons run (which read no skills).

**As shipped (2026-10-17, revised after review round 1).**

- L1 (`AgentRpc.cpp`): a chunk result is CLEAN only when it is applied,
  has no `issues`, and its rawCode and message are exactly the canonical
  pair every insert fold writes (`kCstChunkInsertedRawCode` /
  `kCstChunkInsertedMessage`, `IJobPriv.h`, shared by the headless fold,
  the controller folds and the classifier).  Any appended notice -- the
  auto-parent disclosure, the CSG-operand detach warning, the S1/G2 folds,
  a batch's rawCode-3 "ANOTHER element" note -- makes it not clean.
  `chunkResults` of `build_element`, `light_scene`, `populate_scene` and
  `environment_scene` (which carry the identical pattern) goes through
  `NotableChunkResultsJson` -- every non-clean or unnamed chunk keeps its
  `ChunkResultJson`; a clean, named one is reported in `landed` /
  `created` only, with its kind in `landedKinds` (index-parallel; the pass,
  not the model, chose the kinds) or `created[].kind`.  `frame_scene`
  keeps EVERY entry (at most two -- the new camera's insert and the old
  one's removal -- and a failed removal would make "named in
  cameraNameAfter" untrue).  `build_element`'s `message` states the count
  ("Chunks inserted: 16 (named in `landed`)") instead of the names.
  `insert_chunks` keeps its index-aligned `results` (ToolOutcomeLine and
  both GUI retry gates read it) and slims only a clean element to
  `{applied,status,headVersion,name,kind,element?}`
  (`BatchChunkResultJson`); `headVersion` stays because it is the next
  call's `baseHeadVersion`.  No top-level `headVersion` was added (it would
  newly trigger the recorder's head-bump document snapshots).  Measured in
  `AgentChunkCrudTest` S2h-L1 (fake builder, short names): a 16-chunk
  report 5,347 -> 1,438 bytes (4 chunks: 1,974 -> 1,004), marginal cost per
  extra clean chunk 281 -> 36 bytes (name + kind); a 4-chunk
  `insert_chunks` report 998 -> 554 bytes (IC4a).  Not changed:
  `place_element` / `environment_scene` / `frame_scene` `patchResults` (no
  per-entry identity -- the per-patch record IS the edit record, pinned as
  such), and the scaffold / `remove_chunks` batch verbs, whose
  descriptions accordingly still promise the full insert_chunk shape.
  Consumer audit: no eval checkpoint, MCP client code or GUI shell reads a
  clean entry's `rawCode` / `retriable` / `message`; the GUI gates read
  them only on wholly-refused batches, whose entries are never slimmed.
- L2/L3 (`AgentChatLoop.cpp` `RetireToolResultsByLifetime`, sharing
  `RewriteToolResultSlots` with supersession): slots record `verb`,
  `lifetimeElement` (insert_chunks: from its APPLIED/diagnosed chunks
  only; a batch spanning two elements is never retired), `lifetimeCarry`
  (reviews 2-4: the per-chunk outcomes the document does not record, each
  named and stated only as RECORDED -- "refused: <reason>"; "rejected on
  attempt A (as K), landed on attempt B[ as K2]" only when build_element's
  report says so (`rejected[].attempt`, `landedAttempts`, both recorded by
  BuildElement since round 4 -- one answer that lands X and has a second X
  refused is a refusal, never a retry); "landed with a notice" / "landed,
  diagnosed"; keyed by (name, kind, text), a repeated identical refusal
  shown once with "(xN)", at most 12 items then "and N more";
  build_element's top-level message prose -- give-up fold, unquoted-name
  disclosure, total-rejection excerpt -- is not carried), `lifetimeFacts`
  (build_element's realised world bounding box; insert_chunks' last
  `headVersion`), which the element-finished stub carries so retiring never
  deletes the only copy, and
  `lifetimeSkill` at pack time.  Known blur, accepted: the loop keys a
  result by ELEMENT NAME, not by plan, so after a new `file_build_plan`
  that reuses an element name, still-live results from the old plan's
  element of that name retire with the new one's finish (the stub claims
  nothing about which plan they belonged to).  A carried notice has the canonical insert
  prefix stripped so its 160 bytes go to the notice itself, carries a
  landed chunk's `issues` warnings, and a staged chunk is labelled
  "staged as a proposal".  Lifetime stubs ride under the JSON key
  `retired_note` on the Gemini / OpenAI wires (supersession keeps
  `superseded_note`).  A successful `reopen_element` E cancels
  an element-finished retirement for E that was decided but not yet
  applied.  A lifetime event DECIDES (marks
  `pendingRetireNote` on earlier slots) and the rewrite is APPLIED only to
  entries before the one being flushed, retried each flush until it lands
  -- so a same-turn build+finish still delivers the build result once.
  **The L3b trigger is NOT `place_element`** (that is legal and prescribed
  in the pieces phase, with later elements unbuilt): it is the successful
  `finish_element` whose result reports `phase:"compose"` (every element
  finished), once per build plan, and it retires skills read in EARLIER
  turns only -- a skill read in the same turn as that finish counts as
  post-compose in either order; a successful `file_build_plan` re-arms
  it, as does `Reset()`.  history_edit reasons
  `element_finished_retirement`, `compose_skill_retirement`; the same-name
  `read_skill` supersession is `tool_result_supersession`.  An `ok:false`
  finish retires nothing.  Tests: `AgentChatLoopTest` T14c (all four
  wires).

What is deliberately NOT here: LLM summarization (doc 71 S4), aging by
turn count (an age says nothing about whether the content is still
needed), rewriting assistant entries (byte-preserved by contract).

Risk to measure: L3 could cost Compose-phase quality if the model needed
a retired skill and did not re-read it.  The census reports re-reads; a
re-read is the rule working, a quality drop without one is the rule
failing.

## 3. Visible detail (slices V1–V2)

- **V1 — the close-up fits the surface, not the box.**  The isolate
  auto-framer (`IsolateFitDistance`) fits the 8 corners of the WORLD-axis
  AABB of an already-transformed local AABB, from a fixed world-space
  three-quarter vantage.  A box fixture is its own AABB, so the pinned
  test passed while a rotated, elongated, smin-padded creature filled a
  quarter of the frame (the pinned SDF arm itself measures 8%).  Fit
  instead to points ON the object's surface (a tessellation or surface
  sample of the actual geometry, transformed to world), falling back to
  the AABB corners only where no surface points are available.  Gate on a
  fixture class that failed in reality: a rotated, elongated SDF body.
- **V2 — a zoom look from the scene camera.**  `render{zoom:"<object>"}`
  keeps the scene camera's LOCATION, aims at the object and narrows the
  field of view to fit it (same surface-point fit), with every other
  object and every light in place.  It is a crop of the final frame at
  full agent resolution: what a viewer of this scene would see of that
  object, occlusion included, at a size where its material and detail
  read.  Reach: the tool description, plus one measured sentence in the
  render inventory when a plan element covers under ~1% of the frame
  ("`cat` covers 0.4% of this frame — render{zoom:...} shows it at full
  resolution from this camera").  Advisory, self-disarming, no number to
  chase.

### 3.1 As built (V1, V2)

- **V1.** `render{isolate:}` (and so `finish_element`'s close-up) gathers
  world-space points on the object's real surface -- the vertices of its
  geometry's tessellation (SDF/skeleton: 48 cells along the longest axis;
  primitives: 32 segments; meshes as authored; capped at 24k points) -- and
  fits THEM (`IsolateFitPoints`): the perspective generalisation of the old
  8-corner fit, with the aim refined to the centre of the projected extent
  rather than the 3D box centre.  CSG composites, infinite planes and any
  geometry that cannot tessellate fall back to the old AABB-corner fit
  (`isolate.fitBasis` says which).  The scene camera's own `orientation` /
  `target_orientation` are cleared for the render (they were applied ON TOP
  of the override and swung the aim off the object).  Measured on the exact
  objectmap pixel share of the isolated frame: rotated elongated creature
  0.038 -> 0.300, smin blob (draft panel) 0.083 -> 0.484, box 0.402 -> 0.529;
  gather + fit 7-15 ms.  Side finding: the pinned finish_element coverage
  arms measured the two-panel composite and had been vacuous (~0.95-0.99 for
  any fit) since the material-look panel was added; they now crop to the
  draft panel.
- **V2.** `render{zoom:"<object>"}` keeps the scene camera's real (post-orbit)
  eye, aims at the surface-point extent's centre and narrows the vertical fov
  to fit at the same 0.85 fill (floor 2 deg; never wider than the scene fov --
  an object that already fills the frame keeps it and the result says so).
  Nothing is hidden.  Exclusive with `isolate`, `view` and `camera`; pinhole
  only.  A zoom render carries no scene inventory, anchor or scene-target
  comparison (its camera is not the reconstructable one).  The result's
  `zoom` block: `object`, `eye`, `fovDegrees`, `sceneFovDegrees`,
  `keptSceneFov`, `coverage`, `fitBasis`.  Reach: the production inventory
  appends one sentence naming up to three plan elements whose objects cover
  under 1% of the frame in total.
- **Camera restore.** An override render (`view`, `camera`, `isolate`,
  `zoom`) captures the active camera's pose fields bit-exactly
  (`CameraIntrospection::CaptureExactValue`: the stored doubles, radians)
  and puts them back with `RestoreExactValue`.  Capturing through the
  panel's `%.6g` strings (and degrees) used to round the live camera away
  from the document on every override render.
  `SetProperty("target_orientation")` also accepts the two-component form
  `GetPropertyValue` emits, so the string fallback can write back what it
  captured.
- **Zoom's refusals (all `ok:false`, no `zoom` block).** Unknown or
  non-renderable name (names listed); a conflict with `isolate`/`view`/
  `camera`, or a non-pinhole active camera; the object **not in view** of
  the scene camera -- its surface centre behind the camera, or more than
  10% past the frame edge (`DirectionInSceneView`, shared with the
  inventory hint); any surface point on or behind the plane through the eye
  (a floor or room around the camera); a degenerate or unbounded extent.
  The zoom keeps the scene camera's *realized* up (its own roll), read from
  the projection matrix; a line of sight parallel to that up falls back to
  the camera's realized screen-right, never a fixed world axis.  The
  inventory's hint only names elements that pass the same in-view test.
- **Thin SDF parts.** The 48-cell SDF grid misses features thinner than
  about half a cell, so each additive part thinner than 1.5 cells also feeds
  the fit the corners of its own *unpadded* primitive box
  (`SDFGeometry::GetAdditivePartExtents`).
- **Gates.** A zoom render, like an isolate one, is an inspection look: it
  is not held behind the compose-phase build checklist.  No tonal fact is
  attached to a zoom frame (a close crop would read as a statement about
  the scene's picture), and no inventory, anchor or scene-target comparison
  either.

## 4. Measurement

Re-run the apothecary and dragons prompts.  Census: per-turn input-token
curve (L1–L3 working = flatter curve, lower total), skill re-reads after
Compose, zoom calls and whether an edit followed one, finish-element
close-up coverage, and the usual adoption/orphan columns.  Delivery is
not compliance — census the document and the results, never the note.
