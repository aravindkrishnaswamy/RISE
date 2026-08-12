# Creative Lighting — Palette and a Clean Room (Arc 81 Design, 2026-08-12)

> **Status:** BUILT 2026-08-12, not yet measured live — §7 records what
> shipped and the decisions this design left to the build.  User direction
> 2026-08-09 (*"an arc on creative lighting especially using area lights"*),
> selected 2026-08-12 over the competing "chase the flat scene" arc.  Depends
> on arc 80's scene inventory, which is what makes a lighting clean room
> possible at all.

---

## 1. A correction that shaped this arc

After arc 80's inventory proved the mermaid scene renders at ~3% contrast,
the supervisor told the user lighting was "empirically the binding
constraint".  **That was an overreach from n=1**, and measuring the rest of
the runs contradicted it:

| scene | luma stdev | p99−p1 |
|---|---|---|
| Fable benchmark (frontier, same prompt) | 29.3 | 116 |
| agent dragon, arc 78 | **31.9** | **183** |
| agent dragon, arc 79 | 23.2 | 121 |
| agent mermaid2 | **0.4** | **1.7** |

Agent tonal range matches or exceeds the frontier benchmark on two of three
scenes.  **Agent lighting is not globally flat**; mermaid2 is a pathological
outlier that happened to be the scene in view.  Nothing in this arc may be
premised on flatness being the norm.

What the data DOES support is the original creative ask.

## 2. The actual gap

| | lights | power range | kinds used |
|---|---|---|---|
| agent runs (all of them) | 3–5 | ~40× (3.5 → 150) | omni, spot, directional |
| Fable benchmark | 8 | **~1000×** (1.6 → 2200) | omni, spot, directional |

Two facts fall out.  First, a 1000× power range is what a key/fill/rim
structure looks like numerically; a 40× range is one undifferentiated wash of
lights.  Second — and this is the surprise — **the frontier scene does not
use area lights either.**  Its god-rays are spots.  So the gap is structure
and range, not exotic light types.

But RISE also offers `hosek_wilkie_skylight`, `ambient_light`, and area/mesh
lighting through emissive materials, and **no agent run has ever used any of
the three.**  Whether that is a taste ceiling or simple non-discovery is the
question mechanism 2 tests.

## 3. Mechanism 1 — the tonal fact (free)

The arc-80 inventory payload gains a line describing the frame's luma
distribution — spread and concentration — computed from the beauty pixels
**already rendered**.  No extra pass; in deliberate contrast to the
inventory's ~130 ms identity render, this costs essentially nothing.

It must make the mermaid2 case unmistakable (stdev 0.4, p99−p1 = 1.7 of 255)
while saying something unremarkable about a healthy frame (stdev ~29).

FACTS ONLY: no "this looks flat", no threshold verdict.  The distribution is
stated; the model draws the conclusion.  This is the same discipline the
inventory ships under, and the same reason: a verdict would both contaminate
the measurement and risk a false clause in a model-facing payload.

## 4. Mechanism 2 — `light_scene`, a clean-room lighting pass

The clean room is the only mechanism in this workstream that ever moved a
number (arc 79: wizard 4 → 10 SDF parts).  Lighting is authored in precisely
the regime arc 79 relieved — the compose phase, 60–70 turns deep, with the
whole scene's history in context.

So: one fresh, minimal provider completion that designs the lighting, built
on `build_element`'s pattern (same host-mediated transport, same validated
insertion, same single repair retry, same honest partial reporting).

What it gets that `build_element` never could:

- **Arc 80's scene inventory** — object names, world bbox centres and sizes —
  so lighting is designed against where things actually are.  This
  composition is why the arc is possible now and was not a week ago.
- Camera, world bounds, the subject/mood, and any existing lights.
- **The full palette named explicitly**, with literal chunk syntax for each:
  omni, spot, directional, `ambient_light`, `hosek_wilkie_skylight`, and
  area/mesh lighting via emissive materials.  Presenting the three unused
  kinds in a minimal context IS the hypothesis.

It is NOT told to be dramatic, to use many lights, or to widen its power
range.  That is advice; advice measures ~0 here; and it would contaminate
exactly what is being measured.  Palette, scene, mood, syntax — nothing else.

Because this call runs once, N small solo renders are affordable where they
would never be per-render, so the result reports **each light's measured
contribution** (`LightSoloRestoreGuard` already exists).

Forced first use mirrors 79 §4: in COMPOSE the first light-authoring edit
must come from `light_scene`; afterwards hand refinement is open.  The seam
to respect: arc-78 §2.3 deliberately allows lights during element windows so
a model can see the part it is building — those must neither disarm this gate
nor be refused by it.

## 5. Measurement (pre-committed)

1. **HEADLINE: light-power range and light count** — 40×/3–5 today against
   the benchmark's 1000×/8.
2. **Do the three never-used kinds appear** (`ambient_light`,
   `hosek_wilkie_skylight`, emissive area lights)?  This is the direct test
   of palette-in-a-clean-context.
3. Tonal spread of the final render vs the benchmark's stdev 29 / range 116.
4. Does the model refine lighting after `light_scene`, or accept it whole?
5. Cost of both mechanisms; the tonal fact must be ~free.

Stop rules: kinds unchanged and range unchanged → the clean room does not
transfer from geometry to lighting, and the palette was not the constraint;
tonal spread unchanged AND composition still poor → lighting was never the
creative gap, and the workstream returns to composition and richness with
that settled.  Do not tighten into prescription (a minimum light count, a
required range) — that would manufacture the metric instead of moving it.

## 6. What this does NOT do

No judgement of lighting quality.  No automatic relighting.  No change to
geometry, placement or the camera.  No requirement to use any particular
light kind.  The clean room proposes; the model disposes.

---

## 7. AS BUILT (2026-08-12)

Both mechanisms shipped.  Nothing in §1–§6 changed; this section records the
decisions the design deliberately left to the build, and the numbers the
build produced.

### 7.1 The tonal fact — where it attaches, and the one place it differs from the inventory

Reported per full production beauty render, from the pixels already produced:
mean, standard deviation, the 1st-to-99th percentile span, the most common
luma level, and what fraction of the frame sits within **±5 levels** of it (a
FIXED band, so the figure is comparable between two frames, and the text
always states the band it used).  Luma is Rec.709 of the DISPLAYED 8-bit
pixels — the bytes a viewer and a vision model actually see, after exposure,
tone curve and colour space.

Calibrated against the two frames §3 requires it to separate:

```
FLAT   : FRAME TONE -- ... mean 114.0, standard deviation 0.00,
         1st-to-99th-percentile span 0 levels (114 to 114). The most common
         luma value is 114, and 100.0% of the frame lies within +/-5 levels of it.
NORMAL : FRAME TONE -- ... mean 41.8, standard deviation 63.35,
         1st-to-99th-percentile span 171 levels (0 to 171). The most common
         luma value is 0, and 66.7% of the frame lies within +/-5 levels of it.
```

**Suppression (the question §3 left open).**  It attaches to a strict SUBSET
of what the inventory attaches to: `renderMode == "production"` and not
`isolate`.

- **objectmap and the false-colour data modes** — suppressed.  Those pixels
  are identity or data colours; a luma histogram of them is a histogram of a
  palette, true of the bytes and about nothing.
- **isolate** — suppressed.  That frame is one object on a deliberately
  emptied scene, so most of its distribution is the background the isolation
  created, not the scene's tone.
- **draft** — suppressed, where the inventory ALLOWS it.  Draft pixels come
  from a fixed studio-preview shader that ignores the scene's authored
  lighting entirely, so a tonal number there would be true of the preview and
  read as a statement about the scene — the most expensive kind of true
  clause.
- The production-transport modes (deep_reflect / direct / indirect /
  clay_lights) fall out of the same `renderMode` test that excludes them from
  the inventory.

**Cost: 0.2 ms on a 256×192 frame** — one decode of the PNG the render had
already encoded.  Measured directly (`AgentProposeRenderTest`, arc 81 cost
line), because timing `Render()` twice cannot separate it: every suppression
that would turn the fact off also changes what gets rendered.

### 7.2 `light_scene` — the naming contract, and what bounds it

**Naming: there is NO prefix rule, deliberately.**  `build_element` enforces
`<element>_` because element chunks must be attributable — `place_element`
transforms everything recorded against an element, and the cross-element edit
rule has to know who owns what.  **Lights are scene-global**: arc 78 §2.3
exempts the whole Light category from every element-window rule,
`place_element` transforms only `standard_object`s, and in COMPOSE — the phase
this verb is designed for — no element is active and nothing is attributed at
all.  The prefix idiom's precondition does not hold, and transplanting it
anyway is the failure this repo has hit four times.  The real constraint is
UNIQUENESS, which `InsertChunks` already enforces: a colliding name is
rejected, never renamed (renaming would break the references the pass wrote
between its own chunks), and that rejection text is what drives the one repair
retry.

**The insertion is exempted from the compose-phase creation ban**, because an
area light legitimately creates geometry and that ban exists to stop a model
REPLACING form it already built (79 §8.2).  An exemption needs a bound, so the
pass admits only: any Light chunk; a Painter and a Material; and a Geometry or
Object ONLY when the same answer defines an emissive material — detected by
the descriptor registry (`exitance`), so a luminaire material added later is
covered with no edit.  Camera, film, rasterizer, rasterizer output and shader
op are rejected with a reason.  Re-aiming the camera is not lighting.

**Contributions** are measured by soloing every light source the renderer can
name — explicit lights, emissive objects, and the reserved `environment` —
in `RayCaster::SetSoloLightByName`'s own resolution order, capped at 8 with
the cap stated.  A rasterizer that cannot solo costs exactly ONE failed
render, after which every remaining entry carries the same stated reason.  The
report says outright that the figures **do not sum** to the all-lights frame;
transport through a path tracer's shadowing and MIS is not additive, and
implying otherwise would be a false clause.

```
light_scene ran one lighting completion on <provider>/<model>. Chunks inserted:
lit_key, lit_fill, lit_panel_pnt, lit_panel_mat, lit_panel_geo, lit_panel_obj.
Contribution of each light, measured by rendering the scene with only that light
active and taking the frame's mean Rec.709 luma on the 0-255 scale (the same
frame with every light active measures 54.3): obj_emit (emissive object) 41.9,
39% of the soloed total; lit_key (light) 40.8, 38% of the soloed total;
lit_panel_obj (emissive object) 17.6, 16% of the soloed total; lit_fill (light)
8.0, 7% of the soloed total. Light transport is not additive through this
renderer's shadowing and MIS, so those figures do not sum to the all-lights one.
```

**Availability.**  Callable in EVERY phase and with the staged build protocol
off: unlike `build_element` it needs no active element, and a protocol-off
session has no phases at all, so refusing it there would be the over-refusal
arc 78 §2.3 names as this design family's worst failure mode.  One stated
consequence: called from inside an element window its chunks are attributed to
the active element by arc 78's universal rule — inert for a light chunk, but
it would make `place_element` carry an area light along with the element.
COMPOSE, where the gate forces it, has no active element.

**Prompt size: 14,745 characters**, against `build_element`'s 14,421 on the
same fixture — the same order, so arc 79 §1's warning (60k of prepended text
halves construction richness) is respected.

**Cost: 9.4 ms of harness work per call** on a 24×24 fixture (one inventory
pass + one all-lights reference + 4 solo renders), plus the provider round
trip.  Paid ONCE per scene, which is the entire reason N solo renders are
affordable at all.

### 7.3 The gate, and the seam

`CheckFirstLightThroughCleanRoom_` is the FOURTH arm of `RefuseForPhase_`,
sharing its 3-refusal cap and give-up with the cross-element, compose-create
and first-geometry arms, and dying with `--agent-build-protocol=off` like all
of them.  It fires in COMPOSE only, on light-CHUNK-creating text only, and
lifts once `light_scene` has reached the provider — whatever came of it, so a
failed builder cannot strand the session.

The seam §4 flagged is honoured in both directions and pinned by test A81e:
pieces-phase lights are neither refused (the arm returns early outside
COMPOSE) nor disarming (the condition is *has light_scene run*, not *does the
scene have lights*).

### 7.4 Tests

`AgentProposeRenderTest` — the tonal fact: flat-vs-normal separation, the four
suppression surfaces plus the failed render, and the cost line.
`AgentChunkCrudTest` A81a–A81f — the lighting pass: happy path with the prompt
composition and the palette pins, admissibility and the one retry, the
duplicate-name contract, capability refusal and the spend cap, the COMPOSE
gate with its cap/give-up/protocol-off arms, the PIECES seam, and the wire
shape.  `SourceHygieneTest` gains an A81 pin block holding both model-facing
surfaces to the same palette, naming and measurement clauses.
