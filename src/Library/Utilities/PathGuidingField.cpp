//////////////////////////////////////////////////////////////////////
//
//  PathGuidingField.cpp - Intel OpenPGL wrapper implementation.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 28, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PathGuidingField.h"

#ifdef RISE_ENABLE_OPENPGL

#include "../Interfaces/ILog.h"
#include <cmath>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	static const size_t kGuidingMaxSamplesPerLeaf = 64000;

	inline Scalar GuidingSampleEnergy( const PGLSampleData& sample )
	{
		const Scalar energy =
			static_cast<Scalar>( sample.weight ) *
			static_cast<Scalar>( sample.pdf );
		return (std::isfinite( energy ) && energy > 0) ? energy : 0;
	}
}

PathGuidingField::PathGuidingField(
	const PathGuidingConfig& cfg,
	const Point3& boundsMin,
	const Point3& boundsMax
	) :
  device( 0 ),
  field( 0 ),
  sampleStorage( 0 ),
  trained( false ),
  collectingTraining( false ),
  config( cfg ),
  sampleCount( 0 ),
  zeroValueSampleCount( 0 ),
  volumeSampleCount( 0 ),
  zeroValueVolumeSampleCount( 0 ),
  sampleEnergy( 0 ),
  directSampleEnergy( 0 ),
  indirectSampleEnergySquaredSum( 0 )
{
	// Create the OpenPGL device (CPU, 8-wide VMM)
	device = pglNewDevice( PGL_DEVICE_TYPE_CPU_8, 0 );

	if( !device ) {
		GlobalLog()->PrintEasyError( "PathGuidingField:: Failed to create OpenPGL device" );
		return;
	}

	// Configure field arguments with defaults
	PGLFieldArguments fieldArgs;
	pglFieldArgumentsSetDefaults(
		fieldArgs,
		PGL_SPATIAL_STRUCTURE_KDTREE,
		PGL_DIRECTIONAL_DISTRIBUTION_PARALLAX_AWARE_VMM,
		false,	// non-deterministic (faster)
		kGuidingMaxSamplesPerLeaf	// slightly coarser spatial field for smoother transport
		);

	field = pglDeviceNewField( device, fieldArgs );

	if( !field ) {
		GlobalLog()->PrintEasyError( "PathGuidingField:: Failed to create OpenPGL field" );
		return;
	}

	// Set scene bounds
	pgl_box3f bounds;
	pglBox3f( bounds,
		static_cast<float>( boundsMin.x ), static_cast<float>( boundsMin.y ), static_cast<float>( boundsMin.z ),
		static_cast<float>( boundsMax.x ), static_cast<float>( boundsMax.y ), static_cast<float>( boundsMax.z ) );
	pglFieldSetSceneBounds( field, bounds );

	// Create sample storage
	sampleStorage = pglNewSampleStorage();

	GlobalLog()->PrintEx( eLog_Info,
		"PathGuidingField:: Initialized (bounds [%.1f,%.1f,%.1f]-[%.1f,%.1f,%.1f], %u training iterations, %u spp, alpha=%.2f, sampling=%s, risCandidates=%u, maxSamplesPerLeaf=%zu)",
		boundsMin.x, boundsMin.y, boundsMin.z,
		boundsMax.x, boundsMax.y, boundsMax.z,
		config.trainingIterations, config.trainingSPP, config.alpha,
		config.samplingType == eGuidingRIS ? "RIS" : "OneSampleMIS",
		config.risCandidates,
		kGuidingMaxSamplesPerLeaf );
}

PathGuidingField::~PathGuidingField()
{
	if( sampleStorage ) {
		pglReleaseSampleStorage( sampleStorage );
		sampleStorage = 0;
	}
	if( field ) {
		pglReleaseField( field );
		field = 0;
	}
	if( device ) {
		pglReleaseDevice( device );
		device = 0;
	}
}

void PathGuidingField::BeginTrainingIteration()
{
	sampleCount = 0;
	zeroValueSampleCount = 0;
	volumeSampleCount = 0;
	zeroValueVolumeSampleCount = 0;
	sampleEnergy = 0;
	directSampleEnergy = 0;
	indirectSampleEnergySquaredSum = 0;
	collectingTraining = true;
	if( sampleStorage ) {
		pglSampleStorageClear( sampleStorage );
	}
}

void PathGuidingField::AddSample(
	const Point3& position,
	const Vector3& direction,
	Scalar distance,
	Scalar pdf,
	Scalar luminance,
	bool isDirect
	)
{
	if( !sampleStorage || !field ) {
		return;
	}

	if( pdf <= 0 || !std::isfinite( luminance ) || luminance < 0 ) {
		return;
	}

	// Clamp extreme weights to prevent outliers from corrupting the field
	const float weight = static_cast<float>( luminance / pdf );
	if( !std::isfinite( weight ) || weight < 0 ) {
		return;
	}

	PGLSampleData sample;
	pglPoint3f( sample.position,
		static_cast<float>( position.x ),
		static_cast<float>( position.y ),
		static_cast<float>( position.z ) );

	pglVec3f( sample.direction,
		static_cast<float>( direction.x ),
		static_cast<float>( direction.y ),
		static_cast<float>( direction.z ) );

	sample.weight = weight;
	sample.pdf = static_cast<float>( pdf );
	sample.distance = static_cast<float>( distance );
	sample.flags = isDirect ? PGLSampleData::EDirectLight : 0;

	pglSampleStorageAddSample( sampleStorage, sample );
}

void PathGuidingField::AddZeroValueSample(
	const Point3& position,
	const Vector3& direction
	)
{
	if( !sampleStorage || !field ) {
		return;
	}

	PGLZeroValueSampleData sample;
	pglPoint3f( sample.position,
		static_cast<float>( position.x ),
		static_cast<float>( position.y ),
		static_cast<float>( position.z ) );

	pglVec3f( sample.direction,
		static_cast<float>( direction.x ),
		static_cast<float>( direction.y ),
		static_cast<float>( direction.z ) );

	sample.volume = false;

	pglSampleStorageAddZeroValueSample( sampleStorage, sample );
}

void PathGuidingField::AddPathSegments(
	PGLPathSegmentStorage pathSegments,
	bool useNEEMiWeights,
	bool guideDirectLight,
	bool rrAffectsDirectContribution
	)
{
	if( !sampleStorage || !field || !pathSegments ) {
		return;
	}

	pglPathSegmentStoragePrepareSamples(
		pathSegments,
		useNEEMiWeights,
		guideDirectLight,
		rrAffectsDirectContribution );

	size_t numSamples = 0;
	const PGLSampleData* samples =
		pglPathSegmentStorageGetSamples( pathSegments, numSamples );
	if( samples && numSamples > 0 ) {
		pglSampleStorageAddSamples( sampleStorage, samples, numSamples );
	}

	size_t numZeroValueSamples = 0;
	const PGLZeroValueSampleData* zeroValueSamples =
		pglPathSegmentStorageGetZeroValueSamples( pathSegments, numZeroValueSamples );
	if( zeroValueSamples && numZeroValueSamples > 0 ) {
		pglSampleStorageAddZeroValueSamples(
			sampleStorage,
			zeroValueSamples,
			numZeroValueSamples );
	}
}

void PathGuidingField::EndTrainingIteration()
{
	if( !field || !sampleStorage ) {
		return;
	}

	const size_t numSamples = pglSampleStorageGetSizeSurface( sampleStorage );
	const size_t numZeroValueSamples =
		pglSampleStorageGetSizeZeroValueSurface( sampleStorage );

	sampleCount = numSamples;
	zeroValueSampleCount = numZeroValueSamples;
	sampleEnergy = 0;
	directSampleEnergy = 0;
	indirectSampleEnergySquaredSum = 0;
	for( size_t i = 0; i < numSamples; i++ )
	{
		const PGLSampleData sample =
			pglSampleStorageGetSampleSurface( sampleStorage, static_cast<int>( i ) );
		const Scalar energy = GuidingSampleEnergy( sample );
		sampleEnergy += energy;
		if( sample.flags & PGLSampleData::EDirectLight ) {
			directSampleEnergy += energy;
		} else {
			indirectSampleEnergySquaredSum += energy * energy;
		}
	}

	// pglFieldUpdate trains both surface and volume distributions
	// from the shared sample storage in a single pass — the same
	// API that Cycles uses for mixed surface+volume scenes.
	// Do NOT call pglFieldUpdateVolume separately; that would
	// double-train the volume distribution.
	pglFieldUpdate( field, sampleStorage );

	// Per-cell adaptive-α map is intentionally NOT cleared here.
	// OpenPGL's kdtree only subdivides leaves — old leaf ids become
	// orphaned interior-node ids and are never reused.  Stale
	// entries in cellState therefore just bloat memory by O(field-
	// cells) without causing wrong queries, while keeping them lets
	// α learning compound across training iterations and freeze
	// into the final render — exactly Müller 2017 v2's prescribed
	// schedule.

	// Query volume sample counts for diagnostics only.
	const size_t numVolumeSamples = pglSampleStorageGetSizeVolume( sampleStorage );
	const size_t numZeroValueVolumeSamples =
		pglSampleStorageGetSizeZeroValueVolume( sampleStorage );
	volumeSampleCount = numVolumeSamples;
	zeroValueVolumeSampleCount = numZeroValueVolumeSamples;

	trained = true;
	collectingTraining = false;

	const size_t iteration = pglFieldGetIteration( field );
	GlobalLog()->PrintEx( eLog_Info,
		"PathGuidingField:: Training iteration %zu complete (%zu surface, %zu zero-value, %zu volume, %zu vol-zero-value, energy %.6f, direct %.6f)",
		iteration,
		numSamples,
		numZeroValueSamples,
		numVolumeSamples,
		numZeroValueVolumeSamples,
		sampleEnergy,
		directSampleEnergy );
}

GuidingDistributionHandle* PathGuidingField::NewDistributionHandle() const
{
	if( !field ) {
		return 0;
	}

	GuidingDistributionHandle* handle = new GuidingDistributionHandle();
	handle->dist = pglFieldNewSurfaceSamplingDistribution( field );
	return handle;
}

bool PathGuidingField::InitDistribution(
	GuidingDistributionHandle& handle,
	const Point3& position,
	Scalar sample1D
	) const
{
	if( !field ) {
		return false;
	}

	// Lazily allocate the per-thread distribution handle
	if( !handle.dist ) {
		handle.dist = pglFieldNewSurfaceSamplingDistribution( field );
		if( !handle.dist ) {
			return false;
		}
	}

	pgl_point3f pos;
	pglPoint3f( pos,
		static_cast<float>( position.x ),
		static_cast<float>( position.y ),
		static_cast<float>( position.z ) );

	float s1d = static_cast<float>( sample1D );

	return pglFieldInitSurfaceSamplingDistribution( field, handle.dist, pos, &s1d );
}

void PathGuidingField::ApplyCosineProduct(
	GuidingDistributionHandle& handle,
	const Vector3& normal
	) const
{
	if( !handle.dist ) {
		return;
	}

	if( pglSurfaceSamplingDistributionSupportsApplyCosineProduct( handle.dist ) )
	{
		pgl_vec3f n;
		pglVec3f( n,
			static_cast<float>( normal.x ),
			static_cast<float>( normal.y ),
			static_cast<float>( normal.z ) );
		pglSurfaceSamplingDistributionApplyCosineProduct( handle.dist, n );
	}
}

Vector3 PathGuidingField::Sample(
	GuidingDistributionHandle& handle,
	const Point2& xi,
	Scalar& pdf
	) const
{
	if( !handle.dist ) {
		pdf = 0;
		return Vector3( 0, 0, 0 );
	}

	pgl_point2f sample;
	pglPoint2f( sample, static_cast<float>( xi.x ), static_cast<float>( xi.y ) );

	pgl_vec3f dir;
	float pdfF = pglSurfaceSamplingDistributionSamplePDF( handle.dist, sample, dir );

	pdf = static_cast<Scalar>( pdfF );
	return Vector3( static_cast<Scalar>( dir.x ),
					static_cast<Scalar>( dir.y ),
					static_cast<Scalar>( dir.z ) );
}

Scalar PathGuidingField::Pdf(
	GuidingDistributionHandle& handle,
	const Vector3& direction
	) const
{
	if( !handle.dist ) {
		return 0;
	}

	pgl_vec3f dir;
	pglVec3f( dir,
		static_cast<float>( direction.x ),
		static_cast<float>( direction.y ),
		static_cast<float>( direction.z ) );

	return static_cast<Scalar>( pglSurfaceSamplingDistributionPDF( handle.dist, dir ) );
}

Scalar PathGuidingField::IncomingRadiancePdf(
	GuidingDistributionHandle& handle,
	const Vector3& direction
	) const
{
	if( !handle.dist ) {
		return 0;
	}

	pgl_vec3f dir;
	pglVec3f( dir,
		static_cast<float>( direction.x ),
		static_cast<float>( direction.y ),
		static_cast<float>( direction.z ) );

	return static_cast<Scalar>(
		pglSurfaceSamplingDistributionIncomingRadiancePDF( handle.dist, dir ) );
}


//
// Per-cell adaptive alpha (Müller 2017 v2 / practical-path-guiding)
//
// Online learning of the guide-vs-BSDF mixing weight, with:
//   • Adam optimizer (β₁=0.9, β₂=0.999) — far more stable at low
//     SPP than vanilla SGD because the per-sample gradient is
//     extremely noisy (most paths see f≈0; rare ones see f much
//     larger than the mean).
//   • Deferred f estimate.  At sample time we don't know Li yet;
//     callers snapshot resultBefore + throughputBefore and compute
//     f after the path completes via:
//        f = lum(deltaResult) · combinedPdf / lum(throughputBefore)
//     This is the per-sample integrand value at the chosen
//     direction — the same quantity Tom94's reference uses.
//   • Per-cell state persists across training iterations and into
//     the final render (no clear in EndTrainingIteration; see
//     comment there).

uint32_t PathGuidingField::GetCellId(
	const GuidingDistributionHandle& handle
	) const
{
	return handle.dist ? pglSurfaceSamplingDistributionGetId( handle.dist ) : 0;
}

Scalar PathGuidingField::GetCellAlpha(
	const GuidingDistributionHandle& handle
	) const
{
	if( !handle.dist ) {
		return 0.5;
	}
	const uint32_t cellId = pglSurfaceSamplingDistributionGetId( handle.dist );

	std::lock_guard<std::mutex> lock( cellAlphaMutex );
	auto it = cellState.find( cellId );
	if( it == cellState.end() ) {
		return 0.5;
	}
	const float theta = it->second.theta;
	return static_cast<Scalar>( 1.0f / (1.0f + std::exp( -theta )) );
}

void PathGuidingField::UpdateCellAlpha(
	uint32_t cellId,
	Scalar bsdfPdf,
	Scalar guidePdf,
	Scalar f,
	Scalar combinedPdf,
	Scalar learningRate
	) const
{
	if( combinedPdf <= 0 || !std::isfinite( f ) || f <= 0 ) {
		return;
	}

	std::lock_guard<std::mutex> lock( cellAlphaMutex );
	CellAdamState& s = cellState[ cellId ];	// inserts default if missing
	const float alpha = 1.0f / (1.0f + std::exp( -s.theta ));

	// dL/dα for KL(p_optimal || p_combined), p_optimal ∝ f, single-
	// sample IS estimator drawn from p_combined.  Negative when
	// guide is more concentrated where f is large (push α up).
	const float dL_dalpha = -( static_cast<float>( f ) /
		static_cast<float>( combinedPdf * combinedPdf ) ) *
		( static_cast<float>( guidePdf ) - static_cast<float>( bsdfPdf ) );

	// Sigmoid derivative: dα/dθ = α(1-α).
	const float g = dL_dalpha * alpha * (1.0f - alpha);

	if( !std::isfinite( g ) ) {
		return;
	}

	// Adam update.
	const float beta1 = 0.9f;
	const float beta2 = 0.999f;
	const float eps   = 1e-8f;

	s.t++;
	s.m = beta1 * s.m + (1.0f - beta1) * g;
	s.v = beta2 * s.v + (1.0f - beta2) * g * g;

	const float bias1 = 1.0f - std::pow( beta1, static_cast<float>( s.t ) );
	const float bias2 = 1.0f - std::pow( beta2, static_cast<float>( s.t ) );
	const float m_hat = s.m / bias1;
	const float v_hat = s.v / bias2;

	s.theta -= static_cast<float>( learningRate ) * m_hat /
		( std::sqrt( v_hat ) + eps );

	// Clamp so α stays in (~3.4e-4, ~0.9997); keeps the sigmoid in
	// its sensitive range while allowing pure-BSDF / pure-guide
	// modes to be chosen confidently.
	if( s.theta > 8.0f ) s.theta = 8.0f;
	if( s.theta < -8.0f ) s.theta = -8.0f;
}


//
// Volume guiding — training
//

void PathGuidingField::AddVolumeSample(
	const Point3& position,
	const Vector3& direction,
	Scalar distance,
	Scalar pdf,
	Scalar luminance,
	bool isDirect
	)
{
	if( !sampleStorage || !field ) {
		return;
	}

	if( pdf <= 0 || !std::isfinite( luminance ) || luminance < 0 ) {
		return;
	}

	const float weight = static_cast<float>( luminance / pdf );
	if( !std::isfinite( weight ) || weight < 0 ) {
		return;
	}

	PGLSampleData sample;
	pglPoint3f( sample.position,
		static_cast<float>( position.x ),
		static_cast<float>( position.y ),
		static_cast<float>( position.z ) );

	pglVec3f( sample.direction,
		static_cast<float>( direction.x ),
		static_cast<float>( direction.y ),
		static_cast<float>( direction.z ) );

	sample.weight = weight;
	sample.pdf = static_cast<float>( pdf );
	sample.distance = static_cast<float>( distance );
	sample.flags = (isDirect ? PGLSampleData::EDirectLight : 0) |
		PGLSampleData::EInsideVolume;

	pglSampleStorageAddSample( sampleStorage, sample );
}

void PathGuidingField::AddZeroValueVolumeSample(
	const Point3& position,
	const Vector3& direction
	)
{
	if( !sampleStorage || !field ) {
		return;
	}

	PGLZeroValueSampleData sample;
	pglPoint3f( sample.position,
		static_cast<float>( position.x ),
		static_cast<float>( position.y ),
		static_cast<float>( position.z ) );

	pglVec3f( sample.direction,
		static_cast<float>( direction.x ),
		static_cast<float>( direction.y ),
		static_cast<float>( direction.z ) );

	sample.volume = true;

	pglSampleStorageAddZeroValueSample( sampleStorage, sample );
}


//
// Volume guiding — query
//

bool PathGuidingField::InitVolumeDistribution(
	GuidingVolumeDistributionHandle& handle,
	const Point3& position,
	Scalar sample1D
	) const
{
	if( !field ) {
		return false;
	}

	if( !handle.dist ) {
		handle.dist = pglFieldNewVolumeSamplingDistribution( field );
		if( !handle.dist ) {
			return false;
		}
	}

	pgl_point3f pos;
	pglPoint3f( pos,
		static_cast<float>( position.x ),
		static_cast<float>( position.y ),
		static_cast<float>( position.z ) );

	float s1d = static_cast<float>( sample1D );

	return pglFieldInitVolumeSamplingDistribution( field, handle.dist, pos, &s1d );
}

void PathGuidingField::ApplyHGProduct(
	GuidingVolumeDistributionHandle& handle,
	const Vector3& wo,
	Scalar meanCosine
	) const
{
	if( !handle.dist ) {
		return;
	}

	if( pglVolumeSamplingDistributionSupportsApplySingleLobeHenyeyGreensteinProduct( handle.dist ) )
	{
		pgl_vec3f dir;
		pglVec3f( dir,
			static_cast<float>( wo.x ),
			static_cast<float>( wo.y ),
			static_cast<float>( wo.z ) );
		pglVolumeSamplingDistributionApplySingleLobeHenyeyGreensteinProduct(
			handle.dist, dir, static_cast<float>( meanCosine ) );
	}
}

Vector3 PathGuidingField::SampleVolume(
	GuidingVolumeDistributionHandle& handle,
	const Point2& xi,
	Scalar& pdf
	) const
{
	if( !handle.dist ) {
		pdf = 0;
		return Vector3( 0, 0, 0 );
	}

	pgl_point2f sample;
	pglPoint2f( sample, static_cast<float>( xi.x ), static_cast<float>( xi.y ) );

	pgl_vec3f dir;
	float pdfF = pglVolumeSamplingDistributionSamplePDF( handle.dist, sample, dir );

	pdf = static_cast<Scalar>( pdfF );
	return Vector3( static_cast<Scalar>( dir.x ),
					static_cast<Scalar>( dir.y ),
					static_cast<Scalar>( dir.z ) );
}

Scalar PathGuidingField::PdfVolume(
	GuidingVolumeDistributionHandle& handle,
	const Vector3& direction
	) const
{
	if( !handle.dist ) {
		return 0;
	}

	pgl_vec3f dir;
	pglVec3f( dir,
		static_cast<float>( direction.x ),
		static_cast<float>( direction.y ),
		static_cast<float>( direction.z ) );

	return static_cast<Scalar>( pglVolumeSamplingDistributionPDF( handle.dist, dir ) );
}

#endif // RISE_ENABLE_OPENPGL
