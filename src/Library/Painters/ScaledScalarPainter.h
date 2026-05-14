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
		};
	}
}

#endif
