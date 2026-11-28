//////////////////////////////////////////////////////////////////////
//
//  ManifoldSolver.h - Specular Manifold Sampling (SMS) Newton solver
//  (Zeltner et al. 2020; Hanika et al. 2015; Fan et al. 2023).
//
//  Solves the 2D specular angle-difference constraint C_i = 0 along an
//  arbitrary chain of delta reflection/refraction surfaces via a
//  block-tridiagonal Newton iteration, using material IOR from
//  IMaterial::GetSpecularInfo().
//
//  For delta materials SMS and BDPT occupy disjoint path spaces (BDPT
//  skips delta vertices in MIS), so contributions add without cross-MIS.
//  See docs/SMS.md for full analysis and glossy-extension notes.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MANIFOLD_SOLVER_
#define MANIFOLD_SOLVER_

// Allows regression harnesses to invoke the shipped legacy entry points
// when this implementation is replaced by committed master sources.
#define RISE_SMS_DOMAIN_REPLAY 1
#define RISE_SMS_SCENE_POLICY 1
#define RISE_SMS_REFERENCE_A 1
#define RISE_SMS_SCRATCH_COUNTERS 1
#define RISE_SMS_NATIVE_EVENT_NORMALS 1
#define RISE_SMS_FRESNEL_FALLBACK_DOMAIN 1
#define RISE_SMS_EXTENDED_PARTITION 1

#include "../Interfaces/IReference.h"
#include "../Interfaces/IGeometry.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IObject.h"
#include "../Utilities/Reference.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/ISampler.h"
#include "../Utilities/IORStack.h"
#include <vector>
#include <memory>
#include <atomic>
#include "../Intersection/RayIntersection.h"

// <wingdi.h> (pulled in by <windows.h>) defines RGB(r,g,b) as a function-like
// macro, which would mangle SMSQueryDomain::RGB.  Nothing in the library uses
// the GDI macro; drop it where this name is declared and called.
#ifdef RGB
#undef RGB
#endif

namespace RISE
{
	class IScene;
	class IRayCaster;
	class IBSDF;

	namespace Implementation
	{
        // Unbiased reciprocal-probability tail for alpha seed proposals.
        // After B failures, survival to term n is (B/(n+1))^2. Expected
        // trial count is bounded by about 2B even as p approaches zero;
        // for every p>0 the exponential failure tail gives finite variance.
        class SMSReciprocalTail {
            Scalar budget, failures = 0, estimate = 1;
        public:
            explicit SMSReciprocalTail(unsigned int b) : budget(b ? b : 1024) {}
            bool ContinueAfterFailure(ISampler& sampler) {
                failures += 1;
                Scalar weight = 1;
                if (failures >= budget) {
                    const Scalar ratio = failures / (failures + 1);
                    if (sampler.GetAlpha1D() >= ratio * ratio) return false;
                    const Scalar inverse = (failures + 1) / budget;
                    weight = inverse * inverse;
                }
                estimate += weight;
                return true;
            }
            Scalar Estimate() const { return estimate; }
        };

        // Extended-domain records are separate from legacy vertex metadata.
        struct SMSDomainCounters {
            std::atomic<unsigned long long> attempts{0}, newtonIterations{0};
            std::atomic<unsigned long long> acceptedRoots{0}, rejectedRoots{0};
        };

        struct SMSReferenceCounters {
            std::atomic<unsigned long long> proposalTrials{0}, zeroTrials{0};
            std::atomic<unsigned long long> retryTrials{0}, tailTrials{0}, rouletteStops{0};
            std::atomic<unsigned long long> acceptedDiscoveries{0}, rejectedRoots{0}, ownedRoots{0};
            std::atomic<unsigned long long> sceneIntersectionQueries{0}, objectIntersectionQueries{0}, materialQueries{0};
            std::atomic<unsigned long long> scratchBufferGrowths{0}, scratchFrames{0}, scratchPeakBytes{0};
            std::atomic<unsigned long long> retryHistogram[32]{}; // powers of two, last bucket saturates
            // Estimator B (area-emitter partition). `retryTrials` counts every
            // reciprocal trial of both estimators; `topologyRetryTrials` is the
            // walk-only subset estimator B spends, so estimator A's root-level
            // K is retryTrials - topologyRetryTrials.
            std::atomic<unsigned long long> topologyRetryTrials{0}, canonicalSolves{0}, canonicalRoots{0};
            // Draws requested from the canonical predicate's guard sampler.
            // Any draw makes that answer uncertain (unowned); tests assert 0.
            std::atomic<unsigned long long> canonicalSamplerDraws{0};
            std::atomic<unsigned long long> partitionQueries{0}, partitionOwned{0}, partitionUncertain{0};
            // Diagnostics of an unaccepted Newton projection of PT's chain
            // (counted only when counters are attached): Newton itself
            // failed; a canonical root of the same topology lies within
            // 1e-3 / 1e-8 of the scale of every recorded PT vertex.
            std::atomic<unsigned long long> projectionNewtonFailures{0};
            std::atomic<unsigned long long> uncertainNearOwned{0}, uncertainAtOwned{0}, uncertainWithOwnedSet{0};
            // Cost/consistency diagnostics: RGB queries answered by one
            // shared classification, canonical sets reused within one B
            // evaluation, robust projection retries that succeeded, and
            // canonical roots B declined because PT's record of that very
            // root would not classify owned (the symmetric rule).
            std::atomic<unsigned long long> componentReuse{0}, canonicalCacheHits{0};
            std::atomic<unsigned long long> robustProjections{0}, asymmetricRootsDeclined{0};
        };

        // Counts logical scene queries in the active extended diagnostic scope.
        void SMSRecordSceneIntersection();
        void SMSRecordObjectIntersection();

        // Estimator A's reciprocal is conditional on the SAME channel and
        // endpoint context as discovery. Proposal includes every zero trial.
        // The weighted roulette sum is an estimate even when roulette stops;
        // discarding it would bias the accepted discovery toward zero.
        struct SMSRootReference {
            template<class Root, class Proposal, class Match>
            static Scalar Reciprocal(const Root& root, ISampler& retrySampler,
                Proposal&& proposal, Match&& match, unsigned int budget,
                bool roulette, SMSReferenceCounters* counters = nullptr) {
                SMSReciprocalTail tail(budget);
                unsigned long long trials = 0;
                Scalar uncapped = 1;
                for(;;) {
                    ++trials;
                    if(counters) {
                        counters->retryTrials.fetch_add(1, std::memory_order_relaxed);
                        if(trials > (budget ? budget : 1024))
                            counters->tailTrials.fetch_add(1, std::memory_order_relaxed);
                    }
                    decltype(auto) candidate = proposal(retrySampler);
                    if(match(root, candidate)) break;
                    if(roulette) {
                        if(!tail.ContinueAfterFailure(retrySampler)) {
                            if(counters) counters->rouletteStops.fetch_add(1, std::memory_order_relaxed);
                            break;
                        }
                    } else ++uncapped;
                }
                if(counters) {
                    unsigned int bucket = 0;
                    for(auto n = trials; n > 1 && bucket < 31; n >>= 1) ++bucket;
                    counters->retryHistogram[bucket].fetch_add(1, std::memory_order_relaxed);
                }
                return roulette ? tail.Estimate() : uncapped;
            }
            static Scalar Deposit(Scalar physicalContribution, Scalar reciprocal,
                Scalar channelProbability, Scalar emitterProbability, unsigned int originalTrials);
        };

        // TEST-ONLY. Never set by production code, a parser or the public
        // API (no production source references it; tests set and restore it
        // around a render). When set, estimator B's area deposit is zeroed
        // after its fixed parent draws while PT keeps applying ownership,
        // so a render isolates the PT-kept set and shares every PT path
        // with the full render. Left set, it would drop owned caustics.
        struct SMSExtendedTestHooks {
            static std::atomic<bool>& DropAreaContributions();
        };

        struct SMSQueryDomain {
            enum Kind { RGBComponent, Wavelength } kind;
            unsigned int component;
            Scalar nm;
            static SMSQueryDomain RGB(unsigned int c) { return {RGBComponent, c, 0}; }
            static SMSQueryDomain NM(Scalar wavelength) { return {Wavelength, 0, wavelength}; }
            bool Valid() const;
        };
        struct SMSNativeMaterialQuery {
            Scalar index = 1;
            Scalar attenuation = 1;
            bool reflection = false, transmission = false;
            bool dielectricInterface = false;
            bool deltaLimitProxy = false; // adopted finite-Phong dielectric approximation
            bool interiorTransmittance = false, reflectionTint = false;
            bool customFresnel = false;
        };
        struct SMSMediumCapture {
            const IObject* identity;
            const IMaterial* material;
            RayIntersectionGeometric context;
            SMSMediumCapture(const IObject* object, const IMaterial* provider,
                             const RayIntersectionGeometric& hit)
                : identity(object), material(provider), context(hit) {}
        };
        struct SMSStartingMedia {
            Scalar environmentIndex = 1;
            std::vector<SMSMediumCapture> enclosing; // outermost first
            bool reconstructible = false;
        };
        class SMSDomainReplay {
        public:
            // Static potential, not a sampled zero-lobe classification. A
            // supported patch must retain proposal mass even if probes miss it.
            static bool PotentialCaster(const IMaterial&);
            // PotentialCaster's exact-type gate alone (fixed for the life of
            // the material object).  PotentialCaster(m) is
            // PotentialCasterKind(m) == Reflector, or a Dielectric / Refractor
            // / Polished kind whose index painter IsPositionIndependent().
            enum CasterKind { NotCaster = 0, Reflector, Dielectric, Refractor, Polished };
            static CasterKind PotentialCasterKind(const IMaterial&);
            // Neutral refusal for unrecognized providers, composites and spatial IOR.
            static bool Query(const IMaterial&, const RayIntersectionGeometric&,
                              const IORStack&, SMSQueryDomain, SMSNativeMaterialQuery&);
            static bool Capture(const IScene&, const Point3&, const IORStack&, SMSStartingMedia&);
            static bool BuildStack(const SMSStartingMedia&, SMSQueryDomain, IORStack&);
            static bool EventWeight(const IMaterial&, const RayIntersectionGeometric&,
                                    const IORStack&, SMSQueryDomain, bool reflection,
                                    bool exiting, Scalar etaI, Scalar etaT,
                                    Scalar segmentLength, Scalar& weight);
            // Identity is a live scene object, not an opaque stack token. The caller
            // retains the real hit context, including the open-sheet certificate.
            // Reflection leaves membership unchanged; transmission applies the native crossing.
            static bool Cross(const IMaterial&, const IObject*, const RayIntersectionGeometric&,
                              SMSQueryDomain, bool reflection, IORStack&,
                              Scalar& etaI, Scalar& etaT, bool& exiting);
        };

		/// Data stored at each specular vertex during the manifold walk.
		struct ManifoldVertex
		{
			Point3				position;		///< World-space position on specular surface
			Vector3				normal;			///< World-space shading normal (ray-facing); used for Newton Jacobian & throughput
			//! True, ray-independent outward geometric face normal (`UnflippedGeomNormal()`,
			//! DL-70). Used by `BuildSeedChain` (`bEntering`), synthetic RIGs
			//! (`bGeomNormalOrientedToRay=false`), and `ValidateChainPhysics` (avoids
			//! Phong-tilt false rejections; docs/SMS_LEVENBERG_MARQUARDT.md).
			//! Exception: legacy-sentinel `SMSPhotonChainVertex` falls back to ray-facing
			//! `pv.normal`; no production consumer compares `normal` vs `geomNormal` sign.
			Vector3				geomNormal;
			Vector3				dpdu;			///< Position derivative w.r.t. first surface param (world space)
			Vector3				dpdv;			///< Position derivative w.r.t. second surface param (world space)
			Vector3				dndu;			///< Normal derivative w.r.t. first surface param (world space)
			Vector3				dndv;			///< Normal derivative w.r.t. second surface param (world space)
			Point3				objectPosition;	///< Exact hit in the geometry/CSG child frame for Po painters
			Point2				uv;				///< Surface parameters
			Scalar				eta;			///< Nominal material IOR (legacy fallback when etaI/etaT are default 1.0)
			Scalar				etaI;			///< Incoming-side (`wi`) medium IOR (Walter 2007 η_i; supports nested dielectrics)
			Scalar				etaT;			///< Outgoing-side (`wo`) medium IOR (Walter 2007 η_t; supports nested dielectrics)
			Scalar				radianceEtaI;	///< IOR `RadianceEtaScale` prices from (receiver->light stack top, or `etaI` on open sheets; 0 sentinel = use `etaI`; DL-619)
			RISEPel				attenuation;	///< Color attenuation at this vertex (e.g., colored glass refractance, mirror reflectance)
			Scalar				attenuationNM;	///< Cached wavelength-resolved multiplier, filled by spectral seed/reconstruction queries.
			bool				attenuationAppliesToReflection; ///< Native refractor tint is transmission-only.
			bool				hasCustomSpecularFresnel; ///< Reuse the material SPF coating law at the solved angle.
			bool				attenuationIsInteriorTransmittance;	///< Dielectric per-unit-distance tau; exiting transmission only.
			bool				isReflection;	///< True if ray reflects (mirror, Fresnel reflection, or TIR); false if refracts.
			bool				canRefract;		///< True for dielectrics (angle-dependent Fresnel); false for pure mirrors/conductors.
			bool				isOpenSheet;	///< Certified open sheet (`bProvablyNoInterior`), crossed by its face rule (DL-345/DL-619).
			bool				isExiting;		///< True if exiting object (from IOR-stack tracking, not local dot test, for double-sided sheets).
			const IObject*		pObject;		///< Object this vertex lies on
			const IMaterial*	pMaterial;		///< Material at this vertex
			bool				valid;			///< True if vertex data is complete

            // Scene-local opt-in. Retain ONLY hits that publish this vertex's
            // position, never neighbouring finite-difference validation hits.
            // Immutable sharing makes Newton copies/rollback preserve provenance.
            bool retainAlphaEndpoint = false;
            std::shared_ptr<const RayIntersection> alphaEndpoint;
            Point3 alphaEndpointPosition;
            bool HasAlphaEndpoint() const {
                return alphaEndpoint && alphaEndpoint->pObject == pObject &&
                    position.x == alphaEndpointPosition.x &&
                    position.y == alphaEndpointPosition.y &&
                    position.z == alphaEndpointPosition.z;
            }

			ManifoldVertex() :
			position( Point3(0,0,0) ),
			normal( Vector3(0,0,0) ),
			geomNormal( Vector3(0,0,0) ),
			dpdu( Vector3(0,0,0) ), dpdv( Vector3(0,0,0) ),
			dndu( Vector3(0,0,0) ), dndv( Vector3(0,0,0) ),
			objectPosition( Point3(0,0,0) ),
			uv( Point2(0,0) ),
			eta( 1.0 ),
			etaI( 1.0 ),
			etaT( 1.0 ),
			radianceEtaI( 0.0 ),
			attenuation( 1.0, 1.0, 1.0 ),
			attenuationNM( 1.0 ),
			attenuationAppliesToReflection( true ),
			hasCustomSpecularFresnel( false ),
			attenuationIsInteriorTransmittance( false ),
			isReflection( false ),
			canRefract( true ),
			isOpenSheet( false ),
			isExiting( false ),
			pObject( 0 ),
			pMaterial( 0 ),
			valid( false )
			{
			}
		};

		/// Configuration for the manifold solver.
        struct SMSDomainVertex {
            ManifoldVertex geometry;
            RayIntersectionGeometric context;
            Scalar contextSlope = 0; // measured context and normalized native-Fresnel price change per physical displacement
            explicit SMSDomainVertex(const RayIntersectionGeometric& hit) : context(hit) {}
        };

		struct ManifoldSolverConfig
		{
			bool			enabled;				///< Master switch: when false, no ManifoldSolver is created
            bool extendedMode;             ///< Opt-in; scene parameter `sms_extended` on pathtracing_{pel,spectral}_rasterizer (SMSConfig::extended -> RISE_API_CreatePathTracing*RasterizerEx).
            SMSDomainCounters* domainCounters; ///< Optional diagnostics; caller owns lifetime.
            SMSReferenceCounters* referenceCounters; ///< Optional estimator A diagnostics; caller owns lifetime.
            Scalar extendedEventFloor;     ///< Positive exploration mass for both supported R/T events.

			unsigned int	maxIterations;			///< Newton iteration limit
			Scalar			solverThreshold;		///< Convergence threshold on ||C||
			Scalar			uniquenessThreshold;	///< Threshold to distinguish solutions
			unsigned int	maxBernoulliTrials;		///< Max trials for unbiased PDF estimation
			bool			biased;					///< Skip Bernoulli PDF estimation (biased but fast)
			unsigned int	maxChainDepth;			///< Maximum number of specular vertices in chain
			Scalar			maxGeometricTerm;		///< Clamp for manifold geometric term

			/// Independent Newton solves per EvaluateAtShadingPoint call (default 1).
			/// Trial 0 uses the deterministic Snell seed; trials 1..N-1 use photon seeds
			/// (if photonCount > 0) and deduplicate unique converged roots by v_0 position.
			unsigned int	multiTrials;

			/// Photons emitted in PreRenderSetup to build SMSPhotonMap (0 disables).
			/// Stored records keep first specular entry points as fixed-radius kd-tree seeds.
			unsigned int	photonCount;

			/// World-space search radius for photon-seed lookup (0 = auto ~0.01 * bbox diag).
			Scalar			photonSearchRadius;

			/// Per-shading-point cap on photon seeds fed to Newton (default 16; 0 = uncapped).
			/// Excess kd-tree neighbors are randomly subsampled.
			unsigned int	maxPhotonSeedsPerShadingPoint;

			/// Two-stage Newton solver (Zeltner 2020 §5; `sms_two_stage`, default false):
			/// solves on smoothing=1 analytical base then refines on smoothing=0.
			/// No-op unless geometry exposes `ComputeAnalyticalDerivatives`. See docs/SMS_TWO_STAGE_SOLVER.md.
			bool			twoStage;

			/// Damped-Newton Levenberg-Marquardt diagonal damping `λ * mean(|J_ii|)` on J
			/// (default false; preserves block-tridiagonal solve). See docs/SMS_LEVENBERG_MARQUARDT.md.
			bool			useLevenbergMarquardt;

			/// Seeding strategy (default Snell; see docs/SMS.md):
			///   Snell:   trace from shading point toward light (RISE legacy).
			///   Uniform: uniform-area sample per caster in `mSpecularCasters` + SnellContinueChain
			///            (Mitsuba manifold_ss/ms; required for geometric Bernoulli 1/p).
			enum SeedingMode {
				eSeedingSnell   = 0,
				eSeedingUniform = 1
			};
			SeedingMode		seedingMode;

			/// Required specular chain length (0 = auto/maxChainDepth; recommended >0 for Uniform seeding).
			unsigned int	targetBounces;

			ManifoldSolverConfig() :
			enabled( false ),
            extendedMode( false ),
            domainCounters( nullptr ),
            referenceCounters( nullptr ),
            extendedEventFloor( 0.05 ),
			maxIterations( 15 ),
			solverThreshold( 1e-4 ),
			uniquenessThreshold( 1e-2 ),
			maxBernoulliTrials( 100 ),
			biased( true ),
			maxChainDepth( 30 ),
			maxGeometricTerm( 10.0 ),
			multiTrials( 1 ),
			photonCount( 0 ),
			photonSearchRadius( 0 ),
			maxPhotonSeedsPerShadingPoint( 16 ),
			twoStage( false ),
			useLevenbergMarquardt( false ),
			seedingMode( eSeedingSnell ),
			targetBounces( 0 )
			{
			}
		};

		/// Result of a manifold solve attempt.
		struct ManifoldResult
		{
			std::vector<ManifoldVertex>	specularChain;	///< Converged specular vertices
			RISEPel						contribution;	///< Path contribution (RGB)
			Scalar						contributionNM;	///< Path contribution (spectral)
			Scalar						pdf;			///< SMS PDF (1/p_k or 1.0 for biased)
			Scalar						jacobianDet;	///< |det(∂C/∂x_⊥)| of constraint Jacobian
			bool						valid;			///< True if Newton converged

			ManifoldResult() :
			contribution( RISEPel(0,0,0) ),
			contributionNM( 0 ),
			pdf( 1.0 ),
			jacobianDet( 1.0 ),
			valid( false )
			{
			}
		};

        struct SMSDomainRoot {
            SMSQueryDomain domain;
            IORStack startingStack;
            std::vector<SMSDomainVertex> vertices;
            ManifoldResult result;
            bool accepted = false;
            Scalar scale = 1;
            Scalar uncertainty = 0; // world-space last Newton correction plus coordinate roundoff
            explicit SMSDomainRoot(SMSQueryDomain d, const IORStack& stack) : domain(d), startingStack(stack) {}
        };

		/// DL-372 / DL-336 split suppression: what PT carries from the SMS
		/// ANCHOR (the non-delta vertex whose SMS evaluation estimates the
		/// chain) through the delta chain to an emitter hit, so the hit can
		/// ask `ManifoldSolver::ClassifyEmitterHitCoverage` whether SMS's
		/// own deterministic seed + solve reaches the same path.  See
		/// docs/SMS_ENERGY_LOSS_INVESTIGATION.md section 7.
		struct SMSChainRecord
		{
			static const unsigned int kMaxVertices = 16;

			struct Vertex
			{
				Point3			position;		///< PT's hit point at this delta vertex
				Vector3			normal;			///< shading normal (post-modifier), as RayCaster published it
				Vector3			geomNormal;		///< TRUE outward geometric normal (DL-70)
				Point3			objectPosition;
				Point2			uv;
				const IObject*	pObject;
				bool			isReflection;	///< the PT path stayed on the incident side here
			};

			bool			anchorValid;		///< the anchor's SMS inputs are recorded below
			/// The anchor evaluated EXTENDED SMS (estimator B for area
			/// emitters), so an emitter hit is owned only through
			/// ManifoldSolver::ExtendedEmitterHitOwned.  False: a legacy
			/// anchor, which keeps the split rule or suppress-all.
			bool			extendedAnchor;
			bool			broken;				///< the path left the specular-chain model (a medium scatter) -- no exact answer
			Point3			anchorPos;
			Vector3			anchorGeomNormal;
			Vector3			anchorShadingNormal;
			IORStack		anchorStack;		///< the receiver stack SMS's seed walk started from
			unsigned int	count;				///< delta vertices since the anchor (may exceed kMaxVertices)
			Vertex			v[kMaxVertices];

			SMSChainRecord() : anchorValid( false ), extendedAnchor( false ), broken( false ), anchorStack( 1.0 ), count( 0 ) {}

			//! A non-delta vertex that is NOT an SMS anchor (SPF-only,
			//! BSSRDF): no SMS estimate covers what follows.
			void Invalidate() { anchorValid = false; extendedAnchor = false; broken = false; count = 0; }

			//! A new anchor: the vertex whose SMS evaluation recorded
			//! these inputs scattered non-delta.
			void SetAnchor( const Point3& p, const Vector3& gN, const Vector3& sN, const IORStack& s,
				bool extended = false )
			{
				anchorValid = true; extendedAnchor = extended; broken = false; count = 0;
				anchorPos = p; anchorGeomNormal = gN; anchorShadingNormal = sN; anchorStack = s;
			}

			//! One delta vertex of the chain: @a dIn the incident ray
			//! direction, @a dOut the scattered one.
			void Append( const RayIntersectionGeometric& rig, const IObject* pObj,
				const Vector3& dIn, const Vector3& dOut )
			{
				if( count < kMaxVertices ) {
					Vertex& w = v[count];
					w.position = rig.ptIntersection;
					w.normal = rig.vNormal;
					w.geomNormal = rig.UnflippedGeomNormal();
					w.uv = rig.ptCoord;
					w.objectPosition = rig.ptObjIntersec;
					w.pObject = pObj;
					const Vector3& n = rig.vGeomNormal;
					w.isReflection = Vector3Ops::Dot( dIn, n ) * Vector3Ops::Dot( dOut, n ) < 0;
				}
				count++;
			}
		};

		/// Answer of `ManifoldSolver::ClassifyEmitterHitCoverage`.
		enum SMSChainCoverage
		{
			eSMSChainCovered,			///< SMS's estimator contains this path: PT suppresses it
			eSMSChainNotCovered,		///< SMS never estimates this path: PT keeps it
			eSMSChainCoverageUnknown	///< no exact answer: PT keeps today's rule (suppress)
		};

		/// Specular Manifold Sampling solver.
		///
		/// Given two non-specular endpoints (a shading point and an emitter),
		/// finds a valid path through a chain of specular surfaces using Newton
		/// iteration on the specular constraint manifold.
		class LightSampler;
		struct LightSample;
		class SMSPhotonMap;
		struct SMSPhoton;

		class ManifoldSolver :
			public virtual IReference,
			public virtual Reference
		{
		protected:
			ManifoldSolverConfig config;
            bool nativeEventConstraints = false; // Private extended solve instance; legacy instances stay false.
            const std::vector<SMSDomainVertex>* nativeContexts = nullptr; // Borrowed only by a private solve instance.
            Scalar nativeWavelength = 550; // Immutable private-solve domain, including fallback decisions.
            ManifoldSolver(const ManifoldSolverConfig&, bool nativeEvents, const std::vector<SMSDomainVertex>* = nullptr, Scalar = 550);
            const RasterizerState& NativeRaster(std::size_t i) const {
                return nativeContexts && i<nativeContexts->size() ? (*nativeContexts)[i].context.rast : nullRasterizerState;
            }
            const IObjectManager* NativeSceneObjects(std::size_t i) const {
                return nativeContexts && i<nativeContexts->size() ? (*nativeContexts)[i].context.signals.pScene : nullptr;
            }
            bool ComputeNativeVertexFrame(ManifoldVertex&, Scalar projectionDistance) const;
            ManifoldResult SolveDomainCore(const Point3&,const Vector3&,const Point3&,const Vector3&,
                const IScene&,const IORStack&,SMSQueryDomain,std::vector<SMSDomainVertex>&,ISampler&,Scalar,Scalar) const;
            ManifoldResult SolveCore(const Point3&, const Vector3&, const Point3&, const Vector3&,
                std::vector<ManifoldVertex>&, ISampler&, bool estimateLegacyPDF, Scalar convergenceThreshold = 0) const;
            void SolveDomainCoreInto(const Point3&,const Vector3&,const Point3&,const Vector3&,
                const IScene&,const IORStack&,SMSQueryDomain,std::vector<SMSDomainVertex>&,ISampler&,Scalar,Scalar,ManifoldResult&) const;
            void SolveCoreInto(const Point3&,const Vector3&,const Point3&,const Vector3&,
                std::vector<ManifoldVertex>&,ISampler&,bool,Scalar,ManifoldResult&) const;
            bool BuildExtendedWalk(const Point3&,const Point3&,const IScene&,const IORStack&,
                SMSQueryDomain,ISampler&,std::vector<SMSDomainVertex>&,const RasterizerState&,
                const IObject*,const std::vector<SMSDomainVertex>*,const Vector3*) const;
            // emitterObject null: a delta endpoint (any farther hit is unoccluded);
            // otherwise the final segment must reach that luminary at `end`.
            void FinalizeExtendedRootInto(const Point3&,const Vector3&,const Point3&,const Vector3&,
                const IScene&,ISampler&,const RasterizerState&,SMSDomainRoot&,const IObject* emitterObject,
                bool countRejection=true) const;
            // Rebuild the domain topology of an actual PT delta chain by
            // replaying its recorded vertices (objects, events, positions)
            // from the anchor. Applies the walk's acceptance filters.
            bool ReplayExtendedChain(const SMSChainRecord&,const IObject& luminary,const IScene&,
                SMSQueryDomain,const RasterizerState&,SMSDomainRoot&,bool* customFresnel=nullptr) const;
            // One classification of a recorded chain ending on `luminary` at
            // the partition point y: 1 owned, 0 not owned (no replayable
            // topology / not in the canonical set), -1 uncertain (PT's
            // chain has no accepted projection). `ownedSet` reuses an
            // already computed canonical set of the same (anchor,y,T).
            int ClassifyExtendedChain(const SMSChainRecord&,const IObject& luminary,const Point3& y,
                const Vector3& yNormal,const IScene&,SMSQueryDomain,const RasterizerState&,
                const std::vector<SMSDomainRoot>* ownedSet) const;
            // Static/scene inputs both sides of the area partition share.
            bool ExtendedAreaPartitionApplies(const IScene&,const IRayCaster&,const IObject* luminary,
                Scalar* pdfSelect) const;
            void ProposeExtendedRootInto(const Point3&,const Vector3&,const Point3&,const IScene&,
                const IORStack&,SMSQueryDomain,ISampler&,const RasterizerState&,SMSDomainRoot&) const;
            RISEPel EvaluateExtendedDelta(const Point3&, const Vector3&, const Vector3&,
                const OrthonormalBasis3D&, const IMaterial&, const Vector3&, const IScene&,
                const IRayCaster&, ISampler&, const LightSample&, const IORStack&,
                const RayIntersectionGeometric*, Scalar nm = 0) const;
            std::atomic<bool> hwssExtendedWarningEmitted{false};
            mutable std::atomic<bool> mediumExtendedWarningEmitted{false};
			LightSampler* pLightSampler;

			/// Optional borrowed photon-seed map set once in PreRenderSetup (null = Snell seed only).
			const SMSPhotonMap* pPhotonMap;

			/// Read-only cached specular-caster list populated at scene-prep for uniform seeding.
			std::vector<const IObject*> mSpecularCasters;

			/// True when `mSpecularCasters` contains a pure mirror (RGB snell mode adds random seeds).
			bool mHasPureMirrorCaster;

			virtual ~ManifoldSolver();

		public:
			ManifoldSolver( const ManifoldSolverConfig& cfg );
            void WarnHWSSLegacyMode();
            // Effective opt-in after the prepared scene-wide policy. Unknown
            // or unprepared managers leave extended mode inert.
            bool ExtendedModeActive(const IScene&) const;
            // When extended mode is inert this returns true to preserve the
            // existing contribution/suppression/shadow switches together.
            bool ExtendedAnchorEligible(const IScene&, const IRayCaster&, const Point3&,
                                        const IORStack&, Scalar nm = 0) const;
            // HWSS (Phase 4): eligible[i] == ExtendedAnchorEligible(..., nm[i])
            // for every i with evaluate[i] (false otherwise). Only the inputs
            // that do not depend on the wavelength (global checks, modifier
            // audit, starting-media capture) are evaluated once; the HG
            // scattering test and the domain stack build run per lane.
            void ExtendedAnchorEligibleNM(const IScene&, const IRayCaster&, const Point3&,
                const IORStack&, const Scalar* nm, const bool* evaluate, unsigned int count,
                bool* eligible) const;

			/// Attach a photon-aided seed map.  Must be called AFTER the
			/// map's Build() has completed (the map is read-only from
			/// then on, so concurrent render workers are safe).  Pass
			/// nullptr to detach.
			void SetPhotonMap( const SMSPhotonMap* pm ) { pPhotonMap = pm; }
			const SMSPhotonMap* GetPhotonMap() const { return pPhotonMap; }

			/// Attach the cached specular-caster list.  Call once at
			/// scene-prep (after EnumerateSpecularCasters).  Pass an
			/// empty vector to clear.
			void SetSpecularCasters( std::vector<const IObject*> list );
			const std::vector<const IObject*>& GetSpecularCasters() const {
				return mSpecularCasters;
			}

			/// Scan the scene's object manager and collect every object
			/// whose material reports `isSpecular`.  Each object is
			/// queried via UniformRandomPoint(prand=(0.5,0.5,0.5))
			/// to obtain a valid `RayIntersectionGeometric` for the
			/// material's GetSpecularInfo call.  Result is appended to
			/// `out` (not cleared) — callers manage the vector's lifetime.
			static void EnumerateSpecularCasters(
				const IScene& scene,
				std::vector<const IObject*>& out
				);

			/// Main entry point: solve for a specular path connecting
			/// shadingPoint to emitterPoint through the given chain of
			/// specular objects.
			///
			/// \param shadingPoint   Non-specular endpoint (e.g. diffuse surface hit)
			/// \param shadingNormal  Normal at shading point
			/// \param emitterPoint   Light sample point
			/// \param emitterNormal  Normal at light sample
			/// \param specularChain  Initial seed vertices on specular surfaces
			/// \param sampler            Sampler with dimensional management (for Bernoulli trials)
			/// \return ManifoldResult with converged chain and contribution info
			ManifoldResult Solve(
				const Point3& shadingPoint,
				const Vector3& shadingNormal,
				const Point3& emitterPoint,
				const Vector3& emitterNormal,
				std::vector<ManifoldVertex>& specularChain,
				ISampler& sampler
				) const;

            // Domain-isolated solve. Proposals and production estimators are separate.
            ManifoldResult SolveDomain(const Point3& start, const Vector3& startNormal,
                const Point3& end, const Vector3& endNormal, const IScene& scene,
                const IORStack& startingStack, SMSQueryDomain domain,
                std::vector<SMSDomainVertex>& vertices, ISampler& sampler,
                Scalar positionTolerance, Scalar convergenceThreshold = 0) const;

            bool BuildExtendedSeed(const Point3& start, const Point3& end,
                const IScene&, const IORStack&, SMSQueryDomain, ISampler&,
                std::vector<SMSDomainVertex>&, const RasterizerState& = nullRasterizerState,
                const IObject* selectedEmitter = nullptr) const;
            SMSDomainRoot ProposeExtendedRoot(const Point3& start, const Vector3& startNormal,
                const Point3& end, const IScene&, const IORStack&, SMSQueryDomain,
                ISampler&, const RasterizerState& = nullRasterizerState) const;
            // Shared bounded ownership policy. Seeds depend only on endpoints,
            // topology and domain; no sampled vertex positions or retry history.
            // Fixed first-caster surface seeds of the canonical predicate.
            static const unsigned int kExtendedCanonicalSurfaceSeeds = 5;
            // The returned roots are the OWNED set O(T,y) of estimator B and
            // PT's suppression alike; an uncertain solve contributes nothing.
            void CanonicalExtendedRoots(const Point3& anchor,const Vector3& anchorShadingNormal,
                const IObject& luminary,const Point3& y,const Vector3& yNormal,const IScene&,
                const SMSDomainRoot& topology,const RasterizerState&,std::vector<SMSDomainRoot>& owned,
                const SMSDomainRoot* stopAt=nullptr) const;
            // The partition's emitter point: `p` projected onto the luminary
            // along `n`, removing a single-sided sampler's object-space 1e-5
            // push. False (uncertain) unless the luminary is met within that
            // push's world-space bound of `p`.
            static bool ExtendedLuminaryPoint(const IObject& luminary,const Point3& p,const Vector3& n,Point3& out);
            // PT side for RGB: per component c with evaluate[c], owned[c].
            // When the three replayed component topologies are identical
            // (objects, events, indices, membership, no coating Fresnel)
            // one classification serves all three.
            void ExtendedEmitterHitOwnedRGB(const SMSChainRecord& rec,const IObject& luminary,
                const Point3& y,const Vector3& yNormal,const IScene&,const IRayCaster&,
                const bool evaluate[3],bool owned[3],const RasterizerState& = nullRasterizerState) const;
            // PT side of the area partition: does the canonical predicate own
            // the actual chain recorded in `rec`, ending on `luminary` at `y`,
            // in `domain`? False whenever the answer is uncertain.
            bool ExtendedEmitterHitOwned(const SMSChainRecord& rec,const IObject& luminary,
                const Point3& y,const Vector3& yNormal,const IScene&,const IRayCaster&,
                SMSQueryDomain domain,const RasterizerState& = nullRasterizerState) const;
            RISEPel EvaluateExtendedAreaReference(const Point3&,const Vector3&,const Vector3&,
                const OrthonormalBasis3D&,const IMaterial&,const Vector3&,const IScene&,
                const IRayCaster&,ISampler&,const LightSample&,const IORStack&,
                const RayIntersectionGeometric* = nullptr,Scalar nm = 0,int rgbComponent = -1) const;
            static bool SameExtendedTopology(const SMSDomainRoot&,const SMSDomainRoot&);
            // partitionBand: the area partition's identity (full tolerance band
            // for two converged solves of one canonical topology -- also
            // estimator A's rediscovery, via SameRediscoveredRoot); false:
            // the resolution-limited identity.
            static bool SameExtendedRoot(const SMSDomainRoot&, const SMSDomainRoot&, Scalar tolerance,
                bool partitionBand = false);
            // Estimator A's K-loop identity: a retry that re-solves the
            // discovered root from another seed (full tolerance band, DL-455).
            static bool SameRediscoveredRoot(const SMSDomainRoot&, const SMSDomainRoot&, Scalar tolerance);
            static Scalar ExtendedReflectionProbability(bool reflection, bool transmission,
                Scalar fresnel, bool tir, Scalar explorationFloor);

			/// Traces a seed ray from start toward end, collecting intersections
			/// with specular objects to build the initial chain.
			///
			/// \param start        Start point
			/// \param end          End point
			/// \param scene        Scene for ray casting
			/// \param caster       Ray caster
			/// \param chain        [out] Populated with seed ManifoldVertices
			/// \return Number of specular vertices found
			unsigned int BuildSeedChain(
				const Point3& start,
				const Point3& end,
				const IScene& scene,
				const IRayCaster& caster,
				std::vector<ManifoldVertex>& chain,
				bool applyEmitterStop = true,      ///< Snell mode: true (stop at emitter projection).  Uniform mode: false (sp is a direction probe, not the emitter).
				const IORStack* pStartStack = nullptr, ISampler* alphaSampler = nullptr	///< DL-290: the RECEIVER's live IOR stack; the walk starts in that medium.  Null = air (the pre-DL-290 behaviour).
				) const;

			/// Single seed chain with `proposalPdf` (always 1.0; retained for uniform-mode callers).
			struct SeedChainResult {
				std::vector<ManifoldVertex> chain;
				Scalar proposalPdf = 1.0;
			};

			/// Packages a single `BuildSeedChain` result into `std::vector<SeedChainResult>`
			/// with `proposalPdf = 1.0` for uniform-mode call sites.
			unsigned int BuildSeedChainBranching(
				const Point3& start,
				const Point3& end,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				std::vector<SeedChainResult>& out,
				bool applyEmitterStop = true,      ///< Snell mode: true.  Uniform mode: false (see BuildSeedChain).
				const IORStack* pStartStack = nullptr	///< DL-290: see BuildSeedChain.
				) const;

			/// Continues a seed chain by Snell-tracing from `(currentOrigin, dir)`,
			/// appending specular hits to `chain` and mutating `currentOrigin`, `dir`,
			/// `currentIOR`, and `seedIor` in place. Stops at non-specular hit, miss,
			/// `range > maxDist*3`, or `config.maxChainDepth`.
			/// \return Number of vertices appended.
			unsigned int SnellContinueChain(
				Point3& currentOrigin,
				Vector3& dir,
				Scalar maxDist,
				Scalar& currentIOR,
				IORStack& seedIor,
				const IScene& scene,
				const IRayCaster& caster,
				std::vector<ManifoldVertex>& chain,
				bool applyEmitterStop = true, ISampler* alphaSampler = nullptr       ///< false ⇒ ignore the projection cap; trace continues through every specular hit until maxChainDepth or non-specular hit.
				) const;

			/// Computes the geometric coupling factor through a specular
			/// chain for the path integral (incoming cosines / dist² at
			/// each specular vertex, plus 1/dist² for the last segment).
			/// Caller multiplies by cosAtShading and cosAtLight separately.
			/// NOTE: the 1/dist² terms overlap with the Jacobian determinant's
			/// internal direction derivatives.  Prefer EvaluateChainCosineProduct
			/// for the SMS contribution formula to avoid double-counting.
			Scalar EvaluateChainGeometry(
				const Point3& startPoint,
				const Point3& endPoint,
				const std::vector<ManifoldVertex>& chain
				) const;

			/// Computes the product of incoming cosines at each specular
			/// vertex in the chain, WITHOUT distance terms.  The distance
			/// factors (1/dist²) are already encoded in the Jacobian
			/// determinant via its direction derivatives (dwi/du ∝ 1/dist_i).
			/// Used in the SMS contribution formula: the weight is
			///   cos(θ_x) × cos(θ_y) × ∏cos(θ_in_j) / |det(∂C/∂x)|
			/// where only endpoint and vertex cosines are explicit.
			Scalar EvaluateChainCosineProduct(
				const Point3& startPoint,
				const Point3& endPoint,
				const std::vector<ManifoldVertex>& chain
				) const;

			/// Computes Fresnel-weighted transmittance/reflectance product
			/// along a chain. Boundary multipliers apply per event; dielectric
			/// tau applies per world-unit distance on exiting transmission,
			/// matching DielectricSPF. Participating-medium extinction is a
			/// separate factor, EvaluateChainMediumTransmittance (DL-419).
			///
			/// \return Per-channel attenuation factor for the chain
			RISEPel EvaluateChainThroughput(
				const Point3& startPoint,
				const Point3& endPoint,
				const std::vector<ManifoldVertex>& chain
				) const;

			/// Scalar (spectral) variant of EvaluateChainThroughput.
			Scalar EvaluateChainThroughputNM(
				const Point3& startPoint,
				const Point3& endPoint,
				const std::vector<ManifoldVertex>& chain,
				const Scalar nm
				) const;

			/// Participating-medium transmittance along receiver -> v_1 -> ... -> v_k -> light
			/// (DL-419), following `MediumTracking` from `pIorStack` (null = global medium only).
			/// Transmissive chain vertices enter/exit their object; interior medium boundaries
			/// cross by geometric facing; returns 1 without ray casts if scene has no media.
			RISEPel EvaluateChainMediumTransmittance(
				const Point3& startPoint,
				const Point3& endPoint,
				const std::vector<ManifoldVertex>& chain,
				const IRayCaster& caster,
				const IORStack* pIorStack
				) const;

			/// Scalar (spectral) variant of EvaluateChainMediumTransmittance.
			Scalar EvaluateChainMediumTransmittanceNM(
				const Point3& startPoint,
				const Point3& endPoint,
				const std::vector<ManifoldVertex>& chain,
				const IRayCaster& caster,
				const IORStack* pIorStack,
				const Scalar nm
				) const;

			const ManifoldSolverConfig& GetConfig() const { return config; }

			/// Result of a single SMS evaluation at a shading point.
			struct SMSContribution
			{
				RISEPel		contribution;	///< Total SMS contribution (BSDF * G * throughput * Le / pdf)
				Scalar		misWeight;		///< MIS weight for this contribution
				bool		valid;			///< True if a valid specular path was found
                bool referenceA;    ///< Extended reference estimator (A: delta lights, B: area partition); unclamped.

				SMSContribution() : contribution( RISEPel(0,0,0) ), misWeight( 1.0 ), valid( false ), referenceA( false ) {}
			};

			/// Standalone SMS evaluation at a single shading point.
			///
			/// Samples a light, builds a seed chain toward it, solves the
			/// manifold, evaluates the BSDF at the shading point, and
			/// assembles the full caustic contribution.
			///
			/// This is the reusable core that both BDPTIntegrator and
			/// SMSShaderOp call.
			///
			/// The receiver-frame normal is supplied as a SHADING/GEOMETRIC
			/// pair so the BSDF eval and BSDF-cosine factor stay in the
			/// shading frame (Veach §5.3.6) while probe-direction fallback
			/// and chain-topology decisions use the actual face orientation
			/// (PBRT 4e §10.1.1).
			///
			/// \param pos              Shading point position (non-specular surface)
			/// \param geomNormal       Geometric (flat-face) normal at shading point
			/// \param shadingNormal    Shading normal at shading point (BSDF frame)
			/// \param onb              Orthonormal basis at shading point (built from shading)
			/// \param pMaterial        Material at shading point (must have a BSDF)
			/// \param woOutgoing       Direction toward the viewer/previous vertex
			/// \param scene            Scene for ray casting and light access
			/// \param caster           Ray caster
			/// \param sampler          Sampler with proper dimensional seperation
			SMSContribution EvaluateAtShadingPoint(
				const Point3& pos,
				const Vector3& geomNormal,
				const Vector3& shadingNormal,
				const OrthonormalBasis3D& onb,
				const IMaterial* pMaterial,
				const Vector3& woOutgoing,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				const IORStack* pIorStack = nullptr,
                const RayIntersectionGeometric* anchorContext = nullptr, bool forceLegacy = false
				) const;

			/// Uniform-on-shape SMS evaluator (selected when `seedingMode == eSeedingUniform`;
			/// Mitsuba manifold_ss/ms). Draws uniform-area seeds per caster in `mSpecularCasters`,
			/// continues via `SnellContinueChain`, and sums per-caster estimates unweighted
			/// (no caster-choice MIS). Biased mode sums unique roots across multiTrials/photons;
			/// unbiased mode (and alpha coverage) uses geometric Bernoulli `1/p` with area seeds only.
			SMSContribution EvaluateAtShadingPointUniform(
				const Point3& pos,
				const Vector3& geomNormal,
				const Vector3& shadingNormal,
				const OrthonormalBasis3D& onb,
				const IMaterial* pMaterial,
				const Vector3& woOutgoing,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				const IORStack* pIorStack = nullptr,
                const RayIntersectionGeometric* anchorContext = nullptr, bool forceLegacy = false
				) const;

			/// Spectral variant of SMS evaluation.
			struct SMSContributionNM
			{
				Scalar		contribution;
				Scalar		misWeight;
				bool		valid;
                bool referenceA;

				SMSContributionNM() : contribution( 0 ), misWeight( 1.0 ), valid( false ), referenceA( false ) {}
			};

			/// Spectral counterpart of `EvaluateAtShadingPointUniform`.
			/// Iterates `mSpecularCasters` per-shading-point, runs M-trial
			/// biased or geometric-Bernoulli unbiased Newton solves with
			/// per-wavelength throughput, sums per-caster contributions
			/// (no MIS over caster choice — Mitsuba pattern).  Selected
			/// at runtime by `config.seedingMode == eSeedingUniform`.
			SMSContributionNM EvaluateAtShadingPointNMUniform(
				const Point3& pos,
				const Vector3& geomNormal,
				const Vector3& shadingNormal,
				const OrthonormalBasis3D& onb,
				const IMaterial* pMaterial,
				const Vector3& woOutgoing,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				const Scalar nm,
				const IORStack* pIorStack = nullptr,
                const RayIntersectionGeometric* anchorContext = nullptr, bool forceLegacy = false
				) const;

			SMSContributionNM EvaluateAtShadingPointNM(
				const Point3& pos,
				const Vector3& geomNormal,
				const Vector3& shadingNormal,
				const OrthonormalBasis3D& onb,
				const IMaterial* pMaterial,
				const Vector3& woOutgoing,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				const Scalar nm,
				const IORStack* pIorStack = nullptr,
                const RayIntersectionGeometric* anchorContext = nullptr, bool forceLegacy = false
				) const;

			/// Tests whether external segments (shadingPoint -> v_0 and v_{k-1} -> lightPoint)
			/// are unoccluded. Inter-specular segments inside glass are not tested (scene shadow
			/// rays would self-intersect the caster).
			bool CheckChainVisibility(
				const Point3& shadingPoint,
				const Point3& lightPoint,
				const std::vector<ManifoldVertex>& chain,
				const IRayCaster& caster, ISampler* alphaSampler = nullptr
				) const;

			/// The snell-mode BASE seed SMS builds at @a pos for emitter
			/// point @a lightPos: BuildSeedChain toward the light, then the
			/// surface-normal and midpoint fallbacks, and (spectral, @a nm
			/// > 0) the per-wavelength eta override.  Shared by
			/// EvaluateAtShadingPoint{,NM} and ClassifyEmitterHitCoverage,
			/// so the two cannot drift apart.  @a chain is empty when no
			/// seed exists.
			unsigned int BuildSnellBaseSeed(
				const Point3& pos,
				const Vector3& geomNormal,
				const Point3& lightPos,
				const IScene& scene,
				const IRayCaster& caster,
				std::vector<ManifoldVertex>& chain,
				const IORStack* pIorStack,
				ISampler* pSampler,
				const Scalar nm
				) const;

			/// DL-372 / DL-336: true when SMS's estimator at an anchor is a
			/// DETERMINISTIC function of (anchor, emitter point) -- snell
			/// seeding, biased, no photon map, no alpha coverage, and (RGB
			/// only, @a spectral false) no pure-mirror caster.  Only then
			/// can PT keep exactly the paths SMS does not estimate.
			/// Other modes cannot use deterministic root replay; DL-339's
			/// conservative first-reflection proof is handled separately.
			bool SplitSuppressionExact( const IRayCaster& caster, bool spectral ) const;

            /// DL-339: record stochastic legacy chains too when a native
            /// first-reflection impossibility proof may keep the emitter hit.
            /// Unknown classes retain the existing suppression rule.
            bool NeedsLegacyChainRecord( const IRayCaster& caster, bool spectral ) const;

			/// DL-372 / DL-336: does SMS's own snell seed + solve, run from
			/// the anchor recorded in @a rec toward emitter point @a y,
			/// reach the chain PT traced (same vertex objects, same
			/// reflect/refract pattern, same root)?  A PT chain through a
			/// WARPED delta lobe (a dielectric with finite `scattering`) is
			/// not exactly a root; it is assigned to the root Newton
			/// reaches from PT's own vertices (the delta-limit partition).
			/// @a nm 0 = the RGB estimator, > 0 = the spectral one at that
			/// wavelength. Call when NeedsLegacyChainRecord holds; stochastic modes
            /// only use the DL-339 first-reflection impossibility proof.
			SMSChainCoverage ClassifyEmitterHitCoverage(
				const SMSChainRecord& rec,
				const Point3& y,
				const Vector3& yNormal,
				const IScene& scene,
				const IRayCaster& caster,
				const Scalar nm
				) const;

			/// Reverses a photon's specular chain (light->receiver) into an SMS seed
			/// chain (receiver->light): flips `isExiting` and swaps interface `(etaI, etaT)`
			/// on refraction vertices, re-queries material attenuation/canRefract (and spectral
			/// IOR when `nm > 0`), restores UV/objectPosition, and sets `mv.valid = false`.
			/// \return Vertices written into `chain` (0 if photon chain length is invalid).
			unsigned int ReversePhotonChainForSeed(
				const SMSPhoton& photon,
				std::vector<ManifoldVertex>& chain,
				Scalar nm = 0,
				const IORStack* pReceiverStack = 0
				) const;

			/// Assigns each vertex's `radianceEtaI` (DL-619) as the receiver->light
			/// stack top before each crossing (`receiverTop`, then each refraction's
			/// `etaT`; certified open sheets use their own `etaI`).
			static void AssignRadianceEtas(
				std::vector<ManifoldVertex>& chain,
				Scalar receiverTop
				);

			/// Evaluates one converged trial's contribution (external visibility, BSDF,
			/// endpoint cosines, Le, and `G_x_v1 * |det dv/dy|`). Returns false on
			/// occlusion, zero/negative cosines, near-zero segment, or zero BSDF.
			/// `outSmsGeometric` reports the unclamped geometric factor; `clampGeometric`
			/// applies `maxGeometricTerm` (>0) per trial when true.
			bool ComputeTrialContribution(
				const Point3& pos,
				const Vector3& geomNormal,
				const Vector3& shadingNormal,
				const OrthonormalBasis3D& onb,
				const Vector3& woOutgoing,
				const IBSDF* pBSDF,
				const LightSample& lightSample,
				const ManifoldResult& mResult,
				const IRayCaster& caster,
				Vector3& outDir,
				RISEPel& outContribution,
				bool clampGeometric = true,
				Scalar* outSmsGeometric = nullptr,
				const IORStack* pIorStack = nullptr, ISampler* alphaSampler = nullptr
				) const;

			/// Spectral counterpart of `ComputeTrialContribution`.
			bool ComputeTrialContributionNM(
				const Point3& pos,
				const Vector3& geomNormal,
				const Vector3& shadingNormal,
				const OrthonormalBasis3D& onb,
				const Vector3& woOutgoing,
				const IBSDF* pBSDF,
				const LightSample& lightSample,
				const ManifoldResult& mResult,
				const IRayCaster& caster,
				const Scalar nm,
				Vector3& outDir,
				Scalar& outContribution,
				bool clampGeometric = true,           ///< See `ComputeTrialContribution`.
				Scalar* outSmsGeometric = nullptr,     ///< See `ComputeTrialContribution`.
				const IORStack* pIorStack = nullptr, ISampler* alphaSampler = nullptr
				) const;

			// ============================================================
			// Static utility methods (2x2 block arithmetic, Fresnel, etc.)
			// ============================================================

			/// Derivative of a normalized vector: d/dx [v/|v|].
			/// Given h = v/|v|, dv = derivative of unnormalized v, and
			/// vLen = |v|, returns the derivative of h.
			static Vector3 DeriveNormalized(
				const Vector3& h, const Vector3& dv, Scalar vLen );

			/// Inverts a 2x2 block stored as [a,b,c,d] row-major.
			/// Returns false if the block is singular.
			static bool Invert2x2( const Scalar* m, Scalar* inv );

			/// Multiplies 2x2 blocks: C = A * B  (all row-major).
			static void Mul2x2( const Scalar* A, const Scalar* B, Scalar* C );

			/// Multiplies 2x2 block by 2-vector: r = A * v.
			static void Mul2x2Vec( const Scalar* A, const Scalar* v, Scalar* r );

			/// Subtracts 2x2 blocks: C = A - B.
			static void Sub2x2( const Scalar* A, const Scalar* B, Scalar* C );

			static Scalar EvaluateVertexFresnel( const ManifoldVertex& v, Scalar cosI, Scalar etaI, Scalar etaT, Scalar nm );
			static RISEPel EvaluateVertexFresnelRGB( const ManifoldVertex& v, Scalar cosI, Scalar etaI, Scalar etaT );

			/// Exact dielectric Fresnel reflectance (unpolarized average).
			/// \param cosI     Cosine of incidence angle (positive)
			/// \param eta_i    IOR of the incoming medium
			/// \param eta_t    IOR of the transmitted medium
			/// \return Reflectance F in [0, 1].  Returns 1.0 for TIR.
			static Scalar ComputeDielectricFresnel(
				Scalar cosI, Scalar eta_i, Scalar eta_t );

			/// Computes world-space gradients of spherical coordinates
			/// (theta, phi) with respect to the direction vector.
			/// \param dir       Direction vector (world space)
			/// \param s         Normalized tangent u (= normalize(dpdu))
			/// \param t         Normalized tangent v (= normalize(dpdv))
			/// \param normal    Surface normal
			/// \param dTheta_dDir  [out] World-space gradient of theta
			/// \param dPhi_dDir    [out] World-space gradient of phi
			static void ComputeSphericalDerivatives(
				const Vector3& dir,
				const Vector3& s,
				const Vector3& t,
				const Vector3& normal,
				Vector3& dTheta_dDir,
				Vector3& dPhi_dDir );

			/// Computes 3x3 Jacobian d(wo)/d(wi) for the specular
			/// direction (reflection or refraction).  Assumes unnormalized
			/// wo; apply DeriveNormalized correction if chaining with
			/// the normalized output of ComputeSpecularDirection.
			static void ComputeSpecularDirectionDerivativeWrtWi(
				const Vector3& wi,
				const Vector3& normal,
				Scalar eta,
				bool isReflection,
				Scalar dwo_dwi[9] );

		protected:
			/// Builds the block-tridiagonal Jacobian for the
			/// angle-difference constraint via analytical derivatives.
			/// Uses ComputeSphericalDerivatives, ComputeSpecularDirection
			/// DerivativeWrtWi, and ComputeSpecularDirectionDerivative
			/// WrtNormal for the chain rule through the constraint.
			void BuildJacobianAngleDiff(
				const std::vector<ManifoldVertex>& chain,
				const Point3& fixedStart,
				const Point3& fixedEnd,
				std::vector<Scalar>& diag,
				std::vector<Scalar>& upper,
				std::vector<Scalar>& lower
				) const;

			/// Numerical Jacobian for the angle-difference constraint.
			/// Finite-differences EvaluateConstraintAtVertex.
			void BuildJacobianAngleDiffNumerical(
				const std::vector<ManifoldVertex>& chain,
				const Point3& fixedStart,
				const Point3& fixedEnd,
				std::vector<Scalar>& diag,
				std::vector<Scalar>& upper,
				std::vector<Scalar>& lower
				) const;

		protected:
			/// Validates that converged specular chain vertices have
			/// physically consistent ray geometry.  Refraction vertices
			/// must have wi and wo on opposite sides of the surface;
			/// reflection vertices must have both on the same side.
			/// \return True if all vertices pass the check.
			bool ValidateChainPhysics(
				const std::vector<ManifoldVertex>& chain,
				const Point3& fixedStart,
				const Point3& fixedEnd
				) const;

			/// Computes the determinant of a block-tridiagonal matrix
			/// via LU forward elimination.
			/// \return det(J) = product of det(Dp[i]) diagonal blocks.
			Scalar ComputeBlockTridiagonalDeterminant(
				const std::vector<Scalar>& diag,
				const std::vector<Scalar>& upper,
				const std::vector<Scalar>& lower,
				unsigned int k
				) const;

			/// Runs Newton iteration to solve C(x) = 0.
			/// Modifies chain in-place. Returns true if converged.
			///
			/// `smoothing` ∈ [0, 1] is forwarded to `UpdateVertexOnSurface`
			/// — > 0 walks on a smoothed reference surface (Stage 1 of the
			/// SMS two-stage solver), 0 (default) walks on the actual surface.
			bool NewtonSolve(
				std::vector<ManifoldVertex>& chain,
				const Point3& fixedStart,
				const Point3& fixedEnd,
				Scalar smoothing = 0.0, Scalar convergenceThreshold = 0
				) const;

			/// Evaluates the 2k-dimensional constraint vector.
			/// C[2*i] and C[2*i+1] are the angle-difference constraint
			/// at specular vertex i.
			void EvaluateConstraint(
				const std::vector<ManifoldVertex>& chain,
				const Point3& fixedStart,
				const Point3& fixedEnd,
				std::vector<Scalar>& C
				) const;

			/// Builds block-tridiagonal Jacobian dC/dx (row-major 2x2 blocks:
			/// diag[k], upper[k-1], lower[k-1]). `includeCurvature = true`
			/// includes tangent-frame rotation terms (required for caustic focusing
			/// |det|); `false` treats surface as locally flat for mesh Newton stability.
			void BuildJacobian(
				const std::vector<ManifoldVertex>& chain,
				const Point3& fixedStart,
				const Point3& fixedEnd,
				std::vector<Scalar>& diag,
				std::vector<Scalar>& upper,
				std::vector<Scalar>& lower,
				bool includeCurvature = true
				) const;

			/// Solves block-tridiagonal system J * delta = rhs.
			/// Returns false if system is singular.
			bool SolveBlockTridiagonal(
				std::vector<Scalar>& diag,
				const std::vector<Scalar>& upper,
				const std::vector<Scalar>& lower,
				const std::vector<Scalar>& rhs,
				unsigned int k,
				std::vector<Scalar>& delta
				) const;

			/// Computes the generalized geometric term (Jacobian determinant
			/// ratio) for MIS weighting.
			Scalar ComputeManifoldGeometricTerm(
				const std::vector<ManifoldVertex>& chain,
				const Point3& fixedStart,
				const Point3& fixedEnd
				) const;

			/// Computes the 2x2 block ∂C_at_vk / ∂y_tangent  — i.e., how the
			/// constraint at the LAST specular vertex v_k changes when the
			/// light endpoint y moves in its own tangent plane (y_s, y_t).
			/// Output `Jy` is row-major: [∂Cs/∂ys, ∂Cs/∂yt; ∂Ct/∂ys, ∂Ct/∂yt].
			void ComputeLastBlockLightJacobian(
				const ManifoldVertex& vk,
				const Point3& prevPos,
				const Point3& lightPos,
				const Vector3& lightNormal,
				Scalar Jy[4]
				) const;

			/// Solves the block-tridiagonal implicit-function-theorem problem
			/// to compute |det(δv_1_⊥ / δy_⊥)| — how the FIRST specular
			/// vertex's tangent-plane position responds to infinitesimal
			/// motion of the light endpoint y in its tangent plane.
			/// Returns 0 on singular Jacobian.  Sign is discarded (abs value).
			Scalar ComputeLightToFirstVertexJacobianDet(
				const std::vector<ManifoldVertex>& chain,
				const Point3& shadingPoint,
				const Point3& lightPos,
				const Vector3& lightNormal
				) const;

			/// Steps a vertex by `(du, dv)` on its surface and refreshes derivatives.
			/// When `smoothing > 0` and geometry supports `ComputeAnalyticalDerivatives`,
			/// steps directly in (u, v) on the smoothed analytical surface (docs/SMS_TWO_STAGE_SOLVER.md).
			bool UpdateVertexOnSurface(
				ManifoldVertex& vertex,
				Scalar du,
				Scalar dv,
				Scalar smoothing = 0.0, bool referenceRefinement = false
				) const;

			/// Populates world-space `(dpdu, dpdv, dndu, dndv)` for `vertex` (using
			/// analytical smoothed surface when `smoothing > 0` and supported).
			bool ComputeVertexDerivatives(
				ManifoldVertex& vertex,
				Scalar smoothing = 0.0, bool referenceRefinement = false
				) const;

			/// Gram-Schmidt orthonormalizes `(dpdu, dpdv)` in the tangent plane of
			/// `vertex.normal` and rescales `(dndu, dndv)` consistently so Jacobian
			/// determinants are parameterization-invariant.
			void OrthonormalizeTangentFrame(
				ManifoldVertex& vertex
				) const;

			/// Computes the specularly scattered direction at a vertex.
			/// For refraction: Snell's law. For reflection: mirror law.
			/// Returns false on total internal reflection (refraction only).
			bool ComputeSpecularDirection(
				const Vector3& wi,
				const Vector3& normal,
				Scalar eta,
				bool isReflection,
				Vector3& wo
				) const;

			/// Computes the derivative of the specular direction w.r.t.
			/// the surface normal. Returns a 3x3 matrix stored as 9 scalars
			/// (row-major).
			void ComputeSpecularDirectionDerivativeWrtNormal(
				const Vector3& wi,
				const Vector3& normal,
				Scalar eta,
				bool isReflection,
				Scalar dwo_dn[9]
				) const;

			/// Converts a direction to local spherical coordinates (theta, phi)
			/// in the tangent frame defined by dpdu, dpdv, normal.
			Point2 DirectionToSpherical(
				const Vector3& dir,
				const Vector3& dpdu,
				const Vector3& dpdv,
				const Vector3& normal
				) const;

			/// Evaluates the angle-difference constraint at a single specular
			/// vertex (Zeltner et al. 2020).  The constraint measures the
			/// angular deviation between the actual outgoing direction and the
			/// specularly scattered direction in the local tangent frame:
			///   C0 = theta_actual - theta_specular
			///   C1 = wrapToPi(phi_actual - phi_specular)
			void EvaluateConstraintAtVertex(
				const Point3& vertexPos,
				const Vector3& vertexNormal,
				const Vector3& vertexDpdu,
				const Vector3& vertexDpdv,
				Scalar vertexEta,
				bool vertexIsReflection,
				const Point3& prevPos,
				const Point3& nextPos,
				Scalar& C0,
				Scalar& C1
				) const;

			/// Builds the block-tridiagonal Jacobian via central finite
			/// differences on the constraint function.  Matches whichever
			/// constraint formulation EvaluateConstraint uses.
			void BuildJacobianNumerical(
				const std::vector<ManifoldVertex>& chain,
				const Point3& fixedStart,
				const Point3& fixedEnd,
				std::vector<Scalar>& diag,
				std::vector<Scalar>& upper,
				std::vector<Scalar>& lower
				) const;

			/// Bernoulli trial estimator for unbiased PDF of solution k.
			Scalar EstimatePDF(
				const ManifoldResult& solution,
				const Point3& shadingPoint,
				const Point3& emitterPoint,
				const std::vector<ManifoldVertex>& seedTemplate,
				ISampler& sampler
				) const;
		};
	}
}

#endif
