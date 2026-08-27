//////////////////////////////////////////////////////////////////////
//
//  ProceduralDescriptors.h - Plain public parameter blocks for the
//  procedural construction factories (profile sweep, along-path
//  instances).  Lives in Interfaces so IJob and RISE_API can share
//  the types without touching Implementation headers.
//
//  These mirror the retired Python bakers' argparse surfaces; the
//  defaults are the bakers' defaults, and the per-pattern overrides
//  (gen_dials.sh's blessed parameter sets) live in the scene chunks.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PROCEDURAL_DESCRIPTORS_
#define PROCEDURAL_DESCRIPTORS_

namespace RISE
{
	class IGeometry;
	class IPainter;
	class IScalarPainter;

	//! General profile-sweep parameters: an arbitrary CLOSED 2D profile
	//! polygon swept along an arbitrary 3D Catmull-Rom path with
	//! rotation-minimizing frames.  Tubes, rails, mouldings, bands, cables.
	//! The scene chunk supplies the profile as repeatable
	//! `profile_point <x> <h>` lines (x = local binormal/width axis,
	//! h = local normal/height axis) and the path as repeatable
	//! `point <x> <y> <z>` lines.
	//!
	//! It is also the LOFT (doc 89 slice A): the swept section need not be
	//! circular and need not keep one shape.  pointScales / pointScalesY
	//! scale the two profile axes independently per station (a torso, a
	//! fin, a hull), and profile2Points + pointMorphs interpolate the
	//! section's OUTLINE from one profile to another along the path (a
	//! snout, a round-to-square leg).  Order of operations: MORPH first, in
	//! the profile plane, then the anisotropic scale, then pointWidths (x
	//! only) and the linear end_scale taper -- all multiplicative.  With
	//! none of those set the bake is byte-identical to the pre-loft sweep.
	struct SweepDescriptor
	{
		const double* profilePoints;	//!< x0 h0 x1 h1 ... (pairs; CLOSED polygon, >= 3)
		unsigned int  numProfilePoints;	//!< number of (x, h) PAIRS
		const double* pathPoints;		//!< x0 y0 z0 x1 y1 z1 ... (triples)
		unsigned int  numPathPoints;	//!< number of (x, y, z) TRIPLES (>= 2)
		int    nLen;					//!< samples along the path
		double endScaleX;				//!< profile x scale at the path end (linear taper from 1)
		double endScaleY;				//!< profile h scale at the path end (linear taper from 1)
		bool   capStart;				//!< triangulate the start cross-section closed
		bool   capEnd;					//!< triangulate the end cross-section closed
		double frameHintX, frameHintY, frameHintZ;	//!< initial binormal hint (0,0,0 = auto: world axis most perpendicular to the start tangent)
		const double* pointWidths;	//!< OPTIONAL per-control-point width (x/binormal axis) multipliers, one per path point (count <= numPathPoints; missing padded with 1.0).  Catmull-Rom interpolated onto the path samples and composed MULTIPLICATIVELY with the linear end_scale_x taper.  NULL/0 = uniform width (byte-identical to the linear-taper-only sweep)
		unsigned int  numPointWidths;	//!< number of pointWidths entries (0 = per-station width OFF)
		const double* pointScales;	//!< OPTIONAL per-control-point scale multipliers on the profile's X (binormal) axis, one per path point (count <= numPathPoints; missing padded with 1.0).  Catmull-Rom interpolated onto the path samples with the SAME sampler as pointWidths, and composed MULTIPLICATIVELY with pointWidths (x only) and the linear end_scale taper.  When pointScalesY is NULL/0 this track drives BOTH profile axes (the historical UNIFORM one-argument `point_scale` meaning -- a ROUND varying radius); supply pointScalesY for the ANISOTROPIC two-argument form (`point_scale sx sy`).  NULL/0 = uniform scale (byte-identical to a sweep without it)
		unsigned int  numPointScales;	//!< number of pointScales entries (0 = per-station scale OFF)
		const double* pointScalesY;	//!< OPTIONAL per-control-point scale multipliers on the profile's Y (frame-normal) axis -- the second half of the ANISOTROPIC two-argument form (`point_scale sx sy`).  Same count rules and sampler as pointScales; must be supplied ALONGSIDE pointScales (numPointScalesY, when non-zero, must equal numPointScales).  NULL/0 = pointScales drives both axes uniformly, exactly as before
		unsigned int  numPointScalesY;	//!< number of pointScalesY entries (0 = pointScales is UNIFORM over both axes)
		const double* profile2Points;	//!< OPTIONAL SECOND closed 2D profile polygon (x0 h0 x1 h1 ..., >= 3 pairs, same CCW-is-outward convention as profilePoints).  Present => the swept section MORPHS profilePoints -> profile2Points across the path (per-station blend factor from pointMorphs, or a linear 0->1 ramp when pointMorphs is empty).  Both profiles are resampled onto the UNION of their own normalized arc-length parameters (so every authored vertex of BOTH survives exactly; max(n1,n2) <= N <= n1+n2), profile2 is index-rotated to the twist-free alignment, and the blend runs on the CENTRED point lists anchored at profilePoints' own VERTEX MEAN -- so an off-centre profile2 never TRANSLATES the section.  NULL/0 = the historical fixed-shape sweep
		unsigned int  numProfile2Points;	//!< number of profile2Points (x, h) PAIRS (0 = morph OFF)
		const double* pointMorphs;	//!< OPTIONAL per-control-point morph blend factors t in [0, 1] (0 = profilePoints, 1 = profile2Points), one per path point (count <= numPathPoints; missing padded with 1.0 -- the far profile, so a short track still completes the morph).  Catmull-Rom interpolated onto the path samples with the SAME sampler as pointScales, then CLAMPED to [0, 1] (spline overshoot between non-monotone controls would extrapolate past both profiles).  Requires profile2Points.  NULL/0 with profile2Points present = the default linear 0->1 ramp along the stations -- which is REFUSED when pathClosed is TRUE, since a 0->1 ramp is discontinuous across the loop's seam (a closed loop must author EXPLICIT values, which sample periodically)
		unsigned int  numPointMorphs;	//!< number of pointMorphs entries (0 = default ramp when profile2 is present)
		bool   pathClosed;				//!< sweep a CLOSED loop: periodic Catmull-Rom path sampling (segments wrap point[n-1]->point[0]), a periodic rotation-minimizing frame with a holonomy correction removing the seam twist, and cyclic ring stitching (no end caps).  Requires numPathPoints >= 3 and the first/last point NOT authored coincident.  end_scale_x/end_scale_y must stay 1.0 (point_scale IS allowed -- it samples periodically like point_width).  Default FALSE = the existing open-path behaviour, byte-identical

		SweepDescriptor() :
			profilePoints( 0 ), numProfilePoints( 0 ),
			pathPoints( 0 ), numPathPoints( 0 ),
			nLen( 64 ),
			endScaleX( 1.0 ), endScaleY( 1.0 ),
			capStart( true ), capEnd( true ),
			frameHintX( 0.0 ), frameHintY( 0.0 ), frameHintZ( 0.0 ),
			pointWidths( 0 ), numPointWidths( 0 ),
			pointScales( 0 ), numPointScales( 0 ),
			pointScalesY( 0 ), numPointScalesY( 0 ),
			profile2Points( 0 ), numProfile2Points( 0 ),
			pointMorphs( 0 ), numPointMorphs( 0 ),
			pathClosed( false )
		{
		}
	};

	//! General along-path instancing: a named TEMPLATE geometry (tessellated
	//! once through the universal TessellateToMesh contract) stamped along a
	//! 3D Catmull-Rom path at arc-length pitch.  Fence posts, rivets, beads,
	//! stitches, chain links.  The template's local +Y aligns with the path
	//! tangent (rotated by slant about the frame normal), +Z with the frame
	//! normal, +X with the binormal.
	struct PathInstancesDescriptor
	{
		const IGeometry* pGeometry;		//!< resolved template geometry (NOT addref'd here; consumed synchronously)
		const double* pathPoints;		//!< x0 y0 z0 ... (triples, >= 2)
		unsigned int  numPathPoints;	//!< number of (x, y, z) TRIPLES
		int    nLen;					//!< arc-length walk resolution (path samples)
		double pitch;					//!< arc-length distance between instances (> 0)
		double phase;					//!< arc-length distance before the first instance (< 0 = pitch/2, the centred default)
		double slantDeg;				//!< rotation of the template about the frame normal (degrees)
		double scale;					//!< uniform template scale
		unsigned int detail;			//!< TessellateToMesh detail for the template (clamped 3..256)
		double frameHintX, frameHintY, frameHintZ;	//!< initial binormal hint (0,0,0 = auto), as in SweepDescriptor

		PathInstancesDescriptor() :
			pGeometry( 0 ),
			pathPoints( 0 ), numPathPoints( 0 ),
			nLen( 256 ),
			pitch( 1.0 ), phase( -1.0 ), slantDeg( 0.0 ), scale( 1.0 ),
			detail( 16 ),
			frameHintX( 0.0 ), frameHintY( 0.0 ), frameHintZ( 0.0 )
		{
		}
	};

	//! Surface-of-revolution (lathe) parameters: an OPEN 2D profile
	//! polyline authored in the half-plane (r = radius from the axis,
	//! h = height along it) revolved about a world axis.  Vases, bottles,
	//! turned table/chair legs, lamp bases, pedestals, knobs, goblets --
	//! the furniture-and-vessel vocabulary.  A silhouette is the single
	//! most reliable thing to author by hand (or to generate): only the
	//! outline is described, and the revolution supplies the solid.
	//!
	//! Distinct from SweepDescriptor: a sweep moves a CLOSED profile
	//! polygon along an arbitrary 3D path; a lathe spins an OPEN profile
	//! polyline about a fixed straight axis.  A profile point with r == 0
	//! sits ON the axis, so its ring collapses to a single POLE vertex
	//! (a triangle fan, no degenerate quad band) -- which is what makes a
	//! vase closed at both ends watertight with no caps at all.  An
	//! INTERIOR pole (a waisted profile pinched to r == 0 mid-way) gets one
	//! pole vertex per adjacent band, since the two sides of a pinch demand
	//! opposite axial normals.  The baked mesh is DOUBLE-SIDED, so an
	//! emissive material on a lathe radiates from both faces.
	//!
	//! CONSEQUENCE of that split, worth knowing before reaching for a fix:
	//! displaced_geometry over a PINCHED lathe TEARS at the waist.  The two
	//! pinch poles are coincident but carry exactly opposite normals, so
	//! displacement pushes them apart by 2 * disp_scale and opens a crack.
	//! This is inherent to split normals -- sweep_geometry's duplicate-a-
	//! profile-point hard-edge idiom has it too -- and sharing one vertex
	//! instead would trade the crack for a shading normal in the wrong
	//! half-space on one of the two bands, which is the very defect the
	//! split exists to prevent.  Displace an UNPINCHED profile, or keep
	//! disp_scale small relative to the waist, rather than "fixing" it.
	struct LatheDescriptor
	{
		const double* profilePoints;	//!< r0 h0 r1 h1 ... (pairs; OPEN polyline, >= 2, every r >= 0, not ALL r == 0)
		unsigned int  numProfilePoints;	//!< number of (r, h) PAIRS
		int    axis;					//!< revolution axis: 0 = world X, 1 = world Y (the default, matching cylinder_geometry and the SDF roundcone/capsule local-Y convention), 2 = world Z
		double sweepDegrees;			//!< angular extent of the revolution, in (0, 360].  360 closes the surface on itself (the last radial column IS the first -- no duplicated seam); anything less is a section/cutaway and gets two flat radial caps
		int    nRadial;					//!< radial SEGMENTS around the axis (clamped 3..2048).  A full 360 sweep emits nRadial columns (wrapping); a partial sweep emits nRadial + 1
		bool   smooth;					//!< TRUE: one row per profile point carrying the AVERAGE of its two adjacent segment normals (a turned surface reads smooth; duplicate a profile point to harden one edge).  FALSE: two rows per profile segment carrying that segment's flat normal (fully faceted)

		LatheDescriptor() :
			profilePoints( 0 ), numProfilePoints( 0 ),
			axis( 1 ), sweepDegrees( 360.0 ), nRadial( 48 ), smooth( true )
		{
		}
	};

	//! SKIN (doc 89 slice B): the ruled / billowed OPEN SHEET spanning two
	//! boundary polylines.  Wings, fins, webbing, sails, leaves, awnings,
	//! lampshade panels, tarps -- "a thin surface bounded by curves", the
	//! one shape class neither a sweep (a closed section along a path) nor
	//! a lathe (a silhouette about an axis) can state at all.
	//!
	//! Both rails are resampled onto the UNION of their own normalized
	//! arc-length parameter sets, so EVERY authored vertex of BOTH rails
	//! is reproduced VERBATIM in the mesh (a kink authored on one rail is
	//! a kink in the surface, not a chamfer).  nLen only ever ADDS
	//! stations to that union; it never replaces it.  The one exception
	//! is a station where the two rails come within 1e-9 of the sheet's
	//! own extent of each other: there rail B's point is snapped onto
	//! rail A's so the tip collapses to a single shared vertex (the
	//! lathe's pole, and its axis snap, applied to a sheet) instead of a
	//! skirt of sub-degenerate slivers.
	//!
	//! The base surface is RULED: station i spans the straight segment
	//! from railA(i) to railB(i), so v = 0 is rail A and v = 1 is rail B.
	//! `billow` then inflates the interior along the RULED sheet's own
	//! per-vertex normal, with a sin^2(pi*v) falloff that is exactly zero
	//! AND tangent at both rails -- so the rails themselves are never
	//! moved, whatever the billow.  A billow large enough to fold the
	//! sheet through itself at an authored crease is built as authored
	//! and WARNED about (count of folded faces, first offending station,
	//! and the remedy); it is neither refused nor clamped, because the
	//! fold bound depends on the rails' own crease angle and a clamp
	//! would silently change authored shapes.
	//!
	//! WHERE THE TWO RAILS MEET, the station collapses to a pole rather
	//! than emitting nAcross coincident copies, so the tip is a connected
	//! fan.  An END meeting (a leaf tip, a pinched sail corner) is ONE
	//! vertex carrying the mean of its single fan's normals.  An INTERIOR
	//! meeting -- rails that touch or CROSS mid-span -- is TWO coincident
	//! poles, one per side, each carrying its own fan's normals: the two
	//! sides of a pinch are different surfaces (opposed normals outright,
	//! in the crossing case), so no single normal represents both.  The
	//! sheet is then two lobes touching at a point, its boundary is still
	//! exactly its perimeter, and the author is WARNED, since a mid-span
	//! meeting is as often a mistake as an intent.  Two ADJACENT pinched
	//! stations leave a GAP (every quad between them is degenerate) and
	//! are warned about separately.
	//!
	//! The bake is ONE double-sided sheet, not a closed slab: RISE's
	//! double-sided meshes re-orient both the shading and the geometric
	//! normal toward the incoming ray, so each face shades correctly from
	//! ITS OWN side and the rail order never creates a black side.  An
	//! opaque membrane lit only from the far side is dark either way -- a
	//! slab would not change that -- so thickness buys nothing a
	//! transmitting material does not buy better.
	struct SkinDescriptor
	{
		const double* railAPoints;		//!< x0 y0 z0 x1 y1 z1 ... (triples; OPEN polyline, >= 2 points).  v = 0 boundary
		unsigned int  numRailAPoints;	//!< number of rail-A (x, y, z) TRIPLES
		const double* railBPoints;		//!< x0 y0 z0 ... (triples; OPEN polyline, >= 2 points).  v = 1 boundary.  The count NEED NOT match rail A
		unsigned int  numRailBPoints;	//!< number of rail-B (x, y, z) TRIPLES
		int    nLen;					//!< REQUESTED stations ALONG the rails (clamped 2..4096).  A MINIMUM, not an exact count: the station set is the union of both rails' authored arc-length parameters plus however many of the nLen uniform parameters do not coincide with one, so the actual count is >= max(nLen, |union|)
		int    nAcross;					//!< vertex ROWS ACROSS the sheet, rail A to rail B (clamped 2..1024).  2 = the two rails alone (no interior, so `billow` has nothing to displace)
		double billow;					//!< inflation of the sheet's INTERIOR, as a fraction of each station's own rail-to-rail span, along the ruled sheet's per-vertex normal (normalize(dP/du x dP/dv), with u along the rails and v from rail A to rail B).  Falloff sin^2(pi*v): exactly 0 and tangent at BOTH rails.  POSITIVE billows toward +normal, NEGATIVE toward -normal.  0 = the flat ruled surface.  A LARGE billow across a SHARP authored crease pinches at the crease -- inflating a folded sheet folds it further; that is the surface, not an artifact, so it is built as authored, but the fold INVERTS the geometric normal every side test reads on the affected faces and is reported as a warning naming the count and the first station.  The fold test is PER CORNER (the shading normal opposes the face's geometric normal at some vertex, which is exactly what a ray hitting near that vertex is handed), not against the three-normal sum -- a collapsed pole ships one normal for a whole fan, so a summed test cannot see a fan with only SOME lobes folded.  The same test runs at billow 0, where a fold can only be the RAILS' own (they cross or double back) and the warning says so instead of blaming billow

		SkinDescriptor() :
			railAPoints( 0 ), numRailAPoints( 0 ),
			railBPoints( 0 ), numRailBPoints( 0 ),
			nLen( 32 ), nAcross( 8 ), billow( 0.0 )
		{
		}
	};

	//! The NUMERIC half of a `hair_geometry` groom recipe -- everything
	//! that is a plain number rather than a reference to another scene
	//! chunk.  Shared verbatim by `HairGroomDescriptor` (the by-NAME form
	//! IJob takes) and `HairGroomRecipe` (the resolved-POINTER form
	//! RISE_API and the generator take), so the two cannot drift.
	//!
	//! LENGTH UNITS.  `length`, `widthRoot`, `widthTip`, `clumpSize` and
	//! `curlRadius` / `curlStep` are all in SCENE UNITS (metres in a
	//! default scene, per `scene_options scene_unit`).  The defaults are
	//! human-scalp numbers: 0.1 mm at the root tapering to 0.03 mm at the
	//! tip.
	struct HairGroomParams
	{
		unsigned int count;			//!< strand budget, PRE-density-mask (the density painter only ever REMOVES candidates, so the realised strand count is <= this)
		unsigned int segments;		//!< control points per strand (>= 2; 2 is a perfectly straight quill)
		unsigned int seed;			//!< every random draw in the generator derives from this; the same recipe + seed always yields byte-identical strands
		unsigned int baseDetail;	//!< tessellation detail handed to the base geometry's TessellateToMesh (roots are sampled on THAT mesh, so a coarse value quantises where hair can grow)
		double length;				//!< nominal strand length in scene units, scaled per-strand by the `length_painter`
		double widthRoot;			//!< FULL fibre width (not radius) at the root
		double widthTip;			//!< FULL fibre width at the tip; linearly interpolated in arc-length fraction between the two
		double gravity;				//!< downward (world -Y) droop at the tip as a fraction of the strand's own length; 0 = no droop.  Quadratic in the tip fraction, so the root stays normal-aligned
		double frizz;				//!< per-control-point random jitter amplitude as a fraction of the strand's own inter-control-point spacing; 0 = perfectly smooth
		double clump;				//!< clump attraction strength in [0,1]: how far a strand's tip is pulled toward its clump centre's tip.  0 = no clumping (also disabled when clumpSize <= 0)
		double clumpSize;			//!< clump cell size in SCENE UNITS -- strands whose roots share a cell of this side clump together.  0 = no clumping
		double curlRadius;			//!< helix radius in scene units; 0 = no curl
		double curlStep;			//!< helix pitch (arc length per full turn) in scene units; must be > 0 for curl to apply

		HairGroomParams() :
			count( 0 ), segments( 8 ), seed( 1 ), baseDetail( 32 ),
			length( 0.0 ), widthRoot( 0.0001 ), widthTip( 0.00003 ),
			gravity( 0.0 ), frizz( 0.0 ),
			clump( 0.0 ), clumpSize( 0.0 ),
			curlRadius( 0.0 ), curlStep( 0.0 )
		{
		}
	};

	//! An AUTHORED GUIDE-STRAND SET -- the `hair_guides` scene chunk in
	//! flat, STL-free form (Phase 2, docs/HAIR_FUR_DESIGN.md section 5.3).
	//!
	//! A guide is a plain open polyline in the SAME space the base
	//! geometry lives in.  Guides carry SHAPE ONLY: they never place a
	//! strand (`count` + the density mask still own placement) and their
	//! own point counts never decide a strand's `segments` -- each guide is
	//! resampled by ARC-LENGTH FRACTION onto whatever control-point count
	//! the groom asked for.
	//!
	//! STORAGE IS FLAT so this header stays dependency-free (the
	//! SkinDescriptor convention): every guide's (x, y, z) triples are
	//! concatenated into `points`, and `pointCounts[i]` says how many
	//! POINTS (not doubles) guide `i` contributes.  The caller owns both
	//! arrays; `IJob::AddHairGuides` copies out of them.
	struct HairGuidesDescriptor
	{
		const double*       points;			//!< x0 y0 z0 x1 y1 z1 ... -- EVERY guide's points concatenated, in guide order
		const unsigned int* pointCounts;	//!< pointCounts[i] = number of POINTS in guide i (each >= 2); the array has `numGuides` entries and must sum to points/3
		unsigned int        numGuides;		//!< number of guides (>= 1)

		HairGuidesDescriptor() :
			points( 0 ), pointCounts( 0 ), numGuides( 0 )
		{
		}
	};

	//! A `hair_geometry` groom recipe in BY-NAME form: what the scene
	//! chunk parsed, before any manager lookup.  `IJob::AddHairGeometry`
	//! resolves the five names into the pointers of `HairGroomRecipe`.
	//! An unbound optional reference is spelled NULL, "" or "none".
	struct HairGroomDescriptor
	{
		const char* baseGeometry;	//!< REQUIRED: name of a previously-registered geometry to grow hair on.  It must be tessellatable
		const char* density;		//!< optional IScalarPainter name: [0,1] rejection mask evaluated at each candidate root
		const char* lengthPainter;	//!< optional IScalarPainter name: per-root multiplier on `length`
		const char* comb;			//!< optional IPainter name: RGB-encoded tangent-space comb direction at each root
		const char* guides;			//!< optional `hair_guides` name: the strand SHAPE is interpolated from the 3 nearest guides instead of growing straight along the normal
		HairGroomParams p;

		HairGroomDescriptor() :
			baseGeometry( 0 ), density( 0 ), lengthPainter( 0 ), comb( 0 ), guides( 0 ), p()
		{
		}
	};

	//! A `hair_geometry` groom recipe in RESOLVED form: the pointers the
	//! generator actually evaluates.  `HairGeometry`'s deferred-groom
	//! constructor takes one of these, addrefs every non-null pointer,
	//! and holds it until `Realize()` runs the generation.
	//!
	//! Ownership: the CALLER's references are its own -- the constructor
	//! takes its OWN addref on each pointer, so the caller still releases
	//! whatever it resolved.  Same convention as every other RISE
	//! composite (DisplacedGeometry's base + displacement).
	struct HairGroomRecipe
	{
		IGeometry*            pBase;		//!< REQUIRED, non-null, and CanTessellate()
		const IScalarPainter* pDensity;		//!< optional
		const IScalarPainter* pLengthScale;	//!< optional
		const IPainter*       pComb;		//!< optional
		HairGroomParams       p;

		//! Optional GUIDE SET, in the flat form of `HairGuidesDescriptor`
		//! (all three fields are set together or all left null/zero).
		//!
		//! UNLIKE THE POINTERS ABOVE, THESE ARE NOT REFCOUNTED -- they are
		//! borrowed arrays.  `HairGeometry`'s deferred-groom constructor
		//! therefore takes its own DEEP COPY of both arrays and repoints
		//! its stored recipe at that copy, exactly as the pointers above
		//! get their own addref: a groom must not outlive-dangle on
		//! whatever vector the caller happened to build the recipe from.
		const double*         guidePoints;		//!< x y z triples, every guide concatenated
		const unsigned int*   guidePointCounts;	//!< POINTS per guide; `numGuides` entries
		unsigned int          numGuides;		//!< 0 = no guides bound (grow straight along the normal)

		HairGroomRecipe() :
			pBase( 0 ), pDensity( 0 ), pLengthScale( 0 ), pComb( 0 ), p(),
			guidePoints( 0 ), guidePointCounts( 0 ), numGuides( 0 )
		{
		}
	};
}

#endif
