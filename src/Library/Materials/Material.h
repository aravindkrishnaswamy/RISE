//////////////////////////////////////////////////////////////////////
//
//  Material.h - Implementation help for materials
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 19, 2001
//  Tabs: 4
//  Comments:  Changed February 26 / 2002 to support a more robust
//  and much cooler material system.  Note that our spiffy material
//  system is taken from ggLibrary, and *should* be almost totally
//  compatible.  This was needed to facilitate my research work.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MATERIAL_
#define MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class NullMaterial :
			public virtual IMaterial,
			public virtual Reference
		{
		public:
			inline IBSDF* GetBSDF( ) const		{	return 0; };
			inline ISPF* GetSPF() const {			return 0; };
			inline IEmitter* GetEmitter() const {	return 0; };
		};
	}
}

#endif
