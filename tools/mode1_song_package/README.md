# Mode 1 song package builder

This tool combines four different representations of a song:

- score-timed lyrics and notes;
- a source-audio-timed chord sequence;
- the synthesized guide vocal used by the score;
- the isolated original vocal and its RVC-converted counterpart.

It aligns the guide and original vocal with chroma DTW, transfers the score
line boundaries to original-audio time, and writes `song_package.json` plus
phrase-level converted vocal WAV files.

The Python environment must provide `librosa`, `numpy`, and `soundfile`.

```powershell
python build_song_package.py `
  --score-json bansanka_alignment.json `
  --chords-json bansanka_chords_audio_aligned.json `
  --guide-wav bansanka_guide.wav `
  --source-vocal-wav bansanka_original_vocal.wav `
  --converted-vocal-wav bansanka_rvc.wav `
  --output-dir build/mode1/bansanka
```

## Playback segmentation granularity

Each guitar strum advances playback by one micro-phrase, so the micro-phrase
length decides how responsive Mode 1 feels and where the audio can be cut.
By default consecutive score notes are grouped into roughly 0.55-1.20 s
spans. Since one score note is about one mora, `--mora-granularity` instead
emits one micro-phrase per mora -- the finest split the score data supports:

```powershell
python tools/mode1_song_package/build_song_package.py `
  ... `
  --mora-granularity
```

For the current 만찬가 package that is 558 spans instead of 198. Finer spans
mean strums land on syllable boundaries rather than partway through a
syllable, at the cost of more splice points and more WAV files. The grouping
thresholds can also be tuned directly with `--micro-minimum-sec`,
`--micro-maximum-sec`, and `--micro-merge-tail-below-sec`.

Rebuilding the package rewrites `song_package.json`, so re-apply any
`lyrics_reading_ko` fields afterwards with `apply_lyrics_reading.py` and
re-attach key anchors with `add_key_anchor.py`.

## Chord cleanup and offline validation

Normalize detector-specific chord labels and remove only true duplicate/flicker
events:

```powershell
python tools/mode1_song_package/chord_cleanup.py `
  build/mode1/bansanka/song_package.json --in-place
```

The command preserves `song_package.raw.json` as a backup. To validate Mode 1
without a guitar, render normal, +2-semitone, and one-mistake scenarios:

```powershell
& .\external\seed-vc\venv\Scripts\python.exe `
  tools/mode1_song_package/simulate_mode1.py `
  build/mode1/bansanka/song_package.json
```

Outputs are written under `build/mode1/bansanka/simulation`.

## User-style RVC guide pipeline

Create a reusable user range/style profile and a WORLD-resynthesized RVC input:

```powershell
& .\external\seed-vc\venv\Scripts\python.exe -m pip install `
  -r tools\mode1_song_package\requirements.txt

& .\external\seed-vc\venv\Scripts\python.exe `
  tools\mode1_song_package\prepare_rvc_guide.py `
  --user-voice <user.wav> `
  --source-vocal <isolated-vocal.wav> `
  --output-dir build\mode1\<song>\style_adaptation `
  --style-strengths 0,25,50,75,100
```

The tool selects `base_key_shift`, suppresses most source micro-pitch
expression, adds the measured user vibrato to sustained regions, compresses
the energy curve, attenuates notes above the user's safe range, and writes
range warnings.

The approved user-style rendering is strength `25`, which is also the app
default. Strength `100` retains source expression while keeping the same
comfortable base key.

Convert the prepared input using the existing GPU server:

```powershell
python tools\mode1_song_package\run_rvc_remote.py `
  --host <username@vpn-host> `
  --input build\mode1\<song>\style_adaptation\rvc_input_style_adapted.wav `
  --output build\mode1\<song>\style_adaptation\style_rvc_user.wav
```

For all slider variants, run:

```powershell
python tools\mode1_song_package\run_rvc_variants_remote.py `
  --host <username@vpn-host> `
  --manifest build\mode1\<song>\style_adaptation\style_variants.json

& .\external\seed-vc\venv\Scripts\python.exe `
  tools\mode1_song_package\add_style_variants.py `
  --package build\mode1\<song>\song_package.json `
  --manifest build\mode1\<song>\style_adaptation\style_variants.json
```

Pass `style_plan.json` to the package builder. This embeds the base key and
transposes expected guitar chords by the same amount, preventing double
transposition:

```powershell
& .\external\seed-vc\venv\Scripts\python.exe `
  tools\mode1_song_package\build_song_package.py `
  ... `
  --converted-vocal-wav build\mode1\<song>\style_adaptation\style_rvc_user.wav `
  --style-plan-json build\mode1\<song>\style_adaptation\style_plan.json
```

## Pre-rendered key anchors

Large real-time pitch shifts degrade vocal quality. Prepare a small set of
absolute key anchors (for example `-17,-12,-6,0`) with the same expression
strengths, convert each manifest on the GPU server, and attach it:

```powershell
& .\external\seed-vc\venv\Scripts\python.exe `
  tools\mode1_song_package\prepare_rvc_guide.py `
  --user-voice <user.wav> `
  --source-vocal <isolated-vocal.wav> `
  --output-dir build\mode1\<song>\key_anchors\m012 `
  --key-shift -12 `
  --style-strengths 0,25,50,75,100

python tools\mode1_song_package\run_rvc_variants_remote.py `
  --host <username@vpn-host> `
  --manifest build\mode1\<song>\key_anchors\m012\style_variants.json

& .\external\seed-vc\venv\Scripts\python.exe `
  tools\mode1_song_package\resample_manifest_audio.py `
  --manifest build\mode1\<song>\key_anchors\m012\style_variants.json `
  --sample-rate 48000

& .\external\seed-vc\venv\Scripts\python.exe `
  tools\mode1_song_package\add_key_anchor.py `
  --package build\mode1\<song>\song_package.json `
  --manifest build\mode1\<song>\key_anchors\m012\style_variants.json
```

The app loads only the nearest anchor and applies the remaining small pitch
offset in real time. A playing phrase keeps its old audio bank; the new
anchor takes effect from the next phrase.

## Retraining the RVC voice model from a guided-recording session

Recording a new session in the app (see `RecordingSessionScreen`) only saves
WAV clips locally — it does not retrain the RVC voice model by itself. See
`docs/GPU_VOICE_CONVERSION_HANDOFF_2026-07-28.md` for the full background;
this is the scripted version of that document's manual RVC WebUI steps.

```powershell
& .\external\seed-vc\venv\Scripts\python.exe `
  tools\mode1_song_package\prepare_voice_training_dataset.py `
  --output build\mode1\voice_training\combined_user_voice.wav
```

Combines the most recent `voice_profiles/<id>/accepted/*.wav` clips into one
training WAV (pass `--profile-dir` to target a specific session instead of
the latest one).

```powershell
& .\external\seed-vc\venv\Scripts\python.exe `
  tools\mode1_song_package\train_rvc_voice.py `
  --host <username@vpn-host> `
  --voice-wav build\mode1\voice_training\combined_user_voice.wav `
  --experiment-name rvc_user_<date>
```

Runs preprocess -> F0 extraction (RMVPE/GPU) -> HuBERT feature extraction ->
training -> index build on the server, then prints the resulting
`assets/weights/<experiment-name>.pth` and `added_*.index` paths. Add
`--dry-run` to print the commands without running them, and
`--skip-preprocess` / `--skip-extract` / `--skip-train` / `--skip-index` to
resume after a failed step instead of restarting from scratch. Training
takes a few minutes on an RTX 3090 for a ~3 minute recording (see the
handoff doc for reference timings).

Once you have a `.pth` and index you're happy with, point
`run_rvc_remote.py` / `run_rvc_variants_remote.py` at them with `--model`
and `--index` to use the retrained voice for any song's conversion step.
