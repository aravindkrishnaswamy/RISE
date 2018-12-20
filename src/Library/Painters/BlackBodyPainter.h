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
		class BlackBodyPainter : public virtual Painter
		{
		protected:
			RISEPel					color;					///< Color in RISEPel terms
			SpectralPacket			spectrum;				///< The actual spectrum (no scale)
			Scalar					temperature;			///< Temporature in Kelvins
			Scalar					scale;					///< A scale factor

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
			// Constructor based on temperature of blackbody
			BlackBodyPainter( const Scalar temp, const Scalar lambda_begin, const Scalar lambda_end, const unsigned int num_freq, const bool normalize, const Scalar scale );

			// Constructor based on the peak wavelength
	//		BlackBodyPainter( const Scalar peak_lambda, const Scalar lambda_begin, const Scalar lambda_end, const unsigned int num_freq, const Scalar scale=1.0 );
			
			RISEPel							GetColor( const RayIntersectionGeometric& ri  ) const;
			SpectralPacket					GetSpectrum( const RayIntersectionGeometric& ri ) const;
			Scalar							GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
			void RegenerateData( );
		};
	}
}

#endif
