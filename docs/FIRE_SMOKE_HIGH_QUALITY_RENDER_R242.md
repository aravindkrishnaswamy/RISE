# r242 — high-quality rendering of the completed r241 sequence

The r241 tier-8 run completed its full 25.091040127156969-s window,
publishing 235 snapshots at 16 fps. Its original movie is only 64x64 at
four samples per pixel with denoising disabled. Re-render the immutable
sequence rather than upscale that movie or fabricate intermediate states.
Source manifest SHA256:
`debf9365443bde85e5d5909e8c24b7c4c0d49804d9660eb495ae0747a3cbb346`.

Calibrate the existing spectral rasterizer with denoising and improved
portrait framing, retaining linear EXRs and provenance sidecars. Choose
sampling/resolution from the measured image and time. Preserve the 235-frame
schedule. Render fidelity and simulation qualification are separate: this
tier-8 source remains preview-labelled, with no tier-10 empirical claims.
No changes to simulation, source fields, optics records or physical gates.

The standalone renderer was stale and first refused the manifest schema;
relinked against current library objects. The next invocation exposed the
CLI animation command installing a stack progress callback, which the
prepared time-varying-media path correctly refuses. The minimal launch fix
removes that external callback from the animation command; frame/output
logging remains. The core `PreparedProgressSafe` refusal is unchanged.
Calibration also requires media root `/` with absolute output paths because
the output resolver prefixes the media path. Failed calibration outputs are
not movie evidence. No simulation runs concurrently with rendering.

Calibration: 256x384, 16 spatial samples, one HWSS spectral batch, OIDN,
display exposure 7 rendered in 92.995 seconds (85.15 seconds before final
processing). Inspected raw and denoised PNGs: denoising removes speckles
while retaining the simulated blue methane plume. Final settings are
512x768 with the same sampling, 235 frames at 16 fps, exact scene times
n/16 from 0 through 14.625 s. Zero camera shutter/scanning/pixel time avoids
smearing or sampling outside the sequence. Linear EXRs and all per-frame
provenance sidecars are retained alongside the display PNGs.

Pixel-count extrapolation is 24.28 hours, not a measured full-movie time;
frame-dependent ray cost and loading overhead can change it. H.264 encoding
is 10 Mbps, SDR, 14.6875 s including the final-frame hold. No HDR claim.
The launcher checks all 235 outputs and sidecars before encoding, and the
decoder checks every timestamp, dimensions, count and duration. Payloads
live under `/Volumes/SSD/RISE/fire_production_calibration/r242_tier8_hq.v1`;
the launch receipt and completion manifests bind source, executable,
scene, frame hashes and final movie hash. Movie completion is not claimed
until those checks succeed and the decoded image is inspected.

Launch receipt: 2026-09-21 05:39:33 UTC, renderer SHA256
`5bf8c590393e7d04f69e2c1c810e8a43a442e2519cd64e24bf9805d6f66fe10c`,
scene SHA256 `47992591450496377358fca92fb54aaf12cbd7a6b8b275b726e7a2618580bd9e`.
The first background-shell launch exited before writing a receipt; its
empty directory was preserved as `r242_tier8_hq.empty-launch.v1`. The live
launch uses a persistent execution session with caffeinate. No frames were
overwritten. Output directory acquisition now uses an exclusive mkdir.

Review dispositions: decoded dimensions are checked independently; exclusive
directory creation prevents duplicate writers. H.264/SDR is not a supported
canonical fire movie-provenance encoding, so do not counterfeit one. The
post-decode `r242_finalize.py` instead publishes explicitly external run
evidence with ordered primary/display hashes and their canonical sidecars,
rechecking input hashes before delivery. The existing primary sidecars retain
their authority. This is a display derivative, not a canonical predictive movie.

Backlog under the owner's tangible-output-first scope: reusing a rasterizer
after a prior ordinary CLI render can retain its expired progress callback
when entering prepared animation (Job.cpp prepared animation and selected-frame
branches). This fresh-process scene load has no prior render or callback;
the reported reuse scenario is outside this run. Address with a reuse regression
test in the later CLI lifecycle pass, not a concurrent renderer rebuild.
