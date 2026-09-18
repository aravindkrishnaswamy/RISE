//////////////////////////////////////////////////////////////////////
//
//  Atan2ScalarPainter.h - Composition operator: two-argument
//    arctangent of two scalar painters, element-wise on the
//    ScalarTriple and per-wavelength -- atan2(y, x).
//
//  DL-17 (docs/DEBT_LEDGER.md, docs/CLOTH_FABRIC_DESIGN.md sec 15
//  item 12): built so a per-texel direction encoded in two texture
//  channels (glTF KHR_materials_anisotropy's R/G, remapped to
//  [-1, 1]) can be turned into a per-texel rotation angle:
//  `rotation = atan2(dir.y, dir.x)`, dir = normalize(2*RG - 1).
//  `y`/`x` are typically two `PainterChannelScalarPainter`s reading
//  the same texture's G / R channels with `scale 2 bias -1`.
//
//  No colourspace, no JH uplift (IScalarPainter contract) -- the
//  angle is a physical scalar by meaning, same footing as GGX's
//  `tangent_rotation_scalar` (DL-16) this feeds.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 17, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ATAN2_SCALAR_PAINTER_
#define ATAN2_SCALAR_PAINTER_

#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"
#include <cmath>

namespace RISE
{
	namespace Implementation
	{
		class Atan2ScalarPainter :
			public virtual IScalarPainter,
			public virtual Reference
		{
		protected:
			IScalarPainter* const pY;
			IScalarPainter* const pX;
			virtual ~Atan2ScalarPainter()
			{
				if( pY ) pY->release();
				if( pX ) pX->release();
			}

		public:
			Atan2ScalarPainter( IScalarPainter* y, IScalarPainter* x )
				: pY( y ), pX( x )
			{
				if( pY ) pY->addref();
				if( pX ) pX->addref();
			}

			//! Structural introspection: operands.
			const IScalarPainter* GetY() const { return pY; }
			const IScalarPainter* GetX() const { return pX; }

			ScalarTriple GetValuesAt(
				const RayIntersectionGeometric& ri
				) const override
			{
				if( !pY || !pX ) return ScalarTriple();
				const ScalarTriple ty = pY->GetValuesAt( ri );
				const ScalarTriple tx = pX->GetValuesAt( ri );
				return ScalarTriple(
					Scalar( atan2( double(ty.v[0]), double(tx.v[0]) ) ),
					Scalar( atan2( double(ty.v[1]), double(tx.v[1]) ) ),
					Scalar( atan2( double(ty.v[2]), double(tx.v[2]) ) )
					);
			}

			Scalar GetValueAtNM(
				const RayIntersectionGeometric& ri,
				Scalar nm
				) const override
			{
				if( !pY || !pX ) return Scalar( 0 );
				return Scalar( atan2(
					double( pY->GetValueAtNM( ri, nm ) ),
					double( pX->GetValueAtNM( ri, nm ) ) ) );
			}

			bool HasPerChannelVariation() const override
			{
				return ( pY && pY->HasPerChannelVariation() ) ||
				       ( pX && pX->HasPerChannelVariation() );
			}

			//! Same rationale as MultiplyScalarPainter.h's sibling
			//! method: only an operand that itself varies per-channel
			//! needs its own single-scalar-slot view.  Returns nullptr
			//! if either present, per-channel-varying operand has no
			//! view of its own.
			IScalarPainter* MakeSingleScalarSlotView() const override
			{
				if( !pY && !pX ) return nullptr;

				bool ownsY = false;
				IScalarPainter* vy = pY;
				if( pY && pY->HasPerChannelVariation() ) {
					vy = pY->MakeSingleScalarSlotView();
					if( !vy ) return nullptr;
					ownsY = true;
				}
				bool ownsX = false;
				IScalarPainter* vx = pX;
				if( pX && pX->HasPerChannelVariation() ) {
					vx = pX->MakeSingleScalarSlotView();
					if( !vx ) {
						if( ownsY ) vy->release();
						return nullptr;
					}
					ownsX = true;
				}

				IScalarPainter* view = new Atan2ScalarPainter( vy, vx );
				if( ownsY ) vy->release();
				if( ownsX ) vx->release();
				return view;
			}
		};
	}
}

#endif
