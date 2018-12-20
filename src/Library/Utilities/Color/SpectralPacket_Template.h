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

#ifndef SPECTRAL_PACKET_TEMPLATE_H
#define SPECTRAL_PACKET_TEMPLATE_H

#include <memory.h>
#include "../../Interfaces/IFunction1D.h"
#include "ColorUtils.h"

namespace RISE
{
	struct XYZPel;

	////////////////////////////////////
	//
	// This is a templated spectral class which will be a lot faster
	// and that's why we should use it instead
	//
	////////////////////////////////////

	template< class T, int lambda_begin, int lambda_end, int I >
	class SpectralPacket_Template
	{
	protected:
		T			amplitudes[I];
		int			delta;

	public:
		SpectralPacket_Template( ) :
		delta( (lambda_end-lambda_begin) / (I-1) )
		{
			for( int i=0; i<I; i++ ) {
				amplitudes[i] = 0;
			}
		}

		SpectralPacket_Template( const T d ) :
		delta( (lambda_end-lambda_begin) / (I-1) )
		{
			for( int i=0; i<I; i++ ) {
				amplitudes[i] = d;
			}
		}

		SpectralPacket_Template( const IFunction1D& pFunc ) :
		delta( (lambda_end-lambda_begin) / (I-1) )
		{
			// Pull the values from the function
			for( int i=0; i<I; i++ ) {
				amplitudes[i] = pFunc.Evaluate( T(i) / T(I) );
			}
		}

		virtual ~SpectralPacket_Template( )
		{}

		//
		// Getters
		//
		inline T		minLambda( ){ return T(lambda_begin); }
		inline T		maxLambda( ){ return T(lambda_end); }
		inline T		deltaLambda( ){ return T(delta); }

		//
		// Setters
		//
		inline void		SetAll( const T d )
		{
			for( int i=0; i<I; i++ ) {
				amplitudes[i] = d;
			}
		}

		inline void		SetIndex( unsigned int idx, const T d)
		{
			amplitudes[idx] = d;
		}

		//
		// Operators
		//

		// Assignment
		inline		SpectralPacket_Template<T, lambda_begin, lambda_end, I>	operator=(
			const SpectralPacket_Template<T, lambda_begin, lambda_end, I>& s )
		{
			memcpy( amplitudes, s.amplitudes, sizeof( T ) * I );

			// Return the left hand side
			return *this;
		}

		// Addition
		inline friend	SpectralPacket_Template<T, lambda_begin, lambda_end, I> operator+(
			const SpectralPacket_Template<T, lambda_begin, lambda_end, I>& a,
			const SpectralPacket_Template<T, lambda_begin, lambda_end, I>& b )
		{
			SpectralPacket_Template<T, lambda_begin, lambda_end, I> ret;
			for( int i=0; i<I; i++ ) {
				ret.amplitudes[i] = a.amplitudes[i] + b.amplitudes[i];
			}

			return ret;
		}

		// Self-Addition
		inline	SpectralPacket_Template<T, lambda_begin, lambda_end, I>& operator+=(
			const SpectralPacket_Template<T, lambda_begin, lambda_end, I>& p )
		{
			for( int i=0; i<I; i++ ) {
				amplitudes[i] += p.amplitudes[i];
			}

			return *this;
		}

		// Subtraction
		inline friend	SpectralPacket_Template<T, lambda_begin, lambda_end, I> operator-(
			const SpectralPacket_Template<T, lambda_begin, lambda_end, I>& a,
			const SpectralPacket_Template<T, lambda_begin, lambda_end, I>& b )
		{
			SpectralPacket_Template<T, lambda_begin, lambda_end, I> ret;
			for( int i=0; i<I; i++ ) {
				ret.amplitudes[i] = a.amplitudes[i] - b.amplitudes[i];
			}

			return ret;
		}

		// Self-Subraction
		inline	SpectralPacket_Template<T, lambda_begin, lambda_end, I>& operator-=(
			const SpectralPacket_Template<T, lambda_begin, lambda_end, I>& p )
		{
			for( int i=0; i<I; i++ ) {
				amplitudes[i] -= p.amplitudes[i];
			}

			return *this;
		}

		// Scalar multiplication
		inline friend SpectralPacket_Template<T, lambda_begin, lambda_end, I> operator*(
			SpectralPacket_Template<T, lambda_begin, lambda_end, I> p, T& d )
		{
			SpectralPacket_Template<T, lambda_begin, lambda_end, I> ret;
			for( int i=0; i<I; i++ ) {
				ret.amplitudes[i] = p.amplitudes[i] * d;
			}

			return ret;
		};

		// Self scalar multiplication
		inline SpectralPacket_Template<T, lambda_begin, lambda_end, I>& operator*=( T& d )
		{
			for( int i=0; i<I; i++ ) {
				amplitudes[i] *= d;
			}

			return *this;
		}

		// Spectrum multiplication
		inline friend SpectralPacket_Template<T, lambda_begin, lambda_end, I> operator*(
			SpectralPacket_Template<T, lambda_begin, lambda_end, I> a,
			SpectralPacket_Template<T, lambda_begin, lambda_end, I> b )
		{
			SpectralPacket_Template<T, lambda_begin, lambda_end, I> ret;
			for( int i=0; i<I; i++ ) {
				ret.amplitudes[i] = a.amplitudes[i] * b.amplitudes[i];
			}

			return ret;
		};

		// Self spectrum multiplication
		inline SpectralPacket_Template<T, lambda_begin, lambda_end, I>& operator*=(
			SpectralPacket_Template<T, lambda_begin, lambda_end, I> p )
		{
			for( int i=0; i<I; i++ ) {
				amplitudes[i] *= p.amplitudes[i];
			}

			return *this;
		}

		// Addition to the other spectral packet type
		inline friend	SpectralPacket_Template<T, lambda_begin, lambda_end, I> operator+(
			const SpectralPacket_Template<T, lambda_begin, lambda_end, I>& a,
			const SpectralPacket& b )
		{
			SpectralPacket_Template<T, lambda_begin, lambda_end, I> ret;
			for( int i=0; i<I; i++ ) {
				ret.amplitudes[i] = a.amplitudes[i] + b.ValueAtNM( lambda_begin + a.delta*i );
			}

			return ret;
		}

		inline XYZPel GetXYZ( ) const
		{
			// For each wavelength, get the XYZ value, scale it by the value at that wavelength in the spectral
			// packet, and sum... then divide out the total by the wavelength range...
			XYZPel	sum( 0, 0, 0 );

			int freq=lambda_begin;
			for( int i=0; i<I; i++, freq+=delta )
			{
				XYZPel thisNM( 0, 0, 0 );
				if( ColorUtils::XYZFromNM( thisNM, freq ) )
				{
					thisNM = thisNM * amplitudes[i];
					sum = sum + thisNM;
				}
			}

			static const T OVrange = 1.0 / T(lambda_end - lambda_begin);
			sum = sum * OVrange;

			return sum;
		}

		inline T ValueAtNM( const int& nm ) const
		{
			// Get the value at the particular wavelength

			// Outside the frequency range
			if( nm < lambda_begin || nm > lambda_end ) {
				return 0;
			}

			// Find the rigt frequency
			int idx = (nm-lambda_begin)/delta;
			return amplitudes[idx];
		}
	};

	typedef SpectralPacket_Template<Scalar, 380, 780, 40> VisibleSpectralPacket;
}

#include "CIE_XYZ.h"

#endif
