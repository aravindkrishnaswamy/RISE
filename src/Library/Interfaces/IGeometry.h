//////////////////////////////////////////////////////////////////////
//
//  IGeometry.h - Geometry interface
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 28, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IGEOMETRY_
#define IGEOMETRY_

#include "IReference.h"
#include "IKeyframable.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include "../Utilities/BoundingBox.h"
#include "../Polygon.h"
#include <cmath>			// std::fabs (SelfHitRootFloor's default body)

namespace RISE
{
	//! Surface derivative data for specular manifold sampling.
	//! Contains position and normal partial derivatives at a surface point,
	//! needed for the Newton solver's constraint Jacobian.
	struct SurfaceDerivatives
	{
		Vector3 dpdu;		///< Position partial derivative w.r.t. first surface parameter
		Vector3 dpdv;		///< Position partial derivative w.r.t. second surface parameter
		Vector3 dndu;		///< Normal partial derivative w.r.t. first surface parameter
		Vector3 dndv;		///< Normal partial derivative w.r.t. second surface parameter
		Point2  uv;			///< Surface parameters at this point
		bool    valid;		///< True if derivatives were successfully computed

		//! THE CHART MAP: the 2x2 Jacobian of the TEXTURE coordinate
		//! `(s, t)` this geometry stamps into
		//! `RayIntersectionGeometric::ptCoord` with respect to the
		//! parameters `(u, v)` the four vectors above differentiate.
		//!
		//! `dpdu` / `dpdv` are free to use the geometry's OWN natural
		//! parameters (docs/GEOMETRY_DERIVATIVES.md "Magnitudes and
		//! parameter scaling": the sphere's `u` is an azimuth in
		//! RADIANS, the cylinder's is an axial WORLD coordinate, the
		//! torus swaps its two angles for right-handedness), while
		//! `ptCoord` is whatever normalised, possibly axis-swapped,
		//! possibly sign-flipped `[0, 1]^2` chart the matching
		//! `GeometricUtilities::*TextureCoord` produces.  A consumer
		//! that combines `ptCoord` with a `(u, v)`-chart derivative --
		//! `SolveFootprintUV`, whose output feeds mip LOD -- needs the
		//! affine bridge between the two, and cannot guess it.
		//!
		//!   ds = dsdu*du + dsdv*dv
		//!   dt = dtdu*du + dtdv*dv
		//!
		//! LOCAL: at a wrap seam or a pole the underlying map is not
		//! differentiable, and this states the Jacobian of the local
		//! branch only.
		//!
		//! DEFAULT identity with `texChartValid = false` -- the honest
		//! "this geometry did not say".  A consumer must treat false as
		//! "no texcoord Jacobian available" rather than assuming the
		//! identity: an unset flag on a geometry whose two charts differ
		//! by 2*pi is exactly the wrong-chart bug this field exists to
		//! prevent.
		Scalar  dsdu, dsdv, dtdu, dtdv;
		bool    texChartValid;

		SurfaceDerivatives() :
		dpdu( Vector3(0,0,0) ), dpdv( Vector3(0,0,0) ),
		dndu( Vector3(0,0,0) ), dndv( Vector3(0,0,0) ),
		uv( Point2(0,0) ), valid( false ),
		dsdu( 1 ), dsdv( 0 ), dtdu( 0 ), dtdv( 1 ), texChartValid( false )
		{
		}
	};

	//! Geometry represents the basic geometry of a scene object
	//! It needs only to provide basic geometric intersection details
	class IGeometry :
		public virtual IReference,
		public virtual IKeyframable
	{
	protected:
		IGeometry(){};
		virtual ~IGeometry(){};

	public:
		//! Tessellates this geometry into an indexed triangle mesh.
		//!
		//! Consumers (e.g. DisplacedGeometry, future GPU mesh export) use this to obtain a
		//! triangle-mesh representation of any geometry that supports it.  The four output
		//! vectors are appended to; callers typically pass freshly-constructed empty vectors.
		//!
		//! Detail semantics are per-geometry and documented on each override:
		//!  - Parametric primitives (sphere, torus, cylinder, box, disk, clipped plane):
		//!    `detail` is the number of segments per natural parameter axis.
		//!  - Mesh geometries (TriangleMesh, TriangleMeshGeometryIndexed): `detail` is ignored;
		//!    existing triangles are emitted unchanged.
		//!  - InfinitePlaneGeometry: returns false (cannot tessellate infinite extent).
		//!
		//! Default implementation returns false (unsupported).  Callers that need an error
		//! message should log it themselves using whatever user-facing name they have for
		//! the geometry — the base default deliberately stays silent to keep the interface
		//! header free of logging dependencies.
		/// \return TRUE if tessellation produced a mesh, FALSE if the geometry cannot be tessellated.
		virtual bool TessellateToMesh(
			IndexTriangleListType& tris,		///< [out] Indexed triangles (appended)
			VerticesListType&      vertices,	///< [out] Vertex positions (appended)
			NormalsListType&       normals,		///< [out] Per-vertex normals (appended)
			TexCoordsListType&     coords,		///< [out] Per-vertex (u,v) texture coords (appended)
			const unsigned int     detail		///< [in] Tessellation detail level (see per-geometry docs)
			) const
		{
			return false;
		}

		//! This the most important function
		//! It asks the geometric object to intersect itself
		//! and return intersection details
		//
		//! If a sub class doesn't override this method, then
		//! it will be called here and we'll handle it
		virtual void IntersectRay( 
			RayIntersectionGeometric& ri,				///< [in/out] Receives the geometric intersection information
			const bool bHitFrontFaces,					///< [in] Should we process the intersection if the element is front facing?
			const bool bHitBackFaces,					///< [in] Should we process the intersection if the element is back facing?
			const bool bComputeExitInfo					///< [in] Should exit information be computed (the ray continues until exiting the object) in addition of initial intersection information?
			) const = 0;

		//! This function is here to help the shadow checks
		//! It asks the geometry object if the given ray
		//! will intersect the object, we don't care about
		//! where or the normal or any of that junk
		//!
		//! Similar to IntersectRay, if the sub class doesn't
		//! override this method, then it will be called here
		//! and we'll handle it
		/// \return TRUE if there is an intersection, FALSE otherwise
		virtual bool IntersectRay_IntersectionOnly(
			const Ray& ray,								///< [in] The ray to process the intersection from
			const Scalar dHowFar,						///< [in] Maximum distance to travel along that ray (optimization parameter)
			const bool bHitFrontFaces,					///< [in] Should we process the intersection if the element is front facing?
			const bool bHitBackFaces					///< [in] Should we process the intersection if the element is back facing?
			) const = 0;

		//! Generates a sphere that envelopes all the geometry
		virtual void GenerateBoundingSphere(
			Point3& ptCenter,							///< [out] Center of the bounding sphere
			Scalar& radius								///< [out] Radius of the bounding sphere
			) const = 0;

		//! Generates a box that envelopes all the geometry
		virtual BoundingBox GenerateBoundingBox(
			) const = 0;

		//! Should bounding spheres or boxes be tested against before doing a full out intersection test?
		/// \return TRUE if bounding boxes/sphere should be checked, FALSE otherwise
		virtual bool DoPreHitTest() const = 0; 

		//! Returns a uniformly random point on the surface.  Needed to sample luminary geometry surfaces
		//! This function guarantees that for the same prand, the same data is returned
		virtual void UniformRandomPoint(
			Point3* point,								///< [out] Point on the surface
			Vector3* normal,							///< [out] Normal at the point on the surface
			Point2* coord,								///< [out] Texture co-ordinate at the point on the surface
			const Point3& prand						///< [in] Variables used in point generation
			) const = 0;

		//! Gets the area of the geometry.  Needed for luminaries.
		/// \return Area of the geometry in Meters Squared
		virtual Scalar GetArea( ) const = 0;

		//! Computes surface derivatives at a point on the geometry.
		//! Used by specular manifold sampling for the Newton solver's
		//! constraint Jacobian.  All inputs and outputs are in object space.
		//!
		//! Default implementation returns invalid (valid=false).
		//! Geometry subclasses should override with analytical derivatives.
		/// \return SurfaceDerivatives struct with dpdu, dpdv, dndu, dndv
		virtual SurfaceDerivatives ComputeSurfaceDerivatives(
			const Point3& objSpacePoint,		///< [in] Point on the surface in object space
			const Vector3& objSpaceNormal		///< [in] Normal at the point in object space
			) const
		{
			return SurfaceDerivatives();
		}

		//! Smoothing-aware analytical surface query keyed on parametric (u, v).
		//!
		//! Returns position, unit normal, and tangent / normal partial
		//! derivatives of the underlying parameterised surface.  The
		//! `smoothing` parameter ∈ [0, 1] interpolates between the actual
		//! high-frequency-detailed surface (s = 0) and a Lipschitz-smooth
		//! reference surface (s = 1).  For tessellated analytical primitives
		//! (sphere, ellipsoid, ...) smoothing is a no-op — there is no
		//! high-frequency detail to attenuate.  For composite surfaces like
		//! `displaced_geometry`, smoothing scales the displacement amplitude:
		//! at s = 1 the surface collapses to its smooth base.
		//!
		//! Used by the SMS two-stage Newton solver: stage 1 walks on s = 1
		//! to find a seed in a C1-smooth landscape, stage 2 refines on
		//! s = 0.  See `docs/SMS_TWO_STAGE_SOLVER.md`.
		//!
		//! Default implementation returns false.  Pure triangle meshes
		//! loaded from .obj/.glTF have no smooth analytical surface.
		//!
		//! \return TRUE if analytical derivatives were produced; FALSE otherwise.
		virtual bool ComputeAnalyticalDerivatives(
			const Point2& uv,
			Scalar        smoothing,			///< [in] 0 = full detail, 1 = smooth base
			Point3&       outPosition,			///< [out] Object-space surface position
			Vector3&      outNormal,			///< [out] Object-space unit normal
			Vector3&      outDpdu,				///< [out] dP/du
			Vector3&      outDpdv,				///< [out] dP/dv
			Vector3&      outDndu,				///< [out] dN/du
			Vector3&      outDndv				///< [out] dN/dv
			) const
		{
			(void)uv; (void)smoothing;
			(void)outPosition; (void)outNormal;
			(void)outDpdu; (void)outDpdv;
			(void)outDndu; (void)outDndv;
			return false;
		}

		//! Whether this geometry may serve as an AREA LIGHT (and, more generally,
		//! whether UniformRandomPoint() / GetArea() honour their EXACT-surface-
		//! sampling contract).  The light sampler assumes UniformRandomPoint()
		//! samples the true emitting SURFACE uniformly and GetArea() returns that
		//! surface's area, using pdfPosition = 1/area for NEE + MIS; the point-set
		//! SSS shaderops build their irradiance caches on the same contract.  A
		//! geometry that cannot honour it (e.g. a field that tessellates to zero
		//! surface area) returns false so those consumers can refuse it -- with a
		//! diagnostic -- instead of sampling a broken surface.  Default true
		//! (analytic primitives and meshes sample their surface exactly).
		//!
		//! Declared last + defaulted so adding it keeps every existing IGeometry
		//! vtable slot (ABI-stable) and needs no change to existing geometries.
		virtual bool CanBeAreaLight() const { return true; }

		//! Materialize any deferred (lazily-built) representation this
		//! geometry needs before rendering.  Called ONCE per render from a
		//! SINGLE-THREADED point (the realize pass in RayCaster::AttachScene)
		//! BEFORE the parallel rasterize — RISE's scene is immutable during
		//! the parallel pass, so expensive build work (e.g. tessellating +
		//! baking a displaced mesh) cannot happen lazily on the const hot
		//! path.  Idempotent: a second call is a no-op once realized.
		//! Composite geometries (DisplacedGeometry) must CASCADE —
		//! realize their base(s) first.
		//!
		//! `const` because realization materializes a build-time cache that
		//! is a pure function of the geometry's recipe; the observable
		//! surface is unchanged (matching the `mutable` lazy-BVH pattern in
		//! ObjectManager::PrepareForRendering, also const).  Default: cheap
		//! geometries (sphere, mesh, ...) are always realized — no-op.
		virtual void Realize() const {}

		//! The smallest ray parameter this geometry's intersection routines will
		//! accept as a genuine hit, for a ray leaving `localOrigin` (which sits on
		//! or immediately off the face whose OUTWARD unit normal is `localNormal`)
		//! along the UNIT direction `localDir`.  Everything is in this geometry's
		//! OWN object space, and the return value is a range in those units.
		//!
		//! Every RISE intersector gates its roots with a self-hit floor so that a
		//! ray published FROM a surface (Object::IntersectRay backs the hit point
		//! off SURFACE_INTERSEC_ERROR = 1e-12 along the incoming ray) does not
		//! re-hit that same surface at t ~ 0.  Since a8bef210 those floors are
		//! SCALE-RELATIVE, so they are no longer a single global constant a caller
		//! can hard-code: a sphere of radius 4 rejects roots below ~9e-12, a
		//! 1000-unit one below ~2e-9.  Any caller that deliberately stands a ray
		//! off a surface in order to re-hit it must clear the floor of the
		//! geometry it is aiming at, so it has to be able to ASK.  The one such
		//! caller today is CSGObject's exit-face payload probe
		//! (AdoptCsgExitFacePayloadViaProbe), which stood off by a box-derived
		//! band and silently fell back to the ENTRY face's payload -- an
		//! antipodal UV on the far cavity wall -- on any operand bigger than a few
		//! units.
		//!
		//! Default: the generic `NEARZERO * (1 + |localOrigin|_1)` floor that
		//! RayQuadricIntersection (EllipsoidGeometry) and RayPlaneIntersection
		//! (InfinitePlaneGeometry, CircularDiskGeometry) use verbatim.  Override
		//! wherever the geometry's own gate is different (a larger coordinate
		//! scale, a plane-distance band, a ray-march surface epsilon).  A
		//! geometry whose gate is SMALLER than the default may leave it alone --
		//! over-stating is the safe direction for the contract below, just
		//! conservative (HairGeometry, whose curve gate is a flat NEARZERO, does
		//! exactly that).
		//!
		//! Contract for overriders: the return must be an UPPER bound on the
		//! geometry's own gate along `localDir` -- a caller standing off by more
		//! than this and firing back must get the hit.  A geometry whose gate is
		//! a PLANE DISTANCE rather than a range (BoxGeometry) must therefore
		//! divide the band by |localDir . localNormal|, clamped away from zero so
		//! a grazing query returns a finite (if large) answer rather than
		//! infinity.
		//!
		//! DEFAULTED (a real body, not `= 0`) so an out-of-tree IGeometry
		//! implementer that predates this method still COMPILES and gets the
		//! generic floor, the same reason CanBeAreaLight / CanTessellate carry
		//! bodies.  It is NOT declared last -- CanTessellate follows it -- so it
		//! makes no vtable-slot claim: this is a source-compatibility promise
		//! only.  (An earlier draft of this comment asserted both; corrected in
		//! the adversarial review of 4b141ad3.)  In-tree, `src/3DSMax`'s
		//! MAXGeometry is one such default-inheriting implementer -- it is not
		//! part of any in-tree build, and it never acts as a CSG operand, so the
		//! generic floor is all it needs.
		virtual Scalar SelfHitRootFloor(
			const Point3&  localOrigin,		///< [in] Ray origin, this geometry's object space
			const Vector3& localDir,		///< [in] UNIT ray direction, same space
			const Vector3& localNormal		///< [in] Unit outward normal of the face being re-hit
			) const
		{
			(void)localDir; (void)localNormal;
			return NEARZERO * ( Scalar(1) +
				std::fabs( localOrigin.x ) + std::fabs( localOrigin.y ) + std::fabs( localOrigin.z ) );
		}

		//! Cheap, static capability hint: can this geometry produce a triangle
		//! mesh via TessellateToMesh (after Realize, if deferred)?  Used at SCENE-
		//! PARSE time so a composite like DisplacedGeometry can REFUSE a
		//! non-tessellatable base (e.g. InfinitePlaneGeometry) immediately rather
		//! than failing later at realize-time.  Default TRUE (every concrete RISE
		//! geometry tessellates except InfinitePlaneGeometry, which overrides); a
		//! future non-tessellatable geometry that forgets to override merely
		//! degrades to the graceful realize-time guard-fail, never a false refusal.
		//! Declared last + defaulted so adding it keeps every existing IGeometry
		//! vtable slot ABI-stable (the mid-vtable insert this replaces would have
		//! shifted IntersectRay and every later slot for stale implementers).
		virtual bool CanTessellate() const { return true; }

		//! SHORTEST DISTANCE from `ptObject` to this geometry's own surface,
		//! in this geometry's own object space -- the per-family half of the
		//! `proximity(r)` query (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §5.2).
		//!
		//! UNSIGNED.  A point INSIDE the solid is at distance 0 from the
		//! surface for this query's purposes, not at a negative distance:
		//! interpenetration is contact, and the signal is deliberately
		//! sign-free in v1 (a signed variant needs an inside test per
		//! family, and is Phase 3).
		//!
		//! THE CONTRACT FOR OVERRIDERS IS ONE-SIDED, and it is the whole
		//! safety argument of the signal: the value written must be the
		//! true distance or an UPPER BOUND on it -- NEVER a lower one.  An
		//! over-report makes `proximity` under-paint a seam, which is a
		//! feature failure.  An under-report makes it paint contact where
		//! there is none, which is a wrong render.  A family that cannot
		//! honour that must REFUSE.
		//!
		//! `maxDistObject` is a permission, not a promise: an implementer
		//! may return false as soon as it can prove the answer exceeds it,
		//! and one that ignores it is still correct, only slower.  A
		//! refusal means "this geometry contributes nothing to this query",
		//! and the caller treats it as far.
		//!
		//! DEFAULTED to a refusal (a real body, not `= 0`) so every
		//! geometry that has no closed form -- non-indexed RAW meshes,
		//! Bezier patches, hair, and the out-of-tree 3DSMax implementer --
		//! compiles unchanged and honestly contributes nothing.  Declared
		//! LAST and defaulted, so it makes no claim on any existing vtable
		//! slot.
		//! \return TRUE and writes `outDist` (>= 0), or FALSE with
		//!         `outDist` untouched.
		virtual bool DistanceToSurface(
			const Point3& ptObject,			///< [in] Query point, THIS geometry's object space
			const Scalar maxDistObject,		///< [in] Search radius in the same space; may refuse beyond it
			Scalar& outDist					///< [out] Distance to the surface, same space
			) const
		{
			(void)ptObject; (void)maxDistObject; (void)outDist;
			return false;
		}

		//! SIGNED distance LOWER BOUND from `ptObject` to this geometry's
		//! surface, with an EXACT SIGN -- the other direction from
		//! `DistanceToSurface`, and the capability Phase 3 of the
		//! cross-object proximity design rests on
		//! (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §5.6).
		//!
		//! THE CONTRACT, and it is the mirror image of the unsigned one:
		//!   * SIGN IS EXACT.  `outSigned < 0` iff `ptObject` is strictly
		//!     inside this geometry's solid, `> 0` iff strictly outside,
		//!     `0` on the surface.  A family that cannot decide inside
		//!     from outside -- every SHEET (plane, disk, clipped plane,
		//!     open cylinder, mesh, patch, hair) -- must REFUSE.
		//!   * MAGNITUDE IS A LOWER BOUND: `|outSigned| <= ` the true
		//!     distance to the surface.  That is what makes the CSG
		//!     descent step `|f|` unable to overshoot the zero set and
		//!     what makes an `interior` depth an under-read rather than an
		//!     over-read.  It is the OPPOSITE direction from
		//!     `DistanceToSurface`'s upper bound, so the two cannot share
		//!     an implementation and the transform layer converts them by
		//!     different singular values (`×σ_min` here, `×σ_max` there).
		//!
		//! `outExact` says the magnitude IS the distance (not merely a
		//! bound), which is what a CSG composite's BOUNDARY ARM consumes
		//! to admit a landing exactly on this operand's surface.  Set it
		//! ONLY for a closed-form family on a NON-DEGENERATE instance; a
		//! bounded family (ellipsoid, SDF) must leave it false.  Cleared
		//! by the refusing default below, so an implementer that forgets
		//! it cannot leak a stale `true` from the caller's stack.
		//!
		//! IT NEVER REFUSES FOR RANGE.  `maxDistObject` bounds EFFORT only
		//! -- the CSG descent evaluates operands at points well outside
		//! any query radius, so an implementer that copied
		//! `DistanceToSurface`'s lower-bound early-out into this method
		//! would make every intersection with a small subtrahend refuse.
		//! Only a FAMILY or a DEGENERACY refusal is legitimate here.
		//!
		//! DEFAULTED to a refusal, declared LAST: no claim on any existing
		//! vtable slot, and every sheet family compiles unchanged.
		//! \return TRUE and writes both outputs, or FALSE with
		//!         `outSigned` untouched and `outExact` cleared.
		virtual bool SignedDistanceLower(
			const Point3& ptObject,			///< [in] Query point, THIS geometry's object space
			const Scalar maxDistObject,		///< [in] Effort budget in the same space; NOT a range refusal
			Scalar& outSigned,				///< [out] Signed lower bound (negative inside), same space
			bool& outExact					///< [out] TRUE when the magnitude is the exact distance
			) const
		{
			(void)ptObject; (void)maxDistObject; (void)outSigned;
			outExact = false;
			return false;
		}
	};
}


#endif
