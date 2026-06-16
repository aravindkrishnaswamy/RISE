//////////////////////////////////////////////////////////////////////
//
//  Function2DColorPainter.h - Spatially-varying COLOUR painter that
//    evaluates an IFunction2D at the hit's UV and returns it as a
//    greyscale RISEPel (r = g = b = bias + scale * f(u, v)).
//
//  The colour analogue of Function2DScalarPainter: where the scalar
//  wrapper feeds physical-scalar slots (IScalarPainter, no JH uplift),
//  this wrapper feeds COLOUR slots (IPainter) so any procedural 2D
//  field becomes a greyscale texture or a blend_painter mask.  Built
//  for the guilloché spall mask (a radial smoothstep through the
//  flaking temperature) driving the matte-scale blend, but general:
//  any IFunction2D (Perlin, Worley, polynomial, composite, guilloché)
//  drops in.  GetColorNM returns a FLAT spectral response (the value
//  itself), so a [0,1] mask stays neutral under spectral rendering.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FUNCTION2D_COLOR_PAINTER_
#define FUNCTION2D_COLOR_PAINTER_

#include "Painter.h"
#include "../Interfaces/IFunction2D.h"
#include "../Intersection/RayIntersectionGeometric.h"

namespace RISE
{
	namespace Implementation
	{
		class Function2DColorPainter : public Painter
		{
		protected:
			IFunction2D* const pFunc;
			const Scalar scale;
			const Scalar bias;
			virtual ~Function2DColorPainter()
			{
				if( pFunc ) pFunc->release();
			}

		public:
			//! greyscale out = bias + scale * f(u, v)
			explicit Function2DColorPainter( IFunction2D* p, const Scalar scale_ = Scalar(1), const Scalar bias_ = Scalar(0) ) :
				pFunc( p ), scale( scale_ ), bias( bias_ )
			{
				if( pFunc ) pFunc->addref();
			}

			RISEPel GetColor( const RayIntersectionGeometric& ri ) const override
			{
				if( !pFunc ) return RISEPel( 0, 0, 0 );
				const Scalar v = bias + scale * pFunc->Evaluate( ri.ptCoord.x, ri.ptCoord.y );
				return RISEPel( v, v, v );
			}

			Scalar GetColorNM( const RayIntersectionGeometric& ri, const Scalar ) const override
			{
				if( !pFunc ) return Scalar(0);
				return bias + scale * pFunc->Evaluate( ri.ptCoord.x, ri.ptCoord.y );
			}

			Scalar Evaluate( const Scalar x, const Scalar y ) const override
			{
				if( !pFunc ) return Scalar(0);
				return bias + scale * pFunc->Evaluate( x, y );
			}

			// IKeyframable: this wrapper is a static derived view of its
			// source function, so it is not independently keyframable.
			IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) override { return 0; }
			void SetIntermediateValue( const IKeyframeParameter& ) override {}
			void RegenerateData() override {}
		};
	}
}

#endif
