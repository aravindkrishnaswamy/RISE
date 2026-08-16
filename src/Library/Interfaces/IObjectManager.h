//////////////////////////////////////////////////////////////////////
//
//  IObjectManager.h - Interface to the object manager, which manages
//    all objects in a scene
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 30, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IOBJECTMANAGER_
#define IOBJECTMANAGER_

#include "IManager.h"
#include "IEnumCallback.h"
#include "../Intersection/RayIntersection.h"

namespace RISE
{
	class IObjectPriv;

	class IObjectManager : public virtual IManager<IObjectPriv>
	{
	protected:
		IObjectManager(){};
		virtual ~IObjectManager(){};

	public:
		//
		// Object specific
		//

		//! Intersects a ray with the object
		virtual void IntersectRay( 
			RayIntersection& ri,						///< [in/out] Intersection details at point of intersection if there is an intersection
			const bool bHitFrontFaces,					///< [in] Should front facing hits be processed?
			const bool bHitBackFaces,					///< [in] Should back facing hits be processed?
			const bool bComputeExitInfo					///< [in] Should exit information be computed (the ray continues until exiting the object) in addition of initial intersection information?
			) const = 0;

		//! Intersects, but performs intersection test only with object
		virtual bool IntersectShadowRay( 
			const Ray& ray,								///< [in] The ray to process the intersection from
			const Scalar dHowFar,						///< [in] Maximum distance to travel along that ray (optimization parameter)
			const bool bHitFrontFaces,					///< [in] Should we process the intersection if the element is front facing?
			const bool bHitBackFaces					///< [in] Should we process the intersection if the element is back facing?
			) const = 0;

		//! Enumerates the objects with normal interfaces
		virtual void EnumerateObjects( 
			IEnumCallback<IObject>& pFunc				///< [in] Callback functor interface that accepts normal IObject interface
			) const = 0;

		//! Enumerates the objects with their priviledged interfaces
		virtual void EnumerateObjects( 
			IEnumCallback<IObjectPriv>& pFunc			///< [in] Callback functor interface that accepts the IObjectPriv interface
			) const = 0;

		//! Tells all the objects to reset any runtime data
		virtual void ResetRuntimeData() const = 0;

		//! Eagerly builds spatial acceleration structures before rendering begins.
		//! Must be called before multi-threaded rendering to avoid lazy init during render.
		virtual void PrepareForRendering() const = 0;

		//! Destroys the current spatial acceleration structure so it can be rebuilt.
		//! Used when animated object transforms invalidate the world-space bounding
		//! boxes that the BSP/octree was built from. Call PrepareForRendering() after
		//! this to rebuild with current transforms.
		virtual void InvalidateSpatialStructure() const = 0;

		// ---- Authored scene graph (docs/agentic-redesign/87-recursive-scene-graph.md)
		//
		// TWO REPRESENTATIONS, SEPARATE JOBS.  The manager's item map is the
		// FLAT RENDER LIST: every entry carries a fully composed world
		// transform, and it is what the TLAS is built over and what all nine
		// world-visible enumeration consumers walk.  The parent links below are
		// the AUTHORED GRAPH: a side index that says nothing about rendering
		// and exists so ComposeWorldTransforms can bake the flat list's world
		// transforms, and so the outliner can show a tree.
		//
		// Links are stored BY NAME, one direction only (child -> parent name).
		// No pointer is retained, so no lifetime cycle is possible and a
		// removed parent degrades to "treat the child as a root" rather than
		// to a dangling deref.

		//! Record `child`'s parent.  A null / empty `parent` DETACHES the child
		//! (it becomes a root).  Refuses -- with a diagnostic, changing
		//! nothing -- when the child or the parent is not a registered object,
		//! when they are the same object, or when the link would close a cycle.
		//! \return TRUE if the link was recorded (or cleared), FALSE if refused.
		virtual bool SetObjectParent(
			const char* child,							///< [in] Name of the child object
			const char* parent							///< [in] Name of the parent object, or null / "" to detach
			) = 0;

		//! The recorded parent name for `child`, or "" when it is a root or
		//! not a registered object.  The returned pointer is owned by the
		//! manager and is valid until the next SetObjectParent call.
		virtual const char* GetObjectParent(
			const char* child							///< [in] Name of the child object
			) const = 0;

		//! Does any registered object name `parent` as its parent?  Cheap query
		//! over the link map.  Removing such an object MOVES its children (they
		//! fall back to their own local transforms), which is a spatial and
		//! potentially a light-topology change that no per-object check can
		//! see -- the children are not the object being removed.
		virtual bool HasChildren(
			const char* parent							///< [in] Name of the candidate parent
			) const = 0;

		//! Walk the authored graph and bake `world = parent.world * local` into
		//! every object, in parent-before-child order, via
		//! ITransformable::FinalizeTransformations( parentWorld ).  Idempotent:
		//! composition is an ARGUMENT, never a stack push, so running this
		//! twice yields the same matrices.  Objects with no parent link are
		//! finalized against identity, which is exactly what they did before
		//! hierarchy existed.
		//! \return TRUE if ANY object's world matrix actually changed.  The CST
		//! incremental apply uses this as its spatial gate: a re-parented or
		//! re-posed container moves DESCENDANTS that the edit closure never
		//! contains, so their bounding boxes cannot be compared pairwise -- this
		//! answer is what makes the TLAS invalidation sound.
		virtual bool ComposeWorldTransforms() const = 0;

		//! Monotonic counter advanced every time InvalidateSpatialStructure() runs.
		//! A consumer reads it across an edit to confirm whether the top-level
		//! acceleration was invalidated: the CST incremental apply's closure-gated
		//! invariant pass leaves it UNCHANGED for a non-spatial edit (material/painter
		//! value -- object bboxes unchanged) and advances it for a spatial edit
		//! (geometry-extent / transform change), which is exactly the "non-spatial
		//! edit skips the TLAS" property (docs/agentic-redesign/21-stable-apply-and-
		//! resolver.md slices 3-4).
		virtual unsigned long long GetSpatialStructureGeneration() const = 0;
	};
}

#include "IObjectPriv.h"

#endif
