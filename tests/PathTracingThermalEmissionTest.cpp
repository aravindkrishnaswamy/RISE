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

#include <algorithm>
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
#include "../src/Library/Interfaces/IObjectPriv.h"
#include "../src/Library/Interfaces/IRayCaster.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IShader.h"
#include "../src/Library/Interfaces/IShaderManager.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Job.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Materials/HeterogeneousMedium.h"
#include "../src/Library/Materials/IsotropicPhaseFunction.h"
#include "../src/Library/Materials/PerfectRefractorMaterial.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/SceneEditor/SceneEdit.h"
#include "../src/Library/SceneEditor/SceneEditor.h"
#include "../src/Library/Shaders/PathTracingIntegrator.h"
#include "../src/Library/Shaders/PathTracingShaderOp.h"
#include "../src/Library/Shaders/StandardShader.h"
#include "../src/Library/Utilities/EquiangularSampler.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/IORStackSeeding.h"
#include "../src/Library/Utilities/MediumTracking.h"
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
		const Scalar sootAlbedoHot = 0.0,
		const Scalar positionalLightPower = 1.0 )
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
			ss << "omni_light\n{\nname sampling_pivot\npower " <<
				positionalLightPower << "\ncolor 1 1 1\nposition "
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
			const Scalar sootAlbedoHot = 0.0,
			const Scalar positionalLightPower = 1.0 )
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
					sceneUnitMeters, length, positionalLight, sootAlbedoHot,
					positionalLightPower );
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

	class SyntheticChemEmissionMedium :
		public virtual IMedium,
		public virtual Reference
	{
	public:
		mutable unsigned int segmentSamples;
		mutable unsigned int uniformTechniqueSamples;
		mutable unsigned int reactionTechniqueSamples;
		mutable unsigned int supportSamples;

		explicit SyntheticChemEmissionMedium(
			const Scalar sigmaT = 0.0 ) :
		  segmentSamples(0), uniformTechniqueSamples(0),
		  reactionTechniqueSamples(0), supportSamples(0),
		  sigmaT_(sigmaT)
		{}

		static Scalar ReactionStart() { return 0.45; }
		static Scalar ReactionEnd() { return 0.55; }
		static Scalar BandStartNM() { return 425.0; }
		static Scalar BandEndNM() { return 435.0; }
		static Scalar BandIntegratedSource() { return 8.0*PI; }

		static Scalar SpectralSource( const Point3& point, const Scalar nm )
		{
			if( point.z < ReactionStart() || point.z > ReactionEnd() ||
				nm < BandStartNM() || nm > BandEndNM() ) return 0.0;
			const Scalar normalizedSyntheticSPD =
				1.0/(BandEndNM()-BandStartNM());
			return BandIntegratedSource() * normalizedSyntheticSPD / (4.0*PI);
		}

		MediumCoefficients GetCoefficients( const Point3& ) const override
		{
			MediumCoefficients result;
			result.sigma_t = RISEPel(sigmaT_,sigmaT_,sigmaT_);
			result.sigma_s = RISEPel(0,0,0);
			result.emission = RISEPel(0,0,0);
			return result;
		}

		MediumCoefficientsNM GetCoefficientsNM(
			const Point3&, const Scalar ) const override
		{
			MediumCoefficientsNM result;
			result.sigma_t = sigmaT_;
			result.sigma_s = 0.0;
			result.emission = 0.0;
			return result;
		}

		const IPhaseFunction* GetPhaseFunction() const override { return nullptr; }

		Scalar SampleDistance(
			const Ray&, const Scalar maxDist, ISampler& sampler,
			bool& scattered ) const override
		{
			if( sigmaT_<=0.0 ) {
				scattered = false;
				return maxDist;
			}
			const Scalar t = -std::log1p(-sampler.Get1D())/sigmaT_;
			scattered = t<maxDist;
			return scattered ? t : maxDist;
		}

		Scalar SampleDistanceNM(
			const Ray& ray, const Scalar maxDist, const Scalar,
			ISampler& sampler, bool& scattered ) const override
		{
			return SampleDistance(ray,maxDist,sampler,scattered);
		}

		RISEPel EvalTransmittance(
			const Ray&, const Scalar distance ) const override
		{
			const Scalar transmittance = std::exp(-sigmaT_*distance);
			return RISEPel(transmittance,transmittance,transmittance);
		}

		Scalar EvalTransmittanceNM(
			const Ray&, const Scalar distance, const Scalar ) const override
		{
			return std::exp(-sigmaT_*distance);
		}

		Scalar EvalDistancePdfNM(
			const Ray&, const Scalar t, const bool scattered,
			const Scalar maxDist, const Scalar ) const override
		{
			return scattered
				? sigmaT_*std::exp(-sigmaT_*t)
				: std::exp(-sigmaT_*maxDist);
		}

		Scalar EvalLogDistancePdfNM(
			const Ray&, const Scalar t, const bool scattered,
			const Scalar maxDist, const Scalar ) const override
		{
			if( scattered ) {
				return sigmaT_>0.0
					? std::log(sigmaT_)-sigmaT_*t
					: -RISE_INFINITY;
			}
			return -sigmaT_*maxDist;
		}

		bool IsHomogeneous() const override { return false; }
		bool IsFireMedium() const override { return true; }

		bool GetBoundingBox( Point3& bbMin, Point3& bbMax ) const override
		{
			bbMin = Point3(-0.5,-0.5,0.0);
			bbMax = Point3(0.5,0.5,1.0);
			return true;
		}

		Scalar EstimateChemEmissionSegmentNM(
			const Ray& ray,
			const Scalar segmentStart,
			const Scalar segmentEnd,
			const Scalar nm,
			ISampler& sampler ) const override
		{
			const Scalar segmentLength = segmentEnd-segmentStart;
			if( segmentLength<=0.0 || ray.Dir().z<=0.0 ) return 0.0;
			const Scalar reactionStart = std::fmax(
				segmentStart,(ReactionStart()-ray.origin.z)/ray.Dir().z);
			const Scalar reactionEnd = std::fmin(
				segmentEnd,(ReactionEnd()-ray.origin.z)/ray.Dir().z);
			const Scalar reactionLength = reactionEnd-reactionStart;
			if( reactionLength<=0.0 ) return 0.0;

			++segmentSamples;
			const bool reactionTechnique = sampler.Get1D()>=0.5;
			const Scalar xi = sampler.Get1D();
			const Scalar t = reactionTechnique
				? reactionStart+xi*reactionLength
				: segmentStart+xi*segmentLength;
			if( reactionTechnique ) {
				++reactionTechniqueSamples;
			} else {
				++uniformTechniqueSamples;
			}
			const bool inReactionSupport =
				t>=reactionStart && t<=reactionEnd;
			if( inReactionSupport ) ++supportSamples;

			const Scalar qReaction = inReactionSupport
				? 1.0/reactionLength : 0.0;
			const Scalar qChem = 0.5/segmentLength + 0.5*qReaction;
			const Scalar epsilonChem =
				SpectralSource(ray.PointAtLength(t),nm);
			const Scalar logTransmittance = EvalLogDistancePdfNM(
				ray,t,false,t,nm);
			return std::exp(logTransmittance)*epsilonChem/qChem;
		}

	protected:
		~SyntheticChemEmissionMedium() override = default;

	private:
		const Scalar sigmaT_;
	};

	class TerminalChemEmissionMedium :
		public virtual IMedium,
		public virtual Reference
	{
	public:
		MediumCoefficients GetCoefficients( const Point3& ) const override
		{
			MediumCoefficients result;
			result.sigma_t = RISEPel(0,0,0);
			result.sigma_s = RISEPel(0,0,0);
			result.emission = RISEPel(0,0,0);
			return result;
		}

		MediumCoefficientsNM GetCoefficientsNM(
			const Point3&, const Scalar ) const override
		{
			MediumCoefficientsNM result;
			result.sigma_t = 0.0;
			result.sigma_s = 0.0;
			result.emission = 0.0;
			return result;
		}

		const IPhaseFunction* GetPhaseFunction() const override { return nullptr; }

		Scalar SampleDistance(
			const Ray&, const Scalar maxDist, ISampler&,
			bool& scattered ) const override
		{
			scattered = false;
			return maxDist;
		}

		Scalar SampleDistanceNM(
			const Ray& ray, const Scalar maxDist, const Scalar,
			ISampler& sampler, bool& scattered ) const override
		{
			return SampleDistance(ray,maxDist,sampler,scattered);
		}

		RISEPel EvalTransmittance(
			const Ray&, const Scalar ) const override
		{
			return RISEPel(1,1,1);
		}

		Scalar EvalTransmittanceNM(
			const Ray&, const Scalar, const Scalar ) const override
		{
			return 1.0;
		}

		bool IsHomogeneous() const override { return false; }
		bool IsFireMedium() const override { return true; }
		bool GetBoundingBox( Point3&, Point3& ) const override { return false; }

		Scalar EstimateChemEmissionSegmentNM(
			const Ray& ray,
			const Scalar,
			const Scalar,
			const Scalar,
			ISampler& ) const override
		{
			// The camera segment approaches the receiver from z=-1.  Only the
			// terminal Lambertian segment launched back into that hemisphere
			// carries this synthetic source.
			return ray.origin.z > -0.2 && ray.Dir().z < 0.0 ? 0.25 : 0.0;
		}

	protected:
		~TerminalChemEmissionMedium() override = default;
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

	enum class SurfaceReceiverMaterial
	{
		Lambertian,
		OrenNayar,
		IsotropicPhong
	};

	std::string SurfaceFireReceiverScene(
		const bool positionalLight = false,
		const SurfaceReceiverMaterial receiverMaterial =
			SurfaceReceiverMaterial::Lambertian )
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
			"uniformcolor_painter\n{\nname white\ncolor 0.8 0.8 0.8\n}\n";
		if( receiverMaterial == SurfaceReceiverMaterial::OrenNayar ) {
			scene <<
				"orennayar_material\n{\nname receiver\nreflectance white\n"
				"roughness 1.0\n}\n";
		} else if(
			receiverMaterial == SurfaceReceiverMaterial::IsotropicPhong ) {
			scene <<
				"uniformcolor_painter\n{\nname phong_diffuse\n"
				"color 0.25 0.25 0.25\n}\n"
				"uniformcolor_painter\n{\nname phong_glossy\n"
				"color 0.75 0.75 0.75\n}\n"
				"isotropic_phong_material\n{\nname receiver\n"
				"rd phong_diffuse\nrs phong_glossy\nN 1.0\n}\n";
		} else {
			scene <<
				"lambertian_material\n{\nname receiver\nreflectance white\n}\n";
		}
		scene <<
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

	std::string HollowCavityFireReceiverScene()
	{
		const Scalar carbon = kTargetSigmaSI / HotAbsorptionMass633();
		std::ostringstream scene;
		scene << std::setprecision(17) <<
			"RISE ASCII SCENE 7\n"
			"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n"
			"uniformcolor_painter\n{\nname white\ncolor 0.8 0.8 0.8\n}\n"
			"lambertian_material\n{\nname receiver\nreflectance white\n}\n"
			"null_boundary_material\n{\nname medium_boundary\n}\n"
			"homogeneous_medium\n{\nname cavity_vacuum\n"
			"absorption 0 0 0\nscattering 0 0 0\nphase isotropic\n}\n"
			"scalar_painter\n{\nname carbon\nvalue " << carbon << "\n}\n"
			"scalar_painter\n{\nname temperature\nvalue " << kTemperatureK << "\n}\n"
			"multichannel_heterogeneous_medium\n{\nname shell_fire\n"
			"channel_carbon painter carbon\nchannel_temperature painter temperature\n"
			"bake_resolution 4 4 4\nbbox_min -2 -2 -2\nbbox_max 2 2 2\n"
			"soot_em 0.26\nsoot_density 1800\nsoot_albedo_hot 0\nsoot_g_hot 0.5\n"
			"smoke_km_carbon 8.7\nsmoke_n_carbon 1.2\n"
			"smoke_albedo_carbon 0\nsmoke_g_carbon 0.6\n}\n"
			"global_medium\n{\nmedium shell_fire\n}\n"
			"sphere_geometry\n{\nname cavity_geometry\nradius 0.75\n}\n"
			"standard_object\n{\nname cavity\ngeometry cavity_geometry\n"
			"material medium_boundary\ninterior_medium cavity_vacuum\n"
			"casts_shadows TRUE\n}\n"
			"sphere_geometry\n{\nname receiver_geometry\nradius 0.12\n}\n"
			"standard_object\n{\nname receiver_probe\ngeometry receiver_geometry\n"
			"material receiver\ncasts_shadows TRUE\n}\n";
		return scene.str();
	}

	std::string NullBoundarySurvivalScene(
		const unsigned int segmentCount,
		const bool downstreamEmitter,
		const bool includePivot )
	{
		std::ostringstream scene;
		scene << std::setprecision(17) <<
			"RISE ASCII SCENE 7\n"
			"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n"
			"homogeneous_medium\n{\nname fog\n"
			"absorption 0.2 0.2 0.2\nscattering 0 0 0\nphase isotropic\n}\n"
			"null_boundary_material\n{\nname medium_boundary\n}\n"
			;
		if( includePivot ) {
			scene <<
				"omni_light\n{\nname mixture_pivot\npower 0.000000000001\n"
				"color 1 1 1\nposition 2 0 0\n}\n";
		}
		for( unsigned int i = 0; i < segmentCount; ++i ) {
			const Scalar radius = 0.35*static_cast<Scalar>(i+1);
			scene <<
				"sphere_geometry\n{\nname boundary_geometry_" << i <<
				"\nradius " << radius << "\n}\n"
				"standard_object\n{\nname boundary_object_" << i <<
				"\ngeometry boundary_geometry_" << i <<
				"\nmaterial medium_boundary\ninterior_medium fog\n"
				"casts_shadows FALSE\n}\n";
		}
		if( downstreamEmitter ) {
			const Scalar z = 0.35*static_cast<Scalar>(segmentCount)+0.5;
			scene <<
				"uniformcolor_painter\n{\nname target_exitance\n"
				"color 1 1 1\n}\n"
				"lambertian_luminaire_material\n{\nname target_material\n"
				"exitance target_exitance\nscale 1\nmaterial none\n}\n"
				"clippedplane_geometry\n{\nname target_geometry\n"
				"pta -1 1 " << z << "\nptb 1 1 " << z <<
				"\nptc 1 -1 " << z << "\nptd -1 -1 " << z << "\n}\n"
				"standard_object\n{\nname target_object\n"
				"geometry target_geometry\nmaterial target_material\n}\n";
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
		explicit TwoEventFireMedium(
			IPhaseFunction& phase,
			const bool chemOnlyContinuation = false ) :
		  chemSegmentCalls( 0 ),
		  pPhase( &phase ),
		  chemOnlyContinuation_( chemOnlyContinuation )
		{
			pPhase->addref();
		}

		mutable unsigned int chemSegmentCalls;

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
			return !chemOnlyContinuation_ && pt.z > 0.5 ? 1.0 : 0.0;
		}
		Scalar EstimateChemEmissionSegmentNM(
			const Ray& ray,
			const Scalar segmentStart,
			const Scalar segmentEnd,
			const Scalar,
			ISampler& ) const override
		{
			if( !chemOnlyContinuation_ || ray.Dir().z <= 0.0 ) return 0.0;
			const Scalar supportStart = std::fmax(
				segmentStart,(0.45-ray.origin.z)/ray.Dir().z);
			const Scalar supportEnd = std::fmin(
				segmentEnd,(0.55-ray.origin.z)/ray.Dir().z);
			if( supportEnd <= supportStart ) return 0.0;
			++chemSegmentCalls;
			return 0.2*(supportEnd-supportStart);
		}

	protected:
		~TwoEventFireMedium() override { safe_release( pPhase ); }

	private:
		IPhaseFunction* pPhase;
		const bool chemOnlyContinuation_;
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

	class ImmersedDistanceMixtureFireMedium :
		public virtual IMedium,
		public virtual Reference
	{
	public:
		mutable unsigned int deltaTrackingCalls;

		ImmersedDistanceMixtureFireMedium() : deltaTrackingCalls(0) {}

		static Scalar SigmaT() { return 0.8; }
		static Scalar SigmaS() { return 0.4; }
		static Scalar ThermalEmission() { return 2.0; }

		MediumCoefficients GetCoefficients( const Point3& ) const override
		{
			MediumCoefficients result;
			result.sigma_t = RISEPel(SigmaT(),SigmaT(),SigmaT());
			result.sigma_s = RISEPel(SigmaS(),SigmaS(),SigmaS());
			result.emission = RISEPel(0,0,0);
			return result;
		}

		MediumCoefficientsNM GetCoefficientsNM(
			const Point3&, const Scalar ) const override
		{
			MediumCoefficientsNM result;
			result.sigma_t = SigmaT();
			result.sigma_s = SigmaS();
			result.emission = 0.0;
			return result;
		}

		const IPhaseFunction* GetPhaseFunction() const override { return nullptr; }

		const IPhaseFunction* MakePhaseClosure(
			const Point3&, const Scalar ) const override
		{
			return new IsotropicPhaseFunction();
		}

		Scalar SampleDistance(
			const Ray& ray, const Scalar maxDist,
			ISampler& sampler, bool& scattered ) const override
		{
			return SampleDistanceNM(ray,maxDist,500.0,sampler,scattered);
		}

		Scalar SampleDistanceNM(
			const Ray&, const Scalar maxDist, const Scalar,
			ISampler& sampler, bool& scattered ) const override
		{
			const Scalar t = -std::log1p(-sampler.Get1D())/SigmaT();
			scattered = t < maxDist;
			return scattered ? t : maxDist;
		}

		DistanceSample SampleDistanceWithPdfNM(
			const Ray& ray, const Scalar maxDist, const Scalar nm,
			ISampler& sampler ) const override
		{
			++deltaTrackingCalls;
			DistanceSample result;
			result.t = SampleDistanceNM(ray,maxDist,nm,sampler,result.scattered);
			result.pdf = result.scattered ?
				SigmaT()*std::exp(-SigmaT()*result.t) :
				std::exp(-SigmaT()*maxDist);
			return result;
		}

		Scalar EvalDistancePdfNM(
			const Ray&, const Scalar t, const bool scattered,
			const Scalar maxDist, const Scalar ) const override
		{
			return scattered ?
				SigmaT()*std::exp(-SigmaT()*t) :
				std::exp(-SigmaT()*maxDist);
		}

		Scalar EvalLogDistancePdfNM(
			const Ray&, const Scalar t, const bool scattered,
			const Scalar maxDist, const Scalar ) const override
		{
			return scattered ?
				std::log(SigmaT())-SigmaT()*t :
				-SigmaT()*maxDist;
		}

		RISEPel EvalTransmittance( const Ray&, const Scalar dist ) const override
		{
			const Scalar tr = std::exp(-SigmaT()*dist);
			return RISEPel(tr,tr,tr);
		}

		Scalar EvalTransmittanceNM(
			const Ray&, const Scalar dist, const Scalar ) const override
		{
			return std::exp(-SigmaT()*dist);
		}

		bool IsHomogeneous() const override { return true; }

		bool GetBoundingBox( Point3& bbMin, Point3& bbMax ) const override
		{
			bbMin = Point3(-0.5,-0.5,-1.0);
			bbMax = Point3(0.5,0.5,2.0);
			return true;
		}

		bool IsFireMedium() const override { return true; }

		Scalar GetThermalEmissionNM(
			const Point3&, const Scalar ) const override
		{
			return ThermalEmission();
		}

	protected:
		~ImmersedDistanceMixtureFireMedium() override = default;
	};

	class BackwardPhaseFunction :
		public virtual IPhaseFunction,
		public virtual Reference
	{
	private:
		static Vector3 Direction()
		{
			return Vector3(0.1,0.0,-std::sqrt(Scalar(0.99)));
		}

		static bool IsBackwardDirection( const Vector3& direction )
		{
			return Vector3Ops::Dot(direction,Direction()) >
				1.0-1e-12;
		}

	public:
		Scalar Evaluate( const Vector3&, const Vector3& wo ) const override
		{
			return IsBackwardDirection(wo) ? 1.0 : 0.0;
		}

		Vector3 Sample( const Vector3&, ISampler& ) const override
		{
			return Direction();
		}

		Scalar Pdf( const Vector3&, const Vector3& wo ) const override
		{
			return IsBackwardDirection(wo) ? 1.0 : 0.0;
		}

	protected:
		~BackwardPhaseFunction() override = default;
	};

	class ContinuationDistanceMixtureFireMedium :
		public ImmersedDistanceMixtureFireMedium
	{
	private:
		static bool Inside( const Point3& pt )
		{
			return pt.x >= -0.5 && pt.x <= 0.5 &&
				pt.y >= -0.5 && pt.y <= 0.5 &&
				pt.z >= -1.0 && pt.z <= 2.0;
		}

		static Scalar SegmentLength( const Ray& ray, const Scalar maxDist )
		{
			Point3 bbMin(-0.5,-0.5,-1.0);
			Point3 bbMax(0.5,0.5,2.0);
			Scalar tNear = 0.0;
			Scalar tFar = maxDist;
			for( unsigned int axis=0; axis<3; ++axis ) {
				const Scalar direction = ray.Dir()[axis];
				if( std::fabs(direction) <= 1e-20 ) {
					if( ray.origin[axis] < bbMin[axis] ||
						ray.origin[axis] > bbMax[axis] ) return 0.0;
					continue;
				}
				Scalar t0 = (bbMin[axis]-ray.origin[axis])/direction;
				Scalar t1 = (bbMax[axis]-ray.origin[axis])/direction;
				if( t0>t1 ) std::swap(t0,t1);
				tNear = std::fmax(tNear,t0);
				tFar = std::fmin(tFar,t1);
				if( tNear>=tFar ) return 0.0;
			}
			return std::fmax(Scalar(0.0),tFar-tNear);
		}

	public:
		mutable unsigned int primaryDeltaTrackingCalls;

		ContinuationDistanceMixtureFireMedium() :
			primaryDeltaTrackingCalls(0)
		{}

		MediumCoefficients GetCoefficients( const Point3& pt ) const override
		{
			if( Inside(pt) ) {
				return ImmersedDistanceMixtureFireMedium::GetCoefficients(pt);
			}
			return MediumCoefficients();
		}

		MediumCoefficientsNM GetCoefficientsNM(
			const Point3& pt, const Scalar nm ) const override
		{
			if( Inside(pt) ) {
				return ImmersedDistanceMixtureFireMedium::GetCoefficientsNM(pt,nm);
			}
			return MediumCoefficientsNM();
		}

		const IPhaseFunction* MakePhaseClosure(
			const Point3&, const Scalar ) const override
		{
			return new BackwardPhaseFunction();
		}

		Scalar SampleDistance(
			const Ray& ray, const Scalar maxDist,
			ISampler& sampler, bool& scattered ) const override
		{
			return SampleDistanceNM(ray,maxDist,500.0,sampler,scattered);
		}

		Scalar SampleDistanceNM(
			const Ray& ray, const Scalar maxDist, const Scalar,
			ISampler& sampler, bool& scattered ) const override
		{
			const Scalar mediumLength = SegmentLength(ray,maxDist);
			if( mediumLength<=0.0 ) {
				scattered = false;
				return maxDist;
			}
			const Scalar t = -std::log1p(-sampler.Get1D())/SigmaT();
			scattered = t<mediumLength;
			return scattered ? t : maxDist;
		}

		DistanceSample SampleDistanceWithPdfNM(
			const Ray& ray, const Scalar maxDist, const Scalar nm,
			ISampler& sampler ) const override
		{
			++deltaTrackingCalls;
			if( std::fabs(ray.origin.x)<1e-14 &&
				std::fabs(ray.origin.y)<1e-14 &&
				std::fabs(ray.origin.z)<1e-14 &&
				ray.Dir().z>1.0-1e-14 ) {
				++primaryDeltaTrackingCalls;
			}
			DistanceSample result;
			result.t = SampleDistanceNM(
				ray,maxDist,nm,sampler,result.scattered);
			result.pdf = EvalDistancePdfNM(
				ray,result.t,result.scattered,maxDist,nm);
			return result;
		}

		Scalar EvalDistancePdfNM(
			const Ray& ray, const Scalar t, const bool scattered,
			const Scalar maxDist, const Scalar ) const override
		{
			const Scalar distance = scattered ? t : SegmentLength(ray,maxDist);
			return scattered ?
				SigmaT()*std::exp(-SigmaT()*distance) :
				std::exp(-SigmaT()*distance);
		}

		Scalar EvalLogDistancePdfNM(
			const Ray& ray, const Scalar t, const bool scattered,
			const Scalar maxDist, const Scalar ) const override
		{
			const Scalar distance = scattered ? t : SegmentLength(ray,maxDist);
			return scattered ?
				std::log(SigmaT())-SigmaT()*distance :
				-SigmaT()*distance;
		}

		RISEPel EvalTransmittance(
			const Ray& ray, const Scalar maxDist ) const override
		{
			const Scalar tr =
				std::exp(-SigmaT()*SegmentLength(ray,maxDist));
			return RISEPel(tr,tr,tr);
		}

		Scalar EvalTransmittanceNM(
			const Ray& ray, const Scalar maxDist, const Scalar ) const override
		{
			return std::exp(-SigmaT()*SegmentLength(ray,maxDist));
		}

		Scalar GetThermalEmissionNM(
			const Point3&, const Scalar ) const override
		{
			return 0.0;
		}

	protected:
		~ContinuationDistanceMixtureFireMedium() override = default;
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

	void TestImmersedReceiverDeltaTrackingCarriesDistanceMixture()
	{
		std::cout << "TestImmersedReceiverDeltaTrackingCarriesDistanceMixture" << std::endl;
		Fixture fixture;
		Check( fixture.Initialize("immersed_distance_mixture",1.0,1.0,false,0.1),
			"immersed-receiver fixture prepares a thermal pivot distribution" );
		if( !fixture.job || !fixture.caster || !fixture.integrator ) return;

		IRayCaster* neeCaster = nullptr;
		IRayCaster* marchOnlyCaster = nullptr;
		IShader* globalShader = fixture.job->GetShaders()->GetItem("global");
		const IMedium* preparedFire =
			fixture.job->GetScene()->GetGlobalMedium();
		if( preparedFire ) preparedFire->addref();
		IsotropicPhaseFunction* vacuumPhase = new IsotropicPhaseFunction();
		UnsupportedHomogeneousSmoke* preparationVacuum =
			new UnsupportedHomogeneousSmoke(*vacuumPhase,0.0,0.0);
		fixture.job->GetScene()->SetGlobalMedium(preparationVacuum);
		Check( globalShader && RISE_API_CreateRayCaster(
			&marchOnlyCaster,false,8,*globalShader,true) && marchOnlyCaster,
			"immersed-receiver march reference prepares without a thermal CDF" );
		if( marchOnlyCaster ) {
			marchOnlyCaster->AttachScene(fixture.job->GetScene());
		}
		fixture.job->GetScene()->SetGlobalMedium(preparedFire);
		safe_release(preparationVacuum);
		safe_release(vacuumPhase);
		Check( globalShader && RISE_API_CreateRayCaster(
			&neeCaster,false,8,*globalShader,true) && neeCaster,
			"immersed-receiver NEE route prepares the active fire CDF" );
		if( neeCaster ) neeCaster->AttachScene(fixture.job->GetScene());
		safe_release(preparedFire);
		if( !neeCaster || !marchOnlyCaster ) {
			safe_release(neeCaster);
			safe_release(marchOnlyCaster);
			return;
		}

		const LightSampler* lights = neeCaster->GetLightSampler();
		const LightSampler* referenceLights = marchOnlyCaster->GetLightSampler();
		Check( lights && lights->GetVolumeEmissionMediumCount()==1 &&
			lights->GetEquiangularPivotEntryCount()==1,
			"immersed-receiver fixture has one thermal endpoint and pivot label" );
		Check( referenceLights &&
			referenceLights->GetVolumeEmissionMediumCount()==0 &&
			referenceLights->GetEquiangularPivotEntryCount()==0,
			"immersed-receiver march reference has no endpoint or pivot labels" );

		const Scalar nm = 500.0;
		const Point3 receiver(0,0,0);
		const IMedium* activeFire =
			fixture.job->GetScene()->GetGlobalMedium();
		const MediumCoefficientsNM activeReceiverCoefficients = activeFire ?
			activeFire->GetCoefficientsNM(receiver,nm) : MediumCoefficientsNM();
		Check( activeFire && activeFire->IsFireMedium() &&
			activeReceiverCoefficients.sigma_t>0.0 &&
			activeReceiverCoefficients.sigma_s>0.0 &&
			activeFire->GetThermalEmissionNM(receiver,nm)>0.0,
			"real immersed receiver is scattering, extinguishing, and emissive" );

		const unsigned int equalitySamples = 160000;
		const RasterizerState rast = {0,0};
		const Ray ray(receiver,Vector3(0,0,1));
		struct SampleMoments
		{
			Scalar mean;
			Scalar variance;
		};
		auto momentsPT = [&]( PathTracingIntegrator& integrator,
			const IRayCaster& route, const unsigned int seed,
			const unsigned int samples ) {
			RandomNumberGenerator rng(seed);
			RuntimeContext rc(rng,RuntimeContext::PASS_NORMAL,false);
			IndependentSampler sampler(rng);
			long double sum = 0.0;
			long double sumSquares = 0.0;
			for( unsigned int i=0; i<samples; ++i ) {
				const Scalar value = integrator.IntegrateRayNM(
					rc,rast,ray,nm,*fixture.job->GetScene(),route,
					sampler,nullptr,nullptr);
				sum += value;
				sumSquares += static_cast<long double>(value)*value;
			}
			const long double count = static_cast<long double>(samples);
			const long double mean = sum/count;
			const long double variance =
				(sumSquares-sum*sum/count)/(count-1.0);
			return SampleMoments{
				static_cast<Scalar>(mean),
				static_cast<Scalar>(variance>0.0 ? variance : 0.0)
			};
		};
		auto momentsShaderRoute = [&]( const IRayCaster& route,
			const unsigned int seed, const unsigned int samples,
			const unsigned int volumeBounces ) {
			RandomNumberGenerator rng(seed);
			RuntimeContext rc(rng,RuntimeContext::PASS_NORMAL,false);
			IRayCaster::RAY_STATE state;
			state.volumeBounces = volumeBounces;
			long double sum = 0.0;
			long double sumSquares = 0.0;
			for( unsigned int i=0; i<samples; ++i ) {
				Scalar value = 0.0;
				route.CastRayNM(
					rc,rast,ray,value,state,nm,nullptr,nullptr);
				sum += value;
				sumSquares += static_cast<long double>(value)*value;
			}
			const long double count = static_cast<long double>(samples);
			const long double mean = sum/count;
			const long double variance =
				(sumSquares-sum*sum/count)/(count-1.0);
			return SampleMoments{
				static_cast<Scalar>(mean),
				static_cast<Scalar>(variance>0.0 ? variance : 0.0)
			};
		};
		auto withinSixStandardErrors = []( const SampleMoments& a,
			const SampleMoments& b, const unsigned int samples ) {
			const Scalar standardError = std::sqrt(
				(a.variance+b.variance)/static_cast<Scalar>(samples));
			return standardError>0.0 &&
				std::fabs(a.mean-b.mean)<=6.0*standardError;
		};

		StabilityConfig ordinaryConfig;
		ordinaryConfig.maxVolumeBounce = 8;
		PathTracingIntegrator* ordinaryIntegrator =
			new PathTracingIntegrator(ManifoldSolverConfig(),ordinaryConfig);
		ordinaryIntegrator->SetMaxPathDepth(8);
		const SampleMoments ordinaryPTOn = momentsPT(
			*ordinaryIntegrator,*neeCaster,0x1aae441u,equalitySamples);
		const SampleMoments ordinaryPTOff = momentsPT(
			*ordinaryIntegrator,*marchOnlyCaster,0x1aae442u,equalitySamples);
		const SampleMoments ordinaryShaderOn = momentsShaderRoute(
			*neeCaster,0x1aae443u,equalitySamples,0);
		const SampleMoments ordinaryShaderOff = momentsShaderRoute(
			*marchOnlyCaster,0x1aae444u,equalitySamples,0);
		std::cout << "  immersed NEE/march PT=" << ordinaryPTOn.mean << "/" <<
			ordinaryPTOff.mean << " shader=" << ordinaryShaderOn.mean << "/" <<
			ordinaryShaderOff.mean << " variances PT=" << ordinaryPTOn.variance <<
			"/" << ordinaryPTOff.variance << " shader=" <<
			ordinaryShaderOn.variance << "/" << ordinaryShaderOff.variance <<
			std::endl;
		Check( ordinaryPTOn.mean>0.0 && ordinaryPTOff.mean>0.0 &&
			ordinaryShaderOn.mean>0.0 && ordinaryShaderOff.mean>0.0 &&
			withinSixStandardErrors(
				ordinaryPTOn,ordinaryPTOff,equalitySamples) &&
			withinSixStandardErrors(
				ordinaryShaderOn,ordinaryShaderOff,equalitySamples) &&
			withinSixStandardErrors(
				ordinaryPTOn,ordinaryShaderOn,equalitySamples),
			"real immersed-fire NEE and march agree within MC noise in both PT entries" );

		StabilityConfig cappedConfig;
		cappedConfig.maxVolumeBounce = 0;
		PathTracingIntegrator* cappedIntegrator =
			new PathTracingIntegrator(ManifoldSolverConfig(),cappedConfig);
		cappedIntegrator->SetMaxPathDepth(2);
		const unsigned int activationSamples = 80000;
		const SampleMoments cappedPTOn = momentsPT(
			*cappedIntegrator,*neeCaster,0x1aae445u,activationSamples);
		const SampleMoments cappedPTOff = momentsPT(
			*cappedIntegrator,*marchOnlyCaster,0x1aae446u,activationSamples);
		const SampleMoments cappedShaderOn = momentsShaderRoute(
			*neeCaster,0x1aae447u,activationSamples,64);
		const SampleMoments cappedShaderOff = momentsShaderRoute(
			*marchOnlyCaster,0x1aae448u,activationSamples,64);
		std::cout << "  immersed capped NEE/march PT=" << cappedPTOn.mean <<
			"/" << cappedPTOff.mean << " shader=" << cappedShaderOn.mean <<
			"/" << cappedShaderOff.mean << std::endl;
		auto neeSignalExceedsSixStandardErrors = [](
			const SampleMoments& on, const SampleMoments& off,
			const unsigned int samples ) {
			const Scalar standardError = std::sqrt(
				(on.variance+off.variance)/static_cast<Scalar>(samples));
			return standardError>0.0 &&
				on.mean-off.mean>6.0*standardError;
		};
		Check( neeSignalExceedsSixStandardErrors(
				cappedPTOn,cappedPTOff,activationSamples) &&
			neeSignalExceedsSixStandardErrors(
				cappedShaderOn,cappedShaderOff,activationSamples),
			"capped real-fire receiver proves immersed volume NEE is active in both PT entries" );
		safe_release(cappedIntegrator);
		safe_release(ordinaryIntegrator);

		ImmersedDistanceMixtureFireMedium* probe =
			new ImmersedDistanceMixtureFireMedium();
		fixture.job->GetScene()->SetGlobalMedium(probe);
		const MediumCoefficientsNM receiverCoefficients =
			probe->GetCoefficientsNM(receiver,nm);
		Check( receiverCoefficients.sigma_t>0.0 &&
			receiverCoefficients.sigma_s>0.0,
			"immersed receiver resolves inside nonzero-extinction scattering fire" );

		bool octants[8] = {false,false,false,false,false,false,false,false};
		if( lights ) {
			RandomNumberGenerator pivotRng(0x1aae451u);
			IndependentSampler pivotSampler(pivotRng);
			for( unsigned int i=0; i<4096; ++i ) {
				VolumeEmissionPivotState pivots;
				if( !lights->SampleVolumeEmissionPivots(pivotSampler,pivots) ||
					pivots.mediumPivots.size()!=1 ) continue;
				const Point3& pivot = pivots.mediumPivots[0];
				const unsigned int octant =
					(pivot.x>=receiver.x ? 1u : 0u) |
					(pivot.y>=receiver.y ? 2u : 0u) |
					(pivot.z>=receiver.z ? 4u : 0u);
				octants[octant] = true;
			}
		}
		bool pivotsSurroundReceiver = true;
		for( unsigned int i=0; i<8; ++i ) pivotsSurroundReceiver &= octants[i];
		Check( pivotsSurroundReceiver,
			"thermal pivots surround the immersed receiver in every octant" );

		struct StrategyBoundarySeed
		{
			unsigned int seed;
			Scalar xi;
			StrategyBoundarySeed() : seed(0), xi(0.0) {}
		};
		StrategyBoundarySeed belowBoundary;
		StrategyBoundarySeed aboveBoundary;
		if( lights ) {
			for( unsigned int candidate=1;
				candidate<500000 &&
					(!belowBoundary.seed || !aboveBoundary.seed);
				++candidate ) {
				RandomNumberGenerator auditRng(candidate);
				IndependentSampler auditSampler(auditRng);
				VolumeEmissionPivotState auditPivots;
				Point3 auditPivot;
				Scalar auditPivotPdf = 0.0;
				if( !lights->ResolveVolumeEmissionPivots(
						auditSampler,nullptr,auditPivots) ||
					!lights->SampleEquiangularPivot(
						auditPivots,auditSampler.Get1D(),
						auditPivot,auditPivotPdf) ) continue;
				const Scalar xiStrategy = auditSampler.Get1D();
				if( !belowBoundary.seed &&
					xiStrategy>=0.499 && xiStrategy<0.5 ) {
					belowBoundary.seed = candidate;
					belowBoundary.xi = xiStrategy;
				}
				if( !aboveBoundary.seed &&
					xiStrategy>=0.5 && xiStrategy<0.501 ) {
					aboveBoundary.seed = candidate;
					aboveBoundary.xi = xiStrategy;
				}
			}
		}
		Check( belowBoundary.seed && aboveBoundary.seed &&
			belowBoundary.xi<0.5 && aboveBoundary.xi>=0.5,
			"deterministic strategy seeds straddle the pinned 50/50 boundary" );
		auto ptUsesDeltaTracking = [&]( const unsigned int seed ) {
			const unsigned int callsBefore = probe->deltaTrackingCalls;
			RandomNumberGenerator rng(seed);
			RuntimeContext rc(rng,RuntimeContext::PASS_NORMAL,false);
			IndependentSampler sampler(rng);
			fixture.integrator->IntegrateRayNM(
				rc,rast,ray,nm,*fixture.job->GetScene(),*neeCaster,
				sampler,nullptr,nullptr);
			return probe->deltaTrackingCalls==callsBefore+1;
		};
		auto shaderUsesDeltaTracking = [&]( const unsigned int seed ) {
			const unsigned int callsBefore = probe->deltaTrackingCalls;
			RandomNumberGenerator rng(seed);
			RuntimeContext rc(rng,RuntimeContext::PASS_NORMAL,false);
			IRayCaster::RAY_STATE state;
			state.volumeBounces = 64;
			Scalar value = 0.0;
			neeCaster->CastRayNM(
				rc,rast,ray,value,state,nm,nullptr,nullptr);
			return probe->deltaTrackingCalls==callsBefore+1;
		};
		if( belowBoundary.seed && aboveBoundary.seed ) {
			Check( ptUsesDeltaTracking(belowBoundary.seed) &&
				!ptUsesDeltaTracking(aboveBoundary.seed) &&
				shaderUsesDeltaTracking(belowBoundary.seed) &&
				!shaderUsesDeltaTracking(aboveBoundary.seed),
				"both PT entries switch techniques exactly at xi = 0.5" );
		}

		const unsigned int samples = 80000;
		const Scalar segmentLength = 1.0;
		const Scalar expected =
			ImmersedDistanceMixtureFireMedium::ThermalEmission() /
			ImmersedDistanceMixtureFireMedium::SigmaT() *
			(-std::expm1(
				-ImmersedDistanceMixtureFireMedium::SigmaT()*segmentLength));
		struct BranchMoments
		{
			long double sum;
			long double sumSquares;
			long double deltaTrackingSum;
			unsigned int deltaTrackingSelections;
			unsigned int deltaTrackingPositiveSamples;
			BranchMoments() :
				sum(0.0), sumSquares(0.0), deltaTrackingSum(0.0),
				deltaTrackingSelections(0), deltaTrackingPositiveSamples(0)
			{}
		};
		auto measurePT = [&]( const unsigned int seed ) {
			BranchMoments result;
			RandomNumberGenerator rng(seed);
			RuntimeContext rc(rng,RuntimeContext::PASS_NORMAL,false);
			IndependentSampler sampler(rng);
			for( unsigned int i=0; i<samples; ++i ) {
				const unsigned int callsBefore = probe->deltaTrackingCalls;
				const Scalar value = fixture.integrator->IntegrateRayNM(
					rc,rast,ray,nm,*fixture.job->GetScene(),*neeCaster,
					sampler,nullptr,nullptr);
				const bool usedDeltaTracking =
					probe->deltaTrackingCalls==callsBefore+1;
				result.sum += value;
				result.sumSquares += static_cast<long double>(value)*value;
				if( usedDeltaTracking ) {
					++result.deltaTrackingSelections;
					result.deltaTrackingSum += value;
					if( value>0.0 ) ++result.deltaTrackingPositiveSamples;
				}
			}
			return result;
		};
		auto measureShaderRoute = [&]( const unsigned int seed ) {
			BranchMoments result;
			RandomNumberGenerator rng(seed);
			RuntimeContext rc(rng,RuntimeContext::PASS_NORMAL,false);
			IRayCaster::RAY_STATE state;
			state.volumeBounces = 64;
			for( unsigned int i=0; i<samples; ++i ) {
				const unsigned int callsBefore = probe->deltaTrackingCalls;
				Scalar value = 0.0;
				neeCaster->CastRayNM(
					rc,rast,ray,value,state,nm,nullptr,nullptr);
				const bool usedDeltaTracking =
					probe->deltaTrackingCalls==callsBefore+1;
				result.sum += value;
				result.sumSquares += static_cast<long double>(value)*value;
				if( usedDeltaTracking ) {
					++result.deltaTrackingSelections;
					result.deltaTrackingSum += value;
					if( value>0.0 ) ++result.deltaTrackingPositiveSamples;
				}
			}
			return result;
		};
		auto mean = [&]( const BranchMoments& result ) {
			return static_cast<Scalar>(
				result.sum/static_cast<long double>(samples));
		};
		auto variance = [&]( const BranchMoments& result ) {
			const long double count = static_cast<long double>(samples);
			const long double value =
				(result.sumSquares-result.sum*result.sum/count)/(count-1.0);
			return static_cast<Scalar>(value>0.0 ? value : 0.0);
		};
		auto deltaTrackingFraction = [&]( const BranchMoments& result ) {
			return result.sum>0.0 ?
				static_cast<Scalar>(result.deltaTrackingSum/result.sum) : 0.0;
		};
		auto selectionFrequency = [&]( const BranchMoments& result ) {
			return static_cast<Scalar>(result.deltaTrackingSelections)/
				static_cast<Scalar>(samples);
		};

		const BranchMoments pt = measurePT(0x1aae452u);
		const BranchMoments shader = measureShaderRoute(0x1aae453u);
		const Scalar ptMean = mean(pt);
		const Scalar shaderMean = mean(shader);
		const Scalar ptVariance = variance(pt);
		const Scalar shaderVariance = variance(shader);
		const Scalar ptDTFraction = deltaTrackingFraction(pt);
		const Scalar shaderDTFraction = deltaTrackingFraction(shader);
		const Scalar selectionTolerance =
			6.0*0.5/std::sqrt(static_cast<Scalar>(samples));
		const Scalar combinedStandardError = std::sqrt(
			(ptVariance+shaderVariance)/static_cast<Scalar>(samples));
		std::cout << "  immersed expected=" << expected <<
			" PT/shader=" << ptMean << "/" << shaderMean <<
			" DT selection=" << selectionFrequency(pt) << "/" <<
			selectionFrequency(shader) << " DT contribution=" <<
			ptDTFraction << "/" << shaderDTFraction << std::endl;

		Check( std::fabs(selectionFrequency(pt)-0.5)<=selectionTolerance &&
			std::fabs(selectionFrequency(shader)-0.5)<=selectionTolerance,
			"immersed-receiver distance sampler selects DT at its pinned 50 percent mass" );
		Check( pt.deltaTrackingPositiveSamples>0 &&
			shader.deltaTrackingPositiveSamples>0 &&
			ptDTFraction>0.10 && shaderDTFraction>0.10,
			"delta-tracking half carries nonzero immersed-fire radiance in both PT entries" );
		Check( std::fabs(ptMean-expected)<=
				6.0*std::sqrt(ptVariance/static_cast<Scalar>(samples)) &&
			std::fabs(shaderMean-expected)<=
				6.0*std::sqrt(shaderVariance/static_cast<Scalar>(samples)),
			"immersed 50/50 distance mixture matches the analytic thermal slab mean" );
		Check( combinedStandardError>0.0 &&
			std::fabs(ptMean-shaderMean)<=6.0*combinedStandardError,
			"PT rasterizer and shader-dispatch entries agree for the immersed mixture" );

		ContinuationDistanceMixtureFireMedium* continuationProbe =
			new ContinuationDistanceMixtureFireMedium();
		fixture.job->GetScene()->SetGlobalMedium(continuationProbe);
		IPainter* environmentPainter = nullptr;
		IRadianceMap* environment = nullptr;
		Check( RISE_API_CreateUniformColorPainter(
				&environmentPainter,RISEPel(1.0,1.0,1.0),
				eSpectrumKind_Unbounded) && environmentPainter &&
			RISE_API_CreateRadianceMap(
				&environment,*environmentPainter,1.0) && environment,
			"continuation-density probe creates a constant spectral environment" );

		StabilityConfig continuationConfig;
		continuationConfig.maxVolumeBounce = 1;
		PathTracingIntegrator* continuationIntegrator =
			new PathTracingIntegrator(
				ManifoldSolverConfig(),continuationConfig);
		continuationIntegrator->SetMaxPathDepth(4);
		const unsigned int continuationSamples = 240000;
		auto continuationPTMoments = [&]( const unsigned int seed ) {
			RandomNumberGenerator rng(seed);
			RuntimeContext rc(rng,RuntimeContext::PASS_NORMAL,false);
			IndependentSampler sampler(rng);
			long double sum = 0.0;
			long double sumSquares = 0.0;
			for( unsigned int i=0; i<continuationSamples; ++i ) {
				const Scalar value = continuationIntegrator->IntegrateRayNM(
					rc,rast,ray,nm,*fixture.job->GetScene(),*neeCaster,
					sampler,environment,nullptr);
				sum += value;
				sumSquares += static_cast<long double>(value)*value;
			}
			const long double count =
				static_cast<long double>(continuationSamples);
			const long double sampleMean = sum/count;
			const long double sampleVariance =
				(sumSquares-sum*sum/count)/(count-1.0);
			return SampleMoments{
				static_cast<Scalar>(sampleMean),
				static_cast<Scalar>(
					sampleVariance>0.0 ? sampleVariance : 0.0)
			};
		};
		auto continuationShaderMoments = [&]( const unsigned int seed ) {
			RandomNumberGenerator rng(seed);
			RuntimeContext rc(rng,RuntimeContext::PASS_NORMAL,false);
			IRayCaster::RAY_STATE state;
			state.volumeBounces = 63;
			long double sum = 0.0;
			long double sumSquares = 0.0;
			for( unsigned int i=0; i<continuationSamples; ++i ) {
				Scalar value = 0.0;
				neeCaster->CastRayNM(
					rc,rast,ray,value,state,nm,nullptr,environment);
				sum += value;
				sumSquares += static_cast<long double>(value)*value;
			}
			const long double count =
				static_cast<long double>(continuationSamples);
			const long double sampleMean = sum/count;
			const long double sampleVariance =
				(sumSquares-sum*sum/count)/(count-1.0);
			return SampleMoments{
				static_cast<Scalar>(sampleMean),
				static_cast<Scalar>(
					sampleVariance>0.0 ? sampleVariance : 0.0)
			};
		};
		if( environment ) {
			const Scalar backwardZ = std::sqrt(Scalar(0.99));
			const Vector3 backwardDirection(0.1,0.0,-backwardZ);
			const Scalar environmentNM = environment->GetRadianceNM(
				Ray(receiver,backwardDirection),rast,nm);
			const Scalar attenuationRate =
				ImmersedDistanceMixtureFireMedium::SigmaT()*
				(1.0+1.0/backwardZ);
			const Scalar continuationExpected =
				environmentNM*
				ImmersedDistanceMixtureFireMedium::SigmaS()*
				std::exp(
					-ImmersedDistanceMixtureFireMedium::SigmaT()/backwardZ)*
				(-std::expm1(-attenuationRate))/attenuationRate;
			const SampleMoments continuationPT =
				continuationPTMoments(0x1aae454u);
			const SampleMoments continuationShader =
				continuationShaderMoments(0x1aae455u);
			const Scalar continuationCombinedSE = std::sqrt(
				(continuationPT.variance+continuationShader.variance)/
				static_cast<Scalar>(continuationSamples));
			std::cout << "  immersed continuation expected=" <<
				continuationExpected << " PT/shader=" <<
				continuationPT.mean << "/" << continuationShader.mean <<
				" variances=" << continuationPT.variance << "/" <<
				continuationShader.variance << std::endl;
			Check( continuationPT.mean>0.0 &&
				std::fabs(continuationPT.mean-continuationExpected)<=
					6.0*std::sqrt(
						continuationPT.variance/
						static_cast<Scalar>(continuationSamples)) &&
				std::fabs(continuationShader.mean-continuationExpected)<=
					6.0*std::sqrt(
						continuationShader.variance/
						static_cast<Scalar>(continuationSamples)),
				"continuation-visible mixture matches the analytic scalar-density target" );
			Check( continuationCombinedSE>0.0 &&
				std::fabs(continuationPT.mean-continuationShader.mean)<=
					6.0*continuationCombinedSE,
				"both PT entries agree on continuation throughput from the mixed distance density" );
		}

		Fixture balanceFixture;
		Check( balanceFixture.Initialize(
				"immersed_balance_density",1.0,1.0,true,0.1,1e-12),
			"balance-density fixture creates one deterministic point pivot" );
		IRayCaster* pointPivotCaster = nullptr;
		ContinuationDistanceMixtureFireMedium* balanceProbe = nullptr;
		if( balanceFixture.job ) {
			IShader* balanceShader =
				balanceFixture.job->GetShaders()->GetItem("global");
			const IMedium* balanceFire =
				balanceFixture.job->GetScene()->GetGlobalMedium();
			if( balanceFire ) balanceFire->addref();
			IsotropicPhaseFunction* balanceVacuumPhase =
				new IsotropicPhaseFunction();
			UnsupportedHomogeneousSmoke* balanceVacuum =
				new UnsupportedHomogeneousSmoke(
					*balanceVacuumPhase,0.0,0.0);
			balanceFixture.job->GetScene()->SetGlobalMedium(balanceVacuum);
			Check( balanceShader && RISE_API_CreateRayCaster(
					&pointPivotCaster,false,8,*balanceShader,true) &&
				pointPivotCaster,
				"balance-density caster prepares with the point pivot but no thermal pivot" );
			if( pointPivotCaster ) {
				pointPivotCaster->AttachScene(
					balanceFixture.job->GetScene());
			}
			balanceFixture.job->GetScene()->SetGlobalMedium(balanceFire);
			safe_release(balanceVacuum);
			safe_release(balanceVacuumPhase);
			safe_release(balanceFire);
			balanceProbe =
				new ContinuationDistanceMixtureFireMedium();
			balanceFixture.job->GetScene()->SetGlobalMedium(balanceProbe);
		}

		const LightSampler* pointPivotLights =
			pointPivotCaster ? pointPivotCaster->GetLightSampler() : nullptr;
		Check( pointPivotLights &&
			pointPivotLights->GetPositionalLightCount()==1 &&
			pointPivotLights->GetVolumeEmissionMediumCount()==0 &&
			pointPivotLights->GetEquiangularPivotEntryCount()==1,
			"balance-density route has exactly one fixed point-pivot label" );

		struct TechniqueMoments
		{
			long double sum;
			long double sumSquares;
			TechniqueMoments() : sum(0.0), sumSquares(0.0) {}
		};
		struct TechniquePair
		{
			TechniqueMoments deltaTracking;
			TechniqueMoments equiangular;
			bool classified;
			TechniquePair() : classified(true) {}
		};
		const unsigned int balanceSamples = 320000;
		auto recordTechniqueValue = [](
			TechniquePair& result, const Scalar value,
			const unsigned int calls ) {
			if( value<=0.0 ) return;
			TechniqueMoments* technique = nullptr;
			if( calls==1 ) technique = &result.deltaTracking;
			if( calls==0 ) technique = &result.equiangular;
			if( !technique ) {
				result.classified = false;
				return;
			}
			technique->sum += value;
			technique->sumSquares +=
				static_cast<long double>(value)*value;
		};
		auto balanceMomentsPT = [&]( const unsigned int seed ) {
			TechniquePair result;
			RandomNumberGenerator rng(seed);
			RuntimeContext rc(rng,RuntimeContext::PASS_NORMAL,false);
			IndependentSampler sampler(rng);
			for( unsigned int i=0; i<balanceSamples; ++i ) {
				const unsigned int callsBefore =
					balanceProbe->primaryDeltaTrackingCalls;
				const Scalar value = continuationIntegrator->IntegrateRayNM(
					rc,rast,ray,nm,*balanceFixture.job->GetScene(),
					*pointPivotCaster,sampler,environment,nullptr);
				recordTechniqueValue(
					result,value,
					balanceProbe->primaryDeltaTrackingCalls-callsBefore);
			}
			return result;
		};
		auto balanceMomentsShader = [&]( const unsigned int seed ) {
			TechniquePair result;
			RandomNumberGenerator rng(seed);
			RuntimeContext rc(rng,RuntimeContext::PASS_NORMAL,false);
			IRayCaster::RAY_STATE state;
			state.volumeBounces = 63;
			for( unsigned int i=0; i<balanceSamples; ++i ) {
				const unsigned int callsBefore =
					balanceProbe->primaryDeltaTrackingCalls;
				Scalar value = 0.0;
				pointPivotCaster->CastRayNM(
					rc,rast,ray,value,state,nm,nullptr,environment);
				recordTechniqueValue(
					result,value,
					balanceProbe->primaryDeltaTrackingCalls-callsBefore);
			}
			return result;
		};
		auto techniqueMean = [&]( const TechniqueMoments& technique ) {
			return static_cast<Scalar>(
				technique.sum/static_cast<long double>(balanceSamples));
		};
		auto techniqueVariance = [&]( const TechniqueMoments& technique ) {
			const long double count =
				static_cast<long double>(balanceSamples);
			const long double value =
				(technique.sumSquares-
					technique.sum*technique.sum/count)/(count-1.0);
			return static_cast<Scalar>(value>0.0 ? value : 0.0);
		};
		if( pointPivotCaster && balanceProbe && environment ) {
			const Scalar backwardZ = std::sqrt(Scalar(0.99));
			const Scalar environmentNM = environment->GetRadianceNM(
				Ray(receiver,Vector3(0.1,0.0,-backwardZ)),rast,nm);
			const Point3 pointPivot(0.75,0.0,0.5);
			const unsigned int integrationIntervals = 400000;
			const Scalar integrationStep =
				1.0/static_cast<Scalar>(integrationIntervals);
			long double expectedDT = 0.0;
			long double expectedEQ = 0.0;
			for( unsigned int i=0; i<integrationIntervals; ++i ) {
				const Scalar t =
					(static_cast<Scalar>(i)+0.5)*integrationStep;
				const Scalar pdfDT =
					ImmersedDistanceMixtureFireMedium::SigmaT()*
					std::exp(
						-ImmersedDistanceMixtureFireMedium::SigmaT()*t);
				const Scalar pdfEQ = EquiangularSampling::Pdf(
					ray,pointPivot,0.0,1.0,true,t);
				const Scalar mixturePdf = 0.5*pdfDT+0.5*pdfEQ;
				const Scalar integrand =
					environmentNM*
					ImmersedDistanceMixtureFireMedium::SigmaS()*
					std::exp(
						-ImmersedDistanceMixtureFireMedium::SigmaT()*
						(t+(t+1.0)/backwardZ));
				expectedDT +=
					0.5*pdfDT*integrand/mixturePdf*integrationStep;
				expectedEQ +=
					0.5*pdfEQ*integrand/mixturePdf*integrationStep;
			}

			const TechniquePair balancePT =
				balanceMomentsPT(0x1aae456u);
			const TechniquePair balanceShader =
				balanceMomentsShader(0x1aae457u);
			const Scalar ptDT = techniqueMean(balancePT.deltaTracking);
			const Scalar ptEQ = techniqueMean(balancePT.equiangular);
			const Scalar shaderDT =
				techniqueMean(balanceShader.deltaTracking);
			const Scalar shaderEQ =
				techniqueMean(balanceShader.equiangular);
			const Scalar expectedDTScalar =
				static_cast<Scalar>(expectedDT);
			const Scalar expectedEQScalar =
				static_cast<Scalar>(expectedEQ);
			auto withinSixTechniqueErrors = [&](
				const Scalar actual, const Scalar expectedValue,
				const TechniqueMoments& technique ) {
				const Scalar standardError = std::sqrt(
					techniqueVariance(technique)/
					static_cast<Scalar>(balanceSamples));
				return standardError>0.0 &&
					std::fabs(actual-expectedValue)<=
						6.0*standardError;
			};
			std::cout << "  immersed balance DT/EQ expected=" <<
				expectedDTScalar << "/" << expectedEQScalar <<
				" PT=" << ptDT << "/" << ptEQ <<
				" shader=" << shaderDT << "/" << shaderEQ << std::endl;
			Check( balancePT.classified && balanceShader.classified,
				"positive continuation samples identify their initial distance technique" );
			Check( withinSixTechniqueErrors(
					ptDT,expectedDTScalar,balancePT.deltaTracking) &&
				withinSixTechniqueErrors(
					ptEQ,expectedEQScalar,balancePT.equiangular) &&
				withinSixTechniqueErrors(
					shaderDT,expectedDTScalar,
					balanceShader.deltaTracking) &&
				withinSixTechniqueErrors(
					shaderEQ,expectedEQScalar,
					balanceShader.equiangular),
				"each distance technique carries its balance-mixture conditional target" );
			Check( std::fabs(expectedDTScalar-expectedEQScalar)>
				12.0*std::fmax(
					std::sqrt(
						techniqueVariance(balancePT.deltaTracking)/
						static_cast<Scalar>(balanceSamples)),
					std::sqrt(
						techniqueVariance(balancePT.equiangular)/
						static_cast<Scalar>(balanceSamples))),
				"balance targets distinguish mixture density from selected-component density" );
		}

		safe_release(balanceProbe);
		safe_release(pointPivotCaster);
		safe_release(continuationIntegrator);
		safe_release(environment);
		safe_release(environmentPainter);
		safe_release(continuationProbe);
		safe_release(probe);
		safe_release(neeCaster);
		safe_release(marchOnlyCaster);
	}

	void TestThinEmitterSheetAtGrazingAngles()
	{
		std::cout << "TestThinEmitterSheetAtGrazingAngles" << std::endl;
		const std::filesystem::path scenePath =
			std::filesystem::temp_directory_path() /
			( "rise_thin_grazing_fire_sheet_" +
				std::to_string(static_cast<int>(::getpid())) +
				".RISEscene" );
		{
			std::ofstream output(scenePath);
			output << SurfaceFireReceiverScene();
		}

		IJobPriv* job = nullptr;
		IRayCaster* neeCaster = nullptr;
		IRayCaster* marchOnlyCaster = nullptr;
		IScalarPainter* carbonPainter = nullptr;
		IScalarPainter* temperaturePainter = nullptr;
		IMedium* thinFire = nullptr;
		const Point3 sheetMin(0.4,-2.0,-0.15);
		const Point3 sheetMax(1.4,2.0,-0.13);
		const Scalar carbon =
			5.0*kTargetSigmaSI/HotAbsorptionMass633();
		Check( RISE_CreateJobPriv(&job) && job &&
			job->LoadAsciiSceneViaCst(scenePath.string().c_str()) &&
			RISE_API_CreateUniformScalarPainter(
				&carbonPainter,carbon) && carbonPainter &&
			RISE_API_CreateUniformScalarPainter(
				&temperaturePainter,2600.0) && temperaturePainter &&
			RISE_API_CreateMultichannelHeterogeneousMedium(
				&thinFire,*carbonPainter,*temperaturePainter,
				8,8,2,sheetMin,sheetMax,1.0,
				0.26,1800.0,0.0,0.5,8.7,1.2,0.0,0.6) &&
			thinFire,
			"thin anisotropic fire sheet and grazing receiver fixture build" );
		if( job && thinFire ) {
			job->GetScene()->SetGlobalMedium(thinFire);
			IShader* shader = job->GetShaders()->GetItem("global");
			const IMedium* preparedThinFire =
				job->GetScene()->GetGlobalMedium();
			if( preparedThinFire ) preparedThinFire->addref();
			IsotropicPhaseFunction* vacuumPhase =
				new IsotropicPhaseFunction();
			UnsupportedHomogeneousSmoke* vacuum =
				new UnsupportedHomogeneousSmoke(
					*vacuumPhase,0.0,0.0);
			job->GetScene()->SetGlobalMedium(vacuum);
			Check( shader && RISE_API_CreateRayCaster(
					&marchOnlyCaster,false,8,*shader,true) &&
				marchOnlyCaster,
				"thin-sheet march reference prepares without a thermal CDF" );
			if( marchOnlyCaster ) {
				marchOnlyCaster->AttachScene(job->GetScene());
			}
			job->GetScene()->SetGlobalMedium(preparedThinFire);
			safe_release(vacuum);
			safe_release(vacuumPhase);
			Check( shader && RISE_API_CreateRayCaster(
					&neeCaster,false,8,*shader,true) && neeCaster,
				"thin-sheet NEE route prepares the anisotropic thermal CDF" );
			if( neeCaster ) neeCaster->AttachScene(job->GetScene());
			safe_release(preparedThinFire);
		}

		const LightSampler* lights =
			neeCaster ? neeCaster->GetLightSampler() : nullptr;
		const LightSampler* referenceLights =
			marchOnlyCaster ? marchOnlyCaster->GetLightSampler() : nullptr;
		const Point3 receiver(0,0,-0.1);
		const Point3 sheetCenter(
			0.5*(sheetMin.x+sheetMax.x),
			0.5*(sheetMin.y+sheetMax.y),
			0.5*(sheetMin.z+sheetMax.z));
		Vector3 receiverToSheet =
			Vector3Ops::mkVector3(receiver,sheetCenter);
		Vector3Ops::Normalize(receiverToSheet);
		const Scalar sheetNormalCosine =
			std::fabs(receiverToSheet.z);
		Check( lights && lights->GetVolumeEmissionMediumCount()==1 &&
			lights->GetEquiangularPivotEntryCount()==1 &&
			referenceLights &&
			referenceLights->GetVolumeEmissionMediumCount()==0 &&
			sheetMax.z-sheetMin.z<0.025 &&
			sheetNormalCosine<0.05,
			"receiver views a twenty-millimetre emitter sheet at a grazing angle" );

		if( job && neeCaster && marchOnlyCaster && thinFire ) {
			struct SampleMoments
			{
				Scalar mean;
				Scalar variance;
				unsigned int positive;
			};
			const Scalar nm = 500.0;
			const unsigned int samples = 2000000;
			const RasterizerState rast = {0,0};
			const Ray ray(Point3(0,0,-1),Vector3(0,0,1));
			StabilityConfig config;
			config.maxDiffuseBounce = 1;
			PathTracingIntegrator* integrator =
				new PathTracingIntegrator(
					ManifoldSolverConfig(),config);
			integrator->SetMaxPathDepth(2);

			auto momentsPT = [&]( const IRayCaster& route,
				const unsigned int seed ) {
				RandomNumberGenerator rng(seed);
				RuntimeContext rc(
					rng,RuntimeContext::PASS_NORMAL,false);
				IndependentSampler sampler(rng);
				long double sum = 0.0;
				long double sumSquares = 0.0;
				unsigned int positive = 0;
				for( unsigned int i=0; i<samples; ++i ) {
					const Scalar value = integrator->IntegrateRayNM(
						rc,rast,ray,nm,*job->GetScene(),route,
						sampler,nullptr,nullptr);
					sum += value;
					sumSquares +=
						static_cast<long double>(value)*value;
					if( value>0.0 ) ++positive;
				}
				const long double count =
					static_cast<long double>(samples);
				const long double mean = sum/count;
				const long double variance =
					(sumSquares-sum*sum/count)/(count-1.0);
				return SampleMoments{
					static_cast<Scalar>(mean),
					static_cast<Scalar>(
						variance>0.0 ? variance : 0.0),
					positive
				};
			};
			auto momentsShader = [&]( const IRayCaster& route,
				const unsigned int seed ) {
				RandomNumberGenerator rng(seed);
				RuntimeContext rc(
					rng,RuntimeContext::PASS_NORMAL,false);
				IRayCaster::RAY_STATE state;
				long double sum = 0.0;
				long double sumSquares = 0.0;
				unsigned int positive = 0;
				for( unsigned int i=0; i<samples; ++i ) {
					Scalar value = 0.0;
					route.CastRayNM(
						rc,rast,ray,value,state,nm,nullptr,nullptr);
					sum += value;
					sumSquares +=
						static_cast<long double>(value)*value;
					if( value>0.0 ) ++positive;
				}
				const long double count =
					static_cast<long double>(samples);
				const long double mean = sum/count;
				const long double variance =
					(sumSquares-sum*sum/count)/(count-1.0);
				return SampleMoments{
					static_cast<Scalar>(mean),
					static_cast<Scalar>(
						variance>0.0 ? variance : 0.0),
					positive
				};
			};
			auto withinSixStandardErrors = [&]( const SampleMoments& a,
				const SampleMoments& b ) {
				const Scalar standardError = std::sqrt(
					(a.variance+b.variance)/
					static_cast<Scalar>(samples));
				return standardError>0.0 &&
					std::fabs(a.mean-b.mean)<=
						6.0*standardError;
			};

			const SampleMoments ptOn =
				momentsPT(*neeCaster,0x7a1a510u);
			const SampleMoments ptOff =
				momentsPT(*marchOnlyCaster,0x7a1a511u);
			const SampleMoments shaderOn =
				momentsShader(*neeCaster,0x7a1a512u);
			const SampleMoments shaderOff =
				momentsShader(*marchOnlyCaster,0x7a1a513u);
			std::cout << "  thin grazing PT on/off=" <<
				ptOn.mean << "/" << ptOff.mean <<
				" shader on/off=" << shaderOn.mean << "/" <<
				shaderOff.mean << " positive NEE=" <<
				ptOn.positive << "/" << shaderOn.positive <<
				" march=" << ptOff.positive << "/" <<
				shaderOff.positive <<
				std::endl;
			Check( ptOn.mean>0.0 && ptOff.mean>0.0 &&
				shaderOn.mean>0.0 && shaderOff.mean>0.0 &&
				ptOn.positive>100000 && shaderOn.positive>100000 &&
				ptOff.positive>3000 && shaderOff.positive>3000,
				"ordinary NEE is active and brute-force routes retain nontrivial grazing support" );
			Check( withinSixStandardErrors(ptOn,ptOff) &&
				withinSixStandardErrors(shaderOn,shaderOff),
				"thin grazing fire-sheet NEE and brute-force march agree within MC noise" );
			Check( withinSixStandardErrors(ptOn,shaderOn),
				"both PT entry routes agree on thin-sheet volume NEE" );

			StabilityConfig cappedConfig;
			cappedConfig.maxDiffuseBounce = 0;
			PathTracingIntegrator* cappedIntegrator =
				new PathTracingIntegrator(
					ManifoldSolverConfig(),cappedConfig);
			cappedIntegrator->SetMaxPathDepth(2);
			const unsigned int activationSamples = 4096;
			auto cappedMeanPT = [&]( const IRayCaster& route,
				const unsigned int seed ) {
				RandomNumberGenerator rng(seed);
				RuntimeContext rc(
					rng,RuntimeContext::PASS_NORMAL,false);
				IndependentSampler sampler(rng);
				Scalar sum = 0.0;
				for( unsigned int i=0; i<activationSamples; ++i ) {
					sum += cappedIntegrator->IntegrateRayNM(
						rc,rast,ray,nm,*job->GetScene(),route,
						sampler,nullptr,nullptr);
				}
				return sum/static_cast<Scalar>(activationSamples);
			};
			auto cappedMeanShader = [&]( const IRayCaster& route,
				const unsigned int seed ) {
				RandomNumberGenerator rng(seed);
				RuntimeContext rc(
					rng,RuntimeContext::PASS_NORMAL,false);
				IRayCaster::RAY_STATE state;
				Scalar sum = 0.0;
				for( unsigned int i=0; i<activationSamples; ++i ) {
					Scalar value = 0.0;
					route.CastRayNM(
						rc,rast,ray,value,state,nm,nullptr,nullptr);
					sum += value;
				}
				return sum/static_cast<Scalar>(activationSamples);
			};
			const Scalar cappedPTOn =
				cappedMeanPT(*neeCaster,0x7a1a514u);
			const Scalar cappedPTOff =
				cappedMeanPT(*marchOnlyCaster,0x7a1a515u);
			PathTracingShaderOp* cappedShaderOp =
				new PathTracingShaderOp(
					ManifoldSolverConfig(),cappedConfig);
			cappedShaderOp->SetMaxPathDepth(2);
			std::vector<IShaderOp*> cappedShaderOps;
			cappedShaderOps.push_back(cappedShaderOp);
			StandardShader* cappedShader =
				new StandardShader(cappedShaderOps);
			IRayCaster* cappedShaderNEECaster = nullptr;
			IRayCaster* cappedShaderMarchCaster = nullptr;
			const IMedium* cappedPreparedFire =
				job->GetScene()->GetGlobalMedium();
			if( cappedPreparedFire ) cappedPreparedFire->addref();
			IsotropicPhaseFunction* cappedVacuumPhase =
				new IsotropicPhaseFunction();
			UnsupportedHomogeneousSmoke* cappedVacuum =
				new UnsupportedHomogeneousSmoke(
					*cappedVacuumPhase,0.0,0.0);
			job->GetScene()->SetGlobalMedium(cappedVacuum);
			Check( RISE_API_CreateRayCaster(
					&cappedShaderMarchCaster,false,8,
					*cappedShader,true) &&
				cappedShaderMarchCaster,
				"capped thin-sheet shader march reference initializes" );
			if( cappedShaderMarchCaster ) {
				cappedShaderMarchCaster->AttachScene(
					job->GetScene());
			}
			job->GetScene()->SetGlobalMedium(cappedPreparedFire);
			safe_release(cappedVacuum);
			safe_release(cappedVacuumPhase);
			Check( RISE_API_CreateRayCaster(
					&cappedShaderNEECaster,false,8,
					*cappedShader,true) &&
				cappedShaderNEECaster,
				"capped thin-sheet shader NEE route initializes" );
			if( cappedShaderNEECaster ) {
				cappedShaderNEECaster->AttachScene(
					job->GetScene());
			}
			safe_release(cappedPreparedFire);
			const Scalar cappedShaderOn =
				cappedShaderNEECaster ?
					cappedMeanShader(
						*cappedShaderNEECaster,0x7a1a516u) : 0.0;
			const Scalar cappedShaderOff =
				cappedShaderMarchCaster ?
					cappedMeanShader(
						*cappedShaderMarchCaster,0x7a1a517u) : 1.0;
			std::cout << "  thin capped PT on/off=" <<
				cappedPTOn << "/" << cappedPTOff <<
				" shader on/off=" << cappedShaderOn << "/" <<
				cappedShaderOff << std::endl;
			Check( cappedPTOn>0.0 && cappedShaderOn>0.0 &&
				cappedPTOff==0.0 && cappedShaderOff==0.0,
				"thin-sheet endpoint remains active when the diffuse march family is capped" );
			safe_release(cappedShaderNEECaster);
			safe_release(cappedShaderMarchCaster);
			safe_release(cappedShader);
			safe_release(cappedShaderOp);
			safe_release(cappedIntegrator);
			safe_release(integrator);
		}

		safe_release(neeCaster);
		safe_release(marchOnlyCaster);
		safe_release(thinFire);
		safe_release(temperaturePainter);
		safe_release(carbonPainter);
		safe_release(job);
		std::filesystem::remove(scenePath);
	}

	void TestCameraPrimaryDirectViewKeepsWeightOne()
	{
		std::cout << "TestCameraPrimaryDirectViewKeepsWeightOne" << std::endl;
		Fixture fixture;
		Check( fixture.Initialize("camera_primary_weight_one",1.0,1.0),
			"camera-primary direct-fire fixture initializes" );
		if( !fixture.job || !fixture.caster || !fixture.integrator ) return;

		IRayCaster* marchOnlyCaster = nullptr;
		const IMedium* fire = fixture.job->GetScene()->GetGlobalMedium();
		if( fire ) fire->addref();
		IsotropicPhaseFunction* vacuumPhase =
			new IsotropicPhaseFunction();
		UnsupportedHomogeneousSmoke* vacuum =
			new UnsupportedHomogeneousSmoke(*vacuumPhase,0.0,0.0);
		fixture.job->GetScene()->SetGlobalMedium(vacuum);
		IShader* shader = fixture.job->GetShaders()->GetItem("global");
		Check( shader && RISE_API_CreateRayCaster(
				&marchOnlyCaster,false,0,*shader,true) &&
			marchOnlyCaster,
			"camera-primary march reference prepares without a thermal CDF" );
		if( marchOnlyCaster ) {
			marchOnlyCaster->AttachScene(fixture.job->GetScene());
		}
		fixture.job->GetScene()->SetGlobalMedium(fire);
		safe_release(vacuum);
		safe_release(vacuumPhase);
		safe_release(fire);

		const LightSampler* neeLights =
			fixture.caster->GetLightSampler();
		const LightSampler* marchLights =
			marchOnlyCaster ? marchOnlyCaster->GetLightSampler() : nullptr;
		const IMedium* activeFire =
			fixture.job->GetScene()->GetGlobalMedium();
		Check( neeLights && activeFire &&
			neeLights->GetVolumeEmissionMediumCount()==1 &&
			neeLights->VolumeEmissionPdf(
				*activeFire,Point3(0,0,0.5))>0.0 &&
			marchLights &&
			marchLights->GetVolumeEmissionMediumCount()==0,
			"camera-primary comparison has one live endpoint density and a pure-march control" );

		if( marchOnlyCaster ) {
			const unsigned int samples = 80000;
			const Scalar nm = 500.0;
			const RasterizerState rast = {0,0};
			const Ray ray(Point3(0,0,0),Vector3(0,0,1));
			auto meanPT = [&]( const IRayCaster& route,
				const unsigned int seed ) {
				RandomNumberGenerator rng(seed);
				RuntimeContext rc(
					rng,RuntimeContext::PASS_NORMAL,false);
				IndependentSampler sampler(rng);
				Scalar sum = 0.0;
				for( unsigned int i=0; i<samples; ++i ) {
					sum += fixture.integrator->IntegrateRayNM(
						rc,rast,ray,nm,*fixture.job->GetScene(),
						route,sampler,nullptr,nullptr);
				}
				return sum/static_cast<Scalar>(samples);
			};
			auto meanShader = [&]( const IRayCaster& route,
				const unsigned int seed ) {
				RandomNumberGenerator rng(seed);
				RuntimeContext rc(
					rng,RuntimeContext::PASS_NORMAL,false);
				IRayCaster::RAY_STATE state;
				state.volumeBounces = 64;
				Scalar sum = 0.0;
				for( unsigned int i=0; i<samples; ++i ) {
					Scalar value = 0.0;
					route.CastRayNM(
						rc,rast,ray,value,state,nm,nullptr,nullptr);
					sum += value;
				}
				return sum/static_cast<Scalar>(samples);
			};

			const Scalar expected = ExpectedSlab(fixture,nm);
			const Scalar ptNEE =
				meanPT(*fixture.caster,0xca0e001u);
			const Scalar ptMarch =
				meanPT(*marchOnlyCaster,0xca0e002u);
			const Scalar shaderNEE =
				meanShader(*fixture.caster,0xca0e003u);
			const Scalar shaderMarch =
				meanShader(*marchOnlyCaster,0xca0e004u);
			std::cout << "  camera-primary expected=" << expected <<
				" PT NEE/march=" << ptNEE << "/" << ptMarch <<
				" shader NEE/march=" << shaderNEE << "/" <<
				shaderMarch << std::endl;
			Check( NearRelative(ptNEE,expected,0.015) &&
				NearRelative(shaderNEE,expected,0.015),
				"camera-primary collision emission stays at weight one with an active endpoint density" );
			Check( NearRelative(ptMarch,expected,0.015) &&
				NearRelative(shaderMarch,expected,0.015),
				"camera-primary pure-march controls match the absolute slab target" );
			Check( NearRelative(ptNEE,ptMarch,0.015) &&
				NearRelative(shaderNEE,shaderMarch,0.015) &&
				NearRelative(ptNEE,shaderNEE,0.015),
				"camera-primary NEE-ready and march-only entry routes agree" );
		}

		safe_release(marchOnlyCaster);
	}

	void TestFlameBehindGlassIsMarchOnly()
	{
		std::cout << "TestFlameBehindGlassIsMarchOnly" << std::endl;
		auto sceneText = []( const bool includeGlass ) {
			const Scalar carbon =
				5.0*kTargetSigmaSI/HotAbsorptionMass633();
			std::ostringstream scene;
			scene << std::setprecision(17) <<
				"RISE ASCII SCENE 7\n"
				"standard_shader\n{\nname global\n"
				"shaderop DefaultPathTracing\n}\n"
				"uniformcolor_painter\n{\nname white\n"
				"color 0.8 0.8 0.8\n}\n"
				"uniformcolor_painter\n{\nname clear_glass\n"
				"color 1 1 1\n}\n"
				"scalar_painter\n{\nname glass_ior\nvalue 1.5\n}\n"
				"lambertian_material\n{\nname receiver\n"
				"reflectance white\n}\n"
				"perfectrefractor_material\n{\nname glass\n"
				"refractance clear_glass\nior glass_ior\n}\n"
				"scalar_painter\n{\nname carbon\nvalue " <<
				carbon << "\n}\n"
				"scalar_painter\n{\nname temperature\nvalue 2600\n}\n"
				"multichannel_heterogeneous_medium\n{\nname fire\n"
				"channel_carbon painter carbon\n"
				"channel_temperature painter temperature\n"
				"bake_resolution 4 4 4\n"
				"bbox_min 1.1 -0.25 -1.4\n"
				"bbox_max 1.5 0.25 -1.1\n"
				"soot_em 0.26\nsoot_density 1800\n"
				"soot_albedo_hot 0\nsoot_g_hot 0.5\n"
				"smoke_km_carbon 8.7\nsmoke_n_carbon 1.2\n"
				"smoke_albedo_carbon 0\nsmoke_g_carbon 0.6\n}\n"
				"global_medium\n{\nmedium fire\n}\n";
			if( includeGlass ) {
				scene <<
					"sphere_geometry\n{\nname glass_geometry\n"
					"radius 0.42\n}\n"
					"standard_object\n{\nname glass_barrier\n"
					"geometry glass_geometry\nmaterial glass\n"
					"position 0.65 0.12 -0.65\n"
					"casts_shadows FALSE\n}\n";
			}
			scene <<
				"box_geometry\n{\nname receiver_geometry\n"
				"width 8\nheight 8\ndepth 0.2\n}\n"
				"standard_object\n{\nname receiver_wall\n"
				"geometry receiver_geometry\nmaterial receiver\n"
				"position 0 0 0\n}\n";
			return scene.str();
		};

		const std::filesystem::path glassPath =
			std::filesystem::temp_directory_path() /
			( "rise_fire_behind_glass_" +
				std::to_string(static_cast<int>(::getpid())) +
				".RISEscene" );
		const std::filesystem::path clearPath =
			std::filesystem::temp_directory_path() /
			( "rise_fire_without_glass_" +
				std::to_string(static_cast<int>(::getpid())) +
				".RISEscene" );
		{
			std::ofstream output(glassPath);
			output << sceneText(true);
		}
		{
			std::ofstream output(clearPath);
			output << sceneText(false);
		}

		IJobPriv* glassJob = nullptr;
		IJobPriv* clearJob = nullptr;
		IRayCaster* glassNEECaster = nullptr;
		IRayCaster* glassMarchCaster = nullptr;
		IRayCaster* clearNEECaster = nullptr;
		Check( RISE_CreateJobPriv(&glassJob) && glassJob &&
			glassJob->LoadAsciiSceneViaCst(
				glassPath.string().c_str()) &&
			RISE_CreateJobPriv(&clearJob) && clearJob &&
			clearJob->LoadAsciiSceneViaCst(
				clearPath.string().c_str()),
			"glass-blocked and unobstructed fire fixtures load" );
		if( glassJob ) {
			IShader* shader =
				glassJob->GetShaders()->GetItem("global");
			Check( shader && RISE_API_CreateRayCaster(
					&glassNEECaster,false,8,*shader,true) &&
				glassNEECaster,
				"glass fire NEE route prepares" );
			if( glassNEECaster ) {
				glassNEECaster->AttachScene(glassJob->GetScene());
			}
			const IMedium* preparedFire =
				glassJob->GetScene()->GetGlobalMedium();
			if( preparedFire ) preparedFire->addref();
			IsotropicPhaseFunction* vacuumPhase =
				new IsotropicPhaseFunction();
			UnsupportedHomogeneousSmoke* vacuum =
				new UnsupportedHomogeneousSmoke(
					*vacuumPhase,0.0,0.0);
			glassJob->GetScene()->SetGlobalMedium(vacuum);
			Check( shader && RISE_API_CreateRayCaster(
					&glassMarchCaster,false,8,*shader,true) &&
				glassMarchCaster,
				"glass fire march reference prepares without a thermal CDF" );
			if( glassMarchCaster ) {
				glassMarchCaster->AttachScene(glassJob->GetScene());
			}
			glassJob->GetScene()->SetGlobalMedium(preparedFire);
			safe_release(vacuum);
			safe_release(vacuumPhase);
			safe_release(preparedFire);
		}
		if( clearJob ) {
			IShader* shader =
				clearJob->GetShaders()->GetItem("global");
			Check( shader && RISE_API_CreateRayCaster(
					&clearNEECaster,false,8,*shader,true) &&
				clearNEECaster,
				"unobstructed fire NEE route prepares" );
			if( clearNEECaster ) {
				clearNEECaster->AttachScene(clearJob->GetScene());
			}
		}

		const LightSampler* glassLights = glassNEECaster ?
			glassNEECaster->GetLightSampler() : nullptr;
		const LightSampler* marchLights = glassMarchCaster ?
			glassMarchCaster->GetLightSampler() : nullptr;
		const IObject* glassObject = glassJob ?
			glassJob->GetScene()->GetObjects()->GetItem(
				"glass_barrier") : nullptr;
		const IMaterial* glassMaterial =
			glassObject ? glassObject->GetMaterial() : nullptr;
		const Point3 receiver(0,0,-0.1);
		const Point3 fireCenter(1.3,0,-1.25);
		const Point3 glassCenter(0.65,0.12,-0.65);
		Vector3 centerDirection =
			Vector3Ops::mkVector3(fireCenter,receiver);
		const Scalar centerDistance =
			Vector3Ops::Magnitude(centerDirection);
		centerDirection = Vector3Ops::Normalize(centerDirection);
		const Scalar along = Vector3Ops::Dot(
			Vector3Ops::mkVector3(glassCenter,receiver),
			centerDirection);
		const Point3 closest =
			Point3Ops::mkPoint3(
				receiver,centerDirection*along);
		const Scalar glassOffset = Vector3Ops::Magnitude(
			Vector3Ops::mkVector3(closest,glassCenter));
		const bool exactGlass = glassMaterial &&
			typeid(*glassMaterial)==typeid(PerfectRefractorMaterial);
		const bool fixtureAuthentic = glassLights &&
			glassLights->GetVolumeEmissionMediumCount()==1 &&
			marchLights &&
			marchLights->GetVolumeEmissionMediumCount()==0 &&
			exactGlass &&
			along>0.0 && along<centerDistance &&
			glassOffset>0.05 && glassOffset<0.42;
		if( !fixtureAuthentic ) {
			std::cout << "  glass fixture counts=" <<
				(glassLights ?
					glassLights->GetVolumeEmissionMediumCount() : 999) <<
				"/" << (marchLights ?
					marchLights->GetVolumeEmissionMediumCount() : 999) <<
				" exact=" << exactGlass <<
				" along/distance=" << along << "/" <<
				centerDistance << " offset=" << glassOffset <<
				std::endl;
		}
		Check( fixtureAuthentic,
			"the live fire endpoint lies behind an exact off-axis direction-bending glass barrier" );

		if( glassJob && clearJob && glassNEECaster &&
			glassMarchCaster && clearNEECaster ) {
			struct SampleMoments
			{
				Scalar mean;
				Scalar variance;
				unsigned int positive;
			};
			const unsigned int samples = 2000000;
			const Scalar nm = 500.0;
			const RasterizerState rast = {0,0};
			const Ray ray(Point3(0,0,-1),Vector3(0,0,1));
			StabilityConfig config;
			config.maxDiffuseBounce = 1;
			PathTracingIntegrator* integrator =
				new PathTracingIntegrator(
					ManifoldSolverConfig(),config);
			integrator->SetMaxPathDepth(6);
			auto momentsPT = [&]( const IRayCaster& route,
				const unsigned int seed ) {
				RandomNumberGenerator rng(seed);
				RuntimeContext rc(
					rng,RuntimeContext::PASS_NORMAL,false);
				IndependentSampler sampler(rng);
				long double sum = 0.0;
				long double sumSquares = 0.0;
				unsigned int positive = 0;
				for( unsigned int i=0; i<samples; ++i ) {
					const Scalar value = integrator->IntegrateRayNM(
						rc,rast,ray,nm,*glassJob->GetScene(),
						route,sampler,nullptr,nullptr);
					sum += value;
					sumSquares +=
						static_cast<long double>(value)*value;
					if( value>0.0 ) ++positive;
				}
				const long double count =
					static_cast<long double>(samples);
				const long double mean = sum/count;
				const long double variance =
					(sumSquares-sum*sum/count)/(count-1.0);
				return SampleMoments{
					static_cast<Scalar>(mean),
					static_cast<Scalar>(
						variance>0.0 ? variance : 0.0),
					positive
				};
			};
			auto momentsShader = [&]( const IRayCaster& route,
				const unsigned int seed ) {
				RandomNumberGenerator rng(seed);
				RuntimeContext rc(
					rng,RuntimeContext::PASS_NORMAL,false);
				IRayCaster::RAY_STATE state;
				long double sum = 0.0;
				long double sumSquares = 0.0;
				unsigned int positive = 0;
				for( unsigned int i=0; i<samples; ++i ) {
					Scalar value = 0.0;
					route.CastRayNM(
						rc,rast,ray,value,state,nm,nullptr,nullptr);
					sum += value;
					sumSquares +=
						static_cast<long double>(value)*value;
					if( value>0.0 ) ++positive;
				}
				const long double count =
					static_cast<long double>(samples);
				const long double mean = sum/count;
				const long double variance =
					(sumSquares-sum*sum/count)/(count-1.0);
				return SampleMoments{
					static_cast<Scalar>(mean),
					static_cast<Scalar>(
						variance>0.0 ? variance : 0.0),
					positive
				};
			};
			auto withinSixStandardErrors = [&]( const SampleMoments& a,
				const SampleMoments& b ) {
				const Scalar standardError = std::sqrt(
					(a.variance+b.variance)/
					static_cast<Scalar>(samples));
				return standardError>0.0 &&
					std::fabs(a.mean-b.mean)<=
						6.0*standardError;
			};

			const SampleMoments ptOn =
				momentsPT(*glassNEECaster,0x61a5501u);
			const SampleMoments ptOff =
				momentsPT(*glassMarchCaster,0x61a5502u);
			const SampleMoments shaderOn =
				momentsShader(*glassNEECaster,0x61a5503u);
			const SampleMoments shaderOff =
				momentsShader(*glassMarchCaster,0x61a5504u);
			std::cout << "  glass march-only PT on/off=" <<
				ptOn.mean << "/" << ptOff.mean <<
				" shader on/off=" << shaderOn.mean << "/" <<
				shaderOff.mean << " positives=" <<
				ptOn.positive << "/" << ptOff.positive << " " <<
				shaderOn.positive << "/" << shaderOff.positive <<
				std::endl;
			Check( ptOn.positive>1000 && ptOff.positive>1000 &&
				shaderOn.positive>1000 && shaderOff.positive>1000,
				"refracted march paths reach the fire with nontrivial support in both entry routes" );
			Check( withinSixStandardErrors(ptOn,ptOff) &&
				withinSixStandardErrors(shaderOn,shaderOff),
				"flame behind glass is march-only and NEE-on/off agree within MC noise" );
			Check( withinSixStandardErrors(ptOn,shaderOn),
				"both PT entry routes agree on refracted march-only fire" );

			StabilityConfig cappedConfig;
			cappedConfig.maxDiffuseBounce = 0;
			PathTracingIntegrator* cappedIntegrator =
				new PathTracingIntegrator(
					ManifoldSolverConfig(),cappedConfig);
			cappedIntegrator->SetMaxPathDepth(6);
			const unsigned int activationSamples = 4096;
			auto cappedMeanPT = [&]( IJobPriv& job,
				const IRayCaster& route, const unsigned int seed ) {
				RandomNumberGenerator rng(seed);
				RuntimeContext rc(
					rng,RuntimeContext::PASS_NORMAL,false);
				IndependentSampler sampler(rng);
				Scalar sum = 0.0;
				for( unsigned int i=0;
					i<activationSamples; ++i ) {
					sum += cappedIntegrator->IntegrateRayNM(
						rc,rast,ray,nm,*job.GetScene(),
						route,sampler,nullptr,nullptr);
				}
				return sum/
					static_cast<Scalar>(activationSamples);
			};
			const Scalar cappedGlassPT = cappedMeanPT(
				*glassJob,*glassNEECaster,0x61a5505u);
			const Scalar cappedClearPT = cappedMeanPT(
				*clearJob,*clearNEECaster,0x61a5506u);

			PathTracingShaderOp* cappedShaderOp =
				new PathTracingShaderOp(
					ManifoldSolverConfig(),cappedConfig);
			cappedShaderOp->SetMaxPathDepth(6);
			std::vector<IShaderOp*> cappedOps;
			cappedOps.push_back(cappedShaderOp);
			StandardShader* cappedShader =
				new StandardShader(cappedOps);
			IRayCaster* cappedGlassCaster = nullptr;
			IRayCaster* cappedClearCaster = nullptr;
			Check( RISE_API_CreateRayCaster(
					&cappedGlassCaster,false,8,
					*cappedShader,true) &&
				cappedGlassCaster &&
				RISE_API_CreateRayCaster(
					&cappedClearCaster,false,8,
					*cappedShader,true) &&
				cappedClearCaster,
				"capped glass and clear shader routes initialize" );
			if( cappedGlassCaster ) {
				cappedGlassCaster->AttachScene(
					glassJob->GetScene());
			}
			if( cappedClearCaster ) {
				cappedClearCaster->AttachScene(
					clearJob->GetScene());
			}
			auto cappedMeanShader = [&]( const IRayCaster& route,
				const unsigned int seed ) {
				RandomNumberGenerator rng(seed);
				RuntimeContext rc(
					rng,RuntimeContext::PASS_NORMAL,false);
				IRayCaster::RAY_STATE state;
				Scalar sum = 0.0;
				for( unsigned int i=0;
					i<activationSamples; ++i ) {
					Scalar value = 0.0;
					route.CastRayNM(
						rc,rast,ray,value,state,nm,nullptr,nullptr);
					sum += value;
				}
				return sum/
					static_cast<Scalar>(activationSamples);
			};
			const Scalar cappedGlassShader = cappedGlassCaster ?
				cappedMeanShader(
					*cappedGlassCaster,0x61a5507u) : 1.0;
			const Scalar cappedClearShader = cappedClearCaster ?
				cappedMeanShader(
					*cappedClearCaster,0x61a5508u) : 0.0;
			std::cout << "  capped glass/clear PT=" <<
				cappedGlassPT << "/" << cappedClearPT <<
				" shader=" << cappedGlassShader << "/" <<
				cappedClearShader << std::endl;
			Check( cappedGlassPT==0.0 &&
				cappedGlassShader==0.0 &&
				cappedClearPT>0.0 && cappedClearShader>0.0,
				"the endpoint estimator is live at the receiver but exact glass blocks it in both routes" );

			safe_release(cappedGlassCaster);
			safe_release(cappedClearCaster);
			safe_release(cappedShader);
			safe_release(cappedShaderOp);
			safe_release(cappedIntegrator);
			safe_release(integrator);
		}

		safe_release(clearNEECaster);
		safe_release(glassMarchCaster);
		safe_release(glassNEECaster);
		safe_release(clearJob);
		safe_release(glassJob);
		std::filesystem::remove(clearPath);
		std::filesystem::remove(glassPath);
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

	void TestOrenNayarSurfaceVolumeNEEEquality()
	{
		std::cout << "TestOrenNayarSurfaceVolumeNEEEquality" << std::endl;
		const std::filesystem::path scenePath =
			std::filesystem::temp_directory_path() /
			( "rise_oren_nayar_fire_receiver_" +
				std::to_string(static_cast<int>(::getpid())) +
				".RISEscene" );
		{
			std::ofstream output(scenePath);
			output << SurfaceFireReceiverScene(
				false,SurfaceReceiverMaterial::OrenNayar);
		}

		IJobPriv* job = nullptr;
		IRayCaster* neeCaster = nullptr;
		IRayCaster* marchOnlyCaster = nullptr;
		Check( RISE_CreateJobPriv(&job) && job &&
			job->LoadAsciiSceneViaCst(scenePath.string().c_str()),
			"Oren-Nayar receiver and off-axis fire fixture loads" );
		if( job ) {
			IShader* shader = job->GetShaders()->GetItem("global");
			const IMedium* fire = job->GetScene()->GetGlobalMedium();
			if( fire ) fire->addref();
			IsotropicPhaseFunction* vacuumPhase =
				new IsotropicPhaseFunction();
			UnsupportedHomogeneousSmoke* vacuum =
				new UnsupportedHomogeneousSmoke(*vacuumPhase,0.0,0.0);
			job->GetScene()->SetGlobalMedium(vacuum);
			Check( shader && RISE_API_CreateRayCaster(
				&marchOnlyCaster,false,20,*shader,true) &&
				marchOnlyCaster,
				"Oren-Nayar brute-force caster initializes without a volume-emission CDF" );
			if( marchOnlyCaster ) {
				marchOnlyCaster->AttachScene(job->GetScene());
			}
			job->GetScene()->SetGlobalMedium(fire);
			safe_release(vacuum);
			safe_release(vacuumPhase);
			Check( shader && RISE_API_CreateRayCaster(
				&neeCaster,false,20,*shader,true) && neeCaster,
				"Oren-Nayar volume-NEE caster initializes with the fire CDF" );
			if( neeCaster ) neeCaster->AttachScene(job->GetScene());
			safe_release(fire);
		}

		if( job && neeCaster && marchOnlyCaster ) {
			const Scalar nm = 500.0;
			const unsigned int samples = 240000;
			const RasterizerState rast = {0,0};
			const Ray ray(Point3(0,0,-1),Vector3(0,0,1));
			StabilityConfig config;
			config.maxDiffuseBounce = 2;
			PathTracingIntegrator* integrator =
				new PathTracingIntegrator(ManifoldSolverConfig(),config);
			integrator->SetMaxPathDepth(2);
			auto meanPT = [&]( const IRayCaster& route,
				const unsigned int seed ) {
				RandomNumberGenerator rng(seed);
				RuntimeContext rc(
					rng,RuntimeContext::PASS_NORMAL,false);
				IndependentSampler sampler(rng);
				Scalar sum = 0.0;
				for( unsigned int i=0; i<samples; ++i ) {
					sum += integrator->IntegrateRayNM(
						rc,rast,ray,nm,*job->GetScene(),route,
						sampler,nullptr,nullptr);
				}
				return sum/static_cast<Scalar>(samples);
			};
			auto meanShaderRoute = [&]( const IRayCaster& route,
				const unsigned int seed ) {
				RandomNumberGenerator rng(seed);
				RuntimeContext rc(
					rng,RuntimeContext::PASS_NORMAL,false);
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

			const Scalar ptOn = meanPT(*neeCaster,0x0a3e0001u);
			const Scalar ptOff = meanPT(*marchOnlyCaster,0x0a3e0002u);
			const Scalar shaderOn =
				meanShaderRoute(*neeCaster,0x0a3e0003u);
			const Scalar shaderOff =
				meanShaderRoute(*marchOnlyCaster,0x0a3e0004u);
			if( !(ptOn>0.0 && ptOff>0.0 && shaderOn>0.0 &&
				shaderOff>0.0 &&
				NearRelative(ptOn,ptOff,0.08) &&
				NearRelative(shaderOn,shaderOff,0.08)) ) {
				std::cout << "  Oren-Nayar PT on/off=" <<
					ptOn << "/" << ptOff << " shader on/off=" <<
					shaderOn << "/" << shaderOff << std::endl;
			}
			Check( ptOn>0.0 && ptOff>0.0 &&
				NearRelative(ptOn,ptOff,0.08),
				"Oren-Nayar surface volume NEE and brute-force march agree through pure PT" );
			Check( shaderOn>0.0 && shaderOff>0.0 &&
				NearRelative(shaderOn,shaderOff,0.08),
				"Oren-Nayar surface volume NEE and brute-force march agree through shader dispatch" );
			Check( NearRelative(ptOn,shaderOn,0.08),
				"Oren-Nayar surface volume NEE agrees across both PT entry routes" );
			safe_release(integrator);
		}

		safe_release(neeCaster);
		safe_release(marchOnlyCaster);
		safe_release(job);
		std::filesystem::remove(scenePath);
	}

	void TestIsotropicPhongSurfaceVolumeNEEEquality()
	{
		std::cout << "TestIsotropicPhongSurfaceVolumeNEEEquality" << std::endl;
		const std::filesystem::path scenePath =
			std::filesystem::temp_directory_path() /
			( "rise_isotropic_phong_fire_receiver_" +
				std::to_string(static_cast<int>(::getpid())) +
				".RISEscene" );
		{
			std::ofstream output(scenePath);
			output << SurfaceFireReceiverScene(
				false,SurfaceReceiverMaterial::IsotropicPhong);
		}

		IJobPriv* job = nullptr;
		IRayCaster* neeCaster = nullptr;
		IRayCaster* marchOnlyCaster = nullptr;
		Check( RISE_CreateJobPriv(&job) && job &&
			job->LoadAsciiSceneViaCst(scenePath.string().c_str()),
			"mixed Isotropic-Phong receiver and off-axis fire fixture loads" );
		if( job ) {
			IShader* shader = job->GetShaders()->GetItem("global");
			const IMedium* fire = job->GetScene()->GetGlobalMedium();
			if( fire ) fire->addref();
			IsotropicPhaseFunction* vacuumPhase =
				new IsotropicPhaseFunction();
			UnsupportedHomogeneousSmoke* vacuum =
				new UnsupportedHomogeneousSmoke(*vacuumPhase,0.0,0.0);
			job->GetScene()->SetGlobalMedium(vacuum);
			Check( shader && RISE_API_CreateRayCaster(
				&marchOnlyCaster,false,20,*shader,true) &&
				marchOnlyCaster,
				"Isotropic-Phong brute-force caster initializes without a volume-emission CDF" );
			if( marchOnlyCaster ) {
				marchOnlyCaster->AttachScene(job->GetScene());
			}
			job->GetScene()->SetGlobalMedium(fire);
			safe_release(vacuum);
			safe_release(vacuumPhase);
			Check( shader && RISE_API_CreateRayCaster(
				&neeCaster,false,20,*shader,true) && neeCaster,
				"Isotropic-Phong volume-NEE caster initializes with the fire CDF" );
			if( neeCaster ) neeCaster->AttachScene(job->GetScene());
			safe_release(fire);
		}

		if( job && neeCaster && marchOnlyCaster ) {
			const Scalar nm = 500.0;
			const unsigned int samples = 320000;
			const RasterizerState rast = {0,0};
			const Ray ray(Point3(0,0,-1),Vector3(0,0,1));
			StabilityConfig config;
			config.maxDiffuseBounce = 2;
			config.maxGlossyBounce = 2;
			PathTracingIntegrator* integrator =
				new PathTracingIntegrator(ManifoldSolverConfig(),config);
			integrator->SetMaxPathDepth(2);
			auto meanPT = [&]( const IRayCaster& route,
				const unsigned int seed ) {
				RandomNumberGenerator rng(seed);
				RuntimeContext rc(
					rng,RuntimeContext::PASS_NORMAL,false);
				IndependentSampler sampler(rng);
				Scalar sum = 0.0;
				for( unsigned int i=0; i<samples; ++i ) {
					sum += integrator->IntegrateRayNM(
						rc,rast,ray,nm,*job->GetScene(),route,
						sampler,nullptr,nullptr);
				}
				return sum/static_cast<Scalar>(samples);
			};
			auto meanShaderRoute = [&]( const IRayCaster& route,
				const unsigned int seed ) {
				RandomNumberGenerator rng(seed);
				RuntimeContext rc(
					rng,RuntimeContext::PASS_NORMAL,false);
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

			const Scalar ptOn = meanPT(*neeCaster,0x15070001u);
			const Scalar ptOff = meanPT(*marchOnlyCaster,0x15070002u);
			const Scalar shaderOn =
				meanShaderRoute(*neeCaster,0x15070003u);
			const Scalar shaderOff =
				meanShaderRoute(*marchOnlyCaster,0x15070004u);
			std::cout << "  Isotropic-Phong PT on/off=" <<
				ptOn << "/" << ptOff << " shader on/off=" <<
				shaderOn << "/" << shaderOff << std::endl;
			Check( ptOn>0.0 && ptOff>0.0 &&
				NearRelative(ptOn,ptOff,0.01),
				"mixed Isotropic-Phong surface volume NEE and brute-force march agree through pure PT" );
			Check( shaderOn>0.0 && shaderOff>0.0 &&
				NearRelative(shaderOn,shaderOff,0.01),
				"mixed Isotropic-Phong surface volume NEE and brute-force march agree through shader dispatch" );
			Check( NearRelative(ptOn,shaderOn,0.01),
				"mixed Isotropic-Phong surface volume NEE agrees across both PT entry routes" );
			safe_release(integrator);
		}

		safe_release(neeCaster);
		safe_release(marchOnlyCaster);
		safe_release(job);
		std::filesystem::remove(scenePath);
	}

	void TestNullBoundaryMixtureSurvivalEquality()
	{
		std::cout << "TestNullBoundaryMixtureSurvivalEquality" << std::endl;
		struct SurvivalStats
		{
			Scalar mean;
			Scalar variance;
			Scalar positiveFraction;
			Scalar positiveMean;
		};
		const unsigned int samples = 160000;
		const Scalar nm = 500.0;
		const RasterizerState rast = {0,0};
		const Ray ray(Point3(0,0,0),Vector3(0,0,1));

		for( unsigned int topology = 0; topology < 2; ++topology ) {
			const unsigned int segmentCount = topology == 0 ? 1 : 3;
			for( unsigned int targetKind = 0; targetKind < 2; ++targetKind ) {
				const bool downstreamEmitter = targetKind != 0;
				const std::filesystem::path pureScenePath =
					std::filesystem::temp_directory_path() /
					( "rise_null_survival_" +
						std::to_string(segmentCount) + "_" +
						std::to_string(targetKind) + "_" +
						"pure_" +
						std::to_string(static_cast<int>(::getpid())) +
						".RISEscene" );
				const std::filesystem::path mixtureScenePath =
					std::filesystem::temp_directory_path() /
					( "rise_null_survival_" +
						std::to_string(segmentCount) + "_" +
						std::to_string(targetKind) + "_" +
						"mixture_" +
						std::to_string(static_cast<int>(::getpid())) +
						".RISEscene" );
				{
					std::ofstream output(pureScenePath);
					output << NullBoundarySurvivalScene(
						segmentCount,downstreamEmitter,false);
				}
				{
					std::ofstream output(mixtureScenePath);
					output << NullBoundarySurvivalScene(
						segmentCount,downstreamEmitter,true);
				}

				IJobPriv* pureJob = nullptr;
				IJobPriv* mixtureJob = nullptr;
				IRayCaster* pureDTCaster = nullptr;
				IRayCaster* mixtureCaster = nullptr;
				Check( RISE_CreateJobPriv(&pureJob) && pureJob &&
					pureJob->LoadAsciiSceneViaCst(
						pureScenePath.string().c_str()) &&
					RISE_CreateJobPriv(&mixtureJob) && mixtureJob &&
					mixtureJob->LoadAsciiSceneViaCst(
						mixtureScenePath.string().c_str()),
					"paired null-boundary homogeneous survival fixtures load" );
				if( pureJob ) {
					IShader* shader =
						pureJob->GetShaders()->GetItem("global");
					Check( shader && RISE_API_CreateRayCaster(
						&pureDTCaster,false,12,*shader,true) &&
						pureDTCaster,
						"pure-DT null-boundary caster initializes" );
					if( pureDTCaster ) {
						pureDTCaster->AttachScene(pureJob->GetScene());
					}
				}
				if( mixtureJob ) {
					IShader* shader =
						mixtureJob->GetShaders()->GetItem("global");
					Check( shader && RISE_API_CreateRayCaster(
						&mixtureCaster,false,12,*shader,true) &&
						mixtureCaster,
						"50/50 null-boundary caster initializes" );
					if( mixtureCaster ) {
						mixtureCaster->AttachScene(
							mixtureJob->GetScene());
					}
				}

				IPainter* environmentPainter = nullptr;
				IRadianceMap* environment = nullptr;
				if( !downstreamEmitter ) {
					Check( RISE_API_CreateUniformColorPainter(
							&environmentPainter,RISEPel(1.0,1.0,1.0),
							eSpectrumKind_Unbounded) &&
						environmentPainter &&
						RISE_API_CreateRadianceMap(
							&environment,*environmentPainter,1.0) &&
						environment,
						"null-boundary survival fixture creates a constant spectral background" );
				}

				if( pureJob && mixtureJob && pureDTCaster && mixtureCaster &&
					(downstreamEmitter || environment) ) {
					const RayCaster* pureConcrete =
						dynamic_cast<const RayCaster*>(pureDTCaster);
					const RayCaster* mixtureConcrete =
						dynamic_cast<const RayCaster*>(mixtureCaster);
					const LightSampler* pureLights =
						pureConcrete ? pureConcrete->GetLightSampler() : nullptr;
					const LightSampler* mixtureLights =
						mixtureConcrete ? mixtureConcrete->GetLightSampler() : nullptr;
					Check( pureLights && mixtureLights &&
						pureLights->GetEquiangularPivotEntryCount()==0 &&
						mixtureLights->GetEquiangularPivotEntryCount()>0,
						"paired casters select pure DT versus the 50/50 distance mixture" );

					StabilityConfig config;
					config.maxVolumeBounce = 0;
					PathTracingIntegrator* integrator =
						new PathTracingIntegrator(
							ManifoldSolverConfig(),config);
					integrator->SetMaxPathDepth(4);
					auto measure = [&]( IJobPriv& renderJob,
						const IRayCaster& caster, const bool purePT,
						const unsigned int seed ) {
						RandomNumberGenerator rng(seed);
						RuntimeContext rc(
							rng,RuntimeContext::PASS_NORMAL,false);
						IndependentSampler sampler(rng);
						IRayCaster::RAY_STATE state;
						long double sum = 0.0;
						long double sumSquares = 0.0;
						long double positiveSum = 0.0;
						unsigned int positive = 0;
						for( unsigned int i=0; i<samples; ++i ) {
							Scalar value = 0.0;
							if( purePT ) {
								value = integrator->IntegrateRayNM(
									rc,rast,ray,nm,*renderJob.GetScene(),
									caster,sampler,environment,nullptr);
							} else {
								caster.CastRayNM(
									rc,rast,ray,value,state,nm,nullptr,
									environment);
							}
							sum += value;
							sumSquares +=
								static_cast<long double>(value)*value;
							if( value>0.0 ) {
								positiveSum += value;
								++positive;
							}
						}
						const long double count =
							static_cast<long double>(samples);
						const long double variance =
							(sumSquares-sum*sum/count)/(count-1.0);
						SurvivalStats result;
						result.mean =
							static_cast<Scalar>(sum/count);
						result.variance = static_cast<Scalar>(
							variance>0.0 ? variance : 0.0);
						result.positiveFraction =
							static_cast<Scalar>(positive)/samples;
						result.positiveMean = positive ?
							static_cast<Scalar>(
								positiveSum/
								static_cast<long double>(positive)) : 0.0;
						return result;
					};
					auto checkRoute = [&]( const char* routeName,
						const SurvivalStats& pure,
						const SurvivalStats& mixed ) {
						const Scalar combinedSE = std::sqrt(
							(pure.variance+mixed.variance)/
							static_cast<Scalar>(samples));
						const Scalar survivalScale =
							std::ldexp(Scalar(1.0),
								-static_cast<int>(segmentCount));
						const Scalar expectedMixedFraction =
							pure.positiveFraction*survivalScale;
						const Scalar fractionSE = std::sqrt(
							(mixed.positiveFraction*
								(1.0-mixed.positiveFraction) +
								survivalScale*survivalScale*
								pure.positiveFraction*
								(1.0-pure.positiveFraction))/
							static_cast<Scalar>(samples));
						std::cout << "  null-" << segmentCount << " " <<
							(downstreamEmitter ? "emitter " : "background ") <<
							routeName << " means=" << mixed.mean << "/" <<
							pure.mean << " positive=" <<
							mixed.positiveFraction << "/" <<
							pure.positiveFraction << " conditional=" <<
							mixed.positiveMean << "/" <<
							pure.positiveMean << std::endl;
						Check( pure.mean>0.0 && mixed.mean>0.0 &&
							combinedSE>0.0 &&
							std::fabs(mixed.mean-pure.mean)<=
								6.0*combinedSE,
							"50/50 null-boundary survival matches pure-DT mean" );
						Check( fractionSE>0.0 &&
							std::fabs(
								mixed.positiveFraction-
								expectedMixedFraction)<=6.0*fractionSE,
							"50/50 survivor frequency carries one half per finite null-boundary segment" );
						Check( pure.positiveMean>0.0 &&
							NearRelative(
								mixed.positiveMean,
								std::ldexp(
									pure.positiveMean,
									static_cast<int>(segmentCount)),
								1e-10),
							"surviving path divides deterministic transmittance by every no-event atom" );
					};

					const unsigned int seedBase =
						0x9b000000u+segmentCount*0x1000u+
						targetKind*0x100u;
					const SurvivalStats purePT =
						measure(*pureJob,*pureDTCaster,true,seedBase+1);
					const SurvivalStats mixedPT =
						measure(*mixtureJob,*mixtureCaster,true,seedBase+2);
					const SurvivalStats pureShader =
						measure(*pureJob,*pureDTCaster,false,seedBase+3);
					const SurvivalStats mixedShader =
						measure(*mixtureJob,*mixtureCaster,false,seedBase+4);
					checkRoute("PT",purePT,mixedPT);
					checkRoute("shader",pureShader,mixedShader);
					const Scalar routeSE = std::sqrt(
						(mixedPT.variance+mixedShader.variance)/
						static_cast<Scalar>(samples));
					Check( routeSE>0.0 &&
						std::fabs(mixedPT.mean-mixedShader.mean)<=
							6.0*routeSE,
						"PT and shader entries agree after null-boundary mixture survival" );
					safe_release(integrator);
				}

				safe_release(environment);
				safe_release(environmentPainter);
				safe_release(mixtureCaster);
				safe_release(pureDTCaster);
				safe_release(mixtureJob);
				safe_release(pureJob);
				std::filesystem::remove(mixtureScenePath);
				std::filesystem::remove(pureScenePath);
			}
		}
	}

	void TestHollowCavityFullSphereVolumeNEEEquality()
	{
		std::cout << "TestHollowCavityFullSphereVolumeNEEEquality" << std::endl;
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() /
			( "rise_hollow_cavity_fire_receiver_" +
				std::to_string(static_cast<int>(::getpid())) + ".RISEscene" );
		{
			std::ofstream output(scenePath);
			output << HollowCavityFireReceiverScene();
		}

		IJobPriv* job = nullptr;
		IRayCaster* neeCaster = nullptr;
		IRayCaster* marchOnlyCaster = nullptr;
		Check( RISE_CreateJobPriv(&job) && job &&
			job->LoadAsciiSceneViaCst(scenePath.string().c_str()),
			"hollow-cavity emissive-shell fixture loads" );
		if( job ) {
			IShader* shader = job->GetShaders()->GetItem("global");
			const IMedium* fire = job->GetScene()->GetGlobalMedium();
			if( fire ) fire->addref();
			IsotropicPhaseFunction* vacuumPhase = new IsotropicPhaseFunction();
			UnsupportedHomogeneousSmoke* preparationVacuum =
				new UnsupportedHomogeneousSmoke(*vacuumPhase,0.0,0.0);
			job->GetScene()->SetGlobalMedium(preparationVacuum);
			Check( shader && RISE_API_CreateRayCaster(
				&marchOnlyCaster,false,20,*shader,true) && marchOnlyCaster,
				"hollow-cavity reference caster prepares without a thermal CDF" );
			if( marchOnlyCaster ) marchOnlyCaster->AttachScene(job->GetScene());
			job->GetScene()->SetGlobalMedium(fire);
			safe_release(preparationVacuum);
			safe_release(vacuumPhase);
			Check( shader && RISE_API_CreateRayCaster(
				&neeCaster,false,20,*shader,true) && neeCaster,
				"hollow-cavity volume-NEE caster prepares the shell CDF" );
			if( neeCaster ) neeCaster->AttachScene(job->GetScene());
			safe_release(fire);
		}

		if( job && neeCaster && marchOnlyCaster ) {
			const Scalar nm = 500.0;
			const IObject* cavity = job->GetScene()->GetObjects() ?
				job->GetScene()->GetObjects()->GetItem("cavity") : nullptr;
			const IMedium* cavityVacuum = cavity ? cavity->GetInteriorMedium() : nullptr;
			const IMedium* fire = job->GetScene()->GetGlobalMedium();
			IORStack receiverStack(1.0);
			IORStackSeeding::SeedFromPoint(
				receiverStack,Point3(0,0,-0.5),*job->GetScene());
			const IMedium* receiverMedium = MediumTracking::GetCurrentMedium(
				receiverStack,job->GetScene());
			IORStack shellStack(1.0);
			IORStackSeeding::SeedFromPoint(
				shellStack,Point3(0,0,-1.5),*job->GetScene());
			const IMedium* shellMedium = MediumTracking::GetCurrentMedium(
				shellStack,job->GetScene());
			const MediumCoefficientsNM receiverCoefficients = receiverMedium ?
				receiverMedium->GetCoefficientsNM(Point3(0,0,-0.5),nm) :
				MediumCoefficientsNM();
			const MediumCoefficientsNM shellCoefficients = shellMedium ?
				shellMedium->GetCoefficientsNM(Point3(0,0,-1.5),nm) :
				MediumCoefficientsNM();
			const Scalar shellEmission = shellMedium ?
				shellMedium->GetThermalEmissionNM(Point3(0,0,-1.5),nm) : 0.0;
			Check( cavityVacuum && receiverMedium==cavityVacuum &&
				receiverCoefficients.sigma_t==0.0,
				"receiver resolves to the hollow cavity with sigma_t equal to zero" );
			if( !(fire && shellMedium==fire && shellCoefficients.sigma_t>0.0 &&
				shellEmission>0.0) ) {
				std::cout << "  shell medium=" << shellMedium << " fire=" << fire <<
					" top object=" << shellStack.topObject() << " sigma_t=" <<
					shellCoefficients.sigma_t << " thermal emission=" <<
					shellEmission << std::endl;
			}
			Check( fire && shellMedium==fire && shellCoefficients.sigma_t>0.0 &&
				shellEmission>0.0,
				"the surrounding shell resolves to emissive fire with nonzero extinction" );

			const RayCaster* concrete = dynamic_cast<const RayCaster*>(neeCaster);
			const LightSampler* lights = concrete ? concrete->GetLightSampler() : nullptr;
			const RayCaster* referenceConcrete =
				dynamic_cast<const RayCaster*>(marchOnlyCaster);
			const LightSampler* referenceLights = referenceConcrete ?
				referenceConcrete->GetLightSampler() : nullptr;
			Check( lights && lights->GetVolumeEmissionMediumCount()==1 &&
				lights->GetEquiangularPivotEntryCount()==1,
				"hollow-cavity fixture prepares one shell endpoint and pivot label" );
			Check( referenceLights &&
				referenceLights->GetVolumeEmissionMediumCount()==0 &&
				referenceLights->GetEquiangularPivotEntryCount()==0,
				"hollow-cavity march reference has no thermal endpoint or pivot entries" );
			bool octants[8] = {false,false,false,false,false,false,false,false};
			if( lights ) {
				RandomNumberGenerator pivotRng(0xc4717a1u);
				IndependentSampler pivotSampler(pivotRng);
				for( unsigned int i=0; i<4096; ++i ) {
					VolumeEmissionPivotState pivots;
					if( !lights->SampleVolumeEmissionPivots(pivotSampler,pivots) ||
						pivots.mediumPivots.size()!=1 ) continue;
					const Point3& pivot = pivots.mediumPivots[0];
					const Vector3 offset = Vector3Ops::mkVector3(Point3(0,0,0),pivot);
					if( Vector3Ops::Magnitude(offset) <= 0.75 ) continue;
					const unsigned int octant =
						(pivot.x>=0.0 ? 1u : 0u) |
						(pivot.y>=0.0 ? 2u : 0u) |
						(pivot.z>=0.0 ? 4u : 0u);
					octants[octant] = true;
				}
			}
			bool surroundsReceiver = true;
			for( unsigned int i=0; i<8; ++i ) surroundsReceiver &= octants[i];
			Check( surroundsReceiver,
				"thermal pivot sampling covers every octant around the cavity receiver" );

			StabilityConfig config;
			config.maxDiffuseBounce = 2;
			PathTracingIntegrator* integrator = new PathTracingIntegrator(
				ManifoldSolverConfig(),config);
			integrator->SetMaxPathDepth(2);
			const unsigned int samples = 220000;
			const RasterizerState rast = {0,0};
			const Ray ray(Point3(0,0,-0.5),Vector3(0,0,1));
			struct SampleMoments
			{
				Scalar mean;
				Scalar variance;
			};
			auto momentsPT = [&]( PathTracingIntegrator& testedIntegrator,
				const IRayCaster& route, const unsigned int seed,
				const unsigned int sampleCount ) {
				RandomNumberGenerator rng(seed);
				RuntimeContext rc(rng,RuntimeContext::PASS_NORMAL,false);
				IndependentSampler sampler(rng);
				long double sum = 0.0;
				long double sumSquares = 0.0;
				for( unsigned int i=0; i<sampleCount; ++i ) {
					const Scalar value = testedIntegrator.IntegrateRayNM(
						rc,rast,ray,nm,*job->GetScene(),route,sampler,
						nullptr,nullptr);
					sum += value;
					sumSquares += static_cast<long double>(value)*value;
				}
				const long double count = static_cast<long double>(sampleCount);
				const long double mean = sum/count;
				const long double variance =
					(sumSquares-sum*sum/count)/(count-1.0);
				SampleMoments result;
				result.mean = static_cast<Scalar>(mean);
				result.variance = static_cast<Scalar>(
					variance>0.0 ? variance : 0.0);
				return result;
			};
			auto momentsShaderRoute = [&]( const IRayCaster& route,
				const unsigned int seed, const unsigned int sampleCount ) {
				RandomNumberGenerator rng(seed);
				RuntimeContext rc(rng,RuntimeContext::PASS_NORMAL,false);
				IRayCaster::RAY_STATE state;
				long double sum = 0.0;
				long double sumSquares = 0.0;
				for( unsigned int i=0; i<sampleCount; ++i ) {
					Scalar value = 0.0;
					route.CastRayNM(
						rc,rast,ray,value,state,nm,nullptr,nullptr);
					sum += value;
					sumSquares += static_cast<long double>(value)*value;
				}
				const long double count = static_cast<long double>(sampleCount);
				const long double mean = sum/count;
				const long double variance =
					(sumSquares-sum*sum/count)/(count-1.0);
				SampleMoments result;
				result.mean = static_cast<Scalar>(mean);
				result.variance = static_cast<Scalar>(
					variance>0.0 ? variance : 0.0);
				return result;
			};

			const SampleMoments ptOn = momentsPT(
				*integrator,*neeCaster,0xc4717b1u,samples);
			const SampleMoments ptOff = momentsPT(
				*integrator,*marchOnlyCaster,0xc4717b2u,samples);
			const SampleMoments shaderOn = momentsShaderRoute(
				*neeCaster,0xc4717b3u,samples);
			const SampleMoments shaderOff = momentsShaderRoute(
				*marchOnlyCaster,0xc4717b4u,samples);
			const Scalar varianceRatio = ptOff.variance>0.0 ?
				ptOn.variance/ptOff.variance : -1.0;
			std::cout << "  hollow-cavity PT on/off=" << ptOn.mean << "/" <<
				ptOff.mean << " shader on/off=" << shaderOn.mean << "/" <<
				shaderOff.mean << " variance NEE/march=" << varianceRatio << std::endl;
			auto withinSixStandardErrors = [&]( const SampleMoments& a,
				const SampleMoments& b ) {
				const Scalar combinedStandardError = std::sqrt(
					(a.variance+b.variance)/static_cast<Scalar>(samples));
				return combinedStandardError>0.0 &&
					std::fabs(a.mean-b.mean) <= 6.0*combinedStandardError;
			};
			const bool meansAgree = ptOn.mean>0.0 && ptOff.mean>0.0 &&
				shaderOn.mean>0.0 && shaderOff.mean>0.0 &&
				withinSixStandardErrors(ptOn,ptOff) &&
				withinSixStandardErrors(shaderOn,shaderOff) &&
				withinSixStandardErrors(ptOn,shaderOn);
			if( !meansAgree || !std::isfinite(varianceRatio) ) {
				std::cout << "  hollow cavity PT on/off=" << ptOn.mean << "/" <<
					ptOff.mean << " shader on/off=" << shaderOn.mean << "/" <<
					shaderOff.mean << " variance ratio=" << varianceRatio << std::endl;
			}
			Check( meansAgree,
				"hollow-cavity shell NEE and march agree within MC noise in both PT entries" );
			// The r43 full-sphere standoff deliberately weakens the endpoint
			// proposal: the fixed-seed baseline is 8.68x the conditioned-march
			// variance.  This is a measured regression ceiling, not a claim that
			// NEE must win on every canonical geometry.
			Check( std::isfinite(varianceRatio) && varianceRatio>0.0 &&
				varianceRatio<12.0,
				"hollow-cavity NEE-to-march variance stays within its recorded bound" );

			StabilityConfig cappedConfig;
			cappedConfig.maxDiffuseBounce = 0;
			PathTracingIntegrator* cappedIntegrator = new PathTracingIntegrator(
				ManifoldSolverConfig(),cappedConfig);
			cappedIntegrator->SetMaxPathDepth(2);
			const unsigned int activationSamples = 30000;
			const SampleMoments cappedOn = momentsPT(
				*cappedIntegrator,*neeCaster,0xc4717c1u,activationSamples);
			const SampleMoments cappedOff = momentsPT(
				*cappedIntegrator,*marchOnlyCaster,0xc4717c2u,activationSamples);
			Check( cappedOn.mean>0.0 && std::isfinite(cappedOn.mean) &&
				cappedOff.mean==0.0,
				"capped diffuse receiver proves the shell volume-NEE estimator is active" );

			PathTracingShaderOp* cappedShaderOp = new PathTracingShaderOp(
				ManifoldSolverConfig(),cappedConfig);
			cappedShaderOp->SetMaxPathDepth(2);
			std::vector<IShaderOp*> cappedShaderOps;
			cappedShaderOps.push_back(cappedShaderOp);
			StandardShader* cappedShader = new StandardShader(cappedShaderOps);
			IRayCaster* cappedShaderNEECaster = nullptr;
			IRayCaster* cappedShaderMarchCaster = nullptr;
			const IMedium* fireForCappedShader =
				job->GetScene()->GetGlobalMedium();
			if( fireForCappedShader ) fireForCappedShader->addref();
			IsotropicPhaseFunction* cappedVacuumPhase =
				new IsotropicPhaseFunction();
			UnsupportedHomogeneousSmoke* cappedPreparationVacuum =
				new UnsupportedHomogeneousSmoke(*cappedVacuumPhase,0.0,0.0);
			job->GetScene()->SetGlobalMedium(cappedPreparationVacuum);
			Check( RISE_API_CreateRayCaster(
				&cappedShaderMarchCaster,false,20,*cappedShader,true) &&
				cappedShaderMarchCaster,
				"capped shader-dispatch march reference initializes" );
			if( cappedShaderMarchCaster )
				cappedShaderMarchCaster->AttachScene(job->GetScene());
			job->GetScene()->SetGlobalMedium(fireForCappedShader);
			safe_release(cappedPreparationVacuum);
			safe_release(cappedVacuumPhase);
			Check( RISE_API_CreateRayCaster(
				&cappedShaderNEECaster,false,20,*cappedShader,true) &&
				cappedShaderNEECaster,
				"capped shader-dispatch volume-NEE route initializes" );
			if( cappedShaderNEECaster )
				cappedShaderNEECaster->AttachScene(job->GetScene());
			safe_release(fireForCappedShader);
			if( cappedShaderNEECaster && cappedShaderMarchCaster ) {
				const SampleMoments cappedShaderOn = momentsShaderRoute(
					*cappedShaderNEECaster,0xc4717c3u,activationSamples);
				const SampleMoments cappedShaderOff = momentsShaderRoute(
					*cappedShaderMarchCaster,0xc4717c4u,activationSamples);
				Check( cappedShaderOn.mean>0.0 &&
					std::isfinite(cappedShaderOn.mean) &&
					cappedShaderOff.mean==0.0,
					"capped shader dispatch proves the shell volume-NEE estimator is active" );
			}
			safe_release(cappedShaderNEECaster);
			safe_release(cappedShaderMarchCaster);
			safe_release(cappedShader);
			safe_release(cappedShaderOp);
			safe_release(cappedIntegrator);
			safe_release(integrator);
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
				&caster, false, 2, *shader, true ) && caster,
				"two-event continuation caster initializes" );
			if( caster ) caster->AttachScene( job->GetScene() );
			StabilityConfig stability;
			stability.rrMinDepth = 0;
			stability.rrThreshold = 2.0;
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

			TwoEventFireMedium* chemMedium =
				new TwoEventFireMedium( *phase, true );
			job->GetScene()->SetGlobalMedium( chemMedium );
			caster->AttachScene( job->GetScene() );

			const Ray chemRay(
				Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
			const RayIntersection chemEntry( chemRay, rast );
			const IORStack chemIorStack( 1.0 );
			bool everyTerminalChemPTSample = true;
			for( unsigned int i = 0; i < 64; ++i ) {
				RandomNumberGenerator chemRng( 0xc4e8c017u+i );
				RuntimeContext chemRc(
					chemRng, RuntimeContext::PASS_NORMAL, false );
				IndependentSampler chemSampler( chemRng );
				const Scalar chemPT = integrator->IntegrateFromHitNM(
					chemRc, rast, chemEntry, 430.0,
					*job->GetScene(), *caster, chemSampler, nullptr,
					0, chemIorStack, 0.0, 0.0, true, 1.0,
					IRayCaster::RAY_STATE::eRayView,
					0, 0, 0, 0, 0, 0.0, false, false, nullptr );
				everyTerminalChemPTSample =
					everyTerminalChemPTSample &&
					NearRelative( chemPT, 0.04, 1e-13 );
			}

			RandomNumberGenerator chemCasterRng( 0xc4e8c018u );
			RuntimeContext chemCasterRc(
				chemCasterRng, RuntimeContext::PASS_NORMAL, false );
			IRayCaster::RAY_STATE chemState;
			Scalar chemShader = 0.0;
			caster->CastRayNM(
				chemCasterRc, rast,
				chemRay,
				chemShader, chemState, 430.0, nullptr, nullptr );
			if( !everyTerminalChemPTSample ||
				!NearRelative( chemShader, 0.04, 1e-13 ) ||
				chemMedium->chemSegmentCalls != 130 ) {
				std::cout << "  terminal chem PT-all/shader/calls=" <<
					everyTerminalChemPTSample << "/" << chemShader << "/" <<
					chemMedium->chemSegmentCalls << std::endl;
			}
			Check( everyTerminalChemPTSample &&
				NearRelative( chemShader, 0.04, 1e-13 ) &&
				chemMedium->chemSegmentCalls == 130,
				"path-depth-capped PT skips intermediate roulette and RayCaster scores the chem-only outgoing segment" );
			safe_release( chemMedium );
		}

		safe_release( integrator );
		safe_release( caster );
		safe_release( job );
		safe_release( medium );
		safe_release( phase );
		std::filesystem::remove( scenePath );
	}

	void TestTerminalSurfaceChemSegmentSkipsRoulette()
	{
		std::cout << "TestTerminalSurfaceChemSegmentSkipsRoulette" << std::endl;
		const std::filesystem::path scenePath =
			std::filesystem::temp_directory_path() /
			( "rise_pt_terminal_surface_chem_" +
				std::to_string( static_cast<int>( ::getpid() ) ) +
				".RISEscene" );
		{
			std::ofstream output( scenePath );
			output <<
				"RISE ASCII SCENE 7\n"
				"standard_shader\n{\nname global\n}\n"
				"uniformcolor_painter\n{\nname white\ncolor 0.8 0.8 0.8\n}\n"
				"lambertian_material\n{\nname receiver\nreflectance white\n}\n"
				"box_geometry\n{\nname receiver_geometry\n"
				"width 8\nheight 8\ndepth 0.2\n}\n"
				"standard_object\n{\nname receiver_wall\n"
				"geometry receiver_geometry\nmaterial receiver\n}\n";
		}

		IJobPriv* job = nullptr;
		IRayCaster* caster = nullptr;
		PathTracingIntegrator* integrator = nullptr;
		TerminalChemEmissionMedium* medium =
			new TerminalChemEmissionMedium();
		Check( RISE_CreateJobPriv( &job ) && job &&
			job->LoadAsciiSceneViaCst( scenePath.string().c_str() ),
			"terminal surface chem fixture loads" );
		if( job ) {
			job->GetScene()->SetGlobalMedium( medium );
			IShader* shader = job->GetShaders()->GetItem( "global" );
			Check( shader && RISE_API_CreateRayCaster(
				&caster, false, 4, *shader, true ) && caster,
				"terminal surface chem caster initializes" );
			if( caster ) caster->AttachScene( job->GetScene() );
			StabilityConfig stability;
			stability.rrMinDepth = 0;
			stability.rrThreshold = 2.0;
			integrator = new PathTracingIntegrator(
				ManifoldSolverConfig(), stability );
			integrator->SetMaxPathDepth( 1 );
		}

		if( job && caster && integrator ) {
			const RayCaster* concrete =
				dynamic_cast<const RayCaster*>( caster );
			const LightSampler* lights =
				concrete ? concrete->GetLightSampler() : nullptr;
			Check( lights && lights->SceneHasFireMedia() &&
				lights->GetVolumeEmissionMediumCount() == 0,
				"terminal surface fixture is chem-only with no thermal NEE competitor" );

			const RasterizerState rast = { 0, 0 };
			const Ray ray( Point3( 0, 0, -1 ), Vector3( 0, 0, 1 ) );
			const IPainter* white =
				job->GetPainters()->GetItem( "white" );
			const RayIntersectionGeometric referenceIntersection( ray, rast );
			const Scalar expected = white ?
				0.25*white->GetColorNM( referenceIntersection, 430.0 ) : 0.0;
			bool everySample = true;
			Scalar firstValue = 0.0;
			for( unsigned int i = 0; i < 256; ++i ) {
				RandomNumberGenerator rng( 0x57face00u+i );
				RuntimeContext rc(
					rng, RuntimeContext::PASS_NORMAL, false );
				IndependentSampler sampler( rng );
				const Scalar value = integrator->IntegrateRayNM(
					rc, rast, ray, 430.0, *job->GetScene(), *caster,
					sampler, nullptr, nullptr );
				if( i == 0 ) firstValue = value;
				everySample = everySample &&
					NearRelative( value, expected, 1e-12 );
			}
			if( !everySample ) {
				std::cout << "  terminal surface expected/first=" <<
					expected << "/" << firstValue << std::endl;
			}
			Check( expected > 0.0 && everySample,
				"terminal Lambertian A_march segment collects chem without a roulette atom when thermal NEE is absent" );
		}

		safe_release( integrator );
		safe_release( caster );
		safe_release( job );
		safe_release( medium );
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

	void TestZeroSootChemOnlyLineEstimator()
	{
		std::cout << "TestZeroSootChemOnlyLineEstimator" << std::endl;
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() /
			( "rise_zero_soot_synthetic_chem_" +
				std::to_string(static_cast<int>(::getpid())) + ".RISEscene" );
		{
			std::ofstream output(scenePath);
			output <<
				"RISE ASCII SCENE 7\n"
				"standard_shader\n{\nname global\n}\n"
				"null_boundary_material\n{\nname source_seam\n}\n"
				"sphere_geometry\n{\nname seam_geometry\nradius 0.4\n}\n"
				"standard_object\n{\nname seam\ngeometry seam_geometry\n"
				"material source_seam\nposition 0 0 0.5\n}\n";
		}

		IJobPriv* job = nullptr;
		IRayCaster* caster = nullptr;
		IRayCaster* rrCaster = nullptr;
		PathTracingIntegrator* integrator = nullptr;
		SyntheticChemEmissionMedium* medium = nullptr;
		Check( RISE_CreateJobPriv(&job) && job &&
			job->LoadAsciiSceneViaCst(scenePath.string().c_str()),
			"zero-soot synthetic-chem fixture initializes" );
		if( job ) {
			medium = new SyntheticChemEmissionMedium();
			IObjectPriv* enclosure =
				job->GetScene()->GetObjects()->GetItem("seam");
			Check( enclosure && enclosure->AssignInteriorMedium(*medium),
				"zero-soot chem source is object-bound behind a null boundary" );
			IShader* shader = job->GetShaders()->GetItem("global");
			Check( shader && RISE_API_CreateRayCaster(
				&caster,false,0,*shader,true) && caster,
				"zero-soot synthetic-chem caster initializes" );
			if( caster ) caster->AttachScene(job->GetScene());
			Check( shader && RISE_API_CreateRayCaster(
				&rrCaster,false,4,*shader,true) && rrCaster,
				"zero-soot roulette-branch caster initializes" );
			if( rrCaster ) rrCaster->AttachScene(job->GetScene());
			StabilityConfig stability;
			stability.maxVolumeBounce = 0;
			integrator = new PathTracingIntegrator(
				ManifoldSolverConfig(),stability);
		}

		if( job && caster && rrCaster && integrator && medium ) {
			const RayCaster* concrete = dynamic_cast<const RayCaster*>(caster);
			const LightSampler* lights =
				concrete ? concrete->GetLightSampler() : nullptr;
			const MediumCoefficientsNM coefficients =
				medium->GetCoefficientsNM(Point3(0,0,0.5),430.0);
			Check( coefficients.sigma_t==0.0 && coefficients.sigma_s==0.0 &&
				coefficients.emission==0.0 &&
				medium->GetThermalEmissionNM(Point3(0,0,0.5),430.0)==0.0,
				"synthetic chem source has zero soot, extinction, and Kirchhoff emission" );
			Check( lights && lights->GetVolumeEmissionMediumCount()==0 &&
				lights->GetEquiangularPivotEntryCount()==0,
				"chem-only source is excluded from volume NEE and equiangular pivots" );

			const Scalar nm = 430.0;
			const Scalar expected =
				(SyntheticChemEmissionMedium::ReactionEnd()-
					SyntheticChemEmissionMedium::ReactionStart()) *
				SyntheticChemEmissionMedium::BandIntegratedSource() /
				(4.0*PI) /
				(SyntheticChemEmissionMedium::BandEndNM()-
					SyntheticChemEmissionMedium::BandStartNM());
			const unsigned int samples = 120000;
			const RasterizerState rast = {0,0};
			const Ray ray(Point3(0,0,0),Vector3(0,0,1));

			RandomNumberGenerator ptRng(0xc4e8001u);
			RuntimeContext ptRc(ptRng,RuntimeContext::PASS_NORMAL,false);
			IndependentSampler ptSampler(ptRng);
			long double ptSum = 0.0;
			for( unsigned int i=0; i<samples; ++i ) {
				ptSum += integrator->IntegrateRayNM(
					ptRc,rast,ray,nm,*job->GetScene(),*caster,
					ptSampler,nullptr,nullptr);
			}

			RandomNumberGenerator shaderRng(0xc4e8002u);
			RuntimeContext shaderRc(
				shaderRng,RuntimeContext::PASS_NORMAL,false);
			RandomNumberGenerator terminalRng(0xc4e8003u);
			RuntimeContext terminalRc(
				terminalRng,RuntimeContext::PASS_NORMAL,false);
			IRayCaster::RAY_STATE state;
			IRayCaster::RAY_STATE terminalState;
			terminalState.depth = 1;
			long double shaderSum = 0.0;
			long double terminalSum = 0.0;
			for( unsigned int i=0; i<samples; ++i ) {
				Scalar value = 0.0;
				caster->CastRayNM(
					shaderRc,rast,ray,value,state,nm,nullptr,nullptr);
				shaderSum += value;
				Scalar terminalValue = 0.0;
				caster->CastRayNM(
					terminalRc,rast,ray,terminalValue,terminalState,
					nm,nullptr,nullptr);
				terminalSum += terminalValue;
			}

			const Scalar ptMean =
				static_cast<Scalar>(ptSum/static_cast<long double>(samples));
			const Scalar shaderMean =
				static_cast<Scalar>(shaderSum/static_cast<long double>(samples));
			const Scalar terminalMean =
				static_cast<Scalar>(terminalSum/static_cast<long double>(samples));
			std::cout << "  zero-soot chem target/PT/shader/terminal=" << expected <<
				"/" << ptMean << "/" << shaderMean << "/" << terminalMean <<
				std::endl;
			Check( NearRelative(ptMean,expected,0.012) &&
				NearRelative(shaderMean,expected,0.012) &&
				NearRelative(terminalMean,expected,0.012) &&
				NearRelative(ptMean,shaderMean,0.012),
				"zero-soot chem-only line source behind a null boundary matches its absolute spectral integral under the RayCaster depth cap and pure PT" );

			RandomNumberGenerator terminalRouletteRng(0xc4e8008u);
			RandomNumberGenerator terminalRouletteControl(0xc4e8008u);
			terminalRouletteControl.CanonicalRandom();
			terminalRouletteControl.CanonicalRandom();
			terminalRouletteControl.CanonicalRandom();
			RuntimeContext terminalRouletteRc(
				terminalRouletteRng,RuntimeContext::PASS_NORMAL,false);
			IRayCaster::RAY_STATE terminalRouletteState;
			terminalRouletteState.depth = 1;
			terminalRouletteState.importance = 0.005;
			Scalar terminalRouletteValue = 0.0;
			caster->CastRayNM(
				terminalRouletteRc,rast,ray,terminalRouletteValue,
				terminalRouletteState,nm,nullptr,nullptr);
			const Scalar terminalRouletteNext =
				terminalRouletteRng.CanonicalRandom();
			const Scalar terminalRouletteControlNext =
				terminalRouletteControl.CanonicalRandom();
			if( terminalRouletteNext != terminalRouletteControlNext ) {
				std::cout << "  terminal next/control RNG=" <<
					terminalRouletteNext << "/" <<
					terminalRouletteControlNext << std::endl;
			}
			Check( terminalRouletteNext == terminalRouletteControlNext,
				"terminal RayCaster source-only segment consumes chem proposal draws but no roulette draw" );

			const unsigned int rrBranchSamples = 30000;
			unsigned int rejectedSamples = 0;
			unsigned int survivedSamples = 0;
			unsigned int rrSeed = 1;
			long double rejectedSum = 0.0;
			long double survivedSum = 0.0;
			while( rejectedSamples<rrBranchSamples ||
				survivedSamples<rrBranchSamples ) {
				RandomNumberGenerator probe(rrSeed);
				const bool rejected = probe.CanonicalRandom()>=0.5;
				if( (rejected && rejectedSamples>=rrBranchSamples) ||
					(!rejected && survivedSamples>=rrBranchSamples) ) {
					++rrSeed;
					continue;
				}
				RandomNumberGenerator branchRng(rrSeed++);
				RuntimeContext branchRc(
					branchRng,RuntimeContext::PASS_NORMAL,false);
				IRayCaster::RAY_STATE branchState;
				branchState.importance = 0.005;
				Scalar value = 0.0;
				rrCaster->CastRayNM(
					branchRc,rast,ray,value,branchState,nm,nullptr,nullptr);
				if( rejected ) {
					rejectedSum += value;
					++rejectedSamples;
				} else {
					survivedSum += value;
					++survivedSamples;
				}
			}
			const Scalar rejectedMean = static_cast<Scalar>(
				rejectedSum/static_cast<long double>(rejectedSamples));
			const Scalar survivedMean = static_cast<Scalar>(
				survivedSum/static_cast<long double>(survivedSamples));
			Check( rejectedSamples==rrBranchSamples &&
				survivedSamples==rrBranchSamples &&
				NearRelative(rejectedMean,expected,0.02) &&
				NearRelative(survivedMean,expected,0.02),
				"representable p=0.5 roulette rejection and survival both leave same-segment chem source unweighted across a null boundary" );

			const unsigned int hwssSamples = 40000;
			SampledWavelengths requested;
			requested.lambda[0] = nm;
			requested.lambda[1] = 500.0;
			requested.lambda[2] = 600.0;
			requested.lambda[3] = 700.0;
			for( unsigned int w=0; w<SampledWavelengths::N; ++w ) {
				requested.pdf[w] = 1.0/400.0;
			}
			RandomNumberGenerator hwssRng(0xc4e8004u);
			RuntimeContext hwssRc(
				hwssRng,RuntimeContext::PASS_NORMAL,false);
			IndependentSampler hwssSampler(hwssRng);
			RandomNumberGenerator casterHWSSRng(0xc4e8007u);
			RuntimeContext casterHWSSRc(
				casterHWSSRng,RuntimeContext::PASS_NORMAL,false);
			long double hwssSums[SampledWavelengths::N] = {0,0,0,0};
			long double casterHWSSSums[SampledWavelengths::N] = {0,0,0,0};
			const IORStack hwssIorStack(1.0);
			for( unsigned int i=0; i<hwssSamples; ++i ) {
				SampledWavelengths swl = requested;
				Scalar values[SampledWavelengths::N] = {0,0,0,0};
				integrator->IntegrateRayHWSS(
					hwssRc,rast,ray,swl,*job->GetScene(),*caster,
					hwssSampler,nullptr,values,nullptr);
				for( unsigned int w=0; w<SampledWavelengths::N; ++w ) {
					hwssSums[w] += values[w];
				}
				SampledWavelengths casterSWL = requested;
				Scalar casterValues[SampledWavelengths::N] = {0,0,0,0};
				caster->CastRayHWSS(
					casterHWSSRc,rast,ray,casterValues,terminalState,
					casterSWL,nullptr,nullptr,hwssIorStack);
				for( unsigned int w=0; w<SampledWavelengths::N; ++w ) {
					casterHWSSSums[w] += casterValues[w];
				}
			}
			const Scalar hwssHeroMean = static_cast<Scalar>(
				hwssSums[0]/static_cast<long double>(hwssSamples));
			const Scalar casterHWSSHeroMean = static_cast<Scalar>(
				casterHWSSSums[0]/static_cast<long double>(hwssSamples));
			Check( NearRelative(hwssHeroMean,expected,0.02) &&
				hwssSums[1]==0.0 && hwssSums[2]==0.0 && hwssSums[3]==0.0,
				"PT fire HWSS requests take four independent NM chem paths with wavelength-bound values" );
			Check( NearRelative(casterHWSSHeroMean,expected,0.02) &&
				casterHWSSSums[1]==0.0 && casterHWSSSums[2]==0.0 &&
				casterHWSSSums[3]==0.0,
				"RayCaster fire HWSS falls back before terminal depth and crosses an exact null boundary on four wavelength-bound NM paths" );

			const unsigned int rrHWSSBranchSamples = 12000;
			unsigned int rejectedHWSSSamples = 0;
			unsigned int survivedHWSSSamples = 0;
			unsigned int rrHWSSSeed = 1;
			long double rejectedHWSSSum = 0.0;
			long double survivedHWSSSum = 0.0;
			while( rejectedHWSSSamples<rrHWSSBranchSamples ||
				survivedHWSSSamples<rrHWSSBranchSamples ) {
				RandomNumberGenerator probe(rrHWSSSeed);
				const bool rejected = probe.CanonicalRandom()>=0.5;
				if( (rejected && rejectedHWSSSamples>=rrHWSSBranchSamples) ||
					(!rejected && survivedHWSSSamples>=rrHWSSBranchSamples) ) {
					++rrHWSSSeed;
					continue;
				}
				RandomNumberGenerator branchRng(rrHWSSSeed++);
				RuntimeContext branchRc(
					branchRng,RuntimeContext::PASS_NORMAL,false);
				IRayCaster::RAY_STATE branchState;
				branchState.importance = 0.005;
				SampledWavelengths branchSWL = requested;
				Scalar branchValues[SampledWavelengths::N] = {0,0,0,0};
				rrCaster->CastRayHWSS(
					branchRc,rast,ray,branchValues,branchState,
					branchSWL,nullptr,nullptr,hwssIorStack);
				if( rejected ) {
					rejectedHWSSSum += branchValues[0];
					++rejectedHWSSSamples;
				} else {
					survivedHWSSSum += branchValues[0];
					++survivedHWSSSamples;
				}
			}
			const Scalar rejectedHWSSMean = static_cast<Scalar>(
				rejectedHWSSSum/
					static_cast<long double>(rejectedHWSSSamples));
			const Scalar survivedHWSSMean = static_cast<Scalar>(
				survivedHWSSSum/
					static_cast<long double>(survivedHWSSSamples));
			Check( NearRelative(rejectedHWSSMean,expected,0.035) &&
				NearRelative(survivedHWSSMean,expected,0.035),
				"RayCaster fire HWSS roulette rejection and survival leave same-segment chem unweighted across an exact null boundary" );

			const unsigned int expectedSegmentSamples =
				3*samples+1+2*rrBranchSamples+
				2*SampledWavelengths::N*hwssSamples+
				2*SampledWavelengths::N*rrHWSSBranchSamples;
			Check( medium->segmentSamples==expectedSegmentSamples &&
				medium->uniformTechniqueSamples>
					expectedSegmentSamples*45/100 &&
				medium->reactionTechniqueSamples>
					expectedSegmentSamples*45/100 &&
				medium->supportSamples>expectedSegmentSamples*45/100,
				"both halves of the pinned reaction/uniform chem proposal carry the estimate" );

			SyntheticChemEmissionMedium* attenuated =
				new SyntheticChemEmissionMedium(1.0);
			IObjectPriv* enclosure =
				job->GetScene()->GetObjects()->GetItem("seam");
			Check( enclosure && enclosure->AssignInteriorMedium(*attenuated),
				"attenuated chem reference replaces the object-bound medium" );
			caster->AttachScene(job->GetScene());
			const unsigned int attenuatedSamples = 160000;
			RandomNumberGenerator attenuatedPTRng(0xc4e8005u);
			RuntimeContext attenuatedPTRc(
				attenuatedPTRng,RuntimeContext::PASS_NORMAL,false);
			IndependentSampler attenuatedPTSampler(attenuatedPTRng);
			RandomNumberGenerator attenuatedRCRng(0xc4e8006u);
			RuntimeContext attenuatedRCRc(
				attenuatedRCRng,RuntimeContext::PASS_NORMAL,false);
			long double attenuatedPTSum = 0.0;
			long double attenuatedRCSum = 0.0;
			for( unsigned int i=0; i<attenuatedSamples; ++i ) {
				attenuatedPTSum += integrator->IntegrateRayNM(
					attenuatedPTRc,rast,ray,nm,*job->GetScene(),*caster,
					attenuatedPTSampler,nullptr,nullptr);
				IRayCaster::RAY_STATE attenuatedState;
				Scalar value = 0.0;
				caster->CastRayNM(
					attenuatedRCRc,rast,ray,value,attenuatedState,
					nm,nullptr,nullptr);
				attenuatedRCSum += value;
			}
			const Scalar attenuatedPTMean = static_cast<Scalar>(
				attenuatedPTSum/static_cast<long double>(attenuatedSamples));
			const Scalar attenuatedRCMean = static_cast<Scalar>(
				attenuatedRCSum/static_cast<long double>(attenuatedSamples));
			const Scalar sourcePerMetre =
				SyntheticChemEmissionMedium::BandIntegratedSource() /
				(4.0*PI) /
				(SyntheticChemEmissionMedium::BandEndNM()-
					SyntheticChemEmissionMedium::BandStartNM());
			const Scalar attenuatedExpected = sourcePerMetre*
				(std::exp(-0.35)-std::exp(-0.45));
			Check( NearRelative(attenuatedPTMean,attenuatedExpected,0.015) &&
				NearRelative(attenuatedRCMean,attenuatedExpected,0.015) &&
				NearRelative(attenuatedPTMean,attenuatedRCMean,0.015),
				"absorption-independent chem line estimator includes deterministic transmittance in both transport entries" );
			const RayCaster* attenuatedConcrete =
				dynamic_cast<const RayCaster*>(caster);
			const LightSampler* attenuatedLights =
				attenuatedConcrete ? attenuatedConcrete->GetLightSampler() : nullptr;
			Check( attenuatedLights &&
				attenuatedLights->GetVolumeEmissionMediumCount()==0 &&
				attenuatedLights->GetEquiangularPivotEntryCount()==0,
				"attenuated chem source remains outside thermal CDF, NEE, and equiangular sampling" );
			safe_release(attenuated);
		}

		safe_release(integrator);
		safe_release(rrCaster);
		safe_release(caster);
		safe_release(medium);
		safe_release(job);
		std::filesystem::remove(scenePath);
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
	TestImmersedReceiverDeltaTrackingCarriesDistanceMixture();
	TestThinEmitterSheetAtGrazingAngles();
	TestCameraPrimaryDirectViewKeepsWeightOne();
	TestFlameBehindGlassIsMarchOnly();
	TestPrimaryScatteringEventHonorsVolumeCapAfterEmission();
	TestSurfaceVolumeNEEProductionRoutes();
	TestOrenNayarSurfaceVolumeNEEEquality();
	TestIsotropicPhongSurfaceVolumeNEEEquality();
	TestNullBoundaryMixtureSurvivalEquality();
	TestHollowCavityFullSphereVolumeNEEEquality();
	TestPointLightFlameThreeStrategyEquality();
	TestNonNullSurfaceClearsMediumMarchCompetition();
	TestCollisionEmissionConsumesMarchCompetitionState();
	TestDirectPelEntryRejectsFire();
	TestBoundedObjectFireRejectsShaderDispatchPel();
	TestPostScatterSegmentRunsMediumTransport();
	TestTerminalSurfaceChemSegmentSkipsRoulette();
	TestForcedNullProposalsThroughPathTracing();
	TestOpticallyThickMixedDensityUsesLogDomain();
	TestObjectFireRoutingCacheRefreshesAfterBinding();
	TestLegacySceneEditorMediumUndoRefreshesCache();
	TestSpatialAdditiveSourceIsAnIndependentFullSegmentEstimator();
	TestZeroSootChemOnlyLineEstimator();
	TestFirePhaseClosureRoutesOneBoundInstancePerCollision();
	std::cout << passed << " passed, " << failed << " failed" << std::endl;
	return failed == 0 ? 0 : 1;
}
