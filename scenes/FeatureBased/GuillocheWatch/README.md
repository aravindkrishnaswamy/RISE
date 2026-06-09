# Guilloché Thin-Film Watch — RISE showcase scene

A physically-based hero render of a guilloché-dial titanium watch (inspired by
the MING × J.N. Shapiro 37.06 "Lightning") showcasing RISE's **thin-film
interference** material. The anodized-titanium dial's gold → magenta → blue
iridescence is *real* thin-film (TiO₂-on-Ti) Fresnel — not a painted texture; an
oxide-thickness map drives the film thickness per-pixel across the woven
guilloché relief.

Everything for this scene lives in this folder: the scene file, the asset
generators, and the generated meshes/textures. Asset `file` paths in the scene
are repo-root-relative (resolved via `RISE_MEDIA_PATH`), e.g.
`scenes/FeatureBased/GuillocheWatch/dial.raw2`.

## Render

```sh
export RISE_MEDIA_PATH="$(pwd)/"          # repo root
printf "render\nquit\n" | ./bin/rise scenes/FeatureBased/GuillocheWatch/watch_dial.RISEscene
# -> rendered/watch_dial.png (+ _denoised)
```

The scene defines **7 cameras**; `cam_photo` (the hero) is active by default.
Render every angle, or a subset:

```sh
python3 scenes/FeatureBased/GuillocheWatch/render_watch_views.py                 # all 7
python3 scenes/FeatureBased/GuillocheWatch/render_watch_views.py --cam cam_macro cam_profile
# one PNG per angle -> rendered/watch_<name>.png
python3 scenes/FeatureBased/GuillocheWatch/render_watch_turntable.py             # +Z-orbit GIF
```

| Camera | Angle | Purpose |
|---|---|---|
| `cam_photo` | 3/4 (ψ=315°) | **HERO** — 100 mm macro f/16; 12 o'clock to NE, crown right |
| `cam_macro` | low, crown-side | 100 mm f/8 punch-in, shallow DOF (bokeh strap) |
| `cam_topdown` | straight down | flat-lay, whole dial + strap |
| `cam_high34` | high 3/4 | rakes the guilloché relief from above |
| `cam_graze` | low ~12° | dramatic grazing rake |
| `cam_profile` | side | case slab, domed crystal, crown, lugs, strap |
| `cam_low34` | low 3/4 | sharp (no-DOF) alt beauty |

## Asset pipeline (regenerate)

The scene consumes three generated assets. The generators write next to the
scene (this folder) by default:

```sh
python3 scenes/FeatureBased/GuillocheWatch/dial_mesh_gen.py --cell 1.35 --disp 0.46
#   -> dial.raw2        (Cartesian guilloché dial mesh)
#   -> oxide_cart.png   (normalized radial heat-tint SHAPE map; torch start/end
#                        nm are set IN THE SCENE via the oxide_thk scalar_painter)
python3 scenes/FeatureBased/GuillocheWatch/strap_mesh_gen.py
#   -> strap.raw2       (curved leather strap mesh)
python3 scenes/FeatureBased/GuillocheWatch/thermal_oxide_sim.py
#   -> oxide_thickness.png + oxide_calibration.txt (torch heat -> oxide nm sim)
python3 scenes/FeatureBased/GuillocheWatch/guilloche_gen.py
#   -> guilloche_{height,normal,angle}.png  (earlier POLAR rose-engine generator;
#      superseded for the production dial by dial_mesh_gen.py, kept for reference)
```

**`dial.raw2` (~28 MB) is gitignored** — regenerate it with `dial_mesh_gen.py`
after a fresh clone. `strap.raw2` (small) is tracked, so the watch renders as
soon as `dial.raw2` is regenerated.

## Heat-tint tuning

The iridescence palette is dialable from the scene without re-baking: the
`oxide_thk` `scalar_painter` `bias`/`scale` set the torch START / SPAN thickness
in nm (centre = bias, rim = bias + scale). Presets are in comments at the top of
`watch_dial.RISEscene`. The *radial falloff* (how fast it heats outward) is baked
into `oxide_cart.png` — re-bake with `dial_mesh_gen.py --oxide-falloff
{quadratic|smooth|linear}` to change it.

See [docs/THIN_FILM_INTERFERENCE.md](../../../docs/THIN_FILM_INTERFERENCE.md) for
the full feature writeup (TMM/Airy reference, n,k data, GGX `fresnel_mode
thinfilm`, the spectral path).
