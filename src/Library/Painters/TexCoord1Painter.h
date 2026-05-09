//////////////////////////////////////////////////////////////////////
//
//  TexCoord1Painter.h - Wraps a source painter and routes its UV
//  reads through ri.ptCoord1 (TEXCOORD_1) instead of ri.ptCoord
//  (TEXCOORD_0).
//
//  Designed for glTF L12.D: per-binding texCoord selector (a value
//  in textureInfo.texCoord, or in textureInfo.extensions
//  .KHR_texture_transform.texCoord which overrides it).  The importer
//  wraps a base / UV-transformed painter with this when the binding
//  declares texcoord = 1; the underlying texture decode stays shared
//  with TEXCOORD_0 bindings of the same image.
//
//  Falls back to ptCoord (TEXCOORD_0) when the geometry doesn't
//  carry TEXCOORD_1 (ri.bHasTexCoord1 == false).  The triangle-mesh
//  intersection code mirrors ptCoord into ptCoord1 in that case, so
//  the fallback is geometry-side and this wrapper just unconditionally
//  swaps in ptCoord1.  Either way, sampling on a non-UV1 mesh produces
//  TEXCOORD_0 sampling — visually wrong by the asset's intent but
//  doesn't crash and stays in [0, 1].
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 7, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TEXCOORD1_PAINTER_
#define TEXCOORD1_PAINTER_

#include "Painter.h"

namespace RISE
{
	namespace Implementation
	{
		class TexCoord1Painter : public Painter
		{
		protected:
			const IPainter&	source;

			virtual ~TexCoord1Painter()
			{
				source.release();
			}

		public:
			TexCoord1Painter( const IPainter& src ) :
			  source( src )
			{
				source.addref();
			}

			// `source` is a reference member; an implicit copy ctor would
			// rebind by reference (no addref) and the second dtor's
			// release() on the same painter would double-decrement and
			// potentially UAF.  Disallow.
			TexCoord1Painter( const TexCoord1Painter& ) = delete;
			TexCoord1Painter& operator=( const TexCoord1Painter& ) = delete;

			// Defensive: if the geometry didn't carry a TEXCOORD_1
			// (analytic primitives like spheres/cylinders, non-indexed
			// meshes whose loader didn't mirror) fall through to the
			// source painter without UV1 swap.  Triangle-mesh side mirrors
			// ptCoord1 = ptCoord when bHasTexCoord1 is false, so this gate
			// is doubly safe — but covers the case where ptCoord1 might
			// be left at its default-constructed value.
			//
			// txFootprint invalidation: ri.txFootprint stores the (dudx,
			// dudy, dvdx, dvdy) partial derivatives of the PRIMARY UV
			// coords w.r.t. screen pixels; downstream TexturePainter
			// uses them to pick a mip level / pre-filter footprint.
			// On a UV1 swap the footprint is now describing the WRONG
			// chart — UV1's density / orientation can be wholly different
			// from UV0 (lightmap atlases especially).  Invalidating
			// `txFootprint.valid` forces TexturePainter down the
			// bilinear-base path: correct sampling at the right texel,
			// no mip-level confusion, with the cost of more aliasing on
			// UV1-heavy textures viewed at footprint > 1 texel.  Proper
			// per-UV-set footprints (txFootprint1 carrying ∂UV1/∂x|y)
			// require geometry-side dpdu1/dpdv1 derivatives and are
			// their own future landing.
			RISEPel GetColor( const RayIntersectionGeometric& ri ) const
			{
				if( !ri.bHasTexCoord1 ) return source.GetColor( ri );
				RayIntersectionGeometric ri2 = ri;
				ri2.ptCoord = ri.ptCoord1;
				ri2.txFootprint.valid = false;
				return source.GetColor( ri2 );
			}

			Scalar GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
			{
				if( !ri.bHasTexCoord1 ) return source.GetColorNM( ri, nm );
				RayIntersectionGeometric ri2 = ri;
				ri2.ptCoord = ri.ptCoord1;
				ri2.txFootprint.valid = false;
				return source.GetColorNM( ri2, nm );
			}

			SpectralPacket GetSpectrum( const RayIntersectionGeometric& ri ) const
			{
				// Spectral counterpart of GetColor / GetColorNM.  Without
				// this override the base Painter::GetSpectrum returns a
				// dummy SpectralPacket and any spectral painter wrapped
				// behind a TexCoord1Painter (e.g. a spectral emissive
				// bound to TEXCOORD_1 on a glTF luminary) silently loses
				// its spectral response on the spectral integrator path.
				if( !ri.bHasTexCoord1 ) return source.GetSpectrum( ri );
				RayIntersectionGeometric ri2 = ri;
				ri2.ptCoord = ri.ptCoord1;
				ri2.txFootprint.valid = false;
				return source.GetSpectrum( ri2 );
			}

			Scalar GetAlpha( const RayIntersectionGeometric& ri ) const
			{
				if( !ri.bHasTexCoord1 ) return source.GetAlpha( ri );
				RayIntersectionGeometric ri2 = ri;
				ri2.ptCoord = ri.ptCoord1;
				ri2.txFootprint.valid = false;
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
