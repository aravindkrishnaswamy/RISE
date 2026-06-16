#!/usr/bin/env python3
"""Encode RISE Rec.709-linear HDR EXR frames -> Rec.2020 + PQ (ST.2084) yuv444p10le
raw on stdout, matching the GUI MovieRasterizerOutput.mm pipeline:
  Rec.709 scene-linear -> Rec.2020 (D65 RGB matrix) -> ST.2084 PQ (linear 1.0 = 100 nits,
  10000-nit peak) -> Rec.2020 non-constant-luminance full-range 10-bit YUV.
Pipe to:  ffmpeg -f rawvideo -pix_fmt yuv444p10le -s WxH -r 30 -i - \
            -vf "setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc:range=tv" \
            -c:v prores_ks -profile:v 4444 -movflags +write_colr out.mov
          *** The setparams filter + `-movflags +write_colr` are REQUIRED. *** They write the
          container 'colr' atom (verify: ffprobe -v trace out.mov | grep nclc  ->  must show
          "nclc: pri 9 trc 16 matrix 9").  QuickTime reads THAT atom to engage HDR.  The
          `-color_primaries/-color_trc` output flags and the `prores_metadata` bsf only set the
          ProRes bitstream/codec tags (visible to `ffprobe -show_entries stream=color_*`) but do
          NOT write the colr atom -> the file is HDR in pixels yet plays back as SDR.
Usage:  encode_pq_prores.py frame0000.exr frame0001.exr ...   (sorted order = frame order)
"""
import sys, numpy as np, OpenEXR, Imath

# Rec.709(D65) -> Rec.2020(D65) linear RGB
M = np.array([[0.6274039, 0.3292830, 0.0433136],
              [0.0690973, 0.9195404, 0.0113623],
              [0.0163914, 0.0880133, 0.8955857]])
m1, m2 = 0.1593017578125, 78.84375
c1, c2, c3 = 0.8359375, 18.8515625, 18.6875
Kr, Kb = 0.2627, 0.0593            # Rec.2020 luma coefficients
Kg = 1.0 - Kr - Kb

def load(p):
    f = OpenEXR.InputFile(p); dw = f.header()['dataWindow']
    w = dw.max.x - dw.min.x + 1; h = dw.max.y - dw.min.y + 1
    pt = Imath.PixelType(Imath.PixelType.FLOAT)
    rgb = np.stack([np.frombuffer(f.channel(c, pt), np.float32).reshape(h, w) for c in "RGB"], -1)
    return rgb.astype(np.float64), w, h

out = sys.stdout.buffer
for p in sys.argv[1:]:
    rgb, w, h = load(p)
    v = np.maximum(rgb, 0.0) @ M.T                       # 709 -> 2020 (linear)
    Lp = np.clip(v * (100.0 / 10000.0), 0, 1)            # linear 1.0 = 100 nits, 10000-nit peak
    Lm = Lp ** m1
    E = np.clip(((c1 + c2 * Lm) / (1.0 + c3 * Lm)) ** m2, 0, 1)   # PQ OETF, R'G'B' in [0,1]
    R, G, B = E[..., 0], E[..., 1], E[..., 2]
    Y = Kr * R + Kg * G + Kb * B
    Pb = (B - Y) / (2 * (1 - Kb))
    Pr = (R - Y) / (2 * (1 - Kr))
    # full-range 10-bit (color_range pc): Y in [0,1023], chroma centred 512
    Y10 = np.clip(np.round(Y * 1023), 0, 1023).astype('<u2')
    U10 = np.clip(np.round(Pb * 1023) + 512, 0, 1023).astype('<u2')
    V10 = np.clip(np.round(Pr * 1023) + 512, 0, 1023).astype('<u2')
    out.write(Y10.tobytes()); out.write(U10.tobytes()); out.write(V10.tobytes())
    sys.stderr.write(f"  {p}: {w}x{h}  PQmax={E.max():.3f}\n")
