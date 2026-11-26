# Batch 8 DL-536: owned MT copy cursor

The source owns its 624-word state. Its implicit copy copied `next` into that source array. Explicit copy construction and assignment now copy initialized words and rebase a live cursor. Unseeded and first-draw/block-boundary states do not consume the cursor, and their copies avoid reading it. Self assignment preserves the stream. No layout, sampling formula, uncopied sequence or scene changes.

`MersenneStateCopyTest` uses independent `std::mt19937` output. Each case consumes the source after copying; it then checks 1280 copied draws. Warm counts 0, 13, 623, 624 and 625 cover first draw and twist boundaries for construction and assignment. Red on the unfixed library: **16 passed / 6 failed**, rc 1; warm 13 and 625 had **611/1280** and **623/1280** mismatches, warm 623 had 1/1280. Green with `--lifetime`: **23/0**, rc 0, all mismatches zero, including 4096 draws after source destruction. These are deterministic integer checks, n=1, sd=0, not render variance estimates.

Named builds use `make -C build/make/rise -j8 build-test/<Name>`. Related guards: `SobolDimensionBudgetTest`, `SampledWavelengthsTest`, and `DeprecatedMaterialRenderIdentityTest` pass. Supplemental clang ASan/UBSan build and lifetime run pass without diagnostics. Build logs contain no compiler warnings. Ignored local logs: `.claude/logs/codex8/536-{red,green,confirm}-*`.

This adjacent P1 was found while making DL-367 render replay deterministic. Reseeding an existing generator avoids copying, but leaving public generator copies broken would retain a real lifetime defect. This row does not settle DL-367's physical spp/batching dependence.
