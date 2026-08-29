r175 tier-6 first light
======================

Status: monitored preview (not a production-fidelity render)
Simulation endpoint: 2.3170101413580206 s, 523 accepted steps
Maximum physical temperature observed: 2284.8533 K
Terminal maximum temperature: 1784.60397 K
Terminal maximum heat release: 12771893.4 W/m3
Maximum accepted EOS drift: 3.5717839734772383e-6

Preview primaries
-----------------
methane_preview.exr
  sha256 b632f77ba488a4e9a04b36752ddfde37336a2f3bd10731644b39af635bbd3ade
methane_preview.exr.provenance.cbor
  sha256 5dd4afa107d1008749d26ada7f96bdbc66133db6a52d15e986f40ab6cb8719a1

The animation's eight identity-bearing scene-linear FP32 EXR primaries are
methane_preview_frame0000.exr through methane_preview_frame0007.exr. Every
primary has its own canonical `.provenance.cbor` sidecar. Their artifact
SHA-256 values, in frame order, are:

  f8d3889cff4d84572ee890fbe86fbd6690c4e4de7ac7efff14742b9a82d54a41
  4a8d51c555ec879fd4cc64961dec2b79cd987f961829c4d367587c1a73700b3f
  c5defdd855cf73820c6393cb079fd75dc902caa1d36183c913d04e4def7dfc89
  6af340839a61dff57bcdc0ca543a555274ffb31cbf57061bf1d106d688ca2b6f
  bd97315de7bdc3c9081bdb95f65b19ba5e4fc06d90ed3a8dd30665bbaecf472b
  d7670e96dc60aa009eb90736ecb047a1bec40f6042f2876f605ba1ca1ecdd40c
  dfa9012944fda9382fac88db047e8a0c961be825137e6b0bdfb5faca147f2f10
  b632f77ba488a4e9a04b36752ddfde37336a2f3bd10731644b39af635bbd3ade

Display derivatives
-------------------
methane_preview_display.png
  sha256 ad351f00fcdde2958308758a1d10571b6acdb603cd9087986f62c4c7e4fce901
methane_preview_display.png.provenance.cbor
  sha256 fb4784af377713826f224949276b75d44f41c16911c16c078f04de5b1bb65ec8
methane_preview_animation.gif
  sha256 5a59652588c89720c2791fb043b3e9a4aef9190b37aa6b97122c37bab735506f
methane_preview_animation.gif.provenance.cbor
  sha256 3dbbd57ad91526f5b00df409acb4f976d21e35bc5f12e9bbd29a5e2c7ba3c7d0

The PNG and looping 8-frame/8-fps GIF are explicitly
`display_derivative`, linked to the preview primaries. They use a
renderer-owned +65 EV ACES-to-sRGB display transform because this tier-6
methane fixture has zero soot yield and extremely small scene-linear visible
radiance. The transform changes display visibility only; it never feeds the
simulation or primary radiance bytes. The GIF route is ImageIO/LZW because
this headless macOS process reported `AVErrorCannotEncode` for its optional
AVFoundation movie derivative. The primaries, not the GIF codec, carry the
validation evidence.
