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
  sampleEnergy( 0 ),
  directSampleEnergy( 0 )
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

	GlobalLog()->PrintEx( eLog_Event,
		"PathGuidingField:: Initialized (bounds [%.1f,%.1f,%.1f]-[%.1f,%.1f,%.1f], %u training iterations, %u spp, alpha=%.2f, maxSamplesPerLeaf=%zu)",
		boundsMin.x, boundsMin.y, boundsMin.z,
		boundsMax.x, boundsMax.y, boundsMax.z,
		config.trainingIterations, config.trainingSPP, config.alpha, kGuidingMaxSamplesPerLeaf );
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
	sampleEnergy = 0;
	directSampleEnergy = 0;
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
	for( size_t i = 0; i < numSamples; i++ )
	{
		const PGLSampleData sample =
			pglSampleStorageGetSampleSurface( sampleStorage, static_cast<int>( i ) );
		const Scalar energy = GuidingSampleEnergy( sample );
		sampleEnergy += energy;
		if( sample.flags & PGLSampleData::EDirectLight ) {
			directSampleEnergy += energy;
		}
	}

	pglFieldUpdate( field, sampleStorage );
	trained = true;
	collectingTraining = false;

	const size_t iteration = pglFieldGetIteration( field );
	GlobalLog()->PrintEx( eLog_Event,
		"PathGuidingField:: Training iteration %zu complete (%zu surface samples, %zu zero-value, %zu added, %zu zero-value added, energy %.6f, direct %.6f)",
		iteration,
		numSamples,
		numZeroValueSamples,
		sampleCount,
		zeroValueSampleCount,
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

#endif // RISE_ENABLE_OPENPGL
