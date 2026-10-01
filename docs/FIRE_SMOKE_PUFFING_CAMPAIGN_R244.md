# r244 — matched tier-6 puffing campaign (in progress)

## CURRENT: r245 descriptive budgets reviewed; top spatial localization next

The reviewed r245 observation-only budget run launched from zero at
2026-09-30 03:24:04 UTC, through the unchanged doubled-domain full window.
See [r245 current status](FIRE_SMOKE_BUDGET_LOCALIZATION_R245.md) for frozen
binary, process/receipt paths and post-completion obligations. Host exclusivity
was preserved through the sequential frame/history and budget verification,
which both passed. Descriptive budget interpretation passed two fresh post-fix reviews.
Top radial/time exchange localization precedes a numbered sensitivity design.
The r245 simulation completed exit 0 at 2026-10-01 05:00:32 UTC, reporting
15,315 steps and 235 frames; all ledgers and captured frame operands passed
independent verification. Physical localization remains pending. Both r244 spectra below are already complete.

The doubled-domain simulation completed exit 0 at 2026-09-29 18:56:55 UTC:
15,315 accepted steps, final time 25.156864278920693 s, all 235 frames across
the unchanged 14.625 s statistics window. Its 242 selected payloads passed
independent SHA-256/Merkle checks. The independent checkpoint/case/history
review returned zero P1/P2 (`lateral_completion_review.json`). The formal
verdict remains `pending_equal_time_composition`, not empirical qualification.

The frozen-v3 matched preview and spectrum pipeline completed both stages
exit 0 at **2026-09-29 21:13:27 UTC**. No campaign process remains active.
The generated scene, producer receipt and all 235 EXR/display pairs are retained.

| Observable | Baseline peak (Hz) | Doubled-domain peak (Hz) | Expected-bin / peak, baseline → wide |
|---|---:|---:|---:|
| Centerline heat release | 1.774306 | 1.569578 | 0.05173 → 0.35072 |
| Thresholded spectral-preview lit area | 0.068243 | 0.068243 | 0.002567 → 0.06606 |

Expected frequency is 2.738613 Hz; nearest exported bin is 2.729701 Hz.
At the 0.136485 Hz bin, wide/base **absolute** power is 0.36856 for centerline
and 5.95789 for lit area. Domain doubling changes the response but does not
make expected puffing dominant. Neither slow-mode collapse nor absence of a
domain effect is supported. Do not amend production domain multipliers from
this evidence. Budget localization comes before any boundary remedy, soot
work or tier-10 window.

`tools/fire/r244_spectrum_report.py --variant lateral` independently hashes
exported inputs, decodes all 235 lit-area PNGs and reproduces the spectra with
NumPy FFT (maximum peak-scaled difference below 4e-14). The full 257 bins per
observable are retained in `lateral_full_spectrum.csv`; the matched comparison
is `domain_comparison.json`, `domain_comparison_full_spectrum.csv` and
`domain_comparison_spectra.svg` under `docs/data/fire_r244/`. Baseline artifacts
remain unchanged.

Interpretation limits: first/second bins cover about one/two cycles and can
represent nonstationarity; they do not prove recirculation. Lit area is a
thresholded spectral display, not direct geometry, with native Nyquist 8 Hz.
Centerline processing downsamples to 512 points without an antialias filter;
an 8192-point processing sensitivity is separately recorded in the comparison,
without replacing native evidence, and preserves the unresolved verdict.
Raw DFT powers compare only the same observable/processing, not physical energy.
Tier 6 has 7.35 burner cells, so no refined empirical puffing claim is made.
Both native preview logs have 470 negative-radiance warning lines (two per
frame), maximum magnitude 4.7471e-9 baseline / 4.3722e-9 wide. Both use box
filtering; the generic filter warning does not diagnose the cause. This is an
image-interpretation limitation, not evidence of physical solver failure.

Artifact roots under `/Volumes/SSD/RISE/fire_production_calibration/`:
- `r244_tier6_lateral_double.v1` (simulation), `r244_tier6_lateral_preview.v1`
  (preview), `r244_tier6_lateral_spectrum.v1` (native spectra and comparison PNG).
- `r244_tier6_lateral_spectrum_launch.v1` (both stage logs and completion receipts).
- `r244_runtime.v3/FireSequenceTest`, SHA-256
  `afce9d45a9d07d6269757f04903e63b9988c25cceb0b91b78b10e2d17af62d45`.
- Wide case ID `a9369e770465bd76e2cc8d5fc3b79e7c2aac038ecb14a0e4424a97ba2f98d586`;
  producer ID `11edf3509c63076ec9f7b5c62f1e642366a0dda50a10d82773f79e683fcd9456`.

Fresh preview-provenance and spectrum-interpretation reviews returned zero
P1/P2. A final report-script review found one P2 input-hash gap in the
8192-point sensitivity; it was fixed with copied-input GREEN/RED controls.
Fresh post-fix review returned zero P1/P2. Existing full-window station
histories have now been extracted by height;
see [r245 localization](FIRE_SMOKE_BUDGET_LOCALIZATION_R245.md). Signed physical
budget instrumentation and its complete from-zero run remain next. No source/pilot,
physical gate, statistics window, derived bound or golden checkpoint changes
are authorized. Earlier sections below are chronological history.


The r243 diagnostic milestone is complete at commit `ee0296f6`: two review
rounds, independent visual/numerical and provenance/movie-QA lenses. Round 1
found one duplicated P2 color-bar alignment defect; corrected and regenerated.
The two fresh reviewers of v3 reported zero P1/P2. Artifacts and limits are in
[FIRE_SMOKE_STRUCTURE_DIAGNOSTIC_R243.md](FIRE_SMOKE_STRUCTURE_DIAGNOSTIC_R243.md).

## Completed baseline and spectra

The canonical tier-6 full-window run completed from zero with the exact
r241 frozen executable at 2026-09-28 15:33:43 UTC (exit 0). This is a new baseline, not reuse of old tier-6
results from a different kernel. No solver, owner-gate, physical gate, or
statistics policy was changed.

- Executable: `/Volumes/SSD/RISE/fire_production_calibration/r241_runtime.v2/FireSequenceTest`
- SHA-256: `7b5c95579e608880c0d7da4fed8fc7f3031cf2f86e7dc79dbbfd80f7e79281ba`
- Command: `--fire-production-temporal-capstone 6 0.0625 /Volumes/SSD/RISE/fire_production_calibration/r244_tier6_baseline.v1`
- Case ID: `73d1cac70e5e68b697e0462ef3b5784c606606372397823d08cce9815b833b1e`
- Discard: 10.531864278900095 s; statistics: 14.625 s; target: 25.156864278900095 s.
- Host ceiling: 32 GiB RSS/footprint; next-step reserve 4,693,590,016 bytes;
  graceful stop remains exit 97 with checkpoint sealing.
- Worktree: `/Users/aravind/Working/RISE/worktrees/r243-structure-diagnostic`.
- Launch receipt directory: `/Volumes/SSD/RISE/fire_production_calibration/r244_tier6_baseline_launch.v1`.
- Log: `/Volumes/SSD/RISE/fire_production_calibration/r244_tier6_baseline.v1.log`.
- Execution session: 66638. A wrapper writes `completion.json` under the launch
  directory when the child exits. Do not assume an absent receipt means success.

Receipt hashes are in `docs/data/fire_r243/tier6_baseline_launch.json`.
No full builds, LTO links, exhaustive qualification, second simulation, or
renders may run while a campaign process is active. Source/doc inspection is fine.
Check both the process and completion record before starting any subsequent
heavy work. Preserve any refused run; do not shorten windows or widen bounds.

The baseline accepted 15,315 steps to 25.156864278920693 s and published
235 frames across the full 14.625 s statistics window. The independent
`tools/fire/r244_verify_baseline.py` checked the ordered history, frame
schedule, every frame SHA, and all selected payload-v2 Merkle seals; its
small result is `docs/data/fire_r244/baseline_completion.json`. The producer's
formal verdict remains `pending_equal_time_composition`; completion is not
an empirical qualification or puffing verdict.

The frozen executable completed the matched preview and spectrum sequentially
at 2026-09-28 16:40:58 UTC, with exit 0 for both stages and the wrapper.
Receipts and logs remain in
`/Volumes/SSD/RISE/fire_production_calibration/r244_tier6_spectrum_launch.v1`.
The outputs are `r244_tier6_baseline_preview.v1` and
`r244_tier6_baseline_spectrum.v1`, under the same calibration root.

The full spectra are copied without alteration to
`docs/data/fire_r244/baseline_full_spectrum.csv` and plotted in
`docs/data/fire_r244/baseline_spectra.svg`. Independent FFT recomputation agrees
on each dominant bin (largest power discrepancy, divided by peak power,
about 5e-14). All 235 decoded displays reproduce their recorded lit areas;
all 8,916 centerline samples and 235 display samples span the complete
statistics window. The audit is `baseline_spectrum_audit.json`.

| Observable | Dominant Hz | Expected-bin Hz | Expected/peak power | 0.136485-Hz/peak power |
| --- | ---: | ---: | ---: | ---: |
| Centerline heat release | 1.774306 | 2.729701 | 0.051729 | 0.433346 |
| Physical-preview lit area | 0.068243 | 2.729701 | 0.002567 | 0.100221 |

These are baseline observations, not the paired domain verdict. The expected
frequency is 2.738613 Hz; the 0.137-Hz nearest bin is not dominant in either
observable. A 14.625 s window contains only about two periods of that slow
mode, so spectral power alone does not identify recirculation as its cause.
Interpolation to 512 samples does not add independent lit-area information
above the original 16 fps Nyquist frequency of 8 Hz. The doubled-domain
experiment remains required; no production domain multiplier has changed.

### Wider-domain implementation — validation in progress, launch pending review

Commits `075ab2ee` and `32224a28` contain the r244
case-variant implementation in `FireCase.{h,cpp}`, `FireCaseTest.cpp`, and
`FireSequenceTest.cpp`. It introduces the optional identity-bearing
`domain_study = r244-lateral-double-v1`, copies the canonical source and pilot
into the doubled grid with offsets (26,26,0), and carries that identity into
the producer, final manifest, preview selection, and spectrum selection.
The wider preview preserves the baseline's physical field of view.
The fixed CLI commands prepared are:

- `--fire-r244-domain-study-gate` (no simulation steps; checks canonical
  baseline identity, producer/final-manifest identity, full window, and
  opposite-domain rejection).
- `--fire-r244-lateral-double OUTPUT_DIR` (tier 6, 0.0625 s cadence, full window).
- `--fire-r244-lateral-preview SIMULATION_DIR PREVIEW_DIR`.
- `--fire-r244-lateral-spectrum CHECKPOINT SIMULATION_DIR PREVIEW_DIR SPECTRUM_DIR`.

**Engineering gate (completed; render-profile correction under validation):**
inspect the complete diff, clean warning-free make and Xcode Deployment
builds (plus Opto for any FP/LTO-sensitive change), run FireCaseTest, the r244
producer/manifest gate and relevant existing publication/owner gates. Prove
RED mutations for source rehashing and pilot misplacement; check runtime
pilot/source-boundary contact translation too. Validate that the old baseline
case ID remains byte-identical and that no solver kernel changed. Run fresh
independent correctness/provenance and design/test-fidelity reviewers only
after the self-gate passes. Fix findings and repeat with fresh reviewers.
Commit explicit source/docs/evidence pathspecs, freeze/hash the resulting
executable, and only then launch the full doubled-domain run from zero with
the unchanged host admission ceiling. No partial implementation is a launch
permit. The baseline verifier and its evidence also await milestone review.

## Ordered campaign milestones

1. **Completed:** full baseline completion, hashes and history coverage, and full
   centerline and lit-area spectra with the existing frozen binary. The
   established sequential commands after simulation are
   `--fire-production-temporal-preview SIMULATION_DIR PREVIEW_DIR`, then
   `--fire-production-puffing-spectrum 6 FINAL_CHECKPOINT SIMULATION_DIR PREVIEW_DIR SPECTRUM_DIR`.
   Keep display rendering identical between audit variants. A diagnostic lit
   area based on temperature must be separately named; it cannot be substituted
   into the physical-preview lit-area observable silently.
2. **Implementation complete; final review pending:** the doubled-lateral-domain experiment is an explicitly
   identified case variant. Launch from zero only after the final review. Do not amend r54's
   production default without evidence. Preserve dz/dx, vertical extent,
   source/pilot cells, source perturbation values, physical times, and window.
   Important implementation trap: current source perturbations hash GLOBAL
   `i+Nx*j` indices. Merely widening Nx with the same seed changes the burner
   pattern. Tier-6 canonical Nx=52 can be doubled to 104 with integer-cell
   translation; the source/pilot can be embedded exactly. Both pattern and
   pilot must be proved identical under that translation, with variant identity
   and provenance updated honestly. Current case-v1 validates canonical
   geometry, so an unrecorded grid override is prohibited.
3. Publish spectra for both variants. If the slow mode persists, localize by
   budgets before selecting a design-supported boundary experiment. Current
   r241 tier-8 evidence does NOT support the handoff's 1000-fold slow mode;
   preserve the actual observable-specific result rather than that premise.
4. Only after the puffing verdict: primary-source in-flame methane soot data,
   numbered design revision, conservative formation/oxidation implementation,
   mirror/RED gates, soot-on tier-8 full simulation and spectral movie.
5. Only after the corrected tier-8 milestone: tier-10 from zero, full empirical
   rows and spectral movie. No tier substitution or shorter statistics window.

The original owner request and authority limits are in the task attachment
`/Users/aravind/.codex/attachments/de79224d-bb7d-4039-abd4-31617c14385f/Pasted text.txt`.
Payloads stay outside git; source/docs/small evidence commit by explicit
pathspec. Never modify the main checkout or golden checkpoint. Fresh reviewers
must converge at each milestone. This file is a continuation record, not a
claim that the campaign or later milestones have completed.

### Self-audit before launch review

The specific failure risks are: (1) changed source perturbations from global
index hashing; (2) displaced or newly activated runtime pilot/contact cells;
(3) correct simulation identity but a canonical-domain final manifest;
(4) changed camera scale contaminating lit-area comparison; and (5) a shorter
statistics window or stale numerical mirror. The array and runtime-contact
checks, explicit manifest/producer preflight, fixed physical camera span,
full-window evidence audit, and regenerated mirror dependency manifests
address these separately. Canonical baseline preflight reproduces the exact
completed case ID; the wider case ID is
`a9369e770465bd76e2cc8d5fc3b79e7c2aac038ecb14a0e4424a97ba2f98d586`.
The clean make build and case tests pass; publication, runtime translation,
burner owner and launcher checks pass. The sanitizer/mutation gates, physical-flux gate, full Metal/fp64 kernel
sweep, and clean Xcode Deployment and Opto builds also pass. The only Xcode
warnings are the documented missing checkout-local OIDN search path and
AppIntents metadata notice. SHA-bound results are in
`docs/data/fire_r244/engineering_gate.json`; fresh independent review is
pending. No widened-domain run has launched.
Raw validation logs remain outside git in
`/Volumes/SSD/RISE/fire_production_calibration/r244_engineering_gate.v1`.


Sanitizer scope: the isolated instrumented build exercises the r244 derived
field and exact-array translation invariants, while the unchanged optics
dependency links its canonical make object. It is not an authorized producer
of campaign bytes. Running the historical whole-test digest assertion under
ASan/UBSan produced a different legacy small-case source digest even with the
make FP flags; the failed logs are retained. The full native FireCaseTest
still enforces and passes that original digest, and the native producer gate
still enforces and passes the exact completed capstone case ID. No digest
expectation or tolerance was changed. The two intended mutants respectively
restore wide-grid source rehashing and shift the pilot one extra cell.

The spectrum audit/plot can be reproduced with the bundled Python runtime
and `PYTHONPATH=rendered/r244_plot_deps` after installing Matplotlib there;
its CSV is the complete unmodified native export. Figure generation uses a
fixed SVG hash salt and no date metadata.


### Review round 1 and input-consumption correction

Two independent reviewers of `075ab2ee` each found the same two P2 gaps and
no P1: the spectrum consumer accepted arbitrary preview PNGs and incomplete
observation windows. Neither found actual corruption in the completed
baseline. The consumer now validates display→verified primary EXR→sequence
identity→selected frame/VDB hash, and binds the canonical full schedule,
manifest count, producer identity and centerline endpoints to the completed
checkpoint. It retains the existing cadence enclosure without widening it.

`tools/fire/r244_spectrum_input_gate.py` exercises the real consumer using
the immutable baseline artifacts. Both the original inputs and a reserialized
CSV control pass; full exported CSVs remain byte-identical. Truncation,
shifted endpoint, wrong VDB digest, retained prefix checkpoint, foreign
sequence preview and valid-but-swapped frame pairs all refuse before output
publication. Evidence is under external `r244_spectrum_input_gate.v2`.
An earlier harness used CRLF CSV and rejected at the header instead; that
false-positive setup was corrected to native LF, with the copied-CSV green
control proving the format is accepted. The earlier logs remain preserved.
The correction changes only the test/producer harness; shared library and
GPU source hashes remain those of the clean make/Deployment/Opto gates.
Fresh review of the corrected state is required before launch.

### Review round 2 — matched rendering profile

The two fresh reviewers of `32224a28` agreed on one remaining P2 and no P1:
valid same-frame previews with a different camera, display transform or
sampling recipe could still pass provenance verification. The completed
baseline itself is unchanged; both independent recomputations confirm its
spectra. The correction now checks the complete primary and display render
configuration against the baseline recipe in `matched_render_profile.json`.
Camera objects must exactly match the canonical pinhole camera built from
manifest geometry at the existing scene writer's six significant digits;
frame times must exactly match the manifest scene-time schedule. Only these
verified camera objects and times are normalized before hashing. Every other
configuration member remains bound, including animation structure, worker
policy, spectral sampling, filter, integrator, output exposure/ACES/white
balance and encoder settings. Predictive primaries are not this preview
recipe and are rejected. No tolerance or physical gate changes.

The prepared `r244_runtime.v1` binary is superseded by this correction and
has never launched a simulation. It remains preserved for negative-control
checks. The next frozen runtime will use a new directory. Fresh review is
required after the corrected input gate passes.

The final profile gate uses 15 controls/refusals, including original and
reserialized full-window inputs, no-op metadata reserialization, and three
synthetic display-metadata mutants (exposure, camera, spectral sampling).
Each mutant retains the original pixels and primary linkage, recomputes its
metadata hashes, passes the old frozen consumer, and is refused by the
corrected consumer. These are explicitly labeled synthetic metadata tests,
not re-renders or native encoder evidence. The three accepted controls leave
the established spectra unchanged. `render_profile_gate.json` records the
current executable, build log and repeated native gates; external input
receipts are in `r244_spectrum_input_gate.v3`. A mistyped publication option
briefly entered the default test suite; it was stopped, its log retained,
and the correct focused publication gate passed. No sealed run was active.

### Review round 3 — generated scene and resulting frame pairs

Both fresh reviewers of `7414bae9` found one remaining P2 and no P1: the
resolved render configuration omits scene geometry and lights. An otherwise
matching render with an added occluder would pass it. The native preview
producer now retains its exact generated scene and emits a receipt binding
that scene, the source manifest, and all four files in every frame pair.
The consumer compares the retained scene byte-for-byte to its canonical
scene generator and checks every receipt hash. This prevents alternate-scene
frames from reusing the canonical producer receipt. The receipt is an
ordinary producer record, not a signature against someone forging records;
no new retroactive provenance is claimed.

The historical baseline predates these receipts. Its already-audited 235
frame pairs are instead pinned by an explicit inventory root and sequence
ID; `baseline_preview_inventory.json` retains all 940 hashes and the exact
root preimage convention. A different baseline render cannot enter through
that exception. Synthetic receipt fixtures in both domain geometries check
the canonical positive case, added scene content even with recomputed scene
hashes, and a substituted frame under a retained receipt. These are labeled
byte fixtures, not rendered occluder images. Native image/provenance validation
still occurs separately before any spectrum is published. Receipt scene
text binds the original manifest path, so moving a new preview to a different
simulation path requires deliberate regeneration rather than silent adoption.

Prepared runtimes v1 and v2 never launched a simulation and remain preserved.
The scene correction will freeze into v3 only after validation. No source,
pilot, solver, physical gate, or temporal policy changes in this correction.

The scene-receipt self-gate is recorded in `scene_receipt_gate.json`; the
repeat full-window input gate is external `r244_spectrum_input_gate.v4`.
The original 235-frame baseline remains byte-identical. The native receipt
fixtures pass for both 52×52×80 and 104×104×80 domains, and their two targeted
mutations fail as intended. The same unchanged library/GPU build evidence
and current-executable native qualifications apply; no wider run has started.
