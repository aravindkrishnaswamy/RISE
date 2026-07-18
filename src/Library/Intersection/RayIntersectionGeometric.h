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

		SurfaceDerivativesInfo() :
		dpdu( Vector3(0,0,0) ), dpdv( Vector3(0,0,0) ),
		dndu( Vector3(0,0,0) ), dndv( Vector3(0,0,0) ),
		valid( false )
		{
		}
	};

	//! Texture-space footprint at the hit point — the projection of
	//! the incoming ray's screen-space differentials onto the surface
	//! UV plane.  Populated at intersection time by geometries that
	//! support it (currently: triangle meshes) when the incoming
	//! ray has hasDifferentials = true.  Consumed by TexturePainter
	//! to compute mip LOD per Landing 2 of the PB pipeline plan.
	//!
	//! Units: dudx / dudy / dvdx / dvdy are the partial derivatives
	//! of the surface UV coordinates with respect to screen-space
	//! pixel x and y.  In other words, advancing one pixel in x
	//! moves the UV by (dudx, dvdx).  The texture-space Jacobian
	//! follows by multiplying by texture width / height.
	struct TextureFootprint
	{
		Scalar  dudx, dudy;
		Scalar  dvdx, dvdy;
		bool    valid;

		TextureFootprint() :
		dudx( 0 ), dudy( 0 ), dvdx( 0 ), dvdy( 0 ), valid( false )
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
		Vector3						vNormal;		// normal at the point of intersection (SHADING normal — Phong-interpolated on triangle meshes, perturbed by bump/normal-map modifiers)
		Vector3						vNormal2;		// normal at the point of exit
		//! GEOMETRIC normals at the entry / exit points — the actual
		//! flat-triangle face normal on triangle meshes (independent of
		//! Phong interpolation), or identical to `vNormal` / `vNormal2`
		//! on analytical primitives (sphere, ellipsoid, plane, …) where
		//! the surface IS smooth and shading == geometric by construction.
		//! Modifiers (bump map, normal map) perturb `vNormal` only;
		//! `vGeomNormal` / `vGeomNormal2` always reflect the underlying
		//! geometry.
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
		//! incoming ray (currently: double-sided triangle meshes -- see
		//! TriangleMeshGeometry::IntersectRay / TriangleMeshGeometryIndexed::
		//! IntersectRay).  Default false -- geometries that do not flip
		//! (single-sided meshes, and every analytical primitive whose
		//! geometric normal is the true, unmodified surface normal) leave
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
		bool						bGeomNormalOrientedToRay;

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

		Point3						ptIntersection;	// the point in world co-ordinates of the intersection, only	
													// set if there was an intersection
		Point3						ptExit;			// point at which the ray exits the object

		Point3						ptObjIntersec;	// the point of intersection on object space
		Point3						ptObjExit;		// the point of exit in object space

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
		  bHasTexCoord1( false ),
		  pCustom( 0 ),
		  glossyFilterWidth( 0 ),
		  ambientIOR( 1.0 ),
		  bHasVertexColor( false ),
		  bitangentSign( 1.0 ),
		  bHasTangent( false ),
		  bShadingTangentFromGeometry( false ),
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
		  ptCoord( r.ptCoord ),
		  ptCoord1( r.ptCoord1 ),
		  bHasTexCoord1( r.bHasTexCoord1 ),
		  ptIntersection( r.ptIntersection ),
		  ptExit( r.ptExit ),
		  ptObjIntersec( r.ptObjIntersec ),
		  ptObjExit( r.ptObjExit ),
		  onb( r.onb ),
		  pCustom( r.pCustom ),
		  glossyFilterWidth( r.glossyFilterWidth ),
		  ambientIOR( r.ambientIOR ),
		  derivatives( r.derivatives ),
		  txFootprint( r.txFootprint ),
		  vColor( r.vColor ),
		  bHasVertexColor( r.bHasVertexColor ),
		  vTangent( r.vTangent ),
		  bitangentSign( r.bitangentSign ),
		  bHasTangent( r.bHasTangent ),
		  bShadingTangentFromGeometry( r.bShadingTangentFromGeometry ),
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
			ptCoord = r.ptCoord;
			ptCoord1 = r.ptCoord1;
			bHasTexCoord1 = r.bHasTexCoord1;
			derivatives = r.derivatives;
			txFootprint = r.txFootprint;
			ptIntersection = r.ptIntersection;
			ptExit = r.ptExit;
			ptObjIntersec = r.ptObjIntersec;
			ptObjExit = r.ptObjExit;
			onb = r.onb;
			glossyFilterWidth = r.glossyFilterWidth;
			ambientIOR = r.ambientIOR;
			vColor = r.vColor;
			bHasVertexColor = r.bHasVertexColor;
			vTangent = r.vTangent;
			bitangentSign = r.bitangentSign;
			bHasTangent = r.bHasTangent;
			bShadingTangentFromGeometry = r.bShadingTangentFromGeometry;
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
