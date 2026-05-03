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
			const IMedium*									pInteriorMedium;

			bool											bIsWorldVisible;
			bool											bCastsShadows;
			bool											bReceivesShadows;

			Scalar											SURFACE_INTERSEC_ERROR;

			// Transpose of the inverse matrix, used for normal transformations
			// We do( M^-1)^T instead of M*n because this will work with shears and reflections
			Matrix4											m_mxInvTranspose;

			//! Sign of the tangent-frame chirality flip introduced by
			//! the object transform.  +1 for orientation-preserving
			//! transforms (rotation, translation, positive non-uniform
			//! scale); -1 for orientation-reversing transforms (e.g.
			//! `scale -1 1 1`, mirrored instances).  Computed from the
			//! sign of `det(m_mxFinalTrans)` in FinalizeTransformations()
			//! and applied to `ri.geometric.bitangentSign` at hit time
			//! so tangent-space normal maps render correctly on
			//! mirrored object instances of the same source mesh.
			Scalar											m_tangentFrameSign;

			virtual ~Object( );

		public:
			Object( );
			Object( const IGeometry* pGeometry_ );

			virtual IObjectPriv* CloneFull() override;
			virtual IObjectPriv* CloneGeometric() override;

			virtual bool AssignMaterial( const IMaterial& pMat ) override;
			virtual bool AssignModifier( const IRayIntersectionModifier& pMod ) override;
			virtual bool AssignShader( const IShader& pShader ) override;
			virtual bool AssignRadianceMap( const IRadianceMap& pRadianceMap ) override;
			virtual bool AssignInteriorMedium( const IMedium& medium ) override;

			virtual void IntersectRay( RayIntersection& ri, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const override;
			virtual bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;

			virtual bool IsWorldVisible() const override { return bIsWorldVisible; }
			virtual void SetWorldVisible( bool b ) override { bIsWorldVisible = b; }

			virtual bool DoesCastShadows() const override { return bCastsShadows; }
			virtual bool DoesReceiveShadows() const override { return bReceivesShadows; }

			virtual void SetSurfaceIntersecError( Scalar d ) override { SURFACE_INTERSEC_ERROR = d; }
			virtual bool SetUVGenerator( const IUVGenerator& pUVG ) override;
			virtual void SetShadowParams( const bool bCasts, const bool bReceives ) override;

			virtual const IMaterial* GetMaterial() const override;
			virtual const IShader*   GetShader() const override { return pShader; }
			virtual const IGeometry* GetGeometry() const override { return pGeometry; }
			virtual const IMedium* GetInteriorMedium() const override;

			virtual bool ComputeAnalyticalDerivatives(
				const Point2& uv,
				Scalar        smoothing,
				Point3&       outWorldPosition,
				Vector3&      outWorldNormal,
				Vector3&      outWorldDpdu,
				Vector3&      outWorldDpdv,
				Vector3&      outWorldDndu,
				Vector3&      outWorldDndv
				) const override;
			virtual void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const override;
			virtual Scalar GetArea( ) const override;

			virtual const BoundingBox getBoundingBox() const override;

			virtual void ResetRuntimeData() const override;

			void FinalizeTransformations( ) override;
		};
	}
}

#endif
