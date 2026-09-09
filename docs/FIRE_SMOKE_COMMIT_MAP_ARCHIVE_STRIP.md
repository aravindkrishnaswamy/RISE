# Commit map: archive strip of `fire-smoke-design` (2026-09-08)

The 190 unpushed commits on `fire-smoke-design` carried 8.8 GB of run-artifact
archives (`rendered/**/*.tar.gz`, split `*.tar.gz.part-*`, and `*.checkpoint`
files), which made the branch unpushable to GitHub. Those paths were removed
from history with `git filter-branch --index-filter`; every commit was kept
(no pruning), so the mapping below is one-to-one and message-preserving.

The bytes were NOT deleted from disk — they remain under `rendered/` on the
producing machine, untracked and covered by the top-level `/rendered`
ignore rule — and their SHA-256 manifests (`*.sha256`, `*.v1` evidence
records) remain in git, so provenance is preserved the same way the HITEMP
raw-bytes decision preserved it: manifests in git, bytes outside.

Any document or evidence record citing an OLD hash below refers to the
corresponding NEW hash. The pre-strip tip is preserved locally as
`refs/backup/fire-smoke-design-pre-strip`.

| old | new | subject |
|---|---|---|
| `722144bf20fb` | `722144bf20fb` | Add compatible FCT momentum oracle seam |
| `1c9227ee25ea` | `1c9227ee25ea` | Scaffold strict FCT Heun Metal diagnostic |
| `8987f73fd53a` | `8987f73fd53a` | Fail closed for unsupported FCT Heun diagnostic |
| `ccab66efe463` | `ccab66efe463` | Add strict fp32 scalar FCT oracle stages |
| `837f17b08bae` | `837f17b08bae` | Tighten scalar FCT oracle authority |
| `c253d4cc3a0d` | `c253d4cc3a0d` | Execute single-stage production FCT diagnostic |
| `93e696c05ba4` | `93e696c05ba4` | Align unsupported single-stage FCT diagnostic |
| `6c1ccfc62c9e` | `6c1ccfc62c9e` | Complete single-stage compatible FCT bootstrap |
| `aa5037b968d9` | `aa5037b968d9` | Add tokenless single-stage FCT onset owner |
| `bd8cb67c156d` | `bd8cb67c156d` | Honor affine envelope in fire source staging |
| `f393c7cda9da` | `f393c7cda9da` | Bootstrap faithful fire transport staging |
| `39ad06259ecf` | `39ad06259ecf` | Add authenticated fire EOS prerequisite |
| `eea6fa0cab37` | `eea6fa0cab37` | Add exact Heun flux composition prerequisite |
| `df2ec4fe06e3` | `df2ec4fe06e3` | Add canonical fire source authority |
| `4bccb637d9dd` | `4bccb637d9dd` | Publish canonical fire divergence target |
| `ae8b1bd05497` | `ae8b1bd05497` | Compose authenticated fire divergence target |
| `532d0d4b60e2` | `532d0d4b60e2` | Add authenticated initial projection target |
| `7371ed6b64a5` | `7371ed6b64a5` | Add authenticated projected Heun CPU owner |
| `b440bffc56d2` | `b440bffc56d2` | Complete authenticated projected Heun owner |
| `0e70f17f1895` | `0e70f17f1895` | Harden complete projected Heun owner |
| `73d6ff247c07` | `73d6ff247c07` | Complete authenticated projected Heun owner |
| `5231af3137df` | `5231af3137df` | Harden projected Heun owner publication REDs |
| `1b9dc135ea7d` | `1b9dc135ea7d` | Prove stale r70 refusal is retryable |
| `dfc03fc97127` | `dfc03fc97127` | Close final projected Heun review gaps |
| `b11f20824bb1` | `b11f20824bb1` | Harden projected Heun owner authority |
| `539acac885ab` | `539acac885ab` | Close projected Heun test and manifest gaps |
| `935f103a2cbf` | `935f103a2cbf` | Close accepted-target owner authority gaps |
| `034d4af56152` | `034d4af56152` | Complete projected-Heun R0 R1 R2 owner authority |
| `cc1031f7c10b` | `cc1031f7c10b` | Correct Metal context detection and seal device qualification |
| `b0a4dad10d3a` | `b0a4dad10d3a` | Record r192 tier-8 live-trajectory rejection |
| `8160c2670244` | `8160c2670244` | Record r193 operator identity stop |
| `d3957bcff109` | `d3957bcff109` | Seal r194 tier-8 baseline replay |
| `d01f7a6614bd` | `d01f7a6614bd` | Publish r194 replay summaries atomically |
| `d8d12088c506` | `d8d12088c506` | Record r194 tier-spanning baseline verdict |
| `b79169f23070` | `b79169f23070` | Correct r194 independent schedule wording |
| `419719bb3f20` | `419719bb3f20` | Author r194 tier-spanning replay entry |
| `326477d105a9` | `326477d105a9` | Clarify r194 SGS evidence |
| `ad6f2668c2e8` | `ad6f2668c2e8` | Record r195 live-owner prerequisite audit |
| `1d0566160ffe` | `1d0566160ffe` | Authorize resident candidate lineage |
| `0d08a7c2277d` | `0d08a7c2277d` | Add resident Metal transport authority |
| `34bcb143dd85` | `34bcb143dd85` | Clarify resident transport validation units |
| `df35bdedbf89` | `df35bdedbf89` | Add resident physical-flux authority |
| `e776ac74fa12` | `e776ac74fa12` | Add resident EOS candidate identity |
| `5967a302685e` | `5967a302685e` | Correct resident EOS lineage and bounds |
| `bfbd66ab93d1` | `bfbd66ab93d1` | Correct resident EOS candidate lineage |
| `b417bcbd0e7b` | `b417bcbd0e7b` | Correct resident EOS authority proof |
| `4fd07d1ae9be` | `4fd07d1ae9be` | Close resident EOS authority review findings |
| `83f526c06c09` | `83f526c06c09` | Prove resident EOS log and publication rounding |
| `10e6744d3cdf` | `10e6744d3cdf` | Exhaust resident EOS log and rounding domain |
| `cdb6a3c78201` | `cdb6a3c78201` | Bind resident EOS qualification to Metal identity |
| `8f8a8c23436d` | `8f8a8c23436d` | Seal resident EOS candidate identity |
| `cfd985c90032` | `cfd985c90032` | Add authenticated resident target lineage |
| `5ffaf06a2b94` | `5ffaf06a2b94` | Fix resident target authority validation |
| `11214b149367` | `11214b149367` | Harden resident target authority proof |
| `1b779752b23c` | `1b779752b23c` | Authenticate resident target parent conjunction |
| `d48861538e0c` | `d48861538e0c` | Close r200 source-lineage review findings |
| `477d298fbb11` | `477d298fbb11` | Close r200 parent and residency proofs |
| `1aa1f266de91` | `1aa1f266de91` | Fix traced binary32 identity comparisons |
| `a7e45b516844` | `a7e45b516844` | Close r200 mirror and Q-lineage review |
| `f7a89ab53b3a` | `f7a89ab53b3a` | Seal r200 target lineage rung |
| `9fa2691a72e2` | `9fa2691a72e2` | fire: bring projected Heun owner live on Metal |
| `196e046373c8` | `196e046373c8` | fire: reject unverified Metal owner rung |
| `5b3800825446` | `5b3800825446` | fire: retain r194 and r201 validation evidence |
| `da98b8d29625` | `da98b8d29625` | fire: repair resident projected-Heun owner |
| `85251c5d95a0` | `85251c5d95a0` | fire: preserve no-Metal projected-Heun refusal |
| `87bc6dee569b` | `87bc6dee569b` | fire: complete resident projected-Heun owner gate |
| `9382965c4944` | `9382965c4944` | fire: seal r201 owner and kernel evidence |
| `7584466b8b91` | `7584466b8b91` | fire: repair shared-alpha resident Heun owner |
| `4c9097530a7c` | `4c9097530a7c` | fire: record rejected r201 coupled-owner gate |
| `07a84fb92283` | `07a84fb92283` | fire: repair projected-Heun owner trace gate |
| `49f19a8c775b` | `49f19a8c775b` | fire: bind owner working set to Metal capacity |
| `04e1757b1dae` | `04e1757b1dae` | fire: certify continuous owner enclosures |
| `81f1acbaa747` | `81f1acbaa747` | fire: gate resident owner iteration trajectory |
| `d3e4b9c8a526` | `d3e4b9c8a526` | fire: reproduce resident R2 lineage defect |
| `e45783a0795c` | `e45783a0795c` | fire: authenticate resident endpoint classes |
| `136297f25fe7` | `136297f25fe7` | fire: seal repaired resident owner gate |
| `1d464754a6a0` | `1d464754a6a0` | fire: certify resident owner trajectory |
| `0e120eea6c90` | `0e120eea6c90` | fire: seal owner branch obligation gate |
| `3ebe297421c7` | `3ebe297421c7` | fire: certify owner continuous trajectory |
| `012a0d5fc409` | `012a0d5fc409` | fire: seal r201g owner trajectory gate |
| `fea084fcd188` | `fea084fcd188` | fire: restore immutable r201f evidence |
| `fcc45f52557f` | `fcc45f52557f` | fire: certify resident owner iteration trajectory |
| `eb7d1d87ab91` | `eb7d1d87ab91` | Validate resident owner input trajectory |
| `651351b79ecb` | `651351b79ecb` | Seal r201i resident trajectory evidence |
| `a0c3a824df43` | `a0c3a824df43` | Repair resident owner trajectory gate |
| `a5c1d36b1ee7` | `a5c1d36b1ee7` | Seal r201j actual candidate trajectory |
| `b9360ae690f8` | `b9360ae690f8` | Gate resident owner on full-field transfers |
| `fbd3a70b8557` | `fbd3a70b8557` | Seal r201k full-field residency gate |
| `784a266c60e8` | `784a266c60e8` | Stage resident source before owner iterations |
| `104b1da5c810` | `104b1da5c810` | Seal r201l setup-staged source gate |
| `314da2442411` | `314da2442411` | Scale resident device authority seals |
| `f1e3f39cf2fc` | `f1e3f39cf2fc` | Record r201m owner trace gate |
| `2189762be4f8` | `2189762be4f8` | Reinstate payload-bound resident authority |
| `fdf3d0f46d3e` | `fdf3d0f46d3e` | Seal r201m owner trace verdict |
| `b2ce94e43070` | `b2ce94e43070` | Preserve rejected r201m authority evidence |
| `edb4afb69145` | `edb4afb69145` | Instrument owner cost and momentum convergence diagnostics |
| `919a604a8a76` | `a570b71d9bd4` | Seal r202 owner cost evidence and withheld convergence verdict |
| `827608d53fb7` | `dcab769edbbc` | Close r202 diagnostic scope and evidence association REDs |
| `e182035007ce` | `662feda7365a` | Reseal r202 review repairs against exact owner diagnostics |
| `8ffc6ec624aa` | `fd6b9a4345b6` | Require complete fixture verdict in r202 observer qualification |
| `ec33256ca854` | `772c971ff038` | r203: instrument producer kernels and repair certified EOS bin selection |
| `dc215421d2db` | `2aff6ada388f` | r203: pin reproduction and harden diagnostic counter probe |
| `7e903caacbdd` | `14ce50ea0aa7` | r203: seal kernel qualification, EOS witnesses, and exploratory cost evidence |
| `6c03556f314b` | `5d0bc6318752` | r203 review: bind timing to trajectories and require complete EOS witnesses |
| `73d6638c250a` | `612b10a4e9b1` | r203: reseal complete witness and associated producer timing evidence |
| `c22e301d8482` | `69adb69ddfb1` | r203: record fresh three-reviewer zero-P1/P2 verdict |
| `ebf91c2efab2` | `dda13d6b4ad9` | r204: add deterministic payload Merkle v2 and preserve v1 bridge |
| `9666ddeefbf3` | `4cf81ab6755c` | r204: close allocation lifetime and bridge schema REDs |
| `10cd930eebfa` | `ff59220b620f` | fire r204: seal owner publication with Merkle roots, keep intermediates resident |
| `de79d3efc67e` | `4bb95248235a` | fire r204: bind full owner kernel set and close publication review findings |
| `fda111259ed2` | `a7d1c0c33d91` | fire r204: preserve publication attribution and bind cost evidence to executed gates |
| `d6e766579baf` | `7d3783bd2a11` | fire r204: preserve attested placement cost and open wider qualification findings |
| `169c95780d39` | `eac80755b00d` | fire r205: isolate historical refusal and synthetic preview fixtures without repinning expectations |
| `bfca02462d3d` | `90290dbe9a68` | fire r205: close publication preservation and qualification attribution review findings |
| `b97f46c9f4e4` | `aac182e4b18c` | fire r205: close inherited build flags and publication validator RED gaps |
| `7dc6bbf50c91` | `1b787dcd73b2` | fire r205: close makefile injection and bridge restoration coverage gaps |
| `ab5e7d78c21f` | `343e07c15cc1` | fire r205: isolate build environment and require executed owner verdicts |
| `607536694759` | `612ac813d6c8` | fire r205: authenticate disk build configuration and exclude cached recipes |
| `904c57b8f754` | `6b6598b62015` | fire r205: close vendored and wildcard compilation source admission |
| `bf4879ad9fef` | `f74d22c5fe79` | fire r205: include ignored source-like wildcard inputs in admission RED |
| `01985f359058` | `40e5bae9b86c` | fire r205: authenticate repository-local compiler include root |
| `3a732a00fda8` | `57f8adde52d5` | fire r205: bind Git admission and compiler to the same execution context |
| `0c64513c2dfc` | `c15a41f58db3` | fire r205: seal fixture gates, owner cost accounting, and raw claim exports |
| `a660cbc583c4` | `881c43ff4ce6` | fire r205: record fresh zero-P1/P2 signoff and clean-export regression proof |
| `20607aea6da0` | `6a89caf8bee4` | r206: reuse device-produced EOS endpoint enthalpies without changing root search |
| `07f339279e99` | `b3a9d17d86a7` | r206: bind exact-commit EOS qualification and repeated device cost |
| `bf5fe9dcfcb0` | `fba3fbba43d7` | r206 review: require complete counters, distinct runs, and canonical owner fields |
| `5c48670e3a86` | `8c8ded73f976` | r206 review round 2: reject conflicting verdicts and impossible histogram metadata |
| `2780f93a7b36` | `c12f7a59c980` | r206 review round 3: strict completion and profile counter family admission |
| `d4060cd1235a` | `02fc446a31eb` | r206 review round 4: close legacy completion and qualification bypasses |
| `36bd44ae51b0` | `e256fc59704f` | r206 review round 5: strict artifact and named-RED records; correct historical proof |
| `c9993d15c005` | `cb8a7d2f7efa` | r206 review round 6: reject whitespace-delimited contradictory RED counters |
| `82b7c894b6f5` | `644ddb45bbf3` | r206 review round 7: complete exact-source EOS qualification and record dispatch REDs |
| `f2ba99946f1e` | `9dbd92061538` | r206 review round 8: bind trusted EOS execution and close off-mode profile bypass |
| `06b8ac6c2664` | `363c48c98891` | r206 review round 9: bind independent measured executions, not cosmetic log variants |
| `5d9ae2a52426` | `0da05194418d` | Record r206 clean independent review closure |
| `162b46400e14` | `d8dbc65e6674` | Bind crossing convergence diagnostics to accepted resident owner |
| `2b66b5eccecd` | `14f1951bbc86` | Separate crossing RED family from historical convergence fixture |
| `357c93369ff9` | `30823e12141a` | Observe consumed tail targets and distinguish combined projection budget |
| `978a59300469` | `6b73a553e0a7` | Gate crossing and consumed-target evidence with named mutation REDs |
| `500dd40e7a86` | `72f33545837a` | Bind r207 qualification to executed v3 log and reject rehashed evidence |
| `ea703eaa4fe3` | `69c1f6b5ce2a` | Seal r207 crossing qualification and untouched from-zero replay launch |
| `29796a6a0299` | `02616ee09ff4` | Record sealed r207 source identity refusal without a focusing verdict |
| `b39785780f9e` | `d97387d5d7a3` | Carry canonical source bytes once through projected-owner persistence |
| `2790a8ddc7bd` | `9d018f25757c` | Close r208 pilot-ledger coverage gap with authenticated active-source REDs |
| `a0c85dd894a5` | `9d1bb2beb395` | Seal r208 exact-commit owner qualification and retained refusal replay evidence |
| `786d44d162eb` | `ea0343e6d911` | Close r208 soot-ledger false-green with affine qualified inputs and all-field REDs |
| `97ecda96a1c6` | `f0081f1c15fe` | Extend soot carry qualification to every source byte and carbon omission RED |
| `84128a500fe1` | `91755a9fc6c8` | Prove exact source widening with shared byte comparator and sub-float REDs |
| `b4f828b9d3a1` | `e84349ec5e02` | Seal reviewed r208 qualification, source carry REDs and from-zero replay plan |
| `17d31a8d6f57` | `11f16759cae8` | Record r208 sealed from-zero replay launch with matched seed and identity |
| `f8dbc1ec1695` | `c43e2bff3a81` | Separate qualification observer working-set certificate from production cap |
| `1d196178e5c3` | `71677165ce86` | Close observer review gaps in ARC builds and refused checkpoint evidence |
| `04e32fedfb3c` | `3be00078c66c` | Seal r210 observer repair qualification and lossless replay evidence |
| `8561e7b237f4` | `46e2a4f4fa8c` | Add resident r78 v2 certificate and sealed step-1300 continuation |
| `f4a9dd21224e` | `b107a6a54e64` | Bind resident migration traces to executed checkpoint and binary log identities |
| `a16de03694e9` | `4780d9276d47` | Initialize resident continuation cursor through sealed pair publication |
| `c363b643ef8b` | `a59c45af6d16` | Seal r211 executed migration bridges and verified continuation handoff |
| `fdd296e2dd88` | `65d044803cfb` | Adopt ported temporal transport and add same-tier oracle reference rung |
| `e4c9aa792cbf` | `5672c0f00731` | Close r212 cost provenance and mixed-operator resume REDs |
| `854a0d0c3dde` | `4266d02e0808` | Bind r212 kernel timing reread and record refusal scope |
| `15b5d2447f2a` | `121c9317ab0e` | Distinguish r212 executable identity from qualification receipt |
| `845114c89fb4` | `f8f507771acf` | Seal r212 adoption costs, survival archive and hot EOS observation |
| `1c1d70a99cb6` | `34122c27262e` | Reconcile main diagnostic lineage before port adoption integration |
| `b889ad875df3` | `602b62607f3a` | Record main port integration with exact owner-edit preservation |
| `9f1b80fb64a1` | `77b7ad35ab85` | Track compact r212 report inputs for direct reproduction |
| `8be70f5d481d` | `060a26b2d59d` | Add sealed same-tier oracle composition diagnostic without a verdict claim |
| `ee4da5d3abd9` | `90571f0e76f7` | Prototype exact compensated EOS temperature-basis reuse with differential REDs |
| `18183251ed4e` | `ecb560bf4ce0` | Retain oracle composition budget operands and preserve diagnostic refusal cause |
| `5fd95c9092dd` | `e7ffd066fac7` | Report sealed equal-time column context without promoting it to a contract verdict |
| `d1406f4f2bfa` | `ac0d227f4008` | Authenticate composition metadata and add physical-domain filtered diagnostics |
| `aed3fa34476d` | `2c9176cc0e87` | Record r213 isolated hot-owner cost observations and guarded scope |
| `02456eeb1981` | `30b30b0ea235` | Add native eight-step numerical snapshot comparison and semantic schedule REDs |
| `4a79c01721c3` | `5f6368ca4ebb` | Qualify r213 EOS optimization with eight full native payload comparisons |
| `a1b327ee9e26` | `ea7a1cb07cc1` | Bind numerical qualification to executed reporter evidence and refuse joint forgery |
| `e4e8512e48ed` | `c2fc4d3d4c17` | Retain sealed r213 oracle compositions, hot snapshots and review evidence |
| `6f444137733a` | `316e9265a283` | Preserve exact r213 producer and diagnostic executables as sealed evidence |
| `80dc8e126da4` | `8a6fd19804e8` | r214: pin adopted-owner tier-ten onset and filtered-only verdict scope |
| `5a3b0e25d5ae` | `efb98afdf1e7` | r214: retain crossing verdict when it supersedes fixed-column observation |
| `439dc67dddd5` | `abdbc55d9f64` | r214 review: close horizon-crossing false green and all-axis evidence gaps |
| `7668b3de9357` | `2ba24107cabc` | r214: align checked-close declaration with failure-injection template |
| `3dc96320527f` | `9c3951cbc707` | r214 review: preserve true high-boundary face coordinates in live onset evidence |
| `d835215de853` | `e74a3ee297c0` | r214: retain qualified workers, review ledger and sealed onset launch evidence |
| `fbe7bc1213b9` | `73c4817a03fb` | r214: retain prefix host sample and unpromoted target-basis CPU/offline evidence |
| `637a744a7808` | `5e1012f4b334` | r214: capture shared canonical owner inputs and derive fp64 qualification capacity |
| `638d616e4509` | `d2bc2c99faea` | r214: refuse malformed resident input surfaces before shared capture |
| `4b8c1ec16682` | `4f62c59a4be8` | r214: close force-input sibling preflight gaps with named REDs |
| `d831ce31e0af` | `5ee8bafc017d` | r214: seal CPU-prerequisite review and unchanged-onset continuation handoff |
| `3c82a85bdf10` | `051a53cbcf96` | r215: promote bit-identical target cost reduction; preserve interrupted onset |
| `609a5531bbcb` | `c0358005162a` | r215: keep cost promotion separate from sealed onset lineage |
