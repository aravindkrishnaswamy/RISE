# M3b results: rendering simulated deforming meshes

2026-11-23. Design: [SIMULATION_M3_DESIGN.md](SIMULATION_M3_DESIGN.md) §5 (step M3b). Plan:
[SIMULATION_IN_RISE_PROPOSAL.md](SIMULATION_IN_RISE_PROPOSAL.md) §4.3–4.4. Payload:
[SIMULATION_MESH_FORMAT.md](SIMULATION_MESH_FORMAT.md). Solver: [SIMULATION_M3A_RESULTS.md](SIMULATION_M3A_RESULTS.md).

**Status.** A baked cloth (`mesh.v1`) renders in production animation renders through a new geometry chunk,
`simulated_mesh_geometry`. Each frame installs the cache frame for its time between frames under the render
freeze, and deformation motion blur interpolates the vertices per ray between two shutter keys. The blur
matches the time-average of static renders (PT and BDPT). A rigid translation done by the mesh's keys matches the
same motion done by the existing transform blur. A non-blurred simulated mesh hits exactly like a static indexed
mesh. Scenes without a simulated mesh render as before. Not in this pass: the live viewport (§6, M3c), the
`source <simulation>.<output>` binding to a producer chunk (M4), and changing topology (M7).

## 1. Files

| File | Content |
|---|---|
| `src/Library/Simulation/SimulatedMeshSource.{h,cpp}` | `MeshCacheSource`: opens a cache and binds to a `mesh.v1` output; maps time (offset, scale, end policy, nearest frame); reads each payload with its SHA-256 check and decodes it; derives the shutter keys; handles missing-frame, predictive and failure policy |
| `src/Library/Geometry/SimulatedMeshGeometry.{h,cpp}` | `SimulatedMeshGeometry`: subclass of the indexed mesh and an `ITimeVaryingAssetProvider`; installs frames with rollback and freeze; overrides the blur element callbacks and the sample-time lookup |
| `src/Library/Interfaces/IRenderPreparationController.h` | New `ITimeVaryingAssetProvider` (prepare with undo, acquire/release freeze) |
| `src/Library/Geometry/TriangleMeshGeometryIndexed.{h,cpp}` | `UpdateVerticesLean` (§3), `ComputeAreasUncertified`, `ClearSurfaceCertificates`, `RebuildBVHFromPolygons`; `StampSurfaceSignals` in `…Specializations.h` (keeps the signal channel's closed set of writers) |
| `src/Library/Acceleration/BVH.h` | `Refit( logSummary = true )`, `BuildFastFilter( logSummary = true )`: the defaults are unchanged; the lean install passes `false` |
| `src/Library/Animation/{TimeIndexedView.h,Animator.h,Animator.cpp}`, `src/Library/Rendering/TimeIndexedMotionBlur.{h,cpp}` | Sample-time plumbing (§4) |
| `src/Library/Rendering/PixelBasedRasterizerHelper.cpp` | Mesh-only prepared animations keep the ordinary `SetSceneTime` (§2) |
| `src/Library/Job.{h,cpp}`, `src/Library/Interfaces/IJob.h` | `AddSimulatedMeshGeometry` (IJob tail append; manifest regenerated); `ActiveTimeVaryingAssets`, `HasTimeVaryingAssets`; the preparation group walks the assets; render-path gating |
| `src/Library/Parsers/ChunkParserRegistry.cpp` | The `simulated_mesh_geometry` chunk |
| `src/Library/SceneEditor/SceneEditController.cpp` | A production render of a mesh scene from the editor renders at the scrubbed time |
| `tests/SimulatedMeshGeometryTest.cpp` | Unit checks A–H (§5); `--bench` gives the timings in §3 |
| `tests/SimulatedMeshBlurTest.cpp` | Blur rows d, r, e (§5) |
| `scenes/Tests/Simulation/simulated_cloth_drape.RISEscene`, `tools/simulation/render_cloth_drape.sh` | Sample scene and bake/render script (§6) |
| Build lists | Four new library files in all five projects (make `Filelist`, Android cmake, VS2022 vcxproj and filters, Xcode) |

`RISE_API` is unchanged: the geometry is built by `Job` from a cache path and has no C-API constructor.

## 2. The chunk and the per-frame seam

```text
simulated_mesh_geometry
{
	name            geo_cloth
	cache           ~/.cache/rise/simulation-m3b/cloth_drape   # `~/` = home; other relative paths use the media path
	output          surface            # must be mesh.v1
	time_offset     0                  # scene time t reads cache time (t - time_offset) * time_scale,
	time_scale      1                  #   then the cache's own time map
	end_policy      cache              # cache | hold | loop | error
	missing_frame   hold               # preview only: hold | hide (with a warning)
	fidelity        preview            # predictive: fail closed
	motion_blur     TRUE
	face_normals    FALSE              # otherwise the cache's normals, or area-weighted ones
	double_sided    TRUE
}
```

**Class structure.** `SimulatedMeshGeometry` derives from `TriangleMeshGeometryIndexed`. Every geometry service
is the indexed mesh's own: BVH, area sampling, signals, proximity, double-sided handling, and the hit record
shaders read. So a simulated mesh without blur *is* an indexed mesh at the installed frame. The subclass
overrides only the `TreeElementProcessor` callbacks the blur changes (element box, float-filter vertices,
element intersection), `IntersectRay` for a hidden frame, and the installer. Static meshes keep their code and
vtable. The obvious alternative, a wrapper around an inner mesh, would have had to forward every `IGeometry`
call, and it would have stopped the dynamic casts other code makes to the indexed mesh (vertex colours, SMS
audits).

**Seam.** The render path's per-frame controller (`FireSequencePreparationGroup` in `Job.cpp`, handed to
`PrepareAndFreeze`) now also walks `ITimeVaryingAssetProvider`s: every simulated mesh that an object renders.
They join the fire media's transaction. Each prepare returns an undo, so a failure later in the frame (another
asset, the fire fidelity publication) restores the previous frame. The render freeze makes the mesh refuse a
prepare until the freeze is released, and the release reports whether the installed frame stayed put. A frame
that fails (corrupt payload, budget, changed topology, time outside support under `error`, predictive missing
frame) fails the render with `prepared render rejected: …`, and the previous frame stays installed.

`Job::HasTimeVaryingAssets()` (time-varying media, or a bound simulated mesh) now routes a render to the prepared
path everywhere `HasTimeVaryingMedia()` did: still, scene-time still, external still, prediction, owned and
options animations. The `controllers.empty()` refusals now apply only when there are time-varying media. Without
a simulated mesh every condition reduces to the old one.

**Top-level structure, bounds and light tables.** `PrepareAndFreeze` already invalidates the top-level BVH and
forces a light-sampler rebuild *before* the prepare. The install therefore happens after both, and they are
rebuilt from the new frame: the TLAS in `PrepareSceneForFrame` / `PrepareForRendering`, the light tables at
`AttachScene`. Object bounds derive from the geometry box on demand, and the geometry box is the BVH root, which
encloses both keys. The emissive row (§5, e) checks this. One change was needed in the rasterizer.
`RasterizeSceneAnimation` used to call `SetPreparedNominalSceneTime` after `PrepareSceneForFrame` on the prepared
path. That invalidated the TLAS it had just built, and with it the shutter sweep and the per-sample re-compose
plan, so `TimeIndexedFrame::TryCreate` refused ("a per-sample hierarchy re-compose no shutter sweep narrowed")
and every blurred frame rendered single-threaded. Prepared renders *with time-varying media* keep that call. A
mesh-only prepared render has no mutation freeze, so it keeps the ordinary `SetSceneTime`, as non-prepared
animation always did (photon maps regenerate, the TLAS stays). Blurred mesh frames now take the multi-threaded
time-indexed path; the blur test checks this per frame.

**Frame lookup, policies and verification** mirror `fire_medium`'s native reader. A sealed (or
predictive-qualified) snapshot is pinned at bind. A partial cache is reopened each frame (follow mode) and must
still be the same cache: same key, epoch, producer, time map and outputs, or else "reload the scene". Every
payload is read through `CacheSnapshot::Read` (size and SHA-256 against the manifest). It is decoded under the
binding's budget (`frameBudget`, 512 MiB per read and per decode), and its topology digest is checked against
the bound one. A frame a partial cache does not have yet comes back `Missing`:
- preview `hold` keeps the installed frame's nominal pose (dropping its shutter keys) and warns once per frame;
- preview `hide` hides the surface (no hits) and warns;
- `predictive` fails closed.

`predictive` admission requires a complete cache labelled `predictive` that holds every frame of the output, and
runs `IReadOnlyCacheSnapshot::PreflightPredictive`, which decodes every frame to one topology. Positions must be
in metres and velocities in m/s.

## 3. The lean per-frame install

`UpdateVertices` (unchanged for its callers) overwrites positions and normals, recomputes areas, re-proves the
open-sheet and watertight certificates (a position weld plus a sort), drops the signal bakes and refits the BVH,
logging at Info. `UpdateVerticesLean( positions, normals, recomputeAreas )` does the same minus the certificate
work and the logging. It leaves the mesh **uncertified** (neither watertight nor a provably open sheet), the
conservative state every consumer already handles. Topology is fixed, so the referenced-vertex set, the triangle
list and the BVH topology are reused. Areas stay on by default, because area sampling and `GetArea` use them. A
simulated mesh is uncertified from its bind frame on, so it does not change technique between frames.

Timings: `SimulatedMeshGeometryTest --bench`, an M1 Max, one thread, median of 11 frames of a sheet whose every
coordinate moves. The machine was loaded by a GPU fire bake (about 8 cores; load average 5.5–8.5), so the ranges
are ±10–20%.

| Vertices (triangles) | `UpdateVertices` | lean | lean, no areas | 2 Info log lines | cache read + SHA-256 + decode | full install | install with shutter keys |
|---|---|---|---|---|---|---|---|
| 2,601 (5,000) | 2.1–2.2 ms | 0.11 ms | 0.08 ms | 0.007 ms | 0.57 ms | 0.67–0.69 ms | 0.75–0.77 ms |
| 10,201 (20,000) | 8.4–8.8 ms | 0.42–0.47 ms | 0.30 ms | 0.007 ms | 2.1 ms | 2.6–2.7 ms | 2.9–3.0 ms |
| 100,489 (199,712) | 92–94 ms | 4.7–5.2 ms | 3.4–3.6 ms | 0.008 ms | 21 ms | 25.7–26.9 ms | 30.5–31.7 ms |

The certificates (weld and sort) are about 95% of `UpdateVertices` when every coordinate moves. The areas cost
1–1.5 ms per 100k vertices, the refit (with its float-filter and BVH4 re-derivation) 3.5 ms, and the logging
nothing measurable. A motion that only changes one coordinate takes `UpdateVertices`' reusable open-graph proof
and skips the weld: 0.19, 0.80 and 7.4 ms on the same meshes. In the lean install, reading and verifying the
payload (two SHA-256 passes: the file against the manifest, the topology inside the decoder) is about 80% of
the time.

## 4. Deformation blur

**Keys.** Each install holds two keys: the positions (and normals) at shutter open and at shutter close.

*Chosen:* the frame's own velocities. The frame is the nearest one to the nominal time, `k`, and each key is
`P_k + V_k · Δs`. `Δs` is the cache-time offset of the key from frame `k`: the scene-time offset scaled by the
cache and consumer time scales. A static render (zero exposure) at a time between frames uses the same model at
its own time, `P_k + V_k · Δs`. A frame time gives exactly `P_k`.

*Rejected:* positions interpolated from the adjacent frames. The reasons:
1. The cloth writes its integrator's end-of-step velocity, so the keys are first-order exact at the frame.
2. A shutter centred on a frame time straddles the frame. Piecewise-linear interpolation of the adjacent frames
   has a kink there that two keys cannot represent: the midpoint error is `(P_{k+1} − 2P_k + P_{k−1})/8`.
3. One payload decode per frame instead of two or three.
4. A partial cache does not need the next frame to exist yet.

Without velocities in the payload, `V_k` comes from the adjacent frames by central difference (one-sided at the
ends, zero without neighbours). Key normals are the frame normal turned by the change of the recomputed
area-weighted normal between the frame and the key, which is exactly the recomputed normal when the payload has
none. A key whose positions do not differ from the other key's installs no blur.

**Bounds and intersection.** While keys are installed:
- `GetElementBoundingBox` returns the union of the triangle's box at both keys and at the nominal pose.
  Linear interpolation never leaves this box, and `Refit` and the 2× SAH rebuild both use it.
- `GetFloatTriangleVertices` returns false, which turns off the BVH's float Möller–Trumbore filter: its
  precomputed edges are one pose.
- The element tests (closest hit and any hit) interpolate `p(f) = (1−f)·p_open + f·p_close` per vertex, and the
  normals likewise. That is exact at `f = 0` and `f = 1`. They then run the indexed mesh's own triangle test,
  hit record and signal stamp for a mesh without UVs.

**Sample time.** The time comes from the mechanism the transform blur already uses:
- Inside a time-indexed pass (DL-465: the multi-threaded path, and a one-thread frame with a time-indexed
  frame), `TimeIndexedThread::PoseAt(t)` now also writes the calling thread's `TimeIndexed::tlsSampleTime`.
  The thread's view saves and restores it.
- Everywhere else the shared scene is posed through `IAnimator::EvaluateAtTime`: the single-threaded
  per-sample fallback, VCM's per-pass shutter time, photon-tracer time strata, the frame and shutter-sweep poses.
  `Animator` now records that time (`LastEvaluatedTime`, a relaxed atomic). The mesh holds a counted reference
  to the scene's animator, set by the Job at each prepared render.
- With neither, the mesh uses the nominal time.

The normalised fraction is `(t − open)/(close − open)`, clamped to [0, 1]. Only the simulated mesh's own element
test reads it: one thread-local load, or one atomic load. Static geometry pays nothing; the static mesh's
intersection code is unchanged (§5, static check).

`TimeIndexedFrame::TryCreate` sees no animated element for the mesh, which is not keyframable, so a scene whose
only time-varying geometry is simulated meshes takes the parallel path. Every blurred frame in the blur test
asserts this (`TimeIndexedFrame::CreatedCount`).

`motion_blur FALSE` renders the nearest frame as a static mesh, with no extrapolation. Zero exposure installs
no keys, and the mesh is a static mesh at the nominal time.

**Cost.** Closest-hit time of the installed mesh, rays through the sheet, median of 5 runs of 200k rays:

| Triangles | without keys | with keys (mid-shutter) |
|---|---|---|
| 5,000 | 163 ns/ray | 196 ns/ray (×1.20) |
| 20,000 | 199 ns/ray | 240 ns/ray (×1.21) |
| 199,712 | 407 ns/ray | 618 ns/ray (×1.52) |

The extra cost is the float filter going off plus boxes widened by the shutter motion (a quarter frame of motion
here). It grows with the motion relative to the triangle size. The install with keys adds 0.07–5 ms (§3).

## 5. Verification

Builds are warning-free in the files this pass touched. The other agent's in-progress cloth files were warning
or error intermittently during the session. The make build compiles all of them; the other four build lists
were edited by hand and not built.

**`SimulatedMeshGeometryTest`** (106 checks, under 1 s; each case writes its own small `mesh.v1` cache):

| Case | Result |
|---|---|
| A: bind frame vs a static indexed mesh built from the same frame (2,304 rays) | 783 hits; hits, any-hit results, distances and normals **bit-equal** |
| A: installed frame 3 (refit tree) vs a fresh static build | 862 hits; hits, any-hit and distances bit-equal, normals bit-equal (they could differ only where a ray meets a shared edge at a tie); `GetArea` equal; re-preparing is a no-op; `motion_blur FALSE` renders the nearest frame |
| B: keys from velocities; at the open / mid / close sample time vs a static mesh at that pose | 0 hit mismatches; distances bit-equal at both shutter ends (847/847, 873/873) and mid-shutter; the fraction follows the thread's time-indexed time, the animator's shared time, clamps, and falls back to nominal |
| C: keys from adjacent frames (no velocities) | 0 hit mismatches, distance error ≤ 5.8e-9 (float positions) |
| D: time map | Nearest frame; keys keep the nominal's frame; offset and scale; hold (no motion while held); `error` refuses a nominal time outside the support and clamps a key; loop period and wrap; zero scale and an absent output refused |
| E: missing frames, predictive | Partial cache: `hold` keeps the frame (dropping its keys); `hide` hides and the undo restores it; predictive refuses a partial and a preview-labelled cache, admits a predictive one, and fails closed on a frame corrupted after admission |
| F: failures | Changed topology, corrupt payload (digest), over-budget decode: each fails its frame and the installed frame stays (hits bit-equal) |
| G: freeze and rollback | A prepare under the freeze gets `mutation_frozen`; release confirms; the undo restores the previous frame exactly |
| H: through a Job | The scene loads; a prepared animation render installs the frame for its time; a still installs the frame at the animation start; a predictive binding of a preview cache and a missing cache fail the load; a frame outside the support under `end_policy error` fails the render |

**`SimulatedMeshBlurTest`.** The gate is DL-457's: Welch t, family α 0.01 Bonferroni, 16×16 film, 32 spp, 8
blurred renders, 8 reference times × 3 statics. The fixture cache has frames at t = −1.5, 0.5 and 2.5, so the
shutter [0, 1] of the frame at 0.5 reads one frame.

| Row | Blurred | Reference | t (crit) |
|---|---|---|---|
| d, PT: deforming sheet (translation plus anisotropic stretch: its area is ×0.42 at open and ×1.82 at close) vs the time-average of statics | 0.876252 ± 0.000669 | 0.875321 ± 0.000055 | +1.39 (4.57) |
| d, BDPT | 0.875275 ± 0.001101 | 0.875184 ± 0.000045 | +0.08 (4.59) |
| r, PT: rigid translation, the mesh's keys vs an indexed mesh moved by a keyframed object (transform blur) | 0.879731 ± 0.001037 | 0.880356 ± 0.000490 | −0.55 (4.01) |
| e, PT: emissive sheet, no blur, installed frame vs an indexed mesh at that pose (the bind frame has a quarter of the area) | 0.185215 ± 0.000118 | 0.185450 ± 0.000196 | −1.02 (3.85) |

- **Time-indexed path:** all 8 blurred frames of each blurred row took it.
- **Seeds:** six other seeds gave |t| ≤ 3.24 over 18 comparisons, against critical values of 3.5–4.6.
- **Bias check:** 40 blurred repeats of row d gave t = −0.79, −1.13, −2.76, −0.55 (crit 2.97); the signs vary.
- **Red-proofs** (temporary edits, reverted):
  - shutter fraction frozen at 0.5: row d fails, t = +56.5 (PT) and +45.3 (BDPT);
  - areas left stale on install: row e fails, t = −116.
- **Run time:** about 1 s.

**Static geometry.** The indexed mesh's intersection code is untouched; the default `Refit`/`BuildFastFilter` and
`UpdateVertices` behave as before. Closest-hit and any-hit throughput on a static 100,352-triangle mesh (400k
rays, single thread, median of 7) was measured with the binary built before any change and the binary built
after, run alternately. This is the temporary benchmark `M3bMeshBench`, kept in the session scratch, not in the
tree.

| Pair | closest hit, before / after (ns/ray) | any hit, before / after (ns/ray) |
|---|---|---|
| 1 | 361 / 355 | 241 / 240 |
| 2 | 352 / 353 | 238 / 245 |
| 3 | 356 / 426 | 237 / 293 |
| 4 | 421 / 416 | 285 / 276 |
| 5 | 466 / 380 | 317 / 279 |

These are equal within the load noise, which swings ±15% between consecutive runs; checksums and hit counts are
identical.

**Suites.** `SourceHygieneTest` passes (706 checks), with one change: the IJob vtable manifest gains one tail line
for `AddSimulatedMeshGeometry`. `CstDeriveGoldenTest` passes (stratified, 142 MATCH, 0 DRIFT, 0 UNCOVERED). The
new scene is machine-specific for it (§6) and is not covered. `SceneEditorSuggestionsTest` pins the chunk-keyword
count; it moves from 180 to 181. The fast tier without the `Fire*` tests
(`./run_all_tests.sh --tier fast --timeout 300 --filter '[!F]*,F[!i]*,Fi[!r]*,Fir[!e]*'`, 415 tests) passed 414.
The one failure was that keyword-count pin, now updated and passing on its own. `MotionBlurTimeAverageTest`,
`TimeIndexedMotionBlurTest` and `TimeIndexedPainterTest` pass unchanged. The `Fire*` tests were not run.

## 6. Sample scene and demonstration

`scenes/Tests/Simulation/simulated_cloth_drape.RISEscene` is the M3a demonstration: a 1 m cotton sheet pinned at
two corners, falling over a 0.3 m sphere, 51×51 vertices, 2 s at 30 Hz. It is rendered through
`simulated_mesh_geometry` with a 1/60 s shutter (180°). `tools/simulation/render_cloth_drape.sh [frames]` does
the rest:
- bakes `rise_sim bake cloth.shell` into `~/.cache/rise/simulation-m3b/cloth_drape` (mode 700, private), or
  reuses or resumes an earlier bake. The cache writer refuses group-writable ancestors such as the shared volume
  holding the checkout, so the bake cannot live under the repo;
- renders each frame with blur, and with a copy whose shutter is closed, into `rendered/simulation_m3b/`
  (gitignored).

On this machine under the GPU bake's load:
- the bake took 11.9 s for 61 frames, with the cloth solver as of this session; the other agent was changing it
  concurrently;
- the 640×360, 64 spp renders with OIDN took about 3 s per frame;
- output: `rendered/simulation_m3b/cloth_drape_denoised00{06,09,12,18,24}.png` and the same frames as
  `cloth_drape_noblur_denoised…`.

The sheet's free edge streaks in the blurred frames (frame 6, during the fall, shows it best).

The scene names its cache with a per-user path. Its header comment carries an absolute example path, so
`CstDeriveGoldenTest` classifies it as machine-specific (its rule (c)) on every machine. Without that, its
derive status would flip between machines with and without a bake.

## 7. Limits

- **Velocity extrapolation is linear over the shutter.** Acceleration inside the shutter, and rotation, are not
  captured. At a velocity discontinuity (a collision), a key can sit a little inside a collider: at most
  `|ΔV|·exposure/2`, about 8 ms of motion at 180° and 30 fps. Two keys only.
- **Nearest-frame model.** A static render between frames extrapolates from the nearest frame, so a time just past
  the midpoint between frames switches frame. The jump is of order `a·Δ²/8`. A shutter longer than one cache
  interval extrapolates one frame across all of it.
- **Nominal pose for everything except ray hits.** Area sampling, `GetArea`, the signal bakes, proximity, SMS and
  `DistanceToSurface` use the nominal positions.
- **Emissive simulated meshes.** Without blur they are supported: the light tables and the area CDF follow each
  installed frame (row e, red-proved). Under deformation blur they are **unsupported**: emission is sampled at
  the nominal pose while hits interpolate, which biases NEE against BSDF sampling. A hidden frame is still
  area-sampled as an emitter.
- **Certificates.** The mesh is never certified watertight or a provably open sheet. A transmissive or
  medium-filled simulated mesh takes the IOR-stack / uncertified rules (ObjectManager warns), even where the same
  static mesh would be certified. Hits are bit-equal; these surface classifications are not.
- **No texture coordinates.** `mesh.v1` has none (M3a open question 1). Positions are object space in metres, with
  no `scene_unit` conversion.
- **Where frames are installed.** Only meshes some object renders directly (`object.GetGeometry()`) are installed.
  A `displaced_geometry` over a simulated mesh tessellates the bind frame. `RasterizeRegion` and the interactive
  viewport render whatever frame was last installed, without a per-frame install (M3c).
- **Interlaced fields.** These install once per frame, for the frame's full support.
- **The shared pose clock.** It is the scene's animator. Two scenes rendering concurrently each read their own,
  but a mesh shared between two scenes reads the last one bound.

## 8. What M3c and M7 need

**M3c (live viewport).**
- **A frame-provider entry on the scene edit controller.** It parks the interactive render, calls
  `PrepareAssetForRender({t, t, t})`, invalidates the TLAS and kicks the render, reusing the scrub path's
  cancel–wait–mutate–kick protocol. Today `SceneEditController` only routes a production render of a mesh scene
  to the scrubbed time.
- **An in-memory source.** `MeshCacheSource` reads a disk `CacheSnapshot`. The in-process host publishes through
  `IReadOnlyCacheSnapshot`, so the source needs that interface. It may skip the payload SHA-256 for trusted
  in-memory frames, which is 80% of the install at 100k vertices.
- **Measured budget.** The install costs 0.7 ms at 2.6k vertices and 2.6 ms at 10k (§3), against a viewport
  frame. The lean path with no areas (0.08 / 0.30 ms) is available when the viewport has no emissive meshes.

**M7 (changing topology, liquids).**
- **A per-frame rebuild.** `DoneIndexedTriangles` with a new topology, a full BVH build and a TLAS rebuild, in
  place of a refit.
- **Accepting a changing digest.** `ReadFrame` today refuses one.
- **The empty mesh.** `mesh.v1` allows `V = T = 0`; `Open` refuses it today.
- **Velocities per vertex of the current frame only.** The shutter keys still work within a frame, but there is
  no correspondence across frames.
- **A bounded mailbox table.** It is the per-geometry table the design note flags, keyed by geometry id and
  growing with every rebuilt geometry. Either the geometry is kept and rebuilt in place, or the mailbox ids must
  be recycled.
- **Light tables and certificates** rebuilt per frame, as here.
