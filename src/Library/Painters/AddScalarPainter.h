//////////////////////////////////////////////////////////////////////
//
//  AddScalarPainter.h - Composition operator: weighted sum of two
//    scalar painters element-wise on the ScalarTriple and per-
//    wavelength -- `weightA*a + weightB*b`.  MultiplyScalarPainter's
//    additive sibling: where multiply composes two scalar fields
//    multiplicatively (spatial x spectral, etc.), add lets one field's
//    detail be ADDED on top of another's without dragging its whole
//    range through a product (e.g. a base roughness field plus a
//    small additive pore/kerf-mark detail field that should not zero
//    out where the base field is zero).
//
//  Common use: `add(bridge_roughness, pore_field)` -- a physically-
//  bridged base roughness with fine sub-pixel detail layered on top.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 6, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ADD_SCALAR_PAINTER_
#define ADD_SCALAR_PAINTER_

#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class AddScalarPainter :
			public virtual IScalarPainter,
			public virtual Reference
		{
		protected:
			IScalarPainter* const pA;
			IScalarPainter* const pB;
			const Scalar weightA;
			const Scalar weightB;
			virtual ~AddScalarPainter()
			{
				if( pA ) pA->release();
				if( pB ) pB->release();
			}

		public:
			AddScalarPainter( IScalarPainter* a, IScalarPainter* b, Scalar weight_a = Scalar( 1.0 ), Scalar weight_b = Scalar( 1.0 ) )
				: pA( a ), pB( b ), weightA( weight_a ), weightB( weight_b )
			{
				if( pA ) pA->addref();
				if( pB ) pB->addref();
			}

			ScalarTriple GetValuesAt(
				const RayIntersectionGeometric& ri
				) const override
			{
				// Unlike MultiplyScalarPainter (where a missing operand
				// makes the whole product meaningless, so it zeroes the
				// result), a missing operand here just contributes
				// nothing -- 0 is addition's neutral element, so a null
				// pA or pB still yields the OTHER operand's (weighted)
				// value rather than collapsing the whole sum to zero.
				const ScalarTriple ta = pA ? pA->GetValuesAt( ri ) : ScalarTriple();
				const ScalarTriple tb = pB ? pB->GetValuesAt( ri ) : ScalarTriple();
				return ScalarTriple(
					weightA * ta.v[0] + weightB * tb.v[0],
					weightA * ta.v[1] + weightB * tb.v[1],
					weightA * ta.v[2] + weightB * tb.v[2]
					);
			}

			Scalar GetValueAtNM(
				const RayIntersectionGeometric& ri,
				Scalar nm
				) const override
			{
				const Scalar va = pA ? pA->GetValueAtNM( ri, nm ) : Scalar( 0 );
				const Scalar vb = pB ? pB->GetValueAtNM( ri, nm ) : Scalar( 0 );
				return weightA * va + weightB * vb;
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
