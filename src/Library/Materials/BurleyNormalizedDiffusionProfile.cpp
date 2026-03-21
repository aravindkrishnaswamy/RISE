//////////////////////////////////////////////////////////////////////
//
//  BurleyNormalizedDiffusionProfile.cpp - Implementation of Burley's
//  normalized diffusion profile for subsurface scattering.
//
//  See BurleyNormalizedDiffusionProfile.h for the mathematical
//  formulation and references.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 21, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "BurleyNormalizedDiffusionProfile.h"

using namespace RISE;
using namespace RISE::Implementation;

BurleyNormalizedDiffusionProfile::BurleyNormalizedDiffusionProfile(
	const IPainter& ior_,
	const IPainter& absorption_,
	const IPainter& scattering_,
	const Scalar g_
	) :
  ior( ior_ ),
  absorption( absorption_ ),
  scattering( scattering_ ),
  g( g_ )
{
	ior.addref();
	absorption.addref();
	scattering.addref();
}

BurleyNormalizedDiffusionProfile::~BurleyNormalizedDiffusionProfile()
{
	ior.release();
	absorption.release();
	scattering.release();
}

//=============================================================
// Static helpers
//=============================================================

Scalar BurleyNormalizedDiffusionProfile::ComputeScalingFactor( const Scalar A )
{
	// Christensen & Burley 2015, empirical fit for the scaling factor.
	// s controls the width of the profile relative to the mean free path.
	// For low albedo, the profile is narrow (light is quickly absorbed).
	// For high albedo, the profile is wide (light scatters many times).
	const Scalar diff = A - 0.8;
	return 1.9 - A + 3.5 * diff * diff;
}

Scalar BurleyNormalizedDiffusionProfile::SchlickFresnel(
	const Scalar cosTheta,
	const Scalar eta
	)
{
	// Schlick's approximation for Fresnel reflectance.
	// R0 = ((eta_i - eta_t) / (eta_i + eta_t))^2
	// F(cos) = R0 + (1 - R0) * (1 - cos)^5
	const Scalar R0 = ((1.0 - eta) / (1.0 + eta)) * ((1.0 - eta) / (1.0 + eta));
	const Scalar c = 1.0 - cosTheta;
	const Scalar c2 = c * c;
	return R0 + (1.0 - R0) * c2 * c2 * c;
}

//=============================================================
// Profile evaluation
//=============================================================

/// Evaluates Rd(r) for a single channel given precomputed albedo,
/// mean free path, and scaling factor.
///
/// Rd(r) = A * s / (8*pi*d) * (exp(-s*r/d) + exp(-s*r/(3*d))) / r
///
/// For r approaching 0, the 1/r singularity is integrable (the
/// 2D integral over a disk converges).  We clamp to a small minimum
/// to avoid numerical overflow.
static Scalar EvaluateRdChannel(
	const Scalar r,
	const Scalar albedo,
	const Scalar mfp,
	const Scalar s
	)
{
	if( albedo < 1e-10 || mfp < 1e-10 ) return 0.0;

	const Scalar rClamped = (r < 1e-10) ? 1e-10 : r;
	const Scalar sr_over_d = s * rClamped / mfp;

	// Sum of two exponentials: the first captures multiple scattering
	// (broader, slower decay), the second captures single scattering
	// (narrower, faster decay with 3x the rate).
	const Scalar term1 = exp( -sr_over_d );
	const Scalar term2 = exp( -sr_over_d / 3.0 );

	return albedo * s / (8.0 * PI * mfp) * (term1 + term2) / rClamped;
}

RISEPel BurleyNormalizedDiffusionProfile::EvaluateProfile(
	const Scalar r,
	const RayIntersectionGeometric& ri
	) const
{
	const RISEPel sa = absorption.GetColor( ri );
	const RISEPel ss = scattering.GetColor( ri );

	RISEPel result;
	for( int c = 0; c < 3; c++ )
	{
		// Similarity relation: reduce scattering to account for
		// anisotropy of the phase function (Wyman et al. 1989)
		const Scalar ss_prime = ss[c] * (1.0 - g);
		const Scalar st_prime = ss_prime + sa[c];
		const Scalar albedo = st_prime > 1e-10 ? ss_prime / st_prime : 0.0;
		const Scalar mfp = st_prime > 1e-10 ? 1.0 / st_prime : 0.0;
		const Scalar s = ComputeScalingFactor( albedo );

		result[c] = EvaluateRdChannel( r, albedo, mfp, s );
	}

	return result;
}

Scalar BurleyNormalizedDiffusionProfile::EvaluateProfileNM(
	const Scalar r,
	const RayIntersectionGeometric& ri,
	const Scalar nm
	) const
{
	const Scalar sa = absorption.GetColorNM( ri, nm );
	const Scalar ss = scattering.GetColorNM( ri, nm );

	const Scalar ss_prime = ss * (1.0 - g);
	const Scalar st_prime = ss_prime + sa;
	const Scalar albedo = st_prime > 1e-10 ? ss_prime / st_prime : 0.0;
	const Scalar mfp = st_prime > 1e-10 ? 1.0 / st_prime : 0.0;
	const Scalar s = ComputeScalingFactor( albedo );

	return EvaluateRdChannel( r, albedo, mfp, s );
}

//=============================================================
// Importance sampling
//=============================================================

Scalar BurleyNormalizedDiffusionProfile::SampleRadius(
	const Scalar u,
	const int channel,
	const RayIntersectionGeometric& ri
	) const
{
	const RISEPel sa = absorption.GetColor( ri );
	const RISEPel ss = scattering.GetColor( ri );

	const Scalar ss_prime = ss[channel] * (1.0 - g);
	const Scalar st_prime = ss_prime + sa[channel];
	const Scalar albedo = st_prime > 1e-10 ? ss_prime / st_prime : 0.0;
	const Scalar mfp = st_prime > 1e-10 ? 1.0 / st_prime : 0.0;
	const Scalar s = ComputeScalingFactor( albedo );

	if( mfp < 1e-10 || s < 1e-10 ) return 0.0;

	// The profile's radial CDF is a 50/50 mixture of two exponential
	// distributions with rates s/d and s/(3d).  To sample:
	//   1. Choose which exponential with probability 0.5 each
	//   2. Invert the chosen exponential CDF
	//
	// If u < 0.5: sample from the sharper exponential (rate = s/d)
	//   r = -d/s * ln(1 - 2*u)
	// If u >= 0.5: sample from the broader exponential (rate = s/(3d))
	//   r = -3*d/s * ln(1 - 2*(u - 0.5))
	//
	// The factor of 2 rescales u into [0,1) for each half.
	if( u < 0.5 )
	{
		const Scalar u_remapped = 2.0 * u;
		// Clamp to avoid log(0)
		const Scalar one_minus_u = (u_remapped > 0.999999) ? 1e-6 : (1.0 - u_remapped);
		return -mfp / s * log( one_minus_u );
	}
	else
	{
		const Scalar u_remapped = 2.0 * (u - 0.5);
		const Scalar one_minus_u = (u_remapped > 0.999999) ? 1e-6 : (1.0 - u_remapped);
		return -3.0 * mfp / s * log( one_minus_u );
	}
}

Scalar BurleyNormalizedDiffusionProfile::PdfRadius(
	const Scalar r,
	const int channel,
	const RayIntersectionGeometric& ri
	) const
{
	const RISEPel sa = absorption.GetColor( ri );
	const RISEPel ss = scattering.GetColor( ri );

	const Scalar ss_prime = ss[channel] * (1.0 - g);
	const Scalar st_prime = ss_prime + sa[channel];
	const Scalar albedo = st_prime > 1e-10 ? ss_prime / st_prime : 0.0;
	const Scalar mfp = st_prime > 1e-10 ? 1.0 / st_prime : 0.0;
	const Scalar s = ComputeScalingFactor( albedo );

	if( mfp < 1e-10 || s < 1e-10 ) return 0.0;

	// The sampling PDF is the 50/50 mixture of two exponential PDFs.
	// For exponential with rate lambda, PDF(r) = lambda * exp(-lambda * r).
	// Rate 1: lambda_1 = s/d
	// Rate 2: lambda_2 = s/(3d)
	// PDF(r) = 0.5 * lambda_1 * exp(-lambda_1 * r) + 0.5 * lambda_2 * exp(-lambda_2 * r)
	const Scalar lambda1 = s / mfp;
	const Scalar lambda2 = s / (3.0 * mfp);

	return 0.5 * lambda1 * exp( -lambda1 * r )
	     + 0.5 * lambda2 * exp( -lambda2 * r );
}

//=============================================================
// Fresnel and IOR
//=============================================================

Scalar BurleyNormalizedDiffusionProfile::FresnelTransmission(
	const Scalar cosTheta,
	const RayIntersectionGeometric& ri
	) const
{
	const Scalar eta = ior.GetColor( ri )[0];
	return 1.0 - SchlickFresnel( fabs(cosTheta), eta );
}

Scalar BurleyNormalizedDiffusionProfile::GetIOR(
	const RayIntersectionGeometric& ri
	) const
{
	return ior.GetColor( ri )[0];
}

//=============================================================
// ISubSurfaceExtinctionFunction compatibility
//=============================================================

Scalar BurleyNormalizedDiffusionProfile::GetMaximumDistanceForError(
	const Scalar error
	) const
{
	if( error <= 0 ) return RISE_INFINITY;

	// The slowest-decaying term is exp(-s*r/(3*d)).  Setting this
	// equal to the error tolerance and solving for r gives:
	//   r_max = -3*d/s * ln(error)
	// We use conservative values (largest mfp, smallest s across
	// channels) since we don't have ri here.  For the interface
	// compatibility, use a reasonable default.
	//
	// Since this method doesn't have access to ri for texture
	// lookups, we return a generous upper bound.  The actual
	// sampling in the integrator uses the per-channel profile
	// which has proper bounds.
	return -log(error) * 10.0;
}

RISEPel BurleyNormalizedDiffusionProfile::ComputeTotalExtinction(
	const Scalar distance
	) const
{
	// This method exists for ISubSurfaceExtinctionFunction
	// compatibility.  Without ri, we cannot evaluate the profile
	// accurately.  Return zero — the actual evaluation is done
	// through EvaluateProfile() which has access to ri.
	return RISEPel( 0, 0, 0 );
}
