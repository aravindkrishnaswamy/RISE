//////////////////////////////////////////////////////////////////////
//
//  Transformable.h - Defines a class that when extended gives the
//  child class the ability to be transformed in 3 space
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 2, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TRANSFORMABLE_
#define TRANSFORMABLE_

#include "../Interfaces/ITransformable.h"

#include <deque>

namespace RISE
{
	namespace Implementation
	{
		class Transformable : public virtual ITransformable
		{
		protected:
			Transformable( );
			virtual ~Transformable( );

			Matrix4		m_mxFinalTrans;		// Finalized WORLD transformation matrix (parentWorld * local)
			Matrix4		m_mxInvFinalTrans;	// Inverse of finalized transformation matrix

			typedef std::deque<Matrix4> TransformStackType;
			TransformStackType	m_transformstack;

			Matrix4		m_mxPosition;		// Position matrix
			Matrix4		m_mxOrientation;	// Orientation matrix
			Matrix4		m_mxScale;			// Scale matrix
			Matrix4		m_mxStretch;		// Stretch matrix

			//! This node's OWN transform: (P * O * Stretch * Scale) folded with
			//! the transform stack, with NO parent contribution.  Cached by
			//! FinalizeTransformations so the authored value can be read back
			//! exactly -- without inverting anything -- by the scene-document
			//! commit, the transform panel and undo capture.
			Matrix4		m_mxLocalTrans;

			//! The parent world transform last composed into this node
			//! (identity for a root), plus its VERIFIED inverse.  Held as
			//! VALUES, never as a pointer to the parent: `Reference` has no
			//! weak-ref primitive, so a child -> parent back-pointer is the one
			//! change that would introduce a genuine lifetime cycle
			//! (docs/agentic-redesign/86-object-grouping.md §3).
			Matrix4		m_mxParentWorld;
			Matrix4		m_mxParentWorldInv;

			//! Did `m_mxParentWorld * m_mxParentWorldInv == I` verify?
			//! Matrix4Ops::Inverse returns its INPUT unchanged at zero
			//! determinant, so a determinant epsilon is not enough -- the
			//! product is checked against identity directly.
			bool		m_bParentWorldInvertible;

			Matrix4 CollapsedTransformStack_( ) const;
			void ReplaceFinalStack_( const Matrix4& matrix );

		public:
			// These two methods allow direct access to the transformation stack
			virtual void PushTopTransStack( const Matrix4& mat ) override;
			virtual void PushBottomTransStack( const Matrix4& mat ) override;
			virtual void PopTopTransStack( ) override;
			virtual void PopBottomTransStack( ) override;
			virtual void ClearTransformStack( ) override;

			// Clears all transforms
			virtual void ClearAllTransforms( ) override;

			// Applies a translation to the transformation stack
			virtual void TranslateObject( const Vector3& vec ) override;

			// Applies rotations to the transformation stack
			virtual void RotateObjectXAxis( const Scalar nAmount ) override;
			virtual void RotateObjectYAxis( const Scalar nAmount ) override;
			virtual void RotateObjectZAxis( const Scalar nAmount ) override;
			virtual void RotateObjectArbAxis( const Vector3& axis, const Scalar nAmount ) override;

			// Allows the user to directly set the position, orientation and scale
			// values
			virtual void SetPosition( const Point3& pos ) override;
			virtual void SetOrientation( const Vector3& orient ) override;
			virtual void SetScale( const Scalar nAmount ) override;
			virtual void SetStretch( const Vector3& stretch ) override;

			// Finalizes all transformations and computes the final matrix
			virtual void FinalizeTransformations( ) override;

			//! Legacy component-only snapshot retained for ABI compatibility.
			//! Matrix-authored callers needing exact subsequent setter semantics
			//! must use the additive RISE_API_*TransformStateV2 entry points.
			virtual TransformState CaptureTransformState( ) const;

			//! Restore a legacy component-only state, then finalize.
			virtual void RestoreTransformState( const TransformState& st );
			TransformStateV2 CaptureTransformStateV2( ) const;
			bool RestoreTransformStateV2( const TransformStateV2& st );
			void CopyTransformMetadataTo( Transformable& destination ) const;

			//! Replace this node's whole LOCAL transform with one authoritative
			//! affine matrix.  Unlike a raw Clear+Push sequence, later absolute
			//! SetPosition / SetOrientation / SetScale / SetStretch calls edit
			//! this matrix rather than composing hidden component state
			//! underneath it.
			//!
			//! LOCAL, not world: a parented node's world matrix is
			//! `parentWorld * matrix` after the next finalize.  A caller
			//! holding a WORLD matrix must convert with WorldToLocal() first
			//! (having checked IsParentWorldInvertible()).
			void SetFinalTransformMatrix( const Matrix4& matrix );

			// Retrieves the transformation matrix
			virtual inline Matrix4 const GetFinalTransformMatrix( ) const override { return m_mxFinalTrans; };
			virtual inline Matrix4 const GetFinalInverseTransformMatrix( ) const override { return m_mxInvFinalTrans; };

			// ---- recursive scene graph (docs/agentic-redesign/87-recursive-scene-graph.md)
			void FinalizeTransformations( const Matrix4& parentWorld ) override;
			virtual inline Matrix4 const GetLocalTransformMatrix( ) const override { return m_mxLocalTrans; };
			virtual inline Matrix4 const GetParentWorldTransformMatrix( ) const override { return m_mxParentWorld; };
			virtual inline bool IsParentWorldInvertible( ) const override { return m_bParentWorldInvertible; };
			virtual inline Matrix4 const WorldToLocal( const Matrix4& worldMatrix ) const override
				{ return m_bParentWorldInvertible ? Matrix4( m_mxParentWorldInv * worldMatrix ) : worldMatrix; };

			// For keyframamble interface
			virtual IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ) override;
			virtual void SetIntermediateValue( const IKeyframeParameter& val ) override;
			virtual void RegenerateData( ) override;
		};
	}
}

#endif
