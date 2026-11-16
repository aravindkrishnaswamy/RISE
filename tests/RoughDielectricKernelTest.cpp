// Tier 1.1: exact limits, adjoint reciprocity, independent Walter evaluation,
// a numerical refraction Jacobian, and sampled-vs-integrated probability law.
// The kernel is single scattering; a furnace may lose energy to null events.
#include "../src/Library/Utilities/MicrofacetUtils.h"

#include <array>
#include <cmath>
#include <iostream>
#include <random>

using namespace RISE;
namespace RD = MicrofacetUtils::RoughDielectric;

static int passed = 0, failed = 0;
static void Check( bool ok, const char* label )
{
	if( ok ) ++passed;
	else { ++failed; if( failed <= 12 ) std::cout << "FAIL: " << label << '\n'; }
}

static bool Close( double a, double b, double relative = 1e-10 )
{
	return std::isfinite( a ) && std::isfinite( b ) &&
		std::abs( a - b ) <= relative * std::max( 1.0, std::max( std::abs( a ), std::abs( b ) ) );
}

static Vector3 Unit( double x, double y, double z )
{
	return Vector3Ops::Normalize( Vector3( x, y, z ) );
}

// Independent direct Walter equations. Deliberately uses Lambda and the
// unreduced Jacobian instead of the kernel's projected-length formulation.
static RD::Evaluation Reference( const Vector3& wi, const Vector3& wo,
	double ax, double ay, double eta, bool radiance )
{
	RD::Evaluation result;
	const bool reflection = wo.z > 0;
	Vector3 m = Vector3Ops::Normalize( wi + wo * ( reflection ? 1.0 : eta ) );
	if( m.z < 0 ) m = -m;
	const double ci = Vector3Ops::Dot( wi, m ), co = Vector3Ops::Dot( wo, m );
	if( ci <= 0 || ( reflection ? co <= 0 : co >= 0 ) ) return result;
	const double sinT = std::sqrt( std::max( 0.0, 1 - ci * ci ) ) / eta;
	double F = 1;
	if( sinT < 1 ) {
		const double ct = std::sqrt( 1 - sinT * sinT );
		const double rs = ( ci - eta * ct ) / ( ci + eta * ct );
		const double rp = ( eta * ci - ct ) / ( eta * ci + ct );
		F = ( rs * rs + rp * rp ) / 2;
	}
	const double q = m.x * m.x / ( ax * ax ) + m.y * m.y / ( ay * ay ) + m.z * m.z;
	const double D = 1 / ( PI * ax * ay * q * q );
	const auto lambda = [ax,ay]( const Vector3& w ) {
		return ( std::sqrt( 1 + ( ax*ax*w.x*w.x + ay*ay*w.y*w.y ) / ( w.z*w.z ) ) - 1 ) / 2;
	};
	const double G1 = 1 / ( 1 + lambda( wi ) );
	const double G2 = 1 / ( 1 + lambda( wi ) + lambda( wo ) );
	const double pm = D * G1 * ci / wi.z;
	if( reflection ) {
		result.f = F * D * G2 / ( 4 * wi.z * wo.z );
		result.pdf = F * pm / ( 4 * ci );
	} else {
		const double denom = co + ci / eta;
		result.f = ( 1 - F ) * D * G2 * std::abs( ci * co / ( wi.z * wo.z * denom * denom ) );
		result.pdf = ( 1 - F ) * pm * std::abs( co ) / ( denom * denom );
		if( radiance ) result.f /= eta * eta;
	}
	return result;
}

static void ExactLimits()
{
	const Vector3 wi( 0, 0, 1 );
	const double D = 1 / ( PI * 0.2 * 0.4 );
	const auto r = RD::Evaluate( wi, wi, 0.2, 0.4, 1.5, false );
	Check( Close( r.f, 0.04 * D / 4 ) && Close( r.pdf, r.f ), "normal reflection: Fresnel, NDF, G2, and Jacobian" );
	const auto t = RD::Evaluate( wi, -wi, 0.2, 0.4, 1.5, false );
	const auto tr = RD::Evaluate( wi, -wi, 0.2, 0.4, 1.5, true );
	Check( Close( t.f, 0.96 * D * 9 ) && Close( t.pdf, t.f ), "normal transmission: squared refractive Jacobian" );
	Check( Close( tr.f, t.f / 2.25 ) && tr.pdf == t.pdf, "radiance scaling changes the value, not the PDF" );
	for( const auto& widths : { std::array<double,2>{0,0}, {0.3,0.7} } ) {
		const Vector3 incident = Unit( 0.7, 0.2, 1 );
		const auto equal = RD::Draw( incident, widths[0], widths[1], 1, 0.4, 0.6, 0, true );
		Check( equal.valid && equal.delta && equal.transmission && equal.pdf == 1 && equal.weight == 1 &&
			Vector3Ops::SquaredModulus( equal.wo + incident ) == 0, "equal indices collapse to exact straight-through delta" );
		Check( RD::Evaluate( incident, -incident, widths[0], widths[1], 1, false ).pdf == 0,
			"delta has no solid-angle density" );
	}
	const auto sr = RD::Draw( wi, 0, 0, 1.5, 0.2, 0.3, 0, true );
	const auto st = RD::Draw( wi, 0, 0, 1.5, 0.2, 0.3, 0.5, true );
	Check( sr.valid && sr.delta && !sr.transmission && Close( sr.pdf, 0.04 ) && sr.weight == 1,
		"smooth reflection has Fresnel probability mass and unit weight" );
	Check( st.valid && st.delta && st.transmission && Close( st.pdf, 0.96 ) && Close( st.weight, 1/2.25 ),
		"smooth transmission has radiance weight, without a second Fresnel factor" );
	const auto tir = RD::Draw( Unit( std::sqrt(3.0), 0, 1 ), 0, 0, 2.0/3, 0.2, 0.3, 0.99, true );
	Check( tir.valid && !tir.transmission && tir.pdf == 1 && tir.weight == 1, "smooth inside incidence above the critical angle is TIR" );
	Check( !RD::Draw( wi, 0, 0.2, 1.5, 0.1, 0.2, 0.3, false ).valid, "one zero roughness axis is explicitly unsupported" );
	Check( !RD::Draw( wi, 0.2, 0.2, 0, 0.1, 0.2, 0.3, false ).valid &&
		!RD::Draw( wi, 0.2, 0.2, 1.5, 0.1, 1, 0.3, false ).valid, "invalid index and closed-right random input are rejected" );
	for( double eta : { 1e-200, 1e200 } ) {
		const auto extreme = RD::Draw( wi, 0, 0, eta, 0.2, 0.3, 0.5, false );
		Check( extreme.valid && !extreme.transmission && extreme.pdf == 1 && extreme.weight == 1,
			"extreme finite index: normal-incidence Fresnel limit is unit reflection, without squaring eta" );
		Check( RD::Evaluate(wi,-wi,0.2,0.4,eta,false).pdf == 0,
			"unit-Fresnel transmission evaluates to zero before computing its Jacobian" );
	}
	Check( !RD::ValidParameters(1e155,1e155,1.5) &&
		!RD::Draw(Unit(0.3,0.2,1),1e155,1e155,1.5,0.2,0.4,0.5,false).valid,
		"widths above one are explicitly outside this kernel's supported roughness domain" );
}

static void AdjacentIndicesAndEndpoints()
{
	for( double eta : { std::nextafter(1.0,0.0), std::nextafter(1.0,2.0), 1-1e-12, 1+1e-12 } ) {
		const double delta=eta-1;
		const double F=(delta/(eta+1))*(delta/(eta+1));
		const double expected=(1-F)/(PI*0.2*0.4)*(eta/delta)*(eta/delta);
		const auto directional=RD::Evaluate(Vector3(0,0,1),Vector3(0,0,-1),0.2,0.4,eta,false);
		const auto sampled=RD::Draw(Vector3(0,0,1),0.2,0.4,eta,0.2,0,0.5,false);
		Check( Close(directional.pdf,expected,1e-12) && Close(directional.f,expected,1e-12),
			"adjacent-index normal-incidence directional value and PDF match analytic Jacobian, separately" );
		Check( sampled.valid && Close(sampled.pdf,expected,1e-12) && Close(sampled.f,expected,1e-12) &&
			sampled.wo.z == -1, "adjacent-index normal-incidence draw preserves exact contrast and direction" );
	}
	const Vector3 wi = Unit(0.3,0.2,1);
	const double ax=0.3, ay=0.6;
	const double lambda=(std::sqrt(1+(ax*ax*wi.x*wi.x+ay*ay*wi.y*wi.y)/(wi.z*wi.z))-1)/2;
	const double expectedWeight=(1+lambda)/(1+2*lambda);
	for( double eta : { std::nextafter(1.0,0.0), std::nextafter(1.0,2.0) } ) {
		// This exact draw formerly lost m.z when m was reconstructed from wo.
		const auto regression=RD::Draw(wi,ax,ay,eta,0.5066881612090306,0.99892030650086816,0.5,false);
		Check(regression.valid && regression.transmission && std::isfinite(regression.pdf),
			"adjacent-index regression retains the sampled normal instead of creating a null event");
		std::mt19937_64 rng(217507);
		const auto uniform=[&rng](){return std::generate_canonical<double,53>(rng);};
		double totalWeight=0;
		for(int n=0;n<10000;++n) {
			const double u=uniform(),v=uniform();
			const auto s=RD::Draw(wi,ax,ay,eta,u,v,0.5,false);
			Check(s.valid && s.transmission && !s.delta && std::isfinite(s.pdf) &&
				std::isfinite(s.f) && Close(s.weight,expectedWeight,1e-12),
				"adjacent-index VNDF transmission has no artificial rejection or energy loss");
			totalWeight+=s.valid?s.weight:0;
		}
		Check(Close(totalWeight/10000,expectedWeight,1e-12),
			"adjacent-index mean equals the analytic single-scatter G2/G1 limit");
	}
	const double upper=std::nextafter(1.0,0.0);
	for(double u : {0.0,upper}) {
		const auto lowerRadial=RD::Draw(Vector3(0,0,1),0.1,0.1,1.5,u,0,0.5,false);
		Check(lowerRadial.valid && lowerRadial.transmission,
			"normal incidence accepts lower radial endpoint at both azimuth endpoints");
		const auto upperRadial=RD::Draw(Unit(1,0,1e-10),0.1,0.1,1.5,u,upper,0.5,false);
		Check(upperRadial.valid && upperRadial.transmission,
			"grazing incidence accepts known valid upper radial and azimuth endpoint transmissions");
	}
}

static void EvaluationAndAdjoint()
{
	std::mt19937_64 rng( 217507 );
	const auto uniform = [&rng]() { return std::generate_canonical<double,53>( rng ); };
	for( const double eta : { 1.5, 2.0/3, 1.01, 0.99 } ) {
		int accepted = 0;
		for( int n = 0; n < 3000; ++n ) {
			const Vector3 wi = Unit( 2 * uniform() - 1, 2 * uniform() - 1, 0.02 + uniform() );
			const double u = uniform(), v = uniform(), lobe = uniform();
			const auto s = RD::Draw( wi, 0.25, 0.7, eta, u, v, lobe, false );
			if( !s.valid ) continue;
			++accepted;
			const auto ref = Reference( wi, s.wo, 0.25, 0.7, eta, false );
			const auto directional = RD::Evaluate( wi, s.wo, 0.25, 0.7, eta, false );
			Check( Close( s.f, ref.f, 2e-8 ) && Close( s.pdf, ref.pdf, 2e-8 ), "evaluation agrees with independent unreduced Walter equations" );
			Check( Close( directional.f, ref.f, 2e-8 ) && Close( directional.pdf, ref.pdf, 2e-8 ),
				"direction-only evaluation agrees with independent Walter value and PDF" );
			Check( Close( s.f * std::abs(s.wo.z) / s.pdf, s.weight, 2e-8 ), "sample throughput agrees with f*cos/pdf" );
			Check( s.weight >= 0 && s.weight <= 1 + 1e-12, "single-scatter importance weight cannot create energy" );
			if( s.transmission ) {
				// Rotate both directions by pi about x to face the other medium.
				const Vector3 backWi( s.wo.x, -s.wo.y, -s.wo.z ), backWo( wi.x, -wi.y, -wi.z );
				const auto forward = RD::Evaluate( wi, s.wo, 0.25, 0.7, eta, true );
				const auto backward = RD::Evaluate( backWi, backWo, 0.25, 0.7, 1/eta, false );
				Check( Close( forward.f, backward.f, 2e-8 ), "radiance/importance adjoint reciprocity across the interface" );
			} else {
				const auto backward = RD::Evaluate( s.wo, wi, 0.25, 0.7, eta, false );
				Check( Close( s.f, backward.f, 2e-8 ), "reflection reciprocity" );
			}
		}
		Check( accepted > 0, "every tested index ratio actually exercises non-null rough scattering" );
	}
	for( const double eta : { 1-1e-7, 1+1e-7 } ) {
		const auto s = RD::Draw( Unit(0.3,0.2,1), 0.3, 0.6, eta, 0.2, 0.4, 0.5, true );
		Check( s.valid && s.transmission && std::isfinite(s.f) && std::isfinite(s.pdf) &&
			Close( s.f * std::abs(s.wo.z) / s.pdf, s.weight, 1e-6 ), "near-unity index: rationalized denominator remains finite and consistent" );
	}
}

static void RefractionJacobian()
{
	const Vector3 wi = Unit( 0.3, 0.2, 1 );
	const double theta = 2.7, phi = 0.7, h = 1e-5;
	for( const double eta : { 1.5, 2.0/3 } ) {
		const auto direction = []( double t, double p ) { return Vector3( std::sin(t)*std::cos(p), std::sin(t)*std::sin(p), std::cos(t) ); };
		const auto half = [&]( double t, double p ) {
			Vector3 m = Vector3Ops::Normalize( wi + direction(t,p) * eta );
			return m.z < 0 ? -m : m;
		};
		const Vector3 dt = ( half(theta+h,phi) - half(theta-h,phi) ) * (1/(2*h));
		const Vector3 dp = ( half(theta,phi+h) - half(theta,phi-h) ) * (1/(2*h));
		const double numerical = std::sqrt( Vector3Ops::SquaredModulus( Vector3Ops::Cross(dt,dp) ) ) / std::sin(theta);
		const Vector3 wo = direction(theta,phi), m = half(theta,phi);
		const double denom = Vector3Ops::Dot(wo,m) + Vector3Ops::Dot(wi,m)/eta;
		const double analytic = std::abs(Vector3Ops::Dot(wo,m)) / (denom*denom);
		Check( Close( numerical, analytic, 1e-8 ), "transmission Jacobian agrees with an independent spherical finite difference" );
	}
}

static void BoundaryCases()
{
	int accepted = 0;
	for( double ax : { 1e-6, 0.001, 0.1, 1.0 } )
	for( double ay : { 1e-6, 0.001, 0.1, 1.0 } )
	for( double eta : { 2.0/3, 1.0, 1+1e-12, 1.5 } )
	for( double z : { 1e-10, 0.01, 0.5, 1.0 } )
	for( double u : { 0.0, 0.5, std::nextafter(1.0,0.0) } )
	for( double v : { 0.0, 0.5, std::nextafter(1.0,0.0) } )
	for( double lobe : { 0.0, 0.5, std::nextafter(1.0,0.0) } ) {
		const auto s = RD::Draw( Unit(std::sqrt(1-z*z),0,z), ax, ay, eta, u, v, lobe, true );
		if( !s.valid ) continue;
		++accepted;
		Check( std::isfinite(s.f) && std::isfinite(s.pdf) && std::isfinite(s.weight) &&
			s.weight >= 0 && Close(Vector3Ops::SquaredModulus(s.wo),1,1e-10),
			"grazing, anisotropic, low-contrast-index, and random endpoint samples stay finite and unit length" );
	}
	Check( accepted > 0, "boundary sweep actually exercises accepted samples" );
}

static void ExtremeScaleArithmetic()
{
	// Here the NDF itself and the final f/PDF are representable, while
	// the unreduced product of two inverse widths overflows.
	const auto offPeak=RD::Evaluate(Vector3(0,0,1),Vector3(2e-200,0,1),1e-300,1e-300,1.5,false);
	const double expectedOffPeak=1e198*INV_PI;
	Check(Close(offPeak.f,expectedOffPeak,1e-12) && Close(offPeak.pdf,expectedOffPeak,1e-12),
		"tiny-width off-peak evaluation keeps cancelling NDF factors scaled until the final value");
	const double alpha=1e-160, radial=1-1e-8;
	const double slopeScale=(1-radial)/alpha;
	const double expectedDraw=0.01*INV_PI*slopeScale*slopeScale;
	const auto narrow=RD::Draw(Vector3(0,0,1),alpha,alpha,1.5,0.5,radial,0,false);
	Check(narrow.valid && Close(narrow.f,expectedDraw,1e-12) && Close(narrow.pdf,expectedDraw,1e-12),
		"tiny-width draw has independently predicted finite value and density");

	// Normalize the stretched incident vector analytically before applying
	// the normal-incidence GGX stretch. All reference algebra is order one;
	// only the final direction's z component is multiplied by the tiny width.
	const double vh=1/std::sqrt(2.0), capZ=1-0.4*(1+vh);
	const double capR=std::sqrt((1-capZ)*(1+capZ));
	const double cx=vh+capR*std::cos(TWO_PI*0.2), cz=(1+vh)*0.6;
	const double ratio=1+2*cx/cz;
	const double expectedWeight=ratio*(1+std::sqrt(2.0))/(ratio*std::sqrt(2.0)+std::sqrt(1+ratio*ratio));
	for(double width : {1e-160,1e-170,1e-200,1e-300}) {
		const auto grazing=RD::Draw(Vector3(1,0,width),width,width,1.5,0.2,0.4,0.5,false);
		Check(grazing.valid && !grazing.transmission && std::isfinite(grazing.weight) &&
			Close(grazing.wo.z/width,ratio,1e-12) && Close(grazing.weight,expectedWeight,1e-12),
			"subnormal-squared stretches preserve the sampled direction and finite analytic G2/G1 throughput");
	}
}

static void ProbabilityAndFurnace()
{
	constexpr int nMu = 8, nPhi = 12, subdivisions = 24, samples = 240000;
	constexpr int bins = 2*nMu*nPhi;
	const Vector3 wi = Unit(0.3,0.1,1);
	for( const double eta : { 1.5, 2.0/3 } ) {
		std::array<double,bins> expected{};
		std::array<int,bins> observed{};
		double integralPdf = 0, integralEnergy = 0;
		for( int side=0; side<2; ++side ) for( int m=0; m<nMu*subdivisions; ++m ) {
			const double mu = (m+0.5)/(nMu*subdivisions);
			const double r = std::sqrt(1-mu*mu);
			for( int p=0; p<nPhi*subdivisions; ++p ) {
				const double phi = TWO_PI*(p+0.5)/(nPhi*subdivisions);
				const Vector3 wo(r*std::cos(phi),r*std::sin(phi),side ? -mu : mu);
				const auto ref = Reference(wi,wo,0.9,0.6,eta,false);
				const double domega = TWO_PI/(nMu*nPhi*subdivisions*subdivisions);
				expected[(side*nMu+m/subdivisions)*nPhi+p/subdivisions] += ref.pdf*domega;
				integralPdf += ref.pdf*domega;
				integralEnergy += ref.f*mu*domega;
			}
		}
		std::mt19937_64 rng(73017);
		const auto uniform = [&rng]() { return std::generate_canonical<double,53>(rng); };
		int accepted=0;
		double energy=0;
		for( int n=0; n<samples; ++n ) {
			const double u=uniform(), v=uniform(), lobe=uniform();
			const auto s=RD::Draw(wi,0.9,0.6,eta,u,v,lobe,false);
			if( !s.valid ) continue;
			++accepted; energy += s.weight;
			double phi=std::atan2(s.wo.y,s.wo.x); if(phi<0) phi+=TWO_PI;
			const int m=std::min(nMu-1,int(std::abs(s.wo.z)*nMu));
			const int p=std::min(nPhi-1,int(phi/TWO_PI*nPhi));
			++observed[((s.wo.z<0 ? 1:0)*nMu+m)*nPhi+p];
		}
		double l1=0;
		for(int b=0;b<bins;++b) l1+=std::abs(double(observed[b])/samples-expected[b]);
		std::cout << "eta=" << eta << " PDF integral=" << integralPdf << " accepted=" << double(accepted)/samples
			<< " histogram L1=" << l1 << " energy integral=" << integralEnergy << " sampled=" << energy/samples << '\n';
		Check( l1<0.025, "sampled directions match independent per-bin PDF integrals" );
		Check( std::abs(integralPdf-double(accepted)/samples)<0.01, "PDF integrates to accepted probability including null events" );
		Check( std::abs(integralEnergy-energy/samples)<0.01, "sampled furnace energy agrees with independent BSDF quadrature" );
		Check( integralEnergy>0 && integralEnergy<=1.001 && energy/samples<=1, "single-scatter furnace conserves importance energy" );
	}
}

int main()
{
	ExactLimits();
	AdjacentIndicesAndEndpoints();
	EvaluationAndAdjoint();
	RefractionJacobian();
	BoundaryCases();
	ExtremeScaleArithmetic();
	ProbabilityAndFurnace();
	std::cout << passed << " passed, " << failed << " failed\n";
	return failed ? 1 : 0;
}
