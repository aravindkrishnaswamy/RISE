# V2 implemented sufficient-support ledger

Read HYLIOS_COUPLED_LINEAGE_PROOF.md and HYLIOS_ANGULAR_LINEAGE.md for the implementation contract. All marks describe ensemble
laws in a named frame, not numeric rank or observed direction equality. These
transitions are implemented as hylios_angular_lineage_v2;
physical versions and RNG remain unchanged.

Support certificates:

- **P4(frame)**: joint boundary position2/direction2 AC; frame is current individual
  relative body or entire fixed complex layout. Also valid on transverse sections
  of its current internal ray via the geometric-flight theorem.
- **Q2**: current optical-boundary position has surface-area AC in that frame.
- **Fan**: the current same-child chord was launched by an accepted cosine with
  fresh angular density conditional on its starting point. The point need not Q2.
- **Beam**: complex still follows its initial freshly projected parallel beam;
  no nonidentity directional child transform has occurred.

| Actual seam | Sufficient certificate | Public law and support update |
|---|---|---|
| New individual placement | Incoming angular AC; actual conditional axis density and fresh impact | Acquire P4 in new body frame. |
| First individual far boundary | Fresh projected impact for fixed pre-placement D and chosen body axis | Q2, plus v1 first-projected-normal angular proof for unmatched first exit. |
| New complex envelope placement/build | Incoming angular AC; fresh impact; independent layout-only generator | Acquire P4 conditional fixed layout a.e.; set Beam regardless of input class. |
| Layout proposals/yields/restarts | Any | Preserve public class and support; no travel/scattering. |
| First untouched-beam child far boundary | Beam | Q2 and first-projected-normal angular proof. First curved direction change consumes Beam. |
| Far boundary after a Fan chord | Fan | Acquire Q2 by radial projection; consume Fan after using its exit-map certificate. |
| Any strict flight or Snell/reflection | P4 in active frame | Preserve P4 and public angular AC, including arbitrary complex revisits. |
| Child transmitted exit, exact matched index | Any | Identity preserves public class and P4. Preserve Beam if it held. Do not carry bare Q2 into the next unproved host/child traversal. |
| Unmatched curved exit | P4 or first-projected-normal proof | Angular AC. Preserve P4 only if it already held. Otherwise angular-only support. |
| Unmatched same-child exit of Fan | Sphere, or eta<=1 convex point-source certificate | Angular AC. No automatic P4. |
| Unmatched curved exit without a certificate | Angular-only/unknown | Keep v1 UnclassifiedSingular. Angular AC alone is insufficient. |
| Curved reflected branch without P4/first-normal proof | Any other support | Conservative public unknown, retain any Q2 for the pending cosine. |
| Rejected cosine proposal | Any | Preserve all current class/support; point and selected old axis stay fixed in the state. |
| Accepted cosine | Q2 or P4 | Acquire/preserve P4; public angular AC. |
| Accepted cosine | Neither Q2 nor P4 | Public angular AC and set Fan. Do not claim joint P4. The next far point becomes Q2; a further reflection+cosine can acquire P4. |
| Bottom/Rayleigh/nonzero-exponential/nonzero-fold accepted law | Any | Refresh angular AC only. Do not invent P4 for an already retained layout. |
| Envelope crossing / zero final kick | Any | No Snell event; preserve public class. |
| Empty complex | Any | Host-only identity at every wavelength; no final kick. |
| Body discard | Any | Clear body-frame support; preserve public angular class. |
| Cancel/budget/error/zero/absorbed | Any | Preserve diagnostic support as appropriate to the last successful event; no returned atom or returned density is claimed for nonreturns. |

The first-reflection intermediate state and its cosine rejections need separate
Q2 bookkeeping. A single monotone `hasRandomized` flag is insufficient. No table
entry is authorized by child-entry counts, RNG draw counts, exact numeric vector
equality, or a finite-difference determinant.

Residual unknown family: atom-entered multiple-child transmission chains that
have neither acquired P4 nor reached an applicable first-normal/point-source
certificate at their last unmatched exit. No blanket generic-layout rank proof
is supplied. Positive mass in this family continues to prohibit a complete bake.
