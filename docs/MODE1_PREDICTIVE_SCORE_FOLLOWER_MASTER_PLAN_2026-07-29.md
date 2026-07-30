# Mode 1 예측형 스코어 팔로워 전환·검증 마스터 플랜

- 작성일: 2026-07-29
- 상태: **검토용 계획서 — 아직 구현 승인 전**
- 적용 대상: Mode 1 기타 연주 추종 보컬
- 1차 검증 곡: `만찬가`, `Don't Look Back in Anger`
- 핵심 원칙: **보컬은 예측된 음악 시간에 재생하고, 기타 입력은 그 시간을 계속 교정한다.**

---

## 0. 이 문서의 목적과 승인 규칙

이 문서는 현재의 “기타 이벤트가 확정되어야 다음 가사가 시작되는” 구조를, “예측된 박자 시계가 계속 진행하고 기타가 그 시계를 교정하는” 구조로 바꾸기 위한 구현·측정·실연 계획이다.

이번 단계에서는 코드를 바꾸지 않는다. 먼저 이 문서를 읽고 다음을 합의한 뒤 구현에 들어간다.

1. 어느 정도의 자율 진행을 허용할지
2. 기타가 멈췄을 때 보컬이 얼마 동안 진행해도 되는지
3. 첫 가사의 시작 방식을 어떻게 할지
4. “성공”으로 볼 수 있는 지연·안정성 수치가 무엇인지
5. 실제 기타 시연에서 기존 방식보다 의미 있게 낫다는 것을 어떻게 판단할지

각 구현 단계는 독립적인 합격 관문을 가진다. 한 단계가 합격하지 못하면 다음 단계로 넘어가지 않는다. 기존 Mode 1은 기능 플래그로 남겨 언제든 즉시 비교하거나 되돌릴 수 있게 한다.

---

## 1. 결론부터: 만들려는 시스템

최종 구조는 단순한 “코드 인식기 → WAV 재생 버튼”도 아니고, 정해진 노래를 일방적으로 틀어 놓는 반주기도 아니다.

```text
기타 입력
  ├─ onset: 박자 위상과 템포를 빠르게 교정
  ├─ chroma/chord: 현재 악보 위치 확인 및 놓쳤을 때 복구
  └─ RMS/silence: 연주 중단과 신뢰도 판단
        ↓
예측형 BeatClock + 제한된 악보 위치 추정
        ↓ 80~150 ms 앞을 예측
샘플 단위 보컬 예약
        ↓
자음 선행 재생 + 모음 핵심을 목표 박에 정렬
```

보컬은 매 기타음을 기다리지 않고 짧은 범위 안에서 미리 예약된다. 그러나 기타 입력이 사라지거나 악보와 맞지 않으면 신뢰도가 떨어지고, 허용된 관성 구간 이후에는 새 가사를 시작하지 않는다. 따라서 사용자는 템포와 위치를 계속 통제하며, 시스템은 그 통제를 지연 없이 보컬로 표현한다.

### 최종 사용자 경험

- 정상 연주 중에는 가사가 반 박자나 한 박자 뒤늦게 따라오는 느낌이 없어야 한다.
- 코드 스트럼뿐 아니라 아르페지오에서도 한 마디 안의 여러 onset 때문에 가사가 폭주하지 않아야 한다.
- 기타를 조금 빠르게 또는 느리게 치면 보컬도 부드럽게 따라가야 한다.
- 기타를 멈추면 보컬이 곡 끝까지 혼자 달려가면 안 된다.
- 인트로 동안에는 보컬이 나오지 않지만, 시스템은 템포·위상·악보 위치를 충분히 학습해야 한다.
- 첫 가사가 시작될 때는 이미 시계가 잠겨 있어야 하며, 첫 음절부터 붙어 있는 느낌이 나야 한다.
- 곡별 코드 이름을 하드코딩하지 않고, 같은 엔진과 패키지 형식으로 여러 곡을 처리해야 한다.

---

## 2. 왜 구조 변경이 필요한가

### 2.1 현재 지연의 핵심은 오디오 파일 로딩이 아니다

현재 구현에서 이미 확인된 사실은 다음과 같다.

| 항목 | 현재 상태 | 판단 |
|---|---|---|
| 보컬 WAV 준비 | 패키지 로드 시 디코딩·리샘플링·캐시 | 트리거 순간 파일 I/O가 주원인 아님 |
| phrase 요청 | onset과 같은 audio callback에서 요청 | 과거의 한 블록 commit 지연은 제거됨 |
| 오디오 믹스 | audio callback 안에서 처리 | UI/message thread 시작 지연이 현재 핵심은 아님 |
| 장치 버퍼 | 가능한 값 중 128 samples 우선 | 장치 자체 지연은 비교적 작음 |
| generic chord FFT | onset 후 약 75 ms 지점 분석 | 현재 phrase 시작을 직접 막지는 않음 |
| 악보 진행 | 다음 인쇄 코드 경계에서 strike를 기다리며 정지 | **현재 구조적 지연의 가장 유력한 원인** |
| 지연 보정 | 출력 장치 지연만 전달 | 입력 지연·분석 지연까지 같은 시간축으로 보정하지 않음 |

현재 `PhraseScheduler`에는 다음 코드 경계를 연주자가 칠 때까지 song time이 그 경계 직전에 고정되는 정책이 있다. 정상적으로 정확한 onset이 받아들여지면 안전하지만, 첫 strike가 timing gate에서 거절되면 다음 strike까지 기다리게 된다. 그 결과 반 박자 또는 한 박자 늦게 진행할 수 있다.

### 2.2 버퍼만 줄여서는 반 박자 지연을 해결할 수 없다

만찬가를 약 103 BPM으로 보면:

- 1박: 약 582.5 ms
- 반 박: 약 291.3 ms
- 1/4박: 약 145.6 ms
- 48 kHz / 128 samples 한 블록: 약 2.67 ms
- 48 kHz / 256 samples 한 블록: 약 5.33 ms

128에서 64 samples로 줄여 얻는 이론적 차이는 한 블록당 약 1.33 ms다. 반 박 지연 291 ms와는 두 자릿수 이상 차이가 난다. 128은 안정적인 기본값으로 유지하고, 64는 드라이버와 CPU가 허용하는 경우에만 실험한다.

### 2.3 “모든 onset을 바로 승인”하는 것도 해결책이 아니다

이전 실험에서 timing gate를 제거하고 모든 onset을 받아들이자, 실제 약 81초 연주가 149개 중 130개 조각까지 과도하게 전진했다. 스트럼의 여러 현, 아르페지오 음, 노이즈가 모두 악보 전진 신호가 될 수 있기 때문이다.

따라서 두 극단을 모두 피해야 한다.

- 너무 보수적: 확정된 chord event를 기다리므로 항상 뒤늦음
- 너무 공격적: 모든 onset을 가사 전진으로 사용하므로 폭주함

필요한 것은 **시간은 예측으로 진행하되, 관측은 시간과 위치를 제한된 범위에서 교정하는 방식**이다.

---

## 3. 성공 정의와 정량 목표

“지연”이라는 한 단어를 여러 개의 측정값으로 분리한다. 직접 모니터링, onset 검출, 즉각적 반응음, UI, 스케줄러, 음악적 동기 오차를 서로 섞지 않는다.

### 3.1 지연 지표

| ID | 측정값 | 최상 목표 | 출시 합격선 |
|---|---|---:|---:|
| L0 | 기타 직접 모니터링 | median ≤ 10 ms | P95 ≤ 15 ms |
| L1 | onset 검출 완료 | median ≤ 15 ms | P95 ≤ 25 ms, 최대 허용 P95 30 ms |
| L2 | onset → 즉각적 반응음 물리 지연 | median ≤ 20 ms | P95 ≤ 35 ms, 장치 편차 포함 상한 50 ms |
| L3 | onset 관측 → BeatClock 반영 | 다음 audio block | 최대 2 audio blocks |
| L4 | 보컬 예약 실행 오차·jitter | P99 ≤ 3 ms | P99 ≤ 1 audio block |
| L5 | 정상 `LOCKED` 구간 모음 동기 오차 | median absolute ≤ 20 ms, P95 ≤ 40 ms | median absolute ≤ 30 ms, P95 ≤ 60 ms |
| L6 | 위치 복구 시간 | 1박 이내 | 정상 신뢰도에서 1~2박 이내 |
| L7 | UI 반응 | 60 Hz 화면에서 P95 ≤ 33 ms | 오디오 지연 합격 여부와 별도 판정 |
| L8 | 체감 지연 | “거의 붙어 있음” | 기존 방식 대비 블라인드 A/B에서 명확한 우세 |

L0는 연주자가 자기 기타를 듣는 monitoring 경로다. 하드웨어 direct monitoring과 앱을 통과하는 software monitoring을 따로 기록한다. L1은 latency-compensated 실제 onset 시각부터 검출 결과가 사용 가능해질 때까지다. L2는 오디오 인터페이스, 드라이버, 검출, 출력 latency를 포함한 물리 경로다. L4는 소프트웨어 스케줄러의 정확도이고, L5가 최종 음악적 동기다.

L7은 오디오 지표가 아니다. 60 Hz 화면은 프레임 대기만 0~16.7 ms가 추가될 수 있으므로 “소리 20 ms”와 “화면 20 ms”를 동일한 합격선으로 묶지 않는다. 내부 UI event timestamp와 실제 화면 pixel 변화도 가능하면 분리해 측정한다.

### 3.2 체감 해석 기준

다음 범위는 절대적인 청각 법칙이 아니라 본 프로젝트의 초기 제품 판단 기준이다. 실제 블라인드 A/B 결과로 보정한다.

| 전체 음악 동기 오차 | 예상 체감 |
|---:|---|
| ±20 ms 이내 | 거의 붙어 있다고 느끼는 목표 영역 |
| ±30 ms 이내 | 일반 사용자가 대체로 자연스럽게 받아들이는 영역 |
| 약 40~60 ms | 박자가 약간 헐겁게 느껴질 수 있음 |
| 100 ms 이상 | 명확히 늦거나 빠르다고 느낄 가능성이 큼 |
| 반 박 이상 | 시스템이 연주를 뒤에서 따라오는 느낌 |

### 3.3 절대 합격 기준

103 BPM 기준 정상 실연에서:

- 반 박(약 291 ms) 이상 늦는 vocal anchor: **0회**
- 1/4박(약 146 ms) 이상 늦는 vocal anchor: **전체의 1% 미만**
- 100 ms 이상 벗어나는 vocal anchor: **최상 목표 0회, 출시 합격선 전체의 1% 미만**
- 정상 `LOCKED` 구간 vocal anchor의 95%: **±60 ms 이내**
- 갑작스러운 다중 phrase 시작 또는 catch-up burst: **0회**
- 정상 연주 중 score가 뒤로 점프: **0회**
- 아르페지오 때문에 한 박에 여러 가사가 소비되는 현상: **0회**
- 기타 정지 확정 후 새로운 음절 시작: 설정된 정지 한계 이후 **0회**
- 오디오 xrun/glitch: 합격 시연 take에서 **0회**

### 3.4 첫 가사와 인트로 목표

- 충분한 인트로가 있는 곡: 첫 모음 동기 오차 P95 ≤ 60 ms
- 인트로가 짧거나 없는 곡: 선택한 시작 정책에 따라 P95 ≤ 80 ms
- 첫 음을 듣고 “같은 물리적 순간”에 첫 가사를 내는 것은 목표가 아니다. 인과적으로 불가능하므로 count-in, arm 후 다음 박/마디 시작, 또는 명시된 pickup 정책 중 하나를 사용한다.

### 3.5 목표가 아닌 것

- 물리적 연산 시간 0 ms
- 처음 듣는 임의의 곡을 악보 없이 따라가기
- 기타 한 음만으로 템포·위치·코드를 완벽히 확정하기
- 모든 곡에 하나의 고정된 stop/coast 값 사용하기
- 처음부터 복잡한 HMM/Kalman 모델을 도입하는 것
- Rubber Band를 넣는 것만으로 지연 구조가 자동 해결된다고 가정하는 것

---

## 4. 목표 아키텍처

### 4.1 처리 계층

```text
1. Listen
   입력 latency를 보정한 sample timestamp
   onset / RMS / chroma / onset interval

2. Estimate
   beat position / tempo / phase / confidence
   현재 score event 후보와 국소적인 대안

3. Predict
   1~2박 범위의 beat와 다음 vocal target sample을 tentative하게 예측

4. Schedule
   자음 길이·출력 지연을 고려한 commit deadline 전에
   보컬 클립을 output sample timeline에 예약

5. Render
   audio callback에서 메모리 할당·파일 I/O·mutex 없이 직접 mix

6. Correct & Recover
   새 onset으로 위상·템포를 부드럽게 수정
   chord/chroma로 악보 위치가 틀렸을 때만 제한적으로 복구
```

### 4.2 BeatClock

최소 상태는 다음과 같다.

```cpp
struct BeatClock
{
    double beatPosition;
    double secondsPerBeat;
    double phaseErrorSeconds;
    double confidence;
    double lastObservationTime;
    int scoreEventIndex;
    ClockState state;
};
```

실제 구현에서는 모든 시각을 가능하면 `int64 sample` 기반의 단조 증가 시간축에 둔다. callback 시각, reported input latency, 분석 window의 group delay, output latency를 동일한 sample timeline으로 환산해야 한다.

미래 beat의 기본 예측식은 다음과 같이 부호가 `+`다.

```text
predictedTargetTime
= currentReferenceTime
  + (targetBeat - currentBeatPosition) × secondsPerBeat
```

`targetBeat`가 현재보다 미래라면 예측 시각도 현재보다 미래여야 한다. 입력 latency를 뺄 때와 미래 beat까지의 시간을 더할 때를 혼동하지 않는다.

### 4.3 상태 머신

| 상태 | 의미 | 보컬 정책 |
|---|---|---|
| `DISARMED` | 곡은 선택됐지만 추종 시작 전 | 재생 금지 |
| `ARMING` | 초기 onset을 모으며 템포·위상 추정 | 인트로/무가사 이벤트만 추적 |
| `LOCKED` | 템포·위상·위치가 충분히 안정 | look-ahead 예약 허용 |
| `COASTING` | 잠시 입력이 없지만 관성 허용 범위 안 | 짧게 예측 진행, 신뢰도 감소 |
| `HOLDING` | 입력 부재 또는 불일치가 한계 초과 | 새 음절 예약 금지, 안전한 tail 처리 |
| `RECOVERING` | 위치 후보가 갈렸거나 연주가 점프함 | 국소 탐색, 대규모 즉시 점프 금지 |

전이 조건은 곡별 하드코딩이 아니라 beat 단위 설정과 confidence로 표현한다.

### 4.4 템포·위상 갱신

초기 버전은 복잡한 확률 모델보다 검증 가능한 alpha-beta 계열 추정기로 시작한다.

- onset 시각과 현재 악보 후보의 예상 onset 시각 차이를 phase residual로 계산한다.
- 작은 residual은 위상을 빠르게 보정한다.
- 최근 onset 간격의 robust median/trimmed mean으로 tempo를 천천히 보정한다.
- 한 번의 이상 onset은 tempo를 크게 바꾸지 못한다.
- correction은 즉시 순간이동하지 않고 1~2박의 target horizon에 걸쳐 수렴한다.
- residual이 너무 크면 현재 후보에 억지로 붙이지 않고 recovery 후보로 보낸다.

필요할 때만 이후 단계에서 Kalman filter 또는 score-position HMM으로 교체한다. 교체 여부는 shadow-mode 로그가 단순 모델의 한계를 보여 줄 때 결정한다.

### 4.5 onset과 chord의 역할 분리

- **onset:** 위상 보정의 주 관측값, 템포 갱신의 주 관측값
- **RMS/silence:** 연주 중단, ghost onset, 연결 해제 판단
- **chroma/chord:** 현재 score event가 맞는지 검증하고 위치를 잃었을 때 복구
- **악보에 없는 세부 chord label:** 현재 곡 후보 밖이면 진단 정보로만 사용

코드 인식은 매번 12개 root와 모든 quality 중 하나를 확정하지 않는다. 현재 score position 주변의 악보 코드 후보만 비교한다. 예를 들어 현재 위치 주변에 C, G, Am, F만 있다면 이 후보와 `unknown`만 평가한다. 다만 동일 코드가 오래 반복되는 구간에서는 chord가 위치를 구분하지 못하므로 onset timing과 누적 beat position을 우선한다.

---

## 5. “그냥 노래를 틀어 놓는 것”이 되지 않게 하는 제약

예측 재생이 반주기와 다른 핵심은 **예측 범위가 제한되고, 연주자 관측 없이는 계속 진행할 권한을 잃는 것**이다.

예측과 예약은 세 단계로 나눈다.

| 단계 | 범위 | 취소/수정 | 실제 소리 |
|---|---|---|---|
| score prediction | 약 1~2박 | 자유롭게 수정 | 없음 |
| tentative vocal reservation | 다음 event 중심 | 새 관측으로 이동 가능 | 없음 |
| audio commit | 자음 pre-roll + 출력 latency + 안전 여유 전 | commit 이후 큰 이동 금지 | callback 도달 시 출력 |

따라서 “1~2박 뒤를 계산한다”는 것이 그만큼의 보컬을 미리 틀어 놓는다는 뜻은 아니다. 실제로 되돌릴 수 없는 범위는 필요한 자음 선행 시간과 장치 지연을 포함한 짧은 commit window뿐이다.

### 5.1 강제 제약

- 시작 전 기타 onset 없이 보컬은 절대 시작하지 않는다.
- score prediction은 기본 1~2박까지만 허용한다.
- tentative 예약은 새 관측에 따라 수정할 수 있다.
- 실제 audio commit window는 `max(80 ms, vowel/consonant pre-roll + output latency + safety margin)`으로 계산하고 상한을 둔다.
- confidence는 관측이 없으면 시간에 따라 감소한다.
- silence가 정지 한계를 넘으면 새 vocal onset을 금지한다.
- 현재 score 주변의 제한된 후보만 허용하며, 먼 위치로 한 번에 점프하지 않는다.
- 기타가 지속적으로 예상보다 빠르거나 느릴 때만 tempo가 변한다.

### 5.2 제안 기본값

- score prediction horizon: 2박
- 최소 audio commit look-ahead: 100 ms
- 최대 audio commit look-ahead: 200 ms(긴 자음은 별도 검수)
- phase correction horizon: 1박
- 큰 오차 correction horizon: 2박
- 짧은 coast: 0.5박
- hold 진입: 1박 동안 유효 관측 없음
- 위치 recovery 탐색: 현재 위치 기준 뒤 1개, 앞 2~4개 event

이 값은 시작점일 뿐이며, 오프라인 grid search와 실제 연주로 곡 독립적인 범위를 찾는다. 곡별로 달라야 하는 경우 beat 단위의 최소한의 정책만 패키지에 기록한다. 200 ms보다 긴 선행 발성이 필요한 clip은 조각 경계 또는 vocal anchor가 잘못되었을 가능성까지 포함해 수동 검수한다.

---

## 6. 보컬을 박자에 붙이는 방법

### 6.1 파일 시작이 아니라 지각되는 음절을 맞춘다

가령 클립에 자음이 70 ms 있고 모음이 그 뒤에 시작된다면 파일을 박 정각에 시작할 경우 가사는 최소 70 ms 늦게 느껴진다.

각 vocal event는 다음 메타데이터를 가진다.

```json
{
  "score_beat": 24.0,
  "audio_file": "vocal_0042.wav",
  "content_start_sec": 0.018,
  "consonant_preroll_sec": 0.072,
  "vowel_onset_sec": 0.090,
  "vowel_center_sec": 0.125,
  "sync_anchor": "vowel_onset"
}
```

예약 공식은 개념적으로 다음과 같다.

```text
clipStartSample
= predictedTargetSample
  - reportedOutputLatencySamples
  - vowelOnsetSamples
```

입력 관측도 다음처럼 물리 시간축으로 복원한다.

```text
physicalGuitarOnset
= inputCallbackTime
  - reportedInputLatency
  - detectorGroupDelay
```

### 6.2 메타데이터 생성과 검수

1. 자동 energy/phoneme 기반 후보 생성
2. 원본 보컬 stem 및 score note onset과 교차 정렬
3. 신뢰도 낮은 음절만 수동 검수 목록에 추가
4. consonant가 긴 한국어·일본어·영어 음절을 별도 표본으로 청취
5. 자동 검출값과 수동 표식의 차이를 기록

모음 onset을 정확히 찾기 어려운 호흡음, 무성 자음, melisma는 `note_attack`, `syllable_onset`, `vowel_center` 중 적절한 anchor를 선택할 수 있게 한다.

### 6.3 이미 시작된 오디오는 함부로 되감지 않는다

- 아직 commit 전인 예약: 이동 가능
- 자음 pre-roll이 시작되었지만 모음 전: 작은 보정만 허용
- 모음이 이미 출력 중: 순간 점프 금지
- 지속음: 필요할 때 제한적인 time-stretch로 다음 anchor에 수렴

Rubber Band는 이미 재생 중인 긴 sustain을 자연스럽게 늘이거나 줄이는 도구로 사용할 수 있지만, 예측 시계와 sample 예약을 대체하지 않는다. 1차 구현에서는 고정 클립으로 스케줄러를 먼저 증명하고, stretch는 별도 단계에서 A/B한다.

---

## 7. 인트로·무가사 구간 처리

인트로는 지연을 만드는 대기 구간이 아니라 **첫 가사를 위한 가장 좋은 calibration 구간**이다. 현재 우선 적용할 곡들은 모두 인트로가 있으므로, 첫 출시에서는 인트로를 예외 처리가 아닌 기본 시작 경로로 취급한다.

### 7.1 인트로가 지연을 숨기고 줄여 주는 이유

예를 들어 첫 가사가 5마디에 시작한다면 시스템은 다음처럼 움직일 수 있다.

```text
1마디: 실제 연주 시작과 첫 score-position 후보 확보
2마디: BPM·beat phase 초기 추정
3마디: 위치·tempo·confidence 안정화
4마디: 첫 vocal target을 tentative 예약하고 commit 시각 계산
5마디: 자음을 선행 출력해 첫 모음을 목표 박에 정렬
```

첫 보컬 전까지 수초의 관측 시간이 있으므로 다음 정보를 미리 확보할 수 있다.

- 곡이 실제로 시작했는지
- 첫 마디와 현재 score position
- 현재 BPM
- 사용자가 원곡보다 빠른지 느린지
- beat phase
- 다음 가사와 첫 모음의 예상 도착 시각
- 출력 latency와 자음 pre-roll을 포함한 clip commit deadline

따라서 첫 보컬 100~200 ms 전에 이미 필요한 audio commit을 완료할 수 있다. 인트로가 길수록 무조건 좋아지는 것은 아니지만, 최소 2~4마디의 유효한 기타 관측은 첫 가사의 인과적 지연을 대부분 예측 구간 안으로 숨길 수 있다.

Music Plus One처럼 알려진 악보 위치와 과거 timing 관측으로 미래 이벤트 시각을 예측하는 구조가 이 목적에 맞는다. SongDriver의 미래 구간 준비·캐시 개념은 보조적인 설계 참고로만 사용한다.

인트로에서 지연이 발생한다면 “인트로가 있기 때문”이 아니라 다음 구조를 먼저 의심한다.

- 인트로 chord/score event가 최종 패키지에서 제거됨
- 인트로를 `instrumental` section으로 표시하지 않음
- 스케줄러가 첫 코드의 절대 score position을 0으로 잘못 재기준화함
- 매 intro 코드를 완전히 검출해야만 다음 위치로 이동함
- 첫 vocal event를 곡 시작 event로 사용함

### 7.2 패키지의 section 모델

```json
{
  "id": "intro_1",
  "start_beat": 0.0,
  "end_beat": 16.0,
  "type": "instrumental",
  "vocal_allowed": false,
  "clock_policy": "lock_from_guitar",
  "entry_policy": "predict_first_vowel"
}
```

section type은 최소 다음을 지원한다.

- `count_in`
- `instrumental_intro`
- `vocal`
- `instrumental_break`
- `rest`
- `outro`

### 7.3 인트로 동안 수행할 작업

1. 첫 onset으로 `ARMING` 진입
2. onset 간격 분포를 모아 subdivision 후보 제거
3. 악보의 intro chord sequence와 local alignment
4. tempo의 robust 초기값 계산
5. beat phase 후보를 여러 개 유지한 뒤 관측으로 제거
6. confidence를 갱신하고 UI에 `ARMING → LOCKED` 표시
7. 첫 vocal event, 자음 pre-roll, 출력 latency를 미리 계산
8. 첫 가사 1~2개 클립을 캐시에 재확인
9. 첫 모음 목표 시각 전에 clip을 sample timeline에 예약

`vocal_allowed=false`인 동안에는 score position이 vocal 경계에 가까워져도 보컬을 내지 않는다.

### 7.4 만찬가

현재 패키지의 선행 9개 chord event와 4마디 intro를 calibration에 사용한다. 첫 가사가 약 4마디 뒤에 있다는 사실을 시간초 하드코딩으로 처리하지 않고, section과 score beat로 표현한다.

검증 포인트:

- 인트로 중 잘못된 vocal 출력 0회
- 2마디 이내 `LOCKED` 도달
- 3~4마디에서 템포가 안정
- 첫 모음의 오차가 P95 60 ms 이내
- 인트로 마지막 코드가 아르페지오여도 첫 가사 중복 트리거 없음

### 7.5 Don't Look Back in Anger 인트로 누락 진단

실제 시연에서 인트로 약 2마디를 거치지 않고 첫 가사가 바로 나온다는 현상을 별도 결함으로 추적한다.

#### 현재 확인된 잠정 원인

읽기 전용으로 현재 빌드 산출물을 비교한 결과:

- `build/mode1/dont_look_back_in_anger/chords.json`에는 0초부터 10.741890초까지의 인트로 코드 event가 존재한다.
- 같은 파일의 초반에는 `Am → Am → F → C → Am → Am → F → C` 계열 event가 들어 있다.
- 첫 보컬 phrase는 `song_package.json`에서 11.184초에 시작한다.
- 그러나 최종 `song_package.json`의 `chord_timeline` 첫 event도 11.184초의 `C`다.
- 첫 `micro_phrase` 역시 11.184초의 `Slip...`이다.

즉 원본 chord 자료에는 인트로가 있지만, 실제 앱이 읽는 최종 package timeline에서는 첫 가사 이전 chord event가 빠져 있다. 현재 scheduler는 첫 매칭 event의 `startSeconds`로 `songTimeSeconds`를 이동시키므로, 첫 기타 onset이 11.184초 event로 매칭되면 첫 가사가 즉시 due가 될 가능성이 높다.

이것은 강한 잠정 진단이지만, 실제 수정 전에는 런타임 로그로 확정한다.

#### 진단 순서

1. **원천 악보 확인**
   - MusicXML의 첫 vocal note measure와 beat
   - GP5/chord 자료의 첫 기타 event
   - 원곡/stem의 실제 첫 vocal onset
2. **빌드 단계 추적**
   - 각 변환 스크립트 실행 전후의 첫 chord time, 첫 vocal time, event 수
   - intro prepend/promote/refresh 단계에서 timeline이 잘리는 지점
   - 절대 시간과 vocal-relative 시간이 섞이는지
3. **최종 package 불변조건 검사**
   - 첫 score/chord event가 곡 시작 beat에 존재
   - 첫 vocal event가 첫 score event보다 뒤에 존재
   - 그 사이가 `instrumental_intro`, `vocal_allowed=false`
   - 예상 intro event 수가 보존됨
   - first vocal beat 이전 vocal event가 0개
4. **런타임 추적**
   - 앱이 실제로 로드한 package 절대 경로와 hash
   - reset 직후 song time, current/next chord index
   - 첫 12개 raw/accepted onset과 매칭 score event
   - 최초 vocal reservation/commit/start의 score beat와 이유
5. **오디오 확인**
   - intro-only 기타 입력에서는 vocal 출력이 완전 무음인지
   - 첫 vocal target 전후 파형과 event log가 일치하는지

#### 수정 원칙

- “약 2마디”를 고정 초 단위 silence로 앞에 붙이지 않는다.
- 누락된 intro chord/score event를 최종 공통 timeline에 복원한다.
- 곡 시작 origin, 첫 instrumental event, 첫 vocal event를 각각 명시한다.
- intro 동안 BeatClock은 진행하고 관측을 누적하되 vocal gate는 닫는다.
- 첫 vocal은 지정 beat를 예측해 예약하며, 그 beat의 코드를 검출한 뒤에야 사후적으로 시작하지 않는다.

#### Oasis 합격 조건

- 최종 package에 원천 악보와 일치하는 intro section 및 event 수가 존재
- intro-only 입력에서 vocal non-zero sample 0개
- 첫 vocal reservation이 지정 entry보다 100~200 ms 앞서 생성됨
- 첫 vocal commit/start가 지정 entry와 일치
- 원 템포, ±10%, ±20% 실연 모두 intro 길이를 beat 기준으로 보존
- 첫 onset 직후 `Slip...`이 나오는 현상 0회
- 영어 자음 pre-roll과 첫 모음을 따로 측정해 첫 모음 P95 ≤ 60 ms

### 7.6 인트로가 없는 곡

첫 기타음을 듣고 같은 순간의 첫 가사를 정확히 출력할 수는 없다. 다음 정책 중 곡 패키지에 명시된 하나를 사용한다.

- `count_in_then_start`: 2~4박 count-in 뒤 시작
- `arm_then_next_beat`: 첫 유효 코드로 arm하고 다음 박에 보컬
- `arm_then_next_bar`: 첫 유효 코드로 arm하고 다음 마디에 보컬
- `pickup_window`: 약속된 pickup 구간을 이용해 첫 정박에 보컬

기본 권장은 `arm_then_next_bar`이며, 사용자가 즉각성을 더 중시하면 `arm_then_next_beat`를 비교한다.

### 7.7 중간 간주와 휴지

- vocal event가 없는 동안 clock과 score follower는 계속 동작한다.
- 기타가 계속되면 tempo·phase confidence를 유지한다.
- 기타까지 쉬는 악보상 rest에서는 silence를 “연주 중단”으로 오해하지 않는다.
- 악보가 기타 onset을 기대하는데 silence가 지속될 때만 confidence를 낮춘다.
- 다음 vocal entry 전에 최소 1~2개의 유효 관측을 확보하지 못하면 보수적인 entry 정책을 사용한다.

---

## 8. 모든 곡에 적용하기 위한 일반화된 패키지

엔진은 곡 제목이나 특정 코드 이름을 몰라야 한다. 곡별 차이는 package data로만 표현한다.

### 8.1 공통 타임라인

```json
{
  "schema_version": 2,
  "meter_map": [],
  "tempo_hint_map": [],
  "sections": [],
  "score_events": [],
  "vocal_events": [],
  "local_chord_vocabulary": [],
  "entry_policy": {},
  "stop_policy": {}
}
```

각 score event는 다음을 표현한다.

- beat 위치와 길이
- 기대 onset 여부
- chord pitch-class set
- 동일 chord 반복 여부
- strum/arpeggio 허용 패턴
- 위치 식별력
- optional tab/rhythm hint

각 vocal event는 다음을 표현한다.

- score beat
- 음절 또는 mora의 clip
- 자음 pre-roll, 모음 onset/center
- sustain/release
- sync anchor와 허용 오차
- section

### 8.2 입력 자료에서 패키지까지

```text
MusicXML / GP5 / MIDI / chord chart
        ↓
beat·meter·tempo·note·chord 정규화
        ↓
보컬 stem 및 가사와 음절 정렬
        ↓
section / entry / rest / instrumental 표식
        ↓
곡 독립 schema 검증
        ↓
오프라인 synthetic performance 자동 생성
        ↓
package acceptance test
```

MusicXML에 보컬 멜로디가 있고 기타가 별도 트랙이면 가장 좋지만, 반드시 하나의 파일에 붙어 있을 필요는 없다. 공통 beat timeline으로 정렬할 수 있으면 별도 소스도 처리한다.

현재 우선 적용 곡은 모두 인트로가 있으므로 package validator는 원천 자료에 첫 vocal 이전 event가 존재하는데 최종 package에서 사라지면 빌드를 실패시킨다. `first_chord_time == first_vocal_time`인 패키지는 `entry_policy=no_intro`가 명시된 경우에만 허용한다.

### 8.3 일반화의 합격 범위

첫 출시는 다음 두 실제 곡에 대해 완전 합격해야 한다.

- 만찬가: 4마디 intro, 일본어 음절, 세분화된 149 vocal control pieces
- Don't Look Back in Anger: 영어 음절, 210 pieces, 다른 chord/rhythm 패턴

추가로 실제 음원을 만들지 않아도 다음 synthetic archetype을 통과해야 한다.

- 인트로 없음
- pickup/anacrusis
- 동일 코드가 여러 마디 반복
- 스트럼과 8분음표 아르페지오 혼합
- 중간 간주와 긴 rest
- 의도적 한 코드 누락
- 한 박 일찍/늦게 연주
- 3/4 또는 6/8
- tempo가 점진적으로 ±20% 변함
- 박자표 또는 tempo map 변경

두 실제 곡만 통과한 상태에서는 “현재 두 곡에 일반화됨”이라고 표현한다. 위 archetype과 세 번째 독립 곡까지 통과해야 “일반화된 파이프라인”으로 판정한다.

---

## 9. 기타 미연결 환경과 실제 환경을 분리한 검증

### 9.1 기타 없이 검증할 수 있는 것

- 동일 입력에 대한 결정론
- block size에 따른 scheduler 오차
- synthetic strum/arpeggio 처리
- 기존 녹음의 score alignment
- onset 누락·추가·노이즈에 대한 견고성
- tempo·phase estimator의 수렴
- vocal 예약 sample의 정확도
- stop/coast/recovery 상태 전이

현재 fixture:

- 최신 만찬가 실제 기타 녹음
- 이전 만찬가 복구 녹음  
  `output/audio/guitar_tests/bansanka_previous_guitar_take_recovered.wav`
- 만찬가 synthetic strum/arpeggio
- Oasis synthetic strum/arpeggio

Oasis는 최종 실연 합격을 위해 실제 기타 take가 최소 3개 필요하다.

### 9.2 기타 없이 검증할 수 없는 것

- 오디오 인터페이스의 실제 round-trip latency
- ASIO 드라이버의 reported latency 정확도
- 기타 pickup과 케이블의 노이즈
- 연주자가 느끼는 반응성
- 스피커/헤드폰에서 보컬이 기타와 붙는 지각
- 실제 연주자가 예측형 동작을 “내가 통제한다”고 느끼는지

따라서 offline renderer 결과만으로 “실제 지연 30 ms”라고 주장하지 않는다.

### 9.3 세 가지 실행 환경

| 환경 | 목적 | 합격 주장의 범위 |
|---|---|---|
| Offline deterministic replay | 알고리즘 회귀·정확도 | 소프트웨어 로직 |
| Audio loopback | 물리적 I/O·검출·출력 지연 | 장치 포함 latency |
| Live guitar A/B | 체감·통제감·음악성 | 최종 사용자 경험 |

### 9.4 block size 행렬

모든 offline fixture를 64, 128, 256, 512, 1024 samples에서 재생한다.

- score decision은 block size가 달라도 동일하거나 허용 범위 안이어야 한다.
- 예약 시각 차이는 최대 한 block 이내여야 한다.
- 512/1024는 제품 권장값이 아니라 알고리즘이 블록 경계에 잘못 의존하는지 찾는 stress test다.
- 실제 앱 기본값은 128, 불안정한 장치의 fallback은 256으로 한다.

---

## 10. 단계별 검증 실험

### Test A0 — 기타 직접 모니터링

목적: score follower와 onset detector를 완전히 우회하고, 연주자가 듣는 자기 기타의 최소 지연을 측정한다. 이 값이 크면 보컬 엔진이 정확해도 전체 연주 경험이 늦게 느껴진다.

경로를 두 개로 나눈다.

1. 오디오 인터페이스의 hardware direct monitoring
2. 앱 input callback → output callback의 software monitoring

절차:

1. 기타 입력 원신호와 monitoring 출력을 두 채널로 동시에 loopback 녹음한다.
2. 동일한 피킹 transient 사이의 sample 차이를 측정한다.
3. ASIO 64/128/256에서 각 50회 이상 반복한다.
4. hardware direct monitoring과 software monitoring 결과를 섞지 않고 기록한다.

합격:

- 최상 목표 median ≤ 10 ms
- software monitoring 출시 합격선 P95 ≤ 15 ms
- hardware direct monitoring이 제공되는 경우 그 값을 사용자가 선택할 수 있게 함
- monitoring 경로에 불필요한 분석 window, resampling 또는 message thread 경유 0회

### Test A — 기타 onset → 메모리 클릭

목적: score follower와 vocal clip을 제거하고, 입력부터 출력까지의 최소 경로를 잰다.

절차:

1. 오른쪽 또는 별도 channel에 짧은 기준 클릭을 넣는다.
2. 실제 기타 신호와 출력 클릭을 동시에 외부/loopback으로 녹음한다.
3. 입력 기타 onset과 출력 클릭 onset을 sample 단위로 측정한다.
4. 128/256 buffer, ASIO/WASAPI 사용 여부를 기록한다.
5. 50회 이상 반복해 median, P95, max를 계산한다.
6. latency-compensated onset 시각부터 detector 결과 생성까지의 L1과, 물리 기타 onset부터 출력 click까지의 L2를 동시에 계산한다.
7. UI 반응도 켠 경우 내부 UI event timestamp와 실제 화면 변경 시각을 별도 열에 기록한다.

합격:

- onset 검출 완료: 최상 목표 median ≤ 15 ms, 출시 합격선 P95 ≤ 25 ms
- onset → 반응음: 최상 목표 median ≤ 20 ms, 출시 합격선 P95 ≤ 35 ms
- 장치 편차를 포함한 물리 반응음 상한은 P95 ≤ 50 ms이며, 35~50 ms이면 원인을 리포트에 명시
- onset 관측의 BeatClock 반영은 목표 다음 audio block, 최대 2 blocks
- 60 Hz 실제 화면 반응은 P95 ≤ 33 ms를 목표로 하되 오디오 합격과 별도 판정
- message thread 경유 0회
- 클릭 cache miss, allocation, mutex wait 0회

실패 시 먼저 확인:

- 입력·출력 장치가 같은 인터페이스인지
- reported input/output latency
- detector window와 hop
- confirmation frame 수
- driver mode
- xrun

### Test B — 고정 transport → 보컬

목적: 기타 인식 없이 vocal scheduling과 음절 내부 offset만 검증한다.

절차:

1. score tempo 그대로 deterministic transport 실행
2. 목표 beat마다 vocal event 예약
3. mixed output을 렌더링
4. 목표 beat와 파일 시작, 첫 energy, vowel onset/center를 각각 비교

합격:

- 예약 실행 오차 P99 ≤ 1 block
- 모음 동기 오차 median absolute ≤ 20 ms
- clip 경계 click/pop 0회

이 단계가 실패하면 score following을 조정하지 않는다. 먼저 vocal metadata와 renderer를 고친다.

### Test B0 — 인트로 보존과 첫 vocal entry

목적: 원천 악보부터 최종 package와 런타임까지 인트로 event가 보존되고, 그동안 clock은 학습하지만 보컬은 나오지 않는지 검증한다.

절차:

1. 원천 MusicXML/GP5/chord timeline의 첫 event와 첫 vocal beat를 추출한다.
2. 중간 및 최종 package의 동일 좌표와 event 수를 비교한다.
3. intro-only synthetic guitar를 입력하고 vocal output을 렌더링한다.
4. 원 템포와 ±10%, ±20% tempo로 반복한다.
5. `ARMING`, `LOCKED`, 첫 tentative reservation, commit, 첫 non-zero vocal sample을 기록한다.

합격:

- 현재 대상 곡 모두 첫 vocal 이전 intro event가 최종 package에 보존
- `vocal_allowed=false` 구간의 vocal non-zero sample 0개
- 인트로 2마디 이내에 `LOCKED` 또는 명시된 최소 confidence 도달
- 첫 vocal reservation이 commit deadline 전에 존재
- 첫 vocal onset과 모음이 package의 지정 entry beat에 정렬
- 첫 기타 onset 하나만으로 첫 vocal phrase가 즉시 시작되는 경우 0회

### Test C — 기타는 tempo·phase만 교정

목적: chord 판단 없이 predictive clock 자체를 검증한다.

입력:

- synthetic steady strum
- humanized ±10/20/40 ms jitter
- 점진적 tempo ±20%
- 실제 이전/최신 만찬가 take

합격:

- 정상 구간 모음 P95 ≤ 60 ms
- 단일 extra onset으로 tempo 변화 ≤ 설정 한계
- 아르페지오 subdivision을 beat로 오인해 2배 tempo가 되는 경우 0회
- 입력 중단 후 정책대로 `COASTING → HOLDING`

### Test D — 제한된 chord/score recovery

목적: chord를 재생 버튼이 아니라 위치 복구 센서로 추가한다.

교란:

- 한 onset 누락
- 한 코드 건너뜀
- 동일 chord 반복
- 틀린 코드 1회
- 한 마디 재시작
- 노이즈 onset 추가

합격:

- 작은 누락은 clock이 끊기지 않음
- 실제 위치 이탈은 1~2박 안에 복구
- 먼 event로 오점프 0회
- 복구 시 vocal catch-up burst 0회

### Test E — 전체 시스템

목적: predictor, local chord recovery, vocal anchor, stop policy를 함께 검증한다.

필수 실행:

- 두 곡 synthetic strum
- 두 곡 synthetic arpeggio
- 만찬가 이전 실제 take
- 만찬가 최신 실제 take
- 두 곡 live guitar 각 3 take 이상

합격 기준은 3장의 정량 목표와 14장의 실연 기준을 모두 만족해야 한다.

---

## 11. Fault-injection 검증 매트릭스

| 상황 | 기대 동작 | 금지 동작 |
|---|---|---|
| 짧은 ghost onset | phase에 거의 영향 없음 | 가사 하나 소비 |
| 스트럼의 여러 현 | 하나의 musical attack으로 묶거나 낮은 가중치 | tempo 2~6배 증가 |
| 아르페지오 | subdivision으로 사용 가능, score beat 유지 | 각 음마다 가사 진행 |
| 한 코드 누락 | clock으로 짧게 유지 | 즉시 freeze |
| 연속 1박 silence | 설정에 따라 coast 후 hold | 곡 끝까지 자동 진행 |
| 악보상 rest | 정상 score 진행 | 연주 중단으로 오판 |
| 틀린 코드 1회 | confidence 감소, 위치 유지 | 먼 위치 점프 |
| 반복적으로 틀린 코드 | recovering/holding | 잘못된 lyric 계속 재생 |
| USB/입력 disconnect | 즉시 안전 hold | 노이즈를 onset으로 처리 |
| CPU spike | 예약된 작은 범위만 정상 출력 | 메모리 할당·deadlock |
| 패키지 누락 파일 | 로드 단계 실패와 명확한 오류 | 실연 중 disk lookup |
| 잘못된 vowel metadata | 품질 검증 실패 | 조용히 늦은 상태로 배포 |

각 fault는 자동 테스트에서 seed를 고정해 재현 가능하게 만든다.

---

## 12. 계측 설계

### 12.1 반드시 기록할 sample timestamp

- input callback 시작 sample
- reported input latency
- raw onset sample
- detector group-delay 보정 onset sample
- onset accept/reject와 이유
- 예상 beat/score position
- phase residual
- tempo estimate
- confidence와 state
- vocal target sample
- reservation sample
- commit sample
- 첫 non-zero mixed sample
- vowel target sample
- reported output latency
- xrun count

### 12.2 실시간 안전성

audio thread에서는 문자열 포맷, 파일 쓰기, 동적 할당, UI 호출을 하지 않는다. 고정 크기 POD event를 lock-free SPSC ring buffer에 넣고 비실시간 thread가 CSV/JSON으로 저장한다.

### 12.3 결과 산출물

각 run은 다음을 남긴다.

```text
output/mode1_predictive_runs/<run_id>/
  config.json
  environment.json
  events.csv
  metrics.json
  alignment.png
  output.wav
  notes.md
```

`environment.json`에는 commit/hash, package version, OS, driver, sample rate, buffer, reported input/output latency, 장치명, xrun을 기록한다.

### 12.4 앱 진단 UI

개발 모드에서 다음을 표시한다.

- driver type
- sample rate / buffer
- input/output reported latency
- 현재 BPM / beat / score index
- clock state / confidence
- look-ahead
- phase residual
- accepted/rejected onset 수와 이유
- 마지막 onset 검출 완료 latency
- 마지막 BeatClock 반영 block 수
- UI event 생성부터 표시 요청까지의 latency
- xrun

사용자 모드에서는 `대기`, `박자 맞추는 중`, `준비됨`, `연주를 기다리는 중`처럼 단순화한다.

UI 수치는 디버깅 보조용이다. 화면 표시가 한 프레임 늦었다는 이유로 audio scheduler를 늦추거나 UI와 오디오를 동기화하기 위해 audio thread를 대기시키지 않는다.

---

## 13. 기준선과 비교 방법

### 13.1 기준선 동결

구현 전에 현재 상태를 별도 baseline으로 기록한다.

- 현재 소스 commit 또는 working-tree patch hash
- 두 곡 package manifest/hash
- 현재 설정값
- 이전·최신 녹음 fixture hash
- synthetic generator seed
- block size별 현재 결과
- 현재 실제 기타 take의 화면/음원/로그

사용자의 기존 수정 파일은 덮어쓰지 않는다. 새 구현은 feature flag 또는 별도 scheduler strategy로 추가한다.

### 13.2 동일 입력 비교

같은 녹음 파일을 다음 두 엔진에 넣는다.

- A: 현재 event-gated/freeze baseline
- B: predictive BeatClock

비교:

- vocal anchor error 분포
- 반 박 이상 stall 수
- score progress
- extra/missed onset 민감도
- stop 후 추가 음절 수
- CPU와 xrun

### 13.3 현재 알려진 회귀 기준

- 만찬가 fine synthetic strum: 149/149
- 만찬가 fine synthetic arpeggio: 149/149
- Oasis fine synthetic strum: 210/210
- Oasis fine synthetic arpeggio: 210/210
- skip/expire: 0

새 엔진은 이 완주 회귀를 유지하면서, 실제 녹음에서 반 박 stall을 줄여야 한다. 단순히 더 많은 piece를 소비하는 것은 성공이 아니다. 예상 score beat와 시간 정렬이 맞아야 한다.

---

## 14. 실제 기타 시연에서 “유의미한 차이”를 증명하는 방법

### 14.1 시연 조건

- 동일 기타, 오디오 인터페이스, gain, driver, buffer
- 가능하면 헤드폰 사용
- baseline과 predictive 출력 loudness matching
- 각 곡 3 take 이상
- 스트럼 take와 아르페지오 take 분리
- 한 번은 의도적 pause, 한 번은 tempo 변화, 한 번은 코드 누락 포함

### 14.2 블라인드 A/B

가능하면 사용자가 A/B 중 어떤 엔진인지 모르게 순서를 무작위로 한다.

평가 항목을 1~7점으로 기록한다.

- 내 기타에 즉시 붙는 느낌
- 가사가 뒤에서 끌려오는 느낌의 부재
- 내가 템포를 통제한다는 느낌
- 멈췄을 때 함께 멈춘다는 느낌
- 아르페지오 안정성
- 가사 명료도
- “그냥 원곡을 틀어 놓은 것 같다”는 느낌의 정도

### 14.3 체감 합격 기준

- 두 곡 모두 predictive를 3회 중 최소 2회 이상 선호
- 전체 평가에서 responsiveness 중앙값이 baseline보다 최소 2점 향상
- control/autopilot 항목이 baseline보다 나빠지지 않음
- 측정 로그에서 반 박 이상 stall 0회
- 연주자와 별도 청취자 모두 첫 vocal entry를 어색하다고 표시한 take가 20% 미만

### 14.4 최종 데모 시나리오

1. 만찬가 4마디 intro 정상 연주
2. 첫 가사 정렬 확인
3. verse 중 약간 빠르게 전환
4. 한 코드 의도적 누락
5. 짧은 정지 후 재개
6. 아르페지오로 전환
7. Oasis로 곡 변경
8. 영어 첫 가사와 긴 자음 처리 확인
9. baseline/predictive 즉시 전환 비교

데모 성공은 완주 여부보다 “지연 감소와 통제 유지가 동시에 보이는가”로 판단한다.

---

## 15. 구현 단계와 각 단계의 중단 조건

### Phase 0 — 기준선 동결

작업:

- 현재 코드·패키지·fixture hash 저장
- 기존 두 곡의 offline/live 기준 측정
- Oasis 원천 `chords.json`과 최종 `song_package.json`의 intro event 차이 기록
- 앱이 실제 로드하는 package 경로/hash 및 첫 vocal trigger 로그 기록
- feature flag 설계

완료 조건:

- 동일 입력으로 baseline을 언제든 재현 가능

중단 조건:

- 기존 fixture 또는 package version을 식별할 수 없음

### Phase 1 — 계측과 지연 분해

작업:

- sample timestamp event ring buffer
- input/output latency 표시
- Test A0, Test A, Test B harness
- 지연 리포트 자동 생성

완료 조건:

- onset, 예약, 출력, vowel anchor를 하나의 sample timeline에서 설명 가능

중단 조건:

- measured latency와 로그 계산이 loopback에서 일치하지 않음

### Phase 2 — BeatClock shadow mode

현재 음향 결과는 baseline이 담당하고, 새 BeatClock은 소리를 내지 않은 채 예측만 로그에 남긴다.

작업:

- 상태 머신
- tempo/phase/confidence
- intro lock
- stop/coast
- block-size 회귀

완료 조건:

- Test C 합격
- 기존 take에서 baseline보다 예측 오차가 작음
- 아르페지오 폭주 0회

롤백:

- 음향 경로에 영향이 없으므로 shadow module 제거 가능

### Phase 3 — 예측형 sample scheduler

작업:

- 80~150 ms look-ahead
- audio callback sample 예약
- commit window
- fixed clip로 Test B/C/E 실행

완료 조건:

- L2 합격
- 반 박 stall 0회
- stop policy 합격

롤백:

- feature flag로 baseline scheduler 선택

### Phase 4 — vowel-anchor 정렬

작업:

- 자동 vowel/consonant metadata
- 수동 검수 도구와 confidence
- pre-roll 예약

완료 조건:

- L3 합격
- 한국어/일본어/영어 표본 모두 청취 합격

롤백:

- metadata가 없는 event는 기존 content offset 사용

### Phase 5 — local chord recovery

작업:

- score 주변 chord 후보만 평가
- 동일 chord 구간의 식별력 가중치
- recovery horizon

완료 조건:

- Test D 및 fault matrix 합격
- 정상 연주 정확도가 phase-only보다 나빠지지 않음

롤백:

- chord evidence를 diagnostic-only로 전환

### Phase 6 — 패키지 파이프라인 일반화

작업:

- schema v2
- MusicXML/GP5 importer 정규화
- section/intro/rest/vowel metadata 생성
- 원천 대비 intro event 보존 불변조건
- `no_intro`가 아닌 곡의 first score/chord와 first vocal 분리 검사
- package validator와 synthetic archetype generator

완료 조건:

- 만찬가와 Oasis를 같은 명령·schema로 재생성
- 곡 이름 하드코딩 0개
- archetype 자동 테스트 합격

### Phase 7 — live A/B와 출시 후보

작업:

- loopback 50회 이상
- 두 곡 live take
- 블라인드 A/B
- 최종 demo와 결과 보고서

완료 조건:

- 3장, 10장, 14장 기준 모두 합격
- 사용자가 predictive를 실제 시연 기본값으로 승인

---

## 16. 파라미터 튜닝 원칙

개별 녹음 하나에 맞춰 손으로 수치를 고정하지 않는다.

1. training fixture와 validation fixture를 분리한다.
2. 같은 연주의 다른 구간도 validation으로 유지한다.
3. objective function은 timing error만이 아니라 false advance, stop overrun, recovery jump를 함께 포함한다.
4. 곡별 값보다 공통값을 우선한다.
5. 곡별 override가 필요하면 음악적 속성(beat, meter, section)으로 설명 가능해야 한다.
6. 실제 test take를 튜닝에 사용한 경우 별도 blind take로 재검증한다.

예시 목적 함수:

```text
score =
  medianAbsoluteVowelError
  + 2 × P95VowelError
  + 300 ms × falseAdvanceCount
  + 500 ms × halfBeatStallCount
  + 500 ms × catchUpBurstCount
  + stopOverrunPenalty
```

---

## 17. 위험 요소와 대응

### 17.1 예측이 너무 강해 반주기처럼 느껴짐

대응:

- look-ahead 상한
- confidence decay
- 짧은 coast와 명확한 hold
- stop test를 정량 합격 조건으로 유지

### 17.2 예측이 너무 약해 기존 지연이 남음

대응:

- chord 확정을 phrase 시작 gate에서 제거
- shadow log에서 freeze 발생 위치 확인
- prediction horizon과 phase gain을 offline grid search

### 17.3 아르페지오가 tempo를 두 배로 만듦

대응:

- score의 subdivision 후보와 비교
- onset interval cluster
- beat-level observation 가중치
- chord boundary와 metrical accent 사용

### 17.4 첫 가사만 늦음

대응:

- intro에서 충분히 lock
- vowel pre-roll을 vocal entry 이전에 예약
- 인트로 없는 곡은 명시적 entry policy

### 17.5 긴 음이 tempo 변화에 못 따라감

대응:

- 먼저 다음 event scheduling으로 해결
- 남은 문제만 stretch 대상
- stretch 사용 전후 artifact와 latency 별도 측정

### 17.6 코드가 반복되어 위치를 알 수 없음

대응:

- chord보다 beat clock 우선
- 반복 구간 내부에서는 먼 recovery 금지
- 이후 고유 chord transition에서 위치 확정

### 17.7 입력 장치가 끊겼는데 노이즈가 들어옴

대응:

- RMS와 spectral validity 병행
- device state 감지
- 즉시 `HOLDING`, 새 가사 금지

### 17.8 CPU 최적화가 음악 로직과 섞임

대응:

- 실시간 안전성 테스트를 별도 유지
- allocation/mutex/file I/O 계측
- 알고리즘 정확도와 xrun 리포트 분리

---

## 18. 구현 전 합의할 제품 선택

다음은 코드 문제가 아니라 연주 경험의 선택이다.

| 선택 | 권장 초깃값 | 대안 |
|---|---|---|
| 인트로 없는 곡 시작 | 첫 코드 arm 후 다음 마디 | 다음 박 또는 count-in |
| 짧은 입력 공백 | 0.5박 coast | 즉시 hold / 1박 coast |
| hold 진입 | 1박 유효 관측 없음 | section별 설정 |
| audio commit look-ahead | 최소 100 ms, 동적 계산 | 80~200 ms |
| 동기 기준 | vowel onset | vowel center / syllable attack |
| phase 수렴 | 1박 | 큰 오차는 2박 |
| 앱 기본 buffer | 128 | 불안정 시 256 |
| 64 buffer | 실험 전용 | 지원하지 않음 |
| chord 역할 | local recovery | timing과 recovery 병행 |
| 직접 모니터링 | hardware direct 또는 저지연 software monitor | 사용자가 on/off 선택 |
| 수치 판정 | 최상 목표와 출시 합격선 병기 | 단일 합격선 |

실제 구현 전에 이 표의 기본값을 승인하거나 수정한다.

---

## 19. 완료 정의

다음이 모두 참일 때만 작업을 완료로 본다.

- [ ] 현재 baseline을 동일 입력으로 재현할 수 있다.
- [ ] Test A0에서 hardware/software 직접 모니터링 지연을 분리해 측정했다.
- [ ] Test A에서 실제 장치 포함 onset-to-click 지연을 측정했다.
- [ ] onset 검출, 반응음, BeatClock 반영, UI 반응을 서로 다른 지표로 기록했다.
- [ ] Test B에서 vocal clip 자체의 offset을 분리해 검증했다.
- [ ] BeatClock이 shadow mode에서 실제 녹음과 synthetic fixture를 통과했다.
- [ ] 기타 미연결 테스트가 물리 latency를 증명하지 않는다는 한계를 문서화했다.
- [ ] 만찬가 인트로 동안 보컬 0회, 첫 가사 목표 오차 합격.
- [ ] Oasis 최종 package에서 누락된 인트로 event의 발생 단계를 확정하고 복원했다.
- [ ] Oasis 첫 기타 onset 직후 `Slip...`이 나오는 현상이 재현 테스트에서 0회다.
- [ ] 현재 대상 곡 모두 Test B0 인트로 보존·첫 vocal entry 검사를 통과했다.
- [ ] Oasis 실제 기타 take 3개 이상으로 합격.
- [ ] 스트럼과 아르페지오 모두 false advance 0회.
- [ ] 반 박 이상 vocal stall 0회.
- [ ] pause/stop 후 곡이 혼자 계속 가지 않는다.
- [ ] 코드 누락·추가·틀림 후 1~2박 안에 안전하게 복구한다.
- [ ] 64/128/256/512/1024 offline block-size 회귀를 통과한다.
- [ ] 두 실제 곡과 synthetic archetype이 같은 package schema를 사용한다.
- [ ] audio thread에 파일 I/O, 동적 할당, mutex wait, UI 호출이 없다.
- [ ] ASIO 128 live take에서 xrun 0회.
- [ ] 정상 `LOCKED` 구간 모음 오차 median ≤ 30 ms, P95 ≤ 60 ms를 만족했다.
- [ ] 100 ms 이상 동기 오차가 전체 anchor의 1% 미만이고 반 박 이상은 0회다.
- [ ] 기존 방식과 predictive 방식을 즉시 전환할 수 있다.
- [ ] 블라인드 A/B에서 정한 체감 합격 기준을 통과한다.
- [ ] 최종 결과 리포트와 재현 명령을 남긴다.
- [ ] 사용자가 실제 기타 시연 후 기본 엔진 전환을 승인한다.

---

## 20. 예상 작업 산출물

계획 승인 후 다음 파일군을 만든다.

```text
docs/
  MODE1_PREDICTIVE_BASELINE_REPORT.md
  MODE1_LATENCY_BREAKDOWN_REPORT.md
  MODE1_LIVE_AB_REPORT.md

src/mode1_vocal_follower/
  BeatClock.*
  ScorePositionEstimator.*
  PredictivePhraseScheduler.*
  RealtimeTraceBuffer.*

tools/mode1_song_package/
  package_schema_v2.json
  validate_song_package.py
  annotate_vocal_anchors.py
  run_predictive_regression.py
  measure_loopback_latency.py

tests/fixtures/mode1/
  manifests and hashes

output/mode1_predictive_runs/
  reproducible run artifacts
```

정확한 파일명은 기존 빌드 구조를 다시 확인한 뒤 조정할 수 있지만, 역할과 결과물은 유지한다.

---

## 21. 연구 근거와 적용 범위

- [Music Plus One 논문](https://f.aaai.org/Papers/AAAI/2006/AAAI06-353.pdf): 현재 연주를 듣는 모델, 미래 timing을 예측하는 모델, 준비된 오디오의 시간 변형을 분리하는 선례다. 본 프로젝트는 더 제한된 곡·악보를 사용하므로 초기에는 더 단순하고 검증 가능한 estimator로 시작한다.
- [Music Plus One 개요](https://music.informatics.indiana.edu/~craphael/music_plus_one/what.html): live performer를 듣고 동기화된 반주를 만드는 목표와 시스템 구성의 참고다.
- [Antescofo synchronization strategies](https://antescofo-doc.ircam.fr/Reference/time_synchro/): 관측을 기다리는 보수적 동작, 예측하며 진행하는 동작, 미래 horizon에 부드럽게 맞추는 동작을 구분하는 데 참고한다. 본 계획은 progressive와 target 방식의 제한된 혼합에 가깝다.
- [Antescofo tempo inference](https://antescofo-doc.ircam.fr/Reference/tempo_inference/): score position과 tempo 추정을 분리해 다루는 근거다.
- [SongDriver](https://arxiv.org/abs/2209.06054): 미래 음악 정보를 준비해 논리적 지연을 줄이는 발상의 참고이며, 본 오디오 스코어 팔로워의 직접적인 구현 증거로 과장하지 않는다.
- [Rubber Band integration notes](https://www.breakfastquay.com/rubberband/integration.html): time-stretch의 실시간 통합 제약을 검토할 때 사용한다.
- [Rubber Band LiveShifter API](https://breakfastquay.com/rubberband/code-doc/classRubberBand_1_1RubberBandLiveShifter.html): 긴 sustain의 제한적 실시간 보정 후보이며, 예측 scheduler 이후에만 평가한다.

---

## 22. 구현 착수 전 최종 체크포인트

이 문서를 검토한 뒤 다음 순서로 진행한다.

1. 제품 선택 표의 기본값 확정
2. Phase 0 기준선 동결
3. Phase 1 계측만 구현
4. 측정 결과를 사용자와 함께 확인
5. Phase 2 shadow mode 결과를 시각화
6. predictive가 실제 오디오를 제어하도록 허용
7. 만찬가와 Oasis 실연 A/B

첫 번째 시연은 “완성품 시연”이 아니라 Test A, Test B, shadow-mode alignment를 보여 주는 기술 검증이다. 이 세 가지가 통과된 뒤 실제 보컬을 predictive scheduler에 연결한다.

이 순서를 지키면 버퍼, onset, scheduler, vocal clip 내부 자음, score recovery 중 무엇이 개선에 기여했는지 분명히 알 수 있다. 또한 오프라인에서 좋은 숫자만 나온 상태를 실제 기타 지연 개선으로 오인하지 않고, 마지막에는 반드시 물리 loopback과 실제 연주자의 체감으로 결론을 내릴 수 있다.
