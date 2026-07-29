# Render Preparation Lifecycle — Mutation Tracking, Freeze, and Artifact Publication

**Status:** DRAFT, extracted 2026-07-28 from `FIRE_SMOKE_DESIGN.md` §8.
**Scope:** repo-wide RISE concern, not fire-specific.

## Why this is its own document

The fire/smoke arc needs one narrow guarantee from this area: **per-frame
volume-grid swaps, majorant rebuilds, and emission-CDF rebuilds must happen
between renders, never during one** (`FIRE_SMOKE_DESIGN.md` §10.3), which in
turn needs a prepared-input/freeze seam on the render entry points. Answering
that question thoroughly produced a general design for scene-mutation tracking,
render-preparation freezing, and transactional artifact publication that
applies to *every* RISE render, fire or not — camera moves, material edits,
animation, AOVs, movie finalization.

Keeping it inside the fire design made a fire document own a renderer-wide
lifecycle contract. It is extracted here verbatim so it can be reviewed,
scheduled, and implemented on its own merits. The fire design keeps only the
requirement it actually depends on and points here.

**Relationship to the fire arc:** `FIRE_SMOKE_DESIGN.md` Phase C depends on the
prepared-input/freeze seam and the between-renders mutation rule described
below. It does *not* depend on the artifact publication state machine, the
cohort/marker scheme, or the crash-recovery journal; those may be scheduled
independently.

---

## Extracted specification

Scene identity for a file-loaded job is a sorted array of
`{role,logical_locator,sha256}` for the top-level scene and every external asset
byte stream actually opened, where each digest covers the exact opened bytes.
Locators are UTF-8 NFC with `/` separators and the array is lexicographic by
encoded `(role,logical_locator)` bytes; duplicate pairs with the same digest
collapse and a duplicate pair with different bytes is an error. This arc does
not permit the initially opened bytes to stand in for later state. Phase C adds
one `IRenderMutationSink` owned by Job/Scene and installs it into **every built-in
render-affecting manager item/asset on insertion and the active rasterizer,
sampler/config override, FrameStore, rasterizer-output, and encoder graph**. All built-in mutators—including
objects obtained as mutable pointers through `IManager::GetItem`, every
`IBasicTransform` path, painters/materials/lights/cameras/geometries, manager
replace/remove, `RegenerateData`, IJob, editor, parser-finalization, and asset
rebind, `IJobPriv::GetRasterizer()` mutations, SPP/sample overrides,
`AddRasterizerOutput`/`FreeRasterizerOutputs`, callbacks, output routing and
encoder options—must enter the sink before changing state and name their domain.
Scene/entity/asset mutations increment `author_generation` and set sticky
`unrepresented_scene_mutation` unless the editor
atomically commits the matching retained-CST edit and marks that exact generation
represented. Rasterizer/sampling/output/encoder configuration mutations instead
increment `config_generation`; they are allowed before freeze and are represented
by the captured `render_config_v1`, never by clearing scene sticky state. The
animator alone receives a private, unforgeable
`DerivedAnimationUpdate` token: its deterministic nominal evaluation increments
a separate runtime generation/rebuild mask but cannot dirty authored identity.
`Animator::EvaluateAtTime` opens an internal RAII sink scope with that token, so
nested timeline setters and `RegenerateData()` inherit the derived classification
without changing every mutator signature; no other call site can construct the
scope/token. Public callers cannot obtain it.

The actual `RISE_CreateJob → Job::Job/InitializeContainers →
LoadAsciiSceneAuto|ViaCst` lifecycle, including reuse of one Job through
`Job::ClearAll()`, is supported by four private transaction types.
The Job-owned sink exists before the Scene. The constructor opens a
`JobBootstrapTransaction` around `InitializeContainers()`, commits the exact
default-asset/config baseline, records `bootstrap_author_generation`, and marks
the new Job `load_eligible`. It does not claim a file scene identity.
Each public `LoadAsciiSceneAuto` or `LoadAsciiSceneViaCst` first takes a strong
reference to the current `MutationOwnerState` epoch and acquires **that exact
state's** exclusive lease, before any open, classification, parser work, or
`load_attempted` CAS. Under the lease it verifies that Job still points to that
same state and that the state is neither `epoch_revoked` nor `owner_gone`; a
stale waiter returns `mutation_epoch_stale` and never reads or writes the new
epoch. Only then does it atomically CAS that state's `load_attempted` false→true,
compare current author generation with `bootstrap_author_generation`, inspect
sticky state, and establish the `SceneLoadTransaction` starting
generation/scope. A losing CAS returns `load_already_attempted`. Thus CAS,
epoch identity, and eligibility admission are one under-lease critical section;
`ClearAll()` cannot move a paused admission into its replacement epoch. A failed
eligibility check leaves the Job preview-only and the transaction cannot clear
it. The lease is held before the first file open and forbids render or external
mutation entry while open. The transaction opens and reads the top-level scene
exactly once into an immutable transaction-owned
`SceneSourceBlob{logical_locator,bytes,sha256}`. Auto classification consumes
that blob and calls a private `LoadAsciiSceneViaCstImpl(blob,transaction)`;
direct ViaCst reads one blob and calls the same private implementation. Neither
path calls the other public entry, nests a load transaction, reopens the
top-level file, or classifies bytes different from those parsed, retained, and
digested. Failure to open, classify, or parse consumes the attempt and requires
`ClearAll()` for another load. Load-time mutations remain authored and increment author generation, but
sticky-state publication is deferred. Only after the entire CST derives
successfully, all external assets are opened/enumerated, and the current
`RISE-CST-CANON-v1` plus asset identity is built does one atomic commit set
`represented_author_generation=author_generation`, install that identity, and
leave the sticky bit clear. A failed/partial load is discarded or remains
unrepresented preview state; the transaction can never clear a sticky bit that
predated its starting generation. IJob calls outside this private transaction
remain ordinary unrepresented author mutations. Gate the repository's existing
`RISE_CreateJob → LoadAsciiSceneAuto` and `ViaCst` routes, plus pre-load mutation,
second-load, parse-failure, failed-open attempt, mid-load render attempt, and
mutate-after-commit. A replace-at-classification test hook atomically swaps the
path after the blob read and proves Auto still derives and hashes the original
bytes; direct ViaCst and Auto must produce identical retained-CST identity from
the same blob. A second barrier test pauses the loader before exclusive-lease
acquisition while a manager/item mutator enters; after the mutator completes,
the under-lease eligibility check must reject the load as unrepresented. A third
pauses after capturing epoch E, runs `ClearAll()` to install E+1, and proves the
stale loader rejects without setting E+1's `load_attempted` while one legitimate
E+1 loader may proceed.

`ClearAll()` must open a private **JobResetTransaction** before destroying any
tracked container. It acquires the sink's exclusive lease, increments a
monotonic `job_epoch` (generation counters are never reset), revokes every old
capability/freeze token and scene identity, detaches the old graph, and rejects
subsequent sink entry from stale externally retained object pointers. The sink
core is a reference-counted `MutationOwnerState`, not a raw Job pointer: every
tracked object holds a strong reference to its epoch state. `ClearAll()` marks
the old state `epoch_revoked`; Job destruction first takes its exclusive lock
and atomically marks it `owner_gone`, then may release Job/Scene storage. The
tombstone remains alive until the last retained object releases it, and later
mutation returns `mutation_epoch_stale` or `mutation_owner_gone` without
dereferencing freed storage. Destruction waits for any mutator that already
entered the state, so revocation and mutation are race-free. It then
creates the new Scene/managers under a fresh epoch-bound sink scope and runs the
same `JobBootstrapTransaction` to establish a fresh default baseline and
per-epoch `load_eligible`/`load_attempted=false`. A new file may therefore load
predictively after a clean reset, but no generation, sticky bit, identity, or
tracked object from the discarded epoch can qualify the new graph.

The editor's `ClearAll()`-then-rederive path is not treated as an unrelated new
file. It opens an **EditorRederiveTransaction** carrying the exact already
represented retained-CST snapshot across the epoch change, reopens and hashes
every asset byte stream actually consumed, then atomically derives and commits
the resulting current canonical identity in the fresh graph. Stale prior asset
digests may not be copied forward. Entry is forbidden if the old epoch had an unrepresented authored
mutation or if the retained snapshot/generation does not match; failure leaves
the new epoch preview-only. Tests cover GUI clear/open/clear/open, variant/full
rederive, stale-pointer mutation after reset, retain-object → destroy-Job →
mutate, reset after failed load, and an
attempt to use reset/rederive to clear pre-existing sticky state.

Predictive preflight recursively enumerates every render-reachable manager item/
asset **and the complete active rasterizer/config/output graph** and requires the
`IMutationTracked` capability bound to this exact sink;
legacy/plugin types lacking it cause `untracked_scene_mutability` and are
preview-only. Prepared entry points also replace the legacy raw
`IProgressCallback*` with a reference-counted `IRequestProgressCallback` and
accept a fixed ordered vector of reference-counted
`IPreparedArtifactFinalizer`s. Under one pre-freeze exclusive lease, Job
validates the complete prepared-request input, recursively mutation-tracks each
finalizer/encoder graph, snapshots its effective output-configuration fragment,
and `addref()`s the callback and every finalizer.
The exclusive-to-freeze handoff is atomic; add/remove/replace after it returns
`mutation_frozen`. Those references are held through artifact finalization and
released exactly once on every exit, so concurrent bridge destruction or caller
unregister cannot dangle either callback class. The existing non-owning `SetProgress`
pointer is admitted only by legacy nonprepared preview entry points—if non-null
on a prepared request, entry fails with `unsafe_progress_callback` before
workers launch. Freezing a raw pointer is not lifetime ownership.

The request issues one refcounted `IRequestControlHandle` bound to
`(job_epoch,request_generation,freeze_token)` and gives that same handle to
callbacks/finalizers. `RequestCancel(handle)` is the only external
request-control operation permitted while frozen; it atomically sets the token
only if all three fields still name the active request. A stale/foreign handle
is a no-op with `request_not_active`, so a late cancel from request N cannot
cancel N+1. Cancellation after publication commit reports `already_committed`.
Cancellation after the publication CAS but before durable group commit reports
`already_committing` and recovery owns the outcome.
It does not enter the mutation sink and cannot change pixels already committed
or any captured configuration. Callback unregister/destruction,
replacement, SPP changes, and every other control mutation remain fail-fast.
Before acquiring the freeze, every prepared entry point also takes a strong
self-reference to Job/`MutationOwnerState` and releases it only after all
finalizers and the freeze have unwound. Releasing the caller's last Job
reference from a callback therefore cannot run the destructor or attempt a
same-thread exclusive upgrade mid-request.
At request start, preparation acquires a sink freeze lease; under
that lease it performs the private-token nominal animation, then re-reads
generations, capabilities, sticky state, and the
current CST/asset identity, captures `render_config_v1` (including the frozen
finalizer fragment) and config generation
only then, and completes
final fidelity preflight. The lease covers the whole user-visible rasterize
request through every still/AOV sidecar or final movie finalization—not merely
worker completion. One animation lease spans all frames/fields; later nominal
animator evaluations use the private derived token under the lease. Built-in mutators require
the exclusive side of that lease. **Every external mutation entry while frozen
fails immediately with `mutation_frozen`; it never blocks and a shared-to-exclusive
upgrade is forbidden.** This includes re-entry from progress, output, encoder,
and finalization callbacks running on the lease-owning thread, eliminating the
otherwise deterministic self-deadlock. Concurrent/direct mutations therefore
cannot race or silently change a predictive render. The provenance records author/config/runtime
generations and the prepared freeze token. Tests mutate a transform through the
const-manager→mutable-item→`IBasicTransform` route, mutate each other built-in
asset family, attempt a concurrent mutation under the freeze, and inject an
untracked plugin object; none may preserve predictive eligibility unnoticed.
The lease owner receives a private main-thread `RenderPreparationUpdate`
capability used only for the named cache/control-plane operations in §10.3
(spatial/light invalidation, media-state swap, TLAS, prepared safe-cache, and guide
rebuild); it cannot invoke authored-value setters and therefore does not deadlock
on its own freeze or alter scene identity.
Worker/frame accumulation uses a separate private `RenderExecutionUpdate`
capability; post-worker FrameStore/encoder/sidecar/movie completion uses
`ArtifactFinalizationUpdate`. Both are bound to the freeze token and permit only
data accumulation/finalization under the already captured configuration—never
SPP, callback, route, container, channel, compression, transform, or encoder-
option mutation. Finalization ownership is concrete: every platform bridge
constructs and registers its `IPreparedArtifactFinalizer` in the prepared input
before the exclusive-to-freeze handoff above; there is no later registration
seam. After workers join, Job invokes the captured finalizers synchronously under an
`ArtifactFinalizationUpdate`, and `Rasterize*` may not return request completion
until they have all succeeded, failed, or acknowledged cancellation. Detached
or post-return movie/sidecar finalization is forbidden. RAII releases every
finalization capability and then the freeze exactly once on success, encoder
error, callback exception, and cancellation; finalizer failure makes the request
follow the required/optional state machine below and cannot leak the lease. The macOS and Windows movie-finalization code
currently after `RasterizeAnimationUsingOptions()` returns moves behind this
owned seam.

Publication has explicit artifact and request state. Every prepared finalizer
descriptor labels its outputs `required_primary` or `optional_derivative`; the
classification and canonical target are captured in `render_config_v1`.
All required outputs (primary plus any configured required AOV/frame products)
form one **required cohort** and receive one group marker. They all stage and
validate before any is published; failure rolls back the whole cohort. Optional
movies/display derivatives run only after that cohort commits, each with its own
artifact marker/state. Their failure returns
`primary_committed_with_derivative_failures` plus the failed labels and never
invalidates the primary cohort. The request state is

> ACTIVE → CANCELLED|FAILED_PRECOMMIT|COMMITTING,
> COMMITTING → COMMITTED|FAILED_RECOVERABLE.

**FAILED_PRECOMMIT** covers worker/callback/required-encoder
failure or staging/validation failure before the publication CAS. Failure and
cancellation race by CAS on the same state: whichever wins is the terminal
result (`failed_precommit` with the captured reason, or `cancelled`); the loser
observes `request_not_active`. FAILED_PRECOMMIT removes staging, restores no
canonical path because publication has not begun, releases handles/lease, and
is distinct from FAILED_RECOVERABLE after COMMITTING.

`RequestCancel(handle)` and the final pre-publication check use one atomic CAS:
cancel wins ACTIVE→CANCELLED, or the finalizer wins ACTIVE→COMMITTING before any
irreversible rename. Once COMMITTING, cancel returns `already_committing`; only
the **durably synced required-cohort group marker** transitions to COMMITTED.
The first of several artifact markers never commits the request. Optional
artifact states are independently PENDING→COMMITTING→COMMITTED|FAILED after the
request is COMMITTED.

Canonical-path authority belongs only to Job. For each captured descriptor Job
exclusive-creates and retains the **sole native seekable handle** inside the
target directory (the later commit takes its interprocess lock). It passes only
a call-scoped, reference-safe `IStagingWriteFacade` plus immutable
execution/encoding inputs to
`IPreparedArtifactFinalizer::EncodeToStaging`; canonical/staging paths, native
handles, duplication/export, sidecar/marker creation, and rename capability are
not exposed. The facade serializes seek/write, tracks every operation already
admitted, and offers no detached/asynchronous-write API. Immediately when
`EncodeToStaging` returns or throws, Job atomically seals the facade, rejects all
new or retained-facade operations with `staging_sealed`, joins every admitted
operation, propagates any write failure as FAILED_PRECOMMIT, flushes, and closes
the sole native handle. Only after that close does Job hash the immutable staged
artifact, construct the provenance sidecar into its own exclusive-created handle, and
alone performs every sync, recovery, canonical rename, and marker operation.
Built-in file/movie encoders are adapted to handle-based I/O. A legacy/plugin
finalizer is default-deny; only audited built-in implementation IDs in the
renderer-build manifest may expose `IManagedStagingFinalizer`. Any finalizer
that owns an output path or lacks that capability hard-errors prepared entry
with `unmanaged_finalizer_io`; it remains available only to legacy nonprepared
preview.

The filesystem transaction is fail-closed and process-safe. Job generates a
256-bit CSPRNG `request_id` when the render request enters ACTIVE, and a fresh
256-bit CSPRNG `tx_id` for each required-cohort or optional transaction (neither
is a Job-local generation). For every transaction, **`cohort_id=tx_id` byte for
byte**. Create every staging/journal
path with exclusive-create, canonicalize parent directory + leaf identity, and
reject duplicate/alias targets across finalizers. Acquire interprocess locks for
all target directories in canonical sorted order and hold them through recovery,
commit, and cleanup. A versioned durable journal records the transaction/request
ID, complete target set, new digests, prior marker bytes/digests, and every
staging/final/rollback and recovery-intent name before relocating anything. The lexicographically
first locked directory is the cohort coordinator and owns the journal and group
marker; that marker references every cohort member across all locked directories.
For canonical artifact locator P, the stable discovery locator for its head
marker is exactly `P + ".rise-artifact-marker-v1"`; the provenance sidecar is
exactly `P + ".riseprov.cbor"`, and its stable recovery-intent locator is exactly
`P + ".rise-recovery-intent-v1"`. The required group-marker locator is exactly
`coordinator_directory + "/.rise-required-cohort-" +
lowercase_hex(cohort_id) + ".v1"`. All components are NFC-normalized before joining. These suffixes,
the recovery-intent suffix, and the `.rise-required-cohort-*.v1` namespace are
reserved: predictive preflight
rejects any configured artifact whose canonical/file identity aliases any
other artifact or any derived sidecar, head-marker, recovery-intent,
group-marker, journal, or rollback locator.

Cross-directory recovery is discoverable from every member. Under the complete
sorted lock set, Job first writes and syncs the immutable coordinator journal,
then exclusive-creates and durably syncs one canonical-CBOR recovery intent at
every member's stable intent locator:
`{schema:1,tx_id,coordinator_journal_locator,coordinator_journal_sha256,
lock_directories:[...]}`, with the directory array sorted canonically. Every
intent must be durable and every containing directory synced **before any
artifact, sidecar, or head marker is relocated**. A reader/writer probes the
stable intent beside each target before locking, unions any recorded lock sets,
acquires the union in canonical order, and re-probes/revalidates after acquiring
locks; this second probe closes the race with an intent installed after the
first probe. It resolves journal recovery to a terminal generation before
touching the target. Intents are removed and their directories synced only
after COMMITTED/FAILED recovery and terminal cleanup. Thus a later transaction
targeting only a non-coordinator directory must discover and finish an earlier
cross-directory transaction first.

Marker linkage is an exact canonical-CBOR tagged union; fields not listed for a
variant are forbidden. A required-member `artifact_marker_v1` is
`{schema:1,kind:"required_member",tx_id,request_id,cohort_id,member_index,
artifact_locator,artifact_sha256,sidecar_locator,sidecar_sha256,provenance_id,
required_group_locator}`. An optional marker is
`{schema:1,kind:"optional_singleton",tx_id,request_id,cohort_id,member_index:0,
artifact_locator,artifact_sha256,sidecar_locator,sidecar_sha256,provenance_id,
primary_required_group_locator,primary_required_group_sha256}`. The canonical
`required_cohort_marker_v1` is
`{schema:1,kind:"required_group",tx_id,request_id,cohort_id,members:[...]}`,
where `members` is the complete lexicographically sorted array of
`{artifact_marker_locator,artifact_marker_sha256}`. Every locator is the
canonical locator defined above, not an implementation-relative path.
`schema` and `member_index` are canonical minimally encoded unsigned CBOR
integers (`member_index<2^64`); every ID and SHA-256 field, including
`request_id`, `tx_id`, `cohort_id`, `provenance_id`, and all digest fields, is
an exact 32-byte CBOR byte string. A required group marker is immutable:
installation uses exclusive-create/no-replace at its tx-derived locator, and it
is retained for the lifetime of every artifact generation that references it
(this baseline performs no group-marker garbage collection).
A reader starting from any required member acquires shared versions of the same
directory locks (or retries if the generation changes), validates artifact +
sidecar against its artifact marker, follows `required_group_locator`, validates
the durable group marker, and proves exact marker-digest membership. Until all
steps pass, the member is unpublished even if its individual marker has already
been renamed. Optional derivatives follow their primary group locator and
digest and are explicitly valid without membership in a new group. Locator
comparison uses the same canonical path/file-identity rules as alias detection.
Canonical-CBOR byte fixtures for all three variants, stale/overwritten
head-marker races, reserved-name collisions, and artifact→head→group discovery
are required gates.

Each artifact, sidecar, staged marker, and journal is closed and durably synced
(`fsync`/`fdatasync`, Windows `FlushFileBuffers`, or a tested platform-equivalent)
before rename. Relocate prior components and install new data; sync every target
directory; rename each artifact marker and finally the required-cohort group
marker; sync directories **again**, which is the publication linearization
point. Only then transition COMMITTING→COMMITTED and perform cleanup in two
durably ordered phases under the full lock set: (1) unlink **all** recovery
intents and sync every intent directory while the coordinator journal and every
rollback component remain durable; only then (2) unlink journal/rollback files
and sync their directories again. A crash during phase 1 leaves any surviving
intent pointing to a still-valid journal; a crash after its directory sync
cannot leave a durable intent whose digest-pinned journal has been removed. The
same intent-first ordering applies after rollback/FAILED recovery. If the platform cannot provide the
required file/directory durability semantics, predictive publication hard-errors
`durable_publication_unavailable` before rendering rather than weakening them.

Recovery does not assume the three old components moved together. Under the
same interprocess locks, startup/pre-output reads the durable journal and old
marker digest set, gathers candidates across final, rollback, and staging names,
and reconstructs a generation only when artifact+sidecar digests match one
marker. If the new group marker is durably valid, keep the new cohort; otherwise
restore every old digest-matching cohort member, or quarantine incomplete files
when no generation validates. This also resolves a caught failure in COMMITTING
to COMMITTED or FAILED_RECOVERABLE. Neither a lone artifact nor an unmarked pair
is valid. The protocol applies to stills, AOVs, frames, and movies.

Public `GetRasterizer()` callers cannot obtain either token and receive
`mutation_frozen` from the exclusive side while frozen. Tests directly mutate live sample
count, callback, add/free outputs, FrameStore route, and encoder settings before
capture and concurrently after capture; provenance must reflect the former and
the latter must not race or change pixels/artifacts. Separate progress,
final-output, and encoder callbacks attempt transform, output-route, and SPP
mutations and must fail immediately rather than hang. Finalization tests inject
success, error, exception, and cancellation and require exactly-one release and
no `Rasterize*` return while a finalizer remains live. They also destroy or
unregister the platform bridge after capture, attempt late finalizer
registration, and verify the owned snapshot/config does not change. Callback tests retain the
owned snapshot while another thread unregisters/releases its caller reference;
the callback stays live through finalization. A raw prepared callback is
rejected, and atomic cancellation remains usable from inside the callback. A
callback that releases the caller's last Job reference must not destroy Job
until request/finalization teardown completes. Request N's retained handle is
fired during N+1 and must be a no-op. Cancellation/error/crash is injected after
each artifact/sidecar/marker/journal sync, each of the three old-component
relocations, every new data/marker rename, and every directory sync; recovery
must expose either the complete prior pair or the complete new marked pair,
never an unmarked partial artifact. A barrier races cancellation against the
ACTIVE→COMMITTING CAS. Two required finalizers exercise first-ready/second-fail
rollback; two optional movie encoders exercise first-commit/second-fail without
invalidating primary. Two Jobs/processes target aliased paths concurrently and
must serialize without cross-pairing transaction data. Required staging,
validation, encoder, and callback failures cover ACTIVE→FAILED_PRECOMMIT and
race cancellation. A malicious/legacy finalizer that writes a canonical path or
substitutes/reopens the staging path must be rejected before dispatch. A
finalizer retains its facade and races a write across return: an operation
admitted before sealing is joined, while every post-seal write returns
`staging_sealed` and the hashed bytes cannot change. A two-directory crash after
relocating only the non-coordinator member leaves an intent there; a later Job
targeting only that member must discover the coordinator journal, recover, and
only then commit. Cleanup crash injection after each individual intent unlink,
each intent-directory sync, and before journal unlink must never leave a durable
intent whose pinned journal is absent. Two disjoint cohorts in one directory prove their random
tx/cohort IDs produce distinct immutable group markers and neither head is
invalidated by the other. A
concurrent reader probes after every individual marker rename/sync, including a
cross-directory member: it may accept only after matching required-group
membership, while an optional singleton validates only through its committed
primary linkage.

For an unmodified file-loaded job, the exact opened-byte array above is the
identity. An editor mutation is predictive-capable only when it updates the
retained CST and the payload additionally embeds the exact deterministic `RISE-CST-CANON-v1`
serialization and SHA-256 of that **current** CST; that versioned serializer
is a RISE-CBOR64-v1 syntax tree preserving top-level/chunk source order while
putting parameters in descriptor order; it uses NFC text, explicit resolved
defaults, binary64 numbers, and the sorted external-asset identity array.
Any author mutation not representable in/currently mirrored to retained CST has
already set the sticky bit; predictive
preflight then fails until the scene is saved and reloaded or a future canonical
programmatic-build record is implemented. Clearing the bit without rebuilding
identity is forbidden and a mutate-after-load fixture is RED. The provenance
stores both final generations, freeze token, and chosen identity form.

This arc does not define a canonical serialization of a wholly programmatically
built scene graph, so **programmatic jobs are preview-only** and carry
`programmatic_scene_unqualified` plus a per-job 128-bit UUID and optional caller
label for disambiguation. Caller-supplied opaque bytes are not accepted as a
predictive build manifest.


---

## Prepared transport-dependency classification

Extracted from `FIRE_SMOKE_DESIGN.md` §10 item 3. The fire arc needs only the
conclusion — an active photon-map or irradiance-cache consumer cannot claim the
prepared immutable-time guarantee, and preparation must not invoke the legacy
photon-regeneration path. The recursive tri-state classifier that decides this
for arbitrary shader/op/CSG graphs is a general RISE facility.

`ClassifyPreparedTransportDependencies(DependencyTraversal&)` virtual to
`IRasterizer`, `IShader`, `IShaderOp`, and `IObject`. It returns a
**tri-state per dependency bit**
(`no|yes|unknown`) for Scene photon maps, SMS, irradiance cache, and nonlocal
SSS; unknown is never coerced to no and hard-errors prepared entry with
`transport_dependency_unknown`. The componentwise join is explicitly
**unknown-dominates-yes-dominates-no** (equivalently retain separate
`has_unknown` and `known_yes` bits); any reachable unknown in any component
rejects before a known yes is interpreted or an execution policy is built.
Ordinary/Kleene boolean OR is forbidden because `yes OR unknown` could mask
an ungoverned plugin. `StandardShader`/`AdvancedShader` join the
results of every child op, nested shader-owning ops forward recursively, and
plugin defaults return unknown. Every built-in leaf-object override joins
its own shader; every built-in container recursively joins all children that
can supply a hit/shader, whether or not the child is independently world-
visible. In particular, `CSGObject` joins its own shader and both operand
object subgraphs: `SetWorldVisible(false)` on an operand never removes it
from dependency reachability. `DependencyTraversal` owns an identity visited
set and recursion stack; repeated DAG nodes are joined once, and a cycle
rejects with `transport_dependency_cycle` rather than recursing or returning
no. An unoverridden/plugin object returns unknown. Job/RayCaster combines the
active rasterizer's dedicated-integrator flags with the default shader and
the classifier result of every TLAS root. The SMS bit therefore includes pure PT,
integrated `PathTracingShaderOp`, and standalone `SMSShaderOp`; the SSS bit
includes the two named §7.2.2 shader ops. Dependency preflight runs this walk
before any TLAS/cache mutation. Active Scene-photon-map
consumption hard-errors `photon_transport_unprepared`; a configured but
unreachable map is skipped and never regenerated. Predictive SMS is already
rejected by §8. In prepared preview, Job derives an immutable
`PreparedExecutionPolicy{requested_sms=true,effective_sms=false}` before
`render_config_v1` capture, without mutating authored/constructor state under
the freeze. One immutable request policy is carried by the existing
per-ray/request `RuntimeContext`, including nested shader calls. That policy
is passed through **both transport surfaces**:
rasterizer `PreRenderSetup` must skip SMS caster enumeration/map build, and
pure/integrated PT plus standalone `SMSShaderOp` must skip every SMS
evaluation and SMS-specific emission-suppression branch. Every Pel, NM, and
HWSS evaluation/suppression sibling reads the same effective bit (even though
predictive fire selects NM). The requested and
effective states plus `sms_unqualified` are recorded.
Active irradiance-cache consumption hard-errors
`irradiance_transport_unprepared`; a dormant configured cache skips the
helper's current scene-wide prepass and remains bitwise unmodified. All
classifier bits are reachability properties, not tests for whether a cache
object merely exists. The appended virtuals follow the public ABI and
all-build-project checklist and have recursive default/object/nested-op,
unknown-plugin, mixed known-yes+unknown sibling/nested, mock-graph
forwarding, hidden CSG-operand photon/irradiance/SSS, derived/plugin object,
shared-DAG, and cycle tests.
Preview use of an active photon/irradiance consumer remains possible only on
a legacy nonprepared static-scene path and cannot claim this arc's
immutable-time guarantee.
