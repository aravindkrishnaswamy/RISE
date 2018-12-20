//////////////////////////////////////////////////////////////////////
//
//  VolumeOp_MIP.cpp - Implementation of the MIP composite operator
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 22, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////



#ifndef _VOLOP_MIP
#define _VOLOP_MIP

#include "../Interfaces/IVolumeOperation.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	class VolumeOp_MIP : 
		public virtual IVolumeOperation,
		public virtual Implementation::Reference
	{
	protected:
		Scalar		highestIntensity;
		
	public:
		VolumeOp_MIP( ){};
		virtual ~VolumeOp_MIP( ){};

		void BeginOperation( unsigned int num_samples, const IVolumeAccessor* pVolume )
		{
			highestIntensity = 0;
		}

		void EndOperation( )
		{
		}

		CompOpRet DoIt(
			const Scalar voxel_value,
			const Point3& ptInVolume )
		{
			if( voxel_value > highestIntensity ) {
				highestIntensity = voxel_value;

				if( voxel_value == 1.0 ) {
					return Stop;
				}
			}
			return Continue;
		}

		CompOpRet ApplyComposite( 
			RISEColor&,
			const Scalar voxel_value, 
			const TransferFunctions*, 
			const Point3& ptInVolume
			)
		{	
			return DoIt( voxel_value, ptInVolume );
		}

		CompOpRet ApplyCompositeNM( 
			SpectralColor& Pel,
			const Scalar nm,
			const Scalar voxel_value, 
			const SpectralTransferFunctions* pTransferFunc, 
			const Point3& ptInVolume 
			)
		{
			return DoIt( voxel_value, ptInVolume );
		}

		bool doTransferFunctionAtEnd( ){ return true; }
		Scalar getSelectedVoxelValue(){ return highestIntensity; }
	};
}

#endif

