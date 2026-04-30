//////////////////////////////////////////////////////////////////////
//
//  NormalMap.h - Tangent-space normal-map modifier.  Samples a
//  normal-map painter at the hit's TEXCOORD_0, decodes the
//  tangent-space normal, transforms it to world space using the
//  hit's TBN basis, and updates the intersection's normal + ONB.
//
//  Designed for glTF 2.0 normal maps but works with any source: the
//  glTF convention (RGB in linear-encoded [0,1] mapping to vector
//  components in [-1,1]) is what every modern exporter emits.  The
//  caller is responsible for setting up the normal-map painter with a
//  color space that does NOT apply a gamma decode AND does NOT apply
//  a Rec709 -> ROMM color-matrix conversion -- both would warp the
//  encoded vector.  In RISE's current build (RISEPel = ROMMRGBPel,
//  see Color.h) that means `color_space ROMMRGB_Linear` on the
//  png_painter / jpg_painter / etc. -- this loads the bytes verbatim
//  into the engine's working space with zero matrix conversion.
//  `Rec709RGB_Linear` would skip the gamma but still apply the
//  Rec709 -> ROMM matrix and produce subtly wrong normals.  The
//  visual-regression scene gltf_normal_mapped.RISEscene captures the
//  recommended setup.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 30, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef NORMAL_MAP_MODIFIER_
#define NORMAL_MAP_MODIFIER_

#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"
#include "../Interfaces/IRayIntersectionModifier.h"

namespace RISE
{
	namespace Implementation
	{
		class NormalMap :
			public virtual IRayIntersectionModifier,
			public virtual Reference
		{
		protected:
			virtual ~NormalMap();

			const IPainter&		pNormalMap;		///< Normal-map texture painter (sampled in linear, NOT sRGB-decoded)
			Scalar				dScale;			///< glTF normalTexture.scale: scales the xy components of the sampled normal

		public:
			NormalMap( const IPainter& painter, const Scalar scale );
			void Modify( RayIntersectionGeometric& ri ) const;
		};
	}
}

#endif
