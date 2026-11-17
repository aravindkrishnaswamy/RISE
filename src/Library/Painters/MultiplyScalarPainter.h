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

			//! Structural introspection: operands.
			const IScalarPainter* GetA() const { return pA; }
			const IScalarPainter* GetB() const { return pB; }

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

			//! DL-292: a composite of world-position fields and
			//! position-independent operands is a world-position field
			//! (IScalarPainter.h, CompositeIsWorldPositionField).  Before
			//! DL-292 this composite did not forward the question, so a
			//! graded `ior` built from it kept the pre-DL-09 accounting.
			bool IsWorldPositionField() const override
			{
				return CompositeIsWorldPositionField( pA, pB, HasPerChannelVariation() );
			}
			bool IsPositionIndependent() const override { return CompositeIsPositionIndependent( pA, pB ); }
			bool ReadsWorldPosition() const override { return CompositeReadsWorldPosition( pA, pB ); }

			//! DL-09/precision-slice P2-1: forward the single-scalar-slot
			//! view through the composite (see ScaledScalarPainter.h's
			//! sibling comment for the full rationale). Only an operand
			//! that ITSELF varies per-channel needs its own view -- an
			//! operand that doesn't is used as-is. Returns `nullptr` if
			//! either present, per-channel-varying operand has no view
			//! of its own.
			IScalarPainter* MakeSingleScalarSlotView() const override
			{
				if( !pA && !pB ) return nullptr;

				bool ownsA = false;
				IScalarPainter* va = pA;
				if( pA && pA->HasPerChannelVariation() ) {
					va = pA->MakeSingleScalarSlotView();
					if( !va ) return nullptr;
					ownsA = true;
				}
				bool ownsB = false;
				IScalarPainter* vb = pB;
				if( pB && pB->HasPerChannelVariation() ) {
					vb = pB->MakeSingleScalarSlotView();
					if( !vb ) {
						if( ownsA ) va->release();
						return nullptr;
					}
					ownsB = true;
				}

				IScalarPainter* view = new MultiplyScalarPainter( va, vb );
				if( ownsA ) va->release();
				if( ownsB ) vb->release();
				return view;
			}
		};
	}
}

#endif
