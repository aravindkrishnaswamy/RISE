# Thin-Film Optical Constants — Substrate Metals & Heat-Tint Oxides

Spectral complex-IOR (`n` + i·`k`) data for the four substrate metals and their
heat-tint / anodize oxides used by RISE's thin-film interference material
(see [`docs/THIN_FILM_INTERFERENCE.md`](../../docs/THIN_FILM_INTERFERENCE.md),
Phase 1 piece B / §6 / §15).

These are **tabulated assets**, not code. Adding a new metal + oxide is pure
data: drop two more `*.n` / `*.k` files here and reference them from a
`scalar_painter { file … }` chunk. No source changes (the §1 / §6 generality
requirement).

## File format (STRICT — the loader is a bare `fscanf`)

The parser path for `scalar_painter { file … }`
([`AsciiSceneParser.cpp` ~L1249](../../src/Library/Parsers/AsciiSceneParser.cpp))
reads each file with a bare

```c
while (fscanf(f, "%lf %lf", &nm, &val) == 2) { … }
```

loop and builds a
[`PiecewiseLinearScalarPainter`](../../src/Library/Painters/PiecewiseLinearScalarPainter.h).
Consequences these files obey:

- **Two whitespace-separated numeric columns per line:** `wavelength_nm  value`.
- **Wavelength in NANOMETRES**, ascending. (refractiveindex.info publishes µm;
  every value here is ×1000 from the source.)
- **No header line, no comments, no commas, no unit suffixes** — any
  non-numeric token makes the first `fscanf` at that point return `!= 2` and the
  loop *silently truncates*. Pure numeric pairs only.
- `n` and `k` are **separate files** (`Ti.n`, `Ti.k`, …). refractiveindex.info
  ships a 3-column `wl n k` block (or two separate `wl n` / `wl k` blocks); each
  was split into two 2-column files here.
- Each file covers **≥ 380–780 nm**. `PiecewiseLinearScalarPainter` clamps to the
  endpoint value outside the sample range (`EvalAtNM` returns `front()`/`back()`
  for out-of-range `nm`), so where a published dataset starts above 380 nm or
  ends below 780 nm, an explicit boundary sample **holds the nearest measured
  value flat** to the boundary (a ≤ ~5 nm extrapolation that matches what the
  renderer would compute anyway). No interior value is invented by this padding.

## Metal ↔ oxide pairings

Heat tint / anodization grows the metal's own native oxide; each substrate gets
the oxide it actually forms (§6):

| Substrate file | Oxide file | Oxide phase | Optical character |
|---|---|---|---|
| `substrates/Ti.{n,k}`    | `oxides/TiO2.{n,k}`  | rutile TiO₂ (ordinary ray) | transparent (k ≈ 0) — canonical case |
| `substrates/Steel.{n,k}` | `oxides/Fe3O4.{n,k}` | magnetite Fe₃O₄            | **absorbing (k > 0)** — duller, browner ladder |
| `substrates/Ta.{n,k}`    | `oxides/Ta2O5.{n,k}` | amorphous Ta₂O₅           | transparent (k ≈ 0) — vivid clean anodize |
| `substrates/Nb.{n,k}`    | `oxides/Nb2O5.{n,k}` | Nb₂O₅                     | transparent (k ≈ 0) — anodized-jewellery palette |

Fe₃O₄'s non-zero `k` is deliberate coverage: it exercises the complex-film-index
path that the three transparent oxides barely touch (§6).

"Transparent" here means *optically* transparent in the visible, not literally
`k = 0` at every sample. Ta₂O₅ (Gao) and Nb₂O₅ (Lemarchand) carry a real,
measured **tiny** `k` in the deep blue (≤ 5×10⁻⁴ and ≤ 6×10⁻³ respectively,
decaying to exactly 0 in the green/red) — shipped **verbatim** rather than zeroed,
so the files stay faithful to the source. TiO₂ is the one genuinely `k = 0`
oxide here, because DeVore's rutile dataset is n-only (no measured k); rutile's
real absorption onset sits in the near-UV, outside the visible band we cover.

## Per-file provenance

All metal/oxide data is from the open **refractiveindex.info** database
(M. N. Polyanskiy, *Refractiveindex.info database of optical constants*,
Sci. Data **11**, 94 (2024)), except TiO₂ which is evaluated from its source
paper's own published dispersion formula (see below). Each entry below names the
exact dataset author/year, the underlying publication, and the RII page.

### Substrates

| File | Source dataset | Underlying reference | RII page |
|---|---|---|---|
| `Ti.n` / `Ti.k` | **Rakić 1998** (Lorentz–Drude model fit) | A. D. Rakić, A. B. Djurišić, J. M. Elazar, M. L. Majewski, *Optical properties of metallic films for vertical-cavity optoelectronic devices*, Appl. Opt. **37**, 5271–5283 (1998) | `main / Ti / Rakic-LD` — <https://refractiveindex.info/?shelf=main&book=Ti&page=Rakic-LD> |
| `Ta.n` / `Ta.k` | **Werner 2009 (DFT)** — theoretical DFT calculation | W. S. M. Werner, K. Glantschnig, C. Ambrosch-Draxl, *Optical constants and inelastic electron-scattering data for 17 elemental metals*, J. Phys. Chem. Ref. Data **38**, 1013–1092 (2009) | `main / Ta / Werner-DFT` — <https://refractiveindex.info/?shelf=main&book=Ta&page=Werner-DFT> |
| `Nb.n` / `Nb.k` | **Weaver 1973** (points digitised from the figure by RII) | J. H. Weaver, D. W. Lynch, C. G. Olson, *Optical properties of niobium from 0.1 to 36.4 eV*, Phys. Rev. B **7**, 4311–4318 (1973) | `main / Nb / Weaver` — <https://refractiveindex.info/?shelf=main&book=Nb&page=Weaver> |
| `Steel.n` / `Steel.k` | **Johnson & Christy 1974** (iron, Fe — see steel≈Fe note) | P. B. Johnson, R. W. Christy, *Optical constants of transition metals: Ti, V, Cr, Mn, Fe, Co, Ni, and Pd*, Phys. Rev. B **9**, 5056–5070 (1974) | `main / Fe / Johnson` — <https://refractiveindex.info/?shelf=main&book=Fe&page=Johnson> |

### Oxides

| File | Source dataset | Underlying reference | RII page |
|---|---|---|---|
| `TiO2.n` / `TiO2.k` | **Devore 1951** (rutile, ordinary ray) — evaluated from the published dispersion formula | J. R. DeVore, *Refractive indices of rutile and sphalerite*, J. Opt. Soc. Am. **41**, 416–419 (1951) | `main / TiO2 / Devore-o` — <https://refractiveindex.info/?shelf=main&book=TiO2&page=Devore-o> |
| `Fe3O4.n` / `Fe3O4.k` | **Querry 1985** (magnetite) | M. R. Querry, *Optical constants*, Contractor Report CRDC-CR-85034 (1985) | `main / Fe3O4 / Querry` — <https://refractiveindex.info/?shelf=main&book=Fe3O4&page=Querry> |
| `Ta2O5.n` / `Ta2O5.k` | **Gao 2012** | L. Gao, F. Lemarchand, M. Lequime, *Exploitation of multiple incidences spectrometric measurements for thin film reverse engineering*, Opt. Express **20**, 15734–15751 (2012) | `main / Ta2O5 / Gao` — <https://refractiveindex.info/?shelf=main&book=Ta2O5&page=Gao> |
| `Nb2O5.n` / `Nb2O5.k` | **Lemarchand 2013** (private communication; 500-nm monolayer on BK7, magnetron-sputtered) | F. Lemarchand, private communication (2013); measurement method per L. Gao, F. Lemarchand, M. Lequime, *Exploitation of multiple incidences spectrometric measurements for thin film reverse engineering*, Opt. Express **20**, 15734–15751 (2012) | `main / Nb2O5 / Lemarchand` — <https://refractiveindex.info/?shelf=main&book=Nb2O5&page=Lemarchand> |

### License

refractiveindex.info data is released into the **public domain (CC0 1.0)** by
the database maintainer; the underlying journal papers are the scientific
sources cited above and should be cited in any publication. The DeVore TiO₂
dispersion formula is a published closed form (DeVore 1951). No license
restriction applies to redistributing these tabulated constants alongside RISE.

## Caveats (per §15 — documented, NOT fudged in the optics)

These shift the thickness↔colour mapping. They are recorded here and absorbed by
Phase-3's heat→thickness growth-law calibration, never by tampering with the
optical constants:

1. **Steel ≈ Fe approximation.** Carbon/alloy steel has no single published
   visible n,k; we use elemental iron (Johnson & Christy 1974). Real steel's few-%
   alloy content shifts n,k slightly. The *substrate* metal matters far less to
   the heat-tint colour than the *oxide* film, so this is a small error.
2. **Steel's oxide is mixed and absorbing.** Temper colours are mainly magnetite
   (Fe₃O₄) with some haematite (Fe₂O₃). We model the dominant Fe₃O₄ phase
   (Querry 1985), which **absorbs** (k ≈ 0.03–0.15 across the visible) — this is
   why steel temper colours are duller / browner than the vivid Ti/Ta/Nb anodize
   ladders. A pure-Fe₂O₃ or graded mix would shift hues; not modelled here.
3. **Tantalum: Werner-DFT chosen over Werner-experimental.** refractiveindex.info
   carries two Werner 2009 Ta datasets. The *experimental* (REELS-derived) set
   gives k ≈ 3.4–6.4 across the visible; the *DFT* set gives k ≈ 1.9–3.4. An
   independent reference value (n ≈ 1.72, k ≈ 2.08 at 633 nm) agrees with the DFT
   set, not the experimental one — REELS optical constants for refractory metals
   are known to be perturbed by surface plasmon / oxide effects, inflating k. We
   therefore ship **Werner-DFT** as the more physically representative clean-bulk
   substrate. (Switching to the experimental set is a one-line data swap if a
   future use case wants it.)
4. **TiO₂ rutile: anatase / amorphous alternatives have lower n.** We use bulk
   rutile *ordinary ray* (DeVore 1951), the canonical high-index phase. Real
   Ti heat-tint film can be anatase or amorphous (n ≈ 2.1–2.5, lower than rutile's
   ≈ 2.5–2.9), which would shift the colour ladder. Rutile is the §6 canonical
   choice; the alternative phases are a documented future data swap.
5. **Rutile birefringence (ordinary ray used).** Rutile is uniaxial
   (n_o ≈ 2.6, n_e ≈ 2.9 mid-visible, Δn ≈ 0.3). Polycrystalline heat-tint film
   has no single optic axis; we use the **ordinary ray** as the representative
   index and document the approximation. The extraordinary ray (`Devore-e`) is a
   drop-in swap if a single-crystal case ever needs it.
6. **TiO₂ below 430 nm is a flat-hold, not measured.** DeVore's measured validity
   is 0.43–1.53 µm. We evaluate his dispersion formula
   `n² = 5.913 + 0.2441/(λ_µm² − 0.0803)` over 430–780 nm (it reproduces the RII
   `Devore-o` tabulated points to 3 decimals: 2.872 @ 430 nm, 2.659 @ 550 nm) and
   **hold the 430 nm value (n ≈ 2.872) flat down to 380 nm**. The bare formula
   over-predicts below its floor (n ≈ 3.12 @ 380 nm), exceeding the physical
   rutile n_o ceiling (~2.9); the flat-hold is the conservative, defensible
   choice for the deep-blue tail. TiO₂ `k` is taken as 0 throughout the visible
   (rutile is transparent; the band-edge absorption onset is in the near-UV and
   DeVore's n-only dataset carries no k).
7. **Nb / Ta substrate data is sparse** (Weaver: 17 points digitised from a
   figure; Werner-DFT: 8 points). `PiecewiseLinearScalarPainter` interpolates
   linearly between them — adequate for the smooth metal dispersion, but coarser
   than the dense Ti / oxide tables.
8. **Substrate microroughness desaturates real heat tint.** The optical constants
   are for ideal flat interfaces; real surfaces scatter and desaturate. Captured
   later by GGX roughness, not by these files.
9. **µm → nm conversion.** Every wavelength here is the refractiveindex.info µm
   value ×1000. (Off-by-1000 is the classic error; values were range-checked
   against known visible magnitudes per material.)

## Sanity ranges (gross magnitude checks — for catching unit/typo errors)

The shipped data falls in these published-physics bands across 380–780 nm
(these are bounds to catch hallucination/unit errors, not target values):

| Material | n range | k range | Character |
|---|---|---|---|
| Ti       | 0.91 – 2.47 | 2.37 – 3.48 | metal |
| Ta       | 0.97 – 3.22 | 1.90 – 4.47 | metal |
| Nb       | 2.48 – 2.93 | 2.52 – 3.08 | metal |
| Steel(Fe)| 2.12 – 2.95 | 2.50 – 3.39 | metal |
| TiO₂     | 2.52 – 2.87 | 0           | transparent oxide |
| Ta₂O₅    | 2.11 – 2.28 | ≤ 5×10⁻⁴ ≈ 0 | transparent oxide |
| Nb₂O₅    | 2.28 – 2.64 | ≤ 6×10⁻³ ≈ 0 | transparent oxide |
| Fe₃O₄    | 2.28 – 2.46 | 0.026 – 0.148 | **absorbing** oxide |
