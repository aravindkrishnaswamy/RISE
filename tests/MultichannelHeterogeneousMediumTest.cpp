//////////////////////////////////////////////////////////////////////
//
//  MultichannelHeterogeneousMediumTest.cpp
//
//  Phase-A gate for the painter-baked carbon + temperature medium:
//  shared trilinear lattice, chromatic phi(T) optics, the 10^-3 g/m^3
//  conversion, spectral tracking, scene-unit invariance, and §9 requiredness.
//
//////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScalarPainterManager.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Materials/HenyeyGreensteinPhaseFunction.h"
#include "../src/Library/Materials/HeterogeneousMedium.h"
#include "../src/Library/Materials/HomogeneousMedium.h"
#include "../src/Library/Parsers/ChunkParserRegistry.h"
#include "../src/Library/Rendering/FrameStore.h"
#include "../src/Library/Rendering/Rasterizer.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/Color/ColorUtils.h"
#include "../src/Library/Utilities/GaussLegendreQuadrature.h"
#include "../src/Library/Utilities/MediumTransport.h"
#include "../src/Library/Utilities/PlanckRadiance.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

namespace
{
	int passed = 0;
	int failed = 0;

	void Check( const bool condition, const char* label )
	{
		if( condition ) {
			++passed;
		} else {
			++failed;
			std::cout << "FAIL: " << label << std::endl;
		}
	}

	bool Near( const Scalar actual, const Scalar expected, const Scalar tolerance )
	{
		return std::fabs( actual - expected ) <= tolerance;
	}

	bool NearRelative( const Scalar actual, const Scalar expected, const Scalar tolerance )
	{
		const Scalar scale = std::fmax( std::fabs( expected ), Scalar( 1e-300 ) );
		return std::fabs( actual - expected ) <= tolerance * scale;
	}

	bool HasFireReason(
		const IMedium& medium,
		const bool predictiveRequested,
		const char* reason
		)
	{
		for( unsigned int i=0;
			i<medium.GetFireRenderReasonCodeCount(predictiveRequested); ++i ) {
			const char* candidate = medium.GetFireRenderReasonCode(
				predictiveRequested,i);
			if( candidate && std::strcmp(candidate,reason) == 0 ) return true;
		}
		return false;
	}

	RISEPel PelResponse( const Scalar nm )
	{
		XYZPel xyz;
		if( !ColorUtils::XYZFromNM(xyz,nm) ) return RISEPel(0.0);
		return ColorUtils::XYZtoRec709RGBMatrixOnly(xyz) *
			(1.0/ColorUtils::CIE_Y_Integral(380.0,780.0));
	}

	RISEPel ResponsePowerMean( const Scalar exponent )
	{
		RISEPel mass(0.0);
		RISEPel weighted(0.0);
		for( unsigned int channel = 0; channel < 3u; ++channel ) {
			mass[channel] = GaussLegendre21::IntegrateVisibleBand(
				[channel](const Scalar nm) { return PelResponse(nm)[channel]; } );
			weighted[channel] = GaussLegendre21::IntegrateVisibleBand(
				[channel,exponent](const Scalar nm) {
					return PelResponse(nm)[channel]*pow(633.0/nm,exponent);
				} );
			weighted[channel] /= mass[channel];
		}
		return weighted;
	}

	RISEPel ResponseMass()
	{
		RISEPel mass(0.0);
		for( unsigned int channel = 0; channel < 3u; ++channel ) {
			mass[channel] = GaussLegendre21::IntegrateVisibleBand(
				[channel](const Scalar nm) { return PelResponse(nm)[channel]; } );
		}
		return mass;
	}

	Scalar SamplingPowerMass( const Scalar exponent )
	{
		return GaussLegendre21::IntegrateVisibleBand(
			[exponent](const Scalar nm) {
				XYZPel xyz;
				if( !ColorUtils::XYZFromNM(xyz,nm) ) return Scalar(0.0);
				return (xyz.X+xyz.Y+xyz.Z)*pow(633.0/nm,exponent);
			} );
	}

	Scalar ScalarFromBits( const std::uint64_t bits )
	{
		static_assert( sizeof( Scalar ) == sizeof( bits ),
			"non-finite regression inputs require binary64 Scalar" );
		volatile std::uint64_t barrier = bits;
		const std::uint64_t materialised = barrier;
		Scalar value = 0.0;
		std::memcpy( &value, &materialised, sizeof( value ) );
		return value;
	}

	class AffineWorldScalarPainter :
		public virtual IScalarPainter,
		public virtual Implementation::Reference
	{
	public:
		Scalar bias;
		Scalar x;
		Scalar y;
		Scalar z;

		AffineWorldScalarPainter( Scalar bias_, Scalar x_, Scalar y_, Scalar z_ ) :
		  bias( bias_ ), x( x_ ), y( y_ ), z( z_ )
		{
		}

		ScalarTriple GetValuesAt( const RayIntersectionGeometric& ri ) const override
		{
			return ScalarTriple(
				bias + x * ri.ptIntersection.x + y * ri.ptIntersection.y + z * ri.ptIntersection.z );
		}

		bool HasPerChannelVariation() const override { return false; }

	protected:
		~AffineWorldScalarPainter() override = default;
	};

	class TrilinearProductPainter :
		public virtual IScalarPainter,
		public virtual Implementation::Reference
	{
		const Scalar bias_;
		const Scalar scale_;

	public:
		TrilinearProductPainter( const Scalar bias, const Scalar scale ) :
		  bias_(bias), scale_(scale)
		{
		}

		ScalarTriple GetValuesAt( const RayIntersectionGeometric& ri ) const override
		{
			return ScalarTriple( bias_ + scale_*ri.ptIntersection.x*
				ri.ptIntersection.y*ri.ptIntersection.z );
		}

		bool HasPerChannelVariation() const override { return false; }

	protected:
		~TrilinearProductPainter() override = default;
	};

	class QuadraticXPainter :
		public virtual IScalarPainter,
		public virtual Implementation::Reference
	{
	public:
		ScalarTriple GetValuesAt( const RayIntersectionGeometric& ri ) const override
		{
			return ScalarTriple( ri.ptIntersection.x*ri.ptIntersection.x );
		}

		bool HasPerChannelVariation() const override { return false; }

	protected:
		~QuadraticXPainter() override = default;
	};

	class BilinearHumpTemperaturePainter :
		public virtual IScalarPainter,
		public virtual Implementation::Reference
	{
	public:
		ScalarTriple GetValuesAt( const RayIntersectionGeometric& ri ) const override
		{
			return ScalarTriple( 600.0 + 1500.0*ri.ptIntersection.x*
				(1.0-ri.ptIntersection.y) );
		}

		bool HasPerChannelVariation() const override { return false; }

	protected:
		~BilinearHumpTemperaturePainter() override = default;
	};

	class IntervalTopHatFunction :
		public virtual IFunction1D,
		public virtual Implementation::Reference
	{
		const Scalar minimum_;
		const Scalar maximum_;
		const Scalar height_;

	public:
		IntervalTopHatFunction(
			const Scalar minimum,
			const Scalar maximum,
			const Scalar height = 1.0
			) : minimum_(minimum), maximum_(maximum), height_(height)
		{
		}

		Scalar Evaluate( const Scalar wavelength ) const override
		{
			return wavelength >= minimum_ && wavelength <= maximum_ ? height_ : 0.0;
		}

	protected:
		~IntervalTopHatFunction() override = default;
	};

	class OffsetQuadraticFunction :
		public virtual IFunction1D,
		public virtual Implementation::Reference
	{
		const Scalar origin_;

	public:
		explicit OffsetQuadraticFunction( const Scalar origin ) : origin_(origin) {}

		Scalar Evaluate( const Scalar wavelength ) const override
		{
			const Scalar offset = wavelength-origin_;
			return offset*offset;
		}

	protected:
		~OffsetQuadraticFunction() override = default;
	};

	class ThinSheetXPainter :
		public virtual IScalarPainter,
		public virtual Implementation::Reference
	{
		const Scalar minimum_;
		const Scalar maximum_;
		const Scalar value_;

	public:
		ThinSheetXPainter(
			const Scalar minimum,
			const Scalar maximum,
			const Scalar value
			) : minimum_(minimum), maximum_(maximum), value_(value)
		{
		}

		ScalarTriple GetValuesAt( const RayIntersectionGeometric& ri ) const override
		{
			return ScalarTriple(ri.ptIntersection.x >= minimum_ &&
				ri.ptIntersection.x <= maximum_ ? value_ : 0.0);
		}

		bool HasPerChannelVariation() const override { return false; }

	protected:
		~ThinSheetXPainter() override = default;
	};

	class ForcedBranchSampler : public ISampler
	{
		const Scalar branch_;
		RandomNumberGenerator random_;
		bool first_;

	public:
		ForcedBranchSampler( const Scalar branch, const unsigned int seed ) :
			branch_(branch), random_(seed), first_(true)
		{
		}

		void BeginSample() { first_ = true; }

		Scalar Get1D() override
		{
			if( first_ ) {
				first_ = false;
				return branch_;
			}
			return random_.CanonicalRandom();
		}

		Point2 Get2D() override { return Point2(Get1D(),Get1D()); }
	};

	Scalar ReferencePhiAwareSigmaT( const Scalar x )
	{
		const Scalar carbon = 1.0 + 8.0*x*x*x;
		const Scalar temperature = 600.0 + 1600.0*x*x*x;
		Scalar phi = 0.0;
		if( temperature >= 900.0 ) {
			phi = 1.0;
		} else if( temperature > 700.0 ) {
			const Scalar s = (temperature-700.0)/200.0;
			phi = s*s*(3.0-2.0*s);
		}
		return carbon*(0.2 + phi*(2.0-0.2));
	}

	Scalar ReferencePhiAwareOpticalDepth(
		const Scalar xBegin,
		const Scalar xEnd
		)
	{
		// Composite Simpson is an independent reference for the piecewise
		// degree-12 integrand.  The fixed even panel count puts the numerical
		// error far below the gate while making no use of renderer breakpoints.
		const unsigned int panelCount = 20000u;
		const Scalar h = (xEnd-xBegin)/Scalar(panelCount);
		Scalar sum = ReferencePhiAwareSigmaT(xBegin) +
			ReferencePhiAwareSigmaT(xEnd);
		for( unsigned int i = 1u; i < panelCount; ++i ) {
			const Scalar value = ReferencePhiAwareSigmaT(xBegin+Scalar(i)*h);
			sum += (i&1u ? 4.0 : 2.0)*value;
		}
		return sqrt(3.0)*h*sum/3.0;
	}

	Scalar ReferenceHumpSigmaT( const Scalar x )
	{
		const Scalar temperature = 600.0 + 1500.0*x*(1.0-x);
		Scalar phi = 0.0;
		if( temperature >= 900.0 ) {
			phi = 1.0;
		} else if( temperature > 700.0 ) {
			const Scalar s = (temperature-700.0)/200.0;
			phi = s*s*(3.0-2.0*s);
		}
		return 0.2 + phi*(2.0-0.2);
	}

	Scalar ReferenceHumpOpticalDepth()
	{
		const unsigned int panelCount = 20000u;
		const Scalar xBegin = 0.25;
		const Scalar xEnd = 0.75;
		const Scalar h = (xEnd-xBegin)/Scalar(panelCount);
		Scalar sum = ReferenceHumpSigmaT(xBegin) +
			ReferenceHumpSigmaT(xEnd);
		for( unsigned int i = 1u; i < panelCount; ++i ) {
			const Scalar value = ReferenceHumpSigmaT(xBegin+Scalar(i)*h);
			sum += (i&1u ? 4.0 : 2.0)*value;
		}
		return sqrt(2.0)*h*sum/3.0;
	}

	class DerivedMultichannelHeterogeneousMedium final :
		public MultichannelHeterogeneousMedium
	{
	public:
		DerivedMultichannelHeterogeneousMedium(
			const IScalarPainter& carbonPainter,
			const IScalarPainter& temperaturePainter,
			const IPhaseFunction& phase
			) :
		  MultichannelHeterogeneousMedium(
			carbonPainter, temperaturePainter, 2, 2, 2,
			Point3( 0, 0, 0 ), Point3( 1, 1, 1 ), 1.0,
			0.26, 1800.0, 0.40, 0.85, 8.7, 1.2, 0.70, -0.35,
			phase )
		{
		}

		~DerivedMultichannelHeterogeneousMedium() override = default;
	};

	class PluginPhase final :
		public virtual IPhaseFunction,
		public virtual Implementation::Reference
	{
	public:
		Scalar Evaluate( const Vector3&, const Vector3& ) const override
		{
			return 1.0 / FOUR_PI;
		}
		Vector3 Sample( const Vector3& wi, ISampler& ) const override { return wi; }
		Scalar Pdf( const Vector3&, const Vector3& ) const override
		{
			return 1.0 / FOUR_PI;
		}

	protected:
		~PluginPhase() override = default;
	};

	class DerivedHomogeneousMedium final : public HomogeneousMedium
	{
	public:
		explicit DerivedHomogeneousMedium( const IPhaseFunction& phase ) :
		  HomogeneousMedium( RISEPel( 0, 0, 0 ), RISEPel( 1, 1, 1 ), phase )
		{
		}

		~DerivedHomogeneousMedium() override = default;
	};

	class PluginMedium final :
		public virtual IMedium,
		public virtual Implementation::Reference
	{
	public:
		explicit PluginMedium( const IPhaseFunction& phase ) : m_phase( phase )
		{
			m_phase.addref();
		}

		MediumCoefficients GetCoefficients( const Point3& ) const override
		{
			MediumCoefficients c;
			c.sigma_t = RISEPel( 1, 1, 1 );
			c.sigma_s = RISEPel( 1, 1, 1 );
			c.emission = RISEPel( 0, 0, 0 );
			return c;
		}
		MediumCoefficientsNM GetCoefficientsNM( const Point3&, const Scalar ) const override
		{
			MediumCoefficientsNM c;
			c.sigma_t = 1;
			c.sigma_s = 1;
			c.emission = 0;
			return c;
		}
		const IPhaseFunction* GetPhaseFunction() const override { return &m_phase; }
		Scalar SampleDistance(
			const Ray&, const Scalar maxDist, ISampler&, bool& scattered ) const override
		{
			scattered = false;
			return maxDist;
		}
		Scalar SampleDistanceNM(
			const Ray&, const Scalar maxDist, const Scalar,
			ISampler&, bool& scattered ) const override
		{
			scattered = false;
			return maxDist;
		}
		RISEPel EvalTransmittance( const Ray&, const Scalar ) const override
		{
			return RISEPel( 1, 1, 1 );
		}
		Scalar EvalTransmittanceNM(
			const Ray&, const Scalar, const Scalar ) const override { return 1; }
		bool IsHomogeneous() const override { return true; }

	protected:
		~PluginMedium() override { m_phase.release(); }

	private:
		const IPhaseFunction& m_phase;
	};

	IMedium* CreateMedium(
		const IScalarPainter& carbon,
		const IScalarPainter& temperature,
		const Point3& bboxMin,
		const Point3& bboxMax,
		const Scalar sceneUnitMeters,
		const unsigned int resolution = 8
		)
	{
		IMedium* medium = nullptr;
		const bool ok = RISE_API_CreateMultichannelHeterogeneousMedium(
			&medium, carbon, temperature,
			resolution, resolution, resolution,
			bboxMin, bboxMax, sceneUnitMeters,
			0.26, 1800.0, 0.10, 0.5,
			8.7, 1.2, 0.6, 0.6 );
		return ok ? medium : nullptr;
	}

	IMedium* CreateMediumWithCondensed(
		const IScalarPainter& carbon,
		const IScalarPainter& temperature,
		const IScalarPainter& condensed,
		const Point3& bboxMin,
		const Point3& bboxMax,
		const Scalar sceneUnitMeters,
		const unsigned int resolution = 8
		)
	{
		IMedium* medium = nullptr;
		const bool ok = RISE_API_CreateMultichannelHeterogeneousMediumWithCondensed(
			&medium, carbon, temperature, condensed,
			resolution, resolution, resolution,
			bboxMin, bboxMax, sceneUnitMeters,
			0.26, 1800.0, 0.10, 0.5,
			8.7, 1.2, 0.6, -0.4,
			4.0, 0.5, 0.9, 0.7 );
		return ok ? medium : nullptr;
	}

	IMedium* CreateMediumWithChem(
		const IScalarPainter& carbon,
		const IScalarPainter& temperature,
		const IScalarPainter& chemCH,
		const IScalarPainter& chemC2,
		const IScalarPainter& chemCO2,
		const IFunction1D& spdCH,
		const IFunction1D& spdC2,
		const IFunction1D& spdCO2,
		const Scalar intervalMin,
		const Scalar intervalMax,
		const Point3& bboxMin,
		const Point3& bboxMax,
		const Scalar sceneUnitMeters,
		const unsigned int resolution = 8
		)
	{
		IMedium* medium = nullptr;
		const bool ok = RISE_API_CreateMultichannelHeterogeneousMediumWithChem(
			&medium, carbon, temperature, nullptr,
			chemCH, chemC2, chemCO2, spdCH, spdC2, spdCO2,
			intervalMin, intervalMax, intervalMin, intervalMax,
			intervalMin, intervalMax,
			resolution, resolution, resolution, bboxMin, bboxMax,
			sceneUnitMeters, 0.26, 1800.0, 0.10, 0.5,
			0.0, 1.2, 0.0, 0.6, 0.0, 0.0, 0.0, 0.0 );
		return ok ? medium : nullptr;
	}

	Scalar SampleMeanCosine(
		const IPhaseFunction& phase,
		const unsigned int seed,
		const unsigned int sampleCount = 40000
		)
	{
		RandomNumberGenerator rng( seed );
		Implementation::IndependentSampler sampler( rng );
		const Vector3 wi( 0, 0, 1 );
		Scalar sum = 0.0;
		for( unsigned int i = 0; i < sampleCount; ++i ) {
			sum += Vector3Ops::Dot( wi, phase.Sample( wi, sampler ) );
		}
		return sum / Scalar( sampleCount );
	}

	Scalar SampleMeanSquaredCosine(
		const IPhaseFunction& phase,
		const unsigned int seed,
		const unsigned int sampleCount = 80000
		)
	{
		RandomNumberGenerator rng( seed );
		Implementation::IndependentSampler sampler( rng );
		const Vector3 wi( 0, 0, 1 );
		Scalar sum = 0.0;
		for( unsigned int i = 0; i < sampleCount; ++i ) {
			const Scalar cosine = Vector3Ops::Dot( wi, phase.Sample( wi, sampler ) );
			sum += cosine * cosine;
		}
		return sum / Scalar( sampleCount );
	}

	struct FactoryInputs
	{
		Point3 bboxMin;
		Point3 bboxMax;
		Scalar sceneUnitMeters;
		Scalar sootEm;
		Scalar sootDensity;
		Scalar sootAlbedoHot;
		Scalar sootGHot;
		Scalar smokeKmCarbon;
		Scalar smokeNCarbon;
		Scalar smokeAlbedoCarbon;
		Scalar smokeGCarbon;

		FactoryInputs() :
		  bboxMin( 0, 0, 0 ), bboxMax( 1, 1, 1 ), sceneUnitMeters( 1.0 ),
		  sootEm( 0.26 ), sootDensity( 1800.0 ), sootAlbedoHot( 0.10 ), sootGHot( 0.5 ),
		  smokeKmCarbon( 8.7 ), smokeNCarbon( 1.2 ),
		  smokeAlbedoCarbon( 0.6 ), smokeGCarbon( 0.6 )
		{
		}
	};

	bool FactoryRejects(
		const IScalarPainter& carbon,
		const IScalarPainter& temperature,
		const FactoryInputs& inputs
		)
	{
		IMedium* medium = nullptr;
		const bool created = RISE_API_CreateMultichannelHeterogeneousMedium(
			&medium, carbon, temperature, 2, 2, 2,
			inputs.bboxMin, inputs.bboxMax, inputs.sceneUnitMeters,
			inputs.sootEm, inputs.sootDensity, inputs.sootAlbedoHot, inputs.sootGHot,
			inputs.smokeKmCarbon, inputs.smokeNCarbon,
			inputs.smokeAlbedoCarbon, inputs.smokeGCarbon );
		safe_release( medium );
		return !created && !medium;
	}

	void TestMatrixOnlyFilmResponse()
	{
		std::cout << "TestMatrixOnlyFilmResponse" << std::endl;
		XYZPel xyz;
		Check( ColorUtils::XYZFromNM(xyz,500.0),
			"500 nm lies inside the film response support" );
		const RISEPel linear = ColorUtils::XYZtoRec709RGBMatrixOnly(xyz);
		const RISEPel mapped = ColorUtils::XYZtoRec709RGB(xyz);
		Check( linear.r < 0.0 && linear.g > 0.0 && linear.b > 0.0,
			"matrix-only spectral response preserves the signed 500 nm red lobe" );
		Check( !Near(linear.r,mapped.r,1e-12),
			"matrix-only response is distinct from nonlinear gamut mapping" );
		const RISEPel responseMass = ResponseMass();
		Check( responseMass.r > 1.15 && responseMass.r < 1.25 &&
			responseMass.g > 0.90 && responseMass.g < 1.00 &&
			responseMass.b > 0.85 && responseMass.b < 0.95,
			"each signed film-response mass K_c is positive and near its recorded value" );
		const RISEPel greyMean = ResponsePowerMean(0.0);
		Check( NearRelative(greyMean.r,1.0,1e-14) &&
			NearRelative(greyMean.g,1.0,1e-14) &&
			NearRelative(greyMean.b,1.0,1e-14),
			"response-weighted means divide by each positive K_c" );
	}

	void TestPredictivePresetSpectralConsumptionAndFidelity()
	{
		std::cout << "TestPredictivePresetSpectralConsumptionAndFidelity" << std::endl;
		AffineWorldScalarPainter* carbon = new AffineWorldScalarPainter(
			1.0,0.0,0.0,0.0);
		AffineWorldScalarPainter* temperature = new AffineWorldScalarPainter(
			1000.0,0.0,0.0,0.0);
		AffineWorldScalarPainter* condensed = new AffineWorldScalarPainter(
			1.0,0.0,0.0,0.0);
		IMedium* medium = 0;
		const bool created = RISE_API_CreateMultichannelHeterogeneousMediumWithPreset(
			&medium, *carbon, *temperature, condensed,
			0,0,0,0,0,0, 0.0,0.0,0.0,0.0,0.0,0.0,
			4,4,4, Point3(0,0,0), Point3(1,1,1), 1.0,
			"fire_optics_v1" );
		Check( created && medium, "the named predictive optical record constructs a medium" );
		const MultichannelHeterogeneousMedium* fire =
			dynamic_cast<const MultichannelHeterogeneousMedium*>(medium);
		if( fire ) {
			const FireOpticsPreset& optics = FireOpticsPreset::PredictiveV1();
			const MediumCoefficientsNM coefficients = fire->GetCoefficientsNM(
				Point3(0.5,0.5,0.5),550.0);
			const Scalar expectedHotAbsorption = optics.HotAbsorptionMass(550.0);
			const Scalar expectedHotExtinction = optics.HotExtinctionMass(550.0);
			const Scalar expectedCondExtinction = optics.CondensedExtinctionMass(550.0);
			const Scalar expectedScattering =
				(expectedHotExtinction-expectedHotAbsorption)+
				expectedCondExtinction*optics.CondensedAlbedo(550.0);
			Check( NearRelative(coefficients.sigma_t,
				expectedHotExtinction+expectedCondExtinction,1e-13) &&
				NearRelative(coefficients.sigma_s,expectedScattering,1e-13),
				"NM coefficients consume wavelength-dependent record MAC, omega, and condensed optics" );
			Check( NearRelative(fire->HotSootVolumeFraction(Point3(0.5,0.5,0.5)),
				5.555555555555556e-7,1e-13),
				"the medium applies the pinned 1 g/m3 soot volume-fraction unit gate" );
			const IPhaseFunction* phase = fire->MakePhaseClosure(
				Point3(0.5,0.5,0.5),550.0);
			const Scalar expectedG =
				((expectedHotExtinction-expectedHotAbsorption)*optics.HotG(550.0)+
				 expectedCondExtinction*optics.CondensedAlbedo(550.0)*
				 optics.CondensedG(550.0))/expectedScattering;
			Check( phase && Near(phase->GetMeanCosine(),expectedG,1e-12),
				"NM phase closure is sigma_s-weighted from record g spectra" );
			if( phase ) phase->release();
			Check( std::string(fire->GetFireOpticsRecordId()) == optics.RecordId(),
				"the consumed record identity is exposed through the medium" );
			Check( !fire->FirePredictiveAllowed() &&
				std::string(fire->GetFireRenderFidelityStatus(true)) == "preview" &&
				HasFireReason(*fire,true,"producer_unqualified") &&
				HasFireReason(*fire,true,"chem_none_unqualified") &&
				HasFireReason(*fire,true,"condensed_organics_ir_unclosed"),
				"predictive evaluation fails closed with specific producer, chem, and IR reasons" );
		}
		safe_release(medium);
		safe_release(carbon);
		safe_release(temperature);
		safe_release(condensed);
	}

	void TestBakedTrilinearChannelsAndOptics()
	{
		std::cout << "TestBakedTrilinearChannelsAndOptics" << std::endl;
		AffineWorldScalarPainter* carbon = new AffineWorldScalarPainter( 2.0, 1.0, 2.0, 3.0 );
		AffineWorldScalarPainter* temperature = new AffineWorldScalarPainter( 600.0, 400.0, 0.0, 0.0 );
		IMedium* medium = CreateMedium(
			*carbon, *temperature, Point3( 0, 0, 0 ), Point3( 1, 1, 1 ), 1.0 );
		Check( medium != nullptr, "factory creates the two-channel medium" );
		MultichannelHeterogeneousMedium* fire =
			dynamic_cast<MultichannelHeterogeneousMedium*>( medium );
		Check( fire != nullptr && fire->IsValid(), "factory returns the multichannel concrete type" );
		if( !fire ) {
			safe_release( medium );
			safe_release( carbon );
			safe_release( temperature );
			return;
		}

		const Point3 p( 0.4375, 0.4375, 0.4375 );
		const Scalar expectedCarbon = 4.625;
		const Scalar expectedTemperature = 775.0;
		const Scalar smoothArg = (expectedTemperature - 700.0) / 200.0;
		const Scalar expectedPhi = smoothArg * smoothArg * (3.0 - 2.0 * smoothArg);
		Check( Near( fire->LookupCarbon( p ), expectedCarbon, 1e-12 ),
			"trilinear bake reproduces an affine carbon field between voxels" );
		Check( Near( fire->LookupTemperature( p ), expectedTemperature, 1e-12 ),
			"temperature shares the same trilinear lattice" );
		Check( Near( fire->HotOpticsFraction( p ), expectedPhi, 1e-12 ),
			"phi(T) is the pinned 700-900 K smoothstep" );

		const Scalar hotAbsorptionMass =
			6.0 * PI * 0.26 * 1.0e-3 / (633.0e-9 * 1800.0);
		const Scalar hotAbsorption = expectedCarbon * expectedPhi * hotAbsorptionMass;
		const Scalar hotScattering = hotAbsorption * 0.10 / 0.90;
		const Scalar coolExtinction = expectedCarbon * (1.0 - expectedPhi) * 8.7;
		const Scalar coolScattering = coolExtinction * 0.6;
		const MediumCoefficients c = fire->GetCoefficients( p );
		const RISEPel hotMean = ResponsePowerMean(1.0);
		const RISEPel coolMean = ResponsePowerMean(1.2);
		const RISEPel expectedPelSigmaT = hotMean*(hotAbsorption+hotScattering) +
			coolMean*coolExtinction;
		const RISEPel expectedPelSigmaS = hotMean*hotScattering +
			coolMean*coolScattering;
		Check( NearRelative(c.sigma_t[0],expectedPelSigmaT[0],1e-13) &&
			NearRelative(c.sigma_t[1],expectedPelSigmaT[1],1e-13) &&
			NearRelative(c.sigma_t[2],expectedPelSigmaT[2],1e-13),
			"Pel extinction is the signed-response weighted mean of spectral extinction" );
		Check( NearRelative(c.sigma_s[0],expectedPelSigmaS[0],1e-13) &&
			NearRelative(c.sigma_s[1],expectedPelSigmaS[1],1e-13) &&
			NearRelative(c.sigma_s[2],expectedPelSigmaS[2],1e-13),
			"Pel scattering is projected independently from absorption" );
		Check( ColorMath::MinValue(c.sigma_t) >= 0.0 &&
			ColorMath::MinValue(c.sigma_s) >= 0.0,
			"admitted broadband optics satisfy the nonnegative projected-coefficient invariant" );
		const Scalar hotScale500 = 633.0 / 500.0;
		const Scalar coolScale500 = std::pow( hotScale500, 1.2 );
		const Scalar expectedSigmaS500 =
			hotScattering * hotScale500 + coolScattering * coolScale500;
		const Scalar expectedSigmaT500 =
			(hotAbsorption + hotScattering) * hotScale500 +
			coolExtinction * coolScale500;
		const MediumCoefficientsNM cnm = fire->GetCoefficientsNM( p, 500.0 );
		Check( Near( cnm.sigma_t, expectedSigmaT500, 1e-11 ) &&
			Near( cnm.sigma_s, expectedSigmaS500, 1e-11 ),
			"NM coefficients apply the pinned hot 1/lambda and cool lambda^-n laws" );

		carbon->bias = 200.0;
		temperature->bias = 2000.0;
		Check( Near( fire->LookupCarbon( p ), expectedCarbon, 1e-12 ) &&
			Near( fire->LookupTemperature( p ), expectedTemperature, 1e-12 ),
			"channel painters are baked once rather than sampled during rendering" );

		safe_release( medium );
		safe_release( carbon );
		safe_release( temperature );
	}

	void TestChromaticNMTrackingAndTransmittance()
	{
		std::cout << "TestChromaticNMTrackingAndTransmittance" << std::endl;
		IScalarPainter* carbon = nullptr;
		IScalarPainter* hotTemperature = nullptr;
		IScalarPainter* coolTemperature = nullptr;
		RISE_API_CreateUniformScalarPainter( &carbon, 0.2 );
		RISE_API_CreateUniformScalarPainter( &hotTemperature, 1000.0 );
		RISE_API_CreateUniformScalarPainter( &coolTemperature, 600.0 );
		IMedium* hot = CreateMedium( *carbon, *hotTemperature,
			Point3( 0, 0, 0 ), Point3( 1, 1, 1 ), 1.0 );
		IMedium* cool = CreateMedium( *carbon, *coolTemperature,
			Point3( 0, 0, 0 ), Point3( 1, 1, 1 ), 1.0 );
		Check( hot && cool, "uniform hot and cool chromatic fixtures construct" );
		if( hot && cool ) {
			const Point3 midpoint( 0.5, 0.5, 0.5 );
			const MultichannelHeterogeneousMedium* hotFire =
				dynamic_cast<const MultichannelHeterogeneousMedium*>( hot );
			const MediumCoefficientsNM hot500 = hot->GetCoefficientsNM( midpoint, 500.0 );
			const MediumCoefficientsNM hot700 = hot->GetCoefficientsNM( midpoint, 700.0 );
			Check( NearRelative( hot500.sigma_t / hot700.sigma_t, 1.4, 1e-13 ) &&
				NearRelative( hot500.sigma_s / hot700.sigma_s, 1.4, 1e-13 ),
				"hot-soot absorption and scattering both follow 1/lambda" );

			const MediumCoefficientsNM cool500 = cool->GetCoefficientsNM( midpoint, 500.0 );
			const MediumCoefficientsNM cool700 = cool->GetCoefficientsNM( midpoint, 700.0 );
			const Scalar coolRatio = std::pow( 700.0 / 500.0, 1.2 );
			Check( NearRelative( cool500.sigma_t / cool700.sigma_t, coolRatio, 1e-13 ) &&
				NearRelative( cool500.sigma_s / cool500.sigma_t, 0.6, 1e-13 ),
				"cool-smoke extinction follows lambda^-n with its authored albedo split" );
			if( hotFire ) {
				const Scalar majorant380 = hotFire->TrackingMajorantAtNM( midpoint, 380.0 );
				const Scalar majorant500 = hotFire->TrackingMajorantAtNM( midpoint, 500.0 );
				const Scalar majorant780 = hotFire->TrackingMajorantAtNM( midpoint, 780.0 );
				Check( majorant380 >= hot->GetCoefficientsNM( midpoint, 380.0 ).sigma_t &&
					majorant500 >= hot500.sigma_t &&
					majorant780 >= hot->GetCoefficientsNM( midpoint, 780.0 ).sigma_t,
					"spectral tracking majorant bounds extinction across the visible interval" );
				Check( majorant380 == majorant500 && majorant500 == majorant780,
					"Phase-A NM tracking retains the locked max-over-lambda majorant" );
			}

			const Ray ray( Point3( 0.5, 0.5, 0.125 ), Vector3( 0, 0, 1 ) );
			const Scalar length = 0.5;
			const Scalar expectedT500 = std::exp( -hot500.sigma_t * length );
			const Scalar expectedT700 = std::exp( -hot700.sigma_t * length );
			Check( NearRelative( hot->EvalDistancePdfNM(
				ray, length, false, length, 500.0 ), expectedT500, 1e-12 ) &&
				NearRelative( hot->EvalDistancePdfNM(
				ray, length, false, length, 700.0 ), expectedT700, 1e-12 ),
				"deterministic NM distance survival uses chromatic optical depth" );

			const unsigned int samples = 30000;
			RandomNumberGenerator rng500( 0x500u );
			RandomNumberGenerator rng700( 0x700u );
			Implementation::IndependentSampler sampler500( rng500 );
			Implementation::IndependentSampler sampler700( rng700 );
			unsigned int events500 = 0;
			unsigned int events700 = 0;
			Scalar transmittance500 = 0.0;
			Scalar transmittance700 = 0.0;
			for( unsigned int i = 0; i < samples; ++i ) {
				bool scattered500 = false;
				bool scattered700 = false;
				hot->SampleDistanceNM( ray, length, 500.0, sampler500, scattered500 );
				hot->SampleDistanceNM( ray, length, 700.0, sampler700, scattered700 );
				if( scattered500 ) ++events500;
				if( scattered700 ) ++events700;
				transmittance500 += hot->EvalTransmittanceNM( ray, length, 500.0 );
				transmittance700 += hot->EvalTransmittanceNM( ray, length, 700.0 );
			}
			const Scalar eventRate500 = Scalar(events500) / Scalar(samples);
			const Scalar eventRate700 = Scalar(events700) / Scalar(samples);
			Check( Near( eventRate500, 1.0 - expectedT500, 0.012 ) &&
				Near( eventRate700, 1.0 - expectedT700, 0.012 ),
				"delta tracking samples each wavelength's extinction law" );
			Check( Near( transmittance500 / Scalar(samples), expectedT500, 0.012 ) &&
				Near( transmittance700 / Scalar(samples), expectedT700, 0.012 ),
				"ratio tracking estimates chromatic NM transmittance" );
		}

		safe_release( hot );
		safe_release( cool );
		safe_release( carbon );
		safe_release( hotTemperature );
		safe_release( coolTemperature );
	}

	void TestCondensedConstituentOpticsAndClosure()
	{
		std::cout << "TestCondensedConstituentOpticsAndClosure" << std::endl;
		AffineWorldScalarPainter* carbon =
			new AffineWorldScalarPainter( 1.0, 0.0, 0.0, 0.0 );
		AffineWorldScalarPainter* temperature =
			new AffineWorldScalarPainter( 800.0, 0.0, 0.0, 0.0 );
		AffineWorldScalarPainter* condensed =
			new AffineWorldScalarPainter( 2.0, 1.0, 0.0, 0.0 );
		IMedium* medium = CreateMediumWithCondensed(
			*carbon, *temperature, *condensed,
			Point3( 0, 0, 0 ), Point3( 1, 1, 1 ), 1.0 );
		MultichannelHeterogeneousMedium* fire =
			dynamic_cast<MultichannelHeterogeneousMedium*>( medium );
		Check( fire && fire->IsValid(), "three-channel fire medium constructs" );
		if( fire ) {
			const Point3 p( 0.4375, 0.4375, 0.4375 );
			const Scalar expectedCondensed = 2.4375;
			Check( Near( fire->LookupCondensed( p ), expectedCondensed, 1e-12 ),
				"condensed channel is baked on the shared trilinear lattice" );

			const Scalar nm = 500.0;
			const Scalar wavelengthScale = 633.0 / nm;
			const Scalar hotAbsorptionMass =
				6.0 * PI * 0.26 * 1.0e-3 / (633.0e-9 * 1800.0);
			const Scalar hotAbsorption = 0.5 * hotAbsorptionMass * wavelengthScale;
			const Scalar hotScattering = hotAbsorption * 0.10 / 0.90;
			const Scalar coolExtinction = 0.5 * 8.7 * std::pow( wavelengthScale, 1.2 );
			const Scalar coolScattering = coolExtinction * 0.6;
			const Scalar condExtinction = expectedCondensed * 4.0 *
				std::pow( wavelengthScale, 0.5 );
			const Scalar condScattering = condExtinction * 0.9;
			const MediumCoefficientsNM coeff = fire->GetCoefficientsNM( p, nm );
			Check( NearRelative( coeff.sigma_t,
				hotAbsorption + hotScattering + coolExtinction + condExtinction, 1e-13 ) &&
				NearRelative( coeff.sigma_s,
					hotScattering + coolScattering + condScattering, 1e-13 ),
				"summed spectral optics include condensed extinction and albedo split" );

			const Scalar expectedMean =
				(hotScattering * 0.5 + coolScattering * -0.4 + condScattering * 0.7) /
				(hotScattering + coolScattering + condScattering);
			const Scalar totalScattering =
				hotScattering + coolScattering + condScattering;
			const Scalar hotWeight = hotScattering / totalScattering;
			const Scalar coolWeight = coolScattering / totalScattering;
			const Scalar condWeight = condScattering / totalScattering;
			const IPhaseFunction* closure = fire->MakePhaseClosure( p, nm );
			Check( closure && NearRelative( closure->GetMeanCosine(), expectedMean, 1e-13 ),
				"phase closure is the sigma_s-weighted three-constituent HG mixture" );
			if( closure ) {
				Check( std::fabs( SampleMeanCosine( *closure, 0xc0ddu ) - expectedMean ) < 0.015,
					"condensed g affects sampled continuation directions" );
				const Scalar expectedSecondMoment =
					hotWeight * (1.0 + 2.0 * 0.5 * 0.5) / 3.0 +
					coolWeight * (1.0 + 2.0 * -0.4 * -0.4) / 3.0 +
					condWeight * (1.0 + 2.0 * 0.7 * 0.7) / 3.0;
				Check( std::fabs( SampleMeanSquaredCosine(
					*closure, 0x5ec0du ) - expectedSecondMoment ) < 0.01,
					"closure sampling preserves the HG-mixture second moment" );

				const Scalar cosine = 0.25;
				const Vector3 wi( 0, 0, 1 );
				const Vector3 wo( std::sqrt( 1.0 - cosine*cosine ), 0, cosine );
				const Scalar expectedPhase =
					hotWeight * HenyeyGreensteinPhaseFunction::EvaluateWithG( cosine, 0.5 ) +
					coolWeight * HenyeyGreensteinPhaseFunction::EvaluateWithG( cosine, -0.4 ) +
					condWeight * HenyeyGreensteinPhaseFunction::EvaluateWithG( cosine, 0.7 );
				Check( NearRelative( closure->Evaluate( wi, wo ), expectedPhase, 1e-13 ) &&
					NearRelative( closure->Pdf( wi, wo ), expectedPhase, 1e-13 ),
					"closure Evaluate and Pdf retain the explicit three-lobe mixture" );
			}
			safe_release( closure );

			const Scalar hotScattering633 = 0.5*hotAbsorptionMass*0.10/0.90;
			const Scalar coolScattering633 = 0.5*8.7*0.6;
			const Scalar condScattering633 = expectedCondensed*4.0*0.9;
			const RISEPel hotProjected = ResponsePowerMean(1.0)*hotScattering633;
			const RISEPel coolProjected = ResponsePowerMean(1.2)*coolScattering633;
			const RISEPel condProjected = ResponsePowerMean(0.5)*condScattering633;
			const Scalar hotProposal = hotScattering633*SamplingPowerMass(1.0);
			const Scalar coolProposal = coolScattering633*SamplingPowerMass(1.2);
			const Scalar condProposal = condScattering633*SamplingPowerMass(0.5);
			const Scalar proposalTotal = hotProposal+coolProposal+condProposal;
			const Scalar expectedPelMean =
				(hotProposal*0.5+coolProposal*-0.4+condProposal*0.7)/proposalTotal;
			const IPhaseFunction* pelClosure = fire->MakePhaseClosurePel(p);
			Check( pelClosure && NearRelative(
				pelClosure->GetMeanCosine(),expectedPelMean,1e-13),
				"Pel closure guiding mean uses the nonnegative CMF-sum proposal mixture" );
			if( pelClosure ) {
				const Scalar cosine = 0.25;
				const Vector3 wi(0,0,1);
				const Vector3 wo(std::sqrt(1.0-cosine*cosine),0,cosine);
				const Scalar pHot = HenyeyGreensteinPhaseFunction::EvaluateWithG(cosine,0.5);
				const Scalar pCool = HenyeyGreensteinPhaseFunction::EvaluateWithG(cosine,-0.4);
				const Scalar pCond = HenyeyGreensteinPhaseFunction::EvaluateWithG(cosine,0.7);
				RISEPel expectedPel(0.0);
				for( unsigned int channel = 0; channel < 3u; ++channel ) {
					const Scalar total = hotProjected[channel]+coolProjected[channel]+
						condProjected[channel];
					expectedPel[channel] = (hotProjected[channel]*pHot+
						coolProjected[channel]*pCool+condProjected[channel]*pCond)/total;
				}
				const RISEPel actualPel = pelClosure->EvaluatePel(wi,wo);
				const Scalar expectedProposal = (hotProposal*pHot+coolProposal*pCool+
					condProposal*pCond)/proposalTotal;
				Check( NearRelative(actualPel.r,expectedPel.r,1e-13) &&
					NearRelative(actualPel.g,expectedPel.g,1e-13) &&
					NearRelative(actualPel.b,expectedPel.b,1e-13),
					"Pel closure Evaluate retains signed per-channel projected lobe weights" );
				Check( NearRelative(pelClosure->PdfProposal(wi,wo),expectedProposal,1e-13) &&
					NearRelative(pelClosure->Pdf(wi,wo),expectedProposal,1e-13),
					"Pel closure samples and reports the same nonnegative proposal density" );
				Check( std::fabs(SampleMeanCosine(*pelClosure,0x9e1u)-expectedPelMean)<0.015,
					"Pel proposal mixture controls sampled directions" );
			}
			safe_release(pelClosure);

			const Scalar majorant = fire->TrackingMajorantAtNM( p, 500.0 );
			Check( majorant >= fire->GetCoefficientsNM( p, 380.0 ).sigma_t &&
				majorant >= coeff.sigma_t &&
				majorant >= fire->GetCoefficientsNM( p, 780.0 ).sigma_t,
				"shared extinction majorant bounds the summed carbon and condensed optics" );

			condensed->bias = 20.0;
			Check( Near( fire->LookupCondensed( p ), expectedCondensed, 1e-12 ),
				"condensed painter is baked once rather than read during transport" );
		}

		safe_release( medium );
		safe_release( condensed );
		safe_release( temperature );
		safe_release( carbon );

		IScalarPainter* zeroCarbon = nullptr;
		IScalarPainter* hotTemperature = nullptr;
		IScalarPainter* pureCondensed = nullptr;
		RISE_API_CreateUniformScalarPainter( &zeroCarbon, 0.0 );
		RISE_API_CreateUniformScalarPainter( &hotTemperature, 1200.0 );
		RISE_API_CreateUniformScalarPainter( &pureCondensed, 1.0 );
		IMedium* condensedOnly = CreateMediumWithCondensed(
			*zeroCarbon, *hotTemperature, *pureCondensed,
			Point3( 0, 0, 0 ), Point3( 1, 1, 1 ), 1.0, 4 );
		const MultichannelHeterogeneousMedium* condensedFire =
			dynamic_cast<const MultichannelHeterogeneousMedium*>( condensedOnly );
		const Scalar emissionNM = 550.0;
		const Scalar condensedExtinction = 4.0 * std::pow( 633.0 / emissionNM, 0.5 );
		const Scalar expectedEmission = condensedExtinction * (1.0 - 0.9) *
			PlanckSpectralRadianceNM( emissionNM, 1200.0 );
		Check( condensedFire && NearRelative( condensedFire->GetThermalEmissionNM(
			Point3( 0.5, 0.5, 0.5 ), emissionNM ), expectedEmission, 1e-13 ) &&
			condensedFire->GetThermalEmissionImportance() > 0.0,
			"pure-condensed thermal emission is sigma_a times Planck radiance with CDF support" );
		safe_release( condensedOnly );
		safe_release( pureCondensed );
		safe_release( hotTemperature );
		safe_release( zeroCarbon );

		IScalarPainter* equalCarbon = nullptr;
		IScalarPainter* equalTemperature = nullptr;
		IScalarPainter* equalCondensed = nullptr;
		RISE_API_CreateUniformScalarPainter( &equalCarbon, 1.0 );
		RISE_API_CreateUniformScalarPainter( &equalTemperature, 800.0 );
		RISE_API_CreateUniformScalarPainter( &equalCondensed, 1.0 );
		const Scalar equalExtinctionMass = 8.7;
		const Scalar equalHotAlbedo = 0.1;
		const Scalar equalSootDensity = 1800.0;
		const Scalar equalSootEm = equalExtinctionMass * (1.0 - equalHotAlbedo) *
			633.0e-9 * equalSootDensity / (6.0 * PI * 1.0e-3);
		IMedium* equalMedium = nullptr;
		const bool equalCreated =
			RISE_API_CreateMultichannelHeterogeneousMediumWithCondensed(
				&equalMedium, *equalCarbon, *equalTemperature, *equalCondensed,
				4, 4, 4, Point3( 0, 0, 0 ), Point3( 1, 1, 1 ), 1.0,
				equalSootEm, equalSootDensity, equalHotAlbedo, 0.5,
				equalExtinctionMass, 1.2, 0.6, -0.4,
				equalExtinctionMass, 0.5, 0.9, 0.7 );
		const MultichannelHeterogeneousMedium* equalFire =
			dynamic_cast<const MultichannelHeterogeneousMedium*>( equalMedium );
		bool equalMajorantBoundsSupport = equalCreated && equalFire;
		if( equalFire ) {
			const Scalar positions[] = { 0.125, 0.5, 0.875 };
			const Scalar wavelengths[] = { 380.0, 500.0, 633.0, 780.0 };
			for( const Scalar x : positions ) {
				const Point3 samplePoint( x, 1.0-x, 0.5 );
				for( const Scalar wavelength : wavelengths ) {
					equalMajorantBoundsSupport = equalMajorantBoundsSupport &&
						equalFire->TrackingMajorantAtNM( samplePoint, wavelength ) >=
							equalFire->GetCoefficientsNM( samplePoint, wavelength ).sigma_t;
				}
			}
		}
		Check( equalMajorantBoundsSupport,
			"majorant bounds equal carbon-plus-condensed extinction over space and wavelength" );
		safe_release( equalMedium );
		safe_release( equalCondensed );
		safe_release( equalTemperature );
		safe_release( equalCarbon );
	}

	void TestWavelengthBoundConstituentPhaseClosure()
	{
		std::cout << "TestWavelengthBoundConstituentPhaseClosure" << std::endl;
		const Scalar hotG = 0.85;
		const Scalar coolG = -0.35;
		const Scalar hotAlbedo = 0.40;
		const Scalar coolAlbedo = 0.70;
		AffineWorldScalarPainter* carbon =
			new AffineWorldScalarPainter( 1.0, 0.0, 0.0, 0.0 );
		AffineWorldScalarPainter* temperature =
			new AffineWorldScalarPainter( 600.0, 400.0, 0.0, 0.0 );
		IMedium* medium = nullptr;
		const bool created = RISE_API_CreateMultichannelHeterogeneousMedium(
			&medium, *carbon, *temperature, 8, 8, 8,
			Point3( 0, 0, 0 ), Point3( 1, 1, 1 ), 1.0,
			0.26, 1800.0, hotAlbedo, hotG,
			8.7, 1.2, coolAlbedo, coolG );
		Check( created && medium, "phase-closure fixture constructs" );
		if( !medium ) {
			safe_release( carbon );
			safe_release( temperature );
			return;
		}

		Check( medium->GetPhaseFunction() == nullptr,
			"fire medium exposes no legacy fixed phase function" );
		Check( MediumTransport::IsContinuationPhaseClosureNMPreflightAllowlisted( *medium ),
			"exact multichannel fire medium occupies the continuation-closure allowlist row" );
		{
			IPhaseFunction* legacyPhase = nullptr;
			RISE_API_CreateHenyeyGreensteinPhaseFunction( &legacyPhase, 0.0 );
			DerivedMultichannelHeterogeneousMedium derived(
				*carbon, *temperature, *legacyPhase );
			Check( !MediumTransport::IsContinuationPhaseClosureNMPreflightAllowlisted( derived ),
				"derived multichannel fire medium is default-denied by the exact-type table" );
			safe_release( legacyPhase );
		}

		const Point3 coolPoint( 0.125, 0.5, 0.5 );
		const Point3 hotPoint( 0.875, 0.5, 0.5 );
		const IPhaseFunction* pelClosure =
			medium->MakeContinuationPhaseClosurePel(coolPoint);
		Check( pelClosure != nullptr && Near(pelClosure->GetMeanCosine(),coolG,1e-12),
			"step-7 Pel continuation binds the local cool-constituent lobe" );
		if( pelClosure ) {
			const Vector3 wi(0,0,1);
			const Vector3 wo = Vector3Ops::Normalize(Vector3(0.6,0,0.8));
			const RISEPel value = pelClosure->EvaluatePel(wi,wo);
			const Scalar expected =
				HenyeyGreensteinPhaseFunction::EvaluateWithG(0.8,coolG);
			Check( NearRelative(value.r,expected,1e-13) &&
				NearRelative(value.g,expected,1e-13) &&
				NearRelative(value.b,expected,1e-13) &&
				NearRelative(pelClosure->PdfProposal(wi,wo),expected,1e-13),
				"single-constituent Pel Evaluate and proposal density reduce to the same HG lobe" );
		}
		safe_release(pelClosure);
		const Scalar wavelengths[] = { 450.0, 750.0 };
		for( const Scalar nm : wavelengths )
		{
			const IPhaseFunction* coolClosure = medium->MakePhaseClosure( coolPoint, nm );
			const IPhaseFunction* hotClosure = medium->MakePhaseClosure( hotPoint, nm );
			const IPhaseFunction* coolContinuation =
				medium->MakeContinuationPhaseClosureNM( coolPoint, nm );
			const IPhaseFunction* hotContinuation =
				medium->MakeContinuationPhaseClosureNM( hotPoint, nm );
			Check( coolClosure && hotClosure,
				"two-position/two-wavelength closures construct" );
			Check( coolContinuation && hotContinuation && coolClosure && hotClosure &&
				Near(coolContinuation->GetMeanCosine(),
					coolClosure->GetMeanCosine(),1e-15) &&
				Near(hotContinuation->GetMeanCosine(),
					hotClosure->GetMeanCosine(),1e-15),
				"fire continuation factory preserves both local constituent mixtures" );
			if( coolClosure && hotClosure )
			{
				Check( Near( coolClosure->GetMeanCosine(), coolG, 1e-12 ) &&
					Near( hotClosure->GetMeanCosine(), hotG, 1e-12 ),
					"opposite constituent positions bind their authored g values" );
				const Vector3 wi( 0, 0, 1 );
				const Vector3 wo = Vector3Ops::Normalize( Vector3( 0.6, 0, 0.8 ) );
				const Scalar expectedCool =
					HenyeyGreensteinPhaseFunction::EvaluateWithG( 0.8, coolG );
				const Scalar expectedHot =
					HenyeyGreensteinPhaseFunction::EvaluateWithG( 0.8, hotG );
				Check( NearRelative( coolClosure->Evaluate( wi, wo ), expectedCool, 1e-13 ) &&
					NearRelative( hotClosure->Evaluate( wi, wo ), expectedHot, 1e-13 ) &&
					NearRelative( coolClosure->Pdf( wi, wo ), expectedCool, 1e-13 ) &&
					NearRelative( hotClosure->Pdf( wi, wo ), expectedHot, 1e-13 ),
					"closure Evaluate/Pdf are the sigma_s-weighted HG law" );
			}
			if( coolContinuation && hotContinuation && coolClosure && hotClosure )
			{
				const Vector3 wi(0,0,1);
				const Vector3 wo = Vector3Ops::Normalize(Vector3(0.6,0,0.8));
				Check( NearRelative(coolContinuation->Evaluate(wi,wo),
					coolClosure->Evaluate(wi,wo),1e-15) &&
					NearRelative(coolContinuation->Pdf(wi,wo),
						coolClosure->Pdf(wi,wo),1e-15) &&
					NearRelative(hotContinuation->Evaluate(wi,wo),
						hotClosure->Evaluate(wi,wo),1e-15) &&
					NearRelative(hotContinuation->Pdf(wi,wo),
						hotClosure->Pdf(wi,wo),1e-15),
					"fire continuation Evaluate/Pdf match the retained local phase closures" );
			}
			safe_release( coolClosure );
			safe_release( hotClosure );
			safe_release( coolContinuation );
			safe_release( hotContinuation );
		}

		const Point3 mixedPoint( 0.5, 0.5, 0.5 );
		const Scalar mixedWavelengths[] = { 450.0, 750.0 };
		Scalar measuredMeans[2] = { 0.0, 0.0 };
		for( unsigned int i = 0; i < 2; ++i )
		{
			const Scalar nm = mixedWavelengths[i];
			const IPhaseFunction* closure =
				medium->MakeContinuationPhaseClosureNM( mixedPoint, nm );
			Check( closure != nullptr, "mixed-constituent closure constructs" );
			if( closure )
			{
				const Scalar scale = 633.0 / nm;
				const Scalar hotAbsorptionMass =
					6.0 * PI * 0.26 * 1.0e-3 / (633.0e-9 * 1800.0);
				const Scalar hotS = 0.5 * hotAbsorptionMass * scale *
					hotAlbedo / (1.0 - hotAlbedo);
				const Scalar coolS = 0.5 * 8.7 * std::pow( scale, 1.2 ) * coolAlbedo;
				const Scalar expectedMean = (hotS * hotG + coolS * coolG) / (hotS + coolS);
				measuredMeans[i] = closure->GetMeanCosine();
				Check( NearRelative( measuredMeans[i], expectedMean, 1e-13 ),
					"mixed closure mean cosine uses local wavelength-dependent sigma_s weights" );

				const Vector3 wi( 0, 0, 1 );
				const Vector3 wo = Vector3Ops::Normalize( Vector3( 0.6, 0, 0.8 ) );
				RayIntersectionGeometric ri( Ray( mixedPoint, wi ), RasterizerState{ 0, 0 } );
				MediumTransport::MediumScatterBSDF adapterBSDF( closure, wi );
				MediumTransport::MediumScatterMaterial adapterMaterial( closure, wi );
				const Scalar expected = closure->Evaluate( wo, wi );
				const IORStack ior( 1.0 );
				Check( NearRelative( adapterBSDF.valueNM( wo, ri, nm ), expected, 1e-13 ) &&
					NearRelative( adapterMaterial.PdfNM( wo, ri, nm, ior ),
						closure->Pdf( wo, wi ), 1e-13 ),
					"NEE evaluation and Pdf adapters consume the retained closure instance" );
			}
			safe_release( closure );
		}
		Check( std::fabs( measuredMeans[0] - measuredMeans[1] ) > 1e-4,
			"one position binds distinct phase closures at distinct wavelengths" );

		const IPhaseFunction* coolClosure =
			medium->MakeContinuationPhaseClosureNM( coolPoint, 500.0 );
		const IPhaseFunction* hotClosure =
			medium->MakeContinuationPhaseClosureNM( hotPoint, 500.0 );
		if( coolClosure && hotClosure )
		{
			const Scalar sampledCool = SampleMeanCosine( *coolClosure, 0xc001u );
			const Scalar sampledHot = SampleMeanCosine( *hotClosure, 0x807u );
			Check( std::fabs( sampledCool - coolG ) < 0.015 &&
				std::fabs( sampledHot - hotG ) < 0.015 && sampledHot - sampledCool > 1.0,
				"stored constituent g values materially change sampled directions" );
		}
		safe_release( coolClosure );
		safe_release( hotClosure );

		safe_release( medium );
		safe_release( carbon );
		safe_release( temperature );
	}

	void TestContinuationPhaseClosurePreflightTable()
	{
		std::cout << "TestContinuationPhaseClosurePreflightTable" << std::endl;
		IPhaseFunction* isotropic = nullptr;
		RISE_API_CreateIsotropicPhaseFunction( &isotropic );
		HomogeneousMedium* exactHomogeneous = new HomogeneousMedium(
			RISEPel( 0, 0, 0 ), RISEPel( 1, 1, 1 ), *isotropic );
		Check( MediumTransport::IsContinuationPhaseClosureNMPreflightAllowlisted(
			*exactHomogeneous ),
			"exact homogeneous medium with exact isotropic phase is allowlisted" );

		DerivedHomogeneousMedium* derived = new DerivedHomogeneousMedium( *isotropic );
		Check( !MediumTransport::IsContinuationPhaseClosureNMPreflightAllowlisted( *derived ),
			"derived built-in medium is default-denied" );

		PluginMedium* pluginMedium = new PluginMedium( *isotropic );
		Check( !MediumTransport::IsContinuationPhaseClosureNMPreflightAllowlisted(
			*pluginMedium ),
			"plugin medium is default-denied" );

		PluginPhase* pluginPhase = new PluginPhase();
		HomogeneousMedium* pluginPhaseMedium = new HomogeneousMedium(
			RISEPel( 0, 0, 0 ), RISEPel( 1, 1, 1 ), *pluginPhase );
		Check( !MediumTransport::IsContinuationPhaseClosureNMPreflightAllowlisted(
			*pluginPhaseMedium ),
			"plugin phase on an exact built-in medium is default-denied" );

		HenyeyGreensteinPhaseFunction* invalidHG =
			new HenyeyGreensteinPhaseFunction( 1.0 );
		HomogeneousMedium* invalidHGMedium = new HomogeneousMedium(
			RISEPel( 0, 0, 0 ), RISEPel( 1, 1, 1 ), *invalidHG );
		Check( !MediumTransport::IsContinuationPhaseClosureNMPreflightAllowlisted(
			*invalidHGMedium ),
			"finite-density HG qualification rejects g at the singular endpoint" );

		AffineWorldScalarPainter* carbon =
			new AffineWorldScalarPainter( 1.0, 0.0, 0.0, 0.0 );
		AffineWorldScalarPainter* temperature =
			new AffineWorldScalarPainter( 1000.0, 0.0, 0.0, 0.0 );
		MultichannelHeterogeneousMedium* invalidFire =
			new MultichannelHeterogeneousMedium(
				*carbon, *temperature, 2, 2, 2,
				Point3( 0, 0, 0 ), Point3( 1, 1, 1 ), 1.0,
				0.26, 1800.0, 0.4, 1.0, 8.7, 1.2, 0.7, -0.35,
				*isotropic );
		Check( !invalidFire->IsValid() &&
			!MediumTransport::IsContinuationPhaseClosureNMPreflightAllowlisted(
				*invalidFire ),
			"invalid exact fire instance cannot pass continuation preflight" );

		safe_release( invalidFire );
		safe_release( temperature );
		safe_release( carbon );
		safe_release( invalidHGMedium );
		safe_release( invalidHG );
		safe_release( pluginPhaseMedium );
		safe_release( pluginPhase );
		safe_release( pluginMedium );
		safe_release( derived );
		safe_release( exactHomogeneous );
		safe_release( isotropic );
	}

	void TestPhysicalUnitsAndSceneScale()
	{
		std::cout << "TestPhysicalUnitsAndSceneScale" << std::endl;
		IScalarPainter* carbon = nullptr;
		IScalarPainter* temperature = nullptr;
		RISE_API_CreateUniformScalarPainter( &carbon, 1.0 );
		RISE_API_CreateUniformScalarPainter( &temperature, 1000.0 );
		IMedium* metres = CreateMedium(
			*carbon, *temperature, Point3( 0, 0, 0 ), Point3( 1, 1, 1 ), 1.0 );
		IMedium* centimetres = CreateMedium(
			*carbon, *temperature, Point3( 0, 0, 0 ), Point3( 100, 100, 100 ), 0.01 );
		MultichannelHeterogeneousMedium* m =
			dynamic_cast<MultichannelHeterogeneousMedium*>( metres );
		MultichannelHeterogeneousMedium* cm =
			dynamic_cast<MultichannelHeterogeneousMedium*>( centimetres );
		Check( m != nullptr && cm != nullptr, "metre and centimetre fixtures construct" );
		if( m && cm ) {
			Check( Near( m->HotSootVolumeFraction( Point3( 0.5, 0.5, 0.5 ) ),
				5.555555555555556e-7, 1e-18 ),
				"1 g/m^3 at phi=1 gives f_v=5.5556e-7" );
			const MediumCoefficients cM = m->GetCoefficients( Point3( 0.5, 0.5, 0.5 ) );
			const MediumCoefficients cCM = cm->GetCoefficients( Point3( 50, 50, 50 ) );
			Check( Near( cCM.sigma_t[0], 0.01 * cM.sigma_t[0], 1e-13 ),
				"inverse-length coefficients convert from SI to scene units" );

			const Ray rayM( Point3( 0.5, 0.5, 0.125 ), Vector3( 0, 0, 1 ) );
			const Ray rayCM( Point3( 50, 50, 12.5 ), Vector3( 0, 0, 1 ) );
			const Scalar transM = m->EvalDistancePdf( rayM, 0.5, false, 0.5 );
			const Scalar transCM = cm->EvalDistancePdf( rayCM, 50.0, false, 50.0 );
			const Scalar expectedTau = ColorMath::MaxValue(cM.sigma_t) * 0.5;
			Check( Near( -std::log( transM ), expectedTau, 1e-10 ),
				"deterministic optical depth matches the uniform slab" );
			Check( Near( transCM, transM, 1e-12 ),
				"the same physical slab is invariant between metre and centimetre scenes" );

			const MediumCoefficientsNM cM500 = m->GetCoefficientsNM(
				Point3( 0.5, 0.5, 0.5 ), 500.0 );
			const MediumCoefficientsNM cCM500 = cm->GetCoefficientsNM(
				Point3( 50, 50, 50 ), 500.0 );
			const Scalar transM500 = m->EvalDistancePdfNM(
				rayM, 0.5, false, 0.5, 500.0 );
			const Scalar transCM500 = cm->EvalDistancePdfNM(
				rayCM, 50.0, false, 50.0, 500.0 );
			Check( NearRelative( cCM500.sigma_t, 0.01 * cM500.sigma_t, 1e-13 ) &&
				NearRelative( transCM500, transM500, 1e-12 ),
				"chromatic NM coefficients and optical depth are scene-unit invariant" );
		}

		safe_release( metres );
		safe_release( centimetres );
		safe_release( carbon );
		safe_release( temperature );
	}

	void TestPhiSupMajorant()
	{
		std::cout << "TestPhiSupMajorant" << std::endl;
		// Opposing gradients make every x-cell endpoint product small while
		// carbon*k(phi(T)) is large in the interior.  A grid built from the
		// nonlinear product's knots is non-conservative; the required bound
		// is max_phi(k) times the carbon-only lattice majorant.
		AffineWorldScalarPainter* carbon = new AffineWorldScalarPainter( 1.5, -2.0, 0.0, 0.0 );
		AffineWorldScalarPainter* temperature = new AffineWorldScalarPainter( 600.0, 400.0, 0.0, 0.0 );
		const Scalar targetHotMassExtinction = 100.0;
		const Scalar sootEm = targetHotMassExtinction * 633.0e-9 * 1800.0 /
			(6.0 * PI * 1.0e-3);
		IMedium* medium = nullptr;
		const bool created = RISE_API_CreateMultichannelHeterogeneousMedium(
			&medium, *carbon, *temperature,
			2, 2, 2, Point3( 0, 0, 0 ), Point3( 1, 1, 1 ), 1.0,
			sootEm, 1800.0, 0.0, 0.5,
			1.0, 1.2, 0.0, 0.6 );
		MultichannelHeterogeneousMedium* fire =
			dynamic_cast<MultichannelHeterogeneousMedium*>( medium );
		Check( created && fire, "adversarial majorant fixture constructs" );
		if( fire ) {
			const Point3 interior( 0.5, 0.5, 0.5 );
			const RISEPel sigmaT = fire->GetCoefficients( interior ).sigma_t;
			const RISEPel hotMean = ResponsePowerMean(1.0);
			const RISEPel coolMean = ResponsePowerMean(1.2);
			const RISEPel expected = hotMean*25.0 + coolMean*0.25;
			const Scalar majorant = fire->TrackingMajorantAtPel( interior );
			Check( NearRelative(sigmaT.r,expected.r,1e-13) &&
				NearRelative(sigmaT.g,expected.g,1e-13) &&
				NearRelative(sigmaT.b,expected.b,1e-13),
				"opposing carbon/temperature gradients create the intended interior maximum" );
			Check( majorant >= ColorMath::MaxValue(sigmaT),
				"phi-sup carbon majorant bounds the nonlinear interior extinction" );
			const Scalar spectralMajorant = fire->TrackingMajorantAtNM( interior, 500.0 );
			Check( spectralMajorant >= fire->GetCoefficientsNM( interior, 380.0 ).sigma_t &&
				spectralMajorant >= fire->GetCoefficientsNM( interior, 500.0 ).sigma_t &&
				spectralMajorant >= fire->GetCoefficientsNM( interior, 780.0 ).sigma_t,
				"max-over-lambda majorant bounds an adversarial nonlinear phi mixture" );
		}
		safe_release( medium );
		safe_release( carbon );
		safe_release( temperature );
	}

	void TestPhiAwareQuadratureAndDistancePdf()
	{
		std::cout << "TestPhiAwareQuadratureAndDistancePdf" << std::endl;
		// First isolate the painter-baked lattice convention.  A quadratic
		// painter becomes piecewise linear after baking, with real knots at
		// x={0.125,0.375,0.625,0.875}.  The legacy density offset instead
		// splits at quarter boundaries and misses those polynomial changes.
		QuadraticXPainter* quadraticCarbon = new QuadraticXPainter();
		IScalarPainter* coolTemperature = nullptr;
		RISE_API_CreateUniformScalarPainter( &coolTemperature, 600.0 );
		IMedium* latticeMedium = nullptr;
		const bool latticeCreated = RISE_API_CreateMultichannelHeterogeneousMedium(
			&latticeMedium, *quadraticCarbon, *coolTemperature,
			4, 4, 4, Point3(0,0,0), Point3(1,1,1), 1.0,
			0.0, 1800.0, 0.0, 0.5,
			0.2, 0.0, 0.0, -0.4 );
		Check( latticeCreated && latticeMedium,
			"piecewise baked-lattice fixture constructs" );
		if( latticeMedium ) {
			const Scalar samples[4] = {
				0.125*0.125, 0.375*0.375, 0.625*0.625, 0.875*0.875 };
			Scalar expectedIntegral = 0.0;
			for( unsigned int i = 0u; i < 3u; ++i ) {
				expectedIntegral += 0.25*0.5*(samples[i]+samples[i+1]);
			}
			const Scalar expectedTau = 0.2*expectedIntegral;
			const Ray latticeRay( Point3(0.125,0.5,0.5), Vector3(1,0,0) );
			const Scalar survival = latticeMedium->EvalDistancePdfNM(
				latticeRay, 0.75, false, 0.75, 633.0 );
			Check( NearRelative( -log(survival), expectedTau, 2e-13 ),
				"deterministic DDA splits at the actual painter-baked trilinear knots" );
		}
		safe_release(latticeMedium);
		safe_release(quadraticCarbon);
		safe_release(coolTemperature);

		// A non-monotone, renderer-realizable polynomial needs derivative
		// partitioning to reveal both roots.  Along x=y, x*(1-y) forms a
		// hump whose endpoints are below 900 K while its interior is above;
		// endpoint sign checks alone therefore miss both 900 K crossings.
		IScalarPainter* uniformCarbon = nullptr;
		RISE_API_CreateUniformScalarPainter( &uniformCarbon, 1.0 );
		BilinearHumpTemperaturePainter* humpTemperature =
			new BilinearHumpTemperaturePainter();
		const Scalar humpHotMassExtinction = 2.0;
		const Scalar humpSootEm = humpHotMassExtinction*633.0e-9*1800.0 /
			(6.0*PI*1.0e-3);
		IMedium* humpMedium = nullptr;
		const bool humpCreated = RISE_API_CreateMultichannelHeterogeneousMedium(
			&humpMedium, *uniformCarbon, *humpTemperature,
			2, 2, 2, Point3(0,0,0), Point3(1,1,1), 1.0,
			humpSootEm, 1800.0, 0.0, 0.5,
			0.2, 0.0, 0.0, -0.4 );
		Check( humpCreated && humpMedium,
			"non-monotone phi-transition fixture constructs" );
		if( humpMedium ) {
			const Scalar invSqrtTwo = 1.0/sqrt(2.0);
			const Ray humpRay( Point3(0.25,0.25,0.5),
				Vector3(invSqrtTwo,invSqrtTwo,0.0) );
			const Scalar humpLength = 0.5*sqrt(2.0);
			const Scalar survival = humpMedium->EvalDistancePdfNM(
				humpRay, humpLength, false, humpLength, 633.0 );
			Check( NearRelative( -log(survival),
				ReferenceHumpOpticalDepth(), 2e-12 ),
				"derivative partitions expose both same-sign-endpoint 900 K roots" );
		}
		safe_release(humpMedium);
		safe_release(uniformCarbon);
		safe_release(humpTemperature);

		TrilinearProductPainter* carbon = new TrilinearProductPainter( 1.0, 8.0 );
		TrilinearProductPainter* temperature =
			new TrilinearProductPainter( 600.0, 1600.0 );
		const Scalar targetHotMassExtinction = 2.0;
		const Scalar sootEm = targetHotMassExtinction*633.0e-9*1800.0 /
			(6.0*PI*1.0e-3);
		IMedium* medium = nullptr;
		const bool created = RISE_API_CreateMultichannelHeterogeneousMedium(
			&medium, *carbon, *temperature,
			2, 2, 2, Point3(0,0,0), Point3(1,1,1), 1.0,
			sootEm, 1800.0, 0.0, 0.5,
			0.2, 0.0, 0.0, -0.4 );
		MultichannelHeterogeneousMedium* fire =
			dynamic_cast<MultichannelHeterogeneousMedium*>(medium);
		Check( created && fire && fire->IsValid(),
			"phi-transition quadrature fixture constructs" );
		if( fire ) {
			const Scalar invSqrtThree = 1.0/sqrt(3.0);
			const Ray ray( Point3(0.25,0.25,0.25),
				Vector3(invSqrtThree,invSqrtThree,invSqrtThree) );
			const Scalar length = 0.5*sqrt(3.0);
			const Scalar expectedTau = ReferencePhiAwareOpticalDepth(0.25,0.75);
			const Scalar survival = fire->EvalDistancePdfNM(
				ray, length, false, length, 633.0 );
			Check( NearRelative( -log(survival), expectedTau, 2e-12 ),
				"root-split 7-point optical depth integrates a degree-12 phi transition" );

			const Scalar scatterDistance = 0.61*length;
			const Scalar scatterX = 0.25 + scatterDistance*invSqrtThree;
			const Scalar expectedScatterPdf = ReferencePhiAwareSigmaT(scatterX)*
				exp( -ReferencePhiAwareOpticalDepth(0.25,scatterX) );
			const Scalar scatterPdf = fire->EvalDistancePdfNM(
				ray, scatterDistance, true, length, 633.0 );
			Check( NearRelative( scatterPdf, expectedScatterPdf, 2e-12 ),
				"EvalDistancePdfNM retains the deterministic pure-DT density through both clamps" );

			bool majorantBounded = true;
			for( unsigned int i = 0u; i <= 256u; ++i ) {
				const Scalar x = 0.25 + 0.5*Scalar(i)/256.0;
				const Point3 point(x,x,x);
				if( fire->TrackingMajorantAtNM(point,633.0) <
					fire->GetCoefficientsNM(point,633.0).sigma_t ) {
					majorantBounded = false;
					break;
				}
			}
			Check( majorantBounded,
				"shared-lattice majorant bounds every sampled point across both phi transitions" );
		}
		safe_release(medium);
		safe_release(carbon);
		safe_release(temperature);
	}

	void TestChemSPDNormalizationClippingAndSceneScale()
	{
		std::cout << "TestChemSPDNormalizationClippingAndSceneScale" << std::endl;
		AffineWorldScalarPainter* carbon =
			new AffineWorldScalarPainter( 0.0, 0.0, 0.0, 0.0 );
		AffineWorldScalarPainter* temperature =
			new AffineWorldScalarPainter( 1000.0, 0.0, 0.0, 0.0 );
		AffineWorldScalarPainter* chemCH =
			new AffineWorldScalarPainter( 120.0, 0.0, 0.0, 0.0 );
		AffineWorldScalarPainter* chemC2 =
			new AffineWorldScalarPainter( 50.0, 0.0, 0.0, 0.0 );
		AffineWorldScalarPainter* chemZero =
			new AffineWorldScalarPainter( 0.0, 0.0, 0.0, 0.0 );
		IntervalTopHatFunction* tenNanometre =
			new IntervalTopHatFunction( 500.0, 510.0 );

		IMedium* medium = CreateMediumWithChem(
			*carbon, *temperature, *chemCH, *chemZero, *chemZero,
			*tenNanometre, *tenNanometre, *tenNanometre,
			500.0, 510.0, Point3(0,0,0), Point3(1,1,1), 1.0 );
		const MultichannelHeterogeneousMedium* fire =
			dynamic_cast<const MultichannelHeterogeneousMedium*>(medium);
		Check( fire != nullptr, "chem medium constructs from three authored SPD curves" );
		if( fire ) {
			Check( Near(fire->GetChemSPDAuthoredArea(0),10.0,1e-15),
				"pinned 1 nm trapezoid normalizes a 10 nm top-hat to area ten" );
			const Scalar glArea = GaussLegendre21::IntegrateVisibleBand(
				[&](const Scalar nm){ return tenNanometre->Evaluate(nm); } );
			Check( std::fabs(glArea-10.0) > 0.5,
				"the shared smooth-band GL21 rule is observably unsuitable for the 10 nm top-hat" );
			const Scalar expectedSource = 120.0/(FOUR_PI*10.0);
			Check( NearRelative(fire->GetChemEmissionNM(Point3(0.5,0.5,0.5),505.0),
				expectedSource,1e-14),
				"band-integrated W/m^3 becomes per-nm per-steradian source after authored-interval normalization" );
			Check( fire->GetChemEmissionNM(Point3(0.5,0.5,0.5),499.0) == 0.0,
				"normalized chem SPD has no support outside its declared interval" );

			RandomNumberGenerator rng(901u);
			Implementation::IndependentSampler sampler(rng);
			const Ray ray(Point3(-1.0,0.5,0.5),Vector3(1.0,0.0,0.0));
			const Scalar lineEstimate = fire->EstimateChemEmissionSegmentNM(
				ray,1.0,2.0,505.0,sampler);
			Check( NearRelative(lineEstimate,expectedSource,1e-14),
				"full-segment chem estimator is exact for a zero-extinction uniform source" );
		}

		OffsetQuadraticFunction* quadraticSPD =
			new OffsetQuadraticFunction(500.0);
		IMedium* quadraticMedium = CreateMediumWithChem(
			*carbon, *temperature, *chemCH, *chemZero, *chemZero,
			*quadraticSPD, *quadraticSPD, *quadraticSPD,
			500.0, 510.0, Point3(0,0,0), Point3(1,1,1), 1.0 );
		const MultichannelHeterogeneousMedium* quadratic =
			dynamic_cast<const MultichannelHeterogeneousMedium*>(quadraticMedium);
		if( quadratic ) {
			// Composite h=1 trapezoid for x^2 over [0,10]:
			// 1/2(0^2+10^2) + sum_{i=1}^{9} i^2 = 50+285 = 335.
			Check( Near(quadratic->GetChemSPDAuthoredArea(0),335.0,1e-14),
				"nonconstant SPD pins every 1 nm composite-trapezoid sample" );
			Check( NearRelative(quadratic->GetChemEmissionNM(
				Point3(0.5,0.5,0.5),505.0),120.0*25.0/(FOUR_PI*335.0),1e-14),
				"the pinned nonconstant normalization area is used by spectral emission" );
		}

		IntervalTopHatFunction* fractionalHat =
			new IntervalTopHatFunction( 500.0, 510.5 );
		IMedium* fractionalMedium = CreateMediumWithChem(
			*carbon, *temperature, *chemCH, *chemZero, *chemZero,
			*fractionalHat, *fractionalHat, *fractionalHat,
			500.0, 510.5, Point3(0,0,0), Point3(1,1,1), 1.0 );
		const MultichannelHeterogeneousMedium* fractional =
			dynamic_cast<const MultichannelHeterogeneousMedium*>(fractionalMedium);
		if( fractional ) {
			Check( Near(fractional->GetChemSPDAuthoredArea(0),10.5,1e-15),
				"1 nm trapezoid normalization includes the final fractional panel" );
			Check( NearRelative(fractional->GetChemEmissionNM(
				Point3(0.5,0.5,0.5),505.0),120.0/(FOUR_PI*10.5),1e-14),
				"fractional-panel normalization controls the per-nm source" );
		}

		IMedium* overlappingMedium = CreateMediumWithChem(
			*carbon, *temperature, *chemCH, *chemC2, *chemZero,
			*tenNanometre, *tenNanometre, *tenNanometre,
			500.0, 510.0, Point3(0,0,0), Point3(1,1,1), 0.01 );
		const MultichannelHeterogeneousMedium* overlapping =
			dynamic_cast<const MultichannelHeterogeneousMedium*>(overlappingMedium);
		if( overlapping ) {
			Check( NearRelative(overlapping->GetChemEmissionNM(
				Point3(0.5,0.5,0.5),505.0),
				0.01*(120.0+50.0)/(FOUR_PI*10.0),1e-14),
				"overlapping chem bands add their normalized per-nm sources" );
		}

		IntervalTopHatFunction* clippedHat =
			new IntervalTopHatFunction( 375.0, 385.0 );
		IMedium* clippedMedium = CreateMediumWithChem(
			*carbon, *temperature, *chemCH, *chemZero, *chemZero,
			*clippedHat, *clippedHat, *clippedHat,
			375.0, 385.0, Point3(0,0,0), Point3(1,1,1), 1.0 );
		const MultichannelHeterogeneousMedium* clipped =
			dynamic_cast<const MultichannelHeterogeneousMedium*>(clippedMedium);
		if( clipped ) {
			Scalar visibleIntegral = 0.0;
			for( unsigned int nm = 380u; nm < 385u; ++nm ) {
				visibleIntegral += 0.5*(
					clipped->GetChemEmissionNM(Point3(0.5,0.5,0.5),Scalar(nm))+
					clipped->GetChemEmissionNM(Point3(0.5,0.5,0.5),Scalar(nm+1u)) );
			}
			Check( NearRelative(visibleIntegral,0.5*120.0/FOUR_PI,1e-14),
				"renderer-band clipping loses half the authored power without renormalizing" );
		}

		IMedium* centimetreMedium = CreateMediumWithChem(
			*carbon, *temperature, *chemCH, *chemZero, *chemZero,
			*tenNanometre, *tenNanometre, *tenNanometre,
			500.0, 510.0, Point3(0,0,0), Point3(100,100,100), 0.01 );
		const MultichannelHeterogeneousMedium* centimetres =
			dynamic_cast<const MultichannelHeterogeneousMedium*>(centimetreMedium);
		if( fire && centimetres ) {
			RandomNumberGenerator metreRng(77u);
			RandomNumberGenerator centimetreRng(77u);
			Implementation::IndependentSampler metreSampler(metreRng);
			Implementation::IndependentSampler centimetreSampler(centimetreRng);
			const Scalar metres = fire->EstimateChemEmissionSegmentNM(
				Ray(Point3(-1.0,0.5,0.5),Vector3(1,0,0)),1.0,2.0,505.0,metreSampler);
			const Scalar cm = centimetres->EstimateChemEmissionSegmentNM(
				Ray(Point3(-100.0,50.0,50.0),Vector3(1,0,0)),100.0,200.0,
				505.0,centimetreSampler);
			Check( NearRelative(cm,metres,1e-14),
				"chem line radiance is invariant between metre and centimetre scene units" );
		}

		IntervalTopHatFunction* zeroSPD =
			new IntervalTopHatFunction(500.0,510.0,0.0);
		IMedium* invalidSPDMedium = CreateMediumWithChem(
			*carbon, *temperature, *chemCH, *chemZero, *chemZero,
			*zeroSPD, *tenNanometre, *tenNanometre,
			500.0, 510.0, Point3(0,0,0), Point3(1,1,1), 1.0 );
		Check( invalidSPDMedium == nullptr,
			"zero-area authored SPD is rejected at medium construction" );
		safe_release(invalidSPDMedium);
		safe_release(zeroSPD);

		IntervalTopHatFunction* negativeSPD =
			new IntervalTopHatFunction(500.0,510.0,-1.0);
		invalidSPDMedium = CreateMediumWithChem(
			*carbon, *temperature, *chemCH, *chemZero, *chemZero,
			*negativeSPD, *tenNanometre, *tenNanometre,
			500.0, 510.0, Point3(0,0,0), Point3(1,1,1), 1.0 );
		Check( invalidSPDMedium == nullptr,
			"negative authored SPD samples are rejected at medium construction" );
		safe_release(invalidSPDMedium);
		safe_release(negativeSPD);

		safe_release(centimetreMedium);
		safe_release(clippedMedium);
		safe_release(clippedHat);
		safe_release(overlappingMedium);
		safe_release(fractionalMedium);
		safe_release(fractionalHat);
		safe_release(quadraticMedium);
		safe_release(quadraticSPD);
		safe_release(medium);
		safe_release(tenNanometre);
		safe_release(chemZero);
		safe_release(chemC2);
		safe_release(chemCH);
		safe_release(temperature);
		safe_release(carbon);
	}

	void TestChemReactionMixtureAgainstAttenuatedReference()
	{
		std::cout << "TestChemReactionMixtureAgainstAttenuatedReference" << std::endl;
		ThinSheetXPainter* carbonSheet =
			new ThinSheetXPainter(0.66,0.72,4.0);
		AffineWorldScalarPainter* temperature =
			new AffineWorldScalarPainter(1000.0,0.0,0.0,0.0);
		ThinSheetXPainter* reactionSheet =
			new ThinSheetXPainter(0.66,0.72,200.0);
		AffineWorldScalarPainter* chemZero =
			new AffineWorldScalarPainter(0.0,0.0,0.0,0.0);
		IntervalTopHatFunction* spd = new IntervalTopHatFunction(500.0,510.0);
		IMedium* medium = CreateMediumWithChem(
			*carbonSheet,*temperature,*reactionSheet,*chemZero,*chemZero,
			*spd,*spd,*spd,500.0,510.0,
			Point3(0,0,0),Point3(1,1,1),1.0,32u);
		const MultichannelHeterogeneousMedium* fire =
			dynamic_cast<const MultichannelHeterogeneousMedium*>(medium);
		Check( fire != nullptr,
			"thin reaction sheet aligned with an extinction transition constructs" );
		if( fire ) {
			const Scalar wavelength = 505.0;
			const unsigned int referenceSteps = 200000u;
			const Scalar h = 1.0/Scalar(referenceSteps);
			Scalar reference = 0.0;
			Scalar tau = 0.0;
			for( unsigned int i = 0; i < referenceSteps; ++i ) {
				const Scalar x = (Scalar(i)+0.5)*h;
				const Point3 point(x,0.5,0.5);
				const Scalar sigmaT = fire->GetCoefficientsNM(point,wavelength).sigma_t;
				const Scalar epsilon = fire->GetChemEmissionNM(point,wavelength);
				reference += exp(-(tau+0.5*sigmaT*h))*epsilon*h;
				tau += sigmaT*h;
			}
			Check( tau > 1.0,
				"the adversarial reaction sheet overlaps a genuinely high optical-depth transition" );

			const Ray ray(Point3(0,0.5,0.5),Vector3(1,0,0));
			const unsigned int sampleCount = 12000u;
			ForcedBranchSampler uniformSampler(0.25,7101u);
			ForcedBranchSampler reactionSampler(0.75,9107u);
			Scalar uniformSum = 0.0;
			Scalar uniformSquareSum = 0.0;
			Scalar reactionSum = 0.0;
			Scalar reactionSquareSum = 0.0;
			for( unsigned int i = 0; i < sampleCount; ++i ) {
				uniformSampler.BeginSample();
				const Scalar uniformEstimate = fire->EstimateChemEmissionSegmentNM(
					ray,0.0,1.0,wavelength,uniformSampler);
				uniformSum += uniformEstimate;
				uniformSquareSum += uniformEstimate*uniformEstimate;

				reactionSampler.BeginSample();
				const Scalar reactionEstimate = fire->EstimateChemEmissionSegmentNM(
					ray,0.0,1.0,wavelength,reactionSampler);
				reactionSum += reactionEstimate;
				reactionSquareSum += reactionEstimate*reactionEstimate;
			}
			const Scalar uniformMean = uniformSum/Scalar(sampleCount);
			const Scalar reactionMean = reactionSum/Scalar(sampleCount);
			const Scalar estimate = 0.5*(uniformMean+reactionMean);
			const Scalar uniformVariance = fmax(0.0,
				uniformSquareSum/Scalar(sampleCount)-uniformMean*uniformMean);
			const Scalar reactionVariance = fmax(0.0,
				reactionSquareSum/Scalar(sampleCount)-reactionMean*reactionMean);
			const Scalar standardError = sqrt(0.25*(uniformVariance+reactionVariance)/
				Scalar(sampleCount));
			Check( uniformMean > 0.0 && reactionMean > 0.0,
				"both the uniform and reaction-importance halves carry thin-sheet contribution" );
			Check( std::fabs(estimate-reference) <=
				6.0*standardError+0.003*reference,
				"real chem mixture matches an independent attenuated thin-sheet reference" );
		}

		safe_release(medium);
		safe_release(spd);
		safe_release(chemZero);
		safe_release(reactionSheet);
		safe_release(temperature);
		safe_release(carbonSheet);
	}

	void TestNonFiniteRejection()
	{
		std::cout << "TestNonFiniteRejection" << std::endl;
		const Scalar infinity = ScalarFromBits( UINT64_C(0x7FF0000000000000) );
		const Scalar nan = ScalarFromBits( UINT64_C(0x7FF8000000000000) );
		AffineWorldScalarPainter* carbon = new AffineWorldScalarPainter( 1.0, 0.0, 0.0, 0.0 );
		AffineWorldScalarPainter* temperature = new AffineWorldScalarPainter( 1000.0, 0.0, 0.0, 0.0 );

		AffineWorldScalarPainter* infiniteCarbon =
			new AffineWorldScalarPainter( infinity, 0.0, 0.0, 0.0 );
		Check( FactoryRejects( *infiniteCarbon, *temperature, FactoryInputs() ),
			"+Inf carbon painter is rejected before majorant construction" );
		safe_release( infiniteCarbon );

		AffineWorldScalarPainter* nanTemperature =
			new AffineWorldScalarPainter( nan, 0.0, 0.0, 0.0 );
		Check( FactoryRejects( *carbon, *nanTemperature, FactoryInputs() ),
			"NaN temperature painter is rejected under fast-math" );
		safe_release( nanTemperature );

		AffineWorldScalarPainter* zeroTemperature =
			new AffineWorldScalarPainter( 0.0, 0.0, 0.0, 0.0 );
		Check( FactoryRejects( *carbon, *zeroTemperature, FactoryInputs() ),
			"non-positive temperature is rejected" );
		safe_release( zeroTemperature );

		FactoryInputs invalid;
		invalid.bboxMin.x = nan;
		Check( FactoryRejects( *carbon, *temperature, invalid ),
			"NaN bbox component is rejected by the direct factory" );
		invalid = FactoryInputs();
		invalid.bboxMax.z = infinity;
		Check( FactoryRejects( *carbon, *temperature, invalid ),
			"+Inf bbox component is rejected by the direct factory" );

		struct ScalarInput
		{
			const char* label;
			Scalar FactoryInputs::* member;
		};
		const ScalarInput scalarInputs[] = {
			{ "scene_unit", &FactoryInputs::sceneUnitMeters },
			{ "soot_em", &FactoryInputs::sootEm },
			{ "soot_density", &FactoryInputs::sootDensity },
			{ "soot_albedo_hot", &FactoryInputs::sootAlbedoHot },
			{ "soot_g_hot", &FactoryInputs::sootGHot },
			{ "smoke_km_carbon", &FactoryInputs::smokeKmCarbon },
			{ "smoke_n_carbon", &FactoryInputs::smokeNCarbon },
			{ "smoke_albedo_carbon", &FactoryInputs::smokeAlbedoCarbon },
			{ "smoke_g_carbon", &FactoryInputs::smokeGCarbon }
		};
		for( const ScalarInput& input : scalarInputs ) {
			invalid = FactoryInputs();
			invalid.*(input.member) = infinity;
			Check( FactoryRejects( *carbon, *temperature, invalid ),
				(std::string( "+Inf " ) + input.label + " is rejected by the direct factory").c_str() );
		}

		invalid = FactoryInputs();
		invalid.sootGHot = 1.0;
		Check( FactoryRejects( *carbon, *temperature, invalid ),
			"g_hot=1 is rejected because the HG continuation density must remain finite" );
		invalid = FactoryInputs();
		invalid.smokeGCarbon = -1.0;
		Check( FactoryRejects( *carbon, *temperature, invalid ),
			"g_carbon=-1 is rejected because the HG continuation density must remain finite" );

		safe_release( carbon );
		safe_release( temperature );
	}

	const IAsciiChunkParser* FindMultichannelParser(
		const std::vector<ChunkParserEntry>& entries
		)
	{
		for( const ChunkParserEntry& entry : entries ) {
			if( entry.keyword == "multichannel_heterogeneous_medium" ) {
				return entry.parser.get();
			}
		}
		return nullptr;
	}

	void FillValidBag( ParseStateBag& bag, const char* omit )
	{
		struct Pair { const char* key; const char* value; };
		static const Pair values[] = {
			{ "name", "fire" },
			{ "channel_carbon", "painter carbon" },
			{ "channel_temperature", "painter temperature" },
			{ "chem_model", "none" },
			{ "bake_resolution", "4 4 4" },
			{ "bbox_min", "0 0 0" },
			{ "bbox_max", "1 1 1" },
			{ "optical_record", "synthetic_regression_v1" }
		};
		for( const Pair& value : values ) {
			if( !omit || std::string( value.key ) != omit ) {
				bag.SetSingle( value.key, value.value );
			}
		}
	}

	void AddChemFields( ParseStateBag& bag, const char* omit )
	{
		struct Pair { const char* key; const char* value; };
		static const Pair values[] = {
			{ "channel_chem_ch", "painter chem_ch" },
			{ "channel_chem_c2", "painter chem_c2" },
			{ "channel_chem_co2", "painter chem_co2" },
			{ "chem_spd_ch", "chem_ch_spd" },
			{ "chem_spd_c2", "chem_c2_spd" },
			{ "chem_spd_co2", "chem_co2_spd" },
			{ "chem_interval_ch", "500 510" },
			{ "chem_interval_c2", "500 510" },
			{ "chem_interval_co2", "500 510" }
		};
		for( const Pair& value : values ) {
			if( !omit || std::string(value.key) != omit ) {
				bag.SetSingle(value.key,value.value);
			}
		}
	}

	void TestDescriptorAndRequiredness()
	{
		std::cout << "TestDescriptorAndRequiredness" << std::endl;
		std::vector<ChunkParserEntry> entries = CreateAllChunkParsers();
		const IAsciiChunkParser* parser = FindMultichannelParser( entries );
		Check( parser != nullptr, "chunk is registered" );
		if( !parser ) return;

		const ChunkDescriptor& descriptor = parser->Describe();
		std::vector<std::string> required;
		for( const ParameterDescriptor& parameter : descriptor.parameters ) {
			if( parameter.required ) required.push_back( parameter.name );
		}
		Check( required.size() == 7, "the seven structural and record fields are required" );
		const char* condensedFields[] = { "channel_condensed" };
		for( const char* field : condensedFields ) {
			bool foundOptional = false;
			for( const ParameterDescriptor& parameter : descriptor.parameters ) {
				if( parameter.name == field ) foundOptional = !parameter.required;
			}
			Check( foundOptional,
				(std::string( field ) + " is advertised as conditionally required").c_str() );
		}
		const char* chemFields[] = {
			"chem_model", "channel_chem_ch", "channel_chem_c2",
			"channel_chem_co2", "chem_spd_ch", "chem_spd_c2",
			"chem_spd_co2", "chem_interval_ch", "chem_interval_c2",
			"chem_interval_co2"
		};
		for( const char* field : chemFields ) {
			bool foundOptional = false;
			for( const ParameterDescriptor& parameter : descriptor.parameters ) {
				if( parameter.name == field ) foundOptional = !parameter.required;
			}
			Check( foundOptional,
				(std::string(field)+" is advertised as part of the conditional chem bundle").c_str() );
		}

		IJobPriv* job = nullptr;
		RISE_CreateJobPriv( &job );
		Check( job != nullptr, "requiredness fixture job constructs" );
		if( !job ) return;

		for( const std::string& omitted : required ) {
			ParseStateBag bag( &descriptor );
			FillValidBag( bag, omitted.c_str() );
			Check( !parser->Finalize( bag, *job ),
				("omitting required parameter " + omitted + " fails").c_str() );
		}
		{
			ParseStateBag bag( &descriptor );
			FillValidBag( bag, "chem_model" );
			Check( !parser->Finalize(bag,*job),
				"omitting both the all-band chem bundle and chem_model none fails" );
		}
		{
			ParseStateBag bag( &descriptor );
			FillValidBag( bag, nullptr );
			bag.SetSingle( "channel_chem_ch", "painter chem_ch" );
			Check( !parser->Finalize(bag,*job),
				"a partial chem bundle fails even when chem_model none is present" );
		}
		const char* requiredChemBundle[] = {
			"channel_chem_ch", "channel_chem_c2", "channel_chem_co2",
			"chem_spd_ch", "chem_spd_c2", "chem_spd_co2",
			"chem_interval_ch", "chem_interval_c2", "chem_interval_co2"
		};
		for( const char* omitted : requiredChemBundle ) {
			ParseStateBag bag( &descriptor );
			FillValidBag( bag, "chem_model" );
			AddChemFields( bag, omitted );
			Check( !parser->Finalize(bag,*job),
				(std::string("chem bundle without ")+omitted+" fails").c_str() );
		}
		safe_release( job );
	}

	std::string SceneText()
	{
		return
			"RISE ASCII SCENE 7\n\n"
			"scene_options\n{\nscene_unit 0.01\n}\n\n"
			"scalar_painter\n{\nname carbon\nvalue 1\n}\n\n"
			"scalar_painter\n{\nname temperature\nvalue 800\n}\n\n"
			"scalar_painter\n{\nname condensed\nvalue 2\n}\n\n"
			"scalar_painter\n{\nname chem_ch\nvalue 120\n}\n\n"
			"scalar_painter\n{\nname chem_c2\nvalue 50\n}\n\n"
			"scalar_painter\n{\nname chem_co2\nvalue 8\n}\n\n"
			"piecewise_linear_function\n{\nname chem_ch_spd\ncp 499 0\ncp 500 1\ncp 510 1\ncp 511 0\n}\n\n"
			"piecewise_linear_function\n{\nname chem_c2_spd\ncp 549 0\ncp 550 1\ncp 555 1\ncp 556 0\n}\n\n"
			"piecewise_linear_function\n{\nname chem_co2_spd\ncp 699 0\ncp 700 3\ncp 702 3\ncp 703 0\n}\n\n"
			"multichannel_heterogeneous_medium\n{\n"
			"name fire\n"
			"channel_carbon painter carbon\n"
			"channel_temperature painter temperature\n"
			"channel_condensed painter condensed\n"
			"channel_chem_ch painter chem_ch\n"
			"channel_chem_c2 painter chem_c2\n"
			"channel_chem_co2 painter chem_co2\n"
			"chem_spd_ch chem_ch_spd\n"
			"chem_spd_c2 chem_c2_spd\n"
			"chem_spd_co2 chem_co2_spd\n"
			"chem_interval_ch 500 510\n"
			"chem_interval_c2 550 555\n"
			"chem_interval_co2 700 702\n"
			"bake_resolution 4 4 4\n"
			"bbox_min 0 0 0\n"
			"bbox_max 100 100 100\n"
			"optical_record synthetic_regression_v1\n"
			"}\n";
	}

	void TestSceneLanguageAndSceneUnitPropagation()
	{
		std::cout << "TestSceneLanguageAndSceneUnitPropagation" << std::endl;
		char filename[128];
		std::snprintf( filename, sizeof(filename),
			"rise_multichannel_medium_%d.RISEscene", static_cast<int>( ::getpid() ) );
		const std::filesystem::path path =
			std::filesystem::temp_directory_path() / filename;
		{
			std::ofstream output( path );
			output << SceneText();
		}

		IJobPriv* job = nullptr;
		RISE_CreateJobPriv( &job );
		const bool loaded = job && job->LoadAsciiSceneViaCst( path.string().c_str() );
		Check( loaded, "complete multichannel scene chunk loads" );
		if( loaded ) {
			const IMedium* medium = job->GetMedium( "fire" );
			const MultichannelHeterogeneousMedium* fire =
				dynamic_cast<const MultichannelHeterogeneousMedium*>( medium );
			Check( fire != nullptr, "scene chunk registers the concrete medium" );
			if( fire ) {
				const Scalar hotAbsorptionMass =
					6.0 * PI * 0.26 * 1.0e-3 / (633.0e-9 * 1800.0);
				const RISEPel expectedSigmaT = 0.01 *
					(ResponsePowerMean(1.0)*(0.5*hotAbsorptionMass/0.90) +
					 ResponsePowerMean(1.2)*(0.5*8.7) +
					 ResponsePowerMean(0.5)*(2.0*3.298));
				const RISEPel actualSigmaT =
					fire->GetCoefficients( Point3( 50, 50, 50 ) ).sigma_t;
				Check( NearRelative(actualSigmaT.r,expectedSigmaT.r,1e-13) &&
					NearRelative(actualSigmaT.g,expectedSigmaT.g,1e-13) &&
					NearRelative(actualSigmaT.b,expectedSigmaT.b,1e-13),
					"scene_options.scene_unit reaches medium construction" );
				Check( Near(fire->GetChemSPDAuthoredArea(0),10.0,1e-15),
					"authored CH fixture curve uses its own normalization interval" );
				Check( Near(fire->GetChemSPDAuthoredArea(1),5.0,1e-15),
					"authored C2 fixture curve uses its distinct normalization interval" );
				Check( Near(fire->GetChemSPDAuthoredArea(2),6.0,1e-15),
					"authored CO2 fixture curve preserves its distinct shape scale" );
				Check( NearRelative(fire->GetChemEmissionNM(Point3(50,50,50),505.0),
					0.01*120.0/(FOUR_PI*10.0),1e-14),
					"CST binding isolates the CH painter, SPD, and interval" );
				Check( NearRelative(fire->GetChemEmissionNM(Point3(50,50,50),552.0),
					0.01*50.0/(FOUR_PI*5.0),1e-14),
					"CST binding isolates the C2 painter, SPD, and interval" );
				Check( NearRelative(fire->GetChemEmissionNM(Point3(50,50,50),701.0),
					0.01*8.0*3.0/(FOUR_PI*6.0),1e-14),
					"CST binding isolates the CO2 painter, SPD, and interval" );

				const Scalar wavelengths[] = { 450.0, 750.0 };
				for( const Scalar nm : wavelengths ) {
					const Scalar wavelengthScale = 633.0 / nm;
					const Scalar hotScattering = 0.5 * hotAbsorptionMass * wavelengthScale *
						0.10 / 0.90;
					const Scalar coolScattering = 0.5 * 8.7 *
						pow( wavelengthScale, 1.2 ) * 0.60;
					const Scalar condScattering = 2.0 * 3.298 *
						pow( wavelengthScale, 0.5 ) * 0.90;
					const Scalar expectedMean =
						(hotScattering * 0.5 + coolScattering * 0.6 +
						 condScattering * 0.7) /
						(hotScattering + coolScattering + condScattering);
					const IPhaseFunction* closure = fire->MakePhaseClosure(
						Point3( 50, 50, 50 ), nm );
					Check( closure && Near( closure->GetMeanCosine(), expectedMean, 1e-12 ),
						"CST wiring consumes the single synthetic optical record" );
					if( closure ) closure->release();
				}
			}
		}

		safe_release( job );
		std::filesystem::remove( path );
	}

	void TestProductionFireFidelityPreflight()
	{
		std::cout << "TestProductionFireFidelityPreflight" << std::endl;
		char filename[128];
		std::snprintf( filename, sizeof(filename),
			"rise_fire_fidelity_%d.RISEscene", static_cast<int>( ::getpid() ) );
		const std::filesystem::path path =
			std::filesystem::temp_directory_path() / filename;
		const auto writeScene = [&path]( const unsigned int nmBegin ) {
			std::ofstream output(path);
			output <<
				"RISE ASCII SCENE 7\n\n"
				"scene_options\n{\nscene_unit 1\nfidelity_mode preview\n}\n\n"
				"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
				"pathtracing_spectral_rasterizer\n{\nsamples 1\nnmbegin " << nmBegin <<
				"\nnmend 780\nnum_wavelengths 4\nspectral_samples 1\nhwss false\nmax_volume_bounce 1\noidn_denoise false\nprogressive_rendering false\n}\n\n"
				"film\n{\nwidth 1\nheight 1\n}\n\n"
				"pinhole_camera\n{\nlocation 0 0 -2\nlookat 0 0 0\nup 0 1 0\nfov 45\n}\n\n"
				"scalar_painter\n{\nname carbon\nvalue 1\n}\n\n"
				"scalar_painter\n{\nname temperature\nvalue 800\n}\n\n"
				"multichannel_heterogeneous_medium\n{\nname fire\nchannel_carbon painter carbon\nchannel_temperature painter temperature\nchem_model none\nbake_resolution 2 2 2\nbbox_min -1 -1 -1\nbbox_max 1 1 1\noptical_record fire_optics_v1\n}\n\n"
				"global_medium\n{\nmedium fire\n}\n";
		};
		writeScene(380u);

		IJobPriv* job = nullptr;
		RISE_CreateJobPriv(&job);
		const bool loaded = job && job->LoadAsciiSceneViaCst(path.string().c_str());
		Check( loaded, "scene-level preview fidelity request loads" );
		if( loaded ) {
			Check( job->Rasterize(),
				"the exact certified 380-780 nm band renders in preview mode" );
			Implementation::Rasterizer* rasterizer =
				dynamic_cast<Implementation::Rasterizer*>(job->GetRasterizer());
			Implementation::FrameStore* store = rasterizer ? rasterizer->GetFrameStore() : 0;
			Check( store && store->Meta().renderFidelityStatus == "preview" &&
				std::is_sorted(store->Meta().renderReasonCodes.begin(),
					store->Meta().renderReasonCodes.end()) &&
				std::find(store->Meta().renderReasonCodes.begin(),
					store->Meta().renderReasonCodes.end(),"requested_preview") !=
					store->Meta().renderReasonCodes.end() &&
				std::find(store->Meta().renderReasonCodes.begin(),
					store->Meta().renderReasonCodes.end(),"table_domain_exceeded") ==
					store->Meta().renderReasonCodes.end() &&
				store->Meta().activeFireOpticsRecordIds.size() == 1u &&
				store->Meta().activeFireOpticsRecordIds[0] ==
					"0b57c2edfb73e06b5d2ceac10f778d1386554be8818434dfc4c71dd9a11d2502",
				"production FrameStore carries sorted preview reasons and record identity" );

			Check( job->SetFireFidelityMode("predictive") && !job->Rasterize(),
				"predictive static-fire request fails closed on its fidelity reasons" );
			rasterizer = dynamic_cast<Implementation::Rasterizer*>(job->GetRasterizer());
			store = rasterizer ? rasterizer->GetFrameStore() : 0;
			Check( store && std::find(store->Meta().renderReasonCodes.begin(),
				store->Meta().renderReasonCodes.end(),"producer_unqualified") !=
				store->Meta().renderReasonCodes.end() &&
				std::find(store->Meta().renderReasonCodes.begin(),
					store->Meta().renderReasonCodes.end(),"requested_preview") ==
					store->Meta().renderReasonCodes.end(),
				"predictive rejection metadata is freshly derived from the request" );
			Check( !job->SetFireFidelityMode("invented"),
				"the job rejects an unknown fire fidelity mode" );
		}
		safe_release(job);

		writeScene(379u);
		RISE_CreateJobPriv(&job);
		const bool outOfDomainLoaded = job &&
			job->LoadAsciiSceneViaCst(path.string().c_str());
		Check( outOfDomainLoaded,
			"out-of-domain wavelength support remains structurally loadable" );
		if( outOfDomainLoaded ) {
			Check( !job->Rasterize(),
				"preview preflight rejects a wavelength below the certified domain" );
			Implementation::Rasterizer* rasterizer =
				dynamic_cast<Implementation::Rasterizer*>(job->GetRasterizer());
			Implementation::FrameStore* store = rasterizer ? rasterizer->GetFrameStore() : 0;
			Check( store && std::find(store->Meta().renderReasonCodes.begin(),
				store->Meta().renderReasonCodes.end(),"table_domain_exceeded") !=
				store->Meta().renderReasonCodes.end(),
				"out-of-domain preview rejection records table_domain_exceeded" );
		}
		safe_release(job);
		std::filesystem::remove(path);
	}
}

int main()
{
	TestMatrixOnlyFilmResponse();
	TestPredictivePresetSpectralConsumptionAndFidelity();
	TestBakedTrilinearChannelsAndOptics();
	TestChromaticNMTrackingAndTransmittance();
	TestCondensedConstituentOpticsAndClosure();
	TestWavelengthBoundConstituentPhaseClosure();
	TestContinuationPhaseClosurePreflightTable();
	TestPhysicalUnitsAndSceneScale();
	TestPhiSupMajorant();
	TestPhiAwareQuadratureAndDistancePdf();
	TestChemSPDNormalizationClippingAndSceneScale();
	TestChemReactionMixtureAgainstAttenuatedReference();
	TestNonFiniteRejection();
	TestDescriptorAndRequiredness();
	TestSceneLanguageAndSceneUnitPropagation();
	TestProductionFireFidelityPreflight();

	std::cout << passed << " passed, " << failed << " failed" << std::endl;
	return failed == 0 ? 0 : 1;
}
