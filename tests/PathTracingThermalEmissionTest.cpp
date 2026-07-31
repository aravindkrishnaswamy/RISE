//////////////////////////////////////////////////////////////////////
//
//  PathTracingThermalEmissionTest.cpp - Phase-A step-6 spectral gates.
//
//  Verifies that the standalone PathTracingIntegrator scores fire thermal
//  emission before the pure-absorber and max-volume-bounce continuation
//  exits, agrees with RayCaster's already-gated estimator, preserves scene-
//  unit invariance, and routes HWSS-requested fire transport through the
//  unbiased per-wavelength NM fallback.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>
#endif

#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRayCaster.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IShader.h"
#include "../src/Library/Interfaces/IShaderManager.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Job.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Materials/HeterogeneousMedium.h"
#include "../src/Library/Materials/IsotropicPhaseFunction.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/SceneEditor/SceneEdit.h"
#include "../src/Library/SceneEditor/SceneEditor.h"
#include "../src/Library/Shaders/PathTracingIntegrator.h"
#include "../src/Library/Utilities/EquiangularSampler.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/MISWeights.h"
#include "../src/Library/Utilities/PlanckRadiance.h"
#ifdef RISE_ENABLE_OPENPGL
	#include "../src/Library/Utilities/PathGuidingField.h"
#endif
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/RuntimeContext.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

namespace
{
	int passed = 0;
	int failed = 0;
	const Scalar kTemperatureK = 1800.0;
	const Scalar kTargetSigmaSI = 0.8;
	const unsigned int kSamples = 80000;

	void Check( const bool condition, const char* label )
	{
		if( condition ) {
			++passed;
		} else {
			++failed;
			std::cout << "FAIL: " << label << std::endl;
		}
	}

	bool NearRelative( const Scalar actual, const Scalar expected, const Scalar tolerance )
	{
		const Scalar scale = std::fmax( std::fabs( expected ), Scalar( 1e-300 ) );
		return std::fabs( actual - expected ) <= tolerance * scale;
	}

	Scalar IntegrateEquiangularPdf(
		const Ray& ray, const Point3& pivot, const Scalar tNear,
		const Scalar tFar, const unsigned int intervals )
	{
		const Scalar step = (tFar-tNear) / static_cast<Scalar>( intervals );
		Scalar integral = 0.0;
		for( unsigned int i = 0; i < intervals; ++i ) {
			const Scalar t = tNear+(static_cast<Scalar>(i)+0.5)*step;
			integral += EquiangularSampling::Pdf(
				ray, pivot, tNear, tFar, true, t ) * step;
		}
		return integral;
	}

	Scalar HotAbsorptionMass633()
	{
		return 6.0 * PI * 0.26 * 1.0e-3 / (633.0e-9 * 1800.0);
	}

	std::string SceneText(
		const Scalar sceneUnitMeters,
		const Scalar slabLength,
		const bool positionalLight,
		const Scalar sootAlbedoHot = 0.0 )
	{
		const Scalar scale = 1.0 / sceneUnitMeters;
		const Scalar carbon = kTargetSigmaSI / HotAbsorptionMass633();
		std::ostringstream ss;
		ss << std::setprecision( 17 ) <<
			"RISE ASCII SCENE 7\n"
			"scene_options\n{\nscene_unit " << sceneUnitMeters << "\n}\n"
			"standard_shader\n{\nname global\n}\n"
			"scalar_painter\n{\nname carbon\nvalue " << carbon << "\n}\n"
			"scalar_painter\n{\nname temperature\nvalue " << kTemperatureK << "\n}\n"
			"multichannel_heterogeneous_medium\n{\n"
			"name fire\n"
			"channel_carbon painter carbon\n"
			"channel_temperature painter temperature\n"
			"bake_resolution 4 4 4\n"
			"bbox_min " << -0.5 * scale << " " << -0.5 * scale << " " << -slabLength << "\n"
			"bbox_max " << 0.5 * scale << " " << 0.5 * scale << " " << 2.0 * slabLength << "\n"
			"soot_em 0.26\n"
			"soot_density 1800\n"
			"soot_albedo_hot " << sootAlbedoHot << "\n"
			"soot_g_hot 0.5\n"
			"smoke_km_carbon 8.7\n"
			"smoke_n_carbon 1.2\n"
			"smoke_albedo_carbon 0.6\n"
			"smoke_g_carbon 0.6\n"
			"}\n"
			"global_medium\n{\nmedium fire\n}\n"
			"box_geometry\n{\nname wall_geometry\nwidth " << 10.0 * scale <<
			"\nheight " << 10.0 * scale << "\ndepth " << 0.2 * scale << "\n}\n"
			"standard_object\n{\nname black_wall\ngeometry wall_geometry\nmaterial none\nposition 0 0 " <<
			(slabLength + 0.1 * scale) << "\n}\n";
		if( positionalLight ) {
			ss << "omni_light\n{\nname sampling_pivot\npower 1\ncolor 1 1 1\nposition "
				<< 0.75 * scale << " 0 " << 0.5 * slabLength << "\n}\n";
		}
		return ss.str();
	}

	struct Fixture
	{
		IJobPriv* job;
		IRayCaster* caster;
		PathTracingIntegrator* integrator;
		std::filesystem::path scenePath;
		Scalar slabLength;

		Fixture() : job( nullptr ), caster( nullptr ), integrator( nullptr ), slabLength( 0 ) {}

		~Fixture()
		{
			safe_release( integrator );
			safe_release( caster );
			safe_release( job );
			if( !scenePath.empty() ) std::filesystem::remove( scenePath );
		}

		bool Initialize(
			const char* tag,
			const Scalar sceneUnitMeters,
			const Scalar length,
			const bool positionalLight = false,
			const Scalar sootAlbedoHot = 0.0 )
		{
			slabLength = length;
			char filename[176];
			std::snprintf( filename, sizeof(filename),
				"rise_pathtracing_thermal_%s_%d.RISEscene", tag, static_cast<int>( ::getpid() ) );
			scenePath = std::filesystem::temp_directory_path() / filename;
			{
				std::ofstream output( scenePath );
				if( !output.is_open() ) return false;
				output << SceneText(
					sceneUnitMeters, length, positionalLight, sootAlbedoHot );
			}

			if( !RISE_CreateJobPriv( &job ) || !job ||
				!job->LoadAsciiSceneViaCst( scenePath.string().c_str() ) ) return false;
			IShader* shader = job->GetShaders()->GetItem( "global" );
			if( !shader || !RISE_API_CreateRayCaster(
				&caster, false, 0, *shader, true ) || !caster ) return false;
			caster->AttachScene( job->GetScene() );

			StabilityConfig stability;
			stability.maxVolumeBounce = 0;
			integrator = new PathTracingIntegrator( ManifoldSolverConfig(), stability );
			return true;
		}

		const IMedium* Medium() const
		{
			return job ? job->GetScene()->GetGlobalMedium() : nullptr;
		}

		Scalar MeanRayCasterNM( const unsigned int seed, const Scalar nm ) const
		{
			RandomNumberGenerator rng( seed );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			const RasterizerState rast = { 0, 0 };
			const Ray ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
			IRayCaster::RAY_STATE state;
			state.volumeBounces = 64;
			Scalar sum = 0;
			for( unsigned int i = 0; i < kSamples; ++i ) {
				Scalar value = 0;
				caster->CastRayNM( rc, rast, ray, value, state, nm, nullptr, nullptr );
				sum += value;
			}
			return sum / Scalar( kSamples );
		}

		Scalar MeanPathTracingNM( const unsigned int seed, const Scalar nm ) const
		{
			RandomNumberGenerator rng( seed );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			IndependentSampler sampler( rng );
			const RasterizerState rast = { 0, 0 };
			const Ray ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
			Scalar sum = 0;
			for( unsigned int i = 0; i < kSamples; ++i ) {
				sum += integrator->IntegrateRayNM( rc, rast, ray, nm,
					*job->GetScene(), *caster, sampler, nullptr, nullptr );
			}
			return sum / Scalar( kSamples );
		}

		Scalar MeanRayCasterNMWithSegmentState(
			const unsigned int seed, const Scalar nm,
			const VolumeEmissionSegmentState& segmentState ) const
		{
			const VolumeEmissionSegmentStateScope stateScope(segmentState);
			return MeanRayCasterNM(seed,nm);
		}

		Scalar MeanPathTracingNMWithSegmentState(
			const unsigned int seed, const Scalar nm,
			const VolumeEmissionSegmentState& segmentState ) const
		{
			const VolumeEmissionSegmentStateScope stateScope(segmentState);
			return MeanPathTracingNM(seed,nm);
		}

		void MeanPathTracingHWSS(
			const unsigned int seed,
			const SampledWavelengths& wavelengths,
			Scalar means[SampledWavelengths::N] ) const
		{
			RandomNumberGenerator rng( seed );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			IndependentSampler sampler( rng );
			const RasterizerState rast = { 0, 0 };
			const Ray ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
			for( unsigned int w = 0; w < SampledWavelengths::N; ++w ) means[w] = 0;
			for( unsigned int i = 0; i < kSamples; ++i ) {
				SampledWavelengths swl = wavelengths;
				Scalar values[SampledWavelengths::N];
				integrator->IntegrateRayHWSS( rc, rast, ray, swl,
					*job->GetScene(), *caster, sampler, nullptr, values, nullptr );
				for( unsigned int w = 0; w < SampledWavelengths::N; ++w ) means[w] += values[w];
			}
			for( unsigned int w = 0; w < SampledWavelengths::N; ++w ) means[w] /= Scalar( kSamples );
		}
	};

	Scalar ExpectedSlab( const Fixture& fixture, const Scalar nm )
	{
		const IMedium* medium = fixture.Medium();
		const MediumCoefficientsNM coeff = medium->GetCoefficientsNM(
			Point3( 0, 0, 0.5 * fixture.slabLength ), nm );
		const Scalar tau = coeff.sigma_t * fixture.slabLength;
		return PlanckSpectralRadianceNM( nm, kTemperatureK ) * ( -std::expm1( -tau ) );
	}

	class SpatialAdditiveMedium :
		public virtual IMedium,
		public virtual Reference
	{
	public:
		MediumCoefficients GetCoefficients( const Point3& pt ) const override
		{
			MediumCoefficients c;
			c.sigma_t = RISEPel( 0, 0, 0 );
			c.sigma_s = RISEPel( 0, 0, 0 );
			c.emission = RISEPel( SourceAt( pt ), SourceAt( pt ), SourceAt( pt ) );
			return c;
		}
		MediumCoefficientsNM GetCoefficientsNM(
			const Point3& pt, const Scalar ) const override
		{
			MediumCoefficientsNM c;
			c.sigma_t = 0;
			c.sigma_s = 0;
			c.emission = SourceAt( pt );
			return c;
		}
		const IPhaseFunction* GetPhaseFunction() const override { return nullptr; }
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
		bool IsHomogeneous() const override { return false; }
		bool GetBoundingBox( Point3& bbMin, Point3& bbMax ) const override
		{
			bbMin = Point3( -0.5, -0.5, 0 );
			bbMax = Point3( 0.5, 0.5, 1 );
			return true;
		}

	protected:
		~SpatialAdditiveMedium() override = default;

	private:
		static Scalar SourceAt( const Point3& pt )
		{
			return pt.z >= 0.5 && pt.z <= 1.0 ? 2.0 : 0.0;
		}
	};

	class ForwardPhase :
		public virtual IPhaseFunction,
		public virtual Reference
	{
	public:
		Scalar Evaluate( const Vector3&, const Vector3& ) const override { return 1.0; }
		Vector3 Sample( const Vector3& wi, ISampler& ) const override { return wi; }
		Scalar Pdf( const Vector3&, const Vector3& ) const override { return 1.0; }
	protected:
		~ForwardPhase() override = default;
	};

	class UnsupportedHomogeneousSmoke :
		public virtual IMedium,
		public virtual Reference
	{
	public:
		UnsupportedHomogeneousSmoke(
			IPhaseFunction& phase,
			const Scalar sigmaA,
			const Scalar sigmaS ) :
		  pPhase_(&phase), sigmaT_(sigmaA+sigmaS), sigmaS_(sigmaS)
		{
			pPhase_->addref();
		}

		MediumCoefficients GetCoefficients( const Point3& ) const override
		{
			MediumCoefficients c;
			c.sigma_t = RISEPel(sigmaT_,sigmaT_,sigmaT_);
			c.sigma_s = RISEPel(sigmaS_,sigmaS_,sigmaS_);
			c.emission = RISEPel(0,0,0);
			return c;
		}
		MediumCoefficientsNM GetCoefficientsNM(
			const Point3&, const Scalar ) const override
		{
			MediumCoefficientsNM c;
			c.sigma_t = sigmaT_;
			c.sigma_s = sigmaS_;
			c.emission = 0.0;
			return c;
		}
		const IPhaseFunction* GetPhaseFunction() const override { return pPhase_; }
		Scalar SampleDistance(
			const Ray&, const Scalar maxDist, ISampler& sampler,
			bool& scattered ) const override
		{
			if( sigmaT_ <= 0.0 ) {
				scattered = false;
				return maxDist;
			}
			const Scalar t = -std::log1p(-sampler.Get1D())/sigmaT_;
			scattered = t < maxDist;
			return scattered ? t : maxDist;
		}
		Scalar SampleDistanceNM(
			const Ray& ray, const Scalar maxDist, const Scalar,
			ISampler& sampler, bool& scattered ) const override
		{
			return SampleDistance(ray,maxDist,sampler,scattered);
		}
		RISEPel EvalTransmittance( const Ray&, const Scalar dist ) const override
		{
			const Scalar tr = std::exp(-sigmaT_*dist);
			return RISEPel(tr,tr,tr);
		}
		Scalar EvalTransmittanceNM(
			const Ray&, const Scalar dist, const Scalar ) const override
		{
			return std::exp(-sigmaT_*dist);
		}
		bool IsHomogeneous() const override { return true; }

	protected:
		~UnsupportedHomogeneousSmoke() override { safe_release(pPhase_); }

	private:
		IPhaseFunction* pPhase_;
		const Scalar sigmaT_;
		const Scalar sigmaS_;
	};

	class VolumeStateProbeShader :
		public virtual IShader,
		public virtual Reference
	{
	public:
		mutable bool sawCompetition;
		VolumeStateProbeShader() : sawCompetition(false) {}
		void Shade(
			const RuntimeContext&, const RayIntersection&, const IRayCaster&,
			const IRayCaster::RAY_STATE&, RISEPel& c,
			const IORStack& ) const override
		{
			sawCompetition = CurrentVolumeEmissionSegmentState().competitionAvailable;
			c = sawCompetition ? RISEPel(0,0,0) : RISEPel(1,1,1);
		}
		Scalar ShadeNM(
			const RuntimeContext&, const RayIntersection&, const IRayCaster&,
			const IRayCaster::RAY_STATE&, const Scalar,
			const IORStack& ) const override
		{
			sawCompetition = CurrentVolumeEmissionSegmentState().competitionAvailable;
			return sawCompetition ? 0.0 : 1.0;
		}
		void ResetRuntimeData() const override {}

	protected:
		~VolumeStateProbeShader() override = default;
	};

	std::string IsolatedSmokeReceiverScene()
	{
		const Scalar carbon = kTargetSigmaSI / HotAbsorptionMass633();
		std::ostringstream scene;
		scene << std::setprecision(17) <<
			"RISE ASCII SCENE 7\nstandard_shader\n{\nname global\n}\n"
			"null_boundary_material\n{\nname medium_boundary\n}\n"
			"homogeneous_medium\n{\nname receiver_smoke\n"
			"absorption 0.9 0.9 0.9\nscattering 0.1 0.1 0.1\nphase isotropic\n}\n"
			"scalar_painter\n{\nname carbon\nvalue " << carbon << "\n}\n"
			"scalar_painter\n{\nname temperature\nvalue " << kTemperatureK << "\n}\n"
			"multichannel_heterogeneous_medium\n{\nname fire\n"
			"channel_carbon painter carbon\nchannel_temperature painter temperature\n"
			"bake_resolution 4 4 4\nbbox_min 0.25 -0.5 -0.5\n"
			"bbox_max 1.25 0.5 0.5\nsoot_em 0.26\nsoot_density 1800\n"
			"soot_albedo_hot 0\nsoot_g_hot 0.5\nsmoke_km_carbon 8.7\n"
			"smoke_n_carbon 1.2\nsmoke_albedo_carbon 0.6\nsmoke_g_carbon 0.6\n}\n"
			"global_medium\n{\nmedium receiver_smoke\n}\n"
			"sphere_geometry\n{\nname fire_box_geometry\nradius 1\n}\n"
			"standard_object\n{\nname fire_box\ngeometry fire_box_geometry\n"
			"material medium_boundary\ninterior_medium fire\nposition 0.75 0 0\n"
			"casts_shadows FALSE\n}\n"
			"box_geometry\n{\nname wall_geometry\nwidth 8\nheight 8\ndepth 0.2\n}\n"
			"standard_object\n{\nname wall\ngeometry wall_geometry\nmaterial none\n"
			"position 0 0 2.1\n}\n";
		return scene.str();
	}

	std::string SurfaceFireReceiverScene( const bool positionalLight = false )
	{
		const Scalar carbon = kTargetSigmaSI / HotAbsorptionMass633();
		const Point3 fireMin = positionalLight ?
			Point3(0.05,-2.0,-2.0) : Point3(0.3,-0.45,-0.9);
		const Point3 fireMax = positionalLight ?
			Point3(2.5,2.0,-0.11) : Point3(1.2,0.45,-0.25);
		std::ostringstream scene;
		scene << std::setprecision(17) <<
			"RISE ASCII SCENE 7\nstandard_shader\n{\nname global\n"
			"shaderop DefaultPathTracing\n}\n"
			"uniformcolor_painter\n{\nname white\ncolor 0.8 0.8 0.8\n}\n"
			"lambertian_material\n{\nname receiver\nreflectance white\n}\n"
			"scalar_painter\n{\nname carbon\nvalue " << carbon << "\n}\n"
			"scalar_painter\n{\nname temperature\nvalue " <<
			(positionalLight ? 2600.0 : kTemperatureK) << "\n}\n"
			"multichannel_heterogeneous_medium\n{\nname fire\n"
			"channel_carbon painter carbon\nchannel_temperature painter temperature\n"
			"bake_resolution 4 4 4\nbbox_min " << fireMin.x << " " <<
			fireMin.y << " " << fireMin.z << "\nbbox_max " << fireMax.x << " " <<
			fireMax.y << " " << fireMax.z <<
			"\nsoot_em 0.26\nsoot_density 1800\n"
			"soot_albedo_hot 0\nsoot_g_hot 0.5\nsmoke_km_carbon 8.7\n"
			"smoke_n_carbon 1.2\nsmoke_albedo_carbon 0\nsmoke_g_carbon 0.6\n}\n"
			"global_medium\n{\nmedium fire\n}\n"
			"box_geometry\n{\nname receiver_geometry\nwidth 8\nheight 8\ndepth 0.2\n}\n"
			"standard_object\n{\nname receiver_wall\ngeometry receiver_geometry\n"
			"material receiver\nposition 0 0 0\n}\n";
		if( positionalLight ) {
			scene <<
				"omni_light\n{\nname point_competitor\npower 0.001\n"
				"color 0.000001 0.000001 0.000001\nposition -0.75 0 -0.5\n}\n";
		}
		return scene.str();
	}

	struct PhaseClosureAudit
	{
		unsigned int nextId;
		unsigned int makeCalls;
		unsigned int destroyCalls;
		unsigned int legacyLookups;
		unsigned int evaluateCalls;
		unsigned int evaluateId;
		unsigned int sampleId;
		unsigned int pdfId;
		unsigned int meanCosineId;
		bool mixedInstanceUse;
		std::vector<Point3> positions;
		std::vector<Scalar> wavelengths;

		PhaseClosureAudit() { Reset(); }

		void Reset()
		{
			nextId = 0;
			makeCalls = 0;
			destroyCalls = 0;
			legacyLookups = 0;
			evaluateCalls = 0;
			evaluateId = 0;
			sampleId = 0;
			pdfId = 0;
			meanCosineId = 0;
			mixedInstanceUse = false;
			positions.clear();
			wavelengths.clear();
		}

		void Record( unsigned int& recordedId, const unsigned int id )
		{
			if( recordedId == 0 ) {
				recordedId = id;
			} else if( recordedId != id ) {
				mixedInstanceUse = true;
			}
		}
	};

	class AuditedPhaseClosure :
		public virtual IPhaseFunction,
		public virtual Reference
	{
	public:
		AuditedPhaseClosure( PhaseClosureAudit& audit, const unsigned int id ) :
		  m_audit( audit ), m_id( id )
		{}

		Scalar Evaluate( const Vector3&, const Vector3& ) const override
		{
			++m_audit.evaluateCalls;
			m_audit.Record( m_audit.evaluateId, m_id );
			return 1.0 / FOUR_PI;
		}

		Vector3 Sample( const Vector3&, ISampler& ) const override
		{
			m_audit.Record( m_audit.sampleId, m_id );
			return Vector3( 1, 0, 0 );
		}

		Scalar Pdf( const Vector3&, const Vector3& ) const override
		{
			m_audit.Record( m_audit.pdfId, m_id );
			return 1.0 / FOUR_PI;
		}

		Scalar GetMeanCosine() const override
		{
			m_audit.Record( m_audit.meanCosineId, m_id );
			return 0.5;
		}

	protected:
		~AuditedPhaseClosure() override { ++m_audit.destroyCalls; }

	private:
		PhaseClosureAudit& m_audit;
		const unsigned int m_id;
	};

	class AuditedFireMedium :
		public virtual IMedium,
		public virtual Reference
	{
	public:
		explicit AuditedFireMedium( PhaseClosureAudit& audit ) : m_audit( audit ) {}

		MediumCoefficients GetCoefficients( const Point3& ) const override
		{
			MediumCoefficients c;
			c.sigma_t = RISEPel( 1, 1, 1 );
			c.sigma_s = RISEPel( 1, 1, 1 );
			c.emission = RISEPel( 0, 0, 0 );
			return c;
		}

		MediumCoefficientsNM GetCoefficientsNM( const Point3& pt, const Scalar ) const override
		{
			MediumCoefficientsNM c;
			const bool onCameraLine = std::fabs( std::fabs( pt.x ) - 0.5 ) < 1e-13 &&
				std::fabs( pt.y ) < 1e-13;
			c.sigma_t = onCameraLine ? 1.0 : 0.0;
			c.sigma_s = onCameraLine ? 1.0 : 0.0;
			c.emission = 0.0;
			return c;
		}

		const IPhaseFunction* GetPhaseFunction() const override
		{
			++m_audit.legacyLookups;
			return nullptr;
		}

		const IPhaseFunction* MakePhaseClosure(
			const Point3& pt, const Scalar nm ) const override
		{
			++m_audit.makeCalls;
			m_audit.positions.push_back( pt );
			m_audit.wavelengths.push_back( nm );
			return new AuditedPhaseClosure( m_audit, ++m_audit.nextId );
		}

		Scalar SampleDistance(
			const Ray&, const Scalar maxDist, ISampler&, bool& scattered ) const override
		{
			scattered = false;
			return maxDist;
		}

		Scalar SampleDistanceNM(
			const Ray& ray, const Scalar maxDist, const Scalar,
			ISampler&, bool& scattered ) const override
		{
			if( std::fabs( ray.origin.z ) < 1e-15 ) {
				scattered = true;
				return 0.25;
			}
			scattered = false;
			return maxDist;
		}

		RISEPel EvalTransmittance( const Ray&, const Scalar dist ) const override
		{
			const Scalar tr = std::exp( -dist );
			return RISEPel( tr, tr, tr );
		}

		Scalar EvalTransmittanceNM(
			const Ray&, const Scalar dist, const Scalar ) const override
		{
			return std::exp( -dist );
		}

		bool IsHomogeneous() const override { return false; }
		bool IsFireMedium() const override { return true; }

		bool GetBoundingBox( Point3& bbMin, Point3& bbMax ) const override
		{
			bbMin = Point3( -2, -2, 0 );
			bbMax = Point3( 2, 2, 1 );
			return true;
		}

	protected:
		~AuditedFireMedium() override = default;

	private:
		PhaseClosureAudit& m_audit;
	};

	class TwoEventFireMedium :
		public virtual IMedium,
		public virtual Reference
	{
	public:
		explicit TwoEventFireMedium( IPhaseFunction& phase ) : pPhase( &phase )
		{
			pPhase->addref();
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
			c.sigma_t = 1.0;
			c.sigma_s = 1.0;
			c.emission = 0.0;
			return c;
		}
		const IPhaseFunction* GetPhaseFunction() const override { return nullptr; }
		const IPhaseFunction* MakePhaseClosure(
			const Point3&, const Scalar ) const override
		{
			pPhase->addref();
			return pPhase;
		}
		Scalar SampleDistance(
			const Ray&, const Scalar maxDist, ISampler&, bool& scattered ) const override
		{
			scattered = false;
			return maxDist;
		}
		Scalar SampleDistanceNM(
			const Ray& ray, const Scalar maxDist, const Scalar,
			ISampler&, bool& scattered ) const override
		{
			if( ray.origin.z < 0.1 ) {
				scattered = true;
				return 0.25;
			}
			if( ray.origin.z < 0.5 ) {
				scattered = true;
				return 0.5;
			}
			scattered = false;
			return maxDist;
		}
		RISEPel EvalTransmittance( const Ray&, const Scalar dist ) const override
		{
			const Scalar tr = std::exp( -dist );
			return RISEPel( tr, tr, tr );
		}
		Scalar EvalTransmittanceNM(
			const Ray&, const Scalar dist, const Scalar ) const override
		{
			return std::exp( -dist );
		}
		bool IsHomogeneous() const override { return false; }
		bool GetBoundingBox( Point3& bbMin, Point3& bbMax ) const override
		{
			bbMin = Point3( -1, -1, 0 );
			bbMax = Point3( 1, 1, 1 );
			return true;
		}
		bool IsFireMedium() const override { return true; }
		Scalar GetThermalEmissionNM( const Point3& pt, const Scalar ) const override
		{
			return pt.z > 0.5 ? 1.0 : 0.0;
		}

	protected:
		~TwoEventFireMedium() override { safe_release( pPhase ); }

	private:
		IPhaseFunction* pPhase;
	};

	class DelayedDensityAccessor :
		public virtual IVolumeAccessor,
		public virtual Reference
	{
	public:
		bool tracking;
		mutable unsigned int zeroDensityQueries;

		DelayedDensityAccessor() : tracking( false ), zeroDensityQueries( 0 ) {}

		Scalar GetValue( Scalar, Scalar, Scalar z ) const override
		{
			if( tracking && z < 0.6 ) {
				++zeroDensityQueries;
				return 0.0;
			}
			return 1.0;
		}
		Scalar GetValue( int, int, int ) const override { return 1.0; }
		void BindVolume( const IVolume* ) override {}

	protected:
		~DelayedDensityAccessor() override = default;
	};

	class DelayedThermalHeterogeneousMedium : public HeterogeneousMedium
	{
	public:
		DelayedThermalHeterogeneousMedium(
			const IPhaseFunction& phase,
			IVolumeAccessor& accessor
			) : HeterogeneousMedium(
				RISEPel( 1000, 1000, 1000 ), RISEPel( 0, 0, 0 ),
				phase, accessor, 2, 2, 2,
				Point3( -1, -1, 0 ), Point3( 1, 1, 2 ) )
		{}

		bool IsFireMedium() const override { return true; }
		Scalar GetThermalEmissionNM( const Point3& pt, const Scalar nm ) const override
		{
			return 3.0 * HeterogeneousMedium::GetCoefficientsNM( pt, nm ).sigma_t;
		}

	protected:
		~DelayedThermalHeterogeneousMedium() override = default;
	};

	class LogTailFireMedium :
		public virtual IMedium,
		public virtual Reference
	{
	public:
		mutable Scalar lastLogEventDistance;
		LogTailFireMedium() : lastLogEventDistance( -1.0 ) {}
		static Scalar EventDistance() { return 1.0e31; }
		static Scalar SigmaT() { return 99.0 / EventDistance(); }

		MediumCoefficients GetCoefficients( const Point3& ) const override
		{
			MediumCoefficients c;
			c.sigma_t = RISEPel( SigmaT(), SigmaT(), SigmaT() );
			c.sigma_s = RISEPel( 0, 0, 0 );
			c.emission = RISEPel( 0, 0, 0 );
			return c;
		}
		MediumCoefficientsNM GetCoefficientsNM( const Point3&, const Scalar ) const override
		{
			MediumCoefficientsNM c;
			c.sigma_t = SigmaT();
			c.sigma_s = 0.0;
			c.emission = 0.0;
			return c;
		}
		const IPhaseFunction* GetPhaseFunction() const override { return nullptr; }
		Scalar SampleDistance(
			const Ray&, const Scalar maxDist, ISampler&, bool& scattered ) const override
		{
			scattered = false;
			return maxDist;
		}
		Scalar SampleDistanceNM(
			const Ray&, const Scalar, const Scalar,
			ISampler&, bool& scattered ) const override
		{
			scattered = true;
			return EventDistance();
		}
		RISEPel EvalTransmittance( const Ray&, const Scalar dist ) const override
		{
			const Scalar tr = std::exp( -SigmaT() * dist );
			return RISEPel( tr, tr, tr );
		}
		Scalar EvalTransmittanceNM(
			const Ray&, const Scalar dist, const Scalar ) const override
		{
			return std::exp( -SigmaT() * dist );
		}
		bool IsHomogeneous() const override { return false; }
		bool GetBoundingBox( Point3& bbMin, Point3& bbMax ) const override
		{
			bbMin = Point3( -1, -1, 0 );
			bbMax = Point3( 1, 1, 1.1 * EventDistance() );
			return true;
		}
		bool IsFireMedium() const override { return true; }
		Scalar GetThermalEmissionNM( const Point3&, const Scalar ) const override
		{
			return SigmaT();
		}
		Scalar EvalDistancePdfNM(
			const Ray&, const Scalar, const bool,
			const Scalar, const Scalar ) const override
		{
			// Deliberately model the historical linear-domain floor.  Correct
			// PT consumes the separate log-density contract below, so this stale
			// compatibility value cannot suppress the measurable tail.
			return 1.0e-30;
		}
		Scalar EvalLogDistancePdfNM(
			const Ray&, const Scalar t, const bool scattered,
			const Scalar maxDist, const Scalar ) const override
		{
			if( scattered ) lastLogEventDistance = t;
			return scattered ? std::log( SigmaT() ) - SigmaT() * t :
				-SigmaT() * maxDist;
		}
	protected:
		~LogTailFireMedium() override = default;
	};

	class UnboundedDistanceProbeMedium : public LogTailFireMedium
	{
	public:
		mutable unsigned int plainCalls;
		mutable unsigned int mixedCalls;
		UnboundedDistanceProbeMedium() : plainCalls(0), mixedCalls(0) {}
		MediumCoefficientsNM GetCoefficientsNM(
			const Point3&, const Scalar ) const override
		{
			MediumCoefficientsNM result;
			result.sigma_t = 0.0;
			result.sigma_s = 0.0;
			result.emission = 0.0;
			return result;
		}
		Scalar SampleDistanceNM(
			const Ray&, const Scalar maxDist, const Scalar,
			ISampler&, bool& scattered ) const override
		{
			++plainCalls;
			scattered = false;
			return maxDist;
		}
		DistanceSample SampleDistanceWithPdfNM(
			const Ray&, const Scalar maxDist, const Scalar,
			ISampler& ) const override
		{
			++mixedCalls;
			DistanceSample result;
			result.t = maxDist;
			result.scattered = false;
			result.pdf = 1.0;
			return result;
		}
		Scalar EvalDistancePdfNM(
			const Ray&, const Scalar, const bool,
			const Scalar, const Scalar ) const override
		{
			return 1.0;
		}
		Scalar EvalLogDistancePdfNM(
			const Ray&, const Scalar, const bool,
			const Scalar, const Scalar ) const override
		{
			return 0.0;
		}
		bool GetBoundingBox( Point3&, Point3& ) const override { return false; }
		bool IsFireMedium() const override { return false; }
		Scalar GetThermalEmissionNM( const Point3&, const Scalar ) const override
		{
			return 0.0;
		}
	protected:
		~UnboundedDistanceProbeMedium() override = default;
	};

	void TestNMAbsoluteParityAndSceneUnits()
	{
		std::cout << "TestNMAbsoluteParityAndSceneUnits" << std::endl;
		Fixture metres;
		Fixture centimetres;
		Check( metres.Initialize( "metres", 1.0, 1.0 ), "metre PT fire slab initializes" );
		Check( centimetres.Initialize( "centimetres", 0.01, 100.0 ),
			"centimetre PT fire slab initializes" );
		if( !metres.integrator || !centimetres.integrator ) return;

		const Scalar wavelengths[] = { 500.0, 700.0 };
		for( const Scalar nm : wavelengths ) {
			const Scalar expected = ExpectedSlab( metres, nm );
			const Scalar rayCaster = metres.MeanRayCasterNM( 0x51ab00u + static_cast<unsigned int>( nm ), nm );
			const Scalar pathTracing = metres.MeanPathTracingNM( 0x51ab00u + static_cast<unsigned int>( nm ), nm );
			const Scalar pathTracingCM = centimetres.MeanPathTracingNM(
				0x51ab00u + static_cast<unsigned int>( nm ), nm );
			Check( NearRelative( pathTracing, expected, 0.012 ),
				"PT NM pure-absorber slab matches absolute spectral target at maxVolumeBounce=0" );
			Check( NearRelative( pathTracing, rayCaster, 0.012 ),
				"PT NM and RayCaster entry routes produce the same spectral radiance" );
			Check( NearRelative( pathTracingCM, pathTracing, 0.012 ),
				"PT NM fire radiance is invariant between metre and centimetre scenes" );
		}
	}

	void TestHWSSRequestedUsesPerWavelengthFallback()
	{
		std::cout << "TestHWSSRequestedUsesPerWavelengthFallback" << std::endl;
		Fixture fixture;
		Check( fixture.Initialize( "hwss", 1.0, 1.0 ), "HWSS-requested PT fire slab initializes" );
		if( !fixture.integrator ) return;

		const SampledWavelengths wavelengths = SampledWavelengths::SampleEquidistant(
			0.30, 380.0, 780.0 );
		Scalar means[SampledWavelengths::N];
		fixture.MeanPathTracingHWSS( 0x48575353u, wavelengths, means );
		for( unsigned int w = 0; w < SampledWavelengths::N; ++w ) {
			const Scalar expected = ExpectedSlab( fixture, wavelengths.lambda[w] );
			Check( NearRelative( means[w], expected, 0.016 ),
				"HWSS-requested fire path matches its per-wavelength NM slab target" );
		}
	}

	void TestEquiangularMixtureUsesItsActualDistanceDensity()
	{
		std::cout << "TestEquiangularMixtureUsesItsActualDistanceDensity" << std::endl;
		Fixture fixture;
		Check( fixture.Initialize( "equiangular", 1.0, 1.0, true ),
			"positional-light PT fire slab initializes" );
		if( !fixture.integrator ) return;
		const Scalar nm = 500.0;
		const Scalar expected = ExpectedSlab( fixture, nm );
		const Scalar measured = fixture.MeanPathTracingNM( 0xe9a117u, nm );
		Check( NearRelative( measured, expected, 0.018 ),
			"PT thermal score uses the active DT/equiangular mixture density" );
	}

	void TestEquiangularBoundednessAndCollinearFallback()
	{
		std::cout << "TestEquiangularBoundednessAndCollinearFallback" << std::endl;
		const Ray ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
		const Scalar tNear = 0.0;
		const Scalar tFar = 10.0;
		const Scalar threshold = 32.0 *
			std::sqrt( std::numeric_limits<Scalar>::epsilon() ) * (tFar-tNear);
		const Point3 onRay( 0, 0, 5 );
		const EquiangularSampling::Sample onRaySample =
			EquiangularSampling::SampleDistance(
				ray, onRay, tNear, tFar, true, 0.63 );
		Check( NearRelative( onRaySample.t, 6.3, 1e-14 ) &&
			NearRelative( onRaySample.pdf, 0.1, 1e-14 ) &&
			NearRelative( IntegrateEquiangularPdf(
				ray, onRay, tNear, tFar, 1000 ), 1.0, 1e-14 ),
			"on-ray bounded segment has a finite normalized uniform density" );

		const Point3 underThreshold( 0.5*threshold, 0, 5 );
		const EquiangularSampling::Sample uniformSample =
			EquiangularSampling::SampleDistance(
				ray, underThreshold, tNear, tFar, true, 0.37 );
		Check( NearRelative( uniformSample.t, 3.7, 1e-14 ) &&
			NearRelative( uniformSample.pdf, 0.1, 1e-14 ) &&
			NearRelative( uniformSample.pdf, EquiangularSampling::Pdf(
				ray, underThreshold, tNear, tFar, true, uniformSample.t ), 1e-14 ),
			"near-collinear bounded segment uses the shared uniform Sample/Pdf branch" );

		const Point3 overThreshold( 2.0*threshold, 0, 5 );
		const EquiangularSampling::Sample angularSample =
			EquiangularSampling::SampleDistance(
				ray, overThreshold, tNear, tFar, true, 0.37 );
		Check( angularSample.pdf > 0.0 &&
			!NearRelative( angularSample.pdf, 0.1, 1e-6 ) &&
			NearRelative( angularSample.pdf, EquiangularSampling::Pdf(
				ray, overThreshold, tNear, tFar, true, angularSample.t ), 1e-12 ),
			"above-threshold segment uses one robust geometry in Sample and Pdf" );

		const Scalar unboundedFar = RISE_INFINITY;
		const EquiangularSampling::Sample unboundedSample =
			EquiangularSampling::SampleDistance(
				ray, Point3( 1, 0, 5 ), tNear, unboundedFar, false, 0.5 );
		Check( unboundedSample.pdf == 0.0 && EquiangularSampling::Pdf(
			ray, Point3( 1, 0, 5 ), tNear, unboundedFar, false, 5.0 ) == 0.0,
			"explicitly unbounded segment cannot enter the equiangular technique" );

		Check( NearRelative( IntegrateEquiangularPdf(
			ray, Point3( 3, 0, 4 ), tNear, tFar, 200000 ), 1.0, 2e-10 ),
			"bounded equiangular PDF integrates to one" );
		const Ray largeRay(
			Point3( 1e200, -2e200, 3e200 ), Vector3( 0, 0, 1 ) );
		const Point3 largePivot( 1.3e200, -2e200, 3.4e200 );
		const EquiangularSampling::Sample largeSample =
			EquiangularSampling::SampleDistance(
				largeRay, largePivot, 0.0, 1e200, true, 0.37 );
		Check( largeSample.pdf > 0.0 &&
			NearRelative( largeSample.pdf, EquiangularSampling::Pdf(
				largeRay, largePivot, 0.0, 1e200, true, largeSample.t ), 1e-12 ) &&
			NearRelative( IntegrateEquiangularPdf(
				largeRay, largePivot, 0.0, 1e200, 200000 ), 1.0, 2e-10 ),
			"equiangular Sample/Pdf stays normalized above square-overflow scale" );

		const Ray tinyRay( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
		const Point3 tinyPivot( 1e-201, 0, 5e-201 );
		const EquiangularSampling::Sample tinySample =
			EquiangularSampling::SampleDistance(
				tinyRay, tinyPivot, 0.0, 1e-200, true, 0.63 );
		Check( tinySample.pdf > 0.0 &&
			!NearRelative( tinySample.pdf, 1e200, 1e-6 ) &&
			NearRelative( tinySample.pdf, EquiangularSampling::Pdf(
				tinyRay, tinyPivot, 0.0, 1e-200, true, tinySample.t ), 1e-12 ) &&
			NearRelative( IntegrateEquiangularPdf(
				tinyRay, tinyPivot, 0.0, 1e-200, 200000 ), 1.0, 2e-10 ),
			"equiangular Sample/Pdf stays normalized below square-underflow scale" );

		const Point3 farPivot( 1e6, 0, 1e12 );
		const EquiangularSampling::Sample farSample =
			EquiangularSampling::SampleDistance(
				ray, farPivot, 0.0, 1.0, true, 0.37 );
		Check( farSample.pdf > 0.0 &&
			NearRelative( farSample.t, 0.37, 1e-9 ) &&
			NearRelative( farSample.pdf, EquiangularSampling::Pdf(
				ray, farPivot, 0.0, 1.0, true, farSample.t ), 1e-12 ) &&
			NearRelative( IntegrateEquiangularPdf(
				ray, farPivot, 0.0, 1.0, 200000 ), 1.0, 2e-10 ),
			"far-pivot short segment avoids angular-span cancellation" );

		const Scalar dualLength = 1e-308;
		const Point3 dualScalePivot( 1e10, 0, 1e16 );
		const EquiangularSampling::Sample dualScaleSample =
			EquiangularSampling::SampleDistance(
				ray, dualScalePivot, 0.0, dualLength, true, 0.37 );
		const Scalar dualMidPdf = EquiangularSampling::Pdf(
			ray, dualScalePivot, 0.0, dualLength, true, 0.5*dualLength );
		Check( dualScaleSample.pdf > 0.0 && dualMidPdf > 0.0 &&
			NearRelative( dualScaleSample.t/dualLength, 0.37, 1e-12 ) &&
			NearRelative( dualScaleSample.pdf, EquiangularSampling::Pdf(
				ray, dualScalePivot, 0.0, dualLength,
				true, dualScaleSample.t ), 1e-12 ) &&
			NearRelative( dualMidPdf*dualLength, 1.0, 1e-12 ),
			"mixed-scale angular span retains its normalized binary exponent" );

		const Point3 scaleBasePivot( 1e8, 0, 2e14 );
		const Point3 scaleTinyPivot( 1e-300, 0, 2e-294 );
		const Scalar scaleTinyLength = 1e-308;
		const Scalar quantiles[] = { 0.1, 0.37, 0.5, 0.83 };
		bool scaleInvariant = true;
		for( const Scalar xi : quantiles ) {
			const EquiangularSampling::Sample base =
				EquiangularSampling::SampleDistance(
					ray, scaleBasePivot, 0.0, 1.0, true, xi );
			const EquiangularSampling::Sample scaled =
				EquiangularSampling::SampleDistance(
					ray, scaleTinyPivot, 0.0, scaleTinyLength, true, xi );
			scaleInvariant = scaleInvariant && base.pdf > 0.0 && scaled.pdf > 0.0 &&
				NearRelative( scaled.t/scaleTinyLength, base.t, 1e-12 ) &&
				NearRelative( scaled.pdf*scaleTinyLength, base.pdf, 1e-12 );
		}
		Check( scaleInvariant,
			"equiangular CDF quantiles and density are invariant at 1e-308 scale" );

		const Scalar subnormalSpanLength = 5e-10;
		const Point3 subnormalSpanPivot( 1e302, 0, 1e308 );
		bool subnormalSpanCorrect = true;
		for( const Scalar xi : quantiles ) {
			const EquiangularSampling::Sample sample =
				EquiangularSampling::SampleDistance(
					ray, subnormalSpanPivot, 0.0,
					subnormalSpanLength, true, xi );
			subnormalSpanCorrect = subnormalSpanCorrect && sample.pdf > 0.0 &&
				NearRelative( sample.t/subnormalSpanLength, xi, 1e-12 ) &&
				NearRelative( sample.pdf*subnormalSpanLength, 1.0, 1e-12 ) &&
				NearRelative( sample.pdf, EquiangularSampling::Pdf(
					ray, subnormalSpanPivot, 0.0, subnormalSpanLength,
					true, sample.t ), 1e-12 );
		}
		Check( subnormalSpanCorrect,
			"subnormal nonzero angular span preserves fractional CDF quantiles" );

		const Scalar boundaryLength = 2.23e6;
		const Scalar boundaryXi = 3.0/9007199254740992.0;
		const EquiangularSampling::Sample boundarySample =
			EquiangularSampling::SampleDistance(
				ray, subnormalSpanPivot, 0.0, boundaryLength,
				true, boundaryXi );
		Check( boundarySample.pdf > 0.0 &&
			NearRelative( boundarySample.t/(boundaryXi*boundaryLength), 1.0, 1e-12 ) &&
			NearRelative( boundarySample.pdf*boundaryLength, 1.0, 1e-12 ) &&
			NearRelative( boundarySample.pdf, EquiangularSampling::Pdf(
				ray, subnormalSpanPivot, 0.0, boundaryLength,
				true, boundarySample.t ), 1e-12 ),
			"subnormal fractional span preserves a 53-bit sampler quantile" );

		const Scalar oppositeLength = 1e308;
		const Point3 oppositePivot( 1e302, 0, -1e308 );
		const EquiangularSampling::Sample oppositeSample =
			EquiangularSampling::SampleDistance(
				ray, oppositePivot, 0.0, oppositeLength, true, 0.37 );
		Check( oppositeSample.pdf > 0.0 &&
			std::isfinite(oppositeSample.t) &&
			NearRelative( oppositeSample.pdf, EquiangularSampling::Pdf(
				ray, oppositePivot, 0.0, oppositeLength,
				true, oppositeSample.t ), 1e-12 ) &&
			NearRelative( IntegrateEquiangularPdf(
				ray, oppositePivot, 0.0, oppositeLength, 200000 ),
				1.0, 2e-10 ),
			"finite opposite-sign offsets remain normalized without overflow" );
	}

	void TestUnboundedGlobalMediumDisablesEquiangularBeforeTechniqueRoll()
	{
		std::cout << "TestUnboundedGlobalMediumDisablesEquiangularBeforeTechniqueRoll" << std::endl;
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() /
			( "rise_pt_unbounded_equiangular_" +
				std::to_string( static_cast<int>( ::getpid() ) ) + ".RISEscene" );
		{
			std::ofstream output( scenePath );
			output <<
				"RISE ASCII SCENE 7\nstandard_shader\n{\nname global\n}\n"
				"omni_light\n{\nname pivot\npower 1\ncolor 1 1 1\nposition 1 0 5\n}\n";
		}

		IJobPriv* job = nullptr;
		IRayCaster* caster = nullptr;
		PathTracingIntegrator* integrator = nullptr;
		UnboundedDistanceProbeMedium* medium = new UnboundedDistanceProbeMedium();
		Check( RISE_CreateJobPriv( &job ) && job &&
			job->LoadAsciiSceneViaCst( scenePath.string().c_str() ),
			"unbounded equiangular-disable fixture initializes" );
		if( job ) {
			job->GetScene()->SetGlobalMedium( medium );
			IShader* shader = job->GetShaders()->GetItem( "global" );
			Check( shader && RISE_API_CreateRayCaster(
				&caster, false, 0, *shader, true ) && caster,
				"unbounded equiangular-disable caster initializes" );
			if( caster ) caster->AttachScene( job->GetScene() );
			integrator = new PathTracingIntegrator(
				ManifoldSolverConfig(), StabilityConfig() );
		}

		if( integrator && caster && job ) {
			RandomNumberGenerator rng( 0x42u );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			IndependentSampler sampler( rng );
			const RasterizerState rast = { 0, 0 };
			integrator->IntegrateRayNM(
				rc, rast, Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ), 500.0,
				*job->GetScene(), *caster, sampler, nullptr, nullptr );
			Check( medium->plainCalls == 1 && medium->mixedCalls == 0,
				"PT unbounded global DBL_MAX segment takes pure DT before technique roll" );

			medium->plainCalls = 0;
			medium->mixedCalls = 0;
			RandomNumberGenerator casterRng( 0x43u );
			RuntimeContext casterRc(
				casterRng, RuntimeContext::PASS_NORMAL, false );
			IRayCaster::RAY_STATE state;
			Scalar value = 0.0;
			caster->CastRayNM(
				casterRc, rast, Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ),
				value, state, 500.0, nullptr, nullptr );
			Check( medium->plainCalls == 1 && medium->mixedCalls == 0,
				"RayCaster unbounded global DBL_MAX segment takes pure DT before technique roll" );
		}

		safe_release( integrator );
		safe_release( caster );
		safe_release( job );
		safe_release( medium );
		std::filesystem::remove( scenePath );
	}

	void TestFlameOnlySceneActivatesCombinedEquiangularSampler()
	{
		std::cout << "TestFlameOnlySceneActivatesCombinedEquiangularSampler" << std::endl;
		Fixture fixture;
		Check( fixture.Initialize("flame_only_equiangular",1.0,1.0,false),
			"flame-only equiangular fixture initializes without positional lights" );
		if( !fixture.job || !fixture.caster || !fixture.integrator ) return;

		UnboundedDistanceProbeMedium* probe = new UnboundedDistanceProbeMedium();
		fixture.job->GetScene()->SetGlobalMedium( probe );
		const RasterizerState rast = { 0, 0 };
		const Ray ray( Point3(0,0,0), Vector3(0,0,1) );

		RandomNumberGenerator ptRng( 0x71u );
		RuntimeContext ptRc( ptRng, RuntimeContext::PASS_NORMAL, false );
		IndependentSampler ptSampler( ptRng );
		fixture.integrator->IntegrateRayNM(
			ptRc, rast, ray, 500.0, *fixture.job->GetScene(),
			*fixture.caster, ptSampler, nullptr, nullptr );
		Check( probe->plainCalls == 0,
			"PT flame-only segment enters the combined equiangular mixture" );

		probe->plainCalls = 0;
		probe->mixedCalls = 0;
		RandomNumberGenerator casterRng( 0x72u );
		RuntimeContext casterRc( casterRng, RuntimeContext::PASS_NORMAL, false );
		IRayCaster::RAY_STATE state;
		Scalar value = 0.0;
		fixture.caster->CastRayNM(
			casterRc, rast, ray, value, state, 500.0, nullptr, nullptr );
		Check( probe->plainCalls == 0,
			"RayCaster flame-only segment enters the combined equiangular mixture" );

		safe_release( probe );
	}

	void TestPrimaryScatteringEventHonorsVolumeCapAfterEmission()
	{
		std::cout << "TestPrimaryScatteringEventHonorsVolumeCapAfterEmission" << std::endl;
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() /
			( "rise_isolated_smoke_receiver_" +
				std::to_string(static_cast<int>(::getpid())) + ".RISEscene" );
		{
			std::ofstream output(scenePath);
			output << IsolatedSmokeReceiverScene();
		}

		IJobPriv* job = nullptr;
		IRayCaster* caster = nullptr;
		IRayCaster* terminalCaster = nullptr;
		PathTracingIntegrator* cappedIntegrator = nullptr;
		PathTracingIntegrator* terminalIntegrator = nullptr;
		Check( RISE_CreateJobPriv(&job) && job &&
			job->LoadAsciiSceneViaCst(scenePath.string().c_str()),
			"isolated smoke receiver and off-axis fire fixture loads" );
		if( job ) {
			IShader* shader = job->GetShaders()->GetItem("global");
			Check( shader && RISE_API_CreateRayCaster(
				&caster,false,4,*shader,true) && caster &&
				RISE_API_CreateRayCaster(&terminalCaster,false,1,*shader,true) &&
				terminalCaster,
				"isolated receiver prepares ordinary and terminal RayCasters" );
			if( caster ) caster->AttachScene(job->GetScene());
			if( terminalCaster ) terminalCaster->AttachScene(job->GetScene());
			StabilityConfig cappedConfig;
			cappedConfig.maxVolumeBounce = 0;
			cappedIntegrator = new PathTracingIntegrator(
				ManifoldSolverConfig(),cappedConfig);
			StabilityConfig terminalConfig;
			terminalConfig.maxVolumeBounce = 2;
			terminalIntegrator = new PathTracingIntegrator(
				ManifoldSolverConfig(),terminalConfig);
			terminalIntegrator->SetMaxPathDepth(1);
		}

		if( job && caster && terminalCaster && cappedIntegrator && terminalIntegrator ) {
			const Scalar nm = 500.0;
			const unsigned int samples = 320000;
			const Ray ray(Point3(0,0,-1),Vector3(0,0,1));
			const RasterizerState rast = {0,0};
			auto meanPT = [&]( PathTracingIntegrator& integrator,
				const IRayCaster& route, const unsigned int seed ) {
				RandomNumberGenerator rng(seed);
				RuntimeContext rc(rng,RuntimeContext::PASS_NORMAL,false);
				IndependentSampler sampler(rng);
				Scalar sum = 0.0;
				for( unsigned int i=0; i<samples; ++i ) {
					sum += integrator.IntegrateRayNM(
						rc,rast,ray,nm,*job->GetScene(),route,sampler,
						nullptr,nullptr);
				}
				return sum/static_cast<Scalar>(samples);
			};
			auto meanRC = [&]( const IRayCaster& route,
				const unsigned int volumeBounces, const unsigned int seed,
				const Scalar importance = 1.0 ) {
				RandomNumberGenerator rng(seed);
				RuntimeContext rc(rng,RuntimeContext::PASS_NORMAL,false);
				IRayCaster::RAY_STATE state;
				state.volumeBounces = volumeBounces;
				state.importance = importance;
				Scalar sum = 0.0;
				for( unsigned int i=0; i<samples; ++i ) {
					Scalar value = 0.0;
					route.CastRayNM(
						rc,rast,ray,value,state,nm,nullptr,nullptr);
					sum += value;
				}
				return sum/static_cast<Scalar>(samples);
			};

			const IMedium* supportedSmoke = job->GetScene()->GetGlobalMedium();
			supportedSmoke->addref();
			const Scalar cappedOnPT = meanPT(*cappedIntegrator,*caster,0x5ca77e2u);
			const Scalar cappedOnRC = meanRC(*caster,64,0xca5700u);
			const Scalar terminalOnPT = meanPT(*terminalIntegrator,*terminalCaster,0x7e2d01u);
			// With sigma_s=0.1, importance=0.05, and the active 50/50
			// DT/equiangular distance mixture, Tr*sigma_s/combinedPdf is
			// positive and strictly below 0.2. The counterfactual ordinary
			// outgoing roulette survival is therefore strictly between zero and
			// one; the terminal A_march density must omit that varying factor.
			const Scalar terminalOnRC = meanRC(
				*terminalCaster,0,0x7e2d02u,0.05);

			IsotropicPhaseFunction* phase = new IsotropicPhaseFunction();
			UnsupportedHomogeneousSmoke* unsupportedSmoke =
				new UnsupportedHomogeneousSmoke(*phase,0.9,0.1);
			job->GetScene()->SetGlobalMedium(unsupportedSmoke);
			const Scalar cappedOffPT = meanPT(*cappedIntegrator,*caster,0x5ca77e2u);
			const Scalar cappedOffRC = meanRC(*caster,64,0xca5700u);
			const Scalar terminalOffPT = meanPT(*terminalIntegrator,*terminalCaster,0x7e2d03u);
			// The unsupported preview reference keeps RayCaster's legacy depth
			// semantics. Give it one ordinary scatter segment, then use the volume
			// cap at the reached fire collision so it is the same source-only
			// brute-force family without the volume-NEE estimator.
			const Scalar terminalOffRC = meanRC(*caster,63,0x7e2d04u);
			job->GetScene()->SetGlobalMedium(supportedSmoke);

			if( !(cappedOnPT > 0.0 && cappedOnRC > 0.0 &&
				terminalOffPT > 0.0 && terminalOffRC > 0.0) ) {
				std::cout << "  capped on/off PT=" << cappedOnPT << "/" << cappedOffPT <<
					" RC=" << cappedOnRC << "/" << cappedOffRC <<
					" terminal on/off PT=" << terminalOnPT << "/" << terminalOffPT <<
					" RC=" << terminalOnRC << "/" << terminalOffRC << std::endl;
			}
			Check( cappedOffPT==0.0 && cappedOffRC==0.0 &&
				cappedOnPT>0.0 && cappedOnRC>0.0,
				"per-type-capped smoke lobe is NEE-only in both production routes" );
			Check( NearRelative(cappedOnPT,cappedOnRC,0.06),
				"PT and RayCaster agree on the isolated per-type-capped NEE estimator" );
			const bool terminalPartitionAgrees = terminalOffPT>0.0 && terminalOffRC>0.0 &&
				NearRelative(terminalOnPT,terminalOffPT,0.10) &&
				NearRelative(terminalOnRC,terminalOffRC,0.10);
			if( !terminalPartitionAgrees ) {
				std::cout << "  terminal on/off PT=" << terminalOnPT << "/" << terminalOffPT <<
					" RC=" << terminalOnRC << "/" << terminalOffRC << std::endl;
			}
			Check( terminalPartitionAgrees,
				"total-depth A_march NEE-on and pure-march references agree" );
			Check( NearRelative(terminalOnPT,terminalOnRC,0.08),
				"PT and RayCaster agree on the terminal source-only MIS partition" );

			safe_release(unsupportedSmoke);
			safe_release(phase);
			safe_release(supportedSmoke);
		}

		safe_release(terminalIntegrator);
		safe_release(cappedIntegrator);
		safe_release(terminalCaster);
		safe_release(caster);
		safe_release(job);
		std::filesystem::remove(scenePath);
	}

	void TestSurfaceVolumeNEEProductionRoutes()
	{
		std::cout << "TestSurfaceVolumeNEEProductionRoutes" << std::endl;
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() /
			( "rise_surface_fire_receiver_" +
				std::to_string(static_cast<int>(::getpid())) + ".RISEscene" );
		{
			std::ofstream output(scenePath);
			output << SurfaceFireReceiverScene();
		}

		IJobPriv* job = nullptr;
		IRayCaster* neeCaster = nullptr;
		IRayCaster* marchOnlyCaster = nullptr;
		Check( RISE_CreateJobPriv(&job) && job &&
			job->LoadAsciiSceneViaCst(scenePath.string().c_str()),
			"surface receiver and off-axis fire fixture loads" );
		if( job ) {
			IShader* shader = job->GetShaders()->GetItem("global");
			const IMedium* fire = job->GetScene()->GetGlobalMedium();
			if( fire ) fire->addref();
			IsotropicPhaseFunction* vacuumPhase = new IsotropicPhaseFunction();
			UnsupportedHomogeneousSmoke* vacuum =
				new UnsupportedHomogeneousSmoke(*vacuumPhase,0.0,0.0);
			job->GetScene()->SetGlobalMedium(vacuum);
			Check( shader && RISE_API_CreateRayCaster(
				&marchOnlyCaster,false,20,*shader,true) && marchOnlyCaster,
				"surface brute-force caster initializes without a volume-emission CDF" );
			if( marchOnlyCaster ) marchOnlyCaster->AttachScene(job->GetScene());
			job->GetScene()->SetGlobalMedium(fire);
			safe_release(vacuum);
			safe_release(vacuumPhase);
			Check( shader && RISE_API_CreateRayCaster(
				&neeCaster,false,20,*shader,true) && neeCaster,
				"surface volume-NEE caster initializes with the fire CDF" );
			if( neeCaster ) neeCaster->AttachScene(job->GetScene());
			safe_release(fire);
		}

		if( job && neeCaster && marchOnlyCaster ) {
			const Scalar nm = 500.0;
			const unsigned int samples = 240000;
			const RasterizerState rast = {0,0};
			const Ray ray(Point3(0,0,-1),Vector3(0,0,1));
			auto meanPT = [&]( PathTracingIntegrator& integrator,
				const IRayCaster& route, const unsigned int seed ) {
				RandomNumberGenerator rng(seed);
				RuntimeContext rc(rng,RuntimeContext::PASS_NORMAL,false);
				IndependentSampler sampler(rng);
				Scalar sum = 0.0;
				for( unsigned int i=0; i<samples; ++i ) {
					sum += integrator.IntegrateRayNM(
						rc,rast,ray,nm,*job->GetScene(),route,sampler,
						nullptr,nullptr);
				}
				return sum/static_cast<Scalar>(samples);
			};
			auto meanShaderRoute = [&]( const IRayCaster& route,
				const unsigned int seed ) {
				RandomNumberGenerator rng(seed);
				RuntimeContext rc(rng,RuntimeContext::PASS_NORMAL,false);
				IRayCaster::RAY_STATE state;
				Scalar sum = 0.0;
				for( unsigned int i=0; i<samples; ++i ) {
					Scalar value = 0.0;
					route.CastRayNM(
						rc,rast,ray,value,state,nm,nullptr,nullptr);
					sum += value;
				}
				return sum/static_cast<Scalar>(samples);
			};

			StabilityConfig ordinaryConfig;
			ordinaryConfig.maxDiffuseBounce = 2;
			PathTracingIntegrator* ordinary = new PathTracingIntegrator(
				ManifoldSolverConfig(),ordinaryConfig);
			ordinary->SetMaxPathDepth(2);
			const Scalar ordinaryOn = meanPT(*ordinary,*neeCaster,0x5100a1u);
			const Scalar ordinaryOff = meanPT(*ordinary,*marchOnlyCaster,0x5100a2u);
			const Scalar shaderOn = meanShaderRoute(*neeCaster,0x5100a3u);
			const Scalar shaderOff = meanShaderRoute(*marchOnlyCaster,0x5100a4u);

			StabilityConfig terminalConfig;
			terminalConfig.maxDiffuseBounce = 2;
			PathTracingIntegrator* terminal = new PathTracingIntegrator(
				ManifoldSolverConfig(),terminalConfig);
			terminal->SetMaxPathDepth(1);
			const Scalar terminalOn = meanPT(*terminal,*neeCaster,0x5100b1u);
			const Scalar terminalOff = meanPT(*terminal,*marchOnlyCaster,0x5100b2u);

			StabilityConfig cappedConfig;
			cappedConfig.maxDiffuseBounce = 0;
			PathTracingIntegrator* capped = new PathTracingIntegrator(
				ManifoldSolverConfig(),cappedConfig);
			capped->SetMaxPathDepth(2);
			StabilityConfig uncappedConfig;
			uncappedConfig.maxDiffuseBounce = 1;
			PathTracingIntegrator* uncapped = new PathTracingIntegrator(
				ManifoldSolverConfig(),uncappedConfig);
			uncapped->SetMaxPathDepth(2);
			const Scalar cappedOn = meanPT(*capped,*neeCaster,0x5100c1u);
			const Scalar cappedReference = meanPT(
				*uncapped,*marchOnlyCaster,0x5100c2u);

			if( !(ordinaryOn>0.0 && ordinaryOff>0.0 && shaderOn>0.0 &&
				shaderOff>0.0 && terminalOn>0.0 && terminalOff>0.0 &&
				cappedOn>0.0 && cappedReference>0.0) ) {
				std::cout << "  surface ordinary=" << ordinaryOn << "/" << ordinaryOff <<
					" shader=" << shaderOn << "/" << shaderOff <<
					" terminal=" << terminalOn << "/" << terminalOff <<
					" capped=" << cappedOn << "/" << cappedReference << std::endl;
			}
			Check( ordinaryOn>0.0 && ordinaryOff>0.0 &&
				NearRelative(ordinaryOn,ordinaryOff,0.08),
				"surface closure NEE and brute-force march agree at a non-terminal vertex" );
			Check( shaderOn>0.0 && shaderOff>0.0 &&
				NearRelative(shaderOn,shaderOff,0.08),
				"shader-dispatch and pure-rasterizer PT entries share the surface estimator" );
			Check( NearRelative(ordinaryOn,shaderOn,0.08),
				"both PathTracingIntegrator entry routes produce the same surface NEE mean" );
			Check( terminalOn>0.0 && terminalOff>0.0 &&
				NearRelative(terminalOn,terminalOff,0.08),
				"total-depth surface vertex launches the A_march source-only competitor" );
			Check( cappedOn>0.0 && cappedReference>0.0 &&
				NearRelative(cappedOn,cappedReference,0.08),
				"diffuse-capped lobe is NEE-only f_D and matches uncapped brute force" );

			safe_release(uncapped);
			safe_release(capped);
			safe_release(terminal);
			safe_release(ordinary);
		}

		safe_release(neeCaster);
		safe_release(marchOnlyCaster);
		safe_release(job);
		std::filesystem::remove(scenePath);
	}

	void TestPointLightFlameThreeStrategyEquality()
	{
		std::cout << "TestPointLightFlameThreeStrategyEquality" << std::endl;
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() /
			( "rise_point_flame_receiver_" +
				std::to_string(static_cast<int>(::getpid())) + ".RISEscene" );
		{
			std::ofstream output(scenePath);
			output << SurfaceFireReceiverScene(true);
		}

		IJobPriv* job = nullptr;
		IRayCaster* neeCaster = nullptr;
		IRayCaster* marchOnlyCaster = nullptr;
		Check( RISE_CreateJobPriv(&job) && job &&
			job->LoadAsciiSceneViaCst(scenePath.string().c_str()),
			"point-light plus off-axis fire receiver fixture loads" );
		if( job ) {
			IShader* shader = job->GetShaders()->GetItem("global");
			const IMedium* fire = job->GetScene()->GetGlobalMedium();
			if( fire ) fire->addref();
			IsotropicPhaseFunction* vacuumPhase = new IsotropicPhaseFunction();
			UnsupportedHomogeneousSmoke* vacuum =
				new UnsupportedHomogeneousSmoke(*vacuumPhase,0.0,0.0);
			job->GetScene()->SetGlobalMedium(vacuum);
			Check( shader && RISE_API_CreateRayCaster(
				&marchOnlyCaster,false,20,*shader,true) && marchOnlyCaster,
				"point-light reference caster prepares without the volume endpoint strategy" );
			if( marchOnlyCaster ) marchOnlyCaster->AttachScene(job->GetScene());
			job->GetScene()->SetGlobalMedium(fire);
			safe_release(vacuum);
			safe_release(vacuumPhase);
			Check( shader && RISE_API_CreateRayCaster(
				&neeCaster,false,20,*shader,true) && neeCaster,
				"point-light plus flame caster prepares all three strategy entries" );
			if( neeCaster ) neeCaster->AttachScene(job->GetScene());
			safe_release(fire);
		}

		if( job && neeCaster && marchOnlyCaster ) {
			const RayCaster* concrete = dynamic_cast<const RayCaster*>(neeCaster);
			const LightSampler* lights = concrete ? concrete->GetLightSampler() : nullptr;
			Check( lights && lights->GetPositionalLightCount()==1 &&
				lights->GetVolumeEmissionMediumCount()==1 &&
				lights->GetEquiangularPivotEntryCount()==2,
				"production fixture contains point-light, flame-endpoint, and march families" );
			const Scalar pointPivotPdf = lights ?
				lights->GetEquiangularPivotSelectionPdf(0) : 0.0;
			Check( pointPivotPdf>0.0 && pointPivotPdf<1.0,
				"point-light and flame pivots both have nonzero production probability" );

			StabilityConfig config;
			config.maxDiffuseBounce = 2;
			PathTracingIntegrator* integrator = new PathTracingIntegrator(
				ManifoldSolverConfig(),config);
			integrator->SetMaxPathDepth(2);
			const Scalar nm = 500.0;
			const unsigned int samples = 280000;
			const RasterizerState rast = {0,0};
			const Ray ray(Point3(0,0,-1),Vector3(0,0,1));
			auto mean = [&]( const IRayCaster& route, const unsigned int seed ) {
				RandomNumberGenerator rng(seed);
				RuntimeContext rc(rng,RuntimeContext::PASS_NORMAL,false);
				IndependentSampler sampler(rng);
				Scalar sum = 0.0;
				for( unsigned int i=0; i<samples; ++i ) {
					sum += integrator->IntegrateRayNM(
						rc,rast,ray,nm,*job->GetScene(),route,sampler,
						nullptr,nullptr);
				}
				return sum/static_cast<Scalar>(samples);
			};

			const Scalar neeOn = mean(*neeCaster,0x3a7e001u);
			const Scalar neeOff = mean(*marchOnlyCaster,0x3a7e002u);
			const IMedium* fire = job->GetScene()->GetGlobalMedium();
			if( fire ) fire->addref();
			IsotropicPhaseFunction* vacuumPhase = new IsotropicPhaseFunction();
			UnsupportedHomogeneousSmoke* vacuum =
				new UnsupportedHomogeneousSmoke(*vacuumPhase,0.0,0.0);
			job->GetScene()->SetGlobalMedium(vacuum);
			const Scalar pointOnly = mean(*marchOnlyCaster,0x3a7e003u);
			job->GetScene()->SetGlobalMedium(fire);
			safe_release(vacuum);
			safe_release(vacuumPhase);
			safe_release(fire);
			const Scalar flameOn = neeOn-pointOnly;
			const Scalar flameOff = neeOff-pointOnly;
			if( !(neeOn>0.0 && neeOff>0.0 && pointOnly>0.0 && flameOn>0.0 &&
				flameOff>0.0 && NearRelative(flameOn,flameOff,0.10)) ) {
				std::cout << "  point+flame NEE on/off=" << neeOn << "/" << neeOff <<
					" point-only=" << pointOnly << " flame=" << flameOn << "/" <<
					flameOff << std::endl;
			}
			Check( pointOnly>0.0 && flameOn>0.0 && flameOff>0.0 &&
				NearRelative(flameOn,flameOff,0.10),
				"point-light, flame-endpoint NEE, and conditioned march converge to one mean" );
			safe_release(integrator);
		}

		safe_release(neeCaster);
		safe_release(marchOnlyCaster);
		safe_release(job);
		std::filesystem::remove(scenePath);
	}

	void TestNonNullSurfaceClearsMediumMarchCompetition()
	{
		std::cout << "TestNonNullSurfaceClearsMediumMarchCompetition" << std::endl;
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() /
			( "rise_medium_competition_surface_" +
				std::to_string(static_cast<int>(::getpid())) + ".RISEscene" );
		{
			std::ofstream output(scenePath);
			output <<
				"RISE ASCII SCENE 7\nstandard_shader\n{\nname global\n}\n"
				"box_geometry\n{\nname stop_geometry\nwidth 4\nheight 4\ndepth 0.2\n}\n"
				"standard_object\n{\nname stop\ngeometry stop_geometry\n"
				"material none\nposition 0 0 1.1\n}\n";
		}

		IJobPriv* job = nullptr;
		IRayCaster* caster = nullptr;
		VolumeStateProbeShader* probe = new VolumeStateProbeShader();
		probe->addref();
		Check( RISE_CreateJobPriv(&job) && job &&
			job->LoadAsciiSceneViaCst(scenePath.string().c_str()),
			"non-null competition-boundary fixture loads" );
		if( job ) {
			Check( job->GetShaders()->AddItem(probe,"volume_state_probe") &&
				job->SetObjectShader("stop","volume_state_probe"),
				"non-null surface installs the competition-state probe shader" );
			IShader* shader = job->GetShaders()->GetItem("global");
			Check( shader && RISE_API_CreateRayCaster(
				&caster,false,4,*shader,true) && caster,
				"non-null competition-boundary caster initializes" );
			if( caster ) caster->AttachScene(job->GetScene());
		}

		if( caster ) {
			RandomNumberGenerator rng(0xfaceb00u);
			RuntimeContext rc(rng,RuntimeContext::PASS_NORMAL,false);
			const RasterizerState rast = {0,0};
			IRayCaster::RAY_STATE state;
			Scalar value = 0.0;
			const VolumeEmissionSegmentState competingState(
				true,false,nullptr,1.0/FOUR_PI,0.0,0.0);
			bool hit = false;
			{
				const VolumeEmissionSegmentStateScope volumeStateScope(competingState);
				hit = caster->CastRayNM(
					rc,rast,Ray(Point3(0,0,0),Vector3(0,0,1)),value,state,
					500.0,nullptr,nullptr);
			}
			Check( hit && value==1.0 && !probe->sawCompetition,
				"a non-null surface resets the originating medium march family before shading" );
		}

		safe_release(caster);
		safe_release(job);
		safe_release(probe);
		std::filesystem::remove(scenePath);
	}

	void TestCollisionEmissionConsumesMarchCompetitionState()
	{
		std::cout << "TestCollisionEmissionConsumesMarchCompetitionState" << std::endl;
		Fixture fixture;
		Check( fixture.Initialize("collision_mis_state",1.0,1.0,false),
			"collision MIS state fixture initializes" );
		if( !fixture.integrator || !fixture.caster ) return;

		const Scalar nm = 500.0;
		const unsigned int seed = 0x6d157a7eu;
		const Scalar baselinePT = fixture.MeanPathTracingNM(seed,nm);
		const Scalar baselineRC = fixture.MeanRayCasterNM(seed,nm);
		const VolumeEmissionSegmentState singularCompetition(
			true,true,nullptr,0.25,0.0,0.0);
		const Scalar singularPT = fixture.MeanPathTracingNMWithSegmentState(
			seed,nm,singularCompetition);
		const Scalar singularRC = fixture.MeanRayCasterNMWithSegmentState(
			seed,nm,singularCompetition);
		Check( singularPT==baselinePT && singularRC==baselineRC,
			"sampled-delta competition leaves collision emission at weight one" );

		const VolumeEmissionSegmentState continuousCompetition(
			true,false,nullptr,0.25,0.0,0.0);
		const Scalar weightedPT = fixture.MeanPathTracingNMWithSegmentState(
			seed,nm,continuousCompetition);
		const Scalar weightedRC = fixture.MeanRayCasterNMWithSegmentState(
			seed,nm,continuousCompetition);
		if( !(weightedPT > 0.0 && weightedPT < baselinePT &&
			weightedRC > 0.0 && weightedRC < baselineRC) ||
			!NearRelative(weightedPT,weightedRC,0.012) ) {
			std::cout << "  baseline PT/RC=" << baselinePT << "/" << baselineRC
				<< " weighted PT/RC=" << weightedPT << "/" << weightedRC
				<< std::endl;
		}
		Check( weightedPT > 0.0 && weightedPT < baselinePT &&
			weightedRC > 0.0 && weightedRC < baselineRC,
			"continuous competing segments apply a nontrivial march-family weight" );
		Check( NearRelative(weightedPT,weightedRC,0.012),
			"PT and RayCaster consume the same collision-side march partition" );
	}

	void TestDirectPelEntryRejectsFire()
	{
		std::cout << "TestDirectPelEntryRejectsFire" << std::endl;
		Fixture fixture;
		Check( fixture.Initialize( "pel_reject", 1.0, 1.0 ),
			"direct Pel rejection fixture initializes" );
		if( !fixture.integrator || !fixture.caster || !fixture.job ) return;

		RandomNumberGenerator rng( 0x9e1f17eu );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
		IndependentSampler sampler( rng );
		const RasterizerState rast = { 0, 0 };
		const RISEPel value = fixture.integrator->IntegrateRay(
			rc, rast, Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ),
			*fixture.job->GetScene(), *fixture.caster, sampler, nullptr, nullptr );
		Check( value.r == 0.0 && value.g == 0.0 && value.b == 0.0,
			"pure PathTracingPel entry rejects fire until Phase-A step 7" );
	}

	void TestBoundedObjectFireRejectsShaderDispatchPel()
	{
		std::cout << "TestBoundedObjectFireRejectsShaderDispatchPel" << std::endl;
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() /
			( "rise_pt_bounded_pel_reject_" +
				std::to_string( static_cast<int>( ::getpid() ) ) + ".RISEscene" );
		{
			std::ofstream output( scenePath );
			output <<
				"RISE ASCII SCENE 7\n"
				"pathtracing_shaderop\n{\nname pathtracer\n}\n"
				"standard_shader\n{\nname global\nshaderop pathtracer\n}\n"
				"standard_shader\n{\nname emission_only\nshaderop DefaultEmission\n}\n"
				"uniformcolor_painter\n{\nname emitter_color\ncolor 1 1 1\n}\n"
				"lambertian_luminaire_material\n{\nname emitter\nexitance emitter_color\n"
				"scale 10\nmaterial none\n}\n"
				"scalar_painter\n{\nname carbon\nvalue 0.2\n}\n"
				"scalar_painter\n{\nname temperature\nvalue 1800\n}\n"
				"multichannel_heterogeneous_medium\n{\nname fire\n"
				"channel_carbon painter carbon\nchannel_temperature painter temperature\n"
				"bake_resolution 4 4 4\nbbox_min -1 -1 0\nbbox_max 1 1 4\n"
				"soot_em 0.26\nsoot_density 1800\nsoot_albedo_hot 0.1\nsoot_g_hot 0.5\n"
				"smoke_km_carbon 8.7\nsmoke_n_carbon 1.2\n"
				"smoke_albedo_carbon 0.6\nsmoke_g_carbon 0.6\n}\n"
				"box_geometry\n{\nname container_geometry\nwidth 2\nheight 2\ndepth 2\n}\n"
				"standard_object\n{\nname container\ngeometry container_geometry\n"
				"material emitter\ninterior_medium fire\nposition 0 0 2\n}\n";
		}

		IJobPriv* job = nullptr;
		IRayCaster* caster = nullptr;
		IRayCaster* emissionCaster = nullptr;
		PathTracingIntegrator* integrator = nullptr;
		Check( RISE_CreateJobPriv( &job ) && job &&
			job->LoadAsciiSceneViaCst( scenePath.string().c_str() ),
			"bounded-object Pel rejection fixture initializes" );
		if( job ) {
			IShader* shader = job->GetShaders()->GetItem( "global" );
			IShader* emissionShader = job->GetShaders()->GetItem( "emission_only" );
			Check( shader && emissionShader && RISE_API_CreateRayCaster(
				&caster, false, 10, *shader, true ) && caster,
				"bounded-object PT Pel rejection caster initializes" );
			Check( emissionShader && RISE_API_CreateRayCaster(
				&emissionCaster, false, 10, *emissionShader, true ) && emissionCaster,
				"bounded-object non-PT emission caster initializes" );
			if( caster ) caster->AttachScene( job->GetScene() );
			if( emissionCaster ) emissionCaster->AttachScene( job->GetScene() );
			integrator = new PathTracingIntegrator(
				ManifoldSolverConfig(), StabilityConfig() );
		}

		if( integrator && caster && emissionCaster && job ) {
			RandomNumberGenerator rng( 0xb0a0dedu );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			IndependentSampler sampler( rng );
			const RasterizerState rast = { 0, 0 };
			const Ray ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
			RayIntersection firstHit( ray, rast );
			job->GetScene()->GetObjects()->IntersectRay(
				firstHit, true, true, false );
			Check( firstHit.geometric.bHit,
				"camera outside the bounded fire reaches its container boundary" );
			IORStack iorStack( 1.0 );
			const RISEPel direct = integrator->IntegrateFromHit(
				rc, rast, firstHit, *job->GetScene(), *caster, sampler,
				nullptr, 0, iorStack, 0.0, RISEPel( 0.0 ), true, 1.0,
				IRayCaster::RAY_STATE::eRayView,
				0, 0, 0, 0, 0, 0.0, false, false, nullptr );
			Check( direct.r == 0.0 && direct.g == 0.0 && direct.b == 0.0,
				"public IntegrateFromHit rejects scene-wide bounded fire" );

			IRayCaster::RAY_STATE state;
			RISEPel dispatched( 1.0 );
			const bool hit = emissionCaster->CastRay(
				rc, rast, ray, dispatched, state, nullptr, nullptr );
			Check( !hit && dispatched.r == 0.0 && dispatched.g == 0.0 && dispatched.b == 0.0,
				"RayCaster rejects bounded fire before a non-PT emission shader" );

			// Matched no-fire control: clear only the interior binding.  The
			// same visible emitter must then be nonzero through both entry routes,
			// proving the black/false results above came from rejection rather
			// than an ordinarily black boundary material.
			SceneEditor editor( *job->GetScene() );
			editor.SetJob( job );
			SceneEdit clear;
			clear.op = SceneEdit::SetObjectInteriorMedium;
			clear.objectName = "container";
			clear.propertyValue = "none";
			Check( editor.Apply( clear ),
				"bounded-fire control clears only the interior medium" );
			caster->AttachScene( job->GetScene() );
			emissionCaster->AttachScene( job->GetScene() );
			const LightSampler* lights = caster->GetLightSampler();
			Check( lights && !lights->SceneHasFireMedia(),
				"bounded-fire control contains no active fire medium" );

			RayIntersection controlHit( ray, rast );
			job->GetScene()->GetObjects()->IntersectRay(
				controlHit, true, true, false );
			const RISEPel directControl = integrator->IntegrateFromHit(
				rc, rast, controlHit, *job->GetScene(), *caster, sampler,
				nullptr, 0, iorStack, 0.0, RISEPel( 0.0 ), true, 1.0,
				IRayCaster::RAY_STATE::eRayView,
				0, 0, 0, 0, 0, 0.0, false, false, nullptr );
			Check( ColorMath::MaxValue( directControl ) > 1.0,
				"no-fire IntegrateFromHit control sees the emissive boundary" );
			RISEPel dispatchedControl( 0.0 );
			const bool controlHitResult = emissionCaster->CastRay(
				rc, rast, ray, dispatchedControl, state, nullptr, nullptr );
			if( !controlHitResult || ColorMath::MaxValue( dispatchedControl ) <= 1.0 ) {
				std::cout << "  shader-dispatch control hit=" << controlHitResult <<
					" value=(" << dispatchedControl.r << "," << dispatchedControl.g <<
					"," << dispatchedControl.b << ")" << std::endl;
			}
			Check( controlHitResult && ColorMath::MaxValue( dispatchedControl ) > 1.0,
				"no-fire non-PT RayCaster control sees the emissive boundary" );
		}

		safe_release( integrator );
		safe_release( emissionCaster );
		safe_release( caster );
		safe_release( job );
		std::filesystem::remove( scenePath );
	}

	void TestPostScatterSegmentRunsMediumTransport()
	{
		std::cout << "TestPostScatterSegmentRunsMediumTransport" << std::endl;
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() /
			( "rise_pt_post_scatter_" + std::to_string( static_cast<int>( ::getpid() ) ) + ".RISEscene" );
		{
			std::ofstream output( scenePath );
			output << "RISE ASCII SCENE 7\nstandard_shader\n{\nname global\n}\n";
		}

		IJobPriv* job = nullptr;
		IRayCaster* caster = nullptr;
		PathTracingIntegrator* integrator = nullptr;
		ForwardPhase* phase = new ForwardPhase();
		TwoEventFireMedium* medium = new TwoEventFireMedium( *phase );
		Check( RISE_CreateJobPriv( &job ) && job &&
			job->LoadAsciiSceneViaCst( scenePath.string().c_str() ),
			"two-event continuation fixture initializes" );
		if( job ) {
			job->GetScene()->SetGlobalMedium( medium );
			IShader* shader = job->GetShaders()->GetItem( "global" );
			Check( shader && RISE_API_CreateRayCaster(
				&caster, false, 0, *shader, true ) && caster,
				"two-event continuation caster initializes" );
			if( caster ) caster->AttachScene( job->GetScene() );
			StabilityConfig stability;
			stability.maxVolumeBounce = 2;
			integrator = new PathTracingIntegrator( ManifoldSolverConfig(), stability );
			integrator->SetMaxPathDepth( 1 );
		}

		if( integrator && caster && job ) {
			RandomNumberGenerator rng( 0x2e7e17u );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			IndependentSampler sampler( rng );
			const RasterizerState rast = { 0, 0 };
			const Scalar value = integrator->IntegrateRayNM(
				rc, rast, Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ), 500.0,
				*job->GetScene(), *caster, sampler, nullptr, nullptr );
			Check( NearRelative( value, 1.0, 1e-13 ),
				"path-depth-capped post-scatter segment still scores its thermal event" );
		}

		safe_release( integrator );
		safe_release( caster );
		safe_release( job );
		safe_release( medium );
		safe_release( phase );
		std::filesystem::remove( scenePath );
	}

	void TestForcedNullProposalsThroughPathTracing()
	{
		std::cout << "TestForcedNullProposalsThroughPathTracing" << std::endl;
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() /
			( "rise_pt_forced_null_" + std::to_string( static_cast<int>( ::getpid() ) ) + ".RISEscene" );
		{
			std::ofstream output( scenePath );
			output << "RISE ASCII SCENE 7\nstandard_shader\n{\nname global\n}\n";
		}

		IJobPriv* job = nullptr;
		IRayCaster* caster = nullptr;
		PathTracingIntegrator* integrator = nullptr;
		DelayedDensityAccessor* accessor = new DelayedDensityAccessor();
		IPhaseFunction* phase = nullptr;
		Check( RISE_API_CreateIsotropicPhaseFunction( &phase ) && phase,
			"forced-null production phase initializes" );
		DelayedThermalHeterogeneousMedium* medium = phase ?
			new DelayedThermalHeterogeneousMedium( *phase, *accessor ) : nullptr;
		accessor->tracking = true;
		Check( RISE_CreateJobPriv( &job ) && job &&
			job->LoadAsciiSceneViaCst( scenePath.string().c_str() ),
			"forced-null PT fixture initializes" );
		if( job && medium ) {
			job->GetScene()->SetGlobalMedium( medium );
			IShader* shader = job->GetShaders()->GetItem( "global" );
			Check( shader && RISE_API_CreateRayCaster(
				&caster, false, 0, *shader, true ) && caster,
				"forced-null PT caster initializes" );
			if( caster ) caster->AttachScene( job->GetScene() );
			StabilityConfig stability;
			stability.maxVolumeBounce = 0;
			integrator = new PathTracingIntegrator( ManifoldSolverConfig(), stability );
		}

		if( integrator && caster && job ) {
			RandomNumberGenerator rng( 0x1024u );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			IndependentSampler sampler( rng );
			const RasterizerState rast = { 0, 0 };
			const Scalar value = integrator->IntegrateRayNM(
				rc, rast, Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ), 500.0,
				*job->GetScene(), *caster, sampler, nullptr, nullptr );
			Check( accessor->zeroDensityQueries > 1024,
				"PT runs the production heterogeneous tracker past 1024 null proposals" );
			Check( NearRelative( value, 3.0, 1e-13 ),
				"PT matches the uncapped thermal-event reference after the null tail" );
		}

		safe_release( integrator );
		safe_release( caster );
		safe_release( job );
		safe_release( medium );
		safe_release( phase );
		safe_release( accessor );
		std::filesystem::remove( scenePath );
	}

	void TestOpticallyThickMixedDensityUsesLogDomain()
	{
		std::cout << "TestOpticallyThickMixedDensityUsesLogDomain" << std::endl;
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() /
			( "rise_pt_log_tail_" + std::to_string( static_cast<int>( ::getpid() ) ) + ".RISEscene" );
		{
			std::ofstream output( scenePath );
			output <<
				"RISE ASCII SCENE 7\nstandard_shader\n{\nname global\n}\n"
				"omni_light\n{\nname pivot\npower 1\ncolor 1 1 1\nposition 1e-10 0 0\n}\n";
		}

		IJobPriv* job = nullptr;
		IRayCaster* caster = nullptr;
		PathTracingIntegrator* integrator = nullptr;
		LogTailFireMedium* medium = new LogTailFireMedium();
		Check( RISE_CreateJobPriv( &job ) && job &&
			job->LoadAsciiSceneViaCst( scenePath.string().c_str() ),
			"tau=99 PT log-tail fixture initializes" );
		if( job ) {
			job->GetScene()->SetGlobalMedium( medium );
			IShader* shader = job->GetShaders()->GetItem( "global" );
			Check( shader && RISE_API_CreateRayCaster(
				&caster, false, 0, *shader, true ) && caster,
				"tau=99 PT log-tail caster initializes" );
			if( caster ) caster->AttachScene( job->GetScene() );
			StabilityConfig stability;
			stability.maxVolumeBounce = 0;
			integrator = new PathTracingIntegrator( ManifoldSolverConfig(), stability );
		}

		Scalar measured = 0.0;
		bool sampledTail = false;
		if( integrator && caster && job ) {
			const RasterizerState rast = { 0, 0 };
			for( unsigned int seed = 1; seed <= 1000 && !sampledTail; ++seed ) {
				medium->lastLogEventDistance = -1.0;
				RandomNumberGenerator rng( seed );
				RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
				IndependentSampler sampler( rng );
				measured = integrator->IntegrateRayNM(
					rc, rast, Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ), 500.0,
					*job->GetScene(), *caster, sampler, nullptr, nullptr );
				sampledTail = medium->lastLogEventDistance == LogTailFireMedium::EventDistance();
			}
		}
		Check( sampledTail, "PT mixed distance proposal exercises a sub-1e-30 tau=99 event" );
		if( sampledTail ) {
			const Ray ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
			const Scalar eventDistance = LogTailFireMedium::EventDistance();
			const Scalar pdfDt = LogTailFireMedium::SigmaT() * std::exp( -99.0 );
			const Scalar pdfEq = EquiangularSampling::Pdf(
				ray, Point3( 1e-10, 0, 0 ), 0.0,
				1.1 * eventDistance, true, eventDistance );
			const Scalar expected = pdfDt / ( 0.5 * pdfDt + 0.5 * pdfEq );
			Check( pdfDt < 1e-30 && pdfEq < 1e-30,
				"both tau-tail proposal densities are materially below the old floor" );
			Check( RISE::IsFiniteDouble( measured ) &&
				NearRelative( measured, expected, 1e-12 ),
				"tau=99 PT tail matches the unfloored log-sum-exp reference" );
		}

		safe_release( integrator );
		safe_release( caster );
		safe_release( job );
		safe_release( medium );
		std::filesystem::remove( scenePath );
	}

	void TestObjectFireRoutingCacheRefreshesAfterBinding()
	{
		std::cout << "TestObjectFireRoutingCacheRefreshesAfterBinding" << std::endl;
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() /
			( "rise_pt_object_fire_cache_" + std::to_string( static_cast<int>( ::getpid() ) ) + ".RISEscene" );
		{
			std::ofstream output( scenePath );
			output <<
				"RISE ASCII SCENE 7\nstandard_shader\n{\nname global\n}\n"
				"scalar_painter\n{\nname carbon\nvalue 0.2\n}\n"
				"scalar_painter\n{\nname temperature\nvalue 1800\n}\n"
				"multichannel_heterogeneous_medium\n{\nname fire\n"
				"channel_carbon painter carbon\nchannel_temperature painter temperature\n"
				"bake_resolution 4 4 4\nbbox_min -1 -1 -1\nbbox_max 1 1 1\n"
				"soot_em 0.26\nsoot_density 1800\nsoot_albedo_hot 0.1\nsoot_g_hot 0.5\n"
				"smoke_km_carbon 8.7\nsmoke_n_carbon 1.2\n"
				"smoke_albedo_carbon 0.6\nsmoke_g_carbon 0.6\n}\n"
				"box_geometry\n{\nname container_geometry\nwidth 2\nheight 2\ndepth 2\n}\n"
				"standard_object\n{\nname container\ngeometry container_geometry\nmaterial none\n}\n";
		}

		IJobPriv* job = nullptr;
		IRayCaster* caster = nullptr;
		Check( RISE_CreateJobPriv( &job ) && job &&
			job->LoadAsciiSceneViaCst( scenePath.string().c_str() ),
			"object-fire cache fixture initializes" );
		if( job ) {
			IShader* shader = job->GetShaders()->GetItem( "global" );
			Check( shader && RISE_API_CreateRayCaster(
				&caster, false, 0, *shader, true ) && caster,
				"object-fire cache caster initializes" );
			if( caster ) {
				caster->AttachScene( job->GetScene() );
				const LightSampler* lights = caster->GetLightSampler();
				Check( lights && !lights->SceneHasFireMedia(),
					"prepared sampler starts without an object fire medium" );
				Check( job->SetObjectInteriorMedium( "container", "fire" ),
					"runtime object fire binding succeeds" );
				caster->AttachScene( job->GetScene() );
				lights = caster->GetLightSampler();
				Check( lights && lights->SceneHasFireMedia(),
					"same-scene caster refreshes fire routing after interior-medium binding" );

				SceneEditor editor( *job->GetScene() );
				editor.SetJob( job );
				SceneEdit clear;
				clear.op = SceneEdit::SetObjectInteriorMedium;
				clear.objectName = "container";
				clear.propertyValue = "none";
				Check( editor.Apply( clear ),
					"CST object interior-medium clear succeeds" );
				caster->AttachScene( job->GetScene() );
				lights = caster->GetLightSampler();
				Check( lights && !lights->SceneHasFireMedia(),
					"CST optional-slot clear invalidates the object-fire cache" );

				Check( editor.Undo(), "CST object interior-medium clear undo succeeds" );
				caster->AttachScene( job->GetScene() );
				lights = caster->GetLightSampler();
				Check( lights && lights->SceneHasFireMedia(),
					"CST undo refreshes the object-fire cache" );
				Check( editor.Redo(), "CST object interior-medium clear redo succeeds" );
				caster->AttachScene( job->GetScene() );
				lights = caster->GetLightSampler();
				Check( lights && !lights->SceneHasFireMedia(),
					"CST redo refreshes the cleared object-fire cache" );

				Check( editor.Undo(), "CST fire binding restored before object removal" );
				caster->AttachScene( job->GetScene() );
				Check( job->RemoveObject( "container" ),
					"last bounded fire object removal succeeds" );
				caster->AttachScene( job->GetScene() );
				lights = caster->GetLightSampler();
				Check( lights && !lights->SceneHasFireMedia(),
					"removing the last bounded fire object invalidates the cache" );
			}
		}

		safe_release( caster );
		safe_release( job );
		std::filesystem::remove( scenePath );
	}

	void TestLegacySceneEditorMediumUndoRefreshesCache()
	{
		std::cout << "TestLegacySceneEditorMediumUndoRefreshesCache" << std::endl;
		Job* job = new Job();
		const char* shaderOps[] = { "DefaultDirectLighting" };
		const double sigmaA[3] = { 0.1, 0.1, 0.1 };
		const double sigmaS[3] = { 0.0, 0.0, 0.0 };
		const double zero[3] = { 0.0, 0.0, 0.0 };
		const double one[3] = { 1.0, 1.0, 1.0 };
		RadianceMapConfig noRadianceMap;
		Check( job->AddStandardShader( "global", 1, shaderOps ) &&
			job->AddBoxGeometry( "container_geometry", 2.0, 2.0, 2.0 ) &&
			job->AddObject(
				"container", "container_geometry", "none", nullptr, nullptr,
				noRadianceMap, zero, zero, one, true, true ) &&
			job->AddHomogeneousMedium(
				"fog", sigmaA, sigmaS, "isotropic", 0.0 ),
			"programmatic legacy medium-edit fixture initializes" );

		IRayCaster* caster = nullptr;
		IShader* shader = job->GetShaders()->GetItem( "global" );
		Check( shader && RISE_API_CreateRayCaster(
			&caster, false, 0, *shader, true ) && caster,
			"programmatic legacy medium-edit caster initializes" );
		if( caster ) caster->AttachScene( job->GetScene() );

		if( caster ) {
			const LightSampler* lights = caster->GetLightSampler();
			Check( lights && !lights->SceneHasMedia(),
				"legacy fixture starts without object media" );
			SceneEditor editor( *job->GetScene() );
			editor.SetJob( job );
			SceneEdit bind;
			bind.op = SceneEdit::SetObjectInteriorMedium;
			bind.objectName = "container";
			bind.propertyValue = "fog";
			Check( editor.Apply( bind ), "legacy SceneEditor medium bind succeeds" );
			caster->AttachScene( job->GetScene() );
			lights = caster->GetLightSampler();
			Check( lights && lights->SceneHasMedia(),
				"legacy forward bind refreshes the object-medium cache" );

			Check( editor.Undo(), "legacy SceneEditor medium-bind undo succeeds" );
			caster->AttachScene( job->GetScene() );
			lights = caster->GetLightSampler();
			Check( lights && !lights->SceneHasMedia(),
				"legacy undo clears and refreshes the object-medium cache" );
			Check( editor.Redo(), "legacy SceneEditor medium-bind redo succeeds" );
			caster->AttachScene( job->GetScene() );
			lights = caster->GetLightSampler();
			Check( lights && lights->SceneHasMedia(),
				"legacy redo restores and refreshes the object-medium cache" );
		}

		safe_release( caster );
		safe_release( job );
	}

	void TestSpatialAdditiveSourceIsAnIndependentFullSegmentEstimator()
	{
		std::cout << "TestSpatialAdditiveSourceIsAnIndependentFullSegmentEstimator" << std::endl;
		char filename[176];
		std::snprintf( filename, sizeof(filename),
			"rise_pathtracing_spatial_additive_%d.RISEscene", static_cast<int>( ::getpid() ) );
		const std::filesystem::path scenePath =
			std::filesystem::temp_directory_path() / filename;
		{
			std::ofstream output( scenePath );
			output << "RISE ASCII SCENE 7\nstandard_shader\n{\nname global\n}\n";
		}

		IJobPriv* job = nullptr;
		IRayCaster* caster = nullptr;
		PathTracingIntegrator* integrator = nullptr;
		Check( RISE_CreateJobPriv( &job ) && job &&
			job->LoadAsciiSceneViaCst( scenePath.string().c_str() ),
			"spatial additive PT fixture initializes" );
		if( job ) {
			SpatialAdditiveMedium* medium = new SpatialAdditiveMedium();
			job->GetScene()->SetGlobalMedium( medium );
			safe_release( medium );
			IShader* shader = job->GetShaders()->GetItem( "global" );
			Check( shader && RISE_API_CreateRayCaster(
				&caster, false, 0, *shader, true ) && caster,
				"spatial additive PT caster initializes" );
			if( caster ) caster->AttachScene( job->GetScene() );
			StabilityConfig stability;
			stability.maxVolumeBounce = 0;
			integrator = new PathTracingIntegrator( ManifoldSolverConfig(), stability );
		}

		if( integrator && caster ) {
			RandomNumberGenerator rng( 0xadd1715u );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			IndependentSampler sampler( rng );
			const RasterizerState rast = { 0, 0 };
			const Ray ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
			Scalar sum = 0;
			for( unsigned int i = 0; i < kSamples; ++i ) {
				sum += integrator->IntegrateRayNM( rc, rast, ray, 500.0,
					*job->GetScene(), *caster, sampler, nullptr, nullptr );
			}
			Check( NearRelative( sum / Scalar( kSamples ), 1.0, 0.012 ),
				"PT scores spatial additive emission on the full escaped segment" );

			RandomNumberGenerator hwssRng( 0xadd4855u );
			RuntimeContext hwssRc( hwssRng, RuntimeContext::PASS_NORMAL, false );
			IndependentSampler hwssSampler( hwssRng );
			const SampledWavelengths wavelengths = SampledWavelengths::SampleEquidistant(
				0.25, 380.0, 780.0 );
			Scalar hwssSums[SampledWavelengths::N] = { 0, 0, 0, 0 };
			for( unsigned int i = 0; i < kSamples; ++i ) {
				SampledWavelengths swl = wavelengths;
				Scalar values[SampledWavelengths::N];
				integrator->IntegrateRayHWSS( hwssRc, rast, ray, swl,
					*job->GetScene(), *caster, hwssSampler, nullptr, values, nullptr );
				for( unsigned int w = 0; w < SampledWavelengths::N; ++w ) {
					hwssSums[w] += values[w];
				}
			}
			for( unsigned int w = 0; w < SampledWavelengths::N; ++w ) {
				Check( NearRelative( hwssSums[w] / Scalar( kSamples ), 1.0, 0.012 ),
					"HWSS PT scores spatial additive emission on the full escaped segment" );
			}
		}

		safe_release( integrator );
		safe_release( caster );
		safe_release( job );
		std::filesystem::remove( scenePath );
	}

	void TestFirePhaseClosureRoutesOneBoundInstancePerCollision()
	{
		std::cout << "TestFirePhaseClosureRoutesOneBoundInstancePerCollision" << std::endl;
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() /
			( "rise_pt_phase_closure_" + std::to_string( static_cast<int>( ::getpid() ) ) +
				".RISEscene" );
		{
			std::ofstream output( scenePath );
			output <<
				"RISE ASCII SCENE 7\n"
				"standard_shader\n{\nname global\n}\n"
				"ambient_light\n{\nname phase_ambient\npower 1\ncolor 1 1 1\n}\n"
				"omni_light\n{\nname phase_nee\npower 10\ncolor 1 1 1\n"
				"position 1 0 0.5\n}\n";
		}

		PhaseClosureAudit audit;
		AuditedFireMedium* medium = new AuditedFireMedium( audit );
		IJobPriv* job = nullptr;
		IRayCaster* caster = nullptr;
		PathTracingIntegrator* integrator = nullptr;
		Check( RISE_CreateJobPriv( &job ) && job &&
			job->LoadAsciiSceneViaCst( scenePath.string().c_str() ),
			"phase-closure transport fixture initializes" );
		if( job ) {
			job->GetScene()->SetGlobalMedium( medium );
			IShader* shader = job->GetShaders()->GetItem( "global" );
			Check( shader && RISE_API_CreateRayCaster(
				&caster, false, 4, *shader, true ) && caster,
				"phase-closure transport caster initializes" );
			if( caster ) caster->AttachScene( job->GetScene() );
			StabilityConfig stability;
			stability.maxVolumeBounce = 1;
			integrator = new PathTracingIntegrator( ManifoldSolverConfig(), stability );
			integrator->SetMaxPathDepth( 2 );
		}

		auto CheckOneCollision = [&audit](
			const Scalar expectedX,
			const Scalar expectedNM,
			const bool guidingExpected,
			const char* label )
		{
			const bool oneBoundInstance = audit.makeCalls == 1 &&
				audit.destroyCalls == 1 && audit.legacyLookups == 0 &&
				audit.evaluateCalls >= 2 && audit.evaluateId != 0 &&
				audit.sampleId == audit.evaluateId &&
				audit.pdfId == audit.evaluateId && !audit.mixedInstanceUse &&
				audit.positions.size() == 1 && audit.wavelengths.size() == 1 &&
				NearRelative( audit.positions[0].x, expectedX, 1e-13 ) &&
				NearRelative( audit.wavelengths[0], expectedNM, 1e-13 ) &&
				( guidingExpected ? audit.meanCosineId == audit.evaluateId :
					audit.meanCosineId == 0 );
			if( !oneBoundInstance ) {
				std::cout << "  audit make=" << audit.makeCalls <<
					" destroy=" << audit.destroyCalls <<
					" legacy=" << audit.legacyLookups <<
					" evalCalls=" << audit.evaluateCalls <<
					" eval=" << audit.evaluateId <<
					" sample=" << audit.sampleId <<
					" pdf=" << audit.pdfId <<
					" mean=" << audit.meanCosineId <<
					" mixed=" << audit.mixedInstanceUse <<
					" positions=" << audit.positions.size() <<
					" wavelengths=" << audit.wavelengths.size() << std::endl;
				if( !audit.positions.empty() ) {
					for( unsigned int i = 0; i < audit.positions.size(); ++i ) {
						std::cout << "  position[" << i << "]=(" << audit.positions[i].x << "," <<
							audit.positions[i].y << "," << audit.positions[i].z << ") nm=" <<
							audit.wavelengths[i] << std::endl;
					}
				}
			}
			Check( oneBoundInstance, label );
		};

		if( integrator && caster && job )
		{
			const RasterizerState rast = { 0, 0 };
			const Scalar positions[] = { -0.5, 0.5 };
			const Scalar wavelengths[] = { 450.0, 750.0 };
			for( const Scalar x : positions ) {
				for( const Scalar nm : wavelengths ) {
					audit.Reset();
					RandomNumberGenerator rng(
						static_cast<unsigned int>( 1000.0 + 10.0 * x + nm ) );
					RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
					IndependentSampler sampler( rng );
					const Ray ray( Point3( x, 0, 0 ), Vector3( 0, 0, 1 ) );
					integrator->IntegrateRayNM( rc, rast, ray, nm,
						*job->GetScene(), *caster, sampler, nullptr, nullptr );
					CheckOneCollision( x, nm, false,
						"PT NEE and continuation share one local wavelength-bound closure" );

					audit.Reset();
					RandomNumberGenerator casterRng(
						static_cast<unsigned int>( 2000.0 + 10.0 * x + nm ) );
					RuntimeContext casterRc(
						casterRng, RuntimeContext::PASS_NORMAL, false );
					IRayCaster::RAY_STATE state;
					Scalar value = 0.0;
					caster->CastRayNM( casterRc, rast, ray, value, state,
						nm, nullptr, nullptr );
					CheckOneCollision( x, nm, false,
						"RayCaster NEE and continuation share one local wavelength-bound closure" );
				}
			}

#ifdef RISE_ENABLE_OPENPGL
			PathGuidingConfig guidingConfig;
			guidingConfig.enabled = true;
			Implementation::PathGuidingField* guiding =
				new Implementation::PathGuidingField(
					guidingConfig, Point3( -2, -2, -1 ), Point3( 2, 2, 2 ) );
			guiding->BeginTrainingIteration();
			const Vector3 trainingDirections[] = {
				Vector3( 1, 0, 0 ), Vector3( -1, 0, 0 ),
				Vector3( 0, 1, 0 ), Vector3( 0, -1, 0 ),
				Vector3( 0, 0, 1 ), Vector3( 0, 0, -1 )
			};
			for( const Scalar x : positions ) {
				for( unsigned int i = 0; i < 96; ++i ) {
					guiding->AddVolumeSample(
						Point3( x, 0, 0.5 ), trainingDirections[i % 6],
						1.0, 1.0 / FOUR_PI, 1.0, false );
				}
			}
			guiding->EndTrainingIteration();
			Check( guiding->IsTrained(), "volume-guiding closure fixture trains" );

			for( unsigned int route = 0; route < 2; ++route ) {
				const Scalar x = route == 0 ? positions[0] : positions[1];
				const Scalar nm = route == 0 ? wavelengths[0] : wavelengths[1];
				audit.Reset();
				RandomNumberGenerator rng( 0x6a1d000u + route );
				RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
				rc.pGuidingField = guiding;
				rc.guidingAlpha = 0.5;
				rc.guidingLearnedAlpha = false;
				rc.maxGuidingDepth = 8;
				IndependentSampler sampler( rng );
				const Ray ray( Point3( x, 0, 0 ), Vector3( 0, 0, 1 ) );
				if( route == 0 ) {
					integrator->IntegrateRayNM( rc, rast, ray, nm,
						*job->GetScene(), *caster, sampler, nullptr, nullptr );
					CheckOneCollision( x, nm, true,
						"guided PT uses the collision closure for HG product and transport" );
				} else {
					IRayCaster::RAY_STATE state;
					Scalar value = 0.0;
					caster->CastRayNM( rc, rast, ray, value, state,
						nm, nullptr, nullptr );
					CheckOneCollision( x, nm, true,
						"guided RayCaster uses the collision closure for HG product and transport" );
				}
			}
			safe_release( guiding );
#endif

			const SampledWavelengths requested = SampledWavelengths::SampleEquidistant(
				0.375, 380.0, 780.0 );
			audit.Reset();
			RandomNumberGenerator hwssRng( 0x48555u );
			RuntimeContext hwssRc( hwssRng, RuntimeContext::PASS_NORMAL, false );
			IndependentSampler hwssSampler( hwssRng );
			SampledWavelengths ptWavelengths = requested;
			Scalar ptValues[SampledWavelengths::N];
			integrator->IntegrateRayHWSS(
				hwssRc, rast, Ray( Point3( 0.5, 0, 0 ), Vector3( 0, 0, 1 ) ),
				ptWavelengths, *job->GetScene(), *caster, hwssSampler,
				nullptr, ptValues, nullptr );
			bool ptWavelengthsBound = audit.wavelengths.size() == SampledWavelengths::N;
			for( unsigned int w = 0; w < audit.wavelengths.size(); ++w ) {
				ptWavelengthsBound = ptWavelengthsBound &&
					NearRelative( audit.wavelengths[w], requested.lambda[w], 1e-13 );
			}
			Check( audit.legacyLookups == 0 &&
				audit.makeCalls == SampledWavelengths::N &&
				audit.destroyCalls == SampledWavelengths::N && ptWavelengthsBound,
				"HWSS-requested PT fire path takes NM closure fallback before legacy lookup" );

			audit.Reset();
			RandomNumberGenerator casterHWSSRng( 0xca57e485u );
			RuntimeContext casterHWSSRc(
				casterHWSSRng, RuntimeContext::PASS_NORMAL, false );
			IRayCaster::RAY_STATE state;
			SampledWavelengths casterWavelengths = requested;
			Scalar casterValues[SampledWavelengths::N];
			const IORStack casterIorStack( 1.0 );
			caster->CastRayHWSS(
				casterHWSSRc, rast, Ray( Point3( 0.5, 0, 0 ), Vector3( 0, 0, 1 ) ),
				casterValues, state, casterWavelengths, nullptr, nullptr, casterIorStack );
			bool casterWavelengthsBound = audit.wavelengths.size() == SampledWavelengths::N;
			for( unsigned int w = 0; w < audit.wavelengths.size(); ++w ) {
				casterWavelengthsBound = casterWavelengthsBound &&
					NearRelative( audit.wavelengths[w], requested.lambda[w], 1e-13 );
			}
			const bool casterHWSSFallsBack = audit.legacyLookups == 0 &&
				audit.makeCalls == SampledWavelengths::N &&
				audit.destroyCalls == SampledWavelengths::N && casterWavelengthsBound;
			if( !casterHWSSFallsBack ) {
				std::cout << "  RayCaster HWSS audit make=" << audit.makeCalls <<
					" destroy=" << audit.destroyCalls <<
					" legacy=" << audit.legacyLookups <<
					" wavelengths=" << audit.wavelengths.size() << std::endl;
			}
			Check( casterHWSSFallsBack,
				"HWSS-requested RayCaster fire path takes NM closure fallback before legacy lookup" );
		}

		safe_release( integrator );
		safe_release( caster );
		safe_release( job );
		safe_release( medium );
		std::filesystem::remove( scenePath );
	}
}

int main()
{
	TestNMAbsoluteParityAndSceneUnits();
	TestHWSSRequestedUsesPerWavelengthFallback();
	TestEquiangularMixtureUsesItsActualDistanceDensity();
	TestEquiangularBoundednessAndCollinearFallback();
	TestUnboundedGlobalMediumDisablesEquiangularBeforeTechniqueRoll();
	TestFlameOnlySceneActivatesCombinedEquiangularSampler();
	TestPrimaryScatteringEventHonorsVolumeCapAfterEmission();
	TestSurfaceVolumeNEEProductionRoutes();
	TestPointLightFlameThreeStrategyEquality();
	TestNonNullSurfaceClearsMediumMarchCompetition();
	TestCollisionEmissionConsumesMarchCompetitionState();
	TestDirectPelEntryRejectsFire();
	TestBoundedObjectFireRejectsShaderDispatchPel();
	TestPostScatterSegmentRunsMediumTransport();
	TestForcedNullProposalsThroughPathTracing();
	TestOpticallyThickMixedDensityUsesLogDomain();
	TestObjectFireRoutingCacheRefreshesAfterBinding();
	TestLegacySceneEditorMediumUndoRefreshesCache();
	TestSpatialAdditiveSourceIsAnIndependentFullSegmentEstimator();
	TestFirePhaseClosureRoutesOneBoundInstancePerCollision();
	std::cout << passed << " passed, " << failed << " failed" << std::endl;
	return failed == 0 ? 0 : 1;
}
