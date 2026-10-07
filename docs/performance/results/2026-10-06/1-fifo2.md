# Performance summary

Warmup excluded: 5.00 s. Observed window: 15.14 s.

| Metric | Samples | Mean | p95 | Max |
| --- | ---: | ---: | ---: | ---: |
| inference duration (ms) | 63 | 238.77 | 246.91 | 337.48 |
| inference input age (ms) | 63 | 7459.22 | 11280.00 | 11697.00 |
| inference result age (ms) | 63 | 7698.00 | 11513.00 | 12035.00 |
| process CPU (%; 100 = one core) | 15 | 243.82 | 253.19 | 253.19 |
| process RSS (MiB) | 15 | 207.10 | 207.13 | 207.13 |
| CPU temperature (C) | 15 | 74.07 | 75.47 | 75.47 |
| OS thread count | 15 | 14.73 | 15.00 | 15.00 |

| Event | Count | Events/s |
| --- | ---: | ---: |
| detection_result | 64 | 4.23 |
| inference | 63 | 4.16 |
| stale_result | 64 | 4.23 |
| stream_encoded | 189 | 12.48 |
| stream_input | 189 | 12.48 |
| trial_end | 1 | 0.07 |

| Thread ID | Samples | Mean CPU (%) | p95 | Max |
| --- | ---: | ---: | ---: | ---: |
| 175475 | 15 | 0.07 | 0.99 | 0.99 |
| 175477 | 15 | 3.84 | 4.97 | 4.97 |
| 175478 | 15 | 20.35 | 22.89 | 22.89 |
| 175479 | 15 | 15.44 | 17.92 | 17.92 |
| 175480 | 15 | 97.52 | 99.64 | 99.64 |
| 175481 | 15 | 3.78 | 4.98 | 4.98 |
| 175482 | 15 | 3.51 | 4.98 | 4.98 |
| 175483 | 15 | 3.51 | 3.99 | 3.99 |
| 175484 | 15 | 3.25 | 4.98 | 4.98 |
| 175485 | 15 | 0.53 | 1.99 | 1.99 |
| 175486 | 15 | 0.40 | 1.00 | 1.00 |
| 175487 | 15 | 0.53 | 1.00 | 1.00 |
| 175488 | 15 | 0.33 | 1.00 | 1.00 |
| 175489 | 11 | 0.00 | 0.00 | 0.00 |
| 175490 | 15 | 90.69 | 93.65 | 93.65 |

Observed thread context-switch deltas: voluntary=6886, involuntary=13165.

These measurements exclude Qt playback and physical servo arrival.
