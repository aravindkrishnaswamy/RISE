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
#include "../Animation/TimeIndexedView.h"
#include "../Interfaces/ILog.h"
#include "FiniteMath.h"

#include <cassert>
#include <atomic>
#include <map>
#include <mutex>
#include <shared_mutex>

using namespace RISE;
using namespace RISE::Implementation;

namespace {
	std::atomic<unsigned int> gPreparedMutationFreezeCount{0u};
	thread_local unsigned int gPreparedInternalMutationDepth=0u;
	thread_local unsigned int gPreparedExternalMutationDepth=0u;
	std::shared_mutex gPreparedMutationMutex;
	// Set by the first refused external mutation of a freeze; cleared when a
	// freeze begins.  Refused setters return void, so the log is the signal.
	std::atomic<bool> gPreparedMutationRefusalReported{false};

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

void Transformable::BeginPreparedMutationFreeze()
{
	gPreparedMutationMutex.lock();
	gPreparedMutationFreezeCount.fetch_add(1u,std::memory_order_acq_rel);
	gPreparedMutationRefusalReported.store(false,std::memory_order_release);
}

void Transformable::EndPreparedMutationFreeze()
{
	const unsigned int previous=gPreparedMutationFreezeCount.fetch_sub(1u,std::memory_order_acq_rel);
	assert(previous>0u);
	gPreparedMutationMutex.unlock();
}

void Transformable::BeginPreparedInternalMutation() { ++gPreparedInternalMutationDepth; }
void Transformable::EndPreparedInternalMutation()
{
	assert(gPreparedInternalMutationDepth>0u);
	if( gPreparedInternalMutationDepth ) --gPreparedInternalMutationDepth;
}
bool Transformable::ExternalMutationIsFrozen()
{
	return !gPreparedInternalMutationDepth &&
		gPreparedMutationFreezeCount.load(std::memory_order_acquire)!=0u;
}

bool Transformable::BeginPreparedExternalMutation()
{
	if( gPreparedInternalMutationDepth ) { ++gPreparedExternalMutationDepth; return true; }
	if( gPreparedExternalMutationDepth ) { ++gPreparedExternalMutationDepth; return true; }
	if( !gPreparedMutationMutex.try_lock_shared() ) {
		// A refused setter leaves the scene unchanged and returns nothing, so
		// say so once per freeze instead of failing silently.  Code that moves
		// the scene as part of the render itself must take
		// BeginPreparedInternalMutation (see PrepareSceneForFrame).
		if( !gPreparedMutationRefusalReported.exchange(true,std::memory_order_acq_rel) ) {
			GlobalLog()->PrintEasyError("mutation_frozen: a scene setter was called while a fire "
				"render holds the prepared mutation freeze; the change was NOT applied "
				"(reported once per render)");
		}
		return false;
	}
	gPreparedExternalMutationDepth=1u;
	return true;
}

void Transformable::EndPreparedExternalMutation()
{
	assert(gPreparedExternalMutationDepth>0u);
	if( gPreparedExternalMutationDepth && --gPreparedExternalMutationDepth==0u &&
		!gPreparedInternalMutationDepth )
		gPreparedMutationMutex.unlock_shared();
}

Transformable::PreparedExternalMutationScope::PreparedExternalMutationScope()
	: valid_(Transformable::BeginPreparedExternalMutation()) {}
Transformable::PreparedExternalMutationScope::~PreparedExternalMutationScope()
{
	if( valid_ ) Transformable::EndPreparedExternalMutation();
}

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
}

// Authoritative-matrix metadata lives per-instance (not in a shared registry)
// so concurrent motion-blur threads touch only their own clones without locking.
bool Transformable::ReadFinalMetadata_( FinalMatrixMetadata& metadata ) const
{
	metadata = m_finalMetadata;
	return metadata.active;
}

void Transformable::StoreFinalMetadata_( const FinalMatrixMetadata& metadata )
{
	m_finalMetadata = metadata;
}

void Transformable::ClearFinalMetadata_( )
{
	m_finalMetadata = FinalMatrixMetadata();
}


void Transformable::PushTopTransStack( const Matrix4& mat )
{
	PreparedExternalMutationScope preparedMutation; if( !preparedMutation.IsValid() ) return;
	m_transformstack.push_front( mat );
	FinalMatrixMetadata metadata;
	if( ReadFinalMetadata_( metadata ) ) {
		++metadata.authoritativeIndex;
		StoreFinalMetadata_( metadata );
	}
}

void Transformable::PushBottomTransStack( const Matrix4& mat )
{
	PreparedExternalMutationScope preparedMutation; if( !preparedMutation.IsValid() ) return;
	m_transformstack.push_back( mat );
}

void Transformable::PopTopTransStack( )
{
	PreparedExternalMutationScope preparedMutation; if( !preparedMutation.IsValid() ) return;
	if( m_transformstack.empty() ) return;
	FinalMatrixMetadata metadata;
	const bool active = ReadFinalMetadata_( metadata );
	if( active && metadata.authoritativeIndex == 0 ) {
		m_transformstack.pop_front();
		ClearFinalMetadata_();
		return;
	}
	m_transformstack.pop_front( );
	if( active ) {
		--metadata.authoritativeIndex;
		StoreFinalMetadata_( metadata );
	}
}

void Transformable::PopBottomTransStack( )
{
	PreparedExternalMutationScope preparedMutation; if( !preparedMutation.IsValid() ) return;
	if( m_transformstack.empty() ) return;
	FinalMatrixMetadata metadata;
	const bool active = ReadFinalMetadata_( metadata );
	if( active && metadata.authoritativeIndex + 1 == m_transformstack.size() ) {
		m_transformstack.pop_back();
		ClearFinalMetadata_();
		return;
	}
	m_transformstack.pop_back( );
}

void Transformable::ClearAllTransforms( )
{
	// Clears local transform only; m_mxParentWorld is preserved so incremental
	// re-applies (Job::AddObject) don't un-parent the node.
	PreparedExternalMutationScope preparedMutation; if( !preparedMutation.IsValid() ) return;
	m_transformstack.clear( );
	m_mxPosition = Matrix4Ops::Identity();
	m_mxOrientation = Matrix4Ops::Identity();
	m_mxScale = Matrix4Ops::Identity();
	m_mxStretch = Matrix4Ops::Identity();
	// Mirror is a local transform and must reset on re-apply.
	m_mxMirror = Matrix4Ops::Identity();
	m_mirrorAxis = -1;
	ClearFinalMetadata_();
	// Re-finalize to refresh m_mxLocalTrans and subclass caches (e.g. Object area/tangent sign).
	FinalizeTransformations();
}

void Transformable::ClearTransformStack( )
{
	PreparedExternalMutationScope preparedMutation; if( !preparedMutation.IsValid() ) return;
	m_transformstack.clear( );
	ClearFinalMetadata_();
}

void Transformable::TranslateObject( const Vector3& vec )
{
	PreparedExternalMutationScope preparedMutation; if( !preparedMutation.IsValid() ) return;
	Matrix4 mx = Matrix4Ops::Translation( vec );
	PushBottomTransStack( mx );
}

void Transformable::RotateObjectXAxis( const Scalar nAmount )
{
	PreparedExternalMutationScope preparedMutation; if( !preparedMutation.IsValid() ) return;
	Matrix4 mx = Matrix4Ops::XRotation( nAmount );
	PushBottomTransStack( mx );
}

void Transformable::RotateObjectYAxis( const Scalar nAmount )
{
	PreparedExternalMutationScope preparedMutation; if( !preparedMutation.IsValid() ) return;
	Matrix4 mx = Matrix4Ops::YRotation( nAmount );
	PushBottomTransStack( mx );
}

void Transformable::RotateObjectZAxis( const Scalar nAmount )
{
	PreparedExternalMutationScope preparedMutation; if( !preparedMutation.IsValid() ) return;
	Matrix4 mx = Matrix4Ops::ZRotation( nAmount );
	PushBottomTransStack( mx );
}

void Transformable::RotateObjectArbAxis( const Vector3& axis, const Scalar nAmount )
{
	PreparedExternalMutationScope preparedMutation; if( !preparedMutation.IsValid() ) return;
	Matrix4 mx = Matrix4Ops::Rotation( axis, nAmount );
	PushBottomTransStack( mx );
}

void Transformable::SetPosition( const Point3& pos )
{
	PreparedExternalMutationScope preparedMutation; if( !preparedMutation.IsValid() ) return;
	FinalMatrixMetadata metadata;
	if( ReadFinalMetadata_( metadata ) ) {
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
			StoreFinalMetadata_( metadata );
			return;
		}
	}
	m_mxPosition = Matrix4Ops::Translation( Vector3( pos.x, pos.y, pos.z ) );
}

void Transformable::SetOrientation( const Vector3& orient )
{
	PreparedExternalMutationScope preparedMutation; if( !preparedMutation.IsValid() ) return;
	const Matrix4 orientation = Matrix4Ops::XRotation( orient.x ) *
		Matrix4Ops::YRotation( orient.y ) * 
		Matrix4Ops::ZRotation( orient.z );
	FinalMatrixMetadata metadata;
	if( ReadFinalMetadata_( metadata ) ) {
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
			StoreFinalMetadata_( metadata );
			return;
		}
	}
	m_mxOrientation = orientation;
}

void Transformable::SetScale( const Scalar nAmount )
{
	PreparedExternalMutationScope preparedMutation; if( !preparedMutation.IsValid() ) return;
	FinalMatrixMetadata metadata;
	if( ReadFinalMetadata_( metadata ) ) {
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
		StoreFinalMetadata_( metadata );
		return;
	}
	m_mxScale = Matrix4Ops::Scale( nAmount );
}

void Transformable::SetStretch( const Vector3& stretch )
{
	PreparedExternalMutationScope preparedMutation; if( !preparedMutation.IsValid() ) return;
	FinalMatrixMetadata metadata;
	if( ReadFinalMetadata_( metadata ) ) {
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
		StoreFinalMetadata_( metadata );
		return;
	}
	m_mxStretch = Matrix4Ops::Stretch( stretch );
}

void Transformable::SetFinalTransformMatrix( const Matrix4& matrix )
{
	PreparedExternalMutationScope preparedMutation; if( !preparedMutation.IsValid() ) return;
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
	StoreFinalMetadata_( metadata );
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
	PreparedExternalMutationScope preparedMutation; if( !preparedMutation.IsValid() ) return false;
	// Refuse (never clamp) out-of-range axes so bad tokens don't mirror silently.
	if( axis < -1 || axis > 2 ) return false;
	m_mirrorAxis = axis;
	m_mxMirror = Matrix4Ops::Identity();
	// Determinant -1: Object::FinalizeTransformations flips m_tangentFrameSign
	// and takes |det| for world-area Jacobian (same path as negative scale).
	if( axis == 0 )      m_mxMirror._00 = -1;
	else if( axis == 1 ) m_mxMirror._11 = -1;
	else if( axis == 2 ) m_mxMirror._22 = -1;
	return true;
}

void Transformable::FinalizeTransformations( )
{
	PreparedExternalMutationScope preparedMutation; if( !preparedMutation.IsValid() ) return;
	// Re-compose against the last parent world transform (identity for roots).
	FinalizeTransformations( m_mxParentWorld );
}

void Transformable::FinalizeTransformations( const Matrix4& parentWorld )
{
	PreparedExternalMutationScope preparedMutation; if( !preparedMutation.IsValid() ) return;
	// Local transform: P * O * Stretch * Scale * M (mirror innermost so it
	// reflects the shape in its own frame before positioning).
	m_mxLocalTrans = m_mxPosition * m_mxOrientation * m_mxStretch * m_mxScale * m_mxMirror;

	// Go through the transformation stack and multiply the transformations...
	TransformStackType::const_iterator		i;
	for( i=m_transformstack.begin(); i<m_transformstack.end(); i++ ) {
		m_mxLocalTrans = (*i) * m_mxLocalTrans;
	}

	// Store parentWorld by value (never push onto m_transformstack) so repeated
	// finalize calls stay idempotent (`parentWorld * local`).
	m_mxParentWorld = parentWorld;

	// Validate parent invertibility on the max-entry-normalized 3x3 linear part
	// Lh (requires projective row (0,0,0,1) and all finite entries including
	// translation):
	//   (a) Rank: ||Lh * Lh^-1 - I||_F <= 1e-6 (catches rank-1/2 matrices where
	//       raw adjugate/det ratios yield O(1) noise).
	//   (b) Conditioning: ||Lh^-1||_F <= 1e9 (~7 digits preserved through
	//       P^-1 * worldOp * P).
	// Builds m_mxParentWorldInv directly from the validated Lh^-1 / frob rather
	// than Matrix4Ops::Inverse (avoiding raw 4x4 determinant under/overflow) and
	// verifies finiteness after un-normalizing.
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
		// Check translation column finiteness too (feeds into stored inverse).
		for( int k = 12; k < 15; ++k ) {
			if( !IsFiniteDouble( static_cast<double>( p[k] ) ) ) finite = false;
		}
		// Normalize by max|entry| (not Frobenius, which under/overflows near 1e+-154);
		// ||Lh||_F stays in [1, 3], so normInv tracks Frobenius condition number within 3x.
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
					// Un-normalize P^-1 = [ L^-1 | -L^-1 t ] with L^-1 = Lh^-1 / frob.
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
					// Verify un-normalized entries did not overflow (tiny frob or huge t).
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
			// Clear to identity; callers must check m_bParentWorldInvertible before use.
			m_mxParentWorldInv = Matrix4Ops::Identity();
		}
	}

	// ---- WORLD.
	m_mxFinalTrans = m_mxParentWorld * m_mxLocalTrans;
	m_mxInvFinalTrans = Matrix4Ops::Inverse( m_mxFinalTrans );
	BumpThreadTransformPoseEpoch();
	if( !TimeIndexed::tlsSlots ) {
		BumpGlobalTransformPoseEpoch();
	}
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
	PreparedExternalMutationScope preparedMutation; if( !preparedMutation.IsValid() ) return;
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
	ClearFinalMetadata_();
	FinalizeTransformations();
}

TransformStateV2 Transformable::CaptureTransformStateV2( ) const
{
	TransformStateV2 state;
	state.transform = CaptureTransformState();
	FinalMatrixMetadata metadata;
	state.finalMatrixOnStack = ReadFinalMetadata_( metadata );
	state.finalScaleBase = metadata.scaleBase;
	state.appliedStretch = metadata.appliedStretch;
	state.finalScaleBaseValid = metadata.scaleBaseValid;
	state.stackEntries.assign( m_transformstack.begin(), m_transformstack.end() );
	state.authoritativeStackIndex = metadata.authoritativeIndex;
	// Mirror is carried in V2 only (V1 TransformState is ABI-frozen, so V1 restore leaves it untouched).
	state.mirrorAxis = m_mirrorAxis;
	return state;
}

bool Transformable::RestoreTransformStateV2( const TransformStateV2& state )
{
	PreparedExternalMutationScope preparedMutation; if( !preparedMutation.IsValid() ) return false;
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
		StoreFinalMetadata_( metadata );
	} else {
		ClearFinalMetadata_();
	}
	FinalizeTransformations();
	return true;
}

void Transformable::CopyTransformMetadataTo( Transformable& destination ) const
{
	FinalMatrixMetadata metadata;
	if( ReadFinalMetadata_( metadata ) ) {
		destination.StoreFinalMetadata_( metadata );
	} else {
		destination.ClearFinalMetadata_();
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
	PreparedExternalMutationScope preparedMutation; if( !preparedMutation.IsValid() ) return;
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
	PreparedExternalMutationScope preparedMutation; if( !preparedMutation.IsValid() ) return;
	FinalizeTransformations();
}
