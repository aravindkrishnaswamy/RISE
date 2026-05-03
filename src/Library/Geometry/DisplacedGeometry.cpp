//////////////////////////////////////////////////////////////////////
//
//  DisplacedGeometry.cpp - Implementation of DisplacedGeometry.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-18
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "DisplacedGeometry.h"
#include "TriangleMeshGeometryIndexed.h"
#include "GeometryUtilities.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Observable.h"

using namespace RISE;
using namespace RISE::Implementation;

DisplacedGeometry::DisplacedGeometry(
	IGeometry*          pBase,
	const unsigned int  detail,
	const IFunction2D*  displacement,
	const Scalar        disp_scale,
	const bool          bDoubleSided,
	const bool          bUseFaceNormals
	) :
  m_pBase( pBase ),
  m_pDisplacement( displacement ),
  m_dispScale( disp_scale ),
  m_detail( detail ),
  m_bDoubleSided( bDoubleSided ),
  m_bUseFaceNormals( bUseFaceNormals ),
  m_pMesh( 0 ),
  m_displacementSubscription()
{
	if( m_pBase ) {
		m_pBase->addref();
	}
	if( m_pDisplacement ) {
		m_pDisplacement->addref();
	}

	if( !m_pBase ) {
		GlobalLog()->Print( eLog_Error, "DisplacedGeometry: base geometry is null" );
		return;
	}

	BuildMesh();

	// If the displacement painter exposes the Observable mixin (i.e. it derives
	// from Painter — true for every in-tree painter), subscribe so we rebuild
	// the mesh whenever the painter's keyframable state changes.  Out-of-tree
	// IFunction2D implementations that don't derive from Observable simply
	// don't subscribe; we keep today's bake-once behaviour for them.
	if( m_pDisplacement ) {
		const Observable* obs = dynamic_cast<const Observable*>( m_pDisplacement );
		if( obs ) {
			m_displacementSubscription = Subscription( obs, [this]{
				// Tier 1 §3 animation refit: instead of destroying and
				// rebuilding the mesh from scratch, re-run tessellate +
				// displacement and feed the new vertices to the existing
				// mesh's UpdateVertices().  Topology (triangle indices,
				// vertex count) is stable across painter notifications
				// because m_detail is a constructor argument — only
				// vertex positions and normals change.  See
				// docs/BVH_ACCELERATION_PLAN.md §4.6 for the full design.
				RefreshMeshVertices();
			} );
		}
	}
}

DisplacedGeometry::~DisplacedGeometry()
{
	// The destructor BODY runs before any member destructor.  Detach from the
	// painter's observer list first, while m_pDisplacement is still alive.
	// Otherwise, m_pDisplacement->release() below could delete the painter,
	// and the subscription's own destructor (which runs after this body) would
	// then call Detach() on freed memory.  Move-assigning an empty Subscription
	// triggers the current subscription's Detach via the assignment operator.
	m_displacementSubscription = Subscription();

	DestroyMesh();
	if( m_pDisplacement ) {
		m_pDisplacement->release();
		m_pDisplacement = 0;
	}
	if( m_pBase ) {
		m_pBase->release();
		m_pBase = 0;
	}
}

void DisplacedGeometry::BuildMesh()
{
	if( !m_pBase ) {
		return;
	}

	IndexTriangleListType tris;
	VerticesListType      vertices;
	NormalsListType       normals;
	TexCoordsListType     coords;

	if( !m_pBase->TessellateToMesh( tris, vertices, normals, coords, m_detail ) ) {
		GlobalLog()->Print( eLog_Error, "DisplacedGeometry: base geometry does not support tessellation (e.g. InfinitePlaneGeometry)" );
		return;
	}

	// Did we actually move any vertex positions?  With disp_scale==0 (or a null
	// displacement painter) ApplyDisplacementMapToObject is a no-op and the
	// analytical per-vertex normals coming out of TessellateToMesh are still
	// correct.  Topology-averaged normals would REPLACE those analytic normals
	// with a locally-linear approximation — fine for displaced surfaces, but
	// unnecessarily lossy when nothing was displaced.  This matters a lot for
	// SMS / Manifold-Solver tests that use disp_scale=0 as a "force tessellation
	// of an otherwise analytic shape" idiom: with this shortcut the tessellated
	// sphere's |∂N/∂u|/|∂P/∂u| ratio lands on 1/R (exactly the analytic value)
	// everywhere except the pole-cap degenerate triangles.
	const bool bVerticesDisplaced = ( m_pDisplacement && m_dispScale != 0.0 );
	if( bVerticesDisplaced ) {
		// RemapTextureCoords is a tent-fold (u → 1−2u on [0,0.5]; u → 2u−1 on
		// [0.5,1]) used ONLY to keep the displacement value consistent across
		// the u=0 / u=1 wrap seam of closed parametric surfaces (sphere,
		// torus, cylinder).  It is destructive to the linear (u, v)
		// parameterisation that the SMS / Manifold-Solver UV-Jacobian path
		// relies on: every triangle straddling u=0.5 or v=0.5 ends up with
		// a non-monotonic UV triple (tent vertex in the middle), degenerating
		// the 2×2 Jacobian.  Keep the original coords for the mesh and feed
		// a tent-remapped COPY into the displacement evaluator only.
		TexCoordsListType displacementCoords = coords;
		RemapTextureCoords( displacementCoords );
		ApplyDisplacementMapToObject( tris, vertices, normals, displacementCoords, *m_pDisplacement, m_dispScale );
	}

	if( !m_bUseFaceNormals && bVerticesDisplaced ) {
		RecomputeVertexNormalsFromTopology( tris, vertices, normals );
	}

	m_pMesh = new TriangleMeshGeometryIndexed( m_bDoubleSided, m_bUseFaceNormals );
	GlobalLog()->PrintNew( m_pMesh, __FILE__, __LINE__, "displaced geometry internal mesh" );

	m_pMesh->BeginIndexedTriangles();
	m_pMesh->AddVertices( vertices );
	m_pMesh->AddNormals( normals );
	m_pMesh->AddTexCoords( coords );
	m_pMesh->AddIndexedTriangles( tris );
	m_pMesh->DoneIndexedTriangles();
}

void DisplacedGeometry::RefreshMeshVertices()
{
	// Tier 1 §3: refit-not-rebuild observer path.  See header.
	if( !m_pBase || !m_pMesh ) {
		// No current mesh — fall back to full build.
		DestroyMesh();
		BuildMesh();
		return;
	}

	IndexTriangleListType tris;
	VerticesListType      vertices;
	NormalsListType       normals;
	TexCoordsListType     coords;

	if( !m_pBase->TessellateToMesh( tris, vertices, normals, coords, m_detail ) ) {
		GlobalLog()->Print( eLog_Error, "DisplacedGeometry::RefreshMeshVertices: base tessellation failed" );
		return;
	}

	const bool bVerticesDisplaced = ( m_pDisplacement && m_dispScale != 0.0 );
	if( bVerticesDisplaced ) {
		TexCoordsListType displacementCoords = coords;
		RemapTextureCoords( displacementCoords );
		ApplyDisplacementMapToObject( tris, vertices, normals, displacementCoords, *m_pDisplacement, m_dispScale );
	}

	if( !m_bUseFaceNormals && bVerticesDisplaced ) {
		RecomputeVertexNormalsFromTopology( tris, vertices, normals );
	}

	// Topology assumption: tri count and vertex count match what
	// the existing m_pMesh has.  m_detail is a const constructor arg,
	// so this holds across every painter notification.  If a future
	// change makes detail keyframable, the mismatch path inside
	// UpdateVertices logs an error and we fall back to full rebuild.
	const unsigned int refitMs = m_pMesh->UpdateVertices( vertices, normals );
	if( refitMs == 0 ) {
		// Mismatch (vertex count differs) — fall back to full rebuild.
		GlobalLog()->PrintEasyWarning(
			"DisplacedGeometry::RefreshMeshVertices: UpdateVertices failed; rebuilding from scratch" );
		DestroyMesh();
		BuildMesh();
	}
}

void DisplacedGeometry::DestroyMesh()
{
	safe_release( m_pMesh );
	m_pMesh = 0;
}

bool DisplacedGeometry::TessellateToMesh(
	IndexTriangleListType& tris,
	VerticesListType&      vertices,
	NormalsListType&       normals,
	TexCoordsListType&     coords,
	const unsigned int     /*detail*/ ) const
{
	// Re-emit the internal mesh so a DisplacedGeometry can itself be the base of another
	// DisplacedGeometry.  The detail parameter is ignored — the internal mesh was built at
	// this DisplacedGeometry's own construction-time detail.
	if( !m_pMesh ) {
		return false;
	}

	// Delegate via the internal TriangleMeshGeometryIndexed's own TessellateToMesh, which
	// is a pass-through of its stored arrays.
	return m_pMesh->TessellateToMesh( tris, vertices, normals, coords, 0 );
}

void DisplacedGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	if( !m_pMesh ) {
		ri.bHit = false;
		return;
	}
	m_pMesh->IntersectRay( ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo );
}

bool DisplacedGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	if( !m_pMesh ) {
		return false;
	}
	return m_pMesh->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces );
}

void DisplacedGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	if( !m_pMesh ) {
		ptCenter = Point3( 0.0, 0.0, 0.0 );
		radius   = 0.0;
		return;
	}
	m_pMesh->GenerateBoundingSphere( ptCenter, radius );
}

BoundingBox DisplacedGeometry::GenerateBoundingBox() const
{
	if( !m_pMesh ) {
		return BoundingBox( Point3( 0.0, 0.0, 0.0 ), Point3( 0.0, 0.0, 0.0 ) );
	}
	return m_pMesh->GenerateBoundingBox();
}

void DisplacedGeometry::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	if( !m_pMesh ) {
		if( point )  *point  = Point3( 0.0, 0.0, 0.0 );
		if( normal ) *normal = Vector3( 0.0, 0.0, 0.0 );
		if( coord )  *coord  = Point2( 0.0, 0.0 );
		return;
	}
	m_pMesh->UniformRandomPoint( point, normal, coord, prand );
}

Scalar DisplacedGeometry::GetArea() const
{
	if( !m_pMesh ) {
		return 0.0;
	}
	return m_pMesh->GetArea();
}

SurfaceDerivatives DisplacedGeometry::ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const
{
	if( !m_pMesh ) {
		return SurfaceDerivatives();
	}
	return m_pMesh->ComputeSurfaceDerivatives( objSpacePoint, objSpaceNormal );
}

#include "../Interfaces/IFunction2D.h"

namespace {
	// Tent-fold of [0, 1] -> [0, 1] centred on 0.5; matches the
	// destination-side mapping in GeometryUtilities::RemapTextureCoords
	// that the tessellator uses when evaluating the displacement painter
	// at mesh-build time.  Equivalent to |2u - 1| with the same
	// exact-on-0.5 handling.  Keeping the analytical query consistent
	// with on-mesh values requires the same tent-fold here.
	inline RISE::Scalar TentFold( RISE::Scalar u )
	{
		if( u > 0.5 ) return ( u - 0.5 ) * 2.0;
		if( u < 0.5 ) return 1.0 - ( u * 2.0 );
		return 0.0;
	}
}

bool DisplacedGeometry::ComputeAnalyticalDerivatives(
	const Point2& uv,
	Scalar        smoothing,
	Point3&       outPosition,
	Vector3&      outNormal,
	Vector3&      outDpdu,
	Vector3&      outDpdv,
	Vector3&      outDndu,
	Vector3&      outDndv
	) const
{
	if( !m_pBase ) return false;

	// Smoothing scales the displacement amplitude.  At s = 1 the
	// effective scale is 0 and the surface collapses to the base; at
	// s = 0 we get the full displaced mesh.  Clamped to [0, 1].
	const Scalar sClamped = (smoothing < 0.0) ? 0.0 :
	                        (smoothing > 1.0) ? 1.0 : smoothing;
	const Scalar effDispScale = m_dispScale * (1.0 - sClamped);

	// Pass `smoothing` through to the base — irrelevant for analytical
	// primitives (sphere / ellipsoid) but matters for nested displaced
	// geometry where the base's own displacement should also smooth.
	auto evalDisplacedSurface = [&](
		const Point2& at,
		Point3&  P_d,
		Vector3& N_d,
		Vector3& dpdu_d,
		Vector3& dpdv_d ) -> bool
	{
		Point3  P_b;
		Vector3 N_b, dpdu_b, dpdv_b, dndu_b, dndv_b;
		if( !m_pBase->ComputeAnalyticalDerivatives(
				at, sClamped, P_b, N_b, dpdu_b, dpdv_b, dndu_b, dndv_b ) )
		{
			return false;
		}

		Scalar f = 0.0, dfdu = 0.0, dfdv = 0.0;
		if( m_pDisplacement && effDispScale != 0.0 )
		{
			// Painter is evaluated at TENT-FOLDED (u, v) — matches
			// GeometryUtilities::ApplyDisplacementMapToObject which
			// receives the tent-folded coords (see DisplacedGeometry::
			// BuildMesh).  Using raw (u, v) here would describe a
			// different displacement than the mesh actually has.
			const Scalar tu = TentFold( at.x );
			const Scalar tv = TentFold( at.y );
			f = m_pDisplacement->Evaluate( tu, tv );

			// FD df/du, df/dv on the painter.  Step is in raw-(u, v)
			// space; the |2| chain-rule factor for the tent fold cancels
			// (same |2| on both ± probes within either side of 0.5).
			// At u = 0.5 exactly there's a sign-flip — acceptable;
			// SMS chains rarely site on the seam.
			const Scalar epsP = 1.0e-3;
			const Scalar f_uplus  = m_pDisplacement->Evaluate( TentFold( at.x + epsP ), tv );
			const Scalar f_uminus = m_pDisplacement->Evaluate( TentFold( at.x - epsP ), tv );
			const Scalar f_vplus  = m_pDisplacement->Evaluate( tu, TentFold( at.y + epsP ) );
			const Scalar f_vminus = m_pDisplacement->Evaluate( tu, TentFold( at.y - epsP ) );
			const Scalar inv2 = 1.0 / (2.0 * epsP);
			dfdu = ( f_uplus - f_uminus ) * inv2;
			dfdv = ( f_vplus - f_vminus ) * inv2;
		}

		// Displaced position: P_d = P_b + s_eff·f·N_b
		P_d = Point3Ops::mkPoint3( P_b, N_b * (effDispScale * f) );

		// Chain rule for displaced tangents:
		//   dP_d/du = dP_b/du + s_eff·(df/du · N_b + f · dN_b/du)
		dpdu_d = Vector3(
			dpdu_b.x + effDispScale * ( dfdu * N_b.x + f * dndu_b.x ),
			dpdu_b.y + effDispScale * ( dfdu * N_b.y + f * dndu_b.y ),
			dpdu_b.z + effDispScale * ( dfdu * N_b.z + f * dndu_b.z ) );
		dpdv_d = Vector3(
			dpdv_b.x + effDispScale * ( dfdv * N_b.x + f * dndv_b.x ),
			dpdv_b.y + effDispScale * ( dfdv * N_b.y + f * dndv_b.y ),
			dpdv_b.z + effDispScale * ( dfdv * N_b.z + f * dndv_b.z ) );

		// Displaced unit normal: cross product of tangents, with handedness
		// reconciled against the base normal.  The (theta, phi) parameter-
		// isation used by EllipsoidGeometry / SphereGeometry produces a
		// LEFT-handed (dpdu, dpdv) frame whose cross points INWARD; the
		// base normal (from the implicit gradient) points OUTWARD.  Without
		// the flip the SMS constraint's tangent-plane projection mirrors
		// and Newton can't converge.  Fall back to the base normal at
		// degenerate poles.
		const Vector3 cross_d = Vector3Ops::Cross( dpdu_d, dpdv_d );
		const Scalar  cMag    = Vector3Ops::Magnitude( cross_d );
		if( cMag > NEARZERO ) {
			N_d = cross_d * (1.0 / cMag);
			if( Vector3Ops::Dot( N_d, N_b ) < 0.0 ) {
				N_d = Vector3( -N_d.x, -N_d.y, -N_d.z );
			}
		} else {
			N_d = N_b;
		}
		return true;
	};

	// Centre evaluation
	if( !evalDisplacedSurface( uv, outPosition, outNormal, outDpdu, outDpdv ) ) {
		return false;
	}

	// dN_d/du, dN_d/dv via central FD on the displaced unit normal.
	// Each probe re-runs evalDisplacedSurface (one base ComputeAnalytical
	// call + five painter evals).  Total cost per query is therefore
	// ~5 base + ~25 painter calls.  At smoothing = 1 the displacement
	// branch is skipped (effDispScale == 0) and we collapse to base
	// derivatives — much cheaper.
	const Scalar epsUv = 1.0e-3;
	Point3  P_dummy;
	Vector3 dpdu_dummy, dpdv_dummy;
	Vector3 N_uplus, N_uminus, N_vplus, N_vminus;
	if( !evalDisplacedSurface( Point2( uv.x + epsUv, uv.y ),
			P_dummy, N_uplus,  dpdu_dummy, dpdv_dummy ) ) return false;
	if( !evalDisplacedSurface( Point2( uv.x - epsUv, uv.y ),
			P_dummy, N_uminus, dpdu_dummy, dpdv_dummy ) ) return false;
	if( !evalDisplacedSurface( Point2( uv.x, uv.y + epsUv ),
			P_dummy, N_vplus,  dpdu_dummy, dpdv_dummy ) ) return false;
	if( !evalDisplacedSurface( Point2( uv.x, uv.y - epsUv ),
			P_dummy, N_vminus, dpdu_dummy, dpdv_dummy ) ) return false;
	const Scalar inv2eps = 1.0 / (2.0 * epsUv);
	outDndu = ( N_uplus  - N_uminus ) * inv2eps;
	outDndv = ( N_vplus  - N_vminus ) * inv2eps;
	return true;
}
