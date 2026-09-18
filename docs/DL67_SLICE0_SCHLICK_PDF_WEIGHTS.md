# DL-67 Slice 0 — `SchlickSPF::Pdf`/`PdfNM` is the density of what `Scatter` emits

Scope: make `SchlickSPF::Pdf`/`PdfNM` return the actual probability
density of the direction the integrator continues along. That is the
shared prerequisite the DL-67/DL-69 design note calls "Slice 0"; it does
**not** touch DL-69 (BDPT/VCM ordinary throughput) or the rest of DL-67
(PT/BDPT guided RIS / one-sample candidates), which consume this fix but
are separate slices.

Status: **landed 2026-09-14** on branch `debt-slice0`. This document
supersedes the first version of this slice, whose fix was measured
afterwards and found to move the density FURTHER from the sampler on 6
of the 8 review configurations; §6 records what that version got wrong,
because the mistake is instructive.

---

## 0. What `Scatter()` emits

`SchlickSPF::Scatter`/`ScatterNM` draw the diffuse ray and the specular
ray **unconditionally, every call**, subject only to geometric
accept-checks, and push both into the `ScatteredRayContainer`:

- Diffuse: `d.kray = pDiffuse->GetColor(ri)` — a pure function of the
  shading point. It does **not** vary with the diffuse lobe's own
  sampled direction. `d.pdf = cosTheta/pi`.
- Specular: `s.kray = rho + (1-rho)*fresnel`, `fresnel = (1-hdotk)^5`,
  `hdotk = dot(h, wi)` with `h` the sampled half-vector — a function of
  the specular lobe's **own** sampled direction. `s.pdf` is the
  half-vector sampling density (`ComputeSchlickSpecularPdf`).
- With a per-channel roughness/isotropy painter, `Scatter` (not
  `ScatterNM`) emits up to **three** specular rays instead of one, all
  three from **the same random pair**.

Both accept-checks are `dot(dir, myonb.w()) > 0 && dot(dir, geomN) > 0`.
The geometric one is not decoration: a specular draw failing it is the
single largest term in the answer below, and under a tilted shading
normal the *diffuse* ray fails it too.

`ScatteredRayContainer::RandomlySelect` then picks one of the survivors
with probability proportional to `MaxValue(kray)` — exactly
`PTScatterSelectWeight` (`PathTracingIntegrator.cpp`). Two of its
branches matter here and are easy to miss:

- `freeidx == 1` returns that single ray **with probability 1**,
  whatever its weight.
- a 2+-ray container whose weights sum below `NEARZERO` returns
  **nothing**.

---

## 1. The density

Write `p_D`, `p_i` for the diffuse / i-th specular sampling densities,
`w_D = MaxValue(rd)` and `w_i(omega) = rho_i + (1-rho_i)*fresnel(omega)`
for the realized selection weights. The density of the SELECTED
direction, evaluable at an arbitrary `omega`, is

```
f(omega) = C_D * p_D(omega) * 1{omega passes the diffuse gate}
         + sum_i q_i(omega) * p_i(omega)
```

with

```
C_D        = E_{(u,v) ~ U[0,1]^2} [ P(the diffuse ray wins | that specular draw) ]
q_i(omega) = P(specular lane i wins | lane i drew omega)
```

Both are expectations over the OTHER lobes' draws, because
`RandomlySelect`'s denominator contains them. Spelled out for the
single-lane case, with `A_D` the probability the diffuse ray survives
its own gate:

```
C_D = E_{(u,v)}[  1                      if the specular draw was rejected
                  w_D/(w_D + w_S)        otherwise (and 0 if that sum underflows) ]

q_S(omega) =   A_D   * w_S(omega)/(w_D + w_S(omega))      (diffuse present)
           + (1-A_D) * 1                                   (diffuse absent -> freeidx==1)
```

Integrating: for every specular draw the two conditional probabilities
sum to 1, so

```
integral f = A_D + (1 - A_D) * P(specular accepted)
           = P(Scatter emits anything at all)
```

which is exactly 1 when the shading and geometric normals agree, and
strictly less under a tilt — correctly, because `Scatter` really does
return an empty container on some draws there.

`A_D` has a closed form. Malley's disk projection turns
"cosine-weighted hemisphere about `n`, clipped by the plane of `geomN`"
into a half-disk plus a half-ellipse of semi-axes `(cos phi, 1)`, so

```
A_D = (1 + cos phi)/2,   cos phi = dot(n, geomN)
```

— the same closed form DL-45 uses for `TranslucentSPF`'s tilted exit.
Verified against the sampler at 20/40/55 degrees of tilt: predicted
emission probabilities 0.99049 / 0.96060 / 0.92516 against measured
0.99038 / 0.96071 / 0.92505 over 600 000 draws.

---

## 2. What the implementation does

`SchlickSPF.cpp`, all inside `Pdf`/`PdfNM` and the helpers they call.
`Scatter`/`ScatterNM` are unchanged in behaviour.

**`C_D` by deterministic quadrature.** No closed form exists (§3 says
why), so `SchlickDiffuseSelectCoefficient` integrates the sampler
itself: a stratified grid of midpoints in `GenerateSpecularRay`'s own
`(xi, b)` inverse-CDF unit square, each node **replayed** through
`SchlickSampleHalfVector` — the same function the sampler calls, so the
two cannot drift — and put through Scatter's own accept-checks and
`RandomlySelect`'s own branch structure. Deterministic, not stochastic:
an MIS weight that wobbled per call would not partition to one.

**`q_i` exactly.** `GenerateSpecularRay` reflects the incoming ray `d`
about its sampled `h`, so `wo = d - 2(d.h)h` and, with `wi = -d`,
`wi + wo = 2(h.wi)h`. Whenever `h.wi > 0` — the sampler's own accept
condition — `normalize(wi + wo)` recovers exactly that `h`, hence
exactly that fresnel. No approximation is needed on the specular side
at all.

**Per-channel lanes.** The three lanes share one random pair, so a
query direction determines the other two lanes' directions.
`SchlickInvertSpecular` inverts the sampler at the query direction (the
theta warp inverts as `xi = c2*r/(1-c2+c2*r)`; the azimuth warp is
monotone and bijective within each of its four quadrants, inverted by
`SchlickInvertPhi`), and the other lanes are replayed from the recovered
pair. **Exact for the INTENDED per-channel semantics** — i.e. exact
under the model where each lane's `GenerateSpecularRay` call writes its
OWN direction into its OWN `ScatteredRay`. Before **DL-101** was closed
(2026-09-17, debt-dl100 slice) it was not exact for what `Scatter`'s
per-channel branch actually did: that branch reused ONE `ScatteredRay`
across all three lanes, so a lane whose own `hdotk <= 0` silently kept
the PREVIOUS lane's direction while still being assigned the CURRENT
lane's `kray`/`pdf` — a sampler defect this file's density model did
not, and could not, replicate (§7 sibling audit;
`tests/SchlickSPFPdfConsistencyTest.cpp`'s `DL-101 KNOWN-FAILURE` row
demonstrated it directly: a duplicate-
direction counter found 111613/200000 corrupted draws for a config with
low-then-high per-channel roughness at grazing incidence, and the row's
measured TVD, 0.01424 against the file's own 0.012 gate, dropped to
0.01009 — a real but partial reduction, not a full collapse — when
`SchlickSPF.cpp`'s per-channel loop is patched to declare a fresh
`ScatteredRay` inside the loop instead of reusing one outside it).

**Branch parity.** `Pdf` now branches on the same
`HasPerChannelVariation()` predicate `Scatter` branches on, and uses
each lane's own `(r, p)`. It used to average the three roughness
channels unconditionally, which is neither branch's behaviour.

**Cost.** The grid is a product grid, so each lane's `kSpecQuadN`
half-angle values and `kSpecQuadN` azimuth values are computed once per
call and the inner loop is ~40 flops with no library calls — no `acos`
(`cos(acos(x)) == x`), no `pow` (`(1-hdotk)^5` as four multiplies), and
no `Normalize` on the replayed direction, since normalizing cannot
change the sign of the two accept dots.

---

## 3. Why no closed-form proxy works

The first version of this slice set
`C_D = MaxValue(rd) / (MaxValue(rd) + SchlickFresnelAvg(rs))`, reusing
DL-64's hemispherical Schlick average as a proxy for
`E_{p_S}[w_S(omega_S)]`. Three things are wrong with that, in
increasing order of how much they matter. Measured with a 2000x2000
quadrature of the sampler's own `(xi,b)` square (`A` = the fraction of
specular draws `Scatter` accepts; `C_D` here agrees with the library's
own empirical value to <= 0.0007 on every row, which is what makes it a
usable reference):

| config | `A` | `E[w_S given accepted]` | `MaxValue(rs)` | `SchlickFresnelAvg(rs)` | true `C_D` | `A*rd/(rd+rs)+(1-A)` |
|---|---|---|---|---|---|---|
| th=30 rd.5 rs.3 r.3 i.8 | 0.7386 | 0.3015 | 0.30 | 0.3333 | 0.72216 | 0.72303 (+0.12%) |
| th=60 rd.5 rs.3 r.3 i.8 | 0.6402 | 0.3232 | 0.30 | 0.3333 | 0.74933 | 0.75991 (+1.41%) |
| th=80 rd.5 rs.02 r.2 i1 | 0.5767 | 0.1961 | 0.02 | 0.0667 | 0.86020 | 0.97782 (+13.67%) |
| th=75 rd.7 rs.1 r.5 i.6 | 0.5299 | 0.1555 | 0.10 | 0.1429 | 0.90809 | 0.93377 (+2.83%) |
| th=45 rd.2 rs.6 r.15 i1 | 0.8065 | 0.6037 | 0.60 | 0.6190 | 0.39420 | 0.39511 (+0.23%) |
| th=45 rd.05 rs.9 r.4 i1 | 0.6590 | 0.9009 | 0.90 | 0.9048 | 0.37565 | 0.37568 (+0.01%) |

1. **Wrong measure, but only mildly.** The expectation runs under `p_S`,
   the half-vector sampling density, not the cosine measure
   `SchlickFresnelAvg` averages against. The table's third and fifth
   columns show the damage is modest where `rs` is large (0.9009 vs
   0.9048) and large where it is small (0.1961 vs 0.0667, a factor of 3
   at 80 degrees incidence). This alone would not have justified the
   rewrite — which is exactly why it is worth writing down: the proxy is
   a *plausible* estimate of `E[w_S]`, and plausibility is what made the
   first fix look finished.

   (An earlier draft of this section claimed `E_{p_S}[w_S]` is "within
   0.15% of `MaxValue(rs)` itself", on the strength of
   `SchlickBRDF::albedo`'s "integrated reflectance simplifies to Rd+Rs".
   That is **refuted** by the table: it holds only where `rs` dominates
   the Fresnel boost — 0.5% at `rs=0.3`/30 degrees, 0.1% at `rs=0.9` —
   and fails by 56% at `rs=0.1`/75 degrees and by 9.8x at `rs=0.02`/80
   degrees. The claim is withdrawn.)

2. **`C_D` is not a function of `E[w_S]` at all.** It is
   `E[w_D/(w_D + w_S)]`, and that function is convex in `w_S`, so
   substituting ANY average of `w_S` into it gives a different number by
   Jensen — before asking whether the average itself was right.

3. **The term that dominates is not a reflectance.** What actually
   drives `C_D` is the specular sampler's **rejection rate**: `Scatter`
   accepts only 0.739 / 0.640 / 0.577 of its specular draws at 30 / 60 /
   80 degrees incidence (table column `A`; the 80-degree row also has a
   lower roughness and no anisotropy, so it is not a pure angle series),
   and a rejected specular draw leaves `RandomlySelect` holding one ray,
   returned with probability 1. `A` is a property of the *geometry* —
   incidence angle, roughness, anisotropy, the geometric normal — and no
   average over reflectances can see it. The last column shows that even
   a model built AROUND `A` (`A*rd/(rd+rs) + (1-A)`, which is what a
   rejection-aware closed form would look like) still misses by up to
   13.7%, because it re-linearises the same convex function point 2
   rules out.

So there is no closed form to reach for, and the honest options were a
quadrature of the sampler or nothing. (An earlier draft also claimed
that `A*rd/(rd+rs)+(1-A)` "reproduces the Monte-Carlo `C_D` to 0.15%".
Also **refuted** by the table's last column — 0.01% to 13.67% — and
withdrawn.)

## 4. Measurements

### 4a. Choosing the quadrature

Error of `C_D` against a converged reference, over **400 randomised**
`(incidence, rd, rs, roughness, isotropy, tilt)` configurations:

| grid | cells | mean abs err | max abs err |
|---|---|---|---|
| 8 x 8 | 64 | 0.0083 | 0.039 |
| 12 x 16 | 192 | 0.0047 | 0.032 |
| 16 x 12 | 192 | 0.0037 | 0.023 |
| **16 x 16** | **256** | **0.0034** | **0.022** |
| 20 x 20 | 400 | 0.0025 | 0.013 |
| 32 x 32 | 1024 | 0.0014 | 0.0083 |

The convergence is `O(1/N)`, not `O(1/N^2)`, because the integrand is
bounded in `[0,1]` and smooth apart from one curve — the accept
boundary. `kSpecQuadN = 16` is the setting that keeps every
configuration in the gate below under 1%; it is the one knob if the cost
ever needs trading back.

Two warped grids were tried and **measured worse**, which is worth
recording because both look like obvious improvements: stratifying
uniformly in `cos(theta_h)` (max error 0.062 at 64 cells) and uniformly
in `theta_h` (0.022 at 64 cells) buy resolution at the tangent end by
starving the near-mirror end, which carries most of the probability mass
at low roughness. Plain midpoints in the sampler's own `xi` win.

**Two different quadratures, not to be confused (round-2 review note).**
The grid above is `kSpecQuadN` — `SchlickSPF.cpp`'s INTERNAL replay grid
for `C_D`, evaluated once per `Pdf()` call regardless of the query
direction. §4c's TEST HARNESS has a SEPARATE, independent hemisphere
quadrature (`kQT`/`kQP` in `tests/SchlickSPFPdfConsistencyTest.cpp`,
400x800 by default) that integrates the already-computed `Pdf(wo)` over
the OUTGOING direction to check normalisation — this is the reader's
own instrument, not part of the implementation, and it has its own,
independent convergence requirement. A direct 200x400 -> 3200x6400
sweep (P2-4) found the 400x800 default converged (<3e-4 drift) for the
gate's own control config AND for a roughness-0.05 config at moderate
incidence, but NOT for roughness 0.02 at theta=85 — that combination
needs the finer 2000x4000 grid `tests/SchlickSPFPdfConsistencyTest.cpp`
now uses for its lowest-roughness rows (see that file's own comment for
the numbers), and even that grid leaves a small (~0.2-0.3%) quadrature-
only residual on top of `kSpecQuadN`'s own bias there (§4f).

### 4b. Two further defects found while measuring

Both are the same one-sentence pattern as the slice itself — *`Pdf`
does not describe what `Scatter` draws* — and both are fixed here.

**The anisotropic azimuthal density was the wrong distribution.**
`ComputeSchlickSpecularPdf` returned
`p/(2 pi (cos^2 phi + p^2 sin^2 phi))`. `GenerateSpecularRay` draws,
per quadrant,

```
phi = (pi/2) * sqrt(p^2 v^2 / (1 - v^2 + v^2 p^2)),   v ~ U[0,1)
```

i.e. with `t := phi/(pi/2)` measured from the quadrant's own axis,
`v = t/sqrt(p^2 + t^2(1-p^2))`, hence

```
p(phi) = |db/dphi| = (1/4)|dv/dt|(2/pi) = p^2 / (2 pi (p^2 + t^2(1-p^2))^(3/2))
```

Both forms integrate to 1 over `[0, 2pi)` and both agree at `p = 1`,
which is how this survived. Away from `p = 1` they are different
distributions. Against a 2 000 000-draw histogram of the real sampler:

| isotropy `p` | `phi` | histogram | shipped formula | corrected formula |
|---|---|---|---|---|
| 1.0 | 0.131 | 0.15907 | 0.15915 | 0.15915 |
| 0.8 | 1.702 | 0.11120 | 0.19706 | 0.11132 |
| 0.6 | 1.702 | 0.06778 | 0.25746 | 0.06735 |
| 0.3 | 0.131 | 0.46732 | 0.04850 | 0.47917 |
| 0.3 | 1.702 | 0.01806 | 0.45256 | 0.01813 |

— up to **25x** wrong. The corrected form matches the histogram to
Monte-Carlo noise at every `p` tested.

**The density was built in the wrong frame.**
`ComputeSchlickSpecularPdf` mirrored `Scatter`'s `FlipW`, with a comment
asserting that "GenerateSpecularRay() builds its half-vector relative to
the (possibly flipped) myonb". It does not: that function's `onb`
parameter was **dead** and it always built `h` in `ri.onb`. (The
parameter is now removed so this cannot be misread again.) The density
now uses `ri.onb` too. As a side effect it returns 0 on a back-face hit,
which is the right answer — `Scatter`'s every specular draw is rejected
there. That the *sampler* loses its whole specular lobe on a back-face
hit is a real energy defect; it was filed as **DL-100**, not fixed here
(fixing it changes what renders sample and needed its own red-proof) --
**CLOSED 2026-09-17** in the debt-dl100 slice: `SchlickSampleHalfVector`,
`GenerateSpecularRay`, and every density helper here now consistently
take the caller's `myonb` instead of `ri.onb` (so `ComputeSchlickSpecularPdf`
now correctly reads NONZERO on a back-face hit too, matching what the
fixed sampler emits there). See
[DL100_SCHLICK_BACKFACE_SPECULAR.md](DL100_SCHLICK_BACKFACE_SPECULAR.md).

### 4c. The gate

`tests/SchlickSPFPdfConsistencyTest.cpp`, 12 configurations (the 8
review configurations, the per-channel branch, three tilts), RGB plus
the spectral twin on 5 of them, 34 checks.

`int Pdf` is a 400x800 hemisphere quadrature; `emitted` is the measured
fraction of 600 000 real `Scatter()` calls that produced any ray; `TVD`
is the total variation between the Pdf quadrature and a histogram of
600 000 real `Scatter()` + real `RandomlySelect()` draws over 12
equal-cos-theta x 8 equal-phi bins.

| configuration | int Pdf (4325f365) | TVD (4325f365) | int Pdf (fixed) | TVD (fixed) | emitted |
|---|---|---|---|---|---|
| th=10 rd.5 rs.3 r.3 i.8 | 0.88760 | 0.0627 | 1.00011 | 0.0070 | 1.00000 |
| th=30 rd.5 rs.3 r.3 i.8 | 0.88017 | 0.0642 | 1.00162 | 0.0072 | 1.00000 |
| th=45 rd.2 rs.6 r.15 i1 | 0.84998 | 0.0750 | 0.99545 | 0.0081 | 1.00000 |
| th=60 rd.5 rs.3 r.3 i.8 | 0.86050 | 0.0876 | 0.99993 | 0.0072 | 1.00000 |
| th=75 rd.7 rs.1 r.5 i.6 | 0.95345 | 0.0680 | 1.00181 | 0.0074 | 1.00000 |
| th=45 rd.9 rs.05 r.4 i1 | 0.94447 | 0.0278 | 0.99966 | 0.0072 | 1.00000 |
| th=45 rd.05 rs.9 r.4 i1 | 0.67672 | 0.1616 | 0.99520 | 0.0077 | 1.00000 |
| th=80 rd.5 rs.02 r.2 i1 | 1.02215 | 0.0125 | 0.99633 | 0.0075 | 1.00000 |
| per-channel roughness | 0.87051 | 0.06744 | 1.00624 | 0.0077 | 1.00000 |
| tilt 20 deg th=45 | 0.84854 | 0.0736 | 0.98913 | 0.0077 | 0.99038 |
| tilt 40 deg th=45 | 0.79061 | 0.0854 | 0.95989 | 0.0072 | 0.96071 |
| tilt 55 deg th=30 | 0.72688 | 0.0996 | 0.92731 | 0.0060 | 0.92505 |

`Checks: 34 Failures: 34` at `4325f365`; `Checks: 34 Failures: 33` at
pre-slice master `32824325` (the `rd=0.9` row's TVD lands at 0.01162,
just inside the 0.012 gate; its normalisation check still fails);
`Checks: 34 Failures: 0` after.

**Round-2 review additions**: 4 gate rows at roughness 0.05 (theta
15/45/70) and roughness 0.02 (theta 85) — P2-4, matching
`scenes/Tests/BDPT/cornellbox_bdpt_materials_pt.RISEscene`'s
`schlick_material` (`rd=0.6, rs=1.0`) — plus 1 control row that was
non-gated KNOWN-FAILURE for DL-101 until that row closed 2026-09-17 (see
docs/DL101_PERCHANNEL_SCATTEREDRAY_REUSE.md) and is now a real gate.
Current total: `Checks: 44 Failures: 0` on this file's HEAD (was 43
while the DL-101 row was non-gated).

The TVD threshold, 0.012, is derived rather than tuned. For a histogram
of `N` draws over `K` bins,

```
E[TVD] = (1/2) sum_k E|phat_k - p_k|
       ~ (1/2) sqrt(2/pi) sum_k sqrt(p_k(1-p_k)/N)
      <= (1/2) sqrt(2/pi) sqrt(K/N)            (Cauchy-Schwarz)
       = 0.399 * sqrt(96/600000) = 0.0051
```

and the measured post-fix values (0.0060-0.0081) sit close to that
floor, with the gate at ~2.4x above it. The whole test is deterministic
(fixed RNG seeds), which is what permits a threshold that tight.

**P2-2 correction (round-2 review): the residual is NOT pure bin
noise.** The previous sentence here claimed it was — refuted by a
half-split check that isolates the true noise floor from the real
signal: 4,000,000 draws on the `th=30 rd.5 rs.3 r.3 i.8` control row,
split into two independent 2,000,000-draw halves, gives a TVD BETWEEN
THE HALVES (pure statistical noise by construction, since both halves
sample the identical distribution) of 0.0037 — while the TVD of the
FULL histogram against the `Pdf()` quadrature (the real signal this
gate measures) is 0.0062, **1.68x** the noise floor. The round-2
review's own broader sweep (this file's 12 original rows, 4,000,000
draws each) reports the noise floor at 0.0017-0.0021 and the real TVD at
0.0064-0.0078 — **3.5-4.2x** — the same conclusion at a larger ratio on
rows further from the control. So a real, small systematic component
survives the fix on every row; §4f identifies it as `kSpecQuadN=16`'s
own C_D quadrature residual, quantifies it at the worst (lowest-
roughness, most-grazing) configuration, and records the decision to
keep `kSpecQuadN=16` and widen the gate for that configuration rather
than pay `kSpecQuadN=32`'s ~4x per-call cost.

### 4d. Cost

`Pdf()` alone, 200 000 calls, one shading point:

| | pre-fix | fixed (kSpecQuadN=16) |
|---|---|---|
| single specular lane | 17.0 ns | 690 ns |
| per-channel (3 lanes) | 17.0 ns | 2003 ns |

**P3-6 correction (round-2 review): the table below was wall-clock;
replaced with USER-CPU** (wall-clock on a loaded machine mixes in
scheduling noise a single-process benchmark should not report). Whole-
render user-CPU time, PT 64 spp at 512x512, `oidn_denoise false` and
`pixel_filter box`, as measured by the round-2 review:

| scene | pre-fix | fixed | delta |
|---|---|---|---|
| showroom | 18.63 s | 18.53 s | -0.5% (noise) |
| Cornell-box material grid (1 of 15 objects schlick) | 99.53 s | 99.61 s | +0.1% (noise) |
| BDPT rasterizer, same material grid | 105.9 s | 108.9 s | +2.8% (noisy) |
| same scene, EVERY surface schlick | 126.3 +/- 2.8 s | 162.5 +/- 0.9 s | +28.6% |

So the realistic cost on scenes with a handful of `schlick_material`
objects is inside run-to-run noise; the worst-case (every surface
schlick) cost is real at ~28.6%. This session independently confirmed
the Pdf()-alone cost is real and roughly quadratic in `kSpecQuadN`
(§4f) but did not re-run the four scene renders above; they are quoted
from the round-2 review as-measured. `kSpecQuadN` trades cost back if
ever needed (8 gives ~200 ns at ~2% `C_D` error, which does not hold the
1% normalisation gate) — §4f records why 16 was kept rather than raised
to 32.

### 4f. The `kSpecQuadN` decision (round-2 review, P2-2)

P2-2 asked whether raising `kSpecQuadN` from 16 to 32 is justified by
its cost, given §4c's finding that the post-fix TVD residual has a real
systematic component (not pure bin noise) attributable to this constant.
Measured this session, independently of §4a's original 400-config sweep:

**Cost.** A microbenchmark (200 000 `Pdf()` calls, one shading point,
same method as §4d's table, this machine) at `kSpecQuadN=16` vs `32`:

| | kSpecQuadN=16 | kSpecQuadN=32 | ratio |
|---|---|---|---|
| single specular lane | ~1.3-1.5 us | ~4.0-5.3 us | ~3.3-3.5x |
| per-channel (3 lanes) | ~6.0-7.3 us | ~14.9-16.5 us | ~2.2-2.5x |

(These absolute numbers run higher than §4d's 690 ns/2003 ns and the
round-2 review's own 804 ns/2243 ns — all three were measured on
different machines/loads; the ~3.3-4x scaling from doubling
`kSpecQuadN`, consistent with the `O(N^2)` replay cost, is the number
that matters for this decision and reproduces across all three.)

**Accuracy.** The worst gate row (`r.02 rd.6 rs1 i.3 th85`, P2-4) was
re-measured at `kSpecQuadN=32` with the SAME hemisphere-quadrature
convergence method as §4a's note: the converged (extrapolated)
integral moves from ~1.010-1.011 at 16 to ~1.003-1.004 at 32 — roughly a
3x reduction in the systematic bias, consistent with §4a's 400-config
sweep (mean |C_D error| 0.0034 -> 0.0014, ~2.4x; max 0.022 -> 0.0083).

**Decision: kept at 16.** Quadrupling the per-call cost to buy a ~3x
reduction in a residual that (a) the existing 1% mass gate and 0.012 TVD
gate already tolerate for every ORIGINAL review configuration, and (b)
is NOT driven to zero by the higher setting either (32x32's own
worst-case error in the 400-config sweep is still 0.0083, i.e. the
residual shrinks, it does not close) is not justified by the measured
whole-render cost in §4d — the every-surface-schlick worst case is
already +28.6% at 16; quadrupling `Pdf()`'s own cost would put a
material this visible in a scene at a materially worse tax for a
residual that stays open regardless. Instead, the two lowest-roughness
P2-4 rows get an honestly widened mass tolerance (`kMassTolLowRough`,
1.5%) rather than a tightened `kSpecQuadN`. This can be revisited if a
future scene makes the every-surface-schlick cost class common rather
than a synthetic worst case.

### 4e. Rendered before/after

`scenes/Tests/BDPT/cornellbox_bdpt_materials_pt.RISEscene` (one
`schlick_material` sphere, `rd` grey / `rs` white, roughness 0.05,
isotropy 0.3), PT 64 spp, `oidn_denoise false`, `pixel_filter box`, EXR
`Rec709RGB_Linear`. RISE seeds renders from an unsynchronized libc
`rand()`, so each run is an independent estimate; 4 runs each:

| | frame mean luminance |
|---|---|
| pre-fix (`4325f365`) | 0.995781 +/- 0.000318 |
| fixed | 0.997451 +/- 0.000065 |

**+0.168%**, about 5x the run-to-run spread. Only PT's NEE MIS weight
changes — no sampling changed — so a small frame-mean move on a scene
where one object of fifteen is `schlick_material` is the expected shape
of the result.

**P3-6 addition (round-2 review): a per-block visible-effect
measurement, not just a frame mean.** The frame mean above is dominated
by the 14 non-Schlick objects; the round-2 review instead compared a
256-region (16x16) block decomposition of the schlick sphere's own
render region between pre-fix and fixed. The schlick sphere's own
highlight block (block index (5,7)) moves 1.44691 -> 1.64856, **+13.9%**,
a t-statistic of approximately +100 given the run-to-run spread; 7 of
the 256 blocks show `|t| > 3`, and all 7 are clustered on the schlick
object's own screen region (not scattered across the frame, which would
suggest a measurement artifact rather than a real, localized effect).
This is quoted from the round-2 review's own block decomposition (not
independently re-run this session); it is consistent in direction and
order of magnitude with this section's own frame-mean result and with
§1's derivation, since the highlight region is exactly where the
diffuse/specular MIS-weight correction has the most leverage.

---

## 5. Consumers

`ISPF::Pdf()`/`PdfNM()` is called from the NEE partner MIS weight
(`LightSampler.cpp` via `IMaterial::Pdf`), BDPT's `pdfRev`
(`PathValueOps::EvalPdfAtVertex`), PT/BDPT's guided RIS candidate 1 and
guided-accepted branches (`PTEvalPdfAtSurface`), and VCM through the
same interface. None is edited here; each picks up the corrected
density through the shared interface. DL-67's own subject — candidate 0
and the three one-sample/RIS overwrite branches — is untouched and still
open.

**This does not mean the NEE↔escape MIS pair now partitions to one in
default (un-guided) PT — it does not.** `LightSampler.cpp`'s NEE arms
weight against `pMaterial->Pdf(...)` (`LightSampler.cpp:2545,2708` and
their `PdfNM` twins at `:3121,3244`) — the AGGREGATE density this slice
fixed. But with guiding inactive, `PathTracingIntegrator.cpp` PART 3's
escape-side partner is `misBsdfPdf = effectiveBsdfPdf`
(`PathTracingIntegrator.cpp:3836`), stored as `rs2.bsdfPdf =
effectiveBsdfPdf; rs2.bsdfMisPdf = misBsdfPdf;` (`:3867-3868`) — i.e.
`effectiveBsdfPdf = pS->isDelta ? 0 : pS->pdf` (`:3521`), the SELECTED
lobe's own per-lobe density, not the aggregate. At a multi-lobe SPF like
Schlick that is a different quantity from what NEE weighs against, so
`w_bsdf + w_nee != 1` there even after this slice's fix — this slice
narrows the gap (the aggregate this file corrected is closer to the
per-lobe density than the old, more wrongly-weighted aggregate was) but
does not close it. Measured (sample-weighted `w_bsdf+w_nee` across three
Schlick-material furnace configurations, un-guided default PT): 1.0102 /
1.0627 / 1.0667 at this slice's HEAD vs 1.0530 / 1.0735 / 1.0697 on
pre-slice master `32824325` — improved, not closed; worst single bin
1.353. No open row named this UN-guided instance (DL-67 is scoped to the
guided one-sample/RIS branches). Filed as **DL-103**; see the ledger for
the full recipe. Cross-references: DL-67 (guided PT/BDPT candidates),
DL-69 (BDPT/VCM ordinary throughput).

---

## 6. What the first version of this slice got wrong

Recorded because the failure mode is general, not specific to Schlick.

The first version reasoned correctly that the specular coefficient is
exactly computable and the diffuse coefficient is an expectation, then
reached for the nearest shipped closed-form average (`SchlickFresnelAvg`,
DL-64) to stand in for the expectation. It shipped with two gates: a
comparison against an inline replica of its own formula, and a per-call
lower bound `Pdf(w_S) >= q_S(w_S) p_S(w_S)`.

Neither gate could fail. The first checks that the code implements
itself. The second is an algebraic identity — `Pdf` adds a non-negative
`C_D p_D(w_S)` on top of exactly the term being bounded — so it holds
for **any** `C_D >= 0`, including the wrong one. Measured afterwards,
that version's aggregate integrated to 0.68-1.02 over the same
configurations, and its total variation against the real sampler was
**worse than the code it replaced on 6 of the 8 review configurations**
(0.0552 -> 0.0627, 0.0553 -> 0.0642, 0.0726 -> 0.0750, 0.0742 -> 0.0876,
0.0313 -> 0.0680, 0.0116 -> 0.0278 — up to **2.4x**), a wash on a
seventh (0.16153 -> 0.16164) and better on the eighth
(0.0938 -> 0.0125, the `rs=0.02` grazing row, where its two errors
happened to cancel).

Three lessons, in order of how much they cost:

1. **A density claim needs a normalisation measurement.** `int Pdf`
   against the sampler's own emission probability is one line of test
   and it is unfoolable. Neither of the original gates was a
   measurement of the sampler at all.
2. **A one-sided bound cannot gate a two-sided quantity.** If the
   quantity under test appears on both sides of the assertion, check
   whether the assertion is an identity before trusting a pass.
3. **When an expectation has no closed form, integrate the sampler.**
   A deterministic replay of the sampler's own inverse-CDF map is cheap
   to write, exact in structure (it inherits every accept-check and
   short-circuit for free), and cannot drift from the sampler as long
   as it shares the sampler's code — which is a stronger guarantee than
   any analytic approximation offers.

An earlier §7 of this document justified keeping
`SPFPdfConsistencyTest`'s Schlick row at a 15% integral tolerance with
`skipChi2=true`, on the theory that the deficit was inherent. It was
not: that row now passes at the standard 5% tolerance (1.00166 at 30
degrees, 0.999932 at 60) and its chi-squared test is gated (783.7 /
816.3 against critical 928.3). `skipCrossVal` stays true, and for a
reason that IS inherent — §1's `C_D` is a constant, and cross-val's
per-call lower bound compares against a denominator that varies per
call.

---

## 7. Sibling audit

One-sentence bug pattern: *`Pdf()` reports a density other than the one
`Scatter()` + `RandomlySelect()` actually generate from.*

| SPF | verdict |
|---|---|
| `SchlickSPF` | **fixed this slice** (lobe coefficients, azimuthal density, frame, per-channel branch parity) |
| `SchlickBRDF` | n/a — BRDF classes are eval-only, no `Pdf()` exists |
| `GGXSPF` / `CoatedSPF` | immune: every emitted lobe's `.pdf` is the SAME aggregate `mixPdf`, and selection is internal and single-lobe. Reconfirmed: `GGXSampleEvaluationConsistencyTest` 48/0 |
| `CookTorranceSPF` | immune by a different architecture: `ComputeLobeWeights` picks a lobe from `ri` alone BEFORE sampling, and `Pdf()` calls the same function with the same inputs. `SPFPdfConsistencyTest`'s CookTorrance rows run with `exactSelectedPdf=true`, 0 cross-val failures |
| `PolishedSPF` | already correct: `Pdf`/`PdfNM` compute the same incident-geometry Fresnel `Rs` the krays use and weight by it |
| `WardIsotropicGaussianSPF`, `WardAnisotropicEllipticalGaussianSPF` | immune, coincidentally: both krays are pure `GetColor(ri)`, so raw-albedo weighting IS the realized weight. (Their rejection-rate term is unmodelled in the same way Schlick's was, which is why their `SPFPdfConsistencyTest` rows still skip cross-val/chi2 — not investigated here) |
| `IsotropicPhongSPF` | **in-pattern, unfixed — DL-98** |
| `AshikminShirleyAnisotropicPhongSPF` | **in-pattern, unfixed — DL-99** |
| `TranslucentSPF` | not this pattern: disjoint-hemisphere lobes, no overlap to mis-weight (per DL-69) |
| `CompositeSPF` | out of scope: `Pdf()` is a documented hard-coded 50/50 placeholder, not an attempt to match `RandomlySelect` |
| `FabricSPF` / `WeaveSPF` | not this pattern: the real priced mixture per `CLOTH_FABRIC_DESIGN.md` §9.2; `SPFPdfConsistencyTest` runs them with `skipCrossVal=false`, 0 mismatches |

Rows opened by this slice: **DL-98** (`IsotropicPhongSPF`), **DL-99**
(`AshikminShirleyAnisotropicPhongSPF`), **DL-100** (`GenerateSpecularRay`
samples the unflipped frame, so a back-face hit loses its whole specular
lobe -- **CLOSED 2026-09-17**, debt-dl100 slice, also fixed the identical
pattern found by sibling audit in `WardIsotropicGaussianSPF`; see
[DL100_SCHLICK_BACKFACE_SPECULAR.md](DL100_SCHLICK_BACKFACE_SPECULAR.md)),
**DL-101** (the per-channel branch reuses one `ScatteredRay`
across its three lanes, so a lane whose `hdotk <= 0` can push the
PREVIOUS lane's direction with its own kray and pdf -- **CLOSED
2026-09-17**, debt-dl100 slice, same fix applied to both Ward files by
sibling audit; see
[DL101_PERCHANNEL_SCATTEREDRAY_REUSE.md](DL101_PERCHANNEL_SCATTEREDRAY_REUSE.md)),
**DL-102** (the
azimuthal-density defect of §4b — recorded as a closed row so the
mechanism is findable, since it is a different bug from this row's
subject).

---

## 8. Note on two stale numbers

Earlier versions of this document and of `tests/README.md` quoted a
per-lobe lower-bound failure count two different ways — `105/36946` and
`2845/32045` in one place, `64/37070` and `2857/32008` in another. The
totals differ (36946 vs 37070 specular draws from a fixed seed), so the
two were measured against different library states, not merely different
seeds. Neither figure is carried forward and neither should be quoted:
the check that produced them has been deleted, because post-fix it is an
identity (§6). The replacement evidence is §4c's table, which is a
measurement of the sampler rather than of the formula.
