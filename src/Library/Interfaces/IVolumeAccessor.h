//////////////////////////////////////////////////////////////////////
//
//  IVolumeAccessor.h - Interface to a volume interpolator
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 22, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


#ifndef _IVOLUMEACCESSOR
#define _IVOLUMEACCESSOR

#include "IVolume.h"

namespace RISE
{
	class IVolumeAccessor : 
		public virtual IReference
	{
	protected:
		IVolumeAccessor(){};
		virtual ~IVolumeAccessor(){};

	public:
		virtual Scalar GetValue( Scalar x, Scalar y, Scalar z ) const = 0;
		virtual Scalar GetValue( int x, int y, int z ) const = 0;
		virtual void BindVolume( const IVolume* pVol ) = 0;
	};
}

#endif

