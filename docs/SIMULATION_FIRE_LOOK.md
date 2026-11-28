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
