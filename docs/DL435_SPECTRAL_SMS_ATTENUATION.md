# DL-435: specular-chain attenuation

## Contract

`GetSpecularInfo` supplies RGB attenuation; `GetSpecularInfoNM` supplies
an explicit `attenuationNM` scalar evaluated at its requested wavelength.
Neither consumer selects an RGB channel as a spectral value. These are
material multipliers, separate from Fresnel and the radiance eta factor.
All metadata defaults to a neutral multiplier of one.

`attenuationIsInteriorTransmittance` distinguishes two existing material
contracts. Mirrors and perfect refractors supply a multiplier per boundary
event. `DielectricSPF` supplies its authored scalar `tau`: transmittance
per world-space unit. Following that SPF's current convention, SMS pays
`pow(tau, distance_from_previous_chain_vertex)` on an exiting transmission,
not on entry or a reflected event. RGB now follows that distance contract
rather than multiplying tau once at every interface. This reproduces the
SPF convention; it does not claim that its omission of attenuation on an
interior Fresnel reflection is an exact general absorbing-volume model.

NM reflection is `attenuationNM * F` for a dielectric and
`attenuationNM` for a pure mirror. NM transmission is
`attenuationNM * (1-F) * (etaI/etaT)^2`. Interior tau replaces the first
factor with the distance law above. This matches the RGB SMS convention;
Fresnel, topology, and interface eta selection are not redesigned here.
The existing perfect-refractor versus dielectric SMS-model distinctions
and open SMS root-coverage rows remain separate debt.

## Producers and propagation

Perfect-reflector metadata queries its reflectance through
`ReflectanceColorNM`, including the existing exact-white and negative
reflectance guards. Perfect-refractor metadata queries refractance through
the same accessor. Dielectric metadata queries its scalar tau directly,
clamping a negative transmittance to zero before the distance power.
Polished and smooth SSS metadata retain their neutral surface multiplier:
their substrate/volume color is not a multiplier of the specular surface
reflection. No diffuse albedo is attached to that Fresnel reflection.

The default material/SPF NM query supports RGB-only C++ extensions by
uplifting their nonnegative RGB metadata with the existing unbounded
reflectance-shaped RGB spectrum. Neutral triples use their exact scalar
value without a LUT lookup. An extension with authored spectral data must
override the NM query; RGB metadata cannot recover such a spectrum.
This fallback preserves an RGB multiplier above one; native painter
accessors keep their existing material-specific rules.

Snell wavelength replay, uniform wavelength overrides and photon-chain
reconstruction copy the scalar and its distance-law flag into each
`ManifoldVertex`. Photon storage still records material/context rather
than a baked wavelength: reconstruction re-queries at each requested
wavelength. RGB metadata and NM metadata have separate slots, so NM throughput never reads an RGB channel. The unrequested slot may
remain neutral; a spectral replay uses its scalar slot exclusively. Newton copies retain
the cached metadata with the vertex.

## Cost and compatibility

There is no new allocation or cache. Each native metadata query adds the
required wavelength painter evaluation to a query already needed for IOR;
the throughput loop reads cached scalars. Neutral dielectric segments skip
`pow`; absorbing exits evaluate it once per NM vertex (or per RGB channel).
The fallback uses a LUT only for a non-neutral RGB extension. Storage-size
and focused runtime measurements are recorded in batch validation when
complete; no renderer-wide speed claim is made.

No exported C construction function or IJob virtual order changes.
`SpecularInfo` and `ManifoldVertex` gain fields: C++ clients sharing these
internal structures must rebuild with the library. No new Library source
file is added, so the five project file lists need no new entry.

## Limits retained explicitly

Spatial metadata uses the captured UV and child-frame object position.
Generic material refresh at a moved final Newton root remains DL-398;
this cache does not claim to resolve that separate design. Nested
wavelength-dependent exterior IOR propagation remains DL-391, and
photon eta-stack reconstruction remains DL-331. Participating-medium
extinction remains DL-419. Dielectric tau is evaluated by the same exit
segment convention as its SPF, not by a new nested-medium path integral.

## Verification

The deterministic gate pins a quarter-grey mirror, tinted Fresnel
reflection, entry and exit radiance factors, distance-dependent RGB/NM tau,
entry/reflection exclusions and wavelength/UV/Po-aware metadata queries.
An actual-material RGB-only fallback control covers extension behavior.
The render gate compares quarter-transmitting and white perfect-refractor
sheets at the same wavelength, geometry and per-pair salt, including both
windings of a double-sided `indexedmesh_geometry`, NM/HWSS and
Snell/uniform. Every band is checked against at least four salted renders
and three standard deviations of its mean. Committed master-source red
proofs and final results belong in `DL_CHEAPBATCH_VALIDATION.md`.
