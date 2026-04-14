//////////////////////////////////////////////////////////////////////
//
//  Wavelet3DPainter.h - Painter using 3D wavelet noise for
//  band-limited noise that maintains detail across all scales.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef WAVELET_3D_PAINTER_
#define WAVELET_3D_PAINTER_

#include "Painter.h"
#include "../Noise/WaveletNoise.h"
#include "../Interfaces/ISimpleInterpolator.h"

namespace RISE
{
	namespace Implementation
	{
		class Wavelet3DPainter : public Painter
		{
		protected:
			virtual ~Wavelet3DPainter();

			const IPainter&			a;
			const IPainter&			b;

			Vector3					vScale;
			Vector3					vShift;

			unsigned int			nTileSize;
			Scalar					dPersistence;
			unsigned int			nOctaves;

			WaveletNoise3D*					pFunc;
			ISimpleInterpolator<Scalar>*	pInterp;
			ISimpleInterpolator<RISEPel>*	pColorInterp;

		public:
			Wavelet3DPainter( const unsigned int nTileSize, const Scalar dPersistence, const unsigned int nOctaves, const IPainter& cA_, const IPainter& cB_, const Vector3& vScale_, const Vector3& vShift_ );

			RISEPel							GetColor( const RayIntersectionGeometric& ri  ) const;
			Scalar							GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;

			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
			void RegenerateData( );
		};
	}
}

#endif
