//////////////////////////////////////////////////////////////////////
//
//  EnvironmentSampler.cpp - Implementation of EnvironmentSampler
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 30, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "EnvironmentSampler.h"
#include "RadianceMap.h"
#include <cmath>
#include "../Utilities/Color/ColorMath.h"
#include "../Intersection/RayIntersectionGeometric.h"

using namespace RISE;
using namespace RISE::Implementation;

EnvironmentSampler::EnvironmentSampler(
	const IPainter& p,
	const Scalar scale,
	const Matrix4& transform,
	const unsigned int resolution
	) :
  painter( p ),
  dScale( scale ),
  mxtransform( transform ),
  width( resolution ),
  height( resolution ),
  totalLuminance( 0 )
{
    if (resolution == 0 || resolution == std::numeric_limits<unsigned int>::max()) {
        throw std::invalid_argument("environment proposal resolution must be positive and leave a CDF endpoint");
    }
    if (!RISE::IsFiniteDouble(scale) || scale < 0) {
        throw std::invalid_argument("environment proposal scale must be finite and nonnegative");
    }
	painter.addref();

	// Compute inverse transform (map-space -> world-space).
	// The transform is a rotation, so inverse = transpose.
	mxInvTransform = Matrix4Ops::Transpose( mxtransform );
}

EnvironmentSampler::EnvironmentSampler(const IRadianceMap& map,
    std::shared_ptr<const SpectralSourceDomain> domain) :
    EnvironmentSampler(map.GetPainter(),map.GetScale(),map.GetTransform(),8)
{
    if (!domain) throw std::invalid_argument("missing spectral environment domain");
    spectralDomain = std::move(domain);
    spectralMap = &map;
    spectralMap->addref();
}

EnvironmentSampler::~EnvironmentSampler()
{
	painter.release();
    if (spectralMap) spectralMap->release();
}

Vector3 EnvironmentSampler::EqualAreaToDirection(Scalar u, Scalar v)
{
    const Scalar phi = TWO_PI*u;
    const Scalar z = 1-2*v;
    const Scalar radius = 2*sqrt(r_max(Scalar(0),v*(1-v)));
    return Vector3(radius*cos(phi),radius*sin(phi),z);
}

bool EnvironmentSampler::DirectionToEqualArea(const Vector3& direction, Scalar& u, Scalar& v)
{
    if (!RISE::IsFiniteDouble(direction.x) || !RISE::IsFiniteDouble(direction.y) ||
        !RISE::IsFiniteDouble(direction.z)) return false;
    const Scalar scale = r_max(std::abs(direction.x),r_max(std::abs(direction.y),std::abs(direction.z)));
    if (!(scale > 0)) return false;
    int exponent = 0;
    std::frexp(scale,&exponent);
    const Vector3 d(std::ldexp(direction.x,-exponent),std::ldexp(direction.y,-exponent),std::ldexp(direction.z,-exponent));
    const Scalar length = std::hypot(d.x,d.y,d.z);
    const Scalar horizontal = r_max(std::abs(direction.x),std::abs(direction.y));
    Scalar phi = 0;
    if (horizontal > 0) {
        int horizontalExponent = 0;std::frexp(horizontal,&horizontalExponent);
        phi = std::atan2(std::ldexp(direction.y,-horizontalExponent),std::ldexp(direction.x,-horizontalExponent));
    }
    if (phi < 0) phi += TWO_PI;
    u = phi/TWO_PI;
    v = r_max(Scalar(0),r_min(Scalar(1),(1-d.z/length)*.5));
    return true;
}

unsigned int EnvironmentSampler::SampleCDF(
	const std::vector<Scalar>& cdf,
	const unsigned int n,
	const Scalar xi,
	Scalar& fractional
	)
{
	// Binary search: find largest index i such that cdf[i] <= xi
	unsigned int lo = 0;
	unsigned int hi = n;

	while( lo + 1 < hi )
	{
		const unsigned int mid = (lo + hi) / 2;
		if( cdf[mid] <= xi )
		{
			lo = mid;
		}
		else
		{
			hi = mid;
		}
	}

	// Fractional position within the bin [lo, lo+1)
	const Scalar binWidth = cdf[lo + 1] - cdf[lo];
	if( binWidth > 0 )
	{
		fractional = (xi - cdf[lo]) / binWidth;
	}
	else
	{
		fractional = 0;
	}

	return lo;
}

void EnvironmentSampler::Build()
{
    if (spectralDomain) {
        spectralPower = SpectralSourceDomain::Mass();
        // Equal-solid-angle world-space cell centres. A missed patch cannot
        // remove support: Sample/Pdf remain uniform everywhere.
        for (unsigned int y=0;y<8;++y) for (unsigned int x=0;x<16;++x) {
            const Scalar z = 1-2*(Scalar(y)+.5)/8;
            const Scalar phi = TWO_PI*(Scalar(x)+.5)/16;
            const Scalar r = sqrt(r_max(Scalar(0),1-z*z));
            const Ray ray(Point3(0,0,0),Vector3(r*cos(phi),r*sin(phi),z));
            const auto m = spectralDomain->Integrate([&](double nm) {
                return spectralMap->GetRadianceNM(ray,nullRasterizerState,nm);
            });
            if (m.positive) spectralPower.AddLog(m.logarithm);
        }
        spectralPower.Multiply(4*PI/128);
        return;
    }
    // Build in temporaries. A rejected source leaves no stale CDF valid.
    totalLuminance = 0;
    conditionalCDF.clear(); marginalCDF.clear();
    std::vector<std::vector<Scalar>> rows(height,std::vector<Scalar>(width+1,0));
    std::vector<Scalar> marginal(height+1,0);
    Scalar maximum = 0;
    for (unsigned int y=0;y<height;++y) for (unsigned int x=0;x<width;++x) {
        const Vector3 mapDirection = EqualAreaToDirection((Scalar(x)+.5)/width,(Scalar(y)+.5)/height);
        const Vector3 worldDirection = Vector3Ops::Transform(mxInvTransform,mapDirection);
        RayIntersectionGeometric rig(Ray(Point3(0,0,0),worldDirection),nullRasterizerState);
        rig.ptCoord = LightProbeTextureCoordinates(mapDirection);
        const RISEPel color = painter.GetRadianceColor(rig)*dScale;
        if (!RISE::IsFiniteDouble(color.r) || !RISE::IsFiniteDouble(color.g) || !RISE::IsFiniteDouble(color.b))
            throw std::invalid_argument("environment proposal source must have finite RGB radiance");
        const Scalar weight = r_max(Scalar(0),ColorMath::MaxValue(color));
        rows[y][x+1] = weight;
        maximum = r_max(maximum,weight);
    }
    if (dScale == 0) return; // This explicit multiplicative factor proves no emission.
    Scalar integral = 0;
    Scalar sum = 0;
    if (maximum > 0) {
        // Normalize before summing. Keep the raw midpoint power separate from
        // the proposal's support mixture: zero probes do not prove blackness.
        int exponent = 0, angularExponent = 0;
        const Scalar mantissa = std::frexp(maximum,&exponent);
        for (unsigned int y=0;y<height;++y) for (unsigned int x=0;x<width;++x) {
            rows[y][x+1] = std::ldexp(rows[y][x+1],-exponent)/mantissa;
            sum += rows[y][x+1];
        }
        const Scalar meanAngularFactor = (sum/Scalar(width)/Scalar(height))*FOUR_PI;
        const Scalar angularMantissa = std::frexp(meanAngularFactor,&angularExponent);
        integral = std::ldexp(mantissa*angularMantissa,exponent+angularExponent);
        if (!RISE::IsFiniteDouble(integral) || !(integral > 0))
            throw std::invalid_argument("environment angular power integral is outside the representable range");
    }
    // A disclosed 5% uniform-solid-angle component protects arbitrary source
    // features between midpoint probes. Integrate it into this one table;
    // there is no extra mixture draw or conditional random-number remapping.
    const Scalar uniformCell = 1/Scalar(width)/Scalar(height);
    for (unsigned int y=0;y<height;++y) {
        Scalar row = 0;
        for (unsigned int x=0;x<width;++x) {
            const Scalar probability = sum > 0 ?
                (1-UniformSupportFraction())*(rows[y][x+1]/sum)+UniformSupportFraction()*uniformCell : uniformCell;
            row += probability;
            rows[y][x+1] = row;
        }
        marginal[y+1] = marginal[y]+row;
        for (unsigned int x=1;x<=width;++x) rows[y][x] /= row;
        rows[y][width] = 1;
    }
    for (unsigned int y=1;y<=height;++y) marginal[y] /= marginal[height];
    marginal[height] = 1;
    conditionalCDF.swap(rows); marginalCDF.swap(marginal);
    totalLuminance = integral;
}

void EnvironmentSampler::Sample(
	const Scalar u1,
	const Scalar u2,
	Vector3& direction,
	Scalar& pdf
	) const
{
    if (spectralDomain) {
        const Scalar z = 1-2*u1;
        const Scalar phi = TWO_PI*u2;
        const Scalar r = sqrt(r_max(Scalar(0),1-z*z));
        direction = Vector3(r*cos(phi),r*sin(phi),z);
        pdf = 1/(4*PI);
        return;
    }
    if (!IsValid()) {
        direction = Vector3(0,0,1); pdf = 0; return;
    }
    Scalar fractionalV=0,fractionalU=0;
    const unsigned int y = SampleCDF(marginalCDF,height,u2,fractionalV);
    const unsigned int x = SampleCDF(conditionalCDF[y],width,u1,fractionalU);
    const Vector3 mapDirection = EqualAreaToDirection((Scalar(x)+fractionalU)/width,
        (Scalar(y)+fractionalV)/height);
    direction = Vector3Ops::Transform(mxInvTransform,mapDirection);
    // A PDF is defined only almost everywhere. At poles and cell seams use
    // the same canonical world-direction lookup as every MIS partner, also
    // avoiding disagreement from floating-point rotation round trips.
    pdf = Pdf(direction);
}

Scalar EnvironmentSampler::Pdf(const Vector3& direction) const
{
    if (spectralDomain) return 1/(4*PI);
    if (!IsValid()) return 0;
    Scalar u=0,v=0;
    if (!DirectionToEqualArea(Vector3Ops::Transform(mxtransform,direction),u,v)) return 0;
    const unsigned int x = r_min(static_cast<unsigned int>(u*width),width-1);
    const unsigned int y = r_min(static_cast<unsigned int>(v*height),height-1);
    const Scalar cellProbability = (marginalCDF[y+1]-marginalCDF[y])*
        (conditionalCDF[y][x+1]-conditionalCDF[y][x]);
    return cellProbability*Scalar(width)*Scalar(height)/FOUR_PI;
}
