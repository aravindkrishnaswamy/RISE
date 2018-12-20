//////////////////////////////////////////////////////////////////////
//
//  VolumeOp_Composite.cpp - Implementation of the composite operator
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 22, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


#ifndef _VOLOP_COMPOSITE
#define _VOLOP_COMPOSITE

#include "../Interfaces/IVolumeOperation.h"
#include "../Utilities/Color/CompositeOperator.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	class VolumeOp_Composite :
		public virtual IVolumeOperation,
		public virtual Implementation::Reference
	{
	public:
		VolumeOp_Composite( ){};
		virtual ~VolumeOp_Composite( ){};

		void BeginOperation( unsigned int num_samples, const IVolumeAccessor* pVolume )
		{
		}

		void EndOperation( )
		{
		}

		CompOpRet ApplyComposite( 
			RISEColor& Pel,
			const Scalar voxel_value, 
			const TransferFunctions* pTransferFunc, 
			const Point3& ptInVolume
			)
		{
			const RISEColor p = pTransferFunc?pTransferFunc->ComputeColorFromIntensity( voxel_value ):RISEColor(1,1,1,1);

			Pel = CompositeOperator::Composite( Pel, p );

			// With front to back compositing we can check our resultant alpha and if it is really close to one
			// we can simply stop
			if( Pel.a >= 0.99 ) {
				return Stop;
			}

			return Continue;
		}

		CompOpRet ApplyCompositeNM( 
			SpectralColor& Pel,
			const Scalar nm,
			const Scalar voxel_value, 
			const SpectralTransferFunctions* pTransferFunc, 
			const Point3& ptInVolume 
			)
		{
			// Compute the PEL
			SpectralColor p;
			p.first = pTransferFunc?pTransferFunc->ComputeAlphaFromIntensity( voxel_value ) : 1.0;
			p.second = pTransferFunc?pTransferFunc->ComputeSpectralIntensityFromVolumeIntensity( nm, voxel_value ) : 1.0;

			
			{
				SpectralColor dest;

				// Apply the composite
				if( Pel.first == 1.0 || p.first == 0.0 ) {
					dest = Pel;
				} else if( Pel.first == 0.0 ) {
					dest = p;
				} else if( p.first == 1.0 ) {
					const Scalar OMAlpha = 1.0 - Pel.first;
					dest.second = p.second * OMAlpha + Pel.second * Pel.first;
					dest.first = 1.0;
				} else {
					const Scalar temp = p.first * (1.0-Pel.first);
					const Scalar alpha = dest.first = temp + Pel.first;
					dest.second = (Pel.second*Pel.first + p.second*temp) * (1.0/alpha);
					dest.first = alpha;
				}

				Pel = dest;
			}
			
			// With front to back compositing we can check our resultant alpha and if it is really close to one
			// we can simply stop
			if( Pel.first >= 0.99 ) {
				return Stop;
			}

			return Continue;
		}

		bool doTransferFunctionAtEnd( ){ return false; }
		Scalar getSelectedVoxelValue(){ return 0; }
	};
}

#endif

