# Mode 1 예측형 스코어 팔로워 구현 현황

- 기준일: 2026-07-29
- 적용 곡: 만찬가, Don't Look Back in Anger
- 기본 앱 모드: `Predictive Shadow`
- 사용자 선택 모드: `Baseline`, `Predictive Shadow`, `Predictive Active`

## 결론

공통 실시간 엔진, 곡 패키지 검증기, 인트로 보존, 보컬 발성 앵커,
오프라인 추적 로그와 블록 크기 회귀 시험까지 구현했다. 새 곡도 같은
패키지 생성·검증·앵커 분석 절차를 통과시키는 구조이며, 두 곡 이름을
조건문으로 넣어 동작시키는 방식이 아니다.

다만 실제 오디오 인터페이스의 물리 입출력 지연과 연주자가 느끼는
개선 폭은 오프라인 렌더러로 증명할 수 없다. 따라서 `Active`를 기본값으로
승격하는 마지막 조건은 ASIO loopback과 실제 기타 블라인드 A/B이다.

## 구현 완료

### 실시간 관측

- 고정 용량 SPSC trace buffer
- onset 검출 시각, 악보 위치 변경, phrase 요청, 첫 유효 보컬 sample 기록
- BeatClock 관측·상태 변경 기록
- 입력/출력 장치 지연의 분리 전달
- CSV 분석 도구와 dropped-event 계수
- 오디오 callback 내부 파일 읽기·문자열 생성·동적 할당 없이 기록

### BeatClock과 예측

- `DISARMED → ARMING → LOCKED → COASTING → HOLDING → RECOVERING` 상태
- onset은 박자 phase/tempo 관측, chord/chroma는 로컬 위치 확인·복구에 사용
- 세 번의 안정된 timing anchor 전에는 미래 boundary를 재생하지 않음
- 최대 두 박자 이내의 바로 다음 악보 boundary만 예측
- 오차가 커지거나 일시정지 후에는 예측 신뢰도를 초기화
- 현재 안전 기본값은 Shadow이며 Active는 앱에서 명시적으로 선택

독립 실험용 미래 phrase scheduler는 과도한 선행 재생으로 phrase 누락을
만들었기 때문에 제품 출력 경로로 채택하지 않았다. 현재 Active 경로는
기존 순차 scheduler를 유지하면서 다음 boundary 하나만 제한적으로 예측한다.

### 인트로와 곡 패키지 일반화

- 패키지 생성 시 첫 가사 전 source chord event를 삭제하지 않음
- `instrumental_intro`, `vocal_allowed: false` section 생성
- intro prepend 작업은 재실행해도 중복되지 않음
- validator가 다음을 검사:
  - chord timeline 정렬
  - 무인트로 곡의 명시적 `no_intro` 정책
  - source의 첫 가사 전 chord 보존
  - intro 구간의 vocal gate
  - vocal anchor 범위

Oasis의 기존 오류 원인은 phrase 기반 chord timeline 생성 단계가 전체 chord
timeline을 교체하여 첫 가사 전 9개 event를 버린 것이었다. 최종 패키지는
9개 intro event를 복원했고, 첫 chord는 0.000초, 첫 vocal 목표는
11.184초다. 만찬가는 첫 vocal 목표 9.952초 전 9개 intro event를 가진다.

### 보컬 발성 앵커

각 phrase의 `vocal.sync`에 재생 구간 기준 `audible_onset_sec`, 신뢰도,
detector version을 기록한다. 자동 에너지 검출값을 모음 onset이라고
간주하지 않는다. 검수된 `vowel_onset_sec`가 나중에 들어오면 그 값이
우선한다.

- 만찬가: 149 phrase 전부 메타데이터 보유, 18개 non-zero,
  P95 72.5ms, 최대 140ms
- Oasis: 210 phrase 전부 메타데이터 보유, 28개 non-zero,
  P95 32.5ms, 최대 230ms
- 250ms를 넘는 자동 검출은 침묵/잘린 클립으로 판단하여 보정하지 않음
- 신뢰도 0.35 미만 자동 앵커는 실시간 scheduler가 사용하지 않음

## 검증 결과

### 자동 테스트

- `Mode1CoreTests`: 통과
- `VoiceCaptureTests`: 통과

### 블록 크기 행렬

Active 모드, 48kHz, 64/128/256/512/1024 sample에서 전부 통과했다.
장치 callback이 커져도 내부 분석은 최대 128 sample sub-block으로 처리한다.

| 곡 | 각 블록에서 phrase | 누락/만료 | trace drop |
|---|---:|---:|---:|
| 만찬가 | 149/149 | 0 | 0 |
| Oasis | 210/210 | 0 | 0 |

재현 결과는 `output/mode1_predictive_runs/final_block_matrix.csv`에 저장했다.

### 현재 수치의 의미

오프라인 합성 입력에서 onset 분석 완료 P95는 약 2.5ms이며, 이는 알고리즘
callback 안의 계산 지연이다. 실제 기타의 ADC, ASIO input latency,
DAC/output latency는 포함하지 않으므로 “실제 20ms 달성” 증거로 사용하지
않는다.

보컬 phrase 수, intro gate, 큰 callback에서의 누락 방지는 검증됐다.
반면 다음 목표는 실제 장치에서만 합격 판정할 수 있다.

- onset → 즉각 반응음: 목표 15~20ms, 허용 30ms
- LOCKED 구간 모음/발성 anchor ↔ 목표 박자: median ±30ms, P95 ±60ms
- scheduling jitter: P95 3ms 이하
- ASIO 128에서 xrun 0
- 100ms 이상 동기 오차: 전체 anchor의 1% 미만
- 반박자 이상 오차: 0

## 실제 시연 순서

1. 앱에서 곡과 오디오 인터페이스 ASIO를 선택한다.
2. 버퍼 128로 시작하고 불안정하면 256으로 올린다.
3. `Baseline`으로 같은 기타 구간을 녹음한다.
4. `Predictive Shadow`에서 BeatClock이 intro 동안 `LOCKED` 되는지 확인한다.
5. `Predictive Active`로 같은 구간을 연주한다.
6. Oasis 첫 가사가 intro 직후에만 나오는지 확인한다.
7. 만찬가와 Oasis 각각 스트럼 3회, 아르페지오 3회를 수집한다.
8. loopback 녹음의 기타 onset과 보컬 audible/vowel anchor 오차를 계산한다.
9. 순서를 가린 A/B에서 연주자가 선호도를 판정한다.

Active가 수치 목표와 블라인드 선호를 함께 통과하기 전까지 Shadow를
기본값으로 유지한다. 이것이 오프라인 숫자는 좋아졌지만 실제 기타에서
더 나빠지는 회귀를 막는 최종 안전장치다.

## 새 곡 추가 계약

새 곡은 다음 데이터만 곡별로 바뀌고 엔진은 바뀌지 않는다.

1. 전체 악보/MusicXML 또는 신뢰 가능한 chord timeline
2. 원곡 전체 WAV와 분리 보컬 WAV
3. 가사 및 보컬 melody timing
4. 필요하면 chord 참고 PDF
5. 첫 가사 전 intro event와 section 정책
6. phrase vocal 파일과 자동/검수 vocal anchor

패키지를 생성한 뒤 validator, synthetic strum, arpeggio, block-size 행렬을
통과해야 앱의 곡 목록에 승격한다. 무인트로 곡도 코드 변경 없이
`entry_policy: no_intro`로 표현한다.

