//////////////////////////////////////////////////////////////////////
//
//  SobelOperator.h - Interface to the Sobel 3D operator
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 22, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////



#ifndef _SOBELOPERATOR
#define _SOBELOPERATOR

#include "../Utilities/Reference.h"
#include "../Interfaces/IGradientEstimator.h"
#include "ConvolutionKernel.h"

namespace RISE
{
	class SobelOperator : 
		public virtual IGradientEstimator,
		public virtual Implementation::Reference
	{
	protected:
		ConvolutionKernel	convX, convY, convZ;

	public:
		SobelOperator( )
		{
			// Setup the convolution kernels
			// Start with x
			convX.mx[0] = Matrix3(
				-1, -3, -1, 
				-3, -6, -3,
				-1, -3, -1
				);

			convX.mx[1] = Matrix3(
				0,  0,  0,
				0,  0,  0,
				0,  0,  0
				);

			convX.mx[2] = Matrix3( 
				1,  3,  1,
				3,  6,  3,
				1,  3,  1
				);

			// Set the weight information
			convX.sumWeights = 22.0;
			convX.OVSumWeights = 1.0 / convX.sumWeights;

			// The Y kernel is a rotated X kernel
			convY.mx[0] = Matrix3(
				1,  3,  1,
				0,  0,  0,
				-1, -3, -1
				);

			convY.mx[1] = Matrix3(
				3,  6,  3,
				0,  0,  0,
				-3, -6, -3
				);

			convY.mx[2] = Matrix3(
				1,  3,  1,
				0,  0,  0,
				-1, -3, -1
				);

			// Set the weight information
			convY.sumWeights = 22.0;
			convY.OVSumWeights = 1.0 / convY.sumWeights;

			// The Z kernel is also the X kernel rotated
			convZ.mx[0] = Matrix3(
				-1,  0,  1,
				-3,  0,  3,
				-1,  0,  1
				);

			convZ.mx[1] = Matrix3(
				-3,  0,  3,
				-6,  0,  6,
				-3,  0,  3
				);

			convZ.mx[2] = Matrix3(
				-1,  0,  1,
				-3,  0,  3,
				-1,  0,  1
				);

			// Set the weight information
			convZ.sumWeights = 22.0;
			convZ.OVSumWeights = 1.0 / convZ.sumWeights;


		};

		virtual ~SobelOperator( ){};

		GRADIENT ComputeGradient( const IVolumeAccessor* pVolume, const Point3& ptRayInVolume ) const
		{
			GRADIENT	grad;

			grad.vNormal.x = convX.ApplyConvolution( pVolume, ptRayInVolume );
			grad.vNormal.y = convY.ApplyConvolution( pVolume, ptRayInVolume );
			grad.vNormal.z = convZ.ApplyConvolution( pVolume, ptRayInVolume );

			grad.dMagnitude = Vector3Ops::NormalizeMag( grad.vNormal );

			return grad;
		}
	};
}

#endif

