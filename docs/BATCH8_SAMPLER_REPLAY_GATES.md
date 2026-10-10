# Batch 8 DL-367 complete-render replay

Partial harness correction. Topology U and its replay probe now reset the complete-render RNG, including camera randomness, alongside SobolSamplerTestHooks::ValueSalt. Other topology probes retain their existing RNG policy. Reseed resets the generator in place; it avoids copying Mersenne's internal cursor. No transport, merge-radius, cap, scene or frozen-feature implementation changes.

Single-thread replay at 64 spp, eight distinct salts per PT / VM-on pipeline, two renders per salt: red 32 passed/16 failed (rc1), every identical-seed pair had different pixel hashes. PT paired mean difference -6.79724157e-6, sample SD 5.43725082e-5; VM-on -1.49228518e-5, SD 5.17902425e-5. Green 48/0 rc0: all pairs match every pixel, mean differences and SD zero; different salts still give different hashes. `--dl367-replay-only` requires force_number_of_threads 1 and rejects extra arguments.

All physical probes use eight ValueSalt replicates, sample SD (n-1), unchanged three combined SE and unchanged default 0.8% physical limit. render_thread_reserve_count 0; fixed-radius probes require vcm_disable_progressive_radius true.

| Probe | PT mean / SD | VM-on mean / SD | VM-off mean / SD | on/PT | Gate |
|---|---|---|---|---|---|
| 2048 spp, matched batching, progressive radius | .0356987129 / 3.30759514e-5 | .0357018402 / 3.30940312e-5 | .0357012349 / 2.92469369e-5 | 1.0000876 | 28/0 rc0 |
| 8192 spp, matched batching, radius .02 | .0357491716 / 9.66367448e-6 | .0357423321 / 1.75109906e-5 | .0357572234 / 1.80647175e-5 | .99980868 | 28/0 rc0 |
| 8192 spp, original unmatched batching, radius .02 | .0356833346 / 9.69374891e-6 | .0357423321 / 1.75109906e-5 | .0356911862 / 1.56415666e-5 | 1.0016534 | 27/1 rc1, retained strict residual |

VM-on gives the same reported aggregate mean and SD in the matched/unmatched controls. The reference PT and VM-off means move with batching. This narrows the residual; it does not establish which finite-budget result is the physical truth. The strict unmatched VM/PT assertion fails with absolute difference 5.89975e-5 versus combined 3SE 2.12292017e-5. No band is widened or recentered.

Named builds and runs: VCMStrategyBalanceTest (modes above); VCMRecurrenceTest 103/0, VCMEyePostPassTest 38/0, VCMLightPostPassTest 72/0. Build rc0, no compiler warnings; 192 library translation units recompiled after the shared header change. An intermediate unmatched run using a temporary RNG assignment was stopped at its verified own PID and supplies no result; the table uses the completed in-place reseed run.

Self-review: the correction is scoped to U/replay; in-place reseeding sets Mersenne left=1 so the next draw refreshes its cursor; replay restores salt/base/index/mode hooks; no sampling density, MIS weight or throughput changes. This does not repair generator copies, which have a separate deterministic cursor-ownership defect found during this audit (reserved DL-536). Native batching/spp dependence remains open. No performance claim.
