//////////////////////////////////////////////////////////////////////
//
//  LuminaryManager.h - Defines a class which handles luminary
//  materials.  It automatically samples them properly and all
//  and returns the light contribution of all the luminaries
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: July 30, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef LUMINARY_MANAGER_
#define LUMINARY_MANAGER_

#include "../Interfaces/ILuminaryManager.h"
#include "../Utilities/Reference.h"
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class LuminaryManager : 
			public virtual ILuminaryManager, 
			public virtual Implementation::Reference
		{
		public:
			struct LUM_ELEM
			{
				const IObject*					pLum;

				LUM_ELEM() : pLum( 0 ) {}
			};

			typedef std::vector<LUM_ELEM> LuminariesList;

		protected:
			virtual ~LuminaryManager( );

			ISampling2D*			pLumSampling;
			LuminariesList			luminaries;

		public:
			LuminaryManager();

			//! Binds the luminary manager to a particular scene
			void AttachScene(
				const IScene* pScene											///< [in] Scene to bind to
				);

			//! Adds the object to the list of luminaries
			void AddToLuminaryList(
				const IObject& pObject											///< [in] Object to add
				);

			//! Sets up luminaire sampling
			void SetLuminaireSampling(
				ISampling2D* pLumSam											///< [in] Sampling kernel to use when the luminaire needs to be sampled
				);

			//! Returns the list of luminaries
			const LuminariesList& getLuminaries( ){ return luminaries; };
		};
	}
}

#endif

