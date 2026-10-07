# Performance summary

Warmup excluded: 5.00 s. Observed window: 15.25 s.

| Metric | Samples | Mean | p95 | Max |
| --- | ---: | ---: | ---: | ---: |
| inference duration (ms) | 47 | 321.24 | 359.44 | 372.47 |
| inference input age (ms) | 47 | 59.19 | 99.00 | 108.00 |
| inference result age (ms) | 47 | 380.40 | 439.00 | 449.00 |
| process CPU (%; 100 = one core) | 15 | 308.76 | 323.51 | 323.51 |
| process RSS (MiB) | 15 | 182.29 | 182.70 | 182.70 |
| CPU temperature (C) | 15 | 72.94 | 74.50 | 74.50 |
| OS thread count | 15 | 15.73 | 16.00 | 16.00 |

| Event | Count | Events/s |
| --- | ---: | ---: |
| detection_result | 48 | 3.15 |
| inference | 47 | 3.08 |
| stream_encoded | 457 | 29.97 |
| stream_input | 458 | 30.04 |
| trial_end | 1 | 0.07 |

| Thread ID | Samples | Mean CPU (%) | p95 | Max |
| --- | ---: | ---: | ---: | ---: |
| 175411 | 15 | 0.07 | 1.00 | 1.00 |
| 175413 | 15 | 8.56 | 8.97 | 8.97 |
| 175414 | 15 | 46.64 | 51.84 | 51.84 |
| 175415 | 15 | 35.69 | 36.87 | 36.87 |
| 175416 | 15 | 84.38 | 91.71 | 91.71 |
| 175417 | 15 | 8.62 | 13.94 | 13.94 |
| 175418 | 15 | 9.29 | 13.94 | 13.94 |
| 175419 | 15 | 7.23 | 9.95 | 9.95 |
| 175420 | 15 | 7.83 | 11.96 | 11.96 |
| 175421 | 15 | 0.80 | 3.99 | 3.99 |
| 175422 | 15 | 1.06 | 3.96 | 3.96 |
| 175423 | 15 | 1.59 | 3.98 | 3.98 |
| 175424 | 15 | 0.66 | 3.98 | 3.98 |
| 175425 | 11 | 0.00 | 0.00 | 0.00 |
| 175426 | 15 | 48.30 | 54.78 | 54.78 |
| 175427 | 15 | 47.97 | 53.82 | 53.82 |

Observed thread context-switch deltas: voluntary=16587, involuntary=39913.

These measurements exclude Qt playback and physical servo arrival.
