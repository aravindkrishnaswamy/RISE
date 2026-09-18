//////////////////////////////////////////////////////////////////////
//
//  Voronoi3DPainter.h - Declaration of a painter which returns a 
//  checker pattern
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 14, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef Voronoi3DPainter_
#define Voronoi3DPainter_

#include "Painter.h"
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class Voronoi3DPainter : public Painter
		{
		public:
			typedef std::pair<Point3,IPainter*> Generator;
			typedef std::vector<Generator>		GeneratorsList;

		protected:		
			const IPainter& border;
			const Scalar border_size;
			GeneratorsList	generators;
			//! P2.5 (doc 88): historically this painter sampled OBJECT space
			//! (`ptObjIntersec`) despite skills/agent/procedural-textures.md
			//! claiming "world-space" for the 3D painter family (perlin3d,
			//! worley3d, ...) -- see the `space` chunk field.  Default FALSE
			//! preserves that historical object-space behaviour byte-
			//! identically; TRUE switches to `ptIntersection` (world space),
			//! matching the other 3D painters and the corrected skill doc.
			const bool		worldSpace;

			virtual ~Voronoi3DPainter( );

			inline const IPainter& ComputeWhich( const RayIntersectionGeometric& ri ) const;

		public:
			Voronoi3DPainter( const GeneratorsList& g, const IPainter& border_, const Scalar border_size_, const bool worldSpace_ = false );

			RISEPel							GetColor( const RayIntersectionGeometric& ri  ) const;
			SpectralPacket					GetSpectrum( const RayIntersectionGeometric& ri ) const;
			Scalar							GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;

			//! SINGLE-SOURCE SELECTOR (Stage C slice 2): `ComputeWhich`
			//! picks ONE child and this forwards to that child's
			//! `GetRadianceNM`.  The generic composed-`GetColor` default is
			//! for painters that BLEND several sources; a selector emits
			//! exactly one child's spectrum, so forwarding preserves a
			//! physical SPD bound to an emissive slot (the default would
			//! re-uplift its RGB projection -- and emit BLACK for a
			//! `piecewise_linear_function`-backed child).
			Scalar							GetRadianceNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ){ return 0;};
			void SetIntermediateValue( const IKeyframeParameter& val ){};
			void RegenerateData( ){};
			//! DL-203 (docs/DEBT_LEDGER.md): `ComputeWhich` selects ONE child
			//! (border or a generator's painter) per query and forwards to its
			//! TRUE `GetColorNM` -- since which child is selected varies with
			//! position, this must OR over EVERY child that could be selected,
			//! not just `border`.  See IPainter::IsSpectrallyDefined's own doc
			//! comment for the full rationale.
			bool IsSpectrallyDefined() const
			{
				if( border.IsSpectrallyDefined() ) { return true; }
				for( GeneratorsList::const_iterator it = generators.begin(), e = generators.end(); it != e; ++it ) {
					if( it->second && it->second->IsSpectrallyDefined() ) { return true; }
				}
				return false;
			}
		};
	}
}

#endif
