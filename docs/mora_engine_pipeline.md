# Real-time mora engine redesign

## Goal and non-negotiable constraints

The singer follows a causal, imperfect live guitar performance. It must not
look ahead in the input, require a metronome, or silently switch to the timing
of a prerecorded reference. The existing phrase engine remains available as
the rollback baseline until all three stages pass.

The test fixture is the user's unedited guitar recording
`output/audio/guitar_tests/bansanka_last_guitar_take.wav`. Every stage also
gets a synthetic full-song regression run so that a change cannot merely
overfit one take.

## Acceptance gates

Each stage is accepted only when all of the following hold:

1. The renderer processes fixed-size blocks causally; no future input samples
   are inspected.
2. The first vocal is not earlier than the established intro guard and stays
   close to the fifth-measure entry of the human take.
3. Phrase target error does not grow with phrase index. In particular, a late
   callback must not move the targets of all following moras.
4. No backlog burst is allowed: at most one new mora is committed per audio
   block and obsolete moras are expired rather than compressed.
5. Intentional source gaps remain silent; they are not filled by extending a
   consonant or by immediately starting the next phrase.
6. The offline renderer remains substantially faster than real time and the
   unit/integration tests pass.
7. A separate guitar+vocal WAV, event log, metrics file, and Git commit are
   produced for the stage.

Subjective timbre cannot be guaranteed numerically. For that reason, the
listening WAV from each accepted stage is retained and the previous stage is
always recoverable.

## Stage 0 — frozen baseline

- Commit the current chord/GP5-assisted engine.
- Render the real guitar fixture with 10 ms blocks.
- Record first-vocal time, event count, expired count, burst count, run time,
  and late-song spacing statistics.

## Stage 1 — independent beat reservations

Problem: the current scheduler checks a mora's score-relative target and then
also checks elapsed time since the previously emitted mora. Any callback
quantisation or late detection therefore becomes the origin for the next
mora, allowing error to accumulate.

Implementation:

- Treat every mora's `source.start_sec` as an immutable musical target inside
  its anchored chord segment.
- Convert that target independently through the current causal tempo estimate.
- Keep the 50 ms anti-burst rule only as a safety collision guard; never use
  the actual previous trigger time to redefine the following target.
- Carry a signed target error to diagnostics.
- Align the audible content onset to the reservation. The clip's
  `content_offset_sec` is pre-roll metadata, not a reason to discard the
  consonant permanently.
- Expire a target when its chord window is passed instead of replaying a
  backlog.

Expected improvement: timing error is bounded by chord-tracking error plus one
audio block instead of accumulating once per mora.

## Stage 2 — attack / sustain / release renderer

Problem: replacing one complete tiny WAV with another cuts vowels short and
makes rapid passages sound as if syllables are being skipped.

Implementation:

- Derive conservative attack, stable-vowel, and release regions for every
  loaded clip. Metadata may override automatic values.
- Preserve attack at normal speed.
- Fit only the stable vowel to the time until the next reserved mora, within
  strict tempo bounds. Never stretch a consonant or breath.
- Preserve or gently crossfade the release when time permits; shorten it
  first when the performer accelerates.
- Keep rests as explicit empty reservations.

Expected improvement: mora onsets stay fixed while their vowel bodies absorb
small timing differences, producing intelligible connected singing.

## Stage 3 — commit window and connection-aware transitions

Problem: chord recognition can revise its best score position. Starting audio
immediately on every tentative answer causes incorrect early vocals, while
waiting for full certainty feels late.

Implementation:

- Maintain several causal score-position candidates in the tracker.
- Reserve future moras from the leading candidate but commit audio only inside
  a short, bounded window.
- A later correction may cancel an uncommitted reservation, never audio that
  has already begun.
- Choose transition length from the boundary type: vowel-to-vowel gets an
  equal-power overlap, consonant or rest boundaries get a shorter fade, and
  explicit rests never overlap.
- Log reservation, commit, cancellation, expiry, and onset-error events.

Expected improvement: fewer false starts without adding a full beat of
latency, and smoother joins that do not erase written rests.

## Deliverables

Each directory `output/audio/mora_pipeline/stage_N_*` contains:

- `guitar_plus_app_vocals.wav`
- `app_vocals_from_guitar.wav`
- `phrase_events.csv`
- `score_tracking_events.csv`
- `metrics.json`

Each accepted stage is committed separately. A final comparison report names
the recommended default and the exact rollback commit.
