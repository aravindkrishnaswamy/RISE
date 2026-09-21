//////////////////////////////////////////////////////////////////////
//
//  RayIntersectionGeometric.h - A class that describes the geometric
//  aspects of ray intersection
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 17, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RAY_INTERSECTION_GEOMETRIC_
#define RAY_INTERSECTION_GEOMETRIC_

#include "../Utilities/Ray.h"
#include "../Utilities/OrthonormalBasis3D.h"
#include "../Utilities/Color/Color.h"
#include "../Interfaces/ISurfaceSignalProvider.h"

namespace RISE
{
	//! Surface derivative data produced at intersection time.
	//! Populated by geometries that know the hit-local parameters
	//! (e.g., triangle mesh: the hit triangle + barycentric coords).
	//! Consumers like SMS ManifoldSolver read this directly to avoid
	//! a second lookup via IGeometry::ComputeSurfaceDerivatives.
	//! See docs/GEOMETRY_DERIVATIVES.md for the contract.
	struct SurfaceDerivativesInfo
	{
		Vector3 dpdu;
		Vector3 dpdv;
		Vector3 dndu;
		Vector3 dndv;
		bool    valid;  // true if geometry populated these fields

		//! CHARACTERISTIC LENGTH of the hit geometry, in WORLD units --
		//! the normalization factor behind the expression VM's
		//! dimensionless `curv` (design doc §5.2).  Mean curvature H has
		//! units of 1/length, which is a trap for an author writing
		//! `clamp(curv,0,1)` on a creature whose features have 0.05-unit
		//! radius; multiplying by this makes the exposed value O(1) at
		//! object scale on any scene scale.
		//!
		//! Stamped OBJECT-SPACE by the geometry at intersection time (the
		//! bounding-box diagonal, uniformly: SDFGeometry's `m_diagonal`,
		//! the mesh BVH root box, the analytic primitives' own radii),
		//! then folded with the object's world scale --
		//! `|det M|^(1/3)`, the linear-measure sibling of the
		//! `|det L|^(2/3)` area Jacobian `Object::GetArea()` uses -- at the
		//! `Object::IntersectRay` / `CSGObject::IntersectRay` transform
		//! layer.  Exact for rotations and uniform scales; under
		//! NON-UNIFORM scale no single scalar can be right (a
		//! `scale 4 0.05 4` panel has no one characteristic length) and
		//! this is the documented geometric-mean approximation.
		//!
		//! DEFAULT 1.0 -- the honest "this geometry did not say", which
		//! makes `curv` collapse to the raw `curvR` rather than to zero.
		//! Populated only when SurfaceCurvatureDemand::Any() (nothing else
		//! reads it, so nothing else should pay for it).
		Scalar  scaleHint;

		//! DIRECT per-hit mean curvature, for geometry families that can
		//! answer the curvature question better than a synthesized
		//! `dndu`/`dndv` pair could (design doc §5.4).  Today that means
		//! the SDF family, whose implicit surface has no natural `(u,v)`
		//! for which those partials are meaningful but whose
		//! `div n_hat = k1 + k2` is a few field evaluations away.
		//!
		//! Same sign convention, units and world-measure as the shape
		//! operator's H (see SurfaceCurvature.h): positive = convex,
		//! 1/world-length after the `Object::IntersectRay` fold divides
		//! out the same `|det M|^(1/3)`.  `BuildContext` PREFERS this when
		//! `curvatureValid`, and falls back to the shape operator over the
		//! four vectors above.
		//!
		//! Independent of `valid`: the SDF sets `curvatureValid` while
		//! leaving `valid` false, and a mesh does the reverse.
		Scalar  curvature;
		bool    curvatureValid;

		//! THE CHART MAP -- the 2x2 Jacobian of the TEXTURE coordinate
		//! `(s, t)` stamped into `RayIntersectionGeometric::ptCoord`
		//! with respect to the `(u, v)` parameters `dpdu` / `dpdv`
		//! differentiate.  Mirrors `SurfaceDerivatives::dsdu`… on the
		//! `IGeometry` side; see the long comment there and
		//! docs/GEOMETRY_DERIVATIVES.md "The texcoord chart map".
		//!
		//! Why it exists: the analytic primitives parameterise by their
		//! own natural coordinates (sphere `u` = azimuth in RADIANS,
		//! cylinder `u` = axial WORLD coordinate, torus/cylinder with
		//! the two axes SWAPPED relative to their texture coordinate),
		//! while `ptCoord` is the normalised `[0, 1]^2` chart the
		//! matching `GeometricUtilities::*TextureCoord` produces.
		//! `SolveFootprintUV` solves the pixel differentials in the
		//! DERIVATIVE chart and then applies this map, so the
		//! `dudx`…`dvdy` it publishes are in the same chart as
		//! `ptCoord` -- which is the pairing `TexturePainter::
		//! SampleTextured` and `WeaveBRDF` assume.  Without it a
		//! textured sphere mips 1.65 levels too blurry, a cylinder
		//! 1.59 and a torus 2.65 (re-measured in fix round 2 by
		//! disabling the multiply-through and reading
		//! `TextureFootprintTest` test 11's per-geometry LOD error:
		//! 1.651 / 1.585 / 2.651, plus 2.33 for the ellipsoid; the
		//! 1.67 / 1.02 / 2.64 this comment used to carry disagreed
		//! with both design docs and, on the cylinder, was not a
		//! rounding of anything).
		//!
		//! DEFAULT identity with `texChartValid = false`, which makes
		//! `SolveFootprintUV` decline to publish a Jacobian at all
		//! (`valid` stays false, `widthValid` is unaffected) rather
		//! than publish one in the wrong chart.
		Scalar  dsdu, dsdv, dtdu, dtdv;
		bool    texChartValid;

		SurfaceDerivativesInfo() :
		dpdu( Vector3(0,0,0) ), dpdv( Vector3(0,0,0) ),
		dndu( Vector3(0,0,0) ), dndv( Vector3(0,0,0) ),
		valid( false ),
		scaleHint( 1.0 ),
		curvature( 0 ), curvatureValid( false ),
		dsdu( 1 ), dsdv( 0 ), dtdu( 0 ), dtdv( 1 ), texChartValid( false )
		{
		}
	};

	//! Pixel footprint at the hit point — the projection of the
	//! incoming ray's screen-space differentials onto the surface
	//! tangent plane, plus (where the surface has a UV chart) the
	//! resulting UV Jacobian.  Populated at intersection time by
	//! `Object::IntersectRay` for EVERY geometry, whenever the
	//! incoming ray has hasDifferentials = true (see
	//! docs/TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md).  Consumed by
	//! TexturePainter to compute mip LOD per Landing 2 of the PB
	//! pipeline plan, and by ExpressionPainter/ExpressionScalarPainter
	//! (doc 88 S9) to populate ExprEvalContext::fw for footprint-aware
	//! fbm/turbulence/ridged octave fade.
	//!
	//! TWO INDEPENDENT VALIDITY FLAGS, with the invariant
	//! `valid ⇒ widthValid`:
	//!
	//!   - `widthValid` — `dpdx`, `dpdy`, `worldWidth` and
	//!     `objectWidth` are usable.  Requires only ray differentials
	//!     and a surface normal, so it is set on every geometry, UV
	//!     chart or not.
	//!   - `valid` — the UV Jacobian (`dudx`…`dvdy`) is usable.
	//!     Additionally requires `derivatives.valid` (a non-degenerate
	//!     dpdu/dpdv basis).  Deliberately NOT widened to mean "some
	//!     footprint exists": `TexturePainter::SampleTextured` and
	//!     `WeaveBRDF` key on it, and a zero Jacobian handed to
	//!     `ComputeLODFromTexelFootprint` would read as LOD 0 (finest)
	//!     rather than the honest base-level fallback.
	//!
	//! Units: dudx / dudy / dvdx / dvdy are the partial derivatives
	//! of the surface UV coordinates with respect to screen-space
	//! pixel x and y.  In other words, advancing one pixel in x
	//! moves the UV by (dudx, dvdx).  The texture-space Jacobian
	//! follows by multiplying by texture width / height.
	//!
	//! dpdx / dpdy are the surface-plane displacements a ONE-PIXEL
	//! step in screen x / y induces at this hit point, and worldWidth
	//! is the mean of their magnitudes — a filter-WIDTH (diameter-like,
	//! full pixel step, not a radius) estimate in the same length units
	//! as ptIntersection / the expression VM's `P`.  All three are 0
	//! when !widthValid.
	//!
	//! `objectWidth` is that SAME width measured in the frame
	//! `ptObjIntersec` (the expression VM's `Po`) is written in — an
	//! OBJECT-space length (2026-09-06).  It exists because the
	//! expression VM can filter an object-space noise domain
	//! (`fbm(Po*62, …)`) only if the hit reports the footprint in that
	//! frame: no compile-time constant converts between the two, since
	//! the object→world map is a per-INSTANCE fact.
	//!
	//! It is not a derived scalar — it is the pre-promotion value of
	//! `worldWidth`, captured by `Object::IntersectRay` in the SAME
	//! block, at the SAME nesting level, that computes
	//! `ptObjIntersec`.  That is what keeps the two co-framed at every
	//! level (a placed object; a CSG composite, where
	//! `AdoptCsgSurfacePayload` copies this whole struct and
	//! `ptObjIntersec` together from the winning CHILD, so both stay
	//! in that child's own frame and the CSG level promotes only
	//! `worldWidth`).  Deriving it instead as `worldWidth / |det
	//! M|^(1/3)` would be exact only under a uniform scale — the same
	//! geometric-mean approximation §3.4 of
	//! docs/TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md retired for
	//! `worldWidth` because it under-counted a `scale 4 0.05 4` panel
	//! by 4.31×.
	//!
	//! Under NON-UNIFORM object scale it carries the same anisotropy
	//! caveat `worldWidth` does: one isotropic width stands in for an
	//! elliptical footprint, so it reports the MEAN of the two
	//! object-space step magnitudes rather than a per-axis pair.  0
	//! when !widthValid.
	//!
	//! FRAME: these fields live in whatever frame the record itself
	//! currently lives in.  The geometry layer stamps them from the
	//! OBJECT-space ray `Object::IntersectRay` transformed on entry, and
	//! `Object::IntersectRay` / `CSGObject::IntersectRay` promote them
	//! to world by applying their own forward map `m_mxFinalTrans` to
	//! `dpdx`/`dpdy` and re-deriving `worldWidth` from the transformed
	//! pair.  That promotion is EXACT for any linear map — non-uniform
	//! scale and shear included — because an affine map carries the
	//! object-space auxiliary line onto the world auxiliary line and the
	//! object-space tangent plane onto the world tangent plane, so the
	//! line∩plane point commutes with the map.  (It superseded a
	//! `worldWidth *= |det M|^(1/3)` geometric-mean fold, which
	//! under-counted a `scale 4 0.05 4` panel by 4.31x.)  By the time
	//! any consumer (ExpressionPainter's `fw`, ReliefModifier's
	//! footprint-aware step) reads these fields they ARE world-space.
	struct TextureFootprint
	{
		Scalar  dudx, dudy;
		Scalar  dvdx, dvdy;
		Vector3 dpdx, dpdy;
		Scalar  worldWidth;
		Scalar  objectWidth;	// the same width in ptObjIntersec's frame
		bool    valid;			// the UV Jacobian is usable
		bool    widthValid;		// dpdx / dpdy / worldWidth / objectWidth are usable

		TextureFootprint() :
		dudx( 0 ), dudy( 0 ), dvdx( 0 ), dvdy( 0 ),
		dpdx( Vector3(0,0,0) ), dpdy( Vector3(0,0,0) ),
		worldWidth( 0 ), objectWidth( 0 ), valid( false ), widthValid( false )
		{
		}
	};

	//! Describes the current state of the rasterizer
	struct RasterizerState
	{
		unsigned int x;						// Which pixel on the row we are processing
		unsigned int y;						// Which row we are processing

		// ... more state elements will be added when they are deemed necessary
		bool operator==( const RasterizerState& other ) {
			return (x==other.x && y==other.y);
		}
	};

	static const RasterizerState nullRasterizerState = {0};

	class RayIntersectionGeometric
	{
	public:
		Ray							ray;			// the ray that is intersecting
		RasterizerState			rast;			// the rasterizer state
		bool						bHit;			// was there an intersection ? 
		Scalar						range;			// distance to the intersection point
		Scalar						range2;			// distance to the exit point
		Vector3						vNormal;		// normal at the point of intersection (SHADING normal — Phong-interpolated on triangle meshes, perturbed by the normal-perturbing modifiers: relief, normal map, glint)
		Vector3						vNormal2;		// normal at the point of exit
		//! GEOMETRIC normals at the entry / exit points — the actual
		//! flat-triangle face normal on triangle meshes (independent of
		//! Phong interpolation), or identical to `vNormal` / `vNormal2`
		//! on analytical primitives (sphere, ellipsoid, plane, …) where
		//! the surface IS smooth and shading == geometric by construction.
		//! The normal-perturbing modifiers (relief, normal map, glint,
		//! relief) perturb `vNormal` only; `vGeomNormal` / `vGeomNormal2`
		//! always reflect the underlying geometry.
		//!
		//! Use these — not `vNormal` / `vNormal2` — for queries that ask
		//! "which side of the actual surface is this direction on?".
		//! Examples: Specular Manifold Sampling's `ValidateChainPhysics`
		//! (Newton converged a chain whose ray-segment topology must
		//! match the real geometry, not the shading approximation), and
		//! any refraction / TIR check that depends on physical surface
		//! orientation rather than visual shading.
		//!
		//! `vGeomNormal2` is symmetric with `vNormal2` — populated only
		//! when the geometry computes exit info (`bComputeExitInfo`).
		//! CSG composition requires both fields so that promoting a
		//! child's exit boundary to a parent's entry uses the actual
		//! geometric exit normal, not a fabricated `-entry` proxy.
		Vector3						vGeomNormal;
		Vector3						vGeomNormal2;

		//! OUTPUT: set by geometries that flip `vGeomNormal` to oppose the
		//! incoming ray.  Default false; the authoritative list of which
		//! geometries set it is below -- geometries that do not flip leave
		//! this false, so the recovery formula below is a no-op for them.
		//!
		//! Consumers that need the TRUE surface facing (which side of the
		//! actual geometry the ray struck, independent of the double-sided
		//! shading convention) must NOT read `Dot(vGeomNormal, dir)`
		//! directly -- that reads the post-flip orientation, which is
		//! deliberately always negative (facing the ray) on a double-sided
		//! hit and therefore useless for distinguishing a true entry from a
		//! true exit.  Recover the real facing as:
		//!
		//!     oriented ? -Dot(vGeomNormal, dir) : Dot(vGeomNormal, dir)
		//!
		//! (RayCaster::ResolveXrayView_'s degenerate-self-hit classifier is
		//! the motivating consumer: a thin double-sided mesh's entry and
		//! genuine exit both face the ray under the flip, which collapses
		//! the raw dot-product facing test to the same sign on both --
		//! this flag lets the caster undo the flip losslessly instead.)
		//!
		//! WHICH GEOMETRIES SET IT (keep this list current -- an earlier
		//! version of this comment, and copies of it in TranslucentSPF.cpp
		//! and IORStackSeeding.h, claimed "single-sided meshes and every
		//! analytical primitive leave the flag false", which is only half
		//! the story):
		//!   * `TriangleMeshGeometry` / `TriangleMeshGeometryIndexed` --
		//!     only when `bDoubleSided`, and only when the flip actually
		//!     happened.
		//!   * `BezierPatchGeometry`, `ClippedPlaneGeometry` -- on a
		//!     BACK-FACE hit (they flip the normal toward the ray there,
		//!     exactly like the double-sided mesh path).
		//!   * `HairGeometry` -- UNCONDITIONALLY; see
		//!     `bGeomNormalRayDerived` below, because the recovery formula
		//!     above does NOT apply there.
		//!   * `CSGObject` propagates whichever operand's surface it is
		//!     actually reporting.
		//! Single-sided triangle meshes and the analytical primitives
		//! (sphere, box, ellipsoid, torus, cylinder, plane, disk, bilinear
		//! patch) do leave it false.
		//! A consumer that instead wants the true, ray-independent
		//! GEOMETRIC NORMAL VECTOR itself (not just its dot-product sign
		//! against one particular ray direction -- e.g. an entry point's
		//! outward normal for a Fresnel/cosine term computed against an
		//! arbitrary later direction, such as a light sample) recovers it
		//! the same way, applied to the vector rather than the scalar dot:
		//!
		//!     oriented ? -vGeomNormal : vGeomNormal
		//!
		//! This is unconditional on WHERE or in what direction the hit was
		//! reached -- the flip predicate is evaluated fresh per hit by each
		//! setter above (see e.g. TriangleMeshGeometry::IntersectRay's
		//! `bFlipGeomNormal`), so undoing it always recovers that hit's true
		//! winding-order normal, never a position- or direction-dependent
		//! approximation of one (BSSRDFSampling.cpp's DL-71/DL-75 probe
		//! correction is the motivating consumer -- see that file).
		//! ONLY valid when `bGeomNormalRayDerived` is false; see that flag.
		bool						bGeomNormalOrientedToRay;

		//! OUTPUT: set by geometries whose `vGeomNormal` is DERIVED FROM
		//! THE RAY rather than being a static property of the surface.
		//! `HairGeometry` is the only such geometry today: a hair strand is
		//! a 1-D curve with no two-sided surface, so it fabricates a flat
		//! normal `Nflat = normalize(-(D - (D.T)T))` from the ray direction
		//! `D` and the strand tangent `T` -- by construction always facing
		//! the ray -- and reports `bGeomNormalOrientedToRay = true`
		//! unconditionally.
		//!
		//! CONTRACT: when this is true, `bGeomNormalOrientedToRay`'s
		//! recovery formula above is MEANINGLESS.  Un-flipping a
		//! ray-derived normal yields a direction that always faces AWAY
		//! from the ray, so a "true entry vs. true exit" classifier built
		//! on it reads EXIT at every single crossing.  A consumer asking a
		//! which-side-of-the-real-surface question must therefore SKIP such
		//! a hit (there is no real side to be on) rather than trust the
		//! recovery -- `IORStackSeeding::SeedFromPoint`'s containment probe
		//! skips it, and `TranslucentSPF`'s exit gate falls back to the
		//! shading normal (making the gate a no-op).  A consumer that only
		//! needs "which way does the surface face the ray" may keep reading
		//! `vGeomNormal` directly; `bGeomNormalOrientedToRay` stays
		//! truthful about the reported orientation either way.
		bool						bGeomNormalRayDerived;

		//! OUTPUT: set by geometries whose double-sided/back-face
		//! `bGeomNormalOrientedToRay` flip happens on an OPEN 2-D SHEET
		//! rather than a closed, watertight solid -- i.e. a surface where
		//! BOTH faces are legitimate physical sides (a leaf, a cloth
		//! card, a bare `clipped_plane`/Bezier patch) rather than the
		//! inside/outside of a solid volume.  Default false.
		//!
		//! DL-96: the BSSRDF front-face admission gate
		//! (`PathTracingIntegrator.cpp`, `BDPTIntegrator.cpp`) uses
		//! `TrueGeomFacing()` to pick the SINGLE true outward face of a
		//! closed solid -- correct there (that is DL-70's own fix), but
		//! it silently drops subsurface-scattering entry from the SECOND
		//! face of an open sheet, where both faces are legitimate entry
		//! points.  `BSSRDFEntryFacing()` below reads this flag to admit
		//! entry on the RAY-FACING (reported) side instead, for that
		//! gate ONLY.  No other DL-70 consumer (the medium-stack walk,
		//! the transmissive-shadow Fresnel pair, the volume-shading
		//! entering/leaving test, the SMS photon `bEntering` stamp, the
		//! alpha-card `bOneSided` cull, the override-UV-generator normal)
		//! reads this flag -- they keep DL-70's closed-solid recovery
		//! unconditionally, so adding this flag does not change their
		//! behaviour.
		//!
		//! WHICH GEOMETRIES SET IT (mirrors `bGeomNormalOrientedToRay`'s
		//! own list, but narrower):
		//!   * `TriangleMeshGeometryIndexed` -- `bDoubleSided &&
		//!     !m_bWatertight` (DL-143's build-time position-weld
		//!     watertightness certification).
		//!   * `TriangleMeshGeometry` (the non-indexed twin) --
		//!     `bDoubleSided` unconditionally: this class has no
		//!     watertightness certification at all, so every
		//!     double-sided non-indexed mesh is treated as an open
		//!     sheet.
		//!   * `ClippedPlaneGeometry`, `BezierPatchGeometry` -- on a
		//!     back-face hit (the same condition each already uses for
		//!     `bGeomNormalOrientedToRay`): a plane or patch never
		//!     encloses a volume, so a double-sided hit on one is always
		//!     an open sheet.
		//! `HairGeometry` does NOT set it -- it is already excluded from
		//! the recovery via `bGeomNormalRayDerived`/`HasTrueGeomSide()`,
		//! for an orthogonal reason (no genuine two-sided winding at
		//! all, not "two legitimate sides of one winding").
		bool						bOpenSheet;

		//! DL-157 review round 2 (2026-09-18): the surface's OBJECT
		//! PROVABLY HAS NO INTERIOR.
		//!
		//! `bOpenSheet` does NOT mean this, and conflating the two is a
		//! measured defect.  For the two mesh classes it means
		//! "UNCERTIFIED": `TriangleMeshGeometryIndexed` sets it whenever
		//! DL-143's build-time weld could not certify watertightness --
		//! which, per DL-143's own audit, is all four glTF assets it
		//! examined, every one of them a closed solid -- and the
		//! non-indexed twin sets it on every double-sided hit because it
		//! has no certification at all.  A consumer that reads
		//! `bOpenSheet` as "no interior" therefore misreads a genuine
		//! interior EXIT on an ordinary closed mesh as an entry.
		//!
		//! Only a geometry whose SHAPE forbids an interior can set this,
		//! and today that is exactly ONE class: `ClippedPlaneGeometry`
		//! -- four corners spanning one bilinear sheet with a boundary,
		//! which cannot enclose a volume however it is transformed, so
		//! the claim needs no build-time check and cannot be wrong.  Set
		//! under the SAME back-face condition as `bOpenSheet` there, so
		//! a front-face hit leaves it false and the ordinary ray anchor
		//! applies.
		//!
		//! TWO CLASSES OF GEOMETRY CAN NEVER SET IT.
		//!
		//! (1) Anything MESH-LIKE.  "Not certified closed" is not
		//! "certified open", and `bOpenSheet` on the two mesh classes
		//! means only the former.
		//!
		//! (2) Anything holding a COLLECTION of primitives, however
		//! interior-free each one is on its own -- an interior is a
		//! property of the whole surface, and N open sheets can bound a
		//! volume that no single sheet can.  `BezierPatchGeometry` is
		//! the concrete case (DL-157 review round 3, P1): it reads as
		//! "one patch" from its name only.  `patches` is a vector with a
		//! BSP/Octree over it, and `Job.cpp`'s `.bezier` loader puts
		//! every patch of a file into ONE geometry --
		//! `models/raw/teapot.bezier` declares 28,
		//! `models/bezier/aphrodite.bezier` and `f16.bezier` are closed
		//! solids.  Round 2 stamped this flag there on the back-face
		//! condition; at a genuine interior exit on such an object that
		//! condition holds, so the stamp asserted "no interior" on
		//! precisely the hit that disproves it.  The setter was removed.
		//!
		//! Explicitly cleared by `CSGObject::IntersectRay` after operand
		//! copies and payload adoption, unlike
		//! `bOpenSheet`: `bOpenSheet` is a property of the SURFACE the
		//! ray struck and survives compositing, while this is a property
		//! of the OBJECT -- a CSG tree built from planes can perfectly
		//! well have an interior, so the wrapper must not inherit an
		//! operand's claim.
		//!
		//! WHO READS IT: `TranslucentSPFDetail::BuildLobeSet`'s STACKLESS
		//! side inference only (a caller with a live IOR stack asks the
		//! stack instead; modern PT/BDPT/VCM integrator paths are
		//! stacked, while photon gathers and SMS still have stackless sites).  See DL-157's closure doc section 3.1.
		bool						bProvablyNoInterior;

		//! Signed cosine in the shading hemisphere facing the incoming
		//! view ray. Ordinary two-sided BRDFs/SPFs orient this hemisphere
		//! to -ray.Dir(); a strong normal perturbation can cross the view
		//! horizon even while the geometric face still points at the view.
		//! Keep the sign toward the opposite shading hemisphere so NEE can
		//! reject it; full-sphere materials explicitly take the magnitude.
		inline Scalar RayFacingShadingCosine( const Vector3& direction ) const
		{
			const Scalar c = Vector3Ops::Dot( vNormal, direction );
			return Vector3Ops::Dot( vNormal, ray.Dir() ) > Scalar(0) ? -c : c;
		}

		//! THE shared recovery for the two flags above (DL-70).  Returns
		//! the TRUE, ray-independent, winding-order geometric normal: the
		//! reported `vGeomNormal` with the double-sided / back-face
		//! orient-to-ray flip undone, and the `bGeomNormalRayDerived`
		//! exception honoured (a fabricated, ray-derived normal has no
		//! flip to undo, so it is returned AS REPORTED -- i.e. still
		//! facing the ray).
		//!
		//! USE THIS, not a raw `vGeomNormal` read, for every
		//! entry-vs-exit / front-vs-back / which-side-of-the-real-surface
		//! test.  A raw read is deliberately always ray-facing on a
		//! double-sided hit, which collapses exactly the distinction such
		//! a test exists to make (DL-70 enumerated fourteen consumers that
		//! did this and went blind on double-sided geometry).
		//!
		//! NOT for "which way does this surface face the ray" -- that
		//! question wants the reported orientation and should keep reading
		//! `vGeomNormal` directly.  And NOT on its own where a hit may be
		//! HAIR: pair it with `HasTrueGeomSide()` and SKIP (or fall back)
		//! when that is false, because for a ray-derived normal this
		//! returns a ray-facing direction that is not a surface side at
		//! all.  See both flags' doc comments above for the contract, and
		//! docs/DL70_GEOM_NORMAL_ORIENTATION_SITES.md for the site table.
		inline Vector3 UnflippedGeomNormal() const
		{
			return ( bGeomNormalOrientedToRay && !bGeomNormalRayDerived )
				? -vGeomNormal : vGeomNormal;
		}

		//! Signed facing of the TRUE surface (`UnflippedGeomNormal()`)
		//! against an arbitrary direction `d`.  Negative = `d` travels
		//! INTO the surface (a true entry / front-face approach);
		//! positive = `d` travels out of it (a true exit / back-face
		//! approach).  Same caveats as `UnflippedGeomNormal()`.
		inline Scalar TrueGeomFacing( const Vector3& d ) const
		{
			return Vector3Ops::Dot( UnflippedGeomNormal(), d );
		}

		//! True when `UnflippedGeomNormal()` / `TrueGeomFacing()` answer a
		//! meaningful which-side-of-the-real-surface question -- i.e. when
		//! the reported geometric normal is a genuine surface property
		//! rather than a per-ray fabrication.  False only for
		//! `HairGeometry` today (see `bGeomNormalRayDerived`).
		inline bool HasTrueGeomSide() const
		{
			return !bGeomNormalRayDerived;
		}

		//! DL-96: the BSSRDF front-face admission test's own facing
		//! function.  Admits entry from the RAY-FACING (reported) side
		//! on an open sheet -- `bOpenSheet`, both faces legitimate -- and
		//! from the TRUE outward side (`TrueGeomFacing`) otherwise
		//! (closed-solid semantics, DL-70's original fix, unchanged).
		//! NOT a general-purpose replacement for `TrueGeomFacing` --
		//! scoped to this one gate; see `bOpenSheet`'s doc comment for
		//! why every other DL-70 consumer keeps `TrueGeomFacing`
		//! unconditionally.
		inline Scalar BSSRDFEntryFacing( const Vector3& d ) const
		{
			return bOpenSheet
				? Vector3Ops::Dot( vGeomNormal, d )
				: TrueGeomFacing( d );
		}

		Point2						ptCoord;		// primary texture mapping co-ordinates (TEXCOORD_0 from glTF)

		//! Secondary texture coordinates (TEXCOORD_1 from glTF; NOT the
		//! transform-overridden coord for KHR_texture_transform — that
		//! lives in the painter wrapper).  Populated only by triangle
		//! meshes that carry a TEXCOORD_1 array (loaded via glTF import).
		//! Consumers (the TexCoord1Painter wrapper, primarily) must check
		//! `bHasTexCoord1` before reading; when false, fall back to
		//! `ptCoord` (TEXCOORD_0).
		Point2						ptCoord1;
		bool						bHasTexCoord1;

		//! OUTPUT (DL-107): whether an overriding `IUVGenerator` has
		//! ALREADY supplied `ptCoord` for the surface this record
		//! reports -- set true the moment ANY `IUVGenerator::GenerateUV`
		//! call fires for this hit (`Object::IntersectRay`'s own
		//! override-generator block, mirrored at composite level by
		//! `CSGObject::IntersectRay`), false otherwise (explicitly, not
		//! just "left unset" -- `Object::IntersectRay` writes both
		//! branches so a farther, rejected BVH candidate's leftover
		//! `true` can never survive onto a later, generator-less hit
		//! that reuses the same shared record).
		//!
		//! WHY IT EXISTS.  A `CSGObject` composite may carry its own
		//! `pUVGenerator` in addition to (or instead of) one on either
		//! operand.  The operand's own generator, if it has one, must
		//! WIN (it is the more specific binding); the composite's is
		//! only the correct FALLBACK for an operand with none of its
		//! own.  This flag is how `CSGObject::IntersectRay` tells the
		//! two cases apart without a getter on `pUVGenerator` itself
		//! (there is none -- see `Object::SetUVGenerator`): it is
		//! carried by BOTH the whole-record `ri = riObjA` / `= riObjB`
		//! copy (via `operator=`, below) AND by
		//! `AdoptCsgSurfacePayload` (CSGObject.cpp), so it composes
		//! correctly through arbitrary CSG nesting depth for free, the
		//! same way `ptObjIntersec` and `ptCoord` themselves do.
		//!
		//! Precedence at any one CSGObject level: operand generator
		//! (this flag already true when the composite's own check runs)
		//! WINS over the composite generator (fires only when this flag
		//! is still false) WINS over the operand's native UV (no
		//! generator anywhere in the chain, flag stays false
		//! throughout).
		bool						bUVGeneratorApplied;

		Point3						ptIntersection;	// the point in world co-ordinates of the intersection, only
													// set if there was an intersection
		Point3						ptExit;			// point at which the ray exits the object

		Point3						ptObjIntersec;	// the point of intersection on object space
		Point3						ptObjExit;		// the point of exit in object space

		//! WORLD -> OBJECT linear map for the frame `ptObjIntersec` is
		//! expressed in, or `nullptr` when it is not known.
		//!
		//! WHY IT EXISTS.  A modifier that wants to evaluate a painter at
		//! an OFFSET point (ReliefModifier's central difference in the
		//! tangent plane, docs/RELIEF_MODIFIER_DESIGN.md 3.2/3.4) has to
		//! move every point domain a painter can read, consistently:
		//! `ptIntersection` by the world step, `ptCoord` by the UV chain
		//! rule -- and `ptObjIntersec` by the SAME step expressed in
		//! object space, or an object-space painter (`mapping_painter
		//! space object`, `voronoi3d space object`, `Po` in an
		//! expression) sees a field that is flat along the step.  The hit
		//! record carried no transform, so this is it.
		//!
		//! WHAT IT POINTS AT.  `Object::m_mxInvFinalTrans` -- the object's
		//! own finalized world->object matrix, which is exactly the
		//! inverse of the `m_mxFinalTrans` that produced `ptIntersection`
		//! from `ptObjIntersec` two lines below the stamp.  It is a
		//! borrowed pointer into the Object that also became `ri.pObject`,
		//! so it is valid for as long as the hit record is (the scene is
		//! immutable during a render, docs/ARCHITECTURE.md).
		//!
		//! USE THE LINEAR PART ONLY.  A step is a DIRECTION, so transform
		//! it with `Vector3Ops::Transform` (which drops the translation
		//! column), never `Point3Ops::Transform`.  Under a non-uniform
		//! scale the object-space step is not the world step's length --
		//! that is correct, not a defect: the painter's field lives in
		//! object space and the finite difference must span the object-
		//! space distance the world step actually covers.
		//!
		//! NULL ON CSG HITS, DELIBERATELY.  `CSGObject::IntersectRay`
		//! clears it.  A CSG composite reports the CHILD operand's own
		//! object-space point in `ptObjIntersec` (see
		//! `AdoptCsgSurfacePayload`, which copies it untransformed, and
		//! its comment naming the resulting frame mismatch as a
		//! pre-existing, deliberate gap), so the map from world to THAT
		//! frame is the product of the child's inverse with every
		//! enclosing composite's inverse -- a per-hit matrix no member
		//! holds and a pointer cannot express.  Stamping the composite's
		//! own inverse would be wrong by exactly the child's transform,
		//! and leaving the child's stamp would be wrong by exactly the
		//! composite's; `nullptr` is the honest third answer, and the
		//! consumer's documented degraded mode (move the object-space
		//! point by the WORLD step, warn once) is at least correct
		//! whenever the composite chain is a pure translation.
		//!
		//! NULL ALSO on any record a transport path builds without going
		//! through `Object::IntersectRay` (BDPT/VCM's
		//! `PopulateRIGFromVertex`, hand-built test hits, the geometry
		//! unit tests).  This is an OUTPUT field, so `PropagateCastInputs`
		//! does not carry it.
		const Matrix4*				pmxWorldToObject;

		OrthonormalBasis3D			onb;			// the orthonormal basis at the point of intersection

		//! Some custom intersection data that an object would
		//! want to pass to a shader, we don't really care what it is
		//! as long as it is reference counted properly.
		//! NOTE: this is a huge hack to get 3DS MAX shaders to 
		//! work with our shaders.  The correct thing to do here is
		//! to refactor the entire idea of RayIntersection and RayIntersectionGeometric
		//! into a ShaderContext, BSDFContext, PainterContext, so that they
		//! can ask for whatever information they want.
		IReference*					pCustom;

		Scalar						glossyFilterWidth;	///< Accumulated glossy filter blur from StabilityConfig (0 = off)

		//! Index of refraction of the AMBIENT (incident) medium at this hit —
		//! the medium the incoming ray was travelling through when it struck the
		//! surface.  Stamped by the integrator / ray caster from `IORStack::top()`
		//! at hit-production time.  DEFAULT 1.0 = air ⇒ byte-identical for every
		//! scene that renders in vacuum.  Consumed by the GGX conductor Fresnel
		//! (`GGXSPF::Scatter*` / `GGXBRDF::value*`) and its Kulla-Conty
		//! multiscatter average so a conductor viewed THROUGH a dielectric (e.g.
		//! silver under enamel glass) reflects for the surrounding medium's IOR
		//! rather than hardcoded air.  In the spectral (NM) path this is the
		//! per-wavelength n(λ) that `DielectricSPF` pushed, so it is
		//! dispersion-correct automatically.
		Scalar						ambientIOR;

		//! Surface derivatives at the hit point.  Populated by geometries
		//! that know them at intersection time (currently: triangle meshes).
		//! Other geometries leave this with valid=false; consumers should
		//! fall back to IGeometry::ComputeSurfaceDerivatives.
		SurfaceDerivativesInfo		derivatives;

		//! GEOMETRY-DERIVED SHADING SIGNALS, Phase 2 — the typed, `const`
		//! channel through which the expression VM's `occlusion(radius)` /
		//! `thickness(radius)` builtins reach the hit geometry
		//! (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md §6.1).  Stamped by
		//! geometries that can answer such queries (today: the SDF family)
		//! inside their own `IntersectRay`, in their own OBJECT space, and
		//! left untransformed by the layers above — both signals are
		//! dimensionless, so no frame conversion is needed or wanted.
		//!
		//! Default (no provider) is the honest "this surface publishes no
		//! signals": the builtins then return their documented neutral
		//! values.  This is the narrow, typed version of the PainterContext
		//! refactor `pCustom`'s comment above has been asking for — one
		//! channel, not the whole refactor.
		//!
		//! MIRRORED ONTO `BDPTVertex` (2026-09-11,
		//! docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §3), so a record
		//! `PathVertexEval::PopulateRIGFromVertex` rebuilds at a BDPT /
		//! VCM / MLT subpath vertex carries the stamp forward instead of
		//! reading the neutral — the bidirectional integrators used to
		//! sample against the live record and then price the same sample
		//! against a neutral one, which is a bias, not a flat mask.
		//!
		//! AND PROBED ONTO A SAMPLED EMISSION POINT (same date, §5 of that
		//! document): a light sample is a point `UniformRandomPoint`
		//! returned, not a hit, so `LightSampler::ProbeEmitterSurface`
		//! fires one closest-hit at the luminary and stamps THIS field
		//! from the resulting record — including the cross-object triple,
		//! with exactly the values `ObjectManager::IntersectRay` uses. It
		//! is the one place other than that function where the triple is
		//! written, and SourceHygieneTest pins the pair.
		SurfaceSignalInfo			signals;

		//! Texture-space footprint at the hit point — Landing 2.  Computed
		//! at intersection time by projecting the incoming ray's screen-
		//! space differentials onto the surface UV plane via dpdu / dpdv.
		//! valid=true iff (a) the hit geometry populates derivatives AND
		//! (b) the incoming ray has hasDifferentials = true.  Consumed by
		//! TexturePainter to compute mip LOD; absence falls back to
		//! base-level sampling (today's behaviour).
		TextureFootprint			txFootprint;

		//! Per-vertex color interpolated at the hit point.  Populated only
		//! by triangle meshes that carry a vertex-color array (loaded from
		//! PLY, RAW2, etc.).  Consumers (the VertexColorPainter, primarily)
		//! must check `bHasVertexColor` before reading `vColor` — when
		//! false, the painter falls back to its configured default color.
		RISEPel						vColor;
		bool						bHasVertexColor;

		//! Per-vertex tangent interpolated at the hit point and transformed
		//! to world space (via Object's forward transform; tangents
		//! transform like positions, not like normals).  Populated only by
		//! triangle meshes that carry a TANGENT array (loaded from glTF;
		//! see `ITriangleMeshGeometryIndexed3` / Tangent4 in Polygon.h).
		//! Consumers (the NormalMap modifier, primarily) must check
		//! `bHasTangent` before reading `vTangent` / `bitangentSign` —
		//! when false, fall back to a tangent derived from the
		//! orthonormal basis or surface derivatives.
		Vector3						vTangent;
		Scalar						bitangentSign;	// +1 or -1; bitangent = sign * cross(vNormal, vTangent)
		bool						bHasTangent;

		//! Set by a geometry that wants the shading ONB's tangent (u-axis)
		//! built from a COHERENT, geometry-defined direction rather than
		//! the arbitrary tangent `OrthonormalBasis3D::CreateFromW` picks
		//! from a canonical axis.  Currently set only by `SDFGeometry` in
		//! heightfield mode, whose linear UV = ((x+R)/2R,(y+R)/2R) matches
		//! the `cartesian_disk` displaced mesh: both want u ∝ world-X
		//! projected into the shading-normal plane so a shared anisotropic
		//! `tangent_rotation` (the groove-direction flash) rotates from the
		//! same base tangent on both realizations.  Object::IntersectRay
		//! honours this AFTER the normal is in world space (see the
		//! `CreateFromWU` branch there); when false the onb is built with
		//! `CreateFromW` exactly as before, so every other geometry is
		//! byte-identical.
		bool						bShadingTangentFromGeometry;

		//! Written by curve geometry (HairGeometry) alongside
		//! bShadingTangentFromGeometry=true: the OBJECT-space fiber
		//! tangent at the hit -- like `vNormal` and `vTangent`, a
		//! geometry writes this in its own object space; it is
		//! `Object::IntersectRay` (and, for a CSG-composed hair fibre,
		//! `CSGObject::IntersectRay`) that lifts it to world space
		//! (forward matrix, like `vTangent` -- a tangent transforms like
		//! a position, NOT inverse-transpose), projects it into the
		//! world-space shading-normal plane, and builds the ONB from it
		//! (falling back to the legacy world-X projection if the
		//! supplied tangent is degenerate, i.e. near-parallel to the
		//! normal).  Same convention as `vTangent` above: each level
		//! OVERWRITES this field IN PLACE with its own promoted value
		//! (one promotion per level of nesting) rather than leaving the
		//! object-space original untouched, so a CSG-of-CSG composite
		//! sees the field arrive one promotion short of world space at
		//! each level and finishes the job itself -- exactly the
		//! invariant `vTangent`'s write-back establishes.  A singular
		//! transform (the promoted tangent itself collapses to
		//! near-zero, not merely its projection) clears
		//! `bHasShadingTangent` and skips the write-back instead of
		//! leaving a "valid" flag paired with a garbage vector.  A
		//! geometry that sets `bShadingTangentFromGeometry` WITHOUT also
		//! setting `bHasShadingTangent` (the SDFGeometry heightfield
		//! case) still gets the legacy world-X projection, byte-identical
		//! to before this field existed.  Consumers downstream of
		//! intersection (e.g. `HairBSDF`) therefore always see this field
		//! in world space, never object space.
		Vector3						vShadingTangent;
		bool						bHasShadingTangent;

		//! Wireframe view-mode edge info (GUI render modes P1,
		//! docs/gui/RENDER_MODES.md).  INPUT: `bWantsWireEdgeInfo` is
		//! stamped onto the record by the ray caster BEFORE the
		//! intersection (only the interactive wireframe view-mode
		//! caster sets it), gating the extra per-hit closest-edge
		//! computation so production renders pay nothing for the
		//! feature.  OUTPUT: triangle-mesh intersectors that honour
		//! the request store the closest point on the hit triangle's
		//! nearest EDGE in `ptWireNearestEdge` (object space at stamp
		//! time; Object::IntersectRay transforms it to world space
		//! exactly like `ptIntersection`, so the world-space distance
		//! |ptIntersection - ptWireNearestEdge| is exact under any
		//! affine transform, including non-uniform scale) and set
		//! `bHasWireEdgeInfo`.  Geometries without polygon edges
		//! (analytical primitives, SDFs) leave it false -- the
		//! wireframe shader falls back to facet shading there,
		//! honestly drawing no lines.
		Point3						ptWireNearestEdge;
		bool						bHasWireEdgeInfo;
		bool						bWantsWireEdgeInfo;

		//! Copies the per-cast INPUT fields -- values stamped by the
		//! integrator/caster BEFORE intersection and consumed AFTER --
		//! onto a freshly-constructed scratch record.  Traversal and
		//! composition code that builds a per-candidate record (top-level
		//! BVH leaf, octree/BSP branch merges, CSG children) MUST call
		//! this before dispatching: the whole-struct copy-back on a hit
		//! otherwise silently clobbers the inputs (the glossyFilterWidth
		//! bug, 2026-07-16 -- StabilityConfig's glossy filter was dead on
		//! those paths).  Add future per-cast inputs HERE, not at the
		//! ~27 call sites.
		inline void PropagateCastInputs( const RayIntersectionGeometric& src )
		{
			glossyFilterWidth = src.glossyFilterWidth;
			bWantsWireEdgeInfo = src.bWantsWireEdgeInfo;
		}

		RayIntersectionGeometric( const Ray& ray_, const RasterizerState& rast_ ) :
		  ray( ray_ ),
		  rast( rast_ ),
		  bHit( false ),
		  range( RISE_INFINITY ),
		  range2( RISE_INFINITY ),
		  bGeomNormalOrientedToRay( false ),
		  bGeomNormalRayDerived( false ),
		  bOpenSheet( false ),
		  bProvablyNoInterior( false ),
		  bHasTexCoord1( false ),
		  bUVGeneratorApplied( false ),
		  pmxWorldToObject( 0 ),
		  pCustom( 0 ),
		  glossyFilterWidth( 0 ),
		  ambientIOR( 1.0 ),
		  bHasVertexColor( false ),
		  bitangentSign( 1.0 ),
		  bHasTangent( false ),
		  bShadingTangentFromGeometry( false ),
		  bHasShadingTangent( false ),
		  bHasWireEdgeInfo( false ),
		  bWantsWireEdgeInfo( false )
		{}

		~RayIntersectionGeometric( )
		{
			safe_release( pCustom );
		}

		RayIntersectionGeometric( const RayIntersectionGeometric& r ) :
		  ray( r.ray ),
		  rast( r.rast ),
		  bHit( r.bHit ),
		  range( r.range ),
		  range2( r.range2 ),
		  vNormal( r.vNormal ),
		  vNormal2( r.vNormal2 ),
		  vGeomNormal( r.vGeomNormal ),
		  vGeomNormal2( r.vGeomNormal2 ),
		  bGeomNormalOrientedToRay( r.bGeomNormalOrientedToRay ),
		  bGeomNormalRayDerived( r.bGeomNormalRayDerived ),
		  bOpenSheet( r.bOpenSheet ),
		  bProvablyNoInterior( r.bProvablyNoInterior ),
		  ptCoord( r.ptCoord ),
		  ptCoord1( r.ptCoord1 ),
		  bHasTexCoord1( r.bHasTexCoord1 ),
		  bUVGeneratorApplied( r.bUVGeneratorApplied ),
		  ptIntersection( r.ptIntersection ),
		  ptExit( r.ptExit ),
		  ptObjIntersec( r.ptObjIntersec ),
		  ptObjExit( r.ptObjExit ),
		  pmxWorldToObject( r.pmxWorldToObject ),
		  onb( r.onb ),
		  pCustom( r.pCustom ),
		  glossyFilterWidth( r.glossyFilterWidth ),
		  ambientIOR( r.ambientIOR ),
		  derivatives( r.derivatives ),
		  signals( r.signals ),
		  txFootprint( r.txFootprint ),
		  vColor( r.vColor ),
		  bHasVertexColor( r.bHasVertexColor ),
		  vTangent( r.vTangent ),
		  bitangentSign( r.bitangentSign ),
		  bHasTangent( r.bHasTangent ),
		  bShadingTangentFromGeometry( r.bShadingTangentFromGeometry ),
		  vShadingTangent( r.vShadingTangent ),
		  bHasShadingTangent( r.bHasShadingTangent ),
		  ptWireNearestEdge( r.ptWireNearestEdge ),
		  bHasWireEdgeInfo( r.bHasWireEdgeInfo ),
		  bWantsWireEdgeInfo( r.bWantsWireEdgeInfo )
		{
			if( pCustom ) {
				pCustom->addref();
			}
		}

		inline RayIntersectionGeometric& operator=( const RayIntersectionGeometric& r )
		{
			rast = r.rast;
			ray = r.ray;
			bHit = r.bHit;
			range = r.range;
			range2 = r.range2;
			vNormal = r.vNormal;
			vNormal2 = r.vNormal2;
			vGeomNormal = r.vGeomNormal;
			vGeomNormal2 = r.vGeomNormal2;
			bGeomNormalOrientedToRay = r.bGeomNormalOrientedToRay;
			bGeomNormalRayDerived = r.bGeomNormalRayDerived;
			bOpenSheet = r.bOpenSheet;
			bProvablyNoInterior = r.bProvablyNoInterior;
			ptCoord = r.ptCoord;
			ptCoord1 = r.ptCoord1;
			bHasTexCoord1 = r.bHasTexCoord1;
			bUVGeneratorApplied = r.bUVGeneratorApplied;
			derivatives = r.derivatives;
			signals = r.signals;
			txFootprint = r.txFootprint;
			ptIntersection = r.ptIntersection;
			ptExit = r.ptExit;
			ptObjIntersec = r.ptObjIntersec;
			ptObjExit = r.ptObjExit;
			pmxWorldToObject = r.pmxWorldToObject;
			onb = r.onb;
			glossyFilterWidth = r.glossyFilterWidth;
			ambientIOR = r.ambientIOR;
			vColor = r.vColor;
			bHasVertexColor = r.bHasVertexColor;
			vTangent = r.vTangent;
			bitangentSign = r.bitangentSign;
			bHasTangent = r.bHasTangent;
			bShadingTangentFromGeometry = r.bShadingTangentFromGeometry;
			vShadingTangent = r.vShadingTangent;
			bHasShadingTangent = r.bHasShadingTangent;
			ptWireNearestEdge = r.ptWireNearestEdge;
			bHasWireEdgeInfo = r.bHasWireEdgeInfo;
			bWantsWireEdgeInfo = r.bWantsWireEdgeInfo;

			safe_release( pCustom );
			pCustom = r.pCustom;

			if( pCustom ) {
				pCustom->addref();
			}

			return *this;
		}
	};
}

#endif
