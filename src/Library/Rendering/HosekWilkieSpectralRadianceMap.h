//////////////////////////////////////////////////////////////////////
//
//  HosekWilkieSpectralRadianceMap.h - IRadianceMap implementation
//    that wraps a HosekWilkieSkyModel to provide analytic spectral
//    sun-and-sky radiance.
//
//    Unlike the existing image-based RadianceMap (which goes through
//    a UV-mapped IPainter), this maps incoming ray direction
//    DIRECTLY to the model's per-direction radiance evaluation —
//    no painter, no UV.  Avoids the round-trip through 2D texture
//    space that would otherwise lose precision near the zenith and
//    horizon poles of the lat-long projection.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef HOSEK_WILKIE_SPECTRAL_RADIANCE_MAP_
#define HOSEK_WILKIE_SPECTRAL_RADIANCE_MAP_

#include "../Interfaces/IRadianceMap.h"
#include "../Utilities/HosekWilkieSkyModel.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class HosekWilkieSpectralRadianceMap :
			public virtual IRadianceMap,
			public virtual Reference
		{
		protected:
			HosekWilkieSkyModel	model;
			Matrix4				mxTransform;
			Scalar				skyScale;	// scene-level intensity multiplier
			class IPainter*		pNullPainter;	// satisfy GetPainter()

			virtual ~HosekWilkieSpectralRadianceMap();

		public:
			HosekWilkieSpectralRadianceMap(
				Scalar solarElevationRadians,
				Scalar solarAzimuthRadians,
				Scalar turbidity,
				const RISEPel& groundAlbedo,
				Scalar skyIntensityScale );

			RISEPel GetRadiance( const Ray& ray, const RasterizerState& rast ) const override;
			Scalar  GetRadianceNM( const Ray& ray, const RasterizerState& rast, const Scalar nm ) const override;

			void SetOrientation( const Vector3& orient ) override;
			void SetTransformation( const Matrix4& mx ) override;

			const IPainter& GetPainter() const override;
			Scalar          GetScale()   const override { return skyScale; }
			void            SetScale( const Scalar scale ) override { skyScale = scale; }
			const Matrix4&  GetTransform() const override { return mxTransform; }

			// Accessors used by the matched-sun directional_light.
			Vector3 SunDirection() const { return model.SunDirection(); }
			Scalar  SampleSolarRadiance( Scalar nm ) const { return model.SampleSolarRadiance( nm ); }
		};
	}
}

#endif
