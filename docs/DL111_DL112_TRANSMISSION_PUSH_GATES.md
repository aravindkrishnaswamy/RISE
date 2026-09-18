# DL-111 / DL-112 — transmission push gates, and the disposal of a geometrically impossible delta lobe

**Status: both CLOSED 2026-09-17** (slice `debt-pushgates`).
Regression: `tests/TransmissionPushGateTest.cpp` — 354 checks, 0 failures
(**82 red** against the slice's parent commit `a4495f94`).

Companion reading:
[DL68_TRANSLUCENT_ENTRY_HORIZON.md](DL68_TRANSLUCENT_ENTRY_HORIZON.md) (the
same pattern in `TranslucentSPF`, and the azimuth-arc construction reused
here), [DL03_GUIDED_IOR_CONTINUATION.md](DL03_GUIDED_IOR_CONTINUATION.md)
(DL-45, the exit-pop twin and the Malley remap reused for DL-112),
[DL70_GEOM_NORMAL_ORIENTATION_SITES.md](DL70_GEOM_NORMAL_ORIENTATION_SITES.md)
(`UnflippedGeomNormal()` / `HasTrueGeomSide()`).

---

## 1. The invariant

Every sub-test in the regression asserts one line:

> a scattered ray whose `ior_stack` membership differs from the caller's
> must satisfy `Dot(dir, geomN) < 0`,

with `geomN` the **ray-anchored** true geometric normal — the geometric
normal flipped to oppose `ri.ray.Dir()`, recovered through
`HasTrueGeomSide()` / `UnflippedGeomNormal()`.

That single form covers **both** crossings. On an entry `geomN` is the
outward normal and the transmission must go in; on an exit `geomN` is the
inward normal and the transmission must go out. It is the exact complement
of the reflection lobes' pre-existing `Dot(dir, geomN) > 0` gate, so the two
lobes of a dielectric interface partition the sphere.

Three SPFs violated it.

| site | what it claimed | what gated it |
|---|---|---|
| `DielectricSPF::GenerateScatteredRay` (RGB + NM, entry push and exit pop) | the continuation crossed | `Dot(dir, ri.onb.w())` — the **shading** normal |
| `PerfectRefractorSPF::DoSingleRGBComponent` + `::ScatterNM` (entry push and exit pop) | the continuation crossed | **nothing at all** |
| `SubSurfaceScatteringSPF::Scatter` + `::ScatterNM` (back-face `exitRay`) | the continuation left the medium | **nothing at all** |

In each file the **companion Fresnel/reflection lobe in the same function
was gated against a geometric normal**, which is what makes the omission
legible: the author knew the shading normal was not the surface, and
applied that knowledge to one lobe of the pair.

A mis-set membership is the DL-03 / DL-45 / DL-68 failure mode: a ray that
leaves the way it came in with the object pushed makes the *next* hit on
that object read as an exit that was never legitimately entered, and the
pop machinery then runs on a lie.

---

## 2. Reachability — a small theorem, and the premise it hides

Work in the plane, angles measured from the true outward geometric normal,
positive toward `+x`. Write `phi` for the shading-normal tilt and `delta`
for the ray's own angle off the geometric normal (the arrival direction on
entry, the departure direction on exit). The incidence angle in the
**shading** frame is `|delta - phi|`, and the refracted direction sits at
`thetaT` from the shading normal on the same side as the ray, so the
emitted direction's angle off the geometric normal is

```
phi + sign(delta - phi) * thetaT
```

and the crossing test is exactly `|phi + sign(delta-phi)*thetaT| < 90 deg`.

**Refraction into a DENSER medium cannot fail it — *while the shading
normal still opposes the incoming ray*.** There `thetaT < |delta-phi|`, so
the emitted direction is angularly *between* the incoming ray and the
shading normal's far side, both of which are already inside the crossing
half-space, which is convex.

Under that premise the reachable set needs `thetaT > |delta-phi|`, i.e.
refraction into a **rarer** medium:

* an ordinary glass → air **exit**, or
* an **entry into a bubble** (an `ior 1.0` object inside a glass block —
  `ior_stack.top()` is then the denser medium);

and it additionally needs `phi` and `thetaT` to **add**, i.e. the ray tilted
the same way as the shading normal and further out — the grazing silhouette
of a normal-mapped or glint-modified refractive object. `MakeObliqueHit`
with a **positive** tilt builds exactly that (sub-test 2b).

### 2a. The premise fails, and a denser-medium ENTRY does land wrong-side

*(Review P2-1, 2026-09-17. An earlier revision of this section, of the
`Reachability` comments in `DielectricSPF` / `PerfectRefractorSPF`, of
`MakeObliqueHit`'s derivation, of `tests/README.md` and of ledger row
DL-111's correction (a), all asserted the theorem without its premise —
"entering glass from air is therefore safe at every tilt". That is
**false**, and the claim also scoped a coverage hole: sub-test 2b sweeps
only POSITIVE tilts.)*

The derivation above silently assumes `Dot(d, n_s) < 0` — that the tilted
shading normal still opposes the incoming ray, so the ray itself lies inside
the half-space the transmitted direction is tested against.
`Optics::CalculateRefractedRay` **flips the normal internally** to restore
that sign convention (its own comment says so). So once a bump / normal map
or `GlintModifier` carries `n_s` *past* a grazing incoming ray —
`Dot(d, n_s) > 0`, the silhouette of any normal-perturbed refractive object,
and `ReliefModifier` is explicitly **not** a horizon clamp — the refraction
is built about `-n_s`, whose far side is **the side the ray came from**, and
the theorem's conclusion inverts.

Closed form, `geomN = +Z`, an air → glass `ior 1.5` **entry** arriving 89°
off the geometric normal with the shading normal tilted 30° the *other* way:

```
t = (-0.911, 0, +0.412)      # above the surface, on an ENTRY into the denser medium
```

All nine `(delta, tilt) ∈ {80,85,89} × {-30,-45,-60}` cells are wrong-side.
Measured against `a4495f94` (sub-test 2c, 64 trials/cell):

| SPF | wrong-side transmissions | total emitted energy |
|---|---|---|
| `DielectricSPF` | (lobe dropped outright, so 0 transitioned) | **0.0000** at 9/9 cells, expected 1 |
| `PerfectRefractorSPF` | **64/64** at 9/9 cells | 0.8290 … 0.9582, expected 1 |

`DielectricSPF` emitted **nothing at all** in those cells: the companion
Fresnel reflection is wrong-side too, so both lobes were dropped. **Ordinary
glass ENTRY at a bump-mapped silhouette was 100 % broken pre-fix** — a
materially larger pre-fix severity than this section originally claimed. All
27 checks are green after the fix.

**`DielectricSPF`'s `scattering` warp escapes the theorem entirely.** The
warp (Henyey-Greenstein when `hg`, Phong `cos^N` otherwise) is not a delta:
it perturbs the Snell direction by `alpha`, so a wide warp reaches the wrong
side from *any* geometry, at any tilt, in either index direction. At
`scattering 0` — which CLAUDE.md records as "maximally DIFFUSE
transmission, not off" — `alpha = acos(u)` is a **uniform hemisphere** about
the Snell direction, and roughly half of it points back out the side the ray
came from.

---

## 3. The disposal ruling, derived

A delta Snell lobe has **no distribution to renormalize**, so DL-68's clip
does not apply to it. Four candidate treatments:

**(a) Mirror the Snell result across the geometric plane.** Energy is
preserved and the direction crosses by construction. Rejected: `t'` obeys
Snell's law about *no* normal, so it is not a refraction at all. For a
dispersive material this is fatal — the three channels' directions differ
only through the index via Snell, and mirroring destroys the ordering of the
dispersion fan in a way that is not a function of `eta`.

**(c) Re-label it as a reflection (TIR-like).** Energy is preserved in
total, but **moved between channels**: transmitted flux is re-emitted as
reflected flux at a rate that has nothing to do with Fresnel, which is the
one thing a dielectric interface is for. At a 60-degree glint tilt a clear
glass sphere would return a large fraction of its transmission as
reflection, reading as a silvered patch. It also collides with the lobe
already emitted: the same hit emits `reflect(d, n)` with weight `ref`, so
this adds a **second, nearly parallel delta reflection** with weight
`1-ref` — two delta reflections at one vertex, which any MIS or merge
consumer has to make sense of.

**(b) Use the geometric normal for the side test and the shading normal
only for the direction, then discard wrong-side samples with the energy
accounted as loss** (the PBRT / Mitsuba framing). The **side-test half is
right and mandatory**, and is adopted. The **discard half is the wrong
disposal**: it is deterministic energy loss growing with tilt, with nothing
to renormalize it into — which is precisely the defect DL-112 exists to fix
on a different lobe. Adopting it here would be filing the same debt on
purpose.

**(d) RE-DERIVE the refraction about the true geometric normal** — chosen.
`t' = refract(d, geomN, eta)`; if *that* total-internally-reflects, `ref`
becomes 1 and the reflection carries everything (the branch the file already
had).

* **Energy**: preserved exactly. The `(1-ref)` weight still rides a
  transmission.
* **Physics**: `t'` obeys Snell's law exactly, at the true interface, with
  the same `eta` — so a dispersion fan stays a fan, ordered by index, each
  channel still deviating by `asin(sin(theta_i)/eta)`.
* **Correctness of the claim**: guaranteed, with **no re-check needed**. A
  refraction about `N` always lands on the far side of `N`, so
  `Dot(t', geomN) < 0` holds unconditionally and the push is truthful.
* **Precedent**: it is *exactly* the treatment all three of these functions
  already applied to their own mandatory (TIR) reflections, and the coarse
  form of the "clamp the shading normal so the lobe stays valid" rule
  production renderers apply (Cycles' `ensure_valid_reflection`).
* **Cost**: one extra `CalculateRefractedRay` on the rare wrong-side branch.

### Reciprocity, stated honestly

**None of the four is strictly reciprocal**, and this should be said plainly
rather than claimed for the winner. For a pure refraction the pair
`(d, t)` generated from side A also arises from side B, because Snell is
reversible: `refract(-t, N, 1/eta) = -d`. But every candidate here is
selected by a **predicate evaluated on the forward direction alone**
(`does the shading normal's answer cross?`), and that predicate is not
symmetric under path reversal — the reverse walk arriving along `-t'`
asks its own, different question. So a path can exist from one end and not
the other under *any* of (a)-(d). This is the well-known
shading-normal/geometric-normal inconsistency, not something a disposal
choice can remove; it is why the choice is made on **energy** and on
**consistency with the file's own existing treatment** instead.

What (d) *does* buy over the others on this axis: its emitted map is the
only one that is exactly invertible with the same index — if the reverse
walk selects this branch at all, it lands exactly back on `-d`. (a) and (c)
have no such inverse.

### Continuity

(d) is discontinuous in **direction** at the switching boundary (the Snell
answer is exactly tangent there, while `refract(d, geomN)` sits in the
interior). That is the price. The alternatives are worse in kind: (b) is
discontinuous in **magnitude** (full → zero, a black patch) and (c) in
**hemisphere** (far side → near side, a silver patch).

### Where a lobe *does* exist: the warp

`DielectricSPF`'s `scattering` / HG warp is the one non-delta part, and
DL-68's construction applies to it **verbatim**. Whether `alpha` came from
the Henyey-Greenstein inverse CDF or the Phong `cos^N` one, the warp is
**uniform in azimuth about the Snell axis at fixed `alpha`**, and the
crossing constraint is a plane through the origin — so the clipped
conditional is still uniform in azimuth, and drawing it on the valid **arc**

```
halfArc(alpha) = PI                                  if cot(alpha)cot(phi) >= 1
               = acos( -cot(alpha)cot(phi) )         otherwise
```

is exact, costs the **same single canonical number**, and renormalizes the
clipped-away energy into the valid region instead of dropping it. That is
`GeometricUtilities::PerturbClipped` — the same construction as DL-68's
`TranslucentSPFDetail::SampleClippedPhong`, written as a second
implementation that takes the polar ANGLE from its caller instead of
drawing it from a `cos^N` inverse CDF. The two are deliberately **not**
folded into one: `SampleClippedPhong` has `cos(theta)` in hand and uses it
directly, so delegating would insert an `acos`/`cos` round trip into a path
whose untilted branch is pinned bit-for-bit by
`TranslucentSpectralParityTest`. **Any change to the arc math belongs in
both**, and both carry a comment saying so. The axis must be inside the
half-space first — which the re-derivation above guarantees — and both
functions fail loudly if it is not.

---

## 4. The geometric reference, and an `nEff`-anchored rule that was not equivalent

`DielectricSPF` and `PerfectRefractorSPF` both oriented their geometric
normal by the **IOR stack** rather than by the ray:

```
nEff   = bFromInside ? -ri.onb.w() : ri.onb.w();
geomN  = ( Dot(geomNRaw, nEff) >= 0 ) ? geomNRaw : -geomNRaw;
```

with a comment in each file arguing this was "provably equivalent to the
ray-anchor rule used elsewhere", on the grounds that `bFromInside` is ground
truth from the stack and cannot be moved by a glint tilt.

**It is not equivalent on a DOUBLE-SIDED triangle mesh.** There the geometry
flips BOTH `vNormal` and `vGeomNormal` to face the incoming ray
(`bGeomNormalOrientedToRay`), so at a genuine EXIT hit `ri.onb.w()` points
INWARD and `nEff = -ri.onb.w()` lands **outward** — the opposite of the
ray-opposing side the gate needs. The gate then inverts. Measured on a
double-sided dielectric slab's inside face
(`TransmissionPushGateTest` sub-test 3, 2048 trials per cell):

| tilt | pre-fix reflection lobe emitted | pre-fix wrong-side |
|---|---|---|
| 0, 15, 30 | **0 / 2048** | — |
| 60, 75, 89 | 2048 / 2048 | **2048 / 2048** |

That is: **100 % of the internal Fresnel reflection of double-sided glass
was dropped at zero tilt** — no bump map needed — and past 45 degrees of
tilt it was emitted pointing OUT of the solid while still carrying the
interior IOR stack.

Both files now ray-anchor:

```
geomN = ( Dot(geomNRaw, ri.ray.Dir()) < 0 ) ? geomNRaw : -geomNRaw;
```

which is correct on both crossings with no `bFromInside` dependence at all,
and is the convention `a4fb884d` established for `GGXSPF` after the same
class of inversion (`GlintModifierTest` Test 8d). `HasTrueGeomSide()`
(DL-70) excludes `HairGeometry`, whose geometric normal is fabricated from
the ray and answers no question about sides; there, as for a degenerate
normal, the fallback is the shading normal and both gates become no-ops.

---

## 5. The reflection half: drop → re-derive

Applying the ruling only to transmissions would have been inconsistent, so
the **same disposal** is now applied to the delta reflections in the same
three files: a wrong-side reflection is re-derived about the geometric
normal **always**, not only when it is mandatory (`ref >= 1`). The drop paid
`ref` to nothing — deterministic energy loss growing with tilt — and on a
shipped SSS material (`bAbsorbBackFace = true`) the front-face delta
reflection is the **only** ray the SPF emits, so the drop was total loss of
its specular response at a tilted shading normal.

`SubSurfaceScatteringSPF`'s **rough (VNDF)** reflection keeps its drop, and
should: that one is a real lobe whose `Pdf()` carries the matching gate, so
the drop is the microfacet shadowing term, not an unmatched loss.

---

## 6. DL-112 — the translucent entry front-reflection lobe

DL-68's sibling audit correctly **refuted** this lobe for the push-gate
pattern: its geometric gate is present and it makes no stack claim, so
nothing is ever misclassified. The two residuals it filed instead are what
is closed here.

**(a) Energy.** The gate DROPPED a below-horizon sample rather than
resampling, so the lobe integrated to `P(valid) = (1+cos(phi))/2 < 1` under
tilt. Measured emitted energy against a `0.3` reflectance painter, 32768
trials per cell (`front.kray` *is* the throughput multiplier, so the mean of
`kray` over trials — counting a non-emitted lobe as zero — is the estimator
of the lobe's total reflected energy):

| tilt | 0 | 15 | 30 | 45 | 60 | 75 | 89 |
|---|---|---|---|---|---|---|---|
| RGB | 0.30000 | 0.29465 | 0.27995 | 0.25594 | 0.22501 | 0.18802 | 0.15265 |
| NM  | 0.30000 | 0.29456 | 0.27986 | 0.25586 | 0.22494 | 0.18796 | 0.15260 |

— exactly `0.3 * (1+cos(phi))/2`. Independently, a 300x600 spherical
quadrature of `Pdf()`'s front branch read `1.00000 / 0.93302 / 0.85356 /
0.75001 / 0.62941 / 0.50873` at 0 / 30 / 45 / 60 / 75 / 89 degrees. Both
read their target (0.3, and 1.0) at every tilt after the fix.

**(b) Axis.** Unlike DL-45's exit lobe and DL-68's two Phong lobes, the
lobe's axis was the raw `n`, never oriented into `geomN`'s hemisphere, so
`P(valid)` was not bounded below by 0.5 and a past-90-degree disagreement
could collapse the lobe instead of clipping it — the trap DL-45 review round
3(a) hit on a double-sided mesh.

Both close with the tool DL-45 already built: orient the axis
(`OrientedLobeAxis`), then draw the exact 2-draw remap onto the
geometrically valid region (`SampleValidDiffuseExit` — nothing in its
construction was specific to the exit lobe; it samples a plain cosine lobe
about any axis clipped to any half-space through the origin), and report the
matching NORMALIZED density from `Pdf()`'s front branch. RGB and NM. The
draw count is unchanged at 2, so `ISampler::HasFixedDimensionBudget()` is
untouched, and the change is an **identity at zero tilt**.

### The MIS movement the ledger row required to be measured

`Pdf()`'s front-face return is an MIS-partner density for every translucent
surface. Its value rises by exactly `1/P(valid)` — bounded by 2x, and only
under tilt. `PTGuidingMISPartitionTest` passes (66 checks), but **its DL-74
premise probe for `TranslucentSPF::Pdf` had to be retargeted, and the reason
is worth recording**: the entry and exit branches are now the *same
normalized clipped-cosine function*, so at a genuine entry-shaped hit on a
geometry that does not flip its normals they coincide exactly and no
value-difference probe exists there. The stack still selects between two
genuinely different functions — the entry lobe clips to the ray-anchored
`geomN`, the exit lobe to the unflipped `geomNRaw` — and on an **exit-shaped**
hit (an interior ray travelling outward, exactly when `containsCurrent()` is
true in production) those references are opposite and the two branches'
supports are **disjoint**. The premise therefore now reads in its strongest
form: the `IORStack(1.0)` sentinel returns **0** where the live stack is
**0.218306**.

What is deliberately **not** fixed: `Pdf()` still does not cover the Phong
`trans` lobes at all. That is **DL-41**, unchanged in kind and scope by this
row.

---

## 7. Red-proof (against `a4495f94`)

`tests/TransmissionPushGateTest.cpp`: **82 failures / 354 checks**. The
census is the fraction of **transitioning** rays on the wrong side.

`DielectricSPF`, entering, closed analytic fixture, 4096 trials per cell —
the `scattering`-warp rows (the delta row is green, per §2's theorem):

| pipe | tilt 15 | 30 | 45 | 60 | 75 | 89 |
|---|---|---|---|---|---|---|
| RGB `scattering 1` | 9 | 38 | 92 | 193 | 369 | **663 / 3572** |
| RGB `scattering 0` | 119 | 249 | 384 | 558 | 799 | **1065 / 3123** |
| RGB dispersive `scattering 1` (x3 channels) | 32 | 129 | 300 | 600 | 1155 | **2030 / 10759** |
| RGB HG `g 0.8` | 8 | 17 | 37 | 62 | 98 | **178 / 3938** |
| NM `scattering 1` | 9 | 38 | 92 | 193 | 369 | **663 / 3572** |

Smooth-shaded mesh, `scattering 1`, no bump map — just an interpolated
normal: **22 / 3989** at 2 degrees of deviation, 80 / 4027 at 8, 190 / 4076
at 20.

The **delta** rows, at the silhouette configuration §2 derives (256 trials
per cell, `scattering 1e6`); `-` is TIR (no transmission emitted) and a
blank is a cell that is green under the (positive-tilt) sweep:

| row | (70,30) | (80,45) | (80,60) | (85,45) | (85,60) |
|---|---|---|---|---|---|
| Dielectric glass→air exit | 256/256 | 256/256 | 256/256 | 256/256 | 256/256 |
| Dielectric bubble entry | | 256/256 | 256/256 | — | 256/256 |
| PerfectRefractor glass→air exit | 256/256 | 256/256 | 256/256 | 256/256 | 256/256 |
| PerfectRefractor bubble entry | | 256/256 | 256/256 | — | 256/256 |
| SSS exit (`ior 1.4`) | 256/256 | 256/256 | | 256/256 | 256/256 |

The **denser-medium entry** rows the theorem wrongly excluded (§2a,
sub-test 2c, 64 trials per cell, air → glass `ior 1.5`, `scattering 1e6`,
9 cells `{80,85,89} × {-30,-45,-60}`):

| row | wrong-side transmissions | total emitted energy (expected 1) |
|---|---|---|
| Dielectric air→glass entry | lobe dropped outright (0 transitioned) | **0.0000** at all 9 |
| PerfectRefractor air→glass entry | **64/64** at all 9 | 0.8290 … 0.9582 |

Double-sided dielectric sheet: §4's table.
DL-112's two tables: §6.

Review follow-ups (P1-1, P1-2, P2-1) added sub-tests 2c and 12 and Part F of
`SPFBSDFConsistencyTest`; the suite is **414 checks / 0 failures** at the
head of this slice.

---

## 8. Sibling audit (`docs/skills/audit-by-bug-pattern.md`)

**Pattern, in one sentence:** *a scattered ray that transitions IOR-stack
membership is built from the shading normal without confirming the direction
is on the side the membership change claims.*

Surface swept: every `push` / `pop` / `remove` on an `IORStack` under
`src/Library/Materials/`, plus every file that names `ior_stack` there.

| site | same pattern? | evidence | action |
|---|---|---|---|
| `DielectricSPF.cpp` `:213` pop / `:241` push (RGB + NM) | **YES** | gated on `ri.onb.w()`; warp widens the wrong-side set | **fixed** |
| `PerfectRefractorSPF.cpp` `:118`/`:323` push, `:148`/`:353` pop | **YES** | no gate on the transmitted direction at all | **fixed** |
| `SubSurfaceScatteringSPF.cpp` `:257`/`:520` `exitStack.pop()` → `exitRay` | **YES** | no gate; companion back-reflection IS gated | **fixed** |
| the three files' delta **reflections** | not the push pattern, **same disposal defect** | dropped instead of re-derived unless mandatory; `ref` paid to nothing | **fixed** (§5) |
| `TranslucentSPF.cpp` `:595`/`:663`/`:871` entry push | already closed | DL-68 `SampleClippedPhong` against `-geomN`; re-verified green | n/a |
| `TranslucentSPF.cpp` `:783`/`:946` exit pop | already closed | DL-45 `SampleValidDiffuseExit` against `geomNRaw`; re-verified green | n/a |
| `TranslucentSPF` entry front-reflection lobe | **NO** — gate present, no stack claim | the residual is energy + axis | **fixed as DL-112** (§6) |
| `SubSurfaceScatteringSPF` **rough (VNDF)** reflection | **NO** | the drop is the microfacet shadowing term and `Pdf()` carries the matching gate | refuted; left as-is |
| `CompositeSPF` | **NO** | makes no membership claim of its own — it forwards whatever a sub-SPF produced (`GapStackBelowTop`) and inherits its sub-SPFs' correctness | refuted (inherits) |
| `CompositeSPF`'s `Dot(dir, ri.onb.w())` **layer routing** | **NO**, with a caveat | it is a side test on the shading normal, but the question it answers ("which layer does this ray go to next") is a shading-space abstraction — a layered BSDF's "top" is defined by the shading frame, not the geometry | refuted, noted |
| `PolishedSPF` | **NO** | reads `ior_stack.top()` for Fresnel only; never transitions (matches DL-68's own audit) | refuted |
| `RandomWalkSSS` (`src/Library/Utilities/`) | **NO** | no `IORStack` reference anywhere in the file | refuted |
| `GenericHumanTissueSPF` | **NO** | reads `containsCurrent()` to pick an attenuation branch; never transitions | refuted — but see **DL-131** |
| `FabricSPF` / `WeaveSPF` / `CoatedSPF` | **NO** | forward `ior_stack` to a base SPF / use it for `Pdf`; the thin-transmission lobes make no membership claim | refuted |
| `HairBSDF` | **NO** | the parameter is `/*ior_stack*/` — unused | refuted |
| `BioSpecSkinSPF` | **NO** | no `push`/`pop` | refuted |

One hop further (downstream consumers of the fixed lobes): the emitted
`ScatteredRay::pdf` for these lobes is 1 (they are flagged `isDelta`), and
`kray` is an opaque throughput multiplier, so no consumer needed a change.
`RISE::RadianceEtaScale` reads the *stack*, which is exactly the quantity
this row made truthful.

---

## 9. New debts opened

* **DL-130** — `TranslucentSPF`'s two Phong `trans` lobes renormalize into
  their valid region (DL-68) while the `kray` they carry is unchanged, so
  the **selection weight** of the clipped lobe is not renormalized alongside
  the direction. Bounded by the same `pi/halfArc <= 2` factor DL-68
  documents, and entangled with DL-41's open mixture-density scope.
* **DL-131** — `GenericHumanTissueSPF::Scatter` / `::ScatterNM` compute a
  scattered direction in the interior branch and then **unconditionally
  overwrite it** with `ri.ray.Dir()` on the next line: the
  `scattering`/`g` parameters are dead in the inside branch of both paths.
  An unrelated defect found during this row's audit.

---

## 10. Gate

Clean library rebuild, zero warnings. Per-test builds, all green.

| suite | result |
|---|---|
| `TransmissionPushGateTest` (new) | 354 checks / 0 failures (red: 82) |
| `TranslucentEntryHorizonTest` | 251 checks / 0 failures |
| `TranslucentTiltedExitTest` | ALL TESTS PASSED |
| `TranslucentDoubleSidedTest` | 44 passed / 0 failed |
| `TranslucentInitialContainmentTest` | 43 passed / 0 failed |
| `TranslucentIORStackTest` | ALL TESTS PASSED |
| `TranslucentSpectralParityTest` | 1846 checks / 0 failures |
| `TranslucentPhotonEnergyTest` | 14 checks / 0 failures |
| `TranslucentSamplerDimensionCountTest` | 65589 checks / 0 failures |
| `DielectricGrazingFresnelTest` | 338 checks / 0 failures |
| `DielectricARTest` | 33 passed / 0 failed |
| `GrazingSnellFresnelTest` | 38 checks / 0 failures |
| `GrazingFresnelThroughputTest` | 44 checks / 0 failures |
| `MatchedIndexGrazingConsumerTest` | 60 checks / 0 failures |
| `GlintModifierTest` | ALL TESTS PASSED |
| `RefractiveRadianceScalingTest` | 38 passed / 0 failed |
| `SSSRadianceScalingTest` | 574017 guards passed / 0 failed |
| `SubSurfaceExitIORTest` | 301 checks / 0 failures |
| `SubsurfaceScatteringSpectralTest` | 5 passed / 0 failed |
| `RandomWalkSSSTest` | all passed |
| `BSSRDFNormalizationTest` / `BSSRDFProjectionNormalTest` / `BSSRDFEntryPointTest` | all passed |
| `BSSRDFPlanarProbeReachTest` | 0 failures |
| `HairSSSEntryNormalTest` | 0 failures |
| `IORStackSeedingRegressionTest` | 15 passed / 0 failed |
| `IORStackBehaviorTest` | all passed |
| `CompositeExtinctionTest` | all checks passed |
| `SPFPdfConsistencyTest` | all passed |
| `SPFBSDFConsistencyTest` | all passed |
| `LayeredWhiteFurnaceTest` | 0 of 57 configurations failed (row 3 re-locked, §11) |
| `PTGuidingMISPartitionTest` | 66 passed / 0 failed (was 63 checks; +3, §6) |
| `EnvLightBalanceTest` | 116 passed / 0 failed |
| `ProximitySignalTest` | 491 passed / 0 failed |
| `CurvePainterRGBDispersionTest` | 15 passed / 0 failed |
| `SobolSelectionChannelBiasTest` | 19 passed / 0 failed |
| `JHWhiteGuardSpectralTest` | 30 passed / 0 failed |
| `PiecewiseLinearScalarPainterRGBTintTest` | 18 passed / 0 failed |
| `TessellatedShapeDerivativesTest` | PASS (0 shapes failed) |
| `CstDeriveGoldenTest` | 452 MATCH / 0 DRIFT |
| `SourceHygieneTest` | 165 passed / 0 failed (347 test files scanned) |

---

## 11. The two gate rows that legitimately moved

**`LayeredWhiteFurnaceTest` configuration 3 (Dielectric / Lambertian)**
moved from `{0.3339, 0.3044, 0.3128, 0.5324}` to
`{0.4271, 0.4284, 0.4569, 0.6367}` — bit-identical across runs, still
strictly below 1, closer to it at every angle. The cause is **not** in
`CompositeSPF` (which this row has historically tracked): the top layer is
`DielectricSPF(tau=1, ior=1.5, scattering=0)`, and `scattering = 0` is the
widest possible warp, so roughly **half** of that uniform hemisphere pointed
back out the side the ray came from and the old shading-normal gate deleted
every one of those samples. Half the transmitted energy of a
low-`scattering` dielectric was being lost, at any tilt, on a flat surface.
The row was re-locked at the new values; the remaining deficit is the finite
recursion-budget truncation its own note describes.

**`PTGuidingMISPartitionTest`'s DL-74 premise probe** — see §6.

---

## 12. Render-level effect

See the commit that lands this document for the measured before/after on
`scenes/Tests/SMS/sms_veach_egg_bumpmap.RISEscene` (a relief-modifier-
perturbed dielectric Veach egg — the normal-mapped-glass configuration this
row is about), rendered against an **isolated build of the slice's parent
commit** with `oidn_denoise FALSE`, `pixel_filter box`, EXR
`Rec709RGB_Linear`, and repeats on both sides (renders are not deterministic
run to run — `BlockRasterizeSequence` shuffles from `std::random_device`).
