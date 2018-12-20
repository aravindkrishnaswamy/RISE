//////////////////////////////////////////////////////////////////////
//
//  ITransformable.h - Transformable interfacec
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 28, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ITRANSFORMABLE_
#define ITRANSFORMABLE_

#include "IKeyframable.h"
#include "../Utilities/Math3D/Math3D.h"

namespace RISE
{
	//! The ability to have a basic transformation abilities
	class IBasicTransform
	{
	protected:
		IBasicTransform(){};
		virtual ~IBasicTransform(){};

	public:
		//! Clears all transforms
		virtual void ClearAllTransforms( ) = 0;
		//! Finalizes all transformations and computes the final matrix
		virtual void FinalizeTransformations( ) = 0;
		//! Retreives the transformation matrix
		virtual Matrix4 const GetFinalTransformMatrix( ) const = 0;
		//! Retreives the inverse transformation matrix
		virtual Matrix4 const GetFinalInverseTransformMatrix( ) const = 0;
	};

	//! Provides a flexible transformation stack
	class ITransformStack : public virtual IBasicTransform
	{
	protected:
		ITransformStack(){};
		virtual ~ITransformStack(){};

	public:
		//! Pushes a matrix on the top of the transformation stack
		virtual void PushTopTransStack( const
			Matrix4& mat							///< [in] Affine Transformation matrix for R3
			) = 0;
		//! Pushes a matrix on the bottom of the transformation stack
		virtual void PushBottomTransStack( 
			const Matrix4& mat					///< [in] Affine Transformation matrix for R3
			) = 0;
		//! Pops the top of the transformation stack
		virtual void PopTopTransStack( ) = 0;
		//! Pops the bottom of the transformation stack
		virtual void PopBottomTransStack( ) = 0;
		//! Clears the transform stack
		virtual void ClearTransformStack( ) = 0;
	};

	//! Provides axial rotation abilities
	class IRotatable : public virtual IBasicTransform
	{
	protected:
		IRotatable(){};
		virtual ~IRotatable(){};

	public:
		//! Applies rotation in the X axis to the transformation stack
		virtual void RotateObjectXAxis( 
			const Scalar nAmount					///< [in] Amount to rotate in radians
			) = 0;
		//! Applies rotation in the Y axis to the transformation stack
		virtual void RotateObjectYAxis(
			const Scalar nAmount					///< [in] Amount to rotate in radians
			) = 0;
		//! Applies rotation in the Z axis to the transformation stack
		virtual void RotateObjectZAxis(
			const Scalar nAmount					///< [in] Amount to rotate in radians
			) = 0;
		//! Applies rotation in an arbritary axis to the transformation stack
		virtual void RotateObjectArbAxis( 
			const Vector3& axis,					///< [in] Axis of rotation, this should be normalized
			const Scalar nAmount					///< [in] Amount to rotate in radians
			) = 0;
	};

	//! Provides precise euler angle orientation abilities
	class IOrientable : public virtual IBasicTransform
	{
	protected:
		IOrientable(){};
		virtual ~IOrientable(){};

	public:
		//! Sets a specific orientation in euler angles
		virtual void SetOrientation( 
			const Vector3& orient					///< [in] Orientation in euler angles, each axis in the vector
													///< corresponds to an axis of rotation
			) = 0;
	};

	//! Provides the ability to be moved or translated
	class ITranslatable : public virtual IBasicTransform
	{
	protected:
		ITranslatable(){};
		virtual ~ITranslatable(){};

	public:
		//! Applies a translation to the transformation stack
		virtual void TranslateObject( const Vector3& vec ) = 0;
	};

	//! Provides precise positioning abilities
	class IPositionable : public virtual IBasicTransform
	{
	protected:
		IPositionable(){};
		virtual ~IPositionable(){};

	public:
		//! Sets a specific position
		virtual void SetPosition( const Point3& pos ) = 0;
	};

	//! Provides uniform scale abilities
	class IScalable : public virtual IBasicTransform
	{
	protected:
		IScalable(){};
		virtual ~IScalable(){};

	public:
		//! Sets a specific scale value
		virtual void SetScale( 
			const Scalar nAmount					///< [in] The amount of uniformly scale all the axis by
			) = 0;
	};

	//! Provides per axis stretching abilities
	class IStretchable : public virtual IBasicTransform
	{
	protected:
		IStretchable(){};
		virtual ~IStretchable(){};

	public:
		//! Sets a specific value to stretch
		virtual void SetStretch( 
			const Vector3& stretch					///< [in] The vector represents the amounts to stretch each axis by
		) = 0;
	};

	//! Agreggate class for transformations
	/// \sa ITransformStack
	/// \sa IRotatable
	/// \sa IOrientable
	/// \sa IPositionable
	/// \sa IScalable
	/// \sa IStretchable
	/// \sa ITranslatable
	class ITransformable : 
		public virtual ITransformStack,
		public virtual IRotatable,
		public virtual IOrientable,
		public virtual ITranslatable, 
		public virtual IPositionable, 
		public virtual IScalable, 
		public virtual IStretchable,
		public virtual IKeyframable
	{
	protected:
		ITransformable(){};
		virtual ~ITransformable(){};
	};
}

#endif
