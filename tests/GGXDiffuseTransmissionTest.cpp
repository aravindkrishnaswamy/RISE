//////////////////////////////////////////////////////////////////////
//
//  GGXDiffuseTransmissionTest.cpp - DL-37 regression coverage.
//
//  Independently integrates the production GGXBRDF over the outgoing
//  hemisphere.  The estimator samples a 50/50 mixture of cosine and
//  anisotropic-VNDF proposals, evaluates the COMPLETE mixture density,
//  and divides by the number of attempted draws.  It deliberately does
//  not reuse GGXSPF or the material's lobe weights, so a shared sampler /
//  evaluator error cannot turn this into a tautology.
//
//  Coverage:
//    * Schlick F0 {0, .04, .5, 1}, four isotropic roughnesses, three
//      incident angles, plus the (.05, .5) anisotropic pair at two
//      azimuths.
//    * Specular-only F0=1 controls, which distinguish pre-existing rough
//      specular energy from diffuse-composition regressions.
//    * Mixed and specular-only conductor / thin-film controls, RGB and
//      at 450/550/650 nm.  Thin-film RGB and NM are intentionally not
//      asserted equal: the RGB path is an albedo-basis spectral preview.
//    * Deterministic RGB/NM reciprocity pairs, including the ray-facing
//      back-face frame.
//
//  The energy gate is mean <= 1 + 6 standard errors + .005.
//  The floor is fixed before measurement; it is not tuned to a result.
//  This is an energy-bound regression, not a proof that the rough
//  interface approximation is an exact energy-conservation theorem.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <cmath>
#include <algorithm>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/MicrofacetUtils.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Materials/GGXBRDF.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	static int checks = 0;
	static int failures = 0;
	static const int kRGBSamples = 30000;
	static const int kNMSamples = 12000;
	static const double kEnergySigma = 6.0;
	static const double kEnergyFloor = 0.005;

	struct Case
	{
		const char* label;
		FresnelMode mode;
		double diffuse;
		double specular;
		double alphaX;
		double alphaY;
		double thetaDeg;
		double azimuthDeg;
		double filmThickness;
	};

	struct ChannelMoments
	{
		double sum[3];
		double sumSq[3];
		unsigned int invalid;
		unsigned int belowHorizon;

		ChannelMoments() : invalid( 0 ), belowHorizon( 0 )
		{
			for( int i = 0; i < 3; ++i ) {
				sum[i] = 0;
				sumSq[i] = 0;
			}
		}
	};

	static bool IsFiniteNonNegative( const double x )
	{
		return std::isfinite( x ) && x >= 0.0;
	}

	static RayIntersectionGeometric MakeIntersectionForView( const Vector3& view )
	{
		// `ray.Dir()` points toward the surface, whereas `view` points away
		// from it.  Passing an outward lower-hemisphere view therefore makes
		// GGXBRDF exercise its ray-facing back-face frame flip.
		const Ray ray( Point3( 0, 0, 1 ), -view );
		const RasterizerState state = { 0, 0 };
		RayIntersectionGeometric ri( ray, state );
		ri.bHit = true;
		ri.range = 1;
		ri.ptIntersection = Point3( 0, 0, 0 );
		ri.vNormal = Vector3( 0, 0, 1 );
		ri.vGeomNormal = Vector3( 0, 0, 1 );
		ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
		ri.ptCoord = Point2( 0.5, 0.5 );
		ri.ambientIOR = 1.0;
		return ri;
	}

	static Vector3 Direction( const double thetaDeg, const double azimuthDeg )
	{
		const double theta = thetaDeg * PI / 180.0;
		const double phi = azimuthDeg * PI / 180.0;
		return Vector3(
			std::sin( theta ) * std::cos( phi ),
			std::sin( theta ) * std::sin( phi ),
			std::cos( theta ) );
	}

	static bool IsFiniteDirection( const Vector3& v )
	{
		const double length2 = Vector3Ops::Dot( v, v );
		return std::isfinite( v.x ) && std::isfinite( v.y ) && std::isfinite( v.z ) &&
			std::isfinite( length2 ) && length2 > 0;
	}

	class BrdfFixture
	{
	public:
		UniformColorPainter* diffuse;
		UniformColorPainter* specular;
		UniformScalarPainter* alphaX;
		UniformScalarPainter* alphaY;
		UniformScalarPainter* ior;
		UniformScalarPainter* extinction;
		UniformScalarPainter* filmIOR;
		UniformScalarPainter* filmExtinction;
		UniformScalarPainter* filmThickness;
		GGXBRDF* brdf;

		explicit BrdfFixture( const Case& c ) :
			diffuse( new UniformColorPainter( RISEPel( c.diffuse, c.diffuse, c.diffuse ) ) ),
			specular( new UniformColorPainter( RISEPel( c.specular, c.specular, c.specular ) ) ),
			alphaX( new UniformScalarPainter( c.alphaX ) ),
			alphaY( new UniformScalarPainter( c.alphaY ) ),
			ior( new UniformScalarPainter( 2.5 ) ),
			extinction( new UniformScalarPainter( 3.0 ) ),
			filmIOR( 0 ),
			filmExtinction( 0 ),
			filmThickness( 0 ),
			brdf( 0 )
		{
			if( c.mode == eFresnelThinFilmConductor ) {
				filmIOR = new UniformScalarPainter( 1.5 );
				filmExtinction = new UniformScalarPainter( 0.0 );
				filmThickness = new UniformScalarPainter( c.filmThickness );
			}
			brdf = new GGXBRDF( *diffuse, *specular, *alphaX, *alphaY, *ior, *extinction,
				c.mode, 0, filmIOR, filmExtinction, filmThickness );
		}

		~BrdfFixture()
		{
			brdf->release();
			if( filmThickness ) filmThickness->release();
			if( filmExtinction ) filmExtinction->release();
			if( filmIOR ) filmIOR->release();
			extinction->release();
			ior->release();
			alphaY->release();
			alphaX->release();
			specular->release();
			diffuse->release();
		}
	};

	static Vector3 SampleCosineHemisphere( const double u1, const double u2 )
	{
		const double phi = TWO_PI * u1;
		const double z = std::sqrt( u2 );
		const double r = std::sqrt( std::max( 0.0, 1.0 - z * z ) );
		return Vector3( r * std::cos( phi ), r * std::sin( phi ), z );
	}

	static double MixtureDensity(
		const Vector3& view,
		const Vector3& light,
		const OrthonormalBasis3D& onb,
		const double alphaX,
		const double alphaY )
	{
		const double cosLight = Vector3Ops::Dot( light, onb.w() );
		if( cosLight <= 0 ) return 0;
		const double cosinePdf = cosLight * INV_PI;
		const double vndfPdf = MicrofacetUtils::VNDF_Pdf_Aniso(
			view, light, onb, alphaX, alphaY );
		if( !IsFiniteNonNegative( vndfPdf ) ) return -1;
		return 0.5 * cosinePdf + 0.5 * vndfPdf;
	}

	static void AccumulateRGB(
		ChannelMoments& moments,
		const RISEPel& f,
		const double cosLight,
		const double density )
	{
		double weights[3];
		for( int c = 0; c < 3; ++c ) {
			const double value = f[c];
			weights[c] = value * cosLight / density;
			if( !IsFiniteNonNegative( value ) || !IsFiniteNonNegative( weights[c] ) ) {
				++moments.invalid;
				return;
			}
		}
		for( int c = 0; c < 3; ++c ) {
			moments.sum[c] += weights[c];
			moments.sumSq[c] += weights[c] * weights[c];
		}
	}

	static void AccumulateNM(
		ChannelMoments& moments,
		const double f,
		const double cosLight,
		const double density )
	{
		const double weight = f * cosLight / density;
		if( !IsFiniteNonNegative( f ) || !IsFiniteNonNegative( weight ) ) {
			++moments.invalid;
			return;
		}
		moments.sum[0] += weight;
		moments.sumSq[0] += weight * weight;
	}

	static ChannelMoments IntegrateRGB(
		GGXBRDF& brdf,
		const Case& c,
		const unsigned int seed,
		const int samples )
	{
		ChannelMoments moments;
		const Vector3 view = Direction( c.thetaDeg, c.azimuthDeg );
		const RayIntersectionGeometric ri = MakeIntersectionForView( view );
		OrthonormalBasis3D onb;
		onb.CreateFromW( Vector3( 0, 0, 1 ) );
		RandomNumberGenerator rng( seed );

		for( int i = 0; i < samples; ++i ) {
			const bool chooseCosine = rng.CanonicalRandom() < 0.5;
			const double u1 = rng.CanonicalRandom();
			const double u2 = rng.CanonicalRandom();
			Vector3 light;
			if( chooseCosine ) {
				light = SampleCosineHemisphere( u1, u2 );
			} else {
				const Vector3 m = MicrofacetUtils::VNDF_Sample_Aniso(
					view, onb, c.alphaX, c.alphaY, u1, u2 );
				const double viewDotM = Vector3Ops::Dot( view, m );
				light = Vector3Ops::Normalize( m * ( 2.0 * viewDotM ) - view );
			}

			if( !IsFiniteDirection( light ) ) {
				++moments.invalid;
				continue;
			}
			const double cosLight = Vector3Ops::Dot( light, onb.w() );
			if( !std::isfinite( cosLight ) ) {
				++moments.invalid;
				continue;
			}
			if( cosLight <= 0 ) {
				++moments.belowHorizon;
				continue;
			}
			const double density = MixtureDensity( view, light, onb, c.alphaX, c.alphaY );
			if( !std::isfinite( density ) || density <= 0 ) {
				++moments.invalid;
				continue;
			}
			AccumulateRGB( moments, brdf.value( light, ri ), cosLight, density );
		}
		return moments;
	}

	static ChannelMoments IntegrateNM(
		GGXBRDF& brdf,
		const Case& c,
		const double nm,
		const unsigned int seed,
		const int samples )
	{
		ChannelMoments moments;
		const Vector3 view = Direction( c.thetaDeg, c.azimuthDeg );
		const RayIntersectionGeometric ri = MakeIntersectionForView( view );
		OrthonormalBasis3D onb;
		onb.CreateFromW( Vector3( 0, 0, 1 ) );
		RandomNumberGenerator rng( seed );

		for( int i = 0; i < samples; ++i ) {
			const bool chooseCosine = rng.CanonicalRandom() < 0.5;
			const double u1 = rng.CanonicalRandom();
			const double u2 = rng.CanonicalRandom();
			Vector3 light;
			if( chooseCosine ) {
				light = SampleCosineHemisphere( u1, u2 );
			} else {
				const Vector3 m = MicrofacetUtils::VNDF_Sample_Aniso(
					view, onb, c.alphaX, c.alphaY, u1, u2 );
				const double viewDotM = Vector3Ops::Dot( view, m );
				light = Vector3Ops::Normalize( m * ( 2.0 * viewDotM ) - view );
			}

			if( !IsFiniteDirection( light ) ) {
				++moments.invalid;
				continue;
			}
			const double cosLight = Vector3Ops::Dot( light, onb.w() );
			if( !std::isfinite( cosLight ) ) {
				++moments.invalid;
				continue;
			}
			if( cosLight <= 0 ) {
				++moments.belowHorizon;
				continue;
			}
			const double density = MixtureDensity( view, light, onb, c.alphaX, c.alphaY );
			if( !std::isfinite( density ) || density <= 0 ) {
				++moments.invalid;
				continue;
			}
			AccumulateNM( moments, brdf.valueNM( light, ri, nm ), cosLight, density );
		}
		return moments;
	}

	static double Mean( const ChannelMoments& moments, const int channel, const int samples )
	{
		return moments.sum[channel] / static_cast<double>( samples );
	}

	static double StandardError( const ChannelMoments& moments, const int channel, const int samples )
	{
		if( !std::isfinite(moments.sum[channel]) || !std::isfinite(moments.sumSq[channel]) )
			return 0; // The reporting checks reject non-finite moments explicitly.
		if( samples < 2 ) return 0;
		const double mean = Mean( moments, channel, samples );
		const double centered = moments.sumSq[channel] - samples * mean * mean;
		const double variance = std::max( 0.0, centered / static_cast<double>( samples - 1 ) );
		return std::sqrt( variance / static_cast<double>( samples ) );
	}

	static bool CheckRGBBound(
		const std::string& label,
		const ChannelMoments& moments,
		const int samples )
	{
		bool passed = moments.invalid == 0;
		std::cout << "  " << std::left << std::setw( 58 ) << label;
		for( int c = 0; c < 3; ++c ) {
			const double mean = Mean( moments, c, samples );
			const double se = StandardError( moments, c, samples );
			const double limit = 1.0 + kEnergySigma * se + kEnergyFloor;
			std::cout << "  " << std::fixed << std::setprecision( 4 ) << mean
				<< "+/-" << std::setprecision( 4 ) << se;
			if( !std::isfinite(moments.sumSq[c]) || !std::isfinite( mean ) || !std::isfinite( se ) || mean > limit ) passed = false;
		}
		std::cout << "  invalid=" << moments.invalid << " below=" << moments.belowHorizon
			<< ( passed ? "  PASS" : "  FAIL" ) << "\n";
		++checks;
		if( !passed ) ++failures;
		return passed;
	}

	static bool CheckNMBound(
		const std::string& label,
		const double nm,
		const ChannelMoments& moments,
		const int samples )
	{
		const double mean = Mean( moments, 0, samples );
		const double se = StandardError( moments, 0, samples );
		const double limit = 1.0 + kEnergySigma * se + kEnergyFloor;
		const bool passed = moments.invalid == 0 && std::isfinite(moments.sumSq[0]) && std::isfinite( mean ) &&
			std::isfinite( se ) && mean <= limit;
		std::cout << "  " << std::left << std::setw( 52 ) << label
			<< "  nm=" << std::setw( 3 ) << static_cast<int>( nm )
			<< "  " << std::fixed << std::setprecision( 4 ) << mean
			<< "+/-" << std::setprecision( 4 ) << se
			<< "  invalid=" << moments.invalid << " below=" << moments.belowHorizon
			<< ( passed ? "  PASS" : "  FAIL" ) << "\n";
		++checks;
		if( !passed ) ++failures;
		return passed;
	}

	static bool RunRGBCase( const Case& c, const unsigned int seed )
	{
		BrdfFixture fixture( c );
		return CheckRGBBound( c.label, IntegrateRGB( *fixture.brdf, c, seed, kRGBSamples ), kRGBSamples );
	}

	static bool RunNMCase( const Case& c, const double nm, const unsigned int seed )
	{
		BrdfFixture fixture( c );
		return CheckNMBound( c.label, nm, IntegrateNM( *fixture.brdf, c, nm, seed, kNMSamples ), kNMSamples );
	}

	static bool CheckRGBReciprocity(
		GGXBRDF& brdf,
		const Vector3& view,
		const Vector3& light,
		const char* label )
	{
		const RISEPel forward = brdf.value( light, MakeIntersectionForView( view ) );
		const RISEPel reverse = brdf.value( view, MakeIntersectionForView( light ) );
		bool passed = true;
		for( int c = 0; c < 3; ++c ) {
			if( !IsFiniteNonNegative( forward[c] ) || !IsFiniteNonNegative( reverse[c] ) ) {
				passed = false;
				continue;
			}
			const double denom = std::max( 1e-12, std::max( std::fabs( forward[c] ), std::fabs( reverse[c] ) ) );
			if( std::fabs( forward[c] - reverse[c] ) / denom > 1e-8 ) passed = false;
		}
		std::cout << "  reciprocity RGB " << std::left << std::setw( 34 ) << label
			<< ( passed ? " PASS" : " FAIL" ) << "\n";
		++checks;
		if( !passed ) ++failures;
		return passed;
	}

	static bool CheckNMReciprocity(
		GGXBRDF& brdf,
		const Vector3& view,
		const Vector3& light,
		const double nm,
		const char* label )
	{
		const double forward = brdf.valueNM( light, MakeIntersectionForView( view ), nm );
		const double reverse = brdf.valueNM( view, MakeIntersectionForView( light ), nm );
		const double denom = std::max( 1e-12, std::max( std::fabs( forward ), std::fabs( reverse ) ) );
		const bool passed = IsFiniteNonNegative( forward ) && IsFiniteNonNegative( reverse ) &&
			std::fabs( forward - reverse ) / denom <= 1e-8;
		std::cout << "  reciprocity NM  " << std::left << std::setw( 34 ) << label
			<< " nm=" << static_cast<int>( nm ) << ( passed ? " PASS" : " FAIL" ) << "\n";
		++checks;
		if( !passed ) ++failures;
		return passed;
	}

	static bool TestReciprocity()
	{
		std::cout << "\n--- Deterministic reciprocity ---\n";
		const Case cases[] = {
			{ "Schlick aniso F0=.04", eFresnelSchlickF0, 1.0, 0.04, 0.05, 0.5, 0, 0, 0 },
			{ "Conductor mixed", eFresnelConductor, 1.0, 1.0, 0.05, 0.5, 0, 0, 0 },
			{ "Thin-film mixed 350nm", eFresnelThinFilmConductor, 1.0, 1.0, 0.05, 0.5, 0, 0, 350.0 }
		};
		const Vector3 frontView = Direction( 60, 25 );
		const Vector3 frontLight = Direction( 80, 205 );
		bool passed = true;
		for( const Case& c : cases ) {
			BrdfFixture fixture( c );
			passed &= CheckRGBReciprocity( *fixture.brdf, frontView, frontLight, c.label );
			passed &= CheckRGBReciprocity( *fixture.brdf, -frontView, -frontLight, c.label );
			for( const double nm : { 450.0, 550.0, 650.0 } ) {
				passed &= CheckNMReciprocity( *fixture.brdf, frontView, frontLight, nm, c.label );
				passed &= CheckNMReciprocity( *fixture.brdf, -frontView, -frontLight, nm, c.label );
			}
		}
		return passed;
	}

	static bool TestSchlickSweep()
	{
		std::cout << "\n--- Schlick RGB energy sweep ---\n";
		bool passed = true;
		unsigned int seed = 1100;
		const double f0s[] = { 0.0, 0.04, 0.5, 1.0 };
		const double alphas[] = { 0.02, 0.16, 0.6, 1.0 };
		const double thetas[] = { 0.0, 60.0, 80.0 };
		for( const double f0 : f0s ) {
			for( const double alpha : alphas ) {
				for( const double theta : thetas ) {
					std::ostringstream label;
					label << "Schlick iso F0=" << f0 << " alpha=" << alpha << " theta=" << theta
						<< ( f0 == 1.0 ? " spec-only" : " mixed" );
					const std::string stableLabel = label.str();
					const Case c = { stableLabel.c_str(), eFresnelSchlickF0,
						f0 == 1.0 ? 0.0 : 1.0, f0, alpha, alpha, theta, 0.0, 0.0 };
					passed &= RunRGBCase( c, seed++ );
				}
			}
		}

		const double azimuths[] = { 0.0, 90.0 };
		for( const double f0 : f0s ) {
			for( const double azimuth : azimuths ) {
				for( const double theta : thetas ) {
					std::ostringstream label;
					label << "Schlick aniso(.05,.5) F0=" << f0 << " theta=" << theta
						<< " az=" << azimuth << ( f0 == 1.0 ? " spec-only" : " mixed" );
					const std::string stableLabel = label.str();
					const Case c = { stableLabel.c_str(), eFresnelSchlickF0,
						f0 == 1.0 ? 0.0 : 1.0, f0, 0.05, 0.5, theta, azimuth, 0.0 };
					passed &= RunRGBCase( c, seed++ );
				}
			}
		}
		return passed;
	}

	static bool TestConductorAndFilmControls()
	{
		std::cout << "\n--- Conductor and thin-film RGB/NM controls ---\n";
		bool passed = true;
		unsigned int seed = 3100;
		const double thetas[] = { 0.0, 80.0 };

		for( const double diffuse : { 0.0, 1.0 } ) {
			for( const double theta : thetas ) {
				std::ostringstream label;
				label << "Conductor " << ( diffuse == 0.0 ? "spec-only" : "mixed" )
					<< " alpha=.16 theta=" << theta;
				const std::string stableLabel = label.str();
				const Case c = { stableLabel.c_str(), eFresnelConductor, diffuse, 1.0,
					0.16, 0.16, theta, 0.0, 0.0 };
				passed &= RunRGBCase( c, seed++ );
				for( const double nm : { 450.0, 550.0, 650.0 } ) passed &= RunNMCase( c, nm, seed++ );
			}
		}

		for( const double diffuse : { 0.0, 1.0 } ) {
			for( const double thickness : { 0.0, 350.0 } ) {
				for( const double theta : thetas ) {
					std::ostringstream label;
					label << "Thin-film " << ( diffuse == 0.0 ? "spec-only" : "mixed" )
						<< " t=" << thickness << "nm alpha=.16 theta=" << theta;
					const std::string stableLabel = label.str();
					const Case c = { stableLabel.c_str(), eFresnelThinFilmConductor, diffuse, 1.0,
						0.16, 0.16, theta, 0.0, thickness };
					passed &= RunRGBCase( c, seed++ );
					for( const double nm : { 450.0, 550.0, 650.0 } ) passed &= RunNMCase( c, nm, seed++ );
				}
			}
		}

		// Small NM Schlick controls: the primary sweep is RGB, but the DL-37
		// fix has an independent hero-wavelength branch too.  The F0=1 row
		// keeps a spectral specular-only baseline beside the mixed row.
		const double schlickF0s[] = { 0.04, 1.0 };
		for( const double f0 : schlickF0s ) {
			const Case c = { f0 == 1.0 ? "Schlick NM F0=1 spec-only alpha=.16 theta=80" :
				"Schlick NM F0=.04 mixed alpha=.16 theta=80", eFresnelSchlickF0,
				f0 == 1.0 ? 0.0 : 1.0, f0, 0.16, 0.16, 80.0, 0.0, 0.0 };
			for( const double nm : { 450.0, 550.0, 650.0 } ) {
				passed &= RunNMCase( c, nm, seed++ );
			}
		}
		return passed;
	}

	// DL-77 (debt-ggx3 CLOSED): GGXBRDF/GGXSPF's Kulla-Conty compensation
	// used to look up the height-correlated LUT at the ISOTROPIZED
	// alphaEff=sqrt(alphaX*alphaY), while the single-scatter term above it
	// uses direction-dependent per-axis MicrofacetUtils::GGX_G2_Aniso --
	// for strongly anisotropic alphaX/alphaY this under-compensated
	// (measured E_ss deficits of 11-44% at F0=1 against an independent
	// per-axis quadrature; see the DL-77 ledger row and
	// docs/DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md "DL-77").
	// CheckRGBBound's energy gate is one-sided (fails only on a GAIN,
	// `mean > 1 + 6*SE + 0.005`), so this deficit was invisible to every
	// aniso F0=1 row in TestSchlickSweep above -- it silently PASSED
	// despite being measurably wrong.  Fixed by resolving the anisotropy
	// RATIO (and, separately, wi/wo's actual AZIMUTH -- an azimuth-
	// averaged-only first cut regressed the existing (.05,.5) az=90 row
	// in TestSchlickSweep into a GAIN; see MicrofacetEnergyLUT.h's
	// LookupEssG2AnisoDirectional doc comment) in the Kulla-Conty LUT.
	//
	// CheckAnisotropicFurnaceBound below is TWO-SIDED (unlike
	// CheckRGBBound): a LOWER floor in addition to the standard upper
	// energy-conservation bound, specifically so a regression of THIS fix
	// (silently reverting to the isotropized lookup, which reads as low
	// as ~0.57 on the worst-measured configuration) fails loudly here
	// instead of being invisible to a gain-only check again.  kAnisoFloor
	// is fixed well below the fixed values (~0.97-1.00 measured) but
	// comfortably above the old broken values (~0.57-0.93) so it has
	// margin against MC noise without being able to pass a reintroduced
	// regression.
	static const double kAnisoFloor = 0.90;

	static bool CheckAnisotropicFurnaceBound( const Case& c, const unsigned int seed )
	{
		BrdfFixture fixture( c );
		const ChannelMoments moments = IntegrateRGB( *fixture.brdf, c, seed, kRGBSamples );
		const double mean = Mean( moments, 0, kRGBSamples );
		const double se = StandardError( moments, 0, kRGBSamples );
		const double upperLimit = 1.0 + kEnergySigma * se + kEnergyFloor;
		const double lowerLimit = kAnisoFloor - kEnergySigma * se;
		const bool validSamples = moments.invalid == 0 && std::isfinite( moments.sumSq[0] ) &&
			std::isfinite( mean ) && std::isfinite( se );
		const bool passed = validSamples && mean <= upperLimit && mean >= lowerLimit;

		std::cout << "  " << std::left << std::setw( 58 ) << c.label
			<< "  " << std::fixed << std::setprecision( 4 ) << mean
			<< "+/-" << std::setprecision( 4 ) << se
			<< "  invalid=" << moments.invalid << " below=" << moments.belowHorizon
			<< ( passed ? "  PASS" : "  FAIL" ) << "\n";
		++checks;
		if( !passed ) ++failures;
		return passed;
	}

	static bool TestAnisotropicFurnaceDL77()
	{
		std::cout << "\n--- DL-77: strongly anisotropic F0=1 furnace, two-sided gate ---\n";
		bool passed = true;

		// alphaX=.02/alphaY=1.0 (ratio=50) is the worst-measured case in
		// the DL-77 ledger row's E_ss evidence (0.533 true vs 0.972
		// isotropized lookup at mu=0.5, an ~45% relative Ess gap):
		// spec-only (F0=1, diffuse=0) so the deficit is not diluted by a
		// diffuse term.  This is the same config the KNOWN-FAILURE control
		// this test replaces used to record (pre-fix: 0.929; the
		// isotropized-lookup-only regression this replaces would have
		// read close to the raw E_ss deficit, ~0.57-0.7).
		passed &= CheckAnisotropicFurnaceBound(
			{ "Schlick aniso(.02,1.0) ratio=50 F0=1 theta=60 az=0 spec-only",
			  eFresnelSchlickF0, 0.0, 1.0, 0.02, 1.0, 60.0, 0.0, 0.0 }, 9001 );

		// alphaX=.05/alphaY=.5 (ratio=10) at az=45 -- BETWEEN the phi grid
		// points (0,15,...,90 in the LUT), so this exercises the
		// quadrilinear phi interpolation rather than an exact grid hit
		// like TestSchlickSweep's az=0/az=90 rows.  theta=75 is a
		// different incidence angle than the existing (.05,.5) rows.
		passed &= CheckAnisotropicFurnaceBound(
			{ "Schlick aniso(.05,.5) ratio=10 F0=1 theta=75 az=45 spec-only",
			  eFresnelSchlickF0, 0.0, 1.0, 0.05, 0.5, 75.0, 45.0, 0.0 }, 9002 );

		// alphaX=.1/alphaY=.9 (ratio=9) at az=90, theta=70 -- a third,
		// independent (alphaX,alphaY,theta,azimuth) combination from the
		// DL-77 ledger row's second-worst measured E_ss deficit
		// (0.573 true vs 0.822 isotropized lookup at mu=0.5).
		passed &= CheckAnisotropicFurnaceBound(
			{ "Schlick aniso(.1,.9) ratio=9 F0=1 theta=70 az=90 spec-only",
			  eFresnelSchlickF0, 0.0, 1.0, 0.1, 0.9, 70.0, 90.0, 0.0 }, 9003 );

		// P1 red proof (debt-ggx3): every row above has alphaX < alphaY,
		// so they were blind to the axis-swap azimuth-mirroring bug the
		// P1 follow-up fixed -- the table's baked phi=0 axis always meant
		// "the smaller-alpha axis", but the lookup read phi off the
		// caller's (localX,localY) with no swap when the caller's actual
		// alphaX was the LARGER of the pair (e.g. every glTF
		// pbrmetallicroughness_material, which always sets alphaX>=
		// alphaY).  These two rows deliberately pass alphaX>alphaY and
		// reproduce the two configurations cited in the DL-77 ledger's
		// P1 follow-up: on the unfixed lookup this test measured
		// mean=1.1699 (row 1, energy GAIN -- alone enough to fail the
		// upper bound) and mean=0.7613 (row 2, well under kAnisoFloor).
		// Fixed, both read close to 1.0 like the alphaX<alphaY rows
		// above.
		passed &= CheckAnisotropicFurnaceBound(
			{ "Schlick aniso(.9,.1) ratio=9 F0=1 theta=70 az=0 spec-only (P1: alphaX>alphaY)",
			  eFresnelSchlickF0, 0.0, 1.0, 0.9, 0.1, 70.0, 0.0, 0.0 }, 9004 );

		passed &= CheckAnisotropicFurnaceBound(
			{ "Schlick aniso(.827,.09) ratio=9.19 F0=1 theta=80 az=90 spec-only (P1: alphaX>alphaY)",
			  eFresnelSchlickF0, 0.0, 1.0, 0.827, 0.09, 80.0, 90.0, 0.0 }, 9005 );

		// P3-d (debt-ggx3, review round 2): a TIGHT two-sided regression
		// guard at the exact point review round 2 used to re-derive the
		// DL-77 residual numbers (alphaX=0.9353, alphaY=0.0752,
		// cos=0.1211 i.e. theta=83.0441deg, az=85.5 -- deliberately
		// mid-interval between the new grid's 82.5/90 degree phi nodes,
		// where table-vs-truth peaked at 3.2-3.3% on the pre-fix
		// ANISO_PHI_SIZE=7 grid).  Unlike CheckAnisotropicFurnaceBound's
		// loose [kAnisoFloor, 1+6SE+.005] band above (a P1 guard against
		// a full regression back to the isotropized lookup, ~0.57-0.93),
		// kPhiGridTightTol below is sized to this test's OWN measured
		// mean/SE at kRGBSamples plus the review round 2 residual
		// measurement (2.58% at this exact point, via an independent
		// 4e6-sample scratch program -- see
		// docs/DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md's "P2 root-cause
		// correction") -- so a future re-coarsening of ANISO_PHI_SIZE, or
		// a change to the low-alpha grid that reopens this specific
		// residual, fails this row long before it could regress all the
		// way down to kAnisoFloor.
		{
			// Uses a much larger local sample count than kRGBSamples
			// (30000): at kRGBSamples this configuration's furnace mean
			// moves only ~0.005 between the pre-fix (ANISO_PHI_SIZE=7)
			// and post-fix (=13) tables -- the H6 direction-aware
			// selection weight dilutes the raw ~2.6% E_ss-level residual
			// heavily before it reaches the full-BRDF furnace mean -- and
			// kRGBSamples' own SE (~0.0032) is too coarse to separate
			// that 0.005 shift from statistical noise (confirmed: at
			// kRGBSamples this row PASSES against both the pre-fix and
			// post-fix header, so it would not actually catch a
			// regression).  400000 samples brings SE down to ~0.0009,
			// enough margin below the measured shift for this to be a
			// real regression guard.
			const int kTightSamples = 400000;
			const Case c = { "Schlick aniso(.9353,.0752) F0=1 theta=83.0441 az=85.5 spec-only (P2: phi-grid regression guard)",
				eFresnelSchlickF0, 0.0, 1.0, 0.9353, 0.0752, 83.0441, 85.5, 0.0 };
			BrdfFixture fixture( c );
			const ChannelMoments moments = IntegrateRGB( *fixture.brdf, c, 9006, kTightSamples );
			const double mean = Mean( moments, 0, kTightSamples );
			const double se = StandardError( moments, 0, kTightSamples );
			// Measured on this tree post-fix at kTightSamples:
			// mean=0.98434, se=0.00087.  Red-proof (debt-ggx3 review
			// round 3): swapping in the pre-fix (ANISO_PHI_SIZE=7)
			// header and rebuilding an isolated copy of just this test
			// measures 0.97933+/-0.00088 at the SAME seed/sample count.
			// That is only a ~5.7*se separation from THIS run's mean --
			// a *6*se* band (as this row originally used) is therefore
			// [0.97912, 0.98956], which the pre-fix value 0.97933 falls
			// INSIDE of: the original 6*se band did not actually
			// red-proof (verified: it reads PASS, not FAIL, against the
			// pre-fix header).  Fixed by tightening to a *4*se* band and
			// computing the multiplier against THIS run's own runtime
			// `se` (not a copied-in literal), so the row tracks its own
			// statistics if the sample count or RNG ever changes: lower
			// bound = mean - 4*se = 0.98434 - 4*0.00087 = 0.98086, a
			// 1.75*se margin above the pre-fix 0.97933 -- comfortably
			// red-proofed (see the isolated pre-fix-header rebuild's
			// FAIL line in the DL-77 ledger row / fix commit message).
			// A looser band built from the raw table-level residual
			// (3*se + 0.0258, the 2.58% relative residual measured at
			// this exact point via an independent 4e6-sample scratch
			// program, converted to absolute since the expected value
			// is ~1.0) would swallow the 0.006 shift entirely, so this
			// row uses the tight statistics-only band instead --
			// appropriate because, unlike the generic
			// CheckAnisotropicFurnaceBound rows above (which must
			// tolerate a much larger regression all the way back to the
			// isotropized lookup, ~0.57-0.93), this row's whole purpose
			// is catching a SMALL re-coarsening of ANISO_PHI_SIZE
			// specifically.
			const double kExpectedMean = 0.98434;
			const double kPhiGridTightSigma = 4.0;
			const double kPhiGridTightTol = kPhiGridTightSigma * se;
			const double lowerLimit = kExpectedMean - kPhiGridTightTol;
			const double upperLimit = kExpectedMean + kPhiGridTightTol;
			const bool validSamples = moments.invalid == 0 && std::isfinite( moments.sumSq[0] ) &&
				std::isfinite( mean ) && std::isfinite( se );
			const bool rowPassed = validSamples && mean >= lowerLimit && mean <= upperLimit;
			std::cout << "  " << std::left << std::setw( 58 ) << c.label
				<< "  " << std::fixed << std::setprecision( 5 ) << mean
				<< "+/-" << std::setprecision( 5 ) << se
				<< "  band=[" << lowerLimit << "," << upperLimit << "]"
				<< "  invalid=" << moments.invalid
				<< ( rowPassed ? "  PASS" : "  FAIL" ) << "\n";
			++checks;
			if( !rowPassed ) ++failures;
			passed &= rowPassed;
		}

		return passed;
	}
}

int main()
{
	std::cout << "=== GGX Diffuse Transmission Test (DL-37) ===\n";
	GlobalLog();

	bool passed = true;
	passed &= TestReciprocity();
	passed &= TestSchlickSweep();
	passed &= TestConductorAndFilmControls();
	passed &= TestAnisotropicFurnaceDL77();

	std::cout << "GGXDiffuseTransmissionTest: " << checks << " checks, " << failures << " failures\n";
	std::cout << "=== " << ( passed ? "ALL TESTS PASSED" : "TESTS FAILED" ) << " ===\n";
	return passed ? 0 : 1;
}
