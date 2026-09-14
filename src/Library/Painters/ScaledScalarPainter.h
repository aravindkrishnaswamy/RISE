//////////////////////////////////////////////////////////////////////
//
//  ScaledScalarPainter.h - Composition operator: wraps another
//    scalar painter and multiplies its output by a constant scale.
//
//  Use: `scalar_painter { name half_rgh base full_rgh scale 0.5 }`.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 14, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SCALED_SCALAR_PAINTER_
#define SCALED_SCALAR_PAINTER_

#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class ScaledScalarPainter :
			public virtual IScalarPainter,
			public virtual Reference
		{
		protected:
			IScalarPainter* const pChild;
			const Scalar          scale;
			virtual ~ScaledScalarPainter()
			{
				if( pChild ) pChild->release();
			}

		public:
			ScaledScalarPainter( IScalarPainter* p, Scalar s )
				: pChild( p ), scale( s )
			{
				if( pChild ) pChild->addref();
			}

			//! Structural introspection: child + scale.
			const IScalarPainter* GetChild() const { return pChild; }
			Scalar GetScale() const { return scale; }

			ScalarTriple GetValuesAt(
				const RayIntersectionGeometric& ri
				) const override
			{
				if( !pChild ) return ScalarTriple();
				const ScalarTriple t = pChild->GetValuesAt( ri );
				return ScalarTriple( t.v[0] * scale, t.v[1] * scale, t.v[2] * scale );
			}

			Scalar GetValueAtNM(
				const RayIntersectionGeometric& ri,
				Scalar nm
				) const override
			{
				return pChild ? pChild->GetValueAtNM( ri, nm ) * scale : Scalar( 0 );
			}

			bool HasPerChannelVariation() const override
			{
				return pChild ? pChild->HasPerChannelVariation() : false;
			}

			//! DL-09/precision-slice P2-1: forward the single-scalar-slot
			//! view through the composite instead of losing it. Before
			//! this, `scalar_painter { base <spectral curve> scale 1 }`
			//! bound to a `requireSingle` slot (e.g. `coated_material`'s
			//! `coat_ior`) hard-failed at derive time even though the
			//! SAME curve bound directly (no `scale` wrapper) resolved
			//! fine via `MakeSingleScalarSlotView` -- `HasPerChannelVariation`
			//! was already forwarded from `pChild` above, but nothing
			//! forwarded the view that makes a `requireSingle` binding
			//! survive it.
			//!
			//! Builds a view of the CHILD (only if the child itself is
			//! per-channel-varying -- if it already isn't, it needs no
			//! view and is used as-is) and wraps that in a fresh
			//! `ScaledScalarPainter` applying the same `scale`. Returns
			//! `nullptr` -- no view available -- when the child DOES
			//! vary per-channel but has no view of its own (an authored
			//! `values` triple, or a vec3-result `expression`): a
			//! composite cannot manufacture a single-scalar reading out
			//! of a channel triple that was independently AUTHORED
			//! rather than sampled from one curve.
			IScalarPainter* MakeSingleScalarSlotView() const override
			{
				if( !pChild ) return nullptr;
				IScalarPainter* childView = pChild;
				bool ownsChildView = false;
				if( pChild->HasPerChannelVariation() ) {
					childView = pChild->MakeSingleScalarSlotView();
					if( !childView ) return nullptr;
					ownsChildView = true;
				}
				IScalarPainter* view = new ScaledScalarPainter( childView, scale );
				if( ownsChildView ) childView->release();
				return view;
			}
		};
	}
}

#endif
