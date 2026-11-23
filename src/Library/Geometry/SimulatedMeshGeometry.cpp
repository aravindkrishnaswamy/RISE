//////////////////////////////////////////////////////////////////////
//
//  SimulatedMeshGeometry.cpp - Simulation M3b, see SimulatedMeshGeometry.h
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SimulatedMeshGeometry.h"
#include "../Animation/Animator.h"
#include "../Animation/TimeIndexedView.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Utilities/SurfaceCurvature.h"
#include "../Interfaces/ILog.h"
#include "../Interfaces/IScene.h"
#include "../Interfaces/IObjectManager.h"
#include <algorithm>
#include <cmath>
#include <limits>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	inline Point3 Mix( const Point3& a, const Point3& b, const Scalar g, const Scalar f )
	{
		// (1-f) a + f b: exactly `a` at f = 0 and exactly `b` at f = 1.
		return Point3( a.x * g + b.x * f, a.y * g + b.y * f, a.z * g + b.z * f );
	}

	inline Vector3 Mix( const Vector3& a, const Vector3& b, const Scalar g, const Scalar f )
	{
		return Vector3( a.x * g + b.x * f, a.y * g + b.y * f, a.z * g + b.z * f );
	}

	void ToPoints( const std::vector<double>& xyz, VerticesListType& out )
	{
		out.resize( xyz.size() / 3 );
		for( std::size_t i = 0; i < out.size(); ++i ) out[i] = Point3( xyz[3*i], xyz[3*i+1], xyz[3*i+2] );
	}

	void ToVectors( const std::vector<double>& xyz, NormalsListType& out )
	{
		out.resize( xyz.size() / 3 );
		for( std::size_t i = 0; i < out.size(); ++i ) out[i] = Vector3( xyz[3*i], xyz[3*i+1], xyz[3*i+2] );
	}
}

SimulatedMeshGeometry* SimulatedMeshGeometry::Create( const std::string& cacheRoot,
	const Options& options, std::string& error )
{
	std::unique_ptr<Simulation::MeshCacheSource> source;
	if( !Simulation::MeshCacheSource::Open( cacheRoot, options.source, source, error ) ) return nullptr;
	SimulatedMeshGeometry* g = new SimulatedMeshGeometry( options, std::move( source ) );
	const Simulation::MeshCacheSource& s = *g->m_source;

	VerticesListType positions;
	ToPoints( s.BindPositions(), positions );
	g->BeginIndexedTriangles();
	g->AddVertices( positions );
	if( !options.faceNormals ) {
		NormalsListType normals;
		if( s.PayloadHasNormals() ) ToVectors( s.BindNormals(), normals );
		else g->ComputeNormals( positions, normals );
		g->AddNormals( normals );
	}
	// mesh.v2 UV set (fixed with the topology) as per-vertex texture coordinates.
	const bool uvs = s.HasUVs();
	if( uvs ) {
		TexCoordsListType coords( s.VertexCount() );
		for( std::size_t i = 0; i < coords.size(); ++i ) coords[i] = Point2( s.BindUVs()[2*i], s.BindUVs()[2*i+1] );
		g->AddTexCoords( coords );
	}
	const std::vector<std::uint32_t>& indices = s.Indices();
	for( std::size_t t = 0; t + 2 < indices.size(); t += 3 ) {
		IndexedTriangle tri;
		for( int k = 0; k < 3; ++k ) {
			tri.iVertices[k] = indices[t + k];
			tri.iNormals[k] = indices[t + k];
			tri.iCoords[k] = uvs ? indices[t + k] : 0;
		}
		g->AddIndexedTriangle( tri );
	}
	g->DoneIndexedTriangles();
	// Uncertified from the first frame on, as every later (lean) install
	// leaves it, so the mesh does not change technique between frames.
	g->ClearSurfaceCertificates();
	g->m_installedFrame = s.BindIndex();
	g->m_identity = "bind|" + std::to_string( s.BindIndex() );
	return g;
}

SimulatedMeshGeometry::SimulatedMeshGeometry( const Options& options,
	std::unique_ptr<Simulation::MeshCacheSource> source ) :
  TriangleMeshGeometryIndexed( options.doubleSided, options.faceNormals ),
  m_options( options ),
  m_source( std::move( source ) ),
  m_clock( nullptr ),
  m_blur( false ),
  m_hidden( false ),
  m_open( 0 ), m_close( 0 ), m_nominal( 0 ), m_invSpan( 0 ),
  m_installedFrame( 0 ),
  m_installs( 0 ),
  m_lastWarnedMissing( std::numeric_limits<std::int64_t>::min() ),
  m_leases( 0 ),
  m_generation( 0 ), m_frozenGeneration( 0 )
{
}

SimulatedMeshGeometry::~SimulatedMeshGeometry()
{
	if( m_clock ) m_clock->release();
}

void SimulatedMeshGeometry::SetSampleClock( const Animator* clock )
{
	if( clock == m_clock ) return;
	if( clock ) clock->addref();
	if( m_clock ) m_clock->release();
	m_clock = clock;
}

bool RISE::Implementation::SceneBindsSimulatedMesh( const IScene& scene )
{
	const IObjectManager* objects = scene.GetObjects();
	if( !objects ) return false;
	struct Finder : public IEnumCallback<IObject>
	{
		bool found = false;
		bool operator()( const IObject& object ) override
		{
			found = dynamic_cast<const SimulatedMeshGeometry*>( object.GetGeometry() ) != nullptr;
			return !found;
		}
	} finder;
	objects->EnumerateObjects( finder );
	return finder.found;
}

void SimulatedMeshGeometry::ComputeNormals( const VerticesListType& p, NormalsListType& out ) const
{
	// Area-weighted vertex normals over the bound topology.
	out.assign( p.size(), Vector3( 0, 0, 0 ) );
	const std::vector<std::uint32_t>& indices = m_source->Indices();
	for( std::size_t t = 0; t + 2 < indices.size(); t += 3 ) {
		const std::uint32_t a = indices[t], b = indices[t+1], c = indices[t+2];
		const Vector3 n = Vector3Ops::Cross( Vector3Ops::mkVector3( p[b], p[a] ), Vector3Ops::mkVector3( p[c], p[a] ) );
		out[a] = out[a] + n; out[b] = out[b] + n; out[c] = out[c] + n;
	}
	for( Vector3& n : out ) {
		const Scalar m = Vector3Ops::Magnitude( n );
		n = m > Scalar( 0 ) ? n * ( Scalar( 1 ) / m ) : Vector3( 0, 0, 1 );
	}
}

SimulatedMeshGeometry::State SimulatedMeshGeometry::Capture() const
{
	State s;
	s.positions = pPoints;
	s.normals = pNormals;
	s.keyOpen = m_keyOpen; s.keyClose = m_keyClose;
	s.normalOpen = m_normalOpen; s.normalClose = m_normalClose;
	s.blur = m_blur; s.hidden = m_hidden;
	s.open = m_open; s.close = m_close; s.nominal = m_nominal;
	s.frame = m_installedFrame;
	s.identity = m_identity;
	return s;
}

bool SimulatedMeshGeometry::Install( State& next, std::string& error )
{
	// The blur state goes in FIRST: the refit below reads the element boxes
	// and the float-filter hook, which both depend on it.
	m_keyOpen.swap( next.keyOpen ); m_keyClose.swap( next.keyClose );
	m_normalOpen.swap( next.normalOpen ); m_normalClose.swap( next.normalClose );
	m_blur = next.blur;
	m_hidden = next.hidden;
	m_open = next.open; m_close = next.close; m_nominal = next.nominal;
	m_invSpan = next.close > next.open ? 1.0 / ( next.close - next.open ) : 0.0;
	m_installedFrame = next.frame;
	m_identity = next.identity;
	if( !UpdateVerticesLean( next.positions, next.normals, true ) ) {
		error = "simulated mesh install failed (vertex/normal count mismatch)";
		return false;
	}
	return true;
}

bool SimulatedMeshGeometry::PrepareAssetForRender( const RenderTimeSupport& support,
	std::string& error, std::function<void()>* rollback )
{
	// Simulation M3c fast path: the viewport prepares every progressive pass.
	// A frame installed for exactly this support from exactly this immutable
	// generation (a sealed disk cache, or one live publication) is already in
	// place, so the pass skips the payload read and decode.
	{
		std::lock_guard<std::mutex> lock( m_freezeMutex );
		if( m_leases ) { error = "mutation_frozen"; return false; }
	}
	std::shared_ptr<const Simulation::IReadOnlyCacheSnapshot> cheap = m_source->CheapSnapshot();
	if( cheap && cheap == m_preparedSnapshot && support.nominal == m_preparedSupport.nominal &&
		support.open == m_preparedSupport.open && support.close == m_preparedSupport.close ) return true;
	m_preparedSnapshot.reset();
	if( !PrepareFrame( support, error, rollback ) ) return false;
	if( rollback ) {
		// Whatever the rollback restores, the prepared-frame memo no longer holds.
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

bool SimulatedMeshGeometry::RebindSource( std::unique_ptr<Simulation::MeshCacheSource>& source, std::string& error )
{
	{
		std::lock_guard<std::mutex> lock( m_freezeMutex );
		if( m_leases ) { error = "mutation_frozen"; return false; }
	}
	if( !source ) { error = "simulated mesh rebind needs a source"; return false; }
	if( source->VertexCount() != m_source->VertexCount() || source->Indices() != m_source->Indices() ) {
		error = "simulated mesh rebind refused: the new source's topology differs from the bound mesh "
			"(changing topology needs a reload; M7)";
		return false;
	}
	m_source.swap( source );
	// The next prepare installs from the new source whatever its identity.
	m_preparedSnapshot.reset();
	m_identity = "rebind|" + std::to_string( m_installedFrame );
	m_lastWarnedMissing = std::numeric_limits<std::int64_t>::min();
	++m_generation;
	return true;
}

bool SimulatedMeshGeometry::PrepareFrame( const RenderTimeSupport& support,
	std::string& error, std::function<void()>* rollback )
{
	{
		std::lock_guard<std::mutex> lock( m_freezeMutex );
		if( m_leases ) { error = "mutation_frozen"; return false; }
	}
	if( !std::isfinite( support.nominal ) || !std::isfinite( support.open ) ||
		!std::isfinite( support.close ) || support.open > support.close ) {
		error = "simulated mesh render time support is invalid";
		return false;
	}
	Simulation::MeshFrameSample sample;
	if( !m_source->Sample( support.nominal, support.open, support.close, m_options.motionBlur, sample, error ) )
		return false;	// nothing installed: the previous frame stays

	if( sample.state == Simulation::MeshFrameSample::State::Missing ) {
		if( m_options.source.predictive ) {
			error = "simulated mesh frame " + std::to_string( sample.index ) +
				" is unavailable; a predictive binding fails closed";
			return false;
		}
		const bool hide = m_options.source.missing == Simulation::MeshMissingPolicy::Hide;
		if( m_lastWarnedMissing != sample.index ) {
			m_lastWarnedMissing = sample.index;
			GlobalLog()->PrintEx( eLog_Warning,
				"SimulatedMeshGeometry:: cache frame %lld is not baked yet (partial cache); %s",
				(long long)sample.index, hide ? "hiding the surface" : "holding the previously installed frame" );
		}
		if( !hide ) {
			// Hold the previous frame's nominal pose, without its shutter keys:
			// they belong to the previous frame's shutter.
			if( !m_blur ) return true;
			State held = Capture();
			held.keyOpen.clear(); held.keyClose.clear();
			held.normalOpen.clear(); held.normalClose.clear();
			held.blur = false;
			held.open = support.open; held.close = support.close; held.nominal = support.nominal;
			State previous = Capture();
			if( !Install( held, error ) ) {
				std::string ignored;
				Install( previous, ignored );
				return false;
			}
			++m_generation;
			if( rollback ) {
				auto saved = std::make_shared<State>( std::move( previous ) );
				*rollback = [this, saved]() {
					std::string ignored;
					Install( *saved, ignored );
					++m_generation;
				};
			}
			return true;
		}
		if( m_hidden ) return true;
		m_hidden = true;
		++m_generation;
		if( rollback ) *rollback = [this]() { m_hidden = false; ++m_generation; };
		return true;
	}
	if( sample.identity == m_identity && !m_hidden &&
		m_open == support.open && m_close == support.close && m_nominal == support.nominal ) return true;

	State next;
	ToPoints( sample.nominal, next.positions );
	if( !m_options.faceNormals ) {
		if( !sample.normals.empty() ) ToVectors( sample.normals, next.normals );
		else ComputeNormals( next.positions, next.normals );
	}
	if( !sample.open.empty() ) {
		ToPoints( sample.open, next.keyOpen );
		ToPoints( sample.close, next.keyClose );
		bool moves = false;
		for( std::size_t i = 0; i < next.keyOpen.size() && !moves; ++i ) {
			const Point3& a = next.keyOpen[i];
			const Point3& b = next.keyClose[i];
			moves = a.x != b.x || a.y != b.y || a.z != b.z;
		}
		next.blur = moves;
		if( !moves ) {
			next.keyOpen.clear(); next.keyClose.clear();
		} else if( !m_options.faceNormals ) {
			// Key normals: the frame's normal turned by the change of the
			// surface between the frame and the key (exactly the recomputed
			// normal when the payload has none).
			NormalsListType atFrame, atOpen, atClose;
			ComputeNormals( next.positions, atFrame );
			ComputeNormals( next.keyOpen, atOpen );
			ComputeNormals( next.keyClose, atClose );
			next.normalOpen.resize( atOpen.size() );
			next.normalClose.resize( atClose.size() );
			for( std::size_t i = 0; i < atOpen.size(); ++i ) {
				if( sample.normals.empty() ) {
					next.normalOpen[i] = atOpen[i];
					next.normalClose[i] = atClose[i];
				} else {
					next.normalOpen[i] = Vector3Ops::Normalize( next.normals[i] + ( atOpen[i] - atFrame[i] ) );
					next.normalClose[i] = Vector3Ops::Normalize( next.normals[i] + ( atClose[i] - atFrame[i] ) );
				}
			}
		}
	}
	next.open = support.open; next.close = support.close; next.nominal = support.nominal;
	next.frame = sample.index;
	next.identity = sample.identity;

	State previous = Capture();
	if( !Install( next, error ) ) {
		std::string ignored;
		Install( previous, ignored );
		return false;
	}
	++m_generation;
	++m_installs;
	if( rollback ) {
		auto saved = std::make_shared<State>( std::move( previous ) );
		*rollback = [this, saved]() {
			std::string ignored;
			Install( *saved, ignored );
			++m_generation;
		};
	}
	return true;
}

bool SimulatedMeshGeometry::AcquireAssetFreeze( std::string& )
{
	std::lock_guard<std::mutex> lock( m_freezeMutex );
	if( m_leases++ == 0 ) m_frozenGeneration = m_generation;
	return true;
}

bool SimulatedMeshGeometry::ReleaseAssetFreeze( std::string& error )
{
	std::lock_guard<std::mutex> lock( m_freezeMutex );
	if( !m_leases ) { error = "simulated mesh freeze released without an acquire"; return false; }
	--m_leases;
	if( m_generation != m_frozenGeneration ) { error = "simulated mesh changed during render"; return false; }
	return true;
}

Scalar SimulatedMeshGeometry::ShutterFraction() const
{
	double t = m_nominal;
	const TimeIndexed::SampleTime& sample = TimeIndexed::tlsSampleTime;
	if( sample.valid ) {
		t = sample.time;
	} else if( m_clock ) {
		const double shared = m_clock->LastEvaluatedTime();
		if( std::isfinite( shared ) ) t = shared;
	}
	const double f = ( t - m_open ) * m_invSpan;
	return f > 0.0 ? ( f < 1.0 ? Scalar( f ) : Scalar( 1 ) ) : Scalar( 0 );
}

void SimulatedMeshGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	if( m_hidden ) return;
	TriangleMeshGeometryIndexed::IntersectRay( ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo );
}

bool SimulatedMeshGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	if( m_hidden ) return false;
	return TriangleMeshGeometryIndexed::IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces );
}

BoundingBox SimulatedMeshGeometry::GetElementBoundingBox( const MYOBJ elem ) const
{
	if( !m_blur ) return TriangleMeshGeometryIndexed::GetElementBoundingBox( elem );
	// Both keys, and the nominal pose the non-blurred queries use: linear
	// interpolation between the keys never leaves this box.
	BoundingBox box( Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ), Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY ) );
	const Point3* base = pPoints.data();
	for( int k = 0; k < 3; ++k ) {
		const std::size_t i = static_cast<std::size_t>( elem->pVertices[k] - base );
		box.Include( *elem->pVertices[k] );
		box.Include( m_keyOpen[i] );
		box.Include( m_keyClose[i] );
	}
	return box;
}

bool SimulatedMeshGeometry::GetFloatTriangleVertices( const MYOBJ elem, float v0[3], float v1[3], float v2[3] ) const
{
	// The float filter precomputes ONE pose's edges; under blur every pose
	// in the shutter must reach the interpolating test, so it is off.
	if( m_blur ) return false;
	return TriangleMeshGeometryIndexed::GetFloatTriangleVertices( elem, v0, v1, v2 );
}

void SimulatedMeshGeometry::RayElementIntersection( RayIntersection& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool /*bComputeExitInfo*/ ) const
{
	RayElementIntersection( ri.geometric, elem, bHitFrontFaces, bHitBackFaces );
}

void SimulatedMeshGeometry::RayElementIntersection( RayIntersectionGeometric& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	if( !m_blur ) {
		TriangleMeshGeometryIndexed::RayElementIntersection( ri, elem, bHitFrontFaces, bHitBackFaces );
		return;
	}
	// The interpolating twin of TriangleMeshGeometryIndexed's element test
	// (TriangleMeshGeometryIndexedSpecializations.h).  A simulated mesh has
	// no colours, tangents or shader directions, and texture coordinates only
	// from a mesh.v2 UV set, so the hit record is the indexed mesh's for that
	// case: with UVs the interpolated coordinate and the UV-Jacobian
	// derivatives (texture chart valid), else UV (0, 0) and the barycentric
	// edge-frame derivatives.  No mailbox: the BVH references every triangle
	// from exactly one leaf.
	const Point3* base = pPoints.data();
	const std::size_t i0 = static_cast<std::size_t>( elem->pVertices[0] - base );
	const std::size_t i1 = static_cast<std::size_t>( elem->pVertices[1] - base );
	const std::size_t i2 = static_cast<std::size_t>( elem->pVertices[2] - base );
	const Scalar f = ShutterFraction();
	const Scalar g = Scalar( 1 ) - f;
	const Point3 p0 = Mix( m_keyOpen[i0], m_keyClose[i0], g, f );
	const Point3 p1 = Mix( m_keyOpen[i1], m_keyClose[i1], g, f );
	const Point3 p2 = Mix( m_keyOpen[i2], m_keyClose[i2], g, f );

	const Vector3 vEdgeA = Vector3Ops::mkVector3( p1, p0 );
	const Vector3 vEdgeB = Vector3Ops::mkVector3( p2, p0 );
	const Vector3 vFaceNormal = Vector3Ops::Cross( vEdgeA, vEdgeB );
	if( !bHitFrontFaces && Vector3Ops::Dot( vFaceNormal, ri.ray.Dir() ) < 0 ) return;
	if( !bHitBackFaces && Vector3Ops::Dot( vFaceNormal, ri.ray.Dir() ) > 0 ) return;

	TRIANGLE_HIT h;
	h.bHit = false;
	h.dRange = RISE_INFINITY;
	RayTriangleIntersection( ri.ray, h, p0, vEdgeA, vEdgeB );
	if( !( h.bHit && h.dRange < ri.range ) ) return;

	ri.bHit = true;
	ri.range = h.dRange;
	if( ri.bWantsWireEdgeInfo ) {
		ri.ptWireNearestEdge = WireClosestPointOnTriangleEdges( ri.ray.PointAtLength( h.dRange ), p0, p1, p2 );
		ri.bHasWireEdgeInfo = true;
	}
	const Scalar a = h.alpha;
	const Scalar b = h.beta;
	const bool smooth = elem->pNormals[0] && !m_normalOpen.empty();
	Vector3 n0, n1, n2;
	if( smooth ) {
		n0 = Mix( m_normalOpen[i0], m_normalClose[i0], g, f );
		n1 = Mix( m_normalOpen[i1], m_normalClose[i1], g, f );
		n2 = Mix( m_normalOpen[i2], m_normalClose[i2], g, f );
		ri.vNormal = n0 + ( n1 - n0 ) * a + ( n2 - n0 ) * b;
	} else {
		ri.vNormal = vFaceNormal;
	}
	{
		Vector3 fn = Vector3Ops::Normalize( vFaceNormal );
		if( Vector3Ops::Dot( fn, ri.vNormal ) < 0 ) fn = Vector3( -fn.x, -fn.y, -fn.z );
		ri.vGeomNormal = fn;
	}
	const bool chart = elem->pCoords[0] && elem->pCoords[1] && elem->pCoords[2];
	ri.ptCoord = chart ? Point2Ops::mkPoint2( *elem->pCoords[0],
		Vector2Ops::mkVector2( *elem->pCoords[1], *elem->pCoords[0] ) * a +
		Vector2Ops::mkVector2( *elem->pCoords[2], *elem->pCoords[0] ) * b ) : Point2( 0, 0 );
	ri.ptCoord1 = ri.ptCoord;
	ri.bHasTexCoord1 = false;
	ri.bHasVertexColor = false;
	ri.bHasTangent = false;
	ri.bHasShaderDirection = false;
	ri.bHasNormalMapFrame = false;

	// Derivatives in the UV chart when there is one (the inverted UV
	// Jacobian of the interpolated triangle), else barycentric edge-frame
	// derivatives, projected on the shading tangent plane, exactly as the
	// indexed mesh does.
	const Vector3 shadingNormal = Vector3Ops::Normalize( ri.vNormal );
	Scalar duA = 0, duB = 0, dvA = 0, dvB = 0, uvDet = 0;
	if( chart ) {
		duA = elem->pCoords[1]->x - elem->pCoords[0]->x;
		duB = elem->pCoords[2]->x - elem->pCoords[0]->x;
		dvA = elem->pCoords[1]->y - elem->pCoords[0]->y;
		dvB = elem->pCoords[2]->y - elem->pCoords[0]->y;
		uvDet = duA * dvB - duB * dvA;
	}
	const bool jacobian = chart && fabs( uvDet ) > NEARZERO;
	const Scalar invDet = jacobian ? 1.0 / uvDet : 0.0;
	Vector3 dpdu = vEdgeA, dpdv = vEdgeB;
	if( jacobian ) {
		dpdu = ( vEdgeA * dvB - vEdgeB * dvA ) * invDet;
		dpdv = ( vEdgeB * duA - vEdgeA * duB ) * invDet;
	}
	{
		const Scalar du = Vector3Ops::Dot( dpdu, shadingNormal );
		const Scalar dv = Vector3Ops::Dot( dpdv, shadingNormal );
		dpdu = Vector3( dpdu.x - shadingNormal.x * du, dpdu.y - shadingNormal.y * du, dpdu.z - shadingNormal.z * du );
		dpdv = Vector3( dpdv.x - shadingNormal.x * dv, dpdv.y - shadingNormal.y * dv, dpdv.z - shadingNormal.z * dv );
	}
	Vector3 dndu( 0, 0, 0 ), dndv( 0, 0, 0 );
	if( smooth ) {
		const Vector3 eA = n1 - n0;
		const Vector3 eB = n2 - n0;
		const Vector3 dA = jacobian ? ( eA * dvB - eB * dvA ) * invDet : eA;
		const Vector3 dB = jacobian ? ( eB * duA - eA * duB ) * invDet : eB;
		const Scalar nrawLen = Vector3Ops::Magnitude( ri.vNormal );
		const Scalar invLen = ( nrawLen > NEARZERO ) ? 1.0 / nrawLen : 0.0;
		const Scalar du = Vector3Ops::Dot( shadingNormal, dA );
		const Scalar dv = Vector3Ops::Dot( shadingNormal, dB );
		dndu = Vector3( ( dA.x - shadingNormal.x * du ) * invLen, ( dA.y - shadingNormal.y * du ) * invLen, ( dA.z - shadingNormal.z * du ) * invLen );
		dndv = Vector3( ( dB.x - shadingNormal.x * dv ) * invLen, ( dB.y - shadingNormal.y * dv ) * invLen, ( dB.z - shadingNormal.z * dv ) * invLen );
	}
	ri.derivatives.dsdu = 1.0; ri.derivatives.dsdv = 0.0;
	ri.derivatives.dtdu = 0.0; ri.derivatives.dtdv = 1.0;
	ri.derivatives.texChartValid = jacobian;
	if( Vector3Ops::Dot( Vector3Ops::Cross( dpdu, dpdv ), shadingNormal ) < 0.0 ) {
		dpdv = Vector3( -dpdv.x, -dpdv.y, -dpdv.z );
		dndv = Vector3( -dndv.x, -dndv.y, -dndv.z );
		ri.derivatives.dtdv = -1.0;
	}
	ri.derivatives.dpdu = dpdu;
	ri.derivatives.dpdv = dpdv;
	ri.derivatives.dndu = dndu;
	ri.derivatives.dndv = dndv;
	ri.derivatives.valid = true;
	if( SurfaceCurvatureDemand::Any() ) {
		ri.derivatives.scaleHint = SurfaceCurvature::ScaleHintFromBoundingBox( GenerateBoundingBox() );
	}
	// Signal bakes are per vertex at the nominal pose; the triangle and its
	// barycentrics are pose-independent.
	StampSurfaceSignals( ri, elem, h.dRange, shadingNormal, a, b );
}

bool SimulatedMeshGeometry::RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	if( !m_blur ) {
		return TriangleMeshGeometryIndexed::RayElementIntersection_IntersectionOnly( ray, dHowFar, elem, bHitFrontFaces, bHitBackFaces );
	}
	const Point3* base = pPoints.data();
	const std::size_t i0 = static_cast<std::size_t>( elem->pVertices[0] - base );
	const std::size_t i1 = static_cast<std::size_t>( elem->pVertices[1] - base );
	const std::size_t i2 = static_cast<std::size_t>( elem->pVertices[2] - base );
	const Scalar f = ShutterFraction();
	const Scalar g = Scalar( 1 ) - f;
	const Point3 p0 = Mix( m_keyOpen[i0], m_keyClose[i0], g, f );
	const Vector3 vEdgeA = Vector3Ops::mkVector3( Mix( m_keyOpen[i1], m_keyClose[i1], g, f ), p0 );
	const Vector3 vEdgeB = Vector3Ops::mkVector3( Mix( m_keyOpen[i2], m_keyClose[i2], g, f ), p0 );
	const Vector3 vFaceNormal = Vector3Ops::Cross( vEdgeA, vEdgeB );
	if( !bHitFrontFaces && Vector3Ops::Dot( vFaceNormal, ray.Dir() ) < 0 ) return false;
	if( !bHitBackFaces && Vector3Ops::Dot( vFaceNormal, ray.Dir() ) > 0 ) return false;
	TRIANGLE_HIT h;
	RayTriangleIntersection( ray, h, p0, vEdgeA, vEdgeB );
	return h.bHit && h.dRange > NEARZERO && h.dRange < dHowFar;
}
