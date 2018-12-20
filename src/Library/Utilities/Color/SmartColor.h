//////////////////////////////////////////////////////////////////////
//
//  SmartColor.h - A smart color that is capable of dealing with
//  both spectra and CIE XYZ.  It's operators are smart enough
//  to figure out whats going on and work with the data that 
//  its given
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 7, 2002
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SMART_COLOR_
#define SMART_COLOR_

#error "Do not ever include SmartColor.h"

namespace RISE
{
	template< class Type >
	class XYZ_Template;

	#include "SpectralPacket.h"

	template< class Type >
	class SmartColor_Template
	{
	protected:
		XYZ_Template<Type>		color;
		VisibleSpectralPacket	spectra;
		bool					bSpectra;

	public:
		// Default constructor
		inline SmartColor_Template( ) : 
		bSpectra( false )
		{}

		// Copy constructor
		inline SmartColor_Template( const SmartColor_Template<Type>& k ) :
		bSpectra( k.bSpectra )
		{
			if( bSpectra )
				spectra = k.spectra;
			else
				color = k.color;
		}

		// Start off with a CIE_XYZ color
		inline SmartColor_Template( const XYZ_Template<Type>& p ) :
		bSpectra( false ),
		color( p )
		{}

		// Start off with spectra
		inline SmartColor_Template( const VisibleSpectralPacket& s ) :
		bSpectra( true ),
		spectra( s )
		{}

		// Assignment
		inline SmartColor_Template<Type>& operator=( const SmartColor_Template<Type>& v )  
		{
			bSpectra = v.bSpectra;
			bColor = v.bColor;
			
			if( bSpectra )
				spectra = v.spectra;
			else
				color = v.color;
			
			return *this;  // Assignment operator returns left side.
		}

		static inline SmartColor_Template<Type> Composite( SmartColor_Template<Type>& cTop, SmartColor_Template<Type>& cBottom )
		{
			SmartColor_Template<Type> ret;
			Composite( ret, cTop, cBottom );
			return ret;

		}

		static inline void Composite( SmartColor_Template<Type>& cDest, SmartColor_Template<Type>& cTop, SmartColor_Template<Type>& cBottom )
		{
			// We do different things depending on whether the top and bottom have valid spectra or
			// just color data

			if( cTop.bSpectra && cBottom.bSpectra )
			{
				// Both are spectra so do spectra
				cDest.bSpectra = true;
				VisibleSpectralPacket::Composite( cDest.spectra, cTop.spectra, cBottom.spectra, cTop.color.a, cBottom.color.a );
			}
			else if( !cTop.bSpectra && !cBottom.bSpectra )
			{
				// Both are color, so do color
				cDest.bSpectra = false;
				XYZ_Template<Type>::Composite( cDest.color, cTop.color, cBottom.color );
			}
			else
			{
				// One is spectra and the other is color, so 
				// we have to pick one and go with that...
				// Lets go with spectra
				cDest.bSpectra = false;
				if( cTop.bSpectra )
				{
					cTop.bSpectra = false;
					cTop.color = cTop.spectra.GetXYZ();
				}
				else
				{
					cBottom.bSpectra = false;
					cBottom.color = cBottom.spectra.GetXYZ();
				}
				RISEColor::Composite( cDest.color, cTop.color, cBottom.color );
			}
		}

		// Operators
		// For all of them, depends on the internal format...
		// Scalar Multiplication (*) 
		inline friend SmartColor_Template<Type> operator*( const SmartColor_Template<Type> &v, const Scalar t )  
		{
			if( v.bSpectra )
				v.spectra = v.spectra * t;
			else
				v.color = v.color * t;
		}

		// Scalar Multiplication (*)
		inline friend SmartColor_Template<Type> operator*( const Scalar t, const SmartColor_Template<Type> &v )  
		{ 
			if( v.bSpectra )
				v.spectra = t * v.spectra;
			else
				v.color = t * v.color;
		}

		// Addition (+)
		inline friend SmartColor_Template<Type> operator+( SmartColor_Template<Type> &a, SmartColor_Template<Type> &b )  
		{
			SmartColor_Template<Type>	ret;
			if( a.bSpectra && b.bSpectra )
			{
				// Both spectra, so do spectra
				ret.bSpectra = true;
				ret.spectra = a.spectra + b.spectra;
			}
			else if( !a.bSpectra && !b.bSpectra )
			{
				// Both XYZ, so do XYZ
				ret.bSpectra = false;
				ret.color = a.color + b.color;
			}
			else
			{
				// Convert to color
				ret.bSpectra = false;

				if( a.bSpectra )
				{
					a.bSpectra = false;
					a.color = a.spectra.GetXYZ( );
				}
				else
				{
					b.bSpectra = false;
					b.color = b.spectra.GetXYZ( );
				}

				ret.color = a.color + b.color;
			}

			return ret;
		}

		// Subtraction (-)
		inline friend SmartColor_Template<Type> operator-( SmartColor_Template<Type> &a, SmartColor_Template<Type> &b )  
		{
			SmartColor_Template<Type>	ret;
			if( a.bSpectra && b.bSpectra )
			{
				// Both spectra, so do spectra
				ret.bSpectra = true;
				ret.spectra = a.spectra - b.spectra;
			}
			else if( !a.bSpectra && !b.bSpectra )
			{
				// Both XYZ, so do XYZ
				ret.bSpectra = false;
				ret.color = a.color - b.color;
			}
			else
			{
				// Convert to color
				ret.bSpectra = false;

				if( a.bSpectra )
				{
					a.bSpectra = false;
					a.color = a.spectra.GetXYZ( );
				}
				else
				{
					b.bSpectra = false;
					b.color = b.spectra.GetXYZ( );
				}

				ret.color = a.color - b.color;
			}

			return ret;
		}

		// Modulate
		inline friend SmartColor_Template<Type> operator*( SmartColor_Template<Type> &a, SmartColor_Template<Type> &b )  
		{
			SmartColor_Template<Type>	ret;
			if( a.bSpectra && b.bSpectra )
			{
				// Both spectra, so do spectra
				ret.bSpectra = true;
				ret.spectra = a.spectra * b.spectra;
			}
			else if( !a.bSpectra && !b.bSpectra )
			{
				// Both XYZ, so do XYZ
				ret.bSpectra = false;
				ret.color = a.color * b.color;
			}
			else
			{
				// Convert to color
				ret.bSpectra = false;

				if( a.bSpectra )
				{
					a.bSpectra = false;
					a.color = a.spectra.GetXYZ( );
				}
				else
				{
					b.bSpectra = false;
					b.color = b.spectra.GetXYZ( );
				}

				ret.color = a.color * b.color;
			}

			return ret;
		}
	};
}

#include "CIE_XYZ.h"

#endif
