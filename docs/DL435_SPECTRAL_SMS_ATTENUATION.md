# DL-435: specular-chain attenuation

## Contract

`GetSpecularInfo` supplies RGB attenuation; `GetSpecularInfoNM` supplies
an explicit `attenuationNM` scalar evaluated at its requested wavelength.
Neither consumer selects an RGB channel as a spectral value. These are
material multipliers, separate from Fresnel and the radiance eta factor.
All metadata defaults to a neutral multiplier of one.

`attenuationIsInteriorTransmittance` distinguishes two existing material
contracts. Mirrors supply a multiplier per reflected boundary. Perfect refractors
supply a multiplier per transmitted boundary: their authored refractance
does not color Fresnel reflection (`attenuationAppliesToReflection=false`).
RGB-only extensions default to a multiplier on both event types. `DielectricSPF` supplies its authored scalar `tau`: transmittance
per world-space unit. Following that SPF's current convention, SMS pays
`pow(tau, distance_from_previous_chain_vertex)` on an exiting transmission,
not on entry or a reflected event. RGB now follows that distance contract
rather than multiplying tau once at every interface. This reproduces the
SPF convention; it does not claim that its omission of attenuation on an
interior Fresnel reflection is an exact general absorbing-volume model.

NM reflection is `F` for a native perfect refractor, `attenuationNM * F`
for a generic boundary multiplier, and `attenuationNM` for a pure mirror.
NM transmission is `attenuationNM * (1-F) * (etaI/etaT)^2`.
Interior tau replaces the first factor with the distance law above.
RGB uses the corresponding channel multipliers and the same event policy.

DL-436 extends the interface law: native AR-coated `DielectricSPF` advertises
`hasCustomSpecularFresnel`. The optional tail-appended
`ISPF::EvaluateSpecularFresnel` receives solved incidence, ordered incident
and transmitted indices, entry/exit orientation, and wavelength. It reuses
the SPF's AR stack, including reversed layer order on exit and its Snell/TIR
classifier. RGB evaluates the same representative wavelengths as the SPF
(611, 549, 465 nm), using the chain's existing scalar interface indices.
It does not construct separate RGB-channel refraction geometries (DL-438).
The RGB Airy controls deliberately use a constant IOR. Uncoated vertices use bare Fresnel without an optional
virtual call. Coated transmission uses `1-F`, matching the native SPF;
this is not a new general transport model for absorbing films.

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
reconstruction copy the scalar, distance law, reflection applicability and custom-Fresnel flags into each
`ManifoldVertex`. Photon storage still records material/context rather
than a baked wavelength: reconstruction re-queries at each requested
wavelength. RGB metadata and NM metadata have separate slots, so NM throughput never reads an RGB channel. The unrequested slot may
remain neutral; a spectral replay uses its scalar slot exclusively. Newton copies retain
the cached metadata with the vertex.

## Cost and compatibility

There is no new allocation or cache. Each native metadata query adds the
required wavelength painter evaluation to a query already needed for IOR;
the throughput loop reads cached scalars. Coated vertices additionally evaluate
the fixed-capacity native coating stack once per NM vertex or three times
per RGB vertex, without painter re-query or heap allocation. Neutral dielectric segments skip
`pow`; absorbing exits evaluate it once per NM vertex (or per RGB channel).
The fallback uses a LUT only for a non-neutral RGB extension. Storage-size
and focused runtime measurements are recorded in batch validation;
no renderer-wide speed claim is made.

No exported C construction function or IJob virtual order changes.
`SpecularInfo` and `ManifoldVertex` gain fields: C++ clients sharing these
structures, or implementing the SPF virtual interface, must rebuild with
the library. The optional SPF method is appended to its virtual interface. No new Library source
file is added, so the five project file lists need no new entry.

## Limits retained explicitly

Spatial metadata uses the captured UV and child-frame object position.
Generic material refresh at a moved final Newton root remains DL-398;
this cache does not claim to resolve that separate design. Nested
wavelength-dependent exterior IOR propagation remains DL-391, and
photon eta-stack reconstruction remains DL-331. Participating-medium
extinction remains DL-419. The reflection-only render pilot exposed missing
entry-facing refractor reflection seeds (DL-437); solving attenuation does
not establish complete root coverage. Dielectric tau is evaluated by the same exit
segment convention as its SPF, not by a new nested-medium path integral.

## Verification

The deterministic gate pins a quarter-grey mirror, generic tinted Fresnel reflection, native
transmission-only refractance, coated native-SPF and independent Airy controls, entry and exit radiance factors, distance-dependent RGB/NM tau,
entry/reflection exclusions and wavelength/UV/Po-aware metadata queries.
An actual-material RGB-only fallback control covers extension behavior.
The render gate compares quarter-transmitting and white perfect-refractor
sheets at the same wavelength, geometry and per-pair salt, including both
windings of a double-sided `indexedmesh_geometry`, NM/HWSS and
Snell/uniform. Every band is checked against at least four salted renders
and three standard deviations of its mean. The reflection-only control
uses reachable reversed-winding roots; dark entry-winding pilot rows are
explicitly recorded as DL-437, not accepted attenuation comparisons. Committed master-source red
proofs and final results belong in `DL_CHEAPBATCH_VALIDATION.md`.
