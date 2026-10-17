# r227 — temporal preview output-path repair

The complete r226 tier-8 sequence is preserved and verified (r226 completion
record, commit 15892e0a). Rendering, not simulation, is blocked: the temporal
scene authors an absolute output pattern while `RISE_MEDIA_PATH` already
names its staging directory. The output consumer concatenates both, creating
a nonexistent doubled path and refusing provenance publication (exit 134).

Use media-root-relative `temporal_primary` and `temporal_display` patterns,
matching the existing first-light sibling. Keep the absolute source manifest
and all frame readers/copies unchanged. No solver arithmetic, kernel,
rendering parameters, provenance gates, or simulation payload changes.
No rerun or migration of simulation is necessary for this rendering-only fix.

Regression: the full 235-frame preview must publish its verified EXR/display
pairs and GIF sidecar; the old executable's failed output-path attempt remains
in `rendered/fire_production_calibration/r226_full_window/tier8_preview.v1.log`.
The first-light sibling already uses relative patterns. Downstream readers
expect exactly staging-directory/filename, so they need no change.

Qualification and movie publication are pending. This is a path-construction
fix, not a new authority surface. Broader output-path API changes are outside
the movie sprint.

## Published preview and full spectrum

Commit `28edd5dd` compiled and linked without warnings in the focused build.
The complete temporal rendering regression exited 0 and published 235 primary
EXRs, 235 display PNGs, their provenance sidecars, and the GIF. Readback shows
64x64, 235 GIF frames; frames 0 and 117 differ over pixel bounding box
(26,0)-(40,64). The existing GIF encoder quantizes every delay to 60 ms, not
the physical 62.5 ms cadence: playback is about 4% fast. Simulation frame times
are unchanged. This is a low-resolution, blue/dim preview, not a final-quality
spectral movie or exact-rate video. Do not confuse preview delivery with that
remaining deliverable.

Under `rendered/fire_production_calibration/r226_full_window/`:

- `tier8_preview.v2/temporal_fire_preview.gif` SHA256
  `f65d5e23d66abee9523d7ba89dcb4c2fde952f4f009d73193467ab7f5a2d2004`;
  provenance sidecar SHA256
  `99cfce1dc0a092a350f8ac4f65c39107c3e4975e69b2ff4fe166573507058dff`.
- Successful render log `tier8_preview.v2.log` SHA256
  `956bbdb090486f33f7655eef3a9590717bbe1dca427c4dad98caae2f4ea645e9`.
- `tier8_spectrum.v1/full_spectrum.csv` SHA256
  `c43c5522b07c36589be5bc0779bc7ebfd8c44672b1f0fa217951ef5ddfb5f0d9`.
- `tier8_spectrum.v1/puffing_spectrum_evidence.v1` SHA256
  `fcdd99e0561e877ad9f963b299ded5843d9bc0308340f50d03200c6ff7aa5269`.

The spectrum uses the central 2x2 full-height cell-volume-integrated,
four-column-averaged heat release and display lit pixels (max sRGB channel
>127). Each complete observation span is linearly resampled to 512 points,
least-squares affine detrended, symmetric-Hann windowed, then a full
nonnegative-bin DFT is published. These are the existing measurement
definitions, not a new peak-picking fit. Upsampling the 16 fps lit-area signal
does not create new information above its original Nyquist frequency.

| Observable | Dominant Hz | 2.72970 Hz / peak power | 0.136485 Hz / peak power |
|---|---:|---:|---:|
| Centerline heat release | 1.5013354700770674 | 0.28924323329950347 | 0.047578811181038454 |
| Display lit area | 1.4330929487099278 | 0.20439351531644961 | 0.32712139657595496 |

The expected-frequency bin has 1.76977% / 1.24917% of positive-frequency
power respectively. It is present, subdominant to a ~1.5 Hz peak, and no
longer ~1000x below the dominant mode reported at tier 6. The old slow domain
mode does not dominate either channel; its lit-area contribution remains
material. The pre-registered strengthening route supports proceeding to
tier 10, not claiming the empirical row at tier 8. A causal resolution-only
conclusion is not justified: the momentum operator also changed across the
historical tier-6 and current tier-8 runs. The formal filtered contract and
final spectral-quality output remain pending.
