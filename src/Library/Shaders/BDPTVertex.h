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

		// ---------------------------------------------------------------
		// Surface state mirrored from RayIntersectionGeometric — the fields
		// in this block PLUS the per-vertex-color block below (vColor,
		// bHasVertexColor).  When adding any new mirrored field, also
		// update PathVertexEval::PopulateRIGFromVertex AND extend
		// tests/BDPTVertexRIGRebuildTest.cpp with a sentinel assertion.
		// See the contract block above PopulateRIGFromVertex for the
		// failure mode this protocol prevents.
		// ---------------------------------------------------------------
		Point3					position;
		Vector3					normal;			///< Shading normal (Phong-interpolated, possibly bump/normal-mapped).
												///< Use for BSDF eval / sample / pdf and the BSDF cosine factor.
		Vector3					geomNormal;		///< Geometric flat-face normal, independent of Phong / bump.
												///< Use for side-of-surface tests (entering/exiting, front/back),
												///< medium-stack push/pop, solid-angle->area Jacobian cosines, and
												///< any rebuilt RayIntersectionGeometric that downstream code asks
												///< "which side of the actual surface is this?".
												///< On analytical primitives equals `normal` by construction.
		OrthonormalBasis3D		onb;
		Point2					ptCoord;		///< Texture coordinate at intersection (for painter evaluation)
		Point3					ptObjIntersec;	///< Object-space intersection point (for 3D procedural painters)
		const IMaterial*		pMaterial;		///< NULL for camera/light endpoints
		const IObject*			pObject;		///< NULL for camera/light endpoints

		/// Per-vertex color interpolated by the geometry at the hit point
		/// (linear ROMM RGB; see RISEPel).  Captured during subpath
		/// generation and replayed into the reconstructed
		/// RayIntersectionGeometric on connection / merge so the
		/// VertexColorPainter sees the same color it would on a direct PT
		/// path.  bHasVertexColor mirrors the intersection field of the
		/// same name; false for camera / light / medium vertices and for
		/// surface hits on geometry without per-vertex colors.
		RISEPel					vColor;
		bool					bHasVertexColor;

		RISEPel					throughput;		///< Cumulative throughput from subpath origin (alpha_i)
		Scalar					throughputNM;	///< Spectral throughput for a single wavelength
		Scalar					pdfFwd;			///< Forward PDF in area measure
		Scalar					pdfRev;			///< Reverse PDF in area measure (filled during MIS weight computation)

		/// Solid-angle emission / importance PDF at path endpoints, consumed
		/// by the VCM post-pass when it recovers per-vertex dVCM/dVC/dVM
		/// running quantities from this vertex's area-measure pdfFwd.
		/// - Light endpoint (vertex 0 on a light subpath): pdfSelect *
		///   pdfPosition * pdfDirection — the full emission solid-angle
		///   direction PDF on the light.
		/// - Camera endpoint (vertex 0 on an eye subpath): pdfCamDir — the
		///   camera's directional importance PDF in solid-angle measure.
		/// - All other vertices: unused; leave at zero.
		/// BDPT itself never reads this field; it only exists to feed the
		/// VCM integrator.
		Scalar					emissionPdfW;

		/// Cached generator-side cosine used by the VCM post-pass when
		/// inverting the area-measure PDF conversion back to solid angle.
		///   pdf_solidAngle = pdf_area * distSq / |cosAtGen|
		/// - Light endpoint (vertex 0): |n_light . direction_out|.
		/// - Camera endpoint (vertex 0): 1.0 (sentinel; camera init uses
		///   emissionPdfW directly).
		/// - Surface vertices: absCosIn of the incoming ray at this vertex.
		/// - Medium vertices: 0 (sentinel; VCM uses sigma_t_scalar instead).
		/// - BSSRDF / random-walk SSS exit vertices: 0 (sentinel; the
		///   post-pass terminates the recurrence on these).
		/// BDPT itself never reads this field; it only exists to feed the
		/// VCM integrator.
		Scalar					cosAtGen;
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
		ptCoord( Point2( 0, 0 ) ),
		ptObjIntersec( Point3( 0, 0, 0 ) ),
		pMaterial( 0 ),
		pObject( 0 ),
		vColor( RISEPel( 0, 0, 0 ) ),
		bHasVertexColor( false ),
		throughput( RISEPel( 0, 0, 0 ) ),
		throughputNM( 0 ),
		pdfFwd( 0 ),
		pdfRev( 0 ),
		emissionPdfW( 0 ),
		cosAtGen( 0 ),
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
