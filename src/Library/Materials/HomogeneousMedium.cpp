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

HomogeneousMedium::HomogeneousMedium(
	const RISEPel& sigma_a,
	const RISEPel& sigma_s,
	const RISEPel& emission,
	const IFunction1D* sigma_a_spectral,
	const IFunction1D* sigma_s_spectral,
	const IPhaseFunction& phase
	) :
  m_sigma_a( sigma_a ),
  m_sigma_s( sigma_s ),
  m_sigma_t( sigma_a + sigma_s ),
  m_emission( emission ),
  m_sigma_t_max( ColorMath::MaxValue( sigma_a + sigma_s ) ),
  m_pPhase( &phase ),
  m_sigma_a_spectral( sigma_a_spectral ),
  m_sigma_s_spectral( sigma_s_spectral )
{
	m_pPhase->addref();
	if( m_sigma_a_spectral ) m_sigma_a_spectral->addref();
	if( m_sigma_s_spectral ) m_sigma_s_spectral->addref();
}

HomogeneousMedium::~HomogeneousMedium()
{
	safe_release( m_pPhase );
	safe_release( m_sigma_a_spectral );
	safe_release( m_sigma_s_spectral );
}

void HomogeneousMedium::SetAbsorption( const RISEPel& v )
{
	m_sigma_a = v;
	m_sigma_t = m_sigma_a + m_sigma_s;
	m_sigma_t_max = ColorMath::MaxValue( m_sigma_t );
}

void HomogeneousMedium::SetScattering( const RISEPel& v )
{
	m_sigma_s = v;
	m_sigma_t = m_sigma_a + m_sigma_s;
	m_sigma_t_max = ColorMath::MaxValue( m_sigma_t );
}

void HomogeneousMedium::SetEmission( const RISEPel& v )
{
	// Emission doesn't feed sigma_t / sigma_t_max — sigma_t controls
	// distance sampling, emission contributes radiance.  No derived
	// state refresh needed.
	m_emission = v;
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
	MediumCoefficientsNM c;
	if( m_sigma_a_spectral || m_sigma_s_spectral ) {
		// G1: genuine per-wavelength coefficients.  A null curve
		// contributes zero for that coefficient (a pure-absorber
		// colorant leaves the scattering curve null).  Homogeneous:
		// position is ignored.  Clamp to physical non-negativity: a
		// curve authored with a negative control point would otherwise
		// give sigma_t < 0, so EvalTransmittanceNM = exp(-sigma_t*d) > 1
		// (unphysical energy gain) while SampleDistanceNM treats it as
		// non-scattering — an inconsistency.  Author error, guarded here
		// so both NM paths agree.
		const Scalar sa = m_sigma_a_spectral ? fmax( m_sigma_a_spectral->Evaluate( nm ), 0.0 ) : 0.0;
		const Scalar ss = m_sigma_s_spectral ? fmax( m_sigma_s_spectral->Evaluate( nm ), 0.0 ) : 0.0;
		c.sigma_s = ss;
		c.sigma_t = sa + ss;
	} else {
		// Luminance fallback for RGB-authored media: use the
		// luminance-weighted average of the channels as the spectral
		// approximation.  Byte-identical to the pre-G1 behaviour.
		c.sigma_t = ColorMath::Luminance( m_sigma_t );
		c.sigma_s = ColorMath::Luminance( m_sigma_s );
	}
	// Emission is not yet spectrally authored; use its luminance in both
	// paths (unchanged from pre-G1).
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
	// Per-wavelength extinction: the G1 spectral curve when present,
	// else the luminance fallback (GetCoefficientsNM encapsulates the
	// choice, keeping sampling / transmittance / pdf consistent).
	const Scalar sigma_t_nm = GetCoefficientsNM( ray.origin, nm ).sigma_t;

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
	// Per-wavelength extinction (G1 spectral curve or luminance
	// fallback); shares GetCoefficientsNM with the sampler so
	// transmittance and distance pdf never drift.
	const Scalar sigma_t_nm = GetCoefficientsNM( ray.origin, nm ).sigma_t;
	return exp( -sigma_t_nm * dist );
}

bool HomogeneousMedium::IsHomogeneous() const
{
	return true;
}


