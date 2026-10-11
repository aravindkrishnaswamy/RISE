# DL-531 design: shading-only uniform colour keyframes

Written before implementation, against master da81a949e. Exact first subset:
UniformColorPainter (exact dynamic type), all colour/radiance/spectrum queries
forward to a per-thread clone. Materials keep immutable painter references;
queries of an animated material colour resolve through that painter. Other
painter types and direct custom material keyframes remain refused.

Safety guard: there must be no registered observers on the animated painter,
and every scene object must be an exact Object with audited painter-independent analytic
geometry (sphere, box, clipped plane, or a null hierarchy node). CSG,
displaced/mesh/SDF/custom geometry, intersection modifiers, and runtime-data
shaders refuse the frame.
Materials must also be exact LambertianMaterial, NullMaterial, or an exact
LambertianLuminaireMaterial wrapping that same audited subset. Unknown material
caches are not proven safe merely because the painter itself is cloned.
Audited material colour bindings must be exact UniformColorPainter objects;
animated emission/environment bindings and any participating media refuse
this first subset, avoiding cached source distributions and unknown painter
graph consumers. This is shading reflectance only.
This deliberately rejects some safe scenes: it is a structural proof that
no geometry/displacement consumer needs the shared painter or cache refresh,
not a heuristic name/binding census. Geometry APIs gain no new virtuals.

Clone construction copies cached spectra exactly, creates a fresh Reference
and Observable, and does not copy observers. Slots are set before workers
and cleared after they join. SetIntermediateValue mutates only the clone;
queries use the existing TimeIndexed TLS slot table. Shared setters outside
the pass keep their old behaviour. Dynamic subclasses do not opt in.

Tests planned: a shading-bound RGB colour timeline (the old fallback fixture's
unused painter does not prove shading), one-thread OLD/TI hashes in RGB,
NM/HWSS across PT/BDPT/VCM, signal with timeline removed, guard refusal for
observer/unsafe geometry, full named animation gates and ThreadSanitizer.
Performance: preserve baseline executable before changing library, alternate
OLD/TI n8 distinct ValueSalts on 192x192 shading-keyframe PT with reserve0.
No build/other own render overlaps timing. Report timings and SD, not an
unsalted single-run speed claim. Static scenes retain their query arithmetic.

## Correctness evidence and self-review

The saved master-production executable with the new harness fails 24 frame-creation checks (50 passed / 24 failed, rc1). Current production passes 74/0, rc0: six PT/BDPT/VCM RGB/spectral pipelines times four ValueSalts, all 24 one-thread pixel pairs bit-identical. The difference in hash comparisons is deterministically zero (n24, SD0); this is not a radiance confidence interval. The bound-painter versus static-painter signal changes pixels. The focused eligibility checks pass 2/0, including registered-observer refusal. All four named builds returned0 without compiler warnings.

Self-review checked slot setup/reset around worker lifetime, fresh unslotted clones and independent observers/refcounts, cached-spectrum copying, query forwarding in RGB/NM/spectral/source accessors, and refusal of geometry/displacement, unknown material caches, emission/environment and medium consumers. The first subset changes material reflectance through immutable bindings; it does not clone arbitrary material implementations. More permissive consumer analysis, nonuniform painters and custom material targets remain open. Full animation gates passed: TimeIndexedMotionBlurTest96/0, AnimationRasterizerParityTest102/0 and MotionBlurTimeAverageTest77/0; all named builds and runs rc0, warning-free. Whole RISE library TSan instrumentation (375 translation units, external OIDN/OpenPGL dependencies uninstrumented) built/linked rc0; four shade_big PT8spp four-thread ValueSalts53190..53193 ran rc0 with halt_on_error=1, no race report.

## Interleaved enablement measurement

Alternate baseline/fix order, reserve0, eight ValueSalts56400..56407, shade_big192x192 PT8spp. Old fallback uses one thread, new time-indexed path four: .652752552s SD.007374194 -> .238268479s SD.036627646, paired2.800017064x SD.454521057. This measures enabling parallel shading-keyframe rendering, not one-thread algorithmic speed. Old frames0 and new frames1 in all pairs. Multi-thread hashes differ (expected scheduling/sample differences); the required one-thread24-pair bit identity is a separate check. Mean radiance .135230614 SD.000812927 versus .134794339 SD.000407816, paired difference-.000436275 SD.000996591 within3SE.001057044. No other own build/render job overlapped timing.
