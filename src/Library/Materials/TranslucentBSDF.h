//////////////////////////////////////////////////////////////////////
//
//  TranslucentBSDF.h - Defines a translucent BSDF 
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 27, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////
	
#ifndef TRANSLUCENT_BRDF_
#define TRANSLUCENT_BRDF_

#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class TranslucentBSDF : public virtual IBSDF, public virtual Reference
		{
		protected:
			virtual ~TranslucentBSDF();

			//! Pointer storage so the interactive editor can rebind via
			//! Set*.  See LambertianBRDF for pattern + lifetime contract.
			//!
			//! DL-157 / DL-38 (2026-09-18): `pExtinction` and `pScat` are
			//! new here.  They are not decoration -- the interior lobes this
			//! BSDF now has to price ARE the Beer-attenuated, scattering-split
			//! pair the SPF samples (`kray = B*(1-s)` for the exit lobe,
			//! `B*s` for the backscatter), so a BSDF that cannot read those
			//! two painters cannot describe them.
			const IPainter*			pRefFront;			// Reflectance (color)
			const IPainter*			pTrans;				// Transmittance (color)
			const IScalarPainter*	pExponent;			// Phong exponent (physical scalar)
			const IScalarPainter*	pExtinction;		// Interior extinction (physical scalar)
			const IScalarPainter*	pScat;				// Interior scattering split (physical scalar)

		public:
			TranslucentBSDF( const IPainter& rF, const IPainter& T, const IScalarPainter& exp,
				const IScalarPainter& ext, const IScalarPainter& scat );

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri) const;
			virtual Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;

			//! DL-157(b): which SIDE a hit is on decides which two lobes
			//! exist, and `Scatter`/`Pdf` take that from the IOR stack.
			//! These are the overrides that let a caller holding the LIVE
			//! stack hand it over, so density, value and sampler cannot land
			//! in different branches; the stackless `value`/`valueNM` above
			//! infer the side geometrically (exact for a closed object --
			//! see `BuildLobeSet`'s contract in TranslucentSPF.h).
			virtual RISEPel valueStateful( const Vector3& vLightIn, const RayIntersectionGeometric& ri,
				const IORStack* pIORStack ) const;
			virtual Scalar valueStatefulNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri,
				const Scalar nm, const IORStack* pIORStack ) const;

			virtual RISEPel albedo( const RayIntersectionGeometric& ri ) const;

			//! Read-back + rebind for the interactive editor.  Exponent
			//! getter is named `GetN` for symmetry with the SPF's `N`
			//! parameter and the scene-file slot name.
			inline const IPainter&       GetRefFront()   const { return *pRefFront; }
			inline const IPainter&       GetTrans()      const { return *pTrans; }
			inline const IScalarPainter& GetN()          const { return *pExponent; }
			inline const IScalarPainter& GetExtinction() const { return *pExtinction; }
			inline const IScalarPainter& GetScat()       const { return *pScat; }
			void SetRefFront( const IPainter& v );
			void SetTrans( const IPainter& v );
			void SetN( const IScalarPainter& v );
			void SetExtinction( const IScalarPainter& v );
			void SetScat( const IScalarPainter& v );
		};
	}
}

#endif

