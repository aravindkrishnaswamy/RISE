//////////////////////////////////////////////////////////////////////
//
//  IntermediateDifferenceOperator.cpp - Intermediate distance gradient
//    operator
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 22, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


#ifndef _INTERMEDIATEDIFFERENCEOPERATOR
#define _INTERMEDIATEDIFFERENCEOPERATOR

#include "../Interfaces/IGradientEstimator.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	class IntermediateDifferenceOperator : 
		public virtual IGradientEstimator,
		public virtual Implementation::Reference
	{
	public:
		IntermediateDifferenceOperator( ){};
		virtual ~IntermediateDifferenceOperator( ){};

		GRADIENT ComputeGradient( 
			const IVolumeAccessor* pVolume, 
			const Point3& ptRayInVolume 
			) const
		{
			const Scalar& x = ptRayInVolume.x;
			const Scalar& y = ptRayInVolume.y;
			const Scalar& z = ptRayInVolume.z;

			GRADIENT	grad;

			grad.vNormal = Vector3( 
				(pVolume->GetValue( x+1,   y,   z ) - pVolume->GetValue(   x,   y,   z ))*0.5,
				(pVolume->GetValue(   x, y+1,   z ) - pVolume->GetValue(   x,   y,   z ))*0.5,
				(pVolume->GetValue(   x,   y, z+1 ) - pVolume->GetValue(   x,   y,   z ))*0.5
				);

			grad.dMagnitude = Vector3Ops::NormalizeMag( grad.vNormal );

			return grad;
		}
	};
}

#endif

