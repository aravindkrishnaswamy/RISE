# DL-37: GGX diffuse interface transmission

Status: CLOSED 2026-09-12 — diffuse composition repaired by `000df0b4`; transport naming correction `d0a8ece0` and film interface range correction `5b69f192`. Independent specular-only failures remain DL-63 with their test exit preserved. DL-62 and DL-64 (both flagged as independent residuals below) were subsequently CLOSED 2026-09-13 — see [DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md](DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md).
Base master: `35a6f45dff7b415ece9b23d3a242ffbeb20514e8`.

The existing Schlick diffuse lobe subtracts constant maximum RGB F0 while rough specular Fresnel increases toward grazing. Conductor and thin-film modes add diffuse without interface attenuation. The same additive composition must be corrected in BRDF evaluation and selected diffuse sample throughput.

Use a reciprocal single-pass interface model: `f_diff(i,o) = c/pi * (1-A(i)) * (1-A(o))`. Here A is Schlick interface Fresnel with authored F0, or conductor/film interface Fresnel times the authored specular tint. The film RGB interface proxy is projected to [0,1] before complements, and its angular mean integrates that same projected function. RGB evaluates each component independently; NM evaluates at the requested wavelength, preserving ambient IOR and film dispersion. The two factors describe transmission into and out of the Lambertian substrate. Higher diffuse round trips are absorbed rather than recycled. This model permits energy loss.

A one-direction factor is rejected because it breaks reciprocity. Normalizing the macro-interface product by `1-Aavg` is also rejected: it restores diffuse energy using an interface average that is not the directional reflectance of rough GGX. Exact energy-preserving product compensation would require the integrated single- and multiple-scattering rough specular lobe, including anisotropy and spectral film parameters; the current isotropic F=1 LUT does not supply that quantity.

The chosen diffuse integral is exactly `c*(1-A(o))*(1-Aavg)` and its cosine-weighted mean is `c*(1-Aavg)^2`. Albedo guides will combine those terms with the existing approximate macro-interface specular estimate, explicitly documenting that approximation. Sampling retains its lobe probabilities and densities; only selected diffuse throughput receives the same two-direction factor as evaluation.

This is a rough-interface approximation, not a theorem that every possible thin-film stack and anisotropic specular configuration conserves energy. Validation must measure the full unchanged specular-plus-new-diffuse model. Test isotropic and anisotropic low-F0 white furnaces, bare and mixed conductor, thin-film mixtures, RGB/NM agreement, reciprocity, sample/evaluation consistency, and a broader roughness/F0/angle sweep. Specular-only controls distinguish defects in the existing specular model from diffuse composition failures. Do not loosen an energy bound to hide a discovered failure.

Commit the tests before running them against the unfixed library. Save their failing output. Then implement, run each suite referencing changed classes, clean Make and Xcode builds, capture sequential before/after linear renders, and obtain fresh independent transport, test, and documentation reviews before closure.

Independent audit residual: DL-62 records the reachable glossy-filter roughness disagreement between GGX sample/density and evaluation. It is a different bug pattern and is not repaired in this slice.

Independent observed residual DL-63: the broader committed test reports the same three specular-only F0=1 failures on the unfixed and repaired libraries, with diffuse exactly zero. The LUT generator integrates the separable VNDF weight G1(wo), whereas GGXBRDF evaluates height-correlated G2. Correcting that specular compensation model is a separate slice. The current test retains a nonzero exit for those failures.

Independent static residual DL-64: Schlick F0=0 still has nonzero oblique reflection, but GGXSPF selects specular and MS lobes using weights proportional to F0. Selected-lobe throughput therefore omits those lobes. The new row requires its own committed sampling red proof; it is not fixed by diffuse attenuation.

## Validation and limits

Committed primary red proof `a7c11d14` failed four of 57 configurations. The first test commit required a compile-only Pel addition correction before any execution; that failed build is retained. The unchanged test after the fix passed all four DL-37 energy regressions and failed two prior-model coated measured pins. Those pins were recomputed in `b03950c5` at the unchanged 0.005 tolerance. Final result: `0 of 57 configurations failed.`

Four-angle curves from this run, ordered 0, 30, 60, 80 degrees:

| Config | Unfixed | Fixed |
|---|---|---|
| 17 | 0.9990, 0.9993, 1.0264, 1.1555 | 0.9168, 0.9175, 0.9169, 0.7345 |
| 20 | 0.9967, 0.9968, 1.0103, 1.0684 | 0.9142, 0.9151, 0.9000, 0.6485 |
| 54 | 0.5211, 0.5185, 0.5006, 0.5026 | 0.5211, 0.5185, 0.5006, 0.5026 |
| 55 | 1.5181, 1.5216, 1.5000, 1.5042 | 0.7452, 0.7395, 0.7290, 0.7048 |
| 56 | 1.5360, 1.5167, 1.4926, 1.4800 | 0.7947, 0.7774, 0.7640, 0.7256 |

The independent BRDF integration test reports `GGXDiffuseTransmissionTest: 150 checks, 46 failures` before and `GGXDiffuseTransmissionTest: 150 checks, 3 failures` after. The three remaining specular-only failures are identical to baseline and belong to DL-63. All assertions and exit 1 are retained. The estimator uses a 50/50 cosine/VNDF mixture independent of material lobe selection, all attempted draws in the denominator, per-channel moments, and the fixed bound `1 + 6*SE + 0.005`. RGB/NM reciprocity includes back faces. This measured coverage is not an all-parameter theorem for arbitrary film stacks.

The ledger's original fifteen-site inventory was wrong: seven constant-split formulas existed, comprising two selected diffuse SPF arms, two BRDF evaluations, one directional albedo and two hemispherical albedo methods. Other cited sites were specular Fresnel or MS terms. All seven consumers now share the interface model. Pdf/PdfNM retain their probabilities because cosine sampling remains a valid proposal for attenuated diffuse. RGB/NM, NM's HWSS evaluation fallback, conductor tint, film dispersion, ambient IOR, and coated/fabric guide consumers were audited. DL-62 and DL-64 remain separate sampling/evaluation defects.

Clean Make and clean Xcode builds passed with zero compiler warnings. Xcode emitted three expected missing-local-OIDN path notices and one AppIntents metadata notice. Nineteen suites ran individually: eighteen passed; the independent sweep retained only the DL-63 baseline failures. The grid test's obsolete constant-split oracle was replaced by analytic entry/exit transmissions at its unchanged tolerance. SourceHygiene's initial infinity-sentinel finding was corrected using explicit finite-moment checks; original failing logs are retained.

The baseline image completed before the modified image began. Both use the default multithreaded renderer, seeded immediately before each render; scheduling can change their noise. The fixture is 384x128, 64 samples, direct lighting, box filtering, OIDN disabled, and linear Rec.709 EXR. Both images have zero nonfinite and zero negative components. Display previews share exposure; no variance or bit-reproducibility claim is made. The unfixed checkout contains only committed test changes.

The final clean builds and test run started at `843c20582ede54a35646f051eff48527634b8ef8`. Subsequent edits change documentation and one inline API comment in IJob.h; executable inputs are unchanged, verified by comparing the header with inline documentation removed. Fresh reviews, merge validation, standalone report, checksums and cleanup are recorded in the completion evidence.

## Review correction: film RGB interface range

Transport review found that a passive dispersive film spectrum can map to an RGB preview channel above one. The committed probe `082bf260` reproduced selected diffuse throughput `(-0.356232131812, 0.531227696068, 0.00646297438237)`, exposing a new negative complement. The RGB interface adapter now projects the tinted preview to [0,1] before computing transmission. This bounds the input reflectance representation rather than clamping transported throughput. Its mean integrates the same projected directional function; projecting an already averaged value would be inconsistent. NM spectral optics and the existing RGB specular radiance proxy are unchanged. The fixed regression reports `diffuse RGB 0 0.531227696068 0.00646297438237` and `GGXFilmTransmissionRangeTest: PASS`. Supplemental guide quadrature differs by `3.77307488675e-06`; these supplemental checks were added after the fix and are not additional red proof. Updated gates and fresh reviews supersede the first review round.
