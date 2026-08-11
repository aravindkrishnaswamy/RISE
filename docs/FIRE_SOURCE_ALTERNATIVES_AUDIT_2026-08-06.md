# Fire source alternatives audit — 2026-08-06

This is an access-and-substitutability audit for P1–P8 in
[`FIRE_DATASET_PULL_MANIFEST.md`](FIRE_DATASET_PULL_MANIFEST.md). It separates
an easier-to-access source from a source that actually carries the observable
needed by the design gate. P9–P10 were intentionally skipped as requested.

Downloaded publications were inspected outside the repository. Only extracted
numeric records and provenance are retained here; copyrighted source PDFs are
not vendored.

## Result at a glance

| Pull | Best accessible result | Status | Recommended design action |
|---|---|---|---|
| P1 young soot | Sunderland's open paper and dissertation provide exact axial `d_p`, sparse `N`, conditions, and uncertainties for four low-pressure acetylene flames | Useful, domain-limited replacement | Add as a separate acetylene/low-pressure morphology axis; do not use it to certify atmospheric CH4/C2H4 or Yazicioglu's `D_f/k_f` convention |
| P2 CO2* numerator | Samaniego 1995 and the accessible Stanford report are numerical/derived, not an absolute measured W/HRR record; DLR is calibrated spectroscopy but not a reported CO2*-power/HRR anchor | Qualifying alternative not found | Reclassify Samaniego as model evidence; use the Slack–Grillo/Jin derived route with full uncertainty or measure it |
| P3 visible denominator | Markstein is total-radiation only; Klassen–Gore emission begins at 850 nm (toluene) or 1200 nm (heptane), with only 632.8 nm absorption in the visible | Negative finding established | Treat the classic series as non-existent for direct 380–780 nm integration; retain derived denominator or commission a measurement |
| P4 overfire validation | NISTGCR 00-796 contains a successor per-fuel/per-wavelength `rho_sa` figure, <20% U95, exact structure tables, and the RDG-PFA reduction | Adoptable successor | Adopt the digitized 514.5 nm validation record; keep its overfire/long-residence domain and note that propane is absent |
| P5 extinction-anchor fine structure | Original reference list is recoverable, but the 29-fuel body, ANOVA, and coverage factor are not open | Still incomplete | Obtain the four-page Wiley body/SFPE chapter; do not silently infer the ANOVA or `k` |
| P6 wood denominator | NIST TN 2327r1 gives per-test Douglas-fir radiative fraction and smoke/CO/CO2 yields with k=2 uncertainties | Adoptable, different scale | Add a large dry-Douglas-fir/tree domain record; do not substitute it for FPA solid-wood values |
| P7 candle denominator | Correct Thomsen record exposes 2000–2200 K but not the field table/uncertainty; an open predecessor supplies an 84.04 W candle, 6.6% radiation, and peak soot near 8 ppm | Useful paired partial | Keep temperature preview-only; add predecessor denominator as a candle-specific check, not a universal candle constant |
| P8 organics red end | The arXiv PDF itself includes SI Tables S1–S9; PyMieScatt does not ship the dataset | Fully pulled through 1047 nm | Adopt the tabulated `n`, `kappa`, and 1-sigma retrieval spread; IR closure beyond 1047 nm remains unmeasured |

## P1 — young-soot morphology

### Accessible sources

- [Sunderland, Koeylue & Faeth 1995 open paper](https://terpconnect.umd.edu/~pbs/1995%20Sunderland%20et%20al.%20CNF.pdf)
- [Sunderland 1995 open dissertation](https://terpconnect.umd.edu/~pbs/Sunderland-dissertation-1995.pdf)
- [Yazicioglu 2001 METU repository record](https://open.metu.edu.tr/handle/11511/93703)

The dissertation's Appendix B contains a machine-readable Table B.1, so this
route is better than digitizing the journal figures. The extracted record is
[`p1_sunderland_1995_acetylene_structure.csv`](data/source_pulls/p1_sunderland_1995_acetylene_structure.csv).

What it establishes:

- Axial `d_p` is **9.26–24.43 nm**, not 20–35 nm, for the reported soot-bearing
  positions.
- The four flames are acetylene/air coflow diffusion flames at 0.125–0.250 atm,
  including one 59% C2H2/41% N2 burner stream; full flow, residence-time,
  temperature, soot-volume-fraction, and radiative-loss fields are retained in
  the CSV.
- The stated 95% uncertainty is less than 10% for `d_p` and less than 20% for
  mean `N`.
- `N` is tabulated only for selected positions in Flame 1: 44, 58, 119, 224,
  257, 373, and 384. This is not the requested same-position `N` grid for all
  fuels/flames.
- This source does not provide the requested fractal `D_f/k_f` fit. The open
  Yazicioglu repository metadata reports aggregate `D_f` of 1.65–1.75 and `k_f`
  endpoints of 6.75 (methane/air) and 8.47 (ethene/air), with uncertainties up
  to 6% and 22%, respectively, but exposes no bitstream carrying the per-height
  table or the convention details.

**Recommendation:** replace the provisional global 20–35 nm statement with a
domain-qualified structure: `9–25 nm` is directly tabulated for low-pressure
acetylene flames; `20–35 nm` remains an unverified lead for the intended
atmospheric/multi-fuel hot-soot domain. P1 is therefore partially closed, not a
substitute for table-verifying Yazicioglu.

## P2 — absolute CO2* numerator

### Checked alternatives

- [Samaniego, Egolfopoulos & Bowman 1995](https://doi.org/10.1080/00102209508951901)
- [Accessible Stanford CTR brief by Samaniego](https://web.stanford.edu/group/ctr/ResBriefs/1994/04_SAMANIEGO.pdf)
- [Kathrotia et al. DLR manuscript](https://elib.dlr.de/75430/1/Kathrotia2012apb571_manuscript.pdf)

The Samaniego paper is a numerical CO2* investigation, not an experimental
absolute-radiant-power publication. The Stanford brief reports computed
reaction-zone radiative-loss fractions of roughly 0.4–2.5 x 10^-3, but it does
not isolate a measured CO2* continuum power with a calibration band and paired
HRR. No matching "Samaniego Stanford thesis" was located; that lead appears to
conflate the paper with a different Stanford report.

The DLR manuscript is useful absolute/quantitative flame-spectroscopy evidence
for model validation, but it does not report an integrated CO2* continuum in
watts divided by HRR. It therefore cannot fill the requested tier-1 field.

**Recommendation:** explicitly label Samaniego as model evidence. The only
identified no-lab path remains the Slack–Grillo/Jin computed record, with
kinetics, calibrated-field, spectral-integration, domain-transfer, and
model-form uncertainties propagated. If §7.0 requires a directly measured
power ratio, P2 remains a laboratory gate.

## P3 — visible radiant-power denominator

### Checked sources

- [Markstein 1985 EPA HERO record](https://hero.epa.gov/reference/5863748/)
- [Klassen & Gore report, official NTIS record](https://ntrl.ntis.gov/NTRL/dashboard/searchResults/titleDetail/PB94193802.xhtml)
- [Rhee et al. 2026 preprint record](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=6110604)

Markstein reports total radiant power/radiative fraction, with values ranging
from 0.181 for methane to 0.429 for 1,3-butadiene, but no visible-resolved
spectrum.

The full 156-page Klassen–Gore report was inspected. Its three-wavelength probe
uses 632.8 nm for **absorption**, with emission channels at 900 +/- 50 nm and
1000 +/- 50 nm. The appendix emission spectra begin at **850 nm for toluene**
and **1200 nm for heptane**. Consequently, it contains no measured 380–780 nm
emission band that can be integrated for the denominator.

Rhee et al. is the closest newly found source: absolutely calibrated spectral
radiance from 400–2150 nm for sooting pressurized combustors. It is a valuable
spectral-shape benchmark, but the accessible record is line-of-sight radiance,
not a whole-flame visible power with geometry and paired HRR ready for a direct
W/HRR calculation.

**Recommendation:** record the classic Markstein/Klassen–Gore branch as an
audited negative, not "still unpulled." No qualifying accessible alternative
was found. Tier 1 still requires a new absolute visible-band measurement or a
formally accepted derived denominator.

## P4 — overfire scattering/absorption validation

### Accessible sources

- [NIST publication page for NISTGCR 00-796](https://www.nist.gov/publications/buoyant-turbulent-jets-and-flames-part-2-refractive-index-extinction-and-scattering)
- [NISTGCR 00-796 PDF](https://tsapps.nist.gov/publication/get_pdf.cfm?pub_id=916825)
- [Krishnan et al. 2002 visible/NIR validation paper](https://tsapps.nist.gov/publication/get_pdf.cfm?pub_id=861126)

The scanned report's Figure 3 was digitized at 514.5 nm. The values and exact
Table 1/Table 2 flame/structure fields are in
[`p4_nistgcr_00_796_overfire_soot.csv`](data/source_pulls/p4_nistgcr_00_796_overfire_soot.csv).

Approximate 514.5 nm `rho_sa` values are: acetylene 0.47, ethylene 0.33,
propylene 0.38, benzene 0.50, toluene 0.71, and n-heptane 0.33. A conservative
absolute digitization allowance of 0.03 is recorded separately from the
authors' experimental uncertainty, which is stated as less than 20% at 95%
confidence. Butadiene and cyclohexane were not reduced because complete
structure inputs were unavailable; propane was not tested in this successor
campaign.

Reduction assumptions recovered from the report:

- RDG-PFA: Rayleigh primary particles and Rayleigh–Debye–Gans aggregate
  scattering; spherical, monodisperse, point-contact primary particles.
- Lognormal `N`, with mass-fractal convention
  `N = k_f (R_g / d_p)^D_f`.
- `k_f = 8.5 +/- 0.5`; per-fuel `D_f` is 1.77–1.80 in the report table.
- `E(m)` is reduced from extinction minus scattering and `F(m)` from the
  angular scattering cross section. At 514.5 nm the fuel-averaged report table
  gives `E(m)=0.29 +/- 0.04`, `F(m)=0.27 +/- 0.06`, and `F/E=0.98`.
- Stated 95% uncertainties are 14–24% for `E(m)`, 19–26% for `F(m)`, and less
  than 5% for `D_f`; propane Rayleigh scattering supplies absolute calibration.

**Recommendation:** adopt this successor for overfire validation now. Keep it
separate from the original 1994 four-gas table because its fuels and mature
long-residence aggregates differ.

## P5 — Mulholland & Croarkin fine structure

### Accessible pieces

- [Mulholland & Croarkin 2000 Wiley record](https://onlinelibrary.wiley.com/doi/abs/10.1002/1099-1018%28200009/10%2924%3A5%3C227%3A%3AAID-FAM742%3E3.0.CO%3B2-9)
- [Open NIST smoke-aerosol review](https://tsapps.nist.gov/publication/get_pdf.cfm?pub_id=861149)

The reference body identifies the seven input-study families:

1. Mulholland, Henzel & Babrauskas (1989), Fire Safety Science.
2. Newman & Steciak (1987), *Combustion and Flame* 67:55.
3. Dobbins, Mulholland & Bryner (1994), *Atmospheric Environment* 28:889.
4. Patterson et al. (1991), *Atmospheric Environment* 25:2539.
5. Patterson et al., DNA-TR-90-98.
6. Choi et al. (1995), *Combustion and Flame* 102:161.
7. Colbeck et al. (1997), *Journal of Aerosol Science* 28:715.

The complete 29-fuel mapping, per-fuel spread, ANOVA components, and the stated
coverage factor behind 8.7 +/- 1.1 m2/g were not accessible. Treating the 95%
interval as `k=2` would be a plausible conventional inference, but it is not a
verified fact from the paper body and should not be encoded as such.

**Recommendation:** this is the one small paywalled item still worth obtaining
verbatim. Reconstructing 29 fuels from seven heterogeneous studies would be
more work and would still not reproduce the authors' ANOVA exactly.

## P6 — wood radiative fraction and smoke yield

### Accessible replacements

- [NIST TN 2327r1 publication page](https://www.nist.gov/publications/burning-characteristics-3-m-6-m-dry-douglas-fir-trees)
- [NIST TN 2327r1 PDF](https://tsapps.nist.gov/publication/get_pdf.cfm?pub_id=959226)
- [NIST TN 2314 PDF, broader moisture series](https://nvlpubs.nist.gov/nistpubs/TechnicalNotes/NIST.TN.2314.pdf)

NIST TN 2327r1 is a stronger open replacement than a secondary cone-derived
value for a **large dry-tree** domain. Oxygen-consumption calorimetry supplies
THR/HRR; two far-field gauges supply total radiated energy. The per-test pull is
[`p6_nist_tn2327r1_douglas_fir.csv`](data/source_pulls/p6_nist_tn2327r1_douglas_fir.csv).

- Per-tree `chi_rad`: 0.22–0.31; mean and sample standard deviation 0.30 +/-
  0.03. Average measurement uncertainty is 17% (expanded, k=2 context).
- Soot yields: 3.44–5.43 g/kg with per-test k=2 uncertainty.
- CO yields: 47.0–101.2 g/kg; CO2 yields: 1.21–1.93 kg/kg, also with k=2
  uncertainty.
- Domain: 4–6 m dry Douglas-fir, about 8–13% moisture, peak HRR 7–42 MW.
- Author caveat: the specimen obstructs some radiation, so the reported
  `chi_rad` does not represent every emitted ray.

This does not independently measure convective power; convection is available
only as a residual closure after chemical THR and radiative energy. It also
does not replace the FPA's small, externally heated solid-wood apparatus
domain.

**Recommendation:** add this as a separate vegetation/tree denominator record
and use 0.30 with its measured spread/uncertainty there. Continue seeking
Tewarson/FPA values only if the solid-wood material domain must be certified.

## P7 — candle soot temperature and denominator

### Accessible sources

- [Thomsen et al. 2017 publisher record, correct DOI](https://doi.org/10.1016/j.expthermflusci.2016.10.033)
- [Universidad Adolfo Ibanez repository record](https://pure.uai.cl/es/publications/soot-measurements-in-candle-flames/)
- [Open predecessor, Characterization of a Buoyant Candle Flame](https://www.combustion-institute.it/proceedings/MCS-7/papers/PFC/PFC-07.pdf)

The manifest DOI ending in `2016.11.006` was incorrect; the correct DOI ends in
`2016.10.033`. The accessible Thomsen record confirms modulated
absorption/emission measurements for wick diameters 2, 3, and 4 mm and wick
lengths 4–10 mm. Reported temperatures are 2000–2200 K and largely independent
of wick dimensions. The accessible record does not expose the field table or
temperature uncertainty, so it is not yet a pinned temperature input.

The open predecessor supplies a useful paired candle record:

- paraffin candle diameter 6 mm, wick diameter 0.8 mm, exposed wick 8–10 mm;
- mass-loss rate 1.9276 x 10^-3 g/s and heat of combustion 43.6 kJ/g, giving
  HRR = 84.04 W;
- integrated radiative fraction 6.91% and view-factor result 6.34%, summarized
  as about 6.6%;
- 660 nm extinction with `m = 1.57 - 0.56i`, with peak soot volume fraction
  near 8 ppm at 19 mm height;
- no soot-temperature measurement.

**Recommendation:** preserve Thomsen's 2000–2200 K only as a preview range
until its uncertainty/field data are obtained. Add the predecessor's 84 W,
6.6% candle denominator as a separate validation point. Its disagreement with
secondary 17% candle values is evidence of geometry sensitivity, not grounds
for averaging them.

## P8 — condensed-organics red end

### Accessible sources

- [Sumlin et al. arXiv record](https://arxiv.org/abs/1712.05028)
- [PyMieScatt repository](https://github.com/bsumlin/PyMieScatt)

The first alternate turned out not to carry the data: a repository-wide check
of PyMieScatt found code, examples, and documentation, but no Sumlin peat
refractive-index table. The better result is that the 25-page arXiv PDF already
appends Supporting Information Tables S1–S9.

Tables S1, S2, and S7 are transcribed in
[`p8_sumlin_2018_complex_refractive_index.csv`](data/source_pulls/p8_sumlin_2018_complex_refractive_index.csv):

- 55 condition/wavelength rows for Alaska (AK) and Indonesia (IN) peat;
- wavelengths 375, 405, 532, and 1047 nm (high-packing-density S7 stops at
  532 nm);
- `n`, `kappa`, and SSA with the authors' one-standard-deviation spreads;
- low fuel packing density about 0.03 g/cm3 and high AK packing density about
  0.06 g/cm3.

At low packing density, `kappa` falls from roughly 0.009–0.014 at 375 nm to
roughly 0.0016–0.0030 at 1047 nm. This directly supports the red/NIR plateau
through 1047 nm. It supplies no evidence beyond 1047 nm.

**Recommendation:** P8 is closed for the requested red end. Do not use this
pull to bless a Kirchhoff-emission extrapolation deeper into the IR; that
remains a model closure with explicit uncertainty or requires another dataset.
