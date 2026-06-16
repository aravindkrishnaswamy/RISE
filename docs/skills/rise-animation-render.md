# RISE animation render → HDR movie

## When to use
- Rendering a RISE scene's animation (a camera/object **timeline**) to a movie file.
- Producing an **HDR** (ProRes 4444 / HDR10) movie of a scene.
- Diagnosing an animation movie that "isn't HDR", plays back washed/dark, or renders the wrong (static) frames.

## Two paths
1. **GUI — simplest for HDR.** The Mac RISE-GUI's `MovieRasterizerOutput.mm` (AVFoundation) renders the animation straight to an HDR10 ProRes `.mov`, writing the container `colr` atom automatically. Windows GUI uses `VideoEncoder.cpp` (FFmpeg). Load the scene, set the video output path, hit Render Animation. **Use this when you're at the GUI** — it Just Works for HDR.
2. **Headless — scripted/automatable.** The CLI renders per-frame EXR stills; encode them to HDR10 ProRes with `tools/encode_pq_prores.py | ffmpeg`. Use for batch/automation/no-GUI. The procedure + the critical `colr`-atom gotcha are below.

## Headless procedure

### 0. Pre-flight
- Scene needs `animation_options { time_start … time_end … num_frames N }` **and** a `timeline` on the **active** camera/object. The active camera is the **LAST-defined** one — verify the *animated* camera is last, or you render N identical static frames. Confirm motion afterwards: frame 0 vs frame N/2 must differ (`|Δ|` well above noise).
- **Denoise stays ON** for any presentation render (standing user preference) — leave `oidn_denoise true`. (Denoise-off is only for variance/A-B diagnostics.)
- **Renders are SEQUENTIAL** — never run two RISE renders at once (each takes all cores; the machine becomes unusable). Background the render and wait for the completion notification.

### 1. Calibrate resolution to a time budget
Render ONE frame at a trial resolution, read `Total Rasterization Time`, scale: per-frame time ∝ pixels (spp fixed), `total = per_frame × N`. Pick the resolution so `total ≈ budget`, and trim slightly under so frame-time variance can't overshoot. (Datapoint: full watch, 304² @ 128 spp spectral, denoise on ≈ 106 s/frame → 48 frames ≈ 85 min.)

### 2. Render the animation (per-frame EXR)
Scene output chunk: `file_rasterizeroutput { pattern rendered/<name> multiple TRUE type EXR color_space Rec709RGB_Linear }` → frame-numbered `rendered/<name>0000.exr …`. With denoise on you also get `<name>_denoised0000.exr` — **encode the `_denoised` ones**.
```sh
export RISE_MEDIA_PATH="$(pwd)/"
./bin/rise scene.RISEscene <<< $'renderanimation 0 1 48\nquit'
```
Resolution is overridable per-render with `--width/--height` (applied after scene load). There is **no `--samples` flag** — edit the rasterizer `samples` in a temp scene copy.

### 3. Encode → HDR10 ProRes 4444 `.mov`
`tools/encode_pq_prores.py` does Rec.709-linear → Rec.2020 + PQ (ST.2084, linear 1.0 = 100 nits) → full-range `yuv444p10le`. **Pass the EXR glob DIRECTLY** — zsh does NOT word-split an unquoted `$VAR`, so a `FRAMES=$(ls …)` var collapses to one giant arg → OpenEXR "file name too long". Let the shell expand the glob into separate args:
```sh
python3 tools/encode_pq_prores.py rendered/<name>_denoised00*.exr \
| ffmpeg -y -f rawvideo -pix_fmt yuv444p10le -s WxH -r 30 -i - \
    -vf "setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc:range=tv" \
    -c:v prores_ks -profile:v 4444 -movflags +write_colr \
    rendered/<name>.mov
```

### 4. Verify — the non-negotiable check
```sh
ffprobe -v trace rendered/<name>.mov 2>&1 | grep nclc    # MUST print:  nclc: pri 9 trc 16 matrix 9
```
If that line is absent or shows `pri 2 trc 2`, HDR will NOT engage — see below.

## THE `colr`-atom gotcha (a "tagged HDR" file that plays as SDR)
QuickTime/macOS engages HDR off the **container `colr` atom** (`nclc: pri 9 trc 16 matrix 9` = Rec.2020 / PQ / Rec.2020) — **NOT** the ProRes bitstream color tags. The trap that wastes a cycle: `-color_primaries/-color_trc` output flags **and** `-bsf:v prores_metadata=…` set the bitstream/codec tags, so `ffprobe -show_entries stream=color_*` *reports* `bt2020`/`smpte2084` — but **neither writes the `colr` atom**. Result: HDR pixels, SDR playback ("I don't see HDR"). Even `-movflags +write_colr` *alone* writes `pri 2 trc 2` (unspecified) because the `-color_*` flags don't reach the ProRes muxer. **Working recipe = `setparams` filter (stamps the frames) + `-movflags +write_colr` (forces the atom).** Diagnose any "not HDR" file with `ffprobe -v trace x.mov | grep -iE "colr|nclc"` and compare against a known-good GUI render (the GUI/AVFoundation path always writes `colr`).

## Other gotchas
- **Homebrew ffmpeg lacks `zscale`** (no libzimg) → do ALL color math (709→2020, PQ OETF, RGB→YUV) in Python (the script), and feed ffmpeg ready-made `yuv444p10le`. Don't reach for `zscale`/`tonemap` filters.
- **Viewing**: HDR is visible only on an HDR display + HDR-aware player (QuickTime on an XDR Mac). SDR displays tone-map it; a non-HDR-aware player shows PQ-as-SDR (wrong). To preview headlessly, tonemap the **linear EXRs** (not the PQ `.mov`) to sRGB.
- **Content brightness**: renders are scene-referred; the script maps linear 1.0 = 100 nits. A dim/moody scene (low median nits) reads as HDR only in its highlights — to make HDR "pop", brighten the render or add a gain before the PQ step.

## Files
- `tools/encode_pq_prores.py` — EXR (Rec.709-linear) → HDR10 (Rec.2020/PQ) `yuv444p10le` raw, piped to `ffmpeg prores_ks`.
- GUI writers: `build/XCode/rise/RISE-GUI/Bridge/MovieRasterizerOutput.{h,mm}` (Mac, AVFoundation), `build/VS2022/RISE-GUI/VideoEncoder.{h,cpp}` (Windows, FFmpeg).
