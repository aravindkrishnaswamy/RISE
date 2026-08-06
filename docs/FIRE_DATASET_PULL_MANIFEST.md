# Fire dataset pull manifest — exact data needed per outstanding source

Companion to [FIRE_OPTICS_PRESET_V1.md](FIRE_OPTICS_PRESET_V1.md) and
[FIRE_CHEM_RECORDS_V1.md](FIRE_CHEM_RECORDS_V1.md). For each outstanding
pull: the *exact quantities* the records need, so an alternate source
carrying the same data closes the gate equally well. The paper is the
means; the data specification is the requirement. Compiled 2026-08-06.
Access results and extracted alternate-source data are recorded in
[FIRE_SOURCE_ALTERNATIVES_AUDIT_2026-08-06.md](FIRE_SOURCE_ALTERNATIVES_AUDIT_2026-08-06.md).

Ordering is by value. "Gate" names what the pull unblocks.

**Status after the 2026-08-06 audit** (details in the audit doc; extracted
data under [data/source_pulls/](data/source_pulls/)):

| pull | status |
|---|---|
| P1 Yazicioglu | **partial** — low-pressure acetylene axis adopted (Sunderland tables, 9.26–24.43 nm); pull stands for atmospheric CH₄/C₂H₄ + D_f/k_f |
| P2 Samaniego | **negative** — reclassified as model evidence (numerical investigation); no measured CO₂* W/HRR exists in this line |
| P3 Markstein/Klassen–Gore | **negative** — measurement absent in the audited classic series (emission starts 850/1200 nm); denominator needs new measurement or accepted derived value |
| P4 Köylü–Faeth | **closed** — NISTGCR 00-796 Fig. 3 digitization record (propane absent) |
| P5 Mulholland–Croarkin | **open** — needed detail inaccessible; do not encode k=2 on plausibility |
| P6 SFPE wood | **closed for large-tree domain** — NIST TN 2327r1 Douglas-fir record; FPA small-sample values still wanted |
| P7 Thomsen | **partial** — 2000–2200 K stays preview-only; DOI corrected (…2016.10.033); open predecessor adds an 84 W / χ_r≈6.6 % geometry-specific validation point |
| P8 Sumlin SI | **closed** — SI tables recovered from the arXiv PDF itself (PyMieScatt has no copy); 55 rows extracted |
| P9, P10 | skipped per ruling |

---

## P1. Yazicioglu et al. 2001 — young-soot morphology

Yazicioglu, Megaridis, Campbell, Lee, Choi, "Measurement of Fractal
Properties of Soot Agglomerates in Laminar Coflow Diffusion Flames Using
Thermophoretic Sampling in Conjunction with Transmission Electron
Microscopy and Image Processing", *Combust. Sci. Technol.* 171(1):71–87
(2001), DOI 10.1080/00102200108907859. **Paywalled (T&F).**
**Gate: predictive freeze of the hot-soot record** (d_p is currently
lead-only; N=30–80 is already verified).

**Exact data needed:**
- Tabulated (or formally digitizable) **primary-particle diameter d_p** for
  in-flame soot in laminar coflow diffusion flames, per fuel (CH₄, C₂H₄)
  and sampling height, with stated uncertainty. Target: confirm/replace the
  provisional 20–35 nm range.
- **N per aggregate** at the same positions (corroborates the verified
  30–80).
- **D_f and prefactor k_f with the exact convention stated**
  (N = k_f(R_g/d_p)^{D_f} vs radius-based — the diameter/radius trap).
- Flame conditions + sampling locations, to map onto the hot-soot
  certified domain.

**Acceptable alternates carrying the same data:**
- Megaridis & Dobbins, *Combust. Sci. Technol.* 71:95–109 (1990) — d_p/N vs
  height, ethylene laminar (paywalled; snippet-level d_p ≈ 10–40 nm).
- Sunderland, Köylü & Faeth, *Combust. Flame* 100:310–322 (1995) — open via
  the UMD Faeth corpus (wam.umd.edu/~pbs); d_p/N live in Fig. 3 →
  digitization-record route.
- Puri, Richardson, Santoro & Dobbins, *Combust. Flame* 92:320 (1993);
  Köylü, McEnally, Rosner & Pfefferle, *Combust. Flame* 110:494 (1997)
  (note the translucent-young-soot TEM/optical caveat).
- Group PhD theses (UIC/Megaridis, Penn State/Santoro) via ProQuest or
  university repositories — often reprint the tables.

---

## P2. Samaniego, Egolfopoulos & Bowman 1995 — CO₂* power per HRR

"CO2* Chemiluminescence in Premixed Flames", *Combust. Sci. Technol.*
109:183–203 (1995), DOI 10.1080/00102209508951901. **Paywalled (T&F).**
**Gate: tier-1 negligibility numerator** (the classic ~10⁻⁴-class anchor;
currently unverified).

**Exact data needed:**
- The **absolute CO₂* radiant power as a fraction of heat release**, with
  the flame conditions it applies to (fuel, φ, pressure), the spectral band
  over which the emission was integrated, and the stated uncertainty.
- Any per-condition variation (φ-dependence) usable for a domain statement.

**Acceptable alternates:**
- Samaniego's Stanford PhD thesis (ProQuest / Stanford library) — likely
  contains the same calibration and number.
- Any absolutely calibrated premixed-flame emission study reporting
  **radiant watts per unit HRR for the CO₂* continuum** (250–700 nm class):
  candidates include the DLR chemiluminescence-kinetics line (Kathrotia et
  al., calibrated low-pressure flame emission studies — access unverified)
  and quantitative CO₂* background measurements in the Nori–Seitzman
  lineage. The record needs the *measured power ratio*, not a kinetics fit.
- Failing both: the Slack & Grillo 1985 absolute rate model (already in
  hand via Nori & Seitzman Eq. 3) can *compute* the ratio for a documented
  flame — but per the 2026-08-05 §7.0 ruling, that route must then carry
  full model-form + domain-transfer uncertainties to qualify.

---

## P3. Markstein radiometry series — visible-band power of sooty flames

Markstein, "Relationship between smoke point and radiant emission from
buoyant turbulent and laminar diffusion flames", *Proc. Combust. Inst.*
20:1055–1061 (1985), DOI 10.1016/S0082-0784(85)80595-6, plus the related
FMRC (Factory Mutual) radiometry reports of the 1974–1985 era.
**Paywalled/print.**
**Gate: tier-1 negligibility denominator** — currently *no measured*
380–780 nm band power exists for any sooty flame; both reconstructions are
derived and disagree ~5×.

**Exact data needed (any one fuel suffices to start):**
- A **measured spectral radiance or band-resolved radiant power** of a
  sooty diffusion flame that **resolves wavelengths ≤ 780 nm**, with
  absolute calibration, geometry (view factor / solid angle), and a paired
  HRR or fuel mass-loss rate. Integration to 380–780 nm must be possible
  without model input.
- If the series turns out to be total-radiometer or IR-only (likely):
  record that finding — it converts the denominator gap from "unpulled"
  to "measurement does not exist", strengthening the case for a
  derived-record or new-measurement route.

**Acceptable alternates:**
- Any calibrated visible spectrometry of candle / pool / wood-crib flames
  with radiometric units and HRR pairing, from fire science, photometry, or
  remote-sensing literature (fire-radiative-power community works mostly
  MWIR — visible-resolved datasets are rare; check Klassen & Gore
  NIST-GCR-94-651 print copy for its shortest wavelength).
- A photometrically calibrated measurement (luminous flux/intensity
  distribution) of a flame with paired HRR *plus* its spectrum shape from
  any source — lets the 12.6 lm → watts conversion be done with recorded
  rather than assumed spectral weighting.

---

## P4. Köylü & Faeth 1994a — per-fuel overfire ρ_sa

"Optical Properties of Overfire Soot in Buoyant Turbulent Diffusion Flames
at Long Residence Times", *J. Heat Transfer* 116(1):152–159 (1994), DOI
10.1115/1.2910849. **Paywalled (ASME).**
**Gate: hardens the hot/cool-soot RDG-FA validation** (currently
abstract-level: ρ_sa 0.22–0.41 at 514.5 nm).

**Exact data needed:**
- The **per-fuel ρ_sa (scattering/absorption) table at 514.5 nm** with
  uncertainties, for acetylene, propylene, ethylene, propane.
- Dimensionless extinction / σ_s at 632.8 nm and 1152 nm per fuel, if
  tabulated.
- Their stated E(m)/F(m) assumptions used in reduction (needed to re-derive
  consistently under our adopted `mac_equivalent_E`).

**Acceptable alternates:**
- Krishnan & Faeth NISTGCR 00-796 (open, already OCR-read) — successor
  dataset; per-fuel/per-λ values live in Figs. 3/6 → digitization-record
  route is acceptable.
- Köylü's University of Michigan PhD thesis (ProQuest) — typically reprints
  the tables.
- Köylü & Faeth, *Combust. Flame* 89:140–156 (1992) (paywalled Elsevier;
  same campaign, morphology emphasis).

---

## P5. Mulholland & Croarkin 2000 body — anchor fine structure

"Specific extinction coefficient of flame generated smoke", *Fire Mater.*
24:227–230 (2000). **Paywalled (Wiley).**
**Gate: uncertainty semantics of the cool-carbon anchor** (value + 95 %
statement already abstract-verified; fine structure missing).

**Exact data needed:**
- The **list of the 7 studies and 29 fuels** and the per-fuel σ_s spread.
- The **ANOVA decomposition** (between-lab vs within) and the **coverage
  factor k** behind the ±1.1 (needed to propagate correctly against B&B's
  1σ ±1.2).
- Any wavelength-dependence statement (the 1060 nm companion 5.6 ± 0.69 is
  currently secondary via Ouf 2008).

**Acceptable alternates:**
- Mulholland's SFPE Handbook chapter "Smoke Production and Properties"
  (2nd/3rd ed.) — same synthesis, includes the smoldering ~4–5 m²/g values
  the secondary literature quotes; print/paywalled but widely held.
- The seven underlying studies directly (several are NIST: Mulholland et
  al.; Newman & Steciak 1987; Patterson et al.) via the NIST fire-research
  publication database — sufficient to reconstruct the spread, though not
  the paper's own ANOVA.

---

## P6. Khan & Tewarson SFPE Ch. 36 — wood radiative fraction

Khan, Tewarson et al., "Combustion Characteristics of Materials and
Generation of Fire Products", *SFPE Handbook* 5th ed., Ch. 36 (2016); or
Tewarson, Ch. 3-4, 3rd ed. (2002). **Print/paywalled.**
**Gate: the wood-flaming negligibility record's denominator inputs**
(currently snippet-level "χ_r ≈ 0.3").

**Exact data needed:**
- **χ_r (radiative fraction of chemical HRR) for wood-class fuels** (red
  oak, Douglas fir, pine) with the chemical/convective/radiative split and
  the measurement basis (fire propagation apparatus conditions).
- Smoke yields y_s for the same fuels (also feeds §3.4 fuel records).

**Acceptable alternates:**
- Tewarson's original FMRC technical reports (some circulate openly;
  FM Global archive).
- Brohez, Torero et al., *Fire Safety J.* 39:399 (2004) or Quintiere, *Fire
  Mater.* 41:131 (2016) (both paywalled but common in university access)
  — cone-calorimeter-derived χ_r for wood products.
- Any peer-reviewed measurement of wood-crib/slab radiative fraction with
  paired HRR and stated apparatus.

---

## P7. Thomsen et al. 2017 — candle soot temperature

Thomsen, Fuentes, Demarco, Volkwein, Consalvi, Reszka, "Soot measurements
in candle flames", *Exp. Therm. Fluid Sci.* 82:116–123 (2017), DOI
10.1016/j.expthermflusci.2016.10.033. **Paywalled (Elsevier).**
**Gate: the wax/candle negligibility record's denominator** (soot T
2000–2200 K currently abstract-verified only).

**Exact data needed:**
- **Measured soot temperature field or peak** in a paraffin candle flame
  (Modulated Absorption/Emission technique), with uncertainty and spatial
  extent — feeds the Planck visible-fraction computation with a recorded
  rather than assumed T.
- Soot volume fraction distribution, if reported (bonus: enables a
  first-principles visible-emission integral for the denominator).

**Acceptable alternates:**
- The co-authors' repositories (Universidad Técnica Federico Santa María /
  Aix-Marseille) or a thesis version of the same campaign.
- Any calibrated candle soot pyrometry: the Optik 2019 hyperspectral
  candle study (paywalled, same quantity), or Sunderland-group UMD candle
  papers (open host wam.umd.edu — check for temperature data).
- NIST candle work (Hamins 2005 quotes only Gaydon's 1400 °C figure —
  insufficient precision; do not substitute).

---

## P8. Sumlin et al. 2018 SI tables — condensed-organics red-end k

Sumlin et al., *JQSRT* 206:392–398 (2018) supplementary tables (the
per-wavelength n, κ at 375/405/532/1047 nm with uncertainties). Main text
open at arXiv:1712.05028; **the arXiv PDF includes SI Tables S1–S9.**
**Gate: firms the condensed-organics visible preset's red end** (currently
figure-level κ ≈ 0.002 plateau claim).

**Exact data needed:**
- The **tabulated retrieved n and κ per wavelength** with retrieval
  uncertainties, per fuel/packing-density condition.

**Acceptable alternates:**
- The arXiv SI is now the primary open route; Tables S1, S2, and S7 are
  transcribed in `docs/data/source_pulls/p8_sumlin_2018_complex_refractive_index.csv`.
- **PyMieScatt's GitHub repository** was checked and does not ship the dataset.
- Direct author request (Chakrabarty group, WUSTL — routinely shares).
- Any other multi-λ BrC complex-index retrieval on fresh biomass smoke
  covering ≥ 600 nm with stated uncertainties.

---

## P9. Koseki & Yumoto 1988 — heptane χ_r vs diameter (LOW VALUE)

*Fire Technol.* 24:33–47 (1988). **Paywalled (Springer).**
**Gate: none independently** — NISTIR 6546 (open, in hand) already
provides the fit χ_r = 0.35·e^(−0.05D) and the data figure.

**Exact data needed:** per-diameter heptane χ_r with uncertainties.
**Alternates:** digitize NISTIR 6546 Fig. 3 with a formal digitization
record (acceptable); Koseki's later open NRIFD publications repeat the
data. Pull only if the digitization route is rejected.

## P10. Turpin & Lim 2001 — organics density (LOW VALUE)

*Aerosol Sci. Technol.* 35:602–610. **Paywalled (T&F).**
**Gate: none independently** — the 1.2 g/cm³ is used via Reid II p. 803's
direct citation, which is already recorded as second-hand.

**Exact data needed:** the organic-aerosol density recommendation (~1.2
g/cm³) and its basis. **Alternates:** keep the Reid-mediated citation
(status quo, honest); or any open SOA-density review corroborating 1.2–1.4
g/cm³ for smoke-relevant organics. Pull only for provenance polish.

---

## Already resolved — no pull needed

- **Chang & Charalampopoulos 1990**: owner read the full text 2026-08-05
  (0.2–6.4 µm measured; 0.4–30 µm fit intent; ~5 %/6 % fit-vs-KK
  agreement). Optional residual: the paper's tabulated experimental n,k
  points (the 1.77−0.63i @540 fit-vs-table discrepancy) — only worth
  extracting if the ablation record is ever promoted.
- **Liu et al. 2020**: owner verified via the NRC-Canada repository
  (MAC(550) = 8.0 ± 0.7); adopted into `mac_equivalent_E`.
