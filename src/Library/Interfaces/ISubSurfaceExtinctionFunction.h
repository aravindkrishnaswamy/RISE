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

		///! DL-291: the same quantity for a body seen through a medium of
		///! index `exteriorIOR` (the IOR-stack top at the shading hit,
		///! `RayIntersectionGeometric::ambientIOR`; 1.0 = air).  A model
		///! whose diffusion boundary condition depends on the RELATIVE
		///! index (the dipole's / multipole's A = (1+Fdr)/(1-Fdr)) must
		///! override this; the default -- for models with no boundary term
		///! (SimpleExtinction) -- ignores the exterior.  At exteriorIOR == 1
		///! every override must return ComputeTotalExtinction( distance )
		///! exactly (the in-air bit-identity pin).
		virtual RISEPel ComputeTotalExtinctionForExterior(
			const Scalar distance,
			const Scalar /*exteriorIOR*/
			) const
		{
			return ComputeTotalExtinction( distance );
		}
	};
}

#endif

