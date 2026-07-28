#!/usr/bin/env python3
"""Build a compact synchronized render/timing diagnostic HTML fragment."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

import numpy as np
import soundfile as sf


def parse_event_csv(path: Path) -> list[tuple[float, int]]:
    rows: list[tuple[float, int]] = []
    pattern = re.compile(r"^([0-9.]+),(\d+),")
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines()[1:]:
        match = pattern.match(line)
        if match:
            rows.append((float(match.group(1)), int(match.group(2))))
    return rows


def parse_tracking_csv(path: Path) -> list[dict[str, float | int]]:
    rows: list[dict[str, float | int]] = []
    pattern = re.compile(r"^([0-9.]+),(\d+),([0-9.]+),(\d+),(\d+)")
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines()[1:]:
        match = pattern.match(line)
        if match:
            rows.append({
                "t": round(float(match.group(1)), 3),
                "chord": int(match.group(2)),
                "tempo": round(float(match.group(3)), 4),
                "expired": int(match.group(5)),
            })
    return rows


def waveform(path: Path, bins: int = 900) -> list[float]:
    audio, _ = sf.read(path, always_2d=True, dtype="float32")
    mono = np.max(np.abs(audio), axis=1)
    edges = np.linspace(0, len(mono), bins + 1, dtype=np.int64)
    values = [
        float(np.max(mono[edges[i]:edges[i + 1]]))
        if edges[i + 1] > edges[i] else 0.0
        for i in range(bins)
    ]
    peak = max(values, default=1.0) or 1.0
    return [round(value / peak, 3) for value in values]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--render-directory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    package = json.loads(
        args.package.read_text(encoding="utf-8"), strict=False
    )
    events = parse_event_csv(args.render_directory / "phrase_events.csv")
    tracking = parse_tracking_csv(
        args.render_directory / "score_tracking_events.csv"
    )
    micro = package["micro_phrases"]
    lines = package["phrases"]
    chord_timeline = package["chord_timeline"]
    audio_path = (
        args.render_directory / "guitar_plus_app_vocals.wav"
    ).resolve()

    first_time, first_index = events[0]
    origin = first_time - float(micro[first_index]["source"]["start_sec"])
    event_data = []
    for actual, index in events:
        item = micro[index]
        source = float(item["source"]["start_sec"])
        parent = int(item.get("parent_line_idx", -1))
        event_data.append({
            "t": round(actual, 3),
            "i": index,
            "m": item.get("lyrics", ""),
            "line": lines[parent].get("lyrics", "") if parent >= 0 else "",
            "parent": parent,
            "drift": round(actual - source - origin, 3),
        })

    chord_data = [{
        "i": index,
        "name": item.get("chord", "N"),
        "source": round(float(item.get("start_sec", 0.0)), 3),
    } for index, item in enumerate(chord_timeline)]
    duration = round(sf.info(audio_path).duration, 3)
    data = {
        "duration": duration,
        "events": event_data,
        "tracking": tracking,
        "chords": chord_data,
        "wave": waveform(audio_path),
        "verse2": round(next(
            event["t"] for event in event_data if event["parent"] == 19
        ), 3),
        "focusStart": round(next(
            event["t"] for event in event_data if event["parent"] == 2
        ), 3),
        "focusEnd": round(next(
            event["t"] for event in event_data if event["parent"] == 3
        ), 3),
    }
    encoded = json.dumps(data, ensure_ascii=False, separators=(",", ":"))
    audio_uri = audio_path.as_uri()

    fragment = f"""
<div id="mora-follow-along">
  <audio id="mora-audio" controls preload="metadata" src="{audio_uri}" style="width:100%"></audio>
  <div class="viz-row text-small" aria-live="polite">
    <span><strong id="mora-time">0:00.00</strong> / {int(duration // 60)}:{duration % 60:05.2f}</span>
    <span>마디 <strong id="mora-measure">-</strong></span>
    <span>코드 <strong id="mora-chord">-</strong></span>
    <span>템포 <strong id="mora-tempo">1.000×</strong></span>
    <span>기준 대비 <strong id="mora-drift">0.00초</strong></span>
  </div>
  <div class="card" aria-live="polite">
    <div id="mora-line">가사 시작 전</div>
    <div class="text-muted text-small">현재 모라: <strong id="mora-token">-</strong></div>
  </div>
  <svg id="mora-chart" viewBox="0 0 1000 270" role="img"
       aria-label="전곡 파형과 기준 보컬 대비 출력 시각 오차. 차트를 눌러 재생 위치를 이동할 수 있습니다.">
    <g id="mora-wave"></g>
    <g id="mora-drift-line"></g>
    <g id="mora-axis"></g>
    <line id="mora-playhead" x1="0" x2="0" y1="8" y2="258"></line>
  </svg>
  <div class="viz-row text-small">
    <span><span class="mora-key mora-zero"></span> 기준 시각</span>
    <span><span class="mora-key mora-error"></span> 누적 빠름/느림</span>
    <button type="button" class="btn" id="mora-first-boundary">키미오… → 하야쿠</button>
    <button type="button" class="btn" id="mora-verse2">2절 시작</button>
  </div>
</div>
<style>
#mora-follow-along {{ display:grid; gap:12px; color:var(--foreground); }}
#mora-follow-along .card {{ padding:12px; }}
#mora-line {{ font-weight:500; }}
#mora-chart {{ width:100%; min-height:210px; cursor:pointer; overflow:visible; }}
#mora-chart .wave {{ fill:var(--muted-foreground); opacity:.28; }}
#mora-chart .grid {{ stroke:var(--border); stroke-width:1; }}
#mora-chart .zero {{ stroke:var(--foreground); stroke-width:1; opacity:.55; }}
#mora-chart .drift {{ fill:none; stroke:var(--viz-series-1); stroke-width:2.5; }}
#mora-chart .verse {{ stroke:var(--viz-series-2); stroke-width:2; stroke-dasharray:6 5; }}
#mora-playhead {{ stroke:var(--primary); stroke-width:3; }}
#mora-chart text {{ fill:var(--muted-foreground); }}
.mora-key {{ display:inline-block; width:18px; height:3px; vertical-align:middle; margin-right:5px; }}
.mora-zero {{ background:var(--foreground); opacity:.55; }}
.mora-error {{ background:var(--viz-series-1); }}
</style>
<script>
(() => {{
  const data = {encoded};
  const root = document.getElementById('mora-follow-along');
  const audio = root.querySelector('#mora-audio');
  const svg = root.querySelector('#mora-chart');
  const ns = 'http://www.w3.org/2000/svg';
  const x = t => 42 + (t / data.duration) * 940;
  const y0 = 185;
  const driftScale = 32;
  const el = (tag, attrs, parent) => {{
    const node = document.createElementNS(ns, tag);
    Object.entries(attrs).forEach(([k,v]) => node.setAttribute(k, v));
    parent.appendChild(node); return node;
  }};
  const wave = root.querySelector('#mora-wave');
  data.wave.forEach((v,i) => {{
    const xx = 42 + i / data.wave.length * 940;
    el('rect', {{x:xx.toFixed(2), y:(92-v*60).toFixed(2),
      width:'1.2', height:(v*120).toFixed(2), class:'wave'}}, wave);
  }});
  const axis = root.querySelector('#mora-axis');
  [-2,-1,0,1,2,3].forEach(d => {{
    const yy = y0 - d*driftScale;
    el('line', {{x1:42,x2:982,y1:yy,y2:yy,class:d===0?'zero':'grid'}}, axis);
    const label=el('text', {{x:4,y:yy+4,class:'text-small'}}, axis);
    label.textContent=(d>0?'+':'')+d+'s';
  }});
  for(let t=0;t<=data.duration;t+=30) {{
    const label=el('text', {{x:x(t),y:264,'text-anchor':'middle',class:'text-small'}}, axis);
    label.textContent=Math.floor(t/60)+':'+String(Math.floor(t%60)).padStart(2,'0');
  }}
  const verseX=x(data.verse2);
  el('line', {{x1:verseX,x2:verseX,y1:8,y2:250,class:'verse'}}, axis);
  const verseLabel=el('text', {{x:verseX+5,y:18,class:'text-small'}}, axis);
  verseLabel.textContent='2절';
  const points=data.events.map(e => `${{x(e.t).toFixed(1)}},${{(y0-e.drift*driftScale).toFixed(1)}}`).join(' ');
  el('polyline', {{points,class:'drift'}}, root.querySelector('#mora-drift-line'));

  let eventCursor=0, trackCursor=0;
  const atOrBefore=(rows,time,cursor) => {{
    while(cursor+1<rows.length && rows[cursor+1].t<=time) cursor++;
    while(cursor>0 && rows[cursor].t>time) cursor--;
    return cursor;
  }};
  const formatTime=t => `${{Math.floor(t/60)}}:${{(t%60).toFixed(2).padStart(5,'0')}}`;
  const update=() => {{
    const t=audio.currentTime||0;
    eventCursor=atOrBefore(data.events,t,eventCursor);
    trackCursor=atOrBefore(data.tracking,t,trackCursor);
    const event=data.events[eventCursor]||null;
    const track=data.tracking[trackCursor]||null;
    const chord=track ? data.chords.find(c=>c.i===track.chord) : null;
    root.querySelector('#mora-time').textContent=formatTime(t);
    root.querySelector('#mora-line').textContent=event&&event.t<=t ? event.line : '가사 시작 전';
    root.querySelector('#mora-token').textContent=event&&event.t<=t ? event.m : '-';
    root.querySelector('#mora-measure').textContent=track ? Math.floor(track.chord/4)+1 : '-';
    root.querySelector('#mora-chord').textContent=chord ? chord.name : '-';
    root.querySelector('#mora-tempo').textContent=track ? track.tempo.toFixed(3)+'×' : '1.000×';
    const drift=event&&event.t<=t ? event.drift : 0;
    root.querySelector('#mora-drift').textContent=(drift>=0?'+':'')+drift.toFixed(2)+'초';
    root.querySelector('#mora-playhead').setAttribute('x1',x(t));
    root.querySelector('#mora-playhead').setAttribute('x2',x(t));
    if(!audio.paused) requestAnimationFrame(update);
  }};
  audio.addEventListener('play',update);
  audio.addEventListener('timeupdate',update);
  audio.addEventListener('seeked',update);
  svg.addEventListener('click',event => {{
    const rect=svg.getBoundingClientRect();
    const local=(event.clientX-rect.left)/rect.width*1000;
    audio.currentTime=Math.max(0,Math.min(data.duration,(local-42)/940*data.duration));
    update();
  }});
  root.querySelector('#mora-first-boundary').addEventListener('click',()=>{{
    audio.currentTime=Math.max(0,data.focusStart-1); audio.play();
  }});
  root.querySelector('#mora-verse2').addEventListener('click',()=>{{
    audio.currentTime=Math.max(0,data.verse2-1); audio.play();
  }});
  update();
}})();
</script>
"""
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(fragment.strip() + "\n", encoding="utf-8")
    print(args.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
