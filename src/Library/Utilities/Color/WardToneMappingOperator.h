//////////////////////////////////////////////////////////////////////
//
//  WardToneMappingOperator.h - A one color operator that converts
//  the given color to its equivelent value given a particular
//  scotopic luminance level.
// 
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 25, 2002
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef WARD_TONE_MAPPING_OPERATOR_
#define WARD_TONE_MAPPING_OPERATOR_

#include "IOneColorOperator.h"

namespace RISE
{
	class WardToneMappingOperator : public IOneColorOperator
	{
	protected:
		// Scotopic luminance is the number of photons absorbed by the rod photoreceptors to
		// give a criterion psychophysical result
		// For a biological explanation look here:
		// http://webvision.med.utah.edu/facts.html
		// Nifty scotopic luminances:
		// Starlight: 0.001
		// Moonlight: 0.1
		// Indoor Lighting: 100
		// Sunlight: 10.0
		// Maximum intensity of common CRT monitors: 100
		
		Chel		scotopic_luminance;

		// xw and yw are chromacities... we are going to use that of a typical 
		// monitor, which is 0.283 and 0.298 respectively
		Chel		xw, yw;

		// The Yw is the luminance characteristic of what we are mapping to, its units 
		// are cd^2/m and for a typical monitor is 100 cd^2/m
		Chel		Yw;

		// The Yi is the log-average of luminances in the computed image, it must be given
		// to us
		Chel		Yi;

		virtual ~WardToneMappingOperator(){};

	public:
		WardToneMappingOperator( const Chel scot_lum_, const Chel avgLum ) : 
		scotopic_luminance( scot_lum_ ), 
		xw( 0.283 ),
		yw( 0.298 ),
		Yw( 100.0 ),
		Yi( avgLum )
		{}

		bool PerformOperation( RISEColor& c )
		{
			xyYPel	p( c.base );

			//
			// The algorithm given here was taken directly from Peter Shirley's Realistic Ray Tracing
			// page 152, which he claims is based on Greg Ward's work
			//

			Scalar	s = 0;

			Scalar& x = p.x;
			Scalar& y = p.y;
			Scalar& Y = p.Y;

			// If luminance is zero, its zero, ain't nothing gonna stop that...
			if( Y > -NEARZERO && Y < NEARZERO ) {
				return true;
			}

			Scalar	log_Y = log10(Y);

			if( log_Y > -2.0 && log_Y < 0.6 ) {
				const Scalar temp = (log_Y + 2.0) / 2.6;
				s = (3.0 * (temp*temp)) - (2.0 * (temp*temp*temp));
			} else if( log_Y > 0.6 ) {
				s = 1.0;
			}
			// Otherwise it is 0

			x = (1.0-s)*xw + s*(x + xw - 0.33);
			y = (1.0-s)*yw + s*(y + yw - 0.33);
			Y = 0.4468*(1.0-s)*scotopic_luminance + s*Y;

			// Scale the luminance
			const Scalar temp = (1.219 + pow((Yw/2.0),0.4)) / (1.219 + pow(Yi,0.4));
			Y = (Y/Yw) * pow(temp, 2.5);

			// And we're done
			c.base = RISEPel( p );

			return true;
		}

	};
}

#endif