//////////////////////////////////////////////////////////////////////
//
//  UniformScalarPainter.h - A scalar painter that returns the same
//    scalar value at every position and every wavelength.
//
//  Used for inline numeric material parameters like `ior 1.5`,
//  `roughness 0.4`, `scattering 1000000`.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 14, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef UNIFORM_SCALAR_PAINTER_
#define UNIFORM_SCALAR_PAINTER_

#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class UniformScalarPainter :
			public virtual IScalarPainter,
			public virtual Reference
		{
		protected:
			const Scalar value;
			virtual ~UniformScalarPainter() {}

		public:
			explicit UniformScalarPainter( Scalar v ) : value( v ) {}

			ScalarTriple GetValuesAt(
				const RayIntersectionGeometric& /*ri*/
				) const override
			{
				return ScalarTriple( value );
			}

			Scalar GetValueAtNM(
				const RayIntersectionGeometric& /*ri*/,
				Scalar /*nm*/
				) const override
			{
				return value;
			}

			bool HasPerChannelVariation() const override { return false; }
		};
	}
}

#endif
