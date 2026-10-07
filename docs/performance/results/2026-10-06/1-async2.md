# Performance summary

Warmup excluded: 5.00 s. Observed window: 15.13 s.

| Metric | Samples | Mean | p95 | Max |
| --- | ---: | ---: | ---: | ---: |
| inference duration (ms) | 54 | 278.28 | 304.48 | 406.59 |
| inference input age (ms) | 54 | 57.70 | 103.00 | 111.00 |
| inference result age (ms) | 54 | 335.98 | 384.00 | 465.00 |
| process CPU (%; 100 = one core) | 15 | 305.15 | 317.45 | 317.45 |
| process RSS (MiB) | 15 | 175.80 | 177.35 | 177.35 |
| CPU temperature (C) | 15 | 69.89 | 71.58 | 71.58 |
| OS thread count | 15 | 14.73 | 15.00 | 15.00 |

| Event | Count | Events/s |
| --- | ---: | ---: |
| detection_result | 55 | 3.64 |
| inference | 54 | 3.57 |
| stream_encoded | 453 | 29.95 |
| stream_input | 454 | 30.02 |
| trial_end | 1 | 0.07 |

| Thread ID | Samples | Mean CPU (%) | p95 | Max |
| --- | ---: | ---: | ---: | ---: |
| 175382 | 15 | 0.07 | 0.98 | 0.98 |
| 175386 | 15 | 8.80 | 8.97 | 8.97 |
| 175387 | 15 | 48.37 | 49.80 | 49.80 |
| 175388 | 15 | 35.86 | 36.66 | 36.66 |
| 175389 | 15 | 95.08 | 100.07 | 100.07 |
| 175390 | 15 | 7.67 | 9.95 | 9.95 |
| 175391 | 15 | 8.60 | 13.93 | 13.93 |
| 175392 | 15 | 8.53 | 10.96 | 10.96 |
| 175393 | 15 | 8.80 | 12.96 | 12.96 |
| 175394 | 15 | 0.73 | 2.95 | 2.95 |
| 175395 | 15 | 1.26 | 3.98 | 3.98 |
| 175396 | 15 | 1.79 | 3.99 | 3.99 |
| 175397 | 15 | 0.66 | 2.99 | 2.99 |
| 175398 | 11 | 0.00 | 0.00 | 0.00 |
| 175399 | 15 | 79.07 | 85.58 | 85.58 |

Observed thread context-switch deltas: voluntary=15854, involuntary=30545.

These measurements exclude Qt playback and physical servo arrival.
