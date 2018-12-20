//////////////////////////////////////////////////////////////////////
//
//  CSGObject.h - A CSG object
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 22, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CSG_OBJECT_
#define CSG_OBJECT_

#include "Object.h"
#include "../Utilities/RString.h"

namespace RISE
{
	namespace Implementation
	{
		enum CSG_OP
		{
			CSG_UNION			= 0,
			CSG_INTERSECTION	= 1,
			CSG_SUBTRACTION		= 2
		};

		class CSGObject : public virtual Object
		{
		protected:
			IObjectPriv*							pObjectA;
			IObjectPriv*							pObjectB;

			CSG_OP									op;

			virtual ~CSGObject( );

		public:

			CSGObject( const CSG_OP& op_ );

			bool AssignObjects( IObjectPriv* objA, IObjectPriv* objB );

			IObjectPriv* CloneFull();
			IObjectPriv* CloneGeometric();

			void IntersectRay( RayIntersection& ri, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const;

			void ResetRuntimeData() const;
		};
	}
}

#endif
