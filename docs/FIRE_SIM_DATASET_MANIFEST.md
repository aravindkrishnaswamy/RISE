# Fire simulator dataset manifest — §12 items 3–7 (pre-Phase-C verification)

Companion to [FIRE_DATASET_PULL_MANIFEST.md](FIRE_DATASET_PULL_MANIFEST.md)
(the Q1/Q2 manifest). For each of the five data records Phase C's predictive
label needs: the binding criteria (extracted from the design by the survey
agents and verified against the cited lines), the verdict, verified sources
with access status, and gaps named precisely. Compiled 2026-08-08, design
r49. **Engineering never waits on this list** (§7.0); these records gate the
predictive claim only. Survey discipline as before: per-fact provenance,
paywalls flagged and never silently substituted, primary-vs-secondary
labeled.

## Summary table

| item | verdict | path |
|---|---|---|
| 3 — y_cond calibration | **NOT closeable from literature** — the required test series has never been run | NIST NFRL collaboration (~90 % of rig exists); FIREX 2016 as interim *bounding* record |
| 4 — absolute radiance gate | **NOT closeable as-released, any fuel** | methane-first via extended Lai author contact (+ a design ruling); one afternoon-scale candle campaign closes wax + the chem denominator together |
| 5 — CO₂/H₂O opacity | **closeable by building** on open data | HITEMP LBL + RISE-owned generation code; EM2C-SNB (CC-BY) as the independent reference |
| 6 — thermochemistry | **closeable, almost fully open** | NASA Glenn (Apache-2.0) + JANAF + ThermoML; two estimation-route fields; Burcat license flag |
| 7 — transport closure | **fully closeable from open sources** | GRI-Mech/CHEMKIN-manual + NASA TM-4513 + Vreman + FDS source; three declared-constant rationale flags |

---

## Item 3 — y_cond calibration dataset

**Criteria** (§3.4 lines ~734–755; §12 item 3): named fields (crib geometry,
moisture, MLR/HRR, sampling height, dilution ratio, sampling T, residence
time, PM size cut, analytical method); **paired particle+gas organic yield
over the SAME exhaust stream** (thermo-optical OC/EC filter + downstream
sorbent gas condensable); OC→surrogate via levoglucosan-class carbon
fraction 0.4444; one fitted reference crib; an **independently measured
second scale** plus the D*/δx ∈ {6,10,14} cross-prediction (±25 %);
archived raw snapshot/hash.

**Verdict: does not exist.** Fire-calorimetry datasets and
aerosol-speciation datasets have never overlapped in one test series:

- **Calorimetry side** (crib geometry, HRR+MLR, paired gases, ≥2 scales,
  open raw CSVs; but total soot only — no OC/EC, no sorbent, no dilution
  metadata): NIST FCD Test13/Test15 wood cribs (open; soot 0.00195±0.00031
  kg/kg k=2); NIST TN 2327r1 Douglas-fir trees (open, already in the Q1/Q2
  records); Davis et al. 2025 *ACS EST Air* (PMC, open) — **two crib
  scales** with isokinetic gravimetric smoke, structurally the closest
  apparatus, but mixed gypsum/wood fuel and no speciation.
- **Speciation side** (thermo-optical OC/EC, gas pairing; but no cribs, no
  O₂-consumption HRR, one scale): Chen et al. 2007 (open USDA PDF — the
  EC/PM≈0.80 anchor; quartz-filter OC artifact acknowledged, no sorbent);
  **FIREX FireLab 2016** (open per-burn archive; OC/EC + QBT backup filters
  + CO/CO₂ + SVOC CIMS, conifer fuels — closest single dataset); Schauer
  2001 (**paywalled**; the only study measuring the exact §3.4 pairing incl.
  quantified levoglucosan, on fireplace cordwood).
- Stitching across the two sides is exactly what the same-exhaust-stream
  clause forbids.

**Path**: (a) **NIST NFRL collaboration** — the Davis 2025 rig plus four
deltas (pure-wood cribs; quartz filters + thermo-optical OC/EC; a sorbent
train; dilution metadata) with FCD-style raw CSV publication; a
commissioned-measurement-by-collaboration outcome. (b) Meanwhile, FIREX
2016 can support a **bounding** record — which likely bounds y_cond as
**non-negligible** for wood, so the y_cond=0 escape hatch stays closed too.
(c) Wax/paraffin y_cond: unmeasured at any scale, anywhere.

---

## Item 4 — absolute flame spectral-radiance dataset

**Criteria** (§3.8 gate (a); §12 item 4): at least one absolute L_λ
comparison in W·m⁻²·sr⁻¹·nm⁻¹ at a stated wavelength/tolerance, read as raw
pre-exposure NM radiance, **through the full sim→grid→renderer pipeline**;
record pins fuel, geometry, view/slit/solid-angle calibration,
atmospheric/path correction, wavelength+bandwidth, raw-unit conversion,
uncertainty, tolerance. Also gates §7.0 chem leg (ii).

**Load-bearing reframing** (survey finding): the gate needs
**line-of-sight** L_λ — the renderer reproduces the LOS integral natively —
so Abel inversion is *not* required for this role. This improves Lai 2025's
standing relative to its chem-record scorecard.

**Verdict: no published dataset closes it as-released for any target
fuel.** Best per fuel:

- **Methane (the realistic first closure)**: Lai et al. 2025 (open accepted
  MS) — in-band 418–708 nm, full calibration chain, LOS maps sufficient;
  fails on: instrument-étendue watts not converted to source radiance
  (parameters characterized, not all published), no end-to-end uncertainty
  budget, figures-only. **New conflict requiring an owner ruling**: the
  flame is a *premixed* Bunsen, and §3.3's mixing-limited model cannot
  simulate a premixed cone while gate (a) runs through the full sim
  pipeline. Closure = extend the drafted author email with (i) an
  end-to-end uncertainty budget ask and (ii) **any laminar diffusion-flame
  case on the same rig**; else the owner must amend the gate (renderer-only
  leg on prescribed fields) or reject. Liu et al. 2020 (open) is the backup
  author-contact candidate (strongest calibration chain, sim-compatible
  diffusion geometry; released as band totals, not L_λ).
- **Ethylene** (non-target): Snelling et al. 2002 (**paywalled**) is the
  methodological gold standard (500–945 nm absolute LOS) — adoptable only
  via a design decision to add an ethylene fuel record (heavy).
- **Wax/heptane/wood/methanol**: nothing exists (consistent with the P3
  audit — physically the same missing measurement as the chem
  denominator). **Cheapest credible closure: a radiometrically calibrated
  spectroradiometer on the Hamins candle configuration — one instrument,
  one traceable lamp, one afternoon-scale campaign — simultaneously closes
  item 4 for wax and the §7.0 chem-denominator gap.**
- Loose end for owner library/access: SSRN 6110604 (calibrated LOS
  400–2150 nm, pressurized combustor — right observable, almost certainly
  wrong configuration; 403 on fetch, verify before dismissing). Both Optik
  candle hyperspectral papers are paywalled with unverified calibration
  status — do not cite as calibrated without full-text verification.

---

## Item 5 — simulator-only CO₂/H₂O opacity record

**Criteria** (§3.5; §12 item 5): spectroscopic source AND table-generation
code; wavelength/T/composition/pressure domains; broadening/band-overlap
convention; interpolation with the §3.5 derivative enclosures; independent
absolute cooling reference + tolerance; deterministic bytes/hash;
380–780 nm upper-bound test; certified closed domains; OOD RED gate.
Structural notes from the survey: Planck means are linear in κ_λ (overlap
convention moot for the cooling quantity itself), and §3.5's two-radiation-
temperature term needs spectral κ_λ or a two-argument table — **a
single-argument κ_P(T) fit cannot populate the record** (disqualifies the
TNF fits and WSGG as primaries on structure alone).

**Verdict: closeable by building.** Recommended primary: **HITEMP
regeneration** (H₂O-2010, 114 M lines to 30,000 cm⁻¹; CO₂-2024, 326 M
lines to 17,697 cm⁻¹; free, account-gated, hashable `.par` bytes) with
RISE-owned LBL table-generation code (Voigt integrator → κ_λ → two-argument
Planck means), which the record wants archived anyway. Independent V5
cooling reference: the **CC-BY EM2C-SNB total-emissivity dataset**
(Mendeley, DOI 10.17632/x5wjzk6sjs.1; 300–2900 K, 94,500 values;
independent lineage). Spot-checks: RADCAL (NIST public-domain repo, pinned
commit; <5 % emissivity error per TN 2064; 1973-era band data with an
**unpinned T domain — verify from TN 1402 at archive time**) and the TNF
κ_P fits (open; diagonal-only). Initial V5 tolerance ~5 %, to be reset from
the first HITEMP-vs-EM2C comparison, not assumed.

**Owner resolution (2026-08-11)**: HITEMP requires citation but imposes no
line-data redistribution license.  RISE nevertheless keeps a strict byte
boundary: owner-downloaded `.par` files are never committed; a fetch/verify
step pins their SHA-256 identities, and only the generator plus derived tables
are repository artifacts.  CO₂ lines end at ~565 nm, so the 380–565 nm remainder
needs a documented physical-negligibility statement (no CO₂ electronic
bands in the visible) as a certified record claim; Soufiani–Taine 1997 and
all WSGG coefficient papers are paywalled (not needed under this plan).

**Generator increment (2026-08-11)**: the committed reference toolchain is
`tools/fire_gas_opacity.py`, `tools/generate_fire_gas_opacity_record.py`, and
`tools/fetch_verify_hitemp_inputs.py`.  It parses the HITRAN 160-character
format, temperature-scales line strengths with separately hash-pinned HITRAN
partition-sum files, evaluates air/self-broadened Voigt profiles at 1 atm,
and deposits each line into declared finite-volume wavenumber bins while
preserving its integrated area.  It derives the required two-temperature
Planck-mean tensors across a self-broadening composition axis and stores exact
per-cell multilinear partial-derivative enclosures.  Each species archive is
streamed once for all table states. Production generation is required to use
the compiled streaming accumulator in `tools/fire_gas_opacity_native.cpp`;
the Python implementation is the independently compared reference used by the
synthetic fixture. Both finite-bin and expanded-wing Planck-mean convergence
limits are manifest gates, and a sum-of-per-line conservative profile bound
gates gas absorption over 380–780 nm rather than mistaking a bin average for a
pointwise upper bound. The owner manifest remains explicitly pending in
`docs/data/source_pulls/hitemp_owner_inputs_v1.pending.json`; it cannot
generate a physical table until the exact H₂O-2010 inventory, the
`02_HITEMP2024.par.bz2` digest, all required isotopologue masses/partition
sums, and the original-source citations from both distributions are pinned.
The committed `docs/data/fire_gas_opacity_synthetic_v1.json` is labeled
`synthetic_test_only` and proves the fixed-width (including CO₂ local
isotopologues 10–12), Voigt, line-area/grid-phase, hash, bzip2,
spectral-table, two-argument-mean, self-broadening, interpolation-certificate,
and OOD paths;
it is never an operational physics fallback.  The independent CC-BY
EM2C-SNB harness is `tools/crosscheck_fire_gas_opacity_em2c.py` (DOI
10.17632/x5wjzk6sjs.1); it refuses the synthetic table and is ready for the
first owner-derived physical record, at which point the provisional 5 % gate
must be reset from observed comparison error rather than assumed.

---

## Item 6 — gas + aerosol thermochemistry records

**Criteria** (§3.3, §3.4; §12 item 6): per-species W, c_p(T), h_s(T) with
common T_ref, h_s(T_ref)=0, dh_s/dT=c_p, c_p bounds; certified closed
domains; compositions + temperatures; fuel formula/LHV; atom-balanced
products; aerosol (soot carbon + condensed organics) records under the same
proofs; admissibility inequalities; freeze + hash.

**Verdict: closeable almost entirely from open canonical databases.**
Per-field ranking (all access-verified by download this session):

- **Gas species** (N₂/O₂/CO₂/H₂O/CO/CH₄/CH₃OH/C₇H₁₆): NASA Glenn
  `thermo.inp` from the official **Apache-2.0 nasa/cea GitHub repo**
  (machine-readable, certified closed domains, ΔfH included — so LHVs are
  *computed* from the record's own formation enthalpies, making the energy
  ledger self-consistent). Cross-checks: Burcat/ReSpecTh, JANAF.
- **Soot-carbon aerosol c_p**: JANAF C-002 graphite (open .txt, 0–6000 K)
  or CEA `C(gr)`.
- **Levoglucosan vapor** (Y_cv): Burcat `C6H10O5` (200–6000 K, in-record
  provenance chain to published papers) — **license flag below**.
- **Paraffin C25H52 vapor**: owner-approved open replacement: least-squares
  per-CH₂ increments across the pinned NASA CEA n-butane through n-octane
  NASA-9 series, with C25H52 formed as n-octane + 17 CH₂ increments.  The
  generated record carries the observed adjacent-increment residuals and a
  propagated `assumption_bound`, including the same-method extension to 200 K.
  RMG Benson groups are a corroboration-only cross-check because that repository
  has no license; no RMG bytes or coefficients enter the operational record.
- **Condensed-organics c_p**: Kabo 2015 measurement 5–370 K — paper
  paywalled but the **raw data is open in the NIST ThermoML archive**
  (verified, 45 C_p points). **Genuine gap: no measurement above 370 K
  exists anywhere** — and §3.3's no-extrapolation rule would cap the common
  certified interval at 370 K and break enthalpy inversion in every flame
  cell, so the record **must** embed a justified published estimation route
  (Růžička–Domalski group additivity, open JPCRD) certified in-record as an
  estimate with bounds.
- **Wood-volatiles surrogate**: Ritchie et al. 1997 (IAFSS, open)
  C₃.₄H₆.₂O₂.₅ with Δh_c = 17.7 MJ/kg — the canonical FDS-lineage lumped
  species; CRECK biomass mechanisms (open GitHub) if a multi-species
  mixture is ever wanted.

**Owner resolution (2026-08-11)**: the open CEA CH₂-increment route above
replaces the Burcat pentacosane estimate and closes its low-temperature flag.
Levoglucosan vapor remains owner-gated and must stay a fail-closed missing
record with no placeholder values.  The condensed-organics route above 370 K
also remains owner-gated while the NIST source is unavailable; consumers that
require either missing record reject, while other fuels may proceed under the
existing preview label.

---

## Item 7 — transport closure record

**Criteria** (§3.2; §12 item 7): sourced μ_k(T)/k_k(T) with certified
domains; Wilke + Wassiljewa/Mason–Saxena mixture rules (verified
implementations); unit-Lewis D; Pr_t = Sc_t = 0.7; Vreman C_v = 0.07 with
directional Δ_m and the zero-denominator rule; resolved no-slip/adiabatic
walls; τ_chem ≈ 10⁻⁴ s; T_CFT ≈ 1700 K.

**Verdict: fully closeable from open sources** — every paywalled original
has a verified open restatement carrying the needed numbers:

- μ_k/k_k: GRI-Mech 3.0 `transport.dat` (live, open) + CHEMKIN TRANSPORT
  manual kinetic theory (open Notre Dame mirror), or NASA TM-4513
  coefficient tables (open, **natively carrying certified per-fit T
  domains**); accuracy anchor Lemmon & Jacobsen 2004 via the open
  NIST-hosted PDF. Cantera (open) as the independent cross-implementation.
- Wilke rule: CHEMKIN manual Eqs. 48–49 (open, textbook-independent).
- **WMS conductivity — content flag**: CHEMKIN/Cantera do *not* implement
  Wassiljewa/Mason–Saxena (they use the Mathur combination average, manual
  Eq. 50). The open WMS statement + implementation is **IDAES** (US-DOE,
  citing Poling §10-6, a paywalled book). The record must cite IDAES/Poling
  and note the deliberate divergence from combustion-code practice.
- Vreman: the author-hosted open PDF confirms c = 2.5·C_S² = 0.07, the
  zero-denominator rule, and **per-direction filter widths** — cite
  Vreman 2004, not FDS (which uses the cube-root width).
- Pr_t/Sc_t: **flag** — FDS's fire-calibrated default is 0.5 (verified
  from the open Tech Guide LaTeX source); the design's 0.7 is the classic
  engineering-LES value and needs an explicit rationale line.
- τ_chem: **flag** — FDS's constant is 10⁻⁵ s (verified in `cons.f90`);
  the design's 10⁻⁴ s is its own declared bound; cite FDS as
  "order 10⁻⁵–10⁻⁴", not as using 10⁻⁴.
- T_CFT: FDS `data.f90` per-fuel table, default 1427 °C = **1700 K exactly
  matching the design**; numeric values copyable from open FDS source (the
  Beyler/SFPE provenance chain is paywalled but not needed for the bytes).

---

## Cross-cutting conclusions

1. **Items 5, 6, 7 are buildable now** — open data plus RISE-owned
  generation code; no laboratory work, no author contact. They are the
  natural first record-generator targets when Phase C starts, and their
  stop-and-report gates are now pre-answered.
2. **Item 4's methane-first path and the Q1 methane record share one
  author contact**: the drafted Lai email needs two additions (end-to-end
  uncertainty budget; any laminar *diffusion* case on the calibrated rig)
  and then serves three purposes at once — chem record, radiance gate, and
  sim-compatible geometry. The premixed-cone/EDC conflict needs an owner
  ruling only if the group has no diffusion data.
3. **One modest instrument campaign closes two blockers at once**: a
  traceable spectroradiometer on the Hamins candle closes item 4 for wax
  *and* the chem-negligibility denominator. Every literature route to
  either has now been exhausted across two independent audits.
4. **Item 3 is the one true measurement gap**: the paired
  calorimetry+speciation crib series has never been run anywhere; NIST
  NFRL collaboration is the credible path, FIREX bounds y_cond
  non-negligible meanwhile (keeping wood predictive grids blocked, which
  the design already handles as a labeled preview).
