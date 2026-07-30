# 만찬가 원곡 대조 진단과 Mode 1 재구축 계획

- 작성일: 2026-07-29
- 기준 원곡: `tuki. - 만찬가(晩餐歌) ... (128k).mp3`
- 실제 기타: `bansanka_last_guitar_take.wav`
- 현재 비교 출력: Predictive Active v8
- 목적: “한 박 늦게 따라오는 느낌”을 원곡·기타·보컬 출력의 동일 시간축
  비교로 분해하고, 조건 미세조정이 아닌 근본 수정 계획을 확정한다.

## 1. 이번 비교에서 확인한 사실

### 1.1 보컬 WAV 시작 자체가 주된 병목은 아니다

실시간 trace에서 phrase 요청부터 첫 non-zero 출력까지는 대부분 같은 audio
callback 안이다. 렌더된 보컬의 유효 RMS도 대부분 요청 직후 나타난다.
일부 클립에는 35~95ms의 내부 attack이 있지만, 반복적으로 느껴지는
약 200~600ms 지연 전체를 설명하지 못한다.

따라서 다음은 주원인이 아니다.

- 파일을 늦게 읽는 문제
- message/UI thread에서 재생을 시작하는 문제
- 128 sample 장치 버퍼 하나의 지연
- 모든 보컬 클립에 동일하게 들어 있는 긴 무음

### 1.2 첫 가사는 실제 첫 스트럼이 아니라 후속 onset에 붙는다

패키지에서 첫 가사와 첫 vocal chord event는 모두 score 9.952초다.
최근 실제 녹음에서 해당 경계 주변에는 약 226~230ms 간격의 onset이 있다.

현재 출력은 첫 스트럼이 아니라 그 다음 onset에 거의 즉시 붙는다. 따라서
앱 내부에서는 “검출 후 즉시 출력”이지만, 연주자가 들을 때는 이미
한 subdivision 늦다.

이것이 다음 현상을 동시에 설명한다.

- onset-to-output 계산은 수 ms인데 체감은 반 박자~한 박자 늦음
- 버퍼를 줄여도 거의 개선되지 않음
- 일부 phrase만 phase lead로 당기면 좋아지지만 전체 중앙값은 0ms

### 1.3 지연은 상수가 아니라 진행 중 누적·해제된다

현재 실제 take의 35개 phrase를 원곡의 immutable source start와 affine
비교한 결과:

- fitted performance seconds / score second: 약 1.158
- 내부 phrase timing residual 중앙 절대값: 약 1.81초
- P95 절대 residual: 약 5.75초
- 최대 절대 residual: 약 7.55초

이 값에는 사용자의 실제 pause도 포함되므로 물리 latency 수치로 사용하면
안 된다. 그러나 한 개의 고정 offset으로 설명되지 않고, 시스템이 중간중간
기타 이벤트를 기다리며 불연속적으로 진행한다는 증거다.

### 1.4 기존 phase-lead 실험의 판정

첫 스트럼/아르페지오 구분은 필요하다. 그러나 이것만으로는 부족하다.

- 안전 버전: 최근 35개 중 6~7개만 개선, 중앙값 개선 0ms
- 공격적으로 chord cursor를 조기 전진한 버전: 대부분 빨라졌지만
  pause에서 2~13초 누적 선행
- phase lead를 vocal target에만 적용한 버전: 누적 폭주는 막았지만
  대부분 phrase가 여전히 다음 chord trigger를 기다림

따라서 “early tolerance를 계속 넓히기”는 최종 방향이 아니다.

## 2. 근본 원인

현재 Active도 핵심적으로 다음 구조를 유지한다.

```text
onset을 다음 chord event로 채택
→ chord cursor 전진
→ songTime을 해당 event로 이동
→ 해당 phrase를 즉시 재생
```

그리고 다음 chord boundary 앞에서 `songTimeSeconds`를 clamp한다. 이 때문에
BeatClock이 미래 시각을 알고 있어도 chord event가 확정되기 전에는 대부분의
phrase가 경계를 넘지 못한다.

즉, 현재 BeatClock은 관측·신뢰도 표시에는 쓰이지만 제품의 vocal transport를
실제로 운전하지 않는다. 이것이 최초 계획과 현재 구현의 가장 큰 차이다.

## 3. 확정할 방향

다음 판단을 최종 설계 원칙으로 채택한다.

1. 첫 스트럼/아르페지오 구분은 필요하다.
2. 현재 phase lead 학습은 보조 관측값으로 유지할 수 있다.
3. onset gate 조건만 계속 조정해서는 한 박 지연을 해결할 수 없다.
4. Active 전용 free-running transport와 sample 예약이 다음 핵심 작업이다.
5. 기타 onset은 vocal 발사 버튼이 아니라 transport phase/tempo 보정 센서다.
6. chord/chroma는 매 event 진행 조건이 아니라 로컬 위치 확인·복구에 쓴다.

목표 구조:

```text
인트로 onset/chroma
→ 여러 beat-phase 가설 추적
→ tempo·phase·score position LOCK
→ 독립적인 continuous score transport
→ 100~250ms 미래 vocal event를 sample timeline에 예약
→ 기타 onset으로 phase/tempo를 완만하게 보정
→ chord 불일치 시에만 제한적 score-position 복구
```

## 4. 구현 전에 반드시 할 Oracle 테스트

엔진을 다시 바꾸기 전에 “정답 timing으로 재생하면 현재 vocal asset이
정상적으로 들리는가”를 먼저 증명한다.

### Oracle A: 원곡 clock으로 앱 vocal 재생

- 기타 추종을 완전히 제거한다.
- 패키지의 immutable source/score time만 사용한다.
- 원곡 MP3와 앱 vocal을 동일한 9.952초 기준으로 시작한다.
- micro phrase를 sample 단위로 예약한다.

판정:

- 원곡과 리듬이 맞으면 asset은 합격, follower가 원인이다.
- 여기서도 밀리면 phrase slicing/content offset/score mapping이 원인이다.

### Oracle B: 실제 기타에 수동 anchor 두 개로 time warp

- 실제 기타 녹음에서 시작 anchor와 한 소절 뒤 anchor를 수동/파형으로 지정
- 두 anchor 사이를 선형 tempo map으로 재생
- onset/chord 인식 없이 vocal을 재생

판정:

- 이 출력이 자연스러우면 free-running transport가 해결 방향이다.
- 이것도 이상하면 기타가 기대한 악보 진행과 다르거나 package timing이 틀렸다.

### Oracle C: 원본 분리 보컬과 micro 재조립 비교

- 분리 보컬 원본
- 변환된 full vocal
- micro clip을 source time에 재조립한 vocal

세 가지의 phrase attack과 빈 구간을 비교한다. micro 재조립에서만 리듬이
깨지면 scheduler 전에 slicing/crossfade를 수정한다.

이 세 Oracle을 통과하기 전에는 Active 알고리즘을 추가 조정하지 않는다.

## 5. Active v2 transport 설계

### 5.1 두 개의 위치를 분리한다

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

- `continuousScoreSeconds`: 보컬 예약을 운전하며 LOCKED 동안 멈추지 않는다.
- `confirmedScoreEventIndex`: chord/chroma로 확인한 최근 악보 위치다.

두 값을 하나의 cursor로 사용하지 않는다.

### 5.2 인트로를 calibration 구간으로 사용한다

인트로의 모든 onset을 관측한다. vocal boundary가 있는 onset만 보는 현재
방식은 폐기한다.

- onset interval histogram으로 beat/subdivision 후보 생성
- score의 intro chord-event 간격과 비교
- 첫 스트럼 phase와 후속 아르페지오 phase를 동시에 가설로 유지
- 3개 이상의 chord boundary 또는 2마디 관측 후 LOCK
- 첫 가사 250ms 전까지 LOCK 실패 시 안전하게 Baseline 또는 count-in 안내

첫 가사를 trigger로 학습을 시작해서는 안 된다.

### 5.3 vocal transport에서는 chord-boundary clamp를 제거한다

Active의 LOCKED/COASTING 구간에서는 `songTimeSeconds`를 다음 chord 직전에
고정하지 않는다.

- transport는 추정 tempo로 계속 전진
- scheduler는 look-ahead horizon 안의 모든 phrase를 예약
- 실제 기타가 예상보다 빠르면 아직 commit되지 않은 이벤트를 당김
- 예상보다 늦으면 이미 시작한 audio를 취소하지 않고 다음 1~2박에 걸쳐 수렴

HOLDING에서는 새 phrase commit을 중단하고, 이미 commit된 80ms 이내 이벤트만
재생할지 정책으로 결정한다.

### 5.4 sample 예약 큐

phrase index 하나를 즉시 재생하는 API 대신 다음 event queue를 사용한다.

```cpp
struct ScheduledVocalEvent
{
    int phraseIndex;
    int64_t targetOutputSample;
    int64_t commitDeadlineSample;
    double confidence;
};
```

- audio callback에서 target sample부터 직접 mix
- 100~250ms look-ahead
- commit horizon은 초기 80ms
- queue는 고정 용량 SPSC 또는 audio-thread-owned ring
- 파일 I/O, allocation, mutex, UI 호출 금지

### 5.5 onset과 chord의 역할

- 첫 스트럼 onset: phase correction의 강한 관측
- 아르페지오 후속 onset: subdivision 후보, phase correction 가중치 낮음
- chord/chroma: 현재 local score window와 일치하는지 확인
- 곡에 없는 chord label은 생성하지 않음
- 위치 복구는 현재 주변 ±1~2 chord event에서만 시작
- 두 번 이상 일치하고 phase cost가 개선될 때만 commit

## 6. 검증 순서

### Stage 1: asset/oracle

- 원곡 clock Oracle A
- 실제 기타 2-anchor Oracle B
- 분리 보컬/micro 재조립 Oracle C
- 첫 소절을 사람이 직접 듣고 pass/fail

### Stage 2: shadow transport

- Active v2는 출력하지 않고 예상 phrase sample만 로그
- 실제 기타 두 take와 synthetic/arpeggio 입력
- oracle target과 shadow target 오차 계산

합격:

- 첫 가사 오차 ±60ms
- LOCKED 구간 median ±30ms
- P95 ±60ms
- 100ms 이상 1% 미만
- 반박자 이상 0

### Stage 3: offline active

- 기존 Baseline과 동일 입력 A/B
- phrase 누락/중복/역순 0
- pause에서 1박 이상 runaway 0
- block 64/128/256/512/1024 모두 동일 sequence

### Stage 4: 실제 ASIO

- onset→click 물리 loopback
- 기타와 vocal output 동시 녹음
- xrun 0
- 스트럼/아르페지오 각각 3 take
- 블라인드 A/B에서 Active 선호

## 7. 중단·롤백 기준

다음 중 하나라도 발생하면 Active v2를 기본값으로 승격하지 않는다.

- 첫 가사 조기 진입
- 잘못된 lyric index
- pause에서 한 박 이상 계속 진행
- 같은 phrase 중복
- 100ms 이상 늦어진 event 비율이 Baseline보다 증가
- 실제 A/B에서 체감 개선 없음

기본 모드는 검증 완료 전까지 Predictive Shadow다.

## 8. 현재 실험 코드의 처리

- bounded 120ms sample reservation: 유지
- early-arrival override: 유지
- vocal audible/vowel anchor: 유지
- 아르페지오 phase lead: Shadow feature로 유지
- chord cursor early-tolerance 확대: 폐기
- chord cursor 전역 phase rebase: 폐기
- Active에서 phase lead를 제품 출력에 직접 쓰는 것은 Oracle/Shadow 검증 후 결정

## 9. 산출물

이번 비교용 청취 파일:

- `01_original_first_verse.wav`
- `02_separated_original_vocal.wav`
- `03_current_app_vocal.wav`
- `04_current_guitar_plus_vocal.wav`
- `05_AB_original_then_app_vocal.wav`
- `analysis.json`

위 파일은 `output/mode1_predictive_runs/original_vs_current_listening/`에 있다.

다음 구현 착수점은 Active v2 코드가 아니라 Oracle A/B/C다. Oracle이
“정답 clock에서는 자연스럽다”를 증명한 뒤 free-running transport를
구현한다.

