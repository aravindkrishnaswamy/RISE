//////////////////////////////////////////////////////////////////////
//
//  WeaveMaterial.h - `weave_material`: a structured two-thread-family
//    cloth BSDF (docs/CLOTH_FABRIC_DESIGN.md Phase 2, slice P2-A).
//
//  The triad is WeaveMaterial + WeaveBRDF + WeaveSPF; the pattern
//  functions and the preset table live in WeavePresets.h, and the
//  fibre-lobe primitives it shares with `hair_material` in
//  FibreLobeMath.h.  Read WeaveBRDF.h for the model and the energy
//  argument, WeaveSPF.h for the sampler.
//
//  WHY THIS IS A NEW MATERIAL AND NOT A MODE ON `fabric_material`.
//  9.9 gate 9b put a number on Phase 1's limit: 95 % (silk) / 99 %
//  (satin) of an anisotropic GGX substrate's highlight anisotropy
//  survives being wrapped in the isotropic Charlie sheen, so 9.5's
//  delegation is working almost losslessly -- and the result still
//  reads as brushed metal.  The deficit is the absence of a PATTERN
//  SCALE, which no amount of sheen tuning reaches.  Nothing in this
//  material is a sheen lobe; sharing a chunk with one would have meant
//  two disjoint parameter sets behind one keyword, each meaningless in
//  the other's mode.
//
//  THE TWO COMPOSE, AND THE STACK IS PHYSICAL.  `fabric_material`'s
//  substrate allowlist accepts `weave_material`, so the Phase-1 fuzz
//  layer sits OVER the structured weave -- which is the real thing:
//  surface fuzz is loose fibre ends standing off the woven cloth
//  underneath.  The energy bookkeeping composes correctly because this
//  material implements `hemisphericalAlbedo`, which is exactly what the
//  allowlist exists to require.
//
//  NO SHEEN TERM HERE, DELIBERATELY.  A `weave_material` on its own is
//  the woven cloth with no fuzz.  Adding a sheen lobe would duplicate
//  `fabric_material`'s, and the two would then have to agree about
//  which one subtracts the sheen's energy from the substrate -- the
//  exact double-counting `fabric_material` was built to end.
//
//  PHASE 2 SLICE A IS REFLECTION-ONLY.  `IsVolumetric`,
//  `ScattersFullSphere` and `CouldLightPassThrough` all stay at their
//  IMaterial defaults (false).  The backlit glow-through cue is slice
//  P2-B: it needs Zhu 2023's delta-transmission lobe
//  `delta(i+o)/(i.n_s)` and the full-sphere NEE machinery, and claiming
//  the flags without the lobe would send NEE hunting for transmission
//  that does not exist.
//
//  NO `GetSpecularInfo` OVERRIDE, for the same reason
//  `fabric_material` has none: neither lobe is a delta distribution, so
//  the ISPF default is correct and SMS ignores this material.
//  `SpecularInfo` also carries `canRefract` + `ior`, which the SMS
//  solver and IOR-stack seeding read as "rays cross a refractive
//  boundary here" -- the per-thread `eta` is a FIBRE property consumed
//  inside the lobe, not a boundary anything crosses, and reporting it
//  would be a lie to both subsystems.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef WEAVE_MATERIAL_
#define WEAVE_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/ILog.h"
#include "WeaveBRDF.h"
#include "WeaveSPF.h"
#include "WeavePresets.h"

namespace RISE
{
	namespace Implementation
	{
		class WeaveMaterial : public virtual IMaterial, public virtual Reference
		{
		protected:
			WeaveBRDF*	pBRDF;
			WeaveSPF*	pSPF;

			virtual ~WeaveMaterial()
			{
				safe_release( pBRDF );
				safe_release( pSPF );
			}

		public:
			WeaveMaterial(
				const WeavePatternKind pattern,
				const IScalarPainter& weaveScale,
				const IScalarPainter& weaveRotation,
				const IScalarPainter& weftSkew,
				const IScalarPainter* coverage,		///< NULL unless `pattern == eWeaveCustom`
				const IScalarPainter& gap,
				const IPainter& warpColor,
				const IScalarPainter& warpIOR,
				const IScalarPainter& warpWidth,
				const IScalarPainter& warpAzimuth,
				const IScalarPainter& warpKd,
				const IScalarPainter& warpTilt,
				const IPainter& weftColor,
				const IScalarPainter& weftIOR,
				const IScalarPainter& weftWidth,
				const IScalarPainter& weftAzimuth,
				const IScalarPainter& weftKd,
				const IScalarPainter& weftTilt
				)
			{
				pBRDF = new WeaveBRDF( pattern, weaveScale, weaveRotation, weftSkew, coverage, gap,
				                       warpColor, warpIOR, warpWidth, warpAzimuth, warpKd, warpTilt,
				                       weftColor, weftIOR, weftWidth, weftAzimuth, weftKd, weftTilt );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new WeaveSPF( *pBRDF );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );
			}

			/// \return The BRDF for this material.  Never NULL.
			inline IBSDF* GetBSDF() const { return pBRDF; }

			/// \return The SPF for this material.  Never NULL.
			inline ISPF* GetSPF() const { return pSPF; }

			/// \return NULL: a weave never emits.
			inline IEmitter* GetEmitter() const { return 0; }

			//! Read-back for the interactive editor / snapshot clone.
			//! Every slot is forwarded from the BRDF, which holds the one
			//! copy of the state.
			inline WeavePatternKind      GetPattern()       const { return pBRDF->GetPattern(); }
			inline const IScalarPainter& GetWeaveScale()    const { return pBRDF->GetWeaveScale(); }
			inline const IScalarPainter& GetWeaveRotation() const { return pBRDF->GetWeaveRotation(); }
			inline const IScalarPainter& GetWeftSkew()      const { return pBRDF->GetWeftSkew(); }
			inline const IScalarPainter* GetCoverage()      const { return pBRDF->GetCoverage(); }
			inline const IScalarPainter& GetGap()           const { return pBRDF->GetGap(); }
			inline const IPainter&       GetWarpColor()     const { return pBRDF->GetWarpColor(); }
			inline const IScalarPainter& GetWarpIOR()       const { return pBRDF->GetWarpIOR(); }
			inline const IScalarPainter& GetWarpWidth()     const { return pBRDF->GetWarpWidth(); }
			inline const IScalarPainter& GetWarpAzimuth()   const { return pBRDF->GetWarpAzimuth(); }
			inline const IScalarPainter& GetWarpKd()        const { return pBRDF->GetWarpKd(); }
			inline const IScalarPainter& GetWarpTilt()      const { return pBRDF->GetWarpTilt(); }
			inline const IPainter&       GetWeftColor()     const { return pBRDF->GetWeftColor(); }
			inline const IScalarPainter& GetWeftIOR()       const { return pBRDF->GetWeftIOR(); }
			inline const IScalarPainter& GetWeftWidth()     const { return pBRDF->GetWeftWidth(); }
			inline const IScalarPainter& GetWeftAzimuth()   const { return pBRDF->GetWeftAzimuth(); }
			inline const IScalarPainter& GetWeftKd()        const { return pBRDF->GetWeftKd(); }
			inline const IScalarPainter& GetWeftTilt()      const { return pBRDF->GetWeftTilt(); }

			//! Rebind for the interactive editor.  Only the BRDF is
			//! touched -- `WeaveSPF` reads every parameter back through it
			//! (WeaveSPF.h), so there is no second copy to keep in
			//! lockstep (contrast SheenMaterial / GGXMaterial, which must
			//! forward to BOTH their BRDF and their SPF).
			//!
			//! `weave` is deliberately NOT rebindable: it selects which
			//! cell function -- and, for `custom`, whether the `coverage`
			//! painter is read at all -- so changing it re-authors the
			//! material's structure rather than one of its fields.
			//! Re-author the chunk.  Same call as `fabric_material`'s
			//! non-rebindable `base`.
			inline void SetWeaveScale( const IScalarPainter& v )    { pBRDF->SetWeaveScale( v ); }
			inline void SetWeaveRotation( const IScalarPainter& v ) { pBRDF->SetWeaveRotation( v ); }
			inline void SetWeftSkew( const IScalarPainter& v )      { pBRDF->SetWeftSkew( v ); }
			inline void SetCoverage( const IScalarPainter& v )      { pBRDF->SetCoverage( v ); }
			inline void SetGap( const IScalarPainter& v )           { pBRDF->SetGap( v ); }
			inline void SetWarpColor( const IPainter& v )           { pBRDF->SetWarpColor( v ); }
			inline void SetWarpIOR( const IScalarPainter& v )       { pBRDF->SetWarpIOR( v ); }
			inline void SetWarpWidth( const IScalarPainter& v )     { pBRDF->SetWarpWidth( v ); }
			inline void SetWarpAzimuth( const IScalarPainter& v )   { pBRDF->SetWarpAzimuth( v ); }
			inline void SetWarpKd( const IScalarPainter& v )        { pBRDF->SetWarpKd( v ); }
			inline void SetWarpTilt( const IScalarPainter& v )      { pBRDF->SetWarpTilt( v ); }
			inline void SetWeftColor( const IPainter& v )           { pBRDF->SetWeftColor( v ); }
			inline void SetWeftIOR( const IScalarPainter& v )       { pBRDF->SetWeftIOR( v ); }
			inline void SetWeftWidth( const IScalarPainter& v )     { pBRDF->SetWeftWidth( v ); }
			inline void SetWeftAzimuth( const IScalarPainter& v )   { pBRDF->SetWeftAzimuth( v ); }
			inline void SetWeftKd( const IScalarPainter& v )        { pBRDF->SetWeftKd( v ); }
			inline void SetWeftTilt( const IScalarPainter& v )      { pBRDF->SetWeftTilt( v ); }
		};
	}
}

#endif
