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
  sha256 8509f3feb6db80dc4b81b98e2504e071415b47c4f4060b1c1592a2bda946f343
methane_preview.exr.provenance.cbor
  sha256 fdc545b02b647cdfa653ad246729bfd0cb5ac88ba409156f5306c06405cd102e

The animation's eight identity-bearing scene-linear FP32 EXR primaries are
methane_preview_frame0000.exr through methane_preview_frame0007.exr. Every
primary has its own canonical `.provenance.cbor` sidecar. Their artifact
SHA-256 values, in frame order, are:

  1d9f40afc02e62bad3b202f48d640724bdb0d917fbf7527c8709376ae115ab5c
  966a47df8a6f4d026491c9be04a42f0132097b6fb8e0de6815e61d7fd292746d
  2356910c0392e7a5b170b1154a287cc6a7891835682ce8eb701d9052fe6a43e8
  29f90101393c86dcd3be7fac616541aa5077ff3263105740bf214d2fd4fff3fc
  074ea1916a1b6144102350af4eacd5f11cf37672053bd053be6f1230fdc3a335
  8e64d609dbe406950844733a33352e3ac5b12224ffb259e6d290955c581064ea
  77abc9e268375c350b9d9760df2e799d9e78ce81cd38d2fe3d737ff4062ef21e
  8509f3feb6db80dc4b81b98e2504e071415b47c4f4060b1c1592a2bda946f343

Display derivatives
-------------------
methane_preview_display.png
  sha256 1abab746d3095f47c485194f7b2db51efda1bbd4c52b9f3a47434138b9d6c7ca
methane_preview_display.png.provenance.cbor
  sha256 8bd73ee7c7926e568e2929ac05f0f223b3436886a7bc878eb5bdc4f4e4a360f6
methane_preview_animation.gif
  sha256 f9c7afe046afc920d2d6c75850809870561b273777fcd372510bffee6cb00291
methane_preview_animation.gif.provenance.cbor
  sha256 b012ab3e46b7eee9d12fabd03e75ca29abb22ee2b2d586a97382965fca3a05ae

The PNG and looping 8-frame/8-fps GIF are explicitly
`display_derivative`, linked to the preview primaries. They use a
renderer-owned +6 EV ACES-to-sRGB display transform. The preview-only spectral
volume spatializes the recorded reaction and thermal-excess maps while keeping
their exact checkpoint maxima; it does not change the solver state, source-map
evidence, or golden checkpoint. The transform changes display visibility only;
it never feeds the simulation or primary radiance bytes. The GIF route is
ImageIO GIF87a/LZW because
this headless macOS process reported `AVErrorCannotEncode` for its optional
AVFoundation movie derivative. The primaries, not the GIF codec, carry the
validation evidence.
