//////////////////////////////////////////////////////////////////////
//
//  HosekWilkieSpectralRadianceMap.cpp - See header.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#define _USE_MATH_DEFINES
#include "HosekWilkieSpectralRadianceMap.h"
#include "../Painters/Painter.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../RISE_API.h"
#include "../Utilities/Color/ColorMath.h"
#include "../Utilities/Color/ColorConversion.h"
#include "../Interfaces/ILog.h"
#include "../Interfaces/IPainter.h"

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	// Adapter painter: EnvironmentSampler's CDF builder iterates a UV
	// grid, asks the painter for radiance per cell, and accumulates
	// luminance.  Without this adapter HW skies appear all-black to
	// the CDF builder (since the radiance comes from the model, not
	// the painter), and env importance sampling is disabled — the
	// sun-disc gets missed by NEE.
	//
	// UV→direction inverse is the lat-long projection used by
	// RadianceMap: u maps to azimuth, v maps to elevation.  The
	// encoding in RadianceMap.h is non-standard
	// (`0.5 + v.x * r * acos(-v.z) / sqrt(v.x²+v.y²)`); we use the
	// standard equirectangular form here for the adapter, which is
	// what most env-CDF builders assume.  The actual radiance the
	// path tracer reads still comes from `GetRadiance` / `GetRadianceNM`
	// (which use the un-mapped direction directly), so the adapter's
	// projection only affects CDF weighting, not rendered radiance.
	class HWAdapterPainter : public Painter
	{
		const HosekWilkieSkyModel* model;
	protected:
		virtual ~HWAdapterPainter() {}
	public:
		HWAdapterPainter( const HosekWilkieSkyModel* m ) : model( m ) {}

		static Vector3 UVtoDir( const Point2& uv )
		{
			// Equirectangular: u ∈ [0,1] → φ ∈ [0, 2π];
			// v ∈ [0,1] → θ ∈ [0, π] (0 = north pole/zenith).
			const Scalar phi   = uv.x * Scalar(2.0 * M_PI);
			const Scalar theta = uv.y * Scalar(M_PI);
			return Vector3( std::sin(theta) * std::sin(phi),
			                std::cos(theta),
			                std::sin(theta) * std::cos(phi) );
		}

		RISEPel GetColor( const RayIntersectionGeometric& ri ) const
		{
			const Vector3 dir = UVtoDir( ri.ptCoord );
			return model->IntegrateRGB( dir );
		}

		Scalar GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
		{
			const Vector3 dir = UVtoDir( ri.ptCoord );
			return model->SampleRadiance( dir, nm );
		}

		Scalar GetAlpha( const RayIntersectionGeometric& ) const { return Scalar(1); }

		IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) { return 0; }
		void SetIntermediateValue( const IKeyframeParameter& ) {}
		void RegenerateData() {}
	};
}

HosekWilkieSpectralRadianceMap::HosekWilkieSpectralRadianceMap(
	Scalar solarElevationRadians,
	Scalar solarAzimuthRadians,
	Scalar T,
	const RISEPel& albedo,
	Scalar skyIntensityScale ) :
  model( solarElevationRadians, solarAzimuthRadians, T, albedo ),
  skyScale( skyIntensityScale ),
  pNullPainter( nullptr )
{
	mxTransform = Matrix4Ops::Identity();
	// GetPainter() must return a painter whose CDF integral is
	// non-trivial — EnvironmentSampler builds its importance-sampling
	// CDF by scanning the painter on a UV grid; an all-black painter
	// disables NEE for the sky entirely (the original bug round 2
	// caught).  HWAdapterPainter back-references the model so the CDF
	// reflects HW radiance.
	pNullPainter = new HWAdapterPainter( &model );
	pNullPainter->addref();
}

HosekWilkieSpectralRadianceMap::~HosekWilkieSpectralRadianceMap()
{
	if( pNullPainter ) {
		pNullPainter->release();
		pNullPainter = nullptr;
	}
}

RISEPel HosekWilkieSpectralRadianceMap::GetRadiance(
	const Ray& ray, const RasterizerState& /*rast*/ ) const
{
	const Vector3 dir = Vector3Ops::Transform( mxTransform, ray.Dir() );
	const Vector3 dirN = Vector3Ops::Normalize( dir );
	return model.IntegrateRGB( dirN ) * skyScale;
}

Scalar HosekWilkieSpectralRadianceMap::GetRadianceNM(
	const Ray& ray, const RasterizerState& /*rast*/, const Scalar nm ) const
{
	const Vector3 dir = Vector3Ops::Transform( mxTransform, ray.Dir() );
	const Vector3 dirN = Vector3Ops::Normalize( dir );
	return model.SampleRadiance( dirN, nm ) * skyScale;
}

void HosekWilkieSpectralRadianceMap::SetOrientation( const Vector3& orient )
{
	mxTransform = Matrix4Ops::XRotation( orient.x ) *
	              Matrix4Ops::YRotation( orient.y ) *
	              Matrix4Ops::ZRotation( orient.z );
}

void HosekWilkieSpectralRadianceMap::SetTransformation( const Matrix4& mx )
{
	mxTransform = mx;
}

const IPainter& HosekWilkieSpectralRadianceMap::GetPainter() const
{
	// Should never be reached for HW skies (radiance comes from
	// GetRadiance / GetRadianceNM); the placeholder is here only to
	// satisfy the pure-virtual contract.
	return *pNullPainter;
}
