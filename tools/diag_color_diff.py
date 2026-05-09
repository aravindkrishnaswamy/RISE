#!/usr/bin/env python
"""Per-region chromaticity / channel diff between RGB-renderer and spectral-renderer outputs.
Usage: python diag_color_diff.py rgb.png spectral.png [out_diff.png]

Crops to non-background pixels (alpha or low-saturation white reject), reports per-channel
mean / median / shift, and writes a 4x amplified diff image.
"""
import sys
import numpy as np
from PIL import Image

def load_rgb(path):
    img = Image.open(path).convert("RGBA")
    arr = np.asarray(img, dtype=np.float32) / 255.0
    return arr[..., :3], arr[..., 3]

def main():
    if len(sys.argv) < 3:
        print("usage: diag_color_diff.py rgb.png spectral.png [diff.png]")
        sys.exit(1)
    rgb_path = sys.argv[1]
    spec_path = sys.argv[2]
    out_path = sys.argv[3] if len(sys.argv) > 3 else None

    rgb, a_rgb = load_rgb(rgb_path)
    spec, a_spec = load_rgb(spec_path)

    if rgb.shape != spec.shape:
        print(f"shape mismatch: rgb={rgb.shape} spec={spec.shape}")
        sys.exit(1)

    # Non-background mask: any channel < 0.97 in either image (rejects pure white BG)
    # AND not pure black (rejects fully transparent that came in as 0)
    mask_fg_rgb = ~((rgb > 0.97).all(axis=-1)) & (rgb.sum(axis=-1) > 0.01)
    mask_fg_spec = ~((spec > 0.97).all(axis=-1)) & (spec.sum(axis=-1) > 0.01)
    mask = mask_fg_rgb & mask_fg_spec
    n = mask.sum()
    print(f"foreground pixels (both): {n}  ({100.0*n/mask.size:.1f}% of image)")
    if n < 100:
        print("too few pixels overlap — quitting")
        sys.exit(1)

    rgb_fg = rgb[mask]
    spec_fg = spec[mask]

    print()
    print("== Mean per channel (RGB renderer)         ==")
    print(f"  R={rgb_fg[:,0].mean():.4f}  G={rgb_fg[:,1].mean():.4f}  B={rgb_fg[:,2].mean():.4f}  L={rgb_fg.mean():.4f}")
    print("== Mean per channel (spectral renderer)    ==")
    print(f"  R={spec_fg[:,0].mean():.4f}  G={spec_fg[:,1].mean():.4f}  B={spec_fg[:,2].mean():.4f}  L={spec_fg.mean():.4f}")
    print("== Spectral - RGB (signed shift)           ==")
    diff = spec_fg - rgb_fg
    print(f"  dR={diff[:,0].mean():+.4f}  dG={diff[:,1].mean():+.4f}  dB={diff[:,2].mean():+.4f}")
    print("== Per-channel ratio (spec / rgb)          ==")
    safe = np.where(rgb_fg > 1e-3, rgb_fg, 1.0)
    ratio = spec_fg / safe
    print(f"  rR={ratio[:,0].mean():.3f}  rG={ratio[:,1].mean():.3f}  rB={ratio[:,2].mean():.3f}")

    # Chromaticity drift: r/(r+g+b), g/(r+g+b)
    rgb_lum = rgb_fg.sum(axis=-1, keepdims=True) + 1e-6
    spec_lum = spec_fg.sum(axis=-1, keepdims=True) + 1e-6
    rgb_chroma = rgb_fg / rgb_lum
    spec_chroma = spec_fg / spec_lum
    print("== Chromaticity drift (d r-chroma, g-chroma) ==")
    dchroma = spec_chroma - rgb_chroma
    print(f"  d_r_chroma={dchroma[:,0].mean():+.4f}  d_g_chroma={dchroma[:,1].mean():+.4f}  d_b_chroma={dchroma[:,2].mean():+.4f}")

    # RMSE
    rmse = np.sqrt(((spec - rgb)**2).mean())
    print(f"== RMSE (whole image): {rmse:.4f}")

    # Diff image (4x amp, centered at 0.5 = neutral)
    if out_path:
        d_img = ((spec - rgb) * 4.0 * 0.5 + 0.5).clip(0, 1)
        d_uint8 = (d_img * 255).astype(np.uint8)
        Image.fromarray(d_uint8).save(out_path)
        print(f"wrote 4x-amplified diff to {out_path}")

if __name__ == "__main__":
    main()
