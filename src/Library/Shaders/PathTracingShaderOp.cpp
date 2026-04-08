//////////////////////////////////////////////////////////////////////
//
//  PathTracingShaderOp.cpp - Unified MIS path tracer with SMS
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 26, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PathTracingShaderOp.h"
#include "../Rendering/LuminaryManager.h"
#include "../Lights/LightSampler.h"
#include "../Utilities/IndependentSampler.h"
#include "../Utilities/BSSRDFSampling.h"
#include "../Utilities/RandomWalkSSS.h"
#include "../Utilities/MediumTracking.h"
#include "../Utilities/PathTransportUtilities.h"
#include "../Utilities/PathVertexEval.h"
#include "../Interfaces/ISubSurfaceDiffusionProfile.h"
#ifdef RISE_ENABLE_OPENPGL
#include "../Utilities/PathGuidingField.h"
#endif

using namespace RISE;
using namespace RISE::Implementation;

static inline Scalar GuidingTrainingLuminance( const RISEPel& pel )
{
	return 0.212671 * pel[0] + 0.715160 * pel[1] + 0.072169 * pel[2];
}

static inline bool GuidingWantsDeepSignalTraining( const unsigned int pathDepth )
{
	// Depth 1 is the first camera hit, depth 2 is one bounce away from it.
	// Bias the guide toward transport that required at least two bounces.
	return pathDepth >= 3;
}

//
// Russian Roulette minimum depth and throughput floor.
// Matches BDPT_RR_THRESHOLD in BDPTIntegrator.cpp.
//
// The survival probability is:
//   rrProb = min(1, pathThroughput / max(prevThroughput, PT_RR_THRESHOLD))
//
// In the recursive PT, pathThroughput = rs.importance * MaxValue(throughput)
// and prevThroughput = rs.importance, so the ratio reduces to:
//   rrProb = min(1, MaxValue(throughput) / max(1, PT_RR_THRESHOLD / rs.importance))
//
// When rs.importance is large the floor has no effect and rrProb = MaxValue(throughput).
// When rs.importance is small (deep paths) the floor prevents runaway compensation.
//
// Default RR parameters — used when StabilityConfig is not provided.
static const unsigned int PT_RR_MIN_DEPTH = 3;
static const Scalar PT_RR_THRESHOLD = 0.05;


static inline Vector3 GuidingCosineNormal( const RayIntersectionGeometric& rig )
{
	Vector3 normal = rig.vNormal;
	if( Vector3Ops::Dot( rig.ray.Dir(), normal ) > NEARZERO ) {
		normal = -normal;
	}
	return normal;
}

static inline bool GuidingSupportsSurfaceSampling( const ScatteredRay& scat )
{
	return !scat.isDelta && scat.type == ScatteredRay::eRayDiffuse;
}

static inline IRayCaster::RAY_STATE::RayType PathTracingRayType( const ScatteredRay& scat )
{
	return (scat.type == ScatteredRay::eRayDiffuse && !scat.isDelta) ?
		IRayCaster::RAY_STATE::eRayDiffuse :
		IRayCaster::RAY_STATE::eRaySpecular;
}

/// Propagate per-type bounce counters and glossy filter width from
using PathTransportUtilities::PropagateBounceLimits;

static inline Scalar GuidingSelectionWeight( const ScatteredRay& scat )
{
	return ColorMath::MaxValue( scat.kray );
}

static inline Scalar GuidingSelectionWeightNM( const ScatteredRay& scat )
{
	return fabs( scat.krayNM );
}

static inline Scalar GuidingEffectiveAlpha(
	const Scalar baseAlpha,
	const ScatteredRayContainer& scattered,
	const IMaterial* const pMaterial,
	const IRayCaster::RAY_STATE& rs,
	const bool bNM
	)
{
	if( baseAlpha <= NEARZERO ) {
		return 0;
	}

	Scalar totalWeight = 0;
	Scalar diffuseWeight = 0;
	bool hasNonDiffuse = false;
	bool hasTransmissive = pMaterial && pMaterial->CouldLightPassThrough();

	for( unsigned int i=0; i<scattered.Count(); i++ )
	{
		const ScatteredRay& scat = scattered[i];
		const Scalar w = bNM ? GuidingSelectionWeightNM( scat ) : GuidingSelectionWeight( scat );

		if( w <= NEARZERO ) {
			continue;
		}

		totalWeight += w;

		if( GuidingSupportsSurfaceSampling( scat ) ) {
			diffuseWeight += w;
		} else {
			hasNonDiffuse = true;
		}

		if( scat.isDelta ||
			scat.type == ScatteredRay::eRayRefraction ||
			scat.type == ScatteredRay::eRayTranslucent )
		{
			hasTransmissive = true;
		}
	}

	if( totalWeight <= NEARZERO || diffuseWeight <= NEARZERO ) {
		return 0;
	}

	const Scalar diffuseFraction = diffuseWeight / totalWeight;
	if( rs.type == IRayCaster::RAY_STATE::eRaySpecular || hasTransmissive ) {
		return 0;
	}

	if( hasNonDiffuse && diffuseFraction < 0.8 ) {
		return 0;
	}

	Scalar alpha = baseAlpha;
	if( hasNonDiffuse ) {
		alpha *= diffuseFraction * diffuseFraction;
		if( alpha > 0.2 ) {
			alpha = 0.2;
		}
	}

	return alpha >= 0.05 ? alpha : 0;
}

#ifdef RISE_ENABLE_OPENPGL
namespace
{
	static inline void SetPGLVec3FromRISEPel( pgl_vec3f& dst, const RISEPel& src )
	{
		dst.x = static_cast<float>( src[0] );
		dst.y = static_cast<float>( src[1] );
		dst.z = static_cast<float>( src[2] );
	}

	static inline void AddRISEPelToPGLVec3( pgl_vec3f& dst, const RISEPel& src )
	{
		dst.x += static_cast<float>( src[0] );
		dst.y += static_cast<float>( src[1] );
		dst.z += static_cast<float>( src[2] );
	}

	struct PTGuidingPathRecorder
	{
		PGLPathSegmentStorage storage;
		bool active;

		PTGuidingPathRecorder() :
			storage( 0 ),
			active( false )
		{
		}

		~PTGuidingPathRecorder()
		{
			if( storage ) {
				pglReleasePathSegmentStorage( storage );
				storage = 0;
			}
		}

		void Begin()
		{
			if( !storage ) {
				storage = pglNewPathSegmentStorage();
				if( storage ) {
					pglPathSegmentStorageReserve( storage, 64 );
				}
			}

			if( storage ) {
				pglPathSegmentStorageClear( storage );
				active = true;
			} else {
				active = false;
			}
		}

			void End( PathGuidingField* field )
			{
				if( active && field && storage && pglPathSegmentGetNumSegments( storage ) > 0 ) {
					field->AddPathSegments( storage, false, false, false );
				}

			active = false;
		}
	};

	struct PTGuidingPathScope
	{
		PTGuidingPathRecorder* recorder;
		PathGuidingField* field;
		bool isRoot;

		PTGuidingPathScope(
			PTGuidingPathRecorder* recorder_,
			PathGuidingField* field_,
			const bool isRoot_
			) :
			recorder( recorder_ ),
			field( field_ ),
			isRoot( isRoot_ )
		{
		}

		~PTGuidingPathScope()
		{
			if( isRoot && recorder ) {
				recorder->End( field );
			}
		}
	};

	static inline PTGuidingPathRecorder& GetPTGuidingPathRecorder()
	{
		static thread_local PTGuidingPathRecorder recorder;
		return recorder;
	}

	static inline bool UsePTPathSegmentTraining(
		const RuntimeContext& rc,
		const bool branch
		)
	{
		return !branch && rc.pGuidingField && rc.pGuidingField->IsCollectingTrainingSamples();
	}

	static inline bool IsPTTrainingRootRay( const IRayCaster::RAY_STATE& rs )
	{
		return rs.type == IRayCaster::RAY_STATE::eRayView && rs.depth == 1;
	}

	static inline PGLPathSegmentData* BeginPTGuidingSegment(
		PTGuidingPathRecorder& recorder,
		const RayIntersectionGeometric& rig
		)
	{
		if( !recorder.active || !recorder.storage ) {
			return 0;
		}

		PGLPathSegmentData* segment = pglPathSegmentStorageNextSegment( recorder.storage );
		if( !segment ) {
			return 0;
		}

		pglPoint3f( segment->position,
			static_cast<float>( rig.ptIntersection.x ),
			static_cast<float>( rig.ptIntersection.y ),
			static_cast<float>( rig.ptIntersection.z ) );

		pglVec3f( segment->directionOut,
			static_cast<float>( -rig.ray.Dir().x ),
			static_cast<float>( -rig.ray.Dir().y ),
			static_cast<float>( -rig.ray.Dir().z ) );

		const Vector3 normal = GuidingCosineNormal( rig );
		pglVec3f( segment->normal,
			static_cast<float>( normal.x ),
			static_cast<float>( normal.y ),
			static_cast<float>( normal.z ) );

		pglVec3f( segment->directionIn, 0.0f, 0.0f, 0.0f );
		segment->volumeScatter = false;
		segment->pdfDirectionIn = 0.0f;
		segment->isDelta = false;
		pglVec3f( segment->scatteringWeight, 0.0f, 0.0f, 0.0f );
		pglVec3f( segment->transmittanceWeight, 1.0f, 1.0f, 1.0f );
		pglVec3f( segment->directContribution, 0.0f, 0.0f, 0.0f );
		segment->miWeight = 1.0f;
		pglVec3f( segment->scatteredContribution, 0.0f, 0.0f, 0.0f );
		segment->russianRouletteSurvivalProbability = 1.0f;
		segment->eta = 1.0f;
		segment->roughness = 1.0f;
		segment->regionPtr = 0;

		return segment;
	}

	static inline void SetPTGuidingDirectContribution(
		PGLPathSegmentData* segment,
		const RISEPel& contribution,
		const Scalar miWeight
		)
	{
		if( !segment ) {
			return;
		}

		SetPGLVec3FromRISEPel( segment->directContribution, contribution );
		segment->miWeight = static_cast<float>( miWeight );
	}

	static inline void AddPTGuidingScatteredContribution(
		PGLPathSegmentData* segment,
		const RISEPel& contribution
		)
	{
		if( !segment ) {
			return;
		}

		AddRISEPelToPGLVec3( segment->scatteredContribution, contribution );
	}

	static inline void SetPTGuidingContinuation(
		PGLPathSegmentData* segment,
		const Vector3& direction,
		const Scalar pdf,
		const RISEPel& scatteringWeight,
		const bool isDelta
		)
	{
		if( !segment ) {
			return;
		}

		pglVec3f( segment->directionIn,
			static_cast<float>( direction.x ),
			static_cast<float>( direction.y ),
			static_cast<float>( direction.z ) );
		segment->pdfDirectionIn = static_cast<float>( pdf );
		SetPGLVec3FromRISEPel( segment->scatteringWeight, scatteringWeight );
		segment->isDelta = isDelta;
		segment->roughness = isDelta ? 0.0f : 1.0f;
	}

	static inline void AddPTGuidingBackgroundSegment(
		PTGuidingPathRecorder& recorder,
		const Ray& ray,
		const RISEPel& radiance
		)
	{
		if( !recorder.active || !recorder.storage ) {
			return;
		}

		PGLPathSegmentData* segment = pglPathSegmentStorageNextSegment( recorder.storage );
		if( !segment ) {
			return;
		}

		const Point3 farPoint(
			ray.origin.x + ray.Dir().x * 1.0e6,
			ray.origin.y + ray.Dir().y * 1.0e6,
			ray.origin.z + ray.Dir().z * 1.0e6 );

		pglPoint3f( segment->position,
			static_cast<float>( farPoint.x ),
			static_cast<float>( farPoint.y ),
			static_cast<float>( farPoint.z ) );
		pglVec3f( segment->directionOut,
			static_cast<float>( -ray.Dir().x ),
			static_cast<float>( -ray.Dir().y ),
			static_cast<float>( -ray.Dir().z ) );
		pglVec3f( segment->normal, 0.0f, 0.0f, 1.0f );
		pglVec3f( segment->directionIn, 0.0f, 0.0f, 0.0f );
		segment->volumeScatter = false;
		segment->pdfDirectionIn = 0.0f;
		segment->isDelta = false;
		pglVec3f( segment->scatteringWeight, 0.0f, 0.0f, 0.0f );
		pglVec3f( segment->transmittanceWeight, 1.0f, 1.0f, 1.0f );
		SetPGLVec3FromRISEPel( segment->directContribution, radiance );
		segment->miWeight = 1.0f;
		pglVec3f( segment->scatteredContribution, 0.0f, 0.0f, 0.0f );
		segment->russianRouletteSurvivalProbability = 1.0f;
		segment->eta = 1.0f;
		segment->roughness = 1.0f;
		segment->regionPtr = 0;
	}
}
#endif

PathTracingShaderOp::PathTracingShaderOp(
	const bool branch,
	const ManifoldSolverConfig& smsConfig
	) :
  pSolver( 0 ),
  bBranch( branch ),
  bSMSEnabled( smsConfig.enabled )
{
	if( smsConfig.enabled )
	{
		pSolver = new ManifoldSolver( smsConfig );
		pSolver->addref();
	}
}

PathTracingShaderOp::~PathTracingShaderOp()
{
	safe_release( pSolver );
}

// Shared transport utilities — imported from PathTransportUtilities.h
using PathTransportUtilities::PowerHeuristic;
using PathTransportUtilities::ClampContribution;
using PathTransportUtilities::ClampContributionNM;

//////////////////////////////////////////////////////////////////////
// BSSRDFEntryBSDF — lightweight IBSDF adapter for BSSRDF entry
// point NEE.
//
// At a BSSRDF re-emission point, the effective "BSDF" for incoming
// light is the directional scattering function Sw:
//
//   Sw(wi) = Ft(cos_theta_i) / (c * PI)
//
// where c = (41 - 20*F0) / 42 and F0 = ((eta-1)/(eta+1))^2.
// This adapter wraps that computation so it can be passed to
// LightSampler::EvaluateDirectLighting().
//
// This is a stack-local transient object — reference counting is
// stubbed out.
//////////////////////////////////////////////////////////////////////
namespace
{
	// Stack-local adapter: must live in the same scope as entryRI.
	// IReference stubs are safe because EvaluateDirectLighting never
	// ref-counts its IBSDF argument — it only calls value()/valueNM().
	// entryRI is a reference to a stack-local RayIntersectionGeometric
	// in the caller; the adapter must not outlive that scope.
	class BSSRDFEntryBSDF : public RISE::IBSDF
	{
		ISubSurfaceDiffusionProfile* pProfile;
		const RayIntersectionGeometric& entryRI;
		Scalar swScale;  // 1 / (c * PI), pre-computed

	public:
		BSSRDFEntryBSDF(
			ISubSurfaceDiffusionProfile* profile,
			const RayIntersectionGeometric& ri,
			const Scalar eta
			) : pProfile( profile ), entryRI( ri )
		{
			const Scalar F0 = ((eta - 1.0) / (eta + 1.0)) * ((eta - 1.0) / (eta + 1.0));
			const Scalar c = (41.0 - 20.0 * F0) / 42.0;
			swScale = (c > 1e-20) ? 1.0 / (c * PI) : 0;
		}

		// IReference stubs — this object lives on the stack only
		void addref() const {}
		bool release() const { return false; }
		unsigned int refcount() const { return 1; }

		RISEPel value(
			const Vector3& vLightIn,
			const RayIntersectionGeometric& ri
			) const
		{
			const Scalar cosTheta = Vector3Ops::Dot( vLightIn, ri.vNormal );
			if( cosTheta <= 0 ) {
				return RISEPel( 0, 0, 0 );
			}
			const Scalar Ft = pProfile->FresnelTransmission( cosTheta, ri );
			const Scalar Sw = Ft * swScale;
			return RISEPel( Sw, Sw, Sw );
		}

		Scalar valueNM(
			const Vector3& vLightIn,
			const RayIntersectionGeometric& ri,
			const Scalar nm
			) const
		{
			const Scalar cosTheta = Vector3Ops::Dot( vLightIn, ri.vNormal );
			if( cosTheta <= 0 ) {
				return 0;
			}
			const Scalar Ft = pProfile->FresnelTransmission( cosTheta, ri );
			return Ft * swScale;
		}
	};

	//////////////////////////////////////////////////////////////////////
	// RandomWalkEntryBSDF — adapter BSDF for NEE at random-walk SSS
	// entry points.  Identical to BSSRDFEntryBSDF but uses Schlick
	// Fresnel with stored IOR instead of pProfile->FresnelTransmission().
	// Stack-local adapter: IReference stubs are safe because
	// EvaluateDirectLighting never ref-counts its IBSDF argument.
	//////////////////////////////////////////////////////////////////////
	class RandomWalkEntryBSDF : public RISE::IBSDF
	{
		Scalar swScale;  // 1 / (c * PI), pre-computed
		Scalar ior;

	public:
		RandomWalkEntryBSDF(
			const Scalar eta
			) : ior( eta )
		{
			const Scalar F0 = ((eta - 1.0) / (eta + 1.0)) * ((eta - 1.0) / (eta + 1.0));
			const Scalar c = (41.0 - 20.0 * F0) / 42.0;
			swScale = (c > 1e-20) ? 1.0 / (c * PI) : 0;
		}

		// IReference stubs — this object lives on the stack only
		void addref() const {}
		bool release() const { return false; }
		unsigned int refcount() const { return 1; }

		RISEPel value(
			const Vector3& vLightIn,
			const RayIntersectionGeometric& ri
			) const
		{
			const Scalar cosTheta = Vector3Ops::Dot( vLightIn, ri.vNormal );
			if( cosTheta <= 0 ) {
				return RISEPel( 0, 0, 0 );
			}
			// Schlick Fresnel transmission: Ft = 1 - F
			const Scalar F0 = ((ior - 1.0) / (ior + 1.0)) * ((ior - 1.0) / (ior + 1.0));
			const Scalar F = F0 + (1.0 - F0) * pow( 1.0 - cosTheta, 5.0 );
			const Scalar Ft = 1.0 - F;
			const Scalar Sw = Ft * swScale;
			return RISEPel( Sw, Sw, Sw );
		}

		Scalar valueNM(
			const Vector3& vLightIn,
			const RayIntersectionGeometric& ri,
			const Scalar nm
			) const
		{
			const Scalar cosTheta = Vector3Ops::Dot( vLightIn, ri.vNormal );
			if( cosTheta <= 0 ) {
				return 0;
			}
			const Scalar F0 = ((ior - 1.0) / (ior + 1.0)) * ((ior - 1.0) / (ior + 1.0));
			const Scalar F = F0 + (1.0 - F0) * pow( 1.0 - cosTheta, 5.0 );
			return (1.0 - F) * swScale;
		}
	};

	//////////////////////////////////////////////////////////////////////
	// BSSRDFEntryMaterial — lightweight IMaterial adapter for MIS at
	// BSSRDF entry points.  EvaluateDirectLighting uses pMaterial->Pdf()
	// to compute the BSDF sampling PDF for MIS.  At BSSRDF entry points
	// the sampling strategy is cosine-weighted hemisphere, so Pdf returns
	// cos(theta)/PI.  All other IMaterial methods return null — only
	// Pdf/PdfNM are meaningful here.
	//
	// Stack-local adapter: IReference stubs are safe because
	// EvaluateDirectLighting never ref-counts its IMaterial argument —
	// it only calls Pdf()/PdfNM().
	//////////////////////////////////////////////////////////////////////
	class BSSRDFEntryMaterial : public RISE::IMaterial
	{
	public:
		BSSRDFEntryMaterial() {}

		// IReference stubs — this object lives on the stack only
		void addref() const {}
		bool release() const { return false; }
		unsigned int refcount() const { return 1; }

		// IMaterial interface — only Pdf/PdfNM are meaningful
		IBSDF* GetBSDF() const { return 0; }
		ISPF* GetSPF() const { return 0; }
		IEmitter* GetEmitter() const { return 0; }

		Scalar Pdf(
			const Vector3& wo,
			const RayIntersectionGeometric& ri,
			const IORStack* ior_stack
			) const
		{
			const Scalar cosTheta = Vector3Ops::Dot( wo, ri.vNormal );
			return (cosTheta > 0) ? cosTheta * INV_PI : 0;
		}

		Scalar PdfNM(
			const Vector3& wo,
			const RayIntersectionGeometric& ri,
			const Scalar nm,
			const IORStack* ior_stack
			) const
		{
			return Pdf( wo, ri, ior_stack );
		}
	};
}

void PathTracingShaderOp::PerformOperation(
	const RuntimeContext& rc,
	const RayIntersection& ri,
	const IRayCaster& caster,
	const IRayCaster::RAY_STATE& rs,
	RISEPel& c,
	const IORStack* const ior_stack,
	const ScatteredRayContainer* pScat
	) const
{
	c = RISEPel( 0.0 );

	if( rc.pass != RuntimeContext::PASS_NORMAL && rs.type == rs.eRayView ) {
		return;
	}

	const IScene* pScene = caster.GetAttachedScene();
	if( !pScene ) return;

	const IBSDF* pBRDF = ri.pMaterial ? ri.pMaterial->GetBSDF() : 0;

	// Determine current participating medium for transmittance on NEE shadow rays.
	// Also get the enclosing object for shadow ray distance clipping.
	const IObject* pMediumObject = 0;
	const IMedium* pCurrentMedium = MediumTracking::GetCurrentMediumWithObject( ior_stack, pScene, pMediumObject );

#ifdef RISE_ENABLE_OPENPGL
	const bool useGuidingPathSegments = UsePTPathSegmentTraining( rc, bBranch );
	PTGuidingPathRecorder* guidingRecorder = useGuidingPathSegments ? &GetPTGuidingPathRecorder() : 0;
	const bool guidingRootRay = guidingRecorder && IsPTTrainingRootRay( rs );
	if( guidingRootRay ) {
		guidingRecorder->Begin();
	}
	PTGuidingPathScope guidingPathScope( guidingRecorder, rc.pGuidingField, guidingRootRay );
	PGLPathSegmentData* guidingSegment =
		(guidingRecorder && guidingRecorder->active) ?
			BeginPTGuidingSegment( *guidingRecorder, ri.geometric ) : 0;
#endif

	// ================================================================
	// PART 1: Emission
	// ================================================================
	// Check emission at ANY surface (including those with BSDFs like
	// lambertian_luminaire_material which has both a BSDF and emitter).
	{
		IEmitter* pEmitter = ri.pMaterial ? ri.pMaterial->GetEmitter() : 0;
		if( pEmitter && rs.considerEmission )
		{
			RISEPel emission = pEmitter->emittedRadiance(
				ri.geometric, -ri.geometric.ray.Dir(), ri.geometric.vNormal );
			const RISEPel rawEmission = emission;
			Scalar emissionMiWeight = 1.0;

			// MIS weight for BSDF-sampled emission hitting a mesh light.
			// The NEE strategy uses unified light selection with
			// probability pdfSelect, so the combined NEE PDF in solid
			// angle is:  p_nee = pdfSelect * dist^2 / (area * cosLight)
			if( rs.bsdfPdf > 0 && ri.pObject )
			{
				const Scalar area = ri.pObject->GetArea();
				if( area > 0 )
				{
					const Scalar cosLight = fabs( Vector3Ops::Dot(
						ri.geometric.ray.Dir(), ri.geometric.vNormal ) );
					if( cosLight > 0 )
					{
						const Scalar dist = Vector3Ops::Magnitude(
							Vector3Ops::mkVector3(
								ri.geometric.ptIntersection,
								ri.geometric.ray.origin ) );

						// MIS weight for BSDF-sampled emitter hit.
						// When RIS is active the exact finite-M technique
						// density is intractable, so MIS is disabled:
						// NEE uses w_nee=1 and the BSDF-hit emitter
						// contribution is suppressed here (w_bsdf=0).
						// Specular paths (bsdfPdf==0) never enter this
						// block and keep their full contribution.
						const LightSampler* pLS = caster.GetLightSampler();
						if( pLS && pLS->IsRISActive() )
						{
							// Suppress BSDF-hit emitter to avoid
							// double-counting with RIS NEE (w=1).
							emissionMiWeight = 0.0;
							emission = emission * 0.0;
						}
						else
						{
							Scalar pdfSelect = 1.0;
							if( pLS )
							{
								pdfSelect = pLS->CachedPdfSelectLuminary( *ri.pObject );
								if( pdfSelect <= 0 )
								{
									pdfSelect = 1.0;
								}
							}

							const Scalar p_nee = pdfSelect * (dist * dist) / (area * cosLight);
							const Scalar w_bsdf = PowerHeuristic( rs.bsdfPdf, p_nee );
							emissionMiWeight = w_bsdf;
							emission = emission * w_bsdf;
						}
					}
				}
			}

			// Clamp BSDF-sampled emitter hits at depth > 1.  At depth 1
			// the emitter is directly visible to the camera and should not
			// be clamped.  At deeper bounces this is a direct-lighting
			// contribution found via BSDF sampling (analogous to NEE).
			if( rc.pStabilityConfig && rs.depth > 1 ) {
				emission = ClampContribution( emission, rc.pStabilityConfig->directClamp );
			}

			c = c + emission;

#ifdef RISE_ENABLE_OPENPGL
			if( guidingSegment && GuidingWantsDeepSignalTraining( rs.depth ) &&
				ColorMath::MaxValue( rawEmission ) > 0 ) {
				SetPTGuidingDirectContribution( guidingSegment, rawEmission, emissionMiWeight );
			}
#endif
		}
	}

	// ================================================================
	// BSSRDF: Subsurface scattering via diffusion profile
	// ================================================================
	// When the ray hits a front-face of a material with a diffusion
	// profile, evaluate BOTH the subsurface and surface contributions
	// without stochastic Fresnel branching:
	//
	//   - Subsurface: importance-sample a nearby entry point via the
	//     disk projection method (Christensen & Burley 2015).  The
	//     BSSRDF weight already includes Ft(exit), so no selection
	//     probability compensation is needed.
	//
	//   - Surface reflection: handled by PART 2 (NEE) and PART 3
	//     (continuation) using the material's BSDF/SPF, which already
	//     include the Fresnel reflectance R in their weights.
	//
	// Energy conservation: R + Ft = 1, and each path is weighted by
	// its respective Fresnel factor, so the sum is correct without
	// any branching or compensation.  This avoids the high variance
	// from 1/R compensation (~59x for IOR 1.3) and eliminates wasted
	// samples when BSSRDF probe rays miss the object.
	{
		ISubSurfaceDiffusionProfile* pProfile =
			ri.pMaterial ? ri.pMaterial->GetDiffusionProfile() : 0;

		if( pProfile && pBRDF )
		{
			const Scalar cosIn = Vector3Ops::Dot( ri.geometric.vNormal,
				Vector3Ops::Normalize( -ri.geometric.ray.Dir() ) );

			// Only front-face hits can enter the subsurface
			if( cosIn > NEARZERO )
			{
				const Scalar Ft = pProfile->FresnelTransmission( cosIn, ri.geometric );

				if( Ft > NEARZERO )
				{
					IndependentSampler fallbackSampler( rc.random );
					ISampler& bssrdfSampler = rc.pSampler ? *rc.pSampler : fallbackSampler;

					BSSRDFSampling::SampleResult bssrdf = BSSRDFSampling::SampleEntryPoint(
						ri.geometric, ri.pObject, ri.pMaterial, bssrdfSampler, 0 );

					if( bssrdf.valid )
					{
						// Full BSSRDF weight (for continuation): includes Sw
						// for the cosine-sampled direction and Ft(exit).
						const RISEPel bssrdfWeight = bssrdf.weight;

						// Spatial-only weight (for NEE): Rd * Ft(exit) / pdfSurface.
						// NEE evaluates Sw independently via BSSRDFEntryBSDF.
						const RISEPel bssrdfWeightSpatial = bssrdf.weightSpatial;

						// Build a synthetic intersection at the entry point
						RayIntersectionGeometric entryRI(
							Ray( bssrdf.entryPoint, bssrdf.scatteredRay.Dir() ),
							ri.geometric.rast );
						entryRI.bHit = true;
						entryRI.ptIntersection = bssrdf.entryPoint;
						entryRI.vNormal = bssrdf.entryNormal;
						entryRI.onb = bssrdf.entryONB;

						const Scalar eta = pProfile->GetIOR( ri.geometric );
						BSSRDFEntryBSDF entryBSDF( pProfile, entryRI, eta );
						BSSRDFEntryMaterial entryMaterial;

						// Per-type bounce limit: skip SSS contribution if
						// exceeded, but still allow surface reflection in
						// PART 2 + 3.
						const unsigned int nextTranslucentBounces = rs.translucentBounces + 1;
						const bool skipSSS = rc.pStabilityConfig &&
							nextTranslucentBounces > rc.pStabilityConfig->maxTranslucentBounce;

						if( !skipSSS )
						{
							const LightSampler* pLS = caster.GetLightSampler();
							if( pLS )
							{
								RISEPel directSSS = pLS->EvaluateDirectLighting(
									entryRI, entryBSDF, &entryMaterial, caster, bssrdfSampler, ri.pObject, 0, false, 0 );
								RISEPel sssDirectContrib = bssrdfWeightSpatial * directSSS;
								if( rc.pStabilityConfig ) {
									sssDirectContrib = ClampContribution( sssDirectContrib, rc.pStabilityConfig->directClamp );
								}
								c = c + sssDirectContrib;
							}

							// Continuation from the entry point
							IRayCaster::RAY_STATE rs2;
							rs2.depth = rs.depth + 1;
							rs2.considerEmission = true;
							rs2.importance = rs.importance * ColorMath::MaxValue( bssrdfWeight );
							rs2.bsdfPdf = bssrdf.cosinePdf;
							rs2.type = IRayCaster::RAY_STATE::eRayDiffuse;
							rs2.diffuseBounces      = rs.diffuseBounces;
							rs2.glossyBounces       = rs.glossyBounces;
							rs2.transmissionBounces = rs.transmissionBounces;
							rs2.translucentBounces  = nextTranslucentBounces;
							rs2.glossyFilterWidth   = rs.glossyFilterWidth;

							// Russian roulette on subsurface continuation
							RISEPel throughput = bssrdfWeight;
							bool skipContinuation = false;
							{
								const unsigned int rrMinDepth = rc.pStabilityConfig ?
									rc.pStabilityConfig->rrMinDepth : PT_RR_MIN_DEPTH;
								const Scalar rrThreshold = rc.pStabilityConfig ?
									rc.pStabilityConfig->rrThreshold : PT_RR_THRESHOLD;
								const PathTransportUtilities::RussianRouletteResult rr =
									PathTransportUtilities::EvaluateRussianRoulette(
										rs.depth, rrMinDepth, rrThreshold,
										rs.importance * ColorMath::MaxValue( throughput ),
										rs.importance, bssrdfSampler.Get1D() );
								if( rr.terminate ) {
									skipContinuation = true;
								} else if( rr.survivalProb < 1.0 ) {
									throughput = throughput * (1.0 / rr.survivalProb);
								}
							}

							if( !skipContinuation )
							{
								RISEPel cthis( 0, 0, 0 );
								Ray continuationRay = bssrdf.scatteredRay;
								caster.CastRay( rc, ri.geometric.rast,
									continuationRay, cthis, rs2, 0,
									ri.pRadianceMap, ior_stack );

								RISEPel indirect = throughput * cthis;
								if( rc.pStabilityConfig && rs.depth > 1 ) {
									indirect = ClampContribution( indirect,
										rc.pStabilityConfig->indirectClamp );
								}
								c = c + indirect;
							}
						}
					}
					// Continue to PART 2 + 3 for surface reflection
				}
			}
		}
	}

	// ================================================================
	// Random-walk subsurface scattering (Chiang & Burley, SIGGRAPH 2016)
	// ================================================================
	// Same structure as disk-projection BSSRDF above: evaluate both
	// subsurface and surface contributions without Fresnel branching.
	// The random walk replaces the disk-projection sampling with a
	// volumetric walk inside the mesh geometry.
	{
		const RandomWalkSSSParams* pRWParams =
			ri.pMaterial ? ri.pMaterial->GetRandomWalkSSSParams() : 0;

		if( pRWParams && pBRDF )
		{
			const Scalar cosIn = Vector3Ops::Dot( ri.geometric.vNormal,
				Vector3Ops::Normalize( -ri.geometric.ray.Dir() ) );

			// Only front-face hits can enter the subsurface
			if( cosIn > NEARZERO )
			{
				// Schlick Fresnel transmission at entry
				const Scalar F0 = ((pRWParams->ior - 1.0) / (pRWParams->ior + 1.0)) *
					((pRWParams->ior - 1.0) / (pRWParams->ior + 1.0));
				const Scalar F = F0 + (1.0 - F0) * pow( 1.0 - cosIn, 5.0 );
				const Scalar Ft = 1.0 - F;

				if( Ft > NEARZERO )
				{
					// The random walk consumes a variable number of
					// sampler dimensions (up to ~7 per scatter event
					// × maxBounces).  Samplers with a fixed dimension
					// budget (SobolSampler, kStreamStride=32) cannot
					// tolerate this — the walk bleeds into dimensions
					// reserved for subsequent operations, creating
					// persistent per-pixel biases.  Use IndependentSampler
					// for those.  Samplers without a budget (PSSMLT,
					// IndependentSampler) are used directly so the walk
					// stays in the primary sample vector.
					IndependentSampler walkSampler( rc.random );
					IndependentSampler fallbackSampler( rc.random );
					ISampler& bssrdfSampler = rc.pSampler ? *rc.pSampler : fallbackSampler;
					ISampler& rwSampler = bssrdfSampler.HasFixedDimensionBudget()
						? static_cast<ISampler&>(walkSampler) : bssrdfSampler;

					BSSRDFSampling::SampleResult bssrdf = RandomWalkSSS::SampleExit(
						ri.geometric, ri.pObject,
						pRWParams->sigma_a, pRWParams->sigma_s, pRWParams->sigma_t,
						pRWParams->g, pRWParams->ior, pRWParams->maxBounces,
						rwSampler, 0 );

					if( bssrdf.valid )
					{
						// SampleExit does not include the entry Fresnel
						// transmission factor — the caller owns it.  In
						// the PT path (no Fresnel coin flip) we scale by
						// Ft so that SSS + surface reflection sum correctly:
						//   Ft * SSS + R * Reflection = 1.
						const RISEPel bssrdfWeight = bssrdf.weight * Ft;
						const RISEPel bssrdfWeightSpatial = bssrdf.weightSpatial * Ft;

						// Build a synthetic intersection at the entry point
						RayIntersectionGeometric entryRI(
							Ray( bssrdf.entryPoint, bssrdf.scatteredRay.Dir() ),
							ri.geometric.rast );
						entryRI.bHit = true;
						entryRI.ptIntersection = bssrdf.entryPoint;
						entryRI.vNormal = bssrdf.entryNormal;
						entryRI.onb = bssrdf.entryONB;

						RandomWalkEntryBSDF entryBSDF( pRWParams->ior );
						BSSRDFEntryMaterial entryMaterial;

						const unsigned int nextTranslucentBounces = rs.translucentBounces + 1;
						const bool skipSSS = rc.pStabilityConfig &&
							nextTranslucentBounces > rc.pStabilityConfig->maxTranslucentBounce;

						if( !skipSSS )
						{
							const LightSampler* pLS = caster.GetLightSampler();
							if( pLS )
							{
								RISEPel directSSS = pLS->EvaluateDirectLighting(
									entryRI, entryBSDF, &entryMaterial, caster, bssrdfSampler, ri.pObject, 0, false, 0 );
								RISEPel sssDirectContrib = bssrdfWeightSpatial * directSSS;
								if( rc.pStabilityConfig ) {
									sssDirectContrib = ClampContribution( sssDirectContrib, rc.pStabilityConfig->directClamp );
								}
								c = c + sssDirectContrib;
							}

							// Continuation from the entry point
							IRayCaster::RAY_STATE rs2;
							rs2.depth = rs.depth + 1;
							rs2.considerEmission = true;
							rs2.importance = rs.importance * ColorMath::MaxValue( bssrdfWeight );
							rs2.bsdfPdf = bssrdf.cosinePdf;
							rs2.type = IRayCaster::RAY_STATE::eRayDiffuse;
							rs2.diffuseBounces      = rs.diffuseBounces;
							rs2.glossyBounces       = rs.glossyBounces;
							rs2.transmissionBounces = rs.transmissionBounces;
							rs2.translucentBounces  = nextTranslucentBounces;
							rs2.glossyFilterWidth   = rs.glossyFilterWidth;

							// Russian roulette on subsurface continuation
							RISEPel throughput = bssrdfWeight;
							bool skipContinuation = false;
							{
								const unsigned int rrMinDepth = rc.pStabilityConfig ?
									rc.pStabilityConfig->rrMinDepth : PT_RR_MIN_DEPTH;
								const Scalar rrThreshold = rc.pStabilityConfig ?
									rc.pStabilityConfig->rrThreshold : PT_RR_THRESHOLD;
								const PathTransportUtilities::RussianRouletteResult rr =
									PathTransportUtilities::EvaluateRussianRoulette(
										rs.depth, rrMinDepth, rrThreshold,
										rs.importance * ColorMath::MaxValue( throughput ),
										rs.importance, bssrdfSampler.Get1D() );
								if( rr.terminate ) {
									skipContinuation = true;
								} else if( rr.survivalProb < 1.0 ) {
									throughput = throughput * (1.0 / rr.survivalProb);
								}
							}

							if( !skipContinuation )
							{
								RISEPel cthis( 0, 0, 0 );
								Ray continuationRay = bssrdf.scatteredRay;
								caster.CastRay( rc, ri.geometric.rast,
									continuationRay, cthis, rs2, 0,
									ri.pRadianceMap, ior_stack );

								RISEPel indirect = throughput * cthis;
								if( rc.pStabilityConfig && rs.depth > 1 ) {
									indirect = ClampContribution( indirect,
										rc.pStabilityConfig->indirectClamp );
								}
								c = c + indirect;
							}
						}
					}
					// Continue to PART 2 + 3 for surface reflection
				}
			}
		}
	}

	// Specular surfaces (no BSDF) — trace scattered rays and return
	if( !pBRDF ) {
		// Still need to trace scattered rays for specular surfaces
		if( pScat ) {
			const ScatteredRayContainer& scattered = *pScat;
			IRayCaster::RAY_STATE rs2;
			rs2.depth = rs.depth + 1;
			// Through specular surfaces, keep emission enabled.
			// The double-counting prevention happens at DIFFUSE surfaces
			// (Part 3) where SMS is the alternative to BSDF-sampled
			// emission through glass.  At specular surfaces we must
			// propagate emission visibility so that non-mesh lights
			// (spot, point) can illuminate surfaces seen through glass.
			rs2.considerEmission = true;

			if( bBranch ) {
				for( unsigned int i = 0; i < scattered.Count(); i++ ) {
					ScatteredRay& scat = scattered[i];
					const Scalar scatmaxv = ColorMath::MaxValue( scat.kray );
					if( scatmaxv > 0 ) {
						rs2.importance = rs.importance * scatmaxv;
						rs2.bsdfPdf = scat.isDelta ? 0 : scat.pdf;
						rs2.type = PathTracingRayType( scat );
						if( PropagateBounceLimits( rs, rs2, scat, rc.pStabilityConfig ) ) {
							continue;
						}
						RISEPel cthis( 0, 0, 0 );
						Ray ray = scat.ray;
						ray.Advance( 1e-8 );
						caster.CastRay( rc, ri.geometric.rast, ray, cthis,
							rs2, 0, ri.pRadianceMap,
							scat.ior_stack ? scat.ior_stack : ior_stack );
						{
							RISEPel indirect = scat.kray * cthis;
							if( rc.pStabilityConfig && rs.depth > 1 ) {
								indirect = ClampContribution( indirect, rc.pStabilityConfig->indirectClamp );
							}
							c = c + indirect;
						}
					}
				}
			} else {
				// When multiple rays are present (e.g. translucent materials),
				// trace all of them to avoid high-variance one-sample correction.
				// Single-ray materials (most cases) take the fast path below.
				if( scattered.Count() > 1 ) {
					for( unsigned int i = 0; i < scattered.Count(); i++ ) {
						ScatteredRay& scat = scattered[i];
						const Scalar scatmaxv = ColorMath::MaxValue( scat.kray );
						if( scatmaxv > 0 ) {
							rs2.importance = rs.importance * scatmaxv;
							rs2.bsdfPdf = scat.isDelta ? 0 : scat.pdf;
							rs2.type = PathTracingRayType( scat );
							if( PropagateBounceLimits( rs, rs2, scat, rc.pStabilityConfig ) ) {
								continue;
							}
							RISEPel cthis( 0, 0, 0 );
							Ray ray = scat.ray;
							ray.Advance( 1e-8 );
							caster.CastRay( rc, ri.geometric.rast, ray, cthis,
								rs2, 0, ri.pRadianceMap,
								scat.ior_stack ? scat.ior_stack : ior_stack );
							{
								RISEPel indirect = scat.kray * cthis;
								if( rc.pStabilityConfig && rs.depth > 1 ) {
									indirect = ClampContribution( indirect, rc.pStabilityConfig->indirectClamp );
								}
								c = c + indirect;
							}
						}
					}
				} else {
					const Scalar xi = rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom();
					const ScatteredRay* pS = scattered.RandomlySelect( xi, false );
					if( pS ) {
						rs2.importance = rs.importance * ColorMath::MaxValue( pS->kray );
						rs2.bsdfPdf = pS->isDelta ? 0 : pS->pdf;
						rs2.type = PathTracingRayType( *pS );
						if( PropagateBounceLimits( rs, rs2, *pS, rc.pStabilityConfig ) ) {
							return;
						}
						RISEPel cthis( 0, 0, 0 );
						Ray ray = pS->ray;

#ifdef RISE_ENABLE_OPENPGL
						if( guidingSegment ) {
							SetPTGuidingContinuation(
								guidingSegment,
								ray.Dir(),
								pS->pdf,
								pS->kray,
								pS->isDelta );
						}
#endif

						ray.Advance( 1e-8 );
						const bool hit = caster.CastRay( rc, ri.geometric.rast, ray, cthis,
							rs2, 0, ri.pRadianceMap,
							pS->ior_stack ? pS->ior_stack : ior_stack );
						{
							RISEPel indirect = pS->kray * cthis;
							if( rc.pStabilityConfig && rs.depth > 1 ) {
								indirect = ClampContribution( indirect, rc.pStabilityConfig->indirectClamp );
							}
							c = c + indirect;
						}

#ifdef RISE_ENABLE_OPENPGL
						if( guidingRecorder && guidingRecorder->active && !hit &&
							GuidingTrainingLuminance( cthis ) > 0 )
						{
							AddPTGuidingBackgroundSegment( *guidingRecorder, ray, cthis );
						}
#endif
					}
				}
			}
		}
		return;
	}

	// ================================================================
	// PART 2: NEE + SMS at diffuse/glossy surfaces
	// ================================================================

	// --- 2a+2b: Unified NEE for all light sources ---
	// Select one light (non-mesh or mesh) proportional to exitance
	// via the unified LightSampler.  Delta lights (point/spot) get
	// no MIS; area lights (mesh) get power-heuristic MIS against
	// the BSDF sampling PDF.  Lights with zero exitance (ambient,
	// directional) are evaluated deterministically.
	{
		const LightSampler* pLS = caster.GetLightSampler();
		if( pLS )
		{
			IndependentSampler fallbackSampler( rc.random );
			ISampler& neeSampler = rc.pSampler ? *rc.pSampler : fallbackSampler;

			RISEPel directAll = pLS->EvaluateDirectLighting(
				ri.geometric, *pBRDF, ri.pMaterial, caster, neeSampler, ri.pObject, pCurrentMedium, false, pMediumObject );
			if( rc.pStabilityConfig ) {
				directAll = ClampContribution( directAll, rc.pStabilityConfig->directClamp );
			}
			c = c + directAll;

#ifdef RISE_ENABLE_OPENPGL
			if( GuidingWantsDeepSignalTraining( rs.depth ) ) {
				AddPTGuidingScatteredContribution( guidingSegment, directAll );
			}
#endif
		}
	}

	// --- 2c: SMS for caustics through specular surfaces ---
	// Attempt a single SMS evaluation using a randomly sampled light.
	// EvaluateAtShadingPoint handles: light sampling, seed chain,
	// Newton solve, BSDF evaluation, geometric terms.
	if( pSolver )
	{
		const Vector3 woOutgoing = Vector3(
			-ri.geometric.ray.Dir().x,
			-ri.geometric.ray.Dir().y,
			-ri.geometric.ray.Dir().z );

		IndependentSampler fallbackSampler( rc.random );
		ISampler& smsSampler = rc.pSampler ? *rc.pSampler : fallbackSampler;

		ManifoldSolver::SMSContribution sms = pSolver->EvaluateAtShadingPoint(
			ri.geometric.ptIntersection,
			ri.geometric.vNormal,
			ri.geometric.onb,
			ri.pMaterial,
			woOutgoing,
			*pScene,
			caster,
			smsSampler );

		if( sms.valid )
		{
			RISEPel smsContrib = sms.contribution * sms.misWeight;
			if( rc.pStabilityConfig ) {
				smsContrib = ClampContribution( smsContrib, rc.pStabilityConfig->directClamp );
			}
			c = c + smsContrib;

#ifdef RISE_ENABLE_OPENPGL
			if( GuidingWantsDeepSignalTraining( rs.depth ) ) {
				AddPTGuidingScatteredContribution(
					guidingSegment,
					sms.contribution * sms.misWeight );
			}
#endif
		}
	}

	// ================================================================
	// PART 3: BSDF sampling (continue path)
	// ================================================================
	if( pScat )
	{
		const ScatteredRayContainer& scattered = *pScat;

		IRayCaster::RAY_STATE rs2;
		rs2.depth = rs.depth + 1;
		// For BSDF-sampled rays at diffuse surfaces, enable emission
		// so that direct light hits (non-specular paths) are counted
		// with MIS weight.  The MIS weight is computed in PART 1 above
		// when the next hit is an emitter.
		rs2.considerEmission = true;

		if( bBranch )
		{
			for( unsigned int i = 0; i < scattered.Count(); i++ )
			{
				ScatteredRay& scat = scattered[i];
				const Scalar scatmaxv = ColorMath::MaxValue( scat.kray );
				if( scatmaxv > 0 )
				{
					rs2.importance = rs.importance * scatmaxv;
					rs2.bsdfPdf = scat.isDelta ? 0 : scat.pdf;

					// Per-type bounce limits
					if( PropagateBounceLimits( rs, rs2, scat, rc.pStabilityConfig ) ) {
						continue;
					}

					// For specular bounces, disable emission to
					// prevent double-counting with SMS
					if( scat.isDelta && bSMSEnabled ) {
						rs2.considerEmission = false;
					} else {
						rs2.considerEmission = true;
					}

					RISEPel cthis( 0, 0, 0 );
					Ray ray = scat.ray;
					ray.Advance( 1e-8 );
					Scalar hitDistBranch = 0;
					caster.CastRay( rc, ri.geometric.rast, ray, cthis,
						rs2, &hitDistBranch, ri.pRadianceMap,
						scat.ior_stack ? scat.ior_stack : ior_stack );
					{
						RISEPel indirect = scat.kray * cthis;
						if( rc.pStabilityConfig && rs.depth > 1 ) {
							indirect = ClampContribution( indirect, rc.pStabilityConfig->indirectClamp );
						}
						c = c + indirect;
					}

#ifdef RISE_ENABLE_OPENPGL
					// Collect training samples in branching mode
					if( rc.pGuidingField && rc.pGuidingField->IsCollectingTrainingSamples() &&
						GuidingSupportsSurfaceSampling( scat ) && scat.pdf > NEARZERO )
					{
						const Scalar lum = GuidingTrainingLuminance( cthis );
						if( lum > 0 )
						{
							rc.pGuidingField->AddSample(
								ri.geometric.ptIntersection,
								ray.Dir(),
								hitDistBranch > 0 ? hitDistBranch : 1.0,
								scat.pdf,
								lum,
								false );
						}
						else
						{
							rc.pGuidingField->AddZeroValueSample(
								ri.geometric.ptIntersection,
								ray.Dir() );
						}
					}
#endif
				}
			}
		}
		else if( scattered.Count() > 1 )
		{
			// Multiple scattered rays (e.g. translucent or polished materials):
			// trace all to avoid high-variance one-sample correction.  Each
			// sub-path continues in non-branching mode at subsequent bounces.
			for( unsigned int i = 0; i < scattered.Count(); i++ )
			{
				ScatteredRay& scat = scattered[i];
				const Scalar scatmaxv = ColorMath::MaxValue( scat.kray );
				if( scatmaxv > 0 )
				{
					rs2.importance = rs.importance * scatmaxv;
					rs2.bsdfPdf = scat.isDelta ? 0 : scat.pdf;
					rs2.type = PathTracingRayType( scat );

					if( PropagateBounceLimits( rs, rs2, scat, rc.pStabilityConfig ) ) {
						continue;
					}

					if( scat.isDelta && bSMSEnabled ) {
						rs2.considerEmission = false;
					} else {
						rs2.considerEmission = true;
					}

					RISEPel cthis( 0, 0, 0 );
					Ray ray = scat.ray;
					ray.Advance( 1e-8 );
					caster.CastRay( rc, ri.geometric.rast, ray, cthis,
						rs2, 0, ri.pRadianceMap,
						scat.ior_stack ? scat.ior_stack : ior_stack );
					{
						RISEPel indirect = scat.kray * cthis;
						if( rc.pStabilityConfig && rs.depth > 1 ) {
							indirect = ClampContribution( indirect, rc.pStabilityConfig->indirectClamp );
						}
						c = c + indirect;
					}
				}
			}
		}
		else
		{
			const Scalar xi = rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom();
			const ScatteredRay* pS = scattered.RandomlySelect( xi, false );
			if( pS )
			{
				Ray traceRay = pS->ray;
				RISEPel throughput = pS->kray;
				Scalar effectiveBsdfPdf = pS->isDelta ? 0 : pS->pdf;
				const IORStack* traceIorStack = pS->ior_stack ? pS->ior_stack : ior_stack;

#ifdef RISE_ENABLE_OPENPGL
				// --------------------------------------------------------
				// Path guiding: one-sample MIS for non-delta surfaces.
				// Blends the BSDF-sampled direction with a direction from
				// the learned incident radiance distribution.
				//
				// One-sample MIS: with probability alpha, sample from the
				// guiding distribution; with probability (1-alpha), keep
				// the BSDF direction.  In either case divide by the
				// combined PDF = alpha*guidePdf + (1-alpha)*bsdfPdf.
				// --------------------------------------------------------
				static thread_local GuidingDistributionHandle guideDist;

				if( rc.pGuidingField && rc.pGuidingField->IsTrained() &&
					rs.depth <= rc.maxGuidingDepth && GuidingSupportsSurfaceSampling( *pS ) )
				{
					const Scalar alpha = GuidingEffectiveAlpha(
						rc.guidingAlpha, scattered, ri.pMaterial, rs, false );

					if( alpha > NEARZERO && rc.pGuidingField->InitDistribution( guideDist,
						ri.geometric.ptIntersection,
						rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom() ) )
					{
						if( pS->type == ScatteredRay::eRayDiffuse ) {
							rc.pGuidingField->ApplyCosineProduct( guideDist, GuidingCosineNormal( ri.geometric ) );
						}

						if( rc.guidingSamplingType == eGuidingRIS )
						{
							// ------------------------------------------------
							// RIS-based guiding: draw two candidates (one from
							// the BSDF, one from the guiding distribution),
							// evaluate a target function at each, and select
							// one proportional to the RIS weight.  This
							// produces lower variance than one-sample MIS.
							// ------------------------------------------------
							const ISPF* pSPF = ri.pMaterial ? ri.pMaterial->GetSPF() : 0;

							PathTransportUtilities::GuidingRISCandidate candidates[2];

							// Candidate 0: BSDF sample (already drawn)
							{
								PathTransportUtilities::GuidingRISCandidate& c = candidates[0];
								c.direction = pS->ray.Dir();
								c.bsdfEval = PathVertexEval::EvalBSDFAtSurface(
									pBRDF, c.direction, ri.geometric );
								c.bsdfPdf = pS->pdf;
								c.guidePdf = rc.pGuidingField->Pdf( guideDist, c.direction );
								c.incomingRadPdf = rc.pGuidingField->IncomingRadiancePdf( guideDist, c.direction );
								c.cosTheta = fabs(
									Vector3Ops::Dot( c.direction, ri.geometric.vNormal ) );
								const Scalar avgBsdf = ColorMath::MaxValue( c.bsdfEval );
								c.risTarget = PathTransportUtilities::GuidingRISTarget(
									avgBsdf, c.cosTheta, c.incomingRadPdf, alpha );
								c.risPdf = PathTransportUtilities::GuidingRISProposalPdf(
									c.bsdfPdf, c.guidePdf );
								c.risWeight = c.risPdf > NEARZERO ? c.risTarget / c.risPdf : 0;
								c.valid = c.bsdfPdf > NEARZERO && c.risPdf > NEARZERO && avgBsdf > 0;
								if( !c.valid ) {
									c.risWeight = 0;
								}
							}

							// Candidate 1: guide sample
							{
								PathTransportUtilities::GuidingRISCandidate& c = candidates[1];
								Scalar guidePdf = 0;
								const Point2 xi2d(
									rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom(),
									rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom() );
								c.direction = rc.pGuidingField->Sample( guideDist, xi2d, guidePdf );
								c.guidePdf = guidePdf;

								if( guidePdf > NEARZERO )
								{
									c.bsdfEval = PathVertexEval::EvalBSDFAtSurface(
										pBRDF, c.direction, ri.geometric );
									c.bsdfPdf = PathVertexEval::EvalPdfAtSurface(
										pSPF, ri.geometric, c.direction, ior_stack );
									c.incomingRadPdf = rc.pGuidingField->IncomingRadiancePdf( guideDist, c.direction );
									c.cosTheta = fabs(
										Vector3Ops::Dot( c.direction, ri.geometric.vNormal ) );
									const Scalar avgBsdf = ColorMath::MaxValue( c.bsdfEval );
									c.risTarget = PathTransportUtilities::GuidingRISTarget(
										avgBsdf, c.cosTheta, c.incomingRadPdf, alpha );
									c.risPdf = PathTransportUtilities::GuidingRISProposalPdf(
										c.bsdfPdf, c.guidePdf );
									c.risWeight = c.risPdf > NEARZERO ? c.risTarget / c.risPdf : 0;
									c.valid = c.bsdfPdf > NEARZERO && avgBsdf > 0;
									if( !c.valid ) {
										c.risWeight = 0;
									}
								}
								else
								{
									c.bsdfEval = RISEPel( 0, 0, 0 );
									c.bsdfPdf = 0;
									c.incomingRadPdf = 0;
									c.cosTheta = 0;
									c.risTarget = 0;
									c.risPdf = 0;
									c.risWeight = 0;
									c.valid = false;
								}
							}

							// Select one candidate proportional to RIS weights
							Scalar risEffectivePdf = 0;
							const Scalar xiRIS = rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom();
							const unsigned int sel = PathTransportUtilities::GuidingRISSelectCandidate(
								candidates, 2, xiRIS, risEffectivePdf );

							if( risEffectivePdf > NEARZERO && candidates[sel].valid )
							{
								throughput = candidates[sel].bsdfEval *
									(candidates[sel].cosTheta / risEffectivePdf);
								traceRay = Ray( pS->ray.origin, candidates[sel].direction );
								effectiveBsdfPdf = risEffectivePdf;
								traceIorStack = ior_stack;
							}
							else
							{
								throughput = RISEPel( 0, 0, 0 );
							}
						}
						else
						{
							// ------------------------------------------------
							// One-sample MIS (original path guiding strategy)
							// ------------------------------------------------
							const Scalar xiG = rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom();

							if( PathTransportUtilities::ShouldUseGuidedSample( alpha, xiG ) )
							{
								// Sample from the guiding distribution.
								Scalar guidePdf = 0;
								const Point2 xi2d(
									rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom(),
									rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom() );
								const Vector3 guidedDir = rc.pGuidingField->Sample( guideDist, xi2d, guidePdf );

								if( guidePdf > NEARZERO )
								{
									const RISEPel fGuided = PathVertexEval::EvalBSDFAtSurface(
										pBRDF, guidedDir, ri.geometric );
									const ISPF* pSPF = ri.pMaterial ? ri.pMaterial->GetSPF() : 0;
									const Scalar bsdfPdfGuided = PathVertexEval::EvalPdfAtSurface(
										pSPF, ri.geometric, guidedDir, ior_stack );
									const Scalar combinedPdf =
										PathTransportUtilities::GuidingCombinedPdf( alpha, guidePdf, bsdfPdfGuided );

									if( combinedPdf > NEARZERO )
									{
										const Scalar cosTheta = fabs(
											Vector3Ops::Dot( guidedDir, ri.geometric.vNormal ) );
										throughput = fGuided * (cosTheta / combinedPdf);
										traceRay = Ray( pS->ray.origin, guidedDir );
										effectiveBsdfPdf = combinedPdf;
										traceIorStack = ior_stack;
									}
									else
									{
										throughput = RISEPel( 0, 0, 0 );
									}
								}
								else
								{
									throughput = RISEPel( 0, 0, 0 );
								}
							}
							else
							{
								// Keep BSDF direction but adjust throughput for
								// the combined PDF.
								const Scalar guidePdfForBsdf = rc.pGuidingField->Pdf( guideDist, pS->ray.Dir() );
								const Scalar combinedPdf =
									PathTransportUtilities::GuidingCombinedPdf( alpha, guidePdfForBsdf, pS->pdf );

								if( combinedPdf > NEARZERO )
								{
									throughput = pS->kray * (pS->pdf / combinedPdf);
									effectiveBsdfPdf = combinedPdf;
								}
							}
						}
					}
				}

				// Training sample collection: record incident radiance
				// samples for the guiding field during training passes.
				const bool collectTrainingSample =
					!useGuidingPathSegments &&
					rc.pGuidingField && rc.pGuidingField->IsCollectingTrainingSamples() &&
					GuidingSupportsSurfaceSampling( *pS ) && effectiveBsdfPdf > NEARZERO;
#endif // RISE_ENABLE_OPENPGL

				bool skipContinuation = ColorMath::MaxValue( throughput ) <= NEARZERO;

				// Russian roulette: probabilistic path termination with
				// throughput compensation to maintain unbiasedness.
				// Branching paths are excluded (they explore all lobes by design).
				if( !skipContinuation )
				{
					const unsigned int rrMinDepth = rc.pStabilityConfig ? rc.pStabilityConfig->rrMinDepth : PT_RR_MIN_DEPTH;
					const Scalar rrThreshold = rc.pStabilityConfig ? rc.pStabilityConfig->rrThreshold : PT_RR_THRESHOLD;
					const PathTransportUtilities::RussianRouletteResult rr =
						PathTransportUtilities::EvaluateRussianRoulette(
							rs.depth, rrMinDepth, rrThreshold,
							rs.importance * ColorMath::MaxValue( throughput ),
							rs.importance,
							rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom() );
					if( rr.terminate ) {
						skipContinuation = true;
					} else if( rr.survivalProb < 1.0 ) {
						throughput = throughput * (1.0 / rr.survivalProb);
					}
				}

				rs2.importance = rs.importance * ColorMath::MaxValue( throughput );
				rs2.bsdfPdf = effectiveBsdfPdf;
				rs2.type = PathTracingRayType( *pS );

				// Per-type bounce limits: propagate counters and
				// skip continuation if the limit for this scatter
				// type has been exceeded.
				if( PropagateBounceLimits( rs, rs2, *pS, rc.pStabilityConfig ) ) {
					skipContinuation = true;
				}

				if( pS->isDelta && bSMSEnabled ) {
					rs2.considerEmission = false;
				} else {
					rs2.considerEmission = true;
				}

				RISEPel cthis( 0, 0, 0 );
				Scalar hitDist = 0;
				bool hit = false;

#ifdef RISE_ENABLE_OPENPGL
				if( guidingSegment && !skipContinuation )
				{
					SetPTGuidingContinuation(
						guidingSegment,
						traceRay.Dir(),
						effectiveBsdfPdf,
						throughput,
						pS->isDelta );
				}
#endif

				if( !skipContinuation )
				{
					traceRay.Advance( 1e-8 );
					hit = caster.CastRay( rc, ri.geometric.rast, traceRay, cthis,
						rs2, &hitDist, ri.pRadianceMap,
						traceIorStack );
					RISEPel indirect = throughput * cthis;
					if( rc.pStabilityConfig && rs.depth > 1 ) {
						indirect = ClampContribution( indirect, rc.pStabilityConfig->indirectClamp );
					}
					c = c + indirect;
				}

#ifdef RISE_ENABLE_OPENPGL
				if( guidingRecorder && guidingRecorder->active && !hit && !skipContinuation &&
					GuidingWantsDeepSignalTraining( rs2.depth ) &&
					GuidingTrainingLuminance( cthis ) > 0 )
				{
					AddPTGuidingBackgroundSegment( *guidingRecorder, traceRay, cthis );
				}
#endif

#ifdef RISE_ENABLE_OPENPGL
				if( collectTrainingSample )
				{
					const Scalar lum = GuidingTrainingLuminance( cthis );
					if( lum > 0 )
					{
						rc.pGuidingField->AddSample(
							ri.geometric.ptIntersection,
							traceRay.Dir(),
							hitDist > 0 ? hitDist : 1.0,
							effectiveBsdfPdf,
							lum,
							false );
					}
					else
					{
						rc.pGuidingField->AddZeroValueSample(
							ri.geometric.ptIntersection,
							traceRay.Dir() );
					}
				}
#endif
			}
		}
	}
}

Scalar PathTracingShaderOp::PerformOperationNM(
	const RuntimeContext& rc,
	const RayIntersection& ri,
	const IRayCaster& caster,
	const IRayCaster::RAY_STATE& rs,
	const Scalar caccum,
	const Scalar nm,
	const IORStack* const ior_stack,
	const ScatteredRayContainer* pScat
	) const
{
	Scalar c = 0;

	if( rc.pass != RuntimeContext::PASS_NORMAL && rs.type == rs.eRayView ) {
		return 0;
	}

	const IScene* pScene = caster.GetAttachedScene();
	if( !pScene ) return 0;

	const IBSDF* pBRDF = ri.pMaterial ? ri.pMaterial->GetBSDF() : 0;

	// Determine current participating medium for transmittance on NEE shadow rays.
	// Also get the enclosing object for shadow ray distance clipping.
	const IObject* pMediumObject = 0;
	const IMedium* pCurrentMedium = MediumTracking::GetCurrentMediumWithObject( ior_stack, pScene, pMediumObject );

	// ================================================================
	// PART 1: Emission (spectral)
	// ================================================================
	{
		IEmitter* pEmitter = ri.pMaterial ? ri.pMaterial->GetEmitter() : 0;
		if( pEmitter && rs.considerEmission )
		{
			Scalar emission = pEmitter->emittedRadianceNM(
				ri.geometric, -ri.geometric.ray.Dir(), ri.geometric.vNormal, nm );

			if( rs.bsdfPdf > 0 && ri.pObject )
			{
				const Scalar area = ri.pObject->GetArea();
				if( area > 0 )
				{
					const Scalar cosLight = fabs( Vector3Ops::Dot(
						ri.geometric.ray.Dir(), ri.geometric.vNormal ) );
					if( cosLight > 0 )
					{
						const Scalar dist = Vector3Ops::Magnitude(
							Vector3Ops::mkVector3(
								ri.geometric.ptIntersection,
								ri.geometric.ray.origin ) );

						// MIS weight for BSDF-sampled emitter hit (spectral).
						// See RGB variant above for full commentary.
						const LightSampler* pLS = caster.GetLightSampler();
						if( pLS && pLS->IsRISActive() )
						{
							// Suppress BSDF-hit emitter to avoid
							// double-counting with RIS NEE (w=1).
							emission = emission * 0.0;
						}
						else
						{
							Scalar pdfSelect = 1.0;
							if( pLS )
							{
								pdfSelect = pLS->CachedPdfSelectLuminary( *ri.pObject );
								if( pdfSelect <= 0 )
								{
									pdfSelect = 1.0;
								}
							}

							const Scalar p_nee = pdfSelect * (dist * dist) / (area * cosLight);
							const Scalar w_bsdf = PowerHeuristic( rs.bsdfPdf, p_nee );
							emission = emission * w_bsdf;
						}
					}
				}
			}

			// Clamp BSDF-sampled emitter hits at depth > 1 (spectral).
			if( rc.pStabilityConfig && rs.depth > 1 ) {
				emission = ClampContributionNM( emission, rc.pStabilityConfig->directClamp );
			}

			c += emission;
		}
	}

	// ================================================================
	// BSSRDF: Subsurface scattering via diffusion profile (spectral)
	// ================================================================
	// Same approach as the RGB path: evaluate both subsurface and
	// surface contributions without Fresnel branching.
	{
		ISubSurfaceDiffusionProfile* pProfile =
			ri.pMaterial ? ri.pMaterial->GetDiffusionProfile() : 0;

		if( pProfile && pBRDF )
		{
			const Scalar cosIn = Vector3Ops::Dot( ri.geometric.vNormal,
				Vector3Ops::Normalize( -ri.geometric.ray.Dir() ) );

			if( cosIn > NEARZERO )
			{
				const Scalar Ft = pProfile->FresnelTransmission( cosIn, ri.geometric );

				if( Ft > NEARZERO )
				{
					IndependentSampler fallbackSampler( rc.random );
					ISampler& bssrdfSampler = rc.pSampler ? *rc.pSampler : fallbackSampler;

					BSSRDFSampling::SampleResult bssrdf = BSSRDFSampling::SampleEntryPoint(
						ri.geometric, ri.pObject, ri.pMaterial, bssrdfSampler, nm );

					if( bssrdf.valid )
					{
						const Scalar bssrdfWeightNM = bssrdf.weightNM;
						const Scalar bssrdfWeightSpatialNM = bssrdf.weightSpatialNM;

						RayIntersectionGeometric entryRI(
							Ray( bssrdf.entryPoint, bssrdf.scatteredRay.Dir() ),
							ri.geometric.rast );
						entryRI.bHit = true;
						entryRI.ptIntersection = bssrdf.entryPoint;
						entryRI.vNormal = bssrdf.entryNormal;
						entryRI.onb = bssrdf.entryONB;

						const Scalar eta = pProfile->GetIOR( ri.geometric );
						BSSRDFEntryBSDF entryBSDF( pProfile, entryRI, eta );
						BSSRDFEntryMaterial entryMaterial;

						const unsigned int nextTranslucentBounces = rs.translucentBounces + 1;
						const bool skipSSS = rc.pStabilityConfig &&
							nextTranslucentBounces > rc.pStabilityConfig->maxTranslucentBounce;

						if( !skipSSS )
						{
							const LightSampler* pLS = caster.GetLightSampler();
							if( pLS )
							{
								Scalar directSSSNM = pLS->EvaluateDirectLightingNM(
									entryRI, entryBSDF, &entryMaterial, nm, caster, bssrdfSampler, ri.pObject, 0, false, 0 );
								Scalar sssDirectContribNM = bssrdfWeightSpatialNM * directSSSNM;
								if( rc.pStabilityConfig ) {
									sssDirectContribNM = ClampContributionNM( sssDirectContribNM, rc.pStabilityConfig->directClamp );
								}
								c += sssDirectContribNM;
							}

							IRayCaster::RAY_STATE rs2;
							rs2.depth = rs.depth + 1;
							rs2.considerEmission = true;
							rs2.importance = rs.importance * fabs( bssrdfWeightNM );
							rs2.bsdfPdf = bssrdf.cosinePdf;
							rs2.type = IRayCaster::RAY_STATE::eRayDiffuse;
							rs2.diffuseBounces      = rs.diffuseBounces;
							rs2.glossyBounces       = rs.glossyBounces;
							rs2.transmissionBounces = rs.transmissionBounces;
							rs2.translucentBounces  = nextTranslucentBounces;
							rs2.glossyFilterWidth   = rs.glossyFilterWidth;

							Scalar throughputNM = bssrdfWeightNM;
							bool skipContinuation = false;
							{
								const unsigned int rrMinDepth = rc.pStabilityConfig ?
									rc.pStabilityConfig->rrMinDepth : PT_RR_MIN_DEPTH;
								const Scalar rrThreshold = rc.pStabilityConfig ?
									rc.pStabilityConfig->rrThreshold : PT_RR_THRESHOLD;
								const PathTransportUtilities::RussianRouletteResult rr =
									PathTransportUtilities::EvaluateRussianRoulette(
										rs.depth, rrMinDepth, rrThreshold,
										rs.importance * fabs( throughputNM ),
										rs.importance, bssrdfSampler.Get1D() );
								if( rr.terminate ) {
									skipContinuation = true;
								} else if( rr.survivalProb < 1.0 ) {
									throughputNM /= rr.survivalProb;
								}
							}

							if( !skipContinuation )
							{
								Scalar cthis = 0;
								Ray continuationRay = bssrdf.scatteredRay;
								caster.CastRayNM( rc, ri.geometric.rast,
									continuationRay, cthis, rs2, nm, 0,
									ri.pRadianceMap, ior_stack );

								Scalar indirectNM = cthis * throughputNM;
								if( rc.pStabilityConfig && rs.depth > 1 ) {
									indirectNM = ClampContributionNM( indirectNM,
										rc.pStabilityConfig->indirectClamp );
								}
								c += indirectNM;
							}
						}
					}
					// Continue to surface reflection below
				}
			}
		}
	}

	// ================================================================
	// Random-walk subsurface scattering (spectral / NM path)
	// ================================================================
	{
		const RandomWalkSSSParams* pRWParams =
			ri.pMaterial ? ri.pMaterial->GetRandomWalkSSSParams() : 0;

		if( pRWParams && pBRDF )
		{
			const Scalar cosIn = Vector3Ops::Dot( ri.geometric.vNormal,
				Vector3Ops::Normalize( -ri.geometric.ray.Dir() ) );

			if( cosIn > NEARZERO )
			{
				const Scalar F0 = ((pRWParams->ior - 1.0) / (pRWParams->ior + 1.0)) *
					((pRWParams->ior - 1.0) / (pRWParams->ior + 1.0));
				const Scalar F = F0 + (1.0 - F0) * pow( 1.0 - cosIn, 5.0 );
				const Scalar Ft = 1.0 - F;

				if( Ft > NEARZERO )
				{
					// See RGB path comment for rationale.
					IndependentSampler walkSampler( rc.random );
					IndependentSampler fallbackSampler( rc.random );
					ISampler& bssrdfSampler = rc.pSampler ? *rc.pSampler : fallbackSampler;
					ISampler& rwSampler = bssrdfSampler.HasFixedDimensionBudget()
						? static_cast<ISampler&>(walkSampler) : bssrdfSampler;

					BSSRDFSampling::SampleResult bssrdf = RandomWalkSSS::SampleExit(
						ri.geometric, ri.pObject,
						pRWParams->sigma_a, pRWParams->sigma_s, pRWParams->sigma_t,
						pRWParams->g, pRWParams->ior, pRWParams->maxBounces,
						rwSampler, nm );

					if( bssrdf.valid )
					{
						// Scale by entry Fresnel (see RGB block comment)
						const Scalar bssrdfWeightNM = bssrdf.weightNM * Ft;
						const Scalar bssrdfWeightSpatialNM = bssrdf.weightSpatialNM * Ft;

						RayIntersectionGeometric entryRI(
							Ray( bssrdf.entryPoint, bssrdf.scatteredRay.Dir() ),
							ri.geometric.rast );
						entryRI.bHit = true;
						entryRI.ptIntersection = bssrdf.entryPoint;
						entryRI.vNormal = bssrdf.entryNormal;
						entryRI.onb = bssrdf.entryONB;

						RandomWalkEntryBSDF entryBSDF( pRWParams->ior );
						BSSRDFEntryMaterial entryMaterial;

						const unsigned int nextTranslucentBounces = rs.translucentBounces + 1;
						const bool skipSSS = rc.pStabilityConfig &&
							nextTranslucentBounces > rc.pStabilityConfig->maxTranslucentBounce;

						if( !skipSSS )
						{
							const LightSampler* pLS = caster.GetLightSampler();
							if( pLS )
							{
								Scalar directSSSNM = pLS->EvaluateDirectLightingNM(
									entryRI, entryBSDF, &entryMaterial, nm, caster, bssrdfSampler, ri.pObject, 0, false, 0 );
								Scalar sssDirectContribNM = bssrdfWeightSpatialNM * directSSSNM;
								if( rc.pStabilityConfig ) {
									sssDirectContribNM = ClampContributionNM( sssDirectContribNM, rc.pStabilityConfig->directClamp );
								}
								c += sssDirectContribNM;
							}

							IRayCaster::RAY_STATE rs2;
							rs2.depth = rs.depth + 1;
							rs2.considerEmission = true;
							rs2.importance = rs.importance * fabs( bssrdfWeightNM );
							rs2.bsdfPdf = bssrdf.cosinePdf;
							rs2.type = IRayCaster::RAY_STATE::eRayDiffuse;
							rs2.diffuseBounces      = rs.diffuseBounces;
							rs2.glossyBounces       = rs.glossyBounces;
							rs2.transmissionBounces = rs.transmissionBounces;
							rs2.translucentBounces  = nextTranslucentBounces;
							rs2.glossyFilterWidth   = rs.glossyFilterWidth;

							Scalar throughputNM = bssrdfWeightNM;
							bool skipContinuation = false;
							{
								const unsigned int rrMinDepth = rc.pStabilityConfig ?
									rc.pStabilityConfig->rrMinDepth : PT_RR_MIN_DEPTH;
								const Scalar rrThreshold = rc.pStabilityConfig ?
									rc.pStabilityConfig->rrThreshold : PT_RR_THRESHOLD;
								const PathTransportUtilities::RussianRouletteResult rr =
									PathTransportUtilities::EvaluateRussianRoulette(
										rs.depth, rrMinDepth, rrThreshold,
										rs.importance * fabs( throughputNM ),
										rs.importance, bssrdfSampler.Get1D() );
								if( rr.terminate ) {
									skipContinuation = true;
								} else if( rr.survivalProb < 1.0 ) {
									throughputNM /= rr.survivalProb;
								}
							}

							if( !skipContinuation )
							{
								Scalar cthis = 0;
								Ray continuationRay = bssrdf.scatteredRay;
								caster.CastRayNM( rc, ri.geometric.rast,
									continuationRay, cthis, rs2, nm, 0,
									ri.pRadianceMap, ior_stack );

								Scalar indirectNM = cthis * throughputNM;
								if( rc.pStabilityConfig && rs.depth > 1 ) {
									indirectNM = ClampContributionNM( indirectNM,
										rc.pStabilityConfig->indirectClamp );
								}
								c += indirectNM;
							}
						}
					}
					// Continue to surface reflection below
				}
			}
		}
	}

	// Specular surfaces — trace scattered rays
	if( !pBRDF ) {
		if( pScat ) {
			const ScatteredRayContainer& scattered = *pScat;
			IRayCaster::RAY_STATE rs2;
			rs2.depth = rs.depth + 1;
			rs2.considerEmission = true;

			if( bBranch ) {
				for( unsigned int i = 0; i < scattered.Count(); i++ ) {
					ScatteredRay& scat = scattered[i];
					if( scat.krayNM > 0 ) {
						rs2.importance = rs.importance * scat.krayNM;
						rs2.bsdfPdf = scat.isDelta ? 0 : scat.pdf;
						if( PropagateBounceLimits( rs, rs2, scat, rc.pStabilityConfig ) ) {
							continue;
						}
						Scalar cthis = 0;
						Ray ray = scat.ray;
						ray.Advance( 1e-8 );
						caster.CastRayNM( rc, ri.geometric.rast, ray, cthis,
							rs2, nm, 0, ri.pRadianceMap,
							scat.ior_stack ? scat.ior_stack : ior_stack );
						{
							Scalar indirectNM = cthis * scat.krayNM;
							if( rc.pStabilityConfig && rs.depth > 1 ) {
								indirectNM = ClampContributionNM( indirectNM, rc.pStabilityConfig->indirectClamp );
							}
							c += indirectNM;
						}
					}
				}
			} else {
				if( scattered.Count() > 1 ) {
					for( unsigned int i = 0; i < scattered.Count(); i++ ) {
						ScatteredRay& scat = scattered[i];
						if( scat.krayNM > 0 ) {
							rs2.importance = rs.importance * scat.krayNM;
							rs2.bsdfPdf = scat.isDelta ? 0 : scat.pdf;
							if( PropagateBounceLimits( rs, rs2, scat, rc.pStabilityConfig ) ) {
								continue;
							}
							Scalar cthis = 0;
							Ray ray = scat.ray;
							ray.Advance( 1e-8 );
							caster.CastRayNM( rc, ri.geometric.rast, ray, cthis,
								rs2, nm, 0, ri.pRadianceMap,
								scat.ior_stack ? scat.ior_stack : ior_stack );
							{
								Scalar indirectNM = cthis * scat.krayNM;
								if( rc.pStabilityConfig && rs.depth > 1 ) {
									indirectNM = ClampContributionNM( indirectNM, rc.pStabilityConfig->indirectClamp );
								}
								c += indirectNM;
							}
						}
					}
				} else {
					const Scalar xi = rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom();
					const ScatteredRay* pS = scattered.RandomlySelect( xi, true );
					if( pS ) {
						rs2.importance = rs.importance * pS->krayNM;
						rs2.bsdfPdf = pS->isDelta ? 0 : pS->pdf;
						if( PropagateBounceLimits( rs, rs2, *pS, rc.pStabilityConfig ) ) {
							return c;
						}
						Scalar cthis = 0;
						Ray ray = pS->ray;
						ray.Advance( 1e-8 );
						caster.CastRayNM( rc, ri.geometric.rast, ray, cthis,
							rs2, nm, 0, ri.pRadianceMap,
							pS->ior_stack ? pS->ior_stack : ior_stack );
						{
							Scalar indirectNM = cthis * pS->krayNM;
							if( rc.pStabilityConfig && rs.depth > 1 ) {
								indirectNM = ClampContributionNM( indirectNM, rc.pStabilityConfig->indirectClamp );
							}
							c += indirectNM;
						}
					}
				}
			}
		}
		return c;
	}

	// ================================================================
	// PART 2: NEE (spectral) — unified light sampler
	// ================================================================
	{
		const LightSampler* pLS = caster.GetLightSampler();
		if( pLS )
		{
			IndependentSampler fallbackSamplerNM( rc.random );
			ISampler& neeSamplerNM = rc.pSampler ? *rc.pSampler : fallbackSamplerNM;

			Scalar directAllNM = pLS->EvaluateDirectLightingNM(
				ri.geometric, *pBRDF, ri.pMaterial, nm, caster, neeSamplerNM, ri.pObject, pCurrentMedium, false, pMediumObject );
			if( rc.pStabilityConfig ) {
				directAllNM = ClampContributionNM( directAllNM, rc.pStabilityConfig->directClamp );
			}
			c += directAllNM;
		}
	}

	// 2c: SMS (spectral — per-wavelength IOR for dispersion)
	if( pSolver && rs.depth <= 1 )
	{
		const Vector3 woOutgoing = Vector3(
			-ri.geometric.ray.Dir().x,
			-ri.geometric.ray.Dir().y,
			-ri.geometric.ray.Dir().z );

		IndependentSampler fallbackSamplerNM( rc.random );
		ISampler& smsSamplerNM = rc.pSampler ? *rc.pSampler : fallbackSamplerNM;

		ManifoldSolver::SMSContributionNM sms = pSolver->EvaluateAtShadingPointNM(
			ri.geometric.ptIntersection,
			ri.geometric.vNormal,
			ri.geometric.onb,
			ri.pMaterial,
			woOutgoing,
			*pScene,
			caster,
			smsSamplerNM,
			nm );

		if( sms.valid )
		{
			Scalar smsContribNM = sms.contribution * sms.misWeight;
			if( rc.pStabilityConfig ) {
				smsContribNM = ClampContributionNM( smsContribNM, rc.pStabilityConfig->directClamp );
			}
			c += smsContribNM;
		}
	}

	// ================================================================
	// PART 3: BSDF sampling (spectral)
	// ================================================================
	if( pScat )
	{
		const ScatteredRayContainer& scattered = *pScat;

		IRayCaster::RAY_STATE rs2;
		rs2.depth = rs.depth + 1;
		rs2.considerEmission = true;

		if( bBranch )
		{
				for( unsigned int i = 0; i < scattered.Count(); i++ )
				{
					ScatteredRay& scat = scattered[i];
					if( scat.krayNM > 0 )
					{
						rs2.importance = rs.importance * scat.krayNM;
						rs2.bsdfPdf = scat.isDelta ? 0 : scat.pdf;
						rs2.type = PathTracingRayType( scat );

					if( PropagateBounceLimits( rs, rs2, scat, rc.pStabilityConfig ) ) {
						continue;
					}

					if( scat.isDelta && bSMSEnabled ) {
						rs2.considerEmission = false;
					} else {
						rs2.considerEmission = true;
					}

					Scalar cthis = 0;
					Ray ray = scat.ray;
					ray.Advance( 1e-8 );
					caster.CastRayNM( rc, ri.geometric.rast, ray, cthis,
						rs2, nm, 0, ri.pRadianceMap,
						scat.ior_stack ? scat.ior_stack : ior_stack );
					{
						Scalar indirectNM = cthis * scat.krayNM;
						if( rc.pStabilityConfig && rs.depth > 1 ) {
							indirectNM = ClampContributionNM( indirectNM, rc.pStabilityConfig->indirectClamp );
						}
						c += indirectNM;
					}
				}
			}
		}
		else if( scattered.Count() > 1 )
		{
			for( unsigned int i = 0; i < scattered.Count(); i++ )
			{
				ScatteredRay& scat = scattered[i];
				if( scat.krayNM > 0 )
				{
					rs2.importance = rs.importance * scat.krayNM;
					rs2.bsdfPdf = scat.isDelta ? 0 : scat.pdf;
					rs2.type = PathTracingRayType( scat );

					if( PropagateBounceLimits( rs, rs2, scat, rc.pStabilityConfig ) ) {
						continue;
					}

					if( scat.isDelta && bSMSEnabled ) {
						rs2.considerEmission = false;
					} else {
						rs2.considerEmission = true;
					}

					Scalar cthis = 0;
					Ray ray = scat.ray;
					ray.Advance( 1e-8 );
					caster.CastRayNM( rc, ri.geometric.rast, ray, cthis,
						rs2, nm, 0, ri.pRadianceMap,
						scat.ior_stack ? scat.ior_stack : ior_stack );
					{
						Scalar indirectNM = cthis * scat.krayNM;
						if( rc.pStabilityConfig && rs.depth > 1 ) {
							indirectNM = ClampContributionNM( indirectNM, rc.pStabilityConfig->indirectClamp );
						}
						c += indirectNM;
					}
				}
			}
		}
		else
		{
			const Scalar xi = rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom();
			const ScatteredRay* pS = scattered.RandomlySelect( xi, true );
			if( pS )
			{
				Ray traceRay = pS->ray;
				Scalar throughputNM = pS->krayNM;
				Scalar effectiveBsdfPdf = pS->isDelta ? 0 : pS->pdf;
				const IORStack* traceIorStack = pS->ior_stack ? pS->ior_stack : ior_stack;

#ifdef RISE_ENABLE_OPENPGL
				// Path guiding (spectral): same one-sample MIS logic
				static thread_local GuidingDistributionHandle guideDistNM;

				if( rc.pGuidingField && rc.pGuidingField->IsTrained() &&
					rs.depth <= rc.maxGuidingDepth && GuidingSupportsSurfaceSampling( *pS ) )
				{
					const Scalar alpha = GuidingEffectiveAlpha(
						rc.guidingAlpha, scattered, ri.pMaterial, rs, true );

					if( alpha > NEARZERO && rc.pGuidingField->InitDistribution( guideDistNM,
						ri.geometric.ptIntersection,
						rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom() ) )
					{
						if( pS->type == ScatteredRay::eRayDiffuse ) {
							rc.pGuidingField->ApplyCosineProduct( guideDistNM, GuidingCosineNormal( ri.geometric ) );
						}

						if( rc.guidingSamplingType == eGuidingRIS )
						{
							// RIS-based guiding (spectral)
							const ISPF* pSPF = ri.pMaterial ? ri.pMaterial->GetSPF() : 0;

							PathTransportUtilities::GuidingRISCandidateNM candidates[2];

							// Candidate 0: BSDF sample
							{
								PathTransportUtilities::GuidingRISCandidateNM& c = candidates[0];
								c.direction = pS->ray.Dir();
								c.bsdfEvalNM = PathVertexEval::EvalBSDFAtSurfaceNM(
									pBRDF, c.direction, ri.geometric, nm );
								c.bsdfPdf = pS->pdf;
								c.guidePdf = rc.pGuidingField->Pdf( guideDistNM, c.direction );
								c.incomingRadPdf = rc.pGuidingField->IncomingRadiancePdf( guideDistNM, c.direction );
								c.cosTheta = fabs(
									Vector3Ops::Dot( c.direction, ri.geometric.vNormal ) );
								const Scalar avgBsdf = fabs( c.bsdfEvalNM );
								c.risTarget = PathTransportUtilities::GuidingRISTarget(
									avgBsdf, c.cosTheta, c.incomingRadPdf, alpha );
								c.risPdf = PathTransportUtilities::GuidingRISProposalPdf(
									c.bsdfPdf, c.guidePdf );
								c.risWeight = c.risPdf > NEARZERO ? c.risTarget / c.risPdf : 0;
								c.valid = c.bsdfPdf > NEARZERO && c.risPdf > NEARZERO && avgBsdf > 0;
								if( !c.valid ) {
									c.risWeight = 0;
								}
							}

							// Candidate 1: guide sample
							{
								PathTransportUtilities::GuidingRISCandidateNM& c = candidates[1];
								Scalar guidePdf = 0;
								const Point2 xi2d(
									rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom(),
									rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom() );
								c.direction = rc.pGuidingField->Sample( guideDistNM, xi2d, guidePdf );
								c.guidePdf = guidePdf;

								if( guidePdf > NEARZERO )
								{
									c.bsdfEvalNM = PathVertexEval::EvalBSDFAtSurfaceNM(
										pBRDF, c.direction, ri.geometric, nm );
									c.bsdfPdf = PathVertexEval::EvalPdfAtSurfaceNM(
										pSPF, ri.geometric, c.direction, nm, ior_stack );
									c.incomingRadPdf = rc.pGuidingField->IncomingRadiancePdf( guideDistNM, c.direction );
									c.cosTheta = fabs(
										Vector3Ops::Dot( c.direction, ri.geometric.vNormal ) );
									const Scalar avgBsdf = fabs( c.bsdfEvalNM );
									c.risTarget = PathTransportUtilities::GuidingRISTarget(
										avgBsdf, c.cosTheta, c.incomingRadPdf, alpha );
									c.risPdf = PathTransportUtilities::GuidingRISProposalPdf(
										c.bsdfPdf, c.guidePdf );
									c.risWeight = c.risPdf > NEARZERO ? c.risTarget / c.risPdf : 0;
									c.valid = c.bsdfPdf > NEARZERO && avgBsdf > 0;
									if( !c.valid ) {
										c.risWeight = 0;
									}
								}
								else
								{
									c.bsdfEvalNM = 0;
									c.bsdfPdf = 0;
									c.incomingRadPdf = 0;
									c.cosTheta = 0;
									c.risTarget = 0;
									c.risPdf = 0;
									c.risWeight = 0;
									c.valid = false;
								}
							}

							Scalar risEffectivePdf = 0;
							const Scalar xiRIS = rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom();
							const unsigned int sel = PathTransportUtilities::GuidingRISSelectCandidateNM(
								candidates, 2, xiRIS, risEffectivePdf );

							if( risEffectivePdf > NEARZERO && candidates[sel].valid )
							{
								throughputNM = candidates[sel].bsdfEvalNM *
									candidates[sel].cosTheta / risEffectivePdf;
								traceRay = Ray( pS->ray.origin, candidates[sel].direction );
								effectiveBsdfPdf = risEffectivePdf;
								traceIorStack = ior_stack;
							}
							else
							{
								throughputNM = 0;
							}
						}
						else
						{
							// One-sample MIS (spectral)
							const Scalar xiG = rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom();

							if( PathTransportUtilities::ShouldUseGuidedSample( alpha, xiG ) )
							{
								Scalar guidePdf = 0;
								const Point2 xi2d(
									rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom(),
									rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom() );
								const Vector3 guidedDir = rc.pGuidingField->Sample( guideDistNM, xi2d, guidePdf );

								if( guidePdf > NEARZERO )
								{
									const Scalar fGuided = PathVertexEval::EvalBSDFAtSurfaceNM(
										pBRDF, guidedDir, ri.geometric, nm );
									const ISPF* pSPF = ri.pMaterial ? ri.pMaterial->GetSPF() : 0;
									const Scalar bsdfPdfGuided = PathVertexEval::EvalPdfAtSurfaceNM(
										pSPF, ri.geometric, guidedDir, nm, ior_stack );
									const Scalar combinedPdf =
										PathTransportUtilities::GuidingCombinedPdf( alpha, guidePdf, bsdfPdfGuided );

									if( combinedPdf > NEARZERO )
									{
										const Scalar cosTheta = fabs(
											Vector3Ops::Dot( guidedDir, ri.geometric.vNormal ) );
										throughputNM = fGuided * cosTheta / combinedPdf;
										traceRay = Ray( pS->ray.origin, guidedDir );
										effectiveBsdfPdf = combinedPdf;
										traceIorStack = ior_stack;
									}
									else
									{
										throughputNM = 0;
									}
								}
								else
								{
									throughputNM = 0;
								}
							}
							else
							{
								const Scalar guidePdfForBsdf = rc.pGuidingField->Pdf( guideDistNM, pS->ray.Dir() );
								const Scalar combinedPdf =
									PathTransportUtilities::GuidingCombinedPdf( alpha, guidePdfForBsdf, pS->pdf );

								if( combinedPdf > NEARZERO )
								{
									throughputNM = pS->krayNM * (pS->pdf / combinedPdf);
									effectiveBsdfPdf = combinedPdf;
								}
							}
						}
					}
				}

				const bool collectTrainingSampleNM =
					rc.pGuidingField && rc.pGuidingField->IsCollectingTrainingSamples() &&
					GuidingSupportsSurfaceSampling( *pS ) && effectiveBsdfPdf > NEARZERO;
#endif

				// Russian roulette (spectral path)
				bool skipContinuationNM = fabs( throughputNM ) <= NEARZERO;
				if( !skipContinuationNM )
				{
					const unsigned int rrMinDepth = rc.pStabilityConfig ? rc.pStabilityConfig->rrMinDepth : PT_RR_MIN_DEPTH;
					const Scalar rrThreshold = rc.pStabilityConfig ? rc.pStabilityConfig->rrThreshold : PT_RR_THRESHOLD;
					const PathTransportUtilities::RussianRouletteResult rr =
						PathTransportUtilities::EvaluateRussianRoulette(
							rs.depth, rrMinDepth, rrThreshold,
							rs.importance * fabs( throughputNM ),
							rs.importance,
							rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom() );
					if( rr.terminate ) {
						skipContinuationNM = true;
					} else if( rr.survivalProb < 1.0 ) {
						throughputNM /= rr.survivalProb;
					}
				}

				rs2.importance = rs.importance * fabs( throughputNM );
				rs2.bsdfPdf = effectiveBsdfPdf;
				rs2.type = PathTracingRayType( *pS );

				// Per-type bounce limits
				if( PropagateBounceLimits( rs, rs2, *pS, rc.pStabilityConfig ) ) {
					skipContinuationNM = true;
				}

				if( pS->isDelta && bSMSEnabled ) {
					rs2.considerEmission = false;
				} else {
					rs2.considerEmission = true;
				}

				Scalar cthis = 0;
				Scalar hitDist = 0;
				if( !skipContinuationNM )
				{
					traceRay.Advance( 1e-8 );
					caster.CastRayNM( rc, ri.geometric.rast, traceRay, cthis,
						rs2, nm, &hitDist, ri.pRadianceMap,
						traceIorStack );
				}
				{
					Scalar indirectNM = cthis * throughputNM;
					if( rc.pStabilityConfig && rs.depth > 1 ) {
						indirectNM = ClampContributionNM( indirectNM, rc.pStabilityConfig->indirectClamp );
					}
					c += indirectNM;
				}

#ifdef RISE_ENABLE_OPENPGL
				if( collectTrainingSampleNM )
				{
					if( fabs( cthis ) > 0 )
					{
						rc.pGuidingField->AddSample(
							ri.geometric.ptIntersection,
							traceRay.Dir(),
							hitDist > 0 ? hitDist : 1.0,
							effectiveBsdfPdf,
							fabs( cthis ),
							false );
					}
					else
					{
						rc.pGuidingField->AddZeroValueSample(
							ri.geometric.ptIntersection,
							traceRay.Dir() );
					}
				}
#endif
			}
		}
	}

	return c;
}
