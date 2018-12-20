//////////////////////////////////////////////////////////////////////
//
//  VolumeOp_AlphaScaledComposite.cpp - Implementation of the composite operator
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 22, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


#ifndef _VolumeOp_AlphaScaledComposite
#define _VolumeOp_AlphaScaledComposite

#include "../Interfaces/IVolumeOperation.h"
#include "../Utilities/Color/CompositeOperator.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	class VolumeOp_AlphaScaledComposite :
		public virtual IVolumeOperation,
		public virtual Implementation::Reference
	{
	protected:
		Scalar	m_dOVSamples;

	public:
		VolumeOp_AlphaScaledComposite( ){};
		virtual ~VolumeOp_AlphaScaledComposite( ){};

		void BeginOperation( unsigned int num_samples, const IVolumeAccessor* pVolume )
		{
			m_dOVSamples = 1.0 / Scalar( num_samples );
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
			RISEColor p = pTransferFunc?pTransferFunc->ComputeColorFromIntensity( voxel_value ):RISEColor(1,1,1,1);
			p.a *= m_dOVSamples;

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

			p.first *= m_dOVSamples;
			
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

		inline bool doTransferFunctionAtEnd( ){ return false; }
		Scalar getSelectedVoxelValue(){ return 0; }
	};
}

#endif

