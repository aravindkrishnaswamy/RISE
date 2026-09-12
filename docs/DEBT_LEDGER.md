# RISE correctness debt ledger

One verified list of the correctness debts genuinely still open in RISE,
produced by re-checking every heading in `CLOTH_FABRIC_DESIGN.md` §15,
`WETNESS_COAT_DESIGN.md` §12, `GEOMETRY_SHADING_SIGNALS_DESIGN.md` §14,
`SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md` §10,
`CROSS_OBJECT_PROXIMITY_DESIGN.md` §10, `RENDERING_INTEGRATORS.md` §7 (debts
27-31), `REFRACTIVE_RADIANCE_SCALING.md` §10, and `IMPROVEMENTS.md` against
the code and tests in this tree. Original sweep: debt-sweep worktree, HEAD
`14bc2cb6`, dated 2026-09-12. **Gap-closure pass** (this revision): same
worktree, starting HEAD `c287f5fb`, also dated 2026-09-12 — an independent
verifier found coverage gaps in the first pass (missing items across all
five design-doc ledgers, stale line-number citations, and one row,
DL-14, that was wrong on the facts) and this pass closes them; see the
`## Counts` section for the delta. Every verdict below is cited to a
file:symbol, test, or commit — no verdict is taken on the source document's
word alone for anything marked OPEN, and every item enumerated in any of
the seven source ledgers now appears in exactly one section below (OPEN,
Already resolved, Doc-rot, or Not-a-debt).

Sorted by class (physics-bias/energy-loss, precision, API/bridge gap,
coverage/test gap, perf, doc-rot), S before M before L within a class (the
`perf` class was added this pass — see DL-33). Struck headings in the
source ledgers point back to the row here (or vice versa) that closed them.

## Table

| id | source doc:item | one-line claim | verdict | evidence | size | class | visibility |
|----|------------------|-----------------|---------|----------|------|-------|------------|
| DL-01 | RENDERING_INTEGRATORS.md debt 31 item 3 / REFRACTIVE_RADIANCE_SCALING.md §10.3 | `TranslucentSPF`'s RGB exit lobe weights by extinction alone while the NM exit lobe also multiplies by the transmittance painter — a 2.5x RGB/NM disagreement | OPEN-confirmed | `TranslucentSPF.cpp` RGB exit ~line 225 (`front.kray = ColorMath::exponential(-distance*ab)`) vs NM exit ~line 384 (`front.krayNM = GuardedGetColorNM(*pTrans,ri,nm) * exp(...)`), read directly this sweep | S | physics-bias | user-visible |
| DL-02 | RENDERING_INTEGRATORS.md debt 31 item 4 / REFRACTIVE_RADIANCE_SCALING.md §10.3 | `TranslucentSPF::ScatterNM`'s exit ray samples/prices a Phong lobe while `PdfNM` forwards to the cosine-density `Pdf` — MIS-inconsistent | OPEN-confirmed | `TranslucentSPF.cpp` ~lines 426-434 (`Nval_front`) vs `PdfNM` forwarding to `Pdf`, read directly this sweep | S | physics-bias | user-visible |
| DL-36 | SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §10 | One residual is not neutral but a bounded neighbour read: a second surface of the SAME luminary inside the emitter probe's standoff band, within 0.01x its diagonal, is accepted with that neighbour's live channel instead of refusing | OPEN-confirmed | Doc's own §10 disclosure (a louvred single-object fixture, blade pitch under ~0.5% of its diagonal); bounded to another point of the same luminary at most `standoff` away; no dedicated red-proof found in `tests/SignalEmitterRecordTest.cpp` this sweep | S | physics-bias | user-visible (narrow: louvred/finely-corrugated single-luminary fixtures only) |
| DL-03 | RENDERING_INTEGRATORS.md debt 31 item 1 / REFRACTIVE_RADIANCE_SCALING.md §10.3 | Path-guiding substitutes a direction at `TranslucentSPF`'s exit lobe without carrying its popped `ior_stack`; the translucent object silently stays on the IOR stack, later hits misclassify entering/exiting | OPEN-confirmed | `TranslucentSPF.cpp` ~line 305-307/438-440 (`front.ior_stack->pop()`); `PathTracingIntegrator.cpp` ~3205/3260 sets `traceIorStack = &iorStack` (pre-scatter stack); `BDPTIntegrator.cpp:161`/`PathTracingIntegrator.cpp:553` `GuidingSupportsSurfaceSampling` admits `eRayDiffuse` unconditionally, confirmed by reading all four sites this sweep | M | physics-bias | user-visible (path guiding + `translucent_material` only) |
| DL-04 | REFRACTIVE_RADIANCE_SCALING.md §10.1 | Whether `SubSurfaceScatteringSPF`/`RandomWalkSSS`/`BSSRDFSampling::Sw` correctly omit the debt-30 eta^2 factor (telescoping argument) or need it (PBRT-style eta^2 divide) is undecided; direction not pinned down | OPEN-confirmed | Doc's own two-reading analysis (§10.1(a)/(b)); the disambiguating render (matched dielectric-shell-with-medium vs `subsurfacescattering_material`, submerged vs air camera) has not been produced — confirmed absent from `tests/` this sweep | M | physics-bias | user-visible (SSS in non-air medium only) |
| DL-34 | CROSS_OBJECT_PROXIMITY_DESIGN.md §10 | `interior(r)` UNDER-READS inside a UNION composite's overlap: the exported `min(f_A,f_B)` is a lower bound everywhere but exact nowhere inside the seam, where the true depth is `max(depth_A,depth_B)` | OPEN-confirmed | Doc's own measured example (two R=2 spheres 1.5 apart, point (0.4,0,0): operand depths 1.6/0.9, exported 1.6, true union depth 1.886796); `ObjectManager::DeepestOtherContainment` (`ObjectManager.cpp:861`) and `CSGObject`'s union path confirmed this sweep to still export the operand min, not the deeper operand, inside an overlap | M | physics-bias | user-visible (under-painted contact inside a union seam) |
| DL-05 | CLOTH_FABRIC_DESIGN.md §15 item 27 | Two-layer gapped weave with the light outside: PT under-reads BDPT/VCM by 1.28-1.55x because PT's binary NEE cannot see through the far layer's delta gap lobe; single layer or light inside is exact | OPEN-confirmed | Doc's own measured table (box/planes, gap 0.1/0.3); mechanism traced to `RayCaster::CastShadowRayTransmittance` (definition starts `RayCaster.cpp:2062`, re-derived this sweep — the previously cited ~1980 was drift) being gated to perfect-specular dielectrics only, confirmed present as described this sweep | L | physics-bias | user-visible |
| DL-06 | IMPROVEMENTS.md §"VCM env-IBL" (Session 9-13) / CLAUDE.md "Env-IBL deficit" entry | VCM env+mesh strict-tolerance residual (env-S0 <-> env-NEE MIS partition violation) — Session 13 explicitly decided to STOP and accept the disc-area baseline rather than fix it; `plank_closeup`'s VCM 0.55x (RENDERING_INTEGRATORS.md debt 28) is the same known bias class, not a new bug | OPEN-confirmed (deprioritized, not fixed) | `docs/VCM_ENV_MIS_PARTITION_INVESTIGATION.md` "Session 13 outcome"; `IMPROVEMENTS.md` lines ~1030-1046; still true in this tree — no VCM env-branch SA-MIS migration commit exists (`git log --oneline -- src/Library/Shaders/VCMIntegrator.cpp` shows no such commit after Session 13) | L | physics-bias | user-visible |
| DL-07 | CLOTH_FABRIC_DESIGN.md §15 item 17 / WETNESS_COAT_DESIGN.md §12 item 13 | `OrenNayarBRDF::hemisphericalAlbedo` over-estimates (measured ~12.6% high at roughness 0.5, ~25.6% at 1.0), which over-amplifies `fabric_material`'s energy-subtraction and `coated_material`'s Saunderson recycling denominator; not fixable in either wrapper, needs its own bake | OPEN-confirmed | `OrenNayarBRDF.cpp:148-190`'s own doc comment states the bias and that "no clean closed form exists to correct it with"; unchanged this sweep | L | physics-bias | user-visible (rough Oren-Nayar under fabric/coat) |
| DL-08 | RENDERING_INTEGRATORS.md debt 29 | `PSSMLTSampler`'s 49-lane stream layout is exceeded by `BDPTIntegrator`'s eye-subpath walk (`StartStream(16u+depth)`) at eye/volume depth >= 32, aliasing MLT's own film/lens/aperture lanes on stream 48 | OPEN-confirmed | `BDPTIntegrator.cpp:1732`; `PSSMLTSampler.cpp:59,75,146-147` (`kNumStreams` default 49, `idx = streamIndex + kNumStreams*sampleIndex`), read directly this sweep; `maxVolumeBounce` default 64 reaches depth 32 in ordinary deep-volume scenes | L | physics-bias | user-visible (MLT + deep volume/eye recursion) |
| DL-24 | WETNESS_COAT_DESIGN.md §12 item 2 | `CompositeSPF`'s random walk loses ~96% of the energy in the coat-over-diffuse configuration wetness needs; this design routes around it rather than fixing it | OPEN-confirmed | Doc's own measured 96% figure (`WETNESS_COAT_DESIGN.md:101,454-488`); `CompositeSPF.cpp`'s 50/50 `Pdf` and top-wins `GetBSDF` architecture is unchanged by the tree's most recent commits touching that file (`7713e509`/`9e3cf85e`/`f4336aba` fix capacity naming, gap-slant-length and a parameter name, none touch the energy-loss mechanism), confirmed this sweep | L | energy-loss | user-visible (any `composite_material` coat-over-diffuse) |
| DL-09 | REFRACTIVE_RADIANCE_SCALING.md §10.2 | `RadianceEtaScale` reads the IOR stack's push/pop-time values while `DielectricSPF`/`PerfectRefractorSPF` re-fetch the `ior` painter fresh at each hit; for a spatially-varying `ior` these can differ at an interior vertex (NEE/bounce before exit) | OPEN-confirmed, unexercised | `IORStack.h`'s `RadianceEtaScale` doc comment; re-checked this sweep: `grep -rl 'ior.*scalar_painter'` is NOT empty — `scenes/Tests/GUI/panel_stress_params.RISEscene:232` binds `ior panel_scalar_dispersion`, a `scalar_painter` — but that painter's body (`:182-188`) is three per-channel CONSTANT `param`s combined algebraically (`vec3(ior_r, ior_r+spread*0.5, ior_r+spread)`), with no positional (`P`) term, so it is not actually spatially-varying and does not exercise this row; still no scene binds a position-dependent `ior` | S | precision | internal (no scene exercises it yet) |
| DL-10 | RENDERING_INTEGRATORS.md debt 28 (fisheye residual) | Fisheye camera's per-pixel solid angle is computed in the pre-stretch local frame while `mxTrans` applies `Stretch(pixelAR,1,1)`; at `pixelAR != 1` the world-space solid angle differs by an uncomputed direction-dependent Jacobian | OPEN-confirmed, unexercised | Doc's own analysis; confirmed no in-tree scene pairs `fisheye_camera` with non-square `pixelAR` (`grep -rl fisheye_camera scenes/` cross-checked against pixelAR values) this sweep | S | precision | internal (no scene exercises it yet) |
| DL-29 | WETNESS_COAT_DESIGN.md §12 item 10 | A measured `tau` curve is grey under the default RGB rasterizer because `PiecewiseLinearScalarPainter` broadcasts one 555nm sample and reports no per-channel variation | OPEN-confirmed | `PiecewiseLinearScalarPainter.h:108` (`HasPerChannelVariation() const override { return false; }`), confirmed unchanged this sweep | S | precision | user-visible (measured spectral tint vanishes under RGB pathtracing) |
| DL-11 | CLOTH_FABRIC_DESIGN.md §15 item 18 | Sheen directional-albedo table's middle band (`kMinSheenAlpha` <= n·v < 0.0349) still reads +0.75% over 1 after the round-9 64x64 rebake; the bottleneck moved (α ≈ 0.065 roughness-floor mechanism), not closed, and root-cause work is unstarted | OPEN-confirmed | Doc's own round-9 table; `tests/SheenDirectionalAlbedoTest.cpp` and `tests/LayeredWhiteFurnaceTest.cpp` grazing-check both still assert the *bounded*, not *exact*, posture (checked this sweep — no round-10 commit exists) | M | precision | user-visible (subtle, grazing sheen) |
| DL-12 | CLOTH_FABRIC_DESIGN.md §15 item 15 | Mirrored UV seams flip weave direction: `vShadingTangent` has no `bitangentSign` companion, so a `dpdu`-derived tangent can mirror across a UV seam | OPEN-confirmed | `RayIntersectionGeometric.h:469-470` (`vTangent` paired with `bitangentSign`) vs `:516` (`vShadingTangent`, no sign field) — re-derived this sweep, the previously cited `:296-299`/`:344-345` had drifted | M | precision | user-visible (mirrored-UV mesh + weave) |
| DL-13 | GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 3 | Displaced geometry curvature is ambiguous between base (`smoothing=1`) and displaced (`smoothing=0`) surface; only `EllipsoidGeometry`/`DisplacedGeometry` override `ComputeAnalyticalDerivatives` at all, and lookdev often wants base curvature the baked path can't isolate tessellation-independently | OPEN-confirmed | `DisplacedGeometry.cpp:471` (`ComputeAnalyticalDerivatives` definition — re-derived this sweep, the previously cited `:407+` had drifted); confirmed no third override or documented convention added since this heading was written | M | precision | user-visible (wear/relief masks on displaced meshes) |
| DL-14 | SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §10 | Ray differentials (`fw`/`fwo`) are 0 under BDPT/VCM/MLT — the FIELD exists and is carried, but no differentials are ever computed for those walks, so it stays zero; a PT-vs-bidirectional difference on filtered-noise / footprint-driven bands | OPEN-confirmed | **Corrected this sweep**: the previous evidence ("no `BDPTVertex` differential field exists", citing `BDPTIntegrator.h`) was wrong on the facts — `BDPTVertex` (`src/Library/Shaders/BDPTVertex.h`) DOES carry the field: `TextureFootprint txFootprint;` at `:162`, whose own comment reads "pixel footprint; all-zero under today's bidirectional rasterizers (they emit no ray differentials) but carried so a future landing cannot silently reopen the gap". So the open work is plumbing ray differentials through the BDPT/VCM/MLT eye subpath (mirroring the S1 `derivatives`/`signals` widening the same struct already got) so `txFootprint` gets populated, not adding a new field | M | precision | user-visible (footprint-aware texture filtering under BDPT/VCM/MLT) |
| DL-15 | CROSS_OBJECT_PROXIMITY_DESIGN.md §10 | An eccentric ellipsoid neighbour's distance bound is loose by its semi-axis ratio (4:1 measured: true distance 2.75 reported as 11.0), so `proximity(r)` silently paints nothing unless the author inflates the radius by the ratio | OPEN-confirmed | Doc's own measured example; Phase 3 (shipped 2026-09-09) makes σ exact but "`x sigma_max` remains a bound attained only along the top singular vector" — confirmed the bound, not exactness, is what Phase 3 delivered, by re-reading §10's Phase-3 paragraph this sweep | M | precision | user-visible (unpainted seam near eccentric ellipsoids) |
| DL-31 | CROSS_OBJECT_PROXIMITY_DESIGN.md §10 | A mesh neighbour is a SHEET (no inside test), unlike every solid family which clamps its signed field at zero; `interior(r)` did not close this either, since a mesh contributes 0 to it too | OPEN-confirmed | Doc's own §10 analysis; no closed-mesh containment test exists on `Object.cpp`'s `DistanceToSurface`/`ObjectManager::DeepestOtherContainment` paths, confirmed this sweep | M | precision | user-visible (a receiver buried inside a closed mesh neighbour reads no contact) |
| DL-16 | CLOTH_FABRIC_DESIGN.md §15 item 4 | `ggx_material.tangent_rotation` stays Color-pipe only; the promised Scalar-pipe alias (so a `fabric_material`'s `weave_rotation` and its substrate's own rotation can share one painter) was never added | OPEN-confirmed | `ChunkParserRegistry.cpp:4495` — `tangent_rotation`'s only descriptor entry is `ParameterPipe::Color`, `p.description` states "a scalar_painter does NOT bind here"; no second `tangent_rotation`-family scalar parameter exists (grepped this sweep) | S | API/bridge gap | user-visible (authoring: can't drive both rotations from one field) |
| DL-17 | CLOTH_FABRIC_DESIGN.md §15 item 12 | glTF `anisotropy_rotation` is still dropped at import, even though the expression VM's `atan2` (confirmed present) makes the sketched fix executable today | OPEN-confirmed | `GLTFSceneImporter.cpp:1300-1307`'s comment stands; `ChunkParserRegistry.cpp:4560`'s `anisotropy_rotation` descriptor still reads "Phase 1 reads but does not yet APPLY the rotation" — read directly this sweep | S | API/bridge gap | user-visible (glTF import only) |
| DL-23 | CLOTH_FABRIC_DESIGN.md §15 item 16 (tail) | `coated_material`'s substrate allowlist does not admit `fabric_material`/`weave_material`, so a coat-over-fabric composition (e.g. waxed canvas) is unreachable | OPEN-confirmed | `CoatedMaterial.h:112-113` (`SubstrateAllowlistText()`: "lambertian_material, orennayar_material, ggx_material, pbr_metallic_roughness_material") and `IsSupportedSubstrate` `:120-134` (the `dynamic_cast` allowlist) — `fabric_material`/`weave_material` absent from both, confirmed this sweep | S | API/bridge gap | user-visible (authoring: can't compose a coat over fabric) |
| DL-26 | WETNESS_COAT_DESIGN.md §12 item 6c | `add_wetness` and `add_wear` mutually exclude on one material; worn-and-wet, the flagship subject, is unreachable | OPEN-confirmed | `src/Library/Agent/AgentSession.cpp` ~8021/8092/8328 ("add_wear / add_wetness cannot currently be combined on one..."), confirmed present this sweep | S | API/bridge gap | user-visible (agent-authored worn-and-wet materials) |
| DL-28 | WETNESS_COAT_DESIGN.md §12 item 9 | Water-absorption spectral files must be pre-converted to a transmittance base because `dielectric_material`'s `tau` is `pow(tau,distance)`, not `exp(-sigma*distance)`; a pasted-in published sigma_a table is silently wrong | OPEN-confirmed | `DielectricSPF.cpp:318-323` (`pow(tauVals.v[i], distance)`), confirmed unchanged this sweep; no runtime validation or warning exists for a mismatched-convention input file | S | API/bridge gap | user-visible (authoring trap only, silent) |
| DL-32 | CROSS_OBJECT_PROXIMITY_DESIGN.md §10 | `standard_object`'s `scale` written with ONE number (e.g. `scale 0.35`) derives to a degenerate transform silently — no diagnostic, the object vanishes from the render, and `DistanceToSurface`'s `sigma_min<=0` gate then refuses every proximity query against it | OPEN-confirmed | `ChunkParserRegistry.cpp`'s `standard_object` `scale` descriptor (`DoubleVec3`, no partial-fill diagnostic); `Object.cpp:1708` warns only for an ANISOTROPIC transform's `proximity()`, not a degenerate one — confirmed no parser-side warning for a partially-specified `DoubleVec3` this sweep | S | API/bridge gap | user-visible (silent scene-authoring trap) |
| DL-18 | CLOTH_FABRIC_DESIGN.md §15 item 13 | The Blender bridge has no sheen, anisotropic, or velvet mapping at all — Principled's Sheen sockets have no `fabric_material` target | OPEN-confirmed | No `fabric_material`/`sheen` reference found in the Blender bridge sources this sweep (`grep -rl fabric_material` under the Blender add-on tree returns nothing) | M | API/bridge gap | user-visible (Blender-authored scenes only) |
| DL-25 | WETNESS_COAT_DESIGN.md §12 item 6b | Phase 1 cannot darken a textured substrate: the expression VM has no painter-sampling builtin | OPEN-confirmed | `src/Library/Painters/ExpressionEval.h` function table (~lines 1166-1171) has no painter-sample builtin alongside `sin`/`cos`/`atan2`/etc.; confirmed absent this sweep by grep | M | API/bridge gap | user-visible (wet textured substrates can't darken) |
| DL-19 | SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §10 | Two S3 conversion sites (BDPT's NM-hero `Le` rebuild, the HWSS companion `rigW` rebuild) share the signals-replay helper but have no dedicated red-proof — their contribution is MIS-weighted to a few percent on the money test's scenes, so skipping them moves the suite by <= 5.7% / 0% | OPEN-confirmed (test gap) | Doc's own §10 disclosure, confirmed current (no new red-proof test added for these two sites since — `tests/SignalEmitterRecordTest.cpp` unchanged in this tree per `git log` this sweep) | S | coverage/test gap | internal (test-suite blind spot) |
| DL-27 | WETNESS_COAT_DESIGN.md §12 item 7 | The wet-highlight variance cost is unmeasured | OPEN-confirmed | No test or scene mentioning "wet_highlight"/"WetHighlight" found in `tests/` or `docs/*.md` this sweep other than the design doc itself | S | coverage/test gap | internal (measurement gap, not a known defect) |
| DL-30 | GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 1 (disclosed residual) | A CSG exit-designated subtraction branch's `dndu` pairing is unverified, reachable only through a nested-CSG construction no test currently produces | OPEN-confirmed, untested | Doc's own disclosure (§14 item 1, appended when item 1 was RESOLVED 2026-08-29); no nested-CSG `dndu`-pairing test found in `tests/CsgSurfacePayloadTest.cpp` or elsewhere this sweep | S | coverage/test gap | internal (no scene exercises it yet) |
| DL-20 | GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 2 | Patch geometries report flat curvature (`valid=false`) while genuinely curved; deferred to Phase 4, no Phase-4 work has landed | OPEN-confirmed | No patch-geometry curvature override exists (only `EllipsoidGeometry`/`DisplacedGeometry` override `ComputeAnalyticalDerivatives`, confirmed this sweep alongside DL-13) | M | coverage/test gap | user-visible (curvature-driven wear on patch geometry reads absent, not wrong) |
| DL-21 | GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 5 | CSG boundary curvature behaviour is unspecified/undecided (forward the contributing surface's curvature, or invalidate at the seam) | OPEN-confirmed | `tests/CsgSurfacePayloadTest.cpp:833-834` exercises the derivative fields there but does not pin a curvature convention at the boundary — confirmed by reading the referenced lines this sweep | M | coverage/test gap | user-visible (CSG seam wear masks) |
| DL-22 | SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §10 | BSSRDF entry vertices read default (neutral) `derivatives`/`signals` under both PT and BDPT — integrator-consistent, but a signal-keyed IOR/Fresnel painter at a subsurface entry point is neutral rather than live | OPEN-confirmed | Doc's own §10 first bullet; closing it needs a probe record in `BSSRDFSampling::SampleResult`, confirmed absent this sweep | M | coverage/test gap | user-visible (signal-keyed SSS entry only) |
| DL-33 | CROSS_OBJECT_PROXIMITY_DESIGN.md §10 | `interior(r)`'s TLAS-backed candidate walk (`BVH::ForEachContainingPoint`) has a genuinely different cost shape from `proximity(r)`'s `NearestOtherSurface` (no shrinking-radius prune; visits every node whose box contains the query point) — NOT YET MEASURED | OPEN-confirmed, unmeasured | `BVH.h:2018` (`ForEachContainingPoint`); no distance-call-count or wall-clock measurement of it exists in `tests/` (the doc's own `NearestOtherSurface` figures, e.g. "11.08 distance calls per query on Sponza", have no `interior(r)` counterpart), confirmed this sweep | S | perf | internal (cost-shape unmeasured, not a correctness bug) |

## Already resolved — verified this sweep, cited for completeness

These carry RESOLVED/CLOSED language in their source doc already; this sweep
independently re-verified the code exists as described rather than taking
the heading's word for it.

| id | source | claim | verdict | evidence |
|----|--------|-------|---------|----------|
| DL-R1 | RENDERING_INTEGRATORS.md debt 28 | BDPT/VCM/PT thin-lens t==1 aperture sampling | CLOSED-by-code | `ThinLensCamera::GetApertureWorldArea`/`SampleLensPoint`/`RasterFromLensPoint` exist; `tests/CameraImportanceTest.cpp` present, confirmed this sweep |
| DL-R2 | RENDERING_INTEGRATORS.md debt 30 | Radiance-mode eta^2 factor at dielectric interfaces | CLOSED-by-code | `RISE::RadianceEtaScale` in `Utilities/IORStack.h`; `tests/RefractiveRadianceScalingTest.cpp` present, confirmed this sweep |
| DL-R3 | RENDERING_INTEGRATORS.md debt 31 item 2 | `TranslucentSPF` per-channel-loop `IORStack` leak (unconditional on success, plus overflow-carryover) | CLOSED-by-code | `TranslucentSPF.cpp` ~lines 169-207 re-arms `delete_stack` before every allocation and frees a carried-over stack before reassignment, read directly this sweep |
| DL-R4 | CLOTH_FABRIC_DESIGN.md §15 item 3 | Furnace config 6 IS config 7's defect (`SheenSPF` never reaches the substrate; reflection-only by construction), now measured rather than asserted | CLOSED-by-test | `SheenSPF.cpp:73-99` (cosine-hemisphere draw gated below the geometric horizon); doc's own measured config 6 `{0.0875,0.1293,0.2812,0.5461}` vs config 2 (bare sheen) `{0.0875,0.1283,0.2810,0.5453}` — same curve to MC noise, confirmed unchanged this sweep |
| DL-R5 | CLOTH_FABRIC_DESIGN.md §15 item 19 | `CookTorranceSPF::PdfNM` reported a different mixture from `ScatterNM` (11 orders of magnitude out of family); fixed via achromatic lobe-selection weights + a `kSelFloor=1e-3` reachability floor | CLOSED-by-test | `CookTorranceSPF.cpp:54` (`kSelFloor`), `:75-78` (`ComputeLobeWeights`); `tests/CookTorranceHWSSTest.cpp` present, confirmed this sweep |
| DL-R6 | CLOTH_FABRIC_DESIGN.md §15 item 20 | BDPT/VCM "100-350x over-count" on a backlit sheer weave curtain was PT being the broken reference (the debt-21 self-hit bug deflating PT), not a BDPT/VCM defect | CLOSED-by-test | `tests/FabricRenderTest.cpp::TestBacklitSheerCurtain` (~926) and `::TestTouchingAreaLitCurtainAllIntegrators` (~1207) present with asserted BDPT/PT and VCM/PT bands; `AutoRasterizer.cpp:121` (`CouldLightPassThrough()` alone — the full-sphere exclusion is lifted), confirmed this sweep |
| DL-R7 | CLOTH_FABRIC_DESIGN.md §15 item 21 | Path tracing's diffuse-transmission lobe read super-linear in `transmit` because of a shadow-ray self-intersection epsilon bug in `RayBilinearPatchIntersection` (93.6% of NEE shadow rays spuriously self-shadowed), not an MIS defect | CLOSED-by-test | `RayBilinearPatchIntersection.cpp:160-162` (scale-relative `coordScale`/`tMin`); `tests/FabricRenderTest.cpp::TestAreaLitSheerWeave` (~1099) present, confirmed this sweep |
| DL-R8 | CLOTH_FABRIC_DESIGN.md §15 item 22 | A `fabric_material` over a `transmission thin` weave extinguished the weave's transmission (100% opaque, no diagnostic) because three sites dropped the substrate's `ScattersFullSphere`/`CouldLightPassThrough` flags and hemisphere gating | CLOSED-by-test | `FabricMaterial.h` forwards both flags; `tests/FabricMaterialChunkTest.cpp:1561` (`TestTransmissiveSubstrateForwarding`) present, confirmed this sweep |
| DL-R9 | CLOTH_FABRIC_DESIGN.md §15 item 23 | BDPT/VCM read ~7-10% under PT on a delta-light backlit thin weave because subpath vertex connectibility was decided from the sampled ray's `isDelta` flag instead of the surface's own BSDF availability | CLOSED-by-code | `BDPTIntegrator.cpp:2293` (`vertices.back().isConnectible = ( ri.pMaterial->GetBSDF() != 0 )`), confirmed present this sweep, matching the item's described fix |
| DL-R10 | CLOTH_FABRIC_DESIGN.md §15 item 24 | VCM read +3.0% over PT on every delta-light scene because `EvaluateNEEImpl` zeroed `wCamera` (a light-side strategy density) whenever the light was a delta position, not just `wLight` | CLOSED-by-code | `VCMIntegrator.cpp:1483-1485` (`camFactor` computed unconditionally from the emission-direction area density), confirmed present this sweep |
| DL-R11 | CLOTH_FABRIC_DESIGN.md §15 item 25 | A closed `box_geometry` whose material is a thin-transmissive weave read BDPT/VCM ~0.17x PT; root cause was `box_geometry`'s own ~1e-12 self-hit root on PT's unadvanced NEE shadow ray, not an integrator defect | CLOSED-by-code | `BoxGeometry.cpp:220` (`DropSelfHitRoot`), `:293` (called from `IntersectRay`); `tests/BoxGeometryTest.cpp:369,885-886` (`RunStandoffReentryContract` at scale 1.0 and 1000.0); `tests/CsgSurfacePayloadTest.cpp` present, confirmed this sweep |
| DL-R12 | CLOTH_FABRIC_DESIGN.md §15 item 26 | Legacy `pixelpel_rasterizer` read 2.46x under modern PT on a gapped weave + area light scene; root cause was a DIRECT-LIGHTING-ONLY shader chain casting no continuation ray at all (not a rasterizer defect) | CLOSED-by-test | `tests/BDPTStrategyBalanceTest.cpp:508` (`kRasterizerPT` uses `DefaultPathTracing`) and its topology F (gapped weave + area light), confirmed present this sweep |
| DL-R13 | GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 10 | Deforming/vertex-animated geometry would stale a bake — the item's ORIGINAL "no `EvaluateAtTime` implementation, so transform-only" assumption was wrong; `TriangleMeshGeometryIndexed::UpdateVertices` mutates vertex/normal arrays in place and refits the BVH, and Phase 3 drops bakes explicitly at every vertex-mutation site | CLOSED-by-code | `TriangleMeshGeometryIndexed.h:359` / `.cpp:338` (`UpdateVertices`), confirmed present this sweep, matching the item's corrected analysis |
| DL-R14 | CLOTH_FABRIC_DESIGN.md §15 item 8 / GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 11 / WETNESS_COAT_DESIGN.md §12 item 1 / CROSS_OBJECT_PROXIMITY_DESIGN.md §10 (bullet 1) | BDPT/VCM/MLT's hand-built vertex records and `LightSampler`'s hand-built NEE/light-subpath-root records read geometry/proximity signals as neutral instead of live | CLOSED-by-test | `BDPTVertex.h` widened with `derivatives`/`signals`/`txFootprint` (S1) and `LightSampler.cpp`'s NEE/light-subpath-root probe (S3); `tests/SignalIntegratorConsistencyTest.cpp` and `tests/SignalEmitterRecordTest.cpp` (77 checks) both present, confirmed this sweep — residuals (a BSSRDF entry vertex, probe-refusal cases, the bounded-neighbour-read) carried forward as DL-22 and DL-36 |
| DL-R15 | CROSS_OBJECT_PROXIMITY_DESIGN.md §10 | `interior()` saw a TLAS-backed scene's object set through the stale AABB snapshot while `proximity()` walked the live tree — an asymmetry that over-painted a just-added 7th object to `interior()` only | CLOSED-by-code | `ObjectManager.cpp:861` (`DeepestOtherContainment` now uses `BVH::ForEachContainingPoint`, the same TLAS `NearestOtherSurface` uses), confirmed present this sweep, matching `ProximitySignalTest`'s g2 case named in the doc |
| DL-R16 | CROSS_OBJECT_PROXIMITY_DESIGN.md §10 | A CSG composite refused every proximity/interior query, making its own surface invisible to every neighbour | CLOSED-by-code | `CSGObject.cpp:2681,2758` (`DistanceToSurfaceWithArm`/`DistanceToSurface` implemented — union answers min over operands that answer, intersection/subtraction bracket the composed signed field), confirmed present this sweep (Phase 3 S3) |
| DL-R17 | CROSS_OBJECT_PROXIMITY_DESIGN.md §10 | Non-uniform transforms used the loose Frobenius/determinant sigma bounds pre-Phase-3 | CLOSED-by-code (partial — residual folded into DL-15) | Phase 3 S2 ships a one-sided Jacobi SVD with the Frobenius/determinant pair only as the non-convergence fallback; doc's own measured `scale (3,1,0.4)` figures (search-radius inflation 8.46667x -> 2.5x); the surviving anisotropy-direction residual (`sigma_max` attained only along the top singular vector) is the same mechanism DL-15 tracks, confirmed this sweep |

## Doc-rot found and struck this sweep

Struck in place in the source ledger with `CLOSED <date> — <evidence>`; not
reflowed otherwise.

| id | heading | why it was stale | commit |
|----|---------|-------------------|--------|
| DR-01 | CLOTH_FABRIC_DESIGN.md §15 item 1 | "No anisotropic material... in that sweep" — `tests/SPFBSDFConsistencyTest.cpp:1220` (re-derived this sweep — the previously cited `:1174` had drifted) already has `Fabric_GGXaniso_weave45` | `f458f4f3` |
| DR-02 | CLOTH_FABRIC_DESIGN.md §15 item 2 | "Charlie/Neubelt" naming already fixed everywhere, including the descriptor `cd.description` this item worried about | `f458f4f3` |
| DR-03 | CLOTH_FABRIC_DESIGN.md §15 item 7 | CLOSED 2026-09-12 — Zhu 2023/Jin 2022 params were obtained and shipped as `weave_material` | `8378266b` |
| DR-04 | CLOTH_FABRIC_DESIGN.md §15 item 9 | `kCapacity` is named, documented, and `CompositeSPF` warns once on drop | `f4336aba` |
| DR-05 | WETNESS_COAT_DESIGN.md §12 item 4 | `tidepools.RISEscene`'s comment was rewritten to describe the `painter`-bridge capability, no longer claims `tau` can't vary spatially | `aa1f0162c` |
| DR-06 | GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 6 | CLOSED 2026-09-12 — requirement (`vGeomNormal` orientation, so `curv` and `N` may legitimately disagree on a bump-mapped surface) is implemented and documented in force; re-derived this sweep against `tests/SurfaceCurvatureTest.cpp`'s case (e) `TestCurvInvariantUnderNormalPerturbation` (~398-440, confirms `curv` invariance under an applied `ReliefModifier`) and `SurfaceCurvature::MeanCurvatureFromDerivatives` (`SurfaceCurvature.h` ~87, whose doc comment ~57-71 states the field takes no normal by design) — the previously cited `RayIntersectionGeometric.h:280-296` was the wrong file/symbol entirely | (pre-existing, verified this sweep — no single fix commit) |
| DR-07 | GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 9 | CLOSED 2026-09-12 — assumption verified: no signal-demand cache couples to `Prepare()`/`SetEnvironmentSampler()` | (verified this sweep by grep; no code change needed) |

## Not-a-debt (design choices / observed-need gates / declined) — reviewed, no action

- CLOTH_FABRIC_DESIGN.md §15 item 5 — anisotropic Charlie sheen deferred; design choice, LTC route noted if ever needed.
- CLOTH_FABRIC_DESIGN.md §15 item 6 — Ashikhmin-Premoze-Shirley velvet normalization unverified; not implemented, not blocking.
- CLOTH_FABRIC_DESIGN.md §15 item 8 / GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 8 — `ScalarToPainterAdapter` doesn't exist; convenience gap, explicitly "not a blocker" in both docs.
- CLOTH_FABRIC_DESIGN.md §15 item 10 — weave aliasing has no automatic mitigation; authoring idiom (`fw` fade), not a defect.
- CLOTH_FABRIC_DESIGN.md §15 item 11 — multi-slot preset hypothesis unmeasured; unbuilt feature, not a defect.
- CLOTH_FABRIC_DESIGN.md §15 item 14 — Tier-1 spectral dye not attempted; future opportunity, non-goal-adjacent.
- CLOTH_FABRIC_DESIGN.md §15 item 16 — `fabric_material`'s own `hemisphericalAlbedo` factorization is closed-form and measured <= 0.64%; the larger residual is DL-07 (Oren-Nayar's own bug), not this item.
- GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 4 — vertex-normal quality is loader-dependent; documented limitation, no fix proposed, none needed.
- GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 7 — `expression_function2d` stays UV-only by design.
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — non-convex coplanar clipped-plane refusal: safe-by-design (refusing rather than under-reporting).
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — emissive objects never count toward proximity: Phase-3-gated on observed need, not a defect.
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — `kL1Ways` cliff: superseded by Phase 3 S4 (8 ways, measured 0.8967->0.8968 hit rate), closed in-doc already.
- IMPROVEMENTS.md §8B/§8C (correlation-aware / efficiency-aware MIS for BDPT), §7D (deferred null-scattering volume item) — roadmap features explicitly marked deferred, not correctness bugs.
- WETNESS_COAT_DESIGN.md §12 item 8 — heightfield-mode SDF returns neutral occlusion silently. Doc's own words: "Not a bug (the fallback is deliberately do-nothing)... it produces a wrong-looking render with no diagnostic." A UX/diagnostics wish (warn when the target's geometry family cannot answer `occlusion()`), not a correctness defect.
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — the refusing-families behavior (RAW meshes, patches, hair, heightfield SDFs; a CSG intersection/subtraction with a sheet/heightfield-SDF operand, a union of two refusing operands, budget-exhausted brackets, a sub-1e-12 seam gradient all read far) is a documented, safe-by-design contract, not a bug — the failure direction is always "reads far / refuses" rather than under-reporting a true contact.
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — SDF-neighbour distance is an upper bound by design (never over-reads contact), bounded by the last probe step; a candidate whose crossing is not found in budget reads far. Documented behavior, not a defect.
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — the reported range is `[0, maxDist)`, not `(0, maxDist]` (zero attained at interpenetration, the radius itself excluded); a stated, continuous convention, not a bug.
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — the eager snapshot build and per-ray `EnsureBoxSnapshot()` are gated on `ProximityDemand` rather than built unconditionally; a deliberate perf/correctness trade (an unregistered direct caller pays the build cost once under a lock instead of finding it pre-built), documented and accepted.
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — `proximity(r)` cannot drive `ReliefModifier`'s relief: confirmed by code comment, `ReliefModifier.cpp:279-283` ("DELIBERATELY LEFT ALONE: vNormal, derivatives, signals... holds them invariant under shading-normal [perturbation]"). A deliberate architectural invariant (the same one that makes `curv` bump-invariant), not an oversight.
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — per-sample motion-blur staleness of a proximity/interior memo entry (a moving neighbour under a non-keyframed painter, or the scalar pipe's `time=0` stamp relying on the jitter argument alone): disclosed, and the doc's own analysis is that sub-pixel jitter makes a stale hit essentially unreachable in practice; no scene in the design's own corpus exercises it.
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — the TLAS-backed candidate-scan shape description (a leaf visits up to 4 occupants before pruning; a per-element box pre-test roughly halves that) is a documented perf characterization with a measured 1.094x wall-clock effect on five pairs, not a correctness bug.
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — `proximity` is unsigned by design; `interior(r)` supplies the inside half for solid families only (documented scope, not a defect in itself — the mesh-sheet gap this leaves is tracked separately as DL-31).
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — the query's read of other objects' live transforms joins the pre-existing per-sample `EvaluateAtTime` race under motion blur, "no wider than `Object::IntersectRay` already does" per the doc's own comparison; not a new or worse hazard.
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — the memo-eligibility threshold moving with `kFields` (29 -> 35) is bit-identical and perf-only, disclosed as such.
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — the agent-facing draft-preview descriptor strings claiming quality:"draft" ignores materials/lighting entirely were wrong (draft actually evaluates the material's full BSDF under a fixed studio rig, only real scene LIGHTING is skipped) — already corrected in the same session in `AgentMcpAdapter.cpp`/`AgentChatCodecs.cpp`/`AgentSession.{h,cpp}`/`AgentRpc.h`; a documentation-accuracy fix already landed, not an open code debt.
- SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §10 — "No weight formula changes": MIS heuristics per integrator stay exactly as documented in MIS_HEURISTICS.md; a stated non-goal, not a debt.
- SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §10 / CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — GUI painter preview, realize-time displacement, and `HairGenerator` build their own hit records and read signals as neutral; doc's own words: "not transport; unchanged and already disclosed." These are non-transport, non-comparison paths (a single-sample preview, a one-shot bake, hair-strand generation), not part of the PT/BDPT/VCM consistency contract that DL-14/DL-19/DL-22/DL-30/DL-36 track.

## Counts

Updated by the 2026-09-12 gap-closure pass (see the header note); the
original sweep's own counts are superseded by these.

- OPEN-confirmed: **35** (DL-01 .. DL-22, plus DL-23..DL-34 and DL-36 added
  this pass — DL-35 was evaluated and placed as NOT-A-DEBT instead, so the
  numbering has a deliberate gap)
- CLOSED-by-sweep (heading was open/unlabeled; a sweep found it actually
  fixed and struck it): **7** (DR-01 .. DR-07, unchanged this pass; DR-01's
  line citation, and DR-03/DR-06/DR-07's missing date tokens, were corrected)
- Already RESOLVED in source, independently re-verified: **17** (DL-R1 ..
  DL-R17 — DL-R1..DL-R3 from the original sweep; DL-R4..DL-R17 added this
  pass, covering CLOTH_FABRIC_DESIGN.md §15 items 3/19-26, GEOMETRY_SHADING_
  SIGNALS_DESIGN.md §14 items 10-11, and CROSS_OBJECT_PROXIMITY_DESIGN.md
  §10's TLAS-snapshot, CSG-refusal and Frobenius-bound closures)
- NOT-A-DEBT: **27** (13 from the original sweep + 14 added this pass —
  design choices, safe-by-design refusals, disclosed non-transport paths,
  and one already-landed documentation-accuracy fix)
- UNVERIFIABLE: **0**

Total items re-verified or newly placed by the gap-closure pass: 13 new
OPEN rows + 14 new Already-resolved rows + 14 new Not-a-debt bullets + 5
citation corrections (DL-05, DL-09, DL-12, DL-13, DL-14) + 1 rewritten-on-
the-facts row (DL-14) + 4 doc-rot citation/date fixes (DR-01, DR-03, DR-06,
DR-07) = **41 items classified, 9 citations corrected**, this pass.

## Verification recipes for OPEN items

Concrete "how to know it's fixed" for each DL-xx row above.

**DL-01 (TranslucentSPF RGB/NM exit weight divergence).** Extend
`tests/TranslucentIORStackTest.cpp` (or add
`tests/TranslucentSpectralParityTest.cpp`) with a `translucent_material`
fixture (`transmittance 0.4`, uniform per-channel `N`), render/evaluate the
exit lobe once through the RGB path and once through `ScatterNM` at a fixed
wavelength set. Expected today: NM exit reads ~0.4x the RGB exit (a `~2.5x`
disagreement, matching the doc's own figure). Fixed when both read the same
value to the suite's `CROSS_VAL_TOL` (1e-3), after deciding which
convention (extinction-only, or extinction*transmittance) is correct and
applying it to both `Scatter` and `ScatterNM`.

**DL-02 (TranslucentSPF spectral exit MIS inconsistency).** In the same
test file, sample `ScatterNM`'s exit lobe N times, and independently compute
`PdfNM` for each sampled direction. Expected today: `PdfNM` returns the
cosine density while the Phong-lobe density the sample was actually drawn
from differs measurably at grazing directions. Fixed when a pointwise
`kray*pdf == value*|cos|`-style check (the existing `SPFBSDFConsistencyTest`
convention) passes for the exit lobe at the shared `CROSS_VAL_TOL`.

**DL-03 (TranslucentSPF guided-direction IOR-stack leak).** Add a scene with
path guiding enabled (`use_path_guiding true`, enough samples to train the
field) and a `translucent_material` object; after a training pass, verify
via a counter/log line in `PathTracingIntegrator` that when the guided field
intercepts a translucent exit vertex, the substituted ray still uses the
SPF's `pS->ior_stack` (post-pop) rather than the pre-scatter `iorStack`.
Fixed when a later hit on the same object after a guided exit reads
`bEntering == true`, not `false`; a red-proof test should assert on the
`IORStack::containsCurrent()` state after a guided translucent exit.

**DL-04 (SSS family eta^2 direction).** Build the two-material observable
the doc names: a semi-infinite slab as (a) a `dielectric_material` shell
with a scattering interior medium, and (b) `subsurfacescattering_material`
with matched albedo/mfp, each rendered with a submerged camera and an
air camera, everything else fixed. Expected invariant if RISE's convention
is self-consistent: the submerged/air brightness ratio is identical for (a)
and (b). A `tests/SSSRadianceScalingTest.cpp` asserting that ratio-of-ratios
== 1 (within MC noise) is the closing guard, however the underlying
direction resolves.

**DL-05 (PT two-layer weave gap, debt 27).** Extend
`tests/BDPTStrategyBalanceTest.cpp` with a topology pairing two
`weave_material transmission thin` layers with a light OUTSIDE both (the
doc's own box/plane fixtures at gap 0.1/0.3). Expected today: PT/BDPT ~=
0.65-0.78 (i.e. BDPT/PT 1.28-1.55x). Fixed when a `HasNonBendingDeltaLobe()`
material query lets `CastShadowRayTransmittance` (or an equivalent walk)
see through the far layer's delta gap for PT's NEE, closing the row to the
suite's standard 8% band, with an added-energy suppression against PT's
own BSDF-sampled delta continuation for AREA lights (to avoid a new
double-count).

**DL-06 (VCM env+mesh SA-MIS partition).** Re-run `EnvLightBalanceTest` at
its strict tolerances (`{0.10, 0.30, 1.00}`); currently 78/80. Fixed when
the monolithic SA-MIS migration (both subpath sides, the shared generator's
env-vertex pdf convention, the 6 BDPT rows, OpenPGL) lands and this reaches
80/80 strict without regressing hwss=true below ~0.82 (its pre-existing,
separately-tracked bias floor).

**DL-07 (OrenNayarBRDF hemisphericalAlbedo bias).** Add a
`tests/OrenNayarHemisphericalAlbedoTest.cpp` comparing
`OrenNayarBRDF::hemisphericalAlbedo()`'s closed form against a brute-force
hemispherical quadrature at roughness in {0, 0.3, 0.5, 1.0}. Expected today:
0% / ~5% / ~12.6% / ~25.6% over. Fixed when a corrected estimator (or a
small baked correction table) brings all four under a stated tolerance
(e.g. 2%), then re-measure `fabric_material`/`coated_material`'s furnace
configs that depend on it (cloth furnace config using Oren-Nayar substrate;
`WETNESS_COAT_DESIGN.md`'s coated-Oren-Nayar furnace row).

**DL-08 (PSSMLT stream aliasing).** Add a case to
`tests/PSSMLTStreamAliasingTest.cpp` that drives a volume-heavy scene to eye
depth >= 32 under `mlt_spectral_rasterizer` (or the RGB MLT rasterizer) and
asserts that stream indices used by the eye walk never collide with
stream 48's film/lens/aperture lanes (e.g. by instrumenting
`PSSMLTSampler::StartStream` to log/assert on `streamIndex >= kNumStreams`
distinctness, or by comparing rendered means with `kNumStreams` raised well
past 32 vs the shipped 49). Fixed when either `kNumStreams` is raised past
any reachable depth or the eye walk's stream assignment is reworked to stay
bounded, and the new test passes where it previously would have aliased.

**DL-09 (RadianceEtaScale spatially-varying ior).** Author a scene with a
graded-index object (`ior` bound to a spatially-varying `scalar_painter`)
and an NEE connection or bounce gathered at an interior vertex. Compare
against a reference computed by an independent means (e.g. a fine-grained
multi-shell approximation with piecewise-uniform `ior`). This is presently
unbuilt — the recipe is to build `tests/RadianceEtaScaleGradedIndexTest.cpp`
first, establish whether the mismatch is measurable, then decide whether
`RadianceEtaScale` needs to read the SPF's freshly-fetched value instead of
the stack's push/pop-time one.

**DL-10 (fisheye pixelAR Jacobian).** Author a fisheye-camera scene with
`pixelAR != 1` and a uniform-radiance sphere filling the frame (the same
fixture `tests/CameraImportanceTest.cpp::TestFisheyeFilmResponse` uses at
`pixelAR = 1`, where it integrates to 1.000166). Expected: at `pixelAR !=
1` the closed-form integral departs from 1 by more than measurement noise.
Fixed when `Importance`'s per-pixel solid-angle term folds in the full
Jacobian of `normalize ∘ Stretch(pixelAR,1,1)` and the extended test
converges to 1 at several `pixelAR` values.

**DL-11 (sheen middle-band residual).** Re-run
`tests/SheenDirectionalAlbedoTest.cpp`'s domination-slack check at the
current 64x64 table; currently the middle band (n·v in [kMinSheenAlpha,
0.0349)) reads +0.75% over 1. Fixed when a root-caused correction near the
roughness floor (α ≈ 0.065) brings that band under the same ~0.2% bar the
other two bands already clear, without another blind resolution doubling.

**DL-12 (mirrored UV weave direction).** Add a mirrored-UV mesh fixture to
`tests/WeaveMaterialChunkTest.cpp` (or a render regression under
`scenes/Tests/`) and compare the weave direction rendered across the seam
against an authored `TANGENT`-accessor re-export. Fixed when a
`bitangentSign` companion on `vShadingTangent` recovers the chirality
`vTangent` already has (`RayIntersectionGeometric.h:469-470` vs `:516`)
and the mirrored half no longer flips weave direction.

**DL-13 (displaced-geometry curvature ambiguity).** Add a
`tests/GeometryCurvatureConventionTest.cpp` case comparing `curv` on a
`displaced_geometry` at `smoothing=0` vs `smoothing=1` against the two
closed-form curvatures (base sphere/plane vs the displaced analytic
surface, using `ComputeAnalyticalDerivatives`). This is a documentation
decision as much as a code one: fixed when the descriptor states which
curvature `smoothing` selects and a test pins both numerically.

**DL-14 (ray differentials 0 under BDPT/VCM/MLT).** The `txFootprint` field
on `BDPTVertex` (`BDPTVertex.h:162`) already exists and is carried through
the struct — this is a plumbing gap, not a missing-field gap. Extend
`tests/BDPTVertexRIGRebuildTest.cpp` (or a new
`tests/BDPTRayDifferentialsTest.cpp`) asserting `txFootprint` is populated
(not all-zero) on a `BDPTVertex` in a scene using footprint-driven texture
filtering. Fixed when ray differentials are computed and threaded through
the BDPT/VCM/MLT eye-subpath walk (mirroring the S1 `derivatives`/`signals`
widening already done for the same struct) and the PT-vs-BDPT filtered-band
difference shrinks to noise on a control scene.

**DL-15 (eccentric ellipsoid proximity bound).** Extend
`tests/CrossObjectProximityTest.cpp` with a 4:1 eccentric ellipsoid and a
probe point at true distance 2.75; assert the reported distance against a
tighter, non-uniform bound (or at minimum that the current ~11.0 reading is
logged/warned per-object as the doc's "quantifies per object" note
promises). Fixed when Phase 3's exact-σ work is extended to report a
tighter bound along more than the single top singular vector, or an
explicit per-object inflation warning is emitted at scene-load time.

**DL-16 (ggx_material tangent_rotation scalar-pipe alias).** Add
`tangent_rotation_scalar` (or an equivalent accept-either-pipe resolution
order) to `ggx_material`'s descriptor in `ChunkParserRegistry.cpp`,
preferring Scalar and keeping the Color binding working-but-deprecated.
Fixed when a `tests/ChunkParserRegistryTest.cpp`-style case binds a
`scalar_painter` to the new slot and a `fabric_material` wrapping the same
GGX base can drive both rotations from one shared painter, matching the
already-shipped `weave_rotation`/`tangent_rotation` additive convention.

**DL-17 (glTF anisotropy_rotation).** Implement the sketch in
`CLOTH_FABRIC_DESIGN.md` §15 item 12 (two `channel_painter`s + an `atan2`
`scalar_painter` bound to `weave_rotation`) in `GLTFSceneImporter.cpp`.
Fixed when a glTF fixture with a per-texel anisotropy rotation texture
imports and renders a spatially-varying anisotropic highlight, verified in
`tests/GLTFClearcoatImportTest.cpp` or a new anisotropy-rotation sibling.

**DL-18 (Blender bridge sheen mapping).** Add Principled BSDF Sheen socket
import to the Blender bridge, targeting `fabric_material`. Fixed when a
round-trip test (Blender fixture -> RISE scene -> render) shows a non-zero
sheen contribution matching the authored Sheen weight/tint within the
bridge's existing tolerance conventions.

**DL-19 (S3 sites without a red-proof).** Build a light-tracing-dominated
spectral scene (a caustic through glass, or an emitter reachable only by
t=1 splats) and add it to `tests/SignalEmitterRecordTest.cpp`. Fixed when
that scene's BDPT NM-hero `Le` rebuild and HWSS `rigW` rebuild sites move
the suite's numbers by more than a few percent when their signal-replay is
disabled — i.e., a real red-proof exists where none did.

**DL-20 (patch geometry flat curvature).** This is a Phase-4 scope item, not
a bug in the current `valid=false` contract. Fixed when Phase 4 adds a
`curv` implementation for patch geometries and a
`tests/PatchCurvatureTest.cpp` compares it against an analytic patch
curvature at a few parametric points.

**DL-21 (CSG boundary curvature).** Add a case to
`tests/CsgSurfacePayloadTest.cpp` at a CSG boundary edge asserting a chosen
convention (forward the contributing surface's curvature, most likely) and
pin the number. Fixed when that assertion exists and passes.

**DL-22 (BSSRDF entry vertex neutral signals).** Add a probe record to
`BSSRDFSampling::SampleResult` carrying `derivatives`/`signals` at the
entry hit, and extend `tests/SignalIntegratorConsistencyTest.cpp` with an
SSS-entry scene keyed on a geometry signal (e.g. `curv`-driven IOR). Fixed
when that scene's PT and BDPT/VCM renders agree with the closed-form
control the way the other five signal kinds already do post-S1/S3.

**DL-23 (coated_material substrate allowlist excludes fabric).** Add
`WeaveMaterial`/`FabricMaterial` (post-DL-R8 forwarding) to
`CoatedMaterial::IsSupportedSubstrate`'s `dynamic_cast` chain and
`SubstrateAllowlistText()`, then extend `CoatedBRDF`/`CoatedSPF`'s
opposite-hemisphere early-outs the same way DL-R8 extended `FabricBRDF`/
`FabricSPF`. Fixed when a `coated_material` over a `transmission thin`
`weave_material` (a waxed sheer curtain) transmits attenuated light rather
than reading opaque, guarded by a `CoatedMaterialChunkTest` case mirroring
`TestTransmissiveSubstrateForwarding`.

**DL-24 (CompositeSPF 96% coat-over-diffuse energy loss).** This is
`coated_material`'s whole reason for existing (WETNESS_COAT_DESIGN.md's own
routing-around decision), so the recipe is architectural, not a small
patch: replace `CompositeSPF`'s 50/50 `Pdf` and top-wins `GetBSDF` random
walk with a closed-form or properly-weighted mixture, guarded by
`tests/LayeredWhiteFurnaceTest.cpp` configs 3 and 7 (the doc's own named
failing configs) reading near 1.0 rather than losing 96%.

**DL-25 (no painter-sampling builtin in the expression VM).** Add a
painter-sample builtin (e.g. `sample(painter_name)` or an implicit
per-channel accessor) to `ExpressionEval.h`'s function table, gated the
same way `occlusion()`/`proximity()` are. Fixed when a
`tests/TextureExpressionVMTest.cpp` case darkens a textured substrate
through `add_wetness`'s recipe and a render regression shows the texture
detail surviving under the wet recipe, closing WETNESS_COAT_DESIGN.md §6.4
clause 2's refusal.

**DL-26 (add_wetness / add_wear mutual exclusion).** Design the composable
representation (WETNESS_COAT_DESIGN.md doesn't sketch one) and extend
`AgentSession.cpp`'s qualifying predicates so both verbs can commit to the
same material. Fixed when `tests/AgentAddWetnessTest.cpp` and its `add_wear`
counterpart both gain a case that applies one verb after the other on the
same target and asserts non-refusal plus both effects visible in the
emitted CST.

**DL-27 (wet-highlight variance unmeasured).** Render the wetness recipe's
`scattering` ceiling at increasing sample counts with `oidn_denoise FALSE`
and measure RMSE-vs-reference per the `variance-measurement` skill. Fixed
when a number exists and, if the cost is too high, the recipe's ceiling is
adjusted with that number cited.

**DL-28 (water-absorption file pre-conversion trap).** Add a parse-time or
load-time sanity check on `colors/water_absorption.spectra`-shaped files
(e.g. flag values that look like a raw `sigma_a` table — order-of-magnitude
too large for a `[0,1]` transmittance) or provide an `exp`-based scalar
painter transform so a published table can be dropped in directly. Fixed
when a malformed file produces a diagnostic instead of a silently wrong
render, or the transform closes the gap outright.

**DL-29 (measured tau curve grey under RGB).** Give
`PiecewiseLinearScalarPainter` (or a sibling) an RGB-aware evaluation mode
that integrates the curve against the CMFs instead of point-sampling one
wavelength. Fixed when `HasPerChannelVariation()` can return true for such
a painter and a render regression shows the depth tint surviving under
`pathtracing_pel_rasterizer` without the three-number RGB authoring idiom.

**DL-30 (CSG exit-designated subtraction dndu pairing, untested).** Author
a nested-CSG scene that reaches a subtraction's exit-designated branch
(the doc names it as the one construction that can) and add it to
`tests/CsgSurfacePayloadTest.cpp`, asserting `dndu`/`dndv` stay sign-paired
with `vNormal` there the way the 2026-08-29 sign-pairing family already
covers the other CSG branches. Fixed when that case exists and passes.

**DL-31 (mesh neighbour is a sheet).** Add a robust closed-mesh
containment test (e.g. parity ray-cast or a precomputed winding number)
and wire it into `Object::DistanceToSurface`/`interior()` for mesh
geometry. Fixed when a receiver point buried inside a closed mesh
neighbour reads `interior(r) > 0` in a `ProximitySignalTest` case, matching
every solid family's existing behavior.

**DL-32 (standard_object single-number scale degenerates silently).**
Add a parser-time diagnostic when a `DoubleVec3`-typed `scale` is given a
partial fill (or an explicit single-scalar broadcast convention, if that
is the intended authoring shorthand) on `standard_object`. Fixed when
authoring `scale 0.35` either broadcasts predictably to `(0.35,0.35,0.35)`
with a stated convention, or fails to parse with a clear message, instead
of silently vanishing the object and refusing every proximity query
against it.

**DL-33 (interior's candidate-walk cost shape unmeasured).** Instrument
`BVH::ForEachContainingPoint` the way `NearestOtherSurface` is instrumented
(distance-call / node-visit counters) and measure it on the same Sponza-
class scene the doc's own `11.08`/`5.617` figures come from. Fixed when a
comparable number exists and, if it regresses badly on a pathological
scene (e.g. deeply nested containing volumes), a prune or early-out is
added.

**DL-34 (interior under-reads inside a union overlap).** Change the union
composite's interior/depth export inside the zero-set overlap from
`min(f_A,f_B)` to `max(depth_A,depth_B)` (matching the doc's own worked
example), guarded by a `CrossObjectProximityTest`/`ProximitySignalTest`
case at the doc's exact two-sphere configuration asserting the exported
depth is within measurement tolerance of the analytically-derived
1.886796, not the under-reading 1.6.

**DL-36 (bounded-neighbour-read residual on a luminary probe).** Add a
red-proof scene to `tests/SignalEmitterRecordTest.cpp` with a louvred or
finely-corrugated single-object luminary (blade pitch under ~0.5% of its
diagonal) and assert the probe either reads that neighbouring surface's
channel consistently (documented as an acceptable second-order effect) or
is tightened to refuse it. Fixed when the scene exists and the chosen
behavior is pinned by an assertion rather than left as a disclosed-only
residual.
