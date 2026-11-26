# Physical spectra for delta lights

`omni_light`, `spot_light` and `directional_light` accept immutable tabulated physical spectra. This first P6a increment implements source authoring, evaluation, cloning and editor readback. Band-aware source proposals and raw radiometric output remain unfinished; this is not a claim of calibrated UV/IR image rendering.

```text
omni_light
{
    name swir_point
    position 0 0 2
    power 3
    spectral_outside zero
    spectral_samples 1100 2
    spectral_samples 1200 4
    spectral_samples 1300 6
}
```

Each repeated pair is wavelength in nm followed by spectral radiant intensity in W/(sr nm) for point and spot lights, or spectral irradiance in W/(m² nm) for a directional light. Spot values describe the on-axis intensity before the existing cone attenuation. Scene distances must be metres for the point/spot inverse-square result to have those physical units; otherwise convert intensity for the scene's distance unit explicitly. `power` is a dimensionless nonnegative multiplier in this mode. It multiplies the samples directly, without spectral or photometric normalization. Directions and spot full cone angles retain existing conventions. Physical spot full cone angles require 0 ≤ inner ≤ outer ≤ 180°, with outer > 0; source position and target must define a nonzero finite axis.

There must be at least two finite, positive, strictly increasing wavelengths and finite nonnegative values. Evaluation is piecewise linear. `spectral_outside zero` is required explicitly and defines zero below/above the closed sample interval. It is an authored model policy; it does not certify measured coverage or justify truncating an unknown spectral tail. Duplicate wavelengths, endpoint holding and implicit tail policies are rejected.

Physical samples exclude `color` and `colorspace`. Existing RGB lights keep their authoring and D65 uplift unchanged. A physical light's RGB preview is the existing linear visible source projection of its spectrum; it retains amplitude and can be black for a pure UV/SWIR source. This preview is not used to reconstruct physical NM emission.

The light panel displays the samples and outside policy read-only and omits color controls. Change samples in the scene text. Scalar `power` / `energy` keyframes can animate the dimensionless scale; invalid negative, nonfinite or overflowing scales are rejected. Point/spot sources additionally require a finite conservative 4π bound on the scaled visible RGB power proxy, including for narrow spots. The source is multiplied by scale before this bound, so a faint spectrum can still use a large finite multiplier. Physical light color keyframes are rejected. Invalid target/cone keyframes are rejected against the current physical spot state; invalid interpolated target/cone/scale values retain the previous accepted value. Invalid physical spot target/cone values also emit a diagnostic. Retained-CST pose edits reject invalid construction atomically. Generic transform setters and position animation use a void interface and cannot report atomic refusal: finalizing an undefined physical spot axis diagnoses the pose and disables its emission, direction PDF and photon-source power until a valid pose is finalized. The authored `shootphotons` flag is retained, including in snapshots. This is an unsupported animated pose, not a valid physical zero-light experiment. The current cone sampler must have a representable nonzero finite direction PDF; numerically unresolved cones are rejected at construction. Snapshots and time views retain the immutable source while keeping their own scalar scale.

C-style construction uses `RISE_API_CreatePhysicalPointOmniLight`, `RISE_API_CreatePhysicalPointSpotLight` and `RISE_API_CreatePhysicalDirectionalLight`; the spot factory takes an explicit position and world-space target and returns a finalized light. All copy their input arrays, take the explicit outside policy string `"zero"`, and clear the output on failure. Matching `IJob::AddPhysical...` wrappers are appended at the interface tail. Existing factory signatures are unchanged.

The current light-selection table, environment distribution and NM light-sample roulette still use RGB energy estimates. A physical source with a black visible projection can be omitted from sampled light emission; no source-support guarantee is made here. The next P6a increment requires a per-pass spectral domain context, support-preserving proposals and matching hit-side PDFs, plus a spectral roulette policy. Direct source evaluation controls in `PhysicalLightSpectrumTest` do not replace those estimator gates. The visible-only analytic sky is also unchanged.
