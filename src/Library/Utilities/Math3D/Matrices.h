//////////////////////////////////////////////////////////////////////
//
//  Matrices.h - Contains definition for a templated 3x3 and 4x4
//               Matrix classes
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 15, 2001
//  Tabs: 4
//  Comments:  These are all affine matrices ONLY!
//             Thanks to Chad Faragher for most of the math 
//             here.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////
//
//  Descriptions of different types
//
//  Matrix3		- Templated 3x3 matrix class
//  Matrix4		- Templated 4x4 matrix class
//
//////////////////////////////////////////////////////////////////////


#ifndef MATRICES_
#define MATRICES_

namespace RISE
{

	////////////////////////////////////
	//
	// 3x3 Matrix class
	//
	////////////////////////////////////

	struct Matrix3
	{
		//
		// Actual data
		//
		Scalar	_00, _01, _02;
		Scalar	_10, _11, _12;
		Scalar	_20, _21, _22;

		//
		// Constructors
		//
		inline Matrix3( )
		{
			_00 = 1;	_01 = 0;	_02 = 0;
			_10 = 0;	_11 = 1;	_12 = 0;
			_20 = 0;	_21 = 0;	_22 = 1;
		};

		inline Matrix3( 
			const Scalar v00, const Scalar v01, const Scalar v02,
			const Scalar v10, const Scalar v11, const Scalar v12,
			const Scalar v20, const Scalar v21, const Scalar v22
			)
		{
			_00 = v00;	_01 = v01;	_02 = v02;
			_10 = v10;	_11 = v11;	_12 = v12;
			_20 = v20;	_21 = v21;	_22 = v22;
		};
			
		// Copy Constructor
		inline Matrix3( const Matrix3& m )
		{
			_00 = m._00;	_01 = m._01;	_02 = m._02;
			_10 = m._10;	_11 = m._11;	_12 = m._12;
			_20 = m._20;	_21 = m._21;	_22 = m._22;
		};

		// 
		// Operators
		//

		// Matrix multiplication, A upon B
		inline friend	Matrix3	operator*( const Matrix3& a, const Matrix3& b )
		{
			Matrix3		ret;
			
			ret._00 = a._00 * b._00 + a._01 * b._10 + a._02 * b._20;
			ret._01 = a._00 * b._01 + a._01 * b._11 + a._02 * b._21;
			ret._02 = a._00 * b._02 + a._01 * b._12 + a._02 * b._22;
			ret._10 = a._10 * b._00 + a._11 * b._10 + a._12 * b._20;
			ret._11 = a._10 * b._01 + a._11 * b._11 + a._12 * b._21;
			ret._12 = a._10 * b._02 + a._11 * b._12 + a._12 * b._22;
			ret._20 = a._20 * b._00 + a._21 * b._10 + a._22 * b._20;
			ret._21 = a._20 * b._01 + a._21 * b._11 + a._22 * b._21;
			ret._22 = a._20 * b._02 + a._21 * b._12 + a._22 * b._22;

			return ret;
		}
	};


	////////////////////////////////////
	//
	// 4x4 Matrix class
	//
	////////////////////////////////////

	struct Matrix4
	{
		//
		// Actual data
		//
		Scalar	_00, _01, _02, _03;
		Scalar	_10, _11, _12, _13;
		Scalar	_20, _21, _22, _23;
		Scalar	_30, _31, _32, _33;

		//
		// Constructors
		//
		inline Matrix4( )
		{
			_00 = 1;	_01 = 0;	_02 = 0;	_03 = 0;
			_10 = 0;	_11 = 1;	_12 = 0;	_13 = 0;
			_20 = 0;	_21 = 0;	_22 = 1;	_23 = 0;
			_30 = 0;	_31 = 0;	_32 = 0;	_33 = 1;
		};

		inline Matrix4( 
			const Scalar v00, const Scalar v01, const Scalar v02, const Scalar v03,
			const Scalar v10, const Scalar v11, const Scalar v12, const Scalar v13,
			const Scalar v20, const Scalar v21, const Scalar v22, const Scalar v23,
			const Scalar v30, const Scalar v31, const Scalar v32, const Scalar v33
			)
		{
			_00 = v00;	_01 = v01;	_02 = v02;	_03 = v03;
			_10 = v10;	_11 = v11;	_12 = v12;	_13 = v13;
			_20 = v20;	_21 = v21;	_22 = v22;	_23 = v23;
			_30 = v30;	_31 = v31;	_32 = v32;	_33 = v33;
		};


		// Copy Constructor
		inline Matrix4( const Matrix4& m )
		{
			_00 = m._00;	_01 = m._01;	_02 = m._02;	_03 = m._03;
			_10 = m._10;	_11 = m._11;	_12 = m._12;	_13 = m._13;
			_20 = m._20;	_21 = m._21;	_22 = m._22;	_23 = m._23;
			_30 = m._30;	_31 = m._31;	_32 = m._32;	_33 = m._33;
		};

		// Matrix multiplication, A upon B
		inline friend	Matrix4	operator*( const Matrix4& a, const Matrix4& b )
		{
			Matrix4		ret;

			ret._00 = a._00 * b._00 + a._10 * b._01 + a._20 * b._02 + a._30 * b._03;
			ret._10 = a._00 * b._10 + a._10 * b._11 + a._20 * b._12 + a._30 * b._13;
			ret._20 = a._00 * b._20 + a._10 * b._21 + a._20 * b._22 + a._30 * b._23;
			ret._30 = a._00 * b._30 + a._10 * b._31 + a._20 * b._32 + a._30 * b._33;
			ret._01 = a._01 * b._00 + a._11 * b._01 + a._21 * b._02 + a._31 * b._03;
			ret._11 = a._01 * b._10 + a._11 * b._11 + a._21 * b._12 + a._31 * b._13;
			ret._21 = a._01 * b._20 + a._11 * b._21 + a._21 * b._22 + a._31 * b._23;
			ret._31 = a._01 * b._30 + a._11 * b._31 + a._21 * b._32 + a._31 * b._33;
			ret._02 = a._02 * b._00 + a._12 * b._01 + a._22 * b._02 + a._32 * b._03;
			ret._12 = a._02 * b._10 + a._12 * b._11 + a._22 * b._12 + a._32 * b._13;
			ret._22 = a._02 * b._20 + a._12 * b._21 + a._22 * b._22 + a._32 * b._23;
			ret._32 = a._02 * b._30 + a._12 * b._31 + a._22 * b._32 + a._32 * b._33;
			ret._03 = a._03 * b._00 + a._13 * b._01 + a._23 * b._02 + a._33 * b._03;
			ret._13 = a._03 * b._10 + a._13 * b._11 + a._23 * b._12 + a._33 * b._13;
			ret._23 = a._03 * b._20 + a._13 * b._21 + a._23 * b._22 + a._33 * b._23;
			ret._33 = a._03 * b._30 + a._13 * b._31 + a._23 * b._32 + a._33 * b._33;

			return ret;
		}
	};
}


#endif
