# Song packages

**Correction:** this folder (`assets/songs/`) is not actually read by the
app. Checked `MainComponent::findBundledSongPackage()`
(`src/main/MainComponent.cpp`) and the real lookup path is:

```
build/mode1/<song_slug>/song_package.json
```

e.g. for Bansanka: `build/mode1/bansanka/song_package.json`.

`build/` is entirely gitignored, so — same conclusion as before, just the
right path this time — **none of this is committed to git** and has to be
shared with teammates separately.

## What to actually share

`build/mode1/<song>/` accumulates a lot of one-off experiment files
(`song_package.before_*.json`, `_backup_pre_*/`, alternate vocal-variant
folders from earlier tuning passes, etc.) that don't need to be sent.
Only the manifest plus whatever vocal-audio subfolders it currently
references matter. Check which subfolders a given `song_package.json`
actually points at:

```bash
grep -o '"directory": "[^"]*"' build/mode1/<song>/song_package.json | sort -u
```

For Bansanka as of 2026-07-30, that's:

```
build/mode1/bansanka/song_package.json
build/mode1/bansanka/fine_candidate/phrase_vocals/
build/mode1/bansanka/fine_candidate/micro_vocals/
```

Zip/share just those three, preserving the relative path, and the
teammate drops them into the same `build/mode1/bansanka/...` location
locally. (Re-run the `grep` above if the package gets rebuilt — the
referenced subfolder can change.)

## Manifest fields (top level)

| Field | Required | Meaning |
|---|---|---|
| `song` | yes | Song display name |
| `score_bpm` | yes | Score tempo |
| `base_key_shift` | yes | Default key transposition |
| `micro_phrases` / `phrases` | yes | Array of phrase objects (see below) |
| `chord_timeline` | no | Array of `{ time, chord }` chord events |
| `key_style.available_key_shifts` | no | Key shifts selectable in guided mode |
| `intro_alignment_offset_sec` | no | Per-song IntroChromaAligner offset override |
| `intro_alignment_preferred_scale` | no | Tie-break bias for intro tempo-scale search |
| `intro_alignment_scale_center` / `..._half_range` | no | Narrows the intro tempo-scale search range |
| `vocal_output_gain` | no | Output-level multiplier for rendered vocal (default 1.0) |
| `vocal_output_delay_sec` | no | Fixed output-side delay applied after tracking (default 0.0) |

Each phrase object carries its own timing/lyrics and a `file` plus a
`directory` (and optional `vocal_variants`) pointing at a WAV under
`build/mode1/<song>/`. See `SongPackage.cpp` for the authoritative
parsing logic if a field here goes stale.
