r175 tier-6 first light
======================

Status: preview_primary (not a production-fidelity render)
Source revision: af93d02d6b55d74f9f63a5ac2a36f81b6e0cb6d1
Simulation endpoint: 2.3170101413580206 s, 523 accepted steps
Maximum physical temperature observed: 2284.8533 K
Terminal maximum temperature: 1784.60397 K
Terminal maximum heat release: 12771893.4 W/m3
Maximum accepted EOS drift: 3.5717839734772383e-6

Primary files
-------------
methane_preview.exr
  sha256 4d0f8ef02ca6ae6328af1775bd32f44f5b950fa285a0a3d6709063d425aa96f2
methane_preview.exr.provenance.cbor
  sha256 e845fe1fa2aea00e4ca959a3ccb69c975112ced78966ca51a163e2c637a1f19b
methane_preview_display.png
  sha256 13d27873d68def0eb8ce03191c570e6954e1f490e43fd312d4efcff6ffbd6b4f
methane_preview_display.png.provenance.cbor
  sha256 58aafbe95e61acc6551e12e8671e0fff6135770307a23adeaf89d21557eab0d6

The EXR is the primary linear-radiance artifact.  The 32x32 PNG is the
renderer-authored ACES/exposure derivative.  Methane has zero soot yield in
this fixture, so the visible preview is intentionally faint and blue; the
physical burning-state validation is carried by the source-map and capstone
records rather than by preview brightness.
