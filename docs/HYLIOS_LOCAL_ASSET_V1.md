# Private HyLIoS local asset codec increment

This increment supplies an immutable bounded file codec around the separately
reviewed numeric local-response core. It supplies neither an MC baker nor a
material/render preparation integration. Synthetic fixtures validate encoding,
identity checks, physical representation and I/O; they do not validate skin.

`LocalAssetDefinition` supplies full independent incident/outgoing projected
`t=mu^2, phi` grids, spectral nodes, continuous F and planar-mirror coefficients,
plus a **separate normal-incidence guide experiment** on exactly those wavelength
nodes. The guide is physical hemispherical reflectance in [0,1], not RGB, and is
never inferred from a cell center. The format stores the guide experiment tag
and explicit histories/replicate declarations. This records a producer claim;
the codec cannot observe how those experiments were performed.

## Semantic and production responsibilities

Every load requires a `LocalAssetSemanticIdentity` independently prepared from
the requested actual configuration. Its closed schema has canonical nonempty
records for model, placement, postscatter, boundary, lineage, data, specimen,
exterior, and approximation, plus the exact covered interval. These records are
opaque **caller-prepared claims**, not biological validation by this codec. The
header documents the caller's obligation to include all chosen model versions,
actual resolved SI values, organization/integerization, full resolved spectral
values/nodes, data intersection, source byte hashes, exterior, physical scale,
elastic/emission assumptions, and placement/discard/approximation laws. Rendering
must implement that preparation obligation before using this codec in production.

The loader compares every exact semantic field against the independent expected
record, naming the differing field. Producer hash strings never replace those
comparisons. Covered wavelength endpoints must equal the actual node endpoints;
strict ordering/full angular coverage/passivity are validated by the numeric core.

The closed production schema declares oracle/lineage/estimator/history/statistics
revisions, named SplitMix64 top53 RNG scheme, an exact uint64 seed, histories,
replicates, separate guide histories, per-history work budget, and bounded build
and statistics records. It also fixes grid averaging, right-handed frame, no
azimuth reduction, physical spectral interpolation, mirror map, guide experiment,
encoding, and an explicit producer declaration of complete physical accounting.
Count/work products are checked for overflow. The actual mapping implementation,
history completeness, uncertainty and scientific convergence are producer
obligations, not certified by parsing a label. The statistics record must contain
bin/row/atom replicate uncertainty and held-out grid/spectral evidence; the build
record must identify the actual producer build/source/executable.

The semantic SHA hashes exact canonical semantic fields. The production SHA
hashes the exact canonical semantic record **and** production declarations,
actual grids/nodes, format and encoding-error record: the entire closed canonical
manifest for this declared experiment.
The content SHA covers all exact framed bytes preceding the digest trailer.
These immutable identities are independent of pathnames and provide integrity,
not authentication or scientific truth.

## Binary framing

All integer fields and physical binary32 coefficients are little endian.
The 80-byte header is:

| Offset | Field |
| --- | --- |
| 0 | `HYLOCAL\0` (8 bytes) |
| 8 | uint32 version=1 |
| 12 | uint32 endian marker=0x01020304 |
| 16 | uint64 metadata bytes |
| 24 | uint64 payload bytes |
| 32,40 | uint64 incident t-edge/phi-edge counts |
| 48,56 | uint64 outgoing t-edge/phi-edge counts |
| 64 | uint64 wavelength node count |
| 72 | uint64 reserved=0 |

The header is followed by canonical RISE-CBOR64-v1 metadata, then contiguous
physical payload `[wavelength][incident][outgoing] F`,
`[wavelength][incident] mirror`, and `[wavelength] guide`. A 32-byte SHA256 digest
of the preceding header/metadata/payload finishes the file. All counts/offsets
are derived and checked; no independent CDF or probability table is trusted.
Exact extent is mandatory, so truncation and extra bytes refuse the load.

The nine top-level metadata keys are `format`, `semantic`, `production`,
`incident_t`, `incident_phi`, `outgoing_t`, `outgoing_phi`, `wavelength_nm`, and
`encoding_error`. Axes/nodes use binary64 CBOR numbers to keep
the supplied core geometry unchanged. Semantic/build/statistics subrecords are
canonical CBOR byte strings, each containing a nonempty map. Unknown keys,
duplicate/noncanonical tokens, wrong scalar types, unsupported conventions,
nonfinite values and invalid maps refuse. Import rejects negative-zero wire
coefficients; export accepts mathematical source zero of either sign and
canonicalizes it to the single positive-zero wire representation.

## Downward encoding and physical gates

`positive_float32_downward_le_v1` truncates the IEEE binary64 mantissa with
integer operations to the greatest nonnegative binary32 not exceeding the input.
Positive-to-zero conversion refuses publication. Integer decoding plus double
`ldexp` preserves binary32 subnormals even with shipping fast-math flags and
avoids dependence on float arithmetic modes. The source core and encoded core
are each independently constructed/validated; no row clamp, normalization or
passivity epsilon is used. Unsafe source rows cannot be made valid by encoding.

The writer reports the maximum absolute coefficient loss. Its forward-row loss
bound uses the actual numeric core's represented scaled cell areas: the upward
successor of each public scalar area bounds its retained dyadic area; every sum,
mirror addition and final product is rounded upward with `nextafter`. Thus tiny
positive area terms cannot silently disappear from the bound. MC, angular and
spectral errors remain separate. The persisted error record is a producer claim
on load because original double coefficients are unavailable. Loaded physical
passivity is independently checked, regardless of that claim.

## Resource and publication policy

Defaults are 256MiB file and 1GiB codec peak resident budget. Metadata has a hard
64KiB profile cap; large grid metadata may exceed it even if the physical payload
would fit. Axis edge/node counts additionally cap at 8192; default numeric-core
limits (including 4096 nodes, 65536 cells and 32M continuous entries) also apply.
Metadata syntax is preflighted without allocations with depth<=8 and <=8192 tokens
before generic decoding; each opaque record is <=4096 bytes with the same caps.

Checked integer arithmetic precedes allocations. The conservative logical peak
estimate includes two wire buffers, decoded physical arrays, two numeric cores
(including CDFs), grid/node copies, a fixed 1MiB I/O/control allowance, and 4096
bytes per metadata byte for bounded recursive Value copies/records/allocation
slack. Save reserves this full configurable metadata allowance before building
metadata; a small save can therefore refuse a tight custom resident/file budget
that load would accept. This conservative v1 policy is explicit, not a precise
malloc measurement. Loading uses the actual metadata length. Aggregate context
residency and per-content sharing belong to the later render preparation layer.

The loader opens nonblocking on POSIX (wide UTF8-path CRT opening on Windows),
validates the same descriptor is a regular file,
checks its stat extent/budget, then reads bounded header/metadata/payload bytes.
It never reopens a pathname for hashing. The fully decoded object owns immutable
values, keys and guide; replacing the file cannot change an in-flight object.

Codec paths are UTF8 on all platforms. Publication uses the shared utility's
explicit `DiskFileWriteBuffer::WriteAtomicallyUTF8` opt-in with `Required` staging.
The existing string APIs retain native pathname decoding and native diagnostic
text; this increment does not reinterpret legacy raw/image filenames as UTF8.
One common `WriteAtomicallyNative(fs::path, ...)` transaction implements both
entry points with the same policy and synchronization. Native filesystem paths
reach all Windows `_wopen`/`_wfopen` calls, including destination probes, staging,
regular/direct opens and completed-file copy reads. Logging/display strings are
kept separate and never converted back into OS paths. The UTF8 opt-in returns
UTF8 recovery/cleanup paths, and codec publication exceptions retain the recovery
hint so the same asset loader can reopen the completed artifact. If publication
fails after copying a write-only destination mode, only the retained completed
artifact requests owner-read permission; successful destination modes stay exact.
If that adjustment fails, a separate secondary log diagnostic preserves the
original operation/error and recovery path without changing `WriteDiagnostics`
layout.
Validation completes before staging. The helper finishes and atomically renames
completed output; a per-call private observer supports failure tests at the real
pre-publication boundary. No alternate writer path, global hook or TLS is added.

## Integration and verification

Only these new source files need registration in all five library projects:
`src/Library/Materials/HyLIoS/HyLIoSLocalResponseAsset.{h,cpp}`. The existing
`src/Library/Utilities/DiskFileWriteBuffer.{h,cpp}` also have a shared patch;
there are no new utility source files, object fields or virtual methods. Register
`tests/HyLIoSLocalResponseAssetTest.cpp` in the normal standalone test/build lists.
The codec is registered in all five authoritative Library projects. Run its
repository gate with:

```sh
make -C build/make/rise build-test/HyLIoSLocalResponseAssetTest
./bin/tests/HyLIoSLocalResponseAssetTest
```

Before integration, 101 focused checks passed strict warning-as-error,
ASan/UBSan and mixed full-LTO builds. Four mutations independently failed the
intended checks: digest bypass, nearest float32 encoding, semantic identity
omission, and unreadable retained recovery. Shared I/O gates covered 171 volume
checks and 302 file-output checks, including sanitized Cst/observer/CBOR
translation units to keep container annotations consistent. Windows CRT and
codepage signatures were checked with extracted branch probes; actual Windows
SDK/runtime execution remains unverified. These codec tests do not establish
biological correctness, a completed bake, or renderer calibration.
