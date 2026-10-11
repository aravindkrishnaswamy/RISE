# DL-482 batch 12 gates

The ordinary finite SSS surface reflection had mask zero at a connection endpoint, while the SPF labelled an interior draw reflection. Diffusion, random-walk SSS and Donner–Jensen skin now declare reflection and expose the complete finite RGB/NM BSDF in that slot. The aggregate BSDF, sampling, densities, contact code and independent translucent jump charging are unchanged. See [design](BATCH12_DL482_DESIGN.md).

The pre-fix build is local master `72beda3ab` plus the new tests. All named `make -C build/make/rise -j8 build-test/<Name>` commands returned 0; the final library build emitted no warnings. Generated fixtures live under tests, not shipped scenes.

## Exact and angular gates

* `SSSReflectionCapCloseoutTest --strict-reflection-pin`: two strict failures become zero. For both measured materials, interior `1/Over` versus endpoint `0/OK` becomes `1/Over` versus `1/Over` (deterministic metadata, n1/SD0).
* `ConnectionTypeSplitTest --dl482-only`: pre-fix 1 pass / 30 failures. All three material declarations, RGB/NM sum identities and native labelled SPF expectations now pass. Each angular comparison uses 400,000 draws versus independent quadrature at 0, 40 and 70 degrees, RGB channels 0/2 and NM550. Example diffusion normal RGB0: MC .038917, SE .000010, quadrature .038938. At 70 degrees: MC .122665, SE .000090, quadrature .122624. These are within the existing statistical gate. The final isolated rebuild/run returned0: 55,462 passed, 0 failed. The full ConnectionTypeSplitTest returned0: 431,782 passed, 0 failed.

## Binding glossy cap

`RISE_DL351_ROWS=S RISE_DL351_REPEATS=8 bin/tests/BDPTDepthCapMISTest`: roughness .4 receiver over a matte floor, glossy cap0, translucent cap0 isolates ordinary reflection. PT is the independent reference. Seeds are salted by `SobolSamplerTestHooks::ValueSalt`. All comparisons use three combined standard errors with no additional tolerance floor. Pre-fix 5 passes / 3 failures; post-fix 8 / 0. The pre-fix failures are VCM, not BDPT; the exact parity pin discriminates endpoint counting independently of rendered significance.

| Mode/material | PT mean / sample SD | BDPT after mean / SD | VCM before → after mean (SD before → after) |
|---|---|---|---|
| RGB diffusion | .126967 / .000322 | .126790 / .000469 | .127666 → .126948 (.000421 → .000412) |
| RGB random-walk | .126840 / .000153 | .126775 / .000405 | .127790 → .127065 (.000651 → .000656) |
| HWSS diffusion | .126846 / .000384 | .126615 / .001171 | .127562 → .126820 (.000526 → .000511) |
| HWSS random-walk | .126998 / .000496 | .127286 / .001133 | .127883 → .127167 (.000739 → .000742) |

Every table entry uses n8. These are radiance comparisons, with no timing claim.

## Controls and prior positive-jump gates

* `RISE_DL482_CONTROL_ONLY=1 RISE_CAP_HASH=1` on row S: 12/0; all 96 raw RGBA hashes equal across pre/post builds. Within each build, all 48 cap-free versus nonbinding-cap100 pairs equal. Four salts per material/mode/integrator; per-pixel difference SD0.
* Row L with `RISE_DL482_POSITIVE_PIN=1`, n8: 53/0. Existing RGB/HWSS diffusion/random-walk caps0/1/2 and exact positive-jump accounting pass without changes to jump transport.
* `SSSExteriorIndexInvarianceTest`: the full n4 run produced 517/1. The only failure was a precision guard: random-walk PT-spectral, eta1.128, meanH .997772, SD .00720451 (n4), whose three-SE precision exceeds its unchanged 1% band. Its physical comparison passed. The n8 run of that fixture has meanH 1.00102, SD .00447942, three SE .0047512, and passes the unchanged precision and physical bands. The full n8 suite returned0: **706 passed, 0 failed**, with no band or gate edits. Rough diffusion BDPT8192 enclosed/air ratio 1.00028 (air SD .00000950526, enclosed SD .0000109425), within the unchanged .006 band.

## Self-review

Checked that the five-slot split sums to the original finite BSDF, has no jump term or second energy contribution, leaves smooth/delta and back-face values zero, and declares the SPF's actual reflection type for every shared-BSDF material. No contact source or integrator path state was edited. The new test cases release their construction references. Fresh independent review is required before merging.

Review round1 found no transport P1, but one reviewer found a P1 stale API-comment family: IMaterial and EndpointBounceType still described SSS masks as empty. Corrected both and the matching MIS positive-cap/ordinary-reflection documentation. These follow-up edits change comments/docs only; the production function bodies and render-band code are identical to those used for the n8 render evidence. All four named builds returned0 without warnings after the correction. The full split, strict parity, exterior unit gate, row S (n8), all controls and positive-jump row L were rerun and returned0 before fresh round2 review; no test bands changed.
