//////////////////////////////////////////////////////////////////////
//
//  KeyframableHelper.cpp - Implementation of template specialized classes
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 31, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "KeyframableHelper.h"

namespace RISE
{
	namespace Implementation
	{
		/////////////////////////////
		// Vector3 specializations
		/////////////////////////////

		template<>
		void Parameter<Vector3>::Add( IKeyframeParameter& result, const IKeyframeParameter& a_, const IKeyframeParameter& b_ )
		{
			const Vector3 a = *(Vector3*)a_.getValue();
			const Vector3 b = *(Vector3*)b_.getValue();
			Vector3 res(a.x+b.x,a.y+b.y,a.z+b.z);
			result.setValue( &res );
		}

		template<>
		void Parameter<Vector3>::Subtract( IKeyframeParameter& result, const IKeyframeParameter& a_, const IKeyframeParameter& b_ )
		{
			const Vector3 a = *(Vector3*)a_.getValue();
			const Vector3 b = *(Vector3*)b_.getValue();
			Vector3 res(a.x-b.x,a.y-b.y,a.z-b.z);
			result.setValue( &res );
		}

		/////////////////////////////
		// Point3 specializations
		/////////////////////////////

		template<>
		void Parameter<Point3>::Add( IKeyframeParameter& result, const IKeyframeParameter& a_, const IKeyframeParameter& b_ )
		{
			const Point3 a = *(Point3*)a_.getValue();
			const Point3 b = *(Point3*)b_.getValue();
			Point3 res(a.x+b.x,a.y+b.y,a.z+b.z);
			result.setValue( &res );
		}

		template<>
		void Parameter<Point3>::Subtract( IKeyframeParameter& result, const IKeyframeParameter& a_, const IKeyframeParameter& b_ )
		{
			const Point3 a = *(Point3*)a_.getValue();
			const Point3 b = *(Point3*)b_.getValue();
			Point3 res(a.x-b.x,a.y-b.y,a.z-b.z);
			result.setValue( &res );
		}

		template<>
		void Parameter<Point3>::ScalarMult( IKeyframeParameter& result, const IKeyframeParameter& a_, const Scalar& t )
		{
			const Point3 a = *(Point3*)a_.getValue();
			Point3 res(a.x*t,a.y*t,a.z*t);
			result.setValue( &res );
		}
	}
}

