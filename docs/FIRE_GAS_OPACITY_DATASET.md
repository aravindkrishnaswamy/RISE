# Fire gas-opacity dataset — HITEMP-derived CO₂/H₂O Planck means (v1)

**What this is.** The simulator-only CO₂/H₂O radiative-cooling data required by
[FIRE_SMOKE_DESIGN.md](FIRE_SMOKE_DESIGN.md) §3.5 and §12 item 5, derived from
the HITEMP line lists. This document records exactly what was done, so that
credit goes to HITRAN*online* and the specific publications, and so the
dataset can be regenerated and audited from first sources.

**Status: DRAFT dataset for owner review — not yet an adopted §8 record.** The
certified record (RISE-CBOR64-v1 encoding, record ID, certified domains,
derivative enclosures, out-of-domain rejection) is produced from this data by
the fire-simulation record generator; this document and the tables under
[data/gas_opacity/](data/gas_opacity/) are its input and its provenance trail.

---

## 1. Credit and required citations

This dataset is **derived work computed from** the following. Any use,
publication, or redistribution of the derived tables must carry these
citations.

**Line lists — HITEMP, obtained from HITRAN*online* (hitran.org):**

- **HITEMP-2010 (H₂O):** Rothman, L.S., Gordon, I.E., Barber, R.J.,
  Dothe, H., Gamache, R.R., Goldman, A., Perevalov, V.I., Tashkun, S.A.,
  Tennyson, J. *HITEMP, the high-temperature molecular spectroscopic
  database.* J. Quant. Spectrosc. Radiat. Transfer **111**(15):2139–2150
  (2010). doi:10.1016/j.jqsrt.2010.05.001
- **HITEMP CO₂ (2024 edition)** as distributed by HITRAN*online*; cite the
  HITEMP article above together with the CO₂-2024 edition article named on
  the [HITEMP page](https://hitran.org/hitemp/), and — per HITRAN's citation
  policy — the **original data sources referenced for the individual
  parameters**.
- HITRAN's [Citation Policy](https://hitran.org/citepolicy/) states: *"Please
  cite the article describing the database edition that you are using"* and
  *"We strongly encourage users of HITRAN to cite the original sources."*

**Partition sums — TIPS-2021 (MIT licensed):**

- Gamache, R.R., Vispoel, B., Rey, M., Nikitin, A., Tyuterev, V., Egorov, O.,
  Gordon, I.E., Boudon, V. *Total internal partition sums for the HITRAN2020
  database.* J. Quant. Spectrosc. Radiat. Transfer **271**:107713 (2021).
  doi:10.1016/j.jqsrt.2021.107713 — data doi:10.5281/zenodo.4708099.
- MIT licence, © 2021 Robert Gamache. The licence notice travels with any
  bundled TIPS files.

**Validation references (not inputs — used only to check our output):**

- Grosshandler, W.L. *RADCAL: A Narrow-Band Model for Radiation Calculations
  in a Combustion Environment.* NIST Technical Note 1402 (1993) — via the
  TNF Workshop radiation page's κ_P polynomial fits.
- Barlow, R.S., Karpetis, A.N., Frank, J.H., Chen, J.-Y. *Combust. Flame*
  **127**:2102–2118 (2001).
- Chmielewski, M., Gieras, M. *J. Power Technologies* **95**(2):97–104 (2015)
  — independent HITEMP-2010 line-by-line Planck-mean fits.
- Zheng et al. *ES Energy & Environment* **17**:33–43 (2022),
  doi:10.30919/esee8c635 — independent CDSD-4000 line-by-line CO₂ Planck
  means.

**Licensing note.** TIPS-2021 is MIT, so derived products are freely
publishable with the notice retained. HITRAN/HITEMP publish **no licence
text** — only a citation policy — and are free to obtain (a free account is
required to download). Accordingly this repository ships **derived tables
only**; the raw `.par` line-list bytes are **not** redistributed here. They
are pinned by SHA-256 in
[data/gas_opacity/hitemp_sources_v1.json](data/gas_opacity/hitemp_sources_v1.json)
and re-obtainable from HITRAN*online* by any account holder. Publishing
derived opacity products from HITEMP with citation is established practice
(cf. the ExoMolOP opacity release).

---

## 2. What the simulator actually needs, and why the reduction is not lossy for it

§3.5's radiative sink is an **optically-thin escape-factor** exchange:

> e(x) = 4σ [ κ_P(T; κ_λ(x))·T⁴ − κ_P(T∞; κ_λ(x))·T∞⁴ ]

so the only gas quantity consumed is the **Planck-mean absorption
coefficient, evaluated at two radiation temperatures from the same local
spectrum**. A Planck mean integrates κ_λ against a Planck function whose
scale of variation is ~10³ cm⁻¹, while individual line widths are ~10⁻¹
cm⁻¹. Therefore:

- **Line shape does not enter.** γ_air, γ_self, n_air and δ_air — the
  majority of every 160-byte HITRAN record — are irrelevant to this
  quantity, because ∫κ_ν dν over a line equals the line strength regardless
  of broadening. They are discarded.
- **Pressure does not enter the cross-section.** The stored quantity is a
  per-molecule cross-section, so it is valid at any pressure in this limit;
  pressure enters only through number density when the user multiplies by
  partial pressure.

⚠ **Applicability limit, stated plainly:** this dataset is valid for
**optically-thin Planck-mean cooling only**. It must **not** be used for
line-of-sight transmission, band-resolved radiative transfer, or any
optically-thick calculation, where line shapes and self-absorption dominate.
§3.5 uses it only as a simulator-side cooling closure — gas bands are never
exported to the renderer — and §3.5's own per-column optical-thickness
monitor (κ_P·L) is what measures whether the thin assumption holds.

---

## 3. Method

### 3.1 Temperature-independent spectral-energy histogram

HITRAN's temperature scaling of line intensity,

> S(T) = S₂₉₆ · [Q(296)/Q(T)] · exp(−c₂E″/T)/exp(−c₂E″/296)
>              · [1−exp(−c₂ν/T)]/[1−exp(−c₂ν/296)]

factors into a temperature-**independent** per-line amplitude

> A = S₂₉₆ / { exp(−c₂E″/296) · [1−exp(−c₂ν/296)] }

multiplied by factors depending only on (E″, ν, T). Binning lines in
(ν, E″) and accumulating **ΣA, ΣA·E″, ΣA·E″², ΣA·ν** per cell therefore
reproduces Σ S(T) at **any** temperature, with no temperature grid baked in.
Reconstruction uses each cell's exact ΣA-weighted mean E″ plus the
second-moment correction ½(c₂/T)²·Var(E″), so binning error is third order.

This form is also **analytically differentiable in T**, which is what §3.5's
certified F′(T) interval enclosure requires — a value-only table would not
close the backward-Euler solver.

> **No cutoff is applied to S₂₉₆, ever.** HITEMP deliberately contains lines
> that are astronomically weak at 296 K and dominant at flame temperatures:
> the first record of the H₂O 50–150 cm⁻¹ segment has E″ = 17632 cm⁻¹ and
> S₂₉₆ = 8.95×10⁻⁵⁹, gaining ~32 orders of magnitude by 2000 K. Pruning is
> applied **only** to reconstructed S(T) over the certified domain, and the
> realized worst-case error is measured, not assumed.

### 3.2 Planck-mean reconstruction

With all physical constants except c₂ cancelling between numerator and
normalizer:

> κ_P(T_gas, T_r)/n = Σ_cells S_cell(T_gas)·ν̄³/[exp(c₂ν̄/T_r)−1]
>                     ÷ { (T_r/c₂)⁴ · π⁴/15 }

giving a per-molecule cross-section in cm²/molecule; the tables also carry
the 1 atm partial-pressure form in m⁻¹·atm⁻¹ for comparison with published
fits. Partition sums Q(T) are per-isotopologue TIPS-2021, linearly
interpolated on its 1 K grid (matching the reference implementation).
HITRAN intensities already include terrestrial isotopologue abundance, so
summing isotopologues yields natural-abundance gas absorption.

### 3.3 Tools (committed, and the regeneration path)

| tool | role |
|---|---|
| [tools/hitemp_reduce.cpp](../tools/hitemp_reduce.cpp) | streams `.par` records → spectral-energy histogram |
| [tools/hitemp_planck_mean.cpp](../tools/hitemp_planck_mean.cpp) | histogram → κ_P(T_gas,T_r) table; prunes with measured error |
| [tools/hitemp_visible_bound.py](../tools/hitemp_visible_bound.py) | 380–780 nm upper bound from the **un-pruned** histogram |

Regeneration: obtain the pinned files from HITRAN*online*, verify SHA-256
against `hitemp_sources_v1.json`, then

```
clang++ -O3 -std=c++17 -o hitemp_reduce tools/hitemp_reduce.cpp
bzcat 02_HITEMP2024.par.bz2 | ./hitemp_reduce --mol 2 --out co2.hist
for z in H2O/*.zip; do unzip -p "$z"; done | ./hitemp_reduce --mol 1 --out h2o.hist
```

---

## 4. Results

### 4.1 Ingest integrity

| | H₂O | CO₂ |
|---|---|---|
| source edition | HITEMP-2010 (34 zip segments) | HITEMP-2024 (single bz2) |
| records read | 114,241,164 | 326,260,084 |
| records kept | 114,241,164 (100%) | 326,260,084 (100%) |
| parse failures | 0 | 0 |
| ν range (cm⁻¹) | 0.0001 – 29,661.8 | ≤ 17,696.93 |
| E″ range (cm⁻¹) | 0 – 29,995.0 | 0 – 39,987.3 |
| isotopologues | 6 | 12 |
| histogram cells | 306,635 | 1,468,935 |

**Both record counts reproduce HITRAN*online*'s stated line counts exactly**
(114,241,164 and 326,260,084), with zero parse failures across all
440,501,248 records — a free end-to-end integrity check on decompression,
record framing and field parsing. CO₂'s observed ν_max of 17,696.93 cm⁻¹
confirms the published ≈565 nm ceiling of the 2024 edition.

### 4.2 Validation against independent published Planck means

Computed diagonal κ_P (T_r = T_gas), m⁻¹·atm⁻¹, against the RADCAL-derived
TNF fit and the independent HITEMP-2010 LBL fit of Chmielewski & Gieras:

| T (K) | **this work (H₂O)** | TNF (RADCAL) | HITEMP LBL (C&G) |
|---|---|---|---|
| 300 | 52.29 | 52.997 | 52.972 |
| 600 | 13.75 | 14.131 | 13.466 |
| 750 | 9.044 | 9.524 | 8.948 |
| 1500 | 2.222 | 2.417 | 2.196 |
| 1800 | 1.459 | 1.585 | 1.501 |

Agreement with the independent line-by-line fit is **1–3% across
300–1800 K**, validating parsing, TIPS scaling, Planck weighting and units
end to end. Comparisons are capped at ~2000 K because both published fits
degrade at their 2500 K endpoints (the C&G Gaussian form falls to 0.238 at
2500 K against TNF's 0.647; the directly computed value here is the more
trustworthy one in that region).

**CO₂** (m⁻¹·atm⁻¹), against both independent line-by-line references and
the RADCAL-derived fit:

| T (K) | **this work (CO₂)** | HITEMP LBL (C&G) | CDSD-4000 LBL | TNF (RADCAL) |
|---|---|---|---|---|
| 300 | 26.31 | 26.344 (−0.1%) | 25.918 | 24.292 |
| 750 | 32.70 | 31.687 (+3.2%) | 31.726 | 36.701 (−10.9%) |
| 1500 | 10.12 | 10.038 (+0.8%) | 9.733 | 12.284 (−17.6%) |
| ~2000 | ≈4.70 | 4.709 (−0.2%) | 4.397 | 5.542 (−15.2%) |

This is the decisive check. Two mutually independent line-by-line
calculations (HITEMP and CDSD-4000) agree with each other on CO₂ to 1–3%
over 300–2000 K while sitting **systematically 15–21% below the RADCAL/TNF
fit above ~600 K**. This work reproduces *both* facts: it lands on the
independent HITEMP LBL reference to within 0.1–3.2%, and it is low against
TNF by exactly the expected margin. RADCAL's narrow-band CO₂ is the
outlier; the offset is the correct line-by-line answer, not a pipeline
error.

### 4.3 380–780 nm upper bound (§12 item 5)

Computed from the **un-pruned** histogram, since pruning only removes
absorption and would understate a bound:

| | worst visible fraction of κ_P | max absolute visible κ_P |
|---|---|---|
| H₂O | 1.694×10⁻⁵ (at T_gas = T_r = 2500 K) | 6.973×10⁻⁵ m⁻¹·atm⁻¹ |
| CO₂ | 1.522×10⁻¹⁰ (at T_gas = T_r = 2500 K) | 1.424×10⁻⁹ m⁻¹·atm⁻¹ |

Both worst cases occur at the hottest corner of the domain, as expected.
Water vapour's visible-band absorption is ~4 orders of magnitude below
in-flame soot extinction and CO₂'s is ~9 orders below, satisfying the
design's requirement that the gas table be bounded negligible in the
renderer's 380–780 nm band by a wide margin.

**Caveat that must travel with the CO₂ number:** the HITEMP-2024 CO₂ line
list ends at 17,696.93 cm⁻¹ (≈565 nm), so the 380–565 nm portion of the
band contains **no tabulated CO₂ lines at all**. The CO₂ figure above is
therefore a bound over 565–780 nm only, and the record must carry the
*physical* negligibility argument for the remainder (CO₂ has no electronic
absorption bands in the visible) rather than implying measured coverage
there. This is the same gap flagged in
[FIRE_SIM_DATASET_MANIFEST.md](FIRE_SIM_DATASET_MANIFEST.md) item 5.

### 4.4 Pruning

Pruning is on reconstructed strength over the certified domain
(300–2500 K), with the realized worst-case relative κ_P error measured
across the whole (T_gas, T_r) grid and recorded in the output header.

| | cells before | cells after | worst relative κ_P error |
|---|---|---|---|
| H₂O | 306,635 | 132,512 | 2.23×10⁻³ |
| CO₂ | 1,468,935 | 75,437 | 1.81×10⁻³ |

CO₂ prunes far harder than H₂O (19.5× versus 2.3×) because its absorption
is concentrated into a few strong bands, while H₂O's is spread across a
dense quasi-continuum.

### 4.5 Delivered artifacts and sizes

| file | size | role |
|---|---|---|
| `data/gas_opacity/hitemp_planck_mean_h2o_v1.txt` | 94 KB | **operational** — κ_P(T_gas,T_r), what §3.5 calls |
| `data/gas_opacity/hitemp_planck_mean_co2_v1.txt` | 94 KB | **operational** |
| `data/gas_opacity/hitemp_spectral_basis_h2o_v1.hist.gz` | 3.4 MB | pruned spectral basis — temperature-continuous, analytically differentiable |
| `data/gas_opacity/hitemp_spectral_basis_co2_v1.hist.gz` | 1.9 MB | as above |
| `data/gas_opacity/hitemp_sources_v1.json` | small | pinned source digests, citations, licensing |

**9.4 GB of source line lists → 188 KB of operational data** (5.5 MB
including the differentiable spectral basis), with the originals staying
out of the repository behind pinned digests and the committed tools.

---

## 5. Certified domains and known limits

- **Temperature domain:** 300–2500 K for both gas and radiation temperature.
  Enforced, never extrapolated. TIPS-2021 covers 1–5000 K for H₂O and most
  CO₂ isotopologues, but **CO₂ isotopologues 3, 4, 5, 6, 8 and 11 stop at
  3500 K** — outside this dataset's ceiling, but a hard guard is required if
  the domain is ever raised.
- **Pressure:** the stored cross-section is pressure-independent in the
  optically-thin Planck-mean limit; the m⁻¹·atm⁻¹ columns assume 1 atm
  partial pressure.
- **Mixtures:** Planck means are linear in κ_λ, so a CO₂/H₂O mixture is the
  partial-density-weighted sum of the per-species tables. No band-overlap
  convention is needed for this quantity — overlap matters only for
  emissivity-type calculations, which this dataset does not support.
- **Spectral coverage:** H₂O to 29,662 cm⁻¹; CO₂ to 17,697 cm⁻¹ (≈565 nm).
- **Not valid for** transmission, optically-thick transfer, or band-resolved
  radiative transfer (§2 above).
