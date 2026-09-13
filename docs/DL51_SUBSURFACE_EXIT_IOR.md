# ~~DL-51: standalone subsurface exit destination~~

CLOSED 2026-09-12 — `34434610`, SubSurfaceExitIORTest:
`Checks: 301  Failures: 0` (unfixed: 36 failures).

## Mechanism

The non-absorbing `SubSurfaceScatteringSPF` inside branch used the current
input-stack top as the destination of an exit, then popped that object only
when constructing the transmitted ray's stack. For a normally nested stack,
the pre-pop top is still the interior IOR. Directions and Fresnel weights
therefore described a different interface from the one carried by the ray.

The exit optics now read a post-pop stack snapshot. A transmitted ray
carries that same state, while a reflected ray retains the original input
state. Both RGB and NM branches follow this contract. SPF weights continue
to exclude the radiance eta-square factor, which belongs to their transport
consumers.

## Reachability and audit scope

This is the standalone non-absorbing fallback. SubSurfaceScatteringMaterial,
RandomWalkSSSMaterial and DonnerJensenSkinBSSRDFMaterial explicitly construct
the SPF with `bAbsorbBackFace=true`; their membership-selected inside branch
returns before this code. The regression includes direct absorbing-flag
controls and runtime controls through the diffusion and random-walk
materials' actual `GetSPF()` implementations. Donner-Jensen construction is
a source audit, not an additional executed material fixture.

DielectricSPF and PerfectRefractorSPF already resolve exit optics from a
popped stack. The SSS front branch reads the incoming exterior stack without
popping, as required. Its inside continuous PDFs remain zero for delta
lobes. TranslucentSPF performs a same-index membership transition without
exit Snell/Fresnel; PolishedSPF is a coating reflection model. Neither has
this pre-pop exit-destination pattern.

PT RGB/NM/HWSS and BDPT consumers use the attached continuation stack;
VCM/MLT share the bidirectional generators. The fix preserves their existing
radiance/importance weighting convention. CompositeSPF follows a child's
attached stack. Supported coated/fabric substrate allowlists exclude SSS.
Random-walk and diffusion non-air conventions remain separate DL-49 work.

## Regression and scope

Terra authored test-only commits `e1af768c` and `82a84036` before the first
execution against the unchanged library at
`ceafc44f3906a4e575e9cc92b54f1d48cf6180e2`. Before execution, the second
commit made scalar/vector assertions reject non-finite values, tightened
deterministic tolerance to 1e-10, completed NM normal/TIR cases, and matched
the NM stack IOR to each wavelength's painter value. No test code changed
between the failing run and production repair `34434610`.

The test uses the real SPF and distinct `StubObject` identity keys, not
geometry intersections or a rendered scene. RGB covers normal incidence,
sin(theta_i)=0.6 oblique incidence, and 70-degree TIR for interior 1.5 and
enclosing 1.33. NM covers those same cases at 450/550/650 nm with matched
interior IORs 1.52/1.50/1.48. A separate RGB air-exit control checks that
popping exposes the ambient root with no object. Assertions cover analytic
Snell directions, Fresnel/complement weights, delta metadata, unchanged
input, retained reflection state and popped transmission state. All these
inside delta cases must consume no sampler draws.

The initial run reported:

```text
Checks: 301  Failures: 36
```

Failures were in exit weights, oblique Snell directions, and TIR behavior.
Stack/sampler and absorbing-material controls passed before the fix. The
same test after repair reported:

```text
Checks: 301  Failures: 0
```

The independent normal Fresnel prediction for 1.5 to 1.33 is
0.003608485559814703; the old equal-index calculation returned zero.
The oblique transmitted sine is 0.6766917293233082, rather than the
unbent input sine 0.6. These values were independently recomputed in the
supervisor's analytical evidence; the C++ expectations use scalar formulae
and do not call the production Optics helpers.

## File status and validation scope

| File | Status |
|---|---|
| `src/Library/Materials/SubSurfaceScatteringSPF.cpp` | Modified: both inside branches share post-pop optics/output state (`34434610`). |
| `tests/SubSurfaceExitIORTest.cpp` | Added: committed-before-execution regression and absorbing controls (Terra). |
| `tests/README.md` | Modified: test scope and measured red/fixed counters. |
| `docs/DEBT_LEDGER.md` | Modified: closure and counts; no new residual row from this slice. |
| `docs/DL04_SSS_RADIANCE_DECISION.md` | Modified: close the standalone exit confound while retaining reachability limits. |
| `docs/DL51_SUBSURFACE_EXIT_IOR.md` | Added: mechanism, reachability, regression and scope. |

No library source files were added or removed, so no build-project source
list updates are needed. Selected gates: SubSurfaceExitIORTest,
SPFPdfConsistencyTest, SPFBSDFConsistencyTest, BSSRDFNormalizationTest,
IORStackBehaviorTest, SSSRadianceScalingTest and SourceHygieneTest. The
standalone completion report records exact gate output, clean make/Xcode
validation, independent reviewer findings and merged-master validation.
