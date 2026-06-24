//////////////////////////////////////////////////////////////////////
//
//  IObjectPriv.h - Priviledged Interface to a rasterizable object
//    in out scene.  Allows it to do more stuff
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 28, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IOBJECTPRIV_
#define IOBJECTPRIV_

#include "IObject.h"
#include "IUVGenerator.h"
#include "ITransformable.h"
#include "IMaterial.h"
#include "IRayIntersectionModifier.h"
#include "IShader.h"
#include "IRadianceMap.h"
#include "IMedium.h"

namespace RISE
{

	//! The priviledged object interface allows things cloning and assigning
	//! of material properties
	/// \sa IObject
	class IObjectPriv : public virtual IObject, public virtual ITransformable
	{
	protected:
		IObjectPriv(){};
		virtual ~IObjectPriv(){};

	public:
		//
		// Priviledged interface functions
		//

		//! Clones the entire object
		virtual IObjectPriv* CloneFull() = 0;

		//! Clones on the geometric properties of the object
		virtual IObjectPriv* CloneGeometric() = 0;

		//! Assigns a material to the object
		virtual bool AssignMaterial(
			const IMaterial& pMat									///< [in] The material to assign
			) = 0;

		//! Assigns a geometry to the object at runtime (geometry swap).
		//! Mirrors AssignMaterial; the interactive editor's `geometry`
		//! row routes here via SceneEdit::SetObjectGeometry.  The object's
		//! bounding box is derived from the geometry, so the caller must
		//! invalidate the top-level acceleration afterward (next render
		//! rebuilds the TLAS against the new extents).
		virtual bool AssignGeometry(
			const IGeometry& pGeom									///< [in] The geometry to assign
			) = 0;

		//! Assigns a shader to the object
		virtual bool AssignShader( 
			const IShader& pShader									///< [in] The shader to assign
			) = 0;

		//! Assigns a radiance map to the object
		virtual bool AssignRadianceMap(
			const IRadianceMap& pRadianceMap						///< [in] The radiance map to assign
			) = 0;
		

		//! Assigns a ray intersection modifier to the object
		virtual bool AssignModifier(
			const IRayIntersectionModifier& pMod					///< [in] The modifier to assign
			) = 0;

		//! Assigns an interior participating medium to the object
		virtual bool AssignInteriorMedium(
			const IMedium& medium									///< [in] The medium to assign
			) = 0;

		//! Clears any previously-assigned interior medium so the
		//! object has no participating-medium interior (equivalent to
		//! the scene-file state where `interior_medium` is absent or
		//! set to `"none"`).  Used by the interactive editor's
		//! `interior_medium` row when the user selects the `(none)`
		//! preset — the AssignInteriorMedium reference signature has
		//! no null-sentinel, so we need a separate clear path for
		//! parser-parity round-trip.
		virtual void ClearInteriorMedium() = 0;

		//! Sets the epsilon error threshold to use when computing intersections
		virtual void SetSurfaceIntersecError(
			Scalar d												///< [in] Amount of error to assume when doing intersection calculations.  This value can range from 1e-3 to about 1e-12
			) = 0;

		//! Sets the UV generator to generate texture co-ordinates
		virtual bool SetUVGenerator( 
			const IUVGenerator& pUVG								///< [in] The UV generator
			) = 0;

		//! Sets the whether the object is visible to the world
		virtual void SetWorldVisible(
			bool b													///< [in] Should the object be world visible?
			) = 0;

		//! Sets how the object deals with shadows
		virtual void SetShadowParams( 
			const bool bCasts,										///< [in] Object casts shadows
			const bool bReceives									///< [in] Object receives shadows
			) = 0;

		//! Tells the object to reset its runtime data
		virtual void ResetRuntimeData() const = 0;

		//! Clear an OPTIONAL object slot back to "unset" -- the state a freshly created
		//! object has, and the state a full derive produces when the chunk omits the slot
		//! or sets it to "none".  The CST incremental apply (stable-object re-point) uses
		//! these when an edit REMOVES a slot the object previously held: an in-place
		//! re-point re-binds the slots the chunk specifies but cannot clear one it omits,
		//! and the AssignX reference signatures have no null-sentinel.  Added at the END of
		//! this interface (after ResetRuntimeData) so EXISTING method vtable offsets are
		//! unchanged -- the mid-vtable insertion the ClearShader/ClearMaterial P1-#9 note
		//! avoided does not apply to end-insertion (out-of-tree CALLERS keep their offsets).
		virtual void ClearMaterial() = 0;
		virtual void ClearModifier() = 0;
		virtual void ClearShader() = 0;
		virtual void ClearRadianceMap() = 0;
	};
}

#endif
