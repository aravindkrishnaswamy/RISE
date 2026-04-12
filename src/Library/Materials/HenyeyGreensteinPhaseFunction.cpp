//////////////////////////////////////////////////////////////////////
//
//  HenyeyGreensteinPhaseFunction.cpp - Implementation of the
//    Henyey-Greenstein phase function
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 31, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "HenyeyGreensteinPhaseFunction.h"
#include <math.h>

using namespace RISE;

HenyeyGreensteinPhaseFunction::HenyeyGreensteinPhaseFunction(
	const Scalar g
	) :
  m_g( g )
{
}

HenyeyGreensteinPhaseFunction::~HenyeyGreensteinPhaseFunction()
{
}

//
// Static: Evaluate the HG phase function given cos(theta) and g
//
// Formula (Henyey & Greenstein, 1941):
//   p(cos_theta, g) = (1 - g^2) / (4*PI * (1 + g^2 - 2*g*cos_theta)^(3/2))
//
// The denominator is always positive for |g| < 1.  When g is exactly
// zero the expression reduces to 1/(4*PI) (isotropic).
//
Scalar HenyeyGreensteinPhaseFunction::EvaluateWithG(
	const Scalar cosTheta,
	const Scalar g
	)
{
	const Scalar g2 = g * g;
	const Scalar denom = 1.0 + g2 - 2.0 * g * cosTheta;

	// Guard against numerical issues when g is very close to +-1
	if( denom <= 0.0 ) {
		return 0.0;
	}

	return (1.0 - g2) / (FOUR_PI * denom * sqrt( denom ));
}

//
// Static: Sample a direction from the HG distribution using the
// closed-form inverse CDF (Kulla & Conty 2017 form, equivalent to
// the classic derivation).
//
// For g != 0:
//   cos_theta = (1/(2g)) * (1 + g^2 - ((1-g^2)/(1-g+2g*xi))^2)
//
// For g == 0:
//   cos_theta = 1 - 2*xi   (uniform/isotropic)
//
// The sampled cos_theta is then converted to a world-space direction
// by perturbing the incoming direction using GeometricUtilities::Perturb.
//
Vector3 HenyeyGreensteinPhaseFunction::SampleWithG(
	const Vector3& wi,
	ISampler& sampler,
	const Scalar g
	)
{
	Scalar cos_theta;

	if( fabs( g ) < 1e-10 ) {
		// Isotropic case — uniform cosine
		cos_theta = 1.0 - 2.0 * sampler.Get1D();
	} else {
		// HG inverse CDF
		const Scalar inner = (1.0 - g * g) / (1.0 - g + 2.0 * g * sampler.Get1D());
		cos_theta = (1.0 / (2.0 * g)) * (1.0 + g * g - inner * inner);
	}

	// Clamp to valid range to handle numerical edge cases
	cos_theta = fmax( -1.0, fmin( 1.0, cos_theta ) );

	// Convert to polar angle and random azimuth, then perturb incoming direction
	const Scalar theta = acos( cos_theta );
	const Scalar phi = TWO_PI * sampler.Get1D();

	return Vector3Ops::Normalize( GeometricUtilities::Perturb( wi, theta, phi ) );
}


Scalar HenyeyGreensteinPhaseFunction::Evaluate(
	const Vector3& wi,
	const Vector3& wo
	) const
{
	const Scalar cosTheta = Vector3Ops::Dot( wi, wo );
	return EvaluateWithG( cosTheta, m_g );
}

Vector3 HenyeyGreensteinPhaseFunction::Sample(
	const Vector3& wi,
	ISampler& sampler
	) const
{
	return SampleWithG( wi, sampler, m_g );
}

Scalar HenyeyGreensteinPhaseFunction::Pdf(
	const Vector3& wi,
	const Vector3& wo
	) const
{
	// For HG, the PDF equals the phase function value (it is normalized)
	return Evaluate( wi, wo );
}
