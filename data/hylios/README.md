# HyLIoS source data evidence and import

This directory records source evidence and literal parameter tables. It does not contain an executable or physiologically certified skin specimen. The `.m` and numeric text files remain in an external cache; their URLs and SHA-256 digests are pinned in [source_manifest.json](source_manifest.json). The published S1–S4 values in [presets_source.json](presets_source.json) remain in the units and form reported by Tables III–IV.

## Reproduce the source cache and checked curves

Fetch is an explicit step. It downloads only 33 pinned artifacts: the original 30 for 14 supported author skin absorber curves and Palmer–Williams real water IOR, plus the Hale–Querry real-index script and imaginary-index script/text pair used by the optional approximation profile. It verifies each against the manifest and stores it under the requested cache root. It never executes MATLAB or downloads automatically during import or rendering. The colliding `water/riW2.txt` IAPWS artifact is excluded from the Hale–Querry sources.

```sh
python3 tools/hylios_fetch_data.py --output /tmp/hylios-source-cache
python3 tools/hylios_import_data.py \
  --source-dir /tmp/hylios-source-cache \
  --output /tmp/hylios-optical-data
python3 tools/check_hylios_sources.py --check
python3 -m unittest discover -s tools -p 'test_hylios_*.py' -v
```

The fetcher reuses files only when their digest matches, rejects a mismatched existing file without overwriting it, caps each download at 16 MiB, and writes verified bytes atomically. It accepts only HTTPS URLs on `www.npsg.uwaterloo.ca` from the manifest. Generated JSON and cached source files belong outside the repository.

The strict importer produces 15 curves from the original 30 artifacts; the additional Hale–Querry files do not change its output. It reads literal numeric assignments from the pinned MATLAB files with a small allowlisted parser. It does not evaluate MATLAB. Other statements are retained only as source text and ignored. Generated JSON preserves each source array’s original grid, raw quantity and units, source file hashes, and actual wavelength endpoints. MATLAB `.txt` interpolation products are also hash-checked but are not substituted for irregular `wave0` source grids.

For absorptance source datasets, the author scripts store absorbance values; their plotting expression converts those to absorptance. The importer records the input absorbance and the reference-sample Napierian coefficient `ln(10) * absorbance / path_length`. DNA’s documented point-12 normalization is applied once. Keratin, DNA, and urocanic-acid values are not scaled to Table IV tissue composition. Mass and molar extinction spectra remain in the source basis, without `ln(10)`, molar-mass, concentration, or tissue-mixture conversion. Water and lipid Napierian coefficients retain their raw `cm^-1` values and also receive the exact `cm^-1` to `m^-1` scale factor of 100.

## Interpretation and coverage limits

Source grid endpoints describe the values actually imported, not a promise that the publication model is validated over that interval. The water absorption and Palmer–Williams real-index candidates reach 2500 nm. The lipid table reaches 2449.5 nm and has irregular source intervals. Other active absorbers cover much narrower ranges: for example, DNA reaches about 360 nm, urocanic acid about 327 nm, keratin about 421 nm, and hemoglobin/melanin tables have distinct endpoints. No strict imported curve is extrapolated, endpoint-held, or given an inferred zero tail. Each JSON includes the source wavelength vector and adjacent interval summary so irregular spacing and gaps remain inspectable.

The source material still leaves scientific interpretation work: the thesis’s printed keratin multiplier conflicts with inverse path-length units; DNA and urocanic-acid concentration scaling and overlapping mixture bases require an explicit model interpretation; extinction conventions need reconciliation before combining coefficients; Table III melanosome dimensions do not clearly identify full axes versus semi-axes; and Table III/Table IV quantities do not by themselves define all layer scaling. The strict importer does not resolve these choices. The separate [cleanroom composition contract](../../docs/HYLIOS_COMPOSITION.md) names executable interpretations and their sensitivity alternatives; it does not establish author equivalence or implement the full transport model.

## Explicit spectral approximation profile

[approximation_profile_v1.json](approximation_profile_v1.json) defines `hylios_cleanroom_approximation_v1`, an optional unvalidated spectral profile covering 250–2500 nm. Build its bundle from the same verified 33-artifact cache, then query it in each curve’s original quantity and units:

```sh
python3 tools/hylios_approximation_profile.py \
  --source-dir /tmp/hylios-source-cache \
  --output /tmp/hylios-approximation-profile-v1.json
python3 tools/hylios_approximation_profile.py \
  --bundle /tmp/hylios-approximation-profile-v1.json \
  --curve water_absorption --wavelength-nm 250 --wavelength-nm 380
```

The bundle preserves the 15 strict curve objects and both Hale–Querry source grids. Evaluation uses linear interpolation within source support and explicit policies outside it:

- Eumelanin and pheomelanin use endpoint-anchored power laws with exponents −3.33 and −4.75, borrowed from [Donner and Jensen (2006), Eqs. (3)–(4)](https://cseweb.ucsd.edu/~henrik/papers/skin_bssrdf/skin_bssrdf.pdf). Only the exponents are borrowed; extending these shapes to 2500 nm is an unvalidated assumption.
- Keratin, DNA and urocanic acid have explicit zero tails. These omit unmeasured absorption; they do not establish measured transparency.
- Beta-carotene, bilirubin and oxy/deoxyhemoglobin use endpoint-anchored exponential upper tails with an arbitrary 100 nm e-fold scale. This is an empirical approximation, not a published extrapolation model.
- Carboxy-, met- and sulfhemoglobin use endpoint-scaled oxyhemoglobin shapes outside their narrower tables, including the oxyhemoglobin upper-tail policy. This explicit species substitution omits unknown spectral features.
- Lipid uses constant endpoint extension. UV water absorption uses Hale–Querry imaginary index via `mu_a = 4*pi*k/lambda`, with a multiplicative continuity splice at 380 nm. Water real index uses Hale–Querry values with an additive continuity splice at 400 nm. These source splices are cleanroom assumptions.

Every query reports `approximate: true`, the profile ID, a canonical generated-bundle SHA-256 fingerprint, applicable policies, and original source-file digests. Loading a bundle checks its generated-content fingerprint and structure; rebuilding from `--source-dir` additionally recomputes the pinned original-byte hashes. The fingerprint is not a signature or a substitute for source verification. No dense resampling or additional concentration, molar-mass, extinction-basis, or tissue-mixture conversion is performed. Numerical tests establish these evaluation contracts, not agreement with published HyLIoS results or measured skin. The strict importer remains independent of this opt-in profile.

## Resolve a specimen

The composition tool converts raw curves once, applies the named layer/blood/organelle conventions, and records constituent contributions, raw evaluation traces and content identities:

```sh
python3 tools/hylios_compose_specimen.py \
  --approximation-bundle /tmp/hylios-approximation-profile-v1.json \
  --preset S3 --wavelength-nm 250 --wavelength-nm 550 \
  --wavelength-nm 1300 --wavelength-nm 2500 \
  --output /tmp/hylios-s3-resolved.json
```

Use `--strict-curves-dir` instead of `--approximation-bundle` for non-extrapolating source curves. It fails when an active constituent lacks coverage; inactive constituents need no spectral evaluation. A resolved specimen is preparation data, not a skin simulation result. The [measurement CLI](../../docs/HYLIOS_MEASUREMENT.md) accepts both individually dispersed S4 specimens and explicit complex layouts (`--complex-layout explicit-pack-v1`). Its top-level approximation label and composition/coverage identities must survive downstream measurement and bake outputs.

The NPSG files are publicly readable, but redistribution/licensing permission for the source tables has not been established. The repository records URLs and digests and does not vendor those files. No author contact or permission claim is made here.
