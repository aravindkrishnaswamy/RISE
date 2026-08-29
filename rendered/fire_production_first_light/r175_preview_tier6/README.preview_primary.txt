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
  sha256 bf02ffa4d99d435ee4c9dc01829233f763a3ee8a32c3653ea27d9e926e7182c9
methane_preview.exr.provenance.cbor
  sha256 5e6187b90c718300a6000100d93c100b55c00814372a112e6d7d327b6e46b44e

The animation's eight identity-bearing scene-linear FP32 EXR primaries are
methane_preview_frame0000.exr through methane_preview_frame0007.exr. Every
primary has its own canonical `.provenance.cbor` sidecar. Their artifact
SHA-256 values, in frame order, are:

  90f6a7156ebe12be1e1b5649cb4fd23f195b0c38629700344579a2fdd575c5db
  fc2840d84b0f7c2b3412411d2bc288a01238b91b3217d4778395b4d5019b895e
  f5e127fe3029f5143daead6caca2c4134c73cd3f955baff60ae839d9764a45e8
  413210ec46fe277f55438a6b41608fff7a31dd7989178e06da13465e5a4d9e03
  6de424e9ae5846aac8bf23b7ff72f769945d5becf3fdc279eca734c7ffe3d2b0
  cf6a3ae9f55afb280a38594bd6850ff46af99c595bdc7ec683f110a565fa0ec1
  461a420b874976ab013ad554de817b8e435a5834d2fdb2d38d82a876677998b8
  bf02ffa4d99d435ee4c9dc01829233f763a3ee8a32c3653ea27d9e926e7182c9

Display derivatives
-------------------
methane_preview_display.png
  sha256 53b0cb296e75583fca6d196ca0f701f7032ce7626c0396b2353eedc4f295f3b3
methane_preview_display.png.provenance.cbor
  sha256 53f578f365ed960f7497278f167c860df111bf48a5b0608fd3436039a8e4f5f9
methane_preview_animation.gif
  sha256 994a5b1a2910f7da4d5b027469cab55b6fbc2226a9521cdd0312cb951eb277f9
methane_preview_animation.gif.provenance.cbor
  sha256 a7571ff50151de2da0a857bc76ee7eb1760f4d9d4a1e2ef2c0ad9ac45929ce1a

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

The eight animation primaries render the same terminal monitored state through
a sealed 20-degree camera orbit with a modest dolly. Every frame must contain a
bounded blue plume, every adjacent decoded frame must differ, and the lit-area
range must exceed five percent.  The lit-mask centroid must also move by at
least two display pixels, excluding static geometry with palette or edge
flicker. This is intentionally a preview camera animation, not a claim of eight
additional solver timesteps.
