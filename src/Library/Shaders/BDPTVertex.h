//////////////////////////////////////////////////////////////////////
//
//  BDPTVertex.h - Vertex data structure for Bidirectional Path
//  Tracing.
//
//  Each vertex stores everything needed for connection evaluation
//  and MIS weight computation:
//
//  - Geometric data (position, normal, ONB) for BSDF evaluation
//    and geometric term computation.
//  - Material pointer for BSDF/SPF lookups during connections.
//  - Cumulative throughput (alpha) from the subpath origin, used
//    directly in the full path contribution formula.
//  - Forward PDF (pdfFwd) in area measure: the probability of
//    generating this vertex during subpath construction.
//  - Reverse PDF (pdfRev) in area measure: the probability of
//    generating this vertex if the subpath were traced in the
//    opposite direction.  Filled retroactively after the next
//    vertex is generated, since it requires the outgoing direction.
//  - isDelta flag: marks specular (delta) interactions so the MIS
//    weight computation can skip strategies that cannot generate
//    this vertex through explicit connections.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BDPT_VERTEX_
#define BDPT_VERTEX_

#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/OrthonormalBasis3D.h"
#include "../Utilities/Color/Color.h"

namespace RISE
{
	class IMaterial;
	class IMedium;
	class IPhaseFunction;
	class IObject;
	class ILight;

	struct BDPTVertex
	{
		/// Vertex classification.
		///
		/// MEDIUM vertices represent volumetric scatter events inside
		/// participating media.  Unlike SURFACE vertices they carry no
		/// material — the angular scattering distribution comes from the
		/// phase function (pPhaseFunc) instead of a BSDF.  Medium
		/// vertices are always connectible and never delta (for smooth
		/// phase functions such as isotropic or Henyey-Greenstein).
		///
		/// In the generalized area-measure PDF (Veach thesis Ch. 11),
		/// sigma_t at the scatter point replaces |cos(theta)| in the
		/// solid-angle-to-area Jacobian:
		///   surface:  pdfArea = pdfDir * |cos| / dist^2
		///   medium:   pdfArea = pdfDir * sigma_t / dist^2
		/// This is stored via sigma_t_scalar and used by the MIS weight
		/// computation.  See BDPTIntegrator.cpp for the full derivation.
		enum Type
		{
			CAMERA = 0,
			LIGHT,
			SURFACE,
			MEDIUM			///< Volumetric scatter event inside participating media
		};

		Type					type;

		Point3					position;
		Vector3					normal;
		OrthonormalBasis3D		onb;
		const IMaterial*		pMaterial;		///< NULL for camera/light endpoints
		const IObject*			pObject;		///< NULL for camera/light endpoints

		RISEPel					throughput;		///< Cumulative throughput from subpath origin (alpha_i)
		Scalar					throughputNM;	///< Spectral throughput for a single wavelength
		Scalar					pdfFwd;			///< Forward PDF in area measure
		Scalar					pdfRev;			///< Reverse PDF in area measure (filled during MIS weight computation)
		bool					isDelta;		///< True if the sampled interaction at this vertex is a delta distribution
		bool					isConnectible;	///< True if material has at least one non-delta BxDF component
		bool					isBSSRDFEntry;	///< True if this vertex is a BSSRDF re-emission point (Sw vertex)
		Scalar					mediumIOR;		///< Top-of-stack IOR seen at this vertex before scattering
		bool					insideObject;	///< True if the current object was already in the IOR stack

		// Medium scatter data (valid only for type == MEDIUM)
		const IMedium*			pMediumVol;		///< The participating medium at this scatter vertex
		const IPhaseFunction*	pPhaseFunc;		///< Phase function for angular scattering distribution
		const IObject*			pMediumObject;	///< Object enclosing the medium (NULL for global medium)
		Scalar					sigma_t_scalar;	///< Scalar extinction at scatter point (MaxValue(sigma_t)),
												///< used as the area-measure conversion factor in place of
												///< |cos(theta)| for surface vertices.

		// Light endpoint data (non-null only for type == LIGHT)
		const ILight*			pLight;
		const IObject*			pLuminary;

		// Camera endpoint data (valid only for type == CAMERA)
		Point2					screenPos;

		// Path-guiding training metadata for the sampled eye-path segment
		Vector3					guidingDirectionOut;
		Vector3					guidingDirectionIn;
		Vector3					guidingNormal;
		RISEPel					guidingScatteringWeight;
		RISEPel					guidingReverseScatteringWeight;	///< f*|cos_in|/revPdf for reversed training segments
		Scalar					guidingPdfDirectionIn;
		Scalar					guidingReversePdfDirectionIn;	///< Solid-angle PDF of sampling guidingDirectionOut
		Scalar					guidingRussianRouletteSurvivalProbability;
		Scalar					guidingEta;
		Scalar					guidingRoughness;
		bool					guidingHasSegment;
		bool					guidingHasDirectionIn;

		BDPTVertex() :
		type( SURFACE ),
		pMaterial( 0 ),
		pObject( 0 ),
		throughput( RISEPel( 0, 0, 0 ) ),
		throughputNM( 0 ),
		pdfFwd( 0 ),
		pdfRev( 0 ),
		isDelta( false ),
		isConnectible( true ),
		isBSSRDFEntry( false ),
		mediumIOR( 1.0 ),
		insideObject( false ),
		pMediumVol( 0 ),
		pPhaseFunc( 0 ),
		pMediumObject( 0 ),
		sigma_t_scalar( 0 ),
		pLight( 0 ),
		pLuminary( 0 ),
		screenPos( Point2( 0, 0 ) ),
		guidingDirectionOut( 0, 0, 0 ),
		guidingDirectionIn( 0, 0, 0 ),
		guidingNormal( 0, 0, 1 ),
		guidingScatteringWeight( RISEPel( 0, 0, 0 ) ),
		guidingReverseScatteringWeight( RISEPel( 0, 0, 0 ) ),
		guidingPdfDirectionIn( 1.0 ),
		guidingReversePdfDirectionIn( 0 ),
		guidingRussianRouletteSurvivalProbability( 1.0 ),
		guidingEta( 1.0 ),
		guidingRoughness( 1.0 ),
		guidingHasSegment( false ),
		guidingHasDirectionIn( false )
		{
		}
	};
}

#endif
