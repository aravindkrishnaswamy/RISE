# DL-09: the interior-segment basic-radiance factor in a graded-index medium

Status: **DERIVATION (commit 1 of the `debt-dl09` slice, 2026-09-28).**
Implementation, measurements and residuals are appended below it in later
commits of the same slice; this section is the part a reviewer checks
first, and it was written and committed before any code changed.

Background: [REFRACTIVE_RADIANCE_SCALING.md](REFRACTIVE_RADIANCE_SCALING.md)
§1 (the η² factor) and §10.2 (this row's history), and the DL-09 row of
[DEBT_LEDGER.md](DEBT_LEDGER.md).

## 1. The three earlier rounds, restated

1. **Round 1 (`ae475295`, reverted).** Substituted the SPF's fresh exit-hit
   index `n_B` for `RadianceEtaScale`'s stale `before.top()` (= `n_A`, the
   value pushed at entry) and added nothing else. A through-trip then nets
   `(1/n_A)²·(n_B/1)² = (n_B/n_A)²` — 2.25× at a 1.2 → 1.8 slab: an emitter
   behind a passive, lossless slab rendered brighter than the emitter.
   The stale read was never the bug; it was silently standing in for the
   missing interior factor.
2. **Round 2, read literally** ("apply the factor on the walk, not at the
   SPF exit read" = add `(n_A/n_B)²` and leave the exit read stale). A
   through-trip then nets `(1/n_A)²·(n_A/n_B)²·(n_A/1)² = (n_A/n_B)²` —
   0.444× at the same slab, the reciprocal (mirror image) of round 1.
3. **Round 3 (the row as it stands).** The interior factor and the switch
   of the exit read to the fresh value must land together; only an
   interior gather (a contribution that never reaches the exit) can tell
   "fixed" from "unfixed"; `n_C` is undefined for UV-driven painters; and
   the DielectricSPF exit-hit Snell trace uses a different index than the
   walk tracks.

## 2. Model and notation

RISE traces **straight** segments. Inside a medium whose index varies with
position the true rays curve, and two quantities are conserved along them:
the basic radiance `L/n²` and the étendue `n² dA dΩ`. A straight-ray
tracer can honour the first exactly and cannot honour the second. The
model this slice implements is therefore:

> **Basic radiance is continuous along every straight segment and across
> every interface.** Equivalently, a graded medium is treated as a
> continuum of infinitesimal index steps along the segment, each paying
> the ordinary interface factor, with no bending.

For an eye (RADIANCE) walk this is the familiar per-event factor
`(η_before/η_after)²` telescoped over the segment:
**`(n_start/n_end)²` per segment**, walk order. `n(x)` is the graded
painter's value at world point `x`; air is `n = 1`.

Fixture used for every number in this document (the red-proof scenes of
§6 below use it): a box `G` spanning `z ∈ [0, 2]`, index
`n(z) = 1.8 − 0.6·|z − 1|` (1.2 at both faces, 1.8 at mid-plane). Top face
`A` (`z = 2`, `n_A = 1.2`), interior diffuse floor `C` (`z = 0.02`,
`n_C = 1.212`), emitter `E` a one-sided plane at `z = 1` facing down
(`n_E = 1.8`), interior camera position `S` (`z = 0.5`, `n_S = 1.5`),
exit face `B` (for through-trips) with `n_B`.

## 3. The factor tables

"Today" = master `259495db`. "After" = this slice: every time a walk
reaches a new vertex `X` while the IOR stack's top medium is a graded
(world-position) field, it multiplies `(top/n(X))²` (RADIANCE) and sets
`top ← n(X)`; `RadianceEtaScale` is unchanged and so reads the UPDATED top
at an exit — that is the exit-read switch, and it cannot be separated from
the interior factor in this design (§4).

### (i) Full through-trip: camera(air) → A → B → air → emitter in air

| event | physics | today | after |
|---|---|---|---|
| entry at A | `(1/n_A)²` | `(1/n_A)²` (after→top = `n_A` pushed fresh) | `(1/n_A)²` |
| segment A → B | `(n_A/n_B)²` | 1 (none) | `(n_A/n_B)²`, top ← `n_B` |
| exit at B | `(n_B/1)²` | `(n_A/1)²` (stale top) | `(n_B/1)²` (updated top) |
| **net** | **1** | **1** | **1** |
| round 1 (reverted) | | `(n_B/n_A)²` = 2.25 | |
| round 2 literal | | `(n_A/n_B)²` = 0.444 | |

Today and after agree on every completed through-trip. This is the
invariant `RadianceEtaScaleGradedIndexTest` pins (PT/BDPT 0.290109 vs
`L·T1·T2` = 0.289909) and it must stay green throughout.

With an interior vertex `C` on the way (camera → A → C → B → air), after
the change: `(1/n_A)²·(n_A/n_C)²·(n_C/n_B)²·(n_B/1)² = 1` — the per-segment
factors telescope, so a through-trip is 1 however many interior vertices
it has.

### (ii) Interior NEE gather at C of an emitter E inside G (camera outside)

| quantity | physics | today | after |
|---|---|---|---|
| eye throughput arriving at C | `(1/n_A)²·(n_A/n_C)² = (1/n_C)²` | `(1/n_A)²` | `(1/n_C)²` |
| NEE connection segment C → E | `(n_C/n_E)²` | 1 | `(n_C/n_E)²` |
| **NEE contribution, total** | **`(1/n_E)²`** | **`(1/n_A)²`** | **`(1/n_E)²`** |
| BSDF-sampled hit of E from C | `(1/n_C)²·(n_C/n_E)² = (1/n_E)²` | `(1/n_A)²` | `(1/n_E)²` |
| camera walk hitting E directly (A → E) | `(1/n_E)²` | `(1/n_A)²` | `(1/n_E)²` |

The eye throughput at C moves from `(1/n_A)²` to `(1/n_C)²` exactly as the
row states. The row does not name the connection segment, and it is
required: without `(n_C/n_E)²` on the NEE arm, NEE would carry
`(1/n_C)²` while the BSDF-sampled hit of the same emitter carries
`(1/n_E)²`, and `w_nee·f_nee + w_bsdf·f_bsdf` would be a mixture of two
different integrands. With it, both strategies carry the same number and
MIS is untouched (the factor is a throughput term, never a density).

Fixture (camera outside, ortho along −z, floor ρ, emitter `L_e`):
physics pixel `= T_A·ρ·L_e/n_E²`; today `= T_A·ρ·L_e/n_A²`. Today/physics
`= (n_E/n_A)² = (1.8/1.2)² = 2.25`.

### (iii) Walk seeded INSIDE G (camera at S)

`IORStackSeeding::SeedFromPoint` fires axis-aligned probes from S and
pushes, for each containing object, the index its probe's FIRST SURFACE
HIT reported (`GetSpecularInfo` at a boundary point, `n_P`), not `n(S)`.
In the fixture the `+z` probe's first hit on G is the top face: `n_P = 1.2`.

| quantity | physics | today | after (seed at S + interior factor) |
|---|---|---|---|
| stack top at the camera | `n_S` | `n_P` = 1.2 | `n_S` = 1.5 |
| eye throughput at C | `(n_S/n_C)²` | 1 | `(n_S/n_C)²` |
| NEE at C of E, total | `(n_S/n_E)²` | 1 | `(n_S/n_E)²` |
| through-trip S → B → air | `(n_S/n_B)²·(n_B/1)² = n_S²` | `n_P²` | `n_S²` |

Fixture: physics pixel `= ρ·L_e·(1.5/1.8)² = 0.694·ρ·L_e`; today
`= ρ·L_e`, ratio 1.44. The two changes are again both needed: the
interior factor with the stale seed gives `(n_P/n_E)² = 0.444` (0.64×
physics); the seed fix alone changes nothing (no factor is ever applied
inside, so the seeded top is never read by an interior gather).

### (iv) Light (IMPORTANCE) subpaths and connections — an extension the row does not state

At a real interface an importance walk needs no explicit factor: the
refraction's direction mapping has a Jacobian that spreads or compresses
the sampled rays, and the light-tracing estimator's density picks up
`(n_i/n_t)²` from that mapping implicitly (§1 of
REFRACTIVE_RADIANCE_SCALING.md). A **straight** graded segment has no
direction mapping and therefore no implicit factor. The path
contribution in the model of §2 carries, for every graded segment, the
factor `(n_cameraside/n_lightside)²` regardless of which walk built the
segment, so:

| segment built by | factor, in that walk's own order |
|---|---|
| eye walk, `x_i → x_{i+1}` | `(n_start/n_end)²` |
| light walk, `y_j → y_{j+1}` | `(n_end/n_start)²` |
| connection eye vertex `e` ↔ light vertex `l` (incl. NEE, BDPT s=1, t=1 splat) | `(n_e/n_l)²` |

Check, camera inside G at S, BDPT light tracing: light walk E → C′
`(n_C′/n_E)²`, splat C′ → S `(n_S/n_C′)²`, product `(n_S/n_E)²` = PT's
(iii). Today both are 1 — consistent with each other and both wrong — so
a PT-vs-BDPT comparison cannot red-prove this; it pins a HALF fix (eye
factor without the light/connection factor), which would make them
disagree by `(n_S/n_E)²`.

A VCM merge joins an eye vertex and a light vertex at the same point: no
segment, no factor; the two throughputs already telescope to
`(n_cam/n_E)²`.

## 4. Why the exit-read switch comes for free here, and why that matters

The factor is implemented as an UPDATE of the IOR stack's top value
(`top ← n(X)`) at every vertex reached inside a graded medium, paid for by
`(top/n(X))²`. The exit crossing's `RadianceEtaScale(before, after)` then
reads `before.top() = n_B` because the walk updated it on arrival at B.
There is no code path that applies the interior factor without also
switching the exit read, or vice versa: both rounds' failure modes are
unreachable by construction. And every factor is `(top/n_new)²` with
`top` whatever the stack holds, so a vertex the update misses (a site out
of scope, §8) does NOT break any later factor — the next update telescopes
over it; only a gather AT the missed vertex keeps today's error.

Constant-index media: the painter is not a world-position field, the
material reports no graded field, the helper returns before evaluating
anything, and the factor is exactly 1 with no floating-point operation —
bit-identical renders.

## 5. What this does NOT fix: ray bending (straight-ray geometry)

The model gets the RADIANCE along each straight segment right. It does not
bend rays, so where bending changes WHICH points a set of directions
reaches, the answer is still approximate — before and after this slice.
Configurations where the straight model is exact:

- radiance along a single ray parallel to `∇n` (the camera ray of §3(ii)
  and (iii) with an orthographic camera looking along the gradient);
- irradiance at C from a strata-parallel emitter plane that covers the
  hemisphere, when `n` is non-decreasing from C toward the emitter (curved
  rays then also reach the plane from every upward direction, so the
  solid-angle set is the same; the fixture is built this way);
- anything in a constant-index region.

Where it is not: a SMALL emitter on the gradient axis at distance `d`
from C. Curved rays collimate the pencil; with `D = ∫dz/n`, physics
`E_C ∝ L_e/(n_E²·D²)` against the model's `L_e·(n_C/n_E)²/d²`, so the
model/physics ratio is `(n_C·⟨1/n⟩)²` (0.658 for `n` 1.2 → 1.8 linear),
while today's ratio is `(n_E·⟨1/n⟩)²·(n_C/n_A)²` (1.48 at `n_A = n_C`).
Both are first-order in `Δn/n`; neither is a radiometric statement. This
is the same approximation as the exit-Snell item below and is filed with
it.

**The DielectricSPF exit-hit Snell trace (the row's "second
inconsistency").** After this slice the walk's tracked index at an exit
hit IS the fresh `n_B` the SPF Snell-tests against, so the inconsistency
AS THE ROW DEFINES IT (SPF index ≠ tracked index) is gone for the walks
this slice covers — and the exit event is radiometrically self-consistent
(its explicit factor `(n_B/1)²` matches the Jacobian of the Snell map it
actually traces). It is still GEOMETRICALLY wrong for a stratified slab:
the true invariant is `n·sinθ` along the curved ray, so a ray entering
from air at `θ_0` must leave a parallel-faced slab at `θ_0`; the straight
segment carries `sinθ_A = sinθ_0/n_A` to the exit and Snell at `n_B` gives
`sinθ_out = (n_B/n_A)·sinθ_0`. At 1.2 → 1.8: 30° exits at 48.6°, and every
entry angle above 41.8° is TOTALLY INTERNALLY REFLECTED at the exit face
(55.6 % of cosine-weighted incident light) where physics transmits all of
it. Snell-testing at the carried `n_A` would be exact for a stratified
slab and wrong for any other gradient; the general answer needs curved
rays. That is a ray-geometry change, not a radiance-accounting one, it is
unchanged by this slice (directions are untouched), and it is deferred
with these numbers as **DL-293**.

## 6. Verdict on the round-3 analysis

Correct in substance, and not re-derived here as wrong: the missing factor
is real, the pairing is mandatory, the interior-gather error is
`(n_C/n_A)²` on the eye throughput, the seeded-inside error is real,
`n_C` is undefined for UV-driven painters. Two places where round 3 is
INCOMPLETE (both anticipated by the slice brief, neither contradicted by
the row): the NEE / connection segment needs its own factor (§3 ii), and
importance walks need an explicit factor on graded segments because a
straight segment has no Jacobian to supply it implicitly (§3 iv). One
place where the recipe's wording is satisfied vacuously and should not be
mistaken for physics: the exit Snell trace becomes "consistent with the
tracked medium" automatically, and remains geometrically approximate
(§5).
