//////////////////////////////////////////////////////////////////////
//
//  VertexColorPainter.h - A painter that returns the per-vertex color
//  interpolated by the geometry at the hit point (`ri.vColor`).  Falls
//  back to a configured default color for hits on geometry that does
//  not carry vertex colors (every IGeometry except a coloured indexed
//  triangle mesh today).
//
//  Spectral path: per-sample uplift the vertex color (or fallback) via
//  the Jakob-Hanika LUT.  The fallback colour's spectrum is cached at
//  construction; the per-vertex colour's spectrum is uplifted at
//  sample time because it varies across the surface.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 28, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef VERTEX_COLOR_PAINTER_
#define VERTEX_COLOR_PAINTER_

#include "Painter.h"
#include "../Animation/KeyframableHelper.h"
#include "../Utilities/Color/RGBSpectra.h"

namespace RISE
{
	namespace Implementation
	{
		class VertexColorPainter : public Painter
		{
		protected:
			RISEPel              Cdefault;
			RGBAlbedoSpectrum    fallbackSpec;	// cached uplift of Cdefault
			virtual ~VertexColorPainter(){};

			void RecomputeFallback()
			{
				fallbackSpec = RGBAlbedoSpectrum::FromRGB( Cdefault );
			}

		public:
			VertexColorPainter( const RISEPel& fallback ) :
			  Cdefault( fallback )
			{
				RecomputeFallback();
			};

			RISEPel	GetColor( const RayIntersectionGeometric& ri ) const
			{
				return ri.bHasVertexColor ? ri.vColor : Cdefault;
			}

			Scalar GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
			{
				if( ri.bHasVertexColor ) {
					// Per-vertex colour varies across the surface; uplift
					// each sample independently.  Same sample-time-uplift
					// pattern TexturePainter uses (see TexturePainter.cpp's
					// GetColorNM for the design rationale).
					const RGBAlbedoSpectrum s = RGBAlbedoSpectrum::FromRGB(
						ri.vColor, RGBToSpectrumTable::Get() );
					return s.Eval( nm );
				}
				return fallbackSpec.Eval( nm );
			}

			// Construction-time cache target for LambertianEmitter /
			// PhongEmitter on vertex-coloured emissive meshes.  Without
			// this, the base Painter::GetSpectrum dummy 1-bin packet
			// silently zeros spectral photon power for any glTF mesh
			// using vertex colour as an emissive driver.
			SpectralPacket GetSpectrum( const RayIntersectionGeometric& ri ) const
			{
				const Scalar lambda_begin = Scalar(380);
				const Scalar lambda_end   = Scalar(780);
				const unsigned int nbins  = 81;
				SpectralPacket sp( lambda_begin, lambda_end, nbins );
				const Scalar delta = (lambda_end - lambda_begin) / Scalar(nbins);

				if( ri.bHasVertexColor ) {
					const RGBAlbedoSpectrum s = RGBAlbedoSpectrum::FromRGB(
						ri.vColor, RGBToSpectrumTable::Get() );
					for( unsigned int i = 0; i < nbins; ++i ) {
						sp.SetAtIndex( i, s.Eval( lambda_begin + Scalar(i) * delta ) );
					}
				} else {
					for( unsigned int i = 0; i < nbins; ++i ) {
						sp.SetAtIndex( i, fallbackSpec.Eval( lambda_begin + Scalar(i) * delta ) );
					}
				}
				return sp;
			}

			// Keyframable interface — drives the fallback color over time.
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value )
			{
				if( name == "fallback_risepel" ) {
					double d[3];
					if( sscanf( value.c_str(), "%lf %lf %lf", &d[0], &d[1], &d[2] ) == 3 ) {
						IKeyframeParameter* p = new Parameter<RISEPel>( RISEPel(d), 200 );
						GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
						return p;
					}
				}
				return 0;
			};

			void SetIntermediateValue( const IKeyframeParameter& val )
			{
				if( val.getID() == 200 ) {
					Cdefault = *(RISEPel*)val.getValue();
					RecomputeFallback();
				}
			}

			void RegenerateData( ){};
		};
	}
}

#endif
