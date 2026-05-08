//////////////////////////////////////////////////////////////////////
//
//  UVTransformPainter.h - Wraps a source painter with a per-binding
//  UV affine transform, sampling the source at transformed (u, v).
//
//  Designed for glTF KHR_texture_transform: every textureInfo binding
//  (baseColorTexture, normalTexture, emissiveTexture, ...) may carry
//  its own offset / rotation / scale, and the SAME image painter may
//  be reused across bindings or materials with DIFFERENT transforms.
//  Wrapping at the binding site (not the image-painter site) lets the
//  underlying decoded image stay shared while each binding samples
//  through its own transform — see GLTFSceneImporter.cpp.
//
//  Transform definition (from KHR_texture_transform spec):
//    final_uv = T * R * S * source_uv
//  with R encoded as cos(-r) / sin(-r) so that positive `rotation`
//  rotates the IMAGE clockwise (UV coords counter-clockwise).
//  Concretely:
//    u' =  cos(r) * sx * u + sin(r) * sy * v + tx
//    v' = -sin(r) * sx * u + cos(r) * sy * v + ty
//  Defaults (no transform): offset = (0,0), rotation = 0, scale = (1,1).
//  This wrapper short-circuits to a passthrough when all three terms
//  are at their defaults — callers SHOULD NOT wrap in that case (the
//  importer skips wrapping when has_transform == false), but the
//  guard means a bug in the importer that wraps unconditionally still
//  costs only one branch per sample.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 6, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef UV_TRANSFORM_PAINTER_
#define UV_TRANSFORM_PAINTER_

#include "Painter.h"
#include <cmath>

namespace RISE
{
	namespace Implementation
	{
		class UVTransformPainter : public Painter
		{
		protected:
			const IPainter&	source;
			Scalar			offsetU;
			Scalar			offsetV;
			Scalar			rotation;	// radians; KHR_texture_transform sign convention
			Scalar			scaleU;
			Scalar			scaleV;
			Scalar			cosR;
			Scalar			sinR;
			bool			isIdentity;

			virtual ~UVTransformPainter()
			{
				source.release();
			}

			inline Point2 ApplyTransform( const Point2& uv ) const
			{
				if( isIdentity ) return uv;
				const Scalar su = scaleU * uv.x;
				const Scalar sv = scaleV * uv.y;
				return Point2(
					 cosR * su + sinR * sv + offsetU,
					-sinR * su + cosR * sv + offsetV );
			}

		public:
			UVTransformPainter(
				const IPainter& src,
				Scalar          offset_u,
				Scalar          offset_v,
				Scalar          rotation_,
				Scalar          scale_u,
				Scalar          scale_v ) :
			  source( src ),
			  offsetU( offset_u ),
			  offsetV( offset_v ),
			  rotation( rotation_ ),
			  scaleU( scale_u ),
			  scaleV( scale_v ),
			  cosR( std::cos( rotation_ ) ),
			  sinR( std::sin( rotation_ ) ),
			  isIdentity(
				  std::fabs( offset_u ) < Scalar( 1e-9 ) &&
				  std::fabs( offset_v ) < Scalar( 1e-9 ) &&
				  std::fabs( rotation_ ) < Scalar( 1e-9 ) &&
				  std::fabs( scale_u - Scalar( 1 ) ) < Scalar( 1e-9 ) &&
				  std::fabs( scale_v - Scalar( 1 ) ) < Scalar( 1e-9 ) )
			{
				source.addref();
			}

			RISEPel GetColor( const RayIntersectionGeometric& ri ) const
			{
				if( isIdentity ) return source.GetColor( ri );
				RayIntersectionGeometric ri2 = ri;
				ri2.ptCoord = ApplyTransform( ri.ptCoord );
				return source.GetColor( ri2 );
			}

			Scalar GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
			{
				if( isIdentity ) return source.GetColorNM( ri, nm );
				RayIntersectionGeometric ri2 = ri;
				ri2.ptCoord = ApplyTransform( ri.ptCoord );
				return source.GetColorNM( ri2, nm );
			}

			Scalar GetAlpha( const RayIntersectionGeometric& ri ) const
			{
				if( isIdentity ) return source.GetAlpha( ri );
				RayIntersectionGeometric ri2 = ri;
				ri2.ptCoord = ApplyTransform( ri.ptCoord );
				return source.GetAlpha( ri2 );
			}

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ) { (void)name; (void)value; return 0; };
			void SetIntermediateValue( const IKeyframeParameter& val ) { (void)val; };
			void RegenerateData( ) {};
		};
	}
}

#endif
