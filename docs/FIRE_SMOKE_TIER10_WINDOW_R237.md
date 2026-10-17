# r237 — tier-10 production window at the achieved cost

Operational launch decision, not a solver amendment. r236 is promoted at
`bc4a087e`; its next production run starts from zero. Tier-8 spectrum and
preview are already delivered, and the owner has accepted tier-10 survival
past the old runaway point as engineering validation. The remaining full
tier-10 empirical window must now run, without changing its duration, cadence,
physical gates or monitored acceptance.

r235 delivered no saving and r236 delivered only 1.10%. Stop these incremental
cost experiments for this run and use the achieved implementation. Its 3.274 s
measurement is tier-8 hot-input qualification, **not** a tier-10 step-time or
window estimate. The one-second/design targets remain unmet. Record actual
tier-10 device/wall times during the run; no under-hour claim is authorized.

## Storage and launch

The internal volume has about 30 GiB free, too little to confidently retain
the complete larger window plus its duplicated published frames while keeping
16 GiB free. Read-only inventory found `/Volumes/SSD` mounted with about
1.7 TiB free. Write new run payloads there, rather than delete old evidence,
shorten retention, split archives or change the solver. No existing external
files are overwritten or moved. Manifests and this entry remain in git; all
checkpoints, frames, logs and other payload bytes stay outside it.

Planned output: `/Volumes/SSD/RISE/fire_production_calibration/r237_tier10_full_window.v1`.
Launch only if that directory does not exist and the SSD mount remains the
observed writable volume with ample free space. Use the preserved qualified
executable in this worktree:
`rendered/fire_production_calibration/r236_endpoint_reuse/FireSequenceTest_d9397b6f`,
SHA256 `d9397b6fddc682819724e2a02ef3c26f1d692eff8ef7b753e2e5e8c62ca83655`.

Arguments: `--fire-production-temporal-capstone 10 0.0625 OUTPUT`.
The existing launcher derives the full five-flow-through discard plus full
statistics window and seals its case/receipt. No target, resume, diagnostic
mutation or shortened-prefix environment overrides. Monitored trajectories,
source ledgers and retained checkpoints stay enabled. Frames use the pinned
cadence and existing preview publication path; this does not claim spectral
final quality or any empirical row in advance.

Native RSS ceiling and graceful stop remain enabled; retain the operational
16-GiB disk reserve on the output volume as well. No builds, qualification,
oracle regeneration or rendering concurrently. A refusal is evidence to
diagnose, never permission to widen a bound. This unchanged-binary production
launch creates no migration certificate or new authority surface.

After completion: full spectra and empirical rows, temporal movie delivery,
then the spectral output and report queue. During this run, only light
read-only/report work proceeds; cost engineering resumes after the host is free.

## Launch confirmed

The qualified binary launched successfully from zero on 2026-09-13. First
accepted step: t=0.0016462659696117043 s, sampled RSS 1824505856 bytes,
compressed bytes zero, native ceiling 34359738368 bytes, stop=0. These are
startup observations, not a full-window memory or speed claim.

The immutable launcher receipt records statistics start 10.42654563611109 s,
duration 14.625 s and full target 25.05154563611109 s; cadence 0.0625 s.
Case ID `3e88f9ca9d13b186b70a66771960a92f32878bce49991fedf44de7272cf8a5ed`.
Receipt path: `OUTPUT/window_launch.v1`, SHA256
`45c81c6f062a732ca1dc87c26dab62de43e92bbcd943e9e9e7087811cdfbac16`.
The executable hash matches the preserved r236 measurement. The log is the
adjacent `r237_tier10_full_window.v1.log`; it remains mutable until the run
ends and is not assigned a final evidence hash prematurely.

The worker is running under `caffeinate`; no other heavy work may start.
Monitor native RSS, accepted-step trajectory, SSD free space and process state.

## Full window completed; preview rendering

The worker exited 0 on 2026-09-15: 16,298 accepted steps, final time
25.051545636115861 s, and 235 frames. The complete 14.625-s statistics
window is present. Actual wall time is 139561.69983512501 s (38.77 h),
not the design speed target. Peak/final velocity is 20.03506/6.75872 m/s;
final sampled RSS is 1,995,325,440 bytes with zero compression. The summary
reports zero hard-bound and physical-projection retries.

`production_capstone_summary.txt` SHA256:
`9702162e20b8cf74b4aaa4e3524df3c7d2b4f4b55ca5bd7facbc4b36dc7b7e42`.
Its puffing estimate remains preliminary pending the published full spectrum;
formal filtered-field composition is still pending.

After confirming no solver/render process remained, the preserved r236 binary
started the existing `--fire-production-temporal-preview` path against the
complete sequence. Output is `/Volumes/SSD/RISE/fire_production_calibration/r237_tier10_preview.v1`,
with the adjacent `.log`. This reuses the r227 preview settings and
`preview_primary` sidecars, not a final-quality spectral/HDR claim. The
existing GIF delay quantization remains disclosed; exact-rate encoding and
presentation-quality spectral output follow separately. Render session: 8511.
No competing heavy task may start while this sequential render runs.

## Preview and spectra published

The 235-frame preview completed with exit 0. Delivered GIF:
`/Volumes/SSD/RISE/fire_production_calibration/r237_tier10_preview.v1/temporal_fire_preview.gif`.
SHA256 `09127ba471e2938d1fcb996bafe34cec7b040cb8b642754bd90098d2eb3e0459`;
provenance sidecar SHA256
`49fdc98b647ed353dffa83a6cc7e68de3b2613834b75666caaffece0ce94b14a`.
This is a 64x64 dim blue preview, not final spectral presentation. The GIF
retains the existing 60-ms delay quantization (about 4% fast). Primary EXRs,
display PNGs and their sidecars remain alongside it. Tiny negative-radiance
warnings remain in the render log for the spectral-path audit.

The existing full-spectrum exporter ran on the tier-10 final checkpoint and
all rendered frames. Centerline HRR peaks at 3.0709134615387481 Hz; the
expected-frequency bin/peak power ratio is 0.10918730852037768. Display lit
area instead peaks at 0.13648504273505546 Hz, with expected-bin/peak ratio
0.0069063768380361514. These disagree: do not call this a joint puffing pass
or absence of the expected component. The slow-mode/open-boundary audit is
still relevant, as is the dim preview's thresholded lit-area definition.
Full spectra and definitions are under
`/Volumes/SSD/RISE/fire_production_calibration/r237_tier10_spectrum.v1`.
The full-spectrum CSV SHA256 is
`3d62cc890bb3ffaf7232b0bf1e6a1d1b3ba6cc134f0fdd0bbf8d5f17a32735f4`;
measurement-definition artifact SHA256 is
`783ba312d1911f1660469584ff1b1089a2ebfe944da03f9ad346b1aed0dde4d1`.

## Spectral-label prerequisite and final-state diagnostic

The delivered preview already executes `pathtracing_spectral_rasterizer` on
sequence-backed media. Spectral execution does not itself authorize a final
label. Inspection of the actual sealed sequence finds `source_qualification`
`preview_only`, `synthetic_chem_fixture`, and preview-only fuel/transport
records. The producer in `tests/FireSequenceTest.cpp` explicitly writes those
qualifications. Do not remove them, swap a label, or present higher samples as
predictive qualification. Resolving the record prerequisites is necessary
before a final-labelled artifact; higher-quality preview output can proceed
without misrepresenting them.

Existing final-checkpoint physics diagnostic exited 0. Payload
`/Volumes/SSD/RISE/fire_production_calibration/r237_final_physics.v1`, SHA256
`8f4f945efff99bfec6fe550db49cfc41959a80f1bdfc9d9fc88b9d76f4391639`.
At the final state, its source re-evaluation gives 22653.44838 W versus
22653.45085 W consumption-times-LHV, relative difference 1.09058e-7. This is
a final-state probe, NOT the integrated empirical ledger row. Maximum
reduced gravity is 49.96550 m/s2 and minimum gas density 0.19227986 kg/m3
at 1726.49244 K. This endpoint alone cannot attribute the spectral slow mode
to boundary entrainment. Missing wall histories in format13 are not zero
costs; the separate per-step trajectory remains authoritative for timing.

## Retained empirical rows — independently checked

Reporting-only commit `eeb1ca03` exposes the checkpoint's retained integrals;
no solver arithmetic or sealed run changed. The focused build completed with
no warnings, extraction exited 0, and all pre-existing diagnostic lines are
byte-identical to the preserved executable's output. Raw extraction:
`/Volumes/SSD/RISE/fire_production_calibration/r237_retained_statistics.v2`,
SHA256 `dd96796e7f7bf6e776b234432e70d1dc40878929f60ad04fef06ff4adea0b1b3`.
Its checkpoint SHA is
`b264cb1f3d296349aaa3e83b851c264cbca018ea25f2e963801d657dd2575719`.

Checkpoint derived summaries precede finalization: their zero McCaffrey and
radiative-fraction fields are NOT observations of zero error or radiation.
Reconstruct from persisted station integrals and 14.625000000004771 s
duration using the existing finalization formulas. Of 132 stations, 94 pass
the existing height selection: above max(flame tip, 0.2*33^0.4), below the
domain top minus half D*. D*=0.24474492853928809 m, flame tip
0.33040565352803891 m. No height or tolerance was changed.

| Quantity | Measured | Original gate | Result |
|---|---:|---:|---|
| McCaffrey maximum temperature-rise relative error | 45.0474814% | <=10% | FAIL |
| McCaffrey maximum velocity relative error | 28.1368465% | <=10% | FAIL |
| HRR versus consumption*LHV relative discrepancy | 7.82031e-11 | <=2e-10 | PASS |
| Integrated radiative fraction | 0.1986850834 | 0.18–0.22, within 0.07–0.28 | PASS |

Both McCaffrey maxima occur at station 126, height 3.0960233460 m.
HRR is 278347.38006772392 J; consumption*record-LHV is
278347.38004595629 J. These are full-window integrals, not endpoint probes.
Centerline puffing's 3.07091346 Hz is 12.1339% above the 2.73861279 Hz law
(within the original 20% frequency gate), but the separate slow display-area
mode and formal contract remain unresolved; no overall capstone pass.

The exact statistics event-split assertion also fails: the first contributing
step starts 0.000233053194510 s before the boundary. Accumulation explicitly
clips its duration to the boundary; the statistics window is not extended or
shortened. Record the event-split failure separately rather than asserting
exact step-boundary equality.

Fresh independent extraction review found no P1/P2 in the reporting change;
the independent arithmetic review reproduced the 94 stations and all four
numbers above, and flagged the failed physical rows and event-split assertion.
These are measured findings, not permission to widen physical gates. Their
cause is not yet assigned to boundary entrainment, manifold drift, or the
transport discretization. Final-qualified claims remain blocked.

### First causal discriminants (not a remedy)

The full-window mean realized HRR is 19.03229949 kW, only 57.6736% of the
authored 33-kW reference. Passing the consumption-times-LHV ledger means heat
matches consumed fuel; it does not mean all supplied fuel burned or the
reference HRR was realized. Do not normalize the empirical 33-kW gate down to
this measured heat release. The next audit must distinguish unburned-fuel
inventory/escape, source realization, and mixing/entrainment before assigning
the low centerline temperature and velocity to an outlet defect.

The existing domain derivation matches r54: lateral source width plus 6D,
and top 5*effective flame height for the plume-law case, rounded upward to
whole cells. There is no multiplier conformance error identified by this
source check. This does not prove the domain is physically adequate.
At z=0.99122 m, observed temperature rise/velocity are 174.26 K / 2.746 m/s
versus 243.27 K / 3.635 m/s; at z=3.09602 m they are 20.03 K / 1.787 m/s
versus 36.45 K / 2.487 m/s. The deficit exists below the top station as well.

The whole-run injection ledger is consistent with the specified source:
0.016524976039961423 kg divided by 25.051545636115861 s, times the record
LHV, gives 32999.99999999983 W. Do not compare this whole-run mass directly
to statistics-only consumption. For the constant source, the 14.625-s
reference injection is 0.00964722009950633 kg versus 0.005563902490389592 kg
consumed. This narrows the deficit beyond simple under-injection, but does
not partition unburned fuel between inventory change and boundary escape.
Source inspection confirms ambient inflow/interior outflow scalar ghosts
in the resident FCT boundary path; it does not measure entrainment rates.

### Matched-interval fuel inventory

Read-only inventory extraction `a5975636` sums methane density times cell
volume. Focused build passed without warnings; all prior output lines remain
byte-identical. Independent review found no P1/P2 in the extraction or the
following balance. From retained step 6489 (t=10.465922561998013 s) to the
final checkpoint (t=25.051545636115861 s): nominal injection is
0.009621245544236105 kg, consumed increment 0.005551167665323289 kg,
inventory change -0.000036208781316426 kg. Final inventory is
0.000652769803797029 kg. Storage therefore does not explain the heat-release
deficit; inventory decreases slightly.

The remaining 0.004106286660229242 kg is an **unaccounted mass-balance
residual**, consistent with escape but not a measured boundary flux. Do not
rename it export until actual flux accounting separates boundary transport
from conservation error or nominal-versus-applied injection discrepancy.
This interval is a diagnostic subset of the completed run, not a replacement
empirical statistics window.

Payloads under `/Volumes/SSD/RISE/fire_production_calibration/`:
`r237_inventory_6489.v1` SHA256
`cfe6d0e3229aa5ee008327eb4a26c6d08e1a4f80343afadaf66e7d79147770d5`;
`r237_inventory_final.v1` SHA256
`c201754213812712428b29fb44d93f489802004b65e1b3102ca16f6c6f8c0c2b`.
The earlier checkpoint SHA is
`68421f97b1035a6788b134c5c1e1ce65fddc9330aeb20ed929298c27c77345ae`.

### Display-spectrum color-encoding defect localized

A raw-pixel cross-check does not reproduce the published ImageIO lit-area
counts. The 16-bit `temporal_display_00000.png` declares gamma=1.0 (sips:
linear sRGB), although `PNGWriter::WritePixel` integerizes the requested
sRGB transfer for 16-bit channels. `PNGWriter::EndWrite` unconditionally
writes gamma=1.0 for bpp16, bypassing its color-space switch. ImageIO then
converts these already encoded values as if linear into the sRGB counting
context. This is an output metadata defect, not evidence of boundary physics.

Exploratory raw-code-value threshold 127 with the same 512-point affine/Hann
spectrum yields 1.43309 Hz rather than the published 0.136485 Hz; expected-bin
power/peak is 0.66283. Raw decoding is not substituted silently for the pinned
color-managed observable. Preserve the original spectrum/movie and fix the
encoder metadata with regression coverage before publishing a corrected
version. The centerline HRR spectrum and physical McCaffrey failures do not
depend on these PNGs and remain unchanged. Other raw thresholds retain slow
power, so even the corrected image measurement will not alone prove or
disprove domain recirculation.

### PNG correction qualification

Commits `515f9a86` and `062861c0` remove the 16-bit-only linear-gamma override
and declare exact sRGB (with standard fallback metadata). The old encoder
fails the new matrix in exactly two cases: 16-bit sRGB and ProPhoto. Fixed
encoder tests: 118 pass / 0 fail; output-shim tests: 275 / 0. The tests
inspect metadata, not a complete color-managed rendering roundtrip. Source
review confirms no pixel conversion/buffer changes; historical IDAT identity
has not been separately asserted.

Clean make passed; clean Xcode Deployment and shipping Opto both passed with
only the known absent-local-OIDN search-path and AppIntents notices. Fresh
review found no P1/P2 in fire/sRGB scope after exact-transfer tagging was
added. Wider ProPhoto ICC/primaries work is a backlog item under the owner's
infrastructure cap, not claimed fixed by the gamma correction.
Build/test logs are retained in `rendered/fire_production_calibration/r237_png_correction/`:
RED SHA `67d746cd5531cf7b8e0dde8594d3bf01b0f782b1698630c0618a2014420c1ee9`;
encoder GREEN `89660205770fe3cc97ad761391f8175d5b89b8d175af6031ffc7cebbe49c4aac`;
shim GREEN `d77ccd5987959d514b0e3de302eb468d117aacc5790b98826807aacb75d98fb9`;
clean make `e0612ce2979f69ff5be3dafba6e56470383d512c1d52f951f0be0c585ad6da29`;
Deployment `7b1270522cbd6826846156a0e85acf21e4ee51e8ca93f17ddf97186df4008433`;
Opto `0ccf09a48988d72556f8ae80ff98e64f1d230b65423a1aa0cc52e5dc0dab0ef2`.

Corrected preview v2 launched on 2026-09-15 using preserved
`r237_png_correction/FireSequenceTest_5e964fe0`, executable SHA256
`5e964fe0a1e1b4a87bb093bdf088270b05eb3a96a0c45979ef08a950ca5d0877`.
Output `/Volumes/SSD/RISE/fire_production_calibration/r237_tier10_preview.v2`
and adjacent log; session 8933. All 235 original simulation frames are used.
No solver rerun, old artifact overwrite, or final-fidelity promotion. Wait
for this sequential render before any heavy build or diagnostic job.

### Corrected preview and spectrum v2 delivered

The v2 render exited 0 with 235 frames. PNG inspection now reports exact sRGB,
gamma 0.45455 and standard sRGB chromaticities. GIF readback: 235 frames,
64x64, 60-ms delays (the disclosed approximately 4% fast GIF cadence remains).
Frames 0 and 117 differ; simulation sequence identity is unchanged.

`r237_tier10_preview.v2/temporal_fire_preview.gif` SHA256
`b823e0a9eca848a8309f038140eb5296870b639f98d951dafc7422bf405f480e`;
sidecar SHA256 `70a391e77ca0db3205e2587c50c7343b45838dbd0822379604f1578ab17a243f`.
`r237_tier10_spectrum.v2/full_spectrum.csv` SHA256
`03381c849cf962ef239b051d1f30fb2aed156d7aaf1fcbee9da2cfbf1f2b7fce`.
All paths are under `/Volumes/SSD/RISE/fire_production_calibration/`.

The same color-managed counting protocol now gives lit-area peak
1.4330929487180823 Hz and expected-bin/peak power 0.6628321034094754,
not v1's 0.136485 Hz and 0.006906 ratio. Centerline HRR remains exactly
3.0709134615387481 Hz with expected-bin/peak power 0.10918730852037768.
Thus v1's dominant slow lit-area mode was not valid boundary-physics
evidence. Slow power may remain; neither this correction nor the preview
improves the unchanged McCaffrey failures or qualifies final fidelity.

### Exact-cadence MOV

The corrected 235 PNGs now also have a native H.264 MOV derivative:
`r237_tier10_preview.v2/temporal_fire_preview_16fps.v3.mov`, SHA256
`b6ad27ec7f2fc73bbdee1f6e5afeec01987b173d2123dddb549425c1af5cce18`.
All decoded presentation timestamps equal n/16 exactly; nominal rate is
16 fps and container duration is exactly 14.6875 s, including a final
1/16-s frame hold after the 14.625-s sampled span. This removes the GIF's
cadence approximation without changing the simulation or source frames.
Track and movie timescales are both 16000: earlier retained candidates
exposed default-600-timescale rounding in PTS and then container duration.
Frame 117 was inspected for orientation. This is a 64x64 SDR preview,
not final-qualified media, and lossy movie pixels are not measurement inputs.
The animation-render skill's decode/metadata verification was applied;
no HDR claim is made. Source-frame and sidecar hashes plus retained recipe
hashes are in `r237_png_correction/exact_cadence_movie.v3.manifest`.

### Fuel-accounting observability audit

Read-only source audit at `0a2534de`: the reported `externalFuelMassKG`
accumulates prescribed massFlux*dt*faceArea during source staging, then
publishes only on acceptance (`FireSequenceTest.cpp`, source staging and
accepted-state statistics). It is not a sum of represented post-staging
CH4 increments: the staged tuple is rounded to binary32 after adding the
source. Therefore its agreement with 33 kW verifies the prescribed ledger,
not an independently measured applied-injection balance. No missing source
application was identified: staged species enter `packetBeginning`, which
is consumed by the canonical source producer and production request.

The retained checkpoint serializer does not carry `externalFuelMassKG`,
and the monitored CSV contains manifold/timing fields, not boundary species
fluxes. The retained threshold-15 source ledger contains reaction-packet
increments, not an integrated open-boundary export ledger. These surfaces
cannot turn the 4.106-g residual into a measured export after the fact.
A retained-state boundary-rate diagnostic can localize likely routes, but
must remain instantaneous and cannot be substituted for the missing
full-window flux integral. No solver rerun or kernel change made in this
audit; next instrumentation should measure applied staging and accepted
boundary flux separately, without adding a provenance authority surface.

Source SHA256 (`tests/FireSequenceTest.cpp`):
`cb03199ace355d2f3561913b7adaec30951748a9edd927a558d06bce63520af0`.
Retained threshold-15 source-ledger SHA256:
`fc4822acdb6bb1a2e352e5ab5a762eb03fb0eee3e0e1ff785c4b167fdb4367dc`.

### Retained-event staging and boundary probe (diagnostic only)

The SHA-pinned threshold-15 artifact at t=21.239038494098168 s contains
both unstaged and staged tuples. Their CH4 difference, summed using the
original double-precision cell volume, is 4.710973696019475e-9 kg versus
prescribed 4.7111502752253045e-9 kg for dt=7.1420131462218706e-6 s.
Exactly 120 cells change; none has a negative increment, and all changes
are in the bottom layer. This directly measures staging at this event,
not full-window applied injection, and identifies no large injection loss
at this event. The small difference includes binary32 representation.

Using this artifact's **frozen input velocity**, staged adjacent-cell CH4
density times positive outward velocity and original face area gives a
top donor-rate diagnostic 2.1225768807326377e-4 kg/s; bottom 5.933618766268854e-7
kg/s; each lateral side is below 2e-8 kg/s. This localizes a plausible
top-boundary route. It is NOT the accepted FCT flux, net export, or a
full-window integral, and does not close the 4.106-g residual or establish
entrainment starvation. Geometry uses original dx; the stored reaction
ledger uses binary32 request dx, so exact equality of their integrated
reaction numbers is not claimed.

Read-only exploratory decoder and JSON are retained outside git under
`r237_png_correction/`: `retained_source_probe.py` SHA256
`63de836b6aeb4ea431bfb95c8686b4fada80ff3641ce1b079344e06c8ae03d45`;
`retained_source_probe.v1.json` SHA256
`8830462185af2cfa95b7a9fde342b072e8d427bedf3fdd924d4ee256d3aacd26`.
Input SHA256 `8bc89865f7a59993cee273ee322d38894a6baa60deb194275ccc0a893cf07f8f`.
Independent layout/units and boundary/scope reviews found no material
P1/P2 in the pinned-event calculation. This is a diagnostic recipe, not a
promoted production implementation or a reusable qualified flux observer.
