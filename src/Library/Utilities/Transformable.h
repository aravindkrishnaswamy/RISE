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

			Matrix4		m_mxFinalTrans;		// Finalized transformation matrix
			Matrix4		m_mxInvFinalTrans;	// Inverse of finalized transformation matrix

			typedef std::deque<Matrix4> TransformStackType;
			TransformStackType	m_transformstack;

			Matrix4		m_mxPosition;		// Position matrix
			Matrix4		m_mxOrientation;	// Orientation matrix
			Matrix4		m_mxScale;			// Scale matrix
			Matrix4		m_mxStretch;		// Stretch matrix

		public:
			// These two methods allow direct access to the transformation stack
			virtual void PushTopTransStack( const Matrix4& mat );
			virtual void PushBottomTransStack( const Matrix4& mat );
			virtual void PopTopTransStack( );
			virtual void PopBottomTransStack( );
			virtual void ClearTransformStack( );

			// Clears all transforms
			virtual void ClearAllTransforms( );

			// Applies a translation to the transformation stack
			virtual void TranslateObject( const Vector3& vec );

			// Applies rotations to the transformation stack
			virtual void RotateObjectXAxis( const Scalar nAmount );
			virtual void RotateObjectYAxis( const Scalar nAmount );
			virtual void RotateObjectZAxis( const Scalar nAmount );
			virtual void RotateObjectArbAxis( const Vector3& axis, const Scalar nAmount );

			// Allows the user to directly set the position, orientation and scale
			// values
			virtual void SetPosition( const Point3& pos );
			virtual void SetOrientation( const Vector3& orient );
			virtual void SetScale( const Scalar nAmount );
			virtual void SetStretch( const Vector3& stretch );

			// Finalizes all transformations and computes the final matrix
			virtual void FinalizeTransformations( );

			// Retrieves the transformation matrix
			virtual inline Matrix4 const GetFinalTransformMatrix( ) const { return m_mxFinalTrans; };
			virtual inline Matrix4 const GetFinalInverseTransformMatrix( ) const { return m_mxInvFinalTrans; };

			// For keyframamble interface
			virtual IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			virtual void SetIntermediateValue( const IKeyframeParameter& val );
			virtual void RegenerateData( );
		};
	}
}

#endif
