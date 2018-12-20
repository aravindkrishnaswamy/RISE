//////////////////////////////////////////////////////////////////////
//
//  VolumeOp_GradISO.cpp - Implementation of the gradient ISO operator
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 22, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////



#ifndef _VOLOP_GRADISO
#define _VOLOP_GRADISO

#include "../Interfaces/IVolumeOperation.h"
#include "../Interfaces/IGradientEstimator.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	class VolumeOp_GradISO : 
		public virtual IVolumeOperation,
		public virtual Implementation::Reference
	{
	protected:
		Scalar	m_dThresholdStart;
		Scalar	m_dThresholdEnd;

		const IGradientEstimator* m_pGrad;
		const IVolumeAccessor*	m_pVolume;


	public:
		VolumeOp_GradISO( 
			Scalar dThresholdStart, 
			Scalar dThresholdEnd, 
			const IGradientEstimator* pGrad 
			) :
		m_dThresholdStart( dThresholdStart ),
		m_dThresholdEnd( dThresholdEnd ),
		m_pGrad( pGrad )
		{
			if( m_pGrad ) {
				m_pGrad->addref();
			}
		};

		virtual ~VolumeOp_GradISO( )
		{
			safe_release( m_pGrad );
		};

		void BeginOperation( unsigned int num_samples, const IVolumeAccessor* pVolume )
		{
			if( pVolume )
			{
				m_pVolume = pVolume;
				m_pVolume->addref();
			}
		}

		void EndOperation( )
		{
			safe_release( m_pVolume );
		}

		CompOpRet ApplyComposite( 
			RISEColor& Pel,
			const Scalar voxel_value, 
			const TransferFunctions* pTransferFunc, 
			const Point3& ptInVolume
			)
		{
			IGradientEstimator::GRADIENT grad = m_pGrad->ComputeGradient( m_pVolume, ptInVolume );

			if( grad.dMagnitude >= m_dThresholdStart && grad.dMagnitude <= m_dThresholdEnd )
			{
				Pel = RISEColor( 1.0, 1.0, 1.0, 1.0 );
	//			Pel = pTransferFunc.ComputeColorFromIntensity( voxel_value );
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
			IGradientEstimator::GRADIENT grad = m_pGrad->ComputeGradient( m_pVolume, ptInVolume );

			if( grad.dMagnitude >= m_dThresholdStart && grad.dMagnitude <= m_dThresholdEnd )
			{
				Pel.first = 1.0;
				Pel.second = 1.0;
	//			Pel = pTransferFunc.ComputeColorFromIntensity( voxel_value );
				return StopApplyLighting;
			}
			
			return Continue;
		}

		bool doTransferFunctionAtEnd( ){ return false; }
		Scalar getSelectedVoxelValue(){ return 0; }

		void SetThresholds( 
			Scalar dThresholdStart, 
			Scalar dThresholdEnd 
			)
		{
			m_dThresholdStart = dThresholdStart;
			m_dThresholdEnd = dThresholdEnd;
		}

		void SetGradientEstimator( 
			IGradientEstimator* pGrad 
			)
		{
			safe_release( m_pGrad );

			if( pGrad )
			{
				m_pGrad = pGrad;
				m_pGrad->addref();
			}
		}
	};
}

#endif

