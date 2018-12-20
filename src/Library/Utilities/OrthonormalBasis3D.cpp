	//////////////////////////////////////////////////////////////////////
//
//  OrthonormalBasis3D.cpp - Implementation of an orthonormal basis in R^3
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 20, 2001
//  Tabs: 4
//  Comments: Much here is influenced by ggLibrary
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "OrthonormalBasis3D.h"

using namespace RISE;

static const Vector3 canonicalU( 1, 0, 0 );
static const Vector3 canonicalV( 0, 1, 0 );
static const Vector3 canonicalW( 0, 0, 1 );

void OrthonormalBasis3D::CreateFromU( const Vector3& u )
{
	U = Vector3Ops::Normalize(u);
	V = Vector3Ops::Normalize(Vector3Ops::Perpendicular(u));
	W = Vector3Ops::Cross( U, V );
}

void OrthonormalBasis3D::CreateFromV( const Vector3& v )
{
	V = Vector3Ops::Normalize(v);
	U = Vector3Ops::Normalize(Vector3Ops::Perpendicular(v));
	W = Vector3Ops::Cross( U, V );
}

void OrthonormalBasis3D::CreateFromW( const Vector3& w )
{
	W = Vector3Ops::Normalize(w);

	if( fabs(Vector3Ops::Dot(W,canonicalV) - 1)  < NEARZERO )
	{
		U = canonicalW;
		V = canonicalU;
	}
	else if ( fabs(Vector3Ops::Dot(W,canonicalV) + 1)  < NEARZERO )
	{
		U = canonicalU;
		V = canonicalW;
	}
	else
	{
		U = Vector3Ops::Normalize(Vector3Ops::Cross( W, canonicalV ));
		V = Vector3Ops::Normalize(Vector3Ops::Cross( W, U ));
	}
}

void OrthonormalBasis3D::CreateFromUV( const Vector3& u, const Vector3& v )
{
	U = Vector3Ops::Normalize(u);
	W = Vector3Ops::Normalize(Vector3Ops::Cross( U, v ));
	V = Vector3Ops::Cross( W, U );
}

void OrthonormalBasis3D::CreateFromVU( const Vector3& v, const Vector3& u )
{
	V = Vector3Ops::Normalize(v);
	W = Vector3Ops::Normalize(Vector3Ops::Cross( V, u ));
	U = Vector3Ops::Cross( W, V );
}

void OrthonormalBasis3D::CreateFromUW( const Vector3& u, const Vector3& w )
{
	U = Vector3Ops::Normalize(u);
	V = Vector3Ops::Normalize(Vector3Ops::Cross( w, U ));
	W = Vector3Ops::Cross( U, V );
}

void OrthonormalBasis3D::CreateFromWU( const Vector3& w, const Vector3& u )
{
	W = Vector3Ops::Normalize(w);
	V = Vector3Ops::Normalize(Vector3Ops::Cross( W, u ));
	U = Vector3Ops::Cross( V, W );
}

void OrthonormalBasis3D::CreateFromVW( const Vector3& v, const Vector3& w )
{
	V = Vector3Ops::Normalize(v);
	U = Vector3Ops::Normalize(Vector3Ops::Cross( V, w ));
	W = Vector3Ops::Cross( U, W );
}

void OrthonormalBasis3D::CreateFromWV( const Vector3& w, const Vector3& v )
{
	W = Vector3Ops::Normalize(w);
	U = Vector3Ops::Normalize(Vector3Ops::Cross( v, W ));
	V = Vector3Ops::Cross( W, U );
}

Matrix4 OrthonormalBasis3D::GetBasisToCanonicalMatrix( ) const
{
	Matrix4	m;

	m._00 = U.x;
	m._01 = U.y;
	m._02 = U.z;

	m._10 = V.x;
	m._11 = V.y;
	m._12 = V.z;

	m._20 = W.x;
	m._21 = W.y;
	m._22 = W.z;

	return m;
}

Matrix4 OrthonormalBasis3D::GetCanonicalToBasisMatrix( ) const
{
	Matrix4	m;

	m._00 = U.x;
	m._10 = U.y;
	m._20 = U.z;

	m._01 = V.x;
	m._11 = V.y;
	m._21 = V.z;

	m._02 = W.x;
	m._12 = W.y;
	m._22 = W.z;

	return m;
}

Vector3 OrthonormalBasis3D::Transform( const Vector3& v ) const
{
	return Vector3(
			U.x*v.x + V.x*v.y + W.x*v.z, 
			U.y*v.x + V.y*v.y + W.y*v.z, 
			U.z*v.x + V.z*v.y + W.z*v.z);
}

