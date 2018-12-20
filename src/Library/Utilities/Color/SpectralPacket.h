//////////////////////////////////////////////////////////////////////
//
//  SpectralPacket.h - Contains implementations of various types
//                     of spectral packets.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 20, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SPECTRAL_PACKET_
#define SPECTRAL_PACKET_

#include <memory.h>
#include "../../Interfaces/ILog.h"
#include "ColorUtils.h"

namespace RISE
{
	////////////////////////////////////
	//
	// This is the definition of a robust spectral packet... 
	// It may be robust but its going to be slow, which why we won't use it
	// unless its absolutely necessary.  Refer to the templated packets
	// below
	//
	// NOTE!  This needs to be modified so that it can take a IFunction1D as an
	// input, BUT keep the original function around for as long as possible!
	//
	////////////////////////////////////
	class SpectralPacket
	{
	protected:
		Scalar			lambda_begin, lambda_end;
		unsigned int	num_freq;
		Scalar*			amplitudes;
		Scalar			delta;
		Scalar			OVnumfreq;

	public:
		SpectralPacket( ) : 
		lambda_begin( 400 ),
		lambda_end( 700 ),
		num_freq( 1 ),
		delta( 300 )
		{
			amplitudes = new Scalar[num_freq];
			GlobalLog()->PrintNew( amplitudes, __FILE__, __LINE__, "amplitudes" );

			for( unsigned int i=0; i<num_freq; i++ ) {
				amplitudes[i] = 0;
			}

			OVnumfreq = 1.0 / Scalar(num_freq);
		}

		SpectralPacket( const SpectralPacket& sp ) : 
		lambda_begin( sp.lambda_begin ),
		lambda_end( sp.lambda_end ),
		num_freq( sp.num_freq ),
		delta( sp.delta )
		{
			amplitudes = new Scalar[num_freq];
			GlobalLog()->PrintNew( amplitudes, __FILE__, __LINE__, "amplitudes" );

			for( unsigned int i=0; i<num_freq; i++ ) {
				amplitudes[i] = sp.amplitudes[i];
			}

			OVnumfreq = sp.OVnumfreq;
		}

		SpectralPacket( Scalar lambda_begin_, Scalar lambda_end_, unsigned int num_freq_ ) : 
		lambda_begin( lambda_begin_ ),
		lambda_end( lambda_end_ ),
		num_freq( num_freq_ ),
		delta( (lambda_end-lambda_begin) / Scalar(num_freq) )
		{
			amplitudes = new Scalar[num_freq];
			GlobalLog()->PrintNew( amplitudes, __FILE__, __LINE__, "amplitudes" );

			for( unsigned int i=0; i<num_freq; i++ ) {
				amplitudes[i] = 0;
			}

			OVnumfreq = 1.0 / Scalar(num_freq);
		}

		SpectralPacket( Scalar lambda_begin_, Scalar lambda_end_, unsigned int num_freq_, Scalar& d ) : 
		lambda_begin( lambda_begin_ ),
		lambda_end( lambda_end_ ),
		num_freq( num_freq_ ),
		delta( (lambda_end-lambda_begin) / Scalar(num_freq) )
		{
			amplitudes = new Scalar[num_freq];
			GlobalLog()->PrintNew( amplitudes, __FILE__, __LINE__, "amplitudes" );

			for( unsigned int i=0; i<num_freq; i++ ) {
				amplitudes[i] = d;
			}

			OVnumfreq = 1.0 / Scalar(num_freq);
		}

		SpectralPacket( Scalar lambda_begin_, Scalar lambda_end_, unsigned int num_freq_, const IFunction1D* pFunc ) : 
		lambda_begin( lambda_begin_ ),
		lambda_end( lambda_end_ ),
		num_freq( num_freq_ ),
		delta( (lambda_end-lambda_begin) / Scalar(num_freq) )
		{
			amplitudes = new Scalar[num_freq];
			GlobalLog()->PrintNew( amplitudes, __FILE__, __LINE__, "amplitudes" );

			// Pull the values from the function
			for( unsigned int i=0; i<num_freq; i++ ) {
				amplitudes[i] = pFunc->Evaluate( lambda_begin + (i*delta) );
			}
				
			OVnumfreq = 1.0 / Scalar(num_freq);
		}

		//
		// Getters
		//
		inline Scalar		minLambda( ) const { return lambda_begin; }
		inline Scalar		maxLambda( ) const { return lambda_end; }
		inline Scalar		deltaLambda( ) const { return delta; }

		//
		// Setters
		//
		inline void		SetAll( Scalar& d )
		{
			for( unsigned int i=0; i<num_freq; i++ )
				amplitudes[i] = d;
		}

		virtual ~SpectralPacket( )
		{
			GlobalLog()->PrintDelete( amplitudes, __FILE__, __LINE__ );
			delete [] amplitudes;
			amplitudes=0;
		}

		static inline		bool	SameFreqs( const SpectralPacket& a, const SpectralPacket& b )
		{
			if( a.lambda_begin == b.lambda_begin &&
				a.lambda_end == b.lambda_end &&
				a.num_freq == b.num_freq )
				return true;
			else
				return false;
		}

		//
		// Operators
		//

		// Assignment
		inline		SpectralPacket	operator=( const SpectralPacket& s )
		{
			// Check if the packet we are assigned to has the same frequencies responsible
			// and same discretization of frequencies
			if( SameFreqs( *this, s ) )
			{
				// Then its a lot easier
				memcpy( amplitudes, s.amplitudes, sizeof( Scalar ) * num_freq );
			}
			else if( s.num_freq == num_freq )
			{
				// If the number of frequencies are atleast the same, we don't need to 
				// reallocate
				lambda_begin = s.lambda_begin;
				lambda_end = s.lambda_end;
				memcpy( amplitudes, s.amplitudes, sizeof( Scalar ) * num_freq );
			}
			else
			{
				// We have to reallocate the array
				GlobalLog()->PrintDelete( amplitudes, __FILE__, __LINE__ );
				delete amplitudes;
				lambda_begin = s.lambda_begin;
				lambda_end = s.lambda_end;
				num_freq = s.num_freq;
				delta = (lambda_end-lambda_begin) / Scalar(num_freq);
				OVnumfreq = 1.0 / Scalar(num_freq);
				amplitudes = new Scalar[num_freq];
				GlobalLog()->PrintNew( amplitudes, __FILE__, __LINE__, "amplitudes" );

				memcpy( amplitudes, s.amplitudes, sizeof( Scalar ) * num_freq );
			}

			// Return the left hand side
			return *this;
		}

		// Scalar multiplication
		inline friend SpectralPacket operator*( SpectralPacket p, const Scalar& d )
		{
			SpectralPacket ret( p );
			ret *= d;
			return ret;
		};

		// Self scalar multiplication
		inline SpectralPacket& operator*=( const Scalar& d )
		{
			for( unsigned int i=0; i<num_freq; i++ ) {
				amplitudes[i] *= d;
			}

			return *this;
		}

		//! Converts the spectral packet into a CIE_XYZ color
		/// \return The CIE_XYZ color that represents this color
		inline XYZPel GetXYZ( ) const
		{
			// For each wavelength, get the XYZ value, scale it by the value at that wavelength in the spectral
			// packet, and sum... then divide out the total by the wavelength range...
			XYZPel	sum( 0, 0, 0 );

			Scalar freq=lambda_begin;
			for( unsigned int i=0; i<num_freq; i++, freq+=delta )
			{
				XYZPel thisNM( 0, 0, 0 );
				if( ColorUtils::XYZFromNM( thisNM, freq ) )
				{
					thisNM = thisNM * amplitudes[i];
					sum = sum + thisNM;
				}
			}

			sum = sum * OVnumfreq;

			return sum;
		}
#if 0  
		// DEAD CODE
		//! Converts the spectral packet into a RGB color given by the
		//! spectral power distributions
		/// \return The RGB resultant RGB color
		inline ArbitraryRGBPel GetRGB( 
			const IFunction1D& spd_r,				///< [in] Spectral power distribution for red
			const IFunction1D& spd_g,				///< [in] Spectral power distribution for green
			const IFunction1D& spd_b				///< [in] Spectral power distribution for blue
			) const
		{
			// For each wavelength, get the RGB value, scale it by the value at that wavelength in the spectral
			// packet, and sum... then divide out the total by the wavelength range...
			ArbitraryRGBPel	sum( 0, 0, 0 );

			Scalar freq=lambda_begin;
			for( unsigned int i=0; i<num_freq; i++, freq+=delta ) {
				ArbitraryRGBPel thisNM( 0, 0, 0 );
				ColorUtils::RGBFromNM( thisNM, freq, spd_r, spd_g, spd_b );
				thisNM = thisNM * amplitudes[i];
				sum = sum + thisNM;
			}

			sum = sum * OVnumfreq;

			return sum;
		}
#endif
		inline Scalar ValueAtNM( const Scalar& nm ) const
		{
			// Get the value at the particular wavelength

			// Outside the frequency range
			if( nm < lambda_begin || nm > lambda_end ) {
				return 0;
			}

			// Find the rigt frequency
			int idx = int((nm-lambda_begin)/delta);
			return amplitudes[idx];
		}
	};
}

#endif
