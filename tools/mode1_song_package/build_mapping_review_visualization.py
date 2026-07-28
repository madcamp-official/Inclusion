#!/usr/bin/env python3
"""Build a synchronized source/app/chord/TAB mapping review fragment."""

from __future__ import annotations

import argparse
import json
import re
from collections import Counter
from pathlib import Path


def event_rows(path: Path) -> list[tuple[float, int]]:
    pattern = re.compile(r"^([0-9.]+),(\d+),")
    rows = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines()[1:]:
        match = pattern.match(line)
        if match:
            rows.append((float(match.group(1)), int(match.group(2))))
    return rows


def tracking_rows(path: Path) -> list[dict]:
    pattern = re.compile(r"^([0-9.]+),(\d+),([0-9.]+),(\d+),(\d+)")
    rows = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines()[1:]:
        match = pattern.match(line)
        if match:
            rows.append({
                "t": float(match.group(1)),
                "i": int(match.group(2)),
                "tempo": float(match.group(3)),
                "expired": int(match.group(5)),
            })
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--render-directory", type=Path, required=True)
    parser.add_argument("--original-audio", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    package = json.loads(
        args.package.read_text(encoding="utf-8"), strict=False
    )
    micro = package["micro_phrases"]
    phrases = package["phrases"]
    chords = package["chord_timeline"]
    actual_events = event_rows(args.render_directory / "phrase_events.csv")
    actual_by_index = {index: time for time, index in actual_events}
    tracking = tracking_rows(
        args.render_directory / "score_tracking_events.csv"
    )
    origin = (
        actual_events[0][0]
        - float(micro[actual_events[0][1]]["source"]["start_sec"])
    )

    lines = []
    for parent, phrase in enumerate(phrases):
        ids = [
            index for index, item in enumerate(micro)
            if int(item.get("parent_line_idx", -1)) == parent
            and index in actual_by_index
        ]
        if not ids:
            continue
        actual_start = actual_by_index[ids[0]]
        last = ids[-1]
        actual_end = actual_by_index[last] + max(
            0.08,
            float(micro[last]["source"]["end_sec"])
            - float(micro[last]["source"]["start_sec"]),
        )
        source_start = float(phrase["source"]["start_sec"])
        source_end = float(phrase["source"]["end_sec"])
        anchor_counts = Counter(
            max(
                (
                    index for index, chord in enumerate(chords)
                    if float(chord["start_sec"])
                    <= float(micro[micro_index]["source"]["start_sec"])
                ),
                default=-1,
            )
            for micro_index in ids
        )
        dominant_chord = anchor_counts.most_common(1)[0][0]
        lines.append({
            "parent": parent,
            "text": phrase.get("lyrics", ""),
            "expectedStart": round(source_start + origin, 3),
            "expectedEnd": round(source_end + origin, 3),
            "actualStart": round(actual_start, 3),
            "actualEnd": round(actual_end, 3),
            "delta": round(actual_start - source_start - origin, 3),
            "chord": dominant_chord,
        })

    track_items = []
    tracked_indices = {row["i"] for row in tracking}
    for position, row in enumerate(tracking):
        end = (
            tracking[position + 1]["t"]
            if position + 1 < len(tracking)
            else row["t"] + 1.0
        )
        chord = chords[row["i"]]
        track_items.append({
            "start": round(row["t"], 3),
            "end": round(end, 3),
            "index": row["i"],
            "name": chord.get("chord", "N"),
            "tempo": round(row["tempo"], 3),
        })

    score_chords = []
    for index, chord in enumerate(chords):
        start = float(chord["start_sec"]) + origin
        end = (
            float(chords[index + 1]["start_sec"]) + origin
            if index + 1 < len(chords)
            else start + 1.0
        )
        score_chords.append({
            "start": round(start, 3),
            "end": round(end, 3),
            "index": index,
            "name": chord.get("chord", "N"),
            "missed": (
                chord.get("chord", "N") not in ("N", "N.C.")
                and index not in tracked_indices
                and index <= max(tracked_indices, default=-1)
            ),
        })

    hints = {
        int(hint["chord_event_index"]): hint
        for hint in package.get("tab_tracking", {}).get("chord_hints", [])
    }
    tab_items = []
    actual_track_by_index = {item["index"]: item for item in track_items}
    for chord_index, hint in hints.items():
        track = actual_track_by_index.get(chord_index)
        if not hint:
            continue
        source_chord = float(chords[chord_index]["start_sec"])
        source_length = max(
            0.01,
            (
                float(chords[chord_index + 1]["start_sec"])
                if chord_index + 1 < len(chords)
                else source_chord + 1.0
            ) - source_chord,
        )
        for event in hint.get("events", []):
            offset = float(event.get("offset_sec", 0.0))
            expected = source_chord + origin + offset
            actual = None
            if track:
                performance_length = track["end"] - track["start"]
                actual = (
                    track["start"]
                    + offset / source_length * performance_length
                )
            notes = event.get("notes", [])
            tab_items.append({
                "t": round(expected, 3),
                "actual": round(actual, 3) if actual is not None else None,
                "chord": chord_index,
                "missed": track is None,
                "measure": int(event.get("measure", 0)),
                "notes": " ".join(
                    f"{note.get('string', '?')}현{note.get('fret', '?')}프렛"
                    for note in notes
                ),
                "midi": [int(note.get("midi", 0)) for note in notes],
            })

    app_audio = (
        args.render_directory / "guitar_plus_app_vocals.wav"
    ).resolve().as_uri()
    source_audio = args.original_audio.resolve().as_uri()
    data = json.dumps({
        "origin": round(origin, 3),
        "duration": 224.748,
        "key": int(package["base_key_shift"]),
        "keyDelta": 5,
        "strength": int(
            package["expression_style"]["default_strength"]
        ),
        "lines": lines,
        "scoreChords": score_chords,
        "tracks": track_items,
        "tabs": tab_items,
    }, ensure_ascii=False, separators=(",", ":"))

    fragment = f"""
<div id="mapping-review">
  <div class="viz-grid">
    <label class="form-label">앱 출력 (+5키 · 표현 100%)
      <audio id="map-app-audio" controls preload="metadata" src="{app_audio}" style="width:100%"></audio>
    </label>
    <label class="form-label">원곡 같은 악보 위치
      <audio id="map-source-audio" controls preload="metadata" src="{source_audio}" style="width:100%"></audio>
    </label>
  </div>
  <div class="viz-row text-small" aria-live="polite">
    <span>출력 <strong id="map-time">0:00.00</strong></span>
    <span>원곡 <strong id="map-source-time">0:00.00</strong></span>
    <span>마디 <strong id="map-measure">-</strong></span>
    <span>코드 <strong id="map-chord">-</strong></span>
    <span>보컬 키 <strong>−7 (기존 대비 +5)</strong></span>
    <span>표현 <strong>100%</strong></span>
  </div>
  <div id="map-window" role="img"
       aria-label="현재 재생 위치 주변의 원곡 가사, 앱 가사, 코드, TAB 음표 매핑">
    <div class="map-ruler" id="map-ruler"></div>
    <div class="map-lane"><span class="map-label">원곡 가사</span><div class="map-track" id="map-expected"></div></div>
    <div class="map-lane"><span class="map-label">앱 가사</span><div class="map-track" id="map-actual"></div></div>
    <div class="map-lane"><span class="map-label">악보 코드</span><div class="map-track" id="map-score-chords"></div></div>
    <div class="map-lane"><span class="map-label">인식 코드</span><div class="map-track" id="map-chords"></div></div>
    <div class="map-lane"><span class="map-label">TAB 음표</span><div class="map-track" id="map-tabs"></div></div>
    <div class="map-center" aria-hidden="true"></div>
  </div>
  <div class="card text-small" id="map-detail">블록을 선택하면 매핑 상세가 표시됩니다.</div>
  <div class="viz-controls">
    <label class="form-label">표시 범위 <span id="map-span-value">16초</span>
      <input id="map-span" class="form-range" type="range" min="8" max="32" step="4" value="16">
    </label>
    <button class="btn" type="button" id="map-first">키미오… → 하야쿠</button>
    <button class="btn" type="button" id="map-verse2">2절 시작</button>
    <button class="btn" type="button" id="map-late">후반 밀림 시작</button>
  </div>
</div>
<style>
#mapping-review {{ display:grid; gap:12px; color:var(--foreground); }}
#map-window {{ position:relative; display:grid; gap:8px; padding:26px 0 4px; }}
.map-lane {{ display:grid; grid-template-columns:74px 1fr; gap:8px; align-items:center; }}
.map-label {{ color:var(--muted-foreground); text-align:right; }}
.map-track {{ position:relative; height:44px; background:color-mix(in srgb,var(--muted) 35%,transparent); overflow:hidden; }}
.map-block {{ position:absolute; top:4px; height:36px; border:0; padding:3px 6px; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; color:var(--foreground); background:color-mix(in srgb,var(--viz-series-1) 22%,var(--card)); }}
#map-actual .map-block {{ background:color-mix(in srgb,var(--viz-series-2) 24%,var(--card)); }}
#map-chords .map-block {{ background:color-mix(in srgb,var(--viz-series-3) 24%,var(--card)); }}
#map-score-chords .map-block {{ background:color-mix(in srgb,var(--viz-series-5) 24%,var(--card)); }}
#map-tabs .map-block {{ background:color-mix(in srgb,var(--viz-series-4) 24%,var(--card)); min-width:5px; padding:0; }}
.map-block.is-missed {{ background:color-mix(in srgb,var(--destructive) 28%,var(--card)) !important; }}
.map-ruler {{ position:absolute; left:82px; right:0; top:0; display:flex; justify-content:space-between; color:var(--muted-foreground); }}
.map-center {{ position:absolute; left:calc(82px + (100% - 82px)/2); top:20px; bottom:0; width:2px; background:var(--primary); pointer-events:none; }}
#map-detail {{ padding:10px 12px; }}
@media(max-width:520px) {{ .map-lane {{ grid-template-columns:58px 1fr; }} .map-center {{ left:calc(66px + (100% - 66px)/2); }} .map-ruler {{ left:66px; }} }}
</style>
<script>
(() => {{
  const d={data}, root=document.getElementById('mapping-review');
  const app=root.querySelector('#map-app-audio'), source=root.querySelector('#map-source-audio');
  let active=app, span=16;
  const fmt=t=>`${{Math.floor(Math.max(0,t)/60)}}:${{(Math.max(0,t)%60).toFixed(2).padStart(5,'0')}}`;
  const outputTime=()=>active===app?(app.currentTime||0):(source.currentTime||0)+d.origin;
  const seek=t=>{{app.currentTime=Math.max(0,t);source.currentTime=Math.max(0,t-d.origin);render();}};
  const block=(parent,left,width,text,detail,missed=false)=>{{
    const b=document.createElement('button');b.type='button';b.className='map-block';
    if(missed)b.classList.add('is-missed');
    b.style.left=left+'%';b.style.width=Math.max(.55,width)+'%';b.textContent=text;
    b.setAttribute('aria-label',detail);b.addEventListener('click',()=>root.querySelector('#map-detail').textContent=detail);
    parent.appendChild(b);
  }};
  const render=()=>{{
    const t=outputTime(), lo=t-span/2, hi=t+span/2, pct=v=>(v-lo)/span*100;
    root.querySelector('#map-time').textContent=fmt(t);
    root.querySelector('#map-source-time').textContent=fmt(t-d.origin);
    root.querySelector('#map-span-value').textContent=span+'초';
    const ids=['map-expected','map-actual','map-score-chords','map-chords','map-tabs'];
    ids.forEach(id=>root.querySelector('#'+id).replaceChildren());
    d.lines.filter(x=>x.expectedEnd>=lo&&x.expectedStart<=hi).forEach(x=>block(
      root.querySelector('#map-expected'),pct(x.expectedStart),pct(x.expectedEnd)-pct(x.expectedStart),
      x.text,`원곡 가사 #${{x.parent}} · ${{fmt(x.expectedStart-d.origin)}} · 앱 시작 오차 ${{x.delta>=0?'+':''}}${{x.delta.toFixed(3)}}초`
    ));
    d.lines.filter(x=>x.actualEnd>=lo&&x.actualStart<=hi).forEach(x=>block(
      root.querySelector('#map-actual'),pct(x.actualStart),pct(x.actualEnd)-pct(x.actualStart),
      x.text,`앱 가사 #${{x.parent}} · 출력 ${{fmt(x.actualStart)}} · 원곡 기준 대비 ${{x.delta>=0?'+':''}}${{x.delta.toFixed(3)}}초`
    ));
    d.scoreChords.filter(x=>x.end>=lo&&x.start<=hi).forEach(x=>block(
      root.querySelector('#map-score-chords'),pct(x.start),pct(x.end)-pct(x.start),x.name,
      `악보 코드 #${{x.index}} · ${{x.name}} · ${{x.missed?'인식 누락':'인식됨'}}`,x.missed
    ));
    d.tracks.filter(x=>x.end>=lo&&x.start<=hi).forEach(x=>block(
      root.querySelector('#map-chords'),pct(x.start),pct(x.end)-pct(x.start),x.name,
      `코드 이벤트 #${{x.index}} · ${{x.name}} · 템포 ${{x.tempo.toFixed(3)}}×`
    ));
    d.tabs.filter(x=>x.t>=lo&&x.t<=hi).forEach(x=>block(
      root.querySelector('#map-tabs'),pct(x.t)-.25,.5,'',
      `TAB 마디 ${{x.measure}} · 코드 #${{x.chord}} · ${{x.notes||'쉼'}} · MIDI [${{x.midi.join(', ')}}] · ${{x.missed?'연결 코드 인식 누락':'연결됨'}}`,x.missed
    ));
    const tr=d.tracks.filter(x=>x.start<=t).at(-1);
    const tab=d.tabs.filter(x=>x.t<=t).at(-1);
    root.querySelector('#map-chord').textContent=tr?tr.name:'-';
    root.querySelector('#map-measure').textContent=tab?tab.measure:'-';
    root.querySelector('#map-ruler').innerHTML=`<span>${{fmt(lo)}}</span><span>${{fmt(t)}}</span><span>${{fmt(hi)}}</span>`;
    if(!active.paused)requestAnimationFrame(render);
  }};
  app.addEventListener('play',()=>{{source.pause();active=app;render();}});
  source.addEventListener('play',()=>{{app.pause();active=source;render();}});
  [app,source].forEach(a=>{{a.addEventListener('timeupdate',render);a.addEventListener('seeked',render);}});
  root.querySelector('#map-span').addEventListener('input',e=>{{span=Number(e.target.value);render();}});
  root.querySelector('#map-first').addEventListener('click',()=>{{seek(16.6);app.play();}});
  root.querySelector('#map-verse2').addEventListener('click',()=>{{seek(77.3);app.play();}});
  root.querySelector('#map-late').addEventListener('click',()=>{{seek(157);app.play();}});
  render();
}})();
</script>
"""
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(fragment.strip() + "\n", encoding="utf-8")
    print(args.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
