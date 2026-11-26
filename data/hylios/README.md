# HyLIoS source data evidence and import

This directory records source evidence and literal parameter tables. It does not contain an executable or physiologically certified skin specimen. The `.m` and numeric text files remain in an external cache; their URLs and SHA-256 digests are pinned in [source_manifest.json](source_manifest.json). The published S1–S4 values in [presets_source.json](presets_source.json) remain in the units and form reported by Tables III–IV.

## Reproduce the source cache and checked curves

Fetch is an explicit step. It downloads only the 30 artifacts listed for the 14 supported author skin absorbers and Palmer–Williams real water IOR, verifies each against the manifest, and stores them under the requested cache root. It never executes MATLAB or downloads automatically during import or rendering.

```sh
python3 tools/hylios_fetch_data.py --output /tmp/hylios-source-cache
python3 tools/hylios_import_data.py \
  --source-dir /tmp/hylios-source-cache \
  --output /tmp/hylios-optical-data
python3 tools/check_hylios_sources.py --check
python3 -m unittest discover -s tools -p 'test_hylios_*data.py' -v
```

The fetcher reuses files only when their digest matches, rejects a mismatched existing file without overwriting it, caps each download at 16 MiB, and writes verified bytes atomically. It accepts only HTTPS URLs on `www.npsg.uwaterloo.ca` from the manifest. Generated JSON and cached source files belong outside the repository.

The importer reads literal numeric assignments from the pinned MATLAB files with a small allowlisted parser. It does not evaluate MATLAB. Other statements are retained only as source text and ignored. Generated JSON preserves each source array’s original grid, raw quantity and units, source file hashes, and actual wavelength endpoints. MATLAB `.txt` interpolation products are also hash-checked but are not substituted for irregular `wave0` source grids.

For absorptance source datasets, the author scripts store absorbance values; their plotting expression converts those to absorptance. The importer records the input absorbance and the reference-sample Napierian coefficient `ln(10) * absorbance / path_length`. DNA’s documented point-12 normalization is applied once. Keratin, DNA, and urocanic-acid values are not scaled to Table IV tissue composition. Mass and molar extinction spectra remain in the source basis, without `ln(10)`, molar-mass, concentration, or tissue-mixture conversion. Water and lipid Napierian coefficients retain their raw `cm^-1` values and also receive the exact `cm^-1` to `m^-1` scale factor of 100.

## Interpretation and coverage limits

Source grid endpoints describe the values actually imported, not a promise that the publication model is validated over that interval. The water absorption and Palmer–Williams real-index candidates reach 2500 nm. The lipid table reaches 2449.5 nm and has irregular source intervals. Other active absorbers cover much narrower ranges: for example, DNA reaches about 360 nm, urocanic acid about 327 nm, keratin about 421 nm, and hemoglobin/melanin tables have distinct endpoints. No curve is extrapolated, endpoint-held, or given an inferred zero tail. Each JSON includes the source wavelength vector and adjacent interval summary so irregular spacing and gaps remain inspectable.

The source material still leaves scientific interpretation work: the thesis’s printed keratin multiplier conflicts with inverse path-length units; DNA and urocanic-acid concentration scaling and overlapping mixture bases require an explicit model interpretation; extinction conventions need reconciliation before combining coefficients; Table III melanosome dimensions do not clearly identify full axes versus semi-axes; and Table III/Table IV quantities do not by themselves define all layer scaling. The RISE data artifacts and parser do not resolve these choices or implement the HyLIoS transport model.

The NPSG files are publicly readable, but redistribution/licensing permission for the source tables has not been established. The repository records URLs and digests and does not vendor those files. No author contact or permission claim is made here.
