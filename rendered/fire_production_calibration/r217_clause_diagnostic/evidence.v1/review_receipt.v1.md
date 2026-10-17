# r217 review ledger

Scope: EOS fifth-primitive formulation fix, diagnostic first-writer/cell/input/
seal reporting, qualification-only energy tap, independent host interval
comparison and reciprocal mutant. Production tolerances are unchanged.

The initial independent audit identified the P1 reciprocal formulation defect:
both EOSDD evaluators used exact-labelled binary32 0.2 instead of certified /5.
Both sites were fixed. Its independent linked fp64 record probe agreed with
the native retained-step diagnostic on energy -40438.5 = -8.682269253768027e-6.
Sibling audit covered direct/shared enthalpy, endpoints, target consumers and
the separately bounded ordinary binary32 physical-flux path.

| Finding | Disposition / named RED |
|---|---|
| Missing/corrupt phase copy could still report success | Fixed: poisoned buffers, shared exact phase predicate, required marker; `r217_missing_or_corrupt_phase_witness_refused` |
| Missing cell/input/seal witness could still report success | Fixed: typed witness conjunction and required complete marker; `r217_missing_predicate_or_seal_witness_refused` |
| Host log treated as an exact mathematical value | Fixed: independent outward atanh/log interval; `r217_log_provider_projection_requires_independent_enclosure` |
| Malformed mutant payload could count as a numerical RED | Fixed: finite components, nonnegative finite bound and host containment required; `r217_invalid_mutant_payload_not_counted_as_reciprocal_red` |
| An upper residual bound crossing does not prove failure | Fixed: downward residual lower bound required for RED and host projection witness; `r217_residual_upper_only_crossing_not_counted_as_reciprocal_red` |

Fresh mathematical/semantic reviewer r217_bounds_review: no new P1/P2 in the
production fix. It independently ran the first energy gate and confirmed the
known missing-host-bound qualification issue, not a completed qualification.
Its exact-1000-K boundary hypothesis was retracted after actual Metal output
demonstrated successful evaluation at that boundary.

Fresh test-integrity reviewer r217_test_review: four P2s above, all fixed and
re-reviewed; final what's-left round returned no new P1/P2. It verified the
archive SHA and git whitespace gate. No GPU execution was claimed by this
reviewer. Earlier phase review was by an existing campaign agent, not counted
as a fresh reviewer.

These are review receipts, not substitutes for exact-build runtime gates.
Final qualification and launch are separate evidence records.
