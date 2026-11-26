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
- **Part A (fire on the CPU): an honest partial. The CPU backend stays `unsupported`.** A complete CPU
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
| `fire.methane` | `cpu` | **unsupported** (unchanged) | §5: the step exists only as a one-step calibration owner the driver cannot call; no solver-level agreement run. Detail text now says so. |
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

1. Fire CPU backend: transport provider and request/result adapters in the library, a driver backend switch with
   CPU-only memory admission, a thread pool with fixed-order reductions, a working-set limit sized to the machine,
   an audit that every default-on Metal change since r239 is mirrored, then the solver-level Metal-fast vs CPU run over
   several steps and the fast-tier determinism/resume test the M8 brief describes.
2. `rise-sim` targets for VS2022 and Android, and a first Windows run of the process host (§4.2).
3. Windows ARM64 configurations.
4. A first Windows and Android run of `SimulationGoldenCacheTest`.
5. A skip, not a failure, for the four Metal-only fire test programs in a build without Metal.
