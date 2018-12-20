//////////////////////////////////////////////////////////////////////
//
//  IGradientEstimator.cpp - Interface to a gradient estimator
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 22, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


#ifndef IGRADIENTESTIMATOR_
#define IGRADIENTESTIMATOR_

#include "IVolumeAccessor.h"
#include "IReference.h"

namespace RISE
{
	class IGradientEstimator :
		public virtual IReference
	{
	public:
		struct GRADIENT
		{
			Vector3	vNormal;
			Scalar dMagnitude;
		};

	protected:
		IGradientEstimator( ){};
		virtual ~IGradientEstimator( ){};

	public:
		virtual GRADIENT ComputeGradient( 
			const IVolumeAccessor* pVolume, 
			const Point3& ptRayInVolume 
			)  const = 0;
		
	};
}

#endif

