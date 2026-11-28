//////////////////////////////////////////////////////////////////////
//
//  BSSRDFSampling.h - Shared BSSRDF importance sampling utility
//
//  Disk-projection entry-point sampling (Christensen & Burley 2015)
//  shared by PathTracingIntegrator and BDPTIntegrator, plus contact-
//  launch and boundary-Fresnel helpers.
//
//  FACTORIZATION:
//    S(wo, xo, wi, xi) = C * Ft(wo) * Rd(||xo - xi||) * Ft(wi)
//    where Ft = 1 - F is exact dielectric Fresnel transmission
//    (matching SubSurfaceScatteringSPF reflection so R + Ft = 1),
//    C = 1 / (c * PI), c = 2 * integral Ft(mu) mu dmu for relative
//    eta = n_interior / n_exterior.  SampleEntryPoint returns:
//      weight        = Rd * Ft(exit) * Ft(entry) / (c * pdfSurface)
//      weightSpatial = Rd * Ft(exit) / pdfSurface (NEE / connections)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 30, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BSSRDF_SAMPLING_
#define BSSRDF_SAMPLING_

#include "Math3D/Math3D.h"
#include "Math3D/Constants.h"
#include "../Interfaces/IObject.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ISubSurfaceDiffusionProfile.h"
#include "../Intersection/RayIntersection.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include "ISampler.h"
#include "Optics.h"

namespace RISE { class IObjectManager; }

namespace RISE
{
	namespace BSSRDFSampling
	{
		/// Small epsilon for ray offsets to avoid self-intersection.
		constexpr Scalar BSSRDF_RAY_EPSILON = 1e-6;

		/// Result of BSSRDF importance sampling at a surface vertex
		struct SampleResult
		{
            Scalar acceptedAlphaCoverage = 1;
			Point3				entryPoint;		///< Entry point on the surface
			/// On-surface entry point before BSSRDF_RAY_EPSILON outward offset (contact launch origin).
			Point3				boundaryPoint;
			Vector3				entryNormal;	///< Shading normal at entry (continuation frame & Sw Fresnel).
			Vector3				entryGeomNormal;///< Geometric normal at entry (area Jacobian & entry facing gate; independent of modifiers).
			OrthonormalBasis3D	entryONB;		///< ONB at entry point
			SurfaceDerivativesInfo derivatives;	///< Surface derivatives at entry point (DL-22)
			SurfaceSignalInfo	signals;		///< Surface signals at entry point (DL-22)
			TextureFootprint	txFootprint;	///< Texture footprint at entry point (DL-22)
			Point2				ptCoord;		///< Texture coordinates at entry point (DL-22)
			Point2				ptCoord1;		///< Secondary texture coordinates (DL-22)
			bool				bHasTexCoord1;	///< True if secondary texture coordinates exist (DL-22)
			Point3				ptObjIntersec;	///< Object-space intersection point (DL-22)
			RISEPel				vColor;			///< Vertex color at entry point (DL-22)
			bool				bHasVertexColor;///< True if vertex color exists (DL-22)
			const Matrix4*		pmxWorldToObject;///< Group 7 space transforms at entry hit
			const Matrix4*		pmxObjectToWorld;///< Group 7 space transforms at entry hit
			const ICamera*		pCamera;		///< Live scene camera at entry hit
			const ObjectInfoData* pObjectInfo;	///< Per-object metadata at entry hit
			Ray					scatteredRay;	///< Cosine-weighted ray from entry point
			RISEPel				weight;			///< Full BSSRDF weight: Rd * Ft(exit) * Ft(entry) / (c * pdfSurface)
			RISEPel				weightSpatial;	///< Spatial-only weight: Rd * Ft(exit) / pdfSurface (no entry Sw)
			Scalar				weightNM;		///< Scalar weight for spectral path (full)
			Scalar				weightSpatialNM;///< Scalar spatial-only weight for spectral path
			Scalar				cosinePdf;		///< PDF of the cosine-weighted direction
			Scalar				pdfSurface;		///< Spatial sampling PDF in area measure
			/// Diffusion jump inputs for HWSS companion re-pricing (RecomputeSubpathThroughputNM):
			/// 3D exit-to-entry distance and exit cosine.  Set by SampleEntryPoint; 0 from RandomWalkSSS.
			Scalar				jumpDistance;
			Scalar				exitCos;
			/// Exited body/material/absolute IOR when a random walk crosses into a touching
			/// neighbour; null / 0 means the entry object (resolved via ExitObject/ExitMaterial/ExitIOR).
			const IObject*		pExitObject;
			const IMaterial*	pExitMaterial;
			Scalar				exitIOR;
			bool				valid;			///< True if sampling succeeded

			const IObject* ExitObject( const IObject* pEntryObject ) const
			{ return pExitObject ? pExitObject : pEntryObject; }
			const IMaterial* ExitMaterial( const IMaterial* pEntryMaterial ) const
			{ return pExitMaterial ? pExitMaterial : pEntryMaterial; }
			Scalar ExitIOR( const Scalar entryIOR ) const
			{ return exitIOR > 0 ? exitIOR : entryIOR; }

			SampleResult() :
			bHasTexCoord1( false ),
			vColor( RISEPel(0,0,0) ),
			bHasVertexColor( false ),
			pmxWorldToObject( 0 ), pmxObjectToWorld( 0 ), pCamera( 0 ), pObjectInfo( 0 ),
			weight( RISEPel(0,0,0) ), weightSpatial( RISEPel(0,0,0) ),
			weightNM(0), weightSpatialNM(0),
			cosinePdf(0), pdfSurface(0), jumpDistance(0), exitCos(0),
			pExitObject(0), pExitMaterial(0), exitIOR(0), valid(false) {}
		};

		// Boundary helpers use the relative index eta = n_interior / n_exterior,
		// where exterior comes from ri.ambientIOR (default 1.0 = air), matching
		// SubSurfaceScatteringSPF reflection.  Sw normalizes to unit exterior
		// cosine-hemisphere integral.

		/// The exterior (incident-medium) index carried by a hit record.
		/// Non-positive or non-finite values fall back to air.
		inline Scalar ExteriorIOR( const RayIntersectionGeometric& ri )
		{
			const Scalar n = ri.ambientIOR;
			return ( n > 0 && n < RISE_INFINITY ) ? n : Scalar( 1.0 );
		}

		/// Relative boundary index n_interior / n_exterior.
		inline Scalar RelativeBoundaryIOR( const Scalar interiorIOR, const Scalar exteriorIOR )
		{
			return ( exteriorIOR > 0 && exteriorIOR < RISE_INFINITY )
				? interiorIOR / exteriorIOR : interiorIOR;
		}

		/// Exact dielectric Fresnel transmission Ft = 1 - F at exterior cosine mu
		/// (matches SubSurfaceScatteringSPF reflection so R + Ft = 1; handles
		/// eta < 1 TIR and eta == 1).  Ordered absolute pair avoids critical-
		/// incidence ratio rounding.
		inline Scalar BoundaryTransmission( const Scalar mu,
			const Scalar interiorIOR, const Scalar exteriorIOR )
		{
			return 1.0 - Optics::CalculateDielectricReflectanceCosine(mu, exteriorIOR, interiorIOR);
		}
		// Relative-index convenience for explicitly dimensionless laws/tests.
		inline Scalar BoundaryTransmission( const Scalar mu, const Scalar eta )
		{
			return BoundaryTransmission(mu, eta, 1.0);
		}

		/// Exact closed-form diffuse Fresnel reflectance r(n) = 2 * integral_0^1
		/// F(mu; 1->n) mu dmu for n > 1 (accurate to < 1e-11 for n - 1 >= 1e-3;
		/// unstable below that due to log cancellation).
		inline Scalar ExactExternalDiffuseFresnelReflectance( const Scalar n )
		{
			const Scalar n2 = n * n;
			const Scalar n4 = n2 * n2;
			const Scalar np1 = n + 1.0;
			const Scalar n2p1 = n2 + 1.0;
			const Scalar n4m1 = n4 - 1.0;
			return 0.5
				+ (n - 1.0) * (3.0 * n + 1.0) / (6.0 * np1 * np1)
				+ n2 * (n2 - 1.0) * (n2 - 1.0) / (n2p1 * n2p1 * n2p1) * log( (n - 1.0) / np1 )
				- 2.0 * n2 * n * (n2 + 2.0 * n - 1.0) / (n2p1 * n4m1)
				+ 8.0 * n4 * (n4 + 1.0) / (n2p1 * n4m1 * n4m1) * log( n );
		}

		/// Exterior cosine-hemisphere normalization c = 2 * integral_0^1 Ft(mu) mu dmu
		/// for relative eta (docs/DEBT_LEDGER.md).  Linearly interpolates for
		/// |n - 1| < 1e-3 (error < 1e-6); for eta < 1 uses c(eta) = eta^2 * c(1/eta).
		inline Scalar BoundaryTransmissionNormalization( const Scalar eta )
		{
			if( !( eta > 0 ) || !( eta < RISE_INFINITY ) ) {
				return 0;
			}
			const bool bDenserExterior = eta < 1.0;
			const Scalar n = bDenserExterior ? 1.0 / eta : eta;
			const Scalar kNearOne = 1e-3;
			Scalar c;
			if( n - 1.0 >= kNearOne ) {
				c = 1.0 - ExactExternalDiffuseFresnelReflectance( n );
			} else {
				const Scalar cAnchor = 1.0 - ExactExternalDiffuseFresnelReflectance( 1.0 + kNearOne );
				c = 1.0 + (cAnchor - 1.0) * ((n - 1.0) / kNearOne);
			}
			return bDenserExterior ? eta * eta * c : c;
		}

		/// Directional scattering factor Sw(wi) = Ft(cos_theta_i) / (c * PI)
		/// (achromatic) for relative eta = n_interior / n_exterior.
		inline Scalar EvaluateSwWithFresnel(
			const Scalar FtEntry,					///< [in] Fresnel transmission at entry point
			const Scalar eta						///< [in] RELATIVE index n_interior / n_exterior (DL-49)
			)
		{
			const Scalar c = BoundaryTransmissionNormalization( eta );

			if( c > 1e-20 ) {
				return FtEntry / (c * PI);
			}
			return 0;
		}

		/// Disk-projection BSSRDF entry-point sampling (Christensen & Burley 2015)
		/// against pObject alone.  Returns SampleResult with valid=true on success.
		SampleResult SampleEntryPoint(
			const RayIntersectionGeometric& ri,		///< [in] Exit point intersection
			const IObject* pObject,					///< [in] Object to cast probe rays against
			const IMaterial* pMaterial,				///< [in] Material with diffusion profile
			ISampler& sampler,						///< [in] Sampler for stochastic decisions
			const Scalar nm,						///< [in] Wavelength for NM path (0 = RGB)
			const IRayIntersectionModifier* pExitLeafModifier = 0	///< [in] Optional resolved mix leaf modifier (Round 3 P1-4)
			);

		// Contact launches (docs/DEBT_LEDGER.md): cast against touching target T
		// alone from backed-off o = p - d * delta so a shared face hits at t ~= delta
		// instead of falling under the self-hit floor; accepted hits set
		// pContactPartner = pSource.

		/// The back-off delta of a contact launch from `p`: scale-relative,
		/// far above every primitive's self-hit floor (~1e-12 (1+s)).
		inline Scalar ContactBackoff( const Point3& p )
		{
			const Scalar s = r_max( r_max( fabs( p.x ), fabs( p.y ) ), fabs( p.z ) );
			return BSSRDF_RAY_EPSILON * ( Scalar( 1 ) + s );
		}

		/// The contact launch ray from surface point `p` along unit `dir`.
		inline Ray ContactRay( const Point3& p, const Vector3& dir )
		{
			return Ray( Point3Ops::mkPoint3( p, dir * ( -ContactBackoff( p ) ) ), dir );
		}

		/// Casts a ContactRay against `pTarget` alone; accepts iff
		/// `delta/2 <= range <= delta + reach` where `reach = 4 * BSSRDF_RAY_EPSILON
		/// * (1+s) / max(|cos|, 0.05)`.  Sets `pContactPartner = pSource` on hit
		/// and resets `ri` on miss.  Deterministic in its inputs.
		bool IntersectContactRay(
			const IObjectManager& objects,			///< [in] The scene's objects
			const IObject* pTarget,					///< [in] The only object tested
			const IObject* pSource,					///< [in] The launching body (the hit's partner)
			RayIntersection& ri,					///< [in/out] Ray in (a ContactRay), hit out
			ISampler* pSampler = 0					///< [in] Optional sampler for mix/alpha contact tests
			);

		/// ContactRay + IntersectContactRay from surface point `p` along `dir`.
		bool IntersectContact(
			const IObjectManager& objects,
			const IObject* pTarget,
			const IObject* pSource,
			const Point3& p,
			const Vector3& dir,
			const RasterizerState& rast,
			RayIntersection& ri,
			ISampler* pSampler = 0
			);

		/// Finds the nearest touching body at an SSS exit along `dir` (or null).
		/// Accepts enclosing bodies (world-box nesting) and rejects flush insets.
		/// Draws no samples.
		const IObject* FindExitContact(
			const IObjectManager& objects,
			const SampleResult& r,
			const IObject* pExitObject,
			const Vector3& dir
			);

		/// General contact search from surface point `p` (outward normal `n`) of
		/// `pSelf` along `dir`.  `bSSSTargetsOnly` restricts candidates to SSS
		/// bodies (used for emitter-on-SSS launches so sheet emitters don't hit
		/// coincident ceilings/enclosures).
		const IObject* FindSurfaceContact(
			const IObjectManager& objects,
			const Point3& p,
			const Vector3& n,
			const IObject* pSelf,
			const Vector3& dir,
			const bool bSSSTargetsOnly = false
			);

		/// True when `pPartner` casts shadows and occludes zero-distance shadow/
		/// connection rays (opaque, non-full-sphere, non-delta-pass-through,
		/// non-alpha, non-unresolved-mix).
		inline bool ContactCovers( const IObject* pPartner )
		{
			if( !pPartner || !pPartner->DoesCastShadows() ) {
				return false;
			}
			const IMaterial* pMat = pPartner->GetMaterial();
			// Alpha-covered and unresolved MixMaterials must not short-circuit
			// zero-distance shadow/connection visibility.
			return pMat && !pMat->ScattersFullSphere() && !pMat->HasDeltaPassThrough()
				&& !pMat->HasAlphaCoverage() && !pMat->IsUnresolvedMix();
		}

		/// Resolves closest-hit ties on exterior entry into an SSS body when
		/// another surface (sheet, adjacent exit, or enclosing body) coincides
		/// within a coordinate rounding budget; rejects flush insets.  Called
		/// from ObjectManager::IntersectRay; draws no samples.
		bool ResolveCoincidentEntry(
			const IObjectManager& objects,
			RayIntersection& ri,
			const bool bHitFrontFaces,
			const bool bHitBackFaces
			);

		/// Manager overload: intersect the chosen neighbour with the caller's
		/// exit-information request. The four-argument entry point stays intact.
		bool ResolveCoincidentEntry(
			const IObjectManager& objects, RayIntersection& ri,
			const bool bHitFrontFaces, const bool bHitBackFaces,
			const bool bComputeExitInfo );

		/// At a CONTACT hit (`ri.pContactPartner` set): the body a
		/// continuation in `dirOut` meets at zero distance -- the partner
		/// when `dirOut` goes back to the arrival side of the hit's
		/// geometric plane, else null.  Null for every ordinary hit.
		inline const IObject* ContactReturnTarget( const RayIntersection& ri, const Vector3& dirOut )
		{
			if( !ri.pContactPartner ) {
				return 0;
			}
			const Vector3& ng = ri.geometric.vGeomNormal;
			const Scalar a = Vector3Ops::Dot( dirOut, ng );
			const Scalar b = -Vector3Ops::Dot( ri.geometric.ray.Dir(), ng );
			return ( a * b > 0 ) ? ri.pContactPartner : 0;
		}
	}
}

#endif
