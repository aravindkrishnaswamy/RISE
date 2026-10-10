# Batch 8 DL-483 single-label guided draws

Scope: preserve reflection on a guide draw when the material declares only reflection. Hair qualifies; mixed-label materials retain the old diffuse convention. No direction, throughput or PDF formula changes. PT and shared BDPT eye/light generators call the helper; VCM consumes the shared walks.

Production red proof: trained-field hair BDPT eye generators, RGB and NM 550, diffuse cap 0/glossy cap 8, 512 independent seeded draws per case. Before: wrong stamps 360/512 in each one-sample case and 152/512 in each RIS case (9 passed/4 failed, rc1). After: wrong stamps 0/512 throughout (13/0, rc0). Changed outgoing directions stay 360 and 152. These are deterministic generator-label checks, repeat n=1, SD=0, not salted radiance estimates.

Full BDPTGuidedContinuationTest 177/0, PTGuidingMISPartitionTest 185/0, ConnectionTypeSplitTest --dl502-only 55459/0, BDPTDepthCapMISTest row P 26/0: build/run rc0, no compiler warnings. Named builds use make -C build/make/rise -j8 build-test/<Name>.

Strict row M remains red, 1/1 rc1: weave gap transmission count 1, expected 0. The single-label guided metadata status is now OK for both interior and endpoint. Mixed-label guide expectations remain unresolved. No new direct PT or light-generator label witness; the production regression witnesses BDPT eye RGB/NM only.

Self-review: single reflection mask equals the SPF label for every hair order; the helper is called only on guided vertices, inactive guiding follows unchanged behavior; guide ownership is non-owning and released after generators; no PDF/energy/MIS rescaling changes. No P1 found in self-review. No performance claim.
