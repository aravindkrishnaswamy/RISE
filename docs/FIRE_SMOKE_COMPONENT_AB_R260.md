# r260 — component A/Bs (in progress)

2026-10-27. The owner accepts r259, drops the inlet question, and switches from
read-only diagnosis to external component experiments. No production change.
No tier 6, tier 10, or from-zero RISE run is authorized by this experiment.

All variants start at r251 step 9224, **15.01872063824 s**, with the same source,
MC scalar transport, geometry and 33 kW input. Runs are serial. Each stops at
the first ordinary accepted step reaching four additional seconds; no shortened
endpoint step is introduced. Reduction uses precisely the first four seconds.
A 8192-step diagnostic ceiling bounds failures rather than replacing physical
elapsed time. The normal 32 GiB protection remains unchanged.

Before a long run, the same observer writes a two-step MC reference. Each B
compares full accepted Q/T/M/U fields in its first two steps, records numeric
counts and maximum differences, and exits before step three if unchanged.
This is a no-op check, not historical checkpoint bit-identity certification.

## Components

1. **Half timestep.** Halve the ordinary CFL/gravity/diffusion and existing
   source-step bounds, before output-event alignment. The dominant source cap
   changes 0.001646266→0.000823133 s. Preserve the existing 1.1× step-growth
   bound: the original selector receives twice the previous dt before halving,
   avoiding a recursive 0.55× timestep collapse. Source/closure formulas,
   scalar/momentum schemes, projection and tolerances remain unchanged.
2. **Restoration off — definition pending.** The active resident path has no
   separate legacy physical-then-manifold-restoration projection call. R2 is
   force-inclusive physical projection of frozen accepted scalars; its composed
   target already omits the absolute EOS tail. The active R0/R1 target iteration
   can apply EOS-tail correction. The owner has been asked whether to test that
   term off while retaining all physical projections, or record the separate
   restoration pass as absent. Do not silently remove physical R2 or publish
   predictor momentum as the accepted Heun result. Independent tests proceed.
3. **Plain centered momentum mass carrier.** For interior primal faces,
   Fadv_g=0.5(rho_g,L+rho_g,R)u_face, with unchanged physical gas-diffusion flux
   added. Restrict to dual faces with the existing arithmetic MAC map, then use
   the same centered velocity factor. Accepted total boundary carriers remain
   unchanged. Scalar MC/FCT, source, stress, force and projections are unchanged.
   This deliberately removes scalar-reconstruction coupling from interior
   momentum; it is not a full implementation of FDS's rotational formulation.
   The existing scalar commuting diagnostic still concerns the unchanged scalar
   flux and must not be interpreted as certification of the new momentum pair.

Builds are generated under external `r260_components/`. The production source
and object files are never overwritten. The centered patch and executable hashes
are in `docs/data/fire_r260/`. Host and Metal compilation are checked; the actual
first-two-step field check is required before any four-second B proceeds.

## Report and decision

One table will include MC A (0.243/0.479/0.719 kg/s), accepted CHARM, all three
components and LES FDS (0.303/0.701/1.238). For each completed component, report
net R<0.8 m mass at the native heights 0.994/2.004/3.013 m, its actual duration,
dt statistics and fraction of the 3 m gap recovered. The earlier CHARM/FDS
reference interval is 3.981891 s and is labelled explicitly; component statistics
use four seconds. An unchanged or refused variant receives a status, not a
fabricated full-window mass measurement.

A component closing most of the gap merits a production proposal and a tier-8
from-zero onset re-verification plan, **before implementation**. None is adopted
automatically. If no component moves the result materially, report the measured
failure to recover entrainment and list remaining RISE/FDS differences. Stop for
owner review.

## Companion FDS run

[r261 full-output FDS](FIRE_SMOKE_FDS_FULL_OUTPUT_R261.md) runs first, with the
same explicit LES/Vreman/CHARM physics as r258. The component runs follow it
serially. All physical outputs are retained; no recurrent automation is used.
**Measured component results pending.**
