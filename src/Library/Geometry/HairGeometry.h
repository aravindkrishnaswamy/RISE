//////////////////////////////////////////////////////////////////////
//
//  HairGeometry.h - Runtime curve primitive: flat-array hair/fur groom
//    with an embedded segment BVH.
//
//  Supports two construction modes: explicit strands (`StrandDesc`, built
//  immediately) and deferred groom (`HairGroomRecipe`, built once via
//  idempotent `Realize()`; empty until realized).
//
//  Attribution: Ray-centric recursive subdivision intersection follows
//  PBRT-v4's `Curve` strategy (Pharr, Jakob & Humphreys, "Physically
//  Based Rendering", Apache-2.0); independent Catmull-Rom / orthonormal
//  implementation reporting closest approach (not true silhouette entry).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef HAIR_GEOMETRY_
#define HAIR_GEOMETRY_

#include "Geometry.h"
#include "../Acceleration/BVH.h"
#include "../Acceleration/AccelerationConfig.h"
#include "../Interfaces/ProceduralDescriptors.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		//! 8-byte naturally aligned BVH element identifying one sub-segment of a
		//! Catmull-Rom span (span uint16_t caps strands at 65,536 control points).
		struct HairSegmentRef
		{
			uint32_t	strand;		///< index of the owning strand
			uint16_t	span;		///< index of the Catmull-Rom span within the strand (0 .. numCP-2)
			uint8_t		subIndex;	///< index of this sub-segment within the span, 0 .. (1<<subDepth)-1
			uint8_t		subDepth;	///< build-time split depth of the span, 0 .. kMaxBuildSplitDepth
		};

		//! Object-space hair/fur curve geometry backed by `BVH<HairSegmentRef>` (leaf cap 4).
		//! - Storage: control points and widths are stored in `float` for memory density,
		//!   widened to `Scalar` (double) for all intersection math.
		//! - Basis: uniform Catmull-Rom with linearly reflected phantom endpoints
		//!   (`P[-1] = 2*P[0]-P[1]`, `P[n] = 2*P[n-1]-P[n-2]`), keeping 2-CP strands straight.
		//! - Face/exit conventions: ribbons always face the ray (front/back cull flags ignored),
		//!   setting `bGeomNormalOrientedToRay = true` and `bGeomNormalRayDerived = true`;
		//!   zero-thickness exit info equals entry.
		class HairGeometry :
			public virtual Geometry,
			public virtual TreeElementProcessor<HairSegmentRef>
		{
		public:
			//! Build-time per-span split depth cap (3 => up to 8 sub-segments/span);
			//! intersection splits further at runtime as needed.
			static const unsigned int kMaxBuildSplitDepth = 3;

			//! Hard cap on control points per strand (imposed by HairSegmentRef::span uint16_t).
			static const unsigned int kMaxControlPointsPerStrand = 65536;

			//! Caller-supplied strand descriptor (root first, tip last).
			struct StrandDesc
			{
				std::vector<Point3>	controlPoints;	///< >= 2 points, root first, tip last
				Scalar				rootWidth;		///< > 0, full width (not radius) at the root
				Scalar				tipWidth;		///< > 0, full width at the tip
				Point2				rootUV;			///< base-surface root UV; reported verbatim as ri.ptCoord1

				StrandDesc() : rootWidth( 0 ), tipWidth( 0 ), rootUV( 0, 0 ) {}
			};

			//! Explicit-strand constructor: invalid strands (<2 or >65536 CPs, non-positive
			//! or non-finite width/CP) are skipped individually with warnings.
			HairGeometry( const std::vector<StrandDesc>& strands );

			//! Deferred-groom constructor: addrefs the recipe's base geometry and painters
			//! and builds nothing until Realize(). Validate via ValidateHairGroomRecipe at parse time.
			HairGeometry( const HairGroomRecipe& recipe, const char* chunkName );

			// ---- IGeometry -------------------------------------------------

			//! Idempotent deferred-groom realization; no-op in explicit mode or after first call.
			void Realize() const override;

			//! Diagnostic/test accessor: true if explicit mode or deferred base is tessellatable.
			bool IsValid() const;

			//! Diagnostic/test accessor: true after Realize() (always true in explicit mode).
			bool IsRealized() const { return bRealized.load( std::memory_order_acquire ); }

			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const override;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const override;
			BoundingBox GenerateBoundingBox() const override;

			bool DoPreHitTest() const override { return true; }

			//! False: emissive fur / exact surface area sampling is unsupported.
			bool CanBeAreaLight() const override { return false; }

			//! False: grooms are not tessellatable into triangle meshes.
			bool CanTessellate() const override { return false; }

			//! Deterministic stub (CanBeAreaLight() is false).
			void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const override;

			//! Returns 0 (surface sampling is unsupported).
			Scalar GetArea() const override { return 0; }

			// ---- IKeyframable ----------------------------------------------

			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ) override { return 0; }
			void SetIntermediateValue( const IKeyframeParameter& val ) override {}
			void RegenerateData() override {}

			// ---- TreeElementProcessor<HairSegmentRef> ----------------------

			typedef HairSegmentRef	MYOBJ;

			void RayElementIntersection( RayIntersectionGeometric& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;
			void RayElementIntersection( RayIntersection& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const override;
			bool RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;
			BoundingBox GetElementBoundingBox( const MYOBJ elem ) const override;
			bool ElementBoxIntersection( const MYOBJ elem, const BoundingBox& bbox ) const override;
			char WhichSideofPlaneIsElement( const MYOBJ elem, const Plane& plane ) const override;

			//! Straight field serialization of 8-byte HairSegmentRef for TreeElementProcessor.
			void SerializeElement( IWriteBuffer& buffer, const MYOBJ elem ) const override;
			void DeserializeElement( IReadBuffer& buffer, MYOBJ& ret ) const override;

			// ---- Introspection (tests / diagnostics) -----------------------

			unsigned int numStrands() const			{ return (unsigned int)( strandCPBegin.size() ? strandCPBegin.size() - 1 : 0 ); }
			unsigned int numRejectedStrands() const	{ return nRejectedStrands; }
			unsigned int numControlPoints() const	{ return (unsigned int)( cps.size() / 3 ); }
			unsigned int numSegments() const		{ return nSegments; }

			//! Number of control points of strand `s`.
			unsigned int numControlPointsOfStrand( unsigned int s ) const { return strandCPBegin[s+1] - strandCPBegin[s]; }
			//! Control point `i` of strand `s`, widened to Scalar.
			Point3 ControlPoint( unsigned int s, unsigned int i ) const;
			//! The strand's stored root UV (what a hit reports as ri.ptCoord1).
			Point2 StrandRootUV( unsigned int s ) const { return Point2( strandRootU[s], strandRootV[s] ); }
			//! Full width of strand `s` at arc-length fraction `sArc` in [0,1].
			Scalar StrandWidthAt( unsigned int s, const Scalar sArc ) const;
			//! Total (polyline-sampled) arc length of strand `s`.
			Scalar StrandArcLength( unsigned int s ) const;
			//! Evaluates strand `s` at GLOBAL parameter `u` in [0, numSpans],
			//! i.e. u = span index + local parameter.  Optionally returns
			//! dP/du.  Public so tests can build an independent reference.
			Point3 EvaluateStrand( unsigned int s, const Scalar u, Vector3* outDeriv ) const;
			//! Arc-length fraction in [0,1] corresponding to global parameter `u`.
			Scalar ArcFractionAt( unsigned int s, const Scalar u ) const;

			//! Rough resident size of the groom's own arrays plus the
			//! embedded BVH.  Diagnostic only.
			size_t ApproximateMemoryBytes() const;

		protected:
			virtual ~HairGeometry();

		private:
			// Storage is `mutable` for const Realize() (serialized by realizeMutex;
			// never mutated on the render hot path).

			mutable std::vector<float>		cps;				//!< strand-major control points (3 floats each)
			mutable std::vector<uint32_t>	strandCPBegin;		//!< CP index offsets, size numStrands+1
			mutable std::vector<float>		cpArcCum;			//!< cumulative polyline arc length per CP
			mutable std::vector<float>		strandRootWidth;
			mutable std::vector<float>		strandTipWidth;
			mutable std::vector<float>		strandRootU;
			mutable std::vector<float>		strandRootV;

			mutable BVH<HairSegmentRef>*	pSegBVH;
			mutable BoundingBox				bbox;
			mutable unsigned int			nSegments;
			mutable unsigned int			nRejectedStrands;

			// Deferred-groom state (pRecipe == nullptr, bRealized == true in explicit mode).
			HairGroomRecipe*		pRecipe;				//!< heap-owned; holds addrefs on base + painters
			std::vector<double>			guidePointsOwned;	//!< owned backing store for pRecipe->guidePoints
			std::vector<unsigned int>	guideCountsOwned;
			std::string				groomName;				//!< scene chunk name for diagnostics
			mutable std::atomic<bool>	bRealized;
			mutable std::mutex		realizeMutex;			//!< serializes Realize(); never taken on hot path

			void BuildFromStrands( const std::vector<StrandDesc>& strands ) const;

			static const unsigned int kArcSamplesPerSpan = 16;

			void EvalSpanCoefficients( unsigned int s, unsigned int span, Vector3 c[4] ) const;
			Scalar StrandMaxWidth( unsigned int s ) const;
			BoundingBox SegmentBoundingBox( const MYOBJ elem ) const;

			//! Shared intersection kernel: outputs ray parameter `outT` and strand-global
			//! parameter `outU` of the closest qualifying approach in (NEARZERO, tMax).
			//! Note: reports closest approach on flattened pieces (not true silhouette entry),
			//! so ptIntersection sits slightly off-silhouette and shadow rays may under-occlude
			//! when dHowFar lies between true entry and closest approach.
			bool IntersectSegment( const Ray& ray, const Scalar tMax, const MYOBJ elem,
			                       Scalar& outT, Scalar& outU ) const;

			HairGeometry( const HairGeometry& );
			HairGeometry& operator=( const HairGeometry& );
		};
	}
}

#endif
