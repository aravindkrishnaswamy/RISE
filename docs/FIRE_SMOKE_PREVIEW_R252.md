# r252: r251 preview through the r242 scene

2026-10-19. Render-only deliverable; no new simulation. The owner selected a
quick **128×192** preview after the original r242 resolution was started.
That larger trial was stopped before its first frame completed, then the same
output directory was reused. Trial launch/log receipts remain outside git.

`tools/fire/r252_r251_preview.RISEscene` is the existing r242 scene with only
sequence and output paths changed, plus its identifying comment corrected. The recipe applies the owner's resolution
with CLI overrides. Spectral sampling (16 spp, 32 wavelengths, HWSS), box filter,
OIDN, camera, PNG exposure 7 and ACES display mapping are unchanged. It renders
all 235 retained frames of the r251 statistics sequence; no time resampling or
fewer-frame substitution is used. The first quick-preview frame took 24.69 s,
indicating about 100 minutes for the sequence, plus load and encoding.

The reusable r242 encoder/verifier use the current AVFoundation receiver/provider
APIs (macOS 26+), with warning-free encoding/decoding on this macOS 27 host.
They also accept optional dimensions, defaulting
to their original 512×768. The verifier's decoded snapshot goes beside the
movie instead of into the old r242 campaign directory. Encoding uses the
**denoised** PNG frames, H.264 in MOV at 16 fps. This is an SDR preview, not an
HDR master. Its 235 frames span 14.6875 s including the final-frame hold; source
sample times span 14.625 s.

The original rendering binary is unchanged; its SHA-256 is in the launch
receipt. Checkpoints, images, provenance sidecars, large logs and the movie
stay in `/Volumes/SSD/RISE/fire_production_calibration/r252_r251_preview/`.
The script is `python3 tools/fire/r252_render_movie.py`. No second renderer or solver
runs concurrently with this render.

The first frame was visually checked. Its expected blue appearance follows
the unchanged zero-soot methane record. The rasterizer reported a tiny
negative-radiance warning (maximum magnitude 3.22×10⁻¹⁴ in frame 0), despite
the existing box filter. No physical-color adjustment was made to this preview.

The scene-to-simulation mapping is `t_sim = 10.466040127212182 + t_scene`,
with unit scale and 0.0625 s cadence. The first and midpoint denoised frames
show different plume shapes; 22.5464% of RGB8 display pixels differ by at least
8 code values in one channel. Per-channel mean absolute differences are
0.3783 / 3.5935 / 6.1573 on the 0–255 scale. This is a reported motion check,
not a new numerical gate. See
[data](data/fire_r252/preview_temporal_check.json).

The full render finished: **2 h 10 min 16.957 s rasterization time**. All 235
frames and their provenance sidecars were present. There are 235 distinct
denoised PNG payload hashes. Encoding and full decoding passed, with every
PTS exactly n/16, 128×192 decoded dimensions, rate 16 fps and duration 14.6875 s.
The decoded midpoint image was visually checked against the source.

The first encode used the older r242 APIs and emitted macOS 27 deprecation
warnings. The existing helpers were migrated to receiver/provider APIs and
the completed frames re-encoded; the final encode and decode logs contain no
warnings. No render or simulation rerun was needed. The final file is:

`/Volumes/SSD/RISE/fire_production_calibration/r252_r251_preview/r251_preview_128x192.mov`

SHA-256: `ed7afdc49ae1540549e0191df4a38937b1849288d80c777e2e89cdaa90da26fe`.
See the [receipt](data/fire_r252/preview_complete.json) and
[decode log](data/fire_r252/preview_decode.txt). The scene's stale r241 header
comment was corrected after rendering; `scene_as_rendered.RISEscene` retains
the byte-exact launched source outside git, matching the launch hash. Only
that comment differs from the current recipe; no scene parameter changed.

Fresh independent writer and reader reviews found no P1/P2 issues in the
final helpers. Source and decoded midpoint images match in orientation and
color. The renderer and solver binaries were not rebuilt or changed.
