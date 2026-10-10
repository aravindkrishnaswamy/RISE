# RISE debt batch 7 results

No push. Production changes are limited to hair’s connection-type declaration/value split and the zero-translucent-cap early rejection in the shared bidirectional SSS walk. Coated/fabric/weave splitting and positive subsurface-cap counts remain open. Composite, legacy shader-op transport, motion state, VCM volume merging and BDPT medium strategies were not changed. No source file, class or shipped scene was deleted.

All reported SDs below are sample SDs across whole-image salted repetitions, unless a deterministic metadata/hash witness is explicitly marked n=1. Named test builds checked actual return codes and compiler warnings; no full-repository test run was used. No new timing claim is made.

| Item | Status | Cause / exact slice | Commit | Gates |
|---|---|---|---|---|
| DL-502 | Partial | Hair SPF labels every order reflection; default all-type declaration rejected its bidirectional cap paths. Whole RGB/NM value is now the reflection part. Coated/fabric/weave need aggregate-sampling density shares and reverse consistency; composite is frozen. | `cfa51a6ea`, warning inventory `ab97fed9c` | ConnectionTypeSplit full 302377/0; hair-only 18487/0; depth-cap K 8/0; HairBSDF 2487/0; HairMaterialChunk 44/0; SPFBSDFConsistency full pass. Weave declaration pin stays red. |
| DL-482 | Partial | Shared eye/light RGB/NM walk now terminates a selected subsurface jump at translucent cap zero, preserving the Fresnel reflection probability. Positive-cap jump counts and endpoint/interior consistency remain unimplemented. | `672600fd5`, limitations `3ee6af9a6` | Depth-cap L 16/0; SSSCriticalPartition 45078/0; RandomWalkSSS full pass; SSSHWSSCompanion 23/0. Optional positive-cap metadata pin stays red (two jumps counted as zero). |
| DL-483 | Partial, pinned | Gap refraction stamp consumes transmission count 1 where pass-through convention expects 0; guided hair interior counts diffuse 1/over while connection endpoint counts 0/OK. No estimator fix. | `2072a7382`, limitations `fa747c17e` | Strict opt-in depth-cap M 0/2 rc1, n1 SD0; normal partition P26/0; ConnectionLegality319/0; WeaveMaterialChunk296/0; BDPTSeeThroughMISPartition8/0. |
| Historical image drift | Closed investigation | Existing intended DL-368 pixel-centre correction: `e19288fe7`, integrated `8dcf20d69`. Exact old/new hashes reproduced at the adjacent boundary and direct implementation parent. | Investigation on `debt-codex7-521`; no new debt row | [Detailed evidence and gates](BATCH7_IMAGE_DRIFT.md). |
| DL-449 | Closed, superseded | Same opt-in SMS cost problem as DL-500. Historical measurements copied verbatim into DL-500 and legacy-SMS comparator distinguished from SMS-disabled costs. | `040800586`, integrated branch `75babe69c` | Text-preservation/recount checks; SMSExtendedAllocation6/0. No production change or applicable red→green test. |

## DL-502 render evidence

Red original production: hair-only split gate **1/9 (rc1)**; the cap render gate **4/4 (rc1)**, with every capped pipeline black (**mean0, SD0, n4**). Green cap gate **8/0 (rc0)**. Four pipelines, n8 paired common ValueSalts each:

| Pipeline | Cap-free mean / SD | Binding cap one mean / SD |
|---|---:|---:|
| BDPT RGB | 1.061081 / 0.008103 | 1.060927 / 0.008058 |
| VCM RGB | 1.048164 / 0.006846 | 1.048072 / 0.006865 |
| BDPT HWSS | 1.099853 / 0.039241 | 1.099602 / 0.038970 |
| VCM HWSS | 1.082071 / 0.043760 | 1.081969 / 0.043760 |

Cap-free and non-binding-100 captures retain all 32 compared pixel hashes across red/green builds. The index-matched shell/single receiver fixture’s cap is active in the mechanism but should not truncate its paths; it isolates the endpoint exclusion, not general truncated multi-bounce accuracy. An independent-salt exploratory RGB comparison missed 3 combined SE; the ledger records that limitation rather than claiming arbitrary-salt calibration. A forward-MC-green weave prototype was rejected and not shipped.

## DL-482 render evidence

Native PT is the independent zero-cap reference; n4 ValueSalts per pipeline, gate 3 combined SE with no relative floor. Values are mean / SD.

| Transport / pipeline | Before | After | PT reference |
|---|---:|---:|---:|
| Diffusion BDPT RGB | 0.544676 / 0.004707 | 0.037462 / 0.000987 | 0.038018 / 0.000310 |
| Diffusion VCM RGB | 0.545374 / 0.004580 | 0.037585 / 0.000880 | 0.038018 / 0.000310 |
| Random-walk BDPT RGB | 0.509577 / 0.002765 | 0.037706 / 0.000653 | 0.037850 / 0.000248 |
| Random-walk VCM RGB | 0.509621 / 0.001437 | 0.038196 / 0.000247 | 0.037850 / 0.000248 |
| Diffusion BDPT HWSS | Not used as a red witness | 0.039673 / 0.002604 | 0.038028 / 0.000323 |
| Diffusion VCM HWSS | Not used as a red witness | 0.039433 / 0.003235 | 0.038028 / 0.000323 |
| Random-walk BDPT HWSS | Not used as a red witness | 0.038786 / 0.002419 | 0.038379 / 0.000295 |
| Random-walk VCM HWSS | Not used as a red witness | 0.038232 / 0.002585 | 0.038379 / 0.000295 |

The positive-cap two-sphere render was nondiscriminating. The retained synthetic two-jump metadata pin reports count0 instead of2 and OK instead of over at cap1; it intentionally fails. No positive-cap repair or general SSS partition closure is claimed.

## DL-483 limits

Row M is helper-level metadata, not a trained guiding-field render. Its metadata is hard-coded; synthetic gap connectibility differs from production but does not affect the interior count; guided equality alone permits joint rejection. These P2s remain disclosed per the instruction to fix P1s only. Rejected salted gap render prototypes already failed their uncapped controls and were not landed as causal regressions. Fresh final accounting/evidence reviewers found zero P1 on the committed pins.

## Integration

Master merges before the image investigation: `e3057ef08` (hair), `64cbd366f` (zero SSS cap), `a83ee56e8` (hair warning inventory), `98cc0bff7` (strict gap/guide pins), `92d746961` (449 superseded). Numeric-ID recount after the last of these: **332 rows, 21 open, 311 closed**. Implementation branch commits carry the requested co-author trailer; the three intermediate merge messages `a83ee56e8`, `98cc0bff7`, `92d746961` omit it. Shared master history was left intact.
