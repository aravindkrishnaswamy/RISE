# DL-03: medium state follows the guided continuation

## Mechanism and scope

Base master is `00bdcef5102224bce02ad24ff674486c12c12833`.
PT reset a guided continuation to the pre-scatter IOR stack; BDPT eye and
light walks skipped the selected SPF stack whenever guiding replaced the
direction. A translucent outward exit therefore lost its pop, and a later
hit on that object was classified as another exit.

The repair resolves one borrowed continuation-stack pointer. A replacement
on the same geometric side as the selected SPF ray keeps that ray's
post-scatter stack; an opposite-side replacement keeps the input stack.
Ordinary SPF samples and exact RIS reuse retain their original state.
The comparison uses the unperturbed geometric normal, falls back to the
sampling frame when that normal is absent, and refuses to infer a crossing
from a tangent direction. Normal sign reversal does not affect the result.

The direction distinction is necessary: one-sample guiding can accept an
inward candidate with positive combined PDF and BSDF even when its SPF PDF
is zero. Blindly copying an exit's popped stack would create incorrect
inward state. This change neither rejects/resamples proposals nor changes
PDFs or BSDF amplitudes. Full translucent guiding weights/densities remain
DL-38/DL-41, PT selected-lobe compensation DL-42, and BDPT eye PDF argument
order DL-43. Existing tilted SPF exit behavior is retained; this is not a
new geometric-horizon correction for the sampler itself.

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
integration fixture: same/opposite sides, tilted sampling frame, reversed
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
unchanged. Functional agent PNG tests retain their encoded-image API
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

Self-audit risks: vacant guiding paths, blindly popping inward replacements,
training/eta reading a different state, dangling borrowed stacks, and
claiming BDPT fixed-code coverage as red proof. Positive substitution and
query counters, inward controls, the consumed-field audit, loop-local
ownership and explicit test-history distinctions address those risks.
