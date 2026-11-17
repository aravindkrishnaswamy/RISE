# Conductor Optical Constants (`colors/conductors/`)

Measured spectral complex refractive indices (`n` + i·`k`) for elemental metals,
used by `ggx_material` (`fresnel_mode conductor` or `fresnel_mode thinfilm`) via
`scalar_painter { file colors/conductors/<Metal>.{n,k} }` and the Material Editor's
Measured Metal ($n, k$) preset picker (Direction D2,
[`docs/gui/SPECTRAL_DIFFERENTIATORS.md`](../../docs/gui/SPECTRAL_DIFFERENTIATORS.md) §3).

## Provenance & Citations

All tabulated constants are from the open **refractiveindex.info** database
(M. N. Polyanskiy, *Refractiveindex.info database of optical constants*,
*Sci. Data* **11**, 94 (2024), released under **CC0 1.0 Universal** public-domain
dedication). The underlying peer-reviewed references for each dataset are:

| File pair | Metal | Dataset | Scientific Reference | RII Page |
|---|---|---|---|---|
| `Au.n` / `Au.k` | Gold (Au) | Johnson & Christy 1972 | P. B. Johnson, R. W. Christy, *Optical constants of the noble metals*, Phys. Rev. B **6**, 4370–4379 (1972) | `main / Au / Johnson` |
| `Ag.n` / `Ag.k` | Silver (Ag) | Johnson & Christy 1972 | P. B. Johnson, R. W. Christy, *Optical constants of the noble metals*, Phys. Rev. B **6**, 4370–4379 (1972) | `main / Ag / Johnson` |
| `Cu.n` / `Cu.k` | Copper (Cu) | Johnson & Christy 1972 | P. B. Johnson, R. W. Christy, *Optical constants of the noble metals*, Phys. Rev. B **6**, 4370–4379 (1972) | `main / Cu / Johnson` |
| `Al.n` / `Al.k` | Aluminium (Al) | Rakić 1995 | A. D. Rakić, *Algorithm for the determination of intrinsic optical constants of metal films: application to aluminum*, Appl. Opt. **34**, 4755–4767 (1995) | `main / Al / Rakic` |
| `Cr.n` / `Cr.k` | Chromium (Cr) | Johnson & Christy 1974 | P. B. Johnson, R. W. Christy, *Optical constants of transition metals: Ti, V, Cr, Mn, Fe, Co, Ni, and Pd*, Phys. Rev. B **9**, 5056–5070 (1974) | `main / Cr / Johnson` |

Additional transition-metal substrates (`Ti.{n,k}`, `Steel.{n,k}`, `Ta.{n,k}`,
`Nb.{n,k}`) ship under [`colors/thinfilm/substrates/`](../thinfilm/README.md).
