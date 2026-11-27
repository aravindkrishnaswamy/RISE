//////////////////////////////////////////////////////////////////////
//
//  HyLIoSReciprocalBoundary.cpp - Strict-FP implementation of the
//    named reciprocal dielectric microfacet boundary extension
//    (`rise_reciprocal_boundary_v1`).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#if defined(__FAST_MATH__)
#error "HyLIoSReciprocalBoundary.cpp must be compiled without -ffast-math"
#endif

#include "pch.h"
#include "HyLIoSReciprocalBoundary.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace RISE::HyLIoS {

namespace {

constexpr double kPi = 3.1415926535897932384626433832795;

double Dot( const SlabVector& a, const SlabVector& b ) noexcept
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

double LenSq( const SlabVector& v ) noexcept
{
	return v.x * v.x + v.y * v.y + v.z * v.z;
}

bool Normalize( const SlabVector& v, SlabVector& out ) noexcept
{
	const double l2 = LenSq( v );
	if( !( l2 > 0.0 ) || !std::isfinite( l2 ) ) {
		return false;
	}
	const double inv = 1.0 / std::sqrt( l2 );
	out = { v.x * inv, v.y * inv, v.z * inv };
	return true;
}

double ClampUnit( const double u ) noexcept
{
	if( !std::isfinite( u ) || u <= 0.0 ) return 0.0;
	if( u >= 1.0 ) return std::nextafter( 1.0, 0.0 );
	return u;
}

} // namespace

HyLIoSReciprocalBoundary::HyLIoSReciprocalBoundary(
	const double roughnessAlpha,
	const double etaExterior,
	const double etaInterior
	) :
  alpha_( roughnessAlpha ),
  etaExt_( etaExterior ),
  etaInt_( etaInterior )
{
	if( !std::isfinite( alpha_ ) || alpha_ < 0.0 || alpha_ > 1.0 ) {
		throw std::invalid_argument( "rise_reciprocal_boundary_v1: roughnessAlpha must be in [0, 1]" );
	}
	if( alpha_ > 0.0 && alpha_ < 1e-3 ) {
		throw std::invalid_argument( "rise_reciprocal_boundary_v1: positive roughnessAlpha must be >= 1e-3 (use 0 for smooth delta)" );
	}
	if( !std::isfinite( etaExt_ ) || etaExt_ <= 0.0 || !std::isfinite( etaInt_ ) || etaInt_ <= 0.0 ) {
		throw std::invalid_argument( "rise_reciprocal_boundary_v1: refractive indices must be finite and positive" );
	}
}

double HyLIoSReciprocalBoundary::FresnelDielectric(
	const double cosThetaI,
	const double etaI,
	const double etaT
	) noexcept
{
	const double cosI = std::clamp( std::fabs( cosThetaI ), 0.0, 1.0 );
	const double sinI2 = std::max( 0.0, 1.0 - cosI * cosI );
	const double eta = etaI / etaT;
	const double sinT2 = eta * eta * sinI2;
	if( sinT2 >= 1.0 ) {
		return 1.0; // Total internal reflection
	}
	const double cosT = std::sqrt( std::max( 0.0, 1.0 - sinT2 ) );
	const double rParl = ( etaT * cosI - etaI * cosT ) / ( etaT * cosI + etaI * cosT );
	const double rPerp = ( etaI * cosI - etaT * cosT ) / ( etaI * cosI + etaT * cosT );
	return std::clamp( 0.5 * ( rParl * rParl + rPerp * rPerp ), 0.0, 1.0 );
}

double HyLIoSReciprocalBoundary::EvalD( const SlabVector& h ) const noexcept
{
	if( IsSmooth() || h.z <= 0.0 ) {
		return 0.0;
	}
	const double a2 = alpha_ * alpha_;
	const double cos2 = h.z * h.z;
	const double denom = cos2 * ( a2 - 1.0 ) + 1.0;
	if( !( denom > 0.0 ) ) {
		return 0.0;
	}
	return a2 / ( kPi * denom * denom );
}

double HyLIoSReciprocalBoundary::Lambda( const SlabVector& w ) const noexcept
{
	if( IsSmooth() ) {
		return 0.0;
	}
	const double absCos = std::fabs( w.z );
	if( !( absCos > 0.0 ) ) {
		return 1e30;
	}
	const double tan2 = std::max( 0.0, ( 1.0 - absCos * absCos ) / ( absCos * absCos ) );
	return 0.5 * ( std::sqrt( 1.0 + alpha_ * alpha_ * tan2 ) - 1.0 );
}

double HyLIoSReciprocalBoundary::EvalG1( const SlabVector& w ) const noexcept
{
	return 1.0 / ( 1.0 + Lambda( w ) );
}

double HyLIoSReciprocalBoundary::EvalG2( const SlabVector& wi, const SlabVector& wo ) const noexcept
{
	return 1.0 / ( 1.0 + Lambda( wi ) + Lambda( wo ) );
}

double HyLIoSReciprocalBoundary::EvalReflection( const SlabVector& wi, const SlabVector& wo ) const noexcept
{
	if( IsSmooth() ) {
		return 0.0;
	}
	// Both directions must lie on the same side of the macro interface.
	if( wi.z * wo.z <= 0.0 ) {
		return 0.0;
	}
	const bool exteriorSide = ( wo.z > 0.0 );
	const double etaI = exteriorSide ? etaExt_ : etaInt_;
	const double etaT = exteriorSide ? etaInt_ : etaExt_;

	SlabVector h{};
	if( !Normalize( { wi.x + wo.x, wi.y + wo.y, wi.z + wo.z }, h ) ) {
		return 0.0;
	}
	if( h.z < 0.0 ) {
		h = { -h.x, -h.y, -h.z };
	}
	const double cosI = std::fabs( wi.z );
	const double cosO = std::fabs( wo.z );
	if( cosI <= 0.0 || cosO <= 0.0 ) {
		return 0.0;
	}
	const double D = EvalD( h );
	const double G2 = EvalG2( wi, wo );
	const double F = FresnelDielectric( Dot( wo, h ), etaI, etaT );
	return ( D * G2 * F ) / ( 4.0 * cosI * cosO );
}

double HyLIoSReciprocalBoundary::PdfReflection( const SlabVector& wi, const SlabVector& wo ) const noexcept
{
	if( IsSmooth() || wi.z * wo.z <= 0.0 ) {
		return 0.0;
	}
	SlabVector h{};
	if( !Normalize( { wi.x + wo.x, wi.y + wo.y, wi.z + wo.z }, h ) ) {
		return 0.0;
	}
	if( h.z < 0.0 ) {
		h = { -h.x, -h.y, -h.z };
	}
	const double cosO = std::fabs( wo.z );
	if( cosO <= 0.0 ) {
		return 0.0;
	}
	const double D = EvalD( h );
	const double G1 = EvalG1( wo );
	// VNDF reflection PDF: D_v(wo, h) / (4 * |wo . h|) = G1(wo) * D(h) / (4 * |wo.z|)
	return ( G1 * D ) / ( 4.0 * cosO );
}

double HyLIoSReciprocalBoundary::EvalTransmission( const SlabVector& wi, const SlabVector& wo ) const noexcept
{
	if( IsSmooth() ) {
		return 0.0;
	}
	// Opposite hemispheres required for transmission.
	if( wi.z * wo.z >= 0.0 ) {
		return 0.0;
	}
	const double etaI = ( wi.z > 0.0 ) ? etaExt_ : etaInt_;
	const double etaO = ( wo.z > 0.0 ) ? etaExt_ : etaInt_;
	if( std::fabs( etaI - etaO ) < 1e-12 ) {
		return 0.0; // Index-matched interface is a pure delta pass-through
	}

	// Half-vector oriented into the +Z (exterior) hemisphere.
	SlabVector hRaw{
		-( etaI * wi.x + etaO * wo.x ),
		-( etaI * wi.y + etaO * wo.y ),
		-( etaI * wi.z + etaO * wo.z )
	};
	SlabVector h{};
	if( !Normalize( hRaw, h ) ) {
		return 0.0;
	}
	if( h.z < 0.0 ) {
		h = { -h.x, -h.y, -h.z };
	}

	const double wiDotH = Dot( wi, h );
	const double woDotH = Dot( wo, h );
	// Microfacet must be visible from both sides (opposite signs of dot product).
	if( wiDotH * woDotH >= 0.0 ) {
		return 0.0;
	}

	const double denom = etaI * wiDotH + etaO * woDotH;
	if( !( denom * denom > 0.0 ) ) {
		return 0.0;
	}

	const double D = EvalD( h );
	const double G2 = EvalG2( wi, wo );
	const double F = FresnelDielectric( std::fabs( woDotH ), etaO, etaI );
	const double cosI = std::fabs( wi.z );
	const double cosO = std::fabs( wo.z );
	if( cosI <= 0.0 || cosO <= 0.0 ) {
		return 0.0;
	}

	// Radiance-transport BSDF (Walter et al. 2007, Eq. 21 with radiance (etaO/etaI)^2 scaling
	// when integrating incoming radiance L_i(wi) over wi -> outgoing radiance L_o(wo)):
	// Note: etaI^2 * f_t(wi -> wo) == etaO^2 * f_t(wo -> wi).
	const double num = ( etaO * etaO ) * D * G2 * ( 1.0 - F ) * std::fabs( wiDotH ) * std::fabs( woDotH );
	const double den = cosI * cosO * denom * denom;
	return num / den;
}

double HyLIoSReciprocalBoundary::PdfTransmission( const SlabVector& wi, const SlabVector& wo ) const noexcept
{
	if( IsSmooth() || wi.z * wo.z >= 0.0 ) {
		return 0.0;
	}
	const double etaI = ( wi.z > 0.0 ) ? etaExt_ : etaInt_;
	const double etaO = ( wo.z > 0.0 ) ? etaExt_ : etaInt_;
	if( std::fabs( etaI - etaO ) < 1e-12 ) {
		return 0.0;
	}

	SlabVector hRaw{
		-( etaI * wi.x + etaO * wo.x ),
		-( etaI * wi.y + etaO * wo.y ),
		-( etaI * wi.z + etaO * wo.z )
	};
	SlabVector h{};
	if( !Normalize( hRaw, h ) ) {
		return 0.0;
	}
	if( h.z < 0.0 ) {
		h = { -h.x, -h.y, -h.z };
	}

	const double wiDotH = Dot( wi, h );
	const double woDotH = Dot( wo, h );
	if( wiDotH * woDotH >= 0.0 ) {
		return 0.0;
	}
	const double denom = etaI * wiDotH + etaO * woDotH;
	if( !( denom * denom > 0.0 ) ) {
		return 0.0;
	}
	const double cosO = std::fabs( wo.z );
	if( cosO <= 0.0 ) {
		return 0.0;
	}
	// VNDF density of h given wo: D_v(wo, h) = G1(wo) * |wo.h| * D(h) / |wo.z|
	// Jacobian |dh / dwi| = etaI^2 * |wi.h| / (etaI*(wi.h) + etaO*(wo.h))^2
	const double dv = EvalG1( wo ) * std::fabs( woDotH ) * EvalD( h ) / cosO;
	const double jacobian = ( etaI * etaI * std::fabs( wiDotH ) ) / ( denom * denom );
	return dv * jacobian;
}

SlabVector HyLIoSReciprocalBoundary::SampleVNDF(
	const SlabVector& woUpper,
	const double u1,
	const double u2
	) const noexcept
{
	// Heitz (2018) "Sampling the GGX Distribution of Visible Normals"
	SlabVector Vh{};
	Normalize( { alpha_ * woUpper.x, alpha_ * woUpper.y, woUpper.z }, Vh );

	const double lensq = Vh.x * Vh.x + Vh.y * Vh.y;
	const SlabVector T1 = ( lensq > 1e-24 )
		? SlabVector{ -Vh.y / std::sqrt( lensq ), Vh.x / std::sqrt( lensq ), 0.0 }
		: SlabVector{ 1.0, 0.0, 0.0 };
	const SlabVector T2{
		Vh.y * T1.z - Vh.z * T1.y,
		Vh.z * T1.x - Vh.x * T1.z,
		Vh.x * T1.y - Vh.y * T1.x
	};

	const double r = std::sqrt( ClampUnit( u1 ) );
	const double phi = 2.0 * kPi * ClampUnit( u2 );
	const double t1 = r * std::cos( phi );
	double t2 = r * std::sin( phi );
	const double s = 0.5 * ( 1.0 + Vh.z );
	t2 = ( 1.0 - s ) * std::sqrt( std::max( 0.0, 1.0 - t1 * t1 ) ) + s * t2;

	const double nhZ = std::sqrt( std::max( 0.0, 1.0 - t1 * t1 - t2 * t2 ) );
	const SlabVector Nh{
		t1 * T1.x + t2 * T2.x + nhZ * Vh.x,
		t1 * T1.y + t2 * T2.y + nhZ * Vh.y,
		t1 * T1.z + t2 * T2.z + nhZ * Vh.z
	};

	SlabVector h{};
	if( !Normalize( { alpha_ * Nh.x, alpha_ * Nh.y, std::max( 0.0, Nh.z ) }, h ) ) {
		return { 0.0, 0.0, 1.0 };
	}
	return h;
}

ReciprocalBoundarySample HyLIoSReciprocalBoundary::SampleReflection(
	const SlabVector& wo,
	const double u1,
	const double u2
	) const noexcept
{
	ReciprocalBoundarySample out;
	if( wo.z == 0.0 ) {
		return out;
	}
	const bool exteriorSide = ( wo.z > 0.0 );
	const double etaI = exteriorSide ? etaExt_ : etaInt_;
	const double etaT = exteriorSide ? etaInt_ : etaExt_;

	if( IsSmooth() ) {
		out.valid = true;
		out.isReflection = true;
		out.isDelta = true;
		out.direction = { -wo.x, -wo.y, wo.z };
		out.pdf = 0.0;
		out.weight = FresnelDielectric( std::fabs( wo.z ), etaI, etaT );
		return out;
	}

	const SlabVector woUpper = exteriorSide ? wo : SlabVector{ -wo.x, -wo.y, -wo.z };
	SlabVector h = SampleVNDF( woUpper, u1, u2 );
	if( !exteriorSide ) {
		h = { -h.x, -h.y, -h.z };
	}
	const double woDotH = Dot( wo, h );
	if( woDotH <= 0.0 ) {
		return out;
	}
	const SlabVector wi{
		2.0 * woDotH * h.x - wo.x,
		2.0 * woDotH * h.y - wo.y,
		2.0 * woDotH * h.z - wo.z
	};
	if( wi.z * wo.z <= 0.0 ) {
		return out;
	}

	const double pdf = PdfReflection( wi, wo );
	if( !( pdf > 0.0 ) || !std::isfinite( pdf ) ) {
		return out;
	}
	const double f = EvalReflection( wi, wo );
	out.valid = true;
	out.isReflection = true;
	out.isDelta = false;
	out.direction = wi;
	out.pdf = pdf;
	out.weight = f * std::fabs( wi.z ) / pdf;
	return out;
}

ReciprocalBoundarySample HyLIoSReciprocalBoundary::SampleTransmission(
	const SlabVector& wo,
	const double u1,
	const double u2
	) const noexcept
{
	ReciprocalBoundarySample out;
	if( wo.z == 0.0 ) {
		return out;
	}
	const bool woExterior = ( wo.z > 0.0 );
	const double etaO = woExterior ? etaExt_ : etaInt_;
	const double etaI = woExterior ? etaInt_ : etaExt_;
	const double etaRatio = etaO / etaI;

	if( IsSmooth() ) {
		const double cosO = std::fabs( wo.z );
		const double sinO2 = std::max( 0.0, 1.0 - cosO * cosO );
		const double sinI2 = etaRatio * etaRatio * sinO2;
		if( sinI2 >= 1.0 ) {
			return out; // TIR
		}
		const double cosI = std::sqrt( std::max( 0.0, 1.0 - sinI2 ) );
		const double F = FresnelDielectric( cosO, etaO, etaI );
		out.valid = true;
		out.isReflection = false;
		out.isDelta = true;
		out.direction = {
			-etaRatio * wo.x,
			-etaRatio * wo.y,
			woExterior ? -cosI : cosI
		};
		out.pdf = 0.0;
		out.weight = ( 1.0 - F ) * ( etaRatio * etaRatio );
		return out;
	}

	const SlabVector woUpper = woExterior ? wo : SlabVector{ -wo.x, -wo.y, -wo.z };
	SlabVector h = SampleVNDF( woUpper, u1, u2 );
	if( !woExterior ) {
		h = { -h.x, -h.y, -h.z };
	}
	const double woDotH = Dot( wo, h );
	if( woDotH <= 0.0 ) {
		return out;
	}

	const double sinO2 = std::max( 0.0, 1.0 - woDotH * woDotH );
	const double sinI2 = etaRatio * etaRatio * sinO2;
	if( sinI2 >= 1.0 ) {
		return out; // TIR against microfacet normal
	}
	const double cosT = std::sqrt( std::max( 0.0, 1.0 - sinI2 ) );
	SlabVector wi{};
	if( !Normalize( {
			( etaRatio * woDotH - cosT ) * h.x - etaRatio * wo.x,
			( etaRatio * woDotH - cosT ) * h.y - etaRatio * wo.y,
			( etaRatio * woDotH - cosT ) * h.z - etaRatio * wo.z
		}, wi ) )
	{
		return out;
	}
	if( wi.z * wo.z >= 0.0 ) {
		return out;
	}

	const double pdf = PdfTransmission( wi, wo );
	if( !( pdf > 0.0 ) || !std::isfinite( pdf ) ) {
		return out;
	}
	const double f = EvalTransmission( wi, wo );
	out.valid = true;
	out.isReflection = false;
	out.isDelta = false;
	out.direction = wi;
	out.pdf = pdf;
	out.weight = f * std::fabs( wi.z ) / pdf;
	return out;
}

ReciprocalBoundarySample HyLIoSReciprocalBoundary::SampleFull(
	const SlabVector& wo,
	const double uBranch,
	const double u1,
	const double u2
	) const noexcept
{
	ReciprocalBoundarySample out;
	if( wo.z == 0.0 ) {
		return out;
	}
	const bool woExterior = ( wo.z > 0.0 );
	const double etaO = woExterior ? etaExt_ : etaInt_;
	const double etaI = woExterior ? etaInt_ : etaExt_;
	const double etaRatio = etaO / etaI;

	SlabVector h{ 0.0, 0.0, woExterior ? 1.0 : -1.0 };
	if( !IsSmooth() ) {
		const SlabVector woUpper = woExterior ? wo : SlabVector{ -wo.x, -wo.y, -wo.z };
		h = SampleVNDF( woUpper, u1, u2 );
		if( !woExterior ) {
			h = { -h.x, -h.y, -h.z };
		}
	}

	const double woDotH = Dot( wo, h );
	if( woDotH <= 0.0 ) {
		return out;
	}

	const double F = FresnelDielectric( woDotH, etaO, etaI );
	const double ub = ClampUnit( uBranch );

	if( ub < F ) {
		// Reflection branch (selected with probability F)
		const SlabVector wi{
			2.0 * woDotH * h.x - wo.x,
			2.0 * woDotH * h.y - wo.y,
			2.0 * woDotH * h.z - wo.z
		};
		if( wi.z * wo.z <= 0.0 ) {
			return out;
		}
		out.valid = true;
		out.isReflection = true;
		out.isDelta = IsSmooth();
		out.direction = wi;
		if( IsSmooth() ) {
			out.pdf = 0.0;
			out.weight = 1.0; // F / F == 1
		} else {
			const double pdfR = PdfReflection( wi, wo );
			out.pdf = F * pdfR;
			out.weight = ( pdfR > 0.0 ) ? ( EvalG2( wi, wo ) / EvalG1( wo ) ) : 0.0;
		}
		return out;
	}

	// Transmission branch (selected with probability 1 - F)
	const double sinO2 = std::max( 0.0, 1.0 - woDotH * woDotH );
	const double sinI2 = etaRatio * etaRatio * sinO2;
	if( sinI2 >= 1.0 ) {
		return out;
	}
	const double cosT = std::sqrt( std::max( 0.0, 1.0 - sinI2 ) );
	SlabVector wi{};
	if( !Normalize( {
			( etaRatio * woDotH - cosT ) * h.x - etaRatio * wo.x,
			( etaRatio * woDotH - cosT ) * h.y - etaRatio * wo.y,
			( etaRatio * woDotH - cosT ) * h.z - etaRatio * wo.z
		}, wi ) )
	{
		return out;
	}
	if( wi.z * wo.z >= 0.0 ) {
		return out;
	}

	out.valid = true;
	out.isReflection = false;
	out.isDelta = IsSmooth();
	out.direction = wi;
	if( IsSmooth() ) {
		out.pdf = 0.0;
		out.weight = etaRatio * etaRatio;
	} else {
		const double pdfT = PdfTransmission( wi, wo );
		out.pdf = ( 1.0 - F ) * pdfT;
		out.weight = ( pdfT > 0.0 ) ? ( ( etaRatio * etaRatio ) * EvalG2( wi, wo ) / EvalG1( wo ) ) : 0.0;
	}
	return out;
}

} // namespace RISE::HyLIoS
