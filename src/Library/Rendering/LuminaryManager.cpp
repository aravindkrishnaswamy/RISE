//////////////////////////////////////////////////////////////////////
//
//  LuminaryManager.cpp - Implements the luminary manager
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: July 30, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "LuminaryManager.h"
#include "../Interfaces/IGeometry.h"		// CanBeAreaLight() capability check below
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/ProbabilityDensityFunction.h"

using namespace RISE;
using namespace RISE::Implementation;

LuminaryManager::LuminaryManager() :
  pLumSampling( 0 )
{
}

LuminaryManager::~LuminaryManager()
{
	safe_release( pLumSampling );

	LuminariesList::const_iterator	i, e;
	for( i=luminaries.begin(), e=luminaries.end(); i!=e; i++ ) {
		i->pLum->release();
	}

	luminaries.clear();
}

namespace RISE {
	namespace Implementation {
		class ObjectEnumerationDispatch : public IEnumCallback<IObject>
		{
		protected:
			ILuminaryManager&	pBackObj;

		public:
			ObjectEnumerationDispatch( ILuminaryManager& pBackObj_ ) : 
			pBackObj( pBackObj_ )
			{
				pBackObj.addref();
			}

			virtual ~ObjectEnumerationDispatch()
			{
				pBackObj.release();
			}

			bool operator()( const IObject& p )
			{
				pBackObj.AddToLuminaryList( p );
				return true;
			}
		};
	}
}

void LuminaryManager::AttachScene( const IScene* pScene )
{
	if( pScene ) {
		// Create the list of luminaries
		luminaries.clear();

		const IObjectManager* pObjMan = pScene->GetObjects();
		ObjectEnumerationDispatch dispatch( *this );
		pObjMan->EnumerateObjects( dispatch );
	} else {
		GlobalLog()->PrintSourceError( "LuminaryManager::AttachScene:: No scene specified", __FILE__, __LINE__ );
	}
}

void LuminaryManager::AddToLuminaryList( const IObject& pObject )
{
	const IMaterial*	pMaterial = pObject.GetMaterial();
	if( pMaterial && pMaterial->GetEmitter() )
	{
		// The light sampler treats a luminary as an area light: it calls
		// UniformRandomPoint() expecting a uniform sample of the emitting SURFACE
		// and GetArea() for pdfPosition = 1/area.  A geometry that cannot honour
		// that contract (CanBeAreaLight() false -- e.g. a field that tessellates to
		// zero surface area) would feed NEE wrong positions and pdfs.  Refuse it as
		// a luminary, with a diagnostic, rather than sample a broken light.  The
		// object still renders and still contributes emission when directly viewed
		// or reached by a BSDF-sampled ray -- it simply is not importance-sampled
		// by NEE.
		//
		// NULL GEOMETRY (crash fix): pObject.GetGeometry() can legitimately be
		// null -- a CSGObject has no single owned IGeometry* (its shape is
		// synthesized on the fly from its two operand objects; see CSGObject.h/
		// .cpp, which override IntersectRay/getBoundingBox but NOT GetArea() or
		// UniformRandomPoint(), so those fall through to the base Object's
		// `pGeometry->...` and null-deref if ever invoked).  The condition below
		// used to be `pGeom && !pGeom->CanBeAreaLight()`, which short-circuited
		// to false -- i.e. "acceptable" -- for exactly this null case, so a
		// csg_object with an emissive material bound was pushed onto the
		// luminaries list, and LightSampler later called GetArea() on it
		// (Object::GetArea() -> pGeometry->GetArea() with pGeometry == 0) ->
		// EXC_BAD_ACCESS.  A null geometry can never honour the
		// UniformRandomPoint()/GetArea() contract, so it must be refused the
		// same as CanBeAreaLight() == false.
		//
		// TRUTH-DEFECT FIX (2026-07-31 fix round 2, scoped precisely fix round
		// 3 / P2b): this gate alone does NOT make "the object still glows
		// when directly viewed" true by itself -- it only keeps the object
		// OFF the NEE luminaries list (this function's job).  A refused
		// object's emission is read back on a DIFFERENT code path entirely
		// (EmissionShaderOp / the integrators' BSDF-hit emission blocks, and
		// BDPT/VCM's s=0 / path-length-1 strategy), which had the IDENTICAL
		// null-geometry crash independently (fixed alongside this in round 2:
		// EmissionShaderOp.cpp, PathTracingIntegrator.cpp, BDPTIntegrator.cpp,
		// VCMIntegrator.cpp, plus a base-layer null guard on Object::GetArea()/
		// UniformRandomPoint() in Object.cpp).
		//
		// EXACTLY WHAT tests/CSGNullGeometryLuminaireCrashTest.cpp VERIFIES
		// (P2b: do not overclaim beyond this) --
		//   DIRECT camera view, non-crashing + non-zero radiance:
		//     pathtracing_pel_rasterizer, bdpt_pel_rasterizer,
		//     vcm_pel_rasterizer, AND the legacy EmissionShaderOp
		//     (DefaultEmission) shaderop chain under pixelpel_rasterizer.
		//   INDIRECT (BSDF-sampled, bsdfPdf>0) bounce hit, non-crashing +
		//     non-zero radiance: pathtracing_pel_rasterizer ONLY
		//     (PathTracingIntegrator.cpp's bsdfPdf>0 block) -- EmissionShaderOp's
		//     OWN bsdfPdf>0 block is gated identically (code-verified, same
		//     `pEmitGeom && pEmitGeom->CanBeAreaLight()` fix) but NOT
		//     independently exercised by an indirect-bounce test.
		//   NOT tested at all: the spectral/HWSS twins of every path above
		//     (PathTracingIntegrator's NM loop, BDPT/VCM spectral rasterizers)
		//     -- code-identical fixes (same file, same pattern) but no
		//     dedicated spectral render test.
		//   BDPT NOTE (updated 2026-08-01): BDPT's s=0 strategy USED to hit a
		//     separate, pre-existing MIS defect -- MISWeight's remap0 promoting
		//     a non-NEE-sampleable emitter's true pdfRev=0 to 1, manufacturing
		//     a phantom NEE strategy in the denominator and giving BDPT LESS
		//     than PT/VCM's full weight here (an energy deficit, not a crash).
		//     That is now FIXED (BDPTVertex::lightSamplingStrategyAbsent +
		//     MISWeight's eye-walk gate; see BDPTIntegrator.cpp's s=0 block).
		//     BDPT now carries FULL weight on such emitters, matching PT and
		//     VCM -- pinned by tests/BDPTPhantomStrategyWeightTest.cpp, which
		//     holds BDPT's mean luma to within 5% of PT's on both flavours of
		//     non-NEE-sampleable emitter (null-geometry CSG and a
		//     CanBeAreaLight()==false SDF).
		const IGeometry* pGeom = pObject.GetGeometry();
		if( !pGeom || !pGeom->CanBeAreaLight() ) {
			GlobalLog()->PrintSourceError(
				!pGeom ?
				"LuminaryManager:: an emissive material is bound to an object with no directly-"
				"owned geometry (e.g. a csg_object, whose shape comes from its two operand "
				"objects rather than a single geometry chunk) -- it cannot be uniformly area-"
				"sampled.  It will NOT act as an area light (no NEE importance sampling); it is "
				"never selected by light-sampling, but still contributes emission on direct "
				"camera view (PT/BDPT/VCM pel + the legacy EmissionShaderOp chain, verified by "
				"tests/CSGNullGeometryLuminaireCrashTest.cpp) or a BSDF-sampled hit (PT pel, "
				"verified; code-identical elsewhere, untested)." :
				"LuminaryManager:: an emissive material is bound to a geometry that cannot be "
				"uniformly area-sampled (CanBeAreaLight() == false, e.g. a degenerate zero-area "
				"field, or an SDF whose sampling mesh provably missed renderable surface -- see "
				"the SDFGeometry warning above; raise sampling_detail).  It will NOT act as an "
				"area light (no NEE importance sampling); it is never selected by light-sampling, "
				"but still contributes emission on direct camera view or a BSDF-sampled hit.",
				__FILE__, __LINE__ );
			return;
		}

		LUM_ELEM elem;
		elem.pLum = &pObject;
		pObject.addref();
		luminaries.push_back( elem );
	}
}


void LuminaryManager::SetLuminaireSampling( ISampling2D* pLumSam )
{
	if( pLumSam )
	{
		safe_release( pLumSampling );
		pLumSampling = pLumSam;
		pLumSampling->addref();
	}
}

