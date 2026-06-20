//////////////////////////////////////////////////////////////////////
//
//  SnapshotLeafClone.cpp - see SnapshotLeafClone.h.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SnapshotLeafClone.h"

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ILog.h"

// Concrete material types, in the same widest-first order
// MaterialIntrospection uses.  Only the faithfully-ctor-clonable ones are
// reconstructed; the rest fall through to the addref tail.
#include "../Materials/LambertianMaterial.h"
#include "../Materials/PerfectReflectorMaterial.h"
#include "../Materials/PerfectRefractorMaterial.h"
#include "../Materials/PolishedMaterial.h"
#include "../Materials/DielectricMaterial.h"
#include "../Materials/IsotropicPhongMaterial.h"
#include "../Materials/OrenNayarMaterial.h"
#include "../Materials/SchlickMaterial.h"
#include "../Materials/SheenMaterial.h"
#include "../Materials/AshikminShirleyAnisotropicPhongMaterial.h"
#include "../Materials/CookTorranceMaterial.h"
#include "../Materials/GGXMaterial.h"
#include "../Materials/WardIsotropicGaussianMaterial.h"
#include "../Materials/WardAnisotropicEllipticalGaussianMaterial.h"
#include "../Materials/TranslucentMaterial.h"

// Factories used to rebuild lights / media / cameras from their public
// construction state (same approach the material clones use — see header).
#include "../RISE_API.h"
#include "../Rendering/RadianceMap.h"   // F8: radiance-map snapshot clone

// Concrete light types (CloneLightForSnapshot).
#include "../Interfaces/ILightPriv.h"
#include "../Lights/PointLight.h"
#include "../Lights/SpotLight.h"
#include "../Lights/DirectionalLight.h"
#include "../Lights/AmbientLight.h"

// Concrete medium types (CloneMediumForSnapshot).
#include "../Interfaces/IMedium.h"
#include "../Materials/HomogeneousMedium.h"

// Concrete camera types (CloneCameraForSnapshot).
#include "../Interfaces/ICamera.h"
#include "../Cameras/CameraCommon.h"
#include "../Cameras/PinholeCamera.h"
#include "../Cameras/ThinLensCamera.h"
#include "../Cameras/OrthographicCamera.h"
#include "../Cameras/FisheyeCamera.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	// Build + log a fresh material clone uniformly.
	template <class T>
	T* NewClone( T* p )
	{
		GlobalLog()->PrintNew( p, __FILE__, __LINE__, "snapshot material clone" );
		return p;
	}
}

const IMaterial* RISE::Implementation::CloneMaterialForSnapshot( const IMaterial* mat )
{
	if( !mat ) {
		return 0;
	}

	// ---- Faithfully-clonable standard materials -------------------------
	// Each rebuilds the concrete type from its public painter-slot
	// accessors; the sub-painters are addref-shared by the ctors.

	if( const LambertianMaterial* m = dynamic_cast<const LambertianMaterial*>( mat ) ) {
		return NewClone( new LambertianMaterial( m->GetReflectance() ) );
	}
	if( const PerfectReflectorMaterial* m = dynamic_cast<const PerfectReflectorMaterial*>( mat ) ) {
		return NewClone( new PerfectReflectorMaterial( m->GetReflectance() ) );
	}
	if( const PerfectRefractorMaterial* m = dynamic_cast<const PerfectRefractorMaterial*>( mat ) ) {
		return NewClone( new PerfectRefractorMaterial( m->GetRefractivity(), m->GetIOR() ) );
	}
	if( const PolishedMaterial* m = dynamic_cast<const PolishedMaterial*>( mat ) ) {
		return NewClone( new PolishedMaterial(
			m->GetDiffuseReflectance(), m->GetTransmittance(),
			m->GetIOR(), m->GetScattering(), m->GetHG() ) );
	}
	if( const DielectricMaterial* m = dynamic_cast<const DielectricMaterial*>( mat ) ) {
		return NewClone( new DielectricMaterial(
			m->GetTransmittance(), m->GetIOR(), m->GetScattering(), m->GetHG(),
			m->GetAnodizationN(), m->GetAnodizationK(), m->GetAnodizationThickness() ) );
	}
	if( const IsotropicPhongMaterial* m = dynamic_cast<const IsotropicPhongMaterial*>( mat ) ) {
		return NewClone( new IsotropicPhongMaterial( m->GetRd(), m->GetRs(), m->GetExponent() ) );
	}
	if( const OrenNayarMaterial* m = dynamic_cast<const OrenNayarMaterial*>( mat ) ) {
		return NewClone( new OrenNayarMaterial( m->GetReflectance(), m->GetRoughness() ) );
	}
	if( const SchlickMaterial* m = dynamic_cast<const SchlickMaterial*>( mat ) ) {
		return NewClone( new SchlickMaterial(
			m->GetDiffuse(), m->GetSpecular(), m->GetRoughness(), m->GetIsotropy() ) );
	}
	if( const SheenMaterial* m = dynamic_cast<const SheenMaterial*>( mat ) ) {
		return NewClone( new SheenMaterial( m->GetColor(), m->GetRoughness() ) );
	}
	if( const AshikminShirleyAnisotropicPhongMaterial* m =
	    dynamic_cast<const AshikminShirleyAnisotropicPhongMaterial*>( mat ) ) {
		return NewClone( new AshikminShirleyAnisotropicPhongMaterial(
			m->GetNu(), m->GetNv(), m->GetRd(), m->GetRs() ) );
	}
	if( const CookTorranceMaterial* m = dynamic_cast<const CookTorranceMaterial*>( mat ) ) {
		return NewClone( new CookTorranceMaterial(
			m->GetDiffuse(), m->GetSpecular(), m->GetMasking(),
			m->GetIOR(), m->GetExtinction() ) );
	}
	if( const GGXMaterial* m = dynamic_cast<const GGXMaterial*>( mat ) ) {
		// The emissive GGX variant bakes an emitter + scale that are not
		// fully readable here.  It is the `ggx_emissive_material` composed
		// type, which SceneEditor::SetMaterialProperty rejects up-front via
		// IsMaterialComposed (so it is not slot-rebound through the editor);
		// addref-sharing it is therefore acceptable.  Only the non-emissive
		// GGX is reconstructed.
		if( !m->GetEmitter() ) {
			return NewClone( new GGXMaterial(
				m->GetDiffuse(), m->GetSpecular(), m->GetAlphaX(), m->GetAlphaY(),
				m->GetIOR(), m->GetExtinction(), m->GetFresnelMode(),
				m->GetTangentRotation(),
				m->GetFilmIOR(), m->GetFilmExtinction(), m->GetFilmThickness() ) );
		}
	}
	if( const WardIsotropicGaussianMaterial* m =
	    dynamic_cast<const WardIsotropicGaussianMaterial*>( mat ) ) {
		return NewClone( new WardIsotropicGaussianMaterial(
			m->GetDiffuse(), m->GetSpecular(), m->GetAlpha() ) );
	}
	if( const WardAnisotropicEllipticalGaussianMaterial* m =
	    dynamic_cast<const WardAnisotropicEllipticalGaussianMaterial*>( mat ) ) {
		return NewClone( new WardAnisotropicEllipticalGaussianMaterial(
			m->GetDiffuse(), m->GetSpecular(), m->GetAlphaX(), m->GetAlphaY() ) );
	}
	if( const TranslucentMaterial* m = dynamic_cast<const TranslucentMaterial*>( mat ) ) {
		return NewClone( new TranslucentMaterial(
			m->GetRefFront(), m->GetTrans(), m->GetExtinction(),
			m->GetN(), m->GetScat() ) );
	}

	// ---- Addref fallback ------------------------------------------------
	// Reached for: NullMaterial (no slots — sharing is trivially safe);
	// composed/read-only materials the editor never rebinds (emissive GGX);
	// any unknown out-of-tree type; AND the baked / wrapping materials
	// (SubSurfaceScattering, RandomWalkSSS, GenericHumanTissue, the
	// luminaires).
	//
	// HONEST RESIDUAL (deferred to increment B): the baked / wrapping
	// materials DO expose editor-mutable slots via
	// MaterialIntrospection::SetSlot (SSS/RandomWalkSSS `ior`, tissue
	// `sca`/`g`, luminaire `exitance`/`N`).  Because we addref-share them
	// here rather than clone, an in-place rebind of one of THOSE slots on
	// the live material WOULD still bleed into a snapshot.  They are not
	// faithfully ctor-reconstructable from their public surface (a sampled
	// diffusion profile / random-walk parameter snapshot / wrapped base
	// material is baked at construction and not read-back-able), so a
	// correct independent clone needs per-class clone methods that reach
	// private state — out of scope for increment A.  The 15 standard
	// reflectance/BRDF materials above (the common editable set, incl. the
	// test-pinned Lambertian) ARE cloned and carry no residual.
	mat->addref();
	return mat;
}

//////////////////////////////////////////////////////////////////////
// CloneLightForSnapshot - see header.
//////////////////////////////////////////////////////////////////////

const ILight* RISE::Implementation::CloneLightForSnapshot( const ILight* light )
{
	if( !light ) {
		return 0;
	}

	// Restore the transform-derived world position (point / spot lights
	// bake `ptPosition` from the transform matrix in FinalizeTransformations;
	// the factory builds at the origin).  Only the net world position
	// matters for rendering these delta lights, and SetPosition() +
	// FinalizeTransformations() reproduce it exactly: the clone starts with
	// an empty transform stack + identity orientation/scale, so the final
	// matrix is precisely Translation(position).  Directional / ambient
	// lights have no positional dependence, so this is a harmless no-op for
	// them.
	struct Restore {
		static const ILight* Place( ILightPriv* clone, const ILight& src ) {
			clone->SetPosition( src.position() );
			clone->FinalizeTransformations();
			return clone;
		}
	};

	if( const PointLight* m = dynamic_cast<const PointLight*>( light ) ) {
		ILightPriv* clone = 0;
		RISE_API_CreatePointOmniLight( &clone, m->emissionEnergy(), m->emissionColor(),
			m->CanGeneratePhotons() );
		if( clone ) {
			GlobalLog()->PrintNew( clone, __FILE__, __LINE__, "snapshot point light clone" );
			return Restore::Place( clone, *m );
		}
	}
	else if( const SpotLight* m = dynamic_cast<const SpotLight*>( light ) ) {
		ILightPriv* clone = 0;
		RISE_API_CreatePointSpotLight( &clone, m->emissionEnergy(), m->emissionColor(),
			m->emissionTarget(), m->emissionInnerAngle(), m->emissionOuterAngle(),
			m->CanGeneratePhotons() );
		if( clone ) {
			GlobalLog()->PrintNew( clone, __FILE__, __LINE__, "snapshot spot light clone" );
			// Spot derives both ptPosition and vDirection (from target +
			// position) in FinalizeTransformations, so the placement also
			// re-derives the cone axis correctly.
			return Restore::Place( clone, *m );
		}
	}
	else if( const DirectionalLight* m = dynamic_cast<const DirectionalLight*>( light ) ) {
		ILightPriv* clone = 0;
		RISE_API_CreateDirectionalLight( &clone, m->emissionEnergy(), m->emissionColor(),
			m->emissionDirection() );
		if( clone ) {
			GlobalLog()->PrintNew( clone, __FILE__, __LINE__, "snapshot directional light clone" );
			return clone;
		}
	}
	else if( const AmbientLight* m = dynamic_cast<const AmbientLight*>( light ) ) {
		ILightPriv* clone = 0;
		RISE_API_CreateAmbientLight( &clone, m->emissionEnergy(), m->emissionColor() );
		if( clone ) {
			GlobalLog()->PrintNew( clone, __FILE__, __LINE__, "snapshot ambient light clone" );
			return clone;
		}
	}

	// Unknown out-of-tree ILight type: no factory to rebuild it.  Addref so
	// the snapshot keeps it alive; an in-place edit of such a light would
	// bleed (documented residual, same policy as the baked materials).
	light->addref();
	return light;
}

//////////////////////////////////////////////////////////////////////
// CloneMediumForSnapshot - see header.
//////////////////////////////////////////////////////////////////////

const IMedium* RISE::Implementation::CloneMediumForSnapshot( const IMedium* medium )
{
	if( !medium ) {
		return 0;
	}

	// HomogeneousMedium is editor-mutable in place (SetAbsorption /
	// SetScattering / SetEmission) — reconstruct it from public coefficient
	// accessors.  The phase function is addref-shared by the factory (it is
	// not property-edited in place).
	if( const HomogeneousMedium* m = dynamic_cast<const HomogeneousMedium*>( medium ) ) {
		const IPhaseFunction* phase = m->GetPhaseFunction();
		if( phase ) {
			IMedium* clone = 0;
			RISE_API_CreateHomogeneousMediumWithEmission( &clone,
				m->GetAbsorption(), m->GetScattering(), m->GetEmission(), *phase );
			if( clone ) {
				GlobalLog()->PrintNew( clone, __FILE__, __LINE__, "snapshot homogeneous medium clone" );
				return clone;
			}
		}
	}

	// HeterogeneousMedium (baked dataset / bounds; editor refuses to edit
	// it) and any unknown type: addref-share.  Honest residual — if a
	// future increment makes a baked medium editor-mutable in place it would
	// need a real clone reaching its private state.
	medium->addref();
	return medium;
}

//////////////////////////////////////////////////////////////////////
// CloneCameraForSnapshot - see header.
//////////////////////////////////////////////////////////////////////

ICamera* RISE::Implementation::CloneCameraForSnapshot( const ICamera* camera )
{
	if( !camera ) {
		return 0;
	}

	// All built-in cameras derive from CameraCommon, which carries the pose
	// + exposure/scanning/pixel-rate + orientation state common to every
	// factory call.  ONB-constructed cameras cannot be rebuilt through the
	// non-ONB factories without degrading their basis — refuse (return 0)
	// per the CameraCommon.h warning so the caller falls back to the pose
	// summary rather than silently shipping a wrong camera.
	const CameraCommon* cc = dynamic_cast<const CameraCommon*>( camera );
	if( !cc ) {
		return 0;   // unknown out-of-tree camera: no factory to rebuild it
	}
	if( cc->IsFromONB() ) {
		return 0;   // honest refusal — see header
	}

	const Point3   loc    = cc->GetRestLocation();
	const Point3   lookAt = cc->GetStoredLookAt();
	const Vector3  up     = cc->GetStoredUp();
	const unsigned w      = cc->GetWidth();
	const unsigned h      = cc->GetHeight();
	const Scalar   pAR    = cc->GetPixelAR();
	const Scalar   expo   = cc->GetExposureTimeStored();
	const Scalar   scan   = cc->GetScanningRateStored();
	const Scalar   prate  = cc->GetPixelRateStored();
	const Vector3  orient = cc->GetEulerOrientation();
	const Vector2  torient= cc->GetTargetOrientation();

	if( const ThinLensCamera* m = dynamic_cast<const ThinLensCamera*>( camera ) ) {
		ICamera* clone = 0;
		RISE_API_CreateThinlensCamera( &clone, loc, lookAt, up,
			m->GetSensorSize(), m->GetFocalLengthStored(), m->GetFstop(),
			m->GetFocusDistanceStored(), m->GetSceneUnitMeters(),
			w, h, pAR, expo, scan, prate, orient, torient,
			m->GetApertureBlades(), m->GetApertureRotation(), m->GetAnamorphicSqueeze(),
			m->GetTiltX(), m->GetTiltY(), m->GetShiftX(), m->GetShiftY(),
			m->GetIsoStored() );
		if( clone ) {
			GlobalLog()->PrintNew( clone, __FILE__, __LINE__, "snapshot thinlens camera clone" );
			return clone;
		}
	}
	else if( const OrthographicCamera* m = dynamic_cast<const OrthographicCamera*>( camera ) ) {
		ICamera* clone = 0;
		RISE_API_CreateOrthographicCamera( &clone, loc, lookAt, up,
			w, h, m->GetViewportScaleStored(), pAR, expo, scan, prate, orient, torient );
		if( clone ) {
			GlobalLog()->PrintNew( clone, __FILE__, __LINE__, "snapshot orthographic camera clone" );
			return clone;
		}
	}
	else if( const FisheyeCamera* m = dynamic_cast<const FisheyeCamera*>( camera ) ) {
		ICamera* clone = 0;
		RISE_API_CreateFisheyeCamera( &clone, loc, lookAt, up,
			w, h, pAR, expo, scan, prate, orient, torient, m->GetScaleStored() );
		if( clone ) {
			GlobalLog()->PrintNew( clone, __FILE__, __LINE__, "snapshot fisheye camera clone" );
			return clone;
		}
	}
	else if( const PinholeCamera* m = dynamic_cast<const PinholeCamera*>( camera ) ) {
		// Pinhole last: ThinLens does NOT derive from PinholeCamera, but
		// keep pinhole after the more-specific perspective type for clarity.
		ICamera* clone = 0;
		RISE_API_CreatePinholeCamera( &clone, loc, lookAt, up,
			m->GetFovStored(), w, h, pAR, expo, scan, prate, orient, torient,
			m->GetIsoStored(), m->GetFstop() );
		if( clone ) {
			GlobalLog()->PrintNew( clone, __FILE__, __LINE__, "snapshot pinhole camera clone" );
			return clone;
		}
	}

	// Known CameraCommon subtype we don't have a factory branch for: refuse
	// rather than ship a degraded camera.  Caller falls back to pose.
	return 0;
}


// CloneRadianceMapForSnapshot - see header (F8).
const IRadianceMap* RISE::Implementation::CloneRadianceMapForSnapshot( const IRadianceMap* src )
{
	if( !src ) return 0;
	// Painter-backed RadianceMap: clone with its own dScale + transform so a
	// live SetScale (Job::SetActiveRasterizerRadianceScale) cannot bleed in.
	if( const RadianceMap* rm = dynamic_cast<const RadianceMap*>( src ) ) {
		IRadianceMap* clone = 0;
		RISE_API_CreateRadianceMap( &clone, rm->GetPainter(), rm->GetScale() );
		if( clone ) clone->SetTransformation( rm->GetTransform() );
		return clone;
	}
	// Procedural / unknown (e.g. HosekWilkie): ADDREF-fallback (documented
	// residual -- a live scale edit on such a map would still bleed).
	src->addref();
	return src;
}
