//////////////////////////////////////////////////////////////////////
//
//  BlackBodyPainter.h - Defines a black body painter, see below for
//    a full description of a what a black body does and how it
//    works
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 21, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BLACKBODY_PAINTER_
#define BLACKBODY_PAINTER_

#include "../Interfaces/IPainter.h"
#include "Painter.h"

namespace RISE
{
	namespace Implementation
	{
		//! `blackbody_painter`: Planck's law at `temperature`.
		//!
		//! Magnitude convention (DL-464, documented 2026-10-09):
		//! `GetColorNM(nm) = M_lambda(T, nm) * s`, where M_lambda is the
		//! HEMISPHERICAL spectral exitance of a blackbody, `2 pi h c^2 /
		//! (lambda^5 (exp(hc / (lambda k T)) - 1))`, in W m^-2 per METRE of
		//! wavelength (so ~1e13 at 6500 K in the visible), and `s` is the
		//! authored `scale` -- divided, when `normalize` is set, by
		//! M_lambda at the Wien peak `0.0029 / T` m, so the curve's PEAK
		//! (which may lie outside the visible band) equals `scale`.
		//! The ruling: `scale` multiplies the spectrum on EVERY path --
		//! `GetColorNM`, `GetRadianceNM`, `GetSpectrum` -- and the RGB
		//! views are the DL-396 projections of that same scaled spectrum
		//! (`GetColor`: reflectance under D65; `GetRadianceColor`: the
		//! source view the spectral film resolves).  Before DL-464
		//! `GetColor` was an unnormalised bin mean of the spectrum that,
		//! with `normalize`, was then max-channel normalised to 1 --
		//! ignoring `scale` entirely -- so RGB and spectral renders of a
		//! blackbody emitter disagreed by large factors.
		class BlackBodyPainter : public virtual Painter
		{
		protected:
			RISEPel					reflectanceRGB;			///< D65 reflectance view of the scaled SPD
			RISEPel					radianceRGB;			///< source view of the scaled SPD
			SpectralPacket			spectrum;				///< The scaled spectrum, binned
			Scalar					temperature;			///< Temporature in Kelvins
			Scalar					scale;					///< The authored (keyframable) scale factor
			Scalar					effectiveScale;			///< `scale`, divided by the peak when normalizing

			const Scalar			lambda_begin; 
			const Scalar			lambda_end; 
			const unsigned int		numfreq; 
			const bool				normalize;

			// Given temperature and lambda, gives the intensity
			// Uses Planck's radiation formula shown above
			static Scalar IntensityForWavelength( const Scalar T, const Scalar lambda );

			// Given tempierature, gives the total radiation output
			// Uses Stefan-Boltzmann's law
			static Scalar TotalRadiationOutput( const Scalar T );

			// Given a required wavelength as the peak, computes the temperature for which this is true
			// Uses Wien's displacement law
			static Scalar TemperatureFromPeakNM( const Scalar nm );

			// Given a temperature, what is the peak wavelength for it?
			static Scalar PeakNMFromTemperature( const Scalar T );

			virtual ~BlackBodyPainter();

		public:
			//! Planck's hemispherical spectral exitance (W m^-2 m^-1) at
			//! wavelength `lambda` in METRES.
			static Scalar SpectralExitance( const Scalar T, const Scalar lambda ) { return IntensityForWavelength( T, lambda ); }

			// Constructor based on temperature of blackbody
			BlackBodyPainter( const Scalar temp, const Scalar lambda_begin, const Scalar lambda_end, const unsigned int num_freq, const bool normalize, const Scalar scale );

			// Constructor based on the peak wavelength
	//		BlackBodyPainter( const Scalar peak_lambda, const Scalar lambda_begin, const Scalar lambda_end, const unsigned int num_freq, const Scalar scale=1.0 );
			
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
			//! For a blackbody in particular the SPD IS the physics --
			//! Planck's law at the authored temperature.  A blackbody bound
			//! to a luminaire's exitance is the canonical physically-authored
			//! emitter in RISE.
			Scalar							GetRadianceNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
			{
				return GetColorNM( ri, nm );
			}

			//! DL-165: Planck's-law SPD -- see IPainter::IsSpectrallyDefined's doc comment.
			bool							IsSpectrallyDefined() const { return true; }

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
			void RegenerateData( );
		};
	}
}

#endif
