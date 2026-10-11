# DL-571 CLI exit-status audit

Audit before implementation, master `d50f5c9a2`, 2026-10-10. Searched tracked
scripts, tools, tests, evals, Android, Blender and both GUI launchers for
`bin/rise`, `rise.exe`, executable variables and process/status consumers.

| Caller | Current exit-status handling |
|---|---|
| `run_scenes.sh` | Ignores status; explicitly documents the old always-1 quit. |
| `bench.sh` | Does not gate status; times the console pipeline. |
| `scripts/capture_refactor_baselines.sh` | Records status for diagnostics, gates on a fresh readable nonblack image. |
| `scripts/check_refactor_baselines.sh` | `|| true`; gates on fresh output and image comparison. |
| `scripts/{divergent,bdpt_eye_subpath,bdpt_light_subpath,bdpt_connection,bdpt_transmittance}_baselines.sh` | `|| true`; image/luminance gates. |
| `tests/test_ris_regression.sh` | Both console calls use `|| true`; image gate. |
| `tools/{fabric_sheen_measure,pixel_gate_global_medium}.py` | Ignores subprocess status; removes stale output and checks the produced image. Comments explicitly describe always-1. |
| `tools/eval_enrich.py` | Console render gates on a fresh PNG; status appears only in a no-image diagnostic. |
| `evals/references/generate_references.sh` | `|| true`, then requires the reference file. |
| `tools/perception_ab_eval.py` | Requires rc0 for **agent-stdio**, not the console. |
| `tests/AgentStdioSmokeTest.cpp` | Requires agent-stdio rc0 on EOF. |
| `tests/AgentAutonomyPolicyTest.cpp` | Requires agent-mode rc0 for valid flags and nonzero for malformed flags. |
| `run_all_tests.sh` / `.ps1` | Runs test executables and checks their statuses; does not directly run the console. The subprocess tests above are indirect CLI consumers. |
| `--agent-eval`, `AgentEvalRunner`, eval runconfigs | Dedicated CLI mode returns rc0/1; renderer/evaluation calls are in-process. |
| Android, macOS/Windows GUI, Blender render engine | In-process library bridges, no console exit-status dependency found. Blender's `tangent_bake.py` checks a **Blender bake subprocess**, not `rise`. |

No caller found that requires **1 as a successful console result**. Existing
output-file gates remain compatible with success becoming zero; their workaround comments now explicitly describe the historical behavior.
This slice does not change harness execution or image gates. README/AGENTS/CLAUDE quickstart wording does not
mention exit status, so requires no change.

Design: one sticky `consoleFailed` flag spans positional scene load and the
console command loop. A failed load remains available for partial-scene
debugging, but later successful commands/quit cannot erase its failure.
`ParseCommand` already propagates `load`, `render` and unknown-command failure.
Clean quit or clean stdin EOF returns zero; failed input acquisition (other
than ordinary EOF) returns nonzero. Dedicated agent modes are unchanged.

Regression: the actual executable loads and renders a tiny native-v7 scene,
writes a fresh PNG, and exercises load, render, unknown-command and EOF cases.
Exit-status assertions are deterministic (n1, SD0); no radiance or timing claim.

Validated production baseline: 9 passed/4 failed, rc1; the fresh PNG check passes.
After the fix: 13/0, rc0; AgentStdioSmoke 13/0 and AgentAutonomyPolicy404/0,
rc0. All four named builds rc0 and no compiler warnings. Early malformed
output-fixture runs were discarded; they are not the red regression evidence.
