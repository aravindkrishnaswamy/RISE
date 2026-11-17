//////////////////////////////////////////////////////////////////////
//
//  Simplex3DPainter.h - Declaration of a painter that uses
//  3D simplex noise FBM to blend between two painters.
//  Produces smoother, more isotropic patterns than classic Perlin.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SIMPLEX_3D_PAINTER_
#define SIMPLEX_3D_PAINTER_

#include "Painter.h"
#include "../Noise/SimplexNoise.h"
#include "../Interfaces/ISimpleInterpolator.h"

namespace RISE
{
	namespace Implementation
	{
		class Simplex3DPainter : public Painter
		{
		protected:
			virtual ~Simplex3DPainter();

			const IPainter&			a;
			const IPainter&			b;

			Vector3					vScale;
			Vector3					vShift;

			Scalar					dPersistence;
			unsigned int			nOctaves;

			SimplexNoise3D*					pFunc;
			ISimpleInterpolator<Scalar>*	pInterp;
			ISimpleInterpolator<RISEPel>*	pColorInterp;

		public:
			Simplex3DPainter( const Scalar dPersistence, const unsigned int nOctaves, const IPainter& cA_, const IPainter& cB_, const Vector3& vScale_, const Vector3& vShift_ );

			RISEPel							GetColor( const RayIntersectionGeometric& ri  ) const;
			Scalar							GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;

			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
			void RegenerateData( );
			//! DL-203 (docs/DEBT_LEDGER.md): this class's `a`/`b` operands genuinely combine or select
			//! among their own TRUE `GetColorNM` (see this class's own GetColorNM),
			//! so this recurses -- see IPainter::IsSpectrallyDefined's own doc
			//! comment for the full rationale and which composites do NOT recurse.
			bool IsSpectrallyDefined() const { return a.IsSpectrallyDefined() || b.IsSpectrallyDefined(); }
		};
	}
}

#endif
