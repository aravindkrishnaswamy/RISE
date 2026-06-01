# PT Pel/NM Asymmetry Audit (PathTracingIntegrator `IntegrateFromHit`)

**Session date**: 2026-05-31 (diagnosis-only follow-up to Phase 2b part 2).
**Scope**: the three Pel/NM behavioral asymmetries in `IntegrateFromHitTemplated<Tag>`
that Phase 2b part 2 deliberately **preserved** (reproduced via `if constexpr (Traits::is_pel)`)
rather than fixed, under the "preserve, don't fix" templatization rule.

This is a **de-risking pass before Phase 2c (BDPT templatization)**. The question per
asymmetry: *is the divergence intentional (a correct per-color-mode difference), a latent
bug (one mode is wrong), a no-op (set-but-never-consumed), or unsure?*

**No integrator code was changed in this session.** The deliverable is the verdict table
below plus the per-asymmetry evidence. Fixes (if the user elects them) are separate,
independently-validated commits.

> Background: these asymmetries predate the refactor — the templatization only made them
> visible in one place. Origin: [PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md)
> §"Phase 2b part 2 outcome" → "Preserved asymmetries"; §"Phase 2b part 1" → "Bugs spotted".

---

> **STATUS 2026-05-31: asymmetries #1 and #3 are FIXED** (working tree, pending commit) — see
> [PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md) §"Session outcome (2026-05-31)". Both `if constexpr`
> divergences were collapsed so NM matches Pel; a third site (HWSS no-BSDF mid-path delegation,
> `IntegrateFromHitTemplated`-adjacent) needed a co-fix (`smsHad=true`) the #1 change exposed.
> Through-glass NM-SMS: **0.0 → 247** (Pel 248); diffuse→glass→light double-count counterfactual
> confirmed and closed (p99.9 floor tail: #1-only 19.45 → #1+#3 15.44 ≈ Pel 13.85). Regression
> fixture: `scenes/Tests/Spectral/sms_through_glass_emitter_pt_sms.RISEscene`. #2 remains NO-OP /
> untouched.

## TL;DR verdicts

| # | Asymmetry | Verdict | Blocks Phase 2c? | Recommended action |
|---|-----------|---------|------------------|--------------------|
| 1 | SPF/no-BSDF `considerEmission`: Pel `true`, NM `(isDelta && bSMS)?false:true` | **LATENT BUG (NM wrong)** — empirically confirmed — **✅ FIXED 2026-05-31** | **No** | ~~Isolated fix chip~~ DONE: NM SPF `considerEmission`→`true` for both tags + #3 co-fix + HWSS delegation co-fix. Validated (Gates 2–8). |
| 2 | BSSRDF/RW-SSS `rs2`: Pel sets `sms*` flags, NM sets `bsdfTimesCos` + `AccumulateCount` | **NO-OP (dead for the spectral renderer)** — **untouched** | **No** | None now. Revisit only if optimal-MIS is wired for spectral, **or** a SMS+SSS+specular-to-light scene is authored (then it becomes a coordinated integrator+shader-op fix). |
| 3 | PART3 BSDF-continuation flag tracking: Pel sets `bPassed/bHad`, NM does not | **NO-OP standalone, REQUIRED co-fix of #1** — **✅ FIXED 2026-05-31** | **No** | ~~Do not touch alone~~ DONE: un-gated the PART3 flag update so NM latches `bPassed/bHad`; landed with #1 (counterfactual confirmed it prevents the double-count). |

**De-risking answer:** **Phase 2c can proceed.** None of the three blocks BDPT
templatization. #2/#3 have no standalone observable effect; #1 is a real but **pre-existing,
PT-specific, faithfully-preserved** NM bug that belongs in its own fix chip. The one thing to
**carry forward into 2c**: asymmetry #1 is an instance of a transferable *pattern* — the NM
path uses a `considerEmission`-based emission-suppression scheme while Pel uses a
flag-predicate scheme, and they diverge on **camera→specular→light**. BDPT's Pel vs NM/HWSS
emission/delta-light handling should be checked for the same class of divergence during 2c
(see [docs/skills/bdpt-vcm-mis-balance.md](skills/bdpt-vcm-mis-balance.md) — the
"delta-position-light vs delta-surface-scatter trap").

---

## Shared machinery (read first)

Two distinct mechanisms can suppress BSDF-sampled emission at a vertex when SMS is enabled
(SMS already accounts for diffuse→specular*→light paths, so counting them again at the light
double-counts):

1. **`considerEmission` flag** — a per-bounce boolean carried in the iterative loop. PART1
   skips emission when it is false.
2. **`smsSuppressEmission` predicate** — `bSMSEnabled && bPassedThroughSpecular &&
   bHadNonSpecularShading`, evaluated in PART1
   ([PathTracingIntegrator.cpp:1812](../src/Library/Shaders/PathTracingIntegrator.cpp)).

PART1 emission gate
([PathTracingIntegrator.cpp:1814](../src/Library/Shaders/PathTracingIntegrator.cpp)):

```cpp
const bool smsSuppressEmission = bSMSEnabled
    && bPassedThroughSpecular && bHadNonSpecularShading;
if( pEmitter && considerEmission ) {
    if( smsSuppressEmission ) { /* skip — SMS handles it */ }
    else { /* add MIS-weighted emission */ }
}
```

The `bHadNonSpecularShading` clause is **load-bearing by design**. Its own comment
([:1805-1813](../src/Library/Shaders/PathTracingIntegrator.cpp), and verbatim in the
pre-refactor NM body at `807ac57c`):

> *"Without that check, camera->glass->light paths (with no diffuse receiver) would be killed."*

i.e. a camera ray that refracts/reflects straight through a specular surface to a light has
**no diffuse anchor for SMS to evaluate at**, so SMS never adds that contribution — the
emission must be kept. The guard exists to keep it.

- **Pel's scheme**: keep `considerEmission = true` at specular (SPF) continuations and rely
  on `smsSuppressEmission` (with its `bHadNonSpecularShading` guard) to do the suppression.
- **NM's scheme**: set `considerEmission = false` after delta scatters (in **both** the SPF
  and PART3 sections) and use `considerEmission` as the primary suppressor.

The two schemes agree on most paths but **diverge on camera→specular(SPF)→light** — that is
asymmetry #1.

**RAY_STATE defaults** ([IRayCaster.h:45-83](../src/Library/Interfaces/IRayCaster.h)):
`considerEmission` defaults `true`; `smsPassedThroughSpecular`/`smsHadNonSpecularShading`
default `false`; `bsdfTimesCos` is `RISEPel` and is **not** in the ctor initializer list
(default-constructed). `bsdfTimesCos` is consumed **only** by optimal-MIS full-integrand
training ([:1709](../src/Library/Shaders/PathTracingIntegrator.cpp),
[:1891](../src/Library/Shaders/PathTracingIntegrator.cpp)); it does not feed NEE or the BSDF
weight.

**Optimal-MIS is Pel-only.** `rc.pOptimalMIS` is assigned in exactly one place —
[PixelBasedPelRasterizer.cpp:144](../src/Library/Rendering/PixelBasedPelRasterizer.cpp)
(`rc.pOptimalMIS = pOptimalMISAccumulator`). The spectral path runs through
`PixelBasedSpectralIntegratingRasterizer : public virtual PixelBasedRasterizerHelper`
([:41](../src/Library/Rendering/PixelBasedSpectralIntegratingRasterizer.h)), whose
`PrepareRuntimeContext` calls the **helper base**
([:74](../src/Library/Rendering/PixelBasedSpectralIntegratingRasterizer.cpp)) and never sets
`pOptimalMIS`. So for any NM/spectral render `rc.pOptimalMIS == nullptr`
([RuntimeContext.h:103,182](../src/Library/Utilities/RuntimeContext.h)). This fact decides
asymmetry #2.

**Shader-op recursion boundary** — when a continuation re-enters via `caster.CastRay*` the
`RAY_STATE` is unpacked by the shader op and re-passed to the integrator. The two color modes
carry *different* fields across that boundary
([PathTracingShaderOp.cpp](../src/Library/Shaders/PathTracingShaderOp.cpp)):

- `PerformOperation` (Pel) passes `rs.smsPassedThroughSpecular, rs.smsHadNonSpecularShading`
  ([:89](../src/Library/Shaders/PathTracingShaderOp.cpp)) — SMS state survives recursion.
- `PerformOperationNM` extracts `bsdfTimesCosNM = rs.bsdfTimesCos.r`
  ([:119](../src/Library/Shaders/PathTracingShaderOp.cpp)) and passes it, but **does not pass
  the SMS flags** ([:121-129](../src/Library/Shaders/PathTracingShaderOp.cpp)) — they reset to
  default at every NM recursion boundary.

This shader-op asymmetry is the architectural root of #2 (and bounds the reach of any #1/#3
fix to the in-loop, non-recursive path).

---

## Asymmetry #1 — SPF/no-BSDF specular `considerEmission`

**Site**: SPF (no-BSDF) continuation,
[PathTracingIntegrator.cpp:2307-2333](../src/Library/Shaders/PathTracingIntegrator.cpp):

```cpp
bool nextConsiderEmissionSPF;
if constexpr ( Traits::is_pel ) {
    nextConsiderEmissionSPF = true;                                   // Pel: always re-enable
} else {
    nextConsiderEmissionSPF = ( pS->isDelta && bSMSEnabled ) ? false : true;  // NM: suppress
}
...
considerEmission = nextConsiderEmissionSPF;
```

The flag update right below
([:2342-2346](../src/Library/Shaders/PathTracingIntegrator.cpp)) is **shared** (both tags set
`bPassedThroughSpecular`/`bHadNonSpecularShading`). Only `considerEmission` diverges.

### Step A — what it does
Fires when a material has **no BSDF, only an SPF** (RISE's dielectric/mirror/glass), i.e. a
pure-specular surface, with SMS enabled and the selected lobe `pS->isDelta`. NM sets
`considerEmission = false` for the next vertex; Pel leaves it `true`.

Observable consequence on **camera→glass(SPF delta)→light** (no diffuse vertex):
- **Pel**: `considerEmission=true`; at the light `smsSuppressEmission = bSMS && bPassed(true)
  && bHad(false) = false` → **emission added** (correct: SMS cannot cover a path with no
  diffuse anchor).
- **NM**: `considerEmission=false` → PART1 short-circuits → **emission skipped**. The light
  viewed through the glass goes **black**. SMS does not add it back (no diffuse vertex), so it
  is pure **energy loss**.

### Step B — consumption trace
`considerEmission` is a loop-local carried into the **same call's** PART1 gate
([:1814](../src/Library/Shaders/PathTracingIntegrator.cpp)); no recursion boundary is
involved for the main camera path, so the divergent value is **directly consumed**. The
NM `considerEmission=false` *bypasses* the `bHadNonSpecularShading` guard that PART1 installs
specifically to protect this path — the two pieces of NM code contradict each other.

### Step C — git archaeology + reference reasoning
`git log -S` pins the origin to **`ce61ea6d "More SMS fixes"` (2026-04-21)**. That single
commit added to the **NM** body *both*:
- the PART1 guard `if( pEmitter && considerEmission && !smsSuppressEmission )` whose comment
  says *"Without the bHadNonSpecularShading clause the camera looking directly through glass
  at the light would be killed too (no diffuse vertex exists for SMS to cover that
  contribution)"*, **and**
- the SPF `const bool nextConsiderEmissionNM = ( pS->isDelta && bSMSEnabled ) ? false : true;`
  whose comment says *"Matches the RGB iterative continuation logic."*

The NM author modeled the NM **SPF** section on RGB's **PART3 (BSDF)** logic — which *does*
set `considerEmission=false` on delta — rather than on RGB's **SPF** logic (which keeps it
`true`). The result is an internal contradiction: the same commit that documents "must not
kill camera→glass→light" ships the SPF suppression that kills it. This is the fingerprint of
an **oversight, not a deliberate per-color-mode choice** — there is no physical reason
emission visibility through glass should differ between RGB and a single spectral wavelength.
Reference renderers (PBRT-v4, Mitsuba) keep direct specular-to-light emission in unidirectional
PT regardless of caustic-sampling state.

### Step D — empirical (READ-ONLY render)
Fixture (throwaway): a glass sphere (SPF dielectric, r=1.2) directly in front of a large
emissive sphere, dark elsewhere. Central camera rays are camera→glass(δ)→glass(δ)→light with
**no** diffuse vertex; rays around the glass hit the light directly (brightness reference).
Rendered with `pixelintegratingspectral_rasterizer` (NM) and `pathtracing_pel_rasterizer`
(Pel), SMS on vs off, 48 spp, `oidn_denoise false`.

Mean pixel value of the central through-glass disc (0–254):

| Render | center (through-glass) | result |
|--------|------------------------|--------|
| NM, SMS **off** | 253.8 | bright — light visible through glass ✔ |
| **NM, SMS on** | **0.0** | **black — light suppressed (BUG)** |
| Pel, SMS **off** | 254.0 | bright ✔ |
| Pel, SMS on | 254.0 | bright ✔ |

`max=254` in the NM-SMS-on render confirms the light *is* being rendered (the direct ring
around the glass survives) — only the through-glass interior is killed. Same scene, same SMS
setting, **Pel bright vs NM black: the difference is purely color mode.** Visual signature:
NM-SMS-on is a black disc with a thin grazing ring; Pel-SMS-on is a fully-lit disc identical
to NM-SMS-off.

This is decisive: enabling SMS must only **add** caustics; it darkened a direct refracted view
of a light only on the NM path.

### VERDICT — **LATENT BUG (NM wrong)** — ✅ FIXED 2026-05-31
> **Fixed**: NM SPF `nextConsiderEmissionSPF` is now unconditional `true` (both tags); the `if constexpr`
> collapsed. Landed with #3 + the HWSS delegation co-fix. Before/after on the regression fixture:
> through-glass disc 0.0 → 247 (Pel 248). See [PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md) §"Session
> outcome (2026-05-31)".

NM over-suppresses BSDF-sampled emission on **camera→SPF-specular(delta)→light** paths (no
intervening diffuse/glossy vertex) whenever SMS is enabled. Symptom: energy loss — lights
seen *directly through/in* glass or mirrors go black under spectral rendering with SMS on.
Pel is correct.

- **Trigger conditions**: SMS enabled (`pSolver != 0`) **and** a pure-SPF specular surface
  (glass/mirror/dielectric) reachable on a camera path **and** an emitter reached through a
  delta lobe with **no** diffuse/glossy shading point earlier on the path **and** NM/spectral
  rendering. Production reach: any spectral PT render
  (`pixelintegratingspectral_rasterizer` → `PerformOperationNM` → `IntegrateFromHitTemplated<NMTag>`)
  of such a scene. The canonical SMS test scenes are *caustic* scenes (light→glass→floor), not
  *direct-view* scenes, which is why the part-2 mean-luminance baselines (NM-pre vs NM-post,
  same behavior) did not surface it.
- **Recommended fix (its own chip, NOT this session)**: port NM to Pel's flag-based scheme —
  in `IntegrateFromHitTemplated`, make the SPF `nextConsiderEmissionSPF` unconditional `true`
  for both tags, **and** un-gate the PART3 flag update (asymmetry #3) so NM also tracks
  `bPassedThroughSpecular`/`bHadNonSpecularShading`. **#1 and #3 must land together** (see #3:
  fixing #1 alone makes NM under-suppress → double-count fireflies on diffuse→glass→light).
- **Risk**: changes pixels on spectral-PT + SMS scenes that contain camera→specular→light
  regions; neutral elsewhere. Validate with the K-trial EXR protocol
  ([docs/skills/variance-measurement.md](skills/variance-measurement.md)) plus a visual
  check on the fixture above (expect NM-SMS-on to match NM-SMS-off in the through-glass
  region) and a firefly/no-double-count check on a diffuse-receiver SMS scene
  ([docs/skills/sms-firefly-diagnosis.md](skills/sms-firefly-diagnosis.md)).

---

## Asymmetry #2 — BSSRDF / RW-SSS continuation `rs2` fields

**Sites** (identical pattern at both SSS entry points): BSSRDF diffusion
[PathTracingIntegrator.cpp:2061-2074](../src/Library/Shaders/PathTracingIntegrator.cpp) and
random-walk SSS
[:2217-2230](../src/Library/Shaders/PathTracingIntegrator.cpp):

```cpp
if constexpr ( Traits::is_pel ) {
    rs2.smsPassedThroughSpecular = false;
    rs2.smsHadNonSpecularShading = true;          // propagate SMS-suppression into SSS child
} else {
    rs2.bsdfTimesCos = RISEPel( std::fabs( sssThroughput ) * bssrdf.cosinePdf );
    if( rc.pOptimalMIS && !rc.pOptimalMIS->IsReady() && bssrdf.cosinePdf > 0 ) {
        const_cast<OptimalMISAccumulator*>( rc.pOptimalMIS )->AccumulateCount(
            rast.x, rast.y, kTechniqueBSDF );      // optimal-MIS bookkeeping
    }
}
```

The continuation re-enters via `PTCastRay<Tag>` → `caster.CastRay{,NM}` → shader op → a fresh
`IntegrateFromHitTemplated`. So unlike #1/#3 this divergence **crosses the recursion
boundary**.

### Step A — what it does
Pel propagates SMS emission-suppression state into the SSS sub-path (so an SSS-emerge →
glass → light child does not re-enable emission SMS already covers). NM instead records the
continuation's `bsdfTimesCos` and counts a BSDF technique sample for optimal-MIS.

### Step B — consumption trace (the decisive step)
- **Pel side is live**: `PerformOperation` reads `rs.smsPassedThroughSpecular,
  rs.smsHadNonSpecularShading` and passes them on
  ([PathTracingShaderOp.cpp:89](../src/Library/Shaders/PathTracingShaderOp.cpp)).
- **NM side is dead**:
  1. `AccumulateCount` is guarded by `rc.pOptimalMIS`, which is **null for the spectral
     renderer** (Shared machinery above) → it never fires.
  2. `rs2.bsdfTimesCos` *is* carried across by `PerformOperationNM`
     ([:119](../src/Library/Shaders/PathTracingShaderOp.cpp)), but in the recursive call
     `bsdfTimesCos` feeds **only** optimal-MIS training
     ([:1891](../src/Library/Shaders/PathTracingIntegrator.cpp)), also `rc.pOptimalMIS`-gated
     → never consumed.
  3. NM does not set the SMS flags — but `PerformOperationNM` would drop them anyway
     ([:121-129](../src/Library/Shaders/PathTracingShaderOp.cpp)), so setting them would have
     no effect either.

  Net: **the entire NM branch sets state that nothing reads on the spectral path.**

### Step C — git archaeology + reference reasoning
The Pel-side SSS suppression-flag propagation traces to the SMS-fix cluster of 2026-04-20/21
(`08261258 "Fixes caustics not being visible through refractive objects when using SMS"`,
`ce61ea6d`). The NM `bsdfTimesCos`/`AccumulateCount` form was the spectral author's parallel
of the optimal-MIS bookkeeping the RGB path does elsewhere — coherent *as RGB code*, but
optimal-MIS was never wired for the spectral rasterizer, so the spectral copy is inert. There
is no per-color-mode physics here; the divergence is an artifact of (a) optimal-MIS being
Pel-only and (b) `PerformOperationNM` not carrying SMS flags.

### Step D — empirical
Not separately rendered: with `rc.pOptimalMIS == nullptr` on the spectral path the NM branch
has **no reachable consumer**, so no scene can make it change a pixel. (The Pel-side flag
propagation is itself inert on all existing SSS test scenes — none combine SMS with a
specular-chain-to-light emerging from the SSS exit.)

### VERDICT — **NO-OP (dead for the spectral renderer as wired)**
The NM-side `bsdfTimesCos`+`AccumulateCount` is dead code because optimal-MIS is Pel-only.
The divergence has no observable effect on any current render.

There is a **latent gap** behind it: NM does not propagate SMS emission-suppression into SSS
continuations, while Pel does. It is **not** independently fixable at the integrator —
`PerformOperationNM` drops the flags too, so closing it is a *coordinated* integrator +
shader-op change. And it is only reachable by a SMS + SSS + specular-chain-to-light scene that
does not exist in the suite. **Recommendation**: leave as-is; revisit only if optimal-MIS is
extended to the spectral renderer (then the dead `bsdfTimesCos` would suddenly matter and
should be audited) **or** such a SMS+SSS scene is authored. Flag for the eventual SMS/MIS-flag
tidy, not urgent. (This is the "entangled with a larger scope" stop-rule case — the shader-op
SMS-flag propagation is itself Pel/NM-inconsistent; that broader inconsistency is noted, not
mapped here.)

---

## Asymmetry #3 — PART3 BSDF-continuation SMS-flag tracking

**Site**: PART3 BSDF continuation,
[PathTracingIntegrator.cpp:2816-2883](../src/Library/Shaders/PathTracingIntegrator.cpp):

```cpp
bool nextConsiderEmission = true;
if( pS->isDelta && bSMSEnabled ) {
    nextConsiderEmission = false;          // BOTH tags
}
...
considerEmission = nextConsiderEmission;   // BOTH tags
...
if constexpr ( Traits::is_pel ) {          // ONLY Pel updates the flags
    if( pS->isDelta ) { bPassedThroughSpecular = true; }
    else { bPassedThroughSpecular = false; bHadNonSpecularShading = true; }
}
```

Unlike #1, the `considerEmission` logic here is **identical** for both tags. Only the
`bPassedThroughSpecular`/`bHadNonSpecularShading` update is Pel-only.

### Step A — what it does
After a BSDF-sampled continuation, Pel maintains the specular-transition flags; NM does not.
The flags feed `smsSuppressEmission` at the next vertex's PART1.

### Step B — consumption trace
Loop-local; consumed by the same call's PART1
([:1812](../src/Library/Shaders/PathTracingIntegrator.cpp)). **But** in PART3
`considerEmission=false` is set on exactly the same condition (`pS->isDelta && bSMSEnabled`)
that would set `bPassedThroughSpecular=true` — so for the *immediate* next vertex,
`considerEmission` already does the suppression for both tags; the Pel flag update is
redundant there. The flags only matter when `considerEmission` is later re-enabled
(medium-scatter [:1660](../src/Library/Shaders/PathTracingIntegrator.cpp), or a subsequent
Pel SPF-delta) while `bPassedThroughSpecular` is still true.

Crucially, in NM `bHadNonSpecularShading` is set **only** by the SPF non-delta branch
([:2346](../src/Library/Shaders/PathTracingIntegrator.cpp)) — never by PART3 (this asymmetry).
For ordinary scenes (non-specular surfaces carry BSDFs and go through PART3), NM's
`bHadNonSpecularShading` **never latches true**, so NM's `smsSuppressEmission` is effectively
**always false** — NM relies entirely on `considerEmission`. This makes #3 a no-op *given #1
is present*: every diffuse→glass→light chain is already suppressed by `considerEmission=false`
(set in SPF for NM via #1, and in PART3 for both). Walking the cases (camera→diffuse→glass→light,
camera→diffuse→glass→diffuse→light) confirms Pel and NM reach the **same** add/suppress
decision. (This refines the part-2 doc's wording: NM does *not* in fact double-count
diffuse→glass→light today — `considerEmission` covers it. The only standalone divergence is a
contrived camera→diffuse→glass→**medium-scatter**→light path, where correctness is itself
ambiguous because SMS does not cover medium scatters.)

### Step C — git archaeology
Same origin cluster (`ce61ea6d`, `ce28c362 "Excise path-tree branching"`). The NM body never
had the PART3 flag update; the templatization preserved that absence
(`if constexpr (Traits::is_pel)`). Consistent with NM having been built around the
`considerEmission` scheme rather than the flag-predicate scheme.

### Step D — empirical
Not separately rendered: no standalone observable effect (the contrived medium case is not in
the suite and is ambiguous). #3's behavioral relevance is entirely as the **co-fix** of #1.

### VERDICT — **NO-OP standalone; REQUIRED co-fix of #1** — ✅ FIXED 2026-05-31
> **Fixed**: the `if constexpr(Traits::is_pel)` wrapper around the PART3 flag update was removed so NM
> latches `bPassedThroughSpecular`/`bHadNonSpecularShading` like Pel. Landed with #1. Counterfactual
> (#1-only build, #3 reverted) confirmed the predicted double-count fireflies (diffuse→glass→light
> floor p99.9 19.45 vs 15.44 with #3) — the co-fix closes it. See
> [PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md) §"Session outcome (2026-05-31)".

Today (with #1's `considerEmission=false` present) #3 changes no production pixel. But it is
**not** safe to "tidy" in isolation, and it is **mandatory** to fix alongside #1: if #1 is
fixed (NM SPF `considerEmission=true`) without also un-gating the PART3 flag update, NM's
`bHadNonSpecularShading` stays unlatched, so `smsSuppressEmission` stays false and
**diffuse→glass→light double-counts** (BSDF emission *and* SMS) → fireflies. Worked example:
camera→diffuse_floor→glass(SPF δ)→light — with #1-only, NM adds emission the SMS pass already
counted. **Recommendation**: fix #1 and #3 in a single commit ("port NM PT emission-suppression
to Pel's flag-based scheme"), validated together.

---

## Phase 2c de-risking — explicit answer

**Can Phase 2c (BDPT templatization) proceed without resolving these first? Yes.**

- **#2, #3**: no standalone observable effect → cannot affect a 2c baseline or review.
- **#1**: a real bug, but **pre-existing** (origin `ce61ea6d`, 2026-04-21), **PT-specific**,
  and **faithfully preserved** by Phase 2b (the templatization did not introduce or worsen it
  — verified: the pre-refactor NM SPF body at `807ac57c` already had the same
  `(isDelta && bSMS)?false` plus the shared flag update). BDPT is a different integrator whose
  Pel/NM/HWSS asymmetries are its own to audit during 2c. #1 should be a separate fix chip,
  sequenced at the user's discretion — it does not gate 2c.

**One pattern to carry into 2c.** Asymmetry #1 is a concrete instance of a class of bug:
*the NM path uses a `considerEmission`-style emission-suppression scheme while Pel uses a
flag-predicate scheme, and the two diverge on direct camera→specular→light*. When BDPT is
templatized, explicitly check that its Pel and NM/HWSS bodies handle **specular(delta)-to-light
emission and MIS** identically — this is exactly the "delta-position-light vs
delta-surface-scatter trap" the [bdpt-vcm-mis-balance skill](skills/bdpt-vcm-mis-balance.md)
warns about. Found early, it is a one-line `if constexpr` audit; missed, it ships as a silent
NM-only energy loss like #1.

---

## Method notes / reproducibility

- Integrator code was **not** modified. Evidence is code reading + `git log -S`/`git show` +
  two read-only renders of a throwaway fixture (glass sphere over an emissive sphere) under
  `pixelintegratingspectral_rasterizer` and `pathtracing_pel_rasterizer`, SMS on/off. Fixtures
  lived under `/tmp`; outputs went to the git-ignored `rendered/`. Working tree ends clean
  except this doc and the [PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md) pointer.
- Key citations: PART1 guard [PathTracingIntegrator.cpp:1812](../src/Library/Shaders/PathTracingIntegrator.cpp);
  SPF #1 [:2307](../src/Library/Shaders/PathTracingIntegrator.cpp);
  BSSRDF #2 [:2061](../src/Library/Shaders/PathTracingIntegrator.cpp),
  [:2217](../src/Library/Shaders/PathTracingIntegrator.cpp);
  PART3 #3 [:2816](../src/Library/Shaders/PathTracingIntegrator.cpp);
  RAY_STATE [IRayCaster.h:45](../src/Library/Interfaces/IRayCaster.h);
  shader op [PathTracingShaderOp.cpp:89,119](../src/Library/Shaders/PathTracingShaderOp.cpp);
  optimal-MIS wiring [PixelBasedPelRasterizer.cpp:144](../src/Library/Rendering/PixelBasedPelRasterizer.cpp);
  origin commit `ce61ea6d` (2026-04-21 "More SMS fixes").
