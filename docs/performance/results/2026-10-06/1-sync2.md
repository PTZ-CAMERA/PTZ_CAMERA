# Performance summary

Warmup excluded: 5.00 s. Observed window: 15.29 s.

| Metric | Samples | Mean | p95 | Max |
| --- | ---: | ---: | ---: | ---: |
| inference duration (ms) | 55 | 239.72 | 250.64 | 259.30 |
| inference input age (ms) | 55 | 8115.49 | 12463.00 | 12835.00 |
| inference result age (ms) | 55 | 8356.76 | 12712.00 | 13087.00 |
| process CPU (%; 100 = one core) | 15 | 210.03 | 215.22 | 215.22 |
| process RSS (MiB) | 15 | 175.81 | 175.89 | 175.89 |
| CPU temperature (C) | 15 | 72.84 | 73.52 | 73.52 |
| OS thread count | 15 | 12.73 | 13.00 | 13.00 |

| Event | Count | Events/s |
| --- | ---: | ---: |
| detection_result | 56 | 3.66 |
| inference | 55 | 3.60 |
| stale_result | 56 | 3.66 |
| stream_encoded | 165 | 10.79 |
| stream_input | 165 | 10.79 |
| trial_end | 1 | 0.07 |

| Thread ID | Samples | Mean CPU (%) | p95 | Max |
| --- | ---: | ---: | ---: | ---: |
| 175442 | 15 | 0.13 | 1.00 | 1.00 |
| 175444 | 15 | 17.91 | 19.95 | 19.95 |
| 175445 | 15 | 99.08 | 100.72 | 100.72 |
| 175446 | 15 | 3.12 | 4.99 | 4.99 |
| 175447 | 15 | 2.59 | 3.99 | 3.99 |
| 175448 | 15 | 3.12 | 5.98 | 5.98 |
| 175449 | 15 | 2.98 | 4.98 | 4.98 |
| 175450 | 15 | 0.40 | 1.99 | 1.99 |
| 175451 | 15 | 0.40 | 1.00 | 1.00 |
| 175452 | 15 | 0.53 | 1.99 | 1.99 |
| 175453 | 15 | 0.20 | 1.00 | 1.00 |
| 175454 | 11 | 0.00 | 0.00 | 0.00 |
| 175455 | 15 | 79.45 | 83.50 | 83.50 |

Observed thread context-switch deltas: voluntary=6174, involuntary=7633.

These measurements exclude Qt playback and physical servo arrival.
