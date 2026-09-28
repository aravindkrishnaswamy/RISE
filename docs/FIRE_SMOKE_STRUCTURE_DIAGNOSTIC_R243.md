# r243 — temperature structure diagnostic of the r241 tier-8 sequence

This increment is an **external, non-physical diagnostic**, never a physical
preview, spectral render, or empirical pass. The simulation and renderer are
unchanged. Source is the complete r241 sequence, manifest SHA-256
`debf9365443bde85e5d5909e8c24b7c4c0d49804d9660eb495ae0747a3cbb346`.
Work starts from r242 (`5fe1a301`) in a new worktree, leaving the main checkout
and its uncommitted Xcode change untouched.

## Decision and alternatives

Use an orthographic, optically thin ray integral of a documented artificial
RGB emissivity derived from stored temperature. The map is piecewise linear
at 300, 600, 1000, 1400, 1800, and 2200 K; its exact RGB knots are in the
script and every frame sidecar. `J=RGB(T)*max((T-600)/1600,0)`,
`I=sum_y(J)*dx`, and displayed RGB is `1-exp(-I/0.03)`. These are arbitrary
visualization units, not calibrated radiance. No extinction or scattering.
The left view crops x indices 18..50 and z indices 0..39 for the base; the
right view is the full-height temperature slice at y index 34. Axis extents
use solver cell volumes, not a shifted reinterpretation of the VDB transform.
Pixels replicate cells; no additional spatial detail is inferred. The color
bar describes the slice and local emission hue, not integrated-ray brightness.

Rejected: modifying the physical carbon or chem channels, fabricating a soot
field, or relabeling a modified sequence as a physical preview. Rejected:
per-frame normalization, interpolation to create missing time samples, and
inferring a puffing frequency from a short clip. The first display trial
(v1, retained externally) was too dark; v2 uses the fixed 0.03 display scale
and base crop above. Two independent reviewers found misaligned color-bar
labels in v2. v3 replaces the space-padded labels with individually anchored
numeric ticks at exact temperature coordinates and regenerates all artifacts. This is an explicit display-only choice, not calibration
of physics. The original full sequence remains immutable.

## Products and checks

External root:
`/Volumes/SSD/RISE/fire_production_calibration/r243_nonphysical_diagnostic.v3/`.
Every output image/movie name and provenance sidecar says
`nonphysical_diagnostic`; visible frames also carry the label. JSON sidecars
set `physical_preview_eligible=false`. No canonical RISE provenance is forged.

- Short clip: source frames 64..127, 14.466040127..18.403540127 s,
  64 native snapshots at 16 fps, four seconds including the final frame hold.
- Full statistics movie: all 235 snapshots, 10.466040127..25.091040127 s,
  14.6875 seconds including the final frame hold, also 16 fps.
- Representative still sheet: source frames 64, 80, 96, 112; original
  full-size stills and all 235 frame sidecars remain available.

The extractor reads each VDB only after its SHA matches the immutable time
index; manifest and time-index hashes are pinned independently. Frame hashes,
sidecar hashes, scripts, extractor binary, input ordering, and movie hashes
are recorded. H.264 is an SDR external derivative. AVFoundation decoding
checks every frame timestamp, dimensions (1040x760), count, duration, and
nominal rate; decoded middle frames were visually inspected. A warning-free
standalone extractor build and analytic uniform-column, ambient-zero, and
asymmetric-axis checks passed. No shared library or solver sources changed,
so no full renderer rebuild or qualification was needed for this display tool.

## Structure read and scope

The stills show a compact hot base with changing width, a narrow neck, and
intermittent larger hot lobes above it. A cooler, laterally wandering plume
continues upward through the full-height slice. The base zoom makes these
changes visible without the physical render's blue-only emission. This is
qualitative evidence of changing plume structure; it does not prove a stable
2.74 Hz cycle, establish luminous flame height, or validate a soot model.

The saved **r241** full-spectrum evidence also contradicts the handoff's
slow-mode summary: centerline peak 3.412126 Hz; expected-nearest bin 2.729701
Hz has 0.061456 of peak power, while the 0.136485 Hz bin has power fraction
0.005435. Lit-area peak is 0.477698 Hz and expected-nearest/peak power is
0.696301. These are distinct observables. The prior 1000-fold slow-mode claim
must not be carried forward as if measured on r241. Tier-6 matched baseline
and lateral-domain tests are still required; this diagnostic does not settle
the domain decision and authorizes no tier-10 campaign.
