# Performance summary

Warmup excluded: 5.00 s. Observed window: 15.04 s.

| Metric | Samples | Mean | p95 | Max |
| --- | ---: | ---: | ---: | ---: |
| process CPU (%; 100 = one core) | 15 | 118.01 | 123.70 | 123.70 |
| process RSS (MiB) | 15 | 108.64 | 108.67 | 108.67 |
| CPU temperature (C) | 15 | 60.15 | 60.86 | 60.86 |
| OS thread count | 15 | 11.67 | 12.00 | 12.00 |

| Event | Count | Events/s |
| --- | ---: | ---: |
| stream_encoded | 451 | 29.99 |
| stream_input | 451 | 29.99 |
| trial_end | 1 | 0.07 |

| Thread ID | Samples | Mean CPU (%) | p95 | Max |
| --- | ---: | ---: | ---: | ---: |
| 175313 | 15 | 0.07 | 1.00 | 1.00 |
| 175317 | 15 | 45.85 | 48.88 | 48.88 |
| 175318 | 15 | 34.89 | 36.91 | 36.91 |
| 175319 | 15 | 8.04 | 8.98 | 8.98 |
| 175320 | 15 | 8.24 | 8.98 | 8.98 |
| 175321 | 15 | 8.51 | 8.98 | 8.98 |
| 175322 | 15 | 8.31 | 8.98 | 8.98 |
| 175323 | 15 | 0.93 | 2.00 | 2.00 |
| 175324 | 15 | 1.00 | 1.99 | 1.99 |
| 175325 | 15 | 1.06 | 1.99 | 1.99 |
| 175326 | 15 | 1.06 | 2.00 | 2.00 |
| 175327 | 10 | 0.00 | 0.00 | 0.00 |

Observed thread context-switch deltas: voluntary=13033, involuntary=13433.

These measurements exclude Qt playback and physical servo arrival.
