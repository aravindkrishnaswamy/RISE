# Fire chemiluminescence records v1 — draft evidence package (§12 Q1)

**Status: DRAFT FOR OWNER REVIEW — nothing here is adopted.** This document
assembles the evidence for the three §12 Q1 tiers: the sooty-fuel
negligibility bound (`chem_model=none` predictive path, §7.0), the methane
candidate dataset (Lai et al. 2025), and the methanol/clean-flame options.
Adopting any record is a design decision routed through the main design loop;
this package exists to make that decision easy. Machine-readable drafts:
[docs/data/](data/) (`fire_chem_*.draft.json`).

Compiled 2026-07-31 against design revision 45. Verification discipline: every
number carries a citation (paper, table/figure/page); paywalled sources are
flagged and never silently substituted; all arithmetic below was computed, not
estimated (script: session scratchpad `fire_optics_calc.py`, pure-stdlib
Python; Planck integrals cross-checked against the σT⁴ identity).

---

## 1. Tier 1 — negligibility bound for sooty fuels (wood volatiles, wax, heptane)

### 1.1 The criterion being targeted

§7.0 (predictive-label gates): `chem_model=none` is predictive **only** when a
pinned measurement reports a 95 %-confidence upper bound below **both**

- (leg i) 1 % of that fuel/case's measured 380–780 nm radiant power, **and**
- (leg ii) the absolute radiance gate's uncertainty at every gated wavelength.

### 1.2 Headline result: the bound is NOT closeable from the open literature as it stands

The working hypothesis ("Q1 is closeable for sooty fuels via a negligibility
bound — no laboratory work") is **refuted in its strong form** by the
assembled evidence, on three independent grounds:

1. **The numerator anchor is paywalled and premixed-only.** The classic
   "chemiluminescence ≈ 10⁻⁴ of HRR" figure traces to Samaniego, Egolfopoulos
   & Bowman 1995 (*Combust. Sci. Technol.* 109:183–203, DOI
   10.1080/00102209508951901) — **paywalled; the specific value could not be
   verified** from any accessible copy. The best open anchor is the same
   author's CTR Annual Research Brief 1994 (pp. 29–43, Table 2, p. 31,
   openly hosted at Stanford CTR): reaction-zone radiative loss / chemical
   heat release = **0.4–2.5 × 10⁻³** for five lean premixed mixtures (CH₄/air
   φ=0.55: 0.55×10⁻³; C₃H₈/air φ=0.51: 0.40×10⁻³; diluted CH₄/O₂ mixtures up
   to 2.5×10⁻³). This is an upper bound on chem (chem emission is a subset of
   reaction-zone radiative loss) but it is loose — it includes non-chem
   reaction-zone gas radiation — and it is for *premixed* flames, where all
   fuel passes through a vigorous chemiluminescent zone. No published
   measurement of total visible chemiluminescent power for a *diffusion*
   flame (candle/wood/heptane class) was found.
2. **The denominator does not exist as a measurement.** No published direct
   radiometry of the 380–780 nm band power of a candle, heptane pool, or wood
   flame was found. All spectral flame-radiation datasets located (Klassen &
   Gore NIST-GCR-94-651; the NIST pool-fire series; Markstein's radiometry)
   are total or infrared (~1–5.5 µm). The visible-band power must currently
   be *derived* (χ_r × a Planck visible fraction at an assumed radiating
   temperature, or luminous flux ÷ luminous efficacy of the spectrum) — and
   the two derivations disagree by ~5× (candle, §1.4), because a flame is not
   isothermal and its radiant output is not all soot continuum.
3. **Leg (ii) cannot be evaluated at all yet.** It compares chem radiance to
   the absolute radiance gate's uncertainty *at every gated wavelength* — but
   the radiance-gate dataset is itself still open (§12 item 4: fuel,
   geometry, wavelengths, uncertainty all unselected). Tier-1 records are
   structurally blocked on Q4's closure regardless of what the literature
   says about leg (i). **This coupling should be recorded in §12** (report,
   not a criterion relaxation: leg (ii) is evaluable only after Q4 pins the
   gate's wavelengths and uncertainty).

### 1.3 Verified inputs

| quantity | value | source | status |
|---|---|---|---|
| Candle HRR | 77 ± 9 W (21 mm paraffin taper) | Hamins, Bundy & Dillon, *J. Fire Prot. Eng.* 15(4):265–285 (2005), p. 277 | open (NIST), full text read |
| Candle radiative fraction χ_r | 0.17 ± 0.01 | ibid., p. 280, Eqs. 3 & 5 | open, read |
| Candle peak soot temperature | 2000–2200 K | Thomsen et al., *Exp. Therm. Fluid Sci.* 82:116–123 (2017) | **paywalled**; abstract-verified |
| Candle luminous intensity | ≈ 1 cd (≈ 12.6 lm quasi-isotropic) | historical candela convention (NIST SI-redefinition pages) | convention, not a modern traceable measurement — flagged |
| Heptane pool χ_r | 0.30–0.40 (D ≲ 4 m); fit χ_r = 0.35·e^(−0.05·D[m]) | McGrattan, Baum & Hamins, NISTIR 6546 (2000), Eq. 3, Fig. 3, pp. 4–5; underlying data Koseki & Yumoto, *Fire Technol.* 24:33–47 (1988) (**paywalled**) | NISTIR open, read |
| Heptane luminous-zone emissive power | 50–100 kW/m² → T_eff ≈ 970–1150 K (σT⁴, our conversion) | NISTIR 6546 pp. 7–10 | open, read |
| Wood flame χ_r | ≈ 0.3 (Douglas fir) | Khan & Tewarson, SFPE Handbook 5th ed. Ch. 36 | **paywalled/print** — snippet only; must be confirmed against the printed table |
| Premixed reaction-zone radiative loss / HRR | 0.4–2.5 × 10⁻³ | Samaniego, CTR Annual Research Briefs 1994, Table 2, p. 31 | open, read |
| CO₂* absolute photon-rate model | i = 3.3(±0.3)×10³·exp(−2300/T)·[CO][O] mol-photon cm⁻³ s⁻¹, 200–700 nm, 1300–2700 K | Slack & Grillo, *Combust. Flame* 59:189–196 (1985), as codified in Nori & Seitzman AIAA 2008-953 Eq. (3) p. 4 (open, read) | usable for a *computed* bound |
| Absolute OH*/CH* rates, methane **diffusion** flames | 2D fields, mol·m⁻³·s⁻¹, integrating-sphere calibrated | Jin et al., *ACS Omega* 5:15922 (2020), Figs. 4–5, Table 1 | open access; **figures only, no data files** |
| Planck visible fractions | computed this session (table below) | `fire_optics_calc.py`; kernel validated against σT⁴ | computed |

Computed visible-band fractions (380–780 nm) of total emission — blackbody
and soot-weighted (emissivity ∝ 1/λ, §4.1):

| T (K) | blackbody | soot-weighted (1/λ) |
|---|---|---|
| 1000 | 0.0011 % | 0.0057 % |
| 1100 | 0.0046 % | 0.021 % |
| 1300 | 0.038 % | 0.151 % |
| 1500 | 0.168 % | 0.595 % |
| 1700 | 0.508 % | 1.61 % |
| 1900 | 1.18 % | 3.40 % |
| 2000 | 1.67 % | 4.62 % |
| 2200 | 3.01 % | 7.67 % |

### 1.4 The bound arithmetic, candle (paraffin wax) — the best-instrumented case

Denominator D_vis (candle 380–780 nm radiant power), two independent routes:

- **Route 1 (luminous flux):** 12.6 lm ÷ visible-band luminous efficacy of a
  soot-weighted Planck spectrum at the measured 1900–2200 K soot temperature
  (computed 103–131 lm/W_vis) → **D_vis ≈ 0.10–0.12 W ≈ 1.3–1.6×10⁻³ of HRR**.
- **Route 2 (radiative fraction):** χ_r·HRR = 13.1 W times the soot-weighted
  visible fraction at an assumed effective radiating temperature:
  T_eff = 1600 K → 0.13 W; 1800 K → 0.31 W; 2000 K → 0.60 W
  (**1.7–7.9×10⁻³ of HRR**). Route 2 overstates D_vis if a large share of
  χ_r is molecular gas-band IR (it is; CO₂/H₂O bands are excluded from
  soot-weighting but present in χ_r), and route 1 understates it if the 1 cd
  convention is low. The honest statement: **D_vis ∈ [0.10, 0.60] W**, with
  route 1 (≈0.11 W) the conservative choice for a negligibility bound.

Numerator N_vis (candle visible chem power) — no measured value exists.
Bracketing:

| numerator basis | N_vis (W) | N/D vs route 1 (0.11 W) | N/D vs route 2 @1800K (0.31 W) |
|---|---|---|---|
| CTR 1994 bound 2.5×10⁻³·HRR (premixed, all reaction-zone radiation) | 0.19 | **175 %** | 62 % |
| "10⁻⁴·HRR" (Samaniego-class; **paywalled, unverified**; premixed) | 0.0077 | **7.0 %** | 2.5 % |
| 10⁻⁵·HRR (if diffusion-flame chem is 10× below premixed — **unsourced**) | 0.00077 | 0.70 % | 0.25 % |

**Verdict (wax/candle): indeterminate at the 1 % threshold.** The defensible
open-literature bound (row 1) fails by two orders of magnitude; the
plausible-but-unverifiable estimates straddle 1 %. The bound is *probably*
satisfiable — the blue base of a candle is faint next to the yellow body, and
the 10⁻⁴ premixed numerator is very likely a large overestimate for a
diffusion flame — but "probably" is not a 95 %-confidence record, and §7.0
explicitly rules that lack of data is not evidence of zero emission.

### 1.5 Heptane and wood

Heptane pool (0.3–1 m class): χ_r = 0.30–0.40 but the luminous zone radiates
at T_eff ≈ 970–1150 K, where the soot-weighted visible fraction is only
0.006–0.03 % — visible power ≈ 1.3×10⁻⁵ to 1.3×10⁻⁴ of HRR (T_eff
970–1150 K). Against even the unverified 10⁻⁴ numerator the ratio is
**76–790 %**; against a 10⁻⁶-class diffusion-flame chem fraction it is
~1–8 %. **The heptane bound is *further* from closure than the candle's**,
because large sooty pools are surprisingly dim in the visible per unit HRR.
(Caveat recorded: T_eff from total emissive power underweights the small
bright 1300–1500 K regions that dominate the *visible*; a per-wavelength
measurement would likely raise D_vis. That is precisely the missing
measurement.) Wood: χ_r ≈ 0.3 is only snippet-verified (SFPE handbook,
paywalled); no visible-band data; same structure as heptane.

### 1.6 What would actually close tier 1 — the pinned-record path

In order of increasing cost:

1. **Owner library pulls** (cheap, do first): Samaniego 1995 (the CO₂*-per-HRR
   number and its stated uncertainty); Markstein's spectral-radiance series
   (Proc. Combust. Inst. 20:1055 (1985) and related) to check whether any
   run resolves ≤ 780 nm; Koseki & Yumoto 1988; Khan & Tewarson SFPE Ch. 36
   (wood χ_r digits); Thomsen 2017 (candle soot T, full text). If Markstein
   or a similar series contains even one visible-band-resolved luminous-flame
   spectrum, both legs of the candle bound become citable.
2. **A computed-bound record** (needs an owner design decision): §7.0 says "a
   pinned measurement reports a 95 % upper bound". A record built as
   *computation over published measured inputs* — Slack–Grillo CO₂* kinetics
   + Jin et al. 2020's calibrated CH*/OH* diffusion-flame fields (digitized)
   for the numerator; Hamins 2005 χ_r + measured soot temperatures for the
   denominator, all uncertainties propagated — is arguably within the spirit
   but not the letter. **The owner must rule whether a computed bound
   qualifies as the §7.0 record**; if yes, tier 1 is ~days of work plus the
   library pulls; if no, tier 1 needs a lab measurement (a
   radiometrically calibrated visible-band spectrometer pointed at a candle
   is a modest experiment, but it is laboratory work — refuting the "no lab
   work" hypothesis).
3. **Leg (ii) in all cases waits on §12 item 4** (the absolute radiance-gate
   dataset): un-evaluable until that record pins wavelengths and uncertainty.

Draft records `fire_chem_negligibility_{wax,heptane,wood}.draft.json` carry
the assembled inputs, the computed ratio matrices, and `status:
"not_closeable_from_open_literature"` with the specific missing items.

---

## 2. Tier 2 — methane: Lai et al. 2025 against §12 Q1's criteria

**Dataset identified and read in full** (accepted manuscript, CC-BY 4.0):

Lai, Y., Liu, X., Davies, M., Fisk, C., Wang, Y., Meng, S., Yang, H., Yang,
J., Hobbs, M., Zhang, Y., Willmott, J.R., "A Novel Method for Spectral and
Spatial Characterisation of Flames Using a Custom-Developed Hyperspectral
Imaging System", *Fuel* 393:135057 (2025), DOI 10.1016/j.fuel.2025.135057.
Open accepted manuscript: White Rose Research Online eprint 235078. Publisher
version paywalled. **No supplementary data files, no dataset link, no
data-availability statement in the accepted MS.**

Instrument/conditions: radiometrically calibrated hyperspectral imaging,
**418–708 nm**, 0.334 nm FWHM; premixed laminar methane–air Bunsen (14.5 mm
nozzle), CH₄ 0.5 L/min fixed, air 2.0–6.0 L/min, twelve equivalence ratios
Φ = 0.79–2.36. Absolute scale via silicon-photodiode radiometer transfer
(scaling factor ≈ 0.5×10⁻¹⁶ W/DL, Eqs. 5–7, Fig. 7); wavelength calibration
vs Hg-Ar/NIST; blackbody flat-fielding (Fig. 2); radiometer circuit
uncertainty "< 2.5 %" (§2.2.2); **no end-to-end radiometric uncertainty
budget**.

### 2.1 Criterion-by-criterion

| §12 Q1 criterion | verdict | evidence |
|---|---|---|
| absolute integrated spectral power per band | **figure-level only** | Fig. 6(b): calibrated spectrum (W) at Φ=1.35; Fig. 8(c): per-band integrated radiant power vs Φ. No numeric tables, no data files. Quantity is power at the instrument étendue (Eq. 3), not source-side radiance; geometry (2 mm FOV, 150 mm working distance) characterized but the conversion is not performed. |
| spatial profiles / mixture-state coordinates | **satisfied** | Fig. 8(a,b): per-band 2D maps at each Φ; Fig. 9: C₂*/CH* ratio maps; Fig. 5: inner-cone/post-flame segmentation across all twelve Φ. |
| normalization interval [λa, λb] per band | **not stated** | §2.3: bands integrated "across specific wavelength intervals" identified via HITRAN — numeric bounds appear nowhere. Authors plausibly have them. |
| geometry corrections | **partial** | Size-of-source, FOV, flat-field, wavelength calibration characterized; **no Abel inversion** (all maps line-of-sight), no solid-angle conversion to radiance. Axisymmetric burner → invertible in principle; the data cube is not released. |
| paired HRR | **not measured** | Global chemical HRR computable from stated flows for Φ ≤ 1; rich cases have substantial post-flame burning (§3.3 of the paper) so the inner-cone/post-flame HRR partition is not closable from released data. |

### 2.2 Band coverage vs §4.4's three channels

CH* A–X 431 nm — **yes**. C₂* Swan — **yes**, resolved into
(0,0),(1,0),(2,0),(0,1),(0,2). **CH* B–X ~390 nm — NO** (below the 418 nm
instrument cutoff). **CO₂* continuum — NO** (mentioned in the paper's
introduction, never decomposed; the 250–418 nm portion is entirely outside
the instrument range). H₂O emission is quantified instead.

### 2.3 Verdict

**ADOPTABLE WITH AUTHOR CONTACT — for the CH*(431 nm) and C₂* Swan channels
only.** Author contact (data cube + band-integration intervals + scaling
factor/geometry) would close criteria (a)–(c) and enable (d) via Abel
inversion; HRR pairing must be computed from the stated flows and is clean
only for Φ ≤ 1. **No amount of author contact makes this dataset cover CH*
B–X 390 nm or the CO₂* continuum** — those need a second, UV-extended source
regardless. Whether a two-channel Lai record plus a separately sourced CO₂*
record satisfies §12 Q1 for methane is a design decision for the owner.

Secondary conflict flagged: if the owner accepts *digitized figures* as
interim provenance (with digitization uncertainty recorded), a record could
be drafted without author contact — but criterion (c) (band bounds) is still
missing, so digitization alone cannot close Q1-methane.

### 2.4 Draft author-contact email (for the owner to send)

> To: Xuanqi Liu (corresponding author, CUMT); cc: Jon R. Willmott, Yang
> Zhang (University of Sheffield)
> Subject: Data request — calibrated hyperspectral methane-flame radiant
> power (Fuel 393:135057, 2025)
>
> Dear Dr. Liu,
>
> I read your paper "A Novel Method for Spectral and Spatial Characterisation
> of Flames Using a Custom-Developed Hyperspectral Imaging System" (Fuel 393,
> 135057, 2025) with great interest. We are building a physically based fire
> rendering system whose chemiluminescence model consumes absolutely
> calibrated, band-resolved flame emission data, and your radiometrically
> calibrated measurements are the strongest published candidate we have found
> for the methane–air case.
>
> The published figures cover our needs qualitatively, but adopting the work
> as a versioned quantitative dataset needs machine-readable values. Would
> you be willing to share:
>
> 1. The calibrated hyperspectral data cubes (radiant power per wavelength
>    per pixel) for the twelve equivalence ratios — or, failing that, the
>    integrated per-band values behind Fig. 8(c) as numbers;
> 2. The numeric wavelength-integration bounds used for each band (CH*, the
>    five C₂* Swan sequences, H₂O) referenced in §2.3;
> 3. The scaling-factor value(s) k with the associated solid-angle/étendue
>    parameters (FOV, working distance, aperture) needed to convert detected
>    power to source-side spectral radiance;
> 4. Any end-to-end radiometric uncertainty estimate beyond the radiometer
>    circuit's < 2.5 % figure — even an informal budget would help us record
>    honest uncertainties;
> 5. Whether you have (or plan) measurements extending below 418 nm — the
>    CH* B–X band near 390 nm and the CO₂* continuum are the missing pieces
>    for a complete visible chemiluminescence record.
>
> We would cite the paper as the data source, record the dataset under your
> names in our provenance metadata, and share our derived record back with
> you if useful.

---

## 3. Tier 3 — methanol / clean blue flames: realistic options

### 3.1 What exists

| work | measured | absolute? | adoptable per-band? | data released? |
|---|---|---|---|---|
| Kim, Lee & Hamins, *Fire Safety J.* 107 (2019) + NIST TN 1928 (open, read) | methanol pool energy balance; **χ_r = 0.19 ± 26 %** (0.30 m, Table 8), 0.22 ± 45 % (1 m); fire "entirely blue" | yes (radiometry) | no — total radiometry, no spectral resolution | report-level |
| Jin et al., *ACS Omega* 5:15922 (2020) (open) | laminar **methane** jet diffusion flames; absolute local OH*/CH* emitting rates (mol·m⁻³·s⁻¹), integrating-sphere calibrated, 2D fields | **yes** | **closest existing template** — absolute + spatial + geometry; but methane, and OH* (309 nm) is outside 380–780 | figures only |
| Fiala & Sattelmayer, *Exp. Fluids* 56:144 (2015) (**paywalled**; Fiala's TUM dissertation is open at mediaTUM) | spectrally+spatially resolved calibrated emission, H₂/O₂ jet flames, 1–40 bar | yes | wrong fuel; methodology + calibration chain fully documented in the open dissertation | dissertation open |
| Lauer & Sattelmayer (ASME 2010/2011) (**paywalled**; thesis open) | OH* vs HRR, turbulent premixed | no (relative) | no | — |
| Nori & Seitzman AIAA 2008-953 (open, read) | OH*/CH*/CO₂* chemiluminescence modeling vs φ, p, T | no (arb. units) | model framework only; useful fact: CO₂* contributes ~70 % of the nominal CH* 431±5 nm signal in atmospheric methane flames (Fig. 13, p. 11) | — |
| Slack & Grillo 1985 (via Nori Eq. 3, open) | CO+O→CO₂* absolute rate, 200–700 nm, 1300–2700 K | **yes** (rate model) | yes, as the CO₂* *source-term model* rather than a dataset | in-paper |
| DLR Stuttgart imaging work | OH*/CH* imaging | predominantly relative | no absolute per-band dataset surfaced | — |

**No absolutely calibrated, band-resolved, HRR-paired methanol-flame emission
dataset exists in the surveyed literature.** Methanol pool fires are
radiometrically characterized (NIST) but never spectrally resolved in the
visible.

### 3.2 Options with recommendation

- **Option A — leave methanol as preview** (recommended for this arc).
  Zero cost; honest; §7.0 already defines the preview labelling. Methanol's
  visible appearance is *entirely* chemiluminescence (the NIST reports call
  the fire "entirely blue", χ_r smallest of the alcohols), so `chem_model=none`
  can never be predictive for it and no negligibility route exists —
  methanol is predictive **only** with a full Q1 record, which does not
  exist to adopt.
- **Option B — commissioned measurement** (if methanol must go predictive).
  Setup: a radiometrically calibrated imaging spectrometer (integrating-sphere
  or standard-lamp calibration traceable to NIST/PTB), 380–780 nm with UV
  extension to ~300 nm for OH*, on a small laminar methanol burner or
  30 cm pool per the NIST TN 1928 configuration, with Abel inversion and
  paired HRR from fuel mass-loss (methanol's combustion efficiency ≈ 1
  makes HRR pairing clean — Kim/Lee/Hamins Fig. 5). The Jin et al. 2020 and
  Fiala-dissertation calibration chains are directly reusable templates.
  Rough scale: a university combustion-diagnostics lab could execute in
  **~3–6 months, order $30–80k** (equipment access dependent) — this is an
  informed engineering estimate, not a quote; treat as ±2× until a lab is
  actually approached.
- **Option C — author-contact parlay**: the Jin et al. group (methane) and
  the Lai et al. group (Sheffield) both have working calibrated rigs; asking
  either to run a methanol case may be far cheaper than a from-scratch
  commission. Worth one email if Option B is ever seriously considered.

**Recommendation: Option A now; revisit B/C only if a predictive methanol
case becomes a product requirement.** The §4.4 machinery still renders
methanol as *preview* with synthetic S_b fixtures.

---

## 4. Summary of owner actions

1. **Library pulls** (tier 1, cheap): Samaniego 1995; Markstein 1985 series;
   Koseki & Yumoto 1988; SFPE Ch. 36 wood tables; Thomsen 2017.
2. **Design ruling**: does a computed, uncertainty-propagated bound over
   published inputs qualify as §7.0's "pinned measurement"? (§1.6.2.)
3. **Record in §12**: leg (ii) of the §7.0 negligibility criterion is
   un-evaluable until §12 item 4 (radiance-gate dataset) closes — the tier-1
   records are blocked on Q4 regardless of leg (i).
4. **Send the Lai author email** (§2.4) if a methane record is wanted; decide
   whether a two-channel (CH*, C₂*) record + separately sourced CO₂* model
   satisfies Q1-methane.
5. **Methanol**: accept the preview recommendation or open a commissioning
   thread.
