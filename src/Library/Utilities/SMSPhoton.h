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

	namespace Implementation
	{
		struct SMSPhoton
		{
			Point3				ptPosition;		///< REQUIRED FIRST: KD-tree builder reads [axis].  DIFFUSE-hit landing position.
			unsigned char		plane;			///< REQUIRED SECOND: KD-tree builder writes this.
			unsigned char		chainLen;		///< Number of specular vertices traversed (1 = entered one caster, 2 = entered+exited, etc.)

			Point3				entryPoint;		///< First specular-surface hit — the SEED for SMS Newton.
			const IObject*		entryObject;	///< Specular caster the photon entered.  Used to filter seeds to the caller's current chain caster.
			RISEPel				power;			///< Cumulative throughput at deposit time, retained for future throughput-weighted seed sampling.  Unused in biased-mode dedupe.

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
