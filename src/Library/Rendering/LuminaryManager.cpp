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
		// object still renders and still emits when hit directly / by BSDF rays --
		// it simply is not importance-sampled by NEE.
		const IGeometry* pGeom = pObject.GetGeometry();
		if( pGeom && !pGeom->CanBeAreaLight() ) {
			GlobalLog()->PrintSourceError(
				"LuminaryManager:: an emissive material is bound to a geometry that cannot be "
				"uniformly area-sampled (CanBeAreaLight() == false, e.g. a degenerate zero-area "
				"field, or an SDF whose sampling mesh provably missed renderable surface -- see "
				"the SDFGeometry warning above; raise sampling_detail).  It will NOT act as an "
				"area light (no NEE importance sampling); it still glows when viewed / hit by "
				"BSDF rays.", __FILE__, __LINE__ );
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

