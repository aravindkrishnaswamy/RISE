# DL-502 coated split design

Written before implementation, against master 987b43824. The walk assigns an
aggregate f/q to each ray. Its label-t expectation is f*q_t/q, where q_t
includes the actual branch probabilities, refracted and recycled density
shares. Splitting physical coat/substrate terms is therefore incorrect.
These directional shares generally differ when the path is reversed. A
forward-only split would give light and eye strategies different capped
integrands; it will not be shipped merely because forward MC is green.

Exact first slice: a live, exact UniformScalarPainter coat_weight=0 over
exact LambertianMaterial or OrenNayarMaterial. Every sampled branch is
labelled diffuse (including refracted/recycled substrate proposals); the
coat reflection branch has probability zero. Thus q_diffuse/q=1 in BOTH
directions, and the entire RGB/NM BSDF belongs to diffuse. CoatedMaterial
will declare this split only for these audited base dynamic types. The
BRDF retains an immutable audited-base flag and checks the LIVE weight
binding, so SetCoatWeight cannot leave a stale split declaration.

No sampler, aggregate BSDF, random draw, density or cap-free path changes.
Mixed positive coverage, textured weight and other bases keep their old
refusal and a strict positive-coat failure. Weave is untouched. Full closure
requires an explicit adjoint labelled-walk design, not just endpoint shares.

Gates planned: ConnectionTypeSplitTest identity/labelled MC vs quadrature
and reverse identity, coated receiver versus independent plain Lambertian
twin in BDPTDepthCapMISTest; baseline cap-free/nonbinding pixel
hashes with four ValueSalts per pipeline, related coated material, white
furnace and SPF/BSDF suites. Red-before/green-after and strict residual kept.

## Split evidence and self-review

The new isolated production split harness is red on master:1/18, rc1. The exact zero-coverage slice passes36976/0, rc0, including identity, reverse identity and labelled SPF Monte Carlo against angular quadrature, plus live weight rebinding. MC uses400000 draws per incidence/channel and360x720 quadrature cells. For the Oren-Nayar 70-degree NM550 diffuse component, MC.496161 SE.000144 versus quadrature.496310. Both diffuse base types keep exactly one label; no physical lobe is added or counted twice.

Self-review checked dynamic-type refusal of unknown bases/weight painters, live SetCoatWeight handling, positive coverage fallback, unchanged aggregate value/density/SPF and RGB/NM consistency. The end-to-end twin is a zero-coverage slice with shell IOR1.0 (not a claim about positive-coverage caustics). An early fixture accidentally replaced coat_ior instead of shell ior; those render artifacts are discarded. The corrected fixture was rebuilt against production master and against the fix, both rc0 without warnings; the controlled one-thread row Q passes8/0, rc0. The original default-thread hash failures are discarded as uncontrolled scheduling evidence; fixed-thread cap-free/nonbinding hashes are all64/64 identical before/after.

Strict positive-coat pin remains36976/9, rc1. This witnesses the unsupported nine split declarations rather than weakening their assertions.

## Binding-cap coverage correction

Row Q's IOR1.0 shell makes its finite glossy cap physically non-binding; it tests the conservative strategy refusal and preserves baseline hash comparability, but does not alone meet the binding-cap gate. Added row R keeps the glass shell IOR1.5 and sets max_glossy_bounce0: glass Fresnel-reflection paths have strictly positive probability and are removed by that cap. Compare zero-coverage coated and plain Lambertian receivers under the SAME cap, using eight paired ValueSalts in all four pipelines. A standalone driver includes row R verbatim so it can be built/red-tested with production master and green-tested with the fix without relinking queued binaries. Production master row R:0/4, rc1, all four coated means0 SD0 (n8). Green:4/0, rc0. BDPT RGB coated/twin1.948922 SD.011974; VCM RGB coated2.037352 SD.043205 vs twin2.037240 SD.043937; BDPT HWSS coated/twin1.976796 SD.055580; VCM HWSS coated2.031816 SD.081976 vs twin2.032191 SD.081654. All paired-salt comparisons pass3 combined SE with no floor. Both baseline/green named wrapper builds rc0, warning-free. Original BDPTDepthCapMISTest was rebuilt warning-free (rc0) after current master integration; row R4/0 and row Q8/0, rc0. Its64 cap-free/nonbinding hashes also match the preserved master exactly.

Full ConnectionTypeSplitTest376321/0 and related CoatedMaterialChunkTest, LayeredWhiteFurnaceTest and SPFBSDFConsistencyTest all ran rc0. Strict positive coverage remains red. Cap-free/nonbinding baseline64 hashes exactly match the fix, controlled one-thread/reserve0. No timing claim.
