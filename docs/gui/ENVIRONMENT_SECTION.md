# GUI Environment / HDRI Section — design & implementation

Task 17 of the UI redesign: a first-class **Environment** section that lets a
user see and edit the scene's image-based-lighting (IBL) environment — swap the
HDRI, dim/brighten it, rotate it, and toggle its camera-background visibility —
without hand-editing the `.RISEscene` text.

## Why the environment isn't just "another Light"

The IBL environment is **not** an `ILight` and is invisible to
`Category::Light`. It is a *scene-level singleton* assembled from two chunks:

1. **The image** — an `hdr_painter` / `exr_painter` Painter chunk with a `file`
   parameter (the HDRI path). Already editable + persistable via the existing
   `Category::Painter` CST-param path.
2. **The binding** — four params on the **active rasterizer** chunk that install
   the map as the scene's global radiance map
   (`IScenePriv::SetGlobalRadianceMap`):
   - `radiance_map <painterName>` — which painter supplies the radiance
   - `radiance_scale <double>` — intensity multiplier (default 1.0)
   - `radiance_orient <x> <y> <z>` — Euler rotation in **degrees** in the scene
     file; stored **radians** in `RadianceMapConfig` (`* DEG_TO_RAD` at parse)
   - `radiance_background <bool>` — whether the map is visible behind geometry
     (default true)

There is also a procedural sky (`hosek_wilkie_skylight`) and `ambient_light`;
both also feed `SetGlobalRadianceMap`. v1 of this section targets the **HDRI /
radiance-map** case (the explicit ask) and *detects* a procedural sky as a
read-only "procedural sky" state.

## Persistence: mirror to the CST, like Film

Save serializes the retained CST Document (`SaveEngine::Save` →
`SerializeCst(*doc)`). A live-Job-only edit (`SetRasterizerParameter` →
`RebuildRasterizer`) does **not** persist. Film solves this with
`Job::ApplyCstFilmEdit`, which resolves the singleton `film` chunk by kind and
`DocSetOrAddParamValue`s the changed param into the Document. The environment
binding lives on the (singleton, active) rasterizer chunk, so it follows the
same pattern via **`Job::ApplyCstEnvironmentEdit`**.

Every environment edit is therefore **two coordinated mutations**:
- **live** — `SetRasterizerParameter(activeName, "radiance_*", value)` rebuilds
  the active rasterizer so the viewport re-renders with the new env (the
  `radiance_*` cases were added to `FormatRasterizerParam`/`ApplyRasterizerParam`);
- **persist** — `ApplyCstEnvironmentEdit("radiance_*", value)` mirrors it into
  the CST Document so a save / re-derive keeps it.

The HDRI **file** swap is different: it edits the *bound painter's* `file`
param, which already round-trips through the `Category::Painter` CST path (a full
D2 re-derive that reloads the texture).

## Controller surface (`SceneEditController`)

```
struct EnvironmentInfo {
    bool   hasEnvironment;   // radiance_map bound (name != "none")
    bool   proceduralSky;    // a hosek/sky map is installed instead of an HDRI
    String painterName;      // bound painter name ("" if none)
    String file;             // resolved HDRI path ("" if unresolved)
    double scale;            // intensity
    double orientDeg[3];     // degrees (converted from stored radians)
    bool   background;       // camera-background visible
    bool   editable;         // false if the active rasterizer takes no radiance map (MLT) or none exists
};

bool GetEnvironment( EnvironmentInfo& out ) const;
bool SetEnvironmentScale( double scale );
bool SetEnvironmentBackground( bool bg );
bool SetEnvironmentOrient( double xDeg, double yDeg, double zDeg );
bool SetEnvironmentFile( const String& absPath );          // swap bound painter's file
AgentCommitResult AddEnvironment( const String& hdriPath ); // insert painter + bind radiance_map
bool RemoveEnvironment();                                   // unbind (radiance_map -> none)
```

Reads come from the **live active-rasterizer snapshot** (`RasterizerParams
.radianceMap`), which stays in sync because every edit mutates live too; the
`file` is resolved by looking up the bound painter chunk in the CST Document and
reading its `file` param.

## v1 scope / known limitations (each an honest refusal, not a silent no-op)

- **Present rasterizer chunk required for persistence.** If the scene has no
  explicit rasterizer chunk (renders on the built-in default), the live edit
  stands but `ApplyCstEnvironmentEdit` warns and the change won't survive a save.
  (Nearly every real scene authors a rasterizer chunk.) A future slice can
  synthesize the chunk the way `ApplyCstFilmEdit` synthesizes an absent `film`.
- **MLT rasterizers take no radiance map** — env edits are refused (`editable
  = false`) on `mlt_rasterizer` / `mlt_spectral_rasterizer`.
- **File-swap keeps the painter kind.** Swapping a `.hdr` for an `.exr` (or vice
  versa) within an existing painter may fail to load; `AddEnvironment` picks the
  painter kind from the file extension, so removing and re-adding is the clean
  path across formats.
- **A bad HDRI path registers a black texture** rather than being rejected — the
  GUI file picker is the guard (only offer existing files).

## UI (both platforms)

An always-present **Environment** section in the right-panel inspector:
- no env → a "No environment" state with an **Add HDRI…** button (file picker);
- env present → the HDRI filename (with a **Swap…** picker), an intensity field,
  three rotation fields (X/Y/Z degrees), a **Show in background** toggle, and a
  **Remove** button;
- procedural sky present → a read-only "Procedural sky (hosek_wilkie)" note.
