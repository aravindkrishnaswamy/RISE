//////////////////////////////////////////////////////////////////////
//
//  VolumeOp_Average.cpp - Implementation of the averaging composite
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


#ifndef _VOLOP_AVERAGE
#define _VOLOP_AVERAGE

#include "../Interfaces/IVolumeOperation.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	class VolumeOp_Average : 
		public virtual IVolumeOperation,
		public virtual Implementation::Reference
	{
	protected:
		Scalar	m_dOVSamples;

	public:
		VolumeOp_Average( ) : 
		m_dOVSamples( 1 ){};
		virtual ~VolumeOp_Average( ){};

		void BeginOperation( unsigned int num_samples, const IVolumeAccessor* pVolume )
		{
			m_dOVSamples = 1.0 / Scalar( num_samples );
		}

		void EndOperation( )
		{
			m_dOVSamples = 1.0;
		}

		CompOpRet ApplyComposite( 
			RISEColor& Pel,
			const Scalar voxel_value, 
			const TransferFunctions* pTransferFunc, 
			const Point3& ptInVolume
			)
		{
			// Compute the PEL
			const RISEColor p = pTransferFunc?pTransferFunc->ComputeColorFromIntensity( voxel_value ):RISEColor(1,1,1,1);
			Pel = Pel + (p * m_dOVSamples);

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
			
			Pel.first += (p.first * m_dOVSamples);
			Pel.second += (p.second * m_dOVSamples);

			return Continue;
		}

		bool doTransferFunctionAtEnd( ){ return false; }
		Scalar getSelectedVoxelValue(){ return 0; }
	};
}

#endif

