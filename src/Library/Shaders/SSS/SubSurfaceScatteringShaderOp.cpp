//////////////////////////////////////////////////////////////////////
//
//  SubSurfaceScatteringShaderOp.cpp - Implementation of the
//  point-sampled BSSRDF shader op.
//
//  See SubSurfaceScatteringShaderOp.h for algorithm overview.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 18, 2005
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SubSurfaceScatteringShaderOp.h"
#include "../../Utilities/GeometricUtilities.h"
#include "../../Utilities/BSSRDFSampling.h"		// ExteriorIOR (DL-291)
#include "../../Interfaces/IGeometry.h"		// CanBeAreaLight(): SSS needs real surface sampling
#include "../../Utilities/stl_utils.h"
#include "../../Sampling/HaltonPoints.h"
#include "../../Utilities/Color/RGBSpectra.h"	// RGBUnboundedSpectrum (RGB->spectral uplift for PerformOperationNM)
#include <memory>
#include <mutex>									// std::lock_guard (exception-safe create_mutex)

using namespace RISE;
using namespace RISE::Implementation;

SubSurfaceScatteringShaderOp::SubSurfaceScatteringShaderOp( 
	const unsigned int numPoints_,
	const Scalar error_,
	const unsigned int maxPointsPerNode_,
	const unsigned char maxDepth_,
	const Scalar irrad_scale_,
	const bool multiplyBSDF_,
	const bool regenerate_,
	const IShader& shader_,
	const ISubSurfaceExtinctionFunction& extinction_,
	const bool cache_,
	const bool low_discrepancy_
	) : 
  numPoints( numPoints_ ),
  error( error_ ),
  maxPointsPerNode( maxPointsPerNode_ ),
  maxDepth( maxDepth_ ),
  irrad_scale( irrad_scale_ ),
  multiplyBSDF( multiplyBSDF_ ),
  regenerate( regenerate_ ),
  shader( shader_ ),
  extinction( extinction_ ),
  cache( cache_ ),
  low_discrepancy( low_discrepancy_ )
{
	extinction.addref();
	shader.addref();
}

SubSurfaceScatteringShaderOp::~SubSurfaceScatteringShaderOp( )
{
	PointSetMap::iterator i, e;
	for( i=pointsets.begin(), e=pointsets.end(); i!=e; i++ ) {
		delete i->second;
	}

	pointsets.clear();

	extinction.release();
	shader.release();
}

//! Tells the shader to apply shade to the given intersection point
void SubSurfaceScatteringShaderOp::PerformOperation(
	const RuntimeContext& rc,					///< [in] Runtime context
	const RayIntersection& ri,					///< [in] Intersection information 
	const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
	const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
	RISEPel& c,									///< [in/out] Resultant color from op
	const IORStack& ior_stack,			///< [in] Index of refraction stack
	const ScatteredRayContainer* pScat			///< [in] Scattering information
	) const
{
	c = RISEPel(0.0);
    SMSReferenceRadianceScope returnRadiance(rc);
    const auto cacheKey=std::make_pair(ri.pObject,rc.smsForceLegacy);

	const IScene* pScene = caster.GetAttachedScene();

	// If these three things don't exist, then we can't do anything for this object
	if( !pScene ) {
		return;
	}

	// Only do stuff on a normal pass or on final gather
	if( !rc.IsNormalShadingPass() && rs.type == rs.eRayView ) {
		return;
	}

	// Fast-preview fallback for the interactive viewport.
	//
	// The full SSS path below builds a per-object irradiance point
	// set on first hit (numPoints surface samples × full Shade per
	// sample, each shade casts shadow rays to all lights) and then
	// evaluates a hierarchical octree per pixel.  At numPoints =
	// 200K (typical for production-quality SSS) the build alone is
	// many seconds in recursive capture, blocking the
	// rasterizer's cancel-restart loop entirely — the user sees a
	// frozen viewport with no preview rendering.
	//
	// In interactive preview, we delegate to the embedded
	// irradiance-capture shader instead.  That shader is the one
	// configured for capturing irradiance at point samples
	// (typically a directlighting_shaderop on a Lambertian BSDF);
	// running it on the camera-ray hit gives a fast direct-lit
	// fallback — visible objects, correct positions, useful for
	// scene navigation.  The production rasterizer leaves
	// bFastPreview false and gets the full SSS contribution.
	//
	// Bypassing the StateCache deliberately: a fast-preview value
	// is NOT a valid cache entry for the production render, and a
	// cached production value isn't valid for fast preview either.
	// Both paths re-compute on demand; the production path is
	// already heavy enough that the cache hit pays off.
	if( rc.bFastPreview ) {
		shader.Shade( rc, ri, caster, rs, c, ior_stack );
		return;
	}

	// Lets check our rasterizer state to see if we even need to do work!
	if( cache ) {
		if( !rc.StateCache_HasStateChanged( this, c, ri.pObject, ri.geometric.rast, &rc.smsReferenceRadiance, unsigned(rc.smsForceLegacy) ) ) {
			// State hasn't changed, use the value already there
			return;
		}
	}

    // Map lookup and publication are serialized. Irradiance capture runs
    // outside the mutex because native continuation can recursively shade
    // another object sharing this operation. Published octrees are immutable
    // until ResetRuntimeData, which belongs to the stopped-render lifecycle.
	PointSetOctree* ps = nullptr;
    bool needsBuild = false;
    {
        std::lock_guard<RMutex> guard(create_mutex);
        const auto it = pointsets.find(cacheKey);
        needsBuild = it == pointsets.end();
        if(!needsBuild) ps = it->second;
    }
    if(needsBuild) {
			// SSS point-set generation uniformly samples the object's SURFACE via
			// UniformRandomPoint/GetArea.  A geometry that cannot honour that contract
			// (CanBeAreaLight() false -- e.g. a degenerate zero-area field) would
			// collapse the samples -> a bogus irradiance cache.  Refuse SSS on such
			// geometry, with a diagnostic, rather than build a garbage sample set.
			//
			// NULL GEOMETRY (crash-sibling fix, see LuminaryManager::AddToLuminaryList):
			// ri.pObject->GetGeometry() can legitimately be null (a CSGObject has no
			// single owned geometry).  The condition used to be `pSSSGeom &&
			// !pSSSGeom->CanBeAreaLight()`, which short-circuits to false -- i.e.
			// "acceptable" -- for exactly the null case, letting a null-geometry
			// object fall through to `ri.pObject->UniformRandomPoint(...)` below,
			// which null-derefs (Object::UniformRandomPoint -> pGeometry->...).
			const IGeometry* pSSSGeom = ri.pObject ? ri.pObject->GetGeometry() : 0;
			if( !pSSSGeom || !pSSSGeom->CanBeAreaLight() ) {
				{
                    std::lock_guard<RMutex> guard(create_mutex);
                    if(pointsets.emplace(cacheKey,nullptr).second) GlobalLog()->PrintEasyWarning( "SubSurfaceScatteringShaderOp:: object geometry cannot be uniformly surface-sampled (CanBeAreaLight() == false, or no directly-owned geometry, e.g. a csg_object); subsurface scattering is unsupported on it -- skipping (no SSS contribution)." );
                }
				c = RISEPel( 0.0 );

				return;
			}
			// Pass 1: Generate the irradiance point set for this object.
			// Concurrent local builders can run; only the first completed tree is retained.
			GlobalLog()->PrintEasyInfo( "SubSurfaceScatteringShaderOp:: Generating point sample set for object" );
		
			PointSetOctree::PointSet points;
			BoundingBox bbox( Point3(RISE_INFINITY,RISE_INFINITY,RISE_INFINITY), Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY) );

			// Since we are uniformly sampling, we just divide the overall surface area by the number of sample points
	//		const Scalar sample_area = ri.pObject->GetArea() / Scalar(numPoints);

			// Use a halton point sequence to make sure the sampling points are distributed in a good way
			MultiHalton mh;

			// REPRODUCIBILITY (1 of 2): a dedicated fixed-seed RNG for the irradiance
			// capture, independent of the render thread's scheduling-dependent
			// rc.random.  Concurrent local builds each use the same fixed capture
			// sampling. Previously consuming the triggering thread's rc.random STATE made
			// the captured irradiance vary run-to-run.  buildRc leaves pSampler null so
			// the irradiance shade falls back to buildRng, not the (also
			// scheduling-dependent) QMC sampler; the capture keeps fresh owning caches and copies the
            // anchor legacy-mode bit below; its sampling uses {random, pSampler, pass}.
			// (The OTHER half of the fix is the sample-point geometric frame set on
			// newri below -- without it the build still craters: for a delta light the
			// RNG here does not even affect the value, but the frame does.)
			RandomNumberGenerator buildRng( 0x9E3779B9u );	// fixed seed (golden ratio)
			RuntimeContext buildRc( buildRng, rc.pass, rc.bThreaded );
            buildRc.smsForceLegacy=rc.smsForceLegacy;

			for( unsigned int i=0; i<numPoints; i++ ) {
				// Ask the object for a uniform random point
				PointSetOctree::SamplePoint sp;
				Vector3 normal;
				Point2 sampleCoord;

				Point3 random_variables;
				if( low_discrepancy ) {
					random_variables = Point3( mh.mod1(mh.halton(0,i)), mh.mod1(mh.halton(1,i)), mh.mod1(mh.halton(2,i)) );
				} else {
					random_variables = Point3( buildRng.CanonicalRandom(), buildRng.CanonicalRandom(), buildRng.CanonicalRandom() );
				}

				ri.pObject->UniformRandomPoint( &sp.ptPosition, &normal, &sampleCoord, random_variables );
				// We may want in the future to move the point in (away from the surface) if we decide to add the option of occluders
	//				sp.ptPosition = Point3Ops::mkPoint3( sp.ptPosition, normal*NEARZERO );		// move the sample points slightly away from the surface

				// Now compute the irradiance for this point using the BDF
				RayIntersection newri( ri );
				newri.geometric.ray = Ray( sp.ptPosition, -normal );
				newri.geometric.bHit = true;
				newri.geometric.ptIntersection = sp.ptPosition;
				newri.geometric.vNormal = normal;
				// REPRODUCIBILITY (2 of 2) + correctness: `RayIntersection newri( ri )`
				// above copied the TRIGGERING pixel's full geometric.  A uniform surface
				// sample carries only position + normal + uv, so represent THAT and clear
				// every trigger-pixel field the irradiance shade can read -- otherwise the
				// capture is both WRONG (shaded with the trigger's frame/coords) and
				// NON-DETERMINISTIC (the trigger is whichever thread/sample first hits the
				// object).  Fields the shade reads:
				//   onb           - BSDF reflect-side test (LambertianBRDF onb.w()) + aniso frame
				//   ptCoord       - 2D-textured reflectance
				//   ptObjIntersec - 3D-solid-textured reflectance: the sample's OBJECT-space
				//                   point.  UniformRandomPoint returns the WORLD point, so map
				//                   it back through the object inverse transform (as
				//                   DirectVolumeRenderingShader does) -- exact even for a
				//                   transformed object, matching a normal camera-ray hit.
				//   rast          - optimal-MIS tile -> null tile (DETERMINISTIC under optimal-
				//                   MIS; the build's auxiliary samples then feed accumulator
				//                   tile (0,0) while it trains -- pre-existing and off-by-
				//                   default: the build is not a camera path, and fed a
				//                   scheduling-dependent tile before)
				//   txFootprint   - mip LOD (cleared: a build sample has no ray differentials)
				//   bHas{TexCoord1,VertexColor,Tangent} - per-vertex MESH attributes a random
				//                   surface point does not carry; cleared so painters use
				//                   their no-data defaults
				// The surface cosine uses vNormal (set above); vGeomNormal is set for frame
				// consistency (not read on the Lambertian path).  This frame leak is the
				// dominant non-determinism source; with buildRng the build is fully
				// reproducible.
				newri.geometric.vGeomNormal = normal;
				newri.geometric.onb.CreateFromW( normal );
				newri.geometric.ptCoord = sampleCoord;
				newri.geometric.ptObjIntersec = Point3Ops::Transform( ri.pObject->GetFinalInverseTransformMatrix(), sp.ptPosition );
				newri.geometric.rast = nullRasterizerState;
				newri.geometric.txFootprint = TextureFootprint();
				newri.geometric.bHasTexCoord1 = false;
				newri.geometric.bHasVertexColor = false;
				newri.geometric.bHasTangent = false;

				// Advance the ray for the purpose of shading, this should help reduce errors
				newri.geometric.ray.Advance( 1e-8 );

				{
                    SMSReferenceRadianceScope sampleRadiance(buildRc);
                    shader.Shade( buildRc, newri, caster, rs, sp.irrad, ior_stack );
                    sp.smsReferenceRadiance=sampleRadiance.HasReferenceRadiance();
                }

				// Keep finite nonzero reference samples, including signed transport.
                // Ordinary-only samples retain the native positive-illumination filter.
				if( ColorMath::MaxValue(sp.irrad) > 0 ||
                    (sp.smsReferenceRadiance && std::isfinite(sp.irrad[0]) && std::isfinite(sp.irrad[1]) && std::isfinite(sp.irrad[2]) &&
                     (sp.irrad[0]!=0 || sp.irrad[1]!=0 || sp.irrad[2]!=0)) ) {
					sp.irrad = sp.irrad * irrad_scale;
					points.push_back( sp );
					bbox.Include( sp.ptPosition );
				}
			}

	//		bbox.Grow( NEARZERO );		// Grow for error tolerance
			bbox.EnsureBoxHasVolume();
			std::unique_ptr<PointSetOctree> built(new PointSetOctree(bbox,maxPointsPerNode));

			if( points.size() < 1 ) {
				GlobalLog()->PrintEasyError( "SubSurfaceScatteringShaderOp:: Not a single sample point could be generated" );
			}

			if( !built->AddElements( points, maxDepth ) ) {
				GlobalLog()->PrintEasyError( "SubSurfaceScatteringShaderOp:: Fatal error while creating irradiance sample set" );
			}
        // Capture can recursively reach this operation or another one. Never
        // hold a cache mutex while calling a shader. Concurrent/reentrant
        // builders keep local ownership; the first completed immutable tree
        // is published, and every later builder safely discards its copy.
        std::lock_guard<RMutex> guard(create_mutex);
        const auto published = pointsets.emplace(cacheKey,built.get());
        if(published.second) built.release();
        ps = published.first->second;
    }

	// Unsupported geometry was cached as a null sentinel above -> no SSS contribution
	// (c is already 0 from the top of the function).
	if( !ps ) return;

	// Pass 2: Evaluate the BSSRDF integral at the shading point.
	// The octree sums Rd(|xi - xo|) * E(xi) over all sample points,
	// using hierarchical approximation for distant clusters (Jensen 2002).
	//
	// DL-291: the diffusion boundary condition (the dipole's A) is a function
	// of the material's index RELATIVE to the medium the body sits in, so the
	// extinction function is evaluated against the live exterior -- the IOR-
	// stack top the ray caster stamped on this hit (`ambientIOR`; 1.0 = air,
	// which reproduces the pre-DL-291 value exactly).
	ps->Evaluate( c, ri.geometric.ptIntersection, extinction, error, multiplyBSDF?ri.pMaterial->GetBSDF():0, ri.geometric, &ior_stack,
		BSSRDFSampling::ExteriorIOR( ri.geometric ), &rc.smsReferenceRadiance );

	// Monte Carlo normalization: divide by N (the number of sample points).
	// Each sample's irradiance was pre-multiplied by irrad_scale, which
	// absorbs the area weight (dA = total_area / N) and the unit conversion
	// between the extinction function's internal units and scene-space.
	c = c * (1.0/Scalar(numPoints));

	// When multiplyBSDF is enabled, the octree evaluation already divided
	// by pi (via the Lambertian BSDF).  Multiply by pi to cancel that and
	// recover the correct BSSRDF integral.
	if( ri.pMaterial->GetBSDF() && multiplyBSDF ) {
		c = c*PI;
	}

    if(c[0]==0 && c[1]==0 && c[2]==0) rc.smsReferenceRadiance=false;

	if( cache ) {
		// Add the result to the rasterizer state cache
		rc.StateCache_SetState( this, c, ri.pObject, ri.geometric.rast, rc.smsReferenceRadiance, unsigned(rc.smsForceLegacy) );
	}
}

//! Tells the shader to apply shade to the given intersection point for the given wavelength
/// \return Amplitude of spectral function 
Scalar SubSurfaceScatteringShaderOp::PerformOperationNM(
	const RuntimeContext& rc,					///< [in] Runtime context
	const RayIntersection& ri,					///< [in] Intersection information 
	const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
	const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
	const Scalar caccum,						///< [in] Current value for wavelength
	const Scalar nm,							///< [in] Wavelength to shade
	const IORStack& ior_stack,			///< [in] Index of refraction stack
	const ScatteredRayContainer* pScat			///< [in] Scattering information
	) const
{
	// Spectral path: evaluate the full RGB BSSRDF (this reuses the
	// per-object irradiance octree -- built once and shared across all
	// wavelengths) and uplift the resulting RGB exitant radiance to
	// wavelength `nm` with the same chroma-preserving JH uplift the RGB
	// painters use for GetColorNM.  The prior `return 0` stub rendered
	// every SSS object BLACK under the *_spectral_* rasterizers
	// (pixelintegratingspectral / pathtracing_spectral / bdpt_spectral /
	// vcm_spectral / mlt_spectral).
	//
	// Result-uplift rather than a true per-wavelength transport because
	// both the diffusion profile (ISubSurfaceExtinctionFunction, RGB
	// ComputeTotalExtinction) and the cached irradiance are RGB-valued;
	// a per-lambda port would need a spectral extinction interface plus a
	// per-wavelength irradiance cache.  Uplifting the result makes the
	// spectral render reconstruct the RGB SSS appearance (the uplift
	// round-trips through the CMFs).  The diffusion *radius* is therefore
	// at RGB resolution, not per-lambda -- documented approximation; a
	// true spectral BSSRDF is future work.
	//
	// RGBIlluminantSpectrum, NOT Unbounded (Stage C slice 2): `c` is the
	// exitant RADIANCE, a source term at this boundary, so it must carry
	// the reference illuminant's shape to round-trip through the film back
	// to the RGB result.  Unbounded is reflectance-shaped and tinted the
	// spectral SSS render by (1.20, 0.95, 0.91).
	//
	// EnsurePositve before the uplift, for the same reason
	// FinalGatherShaderOp::PerformOperationNM clamps there: FromRGB takes
	// the MAX CHANNEL as its scale, so a single negative component (a
	// stray subtraction in the diffusion profile, an octree lerp
	// undershoot) can flip the scale's sign and corrupt EVERY wavelength
	// -- ordinary-only returns therefore retain their native projection.
	SMSReferenceRadianceScope returnRadiance(rc);
	RISEPel c;
	PerformOperation( rc, ri, caster, rs, c, ior_stack, pScat );
    Scalar result;
    if(returnRadiance.HasReferenceRadiance() && (c[0]<0 || c[1]<0 || c[2]<0)) {
        // Extend the native positive-radiance uplift to signed reference
        // returns by subtraction. Do not project away a negative component.
        RISEPel positive(0.0),negative(0.0);
        for(unsigned i=0;i<3;++i) {positive[i]=std::max(Scalar(0),c[i]);negative[i]=std::max(Scalar(0),-c[i]);}
        result=RGBIlluminantSpectrum::FromRGB(positive).Eval(nm)-RGBIlluminantSpectrum::FromRGB(negative).Eval(nm);
    } else {
        ColorMath::EnsurePositve(c);
        result=RGBIlluminantSpectrum::FromRGB(c).Eval(nm);
    }
    if(result==0) rc.smsReferenceRadiance=false;
    return result;
}

void SubSurfaceScatteringShaderOp::ResetRuntimeData() const
{
	if( regenerate ) {
		PointSetMap::iterator i, e;
		for( i=pointsets.begin(), e=pointsets.end(); i!=e; i++ ) {
			delete i->second;
		}

		pointsets.clear();
	}
}
