//////////////////////////////////////////////////////////////////////
//
//  ObjectManager.h - Declaration of the ObjectManager class which
//    helps scenes manage various geometric objects.  It builds a
//    top-level BVH4 acceleration structure (TLAS) over the objects,
//    with a linear-loop fallback for tiny scenes; see
//    Job::SetPrimaryAcceleration and docs/ARCHITECTURE.md "Top-Level
//    Acceleration (TLAS)".
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 16, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef OBJECTMANAGER_
#define OBJECTMANAGER_

#include "../Interfaces/IObjectManager.h"
#include "../Utilities/Threads/Threads.h"
#include "GenericManager.h"
#include "../Acceleration/BVH.h"
#include "../Octree.h"

#include <map>
#include <set>

namespace RISE
{
	namespace Implementation
	{
		class ObjectManager : 
			public virtual IObjectManager, 
			public virtual GenericManager<IObjectPriv>,
			public virtual TreeElementProcessor<const IObjectPriv*>
		{
		protected:
			virtual ~ObjectManager();

			// Top-level acceleration: SAH BVH (BVH4-collapsed, SIMD AABB
			// test) over scene objects.  bUseBSPtree is the constructor
			// gate name kept for back-compat; semantically it now means
			// "build a top-level BVH".
			mutable BVH<const IObjectPriv*>*    pBVH;
			mutable Octree<const IObjectPriv*>* pOctree;
			mutable unsigned long long          mSpatialGen;   //!< advanced on every InvalidateSpatialStructure (see IObjectManager)

			bool bUseBSPtree;
			bool bUseOctree;
			const unsigned int nMaxObjectsPerNode;
			const unsigned int nMaxTreeDepth;

			RMutex treeCreationMutex;

			//! The AUTHORED graph: child name -> parent name.  Only non-root
			//! objects appear.  See IObjectManager's block comment for why this
			//! is by name and one-directional.
			std::map<String,String> parentByName;

			//! Did the LAST walk actually compose anything against a
			//! non-identity parent?  This is what makes the "no links -> no
			//! work" fast path SOUND.  Without it, un-parenting the last child
			//! (a detach, or removing its parent) would empty `parentByName`
			//! and the fast path would then skip the one walk that still had to
			//! run -- leaving the ex-child rendering at its old composed pose
			//! forever.
			mutable bool anyComposedAgainstParent;

			//! Names whose recorded parent has since disappeared from the
			//! manager.  Warned about once each (a dangling link is treated as
			//! "root"), so a per-frame compose cannot turn one authoring
			//! mistake into a log flood.
			mutable std::set<String> danglingParentWarned;

			// Shadow cache: per-slot last occluder, padded to avoid false sharing.
			// Each thread hashes into a slot; collisions are harmless (just a stale hint).
			static const unsigned int kShadowCacheSlots = 64;
			struct ShadowCacheSlot {
				const IObjectPriv* pOccluder;
				char pad[64 - sizeof(const IObjectPriv*)];
			};
			mutable ShadowCacheSlot* shadowCache;

			// Realize all objects' deferred geometry (idempotent) before any bbox/
			// TLAS query.  Called from PrepareForRendering AND CreateBVH/CreateOctree.
			void RealizeAllObjects() const;
			void CreateBVH() const;
			void CreateOctree() const;

		public:
			ObjectManager(
				const bool bUseBSPtree,
				const bool bUseOctree,
				const unsigned int nMaxObjectsPerNode,
				const unsigned int nMaxTreeDepth
				);

			void IntersectRay(
				RayIntersection& ri,
				const bool bHitFrontFaces,
				const bool bHitBackFaces,
				const bool bComputeExitInfo
				) const;

			bool IntersectShadowRay(
				const Ray& ray,
				const Scalar dHowFar,
				const bool bHitFrontFaces,
				const bool bHitBackFaces
				) const;

			void EnumerateObjects( IEnumCallback<IObject>& pFunc ) const;
			void EnumerateObjects( IEnumCallback<IObjectPriv>& pFunc ) const;

			// From TreeElementProcessor
			typedef const IObjectPriv*	MYOBJ;
				void RayElementIntersection( RayIntersectionGeometric& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const;
				void RayElementIntersection( RayIntersection& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const;
				bool RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const;
				BoundingBox GetElementBoundingBox( const MYOBJ elem ) const;
				bool ElementBoxIntersection( const MYOBJ elem, const BoundingBox& bbox ) const;
				char WhichSideofPlaneIsElement( const MYOBJ elem, const Plane& plane ) const;

			void SerializeElement( IWriteBuffer& buffer, const MYOBJ elem ) const;
			void DeserializeElement( IReadBuffer& buffer, MYOBJ& ret ) const;

			//! Tells all the objects to reset any runtime data
			void ResetRuntimeData() const;

			bool SetObjectParent( const char* child, const char* parent );
			const char* GetObjectParent( const char* child ) const;
			bool ComposeWorldTransforms() const;

			//! Removing an object also retires its place in the authored graph:
			//! its own parent link goes, and any object that named it as parent
			//! is RE-ROOTED.  Doing it here rather than leaving the link to
			//! dangle keeps `parentByName` an exact description of the live
			//! object set -- which is what lets ComposeWorldTransforms treat a
			//! dangling link as a genuine anomaly worth a warning.
			bool RemoveItem( const char* szName );

			void PrepareForRendering() const;
			void InvalidateSpatialStructure() const;
			unsigned long long GetSpatialStructureGeneration() const { return mSpatialGen; }
		};
	}
}

#endif
