//////////////////////////////////////////////////////////////////////
//
//  MultiplyScalarPainter.h - Composition operator: multiplies two
//    scalar painters element-wise on the ScalarTriple and per-
//    wavelength.  Lets authors combine spatial × spectral, or
//    spatial × per-channel, etc.
//
//  Common use: `multiply(texture_roughness, sellmeier_ior)` for a
//  spatially-modulated dispersive material.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 14, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MULTIPLY_SCALAR_PAINTER_
#define MULTIPLY_SCALAR_PAINTER_

#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class MultiplyScalarPainter :
			public virtual IScalarPainter,
			public virtual Reference
		{
		protected:
			IScalarPainter* const pA;
			IScalarPainter* const pB;
			virtual ~MultiplyScalarPainter()
			{
				if( pA ) pA->release();
				if( pB ) pB->release();
			}

		public:
			MultiplyScalarPainter( IScalarPainter* a, IScalarPainter* b )
				: pA( a ), pB( b )
			{
				if( pA ) pA->addref();
				if( pB ) pB->addref();
			}

			ScalarTriple GetValuesAt(
				const RayIntersectionGeometric& ri
				) const override
			{
				if( !pA || !pB ) return ScalarTriple();
				const ScalarTriple ta = pA->GetValuesAt( ri );
				const ScalarTriple tb = pB->GetValuesAt( ri );
				return ScalarTriple(
					ta.v[0] * tb.v[0],
					ta.v[1] * tb.v[1],
					ta.v[2] * tb.v[2]
					);
			}

			Scalar GetValueAtNM(
				const RayIntersectionGeometric& ri,
				Scalar nm
				) const override
			{
				if( !pA || !pB ) return Scalar( 0 );
				return pA->GetValueAtNM( ri, nm ) * pB->GetValueAtNM( ri, nm );
			}

			bool HasPerChannelVariation() const override
			{
				return ( pA && pA->HasPerChannelVariation() ) ||
				       ( pB && pB->HasPerChannelVariation() );
			}
		};
	}
}

#endif
