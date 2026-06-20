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

			//! feature/gui-snapshot-prototype: snapshot-clone AS a CSGObject.
			//! The base Object::CloneSnapshot would slice a CSGObject to a
			//! plain Object (operands + operation lost, null geometry).  This
			//! overrides it to snapshot-clone the operation + BOTH operands
			//! (recursively, via each operand's virtual CloneSnapshot) and
			//! then copy the shared mutable state via CopySnapshotStateInto.
			//! NOTE: deliberately NOT marked `override` to match this class's
			//! existing no-`override` style — adding the keyword to one method
			//! wakes -Winconsistent-missing-override on the 7 sibling virtuals.
			Object* CloneSnapshot() const;

			const BoundingBox getBoundingBox() const;
			void IntersectRay( RayIntersection& ri, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const;

			void ResetRuntimeData() const;

			// Deferred-realization (IObject): the realize pass enumerates only
			// world-VISIBLE objects, but AssignObjects() hides our two operands,
			// so they are never reached directly.  Cascade into them here so a
			// deferred geometry (e.g. displaced) used as a CSG operand is baked.
			void Realize() const;
		};
	}
}

#endif
