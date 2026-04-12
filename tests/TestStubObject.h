//////////////////////////////////////////////////////////////////////
//
//  TestStubObject.h - Minimal IObject stub for SPF unit tests.
//
//  Provides a lightweight object that satisfies the IORStack's
//  requirement for a current object during push/pop operations,
//  mimicking how the rendering pipeline sets up the IOR stack
//  before calling SPF::Scatter.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TEST_STUB_OBJECT_H_
#define TEST_STUB_OBJECT_H_

#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Interfaces/IObject.h"

namespace RISE
{
	/// Minimal IObject stub for unit tests.
	/// Satisfies the interface so that IORStack::push/pop work
	/// without warnings, and containsCurrent() returns meaningful
	/// results.
	class StubObject :
		public virtual IObject,
		public virtual Implementation::Reference
	{
	public:
		StubObject() {}

		// IObject
		void IntersectRay( RayIntersection&, const Scalar, const bool, const bool, const bool ) const {}
		bool IntersectRay_IntersectionOnly( const Ray&, const Scalar, const bool, const bool ) const { return false; }
		bool IsWorldVisible() const { return true; }
		bool DoesCastShadows() const { return false; }
		bool DoesReceiveShadows() const { return false; }
		const IMaterial* GetMaterial() const { return 0; }
		const IMedium* GetInteriorMedium() const { return 0; }
		void UniformRandomPoint( Point3*, Vector3*, Point2*, const Point3& ) const {}
		Scalar GetArea() const { return 0; }
		const BoundingBox getBoundingBox() const { return BoundingBox(); }

		// IBasicTransform
		void ClearAllTransforms() {}
		void FinalizeTransformations() {}
		Matrix4 const GetFinalTransformMatrix() const { return Matrix4(); }
		Matrix4 const GetFinalInverseTransformMatrix() const { return Matrix4(); }

	protected:
		~StubObject() {}
	};

	/// Create an IORStack initialized with a default environment IOR
	/// and the given stub object set as the current object, matching
	/// how the rendering pipeline prepares the stack before calling
	/// SPF::Scatter.
	inline IORStack MakeTestIORStack( const IObject* pObj, Scalar envIOR = 1.0 )
	{
		IORStack stack( envIOR );
		stack.SetCurrentObject( pObj );
		return stack;
	}
}

#endif
