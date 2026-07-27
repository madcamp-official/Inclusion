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
  --output-dir build\mode1\<song>\style_adaptation
```

The tool selects `base_key_shift`, suppresses most source micro-pitch
expression, adds the measured user vibrato to sustained regions, compresses
the energy curve, attenuates notes above the user's safe range, and writes
range warnings.

Convert the prepared input using the existing GPU server:

```powershell
python tools\mode1_song_package\run_rvc_remote.py `
  --host <username@vpn-host> `
  --input build\mode1\<song>\style_adaptation\rvc_input_style_adapted.wav `
  --output build\mode1\<song>\style_adaptation\style_rvc_user.wav
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
