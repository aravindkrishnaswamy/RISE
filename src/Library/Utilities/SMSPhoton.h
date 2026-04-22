//////////////////////////////////////////////////////////////////////
//
//  SMSPhoton.h - Per-photon record for Specular Manifold Sampling
//    photon-aided seed generation.
//
//    Classical photon mapping stores photons at their DIFFUSE
//    landing positions so an eye-pass gather can estimate radiance
//    by density estimation.  SMS doesn't want the radiance estimate;
//    it wants to know that a specific photon's path traversed the
//    specular chain and landed near a given shading point — because
//    that path's FIRST SPECULAR HIT is a known-good seed for the
//    Newton iteration in ManifoldSolver::Solve.
//
//    An SMSPhoton therefore stores TWO world-space positions:
//      (1) ptPosition — the diffuse-hit position.  This is what the
//          kd-tree is indexed by.  An eye-pass query "give me seeds
//          near shading point P" is a fixed-radius lookup on this.
//      (2) entryPoint — the position of the FIRST specular-surface
//          hit along the photon's trajectory.  This IS the seed we
//          hand to BuildSeedChain.
//
//    Multi-bounce specular chains (k >= 2) are collapsed down to the
//    first specular entry point; Newton's continuation via Snell /
//    reflection laws populates subsequent vertices from that entry.
//
//    The struct's first two fields (ptPosition and plane) mirror the
//    layout expected by the kd-tree balance/query template cloned
//    from PhotonMapping/PhotonMap.h's PhotonMapCore — same pattern
//    VCMLightVertex uses for its vertex-merging store.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-20
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SMS_PHOTON_
#define SMS_PHOTON_

#include "Math3D/Math3D.h"
#include "Color/Color.h"

namespace RISE
{
	class IObject;
	class IMaterial;

	namespace Implementation
	{
		/// Per-specular-vertex record captured during the photon walk.
		///
		/// Stored in photon-direction order (v[0] is the specular hit
		/// nearest the LIGHT, v[chainLen-1] is nearest the diffuse
		/// landing).  When this chain is used to seed a receiver-side
		/// SMS Newton solve, the consumer must iterate in REVERSE order
		/// and flip `isExiting` on each vertex (the photon going
		/// air→glass at a surface corresponds to the receiver ray going
		/// glass→air at the same surface).
		struct SMSPhotonChainVertex
		{
			Point3				position;
			Vector3				normal;			///< Outward surface normal at hit (geometric, direction-independent).
			Scalar				eta;			///< Material IOR at this vertex.
			const IObject*		pObject;
			const IMaterial*	pMaterial;
			unsigned char		flags;			///< bit 0: isExiting (photon-direction, refractions only); bit 1: isReflection (scatter chose reflection, not refraction).

			SMSPhotonChainVertex() :
				position( 0, 0, 0 ), normal( 0, 0, 0 ), eta( 1.0 ),
				pObject( 0 ), pMaterial( 0 ), flags( 0 ) {}
		};

		/// Fixed upper bound on the number of specular vertices we store
		/// per photon.  Photons with longer chains are dropped at
		/// trace time rather than spilling into a dynamic allocation.
		/// Empirically k=7 covers 99.9% of paths on test scenes and
		/// keeps sizeof(SMSPhoton) at ~560 bytes — ~5.4 MB for a 10k
		/// photon pass, which is negligible.
		enum { kSMSMaxPhotonChain = 7 };

		struct SMSPhoton
		{
			Point3				ptPosition;		///< REQUIRED FIRST: KD-tree builder reads [axis].  DIFFUSE-hit landing position.
			unsigned char		plane;			///< REQUIRED SECOND: KD-tree builder writes this.
			unsigned char		chainLen;		///< Number of specular vertices traversed (1 = entered one caster, 2 = entered+exited, etc.)

			Point3				entryPoint;		///< Kept for quick caster-matching queries: == chain[0].position when chainLen>0.
			const IObject*		entryObject;	///< == chain[0].pObject when chainLen>0.
			RISEPel				power;			///< Cumulative throughput at deposit time.

			/// Full specular chain traversed by the photon, photon-direction order.
			/// [0] = nearest LIGHT; [chainLen-1] = nearest DIFFUSE landing.
			SMSPhotonChainVertex	chain[ kSMSMaxPhotonChain ];

			SMSPhoton() :
				ptPosition( 0, 0, 0 ),
				plane( 0 ),
				chainLen( 0 ),
				entryPoint( 0, 0, 0 ),
				entryObject( 0 ),
				power( 0, 0, 0 )
			{
			}
		};
	}
}

#endif
