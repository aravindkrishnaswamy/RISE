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
	m_mxPosition( Matrix4Ops::Identity() ),
	m_mxOrientation( Matrix4Ops::Identity() ),
	m_mxScale( Matrix4Ops::Identity() ),
	m_mxStretch( Matrix4Ops::Identity() )
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
	m_transformstack.clear( );
	m_mxPosition = Matrix4Ops::Identity();
	m_mxOrientation = Matrix4Ops::Identity();
	m_mxScale = Matrix4Ops::Identity();
	m_mxStretch = Matrix4Ops::Identity();
	m_mxFinalTrans = Matrix4Ops::Identity();
	m_mxInvFinalTrans = Matrix4Ops::Identity();
	ClearFinalMetadata_( this );
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

void Transformable::FinalizeTransformations( )
{
	m_mxFinalTrans = Matrix4Ops::Identity();

	// First apply the scale, orientation and position matrices
	m_mxFinalTrans = m_mxPosition * m_mxOrientation * m_mxStretch * m_mxScale;

	// Go through the transformation stack and multiply the transformations...
	TransformStackType::const_iterator		i;
	for( i=m_transformstack.begin(); i<m_transformstack.end(); i++ ) {
		m_mxFinalTrans = (*i) * m_mxFinalTrans;
	}

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
