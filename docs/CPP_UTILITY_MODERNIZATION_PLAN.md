# C++ utility modernization plan

**Status:** Host implementation verified on `codex/cpp-utility-modernization`. Native Windows and Android verification remain outstanding. Audit baseline `5fc6acaef`.

## Implementation checkpoint

- The local series covers spectral assignment and lookup, scene and asset records, scripts and descriptor ownership, buffer and codec failures, file output paths, Halton storage, and unused utilities. The later commits move manager and options lookups to owned keys with borrowed probes, use a monotonic clock for elapsed timing, normalize media roots, fix intrusive reference locking, and scope procedural scratch and hair file ownership.
- Halton sample and migration outputs matched byte for byte. The TIFF reader correction changes one scene digest, `cc_tiff_painter.RISEscene`. Its golden entry was refreshed after independent TIFF decoding checks. The legacy expression evaluator moved to the migration tool. Valid comparison fixtures produce identical output, malformed expressions remain unexpanded, and nesting is bounded.
- An arm64 probe rebuilt the old scalar writer branches with `RISE_BIG_ENDIAN` and confirmed lost high bits and reversed double halves in both disk and memory output. Files from those branches can contain irreversible loss. The new disk and memory writers matched independent little-endian scalar fixtures. Independent PNG scanline inspection confirmed network-order 16-bit RGBA samples.
- The factory and interface signatures used by supported clients remain the compatibility boundary. Concrete implementation headers including `MultiHalton`, `GenericManager`, `AsciiCommandParser`, `MemoryBuffer`, the disk buffers, `EXRReader`, `EXRWriter`, `RasterImage_Template`, mesh cache and stream-printer types, and file rasterizer output have changed. Clients that construct or subclass these types directly need to rebuild. Existing `String` types at public interfaces and in public configurations retain their layouts.
- CLI token ownership now uses `std::string`, with compatibility adapters at the existing `String` entry points. Descriptor parameter names and material-slot comparisons use borrowed views. Checked descriptor, console, options, and camera-keyframe conversion replaces unchecked numeric reads. Invalid commands are rejected before dispatch. Camera keyframes require complete finite scalar/vector input and reject trailing junk. Interpolated dimensions and aperture counts are range-checked before integer conversion.
- Raster images explicitly own shared backing storage, retain borrowed source owners across nested views, and preserve clipped/strided copies and reload behavior. Disk streams use scoped ownership. Memory buffers use scoped array storage with separate capacity and exact logical size. Growth preserves borrowed/adopted ownership, aliasing, cursor behavior, and failed-write state. HDR/RGBEA headers and RAW/RAW2 importer filenames now use owned strings.
- `stl_utils.h` was removed after pairing the factory inputs explicitly and selecting standard vector operations at each mesh/patch call site. Mesh compaction uses `shrink_to_fit()`. Sites requiring release use empty-vector swaps. Active elapsed-time callers use `steady_clock`, with bounded conversions at integer interfaces. The historical timer and seed APIs remain available for compatibility and frozen callers.
- Private-lock pilots use scoped `std::mutex` in the mesh signal cache and stream printer. Intrusive lifetime semantics continue through `IReference`. The atomic-count prototype and standard RNG prototype were measured separately, as recorded below.
- Native MSVC Debug/Release verification is still required before retiring the Windows keyframe workaround. Windows and Android verification are outside this continuation's requested host scope. Public string replacement and an atomic `Reference` layout change remain conditional on compatibility and workload evidence.


### Compatibility review follow-up

The review identified regressions in accepted inputs and file output. Focused
follow-up commits restore compatible inputs and retain diagnosed errors for
undefined or corrupt data. User-visible changes and binary regeneration
requirements are recorded in [RELEASE_NOTES.md](RELEASE_NOTES.md).

| Finding | Disposition |
| --- | --- |
| 1. 3DS metadata and empty meshes | Accept older/missing version chunks, skip empty meshes, and preserve discovered meshes when later metadata scanning fails, including within truncated `EDIT3DS` containers. Consumed mesh bounds and indices remain checked. The old version gate inspected a fixed-offset byte at 28, normally the low byte of the mesh-version payload, without locating its chunk. |
| 2. Outputs beyond 4 GiB | Disk cursors, sizes, seeks and photon availability checks use 64 bits through an optional `IBuffer64` interface. Existing `IBuffer` virtual signatures remain intact and narrow disk queries saturate. On sparse-capable scratch filesystems, guarded fixtures cover writes, packet reads across the boundary, normal Job loads, and codec offsets. Photon saves require staging. |
| 3. Spectral and piecewise tables | Both consume leading finite columns and tolerate extra columns. File headers/malformed rows are skipped with warnings. Inline rows need two values. NUL, empty results, I/O failure and unequal spectral counts fail. |
| 4. Console input | Commands support standalone trailing `#` comments and leading full-line comments, including `render # comment`; raw tokenizers and options preserve hashes. Malformed numeric arguments, incomplete render rectangles and unknown media-path commands remain diagnosed hard errors. |
| 5. Restricted staging parents | Render output, viewport Save As and thumbnails explicitly permit direct writes on staging creation/open or rename permission failures or through dangling symlinks. Other staging errors fail. Rename fallback copies completed bytes without re-encoding. Failed direct image writes may leave partial output. Photon saves retain staging-required failure safety. |
| 6. Damaged TIFF strips | A compressed-strip fixture confirms partial decoding reports success with an opaque-black row. Strict rejection is retained and tested. |
| 7. Windows path encoding | Existence checks use the native narrow path constructor shared with CRT/stream opens. Native Windows non-ASCII verification is still required. |
| 8. Memory tracking | The scoped disk-writer construction shared by staged saves and image fallback registers its allocation with `PrintNew`. |
| 9. Logger static lifetime | Filename storage survives static destruction. An exit-time recreation fixture covers the confirmed ASan use-after-free. |
| 10. Historical Xcode CLI binary assets | Regenerate affected photon maps, caches, serialized meshes and other outputs using the scalar serializers from source. The plan and release notes record the incompatible endian branches and irreversible bit loss. |
| 11. Checked descriptor numbers | The parser guide documents range errors, non-numeric fields, `.5` in unsigned fields, signs and retained prefix conversion. |
| 12. Script/result corrections | Release notes record media-path registration, cylindrical UV size arguments and the default Voronoi border correction. |

The first follow-up review round found an EOF termination violation after a
file was truncated while open. The cached readable end is now reduced when
I/O observes the earlier EOF, including preflight failures and line/scalar
reads. Missing-file block reads zero the destination. Regression fixtures
cover each scalar width, line and block reads. The media fixture now creates
and reopens native narrow names, including non-ASCII bytes, through the same
convention as the locator.

Codec fault injection covers early and final writes. Frame encoders retain
scoped writer ownership, TIFF metadata/scanline/directory failures propagate,
and EXR finalization checks a latched stream error after OpenEXR closes its
output. These failures preserve staged destinations and release references.

At checkpoint `3c37cb0f7`, follow-up verification passed 13 focused executables, including 480 exact
corpus matches, all four photon families, 119 buffer checks, and 131 encoder
checks. Clean make, Xcode Deployment and Opto builds, and the Blender native
LTO link passed without compiler warnings. Xcode's expected AppIntents
metadata notice was discounted. Targeted ASan/UBSan checks passed for the
buffer, photon, image, parser, spectral and frame-encoder paths. The
`LOG_TRACK_MEMORY` writer probe balanced three allocations and three frees
with zero reported leaks. Concurrent buffer-test runs passed after isolating
temporary paths by process.

The first adversarial source round found the EOF termination issue and the
missing-file zero-fill mismatch. Both were fixed and covered by regressions.
The second fresh source round reported zero P1 findings and one fixture
isolation P2, which was fixed. Native Windows execution of the non-ASCII
path case remains outstanding because this workspace provides macOS and
has no Windows runtime or MSVC toolchain.


### Second-pass compatibility follow-up

The user's second pass found an incomplete 3DS metadata-tail fix and image
callers that still required staging. The current follow-up makes these
additional decisions explicit:

- Retain discovered 3DS meshes across malformed `EDIT3DS` metadata tails;
  corrupt consumed geometry still fails. JPEG's decoder limit is `INT_MAX`
  bytes and this 3DS loader's limit is `UINT_MAX`; larger inputs receive an
  explicit error before payload reads instead of using saturated sizes.
- All staged image publication callers opt into permission-only direct fallback.
  A denied final rename copies already encoded bytes once. Photon saves
  retain required staging and preserve old files on publication failure.
- Image conversion and viewport `SaveTo` catch codec failures and release
  temporary references. Resize conversion explicitly finalizes its writer.
  The public write-only image also releases failed writers safely; callers
  can finalize with `DumpImage(nullptr)` to receive an exception, while its
  destructor logs failures.
- Raw tokenizers and options retain hashes. Command parsing alone strips
  full-line comments or a standalone trailing `#`. Missing and comma-form
  inline control points remain errors: the old unchecked parser supplied
  zero amplitude. Cylindrical UV axes remain lowercase `x`, `y`, or `z`.
- Table errors identify missing files, read failures, or embedded NULs with
  filename and physical line, including spectral `file/nmfile/ampfile`.
- Large-file test sections first probe sparse allocation with a 1 MiB hole
  on the fixture filesystem, and skip those sections if unsupported.
  On POSIX, a parent owns each buffer/photon test's scratch directory and cleans it
  after child exit or crash. Killing both processes can still leave scratch
  files, as with other externally interrupted tests.


At `f5f5488e1`, second-pass host validation passed 15 focused executables: 135 buffer checks,
93 photon checks, 172 CST/command checks, 131 frame-encoder checks, 89 viewport
checks, 438 template checks, and 480 exact corpus matches with zero drift,
plus the image, output, source-hygiene, spectral and mesh suites. The large
file sections explicitly skipped because the scratch filesystem's 1 MiB
probe allocated the entire gap. Seven targeted ASan/UBSan executables passed.
The new 3DS, rename-fallback, hash, diagnostic, and writer-lifetime regressions
were observed failing normally against their previous implementations before
passing with the fixes. A parent-cleanup probe removed a killed child's
fixtures, including a read-only nested directory. Four CLI fault probes
reached TIFF/EXR finalization (normal and resized), diagnosed write failure,
and returned exit 1 without termination. Clean make, Xcode Deployment/Opto,
and Blender native LTO builds passed without compiler warnings; only the
expected Xcode AppIntents metadata notices were discounted. Native Windows
verification remains outstanding.


### Fresh review follow-up after `f5f5488e1`

Three independent reviewers found three additional P1 defects and one P2
fixture collision. Each finding was confirmed and fixed:

| Finding | Correction |
| --- | --- |
| Existing-target probe required read permission | Open an existing-only write descriptor without creating or truncating the target. Write-only targets retain both staging policies, including direct image writes in restricted parents. |
| Partial thumbnail accepted as a cache | Encode the known 4x4 default PNG once, validate cached bytes against that complete payload, regenerate partial/foreign entries, and return an empty path after failed publication. A limited direct-write fixture verifies partial data cannot become a usable cache and a later call repairs it. |
| Repeated JPEG reads leaked decoded pixels | Reset and release the previous decode before each `BeginRead`, including failed reloads; reset output dimensions and internal image state. Valid reloads preserve every pixel. |
| Concurrent parser fixtures collided | On POSIX, parser and template tests run inside parent-owned scratch directories, extending the existing buffer/photon isolation. The Windows wrapper still runs in-process; native Windows fixture isolation remains deferred with the excluded platform work. |


The review fixes passed nine focused executables (including 139 buffer,
62 image, 448 template and 172 parser checks), five targeted ASan/UBSan
runs, and repeated clean make, Xcode Deployment/Opto and Blender LTO builds.
There were no unexpected warnings. The new write-only, failed JPEG reload
and partial-thumbnail regressions failed normally against `f5f5488e1` and
passed with their fixes. Two concurrent parser copies sharing a parent
`TMPDIR` each completed all 172 checks and removed their owned directories.
A 10,000-reload JPEG allocator probe no longer retained growth after reader
cleanup (the final snapshot was 1,936 bytes below the initial decode
snapshot; the unfixed probe grew by 8,958,064 bytes). The four CLI write
failure probes still reached TIFF/EXR finalization and exited cleanly.
Multi-GiB sparse cases remain explicitly skipped on this host, and native
Windows execution remains outstanding.


### Second fresh review follow-up after `50ce03c74`

The next independent round found two more P1 defects: the viewport `SaveTo`
checker hid a destination's optional `IBuffer64`, and a restrictive umask
could permit creating a staging directory while denying creation of its
output file. `SaveTo` now chooses a wide checker only for actual wide
buffers, forwards large queries/seeks, and latches failed seeks. Legacy
buffers keep their original interface. Resize refusals remain advisory
because streaming encoders use them as capacity hints, including on disk
buffers that intentionally decline resizing.

Stage-open errors are captured before logging can change `errno`. Explicit
image fallback now handles permission-denied staging opens before the
encoder runs; encoding and flush failures never replay the callback.
The existing public constructor symbol and concrete layout are retained.
New umask-policy and virtual large-buffer fixtures fail against the earlier
code and pass with these corrections without creating large files.

The round also identified Windows-only fixed-name fixture collisions.
POSIX isolation and crash cleanup are verified; the Windows wrapper still
runs in-process. Native Windows fixture isolation joins the platform work
excluded from this host continuation. This is a disclosed remaining item,
not a claim of cross-platform isolation.


These host corrections passed seven focused executables (including 144
buffer and 93 viewport checks), three targeted ASan/UBSan runs, and repeated
clean make, Xcode Deployment/Opto and Blender LTO builds, with zero unexpected
warnings. The staging-open and `SaveTo64` regressions were observed failing
normally against `50ce03c74` before passing with the corrections. Four CLI
TIFF/EXR limited-write probes still reached finalization and exited 1 without
termination. Sparse multi-GiB cases remain explicitly skipped on this scratch
filesystem.


### Third fresh review follow-up after `f4ee9ef82`

The next round reported two further edge cases: TGA output truncated
16-bit dimensions, and an initial failed thumbnail encoding permanently
initialized its static payload as empty. The TGA writer now checks format
and storage limits before emitting a header, safely replaces prior pixel
storage, bounds pixel writes, and reports failed header/pixel output. CLI
preflight rejects oversized TGA dimensions before opening the destination.
The thumbnail initializer now throws internally on incomplete encoding;
its caller returns an empty path, and C++ static initialization retries on
the next healthy call. A targeted one-shot PNG allocation failure verifies
recovery rather than a permanently poisoned cache.

The sibling audit also confirmed that the TGA reader consumed only 17
header bytes, omitting the descriptor and shifting alpha into the pixel
stream. It now reads 18 bytes, safely skips the image ID, decodes BGRA,
normalizes both origin axes, and handles declared alpha versus opaque
padding. It retains the supported uncompressed 32-bit truecolor subset;
compressed, paletted, 24-bit and interleaved inputs are diagnosed as
unsupported. TGA 2.0 extension metadata is not interpreted; supported alpha
is straight. The [Truevision specification](https://raw.githubusercontent.com/DEAKSoftware/Truevision-TGA/master/truevision-tga.pdf)
and the local stb/Pillow decoders were used to cross-check byte order and
origin fields. Independent handcrafted fixtures cover these cases, plus a
writer/reader RGBA round trip. `TGAReader` gains stored height for bounds
checking, so concrete implementation clients must rebuild. The public
C-style construction boundary remains unchanged.


This follow-up passed all 15 focused executables, including 102 image
checks, 156 frame-encoder checks, 449 template checks, and 480 exact corpus
matches with zero drift, plus six targeted ASan/UBSan runs. Clean make,
Xcode Deployment/Opto and Blender LTO builds passed without unexpected
warnings. New TGA bounds/layout and thumbnail encode-retry fixtures failed
normally against `f4ee9ef82` and passed with the corrections. Pillow
independently decoded 65,535-by-1 and 1-by-65,535 CLI outputs, a
Pillow-authored TGA converted to matching PNG RGBA/orientation within 8-bit
quantization, and both 65,536-axis CLI cases failed while preserving the
previous destination. Existing TIFF/EXR limited-write checks still passed.
Sparse multi-GiB cases and native Windows/Android work retain their stated
limits and deferrals.


### Host measurements and decisions

- A 50,000-scalar append probe reduced backing allocations from 50,000 to 28, with identical logical size, cursor and checksum. Four warm host trials measured 75.33 ± 3.26 ms for exact-growth storage and 0.2245 ± 0.0065 ms for geometric capacity. These are append-probe measurements. A sanitizer differential review also exercised 192,000 buffer operations against a shadow model.
- A 114,242-triangle PLY import and mesh serialization produced byte-identical files before and after the vector-helper replacement. Warm imports rounded to 0.18 s in both implementations, with peak RSS around 97 MB. This host's vector implementation honored the compaction request. The standard permits `shrink_to_fit()` to retain capacity.
- The `std::mt19937` prototype matched tested 53-bit samples across four scalar seeds and copied states around block boundaries. Ten-million-draw warm trials measured 29.280 ± 0.119 ms for the standard engine and 21.008 ± 1.607 ms for the existing engine. The existing engine is retained following the plan's regression criterion. Array-seed and wider-platform evaluation would be required for a future adoption candidate.
- An isolated intrusive-count prototype measured mutex versus atomic retain/release pairs at 2.18 ± 0.05 ms versus 0.69 ± 0.01 ms for one thread, and 32.57 ± 0.54 ms versus 9.03 ± 0.37 ms for four threads, using 200,000 pairs per thread. Renderer-wide ownership cost has yet to be established. `Reference` embeds a mutex in a publicly subclassable concrete type, so a production atomic replacement still needs a concrete-layout compatibility decision and concurrency validation.
- The final corpus sweep exposed an invalid `output f2f1` token in `graph_stress_wide.RISEscene`. Its former unchecked conversion reached an uninitialized integer. The scene now uses canonical `f1`, which reproduces its existing golden digest exactly. Invalid output tokens are rejected, and focused tests guard rejection before job mutation.

### Initial host verification at ffffa9633

- Clean make and Xcode Deployment and Opto builds passed, followed by a clean Blender native LTO bridge link. Compiler output was warning-free. Xcode emitted its expected AppIntents metadata-extraction notice.
- The initial series passed all 21 focused test executables. Coverage includes scalar and packet bytes, mesh compatibility, memory buffers, image ownership and codecs, parser and options behavior, descriptor mutation, mesh-cache construction, native keyframes, camera units and editor angles, animation/render parity, frame output, viewport storage and source-list hygiene. The corpus has 480 exact matches, zero drift, zero uncovered entries and zero stale entries. Animation parity passed 102 checks.
- Targeted ASan/UBSan reruns passed for `VolumeShortReadTest`, `ImageReaderOverflowTest` and `NativeKeyframeAllocationTest`, with buffer, HDR/RGBEA writer, strict-parse and camera implementation objects instrumented.
- Fresh adversarial rounds covered growth contracts and measured cost, CLI and descriptor parsing, raster lifetime and reload, timing bounds, and final file-output behavior. Review findings fixed missing command arguments, media-path spelling, nested borrowed-view lifetime, unsigned conversion before mutation, and empty prediction samples. The first camera review round found zero P1s and requested six-character ASCII whitespace support and stronger valid-application assertions. Both P2 findings were resolved in a separate commit. The final fresh review covers the resulting committed code and this checkpoint.

## Audit baseline and approved sequence

Start with live correctness fixes for spectral packet assignment, scene and asset parser bounds, binary I/O byte order, and failed buffer writes. Then fix Halton table ownership, script and parser resource lifetimes, and the Windows animation build workaround. One `MultiHalton` construction and destruction performs 1,875 array allocations and 257 array frees. A local allocation probe counted this, and the numbers follow from the code (the 256th prime is 1619). Manager name lookups and per-object reference-count mutexes remain performance candidates. Their workload impact needs a baseline.

The plan uses the existing C++17 baseline across make, Xcode, VS2022, Android, and the Blender native bridge (`gnu++17` on make, Xcode, and Android, `stdcpp17` on VS2022, `c++17` on the Blender Makefile). It covers the utilities and recurring implementation patterns listed below across sampling, spectral values, images, importers, scene and script parsing, animation, logging, and platform builds. It is not a full inventory of `src/Library/Utilities`. The section "Out of scope and not yet audited" lists what is excluded. Frozen projects inform the compatibility inventory. Sampling sequences and rendering algorithms provide the reference behavior for storage changes.

## Severity of the findings

| Class | Findings |
| --- | --- |
| **Undefined behavior or wrong output today** | `SpectralPacket` mismatched array deletion and stale bin width. Unbounded scene and asset parser reads. Wrong byte order in the Xcode `rise` target and the ordinary 16-bit PNG reader. Out-of-bounds writes when a memory buffer cannot grow, invalid file-handle use and false success on disk writes, and log records left unterminated at 1,024 characters. |
| **Leaks and resource lifetime** | `MultiHalton` rows (roughly 5 MB per instance). `AsciiScriptParser` command parser on failure exits. |
| **Latent hazards** | `String::concatenate` key inequality. `Reference::release()` returning with the mutex held on over-release. `r_min` / `r_max` macro hygiene. Fixed 1,024-byte path buffers. `MathExpressionEvaluator` allocator overrun. |
| **Style and exception safety only** | Noise scratch arrays, `HairFileLoader` repeated `fclose`, descriptor name slabs, and the registry's raw parser pointer. None of these leaks on current paths. |

## Additional findings from the renderer and tools

| Area | Evidence and proposed migration | Verification |
| --- | --- | --- |
| **Halton permutations** | `MultiHalton` constructs temporary rows for every integer base up to the largest prime, frees only the prime rows, and has no destructor for the retained `SIGMA` arrays. Each instance therefore leaks the 1,362 composite rows and the 256 `SIGMA` rows, roughly 5 MB. Its header also declares an unused internal-linkage instance, so every translation unit that includes the header pays one construction at startup. Instances are also built in render paths: `HaltonPointsSampling2D` holds one as a member, and `SubSurfaceScatteringShaderOp` and `DonnerJensenSkinSSSShaderOp` build one per point-set generation. Use one lazily initialized immutable table with RAII-owned rows and keep only per-dimension counters in each sampler. Remove the unused instance and check the concrete header surface before changing its layout. [HaltonPoints.h](../src/Library/Sampling/HaltonPoints.h#L108) | Compare exact samples across all dimensions, independent instances, and resets. Include the `HaltonPointsSampling2D` and SSS point-set consumers. Compare v6-to-v7 migration bytes for scenes with `hal()`. Repeat the allocation probe and measure CLI startup and peak memory separately. [CstMigrator.h](../tools/CstMigrator.h#L140) |
| **Spectral packet value semantics** | The changing-bin-count assignment path uses scalar `delete` on an array. The default 30-bin blackbody constructor reaches it through `RegenerateData()`, which assigns over a default-constructed one-bin member. A one-bin construction and regenerations with the same bin count take the equal-count paths. The equal-count, different-range path changes the wavelength endpoints without recomputing `delta`. Assignment returns a packet by value, and scalar multiplication copies its already by-value argument. Fix assignment and metadata first. Consider `std::vector<Scalar>` and normal copy/move operations after checking layout and signature compatibility. `IPainter::GetSpectrum` returns this type by value. [SpectralPacket.h](../src/Library/Utilities/Color/SpectralPacket.h#L204) · [IPainter.h](../src/Library/Interfaces/IPainter.h#L147) | Add direct assignment tests for changing bin counts and equal-count range changes. Check endpoints, bin width, amplitudes, and wavelength lookup. Run the blackbody constructor case under ASan. `SpectralPainterScaleTest` already reaches that assignment indirectly through blackbody creation. Keep the fixed-size HWSS path on its existing representation. |
| **Byte order and scalar serialization** | All four Xcode configurations of the `rise` target (Development, Deployment, Default, Opto) pass `-DRISE_BIG_ENDIAN` on this Apple Silicon host. The `RISE-GUI` target, make, VS2022, and Android omit it. Buffer implementations duplicate byte packing behind the flag. Four flagged writers mask the high half with `0xFFF` instead of `0xFFFF`: `setInt` and `setUInt` in both `DiskFileWriteBuffer.cpp` and `MemoryBuffer.cpp`. `setFloat` and `setDouble` route through `setUInt`, so the flagged build clears the high four bits of the float representation and of each 32-bit half of a double. On arm64 the flagged double path also writes the halves in reverse order relative to the ordinary path. A flagged read cannot recover masked bits. The flagged 16-bit PNG writer stores native-order samples, and the flagged PLY loader inverts the flip decision. The ordinary 16-bit PNG reader independently interprets libpng’s network-order samples as little-endian. That reader bug is in every build, while the ordinary writer is correct, so a 16-bit PNG written by RISE does not read back as written. The five tracked 16-bit PNGs are all application icons, so no tracked render input is affected. User assets and PNG data embedded in imported files are not covered by that count. Split the target flag correction from the codec changes. Once the flag leaves the Xcode `rise` target, only the frozen SGI make configurations set it. Delete the flagged branches in favor of the explicit per-format byte order instead of repairing them, unless the frozen-surface policy requires them to stay. Use explicit per-format byte order using fixed-width types, byte operations and `memcpy` for FP representation under C++17. Treat any incorrectly written historical bytes as a compatibility investigation. [Xcode project](../build/XCode/rise/rise.xcodeproj/project.pbxproj#L7155) · [DiskFileWriteBuffer.cpp](../src/Library/Utilities/DiskFileWriteBuffer.cpp#L80) · [MemoryBuffer.cpp](../src/Library/Utilities/MemoryBuffer.cpp#L593) · [PNGWriter.cpp](../src/Library/RasterImages/PNGWriter.cpp#L124) · [PNGReader.cpp](../src/Library/RasterImages/PNGReader.cpp#L225) · [TriangleMeshLoaderPLY.cpp](../src/Library/Geometry/TriangleMeshLoaderPLY.cpp#L855) | Capture historical fixtures from the affected target before removing the flag. Verify scalar and serialized mesh and photon bytes against independent expectations. Verify 16-bit PNG sample values and little- and big-endian PLY geometry using independent fixtures. Add a 16-bit PNG regression test, since none exists: an independent fixture plus a write-then-read round trip. Before the reader fix, check whether embedded PNG textures in imported assets reach this reader, and compare before and after renders for any scene that does, because the fix changes how 16-bit textures, normal maps, and displacement maps read. Check RGBEA where the flag reaches that codec. |
| **Script and descriptor resource ownership** | `AsciiScriptParser` acquires a command parser before early returns and releases it only on success, so the missing-marker, wrong-version, and per-line failure exits leak it. Descriptor finalizers allocate paired 256-byte name slabs and pointer arrays for Voronoi painters, modifiers, and shader ops. The slabs are freed on the single normal return, so they are an exception-safety concern and not a current leak. Registry construction accepts a raw parser pointer before ownership is assigned, with a leak window only on allocation failure. Acquire RAII ownership immediately. Pass a scoped `std::vector<const char*>` over existing owned names where the callee consumes them synchronously. Keep the public pointer/count signatures and decide how to handle historical truncation explicitly. [AsciiScriptParser.cpp](../src/Library/Parsers/AsciiScriptParser.cpp#L49) · [ChunkParserRegistry.cpp](../src/Library/Parsers/ChunkParserRegistry.cpp#L3119) · [Registry construction](../src/Library/Parsers/ChunkParserRegistry.cpp#L14041) | Cover script header and command failures, zero entries, list ordering, long names, and descriptor registration parity. Use direct ownership checks or sanitizers for early exits. |
| **Unbounded token reads** | The `advanced_shader` finalizer reads each `shaderop` entry with `sscanf( ..., "%s %u %u %c", buf, ... )` into `char buf[256]`. A shader-op name longer than 255 characters in a scene file overflows the stack. The same pattern reads `cp` entries into `char y[1024]`, the HDR header into `char dummy[32]`, and RAW mesh lines into a fixed buffer. The shader finalizer also dereferences empty vector iterators. HDR scans for an `X` without a line bound, and RAW ignores conversion counts. Parse complete records using bounded tokens and checked conversions. Preserve valid shader shorthand and its optional-field defaults. Decide explicitly how a zero-entry shader should behave, and reject invalid records before using their fields. [advanced_shader finalizer](../src/Library/Parsers/ChunkParserRegistry.cpp#L11478) · [cp entries](../src/Library/Parsers/ChunkParserRegistry.cpp#L3501) · [HDRReader.cpp](../src/Library/RasterImages/HDRReader.cpp#L95) · [TriangleMeshLoaderRAW.cpp](../src/Library/Geometry/TriangleMeshLoaderRAW.cpp#L49) | Test missing, malformed, and overlong tokens under ASan, including an HDR resolution line missing the X axis, one where `X` is the first character, and incomplete RAW records. Cover valid shader shorthand and the zero-entry decision. Use `CstDeriveGoldenTest` to check existing scene behavior. |
| **Image, importer, and procedural scratch ownership** | `RasterImage_Template` uses a raw pixel pointer for owned, adopted, borrowed, and parent-backed storage. Noise builders allocate several raw scratch arrays. `HairFileLoader` repeats `fclose` on error exits. The scratch arrays and the file handle are released on every current path, so these two are maintenance and exception-safety items. Convert local scratch and file ownership first. For image storage, represent ownership explicitly and keep views tied to their backing store, with checked size and stride arithmetic. [RasterImage.h](../src/Library/RasterImages/RasterImage.h#L29) · [ReactionDiffusion3D.cpp](../src/Library/Noise/ReactionDiffusion3D.cpp#L61) · [WaveletNoise3D.cpp](../src/Library/Noise/WaveletNoise3D.cpp#L95) · [HairFileLoader.cpp](../src/Library/Importers/HairFileLoader.cpp#L269) | Verify clipped and strided views, deep copies, parent lifetime, reload, and adopted storage. Use `HairFileImportTest`, `ImageReaderOverflowTest`, and the noise tests. Compare pixel or tile values and peak memory, including any cost of value-initializing large arrays. |
| **Windows template workaround** | VS2022 excludes `KeyframableHelper.cpp` from compilation and defines `INLINE_TEMPLATE_SPECIALIZATIONS` so the header includes the implementation. The in-tree history is in the `KeyframableStrictParse.cpp` header comment: free functions in the re-included `.cpp` produced `LNK2005` duplicate definitions, which is why the strict parse functions were split out, and dropping `inline` was needed because LTO removed the symbol that the Blender bridge links. The tests CMake file deliberately does not mirror the macro for the same reason. Start from that history. Try compiling the existing explicitly declared specializations once on Windows, matching the other build systems. Keep the separately linked strict parse functions. The public `void*` keyframe API can gain typed internal callers independently if the ownership or type audit warrants it. [KeyframableHelper.h](../src/Library/Animation/KeyframableHelper.h#L158) · [VS2022 project](../build/VS2022/Library/Library.vcxproj#L130) · [KeyframableStrictParse.cpp](../src/Library/Animation/KeyframableStrictParse.cpp) | Clean MSVC Debug and Release link checks, `NativeKeyframeAllocationTest`, `AnimationRasterizerParityTest`, and the Blender native link against the LTO archive. |
| **Legacy expression allocator** | The v6 migrator still uses `MathExpressionEvaluator`, which has a global 1 KiB `MemStack`, scanner, and error state. The allocator logs on exhaustion and still returns a pointer, so the caller writes out of bounds. `tests/CstDeriveGoldenTest.cpp` also consumes the evaluator through `CstMigrator.h`. The evaluator is compiled into the library in all five build lists, and no library code path calls it, so moving it means the five-project source checklist. Move the evaluator into migration-tool support once its consumers are confirmed, with per-evaluation state and RAII-owned nodes. Consider an arena only if the tool's workload justifies it. [MathExpressionEvaluator.h](../src/Library/Parsers/MathExpressionEvaluator.h#L50) · [CstMigrator.h](../tools/CstMigrator.h#L151) | Compare exact migrated output for the existing corpus and check repeated runs, deep expressions, and parse failures. Preserve the legacy evaluator's rounding and operator behavior. |
| **Fixed-size output paths and log records** | `FileRasterizerOutput` concatenates folder and pattern into a 1,024-byte member with `strcpy` and `strcat`. The script loader copies resolved paths into another 1,024-byte buffer with an unchecked `strcpy`, in the same constructor as the parser lifetime fix above. The log uses fixed-size messages, and its plain-copy and Windows format paths can leave a full record unterminated. `Win32Console` concatenates that same record. These three sites are representative. `src/Library` has about 20 `strcpy`, 35 `strcat`, 4 `sprintf`, and 6 `sscanf`-with-`%s` calls, so inventory them before closing this row. Use owned paths at the private I/O boundaries and guarantee termination at the existing log callback boundary. [FileRasterizerOutput.cpp](../src/Library/Rendering/FileRasterizerOutput.cpp#L60) · [AsciiScriptParser.cpp](../src/Library/Parsers/AsciiScriptParser.cpp#L27) · [Log.cpp](../src/Library/Utilities/Log/Log.cpp#L75) | Check long and Unicode paths, existing suffix and frame numbering, log record bounds, and truncation behavior. Use `FileRasterizerOutputShimTest` for existing output parity. |

## Findings and proposed replacements

| Utility | Evidence and cost | Proposed disposition |
| --- | --- | --- |
| `RISE::String` in `RString.h` | Publicly derives from `std::vector<char>`. Construction from a C string scans twice and stores the terminator as an element. Appends rescan the accumulated text and add one more NUL element per call, so `String("a")` with `"b"` appended does not compare equal, as a map key, to `String("ab")`. C-string assignment returns by value. Numeric accessors ignore conversion failure and return an uninitialized value. The header has 38 direct in-tree includes, plus consumers through other headers and the PCH. | Use `std::string` for ownership and `std::string_view` for bounded, borrowed text. Move conversions to the parser or CLI where accepted syntax and errors are defined. Retain the existing type wherever binary compatibility requires it. [RString.h](../src/Library/Utilities/RString.h) |
| Named managers and options | `GenericManager` constructs a `String` for item and serial lookups and stores names in ordered maps. `Options` has a similar lookup pattern. These are direct allocation targets in scene loading, derive, and editor operations. | Use ordered `std::string` keys with transparent lookup using a length-aware view. Preserve enumeration order, empty-name behavior, serials, reference tracking, and callbacks. [GenericManager.h](../src/Library/Managers/GenericManager.h) · [Options.cpp](../src/Library/Options.cpp) |
| `BitField`, `MRUCache`, `SimpleNotification`, `CoreUtils`, `DynamicProperties`, Windows `LoadLibrary` wrapper, `SmartColor`, `WardToneMappingOperator`, `NullScatteringTracker` | The tracked source sweep found no consumer of these utilities outside their own definitions and implementation includes. Some still appear in project files. `DynamicProperties` is a stub and the only one registered in all five build projects, so it is the one that compiles everywhere. `BitField.cpp` is still listed in Xcode and VS2022. The last three are header-only. | Delete after confirming the supported distribution and header surface. `Observable` still drives painter-to-displacement invalidation and stays in use. [DynamicProperties.cpp](../src/Library/Utilities/DynamicProperties.cpp) · [VS2022 Library project](../build/VS2022/Library/Library.vcxproj) · [DisplacedGeometry.cpp](../src/Library/Geometry/DisplacedGeometry.cpp) |
| `stl_utils.h` | `container_optimize` allocates replacement storage and copies elements in mesh and patch builders. `container_erase_all` releases storage. `shuffle` pairs points with values in order and has three callers in `RISE_API.cpp`. | Choose capacity retention or release at each call site. Measure compaction on large meshes before deciding where to retain it. Pair points and values in order at the factory boundary, then remove the helper header. `stl_utils.h` (removed in this series) · [TriangleMeshGeometryIndexed.cpp](../src/Library/Geometry/TriangleMeshGeometryIndexed.cpp) · [RISE_API.cpp](../src/Library/RISE_API.cpp#L5900) |
| `RTime` and `MediaPathLocator` | `RTime` uses 32-bit milliseconds and POSIX wall-clock time for elapsed measurements. `GetMilliseconds()` also supplies RNG seeds. The locator uses `stat`, string concatenation, and an ordered search list with an original-spelling fallback. | Use `steady_clock` for elapsed time and handle seed call sites independently. Keep the media search policy and adopt `std::filesystem` for path operations where behavior is verified. [RTime.cpp](../src/Library/Utilities/RTime.cpp) · [MediaPathLocator.cpp](../src/Library/Utilities/MediaPathLocator.cpp) |
| `RMutex`, `RMutexReadWrite`, and `Reference` | The plain mutex wrapper allocates a platform mutex on the heap. The read/write wrapper builds on two semaphores. Every `Reference` embeds an `RMutex`, while `refcount()` reads the count without taking that lock. `release()` on a count that is already zero returns with the mutex still held. The pthread mutex is nonrecursive and the Windows critical section is recursive. | Pilot standard mutexes on private call sites. Evaluate atomic intrusive counting independently with the existing `IReference` protocol. Keep native worker creation and controls for stack size, remaining stack, QoS, affinity, and topology. [Threads.h](../src/Library/Utilities/Threads/Threads.h) · [Reference.cpp](../src/Library/Utilities/Reference.cpp) · [ThreadPool.cpp](../src/Library/Utilities/ThreadPool.cpp) |
| `MemoryBuffer` and disk buffers | Memory growth can allocate and copy to the exact requested size on successive writes. The buffer may own, adopt, or borrow its storage. `setBytes()` ignores a failed `Resize()`, then copies past borrowed capacity if the write does not fit. Disk scalar writers can use a null file handle, and write methods commonly ignore `fwrite` counts and report success. Public factories and serializers use these interfaces. | Fix write failure handling and checked size arithmetic first. Preserve state when a memory write is rejected, check disk handles and write counts, and verify failure propagation at the relevant output boundary. Keep the interfaces and existing wire formats. Evaluate RAII storage and capacity policy in a later change. [MemoryBuffer.cpp](../src/Library/Utilities/MemoryBuffer.cpp) · [DiskFileWriteBuffer.cpp](../src/Library/Utilities/DiskFileWriteBuffer.cpp) · [RISE_API.h](../src/Library/RISE_API.h#L2529) |
| `MersenneTwister` and `RandomNumberGenerator` | The active builds select `MERSENNE53`. RISE maps two engine draws to one 53-bit sample and has seed and copy behavior that callers rely on. The copy test already uses `std::mt19937` as an integer-stream reference. | Prototype `std::mt19937` as the engine behind the existing sampling interface, with the same integer-to-double mapping and draw order. Adopt only after exact sequence and performance comparisons. [RandomNumbers.h](../src/Library/Utilities/RandomNumbers.h) · [MersenneTwister.cpp](../src/Library/Utilities/MersenneTwister.cpp#L275) · [MersenneStateCopyTest.cpp](../tests/MersenneStateCopyTest.cpp) |
| `r_min` / `r_max`, ownership helpers, and `MemoryTracker` | The macros can evaluate an operand twice and select the second argument on ties. Their arguments are not parenthesized, so an expression argument with a lower-precedence operator misparses. The public reference helpers encode ownership behavior. `MemoryTracker` is tied to logging flags that are commented out in the source. | Migrate integer min/max in touched code. Audit operand types, ties, signed zero, and NaNs before changing floating-point sites. Use standard RAII for clear local ownership where the deleter matches the resource. Decide whether the optional memory tracking mode is still used before removing it. [math_utils.h](../src/Library/Utilities/math_utils.h) · [IReference.h](../src/Library/Interfaces/IReference.h) · [Log.cpp](../src/Library/Utilities/Log/Log.cpp) |

## Out of scope and not yet audited

- Renderer domain code with no standard equivalent stays as it is: `Math3D`, the color templates, interpolators, `MortonCode`, `AliasTable`, `Primes`, the samplers, `IORStack`, `ExpressionMemo`, and `FiniteMath.h` (kept for `-Ofast` resistance).
- Not yet audited, with a likely standard equivalent: `Communications/*` (the DRISE and PRISE socket layer), `CPU_Count.cpp`, `Profiling`, and the logger core and `Win32Console` beyond record termination. `DiskBuffer` ownership and `StreamPrinter` locking have been covered by the host implementation. Decide for each whether it joins this plan or is recorded as frozen with the distributed renderers.

- Frozen DRISE backlog. POSIX `riseSleep` builds an absolute deadline from truncated milliseconds, so the deadline has already passed. The DRISE client is its only in-tree caller. [ThreadsPTHREADs.cpp](../src/Library/Utilities/Threads/ThreadsPTHREADs.cpp#L366) · [drise_client.cpp](../src/DRISE/drise_client.cpp#L375)

## Implementation sequence

### 0. Capture the baseline and decide the compatibility boundary

- List shipped headers, exports, and supported external C++ consumers. `IOptions`, `IScene`, `IAnimator`, and `IKeyframable` expose `String`. `PixelFilterConfig` and `RadianceMapConfig` embed it and cross `IJob` calls. A type alias can change symbols or layouts even when the written virtual signature looks identical. Include the CLI, both desktop GUI bridges (the macOS `RISE-GUI` bridge and the Windows Qt GUI), Android, and the Blender native bridge in the consumer map. [IAnimator.h](../src/Library/Interfaces/IAnimator.h) · [IJob.h](../src/Library/Interfaces/IJob.h) · [Blender bridge](../src/Blender/native/rise_blender_bridge.cpp)
- Capture the relevant behavior and performance baselines before modifying each area. Start with Halton allocation counts, exact sequences, and startup RSS. For string work, record manager hit/miss allocations, cold scene load, and editor derive time on a name-heavy fixture. For later series, record large mesh import time and peak RSS, object construction/destruction allocations and reference operations, buffer growth and file throughput, or RNG draw and render throughput as appropriate.

### First implementation series

1. Land independent correctness fixes for `SpectralPacket` assignment, complete scene and asset record validation, and buffer write failures. Capture historical Xcode `rise` fixtures, then land the target flag correction and codec fixes separately. Keep storage and file-format redesigns in later changes.
2. Fix `MultiHalton` ownership and eliminate the unused header instance. Prove exact sample and migration output first, then measure startup and memory on a real workload.
3. Fix script parser lifetime and modernize descriptor argument staging in separate changes. Handle fixed-size output paths and log records as focused follow-ups with boundary tests.
4. Retire the Windows template workaround once native link coverage is available. Remove unused utilities in a separate change.

The string work below can proceed independently after the compatibility inventory. Later changes can apply the RAII patterns to image and procedural storage, then isolate the legacy expression evaluator after byte-for-byte migration checks. Treat `std::span`, `std::endian`, and `std::bit_cast` as options for a future toolchain decision. The current plan can express ownership and wire formats in C++17.

### 1. Remove unused utilities and migrate internal strings

- For dead utility removal, inspect all five build surfaces for source and header registrations and run `SourceHygieneTest`. Its IJob manifest also guards written signatures, with a separate symbol, layout, and old-client check where ABI is promised. [SourceHygieneTest.cpp](../tests/SourceHygieneTest.cpp) · [AGENTS.md](../AGENTS.md)
- Pilot manager name lookup with allocation-free probes into ordered maps. Check exact key equivalence and enumeration with empty, prefix-related, and non-ASCII names. The old map compares vector elements as `char`, including the terminator, so test byte ordering before choosing the comparator. Include keys built by `concatenate`, which carry extra trailing NUL elements. `ObjectManager` also walks this map in several places, including the proximity box snapshot at the linked line, so include a fixed-seed render check for the pilot. Exercise duplicate registration, removal and re-add, callback/refcount behavior, and CST production and resolution traces. Use `CstReferenceTraceTest`, `CstStableApplyTest`, and `PainterRemovalManagerSyncTest`. Apply the same lookup pattern to the private options map. [ObjectManager.cpp](../src/Library/Managers/ObjectManager.cpp#L1023)
- Convert descriptor parsing, CLI token handling, and editor internals in separate changes. Inventory token semantics first, including first-character booleans, numeric prefix and sign rules, unsigned sentinels, missing versus malformed values, and whitespace or quoting. Reuse existing CST rules where they match. Add checked conversion at remaining call sites and define each invalid-input outcome explicitly. Validate camera keyframes, animation, CLI scripts, descriptor output, and CST golden and round-trip tests. Use owned strings for stored values and views for reads with a clear lifetime. Audit vector-based length, indexing, and trailing-NUL assumptions and convert to a terminated string at C-library calls.
- Migrate the remaining `String` boundary uses after the compatibility decision. With ABI preservation, keep the original class and layouts at existing entry points and convert at the edge. With an approved ABI version change, update the affected interfaces and config layouts together, rebuild their consumers, and remove `RString.h` after the last dependency is gone.

### 2. Simplify time, paths, and container helpers

- Migrate elapsed-time callers first. Preserve reported units, progress behavior, and intentional seed behavior. Check duration conversions where the existing interface still takes an integer. Remove the old timer after the last supported caller moves and update the profiling guidance that points to it. [Performance baselines](skills/performance-work-with-baselines.md)
- Migrate media path internals after the string boundary is established. Test direct-path precedence, ordered fallback, missing paths, relative and absolute forms, UTF-8 filenames on Windows, separator handling, and empty-root behavior. Check whether each consumer accepts directories or requires regular files. Resolve duplicate and removal semantics in the same tests.
- Replace the zip operation and remove stale includes. Then change mesh and patch capacity management separately. Compare import time, peak RSS, retained capacity, and geometry output before dropping forced compaction. `shrink_to_fit()` is a request, so a memory-release requirement needs a measured implementation.

### 3. Modernize private locking and intrusive ownership

- Convert private `RMutex` users in small groups. Check for reentrant acquisition before replacing Windows critical sections with nonrecursive locks. Use scoped locking, preserve lock ordering, and check cache reader/writer progress before adopting `std::shared_mutex`. Preserve the topology-aware pool, worker stack settings, and platform priority policy. [PERFORMANCE.md](PERFORMANCE.md)
- Prototype an atomic counter behind `IReference` if the baseline warrants it. Preserve initial count, copy-as-new-object behavior, `release()`'s return value, deletion logging, and one final destruction. Require callers to hold a live reference when invoking ownership operations. Ensure the existing zero-count branch releases its lock. Calls through a destroyed object are outside the ownership contract. Verify memory ordering, manager shutdown, render snapshots, and concurrent retain/release under valid ownership. Check concrete class layouts for supported external subclassing. Run ASan and a supported TSan configuration, plus the thread-pool, viewport, and editor lifetime tests. Use `unique_ptr` with a `release()` deleter only for code that owns a reference. The engine continues to use intrusive ownership.

### 4. Modernize buffers and evaluate the RNG engine

- After the write-failure fixes, migrate file handle ownership and audit remaining read outcomes. Then change memory storage while preserving adopted and borrowed storage rules, copy semantics, cursor movement, logical size, and raw-pointer invalidation behavior. Check failure paths and size boundaries, including attempts to grow a borrowed buffer. Use golden bytes and old-file reads with `RISEMeshLegacyBSPCompatibilityTest`, `BVHSerializationTest`, `PhotonPacketSerializationTest`, and `VolumeShortReadTest`. Measure separately before choosing a growth policy.
- Prototype the RNG engine only after profiling. Extend the sequence comparison to exact 53-bit output, reseeds, copies at block boundaries, and fixed seeds. Check old array seeding if it is part of the supported surface. Compare state size, construction/copy cost, scalar draw cost, and render throughput under the supported standard libraries. Keep the current engine if the prototype misses sequence parity or shows a repeatable workload regression.

## Verification and review

- Land independently reversible changes with a focused contract test or existing regression suite for each. Use the repository's independent review loop for nontrivial implementation. File additions and removals follow the five-project checklist. Check clean make and Xcode Deployment builds for warnings, and use Xcode Opto for FP- or LTO-sensitive changes. Build affected Windows, Android, and Blender targets before closing changes that cross their boundaries. [Implementation review loop](skills/implementation-review-loop.md) · [AGENTS.md](../AGENTS.md)
- Compare before and after on the same machine and workload using at least three trials, preferably five. Record mean and standard deviation and allocation count or peak RSS where relevant. Run render comparisons sequentially with fixed settings and `render_thread_reserve_count 0`. Use exact output for deterministic storage and ownership changes and document any numeric tolerance for a pure refactor. Check Pel, NM, and HWSS when a change reaches shared sampling or integrator paths. [Performance baselines](skills/performance-work-with-baselines.md)

## Decision for review

Start with the live correctness fixes in the first series, then Halton ownership and the resource and build fixes. Preserve supported C++ binary compatibility by default. A versioned ABI change could simplify public string, spectral packet, and keyframe interfaces if that compatibility is expendable. Keep reference counting, buffer growth, and the RNG engine as separately reviewable changes whose scope depends on the baseline.
