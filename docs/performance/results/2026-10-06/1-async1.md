# Performance summary

Warmup excluded: 5.00 s. Observed window: 15.20 s.

| Metric | Samples | Mean | p95 | Max |
| --- | ---: | ---: | ---: | ---: |
| inference duration (ms) | 34 | 435.19 | 451.60 | 463.49 |
| inference input age (ms) | 34 | 61.79 | 111.00 | 117.00 |
| inference result age (ms) | 34 | 496.94 | 549.00 | 553.00 |
| process CPU (%; 100 = one core) | 15 | 226.45 | 228.17 | 228.17 |
| process RSS (MiB) | 15 | 176.84 | 177.48 | 177.48 |
| CPU temperature (C) | 15 | 64.72 | 66.22 | 66.22 |
| OS thread count | 15 | 13.73 | 14.00 | 14.00 |

| Event | Count | Events/s |
| --- | ---: | ---: |
| detection_result | 35 | 2.30 |
| inference | 34 | 2.24 |
| stream_encoded | 455 | 29.94 |
| stream_input | 456 | 30.01 |
| trial_end | 1 | 0.07 |

| Thread ID | Samples | Mean CPU (%) | p95 | Max |
| --- | ---: | ---: | ---: | ---: |
| 175345 | 15 | 0.13 | 1.00 | 1.00 |
| 175354 | 15 | 8.63 | 8.97 | 8.97 |
| 175355 | 15 | 46.42 | 47.85 | 47.85 |
| 175356 | 15 | 35.59 | 35.90 | 35.90 |
| 175357 | 15 | 99.41 | 100.63 | 100.63 |
| 175358 | 15 | 8.30 | 9.97 | 9.97 |
| 175359 | 15 | 7.70 | 8.97 | 8.97 |
| 175360 | 15 | 8.17 | 8.97 | 8.97 |
| 175361 | 15 | 7.77 | 9.97 | 9.97 |
| 175362 | 15 | 0.40 | 1.00 | 1.00 |
| 175363 | 15 | 1.46 | 3.98 | 3.98 |
| 175364 | 15 | 1.13 | 2.99 | 2.99 |
| 175365 | 15 | 1.26 | 3.99 | 3.99 |
| 175366 | 11 | 0.00 | 0.00 | 0.00 |

Observed thread context-switch deltas: voluntary=16382, involuntary=21416.

These measurements exclude Qt playback and physical servo arrival.
