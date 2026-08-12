# Creative Lighting — Palette and a Clean Room (Arc 81 Design, 2026-08-12)

> **Status:** BUILDING.  User direction 2026-08-09 (*"an arc on creative
> lighting especially using area lights"*), selected 2026-08-12 over the
> competing "chase the flat scene" arc.  Depends on arc 80's scene inventory,
> which is what makes a lighting clean room possible at all.

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
