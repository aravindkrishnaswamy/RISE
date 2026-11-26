//////////////////////////////////////////////////////////////////////
//
//  PathValueOps.h - Tag-dispatched wrappers around the existing
//    PathVertexEval and IBSDF overloads.  Allows integrator code to
//    be written generically on a tag (PelTag or NMTag) and resolve
//    at compile time to the right value() / valueNM() call.
//
//    Raw-value methods are thin dispatchers to the dual-signature
//    helpers; the area-response adapter also converts the projected
//    shading measure for path-space estimators. These let the
//    integrator templatization phase (2a / 2b / 2c) can write one
//    body per concern instead of two.
//
//    Keeping the dispatch in a separate header rather than adding a
//    new method to IBSDF / ISPF avoids widening the virtual
//    interface (see abi-preserving-api-evolution).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 17, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PATH_VALUE_OPS_
#define PATH_VALUE_OPS_

#include "Color/SpectralValueTraits.h"
#include "PathVertexEval.h"
#include "../Interfaces/IBSDF.h"
#include "../Intersection/RayIntersectionGeometric.h"

namespace RISE
{
	namespace PathValueOps
	{
		using SpectralDispatch::PelTag;
		using SpectralDispatch::NMTag;
		using SpectralDispatch::SpectralValueTraits;

		//////////////////////////////////////////////////////////////
		// BSDF value at a raw (bsdf, ri) pair.  Used by shader ops
		// and direct-evaluation paths.
		//////////////////////////////////////////////////////////////

		/// Primary template.  Specialized per tag below.
		template<class Tag>
		typename SpectralValueTraits<Tag>::value_type
		EvalBSDF(
			const IBSDF& bsdf,
			const Vector3& vLightIn,
			const RayIntersectionGeometric& ri,
			const Tag& tag );

		template<>
		inline RISEPel EvalBSDF<PelTag>(
			const IBSDF& bsdf,
			const Vector3& vLightIn,
			const RayIntersectionGeometric& ri,
			const PelTag& /*tag*/ )
		{
			return bsdf.value( vLightIn, ri );
		}

		template<>
		inline Scalar EvalBSDF<NMTag>(
			const IBSDF& bsdf,
			const Vector3& vLightIn,
			const RayIntersectionGeometric& ri,
			const NMTag& tag )
		{
			return bsdf.valueNM( vLightIn, ri, tag.nm );
		}

		//////////////////////////////////////////////////////////////
		// BSDF value at a BDPT / VCM path vertex.  Thin dispatcher
		// over PathVertexEval::EvalBSDFAtVertex / EvalBSDFAtVertexNM.
		//////////////////////////////////////////////////////////////

		template<class Tag>
		typename SpectralValueTraits<Tag>::value_type
		EvalBSDFAtVertex(
			const BDPTVertex& vertex,
			const Vector3& wi,
			const Vector3& wo,
			const Tag& tag );

		template<>
		inline RISEPel EvalBSDFAtVertex<PelTag>(
			const BDPTVertex& vertex,
			const Vector3& wi,
			const Vector3& wo,
			const PelTag& /*tag*/ )
		{
			return PathVertexEval::EvalBSDFAtVertex( vertex, wi, wo );
		}

		template<>
		inline Scalar EvalBSDFAtVertex<NMTag>(
			const BDPTVertex& vertex,
			const Vector3& wi,
			const Vector3& wo,
			const NMTag& tag )
		{
			return PathVertexEval::EvalBSDFAtVertexNM( vertex, wi, wo, tag.nm );
		}

		// Area-measure surface response for connections, light tracing and
		// photon merging. Sampling/guiding callers keep EvalBSDFAtVertex's
		// raw material value, whose cosine is in the shading frame.
		template<class Tag>
		inline typename SpectralValueTraits<Tag>::value_type EvalAreaBSDFAtVertex(
			const BDPTVertex& vertex, const Vector3& wi, const Vector3& wo,
			const Tag& tag )
		{
			return EvalBSDFAtVertex<Tag>( vertex, wi, wo, tag ) *
				PathVertexEval::RadianceShadingNormalFactor( vertex, wi );
		}

		// Area-measure response at a LIGHT-subpath endpoint that connects
		// (or splats) toward `woOut`; `wiArrival` points back toward the
		// previous light vertex.  A BSSRDF entry vertex (`isBSSRDFEntry`)
		// is reached by the subsurface JUMP from the hit where the light
		// went in, so `wiArrival` there is the jump chord, not a ray: the
		// entry re-emits with Sw in the direction it LEAVES, i.e. toward
		// `woOut` (DL-317 for VCM, DL-377 for BDPT/MLT).  Evaluating Sw
		// along the chord instead reads exactly 0 on a convex shape, whose
		// chord faces INTO the surface at the entry.
		template<class Tag>
		inline typename SpectralValueTraits<Tag>::value_type EvalLightEndAreaBSDFAtVertex(
			const BDPTVertex& vertex, const Vector3& wiArrival, const Vector3& woOut,
			const Tag& tag )
		{
			return vertex.isBSSRDFEntry
				? EvalAreaBSDFAtVertex<Tag>( vertex, woOut, wiArrival, tag )
				: EvalAreaBSDFAtVertex<Tag>( vertex, wiArrival, woOut, tag );
		}

		//////////////////////////////////////////////////////////////
		// DL-481: the area-measure response split by lobe label
		// (PathVertexEval::EvalBSDFByTypeAtVertex), with the same
		// shading-normal factor and argument order as
		// EvalAreaBSDFAtVertex / EvalLightEndAreaBSDFAtVertex.  A split
		// endpoint is never a BSSRDF entry (it has no lobe type), so the
		// light-end form needs no entry swap.  False when the vertex has
		// no split.
		//////////////////////////////////////////////////////////////

		inline bool EvalBSDFByTypeAtVertexTag(
			const BDPTVertex& vertex, const Vector3& wi, const Vector3& wo,
			const PelTag& /*tag*/, RISEPel out[5] )
		{
			return PathVertexEval::EvalBSDFByTypeAtVertex( vertex, wi, wo, out );
		}

		inline bool EvalBSDFByTypeAtVertexTag(
			const BDPTVertex& vertex, const Vector3& wi, const Vector3& wo,
			const NMTag& tag, Scalar out[5] )
		{
			return PathVertexEval::EvalBSDFByTypeAtVertexNM( vertex, wi, wo, tag.nm, out );
		}

		template<class Tag>
		inline bool EvalAreaBSDFByTypeAtVertex(
			const BDPTVertex& vertex, const Vector3& wi, const Vector3& wo,
			const Tag& tag, typename SpectralValueTraits<Tag>::value_type out[5] )
		{
			if( !EvalBSDFByTypeAtVertexTag( vertex, wi, wo, tag, out ) ) {
				return false;
			}
			const Scalar k = PathVertexEval::RadianceShadingNormalFactor( vertex, wi );
			for( int t = 0; t < 5; t++ ) {
				out[t] = out[t] * k;
			}
			return true;
		}

		//////////////////////////////////////////////////////////////
		// PDF evaluation at a path vertex.  PDFs are wavelength-
		// independent in their return type (always Scalar) but the
		// NM path may query wavelength-dependent IOR for SPF::PdfNM.
		// The tag dispatch preserves that behavior.
		//////////////////////////////////////////////////////////////

		template<class Tag>
		Scalar EvalPdfAtVertex(
			const BDPTVertex& vertex,
			const Vector3& wi,
			const Vector3& wo,
			const Tag& tag );

		template<>
		inline Scalar EvalPdfAtVertex<PelTag>(
			const BDPTVertex& vertex,
			const Vector3& wi,
			const Vector3& wo,
			const PelTag& /*tag*/ )
		{
			return PathVertexEval::EvalPdfAtVertex( vertex, wi, wo );
		}

		template<>
		inline Scalar EvalPdfAtVertex<NMTag>(
			const BDPTVertex& vertex,
			const Vector3& wi,
			const Vector3& wo,
			const NMTag& tag )
		{
			return PathVertexEval::EvalPdfAtVertexNM( vertex, wi, wo, tag.nm );
		}

		//////////////////////////////////////////////////////////////
		// Context-carrying twins.  Same dispatch, same semantics; the
		// caller supplies a `PathVertexEval::VertexPdfContext` built
		// once for a vertex so that several queries at that ONE vertex
		// share a single reconstructed RayIntersectionGeometric and
		// IORStack.  See that class's comment for the contract (it
		// holds a reference to the vertex; it must not outlive it).
		//////////////////////////////////////////////////////////////

		template<class Tag>
		Scalar EvalPdfAtVertex(
			PathVertexEval::VertexPdfContext& ctx,
			const Vector3& wi,
			const Vector3& wo,
			const Tag& tag );

		template<>
		inline Scalar EvalPdfAtVertex<PelTag>(
			PathVertexEval::VertexPdfContext& ctx,
			const Vector3& wi,
			const Vector3& wo,
			const PelTag& /*tag*/ )
		{
			return PathVertexEval::EvalPdfAtVertex( ctx, wi, wo );
		}

		template<>
		inline Scalar EvalPdfAtVertex<NMTag>(
			PathVertexEval::VertexPdfContext& ctx,
			const Vector3& wi,
			const Vector3& wo,
			const NMTag& tag )
		{
			return PathVertexEval::EvalPdfAtVertexNM( ctx, wi, wo, tag.nm );
		}

		// Note: no Scale() helpers here.  Both RISEPel and Scalar
		// already support operator*(Scalar), so templated code can
		// write `v * s` directly.  A `Scale` free function would
		// also collide on overload lookup with the destructive
		// `ColorMath::Scale( T& )` sibling.
	}
}

#endif
