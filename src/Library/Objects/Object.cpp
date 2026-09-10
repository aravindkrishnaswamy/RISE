//////////////////////////////////////////////////////////////////////
//
//  Object.cpp - Implements the Object class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 2, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Object.h"
#include "SnapshotLeafClone.h"
#include "../Interfaces/ILog.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Intersection/TextureFootprintCompute.h"
#include "../Utilities/GeometricUtilities.h"
#include <atomic>		// P2a: log-once idiom for UniformRandomPoint's null-geometry fallback warning
#include <cmath>		// std::nextafter -- the Jacobi pair's four-ulp widening
#include <algorithm>	// std::min / std::max over the three singular values

using namespace RISE;
using namespace RISE::Implementation;

//////////////////////////////////////////////////////////////////////
// ComputeSigmaExtremes -- the extremal singular values of a transform's
// upper 3x3 (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §5.6, "Exact sigma").
//
// WHY ONE-SIDED JACOBI ON `M` AND NOT AN EIGEN-SOLVE ON `M^T M`.  Forming
// the Gram matrix squares the condition number: a transform spanning
// 1e-3..1e3 would leave the SMALL singular value with roughly 1e-4
// relative error, and the small one is the value the query divides its
// search radius by and (since Phase 3's signed query) multiplies its
// answer by.  Rotating the COLUMNS of `M` until they are mutually
// orthogonal never forms `M^T M` at all and delivers high RELATIVE
// accuracy for every singular value, small ones included.
//
// The rotations are applied on the right, so the column NORMS after
// convergence are the singular values.  Nothing needs the singular
// VECTORS, so the accumulating rotation matrix is simply not formed.
//
// REFLECTIONS NEED NOTHING SPECIAL: the singular values of `M` are those
// of `|M|`, and a negative determinant never enters below except through
// its absolute value in the fallback.
//////////////////////////////////////////////////////////////////////

bool RISE::Implementation::ComputeSigmaExtremes(
	const Matrix4& m, const int maxSweeps,
	Scalar& outSigmaMin, Scalar& outSigmaMax, SigmaSource& outSource )
{
	outSigmaMin = Scalar( 0 );
	outSigmaMax = Scalar( 0 );
	// The refusal below leaves this at `Loose`.  That is not a claim about
	// a degenerate transform -- there is no fourth state and none is
	// needed, because the only consumer of the state is a log line that
	// sits behind `DistanceToSurface`'s `sigmaMin > 0` gate and is
	// therefore unreachable for a refused transform.
	outSource = SigmaSource::Loose;

	// The three vectors below are the IMAGES OF THE BASIS VECTORS under
	// this transform, read straight out of the convention
	// Vector3Ops::Transform uses (`out.x = m._00*v.x + m._10*v.y +
	// m._20*v.z`, and so on): so `M * e0` is (_00, _01, _02) -- the first
	// COLUMN of the linear map, which is what a one-sided Jacobi rotates.
	// Taking them this way rather than transposing by hand is what keeps
	// this routine from silently disagreeing with the transform it is
	// describing.
	const Vector3 c0( m._00, m._01, m._02 );
	const Vector3 c1( m._10, m._11, m._12 );
	const Vector3 c2( m._20, m._21, m._22 );

	// det of the LINEAR part, from those columns.  For an affine transform
	// this equals the 4x4 determinant; computing it here keeps the sigma
	// math self-contained and correct even for a matrix whose bottom row
	// is not (0,0,0,1).
	const Scalar det3 = Vector3Ops::Dot( c0, Vector3Ops::Cross( c1, c2 ) );
	const Scalar absDet3 = fabs( det3 );

	const Scalar n0 = Vector3Ops::SquaredModulus( c0 );
	const Scalar n1 = Vector3Ops::SquaredModulus( c1 );
	const Scalar n2 = Vector3Ops::SquaredModulus( c2 );
	const Scalar frob = sqrt( n0 + n1 + n2 );

	// THE DEGENERATE REFUSAL RUNS FIRST, BEFORE JACOBI, and the order is
	// load-bearing: it is what makes `sigmaMin > 0` true after the
	// four-ulp downward nudge below, since a genuinely invertible map's
	// smallest singular value is many ulps clear of zero.
	if( !( absDet3 > Scalar( 0 ) ) || !RISE::IsFiniteDouble( static_cast<double>( frob ) ) ) {
		return false;
	}

	// THE EXACT FAST PATH, unchanged and un-nudged.  `M^T M == s^2 I` ?
	// Its entries are the pairwise dot products of the three images, so
	// the test is "equal squared lengths, mutually orthogonal".  RELATIVE
	// to `s^2` = the mean squared length, so it scales with the object and
	// is not a fixed absolute epsilon.  A rotation, a reflection, a
	// uniform scale, or any composition of them lands here, every
	// singular value is exactly `sqrt(s2)`, and the query pays nothing.
	{
		const Scalar s2  = ( n0 + n1 + n2 ) / Scalar( 3 );
		const Scalar tol = Scalar( 1e-9 ) * s2;
		const bool uniform =
			   fabs( n0 - s2 ) <= tol && fabs( n1 - s2 ) <= tol && fabs( n2 - s2 ) <= tol
			&& fabs( Vector3Ops::Dot( c0, c1 ) ) <= tol
			&& fabs( Vector3Ops::Dot( c0, c2 ) ) <= tol
			&& fabs( Vector3Ops::Dot( c1, c2 ) ) <= tol;
		if( uniform ) {
			outSigmaMax = sqrt( s2 );
			outSigmaMin = outSigmaMax;
			outSource   = SigmaSource::Exact;
			return true;
		}
	}

	// ONE-SIDED JACOBI.  Each sweep visits the three column pairs and
	// applies the plane rotation that makes that pair orthogonal; a sweep
	// that rotates NOTHING is the convergence test.  `maxSweeps == 0`
	// therefore falls straight through to the loose fallback, which is how
	// a test reaches the `Loose` state without a pathological matrix.
	Vector3 a[3] = { c0, c1, c2 };
	bool converged = false;
	for( int sweep = 0; sweep < maxSweeps && !converged; ++sweep ) {
		bool rotated = false;
		for( int p = 0; p < 2; ++p ) {
			for( int q = p + 1; q < 3; ++q ) {
				const Scalar alpha = Vector3Ops::SquaredModulus( a[p] );
				const Scalar beta  = Vector3Ops::SquaredModulus( a[q] );
				const Scalar gamma = Vector3Ops::Dot( a[p], a[q] );
				// RELATIVE threshold, at a few ulps of the product of the
				// two column norms: an absolute one would either never
				// converge on a large object or stop early on a small one.
				if( !( fabs( gamma ) > Scalar( 1e-15 ) * sqrt( alpha * beta ) ) ) {
					continue;
				}
				rotated = true;
				// The standard stable form: solve for the rotation that
				// zeroes `gamma`, taking the SMALLER root so the rotation
				// angle stays under 45 degrees and the iteration cannot
				// swap columns back and forth.
				const Scalar zeta = ( beta - alpha ) / ( Scalar( 2 ) * gamma );
				const Scalar sgnZ = ( zeta >= Scalar( 0 ) ) ? Scalar( 1 ) : Scalar( -1 );
				const Scalar t    = sgnZ / ( fabs( zeta ) + sqrt( Scalar( 1 ) + zeta * zeta ) );
				const Scalar cs   = Scalar( 1 ) / sqrt( Scalar( 1 ) + t * t );
				const Scalar sn   = cs * t;
				const Vector3 ap = a[p], aq = a[q];
				a[p] = Vector3( cs*ap.x - sn*aq.x, cs*ap.y - sn*aq.y, cs*ap.z - sn*aq.z );
				a[q] = Vector3( sn*ap.x + cs*aq.x, sn*ap.y + cs*aq.y, sn*ap.z + cs*aq.z );
			}
		}
		if( !rotated ) {
			converged = true;
		}
	}

	if( converged ) {
		const Scalar s0 = Vector3Ops::Magnitude( a[0] );
		const Scalar s1 = Vector3Ops::Magnitude( a[1] );
		const Scalar s2 = Vector3Ops::Magnitude( a[2] );
		Scalar smax = std::max( s0, std::max( s1, s2 ) );
		Scalar smin = std::min( s0, std::min( s1, s2 ) );
		if( RISE::IsFiniteDouble( (double)smax ) && smin > Scalar( 0 ) ) {
			// FOUR ULPS APART, because the whole design rests on a chain of
			// inequalities -- `d_w <= sigmaMax * d_o` for the unsigned
			// answer, `d_w >= sigmaMin * d_o` for the signed one -- and
			// Jacobi returns the singular values to rounding, not below
			// and above them.  Widening the pair by a few ulps costs
			// nothing measurable and keeps every one of those inequalities
			// true of the STORED numbers rather than of the exact ones.
			for( int i = 0; i < 4; ++i ) {
				smax = (Scalar)std::nextafter( (double)smax, HUGE_VAL );
				smin = (Scalar)std::nextafter( (double)smin, 0.0 );
			}
			outSigmaMax = smax;
			outSigmaMin = smin;
			outSource   = SigmaSource::Jacobi;
			return true;
		}
	}

	// THE FALLBACK PAIR, unchanged from Phase 1 and still sound:
	// `sigmaMax <= ||M||_F` (since `||M||_F^2` is the SUM of the squared
	// singular values) and `sigmaMin >= |det| / sigmaMax^2` (since
	// `|det| = sigmaMin * s2 * s3 <= sigmaMin * sigmaMax^2`), which stays
	// valid when the upper bound is substituted for the true `sigmaMax`.
	// Both directions stay SAFE for the query: it searches a wider
	// object-space radius than it must, and reports an unsigned distance
	// no smaller than the truth.
	outSigmaMax = frob;
	outSigmaMin = absDet3 / ( frob * frob );
	outSource   = SigmaSource::Loose;
	return true;
}

Object::Object( ) :
  pGeometry( 0 ),
  pUVGenerator( 0 ),
  pMaterial( 0 ),
  pModifier( 0 ),
  pShader( 0 ),
  pRadianceMap( 0 ),
  pInteriorMedium( 0 ),
  bIsWorldVisible( true ),
  bCastsShadows( true ),
  bReceivesShadows( true ),
  nConsumedBy( 0 ),
  SURFACE_INTERSEC_ERROR( 1e-12 ),
  m_tangentFrameSign( 1.0 ),
  m_worldAreaScale( 1.0 ),
  m_worldLinearScale( 1.0 ),
  m_sigmaMax( 1.0 ),
  m_sigmaMin( 1.0 ),
  m_sigmaSource( SigmaSource::Exact ),
  m_sigmaExact( true ),
  m_sigmaLooseWarned( false ),
  m_distanceRefusalWarned( false )
{
}


Object::Object( const IGeometry* pGeometry_ ) :
  pGeometry( pGeometry_ ),
  pUVGenerator( 0 ),
  pMaterial( 0 ),
  pModifier( 0 ),
  pShader( 0 ),
  pRadianceMap( 0 ),
  pInteriorMedium( 0 ),
  bIsWorldVisible( true ),
  bCastsShadows( true ),
  bReceivesShadows( true ),
  nConsumedBy( 0 ),
  SURFACE_INTERSEC_ERROR( 1e-12 ),
  m_tangentFrameSign( 1.0 ),
  m_worldAreaScale( 1.0 ),
  m_worldLinearScale( 1.0 ),
  m_sigmaMax( 1.0 ),
  m_sigmaMin( 1.0 ),
  m_sigmaSource( SigmaSource::Exact ),
  m_sigmaExact( true ),
  m_sigmaLooseWarned( false ),
  m_distanceRefusalWarned( false )
{
	if( pGeometry ) {
		pGeometry->addref();
	} else {
		GlobalLog()->PrintSourceError( "Object:: Geometry ptr was passed in but is invalid", __FILE__, __LINE__ );
	}
}

Object::~Object( )
{
	safe_release( pGeometry );
	safe_release( pMaterial );
	safe_release( pModifier );
	safe_release( pShader );
	safe_release( pUVGenerator );
	safe_release( pRadianceMap );
	safe_release( pInteriorMedium );
}

void Object::RemoveConsumer()
{
	// SATURATION IS A BUG REPORT, NOT A RECOVERY.  Every AddConsumer has exactly
	// one matching RemoveConsumer (CSGObject::AssignObjects pairs with the
	// destructor and with the outgoing branch of a re-assign), so reaching here
	// at zero means some composite released a claim it never took -- and the
	// balance being off by one in that direction means a LATER release will drive
	// a still-consumed operand to zero and let it render as a standalone shape
	// beside the composite that owns it.  That is precisely the failure the count
	// replaced a bool to prevent, so it must not pass silently; the clamp stays
	// because wrapping an `unsigned int` would pin the operand invisible forever,
	// which is the worse of the two.
	if( !nConsumedBy ) {
		GlobalLog()->PrintSourceError(
			"Object::RemoveConsumer:: unbalanced release -- this object is not consumed by any "
			"csg_object, so a composite has released a claim it never took.  The consumption "
			"count is now under-counted and some operand will later be un-hidden while a live "
			"composite is still using it", __FILE__, __LINE__ );
		return;
	}
	--nConsumedBy;
}

IObjectPriv* Object::CloneFull()
{
	// 87: same container handling as CloneSnapshot -- a container has no
	// geometry, and Object(const IGeometry*) logs a source ERROR for a null
	// one.  World visibility is copied rather than left at the ctor's `true`:
	// a container is created HIDDEN (see RISE_API_CreateObjectOrContainer_), so
	// a clone that came out visible with null geometry would enter every
	// world-visible enumeration containers are deliberately kept out of.
	// (Both clone entry points are currently dead public surface -- no caller
	// repo-wide -- but they are CloneSnapshot's siblings and the whole lesson
	// of this arc is that the sibling is where the defect lives.)  The COMPOSED
	// value for the reason spelled out in CopySnapshotStateInto: the clone has no
	// consumers, so its base flag has to carry the whole answer.
	Object* pClone = pGeometry ? new Object( pGeometry ) : new Object();
	GlobalLog()->PrintNew( pClone, __FILE__, __LINE__, "Clone" );
	pClone->bIsWorldVisible = IsWorldVisible();

	if( pMaterial ) {
		pClone->AssignMaterial( *pMaterial );
	}

	if( pModifier ) {
		pClone->AssignModifier( *pModifier );
	}

	if( pShader ) {
		pClone->AssignShader( *pShader );
	}

	if( pRadianceMap ) {
		pClone->AssignRadianceMap( *pRadianceMap );
	}

	return pClone;
}

IObjectPriv* Object::CloneGeometric()
{
	// 87: see CloneFull -- container ctor selection and world visibility.
	Object* pMe = pGeometry ? new Object( pGeometry ) : new Object();
	GlobalLog()->PrintNew( pMe, __FILE__, __LINE__, "cloned object" );
	pMe->bIsWorldVisible = IsWorldVisible();
	return pMe;
}

void Object::CopySnapshotStateInto( Object& dst ) const
{
	// Shared by Object::CloneSnapshot and CSGObject::CloneSnapshot.  Copies
	// every piece of mutable state EXCEPT the geometry (set by the subclass
	// ctor) and, for CSG, the operands (set by the subclass).  `dst` is a
	// freshly-constructed clone; we may touch its protected Object /
	// Transformable members because access to protected members of the same
	// type is permitted.

	// --- Mutable LEAF: material is cloned to an INDEPENDENT instance so a
	//     later in-place painter-slot rebind on the LIVE material (the
	//     editor's SetMaterialProperty path) does NOT bleed into the
	//     snapshot.  CloneMaterialForSnapshot hands back a reference the
	//     caller owns; AssignMaterial addrefs it, so we release our own. ---
	if( pMaterial ) {
		const IMaterial* matClone = CloneMaterialForSnapshot( pMaterial );
		if( matClone ) {
			dst.AssignMaterial( *matClone );
			matClone->release();
		}
	}

	// --- Immutable / non-property-edited leaves: addref-share. ---
	//     (Shader + interior medium are addref-shared in increment A; the
	//     SSS shader-op cache race + in-place medium-coefficient edits are
	//     the documented residual deferred to increment B — see
	//     SnapshotLeafClone.h.)
	if( pModifier )       { dst.AssignModifier( *pModifier ); }
	if( pShader )         { dst.AssignShader( *pShader ); }
	if( pRadianceMap )    { dst.AssignRadianceMap( *pRadianceMap ); }
	if( pInteriorMedium ) { dst.AssignInteriorMedium( *pInteriorMedium ); }
	if( pUVGenerator )    { dst.SetUVGenerator( *pUVGenerator ); }

	// --- Cheap value-typed flags ---
	// `nConsumedBy` is deliberately NOT among them: it counts the LIVE composites
	// consuming this object as a CSG operand, and a clone is consumed by whoever
	// assigns it, not by whoever consumed the original.  CSGObject::CloneSnapshot's
	// own AssignObjects establishes it for the operand clones it makes.
	//
	// WHICH IS EXACTLY WHY THE COMPOSED `IsWorldVisible()` IS COPIED HERE AND NOT THE
	// BASE FLAG.  The clone starts at zero consumers, so the base flag is the ONLY
	// thing left holding its visibility; copying a consumed operand's base flag
	// (which is `true` since 87 step 3b -- being an operand is the COUNT now, not the
	// flag) would hand the clone a world-VISIBLE standalone copy of something that
	// has no existence as a standalone shape.  `Scene::CreateSnapshot` reaches that
	// case directly: it clones every manager item BY NAME, so a `csg_object`'s
	// operands are cloned once on their own account and again, correctly hidden,
	// underneath the composite's own clone.  Copying the composed value restores the
	// pre-3b outcome exactly (before the count, a consumed operand's base flag WAS
	// `false`, so this line already copied `false`), and it stays right for the
	// operand clones CSGObject::CloneSnapshot makes: their AssignObjects consumes
	// them a moment later, so they are hidden either way.
	dst.bIsWorldVisible        = IsWorldVisible();
	dst.bCastsShadows          = bCastsShadows;
	dst.bReceivesShadows       = bReceivesShadows;
	dst.SURFACE_INTERSEC_ERROR = SURFACE_INTERSEC_ERROR;
	dst.m_tangentFrameSign     = m_tangentFrameSign;
	dst.m_worldAreaScale       = m_worldAreaScale;
	dst.m_worldLinearScale     = m_worldLinearScale;
	// The proximity query's transform bounds ride along with the other two
	// transform-derived caches.  A clone's FinalizeTransformations would
	// recompute them anyway; copying keeps a clone that is never finalized
	// answering the same way its source does rather than at the identity.
	dst.m_sigmaMax             = m_sigmaMax;
	dst.m_sigmaMin             = m_sigmaMin;
	dst.m_sigmaSource          = m_sigmaSource;
	dst.m_sigmaExact           = m_sigmaExact;

	// --- Transform BUILDING BLOCKS (Transformable protected state) ---
	// Copying these is what makes the clone independent: a later
	// TranslateObject() on the live object pushes onto the LIVE stack and
	// re-finalizes the LIVE matrices, leaving the clone's copies untouched.
	dst.m_mxPosition      = m_mxPosition;
	dst.m_mxOrientation   = m_mxOrientation;
	dst.m_mxScale         = m_mxScale;
	dst.m_mxStretch       = m_mxStretch;
	// doc 89 slice C: the mirror is one of the building blocks, so it is copied
	// with them.  Omitting it would give the snapshot the right FINAL matrix
	// (copied below) but an un-mirrored local frame -- so the first re-finalize on
	// the clone (any absolute setter, a restore, the animator) would un-reflect it,
	// exactly the failure mode the local/parent-world copy below already documents.
	dst.m_mxMirror        = m_mxMirror;
	dst.m_mirrorAxis      = m_mirrorAxis;
	CopyTransformMetadataTo( dst );
	dst.m_transformstack  = m_transformstack;   // std::deque<Matrix4> value copy

	// --- Finalized matrices (so the clone is render-ready without a
	//     re-finalize, and exactly reflects the live pose at snap time) ---
	dst.m_mxFinalTrans    = m_mxFinalTrans;
	dst.m_mxInvFinalTrans = m_mxInvFinalTrans;
	dst.m_mxInvTranspose  = m_mxInvTranspose;

	// --- 87 scene graph: the LOCAL matrix and the parent world it was
	//     composed against.  Without these the clone's world matrix would be
	//     right but its local/parent state identity, so the very first
	//     re-finalize on the clone (any absolute setter, a restore, the
	//     animator) would collapse it back to an unparented pose.
	dst.m_mxLocalTrans           = m_mxLocalTrans;
	dst.m_mxParentWorld          = m_mxParentWorld;
	dst.m_mxParentWorldInv       = m_mxParentWorldInv;
	dst.m_bParentWorldInvertible = m_bParentWorldInvertible;
}

Object* Object::CloneSnapshot() const
{
	// See Object.h for the rationale.  Build a fresh Object that shares the
	// immutable geometry leaf (the ctor addrefs it), then deep-copy the
	// mutable state.
	// 87: a CONTAINER has no geometry, and Object(const IGeometry*) logs a
	// source ERROR for a null one -- correct for a leaf, noise for a container.
	// Pick the ctor that matches what this object actually is.
	Object* pClone = pGeometry ? new Object( pGeometry ) : new Object();
	GlobalLog()->PrintNew( pClone, __FILE__, __LINE__, "snapshot clone" );

	CopySnapshotStateInto( *pClone );

	return pClone;
}

bool Object::AssignMaterial( const IMaterial& pMat )
{
	safe_release( pMaterial );

	pMaterial = &pMat;
	pMaterial->addref();

	return false;
}

bool Object::AssignGeometry( const IGeometry& pGeom )
{
	// Runtime geometry swap (interactive editor via SceneEdit::
	// SetObjectGeometry).  Mirrors AssignMaterial: release the prior
	// reference and retain the new one.  The bounding box is derived
	// on demand from pGeometry (getBoundingBox), so the caller
	// (SceneEditor::RunObjectInvariantChain) invalidates the top-level
	// acceleration afterward and the next render rebuilds the TLAS.
	safe_release( pGeometry );

	pGeometry = &pGeom;
	pGeometry->addref();

	return true;
}

bool Object::AssignModifier( const IRayIntersectionModifier& pMod )
{
	safe_release( pModifier );

	pModifier = &pMod;
	pModifier->addref();

	return false;
}

bool Object::AssignShader( const IShader& pShader_ )
{
	safe_release( pShader );

	pShader = &pShader_;
	pShader->addref();

	return true;
}

bool Object::AssignRadianceMap( const IRadianceMap& pRadianceMap_ )
{
	safe_release( pRadianceMap );

	pRadianceMap = &pRadianceMap_;
	pRadianceMap->addref();

	return true;
}

bool Object::SetUVGenerator( const IUVGenerator& pUVG )
{
	safe_release( pUVGenerator );

	pUVGenerator = &pUVG;
	pUVGenerator->addref();

	return true;
}

void Object::SetShadowParams( const bool bCasts, const bool bReceives )
{
	bCastsShadows = bCasts;
	bReceivesShadows = bReceives;
}

bool Object::AssignInteriorMedium( const IMedium& medium )
{
	safe_release( pInteriorMedium );

	pInteriorMedium = &medium;
	pInteriorMedium->addref();

	return true;
}

void Object::ClearInteriorMedium()
{
	safe_release( pInteriorMedium );
	pInteriorMedium = 0;
}

void Object::ClearShader()
{
	safe_release( pShader );
	pShader = 0;
}

void Object::ClearMaterial()
{
	safe_release( pMaterial );
	pMaterial = 0;
}

void Object::ClearModifier()
{
	safe_release( pModifier );
	pModifier = 0;
}

void Object::ClearRadianceMap()
{
	safe_release( pRadianceMap );
	pRadianceMap = 0;
}

void Object::ClearGeometry()
{
	// 87 recursive scene graph: a CONTAINER node is an Object with no geometry.
	// The null-geometry guards throughout this file are what make that safe;
	// see getBoundingBox()'s comment for the (now reachable) call-graph note.
	safe_release( pGeometry );
	pGeometry = 0;
}

const IMaterial* Object::GetMaterial() const
{
	return pMaterial;
}

const IMedium* Object::GetInteriorMedium() const
{
	return pInteriorMedium;
}

bool Object::ComputeAnalyticalDerivatives(
	const Point2& uv,
	Scalar        smoothing,
	Point3&       outWorldPosition,
	Vector3&      outWorldNormal,
	Vector3&      outWorldDpdu,
	Vector3&      outWorldDpdv,
	Vector3&      outWorldDndu,
	Vector3&      outWorldDndv
	) const
{
	if( !pGeometry ) return false;

	// Object-space query
	Point3  oP;
	Vector3 oN, oDpdu, oDpdv, oDndu, oDndv;
	if( !pGeometry->ComputeAnalyticalDerivatives(
			uv, smoothing, oP, oN, oDpdu, oDpdv, oDndu, oDndv ) )
	{
		return false;
	}

	// Apply transform — same convention as the IntersectRay path (see the
	// derivatives block comment there for the full derivation):
	//  - Position: full forward transform.
	//  - Tangent vectors (dpdu, dpdv): forward transform's linear part
	//    (translation drops out for vector arithmetic).
	//  - Normal: inverse-transpose's linear part, renormalized.
	//  - Its derivatives (dndu, dndv): NOT a plain inverse-transpose --
	//    they are derivatives of the SHADING normal, i.e. of the
	//    RENORMALIZED world normal field, so they need the quotient-rule
	//    transform (identical to Object::IntersectRay's derivatives
	//    block):
	//      dn_w/du = (I - n_w n_w^T) . (M^-T dndu_obj) / ||M^-T n_obj||
	//    A plain inverse-transpose (no renormalize) under-corrects by a
	//    factor of the local scale under non-uniform / non-rigid
	//    transforms, same bug class as the IntersectRay path had.
	outWorldPosition = Point3Ops::Transform( m_mxFinalTrans, oP );
	outWorldDpdu     = Vector3Ops::Transform( m_mxFinalTrans, oDpdu );
	outWorldDpdv     = Vector3Ops::Transform( m_mxFinalTrans, oDpdv );

	Vector3 outNormalUnnorm = Vector3Ops::Transform( m_mxInvTranspose, oN );
	const Scalar dNormalWorldMag = Vector3Ops::NormalizeMag( outNormalUnnorm );
	outWorldNormal = outNormalUnnorm;

	if( dNormalWorldMag > NEARZERO ) {
		const Scalar invMag = Scalar(1.0) / dNormalWorldMag;

		const Vector3 dndu_lin = Vector3Ops::Transform( m_mxInvTranspose, oDndu );
		outWorldDndu = ( dndu_lin - outWorldNormal * Vector3Ops::Dot( outWorldNormal, dndu_lin ) ) * invMag;

		const Vector3 dndv_lin = Vector3Ops::Transform( m_mxInvTranspose, oDndv );
		outWorldDndv = ( dndv_lin - outWorldNormal * Vector3Ops::Dot( outWorldNormal, dndv_lin ) ) * invMag;
	} else {
		// Transform singular along the normal direction -- mirrors
		// Object::IntersectRay's identical guard.  No well-defined unit
		// world normal to differentiate against; dividing by ~0 would hand
		// back Inf/NaN.
		//
		// P2-1: this function's OWN contract (see IObject.h) is that a
		// `false` return means "geometry can't answer this query, caller
		// falls back" -- e.g. ManifoldSolver::ComputeVertexDerivatives's
		// smoothing>0 analytical path (ManifoldSolver.cpp) treats `true` as
		// full success and feeds these vectors straight into Newton's
		// Jacobian with no further validity check.  Zeroing outWorldDndu/
		// outWorldDndv and still returning `true` was a FABRICATED success:
		// the caller got a flat (zero-curvature) Jacobian instead of the
		// documented "no analytical path available, fall back to the
		// single-stage solver" behaviour it's written to expect.  Return
		// `false` instead, exactly like the `!pGeometry` and
		// `!pGeometry->ComputeAnalyticalDerivatives(...)` guards above.
		outWorldDndu = Vector3( 0, 0, 0 );
		outWorldDndv = Vector3( 0, 0, 0 );
		return false;
	}
	return true;
}

const BoundingBox Object::getBoundingBox() const
{
	// NULL-GEOMETRY GUARD.  This branch is REACHABLE as of 87 (recursive scene
	// graph): a `standard_object` with no `geometry` is a CONTAINER node -- a
	// pure transform other objects are parented to -- and Job::AddObject
	// creates it with no geometry at all.  (Before 87 the only null-geometry
	// object was a CSGObject, which overrides getBoundingBox() entirely and
	// never reaches here, so this used to be dead defensive code; the
	// 2026-07-31 audit-round comment that said so is superseded.)
	//
	// A container is world-INVISIBLE, so nothing in the render path asks it
	// for a box: CreateBVH/CreateOctree filter on IsWorldVisible() before
	// calling GetElementBoundingBox.  The empty box is what a caller outside
	// that gate gets, matching CSGObject::getBoundingBox's own no-operand
	// fallback.
	if( !pGeometry ) {
		return BoundingBox( Point3( 0, 0, 0 ), Point3( 0, 0, 0 ) );
	}

	const BoundingBox bbox = pGeometry->GenerateBoundingBox();

	// Transform all 8 corners of the local bbox and take the AABB of the
	// rotated set.  Transforming only ll and ur produces an AABB that
	// covers a single edge of the rotated cube — for a 50°/120° rotation
	// the resulting world bbox covers ~25% of the actual extent in the
	// rotated axes, so BSP / Octree placement based on this bbox excludes
	// rays that pass through the geometry's true rotated extent.  The
	// downstream symptom is whole strips of a rotated object rendering as
	// background because acceleration-structure traversal never reaches
	// the leaf that holds the object.
	const Point3 corners[8] = {
		Point3( bbox.ll.x, bbox.ll.y, bbox.ll.z ),
		Point3( bbox.ur.x, bbox.ll.y, bbox.ll.z ),
		Point3( bbox.ll.x, bbox.ur.y, bbox.ll.z ),
		Point3( bbox.ur.x, bbox.ur.y, bbox.ll.z ),
		Point3( bbox.ll.x, bbox.ll.y, bbox.ur.z ),
		Point3( bbox.ur.x, bbox.ll.y, bbox.ur.z ),
		Point3( bbox.ll.x, bbox.ur.y, bbox.ur.z ),
		Point3( bbox.ur.x, bbox.ur.y, bbox.ur.z )
	};

	Point3 wll = Point3Ops::Transform( m_mxFinalTrans, corners[0] );
	Point3 wur = wll;
	for( int i = 1; i < 8; i++ ) {
		const Point3 c = Point3Ops::Transform( m_mxFinalTrans, corners[i] );
		if( c.x < wll.x ) wll.x = c.x;
		if( c.y < wll.y ) wll.y = c.y;
		if( c.z < wll.z ) wll.z = c.z;
		if( c.x > wur.x ) wur.x = c.x;
		if( c.y > wur.y ) wur.y = c.y;
		if( c.z > wur.z ) wur.z = c.z;
	}

	return BoundingBox( wll, wur );
}

void Object::IntersectRay( RayIntersection& ri, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	// NULL-GEOMETRY GUARD: see getBoundingBox()'s comment above.  Reachable as
	// of 87 for a CONTAINER node, though the world-visible gate in
	// ObjectManager::RayElementIntersection means no ray reaches a container
	// through the normal traversal.  No hit.  P3 CORRECTION (fix round 3): this does
	// NOT match CSGObject::IntersectRay's own no-operand fallback -- CSG's
	// `if( !pObjectA || !pObjectB ) { ...; return; }` fires BEFORE it ever
	// sets `ri.geometric.bHit = false`, so it returns WITHOUT touching
	// bHit at all (relying on the caller's own pre-call initialization).
	// This guard explicitly clears bHit instead, which is the stronger,
	// correct contract for THIS call site (Object::IntersectRay is a
	// public entry point with no such caller-init guarantee) -- the
	// behavior here is right, but the earlier comment's "matching..."
	// claim was not.
	if( !pGeometry ) {
		ri.geometric.bHit = false;
		return;
	}

	// Bring the ray into our frame, first tuck away the original ray value
	const Ray orig = ri.geometric.ray;

	ri.geometric.ray.origin = Point3Ops::Transform( m_mxInvFinalTrans, orig.origin );

	// Capture the UNNORMALIZED transformed direction's magnitude before
	// normalizing it into the local-frame ray -- this is the direction-true
	// world-to-local distance factor used below (P1 fix: was a +X-axis
	// probe, see the `factor` comment further down).
	const Vector3 dirLocalUnnorm = Vector3Ops::Transform( m_mxInvFinalTrans, orig.Dir() );
	const Scalar dirLocalMag = Vector3Ops::Magnitude( dirLocalUnnorm );
	ri.geometric.ray.SetDir( Vector3Ops::Normalize( dirLocalUnnorm ) );

	// Landing 2: transform ray differentials into object space alongside
	// origin/dir, otherwise ComputeTextureFootprint would project
	// world-space auxiliaries onto object-space dpdu/dpdv and produce
	// the wrong UV footprint (and therefore the wrong mip LOD).
	//
	// Origins are simple: differentials are OFFSETS between two world
	// points, so they transform as vectors (linear part only — the
	// translation cancels in the diff of two transformed points).
	//
	// Directions are NOT simple.  rxDir / ryDir were established by the
	// camera as the offset between two UNIT-normalized world directions,
	//   rxDir_world = aux_x_world_norm − d_world_norm
	// and the same convention must hold in object space:
	//   rxDir_obj   = aux_x_obj_norm   − d_obj_norm
	// Under any non-identity scale (uniform or not) the obvious
	// `M_inv * rxDir_world` gives an unnormalised vector that does not
	// equal `aux_x_obj_norm − d_obj_norm`.  Reconstruct the auxiliary
	// fully: rebuild `aux = d + diff` in world space, transform, re-
	// normalise, then re-difference against the (already normalised)
	// object-space central direction.
	//
	// SetDir() on the central ray cleared hasDifferentials, so re-set
	// it after we've finished writing.
	if( orig.hasDifferentials ) {
		ri.geometric.ray.diffs.rxOrigin = Vector3Ops::Transform( m_mxInvFinalTrans, orig.diffs.rxOrigin );
		ri.geometric.ray.diffs.ryOrigin = Vector3Ops::Transform( m_mxInvFinalTrans, orig.diffs.ryOrigin );

		const Vector3 d_world = orig.Dir();
		const Vector3 aux_x_world( d_world.x + orig.diffs.rxDir.x,
		                           d_world.y + orig.diffs.rxDir.y,
		                           d_world.z + orig.diffs.rxDir.z );
		const Vector3 aux_y_world( d_world.x + orig.diffs.ryDir.x,
		                           d_world.y + orig.diffs.ryDir.y,
		                           d_world.z + orig.diffs.ryDir.z );
		const Vector3 aux_x_obj = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvFinalTrans, aux_x_world ) );
		const Vector3 aux_y_obj = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvFinalTrans, aux_y_world ) );
		const Vector3 d_obj     = ri.geometric.ray.Dir();
		ri.geometric.ray.diffs.rxDir = Vector3( aux_x_obj.x - d_obj.x, aux_x_obj.y - d_obj.y, aux_x_obj.z - d_obj.z );
		ri.geometric.ray.diffs.ryDir = Vector3( aux_y_obj.x - d_obj.x, aux_y_obj.y - d_obj.y, aux_y_obj.z - d_obj.z );
		ri.geometric.ray.hasDifferentials = true;
	}

	// factor converts a WORLD-frame distance limit (dHowFar) into the
	// local-frame traversal limit used by the box pre-test and the
	// dHowFar2-comparisons below.  MUST be the magnitude of the
	// TRANSFORMED RAY DIRECTION (dirLocalMag, captured above before it was
	// normalized into ray.dir) -- NOT an arbitrary +X-axis probe (P1 fix).
	// Under non-uniform scale, |M^-1 * v| depends on which direction v
	// points; a +X-only factor mis-scales the limit for every ray not
	// travelling along local +X (e.g. a shadow ray leaking past the light,
	// or a valid hit truncated short of it).  Guard a degenerate transform
	// that collapses this direction to ~0 (singular along this direction)
	// by falling back to an unscaled factor of 1.0.
	const Scalar factor = (dirLocalMag > NEARZERO) ? dirLocalMag : Scalar(1.0);
	Scalar dHowFar2 = dHowFar;

	// We can't go farther than infinity, so in this case only reduce the
	// length, never extend: dHowFar==RISE_INFINITY is a large FINITE
	// sentinel (not IEEE inf -- see the ffast-math/no-infinity convention),
	// so scaling it by factor>=1 would overflow into a real infinity for
	// no benefit (there's nothing to shrink); only factor<1 is worth
	// applying.  This reasoning is about the sentinel's magnitude, not
	// about how `factor` itself is derived -- unchanged now that factor is
	// direction-true rather than +X-axis-based.
	if( (dHowFar != RISE_INFINITY) || (factor < 1.0) ) {
		dHowFar2 = factor*dHowFar;
	}

	// Compute ray intersection with box.
	//
	// When the ray's ORIGIN is inside the bounding box, RayBoxIntersection
	// flips its contract: hit.dRange reports the distance to EXIT the box
	// (i.e. tmax, since tmin is negative), and hit.dRange2 holds the
	// negative tmin.  We detect "origin inside the box" via `dRange2 < 0`
	// and skip the `dRange > dHowFar2` early-return in that case — the
	// ray may still hit the geometry's surface within dHowFar even though
	// the box it sits inside extends further.
	//
	// Prior to this fix, the early-return fired incorrectly on short
	// probes (e.g. ManifoldSolver::ComputeVertexDerivatives, which
	// shoots a 0.05 probe from +0.05 above a torus surface vertex back
	// down toward it — both probe origin and target sit inside the
	// torus bbox, so the bbox-exit distance ~0.2 always exceeded
	// dHowFar2 = 0.1).  The probe was silently dropped and the SMS
	// solver rejected the whole chain with "ComputeVertexDerivatives
	// failed".  The torus surface was fine; the gate was wrong.
	if( pGeometry->DoPreHitTest() )
	{
		BOX_HIT		hit;
		BoundingBox	bbox = pGeometry->GenerateBoundingBox();
		RayBoxIntersection( ri.geometric.ray, hit, bbox.ll, bbox.ur );
		if( !hit.bHit ) {
			return;
		}

		const bool originInsideBox = ( hit.dRange2 < 0 );
		if( !originInsideBox && hit.dRange > dHowFar2 ) {
			return;
		}
	}

	pGeometry->IntersectRay( ri.geometric, bHitFrontFaces, bHitBackFaces, bComputeExitInfo );
	if( ri.geometric.bHit )
	{
		// PIXEL FOOTPRINT, for EVERY geometry
		// (docs/TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md §3.3).  This must be
		// the FIRST statement in the hit block, and the frame argument is
		// why: `ComputeFootprintVectors` reads ri.ray, ri.range and
		// ri.vNormal, and it needs all three in ONE consistent frame.  At
		// this instant they are all OBJECT-space -- this function
		// transformed the ray (and its differentials) on entry and has not
		// yet run pUVGenerator or promoted any normal.  One statement
		// later, `vNormalWorldUnnorm` moves ri.vNormal to world and the
		// three would disagree.
		//
		// This used to live inside TriangleMeshGeometry{,Indexed}::
		// RayElementIntersection, which made meshes the only geometry with
		// a footprint.  Hoisting it here lights up analytic primitives,
		// SDFs (hence sweeps and skeletons), boxes, disks, planes, patches
		// and hair -- and is also strictly CHEAPER on meshes, since it now
		// runs once per ray rather than once per accepted closer candidate.
		// Mesh values are unchanged: the winning candidate's ri.vNormal /
		// ri.range are exactly what the old per-candidate call last saw.
		//
		// `SolveFootprintUV` self-skips unless ri.derivatives.valid, so a
		// UV-free geometry gets an honest width and no Jacobian -- see the
		// two-flag contract on TextureFootprint.  Costs nothing at all when
		// ray.hasDifferentials is false, which is every shadow ray, every
		// NEE ray, every photon, every ray after the first scattering
		// bounce, and every ray from the thin-lens / orthographic / fisheye
		// cameras.
		if( ri.geometric.ray.hasDifferentials ) {
			ComputeFootprintVectors( ri.geometric, ri.geometric.ray );
			SolveFootprintUV( ri.geometric );
		}

		// This an overriding UV generator only, it is for geometries that don't know how to compute
		// their UV co-ordinates so the user has specified a geometry object to help them out.
		// Box/Cylinder/Sphere UV projections pick the projection axis
		// from the surface face — use the GEOMETRIC normal so the
		// chosen axis is the actual face orientation, not Phong-
		// interpolated or bump-perturbed.  On analytical primitives
		// shading == geometric so this is a no-op there.
		if( pUVGenerator ) {
			pUVGenerator->GenerateUV( ri.geometric.ptIntersection, ri.geometric.vGeomNormal, ri.geometric.ptCoord );
		}

		// Transform the normals back
		//
		// Also capture the PRE-normalization magnitude of the transformed
		// shading normal (norm of M^-T n_obj) -- the derivatives block below
		// reuses both this magnitude and the resulting unit world normal to
		// apply the quotient-rule transform to dndu/dndv (see
		// docs/GEOMETRY_DERIVATIVES.md "World-space transform"). NormalizeMag
		// mutates its argument in place and returns the pre-normalize length.
		Vector3 vNormalWorldUnnorm = Vector3Ops::Transform( m_mxInvTranspose, ri.geometric.vNormal );
		const Scalar dShadingNormalWorldMag = Vector3Ops::NormalizeMag( vNormalWorldUnnorm );
		ri.geometric.vNormal = vNormalWorldUnnorm;
		// Geometric normal transforms identically (it's also a normal vector,
		// just describing the actual face orientation rather than the shading
		// approximation).  Renormalize because non-uniform scales can otherwise
		// leave it un-unit.
		ri.geometric.vGeomNormal = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvTranspose, ri.geometric.vGeomNormal ));
		// Shading ONB.  By default the tangent (u-axis) is whatever
		// CreateFromW picks from a canonical axis -- fine for isotropic
		// materials, but an arbitrary base for anisotropic GGX.  A
		// geometry can instead request a COHERENT, world-X-aligned tangent
		// (bShadingTangentFromGeometry) so an anisotropic tangent_rotation
		// rotates from the same base on it as on the cartesian_disk mesh
		// (whose constant +Z normal makes CreateFromW yield ±world-X).  We
		// project world-X into the now-world-space shading-normal plane and
		// hand it to CreateFromWU (W fixed = normal, U re-orthonormalized
		// against W); when world-X is parallel to the normal we fall back
		// to world-Y so the projection never degenerates.
		if( ri.geometric.bShadingTangentFromGeometry ) {
			const Vector3& n = ri.geometric.vNormal;
			Vector3 t;
			bool bHaveSuppliedTangent = false;

			// C2: a geometry that ALSO supplies a real fibre tangent (currently only
			// HairGeometry, via bHasShadingTangent) gets it promoted to world space
			// exactly like vTangent below -- the forward matrix, not inverse-
			// transpose, because a tangent is a direction ALONG the surface, not a
			// normal.  The WRITE-BACK into ri.geometric.vShadingTangent (not just a
			// local variable) matters beyond this function: it is what lets
			// CSGObject::IntersectRay's identical block treat the field as "one
			// promotion short of world space" for a nested child, exactly the
			// convention vTangent already establishes below -- without the
			// write-back a hair fibre wrapped in CSG would silently lose one
			// level of transform.  The promoted tangent is then projected into
			// the (now world-space) shading-normal plane, mirroring the world-X
			// projection this branch already does for the SDFGeometry
			// heightfield case.  Degenerate (near-parallel to the normal) falls
			// back to that legacy world-X projection below, so a pathological
			// hit never produces a NaN ONB.
			//
			// Two DISTINCT degeneracies here, handled at two different scopes:
			//   - tWorld itself near-zero (SquaredModulus < NEARZERO): the
			//     TRANSFORM was singular along the tangent's object-space
			//     direction (e.g. a zero/near-zero scale axis) -- the
			//     promoted tangent is garbage, not just locally unusable.
			//     A "valid" flag paired with a zero vector is a trap for
			//     any consumer downstream of this function, including a
			//     nested CSG parent that would otherwise promote that
			//     garbage one level further -- so clear bHasShadingTangent
			//     and skip the write-back entirely, leaving
			//     vShadingTangent untouched (stale, but the cleared flag
			//     means nobody reads it).
			//   - only tProj near-zero (tWorld valid but parallel to the
			//     now-world-space normal): this level's ONB alone can't
			//     use it, but the promoted value is still correct and
			//     must be written back so a nested CSG parent (which
			//     applies ITS OWN transform and may un-degenerate it)
			//     sees the real promoted tangent, not the fallback.
			if( ri.geometric.bHasShadingTangent ) {
				const Vector3 tWorld = Vector3Ops::Normalize(
					Vector3Ops::Transform( m_mxFinalTrans, ri.geometric.vShadingTangent ) );
				if( Vector3Ops::SquaredModulus( tWorld ) < NEARZERO ) {
					ri.geometric.bHasShadingTangent = false;
				} else {
					// Write back the UNPROJECTED promoted value, not tProj: the
					// CSG nesting invariant composes raw (un-projected) transforms
					// one level at a time -- projecting into THIS level's shading-
					// normal plane is a purely local ONB concern, done below from
					// the local `t`/`tProj`, not something a nested parent should
					// inherit.
					ri.geometric.vShadingTangent = tWorld;
					const Vector3 tProj = tWorld - n * Vector3Ops::Dot( n, tWorld );
					if( Vector3Ops::SquaredModulus( tProj ) >= NEARZERO ) {
						t = tProj;
						bHaveSuppliedTangent = true;
					}
					// else: tWorld is valid but parallel to the normal here --
					// the write-back above still stands (a nested CSG parent's
					// own transform may un-degenerate it), only THIS level falls
					// back to the legacy world-X projection below.
				}
			}

			if( !bHaveSuppliedTangent ) {
				t = Vector3( 1.0 - n.x*n.x, -n.x*n.y, -n.x*n.z );	// (1,0,0) - n*dot(n,(1,0,0))
				if( Vector3Ops::SquaredModulus( t ) < NEARZERO ) {
					t = Vector3( -n.y*n.x, 1.0 - n.y*n.y, -n.y*n.z );	// (0,1,0) - n*dot(n,(0,1,0))
				}
			}
			ri.geometric.onb.CreateFromWU( n, t );	// W = n (fixed); V = norm(W x t), U = V x W (double-cross => U = t projected into the W-plane)

			// P1 fix (docs/CLOTH_FABRIC_DESIGN.md 9.9 fix round): applies to
			// BOTH sub-branches above (a real supplied tangent, and the legacy
			// world-X-projection fallback), for two related but distinct
			// reasons:
			//
			//  * Supplied-tangent case: V = cross(W,t) = cross(n_world,
			//    t_world), where n_world came from the INVERSE-TRANSPOSE and
			//    t_world is the OBJECT-space tangent forward-transformed --
			//    exactly the mismatched pair that makes
			//    `bitangentSign *= m_tangentFrameSign` a few lines below
			//    necessary for NormalMap's cross(N,T)-built bitangent.  Under
			//    a mirrored instance (`scale -1 1 1`, m_tangentFrameSign ==
			//    -1) `onb.v()` pointed the wrong way, so
			//    `MicrofacetUtils::RotateTangent`'s `u*c + v*s` rotated an
			//    authored `tangent_rotation` the opposite sense on a mirrored
			//    panel versus its unmirrored twin.
			//  * Legacy world-X fallback (SDFGeometry heightfield mode, or a
			//    degenerate supplied tangent): `t` here is already a plain
			//    world-space construction (world-X projected against the
			//    already-promoted `n`), so there is no forward/inverse-
			//    transpose mismatch to correct -- but this branch is also the
			//    SDFGeometry-heightfield half of the doc's named
			//    heightfield/cartesian_disk PAIRING (this file's own comment
			//    a few lines up).  The mesh half of that pairing (once it has
			//    a real UV tangent) takes the supplied-tangent branch above
			//    and DOES get FlipV'd under mirroring -- so flipping V here
			//    too is what keeps the pairing's `tangent_rotation` sense
			//    coherent between the SDF and its mesh twin when EITHER is
			//    mirrored, not an unrelated correctness claim about the
			//    world-X convention in isolation.
			//
			// U is NOT touched in either case: it is the promoted tangent
			// direction itself (forward-transformed dpdu / vTangent, or the
			// world-X projection), which needs no correction --
			// GeometryShadingTangentTest.cpp's money assertions pin `onb.u()`
			// to exactly that value, mirrored transforms included.
			// m_tangentFrameSign is +1 whenever this Object's own transform
			// is orientation-preserving, so this is a no-op there -- the
			// legacy `else` branch (CreateFromW, no geometry-supplied
			// tangent at all) is untouched either way.
			if( m_tangentFrameSign < Scalar( 0 ) ) {
				ri.geometric.onb.FlipV();
			}
		} else {
			ri.geometric.onb.CreateFromW( ri.geometric.vNormal );
		}

		// Transform the per-vertex tangent (v3 storage path) from object
		// space to world space.  Tangents transform with the forward
		// matrix (like positions / dpdu), NOT inverse-transpose -- they
		// are surface-tangent directions, not normals.
		//
		// bitangentSign needs to flip iff the transform reverses
		// orientation (det(M) < 0).  For an orientation-preserving
		// transform, cross(N_world, T_world) gives the same world-space
		// bitangent as transforming the original cross(N_obj, T_obj),
		// so the imported sign carries through unchanged.  For a
		// mirroring transform like `scale -1 1 1` the linear part has
		// negative determinant and cross(N_world, T_world) ends up
		// pointing in the opposite world direction from the
		// transformed-original bitangent -- multiplying the imported
		// sign by m_tangentFrameSign (computed once in
		// FinalizeTransformations) puts it back, so the same source
		// mesh shades correctly under mirrored instancing.
		if( ri.geometric.bHasTangent ) {
			ri.geometric.vTangent = Vector3Ops::Normalize(
				Vector3Ops::Transform( m_mxFinalTrans, ri.geometric.vTangent ) );
		}
		// doc 89 slice C: UNCONDITIONAL, outside the bHasTangent gate.  The sign is
		// a property of THIS TRANSFORM, not of whether the asset shipped a TANGENT
		// accessor -- and the consumer that needs it most is the branch with NO
		// imported tangent: NormalMap's derivative fallback builds
		// `B = cross(N, T) * bitangentSign` from dpdu, which is exactly the
		// cross-product whose world direction flips under a negative determinant.
		// Folded here (it initialises to 1.0, so this is a no-op for an
		// orientation-preserving transform and for an un-tangented hit under one)
		// rather than at each consumer, so every reader of `bitangentSign` sees the
		// same world-handedness convention.  Without it a mirrored, normal-mapped
		// asset renders with its green channel inverted -- lathe / sweep / skin
		// bakes, which carry derivatives but no TANGENT, are exactly that case.
		ri.geometric.bitangentSign *= m_tangentFrameSign;

		// Transform surface derivatives from object space to world space.
		// dpdu, dpdv are tangent vectors -- transform like positions (use
		// the forward transform m_mxFinalTrans).
		//
		// dndu, dndv are derivatives of the SHADING normal (vNormal, already
		// renormalized to world space above), NOT the raw inverse-transpose
		// of the object-space derivative.  A plain inverse-transpose
		// transform is only correct for a normal-LIKE quantity that is not
		// itself required to stay a derivative of a UNIT vector field; dndu/
		// dndv fail that requirement whenever the transform isn't rigid,
		// because the field they differentiate (n) gets renormalized and
		// they must differentiate the renormalized field, not the raw one.
		// The correct transform is the quotient rule applied to
		//   n_w(u,v) = M^-T n_obj(u,v) / ||M^-T n_obj(u,v)||
		// i.e.
		//   dn_w/du = (I - n_w n_w^T) . (M^-T dndu_obj) / ||M^-T n_obj||
		// (same for dndv).  This is a provable no-op under a rigid
		// transform: ||M^-T n|| == 1 and the projection (I - n_w n_w^T)
		// removes a component that is already ~0 by the object-space
		// contract dndu . n ~= 0 (docs/GEOMETRY_DERIVATIVES.md invariant 2).
		// dShadingNormalWorldMag/vNormal were captured/finalized just above
		// when the shading normal itself was promoted to world space.
		if( ri.geometric.derivatives.valid ) {
			ri.geometric.derivatives.dpdu = Vector3Ops::Transform(
				m_mxFinalTrans, ri.geometric.derivatives.dpdu );
			ri.geometric.derivatives.dpdv = Vector3Ops::Transform(
				m_mxFinalTrans, ri.geometric.derivatives.dpdv );

			if( dShadingNormalWorldMag > NEARZERO ) {
				const Vector3& n_w = ri.geometric.vNormal;
				const Scalar invMag = Scalar(1.0) / dShadingNormalWorldMag;

				const Vector3 dndu_lin = Vector3Ops::Transform(
					m_mxInvTranspose, ri.geometric.derivatives.dndu );
				ri.geometric.derivatives.dndu =
					( dndu_lin - n_w * Vector3Ops::Dot( n_w, dndu_lin ) ) * invMag;

				const Vector3 dndv_lin = Vector3Ops::Transform(
					m_mxInvTranspose, ri.geometric.derivatives.dndv );
				ri.geometric.derivatives.dndv =
					( dndv_lin - n_w * Vector3Ops::Dot( n_w, dndv_lin ) ) * invMag;
			} else {
				// The transform is ill-conditioned or degenerate along the
				// normal direction (||M^-T n|| <= NEARZERO = 1e-12, e.g. a
				// zero/near-zero scale axis collapsing the normal) -- there
				// is no well-defined unit world shading-normal to
				// differentiate against, and dividing by dShadingNormalWorldMag
				// here would produce Inf/NaN or an arbitrarily amplified
				// (numerically meaningless) derivative.
				//
				// P1-5 (was FALSE, twice): this code calls
				// Vector3Ops::NormalizeMag, not Normalize, and NormalizeMag's
				// OWN internal guard is `mag > 0.0` (VectorsOps.h) -- strictly
				// looser than this branch's `dShadingNormalWorldMag > NEARZERO`
				// (1e-12) gate.  So for a magnitude in the open interval
				// (0, 1e-12], NormalizeMag's guard passes and `vNormal` (set
				// just above this block, from the same `NormalizeMag` call) IS
				// normalized to a unit -- but ill-conditioned-direction --
				// vector; it is NOT "left un-normalized, still ~0".  Only an
				// EXACTLY-zero transformed normal leaves vNormal at zero.
				// Either way the shading frame is unusable in this regime
				// (a unit vector pointing in a direction dominated by FP
				// noise is exactly as degenerate for shading purposes as a
				// literal zero vector), so the conclusion is unchanged: mark
				// the derivatives invalid rather than hand a consumer a
				// garbage curvature.
				ri.geometric.derivatives.valid = false;
			}
		}

		// WORLD-MEASURE FOLD for the two curvature-facing scalars
		// (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md 5.2).  DELIBERATELY OUTSIDE
		// the `derivatives.valid` gate above: the SDF family sets
		// `curvatureValid` while leaving `valid` false (an implicit surface has
		// no natural (u,v) for dndu/dndv), so gating this on `valid` would
		// leave every SDF hit reporting OBJECT-space curvature.
		//
		//  - scaleHint is a LENGTH:    multiply by |det M|^(1/3).
		//  - curvature is a 1/LENGTH:  divide by the same factor.
		//
		// Both are stamped object-space by the geometry, and both are stamped
		// ONLY when SurfaceCurvatureDemand::Any() -- so on a scene whose
		// expressions never mention `curv` this block runs on the defaults
		// (scaleHint 1, curvatureValid false): two predictable branches, no cost.
		//
		// A DEGENERATE transform (m_worldLinearScale == 0, i.e. |det| == 0 -- a
		// collapsed axis) has no world length to fold into, and dividing the
		// curvature by it would give Inf.  Mark the direct curvature invalid
		// (the honest-absence path, matching the dndu/dndv guard above) and
		// leave scaleHint at its object-space value rather than zeroing it: an
		// object-space characteristic length is still a better normalizer for
		// the `curv` fallback than 0 would be.
		if( m_worldLinearScale > Scalar( 0 ) ) {
			ri.geometric.derivatives.scaleHint *= m_worldLinearScale;
			if( ri.geometric.derivatives.curvatureValid ) {
				ri.geometric.derivatives.curvature /= m_worldLinearScale;
			}
		} else {
			ri.geometric.derivatives.curvatureValid = false;
		}

		// WORLD-MEASURE PROMOTION for txFootprint -- same job as the
		// scaleHint LENGTH fold immediately above (the footprint is stamped
		// from ri.ray, which mid-IntersectRay is still the OBJECT-space ray
		// this function transformed on entry, so it is an object-space
		// measure until promoted here), but by a strictly better operator.
		//
		// Apply this object's FORWARD linear map to the pixel-step VECTORS
		// and re-derive the width from the transformed pair.  This is EXACT
		// for any linear map -- non-uniform scale and shear included --
		// because the map is affine: it carries the object-space auxiliary
		// ray's line onto the world auxiliary ray's line and the
		// object-space tangent plane onto the world tangent plane, so the
		// line∩plane point commutes with the map and dpdx^world =
		// M_linear · dpdx^obj identically.  No determinant, no
		// degenerate-transform special case beyond what Transform already
		// does: a collapsed axis simply yields a shorter (possibly zero)
		// world footprint, which is the truth.
		//
		// This REPLACED a `worldWidth *= |det M|^(1/3)` geometric-mean fold
		// (relief-modifier fix round 2, P2-A), which was exact only under a
		// uniform scale and under-counted a `scale 4 0.05 4` panel viewed
		// face-on by 4.31x (in-plane scale 4, |det|^(1/3) = 0.9283).  Full
		// argument and the measurement:
		// docs/TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md §3.4.
		//
		// `m_worldLinearScale` is deliberately NOT read here any more; it
		// keeps its other two jobs (scaleHint / curvature, above).
		if( ri.geometric.txFootprint.widthValid ) {
			// OBJECT-SPACE WIDTH, captured before the promotion
			// overwrites it (2026-09-06).  `worldWidth` at THIS instant
			// is the footprint measured in this object's own object
			// space -- the very frame `ptObjIntersec` (stamped a few
			// lines below, from the same object-space ray) is written
			// in, and therefore the frame the expression VM's `Po`
			// lives in.  Capturing it here rather than deriving it from
			// a scale factor is what makes `fbm(Po*k, ...)` filter
			// correctly under ANY linear map, non-uniform scale and
			// shear included: it is the same exact-for-affine-maps
			// argument the promotion below rests on, read one step
			// earlier.  See TextureFootprint::objectWidth's own doc
			// comment for why this is a capture and not a division.
			ri.geometric.txFootprint.objectWidth = ri.geometric.txFootprint.worldWidth;

			const Vector3 dx = Vector3Ops::Transform( m_mxFinalTrans, ri.geometric.txFootprint.dpdx );
			const Vector3 dy = Vector3Ops::Transform( m_mxFinalTrans, ri.geometric.txFootprint.dpdy );
			ri.geometric.txFootprint.dpdx = dx;
			ri.geometric.txFootprint.dpdy = dy;
			ri.geometric.txFootprint.worldWidth =
				Scalar(0.5) * ( Vector3Ops::Magnitude( dx ) + Vector3Ops::Magnitude( dy ) );
		}

		// Wireframe view-mode closest-edge point transforms like a
		// position (forward transform) -- exactly as ptIntersection.
		if( ri.geometric.bHasWireEdgeInfo ) {
			ri.geometric.ptWireNearestEdge = Point3Ops::Transform(
				m_mxFinalTrans, ri.geometric.ptWireNearestEdge );
		}

		if( bComputeExitInfo ) {
			ri.geometric.vNormal2 = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvTranspose, ri.geometric.vNormal2 ) );
			ri.geometric.vGeomNormal2 = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvTranspose, ri.geometric.vGeomNormal2 ) );
			ri.geometric.ptObjExit = ri.geometric.ray.PointAtLength( ri.geometric.range2 + SURFACE_INTERSEC_ERROR );
			ri.geometric.ptExit = Point3Ops::Transform( m_mxFinalTrans, ri.geometric.ptObjExit );

			if( ri.geometric.range2 != 0 ) {
				ri.geometric.range2 = Vector3Ops::Magnitude( Vector3Ops::mkVector3( ri.geometric.ptExit, orig.origin ) );
			}
		}

		// Tell which modifier
		ri.pModifier = pModifier;

		// Tell the modifier how to express a WORLD-space step in the frame
		// `ptObjIntersec` (stamped a few lines below) is written in.  For a
		// plain object that frame IS this object's own object space, so the
		// map is exactly m_mxInvFinalTrans -- the inverse of the
		// m_mxFinalTrans that produces ptIntersection from ptObjIntersec.
		// A borrowed pointer into a member of the object that is about to
		// become ri.pObject; the scene is immutable during a render.  See
		// the field's doc comment in RayIntersectionGeometric.h for the
		// direction-vs-point rule and for why CSGObject clears it instead.
		ri.geometric.pmxWorldToObject = &m_mxInvFinalTrans;

		// Tell which material
		ri.pMaterial = pMaterial;

		// Tell which shader
		ri.pShader = pShader;

		// Tell which radiance map
		ri.pRadianceMap = pRadianceMap;

		// Compute the intersection in world space
		ri.geometric.ptObjIntersec = ri.geometric.ray.PointAtLength( ri.geometric.range - SURFACE_INTERSEC_ERROR );
		ri.geometric.ptIntersection = Point3Ops::Transform( m_mxFinalTrans, ri.geometric.ptObjIntersec );
		ri.geometric.range = Vector3Ops::Magnitude( Vector3Ops::mkVector3( ri.geometric.ptIntersection, orig.origin ) );

		ri.pObject = this;
	}

	// Restore the old ray
	ri.geometric.ray = orig;
}

bool Object::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	// NULL-GEOMETRY GUARD: see getBoundingBox()'s comment above.  Reachable as
	// of 87 for a CONTAINER node; the world-visible + casts-shadows gate in
	// ObjectManager::RayElementIntersection_IntersectionOnly keeps shadow rays
	// away from one.  No intersection.
	if( !pGeometry ) {
		return false;
	}

	// Bring the ray into our frame, but use our own copy
	Ray		orig = ray;

	orig.origin = Point3Ops::Transform( m_mxInvFinalTrans, ray.origin );

	// Capture the UNNORMALIZED transformed direction's magnitude before
	// normalizing it into the local-frame ray -- the direction-true
	// world-to-local distance factor used below (P1 fix, mirrors
	// Object::IntersectRay above).
	const Vector3 dirLocalUnnorm = Vector3Ops::Transform( m_mxInvFinalTrans, ray.Dir() );
	const Scalar dirLocalMag = Vector3Ops::Magnitude( dirLocalUnnorm );
	orig.SetDir( Vector3Ops::Normalize( dirLocalUnnorm ) );

	// factor converts a WORLD-frame distance limit (dHowFar) into the
	// local-frame traversal limit.  MUST be the magnitude of the
	// TRANSFORMED RAY DIRECTION (dirLocalMag, captured above) -- NOT an
	// arbitrary +X-axis probe (P1 fix); see Object::IntersectRay above for
	// the full rationale.  Guard a degenerate transform that collapses
	// this direction to ~0 by falling back to an unscaled factor of 1.0.
	const Scalar factor = (dirLocalMag > NEARZERO) ? dirLocalMag : Scalar(1.0);
	Scalar dHowFar2 = dHowFar;

	// We can't go farther than infinity, so in this case only reduce the
	// length, never extend -- see Object::IntersectRay above for why this
	// guard is about the RISE_INFINITY sentinel's magnitude and stays
	// correct regardless of how `factor` is derived.
	if( (dHowFar != RISE_INFINITY) || (factor < 1.0) ) {
		dHowFar2 = factor*dHowFar;
	}

	// Do bounding box check first
	if( pGeometry->DoPreHitTest() ) {
		// Compute ray intersection with box
		BOX_HIT		hit;
		BoundingBox	bbox = pGeometry->GenerateBoundingBox();
		RayBoxIntersection( orig, hit, bbox.ll, bbox.ur );
		if( !hit.bHit ) {
			return false;
		}

		if( hit.dRange > dHowFar2 ) {
			// If we are in the box, this is not a valid test...
			if( !GeometricUtilities::IsPointInsideBox( orig.origin, bbox.ll, bbox.ur ) ) {
				return false;
			}
		}
	}

	return pGeometry->IntersectRay_IntersectionOnly( orig, dHowFar2, bHitFrontFaces, bHitBackFaces );
}

void Object::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	// NULL-GEOMETRY GUARD (2026-07-31 fix round 2, caller list corrected
	// fix round 3; 87: a geometry-less CONTAINER node is now a second source
	// of a null pGeometry, and it reaches these callers no more than a
	// CSGObject does -- a container is world-INVISIBLE, so it never lands on
	// the luminaries list or in any world-visible scan): pGeometry is null
	// for a CSGObject (see GetArea()'s doc comment above).  Every known caller of UniformRandomPoint on an
	// IObject now refuses to reach here with a null pGeometry:
	// LightSampler (safe by LUMINARIES-LIST MEMBERSHIP -- see GetArea()'s
	// doc comment's class (2) argument, not a local check);
	// SubSurfaceScatteringShaderOp / DonnerJensenSkinSSSShaderOp via their
	// own CanBeAreaLight-or-null gate (local check, fix round 1);
	// ManifoldSolver's SpecularCasterCollector via its own GetGeometry()
	// gate (local check, fix round 1); and ManifoldSolver's
	// surfaceSampleReflectionFallback k=1-mirror gate (local check, fix
	// round 3 -- P1a: this call site was MISSED in rounds 1-2 because its
	// object comes from a live ray hit, not the pre-filtered
	// mSpecularCasters cache).  This is a belt-and-suspenders base-layer
	// guard, not a path exercised in the audited call graph.  Returns a
	// DEFINED (not garbage / NaN) fallback rather than crashing: this
	// object's own local origin transformed to world space, a canonical
	// world +Y normal (transformed the same way UniformRandomPoint's
	// normal always is, below), and a zero UV.  This is deliberately NOT a
	// "sampling contract" value (no surface exists to sample uniformly).
	//
	// P2a (fix round 3, Opus review): unlike GetArea()'s silent 0 (0 is
	// the established "not sampleable" signal every caller already reads
	// correctly), a wrong-POSITION fallback point is not self-announcing
	// to a caller that forgets its own gate -- it looks like a valid
	// sample.  Warn once per process (SplatFilm.cpp's EvaluateFilter-
	// support log-once idiom) so a future regression is loud, not silent.
	if( !pGeometry ) {
		static std::atomic<bool> warnedNullGeometryFallback{ false };
		bool expected = false;
		if( warnedNullGeometryFallback.compare_exchange_strong( expected, true ) ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"Object::UniformRandomPoint:: called on an object with no directly-owned "
				"geometry (e.g. a csg_object) -- every known caller gates this away, so "
				"reaching here means a caller is missing its null-geometry check.  "
				"Returning a FABRICATED fallback point (object origin, +Y normal) rather "
				"than crashing -- this is NOT a valid uniform surface sample; fix the "
				"calling site's gate." );
		}
		if( point )  *point  = Point3Ops::Transform( m_mxFinalTrans, Point3( 0, 0, 0 ) );
		if( normal ) *normal = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxFinalTrans, Vector3( 0, 1, 0 ) ) );
		if( coord )  *coord  = Point2( 0, 0 );
		return;
	}

	pGeometry->UniformRandomPoint( point, normal, coord, prand );

	if( point ) {
		*point = Point3Ops::Transform( m_mxFinalTrans, (*point) );
	}

	if( normal ) {
		*normal = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxFinalTrans, (*normal) ));
	}
}

Scalar Object::GetArea( ) const
{
	// NULL-GEOMETRY GUARD (2026-07-31 fix round 2): pGeometry is null for a
	// CSGObject (its shape is synthesized from two operand objects rather
	// than owned directly -- see CSGObject.h/.cpp, which overrides
	// IntersectRay/getBoundingBox but NOT GetArea()) and, as of 87, for a
	// geometry-less CONTAINER node.  Returns 0 -- an area
	// of zero is the same "cannot be uniformly area-sampled" signal every
	// caller already checks for CanBeAreaLight()==false: `area > 0` gates
	// all of them, so 0 flows through as "not sampleable" without a
	// division anywhere reading pGeometry again.
	//
	// P3 CORRECTION (fix round 3): "every known call site ALSO now
	// null-checks GetGeometry() before calling this" OVERSTATED it -- the
	// call sites split into two DIFFERENT safety arguments, not one:
	//   (1) LOCAL CHECK immediately before the call: EmissionShaderOp.cpp,
	//       PathTracingIntegrator.cpp, BDPTIntegrator.cpp, VCMIntegrator.cpp
	//       (all fix-round-2), and SubSurfaceScatteringShaderOp.cpp /
	//       DonnerJensenSkinSSSShaderOp.cpp (fix-round-1) -- each reads
	//       GetGeometry() and gates on it right there.
	//   (2) LUMINARIES-LIST MEMBERSHIP, not a local check: LightSampler.cpp
	//       (4 call sites, `lumEntry.pLum->GetArea()`), PhotonTracer.h,
	//       SpectralPhotonTracer.h, SMSPhotonMap.cpp -- none of these
	//       re-check GetGeometry() at the call site; they are safe because
	//       `pLum`/the luminary they iterate can ONLY be an object that
	//       already passed LuminaryManager::AddToLuminaryList's null-
	//       geometry gate to get onto the luminaries list in the first
	//       place.  A null-geometry object never reaches these loops at
	//       all -- the safety is upstream admission control, not a
	//       per-call re-verification.
	// This base-layer guard is what makes class (2) actually safe (absent
	// it, membership alone wouldn't help if some OTHER path ever mutated
	// or bypassed the luminaries list) and is belt-and-suspenders for
	// class (1); it exists so a future caller that forgets its own check
	// degrades to "zero area" instead of a null-deref.
	if( !pGeometry ) {
		return Scalar( 0 );
	}

	// WORLD-AREA JACOBIAN (2026-08-13): pGeometry->GetArea() is the
	// OBJECT-space surface area, but UniformRandomPoint() returns points
	// transformed through m_mxFinalTrans -- so every consumer that claims
	// pdfPosition = 1/GetArea() (LightSampler NEE, BDPT/VCM InitLight,
	// photon-emission power normalization, SSS dipole sampling) needs the
	// WORLD-space area or the claimed density is wrong by the transform's
	// area scaling (a `scale 2` emitter previously lit its surroundings at
	// 1/4 the correct NEE energy; measured 0.39x after MIS mixing).
	//
	// m_worldAreaScale = |det(linear part)|^(2/3), cached by
	// FinalizeTransformations(): EXACT for rotations, reflections, and
	// uniform scales (the overwhelmingly common case: det = s^3, area
	// scale = s^2).  For non-uniform scale or shear it is the geometric-
	// mean approximation -- the true area factor varies across the surface
	// with the local normal, and object-space-uniform sampling is then not
	// world-uniform either, so the residual error there sits in the
	// sampler, not just this scalar (LuminaryManager warns once when a
	// non-uniformly-scaled luminaire is admitted).  Exactness needs
	// per-geometry integration; not attempted here.
	//
	// RISE_INFINITY guard: InfinitePlaneGeometry::GetArea() returns the
	// DBL_MAX sentinel; multiplying it by any factor > 1 (a scale, or a
	// rotation whose determinant lands at 1+1ulp) would overflow to +inf,
	// which turns the light-selection alias table's TotalWeight into inf
	// and every pdfSelect into NaN.  Pass the sentinel through untouched.
	const Scalar objArea = pGeometry->GetArea();
	if( objArea <= 0 || objArea >= RISE_INFINITY ) {
		return objArea;
	}
	return objArea * m_worldAreaScale;
}

bool Object::DistanceToSurface( const Point3& ptWorld, const Scalar maxDistWorld, Scalar& outDist ) const
{
	if( !pGeometry ) {
		return false;
	}
	// A degenerate transform refuses outright.  Matrix4Ops::Inverse returns
	// its INPUT unchanged for a singular matrix rather than signalling, so
	// the object-space point below would be a silently wrong number rather
	// than an obviously wrong one -- and a wrong point here reads as
	// contact where there is none, the one direction this signal must never
	// fail in.
	if( !( m_sigmaMin > Scalar( 0 ) ) || !( m_sigmaMax > Scalar( 0 ) ) ) {
		return false;
	}

	// THE RADIUS GOES IN DIVIDED BY THE SMALLEST singular value.  A
	// candidate within world distance `r` has object-space distance
	// `d_o <= d_w / sigmaMin <= r / sigmaMin`, so searching that far in
	// object space cannot MISS a neighbour that is within `r` in world.
	// Clamped rather than allowed to overflow: a hugely loose radius only
	// costs time, but an infinity handed to a geometry's own budget
	// arithmetic is a NaN waiting to happen.
	Scalar maxDistObject = maxDistWorld / m_sigmaMin;
	if( !RISE::IsFiniteDouble( static_cast<double>( maxDistObject ) ) || maxDistObject > RISE_INFINITY ) {
		maxDistObject = RISE_INFINITY;
	}

	const Point3 ptObject = Point3Ops::Transform( m_mxInvFinalTrans, ptWorld );

	Scalar dObject = Scalar( 0 );
	if( !pGeometry->DistanceToSurface( ptObject, maxDistObject, dObject ) ) {
		return false;
	}
	// A geometry that answers with a negative or non-finite number has
	// broken its own contract; treat it as a refusal rather than letting it
	// reach the clamp in SurfaceSignalInfo::Proximity as a bogus 1.
	if( !RISE::IsFiniteDouble( static_cast<double>( dObject ) ) || dObject < Scalar( 0 ) ) {
		return false;
	}

	// AND THE ANSWER COMES OUT MULTIPLIED BY THE LARGEST, which is the safe
	// direction: `d_w <= sigmaMax * d_o`, so the reported world distance is
	// an upper bound on the true one and `proximity` can only under-paint.
	Scalar dWorld = dObject * m_sigmaMax;
	if( !RISE::IsFiniteDouble( static_cast<double>( dWorld ) ) ) {
		return false;
	}

	// ONE-SHOT, and won by exactly one thread: `exchange` is what makes
	// "once" true when every render thread is inside this function at the
	// same time.  The fast path is the relaxed load in front of it, so an
	// anisotropic object costs one load per candidate per hit after the
	// first, not a read-modify-write.
	if( !m_sigmaExact
	 && !m_sigmaLooseWarned.load( std::memory_order_relaxed )
	 && !m_sigmaLooseWarned.exchange( true, std::memory_order_relaxed ) ) {
		// No name to quote: an Object carries none -- the manager owns the
		// name-to-object map, and reaching back for it from here would
		// invert that ownership.  The sigma pair identifies the transform
		// well enough for an author to find the chunk that authored it.
		//
		// AND IT NAMES THE STATE, which is the whole reason `m_sigmaSource`
		// replaced a bool (design 5.6).  `Jacobi` and `Loose` are both
		// "not exact" and they are not the same news: a converged object's
		// printed ratio IS the transform's true singular ratio (7.5 on
		// `scale (3, 1, 0.4)`), while a fallen-back one's is a bound on a
		// bound (26.99 on the same transform) and reads alarmingly for no
		// reason an author can act on.  A single `!m_sigmaExact` message
		// could not tell them apart, so it described every object as if it
		// were the second.
		//
		// The search-radius inflation `1/sigmaMin` is printed as its own
		// number because it is the part that costs TIME rather than
		// contact -- 2.5x on that transform since Phase 3, 8.47x before.
		const char* const sourceWord =
			( m_sigmaSource == SigmaSource::Jacobi )
				? "EXACT singular values (one-sided Jacobi, widened four ulps)"
				: "the LOOSE Frobenius/determinant pair (the Jacobi sweep cap was hit)";
		GlobalLog()->PrintEx( eLog_Info,
			"Object::DistanceToSurface:: an object with an ANISOTROPIC transform reads proximity() "
			"through %s.  Reported distances are an UPPER bound, over-read by at most %.3gx "
			"(sigmaMax %.6g / sigmaMin %.6g) -- attained only along the top singular vector, so the "
			"over-report an author measures is typically smaller; and the object-space search "
			"radius is inflated %.3gx, which is the cost side.  The signal can only UNDER-paint "
			"contact as a result; see docs/CROSS_OBJECT_PROXIMITY_DESIGN.md 5.2 and 5.6.",
			sourceWord,
			(double)( m_sigmaMax / m_sigmaMin ), (double)m_sigmaMax, (double)m_sigmaMin,
			(double)( Scalar( 1 ) / m_sigmaMin ) );
	}

	outDist = dWorld;
	return true;
}

//! IObject::SignedDistanceLower -- the same transform layer, with the ONE
//! conversion reversed (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §5.6).
//!
//! Point IN through the inverse, radius IN divided by sigmaMin -- both
//! identical to the unsigned query.  The MAGNITUDE comes back multiplied
//! by sigmaMin rather than sigmaMax, because this query owes a LOWER
//! bound: `d_w >= sigmaMin * d_o` is the inequality that direction needs,
//! and `sigmaMax` here would over-read a depth (an `interior` that paints
//! where nothing is buried) and would let a CSG descent step overshoot the
//! zero set.  The SIGN is untouched by a positive scaling, so it survives
//! the conversion exactly.
//!
//! NO RANGE REFUSAL.  Unlike the unsigned query, `maxDistWorld` is an
//! effort budget that is passed down and never used to reject an answer --
//! a composite's descent asks its operands about points well outside any
//! radius, and a range refusal there would break every intersection with a
//! small subtrahend.
//!
//! THE EXACTNESS FLAG SURVIVES ONLY UNDER A SIMILARITY.  `m_sigmaExact`
//! marks the fast path where `M^T M = s^2 I`, i.e. sigmaMin == sigmaMax ==
//! s; there `x sigmaMin` IS the isometric-up-to-scale image of the
//! object-space distance and the magnitude stays exact.  Under anything
//! anisotropic `x sigmaMin` is a strict under-read attained only along the
//! bottom singular vector, so the flag is dropped -- which is what makes a
//! composite reaching a `scale (3, 1, 0.4)` box take the STRICT arm.
bool Object::SignedDistanceLower( const Point3& ptWorld, const Scalar maxDistWorld,
	Scalar& outSigned, bool& outExact ) const
{
	outExact = false;
	if( !pGeometry ) {
		return false;
	}
	// A degenerate transform refuses outright -- Matrix4Ops::Inverse
	// returns its INPUT unchanged for a singular matrix, so the
	// object-space point would be a silently wrong number.
	if( !( m_sigmaMin > Scalar( 0 ) ) || !( m_sigmaMax > Scalar( 0 ) ) ) {
		return false;
	}

	Scalar maxDistObject = maxDistWorld / m_sigmaMin;
	if( !RISE::IsFiniteDouble( static_cast<double>( maxDistObject ) ) || maxDistObject > RISE_INFINITY ) {
		maxDistObject = RISE_INFINITY;
	}

	const Point3 ptObject = Point3Ops::Transform( m_mxInvFinalTrans, ptWorld );

	Scalar fObject = Scalar( 0 );
	bool   geomExact = false;
	if( !pGeometry->SignedDistanceLower( ptObject, maxDistObject, fObject, geomExact ) ) {
		return false;
	}
	if( !RISE::IsFiniteDouble( static_cast<double>( fObject ) ) ) {
		return false;
	}

	const Scalar fWorld = fObject * m_sigmaMin;
	if( !RISE::IsFiniteDouble( static_cast<double>( fWorld ) ) ) {
		return false;
	}

	outSigned = fWorld;
	outExact  = geomExact && m_sigmaExact;
	return true;
}

void Object::Realize() const
{
	if( pGeometry ) {
		pGeometry->Realize();
	}
}

void Object::ResetRuntimeData() const
{
	if( pShader ) {
		pShader->ResetRuntimeData();
	}
}

void Object::FinalizeTransformations( const Matrix4& parentWorld )
{
	Transformable::FinalizeTransformations( parentWorld );

	// Everything below is derived from m_mxFinalTrans, which is now the
	// COMPOSED world matrix `parentWorld * local`.  That is what makes the
	// three caches correct under hierarchy by construction rather than by a
	// separate recompute pass: there is one finalize, and it is this one.
	m_mxInvTranspose = Matrix4Ops::Transpose( m_mxInvFinalTrans );

	// Sign of the chirality flip the world-space transform applies to
	// the tangent frame.  For an affine object transform the 4D
	// determinant equals the upper-3x3 determinant; <0 means the
	// transform reverses orientation (e.g. `scale -1 1 1` or any
	// reflection / mirror), and the imported TANGENT.w needs to be
	// negated at hit time so cross(N_world, T_world) * w still gives
	// the bitangent that's consistent with the mirrored surface.
	// See Object::IntersectRay where this is multiplied into
	// ri.geometric.bitangentSign.
	const Scalar det = Matrix4Ops::Determinant( m_mxFinalTrans );
	m_tangentFrameSign = (det < Scalar( 0 )) ? Scalar( -1 ) : Scalar( 1 );

	// World-area scaling of the linear part, cached here (the transform is
	// immutable during render) so the hot GetArea() path is a single
	// multiply.  |det|^(2/3) is EXACT for rotations / reflections / uniform
	// scales; for non-uniform scale or shear it is the geometric-mean
	// approximation (see GetArea()'s comment).  Degenerate transform → 0,
	// matching the "cannot area-sample" sentinel convention.
	const Scalar absDet = fabs( det );
	m_worldAreaScale = (absDet > Scalar( 0 ))
		? pow( absDet, Scalar( 2.0 / 3.0 ) )
		: Scalar( 0 );

	// World-LINEAR scaling, |det|^(1/3) -- the length-measure sibling of the
	// area Jacobian just above, cached for the same reason (the transform is
	// immutable during render, and the hit path should be a single multiply).
	// Consumed by the derivatives block in IntersectRay to put
	// `derivatives.scaleHint` (a length) and `derivatives.curvature` (a
	// 1/length) into WORLD measure.  Same degenerate-transform sentinel: 0.
	m_worldLinearScale = (absDet > Scalar( 0 ))
		? pow( absDet, Scalar( 1.0 / 3.0 ) )
		: Scalar( 0 );

	// EXTREMAL SINGULAR VALUES of the upper 3x3 -- see the fields' doc
	// comment in Object.h for what the proximity query does with both ends,
	// and `ComputeSigmaExtremes` for the three branches that can produce
	// them.  HOISTED into that free routine (Phase 3) rather than inlined
	// here, because the `Loose` fallback is unreachable from any real
	// transform and a test that cannot call the routine directly cannot
	// cover it at all.
	//
	// THIRTY SWEEPS is the production cap.  One-sided Jacobi on a 3x3
	// converges quadratically and finishes in a handful; thirty is a
	// runaway guard, not a tuning knob, and hitting it is what the `Loose`
	// state exists to report.
	if( !ComputeSigmaExtremes( m_mxFinalTrans, 30, m_sigmaMin, m_sigmaMax, m_sigmaSource ) ) {
		// DEGENERATE (or non-finite): there is no invertible map to
		// measure, and Matrix4Ops::Inverse silently returns its input for a
		// singular matrix, so the object-space point would be meaningless
		// too.  Zero is the "cannot answer" sentinel the other two caches
		// already use, and both distance queries read it as a refusal.
		m_sigmaMax   = Scalar( 0 );
		m_sigmaMin   = Scalar( 0 );
		m_sigmaSource = SigmaSource::Loose;
	}
	// DERIVED, at the one site that assigns the source -- never written
	// anywhere else, so the two cannot drift.
	m_sigmaExact = ( m_sigmaSource == SigmaSource::Exact );
	// The loose-bound diagnostic latch is deliberately NOT re-armed here,
	// matching the REFUSAL latch just below it.  This used to re-arm on
	// every FinalizeTransformations -- which fires once per animation frame
	// and once per hierarchy re-bake for a parented or keyframed object, so
	// an anisotropically-scaled object under a live parent/keyframe pass
	// logged the warning every frame, not "once per object" as documented
	// (Object.h's own comment on `m_sigmaLooseWarned`) and promised by the
	// design (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §5.2).  A transform that
	// toggles between exact and loose across the object's lifetime (e.g. an
	// animated non-uniform scale that passes through a uniform pose) is
	// reported at most once, on its first loose pose, for the life of the
	// object -- the same one-shot lifetime the refusal latch already uses,
	// and the quieter direction: an author who has seen the warning once
	// does not need it repeated every frame.
	// The REFUSAL latch is deliberately NOT re-armed here.  The promise is
	// "once per refusing object", not "once per pose": the dominant refusal
	// is a property of the geometry FAMILY (a patch, a RAW mesh, a
	// heightfield SDF, a CSG composite) that no transform change can turn
	// into an answering one, and re-arming would reprint the same line on
	// every animation frame.  A transform that becomes degenerate mid-
	// animation is therefore reported only if it had not already refused --
	// the quieter direction, chosen deliberately.
}
