//////////////////////////////////////////////////////////////////////
//
//  VolumeAccessor_Painter.h - Volume accessor that evaluates a
//    painter at world-space points to produce density values
//
//  Wraps any IPainter (e.g., Perlin3DPainter, Voronoi3DPainter)
//  behind the IVolumeAccessor interface so that it can drive a
//  HeterogeneousMedium's density field without any changes to
//  HeterogeneousMedium or MajorantGrid.
//
//  Centered volume coordinates are reverse-mapped to world-space
//  points (mirroring HeterogeneousMedium::LookupDensity's forward
//  mapping), then a synthetic RayIntersectionGeometric is
//  constructed to evaluate the painter.  The resulting color is
//  converted to a scalar density in [0, 1].
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 12, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef VOLUME_ACCESSOR_PAINTER_
#define VOLUME_ACCESSOR_PAINTER_

#include "../Interfaces/IVolumeAccessor.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"
#include "../Utilities/Color/ColorMath.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include <math.h>

namespace RISE
{
	/// \brief Volume accessor that evaluates a painter for density
	///
	/// Converts centered volume coordinates back to world-space
	/// points and evaluates a painter to produce scalar density.
	/// Does not use an IVolume -- BindVolume() is a no-op.
	class VolumeAccessor_Painter :
		public virtual IVolumeAccessor,
		public virtual Implementation::Reference
	{
	protected:
		const IPainter*		m_pPainter;			///< Painter to evaluate (ref-counted)
		unsigned int		m_volWidth;			///< Virtual volume width
		unsigned int		m_volHeight;		///< Virtual volume height
		unsigned int		m_volDepth;			///< Virtual volume depth
		Point3				m_bboxMin;			///< World-space AABB minimum
		Vector3				m_extent;			///< World-space AABB extent (max - min)
		char				m_colorToScalar;	///< Conversion mode: 'l', 'm', or 'r'

		virtual ~VolumeAccessor_Painter()
		{
			safe_release( m_pPainter );
		}

		/// Convert centered volume coordinates to a world-space point.
		/// This is the reverse of HeterogeneousMedium::LookupDensity's
		/// forward mapping:
		///   Forward: nx = (world_x - bboxMin.x) / extent.x
		///            vx = (nx - 0.5) * volWidth
		///   Reverse: nx = vx / volWidth + 0.5
		///            world_x = nx * extent.x + bboxMin.x
		inline Point3 CenteredToWorld( const Scalar vx, const Scalar vy, const Scalar vz ) const
		{
			const Scalar nx = vx / Scalar(m_volWidth) + 0.5;
			const Scalar ny = vy / Scalar(m_volHeight) + 0.5;
			const Scalar nz = vz / Scalar(m_volDepth) + 0.5;

			return Point3(
				nx * m_extent.x + m_bboxMin.x,
				ny * m_extent.y + m_bboxMin.y,
				nz * m_extent.z + m_bboxMin.z );
		}

		/// Convert a RISEPel color to scalar density using the
		/// configured conversion mode.
		inline Scalar ColorToScalar( const RISEPel& color ) const
		{
			Scalar density;
			switch( m_colorToScalar ) {
				case 'm':
					density = ColorMath::MaxValue( color );
					break;
				case 'r':
					density = color[0];
					break;
				case 'l':
				default:
					density = ColorMath::Luminance( color );
					break;
			}
			// Clamp to [0, 1]
			if( density < 0.0 ) density = 0.0;
			if( density > 1.0 ) density = 1.0;
			return density;
		}

	public:
		VolumeAccessor_Painter(
			const IPainter& painter,
			const unsigned int volWidth,
			const unsigned int volHeight,
			const unsigned int volDepth,
			const Point3& bboxMin,
			const Point3& bboxMax,
			const char colorToScalar = 'l'
			) :
		  m_pPainter( &painter ),
		  m_volWidth( volWidth ),
		  m_volHeight( volHeight ),
		  m_volDepth( volDepth ),
		  m_bboxMin( bboxMin ),
		  m_extent( Vector3Ops::mkVector3( bboxMax, bboxMin ) ),
		  m_colorToScalar( colorToScalar )
		{
			m_pPainter->addref();
		}

		Scalar GetValue( Scalar x, Scalar y, Scalar z ) const
		{
			const Point3 worldPt = CenteredToWorld( x, y, z );

			// Build a minimal RayIntersectionGeometric for the painter.
			// Set both ptIntersection (world space, used by Perlin3D)
			// and ptObjIntersec (object space, used by Voronoi3D)
			// to the same world-space point.
			const Ray dummyRay( worldPt, Vector3( 0, 1, 0 ) );
			RayIntersectionGeometric ri( dummyRay, nullRasterizerState );
			ri.ptIntersection = worldPt;
			ri.ptObjIntersec = worldPt;

			const RISEPel color = m_pPainter->GetColor( ri );
			return ColorToScalar( color );
		}

		Scalar GetValue( int x, int y, int z ) const
		{
			return GetValue( Scalar(x), Scalar(y), Scalar(z) );
		}

		void BindVolume( const IVolume* )
		{
			// No-op: this accessor does not use a discrete volume
		}
	};
}

#endif
