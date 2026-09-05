# r204 — payload Merkle digest and sealing placement

Status: digest foundation and production sealing placement implemented; final
placement review and cost evidence sealing remain pending. This is not an affordable-owner or replay
verdict. The r203 EOS bin repair stands. Continuation is from zero, not r78 resume.

## Ruling and identity boundary

Provenance seals what leaves the step; the qualified kernel set vouches for what
stays inside it. The authorized next increment retains intermediate full-payload
seals in qualification/diagnostic mode, uses authenticated kernel-set lineage and
stage tokens internally in production, and computes full payload digests at
checkpoint cadence and every published frame/evidence/run record. This changes
what is hashed, never the arithmetic. The r190 owner comparison, class-obligation
contract, conservation/admissibility, and residency REDs remain gates.

The foundation commits `ebf91c2e` and `9666ddee` did not activate placement.
The placement increment specializes the resident producer library: diagnostic
mode retains full intermediate payload hashes; production uses constant-size
device tokens, authenticated private-buffer capabilities and immediate-parent
checks. The projection adapter likewise retains legacy full hashes outside the
qualified owner. No new replay or fixed-k selection is claimed yet.

## Production placement and publication

The owner binds an ordered, length-described copy of its actual private ingress
buffers to a v2 root, together with the compiled producer/FCT/force/projection/
payload library source SHAs and producer function inventory SHA. Separately
adapted force and acceptance controls are included by their exact bit patterns.
No CPU-produced physics authority is admitted. Domain-separated
internal tokens derive from that root and the existing attempt/stage/immediate-
parent identities. Tokens are protocol identities, **not** payload digests; their
format is `qualified-kernel-stage-token`, version 2. Internal arithmetic is
unchanged. Qualification obligation-map atomics are compiled out in production;
failure/admissibility atomics and numerical interval decisions remain active.

This owner currently publishes a host result every step. Therefore every step
still computes a full device v2 digest over its terminal packet: conservative
fields, nine packed face fields, ten cell fields, commuting witnesses, all
stage identities, and the input root. It is not merely a digest of the final Q.
The root occupies the final 32 bytes of the existing single terminal staging
buffer. A private, move-only accepted token carries it through publication;
copying the result cannot copy its authority. The trajectory captures this root
before the one-use acceptance token is consumed. Checkpoints and temporal VDB
frames additionally receive complete-file v2 sidecars. New run directories seal
all finalized evidence files at finalization; mutable logs are working evidence
until that boundary succeeds. Historical artifacts are never updated in place.

Working-set accounting charges the ingress concatenation, both private Merkle
trees, descriptor and roots; actual ingress length is checked against that
certificate. Tree storage is bounded by payload/120 plus eight allocation quanta
per tree (4-KiB leaves, 32-byte hashes, geometric interior sum). The retained
full-owner working set is not claimed to fit the historical two-GiB fixture cap.

The r190 gate remains primary. Diagnostic versus production sealing additionally
bit-compares 41 numerical fields (including face densities and both commuting
witnesses). A separate
CPU reconstruction of the **complete terminal packet** matches the device root;
a bit mutation changes it and a copied result loses the root capability. The
stale/order/forged-parent/callback/atomic-publication/policy/unverified-buffer and
residency REDs exercise production token mode, not just the diagnostic path.
Placement review found a projected-replay summary-mode exclusion, omitted
publication siblings, and missing ingress controls/library identities. The
fixes add all five library hashes, seven scalar controls plus the iteration cap,
new-directory finalization on the sibling commands, and CPU publication REDs.
Each library identity and each omitted control has an independent mutation test.

Exploratory tier-8 profile (three accepted cold-prefix steps, not an onset run):
42 producer commands. EOS mean 553.579 ms, target terms mean 112.951 ms;
intermediate identity kernels are now microseconds. The producer command mean
is 683.504 ms, **not** the tens-of-ms goal. The residual is compensated
thermochemistry and interval arithmetic, including repeated fixed-endpoint
enthalpy evaluations, rather than full intermediate payload sealing. No interval
proof or tolerance was removed to meet a timing target. Exact-commit repetitions
and their evidence manifest are a separate cost gate; these exploratory numbers
are not a production-speed or focusing verdict.

## Pinned byte format

Format namespace: `rise-payload-sha256-merkle`, `digest_version = 2`.
Chunk size is exactly 4096 bytes; fan-in is exactly 16. These are format/layout
constants, not physical or numerical tolerances. All integers below are unsigned
big endian. Concatenation has no padding except SHA-256's own internal padding.

| Node | SHA-256 preimage, in order |
|---|---|
| Leaf | ASCII `RISELEAF`; version u32; complete payload length u64; zero-based leaf index u64; this leaf length u32; bytes |
| Interior | ASCII `RISENODE`; version u32; complete payload length u64; level u32; zero-based index at this level u64; actual child count u32; ordered 32-byte child hashes |
| Root | ASCII `RISEROOT`; version u32; chunk size u32; fan-in u32; complete payload length u64; leaf count u64; tree height u32; final 32-byte node hash |

An empty payload has one empty leaf. Interior levels start at 1; a single leaf
has height 0. Partial last leaves/parents use their actual lengths/counts; no
duplicated last-child padding. CPU workers and Metal dispatch width never enter
the preimage. Each worker writes a disjoint node; every parent consumes children
in fixed index order. Device tree levels stay private, with one terminal 32-byte
root read. This is collision-resistant full byte binding, not a claim of
mathematical injectivity from arbitrary payloads to 256 bits.

This namespace is distinct from historical `acceptedStateDigestVersion_ = 2`,
which identifies the r148/r164 64-bit fast token digest. Neither historical
version numbers nor stored v1 payloads are reinterpreted or rewritten.

## Bridge and qualification

`tools/fire_payload_merkle.py` independently specifies the format with hashlib,
verifies legacy whole-file SHA-256 and strict new format/version/shape metadata,
and creates an exclusive, detached-seal bridge. The bridge pins historical commit
`c22e301d8482a8ceaff8ab5386f88c49803dcc7e`, requires each evidence file to equal its
tracked blob, and includes all 280 tracked r180–r203 calibration artifacts plus
the unchanged golden checkpoint. Its v1 SHA is
`1837238425d15d7f33aca4ce697202e31042a924da8a06065581851e1a6fa54f`;
its v2 root is `69f6e2b65e98a41b33c12c7dbde4756abb74edaced10924226c778a88b59ee61`.

Golden checkpoint: 125529225 bytes; original whole-file SHA-256
`1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947`;
new Merkle root `120679f1b06a527e6a21dad618ffb54789020e4f6cf55ff86b09d1ac00aaf65a`.
CPU and Metal independently agree with the bridge on the complete file.
The exploratory single device command took 3.733 ms, including its upload/copy
and terminal-root copy; this is **not** producer-stage p95 or owner step cost.

The CPU and Metal fixtures each test 18 independent hashlib vectors at execution
widths 1, 3, 8, 32, 64, 256. Cases straddle SHA padding, 4096-byte leaves,
16-child parents, and the next tree level. Bit flips on both sides of every chunk
edge and the last byte change the root and still match the CPU implementation.
The Python REDs additionally reject wrong version/format/chunk/fan-in/length/root,
appended bytes, reordered chunks, and all eight bit positions at selected edges.
Invalid CPU pointers and invalid dispatch widths refuse with empty results.
Review round 1 found an allocation-failure encoder-lifetime defect and two bridge
validation gaps. The encoder now checks allocation before it is created; Metal
validation-enabled REDs inject refusal at all three private levels of a 17-leaf
tree. Bridge integer metadata rejects bool/float aliases, and the historical
inventory is pinned to the preservation commit rather than the invocation's
HEAD. A temporary-repository RED commits a mutation and then a deletion; both
refuse against that baseline. The existing checked-in bridge remains unchanged.
The standalone CPU lifetime/parallelism fixture is also run under ASan/UBSan.

Reproduction:

```sh
python3 tools/fire_payload_merkle.py --self-test
make -C build/make/rise -j8 build-test/FireSequenceTest
./bin/tests/FireSequenceTest --fire-production-payload-merkle-cpu
./bin/tests/FireSequenceTest --fire-production-payload-merkle-metal
./bin/tests/FireSequenceTest --fire-production-payload-merkle-file /Users/aravind/Working/RISE/rendered/fire_methane_capstone/tier10.run.checkpoint
clang++ -std=c++17 -O1 -g -Isrc/Library -fsanitize=address,undefined -ffunction-sections -fdata-sections -Wl,-dead_strip tools/fire_payload_digest_sanitizer.cpp src/Library/Utilities/FireProductionAdvection.cpp src/Library/Utilities/RISECBOR64.cpp -o /private/tmp/r204-payload-sanitizer
/private/tmp/r204-payload-sanitizer
```

Metal qualification requires the device-visible execution context. Historical
bridge and logs live in `rendered/fire_production_calibration/r204_digest_v2/`.
The public Metal digest entry is a byte-hash qualification utility; it does not
mint an EOS/flux/target/owner authority from a CPU-provided value.

Self-audit risks: framing/padding errors (independent vectors and packet reconstruction); last-child or
parallel-order omissions (boundary mutations/width sweep); private-tree lifetime
(one retained command with tracked resources); version confusion (explicit
format plus strict verification); overstated speed/placement claims (separate
hash-only observation and explicitly unmet producer-speed target).
