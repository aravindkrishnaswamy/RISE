//////////////////////////////////////////////////////////////////////
//
//  VolumeOp_ISO.cpp - Implementation of the ISO surface rendering
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 22, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef _VOLOP_ISO
#define _VOLOP_ISO

#include "../Interfaces/IVolumeOperation.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	class VolumeOp_ISO : 
		public virtual IVolumeOperation,
		public virtual Implementation::Reference
	{
	protected:
		Scalar	m_dThresholdStart;
		Scalar	m_dThresholdEnd;

	public:
		VolumeOp_ISO( 
			Scalar dThresholdStart, 
			Scalar dThresholdEnd 
			) :
		m_dThresholdStart( dThresholdStart ),
		m_dThresholdEnd( dThresholdEnd )
		{};
		virtual ~VolumeOp_ISO( ){};

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
			if( voxel_value >= m_dThresholdStart && voxel_value <= m_dThresholdEnd )
			{
				Pel = pTransferFunc?pTransferFunc->ComputeColorFromIntensity( voxel_value ):RISEColor(1,1,1,1);
				return StopApplyLighting;
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
			if( voxel_value >= m_dThresholdStart && voxel_value <= m_dThresholdEnd )
			{
				Pel.first = pTransferFunc?pTransferFunc->ComputeAlphaFromIntensity( voxel_value ):1.0;
				Pel.second = pTransferFunc?pTransferFunc->ComputeSpectralIntensityFromVolumeIntensity( nm, voxel_value ):1.0;
				return StopApplyLighting;
			}
			
			return Continue;
		}

		bool doTransferFunctionAtEnd( ){ return false; }
		Scalar getSelectedVoxelValue(){ return 0; }

		void SetThresholds( Scalar dThresholdStart, Scalar dThresholdEnd )
		{
			m_dThresholdStart = dThresholdStart;
			m_dThresholdEnd = dThresholdEnd;
		}
	};
}

#endif

