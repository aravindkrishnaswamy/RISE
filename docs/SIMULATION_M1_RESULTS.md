# M1 cache verification

M1 remains open. The reference comparison uses the historical M0 captures approved by the owner. No fire stepping, checkpoint or numerical policy was changed.

## Native data and compatibility

The cache writer and reader are in `src/Library/Simulation/SimulationCache.{h,cpp}`. An immutable manifest identifies the time support, output types, fidelity evidence, committed entries and payload hashes. A reader retains that generation across publication. Grid representation and limits are specified in [SIMULATION_GRID_FORMAT.md](SIMULATION_GRID_FORMAT.md). The OpenVDB importer preserves active and inactive samples, tile values, transforms and per-channel backgrounds. Fire-specific descriptors, optically relevant records and conversion receipts are handled in `src/Library/Utilities/FireSequenceCache.cpp`.

| Section 5 acceptance criterion | Status and observation |
|---|---|
| Existing fire scenes produce bit-equal voxel data | Met for the retained corpus. Three tier-6 frames yielded 6,045,768 identical scalar components. Three tier-8 frames yielded 13,983,840. The comparison also checked dimensions, minima, maxima, source digest and record identity. |
| Renders agree within noise | Met for the specified showcase frame at 256 × 384 with 16 samples. The native and legacy results lie within the measured variation of the historical comparison. The empty-frame control is plainly distinguishable. Details below. |
| Sealed fire manifests still load | Met on the archived 235-entry tier-8 envelope and the unmodified derived showcase envelope. Demand import of the latter produced one native frame for the first render. |
| A half-baked cache loads | Met. A cache containing only ordinal 1 rendered a transparent fire contribution at scene time zero. Its sidecar retained the immutable coverage digest; available frames retained the original source digest. |
| macOS GUI loads and renders a fire frame | Met in the full SwiftUI application executable through its viewport regression probe. It loaded and rendered both a cold legacy scene and a native cache. See the follow-up below for the SDK settings and the outstanding release gate. |

The historical tier-6 frames use a three-frame test manifest constructed from the matching archived follow-mode envelope and original .time digests. It is a test binding. The tier-8 test read ordinals 0, 1 and 2 from the original sealed manifest. Rechecked all 61 M0 reference files with `shasum -a 256 -c SHA256SUMS` and reran the v13 checkpoint normalization.

Run `bin/tests/FireCachePlaybackTest SOURCE NEW_CACHE 0 1 2` to compare historical frames. The native, cold legacy and missing-slot scene variants under `rendered/simulation-m1/render-*` used the original M0 scene and the same camera, resolution, sampling and optics. Only the output paths and, for the two native variants, manifest path differed. Each ran `renderanimation 0 0.0625 1` through `bin/rise`. The two successful sidecars name frame zero, source digest `17906b2d6c30e3a6e3a61abf3eb2a4d684c9f6e066c410a4be77ababd374d756`. The six PNGs passed SHA-256 and canonical provenance checks and their provenance links matched the EXR sidecars.

## Image comparison

RGB RMS uses all 294,912 samples on the M0 0–255 display scale. A single stochastic pair estimates the noise, so differences on either side of that measurement are expected.

| Comparison | Raw RMS | Denoised RMS |
|---|---:|---:|
| Historical base 1 vs base 2 | 41.4504 | 3.2795 |
| Cold legacy playback vs base 1 / base 2 | 41.5790 / 41.4200 | 3.3017 / 3.3405 |
| Native playback vs base 1 / base 2 | 41.5077 / 41.4782 | 3.3357 / 3.3700 |
| Missing-frame control vs base 1 / base 2 | 65.0283 / 65.0296 | 52.0167 / 52.1341 |

As a secondary diagnostic, resampling 96 paired 32 × 32 pixel blocks 5,000 times gave a 95% interval for the increase in RMS over the historical repeat. The native raw interval was [−0.091, 0.178] and the native denoised interval was [−0.013, 0.160]. For the missing-frame control the corresponding intervals were [16.35, 30.91] and [38.74, 58.16]. These intervals describe the spatial variation of this frame.

After switching the make CLI to the isolated import worker, a fresh cold import produced a byte-identical native manifest. A second complete render comparison yielded native RMS 41.5546 / 41.4755 raw and 3.3070 / 3.2784 denoised against the two references. The missing-frame control again separated at 65.0267 / 65.0289 raw and 51.8992 / 52.0145 denoised. All six PNG digest and provenance checks passed again. The second result set is in `rendered/simulation-m1/final-*`.

## Build and review

Independent implementation reviews led to fixes for directory durability during publish/retry, stale generation races, exact frame boundaries, bounded allocation, affine rank checks and predictive evidence binding. Focused sanitizer and warning-as-error tests covered cache publication and grid decoding. The provenance suite passed 303 checks, including the exact successful sequence wire record, missing-frame EXR/PNG linkage and refusal of contradictory reason codes. Actual power-loss persistence and Windows builds have not been measured.

## Using the cache

An existing scene keeps its `sequence_manifest` line. On first use the importer validates the legacy source, converts the selected frame and writes a native generation into the per-user simulation cache. Set `RISE_SIMULATION_CACHE_DIR` to choose an explicit cache root for CLI operation. A native fire scene can point `sequence_manifest` directly at the converted directory's `manifest.cbor`. A detached imported cache has preview fidelity.

For an explicit legacy import, run `bin/tools/simulation_cache_import legacy SOURCE CACHE ENVELOPE_SHA256 ORDINAL`. Supply `all` in the last argument to convert the full sequence. For a general grid sequence use `bin/tools/simulation_cache_import --vdb CACHE INTERVAL density=kg/m3 velocity=m/s -- FRAME0.vdb FRAME1.vdb`. Supply units for every grid and order the files by simulation time. The general import produces preview grid output. The fire medium also requires its fire-specific physical records.

The warning-free make build passed for the CLI, importer, and focused targets. `SimulationGridTest`, `SimulationCacheTest`, `SimulationVDBImportTest` and `FireCachePlaybackTest` passed. `SourceHygieneTest` scanned 478 files and passed 706 checks. Two OpenVDB versions (13.1 and 14.0) passed the bounded foreign-import tests. Independent review verified cumulative limits across grids and preservation of the installed frame on refusal. Nine archived VDB inputs passed preflight.

## macOS playback and release

The relocated smoke app exercised the production Objective-C++ GUI bridge with the renderer compiled without OpenVDB. Cold legacy import, warm reuse and native-only playback with the helper removed each rendered at 32 × 48. For scene times 0 and 0.0625 s, the sidecars selected ordinals 0 and 1 and matched the captured `65fd5624…` and `6182ad9a…` source digests. The two prepared generations were 2 and 3. The central 16 × 24 ROI had nonzero mean linear luminance in all six images. The relocated Mach-O closure passed verification with 24 images and an ad hoc deep signature. In a two-medium failure control, the second medium lost its committed payload. The render refused, the first medium retained identical baked channels, and its majorant and emission counters remained at 4. Restoring the payload let both media render ordinal 1.

The full SwiftUI Deployment target failed at macro expansion for `ChatPanel.swift`: `sandbox-exec: sandbox_apply: Operation not permitted`, followed by `external macro implementation type 'SwiftUIMacros.StateMacro' could not be found`. The bridge test establishes production playback behavior; at that point, launching the finished SwiftUI app remained unverified. The follow-up below supersedes that result. Separately, both locally available OpenVDB builds target macOS 27.0. Shipping the existing 26.2 deployment target requires a compatible OpenVDB build and matching dependency closure. The packaging check refuses a bundle with a higher minimum. Compatible existing TBB and Blosc prefixes can be selected alongside `OPENVDB_PREFIX`. No system software or deployment target was changed.

The make test runner now gives demand import a test-owned writable cache. Four small positive-control filesystem fixtures need a trusted temporary ancestor. The SSD mount is group-writable (0775), so the production publisher correctly refused staged writes under it. The runner gives these cases unique private temporary directories and removes them afterward. Cache, render and suite bulk data stayed on the SSD. All original assertions ran.

The dedicated `bin/tests/FireSequenceTest --fire-cache-playback-contract` passed. It covers a partial cache, rederived predictive evidence rejecting a transplanted frame and consistent forged receipt, recovery after a missing seal, and dense-equivalent playback. Two simultaneous importer processes publishing different tier-6 ordinals preserved both entries. Full replay sealed all three. Importer processes perform CPU file conversion only.

## Final fast tier

Ran the unchanged fast selection and timeout with `./run_all_tests.sh --tier fast --timeout 300` from an SSD-resident source mirror. The suite's safety rule requires its scratch directory to sit outside its checkout. The mirror used the current make inputs and the original branch's read-only Git metadata. All four record preflights passed. All 419 executables built; the bulk compiler/linker log contained zero warnings or errors. Runtime was 586.64 seconds, with 416 passed, three failed and zero skipped. The new cache tests, source hygiene and all four trusted-staging tests passed. The first diagnostic run was 411/419 and is preserved in `rendered/simulation-m1/verify/initial`.

| Failing executable | Exit | Observed output |
|---|---:|---|
| FireSequenceTest | 1 | The canonical loadability/preparation checks passed. The r275 conservation, r181 checkpoint retention and two r61 kill/resume checks failed. Child runs reported zero accepted steps and a working set above the Metal device limit. |
| FireProductionProjectionTest | 1 | 36 failures, including “r239 device burner honors fixed momentum and existing residual gate” and “P2 Metal tier-10 structural trial remains valid”. |
| FireProductionSolverTest | 139 | Reported CPU/Metal and native inlet failures, followed by “Segmentation fault: 11”. This run did not establish the location of the fault. |

As a baseline control, the archived r275 FireSequenceTest from revision `4356d2a27` (executable SHA-256 `de6b68c336157afcba00128c5c394ff40f99236d1933c7b43b2a3af9d959cfd1`) ran alone with the same fast-suite settings. It exited 1 in 95.29 seconds; the current binary exited 1 in 98 seconds. The four failing assertion strings and their counts matched exactly (8, 1, 1, 1). This establishes that those failures predate M1 in this environment. Only the fast-conservation failure was directly traced to the Metal device limit; the individual checkpoint failure causes remain unisolated. See `rendered/simulation-m1/verify/archived-reference/comparison.txt`.

The independent device probe returned `Metal device: (none)`. The suite transcript and complete individual failure outputs are at `rendered/simulation-m1/verify/fast-tier.log` and `rendered/simulation-m1/verify/rise-tests-logs-fast/`. No gate was waived or reclassified as passed. M2 requires an accepted M1.

Git refused staging with `Unable to create '.git/index.lock': Operation not permitted`. The tracked and new source files remain in the checkout on `fire-smoke-design`. The patch set under `rendered/simulation-m1/review/` is checked against the working tree using `git apply --check --reverse`.

## Follow-up on full app playback and the Git lock

On macOS 27.2.1, Xcode could compile all production Swift sources by using the installed macOS 26.5 SDK for Swift compilation. The command-line build setting was `OTHER_SWIFT_FLAGS=$(inherited) -Xfrontend -sdk -Xfrontend /Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk`. The app's deployment target stayed at 26.2. The default 27.2 SDK still requires the SwiftUI macro service that this environment cannot launch. Supplying the 26.5 SDK to Xcode for the entire build instead failed its Info.plist SDK registry lookup.

The full app initially aborted in AppKit registration before its existing headless viewport probe could run. The app entry now dispatches that explicit probe before entering SwiftUI. With the full production viewport controller attached, the first functional run exposed a separate failure: the controller installed an external progress callback that prepared renders refused. The controller now supplies a concrete progress object whose render-thread methods only access internal state. The UI polls it and delivers progress outside the render. Pause and cancellation stay available. A generation check prevents a late progress poll from cancelling the next render. The normal external-callback restriction remains in force.

The rebuilt app's `viewport_reattach` probe passed cold legacy load/render, native load/render, and native rerender after releasing the stale viewport bridge. The smoke scenes used the historical showcase input at 32 × 48, one sample. A relocated, ad-hoc-signed copy also passed both scene loads and all three renders. The cold run populated exactly one native frame. A second copy rendered both scenes from native storage after removing the importer and produced no import cache. The logs are `rendered/simulation-m1/git-followup/full-app-relocated.log` and `full-app-native-only.log`. This verifies the application executable and production viewport path. A visible window still needs a WindowServer session. The 26.2 release warning gate remains open because the available OpenVDB development packages target 27.0 and other linked dependencies target 27.0 or 27.2. A separate 12.1 OpenVDB runtime shipped inside DaVinci Resolve has an earlier deployment minimum, but this host has no matching development headers.

The previous signal 11 in FireProductionSolverTest was a test failure path. A missing Metal device left the remap flux arrays empty, and a byte-comparison assertion indexed them. That assertion now verifies the requested indices exist before reading the arrays. The test built without warnings, ran to completion with 47 failed checks and returned 1. The original byte comparisons still run when the remap produces values. No solver implementation or certified arithmetic changed.

Independent Git diagnosis found no existing lock. The repository and index are owned by the session user; the configured access policy explicitly marks this checkout's `.git` directory read-only. No alternate index, Git directory or worktree was configured. Writing commits requires a session with writable repository metadata or the local Git client.

To reproduce the full-app check, set `RISE_GUI_HEADLESS_PROBE=viewport_reattach`, `RISE_GUI_HEADLESS_PROBE_SCENE_A` to the legacy scene and `RISE_GUI_HEADLESS_PROBE_SCENE_B` to the native scene, then run the relocated application's `Contents/MacOS/RISE-GUI` executable. Set `RISE_MEDIA_PATH=/` for these absolute-path fixtures and `RISE_SIMULATION_CACHE_DIR` to a fresh writable SSD directory. The probe must return 0 and print `ViewportReattachProbe: ALL PASS`. For the native-only control use a separately signed bundle with no helper, point both scene variables at the native fixture, and verify the requested fresh import directory was never created.

After the full-app progress fix, `make -C build/make/rise -j3 all` and the focused test builds succeeded with zero warning diagnostics. The scene-editor tests passed all 70 checks and source hygiene passed all 706. The full fast tier rebuilt 419 executables and finished with 416 passed, three failed and zero skipped. All three failing executables now return 1, including FireProductionSolverTest, which previously exited 139. The runtime phase took 378 s with six run slots. The complete current transcript is `rendered/simulation-m1/git-followup/final/fast-tier.log`; individual failure output is in the sibling `rise-tests-logs-fast` directory.
