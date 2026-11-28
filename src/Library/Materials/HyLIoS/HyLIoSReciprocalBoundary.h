//////////////////////////////////////////////////////////////////////
//
//  HyLIoSReciprocalBoundary.h - Named reciprocal dielectric microfacet
//    boundary extension (`rise_reciprocal_boundary_v1`) for P4/P7
//    nonlocal HyLIoS camera transport, external reflection NEE (K_r),
//    and conditional interior-exit transmission NEE (K_t).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_HYLIOS_RECIPROCAL_BOUNDARY_H
#define RISE_HYLIOS_RECIPROCAL_BOUNDARY_H

#include "HyLIoSSlabTransport.h"
#include <string>

namespace RISE::HyLIoS {

inline constexpr const char* ReciprocalBoundaryVersion = "rise_reciprocal_boundary_v1";

struct ReciprocalBoundarySample {
	bool valid = false;
	bool isReflection = true;
	bool isDelta = false;
	SlabVector direction{ 0, 0, 1 }; ///< Outgoing direction in local frame (+Z outward)
	double pdf = 0.0;                ///< Solid-angle PDF (0 for delta)
	double weight = 0.0;             ///< Unbiased throughput factor (f * |cos| / pdf, or F for delta)
};

//! Two-sided reciprocal dielectric GGX microfacet boundary (`rise_reciprocal_boundary_v1`).
//! Local frame convention: +Z is the outward geometric/macro normal (into exterior medium
//! `etaExt`), -Z points into the stratum corneum (`etaInt`).
//! All direction vectors `wi`, `wo` point AWAY from the interface (standard BSDF convention).
class HyLIoSReciprocalBoundary final {
public:
	//! `roughnessAlpha == 0` constructs an exact smooth dielectric interface (delta-only;
	//! refused for delta-light NEE).  Positive `roughnessAlpha` in `[1e-3, 1.0]` uses
	//! isotropic GGX with Smith height-correlated masking-shadowing and VNDF sampling.
	HyLIoSReciprocalBoundary( double roughnessAlpha, double etaExterior, double etaInterior );

	double RoughnessAlpha() const noexcept { return alpha_; }
	double EtaExterior() const noexcept { return etaExt_; }
	double EtaInterior() const noexcept { return etaInt_; }
	bool   IsSmooth() const noexcept { return alpha_ == 0.0; }

	//! Mechanical guard: delta lights cannot perform NEE through a smooth delta boundary.
	bool AllowsDeltaLightNEE() const noexcept { return !IsSmooth(); }

	//! Exact dielectric Fresnel reflectance for unpolarized light.
	static double FresnelDielectric( double cosThetaI, double etaI, double etaT ) noexcept;

	//! GGX normal distribution D(h) for h.z > 0.
	double EvalD( const SlabVector& h ) const noexcept;

	//! Smith Lambda(w) for isotropic GGX.
	double Lambda( const SlabVector& w ) const noexcept;

	//! Smith G1(w) and height-correlated G2(wi, wo).
	double EvalG1( const SlabVector& w ) const noexcept;
	double EvalG2( const SlabVector& wi, const SlabVector& wo ) const noexcept;

	//! External reflection BSDF K_r(wi, wo) [sr^-1], wi.z > 0, wo.z > 0.
	//! Strictly reciprocal: EvalReflection(wi, wo) == EvalReflection(wo, wi).
	double EvalReflection( const SlabVector& wi, const SlabVector& wo ) const noexcept;

	//! Solid-angle PDF of VNDF reflection sampling for wi.z > 0 given wo.z > 0.
	double PdfReflection( const SlabVector& wi, const SlabVector& wo ) const noexcept;

	//! Complete-event solid-angle PDF `q_B(event=R, wi | wo) = F(wo.h) * PdfReflection(wi, wo)`
	//! matching `SampleFull`'s reflection branch for MIS with `K_r` NEE.
	double PdfReflectionBranch( const SlabVector& wi, const SlabVector& wo ) const noexcept;

	//! Conditional transmission BSDF K_t(wi, wo) [sr^-1] across the dielectric interface
	//! for opposite-hemisphere directions (wi.z * wo.z < 0).
	//! Radiance-transport convention: when light arrives from `wi` (medium etaI) and exits
	//! toward camera `wo` (medium etaO), satisfies exact adjoint reciprocity:
	//!   etaI^2 * EvalTransmission(wi, wo) == etaO^2 * EvalTransmission(wo, wi)
	//! (where `wi` is on side I and `wo` is on side O).
	double EvalTransmission( const SlabVector& wi, const SlabVector& wo ) const noexcept;

	//! Solid-angle PDF of VNDF transmission sampling for `wi` given `wo` (opposite hemispheres).
	double PdfTransmission( const SlabVector& wi, const SlabVector& wo ) const noexcept;

	//! Complete-event solid-angle PDF `q_B(event=T, wi | wo) = (1 - F(wo.h)) * PdfTransmission(wi, wo)`
	//! matching `SampleFull`'s transmission branch for MIS with `K_t` NEE.
	double PdfTransmissionBranch( const SlabVector& wi, const SlabVector& wo ) const noexcept;

	//! Samples a reflection direction given `wo` (same hemisphere as `wo`).
	ReciprocalBoundarySample SampleReflection( const SlabVector& wo, double u1, double u2 ) const noexcept;

	//! Samples a transmission direction given `wo` (opposite hemisphere from `wo`).
	//! Returns `valid = false` on total internal reflection.
	ReciprocalBoundarySample SampleTransmission( const SlabVector& wo, double u1, double u2 ) const noexcept;

	//! Full two-sided Fresnel-branched boundary interaction (reflects or transmits using
	//! VNDF microfacet normal `h` and exact dielectric Fresnel at `h`).
	//! `uBranch`, `u1`, `u2` in [0, 1).
	ReciprocalBoundarySample SampleFull( const SlabVector& wo, double uBranch, double u1, double u2 ) const noexcept;

private:
	SlabVector SampleVNDF( const SlabVector& woUpper, double u1, double u2 ) const noexcept;

	double alpha_;
	double etaExt_;
	double etaInt_;
};

} // namespace RISE::HyLIoS

#endif // RISE_HYLIOS_RECIPROCAL_BOUNDARY_H
