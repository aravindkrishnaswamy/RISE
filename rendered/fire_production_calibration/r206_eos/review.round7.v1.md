# r206 fresh review, round 7

Reviewed HEAD: `c9993d15`; numerical source: `20607aea`.
Independent reviewers: `r206_round7_evidence`, `r206_round7_fidelity`.
No P1; two P2 findings, confirmed and fixed before round 8.

1. Raw-prefix dispatch ignored indented duplicate EOS/producer/profile records.
   Repair: recognize tags by whitespace-aware token identity before parsing.
   REDs exercise each consumer with whole-record and tag-separator whitespace.
2. Preliminary standalone EOS RED log had a different full Metal source hash;
   final build/owner/publication qualification did not include that fixture.
   Repair: executed the unchanged attested binary's standalone EOS fixture,
   retaining its successful log and source/executable-bound supplement. The
   analyzer now requires that supplement, named RED set, exact endpoint 0x80
   refusal/publication counters, and complete 49.5M log-domain study.

The previous owner gate and timing evidence remain valid for their stated
scopes; they were not standalone EOS refusal coverage. Measurement v3 adds
the missing qualification binding without changing any measured values.
New source/missing-fixture/RED/domain mutants guard the mandatory dependency.

No solver source changed. Both reviewers found the exact-cache numerical and
lineage design coherent, and the cost claims correctly limited to cold prefixes.
