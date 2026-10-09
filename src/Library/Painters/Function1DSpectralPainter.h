//////////////////////////////////////////////////////////////////////
//
//  Function1DSpectralPainter.h - Defines a painter that is a spectral
//    painter which gets its spectral values from a Function1D
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 27, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FUNCTION1D_SPECTRAL_PAINTER_
#define FUNCTION1D_SPECTRAL_PAINTER_

#include "../Interfaces/IFunction1D.h"
#include "Painter.h"

namespace RISE
{
	namespace Implementation
	{
		//! DL-396: the linear Rec.709 colour of a physical spectrum `f`
		//! (sampled at 1 nm over the CIE table's 380-780 nm), viewed as a
		//! SOURCE when `bAsRadiance` -- `XYZ = Int f cmf / Int ybar`, the
		//! film's own resolve of a spectral radiance -- and as a
		//! REFLECTANCE under the Y-normalised D65 reference illuminant
		//! otherwise -- `XYZ = Int f D65 cmf / Int D65 ybar`, the Stage C
		//! convention the Jakob-Hanika LUT is trained under.  XYZ goes to
		//! RGB through `ColorUtils::XYZtoRec709RGB`, the film's conversion,
		//! so the result is non-negative.  Defined in Painter.cpp.
		RISEPel ProjectPhysicalSpectrumToRGB( const IFunction1D& f, const bool bAsRadiance );

		class Function1DSpectralPainter : public Painter
		{
		protected:
			const IFunction1D&	func;
			//! RGB projections of `func` (DL-396), computed once: a
			//! Function1D carries no spatial dependence, and the function
			//! is fully built before `Job::AddPiecewiseLinearFunction`
			//! wraps it.
			RISEPel				reflectanceRGB;
			RISEPel				radianceRGB;
			virtual ~Function1DSpectralPainter()
			{
				func.release();
			}

		public:
			Function1DSpectralPainter( const IFunction1D& f ) : func( f )
			{
				func.addref();
				RegenerateData();
			}

			//! DL-396: the spectrum's RGB projection as a REFLECTANCE under
			//! D65.  This used to return BLACK -- a missing projection, not
			//! a design choice: an RGB render showed a black surface, and an
			//! emitter whose `averageRadiantExitance` (built from the RGB
			//! view) was zero was dropped from the light list, so every
			//! spectral NEE / light-subpath / photon / SMS strategy lost it.
			RISEPel			GetColor( const RayIntersectionGeometric& ) const
			{
				return reflectanceRGB;
			}

			//! DL-396: the same spectrum as a SOURCE -- what the spectral
			//! film resolves `GetRadianceNM` (== `func`) to, so an RGB
			//! render of this emitter matches the spectral one.
			RISEPel			GetRadianceColor( const RayIntersectionGeometric& ) const
			{
				return radianceRGB;
			}

			Scalar			GetColorNM( const RayIntersectionGeometric&, const Scalar nm ) const
			{
				return func.Evaluate(nm );
			}

			//! PHYSICAL SPD -- pass through verbatim (Stage C slice 2).
			//! This painter's GetColorNM is not a Jakob-Hanika uplift of an
			//! RGB triple; it is an absolute spectral radiance/reflectance
			//! the author supplied.  Letting the IPainter default run would
			//! throw it away and re-uplift the RGB projection instead, which
			//! is exactly the class of bug the Hosek-Wilkie radiance map is
			//! kept off the painter path to avoid.
			//! (Before DL-396 this painter's `GetColor` returned BLACK, so the
			//! IPainter default would also have made a luminaire driven by
			//! it emit exactly zero on the spectral path.)
			Scalar			GetRadianceNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
			{
				return GetColorNM( ri, nm );
			}

			//! DL-165: a measured-SPD painter whose GetColor is only an RGB
			//! projection -- see IPainter::IsSpectrallyDefined's doc comment.
			bool			IsSpectrallyDefined() const { return true; }

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ){ return 0;};
			void SetIntermediateValue( const IKeyframeParameter& val ){};
			void RegenerateData( )
			{
				reflectanceRGB = ProjectPhysicalSpectrumToRGB( func, false );
				radianceRGB = ProjectPhysicalSpectrumToRGB( func, true );
			}
		};
	}
}

#endif


