//////////////////////////////////////////////////////////////////////
//
//  ObjectManager.h - Declaration of the ObjectManager class which
//    helps scenes manage various geometric objects.  This is the
//    really simple and stupid object manager.  It just stores
//    everything in a list.  For a better object manager, look at
//    DAGObjectManager, which uses a directed acylic graph.
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
#include "../BSPTreeSAH.h"
#include "../Octree.h"

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

			mutable BSPTreeSAH<const IObjectPriv*>* pBSPtree;
			mutable Octree<const IObjectPriv*>* pOctree;

			bool bUseBSPtree;
			bool bUseOctree;
			const unsigned int nMaxObjectsPerNode;
			const unsigned int nMaxTreeDepth;

			RMutex treeCreationMutex;

			// Shadow cache: per-slot last occluder, padded to avoid false sharing.
			// Each thread hashes into a slot; collisions are harmless (just a stale hint).
			static const unsigned int kShadowCacheSlots = 64;
			struct ShadowCacheSlot {
				const IObjectPriv* pOccluder;
				char pad[64 - sizeof(const IObjectPriv*)];
			};
			mutable ShadowCacheSlot* shadowCache;

			void CreateBSPTree() const;
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

			void PrepareForRendering() const;
			void InvalidateSpatialStructure() const;
		};
	}
}

#endif
