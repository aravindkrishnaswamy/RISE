//////////////////////////////////////////////////////////////////////
//
//  Transformable.cpp - Implements transform functions
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
#include "Transformable.h"
#include "../Animation/KeyframableHelper.h"
#include "../Interfaces/ILog.h"
#include "FiniteMath.h"

#include <map>
#include <mutex>

using namespace RISE;
using namespace RISE::Implementation;

namespace {

Vector3 LeastAlignedCardinal_( const Vector3& unit )
{
	const Scalar ax = std::fabs( unit.x );
	const Scalar ay = std::fabs( unit.y );
	const Scalar az = std::fabs( unit.z );
	return ax <= ay && ax <= az ? Vector3( 1, 0, 0 )
	     : ay <= az             ? Vector3( 0, 1, 0 )
	                            : Vector3( 0, 0, 1 );
}

Vector3 PerpendicularUnit_( const Vector3& unit )
{
	return Vector3Ops::Normalize( Vector3Ops::Cross( unit, LeastAlignedCardinal_( unit ) ) );
}

// Split an arbitrary affine linear transform into a proper orthonormal frame
// and the residual scale/shear/reflection.  The construction deliberately
// repairs collapsed columns so absolute orientation/scale edits remain usable
// for singular matrices authored by scenes.
void DecomposeFinalAffine_( const Matrix4& transform, Matrix4& rotation, Matrix4& residual )
{
	const Vector3 c0( transform._00, transform._01, transform._02 );
	const Vector3 c1( transform._10, transform._11, transform._12 );
	const Vector3 c2( transform._20, transform._21, transform._22 );
	const Scalar l0 = Vector3Ops::Magnitude( c0 );
	const Scalar l1 = Vector3Ops::Magnitude( c1 );
	const Scalar l2 = Vector3Ops::Magnitude( c2 );

	Vector3 x;
	if( l0 > 0 ) {
		x = Vector3Ops::Normalize( c0 );
	} else if( l1 > 0 && l2 > 0 && Vector3Ops::Magnitude( Vector3Ops::Cross( c1, c2 ) ) > 0 ) {
		x = Vector3Ops::Normalize( Vector3Ops::Cross( c1, c2 ) );
	} else if( l1 > 0 ) {
		x = PerpendicularUnit_( Vector3Ops::Normalize( c1 ) );
	} else if( l2 > 0 ) {
		x = PerpendicularUnit_( Vector3Ops::Normalize( c2 ) );
	} else {
		x = Vector3( 1, 0, 0 );
	}

	Vector3 yRaw = c1 - x * Vector3Ops::Dot( x, c1 );
	Vector3 y;
	if( Vector3Ops::Magnitude( yRaw ) > 0 ) {
		y = Vector3Ops::Normalize( yRaw );
	} else if( l2 > 0 && Vector3Ops::Magnitude( Vector3Ops::Cross( c2, x ) ) > 0 ) {
		y = Vector3Ops::Normalize( Vector3Ops::Cross( c2, x ) );
	} else {
		y = PerpendicularUnit_( x );
	}
	const Vector3 z = Vector3Ops::Normalize( Vector3Ops::Cross( x, y ) );
	y = Vector3Ops::Normalize( Vector3Ops::Cross( z, x ) );

	rotation = Matrix4Ops::Identity();
	rotation._00 = x.x; rotation._01 = x.y; rotation._02 = x.z;
	rotation._10 = y.x; rotation._11 = y.y; rotation._12 = y.z;
	rotation._20 = z.x; rotation._21 = z.y; rotation._22 = z.z;

	Matrix4 linear = transform;
	linear._30 = linear._31 = linear._32 = 0;
	linear._03 = linear._13 = linear._23 = 0;
	linear._33 = 1;
	residual = Matrix4Ops::Inverse( rotation ) * linear;
}

Scalar ColumnLength_( const Matrix4& matrix, const int column )
{
	const Scalar* values = &matrix._00 + column * 4;
	return std::sqrt( values[0] * values[0] + values[1] * values[1] + values[2] * values[2] );
}

Matrix4 ScaleFreeAffineBase_( const Matrix4& current )
{
	Matrix4 result = current;
	Matrix4 rotation, residual;
	DecomposeFinalAffine_( current, rotation, residual );
	Scalar* columns[3] = { &result._00, &result._10, &result._20 };
	const Scalar* frameColumns[3] = { &rotation._00, &rotation._10, &rotation._20 };
	for( int column = 0; column < 3; ++column ) {
		const Scalar length = ColumnLength_( current, column );
		if( length > 0 ) {
			columns[column][0] /= length;
			columns[column][1] /= length;
			columns[column][2] /= length;
		} else {
			columns[column][0] = frameColumns[column][0];
			columns[column][1] = frameColumns[column][1];
			columns[column][2] = frameColumns[column][2];
		}
	}
	return result;
}

struct FinalMatrixMetadata
{
	bool active;
	Matrix4 scaleBase;
	Vector3 appliedStretch;
	bool scaleBaseValid;
	size_t authoritativeIndex;

	FinalMatrixMetadata() :
		active( false ),
		scaleBase( Matrix4Ops::Identity() ),
		appliedStretch( 1, 1, 1 ),
		scaleBaseValid( false ),
		authoritativeIndex( 0 )
	{}
};

struct FinalMetadataRegistry
{
	std::mutex mutex;
	std::map<const Transformable*, FinalMatrixMetadata> entries;
};

// Process-lifetime sidecar avoids both public Transformable layout changes and
// static-destruction-order hazards: an externally-owned static Transformable
// may be destroyed after this translation unit's ordinary globals.
FinalMetadataRegistry& FinalMetadataRegistry_()
{
	static FinalMetadataRegistry* registry = new FinalMetadataRegistry();
	return *registry;
}

bool ReadFinalMetadata_( const Transformable* transformable, FinalMatrixMetadata& metadata )
{
	FinalMetadataRegistry& registry = FinalMetadataRegistry_();
	std::lock_guard<std::mutex> lock( registry.mutex );
	const std::map<const Transformable*, FinalMatrixMetadata>::const_iterator found =
		registry.entries.find( transformable );
	if( found == registry.entries.end() ) return false;
	metadata = found->second;
	return metadata.active;
}

void StoreFinalMetadata_( const Transformable* transformable, const FinalMatrixMetadata& metadata )
{
	FinalMetadataRegistry& registry = FinalMetadataRegistry_();
	std::lock_guard<std::mutex> lock( registry.mutex );
	registry.entries[transformable] = metadata;
}

void ClearFinalMetadata_( const Transformable* transformable )
{
	FinalMetadataRegistry& registry = FinalMetadataRegistry_();
	std::lock_guard<std::mutex> lock( registry.mutex );
	registry.entries.erase( transformable );
}

bool MatrixIsFinite_( const Matrix4& matrix )
{
	const Scalar* values = &matrix._00;
	for( int i = 0; i < 16; ++i ) {
		if( !IsFiniteDouble( values[i] ) ) return false;
	}
	return true;
}

bool TransformStateV2IsValid_( const TransformStateV2& state )
{
	// doc 89 slice C: an out-of-range mirror axis fails the WHOLE restore rather
	// than being dropped, so a corrupt snapshot cannot half-apply.
	if( state.mirrorAxis < -1 || state.mirrorAxis > 2 ) return false;
	if( !MatrixIsFinite_( state.transform.position )
	 || !MatrixIsFinite_( state.transform.orientation )
	 || !MatrixIsFinite_( state.transform.scale )
	 || !MatrixIsFinite_( state.transform.stretch )
	 || !MatrixIsFinite_( state.transform.stackProduct ) ) return false;
	for( std::vector<Matrix4>::const_iterator i = state.stackEntries.begin();
		i != state.stackEntries.end(); ++i ) {
		if( !MatrixIsFinite_( *i ) ) return false;
	}
	if( state.finalMatrixOnStack ) {
		if( !state.finalScaleBaseValid
		 || state.authoritativeStackIndex >= state.stackEntries.size()
		 || !MatrixIsFinite_( state.finalScaleBase )
		 || !IsFiniteDouble( state.appliedStretch.x )
		 || !IsFiniteDouble( state.appliedStretch.y )
		 || !IsFiniteDouble( state.appliedStretch.z ) ) return false;
	}
	return true;
}

} // namespace

Transformable::Transformable( ) :
	m_mxFinalTrans( Matrix4Ops::Identity() ),
	m_mxInvFinalTrans( Matrix4Ops::Identity() ),
	m_mxPosition( Matrix4Ops::Identity() ),
	m_mxOrientation( Matrix4Ops::Identity() ),
	m_mxScale( Matrix4Ops::Identity() ),
	m_mxStretch( Matrix4Ops::Identity() ),
	m_mxMirror( Matrix4Ops::Identity() ),
	m_mirrorAxis( -1 ),
	m_mxLocalTrans( Matrix4Ops::Identity() ),
	m_mxParentWorld( Matrix4Ops::Identity() ),
	m_mxParentWorldInv( Matrix4Ops::Identity() ),
	m_bParentWorldInvertible( true )
{
}

Transformable::~Transformable( )
{
	ClearFinalMetadata_( this );
}


void Transformable::PushTopTransStack( const Matrix4& mat )
{
	m_transformstack.push_front( mat );
	FinalMatrixMetadata metadata;
	if( ReadFinalMetadata_( this, metadata ) ) {
		++metadata.authoritativeIndex;
		StoreFinalMetadata_( this, metadata );
	}
}

void Transformable::PushBottomTransStack( const Matrix4& mat )
{
	m_transformstack.push_back( mat );
}

void Transformable::PopTopTransStack( )
{
	if( m_transformstack.empty() ) return;
	FinalMatrixMetadata metadata;
	const bool active = ReadFinalMetadata_( this, metadata );
	if( active && metadata.authoritativeIndex == 0 ) {
		m_transformstack.pop_front();
		ClearFinalMetadata_( this );
		return;
	}
	m_transformstack.pop_front( );
	if( active ) {
		--metadata.authoritativeIndex;
		StoreFinalMetadata_( this, metadata );
	}
}

void Transformable::PopBottomTransStack( )
{
	if( m_transformstack.empty() ) return;
	FinalMatrixMetadata metadata;
	const bool active = ReadFinalMetadata_( this, metadata );
	if( active && metadata.authoritativeIndex + 1 == m_transformstack.size() ) {
		m_transformstack.pop_back();
		ClearFinalMetadata_( this );
		return;
	}
	m_transformstack.pop_back( );
}

void Transformable::ClearAllTransforms( )
{
	// Clears the LOCAL transform only.  m_mxParentWorld is deliberately NOT
	// reset: a node's place in the scene graph is not one of its transforms,
	// and Job::AddObject calls this on every re-apply -- resetting the parent
	// here would silently un-parent every object an incremental edit touches
	// until the next compose walk.
	m_transformstack.clear( );
	m_mxPosition = Matrix4Ops::Identity();
	m_mxOrientation = Matrix4Ops::Identity();
	m_mxScale = Matrix4Ops::Identity();
	m_mxStretch = Matrix4Ops::Identity();
	// doc 89 slice C: the MIRROR is a local transform building block, so it goes
	// too.  Job::AddObject calls this on every re-apply and then re-issues only
	// the params the chunk still carries -- so a mirror left standing here would
	// survive an edit that DELETED the `mirror` line, and the object would keep
	// rendering reflected until a save + reload silently un-reflected it.  That is
	// the same failure `parent` avoids by being re-issued unconditionally.
	m_mxMirror = Matrix4Ops::Identity();
	m_mirrorAxis = -1;
	ClearFinalMetadata_( this );
	// Re-finalize rather than hand-assigning the two matrices: that also
	// refreshes m_mxLocalTrans AND the subclass caches (Object's
	// inverse-transpose / tangent sign / world-area Jacobian) through the
	// virtual one-argument overload, so there is no window in which
	// GetLocalTransformMatrix() or GetArea() still describes the PREVIOUS
	// transform while GetFinalTransformMatrix() describes identity.  87 made
	// the local matrix a first-class read (the CST transform commit uses it),
	// so that window would be a live hazard rather than an academic one.
	FinalizeTransformations();
}

void Transformable::ClearTransformStack( )
{
	m_transformstack.clear( );
	ClearFinalMetadata_( this );
}

void Transformable::TranslateObject( const Vector3& vec )
{
	Matrix4 mx = Matrix4Ops::Translation( vec );
	PushBottomTransStack( mx );
}

void Transformable::RotateObjectXAxis( const Scalar nAmount )
{
	Matrix4 mx = Matrix4Ops::XRotation( nAmount );
	PushBottomTransStack( mx );
}

void Transformable::RotateObjectYAxis( const Scalar nAmount )
{
	Matrix4 mx = Matrix4Ops::YRotation( nAmount );
	PushBottomTransStack( mx );
}

void Transformable::RotateObjectZAxis( const Scalar nAmount )
{
	Matrix4 mx = Matrix4Ops::ZRotation( nAmount );
	PushBottomTransStack( mx );
}

void Transformable::RotateObjectArbAxis( const Vector3& axis, const Scalar nAmount )
{
	Matrix4 mx = Matrix4Ops::Rotation( axis, nAmount );
	PushBottomTransStack( mx );
}

void Transformable::SetPosition( const Point3& pos )
{
	FinalMatrixMetadata metadata;
	if( ReadFinalMetadata_( this, metadata ) ) {
		if( metadata.authoritativeIndex < m_transformstack.size() ) {
			Matrix4& authoritative = m_transformstack[metadata.authoritativeIndex];
			authoritative._30 = pos.x;
			authoritative._31 = pos.y;
			authoritative._32 = pos.z;
			if( metadata.scaleBaseValid ) {
				metadata.scaleBase._30 = pos.x;
				metadata.scaleBase._31 = pos.y;
				metadata.scaleBase._32 = pos.z;
			}
			StoreFinalMetadata_( this, metadata );
			return;
		}
	}
	m_mxPosition = Matrix4Ops::Translation( Vector3( pos.x, pos.y, pos.z ) );
}

void Transformable::SetOrientation( const Vector3& orient )
{
	const Matrix4 orientation = Matrix4Ops::XRotation( orient.x ) *
		Matrix4Ops::YRotation( orient.y ) * 
		Matrix4Ops::ZRotation( orient.z );
	FinalMatrixMetadata metadata;
	if( ReadFinalMetadata_( this, metadata ) ) {
		if( metadata.authoritativeIndex < m_transformstack.size() ) {
			Matrix4& authoritative = m_transformstack[metadata.authoritativeIndex];
			const Matrix4 base = metadata.scaleBaseValid
				? metadata.scaleBase : ScaleFreeAffineBase_( authoritative );
			Matrix4 oldRotation, residual;
			DecomposeFinalAffine_( base, oldRotation, residual );
			metadata.scaleBase = Matrix4Ops::Translation( Vector3(
				authoritative._30, authoritative._31, authoritative._32 ) )
				* orientation * residual;
			metadata.scaleBaseValid = true;
			authoritative = metadata.scaleBase
				* Matrix4Ops::Stretch( metadata.appliedStretch );
			StoreFinalMetadata_( this, metadata );
			return;
		}
	}
	m_mxOrientation = orientation;
}

void Transformable::SetScale( const Scalar nAmount )
{
	FinalMatrixMetadata metadata;
	if( ReadFinalMetadata_( this, metadata ) ) {
		if( !metadata.scaleBaseValid ) {
			const Matrix4 current = CollapsedTransformStack_();
			metadata.scaleBase = ScaleFreeAffineBase_( current );
			metadata.scaleBaseValid = true;
		}
		metadata.appliedStretch = Vector3( nAmount, nAmount, nAmount );
		if( metadata.authoritativeIndex < m_transformstack.size() ) {
			m_transformstack[metadata.authoritativeIndex] =
				metadata.scaleBase * Matrix4Ops::Scale( nAmount );
		} else {
			ReplaceFinalStack_( metadata.scaleBase * Matrix4Ops::Scale( nAmount ) );
			metadata.authoritativeIndex = 0;
		}
		StoreFinalMetadata_( this, metadata );
		return;
	}
	m_mxScale = Matrix4Ops::Scale( nAmount );
}

void Transformable::SetStretch( const Vector3& stretch )
{
	FinalMatrixMetadata metadata;
	if( ReadFinalMetadata_( this, metadata ) ) {
		if( !metadata.scaleBaseValid ) {
			const Matrix4 current = CollapsedTransformStack_();
			metadata.scaleBase = ScaleFreeAffineBase_( current );
			metadata.scaleBaseValid = true;
		}
		metadata.appliedStretch = stretch;
		if( metadata.authoritativeIndex < m_transformstack.size() ) {
			m_transformstack[metadata.authoritativeIndex] =
				metadata.scaleBase * Matrix4Ops::Stretch( stretch );
		} else {
			ReplaceFinalStack_( metadata.scaleBase * Matrix4Ops::Stretch( stretch ) );
			metadata.authoritativeIndex = 0;
		}
		StoreFinalMetadata_( this, metadata );
		return;
	}
	m_mxStretch = Matrix4Ops::Stretch( stretch );
}

void Transformable::SetFinalTransformMatrix( const Matrix4& matrix )
{
	ReplaceFinalStack_( matrix );
	FinalMatrixMetadata metadata;
	metadata.active = true;
	metadata.scaleBase = ScaleFreeAffineBase_( matrix );
	metadata.appliedStretch = Vector3(
		ColumnLength_( matrix, 0 ),
		ColumnLength_( matrix, 1 ),
		ColumnLength_( matrix, 2 ) );
	metadata.scaleBaseValid = true;
	metadata.authoritativeIndex = 0;
	StoreFinalMetadata_( this, metadata );
}

void Transformable::ReplaceFinalStack_( const Matrix4& matrix )
{
	m_transformstack.clear();
	m_transformstack.push_front( matrix );
	m_mxPosition = Matrix4Ops::Identity();
	m_mxOrientation = Matrix4Ops::Identity();
	m_mxScale = Matrix4Ops::Identity();
	m_mxStretch = Matrix4Ops::Identity();
}

bool Transformable::SetMirrorAxis( int axis )
{
	// REFUSED, not clamped: the axis comes from an authored `mirror x|y|z` token,
	// and a value outside the set means the caller mis-decoded it.  Mapping it onto
	// some axis would reflect the object about a plane nobody asked for, which is
	// indistinguishable from a correct render of a different scene.
	if( axis < -1 || axis > 2 ) return false;
	m_mirrorAxis = axis;
	m_mxMirror = Matrix4Ops::Identity();
	// A reflection is its OWN inverse and its determinant is -1.  Object::
	// FinalizeTransformations reads that sign into m_tangentFrameSign (so an
	// imported TANGENT.w flips and tangent-space normal maps stay right) and takes
	// |det| for the world-area Jacobian (so a mirrored EMITTER keeps its true,
	// positive area).  Both were already written for `scale -1 1 1`; this is the
	// same negative-determinant transform arriving from a named param.
	if( axis == 0 )      m_mxMirror._00 = -1;
	else if( axis == 1 ) m_mxMirror._11 = -1;
	else if( axis == 2 ) m_mxMirror._22 = -1;
	return true;
}

void Transformable::FinalizeTransformations( )
{
	// Re-compose against the parent world transform this node was LAST given.
	// Every pre-hierarchy caller (Job::AddObject, the editor's op apply, the
	// animator's RegenerateData) reaches finalize through this overload and
	// keeps a parented node correctly composed without knowing the graph
	// exists.  A root's m_mxParentWorld is identity, so this is byte-identical
	// to the historical implementation for an unparented node.
	FinalizeTransformations( m_mxParentWorld );
}

void Transformable::FinalizeTransformations( const Matrix4& parentWorld )
{
	// ---- LOCAL: this node's own authored transform, no parent contribution.
	// First apply the scale, orientation and position matrices
	// doc 89 slice C appends the MIRROR at the RIGHT (innermost) end:
	// `P * O * Stretch * Scale * M`.  Innermost is what makes `mirror` reflect the
	// SHAPE in its own frame and then place the reflected shape by this node's own
	// transform -- so `mirror x  position 3 0 0` puts a reflected copy at +3, where
	// an outermost reflection would put the un-reflected shape at -3.
	m_mxLocalTrans = m_mxPosition * m_mxOrientation * m_mxStretch * m_mxScale * m_mxMirror;

	// Go through the transformation stack and multiply the transformations...
	TransformStackType::const_iterator		i;
	for( i=m_transformstack.begin(); i<m_transformstack.end(); i++ ) {
		m_mxLocalTrans = (*i) * m_mxLocalTrans;
	}

	// ---- PARENT: stored by VALUE and re-supplied on every call, never pushed
	// onto m_transformstack.  The stack has no self-clearing step, so a pushed
	// parent matrix would compose a second time on the next re-apply and
	// square in another factor without bound -- the 86 §3 bug class.  As an
	// argument it is idempotent: this function always yields exactly
	// `parentWorld * local`, however many times it runs.
	m_mxParentWorld = parentWorld;

	// IS THE PARENT INVERTIBLE, well enough to express a world-space operation
	// in this node's local frame?  FOUR formulations were tried and are wrong.
	// The reasoning is recorded because each of them looks obviously right, and
	// three of them were reached for in successive review rounds.
	//
	// (1) NOT an absolute residual epsilon on |P*P^-1 - I|.  The residual in an
	// affine matrix's translation column grows like ||t||*eps, so a
	// well-conditioned container at `position 1e7 0 0` with any rotation gets
	// rejected while a tiny near-singular linear part sails through.
	//
	// (2) NOT the componentwise backward error |P*P^-1 - I| <= tol*|P|*|P^-1|,
	// though that IS scale-invariant.  Backward error is small BY CONSTRUCTION
	// for the computed inverse of a singular matrix: once the determinant
	// underflows to ~1e-17 instead of exactly 0 -- which is what COMPOSITION
	// does, a flattened grandparent's exact zero becoming a rounding residue
	// two multiplies later -- the adjugate/det inverse has entries ~1e16, the
	// bound becomes ~1e16, and an O(1) residual passes at any tolerance.
	//
	// (3) NOT the Hadamard ratio |det L| / (||c0|| ||c1|| ||c2||).  It measures
	// COLUMN DEPENDENCE, not conditioning: it is invariant under per-axis
	// scaling, the very operation that sends the condition number to infinity.
	// `Rz(45) * diag(1e8, 1e-8, 1)` has ratio 1.000 and condition number 1e16.
	//
	// (4) NOT a condition number built from the adjugate inverse of the RAW
	// linear part either.  For a RANK-1 matrix every 2x2 cofactor is zero in
	// exact arithmetic, so det and every adjugate entry are rounding residue of
	// the SAME order, their quotient is O(1), and the computed condition number
	// comes out ~20 -- noise divided by noise.  Measured: 80% of composed
	// rank-1 parents accepted.  Rank 2 was caught, rank 1 was not.
	//
	// What works is to NORMALISE the linear part first and then ask both
	// questions of the normalised matrix, where every quantity is O(1) and an
	// absolute tolerance is therefore meaningful:
	//
	//   a. IS THE COMPUTED INVERSE ACTUALLY AN INVERSE?  ||Lh*Lh^-1 - I||_F.
	//      For any rank-deficient Lh this is O(1) (measured 4.5 to 8.6), and
	//      for a healthy one it is ~1e-16.  This is the rank test, and it works
	//      at rank 1 and rank 2 alike because it never divides two residues.
	//   b. WILL CONJUGATION KEEP ENOUGH PRECISION?  ||Lh^-1||_F, which with
	//      ||Lh||_F == 1 IS the Frobenius condition number.  1e9 keeps ~7 of
	//      double's ~16 digits through `P^-1 * worldOp * P`.
	//
	// Normalising also removes the under/overflow edge the earlier forms had --
	// but ONLY with a max-magnitude normaliser.  A Frobenius normaliser sums
	// squares and so has an edge of its own at ~1e+-154; it did not remove the
	// earlier forms' edge, it moved it, and a perfectly conditioned uniform
	// `scale 1e-170` was still refused.  See the normaliser below.
	//
	// Verified by sweep against these exact formulas: 0/4000 composed rank-1
	// accepted, 0/4000 rank-2, 4000/4000 healthy; the anisotropic 1e16 and
	// 1e10 cases rejected, 1e6 accepted; reflection and shear accepted.
	//
	// The projective row must be (0,0,0,1): `matrix` takes 16 free doubles, so
	// a projective parent is authorable, and for one of those the upper 3x3 is
	// not the whole story.  Refused rather than guessed at.
	// The inverse is built BELOW, from the same normalised quantities the test
	// validates.  Matrix4Ops::Inverse is deliberately NOT used: it is the raw
	// 4x4 adjugate/determinant, and that determinant under/overflows exactly
	// where normalisation exists to stop it -- a uniform `scale 1e-120` gives
	// det == 0.0 and Inverse then returns its INPUT, so the flag would say
	// "invertible" while the stored matrix was P itself.  Validating one matrix
	// and storing another is the shape of that bug; this stores what it checks.
	{
		const Scalar* p = &m_mxParentWorld._00;
		double L[9];
		bool finite = true;
		double biggest = 0;
		for( int c = 0; c < 3; ++c ) {
			for( int r = 0; r < 3; ++r ) {
				const double v = static_cast<double>( p[c * 4 + r] );
				if( !IsFiniteDouble( v ) ) finite = false;
				L[c * 3 + r] = v;
				const double a2 = std::fabs( v );
				if( a2 > biggest ) biggest = a2;
			}
		}
		// The TRANSLATION column too (_30.._32 = p[12..14]).  It is not part of
		// the linear part and cannot make an affine map singular, but it IS fed
		// into the stored inverse's own translation below, so a non-finite one
		// would produce a NaN "inverse" behind a TRUE flag.  An earlier revision
		// checked only the 3x3 and did exactly that.
		for( int k = 12; k < 15; ++k ) {
			if( !IsFiniteDouble( static_cast<double>( p[k] ) ) ) finite = false;
		}
		// Normalise by the LARGEST MAGNITUDE ENTRY, not the Frobenius norm.
		// Frobenius sums squares, so it overflows above ~1e154 and underflows
		// below ~1e-154 -- it did not remove the earlier forms' under/overflow
		// edge, it moved it, and a perfectly conditioned uniform `scale 1e-170`
		// was still refused.  max|entry| is exact and cannot overflow, and it
		// leaves ||Lh||_F in [1, 3], so `normInv` below is the Frobenius
		// condition number to within a factor of 3 -- immaterial against a 1e9
		// bound.
		const double frob = biggest;
		const bool affine = ( p[3] == Scalar( 0 ) ) && ( p[7] == Scalar( 0 ) )
		                 && ( p[11] == Scalar( 0 ) ) && ( p[15] == Scalar( 1 ) );

		bool wellConditioned = false;
		if( finite && affine && IsFiniteDouble( frob ) && frob > 0 ) {
			// Normalise: every entry of Lh is now O(1).
			double Lh[9];
			for( int k = 0; k < 9; ++k ) Lh[k] = L[k] / frob;

			// det and adjugate of the NORMALISED matrix (column-major, so
			// Lh[c*3+r] is column c, row r).
			const double d0 = Lh[4] * Lh[8] - Lh[7] * Lh[5];
			const double d1 = Lh[7] * Lh[2] - Lh[1] * Lh[8];
			const double d2 = Lh[1] * Lh[5] - Lh[4] * Lh[2];
			const double det = Lh[0] * d0 + Lh[3] * d1 + Lh[6] * d2;

			if( IsFiniteDouble( det ) && det != 0.0 ) {
				double inv[9];
				inv[0] = d0;
				inv[1] = d1;
				inv[2] = d2;
				inv[3] = Lh[6] * Lh[5] - Lh[3] * Lh[8];
				inv[4] = Lh[0] * Lh[8] - Lh[6] * Lh[2];
				inv[5] = Lh[3] * Lh[2] - Lh[0] * Lh[5];
				inv[6] = Lh[3] * Lh[7] - Lh[6] * Lh[4];
				inv[7] = Lh[6] * Lh[1] - Lh[0] * Lh[7];
				inv[8] = Lh[0] * Lh[4] - Lh[3] * Lh[1];
				double normInv = 0;
				for( int k = 0; k < 9; ++k ) { inv[k] /= det; normInv += inv[k] * inv[k]; }
				normInv = std::sqrt( normInv );

				// (a) residual of the product against identity.
				double residual = 0;
				for( int c = 0; c < 3; ++c ) {
					for( int r = 0; r < 3; ++r ) {
						double acc = 0;
						for( int k = 0; k < 3; ++k ) acc += Lh[k * 3 + r] * inv[c * 3 + k];
						const double e = acc - ( c == r ? 1.0 : 0.0 );
						residual += e * e;
					}
				}
				residual = std::sqrt( residual );

				wellConditioned = IsFiniteDouble( residual ) && residual <= 1e-6      // (a) rank
				               && IsFiniteDouble( normInv )  && normInv  <= 1e9;      // (b) conditioning

				if( wellConditioned ) {
					// Un-normalise into the real inverse.  For an affine
					// P = [ L | t ],  P^-1 = [ L^-1 | -L^-1 t ], and
					// L^-1 = Lh^-1 / frob.  Every quantity here came out of the
					// normalised computation that was just verified, so the
					// stored matrix is exactly the one the flag vouches for --
					// and it is immune to the determinant under/overflow that
					// the raw 4x4 inverse suffers.
					Matrix4 pinv = Matrix4Ops::Identity();
					Scalar* q = &pinv._00;
					for( int c = 0; c < 3; ++c ) {
						for( int r = 0; r < 3; ++r ) {
							q[c * 4 + r] = static_cast<Scalar>( inv[c * 3 + r] / frob );
						}
					}
					const double tx = static_cast<double>( p[12] );
					const double ty = static_cast<double>( p[13] );
					const double tz = static_cast<double>( p[14] );
					for( int r = 0; r < 3; ++r ) {
						const double li0 = inv[0 * 3 + r] / frob;
						const double li1 = inv[1 * 3 + r] / frob;
						const double li2 = inv[2 * 3 + r] / frob;
						q[12 + r] = static_cast<Scalar>( -( li0 * tx + li1 * ty + li2 * tz ) );
					}
					// The un-normalisation itself can overflow: L^-1 = Lh^-1/frob
					// blows up for a tiny `frob`, and -L^-1 t multiplies that by
					// the translation.  Both run AFTER the two tests above, so
					// without this a TRUE flag could front a non-finite inverse
					// -- exactly the "validate one thing, store another" shape
					// this block already fixed once.  Check what is stored.
					bool pinvFinite = true;
					{
						const Scalar* qq = &pinv._00;
						for( int k = 0; k < 16; ++k ) {
							if( !IsFiniteDouble( static_cast<double>( qq[k] ) ) ) { pinvFinite = false; break; }
						}
					}
					if( pinvFinite ) m_mxParentWorldInv = pinv;
					else             wellConditioned  = false;
				}
			}
		}

		m_bParentWorldInvertible = wellConditioned;
		if( !wellConditioned ) {
			// A garbage "inverse" is worse than none: WorldToLocal returns its
			// input in this state, and its one caller is required to check the
			// flag and refuse rather than compose with it.
			m_mxParentWorldInv = Matrix4Ops::Identity();
		}
	}

	// ---- WORLD.
	m_mxFinalTrans = m_mxParentWorld * m_mxLocalTrans;
	m_mxInvFinalTrans = Matrix4Ops::Inverse( m_mxFinalTrans );
}

Matrix4 Transformable::CollapsedTransformStack_( ) const
{
	Matrix4 product = Matrix4Ops::Identity();
	for( TransformStackType::const_iterator i = m_transformstack.begin(); i < m_transformstack.end(); ++i ) {
		product = (*i) * product;
	}
	return product;
}

TransformState Transformable::CaptureTransformState( ) const
{
	TransformState st;
	st.position    = m_mxPosition;
	st.orientation = m_mxOrientation;
	st.scale       = m_mxScale;
	st.stretch     = m_mxStretch;
	// Collapse the stack into a single product that reproduces its
	// contribution under FinalizeTransformations (each entry left-multiplies,
	// iterated front to back), so restore can re-push one matrix.
	st.stackProduct = CollapsedTransformStack_();
	return st;
}

void Transformable::RestoreTransformState( const TransformState& st )
{
	// Restore the component matrices EXACTLY (the whole point: a later
	// absolute SetPosition / SetOrientation / ... then replaces the right
	// component) and re-push the collapsed stack product as one entry.
	m_transformstack.clear();
	m_mxPosition    = st.position;
	m_mxOrientation = st.orientation;
	m_mxScale       = st.scale;
	m_mxStretch     = st.stretch;
	m_transformstack.push_front( st.stackProduct );
	// Preserve the legacy API's exact historical component+stack behavior.
	// The additive V2 sibling below restores authoritative-matrix metadata for
	// in-tree editor snapshots without changing TransformState's ABI.
	ClearFinalMetadata_( this );
	FinalizeTransformations();
}

TransformStateV2 Transformable::CaptureTransformStateV2( ) const
{
	TransformStateV2 state;
	state.transform = CaptureTransformState();
	FinalMatrixMetadata metadata;
	state.finalMatrixOnStack = ReadFinalMetadata_( this, metadata );
	state.finalScaleBase = metadata.scaleBase;
	state.appliedStretch = metadata.appliedStretch;
	state.finalScaleBaseValid = metadata.scaleBaseValid;
	state.stackEntries.assign( m_transformstack.begin(), m_transformstack.end() );
	state.authoritativeStackIndex = metadata.authoritativeIndex;
	// doc 89 slice C.  Carried by V2 only: TransformState (V1) is a frozen
	// by-value struct with binary callers, so the mirror cannot go in it -- which
	// is why RestoreTransformState leaves the mirror ALONE rather than resetting
	// it.  Restoring a V1 snapshot onto a mirrored node keeps the mirror, matching
	// V1's contract of restoring exactly what it captured.
	state.mirrorAxis = m_mirrorAxis;
	return state;
}

bool Transformable::RestoreTransformStateV2( const TransformStateV2& state )
{
	if( !TransformStateV2IsValid_( state ) ) return false;
	m_transformstack.assign( state.stackEntries.begin(), state.stackEntries.end() );
	m_mxPosition = state.transform.position;
	m_mxOrientation = state.transform.orientation;
	m_mxScale = state.transform.scale;
	m_mxStretch = state.transform.stretch;
	SetMirrorAxis( state.mirrorAxis );   // validated by TransformStateV2IsValid_ above
	if( state.finalMatrixOnStack ) {
		FinalMatrixMetadata metadata;
		metadata.active = true;
		metadata.scaleBase = state.finalScaleBase;
		metadata.appliedStretch = state.appliedStretch;
		metadata.scaleBaseValid = state.finalScaleBaseValid;
		metadata.authoritativeIndex = state.authoritativeStackIndex;
		StoreFinalMetadata_( this, metadata );
	} else {
		ClearFinalMetadata_( this );
	}
	FinalizeTransformations();
	return true;
}

void Transformable::CopyTransformMetadataTo( Transformable& destination ) const
{
	FinalMatrixMetadata metadata;
	if( ReadFinalMetadata_( this, metadata ) ) {
		StoreFinalMetadata_( &destination, metadata );
	} else {
		ClearFinalMetadata_( &destination );
	}
}

static const unsigned int POSITION_ID = 1000;
static const unsigned int ORIENTATION_ID = 1001;
static const unsigned int SCALE_ID = 1002;

IKeyframeParameter* Transformable::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "position" ) {
		double d[3];
		if( ParseStrictVec3( value, d ) ) {
			p = new Point3Keyframe( Point3( d[0], d[1], d[2] ), POSITION_ID );
		}
	} else if( name == "orientation" ) {
		double d[3];
		if( ParseStrictVec3( value, d ) ) {
			p = new Vector3Keyframe(
				Vector3( d[0] * DEG_TO_RAD, d[1] * DEG_TO_RAD, d[2] * DEG_TO_RAD ),
				ORIENTATION_ID );
		}
	} else if( name == "scale" ) {
		double d[3];
		if( ParseStrictVec3( value, d ) ) {
			p = new Vector3Keyframe( Vector3( d[0], d[1], d[2] ), SCALE_ID );
		}
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void Transformable::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case POSITION_ID:
		{
			Point3	v = *(Point3*)val.getValue();
			SetPosition( v );
		}
		break;
	case ORIENTATION_ID:
		{
			Vector3	v = *(Vector3*)val.getValue();
			SetOrientation( v );
		}
		break;
	case SCALE_ID:
		{
			Vector3	v = *(Vector3*)val.getValue();
			SetStretch( v );
		}
		break;
	}
}

void Transformable::RegenerateData( )
{
	FinalizeTransformations();
}
