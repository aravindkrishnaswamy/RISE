//////////////////////////////////////////////////////////////////////
//
//  Object.h - Defines a rasterizable object within our scene.  This
//  Simple object class has only one geometry object, with a 
//  material.  this object is also transformable
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 2, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef OBJECT_
#define OBJECT_

#include "../Interfaces/IObjectPriv.h"
#include "../Interfaces/IGeometry.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IRayIntersectionModifier.h"
#include "../Utilities/Transformable.h"
#include "../Utilities/RString.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class Object : public virtual IObjectPriv, public virtual Transformable, public virtual Reference
		{
		protected:
			const IGeometry*								pGeometry;
			const IUVGenerator*								pUVGenerator;
			const IMaterial*								pMaterial;
			const IRayIntersectionModifier*					pModifier;
			const IShader*									pShader;
			const IRadianceMap*								pRadianceMap;

			bool											bIsWorldVisible;
			bool											bCastsShadows;
			bool											bReceivesShadows;

			Scalar											SURFACE_INTERSEC_ERROR;

			// Transpose of the inverse matrix, used for normal transformations
			// We do( M^-1)^T instead of M*n because this will work with shears and reflections
			Matrix4											m_mxInvTranspose;

			virtual ~Object( );

		public:
			Object( );
			Object( const IGeometry* pGeometry_ );

			virtual IObjectPriv* CloneFull();
			virtual IObjectPriv* CloneGeometric();

			virtual bool AssignMaterial( const IMaterial& pMat );
			virtual bool AssignModifier( const IRayIntersectionModifier& pMod );
			virtual bool AssignShader( const IShader& pShader );
			virtual bool AssignRadianceMap( const IRadianceMap& pRadianceMap );

			virtual void IntersectRay( RayIntersection& ri, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const;
			virtual bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const;

			virtual bool IsWorldVisible() const { return bIsWorldVisible; }
			virtual void SetWorldVisible( bool b ){ bIsWorldVisible = b; }

			virtual bool DoesCastShadows() const { return bCastsShadows; }
			virtual bool DoesReceiveShadows() const { return bReceivesShadows; }

			virtual void SetSurfaceIntersecError( Scalar d ){ SURFACE_INTERSEC_ERROR = d; }
			virtual bool SetUVGenerator( const IUVGenerator& pUVG );
			virtual void SetShadowParams( const bool bCasts, const bool bReceives );

			virtual const IMaterial* GetMaterial() const;
			virtual void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const;
			virtual Scalar GetArea( ) const;

			virtual const BoundingBox getBoundingBox() const;

			virtual void ResetRuntimeData() const;

			void FinalizeTransformations( );
		};
	}
}

#endif
