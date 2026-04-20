//////////////////////////////////////////////////////////////////////
//
//  ManifoldSolver.h - Specular Manifold Sampling solver for BDPT.
//
//    Implements the Newton iteration method from Zeltner et al. 2020
//    ("Specular Manifold Sampling") to find valid specular paths
//    connecting two non-specular endpoints through an arbitrary chain
//    of specular (delta distribution) surfaces.
//
//    The solver works by expressing the specular constraint at each
//    vertex as a 2D equation C_i = 0 (the angle-difference between
//    the actual outgoing direction and the specularly scattered
//    direction), then using Newton's method with block-tridiagonal
//    Jacobian structure to iteratively adjust vertex positions until
//    the constraint is satisfied.
//
//    Supports both refraction and reflection at each vertex, with
//    material-specific IOR via IMaterial::GetSpecularInfo().
//
//    SMS and BDPT occupy disjoint path spaces for delta materials:
//    BDPT skips delta vertices in its MIS walk, so it cannot generate
//    the caustic paths that SMS finds.  Simple addition of SMS and
//    BDPT contributions is correct without cross-strategy MIS.
//    See docs/SMS.md for the full analysis and glossy-extension notes.
//
//  References:
//    - Zeltner, Georgiev, Jakob. "Specular Manifold Sampling."
//      SIGGRAPH 2020.
//    - Hanika, Droske, Fascione. "Manifold Next Event Estimation."
//      CGF 2015.
//    - Fan et al. "Manifold Path Guiding." SIGGRAPH Asia 2023.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MANIFOLD_SOLVER_
#define MANIFOLD_SOLVER_

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

namespace RISE
{
	class IScene;
	class IRayCaster;

	namespace Implementation
	{
		/// Data stored at each specular vertex during the manifold walk.
		struct ManifoldVertex
		{
			Point3				position;		///< World-space position on specular surface
			Vector3				normal;			///< Surface normal (world space)
			Vector3				dpdu;			///< Position derivative w.r.t. first surface param (world space)
			Vector3				dpdv;			///< Position derivative w.r.t. second surface param (world space)
			Vector3				dndu;			///< Normal derivative w.r.t. first surface param (world space)
			Vector3				dndv;			///< Normal derivative w.r.t. second surface param (world space)
			Point2				uv;				///< Surface parameters
			Scalar				eta;			///< IOR ratio (n_incoming / n_outgoing) at this vertex
			RISEPel				attenuation;	///< Color attenuation at this vertex (e.g., colored glass refractance)
			bool				isReflection;	///< True if this vertex uses reflection, false for refraction
			const IObject*		pObject;		///< Object this vertex lies on
			const IMaterial*	pMaterial;		///< Material at this vertex
			bool				valid;			///< True if vertex data is complete

			ManifoldVertex() :
			position( Point3(0,0,0) ),
			normal( Vector3(0,0,0) ),
			dpdu( Vector3(0,0,0) ), dpdv( Vector3(0,0,0) ),
			dndu( Vector3(0,0,0) ), dndv( Vector3(0,0,0) ),
			uv( Point2(0,0) ),
			eta( 1.0 ),
			attenuation( 1.0, 1.0, 1.0 ),
			isReflection( false ),
			pObject( 0 ),
			pMaterial( 0 ),
			valid( false )
			{
			}
		};

		/// Configuration for the manifold solver.
		struct ManifoldSolverConfig
		{
			bool			enabled;				///< Master switch: when false, no ManifoldSolver is created
			unsigned int	maxIterations;			///< Newton iteration limit
			Scalar			solverThreshold;		///< Convergence threshold on ||C||
			Scalar			uniquenessThreshold;	///< Threshold to distinguish solutions
			unsigned int	maxBernoulliTrials;		///< Max trials for unbiased PDF estimation
			bool			biased;					///< Skip Bernoulli PDF estimation (biased but fast)
			unsigned int	maxChainDepth;			///< Maximum number of specular vertices in chain
			Scalar			maxGeometricTerm;		///< Clamp for manifold geometric term

			ManifoldSolverConfig() :
			enabled( false ),
			maxIterations( 15 ),
			solverThreshold( 1e-4 ),
			uniquenessThreshold( 1e-4 ),
			maxBernoulliTrials( 100 ),
			biased( true ),
			maxChainDepth( 30 ),
			maxGeometricTerm( 10.0 )
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

		/// Specular Manifold Sampling solver.
		///
		/// Given two non-specular endpoints (a shading point and an emitter),
		/// finds a valid path through a chain of specular surfaces using Newton
		/// iteration on the specular constraint manifold.
		class LightSampler;

		class ManifoldSolver :
			public virtual IReference,
			public virtual Reference
		{
		protected:
			ManifoldSolverConfig config;
			LightSampler* pLightSampler;

			virtual ~ManifoldSolver();

		public:
			ManifoldSolver( const ManifoldSolverConfig& cfg );

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
				std::vector<ManifoldVertex>& chain
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
			/// along a converged specular chain, including Beer's law attenuation.
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

			const ManifoldSolverConfig& GetConfig() const { return config; }

			/// Result of a single SMS evaluation at a shading point.
			struct SMSContribution
			{
				RISEPel		contribution;	///< Total SMS contribution (BSDF * G * throughput * Le / pdf)
				Scalar		misWeight;		///< MIS weight for this contribution
				bool		valid;			///< True if a valid specular path was found

				SMSContribution() : contribution( RISEPel(0,0,0) ), misWeight( 1.0 ), valid( false ) {}
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
			/// \param pos          Shading point position (non-specular surface)
			/// \param normal       Surface normal at shading point
			/// \param onb          Orthonormal basis at shading point
			/// \param pMaterial    Material at shading point (must have a BSDF)
			/// \param woOutgoing   Direction toward the viewer/previous vertex
			/// \param scene        Scene for ray casting and light access
			/// \param caster       Ray caster
			/// \param sampler          Sampler with proper dimensional seperation
			SMSContribution EvaluateAtShadingPoint(
				const Point3& pos,
				const Vector3& normal,
				const OrthonormalBasis3D& onb,
				const IMaterial* pMaterial,
				const Vector3& woOutgoing,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler
				) const;

			/// Spectral variant of SMS evaluation.
			struct SMSContributionNM
			{
				Scalar		contribution;
				Scalar		misWeight;
				bool		valid;

				SMSContributionNM() : contribution( 0 ), misWeight( 1.0 ), valid( false ) {}
			};

			SMSContributionNM EvaluateAtShadingPointNM(
				const Point3& pos,
				const Vector3& normal,
				const OrthonormalBasis3D& onb,
				const IMaterial* pMaterial,
				const Vector3& woOutgoing,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				const Scalar nm
				) const;

			/// Tests whether the external segments of an SMS specular
			/// chain are unoccluded.  Checks two segments:
			///   1. shading point -> first specular vertex
			///   2. last specular vertex -> light source
			///
			/// LIMITATION: Inter-specular segments (vertices inside
			/// the glass body) are NOT tested.  CastShadowRay uses
			/// the scene's acceleration structure which includes the
			/// glass geometry itself, so any ray between two glass
			/// vertices would report a false self-intersection.
			/// Filtering specular objects from shadow tests would
			/// require per-object exclusion lists, which is a more
			/// invasive change.  This means an opaque object placed
			/// entirely inside a glass body (between two specular
			/// vertices) would not be caught.  In practice this is
			/// rare; the common occlusion case (wall between the
			/// floor and the glass, or between the glass and the
			/// light) is handled by the external-segment checks.
			///
			/// \return True if both external segments are unoccluded.
			bool CheckChainVisibility(
				const Point3& shadingPoint,
				const Point3& lightPoint,
				const std::vector<ManifoldVertex>& chain,
				const IRayCaster& caster
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
			bool NewtonSolve(
				std::vector<ManifoldVertex>& chain,
				const Point3& fixedStart,
				const Point3& fixedEnd
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

			/// Builds the block-tridiagonal Jacobian dC/dx.
			/// Stored as arrays of 2x2 blocks: diag[k], upper[k-1], lower[k-1].
			/// Each block is stored as 4 scalars in row-major order.
			///
			/// `includeCurvature`: if true (default), include the ds_du
			/// tangent-frame-rotation terms — required for the measure-
			/// conversion |det| to reflect surface curvature, hence for
			/// correct caustic focusing in the contribution formula.  If
			/// false, skip them — useful for Newton iteration stability
			/// on meshes where the derivatives jump discontinuously
			/// across triangle boundaries.  The resulting Jacobian is
			/// APPROXIMATE w.r.t. the true dC/du (treats the surface as
			/// locally flat) but converges more reliably under line
			/// search.
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

			/// Updates a vertex position on its surface by stepping along
			/// the surface parameterization by (du, dv), then recomputes
			/// all derivative data via ray intersection.
			bool UpdateVertexOnSurface(
				ManifoldVertex& vertex,
				Scalar du,
				Scalar dv
				) const;

			/// Fills in surface derivative data (dpdu, dpdv, dndu, dndv)
			/// for a vertex by querying its object's geometry with proper
			/// world/object space transforms.
			bool ComputeVertexDerivatives(
				ManifoldVertex& vertex
				) const;

			/// Make dpdu, dpdv an orthonormal basis of the tangent plane
			/// at vertex.normal (Gram-Schmidt + unit-length normalization).
			/// Scales dndu, dndv consistently so dn/du remains the rate of
			/// change per unit displacement in the (new, unit) dpdu direction.
			/// Required because geometry-supplied dpdu/dpdv (e.g. triangle
			/// edges) need not be orthogonal or unit — if we fed those into
			/// BuildJacobian unchanged, |det| would vary with the arbitrary
			/// choice of parameterization instead of tracking the physical
			/// caustic geometry.
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
