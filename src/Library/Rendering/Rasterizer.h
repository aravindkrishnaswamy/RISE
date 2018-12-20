//////////////////////////////////////////////////////////////////////
//
//  Rasterizer.h - Implementation help for rasterizers
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 29, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RASTERIZER_
#define RASTERIZER_

#include "../Utilities/Reference.h"
#include "../Interfaces/IRasterizer.h"
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class Rasterizer : public virtual IRasterizer, public virtual Reference 
		{
		protected:
			typedef std::vector<IRasterizerOutput*>	RasterizerOutputListType;
			RasterizerOutputListType				outs;
			IProgressCallback*						pProgressFunc;

			Rasterizer();
			virtual ~Rasterizer();

			// Figures out the number of threads to spawn based on the number of
			// processors in the system and the option settings
			int HowManyThreadsToSpawn() const;

		public:
			virtual void AddRasterizerOutput( IRasterizerOutput* ro );
			virtual void FreeRasterizerOutputs( );
			virtual void EnumerateRasterizerOutputs( IEnumCallback<IRasterizerOutput>& pFunc ) const;
			virtual void SetProgressCallback( IProgressCallback* pFunc );
		};
	}
}


#endif
