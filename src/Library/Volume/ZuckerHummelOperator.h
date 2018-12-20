//////////////////////////////////////////////////////////////////////
//
//  ZuckerHummelOperator.cpp - Implementation of the Zucker-Hummell operator
//    class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 22, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef _ZUCKERHUMMELOPERATOR
#define _ZUCKERHUMMELOPERATOR

#include "../Interfaces/IGradientEstimator.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	class ZuckerHummelOperator :
		public virtual IGradientEstimator,
		public virtual Implementation::Reference
	{
	public:
		ZuckerHummelOperator( ){};
		virtual ~ZuckerHummelOperator( ){};

		GRADIENT ComputeGradient( 
			const IVolumeAccessor* pVolume, 
			const Point3& ptRayInVolume 
			) const
		{
			const Scalar& x = ptRayInVolume.x;
			const Scalar& y = ptRayInVolume.y;
			const Scalar& z = ptRayInVolume.z;

			static const Scalar k1 = sqrt( 3.0 ) / 3.0;
			static const Scalar k2 = sqrt( 2.0 ) / 2.0;

			GRADIENT	grad;

			// The Zucker-Hummel surface detection operator is defined as follows:
			//
			// ref: http://www.artree.com/rakesh/papers/spie-01.pdf
			//      The reference contains a few errors, mostly stuff that should be +1s being -1s and so on, I have
			//      fixed these. 
			grad.vNormal.x = 
				pVolume->GetValue(x+1,y,z) - pVolume->GetValue(x-1,y,z) + 
				k1 * ( pVolume->GetValue(x+1,y+1,z+1) - pVolume->GetValue(x-1,y+1,z+1) + pVolume->GetValue(x+1,y-1,z+1) - pVolume->GetValue(x-1,y-1,z+1) + 
					pVolume->GetValue(x+1,y+1,z-1) - pVolume->GetValue(x-1,y+1,z-1) + pVolume->GetValue(x+1,y-1,z-1) - pVolume->GetValue(x-1,y-1,z-1) ) + 
				k2 * ( pVolume->GetValue(x+1,y,z+1) - pVolume->GetValue(x-1,y,z+1) + pVolume->GetValue(x+1,y+1,z) - pVolume->GetValue(x-1,y+1,z) + 
					pVolume->GetValue(x+1,y,z-1) - pVolume->GetValue(x-1,y,z-1) + pVolume->GetValue(x+1,y-1,z) - pVolume->GetValue(x-1,y-1,z) );

			grad.vNormal.y = 
				pVolume->GetValue(x,y+1,z) - pVolume->GetValue(x,y-1,z) + 
				k1 * ( pVolume->GetValue(x-1,y+1,z+1) - pVolume->GetValue(x-1,y-1,z+1) + pVolume->GetValue(x+1,y+1,z+1) - pVolume->GetValue(x+1,y-1,z+1) + 
					pVolume->GetValue(x-1,y+1,z-1) - pVolume->GetValue(x-1,y-1,z-1) + pVolume->GetValue(x+1,y+1,z-1) - pVolume->GetValue(x+1,y-1,z-1) ) + 
				k2 * ( pVolume->GetValue(x,y+1,z+1) - pVolume->GetValue(x,y-1,z+1) + pVolume->GetValue(x+1,y+1,z) - pVolume->GetValue(x+1,y-1,z) + 
					pVolume->GetValue(x,y+1,z-1) - pVolume->GetValue(x,y-1,z-1) + pVolume->GetValue(x-1,y+1,z) - pVolume->GetValue(x-1,y-1,z) );

			grad.vNormal.z = 
				pVolume->GetValue(x,y,z+1) - pVolume->GetValue(x,y,z-1) + 
				k1 * ( pVolume->GetValue(x-1,y+1,z+1) - pVolume->GetValue(x-1,y+1,z-1) + pVolume->GetValue(x+1,y+1,z+1) - pVolume->GetValue(x+1,y+1,z-1) + 
					pVolume->GetValue(x-1,y-1,z+1) - pVolume->GetValue(x-1,y-1,z-1) + pVolume->GetValue(x+1,y-1,z+1) - pVolume->GetValue(x+1,y-1,z-1) ) + 
				k2 * ( pVolume->GetValue(x,y+1,z+1) - pVolume->GetValue(x,y+1,z-1) + pVolume->GetValue(x,y-1,z+1) - pVolume->GetValue(x,y-1,z-1) + 
					pVolume->GetValue(x+1,y,z+1) - pVolume->GetValue(x+1,y,z-1) + pVolume->GetValue(x-1,y,z+1) - pVolume->GetValue(x-1,y,z-1) );


			grad.dMagnitude = Vector3Ops::NormalizeMag( grad.vNormal );

			return grad;
		}
	};
}

#endif

