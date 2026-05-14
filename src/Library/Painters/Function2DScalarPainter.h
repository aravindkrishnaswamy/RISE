//////////////////////////////////////////////////////////////////////
//
//  Function2DScalarPainter.h - Spatially-varying scalar painter that
//    evaluates an IFunction2D at the hit's UV coordinates.
//
//  IFunction2D::Evaluate returns a Scalar, which is exactly what we
//  want here.  Procedural 2D functions (Perlin, Worley, polynomial,
//  composite) are all reusable as scalar painters via this wrapper.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 14, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FUNCTION2D_SCALAR_PAINTER_
#define FUNCTION2D_SCALAR_PAINTER_

#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/IFunction2D.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class Function2DScalarPainter :
			public virtual IScalarPainter,
			public virtual Reference
		{
		protected:
			IFunction2D* const pFunc;
			virtual ~Function2DScalarPainter()
			{
				if( pFunc ) pFunc->release();
			}

		public:
			explicit Function2DScalarPainter( IFunction2D* p ) : pFunc( p )
			{
				if( pFunc ) pFunc->addref();
			}

			ScalarTriple GetValuesAt(
				const RayIntersectionGeometric& ri
				) const override
			{
				if( !pFunc ) return ScalarTriple();
				// IFunction2D::Evaluate takes (x, y) — call sites in
				// IPainter-based painters that use Function2D pass
				// (ptCoord.x, ptCoord.y), and ptCoord is (u, v) in
				// RISE's UV convention.  Matches `TexturePainter`'s
				// implicit treatment.
				const Scalar v = pFunc->Evaluate( ri.ptCoord.x, ri.ptCoord.y );
				return ScalarTriple( v );
			}

			bool HasPerChannelVariation() const override { return false; }
		};
	}
}

#endif
