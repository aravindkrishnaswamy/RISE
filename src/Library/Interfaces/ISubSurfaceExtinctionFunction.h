//////////////////////////////////////////////////////////////////////
//
//  ISubSurfaceExtinctionFunction.h - Interface to a subsurface
//    extinction function, which describes how light is attenuated
//    as it is scattered and absorbed in a medium
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 21, 2005
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ISUBSURFACE_EXTINCTION_FUNCTION_H
#define ISUBSURFACE_EXTINCTION_FUNCTION_H

#include "IReference.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/Color/Color.h"

namespace RISE
{
	class ISubSurfaceExtinctionFunction : 
		public virtual IReference
	{
	public:
		///! Returns the maximum distance to bother looking for 
		///! light with the given error tolerance
		virtual Scalar GetMaximumDistanceForError( 
			const Scalar error
			) const = 0;

		///! Computes the total extinction through the given distance
		virtual RISEPel ComputeTotalExtinction(
			const Scalar distance
			) const = 0;
	};
}

#endif

