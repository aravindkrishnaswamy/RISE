# DL-03: medium state follows the guided continuation

## Mechanism and scope

Base master is `00bdcef5102224bce02ad24ff674486c12c12833`.
PT reset a guided continuation to the pre-scatter IOR stack; BDPT eye and
light walks skipped the selected SPF stack whenever guiding replaced the
direction. A translucent outward exit therefore lost its pop, and a later
hit on that object was classified as another exit.

When the selected SPF sample supplies a transition, the repair resolves
one borrowed continuation-stack pointer. A replacement
on the same geometric side as the incoming ray crosses the boundary and
keeps the selected post-scatter stack; an opposite-side replacement keeps
the input stack.
Ordinary SPF samples and exact RIS reuse retain their original state.
The comparison uses the unperturbed geometric normal, falls back to the
sampling frame when that normal is absent, and refuses to infer a crossing
from a tangent direction. Normal sign reversal does not affect the result.

The direction distinction is necessary: one-sample guiding can accept an
inward candidate with positive combined PDF and BSDF even when its SPF PDF
is zero. Blindly copying an exit's popped stack would create incorrect
inward state. This change neither rejects/resamples proposals nor changes
PDFs or BSDF amplitudes. Full translucent guiding weights/densities remain
DL-38/DL-41. PT selected-lobe compensation DL-42 and BDPT eye PDF argument
order DL-43 are both CLOSED 2026-09-13 -- see docs/DEBT_LEDGER.md.
Existing unchanged tilted SPF exit behavior is retained and now tracked
as DL-45; this is not a geometric-horizon correction for the sampler itself.

## Red proof

`TranslucentIORStackTest` calls the real trained OpenPGL field, real
TranslucentSPF and PT RGB/NM integrator loops. An observing SPF forwards all
sampling/evaluation to the production implementation. Only the next hit is
controlled so a later same-object Scatter exposes its received stack and
entry/exit classification. It is a deterministic transport-state fixture,
not a rendered scene or a radiance comparison.

Test `6b9a2404` was committed before execution. Two compile/fixture field
corrections (`2783325f`, `d3a5e732`) preceded the first run; no production
code changed. Against the unfixed library, that run exited 1 with four
failed outward-stack assertions. In each of RGB and NM, one-sample guiding
made 45 outward replacements and 371 inward replacements in 512 trials;
all 45 outward replacements lost the pop. RIS made 14 actual outward
replacements and lost the pop on all 512 continuations, including retained
SPF directions. The disabled-guide controls passed. All observed enclosing
medium values remained 1.33, showing why a numeric-IOR-only check would miss
the object-membership defect. The four failure lines are preserved verbatim
in fix commit `013b3a15` and the standalone report.

Ordinary PT translucent entry/backscatter produces specular-classified
arrivals, which disable guiding. The fixture explicitly seeds a diffuse
arrival and asserts positive actual outward substitutions in both guiding
modes; a merely trained scene would not be a sufficient red proof.

## Consumed-field and sibling audit

Pattern: a replacement direction discarded the selected boundary state,
while training and eta consumers independently chose inconsistent stacks.

| Surface | STATUS | Disposition |
|---|---|---|
| PT RGB/NM RIS and one-sample guiding | FIXED | Shared template resolves traceIorStack from the accepted direction. |
| PT radiance eta and training segEta | FIXED / ALIGNED | Both read the resolved pointer before the final value copy. |
| BDPT eye RGB/NM/HWSS | FIXED | Training ray, radiance eta (including HWSS companions), guidingEta and continuation copy share resolved state. |
| BDPT light RGB/NM/HWSS | FIXED | guidingEta and continuation copy share resolved state; importance transport still has no radiance eta factor. |
| Dedicated PT HWSS loop | VERIFIED unaffected | No OpenPGL replacement block; selected stack already copied. |
| VCM and MLT rasterizers | VERIFIED shared code | Reuse BDPT generators, but currently do not configure guiding fields; this does not claim guided render coverage there. |
| PathTracingShaderOp RGB/NM | VERIFIED shared code | Delegates to the corrected PT integrator. |
| Composite/coated/fabric and other SPF wrappers | VERIFIED no separate guide replacement | Child state reaches the shared walk; no second stack-substitution implementation. |
| Selected stack lifetime | VERIFIED unchanged | Loop-local scattered container owns it until the final value copy. Training calls are synchronous; no borrowed pointer is stored on a vertex. |
| SMS modes | INAPPLICABLE to replacement | No path-guide direction substitution in their stack handling. |

The helper's boundary tests in MISWeightsTest complement the production
integration fixture: incoming-side crossing/reflection, tilted sampling frame, reversed
geometric normal, missing-normal fallback, tangent cases, exact reuse and
no selected transition. No library source files were added or removed.

## Verification and remaining limits

Per-class grep selected 23 existing test binaries. The changed shared
PathTransportUtilities adds MISWeightsTest, and the recipe adds the extended
TranslucentIORStackTest. These 25 complete binaries are run individually;
no full-suite runner is used. Render gates run sequentially with nohup logs.
Necessary harness-only edits seed each render, add missing explicit box
filters and replace removed capture-output placeholders with relative
linear EXR. Assertions, tolerances, case selection and scene physics stay
unchanged. The dark spectral no-curve control in VolumeAbsorptionAttenuationTest
subsequently failed its existing gray-color bound (88 passed, 1 failed).
Its unguided delta-interface scene cannot execute this repair. Eight
independently seeded diagnostic trials at each of 256 and 4096 spp support
sparse spectral/survival sampling as the cause. Only that control now uses
4096 spp; its optical depth and assertion are unchanged. Full measurements
and the failed gate log are retained in the standalone report.
Functional agent PNG tests retain their encoded-image API
contract; seeds are applied at each outer request. Those PNG assertions
are not presented as linear radiance measurements.

Final counters, independent reviews, master validation and cleanup are
recorded in the standalone report after completion.

The expanded fixture passed as `ALL TESTS PASSED` on the fixed library.
BDPT additions first ran after the fix and are additional coverage, not
an independent unfixed red proof. The first BDPT attempt excluded the exit
from its exclusive guiding-depth bound; correcting that fixture exposed
the known eye-RIS DL-43 limitation. Logs retain both incomplete attempts.
The final fixture requires actual outward substitutions in PT both modes,
BDPT eye one-sample and light both modes; it requires inward controls in
one-sample guiding. Eye RIS records retained SPF directions and requires
more exit PDF queries than the unguided baseline, proving guide-candidate
evaluation without claiming successful guide-direction replacement.

**Superseded 2026-09-13**: the preceding paragraph's "Eye RIS records
retained SPF directions ... without claiming successful guide-direction
replacement" described the state while DL-43 was open. DL-43 is now
CLOSED (`a69c9ce6`) -- the same `RunBDPT` fixture (extended further, see
docs/DEBT_LEDGER.md DL-43) now shows eye+RIS achieving actual outward
guide-direction substitutions (`substituted_out>0`) with a correctly-valued
density, not merely a live query count. This paragraph is retained
verbatim as the historical record of what DL-03's own closure proved at
the time; it no longer describes eye RIS's current behavior.

Self-audit risks: vacant guiding paths, blindly popping inward replacements,
training/eta reading a different state, dangling borrowed stacks, and
claiming BDPT fixed-code coverage as red proof. Positive substitution and
query counters, inward controls, the consumed-field audit, loop-local
ownership and explicit test-history distinctions address those risks.

## Tilted-frame correction and newly observed residual

A second audit found a flaw in first repair `013b3a15`: using the selected
SPF ray as the side anchor newly misclassified replacements when that SPF
exit was itself geometrically inward. Exact boundary test `21cf2cec` failed
two assertions, and real trained tilted-frame PT test `47ebdf86` failed
eight assertions against that repair. In each RGB/NM one-sample run, 234
outward and 583 inward replacements following an inward SPF sample got
the wrong stack; corresponding RIS counts were 42 and 11. All positive
coverage guards passed. `8a9bdb18` anchors the decision to the incoming ray
instead. Both red logs and their failing lines are preserved in the fix
commit and report. The expanded test separates unchanged SPF directions
from actual replacements, so it does not silently demand an unrelated
sampler correction from DL-03.

The same fixture observed 1,021 unchanged geometrically inward SPF exits
in 4,096 unguided trials in each RGB/NM run at a 60-degree shading-normal
tilt. The real sampler still attaches its popped stack. This is a separate
observed defect pin, not a failing correctness assertion or an image-bias
measurement. New DL-45 tracks its exit sampling/PDF/energy policy; changing
the sampler is outside this state-propagation repair.

The final expanded regression passed against `8a9bdb18` with test refinements
through `f6aa67fd`: `ALL TESTS PASSED`, exit 0, warning-free test build.
All recorded stack-error counters are zero, including the tilted witnesses
above. New observed medium values must be finite as well as equal to the
expected enclosing IOR.

## Initial-containment residual

Following the test's explicit initial state one step further found separate
DL-46. TranslucentMaterial inherits IMaterial's invalid/non-refracting
GetSpecularInfo default, while IORStackSeeding::SeedFromPoint admits only
valid canRefract materials with positive IOR. Thus origins already inside
a closed translucent object are not seeded with its membership; both
Scatter variants interpret the first physical exit as entry because they
read only containsCurrent. PT camera and BDPT eye/light initial seeds share
this path. This is static evidence with a dedicated origin-inside red proof
pending, not an observed image result. Membership tracking must be separated
from specular-refraction capability; this repair does not relabel translucent
materials as specular. DL-03's seeded PT and actual-entry BDPT tests remain
honest propagation fixtures and do not claim to close initial containment.


## Guide-created entry residual

Fresh transport review identified DL-47, a separate missing-state producer
contract. At an outside translucent entry the selected diffuse reflection
has no IOR-stack transition. An inward guide direction can nevertheless
have positive TranslucentBSDF transmission and positive combined PDF; the
one-sample PT and BDPT paths accept it while retaining absent membership.
The next physical exit is therefore interpreted as entry. This behavior
already exists on the base revision; preserving an available selected exit
transition cannot manufacture a different lobe's missing entry state.
The helper intentionally leaves null selected stacks unchanged.

Static witness: incoming direction +Z, geometric/shading normal -Z,
positive front reflectance and transmission, a selected outward reflection,
and an accepted inward guide. The BSDF's cross-side branch is positive;
the front SPF Pdf is zero there, but the guide term keeps the combined PDF
positive. The null-stack return retains the outside stack. This is source
reasoning, not an executed entry regression or measured image error.

DL-47 needs material-aware state generation for the accepted direction,
including ambiguous mixtures; guessing a generic push or rejecting a
positive proposal without accounting for its probability would be an
incorrect local shortcut. It is distinct from DL-46 initial containment
and from changing a PDF alone. Current PT tests start after entry, and
BDPT tests select actual SPF entry transmission, so neither is claimed as
coverage of this missing entry-state case. DL-03 closes preservation of
available selected exit state, not all guided material-state evaluation.
