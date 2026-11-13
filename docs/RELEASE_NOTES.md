# Release notes

## C++ utility modernization, unreleased

Regenerate photon maps, irradiance caches, serialized meshes, and other
binary assets written through RISE’s scalar serializers by the historical
Xcode `rise` CLI target. That
target defined `RISE_BIG_ENDIAN` on little-endian hosts. Affected writer
branches lost bits and exchanged double halves, so those files can contain
irrecoverable data loss. Regenerate them from the original scenes and
source assets with the corrected build.

- 3DS import accepts older or missing version metadata, skips empty mesh
  objects, and ignores malformed trailing metadata after the mesh section.
  Consumed mesh records retain length and index checks.
- Disk buffers retain 64-bit size, cursor, and seek state. Photon-map saves
  stage their output and preserve the previous file on serialization or
  publication failure. Legacy 32-bit buffer queries saturate at `UINT_MAX`.
  Image output permits direct writes through writable targets when staging
  is unavailable, including dangling symlinks. A failed direct write can
  leave a partial image.
- Spectral and piecewise-function tables accept their leading finite
  columns, skip file headers and malformed rows, and ignore extra columns.
  Inline control points require both values. Unequal spectral sample counts
  remain errors.
- Console commands support trailing `#` comments outside quoted tokens.
  Checked descriptor and command conversions reject malformed and
  out-of-range numbers. Camera keyframes require complete finite values.
  The parser guide records the accepted prefix and sign behavior.
- TIFF decoding rejects damaged strips. The older partial-decoding mode
  could report success while inserting opaque-black rows.
- Media existence checks use the native narrow filename convention shared
  with the existing CRT and stream opens. Native Windows verification of
  non-ASCII paths remains outstanding.
- `mediapath add` in scripts registers the authored path. Cylindrical object
  UV modification reads its size from the correct argument. Voronoi 2D and
  3D painters accept the default `border none`. These corrections can change
  file resolution, UV mapping, and derived scene contents in existing scripts.
- Concrete implementation types changed layout during this series.
  Clients constructing or subclassing them need rebuilding.

See [the modernization plan](CPP_UTILITY_MODERNIZATION_PLAN.md) for the
implementation checkpoint, measurements, and remaining platform gates.
