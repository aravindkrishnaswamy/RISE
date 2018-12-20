//////////////////////////////////////////////////////////////////////
//
//  MatricesOps.h - Contains operations on matrices
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 6, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MATRICES_OPS_
#define MATRICES_OPS_

#include "../../Interfaces/IWriteBuffer.h"
#include "../../Interfaces/IReadBuffer.h"

namespace RISE
{
	namespace Matrix3Ops
	{
		// Matrix determinant
		inline Scalar Determinant( const Matrix3& m )
		{
			// Basket weaving
			return (m._00 * (m._11*m._22-m._12*m._21))
				- (m._10 * (m._01*m._22-m._02*m._21))
				+ (m._20 * (m._01*m._12-m._02*m._11));

		}
		
		// Matrix adjoint
		inline Matrix3 Adjoint( const Matrix3& m )
		{
			Matrix3		ret;

			ret._00 =   m._11*m._22 - m._12*m._21;
			ret._10 = - m._10*m._22 + m._12*m._20;
			ret._20 =   m._10*m._21 - m._11*m._20;
			ret._01 = - m._01*m._22 + m._02*m._21;
			ret._11 =   m._00*m._22 - m._02*m._20;
			ret._21 = - m._00*m._21 + m._01*m._20;
			ret._02 =   m._01*m._12 - m._02*m._11;
			ret._12 = - m._00*m._12 + m._02*m._10;
			ret._22 =   m._00*m._11 - m._01*m._10;

			return ret;
		}

		// Scalar matrix multiplication 
		inline Matrix3 ScalarMul( const Matrix3& m, const Scalar d )
		{
			Matrix3		ret;

			ret._00 = m._00 * d;
			ret._01 = m._01 * d;
			ret._02 = m._02 * d;
			ret._10 = m._10 * d;
			ret._11 = m._11 * d;
			ret._12 = m._12 * d;
			ret._20 = m._20 * d;
			ret._21 = m._21 * d;
			ret._22 = m._22 * d;

			return ret;
		}

		// Matrix inverse
		inline Matrix3 Inverse( const Matrix3& m )
		{
			const Scalar det = Determinant( m );

			if( det == 0 ) {
				// Really should throw an exception of something here
				return m;
			}

			return ScalarMul( Adjoint(m), (1.0 / det) );	
		}

		inline Matrix3 Identity( void )
		{
			Matrix3		ret;

			ret._01 = 0.0;
			ret._02 = 0.0;
			ret._10 = 0.0;
			ret._12 = 0.0;
			ret._20 = 0.0;
			ret._21 = 0.0;

			ret._00 = 1.0;
			ret._11 = 1.0;
			ret._22 = 1.0;

			return ret;
		}

		// Translation by the vector v
		inline Matrix3 Translation( const Vector2& v )
		{
			Matrix3		m;

			m._20 = v.x;
			m._21 = v.y;

			return m;
		}

		// Rotation about the axis
		inline Matrix3 Rotation( const Scalar delta )
		{
			Matrix3		m;
			const Scalar S = (Scalar) sin( (Scalar) delta );
			const Scalar C = (Scalar) cos( (Scalar) delta );


			m._00 = C;
			m._01 = S;
			m._10 = -S;
			m._11 = C;
			m._02 = 0.0;
			m._12 = 0.0;
			m._20 = 0.0;
			m._21 = 0.0;
			m._22 = 1.0;

			return m;
		}

		inline Matrix3 Scale2D( const Scalar d )
		{
			Matrix3		m;

			m._00 = d;
			m._11 = d;

			return m;
		}

		inline Matrix3 Stretch( const Vector2& v )
		{
			Matrix3		m;

			m._00 = v.x;
			m._11 = v.y;

			return m;
		}

		// Matrix transpose
		inline Matrix3 Transpose( const Matrix3& m )
		{
			Matrix3 ret;
			ret._00 = m._00;
			ret._01 = m._10;
			ret._02 = m._20;
			ret._10 = m._01;
			ret._11 = m._11;
			ret._12 = m._21;
			ret._20 = m._02;
			ret._21 = m._12;
			ret._22 = m._22;
			return ret;
		}

		inline void Serialize( 
			const Matrix3& m, 
			IWriteBuffer& buffer )
		{
			buffer.ResizeForMore( sizeof( Scalar ) * 9 );
			buffer.setDouble( m._00 );
			buffer.setDouble( m._01 );
			buffer.setDouble( m._02 );
			buffer.setDouble( m._10 );
			buffer.setDouble( m._11 );
			buffer.setDouble( m._12 );
			buffer.setDouble( m._20 );
			buffer.setDouble( m._21 );
			buffer.setDouble( m._22 );
		}

		inline void Deserialize( 
			Matrix3& ret,
			IReadBuffer& buffer )
		{
			ret._00 = buffer.getDouble();
			ret._01 = buffer.getDouble();
			ret._02 = buffer.getDouble();
			ret._10 = buffer.getDouble();
			ret._11 = buffer.getDouble();
			ret._12 = buffer.getDouble();
			ret._20 = buffer.getDouble();
			ret._21 = buffer.getDouble();
			ret._22 = buffer.getDouble();
		}
	}

	namespace Matrix4Ops
	{
		// Identity matrix
		inline Matrix4 Identity( void )
		{
			Matrix4		a;
			return a;
		}

			// Quaternion Constructor
		inline Matrix4 mkFromQuaternion( const Quaternion& a )
		{
			Matrix4		m;

			const Scalar _2x = 2.0 * a.v.x;
			const Scalar _2y = 2.0 * a.v.x;
			const Scalar _2z = 2.0 * a.v.x;
			const Scalar _2xx = _2x * a.v.x;
			const Scalar _2xy = _2x * a.v.y;
			const Scalar _2xz = _2x * a.v.z;
			const Scalar _2yy = _2y * a.v.y;
			const Scalar _2yz = _2y * a.v.z;
			const Scalar _2zz = _2z * a.v.z;
			const Scalar _2xs = _2x * a.s;
			const Scalar _2ys = _2y * a.s;
			const Scalar _2zs = _2z * a.s;

			m._00 = 1.0 - _2yy - _2zz;
			m._10 =       _2xy - _2zs;
			m._20 =       _2xz + _2ys;
			m._30 = 0.0;
			m._01 =       _2xy + _2zs;
			m._11 = 1.0 - _2xx - _2zz;
			m._21 =       _2yz - _2xs;
			m._31 = 0.0;
			m._02 =       _2xz - _2ys;
			m._12 =       _2yz + _2xs;
			m._22 = 1.0 - _2xx - _2yy;
			m._32 = 0.0;
			m._03 = 0.0;
			m._13 = 0.0;
			m._23 = 0.0;
			m._33 = 1.0;

			return m;
		}

		// Matrix scalar multiplication
		inline Matrix4 ScalarMul( const Matrix4& m, const Scalar t )
		{
			Matrix4		ret;

			ret._00 = m._00 * t;
			ret._01 = m._01 * t;
			ret._02 = m._02 * t;
			ret._03 = m._03 * t;
			ret._10 = m._10 * t;
			ret._11 = m._11 * t;
			ret._12 = m._12 * t;
			ret._13 = m._13 * t;
			ret._20 = m._20 * t;
			ret._21 = m._21 * t;
			ret._22 = m._22 * t;
			ret._23 = m._23 * t;
			ret._30 = m._30 * t;
			ret._31 = m._31 * t;
			ret._32 = m._32 * t;
			ret._33 = m._33 * t;
			
			return ret;
		}

		// Rotation about the X axis
		inline Matrix4 XRotation( const Scalar delta )
		{
			Matrix4		m;
			Scalar			S, C;
			S = (Scalar) sin( (Scalar) delta );
			C = (Scalar) cos( (Scalar) delta );

			m._11 = C;
			m._12 = S;
			m._21 = -S;
			m._22 = C;

			return m;
		}

		// Rotation about the Y axis
		inline Matrix4 YRotation( const Scalar delta )
		{
			Matrix4		m;
			Scalar			S, C;
			S = (Scalar) sin( (Scalar) delta );
			C = (Scalar) cos( (Scalar) delta );

			m._22 = C;
			m._20 = S;
			m._02 = -S;
			m._00 = C;

			return m;
		}

		// Rotation about the Z axis
		inline Matrix4 ZRotation( const Scalar delta )
		{
			Matrix4		m;
			Scalar			S, C;
			S = (Scalar) sin( (Scalar) delta );
			C = (Scalar) cos( (Scalar) delta );

			m._00 = C;
			m._01 = S;
			m._10 = -S;
			m._11 = C;

			return m;
		}

		// Translation by the vector v
		inline Matrix4 Translation( const Vector3& v )
		{
			Matrix4		m;

			m._30 = v.x;
			m._31 = v.y;
			m._32 = v.z;

			return m;
		}

		// Rotation about the vector N in the direction of T
		inline Matrix4 Rotation( const Vector3& N, const Scalar T ) 
		{
			const Scalar			cosT = Scalar( cos( T ) );
			const Scalar			sinT = Scalar( sin( T ) );
			const Scalar			omcosT = 1.0f - cosT;

			// Enhancement
			const Scalar			omcostTn0 = N.x * omcosT;
			const Scalar			omcostTn1 = N.y * omcosT;
			const Scalar			omcostTn2 = N.z * omcosT;
			const Scalar			n01 = N.x * omcostTn1;
			const Scalar			n02 = N.y * omcostTn2;
			const Scalar			n12 = N.z * omcostTn2;
			const Scalar			sinTn0 = sinT * N.x;
			const Scalar			sinTn1 = sinT * N.y;
			const Scalar			sinTn2 = sinT * N.z;

			Matrix4		m;

			m._00 = N.x * omcostTn0 + cosT;
			m._10 = n01						 - sinTn2;
			m._20 = n02						 + sinTn1;
			m._30 = 0.0f;
			m._01 = n01						 + sinTn2;
			m._11 = N.y * omcostTn1 + cosT;
			m._21 = n12						 - sinTn0;
			m._31 = 0.0f;
			m._02 = n02						 - sinTn1;
			m._12 = n12						 + sinTn0;
			m._22 = N.z * omcostTn2 + cosT;
			m._32 = 0.0f;
			m._03 = 0.0f;
			m._13 = 0.0f;
			m._23 = 0.0f;
			m._33 = 1.0f;

			return m;
		}

		inline Matrix4 Scale( const Scalar T )
		{
			Matrix4		m;

			m._00 = T;
			m._11 = T;
			m._22 = T;

			return m;
		}

		inline Matrix4 Stretch( const Vector3& v )
		{
			Matrix4		m;

			m._00 = v.x;
			m._11 = v.y;
			m._22 = v.z;

			return m;
		}

		// Matrix Determinant
		static inline Scalar Determinant( const Matrix4& m )
		{
			return 
				m._00 * 
					(
						m._11 * ( m._22 * m._33 - m._32 * m._23 )
					- m._21 *	( m._12 * m._33 - m._32 * m._13	)
					+ m._31 *	( m._12 * m._23 - m._22 * m._13	)
					)
			- m._10 * 
					(
						m._01 * ( m._22 * m._33 - m._32 * m._23	)
					- m._21 *	( m._02 * m._33 - m._32 * m._03	)
					+ m._31 *	( m._02 * m._23 - m._22 * m._03	)
					)
			+ m._20 * 
					(
						m._01 * ( m._12 * m._33 - m._32 * m._13	)
					- m._11 *	( m._02 * m._33 - m._32 * m._03	)
					+ m._31 *	( m._02 * m._13 - m._12 * m._03	)
					)
			- m._30 * 
					(
						m._01 * ( m._12 * m._23 - m._22 * m._13	)
					- m._11 *	( m._02 * m._23 - m._22 * m._03 )
					+ m._21 * ( m._02 * m._13 - m._12 * m._03	)
					)
				;
		}

		// Matrix Adjoint
		inline Matrix4 Adjoint( const Matrix4& M )
		{
			// Thanks to Adam F. Nevraumont for the following code generated by a perl script
			const Scalar D2_1x0_X_1x0 = M._22 * M._33 - M._32 * M._23;
			const Scalar D2_1x0_X_2x0 = M._21 * M._33 - M._31 * M._23;
			const Scalar D2_1x0_X_3x0 = M._21 * M._32 - M._31 * M._22;
			const Scalar D2_2x0_X_1x0 = M._12 * M._33 - M._32 * M._13;
			const Scalar D2_2x0_X_2x0 = M._11 * M._33 - M._31 * M._13;
			const Scalar D2_2x0_X_3x0 = M._11 * M._32 - M._31 * M._12;
			const Scalar D2_2x1_X_1x0 = M._02 * M._33 - M._32 * M._03;
			const Scalar D2_2x1_X_2x0 = M._01 * M._33 - M._31 * M._03;
			const Scalar D2_2x1_X_3x0 = M._01 * M._32 - M._31 * M._02;
			const Scalar D2_3x0_X_1x0 = M._12 * M._23 - M._22 * M._13;
			const Scalar D2_3x0_X_2x0 = M._11 * M._23 - M._21 * M._13;
			const Scalar D2_3x0_X_3x0 = M._11 * M._22 - M._21 * M._12;
			const Scalar D2_3x1_X_1x0 = M._02 * M._23 - M._22 * M._03;
			const Scalar D2_3x1_X_2x0 = M._01 * M._23 - M._21 * M._03;
			const Scalar D2_3x1_X_3x0 = M._01 * M._22 - M._21 * M._02;
			const Scalar D2_3x2_X_1x0 = M._02 * M._13 - M._12 * M._03;
			const Scalar D2_3x2_X_2x0 = M._01 * M._13 - M._11 * M._03;
			const Scalar D2_3x2_X_3x0 = M._01 * M._12 - M._11 * M._02;

			Matrix4 a;

			a._00 =(M._11 * D2_1x0_X_1x0 - M._21 * D2_2x0_X_1x0	+ M._31 * D2_3x0_X_1x0) * 1;
			a._01 =(M._01 * D2_1x0_X_1x0 - M._21 * D2_2x1_X_1x0 + M._31 * D2_3x1_X_1x0) * -1;
			a._02 =(M._01 * D2_2x0_X_1x0 - M._11 * D2_2x1_X_1x0 + M._31 * D2_3x2_X_1x0) * 1;
			a._03 =(M._01 * D2_3x0_X_1x0 - M._11 * D2_3x1_X_1x0 + M._21 * D2_3x2_X_1x0) * -1;
			a._10 =(M._10 * D2_1x0_X_1x0 - M._20 * D2_2x0_X_1x0	+ M._30 * D2_3x0_X_1x0) * -1;
			a._11 =(M._00 * D2_1x0_X_1x0 - M._20 * D2_2x1_X_1x0 + M._30 * D2_3x1_X_1x0) * 1;
			a._12 =(M._00 * D2_2x0_X_1x0 - M._10 * D2_2x1_X_1x0 + M._30 * D2_3x2_X_1x0) * -1;
			a._13 =(M._00 * D2_3x0_X_1x0 - M._10 * D2_3x1_X_1x0 + M._20 * D2_3x2_X_1x0) * 1;
			a._20 =(M._10 * D2_1x0_X_2x0 - M._20 * D2_2x0_X_2x0 + M._30 * D2_3x0_X_2x0) * 1;
			a._21 =(M._00 * D2_1x0_X_2x0 - M._20 * D2_2x1_X_2x0 + M._30 * D2_3x1_X_2x0) * -1;
			a._22 =(M._00 * D2_2x0_X_2x0 - M._10 * D2_2x1_X_2x0 + M._30 * D2_3x2_X_2x0) * 1;
			a._23 =(M._00 * D2_3x0_X_2x0 - M._10 * D2_3x1_X_2x0 + M._20 * D2_3x2_X_2x0) * -1;
			a._30 =(M._10 * D2_1x0_X_3x0 - M._20 * D2_2x0_X_3x0 + M._30 * D2_3x0_X_3x0) * -1;
			a._31 =(M._00 * D2_1x0_X_3x0 - M._20 * D2_2x1_X_3x0 + M._30 * D2_3x1_X_3x0) * 1;
			a._32 =(M._00 * D2_2x0_X_3x0 - M._10 * D2_2x1_X_3x0 + M._30 * D2_3x2_X_3x0) * -1;
			a._33 =(M._00 * D2_3x0_X_3x0 - M._10 * D2_3x1_X_3x0 + M._20 * D2_3x2_X_3x0) * 1;
			return a;
		}


		// Matrix inverse
		inline Matrix4 Inverse( const Matrix4& m )
		{
			// inverse = Adjoint * (1/determinant).
			Scalar		det = Determinant(m);

			if( det == 0.0 ) {
				return m;
			}
			
			return ScalarMul( Adjoint(m), (1.0/det) );
		}

		// Matrix transpose
		inline Matrix4 Transpose( const Matrix4& m )
		{
			Matrix4 ret;
			ret._00 = m._00;
			ret._01 = m._10;
			ret._02 = m._20;
			ret._03 = m._30;
			ret._10 = m._01;
			ret._11 = m._11;
			ret._12 = m._21;
			ret._13 = m._31;
			ret._20 = m._02;
			ret._21 = m._12;
			ret._22 = m._22;
			ret._23 = m._32;
			ret._30 = m._03;
			ret._31 = m._13;
			ret._32 = m._23;
			ret._33 = m._33;
			return ret;
		}

		inline void Serialize( 
			const Matrix4& m, 
			IWriteBuffer& buffer )
		{
			buffer.ResizeForMore( sizeof( Scalar ) * 16 );
			buffer.setDouble( m._00 );
			buffer.setDouble( m._01 );
			buffer.setDouble( m._02 );
			buffer.setDouble( m._03 );
			buffer.setDouble( m._10 );
			buffer.setDouble( m._11 );
			buffer.setDouble( m._12 );
			buffer.setDouble( m._13 );
			buffer.setDouble( m._20 );
			buffer.setDouble( m._21 );
			buffer.setDouble( m._22 );
			buffer.setDouble( m._23 );
			buffer.setDouble( m._30 );
			buffer.setDouble( m._31 );
			buffer.setDouble( m._32 );
			buffer.setDouble( m._33 );
		}

		inline void Deserialize( 
			Matrix4& ret,
			IReadBuffer& buffer )
		{
			ret._00 = buffer.getDouble();
			ret._01 = buffer.getDouble();
			ret._02 = buffer.getDouble();
			ret._03 = buffer.getDouble();
			ret._10 = buffer.getDouble();
			ret._11 = buffer.getDouble();
			ret._12 = buffer.getDouble();
			ret._13 = buffer.getDouble();
			ret._20 = buffer.getDouble();
			ret._21 = buffer.getDouble();
			ret._22 = buffer.getDouble();
			ret._23 = buffer.getDouble();
			ret._30 = buffer.getDouble();
			ret._31 = buffer.getDouble();
			ret._32 = buffer.getDouble();
			ret._33 = buffer.getDouble();
		}
	}
}


#endif
