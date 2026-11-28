//////////////////////////////////////////////////////////////////////
//
//  SnapshotLeafClone.cpp - see SnapshotLeafClone.h.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SnapshotLeafClone.h"
#include <typeinfo>

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
#include "../Materials/RoughDielectricMaterial.h"
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
#include "../Materials/MixMaterial.h"
#include "../Materials/DiffuseTransmissionMaterial.h"

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
	T* NewClone( const IMaterial* source, T* p )
	{
		GlobalLog()->PrintNew( p, __FILE__, __LINE__, "snapshot material clone" );
        // Scalar subleaves follow the existing addref-sharing policy. The
        // independently reconstructed material owns its own slot binding.
        static_cast<IMaterial*>(p)->SetAlpha(source->GetAlphaPainter(), source->GetAlphaMode(), source->GetAlphaCutoff());
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
		return NewClone( mat, new LambertianMaterial( m->GetReflectance() ) );
	}
	if( const PerfectReflectorMaterial* m = dynamic_cast<const PerfectReflectorMaterial*>( mat ) ) {
		return NewClone( mat, new PerfectReflectorMaterial( m->GetReflectance() ) );
	}
	if( const PerfectRefractorMaterial* m = dynamic_cast<const PerfectRefractorMaterial*>( mat ) ) {
		return NewClone( mat, new PerfectRefractorMaterial( m->GetRefractivity(), m->GetIOR() ) );
	}
	if( const PolishedMaterial* m = dynamic_cast<const PolishedMaterial*>( mat ) ) {
		return NewClone( mat, new PolishedMaterial(
			m->GetDiffuseReflectance(), m->GetTransmittance(),
			m->GetIOR(), m->GetScattering(), m->GetHG() ) );
	}
	if( const RoughDielectricMaterial* m = dynamic_cast<const RoughDielectricMaterial*>(mat) ) {
		return NewClone(mat,new RoughDielectricMaterial(m->GetIOR(),m->GetAlphaX(),m->GetAlphaY()));
	}
	if( const DielectricMaterial* m = dynamic_cast<const DielectricMaterial*>( mat ) ) {
		return NewClone( mat, new DielectricMaterial(
			m->GetTransmittance(), m->GetIOR(), m->GetScattering(), m->GetHG(),
			m->GetARLayerN(), m->GetARLayerK(), m->GetARLayerT(), m->GetARLayerCount() ) );
	}
	if( const IsotropicPhongMaterial* m = dynamic_cast<const IsotropicPhongMaterial*>( mat ) ) {
		return NewClone( mat, new IsotropicPhongMaterial( m->GetRd(), m->GetRs(), m->GetExponent() ) );
	}
	if( const OrenNayarMaterial* m = dynamic_cast<const OrenNayarMaterial*>( mat ) ) {
		return NewClone( mat, new OrenNayarMaterial( m->GetReflectance(), m->GetRoughness(), m->IsEnergyPreserving() ) );
	}
	if( const SchlickMaterial* m = dynamic_cast<const SchlickMaterial*>( mat ) ) {
		return NewClone( mat, new SchlickMaterial(
			m->GetDiffuse(), m->GetSpecular(), m->GetRoughness(), m->GetIsotropy() ) );
	}
	if( const SheenMaterial* m = dynamic_cast<const SheenMaterial*>( mat ) ) {
		return NewClone( mat, new SheenMaterial( m->GetColor(), m->GetRoughness() ) );
	}
	if( const AshikminShirleyAnisotropicPhongMaterial* m =
	    dynamic_cast<const AshikminShirleyAnisotropicPhongMaterial*>( mat ) ) {
		return NewClone( mat, new AshikminShirleyAnisotropicPhongMaterial(
			m->GetNu(), m->GetNv(), m->GetRd(), m->GetRs() ) );
	}
	if( const CookTorranceMaterial* m = dynamic_cast<const CookTorranceMaterial*>( mat ) ) {
		return NewClone( mat, new CookTorranceMaterial(
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
			return NewClone( mat, new GGXMaterial(
				m->GetDiffuse(), m->GetSpecular(), m->GetAlphaX(), m->GetAlphaY(),
				m->GetIOR(), m->GetExtinction(), m->GetFresnelMode(),
				m->GetTangentRotation(),
				m->GetFilmIOR(), m->GetFilmExtinction(), m->GetFilmThickness(),
				m->GetTangentRotationScalar() ) );
		}
	}
	if( const WardIsotropicGaussianMaterial* m =
	    dynamic_cast<const WardIsotropicGaussianMaterial*>( mat ) ) {
		return NewClone( mat, new WardIsotropicGaussianMaterial(
			m->GetDiffuse(), m->GetSpecular(), m->GetAlpha() ) );
	}
	if( const WardAnisotropicEllipticalGaussianMaterial* m =
	    dynamic_cast<const WardAnisotropicEllipticalGaussianMaterial*>( mat ) ) {
		return NewClone( mat, new WardAnisotropicEllipticalGaussianMaterial(
			m->GetDiffuse(), m->GetSpecular(), m->GetAlphaX(), m->GetAlphaY() ) );
	}
	if( const TranslucentMaterial* m = dynamic_cast<const TranslucentMaterial*>( mat ) ) {
		return NewClone( mat, new TranslucentMaterial(
			m->GetRefFront(), m->GetTrans(), m->GetExtinction(),
			m->GetN(), m->GetScat() ) );
	}
	if( const DiffuseTransmissionMaterial* m = dynamic_cast<const DiffuseTransmissionMaterial*>( mat ) ) {
		return NewClone( mat, new DiffuseTransmissionMaterial(
			m->GetReflectance(), m->GetTransmittance() ) );
	}
	if( const MixMaterial* m = dynamic_cast<const MixMaterial*>( mat ) ) {
		const unsigned int n = m->GetChildCount();
		std::vector<const IMaterial*> clonedKids( n, 0 );
		std::vector<const IScalarPainter*> weights( n, 0 );
		std::vector<const IRayIntersectionModifier*> mods( n, 0 );
		for( unsigned int i = 0; i < n; ++i ) {
			const MixMaterialChild& ch = m->GetChild( i );
			clonedKids[i] = ch.pMaterial ? CloneMaterialForSnapshot( ch.pMaterial ) : 0;
			weights[i] = ch.pWeight;
			mods[i] = ch.pModifier;
		}
		const IMaterial* clonedMix = NewClone( mat, new MixMaterial(
			clonedKids.data(), weights.data(), mods.data(), n, m->GetPassthroughWeight() ) );
		for( unsigned int i = 0; i < n; ++i ) {
			if( clonedKids[i] ) clonedKids[i]->release();
		}
		return clonedMix;
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

    // Reconstruct exact built-ins only. A derived light may override RGB/NM
    // emission or other virtual behavior not represented by these accessors;
    // rebuilding its base would slice it instead of preserving the source.
	if( typeid(*light) == typeid(PointLight) ) {
        const auto* m = dynamic_cast<const PointLight*>(light);
		ILightPriv* clone = 0;
		clone = new PointLight( m->emissionEnergy(), m->emissionColor(),
			m->CanGeneratePhotons(), m->GetPhysicalSpectrum() );
		if( clone ) {
			GlobalLog()->PrintNew( clone, __FILE__, __LINE__, "snapshot point light clone" );
			return Restore::Place( clone, *m );
		}
	}
	else if( typeid(*light) == typeid(SpotLight) ) {
        const auto* m = dynamic_cast<const SpotLight*>(light);
		ILightPriv* clone = 0;
		clone = new SpotLight( m->emissionEnergy(), m->emissionTarget(),
			m->emissionInnerAngle(), m->emissionOuterAngle(), m->emissionColor(),
			m->CanGeneratePhotons(), m->GetPhysicalSpectrum() );
		if( clone ) {
			GlobalLog()->PrintNew( clone, __FILE__, __LINE__, "snapshot spot light clone" );
			// Spot derives both ptPosition and vDirection (from target +
			// position) in FinalizeTransformations, so the placement also
			// re-derives the cone axis correctly.
			return Restore::Place( clone, *m );
		}
	}
	else if( typeid(*light) == typeid(DirectionalLight) ) {
        const auto* m = dynamic_cast<const DirectionalLight*>(light);
		ILightPriv* clone = 0;
		clone = new DirectionalLight( m->emissionEnergy(), m->emissionColor(),
			m->emissionDirection(), m->GetPhysicalSpectrum() );
		if( clone ) {
			GlobalLog()->PrintNew( clone, __FILE__, __LINE__, "snapshot directional light clone" );
			return clone;
		}
	}
	else if( typeid(*light) == typeid(AmbientLight) ) {
        const auto* m = dynamic_cast<const AmbientLight*>(light);
		ILightPriv* clone = 0;
		RISE_API_CreateAmbientLight( &clone, m->emissionEnergy(), m->emissionColor() );
		if( clone ) {
			GlobalLog()->PrintNew( clone, __FILE__, __LINE__, "snapshot ambient light clone" );
			return clone;
		}
	}

	// Unknown out-of-tree ILight type (including built-in subclasses): no
    // faithful factory to rebuild its virtual behavior. Addref so
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
	// not property-edited in place), and so are the optional sigma(lambda)
	// spectral curves — a spectral medium cloned through the RGB-only
	// factory would silently lose its curves and fall back to the RGB
	// preview coefficients in NM renders.
	if( const HomogeneousMedium* m = dynamic_cast<const HomogeneousMedium*>( medium ) ) {
		const IPhaseFunction* phase = m->GetPhaseFunction();
		if( phase ) {
			IMedium* clone = 0;
			if( m->GetAbsorptionSpectral() || m->GetScatteringSpectral() ) {
				RISE_API_CreateHomogeneousMediumSpectral( &clone,
					m->GetAbsorption(), m->GetScattering(), m->GetEmission(),
					m->GetAbsorptionSpectral(), m->GetScatteringSpectral(), *phase );
			} else {
				RISE_API_CreateHomogeneousMediumWithEmission( &clone,
					m->GetAbsorption(), m->GetScattering(), m->GetEmission(), *phase );
			}
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

	ICamera* clone = 0;
	const char* logTag = 0;
	if( const ThinLensCamera* m = dynamic_cast<const ThinLensCamera*>( camera ) ) {
		RISE_API_CreateThinlensCamera( &clone, loc, lookAt, up,
			m->GetSensorSize(), m->GetFocalLengthStored(), m->GetFstop(),
			m->GetFocusDistanceStored(), m->GetSceneUnitMeters(),
			w, h, pAR, expo, scan, prate, orient, torient,
			m->GetApertureBlades(), m->GetApertureRotation(), m->GetAnamorphicSqueeze(),
			m->GetTiltX(), m->GetTiltY(), m->GetShiftX(), m->GetShiftY(),
			m->GetIsoStored() );
		logTag = "snapshot thinlens camera clone";
	}
	else if( const OrthographicCamera* m = dynamic_cast<const OrthographicCamera*>( camera ) ) {
		RISE_API_CreateOrthographicCamera( &clone, loc, lookAt, up,
			w, h, m->GetViewportScaleStored(), pAR, expo, scan, prate, orient, torient );
		logTag = "snapshot orthographic camera clone";
	}
	else if( const FisheyeCamera* m = dynamic_cast<const FisheyeCamera*>( camera ) ) {
		RISE_API_CreateFisheyeCamera( &clone, loc, lookAt, up,
			w, h, pAR, expo, scan, prate, orient, torient, m->GetScaleStored() );
		logTag = "snapshot fisheye camera clone";
	}
	else if( const PinholeCamera* m = dynamic_cast<const PinholeCamera*>( camera ) ) {
		// Pinhole last: ThinLens does NOT derive from PinholeCamera, but
		// keep pinhole after the more-specific perspective type for clarity.
		RISE_API_CreatePinholeCamera( &clone, loc, lookAt, up,
			m->GetFovStored(), w, h, pAR, expo, scan, prate, orient, torient,
			m->GetIsoStored(), m->GetFstop() );
		logTag = "snapshot pinhole camera clone";
	}

	if( clone ) {
		if( CameraCommon* cloneCC = dynamic_cast<CameraCommon*>( clone ) ) {
			cloneCC->SetNearClip( cc->GetNearClip() );
		}
		GlobalLog()->PrintNew( clone, __FILE__, __LINE__, logTag );
		return clone;
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
