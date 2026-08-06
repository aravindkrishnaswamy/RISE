# Fire constituent optical preset v1 — draft record (§12 Q2)

**Status: DRAFT FOR OWNER REVIEW — nothing here is adopted.** This document
assembles the candidate v1 constituent optical presets with full provenance,
uncertainties, applicability limits, the §4.3 total-anchor consistency check,
and every computation shown. Adopting these records is a design decision
routed through the main design loop. Machine-readable drafts:
[docs/data/](data/) (`fire_optics_*.draft.json`), which carry the per-value
provenance the §8 metadata contract needs; final adoption re-encodes them as
RISE-CBOR64-v1 with the record ID as SHA-256 of the exact bytes.

Compiled 2026-07-31 against design revision 45. All arithmetic computed
(session scratchpad `fire_optics_calc.py`, pure-stdlib Python: Mie code
validated against Bohren–Huffman m=1.55/x=5.213 → Q=3.10500 vs 3.10543,
Wiscombe x=10/m=1.5+0.1i, and the analytic Rayleigh limit; Planck kernel
validated against the σT⁴ identity). Paywalled sources flagged, never
silently substituted.

**Headline answer to the §12 Q2 hypothesis ("closeable entirely from
published literature"): YES, with three owner decisions and two library
pulls** — no laboratory work required. The decisions: (1) E(m)
magnitude-vs-shape (§1.4), (2) the young-soot d_p range (lead-only, §2.3),
(3) the fresh-vs-aged applicability statement for condensed organics (§4.4).
The pulls: the C&C 1990 primary (uncertainty + long-wave fit validity) and
the Mulholland–Croarkin 2000 body (per-fuel spread, coverage factor).

---

## 1. λ-dependent soot E(m) table — Chang & Charalampopoulos 1990

### 1.1 The dataset

C&C 1990 (Proc. R. Soc. Lond. A 430:577–591, DOI 10.1098/rspa.1990.0107).
**Primary is paywalled** (verified: publisher 403; Semantic Scholar
`isOpenAccess: false`; outside the Royal Society free-archive window — the
design's "openly hosted" belief is **wrong**). Dispersion formulas verified
verbatim against three independent secondary sources (Wen et al., *Sensors*
22:8199 (2022), Eqs. 5–6 p. 5; Yelverton NCSU PhD thesis Eq. 1.1 pp. 19–20;
Flanner et al., ACP 12:4699 (2012), Eqs. 13–14 p. 4706):

> n(λ) = 1.811 + 0.1263 ln λ + 0.0270 ln²λ + 0.0417 ln³λ
> k(λ) = 0.5821 + 0.1213 ln λ + 0.2309 ln²λ − 0.0100 ln³λ  (λ in µm)

- **Measured range 0.2–6.4 µm** (verified via Zhu et al. 2002 NIST copy;
  Moteki et al. 2023 preprint; ADS abstract) — matches the design's claim.
- **Fit validity 0.4–30 µm** — secondary-only (Thermopedia et al.); the
  > 6.4 µm portion is Kramers–Kronig extrapolation. §12 Q2 requires a
  "continuity-checked long-wave extension" for §3.5's low-T Planck means:
  **the C&C fit itself extends to 30 µm and is the natural candidate, but
  its validity claim must be checked against the primary** (library pull).
- **Uncertainty: not recoverable without the primary.** No accessible source
  reproduces C&C's numeric error estimates. Library pull.
- Experimental basis: premixed propane–oxygen flat flame, φ=1.8; in-situ
  extinction + dynamic light scattering at 488 nm, Kramers–Kronig inversion;
  spherical-particle assumption — C&C themselves call the result an
  *effective* index (Moteki et al. 2023, p. 3).

### 1.2 The digitized table

E(m) = −Im[(m²−1)/(m²+2)] under m = n − ik (positive by construction), with
F(m) = |(m²−1)/(m²+2)|², tabulated at 5 nm steps over 380–780 nm in
[fire_optics_em_soot_cc1990.draft.json](data/fire_optics_em_soot_cc1990.draft.json)
(81 rows). Anchors:

| λ (nm) | n | k | E(m) | F(m) |
|---|---|---|---|---|
| 380 | 1.6763 | 0.6900 | 0.2875 | 0.2954 |
| 500 | 1.7225 | 0.6123 | 0.2478 | 0.2735 |
| 550 | 1.7362 | 0.5942 | 0.2382 | 0.2701 |
| 632.8 | 1.7549 | 0.5759 | 0.2277 | 0.2682 |
| 700 | 1.7675 | 0.5687 | 0.2226 | 0.2689 |
| 780 | 1.7806 | 0.5664 | 0.2193 | 0.2715 |

E(380)/E(780) = 1.311 — the table is mildly chromatic on top of the 1/λ law.

### 1.3 Fixture check

The Dalzell–Sarofim fixture m = 1.57 − 0.56i evaluates to **E = 0.2595 ≈
0.26** in this implementation — the design's analytic fixture is reproduced
exactly by the E(m) formula. The C&C *table* gives E(632.8) = 0.228, **12 %
below** the fixture: a dataset difference, flagged, not an implementation
error and not tuned away.

### 1.4 Magnitude conflict — RESOLVED by owner decision, 2026-08-05 (see §1.5)

The analysis below is retained as the decision's evidence base. **Decision:
modified option (b)** — C&C spectral shape, normalized to measured mass
absorption; raw C&C not adopted unchanged; no visible/IR splice.

Modern consensus E(m) in the visible is **0.32–0.40** (Liu, Yon, Fuentes,
Lobo, Smallwood, Corbin 2020, *Aerosol Sci. Tech.* 54:33–51: measured
0.35–0.40, lower limit 0.32 from mean MAC 8.0 m²/g @550). Quantified
consequences (ρ_soot = 1.8 g/cm³, MAC = 6πE/(ρλ)):

| E(633) source | MAC(633) m²/g | vs measured in-flame MAC 7.1 ± 1.1 (Forestieri 2018) |
|---|---|---|
| C&C fit 0.228 | 3.77 | −47 % |
| Dalzell–Sarofim 0.26 | 4.30 | −39 % |
| Liu 2020 range 0.32–0.40 | 5.29–6.62 | −25 % to −7 % |
| implied by measurement | (E ≈ 0.43) | — |

Adopting C&C as-is under-predicts visible soot absorption/emission by
~40–50 % against direct MAC measurements. Options for the owner:

- **(a) C&C as-is** — the design's named lead; best available *spectral
  shape* 0.4–30 µm from one self-consistent KK-constrained measurement;
  known low magnitude.
- **(b) C&C shape, rescaled magnitude** — precedented: Flanner et al. 2012
  kept C&C's non-constant coefficients and refit only the constant offsets
  to hit a modern index at 550 nm. Keeps the IR extension, fixes the
  visible magnitude; the rescaling target (B&B void-fraction line vs Liu
  consensus) is itself a choice.
- **(c) modern constant-E visible + C&C shape for IR only** — simplest, but
  introduces a splice point needing the §12 continuity check.

The E(m) choice is shared with the sim via C₀ = 6πE(m) (§3.5) and carried in
§8 metadata — whichever option is taken applies to both sides.

### 1.5 The frozen form (owner decision, 2026-08-05) — `mac_equivalent_E`

> **E_eff(λ) = 0.42017 · E_CC(λ) / E_CC(550 nm)**, pinned density 1.8 g/cm³.

The normalization corresponds to **MAC(550) = 8.0 m²/g** — Liu et al. 2020's
central value (8.0 ± 0.7 from ten direct measurements; full text verified by
the owner via the NRC-Canada repository), statistically compatible with
B&B's 7.5 ± 1.2. All arithmetic re-verified this session:
E_CC(550) = 0.2382, factor = 1.7638, E_eff(632.8) = 0.4017,
**MAC(632.8) = 6.647 m²/g — within +2.0 % of the independently derived
cool-carbon absorption 6.52** (treated as corroboration, not tuned against;
it makes the φ(T) hot↔cool transition near-continuous in absorption).
Machine-readable record with the full 81-row [λ, E_eff, MAC] table:
[fire_optics_mac_equivalent_e.draft.json](data/fire_optics_mac_equivalent_e.draft.json).

**Semantics**: the quantity is named `mac_equivalent_E` /
`effective_absorption_function`, *not* an unqualified index-derived E(m) —
RISE's Rayleigh expression applies no separate aggregate-interaction
correction, so the effective value must absorb everything needed to
reproduce measured absorption per transported soot mass. (Liu's E > 0.32
lower bound is therefore not the runtime central value; it presumes
optical-model corrections RISE does not apply.) **MAC(λ) is the normative
quantity**; E_eff is derived via the pinned density, and density + table
live in one hashed payload so a later density change cannot silently change
radiance.

**Corrections to §1.4's argument, adopted with the decision:**

- *Simulator coupling was overstated* (verified against §3.5): in the
  burning β-branch, q̇‴_rad = β·e with β = χ_r·Q̇_tot/Σe·V, so total
  radiative loss is pinned at χ_r·Q̇ and a uniform opacity rescale cancels
  through β. E still affects the spatial allocation of cooling, the
  constituent shares, the β ≤ 1 admissibility/optical thickness, the
  implicit local temperature update, and the post-fire γ branch — but not
  the burning-branch global loss. "Low E → sim runs hotter" is not
  generally true for this design.
- *Forestieri scope*: the 9.1/7.1 m²/g plateau describes larger mature soot
  (≳160 nm) — it anchors the mass-to-absorption normalization, not young
  in-flame soot per se. Its nascent-soot AAE 1.38 ± 0.36 is compatible with
  the C&C visible shape — newly computed corroboration: **the C&C shape's
  own visible AAE (380–780 nm) is 1.377**.
- *Constant-E error*: §1.4's "E = 0.39 gives MAC(633) ≈ 6.5" assumed
  constant E; preserving the declining C&C shape at that normalization
  gives 6.17 (verified). The 8.0 m²/g normalization gives 6.647 and is what
  produces the near-continuous hot/cool transition.
- *C&C primary now read* (owner full-text access): measurements cover
  0.2–6.4 µm; the fitted expressions are intended for 0.4–30 µm; the fit
  agrees with the KK-inferred indices on average ~5 % in n, ~6 % in k.
  This closes the range/fit question; the > 6.4 µm portion remains a
  KK-constrained fit, not an independently measured absolute spectrum.

**Qualification**: one nominal record, no selectable "conservative" scene
variants. Uncertainty components recorded separately: visible normalization
±16 % (conservative, covers the B&B ensemble); C&C fit/shape ~5–6 %; a
larger explicit model-form uncertainty on the unmeasured long-wave
extension; applicability = population-averaged hot-carbon closure (not a
material constant, not a maturity model).

**Gates**: (1) MAC(550) = 8.0 and the derived 6.647 at 632.8 nm; (2) C&C
visible shape anchors (E ratio 1.311 over 380/780; AAE 1.377); (3)
independent numerical Planck means and derivatives over the full certified
T range; (4) β ≤ 1 feasibility, spatial cooling redistribution, and
post-fire sensitivity across the uncertainty envelope; (5) the eventual
§12-item-4 absolute flame-radiance dataset as independent validation —
never a hidden one-point refit.

**Ablations**: raw C&C and Dalzell–Sarofim stay as named
ablation/regression records; neither remains the Phase-C predictive
default. (Phase-A's analytic E = 0.26 fixture stays a non-predictive
fixture per §12.)

---

## 2. Hot-soot ω_hot and g_hot — RDG-FA over published aggregate statistics

### 2.1 Method (all formulas from open sources)

RDG-FA per Sorensen 2001 (open KSU copy), *Aerosol Sci. Tech.* 35:648–687:
σ_abs^agg = N·σ_abs^m (Eq. 29); total scattering σ_sca^agg =
N²σ_sca^m·G(kR_g) with the Dobbins–Megaridis factor G = [1 +
(4/3D_f)k²R_g²]^(−D_f/2) (Eq. 35); scattering-to-absorption ratio ρ_sa =
N·G·(2/3)x_p³·(F/E) — algebraically identical to Sorensen Eq. 115 and
Krishnan–Faeth Eq. 17 (verified); ω = ρ_sa/(1+ρ_sa); g = ⟨cos θ⟩ over the
(1+cos²θ)·S(q) phase function with the Dobbins–Megaridis structure factor
(Guinier + power-law, value-and-slope-matched crossover at (qR_g)² = 3D_f/2).
**Fisher–Burford and exponential structure factors rejected** per Sorensen
p. 660 (C = 2.44/2.73 at D_f≈1.8 over-predict large-angle scattering; an
earlier provisional computation here used Fisher–Burford and was corrected).
RDG-FA accuracy: ≤10 % for phase-shift parameter 2kR_g|m−1| < 1 (Sorensen
p. 668; Farias, Carvalho, Köylü & Faeth 1995, *J. Heat Transfer* 117:152 —
open NIST copy — whose conclusion 4 states the refractive index, not RDG-FA,
dominates the error budget).

**Prefactor-convention trap, resolved and pinned**: Köylü–Faeth's k_f = 8.5
± 0.5 uses N = k_f(R_g/**d_p**)^{D_f}; Sorensen's k₀ uses the primary
*radius*. Conversion k₀ = k_f/2^{D_f} = 2.44 (verified against Sorensen
p. 680 verbatim). Sorensen's own DLCA k₀ = 1.3 ± 0.2 disagrees with 2.44 by
~1.9× — a real unresolved literature discrepancy (Sorensen p. 669); this
record pins the K&F value for consistency with the K&F morphology and the
ρ_sa validation below.

### 2.2 Inputs

| input | value | provenance | status |
|---|---|---|---|
| N, young in-flame soot | 30–80 | Krishnan & Faeth NISTGCR 00-796 p. 84 (open, OCR-read; same-author restatement of the Köylü–Faeth underfire data) | **verified** |
| d_p, young soot | 20–35 nm (central 30) | Megaridis & Dobbins CST 71:95; Sunderland et al. CF 100:310 (values in figures) | **lead-only — not table-verified** |
| N, overfire (validation) | 260–552 per fuel | NISTGCR 00-796 Table 2 p. 56 (= Köylü & Faeth 1992/1994a data, openly republished) | verified |
| d_p, overfire | 32–51 nm per fuel | ibid. | verified |
| D_f | 1.79 ± 0.05 | ibid. footnote (Krishnan et al. 1999) | verified |
| k_f | 8.5 ± 0.5 (≡ k₀ 2.44) | ibid.; Sorensen p. 680 | verified |
| F/E, measured | 0.63 (351 nm) → 0.98 (514) → 1.17 (633) | NISTGCR 00-796 Table 4 p. 58 | verified |
| ρ_soot | 1880 kg/m³ | Wu et al. 1997 via ibid. | verified |

### 2.3 Results and validation

Validation against measured overfire optics (computed with per-fuel verified
morphology): ω(514.5) = 0.209 (ethylene), 0.219 (heptane), 0.304
(acetylene), 0.329 (toluene) vs Köylü–Faeth's measured ρ_sa = 0.22–0.41 →
ω = 0.18–0.29; ω(633) = 0.19–0.31 vs Mulholland & Choi's 0.19–0.25 (via
Sorensen p. 681); independent: Heuser 2025 SSA(550) 0.10–0.29, Forestieri
2018 SSA(532) ≈ 0.20. **Three independent routes agree.**

Young in-flame soot (the hot constituent), over N ∈ [30, 80] × d_p ∈
[20, 35] nm at 550 nm: **ω = 0.05–0.16 (central 0.10)**; **g = 0.09–0.37
(central 0.22)**. Spectral run (d_p 30, N 50): ω falls 0.143→0.061 and g
0.355→0.113 across 380→780 nm.

### 2.4 Fixture comparison — one confirmed, one flagged

- **ω_hot ≈ 0.10: CONFIRMED** (central computed value for the verified
  young-soot N range).
- **g_hot ≈ 0.5: NOT CONFIRMED for in-flame soot.** Computed young-soot
  g(550) is 0.09–0.37. g ≈ 0.5–0.6 is the *overfire/mature-aggregate* value
  (computed 0.60 at 550 nm for N=400) — which in this design's φ(T)
  partition belongs to the **cool-carbon** constituent. Flagged as a
  disagreement per instructions, not tuned. If adopted, g_hot ≈ 0.2–0.3
  with the caveat that g is strongly size-dependent (a fixed-g HG lobe is a
  modelling choice; HG mis-fits fractal-aggregate phase functions by up to
  35 % in g — Pandey & Chakrabarty 2016, *Opt. Lett.* 41:3351).

Gaps: per-fuel ρ_sa table (Köylü & Faeth 1994a, ASME, **paywalled**);
in-flame d_p/N tables (Yazicioglu et al. CST 171:71, **paywalled**, best
closure target); validation of computed g against exact methods (Romshoo et
al. 2021 ACP supplement is **open** and tabulates MSTM-fit g coefficients —
recommended cross-check before freeze).

### 2.5 Owner rulings (2026-08-05)

**g_hot: ADOPTED — 0.22 at 550 nm, with 0.09–0.37 recorded as the supported
provenance range.** The computed value is correct for the constituent
actually named "hot soot" (young, comparatively small in-flame aggregates);
0.5 describes mature aggregates, which the φ(T) split already assigns to
cool carbon — keeping it would double-count maturation in the wrong
constituent. Because ω_hot ≈ 0.10, the hot constituent scatters weakly, so
this uncertainty has limited total radiometric leverage. The 0.5 value may
persist **only** in the explicitly synthetic regression fixture, and the
fixture and predictive preset must carry **distinct record IDs** so their
provenance cannot be confused.

**Young-soot d_p: small provenance gate before freeze.** Pull Yazicioglu et
al., Combust. Sci. Technol. 171:71–87 (2001), DOI
10.1080/00102200108907859 (TEM primary-particle and aggregate morphology in
laminar coflow diffusion flames) before declaring the predictive hot-soot
record frozen; the 20–35 nm range is provisional meanwhile and does not
block architecture. If the paper lacks directly adoptable tabulated values,
retain 20–35 nm with a **formal digitization record**: exact page and
figure; extraction method and digitization uncertainty; flame conditions
and sampling location; and how those conditions map to the hot-soot
certified domain. That replaces the vague "values live in figures" label.

---

## 3. Cool-carbon smoke triple (k_m, n, ω, g) + the §4.3 total anchor

### 3.1 The constituent preset

| quantity | value | provenance |
|---|---|---|
| k_m (extinction, 633 nm) | **8.7 ± 1.1 m²/g** (95 % expanded; coverage factor unverified — body paywalled) | Mulholland & Croarkin 2000, *Fire Mater.* 24:227–230 — abstract verified verbatim via OpenAlex: 7 studies, 29 fuels, post-flame overventilated **flaming** smoke; between-lab ANOVA dominant |
| MAC (absorption, 550 nm) | **7.5 ± 1.2 m²/g** (1σ of 17-study ensemble — a *different statistic* from the ±1.1) | Bond & Bergstrom 2006, *Aerosol Sci. Tech.* 40:27–67, §7.3 p. 48, §9.1 p. 58 (open) |
| ω (633 nm) | **0.25** (0.20–0.30) | B&B §9.1 p. 58, Table 7 p. 51 (six studies, 0.15–0.29) |
| n (spectral exponent) | **1.0–1.2**, valid 400–700 nm | B&B §7.6 p. 53 (λ⁻¹ ± ~0.15, visible only); Ouf et al. 2008 FSS 9:231 p. 238 (632→1064 nm EAE 1.03); Heuser 2025 Table 2 (EC-rich 1.12–1.23); Chen 2007 flaming pine ≈1.18 |
| g (633 nm) | **0.58** (0.51–0.64) | RDG-FA over verified overfire morphology (§2 machinery; per-fuel 0.512–0.638) |
| density | 1.8 g/cm³ (1.7–1.9) | B&B §9.2 p. 58 |
| index (if ever needed) | void-fraction line 1.75−0.63i…1.95−0.79i (550 nm), central ≈1.8−0.74i; **1.74−0.44i should be retired** | B&B §7.2.3 p. 47 Table 5, §9.2 p. 58 |

**Trap pinned in the record**: do **not** derive MAC from the refractive
index via Mie/RDG — B&B §7.5 shows a ~30 % under-prediction against measured
MAC; the preset pins measured MAC + ω directly (B&B's own recommendation).

The design's fresh-flaming n ≈ 1–1.5 claim: **supported and sharpened** to
1.0–1.2 for soot-dominated flaming smoke. The 2–2.5 exponents in the
biomass literature (Reid III p. 830) belong to OC-dominated field plumes —
a different regime, cleanly demonstrated by Heuser's EC/TC series (EAE 1.12
→ 3.08 as EC/TC goes 0.79 → 0).

### 3.2 The §4.3 consistency check — PASSES

The anchor is *total post-flame* extinction, deliberately not
per-constituent. Two-step check, all arithmetic computed:

**Step 1 — carbon-constituent internal coherence:**
MAC(633) = 7.5·(550/633) = **6.52 ± 1.04 m²/g** (B&B's own λ⁻¹ rule, inside
its 400–700 nm validity). With ω = 0.25: k_ext = 6.52/0.75 = **8.69 m²/g**.
Equivalently, the anchor implies ω = 1 − 6.52/8.7 = **0.251** — landing
exactly on B&B's independently recommended central SSA. Two disjoint
literature syntheses (atmospheric-absorption vs fire-extinction) are
mutually consistent to < 1 %.

**Step 2 — mixture dilution with the condensed constituent:**
k_total = f_EC·8.7 + (1−f_EC)·k_cond with k_cond(633) = 3.30 m²/g (§4's Mie
result). Staying inside 8.7 ± 1.1 requires **f_EC ≥ 0.80**. Measured flaming
wood: Chen et al. 2007 (*Environ. Sci. Tech.* 41:4317, open USDA copy,
Table 2 p. 4322) ponderosa pine wood at MCE 0.99: **EC/PM ≈ 0.80, EC/TC ≈
0.84** → k_total = 7.62–7.84 m²/g, **inside** the anchor band. Independent
cross-checks: Chen's own flaming-pine MEC(532) = 29.0/3.3 = 8.8 m²/g PM
(within 1 % of the anchor); Ouf et al. 2008 (open IAFSS) measured 9.25 ±
1.17 m²/g at 632 nm across acetylene/toluene/PMMA, fuel-insensitive.

**Verdict: the constituent presets are consistent with the anchor for
wood-class flaming smoke.** Applicability limit recorded: field-plume BC
fractions (5–9 %, Reid II pp. 803–805) describe smoldering-weighted whole
plumes where the flaming-only anchor does not apply; a smoldering-regime
smoke (c_condensed-dominated) legitimately falls below 8.7 — the secondary
literature's smoldering values (~4–5 m²/g) are consistent with this but
rest on unverified sources (SFPE chapter, paywalled).

### 3.3 Gaps

IR absorption for §3.5 Kirchhoff emission: B&B explicitly makes **no
statement outside 400–700 nm** — the cool-carbon IR closure must come from
the E(m) dispersion record (§1), which couples this record to the §1.4
decision and the > 6.4 µm verification. Derivative enclosures (§12 Q2) are
generated at freeze time from whichever tables are adopted.

---

## 4. Condensed-organics triple — Mie over published inputs

### 4.1 Verified inputs

| input | value | provenance |
|---|---|---|
| n (real) | 1.50 (1.42–1.55) | Reid et al. 2005 ACP 5:827 (III) §3 p. 837 (organic shell; Mulholland 1985; closure studies) — open |
| k (550 nm) | 0.004 (0.002–0.006) | Sumlin et al. 2018 JQSRT 206:392 via arXiv:1712.05028 Fig. 3 (κ flat ≥532 nm; peat BrC); Chakrabarty et al. 2010 ACP 10:6363 Table 1 (tar balls, 0.006 @532) |
| k (400 nm) | 0.020 (0.005–0.040) | Chen & Bond 2010 ACP 10:1773 Table 1 p. 1776 (α/ρ methanol extracts × ρ=1.2, k = (α/ρ)·ρ·λ/4π; T-dependent 210–360 °C). **Title correction: "wood combustion", not "wood pyrolysis"** |
| brown-carbon AAE | 6.9–11.4 | Chen & Bond §3.5 p. 1782 |
| size (fresh flaming) | lognormal, CMD 120 nm (110–130), σ_g 1.75 (1.7–1.8); VMD ≈ 0.26 µm cross-check | Reid II §3 p. 801, Tables 1 p. 802 & 7 p. 816 — open |
| density | 1.3 g/cm³ (1.2–1.4) | Reid II p. 803 (POM ~80 % of fresh smoke mass; Turpin & Lim 1.2 cited *second-hand through Reid* — primary paywalled) |

k(λ) model: power law between the 400/550 nm anchors below 532 nm
(exponent ≈ 5.05, AAE-consistent), flat 0.004 above per Sumlin.

### 4.2 Computed outputs (full Mie, validated code)

| λ (nm) | k_ext (m²/g) | ω | g |
|---|---|---|---|
| 380 | 7.63 | 0.872 | 0.684 |
| 450 | 6.05 | 0.940 | 0.655 |
| 550 | 4.31 | 0.976 | 0.621 |
| 633 | 3.30 | 0.974 | 0.595 |
| 780 | 2.10 | 0.970 | 0.547 |

Extinction Ångström exponent (450–633): **1.78**. Sensitivity: k_ext(550)
spans 3.95–4.44 over the size corners, 3.27–4.97 over n ∈ [1.42, 1.55]; ω
0.964–0.988 over k(550) ∈ [0.002, 0.006].

### 4.3 Validation against independent targets

- g(550) = 0.621 vs Reid III Table 5 p. 843 whole-smoke g = 0.55–0.63 —
  **inside**.
- α_s(550) = ω·k_ext = 4.21 m²/g pure-organic vs whole-smoke 3.6 ± 0.4
  (Reid III Table 5) — slightly above, correct direction (the BC fraction
  lowers the mixture's α_s; whole-smoke includes 5–9 % BC).
- ω(550) = 0.976 pure-organic vs whole-smoke fresh-flaming 0.74–0.85 (Reid
  III p. 834) — consistent by construction: the constituent split assigns
  the absorption to the carbon record.

### 4.4 Flags and applicability

- **Computed n_cond ≈ 1.8 vs the design fixture 0.5 and §4.3's "Mie regime,
  n ≈ 0–1, weakly chromatic" prose.** The verified *fresh* size
  distributions (CMD 0.12 µm, VMD 0.26 µm) are smaller than the "0.1–1 µm
  dominant mass" picture; n ≈ 0–1 describes *aged/coagulated* smoke. The
  preset must state fresh-flaming applicability; an aged variant needs its
  own size distribution. Flagged, not tuned.
- Chen & Bond's measured band is ~300–550 nm; red-visible k rests on
  Sumlin's (peat-BrC) flatness. Sumlin's per-λ SI tables ship with the
  **paywalled** Elsevier version — library pull would firm the red end.
- No verified IR (> 1047 nm) absorption dataset for condensed organics is
  in hand — the §3.5 Kirchhoff closure for this constituent is an open
  owner decision (candidates exist in the atmospheric literature but were
  not verified this session).

### 4.5 Owner ruling (2026-08-05): visible preset frozen; IR closure blocked

**n_ext ≈ 1.78 accepted**, with the domain stated precisely as: **"fresh,
dry, near-source condensed organic aerosol produced by flaming
combustion."** (Not "in-flame condensed organics" — material hot enough to
remain in the reaction zone may not yet be condensed. The n ≈ 0–1 language
belongs to aged/coagulated aerosol.)

**IR closure: it is prohibited to extend the 1047 nm value flatly, set IR
absorption to zero, or silently reuse the visible power law.** Sumlin
measured only 375–1047 nm on smoldering peat BrC — not a mid-IR
fresh-flaming surrogate — and at condensed-organic temperatures much of the
Planck power lies several microns into the IR where molecular vibrational
absorption can dominate. Before predictive §3.5 use, one of:

1. a composition-appropriate, **measured** mid-IR optical-constant or
   mass-absorption record; or
2. a conservative bound proving condensed-organic thermal cooling
   negligible over the certified state domain.

Until then, predictive runs with nonzero condensed-organic yield **fail
with reason `condensed_organics_ir_unclosed`**; preview continues.

---

## 5. Summary — what closes Q2, what needs the owner

**Closeable now from published literature** (confirming the working
hypothesis for Q2): the E(m) table (with decision 1), hot-soot ω (validated
three ways), cool-carbon k_m/ω/n (anchor check passes to < 1 %), condensed
organics k_m/ω/g/n (validated against Reid's independent targets), and the
total-anchor consistency for wood-class flaming smoke.

**Owner decisions — all four now taken** (2026-08-05): (1) E(m) →
`mac_equivalent_E`, C&C shape normalized to MAC(550) = 8.0 m²/g (§1.5);
(2) young-soot d_p → provisional 20–35 nm behind a Yazicioglu pull /
formal-digitization provenance gate (§2.5); (3) g_hot → **0.22 at 550 nm
adopted**, 0.09–0.37 provenance range; the 0.5 fixture survives only as an
explicitly synthetic record with a distinct ID (§2.5); (4) condensed
organics → visible preset frozen with the "fresh, dry, near-source,
flaming" domain statement; IR remains blocked with reason
`condensed_organics_ir_unclosed` until a measured mid-IR record or a
negligibility bound exists (§4.5).

**Library pulls** (all cheap): C&C 1990 primary; Mulholland & Croarkin 2000
body; Köylü & Faeth 1994a; Yazicioglu 2001; Sumlin SI tables; Turpin & Lim
2001.

**Not in scope of this record** (already-known design requirements that
land at freeze time): differentiable interpolation + derivative enclosures
(§12 Q2), RISE-CBOR64-v1 encoding and record IDs (§8), the φ(T) band
declaration, and the certified-domain statements.
