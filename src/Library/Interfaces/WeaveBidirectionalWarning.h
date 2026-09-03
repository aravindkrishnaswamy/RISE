//////////////////////////////////////////////////////////////////////
//
//  WeaveBidirectionalWarning.h - One-time diagnostic: a `transmission
//    thin` weave_material rendered under BDPT or VCM.
//
//  WHY.  docs/CLOTH_FABRIC_DESIGN.md §15 / REVIEW_P2R7.md P2: a flat,
//  zero-thickness `IMaterial::ScattersFullSphere()` surface (a sheer
//  weave is RISE's first) triggers an unbounded vertex-connection
//  geometric-term singularity in both BDPT (`BDPTUtilities::GeometricTerm`)
//  and VCM (`VCMIntegrator.cpp`'s merge/connection paths): an eye-subpath
//  vertex on the FRONT face and a light-subpath vertex on the BACK face
//  of the same infinitesimally-thin quad can land arbitrarily close
//  together, driving `cosA*cosB/dist^2` toward infinity while both
//  cosines stay near 1.  Measured 100-350x over PT on the shipped
//  backlit-curtain scene (tests/FabricRenderTest.cpp).  Fixing the
//  integrators' connection-distance regularization is out of scope for
//  this slice (docs/RENDERING_INTEGRATORS.md's known-limitations
//  section); `AutoRasterizer`'s Tier-1 heuristic was fixed separately
//  (`AutoRasterizer.cpp`'s `SceneHasTransmissiveMaterial`, gated on
//  `!ScattersFullSphere()`) so `> render auto` no longer ROUTES here at
//  all -- this warning is for the author who explicitly pins
//  `bdpt_*_rasterizer` / `vcm_*_rasterizer` on such a scene anyway.
//
//  Call once from each BDPT/VCM concrete rasterizer's own pre-render
//  hook (never per pixel or per sample) -- see
//  BDPTPelRasterizer::PreRenderSetup, BDPTSpectralRasterizer::
//  PreRenderSetup and VCMRasterizerBase::PreRenderSetup for the three
//  call sites (VCM's Pel/Spectral variants share one base-class
//  override, unlike BDPT's diamond-inheritance split).  Cheap: one
//  early-out object enumeration, mirroring
//  AutoRasterizer.cpp's `SceneHasTransmissiveMaterial` /
//  `WarnIfNonPTRenderHasLiveSignalConsumer`'s (ISurfaceSignalProvider.h)
//  own call-site convention.
//
//  `pLog` may be null (defensive; every call site has a live log in
//  practice) -- a null log means "cannot report," not "nothing to
//  report," so the check is skipped silently rather than crashing.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef WEAVE_BIDIRECTIONAL_WARNING_
#define WEAVE_BIDIRECTIONAL_WARNING_

#include "IScene.h"
#include "IObjectManager.h"
#include "IObject.h"
#include "IMaterial.h"
#include "IEnumCallback.h"
#include "ILog.h"

namespace RISE
{
	//! True iff any visible object's material is a full-sphere
	//! transmissive one -- today, exactly `weave_material` under
	//! `transmission thin` (P2-B).  Deliberately the same
	//! `CouldLightPassThrough() && ScattersFullSphere()` conjunction
	//! `AutoRasterizer.cpp`'s Tier-1 heuristic uses to EXCLUDE this
	//! material class from its own VCM routing signal -- here it is
	//! the thing being detected, there the thing being ruled out.
	inline bool SceneHasFullSphereTransmissiveMaterial( const IScene& scene )
	{
		struct Scan : public IEnumCallback<IObject>
		{
			bool found;
			Scan() : found( false ) {}
			bool operator()( const IObject& obj )
			{
				const IMaterial* mat = obj.GetMaterial();
				if( mat && mat->CouldLightPassThrough() && mat->ScattersFullSphere() ) {
					found = true;
					return false;	// one is enough -- stop enumeration
				}
				return true;		// keep scanning
			}
		};

		const IObjectManager* objs = scene.GetObjects();
		if( !objs ) {
			return false;
		}
		Scan scan;
		objs->EnumerateObjects( scan );
		return scan.found;
	}

	//! See the file header.  `familyName` is "BDPT" or "VCM" for the
	//! log line.
	inline void WarnIfBidirectionalRenderHasFullSphereTransmissive(
		const IScene& scene, ILog* pLog, const char* familyName )
	{
		if( !pLog || !familyName ) return;
		if( SceneHasFullSphereTransmissiveMaterial( scene ) ) {
			pLog->PrintEx( eLog_Warning,
				"%s:: this scene has a full-sphere transmissive material (e.g. a "
				"`weave_material` under `transmission thin`) -- %s's vertex-connection "
				"geometric term is unguarded at short distance and can read 100-350x too "
				"bright on a flat, zero-thickness surface of this kind (measured, "
				"docs/CLOTH_FABRIC_DESIGN.md section 15).  Prefer a PT rasterizer for this "
				"material class until that integrator limitation is fixed.",
				familyName, familyName );
		}
	}
}

#endif
