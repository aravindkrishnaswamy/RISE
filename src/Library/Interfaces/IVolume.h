//////////////////////////////////////////////////////////////////////
//
//  IVolume.h - Interface to a volume
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 22, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////
	
#ifndef IVOLUME_
#define IVOLUME_

#include "IReference.h"

namespace RISE
{
	class IVolume : 
		public virtual IReference
	{
	protected:
		virtual ~IVolume( ){};

	public:
		IVolume( ){};

		virtual unsigned int Width( ) const = 0;
		virtual unsigned int Height( ) const = 0;	
		virtual unsigned int Depth( ) const = 0;
		virtual Scalar GetValue( const int x, const int y, const int z ) const = 0;
	};
}

#endif

