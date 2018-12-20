//////////////////////////////////////////////////////////////////////
//
//  DAGObjectManager.h - Declaration of the DAGObjectManager class
//    which stores scene objects in a directed acyclic graph.  This
//    allows all sorts of nifty things like hierarchical transformations
//    and hierarchical materails.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 6, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DAGOBJECTMANAGER_
#define DAGOBJECTMANAGER_

#include "../Interfaces/IObjectManager.h"
#include "GenericManager.h"

namespace RISE
{
	namespace Implementation
	{
		class DAGObjectManager : public virtual IObjectManager, public virtual GenericManager<IObjectPriv>
		{
		protected:
			virtual ~DAGObjectManager();

			class DAGNode
			{
			protected:
				typedef std::map<String,IObjectPriv*> ObjectListType;
				ObjectListType					objects;				///< Objects at this particular level
				typedef std::map<String,DAGNode*>	ChildrenListType;
				ChildrenListType				children;				///< Children of this node
				
			public:
				DAGNode();
				virtual ~DAGNode();

				bool AddItem( IObjectPriv* pObject, const std::vector<String>& names, const std::vector<String>::const_iterator& location );
				bool RemoveItem( const std::vector<String>& names, const std::vector<String>::const_iterator& location );

				void Shutdown();
			};

			DAGNode		root;

			void SeperateNames( std::vector<String>& names, const char * szName );

		public:
			DAGObjectManager();

			bool AddItem( IObjectPriv* pObject, const char * szName );
			bool RemoveItem( const char * szName );
			void Shutdown();		

			void IntersectRay( RayIntersection& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const;
			void EnumerateObjects( IEnumCallback<IObject>& pFunc ) const;
			void EnumerateObjects( IEnumCallback<IObjectPriv>& pFunc ) const;
		};
	}
}

#endif
