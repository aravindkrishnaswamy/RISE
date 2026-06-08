//////////////////////////////////////////////////////////////////////
//
//  TexturePainter.h - Defines a texture painter, which is a painter
//  that derives color from some raster image
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 19, 2001
//  Tabs: 4
//  Comments:  Add wrapping and clamping abilities!
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TEXTUREPAINTER_
#define TEXTUREPAINTER_

#include "Painter.h"
#include "../Interfaces/IRasterImageAccessor.h"

namespace RISE
{
	namespace Implementation
	{
		class TexturePainter : public Painter
		{
		protected:
			IRasterImageAccessor*		pRIA;

			// Landing 2: cached dispatch decision.  Eliminates 2-3
			// virtual calls per per-sample GetColor invocation by
			// resolving the LOD strategy at construction.
			//   Mode_Base       — no LOD support; use GetPEL
			//   Mode_Pyramid    — accessor has mip pyramid
			//   Mode_Supersample— accessor uses footprint stochastic
			//                     supersampling at base (lowmem path)
			enum eFilterMode
			{
				Mode_Base       = 0,
				Mode_Pyramid    = 1,
				Mode_Supersample = 2
			};
			eFilterMode					filter_mode;

			//! Shared LOD-aware sample: dispatches on the cached
			//! filter_mode.  Used by both GetColor (returns .base)
			//! and GetAlpha (returns .a) so the alpha path picks
			//! the same LOD as the colour path — important for
			//! alphaMode = BLEND consistency under minification.
			RISEColor		SampleTextured( const RayIntersectionGeometric& ri ) const;

			virtual ~TexturePainter();

			// Spectrum role for sample-time uplift (Landing 3).
			//   Albedo    (default): treat sampled RGB as reflectance ∈ [0,1].
			//   Unbounded            : treat sampled RGB as radiance / illuminant
			//                          (HDR EXR or `KHR_materials_emissive_strength`-
			//                          scaled emissives may exceed 1.0).
			SpectrumKind                spectrumKind;

		public:
			TexturePainter( IRasterImageAccessor* pRIA_,
			                SpectrumKind kind_ = eSpectrumKind_Albedo );

			//! Exposes the backing raster accessor so a spatially-varying
			//! *scalar* painter (TextureScalarPainter) can sample the same
			//! image directly — by raster channel, with NO Jakob-Hanika
			//! uplift / colourspace conversion.  Returns the accessor
			//! WITHOUT adding a reference; the caller addref's if it wants
			//! to outlive this painter.  Concrete-class method (not on the
			//! abstract IPainter) so the public IPainter vtable is unchanged.
			IRasterImageAccessor* GetRasterImageAccessor() const { return pRIA; }

			RISEPel			GetColor( const RayIntersectionGeometric& ri  ) const;
			Scalar			GetAlpha( const RayIntersectionGeometric& ri  ) const;

			// Spectral path: sample mip in RGB (existing GetColor /
			// SampleTextured path), then uplift the per-sample RGB via
			// the Jakob-Hanika LUT to get a sigmoid spectrum, evaluate
			// at the target wavelength.  Adds ~30 FLOPs per spectral
			// texture sample (LUT lookup + sigmoid).  Mip pyramid
			// stays in RGB — filtering sigmoid coefficients is non-
			// linear and produces wrong spectra.  See Landing 3 design
			// doc § Architecture decisions.
			Scalar			GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;

			// GetSpectrum returns a properly-populated SpectralPacket
			// (81 bins, 380-780nm).  Required so LambertianEmitter /
			// PhongEmitter caching `averageSpectrum = radEx.GetSpectrum`
			// gets a real spectrum for textured emissives — without
			// this, spectral photon power is silently zero for any
			// glTF emissive driven by a TexturePainter.
			SpectralPacket	GetSpectrum( const RayIntersectionGeometric& ri ) const;

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ){ return 0;};
			void SetIntermediateValue( const IKeyframeParameter& val ){};
			void RegenerateData( ){};
		};
	}
}

#endif
