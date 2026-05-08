//////////////////////////////////////////////////////////////////////
//
//  UniformColorPainter.h - Defines a painter that paints some
//  uniform color.  Spectral path eagerly uplifts the RGB to a
//  Jakob-Hanika sigmoid spectrum at construction (Landing 3) so
//  GetColorNM / GetSpectrum return a physically-meaningful spectrum
//  rather than the previous luminance-proxy `MaxValue(C)`.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 19, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef UNIFORM_COLOR_PAINTER_
#define UNIFORM_COLOR_PAINTER_

#include "Painter.h"
#include "../Animation/KeyframableHelper.h"
#include "../Utilities/Color/RGBSpectra.h"

namespace RISE
{
	namespace Implementation
	{
		class UniformColorPainter : public Painter
		{
		protected:
			RISEPel                C;
			RGBAlbedoSpectrum      albedoSpec;       // kind = Albedo
			RGBUnboundedSpectrum   unboundedSpec;    // kind = Unbounded
			RGBIlluminantSpectrum  illuminantSpec;   // kind = Illuminant
			SpectrumKind           kind;
			virtual ~UniformColorPainter(){};

			void Recompute()
			{
				switch( kind ) {
					case eSpectrumKind_Unbounded:
						unboundedSpec = RGBUnboundedSpectrum::FromRGB( C );
						break;
					case eSpectrumKind_Illuminant:
						illuminantSpec = RGBIlluminantSpectrum::FromRGB( C );
						break;
					case eSpectrumKind_Albedo:
					default:
						albedoSpec = RGBAlbedoSpectrum::FromRGB( C );
						break;
				}
			}

			Scalar EvalKind( const Scalar nm ) const
			{
				switch( kind ) {
					case eSpectrumKind_Unbounded:   return unboundedSpec.Eval( nm );
					case eSpectrumKind_Illuminant:  return illuminantSpec.Eval( nm );
					case eSpectrumKind_Albedo:
					default:                         return albedoSpec.Eval( nm );
				}
			}

		public:
			UniformColorPainter( const RISEPel& C_, SpectrumKind kind_ = eSpectrumKind_Albedo )
			  : C( C_ ), kind( kind_ )
			{
				Recompute();
			}

			RISEPel GetColor( const RayIntersectionGeometric& ) const { return C; }

			Scalar GetColorNM( const RayIntersectionGeometric&, const Scalar nm ) const
			{
				return EvalKind( nm );
			}

			// Build a properly-populated SpectralPacket sampled at the
			// same 5nm spacing as RISE's CIE_DATA tables (380-780nm,
			// 81 bins).  This is consumed by LambertianEmitter /
			// PhongEmitter at construction time to cache an
			// `averageSpectrum`, which SpectralPhotonTracer later
			// reads for photon power.  Without this populated form,
			// RGB emissive painters silently emit zero on spectral
			// photon paths even though direct GetColorNM shading is
			// correct.  See SpectralPacket::SetAtIndex (added 2026-05).
			SpectralPacket GetSpectrum( const RayIntersectionGeometric& ) const
			{
				const Scalar lambda_begin = Scalar(380);
				const Scalar lambda_end   = Scalar(780);
				const unsigned int nbins  = 81;
				SpectralPacket sp( lambda_begin, lambda_end, nbins );
				const Scalar delta = (lambda_end - lambda_begin) / Scalar(nbins);
				for( unsigned int i = 0; i < nbins; ++i ) {
					const Scalar lambda = lambda_begin + Scalar(i) * delta;
					sp.SetAtIndex( i, EvalKind( lambda ) );
				}
				return sp;
			}

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value )
			{
				if( name == "risepel" ) {
					double d[3];
					if( sscanf( value.c_str(), "%lf %lf %lf", &d[0], &d[1], &d[2] ) == 3 ) {
						IKeyframeParameter* p = new Parameter<RISEPel>( RISEPel(d), 100 );
						GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
						return p;
					}
				}
				return 0;
			};

			void SetIntermediateValue( const IKeyframeParameter& val )
			{
				if( val.getID() == 100 ) {
					C = *(RISEPel*)val.getValue();
					Recompute();
				}
			}

			void RegenerateData( ){};
		};
	}
}

#endif
