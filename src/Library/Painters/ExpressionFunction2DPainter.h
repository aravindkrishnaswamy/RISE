//////////////////////////////////////////////////////////////////////
//
//  ExpressionFunction2DPainter.h - A procedural 2D field whose value is
//  a math expression AUTHORED IN THE SCENE FILE (ExpressionEval).
//
//  Derives from Painter, so the same object is usable as:
//    - a displacement field  (Evaluate(u,v) -> displaced_geometry)
//    - a greyscale colour     (GetColor -> a colour slot / blend mask)
//    - a physical scalar      (via scalar_painter { function2d <name> })
//  i.e. the in-scene-scripted analogue of perlin2d / worley / the
//  guilloché field -- author the formula, no engine recompile.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef EXPRESSION_FUNCTION2D_PAINTER_
#define EXPRESSION_FUNCTION2D_PAINTER_

#include "Painter.h"
#include "ExpressionEval.h"
#include "../Intersection/RayIntersectionGeometric.h"

namespace RISE
{
	namespace Implementation
	{
		class ExpressionFunction2DPainter : public Painter
		{
		protected:
			ExpressionProgram m_prog;
			virtual ~ExpressionFunction2DPainter() {}

			static Scalar Safe( const Scalar v ) { return ExpressionProgram::IsFinite( v ) ? v : Scalar(0); }

		public:
			explicit ExpressionFunction2DPainter( const ExpressionProgram& prog ) : m_prog( prog ) {}

			RISEPel GetColor( const RayIntersectionGeometric& ri ) const override
			{
				const Scalar v = Safe( m_prog.Eval( ri.ptCoord.x, ri.ptCoord.y ) );
				return RISEPel( v, v, v );
			}

			Scalar GetColorNM( const RayIntersectionGeometric& ri, const Scalar ) const override
			{
				return Safe( m_prog.Eval( ri.ptCoord.x, ri.ptCoord.y ) );
			}

			Scalar Evaluate( const Scalar x, const Scalar y ) const override
			{
				return Safe( m_prog.Eval( x, y ) );
			}

			// IKeyframable: a compiled-expression field is not independently
			// keyframable (it has no scalar parameter to interpolate).
			IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) override { return 0; }
			void SetIntermediateValue( const IKeyframeParameter& ) override {}
			void RegenerateData() override {}
		};
	}
}

#endif
