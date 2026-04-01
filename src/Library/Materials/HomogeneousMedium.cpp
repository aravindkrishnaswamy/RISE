//////////////////////////////////////////////////////////////////////
//
//  HomogeneousMedium.cpp - Implementation of the homogeneous
//    participating medium
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
#include "HomogeneousMedium.h"
#include <math.h>

using namespace RISE;

HomogeneousMedium::HomogeneousMedium(
	const RISEPel& sigma_a,
	const RISEPel& sigma_s,
	const IPhaseFunction& phase
	) :
  m_sigma_a( sigma_a ),
  m_sigma_s( sigma_s ),
  m_sigma_t( sigma_a + sigma_s ),
  m_emission( 0, 0, 0 ),
  m_sigma_t_max( ColorMath::MaxValue( sigma_a + sigma_s ) ),
  m_pPhase( &phase )
{
	m_pPhase->addref();
}

HomogeneousMedium::HomogeneousMedium(
	const RISEPel& sigma_a,
	const RISEPel& sigma_s,
	const RISEPel& emission,
	const IPhaseFunction& phase
	) :
  m_sigma_a( sigma_a ),
  m_sigma_s( sigma_s ),
  m_sigma_t( sigma_a + sigma_s ),
  m_emission( emission ),
  m_sigma_t_max( ColorMath::MaxValue( sigma_a + sigma_s ) ),
  m_pPhase( &phase )
{
	m_pPhase->addref();
}

HomogeneousMedium::~HomogeneousMedium()
{
	safe_release( m_pPhase );
}

MediumCoefficients HomogeneousMedium::GetCoefficients(
	const Point3& pt
	) const
{
	// Homogeneous: coefficients are the same everywhere
	MediumCoefficients c;
	c.sigma_t = m_sigma_t;
	c.sigma_s = m_sigma_s;
	c.emission = m_emission;
	return c;
}

MediumCoefficientsNM HomogeneousMedium::GetCoefficientsNM(
	const Point3& pt,
	const Scalar nm
	) const
{
	// For homogeneous RGB medium, use the luminance-weighted
	// average of the channels as the spectral approximation.
	// A true spectral medium would query a spectral extinction
	// function here.
	MediumCoefficientsNM c;
	c.sigma_t = ColorMath::Luminance( m_sigma_t );
	c.sigma_s = ColorMath::Luminance( m_sigma_s );
	c.emission = ColorMath::Luminance( m_emission );
	return c;
}

const IPhaseFunction* HomogeneousMedium::GetPhaseFunction() const
{
	return m_pPhase;
}

//
// Sample a free-flight distance using exponential distribution.
//
// For a homogeneous medium with extinction sigma_t, the PDF for
// the next scatter event at distance t is:
//   p(t) = sigma_t * exp(-sigma_t * t)
//
// The CDF is:
//   P(t) = 1 - exp(-sigma_t * t)
//
// Inverse transform sampling:
//   t = -ln(1 - xi) / sigma_t
//
// We use the max channel of sigma_t for sampling to ensure the
// sampled distance is valid across all channels.  The per-channel
// transmittance difference is accounted for in the weight.
//
// Reference: Cycles volume_integrate_homogeneous() in
// intern/cycles/kernel/integrator/shade_volume.h
//
Scalar HomogeneousMedium::SampleDistance(
	const Ray& ray,
	const Scalar maxDist,
	ISampler& sampler,
	bool& scattered
	) const
{
	if( m_sigma_t_max <= 0.0 ) {
		// Purely transparent medium — no interaction possible
		scattered = false;
		return maxDist;
	}

	const Scalar xi = sampler.Get1D();

	// Avoid log(0) by clamping xi away from 1
	const Scalar t = -log( fmax( 1.0 - xi, 1e-30 ) ) / m_sigma_t_max;

	if( t < maxDist ) {
		scattered = true;
		return t;
	} else {
		scattered = false;
		return maxDist;
	}
}

Scalar HomogeneousMedium::SampleDistanceNM(
	const Ray& ray,
	const Scalar maxDist,
	const Scalar nm,
	ISampler& sampler,
	bool& scattered
	) const
{
	const Scalar sigma_t_nm = ColorMath::Luminance( m_sigma_t );

	if( sigma_t_nm <= 0.0 ) {
		scattered = false;
		return maxDist;
	}

	const Scalar xi = sampler.Get1D();
	const Scalar t = -log( fmax( 1.0 - xi, 1e-30 ) ) / sigma_t_nm;

	if( t < maxDist ) {
		scattered = true;
		return t;
	} else {
		scattered = false;
		return maxDist;
	}
}

//
// Evaluate transmittance along a ray segment using Beer-Lambert law.
//
// For homogeneous media:
//   T(d) = exp(-sigma_t * d)
//
// This is evaluated per-channel to handle colored media correctly.
//
RISEPel HomogeneousMedium::EvalTransmittance(
	const Ray& ray,
	const Scalar dist
	) const
{
	return RISEPel(
		exp( -m_sigma_t[0] * dist ),
		exp( -m_sigma_t[1] * dist ),
		exp( -m_sigma_t[2] * dist )
	);
}

Scalar HomogeneousMedium::EvalTransmittanceNM(
	const Ray& ray,
	const Scalar dist,
	const Scalar nm
	) const
{
	const Scalar sigma_t_nm = ColorMath::Luminance( m_sigma_t );
	return exp( -sigma_t_nm * dist );
}

bool HomogeneousMedium::IsHomogeneous() const
{
	return true;
}
