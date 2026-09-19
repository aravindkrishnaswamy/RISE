//////////////////////////////////////////////////////////////////////
//
//  TranslucentMaterial.h - Defines a material that is partially
//  transparent (like a lampshade)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 27, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TRANSLUCENT_MATERIAL_
#define TRANSLUCENT_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/ILog.h"
#include "TranslucentBSDF.h"
#include "TranslucentSPF.h"

namespace RISE
{
	namespace Implementation
	{
		class TranslucentMaterial :
			public virtual IMaterial, 
			public virtual Reference
		{
		protected:
			TranslucentBSDF*				pBRDF;
			TranslucentSPF*					pSPF;

			virtual ~TranslucentMaterial( )
			{
				safe_release( pBRDF );
				safe_release( pSPF );
			}

		public:
			TranslucentMaterial( const IPainter& rF, const IPainter& T, const IScalarPainter& ext, const IScalarPainter& N_, const IScalarPainter& scat )
			{
				// DL-157/DL-38: the BSDF now prices the interior lobes too,
				// which ARE the Beer-attenuated / scattering-split pair, so
				// it needs `ext` and `scat` as well.
				pBRDF = new TranslucentBSDF( rF, T, N_, ext, scat );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new TranslucentSPF( rF, T, ext, N_, scat );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );
			}

			/// \return The BRDF for this material.  NULL If there is no BRDF
			inline IBSDF* GetBSDF() const {			return pBRDF; };

			/// \return The SPF for this material.  NULL If there is no SPF
			inline ISPF* GetSPF() const {			return pSPF; };

			/// \return The emission properties for this material.  NULL If there is not an emitter
			inline IEmitter* GetEmitter() const {	return 0; };

			// Translucent materials scatter light diffusely through the
			// surface, so straight-line camera connections (t=0, t=1) through
			// them are unphysical.  Light transport through translucent objects
			// is handled correctly by the eye/light subpath tracing.
			inline bool CouldLightPassThrough() const { return false; };

			// DL-46: TranslucentSPF::Scatter/ScatterNM classify entry vs.
			// exit purely from `ior_stack.containsCurrent()` -- exactly
			// like a real dielectric, an object whose interior a ray can
			// be "inside".  The base IMaterial default (isSpecular=false,
			// canRefract=false, valid=false) meant
			// IORStackSeeding::SeedFromPoint's probe -- which only tracks
			// materials reporting `canRefract` -- silently skipped this
			// material, so a camera/light origin already inside a closed
			// translucent object was never seeded with its membership, and
			// the first physical crossing was misclassified as an entry
			// instead of an exit.
			//
			// This is NOT a specular material (its lobes are diffuse / Phong,
			// sampled stochastically, never delta) and it carries no
			// distinct IOR of its own -- interior segments re-push the
			// ENCLOSING medium's IOR unchanged (see the entry/backscatter
			// comments in TranslucentSPF::Scatter).  Report that
			// stateful-but-non-refracting nature via `hasInterior` rather
			// than lying about specularity/refraction just to be picked up
			// by the seeding probe; see SpecularInfo.h and
			// docs/SUBMERGED_CAMERA_IOR_SEEDING.md.  `ior` is left at its
			// default (1.0) -- informational only, since SeedFromPoint
			// re-pushes the caller's own current stack top for a
			// `hasInterior`-only entry instead of reading this field,
			// matching Scatter()'s own `push(ior_stack.top())`.
			inline SpecularInfo GetSpecularInfo(
				const RayIntersectionGeometric&,
				const IORStack&
				) const
			{
				SpecularInfo info;
				info.valid = true;
				info.isSpecular = false;
				info.canRefract = false;
				info.hasInterior = true;
				return info;
			}

			//! DL-157 (2026-09-18): `translucent_material`'s BSDF genuinely
			//! transmits -- `TranslucentBSDF::value` now prices the entry
			//! transmission lobe and, on the interior side, the backscatter
			//! lobe, both of which live BELOW the shading horizon.  Without
			//! this override `LightSampler.cpp`'s three NEE arms `break` at
			//! `cosSurface <= 0` and never light them, while PT's own
			//! BSDF-sampling side still multiplies its below-horizon
			//! emitter hits by `w_bsdf = PowerHeuristic(p_b, p_l) < 1` --
			//! the two strategies then sum to less than 1 over the whole
			//! transmissive half-space and the estimator reads
			//! systematically UNDER.  BDPT/VCM never had that gate
			//! (`PathVertexEval::EvalBSDFAtVertex` has no hemisphere test and
			//! `BDPTUtilities::GeometricTerm` takes `fabs` of both cosines),
			//! which is exactly the PT-vs-BDPT asymmetry DL-157 recorded.
			//!
			//! The capability's own safety condition (see IMaterial.h and
			//! LightSampler.cpp's FULL-SPHERE NEE block) is that the
			//! material's `value()` must really transmit and its aggregate
			//! `Pdf()` must really have support there, so the MIS partition
			//! closes.  Both became true in the same slice: DL-157 for
			//! `value`, DL-41 for `Pdf`.  Granting it BEFORE those would
			//! have lit back faces at full weight against a zero partner
			//! density.
			inline bool ScattersFullSphere() const { return true; }

			//! Read-back + rebind for the interactive editor.  All five
			//! parameters now exist on both BSDF and SPF — Material forwards
			//! in lockstep.
			inline const IPainter&       GetRefFront()   const { return pSPF->GetRefFront(); }
			inline const IPainter&       GetTrans()      const { return pSPF->GetTrans(); }
			inline const IScalarPainter& GetExtinction() const { return pSPF->GetExtinction(); }
			inline const IScalarPainter& GetN()          const { return pSPF->GetN(); }
			inline const IScalarPainter& GetScat()       const { return pSPF->GetScat(); }
			inline void SetRefFront( const IPainter& v )         { pBRDF->SetRefFront( v ); pSPF->SetRefFront( v ); }
			inline void SetTrans( const IPainter& v )            { pBRDF->SetTrans( v );    pSPF->SetTrans( v ); }
			inline void SetExtinction( const IScalarPainter& v ) { pBRDF->SetExtinction( v ); pSPF->SetExtinction( v ); }
			inline void SetN( const IScalarPainter& v )          { pBRDF->SetN( v );        pSPF->SetN( v ); }
			inline void SetScat( const IScalarPainter& v )       { pBRDF->SetScat( v );     pSPF->SetScat( v ); }
		};
	}
}

#endif

