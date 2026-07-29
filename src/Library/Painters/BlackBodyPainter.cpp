//////////////////////////////////////////////////////////////////////
//
//  BlackBodyPainter.cpp - Implements the black body emitter class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 5, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "BlackBodyPainter.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/PlanckRadiance.h"
#include "../Utilities/PiecewiseLinearFunction.h"
#include "../Animation/KeyframableHelper.h"

using namespace RISE;
using namespace RISE::Implementation;

static const Scalar NM_to_M = 1e-9;			// Convert an expression in nanometers to meters

Scalar BlackBodyPainter::IntensityForWavelength( const Scalar T, const Scalar lambda )
{
	// The painter's historical spectral convention is hemispherical exitance
	// per metre of wavelength. The shared fire kernel is radiance per nm, so
	// keep both the pi angular projection and wavelength-unit conversion here
	// at the painter boundary.
	static const Scalar perNanometreToPerMetre = 1e9;
	return PI * perNanometreToPerMetre *
		PlanckSpectralRadianceNM( lambda / NM_to_M, T );
}

Scalar BlackBodyPainter::TotalRadiationOutput( const Scalar T )
{
	static const Scalar stefan_boltzmann_constant = 5.6051e-8;
	
	return (stefan_boltzmann_constant * pow( T, 4.0 ) );
}

Scalar BlackBodyPainter::TemperatureFromPeakNM( const Scalar nm )
{
	static const Scalar wien_constant = 2.8978e6;

	return (wien_constant/nm);
}

Scalar BlackBodyPainter::PeakNMFromTemperature( const Scalar T )
{
	return (0.0029/T);
}

BlackBodyPainter::BlackBodyPainter( 
		const Scalar temp, 
		const Scalar lambda_begin_, 
		const Scalar lambda_end_, 
		const unsigned int numfreq_, 
		const bool normalize_,
		const Scalar scale_ ) : 
  temperature( temp ),
  scale( scale_ ),
  lambda_begin( lambda_begin_ ),
  lambda_end( lambda_end_ ),
  numfreq( numfreq_ ),
  normalize( normalize_ )
{
	RegenerateData();
}

/*
BlackBodyPainter::BlackBodyPainter( const Scalar peak_lambda, const Scalar lambda_begin, const Scalar lambda_end, const unsigned int num_freq, const Scalar scale=1.0 )
{
	BlackBodyPainter::BlackBodyPainter( TemperatureFromPeakNM(peak_lambda), lambda_begin, lambda_end, num_freq, scale );
}
*/

BlackBodyPainter::~BlackBodyPainter( )
{
}

RISEPel BlackBodyPainter::GetColor( const RayIntersectionGeometric& ri  ) const
{
	return color;
}

SpectralPacket BlackBodyPainter::GetSpectrum( const RayIntersectionGeometric& ri ) const
{
	return spectrum;
}

Scalar BlackBodyPainter::GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	return IntensityForWavelength(temperature, nm*NM_to_M) * scale;
}


static const unsigned int TEMPERATURE_ID = 100;
static const unsigned int SCALE_ID = 101;

IKeyframeParameter* BlackBodyPainter::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "temperature" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), TEMPERATURE_ID );
	} else if( name == "scale" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), SCALE_ID );
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void BlackBodyPainter::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case TEMPERATURE_ID:
		{
			temperature = *(Scalar*)val.getValue();
		}
		break;
	case SCALE_ID:
		{
			scale = *(Scalar*)val.getValue();
		}
		break;
	}
}

void BlackBodyPainter::RegenerateData( )
{
	// Using planck's formula
	PiecewiseLinearFunction1D* pFunc = new PiecewiseLinearFunction1D();
	GlobalLog()->PrintNew( pFunc, __FILE__, __LINE__, "piecewise linear function 1D" );

	const Scalar delta = ( (lambda_end-lambda_begin) / Scalar(numfreq) );
	Scalar freq = lambda_begin;

	for( unsigned int i=0; i<numfreq; i++, freq += delta ) {
		pFunc->addControlPoint( std::make_pair( freq, IntensityForWavelength(temperature, freq*NM_to_M) * scale ) );
	}

	spectrum = SpectralPacket( lambda_begin, lambda_end, numfreq, pFunc );
	XYZPel cxyz = spectrum.GetXYZ();
	color = cxyz;

	// If we are to normalize, rescale the scale
	if( normalize ) {
		const Scalar maxima = IntensityForWavelength( temperature, PeakNMFromTemperature( temperature ) );
		scale /= maxima;
		ColorMath::Scale(color);
	}

	safe_release( pFunc );
}
