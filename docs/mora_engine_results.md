# Mora engine stage results

Test date: 2026-07-29  
Audio block: 480 samples at 48 kHz (10 ms)  
Primary input: unedited human guitar take, 61.58 s

| Metric | Stage 0 | Stage 1 | Stage 2 | Stage 3 |
|---|---:|---:|---:|---:|
| First vocal | 12.56 s | 12.56 s | 12.56 s | 12.57 s |
| Emitted moras | 168 | 172 | 172 | 172 |
| Expired moras | 17 | 13 | 13 | 13 |
| Gaps below 50 ms | 0 | 0 | 0 | 0 |
| Median late-third gap | 0.31 s | 0.30 s | 0.30 s | 0.30 s |
| Real-time factor | 284x | 182x | 208x | 253x |

Stage 1 is an objective improvement over the frozen baseline: it recovers four
more moras without moving the first entry or introducing backlog bursts.
Stages 2 and 3 intentionally preserve those timing results while changing the
audio joins and duration handling.

The final full-song synthetic-guitar regression covers 224.95 seconds:

- 536 emitted moras
- 22 expired moras
- 0 gaps below 50 ms
- first vocal at 12.60 s
- last vocal at 211.44 s
- all 182 chord events reached
- 209x faster than real time

## Commits and rollback

- Baseline plan and metric tooling: `79acb43`
- Stage 1 independent mora clock: `05a9267`
- Stage 2 bounded attack/sustain/release: `4b6b202`
- Stage 3 commit window and rest-aware transitions: `31ffad8`

The pre-redesign audio engine is retained at `5e487c4`. If listening reveals a
subjective regression, that commit is the exact rollback point; stage results
remain available for A/B diagnosis.

## Interpretation

These numbers demonstrate bounded scheduling, fewer dropped moras, preserved
intro timing, complete long-run traversal, and ample CPU headroom. They do not
prove that every vocal join sounds better to a listener. The stage WAVs are
therefore the final acceptance artifact, with Stage 1 isolating scheduling,
Stage 2 isolating elastic vowel playback, and Stage 3 isolating commit and
connection behaviour.
