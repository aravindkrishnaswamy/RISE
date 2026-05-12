//////////////////////////////////////////////////////////////////////
//
//  CompositeFunction2DPainter.h - Composable Function2D painter.
//
//  Holds two operand Function2Ds (referenced by name through
//  Function2DManager) and combines them per a chosen binary operator
//  after per-operand weight and (u,v) affine transform.  Implements
//  both IPainter (colorA / colorB interpolated by the normalised
//  composite value) and IFunction2D (the scalar combination — the
//  hook DisplacedGeometry calls per mesh vertex).
//
//  Recursive composition: since the composite is itself an
//  IFunction2D, a composite can be referenced as a child of another
//  composite, letting scenes build arbitrary N-ary expression trees
//  out of binary primitives.
//
//  Operator semantics with weights wA, wB and child values A, B
//  sampled at the per-operand transformed (u,v):
//      sum:        wA*A + wB*B
//      product:    (wA*A) * (wB*B)
//      lerp:       (1-t)*wA*A + t*wB*B,  t ∈ [0,1]
//      max:        max(wA*A, wB*B)
//      min:        min(wA*A, wB*B)
//      difference: wA*A - wB*B
//  Final output: output_scale * result + output_offset.
//
//  Range guidance: composing functions with different intrinsic
//  ranges (e.g. Perlin ~[-1,1] + amplitude-1 polynomial) requires
//  either matching `weight_a`/`weight_b` per operand or a global
//  `output_scale` to bring the composite back to whatever
//  DisplacedGeometry's `disp_scale` expects.
//
//  Author: RISE contributors
//  Date of Birth: 2026-05-11
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef COMPOSITE_FUNCTION2D_PAINTER_
#define COMPOSITE_FUNCTION2D_PAINTER_

#include "Painter.h"
#include "../Interfaces/IFunction2D.h"

namespace RISE
{
	namespace Implementation
	{
		class CompositeFunction2DPainter : public Painter
		{
		public:
			enum CompositeOp
			{
				eSum         = 0,	///< wA*A + wB*B
				eProduct     = 1,	///< (wA*A) * (wB*B)
				eLerp        = 2,	///< (1-t)*wA*A + t*wB*B,  t ∈ [0,1]
				eMax         = 3,	///< max(wA*A, wB*B)
				eMin         = 4,	///< min(wA*A, wB*B)
				eDifference  = 5	///< wA*A - wB*B
			};

		protected:
			virtual ~CompositeFunction2DPainter();

			// Own color painters: blended by normalised Evaluate value.
			const IPainter&		colA;
			const IPainter&		colB;

			// Operand Function2Ds: their Evaluate values are combined.
			const IFunction2D&	fA;
			const IFunction2D&	fB;

			CompositeOp			op;

			// Per-operand affine transform on (u, v):
			//   child is sampled at  ( uvScale * (u, v) + uvOffset )
			Scalar				weightA;
			Scalar				uvScaleAU;
			Scalar				uvScaleAV;
			Scalar				uvOffsetAU;
			Scalar				uvOffsetAV;

			Scalar				weightB;
			Scalar				uvScaleBU;
			Scalar				uvScaleBV;
			Scalar				uvOffsetBU;
			Scalar				uvOffsetBV;

			// Lerp parameter (clamped to [0,1] at construction;
			// ignored unless op == eLerp).
			Scalar				lerpT;

			// Global affine remap applied AFTER the operator combines A, B.
			Scalar				outputScale;
			Scalar				outputOffset;

		public:
			CompositeFunction2DPainter(
				const IPainter&		colorA_,
				const IPainter&		colorB_,
				const IFunction2D&	childA_,
				const IFunction2D&	childB_,
				const CompositeOp	op_,
				const Scalar		weightA_,
				const Scalar		uvScaleAU_,
				const Scalar		uvScaleAV_,
				const Scalar		uvOffsetAU_,
				const Scalar		uvOffsetAV_,
				const Scalar		weightB_,
				const Scalar		uvScaleBU_,
				const Scalar		uvScaleBV_,
				const Scalar		uvOffsetBU_,
				const Scalar		uvOffsetBV_,
				const Scalar		lerpT_,
				const Scalar		outputScale_,
				const Scalar		outputOffset_ );

			CompositeFunction2DPainter( const CompositeFunction2DPainter& ) = delete;
			CompositeFunction2DPainter& operator=( const CompositeFunction2DPainter& ) = delete;

			RISEPel	GetColor( const RayIntersectionGeometric& ri ) const;
			Scalar	GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;

			//! IFunction2D — combines child operand values per op.
			Scalar	Evaluate( const Scalar x, const Scalar y ) const;

			//! Keyframable interface (no animated parameters in v1).
			IKeyframeParameter*	KeyframeFromParameters( const String& name, const String& value );
			void				SetIntermediateValue( const IKeyframeParameter& val );
			void				RegenerateData();
		};
	}
}

#endif
