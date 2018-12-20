//////////////////////////////////////////////////////////////////////
//
//  VolumeInterpolatorHelper.h - Helper for volume interpolators
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 22, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


#ifndef IVOLUMEACCESSOR_HELPER_
#define IVOLUMEACCESSOR_HELPER_

#include "../Interfaces/IVolumeAccessor.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	class VolumeAccessorHelper : 
		public virtual IVolumeAccessor,
		public virtual Implementation::Reference
	{
	protected:
		const IVolume*		pVolume;

		VolumeAccessorHelper( ){ pVolume = 0; }
		virtual ~VolumeAccessorHelper( ){ ReleaseVolume( ); }

	public:
		void BindVolume( const IVolume* pVol )
		{
			if( pVol ) {
				ReleaseVolume( );

				pVol->addref();
				pVolume = pVol;
			}
		}

		void ReleaseVolume( )
		{
			safe_release( pVolume );
		}
	};
}

#endif

