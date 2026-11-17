//////////////////////////////////////////////////////////////////////
//
//  Color_Template.h - A color is defined as some basic color
//  type, which is specified through the template and an alpha
//  component...
//  There are certain functions that the underlying system
//  must support...
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 8, 2002
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef COLOR_TEMPLATE_
#define COLOR_TEMPLATE_

#include "Color.h"
#include "../PEL.h"
#include "../FiniteMath.h"

namespace RISE
{
	template< class ColorBase >
	class Color_Template
	{
	public:
		ColorBase		base;
		Chel			a;

		// Default constructor
		inline Color_Template( ) :
		a( 0 )
		{}

		// Copy constructor
		inline Color_Template( const Color_Template<ColorBase>& k ) :
		base( k.base ),
		a( k.a )
		{}

		// Alternate constructor
		inline Color_Template( const Chel a_, const ColorBase& b_ ) : 
		base( b_ ),
		a( a_ )
		{}

		// Alternate constructor
		inline Color_Template( const ColorBase& b_, const Chel a_ ) : 
		base( b_ ),
		a( a_ )
		{}

		// Alternate constructor
		inline Color_Template( const ColorBase& b_ ) :
		base( b_ ),
		a( 1.0 )
		{}

		// 4-Tuple constructor, this will do bad things with spectra, so don't ever try it!
		inline Color_Template( const Chel b, const Chel c, const Chel d, const Chel a_ ) : 
		base( ColorBase( b, c, d ) ),
		a( a_ )
		{}

		inline Color_Template<ColorBase>& operator=( const Color_Template<ColorBase>& v )  
		{
			base = v.base;
			a = v.a;
			
			return *this;  // Assignment operator returns left side.
		}

		// Operators
		// Scalar Multiplication (*) 
		inline friend Color_Template<ColorBase> operator*( const Color_Template<ColorBase> &v, const Scalar t )  
		{
			return Color_Template<ColorBase>( v.base*t, v.a*t );
		}

		// Scalar Multiplication (*)
		inline friend Color_Template<ColorBase> operator*( const Scalar t, const Color_Template<ColorBase> &v )  
		{ 
			return Color_Template<ColorBase>( v.base*t, v.a*t );
		}

		// Addition (+)
		inline friend Color_Template<ColorBase> operator+( const Color_Template<ColorBase> &a, const Color_Template<ColorBase> &b )  
		{
			return Color_Template<ColorBase>( a.base+b.base, a.a+b.a );
		}

		// Subtraction (-)
		inline friend Color_Template<ColorBase> operator-( const Color_Template<ColorBase> &a, const Color_Template<ColorBase> &b )  
		{
			return Color_Template<ColorBase>( a.base-b.base, a.a-b.a );
		}

		// Modulate
		inline friend Color_Template<ColorBase> operator*( const Color_Template<ColorBase> &a, const Color_Template<ColorBase> &b )  
		{
			return Color_Template<ColorBase>( a.base*b.base, a.a*b.a );
		}

		// Are equal ?
		inline		bool	operator==( const Color_Template<ColorBase>& c )
		{
			return( c.base==base && c.a==a );
		}

		// Raw scaling utility: callers must ensure every scaled RGB/alpha
		// product is representable in T (finite and in range for integral T).
		template< class T, class SrcT >
		inline RGBA_T<T> ScaleRGBPelValuesAndIncludeAlpha( SrcT& pel, double scale ) const
		{
			RGBA_T<T> ret;
			ret.r = (T)(pel.r*scale);
			ret.g = (T)(pel.g*scale);
			ret.b = (T)(pel.b*scale);
			ret.a = (T)(a*scale);
			return ret;
		}

		//
		// Integerizating of values
		//
		// Alpha-only normalized output: max_value must be finite, nonnegative
		// and representable in T, as for Integerize's full RGBA conversion.
		template< class T >
		inline T IntegerizeAlpha( const double& max_value ) const
		{
			return QuantizeNormalizedChannel<T>(a,max_value);
		}

		template< class TCSPACE, class T >
		inline RGBA_T<T> Integerize( const double& max_value ) const
		{
			TCSPACE conv( base );
			// Normalized output saturates every channel before conversion.
			// max_value must itself be finite, nonnegative and representable in T.
			RGBA_T<T> ret;
			ret.r = QuantizeNormalizedChannel<T>(conv.r,max_value);
			ret.g = QuantizeNormalizedChannel<T>(conv.g,max_value);
			ret.b = QuantizeNormalizedChannel<T>(conv.b,max_value);
			ret.a = IntegerizeAlpha<T>(max_value);
			return ret;
		}

		template< class TCSPACE, class T >
		inline void SetFromIntegerized( const RGBA_T<T>& rgba, const double& max_value )
		{
			static const Chel	OVMax = Scalar(1.0 / max_value);
			TCSPACE conv( rgba.r*OVMax, rgba.g*OVMax, rgba.b*OVMax );
			base = ColorBase( conv );
			a = rgba.a*OVMax;
		}

	private:
		template< class T >
		static T QuantizeNormalizedChannel( double value, double max_value )
		{
			if( !IsFiniteDouble(value) )
				return IsPositiveInfinityDouble(value) ? static_cast<T>(max_value) : T(0);
			if( value <= 0 ) return T(0);
			if( value >= 1 ) return static_cast<T>(max_value);
			return static_cast<T>(value * max_value);
		}
	};

	typedef Color_Template<ROMMRGBPel>			ColorROMMRGB;
	typedef Color_Template<Rec709RGBPel>		ColorRec709RGB;
	typedef Color_Template<XYZPel>				ColorXYZ;

	typedef Color_Template<RISEPel>				RISEColor;
}

#endif
