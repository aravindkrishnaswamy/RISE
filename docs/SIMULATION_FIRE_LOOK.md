# Fire look options (soot settings for the showcase fire)

Last updated 2026-11-27. Owner remark that started it: "The fire seems too blue". A methane flame with soot off has
only the blue chemistry light. Soot is already in the solver and renderer but is off by default. These are look
proofs of soot settings, made so a look can be chosen from pictures. No source, scene or test changed. Fire solver
version 4 (uncommitted working tree on `simulation-liquid`, `cc746ac42` + diff), binaries copied once at 20:30
(`rise` sha256 `1e33398d…`, `rise_sim` `81dc596f…`).

## Fire look options

**Short answer.** No soot setting keeps the bright 1.5–2.0 s flame through the window. **Every** variant, including
the one without soot radiation (B, G), drops at about 2.3 s fire time:

- peak temperature falls by 250–420 K;
- the volume above 1300 K falls from 8–11 L to 3–4.5 L;
- the visible flame falls from 0.6–1.1 m to about 0.4–0.5 m;
- light output falls by 8–16 times.

The soot-free `hearth` proof bake (solver version 3, `rendered/showcase/hearth/proof/bake_fire.log`; version 4 differs from it only by rounding, M8 §15.4) does the same (2073 K at 2.07 s, then 1769 / 1730 / 1670 K at 2.48 / 2.90 / 3.32 s).
So the 1.5–2.0 s look is the **start-up transient**, not the settled fire. Soot radiation only deepens the drop:
C settles about 150 K cooler than B. E's 5 s window shows the settled state holds steady from 2.5 to 5.0 s, with no
recovery and no strong puffing. The drop starts when the starting plume's smoke reaches the top of the domain
(carbon in the top 0.3 m rises by two to three orders of magnitude between 2.0 and 2.25 s in every variant). That the two are linked is
inferred, not shown.

Set-up: tier 6, Metal, `mode fast`, burner 0.30 m / 33 kW, fire window 0–3.0 s (B, C) or 0–5.0 s (E, G), frames every
1/16 s from 1.0 s. All flow options are left at their defaults, as in `hearth`. The r272/r273 runs used the r271
candidate flow package, but nothing in that evidence says soot needs it, and its zone layer is tier-specific. The
r273 doc puts this surrogate's radiant fraction between heptane and toluene values.

| Variant | Settings (`fire_simulation`) | Bake (tier 6) | Peak T, 1.0–2.2 s → 2.5 s–end (K) | Hot volume > 1300 K (L) | Visible flame (m), 1.5 s → end | Smoke | Light, settled vs C |
|---|---|---|---|---|---|---|---|
| A soot off | (none) | **not run** (dropped on coordinator request); soot-free reference = `hearth` proof log | 1957–2073 → 1670–1769 (step-log samples, version-3 proof) | — | — | none | — |
| B | `methane_soot_yield 0.03` | 30.3 min (two parts, below), 1851 steps to 3.0 s, 0 retries, 1.39 GB RSS | 2058 (max 2180) → 1735 (1671–1792) | 10.6 → 4.1 | 1.10 → 0.5 | thin grey wisp, barely visible | 1.4× |
| **C** | yield 0.1, `soot_radiative_fraction_cap 0.45`, `soot_self_absorption 1` | 30.9 min, 1851 steps to 3.0 s, 0 retries, 1.40 GB RSS | 1832 (max 1947) → 1588 (1558–1628) | 8.2 → 3.2 | 0.68 → 0.43 | **dark smoke column**, with G the most (3.6× B, 11× E above 1.6 m) | 1 |
| E | C + `soot_sheet_oxidation TRUE` | 54.7 min, 3067 steps to 5.0 s, 0 retries, 1.40 GB RSS | 1850 (max 1945) → 1612 (1559–1668) | 8.6 → 3.6–3.9 | 0.62 → 0.44 | almost none | 2.5× |
| G | yield 0.1, no soot radiation (r272 closure: radiative fraction stays 0.20) | 51.3 min, 3067 steps to 5.0 s, 0 retries, 1.40 GB RSS | 2042 (max 2170) → 1703 (1628–1755) | 10.1 → 4.0 (2.9–5.0) | 0.75 → 0.40 | dark smoke column, as much as C (equal carbon above 1.6 m) | **3.4–3.9×** (early 3–5×) |

How the columns were measured:

- *Peak T* and *hot volume* come from each frame's VDB (`temperature`), averaged over 1.0–2.2 s and over 2.5 s to the
  end; the step log samples every 256 steps agree.
- *Visible flame* is the highest image row in the burner column brighter than 1% of that frame's peak, converted to
  height at the burner plane. Smoke is excluded.
- *Light, settled* is the 99.9th-percentile pixel of the last frame (spectral) relative to C's.
- *Smoke* is carbon above 1.6 m in the last frame, and how it looks against the wall.

C's peak temperature over time, in K (frames): 1766 (1.0 s), 1792, 1832, 1845, 1826 (2.0 s), 1870, **1600 (2.5 s)**,
1584, 1588 (3.0 s). Carbon below 0.8 m doubles over the same step, so soot is building up near the flame. The flame
does **not** quench: it holds at 1560–1630 K with about 3 L above 1300 K. The radiant fraction **was not measured**:
`integrated_radiative_fraction` is reported only after the 10.5 s statistics start, and this path has no per-frame
radiation output.

**What each looks like.**

- **B, yield 0.03.** 1.0–2.0 s: a tall, thin, bright pencil, yellow-white at the base and orange above, about 1 m.
  It lights the card warm orange. Settled: a short orange flame, about 0.5 m, with a blue base (spectral) and a faint
  grey wisp of smoke. The most "methane-faithful" setting (the r272 fit).
- **C, yield 0.1, cap 0.45, self-absorption 1.** 1.0–2.0 s: a bushy yellow-orange flame, 0.6–0.7 m, with a detached
  tongue above. It lights the card and floor orange. Settled: a short red-orange stub, about 0.4 m, with a blue rim at
  the base and a dark smoke column rising out of frame. It is the dimmest variant as a light source. It looks the most
  dramatic because of the smoke.
- **E, C plus sheet oxidation.** Early it is the same as C, and at 2.0 s it is the brightest. Settled: an orange flame
  about 0.45 m with a violet-blue base and almost no smoke. As a light source it is 2–2.5 times C. It is steady from
  2.5 s to 5.0 s.
- **G, yield 0.1, no soot radiation.** The brightest at every time: 1.0–2.0 s is 3–5 times C's peak, a big
  yellow-white flame that lights the whole set warm. Settled (2.5–5.0 s, steady at 1630–1760 K): a yellow-orange flame about 0.4 m, more yellow and 3.5 times brighter than C, under a dark smoke column like C's. Under the RGB path tracer it looks almost the same.

**Spectral against RGB path tracer.** The flame colour is close. The RGB path has no chemistry light, so it loses the
blue base; with soot on it is no longer black. The RGB renders took 5–9 min against 2–3 min spectral, both at 64 spp.

**Recommendation.**

- **For the centrepiece, G** (`methane_soot_yield 0.1`, no other soot option). It is the brightest light source
  and the most yellow flame both early and settled, the same smoke as C, and steady from 2.5 s to 5 s. It changes the
  solver least: the soot is a passive glowing tracer and the flow is the same as B's.
- Its cost is honesty: its soot glows in the renderer but does not cool the gas in the solver.
- If that matters, take **E** (C plus `soot_sheet_oxidation TRUE`): energy-consistent soot radiation, 2.5 times
  C's settled light, steady, but almost smokeless.
- **C** as set is the dimmest settled flame. Its smoke column is the only thing it does best, and G matches that.
- Whichever is chosen, put the hero frames in fire time 1.0–2.2 s for the big flame the owner liked (first item
  below), or accept the settled 0.4 m flame.


**What would hold the 1.5–2.0 s look, since no soot setting does.**

1. Time the shot inside the bright phase. Fire time 1.0–2.2 s is scene t 0–1.2 s in `hearth`
   (`frame_origin_s 1.0`). The current hero frame 40 (fire time 3.5 s) is in the settled phase.
2. Accept the settled flame and expose it 2.5–3 EV brighter. The colour is the same orange; the flame is smaller and
   lights the set less.
3. Something with more heat release: a larger burner, a higher heat release rate or a continuous pilot. The scene
   path certifies only 0.30 m / 33 kW, so any of these is solver work and needs owner review.

**Final-quality cost of C, E or G, extrapolated** (the three cost the same here: G 51.3 min and E 54.7 min for 5 s). Tier 8, fast mode, window 0–4.5 s (the
`hearth` window).

- *Measured at tier 6 here:* about 1.0–1.07 s per step, against 0.54–0.66 s for the soot-free `hearth` proof. Both
  ran beside other jobs; renders were running beside these bakes. That gives about 46 min for the 4.5 s window, at
  1.4 GB RSS and a 2.5 GB peak footprint.
- *Tier 8:* about 3 times tier 6 (`SIMULATION_SHOWCASE.md`), so **about 2–2.5 h of Metal and about 3–3.5 GB**.
  Not measured.
- Renders as in `hearth` final: 1920×1080, spectral, 256 spp. These 960×540, 64 spp proofs took 2–3 min per frame on
  8 threads, so expect roughly 30–45 min per frame (extrapolated).

**Not physically methane.** NIST measures no escaping methane soot.

- Yield 0.03 (B) is the r272 least-squares fit, but even B lets about half its soot escape (r272).
- Yield 0.1 is a surrogate: about three times methane's fitted soot.
- With soot radiation on (C, E) its radiant fraction sits between heptane and toluene (r273: 0.26 frozen at yield
  0.1, not measured in these runs).
- C's smoke column is a sooty-fuel look, not methane.
- G is inconsistent: its soot glows in the renderer but does not cool the gas in the solver (radiative fraction fixed
  at 0.20).
- The chemistry light (blue) is a look choice (`3e-6` per W/m³, r272), labelled `SYNTHETIC_NON_PREDICTIVE`.

**Images.** All in `rendered/fire_look/` (git-ignored). Names are `<variant>_<settings>_t<fire time>_<spectral|rgb>_ev<EV>.png`.

- *Exposure.* The best exposure puts the 99.9th-percentile pixel at ACES input 3, rounded down to 0.5 EV; nothing
  clips. The common exposure is `..._ev3.0_common.png`.
- *Tone mapping.* From the linear EXRs (`rendered/fire_look/raw/*_denoised*.exr`), with RISE's Narkowicz ACES and
  sRGB.
- *Contact sheet:* `rendered/fire_look/fire_look_contact_sheet.png`. Row 1 is C's time strip at its best exposures.
  Then one time strip per variant at the common EV 3, so steadiness and brightness compare. Last rows: the last frame
  of each variant at its best exposure, spectral and RGB.
- *Look-dev set:* a fixed camera 2.3 m from the burner showing 0–1.2 m above it. A dark neutral floor (0.12) and wall
  (0.30) 1.54 m behind the burner. A grey sphere (0.5) and a white card (0.85) beside the burner. A very faint neutral
  fill (directional, power 0.004) so smoke shows against the wall; at EV 6+ the fill, not the fire, dominates the card.
  Scenes, generator, tone and VDB-statistics tools are in `/Volumes/SSD/claude-tmp/firelook-scratch/` (scratch, not in
  git).

**Run notes.**

- B was stopped cleanly (`SIGINT`, exit 3) at step about 1260 when a fire test run started on the GPU. It resumed
  from its step-1272 checkpoint after that run; the two parts took 1161 s and 659 s. The test run overlapped this bake
  for about 5 min before the stop.
- D (yield 0.3, cap 0.45, self-absorption 1) and A (soot off) were dropped on the coordinator's instruction.
- No bake had a retry (`hard_bound_retries=0`, `physical_projection_retries=0`). Every bake printed the fast-mode
  "NOT certified" notice.

## Why the flame shrinks at 2.3 s

Added 2026-11-28. No source, scene, test or git change. Scratch, tools, logs and tables are in
`/Volumes/SSD/claude-tmp/firediag-scratch/` (not in git). Plot: `rendered/fire_look/diagnosis/transition_timeseries.svg`
(and `.png`).

**Short answer.** The drop is the **burner pilot switching off**, not the domain. The case's ignition pilot holds the
burner-rim ring of floor cells at 900 K for exactly one flow-through time, `pilotEndS` = 1 × 3.27 m / √(g D*) =
**2.106 s** (`FireCase.cpp`: `pilotDurationMultiplier=1.0`). While it is on, it heats the air coming into the flame
base by about **12–16 kW at tier 6** (9–12 kW at tier 8). That is 40–50% on top of the fire's own ~26 kW of convective
heat. The bright 0.6–1.1 m flame of the first two seconds is a fire running on preheated air. The plume reaching the
top happens at the same moment **by construction**: the pilot length *is* the time a plume takes to cross the domain.
That is why the two looked linked. The settled flame from 2.5 s on is the solver's established flame: the same one every
earlier campaign measured after its 10.5 s discard. By the campaigns' own flame-height definition it is a physically
reasonable height (H99 0.68–0.81 m against Heskestad 0.646 m). What is unrealistic is that it is too **steady**: a
known solver deficit, not caused by tier or fast mode.

Verdict on the four candidates:

| Candidate | Verdict | Evidence |
|---|---|---|
| (a) domain or boundary: recirculation, O₂ starvation | **Rejected** | O₂ at the flame base stays at ambient (0.2328–0.2329); burning holds at 30–33 kW; an inert plume without a pilot does not change when its plume exits the top; widening the domain 2× (r244) leaves the drop unchanged |
| (b) resolution or fast mode damping the puffing | **Rejected as the cause of the drop.** The steadiness is a known solver trait | The same drop at tier 8 certified (r251, r264, r272) and tier 6 certified (r244); settled puffing is weak at every tier and mode |
| (c) start-up transient, reasonable settled flame | **Accepted, with the cause named**: the transient is the pilot plus the starting plume. It is a numerical start-up, not something a real burner does | Settled H99 0.73–0.77 m (tier 6 v4), 0.68 m (tier 8 r251), against Heskestad 0.646 m and FDS 0.73–0.81 m |
| (d) regression in version 4 or in the scene options | **Rejected** | The same trajectory in September (r244), October (r251, r264) and November (r272) runs and in today's version-4 bakes; the showcase uses default options |

### What was run and reused

- **New, tier 6, version 4, fast, soot-free (`T6`)**: the certified case from fire time 0.0625 to 5.0 s, frames every
  1/16 s, and a solver checkpoint after every frame (79 kept, `ckpt/T6/`). 3071 steps in 37 min, 1.38 GB RSS, no
  retries. Binaries are the look study's copies (`rise` `1e33398d…`, `rise_sim` `81dc596f…`).
- **New, tier 6 inert plume (`I6`, scene option `inert_plume TRUE`)**: the r271 non-reacting plume. It has no pilot
  and no reaction, only a frozen 26.4 kW heat source. Run 0–3.5 s, 20 min. This is the top-boundary control.
- **New, tier 6 candidate flow package (`K6`)**: compact stencil, limited carrier, Vreman zone below:12:0.2857, 0–4 s (option 2 below). A first attempt that also set `floor_emissivity 1` refused at step 1 (side finding).
- **Reused**: the look study's B, C, E and G caches (frames and final checkpoints). Read-only from
  `/Volumes/SSD/RISE/fire_production_calibration/`: r251 tier-8 certified (from-zero 25 s run, 354-entry checkpoint
  history, 235 statistics frames, `flame_height_instantaneous.csv`), r272 `tier8_y030` (tier 8 certified, candidate
  flow package plus soot 0.03, 235 frames), and the step logs of r244 (tier 6 baseline and lateral-double), r251,
  r264 and r272. No tier-8 bake was run: r251 and r272 already answer the tier question with certified 25 s data.
- **Tools** (scratch): `tool/fdiag` reads the frame VDBs. It gives peak T, gas volume above 1300 K, realized heat release,
  the H50/H90/H99 heat-release heights, mass flux through each open face, heat leaving the top, pilot-ring T, centreline
  T and w, and the 1000 K and 1300 K tip heights. `tool/ckdiag` reads the run checkpoints: species, MAC face velocities
  with exact density, O₂ in zones and in the inflow, boundary fluxes, and a pilot-heat estimate. `tool/stats.py` gives
  window statistics and a periodogram. Its face layout was checked against the exported velocity (agrees within 2e-7 m/s).
  `fdiag`'s H99 reproduces r251's own `h99_realized` (0.5967 m at 10.466 s).

### 1. Settled small flame: established behaviour, not new

Peak temperature from the step logs: the max over the domain, sampled every 256 steps. These runs are not today's.

| Run | Tier, mode, solver | 1.2–2.1 s | 2.4–2.5 s | Settled to 25 s |
|---|---|---|---|---|
| r244 baseline | 6, certified, Sept. | 1987–2167 K | 1376 K | 1470–1720 K |
| r244 lateral double (2× wider domain) | 6, certified | 1970–2095 K | 1420 K | 1480–1660 K |
| r251 | 8, certified, default flow | 2089–2096 K | 1778 K | 1660–1785 K |
| r264 | 8, certified, centred momentum | 2114–2183 K | 1939 → 1759 K | 1700–1820 K |
| r272 `tier8_y030` | 8, certified, candidate flow + soot | 2045–2194 K | 1814 K | 1705–1825 K |

Settled flame by the campaigns' measures, from frames (window statistics, `tool/stats.py`):

| Run, window | Peak T (K) | Gas > 1300 K (L) | Heat release (kW) | H90 / **H99** (m) | 1300 K tip, median (50% intermittency) / p10–p90 (m) | Puffing peak, share of 1–6 Hz power |
|---|---|---|---|---|---|---|
| r251 tier 8, 10.5–25 s | 1727 | 4.5 | 32.9 | 0.36 / **0.683** | 0.49 / 0.41–0.58 | 2.5 Hz, 0.07 (weak) |
| r272 tier 8 candidate, 10.5–25 s | 1747 | 4.8 | 32.3 | 0.45 / **0.814** | 0.64 / 0.52–0.77 | 3.4 Hz, 0.09–0.21 |
| T6 tier 6 v4 fast, 2.5–5 s | 1712 | 4.3 | 32.6 | 0.34 / **0.771** | 0.49 / 0.41–0.61 | 2.7 Hz, 0.23–0.36 |
| E (soot, look study), 2.5–5 s | 1612 | 3.6 | 30.4 | 0.31 / 0.732 | 0.41 / 0.33–0.53 | weak |
| G (soot, look study), 2.5–5 s | 1703 | 4.0 | 30.5 | 0.31 / 0.742 | 0.45 / 0.37–0.61 | 2.7 Hz, 0.21–0.35 |
| **T6 pilot phase, 1.0–2.06 s** | **2042** | **10.2** | 34.4 | 0.44 / **0.895** | **0.69 / 0.51–1.09** | **2.7 Hz, 0.38–0.48** |

References: Heskestad 0.646 m; FDS 0.73–0.81 m (99%, r255/r271); puffing correlation 2.74 Hz.

- Today's settled tier-6 fast flame **is** the flame every campaign measured. The 1300 K tip is the same as r251
  tier 8 (median 0.49 m), and H99 is within the spread of r251 and r272.
- The earlier campaigns never showed the bright phase because their statistics start after a 5 flow-through
  discard (10.53 s). The showcase window (`hearth`, fire time 1.0–4.5 s) lies entirely inside that discard.
- Visible height: the look study's "visible flame" (0.4–0.5 m settled) tracks the 1300 K tip, not H99.
- Tier, mode, version 4, the showcase options and the domain change none of this (§2–3).

### 2. Not a domain or boundary effect

Domain: the lateral extent is 7D = 2.1 m and the height is 5 × Heskestad = 3.27 m, at every tier. Only the cell width
changes with tier: 52×52×80 at 0.0408 m (tier 6), 69×69×106 at 0.0306 m (tier 8). The scene path exposes no domain
size. Tier changes resolution, never extent. The 0.30 m / 33 kW case is the only one accepted. The r244/r246 doubled
domains exist only in the `FireSequenceTest` campaign path. Lateral and top faces are FDS-style pressure-open
(`FIRE_SMOKE_DESIGN.md` §3.6): outflow at ambient static pressure, inflow at ambient total head carrying ambient
composition. r271(c) already found them equivalent to FDS.

Measurements, T6 (tier 6) and r251 (tier 8). Values before the transition are at 1.7–2.1 s, after it at 2.5–5 s:

- **O₂ at the flame base stays at ambient.** Mass-weighted Y_O₂ at z < 0.15 m, 0.17 < r < 0.35 m:
  0.2328–0.2329 both before and after, against ambient 0.2329.
  - It dips only during ignition (0.228 at 0.4 s), from the pilot's own products.
  - Side and top inflow carry Y_O₂ 0.2324–0.2329.
  - The whole 14.6 m³ box holds only 5–6 g of O₂ deficit and 6 g of products, so there is no filling up.
  - Near-floor air at 0.35 < r < 0.9 m stays at 300.0 K, with no soot carbon in it.
  - r251 at tier 8 is the same. The look study's B, C, E and G final checkpoints are the same.
- **Burning does not fall.** Realized heat release is 34.4 ± 4 kW at 1.0–2.06 s and 32.6 ± 1.1 kW at 2.5–5 s.
  73% (early) and 80% (settled) of it is released below 0.2 m. A starved flame would burn less.
- **Net mass flux** (kg/s, + = out of the domain):
  - Before 1.75 s the sides push air out (+0.03 to +0.17), as the expanding gas displaces ambient air.
  - From 1.75 s the sides draw air in. Side inflow is about 1.0 kg/s at 2.5–3 s and 1.7 kg/s at 5 s. Top outflow rises
    to match (1.0 → 1.7 kg/s).
  - Top inflow is transient and small: 0.2 kg/s at 2.4–2.6 s, then 0.01–0.05 kg/s, at 300–308 K and ambient O₂.
  - Entrainment keeps growing to 5 s while the flame has been steady since 2.5 s, so the flame does not follow the
    far field.
- **Top-boundary control (inert plume, no pilot).**
  - The inert plume's hot cap leaves the top at 1.56 s: heat leaves the top at 16 kW at 1.56 s and peaks at 68 kW at
    1.81 s.
  - Near-source fields before (0.8–1.5 s) and after (1.6–2.5 s) that event:
    - 1000 K tip, mean: 0.80 and 0.81 m.
    - Centreline T at 0.6 m: 744 and 734 K.
    - Centreline w at 0.6 m: 3.27 and 3.47 m/s.
  - So the plume leaving the domain does **not** change the near field. Only a slow drift follows (1000 K tip
    0.65 m at 2.5–3.5 s).
  - The reacting flame instead loses 60% of its hot gas within 0.2 s of the pilot switching off.
- **Widening the domain 2×** (r244 lateral double, tier 6 certified) gives the same 2.1 s peak and the same drop and
  settled level (above).
- **The taller domain** (r246, 2× height, pilot clock matched to the original 2.106 s) left the settled
  central-column heat release within 3% (1350 → 1312 W) and the puffing unchanged. Its early-time data were deleted in
  2026-10, so it cannot show the start-up; with its matched clock, its pilot also ended at 2.106 s.

**Timing in T6.**

- The pilot ring sits at 884–890 K until 2.106 s, then drops to 712 K (2.13 s), 416 K (2.19 s) and 314 K (2.31 s).
- The estimated pilot heat falls from 12–16 kW to 0 over the same frames.
- Heat starts leaving the top at 2.19 s (5 kW), then 91 kW at 2.31 s: the starting plume's stored heat (domain sensible
  energy peaks at 87 kJ at 2.25 s, then falls to 54 kJ at 5 s).
- Peak T falls from 2124 K (2.19 s) to 1952 (2.31), 1825 (2.38) and 1731 (2.50).
- Gas above 1300 K falls from 9.4 L to 3.7 L. In the base band (z < 0.2 m) it falls from 5.0–6.5 L to 2.2–3.4 L.
- The energy log of the look study's bake G gives the same pilot size independently:
  - d(sensible energy)/dt falls from 43.7 kW (2.076 s) to 18.5 kW (2.170 s).
  - Over that interval, under 2 kW leaves through the top.
  - So about 15–20 kW disappears when the pilot stops. This is an energy-budget inference: radiation and the small
    heat-release change are not separated.

What cannot be fully separated with scene-level controls: the pilot and the plume exit coincide by construction, and
neither the pilot duration nor the domain is a scene parameter. The attribution to the pilot rests on four
observations together:

1. No starvation or recirculation at the base.
2. The inert control shows the top exit does not reach the near field.
3. The lateral and top domain studies leave the settled state unchanged.
4. A heat source of 12–16 kW goes away exactly then, at the flame base, where most of the lost hot gas was.

A direct test (pilot ended earlier or later, or a taller domain with the same pilot) needs the solver-side changes
below. It was not run.

### 3. Resolution and mode

Tier 8 certified (r251, r264, r272) shows the same transition at the same time and settles to the same height as tier
6 fast:

- 1300 K tip median: 0.49 m at tier 8, 0.49 m at tier 6 soot-free.
- H99: 0.68 m at tier 8, 0.77 m at tier 6.
- Tier 8 runs only about 15 K hotter at peak (1727 against 1712 K) and holds 4% more hot gas.

So tier and fast mode neither cause the shrink nor would undo it. A tier-8 bake of today's build was not needed and not
run.

### 4. Puffing and how lively the flame is

The settled flame does puff at the right frequency (2.5–3.4 Hz at every tier), but weakly:

| Phase | Flame-tip spread (sd), tier 6 | Range | Tier 8 r251 sd |
|---|---|---|---|
| Settled | 0.09 m | 0.41–0.61 m (p10–p90) | 0.07 m |
| Pilot phase | 0.25 m | 0.51–1.09 m | — |

- A real 33 kW pool fire pulses roughly 0.4–1 m, which is what the pilot phase looks like.
- This is the known near-base unsteadiness deficit (r271 §22–§31): puffing at about a quarter of FDS's amplitude, the
  base relaxing to steady after start-up. It was measured in certified mode at tiers 6, 8 and 10.
- With the hot pilot ring, the base is forced harder: puffing share 0.4–0.5 of 1–6 Hz power against 0.2–0.35 settled.
  That is why the start-up looks lively.

### 5. Is start-up itself the oddity?

Yes. Two things overlap in the first 2.1 s:

- **The pilot.** It is an ignition device that stays on for a whole flow-through time.
- **The starting plume.** The domain stores heat at about 40 kW until the plume's cap exits, about 87 kJ in total.

Neither matches a real burner after its first half second. The settled state matches the campaigns' measures and
Heskestad within the known RISE biases (H99 +6–26% over Heskestad). The two things wrong with it as a showcase are:

- it is steady;
- it is only modestly hot. Peak 1600–1750 K instantaneous; the volume above 1300 K is 4 L.

**Side finding.** In version-4 fast mode at tier 6, `floor_emissivity 1` refuses at step 1: "fire floor surface balance
residual failed". So the r270 floor, which is part of the r272 candidate set, cannot be used from a scene in fast mode
today. Not investigated (no source change this round). It needs a look before anyone ships the candidate package with
the floor.

### Options for a sustained, lively, taller flame

Ranked by cost.

1. **Stage the shot inside the pilot phase.** Cost: none.
   - The flame is established and lively from about 1.0 s to 2.06 s fire time (T6: H99 0.65–1.17 m, 1300 K tip
     0.51–1.09 m, puffing at 2.7 Hz). That is 17 frames at 16 fps, 1.06 s.
   - In `hearth` (`frame_origin_s 1.0`) these are scene frames 0–16. The current hero frame 40 (fire time 3.5 s) is
     settled.
   - Hold-and-loop or slow motion stretches it. The last usable frame is fire time 2.0625 s; 2.125 s already has the
     ring cooling.
   - This look depends on a non-physical ignition aid. Say so if the shot is labelled as simulation.
2. **Scene-level flow options available today: the r271/r272 candidate package.**
   - `compact_viscous_stencil TRUE`, `limited_momentum_carrier TRUE`, `vreman_zone below:16:0.2857` at tier 8
     (`below:12:0.2857` at tier 6, zone top 0.5 m).
   - From r272 tier 8 (certified, 25 s), against r251 default:
     - H99 0.81 against 0.68 m;
     - 1300 K tip 0.64 against 0.49 m;
     - hot gas 4.8 against 4.5 L;
     - heat-release puffing share 0.21 against 0.08 at 3.4 Hz.
   - So it is about 30% taller and somewhat livelier. It is the closest to FDS's plume (r271 §37–§40).
   - Cost: the same bake time as default.
   - The zone layer is tier-specific: about 0.5 m / cell width.
   - Leave `floor_emissivity` out in fast mode (side finding above).
   - It does **not** bring back the pilot phase's 0.5–1.1 m pulsing.
   - **Tier-6 check (`K6`, new):** the package without the floor, `below:12:0.2857`, version 4 fast, 0–4 s, 29 min,
     1.58 GB RSS.
     - The drop is the same: peak 2064 K at 1.0–2.06 s, then 1670 K.
     - Over 2.5–4 s it is no better than default T6 over the same window:
       - H99 0.73 against 0.81 m;
       - 1300 K tip median 0.45 against 0.53 m (sd 0.05 against 0.08);
       - hot gas 3.9 against 4.4 L.
     - So at tier 6, early in the window, it makes the flame slightly shorter and steadier.
   - The r272 gain is measured only at tier 8, over 10.5–25 s, and with the floor on. Before relying on it for
     `hearth`, bake one tier-8 fast window (about 75–90 min) and check the 1–4.5 s heights. Not run here.
3. **Solver option: let the pilot run longer.** Small code, owner decision, labelled non-physical.
   - The look the owner liked is the pilot flame. A scene parameter such as `pilot_duration_multiplier` (default 1)
     would keep it for the whole window.
   - The change is a few lines: a new `FireSolver` option feeding `d.pilotDurationMultiplier` in `FireCase.cpp`
     line 448, through the case record.
   - Re-baseline implications:
     - `duration_multiplier_t_ft` is serialized into the canonical case record (line 576).
     - Any non-default value changes the case ID and every fire key.
     - The default must stay byte-identical, so `FireCaseTest` r57 and `SimulationSceneTest` J stay pinned.
   - It is an ignition aid kept burning, worth 12–16 kW of free heat at tier 6 (9–12 kW at tier 8). It would be marked
     `SYNTHETIC`, like the chemistry light.
   - Cost: about half a day plus review. Bakes cost the same.
4. **Solver work: a larger certified fire.**
   - Heskestad says a 1 m mean flame on the 0.30 m burner needs about 73 kW. A bigger burner gives a broader flame
     rather than a taller one.
   - The scene path refuses anything other than 0.30 m / 33 kW, and the domain, tier tables and pilot all derive from
     the case.
   - Lifting the guard needs a case validation run, a cost check and owner review. That is days of Metal and review,
     with no re-baseline of existing keys if added as new cases.
   - It also inherits the steady-base deficit.
5. **Solver research: the near-base unsteadiness deficit** (r271 §22–§31). It is the real cause of the "steady stub",
   and the open physics problem. Weeks; not a showcase item.

**Not recommended.**

- Higher tier alone: tier 8 is the same height (§3), at about 3× the cost.
- Domain changes: they are not the cause (§2) and are not exposed.
- No boundary bug was found. The open faces behave as specified: ambient inflow composition, no product re-entry.

**Inferred rather than measured:**

- The pilot heat values. They are an upwind-face estimate of heat added to air entering the ring cells, cp = 1100 J/kgK.
  The energy-log difference in G agrees, but that difference also contains radiation and heat-release changes.
- The causal attribution to the pilot. No run with a different pilot duration was possible without source changes.
- Boundary mass fluxes from frames use ρ = ρ∞·300/T, with ambient molar mass. The checkpoint values use exact density
  and agree to a few per cent.
- The real-fire pulsing range (0.4–1 m) is the coordinator's figure, not measured here.
