# M0 baseline results, 2026-11-22

The owner authorized historical certified-path artifacts. The reference corpus and M2 extraction inventory are ready. The forced make rebuild passed. The current-session full fast tier and GUI build remain open. The recorded source baseline is d34f03024bbbe3a645876c25f7fcde6b93478157 on fire-smoke-design.

## Reference selection

The external SSD copy is at `/Volumes/SSD/Working/RISE/rendered/simulation-m0/historical/` (ignored by git). It holds 61 payload and provenance files, 415,726,010 bytes before indexes. Its SHA256SUMS index hashes to `5e3e4609a2a3d1d8025c0ecb02d79f05c4ef7ef9301ee8e1767f4b746155b1c5`. sources.tsv maps copies to their historical locations. The tier-8 and render-input manifests describe their original full sequences. Use the original directories for full preflight; the retained subset is a comparison corpus.

| Tier | Retained window | Provenance and scope |
|---|---|---|
| 6 | r270 steps 1340 and 1466, followed by r273 frames at 2.5000, 2.5625 and 2.6250 s | The r270 floor-emissivity-1 run produced the checkpoints. r273 t6_y300_off resumed step 1466 at 2.4122076039202511 s with methane soot yield 0.3, default stencil, cap and sheet options unset. Its result.txt binds that exact input path and case. The checkpoint launch records executable 31578eefbc5c2bad5e99e43326eea3c1ff39c70fa61fc54a5f3d94f6da2707c7. The continuation used the historical A/B driver. Its exact binary revision and a post-continuation checkpoint are unverified. Compare the two checkpoints on the r270 trajectory and the three frames on the resumed r273 trajectory. |
| 8 | r273 steps 6339, 6375, 6409 and 6443 bracket frames at 10.466040127212182, 10.528540127212182 and 10.591040127212182 s | tier8_y300_sa, source receipt 556c916d31754af6f8ff1ec011b4d1afc67618a2, executable a442a39d4d28445c1111f92adfb8065d376e5b1c247edba01a5c00bc782e5fe8. Pinned run_tier8_showcase.sh contains the seven model overrides. The run completed 15,255 accepted steps with zero hard-bound or projection retries. Its scientific contract verdict remained pending equal-time composition. |

Both runs predate fast mode. They measure reproducibility of the recorded configurations. Neither establishes default-case equivalence across the historical and current producers or predictive fidelity. Tier-8 includes the sealed native fire manifest with all 235 frame digests; the reference copy pins three payloads.

## Verification

The collection checked whole-file SHA-256 and size against the publication sidecars, case id against the launch receipt, and frame hash against the tier-6 .time or tier-8 frame_times.csv. It rehashed the copied files. The checkpoint comparison helper verifies both v13 payload checksums before computing a normalized SHA-256. It zeroes only the 64 producer-id bytes, 8-byte envelope checksum and 8-byte trailing payload binding. Header sizes, case id, accepted-state digest and every other byte remain in the comparison.

From the reference directory, run `shasum -a 256 -c SHA256SUMS`. From the checkout, run `python3 tools/fire/m0_checkpoint_digest.py rendered/simulation-m0/historical/tier{6,8}/*.checkpoint`. The helper was also checked with a producer-only variation, a state change with valid new checksums, and four separate corruption probes.

| File | Raw SHA-256 | Build-normalized SHA-256 |
|---|---|---|
| tier6/frame_00000.vdb | `2694796d68f0eef9121fc8f55c4e0f5f3b11db9fef70dd5579fdf2a326564370` | whole file |
| tier6/frame_00001.vdb | `68a51830109c44fae7ea9b34c066bd7efe703f64ad85cea91261a3b9f635c308` | whole file |
| tier6/frame_00002.vdb | `33276e2517160b02949afc02bc21e9a707228d1eb887c8a6a76f7843887b83c8` | whole file |
| tier6/step_0000001340.checkpoint | `49e8f2ec663b1d8210096ccf046dbe8844c9b3b7056fec440b7135ce9e3ef656` | `0e80821545f72d662efa830382e8f09d64aede8fdc25e7cfeb2d5f619e02f8f9` |
| tier6/step_0000001466.checkpoint | `fb9f097df156a591b38dad4202d88eaacf85c367128ee107ef919179ab8505e1` | `8e04b800f1b1fd5e5c59d2aa124cef01be498649a399f182264fd8c7df7365df` |
| tier8/frame_00000.vdb | `65fd56242f9134d5cc2d6afe91bec78f7546ef0a2b661c4fcf43a016753e9062` | whole file |
| tier8/frame_00001.vdb | `6182ad9a0ca84e04695032bb775e18210c0f5d9eb23925ad7eadb15edc6c1c0a` | whole file |
| tier8/frame_00002.vdb | `c295ea5640a09320017103f61e8662bdd8447e0cc5570e94f65bfde594fb6083` | whole file |
| tier8/step_0000006339.checkpoint | `2f58b779e639c8b0695e6e58e3e71cc39ed71a0c20d769aed22679c4f7f84614` | `0c8cf0bc47253842b647261ce588988cd1c7caaeb90092cecb7dc3c9532ec092` |
| tier8/step_0000006375.checkpoint | `56d0b8375a1cf46a99f6f2b90753074ebaecc3ac3b5f2dcfc34375cb1392208c` | `e21df52091677f5d5bc18719d6aa4be29ec5b31df01d588b78e67cfea8c72b6e` |
| tier8/step_0000006409.checkpoint | `204dc9ab0f2c8c3c6aa7ccf372ab74b03eb5e8115d041669131e984ae78c178e` | `183ecbcca9d147b477965a1543e22e3dba99d748a54d7a752a7829dd27096047` |
| tier8/step_0000006443.checkpoint | `2fc001c1b0127ae9b92d72602b1f9714e9b8e657e1fbc96d9745a419ee52b977` | `ac8d39327f359b615cf2f78ee68bce551d2fc940aa34199f91bfe20eda0a9fc4` |

## Render spread

Two independently rendered 256 × 384, 16-sample frames from render_speed/base1 and base2 share renderer build 3dbc21c513afe7b53637104075bc229ceb031ca46eaa0c768b6cda45fe33e022. Their scenes match after substitution of the two output paths. Both provenance records identify the same derived showcase VDB, SHA-256 17906b2d6c30e3a6e3a61abf3eb2a4d684c9f6e066c410a4be77ababd374d756 (source frame zero, 10.466040127212182 s). Preserve that derived input for the image comparison; the raw solver VDB has a different digest. Both PNG pairs and EXR pairs are retained with their provenance. PNG artifact hashes and EXR hashes with provenance attributes stripped match their sidecars.

The measurement decodes all 294,912 RGB samples per 16-bit PNG, excludes alpha and divides by 257 to express differences on a 0–255 display scale. This is a paired observation at one frame and sampling rate.

| Output | Pair RMS | Signed mean A minus B |
|---|---:|---:|
| Raw display | 41.450400 | −0.018185 |
| OIDN display | 3.279459 | +0.030870 |

## Build and suite status

- Current incremental `make -C build/make/rise -j8 all` exited 0, with zero warning diagnostics. CLI digest c1d8b8d7db757f3cbbd672397ae7fa1f4ccfe9f6fdf585f18f7101ce0457110c. Observed existing test executable digest ec2c1ef037f7b99e037fb3830c71efa6ee989913e435eedfe8c88809dc22f8b1.
- Historical r275 follow-mode fast log (17:44 Pacific) records 415 built, 415 passed, zero failed/skipped, 289 s wall with 16 jobs, including FireSequenceTest (112 s). Copy: rendered/simulation-m0/historical-fast.log. Its log does not pin a Git revision. e07beb496 and the baseline HEAD differ only in two proposal documents. A fresh full-tier run is outstanding: the host's fast solver and follow-render are active, the suite contains exclusive benchmarks, and MTLCreateSystemDefaultDevice returned no device in this execution session.
- Fresh Deployment Xcode build failed before compilation: “Couldn't create workspace arena folder” / “Unable to write to info file” in the requested derived-data directory. xcodebuild also reports “Operation not permitted” while saving its log manifest. See rendered/simulation-m0/xcode.log. A later retry advanced beyond this preparation failure; see below.
- [M2 extraction inventory](SIMULATION_M0_DRIVER_INVENTORY.md) complete. [M1 cache design](SIMULATION_M1_CACHE_DESIGN.md) passed independent review. The owner has authorized proceeding after review.

### Current-session verification after design review

- Forced all make targets to rebuild via `make -C build/make/rise -B -j4 all`. Exit 0, 390 compilation messages and zero warning/error diagnostics. Existing host render jobs kept their own executable snapshots.
- Ran the fast runner’s preflight section through its final record generator, with the same arguments as the runner: all four parity checks and all 42 Python tests passed. Rebuilt and ran `SourceHygieneTest`: 474 files scanned, 705 checks passed, zero failed. These results are separate from the historical 415-executable suite.
- Retried Xcode with temporary metadata and SSD-resident object files. The build reached Swift compilation. The compiler’s macro service reported `sandbox-exec: sandbox_apply: Operation not permitted` and `swift-plugin-server produced malformed response` for SwiftUI.StateMacro. Exit 65. Detailed log at rendered/simulation-m0/xcode-work/build-objects.log. The GUI warning gate remains open.
- The Metal probe still returned no device. The macOS branch of FireProductionSolverTest requires a device and a successful identity-kernel dispatch (tests/FireProductionSolverTest.cpp, check beginning at line 4430). The host’s existing GPU run was still advancing, at step 9108 on the last observation. No concurrent GPU solver or benchmark was launched.

2026-11-23: Owner authorized proceeding to M1 with the two environment gates deferred to M1 acceptance.
