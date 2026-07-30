# Claude 인수인계: Mode 1 원곡 청취 대조와 예측형 보컬 추종 재구축

## 0. Claude에게 보내는 실행 지시

이 문서를 처음부터 끝까지 읽고 작업한다. 바로 알고리즘 조건을 조정하거나
Active 출력을 수정하지 않는다. 먼저 이 문서에 정의된 Oracle A/B/C를
생성하고, 각 결과를 오디오·CSV·보고서로 남겨 보컬 asset 문제와 score
follower 문제를 분리 판정한다.

이 작업의 최종 목표는 기타 코드 검출 후 가사를 발사하는 구조가 아니라,
인트로에서 연주자의 tempo·phase·score position을 추정한 후 독립적인
continuous score transport가 미래 가사를 sample timeline에 예약하고,
기타 입력은 그 transport를 보정하도록 만드는 것이다.

작업 순서는 반드시 다음과 같다.

1. 작업 트리와 입력 파일을 보존한다.
2. 원곡·분리 보컬·현재 앱 보컬·실제 기타를 직접 청취하고 신호 분석한다.
3. Oracle A/B/C를 구현·렌더링한다.
4. 각 Oracle을 사람에게 들려줄 WAV와 정량 보고서를 만든다.
5. Oracle 통과 여부로 asset과 follower 중 원인을 확정한다.
6. Oracle이 통과한 경우에만 Active v2 free-running transport를 Shadow로
   구현한다.
7. Shadow가 정답 timing과 수치상 일치한 경우에만 실제 보컬 출력을 연결한다.
8. 만찬가와 Don't Look Back in Anger 모두 회귀 검증한다.
9. 모든 실행 명령과 결과를 Markdown으로 기록한다.

사용자에게 중간 결과를 숨기지 않는다. 개선이 없거나 더 나빠졌다면 그대로
보고한다. Baseline 대비 몇 개 phrase만 빨라진 결과를 “지연 해결”이라고
표현하지 않는다.

---

## 1. 저장소와 현재 상태

### 작업 루트

```text
C:\Users\User\kaist_madcamp\week4
```

### Git

```text
branch: feature/guided-recording-and-training
HEAD: b5eeadf7995ac4f3a780f7bd0961f661ac7b530b
```

주의: 현재 작업 트리는 의도적으로 dirty 상태다. HEAD 이후에 이번 예측형
follower 작업이 아직 commit되지 않은 상태로 들어 있다.

- `git reset --hard` 금지
- `git checkout -- <file>` 금지
- 기존 수정 사항 삭제 금지
- unrelated user changes 덮어쓰기 금지
- 먼저 `git status --short`와 `git diff`를 확인할 것
- 현재 상태를 별도 branch/commit으로 저장하려면 사용자에게 먼저 알릴 것

### 현재 최신성 주의

다음 GUI 실행 파일은 Active v7 시점에 빌드됐다.

```text
C:\Users\User\kaist_madcamp\week4\build\VocalGuitarApp_artefacts\Release\Vocal Guitar App.exe
```

그 후 `PhraseScheduler.cpp`의 “인트로에서도 phase 후보를 학습”하는 Active
v8 변경이 들어갔고, Offline Renderer와 tests만 다시 빌드됐다. 따라서 GUI로
최신 소스를 시험하기 전에는 반드시 `VocalGuitarApp`을 재빌드해야 한다.

최신 Offline Renderer:

```text
C:\Users\User\kaist_madcamp\week4\build\Mode1OfflineRenderer_artefacts\Release\Mode1OfflineRenderer.exe
```

---

## 2. 사용자가 최종적으로 원하는 경험

사용자가 곡을 선택하고 기타를 치면, 정해진 가사가 사용자 연주의 tempo와
phase를 따라 보컬로 나온다.

원하는 체감:

- 가사가 기타 뒤를 한 박 따라오지 않음
- 스트럼과 아르페지오 모두 동작
- 인트로 동안 보컬은 나오지 않지만 tempo·phase를 학습
- 첫 가사부터 붙어 나옴
- 사용자가 조금 빠르거나 느려져도 부드럽게 추종
- 짧은 실수나 chord 오인식으로 가사가 건너뛰거나 중복되지 않음
- 기타를 멈추면 보컬이 무한정 혼자 진행하지 않음
- 곡별 하드코딩이 아니라 새 곡에도 적용 가능한 package pipeline

목표 수치:

| 측정 대상 | 목표 |
|---|---:|
| 기타 직접 모니터링 | 10ms 이하 |
| 기타 입력 → 즉각 반응음/UI | 15~20ms 이하, 허용 30ms |
| LOCKED 구간 vocal anchor ↔ 목표 박자 median | ±30ms |
| LOCKED 구간 vocal anchor ↔ 목표 박자 P95 | ±60ms |
| scheduling jitter P95 | 3ms 이하 |
| 100ms 이상 timing error | 전체 anchor의 1% 미만 |
| 반박자 이상 error | 0 |
| phrase 누락·중복·역순 | 0 |
| pause runaway | 0 |
| ASIO 128 xrun | 0 |

물리 ASIO 지연은 오프라인 렌더러로 증명할 수 없다. 오프라인 수치와 실제
loopback 수치를 구분해 기록해야 한다.

---

## 3. 만찬가 원본 입력 파일

### 3.1 반드시 사용할 원곡 MP3

```text
C:\Users\User\Downloads\tuki. - 만찬가(晩餐歌) [가사발음해석] - 지구의 가사집 [J-POP] (128k).mp3
```

- 48kHz, stereo
- 약 220.056초
- 파일 크기 3,521,709 bytes

같은 이름의 `(1).mp3` 복사본도 있지만 기준은 위의 괄호 없는 파일이다.

다음 파일은 잘린 별도 파일이므로 이번 첫 소절 기준 분석에 사용하지 않는다.

```text
C:\Users\User\Downloads\tuki. - 만찬가(晩餐歌) [2분01초부터 끝까지].mp3
```

### 3.2 원곡에서 분리된 보컬

```text
C:\Users\User\sounce_source_only_vocal\1_tuki. - 만찬가(晩餐歌) [가사발음해석] - 지구의 가사집 [J-POP] (128k)_(Vocals).wav
```

- 원곡 timeline을 유지한 분리 보컬
- 파일 크기 38,817,836 bytes

### 3.3 보컬 melody/악보

```text
C:\Users\User\Downloads\tuki. - 만찬가(晩餐歌) [가사발음해석] - 지구의 가사집 [J-POP] (128k).musicxml
```

- 파일 크기 251,170 bytes
- 원곡과 달리 MusicXML 보컬은 초반 4마디 쉼이 없었던 이력이 있으므로,
  package 생성 과정에서 intro offset을 별도로 고려했다.

### 3.4 기타/코드 GP5

```text
C:\Users\User\Downloads\tuki. - 만찬가(晩餐歌) [가사발음해석] - 지구의 가사집 [J-POP] (128k).gp5
```

### 3.5 현재 활성 song package

```text
C:\Users\User\kaist_madcamp\week4\build\mode1\bansanka\song_package.json
```

중요한 package 사실:

- `schema_version`: 2
- `score_bpm`: 약 103.004
- phrase 수: 149
- chord event 수: 158
- first chord: 0.000초
- 첫 vocal 목표: 9.952초
- 첫 vocal 이전 chord event: 9개
- intro section: `instrumental_intro`
- intro section의 `vocal_allowed`: false
- 첫 vocal phrase와 chord event index 9가 모두 9.952초

package 내부 `audio.source_vocal`, `audio.converted_vocal`,
`audio.guide_vocal`도 확인할 것.

### 3.6 변환된 full vocal

현재 package가 참조하는 기준 경로:

```text
C:\Users\User\kaist_madcamp\week4\build\mode1\bansanka\generated_models\rvc_user_20260728_183422\anchor_m007_expression100\rvc_output_style_100_48k.wav
```

### 3.7 micro vocal 조각

```text
C:\Users\User\kaist_madcamp\week4\build\mode1\bansanka\fine_candidate\micro_vocals\
```

예:

```text
micro_000.wav
micro_001.wav
...
```

각 phrase의 실제 파일, `content_offset_sec`, `vocal.sync` 값은
`song_package.json`에서 읽어야 한다. 파일명을 추측하지 않는다.

---

## 4. 실제 기타 녹음

### 최근 녹음

```text
C:\Users\User\kaist_madcamp\week4\output\audio\guitar_tests\bansanka_last_guitar_take.wav
```

- 48kHz, mono
- 약 81.410초
- 파일 크기 11,723,144 bytes

### 이전 녹음

```text
C:\Users\User\kaist_madcamp\week4\output\audio\guitar_tests\bansanka_previous_guitar_take_recovered.wav
```

- 48kHz, mono
- 약 61.580초
- 파일 크기 8,867,564 bytes

### 혼동하지 말아야 할 파일

```text
C:\Users\User\kaist_madcamp\week4\output\audio\guitar_tests\bansanka_last_guitar_take_plus5.wav
```

이 파일과 recovered take는 길이가 같아 중간 변환/복구 과정의 연관 파일일 수
있다. 기준 실제 take는 위의 `last_guitar_take.wav`와
`previous_guitar_take_recovered.wav` 두 개다.

---

## 5. 현재 청취 대조 자료

폴더:

```text
C:\Users\User\kaist_madcamp\week4\output\mode1_predictive_runs\original_vs_current_listening\
```

파일:

```text
01_original_first_verse.wav
02_separated_original_vocal.wav
03_current_app_vocal.wav
04_current_guitar_plus_vocal.wav
05_AB_original_then_app_vocal.wav
analysis.json
```

절대경로:

```text
C:\Users\User\kaist_madcamp\week4\output\mode1_predictive_runs\original_vs_current_listening\01_original_first_verse.wav
C:\Users\User\kaist_madcamp\week4\output\mode1_predictive_runs\original_vs_current_listening\02_separated_original_vocal.wav
C:\Users\User\kaist_madcamp\week4\output\mode1_predictive_runs\original_vs_current_listening\03_current_app_vocal.wav
C:\Users\User\kaist_madcamp\week4\output\mode1_predictive_runs\original_vs_current_listening\04_current_guitar_plus_vocal.wav
C:\Users\User\kaist_madcamp\week4\output\mode1_predictive_runs\original_vs_current_listening\05_AB_original_then_app_vocal.wav
C:\Users\User\kaist_madcamp\week4\output\mode1_predictive_runs\original_vs_current_listening\analysis.json
```

`05_AB_original_then_app_vocal.wav`는 원곡 첫 소절 다음에 1초 무음을 넣고
현재 앱 vocal을 이어 붙인 청취 자료다. 두 구간은 서로 다른 연주 timeline이므로
sample-perfect 동기 비교 파일이 아니다. 음절 attack, phrase spacing, 조각 연결의
자연스러움을 듣기 위한 자료다.

---

## 6. Baseline과 현재 Active 출력

### 최근 take Baseline

```text
C:\Users\User\kaist_madcamp\week4\output\mode1_predictive_runs\existing_recording_v2_last_take_baseline\
```

주요 파일:

```text
app_vocals_from_guitar.wav
guitar_plus_app_vocals.wav
phrase_events.csv
score_tracking_events.csv
raw_guitar_onsets.csv
raw_chord_detections.csv
realtime_trace.csv
```

### 최근 take 최신 Active v8

```text
C:\Users\User\kaist_madcamp\week4\output\mode1_predictive_runs\existing_recording_v8_last_take_active\
```

### 이전 take Baseline

```text
C:\Users\User\kaist_madcamp\week4\output\mode1_predictive_runs\existing_recording_v2_previous_take_baseline\
```

### 이전 take 최신 Active v8

```text
C:\Users\User\kaist_madcamp\week4\output\mode1_predictive_runs\existing_recording_v8_previous_take_active\
```

### 최신 비교 요약

```text
C:\Users\User\kaist_madcamp\week4\output\mode1_predictive_runs\existing_recordings_v8_detailed_summary.json
```

v8 결과:

- 최근 take: 35 phrase 중 7개만 Baseline보다 빨라짐
- 최근 take: 28개는 Baseline과 동일
- 이전 take: 41 phrase 중 9개만 빨라짐
- 이전 take: 32개는 동일
- 늦어진 phrase: 0
- 전체 median 개선: 0ms

따라서 v8도 체감 지연 해결로 판정하지 않는다.

---

## 7. 확인된 정량 진단

### 출력 시작 병목이 아님

- phrase request부터 first non-zero output은 대부분 같은 callback
- 렌더된 vocal RMS도 대부분 request 직후 발생
- 일부 clip의 attack 35~95ms는 존재하지만 반복적인 한 박 지연 전체를
  설명하지 못함

### 첫 가사의 실제 문제

- package 첫 vocal/chord 목표: score 9.952초
- 실제 최근 take에서 해당 경계 주변 onset 간격: 약 226~230ms
- 앱 vocal은 첫 스트럼이 아니라 후속 onset에 거의 즉시 붙음
- 따라서 내부 onset-to-output은 수 ms여도 사용자는 반 박자 이상 늦게 느낌

### 진행 중 불연속

최근 take 35 phrase를 immutable source timing과 affine 비교한 참고값:

- fitted performance seconds / score second: 약 1.158
- fitted tempo scale: 약 0.864
- timing residual median absolute: 약 1.81초
- P95 absolute: 약 5.75초
- max absolute: 약 7.55초

이 값에는 사용자의 실제 pause가 포함되므로 물리 latency로 보고하면 안 된다.
다만 하나의 고정 offset이 아니라 기타 event 대기로 인해 score progression이
불연속이라는 증거로 사용한다.

---

## 8. 현재 구현의 핵심 코드

### 실시간 controller

```text
C:\Users\User\kaist_madcamp\week4\src\mode1_vocal_follower\Mode1Controller.h
C:\Users\User\kaist_madcamp\week4\src\mode1_vocal_follower\Mode1Controller.cpp
```

### 기존 phrase scheduler

```text
C:\Users\User\kaist_madcamp\week4\src\mode1_vocal_follower\PhraseScheduler.h
C:\Users\User\kaist_madcamp\week4\src\mode1_vocal_follower\PhraseScheduler.cpp
```

현재 Active도 대부분 다음 구조다.

```text
onset 채택
→ chord cursor 전진
→ songTime을 chord event로 이동
→ phrase 재생
```

또한 다음 chord boundary 앞에서 `songTimeSeconds`를 clamp한다. 이 clamp가
free-running transport를 막는 핵심 지점이다.

### BeatClock

```text
C:\Users\User\kaist_madcamp\week4\src\mode1_vocal_follower\BeatClock.h
C:\Users\User\kaist_madcamp\week4\src\mode1_vocal_follower\BeatClock.cpp
```

상태:

```text
DISARMED
ARMING
LOCKED
COASTING
HOLDING
RECOVERING
```

현재 BeatClock은 주로 shadow confidence/stop sensor다. 제품 vocal transport를
직접 운전하지 않는다.

### PhrasePlayer

```text
C:\Users\User\kaist_madcamp\week4\src\mode1_vocal_follower\PhrasePlayer.h
C:\Users\User\kaist_madcamp\week4\src\mode1_vocal_follower\PhrasePlayer.cpp
```

현재 들어간 기능:

- vocal preload
- audio callback mix
- bounded delayed sample start
- early-arrival override
- first-output trace

### 추적 로그

```text
C:\Users\User\kaist_madcamp\week4\src\mode1_vocal_follower\RealtimeTraceBuffer.h
```

trace type:

- onset detected
- score event changed
- phrase requested
- vocal first output
- BeatClock observation/state change

### Song package

```text
C:\Users\User\kaist_madcamp\week4\src\mode1_vocal_follower\SongPackage.h
C:\Users\User\kaist_madcamp\week4\src\mode1_vocal_follower\SongPackage.cpp
```

`vocal.sync`:

- `audible_onset_sec`
- optional reviewed `vowel_onset_sec`
- `confidence`
- `detector_version`

### Offline renderer

```text
C:\Users\User\kaist_madcamp\week4\tools\mode1_offline_renderer\Main.cpp
```

### 테스트

```text
C:\Users\User\kaist_madcamp\week4\tests\mode1\Mode1CoreTests.cpp
```

---

## 9. 이미 시도했고 폐기한 접근

### 9.1 chord cursor early tolerance 확대

첫 스트럼을 다음 chord로 더 일찍 받도록 window를 약 반 박자까지 넓혔다.

결과:

- 대부분 phrase가 약 230ms 빨라짐
- 그러나 correction이 event마다 반복 누적
- pause에서 2~12초 이상 선행
- phrase 진행이 실제 연주보다 앞서감

폐기. 다음 결과 폴더는 실패 분석용이며 제품 기준으로 사용하지 않는다.

```text
C:\Users\User\kaist_madcamp\week4\output\mode1_predictive_runs\existing_recording_v5_last_take_active
C:\Users\User\kaist_madcamp\week4\output\mode1_predictive_runs\existing_recording_v5_previous_take_active
```

### 9.2 chord cursor 전역 phase rebase

이른 onset을 받은 뒤 performance origin 자체를 이동했다.

결과:

- 누적 선행이 더 커짐
- 최근 take median이 약 1초 빨라짐
- 일부 phrase는 10초 이상 빨라짐
- pause 처리 실패

폐기.

```text
C:\Users\User\kaist_madcamp\week4\output\mode1_predictive_runs\existing_recording_v6_last_take_active
C:\Users\User\kaist_madcamp\week4\output\mode1_predictive_runs\existing_recording_v6_previous_take_active
```

### 9.3 vocal target에만 learned phase lead 적용

chord cursor는 안전하게 유지하고, 이른 후보 onset과 후속 채택 onset의
150~320ms subdivision 간격을 학습해 vocal target만 당겼다.

결과:

- pause runaway 제거
- 늦어진 phrase 0
- 최근 take 6~7개, 이전 take 8~9개만 개선
- 대부분 phrase는 여전히 chord trigger를 기다림
- median 개선 0ms

이 feature는 Shadow 관측값으로는 유지할 수 있지만 근본 해결책이 아니다.

### 9.4 독립 PredictivePhraseScheduler

과거 실험용:

```text
C:\Users\User\kaist_madcamp\week4\src\mode1_vocal_follower\PredictivePhraseScheduler.h
C:\Users\User\kaist_madcamp\week4\src\mode1_vocal_follower\PredictivePhraseScheduler.cpp
```

standalone으로 제품 출력을 운전했을 때 phrase를 건너뛰었다.

- 만찬가 117/149
- Oasis 167/210

현재 제품 출력 경로로 사용하지 않는다. 설계를 그대로 재활용하지 않는다.

---

## 10. 유지해야 하는 기존 개선

다음은 회귀시키지 않는다.

- Baseline / Predictive Shadow / Predictive Active 즉시 전환
- 기본 앱 모드는 Predictive Shadow
- intro chord event 보존
- intro `vocal_allowed: false`
- Oasis 첫 가사 즉시 진입 오류 수정
- fixed-capacity real-time trace
- audio thread에서 file I/O/allocation/mutex/UI 호출 금지
- input/output latency 분리
- vocal preload
- bounded delayed sample start
- early-arrival override
- vocal audible/vowel anchor schema
- 128 sample 내부 analysis sub-block
- 64/128/256/512/1024 block-size 불변성
- 곡 내부 chord vocabulary 기반 local recovery

---

## 11. 반드시 먼저 수행할 Oracle

### Oracle A: 원곡 clock으로 앱 vocal 재생

목적: 현재 micro vocal asset과 package timing이 정답 clock에서 자연스러운지
확인한다.

절차:

1. 기타 입력, onset, chord follower를 모두 제거한다.
2. package의 immutable score/source time만 사용한다.
3. 원곡과 동일한 48kHz output timeline을 만든다.
4. 각 micro phrase를 정확한 target sample에 배치한다.
5. `content_offset_sec`, `audible_onset_sec`, reviewed
   `vowel_onset_sec` 정책을 각각 비교한다.
6. 원곡/분리보컬/full converted/micro reconstructed를 비교한다.

출력:

```text
output/mode1_oracle/oracle_a/
  original_reference.wav
  separated_vocal_reference.wav
  converted_full_reference.wav
  micro_reconstructed.wav
  micro_reconstructed_with_audible_anchor.wav
  phrase_anchor_errors.csv
  report.md
```

판정:

- 원곡 rhythm과 micro reconstructed가 맞으면 asset 합격, follower가 원인
- 여기서도 밀리면 slicing/content offset/score-source mapping이 원인
- Oracle A가 실패하면 Active v2 구현을 시작하지 않는다

### Oracle B: 실제 기타 수동 2-anchor time warp

목적: 자동 인식 없이 정확한 performance mapping만 주면 vocal이 자연스러운지
확인한다.

절차:

1. 최근/이전 실제 기타 take에서 최소 두 개의 명확한 score boundary를 지정
2. 첫 intro anchor와 첫 소절 뒤 anchor를 사용
3. score time → performance time의 affine map 생성
4. 필요하면 section별 piecewise affine map도 별도 생성
5. chord/onset follower 없이 모든 vocal event를 sample 예약

anchor 선택은 자동 결과를 맹신하지 말고 waveform과 청취로 기록한다.

출력:

```text
output/mode1_oracle/oracle_b/last_take/
output/mode1_oracle/oracle_b/previous_take/
  guitar_plus_oracle_vocal.wav
  oracle_vocal.wav
  manual_anchors.json
  phrase_targets.csv
  report.md
```

판정:

- 이 출력이 자연스러우면 free-running transport 방향 확정
- 이것도 이상하면 실제 기타 진행과 package score가 다르거나 asset timing 오류

### Oracle C: vocal asset 재조립 검증

목적: micro slicing과 crossfade가 발음·쉼·phrase rhythm을 손상시키는지 확인한다.

비교:

1. 원곡 분리 보컬
2. converted full vocal
3. micro clip source-time 재조립
4. 현재 PhrasePlayer 경로 재조립

측정:

- phrase audible onset
- 가능하면 vowel onset
- clip overlap/gap
- syllable truncation
- phrase attack 손실
- crossfade로 인한 음절 먹힘
- time-stretch로 인한 금속성 artifact

출력:

```text
output/mode1_oracle/oracle_c/
  separated_vocal.wav
  converted_full.wav
  reconstructed_no_crossfade.wav
  reconstructed_current_player.wav
  clip_timing.csv
  suspicious_clips/
  report.md
```

---

## 12. Oracle 이후 Active v2 설계

### 12.1 두 cursor를 분리

```cpp
struct PredictiveTransport
{
    double continuousScoreSeconds;
    double secondsPerBeat;
    double phaseSeconds;
    double confidence;
    int confirmedScoreEventIndex;
    int recoveryCandidateIndex;
};
```

- `continuousScoreSeconds`: vocal 예약을 운전
- `confirmedScoreEventIndex`: chord/chroma로 확인한 최근 위치

두 값을 하나의 chord cursor로 사용하지 않는다.

### 12.2 인트로 calibration

모든 대상 곡은 현재 인트로가 있다. 이를 지연 원인이 아니라 calibration
구간으로 사용한다.

- 모든 onset interval 관측
- beat/subdivision 후보를 동시에 유지
- 첫 스트럼 phase와 아르페지오 후속 phase를 multi-hypothesis로 유지
- intro score event 간격과 비용 비교
- chord/chroma는 가설 순위 조정
- 최소 3 boundary 또는 2마디 후 LOCK
- 첫 vocal 250ms 전까지 LOCK 실패 시 안전 fallback
- intro 동안 vocal output은 반드시 0

현재 v8처럼 vocal boundary 주변에서만 phase를 학습하면 안 된다.

### 12.3 free-running transport

Active의 LOCKED/COASTING 상태에서는 기존 chord-boundary clamp를 사용하지
않는다.

- continuous score time은 추정 tempo로 계속 진행
- onset이 없어도 미래 vocal event를 예약
- onset은 phase/tempo correction
- 연주가 빨라지면 commit 전 event를 당김
- 연주가 늦어지면 다음 1~2박에 걸쳐 수렴
- HOLDING 진입 시 새 event commit 중단
- pause에서 transport가 무한 진행하지 않음

### 12.4 sample 예약 queue

```cpp
struct ScheduledVocalEvent
{
    int phraseIndex;
    int64_t targetOutputSample;
    int64_t commitDeadlineSample;
    double confidence;
};
```

- look-ahead 100~250ms
- 초기 commit horizon 80ms
- fixed-capacity queue
- audio callback에서 target sample부터 직접 mix
- 이미 commit된 event와 아직 수정 가능한 event를 분리
- 동일 phrase 중복 금지
- index 역순 금지

### 12.5 입력 feature 역할

- onset: tempo/phase correction
- 첫 스트럼 가설: 높은 phase weight
- 후속 아르페지오 onset: subdivision 가설
- chord/chroma: local score-position confirmation/recovery
- RMS/silence: pause/HOLD 판단
- 곡에 없는 chord label을 새로 탐색하지 않음
- recovery는 현재 위치 주변 ±1~2 chord event부터 시작

---

## 13. 빌드와 테스트 명령

PowerShell 기준.

### Offline Renderer와 tests 빌드

```powershell
cmake --build build --config Release --target Mode1OfflineRenderer Mode1CoreTests -j 4
```

### 전체 CTest

```powershell
ctest --test-dir build -C Release --output-on-failure
```

현재 기대:

```text
Mode1CoreTests: pass
VoiceCaptureTests: pass
```

### GUI 최신 빌드

```powershell
cmake --build build --config Release --target VocalGuitarApp -j 4
```

Release link-time optimization 때문에 1~2분 이상 걸릴 수 있다. 짧은 timeout으로
실패라고 판단하지 않는다.

### Offline Renderer 사용법

```text
Mode1OfflineRenderer <song_package.json> <guitar.wav> <output-directory> [block-size] [guitar-gain] [baseline|shadow|active]
```

최근 take Baseline 예:

```powershell
& 'C:\Users\User\kaist_madcamp\week4\build\Mode1OfflineRenderer_artefacts\Release\Mode1OfflineRenderer.exe' `
  'C:\Users\User\kaist_madcamp\week4\build\mode1\bansanka\song_package.json' `
  'C:\Users\User\kaist_madcamp\week4\output\audio\guitar_tests\bansanka_last_guitar_take.wav' `
  'C:\Users\User\kaist_madcamp\week4\output\claude_handoff_runs\bansanka_last_baseline' `
  128 2 baseline
```

Shadow:

```powershell
& 'C:\Users\User\kaist_madcamp\week4\build\Mode1OfflineRenderer_artefacts\Release\Mode1OfflineRenderer.exe' `
  'C:\Users\User\kaist_madcamp\week4\build\mode1\bansanka\song_package.json' `
  'C:\Users\User\kaist_madcamp\week4\output\audio\guitar_tests\bansanka_last_guitar_take.wav' `
  'C:\Users\User\kaist_madcamp\week4\output\claude_handoff_runs\bansanka_last_shadow' `
  128 2 shadow
```

Active:

```powershell
& 'C:\Users\User\kaist_madcamp\week4\build\Mode1OfflineRenderer_artefacts\Release\Mode1OfflineRenderer.exe' `
  'C:\Users\User\kaist_madcamp\week4\build\mode1\bansanka\song_package.json' `
  'C:\Users\User\kaist_madcamp\week4\output\audio\guitar_tests\bansanka_last_guitar_take.wav' `
  'C:\Users\User\kaist_madcamp\week4\output\claude_handoff_runs\bansanka_last_active' `
  128 2 active
```

### Package validator

```powershell
python tools/mode1_song_package/validate_song_package.py `
  build/mode1/bansanka/song_package.json
```

### Vocal anchor 자동 분석

```powershell
uv run --with numpy --with soundfile python `
  tools/mode1_song_package/annotate_vocal_anchors.py `
  build/mode1/bansanka/song_package.json `
  --output output/claude_handoff_runs/bansanka_annotated_candidate.json
```

자동 audible onset을 phonetic vowel onset이라고 부르지 않는다.

---

## 14. Don't Look Back in Anger 회귀 자료

Active v2는 만찬가 한 곡에 하드코딩하면 안 된다.

### 원곡 전체 WAV

```text
C:\Users\User\Downloads\Don't Look Back In Anger (Remastered) - Oasis (128k).wav
```

### 분리 보컬

```text
C:\Users\User\sounce_source_only_vocal\1_Don't Look Back In Anger (Remastered) - Oasis (128k)_(Vocals).wav
```

### MusicXML

```text
C:\Users\User\Downloads\Don't Look Back In Anger (Remastered) - Oasis (128k).musicxml
```

### GP5

```text
C:\Users\User\Downloads\Don't Look Back In Anger (Remastered) - Oasis (128k).gp5
```

### 가사

```text
C:\Users\User\Downloads\dont_look_back_in_anger.txt
```

### chord 참고 PDF

```text
C:\Users\User\OneDrive\Documents\카카오톡 받은 파일\chord-ai_Don't Look Back In Anger.pdf
```

### 활성 package

```text
C:\Users\User\kaist_madcamp\week4\build\mode1\dont_look_back_in_anger\song_package.json
```

Oasis package 상태:

- phrase 210
- chord event 219
- first chord 0.000초
- first vocal 11.184초
- intro chord event 9개
- intro section 1개
- 기존에는 intro event가 삭제되어 `Slip...`이 즉시 나왔음
- package builder가 phrase-anchored chord timeline으로 전체 timeline을
  교체한 것이 원인이었고 현재 수정됨

합성 intro 입력:

```text
C:\Users\User\kaist_madcamp\week4\output\mode1_predictive_runs\candidate_oasis_intro_128\synthetic_strum_with_intro.wav
```

아르페지오 입력:

```text
C:\Users\User\kaist_madcamp\week4\output\mode1_predictive_runs\oasis_active_arpeggio.wav
```

---

## 15. 회귀 행렬

현재 안전 기준:

| 곡 | 기대 phrase |
|---|---:|
| 만찬가 | 149 |
| Oasis | 210 |

반드시 다음 block size를 모두 검사한다.

```text
64
128
256
512
1024
```

각 조합의 합격 조건:

- phrase count 정확히 일치
- expired phrase 0
- duplicate 0
- out-of-order 0
- trace dropped 0
- intro vocal 0
- pause runaway 0

기존 v7 matrix:

```text
C:\Users\User\kaist_madcamp\week4\output\mode1_predictive_runs\v7_block_matrix.csv
```

새 transport는 새 matrix 파일을 생성해야 한다. 기존 파일을 덮어쓰지 않는다.

---

## 16. 관련 문서

가장 먼저 읽을 문서:

```text
C:\Users\User\kaist_madcamp\week4\docs\MODE1_ORIGINAL_VS_CURRENT_DIAGNOSIS_AND_REBUILD_PLAN_2026-07-29.md
```

전체 계획:

```text
C:\Users\User\kaist_madcamp\week4\docs\MODE1_PREDICTIVE_SCORE_FOLLOWER_MASTER_PLAN_2026-07-29.md
```

구현 현황:

```text
C:\Users\User\kaist_madcamp\week4\docs\MODE1_PREDICTIVE_IMPLEMENTATION_STATUS_2026-07-29.md
```

baseline:

```text
C:\Users\User\kaist_madcamp\week4\docs\MODE1_PREDICTIVE_BASELINE_REPORT_2026-07-29.md
```

latency breakdown:

```text
C:\Users\User\kaist_madcamp\week4\docs\MODE1_LATENCY_BREAKDOWN_REPORT_2026-07-29.md
```

---

## 17. 구현·검증 완료 조건

### Oracle 완료

- [ ] Oracle A 청취 파일·CSV·report
- [ ] Oracle B 두 실제 take 청취 파일·anchor JSON·report
- [ ] Oracle C 4종 vocal 비교·의심 clip 목록·report
- [ ] asset 문제와 follower 문제를 명시적으로 분리 판정
- [ ] 사용자가 청취할 파일 절대경로 제공

### Shadow transport 완료

- [ ] 인트로 동안 vocal 0
- [ ] 첫 vocal 전에 LOCK
- [ ] continuous score time이 chord event 없이 진행
- [ ] confirmed score cursor와 vocal transport cursor 분리
- [ ] shadow target과 Oracle target 비교 CSV
- [ ] first vocal ±60ms
- [ ] LOCKED median ±30ms
- [ ] LOCKED P95 ±60ms
- [ ] 반박자 error 0

### Active 완료

- [ ] fixed-capacity sample event queue
- [ ] 100~250ms look-ahead
- [ ] commit horizon
- [ ] phrase 누락·중복·역순 0
- [ ] pause runaway 0
- [ ] Baseline보다 늦어진 phrase 0
- [ ] block-size matrix 통과
- [ ] 만찬가와 Oasis 모두 통과
- [ ] 실제 ASIO 128 xrun 0
- [ ] 실제 기타 스트럼/아르페지오 각 3 take
- [ ] 블라인드 A/B에서 사용자 선호

Active가 위 조건을 통과하기 전까지 앱 기본값은 Predictive Shadow다.

---

## 18. Claude가 최종 보고할 형식

최종 보고에는 최소한 다음을 포함한다.

1. 실제로 들은/분석한 파일 목록
2. Oracle A/B/C 각각의 결과
3. root cause 판정
4. 수정한 파일 목록
5. 기존 실패 방식을 재사용하지 않았다는 확인
6. 빌드·테스트 명령
7. 테스트 결과 표
8. Baseline/Shadow/Active 청취 파일 절대경로
9. 첫 vocal, median, P95, max timing error
10. phrase 누락·중복·역순·pause runaway 수
11. 물리 ASIO로 아직 검증하지 못한 항목
12. Active를 기본값으로 승격했는지 여부와 근거

정량 개선이 없으면 “구조는 개선됐다”는 이유로 성공 처리하지 않는다.
사용자가 실제로 한 박 늦다고 느끼는 문제가 사라졌는지가 최종 기준이다.

