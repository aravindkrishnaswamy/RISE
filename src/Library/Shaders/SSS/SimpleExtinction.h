//////////////////////////////////////////////////////////////////////
//
//  SimpleExtinction.h - Phenomenological extinction for SSS
//
//  A simple exp(-sigma * r) falloff used as an artist-friendly
//  alternative to the physically-based dipole model.  This is NOT
//  a normalized BSSRDF — it produces a dimensionless weight that
//  falls off exponentially with distance, which is why irrad_scale
//  must be tuned per-scene to achieve the desired look.
//
//  The extinction parameter directly controls the falloff rate
//  without any physical derivation from scattering/absorption.
//  Use DiffusionApproximationExtinction for physically-based SSS.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 21, 2005
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SIMPLE_EXTINCTION_H
#define SIMPLE_EXTINCTION_H

#include "../../Interfaces/ISubSurfaceExtinctionFunction.h"
#include "../../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class SimpleExtinction : 
			public virtual ISubSurfaceExtinctionFunction,
			public virtual Reference
		{
		protected:
			const RISEPel extinction;
			const Scalar geometric_scale;

		public:
			SimpleExtinction(
				const RISEPel extinction_,
				const Scalar geometric_scale_
				) : 
				extinction( extinction_ ),
				geometric_scale( geometric_scale_ )
			{
			}

			virtual ~SimpleExtinction()
			{
			}

			virtual Scalar GetMaximumDistanceForError( 
				const Scalar error
			) const
			{
				if( error > 0 ) {
					return -log(error) / (ColorMath::MinValue(extinction)*geometric_scale) / 1000.0;
				}

				return RISE_INFINITY;
			}

			RISEPel ComputeTotalExtinction(
				const Scalar distance
			) const
			{
																// Convert the distance from meters to mm
				return ColorMath::exponential(extinction*-distance * 1000.0 * geometric_scale);
			}
		};
	}
}

#endif

