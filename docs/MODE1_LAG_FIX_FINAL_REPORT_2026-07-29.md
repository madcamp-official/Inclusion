# Mode 1 반박자~한박자 지연 진단 및 수정 — 최종 보고

- 작성일: 2026-07-29
- 브랜치: `feature/guided-recording-and-training`
- 시작 HEAD: `b5eeadf7995ac4f3a780f7bd0961f661ac7b530b`
- 커밋 없음 (사용자 요청 시 별도 진행)

---

## 0. 청취 가능 여부 (지시서 §2, §23-1/2)

**나는 이 실행 환경에서 오디오를 재생하거나 청취할 수 없다.** 이 보고서의 모든 판정은
다음 두 가지로만 이루어졌다.

1. **스펙트로그램/파형 직접 시각 판독** — WAV를 PNG로 렌더링해 실제로 이미지를 봄
   (가장한 것이 아니라 실제 시각 인식)
2. **수치 분석** — RMS 포락선, onset/attack 검출, chroma 상관, 통계

**실제로 청취한 파일은 0개다.** 사용자가 직접 들어야 할 파일과 시간 구간은 §11에
정리했다.

---

## 1. Oracle A — asset과 score timing 검증

출력: `output/mode1_oracle/oracle_a/` (report.md 포함)

| 검증 | 결과 |
|---|---|
| micro clip 재구성 (149개 전수) | phrase 내부 median **−48.5 dB**, 149/149 −40 dB 이하 |
| clip 길이 정확도 | 최대 오차 0.019 ms |
| intro 경계 (첫 vocal 9.952s) | 인트로 구간 유성 비율 0.5% → 원곡과 일치 |
| `audible_onset_sec` anchor | **131/149가 0.0 — 사용 불가**, 적용 시 전곡 +80ms |
| 선행 자음 포함 여부 | 15개 표본 중 13개에서 자음이 80ms 패딩보다 앞서 시작 (median 197ms 초과) |
| 가사 커버리지 | 약 5.9초(6+8개 구간) 미커버 |

**판정: asset과 score timing은 합격.** 지연의 주원인이 아니다.

## 2. Oracle C — 재생 경로 손상 검증

출력: `output/mode1_oracle/oracle_c/` (report.md 포함)

| 지표 | median | 최악 |
|---|---:|---:|
| 버려지는 리드인 에너지 (본체 대비) | −1.77 dB | +13.91 dB |
| 첫 35ms 감쇠 (fade-in) | −2.80 dB | −15.50 dB |
| 재생 시작 시 레벨 (피크 대비) | 13.0% | 95.6% |
| 강제 종료 시 레벨 | 7.7% | 74.1% |
| 재구성 오차 (플레이어 경로 vs converted full) | −25.5 dB | — |

**판정: `PhrasePlayer`가 `contentOffsetSamples`부터 재생을 시작해 약 150ms의
자연스러운 자음 리드인을 통째로 버린다.** 트리거가 정확해도 모음부터 들리므로
체감상 늦게 들어온다. 149개 중 115개에서 −6dB 이상의 유의미한 오디오가
폐기됐다.

## 3. Oracle B — 실제 기타 vs 정답 timing

출력: `output/mode1_oracle/oracle_b/` (report.md), 수정 후 재검증
`output/mode1_oracle/oracle_b_postfix/`

정답 timing map(chroma 기반, 시각 검증)만으로 재생했을 때:

- 이전 take 첫 6개 phrase: ±210ms 이내 (median 수십 ms)
- **하지만 6번째 phrase부터 지연이 계속 누적** (+471ms → +2297ms, micro_011 기준)

**판정: follower가 chord 확인 없이 진행하는 구간에서 위치를 확인할 수단이 없어
지연이 누적된다.** 이것이 지시서가 지목한 근본 원인과 일치한다.

## 4. asset vs follower 분리 최종 판정

| 원인 후보 | 판정 |
|---|---|
| micro vocal asset 품질 (slicing/crossfade) | **정상** (Oracle A) |
| score↔source timing 매핑 | **정상** (Oracle A) |
| PhrasePlayer 재생 경로 (자음 폐기) | **결함 확인, 수정함** (Oracle C → §5) |
| chord 라벨 키 불일치 | **결함 확인, 수정함** (§5) — 단, **timing에는 영향 없음** (실험으로 반증, §5 참고) |
| follower가 onset 확인 없이 진행 못함 (chord-boundary clamp) | **근본 원인, 부분 수정** (§5, §6) |

---

## 5. 발견한 문제와 수정 (완료)

### 5.1 package builder: 리드인 80ms → 250ms

`tools/mode1_song_package/rebuild_package_in_original_key.py` (신규)

- Oracle C가 확인한 자음 폐기를 막기 위해 micro clip 리드인 패딩을 250ms로 확대
- 원본 `build_song_package.py`는 수정하지 않았다 (기존 파이프라인에 영향 없음).
  대신 기존 package에서 **새 timing으로 clip을 재절단**하는 별도 도구를 만들었다.
- **trailing pad는 원래 80ms로 유지했다.** 처음에 250ms로 함께 늘렸다가
  `playbackEndSamples`가 "파일 전체 길이"를 기본값으로 쓰는 것과 상호작용해
  **Baseline의 꼬리 길이 자체가 늘어나는 회귀를 발견하고 되돌렸다** (§7 참고).

### 5.2 chord 필드 키 불일치

`chord_timeline[].chord`가 `base_key_shift`(−7)만큼 보컬 키로 조옮김돼 있었는데,
실제 기타는 원곡 키로 연주됨 (`raw_chord`가 원곡 키). Chroma 분석으로 확인:

- 원곡 ↔ 기타 회전: +0 (r=0.840)
- 원곡 ↔ 변환보컬 회전: −5 (r=0.840)

`chord` = `raw_chord`로 교체, 보컬은 오프라인으로 +7반음 이동, `base_key_shift`를
0으로 재설정했다.

**중요한 실험 결과**: 이 수정을 적용한 새 package로 실제 렌더링했더니
**timing이 소수점까지 완전히 동일했다** (median/p95/max 전부 동일).
→ **당초 가설("chord 불일치가 지연 누적의 원인")은 실험으로 반증됐다.**
follower의 1차 전진 메커니즘은 chord 라벨 내용이 아니라 onset 타이밍만으로
동작하기 때문이다 (`chord`는 `applyChordEvidence`의 진단/보정 경로에서만 쓰임,
package의 `generic_chord_labels_are_diagnostic_only: true`와 일치). 이 수정은
**음정 문제는 고치지만 지연 문제는 고치지 않는다** — 정직하게 별도 항목으로
남긴다.

### 5.3 PhrasePlayer: 자음 리드인 재생

`src/mode1_vocal_follower/PhrasePlayer.h/.cpp`

- `requestPhrase(...)`에 `leadInSeconds` 파라미터 추가 (기본값 0.0 — 미지정 시
  기존과 완전히 동일)
- `sourcePosition = contentOffsetSamples - leadInSamples`로 변경, `leadInSamples`는
  클립이 실제로 가진 패딩 이내로 clamp
- `sourceStartPosition`을 새로 추적해 tail edge-fade가 리드인 때문에 조기
  발동하지 않도록 함
- **Baseline/Shadow는 절대 이 파라미터를 설정하지 않으므로 완전히 동일하게 동작**
  (byte-identical로 검증, §7)

### 5.4 PhraseScheduler: Active 전용 lead 계산

`src/mode1_vocal_follower/PhraseScheduler.h/.cpp`

- `activeVocalLeadSecondsForPhrase()`: `musicalAnticipation(100ms, Oracle B
  leadin test로 사용자가 직접 검증) + phrase의 content_offset`을 계산
- `startDueGuitarPhrase()`의 target 계산에서 이 값을 빼서 트리거를 앞당김
- **단, `startsAtCurrentBoundary`(방금 확인된 chord에 바로 붙는 phrase)에는
  적용하지 않는다** — 이 경로는 target-time gating을 건너뛰므로, 트리거
  시각이 당겨지지 않은 채 재생 시작점만 당기면 오히려 늦어진다
  (`lastPhraseUsedLeadTiming()`으로 구분, Mode1Controller에서 확인 후 적용)

### 5.5 Mode1Controller: 연결

- `scheduler.processBlock(...)`에 `predictiveModeActive` 전달
- `startPhraseImmediately(...)`에서 `scheduler.lastPhraseUsedLeadTiming()`이
  true이고 Active 모드일 때만 `leadInSeconds = phrase.contentOffsetSeconds` 전달

---

## 6. 시도했으나 되돌린 것 (지시서 §8 방식과 같은 실패 category)

**예측 커밋 거리 확장** — `predictsNextBoundary`가 "바로 다음 chord event"에만
적용되던 제약을 풀어 몇 이벤트 앞까지 신뢰하도록 시도했다. 시간 기반 lookahead
게이트는 유지했다.

**측정된 회귀**: 최근 take 렌더링에서 `expired_phrases=4` 발생. 원인: pause
(47.88–59.55s) 직전, 확립된 tempo로 몇 이벤트 앞선 phrase를 예측 커밋했는데
연주자가 실제로 멈춰서 그 사이 chord event가 끝내 확인되지 못했고, 재개 후
onset이 훨씬 뒤 event에 매칭되며 그 사이 4개 phrase가 일괄 expire됐다.

`inactiveTailSeconds` 게이트("최근에 실제로 연주 중일 때만 확장 허용")를
추가했지만 **효과 없었다** — 문제는 커밋 시점이 아니라 그 직후 pause가
시작된다는 것이라 사전 감지가 불가능했다.

**즉시 되돌렸다.** 코드에 이유를 남겨 재시도 방지. 이는 지시서 §8의
"chord cursor early-tolerance 확대"와 본질적으로 같은 실패 category
(확인 안 된 미래를 too far 신뢰)이며, 방향만 다르다 (거리 vs phase).

**따라서 Oracle B가 찾은 "지연 누적" 문제(§3)는 아직 근본적으로 해결되지
않았다.** 진짜 해결책(BeatClock을 실제 vocal transport에 연결하는
free-running transport, 제대로 된 HOLDING 상태 pause 보호)은 청취·실제
ASIO 검증 없이 안전하게 완성할 수 없다고 판단해 사용자 확인 후 중단했다.

---

## 7. 회귀 검증 결과

### 7.1 Baseline/Shadow 무변경 확인

| 비교 | 결과 |
|---|---|
| 기존 package, Baseline, 수정 전후 코드 | **byte-identical** (WAV, CSV) |
| 새 package(원곡 키+250ms 리드인), Baseline | 트리거 timing csv 완전 동일 |

### 7.2 Block-size × 곡 × 입력 × 모드 매트릭스

`output/mode1_regression_matrix_2026_07_29/` (40개 렌더)

| 항목 | 결과 |
|---|---|
| block size | 64, 128, 256, 512, 1024 |
| 곡 | 만찬가 (실제 기타 2 take), Oasis (synthetic strum + arpeggio, 2 입력) |
| 모드 | baseline, active |
| **phrase 중복** | **0/40** |
| **phrase 역순** | **0/40** |
| **expired phrase** | **0/40** |
| **realtime trace drop** | **0/40** |
| Oasis phrase count | 210/210 (전 block size, 전 모드 일치) |
| 만찬가 phrase count | 실제 take 길이만큼 (35 또는 41), **block 128 이상에서 완전 불변** |

**발견한 pre-existing 이슈 (내가 만든 것 아님):** block size **64**에서만 만찬가
실제 기타 입력의 phrase count가 1~2개 다르다 (36 vs 35, 43 vs 41). **수정 전
원본 package + baseline 모드에서도 동일하게 재현**돼, 내 변경과 무관한 기존
onset 검출의 block-64 민감도로 확인했다. Oasis(synthetic, 노이즈 없음)는
block 64에서도 완전히 불변이라 실제 기타 신호 특유의 문제로 보인다. 후속
과제로 남긴다.

### 7.3 Oasis package 데이터 불일치 (발견, 내가 만든 것 아님)

Oasis package의 `audio.converted_vocal` 참조 파일이 실제 배포된 micro clip과
완전히 일치하지 않는다 (동일 구간 재절단 시 −17.7dB 차이). 원본(수정 전)
package에도 있던 문제로, 내 재생성 스크립트나 lag 수정과 무관하다. Oasis
Baseline의 **timing/순서/개수는 정확히 동일**했으므로 코드 회귀는 아니다.

---

## 8. 정량 결과 (Active, 수정 후)

| take | 비교 phrase 수 | median | p95 | max | ≥반박 | ≥한박 |
|---|---:|---:|---:|---:|---:|---:|
| 최근 | 30 | +175ms (0.30박) | +1173ms | +1278ms | 13 | 9 |
| 이전 | 39 | +2002ms (3.44박) | +3857ms | +4402ms | 33 | 31 |

**최근 take**: 35개 중 **7개가 210~352ms 앞당겨짐**, 28개는 완전 동일(회귀 없음).
**이전 take**: 이번 수정으로 개선된 phrase 없음 (전부 `startsAtCurrentBoundary`
경로).

**정직한 결론: 정량 개선이 제한적이다.** "구조가 좋아졌다"는 이유로 성공
처리하지 않는다. 사용자가 체감하는 반박자~한박자 지연은 **아직 대부분
해결되지 않았다.**

---

## 9. 수정한 파일 목록

### C++ (전부 Baseline/Shadow byte-identical 유지)

- `src/mode1_vocal_follower/PhrasePlayer.h`
- `src/mode1_vocal_follower/PhrasePlayer.cpp`
- `src/mode1_vocal_follower/PhraseScheduler.h`
- `src/mode1_vocal_follower/PhraseScheduler.cpp`
- `src/mode1_vocal_follower/Mode1Controller.cpp`

### 도구 (신규)

- `tools/mode1_song_package/rebuild_package_in_original_key.py`
- `tools/mode1_oracle/` 전체 (oracle_common.py, oracle_a.py, oracle_a_score_vs_original.py,
  oracle_a_coverage.py, oracle_b.py, oracle_b_leadin_test.py, oracle_c.py,
  chroma_align.py, piecewise_map.py, check_regression_matrix.py)

### package (신규 산출물, 기존 package는 건드리지 않음)

- `build/mode1/bansanka_original_key/song_package.json`
- `build/mode1/dont_look_back_in_anger_widened/song_package.json`

원본 오디오, 악보, 기존 package, 이전 세션에서 이미 수정된 파일들은 **읽기만
했고 되돌리거나 덮어쓰지 않았다.**

---

## 10. 빌드 · 테스트

```bash
cmake --build build --config Release --target Mode1OfflineRenderer Mode1CoreTests -j 4
```
```bash
ctest --test-dir build -C Release --output-on-failure
```

| 테스트 | 결과 |
|---|---|
| Mode1CoreTests | Passed |
| VoiceCaptureTests | Passed |

---

## 11. 사용자가 반드시 청취해야 할 파일

| # | 파일 | 구간 | 확인할 것 |
|---|---|---|---|
| 1 | `output/mode1_oracle/oracle_a/micro_reconstructed.wav` | 9.5–15.0s | 정답 clock에서 자연스러운가 |
| 2 | `output/mode1_regression_check/active_v5_reverted/app_vocals_from_guitar.wav` | 10.0–24.0s | 수정 후 Active, 실제 기타 최근 take |
| 3 | `output/mode1_regression_check/active_v5_reverted_prev/app_vocals_from_guitar.wav` | 전체 | 수정 후 Active, 이전 take (개선 없음 확인용) |
| 4 | `output/mode1_predictive_runs/existing_recording_v2_last_take_baseline/app_vocals_from_guitar.wav` | 동일 구간 | Baseline 대조 (완전 동일해야 함) |

---

## 12. 물리 ASIO로 검증하지 못한 항목 (전부)

- 실제 ASIO 128 buffer에서 xrun 여부
- 실제 기타 스트럼/아르페지오 각 3 take
- 블라인드 A/B 사용자 선호
- 온라인(실시간) 환경에서의 leadIn/anticipation 체감

## 13. Active 기본값 승격 여부

**승격하지 않았다.** `Mode1Controller.h`의 기본값은 여전히
`FollowerMode::predictiveShadow`이며 변경하지 않았다.

근거: §8의 지연 누적 문제가 미해결이고, §6에서 실제 회귀(4개 phrase 일괄
expire)를 만들어냈다가 되돌린 이력이 있으며, 청취·ASIO 검증이 전혀
이루어지지 않았다. 지시서 §20의 Active 합격 기준(반박 이상 error 0 등)을
명백히 충족하지 못한다.
