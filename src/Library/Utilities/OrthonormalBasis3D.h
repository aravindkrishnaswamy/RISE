//////////////////////////////////////////////////////////////////////
//
//  OrthonormalBasis3D.h - Definition of an orthonormal basis in R^3
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 20, 2001
//  Tabs: 4
//  Comments: Much here is influenced by ggLibrary
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ORTHONORMALBASIS_3D_
#define ORTHONORMALBASIS_3D_

#include "Math3D/Math3D.h"

namespace RISE
{
	class OrthonormalBasis3D
	{
	protected:
		Vector3	U, V, W;

	public:
		OrthonormalBasis3D( ) :
		U( Vector3( 1, 0, 0 ) ),
		V( Vector3( 0, 1, 0 ) ),
		W( Vector3( 0, 0, 1 ) )
		{
		}

		OrthonormalBasis3D( const Vector3& U_, const Vector3& V_, const Vector3& W_ ) :
		U( U_ ),
		V( V_ ),
		W( W_ )
		{
		}

		OrthonormalBasis3D( const OrthonormalBasis3D& b ) :
		U( b.U ), V( b.V ), W( b.W )
		{}	

		virtual ~OrthonormalBasis3D( ){};

		//
		// Calculate an ONB from just one vector
		//
		void CreateFromU( const Vector3& u );
		void CreateFromV( const Vector3& v );
		void CreateFromW( const Vector3& w );

		// Calculate an ONB from two vectors
		// The first one is the Fixed vector (it is just normalized)
		// The second is normalized and its direction can be ajusted
		void CreateFromUV( const Vector3& u, const Vector3& v );
		void CreateFromVU( const Vector3& v, const Vector3& u );

		void CreateFromUW( const Vector3& u, const Vector3& w );
		void CreateFromWU( const Vector3& w, const Vector3& u );

		void CreateFromVW( const Vector3& v, const Vector3& w );
		void CreateFromWV( const Vector3& w, const Vector3& v );

		//
		// Getters
		//
		inline Vector3	u() const { return U; };
		inline Vector3	v() const { return V; };
		inline Vector3	w() const { return W; };

		inline	void FlipW() { W = -W; U = -U; }

		//! Negates V ONLY, leaving U and W untouched -- deliberately turns a
		//! right-handed (U,V,W) triple into a left-handed one.  This is the
		//! mirrored-instance chirality correction a geometry-supplied tangent
		//! needs: when a shading tangent is built from an OBJECT-space source
		//! (dpdu / an imported vTangent) forward-transformed into world space
		//! while the normal is transformed by the INVERSE-TRANSPOSE, the two
		//! promotions disagree under a negative-determinant (mirrored) linear
		//! map -- V = normalize(cross(W,U)) ends up pointing the opposite way
		//! from the correctly-mirrored bitangent.  Exactly the same asymmetry
		//! is why NormalMap's cross(N,T)-built bitangent is corrected by
		//! multiplying `bitangentSign` by `m_tangentFrameSign` -- this method
		//! lets a caller apply the identical correction directly to an ONB's
		//! V axis (Object::IntersectRay / CSGObject::IntersectRay's
		//! bShadingTangentFromGeometry branch).  Unlike FlipW() above, U is
		//! deliberately NOT touched: it is the promoted tangent direction
		//! itself (dpdu / vTangent forward-transformed, no sign ambiguity --
		//! see GeometryShadingTangentTest.cpp's money assertions), and must
		//! stay exactly that value under mirroring for tangent_rotation's
		//! base axis to remain correct.
		inline	void FlipV() { V = -V; }

		// Generates a transformation matrix for this ONB
		Matrix4	GetBasisToCanonicalMatrix( ) const;
		Matrix4	GetCanonicalToBasisMatrix( ) const;

		// Transforms a vector from Canonical to this casis
		Vector3 Transform( const Vector3& v ) const;
	};
}

#endif

