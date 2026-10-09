//////////////////////////////////////////////////////////////////////
//
//  SpectralColorPainter.h - Defines a painter that paints some
//  uniform color which comes from a spectrum.  This is the only
//  painter that currently properly implements the GetColorNM function
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 14, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SPECTRAL_COLOR_PAINTER_
#define SPECTRAL_COLOR_PAINTER_

#include "Painter.h"

namespace RISE
{
	namespace Implementation
	{
		//! `spectral_painter`: a physical SPD F(lambda) given as a binned
		//! SpectralPacket (piecewise CONSTANT per bin, `ValueAtNM`).
		//!
		//! DL-464 (ruled 2026-10-09): `scale` multiplies the spectrum on
		//! EVERY path -- `GetColorNM`, `GetRadianceNM` and `GetSpectrum`
		//! return `scale * F` -- and the two RGB views are the DL-396
		//! projections of that scaled spectrum (`GetColor`: reflectance
		//! under D65; `GetRadianceColor`: the source view the spectral film
		//! resolves).  Before DL-464 `GetColor` was the unnormalised bin
		//! mean `scale * mean(cmf * F)` (about 0.35 x `scale` in Y for a
		//! flat unit SPD) while the NM path ignored `scale`, so RGB and
		//! spectral renders of the same scene disagreed by up to ~1.4x.
		class SpectralColorPainter : public Painter
		{
		protected:
			const SpectralPacket	spectrum;			///< scale * F
			RISEPel					reflectanceRGB;		///< D65 reflectance view of `spectrum`
			RISEPel					radianceRGB;		///< source view of `spectrum`

			virtual ~SpectralColorPainter();

		public:
			SpectralColorPainter( const SpectralPacket& spectrum_, const Scalar scale );

			RISEPel							GetColor( const RayIntersectionGeometric& ri  ) const;
			SpectralPacket					GetSpectrum( const RayIntersectionGeometric& ri ) const;
			Scalar							GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;
			RISEPel							GetRadianceColor( const RayIntersectionGeometric& ri ) const;

			//! PHYSICAL SPD -- pass through verbatim (Stage C slice 2).
			//! This painter's GetColorNM is not a Jakob-Hanika uplift of an
			//! RGB triple; it is an absolute spectral radiance/reflectance
			//! the author supplied.  Letting the IPainter default run would
			//! throw it away and re-uplift the RGB projection instead, which
			//! is exactly the class of bug the Hosek-Wilkie radiance map is
			//! kept off the painter path to avoid.
			Scalar							GetRadianceNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
			{
				return GetColorNM( ri, nm );
			}

			//! DL-165: this IS the spectral painter -- see IPainter::IsSpectrallyDefined's doc comment.
			bool							IsSpectrallyDefined() const { return true; }

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ){ return 0;};
			void SetIntermediateValue( const IKeyframeParameter& val ){};
			void RegenerateData( ){};
		};
	}
}

#endif


