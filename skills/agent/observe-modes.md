# Observe Modes: Choosing How to Look at the Scene
> hook: Read before deciding HOW to look at the scene -- read_viewport, render{quality:"draft"}, render{mode:"objectmap"|"normals"|"depth"|"facets"|"wireframe"|"deep_reflect"|"direct"|"indirect"|"clay_lights"}/query_object_at, and a production render each answer a DIFFERENT question at a DIFFERENT cost; the wrong pick either lies to you or burns a full render for nothing.

There are exactly FOUR FAMILIES of call for looking at the scene
through the agent surface.  They are not interchangeable, and three of
the four are CHEAP PRECISELY BECAUSE they answer a narrower question
than "what does this look like" -- reaching for the wrong one either
gets you a confidently wrong answer (judging colour from a draft
render) or pays full production cost for a question a free/near-free
call already answers (checking whether an edit landed by re-rendering
production).

Within the third family (`render{mode:}`), GUI render modes P1
(docs/gui/RENDER_MODES.md) widens the mode value from a single choice
(`objectmap`) to SIX: `beauty` (default), `objectmap`, and four
structural DIAGNOSTIC modes -- `normals`, `depth`, `facets`,
`wireframe` -- each answering one narrow structural question with the
SAME exact, single-pass, 1-spp cost profile as `objectmap`.  GUI render
modes P2a adds TWO MORE mode values, `deep_reflect` and `direct`, and P2b
adds TWO MORE STILL, `indirect` and `clay_lights` -- all four are a
DIFFERENT cost tier: real production-class path-traced renders (real
materials, real lights, real OIDN denoise) at a FIXED reduced resolution
and FIXED higher sample count, not single-pass diagnostics. They cost
SECONDS, not milliseconds -- more than a diagnostic mode, still far
cheaper than a full-resolution production render because of the
resolution divisor. See "View modes" below for the per-mode decision
table, and "Transport modes" for all four additions.

## The decision table

| Intent | Call | Cost | Trust it for | Do NOT trust it for |
|---|---|---|---|---|
| "What is the user seeing right now?" | `read_viewport {maxEdge?}` | Free -- copies the GUI's last interactive frame, NEVER renders | The exact live frame, whatever pipeline produced it, PLUS `paneSet` (N-up introspection: layout, primary, per-pane mode/vantage, and `sourcePane` = WHICH pane the PNG holds).  In a multi-pane layout ALWAYS check `sourcePane` first -- the image is the last-RENDERED pane, not necessarily the primary or the pane you care about.  The pane set is read-only by design; you cannot rearrange the user's panes | Anything when `available:false`.  There are SEVEN reasons and they split into three groups (the full table is under "Hard warnings" item 3).  RETRIABLE, clears on its own: `editor_transaction_in_progress`, `render_in_progress`, `editor_interaction_finalize_failed`.  RESOLVES but not by retrying: `no_frame_yet` (viewport exists but hasn't drawn yet).  PERMANENT: `no_controller` (headless session -- there is no viewport and never will be), `editor_shutting_down`, `editor_interaction_unrecoverable` (a latched editor failure that never clears).  Falling back to `render` fixes `no_controller`/`no_frame_yet`; for the OTHER FOUR refusal reasons -- `editor_transaction_in_progress`, `editor_interaction_finalize_failed`, `editor_shutting_down`, `editor_interaction_unrecoverable` -- `render` hits the SAME gate and is refused too.  `render_in_progress` is the one split case, and even there `render` is a POOR fallback: a plain `render {}` BLOCKS up to 30 s on the render slot and only succeeds if the occupant finishes inside that window (the user's own production render, which shares that slot, usually does not), and it is refused with NO wait when a direct parked render holds the gate; a render with a `width`+`height` or `camera`/`view` override is always refused immediately.  Retry the FREE `read_viewport` instead of paying that block |
| "Is this object roughly where I want it?" | `render {quality:"draft", width, height, camera?}` | Cheap -- a wholly separate fixed studio-preview pipeline, samples capped at 4 regardless of what you ask for | Geometry, silhouette, composition, camera framing; relative depth/placement (ESPECIALLY with a second `camera` angle -- one view alone can't tell front-of/behind/inside) | Materials, lighting, exposure, or colour -- the preview shader IGNORES the scene's authored materials and lights entirely |
| "Which object is where? / Find object X on screen." | `render {mode:"objectmap"}` (survey, whole-frame legend) or `query_object_at {x,y}` (one answer) | About one identity render -- fixed 1 spp, no MC noise, ignores `quality`/`samples` entirely | Exact identity: byte-exact `colorHex` <-> `name` legend match, including CSG composites (legend carries the ROOT only, never the hidden operands) and instance arrays (`grid[i,j]`) | The colours as APPEARANCE -- they are arbitrary per-render identity ids, not materials. Read the objectmap PNG at NATIVE size only (omit `maxEdge` -- a box-downscale blends flat ids and breaks the match) |
| "Does it actually look right -- and what geometry/material cues explain it?" | `render {imageMaxEdge:N}` (no `quality`, i.e. production) when appearance alone is enough -- the PNG rides back in that ONE call; add a following `read_image {representation:"perception"}` only when you want the atlas | The real render cost; perception reuses that same render | Beauty is the honest appearance; the atlas adds diffuse albedo, world orientation, and raw primary-hit depth in one bounded image | Do not treat albedo as lit colour, normal RGB as colour, or auto-windowed depth brightness as an absolute cross-frame scale |
| "Verify after an edit." | The cheap loop first (draft and/or objectmap/query at small dims), production LAST once confident | Escalating -- cheap checks first, one production pass at the end | Catching gross breakage (wrong object, black frame, object moved to the wrong side) cheaply, repeatedly | Calling it "done" off a cheap check alone -- ship the final judgment on one production render |

### Production perception: use the richer final observation

Agent transports capture perception by default on a production beauty render.
After that render, prefer:

```json
{"method":"read_image","params":{"representation":"perception","maxEdge":768}}
```

when the task involves occlusion, relative placement, surface orientation,
material-vs-light diagnosis, or edit localization. The stable 2x2 order is
`[beauty, albedo; world_normal, log_depth]`; one image lets a vision model
compare all four panels spatially. Omitting `maxEdge` is safe—the whole atlas
defaults to a 1024-pixel bound—but a smaller explicit value saves image tokens.
Read `guidePrefilter`: `fast` means albedo/normal use the camera first hit;
`accurate` may pass through delta/specular surfaces to the first non-delta
surface. Depth always remains the raw camera ray's first geometric hit, before
alpha/transparency continuation, medium scattering, x-ray traversal, or
refraction. At silhouettes it averages hit samples only; misses stay black.

Use `render {imageMaxEdge:N}` -- one call, image included -- when only
appearance matters or the extra panels
would consume context without answering the prompt. Use `render {mode:"depth"}`
instead when you specifically need the diagnostic mode's default x-ray-through-
glass semantics; perception depth intentionally describes the front geometry.
Set `perception:false` on production render only when saving its 7-byte/pixel
persistent sidecar and transient AOV memory matters more than the added context.

## View modes: which `render{mode:}` value answers which question

| mode | Question it answers | Read it as | `legend`? |
|---|---|---|---|
| `beauty` (default) | What does the scene actually look like -- materials, lighting honored? | The real image (or the draft preview under `quality:"draft"`) | No |
| `objectmap` | Which object is at which pixel? | Flat per-object identity colour, byte-exact `colorHex` match | Yes |
| `normals` | Which way do surfaces face? | World-space shading normal as false colour (`0.5*(N+1)`) | No |
| `depth` | How far away is everything? | Grayscale, near = bright, AUTO-WINDOWED per render to the VISIBLE hit-distance range in that frame (see limitation below) | No |
| `facets` | What does the actual tessellation look like -- where does shading disagree with geometry? | Headlamp-shaded GEOMETRIC normal -- no smoothing, no bump, no shading-normal interpolation | No |
| `wireframe` | Where are the polygon edges? | Triangle-mesh edges over dim facet shading (see mesh-only limitation below) | No |
| `deep_reflect` | What do reflections and refractions resolve to? | REAL path-traced render, quarter-res, 16 spp, 24 bounces, OIDN-denoised | No |
| `direct` | What does direct lighting alone contribute? | REAL path-traced render, half-res, 8 spp, direct lighting only (1 bounce), OIDN-denoised | No |
| `indirect` | What does indirect (bounced) light alone contribute? | REAL path-traced render, half-res, 12 spp, 16 bounces; beauty minus the direct/emission contribution at the camera-visible vertex -- a directly-lit surface or directly-visible emitter/env background reads BLACK, energy that arrived after >=1 bounce reads normally, OIDN-denoised | No |
| `clay_lights` | Is the lighting right, independent of materials? | REAL path-traced render, half-res, 12 spp, 12 bounces; every surface's reflectance replaced by a neutral mid-grey clay Lambertian, real lights/GI untouched, OIDN-denoised | No |

### Transport modes: `deep_reflect` / `direct` / `indirect` / `clay_lights` (GUI render modes P2a + P2b)

Unlike the four false-colour diagnostics above, all four transport modes
are REAL renders through the scene's actual lights (and, except
`clay_lights`, actual materials) -- an ephemeral, fixed-config
production-class path tracer, not a first-hit shader. Consequences that
don't apply to `normals`/`depth`/`facets`/`wireframe`:

- **Cost**: seconds, not milliseconds. Still cheap relative to a
  full-resolution production render (the fixed resolution divisor --
  quarter-res for `deep_reflect`, half-res for the other three -- does
  most of the work), but do not treat them as free the way you'd treat
  `objectmap`/`normals`/etc.
- **`effectiveSamples` reports the REAL spp used** (16, 8, 12, or 12),
  not the "1" the false-colour diagnostics report -- `quality`/`samples`
  are still ignored (the config is fixed by the registry), but check
  `effectiveSamples` if you want to confirm which mode actually ran at
  what fidelity.
- **`xray` is ignored** (honestly noted) -- skipping through glass
  would defeat the whole point of any of these four modes (seeing what
  the glass/material/lighting itself resolves to).

Recipes:

- **"What do the reflections/refractions in the crystal/case/metal
  actually resolve to?"** -- `render {mode:"deep_reflect"}`. Deep
  bounce depth (24) and a real sample count (16 spp) let specular
  chains (glass-through-glass, metal-on-metal, a caustic-adjacent
  reflection) actually converge, unlike a draft/false-colour render
  which either ignores materials entirely or only resolves the first
  hit.
- **"Is the LIGHTING right, independent of indirect bounce / GI?"** --
  `render {mode:"direct"}`. Direct-only transport (no indirect bounces)
  isolates whether light placement/power/colour looks right before
  indirect bounce and material response are layered on top -- a
  narrower, cheaper question than a full production render.
- **"Where does bounced light actually land?"** -- `render
  {mode:"indirect"}`. Beauty minus the direct contribution: a directly-
  lit wall or a directly-visible light/env background goes black, so
  whatever remains lit is being reached ONLY through at least one
  bounce -- useful for spotting where colour bleed, a hidden bounce
  card, or an indirect-only light leak is landing, without the direct
  term drowning it out visually.
- **"Check the lighting rig without material distraction."** -- `render
  {mode:"clay_lights"}`. Every surface's reflectance is replaced by a
  shared neutral clay while real lights and real GI stay untouched --
  the classic "is the lighting right independent of materials" check,
  one step up from `clay` (which also strips the real lights) when you
  need to confirm the ACTUAL light rig reads correctly on neutral
  surfaces before touching material authoring.

### Recipes: matching a symptom to a mode

1. **Placement check ("is X where I think it is / did the move land?")**
   -- `render {mode:"objectmap"}` for the whole-frame survey, then
   `query_object_at {x,y}` to confirm a specific pixel. Same recipe the
   decision table above already gives for identity questions.
2. **Orientation or smoothing artifact ("this looks shaded wrong / the
   normal looks flipped / there's a facet crease that shouldn't be
   there")** -- reach for `normals` first (world-space shading normal
   as false colour -- catches flipped windings, bad tangent frames,
   inverted normal maps at a glance). If `normals` looks plausible but
   the SILHOUETTE still creases oddly, switch to `facets` (the
   GEOMETRIC normal, bypassing any shading-normal smoothing/bump) to
   tell apart "the mesh itself is faceted" from "the shading normal is
   wrong" -- `normals` can look smooth over a low-poly mesh (smoothed
   shading normals hiding the facets) while `facets` reveals the true
   tessellation underneath.
3. **Tessellation / edge layout ("how is this mesh actually
   triangulated? where are the UV seams likely to fall? is this
   polycount too coarse for that curve?")** -- `wireframe`. Remember
   the mesh-only limitation below before reaching for it on an analytic
   primitive or SDF.
4. **Scale or occlusion sanity ("is this object actually behind that
   wall, or does the camera clip through geometry, or is that gap real
   depth or just dim shading?")** -- `depth`. Read brightness as
   RELATIVE distance within this one frame, not an absolute unit scale
   (see the normalization limitation below).
5. **"What's under/inside that glass / crystal / transparent cover?"**
   -- any of the four false-colour view modes already sees through it:
   `xray` defaults to `true`, so `render {mode:"depth"}` or `render
   {mode:"facets"}` already resolve the ray straight through
   transmissive surfaces to the first opaque hit (no refraction bending
   -- deliberately an x-ray, not an optics simulation). Add
   `xray:false` to see the glass surface itself instead of what's
   underneath it. See "X-ray axis" below. (`deep_reflect` answers a
   DIFFERENT question about the same glass -- not what's under it, but
   what the glass ITSELF does to light passing through -- and ignores
   `xray` entirely; see "Transport modes" above.)
6. **"What do reflections/refractions actually resolve to?"** --
   `render {mode:"deep_reflect"}`. See "Transport modes" above.
7. **"Is the lighting right, independent of materials/indirect
   bounce?"** -- `render {mode:"direct"}`. See "Transport modes" above.
8. **"Where does bounced light land?"** -- `render {mode:"indirect"}`.
   See "Transport modes" above.
9. **"Check the lighting rig without material distraction."** --
   `render {mode:"clay_lights"}`. See "Transport modes" above.
10. **"Render the same scene from several saved angles."** -- pass
   `view:"<name>"` on ANY `render` call (composes with every mode
   above). Resolves a live GUI session's Named View bookmark, or --
   headless -- a scene camera of that exact name. See "The `view`
   param" below.
11. **"Is THIS ONE light doing what I think -- shadow shape, colour,
   falloff -- without the others visually competing for attention?"**
   -- pass `light:"<name>"` on `beauty` or any of the four transport
   modes. Every other light contributes exactly zero (an unbiased
   partition of the full lighting, not a dim/approximate preview of
   it). See "The `light` param" below.

### Known limitations (read before reporting a "bug")

1. **`wireframe` draws edges on triangle meshes only.** Analytic
   primitives (sphere, box, cylinder, ...) and SDF geometry have no
   tessellation to draw an edge FROM -- they render as dim facet
   shading with no lines. That is correct, documented behaviour, not a
   missing feature -- see docs/gui/RENDER_MODES.md §10.
2. **`depth` brightness is AUTO-WINDOWED per render to the VISIBLE
   hit-distance range in THAT frame, not a fixed distance unit.** Two
   renders of the same scene from different camera framings (a wide
   shot vs. a close-up) use DIFFERENT brightness scales -- never compare
   two `depth` renders as if they shared one absolute scale, only read
   brightness RELATIVE to other pixels in the SAME image. The window
   self-calibrates within a single `render` call, so you always get a
   windowed (not flat/fallback) image. (The engine falls back to a fixed
   scene-bounding-box-diagonal scale only for a genuinely degenerate
   scene -- an empty frame or a single flat plane filling it -- so a
   mostly-flat scene can still read as unusually uniform; that is
   expected, not a broken render.)
3. **Every non-`beauty` mode (`objectmap`, the four false-colour view
   modes, AND the four transport modes) ignores `quality` and `samples`
   unconditionally.** Each has exactly one FIXED fidelity by design --
   an exact single-pass diagnostic image for `objectmap`/`normals`/
   `depth`/`facets`/`wireframe`, a fixed higher-spp production-class
   render for `deep_reflect`/`direct`/`indirect`/`clay_lights` --
   requesting a different sample count or `quality:"draft"` under any of
   them is a silent no-op, honestly noted in the result `message`, not
   an error. Check `effectiveSamples` for the ACTUAL spp used (1 for the
   false-colour modes, 16/8/12/12 for deep_reflect/direct/indirect/
   clay_lights).
4. **`xray` is a straight line, not real refraction.** It follows the
   ORIGINAL ray direction through every transmissive surface it skips
   -- no bending, no lensing. It answers "what's under/inside this
   transmissive geometry", not "what would actually be seen looking
   through it". A miss after 16 skipped surfaces shows the LAST
   transmissive surface instead of a black hole -- an honest partial
   answer, not a bug.

### X-ray axis: seeing through transmissive geometry

`xray` is an optional boolean param on `render`, ORTHOGONAL to `mode` --
it composes with all four view modes (`normals`/`depth`/`facets`/
`wireframe`), not a mode of its own. **Defaults to `true`** (2026-07-17):
a view-mode render already resolves each ray THROUGH transmissive
(glass-like) surfaces to the first OPAQUE hit, following the ORIGINAL
ray's straight line with NO refraction bending -- deliberately an
x-ray, not an optics simulation. Up to 16 transmissive surfaces are
skipped per ray.

Recipe: **"what's under/inside the glass/crystal/transparent cover?"**
-- just `render {mode:"depth"}` or `render {mode:"facets"}`, no extra
param needed. Add `xray:false` to see the glass/crystal surface itself
instead of what's underneath it -- e.g. to inspect a cover's own
tessellation or normals rather than the mechanism inside.

`xray` is silently ignored (honestly noted in the result `message`)
under `mode:"beauty"`, `mode:"objectmap"`, or a transport mode
(`deep_reflect`/`direct`/`indirect`/`clay_lights`) -- it only means
something for the four false-colour view-mode diagnostics.

## The `view` param: rendering from a saved vantage

`view` is an optional string param on `render`, ORTHOGONAL to `mode` --
it composes with EVERY mode (`beauty`, `objectmap`, any view mode, any
transport mode). Pass a name instead of raw `camera`
location/lookat/up/fov numbers:

```json
{"method": "render", "params": {"mode": "deep_reflect", "view": "hero-angle"}}
```

Resolution order: (1) a live in-app GUI session's Named View bookmark
of that exact name; (2) headless (`rise --agent-stdio`, no live
controller), a scene CAMERA of that exact name. An unresolved name
FAILS the render (`ok:false`) with the available-name list in
`message` -- it never silently falls back to the active camera. If
BOTH `view` and an explicit `camera` override are supplied, `view`
wins.

Use it to compare the SAME render mode from several saved angles
without re-deriving camera math each call -- e.g. checking
`deep_reflect` from three named views of a jewel/watch-crystal scene to
confirm the reflections read correctly from every angle a user might
actually look from.

## The `light` param: isolating one light (light solo)

`light` is an optional string param on `render`, ORTHOGONAL to `mode`
and to `view` -- it composes with `beauty` (the default) and all four
transport modes (`deep_reflect`/`direct`/`indirect`/`clay_lights`).
Pass the name of a light (or an emissive object) to render with it as
the ONLY active light in the scene:

```json
{"method": "render", "params": {"mode": "direct", "light": "keylight"}}
```

This is a genuine **unbiased partition** of the scene's lighting, not
a dim/approximate preview: every OTHER light contributes exactly zero
(both its next-event-estimation contribution AND its BSDF-sampled
emission are suppressed), so summing a solo render of every light in
the scene reproduces the un-soloed render, up to Monte-Carlo noise --
`light` never double-counts or drops energy, it only isolates it. Use
it to inspect one light's shadow shape, colour, and falloff without
the other lights visually competing for attention, or to sanity-check
a specific light's placement/power in isolation before touching the
whole rig.

Resolution: the name is matched first against the scene's lights (any
light type, by name), then against named scene objects whose material
is emissive (a mesh area light, by name). An unresolved name FAILS the
render (`ok:false`) with the available-name list in `message` -- same
contract as an unresolved `view`. `light` is silently ignored (an
honest note in `message`) under `objectmap`, the four false-colour
diagnostics (`normals`/`depth`/`facets`/`wireframe`), and
`quality:"draft"` -- none of those evaluate scene lighting at all, so
there is nothing for `light` to isolate.

Recipe: **"Which light is casting that shadow / that colour cast?"**
-- solo each candidate light in turn (`light:"keylight"`,
`light:"filllight"`, ...) and compare; the offending light is the one
whose solo render reproduces the shadow/cast you're chasing.

## Escalation ladder (cost, cheapest first)

1. **`read_viewport`** -- free, no render at all; only works when a live
   GUI controller is attached and has produced at least one frame.
2. **`query_object_at` / `render {mode:"objectmap"|"normals"|"depth"|
   "facets"|"wireframe"}`** -- about one identity-or-diagnostic render
   at the EFFECTIVE dims (the scene's authored dims unless you pass
   paired `width`/`height` -- do pass small ones, e.g. 128x128, or a
   large authored film makes this step needlessly expensive); fixed
   single-fidelity, no samples/quality to tune, for ALL five mode
   values. Use `query_object_at` for a single pixel's identity, the
   full `mode:"objectmap"` render for a whole-frame identity survey, or
   one of the four structural view modes (see "View modes" above) when
   the question is about geometry/normals/tessellation rather than
   identity.
3. **`render {quality:"draft"}`** -- a real render through a cheap
   separate pipeline, samples capped at 4; tiny `width`/`height` keep
   it fast. Both this and step 2 work on a head with NO active
   production rasterizer chunk -- neither dereferences one.
4. **`render {mode:"deep_reflect"|"direct"|"indirect"|"clay_lights"}`**
   -- a REAL production-class render (real materials/lights/OIDN, except
   `clay_lights` which neutralizes materials) at a fixed reduced
   resolution and fixed higher sample count -- seconds, not
   milliseconds, but still far cheaper than full-res production. Reach
   for this ONLY for the narrow transport/lighting question each
   answers (see "Transport modes" above); it costs more than every step
   before it. Also works on a head with NO active production rasterizer
   chunk (its own ephemeral pipeline, never the production one).
5. **`render` (production)** -- the real integrator at its authored
   (or overridden) sample count. The only step that costs what the
   final image costs; do it last, once the cheap steps already agree
   the edit is roughly right.

This is the loop the modeling-workflow-and-geometry skill's "observe
loop" section already teaches for the geometry-blockout case (insert,
draft-preview, second angle, confirm, THEN full-size) -- this skill
generalizes that same cost-aware escalation to the other three lenses
(viewport, objectmap/query, production) it doesn't cover. Read that
skill first for the multi-angle geometry workflow; come here to decide
which LENS to reach for at each step, including the two it doesn't
mention (viewport, objectmap/query).

## Hard warnings

1. **`quality:"draft"` renders through a wholly separate, fixed
   studio-preview pipeline that IGNORES the scene's authored materials
   and lighting entirely.** Geometry, composition, and camera framing
   are representative; materials, lighting, exposure, and colour are
   NOT. NEVER judge those from a draft image. Check the result's
   `renderMode` field ("production"/"draft"/"objectmap"/"normals"/
   "depth"/"facets"/"wireframe") to see which pipeline actually ran --
   `integrator` always names the head's active PRODUCTION rasterizer
   regardless of `quality`, so it is NOT the field that tells you which
   shading produced THIS image.
2. **Objectmap/`query_object_at` colours are identity ids, not
   appearance.** Match by exact `colorHex` byte, never "by eye" --
   when the palette is exhausted the result's `message` says so
   explicitly. Read an objectmap render's PNG at NATIVE size (omit
   `maxEdge` on `read_image`) -- downscaling box-blends the flat ids
   and breaks the legend match. `render {mode:"objectmap"}` therefore
   REJECTS `imageMaxEdge`: render it without, then `read_image` with no
   `maxEdge`. A pixel that matches NO legend entry
   is the reserved BACKGROUND: RGB `000000` at alpha 0 (never assigned
   to an object); the magenta `FF00FF` sentinel plus an `<unmapped>`
   legend entry mark the should-not-happen unregistered-object case.
   Pixel coordinates: `(0,0)` is the TOP-LEFT corner and y grows
   DOWNWARD, for both `query_object_at {x,y}` and the decoded PNG's
   row order -- they always agree.
3. **`read_viewport`'s `available:false` is a STRUCTURED SUCCESS, and
   which reason you got determines what to do next.** There are SEVEN
   reasons, and `render` is a fallback for exactly two of them:

   | `reason` | Retriable? | What to do |
   |---|---|---|
   | `no_controller` | No -- never | Headless session: there is no GUI viewport at all. Use `render`. |
   | `no_frame_yet` | Not by retrying -- but it does resolve | The viewport exists and hasn't drawn yet. It will draw on its own; nothing you do speeds that up. Use `render` meanwhile. |
   | `editor_transaction_in_progress` | Yes | The user is mid-gesture or saving. Wait a moment and retry read_viewport. |
   | `render_in_progress` | Yes | A render -- the agent's, or the USER'S OWN production render -- holds the admission gate. Retry `read_viewport`; it is free. Do NOT switch to `render` (see the prose below: a 30 s block, then likely a refusal). |
   | `editor_interaction_finalize_failed` | Yes | An open editor interaction could not be finalized this time. Retry. |
   | `editor_shutting_down` | No -- never | The editor is tearing down. Stop observing. |
   | `editor_interaction_unrecoverable` | No -- never | An editor interaction failed to persist and the editor LATCHED that failure. It does NOT clear. Tell the user; do not retry, and do not switch to `render` -- it is refused too. |

   **Do NOT reflexively fall back to `render`.** `render` is the right
   move for the two no-viewport reasons (`no_controller`,
   `no_frame_yet`). For `editor_transaction_in_progress`,
   `editor_interaction_finalize_failed`, `editor_shutting_down` and
   `editor_interaction_unrecoverable`, a `render` call passes through
   the SAME editor/admission gate that just refused read_viewport and
   is refused too (and on the permanent ones, an infinite loop if you
   keep trying).  That leaves `render_in_progress`, the seventh and only
   split case -- covered in the paragraph immediately below, and still
   not a good fallback.

   `render_in_progress` is the ONE reason where a `render` call does
   not hit that same gate -- but it is still a POOR fallback, because
   it can be refused two different ways:

   - A **plain `render {}`** -- no `width`+`height` pair, no `camera`,
     no `view` -- does NOT take the parked path read_viewport takes.
     It queues on the agent-render slot and WAITS up to 30 s. It
     SUCCEEDS if the occupant finishes inside that window, and is
     REFUSED ("render queued or in progress -- retry after it
     completes") if the occupant outlives it. The commonest occupant
     in a live GUI session is the USER'S OWN production render, which
     runs through the very same single slot and routinely takes far
     longer than 30 s -- so the realistic outcome there is a
     thirty-second block followed by a refusal.
   - It is also refused with **NO wait at all** when the gate is held
     by a **direct parked render** -- a film/camera-override agent
     render, or a `read_viewport` in flight. (Those are the only two
     producers: the parked path has exactly two callers, both agent
     ones, and the GUI hosts more than one agent session, so the
     holder may be a sibling session rather than yours. The GUI's own
     interactive preview is NOT a producer -- the gate is what blocks
     it.) That render owns the admission gate without occupying the
     render slot, so the fairness wait is satisfied instantly and the
     admission check refuses on the spot. Do NOT tell the user their
     viewport preview is holding it.
   - A render carrying a **film override (`width` AND `height`)** or a
     **`camera`/`view` override** always takes the parked path and IS
     refused, immediately, for the same reason read_viewport was.
   - `quality:"draft"` and `mode:` do NOT change this by themselves --
     only a width+height pair or a camera/view override changes the
     routing.

   **What to do instead.** `read_viewport` is FREE -- it never renders
   -- and it becomes available the instant the gate clears, so ONE
   short retry of `read_viewport` is the cheap poll here, not
   `render`. If a second read still reports `render_in_progress`, a
   long render (most likely the user's own) owns the gate: tell the
   user and wait for it rather than blocking 30 s on a render that
   will probably be refused anyway. Reach for a plain `render {}` only
   when you genuinely need a NEW image rather than the viewport's
   current one, and budget for that 30 s block.
4. **A generator-synthesized legend name (`grid[0,1]`) is not a CST
   chunk.** `mode:"objectmap"` and `query_object_at` both report it
   verbatim so you can tell instances apart on screen, but to EDIT
   that instance you target the GENERATOR chunk (strip the `[i,j]`
   suffix), not the instance name.
5. **`width`/`height` overrides must be paired (on `render` and
   `query_object_at`; `read_viewport`/`read_image` take `maxEdge`
   instead).** Passing only one is silently ignored (not rejected) and
   the call proceeds at the scene's authored dims -- always pass BOTH,
   clamped `[16,512]`, and confirm the override took by reading
   `previewWidth`/`previewHeight` (`render`) or `width`/`height`
   (`query_object_at`) back from the result.
6. **Draft, objectmap/query_object_at, AND every view mode --
   `normals`/`depth`/`facets`/`wireframe` AND
   `deep_reflect`/`direct`/`indirect`/`clay_lights`
   -- all work on a rasterizer-less head; production does not.** Every
   cheap-render path (including the four transport modes) runs its OWN
   ephemeral pipeline and never touches the production rasterizer, so
   all of them succeed even before any `*_rasterizer` chunk exists in
   the scene -- a production `render` call on the same head fails
   honestly instead.
7. **`deep_reflect`/`direct`/`indirect`/`clay_lights` are NOT free like
   the other four view modes.** They cost seconds (a real path-traced
   render at a fixed reduced resolution/sample count), not
   milliseconds. Reach for a false-colour diagnostic
   (`normals`/`depth`/`facets`/`wireframe`) or `objectmap` first if the
   question is structural rather than about transport/lighting --
   don't default to one of these four for a question `facets` or
   `depth` already answers for free.

## Worked example: locating an object, cheaply, before editing it

The scene below has a red sphere on the left and a blue box on the
right -- a stand-in for "find the sphere, then confirm it's roughly
centered before nudging it."

```rise
RISE ASCII SCENE 7

standard_shader
{
	name		global
	shaderop	DefaultPathTracing
}

pathtracing_pel_rasterizer
{
	samples			16
	pixel_filter	box
	oidn_denoise	FALSE
}

film
{
	width	128
	height	128
}

pinhole_camera
{
	location	0 2 6
	lookat		0 0.5 0
	up			0 1 0
	fov			45.0
}

uniformcolor_painter
{
	name	pnt_floor
	color	0.5 0.5 0.5
}

uniformcolor_painter
{
	name	pnt_sphere
	color	0.8 0.2 0.2
}

uniformcolor_painter
{
	name	pnt_box
	color	0.2 0.3 0.8
}

lambertian_material
{
	name		mat_floor
	reflectance	pnt_floor
}

lambertian_material
{
	name		mat_sphere
	reflectance	pnt_sphere
}

lambertian_material
{
	name		mat_box
	reflectance	pnt_box
}

clippedplane_geometry
{
	name	floor
	pta		-4 0 -4
	ptb		4 0 -4
	ptc		4 0 4
	ptd		-4 0 4
}

standard_object
{
	name		obj_floor
	geometry	floor
	material	mat_floor
}

sphere_geometry
{
	name	sph
	radius	0.6
}

standard_object
{
	name		obj_sphere
	geometry	sph
	material	mat_sphere
	position	-1.1 0.6 0
}

box_geometry
{
	name	box
	width	1.0
	height	1.0
	depth	1.0
}

standard_object
{
	name		obj_box
	geometry	box
	material	mat_box
	position	1.1 0.5 0
}

directional_light
{
	name		key
	power		3.0
	color		1 1 1
	direction	0.3 0.6 0.7
}
```

Step 1 -- cheap placement check, small draft render (materials/colour
NOT trustworthy here, only silhouette/position):

```json
{"method": "render", "params": {"quality": "draft", "width": 160, "height": 120}}
```

Step 2 -- "which object is at the pixel I think `obj_sphere` occupies?"
(cheap identity probe, no beauty render needed):

```json
{"method": "query_object_at", "params": {"x": 45, "y": 70}}
```

...or the whole-frame survey when more than one object needs locating
at once:

```json
{"method": "render", "params": {"mode": "objectmap"}}
```

If instead the box looked like it had a shading crease along one edge,
reach for a view mode rather than staring at the beauty render harder:

```json
{"method": "render", "params": {"mode": "facets"}}
```

Step 3 -- once the edit is confirmed cheaply, the ONE production render
that actually judges the result (materials, lighting, exposure are
only honest here):

```json
{"method": "render", "params": {}}
```

## Traps

1. **`renderMode` vs `integrator` -- read the right field.** `renderMode`
   ("production"/"draft"/"objectmap"/"normals"/"depth"/"facets"/
   "wireframe"/"deep_reflect"/"direct"/"indirect"/"clay_lights") tells
   you which pipeline made THIS image; `integrator`
   always names the head's active PRODUCTION rasterizer chunk
   regardless of `quality`. Checking `integrator` to see if a draft
   render "used PT" is a category error -- it always
   answers a different question.
2. **A draft render's `samples` request is honestly capped, not
   silently honored.** Ask for `samples:64` under `quality:"draft"`
   and the result's `effectiveSamples` still reads back the cap (4)
   with a note in `message` -- don't assume a higher request took
   effect just because it wasn't rejected.
3. **`read_viewport`'s `available:false` is not automatically a retry
   signal -- read the `reason`.** A headless session
   (`rise --agent-stdio`, or this same skill's render-contract test
   harness) reports `no_controller` and will NEVER have a viewport;
   spinning there waiting for `available:true` hangs forever for no
   reason. Use `render` instead -- for `no_controller` and
   `no_frame_yet`. `editor_shutting_down` and
   `editor_interaction_unrecoverable` are equally permanent AND a
   `render` call hits the same gates, so neither retrying nor
   switching to `render` helps; say so and stop. The three retriable
   reasons (`editor_transaction_in_progress`, `render_in_progress`,
   `editor_interaction_finalize_failed`) do clear on their own -- one
   short retry of `read_viewport` is reasonable. `render` is not a way
   around the first and third of those. For `render_in_progress` a
   PLAIN `render {}` (no `width`+`height`, no `camera`/`view`) at
   least reaches the render slot instead of the parked path -- but it
   then BLOCKS up to 30 s and is STILL refused if the occupant
   outlives that window (the user's own production render shares the
   slot and usually does) or if a direct parked render holds the gate
   (no wait at all -- instant refusal). Retry the free `read_viewport`
   rather than paying that block; a render with a film or camera
   override is refused outright. Full table under "Hard warnings" item
   3 above.
4. **An empty-pixel `query_object_at` is `hit:false`, not an error.**
   The probe pixel resolved to background -- a normal, structured
   result. An actually out-of-range `(x,y)` for the effective film
   dims IS a clean error (checked before any render runs), which is a
   different failure mode from `hit:false` -- don't conflate the two.
