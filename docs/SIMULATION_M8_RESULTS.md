# M8 results: portable playback, a non-Metal build, and where fire's CPU path stands

2026-11-26. Plan: [SIMULATION_IN_RISE_PROPOSAL.md](SIMULATION_IN_RISE_PROPOSAL.md) §5, row M8. Contract:
[SIMULATION_SOLVER_CONTRACT.md](SIMULATION_SOLVER_CONTRACT.md) (no contract change; one Appendix C row records the
backend detail text and the trusted-roots separator).

**Outcome.**

- **Part B (cross-platform playback): done on this Mac, inspected for Windows and Android.** Every codec and the cache
  container were already defined byte by byte, little-endian. A committed golden cache now proves it: one sealed
  cache holding all six frame codecs plus a static topology record, decoded and compared value by value, and
  re-encoded to identical bytes (§2). The library, `rise`, `rise-sim`, `simulation_cache_import` and all 467 test
  programs build warning-free with Metal compiled out, and the non-fire fast tier passes in that build (§3).
  Reading the Windows and Android builds found seven certain compile errors in the simulation and fire sources,
  four of which also broke every non-Apple build, plus one non-Metal test link error; all are fixed (§4). **Neither Windows nor Android was compiled.**
- **Second follow-up (§12, M8d, the same night):** the step-6 difference was the CPU's EOS inversion bracket (a CPU
  error, fixed: steps 1–10 from identical bytes now agree to rounding); the rim-ignition fix is specified as a
  checklist for the approved re-baseline; certified hashing is cheaper; the CPU backend is selectable from a scene.
- **Follow-up (§11, the same evening):** the step-2 difference is a rim-ignition rule reading rounding residue (not
  a backend error); CPU fast mode takes Metal's early V-cycle stop; a 2.5 s run of both backends; faster CPU hashing.
- **Part A (fire on the CPU): finished later the same day, see §10. The CPU backend is now `experimental` and the
  exit criterion is met; the gap below was r264's centred momentum carrier, missing on the CPU side.** The
  first-pass text follows, superseded where §10 says so: **an honest partial. The CPU backend stays `unsupported`.** A complete CPU
  projected-Heun owner already exists (r190, extended for the burner inlet in r239) and runs one full owner step
  (R0/R1/R2 Picard, FCT, EOS, targets, projection, momentum) without Metal, but only as a calibration reference:
  single-threaded, capped at a 2 GiB working set, called only by two test programs, and not callable by the run
  driver, which builds a Metal request and has Metal-only memory admission (§5). On the 64-cell burner fixture the
  Metal and CPU owners differ by 0.9–3.2e-6 of field maximum after one step: the CPU owner is within 1–2 binary32 ulps
  of the fp64 mirror, the Metal owner 8–27 ulps, so the difference is 4–10× what rounding of one owner explains and
  its cause is not isolated. At least one default-on Metal scheme change since r239 (r264's centred momentum carrier)
  is not in the CPU owner (§6). No solver-level Metal-vs-CPU run over
  several steps exists, so the agreement exit criterion of M8 is **not met**.

## 1. Files

| File | Change |
|---|---|
| `tests/SimulationGoldenCacheTest.cpp` | New fast-tier test and fixture writer (`--write <dir>`) |
| `tests/data/simulation_golden_cache/` | New committed fixture: `manifest.cbor`, 21 `.frame`, 1 `.static`; 7,875 bytes |
| `tests/test_schedule.tsv` | Row for the new test |
| `tests/data/.gitattributes` | The fixture is `binary` (no end-of-line conversion on a Windows checkout) |
| `tests/SimulationSceneTest.cpp` | Fire memory-estimate checks keyed on "Metal built" instead of `__APPLE__`; without Metal the estimate must be 0 (unknown) |
| `src/Library/Utilities/FireProductionTransport.h`, `FireProductionForce.h`, `FireProductionProjection.h` | Portable declarations no longer hidden behind `__APPLE__` (§4.1) |
| `src/Library/Utilities/FireProductionAdvectionUnsupported.cpp`, `FireProductionForceUnsupported.cpp` | Stubs no longer hidden behind `__APPLE__`; two stubs added for the non-Metal test link |
| `src/Library/Utilities/FireProductionAdvection.cpp` | Duplicate non-Apple digest stub removed (the unsupported file owns it) |
| `tests/fire_production_fp64/*`, `tests/fire_production_trace/*` | Regenerated mirrors (mechanical; `--check` passes) |
| `src/Library/Simulation/Fire/FireRunDriver.cpp` | `setenv`/`unsetenv` through a portable helper (`_putenv_s` on Windows) |
| `src/Library/Simulation/Liquid/LiquidGrid.cpp`, `LiquidViscosity.cpp` | Locals `far`, `near` renamed (`<windows.h>` macros) |
| `src/Library/Simulation/Fire/FireRunCheckpoint.h` | Static asserts for the checkpoint's width assumptions |
| `src/Library/Simulation/SimulationTransform.cpp` | Static assert: binary64 is IEEE |
| `src/Library/Simulation/SimulationCache.cpp`, `.h` | `RISE_SIMULATION_TRUSTED_ROOTS` splits on `;` on Windows (`:` is in a drive letter) |
| `src/Library/Simulation/SimulationBakeManager.cpp` | Windows/Android: the "rise-sim not found" refusal says out-of-process bakes are unsupported there |
| `src/Library/Simulation/{Cloth,Liquid,Rigid}/*Solver.cpp`, `Fire/FireSolver.cpp` | Backend detail text states the platform evidence (§7) |
| `tests/FireSequenceTest.cpp` | `--fire-production-burner-reference` also prints CPU32-vs-fp64 and Metal-vs-CPU32 norms, velocities and timings |

No new library source, so the five build lists are unchanged. Pinned counts unchanged (chunk keywords 191,
RewireConnectionTest 6). No codec, manifest, checkpoint or solver version changed; no cache written before M8 changes
meaning. The Metal path's code is untouched apart from declarations becoming visible on other platforms (a no-op
under `__APPLE__`).

## 2. Byte-level audit and the golden cache

### 2.1 Audit (read, then tested where marked)

| Item | Finding |
|---|---|
| `grid.v1`, `mesh.v1`, `mesh.v2`, `mesh.topology.v1`, `field.v1`, `transform.v1`, `points.v1` | Every integer is written and read with explicit little-endian shifts, never by `memcpy` of a struct; floats are carried as their IEEE bit patterns (`uint32`/`uint64`), so no host float formatting or rounding is involved. `points.v1` bulk-copies arrays only on a little-endian host and falls back to per-byte assembly otherwise. Reads are byte-wise or `memcpy`, so unaligned input is safe on ARM. **Tested** by the golden cache. |
| Cache container | `manifest.cbor` is canonical RISECBOR64 (big-endian per CBOR, written byte-wise); payloads are files named by SHA-256. No timestamps, host names, pids or paths in the manifest. File I/O has separate POSIX (`openat`, `fsync`, `renameat`) and Windows (`CreateFileW`, `FlushFileBuffers`, `MoveFileExW`) branches; seeks are `lseek`/`_fseeki64` on 64-bit offsets; no `mmap`. **Container tested** by the golden cache (manifest bytes reproduced); **Windows branch inspected only**. |
| Worker checkpoint (`RISESIMC`) and streamed checkpoints | Little-endian framing written byte-wise, CBOR header, SHA-256 trailer. Portable. |
| `cloth.shell`, `rigid.bodies`, `liquid.mac`, group checkpoints | CBOR with doubles packed little-endian byte by byte. Portable. |
| Fire checkpoint (`FireRunCheckpoint`) | Host byte order behind an endian tag (`0x0102030405060708`) that the reader checks, so a foreign-order file is refused, never misread. Values are fixed-width except `std::size_t` and `bool` fields. Every target (macOS arm64, Windows x64/ARM64, Android arm64) is little-endian and 64-bit, so files read across them; static asserts now make a 32-bit or exotic target fail to compile instead of writing different bytes. Not rewritten, to keep the Metal path's checkpoint bytes. |
| `size_t`/`long`/`wchar_t`/`time_t` | No `long`, `wchar_t` or `time_t` on disk; `size_t` only in the fire checkpoint (above). Decoder memory budgets charge `sizeof` of in-memory structs and string capacities, which differ by standard library (MSVC's `std::string` is 32 bytes, libc++'s 24): a decode within a few percent of its budget could succeed on one platform and be refused on another. Bytes are unaffected. Recorded, not changed. |
| Locale | Nothing in `src/` calls `setlocale` or `std::locale::global`, so `strtod` and stream formatting run in the "C" locale on every platform. Scene numbers parse with `strtod`; a host application that changed `LC_NUMERIC` would break scene parsing, not caches. |
| Float model | `-ffast-math` (macOS, Android) does not touch the codecs: they never do arithmetic on samples. Grid already asserted IEEE binary64; transform now does. |
| Paths | `std::filesystem::path` throughout; Windows opens use wide paths. `RISE_SIMULATION_TRUSTED_ROOTS` split on `:`, which breaks on `C:\`; now `;` on Windows. |
| Long double | `FireProductionAccepted*Budget.h` accumulate fire budget diagnostics in `long double`: 64-bit on MSVC and Apple arm64, 128-bit (software) on Android arm64. Diagnostic values would differ on Android; no cache or checkpoint byte depends on them. Recorded. |

### 2.2 Golden cache (`SimulationGoldenCacheTest`)

`tests/data/simulation_golden_cache/` is one sealed cache, 3 frames at 0.5 s, seven outputs:

| Output | Codec | What it exercises |
|---|---|---|
| `grid` | `grid.v1` | Two channels (scalar, vector), a dense block with a mask, a constant block, a sheared and translated index-to-world matrix, nonzero backgrounds, −0.0, a subnormal-adjacent value, 65504 |
| `mesh1` | `mesh.v1` | Moving triangle with velocities and normals |
| `splash` | `mesh.v2` inline | Topology changes every frame (2 → 3 triangles, one unreferenced vertex, UV set, one then two attributes) and the last frame is **empty** (V = T = 0) |
| `sheet` | `mesh.v2` + `mesh.topology.v1` | Frames referencing one static topology record with a static UV set |
| `field` | `field.v1` | Scalar and 3-component channels over a domain digest |
| `transforms` | `transform.v1` | Two bodies, binary64 values including 1e-300, 1e10, −0.0 and −9.80665 |
| `points` | `points.v1` | Velocities, per-point radii, 64-bit ids above 2³², two attributes |

The test (default mode, any platform) opens the committed cache and checks the manifest (description, all 21 entries'
times, sizes and digests, the static record), decodes every payload and compares every value with frames built in the
test source from literals, checks a set of raw bit patterns written as hex literals (independent of the builders), and
writes a fresh cache from the same frames: all 23 files must equal the committed files byte for byte. Flipping one
byte of one committed payload gives four failures (digest, two literal checks, re-encode). Regenerate only for a
deliberate format change: `bin/tests/SimulationGoldenCacheTest --write tests/data/simulation_golden_cache` (after
removing the directory; the writer stages in a private temporary directory, as the cache location policy requires).

The existing per-codec tests already held hand-assembled golden byte strings for each codec; the new fixture adds the
container, the static-payload path, changing and empty topology, and a single place a foreign build can be pointed at.

## 3. Build and tests with Metal compiled out (verified on this Mac)

**How.** The make build selects Metal through `IS_MACOS_BUILD` (set only in `Config.OSX`): with it unset, `Filelist`
takes the `*Unsupported.cpp` legs (`FireProduction{Compute,Advection,Projection,Force}`, `TlsTransport`) instead of the
`.mm` files, as the Linux, Windows and Android builds do. The makefile builds objects next to the sources, so an
out-of-tree build is not possible; instead the sources, tests, tools and makefiles were copied (no objects) to
`/Volumes/SSD/claude-tmp/m8-scratch/nometal/` with large read-only data directories symlinked, and built there with a
`Config.NoMetal` (= `Config.OSX` without `IS_MACOS_BUILD` and without `-framework Foundation -framework Metal`). The main
tree's objects were not touched. Four scratch-only overrides were needed, none a source problem: a fixed build-identity
header (the copy has no git), Apple `ld` instead of the GNU `--start-group` flags the non-macOS branch assumes, and
`-framework ImageIO/CoreGraphics/CoreFoundation` for `FireSequenceTest`'s `__APPLE__` preview branch (on a real
non-Apple build that branch is not compiled).

**Result.** Library, `rise`, `rise-sim`, `simulation_cache_import`, fire tools and all 467 test programs built with no
warning and no error after the fixes in §4. `otool -L` shows neither Metal.framework nor Foundation.framework in `rise`,
`rise_sim` or `FireSolverTest`; no `Fire*Mac.o` exists in the copy.

Non-fire fast tier in that build (`--filter '[!F]*,F[!i]*,Fi[!r]*,Fir[!e]*'`): **483 passed, 4 failed**, every failure
an artefact of the copy or of the emulation, none of the code:

| Test | Cause |
|---|---|
| `CstDeriveGoldenTest` | needs `git ls-files`; the copy is not a repository |
| `SourceHygieneTest` | reads repository files the copy omits (Android sources, runner scripts); after copying those, 3 of 711 checks still read files outside the copy. It passes in the checkout (711/711) |
| `AgentEvalLiveTransportTest` | expects a real TLS transport under `__APPLE__`, but the emulation links `TlsTransportUnsupported.cpp` (on Windows the real `TlsTransportWin.cpp` is built) |
| `SimulationSceneTest` | a real non-Metal behaviour: the fire memory estimate comes from the Metal owner's working set and is 0 (unknown) without Metal, but the test keyed its expectation on `__APPLE__`. The test now keys it on whether Metal is built (and requires 0 otherwise); it passes in both builds |

All simulation tests (cloth, rigid, liquid, coupling, cache, codecs, worker, bake manager, scene, viewport, the new
golden cache) passed in the non-Metal build.

Fire tests in that build (`Fire*`; no GPU can be used, Metal is not linked): 9 of 13 pass, including
`FireCachePlaybackTest` (fire playback through the cache without Metal), `FireSimulationSolverTest`,
`FireProductionCalibrationTest`, the record and optics tests. The four that fail (`FireSolverTest`, `FireSequenceTest`,
`FireProductionSolverTest`, `FireProductionProjectionTest`) need the Metal device for what they check; for example
`fire.methane` `Configure` refuses with `host_memory_certificate_unavailable` because the driver's admission uses the
Metal working-set queries. They have no skip for a build without Metal (open item 5).

**What this does not cover.** `__APPLE__` is still defined in this build, so code guarded by `__APPLE__` (rather than
by the Metal choice) compiles its Apple branch; the non-Apple branches of `SimulationProcess.cpp`, `SimulationCache.cpp`,
`SimulationBakeManager.cpp`, `FireRunHostMemory.h` and others were read, not compiled (§4).

## 4. Windows and Android, by inspection (not compiled)

Read against `build/VS2022/Library/Library.vcxproj` (C++17, `/W3`, no `/WX`, `NOMINMAX`, `WIN32_LEAN_AND_MEAN` and
`<windows.h>` from `pch.h`) and `build/cmake/rise-android/` (NDK clang, `android-29`, arm64-v8a).

### 4.1 Fixed (certain compile or link errors)

1. `FireProductionForce.h`: `FireProductionProjectedHeunMetalOwnerRequest` (used by every fire file) has a member of
   type `FireProductionResidentTargetLineageComparatorRequest`, declared only under `__APPLE__` in
   `FireProductionTransport.h`. **Every non-Apple build failed to compile the fire stack**, Linux included.
2. `FireRunDriver.cpp` calls `FireProductionResidentTargetLineageMetalWorkingSetBytes`, declared and stubbed only under
   `__APPLE__`: undeclared, then unresolved.
3. `FireProductionAdvectionUnsupported.cpp`: a stub's result type declared only under `__APPLE__`.
4. `FireProductionForce.cpp` calls `FireProductionResidentEOSCandidateLiveIncrementWorkingSetBytes`, declared only under
   `__APPLE__`.

   Fix for 1–4: the `#if defined(__APPLE__)` blocks around plain declarations (Transport.h, Force.h, Projection.h)
   and around the refusal stubs (both `*Unsupported.cpp`) were removed; Objective-C blocks keep their
   `__OBJC__ && __APPLE__` guard. Non-Apple builds now see exactly what the macOS non-Metal link already compiled. The
   duplicate non-Apple `FireProductionPayloadDigestMetal` stub in `FireProductionAdvection.cpp` was removed (it would
   now collide with the unsupported file's).
5. `FireRunDriver.cpp`: `setenv`/`unsetenv` (MSVC has neither). Now `_putenv_s` on Windows.
6. `LiquidGrid.cpp` (`const double far`) and 7. `LiquidViscosity.cpp` (`bool near`): `<windows.h>` defines `near` and
   `far` as empty macros, so `liquid.mac` did not compile with MSVC. Renamed.
8. Non-Metal link of `FireProductionCalibrationTest` (found by the §3 build): `FireProductionDualLayoutPackRequiresSerialOwner`
   (pure logic) and `BuildFireProductionScalarPhysicalFluxPrerequisiteMetal` (a refusal on every platform) existed only in
   the `.mm`; both now also in `FireProductionAdvectionUnsupported.cpp`.

### 4.2 Open (needs a real port or a Windows machine)

- **`rise-sim` and `simulation_cache_import` have no VS2022 or Android target**; only the make build produces them. The
  out-of-process host (`SimulationProcess.cpp` Windows branch, marked "NOT COMPILED OR TESTED") and the bake manager are
  compiled into the library on both platforms and read as compiling. With no worker, a bake refuses with "rise-sim was
  not found … (out-of-process bakes are unsupported on this platform: no rise-sim worker is built for it)". In-process
  runs (cloth, rigid, liquid) need no worker.
- **Windows ARM64 is not configured**: `RISE.sln` and the projects have only `x64` configurations (vcpkg triplet
  `x64-windows`).
- Windows runtime concerns, not compile errors: the advisory lock locks byte 0 with `LockFileEx`, so reading the lock
  file's text from another process will likely fail; `SpawnDetachedProcess` quotes arguments without escaping embedded
  quotes or trailing backslashes, converts UTF-8 paths through the ANSI code page, and inherits handles; `SyncDirectory`
  flushes a directory handle opened `GENERIC_WRITE`, which may be refused to an ordinary user, failing every durable
  cache publish. Each needs a Windows run to settle.
- Android: `/proc/<pid>/stat` of another app's process may be unreadable, so a worker could be reported dead.
- Fire on either platform refuses at start (no Metal; the CPU backend is unsupported, §5).

### 4.3 Checked and fine

All simulation and fire sources in `Filelist` are in `Library.vcxproj` and `rise_sources.cmake` (and `SourceHygieneTest`
enforces it); no `.mm` is in either; Apple headers (`libproc.h`, `mach-o/dyld.h`, `sys/sysctl.h`, `mach/mach.h`,
`sys/acl.h`) are guarded; POSIX calls are inside `!_WIN32` guards; `posix_spawn` needs API 28 (target 29); no
designated initializers, VLAs or unguarded `__builtin_*`/`__attribute__`; no `long` on disk, no `%ld`; Windows I/O is
chunked in 1 MiB `DWORD` pieces.

## 5. Fire: what the CPU path covers (inventory)

The production step is `AttemptFireProductionProjectedHeunResidentStepMetal` (`FireProductionAdvectionMac.mm`), driven
by `FireRunDriver.cpp`: a resident projected-Heun owner (`ResidentProjectedHeunMetalOwner`, about 4,000 lines of host
orchestration over about 90 of the file's kernels) running three stages R0, R1, R2, each a Picard loop to the
projection tolerance with open-boundary active-set cycle detection and canonical selection, limiter-discontinuity
certification and a terminal re-projection. Per iteration: projection (multigrid pressure solve with open-boundary
classes), transport authority (Vreman ν_t, diffusivities), physical and advective flux pair, FCT limiter, EOS
temperature inversion (deep prefix table; plain binary32 in fast mode), frozen source authority, divergence target,
non-pressure momentum RHS (advection, buoyancy, stress), Heun momentum, gas source. Around the owner the driver, all on
the CPU already: case derivation, the source build (packets, chemistry, floor irradiance, radiation fraction), time-step
selection, frames, checkpoints, budgets.

| Part of the step | CPU code that exists | Status |
|---|---|---|
| Source build, chemistry, radiation source terms, time step, frames, checkpoints | `FireProductionSource*`, `FireSimulationRecords`, `FireRunDriver`, `FireRunFrames`, `FireRunCheckpoint` | CPU already (both backends use it) |
| Whole owner step R0/R1/R2 with Picard, open-boundary cycles and canonical selection | `FireProductionProjectedHeunCPUOwner` (`FireProductionForce.cpp`, r190; burner inlet r239) and its generated fp64 mirror | Exists; **calibration reference only** |
| Projection | `ProjectFireProductionCPU`, `…ResidentPhysicalCPU`, `…RestorationCPU` | Exists |
| Scalar transport, FCT, Heun flux stages | `…ScalarHeunFluxStageCPU`, `…ScalarFCTFluxPairCPU`, `RemapFireProduction*CPU` | Exists |
| EOS acceptance | `EvaluateFireProductionScalarEOSAcceptanceCPU` (fast-mode branch present) | Exists |
| Divergence target | `Compose…DivergenceTargetCPU`, `Project…ScalarTargetCPU` | Exists |
| Momentum RHS, Heun momentum, force | `EvaluateFireProductionNonpressureMomentumRHSCPU`, `…CompatibleHeunMomentumCPU`, `AdvanceFireProductionFrozenForceCPU` | Exists |
| Transport coefficients for the owner (`FireProductionProjectedHeunTransportProvider`) | Only test classes (`ResidentOwnerTransportFP32/64` in `FireSequenceTest.cpp`, `ConstantProjectedHeunTransport32` in the calibration test) | **Missing in the library** |
| Request adapter: driver's `FireProductionProjectedHeunMetalOwnerRequest` → CPU `FireProductionProjectedHeunOwnerRequest` | Hand-written in `FireSequenceTest.cpp` for the burner fixture | **Missing in the library** |
| Result adapter: CPU owner result → `FireProductionResidentStepResult` the driver consumes (projection, dual momentum, EOS deviation, pressure ratio, …) | None | **Missing** |
| Driver backend switch, memory admission without Metal (the driver queries Metal working-set limits; the stubs refuse) | None | **Missing** |
| Opt-in Metal variants: compact viscous stencil, near-wall WALE, Vreman zone, hyper-Vreman (r271), limited momentum carrier | Metal only | Not in the CPU owner (all are off by default) |
| Size and speed | CPU owner refuses working sets above 2 GiB; single-threaded (no thread pool in Force/Transport/Projection) | Production tiers out of reach |
| Determinism | Single-threaded, so the same bytes at any thread count trivially | n/a until threaded |

So the numerical definitions do exist on the CPU, and largely as the same C++ the Metal kernels were qualified against;
what is missing is the plumbing that would let `fire.methane` run them as its step, plus threading and a larger
working-set limit. That is well beyond M8's remaining scope here, so per the M8 brief Part B was completed first and
Part A stops at the evidence below.

## 6. Owner-level agreement on the coarse case (64 cells)

`FireSequenceTest --fire-production-burner-reference` builds the 64-cell burner-inlet fixture (the smallest owner case
in the tree), runs the CPU32 owner, the generated fp64 owner and the Metal owner on the same request, and now also
prints each owner's distance from fp64 and the Metal–CPU32 distance. One owner step, not a solver run.

Certified mode (fast mode gives the same CPU values and Metal values equal to these to the digits shown; one
component differs in the 8th digit). Distances are maxima over the 64 cells (velocity: over faces); "field max" is
the fp64 owner's maximum magnitude.

| Field | Field max | CPU32 − fp64 | Metal − fp64 | Metal − CPU32 | Metal − CPU32 / field max | Unequal words |
|---|---:|---:|---:|---:|---:|---:|
| component 0 (kg/m³) | 0.0410 | 5.8e-9 | 3.7e-8 | 3.8e-8 | 9.3e-7 | 24 / 64 |
| component 1 (kg/m³) | 0.0384 | 2.3e-9 | 3.6e-8 | 3.6e-8 | 9.5e-7 | 20 / 64 |
| component 2 (kg/m³) | 0.241 | 2.1e-8 | 3.1e-7 | 3.0e-7 | 1.2e-6 | 23 / 64 |
| component 3 (kg/m³) | 0.807 | 7.5e-8 | 1.02e-6 | 1.04e-6 | 1.3e-6 | 16 / 64 |
| component 4 (kg/m³) | 0.00703 | 7.1e-10 | 6.4e-9 | 6.3e-9 | 8.9e-7 | 26 / 64 |
| component 5 (kg/m³) | 0.00575 | 5.7e-10 | 5.3e-9 | 5.2e-9 | 9.1e-7 | 27 / 64 |
| component 6 (soot, kg/m³) | 3.4e-14 | 4.2e-14 | 4.2e-14 | 9.6e-16 | 2.8e-2 | 64 / 64 |
| component 7 (soot, kg/m³) | 1.3e-14 | 1.4e-14 | 1.4e-14 | 7.0e-16 | 5.6e-2 | 64 / 64 |
| component 8, energy (J/m³) | 3.40e5 | 0.051 | 0.69 | 0.66 | 1.9e-6 | 24 / 64 |
| velocity x (m/s) | 0.0692 | 1.2e-8 | — | 2.0e-7 | 2.9e-6 | — |
| velocity y (m/s) | 0.0565 | 1.1e-8 | — | 1.8e-7 | 3.2e-6 | — |
| velocity z (m/s) | 0.0540 | 8.0e-9 | — | 1.3e-7 | 2.3e-6 | — |

(Temperature is not an owner output; it follows from the state through the EOS. Fast mode needed one test-only change:
the fixture's certified target-enclosure gate, which fast mode's plain binary32 terms are not meant to meet, is
skipped for this diagnostic and the skip is printed.)

**Tolerance reasoning.** The two binary32 owners implement the same discretisation; differences come from operation
order (GPU threadgroup reductions, fused multiply-add on the GPU, `-ffast-math` contraction on the CPU), not from the
model. A difference explained by rounding should be of the same order as each owner's own distance from the fp64
mirror, and both should be small relative to the field magnitude (binary32 ε = 1.2e-7, amplified by the Picard
iterations and the EOS inversion). Measured: the CPU32 owner is 1–2 binary32 ulps (ε = 1.2e-7) of field size from the fp64 mirror; the
Metal owner is 8–27 ε from both, so the Metal–CPU difference is the Metal owner's own distance from fp64, not
noise in the CPU path. For the two soot traces (≈1e-14 kg/m³, cancellation-dominated) both binary32 owners are far
from fp64 but within 1e-15 of each other, which is what one binary32 algorithm on two machines looks like. A
rounding-only tolerance of a few ε relative to field maximum is **exceeded by 4–10×** for the major scalars and
velocity. Per the brief the tolerance was not widened, and the cause was not isolated. Candidates, none excluded:
(a) GPU reduction order and arithmetic accumulated over the Picard iterations and the multigrid pressure solve (the
kernels compile with `MTLMathModeSafe`, so not fast-math approximations); (b) the two owners stopping on different
Picard iterates inside the same projection tolerance; (c) a scheme difference: since r264 (`a0e0055ba`, default since
2026-10-28) the Metal owner carries interior momentum with a centred gas-mass flux, while the CPU owner's
compatible momentum still uses the FCT-accepted gas flux (`EvaluateFireProductionCompatible*MomentumCPU`); on this
fixture velocities are below 0.07 m/s, so momentum advection is small and (c) may be hiding.

**What this does not show.** It is one step on one 64-cell fixture through a test-only request adapter; it does not
run the driver, the source build, frames or checkpoints on the CPU, and it is not several steps. At r239 the same
diagnostic reported 0 unequal words in 7 of 9 components; it now reports unequal words in every component, so the
two owners have drifted apart since r239 (the Metal owner gained r249–r275 changes, some of which, such as the
centred momentum carrier above, have no CPU counterpart; others, such as the negative-zero normalisation in
`fct_commit_scalar` (`c77b5d7b9`), are GPU-specific). Since 2026-10-01, 14 commits changed the Metal owner files and 4
changed the CPU owner files. Whether every other default-on change is mirrored was not established.

**Cost (one owner step, 64 cells).** CPU32 owner 10.6 ms, fp64 mirror 11.1 ms, Metal owner 82 ms certified and 48 ms fast mode (warm repeat;
first call 83 / 49 ms), one thread, M1 Max. At 64 cells the GPU figure is dispatch and synchronisation overhead, so
the CPU is 5–8× faster here; this says nothing about production sizes. A larger size could not be measured on the CPU: no larger owner
fixture exists, and the CPU owner cannot be driven by the solver. For scale, a fast-mode Metal step at tier 8 costs
about 1.9 s ([r274/r275](FIRE_SMOKE_SPEED_R274.md)).

## 7. Labels

| Solver | Backend | Label | Evidence |
|---|---|---|---|
| `fire.methane` | `metal` | supported (unchanged) | M0–M2 references, fire fast tier |
| `fire.methane` | `cpu` | **unsupported** (unchanged; **experimental since §10**) | §5: the step exists only as a one-step calibration owner the driver cannot call; no solver-level agreement run. Detail text now says so. |
| `cloth.shell`, `rigid.bodies`, `liquid.mac` | `cpu` | supported (unchanged) | Their fast-tier tests pass on macOS with and without Metal (§3). Detail text now adds "built and tested on macOS (with and without Metal), Windows and Android builds not yet compiled": `liquid.mac` did not compile with MSVC before §4.1 (6, 7), so a platform-free "supported" was not true. |

The contract's `backends` entry has no per-platform field; the detail text is where the platform evidence goes. Labels,
details and `backend` are not keyed, so no cache key changes.

## 8. Verified versus inferred

| Claim | Basis |
|---|---|
| Codecs and container decode the committed bytes and re-encode them exactly on macOS arm64 | Test run |
| They do the same on Windows/Android | Inferred from the code (byte-wise LE) and the audit; the fixture is there to prove it on first run |
| Library, CLI, `rise-sim`, tests build without Metal; non-fire tests pass | Built and run on this Mac (§3) |
| Windows/Android compile | Not compiled; read only (§4) |
| Fire CPU owner agrees with Metal on 64 cells | One diagnostic run per mode (§6) |
| Fire CPU backend | Not built |
| Metal path unchanged | Full fast tier in the checkout with Metal: 464 of 464 passed (463 before plus the new test), fire tests and their reference baselines included; build warning-free |

## 9. Open items

(Items 1 and 5 are done in §10; §10.7 lists what remains, and §11.10 supersedes §10.7 items 1–4.)

1. Fire CPU backend: transport provider and request/result adapters in the library, a driver backend switch with
   CPU-only memory admission, a thread pool with fixed-order reductions, a working-set limit sized to the machine,
   an audit that every default-on Metal change since r239 is mirrored, then the solver-level Metal-fast vs CPU run over
   several steps and the fast-tier determinism/resume test the M8 brief describes.
2. `rise-sim` targets for VS2022 and Android, and a first Windows run of the process host (§4.2).
3. Windows ARM64 configurations.
4. A first Windows and Android run of `SimulationGoldenCacheTest`.
5. A skip, not a failure, for the four Metal-only fire test programs in a build without Metal.

## 10. M8 part A finished: the fire CPU backend (2026-11-26, later the same day)

**Outcome.** `fire.methane`'s CPU backend is a runnable backend and is labelled **`experimental`**. The run
driver calls it through the same request, result, accepted-step token, frame, checkpoint and cache code as Metal
(parameter `backend: "cpu"`), it runs in a build with Metal compiled out, its step is threaded and bit-identical at
any worker count, and it agrees with Metal fast mode on the coarse case within a tolerance set from the measured
curve (§10.3). **The M8 exit criterion "CPU fire agrees with Metal fast mode within tolerance on a coarse case" is
met**, with the qualifications in §10.3 (not bit-identical; a localised step-2 difference whose trigger is not
isolated). This section supersedes the Part A bullet of the outcome at the top, §5's "Missing" rows, §6's cause
discussion and candidates, §7's `cpu` row and open items 1 and 5 of §9.

### 10.1 Cause of the one-step gap: r264's centred momentum carrier (isolated)

`FireSequenceTest --fire-production-burner-bisect` (new) runs the burner fixture of §6 through the CPU32, fp64 and
Metal owners with the qualification trace on (the trace does not change the Metal state: checked, bytes equal) and
compares every operand of every Picard-map evaluation, stage by stage. With the historical CPU carrier the first
operand that departs from rounding is R0 iteration 0's `advectionMomentumRate`: 10 % of its magnitude (2.2e-2 of
0.22 kg m⁻² s⁻²) while every earlier operand (projected velocity, transport coefficients, scalar fluxes, FCT alpha,
accepted state, EOS ratio, target, buoyancy, stress, phase source) agrees to 0.7–3.4e-7 relative. Downstream the
momentum difference flips one pressure-open face class at R1 (`s0f9`), which perturbs the R1 Picard path; the
Picard map then contracts back, leaving the 0.9–3.2e-6 the M8 note reported. That is candidate (c) of §6, and it
alone: with the r264 centred carrier on the CPU side (`--fire-production-burner-reference-centered`):

| Field | Metal − CPU32 (max abs) | Unequal words | CPU32 − fp64 |
|---|---:|---:|---:|
| components 0–5 and 8 (densities, energy) | **0** | **0 / 64** each | 5.3e-10 – 0.045 J m⁻³ (1–2 ulps) |
| components 6, 7 (soot traces, ~1e-14) | 9.5e-16, 5.4e-16 | 64 / 64 | 4.1e-14, 1.4e-14 |
| velocity x, y, z | 3.7e-9, 7.5e-9, 3.7e-9 m/s (≤ 2 ulps of 0.07 m/s) | — | 1.1e-8, 6.7e-9, 5.9e-9 |

Fast mode gives the same table. Both owners now take the same Picard iterations (R0 2, R1 2, R2 4) and the same
open-boundary classes at every evaluation. Every intermediate operand differs by at most 3.1e-7 of its scale
(about 2.6 binary32 ε), from operation order (GPU contraction, the GPU's binary32 nullspace projection of the
physical flux where the CPU projects in binary64 and rounds, threadgroup reductions); the accepted scalars come out
bit-identical because the step (2⁻¹⁶ s) puts those flux differences below half an ulp of the state. The soot rows
are cancellation-dominated traces 1e-16 apart in absolute terms (eight orders below an ulp of the gas density).
**The earlier claim that the Metal owner is 8–27 ulps from fp64 was the carrier difference, not Metal rounding:**
with the same scheme Metal is as close to the fp64 mirror as the CPU owner is. Picard stopping iterates and GPU
reduction order (candidates a, b) contribute nothing visible on this fixture.

Other default-on Metal changes since r239 (r240–r275 docs and the 2026-10-01 onward commits to the owner files):
none changes the arithmetic besides r264. `c77b5d7b9` canonicalises a subnormal-flushed −0 to +0 in
`fct_commit_scalar` (no value change); r274 is byte-identical by construction; fast mode (r275) is opt-in; the r271
variants are opt-in; r250/r270/r272/r273 are source-side, shared CPU code. One more Metal/CPU difference surfaced only
on a real grid (§10.3): the resident owner projects with 17 V-cycles when a boundary is pressure-open, doubling to 64
while validation fails (r226), where the CPU owner's projections used the standalone schedule of 16 without retry.

### 10.2 What changed

| Part | Change |
|---|---|
| CPU owner (`FireProductionForce/Transport/Projection.cpp`, mirrored) | Request flags `centeredMomentumCarrier` (the r264 carrier `0.5(ρL+ρR)u + J_g` on interior primal faces, accepted carrier on domain faces, the same expression and summation order as `fct_centered_momentum_carrier`; R0 term from Qⁿ, R1 from the predictor, R1 alpha on boundary faces, as Metal), `residentProjectionSchedule` (the resident V-cycle schedule above, through `FireProductionProjectionRequest::cpuResidentSchedule`) and `workingSetLimitBytes` (0 = the historical 2 GiB). All default off, so the calibration fixtures, the fp64 mirror's defaults and every existing test run the historical reference unchanged. The owner result also returns the Heun provisional momentum. |
| EOS acceptance (CPU only) | Inverts over the record's whole temperature range (then refuses outside the case bounds) instead of over the case bounds: the inversion's binary32 result depends on the bracket, and the R1 base target recomputes it over the record range and demands equality; on the tier-4 run the two differed by one ulp in a cell at step 6 and the step was refused. Metal is unaffected (it has its own EOS). |
| `Simulation/Fire/FireCPUBackend.{h,cpp}` (new; all five build lists) | The transport provider (the CPU transport authority of the r190 reference, threaded), the request adapter (Metal owner request → CPU owner request), `AttemptFireProductionProjectedHeunResidentStepCPU` (runs the owner, publishes `FireProductionResidentStepResult` with the accepted-step token exactly as the Metal entry does, and a diagnostics record with the fast-mode conservation-ledger packet), option refusals, working set and admission. |
| `FireRunDriver.cpp` | `persistence.cpuBackend` (set by the launch plan from `RISE_FIRE_BACKEND=cpu`) switches the one owner call; refuses the Metal-only drivers (r245/r246 studies, replays, diagnostics) and opt-in operators; CPU memory admission from the CPU certificate; a zero-step case probe no longer needs the Metal certificate (Configure works in a non-Metal build). Everything after the step (EOS inversion, frames, checkpoints, budgets, the fast-mode ledger) is shared. The receipt adds `backend cpu-m8-v1` for CPU runs only. |
| `FireRunHostMemory.h` | Physical memory and RSS on Linux/Android (`/proc/self/statm`, `sysconf`) and Windows (`GlobalMemoryStatusEx`, `K32GetProcessMemoryInfo`), read but not compiled; macOS unchanged. |
| `FireSolver.cpp` | Parameter `backend` (`"metal"` default, `"cpu"`); `"cpu"` enters the effective parameters (so the key), `"metal"` or absent does not (every existing key is unchanged). Writes `RISE_FIRE_BACKEND` with the other options. `cpu` is `experimental`, available unless a Metal-only option is set; `capabilities.backend` says which runs; the CPU backend is `deterministic` in both modes. Tier 4 (35×35×53) is accepted as a coarse smoke tier (not a capstone tier; not in the scene schema). The `backend` parameter is not in the scene schema either: adding it there would change completion and so every fire scene key; scenes keep Metal. |
| Threading | `FireProductionCPUParallelFor` (a persistent pool; disjoint outputs per index; maxima combined after; first failure by index) in the scalar FCT stages (reconstruction, low state, limiter ratios, shared alpha, accepted state and both envelope checks), the physical-flux prerequisite, the projection smoother, operator and prolongation, and the transport provider. Left serial: content hashing (FNV over every published payload, about a third of a certified step, sequential by construction), the commuting-identity check, multigrid restriction, the EOS acceptance and target composition loops, the driver's source build (already eight workers). |
| Hashing | The flux stage's content identity is not recomputed immediately after it was computed (same object, same value); in fast mode it is a lineage token over the stage inputs, as Metal fast mode's seals are. |
| `FireProductionAdvectionMac.mm` | `RISE_FIRE_LIMITED_MOMENTUM_CARRIER` is read through the shared `FireSim::FireLimitedMomentumCarrier()` (same parse, same once-per-process cache). The only Metal-source change. |
| Make | `-Wl,-U,_RISEProjectedHeunOwnerTestFailureProbe` in `Config.OSX` (as the Xcode targets have): shipped binaries now link the CPU owner and its weak test probe. |
| Tests | `FireCPUBackendTest`, `FireCPUMetalAgreementTest` (new, §10.4); `FireSequenceTest --fire-production-burner-bisect[-centered]` and `--fire-production-burner-reference-centered`; `FireSolverTest` expects `cpu experimental 1`; `FireSequenceTest`, `FireProductionSolverTest` and `FireProductionProjectionTest` print SKIP and pass without a Metal device. |

### 10.3 Agreement over several steps (coarse case, tier 4, fast mode)

`FireCPUMetalAgreementTest` (and `FireCPUBackendTest --agreement N`, `--agreement-fixed-vcycles N`) runs the tier-4
case from zero on Metal and on the CPU through the solver interface and compares the accepted states after every
step: species and density rows over the maximum gas density, energy over max |E|, temperature over its maximum,
face velocity over the maximum speed (0.22 m/s). Per-species maxima are not useful scales here: the products are
traces early in the run.

| Step (dt 1.646 ms) | 1 | 2 | 3 | 4 | 5 | 8 | 12 | 16 | 20 | 24 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| species, densities (max) | 3.1e-7 | 3.7e-5 | 7.8e-5 | 1.1e-5 | 9.9e-6 | 6.1e-6 | 3.9e-6 | 2.9e-6 | 2.1e-6 | 1.8e-6 |
| temperature | 1.1e-9 | 4.9e-5 | 1.0e-4 | 1.5e-5 | 1.4e-5 | 8.3e-6 | 5.6e-6 | 4.2e-6 | 3.2e-6 | 2.7e-6 |
| energy | 3.0e-7 | 2.8e-3 | 4.0e-3 | 4.4e-4 | 3.0e-4 | 1.1e-4 | 5.0e-5 | 2.7e-5 | 1.7e-5 | 1.2e-5 |
| velocity | 2.0e-4 | 1.9e-3 | 5.0e-3 | 4.4e-3 | 8.5e-5 | 8.5e-5 | 2.5e-4 | 9.7e-5 | 1.5e-5 | 1.3e-5 |

(Metal in its default fast mode; CPU fast mode, which is the certified CPU arithmetic without per-step seals: the
certified CPU run gives the same numbers to two digits.) Reading it:

- **Step 1.** With Metal's V-cycle count fixed (`fast_fixed_vcycles`) the step agrees to rounding: species ≤ 1.0e-7,
  temperature 7e-10, velocity 4.8e-7 (about 1e-7 m/s). In default fast mode Metal stops the pressure V-cycles early
  once the validation band is met (r275); the remaining 2.0e-4 (4.4e-5 m/s) is that solve's slowest mode near the
  upper open faces, the same pattern and size as Metal adaptive against Metal fixed (4.4e-5 m/s) and as the CPU
  owner's old 16 against 17 V-cycles (1.7e-5 m/s). It is inside the projection's validation band. The CPU backend
  does not implement the adaptive stop.
- **Steps 2–4.** Four cells on the burner surface (z = 0, x 16–19, y 15–17) differ by 0.015 K (temperature 4.9e-5 of
  its maximum, energy 2.8e-3 of max |E|, which is small near ambient), and the velocity near them by up to 7e-4 m/s;
  nothing else moves above rounding. The same happens with Metal's V-cycles fixed and in certified mode, so it is
  not fast mode. A 1e-7 difference after step 1 becoming 0.015 K in a few cells is a discrete decision in the step
  (a source, limiter or boundary-class switch at the burner) taking different branches. **Which one is not
  isolated**; candidates are the frozen source's pilot/ignition switches (shared CPU code, fed the two backends'
  temperatures), the FCT limiter's active inequality, and the burner faces' classes.
- **After step 4** the difference does not grow: it decays to 1–3e-6 (scalars) and 1e-5 (velocity) of the field scale
  by step 24 (40 ms). This early, still laminar phase does not show chaotic divergence; whether and when the
  turbulent plume decorrelates the two trajectories, and how its statistics then compare, was **not run** (a CPU run
  to the first statistics frame at tier 4 is about 2 s × 6,000 steps).

**Tolerance.** Per step, of the field scale: species and densities 2e-4, temperature 3e-4, energy 1e-2, velocity
1e-2 (1e-2 of 0.22 m/s is about 1.5 times the owner's own endpoint velocity tolerance, 1e-3·√(g D*) = 1.5e-3 m/s).
These are about 2.5 times the 24-step maxima above; they hold the agreement to the owner's convergence tolerances,
not to rounding, because from step 2 the difference is set by the discrete event, not by rounding. The test checks
steps 1–5, which contain the peak.

### 10.4 Tests

| Test | What it checks | Time |
|---|---|---:|
| `FireCPUBackendTest` | Tier 4 through the solver interface in child processes (in-process host): the description (`backend cpu`, `experimental`, available, deterministic); 1 and 8 workers accept bit-identical states (2 steps); restoring the step-1 checkpoint with 4 workers and taking one step equals step 2 of the 1-worker run bit for bit; finite, positive gas density, methane entering, 300 ≤ T < 2300 K, upward velocity (max 0.20 m/s); the CPU key differs from the Metal default's; an invalid `backend` and the compact stencil on the CPU backend are refused (the latter at Configure). Needs no Metal. | 18 s |
| `FireCPUMetalAgreementTest` | §10.3, five steps; skipped where Metal is absent | 18 s |

Determinism was also checked by hand in certified mode (1, 3 and 8 workers, 3 steps, identical bytes, and identical
to the unthreaded build's bytes) and with the pool against the spawn-per-call threading (identical).

**Without Metal.** A copy of the tree built with Metal compiled out (§3's method, under
`/Volumes/SSD/claude-tmp/m8b-scratch/nometal`) builds warning-free; `otool -L` shows no Metal framework in
`FireCPUBackendTest` or `rise_sim`; `FireCPUBackendTest`'s CPU checks run and pass there (§10.6).

### 10.5 Cost (M1 Max, 8 workers, fast mode, wall seconds per accepted step)

| Case | Cells | CPU | Metal | CPU / Metal |
|---|---:|---:|---:|---:|
| tier 4 | 64,925 | 2.0 (certified 3.2; 1 worker 3.5 fast, 4.7 certified) | 0.19 | 11 |
| tier 8 | 504,666 | 14.5 | 0.96 | 15 |

Tier 8 CPU peak RSS 3.3 GB (certificate 1.8 GB plus the driver's state and source arrays; admission is by RSS, see
below). Eight workers give 1.5× (certified) to 1.7× (fast) over one: about half of a certified step is serial
(hashing, validation scans, EOS loops). A 25 s tier-8 window (about 15,000 steps) would take about 60 hours on the
CPU against about 4 on Metal.

**Memory admission.** The CPU step's working set is the CPU owner certificate plus the adapter's copies; the 2 GiB
cap is replaced by the host ceiling (half of physical memory, at most 32 GiB, the production host policy), and a
grid whose certificate plus 2 GiB of scratch exceeds it is refused at Configure with the sizes in the message. The
per-kernel two-GiB output checks inside the CPU operators remain; they bind only beyond tier 10 (the largest
single kernel output at tier 10 is about 0.4 GiB).

### 10.6 Verified versus inferred

| Claim | Basis |
|---|---|
| The one-step gap was the centred carrier | Bisection over every Picard operand; with the carrier the fixture is bit-identical in 7 of 9 rows (run, both modes) |
| No other default-on Metal scheme change since r239 is missing | Read: r240–r275 docs and the owner-file commits since 2026-10-01; the resident V-cycle schedule found by measurement (§10.3) |
| Metal output bytes unchanged | Full fast tier in the checkout with Metal: **466 of 466** passed (464 before plus the two new tests), every fire test and reference baseline included; the only Metal-source edit reads the same option through a shared function |
| CPU backend bit-identical at any worker count; resume bit-identical | Run (1/3/8 workers certified and fast; 1/8 workers and a 4-worker resume in the test) |
| Agreement with Metal fast mode on tier 4 | Run, 24 steps (§10.3); beyond 24 steps and in the turbulent phase not run |
| Works without Metal | No-Metal copy: library, `rise`, `rise-sim`, `simulation_cache_import`, all tests built warning-free, no Metal framework linked; `FireCPUBackendTest`, `FireCachePlaybackTest`, `FireSimulationSolverTest`, `FireProductionCalibrationTest`, `SimulationSceneTest` pass; `FireSolverTest`'s configuration checks pass and it skips its bakes; `FireSequenceTest`, `FireProductionSolverTest`, `FireProductionProjectionTest` and `FireCPUMetalAgreementTest` skip cleanly |
| Linux/Windows/Android host memory telemetry, the Windows driver thread | Written or read only, not compiled; `FireSolver` still refuses to start a fire run on Windows (no large-stack driver thread there) |
| Builds warning-free | Make build of `all tests rise_sim simulation_cache_import`, with and without Metal; mirrors' `--check` passes |
| Cost | Three-step runs per case (§10.5); one machine |

Pins: chunk keywords 191 and `RewireConnectionTest` 6 unchanged; no codec, manifest, checkpoint, key (Metal) or solver
version changed. `FireSolverTest`'s expectation changed from `cpu unsupported 0` to `cpu experimental 1` (the label
this work moves); no other existing expectation changed.

### 10.7 Open items

(Items 1, 2 and 4 are addressed in §11 and 5 is designed there; §11.10 is the current list.)

1. The step-2 burner-surface difference (§10.3): find which discrete switch the 1e-7 step-1 difference flips.
2. Fast mode's adaptive V-cycle stop on the CPU side (Metal's r275 rule), so CPU fast mode matches Metal fast mode at
   step 1 as the fixed-schedule pair does.
3. A long Metal/CPU run into the turbulent phase, comparing statistics (centreline temperature and velocity, heat
   release, entrainment) once trajectories decorrelate.
4. Speed: about half of a certified CPU step is serial (FNV content hashes, validation scans, EOS and target loops);
   the hashes are sequential by definition, so a faster certified mode needs a cheaper identity, a protocol change.
5. Scene selection of the CPU backend (a schema parameter changes every fire scene's completed key) and a Windows
   driver thread; Windows and Android remain uncompiled (§4).
6. The CPU backend refuses the Metal-only opt-ins (compact stencil and its variants, scalar Vreman override, FCT alpha
   override, slope variants); porting them is separate work.

## 11. M8c follow-up: the step-2 cause, fast mode's early stop, a long run, cost (2026-11-26, evening)

**Outcome.**

- **The step-2 difference is isolated and is not a backend error.** A burner-rim ignition rule in the shared source
  build reads rounding residue: before the pilot is established a rim cell becomes reaction-eligible when its
  CO2+H2O exceeds the ambient zero, and after step 1 those products are signed residue of about 1e-12 kg/m³ that
  differs in sign between the backends in one cell, which then burns on Metal only (§11.1). From identical bytes
  both backends take steps 2–5 to binary32 rounding. Metal's bytes are unchanged; the rule is documented, not
  changed.
- **Agreement is now explained to rounding for steps 1–5 of a step from identical input, and for the first step
  of a trajectory in default fast mode** (CPU fast mode now takes Metal's early V-cycle stop, §11.3). A second,
  smaller owner-level difference appears from step 6 from identical input, in certified and fast mode alike, in
  eight burner-diagonal cells; it is localised, not isolated (§11.2).
- **Long run (tier 4, fast mode, 1,495 steps = 2.5 s):** the trajectories had not decorrelated by the end; the
  temperature RMS difference grows from 1e-5 K to 0.058 K with an e-folding time of about 0.43 s, and the window
  statistics over 1.5–2.5 s agree to 1e-5–1e-4 (§11.5).
- **Cost:** the CPU content hashes give identical digests through a sliced FNV-1a (§11.6); the EOS acceptance and
  base-target loops are threaded. Tier 4 certified 2.2 s per step (3.2 before), fast 1.6 (2.0); tier 8 fast 11.6
  (14.5), certified 16.0.
- **Scene selection:** a design is in §11.7 (not implemented).

### 11.1 The step-2 difference: a source-build switch read from rounding residue (isolated)

**Method.** `FireCPUBackendTest --splice-checkpoint SOURCE TARGET OUT` (new diagnostic) rewrites one solver
checkpoint's configuration digest to another configuration's, so either backend's accepted state can be restored on
the other and both take the next step from identical bytes. Certified mode, tier 4:

| Comparison (state after step 2) | Temperature, max abs | Velocity, max abs | Unequal T words |
|---|---:|---:|---:|
| Metal resumed from its own step 1 vs Metal uninterrupted | 0 | 0 | 0 |
| CPU resumed from its own step 1 vs CPU uninterrupted | 0 | 0 | 0 |
| Uninterrupted Metal vs uninterrupted CPU | **0.0149 K** (cell 19,16,0) | 3.8e-4 m/s | 52 |
| Both from **Metal's** step-1 bytes | 5.7e-13 K | 7.5e-9 m/s | 5 |
| Both from **CPU's** step-1 bytes | 5.7e-13 K | 7.5e-9 m/s | 2 |

So the step itself agrees to rounding on either input, and nothing in the driver between steps differs: the
step-2 difference is the step's response to the step-1 difference (≤ 2e-7 K, 9e-8 m/s). The response is discrete:
from Metal's step-1 state cell (19,16,0) burns (CO2+H2O 2.2e-6 kg/m³, T 305.2298 K), from the CPU's it does not
(−1.2e-11, 305.2149 K), on both backends.

**The switch.** `FireProductionCanonicalSourceAuthority::Build` (`FireProductionSource.cpp`, shared CPU code, the
same rule as the driver's `ProductionEstablishedFlameHolderEligible`): before the pilot is established, a burner
rim (source-boundary contact) cell is made reaction-eligible when it passes the stoichiometric CFT test and its
product mass fraction (CO2+H2O)/ρ is at least `nextafter(ambient product fraction)`. The ambient has no CO2 or
H2O, so the witness is the smallest positive double: any positive product trace. After step 1 nothing has reacted
anywhere, and the product densities are signed rounding residue of the transport step: |ρ_CO2+ρ_H2O| ≤ 2.7e-11
kg/m³ (against 1.2 kg/m³ of gas; four orders below a binary32 ulp of the gas density), positive in 40 % of cells
on either backend, with signs that differ cell by cell between the backends. A temporary trace of the rule
(removed) printed, at the step-2 source build, the rim cells that pass: the same eight on both backends
(541–543, 610, 645, 649, 681, 682, residues 1.9e-12 to 7.7e-12 kg/m³ agreeing to three digits) plus cell 579 =
(19,16,0) on Metal only, residue +1.31e-12 there against −9.74e-13 on the CPU. That cell ignites on Metal.
Confirmation by intervention (temporary, removed): with the witness raised to a product fraction of 1e-9 on both
backends (no rim cell eligible from residue), certified Metal and CPU agree after step 2 to rounding
(T 1.4e-9 of its maximum, velocity 3.3e-6 of the maximum speed, against 4.9e-5 and 1.7e-3), and no cell is
eligible in either run in the first six steps.

**Which side is right.** Neither, and neither is wrong: both backends evaluate the same shared rule on their own
accepted state, and both states are correct to rounding. In exact arithmetic the residue is zero, the rule is not
met, and no rim cell would ignite at step 2; the nine cells that do ignite (eight on both, one more on Metal) are
ignited by rounding residue on both backends. The defect is the rule's witness, which compares a quantity that is
zero in exact arithmetic with zero. Changing it (a witness above the residue, e.g. a product fraction of order
1e-9) would change Metal's bytes and the certified trajectories, so per the brief it is documented here, not
changed (open item 1).

### 11.2 One step from identical bytes, steps 1–7, and the remaining step-6 difference

`/Volumes/SSD/claude-tmp/m8c-scratch/tools/onestep.sh` (scratch): Metal runs 7 steps with a checkpoint after each;
the CPU backend restores each Metal checkpoint (spliced) and takes one step; the result is compared with Metal's
next state. Relative to the field scale as in §10.3 (species over the maximum gas density):

| Step taken | Certified: species / T / E / velocity | Fast: species / T / E / velocity |
|---|---|---|
| 1 → 2 | 1.9e-13 / 1.9e-15 / 6.3e-10 / 3.4e-8 | 2.5e-8 / 3.7e-15 / 1.8e-12 / 6.8e-8 |
| 2 → 3 | 5.1e-8 / 5.9e-15 / 8.4e-10 / 6.8e-8 | 9.7e-14 / 5.9e-15 / 4.2e-10 / 3.4e-8 |
| 3 → 4 | 2.5e-8 / 3.7e-16 / 1.3e-9 / 6.8e-8 | 4.3e-12 / 3.7e-16 / 6.3e-10 / 6.8e-8 |
| 4 → 5 | 5.1e-8 / 3.6e-16 / 1.0e-9 / 6.8e-8 | 5.1e-8 / 3.8e-14 / 1.0e-9 / 6.8e-8 |
| 5 → 6 | 5.1e-8 / 8.5e-8 / **6.9e-6** / **2.7e-5** | 1.0e-7 / 8.9e-8 / **6.9e-6** / **2.7e-5** |
| 6 → 7 | 1.0e-7 / 2.0e-7 / **7.3e-6** / **3.8e-5** | 1.0e-7 / 2.0e-7 / **7.3e-6** / **3.8e-5** |

(A velocity of 6.8e-8 of the maximum speed is 1.5e-8 m/s, one or two binary32 ulps.) Steps 1–5 agree to rounding
in both modes. From step 6 (t = 8.2 → 9.9 ms) the CPU step differs from Metal's by about 6e-6 m/s and 7e-6 of
max |E|, the same in certified and fast mode, so it is not fast mode. It is localised: the largest energy
differences are −0.125 J/m³ (step 6) and −0.154 J/m³ (step 7) in the same eight cells, the z = 0 cells
(13,15), (13,19), (15,13), (15,21), (19,13), (19,21), (21,15), (21,19) diagonal to the burner's corners, where
E ≈ 458 J/m³ (0.125 is about 4,000 ulps) and the accepted temperature stays at 300.000 K; 48 cells (step 6) and
240 (step 7) differ by more than 1e-3 J/m³. Metal is the self-consistent side here: from Metal's certified step-5
bytes, Metal fast mode's step 6 equals Metal certified's to 1.1e-7 of max |E| and 1.5e-5 of the speed (its
early V-cycle stop), while the CPU's differs by 7e-6 / 3e-5. The CPU's R2 Picard residuals (`RISE_FIRE_PICARD_TRACE`, fast mode with fixed
V-cycles) show it: from the step-5 bytes they are 0.012656, 0.004891, 7.2e-6 s⁻¹ against Metal's 0.012563, 0.004891, 7.2e-6 (from the step-4 bytes,
where the step agrees, the first residual is identical to 9 digits on both). So the R2 initial endpoint target
already differs, and the cause is upstream of R2 (R1 accepted state, transport, or the first endpoint
projection) in those eight cells. **Not isolated**: it needs the §10.1 operand bisection on a tier-4 step-6 owner
request, which was not done (open item 2). Which side is right is not established. It is three orders of
magnitude below the step-2 event and does not change the agreement bound of the trajectory test.

### 11.3 Fast mode's early V-cycle stop on the CPU side

`ProjectFireProductionCPUImplementation` (`FireProductionProjection.cpp`, mirrored) now applies Metal's r275 rule
(`FireProductionProjectionMac.mm`): in fast mode (`FireFastAdaptiveVCycles`, so `fast_fixed_vcycles` turns it off
on both), for a resident owner projection (`cpuResidentSchedule`) with an open boundary and at least 13 scheduled
cycles, not a restoration, after cycles 11, 13, 15, … below the schedule, correct the faces from the current
pressure, take the post residual and the maximum velocity (provisional and corrected, as Metal's validation-metrics
kernel), and stop when `FireProductionProjectionResidualWithinBand` holds. The final correction is then the
unchanged code, now one lambda shared with the trial (same arithmetic). One adaptation: every Metal owner
projection preserves its open class and derives the endpoint class from the final velocity afterwards, while the
CPU owner's R2 iterates derive it inside the projection; the CPU therefore applies the stop to endpoint-classified
projections too (excluding them, as a first version did, left the R2 iterates at 17 cycles and the terminal
projection adaptive, so the terminal verification never met the projection tolerance and R2 did not converge).
Certified mode and the calibration owners (no `cpuResidentSchedule`) are untouched.

### 11.4 Agreement re-measured, and the test

Default fast mode on both backends (CPU now with the early V-cycle stop), tier 4, three runs, identical every
time (`FireCPUBackendTest --agreement 5`):

| Step | 1 | 2 | 3 | 4 | 5 |
|---|---:|---:|---:|---:|---:|
| species, densities (max) | 5.1e-8 | 1.0e-7 | 7.6e-5 | 9.8e-6 | 8.3e-6 |
| temperature | 4.5e-10 | 2.3e-9 | 1.0e-4 | 1.3e-5 | 1.1e-5 |
| energy | 1.6e-9 | 8.0e-8 | 3.9e-3 | 3.7e-4 | 2.6e-4 |
| velocity | 6.2e-7 | 3.5e-6 | 3.6e-3 | 4.0e-3 | 8.0e-5 |
| one step from Metal's step-2 bytes | | | species 1.3e-13, T 5.9e-15, E 4.2e-10, velocity 3.4e-8 | | |

Step 1 is now rounding (velocity 2.0e-4 before, the early-stop difference). In this run the residue-driven rim
ignition happens at step 3 instead of step 2 (§10.3): which step and which cell depend on the residue's signs, and
the event's size is the same.

`FireCPUMetalAgreementTest` now checks, besides the §10.3 per-step bounds for steps 1–5 (kept: they bound one
rim-ignition event, now explained, 2.6 times its measured peak):

- step 1 to rounding: species 1e-6, temperature 1e-7, energy 1e-6, velocity 1e-5 of the field scale (measured
  5.1e-8, 4.5e-10, 1.6e-9, 6.2e-7);
- Metal's accepted step-2 bytes restored on the CPU backend (spliced checkpoint) and one step taken: equal to
  Metal's step 3 within the same rounding bounds (measured 1.3e-13, 5.9e-15, 4.2e-10, 3.4e-8).

The tighter bounds are about 16 times the measured velocity difference; they would catch a change in the step's
arithmetic or in the early stop, which the event-sized bounds would not. 20 s in the fast tier (18 s before).
The early stop's decision could in principle fall on different sides of the band on the two backends in some
step (a near tie); none happened in the steps measured, and the identical-start check uses step 2→3 only.

### 11.5 Long run: tier 4, fast mode, 1,495 steps (2.5 s), both backends

`FireCPUBackendTest --long-run-child 4 {0,1} 1 3.2 DIR 1495 5 10` (diagnostic, not a test): statistics every 5
steps, the binary32 state every 10. The CPU run took 3,609 s wall (2.4 s per step on a machine shared with other
agents' builds and tests), the Metal run 409 s. The step stays at the 1.646 ms source cap throughout on both, so
step k is the same time on both. Difference against step (field-scale maxima as §10.3; RMS absolute):

| Step | t (s) | species | E | T max | T RMS (K) | velocity max | velocity RMS (m/s) | max speed (m/s) | max T (K) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 10 | 0.016 | 4.0e-6 | 6.4e-5 | 5.9e-6 | 8.8e-6 | 6.3e-5 | 4.9e-7 | 0.22 | 327 |
| 100 | 0.16 | 6.4e-6 | 1.3e-5 | 1.0e-5 | 5.7e-5 | 4.2e-5 | 4.2e-7 | 0.40 | 763 |
| 200 | 0.33 | 1.4e-5 | 1.7e-5 | 1.1e-5 | 1.7e-4 | 3.1e-5 | 5.0e-7 | 0.83 | 1,620 |
| 300 | 0.49 | 2.5e-5 | 3.7e-5 | 3.4e-5 | 5.7e-4 | 7.8e-3 | 1.0e-4 | 2.1 | 1,770 |
| 500 | 0.82 | 7.4e-5 | 9.0e-5 | 4.6e-5 | 1.2e-3 | 4.5e-5 | 3.8e-6 | 5.3 | 1,820 |
| 700 | 1.15 | 7.3e-5 | 8.7e-5 | 4.0e-5 | 1.9e-3 | 1.8e-4 | 2.0e-5 | 8.1 | 2,080 |
| 900 | 1.48 | 4.0e-4 | 4.8e-4 | 3.4e-4 | 8.8e-3 | 4.8e-4 | 7.7e-5 | 8.5 | 2,100 |
| 1100 | 1.81 | 1.8e-3 | 8.1e-4 | 3.6e-4 | 0.015 | 2.3e-3 | 3.6e-4 | 8.1 | 2,000 |
| 1300 | 2.14 | 1.8e-3 | 1.8e-3 | 7.7e-4 | 0.039 | 9.0e-3 | 1.3e-3 | 8.2 | 2,070 |
| 1490 | 2.45 | 2.7e-3 | 3.6e-3 | 1.4e-3 | 0.058 | 1.5e-2 | 2.2e-3 | 6.8 | 1,490 |

(Full curve: `/Volumes/SSD/claude-tmp/m8c-scratch/longcmp.txt`, scratch.) Reading it: through the laminar start
(to about step 250, 0.4 s) the difference stays at 1e-5 of the field scale; once the plume accelerates (maximum
speed 2 → 8 m/s) it grows roughly exponentially, the temperature RMS by a factor of 100 between steps 300 and 1490,
an e-folding time of about 0.43 s; single-step velocity spikes (step 300: 7.8e-3) are transient. **By 2.5 s the two
trajectories have not decorrelated**: the largest temperature difference is 1.4e-3 of 2,000 K (about 3 K) against
an RMS of 0.06 K. At the measured growth rate the RMS would reach the plume's own fluctuation level (centreline
temperature standard deviation 50–300 K) after another 7–8 e-foldings, about 3.5 s more, so near t ≈ 6 s, roughly
3,600 steps, which is about 2.5 hours of CPU time at this load; not run.

Statistics over 1.5–2.5 s (120 samples each), Metal / CPU:

| Quantity | Metal mean (sd) | CPU mean | (CPU − Metal) / mean |
|---|---:|---:|---:|
| Heat release (frame field, W) | 32,157 (1,920) | 32,158 | 1.5e-5 |
| Domain mean temperature (K) | 309.194 (0.90) | 309.194 | 5.8e-7 |
| Maximum temperature (K) | 1,974.7 (174) | 1,974.7 | −8.6e-6 |
| Methane in the domain (kg) | 6.1508e-5 | 6.1508e-5 | −6.7e-6 |
| CO2 + H2O in the domain (kg) | 5.9155e-3 | 5.9156e-3 | 1.1e-5 |
| Volume above 500 K (m³) | 0.1052 (0.016) | 0.1052 | 7.3e-5 |
| Kinetic-energy measure | 4.678 (1.09) | 4.678 | 3.7e-5 |
| Centreline T, layers 0 / 8 / 16 / 32 / 48 (K) | 1597.9 / 1072.9 / 763.9 / 467.8 / 350.7 | same | +0.03 / −0.01 / +0.005 / −0.002 / −0.006 K |
| Centreline w, faces 4 / 16 / 32 / 48 (m/s) | 3.352 / 4.872 / 4.155 / 2.415 | same | +1e-5 / −4e-5 / −1.5e-4 / +5.8e-4 m/s |

(Heat release is the frame's per-cell field summed, about 32 kW against the case's 33 kW; centreline means the 2×2
column at the axis.) The statistics agree to 1e-5–1e-4 because the trajectories are still close, not because two
independent samples happen to match; this window does not yet test statistical agreement of decorrelated runs.

### 11.6 Cost: content hashing and the EOS/target loops

**Hashes.** Every CPU content identity is FNV-1a over bytes, which looks serial: h ← (h ⊕ b)·P. But ⊕ with a byte
touches only the low byte, so h ⊕ b = h + d with d = (l ⊕ b) − l, l = h mod 256, and the low byte evolves by itself
(l ← ((l ⊕ b)·P) mod 256). A slice of the input therefore needs only its starting low byte: each slice first
composes its low-byte permutation for all 256 starting values (NEON, sixteen 16-lane vectors), the slices' starting
low bytes follow in order, then each slice sums Σ d_i P^(m−i), and the slices combine as h·P^m + S. Zero padding
bytes (the owner hashes each binary32 word as a zero-extended 64-bit word) are one multiplication by P⁴. The digest
is the same value by construction; `FireProductionFNV1aWords` (`FireProductionAdvection.cpp`, mirrored) implements
it, used by `OwnerHashFloats` (Force) and `HashFluxValues`, `EOSValueDigest`, `HashSourceDoubleValues`,
`HashFluxBytes` (Transport) for arrays of 4,096 values or more. It runs on the CPU backend's workers only (one
worker elsewhere, serial; no NEON: serial). `FireCPUBackendTest` checks it against the plain byte loop for five
word layouts at 1, 3 and 8 workers; every existing calibration and qualification digest test passes unchanged.
One tier-8 state digest (4.5 M words): about 31 ms in the original loop (estimated from the 8 multiplications per
word), 19.6 ms serial with the zero batching, 9.1 ms on 8 workers (measured under load; the first pass, 32 vector
operations per byte, limits it).

**EOS and target.** `EvaluateFireProductionScalarEOSAcceptanceCPU` and both cell loops of
`ComposeFireProductionBaseDivergenceTargetCPU` (the canonical temperature inversion and the target composition) run
on the workers: each cell writes its own outputs, the first failure is the lowest failing cell
(`FireProductionCPUFirstFailure`, so the message equals the serial loop's), and the maxima are taken afterwards.

**Measured (M1 Max, 8 workers, wall seconds per accepted step; machine shared with other agents, load average 10–16):**

| Case | Before (§10.5) | Now | Steps measured |
|---|---:|---:|---|
| tier 4, certified | 3.2 | **2.16** | 4 (2.15–2.17) |
| tier 4, fast | 2.0 | **1.6** | 4 (1.54–1.82) |
| tier 8, fast | 14.5 | **11.6** | 3 (24.3 warm-up, then 11.57, 11.57) |
| tier 8, certified | — | **16.0** | 3 (17.7, 16.2, 16.0) |

Under heavier load (average 16–21) the same tier-4 runs took 4.0 and 2.6 s. A `sample` of a certified tier-4 run
still attributes 70 % of the busy samples (all threads) to the two hash passes, now spread over the workers: the
certified mode's per-step content identities remain its largest cost. A cheaper certified mode needs fewer or
cheaper identities (a protocol change), not more threads.

### 11.7 Choosing the CPU backend from a scene or host (design only, not implemented)

**How fire scenes key today.** A `fire_simulation` chunk is built from `FireParameterSchema()`
(`ChunkParserRegistry.cpp`, `SimulationChunks::Build`/`FinalizeSolverChunk`): every schema parameter becomes a
chunk field, and the authored fields become the CBOR parameter map. Fire is keyed by prediction, never configured
in the application (`SceneSimulation.cpp` `SceneSimulationGraph::Configure`): the schema's `completeParameters`
(`Complete` in `FireParameterSchema.cpp`) inserts every schema default that the scene does not state,
`predictEffectiveParameters` adds the derived frame window, and the key is `MakeCacheKey(solver, version,
predicted)`. The worker later calls `FireSolver::Configure` with the completed map; its effective parameters must
equal the prediction or the cache reads as stale. `FireSolver::Configure` already omits `backend` when it is
`metal` or absent and adds `backend: "cpu"` otherwise. The schema has a `keyed = false` flag, but only for
parameters that do not change bytes (`threads`, `pace_ms`); the backend changes bytes, so it must stay keyed.

**Why a plain schema parameter would change every key.** Added as an ordinary `Enum` with default `metal`,
`Complete` would insert `backend: "metal"` into every completed map, so every existing fire scene's predicted key
would change (and would also disagree with the solver's effective map, which omits it).

**Proposal: a keyed parameter that is omitted at its default.**

1. `SchemaParameter` gains `bool omitDefault = false` ("keyed only when it differs from its default"). Fire's
   `backend` is `Enum {metal, cpu}`, default `metal`, `omitDefault = true`.
2. Fire's `Complete` does not insert an `omitDefault` parameter that the scene leaves out, and removes it when the
   scene states the default; `Predict` therefore never sees `backend: "metal"`. Together with the solver's
   existing omission, the completed map, the predicted effective map and the worker's effective map agree for all
   three spellings (absent, `metal`, `cpu`).
3. Result: every existing fire scene, and any scene that writes `backend metal`, keeps its key byte for byte; a
   scene that writes `backend cpu` gets the CPU key (`FireCPUBackendTest` already checks that the CPU key differs
   from the Metal default's). The chunk gains one field (its description says "default metal; enters the cache
   key only as cpu"); chunk keywords stay 191. `FireEstimateCapabilities` already takes the backend for the
   memory estimate.
4. The change is local to the schema struct, fire's schema file and its tests; `SceneSimulation.cpp` and the
   generic completion are untouched (cloth and liquid have no `omitDefault` parameter).

**Host-level override (rejected as an unkeyed switch).** A host setting outside the key (say `rise-sim bake
--fire-backend cpu`, or honouring `RISE_FIRE_BACKEND` from the environment) would put CPU bytes under a Metal key:
the backends differ by 1e-4 of the field scale after a few steps and decorrelate later (§11.5), so a cache's frames
would depend on which host baked them, and a resume on the other backend would splice two trajectories into one
cache. If a host override is wanted (for example a build without Metal, where fire otherwise refuses at start), it
should be a host default for the keyed parameter: the host adds `backend: "cpu"` to the authored map before
completion, so the scene shows the cache under the CPU key (a different, initially missing cache), never a Metal
cache with CPU contents. No automatic fallback: a scene that does not say `cpu` on a host without Metal should keep
refusing with the existing message that names the parameter.

### 11.8 What changed

| File | Change |
|---|---|
| `src/Library/Utilities/FireProductionProjection.cpp` (mirrored) | Fast mode's early V-cycle stop for the CPU owner's resident projections (§11.3); the face correction is one lambda shared by the trial and the final publication; includes `FireSimulationRecords.h` for `FireFastMode.h`. The cycle loop keeps its single `VCycle` call and no `break` (the P2 source guard in `FireProductionProjectionTest` holds unchanged). |
| `src/Library/Utilities/FireProductionAdvection.{h,cpp}` (mirrored) | `FireProductionFNV1aWords`, the sliced FNV-1a (§11.6) |
| `src/Library/Utilities/FireProductionForce.cpp`, `FireProductionTransport.cpp` (mirrored) | Array hashes through it; EOS acceptance and base-target cell loops threaded (§11.6) |
| `tests/fire_production_fp64/*`, `tests/fire_production_trace/*` | Regenerated; `--check` passes for both generators |
| `tools/generate_fire_production_roundoff_trace.py` | The multigrid-cycle seam names the new loop body (two added lines) |
| `src/Library/Simulation/Fire/FireCPUBackend.cpp` | `RISE_FIRE_PICARD_TRACE` prints each stage's Picard residuals and cycle/limiter outcome (`PICARD_CPU`), as the Metal owner's trace does |
| `src/Library/Simulation/Fire/FireSolver.cpp` | `backendEnvironment` moved inside the `RISE_ENABLE_OPENVDB` block, its only user (an Xcode build without OpenVDB warned it unused); no behaviour change |
| `tests/FireCPUBackendTest.cpp` (and `FireCPUMetalAgreementTest`, same source) | Sliced-hash check; the agreement check of §11.4; diagnostics `--splice-checkpoint`, `--long-run-child`, `--hash-check`; the per-step run writes `checkpoint_K.bin` |

The temporary eligibility trace and the raised-witness intervention of §11.1 were in `FireProductionSource.cpp`
only for the measurement and are removed (the file is unchanged). No new source file, so no build list changed.
Metal source files are untouched; no codec, manifest, checkpoint, key or solver version changed. Pins unchanged.

### 11.9 Verified versus inferred

| Claim | Basis |
|---|---|
| The step-2 difference is the rim-ignition rule reading rounding residue | Cross-restores (both backends agree to rounding from either step-1 state); trace of the rule's decisions; intervention removing it (§11.1) |
| Neither backend is wrong at step 2 | Both evaluate the shared rule on states equal to rounding; in exact arithmetic the residue is zero (inferred from the absence of any reaction at step 1) |
| Steps 1–5 from identical bytes agree to rounding | Run, certified and fast (§11.2) |
| The step-6 difference's cause | **Not isolated**; localised to eight cells and to the R2 initial target (§11.2) |
| CPU fast mode takes Metal's early stop | Code read against `FireProductionProjectionMac.mm`; step 1 agrees to rounding in default fast mode, three identical runs |
| Sliced hash digests equal the serial digests | Test (5 layouts, 1/3/8 workers) and every calibration/qualification digest test unchanged |
| Metal output bytes unchanged | No Metal source changed. Fire fast tier after the last library change: 13 of 14 passed; `FireSolverTest` failed once (its stop/resume bake's frames and manifest differed from the uninterrupted bake while the final fire checkpoint was equal), on a machine where other agents were changing the simulation cache layer, and passed when rerun alone and in the run before. The earlier run's one failure (the P2 source guard) was fixed by restructuring the CPU loop, not the test. |
| CPU determinism at any worker count, resume | `FireCPUBackendTest` (1 and 8 workers, 4-worker resume), passing |
| Long-run growth and statistics | One 1,495-step run per backend (§11.5) |
| Cost | Short runs on a shared machine (§11.6) |
| Builds warning-free | `all tests build-tool/rise_sim build-tool/simulation_cache_import`, no warnings |

### 11.10 Open items (supersedes §10.7 items 1–4)

(Items 1 and 2 are addressed in §12.2 and §12.1, item 3 in §12.5, item 4 in §12.3, item 5's scene selection in §12.4;
§12.8 is the current list.)

1. The rim-ignition witness (`nextafter` of the ambient product fraction, §11.1) is decided by rounding residue on
   every backend. A witness above the residue would make step 2 independent of rounding; it changes Metal's bytes
   and the certified trajectories, so it needs its own decision.
2. The step-6 owner difference in the eight burner-diagonal cells (§11.2): run the §10.1 Picard-operand bisection
   on a tier-4 step-6 request (both owners from the same spliced state), then decide which side is right.
3. The long comparison to decorrelation (about 3,600 steps at tier 4) and statistics of independent trajectories.
4. Certified-mode cost is still dominated by content identities (§11.6).
5. Scene selection (§11.7) and the Windows driver thread remain design and open respectively.

## 12. M8d: the step-6 cause (a CPU bracket error, fixed), the rim-ignition fix for the re-baseline, cheaper certified hashing, a longer run, scene selection (2026-11-26, night)

**Outcome.**

- **The step-6 difference was a CPU error, now fixed.** The CPU owner's accepted-state EOS (and the R1 base target
  that recomputes it) inverted the energy row over the methane record's whole range, 300–5000 K, where the source
  build, the Metal owner and solver spec r74 use the case bounds, 300–2300 K. The binary32 inversion returns the
  lower bound whenever E ≤ E(T_min) + κε₃₂·scale, and the scale contains ρ·h(T_max), so the record bracket snaps
  every cell below about 300.71 K to 300 K where the case bracket snaps only below about 300.28 K. At step 6 the
  eight z = 0 cells diagonal to the burner corners are the first to warm into that band (300.376 K): Metal
  resolves them, the CPU called them 300 K, and the R1 transport coefficients there differed by 5–9e-4 (§12.1).
  With the case bracket on the CPU backend, one step from identical bytes agrees to rounding for every step 1→10
  in certified and fast mode (energy ≤ 2.3e-9 of max |E|, velocity ≤ 6.9e-8 of the maximum speed, against
  6.9e-6 and 2.7e-5 before). Metal is unchanged and was the right side.
- **Rim-ignition fix (for the approved re-baseline, not applied):** a product-fraction witness of 1e-6 (§12.2).
  With it, CPU and Metal agree to binary32 rounding along the whole trajectory through the ignition steps (10 steps
  measured), and on Metal the burner first burns at 0.13 s instead of from step 2–3, where today it is ignited by
  rounding residue. Implemented as a test-only solver parameter `rim_product_witness` (default off, keyed only when
  given); the executable checklist for the re-baseline agent is §12.2.4.
- **Certified hashing:** 66 % of the busy samples of a certified tier-4 step were content hashing; now 41 %
  (§12.3), with identical digests: binary32 values widened to binary64 hash three known-zero bytes as one
  multiplication, and an exact three-entry memo removes the re-hash of unchanged flux stages by every consumer that
  validates them. Tier 4 certified 1.63–1.70 s per step (2.16 in §11.6), fast 1.47–1.53 (1.6); tier 8 fast 11.0
  (11.6), certified 14.7–14.9 (16.0), on a machine shared with other agents (load average 11–35).
- **Long run (tier 4, fast, 7.04 s CPU / 8.0 s Metal):** the trajectories decorrelate locally by about 6 s
  (temperature RMS difference at the 14 K level of two independent Metal states); over 5.9–7.04 s the integral
  statistics agree to 0.2–2 %, while one 1.1 s window is too short to compare the decorrelated centreline (§12.5).
- **Scene selection of the CPU backend (owner-approved §11.7, done):** `fire_simulation` takes `backend cpu`;
  every existing fire scene keeps its key byte for byte (§12.4).

### 12.1 The step-6 difference: the CPU's EOS bracket (isolated, fixed)

**Method.** `RISE_FIRE_CPU_BISECT=1` (new diagnostic in `FireCPUBackend.cpp`, prints only): the CPU backend also
runs the Metal owner on the same request with the qualification trace and production stage tokens (as the
driver's crossing diagnostic does) and compares every operand of every Picard-map evaluation, stage by stage, with
the CPU owner's trace (the §10.1 bisection, on a real step), printing the worst cell or face of each operand;
`RISE_FIRE_CPU_BISECT_DUMP=<operand>` lists the cells that differ and, for a per-cell operand, both accepted
tuples and the CPU tuple's canonical inversion over both brackets. Driven by
`/Volumes/SSD/claude-tmp/m8d-scratch/tools/bisect.sh` (scratch): Metal certified to step K+1 with per-step
checkpoints, the CPU backend restored from Metal's step-K bytes (spliced). The test binary is copied to scratch
for these runs: the checkpoint's build identity includes the dirty-tree digest, so a relink by another agent
between the Metal and CPU halves refuses the resume.

**Bisection from Metal's step-5 bytes (certified).** In R0 everything agrees to rounding except one derived
operand: `representedPressureRatio` of the R0 accepted state differs by 1.25e-3 in 16 cells (the eight corner
diagonals and neighbours) while the accepted tuples agree to 1.6e-12 of their scale. In R1 the first operand off
by more than rounding is the **transport temperature**: 300.376068 K on Metal, 300 K exactly on the CPU, in the
eight cells (13,15,0), (13,19,0), (15,13,0), (15,21,0), (19,13,0), (19,21,0), (21,15,0), (21,19,0); from it the
diffusivity (5.3e-4 relative), conductivity (5.7e-4) and molecular viscosity (8.7e-4) differ there, then the R1
energy flux (1.1e-3), the R1 accepted state and the R2 target, which is where §11.2 saw it. From the step-4 bytes
none of this happens (transport temperatures bit-equal).

**The code path.** Both owners invert the accepted state with the same rule (`FireSimulationMethaneRecord::
InvertAcceptedConservativeStateByComponentOrder`, Metal's `eos_temperature`): if E ≤ E(T_lo) + κε₃₂·S the
temperature is T_lo, else bisect, with κ = 1024, ε₃₂ = 2⁻²³ and S = |E| + Σ_s(|ρ_s h_s(T_lo)| + |ρ_s h_s(T_hi)|).
S, and so the snap band, depends on the bracket's upper end. The dump for cell (15,13,0) (E = 445.606 J/m³,
air at ρc_p ≈ 1185 J m⁻³ K⁻¹):

| Bracket | Snap when E ≤ | Temperature | Pressure ratio |
|---|---:|---:|---:|
| record, 300–5000 K (CPU EOS acceptance and R1 base target since M8b) | 845.6 J/m³ (≈ 300.71 K) | **300** | 0.999653 |
| case, 300–2300 K (source build, Metal EOS, spec r74) | 335.7 J/m³ (≈ 300.28 K) | **300.376054** | 1.000906 |

The bisection root (300.376 K) is the solution of the energy row; 300 K is the endpoint snap. M8b (§10.2) had moved
the CPU's EOS acceptance to the record bracket because the R1 base target (shared CPU code) recomputes the
temperature over the record bracket and demands bit equality; that removed a one-ulp refusal but put the CPU's
stage temperatures on a different bracket from Metal's and from the CPU's own source build (which inverts the
beginning state over the case bounds). Cells cross the 0.28–0.71 K band when the gas near the burner first warms;
at tier 4 that is step 6 in these eight cells.

**Which side is right.** Metal. Spec r74: the case-derived `maximum_accepted_temperature_K` (2300 K) "is the upper
endpoint supplied to the canonical accepted-state energy row ... Low-order, corrected, verification, and next-step
beginning states all use that same row". The record bracket is a second row. The fp64 mirror is the same CPU source
(record bracket) but in binary64, where κε₆₄·S is about 1e-9 J/m³, so it resolves these cells (the binary64
inversion of these binary32 tuples refuses them, since the tuples' −1e-11 product residues are outside the binary64
envelope; the binary32 bisection root is bracket-independent to an ulp).

**Fix (CPU backend only; calibration fixtures, the mirrors' defaults and Metal unchanged).**
`FireProductionScalarPhysicalFluxPrerequisiteRequest::canonicalInversionLowerK/UpperK` (zero = the record range, the
default) and `FireProductionScalarEOSAcceptanceRequest::caseBracketInversion` (default false): the CPU owner sets
the EOS acceptance's flag from its physical contract, so the EOS acceptance (Q* and Qⁿ⁺¹) and the R1 base target
invert over the same bracket. `BuildCPUOwnerRequest` sets the case bounds; the CPU backend's published pressure
ratio and deviation (manifold diagnostics) use the case bracket too. Not changed: the owner's qualification-trace
pressure ratio (diagnostic only; it is why `representedPressureRatio` still differs in the bisection) and the r70
tail-drain correction's ratio (it engages only beyond |V−1| = 1/16, and the snap moves V by at most 2.4e-3, so it
cannot change a cell's drain near ambient).

**After the fix**, one step from Metal's step-k bytes (relative to the field scale as in §11.2):

| Step taken | Certified: species / T / E / velocity | Fast: species / T / E / velocity |
|---|---|---|
| 1 → 2 | 1.9e-13 / 1.9e-15 / 6.3e-10 / 3.4e-8 | 9.7e-14 / 3.7e-15 / 1.8e-12 / 6.8e-8 |
| 4 → 5 | 5.1e-8 / 3.6e-16 / 1.0e-9 / 6.8e-8 | 5.1e-8 / 3.8e-14 / 1.0e-9 / 6.8e-8 |
| 5 → 6 | 5.1e-8 / 4.6e-14 / **1.7e-9** / **6.8e-8** | 5.1e-8 / 1.2e-9 / **1.7e-9** / **3.4e-8** |
| 6 → 7 | 2.5e-8 / 1.8e-13 / 1.5e-9 / 3.4e-8 | 5.1e-8 / 1.1e-15 / 1.5e-9 / 6.9e-8 |
| 7 → 8 | 2.5e-8 / 1.2e-13 / 1.3e-9 / 6.9e-8 | 2.5e-8 / 1.2e-13 / 1.3e-9 / 6.9e-8 |
| 8 → 9 | 5.1e-8 / 2.1e-15 / 2.3e-9 / 6.9e-8 | 2.5e-8 / 2.5e-15 / 2.3e-9 / 6.9e-8 |
| 9 → 10 | 5.1e-8 / 1.6e-13 / 3.2e-11 / 6.8e-8 | 5.1e-8 / 3.8e-15 / 2.1e-9 / 6.8e-8 |

(Species 5.1e-8 is one ulp of the N₂ density; the full sweep 1→10 is in `/Volumes/SSD/claude-tmp/m8d-scratch/onestep.txt`.)
The trajectory comparison from zero (default fast mode, 10 steps) is unchanged in kind: step 1–2 rounding, the
rim-ignition event at step 3 (species 7.6e-5, T 1.0e-4, E 3.9e-3, velocity 3.6e-3), then a monotone decay to
species 3.9e-6, T 5.6e-6, E 6.1e-5, velocity 2.7e-5 at step 10.

**The agreement test, tightened.** `FireCPUMetalAgreementTest` now runs 8 steps. Step 1 to rounding (as before);
the event step (the first above rounding, 2 or 3 by the residue's signs) and the next to the event bounds (as
before); **from the second step after the event**, which §12.1 now explains (both backends take each step to
rounding, so what remains is the event's remainder carried by the dynamics), the difference must not grow by more
than 5 % per step and must stay within species 2e-5, temperature 3e-5, energy 7e-4, velocity 2e-4 of the field
scale (measured there 8.3e-6, 1.1e-5, 2.6e-4, 8.0e-5); and one step from Metal's step-2 **and step-5** bytes to
rounding (step 5 → 6 is the step the bracket moved). 31 s in the fast tier (20 before); `tests/test_schedule.tsv`
updated.

### 12.2 The pre-pilot rim-ignition witness: the fix for the re-baseline (approved; to be applied by another agent)

**Measured effect of the fix** (test-only switch, below; default unchanged).

- *Agreement.* With the witness at 1e-6 on both backends (`RISE_M8D_RIM_WITNESS=1e-6 FireCPUBackendTest
  --agreement 10`, default fast mode, tier 4) the trajectories from zero agree to accumulated binary32 rounding at
  every step: maxima over steps 1–10 species 1.5e-7, temperature 8.0e-8, energy 1.3e-6, velocity 1.0e-5 of the field
  scale (steps 1–6: energy ≤ 2.2e-7), against the event's 7.6e-5, 1.0e-4, 3.9e-3, 3.6e-3 at step 3 by default. One
  step from Metal's step-2 bytes: species 2.5e-8 (one O₂ ulp), T 5.5e-15, E 1.6e-12, velocity 6.8e-8.
- *The fire's start* (Metal fast mode, tier 4, 600 steps = 1.0 s, default and switch; statistics every 5 steps,
  `/Volumes/SSD/claude-tmp/m8d-scratch/rim/`): **by default the burner is ignited by rounding residue**: CO₂+H₂O in the
  domain is already 6.9e-8 kg at step 5 and 7.3e-6 kg at step 30 (0.05 s), so the early flame grows from the rim
  cells whose residue happened to be positive. With the witness, the products stay at the residue level
  (4.4e-14 kg, nothing reacts) until step 75 (0.127 s); the first burning appears between steps 75 and 80
  (3.7e-6 kg at 0.135 s) once the pilot has heated the annulus. The flames then converge: products −6.7 % at 0.25 s,
  +1.3 % at 0.50 s, −0.5 % at 1.0 s; maximum temperature 1,281 K with the witness against 1,260 K by default at 0.25 s and 1,953 against 1,962 K
  at 1.0 s; the frame heat release 30.3 against 29.8 kW at 1.0 s. So the fix delays ignition by about 0.12 s (73
  steps) and leaves the developed fire's statistics to within the run-to-run scatter of a perturbed start (§11.5:
  0.06 K RMS growing with an e-folding time of 0.43 s); the statistics window (from 5 flow-through times) is
  unaffected in distribution, not in bytes.

**Threshold: a product mass fraction (CO₂+H₂O)/ρ of 1e-6.**

- *Above binary32 noise:* after step 1, with no reaction anywhere, the product densities are signed residue
  |ρ_CO₂+ρ_H₂O| ≤ 2.7e-11 kg/m³ (fraction 2.3e-11 at ρ = 1.17 kg/m³), positive in 40 % of cells (§11.1). The finest
  physically meaningful resolution of a species in the binary32 state is set by the gas density's own rounding: one
  ulp of 1.17 kg/m³ is 1.2e-7 kg/m³ (fraction 1.0e-7), and every transport step rounds the major species (O₂, N₂,
  0.27 and 0.90 kg/m³) at 3–6e-8 kg/m³, which is what feeds the residue. 1e-6 is about ten gas-density ulps and
  4×10⁴ times the measured residue, so no rounding residue reaches it in the steps before the pilot is established.
- *Far below any real flame product:* fully burnt stoichiometric methane–air has (CO₂+H₂O)/ρ = 80/290.6 = 0.275, so the
  witness is one part in 2.8×10⁵ of burnt gas; a cell one step downstream of a burning cell carries 1e-4–1e-2. The
  established-flame unit fixture (`FireSequenceTest`, 2 % reacted) is about 1e-3 and still passes, as does every
  fire fast-tier test (below).
- The earlier intervention value 1e-9 (§11.1) is also above the residue but below one gas-density ulp, so residue
  growth over many steps could reach it; 1e-6 is not marginal in either direction.

**Test-only switch (implemented; default bytes unchanged).** `FireCase::RimProductWitnessOverride()` reads
`RISE_FIRE_RIM_PRODUCT_WITNESS` once per process (0 when unset, empty or invalid); both sites use
`max(override, nextafter(ambient, +∞))` only when the override is positive, and the historical expression otherwise.
Only the fire solver's parameter `rim_product_witness` (a fraction in (0, 1); not in the scene schema) writes the
variable, and `FireSolver::Configure` writes it empty when the parameter is absent, so no legacy environment can
change a run unkeyed; when given it enters the effective parameters and so the key. Default Metal bytes and the
default key were checked equal after the change (7 certified steps, `state_1/4/7` byte-identical; key
`ee7822cf…a1c7` unchanged).

#### 12.2.4 Checklist for the re-baseline agent

Run from the repository root; nothing else may run the fire solver meanwhile.

1. **The shared source build.** `src/Library/Utilities/FireProductionSource.cpp`, inside
   `FireProductionCanonicalSourceAuthority::Build`, the block after `if(!request.sourceBoundaryContactMask.empty()){`
   (currently lines 322–326): replace the three statements from `// M8d test-only switch` through the second
   `std::nextafter(...)` with

   ```cpp
   // Re-baseline (M8 results 12.2): a product fraction above binary32 residue, not the ambient zero.
   const double productWitness=std::max(FireCase::RimIgnitionProductWitness,
       std::nextafter(ambientProductMassFraction,std::numeric_limits<double>::infinity()));
   ```
2. **The driver.** `src/Library/Simulation/Fire/FireRunDriver.cpp`, `ProductionEstablishedFlameHolderEligible`
   (currently lines 1124–1128): the same replacement.
3. **The constant and the switch.** In `src/Library/Utilities/FireCase.h` add
   `constexpr double RimIgnitionProductWitness=1.0e-6;` beside `RimProductWitnessOverride`, then remove the test
   switch: `RimProductWitnessOverride` (`FireCase.h/.cpp`), the `rim_product_witness` parameter, `known` entry,
   `rimWitnessEnvironment` and its `environment` line in `src/Library/Simulation/Fire/FireSolver.cpp`, and the
   `RISE_M8D_RIM_WITNESS` line in `tests/FireCPUBackendTest.cpp` `Parameters`. (Keeping it as an override is also
   possible; removing it keeps one rule.)
4. **The solver version.** The fix changes fire bytes under unchanged parameters, so bump
   `FireMethaneSolverVersion` in `src/Library/Utilities/FireSequence.h` from `"2"` to `"3"` (every fire key changes;
   existing caches read as stale and rebake). In `src/Library/Utilities/FireSequenceCache.cpp`
   `AttachSolverCache`, accept `"2"` as well as `"1"` and the current version, so caches baked before the
   re-baseline stay playable (same grid and receipt formats).
5. **Mirrors.** `python3 tools/generate_fire_production_fp64_mirror.py && python3
   tools/generate_fire_production_roundoff_trace.py`, then both with `--check` (the build runs the checks).
6. **Build** (about 5 min with other agents idle): `make -C build/make/rise -j10 all tests build-tool/rise_sim
   build-tool/simulation_cache_import`; must be warning-free.
7. **Regenerate the pins that change, and only those.** Measured by building the fix default-on (a temporary
   probe, removed) and running the whole fire fast tier: **all 14 fire tests passed unchanged** (FireSolverTest,
   FireSequenceTest with its 88 pinned digests, FireProductionSolverTest, FireProductionCalibrationTest,
   FireCPUBackendTest, FireCPUMetalAgreementTest, ...), and the Python record tests run by `run_all_tests.sh`
   passed. No committed fire frame, cache or digest therefore contains the pre-pilot rim decision. What changes:
   - the solver-version bump changes the two fire keys pinned in `tests/SimulationSceneTest.cpp` J
     (`pinnedBase`, `pinnedOptions`): run `bin/tests/SimulationSceneTest`, read the `fire keys: base … options …`
     line, paste the two values, and change the comment to say they were re-pinned at the version bump (the test's
     other checks — `backend metal` keys like the default, `backend cpu` differs — must still pass);
   - `FireCPUMetalAgreementTest` (`tests/FireCPUBackendTest.cpp` `Agreement`): with no event the trajectory is held
     to rounding throughout; replace the event logic by one bound per step, species 1e-6, temperature 3e-7, energy
     3e-6, velocity 2e-5 of the field scale (measured maxima over 10 steps 1.5e-7, 8.0e-8, 1.3e-6, 1.0e-5; the
     rounding grows slowly with the step), keep the two identical-start checks, and update the comment block above
     `kAgreementSteps`;
   - not affected, but rerun: `FireSolverTest --extended` (self-consistency bakes; no pinned bytes), and the GUI probe
     expectations in `docs/SIMULATION_GUI_STATUS.md` (the campfire row's key prefix if it is quoted).
   Historical evidence under `docs/data/fire_r2*` is per-revision record, not a baseline; do not regenerate it.
8. **Tests:** `./run_all_tests.sh --tier fast --timeout 300 --filter 'Fire*'` (about 6.5 min: FireSolverTest 205 s,
   FireSequenceTest 121 s, the two CPU tests 80 s) and `./run_all_tests.sh --tier fast --filter
   'SimulationSceneTest,SceneEditorSuggestionsTest,CstDeriveGoldenTest'`; then `RISE_TEST_EXTENDED=1
   bin/tests/FireSolverTest --extended` once.
9. **Confirm agreement through the ignition steps:** `bin/tests/FireCPUBackendTest --agreement 10` and
   `bin/tests/FireCPUBackendTest --agreement-certified 10`: every `AGREEMENT` line within the step-7 bounds above
   (no step near 1e-4 in temperature), and both `IDENTICAL_START` lines at rounding (species ≤ 1e-6, T ≤ 1e-7,
   E ≤ 1e-6, velocity ≤ 1e-5). For the start of the fire: a Metal `--long-run-child 4 0 1 3.2 DIR 600 5 0` should
   show the domain's `products_kg` at residue level (~1e-13 kg) until about step 75 and burning from about step 80
   (0.13 s), as in the table above; record that time in the re-baseline note.
10. Update this document (a dated section: the threshold, the version, the re-pinned keys, the agreement curve) and
    the contract's Appendix C (version bump: every fire key changes; version-2 caches stay playable).

### 12.3 Certified hashing: why it cost 70 %, and what changed

**Profile** (`sample`, 10 s of a certified tier-4 CPU run, 8 workers, all threads): 16,413 of 24,931 busy samples
(66 %) were in the two passes of the sliced FNV (`FireProductionFNV1aWords`: pass 1, the 256-lane low-byte
permutation, 10,070; pass 2, the sums, 6,320). By caller (the main thread's samples while it waits for the
workers): `HeunFluxStageIdentity` 56 % of the hash time, of which a quarter of all hash time was the **same stage
re-hashed** by `ValidScalarHeunFluxStage` in each consumer (`AverageFireProductionScalarHeunFluxStagesCPU` for both
of its inputs, the compatible Heun momentum, the Heun solve), the rest its first evaluation in
`ComposeFireProductionScalarHeunFluxStageCPU` and the average; then the transport publication identity, the stage
input and FCT contract identities, the candidate identity and the R2 endpoint base. 78 % of the hashed bytes went
through `HashEOSFloatArray`, which hashes each binary32 value as its binary64 widening (8 bytes). So the cost is
both bandwidth (each flux stage is about 24 MB at tier 4, hashed as 48 MB) and re-hashing of unchanged objects.
Pass 1 costs 32 vector operations per input byte (16 lanes of 16 starting values), which a cheaper formulation
cannot avoid (the low-byte map is a bijection, so no starting values merge).

**Changes (digests identical by construction and by test):**

1. *Known-zero bytes* (`FireProductionFNV1aWords(..., leadingZeroBytes)`, mirrored): a binary32 value widened to
   binary64 has its low 29 mantissa bits zero, so its first three bytes are zero; three zero bytes are one
   multiplication by P³ (P³ mod 256 in pass 1), and the word is hashed as five bytes. `HashEOSFloatArray` checks the
   low 24 bits of every word and takes the eight-byte form otherwise (the binary64 mirror hashes wider values through
   the same source). Pass 1 drops from 256 to 176 vector operations per value. `FireCPUBackendTest` checks the
   sliced hash against the serial byte loop for seven layouts (two with leading zeros) at 1, 3 and 8 workers.
2. *Exact stage-identity memo* (`HeunFluxStageIdentity`, certified mode, only on the CPU backend's workers): an
   `ExactContentHashMemoSet<3>` (least recently used) keyed on the hash state before the payload and **every
   payload byte** (memcmp against a retained copy), so a hit returns exactly the value a recomputation gives and any
   changed byte recomputes (the forged-stage refusals of the calibration tests still fire: they run with one worker,
   where the memo is not used, and a forged copy differs in content anyway). Memory: three stage payloads (about
   72 MB at tier 4, 560 MB at tier 8), added to `FireCPUBackendWorkingSetBytes`; peak RSS at tier 4 rose by 45 MB.

Not changed: the per-stage digests themselves (a protocol change), and the order of any hash.

**After** (same profile): FNV 5,869 of 14,205 busy samples (41 %). Whole-process user CPU for five certified
tier-4 steps under the same load: 54.8 s with change 1 only, 41.3 s with both.

**Seconds per accepted step** (M1 Max, 8 workers; other agents' builds and tests running, load average 11–35):

| Case | §11.6 | Now | Steps |
|---|---:|---:|---|
| tier 4, certified | 2.16 | **1.63–1.70** | 5 |
| tier 4, fast | 1.6 | **1.47–1.53** | 5 |
| tier 8, fast | 11.6 | **10.95–12.5** (first step 12.5) | 3 |
| tier 8, certified | 16.0 | **14.7, 14.9** (one step 27.6 while the load average rose to 28) | 3 |

Certified is now 1.1× fast at tier 4 (1.35× before) and about 1.35× at tier 8. The quiet-machine numbers were not
obtainable: other agents ran throughout.

### 12.4 Scene selection of the CPU backend (owner-approved §11.7; done)

`FireParameterSchema` gains `backend` (`Enum {metal, cpu}`, default `metal`), so `fire_simulation` has the field and
the GUI, `sim_param_set` and agents can set it. The §11.7 design is implemented locally in fire's schema rather than
as a generic `omitDefault` flag (no shared schema struct changes): fire's `Complete` never inserts `backend` and
drops an authored `backend metal`, so the completed map, the predicted effective map and `FireSolver::Configure`'s
effective map (which already omits `metal`) agree for all three spellings; `DescribeUnkeyed` passes the backend to
the memory estimate. No host override exists outside the key (the solver writes `RISE_FIRE_BACKEND` from the
parameter, as before). The chunk gains one field; **chunk keywords stay 191** (`SceneEditorSuggestionsTest`
unchanged), `tests/data/cst_derive_golden.txt` unchanged (`CstDeriveGoldenTest`: 148 match, 0 drift).

Tests:

- `SimulationSceneTest` J: the two fire scene keys of the existing test (the bare windowed scene and the one with
  five options) are **pinned as recorded with the build before the change** (`f005c0ac…df27`, `5b419d96…712f`) and
  still match; `backend metal` keys like `backend` left out; `backend cpu` has its own key; `backend gpu` is refused
  at load; completion states every schema parameter but `backend`.
- `FireCPUBackendTest` (CPU part): a scene with `backend cpu` (tier 6, fast, one step) baked by `rise-sim bake
  <scene> campfire` finishes, is `complete` under its predicted key, and its launch receipt names the CPU step
  (`cpu-m8-v1`); the same completed parameters run through `InProcessHost` (Configure, Start, step to the end) with
  `capabilities.backend` `cpu` and a cache key equal to `MakeCacheKey` of the completed map and to the key
  `rise-sim status` shows. The test takes 49 s (18 before; `tests/test_schedule.tsv` updated).

Docs: Appendix C row (contract), `src/Library/Parsers/README.md` example, `docs/SIMULATION_GUI_STATUS.md` (the
backend is now a scene parameter; the predicted row's backend detail stays blank before a bake, since a predicted
fire states no `capabilities.backend`).

### 12.5 Long run

**Run.** Tier 4, fast mode, both backends from zero with this section's code (`FireCPUBackendTest --long-run-child 4
{0,1} 1 8.5 DIR 4860 5 100`; scratch `/Volumes/SSD/claude-tmp/m8d-scratch/long/`; the §11.5 checkpoints could not be
resumed: their build identity differs, and the CPU arithmetic has changed since by §12.1). Metal: 4,860 steps (8.0 s)
in 1,474 s. CPU: stopped by the 2.5-hour cap at **4,205 steps (7.04 s)** after 9,000 s (2.1 s per step on average,
1.5–3.1 with the other agents' load). Metal's statistics through 1,495 steps are identical to the §11.5 Metal run
(every printed statistic), as they must be with Metal's bytes unchanged.

**Divergence** (field-scale maxima as §10.3; RMS absolute):

| Step | t (s) | T RMS (K) | velocity RMS (m/s) | species max | E max |
|---:|---:|---:|---:|---:|---:|
| 100 | 0.16 | 3.8e-5 | 3.7e-7 | 5.7e-6 | 1.2e-5 |
| 500 | 0.82 | 6.7e-4 | 2.0e-6 | 3.0e-5 | 3.8e-5 |
| 1000 | 1.65 | 3.7e-3 | 6.2e-5 | 1.4e-4 | 1.6e-4 |
| 1500 | 2.47 | 0.023 | 7.2e-4 | 1.8e-3 | 2.0e-3 |
| 2000 | 3.29 | 0.10 | 3.6e-3 | 5.2e-3 | 6.7e-3 |
| 2500 | 4.12 | 0.59 | 0.022 | 0.033 | 0.043 |
| 3000 | 4.94 | 2.9 | 0.11 | 0.13 | 0.18 |
| 3300 | 5.43 | 6.6 | 0.16 | 0.30 | 0.43 |
| 3600 | 5.93 | 10.6 | 0.19 | 0.33 | 0.44 |
| 4200 | 6.91 | 13.9 | 0.22 | 0.36 | 0.53 |

The growth is exponential from about 0.4 s with an e-folding time of about 0.45 s (the T RMS doubles every 200 steps,
0.33 s), as §11.5 measured, and saturates from about 5.9 s. The decorrelated level, measured as the RMS difference
between two Metal states 300–1,500 steps apart late in the run, is 14–15 K and 0.22–0.24 m/s: **by 6 s the two
trajectories are decorrelated locally** (the extrapolation in §11.5 said about 6 s). Domain integrals are slower:
their CPU–Metal time-series correlation over 5.9–7.04 s is still +0.4 to +0.8 (products, domain mean temperature,
kinetic-energy measure).

**Statistics over 5.9–7.04 s** (136 samples each, every 5 steps; standard deviation in parentheses), with Metal's
own next window (7.05–8.0 s) as the measure of what one window of this length scatters by:

| Quantity | Metal 5.9–7.04 | CPU 5.9–7.04 | (CPU − Metal) / Metal | Metal 7.05–8.0 vs 5.9–7.04 | corr. |
|---|---:|---:|---:|---:|---:|
| Heat release (frame field, W) | 32,177 (374) | 32,242 (440) | +0.2 % | −0.3 % | +0.22 |
| CO₂+H₂O in the domain (kg) | 6.454e-3 (3.4e-5) | 6.406e-3 (4.5e-5) | −0.7 % | +2.0 % | +0.40 |
| Domain mean temperature (K) | 304.059 (0.031) | 304.062 (0.039) | +1e-5 | +6e-5 | +0.82 |
| Maximum temperature (K) | 1,510 (49) | 1,541 (27) | +2.0 % | −2.6 % | +0.43 |
| Volume above 500 K (m³) | 0.0341 (0.0016) | 0.0336 (0.0016) | −1.5 % | −2.6 % | −0.03 |
| Kinetic-energy measure | 2.045 (0.018) | 2.057 (0.025) | +0.6 % | +3.1 % | +0.71 |
| Centreline T, layer 0 / 8 / 16 / 32 / 48 (K) | 1198 / 636 / 445 / 371 / 345 | 1192 / 698 / 476 / 393 / 351 | −0.5 / +9.7 / +6.9 / +6.0 / +1.7 % | −9.8 / −2.4 / +1.1 / +0.3 / −1.3 % | +0.67 / +0.33 / −0.01 / −0.40 / +0.44 |
| Centreline T standard deviation, layers 0 / 8 / 16 (K) | 83 / 117 / 48 | 94 / 79 / 49 | | 101 / 91 / 32 | |
| Centreline w, faces 4 / 16 / 32 / 48 (m/s) | 2.41 / 2.54 / 2.46 / 2.35 | 2.60 / 2.97 / 3.03 / 2.43 | +7.9 / +17 / +23 / +3.2 % | +2.1 / +7.6 / +4.6 / −0.3 % | +0.23 / +0.05 / −0.22 / +0.60 |

(Heat release is the frame's per-cell field summed; frames are 1/16 s apart, so the window holds 18 distinct values.)
Reading it: the integral quantities (heat release, products, hot volume, mean and maximum temperature, kinetic
energy) agree to 0.2–2 %, within what Metal's own adjacent window moves by, but they are not yet independent
samples (correlation up to +0.8). The centreline quantities are decorrelated (correlation near zero) and differ
by up to 10 % in temperature and 23 % in vertical velocity at mid-height, two to five times Metal's window-to-window
change; their standard deviations agree to 10–30 %. One 1.1 s window of a flow puffing at about 1.3 Hz (Metal's
5–8 s centreline spectrum peaks at 1.33 Hz, resolution 0.33 Hz; the CPU window's resolution, 0.87 Hz, gives no
usable estimate) holds about one and a half puffs, so a difference of this size in the centreline means is within
sampling error of so short a window, and **this run does not establish statistical agreement or disagreement of
the decorrelated centreline**; it establishes that the integral statistics agree to about 2 % and that
decorrelation happens at about 6 s.

**Extrapolation.** A decorrelated window long enough for centreline means and the puffing frequency to 5 % (about
10 s, 13 puffs; 6–16 s) needs about 9,700 CPU steps from zero, about 5.7 hours at this run's 2.1 s per step (4 hours at the
1.5 s per step measured in §12.3), plus about 50 minutes of Metal; not run.

### 12.6 What changed

| File | Change |
|---|---|
| `src/Library/Simulation/Fire/FireCPUBackend.cpp` | The case bracket for the CPU owner's EOS acceptance and R1 base target, and for the published pressure ratio (§12.1); the `RISE_FIRE_CPU_BISECT` diagnostic (prints only); the stage-memo bytes in `FireCPUBackendWorkingSetBytes` |
| `src/Library/Utilities/FireProductionTransport.{h,cpp}` (mirrored) | `canonicalInversionLowerK/UpperK` (physical request) and `caseBracketInversion` (EOS request), both defaulting to the record range; `HashEOSFloatArray`'s known-zero-byte form; `ExactContentHashMemoSet` and the stage-identity memo (§12.3) |
| `src/Library/Utilities/FireProductionForce.cpp` (mirrored) | The owner passes the bracket to both EOS acceptances |
| `src/Library/Utilities/FireProductionAdvection.{h,cpp}` (mirrored) | `FireProductionFNV1aWords` takes leading zero bytes |
| `src/Library/Utilities/FireCase.{h,cpp}`, `FireProductionSource.cpp`, `src/Library/Simulation/Fire/FireRunDriver.cpp` | The test-only rim witness switch (§12.2; default: the historical expression) |
| `src/Library/Simulation/Fire/FireSolver.cpp` | Parameter `rim_product_witness` (test only, keyed when given) |
| `src/Library/Simulation/Fire/FireParameterSchema.cpp` | Scene parameter `backend`, keyed only as `cpu` (§12.4) |
| `tests/fire_production_fp64/*`, `tests/fire_production_trace/*` | Regenerated; both `--check`s pass |
| `tests/FireCPUBackendTest.cpp` (and `FireCPUMetalAgreementTest`) | Tightened agreement (8 steps, post-event bounds, identical start at 2 and 5); sliced-hash layouts with leading zeros; scene selection through `rise-sim` and `InProcessHost`; `RISE_M8D_RIM_WITNESS` diagnostic |
| `tests/SimulationSceneTest.cpp` | Pinned fire keys, `backend` keys and refusal |
| `tests/test_schedule.tsv` | `FireCPUBackendTest` 50 s, `FireCPUMetalAgreementTest` 30 s |
| `docs/SIMULATION_SOLVER_CONTRACT.md`, `src/Library/Parsers/README.md`, `docs/SIMULATION_GUI_STATUS.md` | §12.4 |

Metal source files are untouched; no codec, manifest, checkpoint or solver version changed; every existing fire key
is unchanged (pinned). Chunk keywords 191 and `RewireConnectionTest` unchanged.

### 12.7 Verified versus inferred

| Claim | Basis |
|---|---|
| The step-6 difference is the CPU's record-range inversion bracket | Operand bisection on the real step (first operand off: R1 transport temperature in the eight cells); the dump of both brackets' inversion for those tuples; intervention: with the case bracket the step agrees to rounding |
| Metal is the right side | Spec r74 (one energy row, the case ceiling); the source build already uses it; the root of the energy row is 300.376 K |
| Steps 1→10 from identical bytes agree to rounding after the fix | Run, certified and fast (§12.1) |
| The rim fix gives rounding agreement through ignition and delays ignition to 0.13 s | Run: 10-step agreement with the switch on both backends; two 600-step Metal runs |
| No committed fire test or baseline pins the rim decision | The whole fire fast tier run with the fix default-on (temporary probe, removed): 14 of 14 passed. `FireSolverTest --extended` was not run with it (self-consistency only, read) |
| Hash digests unchanged | By construction; the sliced-hash test (7 layouts, 1/3/8 workers); every calibration and qualification digest test passes |
| Hash cost | Two `sample` profiles and timed runs on a loaded machine (load average 11–35) |
| Existing fire keys unchanged | Pinned in `SimulationSceneTest` from the build before the change; Metal default key and bytes compared |
| Metal output bytes unchanged | No Metal source changed; 7 certified Metal steps byte-identical before and after; the fire fast tier passes (14 of 14 on the first full run; on the final run `FireSolverTest` failed once because another agent relinked the test binary between its uninterrupted and resumed bakes, so the two manifests carry different producer build ids while every frame is byte-identical; rerun alone it passed) |
| Builds warning-free | `all tests build-tool/rise_sim build-tool/simulation_cache_import`, no warnings; mirrors' `--check` pass |

### 12.8 Open items (supersede §11.10)

1. The re-baseline (§12.2.4), by another agent.
2. A decorrelated statistics window of about 10 s (6–16 s; §12.5's extrapolation) to compare the centreline means
   and the puffing frequency of independent CPU and Metal trajectories.
3. Fast mode's early V-cycle stop could decide differently on the two backends at a near tie of the validation band
   (§11.4); none seen.
4. A certified mode substantially cheaper than fast needs fewer content identities (a protocol change); the memo
   costs three flux-stage copies of memory.
5. The Windows driver thread; Windows and Android remain uncompiled (§4).
6. The Metal-only opt-ins on the CPU backend (§10.7 item 6).
