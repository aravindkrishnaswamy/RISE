//////////////////////////////////////////////////////////////////////
//
//  SheenMaterial.h - Material wrapper combining SheenBRDF and
//  SheenSPF for fabric / cloth.  Designed as the top layer in a
//  CompositeMaterial(top=sheen, bottom=baseGGX) pairing for glTF
//  KHR_materials_sheen assets, but usable standalone for hand-
//  authored fabric.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 30, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SHEEN_MATERIAL_
#define SHEEN_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ILog.h"
#include "SheenBRDF.h"
#include "SheenSPF.h"

namespace RISE
{
	namespace Implementation
	{
		class SheenMaterial :
			public virtual IMaterial,
			public virtual Reference
		{
		protected:
			SheenBRDF*	pBRDF;
			SheenSPF*	pSPF;

			virtual ~SheenMaterial()
			{
				safe_release( pBRDF );
				safe_release( pSPF );
			}

		public:
			SheenMaterial(
				const IPainter& sheenColor,
				const IPainter& sheenRoughness
				)
			{
				pBRDF = new SheenBRDF( sheenColor, sheenRoughness );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new SheenSPF( sheenColor, sheenRoughness );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );
			}

			inline IBSDF* GetBSDF() const     { return pBRDF; }
			inline ISPF*  GetSPF()  const     { return pSPF;  }
			inline IEmitter* GetEmitter() const { return 0; }
		};
	}
}

#endif
