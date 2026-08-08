# Fire output-provenance pin v1 — §8 completion proposal (draft; ratify in design loop)

**Status: DRAFT PIN — implementation may proceed against it; formal
ratification is a design-loop edit the owner routes (r48+).** This document
completes four §8 output-provenance details the design references but does
not pin, each derived by extending an *existing* §8 pattern rather than
inventing one. If the design loop lands different semantics, the design
wins and the implementation follows it. Compiled 2026-08-08 at the
implementation's eighth provenance stop.

## P-1. `artifact_fidelity` for a preview primary

§8 defines `predictive_primary` and `display_derivative` only. A Phase-A
preview render's lossless primary artifact is neither.

**Pin: add `preview_primary`.** Semantics identical to `predictive_primary`
in every artifact-level respect — lossless, raw pre-exposure NM/Pel data,
empty `artifact_reason_codes`, sidecar required (its absence is
`output_provenance_unavailable` on the render, not a silent skip) — with
`render_fidelity_status=preview` carrying the render-level reason codes.
Rationale: the preview/predictive split is a *render* property (§7.0); the
primary/derivative split is an *artifact* property (§8). The two axes are
orthogonal, so the artifact enum needs the third member; overloading
`display_derivative` for a lossless preview primary would break the
derivative-linkage schema (a primary has no `derived_from_*`). MOV/display
outputs of preview renders remain `display_derivative` linked to
`preview_primary` frames.

## P-2. `active_fire_media` entry for statically authored media

§8's array fields (sequence ID, base-frame index, whole-file digest,
`source_kind`, `physical_mapping`, blur state) are all manifest-derived,
and §8 separately states a statically authored medium
(`multichannel_heterogeneous_medium`) "has no manifest and therefore no
declared `source_kind`" and is `producer_unqualified` by construction.

**Pin: a tagged variant, following §8's own tagged-schema precedent**
(derivative linkage: "a tagged schema, not singular by assumption").
`media_kind = sequence_backed | static_authored`; fields are structurally
present or absent by variant — never null:

- `sequence_backed`: exactly the §8 fields as written.
- `static_authored`: `(manager_name, binding_kind, binding_owner)` key as
  for all entries, plus `authored_config_digest` (SHA-256 over the
  canonical encoding of the medium's resolved authoring parameters —
  channels, painters/bakes, dimensions, transforms — computed at freeze;
  two different authored media must never share a digest, the §8
  metadata-completeness rule applied to authoring) and
  `optical_record_ids` (the versioned records the medium consumes). No
  sequence/frame/digest/blur fields exist in this variant; `source_kind`
  and `physical_mapping` are likewise absent — the variant tag itself
  carries what they would say, and the render carries `producer_unqualified`
  per §8. Blur: statically authored media have no velocity channel and
  never blur; a blur request against one is the existing
  capability-preflight rejection, not a sidecar field.

## P-3. Canonical schemas for resolved render configuration and build identity

§8 pins the *categories* (film/camera/integrator/sampler/depth/clamp/
filter/AOV/output; source revision, dirty flag, feature and dependency
versions) and the enforcement rule ("a render-affecting parameter added
without a schema update is a test failure"), not the field list.

**Pin: the exact enumerations are implementation-defined schema-v1 under
those categories**, encoded as independently schema-versioned
RISE-CBOR64-v1 maps (the §8 record pattern), with the enforcement rule as
the ratchet: the round-trip test enumerates the render-affecting parameter
surface, and any parameter that can alter the image without appearing in
the schema is a test failure. Build identity follows §8's existing
`producer_build_v1` field list (source revision, executable/module SHA-256,
dirty flag + dirty-diff hash, schema/solver and gate-harness versions,
compiler and FP/fast-math/contraction settings, target platform, dependency
versions + loaded-binary hashes) applied to the *renderer*; name it
`renderer_build_v1` with `renderer_build_id = SHA-256(exact bytes)`.

## P-4. Provenance-ID preimage and EXR attribute mirroring

**Pin the preimage by §8's one-preimage pattern** (sequence_id: "payload
contains no sequence_id; sequence_id is SHA-256 over the exact canonical
byte encoding of payload alone"): the sidecar's top-level envelope is
exactly `{payload, provenance_id}`; `payload` contains every semantic
sidecar field including `artifact_sha256` and `schema_version`, and no
`provenance_id`; `provenance_id = SHA-256` over the exact canonical
RISE-CBOR64-v1 encoding of `payload` alone. (The artifact's own bytes
exclude the sidecar entirely, so including `artifact_sha256` in the
preimage is safe and binds the ID to the artifact.)

**EXR attribute mirroring**: string attributes under the reserved prefix
`riseFireProv_`, one per top-level payload field, each value the canonical
JSON text of that field (`riseFireProv_provenance_id` carries the ID;
`riseFireProv_schema_version` the schema). Attributes are a *convenience
copy*: the sidecar is authoritative; a mismatch between attributes and
sidecar is a verifier error; formats without attribute support write the
sidecar only, which is why §8 says "where supported". `artifact_sha256` is
mirrored like any other field — the attributes live inside the artifact
whose hash they'd describe, so the EXR-embedded copy of `artifact_sha256`
necessarily describes the *pre-attribute* artifact bytes; to keep one
preimage, the pin is: `artifact_sha256` is computed over the finalized
artifact bytes **with all `riseFireProv_` attributes excluded from the
hashed byte stream** by the verifier (deterministic attribute-stripping
canonicalization), and round-trip tests must cover it. If the
implementation finds attribute-stripping canonicalization fragile in
practice, the fallback pin is: do not mirror `artifact_sha256` (mirror all
other fields), keeping the sidecar as its only carrier — choose one,
record which, and test it; both are valid readings of §8's "where
supported".

---

Ratification checklist for the design loop: adopt P-1's enum member,
P-2's tagged variant, P-3's delegation rule + `renderer_build_v1`, and
P-4's preimage; resolve P-4's mirroring choice if the implementation has
not already reported which branch it took.
