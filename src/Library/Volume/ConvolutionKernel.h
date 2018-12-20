//////////////////////////////////////////////////////////////////////
//
//  ConvolutionKernel.cpp - Implementation of a general R3 convolution
//    kernel
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 22, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////



#ifndef _CONVOLUTIONKERNEL
#define _CONVOLUTIONKERNEL

#include "../Interfaces/IVolumeAccessor.h"

namespace RISE
{
	class ConvolutionKernel
	{
	public:
		Matrix3		mx[3];
		Scalar		sumWeights;
		Scalar		OVSumWeights;

		Scalar		ApplyConvolution( const IVolumeAccessor* pVolume, const Point3& ptRayInVolume ) const
		{
			const Scalar&		x = ptRayInVolume.x;
			const Scalar&		y = ptRayInVolume.y;
			const Scalar&		z = ptRayInVolume.z;

			Scalar ret = mx[0]._02*pVolume->GetValue(x-1,y+1,z+1) + mx[1]._02*pVolume->GetValue(x,y+1,z+1) + mx[2]._02*pVolume->GetValue(x+1,y+1,z+1) + 
						mx[0]._12*pVolume->GetValue(x-1,y,z+1) + mx[1]._12*pVolume->GetValue(x,y,z+1) + mx[2]._12*pVolume->GetValue(x+1,y,z+1) +
						mx[0]._22*pVolume->GetValue(x-1,y-1,z+1) + mx[1]._22*pVolume->GetValue(x,y-1,z+1) + mx[2]._22*pVolume->GetValue(x+1,y-1,z+1) + 
						mx[0]._10*pVolume->GetValue(x-1,y+1,z) + mx[1]._10*pVolume->GetValue(x,y+1,z) + mx[2]._10*pVolume->GetValue(x+1,y+1,z) + 
						mx[0]._11*pVolume->GetValue(x-1,y,z) + mx[1]._11*pVolume->GetValue(x,y,z) + mx[2]._11*pVolume->GetValue(x+1,y,z) + 
						mx[0]._12*pVolume->GetValue(x-1,y-1,z) + mx[1]._12*pVolume->GetValue(x,y-1,z) + mx[2]._12*pVolume->GetValue(x+1,y-1,z) + 
						mx[0]._00*pVolume->GetValue(x-1,y+1,z-1) + mx[1]._00*pVolume->GetValue(x,y+1,z-1) + mx[2]._00*pVolume->GetValue(x+1,y+1,z-1) + 
						mx[0]._10*pVolume->GetValue(x-1,y,z-1) + mx[1]._10*pVolume->GetValue(x,y,z-1) + mx[2]._10*pVolume->GetValue(x+1,y,z-1) +
						mx[0]._20*pVolume->GetValue(x-1,y-1,z-1) + mx[1]._20*pVolume->GetValue(x,y-1,z-1) + mx[2]._20*pVolume->GetValue(x+1,y-1,z-1);

			return (ret * OVSumWeights);
		}
	};
}

#endif

