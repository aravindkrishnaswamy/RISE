//////////////////////////////////////////////////////////////////////
//
//  ICompositeOperator.cpp - Interface to a composite operator
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 22, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////



#ifndef _IVOLUMEOPERATION
#define _IVOLUMEOPERATION

#include "IReference.h"
#include "IVolumeAccessor.h"
#include "../Volume/TransferFunctions.h"

namespace RISE
{
	class IVolumeOperation : 
		public virtual IReference
	{
	public:
		// Returns codes from the ApplyComposite function for composite operations
		// these codes tell what the caller should do next, like move on to the
		// next sample point, stop sampling and so on...
		enum CompOpRet
		{
			Continue =						0,	// do nothing just continue
			Stop =							2,	// stop after this PEL
			StopApplyLighting =					4	// apply lighting to the current PEL value and then stop
		};

		typedef std::pair<Scalar,Scalar> SpectralColor;

	protected:
		IVolumeOperation( ){};
		virtual ~IVolumeOperation( ){};

	public:
		virtual void BeginOperation( 
			unsigned int num_samples, 
			const IVolumeAccessor* pVolume 
			) = 0;

		virtual CompOpRet ApplyComposite( 
			RISEColor& pPel,
			const Scalar voxel_value, 
			const TransferFunctions* pTransferFunc, 
			const Point3& ptInVolume 
			) = 0;

		virtual CompOpRet ApplyCompositeNM( 
			SpectralColor& pPel,
			const Scalar nm,
			const Scalar voxel_value, 
			const SpectralTransferFunctions* pTransferFunc, 
			const Point3& ptInVolume 
			) = 0;

		virtual void EndOperation( ) = 0;

		virtual bool doTransferFunctionAtEnd( ) = 0;

		// This only needs to be implemented if doTransferFunctionAtEnd returns true
		virtual Scalar getSelectedVoxelValue() = 0;
	};
}

#endif

