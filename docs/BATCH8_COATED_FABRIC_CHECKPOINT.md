# Batch 8 DL-417 checkpoint

No production change. `CoatedFabricAccuracyTest --gate` returns 1; 23 of 24 cases fail equality or conservation. Eight independent seeded SPF-walk replicates x 20000 draws, not image renders. Sample SD is reported SE times sqrt(8). The independent explicit layer walk uses the existing test-only live-ambient adapter; its frozen implementation is unchanged.

Clear white fabric at normal incidence: coated 1.00837802 (SD .000776733), reference .995437574 (SD .00615355), ratio 1.01299975, absolute combined 3SE .00657861663. Independently of the layer reference, the coated estimate exceeds the closed-form unit furnace by more than its 3SE .000823850034. Clear silk at 70 degrees: coated .454498866 (SD .00294970), reference .503438677 (SD .00307161), ratio .902788934, absolute combined 3SE .00451690635.

The existing directional-return probes do not define a complete reciprocal BRDF. A full bidirectional polar/azimuth return operator, live-painter cache, and thin-weave exit frame remain unresolved. No scalar normalization, clamp, reference change or band relaxation is landed.

All four named builds used `make -C build/make/rise -j8 build-test/<Name>`, rc0 and no warnings. Positive guards: CoatedMaterialChunkTest 85/0, LayeredWhiteFurnaceTest 63/0, ConnectionTypeSplitTest --dl502-only 55459/0 (run rc0). The physical strict check remains rc1. Actual results are in ignored `.claude/logs/codex8/417-status.json` and `417-run-*`.
