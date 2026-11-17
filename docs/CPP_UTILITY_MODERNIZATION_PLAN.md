# C++ utility modernization plan

**Status:** Merged to `master` in `556bf4a3b` (audit baseline `5fc6acaef`). Host follow-up work on `codex/cpp-utility-modernization` reached a major roadblock (parallel BDPT intermediate output needed a synchronized snapshot contract). That roadblock and the two further P1s recorded with it (MLT small budgets, thread-pool submission failure) landed in commit `2566595ae` (see "Resolution of the snapshot, MLT budget and submission blockers"); the P2s that round recorded landed in `53805a047` (part (e) of that subsection; two review rounds, the second with no P0 or P1). Three findings of a later independent review of `680f0c44b..53805a047` plus their P2 follow-up landed in `9dba00e2a` (part (f)) and merged to `master` in `556bf4a3b`. Completed increments have macOS host verification (make all + 529 standalone tests + Blender bridge + Xcode Development `RISE-GUI`); native Windows and Android build/runtime verification and an Xcode Deployment (`Opto`) re-run remain host-gated.

## Implementation checkpoint

Compatibility tables summarize current behavior. Later follow-up sections are
chronological checkpoints; the latest third-pass review section supersedes
earlier cache and image-publication details.

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
| 5. Restricted staging parents | Render output and viewport Save As preserve existing destination identity by copying completed staged bytes in place. New destinations use rename; eligible permission/EBUSY publication failures copy the completed stage. Denied or unsafe staging parents, denied staging opens, or a dangling output symlink permit direct encoding. Failed copying retains the complete stage for recovery; direct encoding can leave partial output. Thumbnails use separate validated no-follow cache handles. Photon saves require staging and atomic replacement. |
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
  inline control points are newly rejected: the old unchecked parser supplied
  zero amplitude. Cylindrical UV axes now require lowercase `x`, `y`, or `z`.
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
| Partial thumbnail accepted as a cache | Encode the known 4x4 default PNG once, validate cached bytes against that complete payload, regenerate partial/unrelated owned entries, and return an empty path after failed publication. A limited direct-write fixture verifies partial data cannot become a usable cache and a later call repairs it. |
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


### Test-integrity follow-up after `8e70540df`

The codec review found no remaining production host P1/P2, while the test
review reproduced concurrent TIFF-fixture collisions in the image suite
and found redundant retains in its TIFF driver. The image suite now uses
POSIX parent-owned scratch storage. The TIFF helper removes both extra
retains and asserts that reader destruction releases its borrowed buffer
reference and that the wrapper is destroyed, for successful and rejected
reads. Restoring the old retains caused eight normal assertion failures;
removing them restored all 110 image checks. Three trials of eight concurrent
image-suite copies passed all 110 checks each and left no owned directories.
A targeted image ASan/UBSan run and all 192 source-hygiene checks passed,
with no compiler warnings from the changed test. Production sources remain
unchanged from the fully rebuilt `8e70540df` checkpoint. The Windows wrapper
still requires the already-deferred native fixture-isolation work.


### Reader-family closure after `aa62da0df`

The next source review confirmed repeated TIFF decoding still overwrote an
old raster and retained stale pixels after an empty reload. A sibling sweep
found the corresponding HDR reset gap, PNG failed-dimension/bounds gaps,
and EXR failed-frame/bounds gaps. The remaining family is now handled
uniformly: clear the previous active image first, leave output dimensions
zero until a complete decode, permit healthy reuse after failure, make
`EndRead` inactive and repeatable, and leave caller colors unchanged for
inactive or out-of-range reads. JPEG gains the same coordinate guards.

TIFF handles and temporary pixels are scoped through failure; strict
stop-on-error and straight-alpha conversion remain unchanged. HDR keeps
its established RLE and orientation rules. PNG keeps its transforms and
scanline/interlace loop without introducing automatic C++ objects across
libpng's longjmp boundary. EXR decodes transactionally into reusable owned
storage, checks widened window extents and a 2 GiB owned-pixel budget,
uses the SDK's `Slice::Make` for offset addressing, and returns diagnosed
failure on data errors. FLOAT values above half range and missing-channel
fill remain covered. HDR/PNG/TIFF now also retain height for coordinate
checks; concrete implementation clients must rebuild alongside TGA clients.

One regression spans all six reader formats, exercising repeated success,
empty-source failure, recovery, repeated end, input-reference teardown and
coordinate bounds. It fails normally against the earlier reader code and
passes with the corrections. An independent R-only FLOAT EXR fixture with
a shifted negative-X/positive-Y data window verifies values above 65,504,
zero missing RGB channels and opaque missing alpha. The image suite passes
177 checks at this checkpoint.

The `07dcf4dee` host gate passed all 15 selected executables (including
480 exact scene-corpus matches), clean make and Xcode Deployment/Opto
builds, the Blender native LTO link, four CLI TIFF/EXR write-failure paths,
and independent TGA boundary/orientation checks. No compiler warnings were
reported apart from the expected Xcode AppIntents metadata notice. Six
targeted ASan/UBSan executables passed with container checks enabled.
The sanitizer harness also instruments the mesh-bake object that shares
libc++'s out-of-line float-vector assignment; mixing that uninstrumented
instantiation with instrumented EXR storage caused an initial annotation
mismatch. Three trials of eight concurrent image-suite copies passed all
177 checks per copy and left no owned scratch directories. Multi-GiB sparse
fixtures remain skipped on this host after the filesystem allocation probe;
Windows/Android gates remain outside this host continuation.


### Valid-format closure after `f70cbc550`

The sixth fresh review round found no P1 and two confirmed format-coverage
P2s. RGB-only FLOAT EXR slices silently produced black for luminance and
chroma files, and legacy HDR RLE treated chained count bytes independently.
Both are corrected with regressions that fail normally against the immutable
prior library archive.

EXR preserves the [SDK channel-selection and reconstruction rules](https://openexr.com/en/latest/ReadingAndWritingImageFiles.html):
plain Y/YA takes precedence over RGB, reads luminance as FLOAT and replicates
it to RGB; any RY/BY channel selects the SDK's historical half-precision
YC/YCA reconstruction, including subsampling filters and chromaticities.
RGB and plain Y/YA retain FLOAT values above 65,504. YC/YCA retains the
historical half range and precision. The 2 GiB budget covers combined owned
FLOAT/half pixel storage, including retained capacity; the SDK's internal
scratch allocations are additional. Independent FLOAT Y/YA fixtures cover
range, alpha and offset windows, and SDK-authored subsampled YC/YCA fixtures
compare every decoded pixel against the SDK result.

HDR legacy repeat packets now follow the [reference Radiance decoder](https://raw.githubusercontent.com/LBNL-ETA/Radiance/master/src/common/color.c):
successive count bytes contribute little-endian base-256 digits, including
zero low-order digits, and a literal resets the digit shift. Checked shifts,
row bounds and packet reads reject oversized, over-shifted and truncated
chains. New-RLE zero-length packets remain invalid. Regressions cover 256
and 257 repeats, a literal reset, zero before a literal and malformed chains.

At `ca6b63a84`, all 15 selected executables passed, including 188 image
checks and 480 exact corpus matches. Six ASan/UBSan suites passed with
container checks enabled. Clean make, Xcode Deployment/Opto and Blender LTO
builds were warning-free (excluding the expected AppIntents notice). CLI
write-failure and independent TGA checks also passed. Three further trials
of eight concurrent image-suite copies passed and removed their scratch
roots. The earlier immutable-archive run failed eight new valid-format
assertions normally; the malformed-chain guards remained green. Sparse-file
skips and excluded native Windows/Android verification remain unchanged.


### Third-pass cache safety and compatibility follow-up

The follow-up review of `3c37cb0f7..cfb685dc9` found one new P1: the
byte-exact thumbnail-cache repair followed a planted symlink and overwrote
its target. The cache now validates the opened object before changing any
bytes. POSIX uses an owned private `rise_entity_templates_<uid>` directory,
directory-relative no-follow opens, effective-user ownership checks,
single-link regular files and exclusive writer locks with validation again
under the lock. The legacy shared cache name is ignored. Windows uses
opened-handle reparse/owner/link checks and requires Advapi32; native
Windows execution remains excluded. The PNG bytes remain unchanged.
A sibling audit also found that permissive umasks exposed the predictable
staging leaf. POSIX staging directories now use `mkdir(..., 0700)` in the
creation syscall, and staging files use exclusive no-follow creation.
Windows staging uses exclusive creation; native DACL verification remains
part of the excluded Windows gate. Direct user-authored output opens retain
their prior symlink semantics. A permissive-umask regression checks both
staging privacy and the requested permissions of newly published outputs.
Regression victims behind symbolic and hard links stay unchanged, and a
shared-parent multi-process test verifies complete PNG consumption while
repairing a partial cache. Automatic cache paths are protected separately
from user-authored output paths, where following an explicit symlink is
intentional. Consumers still receive a pathname; same-user changes after
return are outside the cache write operation.

| Follow-up finding | Disposition |
| --- | --- |
| Existing image inode, hardlinks, owner, ACLs and xattrs changed by rename | Existing `AllowInPlace` image destinations receive completed staged bytes in place. This preserves their identity and metadata, and handles writable file bind mounts without a rename. New image paths use rename. Required photon saves deliberately retain atomic replacement, including its inode/metadata consequences. |
| Failed in-place copy removed both versions | A failed post-encode publication retains the complete staged payload for recovery, reports its path and never reruns the encoder. The destination can be partial. Direct encoding after denied staging creation/open has no completed stage to retain. |
| Write failures lacked causes; cleanup could leave a directory | Additive diagnostics and logs identify operation, native cause, recovery path and residual cleanup path. Removing a staging directory cannot be guaranteed after the parent loses write permission; the residual path/error is reported. |
| Large TGA output rejected after CLI truncation | The reported >4 GiB CLI truncation case was already blocked by the generic output budget. Shared TGA validation now runs before source-raster allocation and output opening. The writer retains its checked `UINT_MAX` total-buffer budget, in addition to 16-bit axis limits; the CLI's generic raster budget is tighter. |
| Truncated EXR final chunk returned success with zero pixels | The stream wrapper latches failed reads/seeks even when the SDK catches its exception internally. Both direct FLOAT and SDK chroma paths reject the decode and retain no active image. |
| PNG/JPEG reload required the caller to rewind | Both save and restore their construction cursor. JPEG reads the remaining source span. Tests no longer seek before repeats/recovery and include embedded PNG/JPEG data. Their concrete layouts changed; clients must rebuild. |
| TIFF unassociated-alpha precision | Retain the libtiff 8-bit associated-RGBA conversion and subsequent unassociation. This is a display-oriented decode, not a lossless TIFF round trip: e.g. channel 200 at alpha 128 can return 199, with greater loss at very low alpha. The existing independent TIFF checks use a quantization tolerance. |
| HDR XYZE and scanline-leading repeat markers | Keep diagnosed rejection. XYZE needs a distinct XYZ-to-RGB conversion; interpreting it as RGBE gives wrong colors. A leading repeat has no preceding pixel in that scanline and previously accessed before the row/buffer. These stricter rejections are explicit compatibility changes. |
| Unused TGA color maps | Truecolor images can carry unused 15/16/24/32-bit map entries; skip their bounded storage after the ID and verify pixel bytes. Truncated maps remain errors. Indexed-color decoding remains unsupported. |
| Malformed 3DS child inside a bounded object | Skip the bad object to its known parent end, preserving later valid sibling objects as well as the prefix. Consumed mesh records remain strict. |
| Malformed prefix or short MAIN length before EDIT3DS | Keep bounded container traversal and reject. The loader does not scan arbitrary payload bytes for a coincidental section marker. This intentionally differs from the historical bytewise recovery. |
| Image test depended on launch directory | A target-specific absolute source-root definition locates the shipped JPEG fixture. Validation includes launch from an unrelated scratch directory without `RISE_MEDIA_PATH`. |
| Plan called new parser rejections existing behavior | Wording now identifies comma/missing-amplitude `cp` and lowercase-axis checks as new strictness. |

Validation through `01cb6cc2c` passed all 15 selected executables, including
155 buffer/output checks, 214 image-reader checks, 456 template checks,
192 source-hygiene checks and 480 exact scene-corpus matches. Clean make,
Xcode Deployment/Opto and Blender native LTO builds passed without compiler
warnings, apart from the expected Xcode AppIntents notice. Six targeted
ASan/UBSan suites passed with container checks enabled. Four CLI TIFF/EXR
write-failure paths and independent TGA boundary/orientation checks passed.
A 65,535-by-65,535 TGA request was rejected before opening its writer and
left the existing destination unchanged. The canonical image test passed
from an unrelated directory without `RISE_MEDIA_PATH`; three trials of
eight concurrent copies also passed all 214 checks and removed their roots.

The thumbnail sentinel and existing-hardlink regressions failed normally
against the earlier library and passed after the fixes. An isolated reader
RED run failed 15 assertions normally with the earlier EXR/PNG/JPEG code;
its matching corrected build passed from scratch. Both new permissive-umask
privacy guards failed normally against the preceding staged writer and
passed with private creation. The cache concurrency regression covers eight
processes across eight rounds, with 32 ensure-and-consume operations per
process. The foreign-owner fixture is privilege-gated and was skipped on
this unprivileged host; no claim of a native ownership-change test is made.
Sparse multi-GiB fixtures and native Windows/Android execution remain
excluded as previously recorded. Windows project/test linker lists include
Advapi32; the tool project XML was parsed locally.


### Fresh review of the third-pass changes

The first independent round on `f33987880` found three P1s and three P2s.
The filesystem findings were directory substitution through nonsticky
writable parents and inherited macOS ACL grants overriding mode-bit privacy.
The third P1 was a stale editor-only Voronoi border requirement and comment,
which still rejected zero-argument nodes despite the parser accepting the
default. The P2s covered relative temporary roots, EXR embedded-stream
coordinates and stb's acceptance of physically truncated JPEG tails.

The POSIX staging policy now checks canonical parent ancestry before any
automatic directory is created: owners must be the effective user or root;
foreign-writable directories must be sticky. On macOS it conservatively
rejects extended ALLOW ACL entries, including benign owner-only grants,
while retaining deny-only ACLs. This follows the [ACL model in Apple's filesystem documentation](https://developer.apple.com/library/archive/documentation/FileManagement/Conceptual/FileSystemProgrammingGuide/FileSystemDetails/FileSystemDetails.html):
mode bits alone do not describe all access. No caller ACL is rewritten.
`Required` fails with a diagnosed cause before invoking the encoder in an
unsafe location. `AllowInPlace` opens the explicit user-authored destination
and directly encodes once. This preserves legacy image writes there but
has no completed stage for recovery. Required photon saves need trusted
ancestry; a private child under an unsafe nonsticky ancestor is insufficient.
Native Windows parent/DACL enforcement remains outside the excluded native
Windows gate. The policy assumes trusted owners do not deliberately remove
the security boundary during an operation.

Cache roots use the same ancestry boundary and are canonical absolute paths.
Unsafe roots are refused; existing cache directories and files also reject
macOS ALLOW ACLs before repair. Deny-only ACLs and trusted sticky temp roots
remain usable. Tests preserve and restore fixture ACLs, verify no automatic
cache/stage is created under unsafe roots/ancestors, and check real inherited
everyone grants on this macOS host. Relative `TMPDIR` results remain usable
after changing directory. The obsolete Voronoi requirements and related GUI
notes are removed; both 2D/3D nodes are built, derived and looked up without
caller arguments.

EXR input/output wrappers now have explicit stream origins; default origin
zero preserves direct-wrapper callers. Readers use their saved construction
cursor in both FLOAT and chroma paths. Writers establish blob-relative
coordinates for each `BeginWrite`. Embedded reader reloads cover RGB and
YC/YCA, and independent SDK checks decode isolated HALF/FLOAT writer blobs.

JPEG preflight requires bounded segment/entropy-marker framing from SOI
through EOI before calling stb. It handles progressive scans, restart/fill
markers, stuffed bytes, DNL and trailing data. Physical truncation now fails
with cleared state and healthy recovery. Independent tiny progressive and
restart-coded fixtures compare decoded pixels. This is not a second JPEG
entropy decoder: stb's best-effort interpretation of damaged entropy inside
a structurally closed stream remains a deliberate compatibility choice.

The `b351e8b9b` gate passed all 15 selected executables, with 165
buffer/output, 234 image and 482 template checks, 192 source-hygiene
checks and 480 exact corpus matches. Clean make, Xcode Deployment/Opto and
Blender native LTO builds were warning-free except the expected AppIntents
notice. Six ASan/UBSan suites passed with container checks enabled, plus the
four CLI finalization-failure probes and independent TGA checks. The image
binary also passed from an unrelated directory without a media-path variable,
and 24 concurrent copies passed with complete scratch cleanup.

Source-isolated RED runs against the preceding implementations failed
normally: 10 template assertions (ancestry/ACL/absolute-path/Voronoi), six
staging-parent/ACL assertions and eight EXR-prefix/JPEG-tail assertions.
Corrected runs pass. The positive independent EXR writer, progressive JPEG
and restart-marker checks preserve valid-format coverage; they are not
claimed as individual RED regressions. macOS inherited ALLOW and deny-only
ACL fixtures were executed on this host and restored afterward. The earlier
sparse-file and privileged foreign-owner skips, and native Windows/Android
verification exclusions, remain explicit limitations.


### Publication file-type availability follow-up

Two reviewers in the next round reported no P1/P2 findings. A remaining
availability check identified blocking opens after an earlier regular-file
path check: a destination changed to a FIFO could block the write probe or
completed-stage copying. The helper now opens destinations nonblocking on
POSIX without truncation, validates the actual opened descriptor as regular,
then truncates that same descriptor. Probes validate their opened descriptor
too. A private open policy keeps the public constructor's historical direct
stream/device semantics unchanged. The Windows branch uses its corresponding
64-bit descriptor status/truncation APIs; native execution remains excluded.

The bounded child regression changes a destination to a FIFO during encoding,
with and without a reader. It observes a prompt diagnosed failure, one encoder
invocation, unchanged FIFO identity/no written bytes and complete retained
staging output. A child watchdog converts the preceding implementation's
blocking behavior into an ordinary parent test failure. Both new assertions
fail against the preceding library; the corrected targeted buffer suite passes
169 checks. An independent control verifies public direct FIFO writes still
work. No syscall-interposition harness is needed for this regression.

The final availability gate passed all 15 selected executables (169 buffer,
234 image, 482 template checks; 480 exact corpus matches), six targeted
ASan/UBSan suites with container checks, and clean make, Xcode
Deployment/Opto and Blender LTO builds without compiler warnings except the
expected AppIntents notice. CLI TIFF/EXR finalization failures and independent
TGA checks passed again. The previous cache/decoder concurrency and
unrelated-working-directory checks remain recorded above; those sources
are unchanged by this availability fix. Windows 64-bit CRT type spelling was
checked against the official API reference; native execution remains pending.


### EXR associated-alpha closure

The next fresh round returned two clean reviews and one confirmed codec P2:
EXRReader returned associated RGB as straight `RISEColor`, so subsequent
RGBA EXR writing multiplied alpha again and PNG/TGA conversion darkened
translucent colors. The reader now divides RGB by any nonzero alpha in
double precision before the selected color-space conversion. Missing alpha
stays one. Zero-alpha RGB is left undivided and retained through the selected
color conversion, preserving prior additive/hidden-data reads. Existing
straight-alpha writers still cannot round-trip additive zero-alpha EXR data;
EXR writing premultiplies it to zero as before. RGB-only EXR output also
retains its existing composite-over-black behavior rather than changing the
meaning of the no-alpha option.

Independent SDK-authored HALF/FLOAT fixtures cover fractional, negative,
above-one and tiny alpha, a straight quotient above FLOAT range, missing
alpha and nonlinear color-space selection. YA/YCA expectations derive from
SDK associated values divided by alpha. Fixed quantized PNG/TGA pixels and
independent SDK decoding of subsequent EXR writes prove the conversion
path; separate SDK oracles retain RGB-only flattening. A HALF fixture setup
mismatch was caught before validation and corrected to use HALF-backed
framebuffer slices. The new regression then fails normally against the
previous reader (eight failed checks), and the corrected image suite passes
250 checks.

The associated-alpha gate passed all 15 selected executables, six targeted
ASan/UBSan suites with container checks, and clean make, Xcode
Deployment/Opto and Blender native LTO builds without compiler warnings
apart from the expected AppIntents notice. The 480 corpus hashes still
match; no golden refresh was needed. The image binary passed all 250 checks
from an unrelated directory without `RISE_MEDIA_PATH`. To verify this FP
change under shipping optimization, the same 250-check executable was
linked with the actual Opto EXRReader/EXRWriter objects (confirmed full LTO,
fast-math and disabled finite-math-only) and the immutable gate archive;
all checks passed with an explicitly full-LTO final link. Other library
objects in that focused executable came from the make gate archive. CLI
finalization-failure and independent TGA checks also remain green. Prior
platform/privilege/sparse-fixture exclusions remain unchanged.


### Normalized integer output and PNG transfer metadata

The following fresh round found no P1 and two confirmed writer-boundary P2s.
`Integerize` clipped RGB but cast alpha without normalization, so preserved
EXR alpha outside `[0,1]` could wrap or invoke undefined floating-to-integer
conversion. Normalized integer export now saturates each converted RGBA
channel before casting: NaN/negative infinity/nonpositive values become zero,
positive infinity/values at least one become the requested maximum. Finite
interior values retain legacy truncation. The requested maximum must remain
finite, nonnegative and representable in the destination type. Floating
EXR data is unchanged; the separate raw scaling helper retains its
representable-products precondition and existing behavior.

PNG writing previously labeled every 16-bit output linear even when samples
used sRGB or ProPhoto encoding. Both bit depths now use the selected space's
existing approximate `gAMA` convention: one for linear709/linearROMM, 1/2.2
for sRGB, and 1/1.8 for ProPhoto. Eight-bit metadata is unchanged. This is the
legacy gamma approximation, not complete ICC/chromaticity or exact piecewise
transfer profiling. Independent libpng decoding checks actual raw samples,
network byte order and quantized gamma for both bit depths/all four spaces,
without reader gamma transforms or the RISE reader.

Runtime nonfinite inputs follow the existing `FiniteMathTest` convention;
they are test data, not forbidden control-flow sentinels. Direct 8/16-bit
oracles and EXR-to-PNG/TGA conversions cover clipped alpha and finite RGB.
A shadow header tree keeps old template definitions consistent with the old
archive for RED proof: eight image assertions and two nonlinear 16-bit gamma
assertions fail normally against the preceding code. The corrected targeted
suites pass 272 image and 180 frame-encoder checks.

`AllowInPlace` retains unsynchronized in-place semantics. Callers must
serialize writes to the same destination, including hardlink aliases; the
helper does not promise atomic visibility or multi-writer isolation there.
Required whole-file rename publication retains last-successful-rename wins.
The automatic thumbnail cache has its own cooperative writer lock because
its shared name is managed by the library rather than selected by callers.


A sibling audit found the same unchecked casts in RGBEA alpha and the two
shared-exponent writers. RGBEA alpha now uses the normalized conversion
independently of RGB, so opaque black retains its opacity. After selected
color-space conversion (and HDR compositing), HDR/RGBEA clamp channels to
their representable nonnegative range before packing: NaN/negative values
become zero; positive infinity and larger finite values saturate. For an
N-bit mantissa the ceiling is `(2^N - 1) * 2^(127 - N)`, with N=8 for HDR
and N=16 for RGBEA. This derives from the format's largest stored exponent,
255, with bias 128. It prevents undefined casts and exponent wrapping.
The existing `<1e-32` zero floor remains. These are bounded shared-exponent
formats, unlike floating EXR. Mantissa scaling uses the exact power of two
described below, including the exact upper endpoint.
Twenty-nine independent writer cases check raw bytes plus wire-only decode
bounds, including ROMM conversion and alpha independence. Before the final
HDR scaling correction below, 15 of these assertions failed normally
(exit 1) against the preceding immutable library; the ordinary finite
fixtures matched at that checkpoint. The earlier normalized-integer
and gamma RED runs above used matching old template headers; this focused
shared-exponent RED run keeps the already-corrected normalized policy and
uses the preceding immutable library for compiled writer implementations.
The preliminary bounded-packing gate passed 301 image checks and 180
frame-encoder checks. Eighteen selected executables, four CLI finalization-failure
probes, and clean make plus Xcode Deployment/Opto builds pass. Runtime
codec verification links the actual Opto EXR reader/writer and PNG, TGA,
PPM, HDR and RGBEA writer objects with full LTO; 301 image and 180 frame
checks pass with their fast-math settings. Other linked library objects use
the canonical make archive; this is not a full GUI runtime test.

### HDR wire-format closure

The final sibling audit confirmed a legacy scaling defect. Multiplying the
`frexp` fraction by 255.9999 can produce an RGBE peak mantissa of 127. A
width-eight raw row beginning with color `(.02,.02,1)` then starts with
`{2,2,127,129}`, which Radiance readers interpret as an RLE header instead
of pixel data. Shared-exponent scaling now uses the exact binary factor
`2^(N - e)` for both formats. RGBE's nonzero peak is at least 128, avoiding
that ambiguous prefix; it also fixes the former power-of-two dimming
(`1.0` encodes as 128 at exponent 129, rather than 127). Finite HDR bytes can
therefore change. RGBEA uses the same exact formula without the redundant
division. Mantissas still truncate, and the existing zero floor remains.
The added width-eight regression checks raw `{2,2,128,129}` bytes and decoded
pixels independently; it is separate from the nonfinite/overflow guards.
Against the preceding bounded-packing library, the final test fails five
assertions normally (exit 1), including that width-eight decode and four
updated exact-scale byte expectations.

After the exact-scale correction, clean make and Xcode Deployment/Opto
builds pass without compiler warnings, apart from the expected AppIntents
notice. The five affected codec/finite-math/include-order executables pass,
including 302 image and 180 frame checks; both codec suites pass ASan/UBSan.
Their actual-Opto-object reruns pass the same 302/180 checks under full LTO
and fast-math. The Blender native clean LTO link also passes. The broader
18-executable/six-sanitizer gate immediately before this final formula change
remains a separate checkpoint, not a claim that every suite was repeated.
Native Windows/Android and the previously disclosed privileged ownership
and sparse-file exclusions remain outside the executed host verification.

### Connected ancestry and PNG failure closure

The fresh round on `bc0cbb971` found two further P1s. The staging ancestry
walk validated full paths from leaf to root, so directory substitution could
make separate checks observe different trees. It now opens `/`, validates
it, and descends with `openat`/`O_NOFOLLOW`, checking each child while holding
its already-trusted parent descriptor. An unsafe component is refused before
any lower component or automatic stage is opened. The established trust
policy still assumes trusted owners do not deliberately remove a boundary.
The added static unsafe-ancestor/sticky-descendant/private-leaf test checks
policy composition; it also passes the former implementation and is not
claimed as a live cross-user race reproduction. No concurrent directory-swap
harness or privileged foreign-owner fixture was executed for this fix.

Ordinary PNG and HDR10 PNG encoders previously swallowed libpng's `longjmp`
error and could return after writing a partial header. Both now clean codec
state and throw on encoding or structure-allocation failure; ordinary PNG
also throws on invalid dimensions or pixel allocation failure. Write
callbacks check the sink result and bound lengths, converting sink
exceptions to libpng errors only after leaving the catch scope. Agent-side
PNG helpers now own references with RAII and return their existing empty
failure result on an encoding exception. The separate perception libpng
path already discards its partial bytes and marks output unavailable.

A thread-local test hook triggers libpng's real error/`longjmp` path just
after successful header emission. It models an internal codec failure; it
is not a claim of reproducing allocator exhaustion. Tests use sinks whose
writes all succeed, require direct encoding to throw, require SaveTo/SaveAs
to fail while preserving existing staged destinations, and check healthy
retry. Agent tests cover both ordinary output helpers and the composite
passthrough helper. The default-thumbnail initializer retains its outer
exception containment and completion check.
A controlled mutation in isolated copies restores only the two swallowed-error
returns while keeping the fault hook. It fails nine assertions normally
(exit 1), including both staged-destination preservation guards. This is a
handler mutation check, not a claim that the unmodified old binary contains
the new hook.

The caller sweep also rejects bound empty viewport frames before opening or
encoding, covering `0x0`, `0xH` and `Wx0`, and verifies a healthy rebind.
Existing no-byte checks already refused an empty encoded result; the new
preflight avoids a direct fallback truncating a destination before that
late rejection and prevents a custom encoder from accepting an empty store.
Null writer factories now throw even if they wrote bytes before returning
null. Both disabled-PNG writer branches also throw after their diagnostic.
Against the preceding immutable library, the null-factory suite fails three
assertions and the viewport suite fails six, both with normal exit 1.
A separately compiled `NO_PNG_SUPPORT` probe fails both encoder checks before
the correction and passes both after it; the normal configured suite does
not exercise that compile-time variant.

The same round proposed TGA 2.0 associated-alpha support as a P2. That finding
was withdrawn: the published reader contract explicitly supports straight
alpha and excludes extended TGA 2.0 metadata interpretation. Tolerating an
unsupported extension does not change that declared subset. No new TGA 2.0
feature is included in this follow-up.

The broader functional checkpoint passed 19 executable suites and seven
targeted ASan/UBSan suites, including 171 buffer checks and 380 agent checks.
The final size-type correction was followed by clean make and Xcode
Deployment/Opto builds with zero compiler warnings (excluding the expected
AppIntents tooling notice), plus the Blender native LTO relink. The final
four focused programs pass 302 image, 199 frame-encoder, 104 viewport and
eight agent PNG checks; the frame/viewport sanitizer reruns pass too.
Six CLI TIFF/EXR/PNG write-failure paths return normal exit 1. Actual Opto
codec, frame-encoder, viewport and agent-helper objects, built with full LTO
and fast-math, pass the same four focused programs when linked with the
canonical make archive. This does not claim a full GUI runtime test or a
repeat of all 19 suites after the final size-type/preflight changes.
The live-race, privileged-owner, sparse-file and native Windows/Android
verification limitations recorded above remain unchanged.

### PPM bounds and exclusive emergency recovery

The fresh round on `2f2dcfa28` found two P1s: PPM pixel allocation/indexing
still used unchecked unsigned products and unguarded coordinates, and the
observer's generated `fro_temp_...` emergency name inherited explicit-output
symlink semantics. PPM now checks nonzero dimensions and the complete
`UINT_MAX` header/payload budget before output mutation, owns its allocation
before writing the header, clears replacement state, guards coordinates and
uses `size_t` indices. Header/final write failures throw. Fixed P6 oracles
preserve the historical straight RGB bytes: PPM still drops alpha rather than
compositing it. The former ignored-return alpha-helper call is removed.

The same helper is actively used by the job's 16-bit premultiplied callback.
It divided instead of multiplying, could divide by zero or overflow signed
products, and left returned alpha uninitialized. It now uses exact wide
integral products/division for normalized integer channels, preserves alpha,
and combines bounded mantissas with exponent scaling for floating HDR
channels, avoiding both premature underflow and overflow. Its input
preconditions and positive normalization maximum are explicit. Direct 8/16-bit
and floating oracles cover zero/fractional/maximum alpha, large integer
samples and runtime large floating inputs. The 32-bit float callback now
multiplies source-double RGB and alpha before narrowing once to float, so a
representable product survives source values outside binary32 range.

Automatic emergency files now use exclusive no-follow creation under trusted
parents, requesting POSIX 0600 permissions reduced by umask. Existing symbolic
links, hardlinks and ordinary files are never followed, reused or overwritten.
A collision selects a monotonic suffix before the extension, with at most
16 exclusive attempts; only `EEXIST` retries, before encoding begins. Successful
recovery reports its actual absolute path. An existing completed primary
stage is reported directly without replaying the encoder. If no complete
stage exists, the historical emergency encode attempt remains available.
The observer also rejects bound zero dimensions before touching output.
Sentinel tests require both unchanged planted names and a separate complete
recovery image, and cover repeated emergency renders and retained-stage reuse.
Explicit user-authored output symlink semantics remain unchanged.

The `IFrameEncoder` comment now permits encoding exceptions and requires
callers to contain them and check destination-specific failure reporting;
failed direct encoding can leave partial bytes. No interface vtable or
publicly constructible layout change was needed for these follow-ups.

The active callback consumer had the same unchecked pixel product. Callback
exports now reject zero dimensions or pixel counts above `UINT_MAX`, check
the selected format's byte product against `size_t`, and bound regions before
indexing. The pixel limit preserves the legacy integer-addressable domain;
there is no additional 2 GiB callback ceiling. Only the selected 16-bit or
32-bit storage is allocated. Valid exports read current receiver policy,
including same-size mode changes, and support resizing/recovery.

Parallel render workers can call the adapter concurrently. Its private
cache, conversion and receiver delivery now share one per-adapter mutex;
separate adapters remain independent. The public receiver contract states
worker-thread delivery, borrowed-buffer lifetime, prompt return and the
existing no-reentry/lock-order requirements. The order is caller-held tile
locks, then adapter, then receiver; receiver-side locks must not lead back
into the current FrameStore/render/job or wait for render completion.
The copied adapter gets an independent mutex and empty cache. This mutex
changes only the cpp-local implementation type, not a public interface or
publicly constructible layout.

A cpp-only factory delegate constructs the same adapter used by Job for tests
without a render, extra header API or `const_cast`. Fixed 16/32F pixel oracles,
resize/mode/region tests and bounded three-worker delivery checks cover the
actual dispatcher. Invalid-image canaries throw before any old zero-array
write, and old-unsafe large float allocations/concurrency tests are gated
behind the new preflight proof. The focused callback mode contains 31 checks.

At the PPM/recovery checkpoint, all 19 normal executables and eight targeted
ASan/UBSan suites pass (325 image and 77 output-shim checks). Isolating the
old PPM and observer implementations while retaining current headers and
other compiled code fails eight and nine assertions respectively, with
normal exit 1. Those source-isolated runs avoid incompatible template
headers and unsafe old out-of-allocation writes.

The callback checkpoint passes 356 image checks, its 31-check focused mode
under ThreadSanitizer (Job and the test instrumented), and the Job-instrumented
image/CST ASan/UBSan runs. Isolating the old callback body with only its test
construction delegate added fails six assertions normally; the test skips
old unsafe large float allocations and parallel races after a failed canary.
Actual Opto Job and codec/output objects also pass the five focused programs.
This is a targeted callback concurrency check, not a full-render TSan claim.

Four additional runtime bit oracles cover floating premultiplication range.
The original ratio-first candidate loses tiny alpha in strict arithmetic;
fast-math reassociation can instead lose opaque subnormal RGB. Bounded
mantissas plus exponent restoration preserve the final representable result.
A standalone strict-build check of the actual header passes the huge/tiny
and opaque minimum-normal/subnormal cases without relying on fast-math
reassociation. The full image suite now contains 360 checks.

The resulting shared-header state passes clean make, Xcode Deployment/Opto
and Blender native LTO builds with zero compiler warnings, excluding the
expected AppIntents tooling notice. Six selected programs pass: 360 image,
199 frame-encoder, 104 viewport, 77 output-shim, eight focused agent PNG and
172 CST checks. The image/CST ASan/UBSan reruns include instrumented Job
code; the current 31-check callback mode passes targeted ThreadSanitizer.
Actual Opto codec, output, observer, disk and Job objects pass the five
focused image/frame/viewport/shim/agent programs under full LTO and fast-math,
linked with the canonical make archive. These are focused runtime checks,
not a full GUI runtime or full-render TSan verification. The broader
19-program/eight-sanitizer checkpoint and eight CLI failure paths above are
not claimed to have been repeated after the final callback/range changes.
Native Windows/Android, privileged owner changes, live cross-user directory
substitution and sparse-file exclusions remain as previously recorded.

### Downstream callback and writer follow-up

The next fresh round found four P2s downstream of the previous fixes: 32F
association narrowed operands too early; Blender still used unsigned RGBA
allocation/index products; callback exceptions bypassed manual tile unlocks;
and RGBEA silently ignored failed writes. The writer-pattern sweep also found
the same failure reporting in HDR. These findings are fixed together with a
P3 cleanup gap after exclusive recovery creation but failed stream adoption.

32F callback association now uses the original double operands before one
float conversion. Runtime bit oracles cover both huge RGB with fractional
alpha and tiny alpha whose final RGB product remains representable. HDR and
RGBEA validate complete header-plus-payload sizes, allocate before output,
throw on header/payload failures, and deactivate failed writes; valid wire
encodings remain unchanged. Automatic recovery cleans only its own newly
created leaf if FILE stream adoption fails, preserving colliding names.

Still and animation block writes now own their overlapping tile locks through
an exception-safe scope. Explicit publication unlocks every acquired tile
before rethrowing the first notification failure; cleanup during unwinding
contains notification failures and preserves the original exception. Bulk
writes and tile-copy ingest use the same ownership guard. Partial acquisition
releases its acquired prefix. Existing identity gates, pixel iteration and
successful publication order are preserved. The internal concrete
FrameStoreBulkBracket layout changed; clients including that implementation
header must rebuild. Public interface vtables and the C construction API are
unchanged.

Blender uses checked `size_t` storage, preview offsets and result-copy byte
counts, bounded by native byte and container limits. Its dimension-only ABI
adds no arbitrary 32-bit scalar ceiling. Allocation-free boundary oracles
cover 32768 squared (2^32 RGBA scalars) and offsets beyond `UINT_MAX`; tiny
fixtures check both receiver formats and packed previews. Preview NaN and
negative infinity map to zero, positive infinity to maximum, while final
float samples retain their bits. The C render entry point contains failures,
cleans partial results and releases the job before stack callbacks disappear.

The next exception-lifetime sweep separated authored sampler ownership from
per-pass execution state. Scoped sampling overrides live between joined
worker phases; configuration getters keep reporting the authored kernel.
Pel and shared NM/HWSS integration paths, BDPT/VCM normalization hooks and
training paths use the active kernel. No temporary sampler `const_cast` is
needed. Full still/animation drivers scope image references through the
virtual release hook, temporary sequences/samplers/images, progressive film
pointers and progress state. Failures preserve their original exception and
permit a healthy retry. Concurrent renders on one rasterizer remain
unsupported. PixelBasedRasterizerHelper's concrete layout changed; clients
constructing/subclassing it must rebuild. Custom `IntegratePixel` implementations
should query protected `ActiveSampling()` for per-pass execution; `pSampling`
now consistently denotes authored configuration. Public signatures/vtables
remain unchanged. The retired RGB spectral rasterizer is excluded from the
supported builds and still uses its obsolete sampling API; it is not an active
consumer of this execution-state path.

Tile publication now unlocks the whole acquired rectangle before its first
observer notification. A synchronous full-frame observer read cannot wait
on another tile held by that same bracket. Publication order and notification
exception aggregation remain defined, and a bounded independent readback
fixture checks full-frame pixels, generations and bracket reuse. Observer
comments now describe shared tile locks and synchronous delivery rather than
the retired seqlock model.

The render-lifetime checkpoint passes clean make, Xcode Deployment/Opto and native
Blender LTO builds with zero compiler warnings (apart from the previously
excluded AppIntents tooling notice). Thirteen selected test executables pass;
Blender's full suite has 41 checks and FrameStore has 220, including its
97-case focused exception mode. PT/BDPT/VCM RGB and spectral animation parity
pass at default cadence and with requested uneven batches: 12 selected runs.
VCM merging retains its existing one-sample iteration policy; its VC-only
case exercises the requested 3,3,2 batches.

Five targeted ASan/UBSan suites pass, plus a separate four-check actual Blender
C export callback-failure/retry probe. Targeted TSan passes 33 callback checks
and all 97 exception/readback checks with Job, FrameStore, the helper, the
thread pool and test instrumented. An initial partial run left the pool's
atomic latch uninstrumented and reported closure reuse; instrumenting that
synchronization dependency removed the report without suppressions or a
production code change. This is focused TSan coverage, not a full-render claim.

Actual Opto objects under full LTO and fast-math pass seven focused programs
and all 12 selected animation parity runs. The Blender Opto program covers
buffer arithmetic/previews; the later C export fault probe is verified
separately in normal and sanitizer builds. The C-export test-only addition and
stale lock-comment corrections pass an additional make/test warning check.

Source-isolated old implementations fail normally with two float-range,
six HDR/RGBEA write-error and 18 tile-cleanup assertions. Disabling only
recovery adoption cleanup fails four leaked-leaf assertions. For the render-lifetime
step, the old helper fails 44 image-reference, sampler-state and
retry assertions; the old publication guard fails three bounded synchronous
readback assertions. These proofs retain current headers and other library
objects, and do not count a process crash or harness timeout as success.
Native Windows/Android and the previously disclosed privileged/filesystem
exclusions remain unverified.

### Splat-task exception cleanup

The fresh round on `0bcd191cb` returned clean export and filesystem/fidelity
reviews, but identified one P1 in the renderer. A post-pixel exception could
bypass trailing TLS splat flushing/unbinding. Persistent worker storage then
retained pending records and a pointer to a film that training cleanup or a
retry could destroy. The next binding could access freed storage or merge
old records into a newly allocated film at the same address.

Caller-thread passes and still/animation dispatch workers now scope their
TLS binding. Exceptional exit discards pending records and forgets the film
without allocation, locking or film access. Existing successful flushes and
batching remain; flush failure also unbinds before rethrowing. The sibling
sweep covers the specialized BDPT caller path and both MLT task variants;
Pel and shared NM/HWSS consumers use those same boundaries. The buffer layout
and per-splat body are unchanged. Retained vector capacity is implementation-
dependent; 65,536 is an auto-flush record threshold, not a fixed byte budget.
Already committed partial splats are not rolled back after an aborted pass.

The regression emits real pending splats before a post-pixel observer throws.
It checks null binding and zero pending records on the same caller/worker
thread, destroys the failed 4x4 film, replaces it with a 5x5 film, and verifies
exact healthy retry sums and normal flushes. Cases cover full caller still
and animation entry points, actual still/animation dispatcher tasks on a
persistent local pool worker, and specialized BDPT single/progressive paths.
Unsafe old retry is gated by the cleanup canary and safely discarded in the
counterfactual tests. Focused FrameStore coverage is now 122 checks.

The splat-task checkpoint passes clean make, Xcode Deployment/Opto and native
Blender builds without compiler warnings, thirteen selected executables and
fourteen serial animation parity runs including MLT RGB/spectral. Five
targeted ASan/UBSan suites pass; FrameStore has 245 checks. Focused TSan passes
33 callback and 122 exception/splat checks with the pool, TLS buffer and splat
film synchronization dependencies instrumented. Thirty actual Opto objects
pass seven focused programs and all fourteen parity runs under full LTO and
fast-math. Disabling only task-scope destructor cleanup in an isolated copy
fails eighteen expected same-thread cleanup/discard/retry assertions normally,
with no source mutation or unsafe film dereference.

### Dense film storage and footprint validation

The direct-consumer sweep found unchecked `w*h` allocation products in
SplatFilm, FilteredFilm and ProgressiveFilm, plus a native-size product in
Channel. Oversized dimensions could wrap to a small allocation; later accesses
or accumulation then used the original dimensions. SplatFilm and FilteredFilm
also held raw row-mutex pointers that leaked a constructed prefix if a later
allocation threw. FilteredFilm could retain its row lock when a virtual kernel
evaluation threw.

These dense containers share header-only `Utilities/CheckedSize.h`: multiplication
is checked against native bytes and vector capacity before allocation. Bulk
operations on empty extents remain no-ops; direct pixel access still requires
valid coordinates. SplatFilm additionally respects its 32-bit batch-record
index domain (up to 2^32 pixels on capable native targets); the other containers
use native-sized storage/index arithmetic without an arbitrary byte cap.
Progressive aggregate counters were already 64-bit. These are representability
checks, not a memory quota; representable allocations can still fail normally.

Row mutexes now use `unique_ptr`, and scoped row locks release during unwinding.
SplatFilm and FilteredFilm are explicitly noncopyable. Their concrete headers
and protected ownership details changed, so clients/subclasses must rebuild
and update direct protected-field uses. Public construction interfaces and
virtual signatures remain unchanged. Manual SplatFilm clients must finish
producer work and flush/unbind (or use the task scope) before film teardown;
Resolve only reads committed pixels. `FlushCallingThreadBuffer` commits this
thread's pending contributions while retaining its binding;
`FlushCallingThreadSplatBuffer` commits and unbinds. Manual clients must flush
all producer tasks before resolving.

Both footprint paths intersect floored Scalar bounds with the valid pixel
range before unsigned conversion. They ignore invalid/nonfinite support and
nonfinite positions, preserve FilteredFilm's overlapping off-image footprints,
and preserve SplatFilm's existing home-pixel membership rule. No tolerance was
widened. Independent small-image oracles cover ordinary and DBL_MAX support,
nonfinite/negative inputs, throwing kernel evaluation, unlocked rows and retry.
Pure size validators cover huge boundaries without backing allocations; actual
old-wrap constructor canaries allocate at most small wrapped storage in the
counterfactual. The focused storage/footprint mode contains 38 checks.

The storage checkpoint passes clean make, Xcode Deployment/Opto and native
Blender builds with zero compiler warnings, thirteen selected executables,
and fourteen serial default/batched animation parity runs. FrameStore now
passes 283 checks. Five targeted ASan/UBSan suites pass. Focused TSan passes
33 callback, 122 exception/splat and 38 storage/footprint checks. Thirty-one
actual Opto objects pass seven focused programs and all fourteen parity runs
under full LTO and fast-math. Source hygiene confirms the new header across
the five authoritative build projects; no `.cpp` source was added.

A detached-worktree counterfactual rebuilt the whole library and test with
consistent headers, unchecked constructor products and the former manual
FilteredFilm row lock. It exits normally with four direct failures: three
actual constructor guards and row-lock release after a throwing evaluation.
Twenty-three additional failures are dependent gated checks, not independent
footprint negatives. FilteredFilm's huge-count check remains a pure validator;
no giant backing allocation or unsafe old footprint conversion was run.
Partial-construction cleanup follows standard `unique_ptr` ownership; forced
mutex-allocation failure was not injected. Native Windows/Android and the
previously recorded filesystem privilege exclusions remain outstanding.

### MLT frame references and explicit progress routing

The next fresh round confirmed the TLS and dense-storage fixes, but found
MLT's local film and chain samplers still depended on normal-tail releases.
Throwing phase-three title/progress callbacks or sample evaluation could leak
those references. Both MLT implementations now scope frame resources, including
partial chain initialization, and scope temporary bootstrap samplers. Normal
cleanup still releases chains before film; image transfer and RNG behavior are
preserved.

The sibling animation path also temporarily pointed the authored progress
member at a stack frame adapter. The frame helpers now accept that borrowed
callback explicitly. Existing nonvirtual helper signatures/symbols forward to
additive overloads; public factory signatures, vtables and object layouts stay
unchanged. Animation never swaps the member, so exceptions cannot leave it
pointing at a dead frame adapter. No `mutable` or `const_cast` escape was added;
the four existing progress-member casts were removed.

CPP-only, calling-thread test observations retain actual chain/film references
after ownership is established. RGB, single-wavelength and hero-wavelength
cases throw on phase-three title and progress callbacks in still and animation
entry points, verify that only tester references remain, and retry twice
without reinstalling the progress callback. Tiny scenes use two chains and
finite environment bounds; all render invocations are serial. The focused
mode has 77 checks including manual flush and normalization oracles. The
inactive observation path adds no per-mutation or bootstrap-loop work.

Manual guidance now distinguishes committed film pixels from pending TLS
records: Resolve does not flush. A three-step oracle verifies explicit commit,
retained binding, and unbinding. A separate independent nine-pixel oracle shows
finite positive kernel rescaling leaves normalized splats unchanged. The stale
continuous-unit-integral requirement was removed; the finite positive discrete
sum requirement and legacy centre-value fallback are described explicitly.

Host verification for this increment (`1d8223fbd`, logs under
`/tmp/rise-third-pass/round19`): clean make, Xcode Deployment and shipping Opto,
plus the native Blender link, passed the compiler-warning gate (excluding
the known AppIntents tooling message). Thirteen selected regression programs
passed, including FrameStore's 360 checks; fourteen still/animation parity
cases ran sequentially across PT, BDPT, VCM and MLT with their spectral paths
and supported uneven-batch cases. Five ASan/UBSan suites passed. Focused TSan
checks passed for callback exports (33), tile exceptions (122), film bounds
(38) and MLT ownership (77). Thirty-one actual shipping Opto objects carrying
full LTO, fast math and finite-math protection also passed seven focused
programs and the same fourteen parity cases. These are selected regression
and instrumented-path checks, not evidence that every renderer path is safe.

An isolated normal-build counterfactual removed only the frame-resource guard
releases, retaining bootstrap ownership, explicit progress routing and the
observation hooks. It exited normally with status 1: 41 checks passed and 36
ownership checks failed. Each of the twelve callback scenarios failed its
post-throw and two retry reference-balance assertions. The unchanged fixed
build passed all 77. This demonstrates that the ownership oracle detects the
cleanup regression without restoring the old dangling animation adapter or
executing a known-invalid counterfactual under sanitizers. Exact sources,
hashes, commands and results are in `mlt-resource-red-*` under the same log
directory. Probe compile/link warning scans were empty.

Three fresh reviewers covered MLT ownership/progress, film/snapshot contracts,
and design/cost/documentation fidelity. They found no newly introduced defect
in this bounded increment, confirmed the snapshot roadblock below, and
identified the additional small-budget and submission-failure defects recorded
there (all three are since fixed in `2566595ae`; see the resolution
subsection that follows the roadblock). The review has not converged to
whole-branch zero P1s.

### Major roadblock: parallel BDPT intermediate snapshots (resolved; kept as history)

Status: the text below is the record as written at the roadblock. The three
defects it names are fixed in commit `2566595ae`; see "Resolution
of the snapshot, MLT budget and submission blockers" after this section.

The host continuation stops here under the user's instruction to continue until
completion or a major roadblock. At the time this was an unresolved P1, not a
documented acceptance of the race. The completed cache, publication, codec and lifetime
fixes remain available for branch review; there is no whole-branch zero-P1 claim.

After a tile finishes, `PixelBasedRasterizerHelper::SPRasterizeSingleBlock`
releases its tile locks and calls `GetIntermediateOutputImage` before iterating
the output list. BDPT permits per-block output, and its implementation copies
the entire changing primary image and resolves changing films into one shared
`pScratchImage`. Multiple workers can allocate or write that scratch image
concurrently, and another worker can overwrite it while an output callback
reads the returned reference. This path also executes with only file outputs
or an empty output list. Adapter-level delivery locks are downstream and do
not protect composition or source reads. VCM suppresses per-block output, so
this particular entry path does not establish the same VCM defect.

An isolated no-ray TSan diagnostic reproduced concurrent writes from the real
getter into an already allocated 8x8 scratch image, with a constant primary
and empty film. To expose the stores to TSan, the temporary probe subclasses
the scratch image and overrides `SetPEL` with the inherited pixel assignment;
the immutable LTO archive's original setter was uninstrumented and initially
hid the report. The production getter and film were unchanged. The instrumented
diagnostic reported the same eight-byte store from both threads and exited by
SIGABRT on this host. This corroborates the shared-scratch race; it is not a
fully instrumented production-render test or a proof of the separate source
read races. Probe sources, exact commands and logs are under
`/tmp/rise-third-pass/round19/scratch-snapshot-*` and
`scratch_snapshot_probe.cpp`; these temporary artifacts are not shipped tests.

A getter-local lock would expire before the callback consumes the reference;
preallocation would fix only the pointer race. Per-worker full-frame images
would multiply frame memory by worker count. Suppressing tile previews would
change visible behavior, including Blender updates. Neither trade-off has
been selected as the fix.

Proposed work after review:

1. Map the intermediate-output contract through the CLI, macOS GUI, Windows
   GUI, Android and Blender adapters. Specify the valid region, precision,
   lifetime and callback re-entry rules, including callbacks that retain or
   copy data. Public interface compatibility remains a requirement.
2. Prefer an owned region snapshot if consumers can honor region-only
   validity. Add region-aware internal composition and retain the snapshot
   through all synchronous consumers. Synchronize reads of primary pixels,
   filtered-film rows and splat-film rows against producers, including TLS
   flushes. Establish a single lock order; initial callbacks can run while
   tile locks are held, so an output lock followed by all tile locks can
   deadlock. Cover canonical FrameStore and private/raw image paths.
3. If full-image validity is required, design a shared full-frame snapshot
   whose guard spans composition through consumption and whose source reads
   are synchronized. Preserve native precision; the existing exported float
   buffers alone do not establish equivalent native-image semantics. Compare
   its copy cost and contention with owned region snapshots before choosing.
4. Add focused concurrent composition/consumption tests with deterministic
   pixel oracles and TSan instrumentation of every participating image/film
   producer. Exercise RGB, NM and HWSS, still/animation, canonical/private
   stores, no-output/file-only/live-output configurations, callback failures
   and retry, and stable preview cadence. Run actual renders sequentially.
5. Measure time, peak memory and preview latency against the current path at
   several frame/tile/worker sizes, using fixed seeds and
   `render_thread_reserve_count 0`. Gate clean make, Deployment, shipping Opto,
   Blender, sanitizers and fresh independent reviewers. Native Windows and
   Android follow-up is required for their consumer contracts.

Fresh source review also identified two P1s to handle when work resumed
(both since fixed; see the resolution subsection). These were present before the latest MLT ownership increment; their
provenance does not make them acceptable. Neither was executed as a crashing
or multi-billion-mutation counterfactual:

- **MLT small mutation budgets:** both frame implementations allow a positive
  total budget smaller than the chain count to produce zero mutations per
  chain. A measured bootstrap then clamps `numRounds` to zero before dividing
  by it. With an unmeasurably short bootstrap, the fixed 20-round fallback can
  exceed a small per-chain budget; the last round's unsigned subtraction
  underflows. For a 2x2 frame, eight chains and one mutation/pixel reach the
  zero-budget case; two chains and one mutation/pixel give two per chain but
  can schedule 19 before the last round. Public API, Job and scene paths do
  not reject these configurations. Define zero-work-chain behavior, bound
  rounds by available work in both timing branches, and partition mutations
  without unsigned subtraction underflow. Test exact mutation counts and
  normalization across timing branches, RGB/NM/HWSS and still/animation.
  The 77-check ownership fixture uses 32 mutations per chain and does not
  validate small-budget scheduling.
- **Thread-pool submission failure:** `ThreadPool::ParallelFor` appends tasks
  capturing its stack `body` by reference. If a later queue allocation throws,
  earlier entries remain queued while the stack function and render resources
  unwind. The normal task-exception barrier does not cover submission failure.
  Make enqueue transactional under the queue lock or drain every submitted
  task before propagating a submission error; preserve concurrent unrelated
  submissions, priority and caller participation. Add deterministic enqueue
  allocation-failure tests that verify no escaped closure, no hanging latch,
  exact completion/cancellation and a healthy subsequent call under sanitizers.

Accordingly, the latest resource tests at that point established cleanup for
the exercised callback failures and normal task execution, not safety under
failed task submission or correctness of every accepted MLT budget. The
resolution subsection below supersedes this limitation.

### Resolution of the snapshot, MLT budget and submission blockers

All three defects were present on master before this branch. They are fixed
in commit `2566595ae` (on top of `680f0c44b`). The changes have had one
fresh review round; this is not a whole-branch zero-P1 claim.
Evidence below is as reported by the implementing worker and was not
independently re-run unless stated.

**(a) Tile previews.** Decision: region-only composition. It was chosen over
a lock spanning composition and consumers, and over per-worker frames,
because the consumer map showed that every pixel-reading consumer (callback
dispatch in `Job.cpp`, Win32 window output) reads synchronously inside the
rect it is handed and retains nothing, and the GUI viewport sinks are no-ops
on this path. No lock is held across consumers and there are no per-worker
frame buffers.

`GetIntermediateOutputImage(primary, region)` composes only the inclusive
region. Only pixels inside it are valid in the returned image; concurrent
calls are allowed only for pairwise-disjoint regions on the same primary,
and a null region (whole image) must not run concurrently with any other
call. The primary is never mutated, and the returned reference must not be
retained past the consumers.

Scratch images are prepared single-threaded before worker passes
(`EnsureCompositionScratch`, called from `PreRenderSetup`, which also covers
the BDPT path-guiding training pass where the old lazy allocation ran on
workers, and from `RasterizeScene`). The getter never allocates; if the
scratch is missing or its dimensions do not match, it returns the primary
and logs one warning per process. The same shared-scratch defect in the base
getter (PT with a wide filter) is fixed the same way.

`SplatFilm` and `FilteredFilm` `Resolve`/`Unresolve` now take the writers'
per-row mutex, copy the row, unlock, then write the target, so one row lock
is held at a time and none across the target. `FrameStore::CopyTileFromRasterImage`
gained an optional inclusive clip, and the unbound `ViewportFrameStore` path
uses it, so it no longer reads source pixels outside its region. Behaviour
change: FrameStore pixels of an overlapped tile outside the region keep their
previous content. `ResolveSplatIntoScratch` had no callers and was removed.

Evidence (implementing worker reported). New `tests/IntermediateCompositionTest.cpp`:
120 passed, 0 failed on the fix. Against the HEAD production files its oracle
rows read 59/29, the concurrent compose row 0/2 (97,538 and 74,845
mismatched pixels), film rows 3/2 (28,219 torn `SplatFilm` pixels, 12,620
`FilteredFilm`) and scratch rows 2/7. `ViewportFrameStoreTest` 108/0
(107/1 on HEAD). TSan with the films, rasterizer bases, `FrameStore`,
`ViewportFrameStore` and `ThreadLocalSplatBuffer` instrumented: clean on the
fix; on HEAD 8 warnings in the film `Resolve` reads and 3 on the lazy scratch
allocation. The HEAD concurrent-compose row is red functionally but not under
TSan (the inline image setters resolve to the uninstrumented archive copy;
inferred). ASan/UBSan clean. The final image is unchanged within noise on
`cornellbox_bdpt_materials` at 192x192, 16 spp (mean 0.894351 sd 5.1e-4 vs
0.894846 sd 9.1e-4, n=3 each). Cost: 1024x1024 at 1 spp BDPT 1.79-1.87 s
before vs 1.51-1.56 s after (about 17% faster); 512x512 at 4 spp no
measurable difference. Gate suites green: `FrameStoreTest` 360/0,
`LuminarySamplerAreaTest` 90/0, `AnimationRasterizerParityTest` 102/0,
`SpectralSplatActiveCountTest` 30/0, `SpectralSplatIntegralNormalizationTest`
21/0, `BDPTStrategyBalanceTest` 0 failed, `SourceHygieneTest` 192/0.

**(b) MLT budgets.** One shared 64-bit schedule in `MLTRasterizer.h`
(`ComputeMLTMutationSchedule` and helpers) is used by both rasterizers:
saturating budget, every chain performs at least one mutation, performed
total = perChain x chains (this may exceed a tiny budget by fewer than
`chains`), splat normalization and the preview fraction use the performed
total, rounds are clamped to [1, perChain] in both timing branches, the loop
is driven by remaining work, there is no unsigned underflow, and
float-to-integer conversions are guarded. Also found and fixed in the same
change: the 32-bit budget product wrapped above 2^32 mutations (3840x2160 at
about 518 mutations/pixel) while normalization used the true product, and
truncation of budget/chains left the image dark by performed/budget.

Evidence (worker reported). New `tests/MLTMutationScheduleTest.cpp` 28/0.
With the two rasterizer `.cpp` files at HEAD: 16/9; budget 4 with 3 chains
rendered at 0.750 of reference (RGB 0.750000, spectral 0.749786, HWSS
0.750012); 2x2 with 8 chains produced a black frame on arm64 (integer divide
by zero does not trap there); the too-fast-bootstrap case hung and was
killed after 60 s. Round cadence, RNG order and progress arguments for
ordinary budgets are unchanged. Not covered by a render: budgets above 2^32
(helper and loop simulation only); x86 SIGFPE behaviour is inferred.

**(c) Thread-pool submission.** The `ThreadPool::ParallelFor` enqueue is now
transactional: on any exception in the push loop it pops exactly its own
tasks under the still-held queue lock and rethrows. No task of a failed
submission runs, nothing is notified, and unrelated queued tasks keep their
order. Evidence (worker reported): `ThreadPoolExceptionSafetyTest` 15/0 with
a new case sweeping 305 allocation-failure points over 300 tasks using a
thread-local countdown in a replaced global `operator new`; 14/1 with HEAD
`ThreadPool.cpp`; ASan/UBSan and TSan runs of that test clean (test and
`ThreadPool.cpp` instrumented only).

**(d) Verification and review.** Clean make rebuild: rc 0, zero warnings.
Xcode Deployment clean build (RISE-GUI): succeeded, zero warnings. One fresh round of three independent
reviewers (thread pool and MLT; preview concurrency; film locking and
`FrameStore` clip) found no P0 or P1, by code reading, not by running. This is
one review round on those changes (now commit `2566595ae`), not a whole-branch zero-P1 claim.
Windows, Android, Xcode Opto and Blender builds were not rerun for these
changes.

**(e) P2s recorded by that review, and their follow-up.** The follow-up
is committed as `53805a047`. It had two review rounds: the first found a
Hilbert regression among other findings (addressed below), and the second
found no P0 or P1. Numbers are from the implementing worker's runs. Both
clean builds (make and Xcode Deployment RISE-GUI) were warning-free on the
tree BEFORE this follow-up; for the follow-up only the make build was run
(zero warnings). Windows, Android, Xcode Opto and Blender were not rebuilt.

- Fixed: raster sequences overlapped, escaped their range or read out of
  bounds. Morton (the default for ordinary renders, `raster_sequence_type`
  4, and every rasterizer's internal default) is untouched.
  - `ScanlineRasterizeSequence` returned rects two rows tall (inclusive
    bounds), so neighbouring regions shared a row, and a one-row range
    produced no regions; it now returns one row per region.
  - `BlockRasterizeSequence` started its blocks at (0,0) instead of the
    range origin, so a crop rendered outside itself. Interactive
    region-of-interest renders (the editor's region box, which uses this
    sequence through `InteractivePelRasterizer`) therefore re-rendered
    every block from (0,0) up to the region; they now render only the
    region. Block boundaries now sit on absolute multiples of the block
    size, clipped to the range, so a crop with an unaligned origin keeps
    the same block grid as the full frame and no two blocks share a
    FrameStore tile when the block size is a multiple of 32. The
    centre-out order (type 0, the interactive default) now centres on the
    crop (it centred on a frame-relative offset), and the unsynchronised
    file-scope `g_width`/`g_height` globals are gone (the comparator
    captures the centre). Full-frame tiling changes only when the block is
    at least as large as a frame dimension: one block instead of block-1
    plus a 1-pixel sliver. Against a HEAD copy over 117,450 full-frame
    configurations (9 block sizes, 9 orders), every differing list is such
    a case; all others are identical, order included.
  - Shared FrameStore tiles were checked, not assumed harmful: the
    `bufferMutex_` inversion `AdaptiveTileSizer.h` describes no longer
    exists in the source (the name survives only in comments), each block
    takes its FrameStore tile locks in one global (row, column) order
    (`FrameStoreTileBracket::Acquire`) and releases them all before
    dispatching observers, and `Render` holds one shared tile lock at a
    time. Two blocks sharing a tile therefore serialize but cannot
    deadlock (inferred from reading the locking, not from a stress run).
    Morton crops with an unaligned origin share tiles the same way; Morton
    was not changed.
  - `HilbertRasterizeSequence` walked a num x num grid that its block
    split often did not produce. At HEAD about 2,455 of 7,975 full-frame
    configurations read past the block list (a 1x1 frame aborts under
    ASan) before logging a fallback; the first follow-up's extent change
    moved another 1,500 (any odd dimension at depth 1, up to 1920; depth 4,
    the type-3 default, at sizes up to 49) from clean to out of bounds.
    `Begin` also decremented `depth` permanently on a reused instance, and
    depth 0 recursed forever. The walk now runs on an exactly num x num
    grid with bounds-checked indexing and no member mutation: the
    historical split is used when it yields exactly num columns and rows
    (so every full-frame configuration that was clean at HEAD returns the
    identical list, 5,520 of 5,520), otherwise each axis is split into num
    near-equal spans with the depth reduced until 2^depth fits; depth 0
    returns one block. The fallback remains only as a guard and is never
    taken in the test sweep.
  - `tests/RasterizeSequenceCoverageTest.cpp` (228 full-frame ranges incl.
    1x1, 9x100, 17x3, 101x100, 1920x1080, 1919x1080, plus 6 crops; Hilbert depths 0-5;
    every range through one reused and a fresh instance; Hilbert fallback is
    a failure; block-crop tile sharing and centre order): 25,994/0, and
    25,994/0 compiled with `-fsanitize=address,undefined`. Against the
    first follow-up's headers: 705 failures without sanitizers (694
    reused-vs-fresh mismatches from the depth mutation, 10 tile-sharing,
    1 centre) and an ASan heap-buffer-overflow in `move()` at depth 1;
    against HEAD: 1,255 failures (500 coverage, 17 empty, 733 reuse, 5
    block-crop) and the same ASan abort. No shipped scene selects these
    sequences (only the `raster_sequence_type` option and the interactive
    rasterizer's block orders do); no render was compared.
- Fixed: the composition scratch outlived the render. A virtual
  `ReleaseRenderScopedPreviewResources` (filtered film + its scratch, plus
  `pScratchImage` in `BidirectionalRasterizerBase`) runs from every
  top-level entry point's scope guard (helper still and animation, BDPT
  still) on success, cancel and unwind; frames of one animation still reuse
  the buffer. BDPT/VCM no longer allocate the unread `pFilteredScratch`
  (`UsesFilteredPreviewScratch`). `IntermediateCompositionTest` row E: 0 of
  1 composed images still referenced after every render, vs 1 of 1 for VCM
  still/progressive/animation and BDPT animation with the HEAD files
  (81/8 red).
- Fixed: row E now proves composed delivery. The capture output counts
  previews whose image is not the primary and checks the scratch is
  released; rows cover BDPT pel/spectral single-pass and progressive, a
  BDPT progressive animation frame, VCM single-pass, progressive and
  animation, and PT with a wide filter through `RasterizeScene`, each
  rendered twice on one job. With both getters mutated to return the
  primary: 71/18 red. Animation per-block previews deliver the primary by
  design (`SPRasterizeSingleBlockOfAnimation` composes nothing); only
  pass-end previews of a progressive animation are composed.
- Fixed: `CallbackRasterizerOutputDispatch` no longer allocates a float
  frame per tile. The receiver contract is a full-frame array indexed by the
  rect (the Blender bridge reads it that way), so the layout is kept. The
  frame-sized float and 16-bit buffers are owned by the dispatcher, reused
  between intermediate deliveries under its mutex, zeroed whenever
  (re)allocated, and freed after each final `OutputImage` (also on unwind),
  so they are held only while a render delivers tiles (about 133 MB float
  at 4K) and a whole-buffer reader never sees a previous render's pixels.
  Outside the delivered rect the buffer holds zeros or earlier tiles of the
  same render. New `ImageReaderOverflowTest` assertions (375/0): successive
  tile deliveries keep the full-frame layout, and across a final delivery,
  a resize and a format switch the rect pixels are exact and new storage
  is zero outside the rect.
- Fixed: MLT with `bootstrap_samples 0` (reachable from a scene): the 0/0
  mean is NaN, the `<= 0` guard is then false, and the code read
  `bootstrapSamples[0]` and wrote `cdf[0]` on empty vectors (out-of-bounds
  read and write); the implementing worker observed a crash (exit 139,
  arm64 macOS). The parser now rejects 0 with a diagnostic naming
  `bootstrap_samples`, and both rasterizers fail the render with a logged
  error when constructed with 0 through the API; the animation entry
  points log that as a failure, not as a user cancel.
  `MLTMutationScheduleTest` 37/0 (diagnostic text checked for RGB,
  spectral and HWSS; API still and animation renders for all three);
  with the HEAD files the three load checks fail and the API render
  crashes. `chains 0` and `mutations_per_pixel 0` are clamped to 1 by the
  schedule and remain accepted.
- Fixed, uncompiled: `Win32WindowRasterizerOutput::OutputIntermediateImage`
  clamps the inclusive region to the image and its own buffer (empty,
  inverted and outside regions write nothing). Windows was not built.
- Fixed: the `ThreadPool` allocation-failure case no longer assumes one
  allocation per pushed task; it requires at least one failing point and
  that the sweep reaches an uninjected submission.
- `VCMPelRasterizer.h` now marks all of its overrides `override`.

**(f) Findings of a later independent review of `680f0c44b..53805a047` (committed in `9dba00e2a`, merged in `556bf4a3b`).**
A review round on the three fixes below found no P0 or P1; its P2s are addressed
in the follow-up at the end of this part and committed together in `9dba00e2a`.
Both clean builds (make, Xcode Deployment RISE-GUI) were warning-free on the
tree before that small follow-up; for the follow-up the make build and Xcode
Development RISE-GUI build were run (zero compiler warnings). Numbers below are
from the implementing worker's runs.

- P1, fixed: callback buffers survived a render that never reached a final
  delivery. The dispatcher freed its frame-sized buffers only after a final
  `OutputImage`, so a receiver or later output that threw, a cancelled still,
  or a cancelled animation left them allocated for the Job's lifetime, and a
  retry at the same size and format reused them without zeroing (the
  receiver saw the previous render's pixels outside its rect). Decision:
  bracket every Job render entry point rather than add a begin/end
  notification to `IRasterizerOutput`. `IRasterizerOutput` has no per-render
  notification, adding one is a public-interface change, and every rasterizer
  type would have to call it; callback dispatchers are only ever created by
  `Job::AddCallbackRasterizerOutput`, and every in-tree host that uses them
  (Blender bridge, 3ds Max) renders through `Job::Rasterize`. The Job now
  registers each dispatcher it creates (one counted reference, dropped by
  `RemoveRasterizerOutputs` and the destructor) and a
  `Job::CallbackOutputRenderScope` in `Rasterize`, `RasterizeRegion`,
  `RasterizeAnimation` and both `RasterizeAnimationUsingOptions` releases
  their buffers on entry and on every exit, under each dispatcher's mutex. The
  per-final-delivery free stays. Coverage: `ManagedJob`, DRISE, the ASCII
  command parser, the CLI and the GUIs reach these entry points; the agent's
  ephemeral rasterizers and the AutoRasterizer probe never carry a callback
  output; `SceneEditController`'s legacy production render calls the Job's
  rasterizer directly and is not covered, but no host attaching callback
  outputs uses the scene editor, and the next Job-entry render releases on
  entry. New `tests/CallbackOutputRenderScopeTest.cpp`: real PT and BDPT
  renders through the IJob entry points, both receiver formats, cases
  receiver throws, progress cancels, animation cancelled, a later output
  throws, and success; after each, no dispatcher owns a buffer and a retry
  region render never shows a nonzero pixel outside the rects already
  delivered in that render. 208/0; with the HEAD `Job.cpp`/`Job.h` plus only
  the test probe, 184/24 (every case red on both checks except the cancelled
  still, whose rasterizers still deliver a final image, and success).
  Sanitizers (test, `Job.cpp`, `FrameStore.cpp`, `ViewportFrameStore.cpp`
  instrumented): ASan/UBSan and TSan clean, 208/0 each (the first TSan run's
  17 reports were all in the test harness's unsynchronized counters, now
  atomic).
- P2, fixed: the 64-bit per-chain MLT budget could drive a chain past
  2^32 iterations while `PSSMLTSampler` kept 32-bit iteration stamps, so the
  lazy catch-up comparisons broke at the wrap. Decision: widen
  `currentIteration`, `lastLargeStepIteration` and both `PrimarySample`
  stamps to 64 bits (`PrimarySample` 24 to 32 bytes) rather than cap the
  schedule. Ordinary budgets are bit-identical: a driver dumping 5,970,972
  draws (6 seeds, large-step probabilities 0, 0.3 and 0.9, 20,000 iterations,
  legacy, extra-tier and alpha streams, accepts and rejects) is byte-identical
  between HEAD and the fix (ASan/UBSan run of the suite with the sampler
  instrumented: clean). New `PSSMLTStreamAliasingTest` test I seeds a
  sampler at 2^32-40 and compares 410 draws over 120 iterations with a twin
  seeded at 1000: identical on the fix, 253 of 410 differ on HEAD. Cost on
  `scenes/Tests/Legacy/cornellbox_mlt_fast.RISEscene`, 10 interleaved pairs:
  bootstrap 1705 to 1715 ms (+0.6%, paired t 0.96), total 6736 to 6769 ms
  (+0.5%, t 1.06), not resolvable. Audit of the rest of the MLT path: the
  round loop, remaining-work counts, splat normalization, progress and
  timing are already 64-bit or saturating; per-round and per-segment counts
  are clamped to `UINT_MAX` by the schedule helpers; `MLTAdaptiveRoundResize`
  can wrap its informational round count to 0 only for a saturated
  2^64-mutation budget, which makes every round "planned last" and still
  takes `min(remaining, UINT_MAX)` (not changed). The lazy catch-up still
  applies one mutation per skipped iteration, so a lane untouched for very
  many iterations with a large-step probability near 0 is slow by design.
- P2, fixed: the unbound `ViewportFrameStore::OutputIntermediateImage`
  added 1 to the inclusive endpoint before clamping, so `UINT_MAX` ("to the
  edge") wrapped to 0 and a valid region copied nothing and fired no tile
  notification. The endpoint is now clamped first. Sibling audit: the same
  wrap was live in `FrameStore`'s beauty view `Clear` (cleared nothing), and
  `RISEWriteOnlyRasterImage::Clear` did not clamp at all (an endpoint past the
  image looped writing out-of-range coordinates, forever at `UINT_MAX`); both
  fixed to the `RISERasterImage::Clear` contract. Already safe:
  `CopyTileFromRasterImage` (64-bit clip), `ClampRegionToImage`/
  `IntersectRegion`/`CopyRegion` and `BoundsFromRect` (clamp before use),
  `SplatFilm`/`FilteredFilm` `Resolve`, `OIDNDenoiser`'s region path and
  `RegionalPixelCount` (clamp first), the callback dispatcher (clamps), the
  Blender and 3ds Max receivers (handed clamped rects), `FrameSink` (ignores
  the region), and `Win32WindowRasterizerOutput` (adds 1 only below the
  clamp; read, not compiled). `ViewportFrameStoreTest` 116/0 (110/6 on HEAD)
  and `FrameStoreTest` 362/0 (360/2 on HEAD).
- Follow-up to that review's P2s:
  - Registry lifetime. Each entry now records the rasterizer its
    dispatcher is attached to (outputs are never replayed onto another
    rasterizer) and is dropped where that rasterizer's outputs go away:
    `DestroyContainers` (so `ClearAll`, reload, variant switch and edit
    re-derive), `RegisterAndActivateRasterizer` replacing an instance, and
    `RemoveRasterizerOutputs`, which now drops only the active rasterizer's
    entries (it frees only that rasterizer's outputs). Comments state that
    `ReleaseRenderBuffers` and the dispatcher destructor never call the
    receiver, and that a receiver must not call Job render or
    output-management entry points from its callback (non-recursive
    `dispatchMutex`). New test rows: four `ClearAll` + reload cycles keep the
    registry at one entry and drop it to zero after each reload; replacing
    `pixelpel_rasterizer` drops its entry; `RemoveRasterizerOutputs` on one
    of two rasterizers leaves the other's dispatcher registered and still
    delivering. With the previous round's registry behaviour these rows read
    9 failures (232/9).
  - The three animation entry points never released their raster sequence
    (pre-existing). All five entry points now use a scoped release declared
    before the callback-output scope, so the sequence is released after that
    scope ends, on return or unwind. `leaks --atExit` on the callback test:
    0 leaks; with only the three animation releases removed, 24 leaks
    (`MortonRasterizeSequence`). ASan's LeakSanitizer was not used on this
    macOS host; `leaks` is the check that was run.
  - The cancel rows were vacuous at 64x64 (about 4 tiles). The frame is now
    256x256 and each cancel row first renders the same job uncancelled:
    still renders 4 deliveries cancelled vs 130 uncancelled, animations
    14-26 vs 387; the check requires fewer than half. A new row isolates
    release on ENTRY: a failed direct rasterizer call (not bracketed, like
    `SceneEditController`'s legacy production render) leaves the buffer
    held, and the next Job-entry region render shows no stale pixel; with
    the entry release removed it fails 4 of 4 (237/4).
  - `CallbackOutputRenderScopeTest` 241/0.

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
| **Legacy expression allocator** | The v6 migrator still uses `MathExpressionEvaluator`, which has a global 1 KiB `MemStack`, scanner, and error state. The allocator logs on exhaustion and still returns a pointer, so the caller writes out of bounds. `tests/CstDeriveGoldenTest.cpp` also consumes the evaluator through `CstMigrator.h`. The evaluator is compiled into the library in all five build lists, and no library code path calls it, so moving it means the five-project source checklist. Move the evaluator into migration-tool support once its consumers are confirmed, with per-evaluation state and RAII-owned nodes. Consider an arena only if the tool's workload justifies it. `MathExpressionEvaluator.h` (moved into [CstMigrator.h](../tools/CstMigrator.h) in this series) · [CstMigrator.h](../tools/CstMigrator.h#L151) | Compare exact migrated output for the existing corpus and check repeated runs, deep expressions, and parse failures. Preserve the legacy evaluator's rounding and operator behavior. |
| **Fixed-size output paths and log records** | `FileRasterizerOutput` concatenates folder and pattern into a 1,024-byte member with `strcpy` and `strcat`. The script loader copies resolved paths into another 1,024-byte buffer with an unchecked `strcpy`, in the same constructor as the parser lifetime fix above. The log uses fixed-size messages, and its plain-copy and Windows format paths can leave a full record unterminated. `Win32Console` concatenates that same record. These three sites are representative. `src/Library` has about 20 `strcpy`, 35 `strcat`, 4 `sprintf`, and 6 `sscanf`-with-`%s` calls, so inventory them before closing this row. Use owned paths at the private I/O boundaries and guarantee termination at the existing log callback boundary. [FileRasterizerOutput.cpp](../src/Library/Rendering/FileRasterizerOutput.cpp#L60) · [AsciiScriptParser.cpp](../src/Library/Parsers/AsciiScriptParser.cpp#L27) · [Log.cpp](../src/Library/Utilities/Log/Log.cpp#L75) | Check long and Unicode paths, existing suffix and frame numbering, log record bounds, and truncation behavior. Use `FileRasterizerOutputShimTest` for existing output parity. |

## Findings and proposed replacements

| Utility | Evidence and cost | Proposed disposition |
| --- | --- | --- |
| `RISE::String` in `RString.h` | Publicly derives from `std::vector<char>`. Construction from a C string scans twice and stores the terminator as an element. Appends rescan the accumulated text and add one more NUL element per call, so `String("a")` with `"b"` appended does not compare equal, as a map key, to `String("ab")`. C-string assignment returns by value. Numeric accessors ignore conversion failure and return an uninitialized value. The header has 38 direct in-tree includes, plus consumers through other headers and the PCH. | Use `std::string` for ownership and `std::string_view` for bounded, borrowed text. Move conversions to the parser or CLI where accepted syntax and errors are defined. Retain the existing type wherever binary compatibility requires it. [RString.h](../src/Library/Utilities/RString.h) |
| Named managers and options | `GenericManager` constructs a `String` for item and serial lookups and stores names in ordered maps. `Options` has a similar lookup pattern. These are direct allocation targets in scene loading, derive, and editor operations. | Use ordered `std::string` keys with transparent lookup using a length-aware view. Preserve enumeration order, empty-name behavior, serials, reference tracking, and callbacks. [GenericManager.h](../src/Library/Managers/GenericManager.h) · [Options.cpp](../src/Library/Options.cpp) |
| `BitField`, `MRUCache`, `SimpleNotification`, `CoreUtils`, `DynamicProperties`, Windows `LoadLibrary` wrapper, `SmartColor`, `WardToneMappingOperator`, `NullScatteringTracker` | The tracked source sweep found no consumer of these utilities outside their own definitions and implementation includes. Some still appear in project files. `DynamicProperties` is a stub and the only one registered in all five build projects, so it is the one that compiles everywhere. `BitField.cpp` is still listed in Xcode and VS2022. The last three are header-only. | Delete after confirming the supported distribution and header surface. `Observable` still drives painter-to-displacement invalidation and stays in use. `DynamicProperties.cpp` (removed in this series) · [VS2022 Library project](../build/VS2022/Library/Library.vcxproj) · [DisplacedGeometry.cpp](../src/Library/Geometry/DisplacedGeometry.cpp) |
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
