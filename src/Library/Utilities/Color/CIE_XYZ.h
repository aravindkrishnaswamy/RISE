//////////////////////////////////////////////////////////////////////
//
//  CIE_XYZ.h - Contains a templated CIE XYZ color class.  This is 
//  accepted CIE standard for color, and what we want to mainly
//  use inside the library
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 8, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CIE_XYZ_
#define CIE_XYZ_

namespace RISE
{
	struct XYZPel
	{
		Chel	X;
		Chel	Y;
		Chel	Z;

		// Default constructor
		inline XYZPel( ) :
		X( 0 ),
		Y( 0 ),
		Z( 0 )
		{}

		inline XYZPel( const Chel val ) :
		X( val ),
		Y( val ),
		Z( val )
		{}

		inline XYZPel( const Chel val[3] ) :
		X( val[0] ),
		Y( val[1] ),
		Z( val[2] )
		{}

		// Copy constructor
		inline XYZPel( const XYZPel& k ) :
		X( k.X ),
		Y( k.Y ),
		Z( k.Z )
		{}

		// Alternate constructor
		inline XYZPel( const Chel X_, const Chel Y_, const Chel Z_ ) : 
		X( X_ ),
		Y( Y_ ),
		Z( Z_ )
		{}

		// Conversion 
		inline XYZPel( const Rec709RGBPel& rgb )
		{
			*this = ColorUtils::Rec709RGBtoXYZ( rgb );
		}

		inline XYZPel( const ROMMRGBPel& rgb )
		{
			*this = ColorUtils::ROMMRGBtoXYZ( rgb );
		}
	
		inline XYZPel& operator=( const XYZPel& v )  
		{
			X = v.X;
			Y = v.Y;
			Z = v.Z;
			
			return *this;  // Assignment operator returns left side.
		}

		inline XYZPel& operator=( const Scalar& d )  
		{
			X = Y = Z = d;
			return *this;  // Assignment operator returns left side.
		}

		// Array style access
		inline		Scalar&		operator[]( const unsigned int i )
		{
			return i==0 ? X : i==1 ? Y : Z;
		}

		// Array style access
		inline		Scalar		operator[]( const unsigned int i ) const
		{
			return i==0 ? X : i==1 ? Y : Z;
		}


	#define COLOR_CLASS_TYPE XYZPel
	#include "ColorOperators.h"
	#undef COLOR_CLASS_TYPE
	};
}

#endif
