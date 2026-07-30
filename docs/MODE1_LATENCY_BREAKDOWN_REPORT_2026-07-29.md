# Mode 1 Phase 1 지연 분해 보고서

- 상태: 오프라인 계측 구현 완료, 물리 loopback 대기
- sample rate: 48 kHz
- block size: 128 samples
- block duration: 2.666667 ms

## 1. 구현된 계측

audio thread에는 고정 크기 SPSC trace buffer만 추가했다. 문자열, 파일 I/O, 동적 할당과 mutex를 사용하지 않는다.

기록 event:

- latency-compensated onset 추정 sample
- detector 결과가 사용 가능한 block-end sample
- score event 변경
- phrase 요청
- 해당 phrase의 첫 non-zero vocal output sample

앱은 장치의 input latency와 output latency를 각각 읽는다. Offline renderer는 `realtime_trace.csv`와 `metrics.json`을 생성한다.

## 2. 128-sample synthetic 결과

| 지표 | 만찬가 | Oasis |
|---|---:|---:|
| onset 수 | 689 | 765 |
| phrase 요청/첫 출력 | 149/149 | 210/210 |
| trace drop | 0 | 0 |
| onset completion median | 1.167 ms | 1.250 ms |
| onset completion P95 | 2.542 ms | 2.396 ms |
| onset completion max | 2.667 ms | 2.667 ms |
| phrase request→first output median | 0.021 ms | 0.021 ms |
| phrase request→first output max | 0.042 ms | 104.292 ms |
| 첫 onset→첫 phrase request | 9472.396 ms | -1.396 ms |

`-1.396 ms`는 오프라인 callback 안에서 onset peak가 나타난 sample보다 현재 output block 시작 sample에 phrase를 요청했다는 뜻이다. 물리적으로 음수가 되는 지연이라는 뜻이 아니다.

## 3. 해석

### 만찬가

첫 onset과 첫 phrase 사이에 약 9.47초의 intro가 보존된다. 현재 package의 9개 intro chord event가 실제로 vocal gate 역할을 한다.

### Don't Look Back in Anger

첫 onset과 첫 phrase 요청이 같은 callback이다. 원천 chord 자료에는 intro event가 있지만 최종 package에는 없다는 Phase 0 진단이 런타임 trace에서도 재현됐다.

첫 phrase의 첫 non-zero sample은 요청보다 104.292 ms 늦다. 이후 phrase는 대부분 같은 callback에서 non-zero가 발생한다. 따라서 P95만 보면 첫-entry 문제를 놓치므로 모든 결과에 `first`와 `max`를 함께 기록한다.

이 104.292 ms에는 첫 clip 내부의 선행 무음/attack과 renderer 동작이 포함될 수 있다. Phase 4에서 vowel/consonant anchor와 실제 waveform onset을 분리한다.

## 4. 이 수치가 증명하지 않는 것

오프라인 결과에는 다음이 없다.

- 기타 pickup과 ADC
- ASIO input latency
- 실제 callback scheduling
- DAC/output latency
- 헤드폰/스피커
- 화면 refresh

따라서 onset P95 2.5 ms를 “실제 기타 반응 지연”으로 표현하면 안 된다. Test A0/A의 물리 loopback을 통과하기 전에는 live latency를 확정하지 않는다.

## 5. 현재 합격 상태

- [x] lock-free trace event 손실 0
- [x] phrase 요청과 첫 output 1:1 대응
- [x] input latency 전달 경로 추가
- [x] 128-sample offline trace 생성
- [x] 첫/max/P95를 포함한 분석기 추가
- [x] 기존 C++ 테스트 2/2 통과
- [ ] hardware/software direct monitoring loopback
- [ ] 실제 onset→click 물리 지연
- [ ] 실제 화면 pixel 반응

물리 측정 항목은 오디오 인터페이스와 케이블을 연결한 실연 단계에서 완료한다. 다음 소프트웨어 단계에서는 BeatClock을 shadow mode로 실행하되 기존 음향 결과는 변경하지 않는다.
