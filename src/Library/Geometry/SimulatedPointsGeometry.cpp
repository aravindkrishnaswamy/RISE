//////////////////////////////////////////////////////////////////////
//
//  SimulatedPointsGeometry.cpp - Sphere-per-particle IGeometry and
//    ITimeVaryingAssetProvider backed by Simulation::PointsCacheSource,
//    with SAH BVH, linear shutter motion blur, and render freeze leases.
//
//  Tabs: 4
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SimulatedPointsGeometry.h"
#include "../Animation/Animator.h"
#include "../Animation/TimeIndexedView.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/SurfaceCurvature.h"
#include "../Interfaces/ILog.h"
#include <algorithm>
#include <cmath>
#include <limits>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	void ToPoints( const std::vector<double>& xyz, std::vector<Point3>& out )
	{
		out.resize( xyz.size() / 3 );
		for( std::size_t i = 0; i < out.size(); ++i ) out[i] = Point3( xyz[3*i], xyz[3*i+1], xyz[3*i+2] );
	}

	BoundingBox EmptyBox()
	{
		return BoundingBox( Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ), Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY ) );
	}

	void IncludeSphere( BoundingBox& box, const Point3& c, const Scalar r )
	{
		box.Include( Point3( c.x - r, c.y - r, c.z - r ) );
		box.Include( Point3( c.x + r, c.y + r, c.z + r ) );
	}

	//! SphereGeometry::ComputeSurfaceDerivatives for a sphere of radius r at
	//! the origin (the same expressions, so the hit records agree exactly).
	SurfaceDerivatives SphereDerivatives( const Point3& p, const Scalar r )
	{
		SurfaceDerivatives sd;
		const Scalar phi = atan2( p.z, p.x );
		const Scalar cosTheta = r > NEARZERO ? p.y / r : 0.0;
		const Scalar clampedCosTheta = cosTheta > 1.0 ? 1.0 : ( cosTheta < -1.0 ? -1.0 : cosTheta );
		const Scalar theta = acos( clampedCosTheta );
		const Scalar sinTheta = sin( theta );
		const Scalar cosPhi = cos( phi );
		const Scalar sinPhi = sin( phi );
		sd.dpdu = Vector3( -r * sinTheta * sinPhi, 0.0, r * sinTheta * cosPhi );
		sd.dpdv = Vector3( r * clampedCosTheta * cosPhi, -r * sinTheta, r * clampedCosTheta * sinPhi );
		sd.dndu = Vector3( -sinTheta * sinPhi, 0.0, sinTheta * cosPhi );
		sd.dndv = Vector3( clampedCosTheta * cosPhi, -sinTheta, clampedCosTheta * sinPhi );
		sd.uv = Point2( phi, theta );
		sd.valid = true;
		sd.dsdu = -1.0 / TWO_PI;
		sd.dsdv = 0.0;
		sd.dtdu = 0.0;
		sd.dtdv = 1.0 / PI;
		sd.texChartValid = true;
		return sd;
	}
}

SimulatedPointsGeometry* SimulatedPointsGeometry::Create( const std::string& cacheRoot, const Options& options, std::string& error )
{
	std::unique_ptr<Simulation::PointsCacheSource> source;
	if( !Simulation::PointsCacheSource::Open( cacheRoot, options.source, source, error ) ) return nullptr;
	SimulatedPointsGeometry* g = new SimulatedPointsGeometry( options, std::move( source ) );
	// The bind frame at its own time, no shutter.
	const double t = Simulation::PointsFrameSceneTime( *g->m_source, g->m_source->BindIndex() );
	if( !g->PrepareFrame( RenderTimeSupport{ t, t, t }, error, nullptr ) ) {
		g->release();
		return nullptr;
	}
	return g;
}

SimulatedPointsGeometry* SimulatedPointsGeometry::CreateUnbound( const Options& options )
{
	SimulatedPointsGeometry* g = new SimulatedPointsGeometry( options, nullptr );
	g->m_hidden = true;
	g->m_identity = "unbound";
	return g;
}

SimulatedPointsGeometry::SimulatedPointsGeometry( const Options& options, std::unique_ptr<Simulation::PointsCacheSource> source ) :
  m_options( options ),
  m_source( std::move( source ) ),
  m_clock( nullptr ),
  m_bvh( nullptr ),
  m_blur( false ),
  m_hidden( false ),
  m_openTime( 0 ), m_closeTime( 0 ), m_nominal( 0 ), m_invSpan( 0 ),
  m_installedFrame( 0 ),
  m_area( 0 ),
  m_installs( 0 ),
  m_lastWarnedMissing( std::numeric_limits<std::int64_t>::min() ),
  m_leases( 0 ),
  m_generation( 0 ), m_frozenGeneration( 0 )
{
}

SimulatedPointsGeometry::~SimulatedPointsGeometry()
{
	safe_release( m_bvh );
	if( m_clock ) m_clock->release();
}

void SimulatedPointsGeometry::SetSampleClock( const Animator* clock )
{
	if( clock == m_clock ) return;
	if( clock ) clock->addref();
	if( m_clock ) m_clock->release();
	m_clock = clock;
}

// Swaps prepared particle buffers into active state, updates total surface
// area, rebuilds or creates the SAH BVH over motion-swept AABBs, and bumps revision.
void SimulatedPointsGeometry::Install( State& next )
{
	// Blur state first: the BVH build reads the element boxes.
	m_centre.swap( next.centre ); m_open.swap( next.open ); m_close.swap( next.close );
	m_radius.swap( next.radius ); m_color.swap( next.color );
	std::swap( m_blur, next.blur ); std::swap( m_hidden, next.hidden );
	std::swap( m_openTime, next.openTime ); std::swap( m_closeTime, next.closeTime ); std::swap( m_nominal, next.nominal );
	std::swap( m_installedFrame, next.frame );
	m_identity.swap( next.identity );
	m_invSpan = m_closeTime > m_openTime ? 1.0 / ( m_closeTime - m_openTime ) : 0.0;
	m_area = 0;
	for( const Scalar r : m_radius ) m_area += FOUR_PI * r * r;

	m_elements.resize( m_radius.size() );
	for( std::uint32_t i = 0; i < m_elements.size(); ++i ) m_elements[i] = i;
	BVH<std::uint32_t>::BuildOptions build;
	build.parallel = true;
	build.quiet = true;
	if( m_bvh ) {
		m_bvh->Rebuild( m_elements, build );
	} else {
		AccelerationConfig cfg;
		cfg.maxLeafSize         = 4;
		cfg.binCount            = 32;
		cfg.sahTraversalCost    = 1.0;
		cfg.sahIntersectionCost = 1.0;
		cfg.doubleSided         = false;
		m_bvh = new BVH<std::uint32_t>( *this, m_elements, EmptyBox(), cfg, build );
		GlobalLog()->PrintNew( m_bvh, __FILE__, __LINE__, "simulated points BVH" );
	}
	BumpShapeRevision();
}

// Refuses mutation while frozen for rendering; skips rebuild when both the
// cache's CheapSnapshot and render time support are unchanged.
bool SimulatedPointsGeometry::PrepareAssetForRender( const RenderTimeSupport& support, std::string& error,
	std::function<void()>* rollback )
{
	{
		std::lock_guard<std::mutex> lock( m_freezeMutex );
		if( m_leases ) { error = "mutation_frozen"; return false; }
	}
	if( !m_source ) return true;
	std::shared_ptr<const Simulation::IReadOnlyCacheSnapshot> cheap = m_source->CheapSnapshot();
	if( cheap && cheap == m_preparedSnapshot && support.nominal == m_preparedSupport.nominal &&
		support.open == m_preparedSupport.open && support.close == m_preparedSupport.close ) return true;
	m_preparedSnapshot.reset();
	if( !PrepareFrame( support, error, rollback ) ) return false;
	if( rollback ) {
		std::function<void()> inner = std::move( *rollback );
		*rollback = [this, inner]() {
			if( inner ) inner();
			m_preparedSnapshot.reset();
		};
	}
	if( cheap ) {
		m_preparedSnapshot = std::move( cheap );
		m_preparedSupport = support;
	}
	return true;
}

// Samples the points cache for `support`, applying predictive fail-closed /
// Hide / Hold missing-frame policies, eliding blur for stationary particles,
// and populating an optional state-swap rollback closure.
bool SimulatedPointsGeometry::PrepareFrame( const RenderTimeSupport& support, std::string& error,
	std::function<void()>* rollback )
{
	if( !std::isfinite( support.nominal ) || !std::isfinite( support.open ) ||
		!std::isfinite( support.close ) || support.open > support.close ) {
		error = "simulated points render time support is invalid";
		return false;
	}
	Simulation::PointsFrameSample sample;
	if( !m_source->Sample( support.nominal, support.open, support.close, m_options.motionBlur, sample, error ) )
		return false;
	State next;
	if( sample.state == Simulation::PointsFrameSample::State::Missing ) {
		if( m_options.source.predictive ) {
			error = "simulated points frame " + std::to_string( sample.index ) + " is unavailable; a predictive binding fails closed";
			return false;
		}
		const bool hide = m_options.source.missing == Simulation::MeshMissingPolicy::Hide;
		if( m_lastWarnedMissing != sample.index ) {
			m_lastWarnedMissing = sample.index;
			GlobalLog()->PrintEx( eLog_Warning,
				"SimulatedPointsGeometry:: cache frame %lld is not baked yet (partial cache); %s",
				(long long)sample.index, hide ? "hiding the points" : "holding the previously installed frame" );
		}
		if( hide ) {
			if( m_hidden ) return true;
			m_hidden = true;
			++m_generation;
			if( rollback ) *rollback = [this]() { m_hidden = false; ++m_generation; };
			return true;
		}
		if( !m_blur ) return true;
		// Hold the previous frame's nominal centres without its keys.
		next.centre = m_centre; next.radius = m_radius; next.color = m_color;
		next.frame = m_installedFrame; next.identity = m_identity + "|held";
	} else {
		if( sample.identity == m_identity && !m_hidden && m_openTime == support.open && m_closeTime == support.close &&
			m_nominal == support.nominal ) return true;
		ToPoints( sample.nominal, next.centre );
		next.radius.assign( sample.radii.begin(), sample.radii.end() );
		next.color.swap( sample.colors );
		if( !sample.open.empty() ) {
			ToPoints( sample.open, next.open );
			ToPoints( sample.close, next.close );
			bool moves = false;
			for( std::size_t i = 0; i < next.open.size() && !moves; ++i )
				moves = next.open[i].x != next.close[i].x || next.open[i].y != next.close[i].y || next.open[i].z != next.close[i].z;
			next.blur = moves;
			if( !moves ) { next.open.clear(); next.close.clear(); }
		}
		next.frame = sample.index;
		next.identity = sample.identity;
		++m_installs;
	}
	next.openTime = support.open; next.closeTime = support.close; next.nominal = support.nominal;
	Install( next );	// `next` now holds the previous frame
	++m_generation;
	if( rollback ) {
		auto saved = std::make_shared<State>( std::move( next ) );
		*rollback = [this, saved]() {
			Install( *saved );
			++m_generation;
		};
	}
	return true;
}

// Reference-counted render freeze lease; ReleaseAssetFreeze verifies that
// m_generation did not advance while any lease was held.
bool SimulatedPointsGeometry::AcquireAssetFreeze( std::string& )
{
	std::lock_guard<std::mutex> lock( m_freezeMutex );
	if( m_leases++ == 0 ) m_frozenGeneration = m_generation;
	return true;
}

bool SimulatedPointsGeometry::ReleaseAssetFreeze( std::string& error )
{
	std::lock_guard<std::mutex> lock( m_freezeMutex );
	if( !m_leases ) { error = "simulated points freeze released without an acquire"; return false; }
	--m_leases;
	if( m_generation != m_frozenGeneration ) { error = "simulated points changed during render"; return false; }
	return true;
}

// Computes clamped [0, 1] shutter fraction from TLS per-ray sample time
// (falling back to the animator's last evaluated time or frame nominal time).
Scalar SimulatedPointsGeometry::ShutterFraction() const
{
	double t = m_nominal;
	const TimeIndexed::SampleTime& sample = TimeIndexed::tlsSampleTime;
	if( sample.valid ) {
		t = sample.time;
	} else if( m_clock ) {
		const double shared = m_clock->LastEvaluatedTime();
		if( std::isfinite( shared ) ) t = shared;
	}
	const double f = ( t - m_openTime ) * m_invSpan;
	return f > 0.0 ? ( f < 1.0 ? Scalar( f ) : Scalar( 1 ) ) : Scalar( 0 );
}

Point3 SimulatedPointsGeometry::CentreAt( const std::uint32_t i, const Scalar f ) const
{
	if( !m_blur ) return m_centre[i];
	const Point3& a = m_open[i];
	const Point3& b = m_close[i];
	const Scalar g = Scalar( 1 ) - f;
	return Point3( a.x * g + b.x * f, a.y * g + b.y * f, a.z * g + b.z * f );
}

void SimulatedPointsGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool ) const
{
	if( m_hidden || !m_bvh ) return;
	m_bvh->IntersectRay( ri, bHitFrontFaces, bHitBackFaces );
}

bool SimulatedPointsGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	if( m_hidden || !m_bvh ) return false;
	return m_bvh->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces );
}

void SimulatedPointsGeometry::RayElementIntersection( RayIntersectionGeometric& ri, const std::uint32_t elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	// SphereGeometry::IntersectRay in the sphere's own frame.
	const Scalar r = m_radius[elem];
	if( !( r > 0 ) ) return;
	const Point3 c = CentreAt( elem, m_blur ? ShutterFraction() : Scalar( 0 ) );
	const Ray local( Point3( ri.ray.origin.x - c.x, ri.ray.origin.y - c.y, ri.ray.origin.z - c.z ), ri.ray.Dir() );
	const bool inside = GeometricUtilities::IsPointInsideSphere( local.origin, r, Point3( 0, 0, 0 ) );
	if( bHitFrontFaces && !bHitBackFaces && inside ) return;
	if( !bHitFrontFaces && bHitBackFaces && !inside ) return;
	HIT h;
	RaySphereIntersection( local, h, r );
	if( !h.bHit || !( h.dRange < ri.range ) ) return;
	ri.bHit = true;
	ri.range = h.dRange;
	ri.range2 = h.dRange2;
	const Point3 p = local.PointAtLength( h.dRange );
	ri.ptIntersection = Point3( p.x + c.x, p.y + c.y, p.z + c.z );
	ri.vNormal = Vector3Ops::Normalize( Vector3Ops::mkVector3( p, Point3( 0, 0, 0 ) ) );
	ri.vGeomNormal = ri.vNormal;
	const Point3 pe = local.PointAtLength( h.dRange2 );
	ri.ptExit = Point3( pe.x + c.x, pe.y + c.y, pe.z + c.z );
	ri.vNormal2 = Vector3Ops::Normalize( Vector3Ops::mkVector3( pe, Point3( 0, 0, 0 ) ) );
	ri.vGeomNormal2 = ri.vNormal2;
	GeometricUtilities::SphereTextureCoord( Vector3( 0.0, 1.0, 0.0 ), Vector3( -1.0, 0.0, 0.0 ), ri.vNormal, ri.ptCoord );
	const SurfaceDerivatives sd = SphereDerivatives( p, r );
	ri.derivatives.dpdu = sd.dpdu;
	ri.derivatives.dpdv = sd.dpdv;
	ri.derivatives.dndu = sd.dndu;
	ri.derivatives.dndv = sd.dndv;
	ri.derivatives.valid = true;
	ri.derivatives.dsdu = sd.dsdu;
	ri.derivatives.dsdv = sd.dsdv;
	ri.derivatives.dtdu = sd.dtdu;
	ri.derivatives.dtdv = sd.dtdv;
	ri.derivatives.texChartValid = sd.texChartValid;
	if( SurfaceCurvatureDemand::Any() ) {
		ri.derivatives.scaleHint = SurfaceCurvature::ScaleHintFromBoundingBox( BoundingBox( Point3( -r, -r, -r ), Point3( r, r, r ) ) );
	}
	ri.bShadingTangentFromGeometry = true;
	ri.vShadingTangent = sd.dpdu;
	ri.bHasShadingTangent = true;
	ri.bHasVertexColor = !m_color.empty();
	if( ri.bHasVertexColor ) ri.vColor = RISEPel( m_color[3*elem], m_color[3*elem+1], m_color[3*elem+2] );
}

void SimulatedPointsGeometry::RayElementIntersection( RayIntersection& ri, const std::uint32_t elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool ) const
{
	RayElementIntersection( ri.geometric, elem, bHitFrontFaces, bHitBackFaces );
}

bool SimulatedPointsGeometry::RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const std::uint32_t elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	const Scalar r = m_radius[elem];
	if( !( r > 0 ) ) return false;
	const Point3 c = CentreAt( elem, m_blur ? ShutterFraction() : Scalar( 0 ) );
	const Ray local( Point3( ray.origin.x - c.x, ray.origin.y - c.y, ray.origin.z - c.z ), ray.Dir() );
	const bool inside = GeometricUtilities::IsPointInsideSphere( local.origin, r, Point3( 0, 0, 0 ) );
	if( bHitFrontFaces && !bHitBackFaces && inside ) return false;
	if( !bHitFrontFaces && bHitBackFaces && !inside ) return false;
	HIT h;
	RaySphereIntersection( local, h, r );
	return h.bHit && !( h.dRange < NEARZERO || h.dRange > dHowFar );
}

BoundingBox SimulatedPointsGeometry::GetElementBoundingBox( const std::uint32_t elem ) const
{
	BoundingBox box = EmptyBox();
	const Scalar r = m_radius[elem];
	IncludeSphere( box, m_centre[elem], r );
	if( m_blur ) {
		IncludeSphere( box, m_open[elem], r );
		IncludeSphere( box, m_close[elem], r );
	}
	return box;
}

bool SimulatedPointsGeometry::ElementBoxIntersection( const std::uint32_t elem, const BoundingBox& bbox ) const
{
	return GetElementBoundingBox( elem ).DoIntersect( bbox );
}

char SimulatedPointsGeometry::WhichSideofPlaneIsElement( const std::uint32_t elem, const Plane& plane ) const
{
	const BoundingBox b = GetElementBoundingBox( elem );
	return GeometricUtilities::WhichSideOfPlane( plane, b );
}

void SimulatedPointsGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	const BoundingBox b = GenerateBoundingBox();
	ptCenter = Point3( 0.5 * ( b.ll.x + b.ur.x ), 0.5 * ( b.ll.y + b.ur.y ), 0.5 * ( b.ll.z + b.ur.z ) );
	radius = 0.5 * Vector3Ops::Magnitude( Vector3Ops::mkVector3( b.ur, b.ll ) );
}

BoundingBox SimulatedPointsGeometry::GenerateBoundingBox() const
{
	if( m_bvh && !m_radius.empty() ) return m_bvh->GetBBox();
	return BoundingBox( Point3( 0, 0, 0 ), Point3( 0, 0, 0 ) );
}

void SimulatedPointsGeometry::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	if( m_radius.empty() ) {
		if( point ) *point = Point3( 0, 0, 0 );
		if( normal ) *normal = Vector3( 0, 0, 1 );
		if( coord ) *coord = Point2( 0, 0 );
		return;
	}
	// Not area-weighted (points are not area lights); a point by prand.z.
	const std::size_t i = std::min( m_radius.size() - 1, static_cast<std::size_t>( prand.z * Scalar( m_radius.size() ) ) );
	const Point3 p = GeometricUtilities::PointOnSphere( Point3( 0, 0, 0 ), m_radius[i], Point2( prand.x, prand.y ) );
	const Vector3 n = Vector3Ops::Normalize( Vector3Ops::mkVector3( p, Point3( 0, 0, 0 ) ) );
	if( point ) *point = Point3( p.x + m_centre[i].x, p.y + m_centre[i].y, p.z + m_centre[i].z );
	if( normal ) *normal = n;
	if( coord ) GeometricUtilities::SphereTextureCoord( Vector3( 0.0, 1.0, 0.0 ), Vector3( -1.0, 0.0, 0.0 ), n, *coord );
}
