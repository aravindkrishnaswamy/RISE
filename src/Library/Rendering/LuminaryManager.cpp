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

