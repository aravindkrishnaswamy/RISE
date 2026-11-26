# Batch 8 positive subsurface caps

Partial DL-482: nonlocal jumps are charged once; reflection-label endpoint/interior consistency remains open. Native PT recursive SSS casts inherit the authored jump cap through a native-only context pointer. Standalone frozen rasterizers keep the null pointer fallback; no frozen source is edited.

The unmodified baseline passes the weak sphere render bands but fails all five exact accounting checks. The walker-only intermediate fixes accounting but fails RGB random-walk cap-one parity because the PT reference continuation has the wrong cap. Final code fixes both. Six ValueSalt replicates per render, sample SD with n−1, independent PT comparator at three combined SE, no floor. Deterministic metadata checks have n=1 and SD=0. No timing claim.

## red-cap

```text
    RGB diffusion translucent cap 0 PT mean 0.038050  sd 0.000317  se 0.000130  (n=6)
    RGB diffusion translucent cap 0 bdpt mean 0.037679  sd 0.000844  se 0.000344  (n=6)
    RGB diffusion translucent cap 0 vcm mean 0.037988  sd 0.000926  se 0.000378  (n=6)
    RGB diffusion translucent cap 1 PT mean 0.546060  sd 0.001890  se 0.000772  (n=6)
    RGB diffusion translucent cap 1 bdpt mean 0.545257  sd 0.004417  se 0.001803  (n=6)
    RGB diffusion translucent cap 1 vcm mean 0.544296  sd 0.004256  se 0.001738  (n=6)
    RGB diffusion translucent cap 2 PT mean 0.547909  sd 0.001805  se 0.000737  (n=6)
    RGB diffusion translucent cap 2 bdpt mean 0.546107  sd 0.002679  se 0.001094  (n=6)
    RGB diffusion translucent cap 2 vcm mean 0.543511  sd 0.004183  se 0.001708  (n=6)
    RGB random-walk translucent cap 0 PT mean 0.037737  sd 0.000427  se 0.000174  (n=6)
    RGB random-walk translucent cap 0 bdpt mean 0.037580  sd 0.000702  se 0.000287  (n=6)
    RGB random-walk translucent cap 0 vcm mean 0.038123  sd 0.000353  se 0.000144  (n=6)
    RGB random-walk translucent cap 1 PT mean 0.508936  sd 0.001456  se 0.000594  (n=6)
    RGB random-walk translucent cap 1 bdpt mean 0.507931  sd 0.001525  se 0.000623  (n=6)
    RGB random-walk translucent cap 1 vcm mean 0.509312  sd 0.001975  se 0.000806  (n=6)
    RGB random-walk translucent cap 2 PT mean 0.508917  sd 0.001530  se 0.000625  (n=6)
    RGB random-walk translucent cap 2 bdpt mean 0.508186  sd 0.001589  se 0.000649  (n=6)
    RGB random-walk translucent cap 2 vcm mean 0.508230  sd 0.001513  se 0.000618  (n=6)
    HWSS diffusion translucent cap 0 PT mean 0.037954  sd 0.000281  se 0.000115  (n=6)
    HWSS diffusion translucent cap 0 bdpt mean 0.038407  sd 0.002852  se 0.001164  (n=6)
    HWSS diffusion translucent cap 0 vcm mean 0.038334  sd 0.003078  se 0.001257  (n=6)
    HWSS diffusion translucent cap 1 PT mean 0.543427  sd 0.002146  se 0.000876  (n=6)
    HWSS diffusion translucent cap 1 bdpt mean 0.545579  sd 0.004366  se 0.001782  (n=6)
    HWSS diffusion translucent cap 1 vcm mean 0.540095  sd 0.006697  se 0.002734  (n=6)
    HWSS diffusion translucent cap 2 PT mean 0.546120  sd 0.003717  se 0.001517  (n=6)
    HWSS diffusion translucent cap 2 bdpt mean 0.544930  sd 0.006420  se 0.002621  (n=6)
    HWSS diffusion translucent cap 2 vcm mean 0.542331  sd 0.005169  se 0.002110  (n=6)
    HWSS random-walk translucent cap 0 PT mean 0.038249  sd 0.000320  se 0.000131  (n=6)
    HWSS random-walk translucent cap 0 bdpt mean 0.038531  sd 0.002221  se 0.000907  (n=6)
    HWSS random-walk translucent cap 0 vcm mean 0.038154  sd 0.002577  se 0.001052  (n=6)
    HWSS random-walk translucent cap 1 PT mean 0.508065  sd 0.000870  se 0.000355  (n=6)
    HWSS random-walk translucent cap 1 bdpt mean 0.510601  sd 0.006857  se 0.002799  (n=6)
    HWSS random-walk translucent cap 1 vcm mean 0.512603  sd 0.008810  se 0.003597  (n=6)
    HWSS random-walk translucent cap 2 PT mean 0.508817  sd 0.000944  se 0.000385  (n=6)
    HWSS random-walk translucent cap 2 bdpt mean 0.506610  sd 0.006415  se 0.002619  (n=6)
    HWSS random-walk translucent cap 2 vcm mean 0.508557  sd 0.004488  se 0.001832  (n=6)
DL482 positive-cap metadata: counted jumps=0 expected=2 status=0 expected-over=1
  FAIL: DL482 positive-cap strict pin: two subsurface jumps exceed translucent cap one
  FAIL: DL482 exactly one charge per nonlocal jump
  FAIL: DL482 cap two admits exactly two jumps
  FAIL: DL482 light merge prefix includes jump ending at merge vertex
  FAIL: DL482 eye merge counts nonlocal jumps including free angular endpoint
48 passed, 5 failed
```

## green-cap

```text
    RGB diffusion translucent cap 0 PT mean 0.038050  sd 0.000317  se 0.000130  (n=6)
    RGB diffusion translucent cap 0 bdpt mean 0.037679  sd 0.000844  se 0.000344  (n=6)
    RGB diffusion translucent cap 0 vcm mean 0.037988  sd 0.000926  se 0.000378  (n=6)
    RGB diffusion translucent cap 1 PT mean 0.546060  sd 0.001890  se 0.000772  (n=6)
    RGB diffusion translucent cap 1 bdpt mean 0.542184  sd 0.004477  se 0.001828  (n=6)
    RGB diffusion translucent cap 1 vcm mean 0.540994  sd 0.004196  se 0.001713  (n=6)
    RGB diffusion translucent cap 2 PT mean 0.547909  sd 0.001805  se 0.000737  (n=6)
    RGB diffusion translucent cap 2 bdpt mean 0.545200  sd 0.002789  se 0.001138  (n=6)
    RGB diffusion translucent cap 2 vcm mean 0.542572  sd 0.003996  se 0.001631  (n=6)
    RGB random-walk translucent cap 0 PT mean 0.037737  sd 0.000427  se 0.000174  (n=6)
    RGB random-walk translucent cap 0 bdpt mean 0.037580  sd 0.000702  se 0.000287  (n=6)
    RGB random-walk translucent cap 0 vcm mean 0.038123  sd 0.000353  se 0.000144  (n=6)
    RGB random-walk translucent cap 1 PT mean 0.508936  sd 0.001456  se 0.000594  (n=6)
    RGB random-walk translucent cap 1 bdpt mean 0.501357  sd 0.001682  se 0.000687  (n=6)
  FAIL: RGB random-walk translucent cap 1 bdpt: DL482 agrees with PT at 3 combined SE
    RGB random-walk translucent cap 1 vcm mean 0.500846  sd 0.001968  se 0.000803  (n=6)
  FAIL: RGB random-walk translucent cap 1 vcm: DL482 agrees with PT at 3 combined SE
    RGB random-walk translucent cap 2 PT mean 0.508917  sd 0.001530  se 0.000625  (n=6)
    RGB random-walk translucent cap 2 bdpt mean 0.508178  sd 0.000807  se 0.000330  (n=6)
    RGB random-walk translucent cap 2 vcm mean 0.507829  sd 0.001892  se 0.000772  (n=6)
    HWSS diffusion translucent cap 0 PT mean 0.037954  sd 0.000281  se 0.000115  (n=6)
    HWSS diffusion translucent cap 0 bdpt mean 0.038407  sd 0.002852  se 0.001164  (n=6)
    HWSS diffusion translucent cap 0 vcm mean 0.038334  sd 0.003078  se 0.001257  (n=6)
    HWSS diffusion translucent cap 1 PT mean 0.543427  sd 0.002146  se 0.000876  (n=6)
    HWSS diffusion translucent cap 1 bdpt mean 0.542056  sd 0.005861  se 0.002393  (n=6)
    HWSS diffusion translucent cap 1 vcm mean 0.535798  sd 0.007609  se 0.003106  (n=6)
    HWSS diffusion translucent cap 2 PT mean 0.546120  sd 0.003717  se 0.001517  (n=6)
    HWSS diffusion translucent cap 2 bdpt mean 0.544293  sd 0.006178  se 0.002522  (n=6)
    HWSS diffusion translucent cap 2 vcm mean 0.541968  sd 0.005070  se 0.002070  (n=6)
    HWSS random-walk translucent cap 0 PT mean 0.038249  sd 0.000320  se 0.000131  (n=6)
    HWSS random-walk translucent cap 0 bdpt mean 0.038531  sd 0.002221  se 0.000907  (n=6)
    HWSS random-walk translucent cap 0 vcm mean 0.038154  sd 0.002577  se 0.001052  (n=6)
    HWSS random-walk translucent cap 1 PT mean 0.508065  sd 0.000870  se 0.000355  (n=6)
    HWSS random-walk translucent cap 1 bdpt mean 0.506282  sd 0.004836  se 0.001974  (n=6)
    HWSS random-walk translucent cap 1 vcm mean 0.501337  sd 0.007320  se 0.002988  (n=6)
    HWSS random-walk translucent cap 2 PT mean 0.508817  sd 0.000944  se 0.000385  (n=6)
    HWSS random-walk translucent cap 2 bdpt mean 0.506560  sd 0.005252  se 0.002144  (n=6)
    HWSS random-walk translucent cap 2 vcm mean 0.508635  sd 0.005528  se 0.002257  (n=6)
DL482 positive-cap metadata: counted jumps=2 expected=2 status=1 expected-over=1
51 passed, 2 failed
```

## final-cap

```text
    RGB diffusion translucent cap 0 PT mean 0.038050  sd 0.000317  se 0.000130  (n=6)
    RGB diffusion translucent cap 0 bdpt mean 0.037679  sd 0.000844  se 0.000344  (n=6)
    RGB diffusion translucent cap 0 vcm mean 0.037988  sd 0.000926  se 0.000378  (n=6)
    RGB diffusion translucent cap 1 PT mean 0.542477  sd 0.001227  se 0.000501  (n=6)
    RGB diffusion translucent cap 1 bdpt mean 0.542184  sd 0.004477  se 0.001828  (n=6)
    RGB diffusion translucent cap 1 vcm mean 0.540994  sd 0.004196  se 0.001713  (n=6)
    RGB diffusion translucent cap 2 PT mean 0.546096  sd 0.002742  se 0.001119  (n=6)
    RGB diffusion translucent cap 2 bdpt mean 0.545200  sd 0.002789  se 0.001138  (n=6)
    RGB diffusion translucent cap 2 vcm mean 0.542572  sd 0.003996  se 0.001631  (n=6)
    RGB random-walk translucent cap 0 PT mean 0.037737  sd 0.000427  se 0.000174  (n=6)
    RGB random-walk translucent cap 0 bdpt mean 0.037580  sd 0.000702  se 0.000287  (n=6)
    RGB random-walk translucent cap 0 vcm mean 0.038123  sd 0.000353  se 0.000144  (n=6)
    RGB random-walk translucent cap 1 PT mean 0.500925  sd 0.000941  se 0.000384  (n=6)
    RGB random-walk translucent cap 1 bdpt mean 0.501357  sd 0.001682  se 0.000687  (n=6)
    RGB random-walk translucent cap 1 vcm mean 0.500846  sd 0.001968  se 0.000803  (n=6)
    RGB random-walk translucent cap 2 PT mean 0.507753  sd 0.001805  se 0.000737  (n=6)
    RGB random-walk translucent cap 2 bdpt mean 0.508178  sd 0.000807  se 0.000330  (n=6)
    RGB random-walk translucent cap 2 vcm mean 0.507829  sd 0.001892  se 0.000772  (n=6)
    HWSS diffusion translucent cap 0 PT mean 0.037954  sd 0.000281  se 0.000115  (n=6)
    HWSS diffusion translucent cap 0 bdpt mean 0.038407  sd 0.002852  se 0.001164  (n=6)
    HWSS diffusion translucent cap 0 vcm mean 0.038334  sd 0.003078  se 0.001257  (n=6)
    HWSS diffusion translucent cap 1 PT mean 0.539942  sd 0.002659  se 0.001086  (n=6)
    HWSS diffusion translucent cap 1 bdpt mean 0.542056  sd 0.005861  se 0.002393  (n=6)
    HWSS diffusion translucent cap 1 vcm mean 0.535798  sd 0.007609  se 0.003106  (n=6)
    HWSS diffusion translucent cap 2 PT mean 0.545224  sd 0.003455  se 0.001411  (n=6)
    HWSS diffusion translucent cap 2 bdpt mean 0.544293  sd 0.006178  se 0.002522  (n=6)
    HWSS diffusion translucent cap 2 vcm mean 0.541968  sd 0.005070  se 0.002070  (n=6)
    HWSS random-walk translucent cap 0 PT mean 0.038249  sd 0.000320  se 0.000131  (n=6)
    HWSS random-walk translucent cap 0 bdpt mean 0.038531  sd 0.002221  se 0.000907  (n=6)
    HWSS random-walk translucent cap 0 vcm mean 0.038154  sd 0.002577  se 0.001052  (n=6)
    HWSS random-walk translucent cap 1 PT mean 0.500952  sd 0.000869  se 0.000355  (n=6)
    HWSS random-walk translucent cap 1 bdpt mean 0.506282  sd 0.004836  se 0.001974  (n=6)
    HWSS random-walk translucent cap 1 vcm mean 0.501337  sd 0.007320  se 0.002988  (n=6)
    HWSS random-walk translucent cap 2 PT mean 0.507354  sd 0.001307  se 0.000534  (n=6)
    HWSS random-walk translucent cap 2 bdpt mean 0.506560  sd 0.005252  se 0.002144  (n=6)
    HWSS random-walk translucent cap 2 vcm mean 0.508635  sd 0.005528  se 0.002257  (n=6)
DL482 positive-cap metadata: counted jumps=2 expected=2 status=1 expected-over=1
53 passed, 0 failed
```

Related gates are run separately with their named make targets; their final counts and return codes are recorded below after completion. Row L positive checks remain opt-in via RISE_DL482_POSITIVE_PIN=1; the default retains the zero-cap suite.

Final related gates: SSSCriticalPartitionTest 45078/0; SSSHWSSCompanionTest 23/0; RandomWalkSSSTest full passes. All three named builds and runs returned 0. Library compilation and these targets emitted zero warnings.
