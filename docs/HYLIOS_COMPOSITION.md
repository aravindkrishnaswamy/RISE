# HyLIoS executable composition contract

**Revision:** `hylios_cleanroom_composition_v1`, 2026-11-25. This document
defines how the 15 checked raw curves and the source parameter tables resolve
to six host-layer absorption coefficients, six real refractive indices, and
separate melanosome parameters. It is a source-informed clean-room
interpretation, not the unavailable authors' implementation or a validated
reproduction of their results. BioSpec semantics are unchanged.

The composition revision and the spectral coverage/approximation profile are
separate identities. Selecting this composition does not select any missing
spectral tail. An implementation must record both in resolved provenance.

## 1. Evidence and units

Primary locations used below:

- **P:** Chen et al., [Hyperspectral Modeling of Skin Appearance](https://www.npsg.uwaterloo.ca/resources/docs/a31-chen.pdf),
  2015, §3.1 (absorber locations), §4.1/Eq.2 (layer mixture), §4.2.1/Eqs.3–4
  (melanosomes), §5.1 (size reduction).
- **S:** [Online appendix](https://www.npsg.uwaterloo.ca/resources/docs/a31-chen-app.pdf),
  p.1/Fig.26 (reference sample bases and index construction), p.2/Tables III–IV
  (all defaults).
- **T:** Chen, [PhD thesis](https://www.npsg.uwaterloo.ca/resources/docs/theses/Chen_Tenn_PhD_Thesis.pdf),
  Appendix A, printed pp.93–95, Eqs.A.1–A.5 and their footnote (extinction,
  concentration, and reference-path conversions).
- **D:** [NPSG skin data](https://www.npsg.uwaterloo.ca/data/skin.php),
  [blood data](https://www.npsg.uwaterloo.ca/data/blood.php), and
  [water data](https://www.npsg.uwaterloo.ca/data/water.php), specifically the
  `.m` files pinned in [source_manifest.json](../data/hylios/source_manifest.json).
  Numeric assignments and plot/output expressions were inspected without
  executing MATLAB.
- **H:** Prahl, [Optical Absorption of Hemoglobin](https://omlc.org/spectra/hemoglobin/),
  sections “Equivalents” and “Whole Blood”: the compiled oxy/deoxy tables use
  molecular molarity and a mass-to-mole conversion of 64,500 g/mol. This is
  different from the 66,500 g/mol printed in T Eq.A.2.

[presets_source.json](../data/hylios/presets_source.json) remains a literal
source transcription. The conversions and conventions here belong to the
resolved specimen, not to a rewrite of that transcription. The raw schema and
exact DNA normalization are implemented by
[hylios_import_data.py](../tools/hylios_import_data.py).

Kernel units are m, m^-1, nm, kg/m³ for mass concentration, mol/m³ for molar
concentration, and dimensionless fractions/real indices. Source-table
concentrations are shown in their original units below to make comparisons
auditable:

```
1 cm = 0.01 m                 1 cm^-1 = 100 m^-1
1 µm = 1e-6 m                1 nm = 1e-9 m
1 mg/mL = 1 g/L = 1 kg/m³     1 µg/mL = 0.001 kg/m³
1 mol/L = 1000 mol/m³         p percent -> p/100
ln10 = ln(10), not log10(10)
```

Only explicitly tagged raw quantities may be converted. In particular,
`reference_absorption_coefficient` in imported DNA/UCA/keratin JSON has
already received `ln10/path`; do not apply that transform twice. A raw molar
or mass extinction array has received neither `ln10` nor concentration.

## 2. Constituent conversions

Let `L=ln(10)`, and evaluate each raw curve by linear interpolation in
wavelength **within its own covered interval**. This agrees with the source
scripts' linear interpolation, while preserving their original irregular input
grids. Out-of-range evaluation requires an explicitly selected coverage
profile; endpoint clamping is not implicit in this contract. A zero loading
disables that constituent before asking for its spectrum.

The following formulas return Napierian coefficients in m^-1. `C` is the
numeric concentration in mg/mL (= g/L = kg/m³), `M` is in g/mol, and `c` is
numeric mol/L. These source-unit helper signatures are deliberately explicit;
an SI-only implementation can use the equivalent forms in the last paragraph.

| Raw curve ID | Raw value and reference basis | Resolved coefficient |
|---|---|---|
| `eumelanin_extinction` | `E_e`, cm^-1/(mg/mL), D; T A.1 | `m_e(C)=100*L*E_e*C` |
| `pheomelanin_extinction` | `E_p`, same basis | `m_p(C)=100*L*E_p*C` |
| `oxy_hemoglobin_extinction` | `E_o`, cm^-1/(mol/L), Prahl molecular basis | `h_o(C)=100*L*E_o*C/64500` |
| `deoxy_hemoglobin_extinction` | `E_d`, same basis | `h_d(C)=100*L*E_d*C/64500` |
| `carboxyhemoglobin` | `E_co`, source labels molar, D blood file | `h_co(C)=100*L*E_co*C/66500` |
| `methemoglobin` | `E_met`, source labels molar, D blood file | `h_met(C)=100*L*E_met*C/66500` |
| `sulfhemoglobin` | `E_sulf`, source labels molar, D blood file | `h_sulf(C)=100*L*E_sulf*C/66500` |
| `beta_carotene` | `E_bc`, cm^-1/(mol/L); T A.2 | `b(C)=100*L*E_bc*C/537` |
| `bilirubin` | `E_bil`, cm^-1/(mol/L); T A.2 | `q(C)=100*L*E_bil*C/585` |
| `water_absorption` | `A_w`, already Napierian cm^-1, D | `W=100*A_w` |
| `lipid_absorption` | `A_l`, already Napierian cm^-1, D | `F=100*A_l` |
| `keratin_abs` | `B_k`, decadic absorbance of solid sample over 4 µm | `K=L*B_k/(4e-6)` |
| `dna_abs` | `B_d`, normalized decadic absorbance, 0.05 mg/mL over 1 cm | `D(C)=L*B_d/0.01*(C/0.05)` |
| `uro_acid_abs` | `B_u`, decadic absorbance, 15e-6 mol/L over 1 cm | `U(c)=L*B_u/0.01*(c/15e-6)` |
| `palmer_williams_real_ior` | real water index `n_w`, dimensionless | Section 5; no absorption conversion |

For a fully SI implementation, a decadic mass-extinction value becomes
`100*L*E` m²/kg and is multiplied by kg/m³. A decadic molar-extinction value
becomes `0.1*L*E` m²/mol and is multiplied by mol/m³. The latter concentration
is `1000*C/M`; equivalently use molecular mass `M/1000` kg/mol with `C` kg/m³.
These forms are equal to the table; do not mix their factors of 1000.

### Absorbance, absorptance, and abbreviated thesis formulas

D's keratin/DNA/UCA script comments describe absorptance, but their numeric
assignments and exported text are **decadic absorbance**. Their plot expression
`(1-10.^(-data))*100` supplies percent absorptance. If instead consuming a
fractional absorptance `a`, first compute `L*B=-ln(1-a)`; percent absorptance
must first be divided by 100. Do not use `-ln(1-B)` on the pinned raw arrays.

`dna_abs.m` divides input `data0` by its twelfth element, `0.8373`, located
at 257.9556 nm. The importer applies that operation once. The script calls
this “A260 = 1”, but a second normalization at interpolated 260 nm would change
the source data and is excluded.

The following explicit conventions resolve T's dimensional omissions:

- `keratin_inverse_reference_path_v1`: use the stated 4 µm sample path.
  Its inverse is 2,500 cm^-1 (250,000 m^-1) per unit optical depth. T p.95
  instead prints a multiplier of `4e4` for cm^-1, which is **16 times larger**.
  This contract uses the inverse path, not that printed multiplier.
- `dna_reference_concentration_ratio_v1`: scale the measured sample coefficient
  by target/reference concentration. T A.4 multiplies optical depth by skin
  DNA density without explicitly dividing by the reference 0.05 g/L. At the
  table default the ratio is `0.185/0.05=3.7`. Reading T's density number as
  a bare multiplier would instead be 20 times smaller.
- `uca_reference_concentration_ratio_v1`: use the same Beer–Lambert ratio.
  At the default it is `0.01/(15e-6)=666.6666666666666`. T A.5 calls its
  multiplier `M_w` but the accompanying values are mol/L, hence a
  concentration, not a molecular mass. Multiplying by the bare `0.01` without
  the reference divisor would be 66,666.6667 times smaller.

These are dimensional clean-room resolutions supported by the stated sample
bases and T's absorptivity footnote. They are not claims about what the
authors' code computed.

### Hemoglobin basis pairing

`prahl_molecular_64500_v1` pairs the unchanged Prahl-derived oxy/deoxy values
with their source's 64,500 g/mol. To express the identical mass absorption
using a 66,500 g/mol denominator, first multiply those extinction values by
`66500/64500`; then division by 66,500 cancels that basis change. Merely
switching the denominator to 66,500 reduces the result by
`64500/66500=0.9699248120` (about 3.01%). No factor four is applied to these
functional tables: H explicitly distinguishes molecules from four heme
equivalents.

`npsg_dyshemoglobin_literal_molar_66500_v1` uses D's dysfunctional tables as
the molar quantities they are labeled and T A.2's 66,500 g/mol. The inspected
NPSG headers do not settle whether the upstream experimental molarity is
per heme equivalent or per tetramer. Therefore the selected multiplier is
explicitly **1**, with a required sensitivity alternative **4** if these
extinction values are reinterpreted per equivalent. Do not infer that factor
from relative peak heights, silently multiply every hemoglobin by four, or
present this convention as an independently established basis for the
upstream experiments. This is an irreducible source-basis uncertainty in the
current evidence, not an obstacle to executing the labeled convention.

## 3. Layer assembly and loading rules

Use the outer-to-inner order `SC, SG, SS, SB, PD, RD`: corneum, granulosum,
spinosum, basale, papillary dermis, reticular dermis. `E` below means each of
`SG, SS, SB`, not an extra seventh layer. Let `w_j`, `l_j`, `k_j`, `f_j`, and
`v_b,j` denote water, lipid, keratin, colloidal-melanin, and blood loadings.

```
M_mel = m_e(C_e) + m_p(C_p)
C_functional = C_Hb_total - C_met - C_co - C_sulf
C_oxy = oxygenation * C_functional
C_deoxy = (1-oxygenation) * C_functional
B_blood = h_o(C_oxy) + h_d(C_deoxy)
        + h_met(C_met) + h_co(C_co) + h_sulf(C_sulf)
        + b(C_bc_blood) + q(C_bilirubin_blood)

mu_SC = w_SC*W + l_SC*F + k_SC*K + U(c_UCA) + b(C_bc_SC)
mu_E  = w_E *W + l_E *F + D(C_DNA) + b(C_bc_E) + f_E*M_mel
mu_PD = w_PD*W + l_PD*F + v_b_PD*B_blood
mu_RD = w_RD*W + l_RD*F + v_b_RD*B_blood
```

Every concentration already refers to its enclosing material: `C_bc_SC`,
`C_bc_E`, and `C_DNA` are per tissue volume, while hemoglobin, bilirubin,
and blood beta-carotene are per whole-blood volume. Therefore the tissue
concentrations receive **no additional guessed pigment volume fraction**,
and every whole-blood contribution receives exactly one `v_b,j`. The
functional split receives oxygenation exactly once. Do not multiply all
dysfunctional pigments or bilirubin by oxygenation. The apparent `zeta_ch*v_sh`
subscript in P Eq.2 is not a reason to substitute sulfhemoglobin loading
for carboxyhemoglobin.

The executable choices beyond the printed equations are:

- `whole_blood_total_pool_v1`: interpret Table IV's 147 mg/mL as total Hb,
  including the separately specified dysfunctional species. Subtract them
  before splitting functional Hb. Default functional concentration is 144,
  hence 108 oxy/36 deoxy for 75% and 136.8 oxy/7.2 deoxy for 95%.
  The source does not explicitly say whether 147 instead means functional
  Hb. The alternative adds 3 mg/mL functional Hb at defaults, increasing
  only that component by `147/144-1=2.0833333%`; retain as sensitivity data.
- `viable_epidermal_dna_v1`: apply the tabulated skin DNA density uniformly
  to SG/SS/SB, with zero DNA in SC/PD/RD. P §3.1 places relevant UV absorbers
  in SC/epidermis, Table IV supplies one skin density, and P Eq.2's dermal
  example omits DNA. None specifies a complete DNA spatial distribution.
  This selected mask is explicit; alternative allocation to dermis/SC or
  cellular-volume weighting requires a different revision.
- `colloid_shared_melanin_concentration_v1`: the colloidal fraction multiplies
  the same `M_mel` as organelle-interior absorption. Table III supplies
  melanosomal `C_e,C_p` and colloidal percentages but no separate colloidal
  concentration or eumelanin:pheomelanin ratio. Sharing that basis closes
  the missing input explicitly. `f_E` and organelle fraction are separate;
  neither is subtracted from the other or summed before transport.
- `independent_effective_loadings_v1`: preserve the tabulated coefficients
  as independent absorption loadings. In SC, water .35 + lipid .20 +
  keratin .65 = **1.20**. Thus these inputs cannot honestly be described as
  a normalized, exhaustive partition of disjoint physical volumes. Do not
  renormalize them, compute a negative remainder, or rescale water/lipid by
  `1-blood`, `1-melanosome`, or `1-colloid`. Such choices materially alter
  the authored experiment and need a separately named model.

The above coefficients apply to the homogeneous host walk. Melanosomes are
separate attenuation events with interior coefficient `M_mel`; do not add
`v_mel*M_mel` to `mu_E` as well. While traversing an explicit melanosome
interior, its absorption test uses `M_mel`; the host coefficient is not
applied again over that interior chord. This follows the event separation
in P §4.1/§4.2.1 and freezes its ownership for this implementation.

These are effective optical rules, not a reconstructed mass-density model:
no undocumented pure-chromophore density, hematocrit conversion, or mass to
volume conversion is required or allowed. The source's fractions and
concentrations operate on different bases. Individual fractions must remain
in [0,1], but their cross-constituent sum is not constrained to one.

## 4. Published defaults resolved without hidden normalization

All values in this section are S Tables III–IV unless labeled convention.
Each thickness below is in **µm**, obtained by multiplying source cm by 10,000;
multiply by `1e-6` to obtain the metres stored in a specimen.

| Preset | SC | SG | SS | SB | PD | RD | Surface fold aspect ratio |
|---|---:|---:|---:|---:|---:|---:|---:|
| S1 | 10 | 17 | 17 | 17 | 200 | 1000 | .10 |
| S2 | 10 | 33 | 33 | 33 | 100 | 1000 | .25 |
| S3 | 4 | 33 | 33 | 33 | 200 | 1250 | .10 |
| S4 | 2 | 7 | 7 | 7 | 230 | 2000 | .45 |

| Preset | Organelle fractions SG/SS/SB | Colloid fractions SG/SS/SB | `C_e` | `C_p` | Full base major/minor diameters µm | PD/RD blood fractions | Oxygenation |
|---|---|---|---:|---:|---|---|---:|
| S1 | .01/.01/.01 | .008/.008/.008 | 90 | 4 | .41/.17 | .002/.002 | .75 |
| S2 | 0/0/.0375 | .0125/.0125/.0125 | 50 | 2 | .41/.17 | .007/.007 | .95 |
| S3 | 0/0/.03 | .0135/.0135/.0135 | 32 | 2 | .41/.17 | .003/.003 | .75 |
| S4 | .10/.10/.10 | .15/.15/.15 | 50 | 4 | .69/.28 | .025/.025 | .75 |

`C_e,C_p` are mg/mL. Fractions in this table have already been divided by
100; do not divide them again. No melanin/organelle term is assigned to SC
or either dermis by this revision.

| Fixed input | SC | SG/SS/SB each | PD | RD |
|---|---:|---:|---:|---:|
| Real IOR at 1300 nm | 1.55 | 1.40 | 1.39 | 1.41 |
| Water loading | .35 | .60 | .75 | .75 |
| Lipid loading | .20 | .151 | .1733 | .1733 |
| Keratin loading | .65 | 0 | 0 | 0 |
| DNA concentration mg/mL (selected spatial convention) | 0 | .185 | 0 | 0 |
| UCA concentration mol/L | .01 | 0 | 0 | 0 |
| Tissue beta-carotene concentration mg/mL | .00021 | .00021 | via blood | via blood |

Fixed whole-blood concentrations in mg/mL: total Hb 147, methemoglobin 1.5,
carboxyhemoglobin 1.5, sulfhemoglobin 0, bilirubin .003, beta-carotene .00007.
The zero sulfhemoglobin default disables its curve request; changing it to
nonzero makes its coverage and selected basis part of specimen validity.

`full_diameter_melanosome_v1` treats S Table III's two dimensions as full
major/minor diameters. Semi-axes are `b=q*major_um*0.5e-6`,
`a=q*minor_um*0.5e-6`, with `q=.25,.5,1` in SG, SS, SB respectively
(P §5.1 size reductions). Recompute the geometric rate after scaling:

```
r = a/b
e = sqrt((1-r)*(1+r))
S_over_V = (3/(2*a))*(r + atan2(e,r)/e) # e=0 limit: 3/a
mu_organelle = (S_over_V/4)*v_organelle
```

`atan2(e,r)` equals the publication's `asin(e)` mathematically, while
retaining the small complementary angle for slender spheroids after `e`
rounds to one. Positive unit/loading/coefficient products that underflow to
zero or overflow are diagnosed as outside the supported numerical range;
only true zero inputs disable a term. This is a numerical domain check,
not an implicit zero-tail policy.

S1–S3 use complexes; S4 uses dispersed spheroids (P Table I and S's
qualitative low/high pigmentation classifications). The selected
`low_pigmentation_complex_v1` makes every S1–S3 complex diameter
`D_complex=2*(2*b)` after that layer's size scaling; sphere radius is
`D_complex/2`, encounter rate `3*v_organelle/(4*radius)`, and maximum
internal count is `round_half_up(D_complex/(2*a))`, at least one: 5 for
these three base aspect ratios. This treats the authored fraction as the
complex encounter fraction without reconstructing an unspecified packing
density. “Moderate” complexes with `D_complex=2*b`, loose/compact packing,
and any different meaning of the authored fraction need separately named
variants. No numeric packing factor is implied by the source presets.

Organelle interior absorption is `M_mel` from §3 and real index is constant
1.7 (S Table IV). The surrounding index is that epidermal layer's spectral
index. Geometry and angular/boundary/rejection laws are specified in
[HYLIOS_EQUATIONS.md](HYLIOS_EQUATIONS.md); these numbers do not replace
those laws or resolve all geometry interpretation questions.

The table diameter .41 µm differs from P's .40 µm light-specimen prose.
This revision uses the supplementary table and records that choice.
Interpreting listed dimensions as semi-axes instead doubles both axes,
halving the encounter rate at fixed fraction and doubling interior chord
lengths; it is not an innocuous unit conversion.

## 5. Spectral real IOR: `water_anchor_1300_v1`

S p.1 and T p.93 call for a Gladstone–Dale mixture using water content,
spectral water index, and layer indices measured at 1300 nm. The source
does not give a wavelength-dependent dry-matrix index. Select a constant
dry-matrix contribution and solve its anchor rather than guessing it:

```
n_dry,j = (n_anchor,j - w_j*n_w(1300))/(1-w_j)
n_j(lambda) = w_j*n_w(lambda) + (1-w_j)*n_dry,j
            = n_anchor,j + w_j*(n_w(lambda)-n_w(1300))
```

The equivalent `(n-1)` Gladstone–Dale form gives the same result. This is
an explicit volume-weighted interpretation using the table's water loading;
no tissue mass density appears. Do not multiply the entire anchor index by
the water-index ratio or assign the water curve itself to skin.

`n_w` is the pinned `palmer_williams_real_ior` curve; the anchor is evaluated
on that same curve at exactly 1300 nm. The algebra gives `n_j(1300)` equal
to the tabulated anchor regardless of roundoff in reconstructing `n_dry`.
Store/use the final difference form. If user water content varies, this
revision preserves the authored `n_anchor`; it changes dispersion, not the
1300-nm anchor. A fixed dry matrix with hydration-dependent anchor is a
different model and must have a different identity.

For the pinned curve `n_w(1300)=1.324`. The implied constant dry indices
for SC, E, PD, RD are approximately 1.6716923077, 1.514, 1.588, 1.668.
For custom inputs require `w<1` and positive inferred `n_dry`; admit the
pure-water endpoint `w=1` only when its 1300-nm anchor equals 1.324, in
which case use `n_j=n_w` without a dry-matrix division.

This construction applies identically to all three viable epidermal layers.
The source water curve covers only 400–2500 nm; UV IOR requires the separately
selected approximation/coverage profile. Real index and water absorption
are distinct curves; neither is derived from the other here. Melanin index
remains 1.7 and exterior index is a separate transport input.

## 6. Validation, provenance, and sensitivity

Reject nonfinite/negative concentrations or coefficients, fractions outside
[0,1], nonpositive layer thickness/axes/real indices, major axis smaller than
minor axis, oxygenation outside [0,1], and `C_Hb_total < C_met+C_co+C_sulf`.
Reject missing active curves on the requested wavelength domain unless the
chosen approximation profile covers them. Never make a negative functional
pool zero by clamping, and never silently normalize effective loadings.

Resolved output must identify all supplied curve source checksums, actual raw coverage,
composition revision, individual named conventions above, coverage profile
revision, all source inputs and converted units, per-constituent layer
contributions, total host absorption, organelle interior absorption/rate/axes,
and each layer's anchor and evaluated index. Record curve IDs that were never
evaluated, including missing inactive inputs. The exact raw curve may be
inactive at a wavelength; its skipped status should be observable separately
from a measured zero. An approximation label must survive output/bake cache
keys; a source-backed numerical conversion is not a license to describe
unmeasured tails as measured data.

Minimum analytic checks are: zero concentration/fraction removes just its
term; doubling a loading doubles that term; `ln10` appears exactly once;
1 cm^-1 becomes 100 m^-1; DNA/UCA reference concentrations reproduce their
reference-sample coefficient; 4 µm of solid keratin has optical depth
`ln10*B_k`; blood fraction multiplies every blood term once; oxygenation
endpoints select the correct functional species; `n_j(1300)=n_anchor,j`;
changing all dimensions by a factor two halves organelle rate while leaving
`M_mel` unchanged. Use synthetic constant spectra to isolate these from
missing spectral coverage.

Report observable sensitivity, rather than claiming exact reproduction,
for at least: keratin ×16 literal-thesis scaling; DNA reference-ratio versus
bare-density scaling; UCA reference-ratio versus bare-concentration scaling;
oxy/deoxy 64,500 versus unadjusted 66,500; dysfunctional Hb factor 1 versus 4;
147-total versus 147-functional blood interpretation; alternate DNA masks;
colloidal concentration basis; normalized versus independent SC loadings;
diameter versus semi-axis dimensions; complex packing/size interpretation;
and each selected missing-tail rule. Coefficient factors above do not imply
the same factor in reflectance: transport response must be measured.

The unresolved author-code choices are retained as these named conventions
and sensitivity alternatives. They do not prevent executing this contract,
and executing it does not resolve them historically.

## 7. Resolver utility

[hylios_compose_specimen.py](../tools/hylios_compose_specimen.py) resolves
explicit wavelength lists to versioned JSON with six layers, per-constituent
coefficients, organelle fields, and the complete evaluation provenance:

```sh
python3 tools/hylios_compose_specimen.py \
  --approximation-bundle /path/to/explicit-approximation-bundle.json \
  --preset S1 --wavelength-nm 250 --wavelength-nm 550 \
  --wavelength-nm 1300 --wavelength-nm 2500 \
  --output /tmp/hylios-s1-resolved.json
```

Use `--strict-curves-dir /path/to/imported-json/` instead for strict raw
coverage. A default specimen generally has no common wavelength support
for all its active absorbers; strict failure is expected rather than an
implicit zero tail. Missing inactive constituents are skipped. The explicit
approximation adapter preserves raw source units and records its bundle
fingerprint and tail traces. Loading JSON checks structure, bases and
content identities; it does not re-hash original downloaded source bytes.
The pinned importer/profile builder performs that byte verification.

The Python API also accepts an explicit source-table object for experiments.
It records its complete digest and whether it equals the checked-in source
transcription, and checks source-unit/layer-order tags. This capability does
not grant experimental values the table's historical authority. The command
line uses the checked-in table. Derived index anchors, axes and dimensions
are checked for positive finite values, and results are serialized only
after all requested wavelengths resolve successfully. Top-level `approximate`,
`coverage_profile_id` and `spectral_bundle_sha256` identify the coverage choice
without requiring readers to inspect nested provenance. File replacement uses
a unique sibling temporary and cleans up on failure. No renderer scene,
material API, or transport behavior is installed by this utility.
