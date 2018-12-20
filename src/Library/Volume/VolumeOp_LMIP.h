//////////////////////////////////////////////////////////////////////
//
//  VolumeOp_Grad.cpp - Implementation of the LMIP composite operator
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 22, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


#ifndef _VOLOP_LMIP
#define _VOLOP_LMIP

#include "../Interfaces/IVolumeOperation.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	class VolumeOp_LMIP :
		public virtual IVolumeOperation,
		public virtual Implementation::Reference
	{
	protected:
		unsigned int		nBufferSize;
		unsigned int		nMiddleEntry;
		Scalar*				pSampleBuffer;

		Scalar				dThreshold;

		Scalar		highestIntensity;

		inline void BufferDown( )
		{
			// This shifts the buffer down by a unit
			for( unsigned int i=1; i<nBufferSize; i++ ) {
				pSampleBuffer[i-1] = pSampleBuffer[i];
			}
		}

		bool LocalMaximaCheck( )
		{
			// Checks the buffer for a local maxima
			// In order for there to be a local maxima the middle buffer
			// value MUST be greater than or equal to the left side, and
			// must be less than or equal to the right side, AND must be
			// strictly less than the last value and strictly greater than
			// the first value
			Scalar&			middle_value = pSampleBuffer[nMiddleEntry];

			unsigned int		i;

			if( nBufferSize > 3 )
			{
				for( i=0; i<nMiddleEntry; i++ ) {
					if( pSampleBuffer[i] > middle_value ) {
						return false;
					}
				}

				for( i=nMiddleEntry+1; i<nBufferSize; i++ ) {
					if( pSampleBuffer[i] < middle_value ) {
						return false;
					}
				}
			}

			// Boundary checks to ensure than < does not equal middle does not equal >
			if( pSampleBuffer[0] >= middle_value ) {
				return false;
			}

			if( pSampleBuffer[nBufferSize-1] <= middle_value ) {
				return false;
			}

			return true;
		}

	public:
		VolumeOp_LMIP(
			unsigned int buffer_size,
			Scalar threshold
			) :
		nBufferSize( buffer_size ),
		pSampleBuffer( 0 ),
		dThreshold( threshold )
		{
			if( nBufferSize > 0 ) {
				pSampleBuffer = new Scalar[nBufferSize];
			}
			nMiddleEntry = nBufferSize/2;
		};
		virtual ~VolumeOp_LMIP( )
		{
			safe_delete( pSampleBuffer );
		};

		void BeginOperation( unsigned int num_samples, const IVolumeAccessor* pVolume )
		{
			// Clear the buffer
			for( unsigned int i=0; i<nBufferSize; i++ ) {
				pSampleBuffer[i] = 0;
			}

			highestIntensity = 0;
		}

		void EndOperation( )
		{
		}

		CompOpRet DoIt(
			Scalar voxel_value,
			const Point3& ptInVolume )
		{
			// Remember the last n samples, and check the n/2 one to see if its
			// a local maximum thats greater than the given threshold, if it
			// is then set that to Intensity and return

			BufferDown( );
			pSampleBuffer[nBufferSize-1] = voxel_value;

			if( LocalMaximaCheck() )
			{
				// we have a local maxima...
				// check if its greater than the threshold
				if( pSampleBuffer[nMiddleEntry] >= dThreshold )
				{
					// Bingo!
					highestIntensity = pSampleBuffer[nMiddleEntry];
					return StopApplyLighting;
				}
			}

			// Other wise continue with the regular MIP algorithm

			if( voxel_value > highestIntensity )
			{
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

		void SetBufferAndThreshold( unsigned int buffer_size, Scalar threshold  )
		{
			nBufferSize = buffer_size;
			dThreshold = threshold;

			if( pSampleBuffer ) {
				delete pSampleBuffer;
				pSampleBuffer = 0;
			}

			if( nBufferSize > 0 ) {
				pSampleBuffer = new Scalar[nBufferSize];
			}
			nMiddleEntry = nBufferSize/2;
		}
	};
}

#endif
