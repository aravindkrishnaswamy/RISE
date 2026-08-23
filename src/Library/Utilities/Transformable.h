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

			//! doc 89 slice C -- the MIRROR: a reflection across a plane through
			//! the node's LOCAL origin, perpendicular to one local axis.  Identity
			//! when no mirror is set.  Composed INNERMOST (rightmost) in
			//! `P * O * Stretch * Scale * M`, so it reflects the node's own shape
			//! -- and, through the composed world matrix, its whole subtree --
			//! BEFORE the node's own placement is applied.  Author one wing, mirror
			//! the other.
			//!
			//! A SEPARATE MATRIX, NOT A NEGATIVE `m_mxStretch` COMPONENT, and the
			//! reason is that `stretch` is an AUTHORED param with its own value: a
			//! mirror folded into it could not be read back, could not be cleared
			//! independently, and would be silently destroyed by the next absolute
			//! SetStretch (which the transform panel issues on any `scale` edit).
			//! It is also NOT on the transform stack: stack entries LEFT-multiply
			//! (outermost), which is the wrong side -- a mirror applied outside the
			//! node's own rotation reflects the WORLD placement, not the shape.
			Matrix4		m_mxMirror;
			//! The authored axis behind `m_mxMirror`: -1 none, 0 = x, 1 = y, 2 = z.
			//! Kept alongside the matrix so the value can be read back EXACTLY (for
			//! the properties panel, and to un-apply the reflection before decomposing
			//! the local matrix into position / orientation / scale) without having to
			//! recognise a reflection in a composed matrix.
			int			m_mirrorAxis;

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

			//! Is the parent world transform usable as a change of frame?  TRUE
			//! only when its NORMALISED linear part passes both halves of the
			//! test in FinalizeTransformations: the computed inverse really is
			//! an inverse (a rank test), and it is well enough conditioned to
			//! conjugate a world-space operation through.  FALSE for a
			//! collapsed or rank-deficient ancestor, for an extreme anisotropy,
			//! for a non-finite entry, and for a projective row.  Four earlier
			//! formulations of this test are recorded at the implementation,
			//! each with the input that defeats it.
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
			//!
			//! THE CALLER MUST FINALIZE.  This installs the matrix on the
			//! transform STACK and updates the authoritative-matrix metadata;
			//! it does NOT recompute m_mxLocalTrans, m_mxFinalTrans, or the
			//! Object caches.  Until FinalizeTransformations() runs, every
			//! accessor still describes the PREVIOUS transform.  Every call
			//! site in tree finalizes immediately afterward -- Job::AddObjectMatrix,
			//! override_object's two arms, and SceneEditor's ReplaceFinalTransform_
			//! (whose callers all reach RunObjectInvariantChain).  A new one that
			//! forgets would read stale matrices with no diagnostic.
			void SetFinalTransformMatrix( const Matrix4& matrix );

			//! doc 89 slice C: set (or clear) this node's LOCAL mirror axis.
			//! `axis` is -1 for none, 0 for x, 1 for y, 2 for z; any other value is
			//! REFUSED (returns false, changes nothing) rather than silently mapped.
			//!
			//! NON-VIRTUAL, reached through `dynamic_cast<Implementation::Transformable*>`
			//! exactly as `SetFinalTransformMatrix` is -- no interface vtable grows for
			//! it, so there is no ABI decision to review.
			//!
			//! THE CALLER MUST FINALIZE.  Like SetFinalTransformMatrix, this updates
			//! the building block only; `m_mxLocalTrans` / `m_mxFinalTrans` and the
			//! Object caches still describe the PREVIOUS transform until
			//! FinalizeTransformations() runs.
			/// \return TRUE if the axis was accepted, FALSE if it was out of range
			bool SetMirrorAxis( int axis );

			//! The authored mirror axis: -1 none, 0 = x, 1 = y, 2 = z.
			inline int GetMirrorAxis( ) const { return m_mirrorAxis; }

			//! The mirror as a matrix (identity when no mirror is set).  Its own
			//! inverse, so `GetLocalTransformMatrix() * GetMirrorMatrix()` is the
			//! local transform with the reflection UN-applied -- which is what a
			//! position / orientation / scale readback has to decompose.
			inline Matrix4 const GetMirrorMatrix( ) const { return m_mxMirror; }

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
