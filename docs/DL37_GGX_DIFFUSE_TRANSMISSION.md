# DL-37: GGX diffuse interface transmission

Status: design selected before committed red proof; implementation and validation pending.
Base master: `35a6f45dff7b415ece9b23d3a242ffbeb20514e8`.

The existing Schlick diffuse lobe subtracts constant maximum RGB F0 while rough specular Fresnel increases toward grazing. Conductor and thin-film modes add diffuse without interface attenuation. The same additive composition must be corrected in BRDF evaluation and selected diffuse sample throughput.

Use a reciprocal single-pass interface model: `f_diff(i,o) = c/pi * (1-A(i)) * (1-A(o))`. Here A is Schlick interface Fresnel with authored F0, or conductor/film interface Fresnel times the authored specular tint. RGB evaluates each component independently; NM evaluates at the requested wavelength, preserving ambient IOR and film dispersion. The two factors describe transmission into and out of the Lambertian substrate. Higher diffuse round trips are absorbed rather than recycled. This model permits energy loss.

A one-direction factor is rejected because it breaks reciprocity. Normalizing the macro-interface product by `1-Aavg` is also rejected: it restores diffuse energy using an interface average that is not the directional reflectance of rough GGX. Exact energy-preserving product compensation would require the integrated single- and multiple-scattering rough specular lobe, including anisotropy and spectral film parameters; the current isotropic F=1 LUT does not supply that quantity.

The chosen diffuse integral is exactly `c*(1-A(o))*(1-Aavg)` and its cosine-weighted mean is `c*(1-Aavg)^2`. Albedo guides will combine those terms with the existing approximate macro-interface specular estimate, explicitly documenting that approximation. Sampling retains its lobe probabilities and densities; only selected diffuse throughput receives the same two-direction factor as evaluation.

This is a rough-interface approximation, not a theorem that every possible thin-film stack and anisotropic specular configuration conserves energy. Validation must measure the full unchanged specular-plus-new-diffuse model. Test isotropic and anisotropic low-F0 white furnaces, bare and mixed conductor, thin-film mixtures, RGB/NM agreement, reciprocity, sample/evaluation consistency, and a broader roughness/F0/angle sweep. Specular-only controls distinguish defects in the existing specular model from diffuse composition failures. Do not loosen an energy bound to hide a discovered failure.

Commit the tests before running them against the unfixed library. Save their failing output. Then implement, run each suite referencing changed classes, clean Make and Xcode builds, capture serial before/after linear renders, and obtain fresh independent transport, test, and documentation reviews before closure.

Independent audit residual: DL-62 records the reachable glossy-filter roughness disagreement between GGX sample/density and evaluation. It is a different bug pattern and is not repaired in this slice.

Independent observed residual DL-63: the broader committed test reports the same three specular-only F0=1 failures on the unfixed and repaired libraries, with diffuse exactly zero. The LUT generator integrates the separable VNDF weight G1(wo), whereas GGXBRDF evaluates height-correlated G2. Correcting that specular compensation model is a separate slice. The current test retains a nonzero exit for those failures.

Independent static residual DL-64: Schlick F0=0 still has nonzero oblique reflection, but GGXSPF selects specular and MS lobes using weights proportional to F0. Selected-lobe throughput therefore omits those lobes. The new row requires its own committed sampling red proof; it is not fixed by diffuse attenuation.
